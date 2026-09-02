# -*- coding: utf-8 -*-
r"""Derive la ZONE AU SOL de chaque sort depuis le skill_db du serveur.

Le client sait deja dessiner la zone d'un sort pendant son incantation (mecanisme
« ShowScale » de kRO, cf. project_skill_aoe_preview_showscale), mais il lui faut
la donnee AUX DEUX BOUTS, et kRO ne l'a renseignee que pour 51 sorts de boss :

  * cote SERVEUR, le drapeau `ShowScale` du skill_db decide si le paquet
    ZC_SKILL_SCALE (0x0A41) part ;
  * cote CLIENT, `SKILL_INFO_LIST[id].SkillScale[lv] = {x=,y=}` dit la TAILLE.

Un seul des deux ne produit rien -- et rien ne le signale. Ce script ecrit les
deux a partir de la MEME source, `db/pre-re/skill_db.yml`, pour qu'ils ne
puissent pas diverger.

D'ou vient la taille, et pourquoi c'est defendable :
  * `Unit.Layout: N` -> carre de `2N+1` cases. C'est `skill_init_unit_layout`
    (skill.cpp) qui construit ainsi ses cinq premiers layouts. Storm Gust a
    `Layout: 4`, donc 9x9 -- et Lord of Vermilion 11x11, Meteor Storm 7x7,
    Pneuma 3x3 : les valeurs derivees retombent sur ce que la communaute RO
    connait, ce qui vaut controle de la methode.
  * a defaut, `SplashArea: N` -> `2N+1`, pour les degats instantanes
    (Magnum Break, Hammerfall...).

Ce qui est ECARTE, et pourquoi :
  * les layouts DIRECTIONNELS (`Layout: -1` : Fire Wall, Ice Wall, Sanctuary,
    Grand Cross...) -- leur forme depend de l'orientation du lanceur, un carre
    centre mentirait ;
  * `SplashArea: -1`, qui veut dire « toute la carte » cote rAthena ;
  * les tailles de 1 case, qui n'ont rien a montrer ;
  * les sorts qui ne se visent NI au sol NI ne posent d'unites -- un carre sous
    un buff de groupe n'apprendrait rien a personne.

/!\ La table du client est COSMETIQUE : elle ne change pas d'un pouce ce que le
serveur touche reellement. Une divergence se verrait a l'oeil sans qu'aucun
message ne la signale ; c'est la raison d'etre de ce script.

Usage :  python tools/gen_skill_scale.py [--dry]
"""
import io
import os
import re
import sys

import yaml

RACINE_SERVEUR = r"D:\Mes documents\GitHub\moonlight"
RACINE_CLIENT = r"D:\Mes documents\GitHub\Moonlight-Client"
CLIENT_LIVE = r"E:\Nouveau dossier\Moonlight-Destiny"

SKILL_DB = os.path.join(RACINE_SERVEUR, "db", "pre-re", "skill_db.yml")
IMPORT_DB = os.path.join(RACINE_SERVEUR, "db", "import", "skill_db.yml")
LUB = os.path.join(RACINE_CLIENT, "data", "luafiles514", "lua files",
                   "skillinfoz", "skillinfolist.lub")
LUB_LIVE = os.path.join(CLIENT_LIVE, "data", "luafiles514", "lua files",
                        "skillinfoz", "skillinfolist.lub")

# Le bloc genere est encadre : on le remplace en entier a chaque passage plutot
# que d'essayer de reconnaitre les entrees une a une.
MARQUE_DEBUT = "# >>> bourgeon:showscale -- genere par tools/gen_skill_scale.py"
MARQUE_FIN = "# <<< bourgeon:showscale"


def _eol(src):
    """La fin de ligne MAJORITAIRE, pas la premiere rencontree.

    /!\\ `skillinfolist.lub` est MIXTE (16135 LF contre 1070 CRLF) : un test
    « CRLF present ? » y repond oui et fait ecrire des lignes qui ne
    ressemblent pas a leurs voisines.
    """
    crlf = src.count("\r\n")
    return "\r\n" if crlf * 2 > src.count("\n") else "\n"


def _par_niveau(valeur, cle):
    """Un champ du skill_db vaut soit un scalaire, soit une liste par niveau."""
    if isinstance(valeur, list):
        return {e["Level"]: e.get(cle) for e in valeur}
    return {0: valeur}


def zones(chemin):
    """{nom_aegis: (id, [taille par niveau, index 0 = niveau 1])}."""
    db = yaml.safe_load(io.open(chemin, encoding="utf-8"))
    out = {}
    for e in db["Body"]:
        nom = e.get("Name")
        unit = e.get("Unit") or {}
        layout, splash = unit.get("Layout"), e.get("SplashArea")

        if layout is not None:
            m = _par_niveau(layout, "Size")
            if any(v is not None and v < 0 for v in m.values()):
                continue  # directionnel : un carre centre mentirait
        elif splash is not None:
            m = _par_niveau(splash, "Area")
        else:
            continue

        # Ni vise au sol, ni poseur d'unites : rien a montrer.
        if e.get("TargetType") != "Ground" and not unit:
            continue

        tailles = []
        for lv in range(1, e.get("MaxLevel", 1) + 1):
            v = m.get(lv, m.get(0, 0))
            if v is None or v < 0:  # -1 = toute la carte
                v = 0
            tailles.append(2 * v + 1)
        if max(tailles) <= 1:
            continue
        out[nom] = (e["Id"], tailles)
    return out


def ecrire_lub(chemin, table, dry):
    """Insere `SkillScale` dans chaque bloc `[SKID.NOM] = { ... }` qui n'en a pas.

    /!\\ On ne touche JAMAIS a un bloc qui en porte deja un : les 51 entrees de
    kRO font autorite sur leurs propres sorts.
    """
    with io.open(chemin, "r", encoding="latin-1", newline="") as f:
        src = f.read()
    eol = _eol(src)

    poses, deja = 0, 0
    for nom, (_id, tailles) in sorted(table.items()):
        tete = "\t[SKID.%s] = {" % nom
        i = src.find(tete)
        if i < 0:
            continue  # sort absent du lua du client (classe non livree)

        # Fin du bloc : par comptage d'accolades, `_NeedSkillList` en imbrique.
        j, prof = i + len(tete), 1
        while j < len(src) and prof:
            if src[j] == "{":
                prof += 1
            elif src[j] == "}":
                prof -= 1
            j += 1
        bloc = src[i:j]
        if "SkillScale" in bloc:
            deja += 1
            continue

        lignes = ["\t\tSkillScale = {"]
        for lv, t in enumerate(tailles, 1):
            virgule = "," if lv < len(tailles) else ""
            lignes.append("\t\t\t[%d] = { x = %d, y = %d }%s" % (lv, t, t, virgule))
        lignes.append("\t\t}")
        ajout = eol.join(lignes) + "," + eol

        # Devant l'accolade fermante, en gardant son indentation.
        k = src.rfind(eol, i, j - 1) + len(eol)

        # /!\ Le champ qui precede n'a pas forcement de virgule : quand le bloc
        # se termine par `_NeedSkillList = { ... }`, sa derniere accolade est nue,
        # et y coller un champ de plus donne un fichier Lua qui ne se charge pas.
        # Rien ne le signale cote client : SKILL_INFO_LIST reste simplement vide.
        avant = src[i:k].rstrip()
        if avant and avant[-1] not in ",{":
            pos = i + len(avant)
            src = src[:pos] + "," + src[pos:]
            k += 1

        src = src[:k] + ajout + src[k:]
        poses += 1

    print("  lub : %d entrees posees, %d deja renseignees par kRO" % (poses, deja))
    if dry:
        return
    tmp = chemin + ".tmp"
    with io.open(tmp, "w", encoding="latin-1", newline="") as f:
        f.write(src)
    os.replace(tmp, chemin)


def ecrire_import(chemin, table, dry):
    """Pose (ou remplace) le bloc `ShowScale: true` du skill_db d'import.

    Surcharge sure : l'entree existe deja dans le db de base, donc seul `Id` est
    requis, et `SkillDatabase::parseBodyNode` FUSIONNE les drapeaux un a un --
    les autres flags du sort ne sont pas perdus.
    """
    with io.open(chemin, "r", encoding="utf-8", newline="") as f:
        src = f.read()
    eol = _eol(src)

    # L'essai manuel du 2026-09-02, avant ce script : il est regenere ci-dessous.
    essai = eol.join(["  - Id: 89", "    Name: WZ_STORMGUST", "    Flags:",
                      "      ShowScale: true"]) + eol
    src = src.replace(essai, "")

    src = re.sub(re.escape(MARQUE_DEBUT) + ".*?" + re.escape(MARQUE_FIN) + r"\r?\n",
                 "", src, flags=re.S)
    if not src.endswith(eol):
        src += eol

    lignes = [MARQUE_DEBUT, "#     %d sorts ; ne pas editer a la main." % len(table)]
    for nom, (sid, _t) in sorted(table.items(), key=lambda kv: kv[1][0]):
        lignes += ["  - Id: %d" % sid, "    Name: %s" % nom, "    Flags:",
                   "      ShowScale: true"]
    lignes.append(MARQUE_FIN)
    src += eol.join(lignes) + eol

    print("  skill_db import : %d sorts marques ShowScale" % len(table))
    if dry:
        return
    tmp = chemin + ".tmp"
    with io.open(tmp, "w", encoding="utf-8", newline="") as f:
        f.write(src)
    os.replace(tmp, chemin)


def main():
    dry = "--dry" in sys.argv
    table = zones(SKILL_DB)
    print("%d sorts avec une zone derivable%s" % (len(table), "  (essai a blanc)" if dry else ""))
    ecrire_lub(LUB, table, dry)
    ecrire_import(IMPORT_DB, table, dry)
    if not dry and os.path.isfile(LUB_LIVE):
        import shutil
        shutil.copyfile(LUB, LUB_LIVE)
        print("  copie dans le client de test")


if __name__ == "__main__":
    main()
