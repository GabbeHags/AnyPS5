import struct, sys
types = {8:'1D',9:'2D',10:'3D',11:'CUBE',12:'1D_ARRAY',13:'2D_ARRAY',14:'2D_MSAA',15:'2D_MSAA_ARRAY'}
sel = 'zo??XYZW'.replace('z','0').replace('o','1')
def decode(w, label):
    t = w[3] >> 28
    if t < 8:
        return
    base = (w[0] | ((w[1] & 0xff) << 32)) << 8
    fmt = (w[1] >> 20) & 0x1ff
    width = (((w[2] & 0x3fff) << 2) | (w[1] >> 30)) + 1
    height = ((w[2] >> 14) & 0xffff) + 1
    swz = ''.join(sel[(w[3] >> s) & 7] for s in (0, 3, 6, 9))
    base_level = (w[3] >> 12) & 0xf; last_level = (w[3] >> 16) & 0xf
    sw = (w[3] >> 20) & 0x1f
    depth = (w[4] & 0x1fff) + 1
    print(f'{label}: base 0x{base:x} {types.get(t,t)} {width}x{height} depth/layers {depth} fmt {fmt} swizzle {swz} mips {base_level}-{last_level} swmode {sw} words ' + ' '.join(f'{x:08x}' for x in w))
for path in sys.argv[1:]:
    data = open(path, 'rb').read()
    for off in range(0, len(data) - 31, 16):
        w = struct.unpack_from('<8I', data, off)
        decode(w, f'{path}+0x{off:x}')
