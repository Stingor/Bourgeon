# -*- coding: utf-8 -*-
"""Apparie les blocs d'accesseurs triviaux par leur EMPREINTE de deplacements.

Le compilateur emet ces micro-fonctions contigues et dans l'ordre de declaration.
La suite des deplacements d'un bloc est donc une empreinte tres discriminante :
si elle est identique (ou translatee d'une constante) des deux cotes, le bloc est
le meme et TOUS ses accesseurs s'apparient un a un.

Bonus : le decalage du bloc donne directement le deplacement de la STRUCTURE
entre les deux builds -- 0 si elle n'a pas bouge.
"""
import io
import json
import os
import re

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"

a = json.load(open(os.path.join(D, 'accessors_2025.json')))
b = json.load(open(os.path.join(D, 'accessors_2026.json')))


def sig(bloc):
    """empreinte invariante au decalage : les ecarts entre deplacements successifs"""
    ds = [int(x['disp'], 16) for x in bloc]
    return tuple(ds[i + 1] - ds[i] for i in range(len(ds) - 1))


ga, gb = {}, {}
for bl in a['blocs']:
    ga.setdefault(sig(bl), []).append(bl)
for bl in b['blocs']:
    gb.setdefault(sig(bl), []).append(bl)

pairs, shifts = [], []
amb = 0
for s, la in ga.items():
    lb = gb.get(s)
    if not lb:
        continue
    if len(la) != 1 or len(lb) != 1:
        amb += 1
        continue
    x, y = la[0], lb[0]
    sh = int(y[0]['disp'], 16) - int(x[0]['disp'], 16)
    shifts.append((sh, len(x), x[0]['ea'], y[0]['ea']))
    for p, q in zip(x, y):
        pairs.append({"2025": p['ea'], "2026": q['ea'],
                      "disp2025": p['disp'], "disp2026": q['disp'],
                      "shift": sh, "taille_bloc": len(x)})

print("blocs 2025 / 2026 : %d / %d" % (len(a['blocs']), len(b['blocs'])))
print("blocs apparies par empreinte : %d  (%d ambigus)" % (len(shifts), amb))
print("accesseurs apparies : %d\n" % len(pairs))
print("  %-11s %-11s %-4s %s" % ("2025", "2026", "n", "decalage de la structure"))
for sh, n, x, y in sorted(shifts, key=lambda t: -t[1]):
    note = "structure INCHANGEE" if sh == 0 else ("%+#x" % sh)
    print("  %s %s %-4d %s" % (x, y, n, note))

json.dump(pairs, io.open(os.path.join(DOCS, 'accessor_pairs.json'), 'w', encoding='utf-8'),
          indent=1, ensure_ascii=False)
print("\necrit : docs/2026/accessor_pairs.json (%d paires)" % len(pairs))

# --- combien sont utiles a Bourgeon ? ----------------------------------------
MAN = r"d:/Mes documents/GitHub/Bourgeon/docs/address_manifest.md"
manifest = set()
row = re.compile(r'^\|\s*`(0x[0-9a-fA-F]+)`')
for line in io.open(MAN, encoding='utf-8'):
    m = row.match(line)
    if m:
        manifest.add('0x%08x' % int(m.group(1), 16))
ported = {('0x%08x' % int(k, 16)) for k in json.load(open(os.path.join(DOCS, 'port_2025_2026.json')))}
merged = json.load(io.open(os.path.join(DOCS, 'merged_pairs.json'), encoding='utf-8'))
known = ported | set(merged)

inman = [p for p in pairs if p['2025'] in manifest]
new = [p for p in inman if p['2025'] not in known]
print("\n  dans le manifeste : %d ; dont NOUVELLES : %d" % (len(inman), len(new)))
for p in new:
    print("     %s -> %s  (+%s -> +%s)" % (p['2025'], p['2026'], p['disp2025'], p['disp2026']))

# controle : les paires deja connues concordent-elles ?
ok = ko = 0
for p in pairs:
    if p['2025'] in merged:
        if merged[p['2025']]['to'].lower() == p['2026'].lower():
            ok += 1
        else:
            ko += 1
            print("     !! divergence %s : merged=%s accesseurs=%s"
                  % (p['2025'], merged[p['2025']]['to'], p['2026']))
print("\n  CONTROLE avec merged_pairs : %d concordent, %d divergent" % (ok, ko))
