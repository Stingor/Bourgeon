# -*- coding: utf-8 -*-
"""TEMOIN : la methode retrouve-t-elle des paires DEJA CONNUES ?

23 resolutions « uniques » sur 61 tentatives, c'est un chiffre plausible -- et
un chiffre plausible ne prouve rien. On rejoue exactement le meme algorithme sur
des fonctions dont la reponse est deja etablie par ailleurs, et on compte les
succes ET les erreurs.

Une methode qui rend « unique » une mauvaise reponse est pire qu'une methode qui
ne rend rien : elle empoisonne le portage en silence, comme les 11 entrees
fausses deja trouvees.

Le taux d'ERREUR est ce qui compte, pas le taux de reponse.
"""
import json
import os
import random

import idc
import idaapi
import idautils
import ida_funcs

S = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
WIT = os.path.join(S, 'prop_witness_input.json')
OUT = os.path.join(S, 'prop_witness_result.json')

cases = json.load(open(WIT))       # [{f25, to_expected, anchors:[...], retn}]
prev = json.load(open(OUT)) if os.path.exists(OUT) else {}


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


def callers_of(ea):
    out = set()
    for x in idautils.XrefsTo(ea):
        if x.type not in (16, 17):
            continue
        f = ida_funcs.get_func(x.frm)
        if f:
            out.add(f.start_ea)
    return out


todo = [c for c in cases if c['f25'] not in prev][:40]
print("temoin : %d cas restants, on en traite %d" % (
    len([c for c in cases if c['f25'] not in prev]), len(todo)))

for c in todo:
    sets = []
    for a in c['anchors']:
        try:
            sets.append(callers_of(int(a, 16)))
        except Exception:
            pass
    if not sets:
        prev[c['f25']] = {'status': 'no-anchor'}
        continue
    inter = set.intersection(*sets) if len(sets) > 1 else sets[0]
    kept = [x for x in inter
            if ida_funcs.get_func(x) and retn_of(ida_funcs.get_func(x)) == c['retn']]
    if len(kept) == 1:
        got = '0x%08x' % kept[0]
        prev[c['f25']] = {'status': 'unique', 'got': got,
                          'expected': c['to_expected'],
                          'correct': got.lower() == c['to_expected'].lower()}
    else:
        prev[c['f25']] = {'status': 'ambiguous' if kept else 'empty', 'n': len(kept)}

json.dump(prev, open(OUT, 'w'), indent=0)

uniq = [v for v in prev.values() if v.get('status') == 'unique']
good = [v for v in uniq if v.get('correct')]
bad = [v for v in uniq if v.get('correct') is False]
print("\n=== TEMOIN : %d cas evalues ===" % len(prev))
print("  reponse unique : %d" % len(uniq))
print("  dont CORRECTE  : %d" % len(good))
print("  dont FAUSSE    : %d" % len(bad))
if uniq:
    print("  => precision quand la methode repond : %.1f %%"
          % (100.0 * len(good) / len(uniq)))
for k, v in list(prev.items()):
    if v.get('correct') is False:
        print("    FAUX : %s  attendu %s  obtenu %s"
              % (k, v['expected'], v['got']))
