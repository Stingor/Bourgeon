# -*- coding: utf-8 -*-
"""Apparie les methodes virtuelles par (CLASSE, SLOT).

Le nom de classe vient du RTTI : il ne depend d'aucune adresse et survit a la
recompilation. Deux classes de meme nom, de meme nombre de slots, donnent une
correspondance slot a slot.

Garde-fou : si le nombre de slots differe, la classe a gagne ou perdu des
methodes virtuelles et l'alignement n'est plus garanti -- on n'apparie alors que
le PREFIXE commun, et seulement si l'ecart est faible.
"""
import io
import json
import os
from collections import Counter

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"

a = json.load(open(os.path.join(D, 'vtables_2025.json')))['vtables']
b = json.load(open(os.path.join(D, 'vtables_2026.json')))['vtables']
print("classes 2025 / 2026 : %d / %d" % (len(a), len(b)))
common = sorted(set(a) & set(b))
print("classes COMMUNES    : %d" % len(common))
print("  seulement 2025 : %d | seulement 2026 : %d"
      % (len(set(a) - set(b)), len(set(b) - set(a))))

pairs = {}
stats = Counter()
skipped = []
for cls in common:
    la, lb = a[cls], b[cls]
    if len(la) != 1 or len(lb) != 1:
        stats['heritage multiple (ignore)'] += 1
        continue
    x, y = la[0], lb[0]
    na, nb = x['n'], y['n']
    if na == nb:
        mode = 'exact'
        k = na
    else:
        # ecart tolere : <= 10 % et <= 4 slots
        if abs(na - nb) > max(4, min(na, nb) * 0.1):
            stats['nb de slots trop different'] += 1
            skipped.append((cls, na, nb))
            continue
        mode = 'prefixe'
        k = min(na, nb)
    stats[mode] += 1
    for i in range(k):
        s25, s26 = x['slots'][i], y['slots'][i]
        if s25 in pairs and pairs[s25]['to'] != s26:
            pairs[s25]['conflit'] = True
            continue
        pairs[s25] = {"to": s26, "classe": cls, "slot": i, "mode": mode}

conf = sum(1 for v in pairs.values() if v.get('conflit'))
print("\nappariement par (classe, slot) :")
for k, v in stats.most_common():
    print("   %-30s %d" % (k, v))
print("\n  methodes appariees : %d  (dont %d en conflit -> ecartees)" % (len(pairs), conf))
pairs = {k: v for k, v in pairs.items() if not v.get('conflit')}
print("  RETENUES : %d" % len(pairs))

if skipped:
    print("\n  classes ecartees (nb de slots trop different), les 10 premieres :")
    for cls, na, nb in skipped[:10]:
        print("     %-46s %d vs %d" % (cls[:46], na, nb))

json.dump(pairs, io.open(os.path.join(DOCS, 'vtable_pairs.json'), 'w', encoding='utf-8'),
          indent=1, ensure_ascii=False)
print("\necrit : docs/2026/vtable_pairs.json")
