# -*- coding: utf-8 -*-
"""Regrouper les 185 cibles par FONCTION contenante.

Une adresse au milieu d'une fonction ne se porte pas toute seule : elle se porte
avec sa fonction, puis on y retrouve l'instruction equivalente. Et plusieurs
cibles partagent souvent la meme fonction -- six d'entre elles tombent dans
sub_7F8810.

Le vrai compte n'est donc pas 185 adresses mais N fonctions, dont une partie est
DEJA portee : pour celles-la il ne reste qu'a localiser l'offset.
"""
import json
import os
from collections import defaultdict

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

groups = defaultdict(list)
data_only = []
for a, v in rel.items():
    if v.get('kind') == 'data':
        data_only.append(a)
        continue
    groups[v['func']].append((a, v))

print("185 cibles  ->  %d fonction(s) contenante(s)  (+ %d donnees)"
      % (len(groups), len(data_only)))

ported, todo = {}, {}
for fn, items in groups.items():
    fi = int(fn, 16)
    if fi in port:
        ported[fn] = (('0x%08x' % port[fi]), items)
    else:
        todo[fn] = items

n_cibles_ported = sum(len(v[1]) for v in ported.values())
n_cibles_todo = sum(len(v) for v in todo.values())

print("\n=== la fonction contenante est-elle portee ? ===")
print("  DEJA PORTEE : %3d fonction(s)  ->  %3d cible(s) : reste a localiser l'offset"
      % (len(ported), n_cibles_ported))
print("  A PORTER    : %3d fonction(s)  ->  %3d cible(s)"
      % (len(todo), n_cibles_todo))

print("\nfonctions portees portant le plus de cibles :")
for fn, (f26, items) in sorted(ported.items(), key=lambda kv: -len(kv[1][1]))[:12]:
    nm = items[0][1].get('name', '')
    print("   %s -> %s  %-30s %2d cible(s)" % (fn, f26, nm[:30], len(items)))

print("\nfonctions NON portees portant le plus de cibles :")
for fn, items in sorted(todo.items(), key=lambda kv: -len(kv[1]))[:12]:
    nm = items[0][1].get('name', '')
    ncal = len(items[0][1].get('callees', []))
    print("   %s  %-32s %2d cible(s)  %2d appele(s)"
          % (fn, nm[:32], len(items), ncal))

json.dump({'ported': {k: v[0] for k, v in ported.items()},
           'ported_targets': {k: [a for a, _ in v[1]] for k, v in ported.items()},
           'todo': {k: [a for a, _ in v] for k, v in todo.items()},
           'data': data_only},
          open(os.path.join(S, 'prop_groups.json'), 'w'), indent=0)
print("\necrit : prop_groups.json")
