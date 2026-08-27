# -*- coding: utf-8 -*-
"""Combien des 185 cibles ont une ancre dans le portage existant ?

Une cible est atteignable si l'un de ses appelants ou de ses appeles est deja
porte : l'equivalent 2026 de cette ancre donne un point d'entree, et les
candidats se reduisent a ce qu'elle appelle.

On classe avant de lancer quoi que ce soit sur le pont IDA : inutile d'y envoyer
des cibles qui n'ont aucune prise.
"""
import json
import os
from collections import Counter

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
print("%d cibles, %d paires" % (len(rel), len(port)))

kinds = Counter(v.get('kind') for v in rel.values())
print("nature :", dict(kinds))

anchored_by_caller = {}
anchored_by_callee = {}
orphans = []
for a, v in rel.items():
    if v.get('kind') == 'data':
        continue
    # un appelant porte : on connait la fonction 2026 qui appelle la cible
    cal = []
    for c in v.get('callers', []):
        ci = int(c, 16)
        # l appelant est une ADRESSE D APPEL : remonter a sa fonction n'est pas
        # possible ici, on teste donc l adresse telle quelle ET son voisinage
        if ci in port:
            cal.append((c, '0x%08x' % port[ci]))
    cee = [(c, '0x%08x' % port[int(c, 16)]) for c in v.get('callees', [])
           if int(c, 16) in port]
    if cal:
        anchored_by_caller[a] = cal
    if cee:
        anchored_by_callee[a] = cee
    if not cal and not cee:
        orphans.append(a)

both = set(anchored_by_caller) | set(anchored_by_callee)
print("\n=== prise sur le portage existant ===")
print("  ancrees par un APPELE  : %d" % len(anchored_by_callee))
print("  ancrees par un APPELANT: %d" % len(anchored_by_caller))
print("  au moins une ancre     : %d" % len(both))
print("  sans aucune prise      : %d" % len(orphans))

# les mieux ancrees d'abord : plusieurs appeles portes = signature forte
rank = sorted(anchored_by_callee.items(), key=lambda kv: -len(kv[1]))
print("\ncibles les mieux ancrees (nombre d'appeles deja portes) :")
for a, lst in rank[:15]:
    print("   %s  %-28s %2d appele(s) porte(s)"
          % (a, rel[a].get('name', '')[:28], len(lst)))

json.dump({'by_callee': anchored_by_callee, 'by_caller': anchored_by_caller,
           'orphans': orphans},
          open(os.path.join(S, 'prop_triage.json'), 'w'), indent=0)
print("\necrit : prop_triage.json")
