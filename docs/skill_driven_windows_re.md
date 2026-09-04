# Les fenêtres ouvertes par un SKILL — l'angle mort des deux relevés précédents

> Relevé du **2026-09-03**. Client `2025-07-16_Ragexe` (IDB `E:\Nouveau
> dossier\2025-07-16_Ragexe_175220998_clientinfo.exe.i64`), serveur `moonlight`
> compilé **`PACKETVER 20250716`** (`src/custom/defines_pre.hpp`) — donc
> `PACKETVER_MAIN`, jamais `PACKETVER_RE` (les deux plages de
> `src/config/packets.hpp` s'arrêtent à 20211118).
>
> Complète [unexplored_systems.md](unexplored_systems.md), qui concluait « il ne
> reste AUCUNE candidate vivante et vierge ». Cette conclusion était **fausse**,
> et pour une raison de méthode, pas d'inattention.

## 1. 🔴🔴 Pourquoi les deux vecteurs précédents ne pouvaient PAS voir ces fenêtres

`unexplored_systems.md` trie avec deux vecteurs :

1. **le paquet + `feature.*`** (critère **P** / **C**) — il part des *fenêtres* du
   client et demande si le serveur sert la famille ;
2. **les commandes de script** (`openstorage`, `opendressroom`, …) comptées dans
   `moon/` — le vecteur déclaré « le plus rentable » du chantier.

Les deux partagent le même angle mort : **ils ne voient que ce qu'une
CONFIGURATION ou un SCRIPT déclenche.** Or une partie du client s'ouvre parce
que le joueur a **lancé une compétence**. Aucun `feature.*` ne la garde, aucun
NPC ne l'appelle, aucune icône de menu ne la porte — elle n'apparaît donc dans
aucun des deux relevés, alors qu'elle est jouable **aujourd'hui**, par n'importe
quel joueur, sans qu'on touche à quoi que ce soit.

➡ **Le troisième vecteur, et il est exhaustif :** partir du *dispatcher de
paquets* et demander, pour chaque opcode, **s'il finit par appeler
`UIWindowMgr_MakeWindow`, et avec quel id**. C'est mesurable d'un bout à l'autre
et ça ne suppose rien.

## 2. La mesure

Dans l'IDB, sur `RecvLoop_DispatchPackets` (`0xc9df00`) :

1. `idaapi.calc_switch_cases` sur les tables de saut → **3048 cases**
   (⚠ `si.lowcase` est inexploitable ici, cf. [[feedback_re_method]]) ;
   la valeur de case **EST** l'opcode (témoin : fusion d'objets = `0x96d`) ;
2. **294 fonctions** appellent `UIWindowMgr_MakeWindow` (`0xa39340`) ; pour
   chacune on remonte au `push <imm>` qui précède l'appel → **471 couples
   (fonction, id de fenêtre)** ;
3. jointure opcode → handler du case → id, à **profondeur 1** (handler, puis ses
   appelés directs) : **290 opcodes ouvrent une fenêtre native** ;
4. croisement avec la colonne « statut » de
   [native_window_dispatch.md](native_window_dispatch.md) (258 fenêtres, dont
   **136 « jamais citee »**) : **63 opcodes** mènent à une fenêtre jamais
   outillée.

Table brute : `zc_opens_window.json` (reproductible, cf. §6).

### 🔴 24 des 63 lignes sont un FAUX POSITIF SYSTÉMATIQUE

Les couples `[15, 276]` (`UIItemDropCntWnd`, `UIRoundBoxTooltipWnd`) reviennent
sur 24 opcodes sans le moindre rapport entre eux (`0x00A5`, `0x0177`, `0x0253`,
`0x0904`…). Cause : la profondeur 1 traverse un **helper générique** (info-bulle
/ action de chat) que beaucoup de handlers appellent. Ce n'est pas le handler
qui ouvre la fenêtre.

➡ **Reste 39 lignes réelles.** Conformément à
[[feedback_re_method]] (« avant d'annoncer un volume, ouvrir cinq candidats »),
**neuf** ont été ouvertes une par une dans l'IDB et confrontées au serveur ;
c'est ce que donne le §3. Les autres ne sont pas des verdicts.

## 3. ✅ Six fenêtres VIVANTES, jouables aujourd'hui, jamais outillées

Toutes vérifiées des deux côtés : le handler client a été lu (format du paquet,
id de `MakeWindow`), et l'émetteur serveur a été remonté jusqu'à un appelant
vivant. Tous sont marqués **« jamais citee »** dans
[native_window_dispatch.md](native_window_dispatch.md).

| fenêtre | ZC | handler client (renommé dans l'IDB) | émetteur serveur | déclenché par |
|---|---|---|---|---|
| **39 `UIChooseWarpWnd`** | `0x0ABE` | `Recv_ZC_WARPLIST_0x0ABE` `0xd1b570` | `clif_skill_warppoint` (clif.cpp:11596) | **AL_TELEPORT, AL_WARP, Odin's Recall** |
| **99 `UISpellListWnd`** | `0x0AFB` | `Recv_ZC_AUTOSPELLLIST_0x0AFB` `0xcff0f0` | `clif_autospell` (clif.cpp:13824) | **SA_AUTOSPELL** (Hindsight, Sage) |
| **107 `UIItemRepairWnd`** | `0x0B65` | `Recv_ZC_REPAIRITEMLIST_0x0B65` `0xd0b420` | `clif_item_repair_list` (clif.cpp:12592) | **BS_REPAIRWEAPON** (forgeron) |
| **95 `UISelectCartWnd`** | `0x097F` | `Recv_ZC_SELECTCART_0x097F` `0xcf96a0` | `clif_SelectCart` (clif.cpp:18348) | **MC_CARTDECORATE** |
| **21 `UINotifyLevelUpWnd`** + **49 `UINotifyJobLevelUpWnd`** | `0x00B0` | `sub_CCEFF0` (`0xccf4e1`, `0xccf648`) | `HEADER_ZC_PAR_CHANGE` (clif.cpp:3723) | **montée de niveau** (base / métier) |
| **143 `UINotifyQuestWnd`** | `0x0B0C` | `sub_CB6280` (`0xcb64de`) | `questAddType` (packets_struct.hpp:290) | **ajout d'une quête** |

⚠ Les deux dernières lignes sont mesurées mais **moins creusées** que les quatre
premières : l'émission serveur est prouvée, le contenu de la fenêtre ne l'est
pas. Les quatre premières le sont **au champ près** (§4).

### Détails qui coûteront du temps si on les redécouvre

- **39** : le serveur n'envoie **jamais** `"cancel"` — c'est le **client** qui
  l'ajoute en queue de liste (`Recv_ZC_WARPLIST_0x0ABE`, et de nouveau dans
  `UIChooseWarpWnd_OnMsg` case 123). Le retour joueur part par
  `CGameMode` commande **74** → `CZ_SELECT_WARPPOINT` `0x011B`
  (`clif_parse_UseSkillMap`), contrôle **184** = OK, **185** = annuler.
  Affichage : `"Random"` → msgstring **0xD4**, `"cancel"` → msgstring **0xDA**,
  sinon `Social_GetMapDisplayName`.
- **99** : 🔴 **si aucun skill id n'est > 0, la fenêtre n'est PAS créée** — le
  compteur garde le `MakeWindow`. « Rien ne s'ouvre » n'est donc pas une panne.
- **107** : 🔴 **liste vide ⇒ pas de fenêtre, mais une boîte modale** portant le
  msgstring **0x1A8 (424)**.
- **95** : 🔴 garde `if (len != 8)` — un paquet sans aucun style n'ouvre **rien,
  en silence**. Le serveur envoie toujours 3 styles (10, 11, 12).
- **21 / 49** : les deux `MakeWindow` sont gardés par `g_ReplayActive == 0`.
- **143** : le `MakeWindow` suit immédiatement `QuestMgr_InvalidateTracker`.
  ⚠ Ne pas confondre avec le **suivi** (`0xcc`) ni le **journal** (`0x141`) de
  [[project_quest_tracking_re]] : c'est une **troisième** fenêtre.

## 4. Les quatre protocoles, vérifiés au champ près

Aucun défaut trouvé — les quatre sont **sains des deux côtés**.

| ZC | format client (lu dans l'IDB) | structure rAthena | accord |
|---|---|---|---|
| `0x0ABE` | `hdr.W len.W skillId.W { map.16B }*`, `(len-6)/16` | `PACKET_ZC_WARPLIST`, `MAP_NAME_LENGTH_EXT` = **16**, `packetLength` initialisé à **6** | ✅ |
| `0x0AFB` | `hdr.W len.W { skillId.L }*` | `PACKET_ZC_AUTOSPELLLIST` (`int skills[]`) | ✅ |
| `0x0B65` | `hdr.W len.W { entrée.24B }*`, `(len-4)/0x18` | `REPAIRITEM_INFO2` = `index.W` + `itemId.L` + `EQUIPSLOTINFO` (4 × `uint32` = 16) + `refine.B` + `grade.B` = **24** | ✅ |
| `0x097F` | `hdr.W len.W accountId.L { type.B }*` | `WFIFOW(0)=0x97f; WFIFOW(2)=8+carts; WFIFOL(4)=account_id` | ✅ |

🔴 **Le choix d'en-tête dépend de `PACKETVER_MAIN_NUM`, pas de `_RE`** — et il
tombe juste parce que le serveur compile exactement la version du client
(20250716) : `ZC_WARPLIST` → `0xabe` (≥ 20170502), `ZC_AUTOSPELLLIST` → `0x0afb`
(≥ 20181128), `ZC_REPAIRITEMLIST` → `0x0b65` (≥ 20200916). Sur une autre
`PACKETVER` ce sont `0x11c`, `0x01cd` et `0x01fc`, que le client sert aussi mais
par d'autres `case`.

⚠ `MC_CARTDECORATE` est bien dans **`db/pre-re/skill_tree.yml`** (requiert
`MC_VENDING` 1) — Moonlight étant pré-renewal ([[project_moonlight_is_prerenewal]]),
la question 4 du tri (« la base est-elle peuplée ? ») est tranchée positivement.

## 5. ⛔ Ce que la mesure a TUÉ

### 308 `UIShowWarpWnd` — le « non tranché » de `unexplored_systems.md`

Le relevé précédent laissait la question ouverte et supposait l'**airship privé**
(`feature.privateairship: off`), en notant que « le nom suggère plutôt une liste
de warps ». La deuxième moitié était juste, la première fausse — et surtout la
conclusion pratique était inversée :

- **308 est nourrie par ZC `0x0ABF`** (`Recv_ZC_WARPLIST_NOSKILL_0x0ABF`
  `0xd0bff0`, seul ouvreur de la fenêtre), format `hdr.W len.W { map.16B }*`,
  `(len-4)/16`, **sans skill id** ;
- **aucune fonction de `src/map/` n'émet `0x0ABF`.** La fenêtre est **morte** ;
- la liste de warps réellement jouée est la **39**, un `case` plus bas.

Les deux `case` sont voisins (`0xca986d` et `0xca987e`) et les deux formats ne
diffèrent que de **2 octets** — c'est exactement le genre de paire qui se
confond. `UIShowWarpWnd_OnMsg` `0x9170a0` et `UIChooseWarpWnd_OnMsg` `0x8be990`
partagent d'ailleurs le même code de résolution de nom de carte.

### 315 / 328 / 343 — `0x0AE2` n'est pas émis

`sub_CF98B0` est un **sous-aiguilleur par type** (table de saut) : case 5 →
**315** `UITipboxWnd`, case 7 → **328** `UICheckAttendanceWnd`, case 8 → **343**
`UIGradeEnchantWnd`. L'opcode `0x0AE2` n'apparaît **ni** dans
`clif_packetdb.hpp` **ni** comme littéral dans `clif.cpp` : rien ne l'ouvre.
(Cohérent avec `feature.attendance: off`.)

### `0x07E3` → 160 / 161 : émis, mais déjà couvert

`clif_skill_itemlistwindow` (clif.cpp:25705, `ZC_ITEMLISTWIN_OPEN`) l'envoie pour
`GN_CHANGEMATERIAL` & co. Les ids `160 UIItemShopWnd2` / `161 UIItemSellWnd2`
sont marqués « jamais citee », mais le **sujet** l'est :
[make_item_list_re.md](make_item_list_re.md) et
[[project_changematerial_and_uiwindow_composite_re]]. ⚠ Le rattachement de ces
deux ids précis à ce sujet **n'a pas été vérifié** — c'est un candidat, pas un
verdict.

## 6. Refaire la mesure

Rien à installer : tout tient dans l'IDB via le MCP `ida-pro-mcp`.
🔴 Un seul intervenant sur IDA à la fois, et **jamais `decompile` sur une adresse
de `case`** ([[feedback_re_method]] §10).

1. cases du dispatcher : `idaapi.calc_switch_cases` sur chaque `get_switch_info`
   de `0xc9df00` — la valeur de case est l'opcode ;
2. `idautils.XrefsTo(0xa39340)` → pour chaque `call`, remonter ≤ 8 instructions
   jusqu'au `push <imm>` (s'arrêter sur un autre `call`) ;
3. joindre à profondeur 1, puis **jeter les lignes dont les seuls ids sont
   15 et 276** ;
4. croiser avec la colonne « statut » de `native_window_dispatch.md` ;
5. pour chaque survivant, remonter l'émetteur serveur — et **valider le motif de
   `grep` sur un témoin positif** : `packets.hpp` écrit `0xabe`, pas `0x0abe`,
   et `grep DEFINE_PACKET_HEADER(..., 0x0abe)` rend **vide** sur un fichier qui
   le contient ([[feedback_absence_needs_measurement]]).

➡ **À refaire après tout changement de `PACKETVER`** : c'est lui, et non un
`feature.*`, qui décide de quel `case` du client sera atteint.

---

🔴 Ce vecteur non plus n'est pas suffisant : il part encore d'un **paquet**. Une
fenêtre ouverte par une **touche** lui échappe entièrement — c'est le quatrième
vecteur, [local_openers_re.md](local_openers_re.md), qui a trouvé les clans.

Voir aussi : [unexplored_systems.md](unexplored_systems.md),
[native_window_dispatch.md](native_window_dispatch.md),
[opcode_map.md](opcode_map.md).
