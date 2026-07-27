# Sprite & rendu — reverse engineering (Ghidra)

> Cartographie du sous-système sprite du client (Moonlight-Destiny.exe / RO).
> Objectif : décortiquer toute fonction/data concernant les sprites et leur rendu,
> renommer + commenter dans le projet Ghidra (dépôt durable), sans supposition.
> Chaque entrée = adresse + rôle **vérifié** par décompilation.

Statut : **en cours**. Les fonctions déjà nommées par les sessions précédentes ne sont
re-documentées ici que lorsqu'un helper `FUN_`/`DAT_` a été décortiqué.

---

## 0. Vue d'ensemble du pipeline

Deux chemins de rendu d'un acteur :

1. **Monde (in-world)** : `CActorSprite_RenderDispatch` (0x00d32100) → `CActorSprite_RenderLayered`
   (0x00603f00) → `CActorSprite_BuildPartQuads` (0x00605c30) → couche par couche.
2. **Paperdoll / UI** (fiche perso, aperçu) : `Actor_DrawSprites` (0x007ac820, param_1==1 = quad path)
   → `Actor_SubmitSpriteQuad` (0x00a1b7c0).

Les deux finissent par empiler des quads texturés dans la **scene render-queue**
(`g_SceneRenderQueue`) via `SceneRenderQueue_AcquirePrimRecord`.

---

## 1. Cœur du rendu acteur

### Actor_DrawSprites — 0x007ac820
Dessine toutes les couches d'un acteur (body/head/headgear/weapon/shield/garment).
`param_1==1` = chemin quad (paperdoll/UI). Voir commentaires en place. Ordonne les couches
via `Actor_BuildLayerDrawOrder` + `Actor_BuildSpriteLayers`, résout chaque couche par
`Actor_GetLayerDescriptor`, l'ancre par `Actor_ComputeHeadAttach`, et soumet par
`Actor_SubmitSpriteQuad`. Échelle = `Actor_GetJobSpriteScale(job)`.

Helpers décortiqués (cette session) :

| Adresse | Nom | Rôle vérifié |
|---|---|---|
| 0x007adb50 | `Actor_SelectFrameLayerIndex` | Renvoie l'index de frame-layer (0/1) d'une part : true si `Job_IsHelmRobeClass(+0x14)` ou part==tête(2). Alimente `Act_GetFrameLayer`. |
| 0x00d9cf80 | `Job_NeedsLuaItemPosOffset` | =1 pour jobs Doram/summoner {0x1079-0x107d,0x10d4,0x10db} → déclenche le Lua `OffsetItemPos_GetOffsetForDoram`. |
| 0x00d84760 | `GameState_ReadDirtyCachedField5538` | Lecture dirty-cache du champ +0x5538 du singleton d'état (0x015fa3c0), miroir +0xe78. Gate le path Doram + offset -4px tête novice(job 200). |
| 0x00d83a40 | `Act_ResolveAltAnimFrame` | Résout une frame d'anim alternative haute-résolution : si l'act alt a un multiple de frames du body, calcule un index temporel (`timeGetTime`) et renvoie `Act_GetFrame(alt)`. |
| 0x00d389c0 | `Actor_ComputeBaseClassHeadYOffset` | Décalage Y alignant la tête (classe promue) sur l'ancre tête du corps **classe de base** (charge palette base, compare ancres). |
| 0x00d38f90 | `Actor_QueryLuaHeadLayerAdjust` | Gate Lua (classe promue + dir {0,8,0x10,0x40}, tables `IsIgnoredRidingState`/`TB_Layer_Priority`) décidant si on applique le décalage ci-dessus. |

### Actor_SubmitSpriteQuad — 0x00a1b7c0
Construit un quad texturé (4 sommets) pour une couche `.act` et l'empile dans
`g_SceneRenderQueue`. Applique offX/offY (layer[0]/[1]), scaleX/scaleY (layer[5]/[6],
floats bit-reinterprétés), mirror (flags bit0), et rotation optionnelle (angle → sin/cos).
Déjà largement commenté (sondes cart-vs-body).

Helpers décortiqués :

| Adresse | Nom | Rôle vérifié |
|---|---|---|
| 0x0053add0 | `SceneRenderQueue_AcquirePrimRecord` | Alloue/retourne un `RenderPrimRecord` (0xA8 o) dans le ring-buffer de la render-queue (+0x70..+0x8c). |
| 0x0054b240 | `Math_SinDeg_LUT` | Sinus par table (degrés, wrap 360). Tables `g_SinTableDegPos`/`Neg`. |
| 0x0054b2e0 | `Math_CosDeg_LUT` | Cosinus par table (degrés). Tables `g_CosTableDegPos`/`Neg`. |

---

### CActorSprite_RenderDispatch — 0x00d32100  (vtbl +0x08, entrée rendu/frame)
Branche selon l'état : cloak/invisible/hide → rien ; `this+0xa0/0xa1==0` → rendu simple ;
id spécial → billboard ; sinon rendu layered via contexte transitoire.

| Adresse | Nom | Rôle vérifié |
|---|---|---|
| 0x00c5a010 | `CActorSprite_RenderSimpleProjected` | Rendu minimal : projette pos monde (+0x10) → écran, appelle vtbl+0x18(x,y). |
| 0x00d3b560 | `CActorSprite_RenderLayeredTransient` | Construit un contexte layered transitoire (vtable 0x010945cc, 3 sous-objets) et lance `CActorSprite_RenderLayered`. |
| 0x00d3b170 | `CActorSprite_RenderWeaponLayerBillboard` | Branche spéciale (ids hardcodés) : billboard des couches d'arme (SPR+0x4ac/ACT+0x4b8) avec échelle profondeur. |
| 0x00a9ecf0 | `IsSpecialActorId_Svc0` | =1 si `g_ServiceType==0` ET id∈{0x2c8c7d…0x2c8c82}. (Anciennement supposé « vending » → FAUX, corrigé.) |
| 0x00553ea0 | `SceneRenderQueue_GetGlobalScale` | Getter `*(queue+0x18)` = scale global caméra/zoom. |
| 0x00c59fb0 | `Actor_ExpandScreenAABB` | Étend la bbox écran {minY,minX,maxY,maxX} avec un bord de quad (pick souris). |

### CActorSprite_RenderLayered — 0x00603f00  (rendu in-world par couches)
Phase 1 (build parts 8..0) → `CActorSprite_BuildPartQuads` → insertion triée. Phase 2 (flush) :
parcours ascendant de l'arbre RB `this+0x9c` (arrière→avant). Voir commentaires en place.

| Adresse | Nom | Rôle vérifié |
|---|---|---|
| 0x00c4a670 | `Actor_SubmitQuad_VerticalFlip` | Submit quad flip vertical (rot 180° + réflexion) si `Scene_IsVerticalFlipMode`. |
| 0x00c582d0 | `CActorSprite_StoreRenderScreenParams` | Setter POD 5 champs (+0x84 : viewportW/H, 0,0, invW). |
| 0x00c57a10 | `CActorSprite_ApplyStatusScalePulse` | Pulsation d'échelle (grow/shrink) selon bits +0xe8 0x10/0x400, LUT cos. |
| 0x00d38f20 | `Actor_IsInSpecialOptionState` | Vrai si un des 5 prédicats Option (+0x2c0) ou flag +0x3dc==1. |
| 0x00d39370 | `Action_IsAttackFlatRange` | Vrai si index action à plat ∈ [0x50,0x60) (action 10-11). |
| 0x00605a00 | `Actor_ResolvePart6FrameResName` | Nom de ressource attachée frame (part 6), filtre `.wav`. |
| 0x0070f420 | `Act_GetSoundName` | Nom son/événement `.act` (vecteur act+0x120, stride 0x18). |
| 0x0059cdf0 | `Cstr_snprintf_s` | Wrapper `vsnprintf_s`. |

### CActorSprite_BuildPartQuads — 0x00605c30  (géométrie quads d'une part)

| Adresse | Nom | Rôle vérifié |
|---|---|---|
| 0x00d38340 | `Part_FrameLayerIndex` | Index frame-layer de base (1 pour tête, sinon 0). |
| 0x00605170 | `CActorSprite_ComputePartAttachOffset` | Offset d'ancrage d'une part (chaîne corps→tête→couvre-chef), écrit {sprNo,layer,offY,offX}. |
| 0x00c57af0 | `CActorSprite_ApplyStatusSpinOffset` | Offset spin/wobble de statut (bit +0xe8 0x40). |
| 0x00c57940 | `CActorSprite_SubmitStatusOverlayQuads` | Overlays statut (blink 0x100, teintes 0x4000/0x40000). |
| 0x00c57e40 | `CActorSprite_SubmitTimedTintQuad` | Quad teinte à fondu temporisé (alpha+0xf6, R/G/B), empilé 2×. |

### CActorSprite_DeferQuadSorted — 0x006046e0  (insertion triée z-order)
Calcule une clé et insère dans l'arbre RB `this+0x9c`. Deux branches (map priorité +0x7c).
Cause « arme derrière couvre-chef » documentée en place.

| Adresse | Nom | Rôle vérifié |
|---|---|---|
| 0x006039f0 | `RBTree_FindLowerBound` | Recherche du point d'insertion std::map (clé = node[4]). |

### Actor_ApplyLayerEffects — 0x00d37790  (effets visuels de statut)
Applique tous les modificateurs visuels de statut (bits +0xe8) autour du submit de couche.

| Adresse | Nom | Rôle vérifié |
|---|---|---|
| 0x00c577f0 | `CActorSprite_ComputeHoverYOffset` | Offset Y lévitation/flottement (phase+0xf8, ampl+0xfa, cos×scale). |
| 0x0136e6c8 | `g_StatusEffectList_begin` | Début vecteur effets de statut (stride 0x14). |
| 0x0136e6cc | `g_StatusEffectList_end` | Fin du vecteur. |

### Utilitaires trig/rendu communs

| Adresse | Nom | Rôle |
|---|---|---|
| 0x0054b240 | `Math_SinDeg_LUT` | Sin par table degrés. |
| 0x0054b2e0 | `Math_CosDeg_LUT` | Cos par table degrés. |
| 0x0053add0 | `SceneRenderQueue_AcquirePrimRecord` | Alloue un RenderPrimRecord (0xA8) dans le ring-buffer. |
| 0x004e52a0 | `std_string_copy_ctor` | Copy-ctor std::string (récurrent). |

---

## 2. Format Act/Spr + atlas + texture (fondation du rendu)

### Format `.act` (déjà exhaustivement commenté in-place)
- `Act_GetActionFrames` 0x0070f??0 — actions vector `act+0x110` (stride 0xc {begin,end,cap}), index=animType*8+dir.
- `Act_GetFrame` — frame 0x44 o (layers +0x20/+0x24, ancres +0x34, count ancres +0x40).
- `Act_GetFrameLayer` — ActLayer 0x24 o (x,y,sprNo,flags mirror bit0, color +0x10, scaleX/Y +0x14/+0x18, angle +0x1c, sprType +0x20).
- `Act_GetFrameCount` = (end-begin)/0x44 min 1. `Act_GetSoundName` = vecteur `act+0x120` stride 0x18.

### Atlas de cellules sprite (cache LRU (cell,palette) → CTexture)

| Adresse | Nom | Rôle vérifié |
|---|---|---|
| 0x00566b70 | `SpriteAtlas_GetCachedTexture` | Lookup cache : renvoie CTexture + UV si (cell,palette) présent, sinon 0. |
| 0x005663d0 | `SpriteAtlas_BuildTexture` | Miss : bin par w/h → page, alloue cellule, upload pixels (vtbl+0x34), remplit géom/UV. |
| 0x005667f0 | `SpriteAtlas_AllocPage` | Nouvelle page (CTexture pool/factory) découpée en grille de cellules 0x28 (UV pré-calc). |
| 0x00565a20 | `SpriteAtlasKey_FnvHash` | Hash FNV-1a de la clé 8 o (cell+palette). |
| 0x00565c20 | `SpriteAtlasCache_Find` | Find hashmap (bucket = hash & mask). |
| 0x00565ef0 | `SpriteAtlasCache_Insert` | Insert + rehash. |
| 0x00567650 | `SpriteAtlasCache_EraseByKey` | Erase (éviction LRU). |
| 0x00567040 | `SpriteAtlasCache_Rehash` | Rehash buckets. |
| 0x00565d10 | `SpriteAtlasCells_Resize` | Resize du vecteur de cellules. |

### Décodage pixel + upload GPU

| Adresse | Nom | Rôle vérifié |
|---|---|---|
| 0x00566ca0 | `SpriteFrame_BlitToLockedRect` | Décode RLE indexed-8 (index 0 = run transparent) → 16-bit via palette, écrit dans locked-rect. |
| 0x0056a7a0 | `CTexture_UploadSprite` | vtbl+0x34 : Lock surface D3D9 (+0x130), memset zone, blit, Unlock, D3DXFilterTexture mips. CTexture : +0x12c IDirect3DTexture9*, +0x130 surface, +0x134 D3DFORMAT (0x15=A8R8G8B8, 0x19=16bpp). |
| 0x0053c1b0 | `CTexture_DX7_UploadSprite` | Variante DX7. |

### Cache de ressources sprite (arbre par nom, refcount)

| Adresse | Nom | Rôle vérifié |
|---|---|---|
| 0x00568760 | `SpriteRes_GetOrLoadByName` | Get-or-load d'une ressource sprite indexée par nom (arbre RB). |
| 0x005688f0 | `SpriteRes_LoadFromFile` | Loader : UITextureMgr → `SpriteRes_CreateFromPixels`, sinon sprite défaut. |
| 0x005685a0 | `SpriteRes_CreateFromPixels` | Crée sprite vide + init texture depuis pixels (vtbl+0x20). |
| 0x00560f00 | `SpriteTexFactory_NewEmptySprite` | Nouvel objet sprite vide. |
| 0x00568620 | `SpriteTexFactory_NewTexture` | = GetSingleton + NewEmptySprite. |
| 0x00561240 | `SpriteTexFactory_GetDefaultSprite` | Sprite par défaut du singleton. |
| 0x00569230 | `SpriteResCache_FindByName` | Find arbre par nom. |
| 0x005680d0 | `SpriteResCache_TreeLowerBound` | Lower-bound arbre. |
| 0x00569440 | `SpriteRes_SetName` | Init champ nom (res+9). |
| 0x005694b0 | `SpriteRes_AddRef` | Incrément refcount. |
| 0x00a8f4b0 | `UITextureMgr_Release` | Libère une texture UI chargée. |

---

## 3. Constructeurs de chemins sprite (`.spr`/`.act`)

Tous partagent le même jeu de helpers : `Sprite_GetGenderToken` (token M/F), les tables de noms de
classe, `Race_GetBodyPrefix6` (préfixe dossier 6 car.), `Job_ResolveBodyClass`, `std_string_concat`.

| Adresse | Nom | Rôle vérifié |
|---|---|---|
| 0x00b433b0 | `Job_BuildBodyOrHeadSpritePath_impl` | Corps/tête ; tables `g_JobName_Doram_Male/Female`, `g_JobName_Male/Female`. |
| 0x00b456c0 | `Job_BuildWeaponSpritePath` | Arme `\<job>_<sex>[<wclass>].<ext>`, table `g_JobBodyClassName`. |
| 0x00b41d90 | `Job_BuildHeadgearSpritePath_impl` | Couvre-chef via `Job_GetHeadgearResName`. |
| 0x00b44b80 | `Robe_ResolveSpriteFilename` | Cape/robe (`Robe_ResolveBaseId`/`Robe_HasJobSpecificVariant`/`Robe_GetResName`). |
| 0x00d815f0 | `Sprite_GetGenderToken` | Token de genre (table cfg+0x5280[sex]). |
| 0x00d81480 | `Job_GetHeadgearResName` | Nom ressource couvre-chef par view id. |
| 0x00d60de0 | `Job_GetWeaponResNameByView` | Nom ressource arme par view id. |
| 0x0061c1f0 | `std_string_concat` | operator+ std::string. |
| 0x00a94930 | `std_string_from_cstr` | std::string depuis littéral. |
| Data | `g_JobName_*` / `g_JobBodyClassName` | Tables job→nom de classe (M/F, doram, corps). |

## 4. Constructeurs de slots d'équipement (`CActorSprite_Build*`)

Layout de slot vérifié : **SPR** = tableau `this+0x4b8[slot*4]`, **ACT** = tableau `this+0x4ac[slot*4]`.
Slots : 0=corps, 1=tête, 2=cheveux, 3-4=couvre-chefs, 5=arme, 6=bouclier/traînée, 7=cape, 8=robe.
Ancre arme = `this+0x4dc`(x)/`+0x4e0`(y). View ids : tête `+0x438`, arme `+0x440`, bouclier `+0x444`,
sexe `+0x260`, job `+0x4c8`/`+0x4cc`.

| Adresse | Nom | Rôle vérifié |
|---|---|---|
| 0x00d3d7c0 | `CActorSprite_SetSlotSprite` | Charge SPR → slot ; calcule l'ancre arme (diff frame-0 arme vs corps base-class). |
| 0x00d403a0 | `CActorSprite_BuildWeaponLayers` | Arme(slot5)+bouclier(slot6) ; hook WeaponDualSprites. |
| 0x00d3f4f0 | `CActorSprite_BuildHead_Slot1` | Tête (SPR slot1 + ACT slot1). |
| 0x00d409fc | `Weapon_ResolveTypeBucket` | item-id → bucket arme/bouclier (0x19-0x1e). |
| 0x00d42790 | `CActorSprite_ResolveShieldBucket` | Remap classe arme → bucket bouclier selon job. |
| 0x00a8f910 | `UITexture_Release` | Décrémente refcount texture UI. |
| 0x00573cb0 | `std_string_replace` | Remplace sous-chaîne (ex: MADOGEAR1→2). |
| 0x00a72830 | `IsGidInActorFilterList` | gid ∈ `g_ActorGidFilterList` → traînée d'arme non chargée. |

### Helpers de couches (cœur, vérifiés)

| Adresse | Nom | Rôle vérifié |
|---|---|---|
| 0x00605700 | `CActorSprite_GetPartSprite` | Lookup part dans map `this+0x74` (entrée +0x18=SPR, +0x1c=ACT). |
| 0x00603a40 | `CActorSprite_PartMap_Find` | Find std::map de la map de parts. |
| 0x00d38850 | `Actor_SelectPartLayerByDir` | Sélecteur de couche/skip par facing (flip avant/arrière). |
| 0x007188e0 | `Actor_FindPartInDrawOrder` | Index d'une part dans l'ordre de dessin (ou -1). |

---

## 5. Effets sprite (script + nœuds + draw)

Famille cohérente, vérifiée par représentants (structures de nœud d'effet mappées) :
- **Script** : `Effect_Init*` (table 0xc03xxx-0xc08xxx) — init des nœuds d'effet, cf. `project_effect_script_system_re.md`.
- **Update** : `EffectNode_Update*` — handlers par sous-type (dispatch node+0x1d0). Ex vérifié :
  `EffectNode_UpdateAnimatedSpriteBillboard` (billboard face-caméra, anim frame every 6/8, alpha clampé,
  angles recalés sur caméra +0x3ec/+0x4a0, pos owner +0x140).
- **Draw** : `EzEffect_DrawSub_*` — un sprite `.act` billboard par nœud attaché ; projette sol
  (`Terrain_GetHeightAt`) → écran, boucle couches → `Actor_SubmitQuad_RenderQueue`.
- **ChildSprite** : sprite-enfant (cart/faucon/warg/pet). `ChildSprite_DrawIfActive` → ombre +
  `Actor_DrawSingleActLayers`.

| Adresse | Nom | Rôle vérifié |
|---|---|---|
| 0x00c48820 | `Actor_DrawSingleActLayers` | Rendu d'un CActorSprite simple/enfant (ACT+0x108, SPR+0x104, pos monde +0x10). |
| 0x00c42450 | `Actor_ApplyChildLayerEffects` | Effets/overlays de statut pour le rendu enfant. |
| 0x00c41520 | `CActorSprite_SubmitFrostGhostQuad` | Fantôme gel/frost (bit 8). |
| 0x007110c0 | `Terrain_GetHeightAt` | Hauteur terrain (Y) sous (X,Z). |
| 0x00c44180 | `EffectNode_GetSpr` | SPR de l'effet (node+0x104). |
| 0x00a7d8e0 | `EffectScaleOverride_Contains` | Test map override d'échelle (`g_EffectScaleOverrideMap`). |
| 0x00a7d890 | `EffectScaleOverride_Get` | Valeur d'override d'échelle. |

## 6. Divers (curseur, cash-emotion, aura, nameplate)

| Adresse | Nom | Rôle vérifié |
|---|---|---|
| 0x00a74410 | `CursorMgr_RenderSprite` | Curseur RO (cursors.spr/.act) en quad batché (`RenderQueue_InsertPrimitive` 0x205). |
| 0x0053c2f0 | `SceneRenderQueue_GrowRing` | Grossit le ring-buffer de primitives. |
| 0x00c6e310 | `Actor_SpawnCashEmotionSprite` | Emote payante : lookup table → ChildSprite. |
| 0x00591400 | `CashEmotion_FindEntry` | Lookup entrée (`g_CashEmotionTable`, +0x28=spr, +0x2c=act). |
| 0x00592340 | `CashEmotion_GetSprName` | Nom `.spr` de l'emote. |
| 0x00590fc0 | `CashEmotion_GetActName` | Nom `.act` de l'emote. |
| 0x00d369d0 | `CActorSprite_DrawBodyAuraLayer` | Rendu couche corps/aura (acteur simple). |
| 0x00c430f0 | `Aura_ResolveActionForState` | Action `.act` d'aura selon état (mort=3). |
| 0x00c57f20 | `CActorSprite_UpdateAuraLayerBound` | Borne d'index de couche d'aura (+0x3c8). |
| 0x00c588b0 | `CActorSprite_SubmitNameplateQuad` | Plaque nom/emblème au-dessus de l'acteur. |
| 0x00a79610 | `NameplateQueue_Insert` | Insertion du quad nameplate dans sa file. |

---

## Couverture & limites

- **Cœur / Act-Spr / atlas / path / équipement / divers** : décortiqués en profondeur, tous les
  `FUN_`/`DAT_` sprite-spécifiques rencontrés ont été nommés + commentés dans Ghidra.
- **Effets** : la famille `Effect_Init*` (des centaines d'entrées, table 0xc03xxx-0xc08xxx) et les
  variantes `EffectNode_Update*` / `EzEffect_DrawSub_*` partagent des patterns **vérifiés par
  représentants** ; les structures de nœud sont mappées. Les entrées individuelles restantes suivent
  ces patterns (non re-décompilées une à une — volume, déjà nommées, hors chemin de rendu direct).
- Les allocateurs/STL génériques (operator new `FUN_00dbbc4f`, `_Xlength_error`, gardes d'init
  statiques) sont hors périmètre sprite et laissés tels quels (nommés au besoin quand récurrents).
