# -*- coding: utf-8 -*-
"""Releve COMPLET des longueurs de paquets : tous les remplisseurs de la table.

Le releve du 2026-08-26 partait d'UNE fonction de remplissage. Mesure du
2026-08-27 : il y en a DEUX, et la seconde porte 202 entrees de plus.

On ne part donc pas d'une fonction mais des xrefs vers Insert : la question
« qui remplit la table » n'a pas a etre repondue d'avance.

Format de sortie, identique au releve precedent :
    { "<opcode decimal>": [len, len_bis, flag] }
"""
import json
import os

import idc
import idaapi
import idautils
import ida_funcs
from collections import Counter

# L'IDB courant decide de la sortie : le pont ne transmet pas d'environnement,
# et se tromper de fichier ecraserait le releve de l'autre build.
import ida_nalt
_mod = ida_nalt.get_root_filename() or '?'
LABEL = '2026-07-07' if '2026' in _mod else '2025-07-16'
OUT = (r'C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad/full_%s.json' % LABEL)
print('IDB : %s  ->  %s' % (_mod, LABEL))

target = idc.get_name_ea_simple("PacketLenTable_Insert")
if target == idaapi.BADADDR:
    raise SystemExit("PacketLenTable_Insert introuvable")

calls = [x.frm for x in idautils.XrefsTo(target) if x.type in (16, 17)]
per_func = Counter()
for c in calls:
    f = ida_funcs.get_func(c)
    per_func[f.start_ea if f else 0] += 1

print("%s : %d appels, %d remplisseur(s)" % (LABEL, len(calls), len(per_func)))
for ea, n in sorted(per_func.items(), key=lambda kv: -kv[1]):
    print("   0x%08X  %-26s %4d" % (ea, idc.get_func_name(ea) or '?', n))


def prev_pushes(call_ea, n=4):
    out = []
    ea = idc.prev_head(call_ea)
    while len(out) < n and ea != idaapi.BADADDR:
        m = idc.print_insn_mnem(ea)
        if m == 'push':
            if idc.get_operand_type(ea, 0) != idc.o_imm:
                return None
            out.append(idc.get_operand_value(ea, 0))
        elif m == 'mov':
            pass
        else:
            return None
        ea = idc.prev_head(ea)
    return out if len(out) == n else None


def norm(v):
    return -1 if v > 0x7FFFFFFF else v


table, bad, conflicts = {}, 0, []
for c in calls:
    p = prev_pushes(c)
    if p is None:
        bad += 1
        continue
    opcode, ln, ln_bis, flag = p
    key = str(opcode & 0xFFFF)
    val = [norm(ln), norm(ln_bis), norm(flag)]
    if key in table and table[key] != val:
        conflicts.append((key, table[key], val))
    table[key] = val

print("   entrees %d, illisibles %d, conflits %d" % (len(table), bad, len(conflicts)))
for k, a, b in conflicts[:10]:
    print("      opcode %s : %s puis %s" % (k, a, b))

if OUT:
    json.dump(table, open(OUT, 'w'), sort_keys=True)
    print("   ecrit : %s" % OUT)
