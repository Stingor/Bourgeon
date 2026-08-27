# -*- coding: utf-8 -*-
"""Appariement par signature de co-occurrence sur les ID DE FENETRE.

Meme methode que pour les opcodes, autre identifiant stable : l'id passe a
UIWindowMgr_MakeWindow. Verifie au prealable (correlation de rang des adresses
de case : r=0.90 au decalage 0 contre ~0.50 ailleurs).
"""
import json
import os
from collections import defaultdict

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
a = json.load(open(os.path.join(D, 'switch_makewindow_2025.json')))['cases']
b = json.load(open(os.path.join(D, 'switch_makewindow_2026.json')))['cases']
common = sorted(set(a) & set(b), key=int)
print("ids communs : %d" % len(common))


def collect(cases, kind):
    sig = defaultdict(set)
    for o in common:
        items = cases[o]['calls'] + cases[o]['calls2'] if kind == 'calls' else cases[o]['globals']
        for g in items:
            sig[g].add(o)
    return {g: frozenset(s) for g, s in sig.items()}


def match(sa, sb, label):
    ga, gb = defaultdict(list), defaultdict(list)
    for g, s in sa.items():
        ga[s].append(g)
    for g, s in sb.items():
        gb[s].append(g)
    pairs, amb, orph = {}, 0, 0
    for s, la in ga.items():
        lb = gb.get(s)
        if not lb:
            orph += 1
            continue
        if len(la) == 1 and len(lb) == 1:
            pairs[la[0]] = {"to": lb[0], "nid": len(s), "ids": sorted(s, key=int)}
        else:
            amb += 1
    print("\n=== %s ===" % label)
    print("  objets 2025 / 2026 : %d / %d" % (len(sa), len(sb)))
    print("  paires CERTAINES   : %d  (dont %d avec >= 3 ids)"
          % (len(pairs), sum(1 for p in pairs.values() if p['nid'] >= 3)))
    print("  ambigues / orphelines : %d / %d" % (amb, orph))
    return pairs


fn = match(collect(a, 'calls'), collect(b, 'calls'), "FONCTIONS (fabrique de fenetres)")
gl = match(collect(a, 'globals'), collect(b, 'globals'), "GLOBALES (fabrique de fenetres)")

json.dump({"functions": fn, "globals": gl},
          open(os.path.join(D, 'window_pairs.json'), 'w'), indent=1)
print("\necrit : window_pairs.json")
