# Porter par les OPCODES : l'identifiant qui ne bouge pas d'un build à l'autre

Relevé du **2026-08-27**. Complète [address_port_2025_2026.md](address_port_2025_2026.md),
qui portait 157 adresses par RTTI, chaînes et propagation.

Résultat : **623 paires**, dont **43 adresses du manifeste** que le portage
précédent n'atteignait pas → **157 → 200 / 784 (20 % → 25,5 %)**.
Et, en prime, **11 entrées fausses ou douteuses détectées dans le portage
existant** ([port_suspects.json](port_suspects.json)).

## 🔴 L'idée : l'opcode est stable, le code ne l'est pas

Tous les vecteurs précédents partent du **code** (octets, vtables, chaînes) —
or c'est précisément ce que le compilateur réécrit. L'opcode de paquet, lui, est
imposé par le protocole : `0x00A0` désigne la même chose dans les deux builds,
quoi qu'il arrive au binaire.

Le client concentre tout ce savoir en un seul endroit :

| | 2025-07-16 | 2026-07-07 |
|---|---|---|
| `RecvLoop_DispatchPackets` | `0x00C9DF00`, 50 141 o | `0x005095A0`, 52 074 o |
| switch principal | `0x00C9E2B1` | `0x005097CB` |
| table de sauts | `0x00CAA2E0` | `0x0051610C` |
| cases | **3011** | **3029** |
| `lowcase` (1ᵉʳ opcode) | **115** | **115** |
| opcodes traités | 847 | 784 |

**775 opcodes communs**, et 734 d'entre eux ont des cases de taille comparable :
les deux fonctions se correspondent bien.

## La méthode : signature de co-occurrence

Pour chaque opcode, on relève ce que son case touche — globales lues/écrites et
fonctions appelées — **en suivant les appels d'un cran** (le case délègue au
handler, c'est là que tout se passe : sans ce cran, on ne voit que 196 globales
au lieu de 643).

La **signature** d'un objet est l'ensemble des opcodes qui le touchent.

> Signature identique **et unique des deux côtés** ⇒ paire.
> Signature partagée par plusieurs objets d'un même côté ⇒ **on ne conclut pas.**

C'est ce second point qui fait tout le travail : les fonctions utilitaires
(`memcpy`, `sprintf`…) apparaissent partout, forment de gros groupes ambigus, et
sont écartées d'office. Aucune liste noire à maintenir.

⚠ **La collision est impossible par construction** : chaque objet 2026 n'a
qu'une signature, donc n'appartient qu'à un groupe. Deux sources ne peuvent pas
réclamer la même cible — le défaut qu'on va justement trouver dans le portage
précédent.

## 🔴🔴 La validation, et pourquoi le témoin négatif est indispensable

Le recoupement avec le portage existant ne pouvait rien prouver : **4 adresses
communes sur 623**. Les deux méthodes couvrent des ensembles disjoints — c'est
une bonne nouvelle pour la couverture, une impasse pour la validation.

Le test qui tranche compare la distribution des **ratios de taille**
(`min/max` des deux fonctions) à celle d'un appariement **aléatoire des mêmes
adresses** (20 tirages) :

| ratio de tailles | médiane | > 0,8 | > 0,6 |
|---|---|---|---|
| **paires proposées** (497) | **0,981** | **86,3 %** | **96,8 %** |
| **témoin aléatoire** (9 940) | 0,291 | 10,4 % | 22,4 % |

Sans ce témoin, « médiane 0,98 » n'aurait rien voulu dire — c'est l'écart qui
fait la preuve. Et par force de signature :

| signature | médiane | > 0,6 |
|---|---|---|
| ≥ 3 opcodes (114 paires) | **1,000** | 99,1 % |
| 2 opcodes (77) | 0,976 | 89,6 % |
| 1 opcode (306) | 0,975 | 97,7 % |

➡ **Une signature d'un seul opcode est presque aussi sûre**, parce que ce qui
compte n'est pas la taille de la signature mais son **unicité**.

Second test, les noms présents des deux côtés : **4 identiques, 0 contradiction
réelle** — les 3 « différences » sont des `nullsub_*`, stubs vides à
numérotation automatique.

## 🔴🔴 Ce que l'audit a trouvé dans le portage EXISTANT

Le même contrôle appliqué à `port_2025_2026.json` : **11 entrées suspectes sur
157 (7 %)**. Son « 0 divergence » reposait sur 4 correspondances manuelles
vérifiables — trop peu pour voir ceci :

**Deux collisions**, c'est-à-dire des impossibilités :

| cible 2026 | taille | sources 2025 revendiquées |
|---|---|---|
| `0x00a3c3d0` | **0x0D** | `Arrow_SpawnProjectileToTarget` (0xB0), `ItemSkillMgr_GetInfoByResId` (0xBE) |
| `0x00cca703` | 0x0E | `SkillMgr_SetOption` (0xFA), `operator_delete` (0x0E ✅) |

Deux fonctions de 0xB0 et 0xBE octets ne peuvent pas être devenues le **même
stub de 13 octets**. Ici la méthode par opcodes donne
`0x00d5a980 → 0x00c6dae0`, soit **0xBE ↔ 0xBE**.

**Neuf écarts de taille** en plus, dont `CUIEdit_SetText` 0x88 → **0x03** et
`Cstr_FormatInt32Grouped` 0x53 → **0x4B9**.

⚠ Un écart n'est pas une preuve d'erreur — une fonction peut légitimement
enfler. La liste est un **ordre de priorité de vérification**, pas un verdict.

## ✅ `g_session` est portée, et son layout mesuré

Le chantier était marqué « offsets déplacés, à rescanner **en jeu** ». Il se
règle **statiquement** :

```
g_session : 0x015FA3C0 -> 0x014B73B0      (delta -0x143010)
```

Et le bloc n'a **pas** été déplacé tel quel : il a été **réorganisé par
paliers**. 49 membres relevés, **14 écarts distincts** — d'où l'échec des
approches par delta constant.

| zone | offsets 2025 | écart | membres |
|---|---|---|---|
| en-tête | +0x0 | 0 | 1 |
| — | +0x788 … +0x78C | −40 | 2 |
| chat / vending | +0xF04 … +0xF44 | −48 | 6 |
| **pet + identité** | +0xFF0 … +0x16CC | **−56** | **11** |
| raccourcis | +0x5420 … +0x5424 | −1256 | 2 |
| **homoncule** | +0x543C … +0x55B8 | **−1108** | **8** |
| **état / view-equip** | +0x5918 … +0x5BC6 | **−1660** | **9** |
| titre | +0x6120 … +0x613C | −1764 | 2 |
| poids / inventaire | +0x7F64 … +0x86CD | −2384 … −2424 | 4 |

🔴 **Les membres d'un même sous-système partagent exactement le même écart.**
C'est une validation en soi : des paires fausses donneraient des écarts
dispersés à l'intérieur d'un bloc.

⚠ Les petites variations entre paliers voisins (−1108 vs −1112, −2384 vs −2396
vs −2392) peuvent être de vraies insertions **ou** quelques paires fausses. Les
paliers à un seul membre sont les moins sûrs ; ceux à 8-11 membres, les plus
solides. `kAid`/`kOwnPetAid` a **26 opcodes** de signature.

## Les 43 adresses gagnées

Les plus utiles (⚓ = entrée d'annuaire, elle porte plusieurs déclinaisons) :

| 2025 | 2026 | nom |
|---|---|---|
| `0x015fa3c0` | `0x014b73b0` | ⚓ **`kSessionAddr`**, `kJobNameCtx`, `kOptionContextAddr` |
| `0x0131f6bc` | `0x011f1aac` | ⚓ `kInventoryWndSlot` |
| `0x00a9a7d0` | `0x00a97ca0` | ⚓ `kCallGlobalVaAddr` |
| `0x006a6570` | `0x00711a00` | ⚓ `kInfoSetIdAddr` |
| `0x015ffd78` | `0x014bc6ec` | ⚓ `kStateHolderAddr` |
| `0x015fb9f8` | `0x014b89b0` | ⚓ `kBaseLvl`, `kJobLevel` |
| `0x015fb9a8` | `0x014b8960` | `kOwnCharId` |
| `0x00b314f0` | `0x00b0f8d0` | `CNavigation::SearchRoute` |
| `0x00d5bb40` | `0x00c6eb90` | `kJobDisplayName` |
| `0x01254d70` | `0x01127350` | `kCGuildMgrPtr` |
| `0x0159c230` | `0x0146e938` | `kGuildIdAddr` |

Liste complète et exploitable : [port_opcode_pairs.json](port_opcode_pairs.json)
(champ `confiance` : `haute` 489, `moyenne`, `A VERIFIER` 16).

## ✅✅ Second identifiant stable : l'ID DE FENÊTRE — et la validation croisée

La limite ci-dessus (« ne voit pas l'UI ») se lève avec un autre identifiant que
le compilateur ne touche pas : **l'id passé à `MakeWindow`**.

| | 2025 | 2026 |
|---|---|---|
| fabrique | `UIWindowMgr_MakeWindow` `0x00A39340` (0x9544) | `sub_A07BC0` `0x00A07BC0` (0x93B7) |
| switch | `0x00A394CF` | `0x00A07C7E` |
| ids | **0..362** (363) | **0..369** (370) |

Retrouvée en 2026 (où elle n'est pas nommée) en listant les fonctions à gros
switch : la paire ressort par (nombre de cases, taille).

🔴 **Ces switches sont à table d'INDIRECTION** (363 valeurs → 233 cibles) :
`si.lowcase` y est inexploitable (il rend une adresse). Il faut
**`idaapi.calc_switch_cases(head, si)`**, qui rend les vraies valeurs de case
quel que soit le type de switch.

### 🔴🔴 Les ids sont-ils stables ? — le vérifier, pas le supposer

7 ids de plus en 2026 : ajoutés à la fin (ids stables) ou insérés (tout
décalé) ? Deux tests, dont un mauvais :

- **Par la taille des cases : ne discrimine RIEN** (médiane 0,687 au décalage 0
  contre ~0,67 partout). Pire, un seuil à 0,8 donnait 1,9 % au bon alignement
  contre 20 % ailleurs — un « verdict » exactement à l'envers. La cause : le code
  2026 est plus compact (médiane 131 instructions contre 187), donc le ratio
  tombe sous le seuil pour **tous** les ids. ⚠ **Un seuil mal placé ne rend pas
  un test muet, il le rend MENTEUR.**
- **Par l'ordre des adresses de case : net.** Le compilateur émet les cases dans
  l'ordre du source ; si les ids sont stables, le classement se conserve.
  Corrélation de rang :

| décalage | −5 | −1 | **0** | +1 | +5 |
|---|---|---|---|---|---|
| r | 0,48 | 0,61 | **0,90** | 0,59 | 0,48 |

→ **ids stables**, les 7 nouveaux (363-369) sont bien ajoutés à la fin.

### Résultat, et la meilleure validation du lot

**198 paires**, et le test des tailles est le plus net de tous :

| ratio de tailles | médiane | > 0,8 | > 0,6 |
|---|---|---|---|
| **paires par id de fenêtre** (182) | **1,000** | **95,6 %** | **100 %** |
| témoin aléatoire (3 640) | 0,615 | 25,5 % | 51,8 % |

🔴🔴 **Et surtout : les deux méthodes se croisent.** Opcodes de protocole et
ids de fenêtre sont deux tables sans le moindre rapport :

| | |
|---|---|
| recouvrement des deux jeux | **9** |
| concordent | **9** |
| divergent | **0** |

C'est la validation indépendante qui manquait au début de ce document. Le
recouvrement est petit, mais 9/9 sur des tables disjointes n'est pas un hasard.

**Bilan consolidé : 812 paires, 44 adresses du manifeste → 157 → 201 / 784
(25,6 %).** Les 9 confirmées deux fois sont marquées `opcode+fenetre` dans
[merged_pairs.json](merged_pairs.json).

⚠ **La fenêtre 275 (banque) tombe sur le `defjump` en 2026** alors qu'elle a un
vrai case en 2025 : elle n'est peut-être plus construite par la fabrique. À
vérifier avant de porter quoi que ce soit qui en dépende.

## ✅✅ Généralisation : ONZE tables, signatures composites

Le procédé vaut pour tout switch indexe par un identifiant que le protocole ou
les données figent. Onze paires de tables ont été appariées (repérées en
listant les fonctions à gros switch, puis rapprochées par nombre de cases et
taille) :

| table | plage de valeurs | nature | 2025 → 2026 |
|---|---|---|---|
| `dispatch` | 115..3125 | opcode de paquet | 3011 → 3029 |
| `effectres` | 13..2422 | effect id | 2410 → 2398 |
| `sw491` | 491..2437 | effect id | 1947 → 1923 |
| `effectsnd` | 1101..2422 | effect id | 1322 → 1308 |
| `effectupd` | 0..490 | effect id | **491 → 491** |
| `sw478` | 230..707 | skill id | **478 → 478** |
| `skillcast` | 2202..2610 | skill id | **409 → 409** |
| `makewindow` | 0..362 | id de fenêtre | 363 → 370 |
| `saverect` | 10..361 | id de fenêtre | 352 → 336 |
| `weaponcombo` | 4002..4316 | item id | **315 → 315** |
| `sw289` | 291..579 | skill id | **289 → 289** |

✅ **Cinq tables sont rigoureusement identiques** des deux côtés (même nombre de
valeurs ET de cibles) : leurs identifiants sont stables sans discussion.

La signature devient l'ensemble des couples **(table, valeur)** : un objet vu par
plusieurs tables indépendantes est d'autant plus discriminant. **11 321 cases
communes**, 1 121 paires.

### Bilan consolidé des trois passes

| | paires |
|---|---|
| par opcode (dispatch seul, appels à profondeur 1) | 623 |
| par id de fenêtre | 198 |
| par les 11 tables, signature composite | 1 121 |
| **union** | **1 181** |
| **conflits entre passes** | **0** |

➡ **157 → 205 / 784 adresses du manifeste (26,1 %)**, +48.

### ⚠ Ne pas surestimer la validation croisée

756 paires sont « confirmées par au moins deux passes » — **ce chiffre est
trompeur** : la passe multi-tables **contient** `dispatch` et `makewindow`, donc
un accord entre elles teste la robustesse aux paramètres (profondeur, cache),
pas l'indépendance.

En ne comptant que les accords entre **familles réellement disjointes**
(réseau / UI / effet / skill / item) :

| | |
|---|---|
| paires vues par 2 familles indépendantes | **42** (4 %) |
| divergences | **0** |
| parmi les 48 adresses gagnées | 7 |

C'est peu, mais c'est **la seule validation croisée qui vaille**, et elle est
parfaite. Le gros de la confiance vient du **témoin aléatoire** (médiane 0,999
contre 0,303), pas du recoupement.

## ✅ Troisieme test independant : la MONOTONIE

Ni les tailles, ni les noms, ni les recoupements n'y entrent. Le compilateur
reordonne des fonctions d'un build a l'autre, mais pas au hasard : l'ordre est
largement preserve. Une paire dont la cible casse l'ordre par rapport a ses
voisines immediates est donc suspecte.

| | paires proposées | témoin aléatoire |
|---|---|---|
| inversions d'ordre entre voisines consécutives | **4,1 %** | 50,2 % |

Et en fenêtre glissante (4 voisines de chaque côté, enveloppe élargie d'une
pleine amplitude) : **8 paires sur 1 181 sortent de l'enveloppe (0,7 %), et
aucune n'est utilisée par Bourgeon.** Liste dans
[monotonic_outliers.json](monotonic_outliers.json).

➡ Récapitulatif des contrôles, tous avec témoin négatif :

| test | paires | témoin | indépendant de |
|---|---|---|---|
| ratio de tailles | médiane 0,999 | 0,303 | l'ordre, les noms |
| monotonie | 4,1 % d'inversions | 50,2 % | les tailles, les noms |
| noms des deux côtés | 0 contradiction réelle | — | tout le reste |
| familles de tables disjointes | 42 paires, 0 divergence | — | tout le reste |

## ✅✅ Les trois listes d'objets : CONFIRMEES par les accesseurs

Bourgeon lit trois modeles dans la session. Leurs adresses 2026 etaient
**extrapolees** ; elles sont maintenant **mesurees**.

🔴 Ce qui a debloque : le client expose des **accesseurs triviaux** de 7
octets, `mov eax, [ecx+disp32]` / `retn` avec `ecx = g_session`. Le deplacement
y est **en clair**, et ils sont **contigus** — 66 en 2025, dont deux blocs dans
la zone qui nous interesse.

Une de ces fonctions etait deja appariee : `kPartyMemberCount`
`0x00d5cf50` → `0x00c6ffa0`.

```
2025  d5cf50   8b 81 c0 17 00 00    mov eax, [ecx+17C0h]
2026  c6ffa0   8b 81 88 17 00 00    mov eax, [ecx+1788h]      17C0 - 1788 = 0x38
```

Et toute la sequence se superpose, dans le meme ordre :

| | deplacements |
|---|---|
| 2025 | 170C 17C8 16F4 174C 1724 1734 173C 1744 172C … 17C0 1704 1714 16FC 171C |
| 2026 | 16D4 1790 16BC 1714 16EC 16FC 1704 170C 16F4 … 1788 16CC 16DC 16C4 16E4 |

✅ **14 accesseurs sur 14, ecart −0x38 pour chacun.** Le palier est mesure, plus
suppose, sur la plage **0x16F4..0x17C8** (2025).

| modele | offset | 2025 | **2026** | statut |
|---|---|---|---|---|
| storage | +0x1718 | `0x015FBAD8` | **`0x014B8A90`** | ✅ encadre par 0x1714 et 0x171C |
| cart | +0x1720 | `0x015FBAE0` | **`0x014B8A98`** | ✅ encadre par 0x171C et 0x1724 |
| inventaire | +0x16F0 | `0x015FBAB0` | **`0x014B8A68`** | ⚠ **4 octets** sous le plus petit deplacement mesure |

⚠ L'inventaire reste a 4 octets en dehors de la plage mesuree. C'est tres
probable, ce n'est pas prouve.

En prime, `Cart_GetCount` `0x00d5ce50` (+0x1724) → **`0x00c6fea0`** (+0x16EC),
par sa position dans la sequence. Details : [session_lists_confirmed.json](session_lists_confirmed.json).

### ❌ Ce qui n'a PAS marche, et pourquoi

Avant d'en arriver la, j'ai essaye de comparer le **profil des deplacements**
chez les 935 fonctions (2025) / 844 (2026) qui referencent `g_session` :

| plage | meilleur decalage | second | attendu |
|---|---|---|---|
| 0x400..0x600 | −8 (0,671) | 0,477 | — |
| 0x1000..0x1500 | −8 (1,000) | 0,536 | **−56** ❌ |
| 0x1500..0x1800 | −0x38 (0,800) | 0,280 | **−56** ✅ |

La plage des trois listes donnait la bonne reponse, mais 0x1000..0x1500 donnait
−8 la ou les paliers mesurent −56. 🔴 **Un deplacement ne porte pas sa base** :
ces fonctions manipulent plusieurs structures, celle de l'acteur etant decalee
de −8. Le profil melangeait deux structures ; le « pic » pouvait n'etre qu'un
artefact. Il a fallu passer par des fonctions dont on SAIT que `ecx` est la
session — c'est ce que garantissent les accesseurs triviaux.

Les adresses absolues, elles, n'apparaissent nulle part (0 occurrence, 0 xref).

## ✅ Troisieme test independant : la MONOTONIE

Ni les tailles, ni les noms, ni les recoupements n'y entrent. Le compilateur
reordonne des fonctions d'un build a l'autre, mais pas au hasard : l'ordre est
largement preserve. Une paire dont la cible casse l'ordre par rapport a ses
voisines immediates est donc suspecte.

| | paires proposées | témoin aléatoire |
|---|---|---|
| inversions d'ordre entre voisines consécutives | **4,1 %** | 50,2 % |

Et en fenêtre glissante (4 voisines de chaque côté, enveloppe élargie d'une
pleine amplitude) : **8 paires sur 1 181 sortent de l'enveloppe (0,7 %), et
aucune n'est utilisée par Bourgeon.** Liste dans
[monotonic_outliers.json](monotonic_outliers.json).

➡ Récapitulatif des contrôles, tous avec témoin négatif :

| test | paires | témoin | indépendant de |
|---|---|---|---|
| ratio de tailles | médiane 0,999 | 0,303 | l'ordre, les noms |
| monotonie | 4,1 % d'inversions | 50,2 % | les tailles, les noms |
| noms des deux côtés | 0 contradiction réelle | — | tout le reste |
| familles de tables disjointes | 42 paires, 0 divergence | — | tout le reste |

## ⚠ Les trois listes d'objets : EXTRAPOLATION, pas mesure

Bourgeon lit trois modeles dans la session, et **aucun des trois n'est mesure** :

| | offset | 2025 | 2026 **suppose** |
|---|---|---|---|
| inventaire (`item_cell.h`) | +0x16F0 | `0x015FBAB0` | `0x014B8A68` ? |
| storage (`storage_window.h`) | +0x1718 | `0x015FBAD8` | `0x014B8A90` ? |
| cart (`cart_viewer.h`) | +0x1720 | `0x015FBAE0` | `0x014B8A98` ? |

Ce calcul suppose que le palier **−56** se prolonge. Il est stable sur **11
membres** de +0xFF0 a +0x16CC, et les trois cibles ne sont qu'a **0x24 a 0x54
octets** du dernier membre mesure — c'est donc plausible.

🔴 **Mais c'est une hypothese, pas un releve.** Le layout a change de palier
14 fois ; rien n'interdit une insertion dans cet intervalle. Ces trois valeurs
sont a **confirmer avant tout usage** — ce sont precisement les modeles dont
dependent l'inventaire, le storage et le cart, donc une erreur y serait couteuse.

### ❌ La verification par les DEPLACEMENTS a ete tentee, elle ne tranche pas

Les trois adresses absolues **n'existent nulle part** dans le binaire (recherche
d'octets : 0 occurrence) : le natif accede a ces modeles par deplacement depuis
`g_session`. On a donc releve le profil des deplacements chez les **935
fonctions** (2025) / **844** (2026) qui referencent `g_session`, et cherche le
decalage qui superpose le mieux les deux profils :

| plage | meilleur decalage | second | attendu |
|---|---|---|---|
| 0x400..0x600 | **−8** (0,671) | 0,477 | — |
| 0x1000..0x1500 | **−8** (1,000) | 0,536 | **−56** ❌ |
| 0x1500..0x1800 | **−0x38** (0,800) | 0,280 | **−56** ✅ |

La plage des trois listes donne bien −0x38, avec un pic net. **Mais
0x1000..0x1500 donne −8 la ou les paliers mesurent −56** — une contradiction.

🔴 **La methode est invalide, et il faut le dire** : un deplacement **ne porte
pas sa base**. Ces fonctions manipulent plusieurs structures, et celle de
l'acteur est decalee de −8. Le profil melange donc deux structures, et le
« pic » a −0x38 peut n'etre qu'un artefact du melange.

S'y ajoute que **`storage` (+0x1718) et `cart` (+0x1720) n'apparaissent pas du
tout** comme deplacements en 2025 : meme valide, la mesure n'aurait rien dit sur
eux. Les trois valeurs restent donc a confirmer **en jeu**.

## ✅✅ Passe finale : 217 TABLES appariees automatiquement

Choisir les tables a la main ne passe pas a l'echelle. Une table s'identifie par
sa **forme** — (plage de valeurs, nombre de valeurs, nombre de cibles) — qui ne
depend d'aucune adresse. On peut donc apparier les tables ELLES-MEMES :

| critere | paires |
|---|---|
| forme EXACTE (lo, hi, nvals, ntgts) | **199** |
| meme plage + nombre de valeurs | 18 |
| **total** | **217** tables, dont 108 nommees cote 2025 |

**30 281 cases communes**, 2 070 fonctions et 1 046 globales atteintes.

🔴 **Piege du pont MCP** : au-dela de ~1 min il coupe et **tue le script**
cote IDA sans rien ecrire. La premiere tentative sur 217 tables a echoue
silencieusement de cette facon. La parade : **traiter par lots** (`extract_batch.py`,
`batch.json`) avec un **cache d'analyse persiste sur disque** entre les lots —
15 tables en 8 s, puis 75 en 26 s grace au cache. ⚠ Un timeout du pont ne veut
PAS dire que le script a echoue : verifier le fichier de sortie avant de relancer,
sous peine de double execution.

### Bilan des quatre passes

| passe | paires |
|---|---|
| opcode (dispatch seul) | 623 |
| id de fenetre | 198 |
| 11 tables, signature composite | 1 121 |
| **217 tables appariees automatiquement** | **943** |
| **UNION** | **1 827** |
| **conflits** | **0** |

➡ **157 → 271 / 784 adresses du manifeste (20 % → 34,6 %)**, +114.

Controles sur le jeu final : ratio de tailles **mediane 1,000** (temoin 0,258),
monotonie **5,1 %** d'inversions (temoin 50,0 %).

### Ce que le portage precedent avait faux

11 recouvrements, **4 divergences** — et les tailles tranchent :

| source | portage precedent | tables | verdict |
|---|---|---|---|
| `Arrow_SpawnProjectileToTarget` 0xB0 | 0x0D — **0,07** | 0xBE — **0,93** | tables ✅ |
| `SkillMgr_SetOption` 0xFA | 0x0E — **0,06** | 0xFA — **1,00** | tables ✅ |
| `Weapon_ItemIdToWeaponClass` 0x44 | 0x52 — 0,83 | 0x44 — **1,00** | tables, sans certitude |
| `kItemSkillInfoDtor` 0x57 | 0x5F — 0,92 | 0x57 — 1,00 | 🔴 **rejete par la monotonie** |

🔴 Le dernier cas est exactement ce pour quoi on garde plusieurs tests
independants : les tailles ne departageaient pas (0,92 contre 1,00), la
**monotonie** l'a rejete — sa cible sort de l'enveloppe de ses voisines. Sur ce
point le portage precedent a probablement raison. Avec
`kSpriteTexFactoryGetAddr`, ce sont les **2 seules** paires du manifeste que la
monotonie met en cause (sur 1 827).

## Limites — ce que cette méthode ne peut PAS faire

- Chaque switch ne voit que ce qui lui est **atteignable** : le dispatch de
  paquets ignore l'UI, la fabrique de fenêtres ignore le réseau. L'audio, le
  rendu et les fichiers restent hors d'atteinte des deux. C'est pourquoi
  623 paires ne donnent que 43 adresses du manifeste : Bourgeon travaille
  surtout ailleurs.
- Elle **se tait** sur les objets à signature ambiguë (200 fonctions, 81
  globales) — c'est voulu.
- Un objet touché par **exactement** les mêmes opcodes qu'un autre est
  indiscernable ; deux vrais jumeaux ne seront jamais départagés.

## Rejouer

Les scripts sont dans [scripts/](scripts/). Ils ecrivent leurs fichiers
intermediaires dans le repertoire indique en tete de chaque script (le
scratchpad de session) — a adapter avant de rejouer.

| ordre | script | ou |
|---|---|---|
| 1 | `extract_cases2.py` | **dans CHAQUE IDA** (2025 puis 2026) |
| 2 | `match_v2.py _d1` | en local — apparie par signature |
| 3 | `annotate_pairs.py` | **dans CHAQUE IDA** — noms et tailles |
| 4 | `validate_pairs.py` | en local — noms + tailles contre temoin aleatoire |
| 5 | `audit_sizes.py` | **dans CHAQUE IDA** — pour l'audit du portage existant |
| 6 | `audit_report.py` | en local — collisions et ecarts de taille |
| 7 | `build_deliverable.py` | en local — produit les deux JSON |
| — | `session_layout.py` | en local — le tableau des paliers de `g_session` |
| — | `utility.py` | en local — apport reel au manifeste |
