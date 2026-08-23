# « View Equipment » — la fenêtre d'inspection d'un autre joueur

RE complète du chemin *clic droit → « Voir l'équipement » → fenêtre native*, sur
le client **20250716** (`Moonlight-Destiny.exe`, base `0x400000`) et le serveur
**moonlight** (`PACKETVER 20250716`, pre-renewal). Objectif : disposer de tout ce
qu'il faut pour **remplacer cette fenêtre par une fenêtre ImGui plus riche**.

Fait le 2026-08-22, entièrement au désassemblage et sur les sources serveur.
**Aucune inconnue ne reste en suspens** : les cinq points d'abord rangés « à
mesurer en jeu » ont tous été tranchés statiquement — le relevé est en §11.

---

## 0. En bref

| | |
|---|---|
| Demande | **CZ_EQUIPWIN_MICROSCOPE `0x02D6`** `[op:2][aid:4]`, 6 o |
| Réponse | **ZC_EQUIPWIN_MICROSCOPE `0x0B37`**, variable — en-tête 47 o + N × 68 o |
| Refus | **ZC_MSG `0x0291`** id **1357** `MSI_OPEN_EQUIPEDITEM_REFUSED` |
| Autorisation du porteur | `status.show_equip`, basculée par **CZ_CONFIG `0x02D8` type 0** |
| Handler client | **`Recv_ZC_EQUIPWIN_MICROSCOPE_0B37` `0x00CBDDC0`** |
| Fenêtre | **id 139 (`0x8B`)**, classe `UIEquipWnd` (celle de l'équipement), mode `+0xB4 = 1` |
| Unique créateur | la **dernière ligne du handler** : `MakeWindow(mgr, 139)` |
| Données | deux tableaux de 10 `ItemSkillInfo` : **session+0x2180** (équip) et **session+0x34E0** (costume) |

🔴 **Le point d'interception est unique et propre** : la fenêtre 139 ne naît que
du handler de `0x0B37`. Aucun raccourci, aucun bouton de menu, aucun autre
chemin — contrairement à l'équipement du joueur (id `0x0A`), qui en a trois.

---

## 1. Le chemin complet, du clic au dessin

```
clic droit sur un joueur
  └─ menu contextuel (Bourgeon, EntityContextMenu)      code d'action 42
       └─ rejoué via CGameMode : gm+0x2E0 = aid, vecteur gm+0x1CC, SendMsg 24
            └─ le natif émet CZ_EQUIPWIN_MICROSCOPE 0x02D6 [aid]
                 └─ SERVEUR clif_parse_ViewPlayerEquip (clif.cpp:22436)
                      ├─ cible absente ......................... silence
                      ├─ cible sur une AUTRE map ............... silence
                      ├─ ni show_equip ni view_equipment ....... ZC_MSG 1357
                      └─ sinon clif_viewequip_ack .............. ZC 0x0B37
                           └─ CLIENT Recv_ZC_EQUIPWIN_MICROSCOPE_0B37 0x00CBDDC0
                                ├─ vide les 2 tableaux de slots
                                ├─ décode N items → session+0x2180 / +0x34E0
                                ├─ copie l'apparence → g_ViewEquip_Other_*
                                ├─ UI_RefreshItemWindows
                                └─ Close(139) sinon MakeWindow(139)
```

⚠ **Le menu contextuel de Bourgeon ne fabrique pas le paquet** : il rejoue le
code natif 42 (cf. `docs/entity_context_menu_re.md` §6.3). Rien à changer de ce
côté pour le remplacement — la demande partira exactement pareil.

### 1.1 Le détournement historique, et pourquoi il n'existe plus

Moonlight détournait `0x02D6` : au-delà du groupe 80, le paquet ouvrait le NPC
caché `#gmclicdroit` au lieu d'afficher l'équipement — **le staff était le seul à
ne pas pouvoir en regarder un**. Retiré le 2026-08-22 (cf.
`entity_context_menu_re.md` §10) ; `clif_parse_ViewPlayerEquip` ne fait plus que
sa vérification d'origine.

---

## 2. Côté serveur : la garde

`src/map/clif.cpp:22436` — quatre sorties, dont **trois silencieuses** :

```c
map_session_data* tsd = map_id2sd(aid);
if (!tsd)          return;              // pas connecté / AID inconnu
if (sd->m != tsd->m) return;            // 🔴 doit être sur la MÊME map
if (tsd->status.show_equip || pc_has_permission(sd, PC_PERM_VIEW_EQUIPMENT))
    clif_viewequip_ack(*sd, *tsd);
else
    clif_msg(*sd, MSI_OPEN_EQUIPEDITEM_REFUSED);   // 1357
```

Conséquences pour une fenêtre ImGui :

1. 🔴 **Deux refus sur quatre ne disent RIEN.** Une fenêtre ImGui ouverte « en
   attente de réponse » resterait vide indéfiniment si la cible a quitté la map
   entre le clic et le paquet. ⇒ ouvrir la fenêtre **à la réception**, comme le
   natif, ou prévoir un délai d'expiration explicite.
2. **La même map est garantie** quand la réponse arrive : l'acteur de la cible
   existe donc côté client (même s'il peut être hors de la portée d'affichage).
   C'est ce qui rend possible de compléter le pantin avec l'arme et le bouclier,
   que le paquet ne porte pas (§8).
3. Le perso de test est **GM 999** : il a `view_equipment` et verra donc TOUT le
   monde, `show_equip` ou pas. Tester le refus demande un compte sans droits.

`MSI_OPEN_EQUIPEDITEM_REFUSED` (1357) = « Les informations d'équipement de ce
personnage ne sont pas publiques. » — le client l'affiche déjà par le canal
`ZC_MSG` ; rien à refaire.

---

## 3. Le drapeau « montrer mon équipement »

C'est le seul réglage que le porteur contrôle, et il est **déjà porté par
Bourgeon**.

| | |
|---|---|
| Émission | `CZ_CONFIG 0x02D8` `[type=0][flag]` (`CONFIG_OPEN_EQUIPMENT_WINDOW`) |
| Écho | `ZC_CONFIG 0x02D9` → `Recv_ZC_CONFIG_02D9` **0x00CBB0F0** case 0 |
| Au login | `ZC_CONFIG_NOTIFY 0x02DA` → `Recv_ZC_CONFIG_NOTIFY_02DA` **0x00CBB220** |
| Globale client | **`g_Own_ShowEquipToOthers` 0x015FFD14** (1 = visible) |
| Retour au joueur | chat, msgstring **1358** (non public) / **1359** (public) |
| Côté Bourgeon | `character_sheet.cc` : case « Montrer mon équipement » (`kShowEquipFlag`, `kCmdShowEquip = 0xFD`) |

🔴 **Le client n'écrit jamais ce drapeau lui-même** : il l'apprend du serveur.
La case à cocher envoie, et c'est l'écho `0x02D9` qui met `0x015FFD14` à jour —
donc pas d'état local à maintenir, dans la fenêtre native comme dans la nôtre.

⚠ **Ce drapeau ne concerne QUE mon propre équipement.** Il n'y a **aucun** moyen
de savoir, avant d'envoyer `0x02D6`, si une cible acceptera : le seul retour est
la réponse ou le refus. Une entrée de menu « grisée si le joueur refuse » est
donc impossible.

---

## 4. Le paquet `ZC_EQUIPWIN_MICROSCOPE 0x0B37`

Choisi par `PACKETVER 20250716` (≥ 20200916). Les six variantes historiques ont
chacune leur handler dans le client (§10) ; **seule `0x0B37` est atteinte ici**.

### 4.1 En-tête — 47 octets

| off | taille | champ | destination client |
|---|---|---|---|
| 0 | 2 | `0x0B37` | — |
| 2 | 2 | longueur totale | `(len - 47) / 68` = nombre d'objets |
| 4 | 24 | nom du personnage | **`g_ViewEquip_Other_Name` 0x015FFD40** |
| 28 | 2 | classe (`LOOK_BASE`) | `g_ViewEquip_Other_Job` **0x015FFD18** |
| 30 | 2 | coiffure | `..._HairStyle` **0x015FFD20** |
| 32 | 2 | accessoire BAS | `..._HeadLow` **0x015FFD24** |
| 34 | 2 | accessoire MILIEU | `..._HeadMid` **0x015FFD28** |
| 36 | 2 | accessoire HAUT | `..._HeadTop` **0x015FFD2C** |
| 38 | 2 | cape (`LOOK_ROBE`) | `..._Robe` **0x015FFD38** |
| 40 | 2 | couleur de cheveux | `..._HairColor` **0x015FFD34** |
| 42 | 2 | couleur de vêtements | `..._ClothesColor` **0x015FFD30** |
| 44 | 2 | `body2` (style de corps) | `..._Body2` **0x015FFD3C** |
| 46 | 1 | sexe | `..._Sex` **0x015FFD1C** |

⚠ Ces globales sont **écrasées à chaque inspection** : il n'y a de place que
pour UNE cible. Une fenêtre ImGui qui voudrait garder plusieurs onglets ouverts
doit **copier** ce qu'elle lit, ou mieux : décoder le paquet elle-même (§9).

✅ **Le nom arrive du RÉSEAU ⇒ `ro::WireToUtf8`**, et rien d'autre. La règle est
posée dans `src/ui/ro_imgui.h` : `LocalToUtf8` vaut pour ce que le **client** a
chargé (base d'objets, msgstringtable) ; ce qui vient **du fil** est en 1252 —
sauf ce qui porte un emoji, en UTF-8 — et `WireToUtf8` tranche à la lecture sur
la validité stricte de l'UTF-8. Précédent dans le projet : `char_diagnostics.cc`
convertit déjà un nom de personnage par `ro::WireToUtf8` (ligne 950), et
`chat_window.cc` fait de même pour les expéditeurs.
⚠ Tampon **rotatif** : à consommer tout de suite, jamais à stocker.

### 4.2 Une pièce d'équipement — 68 octets (`0x44`)

| off | taille | champ | remarque |
|---|---|---|---|
| 0 | 2 | index d'inventaire **du porteur** | inutilisable chez nous (c'est SON inventaire) |
| 2 | 4 | **ITID** | l'identifiant d'objet |
| 6 | 1 | type d'objet | |
| 7 | 4 | `location` | masque `EQP_*` où l'objet PEUT aller |
| 11 | 4 | **`WearState`** | masque `EQP_*` où il est PORTÉ — 🔴 c'est LUI qui range |
| 15 | 16 | 4 cartes (`uint32`) | ou l'affixe « forgé par » |
| 31 | 4 | expiration (location) | 0 = définitif |
| 35 | 2 | `bindOnEquipType` | |
| 37 | 2 | `wItemSpriteNumber` | **le look** — c'est lui qui habille le pantin (§7.3) |
| 39 | 1 | nombre d'options | |
| 40 | 25 | 5 × `{id:2, valeur:2, param:1}` | options aléatoires |
| 65 | 1 | **raffinement** | |
| 66 | 1 | **grade** (enchantgrade) | |
| 67 | 1 | drapeaux | bit 0 = identifié, bit 1 = abîmé, bit 2 = onglet ETC |

Rempli par `clif_item_equip()` (clif.cpp:3054). ⚠ Comme partout, les cartes
passent par `clif_addcards`, qui **réécrit** les champs pour les objets spéciaux
(cf. `feedback_re_method` §9) — décoder la structure serveur ne dit pas ce qui
arrive.

La boucle serveur parcourt `i < EQI_MAX`, ce qui **inclut les six emplacements
SHADOW** — voir le piège en §6.2.

---

## 5. Ce que le client en fait — `0x00CBDDC0`

```c
ViewEquip_Other_ClearEquipSlots(session);      // 0x00D56500  -> session+0x2180
ViewEquip_Other_ClearCostumeSlots(session);    // 0x00D7F4C0  -> session+0x34E0
for (i = 0; i < (len - 47) / 68; ++i) {
    ItemSkillInfo it = decode(paquet + 47 + 68*i);
    if (it.WearState) {
        if (EquipLocation_IsCostume(it.WearState))  // 0x00D8E630
             ViewEquip_Other_SetCostumeItem(session, &it);   // 0x00D7E650
        else ViewEquip_Other_SetEquipItem(session, &it);     // 0x00D54F40
    }
    /* + une branche « œuf de pet » héritée du handler de la liste d'équipement
       du joueur : elle écrit g_Own_PetEggInvIndex et peut émettre CZ_CONFIG
       type 2 (auto-feed).  ✅ MORTE ici — voir §5.1. */
}
UI_RefreshItemWindows(mgr);
/* … copie de l'apparence dans les g_ViewEquip_Other_* … */
if (!UIWindowMgr_SaveRectAndCloseWindow(mgr, 139))
    UIWindowMgr_MakeWindow(mgr, 139);
```

🔴 **La dernière ligne est un BASCULEUR, pas une ouverture.** ✅ **Établi** au
désassemblage : le `case 139` de `UIWindowMgr_SaveRectAndCloseWindow`
(**@0x00A300C9**) est

```asm
mov  eax, [edi+3FCh]      ; l'instance en cache
test eax, eax
jz   loc_A2F6CB           ; pas ouverte  -> bl reste 0  -> le handler MakeWindow
push eax
call UIWindowMgr_QueueDestroyWindow
mov  dword ptr [edi+3FCh], 0
mov  bl, 1                ; DÉTRUITE     -> retourne TRUE -> PAS de MakeWindow
```

La valeur de retour vaut donc **1 exactement quand la destruction a eu lieu**.
Conséquence pour le joueur : inspecter B **pendant** qu'on regarde A **ferme** la
fenêtre — alors que les données de B viennent d'écraser celles de A — et il faut
**recliquer** pour la revoir. C'est un défaut visible du natif, et un des
premiers gains d'une fenêtre ImGui.

⚠ `WearState == 0` fait **sauter** l'objet : un objet transporté mais non porté
n'arrive de toute façon jamais ici (la boucle serveur ne prend que `it.equip`).

### 5.1 ✅ La branche « œuf de pet » est MORTE sur ce paquet

Elle inquiétait pour une bonne raison : elle **écrit une globale à nous**
(`g_Own_PetEggInvIndex`) et peut **émettre** `CZ_CONFIG` type 2 — inspecter un
inconnu aurait pu déranger notre propre familier.

Elle est inatteignable, par **double filtre serveur** (`clif_viewequip_ack`,
clif.cpp:14829) : l'objet doit être dans `tsd.equip_index[]` **et** passer
`itemdb_isequip2`, qui n'accepte que `IT_WEAPON`, `IT_ARMOR`, `IT_AMMO`,
`IT_SHADOWGEAR` (itemdb.cpp:3398). Un œuf de pet est `IT_PETEGG` : il n'est ni
dans `equip_index`, ni équipable. `IsPetEggItem(ITID)` est donc toujours faux.

C'est du code recopié du handler de `ZC_EQUIPMENT_ITEMLIST` (la liste
d'équipement du JOUEUR), où l'œuf a un sens. ⇒ **rien à neutraliser** en
remplaçant la fenêtre.

---

## 6. Le rangement — deux tableaux de 10 emplacements

| tableau | adresse | getter | remplisseur |
|---|---|---|---|
| équipement | `session + 0x2180` | `ViewEquip_Other_GetEquipSlot` **0x00D5D040** | **0x00D54F40** |
| costume | `session + 0x34E0` | `ViewEquip_Other_GetCostumeSlot` **0x00D81C70** | **0x00D7E650** |

`session` = **`g_UIWindowContextKey` 0x015FA3C0**. Entrées de **248 octets**
(`ItemSkillInfo`, la structure d'objet universelle du client — la même que rend
`Session_GetEquipInfoBySlot` pour son propre équipement).

### 6.1 Les 10 emplacements

`EquipLocation_DecodeToSlots` (**0x00D55850**) traduit le masque `EQP_*` en
**jusqu'à trois** index (une arme à deux mains occupe arme + bouclier) :

| index | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| pièce | tête (bas) | arme | cape | acc. gauche | armure | bouclier | chaussures | acc. droit | tête (haut) | tête (milieu) |
| bit `EQP` | 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 |

L'index est donc `log2(bit)`. Les **costumes** (0x400 haut, 0x800 milieu, 0x1000
bas, 0x2000 cape) sont **remappés sur les mêmes index** puis rangés dans l'autre
tableau. La munition (`0x8000`) reçoit l'index 15 : elle n'a pas de case dans la
grille et n'est affichée que sur son propre équipement.

### 6.2 🔴 Le piège du SHADOW GEAR

`EquipLocation_IsCostume` rend **vrai** pour `0x4000` et `0x10000..0x200000`,
c'est-à-dire pour les six emplacements d'ombre. Le client les range donc dans le
tableau **COSTUME**, remappés sur les index 1..7 — les mêmes que les costumes
réels. Un joueur portant à la fois un costume et une pièce d'ombre verrait donc
**l'un écraser l'autre**, sans le moindre avertissement.

✅ **Mesuré : le risque est THÉORIQUE sur moonlight.** Les **1028** objets
portant un emplacement `Shadow_*` sont **tous** dans `db/re/item_db_equip.yml` —
zéro dans `db/import/` (la base réellement chargée) et zéro dans `db/pre-re/`.
Confirmé par l'utilisateur : « on n'utilise pas le stuff shadow sur moonlight ».
La collision ne peut donc pas se produire aujourd'hui ; elle réapparaîtrait au
premier objet d'ombre ajouté. Une fenêtre ImGui qui décode le paquet elle-même
n'a pas ce défaut : elle peut leur donner leur propre rangée.

⚠ **La base d'objets chargée est `db/import/items/`**, pas `db/pre-re/` —
sa surcharge est complète. Chercher un objet dans `pre-re/` induit en erreur.

---

## 7. La fenêtre native — id 139 (`0x8B`)

### 7.1 Identité

Créée par le **case 139** de `UIWindowMgr_MakeWindow` (**@0x00A3F09F**) :

```asm
mov  eax, [edi+3FCh]          ; déjà ouverte ? -> on la rend
push 17Ch                     ; 380 octets
call operator new
call UIWnd_id0a_ctor          ; 0x0088D740 — LA MÊME CLASSE que l'équipement
mov  [edi+3FCh], eax          ; cache du manager : mgr+0x3FC
mov  dword ptr [eax+0B4h], 1  ; 🔴 mode « autre joueur »
push 0A7h / push 118h         ; 280 x 167
call UIWindow_SetSize
... x = 0xB9 (base 640), y = 0x12C (base 480), centrée
```

Donc : **même classe, même vtable (`0x0103223C`), même DrawContent** que la
fenêtre Équipement. Tout ce qui les sépare est le champ **`+0xB4`** (0 = la
mienne, cachée à `mgr+0x1E0` ; 1 = celle d'un autre, cachée à `mgr+0x3FC`).

### 7.2 Champs utiles de l'instance

| offset | rôle |
|---|---|
| `+0xB4` | **mode** : 0 = mon équipement, 1 = inspection |
| `+0xF0` | onglet courant : **0 = General, 1 = Costume** (2 = Titre, 3 = indicateur de dégâts — mode 0 seulement) |
| `+0xF4` | Y du haut de la zone d'onglet |
| `+0xEC` | le bandeau d'onglets |
| `+0xF8…+0x11C` | l'apparence servant au pantin (voir 7.3) |

### 7.3 Ce que la fenêtre dessine — `UIEquipWnd_DrawContent 0x008B3190`

1. **Titre** : `MSI_OTHERUSER_EQUIPED_ITEM` (0x551) « Équipement de %s »,
   formaté avec `g_ViewEquip_Other_Name`. (Mode 0 : `MSI_EQUIPED_ITEM` 0x68.)
2. **Fond** : `equipwin_bg.bmp` en mode 1 (`equipwin_bg2.bmp` en mode 0),
   bandeau `titlebar_fix.bmp`.
3. **Grille de 10 emplacements** : `UIEquipWnd_DrawSlotGrid 0x00897D60`, icône +
   nom d'objet tronqué à 70 px (élargi par le module `EquipTweaks`).
4. **Pantin** : `UIEquipWnd_BuildDollAppearance 0x008CF970` puis `Actor_Init` /
   `Actor_DrawSprites`, centré à `largeur/2 + 1`, Y = 149.

🔴 **Le pantin n'utilise PAS les accessoires de l'en-tête du paquet.** En mode 1,
`BuildDollAppearance` ne lit des globales que la classe, le sexe, la coiffure,
les deux couleurs et `body2` ; **les coiffes viennent du champ `look`
(`wItemSpriteNumber`) des objets rangés** — emplacement 8 → tête haute, 9 →
milieu, 0 → bas, 2 → cape. Les globales `..._HeadLow/Mid/Top` sont donc écrites
et **jamais lues** par le pantin. C'est important pour nous : le pantin d'une
fenêtre ImGui doit se construire de la même façon, sinon un joueur portant un
costume de tête apparaîtra tête nue.

### 7.4 Les onglets

`UIEquipWnd_OnCreate` (0x008A4B30) ajoute **General** (`0x70D`, infobulle
`0xC56`) et **Costume** (`0x973` / `0xC57`), puis l'onglet **Titre** (`0xA7D` /
`0xC58`) *uniquement si `+0xB4 == 0`*. ⚠ Sur l'exe **livré**, le patch WARP
`NoEquipWinTitle` remplace ce `JNZ` par un `JMP` : l'onglet Titre n'existe nulle
part (l'IDB, qui est l'exe **vanilla**, porte encore `75 2E` — cf.
`reference_ida_is_vanilla_warp_patches`).

⇒ **La fenêtre d'inspection a exactement deux onglets : General et Costume.**

### 7.5 Ce que le natif permet — et ce qu'il interdit

| geste | mode 0 (mon équipement) | mode 1 (inspection) |
|---|---|---|
| survol | infobulle « nom [Option n] » | **identique** (`UIEquipWnd_OnMouseMove` n'est pas gardée) |
| clic droit | fenêtre de description d'objet (id `0x0C`) | **identique** (`UIEquipWnd_OnRButtonDown`) |
| glisser | déséquiper | 🔴 **rien** — `UIEquipWnd_OnDragStart` s'ouvre sur `if (!this[45])` |
| Maj + clic | lien d'objet dans le chat | 🔴 **rien** — même garde |
| munition | affichée sous le pantin | non affichée |

---

## 8. Ce que le natif ne montre pas

Toutes ces données **sont dans le paquet** et sont perdues à l'affichage :

- le **raffinement** et le **grade** n'apparaissent que dans le nom composé ;
- les **options aléatoires** ne sont comptées, pas détaillées (« [Option 3] ») ;
- les **cartes** ne sont lisibles que via la fenêtre de description ;
- l'objet **abîmé**, **lié au compte**, ou à **durée limitée** : rien ;
- le **shadow gear** serait écrasé par les costumes (§6.2 — sans objet sur moonlight) ;
- pas de lien de chat, pas de comparaison avec son propre équipement.

Et ce que le paquet **ne porte pas**, donc ce qu'aucune fenêtre ne pourra
inventer : niveau, guilde, points de vie, statistiques — et le **look d'arme et
de bouclier**, absents de l'en-tête.

⛔ **Ces deux-là ne seront PAS ajoutés au pantin** (décision de l'utilisateur,
2026-08-22). Ce serait faisable — la cible est sur notre carte, son acteur porte
les chemins déjà résolus — mais la fiche liste déjà l'arme et le bouclier avec
leurs cartes et leur raffinement, ce qui est précisément ce qu'on vient y lire.

---

## 9. Plan de remplacement ImGui

### 9.1 Point d'accroche

Un seul, et il existe déjà : le routeur `MakeWindowHook` de
`src/features/patches/window_pos_tweaks.cc` (~ligne 140). Ajouter :

```c++
if (windowID == 139) {
  if (auto* w = Bourgeon::Instance().view_equip_window())
    w->HandleNativeCreation(win);   // masque +0x28 ici, DÉTRUIT au tick
}
```

C'est le motif éprouvé (`character_sheet`, `pet_window`, `game_menu`) : masquer
à la naissance, détruire dans `OnTick` par `uiwnd::CloseWindow(139)` — jamais
depuis l'intérieur de `MakeWindow`, dont l'appelant déréférence le retour.

**La fenêtre 139 n'émet rien à la naissance ni à la mort** ⇒ la détruire au tick
est sûr. ✅ **Mesuré** (recherche de `CRagConnection_SendPacket` sur
`0x88D000..0x8D1000`, la plage des classes de fenêtre) : les deux seuls envois
imputables à `UIEquipWnd` sont dans son `WndProc` `0x008BF7D0`
(@`0x008C0285` et @`0x008C05E3`) — donc sur des **commandes de boutons**, et ces
boutons ne sont créés que si `+0xB4 == 0`. Ni le ctor `0x0088D740`, ni
`OnCreate`, ni `DrawContent`, ni `SaveLayout 0x008CFE00` n'émettent quoi que ce
soit.

### 9.2 D'où prendre les données — décoder le paquet, pas lire le natif

**Recommandé : décoder `0x0B37` nous-mêmes** dans `OnRecvPacket` (via
`PacketInbox`, cf. `src/features/net_inbox.h` — le décodage doit repartir sur le
fil principal).

Pourquoi plutôt que lire `session+0x2180` :

1. les tableaux natifs **perdent** le grade, les options détaillées, les
   drapeaux, et **écrasent** le shadow gear (§6.2) ;
2. ils ne gardent **qu'une cible** ; le paquet, copié, en garde autant qu'on
   veut (onglets, historique, comparaison de deux joueurs) ;
3. aucun offset natif de 248 octets à deviner — le format du fil, lui, est écrit
   noir sur blanc dans `packets_struct.hpp` et vérifiable des deux côtés ;
4. la règle du projet : dépendre du fil quand le fil suffit.

⚠ Le natif continuera de remplir ses tableaux (le handler n'est pas remplacé,
seule sa fenêtre est tuée) : sans effet, et c'est très bien — on ne touche pas à
un chemin natif qui marche.

### 9.3 Ce qui existe déjà et se réutilise tel quel

| besoin | brique |
|---|---|
| icône + case d'objet + infobulle | `src/features/item_cell.{h,cc}` |
| description complète, comparaison avec l'équipé | `windows/item_desc_window` (`cmp_show_equipped`) |
| pantin composé (job, sexe, coiffure, couleurs, coiffes, cape) | `src/ui/doll.h` — `ro::DollLook` a **exactement** les champs de l'en-tête |
| gestes ET menu contextuel d'un objet | 🔴 `links::Hit` + `links::DrawMenu` + `links::HoverPreview` (`features/link_gesture.h`) |
| habillage RO des fenêtres | `ro_imgui` (`ro::BeginRoWindow`, `ro::RoCheckbox`…) |
| couleurs composées par la cible | `fx::style_sync::RemoteRecipe` + `palette_base` + `ro::ApplyRecipe` |
| MES pièces portées, par emplacement | `rag::equip::ReadWorn` (`ragnarok/equip_slots.h`) |
| corps réel de la cible (3e/4e classes) | `fx::palette_inject::ActorBodySpritePath` |
| position persistée | `window_position_persistence` |
| traduction | `i18n::Tr` + les catalogues `tools/lang/` |

### 9.4 Ce que la fenêtre ImGui peut offrir en plus

Par ordre de rapport valeur / effort :

1. **rester ouverte** et se rafraîchir quand on inspecte quelqu'un d'autre (le
   natif se ferme, §5) ;
2. **cartes, options, grade, raffinement, drapeaux** affichés directement, avec
   les infobulles complètes ;
3. **lien dans le chat** d'une pièce vue (interdit par le natif) ;
4. **comparaison** avec sa propre pièce du même emplacement (case « Comparer » :
   une 3ᵉ colonne, et la fenêtre passe à sa seconde taille fixe) ;
5. **pantin agrandi**, orientable (molette ou boutons), portant les couleurs
   COMPOSÉES par la cible (§9.5) ;
6. **shadow gear** sur sa propre rangée le jour où il entre en jeu (§6.2) ;
7. entrées de staff (`@iteminfo`, `@whodrops`) déjà présentes ailleurs dans le
   menu contextuel ;
8. trois **outils du staff** dans l'en-tête, « Cloner : équipement / stats /
   job » — `@cloneequip <nom>`, `@clonestat <nom>`, `@job <classe>`.

⚠ Sur les deux boutons de staff, rien n'est réimplémenté : la ligne part par le
canal du chat (`ChatWindow::QueueCommand`, qui convertit vers l'encodage du fil
et diffère l'envoi hors frame ImGui), et le SERVEUR reste la seule autorité —
`conf/import/groups.yml` accorde les deux au groupe **80**, exactement le seuil
d'`IsStaff()`, et son verdict revient dans le chat. Les deux prennent un NOM de
personnage, la seule identité que la réponse nous donne. ⚠ `@cloneequip` refuse
la cible « soi-même » (`sd == pl_sd`) et exige qu'elle soit en ligne.

🔴 **`@job` n'est PAS la commande de rAthena sur moonlight.** `ACMD_FUNC(jobchange)`
est **commenté** dans `src/map/atcommand.cpp` (deux fois) ; celle qui tourne vit
dans **`src/custom/atcommand.inc`** et, après le changement de classe, enchaîne
`@blvl 999`, `@jlvl 100`, `@allskills` et `@allstats` (lignes marquées
`[Stingor]`). Le bouton hérite donc de tout ça — l'infobulle le dit, sans quoi il
promettrait beaucoup moins que ce qu'il fait.

🔴 **La classe envoyée est la classe AFFICHÉE** (`vd.look[LOOK_BASE]`), seule
chose que le paquet porte. Sur un joueur monté ou déguisé c'est une classe
« factice » (`JOB_KNIGHT2` & co) que la commande **refuse explicitement**. On
envoie quand même : le refus arrive dans le chat, ce qui vaut mieux que de
deviner une classe de base à partir d'une table à maintenir.

### 9.5 Le pantin doit porter les couleurs COMPOSÉES par la cible

Un joueur qui s'est recoloré (éditeur de style, `CZ 0x0F26`) est vu par tout le
monde dans SES couleurs : le serveur diffuse sa **recette** (`ZC 0x0F27`) et
chaque client recalcule la palette. Un pantin qui l'ignorerait afficherait
l'apparence native d'un personnage qui est, à trois mètres, d'une autre couleur.

Le registre des recettes des joueurs en vue existait déjà (`style_sync.cc`,
`g_remote`, indexé par GID) mais n'était pas lisible de l'extérieur : il est
désormais exposé par **`fx::style_sync::RemoteRecipe(gid, body_key, out)`**, qui
choisit la variante exactement comme le fait l'application sur l'acteur — une
vue qui trancherait autrement montrerait d'autres couleurs que celles à l'écran.

Deux points qui ne s'improvisent pas :

1. 🔴 **Le chemin du sprite de corps vient de l'ACTEUR**
   (`palette_inject::ActorBodySpritePath`), pas d'une déduction : la cible est
   forcément sur notre carte, donc son acteur porte le corps que le client a
   réellement résolu — ce que la déduction rate sur les 3e/4e classes, les
   montures et les costumes de corps. Et depuis la v7 des recettes, ce chemin
   **désigne la variante** : se tromper de corps, c'est appliquer les couleurs
   d'un autre. Il sert aussi de `DollLook::body_spr_override`, ce qui corrige le
   pantin même pour un joueur NON recoloré.
2. 🔴 **La signature du cache se calcule champ par champ.** `PaletteRecipe`
   porte du bourrage (`RampAdjust` = 5 octets utiles, 6 alloués) : hasher la
   structure en bloc ferait varier la clé sans que rien n'ait changé, donc
   ré-analyser un `.spr` entier **à chaque frame**. C'est le même piège que celui
   qui interdit d'envoyer la struct telle quelle sur le fil.

⚠ Sans AID (fiche reçue sans être passée par le menu contextuel), il n'y a ni
acteur ni recette : le pantin retombe sur l'apparence native, ce qui reste juste
pour l'écrasante majorité des joueurs.

### 9.6 La comparaison lit MES pièces dans la session

Mon équipement vit dans deux tableaux de la session, au **même layout** que ceux
du joueur inspecté : `session+0x17D0` (équipement) et `session+0x2B30` (costume),
dix entrées de 248 octets — contre `+0x2180` / `+0x34E0` pour la cible.

Ces onze offsets étaient enfermés dans un namespace anonyme de
`character_sheet.cc`, donc invisibles au `grep` depuis ailleurs. Ils vivent
maintenant dans **`src/ragnarok/equip_slots.h`** avec la lecture SEH
(`rag::equip::ReadWorn`), sur le modèle de `ragnarok/uiwnd.h` : au prochain
portage de client, il y a UN endroit à corriger.
⚠ `character_sheet.cc` garde ses copies (antérieures, valeurs identiques
vérifiées) — la migration est une passe à part.

🔴 **Les deux colonnes passent par le MÊME `ChatLink` et le MÊME compositeur de
nom.** Deux colonnes composées autrement — l'une par le name-builder natif,
l'autre à la main — se compareraient mal : l'œil prendrait une différence de
mise en forme pour une différence d'objet. Même objet de base des deux côtés = la
mienne est teintée en vert ; ce n'est PAS « la même pièce » (raffinement, cartes
et options peuvent différer, le survol les montre).

### 9.7 Le menu d'un objet existe déjà — ne pas en écrire un

La première version portait son propre menu contextuel (lier / copier /
`@iteminfo` / `@whodrops`). C'était une redite APPAUVRIE de `links::DrawMenu`,
qui sert déjà toutes les surfaces d'objet du client et donne : le **nom en
en-tête** + séparateur, **Description**, **Base de données du site**, **Lien dans
le chat**, **Ajouter/Retirer de l'alootid**, `@iteminfo`, `@whodrops`.

Trois règles d'emploi, chacune payée par un défaut visible :

1. 🔴 **UN seul popup pour la fenêtre**, pas un par cellule. Deux cellules qui
   ouvrent `BeginPopupContextItem` sous la même pile d'ids donnent
   « 2 visible items with conflicting ID » et se partagent le menu.
2. 🔴 **`OpenPopup` HORS de la pile d'ids du clic** : l'identifiant d'un popup se
   hache avec cette pile. Le clic lève un drapeau, l'ouverture se fait plus bas —
   mais **dans le même `BeginChild`**, un popup appartenant à sa fenêtre.
3. 🔴 **Un id distinct par colonne** (`DrawItemCell(..., id)`) : les deux
   colonnes de la comparaison dessinent la même structure au même endroit.

`links::Hit` fournit aussi les gestes (clic = description, Maj+clic = lien de
chat, clic droit = menu) et `links::HoverPreview` l'aperçu — la `Target` d'objet
portant la balise relue, cartes et options d'instance comprises.

### 9.8 Pièges à ne pas rater

- 🔴 **Décoder sur le fil réseau = crash** : passer par `PacketInbox` (le
  commentaire d'en-tête de `net_inbox.h` explique pourquoi).
- 🔴🔴 **`PushAnnounced`, pas `Push`** : `0x0B37` est à longueur VARIABLE. Le
  dispatcher ne transmet que ses premiers octets — le corps vit complet dans le
  tampon du client. Copier `len` octets rendrait un en-tête **sans aucune
  pièce**, et rien ne le dirait : la fiche s'ouvrirait sur un joueur en
  sous-vêtements.
- 🔴 **Ne pas rouvrir l'équivalent natif** en basculant « Interface moderne »
  hors ligne : la règle du projet est *fermer, jamais rouvrir*.
- 🔴 **La longueur est annoncée** : `(len - 47) / 68` doit être borné (un paquet
  tronqué ou un `len` absurde ne doit pas faire boucler N fois).
- ⚠ **Le nom vient du fil** : `ro::WireToUtf8`, jamais `LocalToUtf8` ni
  `Cp949ToUtf8` — et copié aussitôt (tampon rotatif).
- ⚠ Le refus (1357) arrive par `ZC_MSG`, **pas** par une réponse à `0x02D6` : si
  la fenêtre s'ouvre « en attente », rien ne la refermera.

---

## 10. Table des adresses (client 20250716, base 0x400000)

### Handlers de paquet

| adresse | nom | opcode |
|---|---|---|
| `0x00CBDDC0` | `Recv_ZC_EQUIPWIN_MICROSCOPE_0B37` | **0x0B37 — le seul actif** |
| `0x00CBD860` | `Recv_ZC_EQUIPWIN_MICROSCOPE_0B03` | 0x0B03 |
| `0x00CBD310` | `Recv_ZC_EQUIPWIN_MICROSCOPE_0A2D` | 0x0A2D |
| `0x00CBD0C0` | `Recv_ZC_EQUIPWIN_MICROSCOPE_0997` | 0x0997 |
| `0x00CBCE90` | `Recv_ZC_EQUIPWIN_MICROSCOPE_02D7_0859` | 0x02D7 et 0x0859 |
| `0x00CC6F20` | `Recv_ZC_EQUIPWIN_MICROSCOPE_0906` | 0x0906 |
| `0x00CBB0F0` | `Recv_ZC_CONFIG_02D9` | 0x02D9 (case 0 = show_equip) |
| `0x00CBB220` | `Recv_ZC_CONFIG_NOTIFY_02DA` | 0x02DA (login) |

Table de répartition : `RecvLoop_DispatchPackets`, case 2871 (`0x0B37`)
@`0x00CA8CEC`.

### Données

| adresse | nom |
|---|---|
| `0x015FA3C0` | `g_UIWindowContextKey` (la « session ») |
| `session+0x2180` | 10 × `ItemSkillInfo` (248 o) — équipement inspecté |
| `session+0x34E0` | 10 × `ItemSkillInfo` — costume (et shadow) inspecté |
| `0x015FFD14` | `g_Own_ShowEquipToOthers` |
| `0x015FFD18` | `g_ViewEquip_Other_Job` |
| `0x015FFD1C` | `g_ViewEquip_Other_Sex` |
| `0x015FFD20` | `g_ViewEquip_Other_HairStyle` |
| `0x015FFD24/28/2C` | `..._HeadLow / HeadMid / HeadTop` (**écrits, jamais lus**) |
| `0x015FFD30` | `..._ClothesColor` |
| `0x015FFD34` | `..._HairColor` |
| `0x015FFD38` | `..._Robe` |
| `0x015FFD3C` | `..._Body2` |
| `0x015FFD40` | `g_ViewEquip_Other_Name` (24 o) |
| `0x0131F4E8` | `g_UIWindowMgr` (fenêtre 139 cachée à `+0x3FC`) |

### Fonctions

| adresse | nom | rôle |
|---|---|---|
| `0x00A3F09F` | `UIWindowMgr_MakeWindow` case 139 | **crée la fenêtre d'inspection** |
| `0x0088D740` | `UIWnd_id0a_ctor` | ctor partagé (380 o) |
| `0x008B3190` | `UIEquipWnd_DrawContent` | dessin, les deux modes |
| `0x00897D60` | `UIEquipWnd_DrawSlotGrid` | la grille de 10 emplacements |
| `0x0089E380` | `UIEquipWnd_GetSlotItem` | **aiguillage mode × onglet** |
| `0x008CF970` | `UIEquipWnd_BuildDollAppearance` | apparence du pantin |
| `0x008B9D20` | `UIEquipWnd_OnMouseMove` | infobulle (non gardée par mode) |
| `0x008BB340` | `UIEquipWnd_OnRButtonDown` | ouvre la description (id 0x0C) |
| `0x008B95B0` | `UIEquipWnd_OnDragStart` | glisser + lien chat — **mode 0 seul** |
| `0x00D5D040` | `ViewEquip_Other_GetEquipSlot` | lecture `session+0x2180` |
| `0x00D81C70` | `ViewEquip_Other_GetCostumeSlot` | lecture `session+0x34E0` |
| `0x00D54F40` | `ViewEquip_Other_SetEquipItem` | écriture |
| `0x00D7E650` | `ViewEquip_Other_SetCostumeItem` | écriture |
| `0x00D56500` | `ViewEquip_Other_ClearEquipSlots` | purge |
| `0x00D7F4C0` | `ViewEquip_Other_ClearCostumeSlots` | purge |
| `0x00D55850` | `EquipLocation_DecodeToSlots` | masque `EQP_*` → 3 index |
| `0x00D8E630` | `EquipLocation_IsCostume` | 🔴 vrai aussi pour le shadow gear |
| `0x00A2E770` | `UIWindowMgr_SaveRectAndCloseWindow` | ferme (et détruit) |
| `0x00A39340` | `UIWindowMgr_MakeWindow` | fabrique |

### Serveur

| fichier | ligne | rôle |
|---|---|---|
| `src/map/clif.cpp` | 22436 | `clif_parse_ViewPlayerEquip` (la garde) |
| `src/map/clif.cpp` | 14799 | `clif_viewequip_ack` (l'émission) |
| `src/map/clif.cpp` | 3054 | `clif_item_equip` (une pièce) |
| `src/map/clif.cpp` | 22455 | `clif_parse_configuration` (type 0 = show_equip) |
| `src/map/packets_struct.hpp` | 1308 | `PACKET_ZC_EQUIPWIN_MICROSCOPE` (0x0B37) |
| `src/map/packets_struct.hpp` | 457 | `EQUIPITEM_INFO` |

---

## 11. Points levés (2026-08-22) — plus rien en suspens

Les cinq inconnues de la première passe ont toutes été tranchées **sans session
de jeu** : quatre par le désassemblage ou les sources serveur, une par
l'utilisateur.

| # | question | verdict | preuve |
|---|---|---|---|
| 1 | la fenêtre se ferme-t-elle au lieu de se rafraîchir ? | ✅ **oui, c'est un basculeur** | `case 139` de `Close` @`0x00A300C9` : `bl = 1` **seulement** après `QueueDestroyWindow` (§5) |
| 2 | le shadow gear écrase-t-il les costumes ? | ✅ **risque théorique** | les 1028 objets `Shadow_*` sont tous dans `db/re/`, zéro dans `db/import/` (la base chargée) ; confirmé par l'utilisateur (§6.2) |
| 3 | quel encodage pour le nom reçu ? | ✅ **`ro::WireToUtf8`** | règle de `ro_imgui.h` + précédents `char_diagnostics.cc:950`, `chat_window.cc` (§4.1) |
| 4 | la branche « œuf de pet » est-elle atteignable ? | ✅ **morte** | double filtre `equip_index[]` + `itemdb_isequip2` (WEAPON/ARMOR/AMMO/SHADOWGEAR) ; `IT_PETEGG` exclu (§5.1) |
| 5 | la classe émet-elle un paquet à la naissance/mort ? | ✅ **non** | recherche de `CRagConnection_SendPacket` sur `0x88D000..0x8D1000` : 2 sites, tous deux dans le `WndProc`, sur des boutons du mode 0 (§9.1) |

⚠ **Une leçon de méthode** : la question 1 avait été rangée en « à mesurer en
jeu » alors qu'elle était **décidable au désassemblage** — il suffisait de lire
la valeur de retour de la fermeture, `case` par `case`. Une inconnue ne mérite
la file « mesure live » que lorsque le code ne la contient pas.

Ce qui demanderait encore le jeu — et qui n'est utile qu'à la recette, pas à la
conception : voir la fenêtre ImGui s'ouvrir à la place de la native.

---

Voir aussi : `entity_context_menu_re.md` (l'entrée de menu et le code 42),
`docs/address_manifest.md`, `project_equip_window_re` (la fenêtre d'équipement du
joueur et son module `EquipTweaks`), `project_doll_composer`,
`project_item_skill_desc_window_re`.
