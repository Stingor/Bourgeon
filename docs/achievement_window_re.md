# Fenêtre « Succès » (UIAchievementWnd) — RE complète

> Client `Moonlight-Destiny.exe` (Ragexe 20250716, imagebase 0x400000), recoupé avec le serveur
> moonlight (fork rAthena). Complète [`achievement_title_re.md`](achievement_title_re.md) qui,
> lui, ne couvre que les **titres** décrochés en récompense.
>
> Objectif : disposer de tout ce qu'il faut pour **remplacer la fenêtre native par une vue ImGui**
> (lecture des données natives + envoi des mêmes paquets, aucune donnée dupliquée en dur).

## TL;DR

- La fenêtre est un **composite** : `UIAchievementWnd` (id **270 / 0x10E**, 750×468) qui contient
  quatre sous-fenêtres — bandeau (`UIAchUpperWnd`), liste (`UIAchListWnd`), détail (`UIAchViewWnd`)
  et résumé (`UIAchSummaryWnd`). Plus un HUD séparé de suivi : `UIAchTracingWnd` (id **271 / 0x10F**).
- **Deux sources de données distinctes**, à ne pas confondre :
  - le **catalogue statique** (nom, résumé, détail, objectifs, récompenses, icône) vient du **Lua
    client** (`Achievements.lub`) et vit dans `CAchievementMgr` (`g_CAchievementMgr` **0x01254D84**) ;
  - la **progression du joueur** (complété, compteurs, date, récompense réclamée) vient du **serveur**
    (ZC 0x0A23 / 0x0A24) et vit dans un `std::map` global à **0x016004E4**.
- Le score/rang du joueur sont 4 globales contiguës : **0x016004EC** (score total), **0x016004F0**
  (rang, u16), **0x016004F4** / **0x016004F8** (points du rang courant / du rang suivant).
- Réclamer une récompense = **CZ_REQ_ACH_REWARD (0x0A25)** `{W op, L achID}` — c'est exactement ce que
  fait le bouton natif (commandId **474** de `UIAchViewWnd`).
- Le **suivi** (épingler jusqu'à **3** succès dans un HUD) est **100 % client** : un `std::vector`
  à **0x01600518**, aucun paquet, rien de persisté.

## 1. Anatomie de la fenêtre

`UIAchievementWnd_ctor` **0x00778250**, vtable **0x01019584**, RTTI `.?AVUIAchievementWnd@@`.
Taille posée par `UIWindowMgr_MakeWindow` (case 270, `SetSize(0x2EE, 0x1D4)` @ 0x00A40524) = **750×468**.

`UIAchievementWnd_OnCreate` **0x0077B920** construit :

| Champ | Sous-fenêtre | vtable | Taille / position | Rôle |
|---|---|---|---|---|
| `+0xB4` | `UIAchUpperWnd` | 0x0101914C | `largeur×46` @ (0,17) | bandeau : score, rang, jauge, 2 radios de filtre |
| `+0xB8` | (arbre d'onglets `UIAchTab`) | 0x01018F9C | 102×40, lignes de 26 px | colonne d'onglets à gauche (+ `UIAchScrollBar`) |
| `+0xBC` | `UIAchListWnd` | 0x010193D4 | 320×405 @ (102,63) | liste des succès de l'onglet courant |
| `+0xC0` | `UIAchViewWnd` | 0x010192FC | 328×405 @ (422,63) | détail : description, objectifs, récompenses |
| `+0xC4` | `UIAchSummaryWnd` | 0x010194AC | 648×404 @ (102,63) | vue **Résumé** (onglet 0), superposée aux deux précédentes |
| `+0xC8` / `+0xCC` | — | — | — | onglet / sous-onglet courants (`-1` = onglet racine entier) |
| `+0xD0` / `+0xD4` | — | — | — | `std::map<achID, AchievementInfo>` : **copie** des infos de l'onglet courant |

Le champ `+0xD0` est une copie de travail alimentée par `CAchievementMgr_CopyTabInfosToMap`
(0x0062C720) à chaque changement d'onglet : c'est lui que lit le rendu de la liste, pas le manager.

### Messages internes (`UIAchievementWnd_OnMsg` 0x0077E0E0)

| msg | Paramètres | Effet |
|---|---|---|
| 6 | cmd 201 | bouton fermer → `SaveRectAndCloseWindow(270)` (⚠ **détruit** la fenêtre) |
| 22 | `(tab, subtab)` | changement d'onglet ; `(-1,-1)` = ré-applique l'onglet courant (utilisé après un changement de filtre) |
| 60 | — | **refresh complet** : détail + résumé + compteurs du bandeau. C'est le message que les handlers réseau envoient |
| 135 | `achID` | sélectionne un succès → `viewWnd+0xE0 = achID`, `+0xE4 = &AchievementInfo` puis `UIAchViewWnd_RefreshDetail` |

### Bandeau — `UIAchUpperWnd`

- `OnCreate` 0x0077B0D0 : deux jauges (84×10 @ (47,24) libellée `MsgStringTable 0xC21` = prochain
  rang ; 124×8 @ (613,29)) et **deux boutons radio 12×12** @ (150,10) et (150,25).
- `OnDrawContent` 0x0077CDD0 : en mode Résumé, fond `bg_upper.bmp` + `upper_trophy.bmp` @ (575,3),
  libellés `0xC22` (total) / `0xC23` (incomplet), `(%d/%d)` aligné à droite @ (737,15), le score
  centré @ (353,6). Hors Résumé, seulement `%d/%d` (complétés/total de l'onglet) et le score.
- `OnMsg` 0x0077DFD0 : msg 6 cmd **215** = clic sur un radio → l'index coché est stocké en
  `upperWnd+180` (= le **filtre** de la liste) puis msg 22 `(-1,-1)` au parent.

### Liste — `UIAchListWnd`

`OnDrawContent` **0x0077BDB0**, une ligne par succès visible (hauteur `this[33]`, index =
`i + this[35]` où `this[35]` est le scroll) :

| Élément | Position | Source |
|---|---|---|
| fond de ligne | (0, y) | `list_press` / `list_complete_press` (sélectionnée) sinon `list_out` / `list_complete_out` |
| icône | (8, y+3) | `\achievement_re\icon_<info.uiParam>.bmp` |
| épingle | (8, y+3) | `list_pin.bmp` si `Achievement_IsTracked(achID)` |
| nom | (53, y+14) | `info.name`, taille 12 |
| score | x=258, largeur 40 | `%3d` = `info.score` |
| résumé | (10, y+54) | `info.summary` |
| badge « complété » | (250, y+5) | `badge_complete.bmp` |
| coffre récompense | (263, y+49) | `list_rewardbox_not_receive.bmp` (à réclamer) / `list_rewardbox_default.bmp` |
| date | centrée x=276, y+55 | `%4d.%2d.%2d` (progression, champs année/mois/jour) |

`OnMsg` 0x0077DE40 : msg 6 = **clic sur une ligne → bascule le suivi** (`Achievement_ToggleTracking`)
et ouvre le HUD 0x10F ; msg 7/9/10 = molette / page haut / page bas. La **sélection**, elle, passe par
le parent (msg 135).

`UIAchListWnd_PopulateFromTab` **0x0077E5B0** reconstruit le vecteur d'ids affichés (`this[40..42]`)
selon le radio du bandeau :

- **filtre 0** : les complétés d'abord (triés par date de complétion, `sub_777480` → tri à 3 clés
  0x00776E30), puis les non complétés ;
- **filtre 1** : uniquement les complétés **dont la récompense n'est pas réclamée**
  (`CAchievementMgr_HasUnclaimedReward` 0x0062E750), puis les non complétés.

### Détail — `UIAchViewWnd`

`OnCreate` **0x0077B2E0** :

| Champ | Contrôle | Position |
|---|---|---|
| `+0x7C` | bouton bitmap `btn_receive_{out,over,press,disable}.bmp`, commandId **474** | (90,372) |
| `+0x80` | `UIAchTV` (RichTextBox) 300×60 | (12,34) — description détaillée |
| `+0x84`…`+0xA8` | 10 `UIAchTV` 280×40 | (28, 99+19·i) — les 10 objectifs |
| `+0xAC`…`+0xD0` | 10 jauges 254×10 | — progression par objectif |
| `+0xD4`/`+0xD8`/`+0xDC` | 3 `UIToolTipBitmapWnd` | y=304, centrées, pas de 70 — récompenses titre / buff / objet |
| `+0xE0` / `+0xE4` | achID sélectionné / `AchievementInfo*` | — |

`UIAchViewWnd_RefreshDetail` **0x0077F1B0** :

- description = `info+0x30` ;
- **bouton « recevoir »** activé si `(rewardItem | rewardTitle | rewardBuff) != 0` **et**
  `progress.rewarded == 0` ; grisé (`bouton+172 = 1`) tant que `progress.completed != 1` ;
- objectifs : `info.resources[i]` ; si `info.uiType == 1` → texte `%s ( %d / %d )` avec
  `progress.count[i]` / `resource.target` **et** jauge visible ; sinon simple puce
  (`detail_bullet.bmp`). Couleur du texte : vert si objectif atteint, gris si compteur nul ;
- récompenses : titre → `reward_title.bmp` + `GetTitleString(info+0x50)` ; buff → `reward_buff.bmp`
  + Lua `GetStateIconDescript(info+0x54)` ; objet → `reward_item.bmp` + itemID `info+0x4C`.

`OnMsg` 0x0077E060 : msg 6 cmd **474** → envoie **CZ_REQ_ACH_REWARD (0x0A25)** avec l'achID courant ;
msg 145 = clic sur un lien → `UIAchievementWnd_SelectAchievementById`.

### Résumé — `UIAchSummaryWnd`

`OnCreate` 0x0077AEB0 : une jauge globale 558×10 @ (43,49), **6 jauges de catégorie** 254×10 en
2 colonnes × 3 lignes à partir de (43,123) (pas 304 / 50), et **2 `UITextButton`** @ (62,325) et
(378,325). `UIAchSummaryWnd_Refresh` 0x0077EDA0 les remplit :
`Achievement_GetTwoMostRecentCompleted` pour les deux boutons (les 2 succès obtenus les plus
récents, cliquables), puis `CountCompletedInTab(i,-1)` / `CountInTab(i,-1)` par catégorie.

### HUD de suivi — `UIAchTracingWnd` (id 271 / 0x10F)

`OnMsg` **0x00780660**. Liste jusqu'à **3** succès épinglés (`Achievement_ToggleTracking`
0x00D7D780) ; au-delà, message chat `MsgStringTable 0xA78`. Entrée = 44 octets :
`{B replié, B ?, L achID, std::string nom, vector<{std::string ligne, B atteint}> (28 o/élément)}`.
msg 98 cmd 478 = replier/déplier, cmd 479 = ouvrir 0x10E et sélectionner. Si la liste se vide, la
fenêtre se ferme toute seule. **Rien n'est persisté ni envoyé au serveur.**

## 2. Onglets

Clé d'onglet = `tab * 10 + subtab` (le 10 vient de `CAchievementMgr+8`).
Libellés = `MsgStringTable` **0xA60 → 0xA76**, construits par `UIAchievementWnd_BuildTabTree`
(0x0077A260) :

| tab | subtab | Id MSI | Constante |
|---|---|---|---|
| 0 | — | 0xA60 | `MSI_ACHIEVEMENT_TAB_SUMMARY` (vue Résumé) |
| 1 | — / 0 / 1 / 2 | 0xA61 / 0xA62 / 0xA63 / 0xA64 | `TAB_GENERAL` (+ `_CHARACTER`, `_ACTION`, `_REST`) |
| 2 | — / 0…5 | 0xA65 / 0xA66…0xA6B | `TAB_ADVENTURE` (+ `_RUNEMIDGARTS`, `_SCHWARZWALD`, `_ARUNAFELTZ`, `_ANOTHERWORLD`, `_LOCALIZING`, `_DUNGEON`) |
| 3 | — / 0 / 1 | 0xA6C / 0xA6D / 0xA6E | `TAB_BATTLE` (+ `_PVP`, `_TRAINING`) |
| 4 | — / 0 / 1 | 0xA6F / 0xA70 / 0xA71 | `TAB_QUEST` (+ `_EPISODE`, `_GENERAL`) |
| 5 | — / 0 / 1 / 2 | 0xA72 / 0xA73 / 0xA74 / 0xA75 | `TAB_MEMORIAL` (+ `_MIDGARD`, `_ANOTHERWORLD`, `_REST`) |
| 6 | — | 0xA76 | `TAB_ACHIEVEMENT` |

Autres ids utiles : **0xA77** `TAB_TOTAL` (titre de la vue Résumé), **0xA78**
`MSI_FAIL_ADD_ACHIEVEMENT_TRACING`, **0xA79** `MSI_NOTICE_COMPLETE_ACHIEVEMENT` (bannière),
**0xA7E** `MSI_TAKEOFF_TITLE`, **0xC21** `MSI_ACHIEVEMENT_NEXT_GRADE`, **0xC22** `MSI_THE_WHOLE`,
**0xC23** `MSI_INCOMPLETE`. (Mapping vérifié par recoupement de trois ancres indépendantes.)

L'appartenance d'un succès à un onglet est déclarée côté **client** par le binding Lua
`SetAchieveIDByTab(achID, tab, subtab)` → `CAchievementMgr_SetAchieveTab` 0x0062E7C0. Un succès
absent de ce mapping **n'apparaît dans aucun onglet**, même si le serveur en envoie la progression.

## 3. Catalogue statique — `CAchievementMgr`

Singleton `g_CAchievementMgr` **0x01254D84**, alloué par `CAchievementMgr_InitFromFile`
(0x0062C2C0), 0x1C octets :

| Offset | Contenu |
|---|---|
| +0x00 | vftable |
| +0x04 | `bool` initialisé |
| +0x08 | **10** = nombre max de sous-onglets (base de la clé d'onglet) |
| +0x0C / +0x10 | `std::map<achID, AchievementInfo>` (nœud 0x94, valeur à nœud+0x14) |
| +0x14 / +0x18 | `std::map<tabKey, std::vector<achID>>` (nœud 0x20) |

### `AchievementInfo` — 0x80 octets

| Offset | Type | Champ | Écrit par |
|---|---|---|---|
| +0x00 | `std::string` | `name` | `InsertAchieveInfo` |
| +0x18 | `std::string` | `summary` | idem |
| +0x30 | `std::string` | `detail` | idem |
| +0x48 | `int` | `score` | idem |
| +0x4C | `int` | `rewardItemId` (**-1** = aucun) | `InsertAchieveRewardItem` (0x0062E3C0) |
| +0x50 | `int` | `rewardTitleId` (**-1** = aucun) | `InsertAchieveRewardTitle` (0x0062E4E0) |
| +0x54 | `int` | `rewardBuffId` (**-1** = aucun) | `InsertAchieveRewardBuff` (0x0062E2A0) |
| +0x58/+0x5C/+0x60 | `std::vector<Resource>` | objectifs (élément **32 o**) | `InsertAchieveResource` (0x0062DFC0) |
| +0x64 | `int` | `uiType` (0 = puces, 1 = jauge `%d/%d`) | `InsertAchieveUIType` (0x0062E600) |
| +0x68 | `std::string` | `uiParam` — suffixe du nom d'icône | idem |

`Resource` (32 o) = `{ std::string libellé (24) ; int target (+24) ; int flag (+28, **-1** par défaut) }`
— `target` est la valeur à atteindre affichée dans `( %d / %d )`.

> ⚠ Les trois champs de récompense valent **-1** (et non 0) quand il n'y en a pas : le client teste
> partout `> 0`, jamais `!= 0`. Un plugin doit faire pareil.

> ⚠ Piège d'offsets : les setters manipulent le **nœud** de la map, donc leurs constantes sont
> décalées de +0x14 par rapport au tableau ci-dessus (ex. `rewardTitleId` = nœud+100 = info+0x50).
> L'ancienne note « titre = champ +0x64 » de `achievement_title_re.md` parlait du **nœud**.

## 4. Progression du joueur (runtime)

`std::map<achID, AchievementProgress>` — objet à **0x016004E4** (`_Myhead` +0x00, `_Mysize` +0x04),
nœud de 0x4C, valeur de **56 octets** :

| Offset | Type | Champ |
|---|---|---|
| +0x00 | `u8` | entrée valide (1) |
| +0x01 | `u8` | **completed** |
| +0x04 | `u32[10]` | `count[10]` — compteur par objectif |
| +0x2C | `u32` | date de complétion (epoch) |
| +0x30 | `u8` | **rewarded** (récompense réclamée) |
| +0x32 / +0x34 / +0x36 | `u16` | année / mois / jour (dérivés de la date par `localtime`) |

Lecture : `Achievement_GetProgressCopy` **0x00D7F940** `(this = 0x015FA3C0, out[56], achID)` — renvoie
une structure remplie de zéros si le succès est inconnu (donc « non commencé »).

Globales voisines (toutes écrites par les deux handlers réseau) :

| Adresse | Contenu |
|---|---|
| 0x016004EC | score total de succès (cumul de tous les `info.score` obtenus) |
| 0x016004F0 | rang (u16) |
| 0x016004F4 | points **déjà acquis dans le palier courant** (relatif, pas absolu) |
| 0x016004F8 | points **que fait le palier courant** (= largeur de la jauge) |
| 0x016004FC | titre équipé (cf. `achievement_title_re.md`) |
| 0x01600500 / 04 / 08 | `std::vector<int>` des titres possédés |
| 0x01600518 / 1C / 20 | `std::vector` des succès **suivis** (44 o/élément, max 3) |

## 5. Protocole

| Opcode | Dir | Len | Charge utile |
|---|---|---|---|
| **0x0A23** | ZC | var (≥22) | `{W op, W len, L count, L totalPoints, W rank, L rankCur, L rankNext, ACH[count]}` |
| **0x0A24** | ZC | 66 | `{W op, L totalPoints, W rank, L rankCur, L rankNext, ACH}` (l'enregistrement commence à +16) |
| **0x0A25** | CZ | 6 | `{W op, L achID}` — réclamer la récompense |
| **0x0A26** | ZC | 7 | `{W op, B failed, L achID}` |

`ACH` = **50 octets** : `{L achID, B completed, L count[10], L date, B rewarded}`.

Handlers (renommés dans l'IDB) :

- `Recv_ZC_ALL_ACH_LIST_0A23` **0x00CFE3D0** : écrit les 4 globales, insère chaque enregistrement
  (`Achievement_ApplyProgressRecord` 0x00D7E1C0), et pour chaque `rewarded == 1` appelle
  `Achievement_GrantRewardTitle` (0x00D7D6C0 → pousse le titre dans `g_OwnTitleList` et rafraîchit la
  fenêtre Équipement). Termine par msg 60 à la fenêtre 270 **si elle est ouverte**.
- `Recv_ZC_ACH_UPDATE_0A24` **0x00CF9BF0** : si le succès passe de « non complété » à « complété »,
  affiche une **bannière plein écran** (`MsgStringTable 0xA79` + nom), joue l'effet **EF 1094** sur le
  joueur et fait clignoter le menu (fenêtre 307). Puis maj progression, msg 60 à la 270, refresh du
  suivi, et ouverture éventuelle du HUD 0x10F.
- `Recv_ZC_REQ_ACH_REWARD_ACK_0A26` **0x00CFA170** : si `failed == 0` → msg 60 à la 270 puis
  `Achievement_GrantRewardTitle(achID)`.

Côté serveur (moonlight) : `db/pre-re/achievement_db.yml` (**517** entrées pre-renewal, groupes
`Adventure`, `Battle`, `Taming`, `Goal_*`, `Job_Change`, …) et `db/pre-re/achievement_level_db.yml`
(paliers de points → rang). Le client, lui, ne connaît que ce que déclare `Achievements.lub` : les
deux doivent rester synchronisés (id, objectifs, récompenses).

## 6. Ouverture de la fenêtre

- Raccourci / action : `UIWindowMgr_DispatchHotkeyBehavior` **0x00A451E0**, **behavior 149** —
  ferme la fenêtre si ouverte (`SaveRectAndCloseWindow(0x10E)`), sinon `MakeWindow(0x10E)`.
- Position persistée par le plugin `WindowPosTweaks` (clé `achievement`).

## 7. Table des symboles (IDB à jour : renommés + commentés)

| Adresse | Nom |
|---|---|
| 0x00778250 | `UIAchievementWnd_ctor` |
| 0x0077B920 | `UIAchievementWnd_OnCreate` |
| 0x0077D410 | `UIAchievementWnd_OnDrawContent` |
| 0x0077E0E0 | `UIAchievementWnd_OnMsg` |
| 0x0077EB60 | `UIAchievementWnd_SetTab` |
| 0x0077DA40 | `UIAchievementWnd_SelectAchievementById` |
| 0x0077A260 | `UIAchievementWnd_BuildTabTree` |
| 0x0077B0D0 / 0x0077CDD0 / 0x0077DFD0 | `UIAchUpperWnd_{OnCreate,OnDrawContent,OnMsg}` |
| 0x00779700 / 0x0077BDB0 / 0x0077DE40 | `UIAchListWnd_{InitLayout,OnDrawContent,OnMsg}` |
| 0x0077DBB0 / 0x0077E5B0 | `UIAchListWnd_{UpdateScrollBar,PopulateFromTab}` |
| 0x0077B2E0 / 0x0077D1E0 / 0x0077E060 | `UIAchViewWnd_{OnCreate,OnDrawContent,OnMsg}` |
| 0x0077F1B0 | `UIAchViewWnd_RefreshDetail` (ex-`UIAchievementWnd_RefreshRewardPanel`) |
| 0x0077AEB0 / 0x0077C570 / 0x0077DF10 / 0x0077EDA0 | `UIAchSummaryWnd_{OnCreate,OnDrawContent,OnMsg,Refresh}` |
| 0x00780660 | `UIAchTracingWnd_OnMsg` |
| 0x0062C2C0 | `CAchievementMgr_InitFromFile` |
| 0x0062C460 / 0x0062BEB0 | `CAchievementMgr_CopyInfoById` / `_GetInfoRefById` |
| 0x0062C0B0 / 0x0062B380 | `CAchievementMgr_GetOrCreateTabList` / `_FindTabList` |
| 0x0062C720 | `CAchievementMgr_CopyTabInfosToMap` |
| 0x0062D3D0 / 0x0062D550 | `CAchievementMgr_CountInTab` / `_CountCompletedInTab` |
| 0x0062D920 / 0x0062D380 / 0x0062E750 | `_FindTabByAchId` / `_GetRewardTitleId` / `_HasUnclaimedReward` |
| 0x00D7F940 | `Achievement_GetProgressCopy` |
| 0x00D7E1C0 / 0x00D7D120 | `Achievement_ApplyProgressRecord` / `AchievementProgressMap_Insert` |
| 0x00D7D780 / 0x00D8E3A0 | `Achievement_ToggleTracking` / `Achievement_IsTracked` |
| 0x00D7D6C0 | `Achievement_GrantRewardTitle` |
| 0x00D83CC0 | `Achievement_GetTwoMostRecentCompleted` |
| 0x00CFE3D0 / 0x00CF9BF0 / 0x00CFA170 | `Recv_ZC_ALL_ACH_LIST_0A23` / `_ACH_UPDATE_0A24` / `_REQ_ACH_REWARD_ACK_0A26` |
| 0x01254D84 | `g_CAchievementMgr` |
| 0x016004E4 / E8 | `g_OwnAchievementMap_head` / `_size` |
| 0x016004EC / F0 / F4 / F8 | `g_Own_AchievementTotalPoints` / `Rank` / `RankPointsCur` / `RankPointsNext` |
| 0x01600518 / 1C / 20 | `g_AchievementTrackList_begin` / `_end` / `_cap` |

Textures : `\achievement_re\` — `bg_tab`, `tab_{out,over,press,sub_out,sub_over,sub_press,subarrow}`,
`bg_upper`, `upper_trophy`, `bg_list`, `list_{out,press,complete_out,complete_press,pin}`,
`badge_complete`, `list_rewardbox_{default,not_receive}`, `icon_<uiParam>`, `bg_detail`,
`detail_{stamp,bullet}`, `btn_receive_{out,over,press,disable}`, `reward_{item,title,buff}`,
`bg_summary`, `scroll_v_*`.

## 8. Validation sur client live (x32dbg, 2026-07-28)

Toutes les structures ci-dessus ont été relues **en mémoire sur un client connecté**, pas seulement
déduites du désassemblage :

| Lecture | Valeur observée | Ce que ça prouve |
|---|---|---|
| `g_CAchievementMgr` (0x01254D84) | → 0x03D7A3B8 ; `+0x00` = 0x00FE7918 (vftable), `+0x04` = 1, `+0x08` = **10**, `+0x10` = **363**, `+0x18` = **70** | layout du manager ; 363 succès déclarés par le Lua client, 70 clés d'onglets |
| nœud de `map<tabKey, vector>` | clé **25** → tab 2 / subtab 5 (Aventure ▸ Donjon), vecteur de **37** ids | formule `tab*10 + subtab` |
| `g_OwnAchievementMap` (0x016004E4) | head valide, size = **25** | map de progression distincte du catalogue |
| nœud de progression | clé 240000 ; `+0x14` = `01 01` (valide, complété) ; counts nuls ; `+0x40` = 0x5E7A41CF ; `+0x44` = 0 ; `+0x46/48/4A` = **2020 / 3 / 24** | layout `AchievementProgress` au bit près (epoch 0x5E7A41CF = 24 mars 2020 = les trois `u16`) |
| 0x016004EC…F8 | 565 / rang 7 / 15 / 178 | `achievement_level_db` pre-re : palier 7 = 550 pts, palier 8 = 728 → **565-550 = 15** et **728-550 = 178** : les deux derniers champs sont bien **relatifs au palier** |
| nœud de `AchievementInfo` (achID 128038) | name = « Show Jailbreak to the captain » (29 c.), score = **10**, item/titre/buff = **-1/-1/-1**, 1 objectif, `uiType` = **1**, `uiParam` = « **BATTLE** » (SSO) | layout `AchievementInfo`, convention **-1 = aucune récompense**, icône = `icon_BATTLE.bmp` |
| `Resource[0]` | libellé « Eliminate 1 Ferlock » (19 c.), `target` = **1**, flag = **-1** | layout `Resource` (32 o) |
| 0x016004FC / 0x01600500 / 0x01600518 | 0 / vecteur vide / vecteur vide | perso sans titre équipé, aucun titre possédé, aucun succès suivi |

## 9. Conséquences pour une réécriture ImGui

Tout est lisible et pilotable **sans hooker le rendu natif** :

| Besoin | Comment |
|---|---|
| liste des succès d'un onglet | parcourir `g_CAchievementMgr+0x14` (`map<tabKey, vector<achID>>`) — ou tout le catalogue `+0x0C` pour une vue « tout à plat » |
| métadonnées d'un succès | `CAchievementMgr_GetInfoRefById` (0x0062BEB0) → `AchievementInfo*` (pas de copie, pas de `std::string` à reconstruire) |
| progression | `Achievement_GetProgressCopy` (0x00D7F940) — 56 octets sur la pile, sans allocation |
| score / rang / jauge | les 4 globales 0x016004EC..F8 |
| réclamer | envoyer CZ 0x0A25 `{W, L achID}` (mêmes conditions que le natif : `completed && !rewarded && (item > 0 \|\| title > 0 \|\| buff > 0)`) |
| suivre / dé-suivre | `Achievement_ToggleTracking` (0x00D7D780) — garde la compatibilité avec le HUD natif |
| titre récompense | `CAchievementMgr_GetRewardTitleId` (0x0062D380) + `Title_GetStringById` (0x00D89ED0) |
| icônes | `\achievement_re\icon_<uiParam>.bmp` via le `UITextureMgr` (cf. `ui/game_texture.h`) |
| notification de maj | il **suffit** d'observer la fenêtre 270 ou de poller ; en pratique, se brancher sur les handlers 0x0A23/0x0A24 (ou relire à chaque frame, le coût est nul) |

Les libellés d'onglets et les messages viennent de `MsgStringTable` (ids §2) : **aucun texte à écrire
en dur**, on garde la traduction du client.

## 10. Proposition : plugin `AchievementsViewer` (ImGui)

### Ce que la fenêtre native fait mal

| Limite native | Conséquence |
|---|---|
| pas de recherche textuelle | 363 succès (live) répartis en 20 sous-onglets : retrouver « celui avec les Ferlock » est impossible |
| filtre binaire (2 radios) | pas de « en cours », pas de « suivis », pas de tri par progression |
| réclamer = 3 clics (onglet → ligne → bouton) | avec plusieurs récompenses en attente, c'est répétitif ; aucun « tout réclamer » |
| progression par catégorie visible **uniquement** dans l'onglet Résumé | on perd le contexte dès qu'on navigue |
| clic sur une ligne = épingler (!) alors que sélectionner passe par un autre chemin | interaction contre-intuitive |
| bannière plein écran + effet à chaque complétion | intrusif en combat |
| 750×468 fixes, textures bitmap | ne suit ni le DPI ni le thème du reste de l'UI Bourgeon |

### Maquette proposée (fenêtre unique, redimensionnable)

```
┌─ Succès ───────────────────────────────────────────────────────────────────┐
│ 🏆 Rang 7   ▓▓▓▓▓▓▓░░░░░░░░░ 15/178      565 pts      142/363 complétés    │
│ [ 3 récompenses à réclamer → Tout réclamer ]                               │
├────────────────┬───────────────────────────────────────────────────────────┤
│ 🔍 [ferlock  ] │ Tout | En cours | Complétés | À réclamer | Suivis    Tri ▾ │
│                ├───────────────────────────────────────────────────────────┤
│ Toutes    142/363│ ┌───┐ Show Jailbreak to the captain          10 pts  📌 │
│  Général   12/28 │ │IMG│ Éliminer 1 Ferlock                                │
│  Aventure  61/166│ └───┘ ▓▓▓▓▓▓▓▓▓▓░░░░░░ 0/1              [ Réclamer ]    │
│  Combat    30/58 │───────────────────────────────────────────────────────  │
│  Quête      9/24 │ ┌───┐ Kill 100 Porings                     5 pts   ✔    │
│  Mémorial  18/52 │ │IMG│ obtenu le 24/03/2020         🎁 Titre « Poring »  │
│  Succès     2/35 │ └───┘ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ 100/100                          │
└────────────────┴───────────────────────────────────────────────────────────┘
```

- **Bandeau héros** : rang + jauge du palier (0x016004F4 / F8), score total, compteur global, et un
  bouton d'action « X récompenses à réclamer ».
- **Colonne catégories** : les 6 catégories + « Toutes », chacune avec `complétés/total`
  (`CountCompletedInTab` / `CountInTab`) et une mini-jauge — l'info que le natif cache dans le Résumé.
  Sous-onglets en arbre repliable, libellés `MsgStringTable`.
- **Recherche + filtres** : recherche insensible à la casse sur nom / résumé / libellés d'objectifs ;
  filtres `Tout / En cours (count>0 && !completed) / Complétés / À réclamer / Suivis` ; tris
  `récent, progression %, score, alphabétique`.
- **Liste** : `ImGuiListClipper` (virtualisation) — icône `icon_<uiParam>.bmp` via
  `ui/game_texture.h`, nom, résumé, jauge **agrégée** (moyenne des `count[i]/target[i]`), badge de
  complétion, date, épingle (📌 = `Achievement_ToggleTracking`, compatible avec le HUD natif 0x10F),
  et **bouton « Réclamer » directement sur la ligne**.
- **Détail** : au survol/déplié — description complète, une ligne par objectif avec sa jauge, et les
  récompenses (objet via `ui/icon_cache.h`, titre via `Title_GetStringById`, buff via le Lua
  `GetStateIconDescript`).
- **Tout réclamer** : file d'attente, un `CZ 0x0A25` par tick (le serveur renvoie un `0x0A26` par
  succès ; pas de flood).
- Option « toast discret » remplaçant la bannière plein écran de `0x0A24`.

### Découpage

| Fichier | Rôle |
|---|---|
| `src/ragnarok/achievements.h/.cc` | **accès natif** : itération des deux `std::map` (catalogue + progression), structs POD miroir, `Info(achID)`, `Progress(achID)`, `Tabs()`, `Rank()`, `IsTracked()`, `ToggleTracking()`, `ClaimReward()` |
| `src/plugins/achievement_viewer.h/.cc` | plugin ImGui (`OnRenderUI`), opt-in via les réglages comme `InventoryViewer` |

Aucune donnée dupliquée : le plugin lit les structures du client (cf.
[`feedback_never_hardcode_use_native`]). Un simple compteur de version
(`g_OwnAchievementMap_size` + `g_Own_AchievementTotalPoints` + taille de la liste de suivi) suffit à
invalider le cache d'affichage sans hooker les handlers réseau.

### Pièges à respecter

1. 🔴 **Aucune commande native bloquante pendant la frame ImGui** — l'envoi de `CZ 0x0A25`, le
   `MakeWindow(0x10E)` ou l'ouverture du HUD doivent être **différés** (`OnTick` / `OnProcessInput`).
2. Lecture des `std::string` natives : SSO — `cap >= 16 ⇒ *(char**)p`, sinon buffer inline.
3. Récompenses absentes = **-1** (tester `> 0`).
4. Textes en CP949 → conversion avant affichage (même chemin que le chat / les tooltips d'objets).
5. Cache de textures à vider au reset du device (`Overlay_DeviceEpoch()`).
6. Ne pas court-circuiter le serveur : le bouton « Réclamer » ne doit s'activer que si
   `completed && !rewarded && récompense > 0`, sinon le serveur répond `failed` et l'UI ment.
