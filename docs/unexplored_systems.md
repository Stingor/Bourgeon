# Les zones vierges du client — et lesquelles Moonlight sert vraiment

> Relevé du **2026-08-31**, **révisé le 2026-09-02**. Client
> `2025-07-16_Ragexe`, serveur `moonlight` (fork rAthena), configuration de
> production.
>
> 🔴🔴 **Lire d'abord la correction du 2026-09-03, en fin de §5** : la
> conclusion « il ne reste aucune candidate vivante et vierge » est **fausse**,
> et les deux vecteurs de ce document ont un angle mort commun. Six fenêtres
> vivantes s'ouvrent sur un **skill** :
> [skill_driven_windows_re.md](skill_driven_windows_re.md).
>
> ⚠ La révision du 2026-09-02 a **retiré deux candidates sur trois** (reforge,
> enchantement) et soldé la troisième (fusion d'objets). Elle a ajouté les
> questions **3** et **4** du §5 — l'ouvreur chargé/atteignable, et la base
> réellement peuplée. Les deux nouvelles questions suffisent à trancher les
> trois cas.

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

⚠ **Éteint ne veut pas dire abandonné.** Le **barter** est éteint aujourd'hui et
**prévu** : la marche à suivre pour l'allumer est écrite dans
[barter_market_re.md](barter_market_re.md) §8.

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
| **Fusion d'objets** (`CMergeItemWnd`) | 227 | **P** — `HEADER_CZ_REQ_MERGE_ITEM` et `0x0974` enregistrés, `clif_merge_item_*` complet | ✅ **relevée le 2026-09-02** → [merge_item_re.md](merge_item_re.md). Le système est **complet et correct des deux côtés** ; seul son NPC est posé sur une carte sans warp |
| ~~**Reforge d'objet**~~ (`UIItemReformWnd`) | 348 | **P**, mais **base VIDE** | ⛔ **morte** — `db/item_reform.yml` n'a **aucun `Body:`** (gabarit nu), et **aucun script chargé** n'appelle `item_reform(` |
| ~~**Enchantement**~~ (`CUIEnchantUI`) | 10006 | **P**, mais **base VIDE** | ⛔ **morte** — `db/item_enchant.yml` sans `Body:`, aucun appel à `item_enchant(` dans `moon/`. Les quatre `enchan_*.npc` chargés sont l'enchantement **par dialogue**, sans fenêtre. Reste l'une des quatre fenêtres qui **bloquent l'ouverture de la banque** (dispatch §4) |
| **Salon de coiffure** (`UIStylingShopWnd`) | 281 | **P** + **C** — `feature.stylist: on`, 5 paquets `STYLE` | déjà décrit en mémoire ; `moon/stylist.npc` existe |
| **Barter market** (troc) | 334, 335, 341, 342 | **C** — `feature.barter: off` **et** `barter_extended: off`, **et** `moon/barters.yml` sans `Body:` | 🔜 éteint aujourd'hui mais **prévu par l'utilisateur** — ✅ relevé, et la marche à suivre pour l'allumer est écrite : [barter_market_re.md](barter_market_re.md) §8 |

### Mort sur Moonlight — ne pas ouvrir

| famille | fenêtres | pourquoi |
|---|---|---|
| **Roulette** | 268 | **C** — `feature.roulette: off` |
| **Carte de présence** | 328 | **C** — `feature.attendance: off` |
| **Lapine (boîtes)** | 290, 302 | **P** — *zéro* occurrence de `lapine` dans `src/map/` : rAthena de ce fork ne connaît pas le système |
| **Rune system** | 361, 362 | **P** — rien côté serveur |
| **Clans** | 237 (`UIClanInfoManageWnd`) | **P** — aucun paquet `CLAN` enregistré, aucun `conf/clans.yml`. Le code existe dans `src/map/`, rien ne le déclenche |
| **Boutique « Para » / mileage** | 184, 254, 255, 256, 267 | **S** — pas de boutique cash active (cf. [[project_cashshop_re]], natif mort) |

⚠ `feature.privateairship: off` est acquis, mais **aucune fenêtre n'a été
rattachée à l'airship privé** dans ce relevé.
~~la ligne « 308 `UIShowWarpWnd` » de la liste §8 du dispatcher n'a pas été
vérifiée, et le nom suggère plutôt une liste de warps. Non tranché.~~
→ **tranché le 2026-09-03**, voir
[skill_driven_windows_re.md](skill_driven_windows_re.md) §5 : **308 n'a rien à
voir avec l'airship**. C'est bien une liste de warps, nourrie par ZC `0x0ABF`
(`Recv_ZC_WARPLIST_NOSKILL_0x0ABF` `0xd0bff0`, son seul ouvreur) — que **rien
dans `src/map/` n'émet**. Elle est donc **morte**. 🔴 La liste de warps
réellement jouée est la fenêtre **39 `UIChooseWarpWnd`**, servie par le `case`
voisin ZC `0x0ABE` ; les deux formats ne diffèrent que du `skillId.W`.

### Sans serveur du tout — purement client

| famille | fenêtres | nature |
|---|---|---|
| **Replay** | 186, 187, 198 | local, lit un `.brw` — déjà décrit par [[reference_replay_reassembly_re]] |
| **Popups « accepter »** | 70, 104, 105, 110, 119, 138, 163 | une classe par motif (guilde, couple, bébé…) ; ce sont des boîtes, pas des systèmes |
| **Framework `CUI`** | 10001, 10002, 10005, 10008-10010, 10012, 10030, 10033 | second système d'interface, cf. dispatch §6 et §13 |

## 3 bis. 🔴 Le balayage par les COMMANDES DE SCRIPT (2026-09-02)

Partir des fenêtres du client donne des candidates que rien n'ouvre. Le vecteur
inverse est bien meilleur : **recenser les commandes de script qui ouvrent une
interface, puis compter leurs appels dans les scripts CHARGÉS** (`moon/`, la
seule arborescence lue — `map.cpp:4383`).

Les 16 commandes de ce fork (`grep -oE 'BUILDIN_DEF2?\((open[a-z_0-9]*|…),'`) :

| commande | appels dans `moon/` | verdict |
|---|---|---|
| `openstorage` | **18** | entrepôt — déjà remplacé par Bourgeon |
| `openstorage2` | 0 | — |
| `opendressroom` | **1** | 🔓 `moon/atcommands.npc:16` — **`@dressroom`, niveau 99/99** : STAFF seulement. Ouvre **ZC `0x0A02`** → `MakeWindow(0x103)` = **259 `UISecondCostumeWnd`**, handler `ZC_DressRoomOpen_Handler` 0x00cf8030. ⚠ Le handler **ignore** le `<view>.W` du paquet |
| `openbank` | **1** | banque (275) — déjà relevée |
| `mergeitem` | **1** | fusion — [merge_item_re.md](merge_item_re.md), NPC sur `itemmall` |
| `guildopenstorage_log` | **1** | 🎯 [guild_storage_log_re.md](guild_storage_log_re.md) — **la seule trouvaille jouable du relevé** |
| `openstylist` | 0 | alors que `feature.stylist: on` — le NPC `moon/stylist.npc` n'utilise pas cette commande |
| `open_roulette`, `openmail`, `openauction`, `open_quest_ui`, `enchantgradeui`, `item_reform`, `item_enchant`, `opentips`, `refineui`, `reputationui` | **0** | aucun ouvreur chargé |

⚠ **`opentips` et `reputationui` ne sont appelées nulle part**, même dans `npc/` :
ce ne sont pas des fonctionnalités désactivées, ce sont des commandes que
personne n'utilise. `refineui` a 3 appelants, tous dans `npc/` (non chargé).

➡ **Refaire ce tableau après toute modification de `scripts_moon.conf`** : il
coûte deux `grep` et il dit, en une passe, quelles interfaces le serveur peut
réellement faire apparaître.

### 🎯 Ce que le balayage élargi a donné (2026-09-02, 30 agents)

Six recensements indépendants — commandes `@` maison, protocole `clif_bourgeon_*`,
ZC réellement émis, buildins produisant de l'UI, couverture des `docs/`, systèmes
maison de `moon/` — puis vérification des candidats par les quatre questions.

**474 entrées brutes → 297 candidats touchant l'interface.**

⚠ **PLAFOND ASSUMÉ : seuls les 24 premiers ont été vérifiés ; 273 ne l'ont pas
été.** Sur ces 24 : **18 déjà documentés**, **4 réservés au staff**
(`@moche`, `@celebrate`, `@trollolol`, `@untrollolol` — des commandes C++ de
`src/custom/atcommand.inc`), **2 chantiers réels** (la même trouvaille vue par
deux angles).

🎯 **La trouvaille : `@searchid` / `@deepsearchid`** — ouvertes à **tout joueur**,
annoncées dans le MOTD, absentes de `docs/` et de `src/`. Elles traversent quatre
opcodes que le catalogue liste mais qu'aucun document de sujet ne couvrait :
`0x02F0` / `0x02F1` / `0x02F2` (barre de progression NPC) et `0x08B3`
(texte flottant). Relevé : [npc_progress_showscript_re.md](npc_progress_showscript_re.md).

⚠ **Deux réserves honnêtes** : (1) c'est de la **valeur ajoutée**, pas une
réparation — la commande marche ; (2) le dédoublonnage n'a écarté **aucun**
doublon sur 297 candidats, ce qui trahit une clé de normalisation trop stricte :
le vrai nombre de sujets distincts est plus bas, et les 273 non examinés
contiennent surtout du bruit.

## 4. Ce qui n'a pas été tranché

Honnêteté sur les limites du présent relevé :

- ~~Les **sous-onglets de stockage** (146-152, 153, 309)~~ → **tranché le
  2026-09-02**, voir [guild_storage_log_re.md](guild_storage_log_re.md) §6 : ce
  ne sont pas un système à part mais les **sous-panneaux de `UIItemStoreWnd`**
  (la fenêtre de stockage, id **33**), fermés **en bloc** par son `OnMsg`
  (`0x954690` : boucle 0x92..0x98, puis 0x135, puis 0x99). Or Bourgeon a déjà
  remplacé cette native — elle **ne naît plus**. **Aucun chantier.**
  ⚠ `UIItemStore*` = l'**entrepôt**, pas une boutique, malgré le voisinage de
  `UIItemShopWnd` dans le binaire.
- ~~**`UIMemorialDunWnd`** (137) : le lien entre le système d'instance et cette
  fenêtre-là n'a pas été établi.~~ → **tranché le 2026-09-01**, voir
  [memorial_dungeon_re.md](memorial_dungeon_re.md) : les cinq opcodes
  `0x02CB`..`0x02CF` correspondent champ par champ, la fenêtre s'ouvre par
  `Alt+B`, et 39 instances sont déclarées en base.
- ~~**`UIRenewQuestUI` / `CUIOngoingQuestInfo` / `CUIRecommendedQuestInfo`**
  (10008-10010)~~ → **tranché le 2026-09-02, en deux temps.**
  1. Côté **serveur** : leur seul déclencheur possible est la commande
     `open_quest_ui` (`script.cpp:27264`), et **aucun script chargé ne
     l'appelle** (vérifié avec témoin positif). Pas de transport serveur.
  2. 🔴 **Mais elles n'en ont pas besoin.** `10008` est ouverte **par l'icône
     `quest` de la barre de menu** (commande **361**,
     `UIWindowMgr_ToggleWindowById(0x2718)`), et `10009`/`10010` par le module
     de quête lui-même (`sub_647120`, `sub_A0B160`). Ce sont des fenêtres
     **client**, alimentées par les opcodes de quête déjà couverts par
     [[project_quest_tracking_re]].
  ⇒ **Atteignables, et déjà servies.** Voir la table complète dans
  [basic_info_re.md](basic_info_re.md) §2. Au passage : `10002`
  `CUIAdventureGuide` a elle aussi une icône (commande **581**), mais Moonlight
  la **cache** par patch WARP.
- Les fenêtres **`UISeekParty*`** sont classées vivantes sur la seule foi des
  paquets. Aucune n'a été ouverte en jeu.
- ~~**`UIGuild_Storage_Log`** (253)~~ → **tranché le 2026-09-02**, et c'est la
  meilleure surprise du relevé : voir
  [guild_storage_log_re.md](guild_storage_log_re.md). Le paquet **est** dans le
  répartiteur (**case 2522 = ZC `0x09DA`**, handler `0x00cb1d50`), les 12 champs
  de l'entrée s'alignent au champ près, et surtout **un joueur peut l'ouvrir
  aujourd'hui** : le NPC kafra de Moonlight (`moon/kafra.npc:96`, chargé)
  appelle `guildopenstorage_log()` derrière une garde `GUILD_PERM_ALL`.
  🔴 La fenêtre ne s'ouvre que sur `result == 1` (`FINAL_SUCCESS`) — protocole
  en plusieurs envois. Seul point à vérifier : l'existence de la table SQL
  `guild_storage_log`, que le dépôt ne porte pas.

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

➡ **Le critère « P » est nécessaire, pas suffisant.** Il faut y ajouter
**quatre** questions, désormais posées à toute candidate — les deux dernières
ajoutées le **2026-09-02** par le relevé de la fusion d'objets :
1. **Un joueur peut-il ouvrir la fenêtre ?** (chercher un ouvreur : raccourci,
   commande, bouton d'une autre fenêtre — pas seulement un `MakeWindow`) ;
2. **Le transport est-il bien le serveur de map ?** (plusieurs sous-systèmes de
   ce client passent par le service web `AssistAddr`, cf.
   `external_settings_re.md` §4) ;
3. 🔴 **L'ouvreur est-il CHARGÉ, et sa carte ATTEIGNABLE ?** Un NPC qui appelle
   la bonne commande ne sert à rien si son fichier n'est pas dans
   `moon/scripts_moon.conf` (c'est `map.cpp:4383` qui fixe ce fichier comme LA
   liste, `npc/` n'est **pas** chargé), ni si sa carte n'a aucun warp entrant.
   C'est exactement ce qui bloque la fusion d'objets, la reforge et
   l'enchantement : **leurs trois NPC sont sur `itemmall`**, une carte murée ;
4. 🔴🔴 **La BASE de données est-elle remplie ?** Un `db/*.yml` **présent** peut
   n'être qu'un gabarit sans `Body:` — c'est le cas de `item_reform.yml`,
   `item_enchant.yml` et de `moon/barters.yml`. Compter les entrées, jamais se
   contenter de constater le fichier.

### Les candidates restantes, soumises aux deux questions

Test passé le 2026-08-31 : ouvreur cherché parmi **tous** les `push <id>` suivis
d'un appel au gestionnaire de fenêtres, puis remontée du handler jusqu'au `case`
du dispatcher de paquets pour établir le transport.

| fenêtre | ouvreur | transport | verdict |
|---|---|---|---|
| **227 `CMergeItemWnd`** | `ZC_MergeItemOpen_Handler` 0x00d036a0, **case 2413 = ZC `0x096D`** | serveur de map ✅ | ✅ **relevée** → [merge_item_re.md](merge_item_re.md). Protocole, fenêtre, messages et traductions : **tout est bon**. Bloquée uniquement par la carte du NPC |
| ~~**348 `UIItemReformWnd`**~~ | `sub_D09EE0`, **case 2959 = ZC `0x0B8F`** | serveur de map ✅ | ⛔ **retirée** — `db/item_reform.yml` est un gabarit **sans `Body:`** et aucun script chargé n'appelle `item_reform(`. Son NPC (`rgsr_in.txt`) n'est même pas dans `scripts_moon.conf` |
| **359 `UIMacroBlackListCheckWnd`** | `sub_D03520`, handler du **case 3035 = ZC `0x0BDB`** | serveur de map ✅ | ⚠ à traiter comme une **contrainte**, pas comme une fonctionnalité : c'est elle qui arme le barrage de `MakeWindow` (dispatcher §3) |
| 284, 286 (captcha / rapport) | `UI_OpenByOutUiType` — un ouvreur **générique par « type d'UI »** | serveur de map ✅ | le jeu complet des paquets macro-détecteur est enregistré côté serveur |
| **137 `UIMemorialDunWnd`** | **`Alt+B`** (comportement **122**, `push 89h` @0x00A458C8) | serveur de map ✅ | ✅ **relevée** → [memorial_dungeon_re.md](memorial_dungeon_re.md). **Marche de bout en bout** : 5 opcodes `0x02CB`..`0x02CF` alignés champ par champ, 39 instances en base. Deux morceaux de code mort, aucun défaut exploitable |
| ~~10006 `CUIEnchantUI`~~ | commande de script `item_enchant`, jamais appelée | serveur de map ✅ | ⛔ **retirée** — `db/item_enchant.yml` sans `Body:`, aucun appel dans `moon/`. Les `enchan_*.npc` chargés enchantent **par dialogue**, sans fenêtre |

### 🗺 `itemmall`, la carte-entrepôt murée

Les trois candidates « vivantes » restantes avaient leur NPC sur **la même
carte**, `itemmall` : le *Mergician* (fusion), l'*Equipment Reform PR Agent*
(reforge) et le *Devil Enchant Master* (enchantement). La carte est déclarée
(`db/map_index.txt:679`, `conf/maps_athena.conf:790`) mais **aucun warp de
l'arbre `moon/` n'y mène** — les scripts d'upstream qui y conduisent
(`npc/re/merchants/cashmall.txt`) ne sont pas chargés. Elle n'est atteignable
qu'au `@warp`.

➡ **La fusion d'objets (227) est SOLDÉE, et sans défaut à corriger** :
[merge_item_re.md](merge_item_re.md). Pour l'ouvrir aux joueurs il suffit de
déplacer le *Mergician* sur une carte fréquentée — le script porte déjà la
variante `prontera` en commentaire. **Aucun `feature.*` à activer.**

➡ ~~**Il ne reste plus de candidate « vivante et vierge » à relever.**~~ Les
candidates sont soldées (fusion, instance, file de BG, **journal de stockage de
guilde**) ou retirées (recherche de groupe, reforge, enchantement). Ce qui reste
demande une décision de l'utilisateur — allumer le barter, peupler une base,
déplacer un NPC — et non un relevé de plus.

## 🔴🔴 Correction du 2026-09-03 — cette conclusion était FAUSSE

**Six fenêtres vivantes, jouables aujourd'hui et jamais outillées**, ont été
trouvées le lendemain : voir
[skill_driven_windows_re.md](skill_driven_windows_re.md). Dont la liste de
destinations du **Téléport / Warp Portal** (39 `UIChooseWarpWnd`, ZC `0x0ABE`),
que tout Acolyte utilise plusieurs fois par session.

**La cause n'est pas l'inattention, c'est la MÉTHODE.** Les deux vecteurs de ce
document — le tri par paquet + `feature.*` (§3) et le balayage des commandes de
script (§3 bis) — partagent un angle mort : ils ne voient que ce qu'une
**configuration** ou un **script** déclenche. Une fenêtre ouverte parce que le
joueur a lancé une **compétence** n'a ni `feature.*`, ni NPC, ni icône de menu :
elle est invisible aux deux, et vivante quand même.

➡ **Le §3 bis se trompe donc en se déclarant « le bon vecteur ».** Il est
meilleur que le premier, pas suffisant. Le vecteur **exhaustif** est le
troisième : partir du dispatcher de paquets et demander, pour chaque opcode,
**s'il appelle `UIWindowMgr_MakeWindow` et avec quel id** — 3048 cases, 290
opcodes ouvreurs, 39 lignes réelles vers une fenêtre jamais outillée. Recette
reproductible en [skill_driven_windows_re.md](skill_driven_windows_re.md) §6.

🔴 **La leçon du 2026-09-02, en une phrase :** la seule fonctionnalité **déjà
jouable et jamais documentée** de tout ce relevé n'était pas dans la liste des
candidates — c'est le **journal de stockage de guilde** (253), qui n'y figurait
qu'au titre des « non tranchés ». Elle a été trouvée en remontant depuis un
**NPC chargé**, pas depuis une fenêtre. ➡ Balayer `moon/scripts_moon.conf` à la
recherche de commandes de script qui ouvrent une interface est un vecteur plus
sûr que partir des fenêtres du client.

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
4. 🔴 **Ajouté le 2026-09-02** — la même règle, encore, sur les clés de YAML :
   chercher `Guid:` dans `db/` rend **zéro** alors que la clé s'appelle
   `UniqueId:`. J'ai failli conclure qu'aucun objet de Moonlight n'était
   fusionnable, alors que **1315 entrées** de `db/import/item_group_db.yml` en
   distribuent. Le remède est le **témoin positif** : chercher d'abord une
   valeur dont on est certain qu'elle existe, pour valider le motif.
5. 🔴🔴 **Ne jamais `decompile` une adresse de `case`** : `0x00ca89e0` appartient
   à `RecvLoop_DispatchPackets` (0xc3dd octets) et la demande fait **expirer le
   pont MCP**, qui reste occupé plusieurs minutes ensuite. Désassembler une
   plage bornée.
