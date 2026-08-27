# -*- coding: utf-8 -*-
"""Combien des 430 adresses en dur de Bourgeon sont DEJA portees ?

C'est la question qui decide de la suite : si le portage existant en couvre une
bonne part, le chantier devient tractable ; sinon il faut le prendre autrement.

On croise les adresses relevees dans le code source avec les paires deja
etablies (8645 dans all_pairs_final.json et consorts), et on classe le reste par
fichier -- c'est la ou il faudra chercher.
"""
import io
import json
import os
import re
from collections import Counter, defaultdict

SRC = r"d:/Mes documents/GitHub/Bourgeon/src"
D = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"
LO, HI = 0x00401000, 0x00E7A60A

# --- 1. les adresses en dur, avec leur provenance ----------------------------
pat = re.compile(r'0[xX]([0-9a-fA-F]{6,8})\b')
where = defaultdict(set)
for root, dirs, files in os.walk(SRC):
    for fn in files:
        if not fn.endswith(('.cc', '.h', '.cpp', '.hpp')):
            continue
        p = os.path.join(root, fn)
        try:
            s = io.open(p, encoding='utf-8', errors='replace').read()
        except Exception:
            continue
        s = re.sub(r'//[^\n]*', '', s)
        s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
        rel = os.path.relpath(p, SRC).replace('\\', '/')
        # configuration.h porte les adresses des DEUX clients, gerees par YAML :
        # ce n'est pas du code en dur, et ses valeurs 2026 seraient comptees
        # a tort comme des adresses 2025 non portees.
        if rel == 'ragnarok/configuration.h':
            continue
        for m in pat.finditer(s):
            v = int(m.group(1), 16)
            if LO <= v <= HI:
                where[v].add(rel)

print("adresses distinctes en dur : %d" % len(where))

# --- 2. le portage existant --------------------------------------------------
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
print("paires de portage chargees : %d" % len(port))

# --- 3. croisement -----------------------------------------------------------
known = {a for a in where if a in port}
unknown = {a for a in where if a not in port}
print("\n=== COUVERTURE ===")
print("  deja portees : %4d  (%.1f %%)" % (len(known), 100.0 * len(known) / len(where)))
print("  a trouver    : %4d  (%.1f %%)" % (len(unknown), 100.0 * len(unknown) / len(where)))

# ou manque-t-il le plus ?
per_file = Counter()
for a in unknown:
    for f in where[a]:
        per_file[f] += 1
print("\nfichiers ou il manque le plus d'adresses :")
for f, n in per_file.most_common(15):
    print("   %-46s %4d" % (f, n))

json.dump({'known': ['0x%08x' % a for a in sorted(known)],
           'unknown': {'0x%08x' % a: sorted(where[a]) for a in sorted(unknown)}},
          open(os.path.join(D, 'hardcoded_coverage.json'), 'w'), indent=1)
print("\necrit : docs/2026/hardcoded_coverage.json")
