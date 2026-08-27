# -*- coding: utf-8 -*-
"""Les champs lointains de CSession, via les GLOBAUX derives de g_session.

Les accesseurs triviaux ne couvrent que 0x16xx-0x17xx : les champs lointains
(talk_type_table_ +0x51F8, hp_ +0x5548, char_name_ +0x81A8) n'ont pas de getter,
le client y accede directement.

Mais g_session est un global a adresse FIXE : `g_session + offset` apparait donc
en clair comme adresse absolue dans le code, et le portage a deja apparie des
milliers de positions globales. Le layout confirme d'ailleurs aid_ par ce biais
(« DAT_015fb9a4 used as AID »), ce qui valide le raisonnement sur un cas connu.

g_session 2025 = 0x015FA3C0 (donne par l'en-tete du layout).
"""
import json
import os

D = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"
G25 = 0x015FA3C0

FIELDS = [
    ("cur_map_type_",     0x0000),
    ("mkcount_",          0x0AFC),
    ("aid_",              0x15E4),
    ("STR",               0x1664),
    ("AGI",               0x1668),
    ("VIT",               0x166C),
    ("INT",               0x1670),
    ("DEX",               0x1674),
    ("LUK",               0x1678),
    ("talk_type_table_",  0x51F8),
    ("hp_",               0x5548),
    ("max_hp_",           0x554C),
    ("sp_",               0x5550),
    ("max_sp_",           0x5554),
    ("char_name_",        0x81A8),
]

port = {}
for c in ('all_pairs_final.json', 'merged_pairs.json', 'global_pos_pairs.json',
          'port_2025_2026.json'):
    p = os.path.join(D, c)
    if not os.path.exists(p):
        continue
    try:
        d = json.load(open(p))
    except Exception:
        continue
    if not isinstance(d, dict):
        continue
    for k, v in d.items():
        tgt = v.get('to') if isinstance(v, dict) else (v if isinstance(v, str) else None)
        if tgt:
            port.setdefault(k.lower(), (tgt, c))

print("%d positions appariees chargees\n" % len(port))

# g_session lui-meme
key = '0x%08x' % G25
hit = port.get(key)
print("g_session 2025 0x%08X  ->  %s" % (G25, hit[0] if hit else 'NON APPARIE'))
g26 = int(hit[0], 16) if hit else None
print()

print("%-18s %-12s %-12s %-12s %s" % ("champ", "off 2025", "abs 2025", "abs 2026", "off 2026"))
found = 0
for name, off in FIELDS:
    a25 = G25 + off
    h = port.get('0x%08x' % a25)
    if h:
        a26 = int(h[0], 16)
        o26 = ('0x%X' % (a26 - g26)) if g26 else '?'
        shift = (a26 - g26 - off) if g26 else None
        print("%-18s 0x%-10X 0x%-10X 0x%-10X %-8s  shift %s"
              % (name, off, a25, a26, o26,
                 ('%+d' % shift) if shift is not None else '?'))
        found += 1
    else:
        print("%-18s 0x%-10X 0x%-10X %-12s %s" % (name, off, a25, 'non apparie', '-'))
print("\n=> %d / %d champs retrouves" % (found, len(FIELDS)))
