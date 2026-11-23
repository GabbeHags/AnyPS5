"""Find code that references a string (rip-relative lea / mov) in a PE image.

usage: python xref.py game.exe "C_ColorGradingTextureUpdater" [more strings]
Prints the image offset of each instruction whose rip-relative target is the string, plus any 8-byte
pointer to the string in the data sections (vtables / type info often hold those).
"""
import struct
import sys

exe = sys.argv[1]
data = open(exe, 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
optional = pe + 24
image_base = struct.unpack_from('<Q', data, optional + 24)[0]
count = struct.unpack_from('<H', data, pe + 6)[0]
table = optional + struct.unpack_from('<H', data, pe + 20)[0]
sections = []
for i in range(count):
    entry = table + i * 40
    name = data[entry:entry + 8].rstrip(b'\0').decode(errors='replace')
    vsize, vaddr, rsize, raw, = struct.unpack_from('<IIII', data, entry + 8)
    characteristics = struct.unpack_from('<I', data, entry + 36)[0]
    sections.append((name, vaddr, vsize, raw, rsize, characteristics))


def rva_of(file_offset):
    for name, vaddr, vsize, raw, rsize, _ in sections:
        if raw <= file_offset < raw + rsize:
            return vaddr + file_offset - raw
    return None


for text in sys.argv[2:]:
    needle = text.encode() + b'\0'
    at = data.find(needle)
    while at != -1 and at > 0 and data[at - 1] != 0:
        at = data.find(needle, at + 1)
    if at == -1:
        print(f'{text}: not found')
        continue
    target = rva_of(at)
    print(f'{text}: string at +0x{target:x}')
    for name, vaddr, vsize, raw, rsize, characteristics in sections:
        if not characteristics & 0x20000000:
            continue
        code = data[raw:raw + rsize]
        for i in range(len(code) - 7):
            if code[i] in (0x48, 0x4c) and code[i + 1] in (0x8d, 0x8b) and (code[i + 2] & 0xc7) == 0x05:
                disp = struct.unpack_from('<i', code, i + 3)[0]
                if vaddr + i + 7 + disp == target:
                    print(f'  code ref at +0x{vaddr + i:x}')
    pointer = struct.pack('<Q', image_base + target)
    at = data.find(pointer)
    while at != -1:
        print(f'  pointer at +0x{rva_of(at):x}')
        at = data.find(pointer, at + 1)
