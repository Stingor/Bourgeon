# Archive des chantiers soldés (18 condensés)

> Journal du chantier. La fiche de mémoire `project_solved_archive` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

Dix-huit chantiers terminés, condensés le 2026-08-16. Chaque correctif vit dans
le code et dans git : ce qui est gardé ici, c'est **la cause racine** (qu'un
`git log` ne donne pas) et **l'adresse ou le fait** qui resservira. Le journal de
débogage a été jeté.

🔴 **État vérifié dans le dépôt le 2026-08-16**, pas d'après les marqueurs ✅ des
anciennes fiches — deux d'entre eux mentaient (voir §17 et §18).

---

## ⚠ Ce qui reste ACTIF malgré le ✅ — à lire, le reste est de l'histoire

1. **`Bourgeon::IsMapLoading()` est une barrière à RÉUTILISER** : tout nouveau
   module qui ne doit pas agir pendant un chargement de carte doit la consulter
   (§14).
2. **Les 5 caches d'icônes locaux qui subsistent le sont pour une raison** — ne
   pas les « finir » sans relire §16.
3. **Toute copie FRAÎCHE de `Moonlight-Destiny.exe` refait le crash de coiffe**
   tant qu'elle ne passe pas par le `.qjs` corrigé ou le patch binaire (§5).
4. **`SnapHiddenFilter` est posé dans `WindowPosTweaks`** et filtre le snap sur
   les fenêtres cachées : le retirer ramène le « ghost snap » (§1).
5. **Le correctif garment vit dans `transparentItem.lub`, pas dans `src/`** — il
   doit être livré dans le patch THOR / `moonlight.grf`, sinon les joueurs
   gardent le bug (§4).
6. Trois correctifs sont **côté serveur moonlight** et disparaîtraient d'un
   `git checkout` de rAthena amont : dual-wield pre-renewal (§9), MATK 16 bits
   (§8), et les bornes de GID de Wind Blade (§7).

---

## Crashs

### 1. « Ghost snap » — une fenêtre cachée reste un aimant
Traîner n'importe quelle fenêtre près du coin haut-gauche la faisait magnétiser
sur un morceau de HUD invisible (Y forcé à 134). **Le calculateur de snap
`FUN_00a32eb0` ne consulte JAMAIS la visibilité du candidat** : il itère l'arbre
`mgr+0x194` avec un rect **CACHÉ** (`node+0x14`) qui reste à l'écran même après
que la fenêtre vivante a été déplacée hors champ. Le fantôme était `basic_info`
(hauteur 134) épinglé à −10000. Vérité de visibilité : `UIWnd_SetVisible`
0x009030c0 → `window+0x28`.
**Correctif** (commit b2bcf90, `WindowPosTweaks`) : `SnapHiddenFilter`, trampoline
naked JMP-hooké à **0x00a33005** (juste avant `FUN_0097ac60`, candidat = ESI) qui
saute vers l'avance d'itérateur **0x00a3304b** si le candidat est caché (+0x28 ==
0) ou épinglé hors écran (+0x1c/+0x20 ≤ −2000).
⚠ Le premier essai — NOPer les deux `ADD` à 0x008818c6/cc — tuait TOUT le snap et
cassait une boucle native de clamp à l'alt-tab. Annulé.

### 2. Char-select : off-by-one natif à la 3e page
Crash `0xC0000005` en paginant, **préexistant à Bourgeon**. La position
sélectionnée `[esi+0x120]` vaut **15** = la sentinelle « aucune sélection », mais
elle est lue sans garde `index < [esi+0x128]` (slots/page = 15, grille 5×3). Donc
`slots[15]` lit un cran après la fin du tableau `[esi+0xe8]` → `0xABABABAB` (garde
du tas) → non-null → déréférencement. Intermittent parce que ce qui suit l'alloc
est parfois 0.
**Correctif livré** : `SelCharPagingFixStub` (`ragnarok_client.cc`), détour à
0x0079d5e0 — `cmp ecx,[esi+0x128]; jae` vers le chemin natif « pas de sélection ».
🔑 **`SLOTS_PER_PAGE` a une SOURCE UNIQUE** : `0x0079a103 MOV [EDI+0x128],0xF`.
Tout s'en dérive. Mais la **grille 5 colonnes est CODÉE EN DUR** (`x=(i%5)*0x9d+10`)
dans BuildPage/RenderSlots/OnMsg — la feature « 25 par page » a été testée puis
abandonnée (mur physique : carte de slot = bitmap fixe ~197 px que `SetSize`
COUPE au lieu de scaler). Archivée dans
`docs/archive/charselect_25_per_page_native_patch.md`. Sans objet depuis le
char-select ImGui.

### 3. Coiffe : off-by-one dans un patch WARP de l'exe
Crash à 0xb41f20. **L'exe LIVE est patché, l'IDB ne l'est pas** : dans
`Hair_BuildHeadgearSpritePath_impl`, les deux chargements `table[idx]` sautent
vers des stubs en code-cave (0x171dd30 / 0x171dd50) dont le clamp est
`cmp ecx,0x2B ; jle load` — **43 passe**, alors que les deux vecteurs font 43
entrées (0..42). idx=43 lit `_Mylast` (poubelle du tas) → `strlen` → AV.
Cause première : `char.hair = 43`, un style de CHEVEUX hors bornes — **pas** un
job, comme le nom de la fonction le laissait croire.
**Correctif** : offsets fichier 0xf55d38 / 0xf55d58, `7E` → `7C`.
🔴 Récidive du 2026-07-31 sur une copie fraîche de l'exe : le correctif n'existe
que dans l'exe déployé et dans `IncrHairs.qjs`.

### 4. Warp/@load : UAF du gestionnaire de snap
Deux crashs `0xC0000005` au warp. **Cause commune** : le chargement tourne
IMBRIQUÉ dans `CGameMode::EnterWorld` 0x00c733d0 — le `OnUpdate` natif est
suspendu (garde `param_1[5]`), mais notre hook EndScene/OnRenderUI, lui, continue.
`SkillBarTweaks::EnsureCreated()` appelait donc `MakeWindow(0x24)` **chaque
frame**, créant des `UIShortCutWnd` en double, libérées en restant inscrites dans
le gestionnaire de snap natif.
**Correctif** (commit c00aad2) : barrière `Bourgeon::IsMapLoading()`, armée sur
recv **ZC_NPCACK_MAPMOVE 0x0091/0x0092**, levée sur send **CZ_NOTIFY_ACTORINIT
0x007d** (les propres signaux du client), plafond de sécurité 20 s. Pendant :
`RenderUI()` saute les plugins et `ProcessPushButtonHook` avale le clavier.
🔑 **RE conservée — gestionnaire de snap/dock natif** : `g_WinSnapMgr`
**0x011FE424** (gate 0x11FE420, ancre active 0x11FE428, liste 0x11FE42C, compte
0x11FE430) ; wrapper par fenêtre, `wrapper+4` = UIWindow. `Register` 0x7a84a0,
`Unregister` 0x7a94a0 (**seul appelant** = `FUN_00a48e70`, donc toute autre
destruction oublie de désenregistrer), `RebuildNeighbors` 0x7a92f0,
`ClassifyEdgeAdjacency` 0x7a85a0 (lit le rect +0x14/18/1c/20, **aucune garde de
pointeur**), `ClearAll` 0x7a9430.

## Performance

### 5. Chat : freeze en autoattaque, word-wrap quadratique
`UIRenderCtx_MeasureText` **0x005474a0** était appelée **8640 fois par rebuild**
(120 lignes × ~72 préfixes) = 94 % du temps : le word-wrap cherche le point de
coupure en mesurant des préfixes croissants, chaque essai étant un
`GetTextExtentPoint32W`, donc un syscall GDI. Coût quadratique, repayé
intégralement à chaque `TrimHistoryHalf` (0x00844f50) → `RebuildFromHistory`
(0x008642d0), **et par fenêtre de chat détachée**.
**Correctif** : mémoïsation dans `MeasureTextHook`, clé FNV-1a 64 bits sur
(texte, len, ctx+0xc, ctx+0x10, id police, ctx+0x1c). 8640 → **57** appels GDI,
236 ms → **1,7 ms**. `--nomeasurecache` restaure l'ancien comportement.
⚠ **Variabilité GDI : piste ÉPUISÉE, ne pas refaire ces tests.** Le même
`GetTextExtentPoint32W` coûte ~32 µs sur certaines sessions et ~1,0 µs sur
d'autres, ~50/50 au lancement, figé pour la session. DC et bitmap **identiques**
au bit près entre les deux. Tous les suspects ont été écartés par mesure directe
(débogueur, charge CPU, mémoire, alt-tab, LLM local, polices, italique/gras).
Reconnaître une session lente : le `us/appel` de `gdimeasure` dans `[ChatProf]`.

### 6. `@storeall` : un paquet revendiqué par FRAME
`@storeall` prenait 1-2 s avec « Interface moderne », instantané sans. **Régression**
introduite le 2026-08-01 en mettant `0x00f2` (`ZC_NOTIFY_STOREITEM_COUNTINFO`,
émis **une fois par objet rangé**) sur le chemin revendiqué.
**Cause** : `RagConnection::RecvPacketHandler` exécutait l'épilogue de
`RecvLoop_DispatchPackets` (0xc9df00) puis `ret` — il ne terminait pas le
*paquet*, il terminait **toute la fonction de réception**. Or celle-ci n'est
appelée **qu'une fois par frame** depuis `GameMode_InGame_ProcessFrame`
(0xc74a80). D'où ~7 ms par paquet.
**Correctif en place** (`rag_connection.cc`) : rebouclage sur la tête de boucle du
dispatch (`RecvDispatchLoopHead` / `g_recv_loop_head`) — **un `case` natif ne rend
jamais la main, il reboucle**. Si la tête est absente pour un client donné, le
stub retombe sur un paquet revendiqué par frame et le journalise.
🔴 **Le commentaire d'IDA annonçait un tail-call, et c'était l'inverse** — il
décrivait le bug de *notre* hook, pas le client.
⚠ Fausses pistes déjà écartées, ne pas y revenir : fsync InnoDB, picklog
synchrone, `clif_bourgeon_storage_prices`, **le `LogDiag` du hook (accusé à tort :
il MESURAIT les 7 ms, il ne les causait pas)**, `Extract()` par frame, la valeur
de retour EAX, un thread réseau (il n'y en a pas).
**Leçon** : le `tcpdump` a innocenté le serveur en une commande, après trois
hypothèses serveur successives. Mesurer où passe le temps avant de raisonner.

## Rendu

### 7. Capes/costumes garment invisibles par intermittence
Les views ≥ 271 étaient invisibles au hasard, une fois par lancement.
`transparentItem.lub` déclare un alpha **par view id** ; le fichier **s'arrêtait à
la view 270**. Pour toute view au-delà, le `lower_bound` échoue et le code prend
le **nœud SENTINELLE** de la `std::map` (`session+0x7EC0`), dont les champs
+0x14/+0x18/+0x1C ne sont **jamais initialisés** : l'alpha vient de mémoire
résiduelle, tiré au sort à chaque lancement et figé pour la session. D'où
l'intermittence et le fait que rééquiper n'y changeait rien.
**Correctif** : entrées `{ N, 255, 255, 25500 }` pour N = 271…306, **avant** le
terminateur `{0,0,0,0}` (le client arrête la boucle au premier id 0).
🔑 **Le garment n'est PAS un emplacement du composite** (`acteur+0x4ac`/`+0x4b8`
portent corps 0, tête 1, coiffes 2-4, arme 5, traînée 6, bouclier 7, helm-robe 8).
Il est résolu et dessiné à chaque frame par `CActorSprite_DrawGarmentOnTop`
0x00604aa0. `CActorSprite_DrawGarmentLayer` 0x00d36430 est une variante sans
xref — ne pas s'y tromper.

### 8. Wind Blade : lames toujours à droite
Skill **540 NJ_HUUJIN** (⚠ pas Kamaitachi 542). **Cause première = CLIENT** : le
vieux binaire Gravity stocke le GID de la cible en **float32**, donc tout GID
au-delà de 2²⁴ (16,7 M) est arrondi et `ftoui + ActorList_FindByGID` ne retrouve
plus personne. Déclenché par le défaut rAthena `START_NPC_NUM = 110 000 000`.
**Correctif serveur** : `END_ACCOUNT_NUM` 100 M → **3 M** (`common/mmo.hpp`) et
`START_NPC_NUM` 110 M → **3 M** (`map/npc.hpp:245`). Les GID de mob tombent dans
[3 M ; 16,7 M] = exactement représentables en float32. Le reboot hebdomadaire
remet `npc_id` à 3 M (fenêtre ~13,7 M spawns/semaine, pas de `MAX_NPC_NUM`).

### 9. Équipement cassé en rouge
`ItemSkillInfo+0x5d` = octet `is_damaged` (juste après « identifié » +0x5c, avant
refine +0x60). **Rendu natif de référence** (`DrawName` 0x008972c0) : le natif ne
colore pas le texte, il dessine une **OMBRE rouge `0x5050fa`** (COLORREF BGR →
RGB 250,80,80) décalée +1,+1 SOUS un texte resté noir.
**Correctif** (commits 02e07a9, 773f759, 60f75df) : `itemcell::kDamagedShadow`,
`itemcell::NameText(texte, damaged)` et un paramètre `damaged` sur `DrawTile`.
Branché dans inventory_viewer, storage, cart, trade et vending ; `character_sheet`
volontairement exclu (rAthena déséquipe à la casse). L'aperçu au survol rend le
titre avec le suffixe **« - Broken »**.
⚠ La teinte d'icône passe par **`ImageWithBg`** : depuis ImGui ≥ 1.91.9, `Image`
a perdu son `tint_col`.

### 10. Description qui sort DERRIÈRE au clic LONG
Clic bref → la description devant ; **appui prolongé** → derrière. Le bug dépendait
de la DURÉE du clic, pas du site d'appel, ce qui l'a rendu insaisissable longtemps.
**Cause** : la remontée est différée d'une frame (`SetNextWindowFocus` au rendu
suivant), et **le focus reste acquis à la fenêtre cliquée tant que le bouton est
enfoncé** — la description remontait, puis le geste en cours rendait le dessus à
notre fenêtre.
**Correctif** (commit 9062c1c) : `itemcell::DeferDescFromIndex` / `DeferDescById`
+ `FlushDeferredDesc()` appelé par `Bourgeon::OnProcessInput`, qui n'ouvre que si
les DEUX boutons sont relâchés. ⚠ Par index de liste de session, l'`ItemSkillInfo`
est **re-résolu au flush** (le nœud peut mourir entre clic et relâchement).
`NoFocusOnAppearing` sur le panneau est VOULU (aperçu au survol du storage).

### 11. Saut cosmétique à la barre espace
Le jeu **recalcule** la hauteur depuis la heightmap chaque frame, **mais ré-ajoute
juste après un vec3 d'offset propre à l'acteur** — c'est lui qu'on pilote. Dans
`CActorSprite_UpdateMotionAndPosition` 0x00c47700 (label de fin LAB_00c47c9c) :
`actor[+0x3f0]` = offset X, **`actor[+0x3f4]` = offset HAUTEUR (le levier)**,
`actor[+0x3f8]` = offset Z. Initialisé à 0 dans `ActorAiClass_ctor` 0x00c3f900 et
**jamais touché en jeu normal**. ⚠ `+0x3fc` est un objet séparé, pas le vec3.
Livré dans `src/features/gameplay/player_jump.{cc,h}`.

### 12. Overlay alootid sur la description d'item
Livré. Ce qui resservira, c'est la **fenêtre de description** :
`[g_UIWindowMgr + 0x218]` = **0x0131F700** porte le pointeur du tooltip actif —
écrit par le JEU indépendamment de nos hooks (fermeture `exe+62F25B`, ouverture
`exe+63D842`), donc scrutable chaque frame pour auto-fermer un overlay.
Messages du handler : **0x22** = ouverture, **0x18** = poser l'item, **0x06** =
fermeture par le X. Un re-clic sur le même item ferme **sans** 0x06 : il faut
détecter un 0x18 portant le même id. Détail complet dans
[[project_item_skill_desc_window_re]].

## Correctifs serveur

### 13. MATK négatif après login/zone
La Status window montrait un MATK négatif, qui **se corrigeait après tout recalcul
de stats** et **revenait à chaque login ou changement de carte**. Deux paquets
portent les stats dérivées à **deux largeurs différentes** :
- **`0x00bd` ZC_STATUS** (bloc complet, envoyé par `clif_initialstatus`) — le
  handler `FUN_00cd9e30` lit en **`MOVSX word`** (signé 16 bits) : au-delà de
  32767, ça passe négatif. Les champs rAthena sont réellement 16 bits ;
- **`0x00b0` ZC_PAR_CHANGE** (mise à jour d'une stat) — handler `0x00cceff0`, lit
  en **32 bits**. Correct.
**Correctif serveur seul** : `clif_initialstatus` re-envoyait déjà les stats de
base en `clif_updatestatus` après le 0x00bd, mais **omettait les stats de combat
dérivées**. Ajout de `SP_ATK1/2, SP_MATK1/2, SP_DEF1/2, SP_MDEF1/2, SP_HIT,
SP_FLEE1/2, SP_CRITICAL`. Plafond porté à l'int32 complet.
🔴 **Leçon** : `get_xrefs_to` avait **raté le vrai écrivain 32 bits** (0x00ccfa36),
parce que Ghidra n'avait jamais analysé la fonction 0x00cceff0, atteinte seulement
par la table de dispatch indirecte — pas de fonction découpée, pas de xref. Quand
une liste de xrefs paraît suspicieusement courte, recouper avec une recherche de
motif live ou un breakpoint matériel en écriture.

### 14. Dual-wield : l'arme remplace toujours la main GAUCHE
Cru bug client pendant longtemps, **c'est le serveur**. Moonlight est compilé
**PRE-RENEWAL** (`config/renewal.hpp` : `#define PRERE` décommenté, donc `RENEWAL`
non défini — ⚠ le dossier `db/re/` est un leurre). Dans `pc_equipitem`, branche
dual-wield :
```c
#else   // pre-renewal
    pos = equip_index[EQI_HAND_R] >= 0 ? EQP_HAND_L : EQP_HAND_R;
#endif
```
Règle pre-RE = « si la main DROITE est occupée, mettre à GAUCHE » ⇒ les deux mains
pleines, on remplace **toujours** la gauche. La règle renewal est celle que le
joueur veut.
**Correctif** : adopter la forme renewal
(`(hand_R >= 0 && hand_L < 0) ? EQP_HAND_L : EQP_HAND_R`). Le glisser explicite
vers l'emplacement gauche continue de marcher (un seul bit `EQP_HAND_L` envoyé,
la branche est sautée).

### 15. Faux kicks « sans DLL Bourgeon » — deux causes distinctes
**(a) Au changement de personnage.** Désalignement de cycle de vie : le client
envoyait `CZ_BOURGEON_INTEGRITY 0x0BFB` **une fois par processus**, le serveur
l'exige **par session zone**. L'enum `ModeType` ne connaît que `{kLogin, kGame}` —
**le char-select est une fenêtre DANS `CLoginMode`, pas un mode** — donc la
transition retour est dédupliquée et rien n'est réémis.
Correctif serveur : `s_verified_login` (account_id → login_id1) ; à `connect_new`,
octroi direct si le `login_id1` correspond. `login_id1` est stable sur toute la
session compte et régénéré à chaque vrai login, donc un relog triché ne matche
pas. Correctif client en défense en profondeur : observer **ZC_ACCEPT_ENTER
0x02eb** et réarmer — miroir exact de `connect_new`.
**(b) Handshake lent au premier login.** Deux timers sur `connect_new` :
`clif_bourgeon_check_dll_timer` à +15 s (broadcast + programme le kick) puis
`clif_bourgeon_integrity_kick_timer` à +5 s (`clif_authfail_fd(fd, 3)` = la boîte
native « time gap… (3) »). Le premier **ne consultait jamais `exempt_ips`**, le
second re-testait `exempt_ips` mais **pas `has_bourgeon`**. ✅ Les deux gardes sont
en place dans `clif.cpp` (vérifié 2026-08-16).
🔴 **Elles avaient déjà disparu une fois** : écrites le 01/08 dans l'arbre de
travail seulement, jamais committées nulle part (`git log --all --reflog -S`).
Le mécanisme de la perte n'a **jamais** été établi — ne pas répéter que « le reset
les a emportées ».

## Refactors

### 16. Cache d'icônes mutualisé
Le squelette copié-collé dans 9 plugins est devenu deux couches :
`src/ui/game_texture.h` (**la vraie primitive** : charger un `.bmp` du jeu PAR
CHEMIN) et `src/ui/icon_cache.h` au-dessus (`ro::ItemIcon(nameid, identified)`,
garde d'epoch interne). La première passe s'était arrêtée trop tôt en croyant que
la primitive était « l'icône d'item ». **Clé = le COUPLE (nameid, identified)** :
les copies ne mémorisaient que le nameid, donc le premier état vu figeait l'icône.
Ce que ça a acheté : le crash d'epoch de device a **UN** point de correction au
lieu de neuf.
🔴 **Cinq caches locaux RESTENT, chacun pour une raison assumée** — ne pas les
« finir » : `item_desc_tweaks` (3 caches, icônes de COLLECTION par chemin et
illustrations de carte), `skill_bar_tweaks` (clé = skill|objet, deux espaces d'ID),
`menu_icons` (clé = un nom de fichier), `roggle_tweaks` (2 textures fixes),
`char_select` et `moonlight_auth` (images d'écran de connexion).

### 17. Refactor `moonlight_ui`
Audit du 26/07 → deux documents qui **font foi** :
`docs/moonlight_ui_audit.md` (16 bugs, 9 chantiers, 13 règles) et
`docs/naming_charter.md` (17 règles de nommage, 15 renommages prioritaires).
Faits et testés en jeu : chantiers 1 à 5 (PCH, persistance atomique, sortie du
toolkit vers `ui/ro_widgets.h`, source unique des 11 sections, découpage —
`OnRenderUI` 1702 → 165 lignes). Bugs B1 à B5 soldés.
⚠ **Chantier 7 (`fea5609`) : build vert, PAS testé en jeu.** `ui/color_codec.h`
porte les 6 conversions. **Les DEUX formats de couleur restent sur disque** (hex
ARGB pour chat_bg/dps/expbar/portrait/grid/skillbar ; **entier décimal ImU32**
pour `statusicon_*` et `ro_skin_*`) — les unifier casserait les yaml des joueurs.
`ro::ParseHex8` remplace les 7 `std::stoul` qui, sur une couleur corrompue,
faisaient perdre TOUTE la configuration (l'exception remontait au `catch` global
de `LoadSettings`).

---

## 18. Deux marqueurs qui mentaient — la leçon de cette archive

En condensant, deux fiches ont été prises en flagrant délit :
- **`storeall_latency`** portait « correctif PAS écrit » (état du 2026-08-08)
  alors que le rebouclage est dans `rag_connection.cc` depuis. Le **corps** de la
  fiche était périmé, l'accroche de l'index avait raison.
- **Quatre fiches sur huit** classées « impasse ⛔ » d'après leur marqueur étaient
  en fait livrées ou résolues autrement (`weapon_zorder`, `ro_skinning`,
  `4th_job_body_palettes`, `effect_zorder`) — corrigé par l'utilisateur.

➡ **Un statut écrit dans une mémoire est daté, pas vivant.** Avant de conclure
qu'un chantier est fait ou abandonné, le vérifier dans le dépôt : le fichier
existe-t-il, la constante est-elle là ? Cf. [[feedback_re_method]] §4
(doc corrigé ≠ code corrigé) et [[feedback_absence_needs_measurement]].

---

## 🗂 Index des chantiers soldés qui ont leur propre fiche (2026-08-29)

Sortis de `MEMORY.md` : l'index se lit à chaque session et ce sont les LIENS qui
y pèsent (195 × ~50 o). Ceux-ci sont **soldés** — on les rouvre en sachant ce
qu'on cherche, donc un cran plus loin suffit. 🔴 Le piège de chacun est recopié
ici : il ne doit pas disparaître avec le lien.

**Rien à retenir de plus que la fiche** : [Patch level](project_patch_level_enforcement.md) ·
[Bug report](project_bug_report_system.md) · [Palettes Doram](reference_doram_palettes.md) ·
[Hat effect .str](project_hat_effect_preview.md) · [Balises NPC](reference_npc_dialog_bourgeon_tags.md) ·
[Skillbar](project_skillbar_multibar_wip.md) · [Bouton cash shop](reference_cashshop_minimap_button.md) ·
[Weapon refine](reference_weapon_refine_re.md) · [Barre d'incantation](reference_cast_bar_re.md) ·
[Bulle de chat](reference_entity_chat_balloon_re.md) · [NPC sourds au clic](project_npc_click_block.md) ·
[Pet](reference_pet_system_re.md) · [Cellule d'item](project_item_cell_widget_todo.md) ·
[DOOM](project_doom_in_ro_todo.md) · [Peggle](project_minigames_imgui.md) ·
[parade au login](project_login_parade.md).

**Avec un piège, interne au sujet** :

- [Couvre-chefs](project_headgear_recolor.md) — 🔴 PAS le site du corps : **0x6060E7**.
- [Emotes au chat](reference_game_emotes_chat.md) — 🔴 index d'**ACTION** ≠ index de **PROTOCOLE**.
- [Homoncule](reference_homunculus_re.md) — 🔴 **CINQ** `ZC_PROPERTY_HOMUN`.
- [Tir de baguette](project_wand_ranged_attack.md) — levier = `BowTypeList` en **Lua** ;
  🔴 sprite du projectile = **JOB du tireur** ; `docs/wand_ranged_attack.md`.
- [Compteur de FPS](project_frame_counter_overlay.md) — 🔴 `ZC_NOTIFY_TIME` **0x007f**.
  ⚠ « FPS » désigne DEUX sujets : celui-ci et la vue 1ʳᵉ personne (abandonnée).
- [Char portrait](project_char_portrait_re.md) — 🔴 la **capture** a été SUPPRIMÉE
  (le pantin est composé, cf. [doll composer](project_doll_composer.md)).
- [Zone → GIF](project_zone_recorder_gif.md) — 🔴 capture **APRÈS** l'overlay.
