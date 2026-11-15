"""Serial console and temporary U-Boot/TFTP loading."""
import re
import time

import serial

from serve_tftp import file_server
from .network import interface_address, wait_address

UBOOT = re.compile(rb"(?:^|[\r\n])luofu # $")
ROOT_PROMPT = re.compile(rb"(?:^|[\r\n])root@[^:\r\n]+:[^\r\n]*# ")
LINUX = re.compile(rb"(?:^|[\r\n])(?:root@[^:\r\n]+:[^\r\n]*# |[^\r\n]* # )")


class Console:
    def __init__(self, port, log):
        self.link = serial.Serial(port=None, baudrate=115200, timeout=0.05)
        self.link.dtr = self.link.rts = False
        self.link.port = port
        self.log = log
        self.buffer = b""

    def __enter__(self):
        self.link.open()
        return self

    def __exit__(self, *exc):
        self.link.close()

    def send(self, line, byte_delay=None):
        self.buffer = b""
        encoded = line.encode() + b"\r"
        if byte_delay is not None:
            for value in encoded:
                self.link.write(bytes([value]))
                time.sleep(byte_delay)
            self.link.flush()
            return
        for offset in range(0, len(encoded), 96):
            self.link.write(encoded[offset:offset + 96])
            self.link.flush()
            time.sleep(0.02)

    def wait(self, predicate, timeout, interrupt=False):
        deadline = time.monotonic() + timeout
        interrupted = False
        while time.monotonic() < deadline:
            data = self.link.read(65536)
            if not data:
                continue
            self.log.write(data)
            self.buffer = (self.buffer + data)[-65536:]
            if interrupt and not interrupted and b"Autoboot in " in self.buffer:
                self.link.write(b"gw92")
                self.link.flush()
                interrupted = True
                print("AUTOBOOT_INTERRUPTED", flush=True)
            if predicate(self.buffer):
                return self.buffer
        raise TimeoutError("Serial stage timed out; see SERIAL_LOG")

    def command(self, line, timeout=10):
        print(f"UBOOT: {line}", flush=True)
        self.send(line)
        return self.wait(UBOOT.search, timeout)

    def detect(self):
        # A bare Enter can repeat an old U-Boot command, including bootfip.
        recognized = lambda b: UBOOT.search(b) or LINUX.search(b) or re.search(rb"login:\s*$", b, re.I)
        self.buffer = b""
        self.link.write(b"\x15\x03")
        try:
            output = self.wait(recognized, 1)
        except TimeoutError:
            self.send("\x15version")
            output = self.wait(lambda b: recognized(b) or re.search(rb"password:\s*$", b, re.I), 8)
            if re.search(rb"password:\s*$", output, re.I):
                # At a login prompt, the probe may have been read as a user
                # name. Cancel it before the normal credential-aware login.
                self.link.write(b"\x03")
                self.buffer = b""
                output = self.wait(recognized, 8)
        return "uboot" if UBOOT.search(output) else "linux"


def load_image(console, file, address, interface):
    for attempt in range(2):
        host, router, mask = address
        print(f"LAN: {interface} PC={host} ROUTER={router} MASK={mask}", flush=True)
        for line in [f"setenv ipaddr {router}", f"setenv serverip {host}", f"setenv netmask {mask}"]:
            console.command(line)
        ram_address = "83200000" if file.name == "kernel-test.bin" else "85000000"
        with file_server(file, router, bind=host) as transfer:
            output = console.command(f"tftpboot {ram_address} {file.name}", timeout=40)
        match = re.search(rb"Bytes transferred = (\d+)", output)
        if match and int(match[1]) == file.stat().st_size and transfer["bytes"] == file.stat().st_size:
            return
        if attempt:
            raise RuntimeError(f"TFTP failed for {file.name}: {transfer['error'] or output[-500:]!r}")
        # DHCP may disappear after reboot. Retry only when the PC address
        # actually changes; do not boot a partial or stale RAM image.
        print("TFTP_INCOMPLETE: waiting for LAN address change", flush=True)
        address = wait_address(interface, previous=address, timeout=60)


