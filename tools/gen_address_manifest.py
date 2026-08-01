# Génère docs/address_manifest.md : l'inventaire EXHAUSTIF des adresses client
# en dur dans src/, trié par adresse — la check-list du portage vers un exe plus
# récent (cf. project_address_directory).
#
#   python tools/gen_address_manifest.py
#
# Pourquoi un manifeste plutôt qu'un « addresses.h » unique : un en-tête inclus
# partout recompile tout le projet à chaque retouche, et arracherait chaque
# constante à son commentaire de contexte. L'annuaire (ragnarok/*.h) mutualise
# ce qui est PARTAGÉ ; ce manifeste rend TOUT retrouvable, y compris les
# adresses à un seul fichier et les littéraux au milieu d'une expression —
# le gisement invisible au grep qui a motivé l'annuaire.
import collections
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src")
OUT = os.path.join(os.path.dirname(ROOT), "docs", "address_manifest.md")

# Plage des adresses du client (base 0x400000, .data jusqu'à ~0x0160xxxx).
# En dessous : des offsets/ids ; au-dessus : des couleurs ou des tailles.
ADDR_MIN, ADDR_MAX = 0x00401000, 0x01700000

HEXLIT = re.compile(r"0x0?[0-9a-fA-F]{6,8}")
DECL = re.compile(r"constexpr\s+[\w:]+\s+(k\w+)\s*=")

# Les six en-têtes de l'annuaire : leurs entrées sont le POINT DE VÉRITÉ,
# marquées comme telles dans le manifeste.
ANNUAIRE = {
    os.path.join("ragnarok", h)
    for h in ("uiwnd.h", "globals.h", "item_db.h", "render.h", "lua.h")
} | {os.path.join("ui", "game_texture.h")}


def collect():
    # adresse -> [(fichier, ligne, nom, commentaire, annuaire?)]
    sites = collections.defaultdict(list)
    for dirpath, _, files in os.walk(ROOT):
        for fn in sorted(files):
            if not fn.endswith((".cc", ".h", ".cpp")):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, ROOT)
            with open(path, encoding="utf-8", errors="replace") as f:
                for lineno, line in enumerate(f, 1):
                    code, _, comment = line.partition("//")
                    matches = HEXLIT.findall(code)
                    if len(matches) >= 4:
                        # Ligne-palette (tableaux de couleurs RGB) : les teintes
                        # sombres tombent dans la plage des adresses .text.
                        continue
                    for m in HEXLIT.finditer(code):
                        value = int(m.group(0), 16)
                        if not ADDR_MIN <= value <= ADDR_MAX:
                            continue
                        decl = DECL.search(code)
                        name = decl.group(1) if decl else "(littéral)"
                        sites[value].append(
                            (rel, lineno, name, comment.strip()[:90], rel in ANNUAIRE)
                        )
    return sites


def write_manifest(sites):
    n_annuaire = sum(1 for occ in sites.values() if any(o[4] for o in occ))
    n_multi = sum(1 for occ in sites.values() if len({o[0] for o in occ}) > 1)
    with open(OUT, "w", encoding="utf-8", newline="\n") as out:
        out.write(
            "# Manifeste des adresses natives (client 20250716, base 0x400000)\n\n"
            "GÉNÉRÉ par `python tools/gen_address_manifest.py` — ne pas éditer à la main.\n"
            "C'est la check-list du portage : chaque adresse ci-dessous est à retrouver\n"
            "dans le nouvel exe, puis à corriger à CHAQUE site listé (un seul pour les\n"
            "entrées ⚓ : leurs déclinaisons passent par l'annuaire).\n\n"
            f"- **{len(sites)} adresses distinctes**, {sum(len(v) for v in sites.values())} sites\n"
            f"- ⚓ {n_annuaire} portées par l'annuaire (`ragnarok/*.h`, `ui/game_texture.h`)\n"
            f"- {n_multi} présentes dans plusieurs fichiers (candidates annuaire si cohérentes)\n\n"
            "| Adresse | Nom(s) | Sites | Commentaire |\n"
            "|---|---|---|---|\n"
        )
        for value in sorted(sites):
            occ = sites[value]
            names = sorted({o[2] for o in occ if o[2] != "(littéral)"}) or ["(littéral)"]
            anchor = "⚓ " if any(o[4] for o in occ) else ""
            locs = "<br>".join(f"`{f}:{ln}`" for f, ln, *_ in occ[:6])
            if len(occ) > 6:
                locs += f"<br>… +{len(occ) - 6}"
            comment = next((c for *_, c, _a in occ if c), "")
            comment = comment.replace("|", "\\|")
            out.write(
                f"| `0x{value:08x}` | {anchor}{', '.join(names)} | {locs} | {comment} |\n"
            )
    return len(sites), n_annuaire, n_multi


def main():
    sites = collect()
    total, n_annuaire, n_multi = write_manifest(sites)
    print(f"{OUT} : {total} adresses ({n_annuaire} annuaire, {n_multi} multi-fichiers)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
