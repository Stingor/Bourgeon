# Storage — RE des paquets et mort de la fenêtre native

Client 20250716 (base 0x400000), serveur moonlight (PACKETVER_RE 20250716).
Complète `project_storage_window_re` (mémoire), qui décrit la **fenêtre** ;
celui-ci décrit les **paquets** et ce qu'il fallait savoir pour la supprimer.

## 1. La fenêtre

`UIItemStoreWnd`, window id **0x21**, vtable **0x0103ca40**. Elle rend le storage
Kafra, le storage de guilde *et* le premium : le client les traite par un
framework d'inventaire générique indexé par un `invType`
(`INVENTORY=0, CART=1, STORAGE=2, GUILD_STORAGE=3`).

## 2. Qui crée la fenêtre — sept cases, pas une

`RecvLoop_DispatchPackets` (0x00c9df00) ; table à **0x00caa2e0**, indexée
`(opcode - 0x73) * 4`. Les cases qui appellent `MakeWindow(mgr, 0x21)` :

| case | opcode | rôle | envoyé par moonlight ? |
|---|---|---|---|
| 2824 | **0x0b08** | `ZC_INVENTORY_START` | **oui** |
| 242 | **0x00f2** | `ZC_NOTIFY_STOREITEM_COUNTINFO` | **oui** |
| 165 | 0x00a5 | liste storage (PACKETVER < 20071002) | non |
| 166 | 0x00a6 | liste equip (idem) | non |
| 496 | 0x01f0 | liste storage | non |
| 746 | 0x02ea | liste storage (≥ 20080102) | non |
| 2308 / 2309 | 0x0904 / 0x0905 | listes storage | non |

Le serveur choisit ses opcodes de liste dans `packets_struct.hpp` : pour ce
packetver, `storageListNormalType = 0xb09` et `storageListEquipType = 0xb39`.
Les cinq legacy sont donc morts **sur ce serveur** — mais ils restent dans le
dispatcher, d'où le filet de `StorageWindow::OnTick` qui détruit toute fenêtre
0x21 trouvée.

### 0x0b08 est MULTIPLEXÉ

`GameMode_OnRecv_ZC_INVENTORY_START` @0x00cd8d10 :

```
switch (invType) {
  case 0: ouvre l'inventaire (+ vide les slots equip/costume)
  case 1: ouvre le cart
  case 2:
  case 3: MakeWindow(mgr, 0x21) ; si name != "" -> UIItemStoreWnd_SetTitleFromServer
}
```

Revendiquer l'opcode entier tuerait l'ouverture de l'inventaire et du cart. Le
prédicat de `RegisterReplaceOpcode` doit donc lire l'`invType` — d'où la
surcharge `std::function<bool(const uint8_t*, uint16_t)>` ajoutée à l'API.

Format (PACKETVER_RE ≥ 20180919) : `[op:2][packetLength:2][invType:1][name:≤24]`,
soit à partir de +2 : `[len:2][invType:1][name]`.

### 0x00f2 est un second créateur

```
push 0x21 ; mov ecx, mgr ; call UIWindowMgr_MakeWindow
movsx ecx, [buf+2]      ; max
mov edx, [eax]          ; <- déréférence le retour SANS test de nullité
movsx ecx, [buf+0]      ; used
push 0x37               ; OnMsg 0x37 (used, max) -> wnd+0x188 / wnd+0x18c
```

Deux conséquences : il faut le remplacer (sinon la fenêtre renaît juste après
l'ouverture), et on ne peut pas se contenter de faire échouer `MakeWindow` —
le `mov edx,[eax]` crasherait.

Format : `[op:2][amount:2][max_amount:2]`, fixe 6 octets
(`clif_updatestorageamount`, `PACKET_ZC_NOTIFY_STOREITEM_COUNTINFO`).

## 3. Ce qui survit sans la fenêtre

- **Le modèle d'items** = `g_session + 0x1718` = **0x015fbad8** (`std::list`).
  Peuplé par `GameMode_IngestItemList` @0x00cd8b00 →
  `ItemModel_AddOrStackStorageItem` @0x00d555c0, qui écrit dans
  `this + 5912` (= +0x1718) **puis** ne rafraîchit la fenêtre que sous
  `if (g_StorageWnd_ptr)`. Sans fenêtre : modèle peuplé, refresh sauté, aucun
  crash. C'est ce test de nullité qui rend tout le remplacement possible.
- **`ZC_INVENTORY_END` (0x0b0b)** → `sub_CD8C90` : même garde
  `if (g_StorageWnd_ptr)`. Inoffensif.

## 4. Les devoirs du handler natif qu'il fallait reprendre

| devoir | qui le faisait | ce qu'on en fait |
|---|---|---|
| créer la fenêtre | 0x0b08 / 0x00f2 | supprimé (c'est le but) |
| poser le titre serveur | 0x0b08 → `UIItemStoreWnd_SetTitleFromServer` | lu du paquet → titre du viewer |
| tenir used/max | 0x00f2 → OnMsg 0x37 → wnd+0x188/+0x18c | lu du paquet |
| **vider le modèle** | **0x00f8** → `sub_D566D0` | **laissé au natif** (observe) |

`0x00f8` (`ZC_CLOSE_STORE`, case 248) :

```
push 0x21 ; call UIWindowMgr_SaveRectAndCloseWindow   ; détruit la fenêtre
mov ecx, g_session ; call sub_D566D0                  ; std::list::erase(begin,end) sur +0x1718
```

Le vidage est indispensable : sans lui, les items du storage précédent
resteraient dans le modèle et se mélangeraient au suivant (perso puis guilde, par
exemple). On garde donc `0x00f8` en **observe** — le handler natif fait le
ménage, la destruction d'une fenêtre absente étant sans effet.

## 5. 🔴 Le warp ferme le storage EN SILENCE

`unit.cpp`, `unit_remove_map_` :

```c
if (sd->state.storage_flag == 1)      storage_storage_quit(sd, 0);
else if (sd->state.storage_flag == 2) storage_guild_storage_quit(sd, 0);
else if (sd->state.storage_flag == 3) storage_premiumStorage_quit(sd);
sd->state.storage_flag = 0; // Force close it when being warped.
```

Or `storage_storage_quit` **ne fait que sauvegarder** — aucun
`clif_storageclose`, donc **aucun 0x00f8**. Le serveur suppose que le client
ferme de lui-même au changement de map. D'où l'observation de `0x0091`
(`ZC_NPCACK_MAPMOVE`) et `0x0092` (`ZC_NPCACK_SERVERMOVE`) : sans elle, le viewer
resterait ouvert indéfiniment sur une session morte.

À l'inverse, `storage_storageclose` (CZ_CloseKafra 0x0193, `@storeall`,
`@clearstorage`…) envoie bien `clif_storageclose`.

## 6. Fermeture côté client

`CZ_CloseKafra` = **0x0193**, fixe 2 octets (juste l'opcode). Non remappé par le
shuffle. Le serveur répond 0x00f8.

## 7. Conséquence pour les modules voisins

L'inventaire et le cart déduisaient « storage ouvert » de
`FindWindow(0x21) != nullptr` (`StorageOpen()` dans `inventory_viewer.cc` et
`cart_viewer.cc`). Ce test rend désormais toujours faux en mode ImGui. Il fallait
le corriger : plusieurs de leurs règles en dépendent, dont le refus serveur de
`inventaire <-> cart` tant qu'un storage est ouvert (`sd->state.storage_flag`,
testé par `pc_putitemtocart` / `pc_getitemfromcart`). Les deux passent maintenant
par `StorageWindow::IsOpen()`, avec repli sur le test natif pour le mode
classique.
