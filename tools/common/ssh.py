"""Bounded SSH command and byte-stream exchange."""
import time


def exchange(client, cmd, payload=b'', limit=65536, seconds=25):
    """Bound elapsed time and output even if a driver streams forever."""
    deadline = time.monotonic() + seconds
    channel = client.get_transport().open_session(timeout=5)
    output, errors = bytearray(), bytearray()
    try:
        channel.settimeout(5)
        channel.exec_command(cmd)
        channel.settimeout(0)
        sent = 0
        eof_sent = False
        while True:
            if time.monotonic() >= deadline:
                raise TimeoutError(f'Remote stage exceeded {seconds}s total limit')
            if sent < len(payload) and channel.send_ready():
                sent += channel.send(payload[sent:sent + 65536])
            if sent == len(payload) and not eof_sent:
                channel.shutdown_write()
                eof_sent = True
            if channel.recv_ready():
                output.extend(channel.recv(65536))
            if channel.recv_stderr_ready():
                errors.extend(channel.recv_stderr(65536))
            if len(output) > limit or len(errors) > 65536:
                raise RuntimeError('Remote output exceeded limit; probe stopped')
            if channel.exit_status_ready() and not channel.recv_ready() and not channel.recv_stderr_ready():
                status = channel.recv_exit_status()
                if status:
                    raise RuntimeError(errors.decode('utf-8', 'replace') or f'Remote exit {status}')
                return bytes(output)
            time.sleep(0.005)
    finally:
        channel.close()


