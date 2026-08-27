# -*- coding: utf-8 -*-
"""Appariement par signature COMPOSITE sur 11 tables indexees par un identifiant
stable (opcode, id de fenetre, effect id, skill id, item id).

Une signature est ici l'ensemble des couples (table, valeur) qui touchent un
objet. Plus un objet est vu par des tables independantes, plus il est
discriminant -- et plus une correspondance fortuite est improbable.
"""
import json
import os
from collections import defaultdict

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
a = json.load(open(os.path.join(D, 'multi_cases_2025.json')))
b = json.load(open(os.path.join(D, 'multi_cases_2026.json')))
ca, cb = a['cases'], b['cases']
common = sorted(set(ca) & set(cb))
print("cases 2025 / 2026 : %d / %d" % (len(ca), len(cb)))
print("cases COMMUNS     : %d" % len(common))

by_table = defaultdict(int)
for k in common:
    by_table[k.split(':')[0]] += 1
print("  par table : %s" % ", ".join("%s=%d" % (t, n) for t, n in sorted(by_table.items())))


def collect(cases, kind):
    sig = defaultdict(set)
    for k in common:
        for g in cases[k][kind]:
            sig[g].add(k)
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
            tables = sorted(set(x.split(':')[0] for x in s))
            pairs[la[0]] = {"to": lb[0], "n": len(s), "tables": tables}
        else:
            amb += 1
    multi = sum(1 for p in pairs.values() if len(p['tables']) >= 2)
    print("\n=== %s ===" % label)
    print("  objets 2025 / 2026 : %d / %d" % (len(sa), len(sb)))
    print("  paires CERTAINES   : %d" % len(pairs))
    print("     vues par >= 2 TABLES independantes : %d" % multi)
    print("     vues par >= 3 cases                : %d"
          % sum(1 for p in pairs.values() if p['n'] >= 3))
    print("  ambigues / orphelines : %d / %d" % (amb, orph))
    return pairs


fn = match(collect(ca, 'calls'), collect(cb, 'calls'), "FONCTIONS")
gl = match(collect(ca, 'globals'), collect(cb, 'globals'), "GLOBALES")

json.dump({"functions": fn, "globals": gl},
          open(os.path.join(D, 'multi_pairs.json'), 'w'), indent=1)
print("\necrit : multi_pairs.json  (%d paires)" % (len(fn) + len(gl)))
