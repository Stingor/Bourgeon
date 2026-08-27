# -*- coding: utf-8 -*-
"""Restaure les noms de l'IDB 2026 tels qu'ils etaient avant apply_names.py."""
import io
import json
import os

import ida_name
import ida_nalt

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

if "2026" not in ida_nalt.get_root_filename():
    print("ERREUR : a lancer sur l'IDB 2026 uniquement")
    raise SystemExit(1)

backup = json.load(io.open(os.path.join(D, 'names_backup_2026.json'), encoding='utf-8'))
n = 0
for addr, old in backup.items():
    ea = int(addr, 16)
    # un nom vide remet le nom auto-genere
    if ida_name.set_name(ea, old, ida_name.SN_NOCHECK | ida_name.SN_FORCE):
        n += 1
print("noms restaures : %d / %d" % (n, len(backup)))
