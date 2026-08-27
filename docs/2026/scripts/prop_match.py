# -*- coding: utf-8 -*-
"""Etape 2 (IDB 2026) : trouver la fonction qui appelle les memes equivalents.

Pour chaque fonction 2025 a porter, on a la liste de ses appeles DEJA PORTES.
La fonction 2026 cherchee appelle leurs equivalents : elle est donc dans
l'INTERSECTION des appelants de ces equivalents.

Deux garde-fous, parce qu'une intersection non vide ne prouve rien :
  - on exige que le `retn N` soit IDENTIQUE (convention d'appel) ;
  - on n'accepte que si l'intersection filtree ne laisse qu'UN candidat.
Tout le reste est rapporte comme ambigu, pas comme resultat.

Reprise sur cache disque : le pont coupe au-dela d'une minute, on traite par
lots et on n'oublie rien entre deux passes.
"""
import json
import os

import idc
import idaapi
import idautils
import ida_funcs

S = r"C:/Users/Sting/AppData/Local/Temp/claude/d--Mes-documents-GitHub-Bourgeon/7514caa0-d9c9-4713-b8ce-1c8e11292980/scratchpad"
JOBS = os.path.join(S, 'prop_jobs.json')
CACHE = os.path.join(S, 'prop_match_cache.json')
BATCH = 30

jobs = json.load(open(JOBS))
cache = json.load(open(CACHE)) if os.path.exists(CACHE) else {}


def retn_of(f):
    r = None
    p = f.start_ea
    while p < f.end_ea:
        if idc.print_insn_mnem(p) == 'retn':
            r = idc.print_operand(p, 0) or '0'
        n = idc.next_head(p, f.end_ea)
        if n <= p:
            break
        p = n
    return r


def callers_of(ea):
    out = set()
    for x in idautils.XrefsTo(ea):
        if x.type not in (16, 17):
            continue
        f = ida_funcs.get_func(x.frm)
        if f:
            out.add(f.start_ea)
    return out


todo = [j for j in jobs if len(j['anchors']) >= 2 and j['f25'] not in cache]
print("%d job(s) restant(s) a >= 2 ancres ; on en traite %d"
      % (len(todo), min(BATCH, len(todo))))

done = 0
for j in todo[:BATCH]:
    sets = []
    for a in j['anchors']:
        try:
            sets.append(callers_of(int(a, 16)))
        except Exception:
            pass
    if not sets:
        cache[j['f25']] = {'status': 'no-anchor-resolved'}
        continue
    inter = set.intersection(*sets) if len(sets) > 1 else sets[0]
    # filtre par convention d'appel
    kept = []
    for c in inter:
        f = ida_funcs.get_func(c)
        if f is None:
            continue
        if retn_of(f) == j['retn']:
            kept.append((c, f.end_ea - f.start_ea))
    if len(kept) == 1:
        c, size = kept[0]
        cache[j['f25']] = {'status': 'unique', 'to': '0x%08x' % c,
                           'size2026': size, 'size2025': j['size'],
                           'anchors': len(j['anchors']),
                           'inter': len(inter)}
    else:
        cache[j['f25']] = {'status': 'ambiguous' if kept else 'empty',
                           'n': len(kept), 'inter': len(inter),
                           'anchors': len(j['anchors'])}
    done += 1

json.dump(cache, open(CACHE, 'w'), indent=0)
uniq = sum(1 for v in cache.values() if v.get('status') == 'unique')
print("traites %d ; cache %d entrees ; %d resolution(s) unique(s)"
      % (done, len(cache), uniq))
