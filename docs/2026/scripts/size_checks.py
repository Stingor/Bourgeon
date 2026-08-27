# -*- coding: utf-8 -*-
"""Recense les controles de taille de paquet du client, et leur diviseur.

Deux blocages ont deja ete causes par un desaccord d'en-tete ou de taille
d'element entre rAthena et ce client. Plutot que d'attendre le suivant, on
releve TOUS les endroits ou le client divise une longueur de paquet, et on
compare 2025 / 2026.

Motif recherche : `movzx r, word ptr [g_packetLength]` puis `sub r, N`, suivi
d'une division par une constante (multiplication magique ou div).
"""
import json
import os
import re

import idaapi
import idautils
import ida_bytes
import ida_nalt

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

root = ida_nalt.get_root_filename()
tag = "2026" if "2026" in root else "2025"

# --- 1. les chaines d'erreur de parsing --------------------------------------
PATS = [b"size error", b"Cnt Over", b"mismatch", b"size mismatch", b"Unknown Packet"]
found = []
for seg_name in ('.rdata', '.data'):
    import ida_segment
    for i in range(ida_segment.get_segm_qty()):
        s = ida_segment.getnseg(i)
        if s is None or ida_segment.get_segm_name(s) != seg_name:
            continue
        data = ida_bytes.get_bytes(s.start_ea, s.end_ea - s.start_ea) or b''
        for pat in PATS:
            for m in re.finditer(re.escape(pat), data):
                ea = s.start_ea + m.start()
                # remonter au debut de la chaine
                st = ea
                while st > s.start_ea and data[st - s.start_ea - 1] not in (0,):
                    st -= 1
                txt = ida_bytes.get_strlit_contents(st, -1, 0)
                if txt:
                    found.append((st, txt.decode('latin-1', 'replace')[:90]))

seen = set()
uniq = []
for ea, txt in found:
    if ea in seen:
        continue
    seen.add(ea)
    uniq.append((ea, txt))

print("=== %s : %d chaines d'erreur de parsing ===" % (tag, len(uniq)))
out = []
for ea, txt in sorted(uniq):
    refs = [x.frm for x in idautils.XrefsTo(ea)]
    fns = []
    for r in refs:
        f = idaapi.get_func(r)
        fns.append("0x%08x" % (f.start_ea if f else r))
    print("  0x%08X  %-62s  %s" % (ea, txt, ",".join(sorted(set(fns))) or "(pas de xref)"))
    out.append({"ea": "0x%08x" % ea, "text": txt, "fns": sorted(set(fns))})

json.dump({"tag": tag, "errors": out}, open(os.path.join(D, 'size_checks_%s.json' % tag), 'w'),
          indent=1)
print("\necrit : size_checks_%s.json" % tag)
