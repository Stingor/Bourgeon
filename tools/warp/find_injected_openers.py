# -*- coding: utf-8 -*-
"""WARP ajoute-t-il un ouvreur de fenetre que l'IDB ne peut pas voir ?

L'IDB decrit l'exe VANILLA ; le client livre est patche par WARP, qui injecte du
code dans la section `.xdiff` (nulle dans le vanilla) et detourne des branches de
`.text`. On scanne les DEUX binaires de la meme facon et on ne garde que l'ecart.

🔴 Le script imprime aussi son TEMOIN POSITIF : combien d'appels il voit dans le
`.xdiff` de chaque binaire. Sans ce chiffre, un resultat vide ne prouverait rien
(cf. feedback_absence_needs_measurement) -- il pourrait tout aussi bien signifier
que le scanner ne regarde pas au bon endroit.

Resultat au 2026-09-04 : aucun appel a MakeWindow n'est ajoute, retire, ni ne
change d'identifiant. Voir docs/local_openers_re.md §5.
"""
import struct
import sys

VANILLA = r"E:\Nouveau dossier\2025-07-16_Ragexe_175220998_clientinfo.exe"
LIVRE = r"E:\Nouveau dossier\Moonlight-Destiny\Moonlight-Destiny.exe"
IMGBASE = 0x400000
SECS = (".text", ".xdiff")

CIBLES = {
    0xA39340: "MakeWindow",
    0xA2E770: "SaveRectAndCloseWindow",
    0xA47B90: "FindWindow",
    0xA451E0: "DispatchHotkeyBehavior",
    0xA32C10: "UserHotkey_ResolveBehavior",
}


def sections(data):
    e = struct.unpack_from("<I", data, 0x3C)[0]
    n = struct.unpack_from("<H", data, e + 6)[0]
    opt = struct.unpack_from("<H", data, e + 20)[0]
    tbl = e + 24 + opt
    out = []
    for i in range(n):
        o = tbl + i * 40
        nom = data[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, o + 8)
        out.append((nom, vaddr, vsize, raddr, rsize))
    return out


def scan(path):
    """{VA de l'appel: (fonction visee, id pousse, section)} + le temoin."""
    data = open(path, "rb").read()
    hits, temoin = {}, {}
    for nom, va, vs, ra, rs in sections(data):
        if nom not in SECS:
            continue
        base = IMGBASE + va
        n_appels = 0
        for o in range(ra, ra + rs - 5):
            if data[o] != 0xE8:
                continue
            n_appels += 1
            rel = struct.unpack_from("<i", data, o + 1)[0]
            src = base + (o - ra)
            cible = (src + 5 + rel) & 0xFFFFFFFF
            if cible not in CIBLES:
                continue
            # l'identifiant de fenetre est le dernier push litteral avant l'appel
            wid = None
            for recul in range(1, 25):
                p = o - recul
                if p < ra:
                    break
                if data[p] == 0x68 and recul >= 5:
                    wid = struct.unpack_from("<I", data, p + 1)[0]
                    break
                if data[p] == 0x6A and recul >= 2:
                    wid = data[p + 1]
                    break
            hits[src] = (CIBLES[cible], wid, nom)
        temoin[nom] = (n_appels, sum(1 for i in range(ra, ra + rs) if data[i]))
    return hits, temoin


def main():
    a, ta = scan(VANILLA)
    b, tb = scan(LIVRE)

    print("TEMOIN POSITIF (le scanner voit-il le code injecte ?)")
    for nom in SECS:
        print("  %-8s vanilla %6d appels / %8d octets non nuls"
              "   |   livre %6d appels / %8d octets non nuls"
              % (nom, ta[nom][0], ta[nom][1], tb[nom][0], tb[nom][1]))

    for titre, ens in (("PRESENTS SEULEMENT dans l'exe livre", sorted(set(b) - set(a))),
                       ("disparus de l'exe livre", sorted(set(a) - set(b)))):
        src = b if "livre" in titre and "SEULEMENT" in titre else a
        print("\n=== %d appels %s ===" % (len(ens), titre))
        for va in ens:
            f, wid, sec = src[va]
            print("  %08X  [%-6s]  %-26s id=%s" % (va, sec, f, wid))

    changes = sorted(k for k in set(a) & set(b) if a[k] != b[k])
    print("\n=== %d appels dont l'identifiant pousse a CHANGE ===" % len(changes))
    for va in changes:
        print("  %08X  vanilla=%s  livre=%s" % (va, a[va], b[va]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
