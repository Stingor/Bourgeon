# Menu contextuel clic-droit sur une entité — RE complète

Client **20250716** (no-ASLR : Ghidra == IDA == live). Relevé statique + vérifications
live x32dbg, menu OUVERT sur une entité `BL_PC` (un NPC scripté sous forme de joueur,
« Blissey(NPC) »).

Complément de [`entity_nameplate_re.md`](entity_nameplate_re.md), qui décrit la brique
d'en dessous (dictionnaire de noms, quadtree de picking, plaque de nom au survol).
Ce document-ci décrit **ce qui se passe quand on clique**, des deux boutons.

---

## 1. Vue d'ensemble : quatre briques, pas une

```
GameMode_ProcessMouseWorldInput  0x00c76400        ← UNE passe, CHAQUE frame
   │
   ├─ Mouse_UpdateFrameState        0x005fd760     ① états de bouton de la frame
   ├─ UIWindowMgr_DispatchMouseInput 0x00a46380    ② une fenêtre native mange-t-elle la souris ?
   ├─ TileQuadTree_QueryPoint       0x00a797b0     ③ QUELLE entité est sous le curseur (quad)
   ├─ GameMode_ShowEntityContextMenu 0x00c6e990    ④ CLIC DROIT  → construit le menu
   └─ GameMode_RouteHoverAndClick   0x00c756a0     ⑤ SURVOL + CLIC GAUCHE → curseur, attaque, NPC
        └─ CursorMgr_UpdateHover    0x00c78180
```

Le menu lui-même est une **fenêtre native générique**, `UIMenuWnd` **id 0x12**, que
`GameMode_ShowEntityContextMenu` remplit ligne à ligne. Le clic sur une ligne revient
au mode de jeu par **`CMode::SendMsg` message 24**, qui relit un **code d'action** dans
un vecteur porté par le `CGameMode`, et le dispatche vers 33 blocs distincts.

---

## 2. ① États de bouton — `Mouse_UpdateFrameState` 0x005fd760

Structure `g_Mouse` **0x011E40D0** :

| offset | adresse | rôle |
|---|---|---|
| +0x04 | `0x011E40D4` | `g_MouseScreenX` (x écran de la frame) |
| +0x08 | `0x011E40D8` | `g_MouseScreenY` |
| +0x0C / +0x10 | | dx / dy depuis la frame précédente |
| +0x14 | `0x011E40E4` | **état bouton GAUCHE** de la frame |
| +0x18 | `0x011E40E8` | **état bouton DROIT** de la frame |
| +0x1C | `0x011E40EC` | molette (copie de +0x28, remise à 0 chaque frame) |
| +0x20 / +0x24 | | x/y bruts posés par le WndProc |
| +0x2C / +0x2D | | état BRUT précédent (gauche / droit) |
| +0x2E / +0x2F | | état BRUT courant, posé par le WndProc |
| +0x38 | | délai de répétition (ms) |

**Les cinq états** (calculés par bouton, à partir du brut courant et du brut précédent) :

| valeur | signification |
|---|---|
| 0 | relâché, et il l'était déjà |
| **1** | **appui NEUF** (front descendant) |
| **2** | maintenu (drag) |
| **3** | **relâché CETTE frame** (front montant) |
| 4 | appui neuf trop rapproché du précédent (répétition/double) |

🔴 C'est la clé de tout ce qui suit : **le menu contextuel s'ouvre sur l'état 3 du bouton
DROIT** — au *relâchement*, pas à l'appui. Et il se **ferme** sur l'état 3 du bouton
GAUCHE (cf. §6).

Un appui **synthétique** (octet brut +0x2E/+0x2F posé à la main, cf. le plugin QuickCast)
traverse ce pipeline exactement comme un vrai clic.

---

## 3. ③ Le « quad » de picking — ce que la souris désigne

`TileQuadTree_QueryPoint(&g_NameplatePickQuadTree, mx, my)` rend un **quad de 10 floats**
(40 octets) ou `NULL`. Les trois champs qui nous intéressent sont des **entiers** :

| index | offset | contenu |
|---|---|---|
| [0]..[5] | +0x00..+0x14 | rectangle écran (x1,y1,x2,y2) + hauteur |
| **[6]** | **+0x18** | **AID / GID de l'entité** |
| **[7]** | **+0x1C** | **job / classe affichée** |
| **[8]** | **+0x20** | **catégorie de pick** |
| [9] | +0x24 | drapeau (état +0x2c8 de l'acteur) |

### Catégories (`quad[8]`) — posées par les trois `SubmitNameplateQuad`

| cat | posée par | signifie |
|---|---|---|
| **0** | `CActorSprite_SubmitNameplateQuad` 0x00c588b0 | acteur ordinaire : **joueur, monstre, NPC** — les vrais NPC de map (CNpc, vtable 0x010939D4) passent par ICI |
| **1** | `CItem_SubmitNameplateQuad` 0x00d1da70 | 🔴 **OBJET AU SOL**, et rien d'autre (voir l'encadré 2026-08-19) |
| **2** | `SkillUnitActor_SubmitNameplateQuad` 0x00db4d60 | **unité de compétence** (trap, warp posé, plante…) |
| **3** | `CActorSprite…` si type d'acteur `+0x314 == 7` | 🔴 **le PET** (voir l'encadré) |
| **4** | `CActorSprite…` si `Job_IsSpecialUnitId(job)` ou type ∈ {9,10,13,14} | **homoncule / mercenaire / élémentaire** (🔴 *pas* le pet) |

🔴 **Correction 2026-08-06 — la catégorie 3 est le PET, pas un objet au sol.**
`+0x314` est le champ `objecttype` du paquet de spawn, c'est-à-dire l'énumération
`clif_bl_type` de rAthena (0 = PC, 2 = ITEM, 4 = UNSPECIFIED *par défaut*, 5 = NPC,
6 = MOB, **7 = PET**, 8 = HOM, 9 = MER, 10 = ELEM). La valeur 7 est écrite
`mov byte ptr [edi+314h], 7` @**0x00CBAB7D**, dans le sous-type 0 de
`ZC_CHANGESTATE_PET` — le paquet qui déclare « cette entité est ton pet » — et le menu
pet du §5.4b l'exige justement (`+0x314 == 7`). ✅ Vérifié live : l'acteur du pet porte
`+0x314 == 0x07`, et sa vtable (`CNpc`, `0x010939D4`) porte bien
`CActorSprite_SubmitNameplateQuad` à vt+0x14 — donc c'est ce chemin-là, donc cat 3.
Détail complet : [`pet_re.md`](pet_re.md) §2.2.

🔴 **Correction 2026-08-19 — la catégorie 1 est l'OBJET AU SOL, pas le NPC de map**
(vérifié LIVE, x32dbg + Red Potion jetée ; l'hypothèse « objet au sol = cat 0 » du
paragraphe précédent était fausse). Le producteur 0x00d1da70, baptisé ici
`NpcActor_SubmitNameplateQuad` par erreur, est **`vt+0x14` de la classe `CItem`**
(RTTI `.?AVCItem@@`, vtable **0x010932AC**) : il écrit `quad[8] = 1` en dur, ainsi que

- `quad[6]` (AID) = **`CItem+0x17C`**, l'AID du flooritem (< 2 000 000) ;
- `quad[7]` (job) = la **CONSTANTE `0x7D03`** (32003) — jamais un job réel.

Les vrais NPC de map (CNpc, vtable 0x010939D4) portent `CActorSprite_SubmitNameplateQuad`
à vt+0x14 et sortent donc en **cat 0**, classés ensuite au job / au prédicat
hostile-ou-spécial. C'est cette confusion qui donnait aux drops le menu d'un NPC.

**La classe `CItem`** (RE live 2026-08-19) :

- les CItem vivent dans **leur propre liste** du gestionnaire d'acteurs :
  `actorMgr+0x18` (liste MSVC à sentinelle, nœud `{next@0, prev@4, CItem*@8}`) —
  ni dans celle de `ActorListFindByGid` (+0x10, NPC/mobs), ni seulement dans la
  liste de rendu (+0x08, où ils figurent AUSSI) ;
- champs, remplis par sa méthode d'init **0x00d1d390** (vérifiés live sur une
  Red Potion : 501 / 1 / 501 / 4796) :
  `+0x158` = `std::string` du nom reçu (l'ITID en décimal), `+0x170` = nameid
  tronqué uint16 (`atoi` du nom), `+0x174` = octet **identifié**, `+0x178` =
  **nameid complet**, `+0x17C` = **AID** ;
- le nom de plaque vient de **`0x00d5afc0`** `__thiscall(g_Session 0x015FA3C0,
  idstr, identified) → char*` — « identifié » compris : un équipement non
  identifié rend son nom générique ;
- le **clic gauche natif** sur un CItem (branche cat 1 de `CursorMgr_UpdateHover`,
  @0x00c792d6, table 0x00c79590) n'agit que sur `LButtonState == 1` et hors
  ciblage (`gm+0x408 == 0`) : il envoie `OnMsg(0, 0x12, 0, aid, 0…)` (13 args)
  à l'acteur du JOUEUR — `[[gm+0xCC]+0x2C]`, vtable+0x8 — dont l'IA fait le
  trajet puis émet CZ_ITEM_PICKUP. Le clic DROIT natif ne fait RIEN sur un CItem.
  Les deux tables de saut par catégorie d'`UpdateHover` : survol 0x00C7948C
  (cat 1 → 0x00c78757, curseur 9), clic 0x00C79590 (cat 1 → 0x00c792d6).

### Prédicats de classe employés partout

| fonction | adresse | vrai pour |
|---|---|---|
| `Job_IsPlayerJobId` | 0x00d73520 | classes de JOUEUR |
| `Job_IsMonsterId` | 0x00c44470 | 1001..3998 et 20021..32046 |
| `Job_IsNpcOrPortalId` (`sub_D71EC0`) | 0x00d71ec0 | **45..999** ou **10001..20014** |
| `Job_IsSpecialUnitId` (`sub_D8E980`) | 0x00d8e980 | **6001..6052** (homoncule / mercenaire / élémentaire) |
| `Job_IsHomunId` (`sub_D8E8D0`) | 0x00d8e8d0 | 6001..6016 ou 6048..6052 |
| `EntityName_IsHostileOrSpecialUnit` | 0x00d9d220 | état acteur `+0x314 ∈ {1,6,0xC}` |

`job == 45` = **portail / warp** (curseur dédié, cf. §7).

---

## 4. ④ `GameMode_ShowEntityContextMenu` 0x00c6e990

`int __thiscall (CGameMode* this, quad* param_1, int param_2)`
— `param_2` = ce que la passe souris a rendu en ②, non nul si une **fenêtre native**
est sous le curseur.

Appelée **chaque frame** (une seule xref, depuis `GameMode_ProcessMouseWorldInput`
@0x00c765a1), et seulement si la fenêtre **10011** n'existe pas.

### 4.1 Les gardes d'entrée, dans l'ordre

```c
if (g_ReplayActive)                    return;   // 0x015BEECC : lecture de replay
if (g_Mouse_LButtonState == 2)         return;   // un drag gauche est en cours
if (GetAsyncKeyState(VK_SHIFT) >> 8)   return;   // 🔴 Maj enfoncée : pas de menu
is_admin = ActiveIdSet_Contains(g_Account_Aid);  // cf. §4.2
if (!param_1)                          return;   // rien sous le curseur
if (param_2)                           return;   // une fenêtre native mange la souris
if (g_Mouse_RButtonState != 3)         return;   // 🔴 RELÂCHEMENT du bouton droit
if (g_StorageWnd_ptr)                  return;   // 0x0131F770 : storage natif ouvert
```

### 4.2 🔴 `ActiveIdSet_Contains` = la liste `<admin>` du **clientinfo.xml**

`ActiveIdSet_Contains(aid)` @0x00a727f0 balaie le `std::vector<int>`
**`0x0159B8E0` (begin) .. `0x0159B8E4` (end)**. C'est la liste d'AID déclarée
**côté client** dans `clientinfo.xml`. Elle décide, à elle seule, de **six entrées de
menu supplémentaires** (§5.4).

⚠ **Vérifié live 2026-08-05** : dans la session observée, `begin == end == 0` — la liste
est **vide**, et les six entrées admin étaient effectivement absentes du vecteur de
commandes. Un compte « déclaré admin » ne l'est donc pas forcément *du point de vue du
client* : c'est ce vecteur-là qu'il faut regarder, pas la config qu'on croit avoir posée.

C'est un gate **purement client** — le serveur, lui, revalide (`pc_get_group_level`).

### 4.3 Ce que la fonction ÉCRIT dans le `CGameMode`

| offset | type | rôle |
|---|---|---|
| **+0x1CC** | `std::vector<int>` | **codes d'action**, un par entrée de menu, dans l'ordre d'affichage (begin/end/cap à +0x1CC/+0x1D0/+0x1D4) |
| **+0x2E0** | `uint32` | **AID de la cible** du menu |
| **+0x2EC** | `std::string` | **nom** de la cible (recopié en fin de construction) |

Le vecteur est vidé au début par `sub_C84B80(&tmp, begin, end)`, puis rempli par
`StdVectorInt_PushBack` (`sub_7A7FA0` @0x007a7fa0, `__thiscall(vec*, int*)`).

### 4.4 La construction, message par message

```c
if (dword_131F6F8) UIWindowMgr_SaveRectAndCloseWindow(mgr, 0x12);  // ferme un menu resté ouvert
wnd = UIWindowMgr_MakeWindow(mgr, 0x12);                            // UIMenuWnd
gm[0x2E0] = aid;
… pour chaque entrée :  PushBack(gm+0x1CC, &code);  OnMsg(wnd, 31, "texte");
… séparateur :                                      OnMsg(wnd, 72, 0);
OnMsg(wnd, 30, 0);                                   // finalise : mesure + redimensionne
SetPos(wnd, g_MouseScreenX + 1, g_MouseScreenY + 1); // vtable+0x10
```

`dword_131F6F8` **0x0131F6F8** = le slot du window-manager qui porte la fenêtre 0x12
(non nul ⇔ un menu est affiché).

---

## 5. La fenêtre `UIMenuWnd` (id 0x12)

**RTTI live** : vtable `0x01034ABC` → COL à vtable−4 → `.?AVUIMenuWnd@@`.
(IDA nomme son `OnMsg` `UIContextMenuWnd_OnMsg` : nom hérité, le RTTI fait foi.)

### 5.1 Modèle d'instance (confirmé live)

| offset | valeur observée | rôle |
|---|---|---|
| +0x00 | `0x01034ABC` | vtable |
| +0x14 / +0x18 | 213 / 203 | largeur / hauteur (calculées par le msg 30) |
| +0x1C / +0x20 | 931 / 355 | x / y écran (`uiwnd::kOffPosX/Y`) |
| +0x28 | 1 | visible (`uiwnd::kOffVisible`) |
| +0x2C | **0x12** | id de fenêtre |
| +0xB4 | | id de la fenêtre propriétaire (posé par le msg 83) |
| +0xBC | **14** | hauteur d'une ligne, en pixels |
| +0xC0/+0xC4/+0xC8 | | `std::vector<std::string>` des lignes (begin/end/cap) |

### 5.2 `UIMenuWnd::OnMsg` 0x008c4ed0 (vtable+0x94)

| msg | effet |
|---|---|
| **31** (0x1F) | **ajoute une entrée** : pousse **DEUX** `std::string` dans le vecteur +0xC0 |
| **72** (0x48) | **séparateur** : pousse **UNE** chaîne (la statique `off_12046B4`) |
| **30** (0x1E) | **finalise** : mesure la plus large des lignes (`UIText_MeasureWidth`, taille 12), puis `Resize(largeur+6, n × +0xBC / 2)` |
| 75 (0x4B) | vide la liste |
| 83 (0x53) | configure le propriétaire (+0xB4) et une donnée utilisateur (+0xCC) |
| autre | délègue à `UIWindow_OnMsg` 0x00a24d70 |

✅ **Arithmétique vérifiée live** : 12 entrées + 5 séparateurs → `(12×2 + 5) = 29`
chaînes ; hauteur = `29 × 14 / 2` = **203** = exactement la hauteur lue à +0x18.
C'est la preuve que « une entrée = deux chaînes, un séparateur = une ».

### 5.3 🔴 Ce menu n'appartient PAS au monde

La fenêtre 0x12 est un **widget générique**, partagé par une dizaine de fenêtres qui
s'en servent pour leurs propres menus : `UIMessengerGroupWnd`, `UIPartyWnd`,
`UINewSelectCharWnd`, `UIChatWnd`, `QuestTracker`, `UIGuildTotalInfoWnd`,
`UIGuildMemberManageWnd`, `UIGuildSkillWnd`… (xrefs de `dword_131F6F8`).

**Conséquence directe pour Bourgeon** : on ne peut PAS « tuer la fenêtre 0x12 ». La
détruire ou l'empêcher de naître casserait les menus de toutes ces fenêtres-là. Le
point d'interception correct est **`GameMode_ShowEntityContextMenu` elle-même** — une
seule xref, et le seul chemin qui produise le menu *du monde*. C'est ce que fait le
module `EntityContextMenu`.

### 5.4 Composition du menu, branche par branche

Les libellés viennent de `MsgStringTable_GetById` (id = **numéro de ligne 0-based** de
`msgstringtable.txt`), les codes sont ceux poussés dans `gm+0x1CC`.

#### a) Cible = **JOUEUR** (le cas courant)

| ordre | code | msg | libellé | condition |
|---|---|---|---|---|
| — | **28** | 458 `MSI_BAN_USER` | « Kick %s » | **admin client** |
| 1 | **42** | 1360 `MSI_REQ_VIEW_OTHERUSER` | « Check %s's Equipment Info » | ni Hide ni Cloak |
| 2 | **4** | 391 `MSI_REQ_DEAL_WITH2` | « Request a deal with (%s) » | ni Hide ni Cloak |
| 3 | **5** | 392 `MSI_REQ_JOIN_PARTY2` | « Ask (%s) to join your party » | on est en groupe (`dword_15FF804`) et la cible n'y est pas |
| 4 | **36** | 887 `MSI_REQ_JOIN_BABY` | « Send an adoption request to %s » | éligible adoption (`sub_D99860` : niveau ≥ 70, marié, non enfant…) |
| 5 | **22** | 382 `MSI_REQ_JOIN_GUILD` | « Send (%s) a Guild Invitation » | on a une guilde, droit d'invitation, cible sans guilde |
| 6 | **25** | 399 `MSI_REQ_ALLY_GUILD` | « Send an Guild Alliance Request » | on est **maître** de guilde, guilde différente |
| 7 | **26** | 403 `MSI_REQ_HOSTILE_GUILD` | « Set this guild as an Antagonist » | idem |
| 8 | **20** | 360 `MSI_OPEN_1ON1_WINDOW` | « Open 1:1 Chat » | toujours |
| 9 | **10** | 358 `MSI_ADD_TO_FRIEND_CHAR_LIST` | « Register as a Friend » | pas déjà ami, `g_ServerType != 3` |
| 10 | **58** | 2808 `MSI_SEND_RODEX` | « Send a mail » | toujours |
| 11 | **12** / **13** | 3820 / 3819 | « Block Chat » / « Unblock Chat » | bascule selon la liste de blocage |
| — | **44** | 1590 `MSI_REMOVE_EQUIPEDITEM` | « Remove all equipment » | **admin client** |
| — | **23** | 389 `MSI_GIVE_PLUS_MANNER_POINT` | « Align with a Good Point » | **admin client**, `g_ServerType != 3` |
| — | **24** | 390 `MSI_GIVE_MINUS_MANNER_POINT` | « Align with a Bad Point » | idem |
| — | **35** | 713 `MSI_PROHIBIT_LOG` | « Chat block record %d times » | idem |
| — | **34** | *(format brut)* | « %s : %d (GID) : %s » | **admin client** |
| 12 | **63** | 4067 `MSI_MACRO_USER_REPORT` | « Report a User » | toujours |
| 13 | **57** | *(format brut)* | « [C-Code] : … » | la cible est un `CPc` (RTTI) |

✅ **Relevé live** du vecteur `gm+0x1CC` sur la cible « Blissey(NPC) » (AID `0x002DC74B`) :
`42, 4, 5, 22, 25, 26, 20, 10, 58, 12, 63, 57` — 12 entrées, **sans aucune entrée admin**,
ce qui recoupe exactement la liste vide du §4.2.

#### b) Cible = **son propre pet** (`aid == dword_15FB3B0` et acteur `+0x314 == 7`)

| code | msg | libellé |
|---|---|---|
| 33 | 596 `MSI_PET_SHOWINFO` | « Check Pet Status » |
| 29 | 592 `MSI_PET_FEEDING` | « Feed Pet » |
| 30 | 593 `MSI_PET_PERFORMANCE` | « Performance » |
| 32 | 595 `MSI_PET_ACC_OFF` | « Unequip Accessory » |
| 31 | 594 `MSI_PET_RETURN_EGG` | « Return to Egg » |

#### c) Cible = **son homoncule** (`GameMode_IsCurrentId5558` : `session+0x5560 == aid`)

codes **37** (1112 « View Status »), **38** (1113 « Feed »), **39** (1114 « Stand By »).

#### d) Cible = **son mercenaire** (`GameMode_IsCurrentId5608` : `session+0x5608 == aid`)

codes **40** (1112), **41** (1113), **39** (1114).

#### e) Cible = **un joueur en échoppe**, et on est admin (`acteur+0x3E0 == 1`)

code **65**, msg 3493 `MSI_STORE_ASSISTANT_TRADE_FORCED_REMOVAL` « Close stall ».

---

## 6. Le dispatch : `CMode::SendMsg` **message 24**

Fermeture et exécution passent par le même endroit.

### 6.1 Fermeture (dans `GameMode_ProcessMouseWorldInput`, @0x00c76518)

```c
if (g_Mouse_LButtonState == 3) {          // relâchement du bouton GAUCHE
    ...
    if (dword_131F6F8) {
        if (*(uint8*)(menu + 0xDC)) *(uint8*)(menu + 0xDC) = 0;  // laissez-passer : on ignore
        else UIWindowMgr_SaveRectAndCloseWindow(mgr, 0x12);      // sinon on FERME
    }
}
```
Le champ **+0xDC** est le « laissez-passer » qui évite que le menu ne se referme sur le
tout premier relâchement suivant son ouverture.

### 6.2 Exécution — `CMode::SendMsg(24, index)` @0x00c8858e

```c
case 24:                                     // arg2 = index de la ligne cliquée
  if (dword_131F6F8) SaveRectAndCloseWindow(mgr, 0x12);   // le menu se ferme
  if (g_StorageWnd_ptr)                    break;         // rien si le storage natif est ouvert
  count = (gm[0x1D0] - gm[0x1CC]) / 4;
  if (index >= count)                      break;         // borne
  code = ((int*)gm[0x1CC])[index];
  switch (code) { … }                                      // 33 blocs, codes 4..65
```

⚠ Le switch interne est encodé en **table d'octets** `byte_C936B4[code−4]` → table de
saut `jpt_C885E5`. Les codes 6..9, 14..19, 27, 43, 45..56, 59..62 et 64 tombent tous sur
le bloc vide `0x00c892dc` (no-op).

### 6.3 Table complète des actions

`SendMsg(m, a)` = `CGameMode::SendMsg` (vtable+0x18) avec le message `m`.
`aid` = `gm+0x2E0`, `name` = recopié à la demande via `GameMode_CopyEntityName(gm, out, aid)`.

| code | entrée de menu | ce qu'elle FAIT |
|---|---|---|
| **4** | échange | gardes (`gm+0x24C != 1`, aucune fenêtre de dialogue NPC 0x10/0xE2, état acteur ≠ 2) puis **`SendMsg(0x31, aid)`** |
| **5** | invitation groupe | **`SendMsg(0x3B, name)`** — passe le **NOM**, pas l'AID |
| **10** | ajouter en ami | `sub_A2C600(mgr, name)` (fenêtre du window-manager) |
| **11** | *(menu du chat)* | `sub_A4F500(mgr, str(gm+0x1FC))` |
| **12** | bloquer le chat | `Chat_HandleChatMessage(gm, 0x12, name)` |
| **13** | débloquer le chat | `Chat_HandleChatMessage(gm, 0x13, name)` |
| **20** | chuchoter | ouvre la fenêtre de chat 1:1 (nom, ou « GuildMember ») |
| **21** | *(menu du chat)* | `sub_A2C600(mgr, str(gm+0x1FC))` |
| **22** | inviter en guilde | **`SendMsg(0x73, aid)`** |
| **23** | manière **+** | garde `IsGidInActorFilterList(g_Account_Aid)` puis petite fenêtre (0xC0 o, 182×46) |
| **24** | manière **−** | garde `ActiveIdSet_Contains(g_Account_Aid)` puis même fenêtre |
| **25** | alliance de guilde | **`SendMsg(0x77, aid)`** |
| **26** | guilde ennemie | **`SendMsg(0x7D, aid)`** |
| **28** | **expulser (kick)** | **`SendMsg(0x0E, aid)`** |
| **29** | nourrir le pet | refus si la fenêtre d'écriture de courrier est ouverte (msg 2985) ; sinon **boîte de confirmation** (msg 601), callback `0x00c866a0` |
| **30** | pet : performance | **`SendMsg(0x96, 2)`** |
| **31** | pet : retour à l'œuf | **`SendMsg(0x96, 3)`** |
| **32** | pet : retirer l'accessoire | **`SendMsg(0x96, 4)`** |
| **33** | pet : statut | **`SendMsg(0x96, 0)`** + bascule la fenêtre **0x58** |
| **34** | GID (admin) | `sprintf("%s : %d (GID) : %s")` → ligne de **chat** |
| **35** | journal de blocage | `MakeWindow(0x67)` + `OnMsg(0x22, aid)` |
| **36** | adoption | **`SendMsg(0xB3, aid)`** |
| **37** | homoncule : statut | **`SendMsg(0xB8, 0)`** + bascule la fenêtre **0x71** |
| **38** | homoncule : nourrir | **boîte de confirmation** (msg 601), callback `0x00c85d60` |
| **39** | homoncule : stand by | acteur `dword_15FF918` → `vtable+0xCC(cmd 9, 0)` |
| **40** | mercenaire : statut | **`SendMsg(0xBA, 1)`** + bascule la fenêtre **0x7D** |
| **41** | mercenaire : stand by | acteur `dword_15FF9C8` → `vtable+0xCC(cmd 9, 1)` |
| **42** | voir l'équipement | **paquet CZ `0x02D6`** `[opcode:2][aid:4]` |
| **44** | retirer tout l'équipement | garde admin, puis **paquet CZ `0x07F5`** `[opcode:2][aid:4]` |
| **57** | copier le « C-Code » | `GlobalAlloc`/`GlobalLock` → **presse-papier Windows** |
| **58** | envoyer un courrier | **`SendMsg(0x10C, name)`** |
| **63** | signaler un joueur | `MakeWindow(0x2715)` |
| **65** | fermer l'échoppe | **paquet CZ `0x0AF9`** `[opcode:2][aid:4]` |

#### 🔴 Vérification côté serveur (moonlight)

| paquet | handler `clif_packetdb.hpp` | verdict |
|---|---|---|
| `0x02D6` | `clif_parse_ViewPlayerEquip` | ✅ actif |
| `0x07F5` | `clif_parse_GMFullStrip` | ✅ actif |
| **`0x0AF9`** | **aucun** | 🔴 **DÉCONNECTE le joueur** (voir ci-dessous) |

🔴 **`0x0AF9` n'est pas seulement inerte : il est DANGEREUX sur moonlight.**
`clif_parse` (`src/map/clif.cpp:28745`) traite tout opcode dont
`packet_db[cmd].len == 0` comme un paquet inconnu et appelle **`set_eof(fd)`** — la
session est fermée. Un GM qui clique « Close stall » sur une échoppe **se fait
déconnecter lui-même**, sans message. L'entrée native est donc un piège, et le menu
ImGui de Bourgeon ne la propose pas. Le geste équivalent côté moonlight est une
commande d'administration (`@` / socket admin), pas ce paquet.

---

## 7. ⑤ Interactions curseur — survol et clic gauche

`GameMode_RouteHoverAndClick` 0x00c756a0 puis `CursorMgr_UpdateHover` 0x00c78180.
Le comportement dépend d'abord de **`quad[8]`** (la catégorie), puis du job.

| cat | survol | clic |
|---|---|---|
| **0** joueur/mob | curseur selon hostilité / cible valide | cf. détail ci-dessous |
| **1** NPC | curseur **9** (dialogue) | `Actor::OnMsg(18, aid)` = **parler au NPC** |
| **2** unité de skill | curseur 5 ou 10 selon le skill visé | `Actor::OnMsg(103, aid, …)` |
| **3** 🔴 **pet** | curseur 0, **aucune** interaction ici | — |
| **4** homon/merc/élém | curseur 0 (ou 5/11 selon le contexte) | `GameMode_PostActorClickAction(aid, 1)` |

Détail de la catégorie 0 :

- `job == 45` (**portail**) → curseur **7** ;
- `EntityName_IsHostileOrSpecialUnit` → curseur **1**, et le clic envoie
  **CZ `0x0090`** (`CZ_CONTACTNPC`, `[op:2][aid:4][0:1]`) après avoir posé la cellule
  cible — sauf si la barre de chat est en saisie, auquel cas le client affiche le
  msg 4027 à la place ;
- **monstre** → curseur **11** (attaque) ou **5**, selon Maj / le mode de ciblage /
  l'option `0x69` ; le clic passe par `GameMode_PostActorClickAction(aid, flag)` ;
- **joueur** → même chemin, plus les règles PvP/GvG.

`GameMode_PostActorClickAction` 0x00c753a0 : hors mode ciblage, il envoie à l'acteur
propre `OnMsg(10, aid, flag)` ; en mode ciblage 2/4, `OnMsg(41, aid, skill)` puis
`OnMsg(90, level)`. Un garde-fou refuse l'action et affiche le msg 307 quand le **poids**
dépasse 90 %.

### 7.1 🔴 Le bouton DROIT agit AUSSI sur le monde — et c'est un piège

`CursorMgr_UpdateHover` teste `g_Mouse_RButtonState == 1` (appui neuf) **au même
endroit** que `LButtonState == 1` (`LABEL_136`/`LABEL_137`, @0x00c78893) : les deux
boutons ouvrent donc le même switch d'actions. Ce que le bouton droit déclenche
dépend ensuite de la catégorie :

| cat | l'appui DROIT déclenche-t-il quelque chose ? |
|---|---|
| **0**, entité hostile/spéciale | **OUI** — le bloc n'teste aucun bouton : cellule cible + `Actor::OnMsg(16)` + **CZ `0x0090`**. C'est **le PNJ qui répond au clic droit**. |
| 0, monstre | non pour l'attaque (`LButtonState == 1` requis) — **mais oui** pour les ordres de compagnon : `RButtonState == 1` + **Alt** commande l'homoncule, +Alt+Maj le mercenaire |
| 0, joueur | non (`LButtonState == 1` requis) |
| **1** NPC de map | non (`LButtonState == 1` requis) |
| **2** unité de compétence | **OUI** en mode ciblage 5 : aucun test de bouton |
| **4** homoncule/merc/élém | **OUI** : `GameMode_PostActorClickAction` sans test de bouton |

**Conséquence — le bug qu'il faut connaître avant de remplacer ce menu** : le menu
s'ouvre au **relâchement** (état 3) tandis que l'interaction part à l'**appui**
(état 1), *une frame plus tôt*. Un clic droit sur un PNJ scripté fait donc les
**deux** : il ouvre le dialogue **et** le menu. Le client d'origine ne le voyait pas,
puisqu'il n'ouvre de menu que sur un joueur ou un compagnon — c'est-à-dire jamais
sur les catégories qui réagissent au bouton droit.

**Fenêtre de tir pour corriger.** Dans la passe souris, les seize lecteurs de
`g_Mouse_RButtonState` se répartissent de part et d'autre de notre point
d'interception :

| lecteur | position vs `ShowEntityContextMenu` |
|---|---|
| `Mouse_UpdateFrameState` (écrit les états) | avant |
| `UIWindowMgr_DispatchMouseInput` (1, 3, 4) | **avant** |
| `Camera_DragControl` | **avant** — la rotation de caméra est donc hors d'atteinte |
| `CursorMgr_UpdateHover` (1, 2) | **après** |
| `GameMode_GroundClick_RequestMove` (1, 2) | après |
| `GameMode_RepeatActorAction`, `sub_C79610` (1) | après |
| `ProcessMouseWorldInput` lui-même (`== 2`, @0x00c767e6) | après |

Effacer l'état du bouton droit **depuis le détour** ne peut donc désarmer que les
actions du monde, jamais l'interface native ni la caméra. Et l'écriture ne survit pas
à la frame : `Mouse_UpdateFrameState` recalcule tout depuis les octets **bruts** du
WndProc, si bien que le relâchement produit quand même son état 3.

Options de configuration lues sur ce chemin : `0x69` (attaque au clic simple),
`0x6D`, `0x2F` (afficher les noms de joueurs), `0xC9` (infobulle des icônes de statut).

---

## 8. Table des adresses

| adresse | symbole |
|---|---|
| `0x00c76400` | `GameMode_ProcessMouseWorldInput` (la passe souris, chaque frame) |
| `0x00c6e990` | **`GameMode_ShowEntityContextMenu`** (construit le menu — point d'interception) |
| `0x00c86740` | `CMode::SendMsg` (switch 335 cases ; **case 24** @0x00c8858e = clic sur une entrée) |
| `0x00c885e5` | table de saut du switch interne (33 codes) ; remap `0x00c936b4`, cibles `0x00c9362c` |
| `0x008c4ed0` | `UIMenuWnd::OnMsg` (vtable+0x94) |
| `0x01034abc` | vtable `UIMenuWnd` |
| `0x0131f6f8` | slot du window-manager portant la fenêtre 0x12 |
| `0x007a7fa0` | `StdVectorInt_PushBack` (`__thiscall(vec*, int*)`) |
| `0x00a727f0` | `ActiveIdSet_Contains` (liste `<admin>` de clientinfo.xml, `0x0159B8E0..E4`) |
| `0x00c68e50` | `GameMode_CopyEntityName(gm, out, aid)` |
| `0x00c756a0` | `GameMode_RouteHoverAndClick` |
| `0x00c78180` | `CursorMgr_UpdateHover` |
| `0x00c753a0` | `GameMode_PostActorClickAction(gm, aid, flag)` |
| `0x00c764a0` | `CursorMgr_SetType` |
| `0x005fd760` | `Mouse_UpdateFrameState` |
| `0x00a797b0` | `TileQuadTree_QueryPoint` |
| `0x011e40d0` | `g_Mouse` (+0x14 gauche, +0x18 droit) |
| `0x0159b8e0` | vecteur des AID admin (begin) / `0x0159b8e4` (end) |
| `GameMode+0x1CC` | `std::vector<int>` des codes d'action du menu |
| `GameMode+0x2E0` | AID de la cible du menu |
| `GameMode+0x2EC` | `std::string` nom de la cible |

---

## 9. Conséquences pour Bourgeon

1. **Point d'interception unique** : `GameMode_ShowEntityContextMenu`. Une seule xref,
   appelée chaque frame, et c'est elle qui décide d'ouvrir. La détourner suffit à rendre
   le menu natif du monde définitivement muet — **sans toucher à la fenêtre 0x12**, qui
   sert à dix autres fenêtres (§5.3).
2. **Rejouer, ne pas réécrire.** Pour exécuter une action, on n'a pas besoin de refaire
   les paquets : il suffit de reproduire les trois gestes du natif —
   `gm+0x2E0 = aid`, vider le vecteur (`gm+0x1D0 = gm+0x1CC`), y pousser le code, puis
   `SendMsg(24, 0)`. Toutes les gardes natives (dialogue NPC en cours, poids, droits de
   guilde, confirmations) restent jouées. C'est ce que fait `EntityContextMenu`.
3. 🔴 **Hors frame ImGui.** `SendMsg` peut ouvrir une **modale bloquante** (nourrir le
   pet/homoncule) qui relance le tick du mode : l'action est donc empilée pendant le
   rendu et rejouée depuis `Bourgeon::OnProcessInput`
   (cf. [[feedback_no_native_cmd_during_imgui_frame]]).
4. Le natif n'ouvre le menu que pour **joueur / pet / homoncule / mercenaire**. Mobs,
   NPC, unités de compétence et objets au sol n'en ont jamais eu — c'est l'espace que
   le menu ImGui occupe, derrière son opt-in. ⚠ Seuls **mobs et NPC** y portent une
   action de jeu (attaquer, fiche, interagir) ; unités de compétence, objets au sol et
   entités non classées ne donnent que de l'identité brute (GID, quad de pick), donc
   du **débogage** : `EntityContextMenu` ne les ouvre que sous le réglage staff, et y
   range aussi les copies d'AID/GID de toutes les autres cibles.
5. 🔴 **Un clic droit ne peut pas faire deux choses.** Ouvrir le menu sur des entités
   que le client ne servait pas fait apparaître le conflit décrit au §7.1 : le
   dialogue du PNJ part à l'appui, le menu s'ouvre au relâchement. `EntityContextMenu`
   efface donc `g_Mouse_RButtonState` (états **1** et **4** seulement) depuis le
   détour, et uniquement quand l'entité visée est une cible de son menu. L'état
   « maintenu » (2) est laissé intact, et **Alt + clic droit sur un monstre** est
   exclu de bout en bout : c'est l'ordre à l'homoncule/au mercenaire.
6. **Où le client lit « déjà en groupe / déjà en guilde ».** Les deux conditions du
   §5.4a sont vérifiables sans rien envoyer, et `EntityContextMenu` s'en sert pour
   **griser** l'entrée (le natif, lui, la faisait disparaître) :
   - **groupe** — `std::list<PartyMember>` à `session+0x17B8` : sentinelle
     `+0x17BC`, taille `+0x17C0` (celle que rend `Social_GetPartyMemberCount`
     0x00d5cf50, et dont ChatWindow se sert déjà). Nœud `{next@0, prev@4,
     valeur@8}`. ✅ **Relevé live** (2026-08-05, groupe d'un seul membre,
     nœud @0x471BA010) :

     | offset nœud | = valeur | contenu relevé |
     |---|---|---|
     | +0x08 | +0x00 | `1` (drapeau) |
     | +0x0C | +0x04 | `2000001` — **l'AID du membre**, égal à `g_Account_Aid` |
     | +0x10 | +0x08 | `150000` |
     | +0x14 | +0x0C | `std::string` « Stingor » (taille +0x24, cap +0x28) |
     | +0x2C | +0x24 | `std::string` « gonryun.rsw » (la map) |

     C'est donc la clé de `Social_FindPartyMemberByAid` (0x00d5d650), et Bourgeon
     compare **l'AID**. Le natif du menu cherche par nom (`sub_D5D960`) : clé plus
     fragile, deux `std::string` à lire au lieu d'un `cmp`.

     🔴 **La liste ne suffit pas.** Le natif ne testait que « la cible n'est pas
     dans NOTRE groupe » — or le serveur refuse **tout** invité déjà associé à un
     groupe (`party_invite`, `PARTY_REPLY_JOIN_OTHER_PARTY`,
     moonlight/src/map/party.cpp:461). L'entrée native était donc cliquable pour
     rien sur un joueur du groupe d'en face. Bourgeon grise les deux cas et lit le
     second dans la **plaque de nom** : `CNameInfo+0x1C` non vide ⇒ la cible est
     en groupe (c'est le `(party643077673)` affiché derrière le pseudo). La liste
     ne sert plus qu'à distinguer les deux messages.

     ⚠ **Piège de test, pas de code** : trois « corrections » successives ont été
     écrites contre un faux négatif — la cible d'essai était dans un AUTRE groupe
     (`party643077673` contre `party18595316`), et le groupe ne comptait qu'un
     membre. Vérifier `count` à `+0x17C0` **avant** de conclure qu'une détection
     de groupe est cassée.
   - **blocage du chat = un ÉTAT, une seule entrée.** Les codes 12/13 ne sont pas
     deux commandes offertes au choix : le natif interroge
     `ChatBlockList_Contains` (0x005ee940) et n'en propose qu'**une**. Cette liste
     est un `std::set<std::string>` derrière le pointeur global **0x01251824** :
     arbre à `objet+0x18` (`_Myhead` ; `sub_5EE730` insère dans `this+24`), racine
     = `_Myhead->_Parent`, nœud `{_Left@0, _Parent@4, _Right@8, _Isnil@0x0D,
     std::string@0x10 (taille +0x20, cap +0x24)}` (offsets lus dans `sub_5EE3A0`).
     Bourgeon la **lit** au lieu d'appeler le prédicat : celui-ci prend sa
     `std::string` **par valeur** (0x18 octets sur la pile, détruits par
     l'appelée), ce qui obligerait à fabriquer une chaîne du client et à allouer
     sur SON tas dès 16 caractères.
     ⚠ Le libellé natif ment par omission : l'action émet **CZ 0x00CF**
     (`CZ_SETTING_WHISPER_PC`) et le serveur ne lit `sd->ignore[]` que sur le
     chemin du **chuchotement** (moonlight clif.cpp:14795, intif.cpp:1301) — chat
     public, groupe et guilde ne sont pas filtrés. C'est dit en infobulle.
   - ⚠ **Le dictionnaire de noms n'a pas d'entrée pour SOI.**
     `CNameDict_GetEntryOrRequest(gm+0x160, g_Account_Aid)` rend un `CNameInfo`
     vide **statique** (0x01251678 en relevé), tous champs à `""`. Nos propres
     nom / groupe / guilde vivent ailleurs : `session+0x54A4` = notre nom de
     groupe (vidé par `sub_D56530` et `sub_D70220` à la dissolution),
     `session+0x551C` = notre nom de guilde.
   - **guilde** — `ActorList_FindByGID(gm+0xCC, aid)` puis **`vtable+0xC4`** sur
     l'acteur rend son **id de guilde** (`call [edx+0C4h]` @0x00c6f4e2). Le natif
     exige en plus `dword_159C234` (droit d'invitation, sémantique non tranchée —
     non repris côté Bourgeon).
7. ⚠ **`EntityName_IsHostileOrSpecialUnit` n'est pas décoratif.** Le natif refuse tout
   menu quand il rend vrai (§4.1) — c'est ce qui empêche de proposer « échanger » à
   une kafra, dont la classe est une classe de **joueur**. Un remplaçant qui classe
   les entités sur le seul `job` se fait piéger.
8. **Outillage NPC de l'administrateur** (niveau de groupe **>= 99**, `IsAdmin()`).
   Trois entrées de plus sur un NPC : *recharger son fichier de script*, *déplacer
   ici*, *décharger* (avec confirmation). Ce sont `@reloadnpcfile`, `@npcmove` et
   `@unloadnpc`.

   🔴 **Elles passent par CZ 0x0F25 (`CZ_BOURGEON_NPC_ADMIN`), PAS par une commande
   `@` rejouée dans le chat.** Les trois atcommands résolvent leur cible avec
   `npc_name2id`, dont la clé est **`nd->exname`** — le nom *unique* du serveur
   (`strdb_put(npcname_db, nd->exname, nd)`, moonlight/src/map/npc.cpp). Le client,
   lui, ne connaît que **`nd->name`**, le nom *affiché* de la plaque. Les deux
   diffèrent dès qu'il y a un `#suffixe` ou un `duplicate` : la commande aurait
   répondu « This NPC doesn't exist » précisément sur les NPC où l'outil sert le
   plus. Le **GID** est ce que le pick fournit déjà et ce que `map_id2nd` prend
   directement. Et le rechargement n'était de toute façon pas exprimable côté
   client : ce qui se recharge est le **fichier**, dont le chemin ne vit que dans
   `nd->path`.

   Le seuil est **99**, pas 80 : l'inspecteur de propriétés (CZ 0x0F22) *affiche*,
   ces trois-là *modifient le monde pour tous les joueurs connectés*. Gate refait
   **côté serveur** dans `clif_parse_bourgeon_npc_admin`. Aucun ZC en retour — le
   compte rendu part en `clif_displaymessage`, le canal des atcommands, et
   s'affiche dans le chat.

   ⚠ Côté serveur, `npc_unloadfile()` finit par `npc_delsrcfile()` et le dernier
   `npc_unload()` libère l'entrée de `npc_path_db` : **`nd` et `nd->path` sont morts
   après l'appel**. Le chemin est copié dans un tampon local avant, et `nd` mis à
   `nullptr`.
