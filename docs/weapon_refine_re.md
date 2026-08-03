# Refine d'arme Whitesmith (« Upgradeable weapons ») — RE de `UIWeaponRefineWnd`

Client cible : `Moonlight-Destiny.exe` (base `0x400000`, build 20250716).
Serveur : fork `moonlight` de rAthena (`src/map/skill.cpp`, `src/map/clif.cpp`).

Objectif du document : décrire **exhaustivement** le chemin natif qui va du lancement du
skill `WS_WEAPONREFINE` jusqu'au refine effectif (ou l'arme détruite), afin de pouvoir remplacer la
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
> refine courant **et les 4 cartes** de l'arme. Le natif ne lit que `index` et `nameid` :
> la liste affiche un nom nu, sans `+7`, sans carte, sans slot. Tout est déjà là, gratuit.

> **2. Un lancement de skill = UN seul refine.** Le serveur fait
> `clif_menuskill_clear(sd)` juste après `skill_weaponrefine()`, et le client
> `SaveRectAndCloseWindow(111)` juste après l'envoi. Pour enchaîner, il faut **relancer le
> skill à la main** à chaque fois. C'est le principal irritant du natif.

> **3. Un échec DÉTRUIT l'arme.** `pc_delitem(&sd, idx, 1, 0, 2, LOG_TYPE_OTHER)` — le
> `item->refine = 0` qui précède est purement cosmétique. Le natif ne prévient de rien, ne
> demande aucune confirmation, et n'affiche même pas le refine courant de l'arme qu'on
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
| vt+0xB0 (sérialisation replay) | `0x009665D0` `UIWeaponRefineWnd_SerializeForReplay` |
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
> refine ni les cartes, qui ne sont nulle part ailleurs que dans `0x0221`.

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
    ItemSkillInfo_SetId(info, Value);               // info fabriqué du seul nameid
    listbox->AddString( ItemSkillInfo_ComposeDisplayName(info) );   // vt+0xD8

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
            //       (l'inverse exact de UIWeaponRefineWnd_SerializeForReplay, vt+0xB0)
default:    UIWindow_OnMsg_Default(...)               // 0x008841D0
```

Détails qui comptent :

- **Annuler envoie un paquet.** `CZ_REQ_WEAPONREFINE` avec `index = -1`. Côté serveur
  `server_index(-1)` sort de `[0, MAX_INVENTORY[`, `skill_weaponrefine` ne fait rien, mais
  `clif_menuskill_clear(sd)` s'exécute quand même : c'est **le désarmement propre** du
  `menuskill`. Un remplacement ImGui doit reproduire ce `182 / -1` à la fermeture, sinon le
  joueur reste avec un `menuskill_id` armé côté serveur.

  🔴 **Et le critère de « faut-il l'envoyer ? » ne doit RIEN devoir au client.** Il était
  `RefineWnd()` — la présence de la fenêtre native — ce qui marchait tant qu'on la laissait
  naître. Le passage de `0x0221` en remplacement d'opcode (§12.5 de `make_item_list_re.md`)
  l'a cassé net : plus de native, donc plus jamais de `-1`, donc un `menuskill` armé pour
  toujours. ⏱ Constaté en jeu — « j'ai fermé la fenêtre, maintenant le skill ne part plus ».

  Le critère est désormais **un seul drapeau**, `session_armed_`, qui ne répond qu'à la
  question « le SERVEUR attend-il encore une réponse ? » :
  - **posé** sur le compte du paquet (`count > 0`, exactement le critère de
    `clif_upgrade_list`) — jamais sur `entries_.empty()`, car un parseur qui échoue rend
    un vecteur vide sur un paquet peuplé et prétendrait donc que rien n'est armé ;
  - **retiré** dès qu'une tentative part (le serveur fait alors son propre
    `clif_menuskill_clear` : un seul refine par lancement) ou qu'une annulation est envoyée ;
  - **non touché par `ResetSession`**, qui referme notre interface — ce qui ne dit rien de
    l'état du serveur. Cette confusion-là a produit le troisième bug, ci-dessous.

  Trois régressions sont venues d'avoir répondu autrement à cette question :
  1. par la présence de la native (`RefineWnd()`) → plus de native, plus de `-1` ;
  2. par `entries_.empty()` → désynchronisable par un échec de parsing ;
  3. **par rien du tout à la bascule de l'interrupteur « Interface moderne »** : la session
     était jetée sans prévenir le serveur. ⏱ Constaté en jeu — « j'ai lancé refine,
     désactivé l'interface moderne, le skill ne part plus », et rebasculer n'aide pas
     puisque le blocage n'est pas côté client. Le gestionnaire de bascule (`OnTick`) poste
     donc l'annulation **avant** `ResetSession`. Même correctif appliqué à la fabrication,
     qui n'avait aucun gestionnaire de bascule du tout.

  Asymétrie à garder en tête : un `-1` **en trop** est inoffensif (`clif_parse_WeaponRefine`
  sort aussitôt si `menuskill_id` ne correspond pas), un `-1` **manquant** bloque le
  personnage. En cas de doute, on envoie. Récupération côté joueur si ça arrive :
  changement de carte, déconnexion ou mort — les trois seuls sites qui remettent
  `menuskill_id` à zéro (`unit.cpp` `unit_remove_map_`, `pc.cpp` `pc_dead`).
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
clif_menuskill_clear(sd);                          // ← UN SEUL refine par lancement
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

> ⚠ **Piège à la lecture côté plugin.** `RegisterObserveOpcode` passe les octets qui suivent
> l'opcode : le `data` du callback vaut **paquet + 2**. Les offsets ci-dessus sont ceux du
> PAQUET, il faut donc leur retrancher 2 — `result` en `data+0`, `itemId` en `data+4`. Écrit
> avec les offsets du paquet, `result` se lit à cheval sur les deux champs et vaut l'`itemId`
> décalé de 16 bits : jamais 0..3, donc le `switch` tombe dans son `default` et le journal de
> session reste vide **sans le moindre signe d'erreur**. C'est ce qui s'est produit, et le
> `0x0221` juste au-dessus ne le trahissait pas — son parseur, lui, part bien de `data+0` pour
> le champ longueur.

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
> fabrication ; le client le réutilise tel quel pour « aucune arme refinable ». D'où un
> message qui ne parle ni d'arme, ni de refine, ni de minerai — alors que le serveur, lui,
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

### Le refine lui-même

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
| `ItemSkillInfo_BuildDisplayName` / `ItemSkillInfo_SetId` | `0x008A0570` / `0x006A6570` |
| `ItemSkillInfo_ComposeDisplayName` (nom composé, SANS contexte) | `0x006A2CE0` |

### `ItemSkillInfo_ComposeDisplayName` — une RE-découverte, et sa leçon

`0x006A2CE0` n'est pas une trouvaille de cette campagne : elle était **déjà décrite par au
moins trois campagnes antérieures** — les liens `<ITEML>` du chat (`docs/chatbox_re.md`), le
tooltip de la fenêtre d'équipement, et la barre de raccourcis. Simplement, elle n'avait
jamais été **renommée** dans l'IDB : elle y ressortait en `FUN_006a2ce0` à chaque fois, donc
elle a été re-dérivée à chaque fois.

C'est le cas d'école de la règle « renommer ET commenter dès qu'on identifie » : le coût
d'un renommage est de dix secondes, celui de son absence se paie en re-RE à chaque campagne
qui recroise la fonction.

Ce qu'elle fait : `__thiscall(ItemSkillInfo* info, std::string* out, char decorate)`, où
`this` est **l'ItemSkillInfo lui-même**. Elle compose le nom complet (préfixe `+N`, grade,
affixes de cartes, nom de base, suffixe `[N]`) et n'a besoin d'**aucun contexte extérieur** —
ni fenêtre, ni gestionnaire. C'est elle que la fenêtre native emploie pour ses libellés de
liste (§3, `OnMsg` case `0x1F`) : si cette liste paraît nue, ce n'est donc pas la faute du
composeur, mais du fait que la fenêtre lui passe un `ItemSkillInfo` fabriqué à partir du seul
`nameid`.
| `UIWindowMgr_FindOrQueueNameRequest` (file de résolution du nom de créateur) | `0x00A2C8B0` |

### Le `this` de `BuildDisplayName` est le GESTIONNAIRE, pas une fenêtre

Trouvaille faite en corrigeant un bug de ce plugin, et **valable pour tout le projet** :
`ItemSkillInfo_BuildDisplayName` (`0x008A0570`) n'utilise son `this` qu'à un seul endroit —
`UIWindowMgr_FindOrQueueNameRequest(this)`, dans les branches « objet forgé dont le nom de créateur est
introuvable ». Or le seul appel **direct** de `UIWindowMgr_FindOrQueueNameRequest` dans le client,
`UIMerchantItemShopWnd_DrawContent` @`0x00948041`, charge explicitement :

```asm
00948040  push edi
00948041  mov  ecx, offset 0131F4E8h   ; g_UIWindowMgr
00948046  call UIWindowMgr_FindOrQueueNameRequest
```

`UIWindowMgr_FindOrQueueNameRequest` lit `this+0x18C` et y pousse une entrée : c'est une `std::list` du
**gestionnaire**. Vérifié en mémoire — `g_UIWindowMgr+0x18C` = `{0x03CDDFC8, 0}`, une liste
vide bien formée.

Deux conséquences :

- Passer `uiwnd::Mgr()` est correct **et toujours disponible**. Passer la fenêtre inventaire
  marche aussi… mais seulement quand elle est OUVERTE, sinon le nom se dégrade en nom de
  base — les préfixes de cartes disparaissent sous les yeux du joueur.
- ⚠ Passer une PETITE fenêtre est dangereux : `UIWeaponRefineWnd` fait `0xD0` octets, donc
  `+0x18C` est hors de ses bornes. Les six autres viewers du projet (inventaire, chariot,
  storage, échoppe, boutique PNJ, cash shop) passent chacun **leur propre fenêtre** : ça ne
  tient que tant que ces objets dépassent `0x190` octets et que la branche « forgé sans nom
  de créateur » reste rare. À reprendre, mais ce n'est pas l'objet de ce document.

---

## 10. Ce que le natif ne sait pas faire

Inventaire des manques, établi à partir du RE ci-dessus. C'était le cahier des charges du
remplacement ImGui : les onze points sont traités par le plugin du §11 — sauf le taux de
réussite, qui n'est pas accessible au client (voir la fin de cette section).

**Données déjà reçues mais jetées** (aucun paquet supplémentaire nécessaire) :

1. **Le refine courant** de chaque arme (`+6` de l'entrée). Le natif affiche
   « Twin Edge of Naght Sieger » qu'elle soit +0 ou +6.
2. **Les 4 cartes** (`+7`, 16 octets). Deux armes identiques dont une sertie sont
   indistinguables dans la liste native — un moyen très efficace de détruire la mauvaise.

**Manques ergonomiques** :

3. **Pas d'icône, pas de couleur, pas de slot** : une `UIListBox` de texte nu.
4. **Un refine = un lancement de skill.** Fermeture forcée après chaque OK, plus
   `clif_menuskill_clear` côté serveur. Enchaîner 5 refines = 5 lancements manuels.
5. **Aucun avertissement de destruction**, aucune confirmation, alors que l'échec détruit
   l'arme.
6. **Succès et échec portent le même texte** (911 = 912) et ne diffèrent que par la couleur.
7. **Aucun compte de minerai** : ni « il te reste N Oridecon », ni le minerai attendu par
   l'arme sélectionnée.
8. **Aucun plafond affiché** : le joueur ne voit pas que son skill niveau N borne le
   refine à +N.
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

## 11. Le remplacement ImGui livré

`src/features/windows/weapon_refine_window.{h,cc}` — membre du groupe « Interface moderne »
(`SetModernInterface`), section « Refine » du panneau Moonlight. Cette section remplace
les « points d'accroche à valider » d'origine : ce qui suit est ce qui a été FAIT, et ce que
l'implémentation a corrigé du plan initial.

### Ce qui s'est confirmé

- **Source des données** : `RegisterObserveOpcode(0x0221, 2)` en observation pure (le handler
  natif continue de tourner, sinon désactiver le plugin laisserait le skill sans fenêtre).
  ⚠ L'API ne transmet qu'un nombre FIXE d'octets alors que `0x0221` est à longueur variable :
  on n'en demande que 2 — le champ `packetLength` — et on lit le reste en se bornant à la
  longueur annoncée, qui est la vraie borne.
- **Masquage** : `+0x28` baissé dans le hook `MakeWindow` (`window_pos_tweaks`), sinon une
  frame native passe (la fenêtre naît entre deux `OnTick`).
- **Validation / fermeture** : `CMode::SendMsg(182, index)` puis `182, -1`, différés hors
  frame ImGui via `FlushPending` (`OnProcessInput`).
- **Modale « liste vide »** : escamotée par un détour ONE-SHOT sur `0x00A31A30`, doublement
  verrouillé (drapeau armé au seul `0x0221` vide **et** égalité du pointeur de texte 424),
  désarmé à chaque appel. Remplacée par l'énumération des vraies causes (§7).

### Ce que l'implémentation a corrigé du plan

1. **La fenêtre ImGui ne suit PAS la native.** Le plan disait « signal d'ouverture = présence
   de la fenêtre 111 ». Faux en pratique : le client la DÉTRUIT dès la tentative envoyée, donc
   la fenêtre disparaissait pile au moment où le résultat arrive et où l'on veut enchaîner —
   c'est-à-dire l'apport principal. Un `ui_open_` propre, ouvert par le paquet et fermé sur
   demande explicite, remplace ce calquage.
2. **Le `-1` de fermeture est CONDITIONNEL.** Après un refine le serveur a déjà fait son
   `clif_menuskill_clear` : on ne l'envoie que si la fenêtre native vit encore.
3. **« Relancer » n'est affiché que sans liste vivante.** Côte à côte, « Refine » et
   « Relancer » se lisaient comme deux façons de faire la même chose, alors que ce sont deux
   ÉTAPES successives — et relancer sur une liste vivante ne ferait que la redemander en
   payant le SP. Les rendre mutuellement exclusifs supprime l'ambiguïté à la racine.
4. **Le comptage de cartes est borné aux emplacements réels.** Les 4 entrées brutes du paquet
   incluent les ENCHANTEMENTS (posés depuis `card[3]` par les scripts serveur) : une arme 2
   slots portant 1 carte et 3 enchantements affichait « 4/2 ». `card[0]` valant 254/255/256
   (CREATE/FORGE/PET) signale en outre une forge, pas des cartes.
5. **Le contexte du name-builder est le GESTIONNAIRE** (cf. §9). Passer la fenêtre inventaire
   marchait… seulement quand elle était ouverte : les préfixes de cartes disparaissaient sous
   les yeux du joueur. Passer la fenêtre 111 aurait été pire — `0xD0` octets, `+0x18C` hors
   bornes.
6. **Les `__try` vivent dans des fonctions séparées.** C2712 se juge sur la fonction ENTIÈRE :
   `OnRenderUI` parcourt des `std::vector`, dont les itérateurs ont un destructeur non trivial
   dès que `_ITERATOR_DEBUG_LEVEL > 0`.
7. **Le nom décoré porte DÉJÀ son « +N », et la liste doit le retirer.** Conséquence directe
   du point 5 : une fois le contexte corrigé, `BuildDisplayName` compose enfin le vrai nom
   d'affichage — donc `« +2 Triple Explosive Twin Edge of Naght Sieger »`, préfixe de refine
   compris. Avec la colonne « +N » à sa gauche, le refine s'affichait deux fois. La liste
   saute donc le préfixe (`SkipRefinePrefix`), mais **uniquement** s'il correspond exactement au
   `refine` de l'entrée du paquet ; le nom mémorisé, lui, reste décoré, parce que l'aperçu au
   survol doit porter le MÊME titre que la fenêtre de description native. Le tri « Nom »
   s'aligne sur le nom affiché : trié sur le nom décoré, tout ce qui porte un refine remontait en
   tête (`+` précède toute lettre), ce qui doublait le tri « Refine » au lieu de trier par
   ordre alphabétique.
8. **Clic gauche = sélectionner, clic DROIT = consulter.** La description complète était
   d'abord câblée sur le clic gauche ; c'est le clic droit qui ouvre une description partout
   ailleurs dans le client (inventaire, chariot, storage, équipement), et l'ouvrir à gauche
   volait le geste de sélection. Le test s'écrivait en quatre orthographes dans le projet ;
   il est désormais dit une fois, par `mui::IsLastItemRightClicked()` (`ui/ro_widgets.h`), et
   les neuf sites strictement équivalents y ont été ramenés.
9. **Le tri est passé du combo à l'EN-TÊTE de table.** Un combo à trois entrées figées ne dit
   pas le sens du tri, et il faut le déplier pour savoir ce qu'il propose. Une
   `ImGuiTableFlags_Sortable` montre les colonnes triables en permanence et donne le
   croissant/décroissant au second clic. `ImGuiTableFlags_SortTristate` fait mieux encore :
   un troisième clic retire le tri, et l'ordre redevient celui du paquet — l'« ordre
   d'inventaire » que le combo devait porter comme une entrée à part n'a plus besoin
   d'exister. Deux détails non évidents :
   - le `Selectable` de la ligne est posé dans la colonne du NOM avec `SpanAllColumns` ;
     ImGui bascule alors son fond dans le canal d'arrière-plan de la table, ce qui fait
     passer le surlignage DERRIÈRE l'icône (colonne 0, soumise avant) au lieu de la couvrir.
     C'est ce qui permet de se passer complètement du dessin `ImDrawList` d'origine ;
   - le comparateur départage les égalités par `index` d'inventaire. `std::sort` n'étant pas
     stable, deux armes identiques échangeaient leur place d'une frame à l'autre et la ligne
     sautait sous le curseur ;
   - **un changement de tri ramène la sélection sur la première ligne**, défilement compris
     (`SpecsDirty`, remis à faux par l'appelant). C'est un geste EXPLICITE du joueur sur la
     table : il réordonne ce qu'il a sous les yeux, et la ligne désignée est visible.
     `SpecsDirty` couvre aussi le troisième clic, qui ne trie rien mais réordonne bel et bien
     l'affichage.

13. 🔴 **Aucun refine ne peut viser une ligne qu'on ne VOIT pas.** Le pire défaut trouvé sur
    cette fenêtre, et il ne se voyait pas : la sélection était cherchée dans `entries_`,
    c'est-à-dire dans TOUTE la liste du serveur, alors que le filtre n'en montre qu'une
    partie. Scénario réel — filtrer sur « kn » pour monter un Knife, refine, la liste revient,
    la sélection par défaut retombe sur la première entrée du SERVEUR (masquée par le
    filtre) : plus rien n'est surligné à l'écran, et Entrée joue quand même cette arme
    fantôme. Un échec la détruit sans que le joueur ait jamais vu ce qu'il visait.

    Deux règles en réponse, et **aucun re-ciblage automatique** :
    - `sel_visible_`, recalculé chaque frame sur les lignes RÉELLEMENT rendues, verrouille
      tout ce qui déclenche un refine (bouton « Refine », Entrée). Invisible = grisé et
      inerte, avec la raison écrite à l'écran ;
    - à l'arrivée d'une nouvelle liste, la sélection est reconduite **si et seulement si**
      son index y figure encore. Sinon elle est VIDÉE — pas de repli sur la première entrée.
      Déplacer silencieusement la cible d'une action destructrice est exactement ce qu'il ne
      faut pas faire ; c'est au joueur de re-désigner. Seule exception : la toute première
      liste d'une session, où il n'y avait aucune arme visée, donc rien à perdre.

    ⚠ **L'index est STABLE, la reconduction est donc légitime** — vérifié dans les sources
    serveur, pas supposé. `clif_item_refine_list` pose `index = client_index(i)` où `i` est
    la position INVENTAIRE (`clif.cpp:9404`), et `skill_weaponrefine` fait `item->refine++`
    **sur place** (`skill.cpp:11177`) : l'objet ne change pas de slot, son index revient à
    l'identique dans la liste suivante. Seul un échec le fait disparaître (`pc_delitem`).

    🔴 Le piège qui a fait échouer la reconduction est ailleurs, et il est instructif :
    `OnTick` remettait `sel_index_ = -1` en constatant la disparition de la fenêtre native.
    Or celle-ci meurt **dès la tentative envoyée**, donc bien avant l'arrivée du nouveau
    `0x0221` — qui ne trouvait alors plus aucune arme visée et retombait sur la première
    entrée. Le joueur qui montait la 4e arme se retrouvait pointé sur la 1re. Garder
    `sel_index_` ne réarme rien : sans liste, `sel_visible_` est faux.
10. **Entrée valide la sélection, et le jeu ne doit pas la voir.** Entrée ouvre la saisie de
    chat côté client : sans précaution, le raccourci refine l'aurait ouverte par-dessus. Le
    hook de `WndProc` l'avale, exactement comme il avale déjà Échap via
    `ro::AnyEscapeWindowOpen()`.

    ⚠ **Confisquer la touche et autoriser l'action sont deux questions distinctes**, et les
    confondre est un vrai piège. `WantsEnterKey()` ne regarde QUE l'ouverture de la fenêtre ;
    calqué au départ sur l'état du bouton, il laissait repasser la touche dans tous les creux
    du cycle — tentative en vol, liste consommée, relance en cours — si bien qu'enchaîner les
    refines à coups d'Entrée ouvrait et refermait le chat sans arrêt. C'est la règle d'Échap :
    une fenêtre RO ouverte s'approprie la touche, point. Contrepartie assumée : pas de chat à
    Entrée tant que la fenêtre est là.

    Le prédicat est **sans état de rendu** (`imgui_enabled_ && ui_open_` + les portes
    monde-de-jeu). Un drapeau posé à la frame précédente deviendrait faux dès que le rendu
    s'arrête sans que la fenêtre se ferme — un chargement de carte — et la touche resterait
    avalée pour un client qui n'affiche plus rien.

    L'ACTION, elle, garde ses verrous : sélection visible, aucune tentative en vol, aucune
    confirmation ouverte, et hors saisie — tant que le champ de filtre a le focus Entrée lui
    appartient (`IsAnyItemActive()`), elle le referme et l'action attend la frappe suivante.
    Sur un geste qui peut DÉTRUIRE l'arme, cette friction est voulue.

    🔴 **Et ce n'est pas seulement le chat qu'il faut protéger : c'est l'arme.** Un réglage
    opt-in a existé pour « rendre Entrée au chat » (défaut décoché). Il était **dangereux**,
    et la touche est désormais confisquée dans les deux réglages, **Espace comprise** :

    | Étape | Adresse | Ce qui se passe |
    |---|---|---|
    | `UIWindowMgr_OnKeyDown` | `0x00A471E0` | `if (key == 13 \|\| key == 32)` |
    | `UIWindowMgr_ActivateDefaultButton` | `0x00A2E270` | `OnMsg(msg = 0)` sur la fenêtre prioritaire |
    | `UIWindow_OnMsg_Default` | `0x008841D0` | `msg 0` → `OnMsg(6, this+0x8C)` |
    | notre §« champs » | — | la **111** a `+0x8C = 184` = le bouton **OK** |
    | `OnMsg` case 6 / 184 | `0x0096AAB0` | `SendMsg(182, liste_native[sél. native])` |

    Aucune de ces étapes ne consulte la visibilité — le prédicat `vt+8` interrogé par le
    gestionnaire est un `return 1` en dur (`0x005A5D90`). Notre fenêtre native masquée
    recevait donc la frappe et lançait un **refine réel sur l'arme sélectionnée par le
    CLIENT**, pas par le joueur. Le même trou a été constaté en jeu sur la fabrication (79),
    dont l'`OnMsg` a exactement la même structure (cf. `make_item_list_re.md` §12.5).

    La fabrication s'en sort en **détruisant** sa native ; ici on ne peut pas, elle porte la
    position d'ouverture et la présence de session. D'où la confiscation inconditionnelle.
    La sortie définitive est le **chat en ImGui** : Entrée pourra alors être bloquée
    globalement pour le jeu et distribuée côté ImGui.

11. **La relance de la compétence peut être automatique — le refine, non.** Demandé après
    coup, et ça touche à une règle de fond du projet (`project_plugin_architecture` : l'API
    Python a été retirée pour empêcher l'automatisation non supervisée). La ligne tenue est
    celle-ci : ce qui se relance seul, c'est le **lancement de la compétence**, la seule chose
    que le serveur oblige à refaire entre deux tentatives (`clif_menuskill_clear`). La liste
    revient ; le choix de l'arme et le déclenchement restent des clics. Aucune tentative ne
    part sans geste du joueur.

    Opt-in (`refine_auto_recast`, défaut OFF), et **trois conditions d'arrêt**, toutes tirées
    de l'état réel plutôt que d'une devinette :
    - une liste `0x0221` **vide** — c'est le vrai « il ne reste plus d'arme », dit par le
      serveur ;
    - `result` 2 ou 3 (niveau insuffisant / minerai manquant), ou un refus par `0x0110` : ce
      ne sont pas des tentatives mais des refus de condition, et cette condition ne changera
      pas d'elle-même — relancer tournerait en rond en brûlant du SP ;
    - plus aucun des trois minerais en inventaire.

    **Aucun plafond de relances**, et c'est délibéré. Un compteur avait été posé par réflexe
    défensif ; il ne protégeait de rien. Une relance ne peut suivre qu'un `0x0223` répondant
    à une tentative de nous, et cette tentative ne part que sur un geste du joueur : la
    chaîne avance au rythme d'un clic, d'un minerai et d'un cast par tour. Rien ne peut
    s'emballer, et le plafond n'aurait fait qu'interrompre une session légitime. La garde
    utile est ailleurs, dans la **causalité** : `ScheduleAutoRecast` n'est appelée que si
    `awaiting_result_` était vrai, donc jamais sur un `0x0223` qu'on n'a pas provoqué.

    L'état est affiché pendant qu'il a lieu (« Relance automatique… (n) ») et la raison de
    l'arrêt reste à l'écran : une action que le client prend de lui-même doit se voir.

11 bis. **Le refine automatique complet (`refine_auto_refine`, OFF).** Demandé ensuite, et il
    franchit délibérément la ligne du point 11 : sous cette option, c'est le tour précédent
    qui déclenche le suivant, la boucle se referme sur elle-même et **plus aucun clic
    n'intervient**. La consigne était « tant que le sort a des SP, il refine ».

    Ce qui change, et ce qu'il faut savoir en relisant le code :
    - l'option **implique** la relance de compétence (`AutoChain()` = `auto_recast_ ||
      auto_refine_`) : sans liste qui revient, il n'y aurait pas de second tour ;
    - la **confirmation est contournée** — le chemin auto pose `pending_ = kActRefine`
      directement, sans passer par `RequestRefine`. Une chaîne qui demande son accord à
      chaque tour n'en est pas une, et c'est écrit noir sur rouge sous la case à cocher ;
    - la borne annoncée est le **SP**. Le coût vient de la fiche de compétence
      (`CSkillInfo+0x14`, écrit par le paquet serveur — cf. `skill_tree_re.md` §9.1), jamais
      de `skill_db.yml` recopié ; le SP courant est le global de la barre de `UIBasicInfoWnd`
      (`0x015FF910`). Coût **inconnu (0) ⇒ on se tait et on laisse passer** : le chien de
      garde de la relance rattrapera. Le contrôle est refait **juste avant l'envoi**, pas
      seulement à l'armement — un tour dure près d'une seconde, le SP peut fondre entre les
      deux ;
    - **la cible ne peut être qu'une ligne AFFICHÉE.** `first_visible_index_` est la première
      ligne rendue (filtre **et** tri appliqués), et `list_drawn_` dit si la liste courante
      est passée à l'écran depuis son arrivée. Les deux sont remis à zéro à chaque `0x0221` :
      tant que `DrawList` n'a pas tourné, la chaîne **attend**. C'est le garde-fou central —
      aucune arme n'est détruite sans être passée sous les yeux du joueur. Liste non vide
      mais rien d'affiché (filtre qui exclut tout) ⇒ arrêt, en le disant ;
    - l'arme visée est `sel_visible_ ? sel_index_ : first_visible_index_`. Un **succès**
      reconduit la sélection (on continue de monter la même arme, jusqu'au plafond) ; un
      **échec** l'a détruite, `sel_index_` retombe à -1 et la chaîne passe à la première
      ligne suivante ;
    - un bouton **« Arrêter »** prend la place de « Relancer le skill » pendant toute la
      chaîne (la largeur de la fenêtre est calée sur trois boutons). Il pose `auto_paused_`,
      un drapeau de **session** : il ne décoche pas le réglage, et un clic manuel sur
      « Refine » ou « Relancer » le lève. Une tentative **déjà envoyée** va à son terme —
      le serveur a l'arme.

    ⚠ Le pavé au-dessus de `ScheduleAutoRecast` disait « un refine ne part que sur un geste
    du joueur » : ce n'est plus vrai, et le commentaire le dit maintenant explicitement. Ce
    ne sont plus les clics qui bornent la chaîne mais `AutoStopCause()` (minerai, SP) et les
    réponses du serveur. **Ces bornes sont donc les seules choses qui l'arrêtent** — les
    toucher, c'est toucher au frein.
12. **Les trois minerais sont des LIENS d'item.** Icône + nom + stock, soulignés au survol,
    curseur main, aperçu au survol et ouverture de la description au clic — le pendant ImGui
    d'un `<ITEM>` de chat. Deux choses à retenir :
    - le nom vient de la **DB client** (`itemInfoMerged.lua`, via `MoonlightUi::ItemName`),
      jamais d'une constante. « Phracon » était écrit en dur : ça aurait menti sur tout
      serveur qui renomme ses objets ou tourne dans une autre langue. Sans DB chargée on
      affiche l'id — faux jamais, muet parfois ;
    - la description s'ouvre **par ID** (`OpenDescById`), pas depuis un `ItemSkillInfo` : à
      stock 0 le minerai n'est pas en inventaire, il n'y a donc pas toujours d'instance
      vivante. C'est exactement la distinction que documente `features/item_cell.h`.

    Le curseur main passe par `ro::SetHoverCursor` : `ImGui::SetMouseCursor` est un no-op
    dans ce client, `io.ConfigFlags` portant `NoMouseCursorChange`.

14. 🔴 **Deux sorties serveur MUETTES, et le blocage qu'elles provoquaient.** Symptôme :
    en enchaînant les refines à coups d'Entrée, la fenêtre restait sur « Tentative
    envoyée — en attente du serveur… » et plus rien ne partait ensuite. Les deux causes sont
    dans les sources, pas dans le client :

    - `clif_parse_WeaponRefine` commence par `if (sd->menuskill_id != WS_WEAPONREFINE) return;`
      (`clif.cpp:15657`) et se **termine** par `clif_menuskill_clear` (`clif.cpp:15666`). Une
      seconde tentative pour la même liste est donc jetée **sans le moindre paquet de
      réponse** ;
    - plusieurs sorties de `skill_weaponrefine` répondent par `clif_skill_fail`, c'est-à-dire
      **ZC_ACK_TOUSESKILL 0x0110** et non 0x0223 : arme non affinable (`skill.cpp:11131`),
      entrée `refine.yml` absente, coût introuvable, échange en cours.

    Trois corrections, chacune indépendante :
    - **la liste est vidée à l'ENVOI**, plus en constatant après coup la disparition de la
      fenêtre native. Le serveur efface son `menuskill` à cet instant précis ; l'observation
      indirecte, elle, rate sa cible dès qu'un nouveau `0x0221` recrée la fenêtre avant le
      tick suivant — et la liste fantôme laissait envoyer dans le vide indéfiniment ;
    - **0x0110 est observé** (12 octets, `skillId` en `data+0`), filtré sur
      `WS_WEAPONREFINE` et sur une attente réellement en cours. Une tentative refusée se dit
      maintenant à l'écran au lieu d'attendre le délai de garde ;
    - **un seul point d'entrée**, `RequestRefine`, pour le bouton, le double-clic et Entrée.
      Chacun portait sa copie des conditions et **seul le bouton** avait la garde
      anti-rafale — d'où une touche maintenue qui expédiait deux tentatives pour une liste.

15. **Le journal nomme l'arme par son INDEX, pas par son `itemId`.** `ZC_ACK_WEAPONREFINE` ne
    porte qu'un id : le résoudre dans l'inventaire retombait sur le PREMIER objet de ce type,
    si bien qu'avec quatre Knife le journal citait toujours le même, jamais celui joué. On
    retient donc l'index envoyé et le nom composé **juste avant l'envoi** — le dernier
    instant où l'arme existe à coup sûr, un échec la détruisant. À la journalisation :
    l'index d'abord (état à jour, refine incrémenté), ce nom en repli si l'objet a disparu.

    ⚠ Et le `nameid` du paquet **ne désigne pas la même chose selon le résultat** : pour 0,
    1 et 2 c'est l'ARME (`clif_upgrademessage(&sd, r, item->nameid)`), mais pour 3 c'est le
    **MINERAI manquant** (`…, material[weapon_level - 1]`). Les confondre faisait dire
    « Knife is required to upgrade this weapon » au lieu de nommer l'Oridecon. Ce minerai
    n'étant par définition pas en inventaire, son nom vient de la DB client.

16. 🔴 **`TextWrapped` dans une fenêtre `AlwaysAutoResize` : la taille ne converge pas.**
    C'est LA cause du feuilleton de la modale de confirmation, et elle n'était pas dans le
    placement — quatre correctifs de position n'y ont rien changé, forcément.

    `BeginRoPopupModal` crée la fenêtre en `AlwaysAutoResize`. `TextWrapped` s'y replie sur la
    largeur de la région de contenu, laquelle dépend de la largeur de la fenêtre, laquelle
    dépend du contenu : la boucle ne converge pas et **la taille change à chaque frame**. Tout
    centrage appuyé sur cette taille fait alors dériver la fenêtre — symptôme observé en jeu :
    la modale apparaît en bas de l'écran puis **remonte jusqu'en haut en quelques frames**.

    Le correctif est une largeur de repli EXPLICITE (`PushTextWrapPos`), qui ferme la boucle.
    C'est ce que faisait déjà la seule autre modale de texte du projet
    (`features/systems/dx7_warning.cc`), et c'est la recommandation d'ImGui pour toute fenêtre
    auto-dimensionnée. Le nom de l'arme passe lui aussi en `TextWrapped` : en
    `TextUnformatted`, un nom décoré élargissait la modale à sa seule mesure.

    Une trace `LogDiag` (pos + taille, écrite seulement sur CHANGEMENT) reste en place : la
    taille doit se figer dès la 2ᵉ frame. Si elle continue de bouger, c'est qu'une boucle de
    repli traîne encore quelque part dans le contenu.

    Le placement, lui, se fait par `ImGui::SetWindowPos()` **depuis l'intérieur du popup**.
    `SetNextWindowPos` — et donc `ro::SetNextRoModalPos` — s'applique sous
    `ImGuiCond_Appearing`, et un popup a des chemins de placement **automatique** (recentrage
    modal, `FindBestWindowPosForPopup`) qui reprennent la main dès que la condition ne mord
    pas. Depuis l'intérieur, l'appel agit sur la fenêtre courante, immédiatement.

    ⚠ **La frame d'apparition ne connaît pas la taille**, et la trace l'a montré noir sur
    blanc : `(16,33)` sur celle-ci contre `(300,133)` sur la suivante, avec
    **`appearing` déjà faux** dès la seconde — il n'y a donc aucune « seconde chance » à
    saisir, contrairement à ce que je supposais. Centrer sur la taille de la frame
    d'apparition posait le coin haut-gauche pile au centre au lieu de la fenêtre elle-même.

    D'où le report : la taille stabilisée est mémorisée pour l'ouverture **suivante**.
    Corriger à la frame 2 serait pire — ImGui y a déjà émis le fond de la fenêtre, et le
    déplacer après coup le fait « baver » une frame (c'est écrit tel quel dans le code
    d'ImGui). La frame d'apparition, elle, n'est pas dessinée du tout : on peut y placer sans
    rien salir. Seule la toute première ouverture de la session est donc approximative.

    La modale ne retient **pas** sa position : elle est recentrée à chaque ouverture. Pour un
    dialogue de confirmation c'est suffisant, et c'est déterministe.

18. **La position de la fenêtre est persistée** (`refine_pos_x` / `refine_pos_y`). Elle ne
    l'était pas : le placement sur la fenêtre native s'appliquait en `ImGuiCond_Appearing`,
    donc **à chaque ouverture**, et la fenêtre retournait se poser sur la native à chaque
    lancement du skill. Le calage sur la native n'est désormais qu'un défaut de première
    utilisation.

    `INT_MIN` marque « jamais posée », comme `game_option_pos_*` — pas `-1` : une fenêtre
    tirée à cheval sur le bord gauche a une abscisse négative légitime. L'écriture du yaml a
    lieu à la FERMETURE (et à la sortie du monde de jeu), pas à chaque frame de glissement.

17. **La description ouverte depuis un lien passait DERRIÈRE la fenêtre.** Le panneau de
    `ItemDescWindow` porte `ImGuiWindowFlags_NoFocusOnAppearing` : il ne prend pas le focus
    en apparaissant, donc ImGui ne le remonte pas — ouvert depuis une fenêtre qui a le focus,
    il reste dessous. Ce flag est **voulu** (rien ne doit voler le focus à une desc qui
    réapparaît sans qu'on l'ait demandé), on ne le touche donc pas.

    Cette fenêtre appelait donc `itemdesc::FocusDescWindow()` après chaque ouverture. **Ce
    n'est plus à faire — et il ne faut PAS le refaire** : la remontée est centralisée dans
    `ItemDescWindow`, qui pose `RaiseItemWindow()` depuis le hook `OnMsg 0x18` de la fenêtre
    native (le point que traversent *toutes* les ouvertures, natives comme
    `itemcell::OpenDesc*`) et le consomme en `SetNextWindowFocus` à la frame suivante — donc
    après tout focus posé par qui que ce soit dans la frame du clic. Une demande de focus
    ajoutée ici en doublon ne ferait que courir contre celle-là.

    Tant qu'elle n'a jamais été rendue, elle s'ouvre **centrée sur la fenêtre de refine**, pas
    au milieu de l'écran : elle parle de la ligne qu'on vient d'y désigner, c'est là que le
    regard est. Ça demande une largeur estimée (`kConfirmW`), la modale étant en
    `AlwaysAutoResize` — sa vraie taille n'existe qu'après son premier rendu. Quelques pixels
    d'erreur, et seulement à la toute première ouverture.

    ⚠ Le « jamais placée » est un **drapeau à part**, pas un `-1` sentinelle dans la
    coordonnée : une modale tirée à cheval sur le bord gauche a une abscisse négative
    parfaitement légitime.

19. 🔴 **Le journal manquait exactement là où il servait.** Il était dessiné dans deux
    branches d'affichage sur trois, et l'oubliée était « aucune arme refinable » — c'est-à-dire
    la fin de partie, le moment même où l'on veut relire ce qui vient de se passer.

    Le cas le plus grave en découlait : un **échec sur la dernière arme**. La ligne
    « ÉCHEC — arme détruite » était bel et bien écrite dans le journal, mais la relance
    ramenait une liste vide, donc cette branche, donc aucun journal — le seul message qui
    comptait ne s'affichait jamais. Le journal est sorti des branches et dessiné après le
    pied dans tous les cas : ça supprime la classe de bug plutôt que d'ajouter un troisième
    appel qu'une quatrième branche oublierait à son tour.

### Ce qui n'est PAS fait, et pourquoi

- **Aucun taux de réussite affiché.** Il vit dans `db/pre-re/refine.yml` (`Rate`/10000 + bonus
  de job level) : hors de portée du client. Le coder en dur contredirait la règle « jamais de
  données codées en dur » et mentirait dès que le serveur ajuste sa table. Ça demande un
  opcode custom.
- ~~**Aucun refine automatique.**~~ **Fait**, et opt-in : `refine_auto_refine` (défaut OFF).
  Tant qu'il est décoché, la limite d'origine tient — chaque tentative demande un clic ou
  Entrée. Coché, la chaîne tourne seule ; cf. le point 11 bis.
- **Aucun compteur de SP affiché.** La borne existe (la chaîne s'arrête quand le SP manque)
  mais elle ne s'affiche pas : le client a déjà sa jauge, et « il reste N lancements » serait
  un chiffre de plus à lire pendant que des armes se jouent. Le motif d'arrêt, lui, s'affiche
  au moment où il tombe.

### Réglages persistants

`refine_imgui` (basculé en groupe), `refine_confirm`, `refine_show_cards`, `refine_filter`,
`refine_desc_tooltip`, `refine_history` (OFF), `refine_log_time`, `refine_auto_recast` (OFF),
`refine_auto_refine` (OFF).

⚠ `refine_auto_refine` n'entre **pas** dans le groupe « Interface moderne » : `SetModernInterface`
ne bascule que `refine_imgui`. Activer l'interface moderne n'allume donc jamais une option qui
joue des armes toute seule.

L'horodatage du journal est **calculé à l'insertion et toujours stocké** ; seul son affichage
suit le réglage. Ne le composer qu'à la demande priverait de leur heure toutes les lignes déjà
écrites au moment où on l'active.

Le tri, lui, n'est **pas** persisté : il vit dans les `ImGuiTableSortSpecs` de la table,
c'est-à-dire dans l'en-tête sur lequel le joueur vient de cliquer.
