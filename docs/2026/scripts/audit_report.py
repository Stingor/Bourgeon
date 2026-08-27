# -*- coding: utf-8 -*-
"""Audit de port_2025_2026.json : collisions et ecarts de taille."""
import json
import os
from collections import defaultdict

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
REF = r"d:/Mes documents/GitHub/Bourgeon/docs/2026/port_2025_2026.json"

ref = json.load(open(REF))
a25 = json.load(open(os.path.join(D, 'audit_2025.json')))
a26 = json.load(open(os.path.join(D, 'audit_2026.json')))


def n(x):
    return '0x%08x' % int(x, 16)


# --- 1. collisions ------------------------------------------------------------
rev = defaultdict(list)
for src, rec in ref.items():
    rev[n(rec['to'])].append((n(src), rec))

collisions = {t: v for t, v in rev.items() if len(v) > 1}
print("=== COLLISIONS (plusieurs sources 2025 -> une cible 2026) ===")
print("  cibles en collision : %d  (%d entrees concernees)"
      % (len(collisions), sum(len(v) for v in collisions.values())))
for t, v in sorted(collisions.items()):
    tsz = a26.get(t, {}).get('size', 0)
    print("\n  cible %s (taille 0x%X)" % (t, tsz))
    for s, rec in v:
        ssz = a25.get(s, {}).get('size', 0)
        flag = "  <-- taille compatible" if tsz and ssz and min(tsz, ssz) / float(max(tsz, ssz)) > 0.8 else ""
        print("     %s  0x%-6X  %-42s via %s%s"
              % (s, ssz, (rec.get('name25') or '')[:42], rec.get('via'), flag))

# --- 2. ecarts de taille ------------------------------------------------------
print("\n=== ECARTS DE TAILLE ===")
bad, ok, skip = [], 0, 0
for src, rec in ref.items():
    s, t = n(src), n(rec['to'])
    ssz = a25.get(s, {}).get('size', 0)
    tsz = a26.get(t, {}).get('size', 0)
    if not ssz or not tsz:
        skip += 1
        continue
    r = min(ssz, tsz) / float(max(ssz, tsz))
    if r < 0.6:
        bad.append((r, s, t, ssz, tsz, rec))
    else:
        ok += 1

print("  paires comparables : %d  (non-fonctions ignorees : %d)" % (ok + len(bad), skip))
print("  ratio > 0.6        : %d" % ok)
print("  ratio < 0.6 SUSPECT: %d" % len(bad))
for r, s, t, ssz, tsz, rec in sorted(bad):
    print("     %.2f  %s (0x%X) -> %s (0x%X)  %-38s via %s"
          % (r, s, ssz, t, tsz, (rec.get('name25') or '')[:38], rec.get('via')))

suspects = set(s for _, s, _, _, _, _ in bad) | set(
    s for v in collisions.values() for s, _ in v)
print("\n=== BILAN ===")
print("  entrees SUSPECTES au total : %d / %d (%.0f%%)"
      % (len(suspects), len(ref), 100.0 * len(suspects) / len(ref)))
json.dump(sorted(suspects), open(os.path.join(D, 'suspects.json'), 'w'), indent=1)
