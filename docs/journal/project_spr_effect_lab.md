# SPR Lab : rendre un hat effect .spr/EZ au centre de l'écran

> Journal du chantier. La fiche de mémoire `project_spr_effect_lab` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

Repart-de-0 (2026-07-18, demande utilisateur) de « rendre les hat effects .spr/EZ ailleurs que sur
le sprite perso ». L'ancienne voie (capture live + matching par propriétaire parmi 50 effets +
reprojection doll dans basic_info.cc, cf. [[project_hat_effect_preview]]) = jugée **trop complexe**.
But réappris **simple/basique** : afficher un effet `.spr` **AU CENTRE DE L'ÉCRAN**, découplé.

**Nouveau module autonome** : `src/plugins/spr_effect_lab.{h,cc}` (ne touche PAS basic_info). Piloté
depuis le debug UI : onglet **« SPR Lab »** dans `MoonlightUi::OnRenderUI` (bouton spawn/ordinal/id +
options) ; `spr_lab::RenderFrame()` appelé chaque frame (après le guard `in_game_`). Ajouté à
`src/CMakeLists.txt`.

**Principe (3 déblocages qui tuent la complexité)** :
1. **Spawn natif d'UN effet à nous** = `Actor_ToggleEffectId(ownActor, ordinal+0x98a, 1)` (0x00c44940,
   `__thiscall`). Le JEU crée le nœud primitif (vtbl 0x01088de8), l'enfant EZ (vtbl 0x01088c48),
   anime, dessine via les ~170 `EzEffect_DrawSub_*` — on ne réécrit **RIEN**. Despawn = `,0`. C'est
   EXACTEMENT ce que fait déjà `ReconcileEzPreview` de basic_info (spawn EZ prouvé).
2. **Hooks CHAÎNÉS** sur `RenderQueue_InsertPrimitive` (0x00550b10) + `EzEffect_Draw` (0x00b666d0) :
   le HookManager (`PrepareHook`) relocalise un `E9` déjà posé → un 2ᵉ hook s'empile SANS conflit
   au-dessus de ceux de basic_info (dernier installé = exécuté en 1er ; chaîne : moi → basic_info →
   natif). Vérifié dans `hook_manager.cc`.
3. **Match par IDENTITÉ** (fin de la fragilité) : `Hooked_EzDraw` arme `g_cur_owned` quand le nœud
   dessiné est le nôtre — vtbl==0x01088c48, parent `+0x140`, **id concret parent `+0x168` ==
   `g_applied_concrete`** ET acteur source `+0x138` == `g_applied_actor` (comparaison seule). Plus de
   heuristique propriétaire/Lua. ⚠ **L'id concret n'est PAS hardcodé** : résolu par le getter NATIF
   `GetHatEffectID(ordinal)` (bridge Lua `ResolveConcreteId`, adresses = HatLuaNum de basic_info :
   g_pLuaStateMgr 0x015ffd78 double-deref, `lua_checkstack` 0x0051b570 AVANT push, lua_tonumber
   0x0051ad20). L'UI ne saisit QUE l'ordinal ; l'id concret s'affiche résolu. `Hooked_Insert` snapshot le quad
   (4 sommets XYZRHW écran stride 0x20 : x@0 y@4 u@0x18 v@0x1c argb@0x10 ; tex = prim[2]=CTexture ->
   +0x12c DX9/+0x128 DX7). **SUPPRESSION in-world** : si `g_suppress`, le hook **NE chaîne PAS** pour
   nos quads → invisibles en jeu, visibles SEULEMENT dans l'overlay.

**Rendu centre** : `DrawCenteredOverlay` (ImGui foreground drawlist). Ancre écran = projection de la
pos monde du nœud (`node+0x10`) via `Scene_ProjectWorldToScreen` (0x005541b0, cam+0xd0 -> view+0x98),
repli = centre de la bbox capturée. `out = centre + (v - ancre)` → conserve forme+TAILLE natives,
juste recentré. `AddImageQuad` (sommets natifs v0=HG v1=HD v2=BG v3=BD → ordre v0,v1,v3,v2). D3DCOLOR
0xAARRGGBB → IM_COL32 0xAABBGGRR (swap R↔B). Blend additif optionnel (`D3D9_AdditiveBlendCallback`).

**Cas test = Digital_Space** : ordinal **87** (spawn). GetHatEffectID(87)=1240 (id concret, résolu
auto). Ordinal 87 dans HatEffectIDs.lub.

**PERSISTANCE au warp/@refresh (RE workflow wf_ed8e7656, haute confiance, 2026-07-18)** : au
changement de map l'acteur joueur est **DÉTRUIT+RECRÉÉ** (nouveau pointeur `operator_new(0x540)`) :
`ZC_MapChange 0x0091/0x92 → GameMode_ResetAndRecreateOwnActor 0x00c7e5e0 → ActorMgr_ClearAllActors
0x00a6be50` (free tous acteurs + `*(actorMgr+0x2c)=0` + EffectMgr_ClearAll) `→
ActorMgr_CreateOwnPlayerActor 0x00a6a0c0`. Le jeu NE rejoue QUE les effets **équipés** (statusId 0x37f
via `Actor_ApplyStatusChangeEffects 0x00d442f0` lisant g_StatusEffectList serveur) → un effet
**purement client** (notre toggle) n'y est pas → **perdu**. FIX = `Reconcile()` re-spawn sur perte,
détectée **UNIQUEMENT** par `actor != g_applied_actor` (le pointeur change toujours au warp). GATE
anti-crash OBLIGATOIRE : `Bourgeon::Instance().IsMapLoading()` (recv 0x91/0x92 → send 0x7d, cap 20 s)
`|| !IsGameActive()` → return (agir pendant le load = UAF). ⚠⚠ `g_applied_actor` **COMPARÉ, JAMAIS
déréférencé** (au warp il est libéré) ; despawn propre seulement si `actor == g_applied_actor`.
⚠ **PAS de branche « repli 0-capture > K frames »** (retirée) : elle re-togglait sur le MÊME acteur →
FUITE de nœuds tickés = bug « animation plus rapide » (ci-dessous).

**BUG "ANIMATION PLUS RAPIDE" (RE workflow wf_58f76cb9, 2026-07-18)** : anim EZ = **step-based** (Verlet
62.5 Hz `EffectNode_UpdateDispatchByEffectId` 0x00b46b50 = **vtbl EZ 0x01088c48 slot +4** ; `EzEffect_Draw`
+0xc = pur draw, n'avance rien). Horloge = **scene-node list** (GameMode+0xcc+8) parcourue par
`EffectList_UpdateTick` 0x00a6ba40 (N fois/frame) → **chaque nœud présent = +1 tick**. `Actor_ToggleEffectId`
fait un **remove-all/add-all** dont la passe *remove* ne nettoie PAS les nœuds orphelins (case 3 ne suit
qu'UN nœud +0x14c ; CEffectMgr `EffectMgr_SpawnEffect` 0x00ac12e0 ne déduplique pas life=0, remove
n'appelle jamais `EffectMgr_RemoveEffect`) → **chaque re-toggle sur le même acteur = +1 nœud tické →
2×,3×…**. Équip natif = 1 application = 1×. FIX = ne re-toggler QUE sur changement d'acteur.

**🔴 CAUSE RACINE des rendus cassés (id 829, 680) — RE 2026-07-18, PROUVÉE** : on supposait
« TOUJOURS 4 sommets stride 0x20 ». **FAUX** : le nombre de sommets est un CHAMP de l'enregistrement
(**+0x04**) et vaut **3** pour la majorité des effets EZ. Lire un 4ᵉ sommet inexistant lisait les octets
0x60..0x7f du buffer source = **le pointeur de texture + les entiers de blend réinterprétés en float**
→ exactement les sommets à ~1000 px et les bbox 900+ px observés (triangle blanc géant, « tout ou rien »
sur le filtre de rayon). ⚠ **Le filtre de proximité était un pansement sur un mauvais diagnostic** :
il n'y a **AUCUN clipping après insertion** dans le natif (seule garde : `vertexCount > 0`) — donc
aucun « quad géant invisible en jeu » n'a jamais existé. Le rejet écran est côté ÉMETTEUR
(`EzEffect_DrawSub_ActSpriteParticles` teste vs queue+0x28/+0x2c).

**LAYOUT RÉEL de l'enregistrement de primitive** (prouvé par le flush `RendererDX9_DrawPrimRecord`
0x0055c830, vtbl queue +0x38) : +0x00 pVertices | **+0x04 vertexCount** | +0x08 CTexture* | +0x0c
pIndexData(16b) | **+0x10 indexCount** (0 = non indexé) | +0x14/+0x15 ZWRITEENABLE | **+0x18 SRCBLEND /
+0x1c DESTBLEND** (D3DBLEND bruts → **le blend est PAR RECORD**) | +0x20 preset combineur |
**+0x24 D3DPRIMITIVETYPE**. Flush : `DrawPrimitiveUP(type, count/divisor, verts, 0x20)` avec
`divisor = (type==4) ? 3 : 2`. **Sommet = FVF 0x1C4** = XYZRHW|DIFFUSE|**SPECULAR**|TEX1, stride 0x20 :
x@0 y@4 z@8 rhw@0xc argb@0x10 **specular@0x14** u@0x18 v@0x1c.
**Les FLAGS (4ᵉ param) ne codent QUE le bucket de tri**, ni format ni compte ni blend.

**DEUX FAMILLES dans la même file** : (a) **STR/sprite** `RenderPrimRecord_Init` 0x00560190 → **4
sommets inline, TRIANGLESTRIP (type 5)** ; utilisée par les sous-types EZ 5-8,0x15,0x28,0x2e,0x33,
0x54,0x55,0x7c via `EzEffect_DrawSub_ActSpriteParticles` 0x00b639e0 → **c'est pourquoi CERTAINS effets
capturaient correctement**. (b) **EZ** `EzPrimRecord_Init` 0x0055ff00 + **`EzEffect_SubmitTriangle3V`
0x00550de0** (ex-`EzEffect_SubmitQuad`, nom trompeur = origine du bug) → **3 sommets, TRIANGLELIST
(type 4)** ; un « quad » EZ = **DEUX appels** de 3 sommets. Dispatch sur `node+0x1d0` (sous-type
0..0xad), PAS sur l'effect id.

**FIX appliqué** : capture lit vtxCount/primType/idxCount, ignore l'indexé et toute forme autre que
(type 4, n=3) ou (type 5, n=4) ; `Quad.n` porté partout (filtres, médiane, dessin) ; rendu en triangle
ou en quad selon `n`. Couleurs **par sommet** (gouraud) — `AddImageQuad` n'accepte qu'UNE couleur et
rendait opaques les traînées à dégradé d'alpha. 
**🔴 BLEND : GLOBAL = IMPOSSIBLE, il faut le blend PAR PRIMITIVE (2026-07-18) — ✅ VALIDÉ UTILISATEUR** — un même effet mélange
des primitives additives ET alpha dans la même frame, donc aucun mode global n'est correct :
en **alpha normal** les primitives que le natif dessine en additif sortent en **CARRÉS NOIRS** (noir
additif = invisible ; noir alpha = du noir — ce n'est PAS un problème de couleur transparente/colorkey,
c'est le mode de mélange) ; en **additif global** les primitives alpha **CRAMENT en blanc**. FIX =
rejouer `src_blend`/`dst_blend` capturés (record **+0x18/+0x1c**, D3DBLEND bruts) via
**`D3D9_ExplicitBlendCallback()`** (déjà existant, utilisé par basic_info pour les couches `.str`) :
`UserCallbackData` = octet bas SRCBLEND, octet suivant DESTBLEND. Callback émis **seulement quand le
couple CHANGE** (sinon on casse le batch à chaque primitive) + `ImDrawCallback_ResetRenderState` à la
fin dès qu'on y a touché (sinon toute l'UI suivante hérite du blend). `g_blend_mode` : 0 natif par
primitive (défaut) / 1 alpha / 2 additif global. ⚠ **DX9 seulement** (le callback agit sur le device
D3D9) ; sous DX7 inopérant. L'UI affiche les couples réellement capturés (« blends: 5/6 + 2/2 »).

**ANCRE (2026-07-18)** : défaut = **MÉDIANE des sommets finis** (`g_anchor_mode` 0) — déduite de la
capture, donc l'amas est centré par construction ET insensible aux outliers (contrairement au centre
de bbox). Modes 1 = projection monde (ancien défaut, peut tomber à ~1000 px de la géométrie),
2 = bbox. Combo « Ancre » dans l'UI.

**STATUT** : code ÉCRIT (POC centre VALIDÉ utilisateur ; persistance warp + no-hardcode AJOUTÉS,
**à rebuild/tester**). L'utilisateur build (cf. [[feedback_dont_relaunch_game]]). Test : Moonlight →
« Commands Settings » → onglet « SPR Lab » → « Spawn + afficher au centre », puis **warp/@refresh** →
doit re-apparaître seul.

**🔴🔴 DEUX CHEMINS DE RENDU MONDE — DX7 vs DX9 (découvert en live 2026-07-18, coûteux)** :
la famille **`0x00552xxx`** (`World_RenderScene` 0x00552fa0, `World_DrawGroundTiles` 0x00552710,
`World_DrawWaterSurface` 0x00552b70) est le chemin **DX7** et **n'est JAMAIS exécutée en DX9**.
Symptôme trompeur : le JMP hook s'installe très bien (trampoline non nul, `SetJmpHook` ne renvoie
le trampoline que si `ActivateHook` a réussi) mais **ne se déclenche pas une seule fois**.
Le chemin **DX9** réellement emprunté :
`RendererDX9_RenderScene 0x0055ca60` (Begin/EndScene) → **`RendererDX9_FlushWorldScene 0x0055e5b0`**
(jumeau de World_RenderScene) → `FUN_0055c5c0` (fog/projection) → `RendererDX9_DrawGroundTiles
0x0055d680` → **`RendererDX9_DrawTerrainSurfaces 0x0055d850`** ; puis `FUN_005511d0` = **RESET des
buckets, PAS un flush** (piège de lecture).
⚠⚠ **`RendererDX9_DrawGroundTiles 0x0055d680` s'exécute avec ses DEUX listes VIDES** (mesuré en live :
`liste sol=0 lightmap=0`, alors que `DrawPrimRecord total=18254`). **Le terrain visible est dessiné par
`RendererDX9_DrawTerrainSurfaces 0x0055d850`** (que le nom « DrawWaterSurface » du jumeau DX7 faisait
prendre à tort pour l'eau) : 3 branches selon `OptionInfo_GetValue(0x77)` + `g_AnisoFilterSupported` —
liste `[0x5a]`=+0x168 via vtbl **+0x38** (`RendererDX9_DrawTerrainSurfacesPlain 0x0055dd00`), liste
`[0x7e]`=+0x1f8 via vtbl **+0x34** (`RendererDX9_DrawPrimRecordDualTex 0x0055c8c0`, BI-texture), liste
`[0x5d]`=+0x174 via vtbl **+0x38**. ➡ **hooker le seul +0x38 ne suffit pas** : la branche aniso passe
par +0x34.
**MÊME objet, MÊMES listes** dans les deux versions (`param_1` est un `int*` → `[n]` = octet `n*4`) :
`[0x57]/[0x58]`=+0x15c/+0x160 (sol), `[0x6c]/[0x6d]`=+0x1b0/+0x1b4 (lightmap), `[0x97]`=+0x25c
(wrapper d'état, objet passé = wrapper+0xc), **`[0x98]`=+0x260 = `IDirect3DDevice9*` BRUT**.
Helpers wrapper : `FUN_00548690(w,type,val)`=SetRenderState, `FUN_00548750(w,stage,tex)`=SetTexture,
`FUN_005486e0(w,stage,type,val)`=sampler state à **énum propre** (type 1=ADDRESSU, 2=ADDRESSV).
➡ **Réflexe** : avant de hooker quoi que ce soit en `0x00552xxx`, chercher le jumeau `0x0055C/D/Exxx`.

**SOL UNI / fond de capture (2026-07-18)** : option « Sol uni » dans l'onglet SPR Lab — repeint tout
le terrain .gnd d'une **couleur réglable** (helper standardisé **`ColorPicker()`** de `moonlight_ui.h`,
portée globale — à utiliser partout plutôt qu'un `ColorEdit` brut). Hook JMP sur
**`RendererDX9_DrawGroundTiles` 0x0055d680**. On ENCADRE l'appel original en tapant sur le **device
COM brut `this+0x260`** avec la **vtable D3D9 STANDARD** (`SetRenderState`=index 57=+0xE4,
`SetTextureStageState`=index 67=+0x10C, `__stdcall`) — prouvée par `RendererDX9_DrawPrimRecord`
0x0055c830 qui appelle `+0x14c` = index 83 = `DrawPrimitiveUP`. ⚠ **NE PAS** passer par le wrapper
d'état : ses slots +0x50/+0x8c/+0x94 venaient de commentaires Ghidra faux (+0x50 charge en réalité
`scr_logo.bmp`). Séquence : `TEXTUREFACTOR=argb` + stage0 `COLORARG1=TFACTOR`, `COLOROP=SELECTARG1`,
appel original, puis restauration `MODULATE`/`TEXTURE` (le stage 0 sert à toute la suite de la scène).
⚠ **PIÈGE ENUM** : `SetTextureStageState(0, 0xc, 3)` dans la version DX7 est **`D3DTSS_ADDRESS`=CLAMP**
(1=WRAP pour la lightmap), **PAS COLOROP** — la base Ghidra l'affirmait à tort (corrigé). Double preuve :
`World_RenderScene` fait (0,1,4)+(0,4,4)=COLOROP/ALPHAOP MODULATE stage 0, (1,1,1)+(1,4,1)=DISABLE
stage 1, (0,0x12,3)=MIPFILTER LINEAR ; **et** la version DX9 remplace ce `(0,0xc,v)` unique par
**deux** appels ADDRESSU+ADDRESSV. Donc la fonction du sol ne touche jamais COLOROP → encadrer suffit,
inutile de réimplémenter les 2 boucles.
Non affectés : eau (`FUN_0055d850`), ciel (`World_ApplyMapSkyData` 0x0059b810), fog. Complément pour la
capture : `D3D9_RequestScreenshot()` shoote la frame **sans** l'overlay ImGui.

**🔴 « CET EFFET NE REND RIEN, MÊME NATIVEMENT » — RÉSOLU (RE 2026-07-18, PROUVÉ)** : ce n'est NI un
bug du lab, NI une ressource absente, NI une condition. Le dispatcher d'effect id **0x00bb9260**
(slot **vtbl+0x3c** de la vtable primitive 0x01088de8) fait `eax = effectId - 491 ; if (eax > 0x79a)
-> DEFAULT ; else jmp [eax*4 + 0x00bc2e04]`. **Table de saut 0x00bc2e04**, 1947 entrées, ids valides
**491..2437**, **DEFAULT 0x00bc2de1 = `mov al,1 ; ret`**. Un id sans entrée crée bien un nœud
(0x11ca8 o), poussé dans la scène ET dans acteur+0x144, **tické chaque frame, jamais détruit, et
totalement MUET** — aucun log, aucune erreur (d'où « aucune erreur ne ressort »). Chaîne :
`Actor_ToggleEffectId 0x00c44940 → Effect_ApplyEffectIdToActor 0x00c41ba0 → GetEffectType(ord) Lua
→ (type 1) Effect_SpawnPrimitiveById 0x00c44540` (seule garde : id<0 || id>60000).
**Contre-exemple décisif** : 1240 (Digital_Space) A un handler (0x00bc087b) et rend ; 1193
(Brysinggamen) n'en a pas. Même chemin, même chargeur → seule l'entrée diffère.
⚠ **Hypothèse « aura de niveau » RÉFUTÉE** : `CActorSprite_ApplyJobClassAura` 0x00c41950 est piloté par
le **job affiché** (JobId_Is4thClass / Job_IsInSpecialAuraSet) et émet des ids internes 0x7d/0x92/0x93/
0x96/0x97 via vtbl[8] — sous-système SANS RAPPORT. Les ids 1164-1183 (ord 59-78, LEVEL99/LEVEL160,
déclinaisons de couleur) ne sont référencés nulle part : 20 ids consécutifs pointant tous sur le DEFAULT.
**Aucun hat effect `hatEffectID` n'est « câblé mais conditionnel »** (famille vide).
**Chiffres** : sur 259 entrées HatEffectInfo.lub → 149 rendent via **.str** (`resourceFileName`,
`GetHatEfResName` ne lit QUE ce champ, **pas de repli .str** pour les entrées hatEffectID) ;
**seulement 18 des 110 `hatEffectID` rendent via EZ** (ord 8,16,25,26,27,28,29,30,31,34,35,36,49,50,
**87**,128,160,219) ; **92 sont inertes** (dont 57,58 / 59-78 / 82-84 / 99-120 / 140-147…).
**IMPLÉMENTÉ dans le lab** : `EffectIdIsImplemented(cid)` **LIT LA TABLE DU CLIENT** (jamais de liste
recopiée : vérité terrain + immunisé aux trous d'une analyse statique) → catalogue filtré
(« implémentés seulement », défaut ON) + marquage `[inerte]` grisé. Ancrage effet : offset numérique
`Actor_ComputeHatEffectAnchorPos` 0x00aebc50 (hatEffectPos/PosX du Lua), **pas de bone**.

**✅ CONFIG VALIDÉE UTILISATEUR (2026-07-18)** : **Blend = « Natif par primitive »** + **Ancre =
« Projection monde »** → bon blend ET bon centrage. Ce sont les DÉFAUTS du code (`g_blend_mode=0`,
`g_anchor_mode=1`). La médiane n'est PAS le bon défaut (mauvaise sur petite capture).

⚠ **CATALOGUE : « inerte » ≠ ne fonctionne pas** — un ordinal rend s'il a une entrée procédurale
**OU** un `resourceFileName` (**.str**). Ne tester QUE la table procédurale marquait à tort inertes des
effets qui marchent (repéré par l'utilisateur). `HatEntry.kind` : **0 inerte** / **1 EZ procédural**
(rendu ET capturable par ce lab) / **2 .str** (rend NATIVEMENT via le pipeline billboard STR = autre
chemin que `EzEffect_Draw` → a priori **NON capturé** par ce lab), teinté bleu + tag `[.str]`.

**2ᵉ analyse (workflow wf_fdeb962e, 2026-07-18) — CONFIRME le verdict, CONTREDIT sur le mécanisme.**
⚠⚠ **À PONDÉRER : 0 de ses 14 affirmations a survécu à sa propre passe de réfutation adversariale.**
Elle n'a PAS énuméré la table 0x00bc2e04 (fonction 0x00bb9260 non définie dans Ghidra) — donc elle ne
réfute pas le mécanisme ci-dessus, qui repose lui sur une lecture x32dbg + contre-exemple (1240 a un
handler et rend / 1193 non). Elle propose un autre modèle : 3 portes indépendantes —
`EffectMgr_RegisterValidEffectId` 0x00ad4af0 (1125 ids, trous francs 1164-1185 et 1193-1195),
`Effect_LoadStrByEffectId` 0x00bb4170 (case max 0x421=1057), `Effect_OnCreateBuildEmitters` 0x00bb80e0
(ids 300..373) — et affirme que `+0x1d0` est un **immédiat codé en dur chez les ~170 appelants** de
`EffectInst_BuildChildNode` 0x00bb5d10, PAS issu d'une table id→sous-type.
🔴 **FAIT UTILE ET CHECKABLE** : un fichier absent **N'échoue PAS en silence** — le loader 0x00a8d050
écrit **3 lignes « Resource File Loading fail » dans le chat**, compilé en RELEASE, et ne renvoie jamais
NULL (substitue un sprite vide puis un sprite par défaut 0x00568760/0x005688f0) ; nom d'effet produit par
du CODE (0x00b17570), préfixe `effect\` **ASCII** (pas de piège CP949). Donc silence ⇒ PAS un fichier
manquant. Test alternatif proposé (NON retenu, layout std::map non validé en runtime, risque de crash) :
`lower_bound` sur la map `*(g_EffectMgr 0x015beeac + 8)` via `EffectClassMap_LowerBound` 0x00ab64a0.
Auras de niveau : `CActorSprite_ApplyLevelJobAura` 0x00c41950 → `Actor_OnMsg` 0x7d/0x92/0x93/0x96/0x97,
gardé par **acteur+0x210 (base level)** + OptionInfo 0x6b.

**SONDE « pourquoi rien ne s'affiche »** (UI) : `N/M dessins / N inserts / N capturés` →
0 dessin = nœud non créé/inerte (AMONT) ; dessins mais 0 insert = sous-rendu muet (ressource/condition) ;
inserts mais 0 capturé = **notre** bug (filtre de forme). `g_match_mode` : 0 = id concret + acteur
(strict) / 1 = **acteur seul** — un effet riche peut avoir PLUSIEURS nœuds primitifs d'ids DIFFÉRENTS,
le strict n'en capture alors qu'une fraction (le reste n'est ni capturé ni supprimé et reste sur le
perso → illusion d'effet « non centré » ; constaté sur Digital_Space : 1 nœud, 2 primitives).
⚠ La MÉDIANE comme ancre est excellente sur ~250 primitives mais **mauvaise sur 2** (elle tombe sur un
sommet quelconque, pas sur le centre visuel).

**🔧 MODULE PARTAGÉ `ez_effect_capture.{h,cc}` (2026-07-18, demande utilisateur)** — la capture EZ était
sur le point d'exister en **3 copies** (lab + doll basic_info + fiche perso), alors qu'elle concentre
3 pièges coûteux. EXTRAITE dans `src/plugins/ez_effect_capture.{h,cc}` (ajouté au CMake).
API : `EnsureInstalled()` (hooks, idempotent) ; `SetOwnerActor(void*)` (⚠ comparé, jamais déréférencé) ;
`SetTargetIds(ids,count)` (**count==0 ⇒ tout effet EZ de l'acteur**) ; `SetCaptureEffectMgr(bool)` ;
`SetSuppressInWorld(bool)` ; `Prims()`/`Count()` ; `ProjectAnchor(&ax,&ay,&screen_scale)` ;
`Draw(dl, DrawOpts{ox,oy,scale,blend_mode,use_zorder,draw_behind,max_r})` ; `LastFrameStats()` ;
`EffectIdIsImplemented(cid)`. `struct Prim` porte **`n`** (3 ou 4), couleurs **par sommet**, **`flags`**
(bucket/z-order, bit 0x8 = derrière le perso) ET **`src_blend`/`dst_blend`** (le vrai blend) — la
confusion flags/blend était la cause du blend global cassé.
**3 CHEMINS hookés** : `RenderQueue_InsertPrimitive` 0x00550b10 (capture) + `EzEffect_Draw` 0x00b666d0
(appartenance EZ) + **`EffectInstance_RenderDraw` 0x00ae8480** (famille **CEffectMgr** : auras/statuts,
ex. Perm_Frost — invisible sans lui). ⚠⚠ **L'OFFSET DE POSITION MONDE DÉPEND DU CHEMIN** :
nœud EZ = **+0x10**, instance CEffectMgr = **+0x8**. Appartenance CEffectMgr = **handle** (effet+0x20 ==
`*(int*)0x015fb9a4`), PAS pointeur d'acteur ; exclure vtbl `kCEZ2STRVtbl` 0x010758d8 (.str name-based).
`spr_effect_lab.cc` = simple consommateur (1126 → 811 lignes). Modes d'ancre médiane/bbox SUPPRIMÉS
(projection monde validée) ; garde-fou « quad max » supprimé (c'était un pansement sur le 4ᵉ sommet
fantôme, cause éliminée) ; rouge/vert de l'overlay debug perdu (le module n'expose pas de prédicat de
rejet — à réexposer si besoin).
⚠⚠ **PIÈGE `SetTargetIds(count==0)`** (bug réel, 2026-07-19) : `count==0` = « accepter TOUT effet EZ de
l'acteur », **PAS** « ne rien capturer ». Combiné à `SetSuppressInWorld(true)`, ça **AVALE le rendu de
tous les effets du joueur** → la **barre d'icônes de statut disparaissait** tant que le lab était
ÉTEINT (spawn off ⇒ concrete=0 ⇒ match-all ⇒ suppression totale ; spawn on ⇒ 1 seul id ⇒ tout revient).
**Pour ne RIEN capturer : `SetOwnerActor(nullptr)`** — vider les cibles ne suffit pas. Corrigé dans le
lab (Reconcile ne transmet l'acteur QUE si `g_applied_concrete > 0`) et documenté dans l'en-tête.
À reproduire lors de la migration du doll.

**✅✅ TOUT VALIDÉ EN JEU (2026-07-19)** : lab (overlay centré), **aperçu voteshop + description
d'item**, suppression in-world de l'aperçu (perso net), doll ne montrant que l'équipé, cadre du doll
stable pendant un aperçu, icônes de statut OK, et **Perm_Frost ÉQUIPÉ rendu sur le doll** (chemin
CEffectMgr / `include_effmgr` confirmé).

**🔴 RE CEffectMgr (2026-07-19) — RENVERSE L'HYPOTHÈSE `include_effmgr`** :
- **effect id à `+0x04` (int32)**, posé par `Effect_SetEffectId` 0x00ae84c0 (**vtable slot +0x6c**),
  appelé depuis `EffectMgr_SpawnEffect` 0x00ac12e0. Champ de la **classe RACINE**
  `ActorAttachedEffectBase_ctor` **0x00ae7cd0** (vtable de base `0x010755f0`, ~100 sous-classes qui
  n'ajoutent leurs champs qu'à partir de ~+0xd0) → **sûr pour toute instance**. Vaut **-1** avant
  l'appel du setter. Layout base : +0x04 id | +0x08/0c/10 pos monde | +0x20 owner | +0x2c lifetime |
  +0xc4 life. Lu comme clé dans `EffectMgr_FindEffectByKey` 0x00ac2050 (clé2 = +0xc4).
- ⚠ **ESPACE D'IDS DIFFÉRENT** de l'id concret : `< 0x98a` = effet **générique** (skill/statut) ;
  `>= 0x98a` = hat effect, valeur = **ordinal + 0x98a**. L'id CONCRET (Digital_Space 1240, jumptable
  0x00bc2e04) n'est **JAMAIS** stocké dans une instance CEffectMgr ; sa résolution est purement **Lua**
  (`GetHatEffectID(ordinal)`). Les deux espaces **se chevauchent** → ne jamais les mélanger dans un
  même champ de filtre.
- 🔴🔴 **CONCLUSION DE LA RE CI-DESSOUS RÉFUTÉE PAR L'ESSAI EN JEU** : mettre `include_effmgr=false`
  a fait disparaître **TOUT** le rendu du doll ET de l'aperçu → les primitives de ces costumes
  arrivent bel et bien par le chemin **CEffectMgr** (`effect_id == -1`). ⚠ Ne pas re-tenter sans
  mesure. **Leçon : ne jamais appliquer une conclusion d'analyse statique sans la confronter au jeu.**
- 🔴 **PAS DE PLAFOND À 0x98a** (corrigé 2026-07-19, 3ᵉ hypothèse fausse sur ce champ) : j'avais borné
  `em_id < 0x98a` en croyant qu'au-delà on basculait dans l'espace `ordinal + 0x98a`. **FAUX** : les ids
  CONCRETS dépassent 0x98a pour les effets récents — mesuré **HAT_EF_C_2025RosFesta, ordinal 277 →
  2443 (0x98b)**, seul effet du voteshop à échouer. Il était rejeté (`effect_id = -1`) → absent de
  l'aperçu ET non masquable sur le perso. L'espace `ordinal + 0x98a` ne nous parvient JAMAIS : il
  correspond aux hat effects à `resourceFileName` → instances **CEZ2STREffect**, exclues par vtable
  (0x010758d8) dans le hook. ⇒ `g_cur_effect_id = (em_id > 0) ? em_id : -1;` sans borne haute.
  ⚠ Noter aussi : la **jump table procédurale ne couvre que 491..2437**, donc un id > 2437 est
  forcément non-procédural — cohérent avec le fait qu'il rende via CEffectMgr.
- ✅🔴 **ord 277 — CHEMIN IDENTIFIÉ EN DIRECT (x32dbg, 2026-07-19)**. ⚠ D'ABORD : ma théorie de
  « collision d'ids » (ci-dessous) est **RÉFUTÉE** — l'utilisateur voit le BON effet, identique qu'il
  soit porté ou spawné par toggle. `EffectMgr_SpawnEffect(2443)` trouve donc bien le bon effet.
  **L'effet rend correctement ; il n'est simplement PAS CAPTURÉ.** Mesures :
  `CEZ2STREffect_CreateHostSprite` **0x00b1b3b0** est appelé avec `ecx` = instance dont
  **vtable = 0x010758d8 (CEZ2STREffect)** et **+0x04 = 0x098B (2443)** ✓ ; sa valeur de retour est
  l'**hôte** : objet dont **vtable = 0x01074c5c**, **COL à vtable-4 = 0x010d16b4** ⇒ **`CEZ2STRParticle`**
  (hiérarchie CParticle → CSpriteParticle → CEZ2STRParticle). Cette vtable n'a que **2 entrées** :
  **0x00AD75A0** et **0x00AD7640** (le reste est de la donnée : regex `^(.+\\)(\w+\.str)$`).
  ⇒ Le rendu passe par le **cluster particules 0x00ad9xx–0x00addxx**, PAS par
  `EffectInstance_RenderDraw` 0x00ae8480 → d'où « ids CEffectMgr : aucun » et l'inefficacité de la case
  « Capturer aussi les .str ». Candidats submitters (RE) : FUN_00ada090 / 00ada590 / 00adb270 /
  00adca60 / 00adcc50 / **00adce30** (ce dernier vérifié : remplit un record en +0xf0 puis appelle
  `RenderQueue_InsertPrimitive`). **RESTE À FAIRE** : identifier lequel porte ces quads et hooker
  l'attribution via `CEZ2STREffect_CreateHostSprite` (hôte → id concret lisible à effet+0x04).

  ⚠ `0x00AD7640` (slot 1 de la vtable hôte) est une méthode de **LIBÉRATION** (elle se déclenche au
  DÉSÉQUIPEMENT), pas le dessin — écartée. ⚠ Les 6 adresses « cluster » proposées par la RE sont
  douteuses : `0x00ADCE30` ne contient **pas de code**. ⚠ **Impossible de poser un `bp` x32dbg sur
  0x00550b10** : notre propre hook JMP y est installé, la commande échoue.
  **OUTIL AJOUTÉ (la bonne approche)** : relevé des **APPELANTS** du puits, pris directement dans
  notre hook via `_ReturnAddress()` → `ez_capture::Callers()/CallerCount()` (`struct Caller{addr,count}`,
  trié par count, bascule à la frontière de frame) + case « Appelants du puits (diag) » dans le lab.
  **MÉTHODE** : relever la liste effet ÉTEINT, puis effet ACTIF — l'adresse qui APPARAÎT est la
  fonction à hooker. Ne dépend ni du debugger ni d'une analyse statique.
  ✅ **RÉSULTAT (mesuré) : `0x00ADA5A4` ×10, présente UNIQUEMENT effet actif** (toutes les autres
  adresses identiques OFF/ON). ⇒ appel au puits en **0x00ADA59F**, dans la fonction **0x00ADA590** —
  l'un des six candidats de la RE, et le seul confirmé.
  **FIX IMPLÉMENTÉ SANS 4ᵉ HOOK** : notre hook du puits reconnaît ces primitives à leur **PROVENANCE**
  (`_ReturnAddress() == kStrParticleRet 0x00ADA5A4`) et les capture même si aucun hook d'appartenance
  n'est armé. Elles sont marquées **`effect_id = kStrParticleId (-2)`** (famille ANONYME : l'id n'est
  pas accessible sur ce chemin) et incluses via **`DrawOpts::include_str_particle`**, mis à `true`
  **uniquement sur les surfaces d'APERÇU** (`with_preview`) — jamais sur le doll, qui ne pourrait pas
  distinguer porté et survolé. ⚠ Pas de position monde sur ce chemin (aucun nœud) → `ProjectAnchor`
  bascule sur la géométrie capturée quand `world == (0,0,0)`.
  🔴 **NE JAMAIS DÉRIVER L'ANCRE DE LA GÉOMÉTRIE CAPTURÉE** : elle est ANIMÉE → la bbox bouge à chaque
  frame → l'effet se recale en permanence ⇒ **rendu saccadé** (« une image sur 2/3 sautée ») **+ zoom**
  (l'échelle de repli restait à 1.0 faute d'autre effet projeté). FIX : quand la primitive n'a pas de
  position monde, prendre celle de l'**ACTEUR** (`g_owner_actor + 0x10` — un acteur dérive du même type
  de nœud qu'un effet, même offset). Ancre STABLE **et** vraie échelle de profondeur. Le bas-centre de
  la bbox n'est plus qu'un dernier recours (acteur indisponible).
  ⚠ Deux corrections sur ce repli (2026-07-19) : l'ancre est le **BAS-CENTRE** et non le centre (le
  natif ancre sur l'ORIGINE de l'acteur = ses pieds ; le centre décalait tout de la demi-hauteur de
  l'effet → « offset Y mauvais » signalé), et `screen_scale` réutilise **`g_last_screen_scale`** (la
  dernière échelle de profondeur valide vue) au lieu de `1.0f`, qui donnait un facteur de
  redimensionnement absurde côté doll.
  **DOLL** : `include_str_particle = with_preview || (g_ez_preview_active == 0)` — sans aperçu en
  cours, ces primitives ne peuvent venir que d'un effet PORTÉ, donc le doll les affiche ; pendant un
  survol l'origine est ambiguë et il s'abstient (conséquence assumée : un effet porté de cette
  famille disparaît du doll pendant un survol). Même règle dans `EzPrimCountForDoll`.
  ✅ **SUPPRESSION IN-WORLD RÉSOLUE pour cette famille** : `kStrParticleId (-2)` est traité comme une
  valeur SUPPRIMABLE (marqueur de FAMILLE, pas id manquant) — `IsSuppressed` l'accepte, et le hook
  teste l'id EFFECTIF (`from_str_particle ? kStrParticleId : g_cur_effect_id`). `ReconcileEzPreview`
  ajoute `kStrParticleId` aux ids masqués **tant qu'un aperçu est en cours**.
  ⚠ Conséquence assumée : pendant un survol, un costume PORTÉ de cette même famille disparaît lui
  aussi du personnage (suppression globale de famille, faute d'identité individuelle).
  🔒 **ÉQUIPÉ = VALIDÉ PARFAIT PAR L'UTILISATEUR — NE PLUS Y TOUCHER.**
  ✅✅ **TOUT VALIDÉ (2026-07-19)** : ordinal 277 s'affiche en aperçu, sur le doll une fois équipé, et
  ne parasite plus le sprite du joueur. La 3ᵉ famille est donc entièrement intégrée.

- 🔴 (RÉFUTÉ — conservé pour mémoire) **théorie « collision d'ids »** — ord 277 :
  `Effect_ApplyHatEffectViaLua` **passe l'id CONCRET là où le gestionnaire attend l'id UNIFIÉ**.
  Chaîne mesurée : `Actor_ToggleEffectId` → `Effect_ApplyEffectIdToActor` 0x00c41ba0 avec
  `[esp+4] = 0xA9F` (**2719** = 277+0x98a) ✓ → branche hat → `Effect_ApplyHatEffectViaLua` 0x00c41ce0
  (`[esp+4]=0xA9F`, `[esp+8]=1`) → `call 0x00A9A7D0` (GetHatEffectID) → **`[ebp-0x14] = 0x098B` (2443)**
  → `call 0x00AC12E0` en 0x00c41e5c avec **`ecx=g_EffectMgr`, `[esp]=ownerHandle 0x1E8481`,
  `[esp+4] = 0x098B` (2443)**.
  ⚠ Or `EffectMgr_RegisterValidEffectId` enregistre les hat effects sous **`ordinal + 0x98a`**
  (0x00ac41e8) → la clé **2443 = ordinal 1**. ⇒ **le client affiche l'effet de l'ORDINAL 1**, pas
  celui du 277. Ça réconcilie tout : `GetHatEfResName(277)` vide MAIS quelque chose s'affiche
  (le `.str` de l'ordinal 1) ; rien de « 277 » à capturer ; collision seulement pour concret ≥ 2443
  (d'où ord 248/2429 qui marche, et 277 seul cas cassé du voteshop).
  💡 **Contournement possible** : appeler nous-mêmes `EffectMgr_SpawnEffect` (0x00AC12E0, __thiscall
  ecx=g_EffectMgr 0x015beeac) avec l'id **UNIFIÉ** `ordinal + 0x98a` → aperçu PLUS JUSTE que le jeu.
  ⚠ Reste à capturer : cette famille dessine via un hôte `CEZ2STRParticle` (effet+0xcc, créé par
  `CEZ2STREffect_CreateHostSprite` 0x00b1b3b0), PAS via `EffectInstance_RenderDraw` 0x00ae8480 — d'où
  « ids CEffectMgr : aucun ».
  🛠 **Méthode x32dbg** : ⚠ la journalisation seule (`SetBreakpointCondition 0` +
  `SetBreakpointLogCondition 1`) **N'ÉCRIT RIEN** ici — il faut des points d'arrêt BLOQUANTS et lire
  les registres via MCP (`get_register`, `read_memory` sur esp/ebp), puis `run`.

- 🟠 (historique) **ord 277, MESURE CLÉ** : `effect_id capturés : **164×2**` (= l'AURA du perso), et
  **aucun rejet** de forme. Donc notre effet **2443 n'est JAMAIS dessiné** par les chemins hookés
  (`1/1 dessins` = le seul nœud de l'aura ; famille CEffectMgr vide) — alors qu'il REND en jeu.
  ⇒ **il emprunte un TROISIÈME chemin non capturé**. Suspect n°1 : notre hook CEffectMgr **exclut
  volontairement** les instances `CEZ2STREffect` (vtbl 0x010758d8, famille `.str`) → on l'écarterait
  nous-mêmes. Ajouté `SetCaptureStrEffects(bool)` + case « Capturer aussi les .str (diag) » pour le
  vérifier. ⚠ Ce cas est aussi celui où `GetHatEfResName(277)` est VIDE (catalogue ne le marque pas
  `[.str]`) alors que l'effet serait de cette famille : incohérence à élucider.
- (historique) **ord 277 (HAT_EF_C_2025RosFesta, id concret 2443)** : l'id se résout TRÈS BIEN
  (`-> id concret 2443 (natif)`, Lua OK — hypothèse « Lua échoue » RÉFUTÉE). Sonde : `1/1 dessins /
  2 inserts / 0 capturés` + **« ids CEffectMgr : aucun »** ⇒ le jeu ÉMET et c'est **NOTRE FILTRE DE
  FORME** qui jette (`shape_ok` : seuls type 4/3 sommets et type 5/4 sommets, non indexé, sont
  acceptés). Sonde enrichie : `Stats.rej_type/rej_vtx/rej_idx/rej_count` + ligne UI
  « rejets: N (type=…, sommets=…, indices=…) » pour identifier la forme refusée. **Prochaine étape :
  lire ces valeurs en jeu** puis étendre la capture à cette forme.
- ✅✅ **VALIDÉ EN JEU** : aperçu EXCLUSIF — doll = costumes PORTÉS seulement, aperçu = le SURVOLÉ,
  personnage NET (plus de parasite). Toute la famille CEffectMgr est désormais traitée comme le
  chemin EZ.
- ✅🔴 **MESURÉ EN JEU (2026-07-19), FIN DES HYPOTHÈSES** : `*(int*)(instance+0x04)` **EST L'ID
  CONCRET**. Relevé : Permafrost Oblivion **ordinal 248 → 2429**, identique à `GetHatEffectID(248)` ;
  valeur présente équipé ET au survol, **absente** une fois déséquipé. Donc l'espace que la RE
  décrivait comme « effet générique, `< 0x98a` » **est exactement celui des ids concrets (491..2437)** —
  je l'avais mal interprété en cherchant la branche « hat effect » (`>= 0x98a`).
  ⇒ **FIX FINAL** : le module expose `g_cur_effect_id = em_id` quand `0 < em_id < 0x98a`, donc la
  famille CEffectMgr devient filtrable ET supprimable **exactement comme le chemin EZ**.
  `include_effmgr = false` partout ; `BuildEffMgrFilter`/`g_ez_effmgr_ids` supprimés (inutiles) ;
  `Prim::effmgr_id` conservé pour diagnostic. Ça règle du même coup le **parasite sur le sprite du
  joueur** pendant un aperçu (enfin supprimable nominativement).
- 🔴🔴 **2ᵉ HYPOTHÈSE RÉFUTÉE AUSSI** : `effmgr_id == ordinal + 0x98a` est **FAUX** — filtrer là-dessus
  faisait disparaître le costume **ÉQUIPÉ** du doll (essai en jeu). L'encodage réel de `+0x04` reste
  **À MESURER** : le lab affiche désormais « ids CEffectMgr capturés » (valeurs brutes). Repli actif =
  `include_effmgr = true` (tout ou rien), seul comportement vérifié fonctionnel.
  ⚠⚠ **Deux hypothèses successives sur ce champ, deux régressions.** Ne plus rien supposer : MESURER.
- **FIX VISÉ (à rebrancher une fois l'encodage connu)** : filtrage **PRÉCIS par id d'instance** au lieu de « tout ou rien ».
  `Prim::effmgr_id` = `*(int*)(inst+0x04)` ; `DrawOpts::effmgr_ids/effmgr_id_count` (si non vide,
  `include_effmgr` est ignoré). Côté basic_info : `g_ez_effmgr_ids[]` = `ordinal + 0x98a` des
  ÉQUIPÉS (rempli dans OnTick à côté de `g_ez_target_ids`) + `BuildEffMgrFilter(ids, with_preview)`
  qui ajoute le survolé. ⇒ doll = costumes PORTÉS ; aperçu = + le SURVOLÉ. Aucun appel Lua requis
  (l'ordinal suffit, contrairement à l'id concret).
- (analyse statique, à relire avec la réfutation ci-dessus) **UN HAT EFFECT `hatEffectID` NE CRÉE AUCUNE INSTANCE CEffectMgr** : `GetEffectType` type 1
  (ex. **Perm_Frost ord 248**) → `Effect_ApplyHatEffectViaLua` 0x00c41ce0 → `Effect_SpawnPrimitiveById`
  0x00c44540 → **nœud primitif EZ (+0x168)**. Seul le type 0/2 (`resourceFileName`) →
  `Effect_ApplyOrRemoveGenericEffect` 0x00c41ec0 → `CEZ2STREffect` (vtbl 0x010758d8) — **justement la
  vtable qu'on exclut**. ⇒ **`include_effmgr` ne faisait entrer que des effets de SKILL/STATUT**
  (empreintes, auras de compétence), sans rapport avec un costume et sans id exploitable.
  **FIX : `include_effmgr = false` partout dans basic_info** (doll + aperçu + `EzPrimCountForDoll`).
  Perm_Frost ÉQUIPÉ continue de s'afficher : il est capturé par le chemin EZ avec son id concret.

**LIMITES RESTANTES (connues, non bloquantes)** — famille CEffectMgr, `effect_id = -1` :
1. **Non supprimable nominativement** → un effet CEffectMgr en APERÇU reste visible sur le personnage
   (constaté sur Perm_Frost survolé). Pour le corriger il faudrait résoudre l'id sur ce chemin (RE).
2. **`include_effmgr` est un inclusif EN BLOC** : sans id, ces primitives échappent au filtre
   équipés/aperçu → un CEffectMgr seulement SURVOLÉ *devrait* fuir sur le doll. Observé : il ne fuit
   PAS (raison non élucidée). Incohérence latente : à traiter si elle se manifeste.

**✅ MIGRATION FAITE (2026-07-19)** : `spr_effect_lab.cc` ET le doll de `basic_info.cc` consomment le
module ; **les 3 hooks locaux de basic_info ont été RETIRÉS** (sinon double capture, et le doll serait
resté piloté par l'ancienne capture buguée). basic_info : -333/+84 lignes ; `EzCapTri`, `g_ez_caps`,
`EzOwnedRoot`, `conv()` supprimés ; `InstallEzCapture()` = `EnsureInstalled()` + `SetCaptureEffectMgr(true)` ;
`OnTick` appelle `SetOwnerActor(g_ez_owner_actor)` ; `DrawEzCapTris` garde signature/FIT/`R=(s/S)*cal`
mais itère `Prims()/Count()` avec `k < p.n` et délègue tout le dessin à `Draw`
(`use_zorder=true`, `draw_behind=before`, `ids=g_ez_target_ids`, `include_effmgr=true`).
⚠ **Le doll n'appelle JAMAIS `SetSuppressedIds`** : sa capture d'origine chaînait toujours → l'effet
DOIT rester visible sur le perso. Si l'effet disparaît du perso quand la fiche est ouverte = régression.
`kCapMax` **2048 → 4096** (la capture n'est plus filtrée : elle contient tous les effets du joueur).
Exclusion des `.str` name-based (vtbl `kCEZ2STRVtbl`) VÉRIFIÉE présente dans le hook CEffectMgr du
module → pas de double dessin avec `DrawStrCapLayers`. Code mort retiré du lab
(`g_no_capture_frames`/`kReviveFrames`, vestiges de la branche « revive » → bug d'anim accélérée).
⚠ L'agent a aussi supprimé de basic_info des constantes de RE DORMANTES (kEzSubmitQuad, kEzRootVtbl,
kEzActorFxListHead/Cnt, kEzListNodeNext/Data…) : si ces notes manquent un jour, elles sont dans Ghidra
et dans [[reference_render_queue_2d]].

**🔴🔴 RÈGLE D'ARCHITECTURE : AUCUN RÉGLAGE DE CAPTURE PARTAGÉ** (2 bugs payés, 2026-07-18/19).
Un module partagé à plusieurs consommateurs ne doit exposer AUCUN état de capture global — chacun est
un état à plusieurs écrivains :
 1. **ciblage global** (`SetTargetIds`) : `count==0` = « tout accepter » + suppression ⇒ **icônes de
    statut disparues** quand le lab était éteint. → supprimé ; `Prim::effect_id` + filtre au DESSIN
    (`DrawOpts::ids`), suppression **nominative** (`SetSuppressedIds`).
 2. **acteur courant** (`SetOwnerActor`) : le doll l'armait dans OnTick (100 ms), le lab le remettait à
    `nullptr` à CHAQUE frame ⇒ **le doll ne captait qu'1-2 frames puis plus rien** (symptôme exact
    signalé). → **supprimé** : le module RÉSOUT lui-même l'acteur joueur (`RefreshOwnerActor()` à
    chaque frontière de frame, GameMode+0xcc → +0x2c). `SetCaptureEffectMgr` supprimé aussi (capture
    CEffectMgr désormais systématique ; opt-in au dessin via `include_effmgr`).
Même `SetSuppressedIds` est devenu **multi-consommateurs** : `SetSuppressedIds(slot, ids, count)` avec
un emplacement par client (`kSlotLab`/`kSlotDoll`/`kSlotSheet`), la suppression effective étant l'**UNION**
des emplacements — sinon le doll et le lab s'écraseraient (même défaut, 3ᵉ fois).
**APERÇU SUR LE DOLL SEULEMENT (2026-07-19)** : `ReconcileEzPreview` résout l'id concret de l'effet
d'aperçu (`HatLuaNum(ord,"GetHatEffectID")`, déclaration anticipée nécessaire : défini plus bas) et le
supprime via `kSlotDoll` → l'aperçu ne s'affiche QUE sur le doll. ⚠ Ne JAMAIS y mettre les hat effects
**ÉQUIPÉS** : légitimement portés, ils doivent rester visibles en jeu. ⚠ Un aperçu de la famille
**CEffectMgr** (effect_id = -1) n'est PAS supprimable par id → restera visible sur le perso.
⚠ **`g_ez_preview_cid` sert DEUX fois** : supprimer l'aperçu du monde ET **l'AUTORISER au dessin**.
`g_ez_target_ids` ne liste que les effets **ÉQUIPÉS** (`own_hat_effects_`) → sans ajouter l'id d'aperçu
au filtre `DrawOpts::ids`, un aperçu d'effet NON équipé est spawné et capturé mais **rejeté au dessin**
= invisible en cash-shop/voteshop et dans la description d'item (symptôme signalé sur ord 23
HAT_EF_DOUBLEGUMGANG id 418), alors que la fiche perso marche (effet équipé, donc dans la liste).
⚠ **LE FIT EST LA PROPRIÉTÉ DU DOLL SEUL** : `g_ez_frozen_valid`/`g_ez_fz_*` ne sont LUS que par
`RenderPlayerAvatar` (cadrage du doll) mais étaient ÉCRITS par les deux surfaces — le tooltip d'aperçu
figeait donc une bbox incluant l'effet SURVOLÉ et redimensionnait le cadre du doll. FIX : la bbox n'est
calculée que si `!with_preview`. Idem la décision d'appliquer le FIT : `EzPrimCountForDoll()` (compte
FILTRÉ) et non `ez_capture::Count()` (qui inclut l'aperçu). Filtre factorisé dans `BuildEzFilter(ids,
with_preview)` pour que dessin / mesure / décision partagent la MÊME définition.
⚠ **MESURER = FILTRER PAREIL QUE DESSINER** : la bbox du FIT (qui dimensionne le cadre du doll) doit
appliquer `ez_capture::Matches(prim, opts)`, sinon un effet capturé mais NON dessiné (l'aperçu survolé
ailleurs) élargit la bbox et **fait rétrécir le cadre du doll** pour faire tenir ce qui n'y est pas
affiché (constaté 2026-07-19). Règle générale : dessin, ancre (`ProjectAnchor`) et mesure doivent voir
le MÊME sous-ensemble — d'où `Matches` exporté.
⚠ **L'id d'aperçu ne doit être dans le filtre QUE des surfaces d'aperçu** : `DrawEzCapTris(..., bool
with_preview)` — **true** depuis `RenderItemPreviewTooltip` (voteshop / description d'item), **false**
depuis `RenderPlayerAvatar` (doll de la fiche). Sinon l'effet survolé ailleurs s'affiche AUSSI sur le
doll, qui doit montrer ce que le perso PORTE (constaté 2026-07-19). ✅ Aperçu voteshop + description
d'item VALIDÉS fonctionnels.
⚠ `RenderItemPreviewTooltip` ne spawne PAS l'aperçu si l'effet est **déjà équipé** (`alreadyEquipped`) :
sinon le toggle-remove retirerait l'instance équipée (script « overruled » jusqu'à @refresh).
`kCapMax` 2048 → 4096.

**PORTRAIT (2026-07-19)** : le portrait ImGui (`DrawPortraitElem`, branche `portrait_head_sprite_`)
affiche AUSSI les hat effects, exactement comme le doll — 2 appels `DrawEzCapTris(dl, ox, oy, s,
before, /*with_preview=*/false)` encadrant la boucle des couches, **à l'intérieur du PushClipRect**.
⚠ Ça marche parce que le portrait utilise la MÊME convention que le doll : `ox`/`oy` = origine de
l'acteur en écran, `s` = échelle. Rien d'autre à adapter.

**RESTE À FAIRE** : (fiche perso = quasi gratuit désormais) ; ancienne note ci-dessous — migrer le doll de `basic_info.cc` (`DrawEzCapTris`, `EzCapTri`, `EzOwnedRoot`,
`Hooked_RenderInsert`, `Hooked_EzDraw`, `Hooked_EffRender`, `InstallEzCapture`) sur le module en
**RETIRANT ses hooks** (sinon double capture, et le doll resterait piloté par l'ancienne capture qui a
les 3 bugs), puis la fiche perso. Le doll fournit `scale = (s/screen_scale)*cal`, `use_zorder=true`
(2 passes autour du sprite) et garde son FIT (bbox figée) calculé depuis `Prims()`.

**HORS SUJET — z-order des chapeaux/costumes NATIFS** : dépend de l'**ordre d'ÉQUIPEMENT**, et
**PRÉ-EXISTE au lab** (vérifié par l'utilisateur contre la DLL de production, 2026-07-19). RIEN à voir
avec `ez_capture` : nos hooks n'ont aucune notion de l'ordre d'équipement. Ne pas ré-instruire ici ;
relève du chantier « ordre des couches d'équipement », cf. [[project_weapon_zorder]] /
[[project_equip_window_re]]. (Signature analogue à [[project_solved_archive]], qui était
côté serveur.) L'interrupteur « Couper le hook CEffectMgr (diag) » du lab reste disponible pour
bissecter une éventuelle régression FUTURE du rendu natif.

**🔴🔴 `Scene_ProjectWorldToScreen` 0x005541b0 ÉCRIT DANS SON VECTEUR MONDE (in place)** — bug réel
2026-07-19, très coûteux à diagnostiquer. Il FAUT lui passer une **COPIE** de la position monde :
```
float world[3] = { g_world[0], g_world[1], g_world[2] };
kSceneProject(queue, nullptr, world, view, &x, &y, &invW);
```
C'est un vrai bug (corrigé), mais ⚠ **ce n'était PAS la cause** de l'ancre aberrante ci-dessous.

**🔴🔴 CAUSE RÉELLE : L'ANCRE DOIT ÊTRE PAR PRIMITIVE, PAS GLOBALE** (2026-07-19) — ✅ FIX VALIDÉ
(overlay du lab de nouveau fonctionnel après correction).
`g_world` était renseigné **une seule fois par frame, par la PREMIÈRE primitive capturée**. Tant que le
lab ne capturait que son effet, c'était forcément le bon. Depuis que la capture est LARGE (tous les
effets du joueur), c'est un **effet arbitraire** qui gagne la course et impose son ancre à tout le
monde → ancre sans rapport (constaté **(4800,2354)** sur un écran **1760×990**, donc très hors écran)
→ `out = dest + (v - ancre)` envoie tout dans le décor → **rien ne s'affiche NULLE PART** alors que la
capture est PARFAITE. Symptôme signature : **visible 1 frame au clic** (l'effet vient de naître et passe
premier), puis plus rien.
**FIX** : `Prim::world[3]` porté PAR PRIMITIVE (relu à chaque insertion) + `ProjectAnchor(const DrawOpts&,
…)` qui prend le **MÊME filtre** que `Draw` et projette la 1ʳᵉ primitive **qui passe ce filtre**.
Les appelants construisent donc leurs `DrawOpts` AVANT de demander l'ancre.
**Diagnostic gagnant** : « Debug capture » traçait les contours verts EXACTEMENT sur l'effet (capture,
effect_id et filtre tous OK) pendant que la ligne `ancre (…)` affichait une valeur hors écran → le
défaut était forcément entre l'ancre et le dessin. **TOUJOURS afficher l'ancre dans l'UI.**
⚠ Piège de raisonnement à éviter : j'ai cru prouver l'anomalie par « l'ancre ne bouge pas quand le perso
traverse l'écran » — c'était la FENÊTRE du jeu qui avait été déplacée, pas le personnage. Seule la
comparaison ancre vs **résolution réelle** était probante. Ne jamais supposer la résolution (elle varie
d'un joueur à l'autre) : la demander ou la lire.

**LIMITES connues** : (1) famille **CEffectMgr** (aura/statut, ex. Perm_Frost ord 248) : **RENDUE** dans
le lab (validé utilisateur) — son enfant EZ passe bien par `EzEffect_Draw`. (2) tester
un effet qu'on a AUSSI équipé nativement (même id concret sur le même acteur) = match ambigu (le toggle
est un no-op car déjà dans le set +0x3fc ; « Arrêter » retirerait l'effet natif) → edge de lab, à
documenter. Une fois le centre validé, « sur le doll » = remplacer l'ancre centre par le rect du doll
(R=s/S au lieu de 1).
