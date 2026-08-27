# -*- coding: utf-8 -*-
"""Appariement 2025 <-> 2026 par SIGNATURE DE CO-OCCURRENCE D'OPCODES.

Applique la meme methode aux deux natures d'objet vues depuis le switch de
RecvLoop_DispatchPackets : les FONCTIONS appelees par un case, et les GLOBALES
touchees par un case.

L'opcode est l'identifiant stable ; l'ensemble des opcodes qui touchent un objet
est sa signature. Signature identique et UNIQUE des deux cotes => paire.
Signature partagee par plusieurs objets d'un meme cote => on ne conclut pas.
"""
import json
import os
import sys
from collections import defaultdict

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

SUFFIX = sys.argv[1] if len(sys.argv) > 1 else ''
a = json.load(open(os.path.join(D, 'dispatch_cases_2025%s.json' % SUFFIX)))
b = json.load(open(os.path.join(D, 'dispatch_cases_2026%s.json' % SUFFIX)))
ca, cb = a['cases'], b['cases']
common = sorted(set(ca) & set(cb))


def collect(cases, opcodes, kind):
    sig = defaultdict(set)
    for o in opcodes:
        items = cases[o][kind]
        if isinstance(items, dict):
            items = items.keys()
        for g in items:
            sig[g].add(o)
    return {g: frozenset(s) for g, s in sig.items()}


def match(sa, sb, label):
    grp_a, grp_b = defaultdict(list), defaultdict(list)
    for g, s in sa.items():
        grp_a[s].append(g)
    for g, s in sb.items():
        grp_b[s].append(g)

    pairs, ambig, orphan = {}, 0, 0
    for s, la in grp_a.items():
        lb = grp_b.get(s)
        if not lb:
            orphan += 1
            continue
        if len(la) == 1 and len(lb) == 1:
            pairs[la[0]] = {"to": lb[0], "nop": len(s), "opcodes": sorted(s)}
        else:
            ambig += 1

    solid = sum(1 for p in pairs.values() if p['nop'] >= 3)
    print("\n=== %s ===" % label)
    print("  objets 2025 / 2026        : %d / %d" % (len(sa), len(sb)))
    print("  paires CERTAINES          : %d  (dont %d avec >= 3 opcodes)" % (len(pairs), solid))
    print("  signatures ambigues       : %d" % ambig)
    print("  signatures sans equivalent: %d" % orphan)
    return pairs


print("opcodes communs : %d" % len(common))

def collect2(cases, opcodes):
    from collections import defaultdict as dd
    sig = dd(set)
    for o in opcodes:
        for g in cases[o].get('calls', []) + cases[o].get('calls2', []):
            sig[g].add(o)
    return {g: frozenset(s) for g, s in sig.items()}


fn_pairs = match(collect2(ca, common), collect2(cb, common),
                 "FONCTIONS atteintes (profondeur 1 + 2)")
gl_pairs = match(collect(ca, common, 'globals'), collect(cb, common, 'globals'),
                 "GLOBALES touchees par les cases")

# --- controle : les deltas se regroupent-ils ? -------------------------------
def deltas(pairs, label):
    d = defaultdict(int)
    for g, p in pairs.items():
        if g.startswith('[') or p['to'].startswith('['):
            continue
        d[int(p['to'], 16) - int(g, 16)] += 1
    top = sorted(d.items(), key=lambda kv: -kv[1])[:6]
    tot = sum(d.values())
    grouped = sum(n for _, n in top)
    print("\n  deltas %s : %d paires, les 6 plus frequents en couvrent %d (%.0f%%)"
          % (label, tot, grouped, 100.0 * grouped / tot if tot else 0))
    for dd, n in top:
        print("     %+#012x : %4d" % (dd, n))


deltas(fn_pairs, "fonctions")
deltas(gl_pairs, "globales")

json.dump({"functions": fn_pairs, "globals": gl_pairs},
          open(os.path.join(D, 'opcode_pairs%s.json' % SUFFIX), 'w'), indent=1)
print("\necrit : opcode_pairs%s.json" % SUFFIX)
