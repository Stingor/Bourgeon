# Sertissage de cartes (« card insert ») — RE du popup natif `UIItemCompositionWnd`

Client cible : `Moonlight-Destiny.exe` (base `0x400000`, build 20250716).

Objectif du document : décrire **exhaustivement** le chemin natif qui va du double-clic
sur une carte dans l'inventaire jusqu'à la carte effectivement sertie, afin de pouvoir
remplacer le popup natif par une fenêtre ImGui portée par le plugin `InventoryViewer`.

---

## 1. Vue d'ensemble du flux

```
double-clic carte (inventaire)
        │
        ▼
UIItemWnd_OnDblClk_UseOrEquip  0x00949fc0      (vtable UIItemWnd +0x68)
        │  switch(itemType) → case 6 (CARTE)
        ▼
CMode::SendMsg(0x7B, invIndexCarte)  0x00c86740
        │  bloc case 0x7B @ 0x00c8f556
        │  ├─ mémorise CMode+0x45c = invIndexCarte      ◄── SEULE trace persistante
        │  └─ envoie CZ_REQ_ITEMCOMPOSITION_LIST (0x017A)
        ▼
                         [ SERVEUR : calcule les équipements compatibles ]
        ▼
ZC_ITEMCOMPOSITION_LIST (0x017B)   handler inline @ 0x00ca5ac9
        ├─ MakeWindow(0x4A)                    → UIItemCompositionWnd
        ├─ OnMsg(0x4B)                         → vide la liste
        ├─ OnMsg(0x4D, CMode+0x45c)            → injecte l'index de la carte
        └─ pour chaque index reçu : OnMsg(0x1F, idString, invIndex)
        ▼
    [ l'utilisateur choisit un équipement et valide ]
        ▼
CZ_REQ_ITEMCOMPOSITION (0x017C)  { u16 op, u16 cardIndex, u16 equipIndex }
        ▼
                         [ SERVEUR : applique le sertissage ]
        ▼
ZC_ACK_ITEMCOMPOSITION (0x017D)  Recv_ZC_ACK_ITEMCOMPOSITION_017D  0x00cddd80
        └─ (si porté) ZC_UPDATE_CARDSLOT (0x0A3F)  Recv_ZC_UPDATE_CARDSLOT_0A3F  0x00cf69d0
```

### Le point crucial pour le portage

> **Le client ne décide rien.** Il n'évalue aucune règle de compatibilité (type
> d'équipement, race, élément, emplacements libres). Il envoie l'index de la carte,
> reçoit une liste d'index d'équipement, et l'affiche telle quelle.

Conséquence : une réimplémentation ImGui **ne peut pas** et **ne doit pas** recalculer la
liste des candidats côté client. Elle doit conserver l'aller-retour `0x017A` → `0x017B`
à l'identique. Toute tentative de filtrage local produirait des divergences avec le
serveur (items proposés puis refusés, ou items valides masqués).

---

## 2. Table des paquets

| Opcode | Sens | Taille | Handler | Nom |
|---|---|---|---|---|
| `0x017A` | CZ | fixe 4 | émis inline @ `0x00c8f556` | `CZ_REQ_ITEMCOMPOSITION_LIST` |
| `0x017B` | ZC | variable | inline @ `0x00ca5ac9` | `ZC_ITEMCOMPOSITION_LIST` |
| `0x017C` | CZ | fixe 6 | — | `CZ_REQ_ITEMCOMPOSITION` |
| `0x017D` | ZC | fixe 7 | `0x00cddd80` | `ZC_ACK_ITEMCOMPOSITION` |
| `0x0A3F` | ZC | fixe 11 | `0x00cf69d0` | `ZC_UPDATE_CARDSLOT` |

> ⚠️ Ces opcodes sont soumis au **shuffle** de la table de paquets comme tous les autres
> (cf. la note en tête de `inventory_viewer.cc` sur `kOpDrop`/`kOpFavorite`). Les valeurs
> ci-dessus sont celles de la table du client courant.

### 2.1 `CZ_REQ_ITEMCOMPOSITION_LIST` — 0x017A (4 octets)

```
+0  u16  opcode = 0x017A
+2  u16  card_index      // index d'inventaire de la carte
```

Construit **inline** dans le méga-switch de `CMode::SendMsg`, il n'existe pas de fonction
`Send_CZ_*` dédiée :

```asm
00c8f556  MOV  [EDI+0x45c], EDX        ; CMode+0x45c = index inventaire de la CARTE
00c8f55c  MOV  EAX, 0x17A
00c8f561  MOV  [EBP-0x10508], AX       ; u16 opcode
00c8f568  MOV  [EBP-0x10506], DX       ; u16 card_index
00c8f56f  CALL 0x00c14d60              ; CRagConnection::GetInstance
00c8f57b  PUSH 0x17A / CALL 0x00c14460 ; PacketLen_Get(0x17A) -> 4
00c8f591  CALL 0x00c14920              ; CRagConnection_SendPacket(conn, len, &buf)
```

### 2.2 `ZC_ITEMCOMPOSITION_LIST` — 0x017B (variable)

```
+0  u16  opcode = 0x017B
+2  u16  packet_length
+4  u16  equip_inv_index[(packet_length - 4) / 2]
```

Handler inline @ `0x00ca5ac9` (dispatch : `0x00caa2e0 + (0x17B - 0x73) * 4 = 0x00caa700`) :

1. `count = (packet_length - 4) / 2`
2. `UIWindowMgr_MakeWindow(0x00a39340, 0x4A)` → crée/récupère `UIItemCompositionWnd`
3. `wnd->OnMsg(0x4B, 0, 0, 0, 0)` — reset / vidage de la liste
4. `wnd->OnMsg(0x4D, CMode->[0x45c], …)` — injecte l'index d'inventaire de la carte
5. boucle `0x00ca5b36`–`0x00ca5bc7`, une itération par index reçu :
   - `Session_GetEquipInfoByInvIndex(0x00d5aa40)(0x015fa3c0, &info, idx)`
   - si `info+0x04 != 0` : `wnd->OnMsg(0x1F, info.idString.c_str(), invIndex, 0, 0)`

**Les index reçus ne sont stockés nulle part.** Ils sont consommés à la volée et poussés
dans la fenêtre. L'unique état persistant est `CMode+0x45c`.

En cas de liste vide, le serveur envoie plutôt le message `MSI_FAIL_ITEMCOMPOSITION_LIST`
(chaîne @ `0x0106bec0`).

### 2.3 `CZ_REQ_ITEMCOMPOSITION` — 0x017C (6 octets)

```
+0  u16  opcode = 0x017C
+2  u16  card_index     // index d'inventaire de la carte
+4  u16  equip_index    // index d'inventaire de l'équipement cible
```

Construit **inline** en `0x00c8f59d`, dans le bloc `case 0x7C` de `CMode::SendMsg`
(jumptable `0x00c930f0`) : `u16 0x017C | u16 cardIndex (ecx) | u16 equipIndex (edx)`,
envoi via `0x00c14920`.

Le bouton **Cancel** emprunte le même sélecteur avec `(-1, -1)` : aucun paquet n'est émis,
la fenêtre se contente de se fermer.

### 2.4 `ZC_ACK_ITEMCOMPOSITION` — 0x017D (7 octets)

```
+0  u16  opcode = 0x017D
+2  u16  equip_inv_index
+4  u16  card_inv_index
+6  u8   result          // 0 = succès, 1 = échec
```

`Recv_ZC_ACK_ITEMCOMPOSITION_017D` @ `0x00cddd80` :

- **échec** → message `MsgStringTable(0x1EE)`
- **succès** → message `MsgStringTable(0x1ED)`, puis :
  - récupère les deux `ItemSkillInfo`
  - cherche le **premier slot libre** dans `info+0x1c` .. `info+0x28`
  - y écrit l'id de carte (`atoi` de la `std::string` id)
  - `Inventory_DecreaseOrRemoveByInvIndex(0x00d57a30)` — consomme 1 carte
  - `Inventory_UpsertOrRemoveItem` — réinjecte l'équipement mis à jour

### 2.5 `ZC_UPDATE_CARDSLOT` — 0x0A3F (11 octets)

```
+0  u16  opcode = 0x0A3F
+2  u16  equip_location   // masque de position (pièce PORTÉE)
+4  u16  slot_index
+6  u32  card_id
+10 u8   op               // 1 = poser la carte, 2|3 = vider l'emplacement
```

`Recv_ZC_UPDATE_CARDSLOT_0A3F` @ `0x00cf69d0` :
`EquipLocation_DecodeToSlots(0x00d55850)` → `Session_GetEquipInfoBySlot(0x00d59b90)` →
écrit `info+0x1c + slot_index*4` → `Equipment_SetSlotItemInfo(0x00d76ec0)` qui réécrit
dans `session+0x17d0 + slot*0xf8`.

> **Distinction importante** : `0x017D` travaille sur des **index d'inventaire** ;
> `0x0A3F` travaille sur une **pièce déjà portée** (masque de position), et ne touche pas
> l'inventaire. Les deux peuvent arriver pour un même sertissage.

---

## 3. Le déclencheur : double-clic dans l'inventaire

`UIItemWnd_OnDblClk_UseOrEquip` @ `0x00949fc0` (slot vtable `+0x68` de `UIItemWnd` id 8,
vtable `0x0103d460`).

Le hit-test `Inventory_HitTestItemInfo` (`0x00939e30`) remplit un `ItemSkillInfo` local ;
`+0x00` = itemType, `+0x04` = index d'inventaire. Puis `switch (itemType)` :

| itemType | Action | msg `CMode::SendMsg` |
|---|---|---|
| 0, 1, 2, 0x12 | utiliser | `0x1B` |
| 4, 5, 8, 9, 0xB..0xF | équiper | `0x13` |
| **6 (CARTE)** | **sertissage** | **`0x7B`** |
| 10, 0x10, 0x11, 0x13 | équiper (alt) | `0x57` |

Le test discriminant est le simple `case 6:` (label `LAB_0094a2b2`), qui exécute
`pCMode->SendMsg(0x7B, invIndex, 0, 0, 0)` via `vtbl+0x18` sur le `CMode` courant
(`GameMode_GetActive(0x1213338)`). **Aucune vérification de compatibilité à ce niveau.**

Côté Bourgeon, ce chemin est **déjà emprunté** : `InventoryViewer::UseOrEquip()`
(`src/plugins/inventory_viewer.cc:201-221`) fait `case 6: → kCmdCard (0x7b)`
(`inventory_viewer.cc:135`, `cc:208`).

---

## 4. La fenêtre native `UIItemCompositionWnd`

| Élément | Valeur |
|---|---|
| **ID de fenêtre** | **`0x4A`** (74) |
| **VTABLE** | **`0x01034684`** |
| RTTI type descriptor | `0x0123fb0c` (nom `.?AVUIItemCompositionWnd@@` @ `0x0123fb14`) |
| Complete Object Locator | `0x010c434c` (pointeur en `0x01034680`, soit vtable−4) |
| Class Hierarchy Descriptor | `0x010c4360` (5 bases, array `0x010c4370`) |
| **Constructeur** | `UIItemCompositionWnd_ctor` @ `0x0088dd80` |
| Destructeur (non-deletant) | `0x00890c50` |
| Scalar deleting dtor (slot +0x00) | `0x00892af0` |
| **Slot global de l'instance** | **`0x0131f6f0`** (= `g_UIWindowMgr + 0x208`) |
| Taille de l'objet | `0xFC` octets |
| Rect persistant | `ITEMCOMPOSITIONWNDINFO.X/Y/W/H` → `UIConfig+0x87c` / `+0x880` / `+0x884` / `+0x888` |
| Sauvegarde rect | `UIConfig_SaveWindowRectsToRegistry` @ `0x00a4eab1` |
| Création | `UIWindowMgr_MakeWindow(0x00a39340, 0x4A)` |

> **`UIConfig` et `g_UIWindowMgr` sont le même objet** : `0x0131f4e8 + 0x87c = 0x0131fd64`,
> valeur effectivement lue dans `this+0xF4` sur l'instance vivante.

### Héritage

`UIItemCompositionWnd` **dérive de `UIItemIdentifyWnd`** (vtable `0x010343fc`,
ctor `0x0088dde0`, id `0x49`). Seuls **6 slots** sont surchargés : `+0x00`, `+0x2c`,
`+0x3c`, `+0x50`, `+0x94`, `+0xb0`. Toute la mécanique de liste / scroll / boutons est
héritée — d'où le fait que le message `0x1F` (ajout de ligne) soit celui du parent.

### Méthodes virtuelles

| Slot | Adresse | Rôle |
|---|---|---|
| `+0x00` | `0x00892af0` | scalar deleting dtor |
| `+0x2c` | `0x008cfed0` | `SyncRectToUIConfig` — recopie x/y/w/h dans `UIConfig+0x87c` |
| `+0x3c` | `0x008a8420` | **OnCreate** |
| `+0x50` | `0x008b4730` | — |
| `+0x94` | `0x008c3590` | **OnMsg** (fallback `UIItemIdentifyWnd_OnMsg` @ `0x008c3960`) |
| `+0xb0` | `0x0089b5c0` | `SerializeStateForReplay` |

`OnCreate` @ `0x008a8420` délègue à la base `0x008a8470`, qui crée :
- la scrollbar en `this+0xB8`, positionnée à `(w-0x0F, 0x22)`
- le **bouton OK**, command id **`0xB8`**, à `(w-0x5C, h-0x18)`
- le **bouton CANCEL**, command id **`0xB9`**, à `(w-0x2E, h-0x18)`
- init : `+0xBC = 0`, `+0xC0 = -1`, `+0xC4 = 1`, `+0xC8 = 4` (lignes par page),
  `this+0x90 = 0xB8` (bouton par défaut), titre `this+0xD8 = MsgStringTable[0x20A]`
  (« Insert Card »), `this+0xF8 = 0`

### Messages `OnMsg` (via `vtbl+0x94`)

| Msg | Arguments | Rôle |
|---|---|---|
| `0x4B` | — | Vider la liste (`FUN_00799a00` sur `this+0xD0`) |
| `0x4D` | `invIndexCarte` | `this+0xF8 = index` **et** titre `"%s (%s)"` = MsgStr(0x20A) + nom de la carte |
| `0x1F` | `idString`, `invIndex` | *(hérité)* Ajoute une ligne : `Session_GetEquipInfoByInvIndex` → `Inventory_AppendNewItem(this+0xD0)`, puis auto-msg `0x3C` |
| `0x06` p3=`0xB8` | — | **OK** : si `this+0xC0 != -1` → `GetCandidateListEntryAt(this, &info, this+0xC0, &found)`, `equipIdx = info+0x04`, puis `CMode::SendMsg(0x7C, equipIdx, this+0xF8)` ; `SaveWindowRect(0x4A)` |
| `0x06` p3=`0xB9` | — | **Cancel** : `CMode::SendMsg(0x7C, -1, -1)` |
| `0x22` | — | Attache le bloc rect `UIConfig` |
| `0x3C` | — | Rebuild des lignes |
| `0x07` / `0x09` / `0x0A` | — | Scroll |

> Le clic sur **OK** ne construit pas le paquet lui-même : il repasse par
> `CMode::SendMsg` sélecteur `0x7C`, dont le bloc `0x00c8f59d` émet `CZ_REQ_ITEMCOMPOSITION`.
> Cancel emprunte **le même** sélecteur avec `(-1, -1)`, ce qui n'émet aucun paquet et se
> contente de fermer.

### Layout de l'objet (taille `0xFC`)

Offsets **vérifiés en mémoire live** (x32dbg, popup ouvert sur une Turtle General Card) :

| Offset | Contenu | Valeur observée |
|---|---|---|
| `+0x00` | vtable | `0x01034684` |
| `+0x14` | largeur | 200 |
| `+0x18` | hauteur | 200 |
| `+0x1c` | pos X | 1333 |
| `+0x20` | pos Y | 254 |
| `+0x28` | visible | 1 |
| `+0x2c` | **id de fenêtre** | `0x4A` |
| `+0x90` | id du bouton par défaut | `0xB8` (OK) |
| `+0xb8` | scrollbar | ptr |
| `+0xbc` | première ligne visible | 0 |
| **`+0xc0`** | **index de la ligne sélectionnée** (`-1` = aucune) | `-1` |
| `+0xc4` | — | 1 |
| `+0xc8` | lignes par page | 4 |
| **`+0xd0`** | **`std::list` des candidats** (sentinelle) | `0x228f63e0` |
| `+0xd4` | nombre d'éléments | 1 |
| `+0xd8` | `std::string` titre (SSO MSVC) | `"Insert Card (Turtle General Card [Weapon])"` |
| `+0xf4` | ptr vers `UIConfig+0x87c` | `0x0131fd64` |
| **`+0xf8`** | **index d'inventaire de la carte source** | 29 |

**Nœud de la `std::list`** (`std::list<ItemSkillInfo>` MSVC) :

```
+0x00  next
+0x04  prev
+0x08  ItemSkillInfo  ← payload
       +0x08 = itemType   (payload +0x00)
       +0x0c = invIndex   (payload +0x04)  ← index de l'équipement candidat
```

Observé : un seul nœud à `0x228f9140`, `itemType = 5` (arme), `invIndex = 33` — soit
l'unique « Sword » affiché dans le popup. ✔

> ⚠️ **AMBIGUÏTÉ NON TRANCHÉE — lire ceci avant de toucher au code.**
>
> Deux sources se contredisent de **4 octets** sur tout ce bloc de champs :
>
> | Champ | Décompilation `OnMsg` (`0x008c3590`) | Dump mémoire live |
> |---|---|---|
> | sélection | `+0xbc` | `+0xc0` |
> | liste | `+0xcc` | `+0xd0` |
> | cardinal | `+0xd0` | `+0xd4` |
> | titre | `+0xd4` | `+0xd8` |
> | rect UIConfig | `+0xf0` | `+0xf4` |
> | index carte source | `+0xf4` | `+0xf8` |
>
> Arguments pour le dump : la `std::string` du titre n'est structurellement cohérente
> (ptr + `size = 42` + `capacité = 47`, et le pointeur contient bien les 42 caractères
> `"Insert Card (Turtle General Card [Weapon])"`) qu'à `+0xd8` ; les valeurs
> d'initialisation de `OnCreate` s'alignent aussi sur cette colonne.
> Argument pour la décompilation : c'est le code réellement exécuté, et un premier essai
> in-game avec les offsets « dump » a donné **liste vide + carte introuvable**.
>
> **Le delta est identique pour tous les champs**, donc un seul offset est à déterminer.
> Le code ne parie sur aucune des deux : il **résout la liste à l'exécution** en validant
> la sentinelle `std::list` (`head->next->prev == head && head->prev->next == head`),
> puis déduit les autres (`cardinal = liste + 4`, `index carte = liste + 0x28`).
> Cf. `ResolveCompListOff()` / `LooksLikeListHead()`.

---

## 5. Lire cartes serties et emplacements (pour l'UI ImGui)

### Layout `ItemSkillInfo`

```
+0x00  u32          itemType        (6 = carte)
+0x04  u32          invIndex        (0 = entrée vide / non trouvée)
+0x08  u32          equipLocation   (masque de position)
+0x1c  u32          card[0]
+0x20  u32          card[1]         4 emplacements, ids d'item (0 = vide)
+0x24  u32          card[2]
+0x28  u32          card[3]
+0x2c  std::string  id              (SSO : ptr heap si +0x40 > 0xF, sinon inline)
+0x40  u32          capacité SSO
+0x5c  u32          identified
+0x60  u32          refine
+0x74  u32          favorite
+0x98  u32          nb random options
+0x9c  …            random options (entrées de 5 octets)
```

### Accesseurs natifs

| Besoin | Fonction |
|---|---|
| Info d'un item d'inventaire | `Session_GetEquipInfoByInvIndex` @ `0x00d5aa40` (mgr `0x015fa3c0`) |
| Info d'une pièce portée | `Session_GetEquipInfoBySlot` @ `0x00d59b90` |
| Écrire un slot de carte | `ItemSkillInfo_SetCardSlot` @ `0x006a6af0` |
| **Nombre d'emplacements** | `ItemSkillDB_GetSlotCount` @ `0x006a4c10` |
| Préfixes Double/Triple/Quad | `ItemTitle_AppendCardPrefixes` @ `0x006a4310` |
| Suffixe `[n]` | `ItemTitle_AppendSlotCount` @ `0x006a4c40` |

`ItemSkillDB_GetSlotCount` : `atoi(info+0x2c)` → `ItemSkillDescDB_Lookup(id, &DAT_01255130)`
→ champ **`+0x30`** du descripteur. Retourne 0 pour l'enregistrement nul.

> **Convention du projet** : ne jamais recopier ces données en dur, toujours appeler le
> getter natif (cf. `feedback_never_hardcode_use_native`).

### ⚠️ Piège : items forgés / créés

Sur un item **forgé ou créé**, les 4 mêmes `u32` (`+0x1c`..`+0x28`) ne contiennent pas des
cartes mais les données du créateur (charid scindé en deux, star crumbs, élément) —
cf. `ItemInfo_IsForgedOrCreated` @ `0x006a5e30` et le commentaire détaillé
`src/plugins/item_desc_tweaks.cc:419-427`.

Heuristique déjà retenue dans le projet : `card[0] != 0 && card[0] <= 500` ⇒ item forgé,
on n'affiche ni cartes, ni emplacements, ni suffixe `[N]`. Les vraies cartes et enchants
ont un id `> 500`.

---

## 6. Plan de remplacement ImGui

### Principe directeur

Conserver **strictement** le protocole natif. Le plugin n'est qu'une surcouche
d'affichage : il observe `0x017B`, dessine la liste en ImGui, et émet `0x017C`. Le serveur
reste seul juge de la compatibilité.

### Approche retenue : lire l'objet natif, pas les paquets

Le RE change la donne par rapport au plan initial. Le popup natif possède un **slot global
stable** (`0x0131f6f0`) et **contient déjà tout l'état nécessaire** :

- `+0xf8` : index d'inventaire de la carte source
- `+0xd0` / `+0xd4` : la `std::list` des candidats et son cardinal
- `+0xc0` : l'index sélectionné

Il est donc inutile d'intercepter `0x017B` et de reconstruire un état parallèle. Le plugin
peut simplement, à chaque frame :

1. lire le slot `0x0131f6f0`, **valider la vtable** contre `0x01034684`
   (pattern `ReadValidWnd()` `inventory_viewer.cc:463`) — si nul, il n'y a pas de
   sertissage en cours, ne rien dessiner ;
2. parcourir la `std::list` en `+0xd0` (borne de sécurité sur `+0xd4`) pour récupérer les
   `invIndex` des candidats ;
3. dessiner la liste en ImGui ;
4. au clic sur un candidat, émettre `0x017C` directement.

Avantages : aucun état dupliqué à resynchroniser, aucun risque de désynchro si le popup
est ouvert par un autre chemin, et **repli automatique sur le natif** si l'option est
désactivée.

### Implémentation livrée

Le code vit dans le plugin **`InventoryViewer`** (pas de plugin séparé : mêmes helpers
`FindInfoByIndex` / `ResolveIcon` / `SafeBuildName`, même cache d'icônes).

| Élément | Emplacement |
|---|---|
| Constantes RE + lecteurs POD | `src/plugins/inventory_viewer.cc` (namespace anonyme) |
| Fenêtre ImGui | `InventoryViewer::RenderCardInsert()` |
| Masquage natif (chaque tick) | `InventoryViewer::OnTick()` |
| Masquage natif (à la création) | `InventoryViewer::HideCardInsertAtCreation()`, appelé depuis `MakeWindowHook` (`window_pos_tweaks.cc`, cas `0x4A`) |

1. **Lecture de l'instance** — `*(void**)0x0131f6f0`, vtable validée contre `0x01034684`,
   sous `__try/__except`, POD uniquement (attention `C2712` : pas de `__try` dans une
   fonction contenant des objets C++ — d'où des helpers dédiés `ReadCompCandidates` /
   `ReadCompCardIndex` / `ReadCompItem` hors de la fonction de rendu).
2. **Parcours de la liste** `+0xd0` : `node = head->next`, arrêt sur la sentinelle, cap
   d'itération, `invIndex = *(u32*)(node + 0x0c)`.
3. **Candidats** lus **directement dans le payload des nœuds** (`node+0x08` EST un
   `ItemSkillInfo` complet), et surtout **pas** via la liste session : celle-ci
   **exclut les items portés**, ce qui ferait disparaître de la liste une arme déjà
   équipée que le serveur propose pourtant. On en tire le nom (`BuildDisplayName`, repli
   `GetBaseName`), l'icône via `ResolveIcon()`, le refine, et le **nombre de cartes serties**
   (`info+0x1c`, avec le garde-fou « item forgé » de §5).

   > ⚠️ **Ne compter que les `ItemSkillDB_GetSlotCount` PREMIÈRES entrées** de
   > `info+0x1c`. Les **enchantements** (items de type carte, sous-type *enchant*) sont
   > écrits par le serveur dans les entrées **hautes** (`card[3]`, puis `card[2]`…) même
   > sur un item à 2 emplacements. Les compter faisait passer l'item pour plein et
   > grisait « Sertir » — alors que `pc_insert_card` ne cherche un emplacement libre que
   > dans `[0, slots)` (d'où le double-clic qui sertissait quand même).
   > ⚠️ **Le payload du nœud est un INSTANTANÉ, pas une vue.** Le handler de `0x017B`
   > *copie* l'`ItemSkillInfo` (`Session_GetEquipInfoByInvIndex(&info, idx)` puis
   > `Inventory_AppendNewItem`). Après un sertissage il ne reflète plus l'item : cartes
   > serties et emplacements libres restent figés. Et la liste n'est vidée (`OnMsg 0x4B`)
   > que si le serveur renvoie un `0x017B` — or quand plus **aucun** équipement n'est
   > compatible il envoie `MSI_FAIL_ITEMCOMPOSITION_LIST` à la place, donc la fenêtre
   > garde éternellement l'ancienne liste. Le plugin relit donc la fiche **vivante** dans
   > l'inventaire session (`FindInfoByIndex`), avec repli sur l'instantané pour un item
   > **porté** (la liste session les exclut), et écarte les entrées dont tous les
   > emplacements sont pleins — même borne que `clif_use_card`, pas un filtrage de
   > compatibilité.
4. **Sélection** mémorisée par **index d'inventaire**, pas par rang de ligne — elle reste
   correcte si le serveur renvoie une liste différente, et elle est invalidée si l'item
   disparaît.
5. **Validation** — on passe par le **sélecteur `0x7C` de `CMode::SendMsg`**, pas par un
   paquet fabriqué :
   - Sertir → `SendMsg(0x7C, equipIndex, cardIndex)` puis `CloseCardInsert()`
   - Fermer / croix → `SendMsg(0x7C, -1, -1)` puis `CloseCardInsert()` (le bloc natif
     n'émet aucun paquet avec `-1, -1`)

   > ⚠️ Le sélecteur `0x7C` **n'envoie que le paquet, il ne ferme pas**. La fermeture est
   > faite séparément par `OnMsg`, via `UIWindowMgr_SaveWindowRect(mgr, 0x4A)` —
   > `0x00a2e770`, `__thiscall(mgr, id)`. Les deux branches natives (OK `0xB8` et
   > cancel `0xB9`) l'appellent explicitement après le `SendMsg`. Oublier cet appel
   > laisse le popup ouvert : c'est le bug du premier jet.

   C'est la variante (a) du plan initial, mais via le **dispatcher** (dont la signature
   est déjà éprouvée dans ce fichier par `SendCmd`) plutôt que via `OnMsg` — l'ordre des
   paramètres d'`OnMsg` restait ambigu à la lecture statique, et une erreur y aurait été
   un crash plutôt qu'un simple mauvais affichage.
6. **Premier plan à l'ouverture** : `ImGui::SetNextWindowFocus()` sur le **front montant**
   uniquement. Le double-clic part de l'inventaire, qui a donc le focus — sans ça le
   popup s'ouvrirait derrière lui. Le forcer à chaque frame rendrait la fenêtre
   impossible à passer en arrière-plan.
7. **Masquage du natif** : `wnd+0x28 = 0` chaque tick **et** à la création (le popup naît
   du handler de `ZC 0x017B`, donc entre deux ticks — sans le hook, une frame native
   passerait à l'écran). **Jamais** de déplacement hors écran (`feedback_no_offscreen_hide`).

### Fonctions de confort ajoutées

- **Compteur de stock** dans l'en-tête (`(xN)`) : somme des quantités de tous les stacks
  du nameid de la carte dans l'inventaire (`CountCardStock`). Lu frais chaque frame, donc
  se met à jour tout seul après chaque sertissage (le handler natif de `ZC 0x017D`
  décrémente le stack).
- **Sertir ×1 à ×N d'affilée** sur le même équipement. `N = min(slots libres, stock)`,
  où slots libres = `ItemSkillDB_GetSlotCount(info)` (`0x006a4c10`, `__fastcall(info)`)
  − cartes déjà serties. Chaque `SendMsg(0x7C, equip, card)` est répété `K` fois ;
  comme `K ≤ min(libres, stock)`, chaque paquet reste valide (un slot libre + une carte
  à consommer). Après le lot, si des cartes restent → re-demande de liste
  (`SendMsg(0x7B, card)` = `CZ 0x017A`) pour rafraîchir **sans fermer** ; sinon fermeture
  (l'index de la carte devient invalide). L'UI plafonne les boutons à `×4`.
- **Aperçu de description au survol** : identique à la grille d'inventaire. Le helper
  `DrawRoDescTooltip()` a été factorisé et est partagé par les deux (aperçu RO riche si
  l'option « Description au survol » est active, sinon tooltip texte). Les cartes serties
  et random options sont lues dans l'`ItemSkillInfo` du candidat (`+0x1c` / `+0x9c`).
  `RenderSimpleDesc` a reçu un paramètre `refine` (le titre de l'aperçu ne portait pas le
  « +N » — `GetCardDesc` renvoie le nom de base) ; il est propagé aux **3 usages** :
  grille inventaire, sertissage, et entrepôt (`storage_tweaks`, dont la struct `Item` a
  gagné un champ `refine` lu à `info+0x60`).

> ⚠️ Le nom renvoyé par `BuildDisplayName` **contient déjà le refine** (« +10 Dagger ») :
> ne pas re-préfixer `+%d` (double « +10 +10 »). On y ajoute seulement le nombre
> d'emplacements « [N] ».

### Sertissage rapide (menu contextuel) — paquet custom serveur

Sur le clic droit d'un **équipement (arme/armure) avec un emplacement libre**, un sous-menu
« Sertissage rapide » liste les cartes de l'inventaire **réellement** sertissables, et
un clic sertit directement (sans ouvrir le popup).

Le client ne connaissant PAS les règles de compatibilité (100 % serveur), la liste est
fournie par un **paquet custom moonlight**, sur le modèle de `ZC_BOURGEON_STORAGE_PRICES` :

| Opcode | Sens | Contenu |
|---|---|---|
| `CZ_BOURGEON_REQ_COMPAT_CARDS` `0x0F18` | client→serveur | `index_equip` (index inventaire client) |
| `ZC_BOURGEON_COMPAT_CARDS` `0x0F19` | serveur→client | `index_equip` (écho) + `count` + `count × index_card` |

Côté serveur (`moonlight`), le prédicat de compatibilité de `pc_insert_card` a été
factorisé en **`pc_can_insert_card(sd, idx_card, idx_equip)`** (pc.cpp/pc.hpp). Le handler
`clif_parse_bourgeon_reqcompatcards` (clif.cpp) applique ce prédicat à chaque carte de
l'inventaire pour l'équipement demandé → **la liste est exacte, zéro faux positif**
(chaque carte listée sera acceptée par le sertissage réel). Index en convention client
(`client_index`/`server_index`). Enregistré dans `clif_packetdb.hpp`, structs dans
`packets_struct.hpp`.

Côté client (`InventoryViewer`) : requête async à l'ouverture du sous-menu
(`RequestCompatCards`, une seule fois par équipement), réponse reçue dans `OnRecvPacket`
(`RegisterRecvOpcode(kCompatCards)` dans le ctor) → cache `qs_cards_`. Le sous-menu
affiche « Chargement… » puis la liste ; un clic envoie `CZ_REQ_ITEMCOMPOSITION` (via le
sélecteur `0x7C`) **sans** toucher au popup, et invalide le cache pour re-demander.

> ⚠ Déploiement **coordonné client + serveur** (rupture de protocole), comme tout opcode
> `bopcodes` — cf. l'en-tête de `bourgeon_opcodes.h`.

### Pas de réglage séparé

Le sertissage suit **`imgui_enabled_`** (« Inventaire Moonlight® ») : les deux fenêtres
forment un tout, et proposer un inventaire ImGui qui ouvre un popup natif — ou l'inverse —
serait incohérent à l'usage. Aucune clé yaml supplémentaire.

---

## 7. Renommages Ghidra effectués

| Adresse | Nom |
|---|---|
| `0x00cddd80` | `Recv_ZC_ACK_ITEMCOMPOSITION_017D` |
| `0x00cf69d0` | `Recv_ZC_UPDATE_CARDSLOT_0A3F` |
| `0x00d76ec0` | `Equipment_SetSlotItemInfo` |
| `0x00d57a30` | `Inventory_DecreaseOrRemoveByInvIndex` |
| `0x00c8f556` | commentaire : bloc d'émission `0x017A` |
| `0x00ca5ac9` | commentaire : handler `0x017B` complet |
| `0x00ca5bd2` / `0x00ca905b` | commentaires : trampolines `0x017D` / `0x0A3F` |
| `0x00949fc0` | commentaire : chemin « carte » du double-clic |
| `0x006a6af0` | commentaire : layout des slots cartes |
| `0x0088dd80` | `UIItemCompositionWnd_ctor` |
| `0x00890c50` | `UIItemCompositionWnd_dtor` |
| `0x00892af0` | `UIItemCompositionWnd_scalar_deleting_dtor` |
| `0x008a8420` | `UIItemCompositionWnd_OnCreate` |
| `0x008c3590` | `UIItemCompositionWnd_OnMsg` |
| `0x008cfed0` | `UIItemCompositionWnd_SyncRectToUIConfig` |
| `0x0089b5c0` | `UIItemCompositionWnd_SerializeStateForReplay` |
| `0x0088dde0` | `UIItemIdentifyWnd_ctor` |
| `0x008a8470` | `UIItemIdentifyWnd_OnCreate` |
| `0x008c3960` | `UIItemIdentifyWnd_OnMsg` |
| — | `UIItemIdentifyWnd_GetCandidateListEntryAt` |
| `0x01034684` | donnée `vtbl_UIItemCompositionWnd` |
| `0x010343fc` | donnée `vtbl_UIItemIdentifyWnd` |
| `0x00c8f59d` | commentaire : bloc d'émission `0x017C` |
| `0x00a3c872` | commentaire : case de `MakeWindow` pour l'id `0x4A` |

> Note méthodologique : la vtable n'apparaissait pas dans les xrefs Ghidra parce que le
> Complete Object Locator n'était pas défini comme donnée. Il a fallu remonter la chaîne
> RTTI à la main (type descriptor → COL → `vtable−4`), ce que la lecture mémoire live a
> ensuite confirmé.
