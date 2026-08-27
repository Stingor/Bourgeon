# -*- coding: utf-8 -*-
"""Constitue le jeu de test du temoin, cote IDB 2025.

\U0001f534 Piege de circularite : si le temoin est fait de paires etablies par la
PROPAGATION, il ne mesure que la coherence de la methode avec elle-meme. On ne
retient donc que des paires venues d'AUTRES vecteurs -- opcodes, RTTI, chaines --
identifiables par leur champ `via`/`src`.

Pour chaque cas : les appeles portes (traduits en adresses 2026, ce sont les
ancres), le `retn`, et la reponse attendue.
"""
import json
import os
from collections import Counter

import idc
import idaapi
import idautils
import ida_funcs

D = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"
S = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

# --- paires, avec leur provenance -------------------------------------------
port, via = {}, {}
for c in ('all_pairs_final.json', 'merged_pairs.json', 'port_2025_2026.json',
          'global_pos_pairs.json', 'vtable_pairs.json'):
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
        t = v.get('to') if isinstance(v, dict) else (v if isinstance(v, str) else None)
        if not t:
            continue
        try:
            ki, ti = int(k, 16), int(t, 16)
        except ValueError:
            continue
        if ki not in port:
            port[ki] = ti
            src = ''
            if isinstance(v, dict):
                src = str(v.get('via', '') or v.get('src', '') or '')
            via[ki] = src

print("provenances :", Counter(via.values()).most_common(8))

# Les provenances relevees sont : vtable, tables, appel-i1, position, appel-i2,
# position+tables, portage, accesseur. ⚠ appel-i1 et appel-i2 SONT des
# propagations par appels -- la meme famille que la methode a tester. Les garder
# rendrait le temoin circulaire : il mesurerait la coherence de la methode avec
# elle-meme, pas sa justesse.
PROP_LIKE = ('appel', 'propag')
NON_PROP = {k for k, s in via.items()
            if not any(t in s.lower() for t in PROP_LIKE)}
print("paires NON issues de la propagation : %d / %d" % (len(NON_PROP), len(port)))


def retn_of(f):
    r = None
    p = f.start_ea
    while p < f.end_ea:
        if idc.print_insn_mnem(p) == 'retn':
            r = idc.print_operand(p, 0) or '0'
        n = idc.next_head(p, f.end_ea)
        if n <= p:
            break
        p = n
    return r


cases = []
for ea in sorted(NON_PROP):
    f = ida_funcs.get_func(ea)
    if f is None or f.start_ea != ea:
        continue
    callees = []
    p = f.start_ea
    while p < f.end_ea:
        if idc.print_insn_mnem(p) == 'call' and idc.get_operand_type(p, 0) == idc.o_near:
            callees.append(idc.get_operand_value(p, 0))
        n = idc.next_head(p, f.end_ea)
        if n <= p:
            break
        p = n
    anchors = ['0x%08x' % port[c] for c in dict.fromkeys(callees) if c in port]
    if len(anchors) >= 2:
        cases.append({'f25': '0x%08x' % ea,
                      'to_expected': '0x%08x' % port[ea],
                      'anchors': anchors[:40],
                      'retn': retn_of(f),
                      'name': idc.get_func_name(ea) or ''})
    if len(cases) >= 120:
        break

print("%d cas de temoin constitues (>= 2 ancres, hors propagation)" % len(cases))
json.dump(cases, open(os.path.join(S, 'prop_witness_input.json'), 'w'), indent=0)
print("ecrit : prop_witness_input.json")
