# -*- coding: utf-8 -*-
"""Valide l'appariement par co-occurrence d'opcodes, DEUX tests independants.

1. NOMS  : deux fonctions appariees portant le meme nom dans les deux IDB.
2. TAILLES : la distribution du ratio de tailles des paires proposees, comparee
   a celle d'un appariement ALEATOIRE des memes adresses (temoin negatif).
   Sans ce temoin, un ratio "plausible" ne prouverait rien.
"""
import json
import os
import random

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

pairs = json.load(open(os.path.join(D, 'opcode_pairs_d1.json')))
a25 = json.load(open(os.path.join(D, 'annot_2025_d1.json')))
a26 = json.load(open(os.path.join(D, 'annot_2026_d1.json')))


def norm(x):
    if x.startswith('['):
        x = x[1:-1]
    return '0x%08x' % int(x, 16)


# ---------------------------------------------------------------- test 1 : noms
same = diff = 0
examples, mismatches = [], []
for src, rec in pairs['functions'].items():
    n25 = a25['functions'].get(norm(src), {}).get('name', '')
    n26 = a26['functions'].get(norm(rec['to']), {}).get('name', '')
    if not n25 or not n26:
        continue
    g25 = n25.startswith('sub_')
    g26 = n26.startswith('sub_')
    if g25 or g26:
        continue
    if n25 == n26:
        same += 1
        examples.append((src, rec['to'], n25, rec['nop']))
    else:
        diff += 1
        mismatches.append((src, rec['to'], n25, n26, rec['nop']))

print("=== TEST 1 : noms presents des DEUX cotes ===")
print("  comparables      : %d" % (same + diff))
print("  MEME nom         : %d" % same)
print("  noms DIFFERENTS  : %d" % diff)
for s, t, n, k in examples[:12]:
    print("    OK  %s -> %s  %s  (%d opcodes)" % (s, t, n, k))
for s, t, x, y, k in mismatches[:12]:
    print("    !!  %s -> %s  %s  vs  %s  (%d opcodes)" % (s, t, x, y, k))

# ------------------------------------------------------------- test 2 : tailles
def ratio(sz1, sz2):
    if not sz1 or not sz2:
        return None
    return float(min(sz1, sz2)) / max(sz1, sz2)


real = []
srcs, tgts = [], []
for src, rec in pairs['functions'].items():
    s1 = a25['functions'].get(norm(src), {}).get('size', 0)
    s2 = a26['functions'].get(norm(rec['to']), {}).get('size', 0)
    r = ratio(s1, s2)
    if r is not None:
        real.append(r)
        srcs.append(s1)
        tgts.append(s2)

random.seed(12345)
ctrl = []
for _ in range(20):
    shuffled = tgts[:]
    random.shuffle(shuffled)
    for s1, s2 in zip(srcs, shuffled):
        r = ratio(s1, s2)
        if r is not None:
            ctrl.append(r)


def summary(v, label):
    if not v:
        print("  %s : vide" % label)
        return
    v = sorted(v)
    n = len(v)
    med = v[n // 2]
    good = sum(1 for x in v if x > 0.8) / float(n)
    ok = sum(1 for x in v if x > 0.6) / float(n)
    print("  %-22s n=%-6d mediane=%.3f   >0.8 : %5.1f%%   >0.6 : %5.1f%%"
          % (label, n, med, 100 * good, 100 * ok))


print("\n=== TEST 2 : ratio de tailles (min/max) ===")
summary(real, "paires proposees")
summary(ctrl, "TEMOIN aleatoire")

# ---------------------------------------------------- qualite par nb d'opcodes
print("\n=== ratio de tailles selon la force de la signature ===")
for lo, hi, label in ((3, 999, ">= 3 opcodes"), (2, 2, "2 opcodes"), (1, 1, "1 opcode")):
    sub = []
    for src, rec in pairs['functions'].items():
        if not (lo <= rec['nop'] <= hi):
            continue
        s1 = a25['functions'].get(norm(src), {}).get('size', 0)
        s2 = a26['functions'].get(norm(rec['to']), {}).get('size', 0)
        r = ratio(s1, s2)
        if r is not None:
            sub.append(r)
    summary(sub, label)
