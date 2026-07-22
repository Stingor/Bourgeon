# RE — Chaîne d'authentification : login → service select → char select

Client `20250716` (base `0x00400000`, pas d'ASLR → adresses Ghidra == adresses
live). Cette page documente **toute la chaîne** que traverse le joueur entre le
lancement du client et l'entrée en jeu, puis sert de base à la conception d'un
**remplacement ImGui moderne de l'authentification** (voir
[login_auth_imgui_design.md](login_auth_imgui_design.md)).

Sources : RE Ghidra/x32dbg des sessions login (mémoires `project_login_screen_re`,
`project_auto_login`, `project_charselect_paging_crash`), lecture du plugin
`AutoLogin` et du hook WndProc.

---

## 0. Vue d'ensemble : les 5 étapes

```
 [lancement exe]
      │  WinMain parse la cmdline (-t:token, 1rag1, /launcherRESTART…)
      ▼
 ┌─────────────────────────────────────────────── CLoginMode (un seul « mode ») ──┐
 │ (1) SERVICE SELECT   liste <connection> de clientinfo.xml (si > 1 entrée)       │
 │        │  choix du monde/serveur → fixe l'IP:port du login-server               │
 │        ▼                                                                          │
 │ (2) ID / PASSWORD    UILoginWnd : saisie identifiants → CA_LOGIN (0x0064)        │
 │        │  AC_ACCEPT_LOGIN (0x0069) : AID + login_id1/2 + sex + LISTE char-serv   │
 │        ▼                                                                          │
 │ (3) CHAR-SERVER SELECT  liste des char-servers renvoyée (si > 1 ; souvent 1)     │
 │        │  choix → CH_ENTER (0x0065) vers le char-server                          │
 │        ▼                                                                          │
 │ (4) [handshake char-server]  HC_ACCEPT_ENTER (0x006b/0x082d/0x099d) :            │
 │        │  slots (normal/premium/billing/creatable) + liste des persos            │
 │        ▼                                                                          │
 │ (5) CHARACTER SELECT   UINewSelectCharWnd (grille de slots) → CH_SELECT_CHAR     │
 └──────────────────────────────────────────────────────────────────────────────────┘
      │  map-server login → bascule en CGameMode (jeu)
      ▼
 [en jeu]
```

Points **non évidents** confirmés :

- **Les étapes 1 à 5 vivent toutes dans le même « mode » `CLoginMode`.** Un seul
  `OnModeSwitch(kLogin)` couvre service-select, ID/pass ET char-select — on ne
  peut pas les distinguer par le mode. `AutoLogin` les sépare **au timing**
  (compteurs de ticks), pas par événement.
- Le **service select** (étape 1) n'apparaît que si `data\clientinfo.xml` contient
  **plus d'un `<connection>`**. Avec 0 ou 1 connexion, le client saute direct à
  l'écran ID/password.
- Le **char-server select** (étape 3) n'apparaît que si le login-server renvoie
  **plus d'un char-server**. Sur moonlight il n'y en a qu'un → l'étape est
  franchie en confirmant l'entrée par défaut.

---

## 1. Ce qui pilote la « saveur » du login : servicetype / servertype

`Config_ParseServiceAndServerType` @ **`0x00a73c98`** lit `clientinfo.xml` et fixe :

- **`g_ServiceType`** @ `0x0159b810` = région (`<servicetype>`) : 0=korea, 1=america,
  2=japan, … 17=france, 19=eu. **Valeur live de ce client moonlight = 1 (america)**
  → login **ID/mot de passe en clair** (pas de SSO/OTP).
- **`g_ServerType`** @ `0x0159b814` = `<servertype>` : 0=primary, 1=sakray, 2=local, 5=qm.
  **Live = 0 (primary)**.
- `<hideaccountlist>`→`g_HideAccountList` @ `0x0159b809` ;
  `<extendedslot>`→`g_ExtendedSlot` @ `0x0159b80b` (**live = 1**).

`Network_ConfigureServerConnection` @ **`0x00d1f710`** choisit l'IP/port du
login-server d'après service/server type + la connexion sélectionnée.

> **Conséquence pour le design** : SSO (token web) et OTP sont *compilés* mais
> gated derrière korea/servicetypes précis, et **sans code de pilotage actif**
> pour l'america (bitmaps d'intégration/`selCharForIntergration.bmp` = zéro xref
> live). Basculer `<servicetype>` casserait le dialogue avec ce serveur. Le
> remplacement moderne ne réactive donc PAS le SSO natif : il vient **par-dessus**
> en ImGui (voir design).

---

## 2. Étape 1 — Service select (liste `<connection>`)

- Alimenté par les `<connection><display>…</display>` de `clientinfo.xml`.
- `AutoLogin` lit ces `<display>` dans l'ordre (`ReadConnectionNames`,
  [auto_login.cc:76](../src/plugins/auto_login.cc#L76)) pour mapper `--server:<nom>`
  → index de liste.
- **Le client mémorise la dernière connexion utilisée** : l'entrée surlignée n'est
  pas forcément la première. `AutoLogin` force le haut de liste (N × `VK_UP`) puis
  descend à l'index cible.
- Combobox compte/serveur = `UILoginWnd this+0x3a` (items msgId `0xcb2`/`0xcb3`).

---

## 3. Étape 2 — ID / password (`UILoginWnd`)

RTTI `.?AVUILoginWnd@@` @ `0x0123ef0c` → **vftable `0x01030168`**.
`UILoginWnd_OnCreate` = vtable+0x3c @ **`0x00876840`** construit l'écran :

| Champ | Offset | Rôle |
|-------|--------|------|
| ID edit | `this+0x2d` | `CUIEdit` (`FUN_00835570`), 0x99×0x10 |
| PASSWORD edit | `this+0x2e` | masqué (`+0x84 = 0x2a = '*'`) |
| Save ID checkbox | `this+0x32` | `checkbox_on/off.bmp` |
| compte/serveur combo | `this+0x3a` | items `0xcb2`/`0xcb3` |
| fond fenêtre | — | `bg_login.tga` (≠ illustration plein écran `t_login.jpg`) |
| boutons | — | `bt_close`(cmd `0xb`), `bt_join`(register), `bt_start`(login) |

- **Focus initial dépendant de « Save ID »** : coché → ID pré-rempli → **password**
  focus en premier ; décoché → **ID** focus. `Tab` bascule. `AutoLogin` s'appuie
  dessus (`--saveid` choisit l'ordre de frappe).
- **Le clic Start construit et envoie le paquet d'auth** dans le `HandleEvent` de
  `UILoginWnd` (slot vtable `0x008896f0`(+0x2c) / `0x00881950`(+0x30), **non
  définis** dans la base Ghidra — région `0x00d24xxx` du login-connect non
  analysée). Le paquet envoyé est le classique **`CA_LOGIN` opcode `0x0064`**
  (car `0x0069` est traité en réponse, cf. §5).
- **Cheat Engine** : l'**ID** a des globals statiques ; le **mot de passe est
  heap-only** (adresse différente à chaque lancement) → pas de write-direct fiable
  du password (raison pour laquelle `AutoLogin` v2 write-direct a été refusé).

### Paquet `CA_LOGIN` (0x0064) — structure standard rAthena

```
0x0064  <version:4> <userid:24> <passwd:24> <clienttype:1>   // 55 octets, fixed-len
```

Réponse `AC_ACCEPT_LOGIN` (0x0069, variable) : header + `login_id1:4`,
`AID:4`, `login_id2:4`, (dépréciés), `sex:1`, puis **N × char-server** :
`ip:4, port:2, name:20, users:2, type:2, new:2` (32 o/serveur).

---

## 4. CLoginMode — la boucle et le dispatch recv

- `CLoginMode_OnUpdate` @ **`0x00d272e0`** : par frame → pompe recv, keepalive,
  timeouts, rendu. (C'est le point hooké par l'architecture plugin comme
  `LoginMode`.) L'état courant est à **`this+0x04`**.
- `LoginCharMode_RecvDispatch` @ **`0x00d27560`** (~49 Ko) = le **switch recv**
  login + char-server. Opcodes clés :

| Opcode | Sens | Rôle |
|--------|------|------|
| `0x0069` | AC_ACCEPT_LOGIN | AID/login_id1/2/sex + liste char-servers |
| `0x006a`/`0x6c`/`0x6e`/`0x70` | AC_REFUSE_LOGIN | errcode → message MSI |
| `0x006b` | — | (aussi utilisé pour liste char-server selon build) |
| `0x0081` | SC_NOTIFY_BAN | kick/ban |
| `0x0283`/`0x028b` | — | accept : stocke AID/login_id1/2/sex @ `this+0x6efc…` |
| `0x0ac4`/`0x0ac9` | AC_ACCEPT_LOGIN2 | variante token (→ `FUN_00d1ea00`) |
| `0x02eb` | — | char-select accept (passe map-server) |

- `Login_SendKeepalivePing` @ **`0x00d1f4e0`** : ping opcode `0x0187` toutes les
  12 s (états connectés 7/8/0xb/0x13).
- `Login_SendExeHashCheck` @ **`0x00d1f310`** : `CA_EXE_HASHCHECK` `0x0204` —
  **korea/indonésie-sakray uniquement, inactif en america**.
- `LoginCharMode_HandleTimeouts` @ **`0x00d26de0`** : msgbox de timeout par état
  (4=connexion login-server, 5/6/0xc=attente char-server).
- Illustration plein écran = std::string membre `this+0x6ee4` (recharge quand
  `g_SceneBg_ReloadFlag` `0x01602adc`≠0), défaut `t_login.jpg` →
  `유저인터페이스\t_login.jpg`.

---

## 5. Étapes 3-4 — Char-server select + handshake

- `AC_ACCEPT_LOGIN` (0x0069) porte la **liste des char-servers**. Si >1 → écran de
  sélection ; sinon confirmation directe (`AutoLogin` : `Stage::kCharServer`
  attend puis `VK_RETURN`).
- Choix → **`CH_ENTER` opcode `0x0065`** vers le char-server :
  `AID:4, login_id1:4, login_id2:4, (client_type:2), sex:1`.
- Le char-server répond **`HC_ACCEPT_ENTER`** (`0x006b`/`0x082d`/`0x099d` selon
  packetver) : nombre de slots + **liste des persos**. C'est ici que sont
  renseignés les globals de comptage :
  - `g_CharSelect_NormalSlots` `0x015ffd60`, `Premium` `0x015ffd64`,
    `Billing` `0x015ffd68`, `Creatable` `0x015ffd6c`.
  - Côté serveur moonlight : `HC_ACCEPT_ENTER2` (`chclif_mmo_send0272`) +
    `HC_CHARLIST_NOTIFY` (`0x099d`). Voir [charselect_re.md](charselect_re.md).

---

## 6. Étape 5 — Character select (`UINewSelectCharWnd`)

Documenté en détail dans [charselect_re.md](charselect_re.md). Rappel utile ici :

- UI `select_character_ver3`, vtable `0x0101d424`. Grille 5×3 = **15 slots/page**
  (source unique `0x0079a103 MOV [EDI+0x128],0xF`), pagination si le compte a plus.
- Slot sélectionné global `g_CharSelect_SelectedSlot` `0x015f8262`.
- Données persos lisibles via le dispatcher `g_UICommandDispatcher` `0x0121333c`
  (cmd 8 = perso par index), records `this+0x108` (stride `0x15c`).
- Sélection → entrée en jeu ; `AutoLogin` (`Stage::kCharSelect`) clampe à gauche
  (`VK_LEFT`) puis `VK_RETURN`.

---

## 7. Contrat launcher / cmdline (`WinMain` @ `0x00db9060`)

Pour un launcher/patcher custom. `WinMain` parse la cmdline via `strstr` :

- **Mono-instance** : `CreateMutexA("MutexInit")`, quitte sur
  `ERROR_ALREADY_EXISTS`. Patcher pour multi-client.
- **`-t:<token> <acct> <sexchar> <extra>`** = **token d'auth** (voie SSO). Copie
  ≤500 o → `g_LoginToken_500` `0x015ffa8d`, `g_HasLoginToken` `0x015ffa8c=1`. Le
  login-connect lit le flag (`0x00d24a6a`) et pilote l'état 4 avec ; recv
  `0x0ad1`/`0x0ae3` rafraîchissent le token (`FUN_00d2b840`/`FUN_00d2b400`).
  **Le transport token est donc câblé et vivant** — mais réclame un serveur qui
  valide le token (rAthena classique = `CA_LOGIN` seulement).
- `1rag1` = passcode classique **parsé mais ignoré** (lancer l'exe directement).
- `/launcherRESTART:<cmdline>` = self-CreateProcess (self-update).
- `pc`=pc-bang, `ApplyNPGE`=skip GameGuard, `Replay`=mode replay,
  `-inf:<n>`=n° instance.
- **Aucun `/account:`/`/passwd:`** natif : hand-off credentials = **token `-t:`
  uniquement** ; pour ID/pass en clair, lancer l'exe et laisser l'écran de login
  (ou `AutoLogin`) remplir.

---

## 8. Substrat ImGui sur l'écran de login (base du remplacement)

Vérifié dans le code Bourgeon — **c'est ce qui rend le formulaire ImGui moderne
faisable** :

- **Dispatch dédié `OnRenderLoginUI`** ([plugin.h:45](../src/plugins/plugin.h#L45)) :
  appelé par `Bourgeon::RenderUI` ([bourgeon.cc:285](../src/bourgeon.cc#L285))
  **uniquement** hors-jeu (login/char-select), dans un frame ImGui vivant, sauf
  pendant un chargement de map. C'est le hook qu'utilise déjà `LoginParade`.
- **Le hook WndProc alimente ImGui en entrées quel que soit le mode** et **avale**
  clavier/souris quand ImGui les capture
  ([ragnarok_client.cc:525-624](../src/ragnarok/ragnarok_client.cc#L525-L624)) :
  ```
  ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam);   // feed ImGui
  …
  if (io.WantCaptureKeyboard || …) {                            // swallow
    case WM_KEYDOWN/WM_CHAR/… : return 0;                       // game never sees it
  }
  ```
  → Une fenêtre `ImGui::Begin` + `InputText` sur l'écran de login **capture le
  clavier** et **le soustrait aux champs natifs** automatiquement. Pas de conflit
  de saisie à gérer à la main.
- Rendu sprites natifs en ImGui (paperdoll/mob) : chaîne atlas D3D9 + gestion
  handle DX7/DX9 déjà éprouvée dans `login_parade.cc` (réutilisable pour afficher
  un aperçu de perso dans un futur char-select ImGui).

> **Garde-fous connus** (à respecter dans le formulaire d'auth) : ne pas déclencher
> `SetCapture()` pendant la coroutine de connexion ; garde `DisplaySize>0` (minimize) ;
> garde `IsMapLoading()`. Voir mémoires `feedback_imgui_minimize_frame_guard`,
> `feedback_no_blocking_dialog_main_thread`.

---

## 9. Deux leviers pour piloter le login depuis un plugin

| Levier | Mécanisme | Fiabilité | Notes |
|--------|-----------|-----------|-------|
| **A. Input synthétique** | `PostMessage(WM_CHAR/WM_KEYDOWN)` dans les champs natifs, comme `AutoLogin` | Éprouvé, live | Password non hardcodable (heap) mais tapé caractère par caractère : OK. Fragile au timing/focus. |
| **B. Paquet direct** | `Bourgeon::SendPacket` construit `CA_LOGIN`(0x0064)/`CH_ENTER`(0x0065)/select | Plus robuste | Demande de répliquer le state machine natif (états `this+0x04`, stockage AID/login_id à `this+0x6efc`). RE de la région `0x00d24xxx` à finir. |

Le design ImGui (§ doc dédiée) retient **A pour l'étape ID/pass** (réutilise
`AutoLogin`, zéro RE supplémentaire, robuste car le serveur reste en `CA_LOGIN`
classique) et réserve B comme évolution.

---

## 10. Côté serveur — modèle de comptes (moonlight + site)

Vérifié dans `D:\…\GitHub\moonlight` (login/web-server rAthena) et
`D:\…\GitHub\moonlightsite` (forum phpBB 3.0.x). **C'est l'enabler du login
Moonlight** : le regroupement « 1 compte web → N comptes RO » **existe déjà en base**.

### 10.1 Table `login` (rAthena) — plate, + colonne custom `user_id`

- Struct `mmo_account` : `src/login/account.hpp:18-40` — `account_id`, `userid`,
  `pass`, `sex`, `email`, `group_id`, `char_slots`, `state`, `web_auth_token` (l.35)…
- SELECT principal : `src/login/account.cpp:512-514`. **Pas de colonne parent**
  (`master_account_id`/`web_id`) dans le schéma rAthena de base.
- **MAIS le site a ajouté une colonne `user_id` à `rathena.login`** = le
  `phpbb_users.user_id` du membre forum. C'est le lien web→jeu.

### 10.2 Le lien web→jeu, déjà implémenté côté site

- `includes\functions_moonlight.php:81-103` — `moonAccount($uid)` :
  `SELECT * FROM rathena.login WHERE user_id = '{$uid}' ORDER BY account_id ASC`
  puis, par compte, `SELECT * FROM rathena.char WHERE account_id = …`.
  → **retourne déjà tous les comptes RO + persos d'un membre web.**
- Création/liaison depuis le site : `index.php:826-873`, page `manageAccount` →
  `INSERT INTO rathena.login (userid, user_pass, sex, email, user_id) VALUES (…,
  '{$user_id}')` (l.860). Le template `moon_account.html` l'annonce : « comptes
  automatiquement liés au compte Web », « plusieurs comptes ragnarok », ban global.
- **Mot de passe de jeu** : le code d'origine posait le hash phpBB
  (`index.php:832`/`845`), **mais le site a été modifié (Stingor) pour hacher en
  MD5** afin de matcher le login-server en `use_MD5_passwords`. Donc
  `rathena.login.user_pass` = **MD5 hex** ; le login-server MD5 le mot de passe
  reçu en clair (`CA_LOGIN`) avant `strcmp`. → l'OTP du design se pose en
  `MD5(otp)` et se transmet en clair (voir doc design § chemin d'auth).

### 10.3 Base MySQL partagée

Une seule instance `localhost:3306`, user `siteweb` : bases **`site`** (phpBB,
`phpbb_*`), **`rathena`** (`login`/`char`/…), **`rathena_logs`**. Accès cross-DB
par noms qualifiés (`\`rathena\`.\`login\``). Aucun second DSN.

### 10.4 Pas d'API de login jeu (à créer)

- Aucun endpoint REST/JSON ne permet aujourd'hui au client de s'authentifier avec
  les identifiants **web** et de lister ses comptes RO. Les seuls JSON existants
  (`changelog_api.php`, `api/translate_script.php`) sont hors sujet.
- Le client s'auth **directement** au login-server rAthena contre `login`
  (login/pass). Le site n'est qu'une couche CRUD sur `rathena.login`.
- → **Point d'extension** : nouvel endpoint `api/game_login.php` (valide
  `phpbb_users`, réutilise `moonAccount()`). Toute l'infra (cross-DB, liaison) est
  en place.

### 10.5 SSO `CA_SSO_LOGIN_REQ` (0x0825) — câblé mais « token = password »

- Paquet : `src/common/packets.hpp:454-465` (`token[]` variable en fin).
- Handler : `logclif_parse_reqauth_sso` (`src/login/loginclif.cpp:328-360`).
  **Point clé (l.342-343)** : commentaire *« just use token as password »* — le
  `token` est copié dans `sd.passwd` puis passé à `login_mmo_auth`. **Aucune
  validation de token dédiée** → il valide contre `login.user_pass`.
- `web_auth_token` (généré `account.cpp:658` : `SHA2(UUID+RAND)`, validé
  `web/auth.cpp:36`) est **par account_id, régénéré à chaque login**, à durée de
  vie liée à la session — c'est le token de l'**API web in-game**
  (userconfig/emblem/party), **PAS** un identifiant SSO d'entrée. Ne pas le
  réutiliser comme clé de mapping.
- → Pour un vrai SSO « 1 login web → choix du compte RO », il faut **réécrire le
  corps de `logclif_parse_reqauth_sso`** (interpréter `token` comme jeton web à
  usage unique → résoudre account_id via table de mapping → court-circuiter
  `login_check_password`). Le paquet lui-même ne change pas.
</content>
