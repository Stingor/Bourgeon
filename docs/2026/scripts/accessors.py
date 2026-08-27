# -*- coding: utf-8 -*-
"""Releve TOUS les accesseurs triviaux `mov eax,[ecx+disp]` / `retn`.

Ces micro-fonctions de 7 octets exposent un offset de structure EN CLAIR, et le
compilateur les emet CONTIGUES, dans l'ordre de leur declaration. La sequence des
deplacements d'un bloc est donc une empreinte : on peut apparier les blocs entre
deux builds et en deduire le decalage de la structure, membre par membre.

C'est ce qui a permis de mesurer -0x38 sur la zone des listes d'objets la ou le
profil statistique des deplacements se contredisait.
"""
import json
import os

import idaapi
import idautils
import ida_bytes
import ida_name
import ida_nalt

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

root = ida_nalt.get_root_filename()
tag = "2026" if "2026" in root else "2025"

# mov eax,[ecx+disp32] ; retn      -> 8B 81 dd dd dd dd C3
# mov eax,[ecx+disp8]  ; retn      -> 8B 41 dd C3
PATS = [("8B 81 ?? ?? ?? ?? C3", 2, 4), ("8B 41 ?? C3", 2, 1)]

acc = []
for pat, off, size in PATS:
    ea = 0
    while True:
        ea = ida_bytes.find_bytes(pat, ea + 1) if hasattr(ida_bytes, 'find_bytes') else idaapi.BADADDR
        if ea == idaapi.BADADDR:
            break
        d = ida_bytes.get_dword(ea + off) if size == 4 else ida_bytes.get_byte(ea + off)
        acc.append((ea, d, size))

acc.sort()
print("%s : %d accesseurs triviaux" % (tag, len(acc)))

# --- regroupement en blocs contigus (pas <= 0x20) -----------------------------
blocs = []
cur = []
for ea, d, sz in acc:
    if cur and ea - cur[-1][0] > 0x20:
        blocs.append(cur)
        cur = []
    cur.append((ea, d, sz))
if cur:
    blocs.append(cur)

blocs = [b for b in blocs if len(b) >= 3]
print("  %d blocs de >= 3 accesseurs contigus" % len(blocs))
for b in sorted(blocs, key=lambda x: -len(x))[:12]:
    ds = " ".join("%X" % d for _, d, _ in b[:12])
    print("     0x%08X  %2d acc.  %s%s"
          % (b[0][0], len(b), ds, " ..." if len(b) > 12 else ""))

out = {"tag": tag,
       "accesseurs": [{"ea": "0x%08x" % ea, "disp": "0x%x" % d, "sz": sz} for ea, d, sz in acc],
       "blocs": [[{"ea": "0x%08x" % ea, "disp": "0x%x" % d} for ea, d, _ in b] for b in blocs]}
path = os.path.join(D, 'accessors_%s.json' % tag)
json.dump(out, open(path, 'w'), indent=1)
print("ecrit : %s" % path)
