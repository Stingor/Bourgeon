# -*- coding: utf-8 -*-
"""Version LEGERE : ne scanne que les fonctions qui referencent g_session.

Le scan complet du binaire depasse le budget du pont MCP et se fait tuer. Or
seules ces fonctions-la nous interessent : ce sont elles qui accedent aux
membres de la session par deplacement.
"""
import json
import os
from collections import Counter

import idaapi
import idautils
import ida_name
import ida_nalt

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
LO, HI = 0x400, 0x2000

root = ida_nalt.get_root_filename()
tag = "2026" if "2026" in root else "2025"
GSESS = 0x014B73B0 if tag == "2026" else 0x015FA3C0

users = set()
for xr in idautils.XrefsTo(GSESS):
    f = idaapi.get_func(xr.frm)
    if f:
        users.add(f.start_ea)
print("%s : g_session 0x%08X, %d fonction(s)" % (tag, GSESS, len(users)))

disp = Counter()
sites = {}
for fea in users:
    f = idaapi.get_func(fea)
    if f is None:
        continue
    for head in idautils.Heads(f.start_ea, f.end_ea):
        insn = idaapi.insn_t()
        if idaapi.decode_insn(insn, head) <= 0:
            continue
        for op in insn.ops:
            if op.type == idaapi.o_void:
                break
            if op.type == idaapi.o_displ and LO <= op.addr < HI:
                disp[op.addr] += 1
                sites.setdefault(op.addr, []).append("0x%08x" % head)

print("  %d deplacements distincts dans %04X..%04X (%d occurrences)"
      % (len(disp), LO, HI, sum(disp.values())))
print("  les 20 plus frequents :")
for d, n in disp.most_common(20):
    print("     +0x%-6X %3d" % (d, n))

json.dump({"tag": tag, "gsess": "0x%08x" % GSESS, "nusers": len(users),
           "disp": {("0x%x" % k): v for k, v in disp.items()},
           "sites": {("0x%x" % k): v[:6] for k, v in sites.items()}},
          open(os.path.join(D, 'disp_%s.json' % tag), 'w'), indent=1)
print("ecrit : disp_%s.json" % tag)
