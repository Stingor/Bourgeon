# Fenêtre d'échange joueur-joueur (Trade / « Deal ») — RE

Client `Moonlight-Destiny.exe` (base 0x400000, **pas de rebase**), PACKETVER **20250716**.
Serveur = fork rAthena `moonlight` (`src/map/trade.cpp`, `clif.cpp`).

L'échange entre joueurs est nommé **« Exchange »** dans les classes RTTI et **« Deal »**
dans les textures/strings (`bt_itemDeal_lock_*.bmp`, `MSI_DEAL_*`, `MSI_REQ_DEAL_WITH`).
⚠ Ne pas confondre avec `UIExchangeShopWnd` (= *buying store* / vending, sans rapport).

Objectif : remplacer la fenêtre native par une fenêtre **ImGui** (skin RO), sur le modèle
prouvé de `shop_tweaks` (packet-driven + masquage du natif par vtable). Voir la section finale.

---

## 0. ⚠️ CORRECTION MAJEURE — la fenêtre vivante est `CUIExchangeUI` (RE live 2026-07-23)

La 1ʳᵉ passe (§1–§2 ci-dessous) a été faite sur `UIExchangeWnd` (0x01031edc) — cette classe
est **MORTE** : jamais instanciée dans ce client. La fenêtre d'échange réellement affichée est
la **NOUVELLE** classe **`CUIExchangeUI`** (famille « CUI » high-id, comme `CUIGameSettingsUI`
0x271e). C'est pourquoi le plugin, qui cherchait 0x01031edc, ne détectait jamais rien.

Méthode : walk **live** de la `std::map` du window-mgr (`mgr+8`) → 7 fenêtres ouvertes → résolution
RTTI de chaque valeur (typedesc → COL → nom) → **key `0x271b` = `.?AVCUIExchangeUI@@`**.

| | Classe MORTE (ignorer) | **Classe VIVANTE** |
|---|---|---|
| Classe | UIExchangeWnd | **`CUIExchangeUI`** |
| vtable | 0x01031edc | **0x010457d8** |
| window id | non pinné | **0x271b** (map, pinné) |
| ctor | 0x0088da00 | **0x009cd970** (`CUIExchangeUI_ctor` → `UIWindow_composite_ctor`) |
| OnMsg | 0x008c0ea0 | **0x009cecd0** (`CUIExchangeUI_OnMsg`) |
| rebuild affichage | — | **0x009ce450** (`CUIExchangeUI_RebuildDisplay`) |
| bouton OK (verrou) | — | **0x009ce140** (`CUIExchangeUI_OnOkButton`) |
| saisie zeny | — | **0x009ce940** (`CUIExchangeUI_OnZenyInput`) |

`CUIExchangeUI` est une **sous-classe `UIWindow`** (vtable[1] = `UIWindow_OnDraw_Base`) → masquage
natif `+0x28 = 0` valide, comme les autres fenêtres.

### Struct `CUIExchangeUI` (`this` = `int*`)

| Offset | rôle |
|---|---|
| +0xD8 | label titre |
| +0xDC | widget titre (`"{} Lv{} ({})"`) |
| **+0xE4** | **widget liste MES objets** (10 slots) |
| +0xF4 | UIEdit zeny (moi) |
| **+0xF8** | **widget liste objets PARTENAIRE** (10 slots) |
| +0x114 | bouton « Échanger » (grisé si commit impossible) |
| **+0x11C** | `std::string` (SSO) **nom du partenaire** |
| +0x134 | `std::string` saisie zeny |

**Verrous** : octet **`+0xC4` du widget liste** (`*(this+0xE4)` = moi, `*(this+0xF8)` = partenaire).
`CUIExchangeUI_OnMsg` grise « Échanger » tant que l'un des deux `+0xC4 == 0`.

### Objets du deal — source UI-INDÉPENDANTE (à privilégier)

`CUIExchangeUI_OnMsg` case 0x17 peuple les listes via deux getters lisant la **session**
`g_session = 0x015fa3c0` (mêmes tableaux `ItemSkillInfo` stride `0xF8` que l'équipement à +0x17d0) :

| Getter | source | array (adresse fixe) |
|---|---|---|
| `Session_GetMyDealItem` 0x00d59cd0 | mes objets | **0x015fe250** (= session+0x3e90) + slot·0xF8 |
| `Session_GetPartnerDealItem` 0x00d5cfd0 | objets partenaire | **0x015fec00** (= session+0x4840) + slot·0xF8 |

10 slots chacun. Champs `ItemSkillInfo` (comme `character_sheet`/`inventory_viewer`) :
**quantité +0x10** (< 1 = slot vide), **idStr +0x2c** (SSO, cap +0x40, `atoi`), **refine +0x60**.

### Commandes — dispatcher CONFIRMÉ IDENTIQUE

`CUIExchangeUI_OnOkButton` (vérifié live) :
```
mode = GameMode_GetActive(0x1213338)   // = (*(0x1213338+0x58)==1) ? *(0x121333c) : 0
mode->[+0x18](0x33, 0, DAT_015ff5b0 + zeny_saisi, 0, 0)   // cmd 0x33 index0 = zeny ABSOLU
mode->[+0x18](0x34, 0, 0, 0, 0)                           // cmd 0x34 = verrou/conclude
```
Donc le bus `CMode::SendMsg` (`vf+0x18` = 0x00c86740, **virtualisé** — Ghidra ne le décompile pas)
et les commandes sont **identiques** à l'ancienne classe (mode-level, partagé).

### Les 3 boutons (handlers vérifiés) — délégué : `bouton.delegate.vtable+8` = thunk d'invoke

| Bouton | widget | délégué vtable | handler | action |
|---|---|---|---|---|
| **OK** (verrou) | +0x110 | 0x01045648 | `CUIExchangeUI_OnOkButton` 0x009ce140 | `cmd 0x33(0, DAT+zeny)` **puis** `cmd 0x34` |
| **trade** (commit) | +0x114 | 0x01045664 | `CUIExchangeUI_OnTradeButton` 0x009ce040 | [`cmd 0x44`] puis **`cmd 0x36`** |
| **cancel** | +0x118 | 0x01045680 | `CUIExchangeUI_OnCancelButton` 0x009ce000 | **`cmd 0x35`** |

⚠ Le serveur n'exécute l'échange que quand **LES DEUX** joueurs ont envoyé `cmd 0x36`.

### Modèle du ZENY (piège majeur)

Il n'existe **pas** de « définir le zeny » séparé : le natif applique le zeny **au verrouillage**.
Un `cmd 0x33(index 0, X)` **isolé reste en attente et n'affiche rien** ; c'est le `cmd 0x34` (verrou)
qui le pousse. L'affichage ne bouge que par `Session_SetMyDealZeny` **0x00d771b0** :
```
session+0x51F0 (= DAT_015ff5b0) = zeny ;  puis FindWindow(0x271b)->OnMsg(0x17)  // refresh
```
appelé depuis l'ACK serveur. ⚠ **`DAT_015ff5b0` n'est PAS remis à zéro entre deux échanges** → sans
reset, l'ancien montant réapparaît au verrouillage suivant.

### Ajout d'un OBJET : `cmd 0x33` **puis `cmd 0x12`**

Le drop natif (`CUIExchangeUI_OnMsg` case 0x26) fait :
```
mode->[+0x18](0x33, index, amount, 0, 0)   // ajoute
mode->[+0x18](0x12, 0, 0, 0, 0)            // applique  <-- INDISPENSABLE
```
Sans le `cmd 0x12`, l'objet n'est pas réellement poussé dans le deal (ajout sans effet).

### Case « Screenshot Trade »

Widget **+0x108** (état booléen à **+0xf8**), libellé = `MsgStringTable_GetById(0x727)`.
Si cochée, le bouton trade envoie `cmd 0x44(MsgStringTable(0x728))` avant le `cmd 0x36`.
`MsgStringTable_GetById` = **0x00a9ed30** (`__cdecl(id) -> const char*`).

Le protocole ZC/CZ (§3) et la logique serveur (§5) restent **valides** (niveau paquet, indépendant
de la classe UI). Les §1–§2 ci-dessous décrivent l'ancienne classe MORTE — gardés pour trace.

---

## 1. Classes et vtables (RTTI vérifié en live) — ⚠ ANCIENNE classe MORTE `UIExchangeWnd`

| Classe | rôle | window id | vtable | COL | TypeDescriptor |
|---|---|---|---|---|---|
| **UIExchangeWnd** | fenêtre d'échange (2 grilles + zeny + boutons) | *map-based* (non pinné, cf. §6) | **0x01031edc** | 0x010c340c | 0x0123f508 |
| **UIExchangeAcceptWnd** | popup « X souhaite échanger » (oui/non) | **0x20** | **0x01033754** | 0x010c3d54 | 0x0123f014 |
| **UIYourItemWnd** | popup « vos objets » (variante offre) | — | **0x01033b8c** | 0x010c3ed8 | 0x0123f960 |

Résolution : `TypeDescriptor` (nom `.?AVUIExchangeWnd@@` @ nom−8) → `RTTIBaseClassDescriptor`
→ `ClassHierarchyDescriptor` → `CompleteObjectLocator` → slot **vtable−4**. La vtable et le COL
ne sont **pas** indexés par Ghidra (bytes non typés, région partiellement virtualisée « Lotus ») :
tout résolu en live via `memory_search` du pointeur COL dans la `.rdata`.

### Slots de vtable (famille UIWindow : +0x3c OnCreate, +0x50 DrawContent, +0x94 OnMsg)

| Fenêtre | dtor (+0) | OnCreate (+0x3c) | DrawContent (+0x50) | OnMsg (+0x94) |
|---|---|---|---|---|
| UIExchangeWnd | 0x00892910 | **0x008a6900** | **0x008b36b0** | **0x008c0ea0** |
| UIExchangeAcceptWnd | 0x008928a0 | 0x008a6260¹ | 0x008b3080¹ | **0x008c0810** |
| UIYourItemWnd | 0x00894020 | 0x008a6260¹ | 0x008b3080¹ | **0x008ce010** |

¹ OnCreate/DrawContent partagés (méthodes de base « popup simple ») entre AcceptWnd et YourItemWnd ;
`0x008b3080` est aussi le DrawContent de `UIChooseSellBuyWnd` → générique, **ne pas** hooker par
fonction, hooker par **vtable**. Ctor UIExchangeWnd = **0x0088da00**.

Tout renommé/commenté dans Ghidra (`UIExchangeWnd_*`, `UIExchangeAcceptWnd_OnMsg`, `UIYourItemWnd_OnMsg`,
`Recv_ZC_ACK_EXCHANGE_ITEM`, `vtable_UIExchangeWnd`, …).

---

## 2. Struct `UIExchangeWnd` (`this` = `int*`, offsets confirmés OnCreate/OnMsg/DrawContent/ctor)

| Offset | idx int | type | rôle |
|---|---|---|---|
| +0xB4 | 0x2d | UIBitmapButton* | bouton **id 0xb8 "ok"** (verrouiller / OK) |
| +0xB8 | 0x2e | UIBitmapButton* | bouton **id 0xe6 "exchange"** (valider l'échange) |
| +0xBC | 0x2f | UIBitmapButton* | bouton **id 0xb9 "cancel"** |
| +0xC0 | 0x30 | UIEdit* | saisie zeny (pos 0x91,0x14a ; maxlen `+0x88`=0xd) |
| +0xCC | 0x33 | int | **lock zeny — moi** (posé par msg 0x33) |
| +0xD0 | 0x34 | int | **lock zeny — partenaire** (msg 0x34) |
| +0xD4 | 0x35 | int | drapeau « OK pressé » (verrou envoyé) |
| +0xD8 | 0x36 | int | drapeau « Exchange pressé » (commit envoyé) |
| +0xDC | 0x37 | int | drapeau « Cancel pressé » |
| +0xE0 | 0x38 | std::list | **mes objets offerts** (≤10) — `Inventory_AppendNewItem` |
| +0xE8 | 0x3a | std::list | **objets du partenaire** (≤10) |
| +0xF0 | 0x3c | std::string | nom du partenaire (msg 0x32) |
| +0x108 | 0x42 | uint | niveau/job du partenaire (affiché DrawContent) |
| +0x10C | 0x43 | short | view/complément (msg 0x32) |
| +0x112 | — | short | **hauteur de ligne** = 0x1e (30 px) |
| +0x118 | 0x46 | UIToggleButton* | toggle (option de confirmation) |
| +0x11C | 0x47 | bool | état du toggle (msg 0xd5) |
| +0x124 | 0x49 | std::string | tampon |

**Zeny du deal (globals)** : `DAT_015ff5b0` = zeny que **j'offre**, `DAT_015ff5b4` = zeny **partenaire**
(dessinés en bas à droite par DrawContent). `g_PlayerZeny` = `DAT_015fba90` (plafond de saisie).

**Nœud d'objet** (liste +0xE0/+0xE8, réutilise le layout item du shop) :
`+0x08` id/nom (std::string), `+0x34` id en texte (→ `atoi`), `+0x90`(short) nb slots carte → overlay
`grade_item.bmp`. Rendu ligne : icône `FUN_008711a0(wnd, ctrlId, y, node+2, …)`, nom
`UIItemSkillDescWnd_DrawName(wnd, ctrlId, …)`, ligne `y = row_height * i + 0x15`.
Ctrl-ids de rendu : colonne gauche (moi) icône **0x10** / nom **0x30** ; colonne droite (partenaire)
icône **0x127** / nom **0x147**.

---

## 3. Commandes UI → paquets (le client ne fait qu'émettre ; le serveur valide tout)

### Commandes de **mode** — `(*(*g_pCurrentMode+0x18))(cmd, …)` = `CMode::SendMsg` (0x00c86740)

| cmd | déclencheur (UI) | paquet CZ émis |
|---|---|---|
| *(à confirmer)* | menu contextuel joueur → « Deal » (hors ces fenêtres) | **CZ_REQ_EXCHANGE_ITEM 0x00e4** `{u16; u32 AID}` |
| **0x32** (arg 3/4) | AcceptWnd : bouton 0xb8 accepter / 0xb9 refuser | **CZ_ACK_EXCHANGE_ITEM 0x00e6** `{u16; u8 result}` (3=accept, 4=reject) |
| **0x33** (index, amount) | ajout objet (drag, msg 0x26) ou ajout zeny (index=0) | **CZ_ADD_EXCHANGE_ITEM 0x00e8** `{u16; u16 index; i32 amount}` |
| **0x34** | bouton 0xb8 « OK » (après avoir poussé le zeny) | **CZ_CONCLUDE_EXCHANGE_ITEM 0x00eb** `{u16}` (verrou) |
| **0x35** | bouton 0xb9 « Cancel » | **CZ_CANCEL_EXCHANGE_ITEM 0x00ed** `{u16}` |
| **0x36** | bouton 0xe6 « Exchange » (commit final) | **CZ_EXEC_EXCHANGE_ITEM 0x00ef** `{u16}` |

> Le zeny est envoyé en **valeur absolue** (`index=0`, `amount` = total offert, clampé à `g_PlayerZeny`).
> Le bouton « OK » (0xb8) pousse d'abord le zeny en attente (cmd 0x33) **puis** verrouille (cmd 0x34).

### Messages de **fenêtre** — `(*(*wnd+0x94))(0, msgId, …)` = OnMsg (interne, ne pas confondre)

`6`=clic bouton · `0x17`=refresh/relayout (reconstruit les 2 listes depuis `g_UIWindowContextKey`,
10 slots) · `0x26`=drag-add item · `0x32`=set item/partenaire courant · `0x33`/`0x34`=poser lock
zeny moi/partenaire · `0x7b`=tag-stream persistance/replay (base **0x5078**, sous-tags
0x507d→+0xcc, 0x507e→+0xd0, 0x507f→+0xd4, 0x5080→+0xd8, 0x5081→+0xdc).

---

## 4. Protocole complet (PACKETVER 20250716 = MAIN ≥ 20200916)

### Serveur → client (ZC)

| Opcode | nom | taille | struct |
|---|---|---|---|
| **0x01f4** | ZC_REQ_EXCHANGE_ITEM | 32 | `{u16; char name[24]; u32 targetId; u16 targetLv}` → ouvre **UIExchangeAcceptWnd (id 0x20)** |
| **0x01f5** | ZC_ACK_EXCHANGE_ITEM | 9 | `{u16; u8 result; u32 targetId; u16 targetLv}` — result 0=trop loin,1=inexistant,2=échec,**3=accept**,4=cancel,5=busy |
| **0x0b42** | ZC_ADD_EXCHANGE_ITEM | 54 | `{u16; u32 itemId; u8 itemType; i32 amount; u8 identified; u8 damaged; EQUIPSLOTINFO slot(8); ItemOptions opt[5](25); u32 location; u16 look; u8 refine; u8 grade}` (index=0 ⇒ zeny) |
| **0x00ea** | ZC_ACK_ADD_EXCHANGE_ITEM | 5 | `{u16; u16 index; u8 result}` — 0=ok,1=surpoids,2=annulé,3=inv. plein,4=stack dépassé |
| **0x00ec** | ZC_CONCLUDE_EXCHANGE_ITEM | 3 | `{u16; u8 who}` — 0=soi verrouillé, 1=l'autre verrouillé |
| **0x00ee** | ZC_CANCEL_EXCHANGE_ITEM | 2 | `{u16}` |
| **0x00f0** | ZC_EXEC_EXCHANGE_ITEM | 3 | `{u16; u8 result}` — 0=succès,1=échec (ferme la fenêtre) |
| **0x00f1** | ZC_EXCHANGEITEM_UNDO | 2 | `{u16}` (reset visuel) |

### Client → serveur (CZ)

| Opcode | nom | taille | struct | parseur serveur |
|---|---|---|---|---|
| **0x00e4** | CZ_REQ_EXCHANGE_ITEM | 6 | `{u16; u32 AID}` | `clif_parse_TradeRequest` → `trade_traderequest` |
| **0x00e6** | CZ_ACK_EXCHANGE_ITEM | 3 | `{u16; u8 result}` (3/4) | `clif_parse_TradeAck` → `trade_tradeack` |
| **0x00e8** | CZ_ADD_EXCHANGE_ITEM | 8 | `{u16; u16 index; i32 amount}` (index 0=zeny) | `clif_parse_TradeAddItem` → `trade_tradeadd{item,zeny}` |
| **0x00eb** | CZ_CONCLUDE_EXCHANGE_ITEM | 2 | `{u16}` | `clif_parse_TradeOk` → `trade_tradeok` |
| **0x00ed** | CZ_CANCEL_EXCHANGE_ITEM | 2 | `{u16}` | `clif_parse_TradeCancel` → `trade_tradecancel` |
| **0x00ef** | CZ_EXEC_EXCHANGE_ITEM | 2 | `{u16}` | `clif_parse_TradeCommit` → `trade_tradecommit` |

`index` client = `server_index(index)` (les objets du deal envoient `inv_index + 2`).

---

## 5. Machine à états serveur (`trade.cpp`) — garanties

1. **Requête** `trade_traderequest` : refuse si NOTRADE map, cible NPC/en deal/en vente/storage, GM
   non autorisé, hors distance (≤ `TRADE_DISTANCE`=2, sauf @trade). Pose `trade_partner` des deux côtés,
   envoie ZC_REQ à la cible.
2. **Réponse** `trade_tradeack(type)` : type 4=annule, 3=accepte. Sur accept, `state.trading=1` des deux
   côtés + `memset(deal)`, renvoie ZC_ACK(3) aux deux.
3. **Ajout** `trade_tradeadditem` / `trade_tradeaddzeny` : bloqué si `deal_locked>0`. Vérifie NOTRADE
   item, bound, rental, œuf éclos, equipSwitch, surpoids/place chez la cible, ≤10 positions. `deal.zeny`
   = valeur absolue clampée. Renvoie ZC_ACK_ADD (résultat) + ZC_ADD à l'autre.
4. **Verrou** `trade_tradeok` : `deal_locked=1`, ZC_CONCLUDE(who=0 à soi, who=1 à l'autre).
5. **Commit** `trade_tradecommit` : nécessite les **deux** verrouillés (`deal_locked==2`). Re-check
   anti-triche (`impossible_trade_check`, `trade_check` = place/poids/MAX_ZENY/MAX_AMOUNT), transfère
   objets+zeny (`pc_additem`/`pc_delitem`/`pc_payzeny`/`pc_getzeny`, `LOG_TYPE_TRADE`), reset, sauve
   les deux persos. ZC_EXEC(0) aux deux.
6. **Annulation** `trade_tradecancel` : rend virtuellement les objets, reset, ZC_CANCEL aux deux.

**Le serveur est autoritaire** : le client ImGui n'a qu'à émettre `{index, amount}` / verrou / commit /
annule ; aucun exploit possible, chaque échec revient en code retour (ZC_ACK_ADD / ZC_EXEC result).

---

## 6. Fenêtre principale : id « map-based », non pinné statiquement

`UIWindowMgr_FindWindow` (0x00a47b90) **n'a pas** de slot dédié pour l'échange → la fenêtre vit dans la
`std::map` à `g_UIWindowMgr(0x0131f4e8)+8` (clé = id, nœud MSVC `{L,P,R,color/isnil, key@+0x10, val@+0x14}`).
La table `MakeWindow` (jump `0x00a42904` / remap `0x00a42ca8`) est **détournée par le hook Bourgeon** →
Ghidra ne décompile pas la table et le ctor n'a pas d'xref ; l'id numérique de `UIExchangeWnd` n'a donc
pas pu être figé statiquement (au moment du RE la fenêtre n'était ouverte que dans l'autre instance
dual-box). **La popup de requête, elle, est confirmée : `UIExchangeAcceptWnd` = id 0x20** (handler recv
ZC_REQ 0x01f4 : `push 0x20 ; mov ecx, mgr ; call 0x00a39340`, et `SaveWindowRect(0x20)`).

→ Pour le plugin, **masquer par vtable** (comme `shop_tweaks` pour `UIItemParamChangeDisplayWnd`), pas
par id : intercepter `MakeWindow`, tester `*obj == 0x01031edc / 0x01033754 / 0x01033b8c`, masquer le
natif (offset visibilité +0x28, jamais hors-écran). L'id map réel se lit au runtime (clé du nœud dont
`val[0]==0x01031edc`) si un `CloseWnd(id)` explicite devient nécessaire.

Dispatch recv : `RecvLoop_DispatchPackets` (0x00c9df00) → `handler = *(0x00caa2e0 + (opcode−0x73)*4)`.
Handlers trade (thunks non définis dans Ghidra) : ZC_REQ 0x01f4→0x00ca1c4e, ZC_ACK 0x01f5→stub→
`Recv_ZC_ACK_EXCHANGE_ITEM` (0x00cb19f0, switch sur `result` via `0x00cb1be4[result]`), ADD 0x0b42→0x00ca9ce3.

---

## 7. Plan de remplacement ImGui (à coder — `trade_tweaks`, calqué sur `shop_tweaks`)

**Principe** : *packet-driven* + masquage natif par vtable. Défaut **OFF** (opt-in), setting `trade_imgui`.

- **Observer (jamais `RegisterRecvOpcode`)** : 0x01f4 (requête → nom+lvl), 0x01f5 (accept/refus →
  ouvrir/fermer), 0x0b42 (ajout objet/zeny partenaire → parse itemId/type/amount/slots/refine/grade),
  0x00ea (ack ajout → toasts d'erreur), 0x00ec (verrou moi/autre), 0x00ee (annulé → fermer),
  0x00f0 (résultat final → toast + fermer). Le natif reste maître quand OFF.
- **Émettre depuis OnTick/OnRenderUI (thread principal, jamais depuis OnRecvPacket)** via `CMode::SendMsg` :
  cmd 0x32(3/4) accept/refus, cmd 0x33(index,amount) ajout objet+zeny, cmd 0x34 verrou, cmd 0x35 annule,
  cmd 0x36 commit. (Ou construire les CZ 0x00e6/0x00e8/0x00eb/0x00ed/0x00ef directement.)
- **Masquer le natif par vtable** (0x01031edc / 0x01033754 / 0x01033b8c) dans le hook `MakeWindow`
  (masquage natif +0x28, cf. [[feedback_no_offscreen_hide]]).
- **UI** : 2 grilles d'icônes (mes objets / partenaire) réutilisant le résolveur nom/icône/slots du shop
  (nœud +0x08/+0x34/+0x90), zone zeny (moi `DAT_015ff5b0` / partenaire `DAT_015ff5b4`), champ zeny +
  boutons **Verrouiller (OK)** / **Échanger** / **Annuler**, voyants « verrouillé » (moi `+0xcc`,
  partenaire `+0xd0`). Drag-drop depuis l'inventaire ImGui → cmd 0x33.
- **Popup de requête** (0x01f4) : petite fenêtre ImGui « *Untel* (Lv N) souhaite échanger » +
  Accepter/Refuser → cmd 0x32(3/4).
- **Sécurité threads** : lectures mémoire OK en running ; ne jamais `pause_process` (déco). Fermeture
  propre = émettre cmd 0x35 (annule) si l'utilisateur ferme en cours, sinon état dialogue client bloqué.

**Réutilisables** : toolkit `ro_imgui` (BeginRoWindow/RoButton/RoCheckbox), résolveur item du shop,
cache d'icônes ImGui (`Overlay_DeviceEpoch`), accents FR ([[feedback_french_ui_accents]]).
