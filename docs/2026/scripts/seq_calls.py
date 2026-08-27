# -*- coding: utf-8 -*-
"""Sequence ORDONNEE des appels de chaque fonction appariee.

Meme principe que pour les globales : deux fonctions appariees appellent en
principe les memes fonctions dans le meme ordre. C'est ce qui permet de propager
l'appariement de proche en proche, et donc d'atteindre les 182 fonctions
restantes que ni les switches ni le RTTI ne touchent.

Lots via batch.json. Le fichier de sortie est indexe par la cle 2025 des DEUX
cotes (comme seq_globals.py).
"""
import json
import os

import idaapi
import idautils
import ida_nalt

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

pairs = json.load(open(os.path.join(D, 'fnpairs_iter.json')))
bp = json.load(open(os.path.join(D, 'batch.json')))
START, COUNT = bp['start'], bp['count']
root = ida_nalt.get_root_filename()
tag = "2026" if "2026" in root else "2025"

keys = sorted(pairs)
out = {}
for k in keys[START:START + COUNT]:
    a = pairs[k] if tag == "2026" else k
    ea = int(a, 16)
    f = idaapi.get_func(ea)
    if f is None or f.start_ea != ea:
        continue
    if f.end_ea - f.start_ea > 0x4000:
        continue
    seq = []
    for head in idautils.Heads(f.start_ea, f.end_ea):
        insn = idaapi.insn_t()
        if idaapi.decode_insn(insn, head) <= 0:
            continue
        if insn.get_canon_mnem() != "call":
            continue
        for op in insn.ops:
            if op.type == idaapi.o_void:
                break
            if op.type == idaapi.o_near:
                seq.append("0x%08x" % op.addr)
            elif op.type == idaapi.o_mem:
                seq.append("[0x%08x]" % op.addr)
            break
    if seq:
        out[k] = seq

path = os.path.join(D, 'calls_%s_%05d.json' % (tag, START))
json.dump(out, open(path, 'w'))
print("lot %d..%d : %d fonctions, %d appels"
      % (START, START + COUNT, len(out), sum(len(v) for v in out.values())))
