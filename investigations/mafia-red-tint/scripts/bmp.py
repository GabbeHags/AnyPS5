import struct, sys, zlib
def read(path):
    d = open(path,'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]; w, h = struct.unpack_from('<ii', d, 18); bpp = struct.unpack_from('<H', d, 28)[0]
    return d, off, w, h, bpp
def png(path, out, step):
    d, off, w, h, bpp = read(path); bp = bpp // 8; stride = (w * bp + 3) & ~3
    rows = []; tot = [0,0,0]; n = 0
    hh = abs(h)
    for y in range(0, hh, step):
        sy = hh - 1 - y if h > 0 else y
        base = off + sy * stride; row = bytearray([0])
        for x in range(0, w, step):
            b, g, r = d[base + x*bp], d[base + x*bp + 1], d[base + x*bp + 2]
            row += bytes((r, g, b)); tot[0]+=r; tot[1]+=g; tot[2]+=b; n+=1
        rows.append(bytes(row))
    ow = len(range(0, w, step)); oh = len(rows)
    def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    open(out,'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', ow, oh, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b''))
    print(path, w, h, bpp, 'avg rgb %.1f %.1f %.1f' % tuple(t/n for t in tot))
for p in sys.argv[1:]: png(p, p.replace('.bmp', '-small.png'), 6)
