# -*- coding: utf-8 -*-
"""Extracteur generique : pour un switch donne, releve par VALEUR de case ce que
le code touche (globales + fonctions appelees, en suivant les appels d'un cran).

Utilise idaapi.calc_switch_cases, qui rend les vraies valeurs de case y compris
pour les switches a table d'INDIRECTION (ou si.lowcase est inexploitable).

Cible lue dans switch_target.json : {"2025": "0x...", "2026": "0x...", "tag": "..."}
"""
import json
import os

import idaapi
import idautils
import idc
import ida_nalt
import ida_segment

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
MAX_INSN_CASE = 600
MAX_INSN_FUNC = 3000

cfg = json.load(open(os.path.join(D, 'switch_target.json')))
root = ida_nalt.get_root_filename()
tag = "2026" if "2026" in root else "2025"
EA = int(cfg[tag], 16)
NAME = cfg['tag']

DATA_SEGS = []
for i in range(ida_segment.get_segm_qty()):
    s = ida_segment.getnseg(i)
    if s is not None and ida_segment.get_segm_name(s) in ('.data', '.rdata', '.bss', '.idata'):
        DATA_SEGS.append((s.start_ea, s.end_ea))


def in_data(ea):
    for lo, hi in DATA_SEGS:
        if lo <= ea < hi:
            return True
    return False


_fc = {}


def analyse_func(fea):
    if fea in _fc:
        return _fc[fea]
    globs, calls = set(), set()
    f = idaapi.get_func(fea)
    if f is not None:
        n = 0
        for head in idautils.Heads(f.start_ea, f.end_ea):
            n += 1
            if n > MAX_INSN_FUNC:
                globs, calls = set(), set()
                break
            insn = idaapi.insn_t()
            if idaapi.decode_insn(insn, head) <= 0:
                continue
            for op in insn.ops:
                if op.type == idaapi.o_void:
                    break
                if op.type in (idaapi.o_mem, idaapi.o_displ) and in_data(op.addr):
                    globs.add("0x%08x" % op.addr)
            if insn.get_canon_mnem() == "call":
                for op in insn.ops:
                    if op.type == idaapi.o_void:
                        break
                    if op.type == idaapi.o_near:
                        calls.add("0x%08x" % op.addr)
                    elif op.type == idaapi.o_mem:
                        calls.add("[0x%08x]" % op.addr)
    _fc[fea] = (frozenset(globs), frozenset(calls))
    return _fc[fea]


f = idaapi.get_func(EA)
best, bhead = None, None
for head in idautils.Heads(f.start_ea, f.end_ea):
    si = idaapi.get_switch_info(head)
    if si and (best is None or si.ncases > best.ncases):
        best, bhead = si, head

ct = idaapi.calc_switch_cases(bhead, best)
print("switch @0x%08X : %d groupes de cases" % (bhead, len(ct.cases)))

# valeur -> cible
val2tgt = {}
for i in range(len(ct.cases)):
    tgt = ct.targets[i]
    for v in ct.cases[i]:
        val2tgt[v] = tgt
print("valeurs distinctes : %d, cibles distinctes : %d"
      % (len(val2tgt), len(set(val2tgt.values()))))
print("plage de valeurs   : %d .. %d" % (min(val2tgt), max(val2tgt)))

entries = set(val2tgt.values())
cases = {}
for val, tgt in sorted(val2tgt.items()):
    seen, stack = set(), [tgt]
    globs, calls1 = set(), set()
    ninsn = 0
    while stack:
        ea = stack.pop()
        if ea in seen or ninsn > MAX_INSN_CASE:
            continue
        seen.add(ea)
        if not (f.start_ea <= ea < f.end_ea):
            continue
        if ea != tgt and ea in entries:
            continue
        insn = idaapi.insn_t()
        if idaapi.decode_insn(insn, ea) <= 0:
            continue
        ninsn += 1
        for op in insn.ops:
            if op.type == idaapi.o_void:
                break
            if op.type in (idaapi.o_mem, idaapi.o_displ) and in_data(op.addr):
                globs.add("0x%08x" % op.addr)
        mn = insn.get_canon_mnem()
        if mn == "call":
            for op in insn.ops:
                if op.type == idaapi.o_void:
                    break
                if op.type == idaapi.o_near:
                    calls1.add("0x%08x" % op.addr)
                elif op.type == idaapi.o_mem:
                    calls1.add("[0x%08x]" % op.addr)
        if mn in ("retn", "ret"):
            continue
        if mn == "jmp":
            for op in insn.ops:
                if op.type == idaapi.o_near:
                    stack.append(op.addr)
                break
            continue
        for r in idautils.CodeRefsFrom(ea, 1):
            stack.append(r)

    calls2 = set()
    for c in calls1:
        if c.startswith('['):
            continue
        g2, c2 = analyse_func(int(c, 16))
        globs |= g2
        calls2 |= c2

    cases["%d" % val] = {"ea": "0x%08x" % tgt, "ninsn": ninsn,
                         "globals": sorted(globs), "calls": sorted(calls1),
                         "calls2": sorted(calls2)}

out = {"build": tag, "func": "0x%08x" % EA, "cases": cases}
path = os.path.join(D, "switch_%s_%s.json" % (NAME, tag))
json.dump(out, open(path, 'w'))
ng = len(set(g for c in cases.values() for g in c['globals']))
nc = len(set(g for c in cases.values() for g in c['calls'] + c['calls2']))
print("ecrit : %s" % path)
print("  %d cases, %d globales, %d fonctions" % (len(cases), ng, nc))
