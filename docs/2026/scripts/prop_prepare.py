# -*- coding: utf-8 -*-
"""Prepare la signature d'appeles portes pour chaque fonction a porter.

Principe : si la fonction 2025 F appelle {a, b, c} et que ces trois sont deja
portes vers {a', b', c'}, alors la fonction 2026 cherchee appelle {a', b', c'}.
Elle se trouve donc dans l'INTERSECTION des appelants de a', b' et c' -- un
ensemble petit, qu'on peut interroger sans balayer le binaire.

Plus il y a d'appeles portes, plus la signature discrimine. On trie donc par ce
nombre : les fonctions a une seule ancre seront ambigues et sont traitees en
dernier, si tant est qu'elles le soient.
"""
import json
import os

D = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"
S = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

port = {}
for c in ('all_pairs_final.json', 'merged_pairs.json', 'global_pos_pairs.json',
          'port_2025_2026.json', 'vtable_pairs.json'):
    p = os.path.join(D, c)
    if not os.path.exists(p):
        continue
    try:
        d = json.load(open(p))
    except Exception:
        continue
    if not isinstance(d, dict):
        continue
    for k, v in d.items():
        t = v.get('to') if isinstance(v, dict) else (v if isinstance(v, str) else None)
        if t:
            try:
                port.setdefault(int(k, 16), int(t, 16))
            except ValueError:
                pass

rel = json.load(open(os.path.join(S, 'prop_2025.json')))
groups = json.load(open(os.path.join(S, 'prop_groups.json')))

jobs = []
for fn in groups['todo']:
    # une cible quelconque de ce groupe porte les infos de la fonction
    info = None
    for a, v in rel.items():
        if v.get('kind') != 'data' and v.get('func') == fn:
            info = v
            break
    if info is None:
        continue
    anchors = []
    for c in info.get('callees', []):
        ci = int(c, 16)
        if ci in port:
            anchors.append('0x%08x' % port[ci])
    jobs.append({
        'f25': fn,
        'name': info.get('name', ''),
        'size': info.get('size', 0),
        'retn': info.get('retn'),
        'anchors': anchors,
        'n_callees': len(info.get('callees', [])),
        'targets': groups['todo'][fn],
    })

jobs.sort(key=lambda j: -len(j['anchors']))
n_multi = sum(1 for j in jobs if len(j['anchors']) >= 2)
n_one = sum(1 for j in jobs if len(j['anchors']) == 1)
n_zero = sum(1 for j in jobs if not j['anchors'])

print("%d fonction(s) a porter" % len(jobs))
print("   >= 2 ancres (signature discriminante) : %d" % n_multi)
print("   1 ancre  (ambigu, a verifier)         : %d" % n_one)
print("   0 ancre  (aucune prise)               : %d" % n_zero)

json.dump(jobs, open(os.path.join(S, 'prop_jobs.json'), 'w'), indent=0)
print("\necrit : prop_jobs.json")
