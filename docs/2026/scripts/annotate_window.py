# -*- coding: utf-8 -*-
"""Annote les adresses appariees avec leur nom et leur taille, cote par cote.

Sert a VALIDER l'appariement par co-occurrence d'opcodes : deux fonctions
appariees qui portent le meme nom dans les deux IDB, c'est une confirmation
independante de la methode.
"""
import json
import os

import idaapi
import idc
import ida_name
import ida_nalt

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

pairs = json.load(open(os.path.join(D, 'window_pairs.json')))
root = ida_nalt.get_root_filename()
tag = "2026" if "2026" in root else "2025"

out = {"functions": {}, "globals": {}}

for kind in ("functions", "globals"):
    for src, rec in pairs[kind].items():
        addr_s = rec['to'] if tag == "2026" else src
        if addr_s.startswith('['):
            addr_s = addr_s[1:-1]
        try:
            ea = int(addr_s, 16)
        except ValueError:
            continue
        name = ida_name.get_name(ea) or ""
        size = 0
        f = idaapi.get_func(ea)
        if f is not None and f.start_ea == ea:
            size = f.end_ea - f.start_ea
        out[kind]["0x%08x" % ea] = {"name": name, "size": size}

path = os.path.join(D, 'annot_%s_win.json' % tag)
json.dump(out, open(path, 'w'), indent=1)
print("ecrit : %s  (%d fonctions, %d globales)"
      % (path, len(out['functions']), len(out['globals'])))

named = sum(1 for v in out['functions'].values()
            if v['name'] and not v['name'].startswith('sub_'))
print("fonctions portant un nom non generique : %d" % named)
