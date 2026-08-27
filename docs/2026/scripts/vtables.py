# -*- coding: utf-8 -*-
"""Releve TOUTES les vtables nommees par le RTTI, et leurs slots.

Le portage initial n'a tire que 67 correspondances du RTTI alors que le binaire
compte plus de 1300 vtables. Une methode virtuelle se retrouve pourtant par
(classe, index de slot) sans dependre d'un seul octet -- c'est le vecteur le plus
robuste qui soit entre deux builds.
"""
import json
import os

import idaapi
import idautils
import ida_bytes
import ida_name
import ida_nalt
import ida_segment

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

root = ida_nalt.get_root_filename()
tag = "2026" if "2026" in root else "2025"

# bornes du segment de code, pour valider les slots
TEXT = None
for i in range(ida_segment.get_segm_qty()):
    s = ida_segment.getnseg(i)
    if s is not None and ida_segment.get_segm_name(s) == '.text':
        TEXT = (s.start_ea, s.end_ea)
        break


def is_code(ea):
    return TEXT and TEXT[0] <= ea < TEXT[1]


vt = {}
for ea, name in idautils.Names():
    if not name.startswith('??_7'):
        continue
    # ??_7CFoo@@6B@  ->  CFoo
    cls = name[4:]
    for sep in ('@@6B', '@@6'):
        if sep in cls:
            cls = cls.split(sep)[0]
            break
    slots = []
    p = ea
    while True:
        v = ida_bytes.get_dword(p)
        if not is_code(v):
            break
        # une autre vtable commence ici ?
        if p != ea and ida_name.get_name(p).startswith('??_7'):
            break
        slots.append(v)
        p += 4
        if len(slots) > 400:
            break
    if slots:
        vt.setdefault(cls, []).append({"ea": "0x%08x" % ea,
                                       "n": len(slots),
                                       "slots": ["0x%08x" % s for s in slots]})

nvt = sum(len(v) for v in vt.values())
nslots = sum(x['n'] for v in vt.values() for x in v)
print("%s : %d classes, %d vtables, %d slots au total" % (tag, len(vt), nvt, nslots))
multi = {k: v for k, v in vt.items() if len(v) > 1}
print("  classes a plusieurs vtables (heritage multiple) : %d" % len(multi))

path = os.path.join(D, 'vtables_%s.json' % tag)
json.dump({"tag": tag, "vtables": vt}, open(path, 'w'))
print("ecrit : %s" % path)
