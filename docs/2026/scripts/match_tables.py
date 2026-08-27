# -*- coding: utf-8 -*-
"""Apparie les TABLES (switches) entre 2025 et 2026.

Une table est identifiee par (lo, hi, nvals, ntgts) : sa forme, pas son adresse.
Signature identique ET unique des deux cotes => paire de tables.
"""
import json
import os
from collections import defaultdict

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
a = json.load(open(os.path.join(D, 'switches_2025.json')))
b = json.load(open(os.path.join(D, 'switches_2026.json')))
print("switches 2025 / 2026 : %d / %d" % (len(a), len(b)))


def group(rows, keyf):
    g = defaultdict(list)
    for r in rows:
        g[keyf(r)].append(r)
    return g


def pair_up(keyf, label, pool_a, pool_b, ratio_min=0.6):
    ga, gb = group(pool_a, keyf), group(pool_b, keyf)
    out, amb = [], 0
    for k, la in ga.items():
        lb = gb.get(k)
        if not lb:
            continue
        if len(la) == 1 and len(lb) == 1:
            x, y = la[0], lb[0]
            r = min(x['size'], y['size']) / float(max(x['size'], y['size']))
            if r < ratio_min:
                continue
            out.append({"2025": x['func'], "2026": y['func'], "how": label,
                        "name25": x['name'], "nvals25": x['nvals'], "nvals26": y['nvals'],
                        "lo": x['lo'], "hi": x['hi'], "ratio": round(r, 3)})
        else:
            amb += 1
    print("  %-28s : %3d paires  (%d signatures ambigues)" % (label, len(out), amb))
    return out


pairs = []
taken_a, taken_b = set(), set()

# du plus strict au plus tolerant ; a chaque tour on retire ce qui est deja pris
steps = [
    (lambda r: (r['lo'], r['hi'], r['nvals'], r['ntgts']), "forme EXACTE"),
    (lambda r: (r['lo'], r['hi'], r['nvals']), "meme plage + nb valeurs"),
    (lambda r: (r['lo'], r['hi']), "meme plage"),
    (lambda r: (r['lo'], r['nvals']), "meme debut + nb valeurs"),
]
print("\nappariement des tables, du plus strict au plus tolerant :")
for keyf, label in steps:
    pa = [r for r in a if r['func'] not in taken_a]
    pb = [r for r in b if r['func'] not in taken_b]
    got = pair_up(keyf, label, pa, pb)
    for g in got:
        taken_a.add(g['2025'])
        taken_b.add(g['2026'])
    pairs.extend(got)

print("\nTOTAL : %d paires de tables" % len(pairs))
named = sum(1 for p in pairs if p['name25'] and not p['name25'].startswith('sub_'))
print("  dont %d avec un nom parlant cote 2025" % named)

# apercu des plus grosses
pairs.sort(key=lambda p: -p['nvals25'])
print("\nles 20 plus grosses :")
print("  %-11s %-11s %-7s %-7s %-6s %s" % ("2025", "2026", "nvals", "plage", "ratio", "nom"))
for p in pairs[:20]:
    print("  %s %s %-7d %-7s %-6.2f %s"
          % (p['2025'], p['2026'], p['nvals25'],
             "%d..%d" % (p['lo'], p['hi']), p['ratio'], p['name25'][:40]))

cfg = [{"tag": "t%03d" % i, "2025": p['2025'], "2026": p['2026']}
       for i, p in enumerate(pairs)]
json.dump(cfg, open(os.path.join(D, 'switch_targets_all.json'), 'w'), indent=1)
json.dump(pairs, open(os.path.join(D, 'table_pairs.json'), 'w'), indent=1)
print("\necrit : switch_targets_all.json (%d tables), table_pairs.json" % len(cfg))
