"""JDLZ, the LZ compression of the game's .lzc files and 0x30210 chunks.

Header: "JDLZ", 0x02, 0x10, u16 0, u32 unpacked size, u32 packed size (header
included). Then two interleaved flag streams, one bit per step: the first
says literal byte or back-reference; the second picks the reference's form --
a near one (offset 1..16, length 3..4098) or a far one (offset 17..2064,
length 3..34).

    python jdlz.py packed.lzc unpacked.bin
"""
import struct
import sys


def decompress(data, at=0):
    if data[at:at + 4] != b'JDLZ':
        raise ValueError('not JDLZ')
    size = struct.unpack_from('<I', data, at + 8)[0]
    packed = struct.unpack_from('<I', data, at + 12)[0]
    end = at + packed
    out = bytearray(size)
    pos, o = at + 16, 0
    flags1 = flags2 = 1
    while pos < end and o < size:
        if flags1 == 1:
            flags1 = data[pos] | 0x100
            pos += 1
        if flags2 == 1:
            flags2 = data[pos] | 0x100
            pos += 1
        if flags1 & 1:
            b0, b1 = data[pos], data[pos + 1]
            pos += 2
            if flags2 & 1:
                length = (b1 | ((b0 & 0xF0) << 4)) + 3
                offset = (b0 & 0x0F) + 1
            else:
                offset = (b1 | ((b0 & 0xE0) << 3)) + 17
                length = (b0 & 0x1F) + 3
            for _ in range(length):
                if o >= size:
                    break
                out[o] = out[o - offset]
                o += 1
            flags2 >>= 1
        else:
            out[o] = data[pos]
            o += 1
            pos += 1
        flags1 >>= 1
    if o != size:
        raise ValueError('ended at %d of %d bytes' % (o, size))
    return bytes(out)


if __name__ == '__main__':
    raw = open(sys.argv[1], 'rb').read()
    open(sys.argv[2], 'wb').write(decompress(raw))
