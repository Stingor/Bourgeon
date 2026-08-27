# -*- coding: utf-8 -*-
"""Appariement des tables, passe ELARGIE.

La passe stricte exigeait (lo, hi, nvals, ntgts) : elle rate les tables dont le
contenu a bouge d'un build a l'autre -- justement les plus grosses et les plus
interessantes (le dispatch de paquets passe de 3011 a 3029 valeurs).

On ajoute donc des criteres plus tolerants, TOUJOURS avec unicite exigee des
deux cotes, et un garde-fou de taille de fonction.
"""
import json
import os
from collections import defaultdict

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
a = json.load(open(os.path.join(D, 'switches_2025.json')))
b = json.load(open(os.path.join(D, 'switches_2026.json')))
print("switches 2025 / 2026 : %d / %d" % (len(a), len(b)))

pairs = []
taken_a, taken_b = set(), set()


def pair_up(keyf, label, ratio_min=0.6, close=None):
    pa = [r for r in a if r['func'] not in taken_a]
    pb = [r for r in b if r['func'] not in taken_b]
    ga, gb = defaultdict(list), defaultdict(list)
    for r in pa:
        ga[keyf(r)].append(r)
    for r in pb:
        gb[keyf(r)].append(r)
    got, amb = [], 0
    for k, la in ga.items():
        lb = gb.get(k)
        if not lb:
            continue
        if len(la) != 1 or len(lb) != 1:
            amb += 1
            continue
        x, y = la[0], lb[0]
        r = min(x['size'], y['size']) / float(max(x['size'], y['size']))
        if r < ratio_min:
            continue
        if close and not close(x, y):
            continue
        got.append({"2025": x['func'], "2026": y['func'], "how": label,
                    "name25": x['name'], "nvals25": x['nvals'], "nvals26": y['nvals'],
                    "lo": x['lo'], "hi": x['hi'], "ratio": round(r, 3)})
    for g in got:
        taken_a.add(g['2025'])
        taken_b.add(g['2026'])
    pairs.extend(got)
    print("  %-38s : %3d paires  (%d ambigues)" % (label, len(got), amb))


def near(x, y, tol=0.03):
    """nombres de valeurs proches a quelques % pres"""
    m = max(x['nvals'], y['nvals'])
    return abs(x['nvals'] - y['nvals']) <= max(4, m * tol)


print("\ndu plus strict au plus tolerant :")
pair_up(lambda r: (r['lo'], r['hi'], r['nvals'], r['ntgts']), "forme EXACTE")
pair_up(lambda r: (r['lo'], r['hi'], r['nvals']), "plage + nb valeurs")
pair_up(lambda r: (r['lo'], r['hi']), "meme plage")
pair_up(lambda r: (r['lo'], r['ntgts']), "meme debut + nb cibles")
# tolerant : meme debut, nb de valeurs PROCHE, taille tres proche
pair_up(lambda r: r['lo'], "meme debut + taille tres proche",
        ratio_min=0.8, close=near)

print("\nTOTAL : %d paires de tables (etait 217)" % len(pairs))
print("  valeurs couvertes : %d" % sum(p['nvals25'] for p in pairs))
gros = sorted(pairs, key=lambda p: -p['nvals25'])[:12]
print("\n  les 12 plus grosses :")
for p in gros:
    print("    %s -> %s  %-6d %-13s %s  [%s]"
          % (p['2025'], p['2026'], p['nvals25'], "%d..%d" % (p['lo'], p['hi']),
             (p['name25'] or '')[:34], p['how']))

cfg = [{"tag": "u%03d" % i, "2025": p['2025'], "2026": p['2026']}
       for i, p in enumerate(pairs)]
json.dump(cfg, open(os.path.join(D, 'switch_targets_all2.json'), 'w'), indent=1)
json.dump(pairs, open(os.path.join(D, 'table_pairs2.json'), 'w'), indent=1)
print("\necrit : switch_targets_all2.json (%d tables)" % len(cfg))
