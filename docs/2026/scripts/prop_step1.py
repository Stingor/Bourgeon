# -*- coding: utf-8 -*-
"""Etape 1 (IDB 2025) : les relations d'appel des adresses encore non portees.

185 des 395 adresses codees en dur dans Bourgeon n'ont pas d'equivalent 2026
connu. Pour chacune on releve ses APPELANTS et ses APPELES : si l'un d'eux est
deja porte, il donne un point d'entree dans le binaire 2026, et les candidats se
reduisent aux quelques fonctions que son equivalent appelle.

On note aussi taille et `retn N` : ce sont eux qui trancheront ensuite entre
plusieurs candidats.
"""
import json
import os

import idc
import idaapi
import idautils
import ida_funcs

D = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"
OUT = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad/prop_2025.json"

cov = json.load(open(os.path.join(D, 'hardcoded_coverage.json')))
targets = [int(a, 16) for a in cov['unknown']]
print("%d cibles non portees" % len(targets))


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


out = {}
no_func = 0
for a in targets:
    f = ida_funcs.get_func(a)
    if f is None:
        no_func += 1
        out['0x%08x' % a] = {'kind': 'data'}
        continue
    is_start = (f.start_ea == a)
    callers = sorted({x.frm for x in idautils.XrefsTo(f.start_ea)
                      if x.type in (16, 17)})
    callees = []
    p = f.start_ea
    while p < f.end_ea:
        if idc.print_insn_mnem(p) == 'call' and idc.get_operand_type(p, 0) == idc.o_near:
            callees.append(idc.get_operand_value(p, 0))
        n = idc.next_head(p, f.end_ea)
        if n <= p:
            break
        p = n
    out['0x%08x' % a] = {
        'kind': 'code' if is_start else 'inside',
        'func': '0x%08x' % f.start_ea,
        'off': a - f.start_ea,
        'size': f.end_ea - f.start_ea,
        'retn': retn_of(f),
        'name': idc.get_func_name(f.start_ea) or '',
        'callers': ['0x%08x' % c for c in callers[:40]],
        'callees': ['0x%08x' % c for c in dict.fromkeys(callees)][:40],
    }

print("  dont %d hors fonction (donnees)" % no_func)
json.dump(out, open(OUT, 'w'), indent=0)
print("ecrit : %s" % OUT)
