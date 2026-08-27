# -*- coding: utf-8 -*-
"""Appariement sur les 217 tables, avec calcul AUTOMATIQUE des familles.

Deux tables dont les plages de valeurs se chevauchent indexent vraisemblablement
le meme espace d'identifiants (meme famille) : un accord entre elles n'est PAS
une confirmation independante. Les familles sont donc les composantes connexes
du graphe de chevauchement des plages -- calculees, pas devinees.
"""
import json
import os
from collections import defaultdict

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

a = json.load(open(os.path.join(D, 'all_cases_2025.json')))
b = json.load(open(os.path.join(D, 'all_cases_2026.json')))
tp = json.load(open(os.path.join(D, 'table_pairs.json')))
cfg = json.load(open(os.path.join(D, 'switch_targets_all.json')))

# tag -> plage
tag_range = {}
for c, p in zip(cfg, tp):
    tag_range[c['tag']] = (p['lo'], p['hi'], p.get('name25', ''))

# --- familles = composantes connexes du chevauchement de plages --------------
tags = sorted(tag_range)
parent = {t: t for t in tags}


def find(x):
    while parent[x] != x:
        parent[x] = parent[parent[x]]
        x = parent[x]
    return x


def union(x, y):
    rx, ry = find(x), find(y)
    if rx != ry:
        parent[ry] = rx


for i, t1 in enumerate(tags):
    lo1, hi1, _ = tag_range[t1]
    for t2 in tags[i + 1:]:
        lo2, hi2, _ = tag_range[t2]
        inter = min(hi1, hi2) - max(lo1, lo2)
        if inter > 0:
            span = min(hi1 - lo1, hi2 - lo2)
            if span > 0 and inter / float(span) > 0.5:
                union(t1, t2)

fam = {t: find(t) for t in tags}
nfam = len(set(fam.values()))
print("tables : %d, familles d'identifiants (calculees) : %d" % (len(tags), nfam))
sizes = defaultdict(list)
for t, f in fam.items():
    sizes[f].append(t)
for f, ts in sorted(sizes.items(), key=lambda kv: -len(kv[1]))[:8]:
    lo, hi, nm = tag_range[f]
    print("   famille %-5s : %2d tables, plage %d..%d  (%s)" % (f, len(ts), lo, hi, nm[:34]))

ca, cb = a['cases'], b['cases']
common = sorted(set(ca) & set(cb))
print("\ncases 2025 / 2026 / communs : %d / %d / %d" % (len(ca), len(cb), len(common)))


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
            tabs = sorted(set(x.split(':')[0] for x in s))
            fams = sorted(set(fam.get(t, t) for t in tabs))
            pairs[la[0]] = {"to": lb[0], "n": len(s), "tables": tabs, "familles": fams}
        else:
            amb += 1
    ind = sum(1 for p in pairs.values() if len(p['familles']) >= 2)
    print("\n=== %s ===" % label)
    print("  objets 2025 / 2026 : %d / %d" % (len(sa), len(sb)))
    print("  paires CERTAINES   : %d" % len(pairs))
    print("     vues par >= 2 FAMILLES independantes : %d" % ind)
    print("  ambigues / orphelines : %d / %d" % (amb, orph))
    return pairs


fn = match(collect(ca, 'calls'), collect(cb, 'calls'), "FONCTIONS")
gl = match(collect(ca, 'globals'), collect(cb, 'globals'), "GLOBALES")

json.dump({"functions": fn, "globals": gl},
          open(os.path.join(D, 'all_pairs.json'), 'w'), indent=1)
print("\necrit : all_pairs.json  (%d paires)" % (len(fn) + len(gl)))
