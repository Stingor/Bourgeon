"""Efface ASLR (DYNAMIC_BASE) et CFG (GUARD_CF) d'un exe Windows 32 bits.

À lancer APRÈS la génération par WARP : l'outil ne patche que les sections, or
ces deux drapeaux vivent dans l'en-tête PE, avant la première section.

    python fix_aslr.py "E:/Nouveau dossier/Moonlight-Destiny/2026-07-07_Ragexe_patched.exe"

Sans argument, agit sur l'exe patché par défaut. Ajouter --check pour seulement
lire l'état sans rien écrire.

Pourquoi : les clients jusqu'à 2025-07-16 étaient liés sans /DYNAMICBASE, donc
l'image se chargeait toujours à 0x400000 et une adresse lue dans IDA était
l'adresse en mémoire. Les clients 2026 ajoutent ASLR (l'image bouge à chaque
lancement) et Control Flow Guard (les appels indirects sont validés contre une
table, ce qui fait rejeter une entrée de vtable détournée).
"""

import os
import struct
import sys

DEFAULT = r"E:/Nouveau dossier/Moonlight-Destiny/2026-07-07_Ragexe_patched.exe"

DYNAMIC_BASE = 0x0040
GUARD_CF = 0x4000

FLAG_NAMES = [
    (0x0020, "HIGH_ENTROPY_VA"),
    (DYNAMIC_BASE, "DYNAMIC_BASE (ASLR)"),
    (0x0080, "FORCE_INTEGRITY"),
    (0x0100, "NX_COMPAT"),
    (0x0200, "NO_ISOLATION"),
    (0x0400, "NO_SEH"),
    (0x0800, "NO_BIND"),
    (0x2000, "WDM_DRIVER"),
    (GUARD_CF, "GUARD_CF"),
    (0x8000, "TERMINAL_SERVER_AWARE"),
]


def describe(value):
    hits = [name for bit, name in FLAG_NAMES if value & bit]
    return ", ".join(hits) if hits else "(aucun)"


def dll_characteristics_offset(data):
    """Offset physique de IMAGE_OPTIONAL_HEADER.DllCharacteristics."""
    if data[:2] != b"MZ":
        raise SystemExit("ce n'est pas un exécutable (signature MZ absente)")

    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe <= 0 or pe + 96 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("en-tête PE introuvable")

    magic = struct.unpack_from("<H", data, pe + 24)[0]
    if magic != 0x10B:
        raise SystemExit("seuls les binaires 32 bits (PE32) sont gérés")

    # PE(4) + IMAGE_FILE_HEADER(20) + 70 dans l'optional header
    return pe + 4 + 20 + 70


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    check_only = "--check" in sys.argv
    path = args[0] if args else DEFAULT

    if not os.path.isfile(path):
        raise SystemExit("fichier introuvable : %s" % path)

    data = bytearray(open(path, "rb").read())
    off = dll_characteristics_offset(data)
    before = struct.unpack_from("<H", data, off)[0]

    print("fichier : %s" % path)
    print("  DllCharacteristics = 0x%04X  ->  %s" % (before, describe(before)))

    after = before & ~DYNAMIC_BASE & ~GUARD_CF & 0xFFFF
    if after == before:
        print("  ni ASLR ni CFG : rien à faire")
        return

    if check_only:
        print("  --check : deviendrait 0x%04X  ->  %s" % (after, describe(after)))
        return

    backup = path + ".aslr.bak"
    if not os.path.exists(backup):
        open(backup, "wb").write(bytes(data))
        print("  sauvegarde -> %s" % os.path.basename(backup))

    struct.pack_into("<H", data, off, after)

    # Écriture sur place : pas de fichier temporaire à renommer, et un verrou
    # donne alors une erreur nette plutôt qu'un échec au moment du remplacement.
    try:
        with open(path, "r+b") as fp:
            fp.seek(off)
            fp.write(struct.pack("<H", after))
    except PermissionError:
        raise SystemExit(
            "ACCÈS REFUSÉ : le fichier est verrouillé.\n"
            "  Ferme x32dbg (ou détache le processus) et le client, puis relance.\n"
            "  Rien n'a été modifié."
        )

    check = struct.unpack_from("<H", open(path, "rb").read(), off)[0]
    if check != after:
        raise SystemExit("ÉCHEC : relu 0x%04X au lieu de 0x%04X" % (check, after))

    print("  écrit 0x%04X  ->  %s" % (after, describe(after)))
    print("  l'image se chargera de nouveau à son ImageBase fixe.")


if __name__ == "__main__":
    main()
