# -*- coding: utf-8 -*-
"""Fabrique la texture de case de GreyWorld : un carre BLANC OPAQUE.

Pourquoi une texture a nous : le client n'en a aucune qui soit unie. `grid.tga`
est un ANNEAU (mesure sur capture, 2026-09-01) dont le centre est TRANSPARENT,
et les seules autres qu'il cite (alpha_center, whitelight) sont des degrades.
Echantillonner un texel opaque « au juge » dans l'une d'elles est un pari qu'on
ne peut pas verifier : data.grf de Moonlight est chiffre.

Blanc PUR et opaque : la couleur vient du diffuse du sommet, donc une seule
texture sert aux trois familles de cases. La bordure ne vient pas d'elle non
plus, mais du JOINT (le quad est retreci, le sol reste visible autour).

32x32 suffit : le quad n'echantillonne qu'un aplat, la taille n'a aucun effet.
"""
import io, os, struct

LARGEUR = HAUTEUR = 32
# Le client cherche sous data\texture\ ; on reste a plat dans ce dossier (c'est
# la ou il faut poser le fichier), mais avec un nom PREFIXE : le disque passe
# AVANT les GRF, donc un nom generique comme « cell.tga » masquerait pour de bon
# une texture du jeu qui porterait le meme nom -- et data.grf etant chiffre, on
# n'a aucun moyen de verifier qu'elle n'existe pas.
DEST = r"D:\Mes documents\GitHub\Bourgeon\assets\data\texture\bourgeon_cell.tga"

entete = struct.pack(
    "<BBBHHBHHHHBB",
    0,        # longueur du champ id
    0,        # pas de palette
    2,        # true-color non compresse
    0, 0, 0,  # specification de palette, vide
    0, 0,     # origine x, y
    LARGEUR, HAUTEUR,
    32,       # bits par pixel : BGRA
    0x28,     # 8 bits d'alpha + origine en HAUT a gauche
)
# BGRA, blanc opaque.
pixels = b"\xff\xff\xff\xff" * (LARGEUR * HAUTEUR)

os.makedirs(os.path.dirname(DEST), exist_ok=True)
tmp = DEST + ".tmp"
with io.open(tmp, "wb") as f:
    f.write(entete + pixels)
os.replace(tmp, DEST)
print(DEST, os.path.getsize(DEST), "octets")
