# Aperçu des hat effects sur l'avatar ImGui

> Journal du chantier. La fiche de mémoire `project_hat_effect_preview` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

Costumes SANS viewid mais avec `hateffect` script → effet `.str` (billboard additif),
pipeline client SÉPARÉ des sprites. But : les preview (cashshop) + les rendre là où on
utilise un sprite d'avatar (fiche perso). Doc complète : `docs/hat_effect_re.md`.

**Approche = miroir de la capture sprite** ([[project_char_portrait_re]]) mais pour le STR :
- Hook `Effect_SubmitStrQuad` 0x00bcfb10 (`Hooked_StrQuad`) : en capture, assemble les 4
  sommets 2D comme le natif (branches min/max workBuf[0xa..0x11]) + UV + couleur, supprime
  l'insert natif. Composite additif via `DrawStrCapLayers` + `D3D9_AdditiveBlendCallback` +
  `AddImageQuad`. Tout dans `basic_info.cc` (anonymous namespace).
- Spawn autonome (`CaptureHatEffectOrdinal` → `StrNode_CreateByName`) : `calloc(0x11ca8)` +
  ctor `EffectInst_Ctor_StrNode` 0x00b90780 + `Effect_LoadStrByEffectId` 0x00bb4170 avec un
  **id concret BIDON (StormGust 0x59)** pour le setup complet (préambule + init vtable), PUIS
  **swap** de la ressource .str (queue de LoadStr : `UITextureMgr_Load` 0x00a8d4a0 +
  `UITexture_AddRef` 0x00a8e800 + champs node+0x7ec/+0x7f0/+0x119f8) + `node+0x11c80=9999`
  (boucle). ⚠ **Chemin texture** : le .str stocke des noms de texture NUS ("coin001.bmp"). Le
  MÉCANISME NATIF (RE 2026-07-12, cf. `docs/hat_effect_re.md`) : le nœud in-world d'un hat effect
  name-based = classe **CEZ2STREffect** (vtbl 0x010758d8, ctor 0x00aeba00, taille 0x108) qui crée
  un **host CActorSprite** (+0xcc) ; `CActorSprite_LoadStrEffect` 0x00ad7d00 fait
  `_splitpath(resname,...,dir,...)` et STOCKE le dir dans un **std::string séparé host+0x3f78**
  (taille +0x3f88) — SANS modifier les noms de couche. Au dessin, `CActorSprite_SubmitStrQuad`
  0x00ad8c40 : si dir vide -> `Str_GetLayerTexture` 0x00715be0 ("effect\{nom}") ; sinon ->
  **`Str_GetLayerTextureWithDir` 0x00715d30** ("effect\{dir}{nom}", ex. "effect\efst_Gold_Shower\
  coin001.bmp"). Les DEUX cachent au MÊME slot `layer+idx*4+0x1c8`. Donc noms NUS + dir séparé.
  ✅ **SOLUTION (révisée)** : le hook `Hooked_StrQuad` appelle DIRECTEMENT le résolveur natif
  subdir-aware `Str_GetLayerTextureWithDir(layer, texIdx, &dir)` (dir = std::string MSVC POD
  reconstruite : `[buf16/ptr][_Mysize][_Myres]`, mode tas si len>15, remplie par `SetStrCapDir`
  depuis la partie dossier de GetHatEfResName, CASSE D'ORIGINE — pas de minuscule, le natif ne
  minuscule pas). Racine -> `Str_GetLayerTexture`. ⚠ **CORRECTION de l'ancienne règle "ne jamais
  écrire +0x1c8"** : écrire +0x1c8 VIA le résolveur natif est SÛR — c'est EXACTEMENT ce que fait
  le rendu in-world, même fonction, même slot, même TYPE d'objet (ctex du même factory). Le crash
  0xc0000005 d'avant (ebx=vtbl STR 0x01088de8 ; puis heap exécuté, render ddraw) venait d'avoir
  écrit un objet ÉTRANGER (résultat d'un factory maison FUN_00568760 pré-rempli) dans +0x1c8, PAS
  du fait d'écrire +0x1c8. Fini le cache maison `g_str_texcache` (le natif cache à +0x1c8 -> FPS
  natif). Toujours interdit : MODIFIER les noms de couche (layer+0x10+i*4) du CStr partagé.
  ⚠ après un build fautif, RELOG (le CStr corrompu persiste jusqu'au relog).
  🔑 **PILOTAGE + BOUCLE (RE EXACT 2026-07-12, PAS deviné)** : `node+0x178` = INDEX DE FRAME
  ENTIER (PAS des ms). Le driver natif l'incrémente de **+1 par tick** (`inc dword[node+0x178]`
  @0x00bc5131) à **CADENCE FIXE 16 ms/frame = 62.5 Hz**, INDÉPENDANTE du FPS de rendu
  (`GameLogic_ComputeTimestepCount` 0x00c16a90 : pas `DAT_015e5a9c`=0x10=16ms, flag
  `DAT_0122b3dc`=1 ; `EffectList_UpdateTick` 0x00a6ba40 appelé N fois/frame = N pas de 16ms). f(T)
  = floor(T_ms/16). `Effect_UpdateStrKeyframes` 0x00bced10 avance un keyframe SEULEMENT si
  `node+0x178 == kf.endFrame` EXACTEMENT (égalité entière) → il FAUT visiter chaque entier (donc
  incrémenter de +1 à la fois, UN UpdateKF par +1 ; JAMAIS écrire une horloge absolue = elapsed_ms
  → sinon les coins tombent sans fin ET pas de boucle). **BOUCLE = INTERNE au nœud** : quand
  toutes les couches finissent, UpdateKF appelle `Effect_ResetStrLoop` 0x00bb5b10 qui rembobine
  `node+0x178=0`, `node+0x119f4=0`, indices keyframe=0 ; plafond de boucles = `node+0x11c80`
  (valeur native des auras qui bouclent = **9999**, compteur `node+0x11c7c`). Donc : pilote
  Bourgeon = accumulateur DE DELTA (temps entre 2 captures RÉELLES, la fiche ne tick que visible →
  pas de fast-forward à la réouverture) : `acc += now-last ; steps = acc/16 ; acc %= 16 ;
  node+0x178 += 1 ×steps (1 UpdateKF chacun)`. `prime` = 1er tick → frame 0. ITÉRER les couches
  (flag node+0x119fc+L, Lyr 1..layerCount, bornées 48) → SubmitStrQuad hooké (bypass gardes
  FUN_00d9d020/FUN_00c0afa0). Nœuds EN CACHE par ordinal (persistants ~72Ko).
- Id → .str : ⚠ **PIÈGE : `GetHatEffectID(ordinal)` renvoie -1** pour les hat effects
  name-based (ex. GOLD_SHOWER ordinal 48). Le bon résolveur = **`GetHatEfResName(ordinal)`**
  (Lua) → CHEMIN du .str (ex. `"efst_Gold_Shower\coin2.str"`), chargé PAR NOM. Cf.
  `Effect_ResolveResourceName` 0x00af0900 (renommé Ghidra) : unified id (ord+0x98a) > 0x989 →
  GetHatEfResName. `HatOrdinalToResName` (bridge Lua brut). 0x0A3B `own_hat_effects_` (suivi
  dans `OnRecvPacket`) = ordinaux bruts actifs. Diag : LogDiag `[HatFx]` (resname/node_ok/
  layers/captured/anchor). HAT_EF_GOLD_SHOWER = ordinal 48 (script.hpp L1961-1913).

⚠ **2E FAMILLE — hat effects `hatEffectID` (EZ-particules, PAS `.str`)** — RE 2026-07-13, PAS ENCORE
GÉRÉ. `HatEffectInfo.lub` a 2 formes : `resourceFileName=.str` (géré) VS **`hatEffectID=<id>`**
(ex. `[HAT_EF_Digital_Space]={hatEffectID=1240}`, ordinal 87 via HatEffectIDs.lub). `GetHatEfResName(87)`
= **vide** → fiche perso NUE (validé). Ces effets = système **PARTICULES procédural "EZ-effect"** :
`GetHatEffectID(87)=1240` → msg acteur 0x6d → `Actor_ManagePrimitiveEffectList(0xc4a7a0)` case3 →
`Effect_SpawnPrimitiveById(actor,1240)` nœud (vtbl 0x01088de8, sur `ownActor+0x144`, CStr vide) →
**nœud ENFANT `+0x11c84`** (vtbl **0x01088c48**) dessiné par **`EzEffect_Draw`(0xb666d0)** (dispatch
`child+0x1d0`, ~170 sous-effets) → quad `child+0x2210` (.tga/.bmp) → **`EzEffect_SubmitQuad`(0x00550de0)
→ RenderQueue_InsertPrimitive**. 🔑 Ce chemin **ne passe NI par Effect_SubmitStrQuad NI par
Actor_SubmitSpriteQuad** (les 2 hooks actuels) → invisible partout dans Bourgeon. Détails
`docs/hat_effect_re.md` (section « Deuxième famille »). Ghidra renommé/commenté.
✅ **DÉBLOQUÉ (RE 2026-07-13) : live-capture-reproject FAISABLE.** Contrairement à une 1ère hypothèse,
les quads EZ SONT **pré-transformés ÉCRAN** : FVF `XYZRHW|DIFFUSE|SPECULAR|TEX1` (stride 0x20 : x@0 y@4
z@8 rhw@0xc argb@0x10 u@0x18 v@0x1c). Diag live tranché : vert0 z=0.9816∈[0,1], rhw=0.0025=1/w → écran, PAS
monde. Le composite `DrawEzCapTris` re-ancre du screen-pos joueur vers le doll. Restait UN bug : le
**sur-matching** (« nuages/near=1/60 ») — le filtre acceptait TOUTES les racines dont `+0x138==ownActor`,
donc AUSSI les auras skill/statut (ids 164/230), pas seulement le hat effect.
🔑🔑 **FIX FINAL = match par effectId via le nœud PRIMITIF parent.** RE décisive `EzEffect_Draw 0x00b666d0`
(vfunc vtbl 0x01088c48 slot +0xc) + `EzEffect_VisibilityGateForActor 0x00b7f8d0` : le nœud EZ ENFANT (celui
qui SUBMIT les quads = `rec-0x2210`, vtbl 0x01088c48) a **`+0x140` = le nœud PRIMITIF parent** (vtbl 0x01088de8,
liste actor+0x144), `+0x1d0` = SOUS-TYPE de rendu (0..0xad, ex. 33=TintedRotatedQuad ; PAS l'id), `+0x144` =
tableau de textures, `+0x2210` = record quad (4 verts). Sur le PRIMITIF : **`+0x168` = effectId (1240)**,
`+0x138` = acteur source, `+0x11ca0`/`+0x11c98` = flags. Donc `EzOwnedRoot` : vtbl==0x01088c48 → `prim=*(ez+0x140)`
→ `id=*(prim+0x168)` → match si `id ∈ cibles`. Repli Lua KO : `*(prim+0x138)==ownActor`.
⚠ **DEUX FAUSSES PISTES (ne pas refaire)** : (1) remonter `+0x140` en cherchant vtbl==0x01088de8 dans la liste
+0x144 par identité pointeur → échec (le walk partait ailleurs) ; (2) `EffectNode_InitFromActor 0x00b46950`
(owner@+0x140/effectId@+0x1d0) = AUTRE type de nœud (init différent), red herring → `seenOwned=0 childIds=[33]`.
Le bon lien = `enfant+0x140` (=primitif) puis `primitif+0x168` (=id). Un effet peut avoir plusieurs enfants EZ
(sous-émetteurs), tous avec `+0x140` vers le même primitif → tous capturés.
🔑🔑🔑 **PIÈGE FINAL = les effets à PARTICULES submittent à `rec = node+0x2210 + i*0x6c`** (stride 0x6c,
`EzEffect_EmitParticleQuadProjected 0x00b7c4e0`, i=indice particule 0..0x1fd), PAS au `node+0x2210` fixe des
effets « inline ». Donc `node = rec-0x2210` ne retombe sur le nœud QUE pour l'inline (footprint 230) → seul 230
était vu (`childIds=[230]`), Digital_Space (1240, à particules) RATÉ. **SOLUTION = hooker `EzEffect_Draw
0x00b666d0` (vfunc, `this`=nœud connu directement)** : on y établit l'appartenance (EzOwnedRoot(this),
save/restore contre nesting → `g_ez_cur_owned`), et le hook `EzEffect_SubmitQuad` capture TOUT quad submité
pendant un draw owned — SANS recalculer node depuis rec. Layout quad identique (3 verts XYZRHW stride 0x20,
tex@rec+0x60) pour inline ET particule. Verts = ÉCRAN (World_ProjectPointToScreen par coin). Ancre = nœud+0x10.
Diag `drawnIds=[...]` = ids des nœuds réellement DESSINÉS (doit inclure 1240).
🔑🔑🔑🔑 **VÉRITÉ FINALE : Digital_Space (1240) NE rend PAS en particules — c'est un SPRITE .act billboard.**
`drawn=[1240:168 230:33]` : sous-type 1240 = **0xa8 = `EzEffect_DrawSub_SpriteActBillboard 0x00b8e050`** (le
footprint 230 = sous-type 0x21 particules). Ce sous-renderer émet ses quads via **`Actor_SubmitQuad_RenderQueue
0x00c4a0d0`** (chemin sprite/ACT, PAS `EzEffect_SubmitQuad`) → d'où `seenOwned=0` avec le hook sur EzEffect_SubmitQuad.
✅ **SOLUTION UNIFIÉE = hooker le PUITS COMMUN `RenderQueue_InsertPrimitive 0x00550b10`** (où convergent
EzEffect_SubmitQuad ET Actor_SubmitQuad_RenderQueue). Record uniforme : **`prim[0]` = float* 4 sommets XYZRHW
écran (stride 0x20 : x@0 y@4 z@8 w@0xc color@0x10 u@0x18 v@0x1c), `prim[2]` = CTexture\*** (handle DX9 @ +0x12c),
`param_2` = flags bucket/blend. On capture quand `g_ez_cur_owned` (posé par le hook EzEffect_Draw). Marche pour
particules ET sprites .act. QUAD (4 sommets v0=HG v1=HD v2=BG v3=BD) → `AddImageQuad(v0,v1,v3,v2)`.
⚠ LEÇON : l'utilisateur avait dit dès le départ « c'est des effets .spr » — l'indice « .spr/hateffectID » signalait
le pipeline SPRITE (pas particules) ; l'écouter plus tôt aurait fait gagner des itérations.
🔑 **Cibles = `GetHatEffectID(ordinal)` (Lua)** : ordinal 87 → 1240. ⚠ Le bridge Lua brut renvoyait -1 ; **FIX =
appeler `lua_checkstack(L,n)` 0x0051b570 AVANT les push** (comme le natif `Lua_CallGlobal_va` 0x00a9a7d0) — sans
ça, pile Lua pleine en OnTick → push perdus → pcall échoue → -1. Bridge = DOUBLE deref `g_pLuaStateMgr`
0x015ffd78 (`*=mgr`, `**=lua_State`), retour via `lua_tonumber` 0x0051ad20. Validé : `target0=1240 tcnt=1`.
Composite `DrawEzCapTris` re-ancre : `ax,ay=projeté(node+0x10=pos acteur)`, `doll=ox+(vx-ax)·R`, `R=(s/S)·cal`.
Capture+filtre+composite dans basic_info.cc (`g_ez_enabled=true`). RESTE : valider rendu doll (build user).

⚠⚠ **3E FAMILLE — hat effects `hatEffectID` type CEffectMgr (aura/statut, PAS EZ ni .str)** — RE + FIX 2026-07-16,
✅ **ANIMÉ + CAPTURÉ VALIDÉ UTILISATEUR.** Ex. **Perm_Frost (hatEffectID 2429, item 410368)**. Diag décisif : `@effect
2429` (CZ ZC_NOTIFY_EFFECT 0x1f3) → `EffectDispatch_SpawnByEffectId` 0x00ceeff0 case 0x97d → **`EffectMgr_SpawnEffect`
(0x00ac12e0) id 0x66a PUIS 0x66b** = une **classe concrète CEffectMgr** (singleton `DAT_015beeac`, ~130 classes RTTI, cf.
`docs/effect_system_re.md`). Mesuré : l'aura **N'EST PAS** sur `actor+0x144` (nodeIds=[230,164] sans elle) et **draws=0 via
`EzEffect_Draw`** → invisible aux hooks EZ ET .str (GetHatEfResName vide). 3ᵉ système distinct.
🔑 **CHEMIN DE RENDU (workflow 4 agents Ghidra, 2 convergents high-confidence)** : phase RENDER chaque frame →
`CScene_RenderCellsAndCursor` 0x00a7b0a0 (= `gamemode+0xd0`->vtbl+0xc, appelée par GameMode_InGame_ProcessFrame
0x00c74a80 à 0xc74fb5, avant flush g_SceneRenderQueue) → **`EffectMgr_RenderAttachedEffects` 0x00ad5700** (walk map
par-acteur `mgr+0x10`, 1 seul appelant) → **`EffectInstance_RenderDraw` 0x00ae8480** par effet [gate `OptionInfo_GetValue(8)`
OU effet+0xc2 ; visibilité vtbl+0x7c = `EffectInstance_ShouldDraw` 0x00ae82d0 ; cull effet+0x34] → **le vrai draw vtbl+0x10**
(surchargé par classe, ex. CJupitelThunderStorm 0x006d2790) → `CActorSprite_RenderStrLayers`/`EzEffect_SubmitQuad` →
`RenderQueue_InsertPrimitive` 0x00550b10. Tous renommés+commentés Ghidra.
⚠ **PIÈGE SIBLING** : `EffectInstance_TickAndDraw` 0x00ae85d0 (appelé par `EffectMgr_UpdateAttachedEffects` 0x00ad55f0
depuis la phase UPDATE catch-up ~62.5Hz) fait vtbl+8 (tick) puis vtbl+0xc — **PAS le submit visible**. Hooker CE sibling
→ `captured=0` (mauvaise phase). Le RENDER est le 0x00ae8480, PAS le 0x00ae85d0.
✅ **FIX = hooker `EffectInstance_RenderDraw` 0x00ae8480** (`__fastcall` ECX=effet ; prologue PUSH ESI/PUSH 8/MOV ESI,ECX =
5o propres, relocalisable). Armer `g_ez_cur_owned` autour quand **`*(effet+0x20)` (owner handle) == `*(0x015fb9a4)`** (handle
joueur, cf. `Actor_ResolveByHandle` 0x00ad6a10 / `EffectInstance_ShouldDraw`). Le hook `RenderQueue_InsertPrimitive`
existant capture les quads. **COUVRE toute la famille CEffectMgr** (aura/statut/costume). Layout pos effet = **`+0x8/c/10`**
(X/Y/Z, ≠ nœud EZ `+0x10`) → flag `g_ez_via_efftick` choisit l'offset d'ancre.
🎬 **ANIMATION = capture LIVE, PAS freeze.** L'ancien `DrawEzCapTris` figeait UN instantané (position stable mais anim
gelée). NOUVEAU : dessine `g_ez_caps` **LIVE chaque frame** + ancre écran **projetée live** (`Scene_ProjectWorldToScreen`
sur `g_ez_world` = `effet+0x8`) → `(vx-ax)` annule la translation quand le perso marche, l'animation joue. `g_ez_count`/
`g_ez_world_set` **reset en fin de DrawEzCapTris** (chaque frame = 1 instantané). Bbox FIT **figée une fois** (`g_ez_frozen_valid`,
recalc au 0x0A3B) pour que le scale doll ne tremble pas.
🔧 **FIT floor** : `g_ez_fit_floor` (défaut 0.6) plancher du rétrécissement — le doll ne descend pas sous 0.6× base même si
l'aura s'étale large (au-delà, elle déborde/est rognée) ; sinon « doll minuscule ». Monter (0.7-0.8) = doll plus grand + effet
rogné ; baisser (0.5) = fit mieux + doll plus petit. + le FIT garde le cadrage du CORPS (centré H, pieds bas) au nouveau
scale — PAS de re-centrage sur la bbox de l'effet (sinon le perso part sur le côté).
🎯 **RENDER-ORDER / Z-ORDER (RE workflow 3 agents 2026-07-16, HAUTE CONFIANCE)** — comment le jeu ordonne sprites/hats/
costumes/effets. La **scene render-queue** (`g_SceneRenderQueue` 0x012515f8) = tri À DEUX NIVEAUX, PAS un z unique :
- **Niveau 1 (primaire, devant/derrière) = LE BUCKET** : `RenderQueue_InsertPrimitive 0x00550b10` range chaque prim dans 1 des
  ~17 buckets (this+0x15c..+0x234) selon les BITS de `param_2` (flags). Le flush `World_RenderScene 0x00552fa0` (dernier vfunc
  de `CScene_RenderCellsAndCursor 0x00a7b0a0`) vide les buckets dans un **ORDRE FIXE** codé en dur → c'est CET ordre qui décide
  devant/derrière, pas une profondeur partagée. Séquence : opaques → 0x210(STR before) → 0x21c → **PERSO 0x180**(flag 0x101) →
  0x1c8 → sprites 0x18c/0x1a4 → robe 0x228 → 0x1d4 → **0x1ec**(flag 5, hat effects après=devant) → 0x1e0 → 0x198 → **0x234**
  (0x205, additif topmost).
- **Niveau 2 (secondaire, intra-bucket trié) = profondeur au sommet `+0xc`** (le w/RHW, PAS le z@+8) : `(v0[+0xc]-v1[+0xc])*0.5
  +v1[+0xc]` (ou v0[+0xc] brut pour buckets sprite de base), tri ASCENDANT (petit=derrière) par `RenderQueue_SortBucket
  0x0054ff80`/`RenderQueue_BucketInsertionSort 0x0054e450`. Buckets opaques + STR (0x1ec/0x210) = ordre de SOUMISSION (non triés).
- **`isRenderBeforeCharacter` = pur SÉLECTEUR DE BUCKET** : au load (`CEZ2STREffect_QueueSoundByEffectId 0x00aed3d0`), le Lua
  IsRenderBeforeCharacter (0x00aee912) écrit le flag dans host `CActorSprite+0x1c` : **0xd (bit 0x8 = AVANT perso, bucket 0x210)**
  / 0x5 (après, 0x1ec) / 0x205 (additif topmost). **Bit 0x8 de param_2 = "render before character" universel.**
- ⭐ **LIMITE MOTEUR** : un effet est TOUJOURS tout-devant OU tout-derrière le perso — jamais intercalé entre ses couches. Donc
  pas besoin de z par-quad relatif au perso : le BUCKET (bit 0x8) suffit.
- Perso : parts corps/tête/arme (flag 0x101) → bucket 0x180 ; leur ordre interne = arbre RB par-acteur (`CActorSprite_
  DeferQuadSorted`, clef node[4] = build-seq OU priorité Lua DrawOnTop) ré-encodé en `seq*0.0002` d'invW dans `Actor_Submit
  Quad_RenderQueue 0x00c4a0d0` (offset +0xc). Costumes/coiffes DrawOnTop = grande priorité → +0x18c dessiné après.
✅ **RECETTE DOLL (implémentée, remplace le flag Lua par-effet)** : on capture DÉJÀ `param_2` dans `EzCapTri.blend`. Dans
`DrawEzCapTris(dl,ox,oy,s,before)` : filtre par-quad `(T.blend & 0x8) != 0 == before`. Appelé 2×/frame — phase `before=true`
AVANT le sprite (derrière), `before=false` APRÈS (devant) — puis `EndEzCapFrame()` vide `g_ez_caps`. Gère plusieurs effets d'un
coup (ordre de capture = ordre de soumission natif), zéro Lua. ⚠ Exclusion `CEZ2STREffect` (vtbl 0x010758d8) de la capture
live `EffectInstance_RenderDraw` (kCEZ2STRVtbl) : les .str name-based ont leur chemin piloté (DrawStrCapLayers) → sinon
double-dessin + mauvais z. Fn Ghidra renommées : RenderQueue_SortBucket, RenderQueue_BucketInsertionSort, RenderPrim_DrawRecord,
RenderQueue_SortedBucketAppend + commentaires sur RenderQueue_InsertPrimitive & World_RenderScene.

**FAIT** : fiche perso (uniquement les `.str` name-based). `RenderPlayerAvatar` superpose les hat effects actifs, ancrés sur la
région tête (aucune édition de character_sheet.cc — il appelle déjà la fn). Testable avec un
costume-hateffect équipé (ex. item 31091). Nécessite charsheet_imgui ON + Alt+F.

**FAIT aussi** : cashshop + item_desc (survol d'item, même NON équipé). Le client ne mappe
PAS item→ordinal (`effectHatItemTable` = simple liste d'appartenance ; `GetHatEffectID` prend
l'ordinal). Source autoritative = scripts item_db. **Push moonlight ZC 0x0F17
ZC_BOURGEON_HATEFFECT_MAP** (au login, clif_bourgeon_grant_verified) : le serveur scanne
`idata->script_src`/`equip_script_src` pour `hateffect HAT_EF_x`, résout via
`script_get_constant` (case-insensitive), envoie {itemId(client), ordinal}. Client :
`RegisterRecvOpcode(kHatEffectMap)`, cache `g_hat_item_ord`, `ItemToHatOrdinal`. Câblé dans
cashshop_tweaks + item_desc_tweaks (RenderItemPreviewTooltip(view, empl, ordinal) ; view==0 =
perso base + effet). Générateur/vérif : parse item_db (192 items ; 31091→48 gold_shower).
Serveur : packets_struct.hpp/clif.hpp/clif.cpp. Client : bourgeon_opcodes.h (kNextFree 0x0F18).

🔑 **TRANSFORM ÉCRAN + ANCRE + BLEND (RE EXACT 2026-07-12, disasm+live)** :
- **Transform** (`Effect_SubmitStrQuad` 0x00bcfb10) : par coin, `rotX=xo·cos−yo·sin`, `rotY=xo·sin+yo·cos`
  (xo=workBuf[0xa+k], yo=workBuf[0xe+k]) ; `screenX=(rotX+(workBuf[0]−320))·S+anchorX`,
  `screenY=(rotY+(workBuf[1]−240))·S+anchorY`. 320/240 = CENTRE canvas .str (DAT_01022f5c/01013e88),
  PAS une origine de projection. S = `Effect_DepthToScreenScale` 0x00553e80 = queue[+4]·invW/7.0.
  anchorX/Y = effect+0x154/0x158. → **notre `DrawStrCapLayers` reproduit ce calcul EXACTEMENT** ;
  seul (ox,oy) importe. ⚠ **ANGLE = 1024 par tour** (pas degrés) : `rad = workBuf[0x15]·2π/1024`
  (0.006135923f), PAS ·π/180.
- ✅✅ **ÉTAT FINAL QUI MARCHE (2026-07-13, validé utilisateur)** : **une seule ancre pour TOUS les effets =
  l'ORIGINE de l'acteur (pieds)** -> `hoy = oy` (oy = écran de capture (0,0)), `hox = ox + hatEffectPosX·s`,
  contenu `.str` à l'échelle `s`. Chaque `.str` place SON contenu par rapport à ça : cercle magique AU SOL
  (Blue Magic Circle, contenu au centre canvas = pieds), pluie de pièces AU-DESSUS (gold_shower), scène
  CENTRÉE (Autumn Evening). Z-order = `IsRenderBeforeCharacter` (Lua) : scènes derrière, pièces/cercle devant.
  `hatEffectPosX` (Lua GetHatEfPosX) = décalage horizontal. Les effets NE suivent PAS l'assise (confirmé
  in-world : ils restent à l'origine sol) -> ancre origine = correct. ⛔ **Approches ABANDONNÉES (fausses,
  ne pas refaire)** : (1) ancre tête / alignement du contenu sur le haut de tête -> casse les effets SOL
  (cercle magique posé sur la tête) ; (2) `Actor_GetHatEffectPosOffset` (renvoie 4 pour gold_shower = minus-
  cule, pas le lift tête) ; (3) tilt caméra 0.6428 + calibrage S. LIMITE ASSUMÉE : le lift ÉCRAN -80 natif
  des effets tête/scène (CActorSprite_ComputeStrScreenAnchor 0x00ad8570, applique
  `anchor = projeté(pieds) + (0,-80,0)` px ÉCRAN pour les effets non-tête) N'est PAS reproductible à plat
  sans l'échelle de rendu en jeu S (offset écran fixe, zoom-dépendant) -> ces effets sont un peu bas, les
  effets sol sont exacts. Ghidra doc : CActorSprite_ComputeStrScreenAnchor, CActorSprite_UpdateStrLayers
  0x00ad7fc0, correction Actor_GetHatEffectPosOffset.
- 🔑 **SOURCE Lua (HatEffectInfo.lub)**
  (trouvé par l'utilisateur 2026-07-13). Chaque entrée `[HatEFID.HAT_EF_x]` a : `resourceFileName`
  (.str), **`hatEffectPos`** (décalage vertical, unités canvas .str ; <0 = vers le haut), **`hatEffectPosX`**
  (horizontal), **`isRenderBeforeCharacter`** (true = effet DERRIÈRE le perso), + `isAdjust*WhenShrinkState`.
  Getters GLOBAUX Lua (appelés avec l'ordinal, comme `GetHatEfResName`) : **`GetHatEfPos`** (0x01085f30),
  **`GetHatEfPosX`** (0x01085ef4), **`IsRenderBeforeCharacter`** (0x01085eb4). Bourgeon les appelle via
  l'API Lua brute (kLuaGetFieldB/PushNumB/PCallB + `lua_tolstring`+atof pour les nombres,
  **`lua_toboolean` 0x0051abf0** pour le flag) -> `HatOrdinalParams(ord)` caché. RECETTE DOLL FINALE
  (basic_info.cc) : `hox = ox + hatEffectPosX·s`, `hoy = oy + hatEffectPos·s` (ox,oy = origine acteur,
  corps capturé à (0,0) ; ×s = échelle du contenu, PLAT, pas de tilt), et `isRenderBeforeCharacter`
  décide l'ordre : effet dessiné AVANT le sprite (derrière) ou APRÈS (devant). ⭐ **Z-ORDER : le .str
  n'a AUCUN z** (format STRM : header + textures + keyframes pos/uv/coins/rot/couleur/blend, zéro
  profondeur). `Effect_DrawStrFrameQuads` 0x00bcfadb passe un depthIdx = ordre de submit (0,2,4…) que
  `Effect_SubmitStrQuad` IGNORE ; la vraie profondeur = `Scene_ProjectWorldToScreen(this+0x138+0x10)` =
  position monde de l'effet, insérée dans la file partagée `RenderQueue_InsertPrimitive(queue,rec,5)`.
  Pour un hat effect = profondeur ≈ acteur -> le derrière/devant vient de `isRenderBeforeCharacter` (Lua).
  ⛔ **Abandonné (rejeté par l'utilisateur, à raison)** : recalculer l'ancre avec `11` (Actor_GetHatEffectPosOffset)
  + tilt `0.6428` (proj[0xd8]) + calibrage live de S. C'était du hardcode/projection ; la vraie donnée est
  dans le .lub. [[feedback_native_replacement]].
- **ANCRE + ÉCHELLE (RE live+disasm 2026-07-13, HISTORIQUE — remplacé par le Lua ci-dessus)** :
  `Actor_ComputeHatEffectAnchorPos` 0x00aebc50 : ancre = **ORIGINE DE L'ACTEUR** (owner+0x24/28/2c,
  la cellule SOL, PAS le centre de bbox ni la tête) + offset FIXE PAR JOB. Horizontal `FUN_00aebf50`
  = **0** sauf strangelights/superstar/ljosalfar (×jobScale). Vertical `Actor_GetHatEffectPosOffset`
  0x00aec250 = **sizeClass+0x12−7 = 11 px sprite** (sizeClass 0 humanoïde ; Merchant job 5 → switch
  sauté car 5−0x40b wrap → return sizeClass+0x12−7=11), SOUSTRAIT en Z (monte l'ancre) ×jobScale.
  AUCUN terme anim/pose → **pose-invariant**. ⭐ **ÉCHELLE : R = S_effet/S_sprite = 1/jobScale** : le
  corps (`CActorSprite_RenderLayered` 0x00603f00) fait `scale = jobScale·Effect_DepthToScreenScale`,
  l'effet fait `S = Effect_DepthToScreenScale` (même machinerie) → 1 unité canvas .str = 1 px sprite.
  🔑 **RECETTE DOLL EXACTE (aucun réglage)** : le corps est capturé à l'ORIGINE (0,0), donc l'origine
  acteur se projette EXACTEMENT en `(ox, oy)` (car `screen = ox+Lcx·s`, `oy+Lcy·s`, et l'origine a
  Lcx=Lcy=0). Donc `hox = ox`, **contenu .str à l'échelle `s`** (DepthScale, PAS hsc).
  ⚠⚠ **LIFT Y — CORRECTION 2026-07-13 (matrices caméra live lues)** : le décalage 11 est SOUSTRAIT en
  Z-VUE puis PROJETÉ par le tilt caméra. Matrices `pCam+0x98`(view)/`+0xc8`(proj) = rotations PURES
  (échelle 1), tilt RO `proj[0xd8]=0.6428=cos50°`, `proj[0xe4]=0.766`. screenY = `fVar18·0.6428 +
  fVar19·0.766 − 173` où fVar18(Z-vue) porte `−jobScale·11`. Donc **lift écran natif = 11·jobScale·0.6428
  ≈ 7 px** (PAS 11·hsc — j'avais confondu). Le CONTENU .str, lui, scale par `S=DepthToScreenScale`
  (`sceneCtx[+4]·invW/7`, par-profondeur) — DEUX facteurs distincts. Converti au doll (grossi ×s/S) :
  `lift_doll = 11·jobScale²·0.6428·s / S`. Comme en jeu le perso rend PETIT (S<0.64), lift_doll > 11·s
  → coins plus HAUT (symptôme « trop bas » = j'utilisais le lift plat 11·s). **AUTO-CALIBRAGE de S** :
  le hook sprite `Hooked_ActorQuad` mémorise l'objet corps joueur (1er `self` de NOTRE capture) puis
  capte au vol son `scale` quand le JEU le redessine en passthrough = `S` (jobScale·DepthToScreenScale) ;
  globals `g_own_body_self`/`g_live_render_scale`. Repli plat `11·s·jobScale` tant que S non vu (pas de
  régression). Log diag confirme feet=9.5 (l'origine .act est 9.5 px au-dessus du bas de bbox du corps). ⚠⚠ **BUG X CORRIGÉ 2026-07-13** :
  ancrer sur `ox+s_cx·s` / `oy+s_feet·s` (CENTRE de bbox) = FAUX — un costume asymétrique décale
  s_cx/s_feet mais PAS l'origine, donc l'effet restait figé au centre-fenêtre pendant que le doll
  glissait (ne suivait pas tête/corps). L'origine (0,0)→(ox,oy) est l'ancre correcte. ⚠ **ancien bug
  antérieur** : on ancrait vers la TÊTE → coins 1 hauteur-perso trop HAUT. ⚠ pièges RE live : pauses
  debugger >30s = déconnexion (données suspectes) ; le hook Bourgeon de Effect_SubmitStrQuad remplace
  le natif ; les constantes viennent de la MACHINERIE SPRITE + fonctions per-job (analytiques).
  **Tout le menu « Effet costume » SUPPRIMÉ** (sliders + checkbox on/off + member hat_enabled_ + YAML) :
  l'effet est **TOUJOURS actif**, rendu 100% auto. Rotation = 2π/1024 exact, winding fixe (0,2,3,1).
- **BLEND PAR COUCHE** (RE : keyframe +0x70/+0x74 → workBuf[0x1a]/[0x1b] → record+0x18/+0x1c →
  `World_RenderScene` 0x00552fa0 `SetRenderState(D3DRS_SRCBLEND/DESTBLEND)`) : facteurs D3DBLEND
  ENTIERS (MOV brut, PAS float → les lire en `((int*)workBuf)[0x1a]`, sinon cast float→int = 0).
  **gold_shower = SRCALPHA(5)/DESTALPHA(7), mtpreset 0 (MODULATE), tint blanc × alpha ANIMÉ**. Le
  backbuffer RO n'a pas d'alpha dest → HW traite DESTALPHA comme 1.0 → **additif modulé par alpha**
  (fond noir de la .bmp coin = invisible). Forcer alpha ImGui peignait le fond noir → **bord noir**.
  FIX = callback DX9 `D3D9_ExplicitBlendCallback` (src/dst encodés dans UserCallbackData), map
  DESTALPHA(7)→ONE(2), INVDESTALPHA(8)→ZERO(1), reste tel quel ; PAR COUCHE (certains effets
  mélangent additif/alpha). DX7 = repli alpha (device DX9-only, comme l'additif existant).

**Chemin natif complet (RE 2026-07-12)** : `0x0A3B` -> `Effect_ApplyEffectIdToActor` 0xc41ba0
(id=ord+0x98a) -> Lua GetEffectType -> `Effect_ApplyOrRemoveGenericEffect` 0xc41ec0 ->
`EffectMgr_SpawnEffect` 0xac12e0 -> crée **CEZ2STREffect** (vtbl 0x010758d8) : vtbl+0x6c
`CEZ2STREffect_SetEffectId` 0xafe670 (met +0xc8=1 si id≥0x98a) ; vtbl+0x4 `CEZ2STREffect_Load`
0xaecb90 -> `CEZ2STREffect_CreateHostSprite` 0xb1b3b0 (host CActorSprite -> this+0xcc) -> vtbl+0x80
`Effect_ResolveResourceName` 0xaf0900 (Lua GetHatEfResName) -> `CActorSprite_LoadStrEffect` 0xad7d00
(_splitpath dir -> host+0x3f78 std::string ; CStr -> host+0x2bc) -> `CActorSprite_InitStrLayers`
0xad7c80. Dessin : `CActorSprite_SubmitStrQuad` 0xad8c40 -> `Str_GetLayerTexture` 0x715be0 /
`Str_GetLayerTextureWithDir` 0x715d30. ⚠ `GetHatEffectID`(-1 pour name-based) = NO-OP :
`Effect_ApplyHatEffectViaLua` 0xc41ce0 SKIP si résultat≤0 -> le concret ne sert PAS à gold_shower.
Autres : `Actor_ToggleEffectId` 0xc44940 (ownActor+0x3fc), `EffectMgr_Init` 0xac2120,
`Client_LoadHatEffectLuaTables` 0xd64e99, `UITextureMgr_LoadResourceByName` 0xa8d9f0,
`CEZ2STREffect_Ctor` 0xaeba00. Live : CModeMgr 0x1213338 -> CMode ; ownActor=CMode+0xcc(actorMgr)
+0x2c ; set effets actifs ownActor+0x3fc. Cf. [[feedback_re_method]].
