# Le pet — fiche, entité et menu contextuel — RE complète

Client **20250716** (no-ASLR : Ghidra == IDA == live). Relevé statique + vérifications
live x32dbg le 2026-08-06, **fiche de pet ouverte et menu contextuel déployé** sur un
pet « Gremlin » (classe 1632, AID `0x002E455C`, intimité 400, faim 84).

Trois briques distinctes, souvent confondues :

| brique | ce que c'est | où |
|---|---|---|
| **l'entité** | un acteur `CNpc` marqué `+0x314 == 7` | monde 3D |
| **la fiche** | `UIPetInfoWnd`, fenêtre **id 88 (0x58)** | interface |
| **le menu contextuel** | 5 entrées de la fenêtre générique 0x12 | cf. [`entity_context_menu_re.md`](entity_context_menu_re.md) |

🔴 Ce document **corrige une erreur** de `entity_context_menu_re.md` §3 — voir §2.2.

---

## 1. Les globals du pet propre — bloc `0x015FB3B0`

Tous écrits par le **handler `ZC_PROPERTY_PET`** (§4.1) et par
`ZC_CHANGESTATE_PET` (§4.2). ✅ Toutes les valeurs ci-dessous ont été **relevées live**.

| adresse | nom | type | rôle | relevé live |
|---|---|---|---|---|
| `0x015FB3B0` | `g_Own_PetAid` | int | **AID/GID** de l'entité pet | `0x002E455C` |
| `0x015FB3B4` | — | int | `rename_flag` (0 = renommable une fois) | 0 |
| `0x015FB3B8` | — | **char[32]** | **nom** du pet (le paquet n'en donne que 24) | `"Gremlin"` |
| `0x015FB3D8` | — | int | **accessoire** équipé (ITID ; 0 = aucun) | 0 |
| `0x015FB3DC` | — | int | **classe** du pet (= mob id) | 1632 |
| `0x015FB3E0` | — | int | **niveau** | 199 |
| `0x015FB3E4` | — | int | **faim** (0..100) | 84 |
| `0x015FB3E8` | — | int | (lu par le seul getter `0x00D34CA0`) | 100 |
| `0x015FB3EC` | — | int | **intimité** (0..1000) | 400 |
| `0x015FB3F0` | — | int | **index d'inventaire de l'œuf** (−1 = aucun) | −1 |
| `0x015FB3F4` | — | int | **faim PRÉCÉDENTE** (sauvée avant écrasement) | 88 |

⚠ `0x015FB3DC` est typé **`char[]` dans l'IDB** : le pseudo-code affiche donc
`*(_DWORD *)dword_15FB3DC = …` comme s'il s'agissait d'un pointeur. C'est faux —
l'assembleur dit `mov dword_15FB3DC, eax` (0x00CBAB6D, 0x00CBACAA). Ne pas propager.

✅ **Cohérence live** : faim courante 84 < faim précédente 88 — le pet avait faim et
descendait d'un cran, exactement ce que produit `ZC_CHANGESTATE_PET` sous-type 2 (§4.2).

---

## 2. L'entité pet

### 2.1 C'est un `CNpc`, pas une classe dédiée

✅ **RTTI relevé live** : l'acteur du pet (`0x476B37F0` dans la session observée) porte
la vtable **`0x010939D4`**, dont le COL (vtable−4 → `0x010D3F30`) donne
**`.?AVCNpc@@`**. Il n'existe **aucune classe `CPet`** : le client réutilise la classe
des NPC et ne distingue le pet que par un octet.

| champ | offset | contenu |
|---|---|---|
| vtable | +0x00 | `0x010939D4` (`CNpc`) |
| GID | **+0x110** | `g_Own_PetAid` (clé de `ActorList_FindByGID`) |
| **type d'acteur** | **+0x314** | **7** ✅ relevé live |
| horodatage de bavardage | +0x27C | `timeGetTime()` du dernier événement (§2.4) |

### 2.2 🔴 `+0x314` = le champ `objecttype` du paquet de spawn

L'octet `+0x314` **n'est pas une invention du client** : les handlers de spawn
(`GameMode_OnRecv_ActorSpawn` @0x00CC985F, `…_Named` @0x00CC9DD4,
`…_ActorEntryUnified` @0x00CCA5E9, …) le recopient **tel quel depuis le paquet**
(`mov [ebx+314h], al`). C'est le champ `objecttype` de `ZC_NOTIFY_STANDENTRY` &
consorts, c'est-à-dire l'énumération **`clif_bl_type`** de rAthena :

| valeur | `clif_bl_type` | posé par |
|---|---|---|
| 0 | `CLIF_BL_PC` | `OwnPlayer_ApplyLookGlobalsToActor` @0x00D4316D |
| 2 | `CLIF_BL_ITEM` | paquet de spawn |
| **4** | `CLIF_BL_UNSPECIFIED` | **valeur par défaut** — `CActorSprite_InitDefaults` @0x00C45F47 |
| 5 | `CLIF_BL_NPC` | paquet de spawn |
| 6 | `CLIF_BL_MOB` | paquet de spawn |
| **7** | **`CLIF_BL_PET`** | **`ZC_CHANGESTATE_PET` sous-type 0** @0x00CBAB7D |
| 8 | `CLIF_BL_HOM` | paquet de spawn |
| 9 | `CLIF_BL_MER` | paquet de spawn |
| 10 | `CLIF_BL_ELEM` | paquet de spawn |

🔴 **Correction de [`entity_context_menu_re.md`](entity_context_menu_re.md) §3.** Ce
document-là écrit « cat 3 · `CActorSprite…` si type d'acteur `+0x314 == 7` · **objet au
sol (item drop)** » et range le pet en cat 4. Les deux affirmations sont fausses :

- **`+0x314 == 7` signifie PET**, prouvé deux fois plutôt qu'une :
  1. *en assembleur brut* — `mov byte ptr [edi+314h], 7` @**0x00CBAB7D**, dans le
     sous-type 0 de `ZC_CHANGESTATE_PET`, c'est-à-dire le paquet qui déclare
     « cette entité est ton pet » ;
  2. *live* — l'acteur `0x476B37F0` (dont le `+0x110` vaut `g_Own_PetAid`) porte bien
     `+0x314 == 0x07`.
- donc `CActorSprite_SubmitNameplateQuad` (@0x00C58C3F : `if (+0x314 == 7) quad[8] = 3`)
  classe le **pet en catégorie 3**, et jamais en catégorie 4. ✅ La vtable de l'entité
  porte bien `CActorSprite_SubmitNameplateQuad` à **vt+0x14** (relevé live dans
  `0x010939D4`), donc c'est bien ce chemin-là qui s'applique.
- la **catégorie 4** reste `Job_IsSpecialUnitId(job)` (6001..6052) ou type ∈ {9,10,13,14},
  soit **homoncule / mercenaire / élémentaire — sans le pet**.
- l'étiquette « objet au sol » n'a aucun support dans le code : un item au sol porte
  `objecttype == 2`, ce qui tombe dans le `else` (catégorie **0**). ⚠ Non vérifié live,
  contrairement au reste de ce paragraphe.

⚠ Une **deuxième** écriture de 7 existe, @0x00CF3014 dans
`World_SpawnOwnPlayerActorFromLookGlobals`, juste avant un
`Effect_SpawnPrimitiveById(0x3C3)` : très probablement la recréation de l'acteur du pet
au chargement de carte depuis les globals du §1. Non confirmé — à ne pas citer comme acquis.

### 2.3 Ce que le type 7 change dans le comportement

| fonction | adresse | effet du `+0x314 == 7` |
|---|---|---|
| `CActorSprite_SubmitNameplateQuad` | 0x00C58A2C / 0x00C58C3F | plaque de nom **+50 px** de haut, catégorie de pick **3** |
| `CActorSprite_StartAction` | 0x00C55C3A | animation spécifique |
| `CActorSprite_UpdateMotionAndPosition` | 0x00C478CE | **bavardage spontané** (§2.4) |
| `GameMode_ShowEntityContextMenu` | 0x00C6ECDB | ouvre **le menu pet** (§3) |
| `CursorMgr_UpdateHover` | 0x00C7860B | curseur au survol |
| `GameMode_RepeatActorAction` | 0x00C77167 | action répétée |
| `Actor_OnMsg` | 0x00D4763E | routage de messages |

### 2.4 Le bavardage spontané — `CActorSprite_UpdateMotionAndPosition` @0x00C478CE

```c
if (actor[0x314] == 7 && actor[0x110] == g_Own_PetAid && actor[0x27C] > 0) {
    if (timeGetTime() > actor[0x27C] + 3000) {        // 0xBB8 = 3 s
        if (rand() % 100 < 25) {                      // 🔴 25 % seulement
            faim  = Pet_GetHungerRank(g_Own_PetHungry);      // 0x00D82190
            intim = Pet_GetIntimacyRank(g_Own_PetIntimacy);  // 0x00D82050
            a = sub_D81F60(ctx, faim, intim, 4);
            b = sub_D83520(ctx, g_Own_PetClass, 4, faim);
            actor->OnMsg(148, b);  actor->OnMsg(148, a);
        }
        actor[0x27C] = 0;
    }
}
```

🔴 **C'est le client du MAÎTRE qui décide de la réplique**, l'envoie en
**CZ `0x01A9`** (§4.4), et le serveur la relaie brute à l'entourage en
`ZC_PET_ACT 0x01AA` — cf. le commentaire déjà posé sur `PetAct_OnPacket` (0x00CD13F0),
dont Bourgeon se sert déjà pour suffixer « (pet) » dans la chatbox ImGui.

### 2.5 Les deux rangs — faim et intimité

| `Pet_GetHungerRank` 0x00D82190 | faim | | `Pet_GetIntimacyRank` 0x00D82050 | intimité |
|---|---|---|---|---|
| 4 | 91..100 | | 4 | > 900 |
| 3 | 76..90 | | 3 | 751..900 |
| 2 | 26..75 | | 2 | 251..750 |
| 1 | 11..25 | | 1 | 101..250 |
| 0 | 0..10 | | 0 | ≤ 100 |

---

## 3. Le menu contextuel — 5 entrées

Construit par `GameMode_ShowEntityContextMenu` (0x00C6E990), dont la condition d'entrée
est, **exactement** :

```c
if (quad[6] /*+0x18, l'AID*/ == g_Own_PetAid && actor[0x314] == 7)
```

soit : **le pet doit être le nôtre**, et il doit porter le type 7. Un pet d'autrui
n'ouvre aucun menu.

✅ **Relevé live** du vecteur `CGameMode+0x1CC` (menu ouvert sur le pet) :
`33, 29, 30, 32, 31` — 5 entrées, dans cet ordre, aucun séparateur.

| ordre | code | msg | libellé (lu live) | action (`CMode::SendMsg` msg 24) |
|---|---|---|---|---|
| 1 | **33** | 596 (0x254) `MSI_PET_SHOWINFO` | `Check Pet Status` | `SendMsg(150, 0)` + bascule la fenêtre **88** |
| 2 | **29** | 592 (0x250) `MSI_PET_FEEDING` | `Feed Pet` | confirmation (msg 601) → callback 0x00C866A0 |
| 3 | **30** | 593 (0x251) `MSI_PET_PERFORMANCE` | `Performance` | `SendMsg(150, 2)` |
| 4 | **32** | 595 (0x253) `MSI_PET_ACC_OFF` | `Unequip Accessory` | `SendMsg(150, 4)` |
| 5 | **31** | 594 (0x252) `MSI_PET_RETURN_EGG` | `Return to Egg` | `SendMsg(150, 3)` |

✅ **Arithmétique de la fenêtre 0x12 revérifiée live** : instance `0x247DA0B0`,
vecteur de lignes `+0xC0..+0xC8` = 240 octets ÷ 24 (`std::string`) = **10 chaînes**
= 5 entrées × 2, hauteur `+0x18` = 10 × 14 ÷ 2 = **70** px, largeur 112.
Les 10 chaînes lues : `Check Pet Status` ×2, `Feed Pet` ×2, `Performance` ×2,
`Unequip Accessory` ×2, `Return to Egg` ×2.

---

## 4. Les paquets

### 4.1 `ZC_PROPERTY_PET` **0x01A2** — case **418** du dispatch @0x00CA6657

Recopié dans le buffer global `0x015E8198`. Découpage **identique à rAthena**
(`clif_send_petdata` / `clif_pet_data`) :

| offset paquet | champ | destination |
|---|---|---|
| +2 | `name[24]` | `0x015FB3B8` |
| +26 | `rename_flag` (byte) | `0x015FB3B4` |
| +27 | `level` (word) | `0x015FB3E0` |
| +29 | `hungry` (word) | `0x015FB3E4` (l'ancienne valeur → `0x015FB3F4`) |
| +31 | `intimate` (word) | `0x015FB3EC` |
| +33 | `equip` (word) | `0x015FB3D8` |
| +35 | `class` (word) | `0x015FB3DC` |

longueur **37**.

🔴 **Le handler a DEUX branches** (`cmp ax, 2710h` @0x00CA666E) :

- `level < 10000` → chemin normal : les globals sont écrits, puis, **si la fiche est
  ouverte** (`0x0131F874` non nul), le client appelle
  `UIPetInfoWnd_RefreshIntimacyLabel`, pousse le nom dans le champ d'édition
  (`fiche+0xB4` → `vt+0xD4`) et invalide la fenêtre (`vt+0x98`) ;
- `level >= 10000` → **rien de tout cela** : `level -= 10000`, puis
  `MakeWindow(mgr, 0x0C)` — la fenêtre de **description** — et les stats sont écrites
  dans *son* instance (`+0x218` level, `+0x21A` faim, `+0x21C` intimité, `+0x21E` nom,
  `+0x236` accessoire, `+0x238` rename_flag) avant `vt+0x94`. C'est le mécanisme
  d'**aperçu** (stats du pet contenu dans un œuf), invisible de rAthena.

### 4.2 `ZC_CHANGESTATE_PET` **0x01A4** — `sub_CBAAE0` @0x00CBAAE0

`[op:2][type:1][GID:4][data:4]`, longueur 11. `type` = `a2+2`, `data` = `a2+7`.

| type | rAthena | ce que fait le client |
|---|---|---|
| **0** | pre-init | `g_Own_PetAid = GID` ; `OnMsg(34)` ; 🔴 **`actor[0x314] = 7`** |
| **1** | intimacy | met à jour l'intimité, rafraîchit la fiche, et si le **rang** change → notification. Puis `sub_C65A60` (§5.4) |
| **2** | hunger | sauve l'ancienne faim dans `0x015FB3F4`, écrit la nouvelle, `OnMsg(34)`, rafraîchit |
| **3** | accessory | `data != 0` → `CActorSprite_SetBodyActFromPath` ; `data == 0` → sprite de base (`vt+0x54`) |
| **4** | performance | `CActorSprite_SetOpt3(data)` + `vt+0x3C` |
| 5 | hairstyle | **absent** (tombe dans le `default` → no-op) |
| **6** | capture | `data == 0` → `Pet_ResetOwnGlobals`, rafraîchit l'inventaire, **ferme la fiche (88)** ; `data == 1` → insère l'œuf en inventaire, mémorise son index dans `0x015FB3F0`, **ferme la fenêtre 90** (`UIPetEggListWnd`, cf. §6.4) |
| **7** | — | boîte de message, msg 4344 (0x10F8) |

⚠ Le type 1 porte un cas particulier : **monture Doram** (classe 4050..4052) avec
intimité < 100 **et** faim < 30 → ligne de chat msg 1188 (0x4A4).

#### 🔴🔴 Le sous-type 6 ne veut PAS dire « le pet s'en va »

`clif_send_petdata` calcule sa valeur ainsi (moonlight `clif.cpp:11380`, identique
à rAthena amont) :

```c
case CHANGESTATEPET_UPDATE_EGG:
    value = ( pd.pet.intimate == 1 ) ? 0 : 1;
```

avec `PET_INTIMATE_AWKWARD == 1` : **un pet d'intimité normale rangé dans son œuf
envoie donc `data = 1`, jamais 0.** Et le sous-type 6 est aussi envoyé **à
l'éclosion** (`pet_birth_process`, pet.cpp:1117) — il ne signifie donc rien de
plus que « l'œuf a changé d'état en inventaire ».

Or la branche `data == 1` du client est gardée par `UIWindowMgr_FindWindow(mgr,
90)` : hors d'une session `@hatch`, **elle ne fait strictement rien**. Sur un
retour à l'œuf ordinaire, le client ne réinitialise donc **aucun** global, et
**sa propre fiche 88 reste à l'écran sur un pet absent** — défaut natif, pas
régression d'un remplaçant.

#### 🔴 « Le pet est parti » ne se lit ni sur l'AID, ni sur la classe

`Pet_ResetOwnGlobals` (**0x00D8A830**) tient en deux affectations :

```c
void Pet_ResetOwnGlobals() {
    g_Own_PetEggInvIndex = -1;   // 0x015FB3F0
    g_Own_PetClass       = -1;   // 0x015FB3DC
}
```

**`g_Own_PetAid` n'est jamais remis à zéro** — ni ici, ni ailleurs : les 44 xrefs
de `0x015FB3B0` ne contiennent aucune écriture hors du sous-type 0. Après un
retour à l'œuf, il désigne donc indéfiniment un pet qui n'est plus là.

Le test du client est la **classe**, pas l'AID — `if (g_Own_PetClass != -1)`
@**0x00CBB416** (`GameMode_OnRecv_ZC_DELETEITEM_FROM_BODY`, le décrément
d'inventaire qui repère la destruction de l'œuf porté).

⚠ Mais cette classe-là **ne retombe que par `Pet_ResetOwnGlobals`**, donc
seulement sur le sous-type 6 **avec `data == 0`** — c'est-à-dire, d'après le
calcul ci-dessus, uniquement quand l'intimité du pet vaut exactement 1. Sur un
retour à l'œuf ordinaire, **elle reste**, et l'AID aussi.

🔴 **Le seul observable du chemin ordinaire est l'ACTEUR.** `pet_return_egg` finit
par `unit_free(pd, CLR_OUTSIGHT)` : l'entité disparaît, et
`Actor_FindByGid(g_Own_PetAid)` (**0x00D806A0**, `__stdcall`, qui résout lui-même
le mode actif) rend `nullptr`. `rag::pet::Present()` exige donc les **trois** :
`aid != 0`, `class > 0`, **et** l'acteur vivant.

⚠ Et l'absence d'acteur ne se lit pas sur un seul tick : un changement de map vide
la liste, et le chargement se termine **avant** que les spawns n'arrivent. Côté
Bourgeon, la fiche cesse de se DESSINER dès que l'acteur manque (le retour à l'œuf
se voit tout de suite) mais ne se FERME qu'après une absence continue d'environ
une seconde, compteur remis à zéro pendant le chargement.

⚠ Le clrtype ne discrimine pas : les warps ordinaires passent eux aussi par
`CLR_OUTSIGHT` (`pc_setpos(sd, …, CLR_OUTSIGHT)`), donc observer
`ZC_NOTIFY_VANISH` n'aurait rien apporté de plus.

### 4.3 `ZC_PET_ACT` **0x01AA** — `PetAct_OnPacket` @0x00CD13F0

Déjà documenté dans l'IDB (bavardage / émoticône, `PetTalkTable.xml`). Le paquet ne
porte **aucun texte**.

### 4.4 Les paquets sortants — `CMode::SendMsg` (0x00C86740)

| message | opcode CZ | forme | site |
|---|---|---|---|
| **145** | **`0x01A5`** `CZ_RENAME_PET` | `[op:2][name:24]` | @0x00C8FAC5 |
| **149** | **`0x019F`** `CZ_TRYCAPTURE_MONSTER` | `[op:2][GID:4]` | @0x00C8FDF4 |
| **150** | **`0x01A1`** `CZ_COMMAND_PET` | `[op:2][cmd:1]` | @0x00C8FC2A |
| *(case précédent)* | **`0x01A9`** `CZ_PET_ACT` | `[op:2][data:4]` | @0x00C8FBE7 |

Les commandes de `CZ_COMMAND_PET` : **0** = info, **1** = nourrir, **2** = performance,
**3** = retour à l'œuf, **4** = retirer l'accessoire.

🔴 **« Nourrir » n'est PAS envoyé aveuglément.** Le case 150 branche sur `cmd == 1` :

```c
if (cmd == 1) {
    itid  = Pet_GetFoodItidByClass(g_Own_PetClass);       // sub_D81FA0
    count = ItemSkillMgr_GetInfoByResId_UNIFIED(...)[0x10];
    if (count <= 0) {                                     // 🔴 pas de nourriture
        // msg 591 (0x24F) MSI_NOT_EXIST_PET_FOOD, couleur 0x1E1EF5,
        // émis DEUX fois : ChatAction(1, …) puis ChatAction(0x13, …)
        return;                                           // rien n'est envoyé
    }
}
SendPacket(0x01A1, cmd);
```

C'est un **gate purement client** : le serveur ne voit jamais la tentative. Un
remplaçant qui envoie `0x01A1 / cmd=1` sans ce test change le comportement observable
(cf. [[feedback_replace_native_fill_gaps]]).

Envoi : `CRagConnection_GetInstance()` → `PacketLen_Get(opcode)` →
`CRagConnection_SendPacket(len, &pkt)` (0x00C14920).

---

## 5. La fiche — `UIPetInfoWnd`, fenêtre **88 (0x58)**

### 5.1 Identité

| | |
|---|---|
| RTTI | **`.?AVUIPetInfoWnd@@`** (TypeDescriptor `0x0123F0BC`) |
| vtable | **`0x01030900`** (COL à `0x010C2E60`) — 53 entrées, +0x00..+0xD0 |
| `OnCreate` | vt+0x3C = **0x00879220** |
| `OnMsg` | vt+0x94 = **0x00885F60** |
| slot du window-manager | **`0x0131F874`** (= `mgr` + 227×4 dans `UIWindowMgr_FindWindow`) |
| taille / position | 280 × 180 ✅ live |

⚠ La vtable **suivante** en mémoire est `.?AVUIHomunInfoWnd@@` : les deux fiches sont
jumelles, ne pas se tromper de table en relisant un dump.

### 5.2 Modèle d'instance ✅ intégralement vérifié live (`0x48B97ED8`)

| offset | contenu | relevé |
|---|---|---|
| +0x00 | vtable | `0x01030900` |
| +0x14 / +0x18 | largeur / hauteur | 280 / 180 |
| +0x1C / +0x20 | x / y écran | 1054 / 369 |
| +0x28 | visible | 1 |
| +0x2C | **id de fenêtre** | **88** |
| +0x8C | id du bouton de fermeture | 201 |
| **+0xB4** | `UIEdit` du **nom** (`SetText` = `vt+0xD4`) | pointeur |
| **+0xBC** | bouton **« rewrite »** (renommer), id 184 | pointeur |
| **+0xC0** | bouton **« Pet Command »**, id 268 | pointeur |
| **+0xC4** | **boîte modale en attente** (garde de réentrance) | 0 |
| **+0xC8** | contrôle de **jauge** (`sub_8364D0`), 120×9 @(145,105) | pointeur |
| **+0xCC** | `std::string` du **libellé d'intimité** | **`" Neutral"`** (taille 8, cap 15) |
| **+0xE4** | *byte* : l'œuf du pet figure dans **`AutoFeedingPetList`** (§8) | **0** |
| **+0xE8** | `UIToggleButton` d'**auto-feeding** (créé seulement si +0xE4 == 1) | **0** |

✅ Les deux derniers champs se confirment l'un l'autre : le `Gremlin_Egg` n'est pas
listé pour l'auto-feeding ⇒ aucun bouton, exactement ce que montre l'instance.

### 5.3 Le libellé d'intimité — `sub_888F00` @0x00888F00

Appelé par `OnCreate` et par tout rafraîchissement. Écrit le `std::string` de +0xCC :

| intimité | msg |
|---|---|
| ≤ 100 | 672 (0x2A0) |
| 101..250 | 673 (0x2A1) |
| 251..750 | **669 (0x29D)** ← ✅ `" Neutral"`, relevé live à 400 |
| 751..900 | 674 (0x2A2) |
| 901..1000 | 675 (0x2A3) |
| > 1000 | 676 (0x2A4) |

⚠ Cas particulier **monture Doram** (`Job_ResolveMountedClassFromOption` ∈ 4050..4052)
et intimité ≤ 100 : les msgs deviennent 1022 (≤ 20), 1021 (≤ 40), 672 (> 40).

Puis la **jauge** de +0xC8 est alimentée par `sub_863790(pct)`, `pct` étant calculé en
flottant à partir de l'intimité et des bornes du palier courant. La fenêtre n'est
invalidée (`vt+0x98`) que **si le libellé a changé** (comparaison `sub_4DCC30`).

### 5.4 Ouverture automatique — `sub_C65A60` @0x00C65A60

🔴 La fiche **s'ouvre toute seule** quand l'intimité tombe :

```c
if (Pet_GetIntimacyRank(intimacy) == 0 && intimacy < 100) {
    if (/* ce pet n'est pas déjà dans la table des avertis */) {
        if (!g_PetInfoWnd_ptr) MakeWindow(mgr, 0x58);   // 🔴 ouverture forcée
        sub_C82940(g_Own_PetAid, 1);
    }
}
// et quand l'intimité repasse au-dessus de 99, l'entrée est retirée de la table
```

La table est une hash-map indexée par **FNV-1a de l'AID** (`0x811C9DC5`, facteur
`16777619`) portée par le `CGameMode` (+394/+395). Conséquence pratique : un
remplaçant ImGui qui se contente de masquer la fenêtre native la verra **réapparaître**
à la prochaine baisse d'intimité (cf. [[feedback_no_offscreen_hide]] et
[[reference_native_window_toggle_router]] : il faut **détruire**, pas masquer).

### 5.5 `UIPetInfoWnd::OnMsg` @0x00885F60

Garde d'entrée : si `+0xC4` est non nul et que l'émetteur n'est pas cette modale-là,
le message est **jeté** (`return 0`).

**msg 6 — un contrôle a été actionné**, `a4` = id du contrôle :

| id | effet |
|---|---|
| **201** | fermeture → `SaveRectAndCloseWindow(mgr, 88)` |
| **184** | **renommer** : refus si longueur ≥ 24 (msg 682) ; refus si le nom contient l'une des deux chaînes interdites `0x0120451C` / `0x01204534` (msg 2812) ; confirmation msg 2931 ; validation `sub_A85BE0` (msg 2932 si invalide) ; sinon **`SendMsg(145, texte)`** et recopie dans `g_Own_PetName`. Enregistré au replay. |
| **213** | bascule **auto-feeding** → `CZ_Config_SendType2(0|1)`. Garde : l'émetteur doit être le toggle de `+0xE8`. Enregistré au replay. |
| **268** | ouvre le **menu de commandes**, fenêtre **260 (0x104)** (§5.6) |
| **488** | callback de la confirmation « nourrir » : si réponse 187 → **`SendMsg(150, 1)`**, sauf si la fenêtre **10011** existe (alors msg 3550 en chat). Remet `+0xC4` à 0. |

**msg 39 — clic dans le menu 260** :

| id | effet |
|---|---|
| 467 | nourrir → refus si la **rédaction de courrier** est ouverte (msg 2985) ; sinon confirmation msg 601, la modale est mémorisée dans `+0xC4` |
| 468 | `SendMsg(150, 2)` performance |
| 469 | `SendMsg(150, 4)` retirer l'accessoire |
| 470 | `SendMsg(150, 3)` retour à l'œuf |
| 471 | **évolution** : 🔴 refusée si `intimité <= 900` (msg 2576) ; sinon ferme 260, ouvre **261 (0x105)** et lui passe l'ITID via `OnMsg(142, itid, mob)` |

Tous ces chemins finissent par `SaveRectAndCloseWindow(mgr, 260)`.

### 5.6 Le menu de commandes — fenêtre **260 (0x104)**

Construit dans le msg 6/268 : quatre entrées fixes puis les évolutions disponibles.

| entrée | msg | id |
|---|---|---|
| `Feed Pet` | 592 (0x250) | 467 |
| `Performance` | 593 (0x251) | 468 |
| `Unequip Accessory` | 595 (0x253) | 469 |
| `Return to Egg` | 594 (0x252) | 470 |
| *(une par recette)* | 2568 (0xA08), nom du mob cible | **471** (+ le mob en paramètre) |

⚠ **L'ordre d'affichage n'est pas l'ordre de construction** : les quatre `std::string`
sont bâties dans l'ordre 0x250, 0x251, 0x252, 0x253 mais **poussées** dans l'ordre
0x250, 0x251, **0x253**, **0x252**. Lire la table ci-dessus, pas le pseudo-code.

Puis `OnMsg(40)` finalise, `Resize(110, 16 × (n+4))`, `OnMsg(83, 88)` déclare la fiche
comme propriétaire, et la fenêtre est posée à (160, 44) — coordonnées passées par
`sub_A1EF70`.

### 5.7 Ce que `OnCreate` construit (0x00879220)

- bouton de fermeture (id 201) ;
- `UIEdit` du nom en (135, 24), 80×16, **limite 50** — pré-rempli avec
  `g_Own_PetName`, ou avec le nom de classe (`Job_GetDisplayNameOrResName`) si le
  global est vide ;
- bouton **`btn_rewrite`** (3 bitmaps) en (230, 21), id 184 ;
- bouton **« Pet Command »** en (270 − largeur, 44), id 268 ;
- jauge en (145, 105) ;
- libellé d'intimité initialisé au msg 676 puis **immédiatement recalculé** par
  `sub_888F00` ;
- ⚠ `Session_GetEquipInfoByInvIndex(ctx, …, g_Own_PetEggInvIndex)` est appelé avec
  l'index de l'œuf — **qui vaut −1** quand le pet est sorti (relevé live) ; l'ITID
  obtenu est passé à `PetAutoFeeding_IsEggListed`, d'où le `+0xE4 == 0` observé ;
- si `+0xE4 == 1`, le `UIToggleButton` d'auto-feeding, positionné après la largeur
  mesurée du msg 2577 (0xA11), initialisé depuis `0x015FB99C`.

Illustration du pet : `userinterface\illust\pet_noimage.bmp` en repli.

---

## 6. Capture, incubation, éclosion

### 6.1 Les cinq classes UI du pet

Résolues par leur RTTI (TypeDescriptor → COL → vtable). 🔴 Deux d'entre elles
**empruntent** des méthodes à d'autres fenêtres : le nom IDA ment, le RTTI fait foi.

| classe (RTTI) | vtable | id | `OnCreate` (vt+0x3C) | `OnMsg` (vt+0x94) |
|---|---|---|---|---|
| `UIPetInfoWnd` | `0x01030900` | **88** | `0x00879220` | `0x00885F60` |
| `UIPetEggListWnd` | `0x010344D4` | **90** | ⚠ `UIItemIdentifyWnd_OnCreate` `0x008A8470` | `0x008C6AB0` |
| `UIPetTamingDeceiveWnd` | `0x01034D44` | **91** | *(nullsub)* | `0x008C6CF0` |
| `UIPetEvolutionWnd` | `0x0103F3EC` | **261** (0x105) | `0x00970F70` | ⚠ `UISkillListWnd_OnMsg` `0x00971560` |
| `UIPetInfoBarGraph` | `0x0102BA18` | *(contrôle)* | *(nullsub)* | hérité |

Les slots du window-manager : **90 → `mgr+0x390`**, **91 → `mgr+0x394`** (`0x0131F87C`),
88 → `mgr+0x38C` (`0x0131F874`).

### 6.2 L'apprivoisement, de bout en bout

```
① item d'apprivoisement utilisé
      ↓  serveur
② ZC_START_CAPTURE 0x019E  (case 414)  →  sub_D0CC60  @0x00D0CC60
      pose un MODE DE CIBLAGE sur le CGameMode :
        +0x408 = 2      (type de ciblage)
        +0x40C = 10000  (pseudo-skill « capture »)
        +0x410 = 5
        +0x414 = 0
      ↓  le joueur clique un monstre
③ GameMode_PostActorClickAction  @0x00C753A0
      🔴 if (+0x40C == 10000) le garde de POIDS (90 %) est court-circuité
      mode 2 ou 4 →  Actor::OnMsg(41, aid, 10000)  puis  Actor::OnMsg(90, 0)
      ↓
④ CMode::SendMsg case 69, sous-cas skill == 10000  @0x00C8DA65
      🔴 refus si aid == g_Own_PetAid          (pas son propre pet)
      🔴 refus si acteur+0x314 == 0            (la cible est un JOUEUR, CLIF_BL_PC)
      MakeWindow(mgr, 0x5B = 91)  puis  OnMsg(80, 0, aid)
      ↓
⑤ clic sur le bouton de la fenêtre 91  →  UIPetTamingDeceiveWnd_SendCatchRequest (vt+0x64)
      SendMsg(149, this+0xC0)  →  CZ_TRYCAPTURE_MONSTER 0x019F  [op:2][aid:4]   ✅ §6.3
      ↓  serveur
⑥ ZC_TRYCAPTURE_MONSTER 0x01A0  [op:2][result:1]  (case 416) → sub_D0D180 @0x00D0D180
      OnMsg(80, 1, result ? 7 : 3)  sur la fenêtre 91
      ↓  si réussite
⑦ ZC_CHANGESTATE_PET 0x01A4 type 6, data 1  →  l'œuf entre en inventaire (§4.2)
```

`UIPetTamingDeceiveWnd::OnMsg` @0x008C6CF0 tient dans huit lignes — c'est un simple
porte-état, tout le reste vient de la composition (`UIWindow_composite_ctor`) :

```c
if (a3 == 80) {
    if (!a4) { this[0xC0] = a5; return 0; }   // a4 == 0 → l'AID de la cible visée
    this[0xC4] = a5;                          // a4 == 1 → 7 = réussite, 3 = échec
    this[0xB4] = 7; this[0xC8] = 7;           // états d'animation
}
```

Valeurs initiales posées par le ctor `sub_88E990` : `+0xB4 = 1`, `+0xC4 = -1`
(aucun résultat), `+0xC8 = 10000`, `+0xCC = -1`.

### 6.3 ✅ Le maillon `CZ 0x019F` — tranché au débogueur

`CMode::SendMsg` **case 149** @0x00C8FDF4 construit `[op:2 = 0x019F][edx:4 = aid]` et
l'envoie via `PacketLen_Get` + `CRagConnection_SendPacket`.

✅ **Vérifié live le 2026-08-06** (breakpoints + capture réelle en jeu). Les trois
points de contrôle sont tombés dans l'ordre :

| bp | relevé |
|---|---|
| `0x00D0CC60` | `mov [ecx+40Ch], 2710h` — le mode capture s'arme au **double-clic sur l'item**, `CGameMode = 0x23F79438` |
| `0x00C8DA9B` | `push 5Bh` — le **clic sur le monstre** a passé les deux gardes → `MakeWindow(91)` |
| `0x00C8FDF4` | `mov eax, 19Fh` — **le paquet part**, `edx = 0x002E4931` = le GID de la cible |

**L'appelant** (lu dans la pile, `[ebp+4]` deux cadres plus haut = `0x008B9985`) est
**`UIPetTamingDeceiveWnd_SendCatchRequest`** @0x008B9930 — **vt+0x64** de la fenêtre 91
(unique xref `0x01034DA8`, vtable `0x01034D44`) :

```c
if (!g_ReplayActive && !this[0xD0]) {        // 🔴 verrou anti-double-envoi
    this[0xB8] = 1;  this[0xC4] = 7;         // état « en attente »
    this[0xD0] = 1;                          // posé ici, JAMAIS remis à 0
    SendMsg(GameMode_GetActive(), 149, this[0xC0]);   // l'AID de OnMsg(80,0,aid)
}
```

Chaîne d'appel complète relevée : `UIWindowMgr` (0x00A46E91) →
`UIPetTamingDeceiveWnd_SendCatchRequest` (@0x008B997D `push 95h`) → *[le hook Bourgeon
de `SendMsg`, en `ddraw.dll` ~`0x56FCA473`]* → `CMode::SendMsg` case 149.
Le cadre du hook explique qu'une lecture naïve de `[ebp+4]` sorte du module principal
(cf. [[feedback_own_calls_bypass_own_hook]]).

🔴 **`+0xD0` n'est jamais remis à zéro** : une instance de la fenêtre 91 n'émet **qu'une
seule** tentative. C'est le garde-fou anti-spam de capture, entièrement côté client.

⚠ **Erreur de méthode à ne pas répéter** — une version antérieure de ce document
affirmait « aucun appelant trouvé, le case est peut-être mort ». C'était faux : le
balayage de `push 95h` avait bel et bien remonté `0x008B997D`, mais **seul le premier
hit de la liste avait été ouvert** (les autres classés faux positifs sans vérification).
Un scan n'est une preuve d'absence que s'il est **entièrement dépouillé**.

### 6.4 L'incubation — `ZC_PETEGG_LIST 0x01A6` (case 422) @0x00CA6539

Paquet **de longueur variable** : `[op:2][len:2][index:2] × n`.

```c
count = (len - 4) / 2;                 // sub 4 ; shr 1
wnd   = MakeWindow(mgr, 0x5A);         // fenêtre 90 = UIPetEggListWnd
wnd->OnMsg(75);                        // vide la liste
for (i = 0; i < count; i++) {
    idx  = *(int16*)(pkt + 4 + 2*i);   // index d'INVENTAIRE de l'œuf
    info = Session_GetEquipInfoByInvIndex(ctx, &buf, idx);
    if (info valide) wnd->OnMsg(31, idx);   // une entrée par œuf
}
```

`UIPetEggListWnd::OnMsg` @0x008C6AB0 :

| msg / id | effet |
|---|---|
| **31** | ajoute l'œuf : `Session_GetEquipInfoByInvIndex` → `Inventory_AppendNewItem(this+0xCC)` → `OnMsg(60)` |
| 60 | recalcule la plage de défilement (`UIMakingArrowListWnd_UpdateScrollRange`) |
| 7 / 9 / 10 | défilement (page/ligne) |
| 6 · **184** | **OK** → entrée sélectionnée (`this+0xBC`) via `UIItemIdentifyWnd_GetCandidateListEntryAt` → **`SendMsg(146, index)`** |
| 6 · **185** | **Annuler** → ferme la fenêtre 90 |

**`SendMsg` case 146** @0x00C8F474 → **`CZ_SELECT_PETEGG 0x01A7`** `[op:2][index:2]`.

✅ **Relevé live 2026-08-06**, fenêtre ouverte par `@hatch` sur un unique « Poring Egg »
(instance `0x1742F3A8`, lue via le slot `mgr+0x390` = `0x0131F878`) :

| offset | valeur | sens |
|---|---|---|
| +0x00 | **`0x010344D4`** | vtable `UIPetEggListWnd` ✅ |
| +0x14 / +0x18 | 200 / 200 | largeur / hauteur |
| +0x1C / +0x20 | 760 / 412 | x / y écran |
| +0x28 | 1 | visible |
| +0x2C | **90** | id de fenêtre ✅ |
| +0xB8 | 0 | offset de défilement |
| **+0xBC** | **−1** | **index sélectionné** — aucune ligne cochée |
| +0xC4 | 4 | lignes par page (le pas des msg 9/10) |
| **+0xCC** | pointeur | **`_Myhead`** de la liste d'entrées (`Inventory_AppendNewItem(this+0xCC, …)`) |
| **+0xD0** | **1** | **`_Mysize`** — nombre d'entrées (un seul œuf) |
| +0xD4 | `"Pet List"` | titre de la fenêtre (`std::string` SSO) |

✅ **Re-relevé live 2026-08-10** sur une liste de **trois** œufs (instance
`0x18DC8DC8`) : `+0xBC` = **−1** et `+0xC4` = 4 confirmés, et `+0xCC`/`+0xD0` sont
bien un **`std::list<ItemInfo>` MSVC**, pas un tableau plat — `+0xCC` pointe la
**sentinelle** (dont le `_Myval` n'est jamais construit : on y lit du remplissage
`0xABABABAB`), et chaque nœud vaut `{next, prev, ItemInfo}`, la valeur commençant
donc en `nœud+0x08`. Les deux entrées lues portaient `ItemInfo+0x04` = **14** et
**18**, avec `+0x10` (quantité) = 1.

🔴 **L'index qui circule est le même partout.** `clif_sendegg` envoie
`client_index(i)` = **i+2** ; le client le range tel quel dans `ItemInfo+0x04` ;
`UIPetEggListWnd::OnMsg` (msg 6 / id 184) repasse ce `+0x04` à `SendMsg(146, …)` ;
et `clif_parse_SelectEgg` retranche les 2. Un remplaçant n'a donc **rien à
convertir** : il renvoie l'entier reçu. (Vérifié : slots serveur 12 et 16 → 14 et
18 sur le fil → 14 et 18 dans les `ItemInfo`.)

**Ce que Bourgeon en fait** : la fenêtre 90 ne naît plus, `ZC_PETEGG_LIST` est
**remplacé** (`RegisterReplaceOpcode`, révocable) et `PetWindow::DrawHatchWindow`
prend sa place — le handler natif ne faisait rien d'autre que créer et remplir sa
fenêtre, il n'y a donc aucun devoir orphelin. On y ajoute la seule chose qui
manquait : l'**œuf vierge est signalé**, et le bouton refuse avec la raison au
lieu de laisser partir un paquet que `pet_select_egg` jette en silence.

🔴 **Garde implicite invisible en statique** : `+0xBC` vaut **−1** tant que rien n'est
sélectionné. Un clic sur **OK** dans cet état fait rendre « non trouvé » à
`UIItemIdentifyWnd_GetCandidateListEntryAt`, et **aucun `SendMsg(146)` n'est émis** —
le client ne peut donc pas envoyer un `CZ 0x01A7` sans sélection.

⚠ Côté serveur, le filtre de `clif_sendegg` (moonlight clif.cpp:10974) est très
permissif : il ne teste que `type == IT_PETEGG` et `amount > 0`, **jamais `card[0]`**.
Un œuf vierge obtenu par `@item` apparaît donc dans la liste, mais `pet_select_egg`
(pet.cpp:1207) refusera l'éclosion faute de `card[0] == CARD0_PET`.
Et `clif_sendegg` n'a **qu'un seul appelant** : la commande **`@hatch`**, qui exige
`pet_id <= 0` — un pet déjà sorti bloque toute la liste.

⚠ Côté serveur, `clif_parse_SelectEgg` (moonlight clif.cpp:17701) **exige**
`sd->menuskill_id == SA_TAMINGMONSTER && sd->menuskill_val == -1`, et **retranche 2**
de l'index reçu. Un client qui enverrait `0x01A7` hors de ce contexte est ignoré en
silence.

### 6.5 Les autres pseudo-skills du case 69 — et pourquoi deux sont morts

Le même sous-switch sur l'id de compétence sert à trois gestes « qui n'en sont pas ».
Chacun a son paquet déclencheur, qui met le client en mode de ciblage :

| pseudo-skill | posé par | déclencheur | envoie |
|---|---|---|---|
| **10000** (0x2710) | `GameMode_OnRecv_ZC_START_CAPTURE` 0x00D0CC60 | ZC `0x019E` (case 414) | capture → fenêtre 91 (§6.2) |
| **20000** (0x4E20) | `sub_D0CCC0` 0x00D0CCC0 | ZC **`0x01E4`** `ZC_START_COUPLE` (case 484) | **CZ `0x01E5`** (mariage) |
| **20001** (0x4E21) | `sub_D0CC30` 0x00D0CC30 | ZC **`0x01F8`** (case 504) | **CZ `0x01F9`** (adoption) |

🔴 **Les deux derniers sont INATTEIGNABLES sur moonlight** — vérifié côté serveur :

- `clif_marriage_process` (celui qui émettrait `0x01E4`) est **entièrement commenté**
  dans `moonlight/src/map/clif.cpp:12193`, sous la note d'origine rAthena :
  *« This packet while still implemented by the client is no longer being officially used. »* ;
- `0x01F8` n'apparaît que dans la table (`packet(0x01f8,2)`, clif_packetdb.hpp:251) :
  **aucun émetteur** dans tout `src/map`.

Sans ces deux ZC, le client n'entre jamais dans les modes de ciblage 20000/20001, et
les branches correspondantes du case 69 ne s'exécutent pas — **y compris la ligne de
chat `"[ (val1 == 20001) ]"`** qu'on lit dans le désassemblage (@0x00C8DB24,
couleur `0x6E96FF`). C'est un `printf` de débogage laissé par les développeurs
d'origine, mais **personne ne l'a jamais vu en jeu, et c'est normal** : le chemin est
mort par le haut. ⚠ Ne pas le citer comme « visible en jeu ».

L'adoption réellement pratiquée emprunte un tout autre chemin : le **menu contextuel**,
code 36 → `SendMsg(0xB3, aid)` (cf. [`entity_context_menu_re.md`](entity_context_menu_re.md) §6.3),
qui aboutit au même `CZ 0x01F9` sans passer par le pseudo-skill.

📌 **Leçon de méthode** : trouver un `ChatAction` dans le désassemblage prouve seulement
que le *code* existe. Tant que le **déclencheur** n'est pas remonté jusqu'à un émetteur
serveur vivant, on ne peut rien affirmer sur ce que le joueur voit
(cf. [[feedback_dead_code_unverified_offsets]]).

### 6.6 🔴 L'œuf vu comme un OBJET — ses quatre « cartes » n'en sont pas

Un œuf de familier (ITID **9001..9499**, `IsPetEggItem` @0x00D8EBE0) range la fiche de
son occupant dans ses quatre emplacements de carte. Mais **ce que le client en reçoit
n'est pas ce que le serveur en garde** : `clif_addcards`
(moonlight `src/map/clif.cpp:2864`) réécrit les quatre slots avant l'envoi.

| slot | base serveur (`pet_create_egg`, pet.cpp:1413) | ce qui arrive au client |
|---|---|---|
| `card[0]` | `CARD0_PET` = **0x0100** | **0** |
| `card[1]` | mot **bas** du `pet_id` | **0** |
| `card[2]` | mot **haut** du `pet_id` | `card[3] >> 1` = **rang d'intimité 1..5** |
| `card[3]` | bit 0 = renommé, bits 1-3 = rang | `card[3] & 1` = **renommé** |

Le rang suit `pet_get_card3_intimacy` (pet.cpp:612) : 1 Awkward · 2 Shy · 3 Neutral ·
4 Cordial · 5 Loyal. Le serveur le réécrit à **chaque** retour à l'œuf
(`card[3] &= 1` puis `|= rang << 1`) et ne le relit jamais : il n'existe que pour
l'affichage.

Trois conséquences pour tout code client :

1. **`CARD0_PET` n'atteint jamais le client.** Le chercher pour reconnaître un œuf
   habité rend systématiquement « œuf vierge ». (C'est le bug qu'a produit la
   première version de `rag::pet::DecodeEggCards`.)
2. **Le `pet_id` non plus** : aucun paquet ne le porte. Il n'y a rien à afficher.
3. Les deux slots restants sont des **nombres**, pas des ITID. Passés au rendu de
   cartes, ils donnent « #5 » et le nom de l'objet d'id 1.

Un œuf **vierge** (obtenu par `@item`) a ses quatre slots à zéro côté serveur et
emprunte le chemin ordinaire de `clif_addcards` : il arrive donc à zéro lui aussi.
C'est ce qui le distingue — *tout à zéro = personne dedans* — et `pet_select_egg`
(pet.cpp:1207) refusera bien son éclosion, faute de `CARD0_PET` dans SA base.

⚠ L'œuf de l'occupant **sorti** reste en inventaire : le serveur pose
`attribute = 1` pour le masquer de la grille (`pet_recv_petdata`), et le client le
saute lui-même dans `Inventory_HitTestSlotSkippingHiddenEgg` @0x009396F0. Ce drapeau
est le même que celui d'un objet **cassé** — d'où le suffixe « - Broken » à corriger
en « éclos » sur un œuf.

---

## 7. L'évolution

### 7.1 Les recettes viennent d'un `.lub`, pas du serveur

`CPetEvolutionMgr` (RTTI `.?AVCPetEvolutionMgr@@`, vtable `0x00FE7E7C`) charge
**`system\PetEvolutionCln_true.lub`** (`CPetEvolutionMgr_InitFromFile` 0x00631A70).

La fonction exposée au Lua est `PetEvolution_Lua_InsertEvolutionRecipe` @0x00632AA0 :

```lua
InsertRecipe(oeufSource, oeufCible, itemMateriel, quantite)   -- 4 nombres
```

Ses gardes, dans l'ordre, avec les messages qu'elle renvoie en 2ᵉ valeur Lua :

| garde | message |
|---|---|
| chaque argument est un nombre | `"argment 1st/2nd/3th/4th is must number"` |
| `IsPetEggItem(arg1)` (0x00D8EBE0) | `"PetEgg Item %d not found or not petegg"` |
| `IsPetEggItem(arg2)` | `"Revolutuion PetEgg Item %d not found or not petegg"` *(sic)* |
| `arg4 > 0` | `"Material Item Cnt %d Error"` |
| insertion réussie | `"good"` / sinon `"InsertRecipe : %s"` |

🔴 **Une recette va d'un ŒUF à un ŒUF**, jamais d'un mob à un mob — les deux premiers
arguments doivent passer `IsPetEggItem`. L'interface, elle, affiche le **nom du mob** :
`UIPetInfoWnd_OnMsg` convertit l'ITID d'œuf en classe via `sub_D823F0`, puis
`Job_GetDisplayNameOrResName`.

`PetEvolution_LookupRequirements` @0x00631E10 rend, pour un couple
(œuf source, œuf cible), le `std::vector<std::pair<itemId, amount>>` des matériaux.
Les recettes vivent dans la **map à `CPetEvolutionMgr+0x08`**.

🔴 **À ne pas confondre avec la liste d'auto-feeding** (§8), qui est un **autre**
conteneur du même manager (le vecteur plat `+0x18..+0x1C`) — l'erreur est facile, les
deux sont peuplés par le même `.lub`.

### 7.2 `CZ_PET_EVOLUTION 0x09FB` — construit à la main, longueur variable

Émis depuis la fenêtre **261** (`UIPetEvolutionWnd`), dans le `OnMsg` qu'elle partage
avec `UISkillListWnd`, @**0x0097192C** :

| offset | champ |
|---|---|
| +0 | `PacketType` = **0x09FB** |
| +2 | `PacketLength` = **8 + (fin − début)** octets |
| +4 | `EvolvedPetEggID` (**4 octets** — variante moderne) |
| +8 | le vecteur de matériaux, recopié **brut** par `memcpy` |

Le vecteur porte des paires `{index:2, amount:2}` — 4 octets par matériau, ce qui
recoupe exactement `struct pet_evolution_items` (moonlight `packets_struct.hpp:2311`).
⚠ Le désassemblage fait `sar ebx,1` puis `8[ebx*2]` : les deux se compensent, la
longueur est bien `8 + taille_du_vecteur`. Ne pas y lire un `count*2`.

Juste après l'envoi : `SaveRectAndCloseWindow(261)` — donc la fenêtre est **détruite**
(cf. l'annotation de cette fonction dans l'IDB : elle ne fait pas que sauver un rect).

✅ **Relevé live 2026-08-06** — fenêtre ouverte sur une évolution **Poring → Mastering**
(instance `0x14176308`, capturée par un breakpoint sur son `OnCreate` 0x00970F70, `ecx`) :

| offset | valeur | sens |
|---|---|---|
| +0x00 | **`0x0103F3EC`** | vtable `UIPetEvolutionWnd` ✅ |
| +0x14 / +0x18 | 290 / 400 | largeur / hauteur |
| +0x1C / +0x20 | 735 / 295 | x / y écran |
| +0x28 | 1 | visible |
| +0x2C | **261** (0x105) | id de fenêtre ✅ |
| **+0xC0** | **9001** | œuf **source** = `Poring_Egg` |
| **+0xC4** | **9069** | œuf **cible** = `Mastering_Egg` — c'est le `[eax+0xC4]` copié dans le paquet ✅ |
| +0xC8 / +0xCC / +0xD0 | vecteur, **16 octets** | les matériaux — 2 entrées de 8 octets |

🔴 **Ce que seul le live pouvait établir : le `.lub` client et le `pet_db.yml` serveur
sont d'accord.** Les deux tables sont indépendantes — le client décide seul d'afficher
l'entrée d'évolution (§7.1), le serveur revalide seul (`pet_db_ptr->evolution_data`).
Ici les deux portent bien Poring(9001) → Mastering(9069) avec **2 matériaux**
(`Leaf_Of_Yggdrasil` ×10 + `Unripe_Apple` ×3, pet_db.yml:77). Une divergence entre les
deux se traduirait par une entrée cliquable qui échoue en `FAIL_RECIPE`.

⚠ Le vecteur de +0xC8 porte des paires de **dwords** (16 o = 2 matériaux), alors que le
paquet transporte des `{index:2, amount:2}`. La source du `memcpy` est une variable
locale (`[ebp+Src]`), pas ce vecteur-là : une conversion a donc lieu entre les deux.
Non tracée — ne pas supposer que le contenu de +0xC8 part tel quel sur le réseau.

### 7.3 `ZC_PET_EVOLUTION_RESULT 0x09FC` → `sub_CD15C0` @0x00CD15C0

`[op:2][result:4]`, dispatché en case **2556**.

| `result` | client | `e_pet_evolution_result` (serveur) |
|---|---|---|
| 0 | msg **2571** en chat | `FAIL_UNKNOWN` |
| 1 | msg **2572** | `FAIL_NOTEXIST_CALLPET` |
| 2 | msg **2573** | `FAIL_NOT_PETEGG` |
| 3 | msg **2574** | `FAIL_RECIPE` |
| 4 | msg **2575** | `FAIL_MATERIAL` |
| 5 | msg **2576** | `FAIL_RG_FAMILIAR` |
| **6** | 🔴 **aucun `case` → rien du tout** | **`SUCCESS`** |
| **7** | `sub_D8A830(ctx)` = **réinitialise les globals pet** | *(n'existe pas)* |

🔴 **Décalage client/serveur, vérifié des deux côtés.** moonlight envoie bien
`SUCCESS` (= 6) en fin de `pet_evolution` (pet.cpp:2368) — et le client **l'ignore** :
aucun message de réussite n'est affiché. Sans conséquence fonctionnelle, parce que les
paquets émis juste avant (`clif_spawn`, `CHANGESTATEPET_INIT`, `clif_send_petstatus`
→ ZC 0x01A2) ont déjà tout rafraîchi. Symétriquement, le `case 7` du client — le seul
qui **efface** les globals du pet — est **inatteignable** sur moonlight.

⚠ Deuxième divergence : quand le pet porte un accessoire, moonlight répond
`FAIL_RG_FAMILIAR` (pet.cpp:2282) — **exactement comme pour une intimité insuffisante**
(pet.cpp:2277). Les deux cas partagent donc le message 2576, qui parle d'intimité : le
joueur dont le familier porte un accessoire lisait une raison fausse.

🔴 **Et le libellé dédié du client est INATTEIGNABLE.** `MSI_PET_EVOLUTION_FAIL_PET_ACC_OFF`
existe bien, mais son index est **2607**, hors de la plage 2571..2576 que mappe le
handler. Aucun code de résultat ne peut le déclencher — et le seul code restant, 7,
réinitialise les globals du pet (§7.3). *Méthode : les `MSI_*` sont référencés depuis une
table indexée par numéro de message ; en calant la base sur deux points connus du handler
(code 0 → 2571, code 5 → 2576), `FAIL_RG_FAMILIAR` retombe bien sur 2576 — recoupement
qui valide la base `0x0104F0A8` — et `FAIL_PET_ACC_OFF` sur 2607.*

✅ **Corrigé côté serveur le 2026-08-06** — sans toucher au protocole : le code
`FAIL_RG_FAMILIAR` est conservé (le client reste cohérent), et un **texte libre** dit la
vraie raison :

```c
if (sd->pd->pet.equip) {
    clif_pet_evolution_result(sd, e_pet_evolution_result::FAIL_RG_FAMILIAR);
    clif_displaymessage(sd->fd, msg_txt(sd, 1882));   // moonlight pet.cpp
    return;
}
```

Message **1882** ajouté à `conf/msg_conf/map_msg.conf` : *« Your pet must remove its
accessory before it can evolve. »* ⚠ Numéro pris **au-delà du dernier id upstream**
(1881), dans la plage des ajouts locaux `[Stingor]`, et non dans un trou du milieu
(1541+ est libre mais serait exposé à un conflit au prochain merge rAthena).

⚠ Rappel du §5.5 : avant même d'ouvrir la fenêtre 261, le client refuse l'évolution
si `intimité <= 900` (msg 2576). Le serveur refait le test à sa façon
(`intimate < PET_INTIMATE_LOYAL`, pet.cpp:2276).

### 7.4 🔴 Les deux boutons de la fenêtre 261 sont en coréen — et c'est indéboguable côté texte

Constaté en jeu : la fenêtre d'évolution affiche **확인** (Confirmer) et **취소**
(Annuler) alors que tout le reste du client est traduit.

La cause est dans `UIPetEvolutionWnd_OnCreate` @0x00970F70 : les deux boutons sont des
**`UIBitmapButton`**, pas des boutons texte. Leurs libellés sont **peints dans les
bitmaps** — aucun passage par `msgstringtable`, donc **aucune traduction possible par
les fichiers de langue** :

| bouton | id | bitmaps (3 états) |
|---|---|---|
| Confirmer | **184** | `유저인터페이스\`**`btn_big_ok.bmp`** (+ `_a` / `_b`) |
| Annuler | **185** | `유저인터페이스\`**`btn_big_cancel.bmp`** (+ `_a` / `_b`) |

**Ampleur exacte du défaut** (xrefs des chemins, binaire entier) :

- **`btn_big_cancel.bmp` : 1 seule référence** — cette fenêtre. C'est le seul endroit du
  client qui l'affiche, d'où sa rareté.
- **`btn_big_ok.bmp` : 2 références** — cette fenêtre, et `UIMacroBlackListCheckWnd`
  (vtable `0x0101FCE8`, OnCreate `0x007B3CC0`).

Les variantes **sans** `_big_` (`btn_ok.bmp` / `btn_cancel.bmp`), elles, sont bien
traduites — c'est pourquoi la fenêtre 90 (§6.4) affiche « OK » / « cancel » en anglais
avec des ids de bouton **identiques** (184 / 185).

**Correction** : remplacer les deux images (et leurs états `_a` / `_b`) dans le GRF.
⚠ Ne pas passer par un override `data\` : le dossier coréen `유저인터페이스` ne se
résout pas en locale fr-FR ([[reference_data_folder_cp949_encoding]]) — c'est le GRF
qu'il faut patcher.

**État des lieux (2026-08-06)** — client `E:\Nouveau dossier\Moonlight-Destiny\` :

| | |
|---|---|
| ordre de chargement (`DATA.INI`) | **1 = `moonlight.grf`**, 2 = `data.grf` |
| `moonlight.grf` | 25 Mo — le GRF **d'override** du serveur, donc **la bonne cible** |
| `data.grf` | 5,4 Go — client de base, à ne pas toucher |
| bitmaps hors GRF | **aucun** (`data\` ne contient que `luafiles514`) |
| outil disponible | `D:\Mes documents\GitHub\GRFEditor\GRFEditor.sln` |

`moonlight.grf` étant lu **en premier**, y ajouter les six fichiers suffit à masquer ceux
de `data.grf` — aucune modification destructive du client de base n'est nécessaire.
⚠ Opération de **déploiement** (le GRF est distribué aux joueurs) : à faire
délibérément, pas au fil d'un RE.

---

## 8. L'auto-feeding

### 8.1 🔴 Le bouton dépend d'une LISTE, pas d'une recette d'évolution

Le `UIToggleButton` d'auto-feeding (`UIPetInfoWnd+0xE8`, id **213**) n'est créé par
`OnCreate` que si le byte `+0xE4` vaut 1, lequel vient de
**`PetAutoFeeding_IsEggListed`** @0x00632610 appliqué à l'ITID de l'œuf du pet.

⚠ **Correction d'une erreur de ce document** (2026-08-06). Cette fonction s'appelait
`PetEvolution_HasRecipeForItem` dans l'IDB, et j'avais écrit que le bouton apparaissait
« si le pet a une recette d'évolution ». **C'est faux.** Elle parcourt le **vecteur plat**
`CPetEvolutionMgr+0x18 (begin) .. +0x1C (end)` — pas la map de recettes, qui est
à `+0x08`. Ce vecteur est `AutoFeedingPetList`, rempli par
`PetAutoFeeding_InsertEgg` @0x00632230, dont le message de doublon nomme la structure
sans ambiguïté : **« PetEggITID already exist in AutoFeedingPetList »**.

Le même `.lub` alimente donc **deux conteneurs distincts** du même manager :

| API Lua | conteneur | usage |
|---|---|---|
| `InsertRecipe(eggSrc, eggDst, item, qté)` | **map** `+0x08` | recettes d'évolution (§7.1) |
| **`InsertPetAutoFeeding(oeufITID)`** | **vecteur** `+0x18..+0x1C` | droit à l'auto-feeding |

`InsertPetAutoFeeding` prend **un seul** argument, exige `IsPetEggItem`, et rend
`(bool, message)` — `"good"`, `"argment 1st is must number"`,
`"PetEgg Item %d not found or not petegg"`, ou le doublon ci-dessus.

✅ **Confirmé en jeu** : un Poring n'avait pas le bouton ; après évolution en Mastering,
il est apparu — non parce que le Mastering aurait une recette, mais parce que
**`Mastering_Egg` figure dans `AutoFeedingPetList`** et pas `Poring_Egg`. La coïncidence
avec l'évolution est trompeuse : c'est l'œuf **porté** qui change, pas le statut de recette.

### 8.2 Le paquet — `CZ_CONFIG 0x02D8`

Le clic sur le toggle passe par `UIPetInfoWnd_OnMsg` msg 6 / id **213** (garde :
l'émetteur doit être le bouton de `+0xE8`), enregistré au replay, puis :

`CZ_Config_SendType2(0|1)` @0x00DA9030 construit un paquet **de 10 octets** :

| offset | valeur |
|---|---|
| +0 | `PacketType` = **728** = **`0x02D8`** (`CZ_CONFIG`) |
| +2 | `type` = **2** (dword) |
| +6 | `value` = 0 ou 1 (dword) |

Le `2` est `CONFIG_PET_AUTOFEED` de l'`enum e_config_type` serveur
(moonlight clif.hpp:748 — 0 = équipement, 1 = call, **2 = pet autofeed**,
3 = homoncule autofeed, 5 = costumes). Handler : `clif_parse_configuration`.

Le serveur répond par **`ZC 0x02D9`** `[op:2][type:4][enabled:4]`
(`clif_configuration`, clif.cpp:13061) — c'est lui qui fait autorité sur l'état affiché.
L'état local est mémorisé dans le global **`0x015FB99C`**, relu par `OnCreate` pour
initialiser le toggle.

⚠ Le nom `CZ_Config_SendType2` est trompeur : ce n'est pas « un paquet de type 2 »,
c'est `CZ_CONFIG` **figé** sur `type = 2`. Le client a d'autres émetteurs pour les
autres valeurs de l'enum.

### 8.3 Côté serveur — quand le repas part réellement

Dans le timer de faim (`pet_hungry`, moonlight pet.cpp:880) :

```c
if( battle_config.feature_pet_autofeed && pd->pet.autofeed
    && pd->pet.hungry <= battle_config.feature_pet_autofeed_rate ){
    pet_food( sd, pd );
}
```

Donc trois conditions cumulées : la fonctionnalité activée serveur, le drapeau du pet,
et un seuil de faim. Le client, lui, possède deux messages de refus dédiés —
`MSI_FAILED_PET_AUTO_FEEDING_CLOSE_RODEX` et
`MSI_FAILED_PET_AUTO_FEEDING_BY_ITEMACTION` — qui n'ont pas d'équivalent explicite
dans ce chemin serveur.

---

## 9. Le bavardage du pet

### 9.1 🔴 C'est le client du MAÎTRE qui écrit les répliques

Le serveur ne porte aucun texte. Le client du propriétaire décide, encode le tout dans
**un seul entier**, l'envoie en `CZ 0x01A9`, et le serveur le relaie brut à l'entourage
en `ZC_PET_ACT 0x01AA`. Chaque client décode ensuite avec **sa** copie de
`PetTalkTable.xml` (chargée par `LoadPetMonsterXmlTables` 0x00D8AF50, rangée à
`ctx+0x154C` ; racine `<monster_talk_table>` — à ne pas confondre avec
`MonsterTalkTable.xml`).

### 9.2 L'émission — deux `SendMsg(148)` d'affilée

Dans le bloc pet de `CActorSprite_UpdateMotionAndPosition` (§2.4), après le tirage à
25 % :

```c
faim  = Pet_GetHungerRank(g_Own_PetHungry);      // 0..4
intim = Pet_GetIntimacyRank(g_Own_PetIntimacy);  // 0..4
a = sub_D81F60(ctx, faim, intim, 4);   // → une ÉMOTICÔNE
b = sub_D83520(classe, 4, faim);       // → une RÉPLIQUE
SendMsg(148, b);   SendMsg(148, a);    // case 148 → CZ 0x01A9
```

⚠ Le `vt+0x18` de ces deux appels porte sur le **CGameMode**, pas sur l'acteur : c'est
bien `CMode::SendMsg`, dont le **case 148** @0x00C8FB8B émet
**`CZ_PET_ACT 0x01A9`** `[op:2][data:4]`.

### 9.3 L'encodage de `data` — quatre chiffres décimaux

`sub_D83520(classe, 4, faim)` compose :

```
data = 1000 × classe  +  100 × act  +  10 × faim  +  reste
```

et `sub_D81F60(ctx, faim, intim, 4)` rend `10 × emoteId + 2`.

Le décodage de `PetAct_OnPacket` (0x00CD13F0) lit donc :

| champ | extraction | rôle |
|---|---|---|
| mob | `data / 1000` | classe → nom du nœud XML (`Monster_GetResNameById`) |
| **act** | `(data / 100) % 10` | nœud de `PetTalkTable.xml` (§9.4) |
| faim | `(data / 10) % 10` | `hungry` / `bit_hungry` / `noting` / `full` / `so_full` |
| **reste** | `data % 10` | **2 = simple émoticône** (msg 160 à l'acteur) ; sinon réplique, et sert de drapeau de filtrage de mots (`1 - reste`) |

🔴 C'est bien pour ça que l'émoticône sort en `10 × id + 2` : le **2** final est le
discriminant. Les deux messages du §9.2 empruntent donc deux branches opposées du même
handler.

⚠ Ce **n'est pas** la formule documentée par rAthena (`(mob-100)*100 + act*10 + hungry`,
clif.cpp:17714). Sans conséquence : l'entier est fabriqué **et** relu par des clients,
le serveur ne fait que le recopier (`RFIFOL`).

Le `reste` vient de `dword_15FF8F8`, recopié dans `dword_15FB238` — un réglage global
(voisin de `g_ChatAutoSaveOn`), vraisemblablement le filtre de langage. Non tranché.

### 9.4 Les nœuds de `PetTalkTable.xml` — `PetTalk_ActIdToNodeName` @0x00D81EB0

| act | nœud | act | nœud |
|---|---|---|---|
| 0 | `feeding` | 6 | `levelup` |
| 1 | `hunting` | 7 | `perfor_1` |
| 2 | `danger` | 8 | `perfor_2` |
| 3 | `dead` | 9 | `perfor_3` |
| **4** | **`stand`** | 10 | `connect` |
| 5 | `perfor_s` | *autre* | `normal` |

Le bavardage spontané passe **`4 = stand`** (le pet est au repos à côté du maître) —
c'est le `a2 = 4` figé dans les deux appels du §9.2.

### 9.5 La sortie

`PetTalkTable_PickLine` (0x00D83160) choisit la ligne, `PetTalk_FormatChatLine`
(0x00D83560) compose `"<nom de l'entité> : <réplique>"`, puis :

* `Actor::OnMsg(7, texte)` → **bulle au-dessus de la tête** (toujours) ;
* `ChatAction(mgr, 1, texte, 0xFAFAFA, 0)` → **chatbox**, uniquement si
  `OptionInfo 0x72 == 0` **et** `OptionInfo 0x94 == 0` — deux interrupteurs qui coupent
  la ligne de chat sans jamais empêcher la bulle.

📌 Bourgeon détourne déjà ce chemin (`chat_window.cc`, `PetActStub`) pour **compter la
profondeur d'exécution** : rien dans les arguments de `ChatAction` ne distingue une
réplique de pet (type 0, expéditeur vide, couleur partagée) — seule la pile le dit.

---

## 10. L'API Lua du pet

Les données pet du client ne viennent pas du serveur mais des `.lub` de
**`Lua Files\DataInfo\PetInfo`**. Quatre fonctions Lua sont appelées depuis le C++ :

| fonction Lua | wrapper C++ | usage relevé |
|---|---|---|
| **`GetPetFood`** | `Pet_GetFoodItidByClass` 0x00D81FA0 | l'ITID de nourriture — c'est **elle** qui décide du gate « pas de nourriture en sac » du `SendMsg(150,1)` (§4.4) |
| **`GetPetString`** | `sub_D83470` 0x00D83470 | libellé d'un œuf ; utilisé par le menu d'évolution pour choisir entre le nom Lua et `Job_GetDisplayNameOrResName` |
| **`IsPetAccessory`** | `sub_D98320` 0x00D98320 | l'item est-il un accessoire de pet |
| **`GetPetRelationship`** | via `sub_67AB20` (table d'enregistrement Lua) | — |

⚠ Conséquence pratique : **la nourriture d'un pet est une donnée CLIENT**. Un
`FoodItem` changé dans `pet_db.yml` sans mise à jour du `.lub` fait diverger le gate
client du comportement serveur — le joueur se voit refuser « Feed Pet » alors que le
serveur aurait accepté (ou l'inverse).

Deux getters trivialement exposés complètent l'ensemble :
`0x00D34C90` → `g_Own_PetHungry`, `0x00D34CA0` → le champ `0x015FB3E8`
(tous deux `mov eax, global ; cdq ; retn`).

---

## 11. Table des adresses

| adresse | symbole |
|---|---|
| `0x01030900` | **vtable `UIPetInfoWnd`** (COL `0x010C2E60`, TD `0x0123F0BC`) |
| `0x00879220` | `UIPetInfoWnd_OnCreate` (vt+0x3C) |
| `0x00885F60` | **`UIPetInfoWnd_OnMsg`** (vt+0x94) |
| `0x00888F00` | `UIPetInfoWnd_RefreshIntimacyLabel` |
| `0x00880290` | vt+0x50 de `UIPetInfoWnd` (lit la classe du pet) |
| `0x0131F874` | **`g_PetInfoWnd_ptr`** (slot de la fenêtre 88) |
| `0x010939D4` | vtable `CNpc` (l'entité pet) ; `SubmitNameplateQuad` à **vt+0x14** |
| `0x00CBAAE0` | **`GameMode_OnRecv_ZC_CHANGESTATE_PET`** (0x01A4) |
| `0x00CA6657` | handler `ZC_PROPERTY_PET` (0x01A2), case 418 du dispatch |
| `0x00CD13F0` | `PetAct_OnPacket` (`ZC_PET_ACT` 0x01AA) |
| `0x00C8FC2A` | `CMode::SendMsg` **case 150** → CZ `0x01A1` |
| `0x00C8FAC5` | `CMode::SendMsg` case 145 → CZ `0x01A5` |
| `0x00C8FDF4` | `CMode::SendMsg` case 149 → CZ `0x019F` (✅ vivant, vérifié live §6.3) |
| `0x008B9930` | **`UIPetTamingDeceiveWnd_SendCatchRequest`** (vt+0x64) — l'unique émetteur de la capture |
| `0x00C8DA65` | `CMode::SendMsg` case 69, sous-cas pseudo-skill **10000** = capture |
| `0x00C8F474` | `CMode::SendMsg` case 146 → CZ `0x01A7` (choix de l'œuf) |
| `0x00D0CC60` | `ZC_START_CAPTURE` 0x019E : pose le mode de ciblage « capture » |
| `0x00D0D180` | `ZC_TRYCAPTURE_MONSTER` 0x01A0 : résultat → fenêtre 91 |
| `0x00CA6539` | `ZC_PETEGG_LIST` 0x01A6 : remplit la fenêtre 90 |
| `0x00C753A0` | `GameMode_PostActorClickAction` (garde de poids court-circuité si `+0x40C == 10000`) |
| `0x010344D4` | vtable `UIPetEggListWnd` (id 90) ; `OnMsg` `0x008C6AB0` |
| `0x01034D44` | vtable `UIPetTamingDeceiveWnd` (id 91) ; `OnMsg` `0x008C6CF0`, ctor `0x0088E990` |
| `0x0103F3EC` | vtable `UIPetEvolutionWnd` (id 261) |
| `0x0102BA18` | vtable `UIPetInfoBarGraph` (la jauge de la fiche) |
| `0x00C8FBE7` | envoi CZ `0x01A9` (`CZ_PET_ACT`) |
| `0x00C65A60` | `GameMode_PetIntimacyWarnAndOpenInfo` (ouverture automatique) |
| `0x00C478CE` | bloc pet de `CActorSprite_UpdateMotionAndPosition` (bavardage) |
| `0x00C58C3F` | `CActorSprite_SubmitNameplateQuad` : `+0x314 == 7 ⇒ quad[8] = 3` |
| `0x00C6ECDB` | condition d'entrée du menu contextuel pet |
| `0x00D82050` | `Pet_GetIntimacyRank` |
| `0x00D82190` | `Pet_GetHungerRank` |
| `0x00D81FA0` | `Pet_GetFoodItidByClass` |
| `0x00D8EBE0` | `IsPetEggItem` |
| `0x00631A70` | `CPetEvolutionMgr_InitFromFile` (`system\PetEvolutionCln_true.lub`) |
| `0x00632610` | **`PetAutoFeeding_IsEggListed`** (ex-`HasRecipeForItem` — nom erroné, §8.1) |
| `0x00632230` | `PetAutoFeeding_InsertEgg` (vecteur `AutoFeedingPetList` `+0x18..+0x1C`) |
| `0x00632CE0` | `InsertPetAutoFeeding(oeufITID)` — API Lua |
| `0x00DA9030` | `CZ_Config_SendType2` → **CZ `0x02D8`** `[type=2][value]` (auto-feeding) |
| `0x00631E10` | `PetEvolution_LookupRequirements` (œuf source + œuf cible → matériaux) |
| `0x00632AA0` | `PetEvolution_Lua_InsertEvolutionRecipe` — `InsertRecipe(eggSrc, eggDst, item, qté)` |
| `0x0097192C` | envoi **CZ `0x09FB`** `CZ_PET_EVOLUTION` (fenêtre 261) |
| `0x00CD15C0` | handler **ZC `0x09FC`** `ZC_PET_EVOLUTION_RESULT` (case 2556) |
| `0x00D8A830` | réinitialise les globals du pet (« le pet est parti ») |
| `0x00D823F0` | ITID d'œuf → classe de mob |
| `0x00C8FB8B` | `CMode::SendMsg` case 148 → **CZ `0x01A9`** `CZ_PET_ACT` |
| `0x00D83520` | `PetTalk_ComposeActCode` (1000·classe + 100·act + 10·faim + reste) |
| `0x00D81F60` | `PetTalk_PickEmotionCode` (→ `10·emote + 2`) |
| `0x00D81EB0` | `PetTalk_ActIdToNodeName` (0 feeding … 4 **stand** … 10 connect) |
| `0x00D8AF50` | `LoadPetMonsterXmlTables` (`PetTalkTable.xml` → `ctx+0x154C`) |
| `0x00D81FA0` | `Pet_GetFoodItidByClass` — wrapper Lua **`GetPetFood`** |
| `0x00D83470` | `Pet_Lua_GetPetString` · `0x00D98320` `Pet_Lua_IsPetAccessory` |
| `0x00C14920` | `CRagConnection_SendPacket` |
| `0x015FB3B0` | `g_Own_PetAid` (bloc des globals du pet, §1) |

---

## 12. Conséquences pour Bourgeon

1. **La fiche native est jetable, mais pas masquable.** `sub_C65A60` la **rouvre de
   force** dès que l'intimité passe sous 100. Toute conversion ImGui doit **détruire**
   la fenêtre 88 et router son ouverture, comme pour les autres natives
   ([[reference_native_window_toggle_router]]).
2. **Rejouer, ne pas réécrire** — comme pour le menu contextuel : `SendMsg(150, cmd)`
   fait tout le travail, y compris les gardes. 🔴 Mais `SendMsg` peut ouvrir une
   **modale bloquante** (la confirmation « nourrir ») : l'appel doit être différé hors
   de la frame ImGui ([[feedback_no_native_cmd_during_imgui_frame]]).
3. 🔴 **Trois gardes purement client à ne pas perdre** en remplaçant l'interface :
   - pas de nourriture en sac ⇒ **le paquet n'est pas envoyé** (msg 591) ;
   - la **rédaction de courrier** ouverte bloque « nourrir » (msg 2985) ;
   - l'évolution exige **intimité > 900** (msg 2576).
   Le serveur ne revalide pas forcément les deux premières.
4. **Le nom du pet** : 24 caractères max côté client (le champ d'édition en autorise 50 —
   c'est le msg 6/184 qui refuse), deux chaînes interdites, et le renommage est
   **définitif** (`rename_flag`).
5. **L'accessoire** est un ITID dans `0x015FB3D8` ; « Unequip Accessory » est proposé
   par le natif **même quand il n'y en a pas** (relevé live : accessoire = 0 et l'entrée
   était bien là). Une interface honnête grise l'entrée.
6. **Catégorie de pick 3 = pet** (§2.2) : toute logique Bourgeon qui range la catégorie 3
   dans « objet au sol » vise le pet sans le savoir.
