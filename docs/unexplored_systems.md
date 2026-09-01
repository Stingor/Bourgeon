# Les zones vierges du client — et lesquelles Moonlight sert vraiment

> Relevé du **2026-08-31**. Client `2025-07-16_Ragexe`, serveur `moonlight`
> (fork rAthena), configuration de production.

## 1. À quoi sert ce document

[native_window_dispatch.md](native_window_dispatch.md) §8 a établi que le client
sert **269 fenêtres** et que **148 n'ont jamais été outillées ni décrites**. Il
répond à « qu'est-ce qui existe ». Il ne répond pas à la question qui décide du
travail : **est-ce que ça tourne sur Moonlight ?**

Une fenêtre dont le serveur n'émettra jamais le paquet est un trou de plus, pas
un chantier. Ce document trie les familles vierges selon ce que le serveur
**envoie réellement**, avec la configuration de production.

## 2. 🔴 Le discriminant : `conf/import/` gagne

`conf/battle/feature.conf` n'est **pas** l'état du serveur : `conf/import/battle_conf.txt`
le surcharge, et il diverge du fichier de base sur cinq fonctionnalités. Trois
familles entières que le fichier de base annonce actives sont **éteintes** en
production.

| réglage | `conf/battle/feature.conf` | `conf/import/battle_conf.txt` | qui gagne |
|---|---|---|---|
| `feature.roulette` | `on` | **`off`** | off |
| `feature.attendance` | `on` | **`off`** | off |
| `feature.barter` | `on` | **`off`** | off |
| `feature.barter_extended` | `on` | **`off`** | off |
| `feature.privateairship` | `on` | **`off`** | off |
| `feature.bgqueue` | `on` | `on` | **on** |
| `feature.stylist` | `on` | `on` | **on** |

Lire le fichier de base seul, c'est se fabriquer trois chantiers imaginaires.
(Même piège que [[feedback_rathena_conf_import_overrides]].)

## 3. Le tri

Trois niveaux de preuve, notés dans la colonne « vérifié » :

- **P** — *paquet* : le CZ correspondant est enregistré dans
  `src/map/clif_packetdb.hpp`, ou le ZC est émis par une fonction de `clif.cpp`.
  C'est la preuve forte.
- **C** — *configuration* : tranché par un `feature.*` de `conf/import/`.
- **S** — *source* : le code existe dans `src/map/`, sans que l'atteignabilité
  ait été tranchée.

### Vivant sur Moonlight — des chantiers réels

| famille | fenêtres | preuve | état |
|---|---|---|---|
| **File d'attente de battleground** | 157, 209, 210, 211 | **P** + **C** — 11 opcodes, `feature.bgqueue: on`, 5 scripts BG chargés, 3 terrains en base, noms client/serveur identiques | ✅ **relevé** → [entry_queue_re.md](entry_queue_re.md). 3 défauts identifiés |
| ~~**Recherche de groupe**~~ | 164, 168, 169, 170, 173, 323, 324, 326, 345 | **P** — mais voir ci-dessous | ⛔ **relevée, et RETIRÉE des chantiers** → [party_search_re.md](party_search_re.md) |
| **Anti-macro / captcha** | 284, 286, 287, 288, 359 | **P** + **C** — 6 paquets enregistrés, `macro_detection_*` réglé dans `conf/battle/client.conf` | ⚠ **conséquence pour Bourgeon** : la fenêtre **359** est ce qui arme le barrage de `MakeWindow` (9 ids rendent `NULL`, cf. dispatch §3). Un remplacement branché sur un `case` ne voit rien passer pendant un captcha |
| **Fusion d'objets** (`CMergeItemWnd`) | 227 | **P** — `HEADER_CZ_REQ_MERGE_ITEM` et `0x0974` enregistrés, `clif_merge_item_*` complet | 🔓 vierge |
| **Reforge d'objet** (`UIItemReformWnd`) | 348 | **P** + base `db/item_reform.yml` | 🔓 vierge |
| **Enchantement** (`CUIEnchantUI`) | 10006 | **P** — 8 paquets `ENCHANT` enregistrés | 🔓 vierge ; c'est aussi l'une des quatre fenêtres qui **bloquent l'ouverture de la banque** (dispatch §4) |
| **Salon de coiffure** (`UIStylingShopWnd`) | 281 | **P** + **C** — `feature.stylist: on`, 5 paquets `STYLE` | déjà décrit en mémoire ; `moon/stylist.npc` existe |

### Mort sur Moonlight — ne pas ouvrir

| famille | fenêtres | pourquoi |
|---|---|---|
| **Barter market** | 334, 335, 341, 342 | **C** + **P** — `feature.barter: off` **et** `barter_extended: off`, **et** `moon/barters.yml` ne déclare aucun PNJ. ✅ **relevé quand même** → [barter_market_re.md](barter_market_re.md) |
| **Roulette** | 268 | **C** — `feature.roulette: off` |
| **Carte de présence** | 328 | **C** — `feature.attendance: off` |
| **Lapine (boîtes)** | 290, 302 | **P** — *zéro* occurrence de `lapine` dans `src/map/` : rAthena de ce fork ne connaît pas le système |
| **Rune system** | 361, 362 | **P** — rien côté serveur |
| **Clans** | 237 (`UIClanInfoManageWnd`) | **P** — aucun paquet `CLAN` enregistré, aucun `conf/clans.yml`. Le code existe dans `src/map/`, rien ne le déclenche |
| **Boutique « Para » / mileage** | 184, 254, 255, 256, 267 | **S** — pas de boutique cash active (cf. [[project_cashshop_re]], natif mort) |

⚠ `feature.privateairship: off` est acquis, mais **aucune fenêtre n'a été
rattachée à l'airship privé** dans ce relevé : la ligne « 308 `UIShowWarpWnd` »
de la liste §8 du dispatcher n'a pas été vérifiée, et le nom suggère plutôt une
liste de warps. Non tranché.

### Sans serveur du tout — purement client

| famille | fenêtres | nature |
|---|---|---|
| **Replay** | 186, 187, 198 | local, lit un `.brw` — déjà décrit par [[reference_replay_reassembly_re]] |
| **Popups « accepter »** | 70, 104, 105, 110, 119, 138, 163 | une classe par motif (guilde, couple, bébé…) ; ce sont des boîtes, pas des systèmes |
| **Framework `CUI`** | 10001, 10002, 10005, 10008-10010, 10012, 10030, 10033 | second système d'interface, cf. dispatch §6 et §13 |

## 4. Ce qui n'a pas été tranché

Honnêteté sur les limites du présent relevé :

- Les **sous-onglets de stockage** (146-152, 153, 309 `UIItemStoreFindWnd`) :
  aucun paquet `STORAGE_SEARCH` n'est enregistré, mais l'onglet est peut-être
  purement client. Non tranché.
- ~~**`UIMemorialDunWnd`** (137) : le lien entre le système d'instance et cette
  fenêtre-là n'a pas été établi.~~ → **tranché le 2026-09-01**, voir
  [memorial_dungeon_re.md](memorial_dungeon_re.md) : les cinq opcodes
  `0x02CB`..`0x02CF` correspondent champ par champ, la fenêtre s'ouvre par
  `Alt+B`, et 39 instances sont déclarées en base.
- **`UIRenewQuestUI` / `CUIOngoingQuestInfo` / `CUIRecommendedQuestInfo`**
  (10008-10010) : le suivi de quête est déjà couvert côté opcodes par
  [[project_quest_tracking_re]] ; le rapport avec ces trois fenêtres `CUI` n'a
  pas été vérifié.
- Les fenêtres **`UISeekParty*`** sont classées vivantes sur la seule foi des
  paquets. Aucune n'a été ouverte en jeu.
- **`UIGuild_Storage_Log`** (253) : `clif_guild_storage_log` existe côté serveur
  (15 occurrences dans `src/map/`), mais le paquet n'a pas été retrouvé dans la
  table du client et le lien avec cette fenêtre n'est pas établi.

## 5. La prochaine cible

### 🔴 Ce que la première tentative a appris — « enregistré des deux côtés » ne suffit pas

Ce document désignait d'abord **la recherche de groupe** comme prochaine cible,
sur le seul critère « les paquets sont enregistrés des deux côtés ». Le relevé
qui a suivi ([party_search_re.md](party_search_re.md)) l'a **infirmé** :

- les neuf fenêtres sont **deux générations** ; seule la moderne est atteignable
  par un joueur, et elle ne parle pas au serveur de map — c'est un **service
  HTTP** à sept routes (`/party/add`, `/party/list`, …) ;
- la génération legacy, la seule qui soit à base de paquets, a son opcode
  d'inscription **absent de la table de longueurs du client** (`0x0802` est le
  seul trou d'une suite contiguë), et le serveur le mappe désormais sur
  `PartyInvite2`.

➡ **Le critère « P » est nécessaire, pas suffisant.** Il faut y ajouter deux
questions, désormais posées à toute candidate :
1. **Un joueur peut-il ouvrir la fenêtre ?** (chercher un ouvreur : raccourci,
   commande, bouton d'une autre fenêtre — pas seulement un `MakeWindow`) ;
2. **Le transport est-il bien le serveur de map ?** (plusieurs sous-systèmes de
   ce client passent par le service web `AssistAddr`, cf.
   `external_settings_re.md` §4).

### Les candidates restantes, soumises aux deux questions

Test passé le 2026-08-31 : ouvreur cherché parmi **tous** les `push <id>` suivis
d'un appel au gestionnaire de fenêtres, puis remontée du handler jusqu'au `case`
du dispatcher de paquets pour établir le transport.

| fenêtre | ouvreur | transport | verdict |
|---|---|---|---|
| **227 `CMergeItemWnd`** | `sub_D036A0`, handler du **case 2413 = ZC `0x096D`** | serveur de map ✅ | 🎯 **candidate n°1** — `clif_merge_item_*` est complet côté serveur, et l'ensemble tient en trois opcodes (`0x096D`/`0x096E`/`0x096F` + `0x0974` d'annulation) |
| **348 `UIItemReformWnd`** | `sub_D09EE0`, handler du **case 2959 = ZC `0x0B8F`** | serveur de map ✅ | 🎯 **candidate n°2** — `clif_parse_item_reform_start`/`_close` enregistrés, `db/item_reform.yml` présent |
| **359 `UIMacroBlackListCheckWnd`** | `sub_D03520`, handler du **case 3035 = ZC `0x0BDB`** | serveur de map ✅ | ⚠ à traiter comme une **contrainte**, pas comme une fonctionnalité : c'est elle qui arme le barrage de `MakeWindow` (dispatcher §3) |
| 284, 286 (captcha / rapport) | `UI_OpenByOutUiType` — un ouvreur **générique par « type d'UI »** | serveur de map ✅ | le jeu complet des paquets macro-détecteur est enregistré côté serveur |
| **137 `UIMemorialDunWnd`** | **`Alt+B`** (comportement **122**, `push 89h` @0x00A458C8) | serveur de map ✅ | ✅ **relevée** → [memorial_dungeon_re.md](memorial_dungeon_re.md). **Marche de bout en bout** : 5 opcodes `0x02CB`..`0x02CF` alignés champ par champ, 39 instances en base. Deux morceaux de code mort, aucun défaut exploitable |
| 10006 `CUIEnchantUI` | aucun `MakeWindow` direct — passe par le registre `CUI` | non établi | l'ouvreur reste à trouver (étage [3] du dispatcher) |

➡ **La fusion d'objets (227) est la prochaine cible** : les deux questions
répondent oui, la surface est de trois opcodes, et le serveur la sert déjà sans
aucun réglage à activer.

---

## Annexe — pièges rencontrés pendant ce relevé

1. 🔴🔴 **`docs/opcode_map.md` écrit l'hexa en MAJUSCULES.** `grep 0x08db`
   rend vide ; `grep -i` rend la ligne. Une première passe a conclu « la file
   de battleground n'existe pas dans ce client » sur ce seul silence.
2. 🔴 **`push 9Dh` s'assemble en `68 9D 00 00 00`, pas en `6A 9D`.** Un
   `find_bytes` sur `6A 9D` rend 0 résultat sur un binaire qui en contient 28.
   Un motif d'octets qui ne trouve **rien** doit être validé sur un site connu
   avant d'être cru.
3. Les deux fois, c'est la même règle : [[feedback_absence_needs_measurement]] —
   une recherche vide ne prouve pas l'absence, elle prouve que la recherche a
   rendu vide.
