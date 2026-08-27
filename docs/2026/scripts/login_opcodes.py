# -*- coding: utf-8 -*-
"""Liste les opcodes traites par le dispatch LOGIN/CHAR (pas celui du jeu).

2025 : LoginCharMode_RecvDispatch 0x00D27560
2026 : sub_C3A670                 0x00C3A670

Le serveur envoie ZC_ACCEPT_LOGIN ; si le client 2026 n'ecoute plus le meme
opcode, la liste des char-servers reste vide alors que le login a reussi.
"""
import json
import os

import idaapi
import idautils
import ida_nalt

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

root = ida_nalt.get_root_filename()
tag = "2026" if "2026" in root else "2025"
EA = 0x00C3A670 if tag == "2026" else 0x00D27560

f = idaapi.get_func(EA)
print("%s : dispatch login 0x%08X (0x%X octets)" % (tag, EA, f.end_ea - f.start_ea))

ops = {}
nsw = 0
for head in idautils.Heads(f.start_ea, f.end_ea):
    si = idaapi.get_switch_info(head)
    if not si:
        continue
    nsw += 1
    try:
        ct = idaapi.calc_switch_cases(head, si)
    except Exception:
        continue
    for i in range(len(ct.cases)):
        tgt = ct.targets[i]
        for v in ct.cases[i]:
            ops.setdefault(v, "0x%08x" % tgt)

print("  %d switch(s), %d opcodes traites" % (nsw, len(ops)))

# opcodes d'interet : ZC_ACCEPT_LOGIN et variantes
INTERET = {105: "0x0069 ZC_ACCEPT_LOGIN", 2756: "0x0AC4", 2761: "0x0AC9",
           2823: "0x0B07", 276: "0x0114", 2438: "0x0986", 2088: "0x0828"}
print("\n  opcodes d'acceptation de login :")
for v, nom in sorted(INTERET.items()):
    print("     %-24s %s" % (nom, ("case present -> " + ops[v]) if v in ops else "ABSENT"))

path = os.path.join(D, 'login_ops_%s.json' % tag)
json.dump({"tag": tag, "func": "0x%08x" % EA, "ops": {str(k): v for k, v in ops.items()}},
          open(path, 'w'), indent=1)
print("\necrit : %s" % path)
