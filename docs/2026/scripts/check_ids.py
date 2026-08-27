# -*- coding: utf-8 -*-
"""Les ids de fenetre sont-ils STABLES entre 2025 et 2026 ?

7 ids de plus en 2026. S'ils ont ete ajoutes A LA FIN, les ids se correspondent.
S'ils ont ete INSERES, tout est decale et l'appariement par id serait faux.

Test : correlation de la taille des cases id par id, comparee au meme test avec
un decalage artificiel. Le bon alignement doit ressortir nettement.
"""
import json
import os

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
a = json.load(open(os.path.join(D, 'switch_makewindow_2025.json')))['cases']
b = json.load(open(os.path.join(D, 'switch_makewindow_2026.json')))['cases']

ka = set(int(k) for k in a)
kb = set(int(k) for k in b)
print("ids 2025 : %d  (%d..%d)" % (len(ka), min(ka), max(ka)))
print("ids 2026 : %d  (%d..%d)" % (len(kb), min(kb), max(kb)))
print("communs  : %d" % len(ka & kb))
print("seulement 2026 : %s" % sorted(kb - ka))
print("seulement 2025 : %s" % sorted(ka - kb))


def score(shift):
    """proportion d'ids dont le case a une taille comparable, avec decalage."""
    ok = tot = 0
    for i in sorted(ka):
        j = i + shift
        if str(i) not in a or str(j) not in b:
            continue
        x = a[str(i)]['ninsn']
        y = b[str(j)]['ninsn']
        if not x or not y:
            continue
        tot += 1
        if min(x, y) / float(max(x, y)) > 0.8:
            ok += 1
    return ok, tot


print("\ndecalage : proportion d'ids dont le case a une taille comparable (>0.8)")
best = None
for sh in range(-10, 11):
    ok, tot = score(sh)
    if tot:
        pct = 100.0 * ok / tot
        star = ""
        if best is None or pct > best[1]:
            best = (sh, pct)
        print("   %+3d : %5.1f%%  (%d/%d)%s" % (sh, pct, ok, tot, star))

print("\n=> meilleur alignement : decalage %+d (%.1f%%)" % best)
if best[0] == 0:
    print("   les ids de fenetre sont STABLES entre les deux builds.")
else:
    print("   ATTENTION : les ids sont DECALES, l'appariement par id serait faux.")
