# -*- coding: utf-8 -*-
"""Fusion finale des trois jeux d'appariement + bilan manifeste.

Chaque recouvrement entre deux jeux est un TEST : ils viennent de tables
differentes, donc un desaccord signale une erreur.
"""
import io
import json
import os
import re
from collections import defaultdict

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"
MAN = r"d:/Mes documents/GitHub/Bourgeon/docs/address_manifest.md"


def norm(x):
    return '0x%08x' % int(x[1:-1] if x.startswith('[') else x, 16)


def load(fn, label, nkey):
    d = json.load(open(os.path.join(D, fn)))
    out = {}
    for kind in ('functions', 'globals'):
        for s, r in d[kind].items():
            out[norm(s)] = {"to": norm(r['to']), "kind": kind,
                            "n": r.get(nkey, 1),
                            "tables": r.get('tables', [label])}
    return out


jeux = [
    (load('opcode_pairs_d1.json', 'opcode', 'nop'), 'opcode'),
    (load('window_pairs.json', 'fenetre', 'nid'), 'fenetre'),
    (load('multi_pairs.json', 'multi', 'n'), 'multi'),
    (load('all_pairs.json', 'tables217', 'n'), 'tables217'),
]

merged = {}
conflicts = []
for jeu, label in jeux:
    for s, r in jeu.items():
        if s in merged:
            if merged[s]['to'] != r['to']:
                conflicts.append((s, merged[s]['to'], r['to'], merged[s]['src'], label))
            else:
                merged[s]['src'] = merged[s]['src'] + '+' + label
                merged[s]['tables'] = sorted(set(merged[s]['tables']) | set(r['tables']))
        else:
            merged[s] = {"to": r['to'], "kind": r['kind'], "n": r['n'],
                         "tables": r['tables'], "src": label}

print("=== FUSION DES TROIS JEUX ===")
for jeu, label in jeux:
    print("  %-9s : %d paires" % (label, len(jeu)))
print("  UNION     : %d paires" % len(merged))
confirmed = sum(1 for v in merged.values() if '+' in v['src'])
print("  confirmees par >= 2 jeux : %d" % confirmed)
print("  CONFLITS  : %d" % len(conflicts))
for c in conflicts[:15]:
    print("     !! %s : %s (%s) vs %s (%s)" % (c[0], c[1], c[3], c[2], c[4]))

# --- bilan manifeste ---------------------------------------------------------
manifest = {}
row = re.compile(r'^\|\s*`(0x[0-9a-fA-F]+)`\s*\|([^|]*)\|([^|]*)\|([^|]*)\|')
for line in io.open(MAN, encoding='utf-8'):
    m = row.match(line)
    if m:
        manifest['0x%08x' % int(m.group(1), 16)] = {
            "names": m.group(2).strip().replace('\u2693', '').strip(),
            "comment": m.group(4).strip()}
ported = {('0x%08x' % int(k, 16)) for k in json.load(open(os.path.join(DOCS, 'port_2025_2026.json')))}

gain = {k: v for k, v in merged.items() if k in manifest and k not in ported}
print("\n=== BILAN MANIFESTE ===")
print("  adresses du manifeste     : %d" % len(manifest))
print("  portees avant             : %d (%.1f%%)" % (len(ported), 100.0 * len(ported) / len(manifest)))
print("  NOUVELLES                 : %d" % len(gain))
print("  TOTAL                     : %d (%.1f%%)"
      % (len(ported) + len(gain), 100.0 * (len(ported) + len(gain)) / len(manifest)))

for k, v in merged.items():
    if k in manifest:
        v['manifeste'] = manifest[k]['names']
        if manifest[k]['comment']:
            v['comment'] = manifest[k]['comment']
        v['nouveau_pour_bourgeon'] = k not in ported

json.dump(merged, io.open(os.path.join(DOCS, 'merged_pairs.json'), 'w', encoding='utf-8'),
          indent=1, ensure_ascii=False)
print("\necrit : docs/2026/merged_pairs.json")

print("\n  les %d adresses gagnees :" % len(gain))
for k in sorted(gain, key=lambda x: merged[x].get('manifeste', '')):
    v = merged[k]
    print("    [%-16s] %s -> %s  %s" % (v['src'], k, v['to'], v.get('manifeste', '')[:46]))
