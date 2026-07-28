# Raffinage d'arme Whitesmith (« Upgradeable weapons ») — RE de `UIWeaponRefineWnd`

Client cible : `Moonlight-Destiny.exe` (base `0x400000`, build 20250716).
Serveur : fork `moonlight` de rAthena (`src/map/skill.cpp`, `src/map/clif.cpp`).

Objectif du document : décrire **exhaustivement** le chemin natif qui va du lancement du
skill `WS_WEAPONREFINE` jusqu'à l'arme raffinée (ou détruite), afin de pouvoir remplacer la
fenêtre native par une fenêtre ImGui — et documenter au passage **la modale bloquante** qui
apparaît quand la liste revient vide.

Tout ce qui suit a été vérifié **en live** (x32dbg, client connecté, fenêtre ouverte sur
deux armes) ou recoupé avec les sources serveur. Les valeurs relevées à chaud sont
signalées par ⏱.

---

## 1. Vue d'ensemble du flux

```
lancement du skill WS_WEAPONREFINE (id 477, MaxLevel 10)
        │
        ▼
                    [ SERVEUR : clif_item_refine_list() — filtre l'inventaire ]
                    [ si count > 0 : arme menuskill_id = WS_WEAPONREFINE      ]
        ▼
ZC_NOTIFY_WEAPONITEMLIST (0x0221)   handler inline @ 0x00CA60C8
        │  count = (packetLength - 4) / 23
        │
        ├─ count == 0 ──► UIWndMgr_ShowMessageBoxModal(MsgString(424))   ◄── LA MODALE
        │                 « You can't create items yet. » — 280×120, BLOQUANTE
        │
        └─ count > 0  ──► si FindWindow(111) : SaveRectAndCloseWindow(111)
                          MakeWindow(111)                → UIWeaponRefineWnd
                          pour chaque entrée : OnMsg(0x1F, nameid, invIndex)
        ▼
    [ l'utilisateur choisit une arme dans la UIListBox et valide ]
        ▼
CMode::SendMsg(182, invIndex)  0x00C86740        bloc case 182 @ 0x00C8F516
        └─ envoie CZ_REQ_WEAPONREFINE (0x0222) { u16 op; u32 index }  len 6
        ▼
                    [ SERVEUR : skill_weaponrefine() + clif_menuskill_clear() ]
        ▼
ZC_ACK_WEAPONREFINE (0x0223)   Recv_ZC_ACK_WEAPONREFINE @ 0x00CDF6C0
        └─ un simple message de CHAT (pas de fenêtre), selon `result` 0..3
```

### Les trois points cruciaux pour le portage

> **1. Le client jette la moitié du paquet.** Chaque entrée de `0x0221` porte le niveau de
> raffinage courant **et les 4 cartes** de l'arme. Le natif ne lit que `index` et `nameid` :
> la liste affiche un nom nu, sans `+7`, sans carte, sans slot. Tout est déjà là, gratuit.

> **2. Un lancement de skill = UN seul raffinage.** Le serveur fait
> `clif_menuskill_clear(sd)` juste après `skill_weaponrefine()`, et le client
> `SaveRectAndCloseWindow(111)` juste après l'envoi. Pour enchaîner, il faut **relancer le
> skill à la main** à chaque fois. C'est le principal irritant du natif.

> **3. Un échec DÉTRUIT l'arme.** `pc_delitem(&sd, idx, 1, 0, 2, LOG_TYPE_OTHER)` — le
> `item->refine = 0` qui précède est purement cosmétique. Le natif ne prévient de rien, ne
> demande aucune confirmation, et n'affiche même pas le raffinage courant de l'arme qu'on
> s'apprête à jouer.

---

## 2. Le paquet `ZC_NOTIFY_WEAPONITEMLIST` (0x0221)

Longueur **variable** (`packetLength` aux octets 2-3). Table client : `VAR`, replay = 1.

Le handler (bloc inline du `switch` géant de `RecvLoop_DispatchPackets`) calcule le nombre
d'entrées par une division exacte par **23** — visible en clair dans le désassemblage :

```asm
00CA60C8  movsx  ecx, word ptr [15E819A]   ; packetLength
00CA60CF  mov    eax, 0B21642C9h           ; magique : division non signée par 23
00CA60D4  sub    ecx, 4                    ; moins l'en-tête [op:2][len:2]
00CA60D7  mul    ecx
00CA60D9  mov    edi, edx
00CA60DB  shr    edi, 4                    ; edi = count
```

### Layout d'une entrée — 23 octets

| Off | Taille | Champ    | Lu par le natif ? |
|-----|--------|----------|-------------------|
| +0  | `u16`  | `index`  | ✅ → vecteur B, renvoyé tel quel dans `0x0222` |
| +2  | `u32`  | `itemId` | ✅ → vecteur A, sert à construire le libellé |
| +6  | `u8`   | `refine` | ❌ **jeté** |
| +7  | 4×`u32`| `card[4]`| ❌ **jeté** |

`2 + 4 + 1 + 16 = 23` ✔. C'est la variante « moderne » de rAthena
(`PACKETVER >= 20181121` : `itemId` sur 32 bits **et** `EQUIPSLOTINFO` sur 4×`uint32`) ; la
variante historique fait 13 octets et n'a pas cours ici.

`index` est déjà décalé côté serveur (`client_index(i)` = `i + 2`) : **le client le renvoie
sans y toucher**, et le serveur refait `server_index()`. Aucun ±2 à gérer dans le portage.

### Boucle d'injection

```asm
00CA6153  mov    esi, 15E819Ch             ; début de la charge utile
00CA6158  movsx  eax, word ptr [esi]       ; index
00CA615B  ...    push [esi+2]              ; itemId (32 bits)
00CA616B  push   1Fh                       ; msg 0x1F
00CA616F  call   eax                       ; wnd->OnMsg(0, 0x1F, itemId, index, 0, 0)
00CA6177  lea    esi, [esi+17h]            ; +23
```

⏱ **Vérification live.** Fenêtre ouverte sur les deux armes du screenshot :

| # | vecteur A (`itemId`) | vecteur B (`index`) | Nom serveur |
|---|----------------------|---------------------|-------------|
| 0 | `13413` (`0x3465`)   | `15`                | Twin Edge of Naght Sieger (`db/pre-re/item_db_equip.yml`) |
| 1 | `700007` (`0x000AAE67`) | `16`             | Superbia String (`db/import/items/item_db_weapon.yml`, bow, WeaponLevel 4) |

`0x000AAE67 = 700007` confirme **définitivement** que `itemId` occupe 4 octets : sur 2
octets on lirait `44647`, qui n'existe dans aucune base de la branche.

---

## 3. La fenêtre `UIWeaponRefineWnd`

| Élément | Valeur |
|---|---|
| RTTI | `.?AVUIWeaponRefineWnd@@` @ `0x01240BCC` (TD `0x01240BC4`, COL `0x010C6A18`) |
| **vtable** | **`0x0103EE00`** |
| **windowID** | **`111` (`0x6F`)** |
| Taille objet | `0xD0` (208 o) — `operator new(0xD0)` dans `MakeWindow` |
| Constructeur | `0x009657D0` (via `UIWindow_composite_ctor` `0x0086B950`) |
| Destructeur | `0x00965E10` (vt+0x00) |
| `OnCreate` | `0x00968BE0` (vt+0x3C) |
| `OnDraw` / titre | `0x00969AE0` (vt+0x50) |
| **`OnMsg`** | **`0x0096AAB0`** (vt+0x94) |
| vt+0xB0 | `0x009665D0` |
| Pointeur singleton | `g_UIWindowMgr + 0x204` (posé par `MakeWindow`) |
| Taille fenêtre | **280 × 150** (`UIWindow_SetSize(0x118, 0x96)`) |

`FindWindow(111)` ne passe **pas** par le `switch` de `UIWindowMgr_FindWindow` : l'id tombe
dans le cas par défaut, donc dans la `std::map` id → fenêtre à `g_UIWindowMgr + 8`
(`0x0131F4F0`). ⏱ Instance vivante retrouvée à `0x22E13A90`.

### Titre

```c
// vt+0x50, 0x00969AE0
MsgStringTable_GetById(0x38E)      // 910 = MSI_REFINEITEMLIST = "Upgradeable weapons"
UIWindow_DrawTitleBar(this, 1, titre, 0);
```

### Layout mémoire (relevé ⏱ sur l'instance vivante)

Base `UIWindow` commune (identique à celle déjà documentée ailleurs) :

| Off | Champ | ⏱ valeur |
|-----|-------|----------|
| +0x00 | vtable | `0x0103EE00` |
| +0x10 | parent | `0` (fenêtre racine) |
| +0x14 | largeur | `280` |
| +0x18 | hauteur | `150` |
| +0x1C | x | `904` |
| +0x20 | y | `306` |
| +0x28 | **visible** | `1` — c'est le flag à baisser pour masquer la native |
| +0x2C | **windowID** | `111` |

Membres propres à la classe (posés à 0 par le ctor, remplis par `OnMsg(0x1F)`) :

| Off | Membre | Rôle | ⏱ valeur |
|-----|--------|------|----------|
| +0x8C | `default_id` | `184` (posé en fin de `OnCreate`) | `184` |
| +0xB4 | `listbox` | `UIListBox*` enfant | `0x0F0A5118` |
| +0xB8 | `vecA.begin` | `std::vector<int>` des **itemId** | `0x22ADBB68` |
| +0xBC | `vecA.end` | | `0x22ADBB70` (→ 2 entrées) |
| +0xC0 | `vecA.cap` | | `0x22ADBB70` |
| +0xC4 | `vecB.begin` | `std::vector<int>` des **index inventaire** | `0x22ADBBB8` |
| +0xC8 | `vecB.end` | | `0x22ADBBC0` (→ 2 entrées) |
| +0xCC | `vecB.cap` | | `0x22ADBBC0` |

> **Conséquence pratique** : les deux vecteurs sont lisibles directement depuis l'instance.
> Un plugin peut donc reconstruire la liste **sans hooker le paquet** — mais il n'aura ni le
> raffinage ni les cartes, qui ne sont nulle part ailleurs que dans `0x0221`.

### `OnCreate` (0x00968BE0)

Construit trois enfants, `a2` = largeur (280), `a3` = hauteur (150) :

| Enfant | Classe | Position / taille | id |
|---|---|---|---|
| Liste | `UIListBox` (`new 0xD4`, ctor `0x00835C60`) | `(12, 22)`, `a2-24 × a3-55` = **256 × 95** | `184` |
| OK | `UIBitmapButton` (`btn_ok`, `btn_ok_a`, `btn_ok_b`) | `(a2-91, a3-24)` = `(189, 126)` | `184` |
| Annuler | `UIBitmapButton` (`btn_cancel`, `btn_cancel_a`, `btn_cancel_b`) | `(a2-46, a3-24)` = `(234, 126)` | `185` |

La liste reçoit aussi ses trois couleurs à `240, 240, 240` (offsets +0x7C/+0x80/+0x84).

> ⚠ La liste **et** le bouton OK portent le même id `184`. Ce n'est pas une erreur de
> lecture : `OnMsg` case 6 ne reçoit que les événements de bouton, la liste ne s'y présente
> jamais. Mais c'est un piège si l'on écrit un dispatcher générique par id.

### `OnMsg` (0x0096AAB0) — le cœur

```c
case 0x1F:  // AJOUT D'UNE LIGNE — Value = itemId, param_2 = index inventaire
    vecA.push_back(Value);          // itemId
    vecB.push_back(param_2);        // index inventaire
    ItemSkillInfo_SetId(info, Value);
    listbox->AddString( BuildDisplayName(info) );   // vt+0xD8

case 0x06:  // CLIC BOUTON
    Replay_RecordUIEvent(this, id, ..., listbox->selIndex);
    if (id == 184) {                                  // OK
        if (UIListBox_GetItemCount(listbox)) {        // 0x0089F9B0
            sel = listbox->selIndex;                  // listbox + 0x94
            if (sel < 0) break;                       // ← rien : la fenêtre RESTE ouverte
            CMode::SendMsg(182, vecB[sel], 0, 0, 0);  // → CZ 0x0222
            SaveRectAndCloseWindow(111);
        } else {
            CMode::SendMsg(182, -1, 0, 0, 0);         // liste vide : on prévient le serveur
            SaveRectAndCloseWindow(111);              // (appelé deux fois — inoffensif)
        }
    } else if (id == 185) {                           // Annuler
        CMode::SendMsg(182, -1, 0, 0, 0);
        SaveRectAndCloseWindow(111);
    }

case 0x12:  // 18 — relayé tel quel à la listbox (défilement)
case 0x13:  // 19 — idem
case 0x7B:  // 123 — restitution replay : TLV tags 22200 / 22201 / 22202, rejoue des 0x1F
default:    UIWindow_OnMsg_Default(...)               // 0x008841D0
```

Détails qui comptent :

- **Annuler envoie un paquet.** `CZ_REQ_WEAPONREFINE` avec `index = -1`. Côté serveur
  `server_index(-1)` sort de `[0, MAX_INVENTORY[`, `skill_weaponrefine` ne fait rien, mais
  `clif_menuskill_clear(sd)` s'exécute quand même : c'est **le désarmement propre** du
  `menuskill`. Un remplacement ImGui doit reproduire ce `182 / -1` à la fermeture, sinon le
  joueur reste avec un `menuskill_id` armé côté serveur.
- **OK sans sélection ne fait rien.** `sel < 0` sort sans fermer ni envoyer.
- `UIListBox_GetItemCount` (`0x0089F9B0`) = `(list+0x8C − list+0x88) / 24`.
- ⚠ `UIWindowMgr_SaveRectAndCloseWindow` (`0x00A2E770`) **détruit** la fenêtre (elle
  enchaîne sur `UIWindowMgr_QueueDestroyWindow` `0x00A447D0`). Le nom est trompeur — c'est
  déjà noté dans l'IDB.

### La `UIListBox` enfant

| Élément | Valeur |
|---|---|
| vtable | `0x0102C1B8` (`??_7UIListBox@@6B@`) |
| ctor | `0x00835C60`, `new 0xD4` |
| `+0x88` / `+0x8C` | begin/end du `std::vector` d'items, **élément = 24 octets** |
| **`+0x94`** | **index sélectionné** (`-1` si aucun) — ⏱ `0` (1re ligne surlignée) |
| vt+0x94 (148) | `OnMsg` |
| vt+0xB4 (180) | `SetId` |
| vt+0xD4 (212) | `SetPositionSize(x, y, w, h)` |
| vt+0xD8 (216) | `AddString` |

⏱ Instance : `0x0F0A5118`, parent `0x22E13A90`, `(12, 22)` `256 × 95`, id `184`, visible.

---

## 4. L'envoi `CZ_REQ_WEAPONREFINE` (0x0222)

Bloc `case 182` de `CMode::SendMsg` (`0x00C86740`), à `0x00C8F516` :

```asm
00C8F516  mov  eax, 222h
00C8F51B  mov  [ebp-106B6h], edx        ; edx = index inventaire (ou -1)
00C8F521  mov  [ebp-106B8h], ax
00C8F528  call CRagConnection_GetInstance
...
00C8F53B  call PacketLen_Get             ; → 6
00C8F54A  call CRagConnection_SendPacket
```

```c
struct CZ_REQ_WEAPONREFINE {   // 6 octets, FIXE
    uint16 packetType;         // 0x0222
    uint32 index;              // valeur renvoyée telle quelle depuis 0x0221 (déjà i+2)
};
```

Côté serveur (`clif_parse_WeaponRefine`) :

```c
if (sd->menuskill_id != WS_WEAPONREFINE) return;   // hors contexte = no-op silencieux
if (pc_istrading(sd)) { clif_skill_fail(...); clif_menuskill_clear(sd); return; }
skill_weaponrefine(*sd, server_index(RFIFOL(fd, 2)));
clif_menuskill_clear(sd);                          // ← UN SEUL raffinage par lancement
```

> Envoyer `0x0222` en dehors du contexte est donc **sans effet** : aucun exploit possible
> depuis un remplacement client.

---

## 5. Le résultat `ZC_ACK_WEAPONREFINE` (0x0223)

Longueur **fixe 10**. Handler `0x00CDF6C0` (appelé depuis `0x00CA6184`).

```c
struct ZC_ACK_WEAPONREFINE {
    uint16 packetType;   // 0x0223
    uint32 result;       // +2
    uint32 itemId;       // +6
};
```

Le handler ne fait **qu'écrire une ligne de chat** — aucune fenêtre, aucune modale :

| `result` | MsgString | Texte (msgstringtable.csv) | Couleur passée à `ChatAction` |
|---|---|---|---|
| 0 | 911 `MSI_ITEM_REFINE_SUCCEESS` | `Refined weapon: %s` | `0x00FFFF00` |
| 1 | 912 `MSI_ITEM_REFINE_FAIL` | `Refined weapon: %s` | `0x00CDCD00` |
| 2 | 913 `MSI_ITEM_REFINE_FAIL_LEVEL` | `You cannot upgrade %s until you level up your Upgrade Weapon skill.` | `0x00C8C8FF` |
| 3 | 914 `MSI_ITEM_REFINE_FAIL_MATERIAL` | `%s is required to upgrade this weapon.` | `0x00C8C8FF` |

Deux remarques :

- **Les libellés 911 et 912 sont identiques** dans le `msgstringtable.csv` livré (`Refined
  weapon: %s`). Succès et échec sont donc, en jeu, **visuellement indiscernables** hors
  couleur — alors que l'échec vient de détruire l'arme. C'est un défaut du natif à corriger
  côté ImGui.
- Les « couleurs » sont des constantes numériques que le désassembleur a étiquetées comme
  des pointeurs (`asc_FFFF00`, `loc_CDCCFF+1`, `loc_C8C8FD+2`). Vérification faite, ces
  adresses tombent en plein `.text`/`.rdata` : ce ne sont **pas** des chaînes. Lues en
  `COLORREF` (`0x00BBGGRR`), `0x00FFFF00` donne cyan — ce qui correspond au succès documenté
  côté rAthena. Je n'ai pas confirmé l'ordre des composantes à l'écran pour les trois autres.
- `result` 2 et 3 sont émis par `skill_weaponrefine` **sans** que la liste soit rouverte :
  après un échec de condition, le joueur doit relancer le skill.

---

## 6. La modale « conditions non remplies »

C'est le chemin `count == 0` du handler `0x0221`, à `0x00CA60E6` :

```asm
00CA60E6  push edi          ; param_9 = 0
00CA60E7  push 78h          ; param_8 = hauteur 120
00CA60E9  push 118h         ; param_7 = largeur 280
00CA60EE  push offset 0FDA1A0h ; param_6 = titre, "메시지" (CP949) = « Message »
00CA60F3  push edi          ; param_5 = 0
00CA60F4  push edi          ; param_4 = 0
00CA60F5  push 1            ; param_3
00CA60F7  push edi          ; param_2 = 0
00CA60F8  push 1A8h         ; ← MsgString 424
00CA60FD  call MsgStringTable_GetById
00CA6105  mov  ecx, offset 131F4E8h    ; g_UIWindowMgr
00CA610B  call UIWndMgr_ShowMessageBoxModal
```

| Élément | Valeur |
|---|---|
| Fonction | `UIWndMgr_ShowMessageBoxModal` @ **`0x00A31A30`** |
| Classe créée | **`UIMessageBox`**, vtable `0x0102FEE0`, ctor `0x0086C210`, `new 0x120` |
| Texte | MsgString **424** = `MSI_CANT_MAKE_ITEM` = **« You can't create items yet. »** |
| Titre | `메시지` (« Message »), CP949 @ `0x00FDA1A0` |
| Taille | 280 × 120 |

> **Le libellé est un recyclage.** `MSI_CANT_MAKE_ITEM` est le message générique de
> fabrication ; le client le réutilise tel quel pour « aucune arme raffinable ». D'où un
> message qui ne parle ni d'arme, ni de raffinage, ni de minerai — alors que le serveur, lui,
> sait exactement pourquoi la liste est vide (cf. §7).

> 🔴 **Piège pour le portage.** `ShowMessageBoxModal` est **bloquante** : elle ne rend pas la
> main, elle **relance la boucle tick/rendu du mode courant** jusqu'à ce que l'utilisateur
> clique. La déclencher depuis `OnRenderUI` — donc entre `ImGui::NewFrame()` et
> `ImGui::Render()` — provoque une ré-entrance du rendu, c'est-à-dire un **freeze muet**.
> C'est exactement le piège déjà rencontré sur l'échoppe joueur (mémoire
> `feedback_no_native_cmd_during_imgui_frame`). Un remplacement ImGui doit soit intercepter
> ce cas **avant** que la modale ne parte, soit ne jamais la déclencher lui-même.

---

## 7. Les règles serveur (fork `moonlight`)

### Ce qui entre dans la liste — `clif_item_refine_list` (`src/map/clif.cpp:9380`)

Une entrée par objet d'inventaire vérifiant **toutes** ces conditions :

- `nameid > 0`
- `refine < skill_lv` — `skill_lv = pc_checkskill(sd, WS_WEAPONREFINE)`
- `type == IT_WEAPON`
- `identify` (identifié)
- `weapon_level >= 1`
- **le minerai correspondant est en inventaire** (`pc_search_inventory` par niveau d'arme)
- `!(equip & EQP_ARMS)` — **une arme portée n'apparaît jamais**

Si `count > 0` : `menuskill_id = WS_WEAPONREFINE`, `menuskill_val = skill_lv`.
Si `count == 0` : le paquet part quand même (vide) → **la modale du §6**.

### Minerai par niveau d'arme — `skill_weaponrefine` (`src/map/skill.cpp:11114`)

| `weapon_level` | Minerai | itemId |
|---|---|---|
| 1 | Phracon | 1010 |
| 2 | Emveretarcon | 1011 |
| 3 | Oridecon | 984 |
| 4 | Oridecon | 984 |

### Le raffinage lui-même

```c
if (ditem->flag.no_refine || ditem->weapon_level < 1)  → clif_skill_fail
if (item->refine >= sd.menuskill_val || item->refine >= 10) → clif_upgrademessage(2)  // « level up your skill »
if (pas de minerai)                                     → clif_upgrademessage(3)      // « %s is required »

per = (cost->chance / 100)                              // refine_db, Rate sur 10000
    + (class & JOBL_THIRD ? 10 : (job_level - 50) / 2);

pc_delitem(minerai, 1);                                 // le minerai part DANS TOUS LES CAS

if (per > rnd() % 100) {   // SUCCÈS
    item->refine++;  déséquipe si porté ;  clif_upgrademessage(0)
    clif_refine(ITEMREFINING_SUCCESS) ;  effet visuel ;  (fame à +10 si arme forgée)
} else {                   // ÉCHEC
    item->refine = 0 ;  déséquipe ;  clif_upgrademessage(1)
    clif_refine(ITEMREFINING_FAILURE)
    pc_delitem(arme, 1, 0, 2)                           // ◄── L'ARME EST DÉTRUITE
    effet visuel ;  clif_emotion(ET_HUK)
}
```

- Plafond atteignable **= le niveau du skill** (`refine >= menuskill_val` refuse), et jamais
  au-delà de `+10`.
- Le taux exact vit dans `db/pre-re/refine.yml` (`Rate` sur 10000) : **inaccessible au
  client**. Un affichage de pourcentage côté ImGui devrait soit être alimenté par un opcode
  custom, soit être clairement étiqueté « estimation » — pas codé en dur en silence.

### Skill

`WS_WEAPONREFINE` = **id 477**, `MaxLevel: 10` (`db/pre-re/skill_db.yml`).

---

## 8. Chaînes `MsgStringTable` utilisées

Accès : `MsgStringTable_GetById` @ `0x00A9ED30` (`__cdecl(unsigned id) -> const char*`,
CP949, ids valides `0..0x1102`). Source : `data/msgstringtable.csv` (base64 `clé,valeur`,
**id = numéro de ligne − 1**).

| id | Clé | Texte |
|---|---|---|
| 424 | `MSI_CANT_MAKE_ITEM` | `You can't create items yet.` |
| 910 | `MSI_REFINEITEMLIST` | `Upgradeable weapons` |
| 911 | `MSI_ITEM_REFINE_SUCCEESS` | `Refined weapon: %s` |
| 912 | `MSI_ITEM_REFINE_FAIL` | `Refined weapon: %s` |
| 913 | `MSI_ITEM_REFINE_FAIL_LEVEL` | `You cannot upgrade %s until you level up your Upgrade Weapon skill.` |
| 914 | `MSI_ITEM_REFINE_FAIL_MATERIAL` | `%s is required to upgrade this weapon.` |

---

## 9. Récapitulatif des adresses

| Symbole | Adresse |
|---|---|
| `UIWeaponRefineWnd` vtable | `0x0103EE00` |
| `UIWeaponRefineWnd` ctor | `0x009657D0` |
| `UIWeaponRefineWnd::OnCreate` | `0x00968BE0` |
| `UIWeaponRefineWnd::OnDraw` (titre) | `0x00969AE0` |
| `UIWeaponRefineWnd::OnMsg` | `0x0096AAB0` |
| `UIListBox` vtable | `0x0102C1B8` |
| `UIListBox` ctor | `0x00835C60` |
| `UIListBox_GetItemCount` | `0x0089F9B0` |
| Handler `ZC 0x0221` | `0x00CA60C8` |
| Handler `ZC 0x0223` | `0x00CDF6C0` (entrée `0x00CA6184`) |
| `CMode::SendMsg` (cmd 182) | `0x00C86740`, bloc `0x00C8F516` |
| `UIWndMgr_ShowMessageBoxModal` | `0x00A31A30` |
| `UIMessageBox` vtable / ctor | `0x0102FEE0` / `0x0086C210` |
| `MsgStringTable_GetById` | `0x00A9ED30` |
| `g_UIWindowMgr` | `0x0131F4E8` (map id→fenêtre à `+8`, singleton refine à `+0x204`) |
| `UIWindowMgr_MakeWindow` | `0x00A39340` (case 111 @ `0x00A3DB41`) |
| `UIWindowMgr_FindWindow` | `0x00A47B90` |
| `UIWindowMgr_SaveRectAndCloseWindow` ⚠ détruit | `0x00A2E770` |
| `BuildDisplayName` / `ItemSkillInfo_SetId` | `0x008A0570` / `0x006A6570` |

---

## 10. Ce que le natif ne sait pas faire

Inventaire des manques, établi à partir du RE ci-dessus — c'est le cahier des charges du
futur remplacement ImGui.

**Données déjà reçues mais jetées** (aucun paquet supplémentaire nécessaire) :

1. **Le raffinage courant** de chaque arme (`+6` de l'entrée). Le natif affiche
   « Twin Edge of Naght Sieger » qu'elle soit +0 ou +6.
2. **Les 4 cartes** (`+7`, 16 octets). Deux armes identiques dont une sertie sont
   indistinguables dans la liste native — un moyen très efficace de détruire la mauvaise.

**Manques ergonomiques** :

3. **Pas d'icône, pas de couleur, pas de slot** : une `UIListBox` de texte nu.
4. **Un raffinage = un lancement de skill.** Fermeture forcée après chaque OK, plus
   `clif_menuskill_clear` côté serveur. Enchaîner 5 raffinages = 5 lancements manuels.
5. **Aucun avertissement de destruction**, aucune confirmation, alors que l'échec détruit
   l'arme.
6. **Succès et échec portent le même texte** (911 = 912) et ne diffèrent que par la couleur.
7. **Aucun compte de minerai** : ni « il te reste N Oridecon », ni le minerai attendu par
   l'arme sélectionnée.
8. **Aucun plafond affiché** : le joueur ne voit pas que son skill niveau N borne le
   raffinage à +N.
9. **Pas de recherche, pas de tri, pas de filtre** — liste brute dans l'ordre d'inventaire.
10. **Le message « liste vide » est un recyclage hors sujet** (« You can't create items
    yet. ») livré dans une modale bloquante, au lieu d'expliquer la vraie cause (aucune arme
    identifiée / minerai manquant / tout est déjà au plafond du skill / arme portée).
11. **Aucun historique** de session (tentatives, réussites, minerai consommé).

**Ce qu'un remplacement ne peut PAS deviner sans aide du serveur** : le taux de réussite
(`refine.yml`, `Rate`/10000 + bonus de job level), le `weapon_level` (donc le minerai
attendu) et le `no_refine` — le client n'a que la base de **descriptions**. À traiter par un
opcode custom si l'on veut ces informations, plutôt que par une table codée en dur.

---

## 11. Points d'accroche pour le portage (à valider à l'implémentation)

- **Source des données** : hooker `0x0221` via `RegisterObserveOpcode(0x0221, …)` est
  **indispensable** — c'est le seul endroit où vivent `refine` et les cartes. Les deux
  vecteurs de la fenêtre native (+0xB8 / +0xC4) ne servent que de repli.
- **Signal d'ouverture/fermeture** : présence de la fenêtre 111 dans la map du gestionnaire,
  contrôlée par la vtable `0x0103EE00` (modèle `BankWindow::BankWnd()`).
- **Masquage** : baisser `+0x28` **à la création** (hook `MakeWindow`, modèle
  `BankWindow::HideNativeAtCreation`) pour éviter une frame native visible. Jamais de
  déplacement hors écran (mémoire `feedback_no_offscreen_hide`).
- **Validation** : `CMode::SendMsg(182, index)` — rejouer le chemin natif plutôt que
  fabriquer le paquet, comme partout ailleurs dans le projet.
- **Fermeture** : impérativement `CMode::SendMsg(182, -1)` pour désarmer le `menuskill`
  serveur, puis `OnMsg(6, 185)` sur la native.
- **Cas liste vide** : intercepter **avant** `ShowMessageBoxModal`, sinon freeze si l'on est
  dans une frame ImGui. Si l'interception s'avère impossible, différer toute action native
  via `OnProcessInput` (modèle `VendingWindow::FlushPending`).
- **Relance du skill** (pour enchaîner les raffinages) : `CMode::SendMsg` cmd `0x45`
  (id skill, GID, niveau) — cf. mémoire `reference_cmode_sendmsg_use_skill`, avec
  `WS_WEAPONREFINE = 477`. ⚠ À garder **déclenché par un clic** : le projet refuse
  délibérément l'automatisation non supervisée (mémoire `project_plugin_architecture`).
