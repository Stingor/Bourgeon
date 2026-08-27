# -*- coding: utf-8 -*-
"""Apparie les GLOBALES par leur POSITION dans des fonctions deja appariees.

Deux fonctions appariees referencent en principe les memes globales, dans le
meme ordre. Si les deux sequences ont la meme longueur, la i-eme globale de
l'une correspond a la i-eme de l'autre.

C'est le seul vecteur qui atteigne les zones de DONNEES : ni les switches, ni le
RTTI, ni les chaines n'y touchent.

Gardes : on ne conclut que sur un vote LARGEMENT majoritaire, une globale 2026 ne
peut etre reclamee que par une seule globale 2025, et les sequences de longueurs
differentes sont ignorees (une reference ajoutee ou retiree decale tout).
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
    for f in sorted(glob.glob(os.path.join(D, 'seqs_%s_*.json' % tag))):
        out.update(json.load(open(f)))
    return out


a, b = load('2025'), load('2026')
pairs = json.load(open(os.path.join(D, 'fnpairs_all.json')))
print("fonctions avec sequence : 2025 %d | 2026 %d" % (len(a), len(b)))

votes = defaultdict(Counter)
stats = Counter()
for k25, seq25 in a.items():
    # les DEUX fichiers de sequences sont indexes par la cle 2025
    if k25 not in pairs:
        stats['pas de paire'] += 1
        continue
    seq26 = b.get(k25)
    if not seq26:
        stats['pas de sequence cote 2026'] += 1
        continue
    if len(seq25) != len(seq26):
        stats['longueurs differentes (ignore)'] += 1
        continue
    stats['exploitees'] += 1
    for g25, g26 in zip(seq25, seq26):
        votes[g25][g26] += 1

print("\nfonctions :")
for k, v in stats.most_common():
    print("   %-34s %d" % (k, v))
print("\nglobales 2025 avec au moins un vote : %d" % len(votes))

# --- decision -----------------------------------------------------------------
MIN_RATIO = 0.8
cand = {}
amb = 0
for g25, c in votes.items():
    tot = sum(c.values())
    g26, n = c.most_common(1)[0]
    if n / float(tot) >= MIN_RATIO:
        cand[g25] = {"to": g26, "votes": n, "total": tot}
    else:
        amb += 1

# collision : une cible pour une seule source
rev = defaultdict(list)
for g25, v in cand.items():
    rev[v['to']].append(g25)
coll = {t: s for t, s in rev.items() if len(s) > 1}
final = {g: v for g, v in cand.items() if len(rev[v['to']]) == 1}

print("  vote majoritaire (>= %.0f%%)      : %d" % (100 * MIN_RATIO, len(cand)))
print("  vote trop partage (ecarte)      : %d" % amb)
print("  collisions (cible partagee)     : %d cibles, %d sources ecartees"
      % (len(coll), sum(len(s) for s in coll.values())))
print("  RETENUES                        : %d" % len(final))

byv = Counter(v['total'] for v in final.values())
print("\n  par nombre de fonctions temoins :")
for k in sorted(byv, reverse=True)[:8]:
    print("     %2d temoin(s) : %4d" % (k, byv[k]))
print("     >= 2 temoins : %d" % sum(v for k, v in byv.items() if k >= 2))

json.dump(final, io.open(os.path.join(DOCS, 'global_pos_pairs.json'), 'w', encoding='utf-8'),
          indent=1, ensure_ascii=False)
print("\necrit : docs/2026/global_pos_pairs.json")
