# -*- coding: utf-8 -*-
"""Extrait PLUSIEURS switches d'un coup, en prefixant chaque valeur de case par
le nom de la table. Les signatures deviennent composites : un objet vu par
plusieurs identifiants independants est d'autant plus discriminant.

Config : switch_targets_multi.json = [{"tag": "...", "2025": "0x..", "2026": "0x.."}, ...]
"""
import json
import os

import idaapi
import idautils
import ida_nalt
import ida_segment

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
MAX_INSN_CASE = 400
MAX_INSN_FUNC = 3000

targets = json.load(open(os.path.join(D, 'switch_targets_multi.json')))
root = ida_nalt.get_root_filename()
tag = "2026" if "2026" in root else "2025"

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


all_cases = {}
meta = {}

for t in targets:
    name = t['tag']
    EA = int(t[tag], 16)
    f = idaapi.get_func(EA)
    if f is None:
        print("%-12s : fonction introuvable a 0x%08X" % (name, EA))
        continue
    best, bhead = None, None
    for head in idautils.Heads(f.start_ea, f.end_ea):
        si = idaapi.get_switch_info(head)
        if si and (best is None or si.ncases > best.ncases):
            best, bhead = si, head
    if best is None:
        print("%-12s : aucun switch" % name)
        continue

    ct = idaapi.calc_switch_cases(bhead, best)
    val2tgt = {}
    for i in range(len(ct.cases)):
        for v in ct.cases[i]:
            val2tgt[v] = ct.targets[i]
    entries = set(val2tgt.values())

    for val, tgt in val2tgt.items():
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

        all_cases["%s:%d" % (name, val)] = {
            "ea": "0x%08x" % tgt, "ninsn": ninsn,
            "globals": sorted(globs), "calls": sorted(calls1 | calls2),
        }

    meta[name] = {"func": "0x%08x" % EA, "switch": "0x%08x" % bhead,
                  "nvals": len(val2tgt), "ntgts": len(entries),
                  "lo": min(val2tgt), "hi": max(val2tgt)}
    print("%-12s 0x%08X : %d valeurs (%d..%d) -> %d cibles"
          % (name, EA, len(val2tgt), min(val2tgt), max(val2tgt), len(entries)))

path = os.path.join(D, 'multi_cases_%s.json' % tag)
json.dump({"build": tag, "meta": meta, "cases": all_cases}, open(path, 'w'))
ng = len(set(g for c in all_cases.values() for g in c['globals']))
nc = len(set(g for c in all_cases.values() for g in c['calls']))
print("\necrit : %s" % path)
print("  %d cases au total, %d globales, %d fonctions" % (len(all_cases), ng, nc))
