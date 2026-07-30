# Fenêtres de fabrication (« LIST » / « Manufacturing List ») — RE complète

Client cible : `Moonlight-Destiny.exe` (base `0x400000`, build 20250716).
Serveur : fork `moonlight` de rAthena (`src/map/clif.cpp`, `src/map/skill.cpp`,
`src/map/skills/**`, `db/pre-re/produce_db.txt`).

Objectif : décrire **exhaustivement** tout ce qui va du lancement d'une compétence de
fabrication jusqu'à l'objet créé, afin de remplacer l'UI native par de l'ImGui.

Tout ce qui suit est vérifié soit dans l'IDB, soit **en live** (x32dbg, client connecté,
fenêtre ouverte par `SA_CREATECON`), soit recoupé avec les sources serveur. Les valeurs
relevées à chaud sont signalées par ⏱.

---

## 0. La conclusion qu'il faut lire en premier

> **Ce n'est pas UNE fenêtre, c'en est TROIS.** L'utilisateur voit un titre `LIST` et en
> déduit une fenêtre unique. En réalité le client a trois classes distinctes, avec trois
> ids, trois layouts et trois protocoles :
>
> | Fenêtre | id | Classe | Ouverte par | Aspect |
> |---|---|---|---|---|
> | « LIST » | **94** | `UIMakingArrowListWnd` | `ZC 0x01AD`, `ZC 0x025A` | grille d'icônes 200×200, **4 lignes visibles** |
> | « Manufacturing List » | **79** | `UIMakeTargetListWnd` | `ZC 0x018D` | `UIListBox` de texte nu 280×150 |
> | (choix des matériaux) | **80** | `UIMakeTargetProcessWnd` | ouverte par la 79 | 3 emplacements glisser-déposer 280×150 |
>
> **Pharmacy (`AM_PHARMACY`) ne passe PAS par la même fenêtre que Create Arrow et
> Elemental Converter.** Pharmacy ouvre la 79, les deux autres la 94. Un remplacement qui
> ne traite que la 94 laisse la moitié des métiers sur du natif.

> 🔴 **Le titre `LIST` est une chaîne ASCII CODÉE EN DUR**, pas une `MsgString`.
> `UIMakingArrowListWnd::OnDraw` (`0x008B4C30`) fait littéralement
> `std_string_assign(this + 212, "LIST", 4)`. Il n'est donc ni traduisible, ni contextuel :
> il dit `LIST` qu'on fabrique des flèches, des convertisseurs, du poison, des leurres, de
> la cuisine ou des bombes.

---

## 1. Vue d'ensemble des flux

```
                    ┌──────────────── ZC 0x018D (ZC_MAKABLEITEMLIST) ──────────────┐
                    │   entrées de 16 o : { u32 itemId ; u32 material[3] }         │
                    │   count == 0 → modale bloquante MsgString 424                │
                    ▼                                                              │
        MakeWindow(79) UIMakeTargetListWnd  ── OK ──┬── Rune Knight ───► CZ 0x018E ─┘
        « Manufacturing List »                      │
                                                    └── autres jobs ──► MakeWindow(80)
                                                          UIMakeTargetProcessWnd
                                                          (3 matériaux au drag&drop)
                                                                    └── OK ► CZ 0x018E

                    ┌──────────────── ZC 0x01AD (ZC_MAKINGARROW_LIST) ─────────────┐
                    │   entrées de 4 o : { u32 itemId }                            │
                    │   le client force mk_type = 2                                │
                    ▼                                                              │
        MakeWindow(94) UIMakingArrowListWnd ── OK ──────────────────► CZ 0x01AE ────┘
        « LIST »                            ▲
                    ┌───────────────────────┘
                    │   ZC 0x025A (ZC_MAKINGITEM_LIST)
                    │   en-tête + u16 mk_type, entrées de 4 o : { u32 itemId }
                    └── OK ──────────────────────────────────────────► CZ 0x025B

                    ┌──────────── résultat : ZC 0x018F (ZC_ACK_REQMAKINGITEM) ─────┐
                    │   { u16 result ; u32 itemId } → effet visuel UNIQUEMENT      │
                    └──────────────────────────────────────────────────────────────┘
```

### Les trois points cruciaux pour le portage

> **1. Le client ne sait RIEN de ce qu'il fabrique.** Aucun des trois paquets de liste ne
> porte la recette. `0x018D` a bien trois champs `material[3]`… que le serveur remplit de
> **zéros** (`clif.cpp:8500-8502`) et que le client **jette** de toute façon. Le joueur voit
> une liste de produits sans prix, sans matériaux, sans quantité réalisable. C'est le
> manque numéro un (§9).

> **2. Le résultat de la fabrication n'est JAMAIS écrit à l'écran par cette fenêtre.** Le
> handler de `ZC 0x018F` (`0x00CDECE0`) compose pourtant `« Successfully created %s. »`
> (MsgString 431) ou `« Failed to create %s. »` (430) — puis **détruit la chaîne sans
> l'afficher**. Vérifié au désassembleur : la fonction ne contient aucun appel de chat,
> aucun `sprintf`, le `%s` n'est même pas substitué. Seuls un effet de sprite et les
> messages émis séparément par le serveur informent le joueur (§8).

> **3. Des produits homonymes sont indiscernables.** ⏱ Cas réel relevé en jeu : les quatre
> convertisseurs élémentaires **12114 (Feu), 12115 (Eau), 12116 (Terre), 12117 (Vent)**
> portent tous `Name = "Elemental Converter"` dans `itemInfomoon.lua`. La liste native
> n'affiche que ce `Name` : un Sage qui a les matériaux des quatre voit **quatre lignes
> rigoureusement identiques**, séparées seulement par une icône de 24 px. L'élément est
> pourtant écrit noir sur blanc dans le champ `Desc` de la même DB, gratuitement
> disponible.

---

## 2. Qui ouvre quoi — table des points d'entrée

Établie en remontant **tous** les appelants côté serveur, pas seulement les trois skills du
cahier des charges.

### 2.1 Via `ZC 0x018D` → fenêtre **79** (`UIMakeTargetListWnd`)

`clif_skill_produce_mix_list(sd, skill_id, trigger)` — `clif.cpp:8472`

| Appelant | Skill | `trigger` (= `itemlv` de `produce_db`) |
|---|---|---|
| `skills/merchant/preparepotion.cpp:16` | **`AM_PHARMACY`** (Prepare Potion / Pharmacy) | 22 |
| `skills/thief/createnewpoison.cpp:17` | `GC_CREATENEWPOISON` → remappé en `GC_RESEARCHNEWPOISON` | 25 |
| `script.cpp:22910` | `RK_RUNEMASTERY` (`makerune`) | 24 |
| `script.cpp:11119` | commande de script `produce()` | paramètre |

> 🔴 **Le `produce()` de script n'est PAS lancé par une compétence, mais par un
> OBJET.** Quatre objets `Usable` le portent (`db/pre-re/item_db_usable.yml`) :
>
> | Item | Script | `trigger` |
> |---|---|---|
> | **612** Mini Furnace | `produce 21` | 21 (fonte de métaux) |
> | **613** Iron Hammer | `produce 1` | armes de niveau ≤ 1 |
> | **614** Golden Hammer | `produce 2` | ≤ 2 |
> | **615** Oridecon Hammer | `produce 3` | ≤ 3 |
>
> ⚠ **Et ces objets sont CONSOMMÉS à chaque usage** : `pc_useitem` termine par
> `pc_delitem(sd, n, 1, 1, 0, LOG_TYPE_CONSUME)` pour tout `Usable`
> (`src/map/pc.cpp:6639` et le bloc qui suit le script). Conséquence directe pour
> le portage : **aucune relance automatique n'est acceptable sur ce chemin** —
> elle brûlerait le stock du joueur en silence. Seules les listes ouvertes par une
> vraie compétence peuvent être relancées.
>
> Le client n'a aucun moyen de distinguer les deux dans le paquet. Le plugin s'en
> sort en observant les envois de `CMode::SendMsg` : sans lancement de compétence
> dans les 3 secondes qui précèdent la liste, elle vient d'un objet.

### 2.2 Via `ZC 0x01AD` → fenêtre **94** (`UIMakingArrowListWnd`), `mk_type` forcé à **2**

| Émetteur serveur | Skill | Contenu de la liste |
|---|---|---|
| `clif_arrow_create_list` (`clif.cpp:4333`) | **`AC_MAKINGARROW`** (Create Arrow) | entrées de `skill_arrow_db` présentes en inventaire, **non équipées et identifiées** |
| `clif_elementalconverter_list` (`clif.cpp:22110`) | **`SA_CREATECON`** (Create Elemental Converter, **id 1007** dans ce fork) | `produce_db` filtré sur `itemlv == 23` |
| `clif_magicdecoy_list` (`clif.cpp:22155`) | `NC_MAGICDECOY` | tout l'inventaire du groupe `IG_ELEMENT` |
| `clif_poison_list` (`clif.cpp:22184`) | `GC_POISONINGWEAPON` | tout l'inventaire du groupe `IG_POISON` |

> ⚠ Ces quatre skills partagent **le même paquet, la même fenêtre et le même paquet de
> réponse** (`CZ 0x01AE`). Le client ne sait pas lequel est en cours : c'est
> `sd->menuskill_id` côté serveur qui tranche (`clif_parse_SelectArrow`, `clif.cpp:15812`).
> Un remplacement ImGui qui veut afficher le bon titre doit donc **retenir la dernière
> compétence lancée** — l'information n'est nulle part dans le paquet.

### 2.3 Via `ZC 0x025A` → fenêtre **94**, `mk_type` transmis par le serveur

`clif_cooking_list(sd, trigger, skill_id, qty, list_type)` — `clif.cpp:8527`

| Appelant | Skill | `list_type` | `trigger` |
|---|---|---|---|
| `script.cpp:11134` (`cooking()`) | `AM_PHARMACY` | 1 | paramètre |
| `skills/merchant/createbomb.cpp:21` | `GN_MAKEBOMB` | 5 | 28 |
| `skills/merchant/mixcooking.cpp:21` | `GN_MIX_COOKING` | **6** | 27 |
| `skills/merchant/specialpharmacy.cpp:19` | `GN_S_PHARMACY` | 6 | 29 |
| `skills/merchant/manufacturemachine.cpp:19` | `MT_M_MACHINE` | 7 | 31 |
| `skills/merchant/bionicpharmacy.cpp:19` | `BO_BIONIC_PHARMACY` | 8 | 32 |

Valeurs documentées de `mk_type` (commentaire de `clif_cooking_list`) :
`1` cuisine, `2` flèches, `3` élémentaire, `4` `GN_MIX_COOKING`, `5` `GN_MAKEBOMB`,
`6` `GN_S_PHARMACY`, `7` `MT_M_MACHINE`, `8` `BO_BIONIC_PHARMACY`.

> ⚠ **Le fork ne suit pas son propre commentaire** : `mixcooking.cpp` envoie `6` là où la
> table annonce `4`. Ce n'est pas anodin — `clif_parse_Cooking` (`clif.cpp:15606`) teste
> `p->type == 6` pour n'accepter que `GN_MIX_COOKING` **ou** `GN_S_PHARMACY`. Le `6` est
> donc cohérent avec le parseur, et c'est le **commentaire** qui est périmé. À ne pas
> « corriger » à l'aveugle.
>
> Les valeurs `3` (élémentaire) et `2` (flèches) ne sont **jamais émises** par ce serveur :
> ces deux cas passent par `0x01AD`, où le client force `mk_type = 2` lui-même.

---

## 3. Les paquets

### 3.1 `ZC_MAKABLEITEMLIST` (0x018D) — longueur variable

Handler inline @ **`0x00CA5EC5`** (`case 397` du `switch` de `RecvLoop_DispatchPackets`).

```asm
00CA5EC5  movsx edi, word ptr [15E819A]  ; packetLength
00CA5ECC  sub   edi, 4                   ; moins [op:2][len:2]
00CA5ECF  shr   edi, 4                   ; edi = count  (÷ 16)
00CA5ED2  test  edi, edi
00CA5ED4  jnz   loc_CA5F0F               ; liste non vide
```

| Off | Taille | Champ | Lu par le natif ? |
|-----|--------|-------|-------------------|
| +0 | `u32` | `itemId` | ✅ (`push dword ptr [esi]`) |
| +4 | 3×`u32` | `material[3]` | ❌ **jeté** — et de toute façon mis à **0** par le serveur |

`4 + 12 = 16` ✔.

**Chemin liste vide** (`0x00CA5EDA`) : `UIWndMgr_ShowMessageBoxModal(MsgString(424), 280×120)`
— la même modale bloquante que le refine d'arme, avec le même libellé recyclé
« You can't create items yet. ».

> ⏱ **Relevé live sur la FORGE Blacksmith** (un hammer utilisé sans les métaux requis).
> C'est la troisième compétence à emprunter ce chemin après Pharmacy et le refine, et elle
> valide la ligne `script.cpp:11119` du §2.1, qui n'était jusque-là qu'une lecture de code.
>
> Instance `UIMessageBox` à `0x233401F0` :
>
> | Off | Champ | ⏱ valeur |
> |---|---|---|
> | +0x00 | vtable | `0x0102FEE0` ✔ (`UIMessageBox`) |
> | +0x14 / +0x18 | largeur / hauteur | `280` / `120` ✔ |
> | +0x1C / +0x20 | x / y | `745` / `435` |
> | +0x28 | visible | `1` |
> | +0x2C | **windowID** | **`599`** (`0x257`) |
>
> ⚠ Et **aucune fenêtre 79 n'est créée** : le handler part dans la branche modale *avant*
> `MakeWindow`. `g_UIWindowMgr + 0x374` reste nul.

### 3.1 bis — détecter une modale native sans rien hooker

Trouvaille faite en cherchant à lire la modale ci-dessus, et **utile à tout le projet** :
`UIWndMgr_ShowMessageBoxModal` empile ses modales dans un `std::deque` du gestionnaire.

| Adresse | Champ |
|---|---|
| `g_UIWindowMgr + 0x1A8` | tableau de blocs (`map`) |
| `g_UIWindowMgr + 0x1AC` | nombre de blocs |
| `g_UIWindowMgr + 0x1B0` | décalage de tête |
| **`g_UIWindowMgr + 0x1B4`** | **nombre de modales empilées** |

Le n-ième élément se lit `map[((off + n) >> 2) & (blocs − 1)][(off + n) & 3]`.

> **`g_UIWindowMgr + 0x1B4 != 0` suffit donc à savoir qu'une modale native bloquante est à
> l'écran** — sans détour, sans hook, en une lecture. Bien plus simple qu'un détour sur
> `0x00A31A30`, et plusieurs plugins peuvent coexister là où deux détours sur la même
> adresse ne se composent pas.

#### Escamoter une modale native (mécanisme RETIRÉ, conservé ici pour mémoire)

Un module `ui/native_modal.{h,cc}` a existé pour supprimer la modale « liste vide ». Il a
été **supprimé** avec le passage au remplacement d'opcode (§12.5) : cette modale était
affichée par le handler NATIF, qui ne tourne plus. Le mécanisme est noté ici au cas où un
autre chemin en aurait besoin — il ne se redécouvre pas facilement :

- détour `kJmpHook` sur `UIWndMgr_ShowMessageBoxModal` (`0x00A31A30`), armé pour UN appel ;
- **renvoyer `185`** quand on escamote : c'est ce que le natif rend lui-même quand il
  n'affiche rien (mode inhibé, ou modale déjà à l'écran), donc la valeur que ses ~250
  appelants savent déjà encaisser ;
- deux verrous cumulés, indispensables avec autant d'appelants : armer juste avant l'appel
  natif attendu, **et** comparer le POINTEUR de texte à celui rendu par
  `MsgStringTable_GetById` (stable par id, ce qui évite un `strcmp` sur du CP949) ;
- désarmement **inconditionnel** à chaque appel de la fonction native, escamoté ou non.

⚠ Le piège qui a motivé le retrait : sans appel natif pour le consommer, le drapeau reste
armé et guette la modale suivante portant le même texte. Un armement qui ne peut plus être
consommé est un piège en attente.

**Chemin liste non vide** (`0x00CA5F0F`) :
`SaveRectAndCloseWindow(79)` ⚠ *détruit* la fenêtre existante, puis `MakeWindow(79)`, puis
une boucle `OnMsg(0, 0x1F, itemId, 0, 0, 0)` avec `esi += 0x10`.

> Noter l'asymétrie : la 79 est **détruite puis recréée** à chaque liste, alors que la 94
> est seulement **réutilisée** (`MakeWindow` renvoie l'instance existante) après un
> `OnMsg(0x4B)` de purge.

### 3.2 `ZC_MAKINGARROW_LIST` (0x01AD) — longueur variable

Handler inline @ **`0x00CA68AB`** (`case 429`).

```asm
00CA68AB  movzx edi, word ptr [15E819A]  ; packetLength
00CA68B7  sub   edi, 4
00CA68BC  shr   edi, 2                   ; edi = count  (÷ 4)
00CA68BF  call  UIWindowMgr_MakeWindow   ; param_1 = 5Eh = 94
00CA68D2  push  4Bh                      ; OnMsg(0x4B) → PURGE de la liste
...
00CA68F1  loop: push esi                 ; ← POINTEUR sur l'entrée, pas la valeur !
00CA6900        push 1Fh                 ; OnMsg(0x1F, &entry)
00CA690C        add  esi, 4
...
00CA692A  push 22h ; push 2              ; OnMsg(0x22, 2) → mk_type = 2, CODÉ EN DUR
```

Entrée = **4 octets** : `{ u32 itemId }`.

> 🔴 **Piège de lecture** : contrairement à `0x018D` et à `ZC_NOTIFY_WEAPONITEMLIST`, le
> `0x01AD` passe à `OnMsg(0x1F)` **l'adresse** de l'entrée, pas sa valeur. Le `OnMsg`
> déréférence (`ItemSkillInfo_SetId(info, *(u32*)a4)`). Une réimplémentation qui appellerait
> `OnMsg(0x1F, itemId)` poserait un id d'objet comme pointeur.

Aucune modale si la liste est vide : la fenêtre s'ouvre simplement **vide**.

### 3.3 `ZC_MAKINGITEM_LIST` (0x025A) — longueur variable

Handler inline @ **`0x00CA6937`** (`case 602`).

```asm
00CA6943  sub  edi, 6                    ; [op:2][len:2][mk_type:2]
00CA6948  shr  edi, 2                    ; count (÷ 4)
00CA694B  call UIWindowMgr_MakeWindow    ; 94
00CA6972  push 4Bh                       ; OnMsg(0x4B) purge
00CA6978  movsx eax, word [15E819C]      ; mk_type
00CA6990  push 22h                       ; OnMsg(0x22, mk_type)
00CA69B0  loop: push esi ; push 1Fh      ; OnMsg(0x1F, &entry), esi += 4
```

En-tête `{ u16 op ; u16 len ; u16 mk_type }`, puis entrées de **4 octets** `{ u32 itemId }`.

> L'ordre diffère de `0x01AD` : ici `mk_type` est posé **avant** le remplissage, là il l'est
> **après**. Sans conséquence — `OnMsg(0x22)` ne fait qu'écrire un champ.

### 3.4 `CZ_REQMAKINGITEM` (0x018E) — 18 octets, FIXE

Bloc `case 130` de `CMode::SendMsg`, @ **`0x00C8F6A3`**.

```c
struct CZ_REQMAKINGITEM {   // 18 octets
    uint16 packetType;      // 0x018E
    uint32 itemId;          // +2
    uint32 material[3];     // +6, +10, +14
};
```

`itemId == 0` ⇒ les trois matériaux sont forcés à zéro (c'est **l'annulation**).
Sinon les trois matériaux valent `atoi()` du champ `+0x2C` de chacune des trois
`ItemSkillInfo` passées (pas 0xF8 : `+0x2C`, `+0x124`, `+0x21C`).

### 3.5 `CZ_REQ_MAKINGARROW` (0x01AE) — 6 octets, FIXE

Bloc `case 153` @ **`0x00C8FE9A`**.

```c
struct CZ_REQ_MAKINGARROW { uint16 packetType /* 0x01AE */; uint32 itemId; };
```

Annulation = `itemId = -1` (`0xFFFFFFFF`).

### 3.6 `CZ_REQ_MAKINGITEM` (0x025B) — 8 octets, FIXE

Bloc `case 207` @ **`0x00C8FEDA`**.

```c
struct CZ_REQ_MAKINGITEM {  // 8 octets
    uint16 packetType;      // 0x025B      [ebp-10658]
    uint16 type;            // +2 mk_type  [ebp-10656]
    uint32 itemId;          // +4          [ebp-10654]
};
```

> ⚠ **`type` précède `itemId`** — l'ordre inverse de l'intuition, et l'inverse de la
> signature `SendMsg(207, itemId, mk_type)` côté client. Relevé directement dans les `mov`.

### 3.7 `ZC_ACK_REQMAKINGITEM` (0x018F) — 8 octets, FIXE

Handler **`0x00CDECE0`**, appelé depuis `0x00CA61FC` (`case 399`).

```c
struct ZC_ACK_REQMAKINGITEM {
    uint16 packetType;   // 0x018F
    uint16 result;       // +2
    uint32 itemId;       // +4
};
```

| `result` | Effet retiré puis rejoué | Effet joué | MsgString **composée puis jetée** |
|---|---|---|---|
| 0 | — | `0x9A` (154) | 431 `MSI_MAKE_TARGET_SUCCEESS_MSG` |
| 1 | — | `0x9B` (155) | 430 `MSI_MAKE_TARGET_FAIL_MSG` |
| 2 | 305 | `0x131` (305) | 431 (succès) |
| 3 | 306 | `0x132` (306) | 430 (échec) |
| 4 | 1015 | `0x3F7` (1015) | 431 (succès) |
| 5 | 1016 | `0x3F8` (1016) | 430 (échec) |
| 6 | 1017 | `0x3F9` (1017) | **430 (échec)** ⚠ |
| 7 | 1018 | `0x3FA` (1018) | 430 (échec) |

> ⚠ **Le cas 6 casse l'alternance pair = succès / impair = échec.** Vérifié au
> désassembleur, pas seulement au décompilateur : `0x00CDF3D9 push 1AEh` (= 430, échec) là
> où le motif appelle 431. Côté serveur, `result = 6` est bien un **succès**
> (`skill.cpp:13910` et `13942`, tous deux suivis de `NOTIFYEFFECT_PHARMACY_SUCCESS` et de
> `MSI_SKILL_SUCCESS`). C'est donc un **bug du client** — sans conséquence visible
> aujourd'hui puisque la chaîne est jetée, mais à **ne pas recopier** dans le portage.

> Cas 6 et 7 seulement : `FindWindow(344)` puis `OnMsg(0, 6, 415)` — un clic simulé sur le
> contrôle 415 d'une autre fenêtre (`0x00CDF52C` / `0x00CDF54F`). Non élucidé, hors périmètre.

**Comptes de la table serveur** (`clif_produceeffect(sd, flag, nameid)`, `clif.cpp:10316`) :
`0`/`1` fabrication générique et forge, `2`/`3` pharmacie (`AM_PHARMACY`), `4`/`5` bombes,
`6` `GN_S_PHARMACY` & assimilés. Chaque envoi est précédé d'un `clif_solved_charname` — le
serveur pousse le nom du personnage avec le résultat.

---

## 4. La fenêtre **94** — `UIMakingArrowListWnd` (« LIST »)

### 4.1 Identité

| Élément | Valeur |
|---|---|
| **vtable** | **`0x010345AC`** (`??_7UIMakingArrowListWnd@@6B@`) |
| **windowID** | **94** (`0x5E`) |
| Taille objet | `0xF4` (244 o) — `operator new(0xF4)` dans `MakeWindow` |
| Constructeur | `0x0088E210` → appelle `UIItemIdentifyWnd_ctor` (`0x0088DDE0`) puis **écrase la vtable** |
| `OnCreate` (vt+0x3C) | `0x008A8470` — **partagée telle quelle avec `UIItemIdentifyWnd`** |
| `OnDraw` / titre (vt+0x50) | `0x008B4C30` → `0x008B4740` |
| **`OnMsg`** (vt+0x94) | **`0x008C4A50`** |
| vt+0xB0 (sérialisation replay) | `0x0089C7F0` |
| Pointeur singleton | `g_UIWindowMgr + 0x398` |
| Taille fenêtre | **200 × 200** (`UIWindow_SetSize(0xC8, 0xC8)`) |

> **`UIMakingArrowListWnd` EST la fenêtre d'identification (loupe) déguisée.** Le
> constructeur construit un `UIItemIdentifyWnd` complet puis remplace le pointeur de vtable,
> et `OnCreate` est littéralement la même fonction. Seuls `OnDraw` (le titre) et `OnMsg` (le
> paquet envoyé à la validation) diffèrent.
>
> ⏱ La preuve est visible en mémoire : `OnCreate` écrit MsgString **521**
> (`MSI_ITEM_IDENTIFY` = `« Item Appraisal »`) dans le tampon SSO du titre, puis `OnDraw`
> écrit `"LIST"` par-dessus. Les cinq premiers octets sont remplacés, le reste survit — le
> tampon relevé vaut exactement `"LIST\0Appraisal\0\0"`.

### 4.2 Layout mémoire (⏱ instance vivante `0x14AA9D48`, ouverte par `SA_CREATECON`)

Base `UIWindow` commune :

| Off | Champ | ⏱ valeur |
|-----|-------|----------|
| +0x00 | vtable | `0x010345AC` |
| +0x10 | parent | `0` |
| +0x14 | largeur | `200` |
| +0x18 | hauteur | `200` |
| +0x1C / +0x20 | x / y | `777` / `550` |
| +0x28 | **visible** | `1` — le flag à baisser pour masquer la native |
| +0x2C | **windowID** | `94` |
| +0x8C | `default_id` | `184` |

Membres propres (posés par `OnCreate`, `0x008A8470`) :

| Off | Membre | Posé à | Rôle |
|---|---|---|---|
| +0xB4 | `scrollbar` | `new(0xB4)`, ctor `0x00836870` | barre de défilement, placée en `(largeur−15, 34)`, hauteur `hauteur−55` |
| +0xB8 | `scroll_top` | `0` | index de la première ligne affichée |
| +0xBC | **`sel_index`** | `-1` | index sélectionné (absolu dans la liste) ⏱ `-1` |
| +0xC0 | `columns` | **`1`** | colonnes de la grille |
| +0xC4 | `rows` | **`4`** | lignes visibles ⏱ `4` |
| +0xC8 | `scroll_needed` | `0` | recalculé par `0x008BC3F0` |
| +0xCC | `list._Myhead` | — | **`std::list<ItemSkillInfo>`**, nœud sentinelle ⏱ `0x2503DB78` |
| +0xD0 | `list._Mysize` | — | **nombre d'entrées** ⏱ `1` |
| +0xD4..+0xE3 | `title` (tampon SSO) | `"LIST"` | ⏱ `"LIST\0Appraisal\0\0"` |
| +0xE4 / +0xE8 | `title.size` / `.capacity` | `4` / `15` | |
| +0xF0 | **`mk_type`** (`u16`) | — | ⏱ **`2`** ✔ (forcé par le handler `0x01AD`) |

`0xF0 + 4 = 0xF4` ✔.

> **`columns × rows = 4`** : la fenêtre native ne montre que **quatre produits à la fois**,
> une seule colonne, dans 200 px de large. Tout le reste passe par la barre de défilement.

### 4.3 La liste : une `std::list`, pas un `std::vector`

`Inventory_AppendNewItem` (`0x00799B00`) alloue un nœud de **`0x100` octets** :

```c
struct Node {           // 0x100
    Node*         next; // +0x00
    Node*         prev; // +0x04
    ItemSkillInfo info; // +0x08, 0xF8 octets
};
```

`OnMsg(0x4B)` purge la liste (`sub_799A00`), `OnMsg(0x1F)` y pousse une entrée.
`UIItemIdentifyWnd_GetCandidateListEntryAt` (`0x0089E6D0`) la parcourt linéairement pour
résoudre l'index sélectionné — **coût O(n) à chaque validation**.

> ⏱ **Vérification live n°1 — `SA_CREATECON`.** Avec les matériaux d'un seul
> convertisseur : `_Mysize = 1`, nœud unique, et le champ `+0x2C` de son `ItemSkillInfo`
> contient la chaîne **`"12114"`** — Elemental Converter (Feu). Cohérent avec
> `produce_db.txt:391` : `141,12114,23,1007,1,7433,1,904,3`.

> ⏱ **Vérification live n°2 — `AC_MAKINGARROW` (Hunter).** Instance `0x14AA8748`.
> C'est la capture qui valide le DÉFILEMENT, absente de la première :
>
> | Off | Membre | ⏱ valeur |
> |---|---|---|
> | +0xBC | `sel_index` | `-1` |
> | +0xC0 | `columns` | `1` |
> | +0xC4 | `rows` | `4` |
> | +0xC8 | `scroll_needed` | **`1`** |
> | +0xCC / +0xD0 | `list` head / **size** | `0x250467B8` / **`5`** |
> | +0xF0 | `mk_type` | **`2`** |
>
> `pages = ceil(5 / columns) = 5`, `rows = 4` ⇒ `5 − 4 = 1 ≠ 0` ⇒ `scroll_needed = 1` :
> la formule de `UpdateScrollRange` (`0x008BC3F0`) se vérifie au chiffre près. **Cinq
> produits pour quatre lignes visibles — le joueur doit défiler dès la cinquième.**
>
> La `std::list` parcourue nœud à nœud donne les ids `994`, `911`, `713`, `904`, `990`
> (Flame Heart, Scell, Empty Bottle, Scorpion Tail, Red Blood), et le `next` du cinquième
> nœud revient exactement sur la sentinelle — la liste circulaire referme sur `_Mysize`.
>
> Et surtout : **`mk_type` vaut 2 ici aussi**, sur une compétence DIFFÉRENTE de la
> première capture. C'est la démonstration directe du §2.2 — `AC_MAKINGARROW` et
> `SA_CREATECON` sont rigoureusement indiscernables côté client, même paquet, même
> fenêtre, même `mk_type` en dur. Le client ne peut pas savoir ce qu'il fabrique.

### 4.4 `ItemSkillInfo` — ce qu'il faut en savoir

| Off | Champ | Note |
|---|---|---|
| **+0x2C** | `std::string` | **l'id d'objet, en DÉCIMAL, sous forme de TEXTE** |
| +0x3C / +0x40 | `.size` / `.capacity` | ⏱ `5` / `15` pour `"12114"` |
| +0x88 | `u16` grade | `> 0` ⇒ le rendu superpose `grade_icon%d.bmp` |
| — | taille totale | `0xF8` (248 o) |

`ItemSkillInfo_GetId` (`0x005D98A0`) vaut exactement `atoi(info + 0x2C)`.

> 🔴 **L'id d'objet transite en `std::string`.** C'est contre-intuitif et ça se paie deux
> fois : `SendMsg` case 130 fait trois `atoi` pour reconstituer les matériaux, et toute
> comparaison d'id passe par une conversion. Un portage ImGui n'a **aucune raison** de
> reproduire ça : il garde des `uint32`.

### 4.5 Le rendu (`0x008B4740`)

```c
UIWindow_DrawTitleBar(this, 0, title, 0);        // paramètre 1 = 0 ; la 79 passe 1
for (node = list.begin, i = 0; node != head; node = node->next, ++i) {
    if (i < columns * scroll_top) continue;      // saute la partie défilée
    y = 32 * (drawn / columns) + 34;
    if (sel_index == i)
        FillRect(5, y - 3, largeur - 24, 30, couleur_surlignage);   // sub_A1D460
    UIWnd_BlitIconByResName(this, 8,  y, &node->info);              // icône, x = 8
    UIItemSkillDescWnd_DrawName(this, 40, y, &node->info, …, 12, 0); // nom, x = 40
    if (node->info.grade > 0)
        Blit("유저인터페이스\\grade_enchant\\grade_icon%d.bmp", 8, y + 12);
    if (++drawn == columns * rows) return;        // 4 lignes, puis stop
}
```

Une ligne = **32 px**, icône à `x=8`, nom à `x=40`. La couleur de surlignage vient de
`sub_7A6DF0(6, 2, …)` (palette d'interface). **Rien d'autre n'est dessiné** : ni quantité,
ni matériaux, ni stock, ni compteur de pages.

### 4.6 `OnMsg` (`0x008C4A50`) — le cœur

```c
case 0x06:  // CLIC BOUTON
    Replay_RecordUIEvent(this, id, …, sel_index);
    if (id == 184) {                                   // OK
        GetCandidateListEntryAt(this, &info, sel_index, &ok);
        if (ok) {
            if (mk_type == 2) CMode::SendMsg(153, GetId(&info));            // → CZ 0x01AE
            else              CMode::SendMsg(207, GetId(&info), mk_type);   // → CZ 0x025B
            SaveRectAndCloseWindow(94);                // ⚠ DÉTRUIT la fenêtre
        }
        // ok == false (aucune sélection) : rien, la fenêtre RESTE ouverte
    } else if (id == 185) {                            // Annuler
        if (mk_type == 2) CMode::SendMsg(153, -1);
        else              CMode::SendMsg(207, -1, mk_type);
        SaveRectAndCloseWindow(94);
    }

case 0x07: sel_index += Value;                    // molette / flèches
case 0x09: sel_index  = sel_index - rows + 1;     // page précédente
case 0x0A: sel_index  = sel_index + rows - 1;     // page suivante
           → si changement : recalcul scrollbar (0x8BC3F0) + repaint (vt+0x98)

case 0x1F: ItemSkillInfo_SetId(&tmp, *(u32*)Value);   // ⚠ Value = POINTEUR
           Inventory_AppendNewItem(&this->list, &tmp);
           this->OnMsg(0, 60, …);                     // rebuild

case 0x22: mk_type = (u16)Value;
case 0x3C: recalcul scrollbar + repaint
case 0x4B: purge de la liste (sub_799A00)
case 0x7B: restitution replay — TLV 22100 / 22101 / 22102 (mk_type) / 22103 (une entrée)
default:   return 0;                              // ⚠ PAS de UIWindow_OnMsg_Default !
```

Détails qui comptent :

- **Annuler envoie un paquet**, avec `itemId = -1`. C'est le désarmement du `menuskill`
  côté serveur (`clif_menuskill_clear` en fin de parseur). Un remplacement ImGui **doit**
  reproduire cet envoi à la fermeture, sinon le joueur reste avec un `menuskill_id` armé.
- **OK sans sélection ne fait rien** et ne ferme pas — `sel_index = -1` fait échouer
  `GetCandidateListEntryAt`, qui renvoie `ok = 0`.
- `case 7/9/10` déplacent la **sélection**, pas le défilement : c'est `0x008BC3F0` qui
  recale ensuite `scroll_top` dans `[0, pages − rows]`.
- ⚠ Le `default` **ne délègue pas** à `UIWindow_OnMsg_Default`, contrairement à la 79 et à
  `UIWeaponRefineWnd`. Cette fenêtre ignore donc tous les messages génériques.
- ⚠ `UIWindowMgr_SaveRectAndCloseWindow` (`0x00A2E770`) **détruit** la fenêtre. Le nom est
  trompeur, c'est déjà noté dans l'IDB.

---

## 5. La fenêtre **79** — `UIMakeTargetListWnd` (« Manufacturing List »)

### 5.1 Identité

| Élément | Valeur |
|---|---|
| **vtable** | **`0x0103EC50`** (`??_7UIMakeTargetListWnd@@6B@`) |
| **windowID** | **79** (`0x4F`) |
| Taille objet | `0xC4` (196 o) |
| Constructeur | `0x00965660` |
| `OnCreate` (vt+0x3C) | `0x009673A0` |
| `OnDraw` / titre (vt+0x50) | `0x009696B0` — MsgString **425** `MSI_MAKE_LIST` = **« Manufacturing List »** |
| **`OnMsg`** (vt+0x94) | **`0x0096A0F0`** |
| Pointeur singleton | `g_UIWindowMgr + 0x374` |
| Taille fenêtre | **280 × 150** |

⏱ Vérifié en live, deux fois :
- `g_UIWindowMgr + 0x374 == 0` pendant que la 94 est ouverte — les deux fenêtres sont bien
  indépendantes ;
- **Pharmacy sur Alchemist ouvre bel et bien la 79**, pas la « LIST » : instance
  `0x2289DE50`, vtable `0x0103EC50`, `windowID = 79`, `280 × 150`, à `(749, 352)`.
  La RE l'avait prédit avant l'observation (§0).

### 5.1 bis — relevé live de l'instance

| Off | Membre | ⏱ valeur |
|---|---|---|
| +0x00 | vtable | `0x0103EC50` ✔ |
| +0x14 / +0x18 | largeur / hauteur | `280` / `150` ✔ |
| +0x28 | visible | `1` |
| +0x2C | windowID | `79` ✔ |
| +0x8C | `default_id` | `184` ✔ |
| +0xB4 | `listbox` | `0x0F20CB30` (vtable `0x0102C1B8` = `UIListBox` ✔) |
| +0xB8 / +0xBC / +0xC0 | `vector<int>` begin / end / cap | `0x25F872F0` / `0x25F872FC` / `0x25F872FC` |

`end − begin = 12` ⇒ **3 entrées**, et le vecteur contient `501`, `504`, `505` —
**Red Potion, White Potion, Blue Potion**, exactement les trois lignes affichées. Ce sont
des `int32` bruts : ça confirme que `OnMsg(0x1F)` de la 79 reçoit la **valeur** de l'itemId,
là où celui de la 94 reçoit un **pointeur** (§3.2).

`listbox + 0x94 = 0` — première ligne sélectionnée, conforme au surlignage à l'écran.

⏱ **Deuxième relevé — la FORGE Blacksmith** (Mini Furnace 612, `produce 21`). Instance
`0x2289EB50`, vtable `0x0103EC50`, listbox `0x0F20F990`, vecteur `0x23257938 → 0x2325793C`
soit **1 entrée : `998` (Iron)**. Cohérent avec `produce_db.txt:112` —
`112,998,21,94,1,1002,1` : Iron ← `BS_IRON` niv. 1 + 1× Iron Ore.

> La 79 sert donc bien **trois familles de métiers** vérifiées en live : Pharmacy
> (`AM_PHARMACY`), la fonte de métaux et la forge (`produce()` de script). Le vecteur est
> dans les trois cas un `std::vector<int>` d'ids bruts.

> ⏱ **Chemin « liste vide » confirmé aussi.** Le même Alchemist sans matériaux déclenche
> précisément la modale du §3.1 : titre `Message`, texte `You can't create items yet.`
> (MsgString 424), 280 × 120 — et rien d'autre.

### 5.2 `OnCreate` (`0x009673A0`) — strictement le layout de `UIWeaponRefineWnd`

| Enfant | Classe | Position / taille | id |
|---|---|---|---|
| Liste | `UIListBox` (`new 0xD4`, ctor `0x00835C60`) | `(12, 22)`, `280−24 × 150−55` = **256 × 95** | `184` |
| OK | `UIBitmapButton` (`btn_ok`…) | `(280−91, 150−24)` = `(189, 126)` | `184` |
| Annuler | `UIBitmapButton` (`btn_cancel`…) | `(280−46, 150−24)` = `(234, 126)` | `185` |

Trois couleurs de liste à `240, 240, 240` (offsets `+0x7C/+0x80/+0x84`), `default_id` = 184.

> Le même piège que sur le refine : **la liste et le bouton OK portent tous deux l'id 184**.
> `OnMsg` case 6 ne reçoit que les boutons, donc ça passe — mais un dispatcher générique par
> id se ferait piéger.

| Off | Membre |
|---|---|
| +0xB4 | `listbox` |
| +0xB8 / +0xBC / +0xC0 | `std::vector<int>` des **itemId** (begin / end / cap) |

`0xC0 + 4 = 0xC4` ✔.

### 5.3 `OnMsg` (`0x0096A0F0`)

```c
case 0x1F:  // AJOUT — Value = itemId (VALEUR, cette fois)
    vec.push_back(Value);
    ItemSkillInfo_SetId(&info, Value);
    listbox->AddString( ItemSkillInfo_ComposeDisplayName(&info) );   // vt+0xD8

case 0x06:
    if (id == 184) {                                   // OK
        if (UIListBox_GetItemCount(listbox) == 0) {
            SaveRectAndCloseWindow(79);  SaveRectAndCloseWindow(79);   // ⚠ deux fois
        } else if (listbox->selIndex >= 0) {
            switch (g_Own_JobId) {
              case 4054: case 4060: case 4096: case 4252:              // Rune Knight
                  ItemSkillInfo mats[3];                               // vides
                  CMode::SendMsg(130, vec[sel], mats);                 // → CZ 0x018E
                  SaveRectAndCloseWindow(79);
                  break;
              default:
                  wnd80 = MakeWindow(80);
                  wnd80->OnMsg(0, 40, vec[sel]);                       // produit visé
                  wnd80->SetPos(this->x, this->y);                     // se colle sur la 79
                  SaveRectAndCloseWindow(79);
            }
        }
        // selIndex < 0 : rien, la fenêtre reste ouverte
    } else if (id == 185) {                            // Annuler
        ItemSkillInfo mats[3];
        CMode::SendMsg(130, 0, mats);                  // → CZ 0x018E avec itemId = 0
        SaveRectAndCloseWindow(79);
    }

case 18: case 19: relayés tels quels à la listbox
case 123: replay — TLV 22050 / 22051 / 22052
default: UIWindow_OnMsg_Default(...)
```

> **Les quatre jobs court-circuités sont TOUTES les variantes de Rune Knight** :
> `JOB_RUNE_KNIGHT` 4054, `JOB_RUNE_KNIGHT_T` 4060, `JOB_BABY_RUNE_KNIGHT` 4096,
> `JOB_DRAGON_KNIGHT` 4252 (`src/common/mmo.hpp:989/996/1028/1077`). Cohérent :
> `RK_RUNEMASTERY` n'admet aucun matériau optionnel, la fenêtre 80 n'aurait rien à montrer.
>
> ⚠ **Le test porte sur le JOB, pas sur la compétence.** Un Rune Knight qui lancerait une
> fabrication à matériaux optionnels (via un `produce()` de script) perdrait donc l'accès à
> la fenêtre 80. C'est un défaut structurel du natif, à ne pas reproduire : le portage doit
> se déterminer sur **ce que la recette accepte**, pas sur la classe du joueur.

---

## 6. La fenêtre **80** — `UIMakeTargetProcessWnd` (choix des matériaux)

| Élément | Valeur |
|---|---|
| **vtable** | **`0x0103EED8`** (`??_7UIMakeTargetProcessWnd@@6B@`) |
| **windowID** | **80** (`0x50`) |
| Taille objet | `0x3A0` (928 o) |
| Constructeur / `OnCreate` / `OnDraw` / `OnMsg` | `0x009656E0` / `0x00967E30` / `0x009696F0` / **`0x0096A6D0`** |
| Pointeur singleton | `g_UIWindowMgr + 0x378` |
| Taille fenêtre | **280 × 150** |

Layout mémoire :

| Off | Membre | ⏱ valeur |
|---|---|---|
| +0x00 | vtable | `0x0103EED8` ✔ |
| +0x14 / +0x18 | largeur / hauteur | `280` / `150` ✔ |
| +0x28 / +0x2C | visible / **windowID** | `1` / **`80`** ✔ |
| +0xB4 | `itemId` du produit visé (posé par `OnMsg(40)`) | **`998`** Iron, puis **`1101`** Sword ✔ |
| +0xB8 | `ItemSkillInfo` emplacement matériau **1** (`0xF8`) | vide ✔ |
| +0x1B0 | `ItemSkillInfo` emplacement matériau **2** | vide ✔ |
| +0x2A8 | `ItemSkillInfo` emplacement matériau **3** | vide ✔ |

`0x2A8 + 0xF8 = 0x3A0` ✔ — l'objet est exactement ses trois emplacements.

⏱ Relevé live sur un Blacksmith, instance `0x1661D700`, **dans les deux modes** :

- **Mini Furnace → Iron (998)** : produit dans `[994, 1000]`, donc **aucun emplacement
  dessiné** (§6.2) — l'écran ne montre que le titre et la recette ;
- **Iron Hammer → Sword (1101)** : produit hors plage, les **trois emplacements
  apparaissent** et les trois `ItemSkillInfo` sont vides.

La bifurcation du §5.3 est donc confirmée : un job qui n'est **pas** Rune Knight passe bien
par cette fenêtre. Et la chaîne d'id de chaque emplacement vide est une `std::string` SSO
nulle (`size = 0`, `capacity = 15`) : le `atoi()` de la commande 130 y lit donc **0**, ce qui
est exactement « aucun matériau optionnel ».

### 6.0 bis — retrouver la 80 sans exécuter de code (et : la 79 lui survit)

⏱ Seconde capture, sur un autre personnage (« Hammer Create », instance **`0x23347F38`**) :
`+0x14/+0x18 = 280 × 150`, `+0x1C/+0x20 = 797, 299`, `+0x28 = 1`, `+0x2C = 80`, vtable
`0x0103EED8` — tout reconfirmé sur une instance différente.

La méthode vaut d'être notée, parce qu'elle se réutilise pour **toute** fenêtre absente du
`switch` de `UIWindowMgr::FindWindow` (`0x00A47B90`) : ce `switch` ne code en dur qu'une
trentaine d'ids ; **80 n'en fait pas partie** et tombe dans le chemin générique, une
`std::map<int, UIWindow*>` en **`g_UIWindowMgr + 8`**. On la parcourt donc à la lecture
seule, sans appeler `FindWindow` dans le client :

```
g_UIWindowMgr+8   -> { _Myhead, _Mysize }        @0x0131F4F0 = 0x01944270, 8 fenêtres
_Myhead+4         -> racine                       (le _Parent de la tête)
nœud MSVC : _Left+0, _Parent+4, _Right+8, _Color+12, _Isnil+13, clé+16, valeur+20
```

⏱ Descente réelle : racine clé 36 → droite clé 190 → gauche clé **79** → droite clé **80**,
valeur `0x23347F38`.

Deux enseignements au passage :

- **la fenêtre 79 est TOUJOURS VIVANTE** dans la map pendant que la 80 est à l'écran (clé 79,
  `0x228F4DC0`). Elle n'est pas détruite en passant la main — notre masquage (`+0x28 = 0`)
  reste donc nécessaire pendant toute la durée de la 80, et un plugin qui la libérerait à cet
  instant casserait le retour ;
- pour une classe hors `switch`, il n'existe **aucun singleton** en `g_UIWindowMgr + N` à
  lire directement (la 80 en a un, `+0x378` ; la plupart n'en ont pas). La map est le seul
  chemin général.

### 6.1 🔴 Le client CONNAÎT des recettes — correction du §9

C'est la découverte la plus importante de cette campagne, et elle **contredit** ce que ce
document affirmait d'abord (« la recette n'atteint jamais le client »). Elle a été faite en
regardant un écran, pas du code : la fenêtre 80 affiche
`« Iron Create » / « Iron's required materials » / « 1 Iron Ore »`.

`UIMakeTargetProcessWnd::OnDraw` (`0x009696F0`) compose ça ainsi :

```c
DrawName(this, 16,  2, info);  DrawText(MsgString(426));  // 426 = " Create"
DrawName(this, 16, 22, info);  DrawText(MsgString(427));  // 427 = "'s required materials"
auto* lines = MetalProcessRecipe_GetLines(this->product);  // 0x006A3F20
for (i = 0; i < lines->size(); ++i)
    UIWindow_DrawText(this, 16, 42 + 20 * i, (*lines)[i], …);
```

`MetalProcessRecipe_GetLines` interroge une **`std::map<int, std::vector<char*>>`** à
`0x01255118` (`g_MetalProcessRecipeMap`), remplie par `ItemInfoDB_LoadFromTextFiles`
(`0x006A4E20`) depuis le fichier de données **`MetalProcessItemList.txt`** (appel
`0x006A59DA`). Renvoie le vecteur vide `0x01255190` si l'id est absent — jamais nul.

> ⚠ **`std::vector<char*>`, pas `std::vector<std::string>`.** Élément de **4 octets**,
> pointeur brut vers une chaîne terminée par NUL dans le tampon de texte du fichier. Deux
> choses le disent : `OnDraw` divise par 4 (`(v10[1] - *v10) >> 2`) puis déréférence
> l'élément en `char*` ; et ⏱ en mémoire, l'entrée `1201` (Knife) a `end − begin = 8` pour
> **deux** lignes — `"1 Iron"` et `"10 Jellopy"`, conformes à `produce_db.txt:10`. Supposer
> un `std::string` (foulée 24) fait lire une entrée sur six et déréférencer du contenu de
> chaîne comme un pointeur.
>
> Disposition du nœud de map (relevée ⏱, et cohérente avec le décompilé) :
> `left@0`, `parent@4`, `right@8`, `color@12`, `isnil@13`, **`key@16`**, **`value@20`**.

**Format du fichier**, lu dans le parseur (`0x006A5A00`-`0x006A5A4B`) :

```
998                 ← une ligne TOUT EN CHIFFRES = un id de produit
Iron's required materials
1 Iron Ore          ← les lignes suivantes sont poussées TELLES QUELLES
999
…
```

⏱ **Contenu réel du fichier** (extrait du GRF), séparateur `#` :

```
998#
1 Iron Ore
#
999#
5 Iron
1 Coal
#
1101#
2 Iron
#
```

**90 produits distincts**, et — contrairement à ce que son nom laisse croire — la couverture
**dépasse largement la fonte de métaux** : on y trouve les armes (`1101` Sword, `1104`
Falchion, `1119`, `1123`…) **et les potions** (`501` Red Potion). C'est donc exploitable pour
les trois familles de la fenêtre 79.

> 🔴 **Deux limites, et elles décident du design du portage :**
>
> **1. C'est du texte d'affichage, pas de la donnée structurée.** Ni id de matériau, ni
> quantité exploitable — impossible d'en déduire « combien puis-je en fabriquer », ou de
> griser ce qui manque. Il existe bien un `MetalProcessItemTable.txt` **structuré** à côté
> (`998#1#` / `1002#1#` = produit, nombre de matériaux, puis `id#quantité`), dont le contenu
> recoupe exactement `produce_db.txt:112-118` — mais **ce client ne le lit pas** : la chaîne
> `MetalProcessItemTable` est absente de tout le binaire (balayage brut de l'image, pas
> seulement de la liste de chaînes d'IDA). Fichier mort, hérité d'un client plus ancien.
>
> **2. Il est écrit côté client et DÉRIVE du serveur — mesuré, pas supposé.** Comparaison
> des ids du `.txt` avec les produits de `db/pre-re/produce_db.txt` du fork :
>
> | | Nombre |
> |---|---|
> | Produits avec recette client | 90 |
> | Produits fabricables côté serveur | 254 |
> | **Fabricables SANS recette côté client** | **166** (`523`, `678`, `703`, `958`-`964`, `1003`, `1008`-`1014`…) |
> | Recettes client SANS production serveur | 2 (`100371`, `500000`) |
>
> Autrement dit : **près des deux tiers de ce que le serveur sait fabriquer n'a aucune
> recette affichable**, et deux entrées client ne correspondent à rien. Un serveur qui
> ajuste `produce_db.txt` sans toucher au `.txt` affichera un mensonge sans que rien ne le
> signale.

> Conclusion pour le §9 : le manque n'est pas « aucune recette », c'est **« une recette
> textuelle, partielle (35 %) et non vérifiée »**. L'afficher dans l'ImGui est **gratuit et
> immédiat** — la map est déjà en mémoire, `MetalProcessRecipe_GetLines` la lit. En faire
> quelque chose de *calculable* et de *fiable* demande toujours l'opcode custom.

### 6.2 Les emplacements ne sont pas toujours dessinés

```c
if ((unsigned)(this->product - 994) > 6)   // hors [994, 1000]
    …dessine les trois emplacements (x = 8, 40, 72, pas de 32)…
```

Les produits **994 à 1000** — Flame Heart, Mystic Frozen, Rough Wind, Great Nature, Iron,
Steel, Star Crumb, c'est-à-dire toute la **fonte de métaux** — n'affichent **aucun
emplacement** : ces recettes n'admettent pas de matériau optionnel. C'est ce que montre la
capture ⏱ ci-dessus.

> ⚠ **Cette règle-là est la bonne, et elle n'est pas celle de la fenêtre 79.** La 79 décide
> d'ouvrir la 80 sur le **job** (Rune Knight ou pas, §5.3) ; la 80 décide de montrer ses
> emplacements sur le **produit**. Le second critère est le seul qui ait un sens. Un
> remplacement doit s'aligner sur celui-là.

```c
case 40:  this->product = Value;  repaint();          // le produit visé, venu de la 79

case 6:
    if (id == 184) {                                   // OK
        CMode::SendMsg(130, this->product, &slots[0]); // → CZ 0x018E, 3 atoi
        SaveRectAndCloseWindow(80);
    } else if (id == 185) {                            // Annuler
        CMode::SendMsg(130, 0, &slots[0]);             // annule côté serveur
        for (s : slots) if (s.occupé)                  // ← RENDS les matériaux
            Inventory_AddOrStackItem(g_UIWindowContextKey, &s, 0);
        if (des matériaux ont été rendus) UI_RefreshItemWindows();
        if (g_UIWindowMgr+0x1D4) → OnMsg(0, 23, 0);
        SaveRectAndCloseWindow(80);
    }

case 75:  // double-clic sur un emplacement : retire le matériau
    trouve l'emplacement dont le nom de ressource correspond ;
    Inventory_AddOrStackItem(…, slot, 1);  ItemSkillInfo_Reset(slot);  repaint();

case 123: replay — TLV 22000 / 22001 / 22002
default:  UIWindow_OnMsg_Default(...)
```

> 🔴 **Les matériaux posés dans la 80 sortent RÉELLEMENT de l'inventaire côté client.** Le
> bouton Annuler doit les rendre un par un (`Inventory_AddOrStackItem`), et le fait. Toute
> autre sortie que « OK » ou « Annuler » — une fermeture par le gestionnaire, un warp, un
> plantage — **perd l'affichage** des objets jusqu'au prochain rafraîchissement complet.
> C'est la raison pour laquelle un remplacement ImGui de cette fenêtre-là ne doit
> **jamais** piloter les emplacements natifs : il doit tenir son propre modèle et n'envoyer
> que le `0x018E` final.

---

## 7. Les règles serveur

### 7.0 🔴 Un `menuskill_id` étranger VIDE la liste — et le blocage est circulaire

La plus vicieuse des règles, et elle tient en deux lignes de `skill_can_produce_mix`
(`skill.cpp` ~13304) :

```c
if (j > 0 && sd->menuskill_id > 0 && sd->menuskill_id != j)
    continue; // special case
```

`j` est le `req_skill` de la recette examinée. Donc **tout `menuskill_id` positif qui ne
correspond pas à la recette la fait écarter** — et comme la boucle sort alors sans avoir
trouvé d'entrée, la fonction rend 0. Recette par recette, la liste entière est vidée.

Enchaînement complet, constaté en jeu deux fois :

1. le joueur lance **Upgrade Weapon** → `menuskill_id = WS_WEAPONREFINE` (positif) ;
2. il refine une arme → `clif_menuskill_clear` → 0 ✔ ;
3. **mais la relance automatique du refine réarme** `WS_WEAPONREFINE` ;
4. il utilise une Mini Furnace → `clif_skill_produce_mix_list` passe le filtre… qui écarte
   TOUTES les recettes de métaux (`req_skill` 94/95/96 ≠ WS_WEAPONREFINE) ;
5. liste **vide** → `if (count > 0)` est faux → le serveur **n'arme rien** → le `menuskill`
   du refine SURVIT ;
6. dès lors, plus rien ne repart : le refine refuse de se relancer, et chaque nouvelle
   Mini Furnace rejoue l'étape 4. Jusqu'à un changement de carte.

> **Conséquence côté client.** Fermer la fenêtre de fabrication n'y change rien : son
> annulation (CZ 0x018E, `itemId = 0`) sort par le `default:` de `clif_parse_ProduceMix`
> **sans rien effacer** — ce handler ne connaît que `-1`, `AM_PHARMACY`, `RK_RUNEMASTERY` et
> `GC_RESEARCHNEWPOISON`. Seul `clif_parse_WeaponRefine` peut effacer un `WS_WEAPONREFINE`.
>
> C'est donc au plugin de REFINE d'envoyer son `182 / -1` quand une liste de fabrication
> arrive (`WeaponRefineWindow::CloseForOtherCraft`). Une première rédaction fermait sans rien
> envoyer, en supposant que la fabrication avait écrasé la session : vrai seulement si sa
> liste est pleine, faux exactement dans le cas qui bloque.

⚠ Un correctif SERVEUR serait légitime : ce `// special case` fait dépendre le contenu d'une
liste de fabrication d'une session de compétence sans rapport. La rendre insensible à un
`menuskill_id` d'une autre famille supprimerait la classe entière.

### 7.0 bis 🔴 La même règle frappe fabrication ↔ fabrication — et là c'est le PROTOCOLE qui se perd

Le §7.0 décrit le cas refine → fabrication, réglé côté refine (`CloseForOtherCraft`). La
variante **fabrication → fabrication** reste dans le même plugin, et elle est pire : elle
n'écrase pas une session étrangère, elle écrase **le protocole d'annulation de la sienne**.

⏱ Constaté en jeu : *« utiliser la furnace durant un skill de craft continu bloque le
fonctionnement des skills après fermeture »*.

1. le joueur lance **Arrow Crafting** (`AC_MAKINGARROW`) → `ZC 0x01AD` non vide →
   `menuskill_id = 147`, et côté client `proto_ = kArrow` ;
2. il utilise une **Mini Furnace** → `produce 21;` → `clif_skill_produce_mix_list(sd, -1, 21)` ;
3. le filtre du §7.0 écarte toutes les recettes de métaux (`req_skill` 94..97 ≠ 147) → **count = 0** ;
4. le serveur envoie quand même la liste (`clif_send` est AVANT le `if (count > 0)`), donc
   `ZC 0x018D` arrive **vide** et `menuskill_id` reste **147** ;
5. le client, lui, adoptait `proto_ = kProduce` (fenêtre 79) et remettait `skill_id_ = 0` ;
6. à la fermeture, l'annulation partait donc en **`CZ 0x018E`** — dont le parseur ne connaît pas
   `AC_MAKINGARROW` : `default: return`, **aucun effacement** ;
7. `menuskill_id = 147` armé à vie ⇒ `clif_parse_skill_toid` refuse **tout** lancement
   (`« Can't use skills while a menu is open »`, `clif.cpp:15291`). Plus un seul skill ne part
   jusqu'au changement de carte, à la mort ou à la déconnexion.

**La règle client qui supprime la classe entière** — et elle se lit directement dans le serveur :

> Une liste dont `count == 0` n'a **rien armé**. La session précédente est donc **toujours
> vivante**, avec son `menuskill_id` et son protocole. Il ne faut donc RIEN en écraser : ni
> `proto_`, ni `skill_id_`, ni `list_armed_`, ni `entries_` (la table affichée appartient à la
> session vivante et reste valable), ni fermer le refine.

Implémenté comme un garde en tête d'`OnRecvPacket`, **avant** toute écriture de membre — d'où
`incoming_proto` / `incoming_mk_type` en locales : la version précédente posait `proto_` dès la
lecture de l'en-tête, c'est-à-dire avant même de connaître le compte.

Corollaire qui rend l'annulation `CZ 0x018E` suffisante : les **seules** sessions qui ouvrent la
fenêtre 79 sont `produce N;` (menuskill `-1`), `AM_PHARMACY` (`skills/merchant/preparepotion.cpp`),
`GC_CREATENEWPOISON` (renommé `GC_RESEARCHNEWPOISON` à l'envoi) et `RK_RUNEMASTERY` — exactement
la liste blanche de `clif_parse_ProduceMix`. Donc `proto_ == kProduce` ⇒ menuskill effaçable,
**à condition que `proto_` décrive la session ARMÉE**. C'est tout l'objet du garde.

Note : les quatre compétences de forge (`BS_IRON` 94, `BS_STEEL` 95, `BS_ENCHANTEDSTONE` 96,
`BS_ORIDEOCON` 97) sont **passives** — elles n'ouvrent aucune liste et ne sont jamais un
`menuskill_id`. C'est justement pourquoi la Mini Furnace existe, et pourquoi c'est toujours
`-1` qu'on trouve armé sur ce chemin.

#### Pourquoi il n'y a RIEN à patcher côté serveur

Tentation évaluée puis écartée : ajouter un `clif_menuskill_clear` au `default:` de
`clif_parse_ProduceMix`, ou aligner `clif_skill_produce_mix_list` sur `clif_cooking_list`
(qui, elle, n'envoie rien et efface quand `count == 0`). **Les deux sont à rejeter.**

1. **Le client natif est immunisé.** Sur liste vide il affiche sa modale et n'ouvre AUCUNE
   fenêtre (§3.1, relevé live : branche modale avant `MakeWindow`, `g_UIWindowMgr+0x374`
   nul). Il n'émet donc jamais le `CZ 0x018E` qui bloque. Le bogue est **entièrement** de
   notre fait : nous ouvrons une fenêtre annulable là où le natif se contente d'un message.
   Un patch serveur ne pourrait que retirer cette modale au natif — du risque pour rien.
2. **Le `default: return` n'est pas un oubli**, c'est un idiome partagé par
   `clif_parse_RepairItem`, `clif_parse_WeaponRefine` (commenté `//Packet exploit?`),
   `clif_parse_AutoSpell` et `clif_parse_UseSkillMap` : « ce paquet ne concerne pas la session
   ouverte, je l'ignore ». Sans lui, un `0x018E` forgé annulerait n'importe quel menu.
3. **Effacer dans le `else` détruirait des sessions légitimes** : `AM_PHARMACY` armé plus une
   Mini Furnace, et le joueur perd sa liste de potions — laquelle était parfaitement
   utilisable. (`clif_cooking_list` a d'ailleurs ce défaut-là, atteignable par un ustensile de
   cuisine ; il n'est pas bloquant, on n'y touche pas.)
4. Et surtout, le patch serait **incompatible avec le garde ci-dessus** : celui-ci repose sur
   « liste vide ⇒ rien changé ». Un serveur qui effacerait dans le `else` rendrait cette
   prémisse fausse et le garde mensonger.

Corollaire sur les menuskills que `CZ 0x018E` n'efface pas : `MC_IDENTIFY` et
`BS_REPAIRWEAPON` **ne bloquent pas** malgré l'apparence. Leur fenêtre native est vivante — ils
ne sont remplacés par aucun plugin — et leurs propres parseurs (`clif_parse_ItemIdentify`,
`clif_parse_RepairItem`) effacent le menuskill. Notre annulation qui les traverse sans effet est
**inutile, pas nuisible**. Ce qui distinguait `AC_MAKINGARROW`, c'est que sa session était la
NÔTRE et que sa native n'existe plus : plus rien ne pouvait l'effacer.

Ce qui reste NON réparable côté client, et qu'il faut connaître :

- l'objet est **perdu** : `pc_useitem` fait son `pc_delitem(LOG_TYPE_CONSUME)` **avant**
  d'exécuter le script, donc l'exemplaire part même quand la liste revient vide. Le plugin le
  DIT dans son journal, c'est tout ce qu'il peut faire ;
- ré-utiliser le même objet pendant sa propre session ne renvoie **rien du tout** :
  `clif_skill_produce_mix_list` commence par `if (sd.menuskill_id == skill_id) return;`
  (« Avoid resending the menu ») et `-1 == -1`. Aucun paquet, donc aucun moyen de le signaler.

### 7.1 La recette vit dans `db/pre-re/produce_db.txt`

```
// ID,ProduceItemID,ItemLV,RequireSkill,RequireSkillLv,MaterialID1,MaterialAmount1,...
141,12114,23,1007,1,7433,1,904,3      // Elemental Converter (Feu)  ← 1 Blank Scroll + 3 Scorpion Tail
142,12115,23,1007,1,7433,1,946,3      // (Eau)   ← 1 Blank Scroll + 3 Snail's Shell
143,12116,23,1007,1,7433,1,947,3      // (Terre) ← 1 Blank Scroll + 3 Horn
144,12117,23,1007,1,7433,1,1013,3     // (Vent)  ← 1 Blank Scroll + 3 Wind of Verdure
```

- `ItemLV` **est** le `trigger` des §2.1/§2.3.
- `MaterialAmount == 0` a un sens particulier : l'objet doit être **présent** en inventaire
  mais n'est pas consommé (les « guides » de fabrication).
- ⏱ Ces quatre lignes confirment `SA_CREATECON = 1007` dans ce fork, et la recette lue à
  l'écran (« 3 Scorpion Tail + 1 Blank Scroll »).

> **Rien de tout ça n'atteint le client.** C'est le gisement de QOL de ce chantier.

### 7.2 Ce qui entre dans la liste — `skill_can_produce_mix` (`skill.cpp:13290`)

Renvoie `index + 1` (donc « vrai ») si **toutes** ces conditions tiennent :

1. l'objet existe dans `item_db` ;
2. une entrée de `skill_produce_db` le produit, avec `pc_checkskill(req_skill) ≥ req_skill_lv` ;
3. si un `menuskill_id` est déjà armé, il doit correspondre au `req_skill` de l'entrée ;
4. cas spécial : `ITEMID_HOMUNCULUS_SUPPLEMENT` exige `AM_BIOETHICS` ;
5. `pc_checkadditem(nameid, qty) != CHKADDITEM_OVERAMOUNT` — **le joueur doit pouvoir porter le résultat** ;
6. correspondance du `trigger` :
   - `> 20` → `itemlv` doit être **égal** (objets non-arme non-nourriture),
   - `11..20` → `itemlv` doit tomber dans `]10, 20]` (nourriture),
   - `≤ 10` → `itemlv ≤ trigger` (armes, par niveau) ;
7. **tous les matériaux** en quantité suffisante (`amt ≥ qty × mat_amount`), ou simplement
   présents si `mat_amount == 0`.

> Le point 5 est un piège de diagnostic : une liste qui revient vide peut vouloir dire
> « inventaire plein », pas « il te manque des matériaux ». Le client, lui, affiche
> l'unique message « You can't create items yet. » (et seulement sur `0x018D`).

### 7.3 Le taux de réussite — `make_per`, `skill_produce_mix` (`skill.cpp:13744+`)

Deux familles :

**Potions / pharmacie** (`AM_PHARMACY` et dérivés) :
```c
make_per = job_level/4 + luk/2 + dex/3;      // puis -= difficulty (potion_db)
// puis discrétisé en paliers : ≥30, ≥10, ≥-10, ≥-30
```

**Forge** (`BS_*`, `WS_WEAPONFORGE`) :
```c
make_per  = job_level*20 + dex*10 + luk*10 + rnd(1..100)*10;
make_per += (4/wlv)*1000;                        // +40 / +20 / +10 %
make_per += pc_checkskill(skill_id)*500;         // +5 / +10 / +15 %
make_per += pc_checkskill(BS_WEAPONRESEARCH)*100;
make_per += pc_checkskill(BS_ORIDEOCON)*100;     // si Oridecon impliqué
make_per -= 2500;                                // conditions défavorables
make_per -= sc*1500;                             // par Star Crumb ajouté
make_per += 1000 / 500 / 250 / 0;                // Emperium / Golden / Oridecon / Anvil
make_per  = make_per * battle_config.wp_rate / 100;
if (make_per < 1) make_per = 1;                  // plancher
```

Tirage : `qty > 1 || rnd()%10000 < make_per`. ⚠ **L'échelle est donc 10000 = 100 %** :
`make_per -= 2500` retire bien **25 points de pourcentage**, pas 2,5. (Corrigé après coup — une
première rédaction parlait d'une échelle de 100000 et rendait tous les malus dix fois trop
petits, alors que les pourcentages affichés étaient justes.)

**Cuisine** (`default:` du switch, `skill.cpp:13706`) — la branche est facile à manquer, elle
n'a pas de `case` à son nom :
```c
if (sd->menuskill_id == AM_PHARMACY && menuskill_val > 10 && menuskill_val <= 20) {
    if (menuskill_val >= 15) make_per = 10000;      // Fantastic (12129) : 100 % GARANTI
    else make_per = 1200 * (menuskill_val - 10)     // le KIT : +12 % par niveau
                  + 20 * (base_level + 1) + 20 * (dex + 1)
                  + 100 * (rnd()%span + lo)         // span/lo dérivés de cook_mastery
                  - 400 * (itemlv - 11 + 1)         // niveau du PLAT
                  - 10 * (100 - luk + 1)
                  - 500 * (num - 1)                 // QUANTITÉ fabriquée (= 1 ici)
                  - 100 * (rnd()%4 + 1);
}
```
`cook_mastery` est persisté (`COOKMASTERY_VAR`, plage 0–1999, `pc.cpp:2385`), incrémenté au
succès (`skill.cpp:13879`) et décrémenté à l'échec (`14047`), et exposé par `SP_COOKMASTERY`.

⚠ Deux corrections d'une rédaction antérieure : c'est **Fantastic Cooking Kit (12129)** qui
atteint 15, pas le Royal (12128, niveau 14) — le commentaire serveur parle d'un « Legendary
Cooking Set » qui n'existe pas sous ce nom dans l'item_db. Et `num` est la **quantité
fabriquée** (paramètre `qty` de `skill_produce_mix`), pas un nombre de matériaux ; le buildin
`cooking` passe toujours 1, donc ce terme est nul.

Le terme de maîtrise, développé : `lo = 6 + cm/80`, `span = 30 + 5*(cm/400) - lo`, en divisions
ENTIÈRES qui sautent par paliers (`span` vaut 24 la plupart du temps, 20 tout en haut). Il vaut
donc `[600, 2900]` à maîtrise nulle et `[3000, 4900]` à maîtrise pleine — **22 points de
pourcentage d'écart en moyenne**, mesurés sur la transcription.

#### 🔴 Trois singularités de la cuisine, toutes vérifiées

1. **Aucun multiplicateur.** `pp_rate` est *à l'intérieur* du `case AM_PHARMACY:` et `wp_rate`
   dans la branche « Weapon Forging » : ni l'un ni l'autre n'atteint ce `default:`. Sur un
   serveur où `weapon_produce_rate` et `potion_produce_rate` valent 500, **la cuisine est la
   seule fabrication qui tourne à taux nu**.
2. **Pas de pénalité baby** non plus, pour la même raison de portée.
3. **Aucune compétence** : les 60 recettes (6 par `itemlv`, de 11 à 20) ont `req_skill = 0`.
   N'importe quelle classe cuisine, et `skill_id` reste 0 dans `skill_produce_mix` — c'est
   d'ailleurs ce qui la fait tomber dans le `default:`.

Le filtre de liste traite `trigger` 11..20 **en bloc** (« any item level between 10 and 20 will
do ») : un kit modeste propose donc **tous** les plats. C'est `-400 * (itemlv - 10)` qui punit,
jusqu'à -40 points pour un plat d'`itemlv` 20.

| Kit | Id | `cooking N;` |
|---|---|---|
| Outdoor Cooking Kit | 12125 | 11 |
| Home Cooking Kit | 12126 | 12 |
| Professional Cooking Kit | 12127 | 13 |
| Royal Cooking Kit | 12128 | 14 |
| Fantastic Cooking Kit | 12129 | **15 → 100 %** |
| Combination Kit | 12849 | **30** — hors [11,20] |

⚠ Le **Combination Kit** ne cuisine pas : `menuskill_val = 30` échoue au test `> 10 && <= 20`,
donc le serveur retomberait sur `make_per = 5000`. Sauf qu'il n'y arrive jamais — `produce_db`
n'a **aucune** recette d'`itemlv` 30, sa liste est donc toujours vide et l'exemplaire est perdu
à chaque usage. Objet mort sur ce serveur.

#### `cook_mastery` côté client : ZC 0x0F1C

C'est le seul terme que le client ne peut pas déduire (char reg serveur, aucun chemin vanilla ne
le sort), et son poids interdit toute fourchette utilisable. D'où un opcode custom :
`ZC_BOURGEON_COOK_MASTERY` (0x0F1C, fixe 6), poussé au login vérifié
(`clif_bourgeon_grant_verified`) et à chaque `pc_setparam(SP_COOKMASTERY)`.

Le niveau du **kit**, lui, n'est dans aucun paquet non plus (le serveur le garde dans
`menuskill_val`, et les cinq kits donnent le même `mk_type = 1`) : il se déduit de l'objet
consommé, observé sur `CZ_USE_ITEM`, via la table `cooking_kits` du YAML de recettes.

🔴 **Les cinq ustensiles ne sont donc PAS interchangeables**, contrairement à ce qu'une version
de ce document affirmait. La confusion venait d'avoir vérifié le **filtre de la liste** puis
conclu sur le **taux** : deux choses distinctes.

| | dépend du kit ? |
|---|---|
| la LISTE des plats proposés | **non** — `trigger` 11 à 20 passe le même filtre |
| le TAUX de réussite | **oui** — +12 % par niveau, et **100 % dès le niveau 15** |

> **Le taux de cuisine est le SEUL entièrement calculable côté client**, contrairement à celui
> de la forge et du refine : tous ses termes sont accessibles — niveau du kit (déductible de
> l'objet qui a ouvert la liste), base level, DEX, LUK, `itemlv` du plat, nombre de matériaux,
> et `cook_mastery` (déjà envoyé comme paramètre `SP_COOKMASTERY`). Seul le `rnd()%4` reste
> inconnu, d'où une **fourchette** honnête plutôt qu'un chiffre unique. Meilleur candidat de
> l'Atlas : il suffit d'ajouter au YAML la table `kit → niveau` (12125→11 … 12129→15), que le
> générateur peut extraire des scripts `cooking N;`.

> **Inaccessible au client**, comme le taux de refine. Un pourcentage affiché côté ImGui
> devrait venir d'un opcode custom, ou être clairement étiqueté « estimation » — jamais
> codé en dur.

### 7.4 Les matériaux optionnels de forge (`slot1..3` de `CZ 0x018E`)

`skill_produce_mix` ne consomme que deux familles depuis les trois emplacements :
- `ITEMID_STAR_CRUMB` → `sc++` (jusqu'à 3), inscrit dans `card[1]` de l'arme forgée ;
- `ITEMID_FLAME_HEART` … `ITEMID_GREAT_NATURE` → élément de l'arme (`ELE_FIRE`/`WATER`/`WIND`/`EARTH`),
  **le premier seulement** (`ele == 0` en garde).

Une arme forgée reçoit `card[0] = CARD0_FORGE`, `card[1] = ((sc*5) << 8) + ele`,
`card[2..3] = char_id`. Avec `wlv ≥ 3` et `(ele ? 1 : 0) + sc ≥ 3`, le forgeron gagne
`battle_config.fame_forge` points de renommée.

### 7.5 Les parseurs et leurs gardes

| Paquet | Parseur | Garde `menuskill_id` |
|---|---|---|
| `0x018E` | `clif_parse_ProduceMix` (`clif.cpp:15553`) | `-1`, `AM_PHARMACY`, `RK_RUNEMASTERY`, `GC_RESEARCHNEWPOISON` — sinon **`return` muet** |
| `0x01AE` | `clif_parse_SelectArrow` (`clif.cpp:15812`) | `switch` : `AC_MAKINGARROW`, `SA_CREATECON`, `GC_POISONINGWEAPON`, `NC_MAGICDECOY` |
| `0x025B` | `clif_parse_Cooking` (`clif.cpp:15595`) | `type == 6` exige `GN_MIX_COOKING` ou `GN_S_PHARMACY` |

Les trois : `pc_istrading` ⇒ `clif_skill_fail` + `clif_menuskill_clear`, et **tous** finissent
par `clif_menuskill_clear(sd)`.

🔴 Mais ils n'ont pas la même **force de désarmement**, et c'est ce qui décide si un
personnage se bloque (§7.0 bis) :

| Paquet | Efface le `menuskill` armé… |
|---|---|
| `0x01AE` | **toujours**, quel que soit `menuskill_id` — le `switch` ne sert qu'à choisir l'action, le `clear` est hors switch. Désarmeur universel. |
| `0x025B` | **toujours** hors `type == 6` (qui n'est aucun de nos chemins). |
| `0x018E` | **seulement** pour `-1` / `AM_PHARMACY` / `RK_RUNEMASTERY` / `GC_RESEARCHNEWPOISON`. Le `default: return` sort **avant** le `clear`. |

⚠ Ne pas en conclure qu'il faut envoyer un `0x01AE` « au cas où » : il effacerait aussi un
`MC_IDENTIFY`, `BS_REPAIRWEAPON`, `WS_WEAPONREFINE`, `SA_AUTOSPELL`, `SA_TAMINGMONSTER` ou
`SG_FEEL` en cours, c'est-à-dire fermerait le menu natif d'un autre pan du jeu.

> **Un lancement = une fabrication.** Comme le refine. Enchaîner impose de relancer la
> compétence, avec le coût en SP et le temps de cast.

> Envoyer l'un de ces paquets hors contexte est **sans effet** : aucun exploit possible
> depuis un remplacement client.

> ⚠ **L'annulation par `0x01AE` avec `itemId = -1` n'est pas gardée dans ce fork.**
> Contrairement au rAthena amont, `clif_parse_SelectArrow` n'a pas de test `nameid == 0` :
> `-1` traverse jusqu'à `skill_arrow_create` (`skill.cpp:14061`), qui sort proprement sur
> `!item_db.exists(nameid)`. Le `clif_menuskill_clear` final s'exécute quand même — l'effet
> voulu est donc obtenu, mais **par accident**. À garder en tête si le fork est resynchronisé.

---

## 8. Chaînes `MsgStringTable` utilisées

Accès : `MsgStringTable_GetById` @ `0x00A9ED30`. Source : `data/msgstringtable.csv`
(**id = numéro de ligne − 1**).

| id | Clé | Texte | Où |
|---|---|---|---|
| 424 | `MSI_CANT_MAKE_ITEM` | `You can't create items yet.` | modale liste vide de `0x018D` |
| 425 | `MSI_MAKE_LIST` | `Manufacturing List` | titre de la fenêtre **79** |
| 430 | `MSI_MAKE_TARGET_FAIL_MSG` | `Failed to create %s.` | `0x018F` — **composée puis jetée** |
| 431 | `MSI_MAKE_TARGET_SUCCEESS_MSG` | `Successfully created %s.` | `0x018F` — **composée puis jetée** |
| 521 | `MSI_ITEM_IDENTIFY` | `Item Appraisal` | titre écrit par le `OnCreate` partagé, puis écrasé par `"LIST"` |
| — | — | **`"LIST"`** (littéral ASCII `0x01036CD4`) | titre de la fenêtre **94** |

---

## 9. Ce que le natif ne sait pas faire

Cahier des charges du remplacement ImGui, établi à partir du RE ci-dessus.

### Ce qui manque alors que la donnée est DÉJÀ côté client

1. **Le nom ne suffit pas à identifier le produit.** ⏱ Les quatre convertisseurs
   (12114/12115/12116/12117) portent le même `Name = "Elemental Converter"` : quatre lignes
   identiques. L'élément est écrit dans le champ `Desc` de la **même** entrée
   d'`itemInfomoon.lua` (« Fire elemental », « Water elemental »…), et le nom de sprite `Res`
   diffère aussi. Rien à demander au serveur.
2. **Aucune description, aucun aperçu.** La ligne native est une icône + un nom. Ni type, ni
   poids, ni effet, alors que `ItemSkillInfo` et la DB client ont tout.
3. **Aucun id affiché.** Le seul discriminant fiable entre homonymes est invisible.
4. **Aucun stock.** « J'en ai déjà 12 » n'est jamais dit, alors que l'inventaire client le sait.

### Ce qui manque et exige un aller-retour serveur (ou une DB client à embarquer)

5. **La recette.** Ni matériau, ni quantité, ni « combien puis-je en faire ». `0x018D` a
   trois champs `material[3]` — le serveur les met à zéro et le client les jette.
6. **Le taux de réussite** (`make_per`, §7.3), et **la raison d'une liste vide** (§7.2 : ça
   peut être l'inventaire plein, pas seulement des matériaux manquants).

### Manques ergonomiques

7. **Quatre lignes visibles** (`columns × rows = 1 × 4`) dans 200 × 200 px. Une pharmacie
   d'Alchemist déroule bien davantage.
8. **Pas de recherche, pas de tri, pas de filtre** — ordre brut du paquet.
9. **Une fabrication = un lancement de compétence** (§7.5), fenêtre détruite dès l'envoi.
10. **Aucun retour écrit sur le résultat depuis la fenêtre** : `0x018F` compose la phrase et
    la jette (§3.7). Et le cas `result = 6` choisit même le mauvais libellé.
11. **Le titre dit `LIST`**, en dur, pour six métiers différents — et la compétence en cours
    n'est **pas** dans le paquet : il faut la retenir au lancement.
12. **Aucun historique** de session (tentatives, réussites, matériaux consommés).
13. **Le message de liste vide n'existe que sur `0x018D`** — et c'est une modale bloquante
    au libellé recyclé. Sur `0x01AD` / `0x025A`, la fenêtre s'ouvre simplement vide, sans un mot.
14. **La fenêtre 80 sort les matériaux de l'inventaire client** pour de bon (§6) : toute
    sortie non prévue les fait disparaître de l'affichage.

### Les deux bugs natifs à ne PAS recopier

- `ZC 0x018F` `result = 6` : succès serveur, libellé d'échec côté client (§3.7).
- Fenêtre 79 : le court-circuit de la fenêtre 80 se décide sur le **job** (Rune Knight) et
  non sur la recette (§5.3).

---

## 10. Récapitulatif des adresses

| Symbole | Adresse |
|---|---|
| `UIMakingArrowListWnd` vtable / ctor | `0x010345AC` / `0x0088E210` |
| `UIMakingArrowListWnd::OnCreate` (= `UIItemIdentifyWnd_OnCreate`) | `0x008A8470` |
| `UIMakingArrowListWnd::OnDraw` (titre `"LIST"`) | `0x008B4C30` → `0x008B4740` |
| **`UIMakingArrowListWnd::OnMsg`** | **`0x008C4A50`** |
| `UIMakingArrowListWnd` recalcul défilement | `0x008BC3F0` |
| `UIItemIdentifyWnd_ctor` | `0x0088DDE0` |
| `UIItemIdentifyWnd_GetCandidateListEntryAt` | `0x0089E6D0` |
| `Inventory_AppendNewItem` / purge de liste | `0x00799B00` / `0x00799A00` |
| `UIMakeTargetListWnd` vtable / ctor | `0x0103EC50` / `0x00965660` |
| `UIMakeTargetListWnd::OnCreate` / `OnDraw` / **`OnMsg`** | `0x009673A0` / `0x009696B0` / **`0x0096A0F0`** |
| `UIMakeTargetProcessWnd` vtable / ctor | `0x0103EED8` / `0x009656E0` |
| `UIMakeTargetProcessWnd::OnCreate` / `OnDraw` / **`OnMsg`** | `0x00967E30` / `0x009696F0` / **`0x0096A6D0`** |
| Handler `ZC 0x018D` | `0x00CA5EC5` (modale `0x00CA5EDA`, liste `0x00CA5F0F`) |
| Handler `ZC 0x018F` | `0x00CDECE0` (entrée `0x00CA61FC`) |
| Handler `ZC 0x01AD` | `0x00CA68AB` |
| Handler `ZC 0x025A` | `0x00CA6937` |
| `CMode::SendMsg` case 130 → `CZ 0x018E` | `0x00C8F6A3` |
| `CMode::SendMsg` case 153 → `CZ 0x01AE` | `0x00C8FE9A` |
| `CMode::SendMsg` case 207 → `CZ 0x025B` | `0x00C8FEDA` |
| `ItemSkillInfo_GetId` (`atoi(info + 0x2C)`) | `0x005D98A0` |
| `ItemSkillInfo_SetId` / `_ComposeDisplayName` / `_CopyFull` / `_Reset` | `0x006A6570` / `0x006A2CE0` / `0x005B72D0` / `0x006A5FF0` |
| `UIWnd_BlitIconByResName` | `0x00871040` |
| `UIItemSkillDescWnd_DrawName` | `0x008972C0` |
| `UIListBox` ctor / `GetItemCount` | `0x00835C60` / `0x0089F9B0` |
| `UIWndMgr_ShowMessageBoxModal` | `0x00A31A30` |
| `MsgStringTable_GetById` | `0x00A9ED30` |
| `g_UIWindowMgr` | `0x0131F4E8` (79 → `+0x374`, 80 → `+0x378`, 94 → `+0x398`) |
| `UIWindowMgr_MakeWindow` | `0x00A39340` (case 79 `0x00A3DA43`, case 80 `0x00A3DBC0`, case 94 `0x00A3E144`) |
| `UIWindowMgr_SaveRectAndCloseWindow` ⚠ détruit | `0x00A2E770` |
| `g_Own_JobId` | `0x015FB9C8` |

---

## 11. Points d'accroche pour le portage ImGui

Ce que le RE impose au plugin, avant même d'écrire une ligne.

1. **Observer les paquets, pas les fenêtres.** Comme pour le refine : le client **détruit**
   la fenêtre dès l'envoi (`SaveRectAndCloseWindow`), donc caler la durée de vie de l'UI
   ImGui sur celle de la native ferait disparaître l'écran au moment précis où le résultat
   arrive. Sources : `0x018D`, `0x01AD`, `0x025A` en observation pure (le handler natif doit
   continuer à tourner, sinon désactiver le plugin laisserait les métiers sans fenêtre), plus
   `0x018F` pour le résultat.
   ⚠ Les trois listes sont à **longueur variable** : n'observer que les 2 octets de
   `packetLength` et se borner à la longueur annoncée.

2. **Retenir la compétence lancée.** Ni `0x01AD` ni `0x018D` ne disent quel skill les a
   provoqués, et `0x01AD` sert quatre métiers. Le titre correct, la recette et le message de
   résultat en dépendent — il faut donc capter le lancement (`CZ 0x0072`/`0x0113` selon le
   chemin, ou le `SendMsg` de la barre de raccourcis) et le corréler.

3. **Trois protocoles de réponse, pas un.** `mk_type == 2` ⇒ `SendMsg(153, id)`.
   Sinon ⇒ `SendMsg(207, id, mk_type)`. Et pour la fenêtre 79 ⇒ `SendMsg(130, id, mats[3])`.
   L'annulation est **obligatoire** dans les trois cas (`id = -1`, ou `0` pour le 130), sinon
   le `menuskill` reste armé côté serveur.

4. **Différer tout envoi hors de la frame ImGui.** `CMode::SendMsg` est ré-entrant et la
   modale native est bloquante : les deux déclenchés depuis `OnRenderUI` provoquent un freeze
   muet (cf. `feedback_no_native_cmd_during_imgui_frame`). Passer par `OnProcessInput`.

5. **Masquer la native par `+0x28` dans le hook `MakeWindow`**, pas hors écran
   (cf. `feedback_no_offscreen_hide`) — et le faire dans le hook, sinon une frame native
   passe entre la naissance de la fenêtre et le premier `OnTick`.

6. **Ne jamais piloter les emplacements de la fenêtre 80.** Ils sortent réellement les objets
   de l'inventaire client (§6). Le plugin tient son propre modèle et n'envoie que le `0x018E`.

7. **La recette exige une décision.** Trois options, par ordre de préférence :
   opcode custom qui pousse l'entrée de `produce_db` concernée ; DB client embarquée
   synchronisée depuis `produce_db.txt` ; ou rien afficher. **Jamais** de table codée en dur
   dans le C++ (cf. `feedback_never_hardcode_use_native`).

8. **Ne pas dépendre du `Name` de la DB client pour désambiguïser.** Même une fois
   `itemInfomoon.lua` corrigé, la fenêtre doit rester lisible sur un serveur qui ne l'a pas
   fait : afficher l'id, et l'élément déduit de la description.

---

## 12. Le remplacement ImGui livré

`src/features/windows/make_item_window.{h,cc}` — membre du groupe « Interface moderne »
(`SetModernInterface`, défaut OFF). Remplace **les deux fenêtres de LISTE** (94 et 79) par
une seule fenêtre ImGui.

### Ce qui est fait

- **Source = les paquets, pas les fenêtres.** `RegisterObserveOpcode` sur `0x018D`,
  `0x01AD`, `0x025A` (2 octets : le champ `packetLength`, qui est la vraie borne), plus
  `0x018F` (résultat) et `0x0110` (refus). Le handler natif continue de tourner : désactiver
  le plugin rend les six métiers au natif.
- **Un modèle, trois protocoles.** Le paquet reçu détermine l'envoi : `0x01AD`/`mk_type==2`
  → `SendMsg(153)`, `0x025A`/`mk_type!=2` → `SendMsg(207, id, mk_type)`, `0x018D` →
  fenêtre 80. L'annulation est envoyée dans les trois cas.
- **Ce que le natif ne montrait pas** : l'**id** en colonne (le seul discriminant entre les
  quatre « Elemental Converter »), le **stock possédé** lu dans le modèle session, l'aperçu
  au survol, la description au clic droit, un filtre texte, un tri par en-tête de table
  (`Sortable` + `SortTristate`, le 3ᵉ clic rend l'ordre du paquet), et **toute la liste
  visible** au lieu de quatre lignes.
- **La RECETTE est affichée dès la sélection**, lue dans la table client déjà en mémoire
  (`MetalProcessRecipe_GetLines`, §6.1) — sans un paquet de plus. Le natif ne la montre
  qu'**après** validation, dans la fenêtre 80, et **jamais du tout** sur le chemin de la
  « LIST ». Quand l'id est absent du fichier (166 produits sur 254), la fenêtre écrit
  « Recette inconnue du client » : un blanc se lirait comme « aucun matériau requis », ce
  qui serait faux.
- **Le résultat est ÉCRIT** — le trou du §3.7. Journal horodaté, libellé exact du client
  (MsgString 430/431), coloré succès/échec.
- **Les causes réelles d'une liste vide** (§7.2) au lieu du message recyclé.
- Masquage natif par `+0x28` dans le hook `MakeWindow` ; envois différés hors frame ImGui
  par `FlushPending` ; Entrée confisquée tant que la fenêtre est ouverte.

### Décisions qui méritent d'être dites

1. **Le bug `result = 6` n'est pas recopié.** Le natif prend le libellé d'échec pour un
   succès serveur ; ici la règle est `pair = succès`, conforme à `skill_produce_mix`.
2. **La fenêtre 80 reste native, mais on ne l'ouvre plus pour rien.** Le natif décide sur le
   **job** (Rune Knight ou pas) — le mauvais critère. Le plugin suit celui de la 80
   elle-même, c'est-à-dire le **produit** (§6.2) :
   - produit dans `[994, 1000]` (fonte de métaux) → **aucun emplacement à remplir**, la 80
     ne serait qu'une confirmation dont notre fenêtre affiche déjà le contenu (nom +
     recette) : on envoie `CZ 0x018E` directement ;
   - tout le reste (les armes) → on passe la main à la 80, qui envoie elle-même et **rend
     les matériaux** sur son Annuler. Envoyer à sa place ferait perdre en silence les Star
     Crumb et les pierres élémentaires, qui n'existent que là.

   ⚠ Trois endroits dépendent de ce critère — l'envoi, l'attente d'un résultat serveur et
   la fermeture de notre fenêtre. Il est donc dit **une seule fois**
   (`ProductUsesNativeSlots`) : trois tests séparés, c'est la garantie qu'un jour l'un des
   trois sera oublié.

3. 🔴 **Les doublons du serveur sont supprimés.** ⏱ Observé en jeu : deux lignes « Steel »
   dans la liste de la Mini Furnace. Ce n'est pas un défaut du portage — le natif les
   affiche aussi. `clif_skill_produce_mix_list` itère `produce_db` **par index** mais
   interroge `skill_can_produce_mix` **par `nameid`**, laquelle re-cherche la **première**
   entrée portant ce nameid. Deux lignes pour un même produit (Steel `999` : entrées `113`
   trigger 21 et `215` trigger 26) valident donc toutes les deux, et le produit part en
   double.

   Dédoublonner ne perd rien : le client ne renvoie qu'un `nameid`, il est incapable de
   désigner l'une plutôt que l'autre, et c'est le serveur qui tranchera de toute façon. Deux
   lignes identiques n'offrent aucun choix — seulement une ambiguïté, et une collision d'ID
   ImGui (l'ID de ligne vient donc de l'**index**, pas de l'id d'objet : s'appuyer sur
   l'unicité des ids ferait dépendre le rendu d'une invariante du serveur).
4. **Pas de relance automatique**, contrairement au refine. Elle exigerait de connaître la
   compétence lancée — absente de tous les paquets, et les captures live le prouvent :
   `AC_MAKINGARROW` et `SA_CREATECON` produisent **le même `mk_type = 2`**. Livrer un
   réglage qui ne peut pas fonctionner serait pire que de ne pas le livrer.
5. **La modale « liste vide » est escamotée, et le mécanisme a été MUTUALISÉ.** Il était
   privé à `WeaponRefineWindow` ; deux `HookManager::SetHook` sur la même adresse ne se
   composent pas — le second écrase le trampoline du premier. Le détour vit désormais dans
   `src/ui/native_modal.{h,cc}` (`romodal::SwallowNext`), s'installe au premier armement, et
   les deux plugins ne font que l'armer. Le module expose aussi `romodal::AnyOpen()`, qui lit
   le compteur `g_UIWindowMgr + 0x1B4` (§3.1 bis) — savoir qu'une modale bloquante est à
   l'écran ne demande donc plus aucun hook.
   ⚠ L'armement est réservé à `0x018D` : les deux autres opcodes n'affichent aucune modale,
   armer sur eux n'ouvrirait qu'une fenêtre de tir inutile.
6. 🔴 **Les `__try` vivent dans des fonctions LIBRES, jamais dans une méthode.** C2712 se
   juge sur la fonction **entière** : `OnRecvPacket` pousse dans un `std::vector` et
   construit un `std::string` temporaire (l'argument de `Log`), dont les itérateurs ont un
   destructeur non trivial dès `_ITERATOR_DEBUG_LEVEL > 0` — le compilateur refuse alors
   tout `__try` dans la fonction, même à cent lignes de là. Les lectures brutes sont donc
   déportées (`ReadU16`, `ReadEntryIds`, `ReadMakeResult`), et la méthode lit dans un
   tableau `uint32_t[]` **avant** de remplir le modèle.
   ⚠ C'est la **deuxième fois** que ce piège se paie dans le projet (déjà
   `docs/weapon_refine_re.md`, point 6). À traiter comme une règle, pas comme un incident.
7. **Le gabarit de MsgString n'est jamais passé comme format à `snprintf`.** C'est une
   chaîne de DONNÉES (`msgstringtable.csv`), pas un littéral : un second `%s` — ou un `%n` —
   y lirait la pile. La première occurrence de `%s` est substituée à la main.

### 12.1 La relance automatique — deux mécanismes, deux prix

Le serveur n'autorise **qu'une fabrication par ouverture de liste**
(`clif_menuskill_clear` en fin de chaque parseur). Enchaîner impose donc de refaire le geste
d'ouverture. Mais ce geste n'a pas le même coût selon ce qui a ouvert la liste — et rien dans
les paquets ne dit lequel des deux c'était. D'où **deux observations** distinctes :

| Origine | Observation | Où | Ce que ça coûte de relancer |
|---|---|---|---|
| Compétence | `CMode::SendMsg` cmd `0x45` / `0x71`, `p1` = id | `ragnarok/game_mode.cc` | du SP |
| **OBJET** | envoi de **`CZ_USE_ITEM` `0x0439`** (`<index>.W <aid>.L`, 8 o) | `ragnarok/rag_connection.cc` | **un exemplaire, DÉTRUIT** |

Le second point d'observation a été choisi pour une raison précise : `0x0439` est le **seul
passage commun** à tous les chemins d'usage (double-clic inventaire, barre de raccourcis,
touche). Hooker une fenêtre en particulier en raterait deux. L'opcode y est **en clair** (le
XOR natif n'agit qu'après ce hook), et **nos propres envois contournent ce hook** (ils
passent par `SendPacketRef`) : ce qu'on y voit est donc forcément un geste du joueur.

**Le paquet ne porte que l'index**, pas l'identifiant. La résolution se fait donc **à
l'instant de l'usage** : l'objet est sur le point d'être consommé, et son index deviendra
caduc dès que l'inventaire se compactera. À la relance, on repart de l'**identifiant** et on
refait la résolution sur l'inventaire courant — jamais de l'index capté, sinon on
consommerait un objet *autre* que celui visé.

#### 🔴 `Session::GetItemInfoById` / `RagnarokClient::UseItemById` font PLANTER ce client

Première rédaction de cette fonctionnalité : elle passait par ces deux-là, qui semblaient
faits pour ça. **Crash immédiat en jeu**, et double symptôme — le clic sur l'objet ne faisait
plus rien *du tout* (l'exception coupait le hook d'envoi avant que le paquet natif ne parte),
puis usage depuis la barre de raccourcis = plantage sec.

⏱ Constat x32dbg : `EIP` dans notre DLL sur `mov eax, [esi]` avec **`esi = 0`** — parcours
d'une `std::list` dont la tête vaut 0.

Cause : `Session::item_list()` lit `item_list_` à **+0x16D8**, offset annoté
« **LIKELY**, xref pattern matches std::list usage » dans
`object_layouts/session/20250716.h`. Il est **faux**. Ce qui l'a rendu indétectable, c'est que
toute la chaîne était **morte** : `GetItemInfoById` n'était appelée que par `UseItemById`, qui
n'avait *aucun* appelant. Un offset jamais exercé n'est pas un offset vérifié — le premier
usage réel paie l'addition, des années plus tard.

Correctif : la résolution passe par le **global d'inventaire `0x015FBAB0`** (liste circulaire ;
nœud : `next+0x00`, `ItemInfo+0x08`, `amount+0x18`), déjà utilisé et vérifié en jeu par la
colonne « Possédé » de ce même plugin, et le paquet est forgé sur place. Les trois points
d'entrée cassés portent désormais un avertissement.

#### 🔴 …et `ItemInfo` est faux lui aussi : l'index est à `info+0x04`

Second piège, découvert dans la foulée : le mirroir `ragnarok/item_info.h` déclare
`item_index_` à `+0x08`, et **c'est faux**. ⏱ Une sonde sur une ligne pourtant valide (id 7144,
quantité 1) y lisait **0**.

Plutôt que d'ajouter une supposition à la précédente, l'offset est **détecté** — et le levier
est solide : le `CZ_USE_ITEM` que le joueur vient d'émettre porte un index dont on sait qu'il
désigne un objet **encore présent**. Il suffit de chercher à quel offset *une seule* ligne
d'inventaire porte cette valeur ; l'unicité écarte les coïncidences (champ à zéro, quantité
qui vaudrait l'index).

⏱ Verdict en jeu : **`node+0x0C`**, soit **`info+0x04`** — c'est-à-dire là où le mirroir
déclare `location_`. **Les deux premiers champs sont intervertis.** Trace :
`offset d'index DETECTE : node+0x0C (index 20 -> id 612)` (Mini Furnace), suivie d'une
ré-utilisation acceptée par le serveur.

Bilan de fiabilité d'`ItemInfo` sur ce client : `num_` (+0x10) et `item_name_` (+0x2C) sont
**confirmés** ; l'index est à **+0x04** ; **tout le reste n'a jamais été vérifié**. Le
détecteur est conservé en filet : sur un autre build, la résolution échouerait et il
rétablirait la valeur au premier objet utilisé, plutôt que de désactiver la relance en
silence.

**Pourquoi le réglage objet est séparé, et plafonné.** `pc_useitem` fait
`pc_delitem(sd, n, 1, 1, 0, LOG_TYPE_CONSUME)` **AVANT** d'exécuter le script de l'objet :
chaque *ouverture de liste* coûte un exemplaire, y compris si le joueur annule ensuite ou si
la fabrication rate. Ce n'est pas une raison d'interdire — vider une pile de marteaux d'un
trait est une demande légitime — mais c'en est une pour que :

- la case soit **distincte** de « relancer la compétence » (une case déjà cochée ne peut pas
  valoir permission de dépenser du stock) ;
- le compteur d'exemplaires consommés **reste à l'écran** pendant toute la chaîne, en nommant
  l'objet ;
- fermer la fenêtre **annule** une relance déjà programmée.

En revanche la chaîne n'est **pas bridée** : `makeitem_auto_reuse_max` vaut **0 = illimité par
défaut** (0–50). Une première rédaction imposait 1–30 ; l'objection est juste — consommer
toute une pile est le choix du joueur, et un plafond obligatoire ne le protège de rien qu'il
n'ait demandé. Le réglage reste **offert** pour le seul cas qu'il couvre réellement : l'opt-in
qu'on a oublié coché.

⚠ Une chaîne illimitée ne peut pas s'emballer, et c'est structurel : chaque tour exige un
**résultat serveur** (`ZC 0x018F`) pour armer le suivant. Si le serveur ignore l'usage (objet
en cooldown, condition non remplie), aucune liste n'arrive, rien ne se reprogramme — la chaîne
**cale**, elle ne tourne pas à vide.

Dans les deux cas, ce qui se relance est l'**ouverture de la liste**, jamais la fabrication :
choisir le produit et déclencher restent des clics.

Conditions d'arrêt : plafond atteint · liste vide (le serveur dit qu'il n'y a plus rien) ·
refus serveur (`ZC_ACK_TOUSESKILL`) · objet absent de l'inventaire · fermeture.

⚠ La forge par la fenêtre **80** ne déclenche aucune chaîne, et c'est correct : sur ce chemin
on n'attend pas de résultat (`awaiting_result_ = false`, c'est la 80 qui enverra), donc la
causalité qui arme la relance n'est jamais réunie.

**Défaut corrigé au passage** : `auto_chain_` n'était jamais remis à zéro — il survivait à la
fermeture de la fenêtre, si bien qu'une chaîne de 1 pouvait s'afficher « (7) ». Un drapeau
`auto_ours_`, posé à l'envoi et consommé à l'arrivée de la liste, distingue désormais une
chaîne qui se poursuit d'une liste que le joueur vient d'ouvrir lui-même.

### 12.2 « Armée » ≠ « affichée » — ce qui a supprimé le clignotement

Première rédaction : `entries_.clear()` à l'envoi, pour empêcher une seconde demande sur une
liste que le serveur venait d'oublier. La règle est bonne, l'implémentation la payait trop
cher — la table disparaissait, la fenêtre (en `AlwaysAutoResize`) se rétractait, puis tout
revenait ~500 ms plus tard. En chaîne automatique, l'écran clignotait à chaque tour et le
résultat n'avait pas le temps d'être lu.

Or **interdire l'ENVOI n'oblige pas à effacer l'AFFICHAGE**. Les deux notions sont désormais
séparées :

- `list_armed_` porte la règle serveur (`clif_menuskill_clear`) — posé à l'arrivée d'une liste,
  retiré à l'envoi. C'est lui qui garde `RequestMake` **et** `CloseAndCancel` (envoyer une
  annulation sur un `menuskill` déjà effacé serait un paquet de trop) ;
- `entries_` ne porte plus que le contenu à l'écran : la table reste affichée, **grisée à 50 %**,
  le temps de l'aller-retour. Le grisé n'est pas décoratif — sans lui, une table encore visible
  laisserait croire qu'un second envoi peut partir.

Même raisonnement pour le résultat : il n'est plus effacé par une liste que **nous** avons
provoquée (`auto_ours_`), seulement par une ouverture manuelle. Sinon un enchaînement d'une
demi-seconde effaçait « Vous avez créé X » avant qu'il soit lisible.

**Entrée maintenue enchaîne**, comme au refine — mais c'est désormais **OPT-IN et décoché par
défaut** (`makeitem_enter_key`, `refine_enter_key`). Raison : une fenêtre qui déclenche sur
Entrée doit CONFISQUER la touche tant qu'elle est ouverte, sinon la frappe ouvre aussi la
saisie du chat. Or depuis le champ Quantité, marteler Entrée n'apporte plus rien — « ×20 »
fait le même travail sans occuper le clavier — alors que perdre le chat pendant qu'on
fabrique se paie à chaque session. Le geste reste offert à qui le préfère.

> Exception, côté refine : la **modale de confirmation** garde Entrée quoi qu'il arrive.
> « Entrée = OK » y est la convention d'une modale, et elle valide une action qui peut
> DÉTRUIRE l'arme — la laisser filer au jeu ouvrirait le chat en même temps. La confiscation
> est donc réduite au seul moment où elle se justifie.

**Le bouton « Relancer »** (qui prend la place de « Fabriquer » quand la liste est consommée)
n'apparaît **que si la relance automatique est décochée**. Avec elle active, la liste revient
en quelques centaines de millisecondes : le bouton n'aurait pas le temps d'être cliqué et ne
ferait que clignoter à la place de l'autre. « Fabriquer » reste alors affiché, simplement
GRISÉ le temps du cycle. Le bouton n'existe donc que dans le cas où il est la seule issue —
sans lui, il faudrait fermer la fenêtre et repasser par la barre d'action.

Quand l'option Entrée est active, la répétition clavier fonctionne (pavé numérique compris) et
aucun garde-fou n'y est attaché — ils vivent tous dans `RequestMake` (`list_armed_`,
`awaiting_result_`, intervalle minimal de 300 ms, qui existe précisément parce que la
répétition clavier va plus vite qu'un aller-retour serveur).

⚠ À noter, parce que la comparaison avec le refine induit en erreur : **le serveur ne réarme
JAMAIS de lui-même**, ni ici ni au refine. `clif_item_refine_list` n'est émise que par le cast
de `WS_WEAPONREFINE` (`skills/merchant/upgradeweapon.cpp`), et son parseur appelle
`clif_menuskill_clear` comme les nôtres. Si le refine enchaîne, c'est qu'il combine Entrée
répétée **et** relance automatique. Entrée maintenue seule ne peut rien enchaîner — il n'y a
plus de liste sur laquelle agir.

### 12.3 🔴 La fenêtre 80 supprimée — et pourquoi c'est PLUS SÛR que le natif

Position initiale de ce document : « la 80 reste native, la piloter ferait disparaître des
objets ». **Le raisonnement était juste, la conclusion fausse** — parce qu'il portait sur le
mauvais verbe. Le danger est de la *piloter*, pas de la *remplacer*.

Ce qui a tranché, dans la décompilation de la 79 (§5.3) :

```c
CMode::SendMsg(130, vec[sel], mats);   // ItemSkillInfo mats[3]
```

Les trois matériaux voyagent **en paramètre**. La 80 ne fait que les collecter à l'écran avant
d'appeler cette même commande — elle n'est pas dans le protocole, elle est devant.

Le plugin les collecte donc lui-même et remplit les trois `ItemSkillInfo` via
`ItemSkillInfo_SetId` (`0x006A6570`, `__thiscall`, ⏱ décompilé :
`std_string_assign(this + 44, itoa(id))`). La 80 n'est plus jamais ouverte, et elle est
masquée à la création comme les deux listes — toute apparition résiduelle venait s'intercaler
entre deux fabrications d'une série.

**En quoi c'est plus sûr.** Les matériaux déposés dans la 80 sortent RÉELLEMENT de l'inventaire
client ; son Annuler les rend un par un (`Inventory_AddOrStackItem`), et toute autre sortie —
warp, fermeture par le gestionnaire, plantage — perd leur affichage jusqu'au prochain
rafraîchissement complet. En envoyant nous-mêmes, **rien ne bouge côté client** : c'est le
serveur qui consomme, comme pour tout le reste du plugin. Le risque qu'on redoutait
appartenait à la fenêtre qu'on hésitait à retirer.

**Ce que l'écran gagne**, et que le natif ne dit nulle part :

| Élément | Effet | Coût en réussite |
|---|---|---|
| Star Crumb (×1 à 3) | **+5 ATK** chacun (`card[1] = ((sc*5) << 8) + ele`) | **−15 %** chacun |
| Pierre élémentaire (la 1re seulement) | élément de l'arme | **−25 %** (`if (ele) make_per -= 2500`) |
| **Enclume** — jamais un matériau | présence en sac seulement | **+10 / +5 / +2,5 / +0 %** |

⚠ Il n'y a que **TROIS emplacements**, donc pas de combinaison à quatre objets. Les deux
extrêmes réels sont **3 Star Crumb → +15 ATK / −45 %** et **2 Star Crumb + 1 pierre → +10 ATK
et un élément / −55 %**. Le natif aligne trois cases vides et n'en dit rien ; la fenêtre affiche
le total dès qu'il y a deux contributions, pour ne pas laisser l'addition au joueur.

⚠ Deux corrections de ma première rédaction : la pierre était affichée en VERT comme un gain
pur — elle est le plus lourd des deux malus ; et l'enclume manquait entièrement. Elle
n'apparaît dans **aucune** recette et n'est **jamais consommée** : le serveur teste seulement
sa présence (`pc_search_inventory`), et ne retient que la **meilleure** (chaîne de `else if`)
— en cumuler plusieurs n'apporte rien. C'est donc une donnée que rien, dans le jeu, ne
rattache à la forge, alors qu'elle vaut jusqu'à 10 points. La fenêtre l'affiche, ou signale
son absence.

La fenêtre dit aussi que seule la **première** pierre élémentaire compte (`ele == 0` en garde)
— et que les suivantes ne sont même pas consommées, ce pour quoi les autres emplacements
n'en proposent plus dès qu'une est posée.

Deux effets de bord, tous deux souhaitables :

- la forge **hérite de la relance automatique et de la série** `×N`. Tant que la 80 emportait
  l'interaction, `awaiting_result_` restait faux et aucune chaîne n'était possible ;
- le défaut natif du §5.3 disparaît de lui-même — la 79 décidait d'ouvrir la 80 selon le
  **job** (variantes de Rune Knight), pas selon ce que la recette accepte. Le plugin, lui, se
  détermine sur le PRODUIT (`(unsigned)(produit - 994) > 6`, le propre test de la 80).

### 12.4 🔴 Le serveur NE RÉPOND PAS sur la plupart des compétences

⏱ Constaté en jeu : Elemental Converter fabriqué (le chat écrit *« You got Stingor's Flame
Elemental Converter (1) »*), et la fenêtre reste sur « en attente du serveur… ». Le natif ne
l'a jamais montré parce qu'il referme sa fenêtre à l'envoi.

Cause, dans le `switch` de succès de `skill_produce_mix` (`skill.cpp:13934`) :

```c
switch (skill_id) {
  case RK_RUNEMASTERY:                                    clif_produceeffect(sd, 4, nameid); break;
  case GN_MIX_COOKING: case GN_MAKEBOMB: case GN_S_PHARMACY: clif_produceeffect(sd, 6, nameid); break;
  case MT_M_MACHINE:                                      clif_produceeffect(sd, 0, nameid); break;
  case BO_BIONIC_PHARMACY:                                clif_produceeffect(sd, 2, nameid); break;
}
```

**`SA_CREATECON`, `AM_PHARMACY`, la forge (`BS_*`) et les flèches n'y sont PAS.** L'objet est
créé (`pc_additem`) et **aucun `ZC 0x018F` ne part**. Seule la mise à jour d'inventaire arrive.

Second silence, distinct : `skill_produce_mix` fait `return false` **sans paquet** quand
`skill_can_produce_mix` échoue à la revalidation (`skill.cpp:13383`), et
`clif_parse_SelectArrow` ignore ce retour. Un refus est donc muet lui aussi.

**Réponse du plugin — constater plutôt qu'attendre.** Le stock du produit est relevé AVANT
l'envoi ; passé 700 ms sans paquet, on le relit. S'il a augmenté, c'est un succès : on l'écrit
(en disant que l'information vient de l'inventaire, pas du serveur) et **on arme la relance**.
C'est ce qui rend la chaîne et la série `×N` possibles sur ces compétences — elles s'armaient
jusqu'ici sur un paquet qui n'arrive jamais. Au-delà de 6 s sans rien, la demande est
abandonnée avec un message explicite plutôt qu'une attente éternelle.

**Bonus maison, et pourquoi l'inventaire est la seule source.** Ce fork ajoute un tirage
aléatoire (`// [Stingor]`, `skill.cpp:13916`) qui peut rendre **jusqu'à 5 exemplaires** d'un
coup sur `AM_PHARMACY`, `SA_CREATECON`, `AL_HOLYWATER` et `ASC_CDP`. Comme aucun paquet de
résultat ne part sur ces compétences, l'écart de stock est la **seule** façon de le rapporter
— la fenêtre écrit donc « ×3 ! » quand le tirage a été généreux, ce que ni le natif ni le
protocole ne savent dire.

⚠ Quatre pièges payés en écrivant cette sonde :
- **l'id envoyé ne désigne pas toujours le PRODUIT.** Pharmacie, convertisseurs et forge
  envoient le produit (stock en hausse) ; la fabrication de **flèches** envoie le **MATÉRIAU** —
  `skill_arrow_create` fait `pc_delitem` dessus et le rendement vit dans `skill_arrow_db`, donc
  ni dans la demande ni dans la réponse. Guetter une seule hausse concluait « le serveur n'a
  pas répondu » sur un craft de flèches réussi. Ce qui prouve que le serveur a agi, c'est le
  **changement**, pas son signe. En baisse, on écrit « transformé » sans inventer un rendement
  qu'on ne connaît pas ;
- **le décompte de la série doit être fait ICI AUSSI.** Il ne vivait que dans le handler de
  `0x018F` — c'est-à-dire dans un paquet qui n'arrive jamais sur ces compétences. `batch_left_`
  restait donc à sa valeur de départ et la chaîne ne s'arrêtait plus, même à une quantité de 1.
  ⚠ La correction a d'abord DÉBORDÉ : on avait aussi barré la relance quand la série était
  finie, ce qui faisait « 2 fabrications » = **trois clics** (Fabriquer, Relancer, Fabriquer).
  Les deux réglages sont **orthogonaux** et ne doivent pas être fusionnés — « relancer
  automatiquement » ROUVRE la liste, la QUANTITÉ dit combien de fabrications partent seules ;
- **calculer l'écart AVANT de remettre la référence à −1**, sinon le message annonce le stock
  total plus un (⏱ « +12 » pour un objet produit à 11 exemplaires) ;
- **`SendRecast` doit remettre `skill_cast_at_` à l'heure lui-même.** Notre relance part
  d'`OnProcessInput`, donc en appel IMBRIQUÉ de `CMode::SendMsg` — or le hook n'observe qu'au
  niveau le plus externe (`g_send_msg_depth == 1`). Sans ce rafraîchissement, l'horodatage
  vieillissait pendant toute la chaîne et, passé `kSkillCastWindowMs`, la liste suivante était
  prise pour un script d'OBJET (⏱ « from_item=1 … skill=1007 il y a 4719 ms ») : la relance
  s'arrêtait alors sur « objet d'origine inconnu ». Le pendant existait déjà côté objet
  (`SendReuseItem` / `item_use_at_`) — c'est la symétrie qui manquait.

> **Le correctif SERVEUR reste souhaitable** et tient en quelques lignes : ajouter les
> compétences manquantes au `switch`, ou un `default: clif_produceeffect(sd, 0, nameid);`.
> Le client saurait alors distinguer un échec d'un succès au lieu de l'inférer — et surtout,
> un ÉCHEC (matériaux consommés, rien produit) resterait aujourd'hui indiscernable d'un refus
> muet, faute de paquet dans les deux cas.

### 12.5 🔴 Une fenêtre native MASQUÉE garde le CLAVIER — et fabrique dans notre dos

Symptôme rapporté : plugin actif, fenêtre ImGui ouverte, réglage « Entrée lance la
fabrication » **décoché**. Le joueur appuie sur Entrée → le chat ne s'ouvre pas, mais
« *Failed to create Iron.* » s'affiche… alors que la table ImGui montrait *Star Crumb*.
Ensuite plus rien ne marche : notre envoi suivant reste sur « Le serveur n'a pas répondu ».

La chaîne, remontée dans le binaire :

| Étape | Adresse | Ce qui se passe |
|---|---|---|
| `UIWindowMgr_OnKeyDown` | `0x00A471E0` | `if (key == 13 \|\| key == 32)` — **Entrée ET Espace** |
| `UIWindowMgr_ActivateDefaultButton` | `0x00A2E270` | cascade de priorité codée en dur sur des champs du mgr → `OnMsg(msg = 0)` |
| `UIWindow_OnMsg_Default` | `0x008841D0` | `msg 0` → `OnMsg(6, this+0x8C)` = **clic réel sur le bouton par défaut** (`msg 1` → `+0x90`, Annuler) |
| `UIMakeTargetListWnd::OnMsg` case 6 / 184 | `0x0096A0F0` | `SendMsg(130, listbox_natif[sel])` → CZ 0x018E |

**Aucune de ces étapes ne consulte la visibilité.** Le prédicat que le gestionnaire
interroge avant d'envoyer la touche est `vt+8`, et `vt+8` est un `return 1` **en dur**
(`Stub_ReturnTrue_Folded` `0x005A5D90`, partagé par ICF entre toutes les `UIWindow`).
`+0x28` masque le **rendu**, rien d'autre — exactement la même leçon que « une fenêtre
cachée reste une cible de drop ».

Conséquences pratiques :

1. Le OK natif fabrique **sa** sélection, c'est-à-dire la ligne 0 de *sa* listbox — l'ordre
   du paquet, pas notre tri. D'où l'*Iron* pendant qu'on affichait *Star Crumb*.
2. Il consomme les matériaux **et** le serveur fait son `clif_menuskill_clear`. Notre envoi
   suivant tombe donc dans le trou décrit en §12.4 : refusé, sans aucun paquet.
3. Le réglage « Entrée lance la fabrication » n'en protégeait pas — au contraire : **décoché**,
   il laisse volontairement filer la touche vers le jeu, qui la donne à cette fenêtre.

**Correctif retenu : la fenêtre ne NAÎT plus** (remplacement d'opcode, plus bas), et
celle qui existerait quand même est **RENDUE par son bouton Annuler** —
`CancelNativeIfClass` dans `FlushPending`, `OnMsg(6, 185)`.

> ⏱ **Deux rédactions fausses avant celle-là**, toutes deux constatées en jeu :
>
> 1. **Fermer par `UIWindowMgr::Close`.** Il n'envoie RIEN, donc le `menuskill` restait
>    armé. Et pour la Mini Furnace `menuskill_id == -1`, or
>    `clif_skill_produce_mix_list` commence par `if (menuskill_id == skill_id) return;`
>    — la compétence ne renvoyait plus jamais rien, définitivement. Le raisonnement
>    d'origine (« Annuler envoie un paquet, donc évitons-le ») était exactement à
>    l'envers : ce paquet EST le désarmement.
> 2. **Armer le pilotage depuis `OnTick`.** Une course perdue d'avance : `OnTick` est
>    limité à 100 ms, `FlushPending` tourne à chaque frame. La fenêtre était détruite
>    avant que le tick ne l'ait vue vivante. Le remède a été de supprimer le drapeau :
>    `FlushPending` annule toute session native vivante, à chaque frame, sans état.
>
> Corollaire : **plus de masquage de rattrapage** dans `OnTick`. Une fenêtre native
> visible une frame de trop est un défaut cosmétique ; un fantôme masqué tenant une
> session armée bloque le personnage. Des deux échecs possibles, on choisit le premier.

L'Annuler natif est d'ailleurs supérieur sur trois points : il connaît le protocole de
réponse que nous ignorons (130 avec `itemId = 0`, ou 153/207 avec `-1`), il ferme la
fenêtre de toute façon, et pour la **80** il RE-CRÉDITE les matériaux déjà posés
(`Inventory_AddOrStackItem`) — ce qu'aucune fermeture ne fait. D'où l'ordre : 80 d'abord.

Points non négociables qui restent :

- **Jamais à la création** : l'appelant natif se sert encore du pointeur que `MakeWindow`
  vient de lui rendre (`OnMsg(0x4B)` puis un `OnMsg(0x1F)` par entrée, puis `0x22`) —
  ce serait un *use-after-free*.
- **Nos envois ne dépendent d'aucune fenêtre** : les commandes 130/153/207 de
  `CMode::SendMsg` ne regardent que leurs paramètres, donc la disparition de la native
  ne gêne rien.
- **On ne désarme PAS sur une perte de focus** (piste écartée) : cette liste est un
  `menuskill` que le joueur a payé d'un consommable (Mini Furnace, kit de conversion).
  L'annuler parce qu'il a cliqué sur une autre fenêtre ImGui lui brûlerait un objet pour
  un clic de souris. La bascule de l'INTERRUPTEUR, elle, désarme — c'est un geste
  explicite, et le laisser sans désarmement bloquait le personnage (§12.6).
- La **80 s'annule comme les autres**, et en PREMIER : c'est la seule qui peut détenir des
  matériaux, et son Annuler les rend un par un (§6). C'est même l'argument décisif en
  faveur de l'Annuler contre la fermeture.

Le **refine (111)** est frappé par le même trou, en pire : son `+0x8C` vaut `184` (documenté
dans `weapon_refine_re.md`), donc Entrée y déclenche `SendMsg(182)` — un refine réel sur
l'arme choisie par le **client**, qu'un échec détruit. Là, on ne peut pas détruire la native
(elle porte la position d'ouverture et la présence de session), donc la touche est
confisquée **quel que soit le réglage**, Espace comprise.

> **La sortie définitive est le chat en ImGui.** Tant que la saisie appartient au natif, on
> arbitre entre « rendre Entrée au chat » et « ne pas la laisser piloter une fenêtre
> fantôme ». Une fois le chat passé en ImGui (`project_chatbox_imgui_conversion`), Entrée
> peut être bloquée **globalement** pour le jeu et distribuée côté ImGui : plus d'arbitrage,
> plus de trou. Les confiscations ci-dessus sont des rustines en attendant ça.

### Réglages persistants

`makeitem_imgui` (basculé en groupe), `makeitem_show_owned`, `makeitem_filter`,
`makeitem_desc_tooltip`, `makeitem_history` (OFF), `makeitem_log_time`,
`makeitem_auto_recast` (OFF), **`makeitem_auto_reuse_item`** (OFF) /
**`makeitem_auto_reuse_max`** (5),
`makeitem_pos_x` / `makeitem_pos_y` (`INT_MIN` = jamais posée ; écrites à la FERMETURE).

### Reste à faire

- ~~**La forge Blacksmith** / la fenêtre 80~~ — **FAIT**, cf. §12.3.
- **La CUISINE — aucun code neuf requis, reste à vérifier en jeu.** Table complète des appels
  à `clif_cooking_list(sd, trigger, skill_id, qty, list_type)` :

  | Appelant | trigger | skill_id | qty | `list_type` = `mk_type` |
  |---|---|---|---|---|
  | `cooking` (**script d'objet**, script.cpp:11134) | arg | **AM_PHARMACY** | 1 | **1** |
  | mixcooking | 27 | GN_MIX_COOKING | 1 ou **10** | **6** |
  | specialpharmacy | 29 | GN_S_PHARMACY | 1 | **6** |
  | createbomb | 28 | GN_MAKEBOMB | 1 ou **10** | 5 |
  | manufacturemachine | 31 | MT_M_MACHINE | 1 | 7 |
  | bionicpharmacy | 32 | BO_BIONIC_PHARMACY | 1 | 8 |

  Le chemin est celui déjà éprouvé : `ZC 0x025A` → fenêtre **94** → `Proto::kMaking` →
  `SendMsg(207, itemId, mk_type)`, `mk_type` relayé tel quel. Les recettes sont dans
  `produce_db`, donc déjà couvertes par le fichier régénéré (les 99 lignes à quantité 0 sont
  les livres de cuisine). Quatre faits à garder en tête :
  - la cuisine vient d'un **script d'OBJET** (l'ustensile) et pose `menuskill_id = AM_PHARMACY` :
    c'est donc la relance par objet qui s'applique, et chaque relance consomme un ustensile ;
  - **`mixcooking` ET `specialpharmacy` émettent tous deux `6`** — le commentaire de
    `clif_cooking_list` qui annonce `4` pour GN_MIX_COOKING est périmé, et
    `clif_parse_Cooking` teste bien `p->type == 6`. Ne pas « corriger » d'après la table ;
  - **une requête peut fabriquer `menuskill_val2` exemplaires d'un coup** (10 pour les bombes
    et le mix-cooking au-delà du niveau 1). Un rendement > 1 n'est donc pas toujours le tirage
    aléatoire maison : le message énonce le nombre sans l'interpréter ;
  - liste vide : au-delà de PACKETVER 20090922 le serveur n'envoie **pas** de liste mais
    `clif_msg_skill(MSI_SKILL_INVENTORY_KINDCNT_OVER)`. Notre fenêtre ne s'ouvrira donc pas
    du tout, ce qui est le bon comportement.

  ⚠ `clif_cooking_list` commence par `if (sd.menuskill_id == skill_id) return;` — « avoid
  resending the menu ». Une relance ne peut donc aboutir que si le `menuskill` a été effacé
  entre-temps, ce que fait le parseur après chaque fabrication comme après une annulation.
  Un chemin qui oublierait d'envoyer sa réponse resterait muet jusqu'au prochain nettoyage.
- La recette et le taux de réussite : opcode custom (§9).
