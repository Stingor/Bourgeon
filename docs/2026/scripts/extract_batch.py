# -*- coding: utf-8 -*-
"""Extraction PAR LOTS : le pont MCP coupe au-dela de ~2 min et tue le script.

Traite les tables [start, start+count) de switch_targets_all.json et ecrit
all_cases_<tag>_<start>.json. Le cache d'analyse de fonctions est persiste sur
disque entre les lots (func_cache_<tag>.json) : les lots suivants sont beaucoup
plus rapides.

Parametres lus dans batch.json : {"start": N, "count": M}
"""
import json
import os
import time

import idaapi
import idautils
import ida_nalt
import ida_segment

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
MAX_INSN_CASE = 300
MAX_INSN_FUNC = 2500

cfg = json.load(open(os.path.join(D, 'switch_targets_all.json')))
bp = json.load(open(os.path.join(D, 'batch.json')))
START, COUNT = bp['start'], bp['count']
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


CACHE_PATH = os.path.join(D, 'func_cache_%s.json' % tag)
_fc = {}
if os.path.exists(CACHE_PATH):
    try:
        raw = json.load(open(CACHE_PATH))
        _fc = {int(k): (frozenset(v[0]), frozenset(v[1])) for k, v in raw.items()}
    except Exception:
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


t0 = time.time()
out_cases = {}
done = 0
for t in cfg[START:START + COUNT]:
    name = t['tag']
    EA = int(t[tag], 16)
    f = idaapi.get_func(EA)
    if f is None:
        continue
    best, bhead = None, None
    for head in idautils.Heads(f.start_ea, f.end_ea):
        si = idaapi.get_switch_info(head)
        if si and (best is None or si.ncases > best.ncases):
            best, bhead = si, head
    if best is None:
        continue
    try:
        ct = idaapi.calc_switch_cases(bhead, best)
    except Exception:
        continue

    val2tgt = {}
    for i in range(len(ct.cases)):
        for v in ct.cases[i]:
            val2tgt[v] = ct.targets[i]
    entries = set(val2tgt.values())

    for val, tgt in val2tgt.items():
        seen, stack = set(), [tgt]
        globs, calls = set(), set()
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
                        calls.add("0x%08x" % op.addr)
                    elif op.type == idaapi.o_mem:
                        calls.add("[0x%08x]" % op.addr)
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

        for c in list(calls):
            if c.startswith('['):
                continue
            g2, c2 = analyse_func(int(c, 16))
            globs |= g2
            calls |= c2

        out_cases["%s:%d" % (name, val)] = {
            "ninsn": ninsn, "globals": sorted(globs), "calls": sorted(calls)}
    done += 1

path = os.path.join(D, 'all_cases_%s_%03d.json' % (tag, START))
json.dump({"build": tag, "start": START, "count": done, "cases": out_cases}, open(path, 'w'))
json.dump({str(k): [sorted(v[0]), sorted(v[1])] for k, v in _fc.items()},
          open(CACHE_PATH, 'w'))
print("lot %d..%d : %d tables, %d cases, %.0fs (cache %d fn)"
      % (START, START + COUNT, done, len(out_cases), time.time() - t0, len(_fc)))
