# -*- coding: utf-8 -*-
"""Recense TOUS les switches exploitables, avec leur vraie signature de table.

Sert a apparier les TABLES elles-memes entre 2025 et 2026 : une table est
identifiee par (plage de valeurs, nombre de valeurs, nombre de cibles), ce qui
ne depend d'aucune adresse.
"""
import json
import os

import idaapi
import idautils
import ida_name
import ida_nalt

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
MIN_CASES = 24

rows = []
for fea in idautils.Functions():
    f = idaapi.get_func(fea)
    if f is None:
        continue
    best, bhead = None, None
    for head in idautils.Heads(f.start_ea, f.end_ea):
        si = idaapi.get_switch_info(head)
        if si and si.ncases >= MIN_CASES and (best is None or si.ncases > best.ncases):
            best, bhead = si, head
    if best is None:
        continue
    try:
        ct = idaapi.calc_switch_cases(bhead, best)
    except Exception:
        continue
    vals = set()
    tgts = set()
    for i in range(len(ct.cases)):
        tgts.add(ct.targets[i])
        for v in ct.cases[i]:
            vals.add(v)
    if len(vals) < MIN_CASES:
        continue
    rows.append({
        "func": "0x%08x" % fea,
        "name": ida_name.get_name(fea) or "",
        "size": f.end_ea - f.start_ea,
        "switch": "0x%08x" % bhead,
        "nvals": len(vals),
        "ntgts": len(tgts),
        "lo": min(vals),
        "hi": max(vals),
    })

root = ida_nalt.get_root_filename()
tag = "2026" if "2026" in root else "2025"
path = os.path.join(D, 'switches_%s.json' % tag)
json.dump(rows, open(path, 'w'), indent=1)
print("%s : %d switches >= %d valeurs" % (tag, len(rows), MIN_CASES))
print("ecrit : %s" % path)
