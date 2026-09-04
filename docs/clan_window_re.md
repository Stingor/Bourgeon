# Le système de clan — fenêtre 237 `UIClanInfoManageWnd`

Relevé du **2026-09-04** sur `2025-07-16_Ragexe_175220998_clientinfo.exe`.
Trouvé par le quatrième vecteur : [local_openers_re.md](local_openers_re.md).

**C'est la dernière fonctionnalité à la fois jouable sur Moonlight et jamais
outillée par Bourgeon.** Client complet, serveur complet, base peuplée,
PNJ en jeu — et zéro ligne dans `src/` pour la fenêtre.

---

## 1. Ouverture

**Ctrl+G**, comportement **144** (`MSI_HK_CLANWND_ONOFF`) de
`UIWindowMgr_DispatchHotkeyBehavior` `0x00A451E0`. Bascule pure : si la fenêtre
existe elle se ferme, sinon `MakeWindow(0xED)`. La **seule** garde est
`mgr + 0x1DC` (« en jeu ») — ni `feature.*`, ni paquet, ni NPC.

🔴 La classe porte son raccourci **en dur** :
`UIClanInfoManageWnd_OnCreate` appelle `UIWindow_AddCloseButton(this, "Ctrl+G")`.
C'est le témoin qui tranche contre la table Lua `BEHAVIOR_TO_WINDOWID`, laquelle
annonce à tort la fenêtre 257.

Aucun autre chemin d'ouverture : la mesure des 546 sites d'appel à `MakeWindow`
n'en donne qu'un pour 237, et aucun opcode ne l'ouvre.

## 2. Le bloc d'état `CClanInfoMgr` — `g_pClanInfoMgr` `0x0159C07C`

Créé par `CClanInfoMgr_CreateInstance` `0x00A7DF50`, vidé par `sub_A7E120`.
⚠ **Le dépôt le connaît déjà** sous le nom `rag::kClanStatePtrAddr`
(`src/ragnarok/globals.h:919`) — mais seulement pour l'octet `+0x5C`, qui garde
le **canal de chat de clan** (`chat_window.cc`, `emotion_hotkey.cc`).

| offset | type | contenu | écrit par |
|---|---|---|---|
| `+0x04` | `std::string` | nom du clan | ZC `0x098A` |
| `+0x1C` | `std::string` | nom du maître | ZC `0x098A` |
| `+0x34` | `int` | **« niveau »** — voir l'anomalie §5 | `OnCreate`, jamais un paquet |
| `+0x38` | `int` | membres **en ligne** | ZC `0x0988` |
| `+0x3C` | `int` | membres **maximum** | ZC `0x0988` |
| `+0x40` | `std::string` | carte administrée (avec `.rsw`) | ZC `0x098A` |
| `+0x58` | `uint32` | **ClanID** | ZC `0x098A` |
| `+0x5C` | `uint8` | **« le personnage est dans un clan »** | ZC `0x098A` (=1), `0x0989` (=0) |
| `+0x60` | `std::list` | alliés **et** hostiles, nœuds de 36 o | ZC `0x098A` |

Nœud de la liste : `{ next, prev, int opposition, std::string name }` —
`opposition` à l'offset **8** du nœud, `0` = allié, `1` = hostile. C'est le
miroir exact de `clan->alliance[i].opposition` côté rAthena.

## 3. Les quatre ZC, alignés des deux côtés

| ZC | nom rAthena | handler client | effet |
|---|---|---|---|
| `0x0988` | `ZC_NOTIFY_CLAN_CONNECTINFO` | `Recv_ZC_NOTIFY_CLAN_CONNECTINFO_0x0988` `0x00CF8A00` | `+0x38` ← `p+2`, `+0x3C` ← `p+4` |
| `0x0989` | `ZC_ACK_CLAN_LEAVE` | `Recv_ZC_ACK_CLAN_LEAVE_0x0989` `0x00CF77C0` | vide le bloc, `+0x5C` ← 0 |
| `0x098A` | `ZC_CLANINFO` | `Recv_ZC_CLANINFO_0x098A` `0x00CF7950` | **la fiche complète** |
| `0x098E` | `ZC_NOTIFY_CLAN_CHAT` | `Recv_ZC_NOTIFY_CLAN_CHAT_0x098E` `0x00CF8860` | ligne de chat de clan |

Découpage de `ZC_CLANINFO` lu dans le client, vérifié champ à champ contre
`struct PACKET_ZC_CLANINFO` (`src/map/packets_struct.hpp:2094`) et son émetteur
`clif_clan_basicinfo` (`src/map/clif.cpp:26566`) :

| offset | taille | champ | destination |
|---|---|---|---|
| 0 | 2 | `PacketType` = `0x098A` | — |
| 2 | 2 | `PacketLength` | — |
| 4 | 4 | `ClanID` | `+0x58` |
| 8 | 24 | `ClanName` | `+0x04` |
| 32 | 24 | `MasterName` | `+0x1C` |
| 56 | 16 | `Map`, avec l'extension `.gat` | `+0x40` |
| 72 | 1 | `AllyCount` | compteur de boucle |
| 73 | 1 | `AntagonistCount` | compteur de boucle |
| 74 | 24×n | noms, **alliés puis hostiles** | `+0x60`, `opposition` 0 puis 1 |

Le client pose aussi `CNameDict_SetGuildPosition` après le nom du clan : c'est
ce qui alimente `GameMode_BuildActorNameLabel` `0x00C6D1D0`, **le nom de clan
sous le nom du personnage**.

## 4. Ce que la fenêtre peint

`UIClanInfoManageWnd_OnPaint` `0x008158D0`, coordonnées client :

| y | x | msgstring | contenu |
|---:|---:|---|---|
| 49 | 8 | `0x935` `MSI_CLAN_NAME` | `%s : %s` |
| 65 | 8 | `0x934` `MSI_CLAN_LEVEL` | `%s : %d` ← `+0x34` |
| 81 | 8 | `0x93A` `MSI_CLAN_MASTER_NAME` | `%s : %s` |
| 97 | 8 / 128 / 138 | `0x93B` `MSI_CLAN_NUM_MEMBER` | libellé, icône, puis `%d` ← `+0x38` |
| 113 | 8 | `0x93C` `MSI_CLAN_MANAGE_LAND` | cherche `.gat` (`memchr` sur `'.'` puis comparaison du `dword` `1952540462`), le remplace par `.rsw`, puis `Social_GetMapDisplayName` |
| 65 | 200 | `0x936` `MSI_CLAN_MARK` | emblème, chargé **seulement si `+0x58` ≠ 0** |
| 155 | 200 | `0x937` `MSI_ALLY_CLAN` | cadre 168×48, lignes à partir de y=157, pas de 16 |
| 229 | 200 | `0x938` `MSI_HOSTILITY_CLAN` | idem, second cadre |

Les deux listes parcourent **la même** `std::list` `+0x60` et filtrent sur le
drapeau `opposition`. La boucle s'arrête à `y >= 205` : **trois lignes visibles
par cadre**, sans ascenseur. `this[47]` / `this[48]` sont les lignes
sélectionnées (initialisées à −1), surlignées en 80×16.

**Lecture seule.** `OnLButtonDown` délègue à la base ; `OnMsg` `0x00816240` ne
traite que `cmd 6` (bouton 201 = fermer), `cmd 60` (reconstruire) et `cmd 34`
(ancrage). Rejoindre ou quitter passe par le PNJ, pas par la fenêtre.

## 5. 🔴 Trois anomalies du natif, à ne pas reproduire

1. **Le « niveau » de clan est faux.** `MSI_CLAN_LEVEL` lit `+0x34`, et le seul
   écrivain de `+0x34` est `OnCreate` lui-même :
   `if (mgr[0x5C]) *(int*)(mgr+0x34) = 1;`. Aucun paquet ne porte de niveau de
   clan, et rAthena n'en a pas la notion. **La fenêtre affiche donc toujours
   « niveau : 1 ».**
2. **Le nombre de membres n'est que le nombre EN LIGNE.** `+0x38` est affiché,
   `+0x3C` (le maximum, pourtant reçu dans le même paquet) ne l'est jamais.
3. **Trois alliés et trois hostiles au maximum.** Le garde-fou est une
   coordonnée (`y >= 205`), pas un compte : au-delà, les entrées existent en
   mémoire et ne se voient nulle part. `MAX_CLANALLIANCE` vaut 6 côté serveur,
   et Moonlight en a **8 en base** — donc le cas déborde déjà.

## 6. Côté Moonlight — c'est jouable aujourd'hui

- PNJ chargés : `moon/rathena/other/clans.txt`, déclaré dans
  `moon/scripts_moon.conf:364` ;
- un *Clan Helper* à `prontera,138,183` explique le système, et les quatre
  maîtres recrutent : *Raffam Oranpere* (`prt_in`), *Devon Aire* (geffen),
  *Berman Aire* (prontera), *Shaam Rumi* (payon) ;
- base de production, mesurée le 2026-09-04 : **4 clans** (Swordman, Arcwand,
  Golden Mace, Crossbow — 500 membres max chacun), **8 alliances**,
  **0 membre**. Personne n'a jamais rejoint ;
- les clans vivent en **SQL** (`clan_table: clan`, `clan_alliance_table` dans
  `conf/import/inter_conf.txt`), pas dans un `db/*.yml` — c'est pourquoi le
  balayage des `db/` ne les voyait pas.

## 7. Table d'adresses

```
── Fenêtre (id 237 = 0xED) ──────────────────────────────────────────────────
0x01028750  vtable UIClanInfoManageWnd    0x008153D0  ctor
0x00815580  dtor                          0x008157D0  OnCreate
0x008158D0  OnPaint                       0x00816230  OnLButtonDown
0x00816240  OnMsg                         0x00816350  RebuildLayout
0x00816400  vf2C

── État ─────────────────────────────────────────────────────────────────────
0x0159C07C  g_pClanInfoMgr (= rag::kClanStatePtrAddr)
0x00A7DF50  CClanInfoMgr_CreateInstance   0x00A7E120  purge du bloc

── Réseau ───────────────────────────────────────────────────────────────────
0x00CF8A00  ZC 0x0988 CONNECTINFO         0x00CF77C0  ZC 0x0989 LEAVE
0x00CF7950  ZC 0x098A CLANINFO            0x00CF8860  ZC 0x098E CHAT

── Autres consommateurs du bloc ─────────────────────────────────────────────
0x00C6D1D0  GameMode_BuildActorNameLabel (nom de clan sous le personnage)
0x00C7A460  Chat_HandleChatMessage (canal de clan)
0x00A47400  ChatMacro_SendEmotionHotkeySlot

── Raccourci ────────────────────────────────────────────────────────────────
0x00A451E0  DispatchHotkeyBehavior, case 144   0x00A32C10  ResolveBehavior
```

---

Voir aussi : [local_openers_re.md](local_openers_re.md),
[chatbox_re.md](chatbox_re.md), [entity_nameplate_re.md](entity_nameplate_re.md),
[native_window_dispatch.md](native_window_dispatch.md).
