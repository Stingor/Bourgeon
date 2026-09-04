# Système de cible : sélection, flèche, HUD de cible

> Journal du chantier. La fiche de mémoire `project_target_system_re` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

RE complète du **ciblage / sélection d'entité**, faite pour préparer un « target system »
(TODO utilisateur, 2026-08-01). **Doc de référence : `docs/target_system_re.md`** — tout y est
(champs, 5 sites d'écriture, routage INF, briques, conception proposée, pièges). Résumé ici.

✅ **Le HUD de cible EST implémenté** (2026-08-19) : `src/features/overlays/target_frame.{h,cc}`,
opt-in « target_frame » du groupe « Interface moderne ». 🔴 **Pas une fenêtre à barre de titre** —
l'utilisateur a explicitement demandé le style « barres d'infos + portrait » : CINQ CADRES LIBRES
(portrait, nom+niveau, barre PV, barre SP, race/élément), chacun avec position/taille/couleurs/on-off,
verrou (fige + clic-traversant), aimantation sur la grille partagée, mode placement pour les poser
sans cible. La mécanique du cadre est FACTORISÉE dans `src/ui/hud_frame.{h,cc}` — ✅ **dette SOLDÉE le
2026-08-24** : les barres et les étiquettes du portrait de BasicInfo y ont migré (−289 lignes dans
`basic_info.cc`), et leur géométrie est désormais un `ro::HudRect` embarqué (`bar.rect`, `e.rect`),
pas quatre `int` maison. 🔴 Deux écarts ASSUMÉS et alignants : les barres gagnent le **bloc CTRL**
(les portraits l'avaient déjà), et les textes de cadre se **tronquent** (« ... » ASCII, jamais U+2026)
au lieu de déborder sur le voisin. 🔴 Le liseré d'une BARRE reste peint par l'appelant, pas par
`opts.border` : le module le trace AVANT le contenu, et une jauge pleine et arrondie le recouvrirait.
Au passage, `force_apply_` était écrit deux fois et **lu nulle part** — supprimé.
Le PORTRAIT tire de deux moteurs : `ro::LoadMobSprite` (monstre) et `ro::DrawDoll` (joueur) ; pour ce
dernier, l'apparence se lit sur l'ACTEUR — carte d'offsets dans `docs/target_system_re.md` §4 ter,
🔴 sexe = +0x260 (et NON +0x4C8 = body style : mislabel `SetSexAndRebuildEquip` corrigé).
Détail : `docs/target_system_re.md` §4 ter.

🔴 **Le SP d'une entité tierce n'existe dans AUCUN paquet du jeu — mais le RENDU natif était prêt.**
`UIMonsterGage` (ctor 0x008361B0) naît en mode DEUX BARRES (`+0x9C = 1`) et `UIPcGage_Paint` 0x008549A0
peint la barre du bas depuis `+0xA8`/`+0xAC` ; seulement, les TROIS appelants de
`UIPcGage_SetGaugeBottom` 0x008645B0 passent tous `(0,0)`, et aucune écriture directe de ces champs
n'existe ailleurs (balayage exhaustif du .text). Côté serveur, le SP ne part que pour soi/homon/merc/elem.
⇒ Les patchs WARP qui « câblent le SP sous les HP » ne peuvent que remplir ces champs : il leur faut un
serveur qui l'envoie. D'où **CZ 0x0F29 → ZC 0x0F2A** (`clif_parse_bourgeon_target_info`), fixe 34 o,
`known` = masque de ce qui est renseigné, **pas d'abonnement** (le client redemande toutes les 400 ms,
zéro état serveur), **gate PVP** : PV/SP d'un autre joueur (ou de son compagnon, en suivant le maître)
réservés à sa party/guilde.

⚠ Les PV, eux, sont GRATUITS côté client : `ActorMgr_GetActorHp` 0x00A69EE0, et les deux jauges
d'acteur — `+0x300` (UIMonsterGage, alimentée par ZC_HP_INFO 0x0977, `monster_hp_bars_info: yes`) et
`+0x488` (UIPcGage, msg 34) — avec PV en `+0xA0`, max en `+0xA4`.

⚠ Pour un MONSTRE, `clif_name` DÉTOURNE la plaque de nom : party = « Lv. X | HP: Y% » (`show_mob_info: 6`),
guilde = **race**, rang = **élément**. C'est la source instantanée, sans un paquet.

## Les 5 faits qui structurent tout
1. 🔒 **`CGameMode+0xF4` = sélection PERSISTANTE** (AID de la dernière entité cliquée). Recherche
   EXHAUSTIVE des écritures : 5 sites, dont **un seul nettoyage — `GameMode_OnEnterMapSetup`
   0x00C6BE20, au changement de map**. Les 4 autres écrivent l'AID (`GameMode_RepeatActorAction`
   0xC7761C, `CursorMgr_UpdateHover` 0xC78E67/0xC7903B, `sub_C79610` 0xC79D3C).
   🔴 **RIEN ne l'efface à la mort de la cible ni au clic sol ⇒ FANTÔME** : revalider par
   `ActorList_FindByGID` (0x00a69eb0) chaque frame, vider sur null.
2. ⚠ **`CGameMode+0xF0` = cible de TRAVAIL, éphémère** — vidée au relâchement, dès que la souris
   BOUGE bouton relâché, ou à la mort. NE JAMAIS bâtir une sélection UI dessus. C'est elle qui porte
   la **répétition** native (`GameMode_RepeatActorAction` 0x00C77120) ⇒ explique pourquoi un sort
   CIBLÉ se relance en maintenant la touche alors qu'un sort de ZONE non (aucune cellule mémorisée).
   🔴🔴 **Et le VOISIN `+0xF8` NE S'ÉCRIT PAS — l'imiter a coûté un bug (2026-08-22).** Il ne dit
   pas « on vient de cliquer » comme la doc le croyait : il dit **« le maintien de souris en cours
   n'a PAS été capturé par un acteur »**. Mis à **1** au RELÂCHEMENT du bouton gauche
   (`GameMode_ProcessMouseWorldInput` 0x00C76538, `LButtonState == 3`) et à l'entrée de carte ; mis
   à **0** par les 7 sites de clic/survol SUR ACTEUR ; **lu** par le chemin « bouton MAINTENU » de
   `GameMode_GroundClick_RequestMove` (0x00C76236), où la marche n'est ré-émise que si
   `+0xF8 != 0`. ⇒ `WriteNativeSelection` le remettait à 0 « comme le chemin natif du clic » :
   **prendre une cible au clavier arrêtait net la marche au clic maintenu**, réparable seulement en
   relâchant puis recliquant — le chemin « appui FRAIS » ne le consulte jamais, et c'est ce qui
   masquait la cause. Le cyclage ne capture aucun geste de souris : seul `+0xF4` le concerne.
   ⚠ Leçon transposable : **imiter un chemin natif « à l'identique » n'est sûr que si l'on sait ce
   que chaque écriture VEUT DIRE.** Deux champs voisins, deux questions différentes.
3. 🔴 **AUCUN bandeau de cible, AUCUN HP côté client.** `0x00c76890` (renommée
   `GameMode_UpdateSelectedTargetNameLabel`, ex-mislabel `UpdateTargetInfoBar`) ne fait que
   réafficher **le même `UIActorNameLabel`** que le survol, pour `+0xF4`. Le « HP: 63% » vu à
   l'écran = **moonlight l'injecte DANS LE NOM du mob** (capturé live :
   `"10Def 10Mdef Small Norm (Lv. 10 | HP: 63%)"`). ⇒ barre de vie possible en parsant (fragile) ou
   via un opcode custom `{GID, hp, maxhp}` (propre). Party = `ZC_NOTIFY_HP_TO_GROUPM` déjà structuré.
   Widget : ptr `GameMode+0x2AC`, lignes `+0x84/+0x88` (std::string 24 o), couleur `+0x90`,
   ⚠ **`+0xa0` est une string VIDE, ce n'est pas le nom**.
4. **Armer = 100 % CLIENT, le serveur n'autorise rien.** `0x71` lit l'INF local, envoie `0x47` puis
   `0x48` (mode dans `+0x408`, id `+0x40C`, niveau `+0x414`) — **aucun paquet**. Le paquet ne part
   qu'au CLIC ; SP/portée/cooldown/légalité sont validés serveur seulement là. Cast sur cible
   imposée = `SendMsg(0x45)` (arg3 = GID) ⚠ **ne valide pas le type** (son propre GID = tout part
   sur soi). Cf. [[reference_cmode_sendmsg_use_skill]].

5. 🔴 **La sélection a un SECOND rendu : la petite flèche blanche 3D** au-dessus de l'entité —
   le `CMousePointer` en **`CGameMode+0xD4`** (vtable `0x0108F928`, `cursors.spr`/`cursors.act`
   **action 3**), un nœud de scène permanent créé à l'entrée dans le monde. C'est **la même
   `0x00c76890`** qui l'allume, chaque frame : `SendMsg(4, acteur.x, acteur.z, 0)` (case 4 →
   `GameMode_ShowSelectionMarkerAt` 0x00C82A90 → msg 1 : pose + `+0xA0 = 1`) ou `SendMsg(5)`
   (→ `GameMode_HideSelectionMarker` 0x00C6B6B0 → msg 2 durée 0 = masqué). Émetteur **unique**
   (recherche exhaustive 0x00a00000-0x00d50000). ⇒ elle marque la **dernière entité CLIQUÉE**,
   donc aussi un joueur ou un NPC. « Elle apparaît quand on attaque » est un effet de bord :
   attaquer, c'est cliquer.
   🔴🔴 **MAIS elle ne PERSISTE PAS, et écrire `+0xF4` ne l'allume PAS** (affirmé ici jusqu'au
   2026-08-20, démenti en jeu quand le cyclage clavier n'a rien allumé). `0x00c76890` commence par
   un VERROU : `CGameMode+0x28` nul ⇒ `SendMsg(5)` immédiat, sans même lire la sélection
   (`0x00C76C9A`). Ce champ est l'**engagement SOURIS** : levé par le **message 26** du mode
   (`0x00C884B9`, émis au clic sur un acteur), il retombe au premier mouvement de souris hors de la
   cible bouton relâché (`0x00C757C4`). Donc **deux durées de vie, pas une** : `+0xF4` jusqu'au
   changement de map, la flèche le temps de l'engagement.
   ⛔ **Ne PAS forcer `+0x28`** : le natif y lit un engagement et colle le curseur « attaque » sur
   TOUT l'écran, réécrit chaque frame (`0x00C7571E`). ✅ La bonne prise est de **réécrire le message
   5 en message 4** dans le hook de `CMode::SendMsg` (déjà posé : `ProcessInput: 0x00c86740`) —
   émetteur unique, bon instant de la frame, aucun champ natif touché. Livré :
   `TargetFrame::WantsSelectionMarker` + `Hooked_ProcessInputMsg`, réglage « Flèche de ciblage du
   jeu sur la cible ». ⚠ Reproduire les filtres du natif, **cloak compris** (virtuelle acteur
   `vtable+0x34` & 4) : sinon la flèche trahit un joueur caché.
   ⚠ Deux voisins à ne PAS confondre (mesurés nuls au moment de l'observation) : marqueur
   **homoncule/mercenaire** (child `cursors.spr` **action 10** en `acteur+0x3A8`, msg **134**/**135**
   de l'acteur, décidé dans `ActorAiClass_UpdatePerFrame` 0x00D3194D) et marqueur de **quête**
   (`emotion.spr` actions 80-85, map `CGameMode+0x1F0` remplie par ZC_QUEST_NOTIFY_EFFECT 0x00CD23C0).
   Détail complet : `docs/target_system_re.md` §4 bis.

## Corrections apportées (3 affirmations fausses tombées)
`docs/entity_nameplate_re.md` disait « bandeau nom+HP+emblème de la CIBLE, widgets +0xac/+0xab » :
faux sur les 3 points, offsets inclus (index de dword confondu avec un offset). Corrigé dans la doc
ET dans [[reference_entity_nameplate_re]]. Leçon : **ne pas faire confiance à un nom de fonction
hérité** — c'est ce mislabel qui avait essaimé jusque dans la mémoire du projet.

## Reste à RE (confort, pas architecture)
Table de statuts par GID (debuffs de la cible). Le ciblage clavier type Tab est **inexistant
nativement** (absent de `UIWindowMgr_DispatchHotkeyBehavior`) : écrit entièrement côté Bourgeon
(`CycleTarget`, deux actions sans touche par défaut, Tab assignable depuis le 2026-08-20).

✅ `dword_15E6E88` **est élucidé** : c'est la cible de **suivi** (`GameMode_FollowTarget_Tick`
0x00C75900) — elle FAIT MARCHER le personnage vers l'entité toutes les secondes, et s'annule au
moindre clic ou après 10 s d'absence. Elle court-circuite bien le verrou `+0x28` du marqueur, mais
s'en servir pour afficher une flèche ferait courir le joueur : **inutilisable pour ça**.

🔴🔴 « Le clic cible sans attaquer » (livré) : **DEUX coupes, parce qu'il y a DEUX sorties.**
Le clic arme une ACTION EN ATTENTE sur l'acteur du joueur (`GameMode_PostActorClickAction`
**0x00C753A0** -> msg 10 -> `+0x500` = 1 ou 5, `+0x514` = GID). C'est ensuite
`Actor_ProcessPendingAction_Tick` qui, selon la distance, émet soit une DEMANDE DE MARCHE
(`SendMsg(0x8a)` -> paquet **0x035F**), soit une DEMANDE DE COUP (`SendMsg(0x89, action, GID)` ->
CZ_REQUEST_ACT **0x0437** en 0x00C8F807).
  · coupe n°1 = hook sur `PostActorClickAction` (adresse dans `configuration.h`) -> rien n'est armé,
    donc ni approche ni coup. Gardes : `CGameMode+0x408 == 0` (sinon on tue le lancement de
    compétence au clic) et `acteur+0x2EC != g_Account_Aid` (`+0x2EC` = AID du MAÎTRE, test du natif
    en 0x00C787CC : mes pet/homoncule/mercenaire gardent leurs ordres).
  · coupe n°2 = msg **0x89** avec action 0 ou 7 dans le hook SendMsg (2 et 3 = s'asseoir/se relever,
    à ne JAMAIS toucher) -> couvre le réglage allumé en plein combat.
Le ciblage survit dans les deux cas : `+0xF4` est écrit par le pipeline souris AVANT tout ça.
⚠ **« Attaquer » du menu contextuel passe par le MÊME `PostActorClickAction`** et tombait donc sous la
coupe n°1 (« ne fait plus rien »). Dispense `TargetFrame::NoteExplicitAttack(gid)` posée juste avant :
UN passage ouvert dans le chemin du clic + dispense durable du COUP pour ce GID (sinon seule la
1ʳᵉ frappe part) ; le prochain clic la referme.

🔴 **L'interrupteur est un vrai MAÎTRE** (« Activer le mode Ciblage + HUD ») : éteint, il coupe HUD,
flèche, cyclage clavier ET clic-sans-attaque, et `DrawSettings` grise tout le panneau
(`ImGui::BeginDisabled`). Un réglage cliquable et sans effet est le pire des deux mondes.

🔴🔴 **UN GESTE désigne la cible, PAS un sondage de `+0xF4`.** Le HUD suivait la sélection frame par
frame : une cible sortie de l'écran disparaissait puis **revenait toute seule** en rentrant dans
AREA_SIZE, la sélection native n'étant purgée qu'au changement de map. Trois gestes, le dernier
gagne : CLIC (`+0xF4` change, OU **front montant de `CGameMode+0x28`** — seul signal quand on
re-clique la MÊME entité), CYCLAGE clavier, SORT qui part. ⇒ une cible perdue est PERDUE ; plus de
liste noire (`dropped_gid_` supprimé), plus de `forced_gid_`.

⚠ **Cible qui SE CACHE = GID perdu** : Hiding `0x02`, Cloaking `0x04`, `@hide` staff `0x40`, lus par
la virtuelle **`vtable+0x34`** de l'acteur (`Option_IsHide` = `& 2`, `Option_IsCloak` = `& 4`).
Garder le GID d'une entité invisible, c'est offrir un détecteur.

🔴 **Le gate PvP ne porte PLUS sur les PV** (changé le 2026-08-20) : les PV partent TOUJOURS, le SP et
le NIVEAU restent réservés à party/guilde. Raison : les chiffres de PV sont désormais masqués pour
tout joueur, donc seule une JAUGE s'affiche. Sans cet envoi, la jauge d'un adversaire restait vide
**jusqu'au premier coup porté** — le client ne sait RIEN des PV d'un tiers, il ne les déduit que des
dégâts vus passer, et un Cloaking / `@hide` recrée l'acteur et remet ce compteur à zéro. ⚠ Le
symptôme se lisait « bug du décloak » : ce n'en était pas un.

⚠ **DOUBLE-CLIC = attaquer** quand « le clic cible sans attaquer » est coché. Deux engagements sur le
même GID dans `GetDoubleClickTime()` (borné 200-900 ms) ; fiable parce que le natif n'appelle
`PostActorClickAction` qu'à chaque appui FRAIS (`g_Mouse_LButtonState == 1`). Dispense DURABLE pour ce
GID (une attaque est une suite de demandes) ; le simple clic suivant la referme.

🔴🔴 **`CNameDict_GetEntryOrRequest` 0x005A1460 N'EST PAS UN ACCESSEUR** : sur un défaut il EMPILE le
gid dans la file des noms à demander et rend une entrée VIDE ET STATIQUE. C'est le tick
`CNameDict_Tick_FlushNameRequests` **0x005A1920** (chaque frame, depuis GameMode_InGame_ProcessFrame
0x00C74B06) qui dépile **UNE** demande par frame -> **CZ_REQNAME 0x0368**, 6 o, avec une fenêtre
anti-répétition de **10 s** par gid.
⇒ Le HUD l'appelait **4×/frame** (nom, party, guilde, rang) : 4 demandes empilées par frame pour le
même gid, ~240/s dans une file vidée à 1/frame. **Plus on réclamait le nom, plus il tardait.**
UNE interrogation par frame, quatre lectures dans le pointeur rendu (qui est déjà `entry + 20`).

⚠ **Ce qu'on ne montre PAS d'un joueur** : les chiffres de PV (jauge seule, ni valeurs ni
pourcentage — un avantage que le jeu ne donne pas), la party et le rang de guilde (seul le nom de
guilde reste), et **le cadre race/élément/taille en entier** (« Neutral 1 · Medium » pour tout le
monde). « inconnus » subsiste quand les PV ne sont pas connus : ce n'est pas une fuite, et
sans lui une jauge vide se lirait « il est mort ».

⚠ **Barre de SP d'un monstre = « No SP »**, pas « 1 / 1 » : `status_calc_misc` finit par
`if (!status->max_sp) status->max_sp = 1;` (un max nul ferait des divisions par zéro). Critère
`max <= 1`, et la barre ne se remplit pas. Aucun joueur n'y tombe.

🔴 **C'est le CLIENT qui DEMANDE l'approche, le SERVEUR qui l'exécute.** J'ai affirmé
successivement les deux extrêmes, tous deux faux. L'argument qui tranche est de l'utilisateur :
« si ça venait du client, une déconnexion me ramènerait en arrière — or le serveur sait que je vais
au mob ». Le client ne bouge jamais seul ; il envoie une vraie demande de marche.

🔴🔴 **Trois pièges payés le 2026-08-20, deux de la même forme : une condition qui ne filtre RIEN
ressemble à une fonctionnalité non branchée.**
1. Filtrer trop BAS : le paquet est la dernière étape d'une machine à états ; couper là laisse
   l'approche.
2. `SendMsg(0x89)` prend **p1 = action, p2 = GID** — dans cet ordre. Lu à l'envers depuis le site de
   construction (le dispatcher charge p1→edx, p2→ecx) on conclut l'inverse. **Trancher chez
   l'ÉMETTEUR** : `Actor_OnMsg` 0x00D4765B / 0x00D47678 / 0x00D476B2 / 0x00D476D8.
3. `acteur+0x314 != 0` NE VEUT PAS DIRE « unité spéciale » : `CActorSprite_InitDefaults`
   (**0x00C45F47**) y écrit **4 PAR DÉFAUT**. Un filtre « ordinaire = 0 » rejette tout le monde.

Liens : [[project_quick_cast]] [[reference_entity_nameplate_re]] [[reference_cmode_sendmsg_use_skill]]
[[feedback_debug_tooling]] (piège du watchpoint sur le tas) [[reference_dev_mode_gating]].
