import struct
d = open('lut.bin','rb').read()
eq4 = [0x00000000,0x00000000,0x00000001,0x00001000,0x01000000,0x00002000,0x00000002,0x02000000,0x00004000,0x00000004,0x04000000,0x00008000,0x00000008,0x08000000,0x00010000,0x00000010]
def addr(x,y,z,eq):
    a = 0
    for bit, m in enumerate(eq):
        v = 0
        for i in range(12):
            if m >> i & 1: v ^= x >> i & 1
        for i in range(12):
            if m >> (12+i) & 1: v ^= y >> i & 1
        for i in range(8):
            if m >> (24+i) & 1: v ^= z >> i & 1
        a |= v << bit
    return a
def texel(off):
    c = d[off:off+4]
    return (c[2], c[1], c[0], c[3])
for name, f in [('linear', lambda x,y,z: ((z*16+y)*16+x)*4), ('swizzle 0x109', lambda x,y,z: addr(x,y,z,eq4))]:
    print('==', name)
    for (x,y,z) in [(0,0,0),(15,0,0),(0,15,0),(0,0,15),(15,15,15),(8,8,8),(4,0,0),(8,0,0),(0,8,0),(0,0,8)]:
        print((x,y,z), texel(f(x,y,z)))
print()
fill = b'\x00\x00\x80\xff'
for name, f in [('linear', lambda x,y,z: ((z*16+y)*16+x)*4), ('swizzle 0x109', lambda x,y,z: addr(x,y,z,eq4))]:
    print('==', name, 'real texels per z slice:', [sum(1 for y in range(16) for x in range(16) if d[f(x,y,z):f(x,y,z)+4] != fill) for z in range(16)])
    print('   per y row:', [sum(1 for z in range(16) for x in range(16) if d[f(x,y,z):f(x,y,z)+4] != fill) for y in range(16)])
    print('   per x col:', [sum(1 for z in range(16) for y in range(16) if d[f(x,y,z):f(x,y,z)+4] != fill) for x in range(16)])
