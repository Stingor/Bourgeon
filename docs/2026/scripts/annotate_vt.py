# -*- coding: utf-8 -*-
"""Taille et nom des 4327 methodes virtuelles appariees, cote par cote."""
import io
import json
import os

import idaapi
import ida_name
import ida_nalt

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"

vt = json.load(io.open(os.path.join(DOCS, 'vtable_pairs.json'), encoding='utf-8'))
root = ida_nalt.get_root_filename()
tag = "2026" if "2026" in root else "2025"

out = {}
for src, rec in vt.items():
    a = rec['to'] if tag == "2026" else src
    ea = int(a, 16)
    f = idaapi.get_func(ea)
    out[a] = {"name": ida_name.get_name(ea) or "",
              "size": (f.end_ea - f.start_ea) if (f and f.start_ea == ea) else 0}

path = os.path.join(D, 'annot_%s_vt.json' % tag)
json.dump(out, open(path, 'w'))
nf = sum(1 for v in out.values() if v['size'])
print("ecrit : %s  (%d adresses, %d sont des debuts de fonction)" % (path, len(out), nf))
