# Hat-effect (.str) preview & avatar rendering — RE + design

**Goal.** Certains costumes n'ont **pas de viewid** mais un **hateffect script**
(`Script: "hateffect HAT_EF_X,true;"`). Ils sont donc invisibles dans les endroits
où l'on rend un *sprite* d'avatar (fiche perso, aperçu cashshop, aperçu item_desc),
qui reposent tous sur la capture de sprites (view id → couches d'acteur). Un hat
effect est un **effet `.str`** (billboard 2D animé, blend additif), rendu par un
pipeline **totalement distinct** du système de sprites. Ce doc décrit ce pipeline et
le plan pour capturer/compositer ces effets comme on capture déjà les sprites.

## Ce qu'est un hat effect

- **Serveur (moonlight).** Le script d'équipement d'un costume appelle
  `hateffect HAT_EF_X,true` (`buildin_hateffect`, script.cpp) → `unit_data.hatEffects`
  → paquet **ZC_EQUIPMENT_EFFECT 0x0A3B**. Id = ordinal de l'enum
  `e_hat_effects` (script.hpp : `HAT_EF_MIN=0, HAT_EF_BLOSSOM_FLUTTERING=1, …`).
  Certains items ont À LA FOIS un `View:` et un hateffect ; d'autres SEULEMENT un
  hateffect (view 0) — c'est ce dernier cas qui n'a aucun rendu aujourd'hui.
- **Paquet 0x0A3B** (VAR) : `[type:2][packetLength:2][aid:4][status:1]{effectId:2}`.
  `status=1` = liste complète au spawn/refresh **et** activation unitaire (équip) ;
  `status=0` = désactivation unitaire (déséquip). Modèle incrémental sur un ensemble.
  (`clif_hat_effects` / `clif_hat_effect_single`, moonlight clif.cpp.)
- **Client.** L'id est mappé vers un fichier `.str` via des tables Lua
  `Lua Files\HatEffectInfo\{HatEffectInfo,HatEffectIDs,EffectHatItemTable,…}` et
  rendu par le système d'effets STR, ancré à la tête de l'acteur.

## Pipeline STR côté client (RE confirmée, client 20250716)

Système d'effet `.str` = billboards 2D animés (glow additif), analogues aux sprites
mais sur un chemin séparé. **Fonctions clés (renommées dans le projet Ghidra) :**

| Adresse | Nom | Rôle |
|---|---|---|
| `0x00bcfb10` | `Effect_SubmitStrQuad` | **LE point de capture.** Construit 1 quad écran depuis un workBuf par-couche. |
| `0x00bcfa60` | `Effect_DrawStrFrameQuads` | vtable **+0x0c**. Boucle les couches actives → `Effect_SubmitStrQuad`. |
| `0x00bced10` | `Effect_UpdateStrKeyframes` | Remplit le workBuf (interp. keyframes) — à appeler AVANT le draw. |
| `0x00bb4170` | `Effect_LoadStrByEffectId` | Charge un `.str` par id d'effet **SKILL** (gros switch). Sa QUEUE = recette de chargement générique. |
| `0x00b90780` | `EffectInst_Ctor_StrNode` | Construit un nœud d'effet STR. |
| `0x00715be0` | `Str_GetLayerTexture(layer, idx)` | Résout la CTexture d'une couche. |
| `0x00aebc50` | `Actor_ComputeHatEffectAnchorPos` | Ancre monde/écran de l'effet tête (view/proj caméra). |
| `0x00aec250` | `Actor_GetHatEffectPosOffset` | Offset vertical par job (table + Lua `GetHatEfPos`). |

### `Effect_SubmitStrQuad` — layout du workBuf (param2, `float[]`)

`__thiscall(effectNode=this, layerStruct=param1, workBuf=param2, depthIdx=param3, camera=param4)`

- `[0]/[1]` = centre x,y de la couche (unités canvas `.str`).
- `[2..5]` = srcU0, srcV0, srcW, srcH (rect source en **pixels** de texture).
- `[0xa..0xd]` = les 4 X des coins ; `[0xe..0x11]` = les 4 Y des coins (quad libre).
  (Le commentaire Ghidra d'origine disait "[0xa/0xb] min/max, [0xc..0x11] coins" —
  la vraie lecture du build de sommets = 4 X + 4 Y ; **formule exacte à confirmer**.)
- `[0x12]` = index de texture (→ `Str_GetLayerTexture(param1, idx)`).
- `[0x15]` = angle (deg ; sin/cos via `0x0054dcc0/b0`).
- `[0x16..0x19]` = R,G,B,A (octets) → D3DCOLOR ARGB.
- `[0x1a..0x1c]` = srcBlend, destBlend, blendOp (le plus souvent **additif**).

Texture native : `Str_GetLayerTexture(param1, idx)` → CTexture ; handle DX9 à
**+0x12c** (DX7 : +0x128) — **identique à la capture sprite**. UV normalisés par
`tex+0x14 / tex+0xc` (U) et `tex+0x18 / tex+0x10` (V) ; srcU/srcV sont en pixels.

Le natif projette le centre monde→écran (`Scene_ProjectWorldToScreen`) puis insère
en file différée (`RenderQueue_InsertPrimitive(g_SceneRenderQueue, prim, 5)`). Pour
un rendu ImGui autonome on **ignore la projection** et on reconstruit le quad 2D
billboard directement depuis le workBuf (voir capture ci-dessous).

### Recette spawn/pilotage STANDALONE (confirmée agent 2026-07-12)

Nœud d'effet : **taille 0x11ca8**, ctor `EffectInst_Ctor_StrNode` @ **0x00b90780**
(`__fastcall(node)`), vtable `PTR_01088de8`. Offsets nœud :
`+0x138` srcPos ptr (→ +0x10 XYZ monde, ignoré en capture) · `+0x168` effectId ·
**`+0x178` horloge d'anim (ms) = LE pilote** · `+0x7ec` CStr\* · `+0x7f0` = CStr+0x110
(base couches) · **workBuf couche L = `node+0x7f4 + L*0x74`** · `+0x119f8` nb couches ·
**flag actif couche L = octet `node+0x119fc + L`** · `+0x11c7c/+0x11c80` compteur/max de
boucles. Couche stride `0x380` : `layerStruct(L) = (CStr+0x110) + 0x10 + L*0x380`.

Recette (mode capture offscreen, SANS lien monde) :
1. `node = calloc(0x11ca8)` ; `EffectInst_Ctor_StrNode(node)`.
2. `concreteId = GetHatEffectID(ordinal)` (Lua) ; `Effect_LoadStrByEffectId(node, srcPos,
   concreteId, 0,0,0)` (srcPos = struct zéro, seul +0x10 XYZ lu). Vérifier `node+0x7ec != 0`.
3. Par image : `node+0x178 = elapsed_ms` (RELATIF, sinon dépasse toutes les keyframes) ;
   `Effect_UpdateStrKeyframes(node, &zero3, 0, 0)` remplit les workBuf ; puis ITÉRER les
   couches (L=1..nb, si flag actif) et appeler `Effect_SubmitStrQuad(node, layerStruct,
   workBuf, depthIdx, cam)` — **directement** (bypass des gardes visibilité de
   Effect_DrawStrFrameQuads). Le hook capture chaque couche.
Gardes bypassées : `FUN_00d9d020` (effets activés), `FUN_00c0afa0(node)` (visible).
Ensemble actif du joueur aussi lisible en `ownActor+0x3fc` (std::set, valeurs ≥ 0x98a =
hat, ordinal = valeur − 0x98a) ; on utilise plutôt le suivi 0x0A3B (aucune dép. RE).
**hat ordinal (e_hat_effects) + 0x98a = effectId unifié** ; `GetHatEffectID(ordinal)` →
effectId concret (résolu par le switch de `Effect_LoadStrByEffectId`).

### `Effect_LoadStrByEffectId` — queue de chargement générique

`strObj = UITextureMgr_Load(UITextureMgr_Get(), "Name.str")` ; `node+0x7ec = strObj`
(CStr*) ; `UITexture_AddRef(strObj)` ; `node+0x7f0 = strObj+0x110` (base couches) ;
`node+0x119f8 = *(strObj+0x118)` (frameCount), `-1` si `*(strObj+0x128)==0`.
→ **Charger un `.str` arbitraire** = résoudre son nom + ces 4 écritures. Le switch de
cette fonction concerne les effets SKILL (EF_*), PAS les hat effects (table Lua
`HatEffectInfo`).

## Chemin NATIF name-based (hat effects, `GetHatEffectID`=-1) — RE confirmée 2026-07-12

Le rendu in-world d'un hat effect nommé (ex. gold_shower, ordinal 48) **n'utilise PAS**
le nœud primitif 0x11ca8 ni `Effect_LoadStrByEffectId`. Il passe par une classe dédiée
**`CEZ2STREffect`** (RTTI `.?AVCEZ2STREffect@@`, vtable `0x010758d8`, obj 0x108) :

| Adresse | Nom (Ghidra) | Rôle |
|---|---|---|
| `0x00ac12e0` | `EffectMgr_SpawnEffect` | Factory : id>=0x98a → crée un `CEZ2STREffect` (map @mgr+8), appelle vtbl+0x6c puis +0x4. |
| `0x00aeba00` | `CEZ2STREffect_Ctor` | ctor (base ctor `0x00ae7cd0`, vtable base `0x010755f0`). +0x4 effectId, +0xcc host CActorSprite, +0xd0 resName. |
| `0x00afe670` | `CEZ2STREffect_SetEffectId` | vtbl **+0x6c** (slot 27) : stocke effectId(+0x4), +0xc8=1 si id>=0x98a. |
| `0x00aecb90` | `CEZ2STREffect_Load` | vtbl **+0x4** (slot 1) : crée le host CActorSprite (+0xcc), puis appelle le resolver (slot 32). |
| `0x00af0900` | `Effect_ResolveResourceName` | vtbl **+0x80** (slot 32) : id>0x989 → Lua `GetHatEfResName(id-0x98a)` (sig `"d>s"`) → nom `.str`, puis `CActorSprite_LoadStrEffect(host, nom)`. |
| `0x00ad7d00` | `CActorSprite_LoadStrEffect` | **charge le `.str` par NOM** dans le host : `UITextureMgr_LoadResourceByName` → host+0x2bc ; nom complet → +0x3f60 ; **`_splitpath` → sous-dossier → +0x3f78**. |
| `0x00ad7c80` | `CActorSprite_InitStrLayers` | +0x2c0=CStr+0x110, +0x3cc8=frameCount, workbufs +0x2c4, flags +0x3d4c (miroir du nœud 0x11ca8). |
| `0x00ad8c40` | `CActorSprite_SubmitStrQuad` | submit par-couche (workBuf **identique** à `Effect_SubmitStrQuad`). Branche texture ci-dessous. |
| `0x00715d30` | `Str_GetLayerTextureWithDir` | `"effect\{}{}"` = `effect\` + dir + nom-nu → charge la texture avec sous-dossier. |

Route packet : `0x0A3B` → `Effect_ApplyEffectIdToActor(0x00c41ba0)` (id=ordinal+0x98a) →
Lua `GetEffectType` → branche générique → `Effect_ApplyOrRemoveGenericEffect(0x00c41ec0)`
→ `EffectMgr_SpawnEffect(mgr, actorHandle, id, …)` → `CEZ2STREffect`.

### Réponse SOUS-DOSSIER (définitive)

Les noms de texture stockés dans le `.str` sont **NUS** (`"coin001.bmp"`), même pour un
`.str` en sous-dossier. Le sous-dossier n'est PAS dans les noms ; c'est un **champ
`std::string` séparé** sur le host CActorSprite (`+0x3f78`, taille `+0x3f88`), posé par
`_splitpath` du chemin `.str`. Preuve = branche texture de `CActorSprite_SubmitStrQuad` :

```c
if (*(host+0x3f88) == 0)  tex = Str_GetLayerTexture(layer, texIdx);          // "effect\<nu>"
else                      tex = Str_GetLayerTextureWithDir(layer, texIdx, host+0x3f78); // "effect\<dir><nu>"
```

Même `layer`, même tableau de noms (`layer+idx*4+0x10`) dans les deux cas.
`Str_GetLayerTexture` (0x00715be0, chemin nœud primitif via `Effect_SubmitStrQuad`) ne
gère PAS les sous-dossiers ; `Str_GetLayerTextureWithDir` (0x00715d30, chemin CActorSprite)
si. → Bourgeon DOIT préfixer le sous-dossier lui-même dans son chemin nœud primitif.

### Recommandation offscreen (Q4)

Garder le **nœud 0x11ca8 autonome** (léger, sans acteur/monde ; marche pour items NON
équipés = cashshop/item_desc), MAIS dans le hook `Hooked_StrQuad`, résoudre la texture
**exactement comme le natif** : dir vide → `Str_GetLayerTexture` ; sinon →
`Str_GetLayerTextureWithDir(layer, texIdx, &dirStdString)` (dirStdString = `std::string`
ABI MSVC du dossier via `_splitpath`). Ça remplace le loader snprintf+cache maison par le
résolveur natif (et son cache par-couche +0x1c8). Idéalement, remplacer aussi le
« dummy-load StormGext + swap CStr » par : setup pré-switch minimal du nœud + queue de
chargement par NOM (déjà répliquée). L'option « nœud live déjà spawné » est REJETÉE :
n'existe que si le JOUEUR porte l'effet (set `ownActor+0x3fc`), inutile pour prévisualiser
un item non équipé, et il faudrait de toute façon hooker `CActorSprite_SubmitStrQuad`.

## Design du rendu (mirror de la capture sprite)

La capture sprite existante (`basic_info.cc`) hooke `Actor_SubmitSpriteQuad`
(`0x00a1b7c0`) et, pendant qu'on rend un acteur standalone, capture chaque couche
(texture + UV + géométrie) dans `g_av_caps[]`, puis recompose via ImGui `AddImage`.
On applique **exactement le même patron** aux effets STR :

1. **Acquisition de l'id d'effet.**
   - *Fiche perso* (propre joueur) : `own_hat_effects_` suivi via l'observation de
     0x0A3B (fait, `BasicInfo::OnRecvPacket`). Aucune RE supplémentaire.
   - *Cashshop / item_desc* (item quelconque) : `GetHatEffectID(itemID)` (Lua natif,
     via le bridge Lua brut déjà utilisé pour `GetStateIconDescript`) → id, 0 si
     aucun. Prérequis data : nos costumes custom présents dans `EffectHatItemTable.lua`
     (qu'on distribue). Repli éventuel : push serveur (opcode bourgeon). **À trancher
     selon le retour agent sur `FUN_00c41ce0`=GetHatEffectID / `FUN_00d64e99`.**

2. **Capture STR** (nouveau moteur dans `basic_info.cc`) :
   - Hook `Effect_SubmitStrQuad` (JMP hook, comme le sprite). Flag `g_str_cap_active`.
   - Nœud d'effet standalone chargé du `.str` du hat effect (recette agent), une image
     forcée via `Effect_UpdateStrKeyframes`, dessin via `Effect_DrawStrFrameQuads`.
   - Dans le hook : lire le workBuf → `StrCapLayer{tex, corners[4], uvs[4], rgba, blend}`
     en espace canvas 2D (sans projection), SEH-gardé ; supprimer l'insert natif.

3. **Composite** (par-dessus l'avatar déjà composité) :
   - `ImGui::GetWindowDrawList()->AddCallback(D3D9_AdditiveBlendCallback(), nullptr)` →
     `AddImageQuad(tex, p0..p3, uv0..uv3, tint=rgba)` par couche → `AddCallback(
     ImDrawCallback_ResetRenderState, nullptr)`. (Callback additif DX9 déjà présent.)
   - Ancrage tête : caler le centre de l'effet sur la tête de l'avatar composité
     (région tête connue via `head_region`), à l'échelle de l'avatar. Offset vertical
     façon `Actor_GetHatEffectPosOffset` si besoin d'affiner.
   - GIF export : compositer les quads STR (blend) dans le RT hors-écran
     (`D3D9_CompositeQuadsRGBA` étendu au blend) — v2 (l'anim STR ≠ anim sprite).

4. **Consommateurs** (les 3 endroits « sprite d'avatar ») :
   - `character_sheet` `DrawDoll` → `RenderPlayerAvatar` superpose `own_hat_effects_`.
   - `cashshop_tweaks` hover → `RenderItemPreviewTooltip` reçoit un hatEffectId.
   - `item_desc_tweaks` hover → idem (les gates `view_id!=0` élargies au hatEffect).

## Risques

- Live client → tout hook/capture STR **SEH-gardé** ; échec ⇒ 0 couche (pas d'overlay).
- Durée de vie du nœud standalone (alloc/dtor), taille de l'objet — **recette agent**.
- Blend non-additif de certaines couches (rare pour les hat effects glow) : v1 tout
  additif, raffinable par-couche via le blend capturé.
- DX7 : callback additif DX9-only ; overlay effet dégradé sous le proxy DX7 (legacy).

## ⚠ Deuxième famille : hat effects `hatEffectID` (EZ-particle, PAS `.str`) — RE 2026-07-13

`HatEffectInfo.lub` a **deux formes** d'entrée :
- `resourceFileName = "x.str"` → billboard STR (résolveur `GetHatEfResName`) — **géré** (gold_shower…).
- **`hatEffectID = <id>`** (ex. `[HAT_EF_Digital_Space] = { hatEffectID = 1240 }`) → **effet PARTICULES
  procédural** (« EZ-effect »), **PAS un `.str`**. `HatEffectIDs.lub` : `HAT_EF_Digital_Space = 87`
  (ordinal). `GetHatEfResName(87)` renvoie **vide** → la fiche perso est **NUE** (validé live).

**Chemin natif (RE live+Ghidra 2026-07-13, effet équipé, x32dbg)** :
`0x0A3B` → `Effect_ApplyEffectIdToActor(id=87+0x98a=2529)` → `GetEffectType` classe HAT →
`Effect_ApplyHatEffectViaLua(0xc41ce0)` : `GetHatEffectID(87)=1240` puis **msg acteur 0x6d** :
`Actor_OnMsg(0xd473b0)[vtbl+8]` → table `0xd47ebc[0x6d]=0x15` → `Actor_OnMsg_Base(0xd3c500)`
→ table `0xd3d5d4[0x6e]=0x10` → `Actor_OnMsg_AppearanceEffects(0xc4aea0)` → byte-table
`0xc55098[0x6d]=0x55` → jumptable `0xc54f40[0x55]=0xc54e84` → **`Actor_ManagePrimitiveEffectList
(0xc4a7a0)` case 3** → `Effect_SpawnPrimitiveById(actor,1240)`.

**Objet live (validé par lecture mémoire)** : `ownActor+0x144` = liste (count `+0x148`=3) de nœuds
primitifs (vtable `0x01088de8`). Le nœud id **1240** (`+0x168`) a `CStr +0x7ec = 0` (aucun `.str`)
et **`Effect_OnCreateBuildEmitters(0xbb80e0)` ne couvre que 300..373** → rien. MAIS `node+0x11c84`
= **nœud ENFANT** (vtable **`0x01088c48`**) = **nœud EZ-particules**. Son draw
`EzEffect_Draw(0xb666d0)` (vtbl+0x0c) dispatche sur `child+0x1d0` (~170 sous-effets) et **pousse un
quad 2D** (`child+0x2210` : 4 verts pos/couleur/UV, textures `.tga/.bmp` p.ex. `effect\ring_purple.tga`,
`Magic_Violet.tga`) via **`EzEffect_SubmitQuad(0x00550de0)` → `RenderQueue_InsertPrimitive`
(g_SceneRenderQueue)**.

🔑 **Conséquence capture** : ce rendu **NE passe NI par `Effect_SubmitStrQuad`(0xbcfb10) NI par
`Actor_SubmitSpriteQuad`(0xa1b7c0)** — les DEUX hooks de Bourgeon aujourd'hui. D'où l'invisibilité
totale dans les previews. **Point de capture pour ces effets = hooker `EzEffect_SubmitQuad`
(0x00550de0) / `RenderQueue_InsertPrimitive`** et lire le quad (4 verts écran + couleur + UV +
texture, comme la capture STR mais struct source différente à `node+0x2210`). Pilotage offscreen =
répliquer `Effect_SpawnPrimitiveById(1240)` + drive/draw du nœud enfant EZ (émetteurs qui spawnent
des sous-nœuds dans le temps) → plus lourd que le STR. Adresses renommées Ghidra : `EzEffect_Draw`,
`EzEffect_SubmitQuad`, `Actor_ManagePrimitiveEffectList`, commentaires sur `Effect_ApplyHatEffectViaLua`.

**Statut EZ (2 familles)** : [x] RE complète (chaîne + capture point + filtre propriétaire, offsets
vérifiés live). [x] **Phase 1** (capture, VALIDÉE live : `[EzFx] captured=256 seen=7200 owner=1`) :
hook `Hooked_EzQuad`/`InstallEzCapture` + `EzCapTri` + `EzOwnedRoot` (remonte `+0x140` -> nœud dont
`+0x138`==ownActor) + arme/reset dans `OnTick`, passthrough non destructif, cap 2048. [x] **Phase 2
code** (composite fiche perso) : `DrawEzCapTris` re-ancre les sommets écran ABSOLU sur le doll —
ancre = `Scene_ProjectWorldToScreen(g_ez_queue, world=root+0x10, viewMtx=cam+0x98, &sx,&sy,&invW)`,
échelle `S = Effect_DepthToScreenScale(g_ez_queue, invW)`, `doll = (ox+(vx-sx)·R, oy+(vy-sy)·R)`,
`R=(s/S)·g_ez_cal`, blend additif, D3DCOLOR->IM_COL32. Appelé dans `RenderPlayerAvatar`. [ ] Validation
build fiche perso + calibrage (`g_ez_cal`, blend, z-order). [ ] item_desc hover. [ ] cashshop
non-équipé = spawn offscreen (`Effect_SpawnPrimitiveById(1240)` + drive) — phase 3.

### ✅ Capture EZ : verts en ÉCRAN (XYZRHW), le vrai bug était un SUR-MATCH (RE 2026-07-13)

Fausse piste initiale (« coords monde ») CORRIGÉE. Chaîne de draw : `EzEffect_Draw` ->
`EzEffect_SubmitQuad(0x550de0)` (enqueue) -> `RenderQueue_InsertPrimitive(0x550b10)` (tri par Z en
buckets, NE projette pas) -> flush `World_RenderScene` -> `RenderPrim_DrawRecord(0x560430)` =
`DrawPrimitiveUP(g_SceneRenderQueue vtbl+0x38)` des verts **XYZRHW+DIFFUSE+SPEC+TEX1**. STR (type 5)
et EZ (type 3) partagent CE MÊME chemin de draw -> **même FVF XYZRHW = ÉCRAN pré-transformé**. Donc
les verts EZ SONT en écran (x@+0, y@+4), comme les STR. Le « vert à (847,2128) / span 7000px / nuages »
venait d'un **SUR-MATCH** : `EzEffect_SubmitQuad` est un enqueue de quad PARTAGÉ (UI/curseur/autres
effets) ; `rec-0x2210` n'est un nœud EZ que pour les appelants `EzEffect_Draw`. Pour les autres, on
lisait des octets étrangers et on capturait des quads hors-sujet. **FIX** : discriminateur vtable dans
`EzOwnedRoot` — n'accepter `rec-0x2210` que si vtable==`0x01088c48` (enfant EZ), et n'accepter la
racine (owner via `+0x140`->`+0x138`) que si vtable==`0x01088de8` (nœud primitif). Ensuite le
re-ancrage écran de `DrawEzCapTris` (ancre = projection de l'origine acteur, `R=s/S`) est correct pour
des verts écran. Ghidra : `RenderQueue_InsertPrimitive`, `RenderPrim_DrawRecord`.

**Confirmation FVF (diag live 2026-07-13)** : `tri0.z=0.9816` (∈[0,1] = depth), `tri0.rhw=0.0025`
(=1/w) -> **XYZRHW ÉCRAN** confirmé. `tri0.xy=(878.9,-5.5)` = à l'ancre x=880, ~500px au-dessus des
pieds (495) = un panneau flottant au-dessus du perso = CORRECT. MAIS le filtre propriétaire laisse
passer des OUTLIERS (verts à x≈-2482..3922, hors-champ) -> « nuages ». **FIX pragmatique** : filtre de
PROXIMITÉ écran (`g_ez_max_r`=700px autour de l'ancre) dans `DrawEzCapTris` — robuste car verts écran
et effet joueur COMPACT. Reste à investiguer pourquoi le walk `+0x140`->`+0x138`==owner sur-matche
(band-aid pour l'instant). Diag `near=X/total`. Note : l'effet (~600px) déborde le cadre doll (190px)
-> clip (ou réduire `g_ez_cal`). `g_ez_enabled=true`.

## Statut

- [x] Suivi `own_hat_effects_` (0x0A3B) — fiche perso.
- [x] Recette spawn/drive STR standalone (agent RE, ci-dessus).
- [x] Moteur de capture STR (`Hooked_StrQuad`) + composite additif (`DrawStrCapLayers`)
      + spawn/pilotage cache (`CaptureHatEffectConcrete`) + `HatOrdinalToConcrete` (Lua).
- [x] Câblage **fiche perso** : `RenderPlayerAvatar` superpose les hat effects actifs,
      ancrés sur la tête (aucune édition de character_sheet.cc — il appelle déjà la fn).
- [x] Résolution **item→ordinal** (cashshop/item_desc, items NON équipés) : le natif ne
      mappe PAS item→ordinal (`effectHatItemTable` = simple appartenance ; `GetHatEffectID`
      prend l'ordinal). Source AUTORITATIVE = scripts item_db (`hateffect HAT_EF_x`). Le
      SERVEUR (moonlight) scanne `script_src`/`equip_script_src`, résout via
      `script_get_constant` (case-insensitive), et POUSSE la table au login → **ZC
      0x0F17 ZC_BOURGEON_HATEFFECT_MAP**. Client : cache `g_hat_item_ord`, `ItemToHatOrdinal`.
- [x] Câblage **cashshop + item_desc** : survol → `ItemToHatOrdinal` → `RenderItemPreviewTooltip`
      (view + ordinal). Costumes view==0 : perso de base + effet superposé.
- [x] **Calibrage LIVE en jeu** : section « Effet costume (calibrage) » dans la fiche perso
      (DrawDoll, visible si un hat effect est actif) — sliders échelle/décalage X-Y/winding/
      rotation (membres `hat_*` de BasicInfo) + DIAGNOSTIC (actifs / couches capturées /
      id concret). `couches 0` = spawn/capture STR KO. Défaut d'échelle = s×g_av_body_scale
      (principe : les coords sprite intègrent l'échelle acteur, pas les coords .str).
- [x] **Persistance** des réglages : sauvés dans bourgeon_settings.yaml (clés `hat_scale`/
      `hat_off_x`/`hat_off_y`/`hat_winding`/`hat_angle_mode`/`hat_enabled`, cf. moonlight_ui.cc)
      à la relâche d'un slider (mu->SaveSettings). Le calibrage survit aux sessions → feature
      auto-suffisante (build → cale en jeu → figé).
- [ ] **Validation live** (réservée à l'utilisateur — build/relaunch géré par lui) : confirmer
      `couches>0` + rendu correct dans les 3 emplacements, ajuster les sliders si besoin.

## Points de calibrage (édition 1-ligne dans basic_info.cc)

- `RenderPlayerAvatar` : `kHatScale` (× échelle avatar), `kHatOffY` (px, +=bas) + l'ancre
  `s_head_cx/s_head_cy` (centre région tête).
- `DrawStrCapLayers` : ordre des sommets `AddImageQuad(p[0],p[2],p[3],p[1])` (strip→périmètre) ;
  conversion d'angle `L.angle * π/180` (constante native exacte : `deg/ *0x01074c80 / *0x00fd6af4 * *0x00fd5b0c`).
- `Hooked_StrQuad` : blend forcé additif (prémultiplié) ; par-couche `sblend/dblend`
  capturés pour un raffinement futur (couches non-additives rares).
