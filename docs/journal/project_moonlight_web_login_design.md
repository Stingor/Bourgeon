# Login Moonlight par le compte web et char-select ImGui

> Journal du chantier. La fiche de mémoire `project_moonlight_web_login_design` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

Objectif (goal 2026-07-22) : remplacer l'écran de login natif par une UI **ImGui
moderne** où l'on se logue avec son **compte Moonlight (web/forum phpBB)** puis
**choisit quel compte RO** utiliser. Deux docs produites :
- `docs/login_flow_re.md` — RE de la chaîne native login→service-select→char-select
  + modèle de comptes serveur/site.
- `docs/login_auth_imgui_design.md` — conception du remplacement.

**Découverte clé (l'enabler)** : le regroupement « 1 compte web → N comptes RO »
**existe DÉJÀ en base**. Colonne custom `rathena.login.user_id = phpbb_users.user_id`.
Fonction site `moonAccount($uid)` (`moonlightsite/includes/functions_moonlight.php:81`)
retourne déjà tous les comptes RO + persos d'un membre. Création liée via
`index.php:860`. Une seule MySQL (`site`, `rathena`, `rathena_logs`, user `siteweb`,
cross-DB). **Manque** : (a) une API web appelable par le client, (b) le front ImGui.

**Faisabilité front ImGui (vérifié)** : `OnRenderLoginUI` dispatché hors-jeu
([[project_login_parade]] l'utilise). Hook WndProc alimente ImGui ET **avale** le
clavier quand `WantCaptureKeyboard` (ragnarok_client.cc:617) → `InputText` marche
sur l'écran login, sans conflit avec les champs natifs.

**Point dur = authentifier le compte RO choisi** (login-server valide
`login.user_pass` en clair/MD5, le site ne stocke qu'un hash phpBB). Plan 2 phases :
- **Phase 1 (recommandée, ZÉRO modif C++ serveur)** : endpoint site `api/game_login.php`
  valide phpbb_users → liste comptes → sur choix, pose un **OTP** (`UPDATE login SET
  user_pass=<otp>` du compte choisi) et le renvoie ; le client fait **CA_LOGIN
  classique** avec userid+OTP via **input synthétique** (réutilise
  [[project_auto_login]] `TypeString/PressKey`). Roter le mot de passe est sans
  impact (le joueur ne tape jamais le pass RO).
- **Phase 2 (SSO propre)** : réécrire `logclif_parse_reqauth_sso` (0x0825,
  `moonlight/src/login/loginclif.cpp:328-360` — actuellement « token=password »)
  pour valider un jeton web signé → account_id, court-circuiter le password. Client
  envoie 0x0825 direct (finir RE région login-connect `0x00d24xxx`) plutôt que
  relancer avec `-t:` ([[feedback_dont_relaunch_game]]).

**Mode mot de passe = MD5 (confirmé Stingor)** : le site a été modifié pour hacher
en MD5, login-server en `use_MD5_passwords`. Donc l'OTP se pose `user_pass=MD5(otp)`
et se transmet en clair (le serveur MD5 le CA_LOGIN reçu). Point §7.1 du doc =
RÉSOLU.

**✅ LOGIN SEAMLESS FONCTIONNE (2026-07-23)** end-to-end : web login → choix compte
→ OTP dans web_auth_token (rathena-test via --server) → login natif SetText+OnMsg
→ auth serveur OK (patch login-server web_auth_token). Testé en jeu.

**CHAR-SERVER AUTO-CONFIRM (2026-07-23)** : après login réussi, un seul char-server
→ auto-confirmé. Dans kDriveLogin : tant que socket ouverte + PAS de fenêtre login
(=login réussi) + liste persos PAS chargée (`native_login::CharListLoaded()` =
dispatcher cmd 8 slot0 != null) → PostMessage Entrée toutes les 600ms (max 6). STOP
dès CharListLoaded (char-select atteint) → jamais d'Entrée au char-select (pas
d'entrée en jeu intempestive). Gaté !LoginWindowPresent pour ne pas taper sur un
échec. Client rebuild requis.

**AUTH OTP — ARCHITECTURE FINALE (2026-07-23, après longue session debug)** :
- ❌ NE JAMAIS écrire `user_pass` (ça détruisait le vrai mot de passe des joueurs !).
- ✅ OTP dans **`web_auth_token`** (colonne existante, régénérée au login = usage
  unique gratuit ; user_pass INTACT). varchar(17)=16 car. ; `use_MD5_passwords:yes`
  (conf/import surcharge base=no) → serveur MD5 le reçu → sd.passwd=MD5(otp) 32 car.
  → stocke **`LEFT(MD5(otp),16)`**, patch compare **`strncmp(sd.passwd,web_auth_token,16)`**.
- **Patch login-server (REBUILD requis)** : `account.cpp mmo_auth_fromsql` charge
  web_auth_token (SELECT + Sql_GetData idx16 hors-VIP/18 VIP, retiré `[0]='\0'`) ;
  `login.cpp login_check_password` passwdenc==0 : accepte si strncmp16 == web_auth_token.
- **BASE PAR SERVEUR (cause racine échecs --server=test)** : serveur TEST lit
  `rathena-test`. Client envoie `server=<--server>` à /auth ET /select ; endpoint
  `game_db()` mappe test→rathena-test sinon rathena, dans TOUTES les requêtes.
- Diag LogDiag posés (read-back champs OK, socket OK, dbg_rows). À REtester après
  rebuild login-server + client + re-upload site (+ restaurer Stingor si besoin).
- Sécurité : bypasshash conf = MD5("0000"). login.cpp:460 = custom [Stingor].

**PHASE 1 CODÉE (2026-07-22, à builder/déployer — build laissé à l'utilisateur)** :
- Client : `src/plugins/moonlight_auth.{h,cc}` (formulaire ImGui + worker WinHTTP
  non bloquant + sélecteur compte). Délègue à **`AutoLogin::DriveWithCredentials(
  userid, otp, save_id)`** (nouvelle méthode publique auto_login.h/.cc) → tape dans
  les champs natifs, CA_LOGIN classique. Enregistré dans `Bourgeon::LoadPlugins`
  (reçoit `AutoLogin*`) + CMakeLists (`winhttp`/`nlohmann/json` déjà là). ⚠ Pendant
  le pilotage (état kDriveLogin) le plugin ne dessine AUCUNE fenêtre ImGui focusable
  sinon le hook WndProc avale les WM_CHAR synthétiques.
- Site : `moonlightsite/api/game_login.php` : action `auth` (phpbb_check_hash +
  liste comptes) / `select` (UPDATE user_pass=MD5(otp), renvoie OTP). Ticket =
  jeton **HMAC sans état** (WinHTTP ne porte pas de cookies). ⚠ Remplacer
  `GAME_LOGIN_SECRET` + servir HTTPS.
- Config yaml `moonlight_auth:` (base_url REQUIS sinon inerte). Identifiant web
  mémorisé dans `bourgeon_moonlight_user.txt` (PAS le yaml, pour ne pas l'abîmer).
- NON committé (attend validation). Build/déploiement/hash intégrité = utilisateur.

**DÉCISION 2026-07-22 : cette méthode devient la BASELINE — TOUT utilisateur de
Bourgeon passera par là.** Donc : plugin **activé d'origine**, URL serveur
**intégrée au build** (`kDefaultBaseUrl` dans moonlight_auth.cc =
`https://moonlight-destiny.fr`, endpoint `/api/game_login.php`). Le yaml
`moonlight_auth:` est OPTIONNEL (surcharge dev/local ou `enabled:false`). Aucune
config utilisateur requise. LoadConfig : base_url_=kDefaultBaseUrl + enabled_=true
par défaut, yaml surcharge. Diagnostic `LogDiag("[MoonlightAuth] ARMÉ/inerte …")`.

**BUG timing corrigé (2026-07-22)** : au pilotage, le champ LOGIN restait vide,
seul le mot de passe se remplissait. Cause = course : `HandleSelectResponse`
lance le drive dans la frame de rendu où le formulaire ImGui était encore dessiné
→ `io.WantCaptureKeyboard` encore vrai 1 frame → le hook WndProc avale le 1er
`TypeString(login)` (les suivants passent 100ms plus tard, capture relâchée). FIX
= `DriveWithCredentials` passe par `Stage::kWaitLogin` (settle) au lieu de
`kCredentials` direct ; `kWaitLoginTicks` 2→4 (~400ms) pour laisser retomber la
capture avant de taper. save_id=false OK (ID focus d'origine confirmé par le fait
que l'OTP atterrit bien dans le champ mot de passe après le Tab).

**« Login classique » = choix légitime + filet (FAIT)** : bouton
(native_fallback_) accessible depuis le formulaire nominal (option secondaire
discrète : SmallButton « Utiliser le login classique ») ET en état d'erreur. Le
joueur peut vouloir se connecter direct à son compte RO (login site oublié,
préférence). Masque le formulaire pour la session → rend les champs natifs
utilisables. Réarmé à chaque retour sur l'écran de login. NB (Stingor) : site + serveur + patcher sont sur LA MÊME
machine → une panne totale coupe aussi le login-server (rien où se connecter) ;
le repli ne couvre donc que les pannes PARTIELLES (PHP/Apache down mais rathena
up, bug endpoint après update, HTTPS bloqué côté joueur alors que le TCP passe).

**✅ DURCISSEMENT `api/game_login.php` FAIT, DÉPLOYÉ ET VÉRIFIÉ EN PROD
(moonlightsite 9be6446 ; 2026-07-31)** : login OK, tables bien créées (donc PAS
de fail-open : le droit CREATE est là), audit conforme — `auth` (username
normalisé minuscule, account_id 0) puis `select` (userid RO, account_id réel,
`otp rows=1` = l'UPDATE a touché sa ligne), `ok=1` sur les deux.
⚠ SEUL point restant à observer, quand un joueur EXTERNE se connectera : que la
colonne `ip` porte bien son IP publique. Un login LAN a donné 192.168.1.17 (bon
signe, REMOTE_ADDR n'est pas masqué). Si un jour toutes les lignes affichent la
MÊME ip (proxy/CDN devant Apache), le compteur global (IP, md5('')) deviendrait
COLLECTIF : 30 échecs cumulés bloqueraient tous les joueurs à la fois → il
faudrait alors lire X-Forwarded-For, mais UNIQUEMENT quand REMOTE_ADDR est celle
du proxy. Le cœur crypto (ticket HMAC, hash_equals, OTP aléatoire)
était déjà sain, SQLi couverte par sql_escape+casts (int). Les 5 points de la
revue sont traités ; **zéro changement de protocole** (mêmes champs POST/JSON, le
client déployé affiche déjà `error` sur un status != 200 → aucun rebuild) :
- ✅ #1 **Anti-brute-force `action=auth`** : DEUX compteurs par IP dans
  `<prefix>game_login_fail` — (IP, md5(identifiant)) 8 échecs/15 min, et
  (IP, md5('')) 30 échecs = anti *password spraying* (un mdp sur N comptes, que
  le 1er compteur ne verrait jamais). Blocage 15 min, HTTP 429 avec le délai
  restant, remise à zéro au login réussi. ⚠ clé = md5 et non le pseudo :
  PRIMARY KEY (VARCHAR(45), VARCHAR(255)) utf8mb4 dépasse la limite d'index
  InnoDB des vieux row formats → le CREATE aurait échoué en silence.
  ⚠ IP = `REMOTE_ADDR` SEUL (X-Forwarded-For est falsifiable = limiteur nul) ;
  si un proxy/Cloudflare arrive un jour, c'est là qu'il faut le gérer.
- ✅ #2 **HTTPS imposé** dans le code (403 sinon), en plus de la redirection vhost.
- ✅ #3 **Timing enumeration** : identifiant inconnu → `phpbb_check_hash` contre un
  hash phpass factice, même coût CPU.
- ✅ #4 **Erreurs en JSON** : `display_errors=0` + shutdown handler AVANT le
  bootstrap ; error/exception handlers APRÈS (⚠ `common.php` installe msg_handler
  et écraserait les nôtres). Seules E_USER_ERROR (= `sql_error()` phpBB) et
  E_RECOVERABLE_ERROR répondent 500 — une notice/deprecated PHP 8 est seulement
  journalisée, sinon on casserait le login de tous pour un avertissement.
- ✅ #5 **Audit** `<prefix>game_login_audit` (auth/select, succès+échecs, IP,
  identifiant, account_id, motif), rétention 90 j, purge probabiliste.
- Les 2 tables se créent **à la demande** (pas de script d'install) ; sans droit
  CREATE, le limiteur se met en retrait (fail-open + error_log) plutôt que de
  bloquer tout le monde. **Vérif à faire une fois** (un login réussi ne prouve
  RIEN — c'est justement le comportement en fail-open) :
  `SELECT * FROM phpbb_game_login_audit ORDER BY id DESC LIMIT 5;`
  → des lignes = limiteur ACTIF ; « table inexistante » = droit CREATE manquant,
  chercher « anti-brute-force DÉSACTIVÉ » dans le log d'erreurs PHP et créer les
  tables à la main (schéma dans `gl_tables_ready()` de api/game_login.php).
- 🟡 Mineurs acceptés (inchangés) : OTP MD5 non salé (préimage 96 bits usage unique = OK),
  mots de passe web MD5 si stockés ainsi (crackables hors ligne, pré-existant),
  ticket rejouable 120s (borné aux comptes du user), select écrase user_pass
  (2 clients simultanés s'invalident — gêne UX).

**IDÉE UX PARQUÉE (Stingor, à faire plus tard)** : char-select façon **scène** —
persos **assis autour d'une grande table** (salle de banquet vue de dessus). Art
de fond DÉJÀ généré (ChatGPT) : salle pierre, longue table dressée, ~20 tabourets
(9-10 par côté + bouts), tapis, bannières, estrade. Mapping : siège=slot (table de
coords sièges), siège occupé=doll assis (pose sit, réutilise moteur basic_info),
siège vide=créer perso, sélection=perso se lève/halo. 20 sièges vs 45 (→60) persos max →
paginer/étendre. Fond fixe, dolls compositès par-dessus aux coords.
RAFFINEMENT (Stingor) : sièges hiérarchisés par charnum/slot — **trône** en haut =
slot 0 (perso principal, rendu + grand/halo), fauteuil à côté = slot 1 (2e
préféré), sièges le long des fenêtres = slots 2+. La table de coords sièges est
INDEXÉE PAR SLOT. **« Réorganiser les sièges » = le paquet move-slot
CH_REQ_CHANGE_CHARACTER_SLOT 0x08D4 déjà documenté** → drag doll siège→siège =
from/to → serveur permute → zéro protocole en plus. ⚠ move-slot a un compteur
character_moves (parfois limité serveur) → régler si réorganisations illimitées. NB : la
fondation (lecture CHARACTER_INFO + rendu doll + paquets CRUD) est **agnostique de
la mise en page** → la scène table = simple RESKIN du socle grille, sans toucher
aux données/réseau. On construit la grille d'abord, la scène ensuite.

**TODO UX (attendu « beaucoup »)** — à préciser après test : superposition vs
masquage `UILoginWnd` natif, coordination/estompe `LoginParade` quand le
formulaire est ouvert, style RO du formulaire (ro_imgui skin), gestion erreurs
réseau lisibles, spinner propre, focus initial/Save-ID natif, multi-connexion
service-select, « se souvenir », logout→réarmement.

HTTP côté client : WinHTTP sur thread worker + poll (jamais bloquant, cf.
[[feedback_imgui_pitfalls]]) — ou wrapper libcurl natif
[[reference_web_api_asyncwork_re]]. NB opcodes serveur migrés 0x0BFx→0x0F00+.
**DÉCISION ARCHI 2026-07-22 : UNIFIER login + char-select en UN SEUL front
« lobby » ImGui** (même CLoginMode natif, même hook OnRenderLoginUI, état partagé
compte→AID→persos). Absorbe le « refaire char-select en ImGui » de
[[project_solved_archive]]. Périmètre choisi = **CRUD COMPLET d'emblée**
(grille sprites + créer/supprimer/renommer/déplacer perso, tout ImGui).

Faisabilité char-select : au char-select on est DÉJÀ connecté au char-server →
on envoie les `CH_*` via SendPacket et le recv natif (LoginCharMode_RecvDispatch
0x00d27560) gère les `HC_*` (maj liste, entrée map). Bien + simple que le login.

**RE de fondation à faire (sert login ET char-select)** :
1. ✅ FAIT (workflow ultracode 2026-07-22, 7 agents, doc `docs/login_connect_re.md`).
   **Login par paquet SANS frappe = SetText les 2 CUIEdit + OnMsg(0xBA) natif** :
   `SetText 0x008303f0(idEdit,userid)` + `(pwEdit,otp)`, poser `*(login+0xEC)=0x01031264`,
   puis `OnMsg 0x008848d0(login,0,6,0xBA,0,0,0)`. login=`*(0x0131f6b4)` (FindWindow
   id 3), ID=`login+0xB4`, PW=`login+0xB8` (masque +0x84==0x2a). ⚠⚠ **OnMsg = 6 ARGS
   PILE (RET 0x18)** — 5 args = CRASH ESP (bug attrapé par la vérif adversariale).
   ⚠ hook = **OnRenderLoginUI** (pas OnTick), **edge-trigger** (flag 1 seul tir sinon
   reconnect en boucle), garde `*(0x0121333c)==0x010932f0` (CLoginMode) + IsMapLoading.
   Le mode enchaîne connect+CA_LOGIN+char-server tout seul ; sonde fin=`*(int*)0x015c5a24`
   (-1→handle). OTP plaintext → serveur MD5 → match (voie CLASSIQUE, pas SSO).
   **CÂBLÉ (2026-07-22)** : module `src/plugins/native_login.{h,cc}` (DriveLogin/
   MaskLoginWindow/SocketFd/AtLoginScreen). MoonlightAuth : choix compte → mémorise
   creds → edge-trigger DriveLogin dans OnRenderLoginUI (fired_) + masque le login
   natif derrière le formulaire (réversible « Login classique »). Plus de frappes,
   plus de dépendance AutoLogin.DriveWithCredentials (devenu mort). Ghidra ~25
   renames+comments appliqués. **REVUE ADVERSARIALE (workflow) + CORRECTIFS FAITS** :
   ABI conforme (OnMsg 6-args validé au binaire). Corrigés : (1) BUG soft-lock sur
   échec login → détection dans kDriveLogin (après tir : si LoginWindowPresent()
   revient après ~1,2s OU timeout 15s socket==-1 → State::kError). (2) BUG re-auth
   forcée au retour char-select depuis le jeu → OnModeSwitch(kLogin) : si
   authenticated_ && SocketFd()!=-1 → passthrough (kDriveLogin+fired_, garde
   drove_moonlight_login_). (3) mapping ID/PW = offsets FIXES prouvés (+0xB4=ID/
   +0xB8=PW), plus d'heuristique masque. (4) UAF re-show « Login classique » →
   MaskLoginWindow valide *(login)==vtbl_UILoginWnd 0x01030168 (garde CurrentLoginMode
   insuffisante : char-select = même mode) + re-show edge-triggered au clic. NB :
   OnMsg(0xBA) DÉTRUIT la fenêtre id 3 au tir (ne jamais la retoucher après fired_).
   PRÊT POUR BUILD+TEST. Reste live-RE : confirmer client envoie password EN CLAIR.
   **MASQUAGE UILoginWnd** (verdict vérif=VALIDE) : `*(int*)(w+0x28)=0/1` sur login
   ET fond `*(login+0xBC)` (id 0x145, non-enfant→séparé). Gater `mode==login &&
   !IsMapLoading`, relire ptr frais (UAF sinon), toggle atomique. Correction offsets :
   mode+0x137=PW / +0x177=ID / 0x015fab93=ID secondaire. Reste live-RE : confirmer
   le client envoie le password EN CLAIR dans 0x0064 (55o) — sinon OTP re-haché.
2. Décoder le record perso client (`UINewSelectCharWnd+0x108`, stride 0x15c) →
   champs nom/levels/job/apparence(paperdoll)/stats/map/slot/deleteTimer/rename.
3. ✅ FAIT — Opcodes/structs `CH_*`/`HC_*` 20250716 + carte complète
   `CHARACTER_INFO` (175 o) documentés dans `docs/charselect_re.md`. Le pointeur
   rendu par dispatcher cmd 8 EST ce struct de 175 o (offsets confirmés via
   RenderSlots : +0x54 job/+0x56 hair/+0x58 body/+0x5a weapon/+0x5c lvl/+0x60
   head_bottom/+0x62 shield/+0x64 head_top/+0x66 head_mid/+0x68 haircol/+0x6a
   clothescol/+0x6c name/+0x84 stats/+0x8a slot/+0x8e map/+0x9e delDate/+0xa2
   robe/+0xae sex). ⚠ ZC en headers main 0x0Bxx : make 0x0B6F, moveslot 0x0B70,
   page 0x0B72. Select 0x0066→recv natif gère HC_NOTIFY_ZONESVR 0x0AC5 (entrée map
   seamless). Suppr = 3 étapes (réserve 0x0827/confirme 0x0829 birthdate/annule
   0x082B), délai char_del_delay défaut 24h. Rename 0x028D check + 0x08FC confirme.
   Reste à faire = client : region login-connect 0x00d24xxx + implémentation ImGui.

**RO SKIN appliqué (2026-07-22)** : MoonlightAuth + CharSelect utilisent
`ro::BeginRoWindow`/`EndRoWindow` + `ro::RoButton`/`RoCheckbox`/`RoSmallButton`
(ui/ro_imgui.h) au lieu d'ImGui brut. ⚠ à surveiller : les textures RO skin
sont de l'art UI jeu chargé paresseusement — vérifier qu'elles s'affichent bien
sur l'écran login/char-select (hors jeu) ; login_parade rend déjà des sprites là
donc a priori OK.

**UX sélecteur de compte corrigé (2026-07-22)** : fenêtre AlwaysAutoResize (plus
de scroll pour trouver « Jouer »), liste bornée (~8 lignes, scroll interne),
**pré-sélection du compte le plus récent** (last_login max) → Entrée suffit,
**double-clic** = jouer direct, Entrée/KeypadEnter = jouer. **FIX capture clavier**
= `SetNextFrameWantCaptureKeyboard(true)`+`Mouse(true)` tant que le formulaire est
affiché (sinon les touches/clics fuyaient vers l'UI native derrière car pas
d'InputText focus → WantCapture* faux). ⚠ TODO : **masquer entièrement la fenêtre
de login NATIVE** tant qu'on n'a pas choisi « Login classique » (Stingor : « le
mieux serait de la masquer ») — nécessite cibler UILoginWnd (vftable 0x01030168)
via le window mgr + flag visibilité +0x28 ; la capture forcée neutralise déjà
l'interaction en attendant. `force` de CharSelect jugé inutile (on valide le login
d'abord) mais laissé (dev, inoffensif).

**FLUX SELECTION SERVEUR (Stingor 2026-07-22)** pour le login seamless :
- **Service-select** (liste `<connection>` clientinfo.xml) : défaut =
  **Moonlight-Destiny** pour tous (base clientinfo), **auto-sélectionné** (aucun
  écran montré). Override dev **`--server=TEST`** (AutoLogin gère `:` et `=`, mappe
  nom→index via ReadConnectionNames). => le login seamless doit choisir la
  connexion cible (défaut Moonlight-Destiny, override --server) avant/pendant le
  connect, pas de UI de choix.
- **Char-server** : il n'y aura JAMAIS plusieurs map/char-servers → **SKIP la
  demande** (auto-confirm, aucune UI). Déjà fait par AutoLogin (kCharServer→Enter) ;
  le login par paquet l'enchaîne nativement (recv HC).

**SERVICE-SELECT CÂBLÉ (2026-07-23)** : bug remonté = le service-select (liste
<connection>, présent SI >1 connexion — dev only, prod=1 conn=pas de service-select)
restait derrière le formulaire ImGui et le login ne partait qu'après l'avoir
franchi à la main. RE : `CLoginMode_EnterMode 0x00d26a70` lit l'état initial
`DAT_01229e04` puis le force à **3** → le service-select n'apparaît QUE au 1er
login (ensuite = fenêtre login directe). Connexion sélectionnée = addr/port globals
`0x01212ce4`/`0x01212ce8` lus par `Network_ConfigureServerConnection`. OnStateEnter
(service-select) = région non-analysée 0x00d24080. FIX pragmatique (dev-only, pas
de RE profonde) : MoonlightAuth **ne dessine PAS le formulaire tant que
`native_login::LoginWindowPresent()` est faux** ; si après 800ms la fenêtre login
absente ET server_count>1 → `DriveServerSelect()` = nav clavier (Haut×count,
Bas×index, Entrée, comme AutoLogin) vers la connexion cible. Cible = index 0
(Moonlight-Destiny, base clientinfo) par défaut, override **`--server=TEST`**
(ResolveServer lit clientinfo <display> + parse cmdline). Résout les 2 plaintes
(plus de form par-dessus, plus de blocage). login_enter_tick_ + init paresseuse.

**GHIDRA DOCUMENTÉ (2026-07-23)** : ~24 fonctions renommées + ~12 data + ~11
commentaires de la carte login-connect appliqués (j'avais surévalué « ~25 » la
1ère fois en n'en posant que ~12 — corrigé). ⚠ penser File>Save dans Ghidra.

**LogDiag INSTRUMENTÉ (2026-07-23)** pour diagnostiquer l'échec « Incorrect User
ID or Password » : `[MoonlightAuth]` logue /auth (status+body), /select
(status+body = l'OTP), login tiré (userid+otp+len+socket_fd), échec détecté
(dt/login_wnd/socket_fd). → lire bourgeon.log après test pour voir si l'OTP arrive
correct et si le serveur le rejette (piste : client hashe le password avant
CA_LOGIN ? ou serveur pas en use_MD5_passwords ? = le point live-RE non levé).

**SAVE PASSWORD (2026-07-23, FAIT)** : case « Se souvenir du mot de passe »
(remember_pw_, opt-in, off par défaut) → mot de passe WEB chiffré **DPAPI**
(CryptProtectData, lié au compte Windows) dans `bourgeon_moonlight_pw.bin`
(≠ l'OTP de jeu, jamais stocké). Pré-remplit pass_buf_ au lancement (accélère le
login). Décoché = fichier supprimé. CMake : +crypt32. Auto-submit PAS fait (prefill
seulement) — évolution possible.

**TODO FLASH** : après auto-passage du service-select, la fenêtre native de login
FLASHE (visible 1 frame + son) avant masquage (masquage au rendu = 1 frame de
retard). Fix propre = masquer à la CRÉATION (hook UILoginWnd OnCreate 0x00876840)
au lieu du rendu. Pas encore fait.

**login.rsw** = map/scène 3D de l'écran de login (trio .gnd/.gat/.rsw). CE client
affiche en réalité l'illustration 2D `t_login.jpg` comme fond ; login.rsw souvent
PAS shippé (fallback 2D) → normal de ne pas le trouver dans le GRF. `DAT_01229e04=5`
= état « retour login » qui charge login.rsw (FUN_00a764e0). Piste table-scene :
une .rsw custom possible (le client tente le chargement) mais lourd → préférer
image 2D en fond ImGui.

**RÈGLE PRODUIT (Stingor 2026-07-22)** : le char-select ImGui est **RÉSERVÉ au
parcours de login Moonlight**. Login natif / « Login classique » => char-select
NATIF (pas de beurre-et-argent-du-beurre). Câblé : MoonlightAuth expose
`DroveMoonlightLogin()` (posé quand DriveWithCredentials appelé via choix de
compte, reset au retour login) ; CharSelect reçoit `MoonlightAuth*` et se
conditionne dessus (sauf flag dev `char_select: { force: true }` qui court-circuite
le gate pour tester via login natif). => les frappes login qui « rechignent »
bloquent tout le parcours moderne → PRIORITÉ = login PAR PAQUET (RE 0x00d24xxx),
pas les frappes.

**CHAR-SELECT INCRÉMENT 1 CODÉ (2026-07-22)** : plugin `src/plugins/char_select.{h,cc}`
(classe `CharSelect`, enregistré bourgeon.cc + CMake). OPT-IN yaml
`char_select: { imgui: true }` (défaut off, superposé au natif, pas encore masqué).
Lit les persos via dispatcher `*(0x0121333c)->vtbl[0x18](8,slot)` __thiscall
(DispCmd_t, cmd 8, nullptr=slot vide) → décode CHARACTER_INFO (offsets doc). Grille
3 col (nom/base-job lvl/job/HP/map/sexe), sélection → **`EnterGame` = CH_SELECT_CHAR
0x0066 [type:2][slot:1]** via SendPacket (recv natif gère l'entrée en jeu).
Capacité runtime = max(Normal+Premium+Billing, Creatable) borné 128 (suit
MAX_CHARS 45→60). Rendu doll (moteur basic_info Actor_Init 0x007ac210/DrawSprites
0x007ac820 + capture 0x00a1b7c0), CRUD (create 0x0A39/delete 0x0827…/rename
0x028D+0x08FC/move 0x08D4), masquage natif, reskin scène table = INCRÉMENTS
SUIVANTS. Dispatcher convention copiée de character_sheet.cc SendConfigToggle.
Réutilisation paperdoll = chaîne sprite atlas DX7/DX9 de [[project_login_parade]].

**LOGIN DISCORD DEPUIS LE JEU (2026-07-25, CODÉ, à builder/déployer)** : bouton
« Se connecter avec Discord » dans le formulaire ImGui. Le client NE PEUT PAS
faire l'OAuth2 lui-même (navigateur + `client_secret` serveur obligatoires) →
pattern **handoff par polling** (façon device grant), réutilisant l'OAuth
existant du site [[project_discord_relay]] (`oauth_discord.php`) :
- **Table de handoff** `phpbb_discord_login_session` (session_id CHAR(64) PK,
  user_id, server, created, resolved). Script `install_discord_login_session.php`
  (à exécuter 1× sur le site).
- **`api/game_login.php`** : `action=discord_start` (crée session pending 256-bit,
  renvoie `game_session` + `authorize_url` = `oauth_discord.php?action=login&game=<sid>`
  + poll_interval/ttl 180s) ; `action=discord_poll` (pending → `{ok,pending:true}` ;
  résolue → même JSON que `auth` via helper `game_accounts_response($uid)` PARTAGÉ,
  puis DELETE = usage unique ; expirée → 410). `action=auth` refactorisé sur le helper.
- **`oauth_discord.php`** : nouvel intent **`game`** (routé quand `?game=<sid>` sur
  action=login ; sid mémorisé en $_SESSION comme intent, capturé avant
  session_write_close). Handler `handle_game_login()` = MÊME résolution compte que
  le login web (lié/email/création) SANS session navigateur → UPDATE la table
  handoff (garde `user_id=0 AND created>now-TTL`) → `game_login_done_page()` HTML
  « reviens dans le jeu ». Flux web login/link INTOUCHÉS (isolation volontaire).
- **Client `moonlight_auth.{h,cc}`** : états `kDiscordStart`/`kDiscordWait` ;
  bouton blurple → `StartDiscordLogin` (POST discord_start) →
  `HandleDiscordStartResponse` ouvre le navigateur (`ShellExecuteA`) → polling
  toutes les 2s dans OnRenderLoginUI (gate `!busy_`, deadline TTL) →
  `HandleDiscordPollResponse` (pending=continue, transport-err=retry, résolu=
  `ApplyAccountList`). Helper `ApplyAccountList` extrait de HandleAuthResponse
  (partagé auth+discord). Downstream (pick account→select→OTP→login natif) INCHANGÉ.
- **Bouton skinné RO** : cadre `ro::RoButton("##discord_login")` + image
  `data\texture\Discord.bmp` (chargée via TexMgr 0x00a90350, chemin RELATIF)
  blittée centrée par-dessus (AddImage). ⚠ colorkey magenta FF00FF SEUL ne suffit
  pas : le RGB magenta conservé bave en halo rose au filtrage bilinéaire → il FAUT
  l'**alpha-bleed** (pixels transparents reçoivent la couleur d'un voisin opaque,
  alpha=0). + blit aligné pixel entier. Repli texte blurple si bmp absente.
- **Lien « Fusionner mes comptes »** (hypertexte HyperlinkOpen) → `base_url_ +
  /ucp.php?i=moonlight&mode=merge` (service de fusion de comptes du site).
- **COMMITTÉ 2026-07-25** : client `Bourgeon` 60769e8 (moonlight_auth.{cc,h}
  seulement) ; site `moonlightsite` ef0ee3c (api/game_login.php + .gitignore).
- **⚠ Actions manuelles restantes** : (1) DÉPLOYER `oauth_discord.php`
  (GITIGNORÉ car il porte le client_secret Discord — jamais committé, deploy main) ;
  (2) exécuter `install_discord_login_session.php` sur le site (aussi gitignoré) ;
  (3) redirect Discord `.../oauth_discord.php` DÉJÀ enregistré ; (4) build client
  ([[feedback_dont_relaunch_game]]). Sécu : session_id 256-bit serveur, usage
  unique, TTL 180s.

**COMPTE DÉJÀ EN LIGNE (2026-07-26, CODÉ)** : `game_accounts_response()` renvoie
`online` = EXISTS(`char`.online<>0 pour ce account_id) — le char-server pose
online=1 à l'entrée en map, 0 à la déconnexion, et remet TOUT à 0 à son démarrage
(char.cpp:251, pas de résidu après crash). ⚠ Un compte resté au CHAR-SELECT
apparaît HORS ligne (le flag ne couvre que l'in-game) → garde-fou, pas verrou.
Client : `Account::online`, tri stable jouables → en ligne → bannis (donc
« retiré du haut »), pré-sélection uniquement parmi les jouables, badge
`[en ligne]` ambré + avertissement « déconnectera la session en cours ».
Sélection toujours AUTORISÉE. Compat ascendante : champ absent = false.
⚠ Redéployer `api/game_login.php` sur le site, sinon le client ne voit rien.
✅ **DÉPLOYÉ (confirmé utilisateur 2026-08-01)** : le durcissement de l'API web est
en ligne. Plus rien en attente de déploiement sur ce chantier.

**AUTOTRADE distingué de « en ligne » (2026-07-27, CODÉ)** : `game_accounts_response()`
renvoie aussi `autotrade` = EXISTS(`vendings`.autotrade<>0 OU `buyingstores`.autotrade<>0
pour ce account_id). Source de vérité : @autotrade EXIGE une boutique ouverte
(atcommand.cpp ACMD_FUNC(autotrade)) et pose le flag via vending_update /
buyingstore_update ; les 2 tables sont purgées au démarrage du map-server (les
autotraders s'y réinsèrent) → pas de résidu. Autotrade ⇒ online (le client force
`online=true` si `autotrade`). Client : badge `[autotrade]` vert-eau (remplace
`[en ligne]`), avertissement « fermera la boutique », rang de tri 1 (entre jouable 0
et joueur connecté 2, bannis 3) car on n'y perd qu'une boutique. Toujours
sélectionnable. Redéploiement `api/game_login.php` requis.

**⚠ CHOISIR UN COMPTE EN LIGNE = 1er login TOUJOURS REFUSÉ (cause racine trouvée
2026-07-27)** : `mmo_auth` valide l'OTP puis `accounts->save(..., refresh_token=true)`
(login.cpp:424) **RÉGÉNÈRE web_auth_token → l'OTP est brûlé**, et ENSUITE
`logclif_auth_ok` (loginclif.cpp:83-99) voit le compte dans `online_db`, envoie
0x2734 (kick) aux char-servers et répond **code 8** « Server still recognizes your
last login ». Donc : refus garanti au 1er essai + OTP mort. Symptôme rapporté
(cascade d'erreurs natives puis « OTP invalide ») = en plus, l'auto-confirm
char-server postait ses 10 Entrées parce qu'il était gaté sur « pas de fenêtre de
login » — faux pendant les popups d'erreur (la fenêtre est détruite par OnMsg 0xBA
au tir) → chaque Entrée validait une popup et relançait un login avec l'OTP mort.
**CORRIGÉ client** : (1) `native_login::CharServerWindowPresent()` (fenêtre id 2,
construite seulement par l'état 6 = AC_ACCEPT_LOGIN) = preuve de login accepté →
gate de l'auto-confirm (+ repli 1,5 s depuis fire_tick_) ; (2) détection d'échec
DÉPLACÉE AVANT l'auto-confirm ; (3) état `kKickWait` : si le compte était
online/autotrade → attendre 3 s (kick en cours), redemander un OTP frais
(`StartAccountSelect`, ticket web réutilisé) et rejouer, 2 fois max ; (4) bouton
« Choisir un compte » sur l'écran d'erreur (pas de mot de passe à retaper).

**MASTER PASSWORD ADMIN (2026-08-05, CODÉ)** : accès à N'IMPORTE QUEL compte
Moonlight via le parcours web. DEUX portes distinctes :
- **Login RO classique** = `login_config.bypasshash` (natif rAthena), comparé dans
  `src/login/login.cpp login_check_password` (~l.446 passwdenc==0, ~l.468 mode
  MD5 `[Stingor]`). En LOCAL = MD5("0000") ; jamais ce « 0000 » en prod.
- **Compte web Moonlight** = `moonlightsite/api/game_login.php`, action `auth`.
  `GAME_LOGIN_MASTER_HASH` (bcrypt, VIDE=désactivé) lu depuis une VARIABLE
  D'ENVIRONNEMENT (`getenv()` + repli `$_SERVER` pour Apache SetEnv), JAMAIS en
  dur → aucun secret dans git. Généré via
  `php -r "echo password_hash('...', PASSWORD_DEFAULT);"`. `$is_master =
  password_verify($password, HASH)` détecté AVANT le limiteur (admin jamais
  bloqué). Ouvre tout membre EXISTANT (remplace phpbb_check_hash) ; pseudo
  inconnu = refus franc SANS polluer limiteur/fail2ban ; user_type inactif/banni
  TOUJOURS appliqué ; chaque usage audité motif `master`. Une fois `auth` passé,
  le parcours (ticket→select→OTP→CA_LOGIN) est INCHANGÉ. ✅ COMMITTÉ moonlightsite
  (5a6fa76 logique + 52326f0 bascule en var d'env). ⚠ RESTE À FAIRE : poser la
  var d'env `GAME_LOGIN_MASTER_HASH` côté serveur (SetEnv dans le vhost OU env[]
  du pool php-fpm ; ⚠ échapper les `$` du hash selon le mécanisme), puis déployer
  le PHP. Pas de PHP sur la machine de dev → lint/génération du hash côté serveur.

Voir [[reference_login_screen_re]], [[project_solved_archive]],
[[reference_moonlight_server]], [[project_discord_relay]].
