# -*- coding: utf-8 -*-
"""Recense TOUTES les fonctions qui resolvent un point d'entree EOS.

Le patch NoEOSAntiCheat en neutralise deja deux, mais l'entree en jeu a trouve
une troisieme (sub_7A2FA0). Plutot que de corriger au coup par coup, on liste
tous les sites : chacun fait GetProcAddress(hModule, "_EOS_...") sur un hModule
NUL, puis appelle le resultat sans le tester.
"""
import json
import os
import re

import idaapi
import idautils
import ida_bytes
import ida_name
import ida_segment

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

# toutes les chaines "_EOS_..." de .rdata
strs = []
for i in range(ida_segment.get_segm_qty()):
    s = ida_segment.getnseg(i)
    if s is None or ida_segment.get_segm_name(s) not in ('.rdata', '.data'):
        continue
    blob = ida_bytes.get_bytes(s.start_ea, s.end_ea - s.start_ea) or b''
    for m in re.finditer(rb'_EOS_[A-Za-z0-9_]+@?\d*\x00', blob):
        ea = s.start_ea + m.start()
        strs.append((ea, m.group(0)[:-1].decode('latin-1')))

print("chaines _EOS_* : %d" % len(strs))

fns = {}
for ea, txt in strs:
    for xr in idautils.XrefsTo(ea):
        f = idaapi.get_func(xr.frm)
        if f is None:
            continue
        fns.setdefault(f.start_ea, {"name": ida_name.get_name(f.start_ea) or "",
                                    "size": f.end_ea - f.start_ea,
                                    "procs": set()})["procs"].add(txt)

print("\n=== fonctions qui resolvent un proc EOS : %d ===" % len(fns))
out = []
for fea in sorted(fns):
    v = fns[fea]
    # prologue, pour savoir comment la neutraliser
    b = ida_bytes.get_bytes(fea, 8) or b''
    # retn N ? on cherche le dernier retn de la fonction
    f = idaapi.get_func(fea)
    retn = None
    for head in idautils.Heads(f.start_ea, f.end_ea):
        insn = idaapi.insn_t()
        if idaapi.decode_insn(insn, head) > 0 and insn.get_canon_mnem() in ("retn", "ret"):
            op = insn.ops[0]
            retn = op.value if op.type == idaapi.o_imm else 0
    print("  0x%08X  %-22s 0x%-5X retn %-4s  prologue %s"
          % (fea, v['name'][:22], v['size'], retn if retn is not None else '?', b.hex(' ')))
    for p in sorted(v['procs']):
        print("        %s" % p)
    out.append({"ea": "0x%08x" % fea, "name": v['name'], "size": v['size'],
                "retn": retn, "prologue": b.hex(' '), "procs": sorted(v['procs'])})

json.dump(out, open(os.path.join(D, 'eos_sites.json'), 'w'), indent=1)
print("\necrit : eos_sites.json")
