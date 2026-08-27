# -*- coding: utf-8 -*-
"""Releve nom + taille des adresses du portage existant, cote par cote.

Sert a AUDITER port_2025_2026.json : une paire dont les deux fonctions ont des
tailles tres differentes est suspecte, et plusieurs sources pointant vers une
meme cible est une impossibilite.
"""
import json
import os

import idaapi
import ida_name
import ida_nalt

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
REF = r"d:/Mes documents/GitHub/Bourgeon/docs/2026/port_2025_2026.json"

ref = json.load(open(REF))
root = ida_nalt.get_root_filename()
tag = "2026" if "2026" in root else "2025"

out = {}
for src, rec in ref.items():
    a = rec['to'] if tag == "2026" else src
    try:
        ea = int(a, 16)
    except ValueError:
        continue
    f = idaapi.get_func(ea)
    out["0x%08x" % ea] = {
        "name": ida_name.get_name(ea) or "",
        "size": (f.end_ea - f.start_ea) if (f and f.start_ea == ea) else 0,
        "isfunc": bool(f and f.start_ea == ea),
    }

path = os.path.join(D, 'audit_%s.json' % tag)
json.dump(out, open(path, 'w'), indent=1)
print("ecrit : %s (%d adresses)" % (path, len(out)))
