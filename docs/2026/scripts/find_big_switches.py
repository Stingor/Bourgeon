# -*- coding: utf-8 -*-
"""Liste les fonctions contenant un GROS switch : candidats 'fabrique' ou
'dispatcher' indexes par un identifiant stable (id de fenetre, opcode, ...)."""
import idaapi
import idautils
import idc
import ida_name
import ida_nalt

MIN_CASES = 80

rows = []
for fea in idautils.Functions():
    f = idaapi.get_func(fea)
    if f is None or (f.end_ea - f.start_ea) < 0x400:
        continue
    best = 0
    bs = None
    for head in idautils.Heads(f.start_ea, f.end_ea):
        si = idaapi.get_switch_info(head)
        if si and si.ncases > best:
            best = si.ncases
            bs = si
    if best >= MIN_CASES:
        rows.append((best, fea, f.end_ea - f.start_ea,
                     ida_name.get_name(fea) or "", bs.lowcase))

rows.sort(reverse=True)
print("=== %s ===" % ida_nalt.get_root_filename())
print("%-7s %-11s %-9s %-6s %s" % ("cases", "adresse", "taille", "low", "nom"))
for ncases, fea, size, name, low in rows[:25]:
    print("%-7d 0x%08X  0x%-7X %-6d %s" % (ncases, fea, size, low, name))
print("total : %d fonctions avec un switch >= %d cases" % (len(rows), MIN_CASES))
