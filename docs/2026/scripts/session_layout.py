# -*- coding: utf-8 -*-
"""Compare le layout de la zone 'session' entre 2025 et 2026.

g_session est appariee ; on regarde si les membres suivent un decalage
CONSTANT (bloc deplace tel quel) ou si la zone a ete REORGANISEE.
"""
import io
import json
import os

DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"
pairs = json.load(io.open(os.path.join(DOCS, 'port_opcode_pairs.json'), encoding='utf-8'))

BASE25 = 0x015FA3C0
base26 = int(pairs['0x015fa3c0']['to'], 16)
print("g_session : 0x%08X -> 0x%08X   (delta %+#x)\n" % (BASE25, base26, base26 - BASE25))

rows = []
for s, v in pairs.items():
    if v['kind'] != 'globale':
        continue
    a = int(s, 16)
    if not (BASE25 <= a < BASE25 + 0x9000):
        continue
    t = int(v['to'], 16)
    rows.append((a - BASE25, t - base26, a, t,
                 v.get('manifeste', '') or v.get('name25', ''), v['nop']))

rows.sort()
print("%-9s %-9s %-7s  %-4s %s" % ("off 2025", "off 2026", "ecart", "opc", "nom"))
prev = None
for o25, o26, a, t, nm, nop in rows:
    ec = o26 - o25
    mark = ""
    if prev is not None and ec != prev:
        mark = "   <-- l'ecart CHANGE"
    prev = ec
    print("+0x%-6X +0x%-6X %+-7d %-4d %s%s" % (o25, o26, ec, nop, nm[:44], mark))

ecarts = {}
for o25, o26, a, t, nm, nop in rows:
    ecarts.setdefault(o26 - o25, []).append(o25)
print("\n%d membres releves, %d ecart(s) distinct(s) :" % (len(rows), len(ecarts)))
for e in sorted(ecarts):
    v = ecarts[e]
    print("   %+6d : %2d membres, de +0x%X a +0x%X" % (e, len(v), min(v), max(v)))
