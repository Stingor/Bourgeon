# Chatbox RE — blueprint pour conversion ImGui totale

> Client **20250716** (Moonlight-Destiny.exe, base 0x00400000, pas d'ASLR).
> Objectif : documenter de fond en comble la chatbox native (logchat / logbattle)
> depuis le `MakeWindow` manager jusqu'aux feuilles, pour préparer une **réécriture
> ImGui complète**. Les adresses sont celles du projet Ghidra ; les noms en
> `CamelCase` sont les renommages Ghidra. Cf. plugin existant `src/plugins/chat.cc`.

Statut de rédaction : **en cours** (RE approfondie 2026-07-09).

---

## 0. Vue d'ensemble — trois systèmes distincts

Le « chat » à l'écran est en réalité un petit **arbre de classes** (vtables
**vérifiées par xref du ctor** le 2026-07-09) + **2 registres de canaux globaux** :

| Rôle | Classe | vtable | ctor | OnMsg |
|---|---|---|---|---|
| **Chat principale DOCKÉE** (cadre + onglets + input, id **1**) | `UINewChatWnd` | **`0x01037F80`** | `0x008d7660` | WndProc `0x008fc220` (+0x94) |
| **Un onglet / canal** (le log défilant) | `UISubChatWnd` | **`0x0102d280`** | `0x00834bf0` | — (msg id `0x1ed`) |
| **Chat DÉTACHÉE** (onglet arraché, flottant) | `UIChatWnd` détachée | **`0x01037ea8`** | `0x008d82a0` (`UIChatWnd_ctor`) | `HandleMsg 0x009020b0` (+0x94) |
| Popup **« nom cliqué »** (`ChatAction` 0xE) | `UIChatNamePopupWnd` | **`0x01034324`** | `0x0088f500` | `HandleEvent 0x008cd650` (+0x94) |
| Scroll interne d'un onglet (`tab+0xac`/`+0xb0`) | *(UIScrollText-like)* | `0x0102afd0` | — | — |
| Boîte de saisie | `UIEditWnd` | `0x01028dac` | `0x00817690` | — |
| Poignées de resize | `UIResizer` | `0x0102a260` | `0x008184a0` | — |

> ⚠ **Corrections de taxonomie (2026-07-09, xref-vérifiées).** (a) `0x01037ea8`
> (que la mémoire nommait `UISubChatHisWnd`) = **la fenêtre chat détachée**
> (`UIChatWnd`, OnMsg `0x009020b0`, onglet actif `+0xB4`, set `DAT_0131f510`). (b)
> `0x01034324` (ctor `0x0088f500`, OnMsg `0x008cd650`) = le **popup « nom »**, **PAS**
> une fenêtre de chat (malgré le nom Ghidra `UIChatWnd_HandleEvent`). Seule
> `UINewChatWnd` (`0x01037F80`) est la chat vivante docké (id 1).

**Registres globaux de canaux** (des `std::set`/`_Tree` MSVC, parcourus par la
création ET par le routage des messages) :

- `DAT_015faadc` — **liste des canaux/onglets de la fenêtre principale** : chaque
  entrée ⇒ un onglet `UISubChatWnd` dans le vector `this+0xF4`. Chaque descripteur
  porte un **nom** et une **table de filtre** (`node+0xB+type`) indiquant quels
  *types* de messages l'onglet accepte.
- `DAT_015faae4` — **liste des canaux des fenêtres détachées** : mappe vers le
  `std::set<UIChatWnd*>` global `DAT_0131f510`.
- `DAT_015faae0` — diviseur de largeur d'onglet (répartition de la barre d'onglets).

Autres globaux :

- `DAT_0131f8dc` = 1 quand la chat existe (posé au ctor).
- `DAT_0131f510` = tête du `std::set<UIChatWnd*>` des fenêtres détachées.
- `DAT_0131f9c4` (`std::string`) = dernier texte tapé (buffer de saisie global).
- `g_UICommandDispatcher` = dispatcher des commandes `/…`.
- `DAT_0131f50c` / `DAT_0131f50e` = flags d'état d'init de la barre d'input.

---

## 1. Création & cycle de vie (du sommet vers le bas)

### 1.1 `UIWindowMgr_MakeWindow(mgr, 1)` → `UINewChatWnd`
Cf. [[project_ui_window_manager]]. Le grand switch factory `UIWindowMgr_MakeWindow`
(`0x00a39340`) sur `windowID==1` fait `new UINewChatWnd` (ctor `0x008d7660`), stocke
le ptr à `mgr+0x1c8`, pose le flag « ouverte ». Les fenêtres persistantes (dont le
chat) sont recréées en **case 0** (entrée en jeu).

### 1.2 `UINewChatWnd_ctor` @ `0x008d7660`
- `UIWindow_composite_ctor(this,0)` (socle composite commun, cf.
  [[project_changematerial_and_uiwindow_composite_re]]), puis `*this = 0x01037F80`.
- Met à zéro les champs du modèle et pose les **valeurs initiales** :
  - `this+0x110 = 10` (`param[0x44]`) — *(cap défaut ?)*
  - `this+0x11c = 0x10` (16) — **hauteur de la barre d'onglets** (bandeau haut).
  - `this+0x120 = 0x47` puis, selon `g_ServiceType`, `0x6e` (services 1/7/0xB),
    `0x47` (10), `0x5a` (0x12) — **hauteur zone input / métrique**.
  - `this+0x124 = 0x18` (24) — **hauteur de la rangée d'input**.
  - `DAT_0131f8dc = 1` (chat vivante).
- **⚠ Comportement conditionné au `g_ServiceType`** : la géométrie diffère selon
  la région du client. À répliquer si on veut coller au natif.

### 1.3 `UINewChatWnd_Create(this, width, height)` @ `0x008e8fc0`
Construit toute la hiérarchie de widgets. Ordre exact :

1. **Calcul du nb de rangées d'onglets** (`this+0x30`) à partir de `height`.
2. `this+0x108 = width - 0x14` (base d'ancrage à droite lue par LayoutButtons).
3. **`this+0x138` = UITabStrip** (ctor `FUN_00837010`, taille 0xFC) —
   `FUN_00844710(strip, 1, this+0x11c, 10, 1,1,1, 0,0, 0xffffff,0x8e8e8e,0xffffff,0xffffff)`.
4. **`this+0x128` = UIBitmapButton `basic_interface\stickoff`** — bouton
   *détacher/coller* l'onglet. msg id **0x166**, position `2*this+0x120 + 4`.
5. **`this+0x12c` = UIBitmapButton `basic_interface\battle_option`** — bouton
   **options de log de combat (logbattle)**. msg id **0xc1**, position `2*this+0x120 + 0x13`.
6. **`this+0x100` = UIBitmapButton `basic_interface\battle_option`** (2ᵉ) —
   msg id **0x176**, position `2*this+0x120 + 0x22`.
7. **`this+0x104` = UIBitmapButton `basic_interface\wnd_mini`** — **minimize**.
   msg id **0xca**.
8. **Boucle sur `DAT_015faadc`** (canaux principaux) : pour chaque canal
   - `FUN_00864690(tabStrip, channelName, "")` ajoute le label d'onglet ;
   - `new UISubChatWnd` (ctor `FUN_00834bf0`, taille 0x124), `SetSize(width-8, 0x46)`,
     position, msg id **0x1ed** ; **push_back dans le vector `this+0xF4`**.
   - Le nom du canal est lu à `node+0x14` (SSO std::string, stride depuis `node+5`).
9. Si `DAT_015faae0 != 0` : `this+0x120 = tabStrip.width / DAT_015faae0`.
10. **Boucle sur `DAT_015faae4`** (canaux détachés) :
    `FUN_00a38ff0(&g_UIWindowMgr, idx, node[0x12], node[0x13], node[0x14], node[0x15])`
    — enregistre la config couleur/flags de chaque canal détaché auprès du manager.
11. **`this+0xBC` = UIEditWnd input** (ctor `UIEditWnd_ctor 0x00817690`, taille
    `0x1CC×0x10=460×16`, position x=`0x6e`). `input+0x88 = 0xEA` (**234** = nombre
    d'unités de remplissage de la barre de fond, **hardcodé**, jamais recalculé au
    resize → cf. bug largeur input dans `chat.cc`). Enregistré comme cible de focus
    (`FUN_00a4b760`). msg id **0x1ed**. `this+0xB4 = this+0xBC` (input actif courant).
12. **`this+0xC0` = bouton envoi** (ctor `FUN_00818c10`, taille `0x5A×0x10`, x=6).
    `sendBtn+0x88 = 0x18`.
13. `this+0x8C = 0xB8`.
14. **`this+0xB8` = UIResizeButton (poignée LARGEUR)**, `SetMode(0)` (0/1=largeur),
    position `(2, height - grip[6])`. **⚠ le WndProc IGNORE mode 0/1** ⇒ largeur non
    grabbable nativement (cf. `chat.cc`, largeur pilotée par slider).
15. **Boucle sur 3 → `this+0xE0[0..2]` = boutons filtre** (UIBitmapButton).
    - Bitmaps `dialog_btn0/1/2` + `sys_base_off/on`.
    - Libellés via `FUN_00a9ed30(0xC73/0xC75/0xC74)` (chaînes localisées).
    - **msg ids** `0xE1`, `0xD4`, `0xEE` ; **positions x** `0x61` (97, gauche),
      `0x24A` (586), `0x23C` (572, droite). ⇒ ce sont les **toggles de filtre de
      canal** au bas de la barre d'input (public / groupe / guilde…). Codés en dur
      à 586/572 (largeur de création 600) → repositionnés par `chat.cc`.
16. **UIResizeButton (poignée HAUTEUR)** anonyme, `SetMode(2)`, taille
    `(width-0x1A, 6)`, position `(3, this+0x11c)`, colorkey `0xffff00ff`. **Non stocké
    dans un champ `this+`** → `chat.cc` le retrouve en parcourant `this+0x50`.
17. `this+0xC4 = mesureX+3` ; `this+0xC8 = mesureY + this+0x11c`.
18. **`this+0xD8 = 0x66000000`** — **couleur de fond ARGB** (site patch WARP opacité).
19. `this+0xCC = width - 6` (bord droit) ; `this+0xD0 = (height - this+0x11c) - this+0x124`.
20. Si `DAT_0131f50e == 0` : `FUN_008f9840(this)` (layout rangée input) +
    focus input.
21. Si input existe : **`this+0x13C` = sous-fenêtre** (0xA0 octets, ctor
    `FUN_0078efd0`), taille `0xC0×0x2C`, ancrée sur l'input → probable **popup
    d'autocomplétion / aide commandes / emoji** (`vtable+0x38(0)` = hide initial).

### 1.4 Destruction
`dtor = FUN_008db7f0` (vtable[0]). *(à détailler : libération du vector d'onglets,
du set détaché, désenregistrement manager)*.

---

## 2. Modèle de données

### 2.1 `UINewChatWnd` — champs (offsets confirmés)

| Offset | Type | Rôle |
|---|---|---|
| `+0x00` | vptr | `0x01037F80` |
| `+0x14` | int | **largeur** fenêtre |
| `+0x18` | int | **hauteur** fenêtre |
| `+0x1C`/`+0x20` | int | position X / Y écran |
| `+0x30` | int | **nb de rangées d'onglets** (0 = barre repliée) |
| `+0x50` | std::list | liste des enfants (sentinelle ; node+0=next, node+8=child) |
| `+0x8C` | int | = 0xB8 |
| `+0xB4` | ptr | **input actif courant** (= `+0xBC`) |
| `+0xB8` | UIResizer* | poignée resize largeur (mode 0, non fonctionnelle) |
| `+0xBC` | UIEditWnd* | **boîte de saisie** |
| `+0xC0` | ptr | **bouton envoi** |
| `+0xC4` | int | métrique layout X |
| `+0xC8` | int | métrique layout Y |
| `+0xCC` | int | bord droit (= width-6) |
| `+0xD0` | int | hauteur zone log |
| `+0xD4` | int | 0 (scroll ?) |
| `+0xD8` | ARGB | **couleur de fond** = `0x66000000` |
| `+0xDC` | int | 0 |
| `+0xE0`..`+0xE8` | ptr[3] | **boutons filtre** (0,1,2) |
| `+0xF0` | int | **cap d'historique** (nb max de lignes brutes) *(propagé aux tabs)* |
| `+0xF4`/`+0xF8`/`+0xFC` | vector | **`std::vector<UISubChatWnd*>`** (onglets) begin/end/cap |
| `+0x100` | ptr | bouton `battle_option` #2 (msg 0x176) |
| `+0x104` | ptr | bouton `wnd_mini` (minimize) |
| `+0x108` | int | base ancrage droite (= width-0x14) |
| `+0x110` | int | = 10 (init ctor) |
| `+0x114` | int | index onglet actif *(lu par case 3/4 scroll, case 0x16 select)* |
| `+0x11C` | int | hauteur bandeau d'onglets (= 0x10) |
| `+0x120` | int | métrique largeur/hauteur onglet (service-dépendant) |
| `+0x124` | int | hauteur rangée input (= 0x18) |
| `+0x128` | ptr | bouton `stickoff` (détacher) msg 0x166 |
| `+0x12C` | ptr | bouton `battle_option` #1 (msg 0xc1) |
| `+0x138` | UITabStrip* | **barre d'onglets** |
| `+0x13C` | ptr | sous-fenêtre popup (autocomplete/aide, 0xA0 o) |

### 2.2 `UISubChatWnd` (un onglet/canal) — champs

Écrit par `UISubChatWnd_AddLine` @ `0x0083f070` — **le chokepoint du modèle** :
il stocke chaque message en **3 vecteurs parallèles** (RAW, distinct des lignes
dessinées) :

⚠ **Correction (2026-07-09)** : il y a **DEUX** modèles de lignes, chacun = **3
vecteurs parallèles** :
- **RAW** (`+0x100/+0x10C/+0x118`) = source de vérité, jamais wrappée (écrite par
  `AddLine`). **C'est ce qu'un viewer ImGui doit lire.**
- **DESSINÉ/wrappé** (`+0x88/+0xE4/+0xF4`) = dérivé par re-wrap à la largeur courante
  (ce que le painter natif lit). La **liste chaînée `+0xD4`** ne contient **pas** de
  texte — seulement les **nœuds de rendu interactifs** (boutons item-link / emoticons).

| Offset | Type | Rôle |
|---|---|---|
| `+0x00` | vptr | `0x0102d280` |
| `+0x14` | int | **largeur onglet** (= wrap width ; wrap coupe à `+0x14 - 0x10`) |
| `+0x18` | int | **hauteur** ; `lignes_visibles = +0x18 / +0xB8` |
| `+0x7C/+0x80/+0x84` | 3×byte | couleur texte défaut (B/G/R) lue si `+0xB4==0` |
| `+0x88/+0x8C/+0x90` | vector | **texte DESSINÉ** `vector<std::string>` (stride 0x18) |
| `+0x94` | int | index de la **ligne surlignée** (barre de sélection) |
| `+0x98` | int | **ligne du haut** (top de scroll) |
| `+0x9C` | int | indent / x-scroll |
| `+0xA0` | int | **largeur px max** des lignes (range h-scroll ; maj dans StoreLine) |
| `+0xA4` | flag | overflow vertical / autoscroll actif |
| `+0xA8` | int | base de scroll |
| `+0xAC` | ptr | **scroll-ctrl A** (vtable `0x0102afd0`, 0xB4 o, recréé par OnResize) |
| `+0xB0` | ptr | **scroll-ctrl B** (même classe ; barre bord droit) |
| `+0xB8` | int | **pitch de ligne** (px/ligne, stocké — PAS dérivé police) |
| `+0xC0` | ARGB | couleur barre de surbrillance |
| `+0xC8` | COLORREF | couleur ligne courante (passée au painter) |
| `+0xCC` | byte | flag centrage |
| `+0xD0` | int | origine X (gauche) |
| `+0xD4/+0xD8` | list/int | **liste chaînée des NŒUDS DE RENDU** (boutons liens) / compteur |
| `+0xE4/+0xE8/+0xEC` | vector | **couleurs DESSINÉES** `vector<uint32_t>` (stride 4) |
| `+0xF0` | uint | **cap historique** (max lignes brutes) |
| `+0xF4/+0xF8/+0xFC` | vector | **senders DESSINÉS** `vector<std::string>` (stride 0x18) |
| `+0x100/+0x104/+0x108` | vector | **RAW texte** `vector<std::string>` (stride 0x18) |
| `+0x10C/+0x110/+0x114` | vector | **RAW couleurs** `vector<uint32_t>` (stride 4) |
| `+0x118/+0x11C/+0x120` | vector | **RAW senders** `vector<std::string>` (stride 0x18) |

- `count = (+0x104 - +0x100) / 0x18`. Quand `count >= +0xF0` → `TrimHistoryHalf`
  (`0x00844f50`) supprime la moitié la plus ancienne.
- **Table de filtre par type** : `node+0xB+type` (dans le descripteur de canal
  `DAT_015faadc`, pas dans l'onglet lui-même) — voir §3.

**Scroll-ctrl (`+0xAC`, vtable `0x0102afd0`, 0xB4 o)** — champs pilotés par
`DrawContent` : `ctrl+0x80` scroll MAX, `ctrl+0x84` scroll POS, `ctrl+0x88`
orientation (1=A,0=B), `ctrl+0x98` **nb total de lignes**, `ctrl+0x9C` **nb visibles**,
`ctrl+0xA0`=10 (pas), `ctrl+0x58` dirty. `+0xB0` = idem orientation 0 (barre droite,
réserve 0x10 px sur la largeur de wrap).

**Nœud de rendu interactif** (liste `+0xD4`) : `+0=next, +4=prev, +8/+0xC/+0x10 =
vector<UIItemLinkBtn*>`. Un **`UIItemLinkBtn`** (~0x200 o) : `+0x18` Y ; `+0x7C`
std::string nom affiché ; `+0x1F0` offset X ; `+0x1F4` index de ligne dessinée ;
`+0x1F8` **nameid** (ou skillId). Posé par `LayoutLinkSubline 0x0084a780`.

### 2.3 Slots vtable sémantiques (socle composite `UIWindow`, byte-offsets)
Relevés à travers les appels `(**(code**)(*this+N))(...)` dans les décompilations.
Communs à `UINewChatWnd` / `UISubChatWnd` / boutons (socle
[[project_changematerial_and_uiwindow_composite_re]]) :

| Slot | Rôle |
|---|---|
| `+0x00` | destructeur |
| `+0x10` | **SetPosition**(x, y) (`UIWindow_SetPos 0x00a23450`) |
| `+0x18` | notify/close (vu dans `Chat_HandleChatMessage`) |
| `+0x38` | **Show/Hide**(bool) |
| `+0x3C` | **Create**(w, h) (chat : `UINewChatWnd_Create 0x008e8fc0`) |
| `+0x50` | DrawContent (poignées `UIResizer`) |
| `+0x94` | **OnMsg / WndProc**(this, p1, msg, p3, p4, p5, p6) — chat : `0x008fc220`. **Point de dispatch de msg 0x25/0x73.** |
| `+0x98` | paint dispatch |
| `+0xB4` | **SetMsgId / SetWindowId**(id) (ex. `0x1ed`, `0x166`, `0xc1`…) |
| `+0xD4` | SetRect(x, y, w, h, flag) |
| `+0xDC` | (UISubChatWnd) **ClearDrawnLines** `0x00844d90` |
| `+0xE0` | (UISubChatWnd) **DrawContent** `0x0085e120` |
| `+0xE4` | (UISubChatWnd) **AppendDrawnLine** `0x0083d840` (ajoute 1 ligne wrappée) |

*(vtables complètes UISubChatWnd `0x0102d280` / scroll-ctrl `0x0102afd0` / UIChatWnd
détaché : voir §4.4 + §7.)*

---

## 3. Ingestion des messages (le cœur logchat/logbattle)

### 3.1 Point d'entrée UI : `UINewChatWnd_WndProc` @ `0x008fc220`, **case 0x25** (= « add line »)
Signature : `__thiscall(this, param_1, msg=param_2, param_3, param_4, param_5, param_6)`.
Pour `msg == 0x25` (et variante `0x73`) : `param_3 = texte`, `param_4 = TYPE/canal`,
`param_5 = couleur`, `param_6 = expéditeur`.

Flux :
1. Copie `param_6` (sender) dans un buffer pile `local_30`.
2. **Selon `param_4` (type)** : types **1, 3, 4, 0x15** → extrait le nom
   d'expéditeur depuis `"Nom: message"` (cherche `": "` = `0x3a20`, ≤ 0x18 chars).
3. Compare le sender au **nom du joueur local** (`FUN_00d7fe40(session 0x15fa3c0)`) ;
   si égal, blanchit (message « à soi »).
4. `FUN_007faf70(text)` = teste si ligne à **liens** (retour bool). Sinon
   `FUN_00979f10(text, &list, 0x5E)` = **splitter 94 caractères** (le hook
   `CharWrapHook` de `chat.cc`), qui produit une liste de sous-lignes.
5. **Flag broadcast** `local_23e` : vrai si `type == 0x19` (25) OU (`type==0 &&
   color==0xff`). ⇒ **type 0x19 = annonce diffusée à TOUS les onglets**.
6. **Boucle onglets principaux** (`DAT_015faadc`, index = position dans `this+0xF4`) :
   - accepte si `channelNode[0xB + type] != 0` **OU** broadcast, ET l'onglet existe.
   - `UISubChatWnd_AddLine(tab, text, color, sender)` → **RAW history**.
   - puis pour chaque sous-ligne wrappée : `UIText_MeasureWidth` ; si trop large et
     pas un lien → `UISubChatWnd_WrapAndDispatch(tab, subline)` ; sinon appel direct
     `tab.vtable+0xE4` (`AppendDrawnLine`) → **lignes DESSINÉES**.
7. **Boucle fenêtres détachées** (`DAT_015faae4` → set `DAT_0131f510`) : mêmes
   critères de filtre ; chaque fenêtre reçoit `vtable+0x94` (redraw/scroll).

> **Conséquence pour ImGui** : `UISubChatWnd_AddLine` est **le** point unique par
> onglet, avec (texte, couleur, sender). Il est déjà hooké (`AddLineHook` dans
> `chat.cc`). Le **type/catégorie** n'y est PAS passé (consommé en amont) ⇒ pour
> mirrorer la catégorie, hooker le case 0x25 ou le helper « AddText ».

### 3.2 API « imprimer une ligne » : `UIWindowMgr_ChatAction` @ `0x00a4ad20`
*(ex-`FUN_00a4ad20`, renommée + commentée en Ghidra le 2026-07-09.)*
C'est **le helper public** appelé 50+ fois par `Chat_HandleChatMessage` et par le
WndProc lui-même. `__thiscall(mgr, action, text, colorRGB, name, extra)`.

| action | effet |
|---|---|
| **1** | **ajouter une ligne** → `chatMain(mgr+0x1C8)->WndProc(msg 0x25, text, …)`. Si `mgr+0x1C8==0` (chat pas créée) → **file d'attente** `mgr+0x4C4` (nœud 0x24 o : `+8` texte, `+0x18` couleur), drainée à la création. |
| **0x13** | idem mais **msg 0x73** (ligne déjà wrappée / pas de re-split — voie « battle » / continuation). |
| 2 | refresh chat |
| **3** | `ToggleWindow(mgr,1,1)` = **ouvrir/afficher** la chat |
| **6** | **/savechat** → `ChatLog_SaveAllToFiles(mgr+0x1C8)` |
| **8/9** | `MakeWindow(0x33)` (fenêtre config/onglets) + refresh |
| **0xE** | popup **« nom cliqué »** : ajoute au set `mgr+0x500`, crée une fenêtre `0x118×0x78` (ctor `FUN_0088f500`), texte `"  (name)  *^_^*"` (whisper/ajout ami…) |
| 4/5/7/0xf/0x10/0x11/0x9a | refresh/msg vers les fenêtres `mgr+0x1bc`, `mgr+0x27c`, ou les popups |

**Fenêtres côté manager** : `mgr+0x1C8` = chat **principale** (id 1) ;
`mgr+0x27C` / `mgr+0x1BC` = **chats secondaires** (candidat : battle-log /
mini-chat) ; `mgr+0x4C4` = **file de lignes en attente** ; `mgr+0x500` = set des
popups de nom.

Couleur = **RGB** (`0`→blanc `0xffffff` ; ex. `0x64ffff`, `0x6E96FF`, `0x1Effff`,
`0xffff` bleu).

### 3.2.1 Codes de TYPE / catégorie (le champ `param_4` du msg 0x25)
Le msg 0x25 reçoit `(text, X, Y, extra)`. Le WndProc lit `local_23c` comme
**type/canal** (parse sender pour 1/3/4/0x15 ; broadcast si `0x19`, ou `0 && color==0xff`)
et `local_24c` comme **couleur** passée à `AddLine`. La séparation exacte
type↔couleur dans les args de `ChatAction` est **ambiguë en statique** (le
décompilateur mélange). ⚠ **Sans impact pour la conversion ImGui** : on mirrore au
niveau `UISubChatWnd_AddLine(tab, text, color, sender)` — texte, couleur et sender
**finaux, par onglet de destination** — donc on récupère le routage déjà décidé sans
avoir à le rejouer. Pour obtenir la *catégorie* (afin de recolorer/filtrer côté
ImGui), trace live à `0x00a4b272` (dispatch msg 0x25) ou hook du WndProc case 0x25.

Types connus (usage sender-parse dans WndProc) : **1, 3, 4, 0x15** = « Nom: msg » ;
**0x19** = annonce diffusée à tous les onglets ; **0** = défaut. Enum complet =
jump-table `switchdataD_00c7d3d4` de `Chat_HandleChatMessage` (catégories entrantes
clif → action `ChatAction`).

### 3.3 Processeur d'input / commandes : `Chat_HandleChatMessage` @ `0x00c7a460`
- `__thiscall(this, param_1=catégorie/commande, param_2=byte* texte)`, jump-table
  `switchdataD_00c7d3d4[param_1]`. C'est le **traitement du texte SORTANT** (tapé)
  et des **commandes**, PAS un logger permanent.
- **⚠ `%s\Chat\Chat.txt` (rotation `Chat%03d.txt`) n'est écrit QUE par `/savechat`**
  (confirmé par l'utilisateur) — ce n'est **pas** un journal continu. Le vrai
  « logchat / logbattle » est le **routage en mémoire par canal** (§3.1), pas un
  fichier. (`ProbabilityLog(...).txt` = log d'enchantement, hors sujet chat.)
- **case 0** = traitement du **texte tapé** : préfixes `%` / `$` / `^` (canaux :
  clan, etc.) + modificateurs `Ctrl/Alt/CapsLock` (`GetAsyncKeyState`) → choix du
  canal de sortie. *(détail du dispatch de sortie à compléter — §5)*
- Appelé depuis `Chat_ProcessStringList 0x00c631e0` (flush par lot : itère un
  `std::set<std::string>` → `Chat_HandleChatMessage(proc, 0x12, &str)` pour chacune ⇒
  **catégorie 0x12 = lignes en lot**) et 3 sites clif en code non-analysé
  (`0x00c88916/…5a/…c0f6`).
- **`/savechat` = `ChatLog_SaveAllToFiles` `0x00907030`** : parcourt le vector
  d'onglets de la fenêtre principale + le set détaché `DAT_0131f510` et vide chaque
  historique brut dans `Chat.txt`.

### 3.3.1 Énumération des catégories (`param_1`)
Le **domaine `param_1` de `Chat_HandleChatMessage` EST l'enum des catégories**
logchat/logbattle (0..~0x77), remappé par la jump-table **`switchdataD_00c7d3d4`**.
Confirmés : **0x12** = lignes en lot (flush) ; **0x32** = cas spécial (case 0 :
`if (param_1==0x32) → repaint direct`). Palette de couleurs observée aux sites
`ChatAction(mgr,1,text,color,…)` (= aspect visuel par catégorie) : `0x64ffff`
(cyan clair / système), `0x6E96FF` (bleu), `0x1Effff` (cyan), `0xFFFF` (bleu),
`0xFF00` (vert / confirmation), `0xFF` (déclencheur broadcast), `0` (blanc défaut).
**Enum sémantique complet = TODO borné** : lire les octets de `switchdataD_00c7d3d4`
(→ mapping `param_1`→case) + le corps de chaque case, ou trace live. **Non bloquant**
pour ImGui (§3.2.1).

---

## 4. Pipeline de rendu (vérifié 2026-07-09)

### 4.1 Chaîne de dessin d'un onglet
```
Entrée : UISubChatWnd.vtable+0x04 = OnDraw (0x0085f630)  (args w,h)
  └─ _BgAndControls (0x0085f930)
       ├─ UIWindow_OnDraw_Base(this)                       // fond + cadre
       ├─ place scroll-ctrl A (+0xAC) : (*+0x04)(0xC,h) ; (*+0x10)(w-0xD,0)
       ├─ place scroll-ctrl B (+0xB0) : (*+0x04)(w-0xD,0xC) ; (*+0x10)(0,h-0xD)
       └─ DrawContent (0x0085e120, vtable+0xE0)
            ├─ excédent = totalLignes - visibles ; +0xA4 = (excédent>0)
            ├─ pousse géométrie dans (+0xAC): +0x80 max, +0x84 pos, +0x98 total, +0x9C visibles
            └─ (*+0xAC +0x98)() = UIWindow_PaintDispatch (0x00a23340)
                 → chaîne d'enfants (+0x10) → **painter feuille** (blit du texte)
```
**Boucle par-ligne** (gabarit exact = `UIScrollText_PaintLines 0x008539c0`) :
```
top  = +0x98 ; last = min(total, +0x18/+0xB8 - +0xA8 + top)
pour i de top à last-1 :
  si i == +0x94 → dessine la barre de surbrillance (y=i*pitch, w=+0x14, h=pitch-1, col=+0xC0)
  x = +0xD0 - +0x9C (+recentrage si +0xCC==1) ; y = (i-top)*+0xB8
  UIText_DrawColored(x, y, str=(+0x88)[i], len, &col=+0xC8, 0, size=0xC, 0)
```
`UIText_DrawColored 0x00a26540` (taille **0xC=12 px** réels, gère `^RRGGBB`) → feuille
GDI `FUN_005471a0` (hook `TextOutLowHook` pour les icônes `^i` dans `chat.cc`).

> ⚠ **Correction d'attribution (2026-07-09)** : `(+0xAC)+0x98` résout statiquement vers
> `UIWindow_PaintDispatch 0x00a23340` (dispatcher composite générique), et
> `xref(0x008539c0)` ne liste `PaintLines` que dans **6 vtables sœurs** (`0x0102c208,
> 0x0102c2f0, 0x0102d940, 0x01030d8c, 0x0103af34, 0x0108858c`) — **aucune n'est
> `0x0102d280` ni `0x0102afd0`**. Donc `PaintLines` n'est **pas littéralement** la
> méthode du chat (la note de [[project_chat_item_icons]] la sur-attribuait) : le
> chat blitte via l'équivalent atteint au bout de la chaîne d'enfants `+0x10`. **Les
> offsets et la formule d'itération sont identiques** → `PaintLines` reste le gabarit
> exact pour la réécriture.

### 4.2 Wrap & production des lignes dessinées
- `WrapAndDispatch 0x0083d3f0` : coupe une ligne brute à `+0x14 - 0x10` via
  `ChatText_WordWrap 0x00a23a20` → appelle le virtuel **`+0xE4`** (`AppendDrawnLine`)
  par chunk.
- `AppendDrawnLine 0x0083d840` (virtuel +0xE4) : détecte liens/URL via `_mbsstr`
  (`_Src_01204068`, `DAT_01204080`) → si lien → `RenderLinkLine 0x00865e80` ; sinon →
  `StoreLine 0x0083dd90` (push texte `+0x88`, maj `+0xA0`). Push couleur `+0xE4` /
  sender `+0xF4`. **Auto-scroll** en bas si `+0xA4` et vue déjà en bas.
- `RenderLinkLine 0x00865e80` : re-découpe puis pour chaque sous-ligne →
  `LayoutLinkSubline 0x0084a780`.
- `LayoutLinkSubline 0x0084a780` : parse les segments `<ITEML>`/skill, crée les
  `UIItemLinkBtn` (`new(0x200)`), renseigne `btn+0x1F4` ligne / `+0x1F8` id / `+0x1F0`
  x / `+0x7C` nom, et **insère un nœud dans la liste `+0xD4`** (`+0xD8`++).
- `LayoutItemLinks 0x00849ed0` (appelé au dessin, via `HandleEvent`/`HandleMsg` case
  **0x9A**) : parcourt `+0xD4`, `SetName` + `UIItemLinkBtn_OnDraw(screen_x, btn+0x18)`,
  `ChatItemSpriteCache_Get(btn+0x1F8)` (enregistre le nameid dans `DAT_01251800`), puis
  `RebuildFromHistory`.

### 4.3 Re-wrap / clear (idempotents)
- `RebuildFromHistory 0x008642d0` : `(*this+0xDC)()` **puis** `ClearDrawnLines(this)`,
  puis re-parcourt le RAW (`+0x100/+0x10C/+0x118`) → `WrapAndDispatch` (texte) ou `+0xE4`
  (liens), puis `(*this+0xE0)()`. **Déclencheurs** : `TrimHistoryHalf`, `FUN_00842e10`,
  `LayoutItemLinks`, `FUN_0085f340` (= SetSize onglet, au resize).
- **Deux clears distincts** : virtuel **`+0xDC` = `FUN_00842230`** vide les **3 vecteurs
  DESSINÉS** (`+0x88`/`+0xE4`/`+0xF4`) ; **`ClearDrawnLines 0x00844d90`** (appel direct,
  PAS le virtuel) détruit les **nœuds de rendu** de `+0xD4` (chaque `Btn*` via
  `FUN_00a24560`) et remet `+0xD8=0`.

### 4.4 Slots vtable `UISubChatWnd 0x0102d280` (confirmés xref)
`+0x04` OnDraw (0x0085f630) · `+0x3C` OnResize (0x0084d310) · `+0x94` OnMsg · `+0x98`
PaintDispatch (0x00a23340) · `+0xDC` ClearDrawnVectors (0x00842230) · `+0xE0`
DrawContent (0x0085e120) · `+0xE4` AppendDrawnLine (0x0083d840). Vtables **sœurs**
(même `+0xE4`) : `0x0102d370` / `0x0102d460` / `0x0102d554`.

---

## 5. Saisie & envoi

### 5.1 Boîte de saisie
`this+0xBC` = `UIEditWnd` (vtable `0x01028dac`, ctor `UIEditWnd_ctor 0x00817690`).
Le texte courant se lit via `FUN_008210a0(input)` ; `FUN_008210d0(input)` = a-t-il du
texte. Historique de saisie (flèches haut/bas) : `input+0x144` (buffer courant),
`input+0x104`/`input+0x114`/`input+0x118` = ligne d'édition (SSO string).

### 5.2 ENTER → envoi (`WndProc case 6`, sous-case `0xB8`)
La sous-case `0xB8` du `switch(controlId)` (l'input a l'id de contrôle 0xB8) traite
la validation. Séquence :
1. Récupère le texte (`FUN_008210a0(input)`), coupe au premier `\n`.
2. **Commande `/…`** : si le texte commence par `/` ou si le bouton d'envoi a du
   contenu → stocke le texte dans **`DAT_0131f9c4`** (`std::string` globale = « texte
   à envoyer ») puis **appelle `g_UICommandDispatcher` slot 6** (`(*disp)[6]()`).
   C'est **LE chemin d'envoi** : slot 6 = « traiter/envoyer l'input courant » →
   route vers `Chat_HandleChatMessage` (`0x00c7a460`) qui parse les **préfixes de
   canal** (`%`, `$`, `^` = clan/etc.) + modificateurs `Ctrl/Alt/CapsLock` et émet
   le **paquet réseau** approprié (CZ_REQUEST_CHAT / whisper / party / guild…).
3. **Nom de skill** : `FUN_00d5e590(&g_SkillInfoMgr, text, …)` — si l'input est un
   nom de skill connu, l'exécute (`FUN_00887fa0`) au lieu d'envoyer.
4. Nettoie l'input, remet le focus (`FUN_00a4b760(mgr, input)`).

> **Pour ImGui** : deux options d'envoi — (a) **piloter l'input natif** : écrire le
> texte dans `this+0xBC` puis poster la notification ENTER (msg 6, ctrl 0xB8) ; (b)
> **répliquer** : `std_string_assign(DAT_0131f9c4, texte)` puis `g_UICommandDispatcher
> ->vtable[6]()`. (a) est le plus robuste (récupère commandes, skills, préfixes,
> historique gratuitement).

### 5.3 Fenêtres annexes de gestion (logchat / **logbattle**)
Ouvertes par les boutons de la barre de chat (`WndProc case 6`) :

| Bouton (msg id) | Champ | Action |
|---|---|---|
| `battle_option` (**0xC1**, aussi 0x176) | `this+0x12C`/`+0x100` | `MakeWindow(**0x84**)` = **fenêtre de config du LOG DE COMBAT** : coche quels messages de combat/exp/drop/miss… s'affichent. C'est le vrai « **logbattle** » — pas une fenêtre séparée, mais un **jeu de filtres** qui alimente la table `channelNode[0xB+type]`. |
| filtre (**0xE1**) | `this+0xE0[0]` | `MakeWindow(**0x1A**)` = **ajout/sélecteur de canal** (crée un onglet). |
| filtre (**0xD4**) | `this+0xE0[1]` | re-dispatch `vtable+0x94` (toggle filtre). |
| filtre (0xEE) | `this+0xE0[2]` | (3ᵉ toggle) |
| `wnd_mini` (**0xCA**) | `this+0x104` | **fermer l'onglet courant** (retire de `DAT_015faadc` + du vector `this+0xF4`, relayout, `SelectTab`). |
| `stickoff` (**0x166**) | `this+0x128` | détacher/coller l'onglet (→ `UIChatWnd`). |

Autres fenêtres : `0x33` = **setup chat** (`ChatAction` 8/9) ; `0x1A` = **canaux**
(SaveWindowRect 0x1a) ; `0x84` = **log combat**.

> **logbattle vs logchat** : ce ne sont **pas** deux fenêtres distinctes. Tout passe
> par la même chatbox ; « logbattle » = la **catégorie battle** des messages (types
> combat) et sa fenêtre de **filtres** (0x84), « logchat » = les catégories de
> conversation. Le routage vers les onglets = la table de filtre par canal (§3.1).

---

## 6. Persistance & config des canaux

### 6.1 Descripteur de canal (`DAT_015faadc` — `std::set`)
Chaque entrée = un nœud `_Tree` MSVC ; le **descripteur (valeur)** commence à
`node+0x14` :
- `node+0x0d` = `_Isnil` ; `node+0x10` = **clé / index de canal** ; `node+0x14` =
  **nom** `std::string` (SSO : buf `+0x14`, len `+0x24`, cap `+0x28`).
- Table de **filtre par type** (`accepte ce type de message ?`) accessible depuis le
  nœud lors du routage (§3.1) — offset exact à confirmer live (candidat `valeur+0xB`).
- Champs `node[0x12]..[0x15]` (couleur/flags) passés à `FUN_00a38ff0` (enregistrement
  manager) dans `Create`.
`DAT_015faae0` = nb de canaux (diviseur largeur). `DAT_015faae4` = registre des
canaux détachés.

Mutations : **ajout de canal** = fenêtre `0x1A` → `FUN_008fa640` (add au registre) ;
**suppression** = bouton 0xCA (WndProc) ; **filtres log combat** = fenêtre `0x84`.
Chargement au **login** (funcs `FUN_00a9cf70` / `FUN_00a9d460` autour du window mgr).

### 6.2 `/savechat` — `ChatLog_SaveAllToFiles` @ `0x00907030`
Pour **chaque** onglet (vector `main+0xF4`) **et chaque** fenêtre détachée (set
`DAT_0131f510`, onglet actif `wnd+0xB4`) : construit `data\Chat\Chat_<nomCanal>.txt`
(rotation `_%03d` si existe) et **écrit l'historique brut** via
`FUN_00866d20(tab, path)`. Confirme un message `"<path> is Saved."` (couleur vert
`0xff00`) via `UIWindowMgr_ChatAction`. **C'est le SEUL producteur de fichier** (pas
de log continu).

### 6.3 Positions des fenêtres
`UIWindowMgr_SaveWindowRect(mgr, id)` (ex. id `0x1A`, `0x33`) sauve la géométrie ;
restaurée à la (re)création. Cf. plugin `WindowPosTweaks` ([[project_window_position_persistence]]).

## 7. Fenêtres détachées (`UIChatWnd`, vtable `0x01037ea8`) — vérifié 2026-07-09
Classe **distincte** de `UINewChatWnd`. ctor `UIChatWnd_ctor 0x008d82a0` (géométrie
service-dépendante comme la docké) ; OnMsg = `UIChatWnd_HandleMsg 0x009020b0` (+0x94).
Tenues dans le **`std::set` global `DAT_0131f510`** (taille `DAT_0131f514` ; value @
`node+0x14`).

**Field map (vérifié via le ctor)** : `+0xB4` = **UISubChatWnd\* onglet actif** (unique)
· `+0xB8` skin/fond · `+0xBC` cadre enfant (resize) · `+0xE8/+0xEC/+0xF0` métriques
largeur-max/largeur-ligne/hauteur-réservée (service-dép. `+0xEC`=0x47/0x6e/0x5a) ·
`+0xF4` **index de canal** (clé dans le set) · `+0x114` `std::string` **caption** ·
`+0x12C` contrôle de saisie enfant.

**Cases `HandleMsg` notables** : **0x25** add-line (→ `AddLine` + `WrapAndDispatch`/`+0xE4`
sur `+0xB4`) · **3/4** scroll page (`(*(+0xB4)+0x94)(0,9|10,…)`) · **0x74** barre de
surbrillance · **0xE** resize (min `0x118×0x4A`) · **0x22** skin · **6** sous-commandes
(0xC1 ouvre la liste d'onglets id `0x84`, **0xCA = ré-indexation détach**, 0x15b, 0x165).

**Détacher / recoller** :
- **Arracher** : clic/drag sur un onglet de la docké → `UINewChatWnd_OnLButtonDown
  0x008f6c20` (hit-test barre d'onglets), bascule `DAT_015faaec`, relayout
  (`LayoutButtons`), itère `DAT_0131f510` (relayout+repaint des flottantes). Une
  nouvelle `UIChatWnd` est créée (`MakeWindow`), insérée dans `DAT_0131f510`, et
  l'onglet migre vers `newwnd+0xB4`.
- **Ré-indexer/recoller** : `HandleMsg` case 6 / sous-cmd **0xCA** — retire `this+0xF4`
  de `DAT_015faae4`, reconstruit, **parcourt `DAT_0131f510` et réassigne
  séquentiellement `*(node[5]+0xF4)=i`** (compacte les index), notifie la liste id `0x84`.
- **Show/hide** : `FUN_00902f30` propage `+0x38` aux onglets `[+0xF4,+0xF8)` puis à
  toutes les flottantes de `DAT_0131f510`.

> ⚠ **Ne PAS confondre** avec `0x008cd650` (`HandleEvent`) qui, malgré le nom Ghidra
> `UIChatWnd_HandleEvent`, est l'OnMsg du **popup « nom »** (`0x01034324`, ctor
> `0x0088f500`), pas de la fenêtre détachée.

---

## 8. Blueprint de conversion ImGui

### 8.1 Stratégie retenue : **« natif vivant mais muet + rendu direct ImGui »**
On **ne remplace PAS** la logique native (elle est trop riche : envoi réseau, parse
commandes/skills/préfixes de canal, cibles de whisper, config des canaux + fenêtre
log-combat 0x84, résolution `<ITEML>`, emoticons, codes couleur). On la garde
**vivante et alimentée**, on **supprime seulement son dessin**, et on **rend le chat
en ImGui à partir de ses propres structures de données**. La saisie ImGui est
**routée vers l'input natif** → envoi gratuit.

Pourquoi c'est le bon compromis :
- `UISubChatWnd_AddLine` remplit l'historique brut (`tab+0x100/+0x10c/+0x118`)
  **que la fenêtre soit visible ou non** (l'appel vient de `ChatAction→WndProc`, pas
  du dessin). ⚠ **à confirmer live** : une fenêtre native cachée continue-t-elle de
  recevoir msg 0x25 ? (attendu : oui, `ChatAction` dispatch sur l'objet `mgr+0x1C8`
  indépendamment du flag visible.)
- On lit donc le modèle **directement**, sans mirroring ni divergence.

### 8.2 Source de données ImGui (lecture directe, par frame)
1. Fenêtre chat = `mgr+0x1C8` (ou `g_chat_wnd` déjà caché par `chat.cc`).
2. Onglets = vector `[main+0xF4, main+0xF8)` ; nom de chaque canal via le descripteur
   `DAT_015faadc` (node+0x14), index onglet actif = `main+0x114`.
3. Par onglet : les 3 vecteurs parallèles `std::vector<std::string>` texte
   (`tab+0x100`), `std::vector<uint32_t>` couleurs (`tab+0x10C`),
   `std::vector<std::string>` senders (`tab+0x118`). Cap = `tab+0xF0`.
   (Lecture SSO MSVC déjà maîtrisée dans `chat.cc` : `ClearOneTab` lit ces offsets.)
4. Fenêtres détachées = set `DAT_0131f510` (onglet actif `wnd+0xB4`) → onglets ImGui
   flottants supplémentaires si on veut les reproduire.

> Avantage : **aucun hook nécessaire pour le modèle d'affichage**. On peut même se
> passer de mirroring : re-parser l'historique brut chaque frame (avec cache par
> compteur de lignes) suffit (quelques centaines de lignes).

### 8.3 Ce que le rendu ImGui doit répliquer (à partir du texte brut)
- **Word-wrap** à la largeur du panneau (on maîtrise — mieux que le natif : pas de
  cap 94 char, pas de reflow buggé).
- **Codes couleur `^RRGGBB`** inline → runs colorés ImGui (parser trivial, cf.
  `UIText_DrawColored`). `^000000` = reset.
- **Liens `<ITEML>…</ITEML>`** → texte cliquable = nom d'objet (résoudre via l'API
  native `FUN_006a2ce0(link, out, 0)` / `ResolveItemResNameById`) + **icône 16px**
  (réutiliser le loader déjà en place dans `chat.cc` : `BuildItemIconGrfPath` →
  texmgr → `D3D9_CreateTextureARGB`). Clic → ouvrir la desc objet (window `0xC`, cf.
  [[project_item_skill_desc_window_re]]) ou le popup nom.
- **Emoticons** `^e[..]` (token 0x236, 50×50) — réutiliser le chemin natif
  d'emoticon si besoin, sinon rendre l'atlas.
- **Préfixe `Nom: `** coloré (le sender est déjà séparé dans `tab+0x118`).
- **Timestamps** optionnels (déjà injectés par `chat.cc`, ou ajout ImGui pur).
- **Scrollback** + autoscroll + « aller en bas » + molette (le natif scrolle via
  `WndProc case 3/4` ; ImGui gère nativement).
- **Onglets** ImGui reflétant les canaux natifs (noms de `DAT_015faadc`).
- **Filtres** : réutiliser le routage natif (les lignes sont déjà réparties par
  onglet) — ou ajouter des filtres ImGui par-dessus. La fenêtre **log-combat 0x84**
  reste la source de vérité des filtres de combat.

### 8.4 Saisie & envoi ImGui
- `ImGui::InputText` (multi-ligne off). Sur **Enter** :
  - **Option A (robuste, recommandée)** : écrire le texte dans l'input natif
    `main+0xBC` (via `UIEditWnd` set-text) puis **poster la notification ENTER**
    (`WndProc(this, msg=6, ctrl=0xB8)`) → récupère commandes `/…`, skills, préfixes
    `%`/`$`/`^`, cibles whisper, historique, **gratuitement**.
  - **Option B** : `std_string_assign(DAT_0131f9c4, texte)` puis
    `g_UICommandDispatcher->vtable[6]()`.
- **Historique de saisie** (↑/↓) : natif dans `input+0x144` — piloter l'input natif
  (option A) le donne gratuitement.
- **Boutons de canal / préfixes** : petits boutons ImGui qui préfixent le texte.
- **Autocomplétion** : la sous-fenêtre native `main+0x13C` existe déjà (optionnel).

### 8.5 Masquer le natif (sans casser sa logique)
- **NE PAS** déplacer hors-écran (corrompt des coords persistées — cf.
  [[feedback_no_offscreen_hide]]).
- **Recommandé** : hooker le **dessin** de la chat native pour le rendre no-op quand
  l'ImGui chat est actif :
  - `UINewChatWnd` `OnDraw`/`Paint` (`UINewChatWnd_Paint 0x008f3340`, `Draw 0x008de120`)
    → early-return.
  - et/ou `UISubChatWnd_DrawContent 0x0085e120` / `OnDraw 0x0085f630` → early-return.
  La fenêtre reste **logiquement active** (reçoit msg 0x25, remplit l'historique,
  gère la config) mais ne peint rien. On dessine ImGui par-dessus.
- Le **curseur RO** et les hooks Present existants ([[project_ro_cursor]],
  [[project_dx7_dx9_rendering]]) restent valides.

### 8.6 Inventaire des hooks (nouveau plugin `ChatImGui` ou extension `ChatTweaks`)
| But | Cible | Type |
|---|---|---|
| Récupérer/rafraîchir le ptr chat | `UINewChatWnd_WndProc 0x008fc220` (déjà hooké) | lire `ecx` |
| Suppr. dessin natif | `UINewChatWnd_Paint 0x008f3340` (+ `UISubChatWnd_OnDraw 0x0085f630`) | early-return gardé |
| Envoi depuis ImGui | piloter input `main+0xBC` + `WndProc msg 6/0xB8` | appel |
| Icônes objet | loader déjà en place (`BuildItemIconGrfPath`…) | réutilisation |
| (optionnel) catégorie/type | `WndProc case 0x25` | lire param |

**Hooks `chat.cc` DEVENANT redondants** en mode ImGui (à retirer ou gater) :
largeur (`ChatWndProcHook`/`InputRowLayoutHook`/`BlitHook`/`CharWrapHook`),
icônes natives (`AppendLineHook`/`TextOutLowHook`/`MeasureHook`), timestamps natifs.
`chat::ClearHistory` reste utile (vide l'historique brut = vide la source ImGui).

### 8.7 Plan de migration progressif
1. **Phase 1** — rendu ImGui **read-only en parallèle** du natif (valider la lecture
   du modèle : onglets, texte, couleurs, senders, liens). Aucun masquage.
2. **Phase 2** — masquer le dessin natif (early-return), ImGui devient primaire ;
   câbler la saisie → envoi natif ; onglets + scroll + liens + icônes.
3. **Phase 3** — retirer les hooks `chat.cc` redondants ; ajouter les extras
   (fenêtres flottantes ImGui, filtres custom, recherche, copie, thèmes).

### 8.8 Points à confirmer en live (x32dbg)
- Une chat native **cachée/non-peinte** reçoit-elle toujours msg 0x25 (remplit
  l'historique) ? (attendu oui.)
- Split exact **type ↔ couleur** dans les args de `ChatAction`/msg 0x25 (§3.2.1).
- Offset exact de la **table de filtre** dans le descripteur de canal.
- Enum complet des **catégories** (jump-table `switchdataD_00c7d3d4` de
  `Chat_HandleChatMessage`).
- Enum complet des catégories entrantes (clif) + fonction réseau exacte d'envoi.
- Fonction réseau exacte d'envoi (`g_UICommandDispatcher[6]` → clif) si on veut
  l'**Option B** d'envoi.
