# RE — Région login-connect : login RO sans frappes + masquage `UILoginWnd`

Client `20250716`, base `0x00400000`, **pas d'ASLR** (adresses Ghidra == live).
Résultat d'un workflow multi-agent (4 finders Ghidra → synthèse → 2 vérifs
adversariales). **Les corrections de la vérif sont intégrées ci-dessous** (un
typedef du plan initial crashait — voir §5). Objectif : que `MoonlightAuth`
pilote le login RO **sans frappes clavier synthétiques**, et masque la fenêtre de
login native tant que « login classique » n'est pas choisi.

---

## 1. Approche retenue : SetText des `CUIEdit` + `OnMsg(0xBA)` natif

On ne simule aucune touche. On **écrit le texte** dans les deux champs natifs puis
on **déclenche le handler du bouton Start** (comme un clic). Le handler natif
route lui-même ID/mot de passe, ouvre la socket, envoie `CA_LOGIN`, enchaîne
char-server → char-select. **Zéro dépendance à la région non-analysée `0x00d24080`.**

### Constantes / adresses
```cpp
constexpr uintptr_t g_pCurrentMode  = 0x0121333C; // void** -> CMode actif (CLoginMode au login)
constexpr uintptr_t vtbl_CLoginMode = 0x010932F0; // garde de mode
constexpr uintptr_t g_pLoginWnd     = 0x0131F6B4; // == mgr+0x1cc : cache UILoginWnd*
constexpr uintptr_t g_UIWindowMgr   = 0x0131F4E8;
constexpr uintptr_t g_LoginAccountId= 0x015FFC81; // 24 o (rempli par OnMsg)
constexpr uintptr_t ACCT_CLASS_NORMAL = 0x01031264;
constexpr uintptr_t g_SocketFd      = 0x015C5A24; // -1 fermé / handle connecté (sonde de progression)

using SetText_t = void (__thiscall*)(void* edit, const char* text); // 0x008303F0  (confirmer RET 4)
using GetLen_t  = int  (__thiscall*)(void* edit);                   // 0x008210D0
// ⚠ 6 ARGS PILE (RET 0x18) — 5 args = corruption ESP +4 = CRASH (bug attrapé par la vérif) :
using OnMsg_t   = int  (__thiscall*)(void*,int,int,int,int,int,int); // 0x008848D0
using FindWnd_t = void*(__thiscall*)(void* mgr, int windowId);      // 0x00A47B90
```

### Champs `UILoginWnd` (confirmés au désasm)
| Offset | Contenu |
|--------|---------|
| `+0xB4` | édit **ID** (non masqué) |
| `+0xB8` | édit **MOT DE PASSE** (masque `+0x84 == 0x2a`) |
| `+0xBC` | **fond** `bg_login.tga` = fenêtre id `0x145`, **NON enfant** (rendu indépendant → masquer à part) |
| `+0xE8` | combo compte (enfant, caché avec le parent) |
| `+0xEC` | classe de compte (`0x01031264` normal / `0x010315a8` alt) |

### Recette (thread jeu, hook rendu-login `OnRenderLoginUI`, **une seule fois**)
```cpp
if (IsMapLoading()) return;
void* mode = *(void**)g_pCurrentMode;
if (!mode || *(uintptr_t*)mode != vtbl_CLoginMode) return;   // pas au login
void* login = *(void**)g_pLoginWnd;                          // FindWindow(mgr,3) équivalent
if (!login) return;

// Discriminer ID/PW par le masque (robuste) :
void* eA = *(void**)((char*)login + 0xB4);
void* eB = *(void**)((char*)login + 0xB8);
bool bMasked = (*(uint32_t*)((char*)eB + 0x84) == 0x2A);
void* idEdit = bMasked ? eA : eB;   // ce build: id=+0xB4
void* pwEdit = bMasked ? eB : eA;   // ce build: pw=+0xB8

((SetText_t)0x008303F0)(idEdit, accountId);       // userid
((SetText_t)0x008303F0)(pwEdit, passwordOrOtp);   // OTP moonlight (plaintext)

*(void**)((char*)login + 0xEC) = (void*)ACCT_CLASS_NORMAL;   // évite +0xEC non initialisé
((OnMsg_t)0x008848D0)(login, 0, /*msg*/6, /*cmd*/0xBA, 0, 0, 0);  // 7 args (6 pile) !
```
Après retour : `mode+0xc=4` (pending connect) → `CLoginMode_OnStateEnter 0x00d24080`
ouvre la socket (`Network_ConfigureServerConnection 0x00d1f710` → `NetConnection_Connect
0x00c13fc0`), envoie `CA_LOGIN` (depuis `mode+0x137`=PW / `mode+0x177`=ID), puis
`LoginCharMode_RecvDispatch 0x00d27560` → `Net_OnAcceptLogin_ParseAccount 0x00d1ea00`
remplit AID/login_id1/2/sex et enchaîne char-server automatiquement.
**Sonde de fin** : `*(int*)g_SocketFd` passe de `-1` à un handle.

### ⚠ Garde-fous obligatoires (vérif adversariale)
1. **Edge-trigger** : armer un `bool fired_` au 1er tir. Sans ça, appelé chaque
   frame → repose `mode+0xc=4` en boucle → reconnexions en rafale, login jamais
   finalisé. Ne re-déclencher qu'après un nouveau retour à l'écran de login.
2. **Hook = `OnRenderLoginUI`** (celui qui tourne à l'écran de login, comme
   `login_parade`), **PAS `OnTick`** (gaté « en jeu »).
3. **`OnMsg_t` = 6 args pile** (RET 0x18). La version 5 args crashe.
4. **`SetText` → confirmer `RET 4`** avant de figer le typedef (prudence ABI).

---

## 2. Masquage de `UILoginWnd` (réversible, verdict vérif = VALIDE)

Drapeau natif `UIWindow+0x28` (`1`=visible, `0`=caché **rendu ET hit-test**).
**Jamais** hors-écran. Au login, les ops de liste du SetVisible « riche » sont des
no-op (OptionInfo pas chargé) → l'écriture directe de `+0x28` **suffit** et
équivaut au chemin natif.

```cpp
inline void SetWndVisible(void* w, int v){ if (w) *(int*)((char*)w + 0x28) = v; }

// Gater sur "encore au login" pour éviter un UAF (voir garde ci-dessous).
void MaskLoginWindow(bool hide) {
  void* login = *(void**)g_pLoginWnd;          // relire FRAIS chaque frame, jamais cacher le ptr
  if (!login) return;
  int v = hide ? 0 : 1;
  SetWndVisible(login, v);                                    // fenêtre id 3 + tous ses enfants
  SetWndVisible(*(void**)((char*)login + 0xBC), v);          // FOND id 0x145 (indépendant, obligatoire)
}
```

### Garde-fous (vérif)
- **UAF** : après login réussi le mode détruit la fenêtre id 3. Gater TOUTE la
  boucle de masquage sur `mode && *mode==vtbl_CLoginMode && !IsMapLoading()`, et
  **arrêter** dès qu'on quitte le mode login. Relire `*(g_pLoginWnd)` frais chaque
  frame (ne jamais mettre le pointeur en cache côté plugin).
- **Ré-appliquer chaque frame** tant que le formulaire moderne est actif (le mode
  peut reconstruire l'écran → recréer des fenêtres visibles). Coût nul.
- **Toggle atomique** vers « login classique » : flip du bool `hide` **puis** un
  seul `MaskLoginWindow(false)`, et **arrêter** le writer par tick — sinon il
  court-circuite le re-show au frame suivant.
- **Combo compte** (`login+0xE8`, index 0x3a) = enfant → déjà caché. **Server-select
  (id 0x1A)** ne s'ouvre que sur déroulé manuel du combo → jamais en login
  programmatique.
- **Non masquées** (top-level séparés) : les **message boxes** natives (échec auth
  serveur, `FUN_00a31a30`) et la **fenêtre d'attente/connexion (id 6)** pendant le
  connect. Avec des creds valides aucune n'apparaît ; sinon feedback gratuit. À
  décider : masquer aussi id 6 ou le laisser (progression).

---

## 3. Voie SSO (0x0825) — secondaire, à valider en live

Pour un vrai **token** (pas un OTP-mot-de-passe), le canal correct est
`CA_SSO_LOGIN_REQ 0x0825`, pas `CA_LOGIN`. Le launcher passe déjà `-t:<token>`.
Recette : poser `g_HasLoginToken 0x015ffa8c=1` + `g_LoginToken_500 0x015ffa8d` +
`g_LoginAccountId 0x015ffc81` + sexe `0x015ffc99`, puis
`CRagConnection_OnDisconnect 0x00c14320` sur le singleton `0x00c14d60`, puis
`SendMsg(mode, 10000, …)`. **Inconnues à lever (live-RE)** : sémantique
consume-once du flag (écrit `@0x00d24a73`), struct `0x0825`, ouverture socket.
→ Notre design **OTP** (mot de passe MD5 à usage unique) reste sur la voie (a)
**classique** : `CA_LOGIN` plaintext, le serveur `use_MD5_passwords` MD5 l'OTP →
match. Cohérent avec [login_auth_imgui_design.md](login_auth_imgui_design.md).

---

## 4. Correction ABI/offsets (vérif — le plan initial se trompait)

- **`OnMsg` = 6 args pile (RET 0x18)**, pas 5 → sinon crash ESP. **Corrigé ci-dessus.**
- **`mode+0x137 = MOT DE PASSE`, `mode+0x177 = ID`** (le plan initial les inversait).
  `SendMsg 0x2717 → +0x137 (PW)` ; `0x2718 → +0x177 (ID)` + copie `0x015fab93`.
- **`0x015fab93` = copie secondaire de l'ID** (pas du password) → le renommer
  `g_LoginAccountId_secondary`.
- **Prose** : le combo compte est à `login+0xE8` (index dword `0x3a`), pas `login+0x3a`.

---

## 5. Reste en live-RE (x32dbg attaché à Moonlight-Destiny.exe, pas d'anti-debug)

- **A. Valider (a)** : `bp 0x008848d0` (entrée case 0xBA) + `bp 0x00c14920`
  (SendPacket : lire opcode avant XOR, confirmer aucune frappe injectée, socket
  `0x015c5a24` −1→handle). **Vérifier le `RET imm` d'OnMsg** (aurait attrapé le bug).
- **B. Contrat OTP/password** : `bp` builder CA_LOGIN (`0x00d2586x`), capturer les
  **55 octets de `0x0064`** — confirmer que le client envoie le mot de passe **en
  clair** (sinon un OTP déjà haché serait re-haché → échec). Confirmer aussi si
  `OnStateEnter(4)` lit `mode+0x137/0x177` ou relit les édits.
- **C. SSO** (si un jour token) : `bp 0x00d24a6a` (lecture flag), `0x00d24a73`
  (consume-once ?), struct `0x0825`.
- Préalable : `CLoginMode_OnStateEnter 0x00d24080` est **non analysé** → tenter
  d'abord *Create Function* dans Ghidra (résout souvent sans debugger).

---

## 6. Carte des fonctions/globals (à documenter dans Ghidra)

Voir la liste consolidée dans le workflow (mémoire `project_moonlight_web_login_design`).
Clés : `UILoginWnd_OnMsg 0x008848d0`, `_OnCreate 0x00876840`, `_ctor 0x0086bb30`
(id classe **3**), `CUIEdit_SetText 0x008303f0`, `_ctor 0x00835570`,
`UIEdit_GetTextPtr 0x008210a0` / `_GetTextLength 0x008210d0`,
`UIWindow_SetVisible 0x005aad80` (vtbl+0x38, écrit `+0x28`),
`UIWindowMgr_FindWindow 0x00a47b90` (case 3→`mgr+0x1cc`),
`CLoginMode_SendMsg 0x00d2a130` (vtbl+0x18 ; renommer depuis `CharSelectMode_OnMsg`),
`_Run_StateLatch 0x00d27190` (vtbl+0x04), `_OnStateEnter 0x00d24080` (vtbl+0x1c, NON défini),
`Net_OnAcceptLogin_ParseAccount 0x00d1ea00`, `Network_ConfigureServerConnection 0x00d1f710`,
`NetConnection_Connect 0x00c13fc0`, `CRagConnection_SendPacket 0x00c14920`.
Globals : `g_pCurrentMode 0x0121333c` (alias trompeur `g_UICommandDispatcher`),
`g_pLoginWnd 0x0131f6b4`, `vtbl_CLoginMode 0x010932f0`, `vtbl_UILoginWnd 0x01030168`,
`vtbl_CUIEdit 0x0102e7fc`, `g_HasLoginToken 0x015ffa8c`, `g_LoginToken_500 0x015ffa8d`,
`g_RagConnection_Instance 0x015c5a20` / fd `0x015c5a24`.

---

## 7. Auto-passage NATIF du service-select + char-server (instantané)

Deux écrans intermédiaires entre « bouton Start » et char-select. Les deux sont
désormais franchis **sans frappe clavier ni délai**.

### 7.1 Service-select (liste `<connection>` de clientinfo) — cmd **`0x2723`**
N'apparaît **que si >1 connexion** (cas dev `--server=TEST` ; en prod 1 seule →
écran sauté nativement).

> ⚠️ **Piège corrigé après test live.** Il y a **DEUX écrans « select »** et ils
> n'ont **rien à voir**. J'ai d'abord utilisé `0x2713` (le sélecteur **char-server
> post-login**) pour le service-select → il lisait `mode+0x1e8`, table non peuplée
> avant login → IP garbage (`244.201.0.0`, nom vide) → **« Failed to Connect to
> Server »**. Le bon cmd est **`0x2723`**.

- **Commit natif d'une connexion** = `CLoginMode_SendMsg(mode, 0x2723, index, 0,0,0)`
  (vtbl+0x18, RET 0x14 = 5 args pile). Effet : `DAT_015ff81c = index`,
  `mode+0x6f48 = index`, puis `Apply_ClientInfoConnection 0x00a72da0` charge
  address/domain/version/servicetype de la connexion `#index`, puis **état 3**
  (écran de login) — ou `0xd` si `g_ServiceType` vaut 5/7.
- **Les `<connection>` ne sont PAS dans le mode.** Elles sont parsées au boot
  (`LoadClientInfoXml`) → arbre XML + copie launcher `DAT_0171d120` (stride `0x40`,
  `"ip:port"`) / count `DAT_0171d11c`. L'UI est le **combo id `0x1A`** construit par
  `UILoginWnd_OnMsg` case `0xe5` depuis le vecteur `UILoginWnd+0xd8` (stride `0x18`) ;
  l'index choisi est stocké à `UILoginWnd+0xc8`.
- Bourgeon : `native_login::SelectClientInfoConnection(index)` tire `0x2723`.
  `MoonlightAuth` l'appelle dès l'entrée au service-select ; **repli clavier**
  (`DriveServerSelect`, éprouvé) si l'écran de login n'apparaît pas sous 1,5 s.

#### Lire les `<connection>` : l'arbre XML natif, JAMAIS le disque
`LoadClientInfoXml 0x0171d320` ouvre `clientinfo.xml` par **`ResFileStream_Open
0x00573750`**, c.-à-d. **par le VFS** (`data\` puis les GRF), et laisse le document
parsé dans **`g_ClientInfoXmlDoc 0x0159B8A8`** ; `Apply_ClientInfoConnection` le
reparcourt à chaque sélection ⇒ **l'arbre vit toute la session**.

> ⚠️ **Bug corrigé.** `MoonlightAuth`/`AutoLogin` lisaient les `<display>` avec un
> `ifstream` sur `data\clientinfo.xml`. Ça ne marche que sur un client de **dev** :
> chez les joueurs le fichier n'existe **que dans `moonlight.grf`** → liste vide →
> `server_count_ == 0` → la condition `> 1 connexion` était fausse → **le
> service-select n'était jamais franchi** et le formulaire Moonlight restait
> derrière lui (il n'est dessiné qu'une fois `UILoginWnd` présente).

Parcours (tout est `__thiscall(node, name)`, sauf `GetText` `__fastcall(node)`) :

| Fonction | Adresse | Renvoie |
|----------|---------|---------|
| `XmlNode_FindChildByName` | `0x00A98400` | noeud enfant nommé, ou 0 |
| `XmlNode_FindNextSiblingByName` | `0x00A98460` | frère suivant nommé, ou 0 |
| `XmlNode_GetText` | `0x00A984C0` | `std::string*` (noeud+0xc) |

`std::string` MSVC : `+0x10` taille, `+0x14` capacité ; **capacité ≥ 0x10 ⇒ le
champ 0 est le pointeur** vers le buffer, sinon SSO en place (le natif teste
exactement `Text[5] >= 0x10`).

- Bourgeon : `native_login::ClientInfoConnectionNames()`. La lecture disque n'est
  gardée qu'en **repli** (arbre pas encore parsé si on interroge trop tôt : le
  ctor du plugin tourne AVANT `LoadClientInfoXml` → `MoonlightAuth` re-résout à
  l'entrée en mode login). Dernier filet : si le compte reste inconnu, « mode login
  sans `UILoginWnd` pendant >1,5 s » vaut service-select, franchi sur l'index 0.

### 7.1-bis Char-server select (**POST**-login) — cmd `0x2713`
- `CLoginMode_SendMsg(mode, 0x2713, index, …)` : table **`mode+0x1e8`, stride `0xa0`**
  (entrée `i` : `+0x1e8` IP u32 · `+0x1ec` port u16 · `+0x1ee` nom · `+0x204` état,
  `==3` ⇒ plein). Index sélectionné → `mode+0x1c8`, count → `mode+0x1c4`, puis état 5.
- Table peuplée **uniquement** par `Net_OnAcceptLogin_ParseAccount 0x00d1ea00` sur
  `AC_ACCEPT_LOGIN 0x0ac4` (`memcpy(mode+0x1e8, pkt+0x40, count*0xa0)`) — donc
  **après** login. La lire avant ⇒ garbage.
- Bourgeon : `native_login::SelectConnection(index)` (à n'utiliser qu'après login).

### 7.2 Char-server (aucune sélection — AUTOMATIQUE)
Le serveur (moonlight/rAthena, PACKETVER `20250716`) envoie `AC_ACCEPT_LOGIN = 0x0ac4`
(paquet moderne avec token). Côté client, `Net_OnAcceptLogin_ParseAccount 0x00d1ea00`
finit par `*(mode+0xc) = 6` → **pending état 6 (connexion char-server automatique)**.
**Il n'y a donc pas de fenêtre de sélection char-server** : le flux login → char-server
→ char-select avance seul, la latence restante = **réseau** (2 RTT). L'auto-confirm
Entrée de `MoonlightAuth` n'est qu'un filet pour tout écran résiduel (tir immédiat,
retry 50 ms, borné par `native_login::CharListLoaded()` = dispatcher cmd 8 slot 0).
