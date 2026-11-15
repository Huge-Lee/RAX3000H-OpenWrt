#!/bin/sh
# Runtime hardware initialization using this device's drivers and calibration.
set -e
load() {
    name="${1##*/}"
    name="${name%.ko}"
    name="$(echo "$name" | tr '-' '_')"
    grep -q "^$name " /proc/modules || insmod "$1"
}
load_group() {
    while read -r group path; do
        [ "$group" = "$1" ] || continue
        load "/$path"
    done < /usr/share/rax3000h/vendor-modules.list
}
wired_modules() {
    local channel irq affinity
    load_group wired
    # Match the vendor PIE channel distribution across the two CPU cores.
    for channel in 0 1 2 3; do
        irq="$(awk -v name="pie-ch$channel," 'index($0, name) {sub(/:$/, "", $1); print $1}' /proc/interrupts)"
        case "$channel" in
            0|2) affinity=1 ;;
            1|3) affinity=2 ;;
        esac
        echo "$affinity" >"/proc/irq/$irq/smp_affinity"
    done
    # Flash installations use this device's factory base address.
    for port in 0 1 2 3; do
        if [ -x /usr/sbin/rax-mac ]; then
            expected=$(/usr/sbin/rax-mac "$port")
        else
            expected="02:e0:fc:66:66:6$port"
        fi
        [ "$(cat "/sys/class/net/eth$port/address")" = "$expected" ] ||
            ip link set "eth$port" address "$expected"
    done
    # Keep WAN IPv6 closed until the dual-stack firewall has committed.
    [ -e /var/run/rax-firewall-ready ] || echo 1 >/proc/sys/net/ipv6/conf/eth0/disable_ipv6
}
wired() {
    wired_modules
    # Vendor bridge creation rejects netifd's extra netlink attributes.
    # Create it after netifd clears old bridge state; netifd supplies the IP.
    [ -d /sys/class/net/br-lan ] || brctl addbr br-lan
    brctl setfd br-lan 0
    for port in eth1 eth2 eth3; do
        [ -d "/sys/class/net/br-lan/brif/$port" ] || brctl addif br-lan "$port"
        ip link set "$port" up
    done
    # network reload can recreate br-lan while the vendor hostapd stays alive.
    for conf in /var/run/rax-wifi/vap*.conf; do
        [ -f "$conf" ] || continue
        vap="${conf##*/}"; vap="${vap%.conf}"
        [ -s "/var/run/rax-wifi/$vap.pid" ] || continue
        kill -0 "$(cat "/var/run/rax-wifi/$vap.pid")" 2>/dev/null || continue
        [ -d "/sys/class/net/$vap" ] || continue
        [ -d "/sys/class/net/br-lan/brif/$vap" ] || brctl addif br-lan "$vap"
        ip link set "$vap" up
    done
}
wifi() {
    # Wi-Fi dynamically requests CFE symbols, beyond its ELF depends field.
    load_group wifi
    /bin/sh /usr/bin/hi5622v100_cal_init.sh >/tmp/wifi-calibration.log 2>&1
}
case "${1:-}" in
    wired-modules) wired_modules ;;
    wired) wired ;;
    wifi) wifi ;;
    *) echo "Usage: $0 wired|wired-modules|wifi" >&2; exit 2 ;;
esac
