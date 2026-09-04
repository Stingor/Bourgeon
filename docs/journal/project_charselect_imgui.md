# Écran de sélection de personnage en ImGui (plugin CharSelect)

> Journal du chantier. La fiche de mémoire `project_charselect_imgui` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

Plugin `CharSelect` (src/plugins/char_select.{h,cc}) — remplacement ImGui de l'écran
de sélection de perso. **Activé par défaut** (opt-out `char_select: { imgui: false }`),
**réservé au parcours de login Moonlight** (gate `MoonlightAuth::DroveMoonlightLogin()`,
règle produit : login natif ⇒ char-select natif). Bouton « Créer / gérer (natif) » =
repli session tant que le CRUD n'existe pas.

**Fenêtre PLEIN ÉCRAN opaque + capture clavier/souris** : couvre totalement le natif
et le rend inatteignable — bien plus simple que masquer son rendu 3D (pas une UIWindow
isolable).

## 🔴 La règle qui a coûté un bug de production
**NE JAMAIS envoyer `CH_SELECT_CHAR 0x0066` soi-même.** Le natif ne l'envoie même pas
depuis le bouton OK : c'est l'état 9 du mode. Envoyer le paquet seul ⇒ le client entre
en jeu **sans nom de perso local** (stocké obfusqué XOR, cf. [[project_charselect_re_docs]])
⇒ rAthena kicke au 1er message de chat (`clif_process_message: incorrect name`).
Second effet : `g_CharSelect_SelectedSlot` (`0x015f8262`) jamais posé ⇒ le cache local
(`g_Own_*`/`g_OwnLook_*`) est semé depuis le **mauvais perso** pour tout slot ≠ 0.

**Fix = piloter le natif** : poser l'octet `0x015f8262`, puis `OnMsg(g_pCharSelectWnd
0x0131f8bc, 0, 6, 0xB8, 0,0,0)` (RET 0x18 = 6 args pile, garde vtable `0x0101d424`).
Séquence complète documentée dans `docs/charselect_re.md`. Même leçon que le
service-select (`0x2723` et pas `0x2713`) et le login (`OnMsg 0xBA`) :
voir [[feedback_native_replacement]].

## Textes du jeu en ImGui
Nom de perso / map / nom de job sont dans la **code-page du client**
(`g_ClientCodePage 0x0159b818`, 949 ou 1252 selon `g_ServiceType`), PAS en UTF-8 →
sinon « ????? ». Convertir avec la code-page LIVE (helper `LocalToUtf8` dans
char_select.cc), jamais 949 en dur. Nom de job = getter natif
`Job_GetDisplayNameOrResName 0x00d5bb40` `__thiscall(0x015fa3c0, job, sex)`.

## Scène « banquet » (LIVRÉE)
Fond BMP plein écran (`유저인터페이스\...lobby_hall.bmp` via GRF) + persos assis (pose
`anim=2`) sur des sièges pilotés par données `g_seats` (nx,ny,scale). **Pagination** :
5 sièges d'honneur FIXES (persos 0-4) + 20 bancs paginés (`CharForSeat(i,page)`,
`NumPages`). Découplage siège(position `i`)/perso(`ci`). **Éditeur layout F10** (staff
via IsStaff FAUX au char-select car flag serveur in-game only → F10 le débloque) :
poignées `Anchor()` + dump. Perso en suppression = sprite **rouge pulsant** (teinte
`RenderDoll(...,tint)`) + bandeau.

## Dolls (LIVRÉS)
`BasicInfoTweaks::RenderDoll(look, x,y,w,h,dir,anim,tint)` — moteur de capture partagé
de basic_info (`Actor_Init 0x007ac210`/`Actor_DrawSprites`/hook `Actor_SubmitSpriteQuad
0x00a1b7c0`), **contexte de rendu factice** (vtable slot 40 → `UIWindow_GetFadeColor
0x00a1edf0`) → marche hors jeu. Cache par apparence, budget/frame. Échelle pilotée par
la HAUTEUR (un compagnon/garment large ne rétrécit plus le perso ; pas de clip → il
déborde). Curseur natif masqué (hors écran) au login/char-select — [[project_ro_cursor]].

## Suppression : détour du handler d'ACK (LIVRÉ)
Réponse `HC_DELETE_CHAR3_RESERVED 0x0828` → handler natif **`Net_OnDeleteCharReserveAck
0x00d21210`** (`__fastcall` ecx=mode, RET 4, pktbuf `0x015e8198` : result +6, char_id
+2, date +10). `result` : 1=succès (écrit del_rev_date +0x9e), 2=no-op, **4=guilde,
5=groupe**, 3=db, 6=échoppe, 0=déjà en file. Sur échec le natif ouvre une **msgbox
MODALE bloquante** (`UIWndMgr_ShowMessageBoxModal 0x00a31a30`, PAS dans FindWindow ;
count `[g_UIWindowMgr+0x1b4]`) qui traînerait SOUS notre UI. **Fix = détour JMP** : si
notre UI couvre (`g_cover_active`) ET échec → NE PAS rappeler l'original (box jamais
créée) + remonter le code exact → bandeau ImGui. Le hook recv de Bourgeon NE voit PAS
les paquets char-server (dispatch `0x00d27560` ≠ boucle zone) → le détour est la seule
voie. Serveur : refus seulement si `char_del_restriction & CHAR_DEL_RESTRICT_GUILD/PARTY`.

## Suppression FINALE : envoyer CH_DELETE_CHAR 0x01fb (EMAIL), PAS piloter le natif
🔴 Le contrôle natif `OnMsg(0xd3)` enchaîne **DEUX modaux BLOQUANTS** (confirmation +
saisie clavier du code, `UICharDeleteCodeInputWnd`, via `UIWndMgr_ShowMessageBoxModal
0x00a31a30` / `CharDelete_RunCodeInputModal 0x00a31600`). Sous notre UI plein écran qui
CAPTE l'input, la pompe modale n'obtient jamais le clic/la frappe → **FREEZE** (input
avalé — pb d'INPUT pas de rendu, une garde de ré-entrance sur le rendu ne suffit pas).
Natif **structurellement inutilisable** ici → EXCEPTION à
[[feedback_native_replacement]] : on **envoie le paquet nous-mêmes**.

⚠ Le bon paquet dépend de `char_del_option` serveur. moonlight (`import/char_conf.txt`)
= **1**, et dans rAthena **1 = EMAIL** (2 = anniversaire, 3 = les deux — PAS l'inverse !)
⇒ le client envoie **`CH_DELETE_CHAR 0x01fb`** (client ≥2004 : `[op u16][CID u32][key
char[50]]` = **56 o**), PAS le flux réservation `0x0829` (qui, lui, est anniversaire
codé en dur). Handler `chclif_parse_delchar` → `chclif_delchar_check(email, CHAR_DEL_EMAIL)` :
passe si `email == sd->email` (nocase) OU (`sd->email=="a@a.com"` défaut ET email vide).
Les comptes moonlight ont un **email RÉEL** (pas le défaut) ⇒ **saisie de l'email requise**
(champ ImGui `del_email_`, partagé entre persos = même compte). `CID = CHARACTER_INFO+0x00
= gid`. Envoi via `Bourgeon::SendPacket(pkt,56)`. `char_del_delay=0` ⇒ suppression
immédiate, pas de délai serveur. Sur succès le serveur re-pousse la liste → perso
disparaît. Sur mauvais email : refus `HC_REFUSE_DELETECHAR 0x0070` **SANS figer** (envoi
direct, aucune modale native en attente — vérifié : notre 1er essai 0x0829 échouait sans
freeze). `del_email_[0]` requis pour activer « Supprimer ». `io.WantTextInput` gate la nav
flèches + Enter pendant la saisie. (❌ mon 1er correctif 0x0829/anniversaire vide était
FAUX : mauvais `char_del_option` supposé + mauvais paquet.)

Annuler + supprimer = **paquets DIRECTS** (le pilotage natif `DriveNativeCtrl 0x198`
n'annulait rien de fiable sous notre UI) : annuler = `CH_DELETE_CHAR3_CANCEL 0x082b`
(6 o `[op][CID]`) ; le clic « Supprimer » ouvre un **popup modal ImGui** (champ email
auto-focus, `SetKeyboardFocusHere`, Entrée=submit) qui envoie `0x01fb`. Popup habillé
via **`ro::BeginRoPopupModal`/`ro::EndRoPopupModal`** — helper RÉUTILISABLE ajouté dans
ro_imgui (barre de titre 3-slice titlebar_* + corps clair, rendu en BeginPopupModal donc
ImGui bloque/assombrit le fond seul ; pas de gestion de modalité côté appelant). Enter/
nav gardés par `IsPopupOpen(AnyPopup)`. Cible figée dans `del_popup_gid_`/`del_popup_slot_`/
`del_popup_name_` au clic. Créer/entrer restent en `DriveNativeCtrl` (0x1a0/0xB8).
Double-clic siège → EnterGame **déjà codé**, mais `can_enter_slot` bloque si
`del_rev_date>0` (perso en suppression) — normal.

🔴 **Poser `kSelectedSlot` (0x015F8262, octet) AVANT tout envoi direct 0x082b/0x01fb.**
La réponse `HC_ACCEPT_DELETECHAR 0x006f` (et l'annul 0x082c) NE porte PAS de char_id : le
handler natif retire/modifie le perso au **SLOT SÉLECTIONNÉ**. Sans le poser, le serveur
supprimait le BON perso (via le CID du paquet) mais le client retirait le MAUVAIS doll de
l'affichage (slot périmé) — désync jusqu'au relog. `*(uint8_t*)kSelectedSlot = slot`
(comme le fait `DriveNativeCtrl`). Le reserve-ack 0x0828, lui, est char_id-based (détour).

## Création : popup ImGui + aperçu live (CH_MAKE_CHAR 0xa39)
Clic siège LIBRE → popup `ro::BeginRoPopupModal` avec **aperçu doll live** (Novice
job/body 0, `DrawCreateDoll` → RenderDoll anim=0) + nom + sexe + **sliders** cheveux/
couleur. Envoi direct **`CH_MAKE_CHAR 0xa39`** (36 o : `[op][name char[24]][slot u8]
[hair_color u16][hair u16][job u32][sex u8]`, PACKETVER≥20151001 ; stats forcées à 1
serveur, job=0 Novice). Bornes client : **hairstyles 1..43, haircolors 0..250, cloths
491** (setarray .@max 491,251,43). Nom : **UTF-8 ImGui → code-page client** via
`Utf8ToLocal` (inverse de `LocalToUtf8`), sinon accents FR en mojibake. Sexe défaut =
compte (`kAccountSex 0x015FB23C`). Pose `kSelectedSlot` avant l'envoi. **Résultat déduit**
(le refus `HC_REFUSE_MAKECHAR 0x006e` est hors de notre recv char-server) : slot rempli =
succès → ferme ; rien après 2,5 s = échec → « nom pris/invalide » dans le popup.

Aperçu : **molette au survol = rotation** (`create_dir_` 0..7) + tooltip. Cheveux/couleur
en **sliders RO + molette** (bornes 43/250).

## Reste à faire
- **CRUD** : créer OK (0xa39 + aperçu) ; suppression OK (0x01fb email) ; annuler OK
  (0x082b) ; renommer/déplacer à faire.
- **Grille d'icônes de coiffure façon natif = FAIT** (module `hairicon` dans
  char_select.cc). Le natif dessine juste une frame du **.spr de coiffure** (pas un doll).
  Chemins CP949 confirmés (RE agent IDA + capture x32dbg sur `make_character_ver2`, caller
  ~0x007a0e99) : format-strings natives **0x0108F9F4** `인간족\머리통\남\%d_남.spr` (mâle,
  `%d`=id brut), **0x0108F9D8** `.act` ; **0x0108F9BC**/**0x0108F9A0** = variantes 여
  (femelle). Rendu = copie de `login_parade::ResolveQuad` (kTexMgrGet 0x00a90350, kMakeKey
  0x00a9f030, kLoadRes 0x00a8d4a0, kActGetFrame 0x0070f4b0 action 0/frame 0, atlas via
  kSceneCtxPtr 0x012515f8+0xc0, kAtlasGetCached 0x00566b70, cellule=plus grande, handle
  CTexture+0x12c DX9/+0x128 DX7 via `g_imgui_dx7_active`). Grille scrollable, `IsRectVisible`
  = seules cases visibles chargent (cache 64). SEH obligatoire.
  Grille **8 colonnes × 10 lignes**, enfant dimensionné à la hauteur exacte (`rows*(cell+
  spacing)`) → **les 80 tiennent SANS scroll** (case 34 px). Ordre ASCENDANT (id affiché =
  id envoyé, **pas de permutation** — confirmé : `UIMakeCharWnd_OnMsg 0x007A1470` case 524
  fait `hairStyle = index+1`, 24 boutons fixes, UI bornée à 23 via `Job_NeedsLuaItemPosOffset`,
  aucune pagination). `char_make_new_char` NE valide PAS hair_style → on expose TOUT data.grf
  (~80, `kMaxHairStyle=80`). `kMaxHairColor=250`.

  ## 🔴 Le doll EST clampé à 23 coiffures — patch WARP « Allow 65k Hair Styles » requis
  `moonlight-destiny.exe` est **BRUT non patché** (= IDB). Le rendu acteur in-world (donc
  notre `RenderDoll`) passe par `CActorSprite_RenderCompositeJobSprite 0x00c48f60`. Le format
  EST `%d` brut (`0x0108F9F4` `인간족\머리통\남\%d_남.spr`, `0x0108F9BC` 여) MAIS le NUMÉRO
  passé au `%d` est **`(index % 23) + 1`** — modulo 23 CODÉ EN DUR à `0x00C49149` (`imul
  0xB21642C9` = div signée /23, `imul 0x17`, `sub esi`, `inc esi` ; `ecx` = coiffure index
  0-based). Donc coiffure 24→1, 45→23, etc. Le 23 = les 23 du make-char natif ; les persos
  réels n'ayant jamais >23 (make-char borné) le wrap ne se voit jamais EN PROD — mais exposer
  80 tape dedans. (Ma pseudo-C Hex-Rays avait masqué ce modulo → vérifier au DÉSASSEMBLAGE.)
  **`IncrHairs.qjs`/`Allow65kHairs` (WARP0716 `Scripts/Patches/`) neutralise ce clamp et ses
  jumeaux** : palette de tête, coiffes-mid dépendantes, table `GetJobName` (stage2_open sites
  `0xB41EEF`/`0xB41F0D`), make-char natif (`Job_GetHeadSpritePath 0x00d5a1f0` → `_impl
  0x00b433b0`, clamp `hair>0x2A`). **Voie recommandée = patcher l'exe via WARP (A)** : couvre
  TOUS les sites d'un coup. Voie B (partielle) = patch runtime Bourgeon du `%23` à 0xC49149
  (`mov esi,ecx`+NOP → esi=ecx+1 brut) : ne corrige QUE le sprite du doll, pas la couleur ni
  les coiffes. ✅ `moonlight-destiny.exe` EST patché (Allow65kHairs) → coupes 13-80 OK au doll.

  ## Remap legacy des 12 coiffures (icône WYSIWYG)
  Le patch 2020+ (`fixTblCond`) PRÉSERVE la table de remap historique pour les index < 43 :
  le jeu rend `coiffure N → fichier T[N]` pour N≤12 (identité 13+). Notre icône chargeait le
  fichier brut `N` → décalée du doll sur 1-12. Fix (`DrawHairIcon`, `kHairFile[13]`) : charger
  `T[hair]` pour l'ICÔNE seulement, **envoyer create_hair_ brut** (le serveur stocke l'id, le
  client ré-applique T). Table id→fichier (vérifiée à l'œil) : `1→2 2→1 3→7 4→5 5→4 6→3 7→6
  8→8 9→9 10→10 11→12 12→11`, identité ≥13. Le numéro sous la case = id ENVOYÉ (hs).

  ## ⚠ 3 bornes de coiffure à garder ALIGNÉES (sinon kick serveur au retour char-select)
  Symptôme faux-ami : après avoir créé un perso avec coiffure >43, « Character Select » en
  jeu rebondissait vers le LOGIN + auto-relogin (2× `[Bourgeon] build`, `[MoonlightAuth]
  kError`). CAUSE = **serveur** `MAX_HAIR_STYLE` encore à 43 → le perso high-hair est REJETÉ
  à la sélection → kick → boucle. PAS un bug MoonlightAuth (la machine d'états réagissait au
  kick). Fix = monter `MAX_HAIR_STYLE` serveur + reboot. Les 3 bornes : (1) CLIENT patch WARP
  `Allow65kHairs` ; (2) SERVEUR `MAX_HAIR_STYLE` ≥ ce qu'on expose ; (3) Bourgeon
  `kMaxHairStyle=80`. Cf. [[reference_moonlight_server]].
  ## 🔴 Auto-entrée parasite / faux kError au char-select (slot >0) — RÉSOLU
  `MoonlightAuth::CharListLoaded()` (native_login.cc) ne testait QUE le slot 0 (cmd 8
  dispatcher → nullptr si slot vide). Compte dont le 1er perso est en slot >0 (création
  siège libre) → renvoie toujours `false` au char-select → `kDriveLogin` ne se TERMINE
  jamais : (1) auto-confirm char-server presse Entrée en boucle → fuite dans le char-select
  ImGui → `EnterGame` parasite (log « entrée en jeu slot=… ») ; (2) la détection d'échec voit
  la socket char-server tomber (NORMAL en entrant en jeu) → faux `kError` (`dt>15s socket=-1`)
  → reset `authenticated_` → re-drive login complet + AutoLogin → boucle (2× `[Bourgeon]
  build`). FIX : `CharListLoaded()` scanne les 45 slots ; latch `charsel_reached_` (moonlight_
  auth) posé dès liste chargée -> stoppe DÉFINITIVEMENT auto-confirm + détection d'échec
  (passthrough idle), posé aussi au retour-jeu (ligne 429), reset sur vrai (re)login.
  ⚠ Ne PAS confondre avec le kick MAX_HAIR_STYLE serveur (autre cause, cf. plus haut).

  ## ✅ Sortie de l'écran : « Revenir au login » / « Quitter le jeu » (2026-07-26)
  Les 2 issues du « Cancel » natif (`ctrl 185`, qui branche sur `g_CanReturnToLoginScreen`
  `0x01602328`) : **`SendMsg(mode, 10011)`** = OnDisconnect + état 3 (écran de connexion) ;
  **`SendMsg(mode, 2)`** → `CMode_SendMsg_Base 0x00a763c0` = arrêt sous-systèmes + `mode+0x14=0`
  = quitter. Envoyés NOUS-MÊMES (`DriveModeCmd`, dispatcher vtbl+0x18) : piloter `ctrl 185` ne
  marche PAS, sa msgbox passe par `ShowMessageBoxModal` que `Detour_ShowModal` supprime (retour
  185 ≠ 187 attendu → le natif conclut « annulé »).
  🔴 Le client RESTE en CLoginMode (seul l'ÉTAT change) ⇒ **aucun OnModeSwitch** : il faut
  (a) `MoonlightAuth::RearmWebLogin(reason, service_select_pending=false)` sinon le plugin reste
  en `kDriveLogin` et c'est le login NATIF qui revient ; (b) ne PAS réarmer le service-select
  (l'état 3 recrée UILoginWnd directement).
  🔴🔴 **Sonde d'écran = la FENÊTRE, jamais les CHARACTER_INFO** : elles SURVIVENT au retour au
  login (cmd 8 répond encore) ⇒ `CharListLoaded()` était vrai dès le 2ᵉ login → `charsel_reached_`
  latché tout de suite → plus d'auto-confirm → blocage sur la fenêtre **id 2 « Select Service »**
  (choix du char-server, état 6). FIX = `native_login::CharSelectWindowPresent()` (FindWindow
  `0x115`) : `CLoginMode_OnStateEnter` appelle `UIWindowMgr_DestroyAllWindows 0x00a482f0` en tête,
  donc **aucune fenêtre ne survit à un changement d'état** — zéro résidu. États live (mesurés) :
  **7 = char-select, 3 = login, 6 = Select Service** (`mode+0x4` = état courant, `+0xc` = demandé).
  ~~Éditeur layout F10 = DÉSACTIVÉ~~ → **RÉACTIVÉ et persisté (2026-08-03)** : mode
  « Personnaliser » joueur (bouton dans la barre du bas + F10), sièges/ancres/décor
  dans `features/windows/char_select_layout` (`charsel::State()`) →
  `bourgeon_charselect_layout.yaml`. Décor au choix : galerie des `.bmp` de
  🔴 `<jeu>\data\TEXTURE\lobby\` — le TexMgr préfixe `texture\` PAR TYPE
  (`Path_ConcatPrefixIfMissing` dans `UITextureMgr_Load 0xa8d4a0`, type résolu sur
  l'extension par `0xa8d310`), PUIS `Res_MakeDataRootRelativePath 0x573380` ajoute
  `data\` ⇒ `data\lobby\` n'est JAMAIS lu (mon 1er jet s'y trompait) — + fonds du
  GRF, changement À CHAUD.
  🔴🔴 `Overlay_ReleaseTexture` IMMÉDIAT = CRASH `EIP=0` dans d3d9 (`mov esi,[eax+0Ch]`
  / `call esi`) : la draw-list de la frame COURANTE référence encore la texture et
  rien d'autre ne la retient ⇒ libération DIFFÉRÉE de 2 frames (file ; vidée SANS
  Release si l'epoch device a changé). ⚠ Même motif latent dans `ro::SpriteView`
  (`Unload` appelé depuis `TrimFor`, pendant une frame) — NON corrigé.
  🔴 `SaveIfDirty` aussi
  dans `DriveModeCmd` : le retour au login NE change PAS de mode ⇒ pas d'OnModeSwitch.
  **Pose PAR SIÈGE** (`charsel::Seat` : dir 0..7, anim, head_dir) : molette =
  tourner, Ctrl+molette = taille, clic droit = menu de pose (Assis 2 / Debout 0 /
  Touché 6 / Mort 8 / Combat 4 (+ `animate` → `freeze_body`, n'agit qu'en Marche
  et Combat)) + orientation de tête. Les 6 index sont VÉRIFIÉS à l'écran (13 types
  d'action dans un `.act` de perso).
  🔴🔴 **TÊTE TOURNÉE (`/doridori`) = index d'IMAGE**, ni action ni direction :
  un `.act` de tête a **3 images par action** (les 3 inclinaisons), choisies par
  `acteur+0x3C`. Les COIFFES suivent SEULES (leur `.act` a 24 img = 8 × 3 ⇒
  l'anim alternative les met dans le bon sous-groupe).
  🔴 Cet index est **PARTAGÉ par tout l'acteur** (un seul champ) ⇒ le poser AUSSI
  pour le CORPS : `Actor_ComputeHeadAttach 0x7adbc0` compare l'ancre tête à celle
  du corps prise SUR LA MÊME IMAGE. Sinon → **le cou se détache** (ancres
  divergentes ; `1_남.act` assis : (1,−35)/(−2,−36)/(4,−36) img 0/1/2). Borner au
  nombre d'images du corps (repli sur 0 comme `Act_GetFrame`), sinon pantin
  INVISIBLE. N'appliquer qu'au corps immobile. Astuce d'enquête : la
  commande `/doridori` est dans le binaire (id 0x6E, chaîne 0x1098298).
  ⚠ Mes 2 pistes fausses, avec leur signature VISUELLE : (1) action
  `head_dir*8+dir` → aucun effet (une tête a les 13 mêmes types qu'un corps :
  104 actions) ; (2) direction `dir±1` → tête DÉTACHÉE du corps + MIROIR d'un côté
  (dirs 5..7 = vues 3..1 retournées). ⇒ un symptôme « décalé/miroir » = on a touché
  à la DIRECTION ; « aucun effet » = on a touché à l'ACTION.
  💡 Mesurer un `.act` hors ligne : `moonlight.grf` = GRF 0x200 (lisible en Python,
  entrées 17 o) ; pour `data.grf` (0x300, 5,08 Gio, offsets > u32) **DEMANDER
  l'extraction** au user au lieu d'écrire un lecteur — cf.
  [[reference_grf_act_spr_reference_impl]] (règle déjà notée, enfreinte 2 fois).
  **ARME + BOUCLIER au char-select** ✅ : `CHARACTER_INFO` **+0x5A arme / +0x62
  bouclier** (item ids). Pas d'acteur ici ⇒ `own_actor` inutilisable ⇒
  `rag::ResolveHeldSprites` (ragnarok/held_sprites) appelle les constructeurs
  natifs : arme `Job_GetWeaponSpritePath 0xd8a010` / `...ActPath 0xd8a160`
  `__stdcall(std::string* out, job, sexe, classe, vue_ou_-1, style)` ; BOUCLIER
  **0xd5e240 (.spr) / 0xd5e1d0 (.act)** `(out, job, sexe, vue, style)`.
  🔴 `Job_GetWeaponShield*Path` = couche voisine de l'ARME (slot 6), PAS le
  bouclier (slot 7 — cf. `CActorSprite_BuildShield_Slot7 0xd401d0`).
  🔴🔴 `CHARACTER_INFO` porte une **VUE** sur **16 BITS** (lu en u32 → 65537 =
  arme+base_level collés). NE PAS passer par `Weapon_ItemIdToWeaponClass`
  (classe 0 → suffixe vide → chemin du CORPS `…\초보자\초보자_남`). Le GRF donne
  la règle : `data\sprite\인간족\초보자\초보자_남_양손도끼.spr` ⇒ suffixe = NOM
  d'arme ⇒ voie « **par vue** » d'abord (param 6 = la vue), suffixe de classe en
  repli. Sonde d'existence via `ro::spract::ReadFile`. `_검광` = éclat de lame
  (slot 6, non branché). 🔴 Chemins natifs
  RELATIFS à la racine sprites ⇒ préfixer `data\sprite\` + retirer l'extension.
  ⚠ std::string du client = 24 o {buf[16], size, capacity} ; lire via
  `cap>=16 ? *(char**) : buf` ; libérer par `std_string_dtor 0x4f08f0` (__fastcall).
  ⚠ Bébés = pas de bouclier (drapeau acteur +716).
  **Image d'action** (`force_frame`) + **places libres masquables/atténuables**
  (masquées, elles réapparaissent AU SURVOL, sinon plus aucun accès à la création).
  **Mises en page NOMMÉES** (max 20) dans le même fichier (clé `presets:`, mêmes clés
  que la racine ⇒ un seul lecteur/émetteur) ; le défaut n'est PAS un preset (code,
  `Factory()`) ⇒ ni écrasable ni supprimable, restauré par « Tout restaurer ».
  Détails : docs/charselect_re.md § « Mode Personnaliser ».

  ## Modale native bloquante supprimée (nom déjà pris, etc.)
  `HC_REFUSE_MAKECHAR 0x6e` (nom pris) → handler INLINE dans `LoginCharMode_RecvDispatch
  0x00d27560` (pas détournable isolément) → `UIWndMgr_ShowMessageBoxModal 0x00a31a30`, qui
  lance SA PROPRE pompe de messages (modale BLOQUANTE) : sous notre UI plein écran qui capte
  l'input, OK jamais cliqué → ImGui FIGÉ. FIX = **détour GLOBAL de `ShowMessageBoxModal`**
  (`Detour_ShowModal`, __thiscall ecx=mgr + 9 args pile) : si `g_cover_active` → retour no-op
  `185` (= code natif "déjà en jeu", pas d'affichage), sinon original. Couvre make-char + toute
  autre modale piégée. Compteur `g_modal_suppressed_seq` ++ à chaque suppression → le popup de
  création conclut « nom pris » INSTANTANÉMENT (baseline `create_modal_base_` à l'envoi) sans
  attendre le timeout 2,5 s. Le détour delete-ack (0xd21210) court-circuite SON handler en
  amont (ShowModal jamais appelé pour delete) → pas de faux échec de création.
- **Palette de COULEUR en swatches** (à faire) : reste en slider. Le natif : `.pal`
  `머리\머리%s_%s_%d.pal` (fmt 0x01088ACC via `Job_GetHeadPalettePath 0x00d5a390`), 1ᵉʳ token
  ambigu + offset table RGBA de l'objet .pal non figé (peek x32dbg à faire). Puis moyenner
  les entrées non transparentes pour la couleur du carré, OU passer le .pal comme `palette`
  à kAtlasBuild pour teinter la coiffure.
- Optionnel : pré-remplir `del_email_` depuis MoonlightAuth si l'email du compte RO est
  connu, pour éviter la ressaisie.

  ## TODO batch (2026-07-25) — priorités demandées par le user
  ✅ FAITS ce jour : tooltip enrichi (classe via `JobName`, niveaux, HP/SP, zeny, stats,
  carte, del) ; aperçu doll CENTRÉ vertical dans la colonne gauche (mesure `create_form_h_`
  frame N-1) ; bouton « Créer / gérer (natif) » → **« Mode Classique »**.
  RESTE :
  - **Scaling baby sur le doll** : les classes baby doivent être plus PETITES. Le rendu
    in-world applique un scale par acteur (`this+0x14`/`this+0x50` dans `CActorSprite_Render
    CompositeJobSprite`) que `RenderDoll` ne pose pas. Fix = détecter classe baby (plage IDs
    baby / helper natif) et multiplier le budget hauteur de RenderDoll (facteur natif à RE,
    sinon ~0,85).
  - **Teinte des ICÔNES de coiffure + swatches couleur** : le DOLL applique déjà `hair_col`
    (via Actor_Init) ; seule l'icône grille est en niveaux bruts. Passer la `.pal` à
    `kAtlasBuild`. (couleur CORPS = NON réglable à la création : `CH_MAKE_CHAR 0x0a39` n'a pas
    de champ clothes_color ; serveur force 0 ; se change en jeu chez le styliste.)
  ## ✅ Coupons Renommer / Changer de slot — LIVRÉ (badge + menu contextuel + paquets)
  Offsets `CHARACTER_INFO` (RE `UINewSelectCharWnd_OnMsg 0x0079d610`) : **+0xa6 = moves (change
  slot)**, **+0xaa = rename** (u32, >0 = coupon actif ; le natif en fait un EFFET via
  `sub_D239E0(45/47)`, pas un contrôle UI). Lus dans `ReadSlot` (`moves_avail`/`rename_avail`).
  **Badge** ImGui en haut du perso (pastille or « Renom. » / cyan « Slot ») si coupon actif.
  **Clic DROIT** sur un perso → menu contextuel `##charctx` (items grisés si pas de coupon,
  label = compteur). ⚠ PAS d'inventaire au char-select -> on gate sur le FLAG, pas l'itemid.
  Paquets envoyés NOUS-MÊMES (comme create/delete) :
  - Rename = **`CH_REQ_CHANGE_CHARNAME 0x08fc`** SEUL (PACKETVER≥20111101 : `[op][CID.4]
    [name.24]` = 30 o ; le serveur `chclif_parse_ackrename` prend le nom DANS le paquet et pose
    `sd.new_name` lui-même -> PAS besoin de l'étape valid 0x028d). Succès déduit (slot renommé,
    `strcmp` vs `rename_old_`) ; échec via modale supprimée (`g_modal_suppressed_seq`) ou timeout.
    🔴 **NE JAMAIS envoyer `CH_REQ_IS_VALID_CHARNAME 0x028d`** : le client natif réagit à la
    réponse `0x028e` en PEUPLANT sa fenêtre de rename native (classe 0x80, `sub_8FA8D0` msg 34)
    avec le perso « sélectionné » de SON flux — NULL ici -> `SetText(null+0x6c)` = `strlen(0x6c)`
    -> CRASH (EIP 0x830400 CUIEdit_SetText, eax=0x6c). Vérifié live x32dbg. Même piège que les
    modales natives : émettre un paquet que le natif interprète le fait agir en état incohérent.
    🔴🔴 **POSER `kSelectedSlot` (0x015F8262) = slot renommé AVANT `0x08fc`** (idem move-slot
    avant `0x08d4`). À la complétion, la **case 352** de `UINewSelectCharWnd_OnMsg` lit
    `g_CharSelect_SelectedSlot` via dispatcher cmd 8 ; slot VIDE -> NULL -> `mov ecx,[eax=0]`
    (EIP 0x79df43) = access-violation (faux « freeze » : x32dbg halte sur l'AV first-chance,
    delete_breakpoint échoue car pas un bp, eax=0). MÊME règle que delete/create. Oubli =
    crash/freeze APRÈS un rename RÉUSSI. Vérifié live x32dbg (novice + coupon 12790, hors party).
  - Change slot = **`CH_REQ_CHANGE_CHARACTER_SLOT 0x08d4`** `[op][slot_before.2][slot_after.2]
    [remaining.2]` = 8 o. Popup liste les slots LIBRES -> clic = envoi `from=move_from_`,
    `to=slot libre`. Serveur `chclif_parse_moveCharSlot` bouge char_num from→to + re-push liste.
  Le coupon se consomme EN JEU (pose le flag). Membres : ctx_*/rename_*/move_* dans char_select.h.

  ## Transition d'entrée en jeu (glitch résolu)
  Après `EnterGame` (OnMsg 0xB8), le natif VIDE les `CHARACTER_INFO` pour semer l'état en jeu.
  Notre boucle de sièges continuait à lire les slots → on voyait ~½ s les dolls s'effacer et le
  slot retomber sur job 0 / sex 0 = **Novice femelle**. FIX = `enter_tick_` posé dans `EnterGame`
  puis, si `entering_`, `DrawDollAt`/sièges court-circuités par un **fondu au noir** (260 ms) +
  « Entrée en jeu… », `End()`+pops avant le loop (char_select.cc, après le décor).

  ## Scaling baby sur le doll ✅ FAIT
  Getter natif **`Actor_GetJobSpriteScale 0x00d7fd30`** (`float __stdcall(job)`) : 1.0 non-bébé ;
  **0.75** bébé 1re classe (4023-4029, 4045, 4191) ; 0.80 si `Job_NeedsLuaItemPosOffset` ; 0.82
  autres bébés. ⚠ Déjà appliqué à la CAPTURE (via `Actor_DrawSprites 0x7ac820` qui l'appelle) mais
  le **fit hauteur de `RenderDoll` le NEUTRALISE** (ré-étire le sprite bébé pour remplir la box) →
  bébés à taille normale. FIX = `DrawDollAt` ré-appelle le getter et **rétrécit la box** passée à
  RenderDoll (`dh = box_h * js`, pieds ancrés) → `s` retombe sur celle d'un adulte, bébé rend `js`
  fois plus petit. Correct que le scale soit baké ou non (hauteur rendue = box_h*js vs box_h).
  `Job_ToBabyClass 0xd83d60` = mapping normal→bébé (bébés = plage 4114-4324, valeurs de retour).

  ## Couleur cheveux = VIGNETTES « ma coupe en couleur N » ✅ (à tester)
  🔑 Les palettes couleur EXISTENT (l'user a montré le GRF) : **`palette\머리\head_<N>.pal`**
  (머리 = octets CP949 **B8 D3 B8 AE**), une palette 256×RGBA par couleur N (0,1,2,…). C'est ÇA la
  couleur cheveux (le RE `Job_GetHeadPalettePath`/`CActorSprite_RebuildHeadPalette` concernait la
  palette de BASE liée à coiffure/job — piste erronée pour la couleur).
  IMPLÉM propre (zéro std::string → plus de crash) : `hairicon::ColorPalette(N)` fait
  `snprintf("palette\\\xB8\xD3\xB8\xAE\\head_%d.pal", N)` → `kMakeKey`/`kLoadRes` (MÊME resource
  mgr que les icônes) → CPaletteRes, table à **+0x110**, renvoie cette adresse. `hairicon::Resolve`
  prend un `pal_override` : si fourni, il le passe à l'atlas (`kAtlasGetCached/Build`) au lieu de
  `spr+0x110` → **recolore** le sprite. `DrawHairIcon(hair,x,y,sz,color_override)`. La grille de
  couleurs (remplace le slider) rend la coupe SÉLECTIONNÉE recolorée par chaque N (IsRectVisible →
  seules les cases visibles buildent). Cache palette par N.
  ✅ Chemin OK (le GRF résout `palette\머리\head_N.pal`), atlas clé bien par palette (vignettes
  distinctes). 🔴 FIX couleurs-en-bouillie : `CPaletteRes` garde le BRUT RGBA à +0x110 et la
  palette CONVERTIE 16-bit-moteur à **+0x510** (via `sub_566770 0x00566770` à la charge :
  RGB888→format interne). L'atlas/`CSprite` utilisent la CONVERTIE (spr+0x110 d'un CSprite EST
  déjà convertie ; CPaletteRes non). Donc `ColorPalette` pointe **res+0x510**, pas +0x110.
  Grille couleur = **20 colonnes** (fenêtre élargie d'autant), ~8 lignes visibles.
  🔴 Palettes `head_0..head_6` du GRF MAL FAITES (quasi plates ; head_8==head_0 ; les head_0-8 ne
  varient que 10 index cheveux `40-47,62,148` ; head_9 = recolor total 237 index). → `kMinHairColor
  = 7` : le picking DÉMARRE à 7 (couleurs 0-6 retirées), défaut = 7. RÉGION palette tête (RE des
  .pal) : cheveux = idx **40-47** (dégradé) **+62 +148** ; FIXE (peau/visage/contour) = **25-30,
  129-135, 210-215** (+0 transparent). Vérité « index réellement utilisés » = pixels du `.spr`
  (bitmap indexé) — non parsé (jugé inutile). `head_N.pal` corrigés à la main (8 teintes) laissés
  sur disque `E:\...\data\palette\머리\` + `.bak`.
  ⚠ SÉPARÉ : le DOLL (aperçu gauche) ne recolore PAS bien selon la couleur (couleurs 0-8 = idem) —
  probablement parce que `CaptureDollActor` passe par le chemin acteur-FENÊTRE (FuncDrawWNDChar
  ctor 0x7ac210) qui n'applique pas le swap palette couleur (celui-ci vit dans CActorSprite in-world
  `RebuildHeadPalette`, PAS dans l'acteur fenêtre). À traiter séparément si besoin ; les VIGNETTES
  head_N.pal sont désormais la vraie réf couleur.
  🔴 **Tentative « vraie couleur » (helper `SwatchColor`) RETIRÉE — crash.** Elle échantillonnait
  la teinte dominante de la palette `.pal` native : `Job_GetHeadPalettePath 0x00d5a390`
  (`std::string* __stdcall(out, color, class, sex)`, format `%s_%s_%d.pal`) → resource mgr
  (`kMakeKey`/`kLoadRes`) → **CPaletteRes**, table 256×RGBA à **+0x110** (RE `CPaletteRes_Load
  0x00725b60` : lit 0x400 o dans this+272). CAUSE : crash EMPIRIQUEMENT lié au helper
  `SwatchColor` (seul code retiré → plus de crash). EIP dans notre ddraw au bloc swatches
  (chaîne « Couleur %d »), `edi`=0 → AV sur `[edi+0xB8]`. ⚠ Mécanisme EXACT NON confirmé :
  vérif live des conventions individuelles OK (`Job_GetHeadPalettePath` __stdcall,
  `std_string_dtor 0x004f08f0` __fastcall 1-arg lisant [+5], tous deux préservent edi) → l'hypo
  « edi=this corrompu » était FAUSSE (esi=adresse pile, pas un int ; edi ≠ this). Piste réelle :
  objet ImGui interne null OU état corrompu par l'interop `std::string` factice. Repro : perso →
  in-game → retour char-select → clic « Créer ». À REFAIRE seulement en session DEBUGGER LIVE
  (pas à l'aveugle).
  🔴🔴 DÉCOUVERTE CLÉ (RE `Actor_BuildSpriteLayers 0x007ae4e0`, case 2 = tête) : la palette tête
  native est chargée par `Job_GetHeadPalettePath(out, hairId, class, sex)` — **AUCUN argument
  COULEUR**. Indexée par (coiffure, job, sexe), PAS par la couleur de cheveux. Donc « lire le .pal
  de la couleur N » ne correspond à AUCUN fichier : la couleur cheveux est appliquée par un
  mécanisme SÉPARÉ (palette-swap au dessin, probablement via `hair_col` acteur +0x38, cf.
  `CActorSprite_GrayscalePalette 0x00c46550` ou overlay), NON identifié. Le doll affiche pourtant
  la bonne couleur → le mécanisme existe, ailleurs. Le chemin `.pal` corps est aussi assemblé via
  lookups natifs `Race_GetBodyPrefix6` + tables `g_JobName_*` + `Sprite_GetGenderToken` (std::string).
  ➡ CHAÎNE COULEUR CHEVEUX ENTIÈREMENT RE (via clif_changelook / LOOK_HAIR_COLOR) :
  `Net_OnSpriteChange_UpdateLookGlobals 0x00cd8e70` case 6 → acteur `vtable+136` = SetHairColor
  `0x00d3de00` → `this[271]=color` + `vtable+124` rebuild `0x00d3ded0` → `if(this[271])
  Job_GetHeadPalettePath(out, this[270]=coiffure, this[307]=job, this[152]=sexe)`. 🔴🔴 VERDICT :
  la couleur `this[271]` ne sert QUE de GARDE (`if`), elle n'est PAS passée au builder → **le
  chemin `.pal` tête est (coiffure, job, sexe), SANS couleur. Il n'existe PAS de fichier `.pal`
  par couleur de cheveux dans ce client.** Le changement de couleur recharge le MÊME fichier ;
  la couleur est appliquée par une TRANSFORMATION runtime (pas un fichier « palette N »). La vtable
  acteur : base `0x1093b48` (+5 sous-classes), +80 ApplyLook `0xd3e850`, +96 SetHair `0xd403a0`,
  +124 rebuildHead `0xd3ded0`, +132 SetClothesColor `0xd3dc50`, +136 SetHairColor `0xd3de00`.
  ➡ DONC vignettes vraie couleur = piloter le DOLL (seul chemin qui applique la transfo couleur),
  coupe fixe + couleur N, échantillonner. PAS de lecture `.pal` par couleur (n'existe pas).
  En attendant : SLIDER + doll (fait foi). Adresses/offsets valides.

  ## Reste du batch TODO (non fait / optionnel)
  - **Teinte des ICÔNES de la grille de coiffure** (pas les swatches, faits) : injecter la `.pal`
    couleur (palette CPaletteRes+0x110, cf. SwatchColor) comme arg `palette` de `kAtlasBuild`/
    `kAtlasGetCached` dans `hairicon::Resolve` (au lieu de `spr+0x110`). Différé (scope). Optionnel.

## ✅ Reprise du dernier perso joué (2026-07-27, CODÉ)
Le natif replace toujours l'autofocus sur le slot 0 → après une déco/reco rapide il
fallait re-désigner son perso. `EnterGame` mémorise désormais le **CID** (pas le slot :
un change-slot 0x08D4 ou un autre compte RO le rendrait faux) dans
`paths::LastCharsPath()` = `bourgeon_last_chars.txt` (liste récence, 16 max, plus récent
en tête — un compte Moonlight porte PLUSIEURS comptes RO, chacun retrouve le sien).
Autofocus = 1er CID de la liste présent parmi les slots, sinon 1er occupé. Lecture du
CID AVANT le pilotage natif (l'entrée en jeu VIDE les CHARACTER_INFO) et écriture du
fichier HORS `__try` (std::ofstream a un destructeur → C2712).

## 🔴 Écran mort « Entrée en jeu… » au 2ᵉ login du même processus (2026-07-26)
Spam d'Entrée au lancement → fondu noir « Entrée en jeu… » DÉFINITIF (jeu jamais entré).
Inspecté live (x32dbg) : mode = CLoginMode **état `+0x4 = 7`** (char-select), **transition
`+0xc = -1`** (aucune demandée), fenêtre native char-select bien **VIVANTE et visible**
(`+0x2c=0x115`, `+0x28=1`) SOUS notre fondu → notre `OnMsg 0xB8` était parti dans le vide,
et `entering_` (jamais réarmé hors `OnModeSwitch`) séquestrait écran + input.
**3 causes cumulées, toutes corrigées** :
1. **Sonde d'écran = les CHARACTER_INFO** (`nfilled>0`) : elles SURVIVENT au retour au login,
   donc au login suivant la table s'affichait DÈS l'envoi des identifiants (avant tout
   char-server). Fix = sonde `FindWindow(0x115)` à chaque frame.
2. **`this` pris dans le cache `mgr+0x3d4` (0x0131F8BC)** : jamais remis à zéro, vtable
   encore valable après destruction → la garde passait, on pilotait une fenêtre MORTE.
   Fix = `uiwnd::FindWindow(0x115)` dans `EnterGame` + `DriveNativeCtrl`.
3. **Entrées en file** (spam joueur + auto-confirm char-server qui postait jusqu'à 20
   `WM_KEYDOWN` en 1 s) consommées par la table à sa 1ʳᵉ frame. Fix = fenêtre
   d'insensibilité `kEnterGraceMs=400` + auto-confirm ralenti (200 ms, 10 essais).
Plus un **filet** : `entering_` + char-select natif toujours là après `kEnterTimeoutMs=2500`
⇒ la séquence n'a rien produit ⇒ on rend la main + bandeau. Le fondu est désormais traité
AVANT toute sonde (`DrawTransitionFade`, décor partagé `DrawHallBackdrop`).

## 🔴 COMPTE SANS PERSONNAGE (2026-08-11) — la salle doit s'afficher quand même
Symptôme : compte neuf ⇒ voile « Chargement des personnages… » 3 s, puis repli sur le
char-select NATIF **avec le clavier encore confisqué** ⇒ impossible de créer, ni chez
nous ni chez lui. Deux faits natifs l'expliquent :
1. **`cmd 8` est borné par `mode+0x1CC` = NOMBRE de persos** (`CLoginMode_SendMsg
   0x00d2a130` case 8 : `count<=0 -> 0`, sinon parcours de `count` × 175 o en
   comparant `+0x8A`). Compte vide ⇒ tous les slots `nullptr` ⇒ **indiscernable
   d'une liste pas encore décodée**. Fix : « aucun slot rempli » vaut COMPTE VIDE dès
   que le dernier témoin de salve a > `kDecodeGraceMs` (600 ms) → on dessine la salle,
   tous sièges libres, création par le popup ImGui.
2. 🔴🔴 **DEUX écrans natifs de création, et un compte vide ouvre CELUI QU'ON NE
   CHERCHAIT PAS.** Mesuré en jeu (inventaire `uiwnd::ListWindowIds`, liste
   `mgr+0x17C`) pendant la saisie du nom : fenêtres vivantes = **`[0x257,0x257,0x116]`**
   — ni `0x115` ni `0xC8` ! **`0x116` (278) = `UINewMakeCharWnd`** (ctor `0x0079F890`,
   vtable `0x0101DCC4`), montée par l'**état 8** du mode login (`0x00D254BA`) ;
   `0xC8` = l'ANCIEN `UIMakeCharWnd` (ctor `0x0086BC10`), ouvert par le ctrl `0x1A0`
   du char-select. États (`byte_D26808[état]`→`jpt_D267A4`) : **7=char-select
   (0x115), 8=création (0x116), 9=entrée en jeu**. Compte vide ⇒ état 8 direct ⇒
   `0x115` JAMAIS construite. Donc `CharSelectWindowPresent()` ne veut pas dire
   « on est arrivé » : `MoonlightAuth::charsel_reached_` doit se latcher sur
   `0x116` AUSSI (sinon l'auto-confirm poste des Entrées dans l'écran de saisie).
   Un aller-retour par un de ces écrans ne périme PAS la liste (`screen_gone_tick_`
   non repoussé) : au retour le serveur n'envoie que `HC_ACCEPT_MAKECHAR`.
   🔴 **LEÇON GÉNÉRALE** : une sonde `FindWindow(id)` ne répond qu'à qui connaît
   déjà l'id — inutile pour « quel écran natif est là ? ». D'où (a) le FILET dans
   `NativeScreenHasKeyboard` (salve char-server passée + on ne couvre pas ⇒ le natif
   a le clavier, quel que soit l'écran) et (b) **`uiwnd::ListWindowIds()`**, qui
   énumère les fenêtres vivantes (nœuds {suivant,précédent,fenêtre}, id à `+0x2c`).
Ajouts : témoin de salve **`0x082D` HC_ACCEPT_ENTER2** (même salve que la liste, seul
porteur d'info quand la liste est vide) ; Entrée d'auto-confirm (canal `PostGameKey`)
**jetée** si `0x115` existe déjà (sinon elle clique le bouton par défaut = « Créer »).
Clavier : cf. [[feedback_native_replacement]] (`Covering()` + relâche explicite).

**FRAÎCHEUR DE LA LISTE (2026-07-28, `18019f7`, testé en jeu).** Les
CHARACTER_INFO SURVIVENT à la déconnexion : la table s'affichait avec les
personnages de la session PRÉCÉDENTE dès que la fenêtre 0x115 existait, et la
seule protection était `kEnterGraceMs` = 400 ms — un pari sur la latence du
char-server, que l'auto-confirm de MoonlightAuth (qui poste des Entrées) pouvait
perdre. On s'arme désormais sur un FAIT : réception de 0x006B HC_ACCEPT_ENTER ou
0x0B72 HC_ACK_CHARINFO_PER_PAGE, OBSERVÉS (forward_len = 0, aucun octet lu).
⚠ Comparaison de TICKS et non un booléen : le paquet arrive AVANT que la fenêtre
native n'existe, donc un drapeau remis à zéro « écran absent » s'effacerait juste
après réception. Règle : fraîche si arrivée APRÈS la dernière SORTIE d'écran.
⚠⚠ Ce front est AUSSI posé dans `OnModeSwitch(non-login)` : `OnRenderLoginUI`,
qui le détecte, n'est plus appelé une fois en jeu — sans ça le bug restait entier
au changement de personnage, le cas le plus fréquent.
Repli délibéré : passé 3 s sans paquet, on s'arme quand même + LogError (un
mauvais opcode rendrait sinon la table invisible à jamais) ; pendant l'attente le
char-select NATIF reste à l'écran, le joueur n'est jamais bloqué.
`RegisterObserveOpcode` voit bien les paquets du char-server : il ne touche pas
la table de dispatch, tout passe par `PacketBufReaderHook`, posé sur une
FONCTION et non sur une instance de connexion.
