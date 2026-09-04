# Fenêtre Storage / Guild storage — RE et remplacement ImGui

> Journal du chantier. La fiche de mémoire `project-storage-window-re` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

# Storage / Guild-Storage window (FAIT 2026-07-02, live-confirmé)

> ## 🔴 NATIF MORT (2026-08-01, commit 3a63952) — `docs/storage_window_re.md`
> La fenêtre 0x21 **ne naît plus** en mode ImGui. Tout ce qui suit sur le
> masquage (+0x28), `HideNativeAtCreation`, la lecture de `wnd+0x188/+0x18c` et
> la dérivation de `open_` depuis `g_StorageWnd_ptr` est **CADUC**.
>
> - **DEUX créateurs**, pas un : `0x0b08` ZC_INVENTORY_START **et** `0x00f2`
>   ZC_NOTIFY_STOREITEM_COUNTINFO (qui fait `MakeWindow(0x21)` puis `mov edx,[eax]`
>   **sans test** → on ne peut pas se contenter de faire échouer MakeWindow).
>   5 autres cases la créent aussi (listes storage legacy) → filet dans OnTick.
> - `0x0b08` est **MULTIPLEXÉ** (invType 0=inv, 1=cart, 2=storage, 3=guilde) ⇒
>   nouvelle surcharge `RegisterReplaceOpcode(op, bool(const uint8_t*, uint16_t))`
>   dont le prédicat LIT le paquet. Générique, réutilisable.
> - `0x00f8` reste **OBSERVÉ** exprès : son handler natif **vide le modèle**
>   (`sub_D566D0` = erase(begin,end) sur g_session+0x1718). Sans lui, les items
>   du storage précédent se mélangent au suivant.
> - 🔴 **Le warp ferme le storage EN SILENCE** : `unit_remove_map_` →
>   `storage_storage_quit()` qui ne fait que sauvegarder, **aucun 0x00f8**.
>   ⇒ observer 0x0091/0x0092, sinon viewer fantôme.
> - Ce qui rend tout ça possible : `ItemModel_AddOrStackStorageItem` peuple
>   `this+0x1718` **puis** teste `if (g_StorageWnd_ptr)` avant de rafraîchir.
> - **Effet de bord voisins** : `StorageOpen()` d'inventory_viewer et cart_viewer
>   cherchait la fenêtre native ⇒ rendait toujours faux ⇒ le garde-fou
>   « inventaire↔cart interdit si storage ouvert » (règle SERVEUR) tombait en
>   silence. Les deux passent par `StorageWindow::IsOpen()`.
> - **Pont natif RETIRÉ** (commit ceb8b7b) : `HandleNativeDrop`, `OnMouseDown`,
>   lecture du payload de drag, icône de drag redessinée, `SendDeposit`,
>   `SendCartToStorage`. Raison : **« Interface moderne » est tout-ou-rien**
>   (`SetModernInterface` écrit tous les `imgui_enabled_` ; le chargement YAML
>   réconcilie en OU) ⇒ inventaire et cart sont ImGui dès que le storage l'est,
>   leurs natives masquées sont **hors hit-test**, aucun drag natif ne peut partir.
>   `PendAction` = 2 sens SORTANTS (`kPendStoToInv`, `kPendStoToCart`) ; ce qui
>   ENTRE est émis par la fenêtre d'origine.
> - ⚠ **Status + Équipement natives : pas encore empêchées de naître** (la
>   character sheet les remplace fonctionnellement). Chantier ouvert.

> ## ONGLETS DE STORAGE (2026-08-02) — `docs/storage_window_re.md` §8
> Six entrepôts perso existent déjà côté serveur (`conf/import/inter_server.yml`,
> id 0..5) et sont ouvrables par **`@storage` / `@storagealt1..5`** — 🔴 ces
> commandes sont dans **`src/custom/atcommand.inc`**, PAS dans
> `src/map/atcommand.cpp` (y chercher = ne rien trouver). Groupe `Player` (id 0)
> les autorise toutes ⇒ tout le monde y a droit.
> - 🔴 `ACMD_FUNC(storagealt)` **FERME** si un storage est ouvert (toggle) : il ne
>   bascule pas. C'est ce que le nouveau CZ **0x0F1D** corrige (ferme PUIS ouvre).
> - ZC **0x0F1E** = liste des storages **filtrée par `pc_can_use_command`** sur le
>   nom de la commande (`storage` / `storagealt<N>`) ⇒ onglets et commandes
>   partagent la MÊME permission, pas de 2e table. Envoyée à chaque ouverture
>   (donc aussi en mode natif) + au login vérifié (`cur_id=0xFF`).
> - 🔴 **C'est la FERMETURE qui nettoie** : le serveur ferme toujours avant de
>   rouvrir, et le 0x00f8 qui en découle fait vider le modèle par le handler
>   NATIF (cf. plus bas). Côté client `switching_` empêche seulement CloseLocal
>   de fermer le viewer ; `OnTick` n'extrait PAS pendant la bascule (le modèle
>   contient encore l'ancien contenu, et le chargement d'un alt est ASYNCHRONE
>   via `intif_storage_request`) ⇒ garde-fou 5 s.
> - Rendu opt-in (`storage_tabs`), onglets **perpendiculaires** aux catégories,
>   libellé = numéro par défaut, renommables + icône d'item (client seul,
>   `storage_tab_custom`).

> **Fantôme inventaire au dépôt drag (RÉSOLU 2026-07-04)** — déposer un item par drag le laissait en fantôme dans l'inventaire. Cause = PAS le drag : le prix-à-l'ajout (`clif_bourgeon_storage_prices` dans `clif_storageitemadded`) envoie un ZC custom 0x0F0F **interleavé avant le `delitem`**, et tout opcode >0x0C35 déclenche un **vide-buffer recv** côté client qui jetait le delitem. Fix = hook `RecvBuffer_ResetAll` (skip pour nos opcodes). Détails complets + « pourquoi jamais vu avant » dans [[project_opcode_system]]. Prix-à-l'ajout serveur = CONSERVÉ (sûr avec le fix client).

**Découverte-clé :** il n'existe PAS de classe `UIStorageWnd`. L'entrepôt Kafra (perso),
l'entrepôt de guilde ET le premium storage sont TOUS rendus par **`UIItemStoreWnd`
(window id 0x21)** — "Item Store" = entrepôt d'items, PAS le shop de vente. Client
packetver moderne (~2018+) : le storage passe par le **framework inventaire générique**
indexé par un enum `invType` (INVENTORY=0, CART=1, STORAGE=2, GUILD_STORAGE=3).

## Identité
- Classe RTTI `.?AVUIItemStoreWnd@@` (type desc 0x01240650). Window **id 0x21 (33)**.
- **vtable = 0x0103ca40** (live confirmé). Sous-fenêtres : `UIItemStoreSubWnd`, `UIItemStoreFindWnd` (recherche).
- Persisté sous **ITEMSTOREWNDINFO** (X/Y/W/H/subWnd/findWnd + `_%d` indexé). Record mémoire = **g_ItemStoreWndInfo_record @0x0131fafc** {X,Y,W,H,tabMask@+0x10}.
- Titre de fenêtre en jeu (bmp) = `\Information\Store.bmp`.

## Fenêtre CART (chariot) — RE live 2026-07-04
- **Global cart = `0x0131f6a0`** (contient le ptr fenêtre cart quand ouvert, 0 sinon ; voisin de l'inventaire 0x0131f6bc). **vtable = `0x0103d538`** (sœur de l'inventaire 0x0103d460 = MÊME framework générique invType=1).
- Rect/liste aux MÊMES offsets que l'inventaire : x=+0x1c, y=+0x20, w=+0x14, h=+0x18 ; liste std::list à +0xe8 (head)/+0xec (size) ; nœud : id str +0x34 (SSO), amount +0x18, index +0xc.
- **Opcodes cart↔storage (fixes 8o `[op][index:2][amount:4]`, confirmés client+serveur)** : storage→cart = **0x0128** (serveur `server_storage_index` = −1) ; cart→storage = **0x0129** (serveur `server_index` = −2). PAS remappés par le shuffle.
- **Plugin (storage_tweaks) — LES 2 SENS FAITS** :
  - storage→cart : drag item viewer → hit-test MouseOverCart → prompt → SendStorageToCart 0x0128.
  - cart→storage : drag natif cart → drop sur viewer → SendCartToStorage 0x0129. **Détection de source = heuristique mousedown** : `OnMouseDown` (appelé par le hook WndProc au WM_LBUTTONDOWN) mémorise `mousedown_over_cart_ = MouseOverCart(clic)` ; `HandleNativeDrop` route `pend_action_ = mousedown_over_cart_ ? kPendCartToSto : kPendDeposit`. Le drag démarrant au mousedown sur l'item, la fenêtre du mousedown = la source. Fiable, contourne le besoin de la catégorie payload.
- **Note RE catégorie (non utilisée au final)** : payload+0x00 = param_1 de DragDropMgr_BeginDrag_FullPayload (0x00c938e0) = catégorie CONSTANTE par appelant (inventaire=4, 0x1c, 0x18/2) — PAS l'invType, et l'appelant cart n'a pas été identifié. L'heuristique mousedown a rendu ça inutile. srcSlot = payload+0x14 (-1 pour inv/cart/storage).

## Cacher le natif = REMPLACEMENT COMPLET (FAIT 2026-07-05)
- **`wnd+0x28` = flag de visibilité UIWindow** (0=caché, 1=montré). Trouvé via la méthode Show/Hide **vtable+0x38 = `UIWindow_SetVisible` @0x005aad80** (fait juste `wnd+0x28 = flag` ; appelée par ToggleWindow). Mettre à 0 = sort du **rendu ET du hit-test input**, SANS toucher position (zéro corruption persistance) ni session serveur.
- **Plugin** : StorageTweaks::OnTick force `*(wnd+0x28) = hide_native_ ? 0 : 1` chaque tick (le natif peut le remettre à 1 sur événement). `g_StorageWnd_ptr` (0x0131f770) reste non-nul → `open_` reste vrai → viewer ouvert. Setting `hide_native_` (défaut ON) + checkbox « Cacher natif ». Placement 1re ouverture : viewer prend la place du natif (nx,ny) si caché, sinon à droite.
- **FLICKER (réglé)** : OnTick seul laissait 1 frame native visible à l'ouverture (fenêtre créée par MakeWindow avec +0x28=1, rendue 1× avant le tick). Fix = `StorageTweaks::HideNativeAtCreation(win)` appelé depuis le **hook MakeWindow de WindowPosTweaks** (id==0x21) → +0x28=0 AVANT le 1er rendu. (msg 0x22 = ancien point de WindowPosTweaks, abandonné car rate les fenêtres qui se placent via SetPos direct dans MakeWindow ; MakeWindow = point universel flicker-free.) Le visible-flag +0x28 est aussi écrit par UIWnd_SetVisible 0x009030c0 (cross-confirmé par WindowPosTweaks).
- **PIÈGE desc (réglé 2026-07-05)** : `OpenItemDesc` parcourait la liste d'AFFICHAGE de la fenêtre (`wnd+0xe8`), qui ne se peuple pas pareil quand la fenêtre est cachée → desc KO en perso, OK en guilde (quirk de peuplement). Fix = parcourir le **modèle session `0x015fbad8`** (comme Extract) : toujours peuplé, info complète, couvre tout storage ouvert. **Règle** : tout accès aux items storage passe par `0x015fbad8`, JAMAIS `wnd+0xe8` (fragile si caché).
- **Pourquoi rien ne casse** : aucune des 4 opérations n'utilise la fenêtre native — dépôt/cart→storage = drag d'inventaire/cart lâché sur le VIEWER ; retrait/storage→cart = drag ImGui DEPUIS le viewer ; close = paquet 0x0193. Le modèle storage lu par Extract = g_session+0x1718 (0x015fbad8), indépendant de la visibilité de la fenêtre. `kOffVisible=0x28`.
- Vtable storage complet lu live : Show/Hide slot 0x38=0x005aad80, DrawContent slot 0x50=0x00946730, OnDraw-ish slot 0x78=0x00a23470.

## Nom de l'entrepôt (titre) — ZC_INVENTORY_START
- Le serveur envoie le nom dans **`ZC_INVENTORY_START` (0x0b08)** à l'ouverture (via `clif_inventoryStart(sd, type, name)` appelé par `clif_storagelist`). Format (packetver RE≥20180919) : `[type:2][len:2][invType:1][name:≤24 null-term]`. invType = **INVTYPE_STORAGE=2** pour TOUS les storages (perso/guilde/premium). Noms serveur : `storage_getName(0)` (perso, défaut "Storage") / "Guild Storage" / `storage_getName(premium.stor_id)`.
- **Plugin** : `RegisterObserveOpcode(0x0b08, 27)` (observe, handler natif intact) → OnRecvPacket lit `data[2]`=invType (filtre ==2) et `data[3..]`=nom → `storage_name_` → **titre du viewer** (`"%s###bourgeon_storage"`, id ImGui stable pour la persistance). Repli "Entrepot".

## Globals plugin-ables (live confirmés)
- **`g_StorageWnd_ptr` @0x0131f770** = pointeur vers la fenêtre storage OUVERTE (0 si fermée). Analogue de kItemDescWndGlobalPtr 0x0131F700.
- **Collection storage = g_session(0x015fa3c0) + 0x1718 = 0x015fbad8** : `std::list` {head, size}. Live: size=114. (Inventaire = g_session+0x16D8, voisin.)
  - Node : list-overhead +0/+4, payload à +8 ; **+0xc = item id, +0x18 = amount**. Élément = ItemSkillInfo (struct item unifié).
- g_session = g_SkillInfoMgr = **0x015fa3c0** (même objet).

## Pipeline réseau -> fenêtre
1. **OUVRIR** : `ZC_INVENTORY_START` **0x0b08** -> handler thunk 0x00ca9a2c -> **`GameMode_OnRecv_ZC_INVENTORY_START` @0x00cd8d10**.
   Paquet var-len : [type:2][PacketLen:2 @buf+2][invType:1 @buf+4][name:var @buf+5], nameLen=PacketLen-5 (max 24).
   switch(invType) : 0->ouvre inventaire (FUN_00d562d0/55ff0/7f450) ; 1->cart (FUN_00d56360) ;
   **2 STORAGE & 3 GUILD_STORAGE -> `MakeWindow(mgr,0x21)` ; si name!="" -> `UIItemStoreWnd_SetTitleFromServer`(0x00ce78c0) pose le titre serveur dans un contrôle enfant en this+0x15c.**
2. **PEUPLER** : `ZC_STORE_ITEMLIST_NORMAL` **0x0b09** / `ZC_STORE_ITEMLIST_EQUIP` **0x0b39** -> thunk 0x00ca9a3d -> **`GameMode_IngestItemList` @0x00cd8b00**(invType, count=(len-5)/0x22, itemArr@buf+5).
   Stride entrée = 0x22 (34o) : count(short@-9), id(int@-7), 4 dwords options aléatoires(@+4 xmm), flags(@+0x18). switch invType : 2&3 -> **`ItemModel_AddOrStackStorageItem` @0x00d555c0** (stack si id existe dans g_session+0x1718, sinon insert ; puis OnMsg(0x17) sur g_StorageWnd_ptr pour rebuild la vue).
3. **FIN** : `ZC_INVENTORY_END` **0x0b0b** -> thunk 0x00ca9a98.
4. **COMPTEUR used/max** : `clif_updatestorageamount` serveur -> OnMsg **case 0x37** : this+0x188=used, this+0x18c=max, formate "%d/%d" dans this+0x144 (std::string). Live "114/600".

## UIItemStoreWnd — carte des méthodes (vtable 0x0103ca40)
- **DrawContent +0x50 = `UIItemStoreWnd_DrawContent` @0x00946730** : titlebar, onglets (blit tex 0x1027d24 gauche / 0x1027d54 droite), grille d'items depuis this+0xe8, texte compteur (rouge si +0x18c<=+0x188). Flag **this+0x190** : ==0 -> panneau info 3 lignes (msg 0xdd4/0xc77/0xe13, ex. permission guilde/vide) ; !=0 -> dessine la grille.
- **+0x80 = `UIItemStoreWnd_OnRButtonUp` @0x0094f200** : clic-droit slot -> desc item (MakeWindow 0xc, msg 0x18) ; Shift+clic-droit -> lien chat (cmd 0x14d) ; Alt+clic-droit -> retrait storage->inventaire (cmd 0x38) ou ->cart (cmd 0x4e).
- **OnMsg +0x94 = `UIItemStoreWnd_OnMsg` @0x009544f0** :
  - 0x22 = OnCreate/init (this+0x164=record ITEMSTOREWNDINFO ; défauts onglets +0x16c..+0x17c={3,3,3,3,4} ; active 8 boutons onglets +0x100 selon record+0x10 bitmask).
  - 0x37 = SetCount(used,max) (cf. ci-dessus).
  - 0x16 = choisit l'onglet-filtre de catégorie (this+0x180, clampé 0..7).
  - 0x17 = reconstruit/re-filtre la liste visible (this+0xe8) par type d'item selon l'onglet : t0={0,1,2} t1={0x12} t2={4,0xb,0xc,0xd,0xe} t3={5,8,9,0xf} t4=tous t5={10,0x10,0x11,0x13} t6={6} t7={3,7}.
  - 0xe/sub3 = redimensionnement VERTICAL : H clampé [0x1ca(458), yBottom*7/10]. Largeur fixe 280. Repositionne boutons onglets (+0x100 stride 0x1a), édit recherche (+0xf0), scrollbar (+0xc0), close (+0xbc).
  - case 6 (boutons) : 0x85/0x16d=ouvre fenêtre recherche 0x99 avec le texte de l'édit +0xf0 ; 0x92..0x98=7 fenêtres-détail d'onglet (this+idx*4+0x120) ; 0x135=toggle sous-fenêtre 0x135 (log guilde ?) ; 0xc9=tout fermer ; 0xf6=cleanup ; 0x16c=refresh.
  - 0x27=focus ; 0x26=parent toggle ; default -> FUN_00950780 (base OnMsg).

## Struct UIItemStoreWnd (offsets confirmés live, id 0x21)
| off | rôle |
|---|---|
| +0x14 / +0x18 | W(280) / H(458) |
| +0x1c / +0x20 | X(700) / Y(85) |
| +0x2c | window id (0x21) |
| +0x3c | alpha (255) |
| +0xac | nb onglets (7) |
| +0xbc | grip de resize (UIResizeButton, créé en relayout ; pos W-0xf,H-0xf) |
| +0xc0 | scrollbar (enfant ; méthode +0x38=setrange, +0x10=setpos) |
| +0xc4/+0xc8 | enfants |
| +0xd8/+0xdc/+0xe0 | grille : scroll(row) / lignes visibles / **colonnes (FIGÉ à 1)** |
| +0xe8 | **liste d'items VISIBLE (std::list, vue filtrée)** |
| +0xf0 | contrôle édit de recherche |
| +0xf4 | enfant |
| +0x100..+0x11c | 8 boutons d'onglets |
| +0x120..+0x13c | 7 fenêtres-détail d'onglet + slot |
| +0x140 | fenêtre recherche (0x99) |
| +0x144 | std::string compteur "used/max" ("114/600") |
| +0x15c | contrôle enfant portant le TITRE serveur |
| +0x164 | record persistant ITEMSTOREWNDINFO (@0x0131fafc) |
| +0x180 | onglet-filtre courant (0..7) |
| +0x188 / +0x18c | used(114) / max(600) |
| +0x190 | flag "afficher la grille" (0=panneau info) |

## Perso vs Guilde
Côté client, invType 2 et 3 sont traités À L'IDENTIQUE (même MakeWindow(0x21)+SetTitle). Seule différence : le titre serveur ("Guild Storage") et la sous-fenêtre LOG `UIGuild_Storage_Log` (RTTI 0x01240154 ; toggle via msg 0x135). Strings MSI_GUILD_STORAGE_* (0x01061744+) pour titre/permission/log.

## Leviers de customisation (voir réponse session)
1. **Titre serveur enrichi** (0 patch client) : `clif_storagelist`/`clif_inventoryStart` envoie `storename` -> le mettre dynamique (nom perso, "114/600", thème event). Serveur: src/map/clif.cpp clif_storagelist (storename) + storage.cpp storage_getName.
2. **Overlay ImGui compteur/poids/valeur** : lire g_StorageWnd_ptr(0x0131f770) ; si !=0 lire g_session+0x1718 (list) -> total items/valeur/poids -> dessiner en bas de la fenêtre (miroir de inventory_tweaks.cc). 100% client, aucun paquet.
3. **Storage élargi/largeur redimensionnable** (extension de BiggerInventoryWindow/EquipTweaks) : patcher la taille de création + colonnes (+0xe0) + handler resize (case 0xe ne gère que la hauteur, largeur fixe 280). Plus de colonnes par ligne.

## Géométrie de la grille (RE approfondi live 2026-07-04)

Les items s'affichent en **liste 1 colonne** (icône + NOM complet + quantité), PAS en grille d'icônes. Live: W=280, H=458, used=121, max=600, mode(+0x190)=1. Champs grille live: +0xd8=0(scroll), +0xdc=12(lignes visibles), **+0xe0=1(colonnes, figé)**, +0xe8=liste filtrée, +0xec=nb items filtrés.

**DrawContent (0x00946730) boucle items** (désasm confirmé) :
- `IDIV [EDI+0xe0]` @0x00946b69 → EAX=idx/cols(row), EDX=idx%cols(col). Le code **supporte nativement les colonnes** (÷ et % par +0xe0), juste figé à 1.
- `SHL EAX,5` @**0x00946b71** → row_y = row*32 (**HAUTEUR LIGNE = 32px**). `SHL ECX,5` @0x00946b78 → col_x = col*32.
- icône (FUN_008711a0) @ (col_x+0x2c=44, row_y+0x15=21) ; nom+qty (FUN_00897860 → ItemSkillInfo_BuildDisplayName) @ (col_x+0x50=80, row_y+0x1d=29, **maxW=0xb4=180**).
- séparateurs de ligne dessinés tous les 0x20 (boucle @0x00946aa3, `ADD ESI,0x20` @0x00946aad).
- compteur used/max @+0x188/+0x18c en bas (rouge 0x232323 sinon bleu 0xff si used>=max, CMOVGE @0x00946c98).
- **+0x190==0** → panneau info 3 lignes (msg 0xdd4/0xc77/0xe13) au lieu de la grille (état vide/permission guilde).

**Hit-test (FUN_00939cf0)** : index cible = `+0xe0*((y-0x11)>>5) + ((x-0x28)>>5)`, zone y valide `< +0xdc*0x20+0x11`. Même hauteur 32 câblée en `>>5`/`*0x20`.

**Setter géométrie** : la classe liste partagée `FUN_0080aca0` (grip+scrollbar+`+0xe0=1`+`+0xdc=(H-header-17)>>5`) et le NPC buy/sell `FUN_0080a140` posent +0xe0=1 pour LEURS fenêtres. Le storage pose son propre +0xe0=1/+0xdc dans **OnMsg case 0xe (resize)** uniquement (pas par frame). Écrire +0xe0 en RAM ne tient pas à travers un resize.

**Faisabilité des customisations (verdict) :**
- La hauteur de ligne 32 est **power-of-2 câblée en shifts** (`<<5`/`>>5`/`*0x20`) sur ~5 sites (dessin, séparateurs, calcul lignes, hit-test ×2). Donc :
  - **Lignes compactes** (choix user) : PAS un patch d'immédiat propre — `SHL EAX,5` @0x00946b71 remplaçable par `IMUL EAX,EAX,<h>` (6B C0 h, 3 octets), MAIS le hit-test/calcul-lignes utilisent `>>5` (÷32) non convertible en ÷non-pow2 sur place → **hooks multi-sites**. Gain modeste (icônes 24px ⇒ min ~28px).
  - **Grille d'icônes multi-colonnes** : le rendu supporte déjà les colonnes ; patcher +0xe0>1 + supprimer le dessin du nom (overlap) + fixer hit-test. Hook moyen. Perd les noms.
  - **Storage plus haut (RECOMMANDÉ, patch propre)** : lignes/hit-test scalent déjà avec +0xdc=usableH/32. Seul le CAP de hauteur borne : OnMsg case 0xe `(yBottom * 7)/10`. Relever le cap = plus de lignes visibles, ZÉRO changement de géométrie. Largeur de création figée à `push 0x118`@**0x00a3a6b7** (hauteur 0x1CA@0x00a3a6b2) → SetSize FUN_00a1cb70.

## Plugin StorageTweaks — viewer ImGui (FAIT 2026-07-04, testé live)

`src/plugins/storage_tweaks.cc`+`.h`, enregistré dans bourgeon.cc `LoadPlugins()` + `src/CMakeLists.txt`. Overlay ImGui EN COEXISTENCE avec la fenêtre native (le natif reste fonctionnel ; l'ImGui est lecture seule à côté, synchro AUTO car même modèle).

**Détection ouverture** : `*(0x0131f770)` != 0 & vtable == 0x0103ca40 (= FindWindow(0x21)).
**Source items** : liste AFFICHÉE de la fenêtre `wnd+0xe8` (std::list ; _Myhead sentinelle @+0xe8, taille @+0xec ; reflète le filtre de catégorie natif). Nœud : next@+0, value(ItemSkillInfo)@+8.
**Par item (LEÇON offsets)** :
- id = `atoi(std::string à info+0x2c)` (cap @info+0x40). ⚠️ **node+0xc (info+4) N'EST PAS l'id fiable** (donnait des icônes "pomme" = placeholder unknown-item).
- identified = `*(info+0x5c)` (byte).
- quantité = `*(node+0x18)` (= info+0x10).
- nom = `ItemSkillInfo_GetBaseName(info, out, &cap, 0)` @0x006a2b50 __thiscall (indép. fenêtre).

**Icônes ImGui** (recette menu_icons/item_desc) : `BuildItemIconGrfPath` @0x00d5a720 **__stdcall(id_str, out[128], identified)** (RET 0xc = 3 args ! ⚠️ skill_bar l'appelle à 2 args par chance) : atoi(id)→ResolveItemResNameById→`유저인터페이스\item\<res>.bmp`. flag identified : `!=0`→resname [rec+8], `0`→[rec+0x1c] ; on passe `info+0x5c`. Puis TexMgr (0x00a90350/0x00a9f030/0x00a8d4a0, pix@+0x11c, w@+0x114 h@+0x118) → colorkey magenta→alpha0 → `Overlay_CreateTextureARGB` → cache par id.

**Clic-droit → description** (LEÇON) : `MakeWindow(mgr,0xc)` (desc ITEM) + `OnMsg(0x18, &ItemSkillInfo)` + `SetPos(x,y)`. item_desc_tweaks détecte 0xc et rend sa version enrichie. ⚠️ **Il FAUT passer l'ItemSkillInfo COMPLET du nœud** (server-loaded) : on re-parcourt la liste live au clic pour retrouver le nœud par id et passer `node+8` (OnMsg 0x18 copie ; on ne free PAS, on ne possède pas). Pièges écartés :
- `GetSkillInfo` (0x00d5a980) exige une **qté inventaire > 0** → desc ne marchait que pour les items aussi en sac.
- `ItemSkillInfo_ctor`(0x006a1b20)+`SetId`(0x006a6570) pose l'id (affiché) mais **desc VIDE** (manque des champs que le nœud a).

**Settings** : checkbox "ID" → colonne id triable (en mémoire ; persistance MoonlightUi = TODO). Fenêtre placée à droite du natif à l'ouverture, déplaçable.

**Favoris CLIENT (FAIT 2026-07-12)** : onglet « Favoris » + tag d'items. ⚠️ Contrairement à l'INVENTAIRE (flag serveur par item `node+0x90` / paquet 0x0907), le STORAGE n'a AUCUN flag favori serveur → favoris 100 % client, keyés par **id d'item** (tous les stacks/raffinements d'un même id = favoris ensemble), set `StorageTweaks::favorites_` persisté par MoonlightUi (yaml `storage_favorites`, trié). Tag = Ctrl+clic-gauche ou menu contextuel « Ajouter/Retirer des favoris ». Onglet Favoris (index 1 de `kStgCats`, champ `bool fav`) filtre par `IsFavorite(id)` au lieu de `ItemInTab`. Étoile dorée = `DrawFavStar` (ImDrawList, 10 triangles éventail : le glyphe ★ U+2605 est HORS des polices chargées — ProggyClean ASCII + Malgun range coréen).
**TODO possibles** : tout afficher (modèle complet g_session+0x1718 vs filtre natif), persister settings, totaux (poids/valeur), interactif (retrait cmd 0x38).

## Related
- [[project_inventory_window]] (g_session+0x16D8, pattern overlay poids), [[project_item_skill_desc_window_re]] (MakeWindow 0xc msg 0x18), [[reference_window_position_persistence]] (ITEMSTOREWNDINFO), [[project_opcode_system]] (dispatch 0x00caa2e0), [[project_20250716_re]] (recv 0x00c9df00, g_session), [[reference_warp0716]] (BiggerInventoryWindow), [[reference_moonlight_server]] (clif_storagelist).
