# -*- coding: utf-8 -*-
"""Confronte l'appariement par co-occurrence d'opcodes au portage deja etabli.

Les deux methodes sont INDEPENDANTES : le portage existant vient du RTTI, des
chaines referencees et de la propagation par appels ; celui-ci ne connait que
les opcodes du switch de dispatch. Un recoupement est donc un vrai test, pas une
verification circulaire.
"""
import json
import os

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
REF = r"d:/Mes documents/GitHub/Bourgeon/docs/2026/port_2025_2026.json"

ref = json.load(open(REF))
new = json.load(open(os.path.join(D, 'opcode_pairs_d1.json')))


def norm(x):
    return '0x%08x' % int(x, 16)


ref_map = {norm(k): norm(v['to']) for k, v in ref.items()}

for kind in ('functions', 'globals'):
    pairs = new[kind]
    agree = disagree = 0
    bad = []
    for src, rec in pairs.items():
        if src.startswith('['):
            continue
        s = norm(src)
        if s not in ref_map:
            continue
        if ref_map[s] == norm(rec['to']):
            agree += 1
        else:
            disagree += 1
            bad.append((s, ref_map[s], norm(rec['to']), rec['nop']))

    overlap = agree + disagree
    print("=== %s ===" % kind)
    print("  paires proposees        : %d" % len(pairs))
    print("  couvertes par le portage: %d" % overlap)
    if overlap:
        print("  CONCORDENT              : %d" % agree)
        print("  DIVERGENT               : %d" % disagree)
        for s, r, n, nop in bad[:15]:
            print("     %s : portage=%s  opcodes=%s (%d opcodes)" % (s, r, n, nop))
    print("  apport NOUVEAU (hors portage) : %d" % (len(pairs) - overlap))
    print()

# combien d'adresses du portage sont des fonctions du dispatch ?
print("adresses dans le portage existant : %d" % len(ref_map))
