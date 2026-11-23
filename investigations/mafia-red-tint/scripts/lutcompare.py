"""Compare a good and a bad 64 KiB LUT block (lut-watch dumps).

Texel slots are the dwords the good LUT fills (it has 4096 nonzero dwords and zero padding);
reports what the bad block holds in those slots and in the padding, and its float content.
usage: python lutcompare.py good.bin bad.bin
"""
import collections
import struct
import sys

good = struct.unpack('<16384I', open(sys.argv[1], 'rb').read())
bad = struct.unpack('<16384I', open(sys.argv[2], 'rb').read())
slots = [i for i, v in enumerate(good) if v]
pad = [i for i, v in enumerate(good) if not v]
print(f'good: {len(slots)} texel slots, {len(pad)} padding dwords; first slots {slots[:8]}')


def describe(name, idx):
    vals = [bad[i] for i in idx]
    c = collections.Counter(vals)
    same = sum(1 for i in idx if bad[i] == good[i])
    print(f'{name}: {len(idx)} dwords, {c[0xff800000]} -inf, {c[0]} zero, {len(c)} distinct, {same} equal to good')
    for v, n in c.most_common(6):
        f = struct.unpack('<f', struct.pack('<I', v))[0]
        print(f'    0x{v:08x} x{n}  float {f:.6g}  rgba {v & 0xff},{(v >> 8) & 0xff},{(v >> 16) & 0xff},{v >> 24}')


describe('bad in texel slots', slots)
describe('bad in padding', pad)
alpha_ff = sum(1 for i in slots if bad[i] >> 24 == 0xff)
print(f'bad texel slots with alpha 0xff: {alpha_ff}')
floats = [struct.unpack('<f', struct.pack('<I', v))[0] for v in bad]
finite = [f for f in floats if f == f and abs(f) != float('inf')]
small = sum(1 for f in finite if 1e-6 < abs(f) < 1e6)
print(f'bad as float32: {len(finite)} finite, {small} with magnitude 1e-6..1e6')
print('bad first 64 dwords:')
for r in range(0, 64, 8):
    print('   ', ' '.join(f'{v:08x}' for v in bad[r:r + 8]))
