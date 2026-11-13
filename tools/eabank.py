"""EA sound banks (.abk) to WAV.

An .abk ("ABKC") carries a BNKl bank: "BNKl", u16 version, u16 sound count,
u32 size, then the offsets of the sounds' headers (u32 each, from the BNKl
start, at +0x18). Each header is "PT\\0\\0" and tagged fields -- a tag byte,
a length byte, that many bytes of big-endian value -- up to 0xFF:

    0x80 version   0x82 channels (1)   0x83 codec    0x84 sample rate
    0x85 samples   0x86 loop start     0x87 loop end 0x88 data offset (from BNKl)

0xFD opens the sample's own fields; tags before it are the bank's. With no
codec field, version 2 on PC is EA-XA: 15-byte frames of 28 samples -- a byte
of filter and shift, then 14 bytes of 4-bit deltas, high nibble first -- or a
0xEE frame of 16-bit big-endian samples. Mono only, which is all these are.

    python eabank.py <bank.abk> <out folder>
      writes <bank>_<n>.wav for each sound, and <bank>_<n>_loop.wav, several
      seconds of the loop, for the sounds that loop.
"""
import os
import struct
import sys
import wave

XA_COEF1 = (0, 240, 460, 392)
XA_COEF2 = (0, 0, -208, -220)


def clamp16(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def decode_ea_xa(data, at, samples):
    out = []
    hist1 = hist2 = 0
    while len(out) < samples and at < len(data):
        info = data[at]
        if info == 0xEE:
            hist1, hist2 = struct.unpack_from('>hh', data, at + 1)
            out.extend(struct.unpack_from('>28h', data, at + 5))
            at += 0x3D
            continue
        c1, c2 = XA_COEF1[(info >> 4) & 3], XA_COEF2[(info >> 4) & 3]
        shift = (info & 0x0F) + 8
        for i in range(28):
            byte = data[at + 1 + i // 2]
            nibble = (byte & 0x0F) if i & 1 else (byte >> 4)
            if nibble & 8:
                nibble -= 16
            s = (nibble * (1 << 28)) >> shift   # the nibble at the top of 32 bits, shifted down
            s = clamp16((s + hist1 * c1 + hist2 * c2 + 128) >> 8)
            hist2, hist1 = hist1, s
            out.append(s)
        at += 0x0F
    return out[:samples]


def read_pt(data, at):
    if data[at:at + 4] != b'PT\x00\x00':
        return None
    at += 4
    fields = {}
    own = False
    while at < len(data):
        tag = data[at]
        at += 1
        if tag == 0xFF:
            break
        if tag == 0xFD:
            own = True
            continue
        if tag == 0xFE or tag == 0xFC:
            continue
        n = data[at]
        at += 1
        value = int.from_bytes(data[at:at + n], 'big') if n else 0
        at += n
        fields[tag] = value
    return fields


def sounds(path):
    data = open(path, 'rb').read()
    bnk = data.find(b'BNKl')
    if bnk < 0:
        return data, []
    count = struct.unpack_from('<H', data, bnk + 6)[0]
    out = []
    for i in range(count):
        rel = struct.unpack_from('<I', data, bnk + 0x18 + 4 * i)[0]
        if not rel:
            continue
        pt = read_pt(data, bnk + 0x18 + 4 * i + rel) or read_pt(data, bnk + rel)
        if not pt:
            continue
        out.append((i, pt, bnk))
    return data, out


def write_wav(path, rate, pcm):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack('<%dh' % len(pcm), *pcm))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    data, found = sounds(src)
    stem = os.path.splitext(os.path.basename(src))[0]
    for i, pt, bnk in found:
        codec = pt.get(0x83)
        if codec not in (None,) or pt.get(0x82, 1) != 1:
            print('%s #%d: codec %s, %d channels -- skipped' % (stem, i, codec, pt.get(0x82, 1)))
            continue
        rate, count = pt.get(0x84, 22050), pt.get(0x85, 0)
        pcm = decode_ea_xa(data, bnk + pt.get(0x88, 0), count)
        name = '%s_%d' % (stem, i)
        write_wav(os.path.join(dst, name + '.wav'), rate, pcm)
        loops = 0x87 in pt
        line = '%s: %.2f s at %d Hz, peak %d' % (name, count / rate, rate, max(map(abs, pcm)) if pcm else 0)
        if loops:
            start, end = pt.get(0x86, 0), pt[0x87] + 1
            body = pcm[start:end]
            if body:
                reps = max(2, int(8 * rate / len(body)) + 1)
                write_wav(os.path.join(dst, name + '_loop.wav'), rate, pcm[:start] + body * reps)
                line += ', loops %d..%d' % (start, end)
        print(line)


if __name__ == '__main__':
    main()
