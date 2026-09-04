# QuickCast — lancer un sort en une action

> Journal du chantier. La fiche de mémoire `project_quick_cast` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

Plugin **QuickCast** (`features/gameplay/quick_cast.{h,cc}`) : lancer un sort en UNE action
(demande utilisateur 2026-07-31). Opt-in **staff** ([[reference_dev_mode_gating]]), 2 cases dans
« Staff Tools » : sorts de **zone** (cast direct sous la souris) et sorts **ciblés** (cast direct si
GID compatible survolé). Persisté `quickcast_ground` / `quickcast_target` (défaut false).
**ÉTAT : code complet, relu, PAS buildé ni testé** ([[feedback_dont_relaunch_game]]). Non commité.

## RE du mode ciblage (client 20250716, IDB annoté)
- `CMode::SendMsg` **case 0x48** (bloc 0xC8D8DF) = ENTRER en ciblage : pose dans CGameMode
  **+0x408 = mode** (p2 : 1 sol, 2 cible, 3, 4 soutien, 5 piège, 0 = annuler), **+0x40C = id**
  (info+8), +0x410 = info+0x1C, **+0x414 = niveau**, curseur 0x15E818C. Bails : fenêtre 0xC4,
  global 0x131F7EC, info nul. **case 0x47** (0xC89B89) = SORTIR (raz +0x408/40C/414 + curseur) —
  envoyé par le pipeline souris à CHAQUE clic gauche frais.
- Pipeline souris par frame `GameMode_ProcessMouseWorldInput` **0x00C76400** (ex-sub) :
  Mouse_UpdateFrameState → `UIWindowMgr_DispatchMouseInput` **0x00A46380** (dispatch UI natif,
  ⚠ GROS effets de bord, ne jamais appeler ; lit AUSSI l'état de frame donc voit un clic
  synthétique) → `TileQuadTree_QueryPoint` 0x00A797B0 sur g_NameplatePickQuadTree 0x012135F0
  (quad : **[6]=AID [7]=job [8]=catégorie**, 0=acteur/1=NPC/2=skill-unit/3-4=spéciaux) →
  `GameMode_RouteHoverAndClick` 0x00C756A0 + `GameMode_GroundClick_RequestMove` 0x00C75AA0 →
  SendMsg(0x47) si LButton frais.
- **Cast sol** (mode 1, dans GroundClick) : acteur joueur `OnMsg(0x41, skillId, x, y)` puis
  `OnMsg(0x5A, niveau)` ; cellule = `GameMode_PickGroundCellUnderMouse` **0x00C69A40**
  `bool __thiscall(gm, int* x, int* y)` (raycast, false si rien). **Cast ciblé** (modes 2/4,
  dans CursorMgr_UpdateHover 0x00C78180 / GameMode_HandleActorClick 0x00C754D0) :
  `GameMode_PostActorClickAction` 0x00C753A0 → `OnMsg(0x29, GID, skillId)` + `OnMsg(0x5A, niveau)`.
  Gates natifs : soi-même interdit sauf mode 4, Maj/PVP/GVG (sub_D8EB40/D8EA30), homon/merc
  cooldown partagé routés à part (Actor vtbl+204/212).
- **g_Mouse** 0x011E40D0 : octets BRUTS boutons à +44 (`g_Mouse_RawButtonBytes`) : +44/+45 = L/R
  précédents, **+46 (0x011E40FE) = L enfoncé courant** (WndProc `Mouse_OnWindowMessage` 0x005FD8C0 :
  WM_LBUTTONDOWN=1/UP=0), +47 = R. `Mouse_UpdateFrameState` 0x005FD760 dérive l'état de frame
  **0x011E40E4** (1/4 = appui frais, 2 = tenu, 3 = relâché) puis copie courant→précédent.
  ⇒ **poser +46 à la main = clic gauche synthétique 100 % natif** (position = vraie souris).
- `UIWindowMgr_GetWindowAtPoint` **0x00A336D0** `UIWindow* __thiscall(g_UIWindowMgr, x, y)` =
  hit-test PUR (liste +0x17C, vtbl+0xC8). ⚠ Décompilé trompeur : IDA affiche `dword_131F4E8` en
  arg mais le désassemblage fait `mov ecx, 0x131F4E8` — l'ADRESSE est bien l'objet (uiwnd.h OK).

## Mécanique du plugin — v2 (2026-08-01), la v1 « clic synthétique » est ABANDONNÉE
🔴 **TROIS approches, deux écartées** (question de l'utilisateur : « pourquoi ne pas envoyer
directement le paquet ? ») :
1. ⛔ **Simuler le clic** (poser `*(uint8_t*)0x11E40FE = 1`, l'octet brut de g_Mouse) = la v1.
   ABANDONNÉE : dépend de l'état souris, course de timing (l'état de frame 0x11E40E4 met 3 frames
   à retomber 1→3→0), risque de cliquer une fenêtre native, et **ne répète PAS au sol**.
2. ⛔ **Fabriquer CZ_USE_SKILL / CZ_USE_SKILL_TOGROUND** : on saute barre de cast, animation,
   cooldowns clients et le rangement de `menuskill_val` ; + une table de paquets à maintenir.
   (Règle déjà posée dans [[reference_cmode_sendmsg_use_skill]].)
3. ✅ **Émettre le MESSAGE que le clic aurait produit** — même pattern que [[project_keyboard_move]]
   (`Actor_OnMsg(0x11)` au lieu d'un clic-sol simulé).

Acteur joueur = `*(*(CGameMode+0xCC) + 0x2C)`, message via **vtable+8** (`Actor_OnMsg`).
🔑 **13 dwords TOUJOURS** : `(0, msg, 0, p1lo, p1hi, p2lo, p2hi, p3lo, p3hi, 0,0,0,0)` = mot de tête
+ msg 64 bits + **5 params 64 bits**. Restaurer ESP soi-même (`__asm`, pas de SEH dedans).
- **sol** : `0x41(skillId, x, y)` puis `0x5A(niveau)` — cellule par `PickGroundCell` 0x00C69A40 ;
- **cible** : `0x29(GID, skillId)` puis `0x5A(niveau)` — ⚠ le GID part avec **mot haut = 0**, PAS
  d'extension de signe (le natif fait pareil ; un AID bit-31 armé deviendrait négatif) ;
- puis **`SendMsg(0x47)`** pour désarmer (⚠ **RE-RENTRE** dans notre hook — sans danger, 0x47 ne
  redéclenche pas de 0x48).
Gates : toggles + IsStaff + timestamp + IsGameActive + !IsMapLoading + !WantCaptureMouse +
`UIWindowMgr_GetWindowAtPoint` nul + **vtable du cmode == 0x010904B8** (SendMsg sert aussi aux modes
login/char-select, où +0x408 ne veut rien dire). Pré-checks : mode 1 → PickGroundCell vrai ;
modes 2/4 → quad cat 0, job ≠ 45, AID ≠ 0 ; mode 2 → pas soi (le filtre monstre est TOMBÉ le 2026-08-30). Modes 3/5 natifs.
INF 4 (self) envoie un `0x48` **mode 0** ⇒ TryCast rend false, pas de double cast. ✅
🔑 **CES MESSAGES N'ENVOIENT RIEN : ils REMPLISSENT UNE FILE** dans l'acteur (`+0x500` nature 3=cible
/4=sol, `+0x514` GID, `+0x524` id, `+0x528` niveau — ce dernier posé par le **0x5A**, case 90 de
`Actor_OnMsg`, écriture vérifiée à **0x00D47B67**). `Actor_ProcessPendingAction_Tick` **0x00D43400**
la consomme à la frame suivante et émet le paquet ([[project_skill_input_latency]]). ⇒ l'ordre
« lancement puis niveau » est CORRECT (les deux remplissent avant lecture), et **la file n'a QU'UN
slot** donc répéter n'inonde pas le réseau (on écrase, on n'empile pas). Les gardes natives du
message survivent (le 0x41 refuse le motion state 6 = FREEZE).

## 🔴 Le client IGNORE l'auto-répétition clavier (cause du « les sorts de zone ne se répètent pas »)
`Game_MainWndProc` **0x00DB8100**, WM_KEYDOWN (256) et WM_SYSKEYDOWN (0x104), appelle
`UIWindowMgr_OnKeyDown` **0x00A471E0** avec un 4e argument = **`(lParam & 0x40000000) == 0`** — le
bit 30 de lParam est l'état PRÉCÉDENT de la touche, donc l'argument n'est vrai que sur un appui
**FRAIS**. Or TOUT le bloc hotkey de `UIWindowMgr_OnKeyDown` (`UIWindowMgr_DispatchHotkeyBehavior`)
est sous `if (cet argument)`. ⇒ **maintenir une touche n'émet AUCUN 0x71/0x48 supplémentaire**.
Réagir au 0x48 ne peut donc donner QU'UN lancement par pression — la répétition doit être À NOUS.
🔑 `UIWindowMgr::ProcessPushButton` du projet (`configuration.h`) EST cette fonction 0x00a471e0, et
son 3e paramètre **`accurate_key` EST ce drapeau d'appui frais** ⇒ `Plugin::OnKeyDown` donne la VK
et le caractère frais, juste AVANT le dispatch natif. C'est ainsi que QuickCast associe une touche
au 0x48 qui suit, puis rejoue par frame via `Update()` (OnRenderUI **et** `Bourgeon::OnProcessInput`,
pour survivre à F11) tant que `GetAsyncKeyState` confirme l'appui. Visée ré-évaluée à chaque tick.
⚠ Ce que le natif répète (`GameMode_RepeatActorAction` **0x00C77120**) dépend de la **SOURIS tenue**
(`g_Mouse_LButtonState`) + cible mémorisée `CGameMode+0xF0` ; aucun équivalent au sol ⇒ l'asymétrie
observée en v1 venait du clic synthétique resté enfoncé, PAS d'un mécanisme clavier.

**Cadence — DEUX niveaux** (question utilisateur : « pourquoi pas le cooldown du sort ? ») :
1. ✅ **cooldown RÉEL d'abord** — `ro::SkillCooldownRemainingMs(skillId)` ([[project_shortcut_bar_re]]
   / `ragnarok/skill_cooldowns.h`, alimentée par ZC 0x043D) : si > 0, on n'émet rien. L'id convient,
   `+0x40C` étant celui qui finit dans CZ_USE_SKILL via `+0x524` (= id SERVEUR, comme 0x043D).
1bis. ✅ **état d'action de l'acteur `+0x70`** — 3e donnée, RÉELLE : `Actor_ProcessPendingAction_Tick`
   n'exécute la requête en file (cas 3 skill/cible, 4 skill/sol) que si **`état < 2 || état == 4 ||
   état >= 16`** (sinon elle patiente ; cas 1/6 = variante + garde 1200 ms sur `+0x4F8`, drapeau
   « déjà tiré » `+0x50C`). QuickCast réplique ce prédicat, **uniquement sur la répétition** (jamais
   sur un appui volontaire : une mauvaise lecture ne peut pas casser le cas nominal). ⚠ Ne connaît
   que l'ANIMATION, pas l'after-cast delay serveur.
2. **période `repeat_ms_`** (défaut 200, persisté `quickcast_repeat_ms`) — c'est la VRAIE période de
   répétition de notre boucle (pas un plancher anti-flood : le flood était impossible, cf. ci-dessus).
   🔴 IRRÉDUCTIBLE tant que le serveur ne transmet pas l'after-cast delay (piste : opcode custom
   moonlight, [[project_opcode_system]]) :
   ZC 0x043D n'existe que pour les compétences **qui ont un cooldown**. La plupart des sorts n'en ont
   pas : ils sont bornés par l'**after-cast delay** (`canact_tick`), que le serveur ne transmet
   JAMAIS au client. Et ce délai est précisément ce qui, côté moonlight, met les skills en file pour
   rejeu ([[project_skill_input_latency]]) — spammer y déclencherait des rejeux en cascade.
⚠ **CE QU'ON PERD** : les validations du chemin du CLIC (soi-même en offensif, Maj/PVP/GVG, HP bas)
vivent dans `CursorMgr_UpdateHover`/`GameMode_HandleActorClick`, PAS dans `Actor_OnMsg`. Les
pré-checks du plugin ne sont donc plus du confort : **ils sont le contrat**.

## OBJETS : même répétition sur les cases de la barre d'action (2026-08-07, demande utilisateur)
« Rester appuyé pour cycler et ouvrir toutes les Old Blue Box / Dead Branch. » 3e case
`quickcast_item` + cadence PROPRE `quickcast_item_repeat_ms` (défaut **400 ms**). ⛔ NON BUILDÉ.
- 🔑 **Point de détection = le hook `UIShortCutWnd::OnMsg` case 0x29 de skill_bar.cc**, PAS OnKeyDown :
  lui seul voit TOUTES les voies (touche de l'onglet affiché via le natif, routage maison de l'autre
  onglet, barre d'objets, clic ImGui). Notifie APRÈS l'appel original si `ReadSlot(region,slot).type==0`
  (= OBJET) → `QuickCast::OnUseItemSlot(region, slot, nameid)`.
  ⚠ Nouveaux globals `g_active_region/g_active_slot` posés par `ActivateSlot` : **indispensables**, car
  `UseItemSlot` détourne `this+0xc4[0]` ⇒ les col/row du 0x29 MENTENT pour la barre d'objets.
- 🔑 **Rejeu = `SkillBar::RepeatItemSlot(region,slot,nameid)`** → `ActivateSlot` = la voie EXACTE de la
  touche. Renvoie **false = arrêt** : case changée/vidée, `GetItemLiveCount()==0`, ou case plus dessinée
  (`g_slot_drawn`, cf. FIX 3 de [[project_skillbar_multibar_wip]] — une activation À NOUS étant exemptée
  du filtre du hook, c'est RepeatItemSlot qui doit re-tester).
- 🔴 **Boucle dans un NOUVEAU point d'appel `Bourgeon::OnGameFrame()`** (bourgeon.cc, appelé par
  `GameMode::OnUpdateHook` juste avant OnTick) = **par FRAME et hors frame ImGui**. Les 3 horloges du
  projet et pourquoi les 2 autres ne vont pas : `Update()`/OnRenderUI = DANS la frame ImGui, or le rejeu
  est une commande native ([[feedback_imgui_pitfalls]]) ; `OnTick` = hors frame mais
  **bridé ~100 ms**, ce qui plafonnerait la cadence 5× trop haut sans le dire ; `OnProcessInput` = sur ce
  client c'est CMode::SendMsg, donc ÉVÉNEMENTIEL. Dispatch NOMINATIF (pas de boucle plugins) : chemin chaud.
- 🔴🔴 **Cadence plancher = le SERVEUR, et il est à 20 ms pour le STAFF** (info utilisateur, VÉRIFIÉE dans
  `moonlight/src/map/pc.cpp` ~6998, patch maison `// opti spam de branch en gm`) :
  `if (pc_get_group_level(sd) >= 40) sd->canuseitem_tick = tick + 20;` — écrase le
  `battle_config.item_use_interval` (325 ms dans `conf/import/battle_conf.txt`, +500 en PvP sur les soins).
  Comme l'option est gatée `IsStaff()` = groupe **>= 80** ([[reference_dev_mode_gating]]), ses utilisateurs
  ont TOUJOURS le plancher à 20 ms. ⇒ slider **20–1000, défaut 50** ; la vraie limite devient la FRAME
  (~16 ms à 60 fps) et la résolution de GetTickCount (~15,6 ms). (`PC_PERM_ITEM_UNCONDITIONAL` bypasse tout,
  mais le groupe ne l'a pas.) ⚠ NE PAS re-documenter « 325 ms » comme le plancher : c'est faux ici.
- Gardes propres aux objets : touche relâchée, `GameHasFocus()` (**nouveau, appliqué AUSSI aux sorts via
  CanCastNow** : `GetAsyncKeyState` lit l'état PHYSIQUE ⇒ alt-tab touche enfoncée = répétition invisible),
  `io.WantTextInput`, IsGameActive/IsMapLoading, OnModeSwitch.
- `pending_vk_` a désormais un HORODATAGE (`TakePendingKey`, 250 ms) : sans lui, une touche tenue pour
  autre chose (direction) se faisait associer à un déclenchement ULTÉRIEUR à la souris.
- ⚠ Dépendance à l'ORDRE d'enregistrement : QuickCast est créé AVANT SkillBar dans `LoadPlugins`, donc son
  OnKeyDown a noté la frappe quand SkillBar::OnKeyDown active l'autre onglet. Réordonner = casser ça.
- Marche même si le module « barre d'action ImGui » est ÉTEINT (le hook OnMsg est posé au ctor de SkillBar,
  quelle que soit l'activation ; ActivateSlot marche sur la native visible).

## Cible JOUEUR autorisée en offensif (2026-08-30, demande utilisateur)

Le filtre « monstre uniquement » du mode 2 est RETIRÉ des DEUX sources de visée
qui le portaient — `QuickCast::PickTargetGid` (souris) et `ValidSkillTarget`
(`target_frame.cc`, qui sert le HUD de cible). Restent : jamais soi-même, jamais
un cadavre (motion state 3), jamais le portail (job 45).
🔴 Conséquence assumée : plus AUCUNE validation PVP/GVG/Maj côté client sur ce
chemin — le SERVEUR est seul juge, un offensif sur un joueur hors PVP part et
c'est lui qui le refuse. (Avant, la fiche disait « les pré-checks SONT le
contrat » : ce n'est plus vrai pour le job de la cible.)
⚠ `PartyFrames::SkillTargetGid` refuse TOUJOURS le mode 2, et c'est délibéré
pour une AUTRE raison : une tuile de raid frame est survolée en permanence
pendant qu'on soigne, un offensif y partirait sur un coéquipier.
Textes d'aide FR/EN/ES mis à jour (`check_catalog.ps1` OK). ⛔ NON BUILDÉ.

## Ciblage natif — voir la fiche dédiée
La RE du ciblage (sélection `CGameMode+0xF4`, cible transitoire `+0xF0`, absence de bandeau/HP,
répétition des skills ciblés) a été poussée à fond le 2026-08-01 et vit désormais dans
**[[project_target_system_re]]** + `docs/target_system_re.md`. Rappel du seul point qui touche
QuickCast : `+0x408` (mode) et `+0x40C` (id) sont posés par le case `0x48`, et `GameMode_OnEnterMapSetup`
(0xC6BE20/0xC6BEA6) les remet à 0 au changement de map.

Liens : [[reference_cmode_sendmsg_use_skill]] [[reference_processinput_sendmsg_hook]]
[[reference_entity_nameplate_re]] [[project_keyboard_move]] [[reference_dev_mode_gating]].

## Ouvert aux JOUEURS (2026-08-18)

Plus de garde `IsStaff()` dans CanCastNow / OnUseItemSlot / UpdateItemRepeat :
le plugin vit dans la section « Gameplay » du panneau joueur (staff_tools le
garde aussi). 🔴 UNE différence de droit : le plancher de la cadence OBJET —
20 ms staff, **100 ms joueur** — appliqué dans `UpdateItemRepeat` sur la période
EFFECTIVE (`max(item_repeat_ms_, floor)`), pour qu'un yaml édité à la main ne le
contourne pas ; le slider suit (borne basse et aide selon `IsStaff()`). Le
serveur borne de toute façon à 325 ms pour un joueur normal (item_use_interval).
