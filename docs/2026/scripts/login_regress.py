# -*- coding: utf-8 -*-
"""Detecte les traitements de paquets VIDES en 2026.

Compare, opcode par opcode, le volume de code atteint dans les deux clients.
Un opcode dont le poids s'effondre est un traitement supprime -- comme 0x0AC4,
dont le case existe toujours mais dont la fonction ne fait plus rien.

Ne signale que les opcodes que MOONLIGHT utilise reellement, sinon le bruit
noie le signal (le client traite des centaines d'opcodes kRO inutilises ici).
"""
import io
import json
import os
import re

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"

a = json.load(open(os.path.join(D, 'login_weight_2025.json')))['cases']
b = json.load(open(os.path.join(D, 'login_weight_2026.json')))['cases']

# opcodes declares par Moonlight (clif_packetdb) — pour filtrer le bruit
srv = set()
try:
    ra = json.load(open(os.path.join(DOCS, 'packet_len_ra_packets.json')))
    for k in ra:
        try:
            srv.add(int(k, 16) if isinstance(k, str) and k.startswith('0x') else int(k))
        except Exception:
            pass
except Exception:
    pass
print("opcodes declares par Moonlight : %d" % len(srv))

common = sorted(set(a) & set(b), key=int)
print("opcodes communs aux deux dispatchs login : %d\n" % len(common))

rows = []
for k in common:
    x, y = a[k]['total'], b[k]['total']
    if x < 12:                      # deja quasi vide en 2025 : rien a dire
        continue
    if y >= max(6, x * 0.35):       # volume conserve
        continue
    rows.append((int(k), x, y, a[k]['ea'], b[k]['ea']))

rows.sort(key=lambda r: -(r[1] - r[2]))
print("=== traitements EFFONDRES en 2026 ===")
print("  %-8s %-10s %-8s %-8s %s" % ("opcode", "hex", "2025", "2026", "utilise par Moonlight ?"))
n_srv = 0
for op, x, y, ea1, ea2 in rows:
    used = op in srv
    if used:
        n_srv += 1
    print("  %-8d 0x%04X     %-8d %-8d %s" % (op, op, x, y, "OUI  <==" if used else ""))
print("\n  %d opcodes effondres, dont %d utilises par Moonlight" % (len(rows), n_srv))

json.dump([{"opcode": "0x%04X" % op, "w2025": x, "w2026": y,
            "ea2025": e1, "ea2026": e2, "moonlight": op in srv}
           for op, x, y, e1, e2 in rows],
          io.open(os.path.join(DOCS, 'login_regressions.json'), 'w', encoding='utf-8'),
          indent=1, ensure_ascii=False)
print("\necrit : docs/2026/login_regressions.json")
