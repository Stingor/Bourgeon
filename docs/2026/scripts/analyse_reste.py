# -*- coding: utf-8 -*-
"""Que reste-t-il a porter, et de quelle NATURE ?

Choisir le prochain vecteur suppose de savoir ce qu'on vise : du code ou des
donnees, et dans quelles zones du binaire.
"""
import io
import json
import os
import re
from collections import Counter

DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"
MAN = r"d:/Mes documents/GitHub/Bourgeon/docs/address_manifest.md"

manifest = {}
row = re.compile(r'^\|\s*`(0x[0-9a-fA-F]+)`\s*\|([^|]*)\|([^|]*)\|([^|]*)\|')
for line in io.open(MAN, encoding='utf-8'):
    m = row.match(line)
    if m:
        sites = [s.strip().strip('`') for s in m.group(3).split('<br>') if s.strip()]
        manifest['0x%08x' % int(m.group(1), 16)] = {
            "names": m.group(2).strip().replace('\u2693', '').strip(),
            "sites": sites, "comment": m.group(4).strip()}

ported = {('0x%08x' % int(k, 16)) for k in json.load(open(os.path.join(DOCS, 'port_2025_2026.json')))}
merged = json.load(io.open(os.path.join(DOCS, 'merged_pairs.json'), encoding='utf-8'))
known = ported | set(merged)
reste = {k: v for k, v in manifest.items() if k not in known}

print("manifeste %d | portees %d | RESTE %d" % (len(manifest), len(known & set(manifest)), len(reste)))

# --- par zone du binaire ------------------------------------------------------
def zone(a):
    x = int(a, 16)
    if x < 0x00E70000:
        return ".text (code)"
    if x < 0x00FC0000:
        return ".rdata"
    if x < 0x01000000:
        return ".idata/autres"
    return ".data / .bss (donnees)"


z = Counter(zone(k) for k in reste)
zp = Counter(zone(k) for k in (known & set(manifest)))
print("\n%-26s %8s %8s" % ("zone", "portees", "RESTE"))
for k in sorted(set(z) | set(zp)):
    print("%-26s %8d %8d" % (k, zp[k], z[k]))

# --- par fichier Bourgeon -----------------------------------------------------
byf = Counter()
for k, v in reste.items():
    for s in v['sites']:
        byf[s.split(':')[0].replace('\\', '/')] += 1
print("\nles 15 fichiers les plus concernes par ce qui RESTE :")
for f, n in byf.most_common(15):
    print("   %-54s %3d" % (f, n))

# --- combien ont un nom parlant ? --------------------------------------------
named = sum(1 for v in reste.values() if v['names'] and v['names'] != '(litt\u00e9ral)')
print("\n  avec un symbole nomme : %d / %d" % (named, len(reste)))
lit = sum(1 for v in reste.values() if '(litt' in v['names'])
print("  litteraux (adresses de patch, pas des symboles) : %d" % lit)

json.dump({k: v for k, v in reste.items()},
          io.open(os.path.join(DOCS, 'reste_a_porter.json'), 'w', encoding='utf-8'),
          indent=1, ensure_ascii=False)
print("\necrit : docs/2026/reste_a_porter.json")
