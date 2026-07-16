# Système d'effets client — RE exhaustive (Ghidra, client 20250716)

Documentation du **sous-système d'effets visuels** du client Ragnarok Online (base 0x400000),
issue d'une passe RE exhaustive **prouvée par la data** (2026-07-16). Tout ce qui suit a été
renommé + commenté dans le projet Ghidra, jamais par supposition : chaque nom vient d'une
ressource (`.str`/`.wav`/`.tga`/`.bmp`/`.spr`), d'un effectId constant, d'une classe RTTI ou
d'une vtable partagée effectivement référencés dans la fonction.

> ⚠ Correction historique : la mémoire projet prétendait « module effet 100 % documenté »
> (2026-07-13). C'était **faux** — seule la machinerie *générique* (EffectNode/EffectMgr/
> CEZ2STREffect + table `Effect_Init*` en 0xc0x + sous-renderers EZ) l'était. Les **classes
> d'effet concrètes par-skill** (~130 classes RTTI) et leurs handlers étaient restés `FUN_`.
> Cette passe les a couverts : **~550+ fonctions + ~65 vtables** nouvellement documentées.

## Architecture

Un effet visuel = une **instance de classe** dérivée de `ActorAttachedEffectBase`
(base ctor `ActorAttachedEffectBase_ctor` 0x00ae7cd0, base dtor 0x00ae7f00). Deux grandes
familles de rendu :

1. **STR / host-sprite** (`CEZ2STREffect` et dérivées) : billboards `.str` animés (glow
   additif). Création de couches via `CEZ2STREffect_CreateHostSprite` (0x00b1b3b0) +
   `CActorSprite_LoadStrEffectVariant` (0x00ad7f30). Cf. `docs/hat_effect_re.md`.
2. **Particules / EZ** : nœuds de particules procéduraux via `EffectInst_BuildChildNode`
   (0x00bb5d10, → nœud enfant `this+0x11c84`), dessinés par `EzEffect_Draw` (0x00b666d0,
   ~170 sous-renderers `EzEffect_DrawSub_*` déjà documentés).

Les deux partagent l'instance (`this+0x178` = compteur de frame, `this+0x17c` = frame de fin,
`this+0x10/14/18` = position monde, `this+0x138` = acteur source) et le tick de keyframes STR
`Effect_UpdateStrKeyframes` (0x00bced10).

### Gestionnaire & application
- `CEffectMgr` (singleton `DAT_015beeac`) : `EffectMgr_SpawnEffect` 0x00ac12e0,
  `EffectMgr_FindEffect` 0x00ac1fe0, `EffectMgr_FindClassByIdAndCallLoad` (map id→classe),
  `EffectMgr_Init` 0x00ac2120 (enregistre ~130 allocateurs `CEffectAllocator<T>` — fonction
  de ~1.5 Mo de désassemblage).
- **Application par id** (0xc41xxx) : `Effect_ApplyEffectIdToActor` 0x00c41ba0,
  `Effect_ApplyOrRemoveGenericEffect` 0x00c41ec0, `Effect_ApplyHatEffectViaLua` 0x00c41ce0.
- **Dispatchers** (0xce–0xd0, cette passe) : `EffectDispatch_SpawnByEffectId` 0x00ceeff0
  (LA table maîtresse : gros switch effectId→SpawnEffect, ~100 entrées, variante via
  `OptionInfo_GetValue(0xe5)`), `Effect_ApplySkillCastVisual` 0x00cee6e0,
  `EffectDispatch_StatusChangeVisual` 0x00d039b0, `EffectDispatch_JobStatusVisual` 0x00d0c6b0,
  `EffectDispatch_SkillEffectOnActors` 0x00d07800, etc.

## Carte des clusters d'adresses

| Plage | Rôle | Convention de nom |
|---|---|---|
| `0x006c0000–0x006f8000` | **Classes STR concrètes** : OnCreate/Update/Spawn + ctor/dtor/scalardtor/vtable | `C<Classe>Effect_*` (RTTI) ou `EffectSTR_*` / `EffectAttachedClass_*_<addr>` |
| `0x00ab0000–0x00b17000` | **Classes non-STR** (particules/EZ) : ctor/dtor/scalardtor/vtable | `EffectAttachedClass_{ctor,dtor,ScalarDeletingDtor}_<addr>` + `vftable_*` |
| `0x00b45000–0x00b8f000` | Machinerie générique EZ/EffectNode (déjà doc. 2026-07-13) : `EffectNode_Update*`, `EzEffect_DrawSub_*` | — |
| `0x00ba0000–0x00c00000` | **Builders/updaters skill** (particules + keyframes STR) | `EffectUpdate_<Skill>`, `Effect_Build_<Type>`, `Effect_TickStr_*` |
| `0x00c00000–0x00c13000` | Table `Effect_Init*` (particules, déjà doc.) | `Effect_Init*` |
| `0x00730000–0x00770000` | **Attache d'effet statut/persistant** (find/spawn ciblé par id) | `EffectAttach_Fx<id>`, `EffectSpawn_<Skill>`, `EffectRespawn/Refresh_*` |
| `0x00cee000–0x00d0e000` | **Dispatchers d'application** d'effet | `EffectDispatch_*`, `EffectApply_*`, `EffectRemove_*` |

## Familles particulières

### Hat effects
Deux formes (cf. `docs/hat_effect_re.md`) : `.str` name-based (CEZ2STREffect → `GetHatEfResName`)
et `hatEffectID` particules/sprite (EZ). Son par id : `CEZ2STREffect_QueueSoundByEffectId`
0x00aed3d0 (jumptable + `GetHatEfPos*` Lua pour id>0x989).

### Footprint (traces de pas) — NOUVEAU cette passe
- `CFootprintEffect` (PNG) : vtable `0x0100df44`, base id 0x96b. ctor 0x006dcde0,
  dtor/scalardtor/ClearSprites + `CFootprintEffect_LoadFromLua` (getters
  `GetFootprintPng{File,Scale,Aplha,Duration}`, `GetFootprintStride`).
- `CFootprintStrEffect` (.str) : vtable `0x0100e054`, base id 0x96c. ctor 0x006ddac0,
  `CFootprintStrEffect_LoadFromLua` (getters `GetFootprintStr{FileBottom,FileTop,Scale,
  TopHeight,Gap}`, `IsFootprintStrAdjustAngle`), `_ComputeFootprintTransform` (direction =
  pos courante − pos préc., rotY 90°, côté G/D), `_CreateFootprintSprite`/`_SpawnFootprintPair`/
  `_CreateShadowSprite`/`_Update`.
- Tables Lua chargées par `FUN_00d64f55` (voisine de `Client_LoadHatEffectLuaTables` 0xd64e99) :
  `FootPrintEffectInfo.lua`, `HatEffect_F.lua`, `SignBoardList_F.lua`.

## Méthodologie & preuve d'épuisement

Le sous-système a été balayé par **ancrage sur les primitives partagées** : toute fonction
appelant une primitive d'effet EST un effet. Pour chaque primitive, on a énuméré ses appelants
(`get_function_xrefs`), documenté tous ceux en `FUN_`, puis **re-scanné** pour prouver qu'il
n'en reste aucun. État final (2026-07-16) :

| Primitive | Adresse | Appelants `FUN_` résiduels |
|---|---|---|
| `CEZ2STREffect_CreateHostSprite` | 0x00b1b3b0 | **0** ✅ |
| `EffectInst_BuildChildNode` | 0x00bb5d10 | **0** ✅ |
| `ActorAttachedEffectBase_ctor` | 0x00ae7cd0 | **0** ✅ |
| `Effect_UpdateStrKeyframes` | 0x00bced10 | **0** ✅ |
| `Effect_BuildRenderVertices` | 0x00b469f0 | **0** ✅ |
| `ActorAttachedEffect_QueueSound` | 0x00ae8030 | **0** ✅ (dans le module effet) |

Les seules entrées « From `<addr>` » sans conteneur `FUN_` restantes sont des **sites d'appel
à l'intérieur du switch** `Effect_LoadStrByEffectId` (région ~0xbc5000–0xbca400) et des
fragments non délimités par Ghidra en 0x743000–0x758000 : ce ne sont pas des fonctions à nommer.

## Résidus assumés (non renommés, par honnêteté data)

1. ~~`FUN_00d0d3a0`~~ → **IDENTIFIÉ = `EffectDispatch_PlayEffectOnActorById`** (~78 Ko, décompile
   échoue). Méthode d'ID d'une fonction non-décompilable, par la data : (a) `get_function_by_address`
   → taille+signature `(this, ushort id, …, GID, …)` ; (b) `get_xrefs_to` → appelant 0xca3a0c
   (handler paquet) ; (c) désassemblage brut → prologue lit `word[EBX+8]`=effectId, résout l'acteur
   par GID (gate 0x015fb9a4 / `ActorList_FindByGID` 0xa69eb0), puis SWITCH dense (15 jumptables
   0xd1a404+) ; (d) grep du désassemblage → **328/673 CALL** vers `SpawnEffect`/`FindEffect`/
   `QueueSound`/`Sound_Play3D`. = dispatcher maître « jouer l'effet EF_ id sur un acteur »,
   frère géant de `EffectDispatch_SpawnByEffectId` (0xceeff0).
2. **RTTI MSVC non analysée dans le projet Ghidra** : les TypeDescriptor `.?AVC…Effect@@` et les
   allocateurs `.?AU?$CEffectAllocator@…@@` n'ont **aucune xref** (COL non formés). Conséquence :
   ~84 classes concrètes (44 STR en 0x6c–0x6f + ~40 non-STR en 0xab–0xb1) portent un nom
   **générique prouvé** `EffectAttachedClass_*_<addr>` au lieu de leur nom RTTI. Chaque commentaire
   contient l'**effectId** (baseId) + la vtable pour permettre le remapping. **Pour convertir en
   noms de classe** : activer l'analyse RTTI MSVC dans Ghidra, puis mapper vtable→`??_R4`→nom.

## Catalogue RTTI (référence)

Liste des classes d'effet (via `list_strings(filter="Effect@@")` / `"CEffectAllocator"`) :
CAstralStrike, CAMachine, CCaneOfEvilEye, CFuumaShouaku, CChargingPierce, CJupitelThunderStorm,
CClimax, CNyanggrass, CCrimsonArrow, CCurseOfCube, CEternalSlashAttack/Count, CFallenAngel,
CFloralFlareRoad, CFromTheAbyss, CHawkVumerang, CPetitio, CPneumaticusProcella, CSavageImpact,
CServantWeapon, CStormCannon, CTetraBall, CViolentQuake, CFootprint, CFootprintStr, CFullScreen,
CMeteor, CLevel99/Orb1/Orb2, CSmoke, CEmitter, CLevel150/Sub, CMapChain, CMagicFloor,
CEZ2STREffect(Ex), CThrowItem, CRotateItem/Line, CEnergyOrb, CSprite, CSquareRange, CScatter,
CCellRange, CCherryBlossom, CStormKick, CEvilsPaw, CFriggSong, CBindTrap, CAnimatedEmitter,
CCloudKill/NewCloudKill, CRectUp, CSpriteMable, CTunaParty, CStemSpear, CPowdering, CRootTwist,
CHeal, CWarningPlane, CSvgSpirit, CRichsCoin, CRandomOpt/Hit, CLightSphere, CJupitelThunderHit,
CGravityControl, CStarEmperor, CSunStance, CSoulUnity/Collect, CSevereRainStorm, CTriangleShotArrow,
CCartCannon, CEvilLand, CAxeBoomerang, CShadowExceed, CCompetentia, CItemDrop, CShow, CSprArrow,
CGaleStorm, CCylinder, CAcidifiedThrow, CBlastForge, CWind, CTalismanMark, CChangeSize,
CShieldShooting, CRollingCutterRag, CAbleToMakeEffect, CEfstAction_ShowEffect… (~130 au total).
