# -*- coding: utf-8 -*-
"""Controle de MONOTONIE : troisieme test, independant des tailles et des noms.

Le compilateur reordonne des fonctions d'un build a l'autre, mais pas au hasard :
l'ordre est largement preserve. Une paire dont la cible casse l'ordre par rapport
a ses VOISINES immediates est suspecte.

Mesure : pour chaque paire triee par adresse 2025, on regarde si l'adresse 2026
est comprise entre celles de ses voisines. Comparaison avec un temoin aleatoire,
sans quoi le taux d'accord ne voudrait rien dire.
"""
import io
import json
import os
import random

DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"
merged = json.load(io.open(os.path.join(DOCS, 'all_pairs_final.json'), encoding='utf-8'))

pts = sorted(((int(k, 16), int(v['to'], 16), k, v) for k, v in merged.items()))
print("paires analysees : %d" % len(pts))


def inversions(seq):
    """proportion de voisins consecutifs dont l'ordre est INVERSE"""
    bad = 0
    for i in range(len(seq) - 1):
        if seq[i + 1] < seq[i]:
            bad += 1
    return bad / float(len(seq) - 1)


ys = [p[1] for p in pts]
inv_real = inversions(ys)

random.seed(7)
inv_ctrl = []
for _ in range(20):
    sh = ys[:]
    random.shuffle(sh)
    inv_ctrl.append(inversions(sh))
inv_ctrl = sum(inv_ctrl) / len(inv_ctrl)

print("\n=== MONOTONIE GLOBALE ===")
print("  inversions entre voisins, paires proposees : %.1f%%" % (100 * inv_real))
print("  inversions, TEMOIN aleatoire               : %.1f%%" % (100 * inv_ctrl))

# --- paires qui cassent l'ordre LOCAL ---------------------------------------
suspects = []
W = 4
for i in range(W, len(pts) - W):
    x, y, k, v = pts[i]
    voisins = [pts[j][1] for j in range(i - W, i + W + 1) if j != i]
    lo, hi = min(voisins), max(voisins)
    # la cible doit tomber dans l'enveloppe de ses voisines, avec une marge large
    span = hi - lo
    if span and not (lo - span <= y <= hi + span):
        suspects.append((k, v['to'], v.get('manifeste', ''), v.get('src', ''),
                         lo, hi, y))

print("\n=== PAIRES QUI CASSENT L'ORDRE LOCAL ===")
print("  fenetre glissante de %d voisines de chaque cote" % W)
print("  suspectes : %d / %d (%.1f%%)" % (len(suspects), len(pts),
                                          100.0 * len(suspects) / len(pts)))
inman = [s for s in suspects if s[2]]
print("  dont utilisees par Bourgeon (dans le manifeste) : %d" % len(inman))
for s in inman[:20]:
    print("     %s -> %s  [voisines %08x..%08x]  %s  (%s)"
          % (s[0], s[1], s[4], s[5], s[2][:38], s[3]))

json.dump([{"2025": s[0], "2026": s[1], "manifeste": s[2], "src": s[3]}
           for s in suspects],
          io.open(os.path.join(DOCS, 'monotonic_outliers.json'), 'w', encoding='utf-8'),
          indent=1, ensure_ascii=False)
print("\necrit : docs/2026/monotonic_outliers.json")
