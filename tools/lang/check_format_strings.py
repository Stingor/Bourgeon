# -*- coding: utf-8 -*-
"""Traque les chaînes de FORMAT qui ne sont pas des littéraux du code.

Les fonctions de texte d'ImGui sont printf-like : leur premier argument est le
format. Y passer autre chose qu'un littéral écrit sur place — `i18n::Tr(...)`,
un accesseur qui traduit lui-même, un `std::string` — fait d'un texte VARIABLE
ce format. Un pour-cent qui s'y trouve devient alors une spécification, et
printf lit un argument que personne n'a poussé sur la pile.

🔴 CE N'EST PAS VISIBLE À LA RELECTURE. Le français d'origine peut n'avoir
aucun pour-cent : c'est le catalogue de traduction qui crée le défaut, et il
s'édite hors compilation. Le catalogue anglais porte déjà « 100 % straight
away », où « % s » est une spécification valide.

Ce que le contrôle accepte :
  · un LITTÉRAL sans spécification            TextDisabled("Aucun NPC bloqué.")
  · un littéral AVEC ses arguments            TextDisabled("(maîtrise %d)", n)
  · "%s" suivi du texte, quelle qu'en soit la provenance
                                              TextDisabled("%s", SlotLabel(s))
Ce qu'il refuse :
  · un format NON littéral, sans argument     TextDisabled(i18n::Tr("…"))
                                              TextDisabled(SlotLabel(slot))
  · un littéral qui porte une spécification sans rien pour la nourrir
                                              TextDisabled("100 % sûr")

⚠ Il sort en CODE 1 dès qu'il trouve : c'est ce qui le rend utilisable en
intégration continue (.github/workflows/i18n.yml), comme check_catalog.ps1.

Usage :
    python3 tools/lang/check_format_strings.py [racine]
"""
from __future__ import print_function

import os
import re
import sys

# Ce contrôle affiche du français. Sur un runner Linux la sortie est déjà en
# UTF-8 ; en local, une console Windows en cp1252 remplacerait les accents et
# ferait échouer l'écriture. `reconfigure` existe depuis Python 3.7 — plus bas,
# on laisse simplement faire, le contrôle vaut mieux que son affichage.
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except (AttributeError, ValueError):
    pass

# ── Les fonctions printf-like ────────────────────────────────────────────────
# TextUnformatted en est ABSENTE : elle prend un const char* brut, sans format,
# et lui coller un "%s" serait une erreur.
FMT_FUNCS = ["TextDisabled", "TextColored", "TextWrapped", "BulletText",
             "SetTooltip", "LabelText", "Text"]

# Celles-ci ne sont dangereuses QUE qualifiées `ImGui::`. Sans préfixe, c'est
# l'enveloppe `mui::` qui répond (ui/ro_widgets.h), et elle pose son "%s"
# elle-même. `Text`, en revanche, est variadique DANS LES DEUX namespaces.
SAFE_WITHOUT_PREFIX = {"TextWrapped"}

CALL = re.compile(r"\b(ImGui::)?(" + "|".join(FMT_FUNCS) + r")\s*\(")

# `%%` d'abord dans l'alternance : consommé d'un bloc, son second pour-cent ne
# peut plus amorcer une fausse spécification. Sans cela « -%d %% de réussite »
# se lit « %d » puis « % d », et le contrôle rend des divergences imaginaires.
TOKEN = re.compile(r"%%|%[-+ #0]*[\d.*]*(?:hh|h|ll|l|j|z|t|L)?[diouxXeEfgGaAcspn]")

SKIP_DIRS = {".git", "build", "out", "third_party", "external", "vendor"}


def specs(text):
    """Les spécifications de format que porte ce texte (hors `%%`)."""
    return [t for t in TOKEN.findall(text) if t != "%%"]


def scan_call(src, open_paren):
    """Parcourt l'appel en suivant chaînes et parenthèses.

    Rend (index de la parenthèse fermante, virgules de premier niveau).
    """
    depth = 0
    in_str = False
    in_chr = False
    esc = False
    commas = []
    i = open_paren
    while i < len(src):
        ch = src[i]
        if in_str or in_chr:
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif in_str and ch == '"':
                in_str = False
            elif in_chr and ch == "'":
                in_chr = False
        elif ch == '"':
            in_str = True
        elif ch == "'":
            in_chr = True
        elif ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
            if depth == 0:
                return i, commas
        elif ch == "," and depth == 1:
            commas.append(i)
        i += 1
    return -1, commas


def literal_value(expr):
    """La valeur d'un format LITTÉRAL, y compris concaténé sur plusieurs lignes.

    Rend None si l'expression n'est pas un littéral pur — c'est ce cas-là qui
    nous intéresse.
    """
    out = []
    i = 0
    n = len(expr)
    while i < n:
        ch = expr[i]
        if ch.isspace():
            i += 1
            continue
        if ch != '"':
            return None                    # autre chose qu'une chaîne : pas un littéral
        i += 1
        esc = False
        while i < n:
            c = expr[i]
            if esc:
                out.append(c)
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                break
            else:
                out.append(c)
            i += 1
        i += 1                             # la guillemet fermante
    return "".join(out) if out else None


def line_of(src, index):
    return src.count("\n", 0, index) + 1


def find_offenders(src):
    """Les appels dont le format n'est pas maîtrisé. Rend (ligne, fn, motif)."""
    found = []
    for m in CALL.finditer(src):
        qualified, fn = m.group(1), m.group(2)
        if fn in SAFE_WITHOUT_PREFIX and not qualified:
            continue                       # l'enveloppe mui::, elle pose son "%s"
        close, commas = scan_call(src, m.end() - 1)
        if close < 0:
            continue
        # TextColored prend la couleur en premier : son FORMAT est le 2e argument.
        if fn == "TextColored":
            if not commas:
                continue
            start = commas[0] + 1
            rest = commas[1:]
        else:
            start = m.end()
            rest = commas
        expr = src[start:close]
        if not expr.strip():
            continue
        lit = literal_value(expr)
        if lit is None:
            # Format calculé. Avec des arguments derrière, c'est un choix
            # assumé (un gabarit monté à la volée) ; sans, le texte EST le
            # format, et c'est le piège.
            if not rest:
                found.append((line_of(src, m.start()), fn,
                              "format non littéral sans argument"))
        elif specs(lit) and not rest:
            # Un littéral qui porte une spécification et rien pour la nourrir.
            found.append((line_of(src, m.start()), fn,
                          "littéral avec %s mais aucun argument"
                          % ", ".join(specs(lit))))
    return found


# ── Témoins ─────────────────────────────────────────────────────────────────
# Un contrôle qu'on n'a jamais vu rendre « rouge » ne prouve rien. Ceux-ci
# tournent à chaque exécution : ils coûtent une milliseconde et garantissent
# que le détecteur n'est pas devenu aveugle.
WITNESSES = [
    # (source, nombre d'anomalies attendues, ce qu'on vérifie)
    ('ImGui::TextDisabled(i18n::Tr("Aucun NPC bloqué."));', 1,
     "un i18n::Tr en position de format"),
    ('ImGui::TextDisabled(SlotLabel(slot));', 1,
     "un accesseur qui traduit lui-même"),
    ('ImGui::TextDisabled("%s", SlotLabel(slot));', 0,
     "le meme accesseur, protégé"),
    ('ImGui::TextDisabled("%s", i18n::Tr("Aucun NPC bloqué."));', 0,
     "un i18n::Tr protégé"),
    ('ImGui::TextDisabled(i18n::Tr("(maîtrise %d / 1999)"), mastery_);', 0,
     "un format VOULU, avec son argument"),
    ('ImGui::TextDisabled("Aucun NPC bloqué.");', 0,
     "un littéral sans spécification"),
    ('ImGui::TextDisabled("100 % sûr");', 1,
     "un littéral dont le pour-cent n'est pas doublé"),
    ('ImGui::TextDisabled("-%d %% de réussite", malus);', 0,
     "un %% littéral, correctement doublé"),
    ('ImGui::TextUnformatted(i18n::Tr("Aucun NPC bloqué."));', 0,
     "TextUnformatted, qui n'a pas de format"),
    ('TextWrapped(i18n::Tr("Aucun NPC bloqué."));', 0,
     "l'enveloppe mui::TextWrapped, qui pose son %s"),
    ('ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), i18n::Tr("Indisponible."));', 1,
     "TextColored, dont le format est le SECOND argument"),
    ('ImGui::TextColored(col, "%s", i18n::Tr("Indisponible."));', 0,
     "le meme TextColored, protégé"),
]


def run_witnesses():
    bad = []
    for src, want, what in WITNESSES:
        got = len(find_offenders(src))
        if got != want:
            bad.append("  %s : %d anomalie(s) au lieu de %d\n    %s"
                       % (what, got, want, src))
    if bad:
        print("TÉMOINS EN ÉCHEC — le détecteur ne voit pas juste :")
        print("\n".join(bad))
        return False
    print("témoins du détecteur : %d/%d" % (len(WITNESSES), len(WITNESSES)))
    return True


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "src"
    if not os.path.isdir(root):
        print("Racine introuvable : %s" % root)
        return 2

    if not run_witnesses():
        return 2

    total = 0
    for base, dirs, names in os.walk(root):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in sorted(names):
            if not name.endswith((".cc", ".cpp", ".h")):
                continue
            path = os.path.join(base, name)
            with open(path, "rb") as f:
                src = f.read().decode("utf-8", "replace")
            for line, fn, why in find_offenders(src):
                rel = os.path.relpath(path, root).replace("\\", "/")
                print("%s:%d: %s — %s" % (rel, line, fn, why))
                total += 1

    print()
    if total:
        print("%d chaîne(s) de format non maîtrisée(s)." % total)
        print("Remède : passer le texte en ARGUMENT, derrière un \"%s\".")
        return 1
    print("OK : aucune chaîne de format non maîtrisée.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
