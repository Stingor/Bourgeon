# -*- coding: utf-8 -*-
"""Combien d'adresses 20250716 sont codees EN DUR dans Bourgeon, et ou ?

Faire demarrer Bourgeon sur le client 2026 ne suffit pas : la configuration YAML
ne porte que 22 champs, alors que le reste du projet appelle des adresses en dur.
Chacune pointe du code 2025 ; dans un binaire 2026 elle designe autre chose.

On ne compte pas n'importe quel litteral hexadecimal : seuls ceux qui tombent
dans le .text du client 2025 (0x00401000 .. 0x00E7A60A) sont des adresses de
code. Un offset de structure (0x1F4) ou une couleur (0xFF00FF) n'en est pas une.
"""
import io
import os
import re
from collections import Counter

SRC = r"d:/Mes documents/GitHub/Bourgeon/src"
LO, HI = 0x00401000, 0x00E7A60A          # .text du client 2025

pat = re.compile(r'0[xX]([0-9a-fA-F]{6,8})\b')
per_file = Counter()
uniq = set()
per_dir = Counter()

for root, dirs, files in os.walk(SRC):
    for fn in files:
        if not fn.endswith(('.cc', '.h', '.cpp', '.hpp')):
            continue
        p = os.path.join(root, fn)
        try:
            s = io.open(p, encoding='utf-8', errors='replace').read()
        except Exception:
            continue
        # neutraliser les commentaires : une adresse citee n'est pas une adresse utilisee
        s = re.sub(r'//[^\n]*', '', s)
        s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
        n = 0
        for m in pat.finditer(s):
            v = int(m.group(1), 16)
            if LO <= v <= HI:
                uniq.add(v)
                n += 1
        if n:
            rel = os.path.relpath(p, SRC).replace('\\', '/')
            per_file[rel] = n
            per_dir[rel.split('/')[0] + ('/' + rel.split('/')[1] if '/' in rel else '')] += n

print("adresses de code 2025 en dur (hors commentaires)")
print("  occurrences : %d" % sum(per_file.values()))
print("  distinctes  : %d" % len(uniq))
print("  fichiers    : %d" % len(per_file))

print("\npar zone :")
for d, n in per_dir.most_common(12):
    print("   %-34s %5d" % (d, n))

print("\nles 12 fichiers les plus charges :")
for f, n in per_file.most_common(12):
    print("   %-46s %5d" % (f, n))
