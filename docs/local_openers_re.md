# Les ouvreurs LOCAUX — le quatrième vecteur de relevé

Relevé du **2026-09-04** sur `2025-07-16_Ragexe_175220998_clientinfo.exe`.

Ce document complète les trois précédents :

| vecteur | ce qu'il voit | document |
|---|---|---|
| 1. paquet enregistré + `feature.*` | ce que la **config** allume | [unexplored_systems.md](unexplored_systems.md) |
| 2. commandes de script dans `moon/` | ce qu'un **NPC** déclenche | idem, §« LE BON VECTEUR » |
| 3. opcode → `MakeWindow` | ce qu'un **paquet** ouvre — dont les skills | [skill_driven_windows_re.md](skill_driven_windows_re.md) |
| **4. ouvreur local** | ce que le **joueur** ouvre sans qu'aucun paquet ne circule | **ce document** |

Les trois premiers partagent le même angle mort : ils partent tous de quelque
chose qui **arrive** au client. Une fenêtre ouverte par une **touche** ou par une
**icône de la barre de menu** n'a ni `feature.*`, ni NPC, ni opcode. Elle est
invisible aux trois, et jouable quand même — c'est ainsi que l'ouvreur de la file
de battleground (icône `battle`, injectée par WARP) avait dû être trouvé à la
main.

> ⚠ **Ce document a été corrigé deux fois le jour même de sa rédaction**, et les
> deux erreurs venaient du même réflexe : conclure d'une mesure faite sur le
> **vanilla** et sur **une seule** primitive. Les corrections sont laissées
> visibles aux §1, §5 et §6 — elles valent plus que le résultat.

---

## 1. 🔴🔴 Il y a TROIS primitives d'ouverture, pas une

Corrigé le 2026-09-04, après coup. Partir des seuls appels à `MakeWindow` est
**incomplet** : deux fonctions l'appellent avec l'identifiant **en argument**,
si bien que leurs propres appelants — les vrais ouvreurs — restent invisibles.

| primitive | adresse | sites | ce qu'elle ajoute |
|---|---|---:|---|
| `UIWindowMgr_MakeWindow` | `0x00A39340` | 546 | la base |
| `UIWindowMgr_ToggleWindow` | `0x00A4BF30` | 48 | **0 identifiant nouveau** (mesuré) |
| `UIWindowMgr_ToggleWindowById` | `0x00812E60` | 17 | **2** : **303** et **10002** |

`ToggleWindowById` ferme (`SaveRectAndCloseWindow`, qui **détruit**) ou crée.
Ses 17 sites sont **tous** dans `UIMenuIconWnd_OnMsg` : c'est **la barre
d'icônes**, seconde racine locale à côté du clavier — voir §4 bis.

## 2. La mesure

Sur les `XrefsTo(UIWindowMgr_MakeWindow 0x00A39340)`, en remontant ≤ 8
instructions jusqu'au `push <imm>` :

- **546 sites d'appel**, dont **526** avec l'identifiant en littéral (les 20
  autres le tiennent dans un registre : table ou boucle, invisibles ici) ;
- **294 fonctions ouvreuses** ;
- **113** sont atteintes depuis `RecvLoop_DispatchPackets` `0x00C9DF00`
  (parcours avant, appels directs, profondeur ≤ 3) — c'est le vecteur 3 ;
- **181 ne le sont pas** ⇒ ouvreurs locaux ;
- **214 identifiants de fenêtre** au total, **88 exclusivement locaux**, dont
  **44** marqués « jamais citee » par
  [native_window_dispatch.md](native_window_dispatch.md) §15.

⚠ Ces chiffres sont ceux de `MakeWindow` **seul** ; les deux autres primitives du
§1 portent le total à **216 identifiants** (+303, +10002).

🔴 Sur ces 44, l'immense majorité sont des **enfants** : une pop-up de
confirmation ouverte par sa fenêtre mère (`UIGuildNoticeWnd` par
`UIGuildWnd_OnMsg_Base`, `UIJobListWnd` par `UISeekPartyWnd_OnMsg`…). Le tri
utile est plus étroit : **quelles racines ne dépendent d'aucune fenêtre déjà
ouverte ?**

Il y en a **deux**, et il fallait les deux :

1. **le clavier** — `UIWindowMgr_DispatchHotkeyBehavior` `0x00A451E0`, appelé par
   `UIWindowMgr_OnKeyDown` `0x00A471E0` (§3 et §4) ;
2. **la barre d'icônes** — `UIMenuIconWnd_OnMsg` `0x00814B00`, qui n'apparaît
   qu'en scannant `ToggleWindowById` (§4 bis).

Il en existe une **troisième**, plus étroite : la **commande de chat**
(`Chat_HandleChatMessage` `0x00C7A460`), qui ouvre `{116, 203, 315, 324}`.
Trois sont déjà atteintes par le clavier ou par une icône ; une seule ne l'est
pas — **116 `UIAutoMessageWnd`**, « jamais citée », dont c'est l'unique ouvreur.

---

## 3. 🔴🔴 La touche ne décide pas — le comportement, oui

`UIWindowMgr_OnKeyDown` ne connaît **aucune** fenêtre. Il compose le combo
(`sub_A2D450`) puis appelle `DispatchHotkeyBehavior`, qui commence par
`UserHotkey_ResolveBehavior` `0x00A32C10` : celui-ci appelle la **fonction Lua
globale `GetBehaviorOfHotKey2`** et récupère un **identifiant de comportement**.
Tout le reste de la fonction est un `switch` sur ce numéro — **59 cas**.

Conséquence de méthode : **la liaison touche → comportement n'est pas dans
l'EXE**. Elle est dans `data\luafiles514\lua files\hotkey.lub`, chargé par
l'entrée `Lua Files\HotKey` (`0x01094A80`). Sur Moonlight ce fichier vient de
**`moonlight.grf`**.

- `HOTKEY_1` (36) = les slots de barre de skills (comportements 0..99, le
  dispatcher les traite par `v4 % 9` / `v4 / 9`) ;
- **`HOTKEY_2` (62) = les comportements 100..161** — l'index dans la table vaut
  `comportement − 100` ;
- `HOTKEY_3` (20) = macros et flags, comportements 200..219 ;
- `HOTKEY_4` (36) = seconde barre de skills.

⚠ **`hotkey_v2.lub` existe dans le même GRF et ne sert à rien** : il ne définit
pas `GetBehaviorOfHotKey2` (seulement `GetBehaviorOfHotKey`), sa `HOTKEY_2`
s'arrête avant l'index 44. C'est un vestige ; l'EXE ne charge que `HotKey`.

### ⛔ La table Lua `BEHAVIOR_TO_WINDOWID` MENT

Elle donne, pour 34 fenêtres, un index de comportement. Deux de ses valeurs
sont **fausses** vis-à-vis de l'EXE livré :

| ce que dit la Lua | ce que fait l'EXE | témoin |
|---|---|---|
| comportement 144 ↔ fenêtre **257** | `case 144:` → `MakeWindow(0xED)` = **237** | `UIClanInfoManageWnd_OnCreate` appelle `UIWindow_AddCloseButton(this, "Ctrl+G", 0)` — la classe 237 porte le libellé du raccourci **en dur** |
| comportement 146 ↔ fenêtre **251** | `case 146:` → banque **275** (via CZ `0x09AB`) | [bank_zeny_re.md](bank_zeny_re.md) |

➡ **Ne lire la Lua que pour la TOUCHE.** L'action se lit dans l'EXE. La table
`BEHAVIOR_TO_WINDOWID` n'est utilisée que par `IsBeHaviorOfWindow` /
`GetBeHaviorOfWindow`, côté interface de réglage des raccourcis.

---

## 4. Les 20 comportements qui ouvrent une fenêtre

Touche = valeur par défaut de `hotkey.lub` (le joueur peut la changer :
`ChangeUserHotKey`, sauvegarde `SaveUserHotKeys2`). Action = lue dans l'EXE.

| comp. | libellé `MSI_HK_…` | touche | fenêtre | statut du dépôt |
|---:|---|---|---:|---|
| 104 / 105 | `FRIENDWND_ONOFF` / `PARTYWND_ONOFF` | Alt+H / Alt+Z | **69** (aiguilleur) | outillé |
| 108 | `MAPWND_ONOFF` | Ctrl+` | 140 `UIRoMapWnd` | outillé |
| 110 | `GUILDWND_ONOFF` | Alt+G | 59 + 212 | outillé |
| 111 | `EMOTIONWND_ONOFF` | Alt+L | 87 `UICashEmotionListWnd` | outillé |
| 113 | `MINIMAP_ONOFF` | Ctrl+Tab | 14 `UIMinimapZoomWnd` | outillé |
| 114 | `MACROWND_ONOFF` | Alt+M | 86 `UIEmotionWnd` | outillé (« Shortcut List ») |
| 122 | `MEMORIALWND_ONOFF` | Alt+B | 137 `UIMemorialDunWnd` | ✅ relevé |
| 123 | `PETWND_ONOFF` | Alt+J | 88 `UIPetInfoWnd` | outillé |
| 124 | `HOMUNWND_ONOFF` | Alt+R | 113 `UIHomunInfoWnd` | outillé |
| **125** | `MERWND_ONOFF` | **Ctrl+R** | **125 `UIMerInfoWnd`** | **jamais citée** |
| 128 | `FRIEND_OPTIONWND` | — | 68 | outillé |
| 129 | `PARTY_OPTIONWND` | — | 53 | outillé |
| 142 | `NAVIGATION_ONOFF` | — | 203 | outillé |
| **144** | `CLANWND_ONOFF` | **Ctrl+G** | **237 `UIClanInfoManageWnd`** | **jamais citée** |
| 149 | `ACHIEVEMENT` | — | 270 | outillé |
| 150 | `SINGLEMAP_ONOFF` | — | 273 `UIMiniMapWnd` | outillé |
| **154** | `TIPBOXWND_ONOFF` | **Alt+D** | **315 `UITipboxWnd`** | **jamais citée** |
| 157 | `PARTYBOARDWND_ONOFF` | Ctrl+Z | 324 `UIAdvenPartyBoardWnd` | ⛔ [party_search_re.md](party_search_re.md) |
| 160 | `REPUTE` | — | 346 `UIReputeWnd` | décrit en mémoire |
| 161 | `EQUIPMENT_PROPERTIES` | Ctrl+Q | 10001 | ✅ relevé |

Deux détails de lecture du `switch` :

- **`*((_DWORD *)this + 119)`** (mgr + 0x1DC) est la garde « en jeu » : presque
  tous les cas y renvoient à `LABEL_45` (avale la touche, ne fait rien) ;
- **104 et 105 partagent le même code** (`LABEL_60`) : ils ouvrent la **même**
  fenêtre 69 et ne diffèrent que par `this + 464`, l'**onglet** (0 = friends,
  1 = party). Une mesure qui attribue 69 au seul 104 est incomplète.

⚠ Le comportement **163** existe dans l'EXE (`ToggleOption(266)`) mais
`HOTKEY_2` s'arrête à l'index 61 : **aucune ligne de réglage, aucune touche
possible**. Il est inatteignable.

---

## 4 bis. L'autre racine locale : la barre d'icônes

Les **17** sites de `ToggleWindowById` sont tous dans `UIMenuIconWnd_OnMsg`
`0x00814B00`, un `switch` sur la **commande d'icône** :

| fenêtre | classe | statut du dépôt |
|---:|---|---|
| 8, 10, 11, 37, 69, 140, 155, 198, 263, 270, 324, 346 | inventaire, équipement, statut, skills, groupe, carte, menu, replay, RODEX, hauts faits, party board, réputation | outillées ou déjà tranchées |
| **303** | `UIHotkeyGuideWnd` | décrite (`docs/`) |
| **315** | `UITipboxWnd` | **jamais citée** — voir §5 |
| **10002** | `CUIAdventureGuide` | **jamais citée** — voir §5 |
| **10008** | `CUI…` (UI de quêtes) | **jamais citée** |

🔴 C'est cette table qu'il fallait lire pour trouver l'ouvreur de la file de
battleground (157) — il n'y est pas, parce que **WARP l'ajoute** : voir §6.

## 5. Les candidates, triées

Les quatre questions de [unexplored_systems.md](unexplored_systems.md) §1
appliquées telles quelles.

### ✅ 237 `UIClanInfoManageWnd` — **Ctrl+G, VIVANTE, jamais approchée**

➡ **Sujet relevé en détail : [clan_window_re.md](clan_window_re.md)** (structure
du bloc d'état, découpage des quatre ZC, mise en page, et trois anomalies du
natif à ne pas reproduire). Ce qui suit en est le résumé.

1. **ouvrable ?** oui, Ctrl+G, sans autre garde que « en jeu » ;
2. **transport ?** le serveur de map ;
3. **ouvreur chargé ?** `moon/rathena/other/clans.txt` est dans
   `moon/scripts_moon.conf:364` — les maîtres de clan sont **en jeu** :
   *Raffam Oranpere* (prontera), *Devon Aire* (geffen), *Berman Aire*
   (prontera), *Shaam Rumi* (payon), plus un *Clan Helper* à `prontera,138,183` ;
4. **base peuplée ?** mesuré sur la base de production le 2026-09-04 :
   **4 clans, 8 alliances, 0 membre**. Personne n'a jamais rejoint.

C'est la **seule fonctionnalité restante à la fois jouable, complète des deux
côtés, et jamais outillée** : ni la fenêtre 237, ni la classe
`UIClanInfoManageWnd`, ni aucune de ses données n'apparaissent dans `src/`,
`docs/` ou `uiwnd.h`.

⚠ **Le dépôt connaît déjà le bloc d'état** — mais pour une autre raison :
`rag::kClanStatePtrAddr` = **`0x0159c07c`**, le même pointeur, sert de garde au
**canal de chat de clan** (`chat_window.cc`, `emotion_hotkey.cc`, via
`clan[0x5C] == 1` = « le personnage est dans un clan »). C'est exactement
l'octet que teste `UIClanInfoManageWnd_OnCreate`
(`*((_BYTE *)g_pClanInfoMgr + 92)`). Ce qui manque n'est donc pas l'accès à la
donnée, c'est **tout ce que la fiche affiche** : niveau, maître, effectif,
carte administrée, alliés, hostiles.

Protocole, aligné des deux côtés (client ⇄ `src/map/packets_struct.hpp`) :

| ZC | nom rAthena | handler client | rôle |
|---|---|---|---|
| `0x0988` | `ZC_NOTIFY_CLAN_CONNECTINFO` | `Recv_ZC_NOTIFY_CLAN_CONNECTINFO_0x0988` `0x00CF8A00` | membres en ligne / max |
| `0x0989` | `ZC_ACK_CLAN_LEAVE` | `Recv_ZC_ACK_CLAN_LEAVE_0x0989` `0x00CF77C0` | départ du clan |
| `0x098A` | `ZC_CLANINFO` | `Recv_ZC_CLANINFO_0x098A` `0x00CF7950` | **la fiche complète** |
| `0x098E` | `ZC_NOTIFY_CLAN_CHAT` | `Recv_ZC_NOTIFY_CLAN_CHAT_0x098E` `0x00CF8860` | canal de clan |

Tout transite par un seul bloc d'état, **`g_pClanInfoMgr` `0x0159C07C`**
(`CClanInfoMgr_CreateInstance` `0x00A7DF60`), et ce bloc a **trois**
consommateurs, pas un :

- la fenêtre 237 elle-même ;
- **`GameMode_BuildActorNameLabel` `0x00C6D78F`** — le nom de clan sous le nom
  du personnage ;
- **`Chat_HandleChatMessage` `0x00C7A99C`** — le canal de clan.

Champs peints par `UIClanInfoManageWnd_OnPaint` `0x008158D0`, par msgstring :

| id | clé | contenu |
|---|---|---|
| 0x933 | `MSI_CLAN_INFOMANAGE` | titre |
| 0x934 | `MSI_CLAN_LEVEL` | niveau |
| 0x935 | `MSI_CLAN_NAME` | nom |
| 0x936 | `MSI_CLAN_MARK` | emblème |
| 0x937 | `MSI_ALLY_CLAN` | alliés (liste) |
| 0x938 | `MSI_HOSTILITY_CLAN` | hostiles (liste) |
| 0x93A | `MSI_CLAN_MASTER_NAME` | maître |
| 0x93B | `MSI_CLAN_NUM_MEMBER` | nombre de membres |
| 0x93C | `MSI_CLAN_MANAGE_LAND` | carte administrée (le `.rsw` est retiré puis passé à `Social_GetMapDisplayName`) |

Adresses de la classe :

```
0x01028750  vtable UIClanInfoManageWnd    0x008153D0  ctor
0x00815580  dtor                          0x008157D0  OnCreate
0x008158D0  OnPaint                       0x00816230  OnLButtonDown
0x00816240  OnMsg                         0x00816350  RebuildLayout
0x00816400  vf2C                          0x0159C07C  g_pClanInfoMgr
```

La fenêtre est **en lecture seule** : `OnLButtonDown` ne fait que fermer (237),
`OnMsg` délègue au défaut. Rejoindre ou quitter passe par le NPC.

### ⛔ 125 `UIMerInfoWnd` — Ctrl+R, **morte par base vide**

`case 125:` est gardé par `dword_15FFA50` (« un mercenaire est invoqué »),
exactement comme Alt+R l'est par `g_Homun_Present`. Or
`db/pre-re/mercenary_db.yml` compte **59 lignes et zéro `Body:`** — c'est le
**gabarit** livré par rAthena, jamais peuplé (témoin positif : `pet_db.yml` du
même répertoire en compte un). Aucun mercenaire ne peut exister ⇒ la garde
n'est jamais franchie. Même piège que la reforge et l'enchantement.

À rouvrir **si et seulement si** quelqu'un peuple `mercenary_db.yml` : le
client est prêt (`0x00D80B30` = skill de mercenaire par index, la fenêtre 126
`UISkillListWnd` sait déjà l'afficher).

### ✅ 315 `UITipboxWnd` — Alt+D, **complète, alimentée, et jamais outillée**

Le `case 154:` n'a **aucune** garde, pas même « en jeu ». Ses données sont
chargées par **`TipBox_RegisterLuaAndLoad` `0x00773CD0`** (qui expose à Lua
`AddTip`, `AddPage`, `AddPageEx`, `AddImgcoord`, `AddIsTag`), appelé sans
condition par `CSession_ctor` `0x00D57780`.

> 🔴🔴 **Correction d'une conclusion fausse.** Une première passe avait annoncé
> « données absentes, la fenêtre s'ouvre vide », en cherchant `System\tipbox.lub`
> — le chemin que pousse le **vanilla**. Faux, et faux pour la raison même que ce
> document rappelle : **c'est l'exe LIVRÉ qui décide**. WARP réécrit l'opérande
> du `push` à **`0x00773DD6`** pour pointer sur une chaîne injectée en
> `0x0171DBE3` : **`SystemEN\tipbox.lub`**, qui existe et pèse **63 267 octets**.
> Dix chemins `System\…` sont ainsi redirigés (`iteminfo`, `Achievements`,
> `Towninfo`, `mapInfo`, `ChangeMaterial`, `CheckAttendance`, `OngoingQuests`,
> `PrivateAirplane`, `monster_size_effect`, `tipbox`), mécanisme que le dépôt
> décrivait **déjà** — [warp_patches.md](warp_patches.md) et
> `src/utils/game_paths.cc`. La leçon n'est pas nouvelle, elle n'avait pas été
> appliquée.

La fenêtre est donc **pleinement fonctionnelle** : ~350 `tip*.bmp` dans
`data.grf`, des pages en anglais, un champ de recherche. Et elle a **huit**
ouvreurs, pas un — c'est de loin la fenêtre la plus accessible du lot :

| ouvreur | où | atteint par un paquet ? |
|---|---|---|
| **Alt+D** | `DispatchHotkeyBehavior` `0x00A46091` | non |
| **`/tip`** | `Chat_HandleChatMessage` `0x00C7B764`, **case 42** | non |
| **icône de menu** | `UIMenuIconWnd_OnMsg` `0x00814E69`, commande **511** | non |
| **lien rich-text** | `UIRichTextBox_OnMsg` `0x00862B0B` — la balise `<TIPBOX>…<INFO>n</INFO></TIPBOX>` | non |
| entrée sur la carte | `GameMode_OnEnterMapSetup` `0x00C6C219` | oui |
| ZC `0x0AE2` | `sub_CF98B0` `0x00CF98DD` | oui (mais jamais émis) |
| `sub_8042B0`, `sub_858710`, `sub_9E79D0` | — | non |

Le premier conseil du fichier livré le dit lui-même : *« You can open the tip box
by clicking /tip, Alt+D, or the Tipbox icon. »*

### ⛔ 10002 `CUIAdventureGuide` — icône de menu, **contenu vide**

Trouvée par `ToggleWindowById`. Le cadre est complet :
`luafiles514\Lua Files\AdventureGuide\AdventureGuide_F.lub` fait **18,6 Ko de
fonctions**, et les sept onglets existent. Mais leurs sept fichiers de contenu
(`Guide_Intro`, `Episode`, `Monster`, `Quest`, `Dungeon`, `Weapon`, `Armor`) ne
portent **qu'un titre et un numéro d'onglet** — ~100 octets utiles chacun,
**zéro accolade, zéro entrée**.

Encore le gabarit sans contenu, cette fois **côté client**. La 4ᵉ question du tri
(« la base est-elle peuplée ? ») vaut donc aussi pour les `.lub` du client, pas
seulement pour les `db/*.yml` et les tables SQL.

---

## 6. 🔴 Et l'exe LIVRÉ ? — WARP ajoute EXACTEMENT un ouvreur

Tout ce qui précède est mesuré sur l'IDB, c'est-à-dire sur le **vanilla**
([[reference_ida_is_vanilla_warp_patches]]). Or c'est exactement ainsi qu'on
avait manqué l'ouvreur de la file de battleground, injecté par WARP.

Contrôle outillé :
[`tools/warp/find_injected_openers.py`](../tools/warp/find_injected_openers.py)
scanne les deux binaires **de la même façon** (tout `E8 rel32` de `.text` et
`.xdiff` visant l'une des **trois** primitives, plus `FindWindow`,
`SaveRectAndCloseWindow` et les deux fonctions de raccourci), puis compare.

> 🔴🔴 **Une première passe, limitée à `MakeWindow`, avait conclu « WARP n'ajoute
> aucun ouvreur ». C'était faux** — et pour la raison du §1 : les icônes de menu
> n'ouvrent pas par `MakeWindow`, mais par `ToggleWindowById`.

Résultat complet — **trois** appels n'existent que dans l'exe livré :

| adresse | appel | ce que c'est |
|---|---|---|
| **`0x0171EB71`** | **`ToggleWindowById(0x9D = 157)`** | **l'ouvreur de la file de battleground** |
| `0x0171FC37` | `FindWindow(0x24 = 36)` | barre de raccourcis |
| `0x0171FC74` | `ToggleWindow(36, …)` | idem |

Le premier est le bloc injecté attendu :

```
0171EB50  cmp   dword ptr [ebp+0x10], 0x178   ; commande d'icône 376 = `battle`
0171EB57  jne   skip
0171EB6C  push  0x9D                          ; fenêtre 157 UIEntryQueueWnd
0171EB71  call  UIWindowMgr_ToggleWindowById
0171EB76  jmp   0x00A24D70
```

C'est **exactement** ce que [entry_queue_re.md](entry_queue_re.md) avait dû
établir à la main. Le scanner le retrouve seul : c'est le **témoin positif nommé**
de la méthode, bien plus solide qu'un simple décompte.

Les deux autres appels n'ouvrent rien de neuf : un détour posé dans
`CharUserData_LoadShortCutBarState` `0x005C45C0` (retour en `0x005C493F`) qui
restaure le rectangle de `UIShortCutWnd` (36) depuis `0x0131FC30` puis lui envoie
`cmd 0x22` et `cmd 6` (bouton `0x24C` ou `0x24D` selon `0x016024C5`).

🔴 **Second témoin, quantitatif** : la section `.xdiff` passe de **799 octets non
nuls et 17 appels** (vanilla) à **8903 octets et 98 appels** (livré) — le reste du
code injecté appelle surtout `Lua_ExecuteScriptFile` (46 fois) et le parseur XML
du `clientinfo`.

➡ Détail des patchs : [warp_patch_map.md](warp_patch_map.md),
[warp_patches.md](warp_patches.md).

## 7. Refaire la mesure

Un seul intervenant sur IDA à la fois, et **jamais `decompile` sur une adresse
de `case`** ([[feedback_re_method]] §10).

1. `XrefsTo` des **trois** primitives — `0x00A39340` `MakeWindow`,
   `0x00A4BF30` `ToggleWindow`, `0x00812E60` `ToggleWindowById` — en remontant
   ≤ 8 instructions au `push <imm>` (s'arrêter sur un autre `call`) → les couples
   (fonction, identifiant). 🔴 `MakeWindow` seul **manque les icônes de menu** ;
2. parcours avant depuis `0x00C9DF00`, appels directs, profondeur 3 → l'ensemble
   « atteint par un paquet » ; le complément est local ;
3. ne garder que les racines qui sont des **touches** :
   `UIWindowMgr_DispatchHotkeyBehavior` ;
4. les cas du `switch` par `idaapi.calc_switch_cases` — la valeur de case **est**
   le comportement ;
5. la touche se lit dans `hotkey.lub` du GRF, pas dans l'EXE : c'est un bytecode
   Lua 5.1, `HOTKEY_2[comportement − 100]`, champs `KEY1` (code) et `KEY2`
   (modificateur), noms dans `KEYNAME` ;
6. croiser avec la colonne « statut Bourgeon » de
   [native_window_dispatch.md](native_window_dispatch.md) §15 ;
7. **et vérifier l'exe livré** :
   [`tools/warp/find_injected_openers.py`](../tools/warp/find_injected_openers.py)
   — il imprime son propre témoin positif, à lire avant sa conclusion.

➡ **À refaire à chaque changement de `moonlight.grf`** : c'est lui qui porte
`hotkey.lub`, donc les touches par défaut.

---

Voir aussi : [unexplored_systems.md](unexplored_systems.md),
[skill_driven_windows_re.md](skill_driven_windows_re.md),
[native_window_dispatch.md](native_window_dispatch.md).
