"""Write Linux newc initramfs entries."""


def cpio_file(name, data, mode, ino):
    name = name.encode() + b"\0"
    fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name), 0]
    entry = b"070701" + b"".join(f"{v:08x}".encode() for v in fields) + name
    entry += b"\0" * (-len(entry) % 4)
    entry += data
    return entry + b"\0" * (-len(entry) % 4)


