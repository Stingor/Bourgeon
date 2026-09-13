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

## Porte d'entrée réseau — `ZC_NOTIFY_EFFECT2` (0x01F3)

C'est le paquet par lequel le serveur dit « joue l'effet N sur l'acteur G ». Chemin complet,
du socket au nœud de scène (client 20250716 ; **aucun hook Bourgeon sur cet opcode**).

### 1. Du socket au `case`

Tout passe par la boucle unique `RecvLoop_DispatchPackets` (0x00C9DF00), appelée **1× par
frame** depuis `GameMode_InGame_ProcessFrame` (call en 0x00C74AB7), `ECX` = la session InGame :

| Étape | Fonction | Ce qu'elle fait |
|---|---|---|
| 1 | `RecvBuffer_ReadPacket` (0x00C147D0) | dépile un paquet complet du `CRagConnection` dans le **buffer statique `_Dst_015E8198`** ; rend 0 quand la file est vide (fin de boucle) |
| 2 | `RecvBuffer_ReadOpcode` (0x00C144B0) | relit les 2 premiers octets → `0x01F3` |
| 3 | `ReplayRecorder_AppendPacket` (0x00B1ED60) | 0x01F3 a le **flag replay = 1** dans la table de longueurs : il est écrit dans le fichier de replay |
| 4 | `CConnection_BufferPacketForOpcode` (0x00B1E920) | archivage/réassemblage |
| 5 | `jmp g_RecvDispatchTable[(op-0x73)*4]` (0x00C9E2B1) | index `0x1F3-0x73 = 0x180` → slot **0x00CAA8E0**, qui contient **0x00CA64A0** |

Le `case` (0x00CA64A0) tient en trois instructions : `push offset _Dst_015E8198` /
`mov ecx, edi` (la session) / `call 0x00D059A0`, puis `jmp RecvLoop_NextPacket` — il ne
retourne jamais, il reboucle.

### 2. Le handler — `RecvHandler_ZC_NOTIFY_EFFECT_1F3` (0x00D059A0)

Le paquet fait **10 octets fixes** (longueur confirmée par la table client) :
`[+0]` opcode · `[+2]` AID (GID de l'acteur) · `[+6]` effectId (dword).

1. **Résolution de l'acteur** — `ActorMgr_FindByGidOrSelf(session+0xCC, AID)` (0x00A69E70) :
   si `AID == g_Account_Aid` c'est l'acteur local (`mgr+0x2C`), sinon parcours linéaire de la
   liste (`mgr+0x10`) en comparant `actor+0x110` (= le GID). **Acteur introuvable ⇒ le paquet
   est silencieusement jeté** : un effet annoncé sur une entité hors de vue ne laisse rien.
2. **Cas spécial `EF_DUSTSTORM` (1021)** — avant l'effet normal, le handler spawne un
   `EF_EMITTER` (974) **libre** (owner = 0) à la position de l'acteur (`actor+0x10/14/18`) et le
   configure entièrement en dur : vecteurs/gravité `+0xCC..+0xF8`, couleurs `+0x124..+0x130`,
   tailles `+0x104..+0x118`, `+0x14C = 5`, `+0x150 = 2`, échelle 3.0 (0x00AEAE80, propagée aux
   nœuds enfants en `+0xC0`), 0x00AEAE50(30), texture `std::string@+0x134 = "smoke2.bmp"`,
   durée `+0x188 = 1250`. C'est le seul effectId traité à part dans ce handler.
3. **Bascule `/mineffect`** — si `GameSettings_GetFlag(0xE5)` (0x0068EA70 ; la clé de
   `g_GameSettingsFlagMap` est le **cmdId de la commande slash**, cf.
   `GameSettings_SetFlagByCommandName` 0x0068FC70 → 0xE5 = `/mineffect`), trois ids sont
   remplacés par leur variante `.str` allégée :

   | effectId reçu | devient | ressource |
   |---|---|---|
   | 90 `EF_LORD` | 1162 | `lord_of_vermillion.str` |
   | 92 `EF_METEORSTORM` | 1163 | `MeteorStorm\meteor%d.str` |
   | 95 `EF_QUAGMIRE` | 1161 | `Quagmire\Quagmire.str` |

   ⚠ **`/mineffect` (0xE5) n'est pas `/effect` (0x8)** — deux bascules distinctes, prouvé par
   `ChatCmd_LookupSlashCommandTable` (`/effect` → 8 en 0x00D6B341, `/mineffect` → 0xE5 en
   0x00D609A1) et par leurs lecteurs (comptés sur les xrefs de `GameSettings_GetFlag`) :
   - **`/effect` = `TT_EFFECT_ON_OFF` (0x8)** : interrupteur **tout ou rien**, 13 sites, dont le
     portier de rendu `Effect_ShouldDisplay` (0x00C0AF90) — `if (!GetFlag(8) && !effet+0x11C98)
     return 0` : à 0, plus aucun effet n'est dessiné, sauf ceux marqués `IsForceRenderEffect`
     (Lua `effecttool\forcerendereffect.lub`). Lu aussi par `EffectInstance_RenderDraw`,
     `MapAmbientEffect_TriggerByName` et `GameMode_OnEnterMapSetup` (ambiance de carte).
   - **`/mineffect` = `TT_MIN_EFFECT_ON_OFF` (0xE5)** : mode **effets allégés**, 229 sites — il
     ne coupe rien, il choisit partout une variante moins coûteuse (`Effect_ResolveResourceName`,
     `Effect_LoadStrByEffectId`, `EffectDispatch_*`, `SkillHit_SpawnEffectsBySkillId`…). Le
     client le **force lui-même** à l'entrée d'une carte de siège (`ZC_NOTIFY_MAPPROPERTY` = 3
     `AGITZONE`, 0x00CA6421).

4. **Spawn** — `EffectMgr_SpawnEffect(g_EffectMgr, ownerGid = actor+0x110, effectId, x, y, z,
   bound = 0, param7 = -1, force = 0, clé = 0)` (0x00AC12E0) : recherche de la classe dans la
   map id→classe (`mgr+0x8`), allocation par la vtable de l'allocateur, `Effect_SetOwnerActor`
   (effet`+0x20` = le GID), position en `+0x8/0xC/0x10`, `vfunc+0x6C(effectId)`, `vfunc+0x4`,
   `vfunc+0x30`, puis `EffectMgr_LinkEffectToActor`.
5. **Repli si l'id n'a pas de classe** — retour nul ⇒ `Effect_SpawnPrimitiveById(actor,
   effectId, 0, 0, 0, 0)` (0x00C44540) : nœud `.str` primitif (`Effect_LoadStrByEffectId`),
   poussé dans la scène (`SceneNodeList_PushBack`) **et** dans la liste `actor+0x144`. Garde-fou
   commun aux deux chemins : `effectId > 0xEA60` (60000) ⇒ rien.
6. **Post-traitement homonculus** — si `Job_IsHomunculusId(CActorSprite_ResolveDisplayJob(actor))`
   et `effectId == 568` (`EF_HO_UP`), le client **incrémente le niveau de base local**
   (`word actor+0x210`), purge les effets communs (`ActorAiClass_ClearCommonEffects`) et
   réapplique l'aura (`CActorSprite_ApplyLevelJobAura` 0x00C41950 : aura 147 si le niveau vaut
   `g_ES_MaxBaseLevelHomun`, le tout sous la bascule `GameSettings_GetFlag(0x6B)`).

> Conséquence pratique : 0x01F3 est la **seule** entrée réseau qui joue un effet « brut » par id.
> Les effets de skill/statut passent, eux, par les dispatchers (`EffectDispatch_SpawnByEffectId`
> 0x00CEEFF0 & co) depuis d'autres opcodes.

### 3. Anatomie de `EffectMgr_SpawnEffect` (0x00AC12E0)

Il n'y a **pas** de grande table `switch` id→classe ici — c'est une erreur de lecture facile,
parce qu'IDA affiche deux jumptables au début de la fonction (`jpt_AC137E`, 130 cas ;
`jpt_AC13A7`, 254 cas). **Elles sont mortes** : les seules entrées de leur arbre (`loc_AC135F`)
sont la garde de plage `id < 0` (js, 0x00AC12FB) et `id > 0xEA60` (jg, 0x00AC1303) — vérifié aux
xrefs — alors que leurs cases testent 0x23–0xA4, 0x128–0x246, 0x28F/0x290/0x2CE/0x430/0x576,
toutes **dans** la plage valide. Un id hors plage traverse donc l'arbre sans jamais matcher et
tombe sur le `default` = épilogue, `return 0`. (Hex-Rays rend ce bloc sous une forme incohérente,
`(unsigned)id > 0xEA60 && id <= 295` : lire le désassemblage.)

Le vrai corps, pour un id valide, est une file de quatre gardes puis une fabrique :

1. **Dédup `EF_LEVEL150`** — `id == 978 || id == 979` : si l'acteur porte déjà un effet de clé
   200 (`EF_LEVEL99`), `EffectMgr_FindEffect(owner, 200)` ⇒ `return 0` (pas de double aura).
2. **Dédup général** — si `owner != 0` : `EffectMgr_FindEffectByKey(owner, clé, id)`. Effet
   déjà présent **et** `clé > 0` ⇒ `return 0` ; sinon on ne ressort que si le drapeau permanent
   `effet+0xC0` ou `param_8` (force) est posé. ⚠ Pour `clé == 0` aucune de ces conditions ne
   tient : le doublon passe (c'est la cause connue de l'empilement des hat effects permanents,
   commentée en 0x00AC1330).
3. **Résolution de classe** — `EffectClassMap_LowerBound(mgr+0x8, id)` : une **`std::map`
   ordonnée id→allocateur**, remplie par `EffectMgr_Init` (0x00AC2120, ~130 `CEffectAllocator<T>`).
   Miss ⇒ `xor edi,edi` / `retn 0x24` : **échec silencieux, sans allocation ni log** (0x00AC1443).
4. **Fabrique et init** — `(**(alloc))(…)` puis, dans l'ordre : `Effect_SetOwnerActor` (GID en
   `+0x20`), `Effect_SetBoundActor`, position `+0x8/0xC/0x10`, `vfunc+0x6C` =
   `Effect_SetEffectId` (`+0x4`), `param_7 != -1` ⇒ `+0xB0`, `clé > 0` ⇒ `+0xC4 = clé` et
   `+0xC0 = 1`, `vfunc+0x4` (OnCreate), `vfunc+0x30`, enfin `EffectMgr_LinkEffectToActor`.

**Le 9ᵉ paramètre n'est pas une durée** — l'appeler « life » (commentaires historiques de l'IDB)
induit en erreur. C'est une **clé d'instance** : `EffectMgr_FindEffectByKey` (0x00AC2050) retient
l'effet dont `+0x4 == effectId` **et** `+0xC4 == clé`, et `EffectMgr_RemoveEffectByKey`
(0x00AC1AD0, qui exige `clé > 0`) s'en sert pour détruire exactement cette instance. Elle permet
donc à un même effectId de coexister en plusieurs exemplaires distinguables sur un acteur.
Mesures (2026-09-13) : sur les **975** appels à `SpawnEffect`, **944 passent 0** (et la plupart des
`push eax` restants sont un registre nul) ; dans les plages du manager, de la classe de base et de
`Effect_Apply*`, `+0xC4` n'est touché qu'en trois endroits — mis à 0 par
`ActorAttachedEffectBase_ctor`, écrit par `SpawnEffect` (0x00AC14AA), comparé par
`FindEffectByKey` (0x00AC2098) : **aucun décompte temporel**. Le seul fournisseur clair est
`Effect_ApplyGenericEffectWithParam` (0x00C41F30), qui relaie la clé à `SpawnEffect` ou à
`RemoveEffectByKey` selon un booléen on/off ; son appelant `Effect_ApplyEffectIdToActor` l'utilise
pour les **footprints** (effectId 0x96B/0x96C) avec `idUnifié − 0x98A`, c'est-à-dire l'ordinal du
hat effect. Le consommateur final est visible : `CFootprintEffect_LoadFromLua` /
`CFootprintStrEffect_LoadFromLua` relisent `+0xC4` et le passent **comme entier** à un appel Lua de
signature `"d>ss"` (un index → deux chaînes de ressource). La boucle clé → ordinal → ressource est
donc complète.

⚠ Nuance honnête : le champ `+0xC4` est **réutilisé librement par certaines classes concrètes**
d'effet (0x006CA8F0, 0x006CF280, 0x006DB4F0…, où il est manipulé en `movss`/`addss`, donc comme un
flottant accumulé). Ces effets-là sont toujours créés avec clé = 0 et ne sont jamais recherchés par
clé, donc il n'y a pas de conflit — mais c'est probablement de là que vient la vieille étiquette
« life ». Ne pas généraliser la lecture flottante à la sémantique du 9ᵉ paramètre.

Enfin, `SpawnEffect` a **trois retours** possibles, tous par le même `mov eax, edi` (0x00AC14D1) :
le nouvel effet, **l'effet existant rendu tel quel** (chemins 0x00AC1401 « déjà présent et permanent »
et 0x00AC140B « forcé » — rien n'est créé), ou 0.

Les vrais gros `switch` du système sont ailleurs : `EffectDispatch_SpawnByEffectId` (0x00CEEFF0),
`Effect_ResolveResourceName` (0x00AF0900, id→`.str`), `Effect_LoadStrByEffectId` (0x00BB4170),
`EffectNode_UpdateDispatchByEffectId` (0x00B46B50) et `EzEffect_Draw` (0x00B666D0).

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
