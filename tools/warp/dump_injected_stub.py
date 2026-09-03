# Dumpe le code injecte par WARP vers lequel le default de UIMenuIconWnd_OnMsg est redirige.
import struct, os
LIVRE = r"E:\Nouveau dossier\Moonlight-Destiny\Moonlight-Destiny.exe"
IMGBASE = 0x400000

def sections(data):
    e = struct.unpack_from('<I', data, 0x3C)[0]
    n = struct.unpack_from('<H', data, e + 6)[0]
    opt = struct.unpack_from('<H', data, e + 20)[0]
    tbl = e + 24 + opt
    out = []
    for i in range(n):
        o = tbl + i * 40
        name = data[o:o+8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rsize, raddr = struct.unpack_from('<IIII', data, o + 8)
        out.append((name, vaddr, vsize, raddr, rsize))
    return out

def va_to_off(secs, va):
    rva = va - IMGBASE
    for name, sva, vs, ra, rs in secs:
        if sva <= rva < sva + max(vs, rs):
            return ra + (rva - sva), name
    return None, None

data = open(LIVRE, 'rb').read()
secs = sections(data)
print("sections :")
for name, va, vs, ra, rs in secs:
    print("  %-8s VA 0x%08X..0x%08X  fichier 0x%08X (+0x%X)" % (name, IMGBASE+va, IMGBASE+va+vs, ra, rs))
print()

# 1) recalculer la cible exacte du call patche
CALL_SITE = 0x00814ADE           # 'call' dans le default de UIMenuIconWnd_OnMsg
off, sec = va_to_off(secs, CALL_SITE)
op = data[off]
rel = struct.unpack_from('<i', data, off + 1)[0]
cible = CALL_SITE + 5 + rel
print("site d appel 0x%08X : opcode 0x%02X, rel32 = 0x%08X -> CIBLE 0x%08X" % (CALL_SITE, op, rel & 0xFFFFFFFF, cible))
toff, tsec = va_to_off(secs, cible)
print("   la cible est dans la section '%s' (offset fichier 0x%X)" % (tsec, toff))
print()

# 2) dumper le stub
N = 160
blob = data[toff:toff+N]
print("=== octets du stub injecte @0x%08X ===" % cible)
for i in range(0, N, 16):
    chunk = blob[i:i+16]
    print("  0x%08X  %-48s %s" % (cible + i, chunk.hex(' '),
          ''.join(chr(c) if 32 <= c < 127 else '.' for c in chunk)))

# 3) reperer les call/jmp rel32 dans le stub pour savoir ou il retourne
print()
print("=== call/jmp rel32 reperes dans le stub ===")
for i in range(N - 5):
    if blob[i] in (0xE8, 0xE9):
        r = struct.unpack_from('<i', blob, i + 1)[0]
        dst = cible + i + 5 + r
        if 0x400000 < dst < 0x1800000:
            kind = 'call' if blob[i] == 0xE8 else 'jmp '
            print("  +0x%02X %s 0x%08X" % (i, kind, dst))
