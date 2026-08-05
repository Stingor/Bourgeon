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

> 🔴 **L'annonce est émise DEUX fois — piège d'ingestion (mesuré le 2026-08-04).**
> Le handler d'annonce **`sub_00CB7510`** (ZC broadcast, texte à `paquet+0x10`)
> appelle `ChatAction` **trois** fois de suite sur le même texte :
>
> | adresse | appel | effet |
> |---|---|---|
> | `0x00cb7957` | `ChatAction(mgr, 3, 0, 0, 0)` | déplie la barre de saisie |
> | `0x00cb7983` | `ChatAction(mgr, 1, txt, couleur, type 0x19)` | **la ligne affichée** |
> | `0x00cb79a9` | `ChatAction(mgr, 0x13, txt, couleur, type 0x19)` | msg 0x73 → **rien** |
>
> Le natif n'affiche donc l'annonce **qu'une fois** ; tout remplaçant qui ingère
> `ChatAction` doit **ignorer l'action 0x13** (sans quoi seul le **type 25** double,
> et lui seul — symptôme observé en jeu) tout en continuant à la **bloquer**, la
> file `mgr+0x4C4` n'étant plus drainée sans chatbox native. Corollaire pour
> l'action 3 : elle est émise par des **dizaines** de handlers de paquets, la
> traduire par « donner le focus clavier » vole ses touches au joueur — c'est
> « déplier », rien de plus.

> 🔴 **Ordre des arguments de `ChatAction` — corrigé le 2026-08-04.** C'est
> `(mgr, action, texte, couleurRGB, **TYPE**, **sender**)`, et non l'inverse comme
> l'annonçait la version précédente de cette page. Preuve dans le relais vers le
> WndProc (`0x00A4B245`) : `push arg_10` → p5, `push var_94` → p4, et le
> `case 0x25` traite **p4 comme le type**, p5 comme le sender.
> ⚠ L'inversion est INVISIBLE tant que la chatbox native vit — c'est son WndProc
> qui alimente, donc dans le bon ordre. Elle ne se manifeste qu'en ingérant depuis
> `ChatAction` (fenêtre détruite), et le symptôme est déroutant : **toutes les
> lignes arrivent en type 0**, parce qu'on lit un pointeur écrêté par la
> validation du type.

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
  `0x01204a3c` **et** `0x01204068`) → `RenderLinkLine 0x00865e80` ; sinon
  `StoreLine 0x0083dd90`. Auto-scroll si vue en bas.
  🔴 **CORRECTION (05/08/2026)** : `0x01204068` n'est PAS « URL ». Son
  initialiseur CRT (`0x00475fe0`) copie le littéral depuis `0x00FD5A50`, soit
  **`<ITEML>`** (7 o, avec `</ITEML>` 8 o à `0x00FD5A58` et les variantes
  minuscules `<iteml>` à `0x00FD5A64`). Les DEUX globales testées ici sont donc
  des balises d'objet. **La chatbox native ne rend AUCUNE adresse web
  cliquable** — ni nue, ni balisée : `<URL>` (`0x0120F668`) appartient à la
  famille **RichTextBox** (`sub_A215D0` l'apparie avec `</URL>` — quêtes,
  dialogues NPC) et au garde-balises des macros, jamais au chat.
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
3. **Commande** : `ChatCmd_TryRegisteredHandler 0x00d7f1a0` (map de handlers
   hashée FNV-1a via `0x00593bf0`
   — commandes désactivables `c_SetCmdOnOffList` ; si géré → stop), sinon
   `ChatCmd_LookupSlashCommandTable 0x00d5e590` → **`SendMsg(0x2A, cmdId,
   args[3])`**. Garde-fou : les commandes 0x12/0x1A/0x37 refusent un texte
   contenant `<ITEML>`/`<ITEM>` (msgstring 0xAFC).
4. **Texte simple** : `g_ChatPendingSendText = texte` puis **`SendMsg(0x2A, 0)`**
   → `Chat_HandleChatMessage` case 0 (préfixes + mode, §3.3).
5. Filtres amont : `g_ChatWordFilterEnabled` → `Chat_ContainsForbiddenWord`
   (modale 0xE53) ; service 1 : `0x00d71ef0` (validation supplémentaire,
   msgstring 190).

> 🔴 **`ChatCmd_LookupSlashCommandTable` est `__thiscall`** (`ret 0Ch`) : tous les
> appelants chargent **`ecx = g_UIWindowContextKey 0x015FA3C0`** avant l'appel
> (voir `0x00A4769C`). `ecx` n'est lu qu'en **`0x00D60804`**, sur le chemin de
> **repli** — nom absent de la table statique → `ChatCmd_LookupDynamicCommandId
> (ecx, nom)`. Conséquence traîtresse : appelée en `__stdcall`, la pile reste
> équilibrée et les commandes CONNUES passent ; ce sont les autres qui lèvent une
> exception. Bourgeon a vécu exactement ça (« L'envoi a échoué (chemin natif) »).

### 5.1bis Le classement (`/blacksmith`, `/alchemist`, `/taekwon`) est du CHAT

`ZC_ACK_RANKING` **0x097D** (288 o) → **`ZC_AckRanking_PrintToChat 0x00caeb90`** :
le client n'ouvre AUCUNE fenêtre, il empile l'en-tête, dix lignes
`"[ %2d ] %-18s  :  %4d POINT"`, un séparateur et `MY POINT` via
`UIWindowMgr_ChatAction(action 1, **type 0**)`. Ces lignes arrivent donc dans
notre modèle comme du chat ordinaire.

Style : `RankingChatStyle_InitMap 0x00c9be10` — `map<type,{en-tête, couleur
en-tête, couleur lignes}>`, clés **0** forgeron `{0x9B9B9B, 0xCDCDCD}` · **1**
alchimiste `{0x9BCD9B, 0xCDFFCD}` · **2** taekwon `{0xCD9B9B, 0xFFCDCD}` · **3**
killer. ⚠ **Type inconnu ⇒ couleurs 0 = NOIR** : les dix lignes sont émises mais
invisibles sur fond sombre. Un classement « qui ne marche pas » peut n'être qu'un
type de rang hors table.

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

### 5.4bis 🔴 `Chat_ContainsBannedTag` est un garde de MACRO, pas de l'ENTRÉE

`ChatMacro_SendEmotionHotkeySlot` commence par
`if (g_ChatWordFilterEnabled 0x0131F6C4) { if (Chat_ContainsBannedTag(txt)) →
modale msgstring 0xE53 }`. Trois pièges, tous vérifiés :

1. **`0x00a23180` n'est PAS un filtre de gros mots** malgré son ancien nom : il ne
   lit AUCUNE liste chargée, il cherche SIX littéraux de **BALISE**. Résolus en
   remontant les initialiseurs CRT jusqu'au bloc de littéraux `0x00FD5A10` :

   | global | littéral |
   |---|---|
   | `0x0120F668` | `<URL>` |
   | `0x0120F698` | `<NAVI>` |
   | `0x0120F6F8` | `<ITEM>` |
   | `0x0120F728` | `<ITEML>` |
   | `0x0120F860` / `0x0120F890` | deux autres du même bloc |

   msgstring **0xE53** se lit « It cannot be used because it contains an Item
   Tag » — ce qui recoupe exactement ce que la fonction cherche.
2. **C'est une règle propre aux MACROS** — une macro est une chaîne enregistrée,
   elle n'a pas d'objet à désigner. L'ENTRÉE du chat (§5.1) ne l'appelle jamais :
   elle ne refuse les balises que pour les commandes slash **18/26/55**, par
   `_mbsstr` sur `<ITEML>`/`<ITEM>`, msgstring **0xAFC**.
3. **Et `NoSwearFilter` ne le désarme PAS.** Le patch WARP se contente de zéroter
   la chaîne `manner.txt` pour que la liste d'insultes ne se charge jamais ; le
   gestionnaire `0x0131F6C4` reste non nul, donc le garde s'ouvre et le test de
   balises s'exécute quand même. Croire « le filtre est patché, il ne peut pas
   venir de là » aurait fait chercher ailleurs pendant longtemps.

Bourgeon a copié son envoi depuis la fonction de MACRO — le seul chemin qui ne
dépende d'aucune fenêtre — et en a hérité le garde. Conséquence : tout message
portant un lien d'objet était refusé… **et le patch WARP était contourné**, parce
qu'on appelait `0x00a23180` en direct au lieu de passer par le code patché.

> **La leçon générale** : rejouer un garde natif en C++, c'est se priver des
> patchs posés dessus. L'IDB montre l'exe VANILLA
> (cf. `reference_ida_is_vanilla_warp_patches`). On ne recopie d'une fonction
> native que ce qui appartient au chemin qu'on remplace.

### 5.5 Fenêtre log de combat (UIBattleMsgOptionWnd, id 0x84) — ✅ RÉSOLUE
Titre « `<TabName>` message log settings » (0x6F1). **25 cases à cocher = les 25
types** (§3.1.1), état local `this+0xEC[25]`, flag « tout » `this+0x10C`, canal
ciblé `this+0x108` (+ byte `+0x110` principal/détaché).
- **msg 6/0xC1(193)** (envoyé par les chats : `(wnd, 6, 193, chanIdx,
  isMain, mode)`) : recharge les 25 octets depuis le nœud du registre.
- **Clic case** (`UIBattleMsgOptionWnd_OnClickCheckbox 0x008f6960`) : toggle et
  **écrit `node+0x2C+i`** directement.
- **msg 6/0xD5(213)** : tout cocher/décocher. **6/0xC9(201)** : fermer.

### 5.6 Maj + clic gauche = LIEN D'OBJET dans la saisie — ✅ RÉSOLU (2026-08-04)

Le geste : Maj enfoncée + clic gauche sur un objet ⇒ son nom apparaît dans la
barre de saisie ; à l'ENTRÉE, il part sous forme `<ITEML>…</ITEML>`, que tous les
clients rendent en icône + nom cliquable. **Trois liens maximum**, puis Entrée.

#### Le chemin natif (4 fonctions)

`UIInventoryWnd_OnLButtonDown 0x0094afb0` (et son jumeau chariot
`UICartWnd_OnLButtonDown 0x0094b460`) :

1. `GetAsyncKeyState(VK_SHIFT)` — c'est bien l'API Win32, pas un état interne ;
2. la fenêtre qui a le **focus** (`UIWindowMgr_GetFocusedWnd`, `mgr+0x1A0`), dont
   le **type est à `wnd+0x2C`** décide de la destination :

| type | destination |
|---|---|
| **0x1ED (493)** | la chatbox : `UIChatWnd_InsertItemLink(chat+0xBC, info)` |
| **0x1EA (490)** / **0x1EE (494)** | un edit de chat directement focus |
| 0x1EC (492) | `sub_8A0340` (autre consommateur) |
| 0x1EB (491) | `sub_7C81A0` (autre consommateur) |

`UIChatWnd_InsertItemLink 0x008217f0(edit, ItemSkillInfo*)` — `this` est **l'EDIT**,
pas la fenêtre :
- `edit+0x144` = **`UIItemTagOnChat`** (0x15C octets, ctor `0x00835aa0`, vtable
  `0x0102e354`), créé paresseusement au premier lien. C'est un **UIEdit dérivé**
  (`UIEdit_OnMsg` en `+0x94`) qui sert d'accumulateur ;
- vtbl **`+0xE8` = `0x00841b50`** décide d'accepter : **`if (count >= 3) return 0`**
  (`count` = `accessoire+0x124`), et refuse aussi si le focus n'est ni 490/494 ni
  `g_ChatInputBarDeployed 0x0131F50C` ;
- `accessoire+0x128` = texte **AFFICHÉ** (le nom, tronqué à la largeur de l'edit
  par `sub_A24EE0`, qui ajoute « … ») ;
- `accessoire+0x140` = texte **RÉSOLU** (std::string, longueur à `+0x150`) ;
- `accessoire+0x120/+0x124` = std::list de `UIItemTagButton` + compteur ;
  `sub_842610` la vide (et remet le compteur à 0).

**À l'ENTRÉE** (`WndProc` case 6/0xB8, §5.1) : si l'accessoire existe ET que
`*(accessoire+0x150) != 0`, c'est **`accessoire+0x140` qui est envoyé**, pas le
contenu de l'edit. C'est la « résolution » du lien.

#### Le format, confirmé DES DEUX CÔTÉS

`UIWnd_AppendItemLinkButton 0x00865230` forge la charge utile. Ses séparateurs
sortent d'une table locale `"!#$%&'()*+,-/"` et son encodeur est
`Base62Encode 0x00842b30` — dont la boucle `do { … } while (v >= 62)` **émet
toujours un chiffre avant le dernier**, donc jamais moins de deux caractères :
c'est exactement `string_left_pad(base62_encode(v), '0', 2)`.

Le serveur a le même code sous les yeux :
`ItemDatabase::create_item_link` (moonlight, `src/map/itemdb.cpp`), branche
`PACKETVER >= 20200724`. **Les deux tables de séparateurs coïncident case pour
case** — ce qui valide l'un par l'autre :

```
<ITEML>
  b62(equip, pad 5)          info+0x08   (masque d'emplacement)
  '1' | '0'                  info+0x00   ItemTitle_IsDecoratedType 0x006a5d70
                                         (types 4,5,8,9,11..15,21 = équipable)
  b62(nameid, pad 2)         info+0x2c   std::string, atoi
  '%' b62(refine, pad 2)     info+0x60   SI refine != 0
  '&' b62(viewID, pad 2)     info+0x70
  ''' b62(grade,  pad 2)     info+0x88   int16
  ')' b62(carte,  pad 2)     info+0x1c   × N (voir ci-dessous)
  '+' b62(optId)  ',' b62(param)  '-' b62(valeur)
                             info+0x98 = nb, info+0x9c = entrées de 5 octets
                             {int16 id, int16 valeur, uint8 param}
</ITEML>
```

⚠ **Deux écarts du CLIENT par rapport au serveur**, et ils sont dans le binaire,
pas dans la doc :
- le champ `&` (viewID) est écrit **même sur un objet non équipable** (rAthena le
  conditionne à `itemdb_isequip2`) ;
- le nombre de cartes émises est `ItemSkillDB_GetSlotCount` (0 ⇒ 4), **et non 4
  d'office** — sauf si l'un des quatre mots `info+0x1c` est non nul, auquel cas
  le client repasse à 4. C'est ce qui sauve les objets **forgés**, dont ces mêmes
  mots portent les données du forgeron et non des cartes.

Bourgeon réplique **le client** : c'est lui la référence de ce geste, c'est son
texte que reçoivent les autres joueurs.

#### Ce que Bourgeon en fait

Le chemin natif est **mort avec la chatbox** : aucun des types 0x1EA/0x1ED/0x1EE
n'existe plus, donc `PostItemLinkToChat` ne faisait plus rien — en silence, ce qui
est précisément le symptôme rapporté (« Maj+clic n'envoie pas le lien »).

- `itemcell::BuildChatLink(info, out, n)` (features/item_cell.cc) rejoue
  l'encodage ci-dessus. Il ne rend **jamais** une balise tronquée : sans
  `</ITEML>`, il rend une chaîne vide. Appeler `UIWnd_AppendItemLinkButton`
  était exclu — elle crée un `UIItemTagButton` de 0x200 octets et l'accroche à la
  fenêtre qui a le focus.
- `ChatWindow::AppendItemLink(info)` insère le **nom décoré** dans `input_` et
  garde le couple `{affiché, câble}` de côté (`item_links_`, plafond **3** comme
  le natif). `QueueSend` substitue de gauche à droite juste avant
  `Utf8ToWire` — le `<ITEML>` étant de l'ASCII pur, il traverse la conversion
  inchangé, ce que le nom qu'il remplace ne garantirait pas.
- Un lien que le joueur a effacé est simplement sauté, et `PruneItemLinks` le
  retire du compte : effacer un lien doit permettre d'en reposer un autre.
- Appelants : `InventoryViewer::LinkItemToChat` (grille d'inventaire **et** fiche
  de personnage, qui relaie ses slots équipés), et le viewer de chariot — qui
  n'avait jamais repris le geste alors que le natif l'a.

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
  `data\Chat\Chat_<canal>.txt` (rotation `_%03d`), dump ponctuel du RAW par
  `UISubChatWnd_SaveRawLinesToFile 0x00866d20` (vecteur brut `+0x88/+0x8C`,
  une ligne + `\n`, aucun horodatage ni couleur).
  🔴 **Échec silencieux** : la fonction fait `_access` puis `_mkdir` du dossier
  `<racine 0x01602AE0>\Chat` et **retourne sans rien dire si le mkdir échoue**.
  `_mkdir` n'est pas récursif : racine absente du disque ⇒ commande morte, sans
  message. Le succès, lui, s'annonce dans le chat (« … is Saved. »).
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

## 8. Blueprint de conversion ImGui — REMPLACEMENT TOTAL

### 8.1 Stratégie retenue (décision 2026-07-30) : le natif devient CODE MORT
Quand « l'interface moderne » est active, la chatbox native n'est **pas
masquée : elle n'existe plus**. `UINewChatWnd` / `UISubChatWnd` / `UIChatWnd` /
`UIBattleMsgOptionWnd` ne sont **jamais créées** ; toute la pile de rendu natif
(wrap 94c, RebuildFromHistory, TrimHistoryHalf, scroll-ctrls…) devient inerte —
la classe entière de freezes disparaît avec elle (cf. project-chat-trim-freeze).
Ce qu'on **garde** du natif, parce que c'est hors-fenêtre : le pipeline d'ENVOI
complet (`SendMsg` + `Chat_HandleChatMessage` + tables de commandes + gates
berserk/mots interdits), les **registres de canaux** et leur persistance, et les
macros EmotionHotkey.

> (Historique : l'ancienne stratégie « natif vivant mais muet » — early-return
> sur Paint/Draw, lecture des vecteurs RAW — reste documentée par le §2 et les
> vtables §2.3 ; elle peut servir de phase de validation intermédiaire, mais la
> cible est le remplacement.)

### 8.2 Ingestion : `ChatAction` N'EST PAS suffisant (démenti en jeu, 2026-08-04)

> ⚠ **Correction empirique.** La section ci-dessous présentait `ChatAction` comme
> LE chokepoint. **C'est faux** : en jeu, la fenêtre ImGui alimentée par ce seul
> détour ne recevait qu'une PARTIE des lignes que le chat natif affichait au même
> instant (5 sur ~14 à l'entrée en jeu — annonces serveur et « Party Setup »
> manquantes, alors que leurs types étaient cochés). D'autres chemins arrivent
> donc au chat sans passer par là.
>
> **Le vrai point de dépôt est le `case 0x25` du WndProc** : `UISubChatWnd_AddLine`
> (0x0083f070) n'a que **trois** appelants (`UINewChatWnd_WndProc`,
> `UIChatWnd_HandleEvent`, `UIChatWnd_HandleMsg`), tous des WndProc de chat.
> S'y brancher donne la parité par construction.
>
> **Montage retenu — deux sources, jamais en doublon** :
> · fenêtre native VIVANTE → l'ingestion vient du `case 0x25` (hook déjà posé par
>   ChatTweaks sur 0x008fc220 ; ⚠ pour ce message les paramètres sont
>   `(texte, couleur, TYPE, sender)`, type et sender INTERVERTIS par rapport à
>   ChatAction) ;
> · fenêtre native ABSENTE (la bascule de la phase 2) → le WndProc ne tourne plus,
>   et `ChatAction` reprend seul — son rôle documenté ci-dessous.
> L'arbitrage se lit dans `g_pNewChatWnd 0x0131f6b0` (nul = pas de native).
> Le détour de `ChatAction` reste indispensable pour la file `mgr+0x4C4` et pour
> le filtre de messages système.

### 8.2bis `UIWindowMgr_ChatAction` — le point d'entrée public
**⚠ Piège central** : si la fenêtre n'existe pas (`mgr+0x1C8 == 0`),
`ChatAction` action 1 **empile chaque ligne dans la file `mgr+0x4C4` sans
limite** (elle n'est drainée qu'à la création de la fenêtre). Supprimer la
fenêtre SANS intercepter `ChatAction` = fuite mémoire illimitée.
→ Le remplacement se fait donc **au niveau de `UIWindowMgr_ChatAction
0x00a4ad20`** (hook, actions 1/0x13 déviées vers notre modèle) :
- On y reçoit directement **(texte, couleurRGB, sender, TYPE)** — le type que
  l'ancienne stratégie devait aller repêcher dans le case 0x25. Enum §3.1.1.
- Extraction du sender « Nom : msg » pour les types {1,3,4,0x15} : à refaire
  chez nous (word ` :` 0x3A20, ≤24c, blanchi si == own name) — trivial.
- Broadcast : type 0x19 ou (type 0 && couleur 0xFF).
- Respecter le gate replay (`g_ReplayActive`) comme l'original.
- Actions à conserver/décider : 3 (ouvrir → focus ImGui), 6 (/savechat → notre
  export), 8/9/10 (IME, cf. 8.5), 0xE/0x9A (popup nom : natif indépendant,
  peut vivre), 2/4/5/7 (fenêtres annexes non-chat : forward inchangé).

### 8.2ter Le LIBELLÉ d'un `<ITEML>` — composé, jamais transmis

La balise ne transporte **aucun texte** : ni le nom de l'objet, ni le nom du
forgeron. Chaque client compose le libellé lui-même, et pour le composer *comme
le natif* il faut rejouer son chemin — sinon on affiche « Axe » là où il écrit
« Test's Axe ».

Recette (implémentée dans `itemcell::ParseChatLink` / `BuildChatLinkName`) :

1. **Décoder la balise** — l'exact inverse de l'encodeur documenté dans
   `item_cell.cc` : `equip`(5c) `typeDécoré`(1c) `nameid`, puis les champs
   séparés par `%`(refine) `&`(viewID) `'`(grade) `)`(carte, ×N) `+,-`(options).
2. **Fabriquer un `ItemSkillInfo`** (ctor `0x006a1b20` + `SetId 0x006a6570` +
   `EnsureLoaded 0x006a06b0`) et y **réécrire** les champs décodés : cartes
   `+0x1c..0x28`, identifié `+0x5c = 1`, refine `+0x60`, viewID `+0x70`,
   grade `+0x88`, options `+0x98/+0x9c`.
3. 🔴 **Le TYPE `+0x00` n'est PAS dans la balise** — et sans lui le nom sort NU.
   `SetId` n'écrit que l'id en texte ; le name-builder, lui, ne consulte ce champ
   qu'à travers `ItemTitle_IsDecoratedType 0x006a5d70`, qui teste l'appartenance
   à `{4,5,8,9,11,12,13,14,15,21}`. Or c'est **exactement ce booléen** que le 6ᵉ
   caractère du lien transporte (cf. `UIWnd_AppendItemLinkButton 0x00865230`) :
   écrire 4 (dedans) ou 3 (dehors) reproduit donc la décision du client sans
   avoir à deviner le vrai type.
4. **Appeler `ItemSkillInfo_BuildDisplayName 0x008a0570`**, qui enchaîne refine
   (`0x006a4600`), grade (`0x006a2ac0`), préfixes de cartes (`0x006a44b0` /
   `0x006a4310`) ou, pour un objet forgé (`ItemInfo_IsForgedOrCreated
   0x006a5e30`), force de forge + **nom du créateur** : `ItemTitle_AppendCreatorName
   0x006a4170` lit `cards[2] | cards[3] << 16` — l'id de PERSONNAGE du forgeron —
   et le résout dans `ChatItemLinkInfoCache_Get 0x005e6df0`, avec repli
   `MsgStringTable[0x245]` tant que le nom n'est pas connu.

Le même info fabriqué sert à ouvrir la **description** (fenêtre `0x0c`, msg
`0x18`) : cartes, refine et options du lien y apparaissent, alors qu'une
ouverture par id seul ne montrerait que l'item de base.

> ⚠ Le **suffixe « [N] »** (emplacements) n'est composé par le name-builder que
> si on lui passe son DERNIER argument à 1 — ce qu'aucun de nos appels ne fait.
> Il s'ajoute donc à la main, et le compte vient de la DB du client
> (`ItemSkillDB_GetSlotCount 0x006a4c10`), pas de la balise : elle ne le
> transporte pas.

### 8.2quater Champ PRIVÉ Moonlight : l'équipement cassé (séparateur `!`)

Le format officiel **ne transporte pas** le drapeau « cassé » (`ItemSkillInfo+0x5d`) :
l'encodeur natif n'écrit que refine / viewID / grade / cartes / options. Bourgeon
l'ajoute donc, **en dernier**, sous le séparateur `!`.

C'est sans danger pour un client qui l'ignore, et ce n'est pas une supposition —
le décodeur natif **`sub_0097E200`** range chaque champ dans une table indexée par
le **rang du séparateur** dans sa propre table `"!#$%&'()*+,-/"` :

- un caractère `>= '0'` est un chiffre base62 : il s'empile dans le champ courant ;
- sinon il est cherché dans la table ; **rang ≥ 14 ⇒ le caractère est purement
  ignoré**, sans rien casser ;
- `)` (rang 7) et `+` (rang 9) ont leur traitement propre (cartes, options) ; tout
  autre rang **referme le champ courant et ouvre le suivant à ce rang**.

`!` vaut le rang **0**, que le client n'écrit jamais et ne relit nulle part : la
valeur est décodée, rangée dans une case morte, et les autres champs sont intacts.
Un lien Moonlight reste donc lisible par un client vanille.

### 8.3 Supprimer la fenêtre native
- Bloquer **`UIWindowMgr_MakeWindow(mgr, 1)`** (et 0x84) quand l'interface
  moderne est active — hook du factory `0x00a39340` (ou de `UINewChatWnd_ctor`).
  Le chat est recréé par le « case 0 » (entrée en jeu) : c'est ce chemin qu'il
  faut couper.
- `ToggleWindow(mgr, 1, …)` et le focus natif (`0x00a4b760`) ne trouveront
  rien : la **touche ENTER** (donner le focus au chat) doit être réimplémentée
  côté ImGui (interception clavier avant le jeu, cf. hooks input existants).
- ✅ **Le chemin natif d'ENTRÉE est résolu** (2026-08-04) : `Game_MainWndProc`
  **`0x00DB8742`** teste `g_GameKeysEnabled 0x015FF820 != 0 && g_ChatInputHasFocus
  0x015FF828 == 0`, puis appelle **`UIWindowMgr_FocusChatInput 0x00A4BB90`**.
  Entrée n'ouvre donc le chat que si l'on n'y tape pas déjà.
  🔴 La **même paire** garde `Hotkey_DispatchShortcutSlot 0x00A321A0` (F1-F9 et la
  barre de raccourcis) : `g_ChatInputHasFocus` est le drapeau « le joueur écrit,
  les touches ne sont plus des raccourcis ». Quand NOTRE saisie a le focus, le
  poser empêcherait le client de déclencher une compétence sous la frappe — c'est
  aujourd'hui la capture clavier de l'overlay qui s'en charge, ce drapeau est le
  filet natif équivalent.
  ⚠ Ce n'est PAS le drapeau du battle mode.
- Fenêtres annexes chat : 0x84 morte (remplacée §8.6) ; 0x1A `UIComboBoxWnd`
  morte (nos combos ImGui) ; le popup nom (0xE) peut rester natif.
- 🔴🔴 **`ChatText_TransformTagLinks 0x008E1730` — CRASH SYSTÉMATIQUE.** Elle prend
  la `UINewChatWnd` pour `this` (la liste des transformateurs de balises est à
  `this+0xF4`), et ses **quinze** appelants lisent `g_pNewChatWnd 0x0131F6B0` puis
  le passent **sans le tester**. Native détruite ⇒ `this = 0` ⇒
  `mov edx,[eax+0xF4]` à **`0x008E17B3`**. Premier déclencheur : `sub_CB7510`
  (**ZC_BROADCAST2 0x01C3**, message de bienvenue) à `0x00CB7875` — donc à
  **chaque entrée en jeu**. Latents : `sub_CAFD00`, `sub_CB7A40`,
  `Actor_OnMsg_AppearanceEffects`, `QuestTracker_DrawContent`, les cinq
  `UIRichTextBox_Layout*TagLinks`.
  ⚠ La chaîne de **sortie arrive NON INITIALISÉE** : rendre la main sans rien
  faire plante le `std_string_dtor` de l'appelant, et une chaîne vide afficherait
  un message vide (l'appelant *move* le résultat dans son tampon). Correctif
  Bourgeon (`ChatTagTransformStub`) : `ecx == 0` ⇒ construire la sortie comme
  copie de l'entrée via `std_string_copy_ctor 0x004E52A0`, rendre ce pointeur ;
  sinon chaîner. Conventions vérifiées au désassemblage : `retn 8` pour l'une,
  `retn 4` pour l'autre. Le handler poursuit ensuite jusqu'à `ChatAction(…, 1,
  msg, color, 0x19)`, donc notre ingestion prend le relais normalement.
- 🔴 **Le PANNEAU DE RÉGLAGES se scinde en même temps.** Aujourd'hui les deux
  chats coexistent, donc `panel_interface.cc` affiche à la suite la section
  « Chatbox ImGui » ET celle de ChatTweaks (largeur, horodatage, icônes,
  « Couleurs du chat » — ces dernières patchent le `.text` du natif). Quand la
  native ne naît plus, TOUTE la section ChatTweaks devient inerte : il faut
  montrer l'une **OU** l'autre selon l'interrupteur `chatwnd_imgui`, jamais les
  deux. Ne pas le faire laisserait des réglages qui ne font plus rien — le pire
  des retours pour un joueur, puisque rien ne le signale.
- **Barre de saisie et `/bm` (battle mode)** — comportement à reproduire, relevé
  en jeu le 2026-08-04 :
  - `/bm ON` : la barre est **masquée**. **Entrée** l'ouvre ET lui donne le focus ;
    **Échap** la referme ; **envoyer un champ vide** la referme aussi — c'est ce
    qui permet de sortir du chat sans lâcher le clavier, en deux Entrée.
  - `/bm off` : barre **affichée en permanence**, Entrée lui donne le focus.
  - ✅ **Drapeau localisé (2026-08-04) : `g_ChatBarAlwaysVisible 0x0131F50E`.**
    🔴 **Polarité inversée, vérifiée EN JEU** : **1 = barre toujours visible**
    (normal), **0 = battle mode**. Le désassemblage seul ne tranche pas —
    `UINewChatWnd_ToggleInputBar 0x008DC0D0` ne teste `!50E` que pour *déployer*
    la barre. Basculé par la **case 135** de `Chat_HandleChatMessage` (`cmp/setz`,
    donc une vraie bascule : un argument « on »/« off » est **ignoré**) et
    persisté par `OptionInfo_SaveToFile`. Second drapeau :
    `g_ChatInputBarDeployed 0x0131F50C` = barre actuellement dépliée.
  - 🔴 `/bm` passe par **`ChatAction` action 3** = `ToggleWindow(mgr, 1)`, donc le
    client **RECRÉE sa chatbox**. La détruire après coup la laisse visible
    quelques frames : il faut bloquer l'action 3 dans le détour et traduire
    l'intention (ouvrir NOTRE saisie).
  - 🔴 Dépend de la reprise de la touche ENTRÉE (§8.3) : tant que la native vit,
    c'est SA barre qui reçoit Entrée.
- `/savechat` (`ChatLog_SaveAllToFiles`) itère `mgr+0x1C8` (null → no-op
  propre) : réimplémenter l'export depuis notre modèle si on veut le garder.
- **Bascule runtime de l'interrupteur** : même filet que make_item_window —
  ON pendant qu'une native existe → la détruire proprement via le manager
  (jamais mid-frame ImGui, cf. [[feedback_no_native_cmd_during_imgui_frame]]) ;
  OFF → laisser `MakeWindow(1)` revivre (la file `mgr+0x4C4` se draine seule).

### 8.4 Modèle de données ImGui (le nôtre, plus de lecture RAW)
- **Ring buffer** par ligne : {texte, couleurRGB, type, sender, timestamp} —
  éviction O(1), ImGuiListClipper, wrap local. Le cap devient un réglage.
- **Canaux + filtres** : notre modèle s'amorce depuis **`g_ChatChannelRegistry`
  / `g_ChatDetachedChannelRegistry`** — ✅ vérifié : les registres sont peuplés
  au boot par `ChatWndInfo_U.lua` (C-funcs Lua) **indépendamment des fenêtres**,
  et portent nom (+0x14), filtres 25 o (+0x2C) **et géométrie des détachées**
  (valeur : x/y/w/h à +0x34/+0x38/+0x3C/+0x40). Onglets ImGui dockés
  (« TabState On ») + fenêtres ImGui flottantes (« Off »).
- Le routage par canal = re-jouer `filtre[type]` chez nous (même sémantique,
  même table).
- Liens `<ITEML>` : parse chez nous → nom (`0x006a2ce0`) + icône (loader de
  chat.cc) ; clic → desc fenêtre 0xC. Codes `^RRGGBB` → runs colorés.

### 8.5 Envoi = Option B (répliquer — l'input natif n'existe plus)
- Texte simple : `std_string_assign(g_ChatPendingSendText, txt)` puis
  `CMode::SendMsg(0x2A, 0, 0, 0, 0)` — les préfixes %/$/#, le mode
  (`g_ChatInputTargetMode`) et le battle-chat sont gérés par le case 0 natif.
- Whisper : pending text + `SendMsg(11, nomCible)` ; notre propre historique de
  destinataires (l'équivalent de la box `+0xC0`).
- Commandes : `ChatCmd_LookupSlashCommandTable(txt, &id, args)` (callable
  directement) → `SendMsg(0x2A, id, args)` ; ou plus simple : TOUT envoyer via
  le même chemin que l'ENTER natif reproduit (lookup + fallback id 0).
- Word-filter : appeler `Chat_ContainsForbiddenWord` si `g_ChatWordFilterEnabled`
  pour iso-comportement (modale 0xE53).
- Mode d'envoi : lire/écrire `g_ChatInputTargetMode` + recolorer l'input ImGui
  (violet groupe / cyan guilde / violet clan — mêmes RGB que le natif §5.3).
- Macros : `ChatMacro_SendEmotionHotkeySlot` marche **sans** fenêtre (vérifié :
  ne touche que les globaux + SendMsg). Émotes : `SendMsg(0x149, "ET_x")`.
- **IME (la seule vraie perte)** : `UICandidateWnd` était pilotée par l'edit
  natif (ChatAction 8/9/10). Saisie CJK dans l'input ImGui → utiliser le
  support IME Win32 de Dear ImGui (`io.PlatformImeData`/imm32). À traiter si le
  besoin CP949 existe réellement côté joueurs.

### 8.6 Filtres & config (remplace UIBattleMsgOptionWnd)
- UI ImGui de filtres par canal = 25 checkboxes (enum §3.1.1), qui écrivent
  **directement `node+0x2C+type`** dans le registre (source de vérité conservée).
- Création/suppression/renommage de canaux = manipuler les registres (mêmes
  invariants : ≤10 canaux, index compacts) — plus aucun WndProc à poster.

### 8.7 Persistance (le dtor natif ne tournera plus)
`SaveChatWndInfo 0x008f9d00` est **indépendante des fenêtres** (elle ne lit que
les registres) mais elle était appelée par le dtor → **c'est à nous de
l'appeler** (logout/quit/changement de mode). Deux options :
- **Compat totale (recommandée)** : maintenir les registres à jour (8.4/8.6) et
  appeler `SaveChatWndInfo()` (__stdcall sans args) aux mêmes moments que le
  natif → `ChatWndInfo_U.lua` reste lisible par un client sans plugin.
- Bourgeon-only : yaml via MoonlightUi (positions des flottantes ImGui, réglages
  d'affichage — de toute façon nécessaires pour ce que le .lua ne couvre pas).
Position de la fenêtre principale ImGui : notre yaml (le `g_ChatSavedPosX/Y`
natif ne concerne que la fenêtre native).

### 8.7bis Plan de migration
1. **Phase 1 (validation) — ✅ LIVRÉE (2026-08-04), `src/features/windows/chat_window.{h,cc}`**
   Rendu ImGui read-only alimenté par un hook ChatAction *en écoute* (le natif
   continue de tourner) : valide le modèle, le parse des liens, les filtres.
   Ce qui est en place :
   - **Le détour de `ChatAction 0x00a4ad20` est posé à sa place définitive** et
     porte AUSSI le filtre de messages système historique (`kBlockedMsgs`), qui
     vivait dans `Bourgeon::Initialize` — une seule adresse, un seul stub, sinon
     le second détour posé désactive silencieusement le premier.
   - Modèle : anneau {runs analysés, texte nu, sender, couleur, type, heure},
     cap réglable 100..5000, verrou (l'ingestion vient du fil du jeu).
     Sender extrait comme le natif (` :`, ≤ 24 c) pour les types {1,3,4,0x15} ;
     `No Msg`/`NO MSG` jetés comme le natif.
   - Canaux + filtres lus dans `g_ChatChannelRegistry` /
     `g_ChatDetachedChannelRegistry` (parcours du std::map MSVC sous SEH, borné à
     10) ; repli sur un canal « Public » qui accepte tout si le registre n'est pas
     lisible. Routage = `filtre[type]`, broadcast partout.
   - Rendu : word-wrap multi-couleur maison (^RRGGBB), icônes `^i[id]`, liens
     `<ITEML>` résolus en `[Nom]` + icône, clic DROIT → description (armée hors
     frame ImGui via `itemcell::DeferDescById`). Zone de log au fond sombre : les
     couleurs du serveur sont pensées pour lui, pas pour un corps RO clair.
   - Coût par frame borné : parse fait à l'ingestion, hauteur de repli mémorisée
     par ligne (largeur + options) → une ligne hors écran ne remesure rien.
   - Réglages MoonlightUi (« Interface » → « Chat ») : clés `chatwnd_*`
     (`chat_*` reste à ChatTweaks, qui règle le chat NATIF).
   **À valider en jeu** : lecture du registre (les onglets doivent porter les vrais
   noms de canaux et respecter les filtres de la fenêtre 0x84), et le libellé des
   liens `<ITEML>`.
2. **Phase 2 (bascule) — PARTIELLEMENT LIVRÉE (2026-08-04)** : bloque
   MakeWindow(1)/0x84, ChatAction dévié, envoi Option B, ENTER-focus ImGui.
   ✅ **Fait : l'ENVOI.** `ChatWindow::QueueSend` arme, `FlushPending` (appelée par
   `Bourgeon::OnProcessInput`, **hors frame ImGui** — le pipeline natif peut ouvrir
   une modale bloquante) rejoue **à l'identique `ChatMacro_SendEmotionHotkeySlot
   0x00a47400`**, le seul chemin d'envoi du client qui ne dépende d'aucune fenêtre :
   - mots interdits : **`g_ChatWordFilterEnabled 0x0131f6c4` n'est PAS un booléen**
     mais le **pointeur du gestionnaire** de filtre, et c'est lui le `this` de
     `Chat_ContainsForbiddenWord` (`mov esi, g_ChatWordFilterEnabled` / `mov ecx,
     esi`). Nul = aucun filtre. Refus → notre chat affiche msgstring 0xE53 au lieu
     de la modale bloquante du natif.
   - `/commande` : `sub_D7F1A0` **__thiscall(`g_UIWindowContextKey`, texte)** d'abord
     (handlers désactivables ; non-nul = déjà traité, on s'arrête), puis
     `ChatCmd_LookupSlashCommandTable` **__stdcall(texte, &cmdId, std::string
     args[3])** → offset des arguments (-1 = aucun) → `SendMsg(0x2A, cmdId, args)`.
     Les 3 `std::string` sont construites vides (SSO, capacité 15) et **détruites
     par le dtor du client** (0x004f08f0) : si elles ont alloué, c'est son CRT.
   - **whisper** (vérifié au désassemblage, 0x8fdc6c) : `SetPendingSendText(texte)`
     puis **`SendMsg(11, (const char*)nomCible)`** — le nom est un `char*` en 2ᵉ
     argument, pas une std::string.
   - texte simple : `SetPendingSendText` + `SendMsg(6/66/129/289)` selon
     `g_ChatInputTargetMode`, **avec les gardes du client** (pas de groupe / pas de
     guilde / pas de clan ⇒ retombe sur le public).
   ⏳ **Reste** : empêcher `MakeWindow(1)` et 0x84 de naître, et reprendre
   l'ENTER-focus (aujourd'hui c'est encore la native qui l'a : les deux chats
   coexistent, la saisie ImGui se prend au clic).

   **Skin de la chatbox** (`ro::BeginRoChatWindow`, ui/ro_imgui) : 3ᵉ style de
   cadre, calqué sur le client — pas de barre de titre, fond translucide sombre,
   filet clair, onglets gris peints à la main, poignée `btn_resize`. Réglages
   joueur : couleurs fond/bordure/onglets, taille de texte, marges, interligne.
   **Bitmaps utilisés par la chatbox native** (relevés dans `UINewChatWnd_Create`,
   tous sous `유저인터페이스\basic_interface\`, suffixes `_a`/`_b` = normal/survol) :
   `stickoff` (détacher, msg 0x166) · `battle_option` (options de log 0xC1, et un
   2ᵉ pour « nouvel onglet » 0x176) · `wnd_mini` (fermer l'onglet 0xCA) ·
   `dialog_btn0/1/2` (bouton filtre 1 = destinataires whisper, msg 0xE1) ·
   `sys_base_off/on` (filtres 2 et 3 : hauteur d'onglets 0xD4, mode d'envoi 0xEE).
   Le fond du log et la barre d'onglets n'ont **aucune image** : rectangle ARGB
   `0x66000000` et `UITabStrip` peints en code (texte inactif ≈ `#8E938E`).
3. **Phase 3 (réorganisation)** — `chat.cc` devient **`chat_tweaks.cc`** : les
   retouches de la fenêtre native (largeur custom, icônes `^i`, timestamps,
   clear history, cache de mesure GDI, fonds) ne sont PAS retirées — elles
   deviennent **les options des joueurs qui restent en chat natif**
   (interface moderne désactivée). Deux modes exclusifs :
   - interface moderne ON → chat ImGui, natif jamais créé, hooks de
     chat_tweaks inactifs (ou inoffensifs : leurs cibles ne tournent plus) ;
   - interface moderne OFF → chat natif + tweaks, comme aujourd'hui.
   Extras côté ImGui : recherche, copie, filtres par type par-dessus le
   routage canal.

### 8.8 Anciennes questions ouvertes — TOUTES RÉSOLUES (2026-07-30)
| Question | Réponse |
|---|---|
| Chat cachée reçoit-elle msg 0x25 ? | **Oui** — aucun test de visibilité sur le chemin ChatAction → WndProc 0x25. |
| Split type↔couleur du msg 0x25 | texte=p3, **couleur=p4, type=p5**, sender=p6 ; ChatAction = (mgr, action, texte, couleur, sender, type). |
| Offset de la table de filtre | **`node+0x2C+type`** (valeur du nœud +0x18 après clé int +0x10 et nom SSO +0x14) ; 25 octets. |
| Enum des catégories | §3.1.1 (25 types + broadcast 0x19) ; côté commandes : jump-table 0..0x117 → `Chat_HandleChatMessage`. |
| Fonction réseau d'envoi | `CMode::SendMsg` cases 6/11/0x42/0x81/0x121/0xFE/0x14A → opcodes 0xF3/0x96/0x108/0x17E/0x98D/0x2DB/0xBDD (§5.2). |
| `g_UICommandDispatcher[6]` | En réalité : ENTER inline dans le WndProc → `SendMsg(0x2A, cmdId)` ; les macros hotkey passent par `ChatMacro_SendEmotionHotkeySlot`. |

---

## 9. Canaux du SERVEUR dans la combo de destination (ZC 0x0F21) — 05/08/2026

Le serveur Moonlight (rAthena) est équipé de **canaux de chat** (`#global`,
`#trade`, `#support`, plus `#map` et `#ally`). Le client, lui, n'en connaît
**aucun** : ils vivent dans `conf/channels.conf`, filtrés par `groupid`, et rien
dans le protocole vanilla ne les annonce.

### 9.1 Parler dans un canal = chuchoter à `#nom`

`clif_parse_WisMessage` (serveur, `clif.cpp`) teste le destinataire :

```c
} else if( target[0] == '#' ) {
    channel = channel_name2channel(chname,sd,3);
    if (channel && ... ) {
        if (channel_pc_haschan(sd,channel)>=0) channel_send(channel,sd,message);
        else if (channel->pass[0] == '\0') { if (channel_join(...)==0) channel_send(...); }
        ...
```

Conséquences, toutes mesurées dans cette fonction et dans
`channel_name2channel` :

- un **chuchotement** vers `#quelquechose` part au canal, pas à un joueur ;
- le joueur y est **rejoint automatiquement** si le canal n'a pas de mot de passe ;
- `#map` et `#ally` sont résolus **par gabarit** (`channel_config.map_tmpl.name`,
  `ally_tmpl.name`) : le nom est le même sur toutes les cartes et pour toutes les
  guildes, seule l'instance change.

Côté client il n'y a donc **rien à inventer** : écrire `#global` dans la box
destinataire et envoyer suffit, c'est le chemin natif `SendMsg(11)` déjà emprunté
par le chuchotement (§5.2). La combo de la barre de saisie ne fait que remplir ce
champ — elle n'ouvre aucun chemin d'envoi nouveau.

### 9.2 Le paquet — pourquoi le serveur doit parler

La seule source in-game de la liste est le texte de `@channel list`
(`channel_display_list`), qui est **localisé** (`msg_txt` 1409/1410, traduit en
français sur Moonlight) et n'arrive que si le joueur tape la commande. En
analyser la sortie serait construire une fonctionnalité sur une chaîne de
traduction. D'où un paquet, sur le patron exact de `ZC_BOURGEON_STORAGE_LIST` :
le serveur possède la liste **et ses droits**, le client dessine ce qu'il reçoit.

| Sens | Opcode | Charge |
|---|---|---|
| ZC | **0x0F21** `ZC_BOURGEON_CHANNEL_LIST` | `[type:2][len:2][count:1]` + `count * [flags:1][color:4][name:20][alias:20]` |

- `flags` bit0 = **n'existe qu'en guilde** (`#ally`), bit1 = `CHAN_OPT_CAN_CHAT`.
- `name` est **sans le `#`** : c'est ainsi que `struct Channel` le range, et
  `channel_name2channel` compare toujours sur `chname + 1`. Le client le remet.
- `color` voyage **en BGR**, parce que c'est ainsi que le serveur la STOCKE :
  `channel_read_config` applique un « RGB to BGR » à la lecture de
  `channels.conf`. Le client refait la conversion à la réception ; la lire comme
  du RGB donnerait du rouge là où la conf dit bleu.
- Envoyé **au login vérifié** (`clif_bourgeon_grant_verified`), une fois. Pas de
  renvoi : les noms sont des gabarits, et le seul élément variable — être en
  guilde ou non — est connu du client (`g_OwnGuildId 0x0159c230`), qui masque
  l'entrée `#ally` tout seul. Une guilde rejointe en cours de session fait donc
  apparaître le canal sans un paquet de plus.
- Les canaux **privés** (créés par les joueurs) sont volontairement absents : la
  combo dit « où je parle », pas « ce qui existe » — `@channel list` garde ce rôle.

### 9.3 Règles d'IHM

- Choisir un canal écrit `#nom` dans la box destinataire ; l'**aperçu** de la
  combo affiche alors le canal et non le mode. Ce n'est pas cosmétique : un
  destinataire non vide **court-circuite** le mode d'envoi (`NativeSendChatText`),
  donc afficher « Tous » pendant qu'on parle à `#map` serait un mensonge.
- Choisir un mode natif (Tous, Groupe, Guilde, Clan, Alliés) **vide** la box.
  Sans ça, le `#canal` resté en place l'emporterait sur le mode qu'on vient de
  désigner, et le message partirait au canal en silence.
- Un canal en lecture seule est **affiché quand même**, avec la mention dans son
  infobulle : le joueur doit comprendre pourquoi son message ne part pas.

---

## 10. Liens MONSTRE et liens WEB — 05/08/2026

Trois genres de fragments cliquables cohabitent maintenant dans le log et dans la
barre de saisie (`Run::LinkKind` : `kItem`, `kMob`, `kUrl`). Ils partagent tout
le mécanisme mis au point pour l'objet — zone de clic **continue** (espaces
compris, refermée au repli et au saut de ligne), couleur, curseur main, menu
contextuel commun — et ne diffèrent que par l'action.

### 10.0 La convention, et son unique implémentation

🔴 **Ces gestes ne sont pas ceux de la chatbox : ce sont ceux du CLIENT.** Ils
vivent dans `features/link_gesture.h` (namespace `links`), et la chatbox n'a plus
ni gestes ni menu à elle — elle décrit ce qu'elle montre (`links::Target`), le
module fait le reste. Ils existaient en trois exemplaires (log, barre de saisie,
table « qui le drop ») et avaient déjà divergé : le clic simple ouvrait le site
web ici et la fiche là, le Maj+clic n'existait qu'à un endroit.

| Genre | Survol | Clic gauche | Clic droit | Maj + clic |
|---|---|---|---|---|
| Objet | description simple (tooltip RO) | description (par la balise, pas par l'id) | site · alootid · `@iteminfo` · `@whodrops` | le lien dans la barre |
| Monstre | mini-fiche : sprite ANIMÉ, niveau, race, élément, PV | fiche en jeu (CZ 0x0F1F) | bestiaire du site · `@mobinfo` · `@whereis` | le lien dans la barre |
| Adresse | l'adresse ENTIÈRE | navigateur | ouvrir · **copier l'adresse** | — |

L'aperçu d'un objet est le tooltip RO déjà partagé (`itemcell::DrawTooltip`),
nourri par la BALISE : refine, cartes, grade, options et forgeron de l'objet du
posteur. Celui d'un monstre est `MonsterInfoWindow::DrawHoverPreview` — il
DEMANDE la fiche au serveur la première fois (le client n'a pas mob_db), une
seule fois par monstre, et n'ouvre ni ne change la fiche affichée : survoler
n'est pas cliquer.

🔴 **Un popup ouvert depuis une cellule de table ne s'ouvre jamais.**
`TableBeginCell` REMPLACE la pile d'ids d'ImGui (et `TableEndCell` la restaure) :
l'identifiant qu'`OpenPopup` écrit depuis une cellule n'est pas celui que
`BeginPopup` cherchera hors de la table. Même piège sous un `PushID` par ligne.
D'où le drapeau : le clic lève `*_menu_open`, l'ouverture se fait plus bas, pile
d'ids stable. Le symptôme, sinon, est un menu qui ne s'ouvre pas — sans un mot.

⚠ **Un LIEN n'est pas une CELLULE.** Sur une cellule d'inventaire, de storage ou
d'équipement, le clic gauche a déjà un métier (sélectionner, utiliser, glisser) :
la convention y reste l'inverse — droite = description. Portée choisie
explicitement. (Ctrl+clic a été envisagé pour une « description complète » puis
abandonné : il n'existe pas de seconde vue à ouvrir.)

Maj + clic sur le lien d'un objet **qu'on ne possède pas** le REPOSE dans la
barre : la balise est ré-encodée depuis le lien relu
(`itemcell::BuildChatLinkFromLink` → `BuildChatLink`, un seul encodeur), donc
refine, cartes, grade, options et forgeron survivent au relais.

### 10.1 `<MOBL>id:rang:nom</MOBL>` — balise à NOUS

`<ITEML>` est une balise du client ; celle-ci ne l'est pas, et le client n'en
fera jamais rien (cf. la correction du §4.2 : sa seule détection de lien porte
sur `<ITEML>`). Elle est donc taillée pour NOUS :

- champs séparés par `:`, **le nom en dernier** — donc libre de contenir espaces,
  apostrophes et ponctuation ;
- `rang` : 0 = normal, 1 = boss, 2 = MVP. Mêmes valeurs que la colonne `boss` de
  la table des drops, pour qu'un même monstre ne change pas d'étiquette selon
  l'endroit d'où le lien a été posé.

🔴 **Pourquoi le nom voyage.** Le client ne sait pas nommer un monstre : il n'a
pas `mob_db`, et le nom n'est même pas dans le paquet de la fiche — c'est le
serveur qui l'écrit dans `ZC_BOURGEON_MOBINFO`. Un lien réduit à l'id
obligerait **chaque** client recevant la ligne à interroger le serveur ; un lien
posté dans `#global`, et c'est toute la population qui envoie un paquet. Le nom
est donc fourni par celui qui l'affiche déjà (la fiche, la table des drops) et
transporté par la balise. Il traverse `Utf8ToWire` comme n'importe quel texte de
la ligne, puisque la substitution `libellé → balise` a lieu **avant** la
conversion (`QueueSend`).

Poseur : `ChatWindow::AppendMobLink(mob_id, rang, nom)`, même plafond de trois
liens et même mécanique `display`/`wire` que l'objet. Deux points d'entrée :

- le bouton « Lien » de l'en-tête de la **fiche de monstre** ;
- **Maj + clic gauche** sur un nom de la table « qui le drop » d'une description
  d'objet — le même geste que sur un objet, pour que le joueur n'ait pas à en
  apprendre un second. Il **désarme** le clic simple (qui ouvre le bestiaire),
  sinon Maj+clic poserait le lien *et* sortirait le navigateur par-dessus le jeu.

⚠ Dans les deux cas le nom arrive du paquet dans la **code-page du client** (il y
est recopié brut) alors que la barre de saisie est en UTF-8 : `ro::LocalToUtf8`
au passage. Sans effet sur un nom ASCII — ce qu'ils sont presque tous — et
indispensable dès qu'il ne l'est pas.

### 10.2 Adresses web — rien à transporter

Le joueur tape son URL, tout le monde reçoit exactement le même texte : nous
sommes seulement les seuls à la rendre cliquable. Aucune balise, aucune question
de compatibilité.

Détection volontairement étroite — `http://`, `https://`, `www.` — parce que tout
ce qui ressemble de loin à un domaine (« truc.fr ») transformerait la moitié des
phrases en liens. La ponctuation finale (`. , ; : ! ?`, et `)` sans `(` en
regard) est **rendue au texte** : « regarde https://moonlight-destiny.fr. »
n'ouvre pas une adresse terminée par un point. `www.` sans schéma est complété
en `https://` à l'ouverture, sans quoi le shell l'ouvrirait comme un chemin de
fichier.

⚠ L'adresse est écrite par un TIERS. Le menu offre donc « copier l'adresse » à
côté de « ouvrir » : personne ne peut juger un lien sur ce qui tient dans une
ligne de chat.
