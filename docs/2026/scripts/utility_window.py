# -*- coding: utf-8 -*-
"""Mesure ce que l'appariement par opcodes apporte VRAIMENT a Bourgeon.

Une paire n'a de valeur que si l'adresse 2025 figure dans le manifeste, c'est-a-
dire si Bourgeon l'utilise reellement. Sinon c'est du RE, pas du portage.
"""
import io
import json
import os
import re

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
MAN = r"d:/Mes documents/GitHub/Bourgeon/docs/address_manifest.md"
REF = r"d:/Mes documents/GitHub/Bourgeon/docs/2026/port_2025_2026.json"

# --- le manifeste : adresse -> noms + commentaire -----------------------------
manifest = {}
row = re.compile(r'^\|\s*`(0x[0-9a-fA-F]+)`\s*\|([^|]*)\|([^|]*)\|([^|]*)\|')
for line in io.open(MAN, encoding='utf-8'):
    m = row.match(line)
    if m:
        manifest['0x%08x' % int(m.group(1), 16)] = {
            "names": m.group(2).strip(),
            "sites": m.group(3).strip(),
            "comment": m.group(4).strip(),
        }
print("manifeste : %d adresses" % len(manifest))

ported = {('0x%08x' % int(k, 16)) for k in json.load(open(REF))}
print("deja portees : %d" % len(ported))
todo = set(manifest) - ported
print("restantes    : %d" % len(todo))

pairs = json.load(open(os.path.join(D, 'window_pairs.json')))


def norm(x):
    if x.startswith('['):
        x = x[1:-1]
    return '0x%08x' % int(x, 16)


gain = {}
already = 0
for kind in ('functions', 'globals'):
    for src, rec in pairs[kind].items():
        s = norm(src)
        if s in ported:
            already += 1
            continue
        if s in todo:
            gain[s] = {"to": norm(rec['to']), "kind": kind, "nop": rec['nid'],
                       "ids": rec['ids'][:6],
                       "names": manifest[s]['names'],
                       "comment": manifest[s]['comment'],
                       "sites": manifest[s]['sites']}

print("\n=== APPORT REEL AU PORTAGE ===")
print("  paires proposees au total     : %d" % (len(pairs['functions']) + len(pairs['globals'])))
print("  deja portees (recoupement)    : %d" % already)
print("  NOUVELLES adresses du manifeste: %d" % len(gain))
print("  -> portage : %d/%d  (%.1f%%) au lieu de %d/%d (%.1f%%)"
      % (len(ported) + len(gain), len(manifest),
         100.0 * (len(ported) + len(gain)) / len(manifest),
         len(ported), len(manifest), 100.0 * len(ported) / len(manifest)))

if gain:
    print("\n  les nouvelles adresses portees :")
    for s in sorted(gain):
        g = gain[s]
        nm = g['names'][:46]
        print("    %s -> %s  %-46s %s" % (s, g['to'], nm, g['comment'][:40]))

json.dump(gain, open(os.path.join(D, 'manifest_gain_win.json'), 'w'), indent=1)
print("\necrit : manifest_gain.json")
