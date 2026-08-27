# -*- coding: utf-8 -*-
"""Pour chaque fonction appariee, la SEQUENCE ORDONNEE des globales qu'elle
reference (ordre des adresses d'instruction).

Deux fonctions appariees qui referencent leurs globales dans le meme ordre
permettent d'apparier ces globales POSITION PAR POSITION -- le seul vecteur qui
atteigne les zones de donnees, hors de portee des switches et du RTTI.

Lots via batch.json : {"start": N, "count": M}
"""
import json
import os

import idaapi
import idautils
import ida_nalt
import ida_segment

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

pairs = json.load(open(os.path.join(D, 'fnpairs_all.json')))
bp = json.load(open(os.path.join(D, 'batch.json')))
START, COUNT = bp['start'], bp['count']
root = ida_nalt.get_root_filename()
tag = "2026" if "2026" in root else "2025"

DATA = []
for i in range(ida_segment.get_segm_qty()):
    s = ida_segment.getnseg(i)
    if s is not None and ida_segment.get_segm_name(s) in ('.data', '.rdata', '.bss'):
        DATA.append((s.start_ea, s.end_ea))


def in_data(ea):
    for lo, hi in DATA:
        if lo <= ea < hi:
            return True
    return False


keys = sorted(pairs)
out = {}
for k in keys[START:START + COUNT]:
    a = pairs[k] if tag == "2026" else k
    ea = int(a, 16)
    f = idaapi.get_func(ea)
    if f is None or f.start_ea != ea:
        continue
    if f.end_ea - f.start_ea > 0x4000:      # fonctions geantes : non discriminantes
        continue
    seq = []
    for head in idautils.Heads(f.start_ea, f.end_ea):
        insn = idaapi.insn_t()
        if idaapi.decode_insn(insn, head) <= 0:
            continue
        for op in insn.ops:
            if op.type == idaapi.o_void:
                break
            if op.type in (idaapi.o_mem, idaapi.o_displ) and in_data(op.addr):
                seq.append("0x%08x" % op.addr)
    if seq:
        out[k] = seq

path = os.path.join(D, 'seqs_%s_%05d.json' % (tag, START))
json.dump(out, open(path, 'w'))
tot = sum(len(v) for v in out.values())
print("lot %d..%d : %d fonctions avec globales, %d references"
      % (START, START + COUNT, len(out), tot))
