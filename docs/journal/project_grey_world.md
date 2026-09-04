# GreyWorld : décors masqués, quadrillage GAT, sol aplati

> Journal du chantier. La fiche de mémoire `project_grey_world` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

**GreyWorld** = l'équivalent RÉGLABLE et RÉVERSIBLE des GRF « greyworld » /
« darkside » de la communauté RO (sol dépouillé pour la WoE). Livré le
2026-09-01 dans **Gameplay > GreyWorld** ([src/features/fx/grey_world.cc]).

🔴 **Le « Sol uni » de ground_paint N'EST PAS un greyworld** : il ne fait qu'UNE
couleur, il ne dit rien des cellules. Il RESTE dans Staff Tools (fond de capture,
décision utilisateur). GreyWorld le réutilise comme brique via
`ground_paint::SetExternalPaint(on, rgba)` — un seul mécanisme, deux
commanditaires, **la couleur du staff gagne** quand les deux sont actifs.

## Les 4 adresses (client 20250716)

- **`Rsm3dModel_RenderNodeAndChildren` 0x00a59e20** — le SEUL chemin des décors
  `.rsm` du `.rsw`. Hook qui retourne 0 ⇒ plus d'arbres/murs/props, enfants
  compris (la récursion repasse par le même slot de vtable). Les mobs et PNJ 3D
  ne bougent pas : eux sont en **Granny .gr2**, cf. [[reference_mob_3d_granny]].
- 🔴🔴 **MASQUER NE SUFFIT PAS : un décor masqué reste dans le QUADTREE DE
  PICKING** — le clic au sol bute dessus, un arbre invisible arrête le curseur
  (constaté par l'utilisateur, surtout visible une fois le terrain aplati). Pour
  l'en sortir il faut ne pas le CHARGER : hook `CRsm_Load` **0x0071a720**
  (`__thiscall`, retn 4) qui rend 0. ⭐ Ne RIEN vider à la main : `CRsm_Load` est
  le slot vtable +20 qu'appelle `UITextureMgr_Load` 0x00a8d4a0 ; un retour ≠ 1 la
  fait **détruire proprement la ressource et rendre nullptr**, et `CWorld_Load`
  ignore alors ce modèle (`if (v33)`). Chemin d'erreur natif, aucune fuite,
  aucune liste à recoudre. Seule trace : une ligne de journal par fichier
  (`UITextureMgr_ReportLoadFailureOnce`).
  🔴🔴 **CE N'EST PAS UN RÉGLAGE À PART** : le déchargement est EMPORTÉ par
  `flatten`. Livré d'abord comme case indépendante, **retiré à la demande de
  l'utilisateur** (2026-09-01) : « ça ne doit pas être un choix sinon je vais
  avoir des bug reports ». Un décor garde sa hauteur d'origine ⇒ sur sol aplati
  il flotte, et il bloque le curseur ⇒ **aplatir sans décharger est un état
  CASSÉ, qui ne doit pas être atteignable**. Règle générale : ne pas exposer une
  combinaison de réglages qui ne peut produire qu'un défaut.
- **`CScene_RenderCellsAndCursor` 0x00a7b0a0** `__fastcall(this)` — la passe où
  le client dessine sa trace de navigation, son curseur de destination et la case
  de l'homoncule. **`scene` = this + 152**, **CWorld = *(this + 432)**. C'est LE
  point d'accroche pour dessiner ses propres cellules.
- 🔴🔴 **`C3dGround15_DrawCellQuad` 0x00a63800** — `(scene, cellX, cellY, argb,
  spriteRes, uvScale8f)`. **`__thiscall`, PAS `__stdcall`** : Hex-Rays affiche
  __stdcall parce qu'elle n'utilise pas `this` elle-même, mais elle transmet
  **ecx INTACT** à `C3dGround15_GetCellCorners` 0x00a62b70, qui lit `this+8`.
  Appeler sans poser `this` lit les coins depuis une adresse arbitraire.
  Le quad épouse le relief, prend un biais z de −3.05e-5 (passe devant le sol
  sans z-fighting) et entre dans la file de scène ⇒ **DX7 ET DX9**.
- **`C3dGround15_DrawCellMarkerGrid` vtbl+0x38** 0x00a637b0 — le même, mais il
  résout `grid.tga` À CHAQUE APPEL. 🔴 `SpriteRes_GetOrLoadByName` 0x00568760
  fait un **addref** : une résolution PAR CELLULE (2400/frame) ferait déborder le
  compteur en une soirée. ⟹ résoudre **une fois par frame** et appeler
  DrawCellQuad, comme le natif le fait pour son curseur.
- 🔴🔴 **`SpriteRes_GetOrLoadByName` prend CINQ args pile, pas trois**
  (`retn 14h`) : `(cache_en_ecx, "grid.tga", 0, 0, 1, 0)`. Le prototype Hex-Rays
  n'en montre que trois et **a coûté un crash** — 8 octets laissés sur la pile
  par frame, mort de l'APPELANT dans `CScene_RenderCellsAndCursor+0x17` avec
  `esi` = une coordonnée de cellule. Cf. [[feedback_native_hooking]] §3 bis.

L'objet : **`*(CWorld + 0x28)`** = C3dGround15 (`gamescene::kAmGround`), CWorld
étant `*(CGameMode + 0xCC)`.

## Le DESSIN d'une case : deux textures du client, aucune à installer

- **`grid.tga`** (`data\texture\`) = la CROIX du curseur de destination. C'est un
  marqueur, pas un quadrillage.
- **`effect\SquareRange.tga`** = le CADRE CARRÉ des sorts de zone
  (`sub_AE6980` → `EffectParticle_SetTextureByName`). C'est celle qui donne un
  **carrelage**, et c'est le défaut livré.

🔴 `data.grf` de Moonlight est **CHIFFRÉ** (« Event Horizon ») ⇒ `tools/grf_reader.py`
le refuse : **on ne peut PAS inventorier les textures disponibles**. Les seuls
noms sûrs sont ceux que l'EXE cite lui-même. Et une texture absente ne se détecte
pas en code : `SpriteRes_GetOrLoadByName` rend un sprite de REPLI, jamais nullptr.

🔴🔴 **`grid.tga` EST UN ANNEAU, PAS UNE CROIX — et son centre est TRANSPARENT.**
Mesuré sur capture zoomée le 2026-09-01, après avoir cru le contraire. Le premier
« carreau plein » échantillonnait ce centre : il ne dessinait **rien du tout**, et
le sol paraissait simplement uni. ⚠ Un motif qui n'apparaît pas ne veut pas dire
que le dessin est cassé — regarder d'abord CE QU'ON ÉCHANTILLONNE.

⭐ **Le CARREAU PLEIN = un texel étiré** : `DrawCellQuad` multiplie les UV par les
8 flottants qu'on lui passe ⇒ **le même point aux quatre coins** = un seul texel
sur toute la case = un aplat de la couleur diffuse (donc UNE texture pour les
trois familles de cases). Mais il faut que ce texel soit **opaque**, et aucune
texture unie n'existe dans ce que cite l'exe (alpha_center, whitelight = des
dégradés). Viser un point de l'anneau « au juge » est un pari INVÉRIFIABLE :
`data.grf` de Moonlight est chiffré, `tools/grf_reader.py` le refuse, et le
fichier n'est ni sur disque ni dans moonlight.grf.
⟹ **On fournit la nôtre** : `assets/data/texture/bourgeon_cell.tga`, un carré
blanc opaque de 4 Ko, généré par `tools/gen_cell_tga.py`. Le client lit le DISQUE
avant les GRF : le fichier posé dans `data\texture\` du client suffit à tester,
`moonlight.grf` sert à le distribuer.

🔴 **À PLAT dans `data\texture\`, PAS dans un sous-dossier** (décision utilisateur,
2026-09-01, redite deux fois). Nom PRÉFIXÉ `bourgeon_` quand même : le disque
passant avant les GRF, un « cell.tga » masquerait pour de bon une texture du jeu
qui porterait ce nom, et `data.grf` chiffré interdit de vérifier qu'elle n'existe
pas.
✅ Le chemin passe intact : `UITexture_MakeKeyFromPath` 0x00a9f030 ne réécrit que
les chemins de skin coréens (elle cherche un octet 0xC0) — un nom ASCII ressort
tel quel, et le préfixe `data\texture\` est ajouté par le VFS.

⭐⭐ **La BORDURE par case ne vient pas d'une texture de cadre, mais d'un JOINT** :
`DrawCellQuad` couvre la case ENTIÈRE, donc elle ne peut rien séparer. On refait
son travail (`DrawCellShrunk`, ~50 lignes calquées sur son désassemblage) et on
rapproche les 4 coins de leur centre : le sol reste visible tout autour, et c'est
lui qui trace la bordure — le rendu exact d'un greyworld GRF, avec UN SEUL quad
par case. Les hauteurs ne sont PAS touchées (le carreau doit épouser le relief).
Briques, conventions vérifiées à l'épilogue : `GetCellCorners` 0xa62b70 (retn 0Ch),
`SceneRenderQueue_AcquirePrimRecord` 0x53add0 (retn, tout en ecx),
`World_ProjectPointToScreen` 0x554380 (retn 0Ch), `RenderQueue_InsertPrimitive`
0x550b10 (retn 08h). Sommet = 32 o : xyzrhw, couleur +0x10, uv +0x18 ; biais de
profondeur −3.05e-5 ; record : [0]=sommets, [2]=sprite, [6]=5, [7]=6 (blend).

Second axe, **calculé** : « toutes » / **« contour des obstacles »** = ne garder
que les cases dont un des 4 voisins est d'une autre FAMILLE (marchable / mur /
tirable). Silhouette des murs pour ~10× moins de quads : le vrai levier de perf,
avant la portée.

🔴 **Un damier « une case sur deux » a été RETIRÉ** (rejeté par l'utilisateur, à
raison) : un filtre qui saute des cases sans regarder leur type peut sauter un
MUR, donc **mentir sur le terrain**. Le contour, lui, garde toujours les bords —
c'est ce qui en fait le seul filtre acceptable. Ne pas le reproposer.

## La GAT (relevé dans `C3dAttr_GetCell` 0x00711070)

`*(CWorld + 0x30)` (`gamescene::kAmTerrain`) :
`+0x110` largeur · `+0x114` hauteur · `+0x118` côté d'une case (=5) ·
**`+0x11C` tableau, 20 o par case**, index `x + y*largeur`.
Case : 4 hauteurs de coin (float) puis **`+0x10` = TYPE**.
🔴 Types refusés par `Pathfind_IsStepWalkable` 0x00a78410 : **1 = mur**,
**5 = infranchissable mais TIRABLE** (les sorts et flèches passent). Tout le
reste se marche. Les confondre ferait mentir toute lecture de terrain.
Le tout est dans le foyer `gamescene::` (kTerrainCells, kCellStride, kCellType,
kCellBlocked, kCellSnipeable), pas dans le module.

## Brouillard

`World_ApplyMapFogParams` 0x00c6e5c0 finit par **`SceneRenderQueue vtbl+0x10`
(fog on/off)**. La vtable n'a pas d'adresse statique ⇒ le hook se pose **depuis
la frame de rendu**, en lisant `*(void**)0x012515f8` puis `vt[4]`.
🔴 On mémorise ce que le CLIENT demandait pour le lui rendre : remettre « 1 »
poserait une brume sur les cartes qui n'en ont aucune. Tant qu'on n'a pas
traversé le hook (= pas changé de carte depuis l'activation), on ne restaure
RIEN — la carte suivante rétablit d'elle-même.

## Deux pièges de MOMENT (2026-09-01)

🔴🔴 **`bourgeon_settings.yaml` est relu APRÈS le chargement de la première
carte** ⇒ un réglage qui agit AU CHARGEMENT n'y a rien à faire seul : ses détours
n'existent pas encore quand la carte d'entrée se charge, et il ne prend qu'au
changement de carte suivant — alors qu'entrer en jeu EST un chargement.
⟹ `grey_world::LoadStartupState()` lit `greyworld` + `greyworld_flatten` par
`startup::Section("moonlight_ui")` (qui retombe sur bourgeon_settings.yaml) et
pose les détours, appelé dans `Bourgeon::Initialize` **après la garde de version
du client**. Même chemin que la langue, la police et l'échelle d'UI.

🔴🔴 **Le DÉCOR DE LOGIN est une VRAIE carte** (session spectateur) : elle passe
par les mêmes chargeurs. Dès que les détours existent au démarrage, ils la
frappent aussi et le décor est ruiné. ⟹ garde `Enabled()` =
`g_cfg.enabled && !spectator::Active()` sur TOUTE application (rendu compris) —
`spectator::Active()` couvre la connexion ET la présence dans le monde.

🔴🔴 **`UITextureMgr_Load` 0x00a8d4a0 REND D'ABORD LE CACHE** et ne rappelle le
`Load` de la ressource que si elle n'y est pas. Une carte déjà visitée dans la
session ne repasse donc jamais par `C3dAttr_Load` : son .gat garde son relief
d'origine, alors que le .gnd dessiné est plat ⇒ **on clique contre une géométrie
invisible**, et l'angle de caméra décide. ⟹ `ReflattenLiveAttr()`, une fois par
`MapLoadEpoch`, réécrit le .gat à chaud (il n'est pas pré-transformé, contrairement
au .gnd) en prenant le niveau **dans le .gnd courant** — les deux racontent alors
la même chose quel que soit celui qui est passé par le cache.
⚠ Règle : un détour sur un `Load` de ressource NE SUFFIT JAMAIS chez ce client ;
toujours se demander ce que fait le cache.

## « Le client peut-il recharger la carte lui-même ? » — répondu (2026-09-01)

**Oui, mais ce n'est pas un rechargement.** `CModeMgr_RequestSwitch` **0x00a764e0**
(type voulu en `this+23`, nom de carte en `this+48` ; 22 = courant) est exécuté par
la boucle `CModeMgr_Run` **0x00a756e0**, qui **DÉTRUIT le mode courant** (vtable
slot 0 avec 1) et en **construit un neuf** — `new(0x6F7C)`+`sub_D1E260` pour le
jeu, `new(0x670)`+`sub_C63570` pour le login — puis vtbl+8 (nom de carte), vtbl+4
(entrée), vtbl+12 (boucle).
⇒ En session CONNECTÉE, cela reconstruit tout l'état client sans que le serveur
ne rejoue rien : **inutilisable** pour re-appliquer un réglage. C'est réservé au
hors-ligne — le décor de login s'en sert.
🔴 **Et FAIRE SEMBLANT DE RECEVOIR UN WARP ne marche pas non plus** : le client
rechargerait le terrain, mais il vide sa liste d'acteurs et renvoie un
`loadendack` que rAthena IGNORE — `clif_parse_LoadEndAck` sort d'entrée sur
`sd->prev != nullptr`, le joueur n'ayant jamais quitté la carte côté serveur.
Décor juste, scène vide. (Et `@warp` sur la carte courante ne recharge rien :
`pc_setpos` n'émet `clif_changemap` que si l'index de carte CHANGE.)

✅ **CE QUI MARCHE : reconstruire le terrain SUR PLACE** (`RebuildTerrain`), en
refaisant ce que `CWorld_Load` fait à cet endroit et rien d'autre :
`UITextureMgr_Get` 0x00a90350 (sans arg) → `UITextureMgr_Load` 0x00a8d4a0
(`__thiscall`, retn 4) sur « <carte>.gnd » → l'aplatir → **vtbl+8 `Build(gnd,
0x0159b1a4, 0x0159b1b0, 0x0159b1bc)`** (retn 10h) → **vtbl+52** (retn 4) →
`UITextureMgr_Release` 0x00a8f4b0. Aucun paquet, aucune bascule de mode, aucun
acteur perdu.
⭐ Rejouable parce que `Build` commence par un `Resize(w,h)` (vtbl+4) qui
réalloue ses tableaux.
⚠ Exécuté au début d'une passe de scène (drapeau posé par la case), pas depuis le
dessin d'une fenêtre ImGui.
⛔ **DÉCOCHER ne rend PAS le relief à chaud, et on n'essaie plus.** Une copie des
hauteurs d'origine a été livrée puis **RETIRÉE** (2026-09-01) : elle marchait une
fois et se détruisait au second cycle, parce qu'après un rebuild on
re-sauvegardait un terrain DÉJÀ PLAT comme s'il était l'original. Piège général :
**une sauvegarde prise juste avant une transformation IDEMPOTENTE ne vaut rien si
la transformation a déjà eu lieu** — il faut un témoin d'état, pas un moment.

⭐ **LE WARP SUR PLACE RÉSOUT TOUT D'UN COUP** (décision utilisateur) et ne demande
AUCUN code : mesuré dans `pc_setpos` (moonlight, src/map/pc.cpp ~7587) —
```c
if (sd->prev != nullptr) { unit_remove_map_pc(sd, clrtype); clif_changemap(...); }
```
⇒ le `changemap` part **même vers la MÊME carte**, et `unit_remove_map_pc` retire
le joueur, donc le `loadendack` qui suit est traité normalement. `@warp <carte
courante> <x> <y>` recharge donc tout proprement : décors, terrain, quadtree.
(`sd->state.changemap` ne sert qu'aux notifications de zone, PAS à décider du
changemap — ne pas s'y fier.)
✅ **LIVRÉ le 2026-09-02 : la commande `@refreshmap`.** Serveur : `ACMD_FUNC(refreshmap)`
(atcommand.cpp, juste après `@refresh`) → `pc_setpos(sd, sd->mapindex, sd->x, sd->y,
CLR_OUTSIGHT)`, plus `refreshmap: true` au groupe 0 de **`conf/import/groups.yml`**
(les autres groupes héritent de Player). Client : `PumpMapReload()` dans
`grey_world.cc` arme `ChatWindow::SendTextNow("@refreshmap")`, appelé depuis le hook
de scène **HORS du test `Enabled()`** — le cas qui compte le plus est celui où le
joueur vient d'ÉTEINDRE GreyWorld sur une carte à plat.

⛔ **UN OPCODE CUSTOM (CZ 0x0F33) AVAIT ÉTÉ LIVRÉ PUIS RETIRÉ** le même jour, sur
la question de l'utilisateur « implanter un paquet était-il vraiment nécessaire ? ».
Non : cf. [[project_opcode_system]] § « commande plutôt qu'opcode ». Ne pas y revenir.

🔴🔴 **LE SERVEUR SEUL NE PEUT PAS Y ARRIVER — mesuré le 2026-09-02.**
`ZC_MapChange_Handler` **0x00ccea30** (case 0x0091 du dispatch, appelé depuis
`loc_C9E908`) compare le nom reçu à celui de la carte courante
(**`byte_15FB9AC`**, « <carte>.gat ») et n'a que deux branches :

| `strcmp` | ce qui se passe |
|---|---|
| **≠ 0** (autre carte) | `sub_D76D80` + `".rsw"` + **`CModeMgr_RequestSwitch`
0x00a764e0** + `UIWindowMgr_MakeWindow(6)` = écran de chargement. **LE seul chemin
qui recharge le monde** : .gnd/.gat, quadtree, décors .rsm. Pose `*(this+1548)=0`. |
| **= 0** (même carte) | `GameMode_ResetAndRecreateOwnActor` 0x00c7e5e0 +
`GameMode_OnEnterMapSetup` 0x00c6b870 + `sub_A75820(0)`. On REPOSITIONNE le pantin,
**rien n'est rechargé**. Pose `*(this+1548)=1`. |

⭐ **C'est aussi pourquoi une fly wing ne montre aucun écran de chargement.** Donc
un `pc_setpos` sur la MÊME carte ne rechargera JAMAIS le monde, quoi que fasse le
serveur. L'utilisateur l'avait constaté avant moi : « un warp sur la même map ne
suffit pas, il faut un changement dans une autre puis retour ».

✅ **CONTOURNEMENT SANS PATCHER DE BRANCHE** (`Hooked_ZcMapChange`) : vider
`byte_15FB9AC` juste avant l'appel pour faire diverger le `strcmp`, puis le
RESTAURER juste après — le basculement de mode n'est ici que DEMANDÉ, il se
joue plus tard dans la frame, donc le nom n'a pas encore été réécrit et la
minimap / la navigation ne doivent pas lire du vide.
🔴 **ARMÉ UN SEUL COUP**, par notre propre `@refreshmap`, et désarmé avec le
délai de grâce : forcer à chaque fois infligerait un écran de chargement à
chaque fly wing, chaque `@jump`, chaque téléportation de PNJ sur place.

➡ **Leçon générale** : avant de faire porter un effet par le serveur, vérifier
que le CLIENT ne court-circuite pas le message. Ici l'aller-retour était complet et
correct des deux côtés — seule une comparaison de chaîne, dans le handler, le
vidait de son effet.

⚠ **PAS DE COOLDOWN** (tranché par l'utilisateur) : une fly wing fait exactement le
même `pc_setpos` et se spamme sans frein. Il reste `pc_cant_act`, `pc_isdead` et la
barrière de déconnexion (`battle_config.prevent_logout` contre `canlog_tick`,
**10 s** ici, déclenchée par attaque/skill/dégâts — `prevent_logout_trigger: 14`, pas le
login). Chaque refus RÉPOND dans le chat : c'est ce qui rend la panne lisible, là où
le paquet échouait en silence.

🔴 **TÉMOIN D'ÉTAT, PAS D'ÉVÉNEMENT** : `g_flatten_on_map` dit ce que la carte
AFFICHÉE porte, et c'est sa divergence avec le réglage qui déclenche la demande
et qui allume le message du panneau. Il est initialisé dans `LoadStartupState()`
(la première carte sera chargée avec CES valeurs) puis relatché sur
`MapLoadEpoch()` **quand `IsMapLoading()` est retombé** — donc aussi après un warp
qui n'est pas le nôtre. ⚠ L'époque ne compte que les WARPS (0x0091/0x0092) :
l'entrée en jeu depuis le char-select ne la bouge pas.

🔴 **UN `pc_setpos` RETIRE LE JOUEUR DE LA CARTE ⇒ LES MONSTRES PERDENT LEUR
CIBLE.** Une case à cocher deviendrait un bouton de fuite. Le handler porte donc
la MÊME barrière que la déconnexion (`battle_config.prevent_logout` contre
`sd->canlog_tick`), plus `pc_cant_act(sd)` (script, échange, entrepôt, salon…)
et un cooldown de 5 s. Refus SILENCIEUX : c'est le CLIENT qui dit au joueur que sa
carte n'a pas été rechargée (délai de grâce de 3 s, puis repli sur `RebuildTerrain`).

⚠ `CLR_OUTSIGHT` et non `CLR_TELEPORT` : le joueur ne bouge pas d'une case, jouer
l'effet de téléportation aux voisins leur raconterait quelque chose qui n'a pas
lieu.

## Retiré / non traité

⛔ **Réglage « retirer les ombres portées » : RETIRÉ à la demande de l'utilisateur
(2026-09-01)**, après avoir été livré. Le hook tenait pourtant :
`ChildSprite_DrawShadow` 0x00c49d20 (retn 08h), appelée depuis
`CActorSprite_RenderDispatch`. Ne pas le reproposer. ⚠ `CActorSprite_RenderModelShadow`
0x00c5c280 n'a **AUCUNE référence** (ni appel ni vtable) : code mort, la détourner
ne couvre rien.

✅ **APLATIR LE RELIEF — fait** (levier 5, `flatten`, défaut OFF).
🔴 « Il faut recharger la carte » N'EST PAS une objection recevable : l'utilisateur
l'a tranché (2026-09-01) — « c'est une option cochée une ou deux fois, pour
essayer ou pour adopter ». Ne pas écarter un réglage au motif qu'il n'est pas
instantané.

On réécrit les hauteurs **à leur chargement**, en chaînant les deux Load
(`__thiscall`, `retn 4`) :
- `C3dAttr_Load` **0x00710820** → la .gat, `gamescene::kTerrainCells`, 20 o/case,
  4 hauteurs en tête. C'est ce que lit `Terrain_GetHeightAt` pour poser les ACTEURS.
- `CGnd_Load` **0x00716010** (→ `CGnd_ParseStream` 0x007169e0) → le .gnd, la
  géométrie DESSINÉE. Structure : **+280 largeur, +284 hauteur, +388 tableau,
  28 o/case** = 4 floats de hauteur puis 3 indices de tuile.
🔴🔴🔴🔴 **LA VRAIE CAUSE DES CASES INERTES : LES BOÎTES DU QUADTREE VIENNENT DU
.RSW, PAS DU TERRAIN.** `QuadTree_Rebuild` 0x00a69610 ne calcule ses bornes que
si le .rsw n'en fournit pas ; sinon (cas courant) elle appelle
**`QuadTree_CopyGeometry` 0x00a69b30**, qui recopie telles quelles celles du
FICHIER — `node[5..7]` = min(x,y,z), `node[8..10]` = max(x,y,z), nœud de 0x7C o,
4 enfants en +4..+16, 5 niveaux. Elles portent le **relief d'origine** : aplatir
le .gnd ET le .gat n'y change RIEN, le rayon vise un sol plat pendant que les
boîtes sont restées en altitude.
⟹ **CORRECTIF : rouvrir les bornes de HAUTEUR à ±999999** sur tout l'arbre
(`OpenNodeHeights`, racine = `monde + 88` = `gamescene::kWorldQuadTree`), une
fois par `MapLoadEpoch`. C'est exactement ce que pose `QuadTree_Subdivide`
0x00a69420 quand il n'a rien à copier. Ne toucher QUE Y : X et Z portent le
découpage spatial, qui est juste.
✅ **RÉSOLU le 2026-09-01** — mais il a fallu DEUX bugs de plus, tous deux dans
mon code, et c'est x32dbg qui les a désignés :
1. 🔴🔴 **`C3dGround15 + 8` n'est PAS un pointeur vers le .gnd** : c'est la
   LARGEUR du .gnd en tuiles (150 sur une carte de 300). Déduit à tort de
   `GetCellCorners`, qui lit bien ce champ — mais pour y trouver une largeur.
   `*(char**)(150 + 388)` partait dans le vide, **le `__try` l'avalait**, et comme
   `OpenNodeHeights` était placé EN DERNIER après quatre sorties anticipées, le
   correctif n'a jamais tourné une seule fois.
   ⟹ **Ce qui doit aboutir coûte que coûte passe EN TÊTE du `__try`** : un bloc
   qui couvre plusieurs étapes ne dit jamais laquelle a échoué.
2. le déclenchement sur un compteur (ci-dessous).

🔴🔴 **ET LE CORRECTIF NE TOURNAIT PAS** : il était gardé par
`if (MapLoadEpoch() != g_last_map_epoch)` avec le témoin initialisé à 0 — or
`MapLoadEpoch()` vaut 0 lui aussi tant qu'aucune TRANSITION n'est comptée, et
entrer en jeu n'en est pas une. ⟹ **ne pas se fier à un compteur pour savoir
s'il faut agir : prendre l'ÉTAT lui-même pour témoin.** Ici : « la borne haute de
la racine vaut-elle la nôtre ? » — une comparaison par frame, qui se réarme seule
à chaque chargement (le client y recopie les bornes du .rsw).

📐 **Mesuré à x32dbg** (le seul tour où j'ai cessé de deviner), gonryun 300×300 :
`g_pCurrentMode` **0x0121333c** → CGameMode ; +0xCC → CWorld ; +0x30 → C3dAttr
(+0x110 w, +0x114 h, +0x118 cellsize=5, +0x11C cases) ; **CWorld+0x58 = racine du
quadtree**, bornes à +20 (min) et +32 (max). Le premier octet d'une fonction
détournée doit être **E9** — vérification en une lecture que le hook est bien posé.

⚠ **TROIS explications fausses ont précédé celle-ci** (boîtes dégénérées, jitter
sur le mauvais fichier, cache de ressources). Leçon : quand un correctif ne
change RIEN au symptôme, ne pas empiler une hypothèse de plus — remonter la
chaîne jusqu'à la SOURCE DE DONNÉES et vérifier d'où chaque terme sort vraiment.

🔴🔴 **Un sol exactement plat gêne AUSSI le test fin** (second facteur, réel mais
secondaire) — et j'ai donné UNE EXPLICATION FAUSSE avant la bonne.
Le clic (`GameMode_PickGroundCellUnderMouse` 0x00c69a40) va en deux temps :
1. collecte des nœuds du quadtree traversés par le rayon. **CE N'EST PAS LÀ** :
   `QuadTree_Subdivide` 0x00a69420 initialise les bornes de hauteur d'un nœud à
   **±999999** — une boîte n'est jamais plate. (Ma première explication —
   « boîtes d'épaisseur nulle » — était fausse, et le correctif qui en découlait
   n'a rien changé. Vérifier l'INITIALISATION avant de conclure au dégénéré.)
2. **`sub_711640`** (méthode du **C3dAttr**, donc du **.GAT**) construit **DEUX
   TRIANGLES** sur les 4 hauteurs de la case et teste le rayon contre eux
   (`sub_54AED0`). ⟹ **C'EST ICI.** Des triangles parfaitement horizontaux sont
   PARALLÈLES à un rayon rasant : pas d'intersection.
Les trois symptômes, qui l'ont prouvé sans debugger :
- caméra PERPENDICULAIRE au sol → **tout cliquable** ;
- caméra ISOMÉTRIQUE → des **BANDES** inertes ;
- caméra RASANTE → **rien de cliquable**.
⟹ **CORRECTIF : `kFlatJitter`**, dents de scie sur les 4 coins de chaque case du
**.GAT** (±0.1 sur des cases de 5 unités, invisible). Le **.gnd reste EXACTEMENT
plat** : il ne sert qu'au rendu et aux coins que lit le quadrillage
(`GetCellCorners` prend ses hauteurs du C3dGnd) — y mettre le jitter fait onduler
les carreaux sans rien apporter au clic.
⚠⚠ Leçon : « tourner la caméra change le résultat » ⇒ c'est la GÉOMÉTRIE DU
RAYON. Mais encore faut-il trouver LEQUEL des deux tests géométriques : suivre la
chaîne jusqu'au test FINAL au lieu de s'arrêter au premier plausible.

🔴🔴 **APLATIR LES DEUX, AU MÊME NIVEAU** — n'en faire qu'un ferait flotter ou
enfoncer tout le monde. Le niveau = la MOYENNE des hauteurs, fixée par la .gat
(chargée AVANT le .gnd dans `CWorld_Load`) ; zéro décrocherait le sol du niveau
d'eau du .rsw, qu'on ne touche pas (sur une carte avec eau, le sol peut passer
dessous — effet de bord assumé, dit dans l'infobulle).
⚠ Les deux structures ont des offsets VOISINS et n'ont rien à voir.

## Non traité

Le **ciel et les nuages** : `World_ApplyMapSkyData` 0x0059b810 les fait naître
par `Effect_SpawnAuraPeriodic` à l'entrée de carte. Couper la couleur de fond ne
retire pas les effets déjà nés.

## Coût

Le nombre de quads croît au CARRÉ du rayon (24 ⇒ 2401 cases). Le réglage
« portée » est le seul levier de perf. **Alpha 0 sur une famille de cases = quad
non soumis du tout** : mettre « marchable » à zéro ne laisse que les obstacles,
et c'est la façon la moins chère d'afficher le quadrillage.

Voir [[reference_dx7_dx9_rendering]], [[reference_effect_zorder_render_bucket]],
[[project_graphics_engine_improvements]], [[feedback_native_hooking]].
