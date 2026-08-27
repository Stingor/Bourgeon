# -*- coding: utf-8 -*-
"""Fusion de TOUS les vecteurs, avec detection de conflits entre eux."""
import io
import json
import os
import re
from collections import Counter

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"
MAN = r"d:/Mes documents/GitHub/Bourgeon/docs/address_manifest.md"


def n(x):
    return '0x%08x' % int(x[1:-1] if x.startswith('[') else x, 16)


sources = []
ref = json.load(open(os.path.join(DOCS, 'port_2025_2026.json')))
sources.append(('portage', {n(k): n(v['to']) for k, v in ref.items()}))
mg = json.load(io.open(os.path.join(DOCS, 'merged_pairs.json'), encoding='utf-8'))
sources.append(('tables', {n(k): n(v['to']) for k, v in mg.items()}))
vt = json.load(io.open(os.path.join(DOCS, 'vtable_pairs.json'), encoding='utf-8'))
sources.append(('vtable', {n(k): n(v['to']) for k, v in vt.items()}))
gp = json.load(io.open(os.path.join(DOCS, 'global_pos_pairs.json'), encoding='utf-8'))
sources.append(('position', {n(k): n(v['to']) for k, v in gp.items()}))
ac = json.load(io.open(os.path.join(DOCS, 'accessor_pairs.json'), encoding='utf-8'))
sources.append(('accesseur', {n(p['2025']): n(p['2026']) for p in ac}))

final, srcs, conflicts = {}, {}, []
for label, d in sources:
    for k, v in d.items():
        if k in final and final[k] != v:
            conflicts.append((k, final[k], v, ','.join(sorted(srcs[k])), label))
            continue
        final[k] = v
        srcs.setdefault(k, set()).add(label)

print("=== FUSION DE TOUS LES VECTEURS ===")
for label, d in sources:
    print("  %-10s : %5d paires" % (label, len(d)))
print("  UNION      : %5d" % len(final))
print("  CONFLITS   : %5d" % len(conflicts))
for c in conflicts[:8]:
    print("     !! %s : %s (%s) vs %s (%s)" % c)

c = Counter(len(v) for v in srcs.values())
print("\n  confirmees par n vecteurs :")
for k in sorted(c, reverse=True):
    print("     %d vecteur(s) : %d" % (k, c[k]))

# --- manifeste ----------------------------------------------------------------
manifest = {}
row = re.compile(r'^\|\s*`(0x[0-9a-fA-F]+)`\s*\|([^|]*)\|([^|]*)\|([^|]*)\|')
for line in io.open(MAN, encoding='utf-8'):
    m = row.match(line)
    if m:
        manifest['0x%08x' % int(m.group(1), 16)] = {
            "names": m.group(2).strip().replace('\u2693', '').strip(),
            "comment": m.group(4).strip(),
            "sites": [s.strip().strip('`') for s in m.group(3).split('<br>') if s.strip()]}

couv = set(manifest) & set(final)
print("\n=== MANIFESTE ===")
print("  adresses          : %d" % len(manifest))
print("  PORTEES           : %d (%.1f%%)" % (len(couv), 100.0 * len(couv) / len(manifest)))
print("  restantes         : %d" % (len(manifest) - len(couv)))

out = {}
for k, v in final.items():
    e = {"to": v, "via": "+".join(sorted(srcs[k]))}
    if k in manifest:
        e["manifeste"] = manifest[k]['names']
        if manifest[k]['comment']:
            e["comment"] = manifest[k]['comment']
    out[k] = e
json.dump(out, io.open(os.path.join(DOCS, 'all_pairs_final.json'), 'w', encoding='utf-8'),
          indent=1, ensure_ascii=False)
print("\necrit : docs/2026/all_pairs_final.json (%d paires)" % len(out))
