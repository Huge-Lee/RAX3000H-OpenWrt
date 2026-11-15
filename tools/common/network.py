"""Read Windows/Linux adapter addresses without changing host networking."""
import ipaddress
import json
import socket
import subprocess
import sys
import time


def _windows_adapters():
    script = r"""
    [Console]::OutputEncoding=[System.Text.UTF8Encoding]::new()
    $ErrorActionPreference='Stop'
    @(Get-NetAdapter | ForEach-Object {
      $a=$_
      $addresses=@(Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object AddressState -eq Preferred | ForEach-Object {
          @{address=$_.IPAddress; prefix=$_.PrefixLength}
        })
      @{name=$a.Name; index=$a.ifIndex; addresses=$addresses;
        wired=($a.HardwareInterface -and $a.MediaType -eq '802.3'); up=($a.Status -eq 'Up')}
    }) | ConvertTo-Json -Depth 4 -Compress
    """
    result = subprocess.check_output(['powershell.exe', '-NoProfile', '-Command', script],
                                     encoding='utf-8', timeout=15).strip()
    data = json.loads(result) if result else []
    return [data] if isinstance(data, dict) else data


def _linux_adapters():
    from pathlib import Path
    result = subprocess.check_output(['ip', '-j', '-4', 'address', 'show'], text=True, timeout=10)
    adapters = []
    for item in json.loads(result):
        device = Path('/sys/class/net') / item['ifname']
        wired = (device / 'device').exists() and not (device / 'wireless').exists()
        adapters.append(dict(name=item['ifname'], index=item['ifindex'], wired=wired,
                             up=item.get('operstate') == 'UP', addresses=[
                                 dict(address=a['local'], prefix=a['prefixlen'])
                                 for a in item.get('addr_info', [])
                                 if a.get('family') == 'inet' and not a.get('tentative', False)]))
    return adapters


def adapters():
    if sys.platform == 'win32':
        return _windows_adapters()
    if sys.platform.startswith('linux'):
        return _linux_adapters()
    raise RuntimeError('Flashing supports Windows and Linux')


def choose(items, label):
    if not items:
        raise RuntimeError(f'No {label} found; connect the device or specify it explicitly')
    if len(items) == 1:
        return items[0]
    for index, item in enumerate(items, 1):
        print(f'  {index}: {item}')
    if not sys.stdin.isatty():
        raise RuntimeError(f'Multiple {label}; use --port / --interface to select explicitly')
    answer = input(f'Select {label} [1-{len(items)}]: ').strip()
    if not answer.isdigit() or not 1 <= int(answer) <= len(items):
        raise RuntimeError('Invalid selection')
    return items[int(answer) - 1]


def select_interface(name=None):
    if name:
        adapter_info(name)
        return name
    wired = [a for a in adapters() if a['wired']]
    candidates = [a for a in wired if a['up']] or wired
    return choose([a['name'] for a in candidates], 'connected Ethernet adapter')


def adapter_info(interface):
    for item in adapters():
        if item['name'] == interface:
            return dict(item, ips=[a['address'] for a in item['addresses']])
    raise RuntimeError(f'Network adapter not found: {interface}')


def interface_address(interface):
    entries = adapter_info(interface)['addresses']
    if not entries:
        return None
    if len(entries) > 1:
        raise RuntimeError(f'Multiple IPv4 addresses on {interface}; select a single-address LAN adapter')
    entry = entries[0]
    address = ipaddress.IPv4Interface(f"{entry['address']}/{entry['prefix']}")
    if address.ip.is_loopback or address.ip.is_unspecified or address.network.prefixlen > 30:
        raise RuntimeError(f'Unsupported LAN address: {address}')
    router = address.network.network_address + 1
    if router == address.ip:
        router += 1
    return str(address.ip), str(router), str(address.netmask)


def wait_address(interface, previous=None, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        address = interface_address(interface)
        if address and address != previous:
            return address
        time.sleep(1)
    raise TimeoutError(f'LAN {interface} has no usable IPv4 address; configure an isolated wired subnet')


def check_tftp_bind(host):
    # Report permissions/firewall setup before starting the router installation.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        try:
            listener.bind((host, 69))
        except OSError as error:
            raise RuntimeError(f'Cannot bind TFTP UDP69 on {host}: {error}. '
                               'On Linux, use the documented privileged invocation.') from error
