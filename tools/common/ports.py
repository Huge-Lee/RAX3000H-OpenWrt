"""Discover serial adapters; prefer CH340/CH341 without requiring a chip type."""
from serial.tools import list_ports

from .network import choose


def serial_ports():
    return sorted(list_ports.comports(), key=lambda port: port.device)


def select_port(name=None):
    if name:
        return name
    ports = serial_ports()
    preferred = [p for p in ports if p.vid == 0x1a86 or any(
        term in f'{p.description} {p.manufacturer}'.lower() for term in ('ch340', 'ch341'))]
    candidates = preferred or ports
    labels = {f'{p.device} — {p.description}': p.device for p in candidates}
    return labels[choose(list(labels), 'serial adapter')]


def print_devices():
    from .network import adapters
    print('Serial adapters:')
    for port in serial_ports():
        print(f'  {port.device}: {port.description} [{port.hwid}]')
    print('Network adapters:')
    for item in adapters():
        addresses = ', '.join(f"{a['address']}/{a['prefix']}" for a in item['addresses'])
        print(f"  {item['name']}: {addresses or 'no IPv4'}; wired={item['wired']}; up={item['up']}")
