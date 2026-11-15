"""Serve one named local file for one TFTP read, then exit. No uploads."""
import argparse
import socket
import struct
import time
import threading
from contextlib import contextmanager
from pathlib import Path


def serve_file(listener, file, timeout=90, stop=None, peer_ip=None):
    stop = stop or threading.Event()
    listener.settimeout(0.2)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and not stop.is_set():
        try:
            request, peer = listener.recvfrom(4096)
        except socket.timeout:
            continue
        parts = request[2:].split(b"\0")
        if peer_ip and peer[0] != peer_ip:
            continue
        if request[:2] != b"\0\1" or parts[:2] != [file.name.encode(), b"octet"]:
            listener.sendto(b"\0\5\0\1File unavailable\0", peer)
            continue
        print(f"TFTP_RRQ: {peer[0]}:{peer[1]}", flush=True)
        # Ignore options: a DATA response selects standard 512-byte blocks.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as transfer, file.open("rb") as source:
            transfer.bind((listener.getsockname()[0], 0))
            transfer.settimeout(1)
            block, total = 1, 0
            while not stop.is_set():
                chunk = source.read(512)
                packet = struct.pack("!HH", 3, block) + chunk
                acknowledged = False
                for attempt in range(8):
                    if stop.is_set():
                        return
                    transfer.sendto(packet, peer)
                    try:
                        ack, sender = transfer.recvfrom(4096)
                    except socket.timeout:
                        continue
                    if sender == peer and ack == struct.pack("!HH", 4, block):
                        acknowledged = True
                        break
                if not acknowledged:
                    raise TimeoutError(f"TFTP block {block} was not acknowledged")
                total += len(chunk)
                if len(chunk) < 512:
                    print(f"TFTP_COMPLETE: {total} bytes", flush=True)
                    return total
                block = (block + 1) & 0xffff
    if not stop.is_set():
        raise TimeoutError("No matching TFTP request received")


@contextmanager
def file_server(file, peer_ip, timeout=40, bind='0.0.0.0'):
    """Bind before yielding; stop/join the one-transfer worker on every exit."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.bind((bind, 69))
        stop = threading.Event()
        result = {"bytes": None, "error": None}

        def worker():
            try:
                result["bytes"] = serve_file(listener, file, timeout, stop, peer_ip)
            except Exception as error:
                result["error"] = error

        thread = threading.Thread(target=worker, name="ram-boot-tftp")
        thread.start()
        try:
            print(f"TFTP_READY: UDP69 {file.name}", flush=True)
            yield result
        finally:
            stop.set()
            thread.join()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("--bind", required=True)
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.bind((args.bind, 69))
        print(f"TFTP_READY: {args.bind}:69 {args.file.name}", flush=True)
        serve_file(listener, args.file, args.timeout)


if __name__ == "__main__":
    main()
