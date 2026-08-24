# -*- coding: utf-8 -*-
"""Fabrique les gabarits de traduction de `msgstringtable.csv` du client.

Le client range TOUS ses textes d'interface dans `data\\msgstringtable.csv` :
4360 entrees, deux colonnes en base64 (cle `MSI_*`, texte). Il ne parle qu'une
langue a la fois, et celle de Moonlight est l'anglais. Bourgeon court-circuite
cette table (`ragnarok/msgstring_override.{h,cc}`) pour rendre le jeu ENTIER
traduisible -- fenetres natives comprises, pas seulement les notres.

  python tools\\lang\\gen_msgstring.py --client "E:\\...\\Moonlight-Destiny"

Ecrit un gabarit par langue dans `tools/lang/` : cle `MSI_*`, valeur vide, et le
texte anglais en commentaire au-dessus. **Une valeur vide garde le texte du
client** -- un fichier a moitie traduit est donc sur a livrer, exactement comme
`en.yaml` / `es.yaml`.

🔴 POURQUOI LA CLE EST `MSI_*` ET NON L'ID NUMERIQUE. Les ids bougent d'une
version de client a l'autre, et se relevent mal : `MSI_OPTION_ESC` etait note
4240 dans notre doc alors qu'il vaut 4241 (4240 = `MSI_EXPANSION_MINIMAP`). Une
erreur d'un cran ne se voit pas -- elle affiche un autre texte, plausible. La
cle, elle, est lisible et se verifie a l'oeil. La correspondance cle -> id est
refaite AU DEMARRAGE depuis le csv du client, donc elle suit ses mises a jour
sans qu'on retouche quoi que ce soit.

⚠ LES SPECIFICATEURS `printf` SONT UN CONTRAT. 543 entrees en portent
(« Error Code - %08d », « %s a rejoint »). Une traduction qui en perd un, en
ajoute un ou en change l'ordre ne donne pas un texte fautif : elle donne un
CRASH, le client passant ses arguments a l'aveugle. Ce script les releve pour
chaque entree ; le chargeur C++ refait la verification au runtime et REJETTE la
traduction fautive au profit de l'original.
"""

import argparse
import base64
import io
import os
import re

# Les langues a produire. L'anglais n'y est PAS : c'est deja ce que le client
# affiche, donc le traduire reviendrait a recopier la colonne de droite.
LANGUES = [
    ("fr", "francais"),
    ("es", "espanol"),
]

# Un specificateur printf, tel que le client les emploie : %s %d %08d %.2f %%…
RX_FORMAT = re.compile(r"%(?:%|[-+ #0]*[0-9*]*(?:\.[0-9*]+)?(?:hh|h|ll|l|L|z|j|t)?[diouxXeEfgGaAcspn])")


def specificateurs(texte):
    """La SEQUENCE des specificateurs, `%%` exclu (il ne consomme rien)."""
    return [s for s in RX_FORMAT.findall(texte) if s != "%%"]


def lire_table(chemin_csv):
    """[(id, cle, texte)] — l'index de ligne EST l'id (ligne 1 = id 0)."""
    with io.open(chemin_csv, "rb") as f:
        brut = f.read()
    entrees = []
    for ident, ligne in enumerate(brut.split(b"\n")):
        ligne = ligne.rstrip(b"\r")
        if not ligne.strip():
            continue
        champs = ligne.split(b",")
        if len(champs) < 2:
            continue
        try:
            cle = base64.b64decode(champs[0]).decode("cp949", "replace")
            # ⚠ Une ligne porte TROIS colonnes (3632). On prend la deuxieme,
            # comme le client : les suivantes sont des champs annexes.
            texte = base64.b64decode(champs[1]).decode("cp949", "replace")
        except Exception:
            continue
        if not cle:
            continue
        entrees.append((ident, cle, texte))
    return entrees


def echapper(valeur):
    return valeur.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r")


def charger_existant(chemin):
    """Traductions deja faites, pour ne JAMAIS les ecraser en regenerant."""
    deja = {}
    if not os.path.exists(chemin):
        return deja
    with io.open(chemin, encoding="utf-8") as f:
        for ligne in f:
            ligne = ligne.rstrip("\r\n")
            if not ligne.startswith('"'):
                continue
            coupe = ligne.find('": "')
            if coupe < 0:
                continue
            cle = ligne[1:coupe]
            val = ligne[coupe + 4:]
            if val.endswith('"'):
                val = val[:-1]
            if val:
                deja[cle] = val
    return deja


def ecrire(chemin, langue, nom_langue, entrees):
    deja = charger_existant(chemin)
    lignes = [
        "# Bourgeon - traduction de la table de messages du CLIENT (%s).\n" % nom_langue,
        "#\n",
        "# Deployer dans : <dossier du jeu>\\SaveData\\lang\\msgstring.%s.yaml\n" % langue,
        "#\n",
        "# Cle   = la cle MSI_* du client. Valeur = la traduction.\n",
        "# Une valeur VIDE garde le texte d'origine : ce fichier est sur a livrer\n",
        "# a n'importe quel stade de traduction.\n",
        "#\n",
        "# 🔴 Les %s, %d et autres specificateurs doivent etre PRESERVES : meme\n",
        "# nombre, meme ordre, memes types. Le chargeur rejette une traduction qui\n",
        "# n'a pas la meme sequence que l'original, et journalise le refus -- une\n",
        "# ligne fautive afficherait n'importe quoi, ou ferait planter le client.\n",
        "#\n",
        "# Les espaces de DEBUT et de FIN, eux, n'ont pas a etre reproduits : le\n",
        "# chargeur recopie ceux de l'original, quoi que porte la traduction. Ils\n",
        "# font partie du contrat -- le client CONCATENE une partie de ces textes,\n",
        "# « Beloved » se prefixe au nom d'un oeuf de familier -- et un espace en fin\n",
        "# de chaine ne se voit pas, donc on ne compte pas dessus.\n",
        "#\n",
        "# Genere par tools/lang/gen_msgstring.py -- les traductions deja ecrites\n",
        "# sont conservees a la regeneration.\n",
        "\n",
    ]
    traduites = 0
    for ident, cle, texte in entrees:
        if not texte or texte == "NO MSG":
            continue  # le client n'a rien a cet id : rien a traduire
        val = deja.get(cle, "")
        if val:
            traduites += 1
        fmts = specificateurs(texte)
        lignes.append("# [%d] %s%s\n" % (ident, echapper(texte),
                                         ("   <<< formats : " + " ".join(fmts)) if fmts else ""))
        lignes.append('"%s": "%s"\n' % (cle, val))
    with io.open(chemin, "w", encoding="utf-8", newline="\r\n") as f:
        f.writelines(lignes)
    return traduites


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--client", required=True, help="dossier d'installation du client")
    args = ap.parse_args()

    csv = os.path.join(args.client, "data", "msgstringtable.csv")
    if not os.path.exists(csv):
        raise SystemExit("introuvable : %s" % csv)

    entrees = lire_table(csv)
    utiles = [e for e in entrees if e[2] and e[2] != "NO MSG"]
    avec_fmt = [e for e in utiles if specificateurs(e[2])]
    print("table  : %d entrees, %d avec du texte, %d avec des formats"
          % (len(entrees), len(utiles), len(avec_fmt)))

    ici = os.path.dirname(os.path.abspath(__file__))
    for code, nom in LANGUES:
        chemin = os.path.join(ici, "msgstring.%s.yaml" % code)
        n = ecrire(chemin, code, nom, entrees)
        print("  %-24s %d/%d deja traduites" % (os.path.basename(chemin), n, len(utiles)))


if __name__ == "__main__":
    main()
