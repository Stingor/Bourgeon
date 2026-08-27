# -*- coding: utf-8 -*-
"""Mesure le VOLUME DE CODE atteint depuis chaque opcode du dispatch login.

Lecon de 0x0AC4 : un `case` peut exister et ne rien faire. Sa presence ne prouve
donc rien ; ce qui compte est la quantite de code reellement executee. On mesure
donc, pour chaque opcode, le nombre d'instructions atteignables depuis son case,
EN SUIVANT LES APPELS d'un cran (c'est dans la fonction appelee que le travail
disparait).

Comparer 2025 et 2026 fait ressortir les traitements vides.
"""
import json
import os

import idaapi
import idautils
import ida_nalt

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
MAX_INSN = 400
MAX_FN = 2500

root = ida_nalt.get_root_filename()
tag = "2026" if "2026" in root else "2025"
EA = 0x00C3A670 if tag == "2026" else 0x00D27560

f = idaapi.get_func(EA)
targets = {}
for head in idautils.Heads(f.start_ea, f.end_ea):
    si = idaapi.get_switch_info(head)
    if not si:
        continue
    try:
        ct = idaapi.calc_switch_cases(head, si)
    except Exception:
        continue
    for i in range(len(ct.cases)):
        for v in ct.cases[i]:
            targets.setdefault(v, ct.targets[i])

entries = set(targets.values())
_fn = {}


def fn_weight(fea):
    """nombre d'instructions d'une fonction (avec cache)"""
    if fea in _fn:
        return _fn[fea]
    n = 0
    g = idaapi.get_func(fea)
    if g is not None:
        for _ in idautils.Heads(g.start_ea, g.end_ea):
            n += 1
            if n > MAX_FN:
                break
    _fn[fea] = n
    return n


out = {}
for op, tgt in targets.items():
    seen, stack = set(), [tgt]
    ninsn, calls, callw = 0, [], 0
    while stack:
        ea = stack.pop()
        if ea in seen or ninsn > MAX_INSN:
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
        mn = insn.get_canon_mnem()
        if mn == "call":
            for o in insn.ops:
                if o.type == idaapi.o_void:
                    break
                if o.type == idaapi.o_near:
                    calls.append("0x%08x" % o.addr)
                    callw += fn_weight(o.addr)
                break
        if mn in ("retn", "ret"):
            continue
        if mn == "jmp":
            for o in insn.ops:
                if o.type == idaapi.o_near:
                    stack.append(o.addr)
                break
            continue
        for r in idautils.CodeRefsFrom(ea, 1):
            stack.append(r)
    out["%d" % op] = {"ea": "0x%08x" % tgt, "n": ninsn, "callw": callw,
                      "total": ninsn + callw, "calls": calls[:4]}

path = os.path.join(D, 'login_weight_%s.json' % tag)
json.dump({"tag": tag, "cases": out}, open(path, 'w'))
print("%s : %d opcodes mesures" % (tag, len(out)))
print("ecrit : %s" % path)
