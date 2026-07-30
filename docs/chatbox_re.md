# Chatbox RE — blueprint pour conversion ImGui totale

> Client **20250716** (Moonlight-Destiny.exe, base 0x00400000, pas d'ASLR).
> Objectif : documenter de fond en comble la chatbox native (logchat / logbattle)
> depuis le `MakeWindow` manager jusqu'aux feuilles, pour préparer une **réécriture
> ImGui complète**. Les adresses sont celles de l'IDB IDA (noms importés de Ghidra +
> renommages 2026-07-30). Cf. plugin existant `src/features/patches/chat.cc`.

Statut de rédaction : **COMPLET** (passe exhaustive 2026-07-30 : toutes les
inconnues de l'ancienne §8.8 sont résolues ; fenêtres annexes, envoi réseau,
enum des types, table de filtre, persistance et commandes slash cartographiés).

---

## 0. Vue d'ensemble — l'arbre de classes complet

| Rôle | Classe (RTTI) | vtable | ctor | OnMsg (+0x94) |
|---|---|---|---|---|
| **Chat principale DOCKÉE** (cadre + onglets + input, id **1**) | `UINewChatWnd` | **`0x01037F80`** | `0x008d7660` | WndProc `0x008fc220` |
| **Un onglet / canal** (log défilant) | `UISubChatWnd` (base **`UITextListView`**) | **`0x0102d280`** | `0x00834bf0` | `0x0085fe60` → `UITextListView_OnMsg 0x00860d50` |
| **Chat DÉTACHÉE** (onglet arraché, flottant) | `UIChatWnd` | **`0x01037ea8`** | `0x008d82a0` | `HandleMsg 0x009020b0` |
| **Options log de combat** (id **0x84**) | `UIBattleMsgOptionWnd` | **`0x01037dcc`** | `0x008d6ba0` | `0x008fa640` |
| **Dropdown générique** (id **0x1A** : cibles whisper OU mode d'envoi) | `UIComboBoxWnd` | **`0x01034b94`** | `0x0088d580` | `0x008bf500` |
| **Candidats IME** (id **0x33**, saisie CJK) | `UICandidateWnd` | **`0x0103c5f4`** | `0x00933180` | — |
| Popup **« nom cliqué »** (`ChatAction` 0xE) | `UIChatNamePopupWnd` | **`0x01034324`** | `0x0088f500` | `0x008cd650` |
| Panneau émotions cash ancré sur l'input (`main+0x13C`) | `UICashEmotionMiniWnd` | **`0x0101b974`** | `0x0078efd0` | — |
| Scroll interne d'un onglet (`tab+0xac`/`+0xb0`) | *(UIScrollText-like)* | `0x0102afd0` | — | — |
| Boîte de saisie / box destinataire whisper | `UIEditWnd` | `0x01028dac` | `0x00817690` | — |
| Poignées de resize | `UIResizer` | `0x0102a260` | `0x008184a0` | — |

> ⚠ Corrections de taxonomie confirmées : `0x01037ea8` = la fenêtre **détachée**
> (`UIChatWnd`) ; `0x01034324` = le **popup nom**, pas un chat. Et **2026-07-30** :
> la fenêtre **0x33 n'est PAS un « setup chat »** mais la fenêtre de **candidats
> IME** (`UICandidateWnd`, 9 std::string) ; `main+0x13C` n'est **pas un
> autocomplete** mais le **panneau d'émotions cash**.

### Globaux (adresses + noms IDB)

| Adresse | Nom IDB | Rôle |
|---|---|---|
| `0x015faadc` | `g_ChatChannelRegistry` | `std::map<int,{name,filtre[25]}>` des **canaux principaux** (1:1 avec le vector d'onglets `main+0xF4`). Nœud MSVC : `+0x0D` isnil, `+0x10` **index**, `+0x14` **nom** (SSO 0x18), **`+0x2C` table de filtre `BYTE[25]`** (1 octet par TYPE). |
| `0x015faae0` | `g_ChatChannelCount` | nb de canaux principaux |
| `0x015faae4` | `g_ChatDetachedChannelRegistry` | même structure pour les canaux **détachés** |
| `0x015faae8` | `g_ChatDetachedChannelCount` | nb de canaux détachés (**principaux + détachés ≤ 10**) |
| `0x015faaec` | `g_ChatCanDragWnd` | détachement autorisé (persisté `CanDragWnd`) |
| `0x0131f510` | set `std::set<UIChatWnd*>` | fenêtres détachées vivantes (value @ `node+0x14`) |
| `0x0131f6b0` | `g_pNewChatWnd` | **ptr direct vers la UINewChatWnd vivante** (utilisé par les détachées ; alternative propre au cache `ecx` de chat.cc) |
| `0x0131f8dc` | `byte_131F8DC` | 1 quand la chat existe |
| `0x0131f9c4` | `g_ChatPendingSendText` | `std::string` texte en attente d'envoi (setter `Chat_SetPendingSendText 0x00887fa0`, getter `Chat_GetPendingSendTextCStr 0x00c85700`) |
| `0x0131f518/51c/534` | `g_ChatSavedPosX/Y/g_ChatSavedTabRows` | position + rangées d'onglets persistées (restaurées par msg 0x22) |
| `0x015ff838` | `g_ChatInputTargetMode` | **mode d'envoi courant** : 0 public, 1 groupe, 2 guilde, 3 clan, 4 alliés |
| `0x015ff824` | `g_BattleChatModeOn` | toggle « battle chat » (cmd 0xBA) → l'envoi passe en chat de champ de bataille |
| `0x015ff8f4` | `g_ChatAutoSaveOn` | log continu « Chatting Auto Save » actif (cmd 0x68) |
| `0x0131f6c4` | `g_ChatWordFilterEnabled` | active `Chat_ContainsForbiddenWord 0x00a23180` (refus = modale msgstring 0xE53) |
| `0x015fb398` | `g_EmotionHotkeyMacros` | **10 macros « EmotionHotkey »** (std::string ×0x18) — envoyées par `ChatMacro_SendEmotionHotkeySlot 0x00a47400` (hotkey Alt+n), persistées en JSON (`UserSettings_SaveJson 0x0059e950` avec la WhisperBlockList) |
| `0x0131f50c/50e` | `byte_131F50C/E` | flags d'état de la barre d'input |
| `0x01204a3c` | std::string | littéral **`<ITEML>`** (test de ligne-à-liens) |

---

## 1. Création & cycle de vie

### 1.1 `UIWindowMgr_MakeWindow(mgr, 1)` → `UINewChatWnd`
Factory `UIWindowMgr_MakeWindow` (`0x00a39340`), dispatch = `switch(
g_UIWindowIdToCaseTable[windowID] @0x00a42ca8 )`, jpt `0x00a42904`. windowID 1 →
`new UINewChatWnd` (ctor `0x008d7660`), ptr caché à `mgr+0x1C8`. Fenêtres annexes :
0x1A → `UIComboBoxWnd` (0xBC o, `mgr+0x214`) ; 0x33 → `UICandidateWnd` (0x19C o,
`mgr+0x3A4`, taille service-dépendante 0x78×0x22 ou 0x124×0xD4, ancrée bas-droite) ;
0x84 → `UIBattleMsgOptionWnd` (0x118 o, `mgr+0x3EC`, 280×200).

### 1.2 `UINewChatWnd_ctor` @ `0x008d7660`
- `UIWindow_composite_ctor(this,0)` puis `*this = 0x01037F80`.
- Valeurs initiales : `+0x110 = 10` ; `+0x11c = 0x10` (hauteur bandeau onglets) ;
  `+0x120 = 0x47`/`0x6e`/`0x5a` selon `g_ServiceType` ; `+0x124 = 0x18`
  (hauteur rangée input) ; `DAT_0131f8dc = 1`.

### 1.3 `UINewChatWnd_Create(this, width, height)` @ `0x008e8fc0`
Ordre exact (⚠ corrections 2026-07-30 en gras) :

1. Nb de rangées d'onglets (`+0x30`) depuis `height` ; `+0x108 = width-0x14`.
2. `+0x138` = UITabStrip (ctor `0x837010`, 0xFC o).
3. `+0x128` = UIBitmapButton `stickoff` — **détacher l'onglet** (msg **0x166**).
4. `+0x12C` = UIBitmapButton `battle_option` (msg **0xC1**) ; `+0x100` = 2ᵉ
   `battle_option` (msg **0x176** = **créer un onglet**) ; `+0x104` = `wnd_mini`
   (msg **0xCA** = **fermer l'onglet courant**).
5. Boucle sur `g_ChatChannelRegistry` : label d'onglet (`0x864690`) +
   `new UISubChatWnd` (0x124 o) par canal → vector `+0xF4`.
6. `+0xBC` = **UIEditWnd input** (460×16, x=0x6e). `input+0x88 = 0xEA` (234,
   unités de remplissage de la barre, hardcodé — bug largeur, cf. chat.cc).
   Enregistré cible de focus (`0x00a4b760`). `+0xB4 = +0xBC` (input actif).
7. **`+0xC0` = UIEditWnd 90×16 x=6 = BOX DU DESTINATAIRE DE WHISPER** (PAS un
   « bouton envoi »). Son vector interne `+0x11C/+0x120` (stride 0x18) =
   **l'historique des noms whisper** (listé par le bouton 0xE1, entrée retirée par
   msg 0x46). `whisperbox+0x88 = 0x18`.
8. `+0x8C = 0xB8` (command-id de l'input).
9. `+0xB8` = UIResizer **largeur** mode 0 — **inopérant** (le WndProc ignore les
   modes 0/1) ; UIResizer **hauteur** mode 2 anonyme (retrouvé via la liste
   d'enfants `+0x50` par chat.cc).
10. Boucle 3 boutons filtre `+0xE0[0..2]` (`dialog_btn0/1/2`), libellés msgstring
    0xC73/0xC75/0xC74, msg ids **0xE1**, **0xD4**, **0xEE**, x hardcodés
    0x61/0x24A/0x23C (largeur de création 600).
11. `+0xD8 = 0x66000000` — couleur de fond ARGB (site patch opacité, cf. chat_bg.cc).
12. **`+0x13C` = `UICashEmotionMiniWnd`** (0xA0 o, ctor `0x0078efd0`), 0xC0×0x2C,
    ancré sur l'input, caché au départ — **panneau d'émotions cash**, pas un
    autocomplete.

### 1.4 Destruction
`UINewChatWnd_dtor` = **`0x008db7f0`** (vtable+0x00). **Appelle
`SaveChatWndInfo 0x008f9d00`** (persistance, §6) avant de libérer.

---

## 2. Modèle de données

### 2.1 `UINewChatWnd` — champs (offsets confirmés)

| Offset | Type | Rôle |
|---|---|---|
| `+0x00` | vptr | `0x01037F80` |
| `+0x14`/`+0x18` | int | largeur / hauteur |
| `+0x1C`/`+0x20` | int | position X / Y écran |
| `+0x30` | int | nb de rangées d'onglets (0 = repliée) |
| `+0x50` | std::list | enfants (sentinelle ; node+0=next, node+8=child) |
| `+0x8C` | int | = 0xB8 (cmd-id input) |
| `+0xB4` | ptr | input actif courant (= `+0xBC`) |
| `+0xB8` | UIResizer* | poignée largeur (mode 0, inopérante) |
| `+0xBC` | UIEditWnd* | **boîte de saisie** |
| `+0xC0` | UIEditWnd* | **box destinataire whisper** (+ historique de noms `+0x11C/+0x120`, index sélection `+0x128`) |
| `+0xC4`/`+0xC8` | int | métriques layout X/Y |
| `+0xCC` | int | bord droit (= width-6) |
| `+0xD0` | int | hauteur zone log |
| `+0xD8` | ARGB | couleur de fond `0x66000000` |
| `+0xE0`..`+0xE8` | ptr[3] | boutons filtre (msg 0xE1/0xD4/0xEE) |
| `+0xF0` | int | **flag : la UIComboBoxWnd ouverte cible 0=whisper / 1=mode d'envoi** (posé par cases 0xE1/0xEE, lu par msg 0x27) |
| `+0xF4`/`+0xF8`/`+0xFC` | vector | `std::vector<UISubChatWnd*>` onglets |
| `+0x100` | ptr | bouton `battle_option` #2 (msg 0x176 = nouvel onglet) |
| `+0x104` | ptr | bouton `wnd_mini` (msg 0xCA = fermer l'onglet) |
| `+0x108` | int | base ancrage droite (= width-0x14) |
| `+0x114` | int | **index onglet actif** |
| `+0x11C` | int | hauteur bandeau onglets (0x10) |
| `+0x120` | int | largeur d'onglet (service-dépendant ; recalc `tabstrip.width / g_ChatChannelCount`) |
| `+0x124` | int | hauteur rangée input (0x18) |
| `+0x128` | ptr | bouton `stickoff` (msg 0x166 = détacher) |
| `+0x12C` | ptr | bouton `battle_option` #1 (msg 0xC1 = fenêtre 0x84) |
| `+0x138` | UITabStrip* | barre d'onglets |
| `+0x13C` | UICashEmotionMiniWnd* | panneau émotions cash |

### 2.2 `UISubChatWnd` (un onglet/canal) — champs

**DEUX modèles de lignes, chacun 3 vecteurs parallèles** :
- **RAW** (`+0x100/+0x10C/+0x118`) = source de vérité, jamais wrappée (écrite par
  `AddLine`). **C'est ce qu'un viewer ImGui doit lire.**
- **DESSINÉ/wrappé** (`+0x88/+0xE4/+0xF4`) = dérivé (re-wrap à la largeur
  courante). La liste chaînée `+0xD4` = **nœuds de rendu interactifs** (boutons
  item-link), pas du texte.

| Offset | Type | Rôle |
|---|---|---|
| `+0x00` | vptr | `0x0102d280` |
| `+0x14` | int | largeur onglet (wrap coupe à `+0x14 - 0x10`) |
| `+0x18` | int | hauteur ; `lignes_visibles = +0x18 / +0xB8` |
| `+0x7C/+0x80/+0x84` | 3×byte | couleur texte défaut (B/G/R) si `+0xB4==0` |
| `+0x88/+0x8C/+0x90` | vector | texte DESSINÉ `vector<std::string>` |
| `+0x94` | int | ligne surlignée (barre de sélection) |
| `+0x98` | int | ligne du haut (top de scroll) |
| `+0x9C` | int | indent / x-scroll |
| `+0xA0` | int | largeur px max des lignes (range h-scroll) |
| `+0xA4` | flag | overflow vertical / autoscroll |
| `+0xA8` | int | base de scroll |
| `+0xAC`/`+0xB0` | ptr | scroll-ctrl A/B (vtable `0x0102afd0`, 0xB4 o, recréés par OnResize) |
| `+0xB8` | int | pitch de ligne (px/ligne, stocké) |
| `+0xC0` | ARGB | couleur barre de surbrillance |
| `+0xC8` | COLORREF | couleur ligne courante |
| `+0xCC` | byte | flag centrage |
| `+0xD0` | int | origine X |
| `+0xD4/+0xD8` | list/int | nœuds de rendu (boutons liens) / compteur |
| `+0xE4/+0xE8/+0xEC` | vector | couleurs DESSINÉES `vector<uint32_t>` |
| `+0xF0` | uint | cap d'historique (max lignes brutes) |
| `+0xF4/+0xF8/+0xFC` | vector | senders DESSINÉS |
| `+0x100/+0x104/+0x108` | vector | **RAW texte** |
| `+0x10C/+0x110/+0x114` | vector | **RAW couleurs** |
| `+0x118/+0x11C/+0x120` | vector | **RAW senders** |

- `count = (+0x104 - +0x100)/0x18` ; au cap → `TrimHistoryHalf 0x00844f50` puis
  `RebuildFromHistory 0x008642d0`.
- **Scroll-ctrl** : `+0x80` max, `+0x84` pos, `+0x88` orientation, `+0x98` total,
  `+0x9C` visibles, `+0xA0`=10 pas, `+0x58` dirty.
- **Nœud interactif** (`+0xD4`) : `+8/+0xC/+0x10` = `vector<UITextButton*>` ;
  bouton : `+0x18` Y, `+0x7C` nom, `+0x1F0` X, `+0x1F4` ligne, `+0x1F8` nameid.

### 2.3 Slots vtable utiles (byte-offsets, vérifiés 2026-07-30 par lecture vtable)

**`UINewChatWnd` `0x01037F80`** : `+0x00` dtor `0x008db7f0` · `+0x04` OnMove
`0x008f9be0` · `+0x10` OnSize · `+0x38` Show/Hide propagé `0x00902f30` · `+0x3C`
Create `0x008e8fc0` · **`+0x50` Paint `0x008f3340`** · `+0x64` OnLButtonDown
`0x008f6c20` (drag onglets/détach) · `+0x94` WndProc `0x008fc220` · `+0x98`
`UIWindow_PaintDispatch 0x00a23340` · **`+0xAC` Draw `0x008de120`**.

**`UISubChatWnd` `0x0102d280`** : `+0x04` **OnDraw `0x0085f630`** · `+0x3C`
OnResize `0x0084d310` (⚠ recrée les scroll-ctrls, fuit si rappelée) · `+0x94`
OnMsg `0x0085fe60` → **`UITextListView_OnMsg 0x00860d50`** (classe de base
générique liste-de-texte) · `+0xDC` ClearDrawnVectors `0x00842230` · **`+0xE0`
DrawContent `0x0085e120`** · `+0xE4` AppendDrawnLine `0x0083d840`.

---

## 3. Ingestion des messages (logchat / logbattle)

### 3.1 `UINewChatWnd_WndProc` case **0x25** (« add line ») — ✅ ENTIÈREMENT RÉSOLU

**Signature du message : `WndProc(this, p1, 0x25, texte=p3, couleur=p4,
TYPE=p5, sender=p6)`** (l'ambiguïté type↔couleur de l'ancienne doc est tranchée).

Flux exact :
1. `p6` (sender) copié en pile ; pour les **types {1, 3, 4, 0x15}** (= public,
   groupe, guilde, clan — les formats « Nom : msg ») le sender est **extrait du
   texte** en cherchant le mot 16-bit ` :` (`0x3A20`), longueur ≤ 24.
2. Sender comparé à `Own_GetCharName(g_UIWindowContextKey)` : si égal → blanchi
   (message « à soi »).
3. Ligne à liens ? (`0x7faf70`) sinon **splitter 94 chars** `0x00979f10`.
4. **Broadcast** si `type == 0x19` **ou** (`type == 0` et `couleur == 0xFF`).
5. **Boucle canaux principaux** (`g_ChatChannelRegistry`, index = position dans
   `main+0xF4`) : accepte si **`*(BYTE*)(node + 0x2C + type) != 0`** ou broadcast.
   → `UISubChatWnd_AddLine(tab, texte, couleur, sender)` (RAW), puis par
   sous-ligne : `UIText_MeasureWidth` → `WrapAndDispatch` ou vtbl `+0xE4` (liens).
6. **Boucle canaux détachés** (`g_ChatDetachedChannelRegistry`, même octet de
   filtre) : chaque fenêtre détachée reçoit **`SendMsg(0x25, texte, couleur,
   sender)`** (son HandleMsg fait l'AddLine local).

> **msg 0x73 est un NO-OP dans la fenêtre principale** (`if (msg==0x73) return 0`)
> → la voie `ChatAction 0x13` est **morte** sur ce client.

### 3.1.1 ✅ ENUM COMPLET DES 25 TYPES (= la table de filtre `node+0x2C+i`)

Résolu via les 25 libellés poussés par `UIBattleMsgOptionWnd_ctor 0x008d6ba0`
(ordre du vector = index de type), noms décodés depuis `msgstringtable` :

| Type | msgstr | Libellé | Notes |
|---|---|---|---|
| 0 | 0x50B | Public (« ST_CHAT ») | messages système/défaut ; broadcast si couleur 0xFF |
| 1 | 0x4FD | Public Chat | « Nom : msg », sender extrait |
| 2 | 0x4FE | Whisper | |
| 3 | 0x4FF | Party Chat | sender extrait |
| 4 | 0x500 | Guild Chat | sender extrait |
| 5 | 0xFBF | Alliance Chat | |
| 6 | 0x501 | Item get/drop | |
| 7 | 0x502 | Equipment on/off | |
| 8 | 0x503 | Abnormal status | |
| 9 | 0x504 | Party member's obtained item | |
| 10 | 0x505 | Party member's abnormal status | |
| 11 | 0x506 | Skill failure | |
| 12 | 0x507 | Party configuration | |
| 13 | 0x508 | Damaged equipment | |
| 14 | 0x533 | WOE information | |
| 15 | 0x60D | Search message for party members | |
| 16 | 0x652 | **Battle message** (mes dégâts) | le fameux type 0x10 du DPS/freeze |
| 17 | 0x653 | Party member's battle message | type 0x11 |
| 18 | 0x654 | Experience message | |
| 19 | 0x656 | Quest information | |
| 20 | 0x657 | Battlefield message | |
| 21 | 0x93D | Clan Chat | sender extrait (type 0x15) |
| 22 | 0xBA4 | Call messages | |
| 23 | 0xFBC | Repayment-exp (guilde) | |
| 24 | 0xFBE | Equip attribute changes | |
| 25 (0x19) | — | **Broadcast / annonce** | hors table : toujours affiché sur tous les onglets |

### 3.2 API `UIWindowMgr_ChatAction` @ `0x00a4ad20` — signature corrigée

**`ChatAction(mgr, action, texte, couleurRGB, sender, TYPE)`** — 6 args ; le
type est le 5ᵉ arg pile, relayé tel quel au msg 0x25. Table exacte :

| action | effet |
|---|---|
| **1** | add-line → `chatMain(mgr+0x1C8)->WndProc(0x25, texte, couleur, sender, type)`. Si la chat n'existe pas → **file `mgr+0x4C4`** (nœuds 0x24 o : `+8` texte **pré-wrappé 94c**, `+0x18` couleur), drainée à la création. Texte `"No Msg"`/`"NO MSG"` = **jeté silencieusement**. |
| **0x13** | idem mais msg 0x73 → **voie morte** (ignorée par le WndProc). |
| 2 | msg 0xF = `SetTabBarHeight(param)` |
| **3** | `ToggleWindow(mgr,1,1)` + msg 0x10 (déplie la barre si repliée) |
| 4 | fenêtre `mgr+0x1BC` ← msg 0x11 |
| 5 | fenêtre `mgr+0x27C` ← **msg 0x25** (2ᵉ fenêtre chat-like, reçoit des add-lines) |
| **6** | `/savechat` → `ChatLog_SaveAllToFiles 0x00907030` |
| 7 | si `mgr+0x27C` : `0x008897c0` |
| **8/9** | `MakeWindow(0x33)` = **UICandidateWnd (candidats IME)** + msg 0x41 (texte, p3) — PAS un « setup chat » |
| 10 | ferme la 0x33 (`SaveRectAndCloseWindow`) |
| **0xE** | popup « nom cliqué » : `UIChatNamePopupWnd` (0x11C o) 280×120, texte `"nom  (info)  *^_^*"`, enregistré dans la liste `mgr+0x17C` et le set `mgr+0x500` |
| **0x9A** | re-broadcast msg 0x9A à tous les popups du set `mgr+0x500` |

Couleur = RGB (0 → blanc).

### 3.3 `Chat_HandleChatMessage` @ `0x00c7a460` — L'EXÉCUTEUR DES COMMANDES SLASH

**Nature exacte (résolue)** : c'est le **corps des 280 commandes** (`switch`
via `byte_C7D3D4[0..0x117]` + `jpt_C7A4ED`, **157 cibles distinctes**), avec
`this = CGameMode`. On l'atteint par **`CMode::SendMsg(0x2A, cmdId, args[3])`**
(case 42 @`0x00c8c0f2`). Ce n'est **pas** le routage des messages entrants.

- **case 0 / 0x32 = TEXTE TAPÉ** (cmdId 0 = « pas une commande ») : relit
  `g_ChatPendingSendText`, gère les préfixes **`%` = groupe, `$` = guilde, `#`**
  (+ modificateurs **Ctrl = groupe, Alt = guilde, CapsLock**), puis émet selon
  `g_ChatInputTargetMode` :
  `SendMsg(6)` public · `SendMsg(0x42)` groupe · `SendMsg(0x81)` guilde ·
  `SendMsg(0x121)` clan · `SendMsg(0x14A)` alliés · `SendMsg(0xFE)` champ de
  bataille (si `g_BattleChatModeOn`).
- **Le gros des cases = émotes** : `SendMsg(0x149, "ET_*")` (~60 émotes ET_).
- Toggles `OptionInfo_Get/SetValue` (`/sound` 7, `/bgm` 0xB, `/effect`…),
  volumes (0xC/0xD), `/where`, `/who` (case 1 → SendMsg 7), etc.
- **case 0x68 = « Chatting Auto Save »** : il EXISTE un **log continu**
  (`g_ChatAutoSaveOn`, fichier `<dir>\Chat\Chat%03d.txt`, handle stocké
  `GameMode+0x5BC`) — c'est un toggle par commande. `/savechat` instantané
  (ChatAction 6) reste un dump ponctuel séparé.
- **case 0x12/0x13 = `/ex` `/in`** : paquet direct **0xCF CZ_SETTING_WHISPER_PC**
  {op, nom, type 0/1}. D'autres cases émettent des paquets directs :
  0xA68/0xA69 (0xB6-0xB8, 0xE0-0xE2), 0xA77 (0xE3), 0xA5F (0xFE), 0xA88 (0xE6),
  0xC0B (0x104).
- **default** = handler « commande inconnue » `0x0068faa0` (objet paresseux
  `dword_131EE7C`).

Extraction du mapping **nom → cmdId** (vérifiée par recoupement avec les cases) :
`/organize`=0x1A · `/expel`=0x1C · `/item /monster`=0x30 · `/mapmove /mm`=0x31 ·
`/breakguild`=0x38 · `/sm`=0x3A · `/resetstate`=0x3D · `/resetskill`=0x3E ·
`/cmt`=0x3F · `/emblem`=0x40 · `/hide`=0x58 · `/font`=0x5C · `/remove`=0x5F ·
`/shift`=0x60 · `/recall`=0x61 · `/summon`=0x62 · `/showname`=0x6C ·
`/str+ /agi+ …`=0x89 · `/check`=0x8F · `/hi`=0x97 · `/invite`=0xB8 ·
`/guildinvite /web`=0xD0 · `/navi`=0xD1 · `/navi2`=0xD2 · `/clanchat`=0xD6 ·
`/naviopen`=0xD9 · `/cashshop`=0xDA · `/goldpc`=0xDB · `/roulette`=0xDC ·
`/quake`=0xDD · `/macro_register /mr`=0xE0 · `/macro_preview /mp`=0xE1 ·
`/mineffect`=0xE5 · `/macro_detector /md /rct`=0xE6 · `/call`=0xE7 ·
`/agency`=0xED · `/changedress`=0xEF · `/showshop`=0xFA · `/prison`=0xFB ·
`/removeon /removeoff /MacroChecker /ProbabilityTest`=0xFE/0x117 …
(chaîne complète lisible dans `ChatCmd_LookupSlashCommandTable`).

---

## 4. Pipeline de rendu (inchangé, vérifié)

### 4.1 Chaîne de dessin d'un onglet
```
UISubChatWnd vtbl+0x04 OnDraw (0x0085f630)
  └─ _BgAndControls (0x0085f930)
       ├─ UIWindow_OnDraw_Base            // fond + cadre
       ├─ place scroll-ctrl A/B (+0xAC/+0xB0)
       └─ DrawContent (0x0085e120, vtbl+0xE0)
            ├─ pousse géométrie dans (+0xAC): +0x80 max, +0x84 pos, +0x98 total, +0x9C visibles
            └─ (*+0xAC +0x98)() = UIWindow_PaintDispatch (0x00a23340)
                 → chaîne d'enfants (+0x10) → painter feuille
```
Boucle par-ligne (gabarit exact = `UIScrollText_PaintLines 0x008539c0`, mêmes
offsets, mais vtables sœurs — le chat blitte via la chaîne d'enfants) :
`UIText_DrawColored 0x00a26540` (taille 0xC=12 px, gère `^RRGGBB`) → feuille GDI
`0x005471a0` (hook `TextOutLowHook` de chat.cc).

### 4.2 Wrap & lignes dessinées
- `WrapAndDispatch 0x0083d3f0` : wrap à `+0x14 - 0x10` (`ChatText_WordWrap
  0x00a23a20`) → virtuel `+0xE4` par chunk.
- `AppendDrawnLine 0x0083d840` (+0xE4) : liens (`_mbsstr` vs `<ITEML>`
  `0x01204a3c` / URL `0x01204068`) → `RenderLinkLine 0x00865e80` ; sinon
  `StoreLine 0x0083dd90`. Auto-scroll si vue en bas.
- `LayoutLinkSubline 0x0084a780` : crée les `UITextButton` (0x200 o) et remplit
  la liste `+0xD4`.
- `LayoutItemLinks 0x00849ed0` (msg 0x9A) : re-layout des liens +
  `RebuildFromHistory`.

### 4.3 Re-wrap / clear (idempotents)
- `RebuildFromHistory 0x008642d0` : clear dessiné + re-wrap tout le RAW.
  Déclencheurs : TrimHistoryHalf, resize onglet, LayoutItemLinks.
- Deux clears : virtuel `+0xDC` (`0x00842230`) vide les 3 vecteurs DESSINÉS ;
  `ClearDrawnLines 0x00844d90` détruit les nœuds de `+0xD4`.

---

## 5. Saisie & envoi — ✅ CHEMIN COMPLET RÉSOLU

### 5.1 ENTER (`WndProc case 6`, sous-case `0xB8`)
1. Texte lu par `UIEdit_GetTextPtr(input +0xBC)`, coupé au premier `\n` ;
   complétion éventuelle depuis le sous-objet `input+0x144` (sélection à +320).
2. **Whisper** : si la **box destinataire `+0xC0` est non vide** et que le texte
   ne commence pas par `/` → `g_ChatPendingSendText = texte` puis
   **`CMode::SendMsg(11, nomCible)`** (le nom est mémorisé dans l'historique
   whisper `+0x11C`).
3. **Commande** : `0x00d7f1a0` (map de handlers hashée FNV-1a via `0x00593bf0`
   — commandes désactivables `c_SetCmdOnOffList` ; si géré → stop), sinon
   `ChatCmd_LookupSlashCommandTable 0x00d5e590` → **`SendMsg(0x2A, cmdId,
   args[3])`**. Garde-fou : les commandes 0x12/0x1A/0x37 refusent un texte
   contenant `<ITEML>`/`<ITEM>` (msgstring 0xAFC).
4. **Texte simple** : `g_ChatPendingSendText = texte` puis **`SendMsg(0x2A, 0)`**
   → `Chat_HandleChatMessage` case 0 (préfixes + mode, §3.3).
5. Filtres amont : `g_ChatWordFilterEnabled` → `Chat_ContainsForbiddenWord`
   (modale 0xE53) ; service 1 : `0x00d71ef0` (validation supplémentaire,
   msgstring 190).

### 5.2 `CMode::SendMsg` (0x00c86740) — cases chat → paquets réseau

Tous les envois texte passent par `Chat_GetPendingSendTextCStr`, subissent le
**gate Berserk** (`Status_IsBerserkActive` → abandon) et le filtre `0x00a85be0`
(mots interdits, manager `0x0159c2c8`), puis formatent `"Nom : texte"` :

| SendMsg | Bloc | Opcode | Paquet |
|---|---|---|---|
| **6** (public) | `0xc8a7f4` → send `0xc8b17f` | **0xF3** | `CZ_REQUEST_CHAT` {op, len, "Nom : txt"} |
| **11 / 7** (whisper) | `0xc8bdf8` | **0x96** | `CZ_WHISPER` {op, len, cible[24], txt} |
| **0x42** (groupe) | `0xc8b868` | **0x108** | `CZ_REQUEST_CHAT_PARTY` |
| **0x81** (guilde) | `0xc8b5d4` | **0x17E** | `CZ_GUILD_CHAT` |
| **0x121** (clan) | `0xc91e30` | **0x98D** | `CZ_CLAN_CHAT` (gate `byte(dword_159C07C+0x5C)`) |
| **0xFE** (champ de bataille) | `0xc8a60f` | **0x2DB** | `CZ_BATTLEFIELD_CHAT` (si `g_BattleChatModeOn`) |
| **0x14A** (alliés) | `0xc8b194` | **0xBDD** | {op, len, txt} variable (gate guilde) |
| **0x149** (émote) | `0xc8833e` | — | route ET_* vers le système cash-emotion |
| **0x2A** | `0xc8c0f2` | — | `Chat_HandleChatMessage(mode, cmdId, args)` |

### 5.3 Sélecteur de mode & liste whisper (UIComboBoxWnd, fenêtre 0x1A)
- Bouton **0xE1** (filtre 1) : ouvre la **liste des destinataires whisper**
  (historique de la box `+0xC0` + son propre nom), `main+0xF0 = 0`.
- Bouton **0xEE** (filtre 3) : ouvre la **liste des modes d'envoi** — msgstrings
  **0x55 « Send to All », 0x56 « Send to Party », 0x1B5 « Send to Guild »,
  0x939 « Send to Clan », 0xFC0 « Sent to Allies »** — présélection
  `g_ChatInputTargetMode`, `main+0xF0 = 1`.
- Sélection → **msg 0x27** : selon `+0xF0`, remplit la box whisper OU pose
  `g_ChatInputTargetMode` et **recolore le texte de l'input** (`0x00830890`) :
  mode 1 violet (128,0,128), 2 cyan (0,128,128), 4 violet (148,0,211).
- Bouton **0xD4** (filtre 2) : re-dispatch msg 2 = cycle hauteur de la barre
  d'onglets. **0xCA** : ferme l'onglet courant (réindexe, notifie la 0x84).
  **0x176** : crée un onglet `NewTab_N` (refus modale 0x62A si 10 canaux, refus
  msgstring 0x6F0 si plus de place en largeur).
- Msg **0x46** : retire un nom de l'historique whisper.

### 5.4 Macros EmotionHotkey
`ChatMacro_SendEmotionHotkeySlot 0x00a47400(slot)` (via
`UIWindowMgr_DispatchHotkeyBehavior`, chunk `0x00a471c0`) envoie
`g_EmotionHotkeyMacros[slot]` par le **même pipeline complet** (word-filter,
`/commandes`, modes). Les 10 textes sont édités dans la fenêtre de raccourcis
(`EmotionHotkey_SaveFromEditBoxes 0x0086b430`) et persistés en **JSON** avec la
WhisperBlockList (`UserSettings_SaveJson 0x0059e950`).

### 5.5 Fenêtre log de combat (UIBattleMsgOptionWnd, id 0x84) — ✅ RÉSOLUE
Titre « `<TabName>` message log settings » (0x6F1). **25 cases à cocher = les 25
types** (§3.1.1), état local `this+0xEC[25]`, flag « tout » `this+0x10C`, canal
ciblé `this+0x108` (+ byte `+0x110` principal/détaché).
- **msg 6/0xC1(193)** (envoyé par les chats : `(wnd, 6, 193, chanIdx,
  isMain, mode)`) : recharge les 25 octets depuis le nœud du registre.
- **Clic case** (`UIBattleMsgOptionWnd_OnClickCheckbox 0x008f6960`) : toggle et
  **écrit `node+0x2C+i`** directement.
- **msg 6/0xD5(213)** : tout cocher/décocher. **6/0xC9(201)** : fermer.

---

## 6. Persistance — ✅ RÉSOLUE

### 6.1 `SaveData\ChatWndInfo_U.lua` (canaux + filtres + géométrie des détachées)
**Écrit par `SaveChatWndInfo 0x008f9d00`, appelé par `UINewChatWnd_dtor`**
(logout / changement de mode / quit) :
```lua
CanDragWnd = 1
ChatSubWnd_1 = {
    XPos = 0, YPos = 0,          -- 0 pour les onglets dockés
    Width = 0, Height = 0,
    TabState = "On",             -- "On" = docké, "Off" = détaché
    TabName = [[Public]],
    option1 = 1, ... option25 = 1,   -- la table de filtre node+0x2C
}
```
**Relu au chargement en EXÉCUTANT le .lua** : l'état Lua du client
(`0x00a9bc00`) enregistre **`SetSubChatWndList`** (`Lua_SetSubChatWndList
0x00a9cf70` — recrée canal/onglet/détachée, **refuse au-delà de 10 canaux**) et
**`SetSubChatWndOption`** (`Lua_SetSubChatWndOption 0x00a9d460` — pose l'octet
`node+0x2C+(i-1)`, routage « On »/« Off » → registre principal/détaché).

### 6.2 Position de la fenêtre principale
`g_ChatSavedPosX/Y` + `g_ChatSavedTabRows`, restaurés par **msg 0x22** (avec
clamp écran). Sauvés par le mécanisme standard `UIWindowMgr_SaveWindowRect`.

### 6.3 Fichiers de log
- `/savechat` (ChatAction 6) → `ChatLog_SaveAllToFiles 0x00907030` :
  `data\Chat\Chat_<canal>.txt` (rotation `_%03d`), dump ponctuel du RAW.
- Commande 0x68 → **log continu** `Chat%03d.txt` tant que `g_ChatAutoSaveOn`.
- JSON settings (`UserSettings_SaveJson`) : WhisperBlockList + EmotionHotkey +
  hotkeys — rien d'autre du chat.

---

## 7. Fenêtres détachées (`UIChatWnd`, vtable `0x01037ea8`)

Tenues dans le set `0x0131f510` ; champ map : `+0xB4` = onglet actif
(UISubChatWnd unique) · `+0xB8` skin · `+0xBC` grip resize · `+0xE8/+0xEC/+0xF0`
métriques · `+0xF4` index de canal · `+0x114` caption · `+0x12C` edit de renommage.

**`UIChatWnd_HandleMsg 0x009020b0`** (compléments 2026-07-30) :
- **case 0** : ENTER dans l'edit caption = **renommage inline du canal**
  (borne la largeur, modale 0x629 ; maj du registre ; refocus sur l'input du
  chat principal via `g_pNewChatWnd` ; notifie la fenêtre 0x84).
- **case 0x25** : `AddLine(+0xB4, texte, couleur=p4, sender=p5)` puis `<ITEML>` ?
  → vtbl `+0xE4` : sinon `WrapAndDispatch`.
- **case 0xE (resize)** : mode 7 uniquement, **snap par pas de 32 px**, largeur
  ∈ [280..512], hauteur ∈ [74..384] ; grip repositionné (w-15, h-15).
- **case 6** : 0xC1 → fenêtre 0x84 ciblée sur ce canal ; **0xCA** → ré-indexation
  (recollage) ; **0x165** → bouton stick = forward **msg 0x76** à `g_pNewChatWnd`.
- case 3/4 : scroll page via l'onglet ; 0x74 : barre de surbrillance ; 0x22 : skin.

**Détacher / recoller** (côté principale) : **msg 0x75** = arracher (gate
`g_ChatCanDragWnd`, crée la UIChatWnd via `0x00a38ff0`, transfère l'onglet dans
`+0xB4`, réindexe) ; **msg 0x76** = recoller (gate « il reste de la place en
largeur », sinon msgstring 0x6F0). Show/hide global : `0x00902f30` propage aux
onglets + à toutes les flottantes.

---

## 8. Blueprint de conversion ImGui

### 8.1 Stratégie retenue : « natif vivant mais MUET + rendu direct ImGui »
On garde toute la logique native (routage, envoi réseau, commandes, filtres,
liens, config) **vivante et alimentée**, on supprime **seulement son dessin**,
et on rend en ImGui à partir de ses propres structures.

- ✅ **Confirmé statiquement** : ni `ChatAction` ni le case 0x25 ne testent la
  visibilité — une fenêtre non peinte **continue de recevoir les lignes** et de
  remplir l'historique brut.

### 8.2 Source de données ImGui (lecture directe, par frame)
1. Fenêtre = `mgr+0x1C8` **ou le global `g_pNewChatWnd 0x0131f6b0`** (plus
   besoin de pêcher `ecx` dans un hook).
2. Onglets = vector `[main+0xF4, main+0xF8)` ; noms + **filtres 25 octets** dans
   `g_ChatChannelRegistry` (`node+0x14` nom, `node+0x2C` filtres) ; onglet actif
   `main+0x114`.
3. Par onglet : RAW texte `+0x100` / couleurs `+0x10C` / senders `+0x118`,
   cap `+0xF0`.
4. Détachées : set `0x0131f510` (onglet `wnd+0xB4`, caption `+0x114`, index
   `+0xF4`).
5. **Catégorie/type par ligne** : le natif ne stocke PAS le type dans l'onglet
   (il route puis l'oublie). Pour des filtres ImGui par type : hooker
   `WndProc case 0x25` (p5 = type) et mirrorer (type, index-ligne) — hook léger,
   ou re-router soi-même en lisant les 25 octets de filtre par canal.

### 8.3 Rendu ImGui — à répliquer depuis le RAW
- Word-wrap ImGui (mieux que le splitter 94c natif).
- Codes `^RRGGBB` inline → runs colorés (cf. `UIText_DrawColored`).
- Liens `<ITEML>…</ITEML>` → nom (API native `0x006a2ce0`) + icône
  (loader déjà en place dans chat.cc) ; clic → desc fenêtre 0xC.
- Sender déjà séparé (`+0x118`) ; couleur par ligne (`+0x10C`).
- Timestamps : déjà injectés dans le RAW par chat.cc (ou ajout ImGui pur).
- Scrollback/autoscroll : ImGuiListClipper + éviction O(1) → supprime la classe
  entière de freezes (cf. project-chat-trim-freeze).

### 8.4 Saisie & envoi ImGui — options classées
- **Option A (recommandée, robuste)** : écrire dans l'input natif `main+0xBC`
  puis poster `WndProc(msg 6, ctrl 0xB8)` → commandes, whisper, préfixes,
  filtres, modes, macros : tout gratuit.
- **Option B (répliquer l'envoi, désormais entièrement documentée)** :
  - texte simple : `std_string_assign(g_ChatPendingSendText, txt)` puis
    `CMode::SendMsg(0x2A, 0, 0, 0, 0)` (préfixes %/$/# gérés par le case 0) ;
  - whisper : pending text + `SendMsg(11, nomCible)` ;
  - commande : `ChatCmd_LookupSlashCommandTable(txt, &id, args)` (callable
    directement) puis `SendMsg(0x2A, id, args)` ;
  - ⚠ passer par `Chat_ContainsForbiddenWord` si on veut le même comportement.
- **Mode d'envoi** : lire/écrire `g_ChatInputTargetMode` (recolorer l'input
  ImGui pareil : violet groupe, cyan guilde…). Whisper-cible : box `+0xC0`.
- Émotes/macros : `SendMsg(0x149, "ET_x")` / `ChatMacro_SendEmotionHotkeySlot`.

### 8.5 Masquer le natif (sans casser la logique)
Cibles **vérifiées dans les vtables** (early-return gardé) :
- `UINewChatWnd_Paint 0x008f3340` (vtbl+0x50) et/ou `UINewChatWnd_Draw
  0x008de120` (vtbl+0xAC) ;
- `UISubChatWnd_OnDraw 0x0085f630` (vtbl+0x04) / `UISubChatWnd_DrawContent
  0x0085e120` (vtbl+0xE0).
- NE PAS déplacer hors-écran ([[feedback_no_offscreen_hide]]) ; le masquage
  natif `vtbl+0x38` (`0x00902f30`) cache aussi les détachées si besoin.

### 8.6 Config / fenêtres annexes en mode ImGui
- **Filtres par onglet** : plus besoin de la fenêtre 0x84 — écrire directement
  les octets `node+0x2C+type` (persistés automatiquement par le dtor natif via
  ChatWndInfo_U.lua). L'UI ImGui peut réutiliser l'enum §3.1.1.
- **Canaux** : création/suppression = répliquer les cases 0x176/0xCA (ou les
  poster au WndProc). Renommage détachée = case 0 du HandleMsg.
- **UICandidateWnd (IME)** : la laisser vivre (saisie CJK) tant que l'input
  natif est piloté (Option A) ; en Option B il faudra ImGui IME.

### 8.7 Plan de migration progressif
1. **Phase 1** — rendu ImGui read-only en parallèle (valider lecture RAW +
   onglets + liens). Aucun masquage.
2. **Phase 2** — early-return sur les 2 Paint/Draw ; saisie ImGui → Option A ;
   onglets + scroll + liens + icônes + modes d'envoi.
3. **Phase 3** — retirer les hooks chat.cc devenus redondants (largeur, icônes
   natives, timestamps natifs — `chat::ClearHistory` reste) ; extras : fenêtres
   flottantes ImGui, filtres par type, recherche, copie.

### 8.8 Anciennes questions ouvertes — TOUTES RÉSOLUES (2026-07-30)
| Question | Réponse |
|---|---|
| Chat cachée reçoit-elle msg 0x25 ? | **Oui** — aucun test de visibilité sur le chemin ChatAction → WndProc 0x25. |
| Split type↔couleur du msg 0x25 | texte=p3, **couleur=p4, type=p5**, sender=p6 ; ChatAction = (mgr, action, texte, couleur, sender, type). |
| Offset de la table de filtre | **`node+0x2C+type`** (valeur du nœud +0x18 après clé int +0x10 et nom SSO +0x14) ; 25 octets. |
| Enum des catégories | §3.1.1 (25 types + broadcast 0x19) ; côté commandes : jump-table 0..0x117 → `Chat_HandleChatMessage`. |
| Fonction réseau d'envoi | `CMode::SendMsg` cases 6/11/0x42/0x81/0x121/0xFE/0x14A → opcodes 0xF3/0x96/0x108/0x17E/0x98D/0x2DB/0xBDD (§5.2). |
| `g_UICommandDispatcher[6]` | En réalité : ENTER inline dans le WndProc → `SendMsg(0x2A, cmdId)` ; les macros hotkey passent par `ChatMacro_SendEmotionHotkeySlot`. |
