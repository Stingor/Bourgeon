# -*- coding: utf-8 -*-
"""Assemble le livrable : paires enrichies + suspects du portage existant."""
import io
import json
import os
import re

D = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
DOCS = r"d:/Mes documents/GitHub/Bourgeon/docs/2026"
MAN = r"d:/Mes documents/GitHub/Bourgeon/docs/address_manifest.md"
REF = os.path.join(DOCS, 'port_2025_2026.json')

pairs = json.load(open(os.path.join(D, 'opcode_pairs_d1.json')))
a25 = json.load(open(os.path.join(D, 'annot_2025_d1.json')))
a26 = json.load(open(os.path.join(D, 'annot_2026_d1.json')))
ref = json.load(open(REF))

manifest = {}
row = re.compile(r'^\|\s*`(0x[0-9a-fA-F]+)`\s*\|([^|]*)\|([^|]*)\|([^|]*)\|')
for line in io.open(MAN, encoding='utf-8'):
    m = row.match(line)
    if m:
        manifest['0x%08x' % int(m.group(1), 16)] = {
            "names": m.group(2).strip().replace('⚓', '').strip(),
            "comment": m.group(4).strip(),
        }
ported = {('0x%08x' % int(k, 16)) for k in ref}


def n(x):
    return '0x%08x' % int(x[1:-1] if x.startswith('[') else x, 16)


out = {}
for kind in ('functions', 'globals'):
    for src, rec in pairs[kind].items():
        s, t = n(src), n(rec['to'])
        s25 = a25[kind].get(s, {})
        s26 = a26[kind].get(t, {})
        sz1, sz2 = s25.get('size', 0), s26.get('size', 0)
        ratio = round(min(sz1, sz2) / float(max(sz1, sz2)), 3) if sz1 and sz2 else None
        e = {
            "to": t,
            "kind": "fonction" if kind == 'functions' else "globale",
            "opcodes": rec['opcodes'],
            "nop": rec['nop'],
        }
        if s25.get('name'):
            e["name25"] = s25['name']
        if sz1:
            e["size25"] = "0x%X" % sz1
        if sz2:
            e["size26"] = "0x%X" % sz2
        if ratio is not None:
            e["ratio"] = ratio
            e["confiance"] = "haute" if ratio > 0.8 else ("moyenne" if ratio > 0.6 else "A VERIFIER")
        elif kind == 'globals':
            e["confiance"] = "haute" if rec['nop'] >= 3 else "moyenne"
        if s in manifest:
            e["manifeste"] = manifest[s]['names']
            if manifest[s]['comment']:
                e["comment"] = manifest[s]['comment']
            e["nouveau_pour_bourgeon"] = s not in ported
        out[s] = e

path = os.path.join(DOCS, 'port_opcode_pairs.json')
json.dump(out, io.open(path, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)

nb_man = sum(1 for v in out.values() if v.get('nouveau_pour_bourgeon'))
haute = sum(1 for v in out.values() if v.get('confiance') == 'haute')
verif = sum(1 for v in out.values() if v.get('confiance') == 'A VERIFIER')
print("port_opcode_pairs.json : %d paires" % len(out))
print("  confiance haute        : %d" % haute)
print("  A VERIFIER (ratio<0.6) : %d" % verif)
print("  nouvelles pour Bourgeon: %d" % nb_man)

# --- suspects du portage existant --------------------------------------------
au25 = json.load(open(os.path.join(D, 'audit_2025.json')))
au26 = json.load(open(os.path.join(D, 'audit_2026.json')))
from collections import defaultdict
rev = defaultdict(list)
for src, rec in ref.items():
    rev['0x%08x' % int(rec['to'], 16)].append('0x%08x' % int(src, 16))

susp = {}
for src, rec in ref.items():
    s, t = '0x%08x' % int(src, 16), '0x%08x' % int(rec['to'], 16)
    sz1 = au25.get(s, {}).get('size', 0)
    sz2 = au26.get(t, {}).get('size', 0)
    motifs = []
    if len(rev[t]) > 1:
        motifs.append("collision : %d sources 2025 pour cette cible" % len(rev[t]))
    if sz1 and sz2 and min(sz1, sz2) / float(max(sz1, sz2)) < 0.6:
        motifs.append("tailles 0x%X vs 0x%X (ratio %.2f)"
                      % (sz1, sz2, min(sz1, sz2) / float(max(sz1, sz2))))
    if motifs:
        susp[s] = {"to": t, "via": rec.get('via'), "name25": rec.get('name25'),
                   "names": rec.get('names'), "motifs": motifs,
                   "propose": out.get(s, {}).get('to')}

path2 = os.path.join(DOCS, 'port_suspects.json')
json.dump(susp, io.open(path2, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
print("\nport_suspects.json : %d entrees suspectes du portage existant" % len(susp))
for s, v in sorted(susp.items()):
    print("  %s -> %s  %-40s %s" % (s, v['to'], (v['name25'] or '')[:40], ' ; '.join(v['motifs'])))
