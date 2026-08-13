# Les libelles qui viennent d'une TABLE, et que les autres outils ne voient pas.
#
# 🔴 L'ANGLE MORT, ET POURQUOI IL EXISTE.
# extract_tr.ps1 et check_catalog.ps1 ne cherchent QUE `i18n::Tr("litteral")`.
# Un libelle range dans un tableau y echappe deux fois :
#   - il n'a pas de litteral au site d'appel, donc l'extracteur ne le voit pas ;
#   - il ressort en ORPHELIN chez check_catalog, qui le declare « legitime ».
# Resultat : check_catalog annonce « SANS traduction : 0 » alors que le libelle
# sort en francais dans toutes les langues. La panne est MUETTE.
#
# Deux verifications, correspondant aux deux façons de rater une table :
#
#   [1] ITEMS DE RoCombo. RoCombo traduit ses items A LA LECTURE (ro_imgui.cc,
#       `i18n::Tr(items[i])`) : c'est le seul endroit possible, les appelants
#       passant un tableau STATIQUE dont un Tr pose a la definition serait fige
#       en francais pour toujours. Les items se passent donc NUS -- correct, mais
#       alors rien ne garantit qu'ils aient une entree au catalogue.
#       ⚠ NE PAS « corriger » un item en l'enveloppant : il serait traduit DEUX
#       fois (cf. l'avertissement en tete de RoCombo, ro_imgui.h).
#
#   [2] LIBELLE INDEXE PASSE NU a un appel d'affichage : `RoCheckbox(kLabels[i])`.
#       La, au contraire, il manque bel et bien un `i18n::Tr` autour.
#
# Trouves a la premiere execution (2026-08-13), tous dans basic_info.cc :
#   [1] « Pourcentage », « Valeurs », « Les deux », « Profil-Droite » ;
#   [2] kBarLabels sur la case a cocher et sur « Couleur %s », kPortLabels sur la
#       sienne -- d'ou la case « Poids » restee francaise en EN et en ES.
#
# Ce que ce script ne voit PAS : une table lue par une fonction qui traduit
# elle-meme (hors RoCombo), et un libelle passe par un champ plutot qu'un index
# (`RoCheckbox(ic.name)`). L'inventaire complet reste l'export runtime du jeu
# (panneau Interface, bouton « Exporter »), seul a connaitre tout ce qui a ete
# demande a Tr pour de vrai.
#
# Usage :
#   python tools/lang/audit_table_labels.py
#   python tools/lang/audit_table_labels.py <racine-du-depot>
#
# Sort en code 1 des qu'il trouve quelque chose, pour servir en integration
# continue -- comme check_catalog.ps1.
import io
import os
import re
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "..")

# Un litteral C echappe : "..." avec \" et \\ tolerees. Sert aux deux cotes, le
# catalogue YAML employant la MEME notation d'echappement que le source.
LITERAL = r'"((?:[^"\\]|\\.)*)"'

cat = io.open(os.path.join(root, "tools", "lang", "en.yaml"), encoding="utf-8").read()
have = set(re.findall(r"^" + LITERAL + r":", cat, re.M))
literal = re.compile(LITERAL)

# 3e argument de RoCombo(label, &index, items, count) = le nom du tableau.
combo = re.compile(r"RoCombo\s*\(\s*[^,]+,\s*[^,]+,\s*([A-Za-z_]\w*)\s*,")

# Appels dont le PREMIER argument est un libelle montre au joueur. Volontairement
# limite a ceux du projet : y ajouter un appel technique noierait le rapport.
DISPLAY = [
    "RoCheckbox", "RoButton", "RoCombo", "RoBeginCombo", "RoSliderFloat",
    "RoSliderInt", "RoInputText", "WheelSliderFloat", "WheelSliderInt",
    "HelpMarker", "BulletWrapped", "SeparatorText", "TextUnformatted",
    "TextDisabled", "TextWrapped", "SetTooltip", "Selectable", "MenuItem",
    "BeginTabItem", "CollapsingHeader", "TreeNode", "Text",
]
# `Fn(identifiant[i]` -- un libelle INDEXE, donc issu d'une table. Enveloppe dans
# Tr, la parenthese suivrait `i18n::Tr` et non un `[` : pas de faux positif de ce
# cote. Deux filtres retirent en revanche le bruit qui noyait le rapport utile :
#   - INDEX LITTERAL (`x[0]`) : c'est une lecture de caractere ou de premier
#     element (`it.name[0]` pour tester le vide), jamais une table de libelles,
#     qui s'indexe toujours par une variable de boucle ;
#   - CHAMP D'UN ENREGISTREMENT, avant ou apres l'index (`a.desc.name[0]`,
#     `known_positions[k].label`, `presets[i].name`) : ce sont des DONNEES --
#     noms d'objets, de joueurs, grades de guilde venus du SERVEUR, prereglages
#     nommes par le joueur. Le projet ne les traduit pas et ne doit pas les
#     traduire : le libelle exact du serveur fait foi.
# Une table de libelles, elle, se lit d'un bloc : `kLabels[i]`, rien autour.
indexed = re.compile(
    r"\b(?:%s)\s*\(\s*([A-Za-z_]\w*(?:::\w+)*\s*\[\s*[A-Za-z_]\w*\s*\])\s*(?![.\[]|->)"
    % "|".join(DISPLAY))

missing_items = {}   # [1] item de combo sans entree au catalogue
raw_labels = []      # [2] libelle indexe passe nu
arrays = 0

for dirpath, _, files in os.walk(os.path.join(root, "src")):
    for fn in files:
        if not fn.endswith((".cc", ".h")):
            continue
        path = os.path.join(dirpath, fn)
        rel = os.path.relpath(path, root).replace("\\", "/")
        txt = io.open(path, encoding="utf-8", errors="replace").read()

        for arr in sorted(set(combo.findall(txt))):
            m = re.search(r"\b%s\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;" % re.escape(arr),
                          txt, re.S)
            if not m:
                continue  # tableau defini ailleurs : hors de portee d'un regex
            arrays += 1
            for s in literal.findall(m.group(1)):
                if s and s not in have:
                    missing_items.setdefault(s, set()).add("%s (%s)" % (rel, arr))

        for i, line in enumerate(txt.split("\n"), 1):
            # Echappatoire : certains libelles NE DOIVENT PAS etre traduits, et
            # les envelopper serait le bug inverse -- les modes de @noks (Off,
            # Self, Party, Guild) sont les arguments litteraux de la commande du
            # serveur, une UI qui dirait « Groupe » ferait taper « party ».
            # Marquer la ligne avec i18n-exempt, en disant POURQUOI.
            if "i18n-exempt" in line:
                continue
            for expr in indexed.findall(line):
                raw_labels.append((rel, i, " ".join(expr.split())))

print("[1] items de RoCombo : %d tableaux inspectes, %d sans entree au catalogue"
      % (arrays, len(missing_items)))
for s, where in sorted(missing_items.items()):
    print("      %-28s %s" % (s, ", ".join(sorted(where))))

print("[2] libelles indexes passes NUS a un appel d'affichage : %d" % len(raw_labels))
for rel, line, expr in raw_labels:
    print("      %s:%d  %s" % (rel, line, expr))

sys.exit(1 if (missing_items or raw_labels) else 0)
