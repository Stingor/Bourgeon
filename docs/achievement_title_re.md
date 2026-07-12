# Titres en récompense d'achievements — RE complète

> Client `Moonlight-Destiny.exe` (Ragexe 20250716, imagebase 0x400000) recoupé avec le
> serveur moonlight (fork rAthena, `d:\Mes documents\GitHub\moonlight`). Objectif :
> documenter **de bout en bout** comment un titre décroché via un achievement est défini,
> attribué, possédé, équipé et affiché.

## TL;DR

- Un **titre** = un simple **entier** (id `1000..1046`, cf. `TITLE_BASE..TITLE_MAX`). Il n'a **pas** de
  texte côté réseau : le libellé est **100 % client**, résolu par le Lua
  `Lua Files\DataInfo\TitleTable.lub` via la fonction globale **`GetTitleString(id)`**.
- **Quel achievement donne quel titre** = défini **côté client** dans `SystemEN\Achievements.lub`
  par l'appel Lua **`InsertAchieveRewardTitle(achID, titleID)`**, ET **côté serveur** dans le YAML
  `db/.../achievement_db.yml` (`Rewards: TitleId:`). Les deux doivent être cohérents.
- **Titre équipé** = `sd->status.title_id` (serveur, **persisté en base**, colonne `char.title_id`).
- **Titres possédés** = `sd->titles` (serveur, **NON persisté** : reconstruit à chaque login depuis les
  achievements complétés). Côté client, la liste `g_OwnTitleList` alimente le menu déroulant de choix
  et est **dérivée** des achievements complétés (le paquet liste-achievements ne transporte aucun id de
  titre).
- **Affichage** : le titre apparaît au-dessus de la tête via `ZC_ACK_REQNAMEALL2` (0x0A30) qui porte le
  `title_id`, résolu en libellé par `GetTitleString`.

## Cycle de vie complet

```
                        DÉFINITION (statique, 2 sources à garder synchro)
  client  SystemEN\Achievements.lub : InsertAchieveRewardTitle(achID, titleID)  ──┐
  serveur achievement_db.yml        : Rewards: { TitleId: <1000..1046> }         ─┤
                                                                                  │
   ┌──────────────────────────────────────────────────────────────────────────────┘
   ▼
  1. PROGRESSION / COMPLÉTION (serveur pilote)
     - le serveur pousse la liste (ZC_ALL_ACH_LIST 0x0A23) et les maj (ZC_ACH_UPDATE 0x0A24)
       -> flags "completed" + "rewarded" par achievement (AUCUN id de titre dans ces paquets)
  2. RÉCLAMATION de la récompense
     - joueur clique "recevoir" -> CZ_REQ_ACH_REWARD (0x0A25) {achID}
     - serveur : achievement_check_reward -> intif_achievement_reward -> achievement_reward()
         run_script(...) ; si rewards.title_id : sd->titles.push_back(title_id) ; clif_achievement_list_all
     - ack : ZC_REQ_ACH_REWARD_ACK (0x0A26) {failed.B, achID.L}
  3. POSSESSION
     - serveur : sd->titles = { title_id des achievements où completed>0 } ; recalculé au login
       par achievement_get_titles() (intif.cpp, à la réception des achievements).  NON persisté.
     - client : g_OwnTitleList dérivée des achievements complétés (croisement avec le mapping Lua).
  4. ÉQUIPEMENT (choix du titre affiché)
     - dropdown (UITitleSelect_PopulateDropdown) : item0 = MsgStringTable(0xa7e) "aucun",
       puis un item par titre possédé (libellé GetTitleString, id en itemdata)
     - sélection -> CZ_REQ_CHANGE_TITLE (0x0A2E) {title_id.L}
     - serveur clif_parse_change_title : refuse si title_id ∉ sd->titles (ack result=1) ;
       sinon sd->status.title_id = title_id ; clif_name_area(sd) ; ack result=0
     - ack : ZC_ACK_CHANGE_TITLE (0x0A2F) {result.B, title_id.L}
       -> client Own_ApplyChangeTitleAck_ZC0A2F : g_Own_TitleId = title_id, refait mon name-tag,
          rafraîchit la fenêtre Équipement (0xa)
  5. AFFICHAGE au-dessus de la tête (tout acteur)
     - ZC_ACK_REQNAMEALL2 (0x0A30) {GID, name.24, party.24, guild.24, position.24, title_id.L}
       -> client Actor_ApplyNameAllWithTitle_ZC0A30 : GetTitleString(title_id) -> pose le name-tag.
```

## Protocole (opcodes) — vérifié client + serveur

| Opcode | Dir | Nom | Len | Charge utile | Rôle |
|---|---|---|---|---|---|
| 0x0A23 | ZC | ZC_ALL_ACH_LIST | var | hdr + count + score + level + exp + `{achID.L, completed.B, count[10].L, date.L, rewarded.B}`×N (50o/rec) | liste complète des achievements (pas d'id de titre) |
| 0x0A24 | ZC | ZC_ACH_UPDATE | 66 | score + level + exp + 1 achievement | maj d'un achievement (+ popup « complété ») |
| 0x0A25 | CZ | CZ_REQ_ACH_REWARD | 6 | `achID.L` | réclamer la récompense (`clif_parse_AchievementCheckReward`) |
| 0x0A26 | ZC | ZC_REQ_ACH_REWARD_ACK | 7 | `failed.B, achID.L` | ack réclamation |
| **0x0A2E** | **CZ** | **CZ_REQ_CHANGE_TITLE** | **6** | `title_id.L` | équiper un titre (`clif_parse_change_title`) |
| **0x0A2F** | **ZC** | **ZC_ACK_CHANGE_TITLE** | **7** | `result.B` (0=ok,1=non possédé), `title_id.L` | ack changement |
| **0x0A30** | **ZC** | **ZC_ACK_REQNAMEALL2** | **106** | `GID.L, name.24, party.24, guild.24, position.24, title_id.L` | name-tag + titre au-dessus de la tête |

> Note serveur : `clif_name()` ne remplit `title_id` que si `PACKETVER_MAIN_NUM >= 20150225 ||
> PACKETVER_RE_NUM >= 20141126 || PACKETVER_ZERO` (notre client 20250716 est au-dessus).

## Sources de données (fichiers Lua client)

| Fichier | Contenu | Chargé par |
|---|---|---|
| `System\Achievement_list.lub` / `SystemEN\Achievements.lub` | définition des achievements + **récompenses titre** via `InsertAchieveRewardTitle(achID, titleID)` | `CAchievementMgr_RegisterLuaAndLoad` @0x0062d9c0 |
| `Lua Files\DataInfo\TitleTable.lub` | table **id de titre → libellé**, exposée par la fonction globale `GetTitleString(id)` | loader DataInfo `FUN_00d64e99` (via `RunLuaFile` 0x00a9bc90) |

Bindings Lua enregistrés par `CAchievementMgr_RegisterLuaAndLoad` (natifs → Lua) :
`InsertAchieveInfo` (id, name, summary, detail, score), `InsertAchieveResource`,
`InsertAchieveRewardItem`, **`InsertAchieveRewardTitle`** (achID, titleID), `InsertAchieveRewardBuff`,
`InsertAchieveUIType`, `SetAchieveIDByTab`.

## Fonctions & données clés (Ghidra — renommées + commentées)

| Adresse | Nom Ghidra | Rôle |
|---|---|---|
| 0x0062d9c0 | `CAchievementMgr_RegisterLuaAndLoad` | enregistre les bindings Lua puis charge `SystemEN\Achievements.lub` |
| 0x0062ed20 | `AchieveLua_InsertAchieveInfo` | binding Lua `InsertAchieveInfo(id, name, summary, detail, score)` |
| 0x0062f450 | `AchieveLua_InsertRewardTitle` | binding Lua `InsertAchieveRewardTitle(achID, titleID)` |
| 0x0062e4e0 | `CAchievementMgr_SetRewardTitle` | écrit `titleID` au champ **+0x64** de l'AchievementInfo (map à `mgr+0xc`, clé=achID) |
| 0x0062c460 | `CAchievementMgr_GetInfo`* | récupère l'AchievementInfo statique (nom/objectifs/récompenses) par id |
| 0x00d89ed0 | `Title_GetStringById` | `GetTitleString(id)` (Lua `g_UILuaState`) → libellé du titre, "" si absent |
| 0x00cf2a50 | `Actor_ApplyNameAllWithTitle_ZC0A30` | applique ZC 0x0A30 : résout le titre + pose le name-tag de l'acteur |
| 0x00cfb210 | `Own_ApplyChangeTitleAck_ZC0A2F` | applique ZC 0x0A2F : `g_Own_TitleId=title_id`, refait mon name-tag, rafraîchit l'équipement |
| 0x008bc4a0 | `UITitleSelect_PopulateDropdown` | remplit le menu déroulant des titres possédés (libellés via `GetTitleString`) |
| 0x00d7c4d0 | `TitleList_SortByName` | tri de la liste de titres par libellé (quicksort sur `GetTitleString`) |
| 0x0077f1b0 | `UIAchievementWnd_RefreshRewardPanel` | panneau récompenses : titre (+0x50) → `reward_title.bmp` + libellé ; buff (+0x54) ; item (+0x4c) |
| 0x00778250 | `UIAchievementWnd_ctor` | ctor fenêtre achievement (id **0x10e**, vtable 0x01019584) |
| 0x01254d84 | `g_CAchievementMgr` (data) | singleton `CAchievementMgr` (map d'AchievementInfo à +0xc) |
| 0x016004fc | `g_Own_TitleId` (data) | mon titre **équipé** ; bloc `g_Own_Title` : +0x00=id, +0x04/08/0c=`std::vector<int> g_OwnTitleList` (possédés) |
| 0x01600500 | `g_OwnTitleList_begin` (data) | début du vecteur des titres possédés (lu par le dropdown) |

\* nom proposé (fetch info par id) ; non renommé si déjà exploité ailleurs.

## AchievementInfo (struct client, dans `g_CAchievementMgr` @+0xc, std::map<achID, info>)

- `+0x10` : achievement id (clé de tri de la map)
- `+0x0d` : flag sentinelle nœud (0 = nœud valide)
- `+0x64` : **id du titre-récompense** (écrit par `CAchievementMgr_SetRewardTitle`) — 0 si aucun
- (+ chaînes name/summary/detail + score renseignés par `InsertAchieveInfo`)

## Fenêtres UI

- **UIAchievementWnd** — id **0x10e**, ctor `0x00778250`, vtable `0x01019584`, textures `\achievement_re\*`.
  Onglets (`MSI_ACHIEVEMENT_TAB_*`), panneau détail avec récompenses (`reward_title.bmp`,
  `reward_buff.bmp`, `reward_item.bmp`). Position persistée par `WindowPosTweaks` (clé `achievement`).
- **Dropdown de sélection de titre** : `UITitleSelect_PopulateDropdown` @0x008bc4a0. L'item 0
  `MsgStringTable(0xa7e)` = « aucun titre ». Hébergé par la fenêtre d'équipement (id 0xa, rafraîchie
  après changement) — le titre équipé est aussi rendu par le doll/avatar `FUN_008b3190`.

## Modèle serveur (moonlight) — références

- `src/map/achievement.hpp` : `enum e_title_table { TITLE_NONE=0, TITLE_BASE=1000, TITLE_MAX=1046 }`,
  `struct s_achievement_db::ach_reward { t_itemid nameid; uint16 amount; script_code* script; uint32 title_id; }`.
- `src/map/achievement.cpp` :
  - `parseBodyNode` : lit `Rewards: TitleId:` (valide `TITLE_BASE..TITLE_MAX`) → `rewards.title_id`.
  - `achievement_reward()` (~l.704) : `if (rewards.title_id) { sd->titles.push_back(title_id); clif_achievement_list_all(sd); }`.
  - `achievement_get_titles(char_id)` (~l.752) : reconstruit `sd->titles` depuis les achievements `completed>0`.
- `src/map/intif.cpp` (~l.2219) : au chargement des achievements → `achievement_get_titles()` puis `clif_achievement_list_all()`.
- `src/map/clif.cpp` : `clif_parse_change_title` (~l.22426, valide vs `sd->titles`), `clif_change_title_ack`
  (~l.22403), `clif_name` (~l.11337, remplit `title_id`), `clif_achievement_list_all` (~l.23480).
- `src/char/char.cpp` : `title_id` persisté (colonne `char.title_id`, `SQLDT_ULONG`). `sd->titles` **non** persisté.

## Conséquences pratiques / pistes plugin

- **Ajouter un titre** = éditer les DEUX sources : le YAML serveur (`Rewards: TitleId`) et le Lua client
  (`InsertAchieveRewardTitle` + entrée dans `TitleTable.lub` pour le libellé). Un id présent d'un seul
  côté = titre sans nom (client) ou refus « non possédé » (serveur).
- **Renommer/traduire un titre** = purement client (`TitleTable.lub`), aucun redéploiement serveur.
- Un éventuel plugin Bourgeon (overlay de sélection de titres, aperçu, etc.) peut :
  - lire le titre équipé via `g_Own_TitleId` (0x016004fc) et la liste possédée via `g_OwnTitleList`
    (0x01600500..),
  - résoudre les libellés en appelant `Title_GetStringById` (0x00d89ed0),
  - envoyer `CZ_REQ_CHANGE_TITLE` (0x0A2E, `{title_id.L}`) pour équiper.
- Les titres possédés n'étant pas persistés serveur, ils sont **toujours** cohérents avec les achievements
  complétés : pas de désync possible entre « achievement complété » et « titre disponible » après un login.
