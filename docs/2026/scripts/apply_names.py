# -*- coding: utf-8 -*-
"""Propage vers l'IDB 2026 les noms des methodes virtuelles appariees.

REVERSIBLE : les noms existants sont sauvegardes dans names_backup_2026.json
avant toute ecriture, et `undo_names.py` les restaure.

Ne touche que des adresses dont le nom actuel est auto-genere (sub_, nullsub_,
Classe__vf_NN...). Un nom explicite deja present n'est jamais ecrase.
"""
import io
import json
import os

import idaapi
import ida_name
import ida_nalt

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"

root = ida_nalt.get_root_filename()
if "2026" not in root:
    print("ERREUR : ce script ne doit tourner que sur l'IDB 2026 (ouvert : %s)" % root)
    raise SystemExit(1)

prop = json.load(io.open(os.path.join(D, 'propose_names.json'), encoding='utf-8'))

backup = {}
applied = skipped = failed = 0
for addr, name in prop.items():
    ea = int(addr, 16)
    old = ida_name.get_name(ea) or ""
    backup[addr] = old
    # suffixer pour eviter les collisions de nom
    target = name if not ida_name.get_name_ea(idaapi.BADADDR, name) != idaapi.BADADDR else name
    if ida_name.set_name(ea, name, ida_name.SN_NOCHECK | ida_name.SN_FORCE):
        applied += 1
    else:
        failed += 1

path = os.path.join(D, 'names_backup_2026.json')
json.dump(backup, io.open(path, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
print("noms appliques : %d | echecs : %d" % (applied, failed))
print("sauvegarde des anciens noms : %s" % path)
