# -*- coding: utf-8 -*-
"""Ce que les globales appariees par POSITION apportent au manifeste."""
import io
import json
import os
import re
from collections import Counter

DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"
MAN = r"d:/Mes documents/GitHub/Bourgeon/docs/address_manifest.md"

gp = json.load(io.open(os.path.join(DOCS, 'call_prop_pairs.json'), encoding='utf-8'))
mg = json.load(io.open(os.path.join(DOCS, 'merged_pairs.json'), encoding='utf-8'))
vt = json.load(io.open(os.path.join(DOCS, 'vtable_pairs.json'), encoding='utf-8'))
ported = {('0x%08x' % int(k, 16)) for k in json.load(open(os.path.join(DOCS, 'port_2025_2026.json')))}
gpo = json.load(io.open(os.path.join(DOCS, 'global_pos_pairs.json'), encoding='utf-8'))
known = ported | set(mg) | set(vt) | set(gpo)

manifest = {}
row = re.compile(r'^\|\s*`(0x[0-9a-fA-F]+)`\s*\|([^|]*)\|([^|]*)\|([^|]*)\|')
for line in io.open(MAN, encoding='utf-8'):
    m = row.match(line)
    if m:
        manifest['0x%08x' % int(m.group(1), 16)] = {
            "names": m.group(2).strip().replace('\u2693', '').strip(),
            "comment": m.group(4).strip(),
            "sites": [s.strip().strip('`') for s in m.group(3).split('<br>') if s.strip()]}

gain = {k: v for k, v in gp.items() if k in manifest and k not in known}
print("=== APPORT DE LA PROPAGATION PAR APPELS ===")
print("  paires proposees  : %d" % len(gp))
print("  dans le manifeste : %d" % len([k for k in gp if k in manifest]))
print("  NOUVELLES         : %d" % len(gain))
tot = len(known & set(manifest))
print("  -> portage %d/784 (%.1f%%) au lieu de %d/784 (%.1f%%)"
      % (tot + len(gain), 100.0 * (tot + len(gain)) / 784, tot, 100.0 * tot / 784))

byf = Counter()
for k in gain:
    for s in manifest[k]['sites']:
        byf[s.split(':')[0].replace(chr(92), '/')] += 1
print("\n  fichiers Bourgeon debloques :")
for f, n in byf.most_common(12):
    print("     %-52s %3d" % (f, n))

print("\n  echantillon (les mieux votes) :")
for k in sorted(gain, key=lambda x: -gain[x]['total'])[:18]:
    v = gain[k]
    print("     %s -> %s  %-38s (%d/%d temoins)"
          % (k, v['to'], manifest[k]['names'][:38], v['votes'], v['total']))

json.dump(gain, io.open(os.path.join(DOCS, 'call_prop_gain.json'), 'w', encoding='utf-8'),
          indent=1, ensure_ascii=False)
print("\necrit : docs/2026/global_pos_gain.json")
