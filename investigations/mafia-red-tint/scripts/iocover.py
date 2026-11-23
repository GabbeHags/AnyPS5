"""List [dispatch-io] resources whose range covers an address.

usage: python iocover.py run.log 0x4537220000 [bytes]
Prints the line number, the covering resource (kind, base+size, shape) and the dispatch line's head.
"""
import re
import sys

path, target = sys.argv[1], int(sys.argv[2], 16)
span = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x10000
resource = re.compile(r'(buffer\(ro\)|buffer|texture|storage)\s+0x([0-9a-f]+)\+0x([0-9a-f]+)(\([^)]*\))?\s*(nz=[0-9.]+)?')
hits = 0
with open(path, 'r', errors='replace') as f:
    for number, line in enumerate(f, 1):
        if '[dispatch-io]' not in line:
            continue
        for m in resource.finditer(line):
            base, size = int(m.group(2), 16), int(m.group(3), 16)
            if base < target + span and target < base + size:
                hits += 1
                if hits <= 60:
                    print(f'{number}: {m.group(1)} 0x{base:x}+0x{size:x}{m.group(4) or ""} {m.group(5) or ""} | {line.strip()[:160]}')
print(f'{hits} covering resources')
