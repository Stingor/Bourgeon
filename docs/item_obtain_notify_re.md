# Bandeau « … - N obtained. » — RE de `UINotifyItemObtainWnd`

Client cible : `2025-07-16_Ragexe_175220998_clientinfo.exe` = `Moonlight-Destiny.exe`
(imagebase `0x400000`, build 20250716).
Serveur : fork `moonlight` de rAthena.

C'est le petit cadre blanc qui apparaît en haut de l'écran au ramassage d'un objet :

```
┌───────────────────────────────────────────┐
│  [icône]  Broken Liquor Jar - 1 obtained. │
└───────────────────────────────────────────┘
```

Tout ce qui suit est lu au désassemblage de l'IDB. Les fonctions et le `case`
de la fabrique ont été renommés et commentés dans l'IDB au passage.

⚠ Rappel projet : l'IDB est l'**exe vanilla** — les patchs WARP n'y sont pas
visibles (cf. `docs/warp_patches.md`). Rien dans ce chantier ne touche à une
zone patchée, mais le contrôle reste à faire avant toute écriture mémoire.

---

## 1. Vue d'ensemble

```
ZC_ITEM_PICKUP_ACK  (7 opcodes, cf. §5)
        │
        ▼
Recv_ZC_ITEM_PICKUP_ACK_xxxx
        │
        ├─ result != 0 && result != 3 ──► ItemPickup_ReportFailureToChat  (chat rouge, PAS de bandeau)
        │
        └─ result == 0 ou 3
             ├─ Inventory_AddOrStackItem / Inventory_UpsertOrRemoveItem
             ├─ ligne de chat  MSI_GET_ITEM  « You got %s (%d). »  en 0xFFFF00
             ├─ UIWindowMgr_MakeWindow(58)          ← LE BANDEAU
             │     ├─ OnMsg 76 (0x4C)  is_identified
             │     ├─ OnMsg 36 (0x24)  quantité + id d'objet en texte
             │     └─ OnMsg 34 (0x22)  &ItemSkillInfo
             └─ fenêtre 307 (UIMenuIconWnd) OnMsg 6, 286, 194, 1   ← clignotement de l'icône « sac »
```

Puis, **cinq secondes plus tard**, le bandeau se ferme tout seul depuis son
propre tick (§4).

Le bandeau et la ligne de chat sont **deux affichages distincts** produits par
le même handler : couper l'un ne coupe pas l'autre.

---

## 2. Identification de la fenêtre

| Élément | Valeur |
|---|---|
| Classe (RTTI) | `.?AVUINotifyItemObtainWnd@@` — descripteur `0x0123F640`, chaîne `0x0123F648` |
| Complete Object Locator | `0x010C3748` |
| vtable | `0x0103274C` (`??_7UINotifyItemObtainWnd@@6B@`) |
| **Identifiant de fenêtre** | **58 = `0x3A`** (`case 58` de `UIWindowMgr_MakeWindow`) |
| Slot dédié du manager | `g_UIWindowMgr + 0x31C` = `0x0131F804` |
| Taille de l'objet | `0x1F0` octets |
| Taille initiale | `SetSize(180, 32)` |
| Position initiale | `SetPos(Screen_CenterXFrom640(220), 75)` |

`Screen_CenterXFrom640(x)` (`0x00A4CA40`) = `x + (largeur_écran - 640) / 2` :
la position est pensée pour un écran 640 puis recentrée. À 640 de large le
cadre occupe donc x = 220..400 (centré à 10 px près), y = 75.

Le slot `+0x31C` n'est touché que par trois sites :

| Adresse | Fonction | Effet |
|---|---|---|
| `0x00A3B22E` / `0x00A3B280` | `UIWindowMgr_MakeWindow` | lecture puis écriture du pointeur |
| `0x00A2F4E3` / `0x00A2F4F9` | `UIWindowMgr_SaveRectAndCloseWindow` | lecture puis **remise à 0** |
| `0x00A436D9` | `sub_A43350` | remise à 0 (destruction en masse) |

Donc : **pas de pointeur pendant** après fermeture, et **aucun autre code du
client ne va chercher cette fenêtre**.

---

## 3. Disposition de l'objet

| Offset | Type | Rôle |
|---|---|---|
| `+0x00` | `void**` | vtable |
| `+0x14` | `int` | largeur (recalculée à chaque message 36) |
| `+0x18` | `int` | hauteur — **toujours 32**, jamais recalculée |
| `+0x1C` / `+0x20` | `int` | x / y écran (`uiwnd::kOffPosX/Y`) |
| `+0x28` | `int` | visible (`uiwnd::kOffVisible`) |
| `+0xB4` | `std::string` | **nom de ressource de l'icône** (en pratique : l'id de l'objet en texte) |
| `+0xCC` | `std::string` | texte ayant servi à **mesurer** la largeur (`MSI_GET_ITEM` formaté) |
| `+0xE4` | `DWORD` | `timeGetTime()` de l'affichage — base du minuteur de 5 s |
| `+0xE8` | `int` | mode : `0` = objet, `!= 0` = zeny |
| `+0xF0` | `ItemSkillInfo` | **la copie de l'objet réellement affichée** |
| `+0x100` | `int` | = `ItemSkillInfo::num_` (`+0x10`) — la quantité imprimée |
| `+0x1E8` | `BYTE` | `is_identified` (message 76) |

### 3.1 Précision sur `ItemSkillInfo` (= `struct ItemInfo` de `src/ragnarok/item_info.h`)

`ItemSkillInfo_SetId` (`0x006A6570`) fait littéralement :

```c
_itoa(item_id, buf, 10);
std_string_assign(this + 0x2C, buf, strlen(buf));
```

⇒ **`item_name_` (`+0x2C`) contient l'identifiant numérique de l'objet écrit en
décimal**, pas un libellé. C'est cohérent avec `BuildItemIconGrfPath`
(`0x00D5A720`) qui fait `atoi()` dessus.

Deux corrections au layout déclaré dans [item_info.h](src/ragnarok/item_info.h),
lues sur le destructeur `0x00891220` et sur les locaux de
`Recv_ZC_ITEM_PICKUP_ACK_0B41` :

* il y a une **deuxième `std::string` à `+0x44`** (24 octets), absente de la
  structure du dépôt ;
* du coup `is_identified_` est à **`+0x5C`**, `is_damaged_` à **`+0x5D`**
  (ce qui recoupe le RE de l'équipement cassé en rouge) et
  `refining_level_` à **`+0x60`**.

Les champs déclarés jusqu'à `item_name_` inclus restent justes.

---

## 4. Cycle de vie

### 4.1 Création — `UIWindowMgr_MakeWindow`, `case 58` (`0x00A3B22E`)

```asm
mov  eax, [edi+31Ch]        ; slot déjà occupé ?
test eax, eax
jnz  loc_A3A142             ; oui -> SetVisible(1) et on rend l'EXISTANTE
push 1F0h
call operator_new
call UINotifyItemObtainWnd_ctor
...
push 20h / push 0B4h        ; SetSize(180, 32)
call UIWindow_SetSize
...                          ; SetPos(Screen_CenterXFrom640(220), 75)
```

**La fabrique est idempotente.** Conséquence directe et visible en jeu :
un deuxième ramassage pendant que le bandeau est affiché **écrase** son contenu
— les notifications **ne s'empilent pas**, il n'y a qu'une seule ligne à
l'écran quoi qu'il arrive.

Autre conséquence : la position/taille de la première ouverture est conservée
tant que la fenêtre vit ; les `SetSize`/`SetPos` du `case 58` ne sont rejoués
qu'à la re-création (donc après la fermeture automatique).

### 4.2 Fermeture automatique — `UINotifyItemObtainWnd_OnTick_AutoClose5s` (`0x008BAF00`, vt+0x4C)

```c
DWORD t = timeGetTime();
if (t > this[57] /* +0xE4 */ + 5000)
    UIWindowMgr_SaveRectAndCloseWindow(g_UIWindowMgr, 58);
```

**5 000 ms en dur, non configurable**, et c'est une vraie **fermeture** :
`SaveRectAndCloseWindow` détruit la fenêtre (cf. le commentaire de
`uiwnd::CloseWindow`). Le minuteur est **remis à zéro à chaque message 36**,
donc à chaque nouvel objet ramassé.

⚠ `timeGetTime()` déborde au bout de ~49,7 jours d'uptime Windows ; la
comparaison est un `>` non signé sur la somme, donc un bandeau ouvert
exactement à cheval sur le débordement reste affiché. Cas d'école, mais c'est
bien le comportement du natif.

### 4.3 Destruction

`UINotifyItemObtainWnd_scalar_dtor` (`0x00893200`, vt+0) détruit les quatre
`std::string` (`+0xB4`, `+0xCC`, `+0x11C`, `+0x134`) puis
`UIWindow_composite_dtor`. Un second destructeur non-deletant existe à
`0x00891220` : **aucun appelant**, vestige conservé par le linkeur.

---

## 5. Protocole `OnMsg` (vt+0x94, `UINotifyItemObtainWnd_OnMsg` `0x008C5860`)

Trois messages seulement. **L'ordre compte** : le 76 doit précéder le 36, qui
lit `+0x1E8`.

| msg | `p2` | `p3` | `p4` | Effet |
|---|---|---|---|---|
| **76** (`0x4C`) | `is_identified` | — | — | `*(this+0x1E8) = (p2 != 0)` |
| **36** (`0x24`) | mode zeny (0 = objet) | quantité | `const char*` id d'objet | remplit icône + texte, **redimensionne**, `+0xE4 = timeGetTime()` |
| **34** (`0x22`) | `ItemSkillInfo*` | — | — | `ItemSkillInfo_Copy(this+0xF0, p2)` |

Tout autre message part dans `UIWindow_OnMsg_Default`.

### 5.1 Message 36, branche « objet » (`p2 == 0`) — le seul chemin utilisé

```c
std_string_assign(this + 0xB4, p4, strlen(p4));          // nom de ressource de l'icône
// ItemSkillInfo temporaire, item_name_ = p4, is_identified = *(this+0x1E8)
ItemSkillInfo_ComposeDisplayName(&tmp, &nom, 0);
Cstr_sprintf(Buffer, MsgStringTable[153 /* MSI_GET_ITEM */]);   // "You got %s (%d)."
std_string_assign(this + 0xCC, Buffer, strlen(Buffer));
```

### 5.2 Message 36, branche « zeny » (`p2 != 0`) — **code mort dans ce client**

```c
Cstr_FormatInt32Grouped(p3, tmp, 24);                    // 1 234 567
Cstr_sprintf(Buffer, MsgStringTable[327 /* MSI_GET_ZENY */]);   // "%s Zeny obtained"
std_string_assign(this + 0xCC, Buffer, strlen(Buffer));
std_string_assign(this + 0xB4, ..., 0);                  // icône VIDÉE
```

Recensement exhaustif des porteurs possibles d'un pointeur sur cette fenêtre —
`MakeWindow(58)` (14 sites, tous dans les 7 handlers de ramassage, tous avec
`p2 = 0`), `FindWindow(58)` (aucun), lecture du slot `+0x31C` (aucune hors
manager) : **aucun appelant n'envoie le message 36 en mode zeny**. La branche
existe mais n'est jamais atteinte sur ce build.

### 5.3 Le calcul de largeur (et sa bizarrerie)

```c
w = UIText_MeasureWidth(this, texte_de_+0xCC, 0, 0, 12, 0, 0) + 54;
if (w % 28 != 0) w += 28;          // NB : ajout, pas un arrondi au multiple
if (w < 34)      w = 34;
if (w != *(this+0x14))
    vt[1](this, w, *(this+0x18));  // recrée le nœud de rendu, hauteur inchangée
vt[38](this);                      // UIWindow_PaintDispatch
*(this+0xE4) = timeGetTime();
```

🔴 **La largeur est mesurée sur `MSI_GET_ITEM` (« You got X (n). ») alors que le
texte réellement dessiné est `MSI_EA_OBTAIN` (« X - n obtained. »).** Les deux
libellés n'ont pas la même longueur : le cadre du natif est donc
systématiquement mal ajusté (trop large en anglais). C'est visible sur la
capture d'origine — le blanc à droite du texte n'est pas une marge voulue.

Le `+ 28` vient de la bordure du cadre 9-slice (§6.1), et le `% 28` en est la
tentative d'alignement — mais l'implémentation ajoute 28 au lieu d'arrondir,
donc le résultat n'est presque jamais un multiple de 28.

---

## 6. Rendu — `UINotifyItemObtainWnd_OnDraw` (`0x008B5520`, vt+0x50)

### 6.1 Le cadre — `UIWindow_DrawSysBoxFrame9Slice` (`0x00871340`)

Neuf tuiles du GRF, appelées avec `(this, 0, 0, 0, 0)` (⇒ toute la fenêtre) :

```
유저인터페이스\sysbox_lu.bmp   유저인터페이스\sysbox_mu.bmp   유저인터페이스\sysbox_ru.bmp
유저인터페이스\sysbox_lm.bmp        (fond uni)               유저인터페이스\sysbox_rm.bmp
유저인터페이스\sysbox_ld.bmp   유저인터페이스\sysbox_md.bmp   유저인터페이스\sysbox_rd.bmp
```

Coins de 14×14, bords répétés par pas de 14 ⇒ **bordure totale de 28 px**.
L'intérieur est rempli à plat par `sub_A1D460` avec la couleur rendue par
`sub_7A6DF0(2, 2, ...)`. Helper **partagé** (18 sites) : c'est le cadre blanc
générique du client, pas un décor propre à ce bandeau.

### 6.2 L'icône — `UIWindow_DrawItemIconAt` (`0x008710E0`)

Dessinée en **(13, 5)** uniquement si `+0xB4` est non vide.

```c
BuildItemIconGrfPath(nom, buf);   // 유저인터페이스\item\%s.bmp
                                  // %s = ResolveItemResNameById(atoi(nom))
UITextureMgr_Load(...); UIWindow_BlitImageToNode(this, 13, 5, tex, 1);
```

Le 5e paramètre (`*(this+0x1E8)`, `is_identified`) est **inutilisé** dans cette
fonction — l'icône ne change pas selon l'identification.

### 6.3 Le texte

```c
ItemSkillInfo_BuildDisplayName(this, this+0xF0, &couleur, &morceaux,
                               &buf, &reste, &nb_morceaux, 1, 0);
n = sub_5DF6B0(buf, reste, MsgStringTable[696 /* MSI_EA_OBTAIN */], *(this+0x100));
sub_8D0AA0(this, x, 10, *(this+0x14) - 49, &morceaux, tampon, nb, couleur, 0, 0, 12);
```

* `x` = **41** en mode objet (place laissée à l'icône), **13** en mode zeny ;
* `y` = **10** ; largeur de repli = `largeur - 49` ; corps de police **12** ;
* le nom vient de la copie `ItemSkillInfo` en `+0xF0` (donc du message **34**),
  avec sa couleur de rareté / son balisage de lien ;
* le suffixe est `MSI_EA_OBTAIN` = **`" - %d obtained."`**.

🔴 **`MSI_EA_OBTAIN` commence par une espace** : le libellé complet est
« `<nom>` + ` - 1 obtained.` ». Si ce texte est un jour repris côté Bourgeon,
c'est exactement le piège d'espace de bord déjà rencontré sur le catalogue i18n
— l'espace appartient à la clé, pas à la concaténation.

---

## 7. Le chemin paquet

### 7.1 Les sept `ZC_ITEM_PICKUP_ACK`

Tous dispatchés par `RecvLoop_DispatchPackets` (table `0x00C9E2B1`, le numéro de
`case` **est l'opcode en décimal**).

| Opcode | `case` | Len | Handler | Nom IDB |
|---|---|---|---|---|
| `0x00A0` | 160 | 33 | `0x00CC3430` | `Recv_ZC_ITEM_PICKUP_ACK_00A0` |
| `0x029A` | 666 | 37 | `0x00CC28D0` | `Recv_ZC_ITEM_PICKUP_ACK_029A` |
| `0x02D4` | 724 | 39 | `0x00CC2E40` | `Recv_ZC_ITEM_PICKUP_ACK_02D4` |
| `0x0990` | 2448 | 41 | `0x00CC3B50` | `Recv_ZC_ITEM_PICKUP_ACK_0990` |
| `0x0A0C` | 2572 | 66 | `0x00CC4240` | `Recv_ZC_ITEM_PICKUP_ACK_0A0C` |
| `0x0A37` | 2615 | 69 | `0x00CC4A80` | `Recv_ZC_ITEM_PICKUP_ACK_0A37` |
| `0x0B41` | 2881 | 70 | `0x00CC5320` | `Recv_ZC_ITEM_PICKUP_ACK_0B41` |

Les sept ont **la même fin de parcours** : chat + `MakeWindow(58)` + les trois
`OnMsg` + clignotement de l'icône de menu. Un futur remplacement doit donc
couvrir les sept, pas seulement celui que le serveur émet aujourd'hui.

### 7.2 Champs lus (`0x0B41`, le plus complet)

Relevé sur les affectations de `Recv_ZC_ITEM_PICKUP_ACK_0B41` ; la colonne de
droite est l'offset dans l'`ItemSkillInfo` construit.

| Offset paquet | Taille | → `ItemSkillInfo` | Rôle |
|---|---|---|---|
| `+0x02` | `u16` | `+0x04` | **index d'inventaire** |
| `+0x04` | `u16` | `+0x10` (`num_`) | **quantité** — aussi `p3` du message 36 |
| `+0x06` | `u32` | `+0x2C` (`item_name_`) | **id d'objet**, via `ItemSkillInfo_SetId` (⇒ écrit en décimal) |
| `+0x0A` | `u8` | `+0x5C` | **`is_identified`** — aussi `p2` du message **76** |
| `+0x0B` | `u8` | `+0x5D` | `is_damaged` |
| `+0x0C` | 4 × `u32` | `+0x1C`…`+0x28` | **cartes** (`slot_[4]`) |
| `+0x1C` | `u32` | `+0x08` | position d'équipement (`location`) |
| `+0x20` | `u8` | `+0x00` | `item_type_` |
| `+0x21` | `u8` | — | **`result`** (voir ci-dessous) |
| `+0x22` | `u32` | `+0x68` | date d'expiration (`delete_time_`) |
| `+0x26` | `u16` | `+0x64` | `is_yours_` |
| `+0x28`…`+0x40` | 5 × 5 o | `+0x98` | options aléatoires (`{u16 id, u16 valeur, u8 param}`) ; le nombre retenu est le **compte d'entrées d'id non nul**, puis les entrées sont recopiées séquentiellement depuis la première |
| `+0x41` | `u8` | `+0x74` | non identifié |
| `+0x42` | `u16` | `+0x70` | non identifié |
| `+0x44` | `u8` | `+0x60` | **niveau d'affinage** (`refining_level_`) |
| `+0x45` | `u8` | `+0x88` | non identifié (probablement le grade d'enchantement) |

Deux de ces lignes confirment l'avertissement en tête de
[item_info.h](src/ragnarok/item_info.h) : l'**index d'inventaire** va bien en
`+0x04` (là où `location_` est déclaré) et la **position d'équipement** en
`+0x08` (là où `item_index_` est déclaré). Les deux noms sont bien intervertis.

### 7.3 `result` (octet `+0x21`)

| Valeur | Traitement |
|---|---|
| `0` | succès — `Inventory_AddOrStackItem` puis bandeau |
| `3` | succès également — `Inventory_UpsertOrRemoveItem` + `Session_GetEquipInfoByInvIndex` (la quantité affichée est corrigée du delta déjà porté par l'entrée existante), puis bandeau |
| autre | `ItemPickup_ReportFailureToChat` — **pas de bandeau** |

`ItemPickup_ReportFailureToChat` (`0x00CC3AA0`) écrit un message rouge (`0xFF`) :

| `result` | MSI | Texte (`msgstringtable`) |
|---|---|---|
| 1, 6 | 53 `MSI_CANT_GET_ITEM` | *You cannot get the item.* |
| 2 | 52 `MSI_CANT_GET_ITEM_BECAUSE_WEIGHT` | *You cannot carry more items because you are overweight.* |
| 4 | 220 `MSI_CANT_GET_ITEM_BECAUSE_COUNT` | *You can't have this item because you will exceed the weight limit.* |
| 5 | 279 `MSI_CANT_GET_ITEM_OVERCOUNT_ONEITEM` | *You cannot carry more than 30,000 of one kind of item.* |
| 7 | 1418 `MSI_PICKUP_MAXCOUNT_LIMIT` | *You have exceeded the maximum amount of the same item.* |
| 9 | 3984 `MSI_PICKUP_FAILED_PARTY_MEMBER` | *It is evenly distributed.\nItems cannot be obtained because…* |

Tout autre code est **silencieux** : ni chat, ni bandeau, ni entrée d'inventaire.

### 7.4 Effets de bord du handler

* Ligne de chat `MSI_GET_ITEM` en `0xFFFF00` via `UIWindowMgr_ChatAction(mgr, 1, …, 6)`.
* `if (atoi(item_name_) == 607) *(GameMode + 1464) = 0;` — remise à zéro d'un
  état lié à l'objet **607** (Yggdrasil Berry), présent dans les sept handlers.
* Fenêtre **307** (`UIMenuIconWnd`) : `OnMsg(6, 286, 194, 1)` — le clignotement
  de l'icône « sac ».
* Sur `result == 3` uniquement : si `GameMode + 276` est non nul, il est remis
  à zéro et `sub_D56190` est appelé.

---

## 8. Hors périmètre : `ZC_ITEM_PICKUP_PARTY`

`0x02B8` (`case 696`, handler `0x00CC5CE0`) et `0x0B67` (`case 2919`, handler
`0x00D01BF0`) — « *Party member's obtained item* » (`MSI_VIEW_GET_ITEM_PARTY_MSG`,
id 1284). **Ils ne touchent pas la fenêtre 58** : c'est un affichage séparé,
non traité ici.

---

## 9. Ce que ça implique pour Bourgeon

Aucun module du dépôt ne pilote aujourd'hui la fenêtre 58 (vérifié sur tous les
`MakeWindow` / `FindWindow` / `CloseWindow` de `src/`). Si elle devait être
reprise en ImGui, les points qui ne se devinent pas :

1. **Le bandeau n'empile pas.** Toute réimplémentation qui afficherait une pile
   de notifications change le comportement — ce serait un progrès, mais c'est un
   choix, pas une transposition.
2. **Cinq secondes en dur**, remises à zéro à chaque objet.
3. **Trois messages dans l'ordre 76 → 36 → 34**, et c'est la copie du message 34
   qui est dessinée : un remplacement qui n'écouterait que le 36 afficherait la
   quantité et l'icône mais pas le nom.
4. **Sept opcodes** à couvrir, pas un seul.
5. **Le chat et le bandeau sont indépendants** : neutraliser la fenêtre ne
   supprime pas la ligne « You got … ».
6. **`item_name_` est l'id numérique en texte**, pas un libellé : le nom affiché
   sort de `ItemSkillInfo_ComposeDisplayName` / `BuildDisplayName`.
7. Le natif se ferme lui-même : le détruire depuis notre code n'est pas
   nécessaire, il suffit de ne pas le laisser apparaître (ou de le remplacer au
   niveau du handler, comme pour les autres fenêtres reprises).

---

## 10. Table des adresses

| Adresse | Nom (IDB, après ce chantier) | Rôle |
|---|---|---|
| `0x0103274C` | `??_7UINotifyItemObtainWnd@@6B@` | vtable |
| `0x010C3748` | — | Complete Object Locator |
| `0x0123F640` | — | Type Descriptor RTTI |
| `0x0131F804` | — | slot manager (`g_UIWindowMgr + 0x31C`) |
| `0x0088E590` | `UINotifyItemObtainWnd_ctor` | constructeur |
| `0x00893200` | `UINotifyItemObtainWnd_scalar_dtor` | vt+0x00 |
| `0x00891220` | *(code non défini)* | destructeur non-deletant, sans appelant |
| `0x008CFC70` | `UINotifyItemObtainWnd_Vf24_ReturnFalse` | vt+0x24, `return 0` |
| `0x008BAF00` | `UINotifyItemObtainWnd_OnTick_AutoClose5s` | vt+0x4C |
| `0x008B5520` | `UINotifyItemObtainWnd_OnDraw` | vt+0x50 |
| `0x008C5860` | `UINotifyItemObtainWnd_OnMsg` | vt+0x94 |
| `0x00871340` | `UIWindow_DrawSysBoxFrame9Slice` | cadre 9-slice (partagé, 18 sites) |
| `0x008710E0` | `UIWindow_DrawItemIconAt` | icône d'objet (partagé, 4 sites) |
| `0x008D0AA0` | `sub_8D0AA0` | dessin de texte riche multi-segments |
| `0x00A4CA40` | `Screen_CenterXFrom640` | `x + (largeur - 640)/2` |
| `0x00D5A720` | `BuildItemIconGrfPath` | `유저인터페이스\item\%s.bmp` |
| `0x006A2BD0` | `ResolveItemResNameById` | id → nom de ressource |
| `0x006A6570` | `ItemSkillInfo_SetId` | `_itoa(id)` → `item_name_` |
| `0x006A25F0` | `ItemSkillInfo_Copy` | copie (message 34) |
| `0x006A2CE0` | `ItemSkillInfo_ComposeDisplayName` | nom affichable |
| `0x008A0570` | `ItemSkillInfo_BuildDisplayName` | nom + segments colorés |
| `0x00CC3AA0` | `ItemPickup_ReportFailureToChat` | échecs de ramassage |
| `0x00CC3430` | `Recv_ZC_ITEM_PICKUP_ACK_00A0` | handler `0x00A0` |
| `0x00CC28D0` | `Recv_ZC_ITEM_PICKUP_ACK_029A` | handler `0x029A` |
| `0x00CC2E40` | `Recv_ZC_ITEM_PICKUP_ACK_02D4` | handler `0x02D4` |
| `0x00CC3B50` | `Recv_ZC_ITEM_PICKUP_ACK_0990` | handler `0x0990` |
| `0x00CC4240` | `Recv_ZC_ITEM_PICKUP_ACK_0A0C` | handler `0x0A0C` |
| `0x00CC4A80` | `Recv_ZC_ITEM_PICKUP_ACK_0A37` | handler `0x0A37` |
| `0x00CC5320` | `Recv_ZC_ITEM_PICKUP_ACK_0B41` | handler `0x0B41` |

### Entrées `msgstringtable` utilisées

| id | Clé | Texte (SystemEN) |
|---|---|---|
| 153 | `MSI_GET_ITEM` | `You got %s (%d).` |
| 327 | `MSI_GET_ZENY` | `%s Zeny obtained` |
| **696** | **`MSI_EA_OBTAIN`** | **` - %d obtained.`** |
