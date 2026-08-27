# -*- coding: utf-8 -*-
"""Les adresses restantes situees AU MILIEU d'une fonction (sites de patch).

Elles ne sont ni un symbole ni un debut de fonction : aucun des vecteurs
precedents ne peut les atteindre. Mais si la fonction QUI LES CONTIENT est
appariee, on peut esperer les retrouver par leur position dans cette fonction.

Ce script mesure d'abord si le cas se presente, et prepare le contexte d'octets
qui servira a les localiser.
"""
import io
import json
import os

import idaapi
import idautils
import ida_bytes
import ida_name

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"

reste = json.load(io.open(os.path.join(DOCS, 'reste_a_porter.json'), encoding='utf-8'))
pairs = json.load(io.open(os.path.join(DOCS, 'all_pairs_final.json'), encoding='utf-8'))

res = {"dans_fn_appariee": {}, "dans_fn_non_appariee": 0, "hors_fonction": 0,
       "debut_de_fonction": 0}
for addr in sorted(reste):
    ea = int(addr, 16)
    f = idaapi.get_func(ea)
    if f is None:
        res["hors_fonction"] += 1
        continue
    if f.start_ea == ea:
        res["debut_de_fonction"] += 1
        continue
    fk = "0x%08x" % f.start_ea
    if fk not in pairs:
        res["dans_fn_non_appariee"] += 1
        continue
    # contexte : 24 octets avant, 24 apres, et l'instruction elle-meme
    insn = idaapi.insn_t()
    ln = idaapi.decode_insn(insn, ea)
    res["dans_fn_appariee"][addr] = {
        "fn2025": fk,
        "fn2026": pairs[fk]['to'],
        "fn_name": ida_name.get_name(f.start_ea) or "",
        "offset": ea - f.start_ea,
        "fn_size": f.end_ea - f.start_ea,
        "insn_len": ln,
        "mnem": insn.get_canon_mnem() if ln else "",
        "bytes": ida_bytes.get_bytes(ea, min(16, f.end_ea - ea)).hex(),
        "before": ida_bytes.get_bytes(max(f.start_ea, ea - 16), min(16, ea - f.start_ea)).hex(),
        "noms": reste[addr]['names'],
    }

print("=== adresses restantes situees dans du CODE ===")
print("  dans une fonction APPARIEE     : %d" % len(res["dans_fn_appariee"]))
print("  dans une fonction non appariee : %d" % res["dans_fn_non_appariee"])
print("  au DEBUT d'une fonction        : %d" % res["debut_de_fonction"])
print("  hors de toute fonction         : %d" % res["hors_fonction"])

from collections import Counter
c = Counter(v['mnem'] for v in res["dans_fn_appariee"].values())
print("\n  instruction visee :")
for m, n in c.most_common(12):
    print("     %-10s %d" % (m or '(indecodable)', n))

path = os.path.join(D, 'literals_2025.json')
json.dump(res["dans_fn_appariee"], open(path, 'w'), indent=1)
print("\necrit : %s" % path)
