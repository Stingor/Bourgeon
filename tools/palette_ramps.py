# -*- coding: utf-8 -*-
"""Détection des rampes d'une palette de corps + application d'une recette HSV.

Ce script est la RÉFÉRENCE de l'éditeur de palette in-game : l'algorithme qu'il
implémente doit être transcrit tel quel en C++, parce que la « recette » envoyée
au serveur ne porte PAS de couleurs — elle porte des plages d'index et un
décalage HSV, que chaque client ré-applique sur la palette interne du `.spr`
qu'il possède déjà. Deux clients doivent donc trouver EXACTEMENT les mêmes
rampes.

Rappels de format (mesurés, cf. mémoire projet) :
* la palette d'un `.spr` = ses 1024 DERNIERS octets, en RGBA ;
* un `.pal` externe = ses 1024 PREMIERS octets (les fichiers de 1028 ont une
  QUEUE de 4 zéros, pas un en-tête) ;
* l'index 0 est le transparent, il ne se colorie jamais.

Ce que le jeu fait, dans l'ordre, et que ce script reproduit :
    1. lire la palette interne du `.spr` du corps ;
    2. FUSIONNER par-dessus celle du serveur (`--pal`), en ne rapatriant du
       sprite que les plages entièrement noires — voir `fusionner_serveur` ;
    3. détecter les rampes sur le RÉSULTAT de la fusion, pas sur le sprite nu ;
    4. appliquer la recette.

Usage :
    python tools/palette_ramps.py --lister
    python tools/palette_ramps.py --corps dragon_knight --sexe m
    python tools/palette_ramps.py --corps dragon_knight --sexe m \
        --pal "data\\palette\\body\\body_56.pal" \
        --recette "0:+120,-10,+0" --png sortie.png

Une recette s'écrit `n:teinte,sat,lum`, plusieurs séparées par « ; ». Un « = »
en tête passe la rampe en mode ABSOLU — la teinte et la saturation sont alors
imposées (0..359 et 0..100) au lieu d'être des décalages :
    --recette "0:=300,100,+0;1:-40,+20,-10"
"""

import argparse
import collections
import colorsys
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from grf_reader import Vfs                                    # noqa: E402

CLIENT_DEFAUT = r"E:\Nouveau dossier\Moonlight-Destiny"

# 🔴 EXACTEMENT ce que `DATA.INI` déclare, dans son ordre — rien d'autre.
# `palettes.grf` traîne dans le dossier du client mais n'y est PAS listé : le jeu
# ne le lit jamais, et s'en servir donnerait des mesures sur des fichiers qui ne
# sont pas ceux du jeu (erreur commise le 2026-08-11).
# `data.grf` est chiffré (« Event Horizon ») : le VFS l'ignore tout seul, donc
# les sprites doivent être EXTRAITS sur disque — le VFS lit disque d'abord.
GRFS = ("moonlight.grf", "data.grf")

RACE_HUMAIN = "\uc778\uac04\uc871"   # 인간족
DOSSIER_CORPS = "\ubab8\ud1b5"       # 몸통
SEXE_M = "\ub0a8"                    # 남
SEXE_F = "\uc5ec"                    # 여

# ---------------------------------------------------------------------------
# Réglages de la détection. Ce sont EUX qui doivent être identiques côté C++.
# ---------------------------------------------------------------------------
# 🔴 UN seul index suffit. Mesuré sur les 421 corps : beaucoup de palettes sont
# ÉPARSES et peignent des aplats de une ou deux couleurs, que le seuil de 3
# écartait alors qu'ils couvrent de vraies pièces du costume.
#   seuil 3 : 85,7 % des pixels réglables en moyenne, 36 corps sous 70 %
#   seuil 2 : 87,7 %,                                 14 corps sous 70 %
#   seuil 1 : 91,0 %,                                  9 corps sous 70 %
# Le vrai filtre anti-bruit est PIXELS_MIN, pas la longueur.
LONGUEUR_MIN = 1
TOL_TEINTE = 40.0       # écart de teinte toléré dans une rampe, en degrés
TOL_REMONTEE = 0.08     # remontée de luminosité tolérée avant de couper
SAT_NEUTRE = 0.12       # en dessous, la teinte n'a plus de sens (gris)
PIXELS_MIN = 200        # une rampe qui couvre moins que ça ne vaut pas un curseur
# Le coude de la courbe de rendement, mesuré sur les 421 corps : 4 → 71 % des
# pixels couverts, 6 → 81 %, 8 → 86 %, 10 → 88 %, 12 → 89 %. Au-delà on paie des
# curseurs pour des broutilles.
# 🔴 Doit valoir `ro::kMaxRamps` : cette valeur dimensionne la recette réseau.
RAMPES_MAX = 8

# Somme R+V+B en dessous de laquelle une entrée compte comme « noire ». Bas
# exprès (24 sur 765) : seulement ce qui est indistinguable du noir à l'œil.
NOIR_SOMME = 24


def _octet(x):
    """Flottant 0..1 → octet, arrondi COMME LE C++.

    🔴 `round()` de Python fait de l'arrondi bancaire (`round(0.5) == 0`), là où
    `static_cast<int>(x * 255.0 + 0.5)` arrondit toujours au-dessus. Utiliser
    `round` ici ferait diverger d'un pas la moitié des entrées de palette entre
    l'outil de mesure et le jeu — et, le jour où deux clients ne partageront
    plus la même implémentation, entre deux joueurs.
    """
    n = int(x * 255.0 + 0.5)
    return 0 if n < 0 else (255 if n > 255 else n)


# ---------------------------------------------------------------------------
# Lecture .spr — strictement ce dont on a besoin : les images INDEXÉES (pour
# compter l'usage réel des index) et la palette.
# ---------------------------------------------------------------------------
def lire_spr(octets):
    if octets[:2] != b"SP":
        raise ValueError("signature 'SP' absente")
    mineur, majeur = octets[2], octets[3]
    version = majeur * 10 + mineur

    if version >= 32:
        n_indexe = struct.unpack_from("<H", octets, 4)[0]
        pos = 8
    elif version >= 20:
        n_indexe = struct.unpack_from("<H", octets, 4)[0]
        pos = 8
    else:
        n_indexe = struct.unpack_from("<H", octets, 4)[0]
        pos = 6

    images = []
    for _ in range(n_indexe):
        largeur, hauteur = struct.unpack_from("<HH", octets, pos)
        pos += 4
        if version >= 21:
            taille = struct.unpack_from("<H", octets, pos)[0]
            pos += 2
            brut = octets[pos:pos + taille]
            pos += taille
            # RLE : un octet nul est suivi du NOMBRE de transparents à poser.
            pixels = bytearray()
            i = 0
            while i < len(brut):
                c = brut[i]
                i += 1
                if c:
                    pixels.append(c)
                else:
                    n = brut[i] if i < len(brut) else 0
                    i += 1
                    pixels.extend(b"\x00" * n)
        else:
            taille = largeur * hauteur
            pixels = bytearray(octets[pos:pos + taille])
            pos += taille
        images.append((largeur, hauteur, bytes(pixels)))

    palette = octets[-1024:]
    return images, palette


def compter_usage(images):
    """Nombre de pixels par index, toutes images confondues.

    Counter sur des `bytes` compte en C : sur 400 corps de ~200 images, une
    boucle Python coûterait des minutes.
    """
    total = collections.Counter()
    for _, _, pixels in images:
        total.update(pixels)
    usage = [total.get(i, 0) for i in range(256)]
    usage[0] = 0  # l'index 0 est le transparent, il ne compte pas
    return usage


# ---------------------------------------------------------------------------
# Fusion sprite + serveur — transcription de `ro::MergeServerPalette`
# ---------------------------------------------------------------------------
def lire_pal(octets):
    """Les 1024 PREMIERS octets d'un `.pal` (un fichier de 1028 a une QUEUE)."""
    return octets[:1024] if octets and len(octets) >= 1024 else None


def _luma(pal, i):
    return pal[4 * i] + pal[4 * i + 1] + pal[4 * i + 2]


def _non_definie(pal, i):
    """Les QUATRE octets nuls : l'outil qui a produit le .pal n'a rien écrit là.

    C'est le séparateur naturel entre deux pièces du costume.
    """
    return _luma(pal, i) == 0 and pal[4 * i + 3] == 0


def fusionner_serveur(sprite, serveur):
    """Pose la palette du serveur sur celle du sprite, trous rapatriés.

    Aucune des deux ne suffit seule. Celle du serveur porte la couleur de
    vêtement que le joueur (et les autres) voient déjà, mais elle ne définit
    qu'une PARTIE des index : mesuré en mémoire vive sur body_56, 126 de ses 255
    entrées sont noires, et les corps de 4e classe utilisent massivement
    celles-là — d'où les silhouettes noires. Celle du sprite est complète mais
    ignore la teinte choisie.

    🔴 Le critère de « trou » n'est PAS « l'index est noir » : un noir peut être
    voulu, et c'est même le cas le plus courant, les sprites RO ayant des
    contours noirs. C'est la PLAGE qui décide — un dégradé qui part du clair et
    finit dans le noir est une ombre légitime, tandis qu'une plage noire d'un
    bout à l'autre n'est que du remplissage.

    Mesuré sur les 421 corps : sans fusion 20,04 % des pixels tombent sur un
    index noir, 2,24 % avec le critère naïf (quatre octets nuls), 1,26 % avec
    celui-ci — sans jamais toucher un contour.
    """
    sprite = sprite[:1024]
    if not serveur or len(serveur) < 1024:
        # Sans palette serveur, celle du sprite EST le résultat : c'est ce que
        # le client afficherait si aucune couleur de vêtement n'était posée.
        return bytes(sprite)

    # On part du serveur — c'est lui qui porte la couleur que le joueur voit —
    # puis on ne rapatrie du sprite que les trous.
    sortie = bytearray(serveur[:1024])
    i = 1  # l'index 0 est le transparent : il ne se peint jamais
    while i < 256:
        if _non_definie(serveur, i):
            sortie[4 * i:4 * i + 4] = sprite[4 * i:4 * i + 4]
            i += 1
            continue
        # Une plage définie va d'ici jusqu'au prochain séparateur.
        j, plus_clair = i, 0
        while j < 256 and not _non_definie(serveur, j):
            plus_clair = max(plus_clair, _luma(serveur, j))
            j += 1
        if plus_clair <= NOIR_SOMME:
            sortie[4 * i:4 * j] = sprite[4 * i:4 * j]
        i = j
    return bytes(sortie)


# ---------------------------------------------------------------------------
# Détection des rampes
# ---------------------------------------------------------------------------
def _hsv(palette, i):
    r, v, b = palette[4 * i], palette[4 * i + 1], palette[4 * i + 2]
    return colorsys.rgb_to_hsv(r / 255.0, v / 255.0, b / 255.0)


def _ecart_teinte(a, b):
    """Écart circulaire entre deux teintes, en degrés (0..180)."""
    d = abs(a - b) % 360.0
    return 360.0 - d if d > 180.0 else d


def _score(palette, usage, debut, fin, pixels):
    """Ce qui décide du RANG d'une rampe : sa surface, pondérée par sa franchise.

    `pixels * (256 + saturation moyenne)`, tout en ENTIERS — la clé de tri doit
    être bit à bit la même en Python et en C++, sinon les ex æquo se départagent
    autrement d'un client à l'autre et une recette désigne d'autres pièces.

    La saturation d'une entrée vaut `(max - min) * 255 // max`, la définition
    HSV ramenée aux entiers. Elle est moyennée en pondérant par les PIXELS : une
    couleur vive employée sur trois pixels ne doit pas colorer tout le verdict.

    Le facteur va donc de 256 (gris pur) à 511 (saturation maximale) : une pièce
    franche l'emporte sur un aplat terne jusqu'à 1,99 fois plus grand — assez
    pour remonter les rangs 9 mesurés, pas assez pour faire passer une paillette
    devant une tunique.
    """
    somme_sat = 0
    somme_px = 0
    for i in range(debut, fin):
        u = usage[i]
        if u == 0:
            continue
        r, g, b = palette[4 * i], palette[4 * i + 1], palette[4 * i + 2]
        haut = max(r, g, b)
        bas = min(r, g, b)
        sat = ((haut - bas) * 255) // haut if haut else 0
        somme_sat += sat * u
        somme_px += u
    moyenne = somme_sat // somme_px if somme_px else 0
    return pixels * (256 + moyenne)


def detecter_rampes(palette, usage):
    """Découpe la palette en dégradés contigus et cohérents.

    Un dégradé RO = une suite d'index consécutifs, de même teinte, dont la
    luminosité décroît (du clair vers le foncé). On n'impose PAS l'alignement
    sur 8 : il est vrai des corps classiques, faux des 4e classes.
    """
    rampes = []
    debut = None
    teinte_ref = None
    v_precedent = None

    def clore(fin):
        if debut is None:
            return
        longueur = fin - debut
        if longueur < LONGUEUR_MIN:
            return
        pixels = sum(usage[debut:fin])
        if pixels < PIXELS_MIN:
            return
        rampes.append({
            "debut": debut,
            "longueur": longueur,
            "pixels": pixels,
            "score": _score(palette, usage, debut, fin, pixels),
            "teinte": _teinte_dominante(palette, usage, debut, fin),
        })

    i = 1
    while i < 256:
        if usage[i] == 0:
            clore(i)
            debut, teinte_ref, v_precedent = None, None, None
            i += 1
            continue

        h, s, v = _hsv(palette, i)
        h_deg = h * 360.0

        if debut is None:
            debut, v_precedent = i, v
            teinte_ref = h_deg if s >= SAT_NEUTRE else None
            i += 1
            continue

        # Deux ruptures possibles, et il faut les DEUX : un dégradé RO va du
        # clair au foncé dans une teinte stable.
        rupture = False
        if s >= SAT_NEUTRE:
            if teinte_ref is None:
                teinte_ref = h_deg          # la rampe démarrait sur du gris
            elif _ecart_teinte(h_deg, teinte_ref) > TOL_TEINTE:
                rupture = True              # une autre pièce du costume
        if v > v_precedent + TOL_REMONTEE:
            rupture = True                  # retour au clair = nouvelle rampe

        if rupture:
            clore(i)
            debut = i
            teinte_ref = h_deg if s >= SAT_NEUTRE else None
        v_precedent = v
        i += 1

    clore(256)
    # Les plus VISIBLES d'abord : c'est l'ordre des curseurs dans l'éditeur, et
    # le plafond doit couper les rampes anecdotiques, pas la tunique.
    #
    # 🔴 « Visible » n'est PAS « grand ». Trier au seul nombre de pixels faisait
    # perdre les pièces petites mais FRANCHES — une rune rouge sur une armure,
    # un œil qui brille — contre de grands aplats ternes. Mesuré sur les 421
    # corps : 265 d'entre eux gardaient au moins une couleur saturée hors des
    # huit retenues, presque toujours au rang 9. Monter le plafond n'y répondait
    # pas (12 → encore 194 corps, 16 → encore 117) : c'est le CRITÈRE qui était
    # faux, pas le nombre.
    #
    # 🔴 Tri STABLE sur une clé ENTIÈRE. Un flottant dans une clé de tri, c'est
    # le piège d'arrondi déjà payé entre Python et C++ : deux implémentations
    # départageraient les ex æquo autrement, et la recette d'un joueur
    # désignerait d'autres pièces chez son voisin. `sorted` est stable, et
    # `std::stable_sort` de l'autre côté.
    rampes.sort(key=lambda r: r["score"], reverse=True)
    return rampes[:RAMPES_MAX]


def _teinte_dominante(palette, usage, debut, fin):
    """Teinte de l'index le plus SATURÉ de la rampe — pas la moyenne.

    Une moyenne circulaire sur des gris donne un angle arbitraire ; l'index le
    plus saturé, lui, porte la couleur que le joueur croit voir.
    """
    meilleur, sat_max = None, -1.0
    for i in range(debut, fin):
        if usage[i] == 0:
            continue
        h, s, _ = _hsv(palette, i)
        if s > sat_max:
            meilleur, sat_max = h * 360.0, s
    if meilleur is None or sat_max < SAT_NEUTRE:
        return None
    # Arrondi ENTIER comme le C++ (`PaletteRamp::hue` est un `int` en degrés) :
    # sans ça les deux implémentations rendraient des teintes différentes de la
    # même rampe, et la comparaison de leurs sorties ne prouverait plus rien.
    deg = int(meilleur + 0.5)
    return deg - 360 if deg >= 360 else deg


NOMS_TEINTE = [
    (15, "rouge"), (45, "orange"), (70, "jaune"), (160, "vert"),
    (200, "cyan"), (255, "bleu"), (290, "violet"), (330, "rose"),
    (360, "rouge"),
]


def nommer(rampe):
    if rampe["teinte"] is None:
        return "neutre"
    for borne, nom in NOMS_TEINTE:
        if rampe["teinte"] < borne:
            return nom
    return "rouge"


# ---------------------------------------------------------------------------
# Application d'une recette
# ---------------------------------------------------------------------------
def appliquer(palette, rampes, recette):
    """recette = {index_de_rampe: (teinte, sat, lum, absolu)}.

    Deux modes, exactement comme `ro::RampAdjust` :

    RELATIF (`absolu` faux) — la teinte PIVOTE, saturation et luminosité
    MULTIPLIENT. La structure du dégradé est intégralement préservée : ombres,
    reflets, écarts de saturation entre index. 🔴 Sa limite : la saturation
    étant un facteur, une rampe terne le reste. Sur une pièce beige, +100 % ne
    fait que doubler une saturation déjà faible, là où une pièce vive sature à
    fond. D'où le second mode.

    ABSOLU — teinte et saturation sont IMPOSÉES à toute la rampe (0..359 et
    0..100). C'est ce que fait une palette de vêtement officielle : une teinte
    unie que seule la lumière du sprite module. Toute pièce peut alors
    atteindre n'importe quelle couleur.

    Dans les DEUX modes la luminosité reste relative — c'est elle qui porte le
    modelé du dégradé, et l'imposer aplatirait le volume.
    """
    sortie = bytearray(palette)
    for n, rampe in enumerate(rampes):
        if n not in recette:
            continue
        teinte, sat, d_lum, absolu = recette[n]
        # Un réglage neutre est SAUTÉ, comme le fait `RampAdjust::IsNeutral()`.
        # L'aller-retour RGB→HSV→RGB se trouve être l'identité (vérifié), donc
        # l'appliquer ne changerait rien — mais la parité avec le jeu doit tenir
        # par construction, pas par une propriété numérique heureuse.
        if not absolu and teinte == 0 and sat == 0 and d_lum == 0:
            continue
        for i in range(rampe["debut"], rampe["debut"] + rampe["longueur"]):
            h, s, v = _hsv(palette, i)
            if absolu:
                h = teinte % 360.0
                s = sat / 100.0
            else:
                h = (h * 360.0 + teinte) % 360.0
                s = s * (1.0 + sat / 100.0)
            v = v * (1.0 + d_lum / 100.0)
            s = min(1.0, max(0.0, s))
            v = min(1.0, max(0.0, v))
            r, vert, b = colorsys.hsv_to_rgb(h / 360.0, s, v)
            sortie[4 * i] = _octet(r)
            sortie[4 * i + 1] = _octet(vert)
            sortie[4 * i + 2] = _octet(b)
            # L'octet d'alpha du fichier ne veut rien dire : laissé INTACT
            # plutôt que d'inventer une valeur.
    return bytes(sortie)


# ---------------------------------------------------------------------------
# Sortie PNG (sans dépendance : le format est trivial en RGB brut)
# ---------------------------------------------------------------------------
def ecrire_png(chemin, largeur, hauteur, rgb):
    def bloc(tag, donnees):
        entete = struct.pack(">I", len(donnees)) + tag
        return entete + donnees + struct.pack(">I", zlib.crc32(tag + donnees))

    lignes = bytearray()
    for y in range(hauteur):
        lignes.append(0)
        lignes.extend(rgb[y * largeur * 3:(y + 1) * largeur * 3])
    with open(chemin, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(bloc(b"IHDR", struct.pack(">IIBBBBB", largeur, hauteur, 8, 2, 0, 0, 0)))
        f.write(bloc(b"IDAT", zlib.compress(bytes(lignes), 9)))
        f.write(bloc(b"IEND", b""))


def rendre(image, palette, echelle=1):
    """Rend une image indexée en RGB, fond damier pour voir la transparence."""
    largeur, hauteur, pixels = image
    out = bytearray(largeur * echelle * hauteur * echelle * 3)
    for y in range(hauteur * echelle):
        for x in range(largeur * echelle):
            idx = pixels[(y // echelle) * largeur + (x // echelle)]
            base = (y * largeur * echelle + x) * 3
            if idx == 0:
                gris = 0xC0 if ((x // 8) + (y // 8)) % 2 else 0xE8
                out[base] = out[base + 1] = out[base + 2] = gris
            else:
                out[base] = palette[4 * idx]
                out[base + 1] = palette[4 * idx + 1]
                out[base + 2] = palette[4 * idx + 2]
    return bytes(out)


def cote_a_cote(image, palettes, echelle=2):
    """Assemble N rendus de la MÊME image sous N palettes, horizontalement."""
    largeur, hauteur, _ = image
    lp, hp = largeur * echelle, hauteur * echelle
    total = lp * len(palettes)
    out = bytearray(total * hp * 3)
    for n, pal in enumerate(palettes):
        rgb = rendre(image, pal, echelle)
        for y in range(hp):
            src = y * lp * 3
            dst = (y * total + n * lp) * 3
            out[dst:dst + lp * 3] = rgb[src:src + lp * 3]
    return total, hp, bytes(out)


# ---------------------------------------------------------------------------
def dossier_corps(sexe):
    return "data\\sprite\\%s\\%s\\%s" % (
        RACE_HUMAIN, DOSSIER_CORPS, SEXE_M if sexe == "m" else SEXE_F)


def chemin_corps(nom, sexe):
    suffixe = SEXE_M if sexe == "m" else SEXE_F
    return "%s\\%s_%s.spr" % (dossier_corps(sexe), nom, suffixe)


def main():
    global RAMPES_MAX
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    ap = argparse.ArgumentParser()
    ap.add_argument("--client", default=CLIENT_DEFAUT)
    ap.add_argument("--corps", help="nom de classe, ex. dragon_knight")
    ap.add_argument("--sexe", default="m", choices=["m", "f"])
    ap.add_argument("--lister", action="store_true",
                    help="détecte les rampes de TOUS les corps et résume")
    ap.add_argument("--pal", help="palette du serveur, ex. "
                                  "data\\palette\\body\\body_56.pal — fusionnée "
                                  "sur celle du sprite avant toute détection")
    ap.add_argument("--recette", default="",
                    help="n:teinte,sat,lum ; séparés par ';' ; un '=' en tête "
                         "des valeurs passe la rampe en mode ABSOLU")
    ap.add_argument("--png", help="fichier de comparaison avant/après")
    ap.add_argument("--image", type=int, default=0, help="image du .spr à rendre")
    ap.add_argument("--rampes-max", type=int, default=RAMPES_MAX,
                    help="plafond de rampes exposées (fixe la taille de la recette)")
    args = ap.parse_args()
    RAMPES_MAX = args.rampes_max
    vfs = Vfs(args.client, GRFS)

    if args.lister:
        return lister(args, vfs)

    if not args.corps:
        ap.error("il faut --corps ou --lister")

    chemin = chemin_corps(args.corps, args.sexe)
    octets = vfs.lire(chemin)
    if octets is None:
        sys.exit("introuvable : %s" % chemin)

    images, palette = lire_spr(octets)
    usage = compter_usage(images)

    serveur = None
    if args.pal:
        brut = vfs.lire(args.pal)
        if brut is None:
            sys.exit("palette introuvable : %s" % args.pal)
        serveur = lire_pal(brut)
        if serveur is None:
            sys.exit("palette trop courte (%d octets) : %s"
                     % (len(brut), args.pal))

    # 🔴 Les rampes se détectent sur la palette FUSIONNÉE, jamais sur le sprite
    # nu : c'est l'ordre que suit l'éditeur, et les frontières de rampes ne sont
    # pas les mêmes des deux côtés de la fusion.
    base = fusionner_serveur(palette, serveur)
    rampes = detecter_rampes(base, usage)

    print("%s (%s) — %d images, %d index utilisés%s"
          % (args.corps, args.sexe, len(images), sum(1 for u in usage if u),
             "" if serveur is None else "  [fusionné avec %s]" % args.pal))
    print()
    for n, r in enumerate(rampes):
        teinte = "—" if r["teinte"] is None else "%3d°" % r["teinte"]
        echantillon = " ".join(
            "#%02x%02x%02x" % (base[4 * i], base[4 * i + 1], base[4 * i + 2])
            for i in range(r["debut"], min(r["debut"] + 4, r["debut"] + r["longueur"])))
        print("  rampe %d : index %3d-%-3d (%2d)  %-6s %s  %6d px   %s"
              % (n, r["debut"], r["debut"] + r["longueur"] - 1, r["longueur"],
                 nommer(r), teinte, r["pixels"], echantillon))

    if not args.png:
        return

    recette = {}
    for part in filter(None, args.recette.split(";")):
        cle, valeurs = part.split(":")
        absolu = valeurs.startswith("=")
        if absolu:
            valeurs = valeurs[1:]
        teinte, sat, lum = (float(x) for x in valeurs.split(","))
        recette[int(cle)] = (teinte, sat, lum, absolu)

    if args.image >= len(images):
        sys.exit("image %d hors bornes (%d)" % (args.image, len(images)))

    # Trois panneaux dès qu'il y a une fusion, parce que c'est justement l'écart
    # entre le premier et le deuxième qui montre ce que la fusion a récupéré.
    variantes = [palette, base] if serveur is not None else [base]
    variantes.append(appliquer(base, rampes, recette))
    legende = ("sprite nu | fusionné | recette" if serveur is not None
               else "sprite nu | recette")
    largeur, hauteur, rgb = cote_a_cote(images[args.image], variantes)
    ecrire_png(args.png, largeur, hauteur, rgb)
    print("\nécrit : %s  (%s)" % (args.png, legende))


def lister(args, vfs):
    """Passe sur tous les corps : combien de rampes, quelle couverture.

    Sans fusion, volontairement : le `.pal` qui va avec un corps se choisit par
    IDENTIFIANT DE CLASSE et couleur de vêtement, pas par nom de sprite, et il y
    en a 553 par classe. Cette passe mesure donc le pire cas — ce que donne le
    sprite nu. Pour voir l'effet de la fusion sur un corps précis, `--corps`
    avec `--pal`.
    """
    total, sans_rampe, histo, couvertures = 0, [], {}, []
    for sexe in ("m", "f"):
        noms = vfs.lister(dossier_corps(sexe), ".spr")
        if not noms:
            print("aucun .spr sous %s" % dossier_corps(sexe))
            continue
        for chemin in noms:
            octets = vfs.lire(chemin)
            if octets is None:
                continue
            court = chemin.rsplit("\\", 1)[-1]
            try:
                images, palette = lire_spr(octets)
                usage = compter_usage(images)
                rampes = detecter_rampes(palette, usage)
            except Exception as exc:            # un .spr hors format ne doit
                print("  ! %s : %s" % (court, exc))     # pas arrêter la passe
                continue
            total += 1
            histo[len(rampes)] = histo.get(len(rampes), 0) + 1
            pixels = sum(usage)
            couverture = (100.0 * sum(r["pixels"] for r in rampes) / pixels
                          if pixels else 0.0)
            couvertures.append((couverture, court, len(rampes)))
            if not rampes:
                sans_rampe.append(court)

    print("%d corps analysés" % total)
    for n in sorted(histo):
        print("  %d rampe(s) : %3d corps" % (n, histo[n]))

    if couvertures:
        couvertures.sort()
        moyenne = sum(c for c, _, _ in couvertures) / len(couvertures)
        print("\ncouverture (part des pixels du corps qui devient réglable)")
        print("  moyenne %.0f %%   médiane %.0f %%"
              % (moyenne, couvertures[len(couvertures) // 2][0]))
        print("  les 8 moins bien couverts :")
        for c, nom, n in couvertures[:8]:
            print("    %-38s %2d rampes  %4.0f %%" % (nom, n, c))

    if sans_rampe:
        print("\n%d corps SANS rampe détectée :" % len(sans_rampe))
        for nom in sans_rampe[:10]:
            print("   ", nom)


if __name__ == "__main__":
    main()
