# -*- coding: utf-8 -*-
"""v2 : comme extract_cases.py, mais SUIT LES APPELS d'un cran.

Le corps d'un case delegue le gros du travail au handler ; les globales
interessantes sont donc dans les fonctions APPELEES, pas dans le case. On
collecte donc, pour chaque opcode : les globales du case + celles des fonctions
qu'il appelle directement, et les fonctions atteintes a profondeur 1 et 2.

Le cache par fonction est indispensable : une meme fonction est appelee par
beaucoup de cases.
"""
import json
import os

import idaapi
import idautils
import idc
import ida_nalt
import ida_bytes
import ida_segment

OUT_DIR = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
MAX_INSN_CASE = 600
MAX_INSN_FUNC = 3000     # au-dela, fonction utilitaire geante : on ignore


def data_segments():
    out = []
    for i in range(ida_segment.get_segm_qty()):
        s = ida_segment.getnseg(i)
        if s is None:
            continue
        if ida_segment.get_segm_name(s) in ('.data', '.rdata', '.bss', '.idata'):
            out.append((s.start_ea, s.end_ea))
    return out


DATA_SEGS = data_segments()


def in_data(ea):
    for lo, hi in DATA_SEGS:
        if lo <= ea < hi:
            return True
    return False


_fcache = {}


def analyse_func(fea):
    """globales + appels directs d'une fonction entiere (avec cache)."""
    if fea in _fcache:
        return _fcache[fea]
    res = (frozenset(), frozenset())
    f = idaapi.get_func(fea)
    if f is not None:
        n = 0
        globs, calls = set(), set()
        for head in idautils.Heads(f.start_ea, f.end_ea):
            n += 1
            if n > MAX_INSN_FUNC:
                globs, calls = set(), set()   # trop grosse : non discriminante
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
        res = (frozenset(globs), frozenset(calls))
    _fcache[fea] = res
    return res


def main():
    fn_ea = idc.get_name_ea_simple("RecvLoop_DispatchPackets")
    f = idaapi.get_func(fn_ea)

    best = None
    for head in idautils.Heads(f.start_ea, f.end_ea):
        si = idaapi.get_switch_info(head)
        if si and (best is None or si.ncases > best[1].ncases):
            best = (head, si)
    head, si = best
    targets = [ida_bytes.get_dword(si.jumps + 4 * i) for i in range(si.ncases)]
    case_entries = set(t for t in targets if t != si.defjump)
    print("switch %s : %d cases, %d entrees distinctes" % (hex(head), si.ncases, len(case_entries)))

    cases = {}
    for i, tgt in enumerate(targets):
        opcode = si.lowcase + i
        if tgt == si.defjump:
            continue

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
            if ea != tgt and ea in case_entries:
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
            mnem = insn.get_canon_mnem()
            if mnem == "call":
                for op in insn.ops:
                    if op.type == idaapi.o_void:
                        break
                    if op.type == idaapi.o_near:
                        calls1.add("0x%08x" % op.addr)
                    elif op.type == idaapi.o_mem:
                        calls1.add("[0x%08x]" % op.addr)
            if mnem in ("retn", "ret"):
                continue
            if mnem == "jmp":
                for op in insn.ops:
                    if op.type == idaapi.o_near:
                        stack.append(op.addr)
                    break
                continue
            for r in idautils.CodeRefsFrom(ea, 1):
                stack.append(r)

        # --- un cran plus loin ------------------------------------------------
        calls2 = set()
        for c in calls1:
            if c.startswith('['):
                continue
            g2, c2 = analyse_func(int(c, 16))
            globs |= g2
            calls2 |= c2

        cases["0x%04X" % opcode] = {
            "ea": "0x%08x" % tgt,
            "ninsn": ninsn,
            "globals": sorted(globs),
            "calls": sorted(calls1),
            "calls2": sorted(calls2),
        }

    root = ida_nalt.get_root_filename()
    tag = "2026" if "2026" in root else "2025"
    out = {"build": tag, "func": "0x%08x" % fn_ea, "lowcase": si.lowcase,
           "ncases": si.ncases, "cases": cases}
    path = os.path.join(OUT_DIR, "dispatch_cases_%s_d1.json" % tag)
    with open(path, 'w') as fp:
        json.dump(out, fp)
    ng = len(set(g for c in cases.values() for g in c['globals']))
    nc = len(set(g for c in cases.values() for g in c['calls'] + c['calls2']))
    print("ecrit : %s" % path)
    print("  %d cases, %d globales distinctes, %d fonctions distinctes" % (len(cases), ng, nc))


main()
