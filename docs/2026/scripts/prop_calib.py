# -*- coding: utf-8 -*-
"""Le ratio de taille distingue-t-il les 3 mauvaises reponses du temoin ?

Le temoin donne 95,1 % de precision : 58 correctes, 3 fausses sur 61 reponses.
Si les fausses ont un ratio de taille (2026/2025) nettement hors de la
distribution des correctes, un filtre sur ce ratio les eliminerait -- et
ameliorerait la precision sur les resolutions reelles.

On mesure au lieu de supposer : peut-etre que le ratio ne separe rien, auquel cas
le filtre serait un faux confort.
"""
import json
import os

import ida_funcs

S = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

res = json.load(open(os.path.join(S, 'prop_witness_result.json')))
cases = {c['f25']: c for c in json.load(open(os.path.join(S, 'prop_witness_input.json')))}
sizes25 = json.load(open(os.path.join(S, 'witness_sizes25.json')))

good, bad = [], []
for f25, v in res.items():
    if v.get('status') != 'unique':
        continue
    s25 = sizes25.get(f25)
    if not s25:
        continue
    f = ida_funcs.get_func(int(v['got'], 16))
    if f is None:
        continue
    s26 = f.end_ea - f.start_ea
    r = s26 / float(s25)
    (good if v.get('correct') else bad).append((f25, r, s25, s26))

good.sort(key=lambda x: x[1])
print("=== ratios de taille 2026/2025 ===")
if good:
    rs = [g[1] for g in good]
    n = len(rs)
    print("CORRECTES (%d) : min %.2f  p10 %.2f  median %.2f  p90 %.2f  max %.2f"
          % (n, rs[0], rs[max(0, n // 10)], rs[n // 2], rs[min(n - 1, 9 * n // 10)], rs[-1]))
print("\nFAUSSES (%d) :" % len(bad))
for f25, r, s25, s26 in bad:
    print("   %s  ratio %.2f  (%d -> %d octets)" % (f25, r, s25, s26))

if good and bad:
    rs = sorted(g[1] for g in good)
    lo, hi = rs[max(0, len(rs) // 20)], rs[min(len(rs) - 1, 19 * len(rs) // 20)]
    print("\nsi on n'acceptait que les ratios dans [%.2f, %.2f] (5e-95e centile des correctes) :" % (lo, hi))
    kept_good = sum(1 for _, r, _, _ in good if lo <= r <= hi)
    kept_bad = sum(1 for _, r, _, _ in bad if lo <= r <= hi)
    print("   correctes conservees : %d / %d" % (kept_good, len(good)))
    print("   fausses  conservees  : %d / %d" % (kept_bad, len(bad)))
    tot = kept_good + kept_bad
    if tot:
        print("   => precision apres filtre : %.1f %% (contre %.1f %% avant)"
              % (100.0 * kept_good / tot, 100.0 * len(good) / (len(good) + len(bad))))
