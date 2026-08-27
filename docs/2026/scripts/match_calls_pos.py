# -*- coding: utf-8 -*-
"""Propagation par APPELS : apparie les fonctions par leur position d'appel.

Deux fonctions appariees appellent en principe les memes fonctions dans le meme
ordre. Chaque paire connue en engendre donc de nouvelles, et les nouvelles en
engendrent d'autres : c'est une propagation, a iterer jusqu'a epuisement.

Gardes identiques au vecteur des globales : longueurs de sequences egales
exigees, vote a 80 %, une cible pour une seule source. S'y ajoute ici une
verification de coherence : une paire proposee ne doit pas contredire une paire
deja etablie.
"""
import glob
import io
import json
import os
from collections import Counter, defaultdict

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"


def load(tag):
    out = {}
    for f in sorted(glob.glob(os.path.join(D, 'calls*_%s_*.json' % tag))):
        out.update(json.load(open(f)))
    return out


a, b = load('2025'), load('2026')
known = json.load(open(os.path.join(D, 'fnpairs_iter.json')))
print("fonctions avec sequence d'appels : 2025 %d | 2026 %d" % (len(a), len(b)))
print("paires connues au depart          : %d" % len(known))

votes = defaultdict(Counter)
stats = Counter()
for k, seq25 in a.items():
    seq26 = b.get(k)
    if not seq26:
        stats['pas de sequence 2026'] += 1
        continue
    if len(seq25) != len(seq26):
        stats['longueurs differentes'] += 1
        continue
    stats['exploitees'] += 1
    for c25, c26 in zip(seq25, seq26):
        if c25.startswith('[') != c26.startswith('['):
            continue
        votes[c25][c26] += 1

print("\nfonctions temoins :")
for k, v in stats.most_common():
    print("   %-26s %d" % (k, v))

MIN = 0.8
cand, amb, contra = {}, 0, 0
for c25, c in votes.items():
    tot = sum(c.values())
    c26, n = c.most_common(1)[0]
    if n / float(tot) < MIN:
        amb += 1
        continue
    if c25 in known:
        if known[c25].lower() != c26.lower():
            contra += 1
        continue                      # deja connue : rien de neuf
    cand[c25] = {"to": c26, "votes": n, "total": tot}

rev = defaultdict(list)
for k, v in cand.items():
    rev[v['to']].append(k)
final = {k: v for k, v in cand.items() if len(rev[v['to']]) == 1}
# une cible deja prise par une paire connue ne peut pas etre reattribuee
taken = set(v.lower() for v in known.values())
final = {k: v for k, v in final.items() if v['to'].lower() not in taken}

print("\n  candidats (vote >= %.0f%%)          : %d" % (100 * MIN, len(cand)))
print("  vote trop partage (ecarte)        : %d" % amb)
print("  CONTREDISENT une paire connue     : %d" % contra)
print("  collision entre candidats         : %d ecartes" % (len(cand) - len([k for k in cand if len(rev[cand[k]['to']]) == 1])))
print("  cible deja prise                  : %d ecartes"
      % (len([k for k in cand if len(rev[cand[k]['to']]) == 1]) - len(final)))
print("  NOUVELLES paires RETENUES         : %d" % len(final))

byv = Counter(v['total'] for v in final.values())
print("     dont >= 2 temoins : %d" % sum(n for k, n in byv.items() if k >= 2))

json.dump(final, io.open(os.path.join(DOCS, 'call_prop_pairs.json'), 'w', encoding='utf-8'),
          indent=1, ensure_ascii=False)
print("\necrit : docs/2026/call_prop_pairs.json")

# base pour l'iteration suivante
nxt = dict(known)
for k, v in final.items():
    nxt[k] = v['to']
json.dump(nxt, open(os.path.join(D, 'fnpairs_iter.json'), 'w'), indent=1)
print("fnpairs_iter.json : %d -> %d paires" % (len(known), len(nxt)))
