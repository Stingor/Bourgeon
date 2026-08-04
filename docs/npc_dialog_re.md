# Dialogue NPC / scripts serveur — rétro-ingénierie complète

> Client Ragnarok (`Moonlight-Destiny.exe`). Toutes les adresses sont des VA de l'image
> chargée (Ghidra == x32dbg). RE réalisée 2026-07-10 (Ghidra + x32dbg live).
> Fonctions renommées et commentées dans le projet Ghidra (dépôt durable).

> ⚠ **Statut du chemin natif décrit ici (2026-08-01).** Quand l'interface moderne est
> active, les neuf handlers de dialogue (`0x00B4/B5/B6/B7`, `0x0142`, `0x01D4`,
> `0x08D6`, `0x0972/0973`) **ne tournent plus** : `NpcDialogWindow` a pris leur place
> dans la table de dispatch (`RegisterReplaceOpcode`, révocable par l'interrupteur).
> Les cinq fenêtres — `0x10`, `0x11`, `0x38`, `0x64`, `0xE2` — ne naissent donc plus,
> au lieu de naître puis d'être masquées : masquée, une native garde le clavier et
> son bouton par défaut répond à Entrée. Ce document reste la référence du
> comportement natif — c'est lui qui repasse aux commandes interrupteur éteint, et
> c'est de lui que le plugin tient les deux écritures qu'il reprend à son compte
> (`CGameMode+0x24C` dialogue actif, `+0x2DC` GID courant, §2).
>
> Le remplacement d'un handler à longueur **fixe** (`0x00B5`, `0x0142`…) a demandé
> d'apprendre au reader-hook la longueur réelle des paquets : il la demande désormais
> au résolveur du client, `PacketLenTable_Lookup` @ `0x00AA7B00` sur la table
> `0x0159D68C` — celui-là même que consulte `RecvBuffer_ReadPacket` @ `0x00C147D0`
> (`out[0] == 1` → longueur fixe `out[1]`, sinon variable).

Ce document couvre **toute la chaîne d'interaction NPC** : réception des scripts envoyés par
le serveur, la fenêtre de dialogue, les boutons **Next / Close**, le **menu**, les
**prompts nombre / texte**, la **gestion des balises**, la **coloration des textes**, les
**ctor/dtor**, et **l'arbre** de widgets (racines → feuilles).

---

## 1. Vue d'ensemble — le flux « script serveur → écran »

Un « script NPC » n'est PAS un blob de bytecode côté client : le serveur pilote la
conversation **paquet par paquet**. Chaque `next`, `menu`, `input`, `mes`, `close` du
script serveur (rAthena/moonlight) devient un paquet `ZC_*` que le client transforme en
opération d'UI. Inversement, chaque clic de l'utilisateur renvoie un paquet `CZ_*`.

```
 SERVEUR (script)         PAQUET ZC              CLIENT (UI)
 ────────────────         ─────────              ───────────
 mes "..."         →  ZC_SAY_DIALOG 0x00B4  →  UISayDialogWnd(0x10)->OnMsg(0x1A) AddLine
 next;             →  ZC_WAIT_DIALOG 0x00B5 →  UISayDialogWnd->OnMsg(0x19)  bouton [Next]
 close;/close2;    →  ZC_CLOSE_DIALOG 0x00B6→  UISayDialogWnd->OnMsg(0x3F)  bouton [Close]
 menu/select       →  ZC_MENU_LIST 0x00B7   →  UIChoose3Wnd(0x11) liste cliquable
 input .@n;        →  ZC_OPEN_EDITDLG 0x0142 → UINpcEditDialogWnd(0x38)  prompt NOMBRE
 input .@s$;       →  ZC_OPEN_EDITDLGSTR 0x1D4→UINpcTextEditDialogWnd(0x64) prompt TEXTE
 say/wait « 2 »   →  ZC_SAY_DIALOG2/WAIT_DIALOG2 0x0972/0973 → UISayDialogWnd(0xE2) secondaire
 align/size/pos   →  ZC_DIALOG_TEXT_ALIGN/SIZE/POS/POS2 0x0BA1/0BA2/0BA3/0BB5 → agit sur 0x10 (primaire)

 CLIENT clic [Next]  →  SendMsg(sel 0x16) →  CZ_REQ_NEXT_SCRIPT  0x00B9   (bouton cmd 0xDA)
 CLIENT choix menu   →  SendMsg(sel 0x17) →  CZ_CHOOSE_MENU      0x00B8 (choix 1..n)
 CLIENT annule menu  →  SendMsg(sel 0x28) →  CZ_CHOOSE_MENU      0x00B8 (0xFF, indirect)
 CLIENT [OK] nombre  →  SendMsg(sel 0x63) →  CZ_INPUT_EDITDLG    0x0143
 CLIENT [OK] texte   →  SendMsg(sel ?)    →  CZ_INPUT_EDITDLGSTR 0x01D5   (non vérifié)
 CLIENT [Close]      →  SendMsg(sel 0x59) →  CZ_CLOSE_DIALOG     0x0146   (bouton cmd 0xC9)

 ⚠ « sel » = SÉLECTEUR SendMsg (index de la jump-table, = *(fenêtre+0xC8)). Le cmd id du
   bouton (0xDA Next / 0xC9 Close) N'EST PAS le sélecteur — il est passé en arg3. Voir §8.
```

### Buffer de staging du paquet — `g_NpcDialogPacketBuf` @ `0x015E8198`

Le dispatcher recv recopie le paquet `ZC_SAY_DIALOG`/`MENU_LIST` courant ici. Layout =
le paquet brut :

| offset | champ |
|---|---|
| `0x015E8198` +0 | opcode (word) |
| `0x015E819A` +2 | **longueur** (word) |
| `0x015E819C` +4 | **GID du NPC** (dword) |
| `0x015E81A0` +8 | **texte** (`len - 8` octets) |

Le handler `ZC_SAY_DIALOG` (recv @ `0x00CA06B6`, dans le dispatcher détourné, non
fonctionnalisé par Ghidra) fait, en live :

```asm
CA06B6  mov  [edi+0x24C], 1              ; GameMode+0x24C = flag "dialogue actif"
CA06C0  mov  ecx, 0x0131F4E8             ; g_UIWindowMgr
CA06C5  mov  eax, [0x015E819C]           ; GID
CA06CB  mov  [edi+0x2DC], eax            ; GameMode+0x2DC = NPC GID courant
CA06D0  movzx esi, word [0x015E819A]     ; longueur
CA06D7  push 0x10                        ; window id
CA06DC  call 0x00A39340                  ; UIWindowMgr::MakeWindow(0x10)
CA06E8  mov  edi, eax                    ; edi = UISayDialogWnd
        ... sprintf du texte reçu -> OnMsg(0x1A) = AddLine
```

Champs `CGameMode` pertinents (`edi` = mode zone actif) :

| offset | rôle |
|---|---|
| `+0x24C` | flag **dialogue NPC actif** (1 à l'ouverture, remis 0 par le dtor/close) |
| `+0x2DC` | **GID** du NPC en cours de conversation |
| `+0x1F8` | pointeur singleton `UIChoose3Wnd` (menu, id 0x11) |
| `+0x314` | pointeur singleton `UINpcEditDialogWnd` (input nombre, id 0x38) |
| `+0x318` | pointeur singleton `UINpcTextEditDialogWnd` (input texte, id 0x64) |

---

## 2. La famille de fenêtres NPC & l'arbre de classes (racines)

Le **nom de classe exact** est lu via le RTTI MSVC (TypeDescriptor à `vtable-4 → COL+0x0C`).

| id | classe (RTTI) | rôle | vtable |
|---|---|---|---|
| **0x10** | `UISayDialogWnd` | dialogue texte principal (Next/Close) | `0x01033094` |
| **0xE2** | `UISayDialogWnd` | dialogue **secondaire** (nouveau style, repositionnable) | `0x01033094` |
| **0x11** | `UIChoose3Wnd` | **menu** de choix | `0x0104ADE0` |
| **0x38** | `UINpcEditDialogWnd` | prompt **nombre** | `0x01032824` |
| **0x64** | `UINpcTextEditDialogWnd` | prompt **texte** | `0x010328FC` |

### Hiérarchie de classes (l'arbre RTTI, racine → feuille)

Extrait du `_RTTIClassHierarchyDescriptor` de `UISayDialogWnd` (CHD @ `0x010C3B08`,
`numBaseClasses = 4`, base-array @ `0x010C3B18`) :

```
UIRPData  ──►  UIWindow  ──►  UIFrameWnd  ──►  UISayDialogWnd
 (racine)      (fenêtre       (fenêtre à       (feuille)
               composite)     cadre/bordure)
```

- `UIRPData` = base de données/rect (racine commune).
- `UIWindow` = fenêtre composite (le socle : géométrie, enfants, `AddChild`).
- `UIFrameWnd` = fenêtre à cadre (fond `dialog_bg.bmp` + bordure).
- `UISayDialogWnd` = spécialisation dialogue NPC.

Ressources associées (strings) : `\basic_interface\dialog_bg.bmp`, `dialog_mid.bmp` ;
clés registre de persistance `SAYDIALOGWNDINFO.X/Y/W/H`. Boutons = `btn_next.bmp` /
`btn_close.bmp` (+ suffixes `_a` survol, `_b` pressé). *(Les bitmaps `dialog_btn0/1/2`
appartiennent au chat, PAS au dialogue NPC.)*

---

## 3. `UISayDialogWnd` — le dialogue texte (id 0x10 / 0xE2)

### 3.1 Constructeur — `UISayDialogWnd_ctor` @ `0x0088EB50`

```c
UIWindow_composite_ctor(this, 0);
this->vtbl = &vtbl_UISayDialogWnd;          // 0x01033094
// deux listes circulaires doublement chainees (sentinelle allouee)
this[+0xB8] = new_sentinel(); this[+0xBC] = 0;   // liste #1 : boutons Next/Close
this[+0xC0] = new_sentinel(); this[+0xC4] = 0;   // liste #2 (2e groupe)
this[+0xB4] = 0;                                  // UIRichTextCtrl (posé par OnCreate)
this[+0xD0] = 0;
```

Champs d'instance :

| offset | champ |
|---|---|
| `+0xB4` | **`UIRichTextCtrl`** enfant (la zone de texte) |
| `+0xB8` / `+0xBC` | liste doublement chaînée des `UIBitmapButton` / son compteur |
| `+0xC0` / `+0xC4` | 2ᵉ liste / compteur |
| `+0xC8` | **sélecteur `CMode::SendMsg`** (0x16 contexte Next / 0x59 contexte Close) — passé en arg1 du dispatch. ⚠ PAS le cmd id du bouton |
| `+0xCC` | contexte de lien courant |
| `+0xD0` | pointeur données menu |
| `+0xD4` | flag « input/effacer avant prochain AddLine » |
| `+0x8C` | largeur de layout (0xDA avec Next, 0xC9 avec Close, 599 défaut) |

### 3.2 Destructeurs — `UISayDialogWnd_dtor` @ `0x00891700`, `_scalar_dtor` @ `0x00893850`

```c
this->vtbl = &vtbl_UISayDialogWnd;
GameMode_GetActive(0x1213338)->[+0x24C] = 0;   // éteint le flag "dialogue actif"
free_list(this[+0xC0]);  free_list(this[+0xB8]);  // vide + libère les 2 listes de boutons
UIWindow_composite_dtor(this);
// scalar_dtor : if (flag & 1) game_free(this);
```

### 3.3 OnCreate — `UISayDialogWnd_OnCreate` @ `0x008AE2E0` (vtable +0x3C)

**Ne crée qu'UN enfant** : le contrôle rich-text. Les boutons sont ajoutés à la demande
par les handlers recv (voir §3.4).

```c
rt = UIRichTextCtrl_ctor(new(0xCC));       // 0x00806900
this[+0xB4] = rt;
UIWindow_SetSize(rt, W-0x14, H-0x28);      // marges 20 large / 40 haut
rt[+0x8C] = 0xFFF2F2F2;                     // couleur de fond claire
rt[+0x90] = W-0x41;                         // largeur de wrap
rt->SetPos(10, 10);
AddChild(this, rt);                         // FUN_00a1b780
this[+0x8C] = 599;
```

### 3.4 OnMsg — `UISayDialogWnd_OnMsg` @ `0x008C74C0` (vtable +0x94)

C'est le cœur. Table des messages internes (`param_2` = msg id) :

| msg | rôle |
|---|---|
| **6** | **clic bouton** : `Replay_RecordUIEvent` (0x00A24610 — enregistreur replay/démo, **pas** un hit-test ; actif seulement en mode enregistrement) puis `CMode::SendMsg(sel=*(this+0xC8), *(this+0xCC), cmd, 0,0)`. Le sélecteur est 0x16 (Next) ou 0x59 (Close) ; le cmd bouton (0xDA/0xC9) est passé en arg3. Next : + throttle 300 ms + `+0xD4=1` + `OnMsg(0x20)`. Exception : cmd `0x257` absorbé sans envoi. |
| **0x1A** | **AddLine texte** : si flag `+0xD4` set, vide d'abord le richtext ; puis `richtext->OnMsg(0x1D, texte, len)` |
| **0x19** | **ajoute bouton [Next]** : largeur 0xDA, cmd `0xDA`, nom `btn_next` → délègue msg 0x55 |
| **0x3F** | **ajoute bouton [Close]** : largeur 0xC9, cmd `0xC9`, nom `btn_close` → délègue msg 0x55 |
| **0x55** | crée un `UIBitmapButton` 3 états (`name.bmp` / `name_a.bmp` survol / `name_b.bmp` pressé), le positionne à droite/bas, fixe son cmd id, `AddChild`, l'ajoute à la liste `+0xB8` |
| **0x56** | **retire** le bouton de cmd id donné (déplace hors écran, `UIWindowMgr` remove, unlink, décrémente `+0xBC`) |
| **0x20** | **reset** : largeur 599, retire le bouton Next (délègue msg 0x56 cmd 0xDA) |
| **0x22** | positionne le menu (`+0xD0` = data, déplace la fenêtre) |
| **0x4B** | efface le contenu du richtext (`richtext->[+0xD4]()`) |
| **0x50** | fixe le cmd id contexte (`+0xC8`) |
| **0x7B** | **parse blob composite** (dialogue nouveau style) : marqueurs `0x57E4`(début)/`0x57E5`(fin)/`0x57E7`(texte→AddLine)/`0x57E8`(métadonnée→`+0xD4`) |
| défaut | `UIWindow_OnMsg_Default` |

> Si `DAT_0131F940 != 0` (input global bloqué), OnMsg court-circuite tout et affiche un
> message via `UIWindowMgr_ChatAction`.

### 3.5 Fermeture — `NpcDialog_OnRecv_ZC_CLOSE_DIALOG` @ `0x00CBAF10`

`ZC_CLOSE_DIALOG` (recv @ `0x00CA0964`) appelle cette routine avec `&g_NpcDialogPacketBuf` :

```c
if (g_UISayDialogWnd_primary_0x10 == 0) {          // aucun dialogue ouvert
    GameMode+0x24C = 0;  GameMode+0x2DC = GID;
    SaveWindowRect(0x10); SaveWindowRect(0x11); SaveWindowRect(0xE2);
} else {                                            // dialogue ouvert : montrer [Close]
    primary->OnMsg(0x20);  primary->OnMsg(0x3F);    // reset puis bouton Close
}
if (g_UISayDialogWnd_secondary_0xE2 != 0) {
    secondary->OnMsg(0x20); secondary->OnMsg(0x3F);
}
```

`ZC_CLOSE_DIALOG` **n'efface pas** le dialogue : il remplace le bouton [Next] par [Close].
Le vrai `game_free` a lieu quand l'utilisateur clique [Close] (→ `CZ_CLOSE_DIALOG 0x0146`,
puis dtor). Globals : `g_UISayDialogWnd_primary_0x10` @ `0x0131F6D8`,
`g_UISayDialogWnd_secondary_0xE2` @ `0x0131F6DC`.

---

## 4. Moteur de texte — `UIRichTextCtrl` + `TextLayout`

⚠ **Deux** contrôles rich-text distincts existent :
- `UIRichTextBox` (ctor `0x008373C0`, OnMsg `0x008620E0`) — utilisé par le **chat**/desc,
  balises `<ITEML>/<ITEM>/<NAVI>/<QUEST>/<URL>` → boutons-liens enfants.
- **`UIRichTextCtrl`** (ctor `0x00806900`) — **c'est celui du dialogue NPC**. Plus simple,
  rend via le lexer `TextLayout`.

### 4.1 `UIRichTextCtrl`

| fonction | adresse | rôle |
|---|---|---|
| `_ctor` | `0x00806900` | objet 0xCC octets |
| `_WndProc` | `0x008079C0` | OnMsg : 0x1D=AddLine, 0x3C=Relayout, 0x12/0x13=scroll page, 0x62=clic lien |
| `_AddLine` | `0x00806FC0` | ajoute une ligne : construit un `TextLayout`, émet les tokens |
| `_Relayout` | `0x00807D20` | recalcule le layout |
| `_ApplyScroll` | `0x00807770` | applique le scroll |

`UIRichTextCtrl_AddLine` : stocke la ligne dans un `std::vector` interne (`+0xC0/+0xC4`,
records de 0x18 o), instancie un **`TextLayout`** avec l'état de style courant
(`+0xA0`=couleur 64 bits, `+0xA8/+0xAC/+0xB0/+0xB4`=flags), puis pour chaque token produit
→ le positionne et fait **`AddChild(this, token)`** (`FUN_00a1b780`). Les tokens (runs de
texte colorés, icônes item, émotes) sont donc des **enfants** = les **feuilles** de l'arbre.

`_WndProc` case `0x62` (clic sur un lien) route par cmd id : `0x1B5` URL (ShellExecute),
`0x1B6` NAVI (route), `0x1D0` item (ouvre desc), `0x205`, `0x21B`.

### 4.2 `TextLayout` — le lexer de balises & couleurs

`TextLayout_ctor` @ `0x007FF3E0`. Le tokenizer maître = **`TextLayout_LexNextToken`
@ `0x008057F0`**.

**Coloration `^RRGGBB`** (dans `LexNextToken`) : si le caractère est `^`, tente de lire
**6 chiffres hexadécimaux** (`strtol` base 16). Si exactement 6 :
```c
uVar2 = 0xRRGGBB;
color = ((uVar2 & 0xFF)<<8 | (uVar2>>8 & 0xFF))<<8 | (uVar2>>0x10 & 0xFF);  // -> 0xBBGGRR
this[+0x20] = this[+0x24] = color;    // couleur de dessin courante (ordre BGR D3D)
this[+0x40] = 1;                      // flag "couleur changée"
pos += 7;                             // consomme '^' + 6 hex
```
`^000000` remet noir, etc. — codes couleur RO standard des scripts (`^FF0000`, `^0000FF`…).

**Chaîne de sous-lexers** (le premier qui avance la position gagne) :

| ordre | fonction | balise |
|---|---|---|
| 1 | `TextLayout_LexNItemIDName` `0x00804FF0` | **`^nItemID^<chiffres>`** (forme caret, PAS `<nItemID>`) → substitue le **nom de base** de l'item (texte simple, ni icône ni lien) |
| 2 | `TextLayout_LexEmoticon` `0x008044E0` | `^e[..]` émote → token 0x236 |
| 3 | `TextLayout_LexItemIcon` `0x00804E00` | `^i[..]` icône item → token 0x241 |
| 4 | `TextLayout_LexBracketTag` `0x00804B30` | `<..>` (liens : tokens 0x1B5..0x233) |
| 5 | `TextLayout_LexFontTag` `0x00804750` | `<FONT>`, `<B>`, `<I>` |
| — | `TextLayout_LexWordOrBreak` `0x00805500` | mot/espace/retour (word-wrap) |

**`TextLayout_LexFontTag`** gère la **pile de style** (`+0x34/+0x38`) :
- `<FONT>…</FONT>` : push/pop couleur + géométrie de style.
- `<B>`/`</B>` : compteur gras `+0x10`.
- `<I>`/`</I>` : compteur italique `+0x14`.

Émission des runs : `TextLayout_EmitStyledTextRun` `0x00800B70` (texte coloré),
`_EmitPlainTextRun` `0x00801180`, `_EmitItemIconToken` `0x00800F60`,
`_EmitEmoticonToken` `0x00800600`.

---

## 5. `UIChoose3Wnd` — le menu de choix (id 0x11)

### 5.1 Réception — `NpcDialog_OnRecv_ZC_MENU_LIST` @ `0x00CC6A00`

`ZC_MENU_LIST` (recv @ `0x00CA09DE`) :

```c
GameMode+0x24C = 1;
SaveWindowRect(0x11);
menu = UIWindowMgr::MakeWindow(0x11);              // UIChoose3Wnd
menuStr = std::string(pkt+8, len-9);              // texte du paquet
tokens  = split(menuStr, ":");                    // délimiteur ':' (0x00FD72CC)
for (t : tokens)  if (!t.empty())
    menu->OnMsg(0x1B, t);                          // ajoute un choix cliquable
menu->OnMsg(0x1C, GID);                            // finalise (mémorise le GID)
g_UISayDialogWnd_primary_0x10->OnMsg(0x20);        // reset le dialogue principal
```

Le serveur envoie donc les choix comme **une chaîne unique séparée par `:`** ; le client
la découpe.

🔴 **Les options VIDES ne sont pas ajoutées à la liste** (`if (!t.empty())`) — et elles ne
comptent donc PAS dans l'index renvoyé. L'octet de `CZ_CHOOSE_MENU` est le **rang 1-based
parmi les options non vides**, pas la position dans la chaîne. Côté serveur, c'est la même
règle : `menu_countoptions()` (rAthena `script.cpp`) saute les vides pour calculer
`sd->npc_menu`, qui borne le contrôle anti-triche `select > sd->npc_menu` de
`clif_parse_NpcSelectMenu` (dépassement ⇒ **kick GM**).

⚠ Piège : la valeur de RETOUR de `select()` côté script, elle, est la position **absolue**
(vides comprises) — le serveur la reconstitue avec le compteur `total` de
`menu_countoptions()`. `select("a","","b")` **renvoie 3** pour « b » alors que le client
**envoie 2**. Les scripts à menu dynamique (ex. `moon/quests/huntmission.npc`) bourrent
exprès des options vides pour figer leurs numéros de `case` : envoyer l'index absolu y
déclenche systématiquement la mauvaise branche.

### 5.2 OnMsg — `UIChoose3Wnd_OnMsg` @ `0x008BE590` (dérivé) → `UIChooseWnd_OnMsg_base` @ `0x008BEE20`

Le dérivé `0x008BE590` intercepte `0x1B` (ajoute item, avec résolution de liens item via
`ResolveItemResNameById`) et délègue le reste à la base `0x008BEE20`.

Table des messages (base) :

| msg | rôle |
|---|---|
| **6** | **clic** : btn `0xB8` (OK/confirmer) → `CMode::SendMsg(0x17, choix, GID)` = **CZ_CHOOSE_MENU** (choix **1-based** = `liste[sel]+1`) ; btn `0xB9` (annuler) → `CMode::SendMsg(0x28)` ; puis `SaveWindowRect(0x11)` |
| **0xA6** | **ESC** → `CMode::SendMsg(0x17, 0xFFFFFFFF, GID)` = CZ_CHOOSE_MENU annulé (0xFF) |
| **0x1B** | **ajoute un choix** : append au sous-contrôle liste `+0xC4` (`[+0xD8]`), incrémente `+0xB4`, auto-resize la fenêtre + repositionne scrollbar `+0xD0` et bouton OK `+0xD4` |
| **0x1C** | set GID (`+0xC8`) |
| **0x12/0x13** | scroll de la liste `+0xC4` |
| **0x22** | positionnement |
| **0x7B** | parse blob (marqueurs `0x558C`/`0x558D`/`0x558F`) → `OnMsg(0x1B)` par segment |

Champs : `+0xC4` = contrôle liste (index sélectionné à `[+0x94]`, `-1` si aucun),
`+0xB8` = tableau des valeurs de choix, `+0xC8` = GID, `+0xD0` = scrollbar, `+0xD4` = OK.

---

## 6. `UINpcEditDialogWnd` — prompt NOMBRE (id 0x38)

Réception : `ZC_OPEN_EDITDLG 0x0142` (recv @ `0x00CA4B7F`) → `MakeWindow(0x38)` +
`OnMsg(0x1C)` (mémorise le GID). Ctor `UINpcEditDialogWnd_ctor` @ `0x0088E710` (objet
0xBC, pose vtable `0x01032824`).

### OnCreate — `UINpcEditDialogWnd_OnCreate` @ `0x008AAE30`

```c
edit = UIEdit_ctor(new(0x11C));  this[+0xB4] = edit;      // champ de saisie
UIWindow_SetSize(edit, W-0x4D, 0x10);  edit->SetPos(0xF, 0x16);
edit[+0x88] = 0xC;                                        // LONGUEUR MAX = 12 chiffres (défaut UIEdit 0xFF ; PAS une hauteur de police). Prompt texte = 0x46 (70 car.)
set_bg_color(edit, 0xE8,0xE8,0xE8);                       // fond gris clair
AddChild(this, edit);  register_focus(edit);             // FUN_00a4b760
ok = UIBitmapButton_ctor(new(0x120));                     // bouton [OK]
set_3_state_bitmaps(ok);  ok->SetCmdId(0xB8);  AddChild(this, ok);
this[+0xB8] = -1;   // GID (fixé par OnMsg 0x1C)
```

### OnMsg — `UINpcEditDialogWnd_OnMsg` @ `0x008C5BE0`

```c
case 6:  if (param_3 == 0xB8) {                    // clic [OK]
    if (edit_not_empty(edit)) {
        n = atoi(edit_text(edit));
        g_UICommandDispatcher->[0x18](99/*0x63*/, GID, n, 0, 0);   // -> CZ_INPUT_EDITDLG 0x143
        SaveWindowRect(0x38);
    }
}
case 0x1C:  this[+0xB8] = GID;
```

---

## 7. `UINpcTextEditDialogWnd` — prompt TEXTE (id 0x64)

Réception : `ZC_OPEN_EDITDLGSTR 0x01D4` (recv @ `0x00CA7388`). Ctor
`UINpcTextEditDialogWnd_ctor` @ `0x0088E770` (objet 0xC0, vtable `0x010328FC`).
OnCreate @ `0x008AB390` (UIEdit + bouton OK, analogue au §6).

### OnMsg — `UINpcTextEditDialogWnd_OnMsg` @ `0x008C5CC0`

```c
case 6:  if (param_3 == 0xB8) {                    // clic [OK]
    s = edit_text(edit);
    s = s.truncate_at('\n');                       // 1 seule ligne
    // VALIDATION : mots interdits (FUN_00a85c00 vs table 0x0159C2C8),
    //              format (FUN_007faf70 / FUN_007faa70)
    if (invalid) { edit->clear(); ChatAction(erreur 0xE / 0xBE2); }
    else {
        g_UICommandDispatcher->[0x18](s...);       // -> CZ_INPUT_EDITDLGSTR 0x1D5
        SaveWindowRect(0x64);
    }
}
case 0x1C:  this[+0xB8] = GID;
```

Le prompt texte fait donc un **filtrage anti-injection / mots interdits** avant l'envoi
(contrairement au prompt nombre).

---

## 8. Le dispatcher de commandes — `CMode::SendMsg` @ `0x00C86740`

Toutes les actions UI convergent vers **`CGameMode::vtbl[0x18]`** (résolu par
`GameMode_GetActive(0x1213338)` @ `0x00A75340`, qui renvoie `*(mgr+4)` si `*(mgr+0x58)==1`).
Ce slot = **`CMode::SendMsg`** @ `0x00C86740` — le dispatcher ré-entrant central
(use/equip/transfert **et** commandes NPC).

**Dispatch vérifié** (désassemblage complet du corps) : `JMP dword[(sélecteur−1)*4 + 0x00C930F0]`,
borne `0x14E`, défaut `0x00C9309B`. ⚠ **Correction** : dans le dump statique, `CMode::SendMsg`
a un prologue SEH **intact** — Ghidra échoue à le *décompiler* uniquement à cause de sa **taille**
(12 887 instructions), **pas** à cause d'un hook. (Le détour inline Bourgeon est un artefact
*runtime* absent du dump ; ne jamais le retirer du hook — casse le nom de map. Cf.
[[project_processinput_sendmsg_hook]].)

Mapping **sélecteur → builder → paquet CZ** (chaque builder : opcode+champs → `PacketLen_Get`
`0x00C14460` → `CRagConnection_SendPacket` `0x00C14920`) :

| sélecteur | builder | paquet CZ | déclencheur |
|---|---|---|---|
| `0x16` | `0x00C8911C` | **`CZ_REQ_NEXT_SCRIPT` 0x00B9** `{op,id}` 6o | clic **[Next]** (bouton cmd 0xDA en arg3) |
| `0x17` | `0x00C892E3` | **`CZ_CHOOSE_MENU` 0x00B8** `{op,GID,choix}` 7o | choix menu (1-based ; ESC=0xFFFFFFFF→0xFF) |
| `0x28` | `0x00C8760F` | **`CZ_CHOOSE_MENU` 0x00B8** (0xFF) *indirect* | annuler : délègue au menu `OnMsg(0xA6)`→sel 0x17, puis détruit les fenêtres NPC |
| `0x59` | `0x00C8932C` | **`CZ_CLOSE_DIALOG` 0x0146** `{op,GID}` 6o | clic **[Close]** (bouton cmd 0xC9) ; détruit 0x10&0x11 + `+0x24C=0` avant l'envoi |
| `0x63` (99) | `0x00C8E723` | **`CZ_INPUT_EDITDLG` 0x0143** `{op,GID,int32}` 10o | **[OK]** prompt nombre |
| `0xA4` | `0x00C864C0` | **`CZ_INPUT_EDITDLGSTR` 0x01D5** (VAR) `{op, len=strlen+8, GID, texte}` | **[OK]** prompt texte (`MOV EAX,0x1D5` @0x00C86609) |

⚠ **Ne pas confondre** *cmd id du bouton* (0xDA Next / 0xC9 Close — passé en arg3, tape le
défaut s'il sert de sélecteur) et *sélecteur SendMsg* (0x16 / 0x59, stocké à `fenêtre+0xC8`).
Menu et input-nombre utilisent des sélecteurs **directs** (0x17/0x28/0x63) ; Next et Close
des sélecteurs **indirects** (0x16/0x59). Les 4 opcodes vérifiés recoupent `docs/opcode_map`.

---

## 9. Table des opcodes NPC (réf. `docs/opcode_map`)

| opcode | sens | nom | handler recv |
|---|---|---|---|
| `0x00B4` | ZC | `ZC_SAY_DIALOG` (texte) | `0x00CA06B6` |
| `0x00B5` | ZC | `ZC_WAIT_DIALOG` (Next) | `0x00CA076E` |
| `0x00B6` | ZC | `ZC_CLOSE_DIALOG` (Close) | `0x00CA0964` |
| `0x00B7` | ZC | `ZC_MENU_LIST` (menu) | `0x00CA09DE` |
| `0x0142` | ZC | `ZC_OPEN_EDITDLG` (nombre) | `0x00CA4B7F` |
| `0x01D4` | ZC | `ZC_OPEN_EDITDLGSTR` (texte) | `0x00CA7388` |
| `0x08D6` | ZC | `ZC_CLEAR_DIALOG` | `0x00CA8654` |
| `0x0972` | ZC | `ZC_SAY_DIALOG2` | `0x00CA07AC` |
| `0x0973` | ZC | `ZC_WAIT_DIALOG2` | `0x00CA0865` |
| `0x0BA1` | ZC | `ZC_DIALOG_TEXT_ALIGN` | `0x00CA9F33` |
| `0x0BA2` | ZC | `ZC_DIALOG_WINDOW_SIZE` | `0x00CA9F47` |
| `0x0BA3` | ZC | `ZC_DIALOG_WINDOW_POS` | `0x00CA9F5F` |
| `0x0BB5` | ZC | `ZC_DIALOG_WINDOW_POS2` | `0x00CA9F77` |
| `0x0BA6/0BA7/0BA9` | ZC | `ZC_QUEST_DIALOG(_MENU_LIST)/MONOLOG` | — |
| `0x0090` | CZ | `CZ_CONTACTNPC` (clic sur NPC) | — |
| `0x00B8` | CZ | `CZ_CHOOSE_MENU` | — |
| `0x00B9` | CZ | `CZ_REQ_NEXT_SCRIPT` | — |
| `0x0143` | CZ | `CZ_INPUT_EDITDLG` | — |
| `0x0146` | CZ | `CZ_CLOSE_DIALOG` | — |
| `0x01D5` | CZ | `CZ_INPUT_EDITDLGSTR` | — |

---

## 10. L'arbre de widgets — racines & feuilles (résumé)

```
CGameMode (mode zone)                            ← host, tient les singletons + flag +0x24C
 └─ g_UIWindowMgr (0x0131F4E8)
     ├─ UISayDialogWnd  id 0x10 / 0xE2           ← RACINE fenêtre (UIFrameWnd<-UIWindow<-UIRPData)
     │   ├─ UIRichTextCtrl  (+0xB4)              ← branche : zone de texte
     │   │   └─ tokens TextLayout                ← FEUILLES : runs colorés, ^i icônes, ^e émotes,
     │   │       (runs, icônes, liens)                        liens cliquables (AddChild)
     │   └─ liste +0xB8 : UIBitmapButton         ← FEUILLES : [Next] (cmd 0xDA) / [Close] (cmd 0xC9)
     │
     ├─ UIChoose3Wnd  id 0x11                     ← menu
     │   ├─ liste +0xC4 (choix cliquables)       ← FEUILLES : lignes de choix (sel -> cmd 0x17)
     │   ├─ scrollbar +0xD0
     │   └─ bouton OK +0xD4 (cmd 0xB8) / annuler (cmd 0xB9)
     │
     ├─ UINpcEditDialogWnd  id 0x38               ← prompt nombre
     │   ├─ UIEdit +0xB4                          ← FEUILLE : champ de saisie
     │   └─ UIBitmapButton [OK] (cmd 0xB8)
     │
     └─ UINpcTextEditDialogWnd  id 0x64           ← prompt texte (+ validation mots interdits)
         ├─ UIEdit +0xB4
         └─ UIBitmapButton [OK] (cmd 0xB8)
```

- **Racine (classe)** : `UIRPData` → `UIWindow` → `UIFrameWnd` → `UISayDialogWnd`.
- **Racine (arbre d'affichage)** : la `UISayDialogWnd`/`UIChoose3Wnd` (fenêtre top-level).
- **Feuilles** : les `UIBitmapButton` (Next/Close/OK), les lignes de choix, et surtout les
  **tokens `TextLayout`** (runs de texte colorés par `^RRGGBB`, icônes `^i`, émotes `^e`,
  liens `<..>`) — chacun `AddChild`é au `UIRichTextCtrl`.

---

## 11. Index des adresses (renommées dans Ghidra)

| symbole | adresse |
|---|---|
| `UISayDialogWnd_ctor` | `0x0088EB50` |
| `UISayDialogWnd_dtor` | `0x00891700` |
| `UISayDialogWnd_scalar_dtor` | `0x00893850` |
| `UISayDialogWnd_OnCreate` | `0x008AE2E0` |
| `UISayDialogWnd_OnMsg` | `0x008C74C0` |
| `vtbl_UISayDialogWnd` | `0x01033094` |
| `NpcDialog_OnRecv_ZC_CLOSE_DIALOG` | `0x00CBAF10` |
| `NpcDialog_OnRecv_ZC_MENU_LIST` | `0x00CC6A00` |
| `UIChoose3Wnd_OnMsg` | `0x008BE590` |
| `UIChooseWnd_OnMsg_base` | `0x008BEE20` |
| `vtbl_UIChoose3Wnd` | `0x0104ADE0` |
| `UINpcEditDialogWnd_ctor` | `0x0088E710` |
| `UINpcEditDialogWnd_OnCreate` | `0x008AAE30` |
| `UINpcEditDialogWnd_OnMsg` | `0x008C5BE0` |
| `vtbl_UINpcEditDialogWnd` | `0x01032824` |
| `UINpcTextEditDialogWnd_ctor` | `0x0088E770` |
| `UINpcTextEditDialogWnd_OnCreate` | `0x008AB390` |
| `UINpcTextEditDialogWnd_OnMsg` | `0x008C5CC0` |
| `vtbl_UINpcTextEditDialogWnd` | `0x010328FC` |
| `UIRichTextCtrl_ctor` | `0x00806900` |
| `UIRichTextCtrl_WndProc` | `0x008079C0` |
| `UIRichTextCtrl_AddLine` | `0x00806FC0` |
| `TextLayout_ctor` | `0x007FF3E0` |
| `TextLayout_LexNextToken` | `0x008057F0` |
| `TextLayout_LexFontTag` | `0x00804750` |
| `TextLayout_LexBracketTag` | `0x00804B30` |
| `CMode::SendMsg` (dispatcher, hooké) | `0x00C86740` |
| `GameMode_GetActive` | `0x00A75340` |
| `UIWindowMgr::MakeWindow` (hooké) | `0x00A39340` |
| `g_UIWindowMgr` | `0x0131F4E8` |
| `g_NpcDialogPacketBuf` | `0x015E8198` |
| `g_UISayDialogWnd_primary_0x10` | `0x0131F6D8` |
| `g_UISayDialogWnd_secondary_0xE2` | `0x0131F6DC` |

---

## 12. Addendum — compléments & corrections vérifiés (workflow 7 agents, 2026-07-10)

> Approfondissement des fonctions initialement inférées/sautées. Tout ci-dessous est
> **vérifié par décompilation/désassemblage** sauf mention « (non vérifié) ».

### 12.1 Corrections majeures
- **Sélecteur `SendMsg` ≠ cmd id bouton** — voir §8 réécrite. Next = sélecteur **0x16**
  (pas 0xDA), Close = **0x59** (pas 0xC9). Opcodes CZ eux-mêmes corrects.
- **`0x00A24610` = `Replay_RecordUIEvent`**, PAS un hit-test. Enregistre l'événement UI
  `{id fenêtre +0x2C, 4 args OnMsg}` dans le fichier replay (pseudo-opcode 0x8AE) — actif
  seulement si `FUN_00b1fac0()->[+0xc]==2`. Appelé en tête de ~40 OnMsg (pas que NPC).
- **`edit[+0x88]` = longueur max de saisie** (défaut UIEdit `0xFF`), PAS une hauteur de
  police : nombre = `0xC` (12 chiffres), texte = `0x46` (70 caractères).
- **`CMode::SendMsg` non hooké dans le dump** (Ghidra échoue sur la taille, cf §8).
- **`^nItemID^<chiffres>`** (forme caret, `LexNItemIDName` 0x00804FF0) = substitution du
  **nom** d'item, distincte du lexer bracket. `ParseInlineNItemIDTag` (0x0097AE30) fait
  la même reconnaissance au rendu des choix de menu.

### 12.2 Handlers recv secondaires (tous = entrées de jump-table, agissent sur la **primaire 0x10** sauf indiqué)
| opcode | effet vérifié | helper |
|---|---|---|
| `0x0972` SAY_DIALOG2 | `MakeWindow(0xE2)` + `OnMsg(0x1A)` — texte à `pkt+9` (1 octet réservé à `+8`), fenêtre **0xE2** | — |
| `0x0973` WAIT_DIALOG2 | `MakeWindow(0xE2)` + `OnMsg(0x19)` [Next] — fenêtre **0xE2** ; GID à `pkt+2` (fixe) | — |
| `0x08D6` CLEAR_DIALOG | `FindWindow(0x10)->OnMsg(0x4B)` efface le richtext ; payload ignoré | `NpcDialog_ClearPrimaryText` 0x00D00060 |
| `0x0BA1` TEXT_ALIGN | `UIRichTextCtrl_SetAlign(0x00807CD0)` : val 0-2→richtext+0xB4 (H L/C/R), 3-5→+0xB5 (V) | `0x00D00960` |
| `0x0BA2` WINDOW_SIZE | tail-call `UISayDialogWnd_OnDraw` (vtbl+4, 0x008BC690)(w=`pkt+6`,h=`pkt+2`) : relayout richtext + boutons | `0x00D00A50` |
| `0x0BA3` WINDOW_POS | `UIWindow_SetPos(0x00874AF0)(x=pkt+2,y=pkt+6)` absolu | `0x00D00A30` |
| `0x0BB5` WINDOW_POS2 | `SetPos( xpct*(screenW−winW)*0.01, ypct*(screenH−winH)*0.01 )` — **pourcentages** de la zone libre | `0x00D00990` |

Nuance staging : les paquets **WAIT** (longueur fixe) lisent le GID à `g_NpcDialogPacketBuf+2`
(`0x015E819A`) ; SAY/MENU (variable) le lisent à `+4` (`0x015E819C`).

### 12.3 `TextLayout` — architecture à **deux phases** (vérifié par xrefs)
`UIRichTextCtrl_AddLine`/`Relayout` → `TextLayout_retokenize` (0x00801AA0) fait :
1. **`preprocess_escapes`** (0x00A92930) : `\r`/`\n`/`\"` normalisés en place → c'est ici que
   `\n` littéral d'un `mes` devient un vrai saut de ligne.
2. **Phase LEX** : boucle `LexNextToken` / `LexWordOrBreak` → **`PushToken`** (0x00803370)
   n'empile que des enregistrements de token de 0x30 o. **La couleur/style est figée ici**
   (`token+0x18`=couleur `this+0x24`, `token+0x20`=flags gras/italique, `token+0x24`=type).
3. **Phase DISPATCH** : `TextLayout_dispatch` (0x00801520) route par `token+0x24` vers les
   `Emit*`, **qui sont les seuls à créer les widgets-feuilles** (jamais pendant le lex).

Les `Emit*` mesurent (`FUN_00802780`), word-wrappent (`BreakLine` sur dépassement de
`this+8`) et attachent chaque feuille via `AddChildToLine`.

**Classes-feuilles concrètes** (nouveauté vs §10) :
| type token | Emit | feuille créée |
|---|---|---|
| 599 | `EmitPlainTextRun` 0x00801180 | **`UIText`** (vtable `0x0102752c`, 0xA8 o) via `CreatePlainRunWidget` 0x008034D0 |
| 0x1B5/1B6/1D0/205/21B | `EmitStyledTextRun` 0x00800B70 | **`UITextButton`** lien (0xF8 o) via `CreateLinkRunWidget` 0x00802DD0 |
| 0x241 | `EmitItemIconToken` 0x00800F60 | `ItemIconWnd` 25×25 (vtable `0x010276dc`) |
| 0x236 | `EmitEmoticonToken` 0x00800600 | émote 50×50 (vtable `0x01027604`) |

### 12.4 Jeu de balises `<…>` exact (`LexBracketTag` 0x00804B30, prefix, casse-sensible)
`<URL>`→0x1B5, `<ITEM>`→0x1D0, `<ITEML>`→0x1F5, `<NAVI>`→0x1B6, `<NAVIL>`→0x232,
`<MSG>`→0x233, `<QUEST>`→0x21B, `<TIPBOX>`→0x205 (chacun avec sa balise fermante). Les
littéraux sont des `std::string` globales construites au runtime (pool `0x01202xxx`).

### 12.5 `<FONT>` / `<B>` / `<I>` — **oui, fonctionnent sur les messages NPC**
`LexFontTag` (0x00804750) : `<FONT>…</FONT>` push/pop couleur+style (pile `+0x34/+0x38`) ;
`<B>`/`</B>` compteur gras `+0x10` ; `<I>`/`</I>` compteur italique `+0x14`. Ces compteurs
sont figés par `PushToken` (`token+0x20`), puis **`CreatePlainRunWidget` recopie le style byte
dans la feuille `UIText+0x88`** (couleurs `+0x7C/+0x80`) — et **re-mesure la largeur si le
style byte==1**, donc la police change réellement. Coloration idiomatique = `^RRGGBB`.

### 12.6 Layout du vtable UIWindow — **53 slots** (0x00→0xD0)
La lecture de 288 o débordait sur le COL + le vtable de la classe suivante. Slots standard
identifiés : **+0x04 = OnDraw** (base `UIWindow_OnDraw_Base` 0x00A245C0 — ⚠ **pas** +0x2C),
+0x10 SetPos, +0x38 SetVisible, **+0x3C OnCreate**, **+0x94 OnMsg**, +0x98 PaintDispatch,
+0xA4 Render, +0xB4 SetCommandId, +0xC8 HitTest. **+0x2C = SyncRectToNode** (recopie x/y/w/h
dans le nœud de données lié), **+0xB0 = SerializeComposite** (le **pendant écriture** de
`OnMsg 0x7B` : Say émet `0x57E4/E5/E7/E8`, Choose3 `0x558C/D/F`).

**Profil de surcharge (l'arbre de classes)** : `UISayDialogWnd` surcharge 8 slots
{0x00,0x04,0x2C,0x3C,0x4C,0x50,0x94,0xB0} = la plus riche (OnDraw + serialize custom) ;
`UIChoose3Wnd` 6 {0x00,0x2C,0x3C,0x50,0x94,0xB0} (pas de OnDraw) ; `UINpcEditDialogWnd` et
`UINpcTextEditDialogWnd` surchargent le **même jeu minimal** {0x00,0x3C,0x50,0x94} = deux
sœurs (texte = nombre + validation).

### 12.7 `UIChoose3Wnd` — nuance vtable
Le ctor `0x0088D3A0` pose la vtable de **BASE** `UIChooseWnd` `0x0103316C` (OnMsg base
`0x008BEE20`) ; la vtable **DÉRIVÉE** `0x0104ADE0` (OnMsg `0x008BE590`) est écrite **inline
par `MakeWindow`** juste après. Le compteur de choix `+0xB4` (init −1) est pré-incrémenté à
chaque `OnMsg(0x1B)` et sa valeur poussée dans le `vector<int>` `+0xB8` (→ 0,1,2…).

### 12.8 Helpers (feuilles logiques)
`UIEdit_GetTextLength` 0x008210D0 / `UIEdit_GetTextPtr` 0x008210A0 (texte `std::string` interne
`+0xD8`/size `+0xE8`/cap `+0xEC` ; si focus → contexte IME global `DAT_0159b80c`, commit
`vtbl[0xE0]`) ; `SplitString_ByDelim` 0x00582890 (menu par `:`) ; `BannedWord_ScanClean`
0x00A85C00 (liste GLOBALE `0x0159C2C8`, DBCS ; 1=propre/0=interdit ; wrapper `0x00A85BE0`
inverse) ; `NpcTextInput_ContainsForbiddenTag` 0x007FAF70 (anti-injection markup) +
`NpcTextInput_ValidateItemSkillLinks` 0x007FAA70 (liens item/skill inexistants → msg 0xE8D) ;
`MsgStringTable_GetById` 0x00A9ED30 (source des messages : 0x29A sépar. menu, 0xAAA input
bloqué, 0xE8D lien invalide, 0xBE2 erreur ; `NO MSG : %d` si id>0x1102).

### 12.9 Restes non vérifiés (honnêteté)
- ~~`CZ_INPUT_EDITDLGSTR` 0x01D5 non vérifié~~ → **VÉRIFIÉ en live (2026-07-11)** : sélecteur
  **`0xA4`** → builder **`0x00C864C0`** → `MOV EAX,0x1D5` @0x00C86609, paquet VAR
  `{op 0x1D5, len=strlen+8, GID, texte}`. `UINpcTextEditDialogWnd_OnMsg` pousse
  `SendMsg(0xA4, GID, textPtr)` après validation.
- Déréférencement littéral du champ `COL+0x0C` de `UINpcEditDialogWnd` non fait (jeu fermé +
  exe packé WinLicense) ; identité confirmée par l'invariant RTTI (vtable-4→COL, TD name lu
  `.?AVUINpcEditDialogWnd@@`).
- `UIText::OnDraw` → `CreateFontA` final (poids/italique) non tracé, mais la re-mesure
  conditionnée au style byte le rend quasi-certain.
- Contenu littéral des 4 marqueurs de balise interdits et OnCreate de `UIChoose3Wnd`
  (crée liste/scrollbar/OK) non décompilés.
