# Porter par le RTTI : (CLASSE, SLOT), et ce que ça donne vraiment

Relevé du **2026-08-27**. Le portage initial n'avait tiré que **67**
correspondances du RTTI, alors que le binaire compte plus de 1 300 vtables. Ce
document exploite le filon jusqu'au bout — et dit franchement ce qu'il rapporte.

## Le principe

Une méthode virtuelle se retrouve par **(nom de classe, index de slot)**. Le nom
vient du RTTI, l'index de la position dans la vtable : ni l'un ni l'autre ne
dépend d'un seul octet de code. C'est le vecteur le plus robuste qui soit entre
deux builds.

| | 2025 | 2026 |
|---|---|---|
| classes RTTI | **1 594** | 1 332 |
| vtables | 1 601 | 1 341 |
| slots au total | **36 834** | 35 717 |
| classes **communes** | \multicolumn — | **964** |

630 classes n'existent qu'en 2025, 368 qu'en 2026.

**Résultat : 4 327 méthodes appariées** (16 écartées pour conflit).

| mode | classes | ce que c'est |
|---|---|---|
| `exact` | 380 | même nombre de slots des deux côtés |
| `prefixe` | 576 | nombre différent → seul le préfixe commun est apparié |

⚠ Le mode `prefixe` est le risqué : si une méthode a été **insérée au milieu**,
tout l'alignement qui suit est faux. C'est lui qu'il fallait valider.

## 🔴🔴 La validation : quatre tests, dont un décisif

**Le test qui tranche** — dans l'IDB 2026, les méthodes non nommées portent un
nom auto de la forme `Classe__vf_NN`, où `NN` est **l'offset du slot en hexa**.
Or mon index de slot vient de mon propre parcours de la vtable, sans jamais lire
ce nom. Les deux doivent coïncider :

| | |
|---|---|
| paires testables | **2 164** |
| slot × 4 == `NN` | **2 164** |
| divergences | **0** |

**100 %, et uniquement sur le mode `prefixe`** — c'est-à-dire précisément celui
dont on doutait.

Les trois autres :

| test | paires | témoin aléatoire |
|---|---|---|
| ratio de tailles, tous | médiane **1,000**, 79,4 % > 0,8 | 0,171 / 7,9 % |
| … mode `exact` | médiane 1,000, **89,9 %** > 0,8 | 0,172 / 8,3 % |
| … mode `prefixe` | médiane 0,970, 74,4 % > 0,8 | 0,180 / 8,3 % |
| monotonie, tous | **5,5 %** d'inversions | 50,0 % |
| … mode `exact` | **1,7 %** | 50 % |
| … mode `prefixe` | 7,4 % | 50 % |

Croisements : **28/28** avec le portage initial, **4/4** avec les paires par
identifiants. **0 divergence** partout.

⚠ Sur les noms bruts, 615 paires portent des noms « différents » — ce n'en est
pas. Ce sont nos noms sémantiques face aux noms auto 2026 :
`UIWindow_OnMsg` ↔ `UIWindow__vf_94` (slot 37 × 4 = 0x94). Le reste vient du
**COMDAT folding** de la STL, où plusieurs fonctions identiques partagent une
adresse et IDA choisit un nom parmi elles.

## ⚠ L'apport au portage est PRESQUE NUL

**1 seule adresse** du manifeste sur 4 327 paires (`0x00c86740 → 0x004e37a0`,
`CGameMode` slot 6). 28 autres étaient déjà connues.

La raison est structurelle : **Bourgeon n'appelle pas les méthodes virtuelles par
adresse en dur** — il passe par la vtable à l'exécution. Le RTTI porte donc du
code que Bourgeon ne référence jamais directement.

➡ Le portage reste à **267/784 (34,1 %)**.

## ✅ Le vrai gain : 561 noms dans l'IDB 2026

C'est là que le travail paye. L'IDB 2026 était presque entièrement anonyme
(24 fonctions nommées sur 4 327 appariées, contre 440 côté 2025). Les noms ont
été **propagés** :

| | |
|---|---|
| noms 2025 explicites, cible anonyme | **561 appliqués**, 0 échec |
| nom 2025 auto-généré (rien à propager) | 3 711 |
| symbole C++ déjà correct | 29 |
| plusieurs noms 2025 pour une cible (écarté) | 15 |

Exemples : `UIWindow_OnMsg`, `UIWindow_Render`, `UIWindow_HitTest`,
`UIWindow_LocalToScreen`, `CompoundFile_Serialize*`…

🔴 **C'est réversible** : les noms précédents sont dans
`scripts/names_backup_2026.json`, et `scripts/undo_names.py` les restaure.
L'IDB a été sauvegardé après l'opération.

## Rejouer

| ordre | script | où |
|---|---|---|
| 1 | `vtables.py` | dans CHAQUE IDA |
| 2 | `match_vtables.py` | local — apparie par (classe, slot) |
| 3 | `annotate_vt.py` | dans CHAQUE IDA — noms et tailles |
| 4 | `apply_names.py` | **IDA 2026 uniquement** — propage les noms |
| — | `undo_names.py` | IDA 2026 — annule la propagation |
