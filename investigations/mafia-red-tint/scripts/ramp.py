import sys
d = open('around.bin','rb').read(); base = 0x4547400000
hits = []
for off in range(0, len(d) - 64, 4):
    for ch in range(3):
        v = d[off+ch:off+64:4]
        if len(v) < 16: continue
        if v[0] <= 20 and v[15] >= 230 and all(v[i] <= v[i+1] for i in range(15)) and v[15] - v[0] > 200:
            hits.append((off, ch)); break
print(len(hits), 'ramp starts')
from collections import Counter
c = Counter((base + o) & ~0xffff for o, _ in hits)
for blk, n in c.most_common(15): print(hex(blk), n)
