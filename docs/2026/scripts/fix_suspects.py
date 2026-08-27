# -*- coding: utf-8 -*-
"""Les 11 entrees suspectes du portage precedent : que proposent les tables ?

Pour chaque suspecte, on regarde si l'appariement par identifiants stables a une
correspondance pour la meme adresse 2025, et on compare les tailles pour dire
laquelle des deux tient.
"""
import io
import json
import os

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"

susp = json.load(io.open(os.path.join(DOCS, 'port_suspects.json'), encoding='utf-8'))
merged = json.load(io.open(os.path.join(DOCS, 'merged_pairs.json'), encoding='utf-8'))
au25 = json.load(open(os.path.join(D, 'audit_2025.json')))
au26 = json.load(open(os.path.join(D, 'audit_2026.json')))
an25 = json.load(open(os.path.join(D, 'annot_2025_multi.json')))
an26 = json.load(open(os.path.join(D, 'annot_2026_multi.json')))


def size25(a):
    return au25.get(a, {}).get('size', 0) or \
        an25['functions'].get(a, {}).get('size', 0) or \
        an25['globals'].get(a, {}).get('size', 0)


def size26(a):
    return au26.get(a, {}).get('size', 0) or \
        an26['functions'].get(a, {}).get('size', 0) or \
        an26['globals'].get(a, {}).get('size', 0)


def ratio(x, y):
    return min(x, y) / float(max(x, y)) if x and y else None


print("=== LES %d SUSPECTES DU PORTAGE PRECEDENT ===\n" % len(susp))
resolved = 0
for a in sorted(susp):
    v = susp[a]
    s1 = size25(a)
    old = v['to']
    r_old = ratio(s1, size26(old))
    prop = merged.get(a, {}).get('to')
    r_new = ratio(s1, size26(prop)) if prop else None

    print("%s  %s" % (a, (v.get('name25') or '')[:46]))
    print("    source 2025            : 0x%X octets" % s1)
    print("    portage precedent      : %s  0x%X  ratio %s"
          % (old, size26(old), ("%.2f" % r_old) if r_old else "?"))
    if prop:
        verdict = ""
        if r_new and (not r_old or r_new > r_old + 0.2):
            verdict = "   <== MEILLEURE, a retenir"
            resolved += 1
        elif r_new and r_old and abs(r_new - r_old) <= 0.2:
            verdict = "   (equivalentes, ne tranche pas)"
        print("    tables (identifiants)  : %s  0x%X  ratio %s%s"
              % (prop, size26(prop), ("%.2f" % r_new) if r_new else "?", verdict))
    else:
        print("    tables (identifiants)  : aucune correspondance")
    print("    motifs : %s" % " ; ".join(v['motifs']))
    print()

print("=> %d suspectes recoivent une correction mieux etayee" % resolved)
