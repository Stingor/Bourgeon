# -*- coding: utf-8 -*-
"""Meme detecteur, applique au dispatch du MAP-SERVER (RecvLoop_DispatchPackets).

On dispose deja des cases extraits pour les deux builds (multi_cases_*.json,
avec les globales et les appels de chaque case). Un opcode dont le volume de
code s'effondre est un traitement supprime -- le piege de 0x0AC4, mais cote jeu.

Filtre sur les opcodes que MOONLIGHT emet reellement, sinon le bruit domine.
"""
import io
import json
import os
import re

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"
SRV = r"d:/Mes documents/GitHub/moonlight/src/map/clif_packetdb.hpp"

a = json.load(open(os.path.join(D, 'multi_cases_2025.json')))['cases']
b = json.load(open(os.path.join(D, 'multi_cases_2026.json')))['cases']

# --- opcodes reellement declares par Moonlight -------------------------------
srv = set()
if os.path.exists(SRV):
    txt = io.open(SRV, encoding='utf-8', errors='replace').read()
    for m in re.finditer(r'packet\(\s*0x([0-9a-fA-F]{3,4})', txt):
        srv.add(int(m.group(1), 16))
    for m in re.finditer(r'parseable_packet\(\s*0x([0-9a-fA-F]{3,4})', txt):
        srv.add(int(m.group(1), 16))
print("opcodes declares par Moonlight : %d" % len(srv))

rows = []
for k in sorted(set(a) & set(b)):
    if not k.startswith('dispatch:'):
        continue
    op = int(k.split(':')[1])
    x, y = a[k], b[k]
    wx = x['ninsn'] + len(x['calls']) * 8
    wy = y['ninsn'] + len(y['calls']) * 8
    if wx < 20:
        continue
    if wy >= max(8, wx * 0.35):
        continue
    rows.append((op, wx, wy, len(x['calls']), len(y['calls']), op in srv))

rows.sort(key=lambda r: (-r[5], -(r[1] - r[2])))
used = [r for r in rows if r[5]]
print("\n=== dispatch JEU : traitements effondres en 2026 ===")
print("  total : %d, dont %d UTILISES par Moonlight\n" % (len(rows), len(used)))
print("  %-8s %-8s %-8s %-8s %s" % ("opcode", "2025", "2026", "appels", "Moonlight"))
for op, wx, wy, cx, cy, u in rows[:40]:
    print("  0x%04X   %-8d %-8d %d->%-6d %s" % (op, wx, wy, cx, cy, "OUI  <==" if u else ""))

json.dump([{"opcode": "0x%04X" % op, "w2025": wx, "w2026": wy,
            "calls2025": cx, "calls2026": cy, "moonlight": u}
           for op, wx, wy, cx, cy, u in rows],
          io.open(os.path.join(DOCS, 'game_regressions.json'), 'w', encoding='utf-8'),
          indent=1, ensure_ascii=False)
print("\necrit : docs/2026/game_regressions.json")
