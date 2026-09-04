# Fenêtre Groupe/Amis : RE et portage ImGui

> Journal du chantier. La fiche de mémoire `project_party_friend_window_re` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

✅ **LIVRÉ le 2026-08-23** (commit `e093c8d`) : la fenêtre à deux onglets, le menu
contextuel, les réglages de groupe, les deux popups reçues ET le HUD en grille.
⚠ La note « pas encore compilé ni testé » ci-dessous datait de la rédaction, pas
de la livraison. **Traductions rattrapées le 2026-08-24** : 62 des 66 clés qui
manquaient aux deux catalogues venaient d'ici (cf. [[project_i18n_language_setting]]).
🔴 **Reste un seul détail** : la COULEUR DU NOM (`+0x44`) est lue
(`party_friend_window.cc`, `out.color`) mais jamais rendue — l'encodage RGB/BGR
n'a pas été tranché.
⚠ Défaut latent repéré et NON corrigé : `OpenPopup(i18n::Tr("Confirmation"))` et
son `BeginRoPopupModal` s'apparient par un titre TRADUIT, sans `###`. Inoffensif
tant que les deux appels sont à deux lignes d'écart dans la même frame, mais
c'est exactement le piège corrigé le même jour dans `integrity_check`.

🔴 **CHANTIER OUVERT le 2026-08-23** : portage ImGui, lot 1 = la fenêtre 0x45 (onglets
Amis + Groupe), lot 2 = les dialogues, le HUD mini-party en chantier séparé.

**Reste à porter (ordre voulu par l'utilisateur)** : 1. les popups REÇUES ✅ (faites) ·
2. **les réglages de groupe** (`UIPartySettingWnd`) · 3. ⛔ **PAS les options d'amis** — les 3
cases sont DÉJÀ dans les réglages de la CHATBOX (globales 0x015fb2f8/2fc/300, chat_window),
ne pas en faire un doublon · 4. le HUD de groupe, mais **en GRILLE moderne façon WoW**
(raid frames), PAS une transposition du mini-party natif.

**État au 2026-08-23** : `features/windows/party_friend_window.{h,cc}` écrit — TEMPS 1
(lecture seule : les 2 onglets, aucun paquet émis), branché partout (CMakeLists, bourgeon.h/.cc,
moonlight_ui `partyfriend_imgui`/`partyfriend_tab` dans le groupe `SetModernInterface`,
et le point d'ouverture au `case 0x45` de window_pos_tweaks). ⚠ **PAS ENCORE COMPILÉ NI TESTÉ**.
Reste : temps 2 = les actions (menu contextuel). Volontairement PAS affichés faute de preuve :
le **niveau** (+0x4A, `sprintf` variadique) et la **couleur du nom** (+0x44, encodage RGB/BGR
non tranché) — les deux sont lus/documentés mais pas rendus. **RE consignée dans
`docs/party_friend_re.md`** — la lire AVANT de coder, elle corrige deux erreurs de la 1re passe.

🔴🔴 **DEUX ids, TRANCHÉ le 2026-08-23** (l'ancienne note « 0x22 à reconfirmer » est résolue) :
**0x22 = l'id de FABRIQUE** (case 34 @0x00a3aeac — seul constructeur du binaire ; le ctor
0x00701fc0 n'a QU'UN appelant), **0x45 = l'id d'ENREGISTREMENT** (FindWindow/CloseWindow).
Le case 69 (0x45) n'est qu'un point d'entrée : il **rappelle MakeWindow(0x22)**, envoie
OnMsg(6, 0xD7, 1|2) pour choisir l'onglet selon `mgr+0x740`, et **sort par le `default` sans
retourner de fenêtre**. ⇒ Un hook de création sur 0x45 est MUET ; il faut accrocher **0x22**.
(Erreur commise puis corrigée le jour même : le premier jet hookait 0x45, la fenêtre aurait été
inouvrable.) ⚠ Reste à vérifier en jeu : le case 34 ne construit que si `mgr+0x2C8` est nul.

🔴🔴 **`g_Own_InParty` (0x015FF804) MENT** — mesuré en jeu : il reste à **0** pour un membre qui
a REJOINT un groupe (le natif s'en sert pourtant comme garde de « quitter »/« expulser »).
⇒ pour « suis-je en groupe ? », compter les MEMBRES (`Social_GetPartyMemberCount` / la liste),
jamais ce drapeau. Six écrivains, aucun tranché.

Trois faits mesurés le 2026-08-23 qui décident du design :
- 🔴 `DrawContent` appelle les accesseurs `Social_*` **à chaque frame** sur le manager
  0x015FA3C0 ; sa liste `+0xFC` n'est qu'un miroir. ⇒ **tuer la native n'assèche RIEN**
  (contrairement à [[reference_trade_window_re]], où on remplaçait les handlers d'opcodes).
  Source de données = le natif, PAS les paquets.
- 🔴🔴 **HP d'un membre = le `CPc` de l'ACTEUR** (`UpdateMemberHpGauges` 0x00705b40 :
  `dynamic_cast<CPc*>(ActorList_FindByGID(...))`, jauge masquée si l'acteur manque). Hors de
  portée ⇒ AUCUN HP, y compris dans le natif. C'est une limite du CLIENT. Pour COMBLER :
  écouter `ZC_NOTIFY_HP_TO_GROUPM` 0x0106 et tenir notre cache.
- Les 3 commandes `CMode::SendMsg` du hub, vérifiées contre Hercules `src/map/packets.h` :
  **0x104 → CZ 0x07DA** passer chef (AID) · **0xB0 → CZ 0x0203** retirer un ami (AID, CID) ·
  **0x3D → CZ 0x0100** quitter le groupe. 🔴 Le 0x3D n'est PAS un simple envoi : il transfère
  d'ABORD le leadership à un membre en ligne sur MA map (`SendMsg(0x104, …)`), sinon modale
  msgstring 0xCB9 (continuer seulement si la réponse == 0xBB). À reproduire, sinon un chef qui
  part laisse le groupe sans chef.
- Entrée sociale = **0x50 o, même type amis et groupe**, data à `nœud+8`, copy-ctor 0x00701df0 :
  +0x04 GID/AID, +0x08 2e id (clé du getter 0x00d5d740), +0x0C nom, +0x24 map, +0x3C **0 = chef**,
  +0x40 hors-ligne, +0x44 couleur, +0x48 job, **+0x4A niveau**.
- 🔴 Niveau et PV passent par un `sprintf` **variadique** : le décompilateur ne montre pas leurs
  arguments, il faut le DÉSASSEMBLAGE. Tranché ainsi le 2026-08-23 : niveau = `[nœud+0x52]` =
  data+0x4A (@0x0070433d) ; **PV = lus sur la JAUGE enfant** `this+0x128[i]` en +0xA0/+0xA4
  (@0x00704622) — les mêmes offsets que sur l'acteur, donc notre chemin acteur+0x488 est
  équivalent. Couleur (+0x44) : offset sûr, ENCODAGE RGB/BGR non tranché ⇒ pas affichée.
- **Icône de classe** = `유저인터페이스\renewalparty\icon_jobs_<job>.bmp` (job = data+0x48, lu
  `[esi+50h]` @0x0070622a), variante `_die` pilotée par un flag ≠ hors-ligne, non identifié.
  Nom de classe = `Job_GetDisplayNameOrResName(mgr, job, 99)` 0x00d5bb40 — **rend bien un nom
  AFFICHABLE** (table mgr+0xF88) malgré son nom ; le 3e arg est un SEXE, 99 ⇒ classe de base.

Fenêtre **Amis/Groupe** du client 20250716 (source `Contents\AdventurerAgency\`). Tout renommé+commenté en Ghidra (préfixe `UIMessengerGroupWnd_*` / `UIDragMiniPartyWnd_*`).

## Fenêtre principale = UIMessengerGroupWnd (window id **0x45**)
UNE seule classe à **onglets** rend Amis ET Groupe ET liste de sorts, multiplexée par le champ mode.
- vtable **0x01010e2c** (renommée `vtable_UIMessengerGroupWnd`), COL 0x010b7aa8, type-desc 0x01239838.
- ctor `UIMessengerGroupWnd_ctor` (ex-UIWnd_id45_ctor). dtor `_dtor` 0x00702450.
- **OnCreate** 0x00702970 (vtable+0x3c) : crée 40 jauges HP (60×5, this[0x4a..]), 40 boutons job (50×50, this[0x72..]), scrollbar this[0x2e], titre this[0x3e], 5 boutons barre d'outils this[0x32..] (libellés AMIS {0x62,0x63,0x64,0x163,0x15f} vs GROUPE {0xdba,0x63,0x64,0x65,0x61} via FUN_00a9ed30), bouton maximiser this[0x9f], bouton resize this[0x3d] ; finit par OnMsg(0x17)=rebuild.
- **DrawContent** 0x00703d10 (vtable+0x50) : dessine selon mode (voir champ +0x28c).
- **OnMsg** 0x00705cc0 (vtable+0x94) : hub. case 0xe=layout/resize, 0x17=rebuild liste, 0x22=clic membre, 0x30=item menu choisi, 0x31=build menu contextuel (popup wnd 0x12), 0xca=minimise/0x20b=maximise, 0xd7=switch onglet, 0xe7..0xec=actions (add ami/invite groupe/whisper/expel), case6 sous-param boutons.
- Souris : `_OnLButtonDown` 0x007050a0 (barre titre→move ; sinon **détache un membre en UIDragMiniPartyWnd**), `_OnLButtonUp` 0x00705730, `_OnMouseMove` 0x00705420 (tooltip "%s(%s)"+"Lv.%d"), `_OnMouseEnter` 0x007053c0, `_OnRButtonDown` 0x007057a0 (→OnMsg 0x31 menu), `_OnMouseWheel` 0x00705870 (scroll).
- Helpers : `_GetEntryInfoAt` 0x00702880, `_UpdateScrollbar` 0x007058c0, `_LayoutTopButtons` 0x00705980, `_UpdateMemberHpGauges` 0x00705b40, `_WriteGeomToRecord` 0x007080d0.

### Carte des champs (this+)
- **+0x28c = MODE** : 0=AMIS, 1=GROUPE. 🔴 **IL N'Y A PAS DE MODE 2** — la note « 2 = liste
  de sorts » de la première passe était un contresens né du faux nom `g_SkillInfoMgr` donné au
  manager (qui n'est qu'un sac d'état d'interface). Mesuré le 2026-08-23 : sur les 17 fonctions
  de la classe, `+0x28C` n'est comparé qu'à **0 ou 1** (35 accès), l'unique écriture dynamique
  est le case 0xd7 (bascule d'onglet) en 0x00707552, qui boucle sur `this+0xBC`..`this+0xC4`
  — soit **exactement deux** onglets — et y range l'indice de boucle ; `OnCreate` initialise à
  1 en 0x00702d17. `DrawContent` n'a que deux branches : `mode==1` → groupe, sinon → amis.
- **+0x268** = 0 maximisé (taille 0x111×0x146=273×326) / 1 minimisé (0xf3×0xb3=243×179).
- +0xb4 = record WNDINFO persisté (pos/taille), +0xfc = liste chaînée circulaire des entrées (nœud 0x58, data à +8), +0x100 = nb entrées.
- +0x104/+0x108 = vector<string> du menu contextuel. +0x114 = scroll (1re ligne), +0x118 = colonnes, +0x11c = lignes visibles, +0x124 = hauteur ligne (déf 0x17=23), +0x120 = index survolé (-1=aucun), +0x110 = flag scroll actif.
- +0x128[..40] = jauges HP enfants, +0x1c8[..40] = boutons icône job / lien. +0x26c = table géométrie par mode. +0x278/+0x27c = 2 onglets. +0xd4/+0xd8 = boutons relabelisés par onglet. +0xf8 = texte titre. +0xa3 = 1 si onglet groupe.
- Globals : 🔴 0x015ff908/0x015ff90c ne sont PAS « mes coords écran » (autre contresens de la
  première passe) mais **`g_Own_Hp` / `g_Own_MaxHp`** — OnMsg s'en sert pour la ligne du joueur
  dans la liste du groupe. Mon AID DAT_015fb9a4, session 0x015fa3c0, drag-global DAT_015fff10. Popups liés : DAT_0131f824 (sous-liste amis), DAT_0131f7f4 (sous-liste groupe), DAT_0131f914 (message).

### Source de données sociale (manager "g_SkillInfoMgr", multi-usage)
Accès amis/groupe (arg = session 0x015fa3c0) : nb amis FUN_00d5ce20 / get ami FUN_00d5a0d0 ; nb membres groupe FUN_00d5cf50 / get membre FUN_00d5da80 ; get info FUN_00d5c850. (Détails affinés par sous-RE — voir Ghidra.)

## Barre HUD membre = UIDragMiniPartyWnd (créée en glissant un membre hors du groupe)
- vtable **0x01010adc** (`vtable_UIDragMiniPartyWnd`), COL 0x010b7968, type-desc 0x012397b4.
- ctor `_ctor` 0x00700620(record, gid, ...), dtor 0x007008d0, vecdtor 0x00700840.
- **DrawContent** 0x00700ae0 : icon_jobs_%d[_die].bmp + ico_partyCrown si chef + "%d %s(%s)" + HP "%d/%d" + icon_party_me/on/off.
- `_BuildHpGauge` 0x00700980 (jauge HP 60×5 à +0x104), `_OnDragMove` 0x00701020 (trouve wnd id 0x22, délègue drop FUN_00702500).
- Champs : +0xb8=GID, +0xc0=nom, +0xd8=map, +0xf0=chef, +0xf4=hors-ligne, +0x100=mort, +0x104=jauge HP, +0x1c/+0x20=pos.

## Assets (dossier `\renewalparty\` + `\basic_interface\`)
bg_partymember.bmp, ico_partyCrown.bmp, icon_party_me/on/off.bmp, icon_jobs_%d[_die].bmp, img_friend1/2.bmp, mesbtn_partymaster_*. Records persistés lua : PARTYSETTINGWNDINFO.*, FRIENDOPTIONWNDINFO.*, ADVEN_PARTYBOARDWNDINFO.*.

## HUD conteneur du groupe = UIMiniPartyWnd (window id 0x12d)
- vtable **0x010113a4** (`vtable_UIMiniPartyWnd`), COL 0x010b7b28, type-desc 0x01239880.
- ctor `UIMiniPartyWnd_ctor` 0x0070aa30 (alloc 0x138, sous-fenêtre membre 0xae×0x34 = 174×52), dtor 0x0070ad30.
- DrawContent 0x0070af20 (vtable+0x50), OnMsg 0x0070b470 (vtable+0x94 : 0x17=ingérer record membre, 0xe/param7=changer orientation HUD, 0x06 clic→wnd 0x22, 0x30/0x31 forward).
- Peuplement : `UIMiniParty_PopulateAllMembers` 0x00a339c0 (boucle sur nb membres cache DAT_015faad8), `UIMiniParty_FindMemberWndByAID` 0x00a33140, `UIMiniParty_SpawnMemberWnd` 0x00702670.
- **UIMiniPartyInfoWnd** (type-desc 0x0123985c) = classe de BASE de UIMiniPartyWnd (pas d'objet séparé, vtable partagée).
- Champs partagés avec UIDragMiniPartyWnd : +0xb8 AID, +0xbc GID, +0xc0 nom, +0xd8 map, +0xf0 chef, +0xf4 hors-ligne, +0x100 mort, +0x104 jauge HP, +0x108 fenêtre liée, +0x110 orientation (0/1/2).

Note id : la classe UIMessengerGroupWnd est construite par le factory via le ctor labellisé id **0x45** ; le chemin « détacher un membre en HUD » fait `FindWindow(0x22)` sur le conteneur (méthode drop `UIMessengerGroupWnd_OnMemberDrop` 0x00702500). Les deux ids coexistent — 0x22 à reconfirmer live.

## Source de données sociale (session 0x015fa3c0, ex-"g_SkillInfoMgr")
- `Social_GetPartyMemberCount` 0x00d5cf50 (*(mgr+0x17c0)), `Social_GetPartyMemberByIndex` 0x00d5da80 (liste mgr+0x17bc).
- `Social_GetFriendCount` 0x00d5ce20 (*(mgr+0x17c8)), `Social_GetFriendByIndex` 0x00d5a0d0 (liste mgr+0x17c4).
- `Social_GetMemberInfoById` 0x00d5c850 (map mgr+0x714), `Social_GetMapDisplayName` 0x00d5bcf0, `Social_GetJobIconResName` 0x00d5bb40.

## Fenêtres de gestion / dialogues (tout renommé+commenté Ghidra)
Framework : dtor=slot0, DrawContent=vtable+0x50, OnMsg=vtable+0x94, OnCreate=vtable+0x3c. Libellés FUN_00a9ed30(id), réseau g_UICommandDispatcher(0x0121333c)+0x18.

🔴 **Réglages de groupe — CORRECTION 2026-08-23** : les deux dernières globales étaient
INVERSÉES dans la note ci-dessous. Tranché par les clés msgstring des radios (OnCreate crée les
3 paires dans l'ordre) et recoupé avec le serveur :
`0x015FF840` = **EXP** (`MSI_EXPDIV`, 0x11F/0x120) · `0x015FF844` = **RAMASSAGE**
(`MSI_ITEMCOLLECT`, 0x121/0x122) · `0x015FF848` = **PARTAGE d'objets** (`MSI_ITEMDIV`,
0x2E3/0x2E4). Envoi = `SendMsg(0x103, exp, pickup, share)` -> **CZ 0x07D7**
`{exp:4 @2, pickup:1 @6, share:1 @7}` (moonlight `clif_parse_PartyChangeOption`, qui exige
d'être CHEF et compose `itemflag = pickup|share<<1`). `CZ 0x0102` = variante exp SEUL (case 96).
⚠ La création se fait en DEUX temps chez le natif : case **0x11D** mémorise le nom dans
`0x015FF84C` (ce n'est PAS un renommage), puis case **0xA8** valide et envoie CZ 0x01E8.

- **UIPartySettingWnd** vtable 0x01034174, OnMsg 0x008c63e0, OnCreate 0x008abf80, DrawContent 0x008b5950. 1 classe / **3 ids** (mode this+0xe0) : 0xb4=créer groupe(mode5), 0xdc=params groupe(mode0x32), 0xf5=agence(mode0x46). OPTIONS (radios) : partage EXP this+0xb4/b8→sel +0xbc (init DAT_015ff840), partage OBJET +0xc0/c4→+0xc8 (DAT_015ff844), ramassage +0xcc/d0→+0xd4 (DAT_015ff848) ; nom groupe edit +0xdc. Réseau cmd 0x11d (rename)+0xa8 (settings), reset msg 0x3c.
- **UIFriendOptionWnd** vtable 0x01010c70, OnMsg 0x00701560, OnCreate 0x00701270 (Alt+I). 3 cases (écrites msg 0xd5) : +0xc0→g_FriendOpt_BlockGuildNotify 0x015fb2f8, +0xbc→g_FriendOpt_BlockFriendNotify 0x015fb300, +0xb8→g_FriendOpt_ConnectNotifySound 0x015fb2fc. Msg6 id0xc9→toggle wnd 0x44.
- **UIJoinPartyAcceptWnd** vtable 0x0103382c, OnMsg 0x008c46d0 : id0xb8=accepter/0xb9=refuser → cmd **0x3c** (partyid +0xb4). ferme wnd 0x23.
- **UIJoinFriendAcceptWnd** vtable 0x01035254, OnMsg 0x008c4030 : id0xb8/0xb9 → cmd **0xaf** (aid +0xc0, cid +0xc4). ferme wnd 0x6d.
- **UIPartyInvitationToWnd** vtable 0x0103424c, OnMsg 0x008c6220 : OK → cmd **0x3b** (invite par nom, edit +0xe4).
- Agence d'aventuriers (catalogue) : UIAdvenPartyBoardWnd (vtable 0x0101a1f8, OnMsg 0x007861a0, booking mgr DAT_01251750, wnd 0x143/0x144/0x159), UIRegisterPartyWnd (0x01019e90/0x00787190), UIApplyForPartyWnd (0x01019f68/0x00786f60), UIRequestJoinPartyWnd (0x0101a2d0/0x007874e0, cmd **0x108**), UISeekPartyWnd (0x010377e4/0x00900500, wnd 0xaa), UIPartyInfoWnd (0x0101a040/0x00785c10).

## Leviers de customisation (pour plugins Bourgeon)
- Plugin `FindWindow(0x45)` → lire/écrire taille, hauteur ligne +0x124, colonnes +0x118, couleurs de texte (DrawContent).
- HP en % au lieu de "%d/%d", masquer membres hors-ligne, recolorer la barre HP par classe.
- Mini-barres HUD (UIDragMiniPartyWnd) : sauvegarder/restaurer positions, échelle, alpha.
- Voir aussi [[reference_ui_window_manager]] (MakeWindow/FindWindow/Toggle), [[reference_status_icon_bar]] (opacité via hook render), [[feedback_build_and_git]] (instrumenter dans le DLL).
