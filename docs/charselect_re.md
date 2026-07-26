# RE — Écran de sélection de personnage (`UINewSelectCharWnd`)

Client `20250716`. UI `select_character_ver3`. Cette page documente la fenêtre de
sélection de perso, sa grille de slots, et **le chemin pour afficher plus de 15
slots par page**.

RTTI : `.?AVUINewSelectCharWnd@@`. Vtable de base : `0x0101d424`.
Global du slot sélectionné : `g_CharSelect_SelectedSlot` = `0x015f8262` (octet).

## Fonctions clés

| Adresse | Nom (renommé Ghidra) | vtbl | Rôle |
|---------|----------------------|------|------|
| `0x0079a030` | `UINewSelectCharWnd_ctor` | — | Constructeur. Fixe `slots_per_page=15`, alloue tous les tableaux. |
| `0x0079b0f0` | `UINewSelectCharWnd_BuildPage` | idx15 `+0x3c` | Construit les objets-slot de la page courante + boutons/labels. |
| `0x0079d170` | `UINewSelectCharWnd_RenderSlots` | idx20 `+0x50` | Rend les sprites persos (paperdoll) dans chaque slot. |
| `0x0079d590` | *(render sélection)* | idx19 `+0x4c` | Rend le halo du slot sélectionné (site du crash off-by-one, cf. plus bas). |
| `0x0079d610` | `UINewSelectCharWnd_OnMsg` | idx37 `+0x94` | Dispatch messages/boutons (sélection, suppression, pagination, nav clavier). |

## ⚠️ Entrée en jeu : NE JAMAIS envoyer `CH_SELECT_CHAR` soi-même

Bug réel (char-select ImGui) : envoyer seulement `[0x0066][slot]` fait entrer en
jeu, **mais le serveur kicke au premier message de chat** :
`clif_process_message: Player 'X' sent a message using an incorrect name!`.

**Pourquoi.** Le nom du perso local est stocké **obfusqué (XOR)** dans le contexte
global `0x015fa3c0` :

| Élément | Adresse | Signature |
|---|---|---|
| Buffer obfusqué (64 o) | `0x015fab54` (= ctx `+0x794`) | clé XOR via `[0x0122acb8]` |
| `Own_SetCharName` | `0x00d969c0` | `void __thiscall(ctx, const char* name)` — **RET 4** |
| `Own_GetCharName` | `0x00d7fe40` | `char* __fastcall(ecx=ctx)` — désobfusque vers `0x01602568` |
| `Own_SetSex` | `0x00d96c00` | `void __thiscall(ctx, uint sex)` — **RET 4** |

Le chat compose `sprintf("%s : %s", Own_GetCharName(), texte)` → `CZ_REQUEST_CHAT
0x00f3` (@`0x00c90648`). rAthena (`clif.cpp`) compare ce nom à `sd->status.name`
⇒ mismatch = `set_eof`. **Seuls DEUX sites** écrivent ce buffer : `0x0079d717`
(char-select natif) et `0x00d26562` (`OnStateEnter` état `0x1c`).

**Séquence native du bouton « OK »** — `UINewSelectCharWnd_OnMsg 0x0079d610`
(vtbl `+0x94`, **RET 0x18 = 6 args pile**), `msg=6`, `ctrl=0xB8` :

1. garde `wnd+0x13c == 0` (verrou anti double-clic) ;
2. lit **`g_CharSelect_SelectedSlot` `0x015f8262`** ;
3. `CLoginMode_SendMsg(mode, 8, slot, …)` → `CHARACTER_INFO*` ;
4. refuse si `ci+0x9e` (DelRevDate) > 0 ;
5. **`Own_SetCharName(0x015fa3c0, ci+0x6c)`** ← le nom ;
6. **`Own_SetSex(0x015fa3c0, ci+0xae == 99 ? g_Account_Sex : ci+0xae)`** ;
7. liste noire GID, son (cosmétique) ;
8. **`CLoginMode_SendMsg(mode, 0, 0x2712, …)`** → `mode+0xc = 9` ;
9. `wnd+0x13c = 1`.

➡️ **Le paquet `0x0066` n'est PAS envoyé ici** : c'est l'**état 9**
(`CLoginMode_OnStateEnter 0x00d24080`, bloc `0x00d25d63`) qui construit
`[0x0066][g_CharSelect_SelectedSlot]` et l'émet. L'envoyer soi-même = **double envoi**.

➡️ **`g_CharSelect_SelectedSlot` doit être posé AVANT et RESTER** : le handler
`HC_NOTIFY_ZONESVR` (`Net_OnNotifyZoneSvr_EnterGame 0x00d23180`) le **relit** pour
semer le cache local (`g_Own_*`, `g_OwnLook_*`) via cmd `0x2719`. Sans ça, jouer un
slot ≠ 0 sème l'état du **mauvais personnage** (second bug, latent).

**Recette Bourgeon** (`CharSelect::EnterGame`) : poser l'octet `0x015f8262`, puis
piloter `OnMsg(wnd, 0, 6, 0xB8, 0,0,0)` sur `g_pCharSelectWnd` (`0x0131f8bc`,
= mgr `+0x3d4`, id de fenêtre **`0x115`**), avec garde de vtable `0x0101d424` — ce
cache n'est **jamais remis à zéro** à la destruction, il peut pendouiller.

⚠️ `Own_SetCharName` copie **64 octets en aveugle** : lui passer `ci+0x6c`
(`0x6c+0x40 = 0xac ≤ 0xaf`, in-bounds), **jamais** un buffer local plus court.

## Carte des champs (`this + offset`, indices dword `param_1[i]`)

| Offset | dword | Contenu |
|--------|-------|---------|
| `+0x00` | `[0]` | vtable = `0x0101d424` |
| `+0xd0` | `[0x34]` | vecteur de 12 labels de texte latéraux (**hardcodé 12**, sans rapport avec slots/page) |
| `+0xdc` | `[0x37]` | vecteur des labels de nom de perso (taille = slots_per_page) |
| `+0xe8` | `[0x3a]` | **vecteur des objets-slot de la page** (taille = slots_per_page) ← tableau du crash |
| `+0x100`| `[0x40]` | objet « effet sélection » (halo) |
| `+0x108`| `[0x42]` | tableau des enregistrements par slot, **stride `0x15c`**, taille = capacité (`+0x12c`) |
| `+0x10c`| `[0x43]` | tableau d'octets « flags » par slot (init à 1), taille = capacité |
| `+0x110`| `[0x44]` | tableau de `short` par slot (headgear top id), taille = capacité |
| `+0x114`| `[0x45]` | `timeGetTime()` de création (animation) |
| `+0x11c`| `[0x47]` | **page courante** (= `CURSLOT / slots_per_page`) |
| `+0x120`| `[0x48]` | **position sélectionnée dans la page** (= `CURSLOT % slots_per_page`) ← index du crash |
| `+0x124`| `[0x49]` | **nombre de pages** = `ceil(usableSlots / slots_per_page)` |
| `+0x128`| `[0x4a]` | **SLOTS_PER_PAGE = 15** ← champ pilote |
| `+0x12c`| `[0x4b]` | **capacité totale** = `slots_per_page * nb_pages` |
| `+0x130`/`+0x134` | `[0x4c]`/`[0x4d]` | `usableSlots` (2 copies) |
| `+0x138`| `[0x4e]` | slot en cours de renommage (`0xffffffff` = aucun) |
| `+0x13c`| — (octet) | flag « entrée en cours » (verrou anti double-clic) |
| `+0xf4`/`+0xf8` | — | boutons flèche page préc. / suiv. |
| `+0x104`| `[0x41]` | label `"%d / %d"` (num. de page) |

## Le « 15 » : source unique et dérivations

Tout le dimensionnement part d'**une seule instruction** dans le constructeur :

```
0x0079a103:  MOV dword ptr [EDI+0x128], 0xF      ; slots_per_page = 15
             ; bytes: C7 87 28 01 00 00 | 0F 00 00 00   (imm32 à l'offset +6)
```

À partir de `+0x128`, le ctor (`0x0079a030`) dérive **automatiquement** :

- vecteur slots `+0xe8` et vecteur name-labels `+0xdc` → redimensionnés à `slots_per_page` ;
- `usableSlots = max(nbPersos+1, normal+premium+billing)` ;
- `nb_pages (+0x124) = ceil(usableSlots / slots_per_page)` ;
- `capacité (+0x12c) = slots_per_page * nb_pages` ;
- records `+0x108` (stride `0x15c`), flags `+0x10c` (init 1), shorts `+0x110` → tous à la taille `capacité`.

`BuildPage`, `RenderSlots` et `OnMsg` relisent tous `+0x128` dynamiquement à
l'exécution. **Conséquence : augmenter l'imm32 à `0x0079a103` suffit pour que le
client *alloue et gère* N slots par page.** Aucune autre allocation n'est à toucher.

## Le vrai blocage : la grille 5 colonnes codée en dur

La disposition n'est PAS dérivée de `slots_per_page` — elle est **figée à 5
colonnes** dans plusieurs fonctions :

- **`BuildPage`** — position slot `i` : `x = (i % 5) * 0x9d + 10`,
  `y = (i / 5) * 0xc3 + 0x15`. Pitch **`0x9d=157` × `0xc3=195`**, taille slot
  `UIWindow_SetSize(0xa1, 0xc5)` = **161 × 197**. Name-labels :
  `x=(i%5)*0x9d+0x5a`, `y=(i/5)*0xc3+0x9e`.
- **`RenderSlots`** — texte « Billing Service » : `(i % slots_per_page) * 0xa3`.
- **`OnMsg`** — nav clavier haut/bas = `±5` (cases msg `0x12`/`0x13`), et
  repositionnement des labels : `((col)*0x9d - w/2)+0x58`, `row*0xc3+0xbd`.

Avec `slots_per_page=15` → grille **5×3**. La 3ᵉ ligne (idx 10-14) est à
`y = 2*195+21 = 411`, bas à `411+197 = 608`. Les flèches de page sont à `y=0x22e=558`.

**Si on passe à 20 (5×4)** : la 4ᵉ ligne (idx 15-19) tombe à `y = 3*195+21 = 606`,
bas à `803` → **déborde l'écran (~768) et recouvre les flèches / boutons OK/Créer.**

### Deux options pour vraiment afficher > 15 par page

1. **Plus de colonnes** (p. ex. 5→6/8) : patcher chaque `5` (`%5`, `/5`), le
   x-pitch `0x9d`, et la nav `±5` dans `BuildPage` + `RenderSlots` + `OnMsg`.
   Contrainte : slot large de 161 px pour un pitch de 157 ⇒ pour ajouter des
   colonnes il faut réduire le pitch (slots plus serrés/petits) ou élargir le fond,
   la zone utile faisant ~1024 px de large (5×157 ≈ 785 px + marge).
2. **Cellules plus petites** : réduire pitch `0x9d`/`0xc3` **et** la taille
   `UIWindow_SetSize(0xa1,0xc5)` pour caser plus de lignes/colonnes. Retouche plus
   lourde (les sprites paperdoll sont dessinés à taille fixe par `RenderSlots`).

Le plus propre reste : **augmenter le nombre de colonnes** en réduisant un peu le
pitch, plutôt que d'empiler des lignes qui débordent verticalement.

## Côté données : déjà en place (serveur moonlight = 45 persos)

**Le côté données est déjà fait** — pas de modif serveur nécessaire :

- [`src/custom/defines_pre.hpp:19`](../../moonlight/src/custom/defines_pre.hpp) :
  `#define MAX_CHARS 45` (override le défaut 15 de `mmo.hpp`). Avec
  `MAX_CHAR_VIP=6`, `MAX_CHAR_BILLING=0` → `MIN_CHARS = 45-6-0 = 39`,
  `char_per_account = 39`.
- `HC_ACCEPT_ENTER2` (`chclif_mmo_send0272`) envoie :
  `normal=MIN_CHARS`, `premium=chars_vip`, `billing=chars_billing`,
  `producible=char_slots`, `total=MAX_CHARS`.
  → globals client : `g_CharSelect_NormalSlots 0x015ffd60`,
  `g_CharSelect_PremiumSlots 0x015ffd64`, `g_CharSelect_BillingSlots 0x015ffd68`,
  `g_CharSelect_CreatableSlots 0x015ffd6c`.
- **Validé live (x32dbg, 2026-07-21)** : `[015ffd60]=45`, `[015ffd64]=0`,
  `[015ffd68]=0`, `[015ffd6c]=45` → `usableSlots = 45+0+0 = 45`,
  `pages = ceil(45/15) = 3`. Instruction `0x0079a103` lue = `C7 87 28 01 00 00
  0F 00 00 00` (conforme). Le champ `normal` observé vaut ici 45 (et non 39) :
  l'important est que **la somme normal+premium+billing = 45** pilote les pages.
- `HC_CHARLIST_NOTIFY` envoie `p.total = max(char_slots/3, 1)` = un *nombre de
  pages* (hypothèse serveur : 3 slots/page). **Le client l'ignore pour la grille** :
  il recalcule ses propres pages depuis `slots_per_page`.

**Situation actuelle** : le compte a jusqu'à 45 persos → le client calcule
`ceil(45 / 15) = 3 pages de 15`, paginées. **Le travail restant est donc
100 % côté client (mise en page).** Objectif : condenser ces 45 slots en moins de
pages (idéalement une seule) en augmentant `slots_per_page` (`+0x128`) et en
élargissant la grille.

### Exemple concret pour 45 slots sur une page

45 = **9 colonnes × 5 lignes** (ou 5×9, mais 9 lignes débordent). Pour 9 colonnes
il faut, en plus de l'imm32 `0x0079a103` = `45` :
- remplacer les `% 5`/`/ 5` par `% 9`/`/ 9` et la nav `±5` par `±9` (BuildPage,
  RenderSlots, OnMsg) ;
- réduire le x-pitch `0x9d`(157) : 9 col × ~110 px ≈ 990 px (tient dans ~1024) →
  slots plus étroits (réduire aussi `UIWindow_SetSize(0xa1,0xc5)`) ;
- 5 lignes × pitch `0xc3`(195) = 975 px → **déborde encore** (~768). Il faut donc
  aussi réduire le y-pitch (p. ex. ~140) et la hauteur de slot.

En clair : passer à « tout sur une page » impose de **rétrécir sensiblement les
cellules** (sprites paperdoll dessinés à taille fixe par `RenderSlots` → à revoir).
Un compromis raisonnable : **5×5 = 25 / page (2 pages)** ou **6×4 = 24 / page**,
qui tiennent mieux dans l'art existant qu'un 9×5.

## 25 slots/page : approche patch-natif ABANDONNÉE → refaire en ImGui

L'implémentation par patch des immédiats de layout du client natif a été **retirée**
(2026-07-21). Elle fonctionnait sur le fond (25/page, `ceil(45/25)=2 pages`) mais
butait sur un mur **physique** : la carte de slot est un bitmap **fixe ~197 px** que
`SetSize` **coupe** (ne met pas à l'échelle), et 5 lignes ne tiennent pas dans la
hauteur disponible sans **couper ou chevaucher** les cartes. Trop de constantes
couplées (pas + hauteur + offsets nom `0xbd`/sprite `0x9e`, ×3 sites OnMsg) pour un
rendu jugé insuffisant.

**Décision : refaire en overlay ImGui** (grille dessinée par-dessus, propre
pagination, tailles libres → ni bitmap fixe ni coupe). Le code retiré, les valeurs
testées et **tous les sites de patch** sont archivés dans
[archive/charselect_25_per_page_native_patch.md](archive/charselect_25_per_page_native_patch.md).
La carte des champs et fonctions ci-dessus reste la base pour l'implémentation ImGui
(lire la liste de persos via le dispatcher `g_UICommandDispatcher` cmd 8, les
records `+0x108`, les comptes `g_CharSelect_*`).

## Rappel : crash de pagination (déjà corrigé)

Le rendu du slot sélectionné (`0x0079d590`, idx19) lit `slots[+0x120]` **sans**
garde `index < slots_per_page`. En paginant, `+0x120` vaut `slots_per_page`
(sentinelle « aucune sélection ») → lecture 1 cran après la fin du vecteur `+0xe8`
→ crash intermittent (souvent 3ᵉ page). **Corrigé** par `InstallCharSelectPagingFix`
([ragnarok_client.cc](../src/ragnarok/ragnarok_client.cc)) : détour à `0x0079d5e0`
qui réinsère la garde `+0x120 < +0x128`. Ce correctif lit `+0x128` dynamiquement →
il **reste valide** si on augmente slots_per_page. Voir `project_charselect_paging_crash`.

---

# Protocole char-server (20250716) + `CHARACTER_INFO` — pour le char-select ImGui

Référence d'implémentation du **char-select ImGui** (front « lobby » unifié, cf.
[login_auth_imgui_design.md](login_auth_imgui_design.md)). Source **serveur**
moonlight (`src/char/char_clif.cpp`, `src/common/packets.hpp`) — autorité pour ce
packetver. **Au char-select on est déjà connecté au char-server** → on **envoie
les `CH_*` via `Bourgeon::SendPacket`** et le recv natif
(`LoginCharMode_RecvDispatch` 0x00d27560) traite les `HC_*` (maj liste, entrée
map). Tous les structs sont `#pragma pack(1)` (aucun padding).

⚠ **Piège packetver** : plusieurs ZC passent aux headers « main » `0x0Bxx`
(car `PACKETVER_MAIN_NUM ≥ 20201007`) : `HC_ACCEPT_MAKECHAR` = **0x0B6F**,
`HC_ACK_CHANGE_CHARACTER_SLOT` = **0x0B70**, `HC_ACK_CHARINFO_PER_PAGE` = **0x0B72**.

## Lire les persos côté client (déjà en mémoire)

`g_UICommandDispatcher` (0x0121333c) `vtbl[0x18](8, slot)` → **`CHARACTER_INFO*`**
(le MÊME struct filaire de 175 o, offsets confirmés live via `RenderSlots`
0x0079d170). Itérer slot `0..CURSLOT` pour peupler la grille. Comptes de slots :
`g_CharSelect_NormalSlots` 0x015ffd60 / `Premium` 64 / `Billing` 68 / `Creatable` 6c.

## `CHARACTER_INFO` — struct 175 octets (Rosetta, `char_mmo_char_tobuf`)

| off | taille | champ | notes |
|----:|:--:|-------|-------|
| 0x00 | 4 | GID (char_id) | |
| 0x04 | 8 | base_exp | **int64** (≥20170830) |
| 0x0c | 4 | zeny | |
| 0x10 | 8 | job_exp | **int64** |
| 0x18 | 4 | job_level | |
| 0x1c | 4 | opt1 (bodystate) | 0 |
| 0x20 | 4 | opt2 (healthstate) | 0 |
| 0x24 | 4 | option (effectstate) | |
| 0x28 | 4 | karma | |
| 0x2c | 4 | manner | |
| 0x30 | 2 | status_point | |
| 0x32 | 8 | hp | **int64** (main≥20220330) |
| 0x3a | 8 | max_hp | **int64** |
| 0x42 | 8 | sp | **int64** (cap INT16) |
| 0x4a | 8 | max_sp | **int64** |
| 0x52 | 2 | speed | DEFAULT_WALK_SPEED (constante) |
| **0x54** | 2 | **class_/job** | |
| **0x56** | 2 | **hair** | |
| **0x58** | 2 | **body** | valeur directe (≥20231220) |
| **0x5a** | 2 | **weapon** | 0 si monture |
| **0x5c** | 2 | **base_level** | |
| 0x5e | 2 | skill_point | |
| **0x60** | 2 | **head_bottom** (accessory) | |
| **0x62** | 2 | **shield** | |
| **0x64** | 2 | **head_top** (accessory2) | |
| **0x66** | 2 | **head_mid** (accessory3) | |
| **0x68** | 2 | **hair_color** (headpalette) | |
| **0x6a** | 2 | **clothes_color** (bodypalette) | |
| **0x6c** | 24 | **name** (char[24]) | |
| 0x84 | 1 | Str | |
| 0x85 | 1 | Agi | |
| 0x86 | 1 | Vit | |
| 0x87 | 1 | Int | |
| 0x88 | 1 | Dex | |
| 0x89 | 1 | Luk | |
| **0x8a** | 1 | **CharNum (slot)** | |
| 0x8b | 1 | hairColor (8 bits) | tronqué de hair_color |
| 0x8c | 2 | bIsChangedCharName | (rename>0)?0:1 |
| **0x8e** | 16 | **mapName** (char[16]) | ≥20100803 |
| **0x9e** | 4 | **DelRevDate** | délai restant avant suppr. (0=aucune) |
| **0xa2** | 4 | **robe** (robePalette) | ≥20110111 |
| 0xa6 | 4 | chr_slot_changeCnt | character_moves |
| 0xaa | 4 | chr_name_changeCnt | (rename>0)?1:0 |
| **0xae** | 1 | **sex** | 0=F,1=M,99=sexe du compte |

(offsets en **gras** = ceux qu'on lit pour l'affichage + le paperdoll, confirmés
dans `RenderSlots`.)

## Opcodes CRUD (CZ = client→serv, à SendPacket ; ZC = recv natif)

| Opération | CZ | taille | ZC succès | ZC échec |
|-----------|----|:--:|-----------|----------|
| **Sélection / jeu** | `CH_SELECT_CHAR` **0x0066** `[type:2][slot:1]` | 3 | `HC_NOTIFY_ZONESVR` **0x0AC5** (156 : CID, map[16], ip, port, domain[128]) | `HC_REFUSE_ENTER` 0x006C `[err:1]` |
| **Liste** (auto entrée) | — | — | `HC_ACCEPT_ENTER` **0x006B** (var : hdr 27 + N×175) + `0x082D` (29) + `0x09A0` (6) | — |
| **Liste paginée** | `CH_CHARLIST_REQ` 0x09A1 | 2 | ⚠`HC_ACK_CHARINFO_PER_PAGE` **0x0B72** (var) | — |
| **Créer** | `CH_MAKE_CHAR` **0x0A39** `[type:2][name:24][slot:1][haircolor:2][hairstyle:2][job:4][sex:1]` | 36 | ⚠`HC_ACCEPT_MAKECHAR` **0x0B6F** (177 : type+CHARACTER_INFO) | `HC_REFUSE_MAKECHAR` 0x006E `[err:1]` (0=nom pris,0xFF=refus,1=mineur,3=slot KO) |
| **Suppr. réserve** | `CH_DELETE_CHAR3_RESERVED` 0x0827 `[CID:4]` | 6 | `HC_..._RESERVED` 0x0828 (14 : CID, result:4, date:4) | result≠1 |
| **Suppr. confirme** | `CH_DELETE_CHAR3` 0x0829 `[CID:4][birthdate:6 "YYMMDD"]` | 12 | `HC_DELETE_CHAR3` 0x082A (10 : CID, result:4) | result≠1 (5=birthdate KO,4=délai) |
| **Suppr. annule** | `CH_DELETE_CHAR3_CANCEL` 0x082B `[CID:4]` | 6 | `HC_..._CANCEL` 0x082C (10) | |
| **Rename check** | `CH_REQ_IS_VALID_CHARNAME` 0x028D `[AID:4][CID:4][name:24]` | 34 | `HC_ACK_IS_VALID_CHARNAME` 0x028E `[result:2]` (1=OK) | result=0 |
| **Rename confirme** | `CH_REQ_CHANGE_CHARNAME` **0x08FC** `[CID:4][name:24]` | 30 | `HC_ACK_CHANGE_CHARNAME` **0x08FD** `[result:4]` (0=OK) | result≠0 |
| **Déplacer slot** | `CH_REQ_CHANGE_CHARACTER_SLOT` 0x08D4 `[from:2][to:2][rem:2]` | 8 | ⚠`HC_ACK_CHANGE_CHARACTER_SLOT` **0x0B70** (8 : hdr, reason:2, moves:2) | reason=1 |

Notes :
- **Suppression** = flux 3 étapes (réserve → attente `char_del_delay`, **défaut 24 h** !
  → délai renvoyé dans 0x0828.date ; confirme par **date de naissance** "YYMMDD").
- Après suppr./rename/move réussi, le serveur **renvoie la liste complète** → le
  recv natif la reçoit ; on **re-lit** via dispatcher cmd 8 pour rafraîchir la grille.
- `CH_SELECT_CHAR` 0x0066 : le recv natif gère `HC_NOTIFY_ZONESVR` (connexion
  map-server) → **entrée en jeu seamless** sans qu'on touche à autre chose.
- Variantes legacy à ignorer (0x0067 make, 0x01FB delete, 0x028F/0x0290 rename,
  0x0071 zonesvr) : non émises à ce packetver.

# Rendu du paperdoll dans le char-select ImGui

Implémenté par `BasicInfoTweaks::RenderDoll` (basic_info.cc), appelé par
`CharSelect::DrawDoll`. Même séquence que le natif `RenderSlots` 0x0079d170 :
`Actor_Init` 0x007ac210 → `Actor_DrawSprites` 0x007ac820 (param 1 = chemin quad) →
`Actor_Dtor` 0x0079a6a0, avec capture de chaque couche via le hook **unique** sur
`Actor_SubmitSpriteQuad` 0x00a1b7c0 (partagé avec le portrait / l'aperçu d'item /
l'avatar de la fiche perso).

## Le contexte de rendu peut être FACTICE (clé du hors-jeu)

Les captures existantes prennent la fenêtre `UIBasicInfoWnd` (0x0131f6c4) comme
`renderCtx` — inexistante au char-select. Or (RE 2026-07-23, commentaires posés dans
Ghidra sur les deux fonctions) :

- `Actor_Init` **stocke** seulement `p1` dans `actor+0x04`, sans jamais le déréférencer ;
- sur le chemin **quad**, `actor+0x04` ne sert qu'à (a) l'appel virtuel `vtbl+0xa0` =
  `UIWindow_GetFadeColor` 0x00a1edf0 — couleur ignorée par la capture — et (b) le `this`
  d'`Actor_SubmitSpriteQuad`, que le hook supprime pendant la capture ;
- `UIWindow_GetFadeColor` ne lit que `+0x38` (alpha cible), `+0x3c` (tick) et `+0x40`
  (alpha courant, réécrit).

D'où un objet de **0x100 octets**, vtable de 48 entrées dont seule l'entrée **40**
(= +0xa0) pointe sur 0x00a1edf0, `+0x38 = +0x40 = 0xff`, `+0x3c = GetTickCount()`.
⚠ La branche **non-quad** d'`Actor_DrawSprites` déréférence `ctx+0x24` : ne jamais
l'emprunter avec un faux contexte (toujours `param = 1`).

## Arguments d'`Actor_Init` (depuis `CHARACTER_INFO`)

| param | champ | note |
|---|---|---|
| p4 sexe | `+0xae` (99 → `g_Account_Sex` **0x015FB23C**) | cast `char` |
| p5/p6 job | `+0x54` (u16 puis i16) | |
| p7 job_body | `+0x58` **body style** | `Job_ResolveBodyClass` 0x00d99150 le prend pour un **ID de classe** (rAthena : `status.body = status.class_`) ; **0 ⇒ corps Novice** → Bourgeon replie sur le job |
| p8 hair | `+0x56` | |
| **p9** | **head LOW** `+0x60` | ⚠ pas « top » : les noms `hg_*` des autres captures sont des noms de *couche* |
| **p10** | **head TOP** `+0x64` | |
| **p11** | **head MID** `+0x66` | |
| p12 garment | `+0xa2` | |
| p15/p16 | clothes `+0x6a` / hair color `+0x68` | |
| p17/p18 | pose (`animType*8+dir`) / image | natif : `rec[0x53]/[0x54]`, `0x10` si suppression programmée (perso assis) |
| p19 | tick de base (`Act_ResolveAltAnimFrame`) | Bourgeon passe **0** → capture déterministe (indispensable pour la cacher) |

Ni arme (`+0x5a`) ni bouclier (`+0x62`) : cet acteur n'a pas ces couches
(`Actor_BuildSpriteLayers` 0x007ae4e0 : 0/7 garment, 1 corps, 2 tête+cheveux, 3-6 coiffes).

## Cache (45 → 60 slots)

Clé = signature FNV-1a de l'apparence + direction (deux persos identiques partagent
l'entrée) ; 64 entrées, éviction LRU. Une capture = un aller-retour natif complet, donc :
**budget de 2 captures/frame** (la grille se remplit en quelques frames, sans à-coup) et
**aucune capture pour une carte hors-vue** (`ImGui::IsRectVisible`).
Les entrées sont **ré-capturées toutes les 500 ms** : une couche mémorise une page
d'atlas + des UV, et l'atlas est un cache LRU `(cellule,palette)` — ré-appeler
`SpriteAtlas_GetCachedTexture` remet la cellule en tête de LRU et rafraîchit le handle
de page (même raison que le « ré-résolu chaque frame » de `login_parade`).
Chaque entrée retient l'`Overlay_DeviceEpoch()` de sa capture et n'est **jamais dessinée**
après un reset de device (textures détruites → crash ddraw sinon).

---

## CRUD natif (contrôles de `UINewSelectCharWnd::OnMsg` 0x0079d610)

Tout le CRUD passe par le **pilotage des contrôles natifs** (msg 6 / `ctrl`), jamais
par des paquets fabriqués à la main — le natif construit chaque paquet. Pré-requis :
poser `g_CharSelect_SelectedSlot` (0x015F8262) au slot visé, puis appeler
`OnMsg(wnd, 0, 6, ctrl, 0, 0, 0)` (RET 0x18 → typedef à **6** args pile).

| ctrl | effet | paquet émis par le natif |
|------|-------|--------------------------|
| 0xB8 | entrer en jeu (Own_SetCharName/Sex + state 9) | CH_SELECT_CHAR 0x0066 |
| 0x1A0 | créer : ouvre `UIMakeCharWnd` (MakeWindow **0xC8**) | CH_MAKE_CHAR 0x0A39 (par 0xC8) |
| 0x197 | **programmer** la suppression | CZ **0x0827** [op:2][GID:4] |
| 0x198 | **annuler** la suppression | CZ **0x082B** [op:2][GID:4] |
| 0xD3 | supprimer maintenant (confirm + code natif) | cmd mode 0x271A |
| 0xB9 (185) | « Cancel » : quitter l'écran (msgbox de confirmation, cf. ci-dessous) | — |

### Quitter l'écran : retour au login vs. quitter le jeu

Le **« Cancel » natif** (`ctrl 185`) confirme par une msgbox, puis branche sur le flag
client `g_CanReturnToLoginScreen` `0x01602328` (posé à 1 en `0x00d24a45` ; même
dichotomie dans le handler de refus de connexion `0x00d29630`) :

| Branche | Commande de mode | Effet |
|---|---|---|
| flag = 1 | `SendMsg(mode, 10011)` = `0x271B` (`CLoginMode_SendMsg 0x00d2a130`) | `CRagConnection_OnDisconnect` + `mode+0xc = 3` ⇒ **retour à l'écran de connexion** |
| sinon | `SendMsg(mode, 2)` → `CMode::SendMsg` de base **`0x00a763c0`** | arrêt des sous-systèmes + `mode+0x14 = 0` ⇒ la boucle principale sort = **quitter le jeu** |

(`SendMsg(mode, 2)` est exactement ce que fait le bouton « Exit » de l'écran de login,
`UILoginWnd_OnMsg 0x008848d0` `ctrl 221`.)

Le plugin expose **les deux** dans une barre bas-droite (« Revenir au login », «
Quitter le jeu »), chacune derrière sa confirmation ImGui, et envoie la commande de
mode **lui-même** (`CharSelect::DriveModeCmd`, dispatcher `*(0x0121333c)` vtbl+0x18).
⚠ Piloter `ctrl 185` ne marcherait PAS : sa msgbox passe par
`UIWndMgr_ShowMessageBoxModal 0x00a31a30`, que le plugin **détourne** sous sa
couverture (retour 185 ≠ 187 attendu) ⇒ le natif conclurait « annulé ».

⚠ **Le client reste en `CLoginMode`** : seul l'ÉTAT change (9/6 → 3), donc **aucun
`OnModeSwitch` n'est émis**. Deux conséquences, traitées explicitement :

1. `MoonlightAuth` resterait bloqué en `kDriveLogin` (session authentifiée, drive
   « terminé ») et ne redessinerait pas son formulaire ⇒ on retombait sur l'écran de
   login **NATIF**. Le bouton appelle donc `MoonlightAuth::RearmWebLogin()` (même
   remise à zéro que la branche « vraie (re)connexion » de `OnModeSwitch`, plus le
   re-passage du service-select et le pré-remplissage du mot de passe DPAPI).
2. Le plugin ne peut pas se fier aux `CHARACTER_INFO` (encore lisibles après la
   déconnexion) pour savoir qu'il doit se retirer : il latche `left_` et se réarme sur
   la **fenêtre native `0x115`** (`UIWindowMgr_FindWindow 0x00a47b90` — fiable,
   contrairement au cache `mgr+0x3d4` jamais remis à zéro). Absente = écran de
   connexion (on ne dessine rien) ; présente = nouveau char-select, on reprend.

Pour la fermeture (`cmd 2`), la table passe derrière un fondu au noir
« Fermeture du jeu… » le temps que la boucle principale sorte.

`CHARACTER_INFO+0x9E` (DelRevDate) > 0 ⇒ suppression programmée en cours (délai
restant, secondes) — bloque l'entrée en jeu. Réservation/annulation (0x197/0x198)
restent **100 % en ImGui** (pas de dialogue natif) ; création et suppression
définitive ouvrent un dialogue natif ⇒ le plugin se **découvre** le temps de l'op et
se re-couvre quand la liste change. Opcodes (opcode_map) : CH_MAKE_CHAR 0x0067(37)/
0x0970(31)/**0x0A39(36, actif)** ; delete3 réservé 0x0827, effacer 0x0829, annuler
0x082B ; 0x08D4 = CH_REQ_CHANGE_CHARACTER_SLOT (déplacer un perso de place).

## Reskin « scène banquet » (plugin CharSelect, incrément 3)

Décor plein écran = **BMP client** chargé par le loader natif (UITextureMgr_Get
0x00a90350 → MakeKey 0x00a9f030 → LoadTex 0x00a8d4a0 ; largeur/hauteur/pixels BGRA à
+0x114/+0x118/+0x11c), converti ARGB → `Overlay_CreateTextureARGB` (cache invalidé
par `Overlay_DeviceEpoch`). **Fichier à déployer dans le GRF/data :**
`lobby_hall.bmp` (constante `kHallBmpPath`, préfixe CP949).
Absent ⇒ repli dégradé sombre.

Les personnages sont posés aux **places** d'une table (table `g_seats`, 25 entrées,
coords **normalisées** [0..1] sur le fond). Ordre = numérotation de l'image :
slot i → place n°(i+1) (slot 0 = grand trône). Chaque place = pieds (nx,ny) + échelle
(hauteur du pantin / hauteur écran, perspective). Rendu du pantin par
`BasicInfoTweaks::RenderDoll` (coords écran). Coords estimées à l'œil ⇒ **éditeur de
layout** (glisser = position, molette = taille, « Dump layout » journalise la table +
les points `Anchor`, prêts à recoller dans `g_seats`).

⚠ **Éditeur DÉSACTIVÉ** : le layout est calé, le déclencheur (F10 / `IsStaff()`) est
**commenté** en bas de `CharSelect::OnRenderLoginUI` — `seat_edit_` ne peut plus
passer à `true`, tout le chemin d'édition est du code mort conservé pour un futur
recalage (décommenter le bloc suffit).
