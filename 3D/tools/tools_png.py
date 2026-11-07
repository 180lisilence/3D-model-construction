"""把叠层落盘的 BMP（预乘 alpha）合成到深色背景上，方便肉眼检查。

用法： python tools_png.py in.bmp out.png [r,g,b]
"""
import struct
import sys
import zlib


def read_bmp(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:2] == b'BM', 'not a bmp'
    off = struct.unpack_from('<I', data, 10)[0]
    w, h = struct.unpack_from('<ii', data, 18)
    bpp = struct.unpack_from('<H', data, 28)[0]
    assert bpp == 32, bpp
    topdown = h < 0
    h = abs(h)
    stride = w * 4
    rows = []
    for y in range(h):
        sy = y if topdown else (h - 1 - y)
        rows.append(data[off + sy * stride: off + (sy + 1) * stride])
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b''.join(b'\x00' + r for r in rows)

    def chunk(tag, payload):
        return (struct.pack('>I', len(payload)) + tag + payload +
                struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff))

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    bg = (0x0B, 0x10, 0x20)
    if len(sys.argv) > 3:
        bg = tuple(int(v) for v in sys.argv[3].split(','))

    w, h, rows = read_bmp(src)
    out = []
    for row in rows:
        buf = bytearray(w * 4)
        for x in range(w):
            b, g, r, a = row[x * 4:x * 4 + 4]
            ia = 255 - a
            # 预乘 alpha：rgb 已经是 color*alpha，直接加上背景的剩余部分
            rr = min(255, r + bg[0] * ia // 255)
            gg = min(255, g + bg[1] * ia // 255)
            bb = min(255, b + bg[2] * ia // 255)
            buf[x * 4 + 0] = rr
            buf[x * 4 + 1] = gg
            buf[x * 4 + 2] = bb
            buf[x * 4 + 3] = 255
        out.append(bytes(buf))

    write_png(dst, w, h, out)
    print(f'{dst}  {w}x{h}')


if __name__ == '__main__':
    main()
