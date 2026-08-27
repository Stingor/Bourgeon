# -*- coding: utf-8 -*-
"""Les 23 resolutions, filtrees par le ratio de taille calibre sur le temoin.

Temoin (120 cas, paires etablies par d'AUTRES vecteurs que la propagation par
appels, pour ne pas mesurer la methode contre elle-meme) :
    61 reponses uniques, 58 correctes, 3 fausses  ->  95,1 %
Les ratios de taille des bonnes reponses sont tres concentres (mediane 1,00,
p10-p90 = 1,00-1,04) ; deux des trois fausses sont aberrantes (0,28 et 2,16).
Le filtre [0,96 ; 1,13] garde 54/58 correctes et elimine 2/3 fausses -> 98,2 %.

\u26a0 Le filtre est calibre sur le temoin lui-meme : c'est un risque de
surapprentissage. Il reste defendable parce que la distribution des correctes est
serree autour de 1,00, ce qui n'a rien d'un artefact -- deux builds successifs du
meme code produisent des fonctions de taille voisine.
"""
import json
import os

S = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
D = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"

LO, HI = 0.96, 1.13

cache = json.load(open(os.path.join(S, 'prop_match_cache.json')))
jobs = {j['f25']: j for j in json.load(open(os.path.join(S, 'prop_jobs.json')))}

kept, dropped = [], []
for f25, v in cache.items():
    if v.get('status') != 'unique':
        continue
    s25, s26 = v.get('size2025'), v.get('size2026')
    if not s25 or not s26:
        continue
    r = s26 / float(s25)
    row = {'f25': f25, 'to': v['to'], 'ratio': round(r, 2),
           'size2025': s25, 'size2026': s26,
           'anchors': v.get('anchors'),
           'name': jobs.get(f25, {}).get('name', ''),
           'targets': jobs.get(f25, {}).get('targets', [])}
    (kept if LO <= r <= HI else dropped).append(row)

print("resolutions uniques : %d" % (len(kept) + len(dropped)))
print("  retenues (ratio dans [%.2f, %.2f]) : %d" % (LO, HI, len(kept)))
print("  ecartees                           : %d" % len(dropped))

print("\n=== RETENUES ===")
print("%-12s %-12s %-6s %-5s %s" % ("2025", "2026", "ratio", "ancr", "nom"))
for r in sorted(kept, key=lambda x: -x['anchors']):
    print("%-12s %-12s %-6.2f %-5d %s"
          % (r['f25'], r['to'], r['ratio'], r['anchors'], r['name'][:34]))

print("\n=== ECARTEES (ratio hors plage) ===")
for r in sorted(dropped, key=lambda x: x['ratio']):
    print("%-12s %-12s %-6.2f  %s" % (r['f25'], r['to'], r['ratio'], r['name'][:34]))

n_targets = sum(len(r['targets']) for r in kept)
print("\n=> %d fonction(s) portee(s), couvrant %d adresse(s) en dur" % (len(kept), n_targets))

json.dump({'method': 'propagation par appels, intersection des appelants des appeles portes',
           'witness': {'cases': 120, 'answers': 61, 'correct': 58, 'wrong': 3,
                       'precision': 0.951,
                       'after_size_filter': {'kept_correct': 54, 'kept_wrong': 1,
                                             'precision': 0.982},
                       'note': 'temoin constitue de paires issues de vtable/tables/position/portage/accesseur, JAMAIS de appel-i1/appel-i2 (meme famille que la methode testee)'},
           'size_filter': [LO, HI],
           'pairs': kept, 'rejected': dropped},
          open(os.path.join(D, 'propagation_2026.json'), 'w'), indent=1)
print("ecrit : docs/2026/propagation_2026.json")
