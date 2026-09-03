# La file d'attente de battleground (`CEntryQueueMgr`) — relevé complet

> Relevé du **2026-08-31** sur le client `2025-07-16_Ragexe` (IDA, base 0x400000)
> et sur le dépôt serveur `moonlight` (fork rAthena).
>
> Sujet **jamais touché par Bourgeon** : ni `src/`, ni `docs/` ne citaient
> `EntryQueue`, `CEntryQueueMgr` ni aucun des onze opcodes ci-dessous. Il était
> listé comme trou dans
> [native_window_dispatch.md](native_window_dispatch.md) §8 (« File d'attente
> d'instance », ids 157, 209, 210, 211) — **le nom était faux** : ce n'est pas
> une file d'instance, c'est la file d'attente de **battleground**.

## 1. Pourquoi ce document

Parce que le sous-système est **vivant sur Moonlight**, contrairement à la
plupart des fenêtres vierges du client :

- `conf/import/battle_conf.txt` laisse `feature.bgqueue: on` (il n'y touche pas,
  et `conf/battle/feature.conf` l'active) ;
- les cinq scripts BG sont chargés (`moon/scripts_moon.conf` lignes 123-137) ;
- `db/battleground_db.yml` déclare trois terrains ;
- **les trois noms de terrain du serveur et ceux de la table du client sont
  identiques au caractère près** (§7). Rien ne manque pour que ça marche.

⚠ Et pourtant ça ne marche pas correctement — trois défauts précis, §8.

## 2. Le gestionnaire

`CEntryQueueMgr` — singleton, pointeur en **`0x015C43E4`**
(`CEntryQueueMgr::CreateInstance()`, chaîne RTTI `.?AVCEntryQueueMgr@@`
@0x0124D5DC). C'est le `this` de **tous** les handlers de paquets du §5 (le
dispatcher fait `mov ecx, dword_15C43E4` avant chaque appel).

| offset | type | rôle | qui l'écrit |
|---|---|---|---|
| `+0x04` | `char[0x18]` | nom du terrain en file | ZC 0x08D8 (accepté), ZC 0x08DF |
| `+0x1C` | `char[0x18]` | nom du **lobby** | ZC 0x08DF |
| `+0x34` | `s_entry*` | l'entrée de table **sélectionnée** | sélection dans la liste, ZC 0x08D9 ; **remis à 0** par ZC 0x08DB et ZC 0x090E |
| `+0x38` | `bool` | « je suis en file » | 1 sur 0x08D8/0x08D9, 0 sur 0x08DB cas 14, 0x090E, et refus de lobby |

🔴 **`+0x34` est le point faible de tout le système** : trois chemins le lisent
sans le tester, deux le remettent à zéro. Voir §8.

## 3. La table des terrains

### 🔴🔴 Il y a DEUX tables, et une seule sert

Le piège le plus coûteux de ce relevé. Le binaire porte **deux `std::map`
distinctes** de terrains, remplies par des chemins différents :

| map | remplie par | lue par | verdict |
|---|---|---|---|
| **`0x015C43E8`** — `g_EntryQueueLuaTable` | `Lua_AddEntryQueue` 0x00b3b1b0 | 🔴 **les handlers de paquets et l'interface** | ✅ **la vraie** |
| `0x015E5AA0` — `g_EntryQueueTable` | `EntryQueueTable_ParseXlsRows` / `..._ParseBexFile` | son seul accesseur, `EntryQueue_FindEntry` 0x00c3c0d0, **n'a aucun xref** | ⛔ code mort |

Le témoin négatif est net : `EntryQueue_FindEntry` n'est appelée nulle part, et
`Lua_AddEntryQueue` écrit ses entrées dans `0x015C43E8` — le même arbre que
`Handler_ZC_NOTIFY_ENTRY_QUEUE_APPLY` parcourt pour retrouver un terrain par son
nom. **Les chargeurs `.xls` / `.bex` ne servent plus à rien.**

⚠ Les deux globales portaient un nom qui se ressemblait ; l'IDB les distingue
désormais (`g_EntryQueueLuaTable` contre `g_EntryQueueTable_XlsBex_UNUSED`).
Modifier `Data\Table\EntryQueue.xls` n'aurait **aucun effet en jeu**.

### Les trois chemins de chargement

L'enregistrement fait **0x738 octets** dans les trois cas.

| chemin | fonction | verdict |
|---|---|---|
| **Lua** | `Lua_LoadAllScriptFiles` 0x00d646c0 charge `Lua Files\EntryQueue\EntryQueueList` ; la fonction C `AddEntryQueue` est enregistrée par `sub_A9BC00` 0x00a9bc00 et implémentée par `Lua_AddEntryQueue` 0x00b3b1b0 | ✅ **c'est celui-ci** — le `.lua` est présent dans `moonlight.grf` |
| `.xls` | `EntryQueueTable_ParseXlsRows` 0x00c3ad60, `Data\Table\EntryQueue.xls` | ⛔ alimente la map morte |
| `.bex` | `EntryQueueTable_ParseBexFile` 0x00c3b530, `Data\Table\EntryQueue.bex` | ⛔ alimente la map morte ; ouvre en plus par **`CreateFileA` direct**, donc **hors GRF** |

Arbre rouge-noir standard MSVC : `nœud+0x10` = clé `int` (l'id de la ligne),
`nœud+0x14` = pointeur d'entrée.

### Le schéma, colonne par colonne

Trois sources concordent, ce qui **nomme** chaque champ de l'enregistrement
binaire sans supposition : les colonnes lues dans l'ordre par
`EntryQueueTable_ParseXlsRows`, les arguments lus dans l'ordre par
`Lua_AddEntryQueue`, et les noms de clés du `.lua` livré. Les offsets écrits par
`Lua_AddEntryQueue` sont identiques à ceux écrits par le parseur `.xls`.

| col | offset | type | nom Lua | lu par |
|---:|---|---|---|---|
| 1 | `+0x004` | `char[0x104]` | `BattleFieldName` | 🔴 **le nom envoyé au serveur** (CZ 0x08D7/0x08DA/0x08E0) |
| 2 | `+0x108` | `char[0x104]` | `DisplayBattleFieldName` | affichage + lignes de chat |
| 3 | `+0x20C` | `u32` | `AllianceNum` | plafond d'effectif — §8.3 |
| 4 | `+0x210` | `u32` | `EnemyNum` | — |
| 5 | `+0x214` | `u8` | `PrivateApply` | non consulté par `OnMsg` |
| 6 | `+0x215` | `u8` | `PartyApply` | non consulté par `OnMsg` |
| 7 | `+0x216` | `u8` | `GuildApply` | non consulté par `OnMsg` |
| 8 | `+0x217` | `u8` | `JobGroup` | — |
| 9 | `+0x218` | `u8` | `EnterCondType` | 1 = niveau max, 2 = niveau min, 3 = fourchette, autre = sans limite |
| 10 | `+0x21C` | `u32` | `EnterCondValue1` | l'argument `%d` du message de condition |
| 11 | `+0x220` | `u32` | `EnterCondValue2` | — |
| 12 | `+0x224` | `char[0x104]` | `RewardWin` | panneau de détail |
| 13 | `+0x328` | `char[0x104]` | `RewardDraw` | panneau de détail |
| 14 | `+0x42C` | `char[0x104]` | `RewardLose` | panneau de détail |
| 15 | `+0x530` | `char[0x104]` | `VictoryCond` | panneau de détail |
| 16 | `+0x634` | `char[0x104]` | `MiniMapFile` | (dessiné ailleurs qu'en `sub_B3FB50`) |

`0x634 + 0x104 = 0x738` — le compte est juste, il n'y a pas de champ caché.

🔑 **Colonne 1 ≠ colonne 2.** Le client affiche `DisplayBattleFieldName` mais
**envoie `BattleFieldName`**. Sur Moonlight les deux sont égales, donc le piège
ne se voit pas — il se verrait le jour où quelqu'un traduirait la colonne
d'affichage en la confondant avec la clé.

### Le panneau de détail — `sub_B3FB50` 0x00b3fb50

Appelé sur sélection d'une ligne (`UIEntryQueueWnd::OnMsg` message **135**).
**Première chose qu'il fait : `*(mgr+0x34) = entrée`.** Puis il remplit sept
libellés : `DisplayBattleFieldName`, `VictoryCond`, `AllianceNum` (via msg
0x866), la condition d'entrée (msg 2151 / 2152 / 2153 / 2154 selon
`EnterCondType`), puis `RewardWin`, `RewardDraw`, `RewardLose`.

## 4. Les quatre fenêtres

| id | hex | classe | ctor | OnCreate | OnMsg |
|---:|---|---|---|---|---|
| 157 | 0x9D | `UIEntryQueueWnd` — la liste des terrains | 0x00b3e2b0 | 0x00b3e6d0 | 0x00b3f700 |
| 209 | 0xD1 | `UIEntryQueueStandByWnd` — « en attente » | 0x00b3d810 | — | 0x00b3e150 |
| 210 | 0xD2 | `UIEntryQueueRequestWnd` — « le terrain est prêt, entrer ? » | 0x00b3ca50 | — | 0x00b3d750 |
| 211 | 0xD3 | `UIEntryQueueHelpWnd` — l'aide | 0x00b3bbd0 | 0x00b3bd00 | 0x00b3c560 |

Plus un contrôle enfant : `UIEntryQueueListBox` (RTTI @0x0124D620), pas une
fenêtre de premier plan.

157 et 209 naissent par la **fabrique Lua** `UIWindowMgr_MakeWindowFromLuaInfo`
(cf. [native_window_dispatch.md](native_window_dispatch.md) §5) ; 210 et 211 par
le switch principal.

### Ce que font les boutons

**`UIEntryQueueWnd::OnMsg`, message 6 (bouton) :**

| bouton | action | garde préalable |
|---:|---|---|
| 0 | s'inscrire **seul** → CZ 0x08D7 type **1** | carte interdite (msg 2155), délai d'une minute (msg 2156), confirmation msg 0x862 |
| 1 | s'inscrire en **groupe** → CZ 0x08D7 type **2** | + membre d'un groupe (2157), en être le chef (2158), `AllianceNum ≥ taille du groupe` (2159) ; confirmation 0x863 |
| 2 | s'inscrire en **guilde** → CZ 0x08D7 type **4** | + avoir une guilde (2160), en être le maître (2161), `AllianceNum ≥ capacité de la guilde` (2162) ; confirmation 0x864 |
| 3 | ouvrir l'aide (fenêtre 211) | — |
| 4, 201 | fermer | — |

**`UIEntryQueueStandByWnd::OnMsg`, message 6 :** bouton 0 = annuler
(`sub_B3A360` → CZ 0x08DA) ; bouton 1 = **fermer la fenêtre seulement**, sans
annuler la file.

**`UIEntryQueueRequestWnd::OnMsg`, message 6 :** bouton 0 = accepter
(`sub_B3A240(1)` → CZ 0x08E0 résultat 1) ; bouton 1 = refuser
(`sub_B3A240(2)`, qui remet aussi `mgr+0x38` à 0). Les deux ferment 209 **et**
210 ; le refus écrit en plus la ligne de chat msg 0x854.

### 🔴 Qui ouvre la fenêtre 157 ?

**Personne, par `MakeWindow`.** Les 28 sites `push 9Dh` du binaire ont été
ouverts un par un : les deux seuls qui visent le gestionnaire de fenêtres
(`sub_D9DAD0` 0x00d9defa, `sub_DA4D60` 0x00da4fc4) font un `FindWindow` +
`__RTDynamicCast` — ils **retrouvent** une fenêtre déjà ouverte pour lui pousser
un message, ils ne la créent pas.

🔴🔴 **CORRECTION du 2026-09-01.** La première rédaction concluait : « le seul
ouvreur est `Chat_HandleChatMessage`, dont le cas 97 appelle `vtable+0x18` du
mode de jeu avec l'id 157 ». **C'était faux, et l'erreur mérite d'être nommée :
j'ai lu un `push 9Dh` et supposé que 0x9D était un id de FENÊTRE parce qu'il
valait le numéro de la fenêtre que je cherchais.**

`CGameMode::vtable+0x18` (= **`sub_C86740`** 0x00c86740, vtable **0x010904B8**)
n'est pas un ouvreur de fenêtres : c'est un **répartiteur de commandes**, avec
son propre espace d'identifiants **1..335** (`cmp eax, 14Eh`, sauts
`jpt_C867B4` @0x00c930f0). Vérifié en suivant la table : la **commande 157**
envoie le paquet **`0x0842`** — elle n'a aucun rapport avec `UIEntryQueueWnd`.
Les deux espaces de numéros se recouvrent, voilà tout.

➡ ~~**L'ouvreur de la fenêtre 157 reste INCONNU.**~~

## ✅✅ TROUVÉ le 2026-09-02 — l'icône « battle » de la barre de menu

Le constat « aucun site du binaire ne passe l'id 157 en immédiat à `MakeWindow` »
était **exact, et c'est précisément pourquoi la recherche ne pouvait pas
aboutir : l'ouvreur n'existe pas dans le binaire qu'IDA a ouvert.** Il est
**injecté par WARP** dans l'exe livré, par le patch
`WARP0716\Scripts\Patches\RestoreBattlegroundUI.qjs`. Relevé complet :
[warp_patch_map.md](warp_patch_map.md) §4.

Le patch fait **deux** choses, mesurées en comparant l'exe livré au vanilla :

1. **Il rend l'icône visible.** L'icône `battle` (commande **376 = `0x178`**, 8ᵉ des
   25 icônes) est **sautée en dur** par `UIMenuIconWnd_BuildIconList` dans le
   vanilla ; WARP repatche la table de visibilité `0x00814064` (`00` → `01`) pour
   qu'elle soit créée. Cf. [basic_info_re.md](basic_info_re.md) §2.
2. **Il branche son clic.** Le `call UIWindow_OnMsg` de la branche `default` de
   `UIMenuIconWnd_OnMsg` (`0x00814ADF`, 3 octets) est détourné vers un stub de la
   section `.xdiff` :

```
cmp  dword [ebp+10h], 178h        ; commande de l'icône « battle » ?
jnz  suite
push ecx ; mov ecx,[015C43E4h]    ; g_EntryQueueMgr
cmp  byte [ecx+3Ch], 1 ; jnz +5
call 007397D0                     ; 🔴 cible FAUSSE, voir ci-dessous
pop  ecx
push 9Dh                          ; 157 = UIEntryQueueWnd
call 00812E60                     ; UIWindowMgr_ToggleWindowById
suite:
jmp  00A24D70                     ; UIWindow_OnMsg (comportement d'origine)
```

`WID_ENTRYQUEUEWND = 0x9D` est écrit tel quel dans le script WARP, et
`0x015C43E4` porte déjà le nom `g_EntryQueueMgr` dans l'IDB — les deux bouts
concordent.

### 🔴🔴 Le patch porte un défaut : une adresse physique prise pour virtuelle

Le `call` intermédiaire vise `0x007397D0`, qui est le **milieu d'une instruction**.
La fonction réellement visée (`CEntryQueueMgr::Cz_Req_Entry_Queue_Ranking`, que WARP
localise par `mov r32, 90Ah`) commence à **`0x00B3A3D0`**. L'écart vaut exactement
`0x400C00`, le décalage `.text` entre adresse virtuelle et offset fichier :
le script utilise le retour de `Exe.FindHex` (**physique**) sans le convertir, là où
les deux autres cibles du stub viennent de `Exe.GetTgtAddr` (**virtuelles**) et sont
justes. Correctif : `const AddrSendEntryPacket = Exe.Phy2Vir(Addr, CODE);`.

Le `call` fautif est **gardé** par `cmp byte [g_EntryQueueMgr+0x3C], 1` : il ne part
que si cet octet vaut 1 au clic. ⚠ **Non vérifié en jeu** — un clic sur l'icône
suffit à trancher.

⚠ **Leçon de méthode.** Une recherche exhaustive et correcte dans l'IDB a rendu
« aucun ouvreur » pendant deux relevés successifs. Le manquant n'était pas la
recherche, c'était la **prémisse** : l'IDB est l'exe vanilla. Avant de conclure
« le binaire ne fait pas X » sur une fonction d'interface, vérifier qu'elle est
**intacte** dans l'exe livré (`tools/warp/diff_warp_patches.py`).

⚠ Même piège à surveiller ailleurs : un id poussé avant `vtable+0x18` est une
**commande**, un id poussé avant `UIWindowMgr_MakeWindow` est une **fenêtre**.
Ne jamais déduire l'un de l'autre. Cf. [[feedback_absence_needs_measurement]]
(« un slot de vtable ne nomme pas un geste »).

## 5. Les onze opcodes

Tous présents dans la table de longueurs du client 20250716.
⚠ Les lignes de `docs/opcode_map.md` écrivent l'hexa **en majuscules**
(`0x08DB`) : un `grep 0x08db` rend vide et ne prouve rien.

| opcode | dir | len | nom rAthena | handler client | ce qu'il fait |
|---|---|---:|---|---|---|
| 0x08D7 | CZ | 28 | `CZ_REQ_ENTRY_QUEUE_APPLY` | `sub_B3A2F0` 0x00b3a2f0 | `<type>.W <BattleFieldName>.24B`, type 1/2/4 |
| 0x08D8 | ZC | 27 | `ZC_ACK_ENTRY_QUEUE_APPLY` | `sub_B3A5D0` 0x00b3a5d0 | résultat en `+2` ; §6 |
| 0x08D9 | ZC | 30 | `ZC_NOTIFY_ENTRY_QUEUE_APPLY` | `sub_B3AB80` 0x00b3ab80 | `<nom>.24B <numéro de file>.L` ; retrouve l'entrée **par le nom**, ouvre 209, ferme 157 |
| 0x08DA | CZ | 26 | `CZ_REQ_ENTRY_QUEUE_CANCEL` | `sub_B3A360` 0x00b3a360 | ✅ **gardé** : ne part que si `mgr+0x34 != 0` |
| 0x08DB | ZC | 27 | `ZC_ACK_ENTRY_QUEUE_CANCEL` | `sub_B3A920` 0x00b3a920 | résultat en `+2` ; 🔴 §8.1 et §8.2 |
| 0x08DC | ZC | 26 | *(non implémenté serveur)* | **`nullsub_25`** | le client l'accepte et ne fait rien |
| 0x08DD | CZ | 27 | *(non implémenté serveur)* | — | jamais émis |
| 0x08DE | ZC | 27 | *(non implémenté serveur)* | `sub_B3B100` 0x00b3b100 | boîte 0x83F si résultat 3, 0x840 si 11 |
| 0x08DF | ZC | 50 | `ZC_NOTIFY_LOBBY_ADMISSION` | `sub_B3AF20` 0x00b3af20 | `<nom>.24B <lobby>.24B` ; ouvre 210, joue `se_btg_ready.wav` ; 🔴 §8.1 |
| 0x08E0 | CZ | 51 | `CZ_REPLY_LOBBY_ADMISSION` | `sub_B3A240` 0x00b3a240 | `<résultat>.B <nom>.24B <lobby>.24B` ; ✅ gardé |
| 0x08E1 | ZC | 51 | `ZC_REPLY_ACK_LOBBY_ADMISSION` | `sub_B3B160` 0x00b3b160 | joue `se_btg_forward.wav` si résultat 1 ; rien sinon |
| 0x090A | CZ | 26 | `CZ_REQ_ENTRY_QUEUE_RANKING` | — | demande la position en file |
| 0x090E | ZC | 2 | `ZC_ENTRY_QUEUE_INIT` | `sub_B3AB60` 0x00b3ab60 | 🔴 **remet `mgr+0x34` et `mgr+0x38` à 0** — deux lignes, rien d'autre |

Sons joués : `se_btg_request.wav` (inscription acceptée), `se_btg_ready.wav`
(terrain prêt), `se_btg_forward.wav` (quelqu'un a accepté).

## 6. Les codes de résultat, client contre serveur

### `0x08D8` — inscription : **correspondance parfaite**

Le `switch` du client et l'énum `e_bg_queue_apply_ack` de rAthena
(`src/map/battleground.hpp:92`) coïncident un pour un, ce qui confirme
l'interprétation des deux côtés :

| code | rAthena | msgstring client | clé |
|---:|---|---|---|
| 1 | `BG_APPLY_ACCEPT` | 0x833 (2099) | `MSI_BATTLEFIELD_MSG_REQUEST_JOINWAIT` |
| 2 | `BG_APPLY_QUEUE_FINISHED` | 0x834 (2100) | `..._MSG_FULL` |
| 3 | `BG_APPLY_INVALID_NAME` | 0x835 (2101) | `..._MSG_UNKNOWN_NAME` |
| 4 | `BG_APPLY_INVALID_APP` | 0x836 (2102) | `..._MSG_UNKNOWN_TYPE` |
| 5 | `BG_APPLY_PLAYER_COUNT` | 0x837 (2103) | `..._MSG_MAXOVER` |
| 6 | `BG_APPLY_PLAYER_LEVEL` | 0x838 (2104) | `..._MSG_JOIN_NOTLEVEL` |
| 7 | `BG_APPLY_DUPLICATE` | 0x839 (2105) | `..._MSG_JOIN_OVERLAP` |
| 8 | `BG_APPLY_RECONNECT` | 0x83A (2106) | `..._MSG_RESTART` |
| 9 | `BG_APPLY_PARTYGUILD_LEADER` | 0x83C (2108) | `..._MSG_JOIN_ONLYBOSS` |
| 10 | `BG_APPLY_PLAYER_CLASS` | 0x83B (2107) | `..._MSG_NOTJOB` |
| 15 | *(jamais émis)* | 0x83D (2109) | `..._MSG_BUSY_PARTYMEMBER` |

Seul le cas **1** ouvre la fenêtre 209 et écrit la ligne de chat.

### `0x08DB` — annulation : **les espaces de valeurs ne coïncident pas**

Le client attend quatre codes ; rAthena en envoie deux.

| code attendu par le client | msgstring | effet côté client |
|---:|---|---|
| 1 | 0x83E (2110) `..._MSG_CANCEL_JOINWAIT` | boîte de dialogue **seulement** |
| 3 | 0x83F (2111) `..._MSG_WRONG_NAME` | boîte seulement |
| 11 | 0x840 (2112) `..._MSG_NOTRANK` | boîte seulement |
| **14** | 0x841 (2113) `..._MSG_FAIL_CHOICE` | boîte **+ ferme la fenêtre 209 + `mgr+0x38 = 0`** |
| *tout autre* | — | **rien du tout** |

`clif_bg_queue_cancel_result` (`src/map/clif.cpp:24242`) écrit
`WFIFOB(fd,2) = success`, c'est-à-dire **0 ou 1**. Conséquences :

- annulation réussie → **1** → le joueur voit bien « file d'attente annulée »,
  mais la fenêtre 209 **reste ouverte** et `mgr+0x38` reste à 1 ;
- annulation refusée → **0** → aucun message, aucune trace.

Le seul code qui referme proprement l'interface, **14**, n'est émis par aucune
ligne du serveur.

## 7. Ce que Moonlight sert, et ce que le client attend

`db/battleground_db.yml` contre `moonlight.grf!data\luafiles514\lua files\entryqueue\entryqueuelist.lua` :

| serveur `Name:` | Lua `BattleFieldName` | `MinPlayers` / `AllianceNum` | `MinLevel` / `EnterCondValue1` |
|---|---|---|---|
| `Tierra Gorge` | `Tierra Gorge` | 3 / 3 | 80 / 80 |
| `Flavius` | `Flavius` | 3 / 3 | 80 / 80 |
| `KVM` | `KVM` | 2 / 2 | 80 / 80 |

Les clés se correspondent exactement : `bg_search_name()` retrouvera le terrain.
Le `.lua` est **déjà traduit en français** (« Une Reserve est detruite. ») —
⚠ mais **sans accents**, alors que `data\` est en CP949 et accepte les accents
(cf. [[reference_data_folder_cp949_encoding]]).

## 8. 🔴 Les trois défauts

### 8.1 Deux déréférencements de `mgr+0x34` sans garde

`sub_B3A920` (ZC 0x08DB) et `sub_B3AF20` (ZC 0x08DF) construisent tous deux une
ligne de chat en lisant `*(mgr+0x34) + 0x108` puis en faisant un `strlen`
dessus. Le désassemblage ne porte **aucun test** :

```
00B3AA51  mov     edx, [esi+34h]
00B3AA54  add     edx, 108h
00B3AA5A  mov     ecx, edx
00B3AA5C  lea     edi, [ecx+1]      ; puis balayage de chaîne
```

Si `mgr+0x34` vaut 0 — et **`ZC_ENTRY_QUEUE_INIT` 0x090E le met à 0 à chaque
entrée sur un terrain**, tout comme le handler d'annulation lui-même — le client
balaie la mémoire depuis l'adresse `0x108` : violation d'accès.

Ce n'est pas une hypothèse de bureau : rAthena porte déjà un contournement
commenté dans `clif_parse_bg_queue_cancel_request` —
*« Make the cancel button do nothing if the entry window is open. Otherwise it'll
crash the game when you click on both the queue status and entry status
window. »* Le présent relevé donne **l'instruction exacte** qui plante, et
montre que le contournement serveur ne couvre qu'un des deux chemins : rien ne
protège **0x08DF**.

À noter, par contraste : les trois émetteurs CZ (`sub_B3A2F0` implicitement,
`sub_B3A360` et `sub_B3A240` explicitement) **testent** `mgr+0x34` avant de
l'utiliser. L'oubli est du côté réception uniquement.

### 8.2 L'annulation ne referme jamais l'interface

Conséquence directe du §6 : sur Moonlight, annuler sa file affiche le bon
message mais laisse la fenêtre « en attente » à l'écran et le drapeau interne à
1. Le joueur croit être encore en file ; le serveur sait que non.

### 8.3 L'inscription en guilde est impossible en l'état

`UIEntryQueueWnd::OnMsg` bouton 2 exige
`AllianceNum >= g_GuildInfo_MemberMax`. `AllianceNum` vaut 2 ou 3 sur les trois
terrains de Moonlight, la capacité d'une guilde en vaut plusieurs dizaines : la
condition est **toujours fausse**, et le bouton rend systématiquement le message
2162 `MSI_BATTLEFIELD_MSG_OVER_GUILDMEMBER`.

Le levier est côté **client**, dans le `.lua` : c'est `AllianceNum` qui borne, et
il sert aussi de plafond à l'inscription en groupe (un groupe de 4 est refusé sur
Tierra Gorge). Rien à changer côté serveur.

## 9. Ce qui reste à faire

- La table **nom de commande → id 0..280** de `Chat_HandleChatMessage` : c'est
  elle qui dit comment un joueur ouvre la fenêtre 157.
- `UIEntryQueueWnd_OnCreate` (0xe28 octets) : y vérifier si `PrivateApply` /
  `PartyApply` / `GuildApply` (colonnes 5-7) pilotent la visibilité des trois
  boutons — `OnMsg` ne les consulte pas.
- Les opcodes **0x08DC / 0x08DD / 0x08DE** : présents dans le client, absents de
  rAthena. Trois codes de résultat connus (3 et 11 sur 0x08DE) restent sans
  émetteur.
