# -*- coding: utf-8 -*-
"""Produit le plan d'application : pour chaque adresse portee, OU la changer.

Ne modifie rien. Le portage n'a de valeur que si l'on sait quels sites du code
sont concernes -- le manifeste les liste, on les croise avec les paires.
"""
import io
import json
import os
import re
from collections import defaultdict

DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"
MAN = r"d:/Mes documents/GitHub/Bourgeon/docs/address_manifest.md"

merged = json.load(io.open(os.path.join(DOCS, 'merged_pairs.json'), encoding='utf-8'))
ref = json.load(open(os.path.join(DOCS, 'port_2025_2026.json')))
suspects = set(json.load(io.open(os.path.join(DOCS, 'port_suspects.json'), encoding='utf-8')))

# manifeste : adresse -> (noms, sites, commentaire)
manifest = {}
row = re.compile(r'^\|\s*`(0x[0-9a-fA-F]+)`\s*\|([^|]*)\|([^|]*)\|([^|]*)\|')
for line in io.open(MAN, encoding='utf-8'):
    m = row.match(line)
    if m:
        sites = [s.strip().strip('`') for s in m.group(3).split('<br>') if s.strip()]
        manifest['0x%08x' % int(m.group(1), 16)] = {
            "names": m.group(2).strip(),
            "sites": sites,
            "comment": m.group(4).strip()}

# toutes les correspondances connues : portage existant + nouvelles
known = {}
for s, r in ref.items():
    k = '0x%08x' % int(s, 16)
    known[k] = {"to": '0x%08x' % int(r['to'], 16), "src": r.get('via', 'portage'),
                "suspect": k in suspects}
for s, r in merged.items():
    k = '0x%08x' % int(s, 16)
    if k in known:
        continue
    known[k] = {"to": r['to'], "src": r.get('src', 'tables'), "suspect": False,
                "familles": len(r.get('tables', []))}

# regroupe par FICHIER
by_file = defaultdict(list)
n_addr = 0
for addr, info in known.items():
    if addr not in manifest:
        continue
    n_addr += 1
    for site in manifest[addr]['sites']:
        f = site.split(':')[0].replace('\\', '/')
        by_file[f].append((site, addr, info, manifest[addr]))

lines = []
w = lines.append
w("# Plan d'application du portage 2025 -> 2026")
w("")
w("GENERE par `scripts/apply_plan.py` a partir de `merged_pairs.json`,")
w("`port_2025_2026.json` et `docs/address_manifest.md`. **Ne rien editer ici.**")
w("")
w("\u26a0 **Rien n'a ete applique au code.** Ce document dit seulement OU il faudra")
w("intervenir, le jour ou le client 2026 deviendra la cible. Le client de")
w("production reste le 2025-07-16.")
w("")
w("- adresses connues et utilisees par Bourgeon : **%d**" % n_addr)
w("- fichiers concernes : **%d**" % len(by_file))
w("- sites a modifier : **%d**" % sum(len(v) for v in by_file.values()))
w("")
w("\U0001f534 Les lignes marquees **SUSPECT** viennent du portage precedent et ont")
w("echoue au controle collision/taille : les verifier AVANT de les appliquer")
w("(voir `port_suspects.json`).")
w("")

for f in sorted(by_file, key=lambda x: -len(by_file[x])):
    seen = set()
    entries = []
    for e in sorted(by_file[f], key=lambda x: x[0]):
        k = (e[0], e[1])
        if k not in seen:
            seen.add(k)
            entries.append(e)
    w("## `%s` — %d site(s)" % (f, len(entries)))
    w("")
    w("| ligne | 2025 | 2026 | symbole | source |")
    w("|---|---|---|---|---|")
    for site, addr, info, man in entries:
        line = site.split(':')[-1] if ':' in site else ''
        nm = man['names'].replace('\u2693', '').strip()[:40]
        src = info['src']
        if info.get('suspect'):
            src = "**SUSPECT** " + src
        w("| %s | `%s` | `%s` | %s | %s |" % (line, addr, info['to'], nm, src))
    w("")

path = os.path.join(DOCS, 'apply_plan.md')
io.open(path, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
print("ecrit : docs/2026/apply_plan.md")
print("  %d adresses, %d fichiers, %d sites"
      % (n_addr, len(by_file), sum(len(v) for v in by_file.values())))
print("\n  les fichiers les plus touches :")
for f in sorted(by_file, key=lambda x: -len(by_file[x]))[:12]:
    print("    %-56s %3d sites" % (f, len(by_file[f])))
