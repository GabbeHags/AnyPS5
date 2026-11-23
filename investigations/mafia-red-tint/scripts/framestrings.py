"""Print the strings that the functions around given code offsets reference (rip-relative lea).

usage: python framestrings.py game.exe 0x8d8742 0x8dda00 ...
Disassembles +-0x600 bytes around each offset (image base read from the PE header) with objdump and prints
each rip-relative lea target that points at printable text.
"""
import re
import struct
import subprocess
import sys

exe = sys.argv[1]
data = open(exe, 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
optional = pe + 24
image_base = struct.unpack_from('<Q', data, optional + 24)[0]
sections = []
count = struct.unpack_from('<H', data, pe + 6)[0]
table = optional + struct.unpack_from('<H', data, pe + 20)[0]
for i in range(count):
    entry = table + i * 40
    vsize, vaddr, rsize, raw = struct.unpack_from('<IIII', data, entry + 8)
    sections.append((vaddr, max(vsize, rsize), raw))


def read_text(rva):
    for vaddr, size, raw in sections:
        if vaddr <= rva < vaddr + size:
            off = raw + rva - vaddr
            end = data.find(b'\0', off, off + 200)
            text = data[off:end if end != -1 else off + 200]
            if len(text) >= 4 and all(32 <= c < 127 for c in text):
                return text.decode()
    return None


lea = re.compile(r'lea\s+0x[0-9a-f]+\(%rip\),%\w+\s+# (0x[0-9a-f]+)')
for arg in sys.argv[2:]:
    offset = int(arg, 16)
    start, stop = image_base + offset - 0x600, image_base + offset + 0x200
    out = subprocess.run(['objdump', '-d', '--no-show-raw-insn', f'--start-address={start}', f'--stop-address={stop}', exe], capture_output=True, text=True).stdout
    seen = []
    for m in lea.finditer(out):
        text = read_text(int(m.group(1), 16) - image_base)
        if text and text not in seen:
            seen.append(text)
    print(f'+{arg}: ' + (' | '.join(seen[:12]) if seen else '(no strings)'))
