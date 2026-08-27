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

## Limites — ce que cette méthode ne peut PAS faire

- Elle ne voit que ce qui est **atteignable depuis le dispatch de paquets**. Le
  rendu, l'UI pure, l'audio, les fichiers en sont hors d'atteinte. C'est pourquoi
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
