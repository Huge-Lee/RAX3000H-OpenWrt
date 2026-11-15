"""RAX3000H UBI geometry and lossless SquashFS extraction/restore wrapping."""
import math
import struct
import zlib

PEB = 131072
DATA = 4096
LEB = PEB - DATA


def crc(data):
    return zlib.crc32(data) ^ 0xffffffff


def wrap(squashfs):
    if squashfs[:4] != b'hsqs':
        raise ValueError('Not SquashFS')
    size = struct.unpack_from('<Q', squashfs, 40)[0]
    if size > len(squashfs):
        raise ValueError('Incomplete SquashFS')
    # ubiblock exposes whole 512-byte sectors only. Trimming to bytes_used
    # without restoring padding can hide the final filesystem metadata.
    # Match mksquashfs's normal 4 KiB padding, leaving its content unchanged.
    squashfs = squashfs[:size] + b'\0' * (-size % 4096)
    count = math.ceil(len(squashfs) / LEB)
    if count > 220:
        raise ValueError('SquashFS exceeds usable rootfs partition capacity')
    record = bytearray(168)
    struct.pack_into('>III', record, 0, count, 1, 0)
    record[12] = 2
    struct.pack_into('>H', record, 14, 8)
    record[16:24] = b'squashfs'
    table = bytes(record) + struct.pack('>I', crc(record))
    empty = bytes(168)
    table += (empty + struct.pack('>I', crc(empty))) * 127
    result = bytearray()
    for index in range(count + 2):
        ec = bytearray(60)
        struct.pack_into('>IB3xQIII', ec, 0, 0x55424923, 1, 0, 2048, DATA, 1)
        vid = bytearray(60)
        if index < 2:
            struct.pack_into('>IBBBBII', vid, 0, 0x55424921, 1, 1, 0, 5, 0x7fffefff, index)
            data = table
        else:
            data = squashfs[(index-2)*LEB:(index-1)*LEB]
            struct.pack_into('>IBBBBII', vid, 0, 0x55424921, 1, 2, 0, 0, 0, index-2)
            struct.pack_into('>IIII', vid, 20, len(data), count, 0, crc(data))
        struct.pack_into('>Q', vid, 40, index+1)
        block = bytearray(b'\xff' * PEB)
        block[:64] = ec + struct.pack('>I', crc(ec))
        block[2048:2112] = vid + struct.pack('>I', crc(vid))
        block[DATA:DATA+len(data)] = data
        result += block
    return bytes(result)


def extract(raw):
    volumes = {}
    for offset in range(0, len(raw), PEB):
        block = raw[offset:offset+PEB]
        if block[:4] != b'UBI#':
            continue
        vid_offset, data_offset = struct.unpack_from('>II', block, 16)
        vid = block[vid_offset:vid_offset+64]
        if vid[:4] != b'UBI!' or struct.unpack_from('>I', vid, 8)[0] != 0:
            continue
        if struct.unpack_from('>I', vid, 60)[0] != crc(vid[:60]):
            raise ValueError('Corrupt UBI volume header in backup')
        index = struct.unpack_from('>I', vid, 12)[0]
        length = struct.unpack_from('>I', vid, 20)[0] if vid[5] == 2 else LEB
        data = block[data_offset:data_offset+length]
        if vid[5] == 2 and struct.unpack_from('>I', vid, 32)[0] != crc(data):
            raise ValueError('Corrupt UBI static volume data in backup')
        sequence = struct.unpack_from('>Q', vid, 40)[0]
        if index not in volumes or sequence > volumes[index][0]:
            volumes[index] = sequence, data
    if not volumes or sorted(volumes) != list(range(len(volumes))):
        raise ValueError('Incomplete rootfs UBI volume')
    data = b''.join(volumes[i][1] for i in sorted(volumes))
    if data[:4] != b'hsqs':
        raise ValueError('Original rootfs is not SquashFS')
    length = struct.unpack_from('<Q', data, 40)[0]
    if length > len(data):
        raise ValueError('Truncated original SquashFS')
    return data[:length]
