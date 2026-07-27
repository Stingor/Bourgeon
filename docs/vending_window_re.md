# RE — Échoppe joueur : vente (*vending*) et achat (*buying store*)

Client `2025-07-16_Ragexe` (base image `0x400000`, pas de rebase). Classes obtenues par
RTTI lu en live (x32dbg attaché, jeu **running** — ne jamais mettre en pause, ça déconnecte).
Tous les offsets de structure ont été **relus sur objets vivants**, une fois en mode vente et
une fois en mode achat.

---

## 1. Vue d'ensemble

L'échoppe joueur, c'est **deux fenêtres** affichées ensemble, et **un seul jeu de classes**
partagé entre le mode vente et le mode achat :

| Rôle | Classe | id vente | id achat | Titre affiché |
|---|---|---|---|---|
| Composition de l'échoppe | `UIMerchantShopMakeWnd` | **0x29** | **0xAE** | « Opening a stall » / « Buying Store Window » |
| Objets disponibles (grille) | `UIMerchantMirrorItemWnd` | **0x2A** | **0xAF** | « Available Items for Vending » / « Available items: » |
| Vue vendeur, échoppe lancée | `UIMerchantItemMyShopWnd` | **0x2D** | **0xB0** | « My Shop » |
| Historique des ventes | `UIMerchantItemLogWnd` | **0x101** | **0x102** | « Item Sell History » |
| **Côté acheteur** — offre du vendeur | `UIMerchantItemShopWnd` | **0x2B** | — | « Merchant Shop - *nom* » |
| **Côté acheteur** — panier | `UIMerchantItemPurchaseWnd` | **0x2C** | — | « Buying Items » | |

### « My Shop » — `UIMerchantItemMyShopWnd`, vtable `0x0103D100`

Ouverte **après** le lancement de l'échoppe, quand les fenêtres de composition ont
déjà disparu — cycle de vie indépendant. OnCreate `0x0093F770`, DrawContent
`0x009475C0`, OnMsg `0x00955EC0`. Liste d'affichage `+0xE8`, alimentée par
`VendingMyShop_GetCount 0x00D5CE70` / `GetAt 0x00D5C360`.

| Offset | Contenu |
|---|---|
| `+0xC4` | bouton `btn_close` (cmd **201**) |
| `+0xF0` | **zeny encaissé** (posé par `OnMsg 119`, libellé `MsgString 0x6C7`) |
| `+0x100` | mode : 0 = vente, ≠ 0 = échoppe d'achat |
| `+0x104` | case **« Notify when item sells out »** (cmd **213**) |
| `+0x11C` | marge haute du calcul de lignes |

État persistant de la case : **`g_MyShopNotifySellOut` `0x015FFFB4`**.

**⚠ Le bouton « close » met fin à la boutique.** Il ne se contente pas de fermer la
fenêtre : il dispatche `CMode::SendMsg` **81** (vente) / **270** (achat), puis
`SendMsg 40`, ferme 45 (`0x2D`) / 176 (`0xB0`) et ouvre `0x101` / `0x102`. Seul le
cas `+0x108 == 1` se contente de fermer. Une UI de remplacement doit donc le
libeller sans ambiguïté — et ne pas y câbler une simple croix.

### ⚠ Correction : `0x2B` / `0x2C` sont les fenêtres de l'**acheteur**

Une première lecture *statique* (le handler **ZC_PC_PURCHASE_MYITEMLIST `0x0136`**,
`RecvHandler_MyShopItemList 0x00CE4E30`, ferme les fenêtres 43 et 44 quand tout est
vendu) m'avait fait conclure que `0x2B` était la vue vendeur. **C'est faux.** RTTI
relu sur session vivante, les deux fenêtres ouvertes en cliquant sur l'échoppe d'un
autre joueur : `0x2B` = `UIMerchantItemShopWnd` (l'offre du vendeur), `0x2C` =
`UIMerchantItemPurchaseWnd` (le panier). La vue vendeur est bien
`UIMerchantItemMyShopWnd` **0x2D**. Moralité : l'id se confirme par RTTI, pas par
déduction sur un handler de paquet.

`UIMerchantItemShopWnd` (vtable `0x0103D028`, OnCreate `0x00941A50`, DrawContent
`0x00947D30`, OnMsg `0x00957190`, 280×184 observé). Son `OnMsg 23` reconstruit sa
liste (`+0xE8`) depuis `VendingOffer_GetCount 0x00D5CE90` / `GetAt 0x00D5C780`, puis
**auto-redimensionne la fenêtre** au nombre de lignes (minimum 4).

| Offset | Contenu |
|---|---|
| `+0xDC` | lignes visibles ; `+0xE0` colonnes (1) ; `+0xEC` nombre d'objets |
| `+0xF0` | marge (H = `+0xF0` + 32 × lignes) |
| `+0x100` | **AID du vendeur** (posé par `OnMsg 28`) |
| `+0x104` | label « Total » — créé **seulement si** `+0x10C ≠ 0` |
| `+0x108` | zeny encaissé (`OnMsg 119`, `MsgString 0x6C7`) |
| `+0x10C` | mode (largeur max 320 en achat, 426 en échoppe d'achat) |

Le basculement vente/achat n'est **pas** un changement de classe : c'est le champ **`+0x130`**
(`UIMerchantShopMakeWnd`) et **`+0xF4`** (`UIMerchantMirrorItemWnd`), passé au constructeur.

Une troisième fenêtre gravite autour sans en faire partie : **`UIMerchantItemWnd` id 0x28**,
qui est en réalité la fenêtre **Cart** (Alt+W) — la source des objets en mode vente.
Voir [`project_cart_window_imgui_todo`].

### Différences vente / achat

| | Vente (`mode = 0`) | Achat (`mode = 1`) |
|---|---|---|
| Emplacements max | `g_VendingMaxSlots`, plafonné à **13** | `g_BuyingStoreMaxSlots`, plafonné à **5** |
| Source des objets | le **cart** | l'**inventaire** |
| Colonne quantité | non | oui (`+0x114+4i`, ctrl id 5) |
| Edit « limite de zeny » | non | oui (`+0x128`, ctrl id 0x0F) |
| Case à cocher « safe check » | oui (`+0xDC`) | non |
| Edits prix au départ | grisés (182) + *readonly* | actifs (232) |
| Lignes dessinées | `+0x15C` | `max(+0x14C, 4)` |
| Taxe | **soustraite** du total | **ajoutée** au total |

Valeurs relevées en live : vente → `+0x12C = 12` emplacements ; achat → `+0x12C = 2`.

---

## 2. RTTI / vtables

Technique : `vtable-4` → *CompleteObjectLocator* → `+0x0c` *TypeDescriptor* → `+0x08` nom manglé.

| Classe | vtable | COL | TypeDescriptor |
|---|---|---|---|
| `UIMerchantItemWnd` (cart, 0x28) | `0x0103D538` | `0x010C63F8` | `0x012407BC` |
| `UIMerchantShopMakeWnd` (0x29 / 0xAE) | `0x0103D7C0` | `0x010C64BC` | `0x01240804` |
| `UIMerchantMirrorItemWnd` (0x2A / 0xAF) | `0x0103D610` | `0x010C6450` | `0x012407DC` |
| `UIMerchantItemPurchaseWnd` (achat *chez* un vendeur) | `0x0103D2B0` | `0x010C6338` | `0x01240764` |

### Slots de vtable

| Slot | `UIMerchantShopMakeWnd` | `UIMerchantMirrorItemWnd` | `UIMerchantItemWnd` (cart) |
|---|---|---|---|
| `+0x00` dtor | `0x00936B30` | `0x00936A90` | `0x009369F0` |
| `+0x3c` OnCreate | `0x00942080` | `0x00941E40` | `0x00941B80` |
| `+0x4c` OnTick | `0x0094E550` | — | — |
| `+0x50` DrawContent | `0x009488D0` | `0x00944240` | `0x00948610` |
| `+0x64` OnLButtonDown | `0x0094B7C0` | `0x0094B680` | `0x0094B460` |
| `+0x70` OnMouseMove | `0x00880CB0` | `0x0094E190` | `0x0094DF40` |
| `+0x80` OnRButtonDown | `0x0094FE40` | `0x0094FC50` | `0x0094FAA0` |
| `+0x94` **OnMsg** | `0x00957F50` | `0x00957A60` | `0x009576A0` |

Constructeur `UIMerchantShopMakeWnd` : **`0x00935080`** (`ctor(this, mode)`).
`UIMerchantMirrorItemWnd::OnMsg` retombe sur le **base partagé `UIItemShopWnd_BaseOnMsg` `0x00950780`**
(le même que les fenêtres shop NPC) et sur `UIItemShopWnd_ScrollRelayout` `0x00950400`.

---

## 3. Struct `UIMerchantShopMakeWnd`

En-tête `UIWindow` commun (validé live) : `+0x14` W · `+0x18` H · `+0x1c` X · `+0x20` Y ·
`+0x28` **visible** · `+0x2c` **window id**.

| Offset | Contenu |
|---|---|
| `+0xB4` | `std::string` **titre** (`MsgString 0xE0` / `0x6B8`) |
| `+0xCC` | `UIEdit` **nom de la boutique** (ctrl id **0x24**, largeur `W-66`, pos (41,22)) |
| `+0xD8` | index de la **ligne de prix qui a le focus** (`-1` = aucune) |
| `+0xDC` | `UIToggleButton` **case « safe check »** — *vente uniquement*, cmd **213** |
| `+0xE0 + 4*i` | `UIEdit` **prix** de la ligne `i` (ctrl id **0x0E**, 94×16) |
| `+0x114 + 4*i` | `UIEdit` **quantité** de la ligne `i` (ctrl id **5**, 42×16) — *achat uniquement* |
| `+0x128` | `UIEdit` **limite de zeny d'achat** (ctrl id **0x0F**) — *achat uniquement* |
| `+0x12C` | **nombre de lignes créées** (emplacements) |
| `+0x130` | **mode** : 0 = vente, 1 = achat |
| `+0x134` | scrollbar |
| `+0x138` | offset de défilement (1re ligne visible) |
| `+0x13C` | colonnes (= 1) |
| `+0x140` | pas de page (= 4) |
| `+0x144` | scrollbar visible (bool) |
| `+0x148` | **`std::list` des objets posés** (sentinelle circulaire, nœud de `0x100` o) |
| `+0x14C` | **nombre d'objets posés** |
| `+0x150` | enregistrement d'ancrage/position (msg 34) |
| `+0x154` | hauteur mémorisée |
| `+0x158` | total de lignes défilables |
| `+0x15C` | **lignes affichées** |
| `+0x160` | bouton **OK** (cmd **184**) |
| `+0x164` | bouton **cancel** (cmd **185**) |
| `+0x168` | bouton **Import** (cmd **560**) |
| `+0x16C` | longueur du texte de l'edit qui a le focus |
| `+0x170` | `std::string` sauvegarde du texte en cours d'édition |
| `+0x188` | octet de mode alternatif (voir §6) ; `+0x18A` / `+0x18C` = `short` init à `-1` |

Géométrie : ligne `i` à `y = 54 + 42*(i - scroll)`. Prix à `x = W-119`, quantité à `x = W-231`.
Vente = 400×260 ; achat = 360×289 (relevés live).

### Nœud de la liste `+0x148`

Identique à la famille panier shop NPC (payload `ItemSkillInfo` à `nœud+0x08`) :

| Offset nœud | Offset payload | Contenu |
|---|---|---|
| `+0x00` / `+0x04` | — | `next` / `prev` |
| `+0x0C` | `+0x04` | **index source** (cart en vente, inventaire en achat) |
| `+0x18` | `+0x10` | **quantité** |
| `+0x1C` / `+0x20` | `+0x14` / `+0x18` | **prix** (0 tant que OK n'a pas été validé) |
| `+0x34` | `+0x2C` | `std::string` = **l'item id EN TEXTE** (ex. `"714"`), pas le nom localisé |
| `+0x90` | `+0x88` | nombre de cartes (overlay d'icône) |

⚠ Même piège que la liste de vente NPC : `+0x34` est l'**id en texte** → `atoi` puis résolution
du nom/icône par id.

---

## 4. Struct `UIMerchantMirrorItemWnd`

| Offset | Contenu |
|---|---|
| `+0xBC` | bouton de redimensionnement (`UIResizeButton`, mode 7) |
| `+0xC0` | scrollbar |
| `+0xDC` | lignes = `(H-38)/32` |
| `+0xE0` | colonnes = `(W-40)/32` |
| `+0xE8` | **`std::list`** des objets disponibles |
| `+0xF0` | enregistrement d'ancrage |
| `+0xF4` | **mode** : 0 = vente, 1 = achat, 2 = 3e variante (non observée) |

Redimensionnement (msg 14, `p3 = 3`… en fait `p3 = 7` ici) : clamp `W ∈ [232, 320]`,
`H ∈ [120, 240]`, aligné sur une grille de 32 à partir de 280×120. Taille live : 280×120.

---

## 5. `OnMsg` de `UIMerchantShopMakeWnd` (`0x00957F50`)

Signature famille : `(this, p1, msgId, p3, p4, p5)`.

| msg | Rôle |
|---|---|
| **6** | clic bouton, `p1` = cmd (voir ci-dessous) |
| **7 / 9 / 10** | molette / page précédente / page suivante → `+0x138` |
| **23** | **rebuild** de la liste d'affichage depuis la session. `p1 > 0` = la ligne `p1-1` vient d'être retirée → décale les prix vers le haut, remet la dernière à `"0"` grisée + *readonly* |
| **34** | ancrage de position |
| **38** | **drop** d'un objet (drag & drop) |
| **123** | flux de *tags* |

### Commandes du msg 6

| cmd | Action |
|---|---|
| **184** (`0xB8`) | **OK** — validation complète puis ouverture (voir §6) |
| **185** (`0xB9`) | **cancel** |
| **213** (`0xD5`) | bascule `g_VendingSafeCheckEnabled` (case à cocher) |
| **343** (`0x157`) | aide « ? » → `MakeWindow(0x14D)` si absente |
| **560** (`0x230`) | **Import** → `UIMerchantShopMakeWnd_ImportSavedShop` |

### msg 38 — types de source du drag

Le type est lu dans `gameMode+0x308`, l'index dans `gameMode+0x30C`
(`gameMode = GameMode_GetActive(0x1213338)`).

- **11** → depuis la fenêtre miroir en **vente** ; plafond de quantité **30000** (au-delà :
  `MsgString 0x6A9` en chat). Ajout via `VendingShop_CanAddItem(rec, 0, 1)` +
  `VendingShop_AddItem(rec, 0, 1)`, puis la ligne correspondante est **dégrisée** (232),
  `readonly = 0`, texte remis au prix précédent (ou `"0"`) et focus posé dessus.
- **20** → depuis l'**inventaire** en mode achat (`…(rec, 1, 1)`), échec → `MsgString 0x6C1`.

Dans tous les cas la séquence se termine par `SendMsg(gameMode, 18, …)` = **fin du drag**
(cmd `0x12` = désélection, identique au shop NPC).

Côté `UIMerchantMirrorItemWnd` (msg 38) les types sont **10**, **19**, **21** — le retour de
l'objet vers la liste « disponibles » (`VendingShop_PrepareRemoveItem` + `VendingShop_RemoveItem`).

### msg 123 — tags

En-tête de 6 octets, boucle `{ tag:2, ?:4 }` :

| tag ouvrant | tag suivant | Effet |
|---|---|---|
| `21300` (`0x5334`) | `21301` (`0x5335`) | `MakeWindow(42 = 0x2A)` — miroir **vente** |
| `21350` (`0x5366`) | `21351` (`0x5367`) | `MakeWindow(175 = 0xAF)` — miroir **achat** |

Sur `UIMerchantMirrorItemWnd` : `21400` / `21401` (`0x5398` / `0x5399`) → `msg 23` (refresh).

---

## 6. Le chemin « OK » (cmd 184) en détail

1. `sub_A38C60(g_Account_Aid)` → si vrai, message de chat `MsgString 0xBA7` et abandon.
2. Si `dword_131F764` ou `dword_131F75C` non nuls → modale `MsgString 0x24B` et abandon.
3. En vente : `UIMerchantShopMakeWnd_CommitPriceEditAndSetFocus(-1)` ; renvoie 0 → abandon.
4. Achat sans aucun objet → `MsgString 0x6BC`.
5. Boucle sur les edits **prix** :
   - texte non numérique → `MsgString 0x25A` ;
   - négatif → 0, `> 0x7FFFFFFF` → clamp ;
   - vente : `VendingShop_GetAt(&rec, i)`, si `rec+0x04 == 0` abandon silencieux, si le
     champ de validité est nul → `MsgString 0x25B` ;
   - prix nul → confirmation `MsgString 0x25C` (bouton **187** = Oui) ;
   - `g_VendingSafeCheckEnabled == 0` **et** prix `> 1 000 000 000` → confirmation `MsgString 0x9A3` ;
   - achat : prix 0 → `MsgString 0x6BD` ; prix `> 0x5F5B9F0` (99 999 984) → `MsgString 0x6BE`.
6. Achat : boucle sur les edits **quantité** — 0 → `MsgString 0x6BF` ;
   `quantité_déjà_posée + saisie > 9999` → `MsgString 0x6C0`.
   Puis la limite de zeny `+0x128` doit être `<= g_PlayerZeny` (sinon chat `MsgString 0xE63`)
   et `<= 0x7FFFFFFF` (sinon `MsgString 0x74E`).
7. Nom de boutique vide → `MsgString 0xE1` ; mot interdit (`BannedWord_ScanClean`, table
   `0x0159C2C8`) → `MsgString 0xE`.
8. Poussée des valeurs dans la session : `VendingShop_SetPriceAt(i, prix)` pour chaque ligne,
   et `BuyingStore_SetAmountAt(i, qté)` en mode achat.
9. Vente : contrôle d'**overflow zeny** — `zeny + Σ(qté × prix) > 0x7FFFFFFF` → `MsgString 0xEF2`.
10. Envoi : `CMode::SendMsg(cmd, nomBoutique, 1, …)` avec **cmd 82** (vente) ou **cmd 271**
    (achat, + la limite de zeny en 64 bits). Le **cancel** passe par les mêmes commandes avec
    l'argument `0`, suivi de `SaveWindowRect(41)` / `SaveWindowRect(174)`.
11. Mémorisation du nom dans `g_VendingSnapshot_ShopName` (`0x016023E4`).

Si `+0x188 == 1`, le OK n'envoie rien tout de suite : il entre en **mode « choisir
l'emplacement »** — voir §6bis.

### La case « Safe check for over 10 mil zeny »

`g_VendingSafeCheckEnabled` (`0x015FFFA1`, persistant) **inverse la stratégie de contrôle** :

- **cochée** → contrôle **à chaque changement de champ** dans
  `UIMerchantShopMakeWnd_CommitPriceEditAndSetFocus` : tout prix `>= atoi(MsgString 0x9AC)`
  (le seuil « 10 mil » de l'étiquette) ouvre une confirmation qui **nomme l'objet** et épelle
  le montant, phrase assemblée depuis `MsgString 0x9A9` + `0x9AB` (si `prix / atoi(0x9AA) > 0`)
  + `0x9AD` + `0x9AE` ;
- **décochée** → aucun contrôle par champ, seulement **une** confirmation au moment du OK si un
  prix dépasse `1 000 000 000`.

---

## 6bis. `+0x188` — le mode « échoppe posée sur une case choisie »

Ce champ est un **mode alternatif complet**, activé par le serveur. Les trois octets/shorts
qui le suivent forment un petit automate :

| Offset | Type | Rôle |
|---|---|---|
| `+0x188` | `byte` | **mode placement disponible** (0 = comportement classique) |
| `+0x189` | `byte` | **sélection en cours** : les deux fenêtres sont cachées, on attend un clic au sol |
| `+0x18A` | `short` | **case X** choisie (`-1` = aucune) |
| `+0x18C` | `short` | **case Y** choisie |

### Qui l'active

**Une seule instruction dans tout le binaire** écrit `+0x188 = 1` : `0x0095AADA`, dans
`VendingWnd_EnablePlacementMode` (`0x0095AAD0`). Et cette fonction n'a que **deux appelants**,
tous deux des handlers de paquets :

| Paquet | Handler | Effet |
|---|---|---|
| **ZC `0x0A7E`** (var, ≥5) | `RecvHandler_OpenVendingWithPlacement` `0x00D0A090` | `MakeWindow(0x29)` **+ mode placement** |
| **ZC `0x0A93`** (fixe, 3) | `RecvHandler_OpenBuyingStoreWithPlacement` `0x00D09FB0` | `MakeWindow(0xAE)` **+ mode placement** |

Les paquets **hérités** `ZC_OPENSTORE 0x012D` et `0x0810` sont traités **en ligne** dans
`RecvLoop_DispatchPackets` (cases 301 et 0x0810) : ils écrivent le compteur d'emplacements et
c'est tout. Ils ne passent jamais par `VendingWnd_EnablePlacementMode` → **`+0x188` reste à 0**.

C'est exactement ce qui a été observé en live : le serveur moonlight envoie la forme héritée,
donc les deux configurations testées avaient `+0x188 = 0`.

Structure des deux paquets (le buffer `0x015E819A` pointe sur le **payload**, opcode exclu) :

```
ZC 0x0A7E : { len:u16, slots:u8, itemIndex:u16 × (len-5)/2 }
ZC 0x0A93 : { slots:u8 }
```

Le tableau d'indices de `0x0A7E` est consommé par `VendingAvail_FillFromIndexList`
(`0x00DA9BF0`) : il **vide puis pré-remplit la liste des objets disponibles** à partir des
indices fournis par le serveur (liste `session+0x172C`, taille `+0x1730`, nœuds de `0x100` o).
Le mode achat (`0x0A93`) ne transporte pas de liste.

### Le déroulé

1. Le serveur envoie `0x0A7E` / `0x0A93` → fenêtres créées, `+0x188 = 1`.
2. Le joueur compose son échoppe normalement, puis clique **OK** (cmd 184).
3. Au lieu d'envoyer l'ouverture, `OnMsg` appelle `VendingWnd_SetPickingPlacement(this, 1)` :
   `+0x189 = 1`, **les deux fenêtres sont masquées**, et un message de chat `MsgString 0xB72`
   invite à cliquer au sol.
4. `GameMode_GroundClick_RequestMove` (`0x00C75AA0`) voit `+0x189 == 1` et **surligne la cellule
   sous le curseur** (couleur `0x66FF0000`), en masquant le curseur normal.
5. Clic gauche → `VendingWnd_SetPlacementCell(x, y)` (écrit `+0x18A`/`+0x18C`), les fenêtres
   réapparaissent, et une modale `MsgString 0xB63` demande confirmation :
   - **Oui** (bouton 187) → `VendingWnd_SendOpenAtPlacement` → `CMode::SendMsg` **cmd 296**
     (vente, nom) ou **cmd 298** (achat, nom + limite de zeny) ;
   - **Non** → `VendingWnd_SetPickingPlacement(this, 1)` : on retourne choisir une case.
6. Tant qu'une case valide est mémorisée (`VendingWnd_HasPlacementCell` : `x >= 0 && y >= 0`),
   le marqueur **reste dessiné** sur la carte à chaque frame, même hors sélection.
7. Refus serveur (`VendingWnd_HandleOpenStoreAck`, résultats 1/2/3) →
   `VendingWnd_SetPickingPlacement(wnd, 0)` : on sort proprement du mode.

### Récapitulatif des commandes `CMode::SendMsg`

| | Ouvrir (normal) | Ouvrir (sur case) | Annuler (mode placement) |
|---|---|---|---|
| Vente | **82** | **296** | **297** |
| Achat | **271** | **298** | **299** |

En mode normal, l'annulation passe par la même commande d'ouverture avec l'argument `0`.

### Effet de bord dans `DrawContent`

Toujours quand `+0x188 == 1`, le pied de fenêtre affiche « `Max Weight : %3d` » au lieu de
« `Weight : %3d / %3d` » — cohérent avec une échoppe **détachée du personnage** : c'est la
capacité de l'étal qui compte, plus le poids porté.

### Globals des fenêtres

| Adresse | Nom | Contenu |
|---|---|---|
| `0x0131F7E4` | `g_VendingShopMakeWnd` | `UIMerchantShopMakeWnd` 0x29 |
| `0x0131F7E0` | `g_VendingMirrorWnd` | `UIMerchantMirrorItemWnd` 0x2A |
| `0x0131F8F4` | `g_BuyingStoreShopMakeWnd` | `UIMerchantShopMakeWnd` 0xAE |
| `0x0131F8F8` | `g_BuyingStoreMirrorWnd` | `UIMerchantMirrorItemWnd` 0xAF |

---

## 7. API côté session (`g_UIWindowContextKey` = `g_session` `0x015FA3C0`)

Le même objet que le « panier » du shop NPC (le symbole Ghidra `g_SkillInfoMgr` est un
*misnomer*). Fonctions renommées dans l'IDB :

| Adresse | Nom | Rôle |
|---|---|---|
| `0x00D5CE40` | `VendingShop_GetCount` | nombre d'objets **posés en échoppe** |
| `0x00D5BEA0` | `VendingShop_GetAt(&rec, i)` | l'objet posé n° `i` |
| `0x00D5CE60` | `VendingAvail_GetCount` | nombre d'objets **disponibles** |
| `0x00D5C160` | `VendingAvail_GetAt(&rec, i)` | l'objet disponible n° `i` |
| `0x00D5BF40` | `VendingShop_GetPlacedAmountBySrcIndex` | quantité déjà posée pour un index source |
| `0x00D5C200` | `VendingShop_ContainsSrcIndex` | présence |
| `0x00D5BE10` | `VendingShop_FindBySrcIndex` | recherche → enregistrement |
| `0x00D5C200` | `VendingAvail_GetAmountBySrcIndex` | **reste à poser** pour un index source |
| `0x00D54C40` | `VendingAvail_AddOrMergeItem`… voir ci-dessous | |
| `0x00D57C60` | `VendingAvail_ConsumeItem` | |
| `0x00D54D80` | `VendingAvail_AddOrMergeItem` | |
| `0x00D57AC0` | `VendingShop_ConsumeItem` | |

### ⚠ Les quatre mutateurs — piège de nommage

Ces fonctions vont **par paires**, et celle qui ressemble à un test n'en est pas un :

| Adresse | Signature | Ce qu'elle fait vraiment |
|---|---|---|
| `0x00D54C40` | `VendingShop_AddOrMergeItem(s, rec, no_merge, refresh)` | **AJOUTE** à la liste échoppe (`+0x1748`), ou **cumule** `nœud+0x18 += rec+0x10` si l'index source y est déjà. Renvoie **1 = nouveau nœud, 0 = fusionné** |
| `0x00D57C60` | `VendingAvail_ConsumeItem(s, rec, whole_node, refresh)` | **DÉCRÉMENTE** la liste des disponibles (`+0x172C`) de `rec+0x10` |
| `0x00D54D80` | `VendingAvail_AddOrMergeItem(s, rec, no_merge, refresh)` | rend l'objet au stock (symétrique du premier) |
| `0x00D57AC0` | `VendingShop_ConsumeItem(s, rec, whole_node)` | retire de l'échoppe |

**Poser** = `VendingShop_AddOrMergeItem` **puis** `VendingAvail_ConsumeItem`.
**Retirer** = `VendingAvail_AddOrMergeItem` **puis** `VendingShop_ConsumeItem`.

En vente, le handler de dépôt natif enchaîne les deux **sans tester le retour du
premier** — et il faut faire pareil. Conditionner le second appel au retour du
premier désynchronise les deux listes : au deuxième dépôt du même objet, la
fusion renvoie 0, le stock n'est jamais décrémenté, et l'objet s'empile
indéfiniment (quantités à six chiffres pour un lot de 120). En échoppe d'achat,
en revanche, un retour 0 signifie « déjà présent » et le natif refuse.

Les deux listes vivent dans la session : **`+0x172C` = disponibles**,
**`+0x1748` = échoppe**. Nœud : `+0x0C` index source, `+0x18` quantité.
| `0x00D77140` | `VendingShop_SetPriceAt(i, prix)` | prix de la ligne `i` |
| `0x00D76990` | `BuyingStore_SetAmountAt(i, qté)` | quantité de la ligne `i` |
| `0x00D56340` | `VendingShop_Clear` | vide la liste posée |
| `0x004C9DA0` | `Vending_GetTaxPercent` | pourcentage de taxe (entier) |
| `0x00D589F0` | `Cstr_IsNumericPriceString` | numérique, séparateurs de milliers tolérés |

Mode placement (§6bis) : `VendingWnd_EnablePlacementMode 0x0095AAD0`,
`VendingWnd_SetPickingPlacement 0x0095A650`, `VendingWnd_SetPlacementCell 0x0095A610`,
`VendingWnd_HasPlacementCell 0x0093A3C0`, `VendingWnd_SendOpenAtPlacement 0x0093A3E0`,
`VendingAvail_FillFromIndexList 0x00DA9BF0`, `VendingAvail_Clear 0x00D56380`,
`VendingWnd_HandleOpenStoreAck 0x00CFB4E0`.

Non identifiées avec certitude, laissées telles quelles : `sub_D566F0`, `sub_D73990`,
`sub_D73B50` (initialisation de la liste « disponibles » selon le mode, appelées par
`UIMerchantMirrorItemWnd::OnCreate`), `sub_D57E40`, `sub_D765B0`, `sub_D765F0`.

### Globals

| Adresse | Nom | Contenu |
|---|---|---|
| `0x015FB2E4` | `g_VendingMaxSlots` | emplacements de vente — écrit par **ZC_OPENSTORE `0x012D`** |
| `0x015FB2E8` | `g_BuyingStoreMaxSlots` | emplacements d'achat |
| `0x015FFFA1` | `g_VendingSafeCheckEnabled` | état de la case à cocher |
| `0x016023E4` | `g_VendingSnapshot_ShopName` | dernier nom de boutique (pour Import) |
| `0x01602400` / `0x01602404` | `g_VendingSnapshot_begin` / `_end` | vecteur `ItemSkillInfo`, pas **0xF8** ; `elem+0x10` = quantité, `elem+0x14` = prix, `elem+0x2C` = itemId en texte |
| `0x015FBA90` | `g_PlayerZeny` | zeny (déjà connu) |
| `0x015FBA9C` / `0x015FBAA0` | poids max / courant | |

---

## 8. Table de chaînes (`MsgStringTable_GetById`, `0x00A9ED30`)

| id | Usage observé |
|---|---|
| `0x0E` | mot interdit dans le nom |
| `0xE0` | titre « Opening a stall » (vente) |
| `0xE1` | nom de boutique vide |
| `0xE8` | titre « Available Items for Vending » |
| `0x11A` | étiquette « Name » |
| `0x171` | « Price: » (vente) |
| `0x24B` | action impossible (autre échoppe ouverte) |
| `0x24C` | « ea » (unité de quantité, achat) |
| `0x25A` | saisie non numérique |
| `0x25B` | objet invalide sur la ligne |
| `0x25C` | confirmation prix nul |
| `0x6A9` | plafond de quantité 30000 |
| `0x6B8` | titre « Buying Store Window » |
| `0x6B9` | « Price: » (achat) |
| `0x6BA` | « Money » |
| `0x6BB` | « Purchase Zeny Limit » |
| `0x6BC` | aucun objet sélectionné (achat) |
| `0x6BD` / `0x6BE` | prix nul / prix trop élevé (achat) |
| `0x6BF` / `0x6C0` | quantité nulle / quantité > 9999 |
| `0x6C1` | ajout impossible (achat) |
| `0x6C5` | titre « Available items: » (miroir achat) |
| `0x6CA` | titre du miroir, 3e variante |
| `0x9A3` | confirmation prix > 1 000 000 000 |
| `0x9A9` / `0x9AB` / `0x9AD` / `0x9AE` | fragments de la phrase de confirmation « safe check » |
| `0x9AA` / `0x9AC` | unités numériques de cette phrase (`0x9AC` = seuil « 10 mil ») |
| `0x9AF` | étiquette « Safe check for over 10 mil zeny » |
| `0x9BE` | aucun objet posé (vente) |
| `0xC96` | « Total amount : » |
| `0xC97` / `0xC98` | avertissement rouge en bas (vente / achat) |
| `0xE63` / `0x74E` | limite de zeny > solde / > 0x7FFFFFFF |
| `0xEF2` | dépassement de la limite de zeny à la vente |
| `0xBA7` | interdiction d'ouvrir une échoppe |
| `0xB72` | invite « cliquez la case où poser l'échoppe » (mode placement) |
| `0xB63` | confirmation de la case choisie (mode placement) |
| `0x7A2` / `0x38` | refus serveur, résultats 3 et 8 |

---

## 9. Couche paquet

Relevé depuis `docs/opcode_map.csv` (table de dispatch du client). **Non re-vérifié
paquet par paquet en live** — à confirmer côté serveur moonlight avant de s'en servir.

### Vente

| Opcode | Sens | Taille | Nom |
|---|---|---|---|
| `0x012D` | ZC | 4 | `ZC_OPENSTORE` — nombre d'emplacements → `g_VendingMaxSlots` |
| `0x012E` | CZ | 2 | fermeture de l'échoppe |
| `0x012F` | CZ | var (84) | ouverture (ancienne forme) |
| `0x01B2` | CZ | var (85) | **ouverture** (forme moderne : `nom[80]`, `flag`, liste) |
| `0x0130` | CZ | 6 | demande de la liste d'un vendeur |
| `0x0131` | ZC | 86 | `ZC_STORE_ENTRY` — pancarte au-dessus du vendeur |
| `0x0132` | ZC | 6 | `ZC_DISAPPEAR_ENTRY` |
| `0x0133` / `0x0800` | ZC | var | `ZC_PC_PURCHASE_ITEMLIST_FROMMC` — vue acheteur |
| `0x0134` / `0x0801` | CZ | var | `CZ_PC_PURCHASE_ITEMLIST_FROMMC` — achat |
| `0x0135` | ZC | 7 | `ZC_PC_PURCHASE_RESULT_FROMMC` |
| `0x0136` | ZC | var | `ZC_PC_PURCHASE_MYITEMLIST` — vue vendeur |
| `0x0137` | ZC | 6 | `ZC_DELETEITEM_FROM_MCSTORE` |
| `0x0A7E` | ZC | var (≥5) | ouverture **avec mode placement** + liste d'indices pré-remplie (§6bis) |

### Achat

| Opcode | Sens | Taille | Nom |
|---|---|---|---|
| `0x0810` | ZC | 3 | ouverture autorisée (emplacements) |
| `0x0812` | ZC | 8 | échec d'ouverture côté acheteur |
| `0x0813` | ZC | var | `ZC_MYITEMLIST_BUYING_STORE` |
| `0x0814` | ZC | 86 | `ZC_BUYING_STORE_ENTRY` — pancarte |
| `0x0816` | ZC | 6 | `ZC_DISAPPEAR_BUYING_STORE_ENTRY` |
| `0x0818` | ZC | var (16) | `ZC_ACK_ITEMLIST_BUYING_STORE` |
| `0x081A` | ZC | 4 | échec de transaction |
| `0x081B` | ZC | 12 | mise à jour d'un objet |
| `0x081C` | ZC | 10 | suppression d'un objet |
| `0x0A93` | ZC | 3 | ouverture **avec mode placement** (§6bis) |

Les CZ correspondants (`0x0811` ouverture, `0x0815` fermeture, `0x0817` clic, `0x0819` achat)
ne figurent pas dans la table de dispatch **reçue** — normal, elle ne liste que les ZC ;
côté client ils sont construits par `CMode::SendMsg`.

---

## 10. Notes pour une conversion ImGui

- **Ne rien réimplémenter du protocole** : l'ouverture passe par le bus
  `CMode::SendMsg` (`[0x0121333C]` → vtable`+0x18` → `0x00C86740`), commandes **82** (vente) et
  **271** (achat) avec `(nom, 1)`. C'est ce chemin qui pose l'état client, pas seulement le
  paquet. Le **cancel** = les mêmes commandes avec `0` (même logique que le `cmd 0x28` du
  shop NPC).
- **Les deux modes sortent du même code** → une seule fenêtre ImGui à deux modes, comme le
  couple Acheter/Vendre de `shop_tweaks`.
- Le contenu se lit **sans paquet** : `VendingShop_GetCount` / `GetAt` et
  `VendingAvail_GetCount` / `GetAt` sur `g_session`, ou directement les `std::list`
  `+0x148` (posés) et `+0xE8` (disponibles). Rappel : le nom d'objet est l'**id en texte**
  à `nœud+0x34`.
- Le poser/retirer se fait par `VendingShop_AddItem` / `VendingShop_RemoveItem`, les prix par
  `VendingShop_SetPriceAt`, les quantités par `BuyingStore_SetAmountAt` — inutile de simuler
  du drag & drop.
- **Masquage du natif** : par window id (`0x29`, `0x2A`, `0xAE`, `0xAF`) dans le hook
  `MakeWindow`, comme `shop_tweaks`. Ne jamais déplacer une fenêtre hors écran
  ([`feedback_no_offscreen_hide`]).
- Penser au **plafond serveur** : `g_VendingMaxSlots` / `g_BuyingStoreMaxSlots`, et à la
  taxe `Vending_GetTaxPercent` pour afficher le net.
- La fonctionnalité **Import** (cmd 560) n'est PAS locale : elle passe par le
  **service web** (voir §11), pas par le protocole de jeu.
- **Le mode placement (§6bis) peut être ignoré** dans un premier temps : moonlight envoie les
  paquets hérités (`0x012D` / `0x0810`), donc `+0x188` vaut 0 et le OK part directement en
  cmd 82/271. Mais si le serveur passe un jour à `0x0A7E` / `0x0A93`, il faudra gérer le
  masquage des fenêtres, le clic au sol et les cmd 296/298 — d'où l'intérêt de tester
  `+0x188` plutôt que de câbler 82/271 en dur.

---

## 11. Le bouton « Import » — sauvegarde d'échoppe par service **web**

*« Get the item information registered in the previous stall »*, cmd **560**.

Contrairement à tout le reste de cette fenêtre, cette fonction ne passe **ni par le
protocole de jeu, ni par un fichier local** : c'est un **AsyncWork libcurl** vers le
service web du client (`AssistAddr` d'ExternalSettings, cf.
[`project_web_api_asyncwork_re`]).

| Sens | Fonction | AsyncWork |
|---|---|---|
| Charger | `VendingSnapshot_LoadForChar 0x005E3980` → `0x0056BAF0` | `MerchantStoreInformation_Load` (`CMCStoreInfoBackupLoadAsyncWork`) |
| Sauver | `0x005E3A50` → `0x0056BC30` | pendant symétrique, déclenché à la **fermeture** de la boutique |

Paramètres postés : `account_id` (`g_Account_Aid`), `char_id` (`g_Own_CharId`),
`world_name` (`g_WorldName`), `store_type` (le mode : 0 = vente, 1 = échoppe
d'achat) et une clé constante **`b8e5c779ed77e055`** (`0x0160241C`).
La sauvegarde sérialise l'échoppe en **JSON** (jsoncpp, `Json::StyledWriter`).

Côté serveur, ça atterrit dans la table **`merchant_configs`** :
`world_name` · `account_id` · `char_id` · `store_type` (clé primaire composite) ·
`data` (`longtext`).

Format de `data` (relevé sur base réelle) :

```json
{ "data": {
    "Title": "dfghdfgh",
    "Item": [ { "ID": 714, "Count": 10, "Price": 500, "Identified": true,
                "RefiningLevel": 0, "GradeLevel": 0,
                "Slot_1": 0, "Slot_2": 0, "Slot_3": 0, "Slot_4": 0,
                "RandomOptionCount": 0, "RandomOptionList": null } ] } }
```

Au retour, le client remplit `g_VendingSnapshot_ShopName` (← `Title`) et le vecteur
`g_VendingSnapshot_begin..end` (← `Item[]`), puis `ImportSavedShop` repose les
objets et recopie prix/quantités dans ses edits.

⚠ **Le JSON ne porte aucun index de cart, seulement des `ID` d'objet.** Le client
saute donc les entrées dont l'objet n'est plus disponible : une UI de remplacement
qui recale ses prix sur l'**indice de ligne** les décalerait tous. Il faut apparier
par **item id**.

### 🔴 Le snapshot est VIDÉ par l'import

`UIMerchantShopMakeWnd_ImportSavedShop 0x00936CE0` s'en fait une **copie locale**
(`ItemSkillInfo_CopyFull`, pas de 248 o), remplit ses edits — `element+0x14` dans les
edits de prix (`this+0xE0+4i`), `element+0x10` dans ceux de quantité en mode achat —
puis termine par **`VendingSnapshot_Clear 0x005E2A60`** :

```c
v1 = this + 22;               // 0x016023A8 + 0x58 = g_VendingSnapshot_begin
sub_5BCD70(this[22], this[23]);
v1[1] = *v1;                  // end = begin  ->  vecteur VIDE
```

Vérifié en mémoire live après un import : `begin == end == 0x22e1fe98`.

**Conséquence** : tout code externe doit lire le vecteur **AVANT** de déclencher la
cmd 560. Le lire après ne ramène que des zéros — alors que
`g_VendingSnapshot_ShopName` (`0x016023E4`), lui, **n'est pas effacé** et se lit encore
très bien. D'où un symptôme trompeur : *les objets et le nom sont importés, les prix
restent à 0*.

---

## 12. Côté acheteur : `0x2B` (offre) + `0x2C` (panier)

Ouvertes ensemble en cliquant sur l'échoppe d'un autre joueur.

### `UIMerchantItemPurchaseWnd` — id `0x2C`, vtable `0x0103D2B0`

OnCreate `0x00940850`, DrawContent `0x00947810`, OnMsg `0x009566B0`. 280×184.

| Offset | Contenu |
|---|---|
| `+0xC4` | bouton **buy** (mode 0) / **sell** (mode ≠ 0) — cmd **184** |
| `+0xC8` | bouton **cancel** — cmd **185** |
| `+0xDC` | lignes visibles = `(H − 38) / 32` ; `+0xEC` nombre de lignes |
| `+0xE8` | **liste panier** |
| `+0xF0` | marge (H = `+0xF0` + 32 × lignes) |
| `+0xF4` | label « Total : %s Zeny » |
| `+0xF8` | **AID du vendeur** (posé par `OnMsg 28`) |
| `+0x100` | index sélectionné (**−1** = aucun) |
| `+0x104` | **UniqueID de l'échoppe** (posé par `OnMsg 28`) |
| `+0x108` | mode : **0 = j'achète**, ≠ 0 = échoppe d'achat (je vends) |

Bornes de redimensionnement (`OnMsg 14`, `p3 == 3`) : W ∈ [232, 320], H ∈ [120, 336],
grille de 32 sur une base 280×120.

### ⚠ Deux prix par ligne

`DrawContent` est formel :

```c
v27 = node[7];            // +0x1C = prix de BASE
v28 = node[8];            // +0x20 = prix EFFECTIF (Discount appliqué)
if (v27 == v28)  " %s Zeny"          // affiche le prix
else             " %d -> %s Zeny"    // affiche "base -> effectif"
```

et le total natif se calcule sur l'**effectif** (`qty × node+0x20`). Une UI de
remplacement qui lit `+0x1C` annonce un prix trop élevé et un total faux dès qu'un
Discount s'applique. Les autres vues de la famille n'exposaient pas le piège : les
deux champs y coïncident.

Layout de nœud complet (identique à toute la famille, `ItemSkillInfo` à `+0x08`) :

| Nœud | ItemSkillInfo | Contenu |
|---|---|---|
| `+0x0C` | `+0x04` | **index d'échoppe** — c'est lui qui part dans le paquet |
| `+0x18` | `+0x10` | quantité |
| `+0x1C` | `+0x14` | prix de base |
| `+0x20` | `+0x18` | prix effectif |
| `+0x34` | `+0x2C` | `std::string` = l'itemId **en texte** (`"714"`) |
| `+0x65` | `+0x5D` | drapeau œuf d'animal (ligne sautée si `IsPetEggItem`) |
| `+0x90` | `+0x88` | slots (`short`) |

### Le bouton « buy » (cmd 184)

En mode 0 il **n'envoie rien directement** : il ouvre la fenêtre de confirmation
`MakeWindow(0xE6)` puis appelle `UIMerchantPurchaseConfirmWnd_SetContext 0x0095CC40`
avec `(AID, UniqueID)` ; celle-ci somme le panier et affiche le total suivi de
`MsgString 0x918`. En mode ≠ 0, il dispatche directement `CMode::SendMsg 273`.

`cancel` (185) est la vraie fermeture : `SaveWindowRect(0x2B, 0x2C)`, `SendMsg 40`,
puis `VendingBasket_Clear`.

### API panier (session `g_UIWindowContextKey`)

`__thiscall`, `this = &g_UIWindowContextKey` (l'**adresse** `0x015FA3C0`), conventions
relues sur les sites d'appel réels :

| Adresse | Rôle |
|---|---|
| `0x00D5CE80` | `VendingBasket_GetCount(session)` |
| `0x00D5C580` | `VendingBasket_GetAt(session, buf, index)` |
| `0x00D54EA0` | `VendingBasket_AddOrMergeItem(rec)` |
| `0x00D57E40` | `VendingBasket_RemoveItem(rec)` |
| `0x00D56300` | `VendingBasket_Clear(session)` |
| `0x00D5C730` | `VendingBasket_GetTotalPrice(session)` → `__int64` |
| `0x00D5CE90` / `0x00D5C780` | `VendingOffer_GetCount` / `GetAt` (l'offre du vendeur) |

### Paquet d'achat — `CZ_PC_PURCHASE_ITEMLIST_FROMMC` `0x0801`

Relevé sur le constructeur natif (autour de `0x00C8E4C0`) :

```
+0  opcode:2 = 0x0801
+2  len:2                     ; len = 12 + 4*n, REFUSÉ au-delà de 0x800
+4  AID:4                     ; 0x2C +0xF8
+8  UniqueID:4                ; 0x2C +0x104
+12 [ amount:u16, index:u16 ] * n   ; index = nœud+0x0C (index d'ÉCHOPPE)
```

Le pendant « échoppe d'achat » est **CZ `0x0819`**, avec des entrées de **8 octets** et
un itemId obtenu par `atoi` sur la chaîne du nœud — **non décodé**, d'où le repli sur
l'UI native dans ce mode.

### 🔴 Aucun rafraîchissement après achat

Le serveur (`vending_purchasereq`, moonlight) ne renvoie à l'acheteur que
`clif_buyvending` = **ZC `0x0135`** `{index:2, amount:2, result:1}` — **pas la liste**.
Résultats : `0` succès, `1` pas de zeny, `2` surcharge, `4` rupture, `5` en échange,
`6` échoppe incorrecte, `7` pas d'info de vente.

Le client natif esquive le problème en **fermant l'échoppe** dès le clic « buy »
(cmd 184 → `SaveWindowRect(0x2B)`/`(0x2C)`, qui ferme). Le commentaire rAthena le
confirme : « *Close Vending (this was automatically done by the client)* ».

Pour garder la fenêtre ouverte et enchaîner les achats, il faut donc **redemander la
liste** : **CZ_REQ_BUY_FROMMC `0x0130`** `{AID:4}` (6 octets, `clif_parse_VendingListReq`
→ `vending_vendinglistreq` → `clif_vendinglist`). TCP garantit l'ordre, donc envoyée
juste après l'achat elle ramène déjà la liste d'après.

Note d'index : le serveur fait `idx -= 2` à la réception et `client_index(idx)` = `idx+2`
au retour — l'index lu dans le nœud (`+0x0C`) est donc **directement** celui à envoyer,
et celui qui revient dans `0x0135`.

---

## 13. 🔴 `0x00A2E770` **détruit** la fenêtre

Le symbole s'appelait `UIWindowMgr_SaveWindowRect` — nom trompeur. Elle sauve bien la
position et la taille, **puis appelle `UIWindowMgr_QueueDestroyWindow 0x00A447D0`**.
Renommée **`UIWindowMgr_SaveRectAndCloseWindow`**. Tous les `SaveWindowRect(id)` qui
parsèment les `OnMsg` de cette famille sont donc des **fermetures**.

### Ce que ça implique pour l'historique des ventes

`UIMerchantItemLogWnd` (`0x101` / `0x102`) n'est pas créée à la fermeture du shop :
elle existe **dès la première vente** et sert d'**accumulateur**. Son `OnMsg 23`
**AJOUTE** une ligne — il ne reconstruit rien depuis une liste de session, contrairement
au reste de la famille. Le `cmd 201` de « My Shop » se contente de la faire remonter :
`MakeWindow(0x101)` rend l'instance existante.

**Donc : ne jamais déclencher sa `cmd 201` pendant que le shop tourne.** Ça détruit la
fenêtre, et avec elle tout l'historique — silencieusement. Une UI de remplacement ne
doit afficher son panneau qu'une fois le shop terminé.

---

## 13bis. ⚠ `UIMerchantItemLogWnd` : `+0x1C` est un **montant**, pas un prix unitaire

Le même offset ne dit pas la même chose selon la fenêtre de la famille :

| Fenêtre | `nœud+0x1C` |
|---|---|
| « Mon shop » `0x2D`/`0xB0`, offre `0x2B`/`0xB1` | prix **unitaire** |
| **Historique `0x101`/`0x102`** | **montant de la ligne** (`prix × qté`, déjà multiplié) |

`UIMerchantItemLogWnd_DrawContent` (`0x009645B0`) tranche : il fait
`sub_A948D0(v15[7], …)` puis `" %10s Zeny"` — il affiche `nœud+0x1C` **tel quel**, et
n'affiche d'ailleurs **aucune colonne de quantité** (ses colonnes sont *objet /
montant / horodatage*).

Vérifié sur données réelles : 50 golds achetés à 100 z donnent `+0x1C = 5000`.
Remultiplier par `nœud+0x18` gonflait chaque ligne et le total — 250 600 z affichés
pour 5 400 z réels, soit bien au-delà du budget de 10 000 z alloué au *buying store*,
ce qui rendait le récapitulatif absurde à l'œil.

Le **prix unitaire n'existe nulle part** dans cette fenêtre : il se déduit
(`montant / quantité`), en gardant la garde sur `quantité == 0`.

Note : `+0x18` reste bien la quantité, et chaque ligne porte un **horodatage** dans un
tableau parallèle (`this+0xF0`, pas de 60 octets, affiché `%02d/%02d - %02d:%02d:%02d`)
— non exploité pour l'instant.

---

## 14. Côté vendeur face à un *buying store* : `0xB1` + `0xB2` + `0xB3`

### 🔴 Correction d'une erreur de la §12

J'avais écrit que ce mode réutilisait `0x2B`/`0x2C` avec `+0x108 ≠ 0`, et le plugin
testait ce champ pour « rendre la main au natif ». **Faux, et structurellement
intestable** : le natif ouvre **trois autres identifiants**. La preuve tient dans
`UIMerchantItemPurchaseWnd_OnMsg` lui-même — ses `cmd 184`/`185` ferment
`177`/`178`/`179` (= `0xB1`/`0xB2`/`0xB3`) quand le mode vaut 1, et `43`/`44`
(= `0x2B`/`0x2C`) sinon. Le test sur `0x2C` ne pouvait jamais se déclencher.

Leçon (la même qu'en §12) : **un identifiant de fenêtre se lit sur objet vivant**,
jamais par déduction depuis un handler.

### Les trois fenêtres (RTTI relu en live, jeu *running*)

| id | Classe | vtable | Titre |
|---|---|---|---|
| `0xB1` | `UIMerchantItemShopWnd` | `0x0103D028` | « Wanted items - *acheteur* » |
| `0xB2` | `UIMerchantItemPurchaseWnd` | `0x0103D2B0` | « Selling Items » + Total + sell/cancel |
| `0xB3` | `UIMerchantMirrorItemWnd` | `0x0103D610` | « Available items: » |

Ce sont les **mêmes classes** que le mode achat (`0x2B`/`0x2C`) et que les miroirs de
composition (`0x2A`/`0xAF`) — d'où une symétrie **complète** des offsets. Rien à
re-relever : `+0xE8` liste, nœud portant l'`ItemSkillInfo` à `+0x08`, etc.

Trois globaux consécutifs les pointent, et sont **remis à 0 à la fermeture** (donc
utilisables comme détecteur) :

| Adresse | Symbole | Fenêtre |
|---|---|---|
| `0x0131F900` | `g_BuyingStoreWantedWnd` | `0xB1` |
| `0x0131F904` | `g_BuyingStoreSellListWnd` | `0xB2` |
| `0x0131F908` | `g_BuyingStoreSellMirrorWnd` | `0xB3` |

⚠ Ne pas confondre avec `0x0131F8F8` `g_BuyingStoreMakeMirrorWnd`, qui est le miroir
de la **composition** d'une échoppe d'achat (`0xAF`). Les deux existaient déjà sous le
même nom dans l'IDB — collision corrigée.

### Champs propres à ce mode

Sur `0xB1` :

| Offset | Contenu |
|---|---|
| `+0xE8` / `+0xEC` | liste des objets recherchés + taille. `nœud+0x18` = quantité **encore** voulue (déjà décrémentée des achats), `nœud+0x1C`/`+0x20` = prix |
| `+0x100` | **AID de l'acheteur** |
| `+0x108` | **fonds restants** de l'acheteur |
| `+0x10C` | `storeId` |

Sur `0xB2` : `+0xF8` AID, `+0x104` `storeId`, `+0x108` **mode = 1**.

Vérifié en live : `Price limit: 10,000 Zeny` avec 3 unités déjà achetées à 100 →
`+0x108` valait `0x25E4` = 9700, et la quantité voulue était tombée de 100 à 97.

### La troisième liste de session : `VendingBasket_*`

Ni la liste « échoppe » (`+0x1748`) ni « disponibles » (`+0x172C`) mais **`+0x1740`**.
C'est elle que `0xB2` recopie à chaque `OnMsg 23`, et elle que le bouton *sell*
sérialise.

| Adresse | Rôle |
|---|---|
| `0x00D5C580` | `VendingBasket_GetAt(session, out_rec, i)` |
| `0x00D54EA0` | `VendingBasket_AddOrMergeItem(session, rec)` — **2 arguments** |
| `0x00D57E40` | `VendingBasket_RemoveItem(session, rec)` — **2 arguments** |
| `0x00D5CE80` | `VendingBasket_GetCount` · `0x00D5C730` `GetTotalPrice` |

### ⚠ Le 3ᵉ paramètre de `VendingAvail_{Consume,AddOrMerge}Item` n'est pas un mode

Décompilées, elles s'en servent **deux fois** :

- **non nul** → on retire / rend le **nœud ENTIER**, pas seulement `rec+0x10` ;
- et il **choisit la fenêtre miroir** à re-notifier : `1` = `0xAF`, `2` = `0xB3`,
  `0` = `0x2A`.

Le natif passe **2** ici. Conséquence visible et voulue : mettre ne serait-ce qu'une
unité d'une pile en vente fait disparaître la pile **entière** des objets disponibles.
Ce n'est pas un bug — un index d'inventaire ne peut figurer qu'une fois dans la vente,
et `buyingstore_trade` **rejette toute la transaction** sur un doublon d'index.

### Les deux mouvements, dans l'ordre du natif

Dépôt (`UIMerchantItemPurchaseWnd_OnMsg` msg 38, branche mode ≠ 0) :

```c
rec.amount = clamp(voulu_restant, 30000);
VendingBasket_AddOrMergeItem(session, rec);
VendingAvail_ConsumeItem(session, rec, /*2 = nœud entier + miroir 0xB3*/ 2, 1);
OnMsg(0xB2, 23);   // la fenêtre recopie la liste de session
```

Retrait (`UIMerchantMirrorItemWnd_OnMsg` msg 38, source = panier de vente) :

```c
VendingAvail_AddOrMergeItem(session, rec, 2, 1);
VendingBasket_RemoveItem(session, rec);
OnMsg(0xB2, 23);
```

### Émission : piloter le natif, pas fabriquer le paquet

**CZ_REQ_TRADE_BUYING_STORE `0x0819`** :
`<len:2> <AID:4> <storeId:4> { <index:2> <nameId:2> <amount:2> }*` — entrées de
**6 octets** (et non 8, comme je l'avais supposé sans le vérifier).

Mais contrairement à l'achat (§12, où l'on émet `0x0801` soi-même), **ici on clique la
`cmd 184` native**. Raison : l'`index` attendu est un index d'**inventaire**
(`buyingstore_trade` fait `index - 2`), tenu par les listes de session que seul le
natif remplit. Le reconstruire à la main serait un risque pour zéro gain — la `cmd 184`
vérifie, émet, puis ferme les trois fenêtres.

Deux refus que le natif oppose en boîte de dialogue, et qu'une UI de remplacement a
tout intérêt à dire **avant** le clic :

- total > fonds restants → `MsgString 0x6CC` ;
- `zeny_joueur + total > 0x7FFFFFFF` → `MsgString 0x2B6`.

### `+0xF0` de « My Shop » (`0x2D` / `0xB0`) change de sens selon le mode

- vente → zeny **encaissé** depuis l'ouverture (part de 0, monte) ;
- *buying store* → zeny **restant** de la cagnotte (part de la limite, descend).

Un libellé « Encaissé » sur une échoppe d'achat est donc un contresens : c'est ce qu'il
reste **à dépenser**. Le mode se lit en `+0x100`.

---

## Voir aussi

- [`project_npc_shop_re`] — famille shop NPC, base `UIItemShopWnd_BaseOnMsg` partagée
- [`project_cart_window_imgui_todo`] — `UIMerchantItemWnd` id 0x28 (cart)
- [`project_trade_window_re`] — échange entre joueurs
