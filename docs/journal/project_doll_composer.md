# Pantin composé ro::DrawDoll

> Journal du chantier. La fiche de mémoire `project_doll_composer` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

**Fait (2026-08-02)** : `src/ui/doll.{h,cc}` compose un personnage à partir d'une
simple apparence (`ro::DollLook`), sans acteur en scène ni hook — voir
[[project_spr_act_own_parser]].

**Consommateurs** : char-select (pantin assis + aperçu de création), **aperçu
d'article** (`RenderItemPreviewTooltip`) et **l'avatar de la fiche de
personnage** (`RenderPlayerAvatar`, migré le 2026-08-02).

## Arme et bouclier — on NE résout PAS, on LIT

🔴 `CActorSprite_BuildWeaponLayers` (0x00d403a0) est irreproductible honnêtement :
classe d'arme, « seau » de bouclier, cas particuliers de montures/madogear, et
surtout deux **sondages d'existence de fichier** qui décident du repli.

Donc : `src/ragnarok/own_actor.{h,cc}` → `rag::ReadOwnActorSprites()` lit les
chemins que le client a DÉJÀ résolus sur l'acteur du joueur, et
`ro::DollLook::weapon / weapon_trail / shield` les prend en `DollHeldPiece`
(chemins sans extension). Conséquence assumée : pas d'arme hors session.

🔴 **Toute ressource chargée par `UITextureMgr_Load` (0x00a8d4a0) porte son
chemin résolu** : `{ uint32 empreinte; char chemin[]; }` à **+0x10**, donc les
caractères à **+0x14**. Passé au `_strlwr` du client — qui ne touche qu'à
l'ASCII, les octets CP949 survivent, et le résultat tout-en-minuscules colle aux
noms du GRF. C'est ce même +0x14 qui servait déjà à reconnaître le chariot.

⚠ **Le chemin stocké commence à `sprite\`, PAS à `data\`** (mesuré 2026-08-03) :
`UITextureMgr_Load` ne recolle que le préfixe de TYPE ; le `data\` vient d'une
couche plus basse qu'on court-circuite. Même piège que pour le corps et les
coiffes — les DEUX préfixes sont à poser soi-même. Exemple réel :
`data\sprite\인간족\위저드\위저드_남_1613.spr` (l'arme est nommée par son view
id).

Emplacements de l'acteur : cellules (.spr) `*(actor+0x4ac)[slot]`, images (.act)
`*(actor+0x4b8)[slot]`. Slots **5 = arme, 6 = traînée, 7 = bouclier** (vérifié en
jeu : seul le 7 bouge quand on change de bouclier). Un emplacement n'est retenu
que si les DEUX ressources sont là — un emplacement périmé n'en garde qu'une, et
le dessiner ressusciterait une arme déséquipée.

Ordre de dessin : traînée, arme, bouclier ; le **bloc entier** passe sous TOUT le
personnage (cape comprise, d'où un 3ᵉ tampon `deep` dans le composeur) pour les
orientations de dos {2,3,4,5}.

🔴 **Ancrage : sur le CORPS, aux images COURANTES** — comme la tête (corrigé le
2026-08-03 ; la note précédente « aucun ancrage » était fausse). Je les posais à
l'origine parce que le moteur de capture faisait ainsi et que ça se voyait juste
— juste **par accident** : sur presque toutes les classes l'ancre du bouclier et
celle du corps coïncident et la différence vaut zéro. Sur le Novice elle vaut
7 px en X (corps `8,-50`, bouclier `1,-50`). Le correctif est donc SANS EFFET là
où ça marchait déjà, ce qui le rend sûr — et explique que l'erreur soit restée
invisible.

⚠ Les calques de bouclier sont **masqués** (`sprNo == -1`) hors combat : c'est
une RÈGLE DU JEU, un bouclier n'est visible que pendant l'attaque. Il n'apparaît
que dans deux groupes d'actions — « After hit » 32-39 et « Attacking1 » 40-47.
Un bouclier absent au Repos n'est donc PAS un bug.

🔴🔴 **ROTATION : vraie pour l'arme et le bouclier, fausse pour les huit parties
composites** (corps, tête, coiffes, cape). Corrigé le 2026-08-03 ; l'ancienne
règle « `apply_rotation=false` pour un acteur » était trop générale.
* Les huit parties passent par `Actor_SubmitSpriteQuad` (0x00a1b7c0), qui bâtit
  un rectangle aligné aux axes et IGNORE l'angle → l'honorer faisait pivoter les
  chapeaux sur la tête (commit `4e460cc`).
* L'arme et le bouclier ne passent JAMAIS par cette fonction (ils vivent sur le
  `CActorSprite`). Et pour eux l'angle EST l'animation : le `.spr` d'un bouclier
  ne contient que **DEUX images** (une de face, une de dos) — tout le mouvement
  du bras vient des transformations du `.act`. Symptôme quand on l'ignore :
  bouclier figé et de travers pendant toute l'animation d'attaque, alors même
  que l'action et l'image demandées sont les bonnes (ce que la trace montrait).

Deux options de cadrage ajoutées pour cette vue : `center_on_body` (centre X et
pieds pris sur le CORPS seul, échelle sur la silhouette entière — sinon une arme
à deux mains pousse le personnage de côté) et `scale_limit` (plafond, pour faire
rétrécir le pantin AVEC un effet de costume trop grand).

🔴 **Le moteur de CAPTURE a été SUPPRIMÉ** (commit `fd20872`, −644 lignes) :
`BasicInfo::RenderDoll`, `BasicInfo::DollLook`, `CaptureDollActor`,
`CaptureItemPreviewActor`, les tampons `g_pv_*` et **tout le cache de pantins**
(64 entrées, budget/frame, époque device, péremption, LRU) n'existent plus. Ce
cache n'était pas là pour la vitesse : une couche capturée référence une PAGE
d'atlas + des UV, et l'atlas est un LRU par cellule — d'où le ré-enregistrement
périodique. Le composeur possède ses propres textures et ignore ce problème.
⚠ Il ne subsiste de capture que pour **l'export GIF** (`ExportAvatarGif`), qui a
besoin d'un rendu hors écran de toute façon : `CaptureAvatarActor`,
`EmitWeaponShieldLayers`, `EmitCompanionLayers` ne servent plus qu'à lui.

## Chemins — trois conventions DIFFÉRENTES

| Pièce | Chemin |
|---|---|
| Corps | `data\sprite\<race>\몸통\<sexe>\<job>_<sexe>` |
| Tête | `data\sprite\<race>\머리통\<sexe>\<n>_<sexe>` |
| Coiffe | `data\sprite\악세사리\<sexe>\<sexe><nom>` — 🔴 **PAS** de préfixe de race |

Palettes : `data\palette\몸\body_<n>.pal` et `data\palette\머리\head_<n>.pal`
(unisexes, cf. [[reference_ida_is_vanilla_warp_patches]]).

## 🔴 `<race>` n'est PAS constant — le Summoner (Doram) (2026-08-04)

Corrigé le 2026-08-04 : `인간족` était écrit en dur, donc **aucun Doram
n'affichait de pantin** — corps introuvable ⇒ `DrawDoll` sort avant tout le
reste, la tête comprise. Tout part d'un seul prédicat, **`Job_NeedsLuaItemPosOffset`
(0x00d9cf80)** = « ce job est un Doram » : 4217..4221, 4308, 4315. Il gouverne
QUATRE choses, désormais dans `src/ui/sprite_path.{h,cc}` (`ro::IsDoramJob`,
`ro::RaceFolder`, `ro::SexFolder`) :

1. **Dossier de race** — `Race_GetBodyPrefix6` (0x00b44190) copie 6 octets :
   `도람족` (0x01088C2C) ou `인간족` (0x01088C34). ⚠ Le natif le choisit avec
   **`Job_ResolveBodyClass(...) + 3950`**, pas avec `look.job` ⇒ `BodyResName`
   remonte cette classe effective, et la tête la suit.
2. **Table de coiffures** — 4 `std::vector<char*>` de NUMÉROS de fichier :
   humain 0x015FB30C / 0x015FB318, doram 0x0160052C / 0x01600538 (M/F). Le
   gabarit relatif est `\머리통\%s\%s_%s.%s` (0x01088A6C).
   🔴 Le bornage lu dans l'IDB (repli sur 13, ou 10 en doram) est **VANILLA** :
   `Allow65kHairs` le neutralise et greffe un itoa pour les index ≥ 43 ⇒ table
   pour < 43, numéro brut au-delà. Cf. [[reference_ida_is_vanilla_warp_patches]].
3. **Couvre-chefs** — variante `악세사리\<sexe>_doram\<sexe><nom>` (0x01088A80),
   que `Hair_BuildHeadgearSpritePath_impl` (0x00b41d90) SONDE
   (`UITextureMgr_ResourceExists`) avant de retomber sur le gabarit commun. D'où
   `Attached::cand[4]` dans `doll.cc` — la liste d'essais a remplacé le couple
   `base`/`alt`, une coiffe de Doram ayant 4 candidats (race × casse).
4. **Palettes — un dossier PAR RACE, jamais partagé (MESURÉ 2026-08-04).**
   `Job_BuildBodyPalettePath_impl` (0x00b42580) et
   `Hair_BuildHeadPalettePath_impl` (0x00b42db0) ont CHACUNE deux gabarits, dont
   un doram à préfixe de race. `BodyPalUnisex` traite le corps doram en gardant
   ce préfixe (`도람족\body_<n>.pal`) ; `HeadPalUnisex` **ignore** la tête doram
   ⇒ patch `DoramHeadPalShared` (`WARP0716/Scripts/Patches/SharedPalDoram.qjs`,
   auteur Stingor) ⇒ `도람족\머리\head_<n>.pal`. Il exige un `HeadPal*` actif
   (sinon `IncrHairs` hooke la même chaîne et écrase la redirection) et suit sa
   saveur MF/Unisex. `SharedPal.qjs` (amont) n'est PAS touché — y rouvrir le tag
   `SHP` plante le patcher (0xC0000005, cf. WARP0716#22).
   🔴 Fusionner les races est IMPOSSIBLE, et il n'existe aucun `.pal` doram
   officiel — tout le dossier (rampes, génération, pièges CP1252 et 1028 octets)
   est dans [[reference_doram_palettes]]. ✅ Résolu et validé en jeu 2026-08-04.

🔴 **`DollLook::body` est la CLASSE, pas un « style »** (tranché le 2026-08-03).
`Job_ResolveBodyClass(job, body, 1)` (0x00d99150) ne lit `job` que pour
reconnaître un BÉBÉ ou une MONTURE ; c'est **`body`** qu'il remappe et qu'il
rend. Le laisser à 0 demande la classe 0 → **tout le monde en Novice**, quel que
soit `job`. C'était le cas de l'avatar ET de l'aperçu d'article jusqu'à ce que
l'utilisateur le repère. En jeu : `g_Own_JobId` = **0x015fb9c8** (= session +
5640, la valeur brute que `Job_ResolveMountedClassFromOption` 0x00d5b580 lit
avant d'y appliquer la monture). Au char-select : CHARACTER_INFO +0x58. Un style
de corps équipé y arrive sous son propre id (4332..4344), remappé par le natif.

🔴 **Le nom de sprite du corps** = `Job_ResolveBodyClass(job, body, 1)` puis ce
même index dans le tableau à `0x015FF634`. PAS `Job_GetDisplayNameOrResName`,
qui rend le nom D'AFFICHAGE (« High Wizard ») pour un personnage. Et c'est le
**body style** qui choisit le sprite, pas le job.
Coiffes : tableau à `0x015FF658` (indexé par view id). Casse variable → essayer
le nom tel quel puis en minuscules (n'abaisser QUE l'ASCII : le 2ᵉ octet d'un
caractère CP949 peut tomber dans `A`-`Z`).

## 🔴 Animation — DEUX horloges, c'est là qu'est tout le piège

**1. L'ACTEUR** (`acteur+0x38` = action, `+0x3c` = image). Mesuré au débogueur :
l'image ne défile qu'en Marche (1) et Combat (4) ; en Repos (0) et **Assis (2)**
elle reste à 0. Corps et tête sont donc figés sur un pantin de char-select.

**2. LES ACCESSOIRES**, qui s'animent QUAND MÊME — port de
`Act_ResolveAltAnimFrame` (`0x00d83a40`) :

```
nRef   = images de la TÊTE pour cette action
nPiece = images de la PIÈCE pour cette action
si nRef >= nPiece ou nPiece % nRef  ->  pas d'animation alternative
mult   = nPiece / nRef
image  = image_acteur * mult + ((écoulé_ms / 24) / delay) % mult
```

Une pièce porte un MULTIPLE exact des images de la tête et parcourt son
SOUS-GROUPE au fil du temps réel. ⚠ Diviseur **24**, pas 25, et `delay` est la
cadence BRUTE du .act (pas des ms) : 4.0 → 96 ms, 8.0 → 192 ms. Avec 24 images
pour 3 de tête, `mult = 8` → la pièce joue 0..7, **jamais ses 24**.

🔴 **Pourquoi `BasicInfo::RenderPlayerAvatar` anime parfaitement sans rien de
tout ça** : il ne compose pas, il fait rendre un vrai acteur par le CLIENT et
capture les quads — donc le client appelle cette fonction pour lui. Dès qu'on
compose soi-même, il faut la refaire.

J'ai fait **sept** réglages tâtonnants (compteur partagé, compteur propre,
cadence du corps, cadence de la pièce, modulo, plafonnement, troncature) avant
de chercher la bonne fonction. Aucun ne pouvait tomber juste.

## Autres pièges déjà payés

- 🔴🔴 **ANCRAGE — QUATRE RÉGIMES** (élargi le 2026-08-03 ; deux notes
  antérieures étaient trop générales). Sources : `Actor_DrawSprites` 0x007AC820
  et **`Actor_ComputeHeadAttach` 0x007ADBC0**.
  | Pièce | Référence | Images |
  |---|---|---|
  | **Tête** (partie 2) | le **CORPS** (partie 1) | ancres des images **COURANTES** des deux |
  | Coiffes (parties 3-6) | la **TÊTE** (partie 2) | ancres des images **COURANTES** des deux |
  | **Cape** (partie 7) | le **CORPS** | ancre de l'image **0** |
  | Arme / bouclier | aucune | leur `.act` place déjà la main |
  🔴 La TÊTE est passée à l'image courante le 2026-08-03 : symptôme signalé par
  l'utilisateur = « tête et corps désynchronisés en **combat** ». Le corps
  s'élance de plusieurs pixels d'une image à l'autre ; une tête recalée à
  l'image 0 gardait un décalage constant et décrochait du cou. En marche le
  défaut ne se voyait pas — le `.act` de la tête porte son propre balancement.
  `Actor_ComputeHeadAttach` est formel : `partie1.ancre − tête.ancre`, action ET
  image COURANTES, si les `attr` coïncident.
  ✅ La cape reste sur l'image 0, VALIDÉE en marche : le natif ne lui donne
  AUCUN décalage (dans `Actor_DrawSprites` la branche par défaut laisse
  l'accumulateur à 0), et son `.act` porte lui-même son mouvement.
  ⚠ Ne généraliser NI l'un NI l'autre. Au repos tous les régimes coïncident
  (`SpriteFrameAnchor` force l'image 0 pour les actions < 8), d'où des défauts
  invisibles tant que le corps était figé.
  ⚠ Il faut aussi reporter le DÉCALAGE de la pièce de référence, sinon l'enfant
  atterrit à l'origine du corps.
- **Cadrage** mesuré sur l'image 0 : le mesurer sur les quads dessinés fait
  grossir et rétrécir tout le pantin. 🔴 **Et ça vaut aussi pour le DÉCALAGE**,
  pas seulement pour les quads : dès qu'une ancre suit l'image courante, la
  boîte englobante la suit aussi. D'où deux décalages par pièce dans le
  composeur — `last_dx` pour dessiner, `last_rdx` pour mesurer. Le défaut ne se
  voyait pas tant que seul l'aperçu d'article animait (il cadre en
  `fit_body_only`, donc les pièces n'entrent pas dans la boîte).
- **Rotation des calques** : `Actor_SubmitSpriteQuad` l'IGNORE (rectangle aligné
  aux axes) → `SpriteResolveFrame(..., apply_rotation=false)` pour un acteur.
- **Attache** : n'appliquer que si les deux ancres ont le même `attr`.

## La CAPE (garment) — fait le 2026-08-02

🔴🔴 **PIÈGE DE NOMMAGE DE L'IDB, corrigé** : la famille `Robe_*` désignait le
**BOUCLIER**, pas la cape. `Robe_ResolveSpriteFilename` compose des chemins en
`방패\…` (bouclier) et son seul appelant est le constructeur du **slot 7**, déjà
documenté comme le bouclier ([[project_weapon_zorder]]). Renommés :
`Shield_ResolveSpriteFilename` 0xb44b80, `Shield_HasJobSpecificVariant` 0xd72190,
`Shield_GetResName` 0xd84780 (table `0x015FF664` = noms de **BOUCLIER**),
`Shield_ResolveViewIdFromItemId` 0xd84850, `CActorSprite_BuildShield_Slot7`
0xd401d0. Et `Job_GetGarmentPalettePath` 0xd84680 → `Job_GetGarmentActPath` :
elle passe l'index 0 = extension `act`, **il n'y a aucune palette de cape**.

**La vraie cape** = `Job_BuildGarmentSpritePath_impl` 0x00b442f0 (via les
enveloppes `Job_GetGarmentSpritePath` 0xd846f0 / `Job_GetGarmentActPath`) et
`CActorSprite_DrawGarmentLayer` 0x00d36430.

| Extension | Chemin |
|---|---|
| `.spr` | d'abord `data\sprite\로브\<robe>\<robe>.spr` ; **si absent** → celui d'en dessous |
| `.act` | **TOUJOURS** `data\sprite\로브\<robe>\<sexe>\<classeDeCorps>_<sexe>.act` |

⚠ Ces gabarits (`0x01088B88` plat, `0x01088BBC` par classe) portent **déjà**
leur `sprite\` — contrairement à ceux du corps et des accessoires. Préfixe à
poser = `data\` seul. ⇒ d'où `ro::LoadSpritePair(spr_base, act_base, …)` :
une cape peut apparier un `.spr` partagé à un `.act` par classe.

🔴 **Le nom de robe ne sort d'AUCUNE table C** : global Lua
`ReqRobSprName(viewId)` (`Lua_GetReqRobSprName_ByRobeId` 0xb44a10). Données dans
`System\spriterobename.lub` : `RobeNameTable` + **`RobeTopLayer`**.

🔴 **Devant ou derrière = décision LUA, pas une constante.**
`CActorSprite_DrawGarmentLayer` est appelée **deux fois par frame** (une par
passe) et ne dessine que si `Actor_GetGarmentDrawOnTopPass` 0xd36d60 égale la
passe. Celle-ci appelle `DrawOnTop(idCape, sexe, job, action, image) → bool`
(ou `_New_DrawOnTop` si `*0x015FB2A4`). Sortie initialisée à **1 avant l'appel**
⇒ Lua muet = DEVANT. ✅ **Ordre des 5 arguments VALIDÉ EN JEU (2026-08-02)** :
il avait été déduit des emplacements de pile du décompilé — la cape passe bien
devant et derrière selon l'orientation.
⚠ Mémo à **64 entrées** pour cette réponse : elle dépend de l'orientation ET de
l'image (8 × 8), une table plus courte se ferait écraser en boucle en marche.

**Animation** : même `Act_ResolveAltAnimFrame`, référence = **slot 1 = la TÊTE**
(`CActorSprite_BuildHead_Slot1`) — donc la cape se traite comme un accessoire de
plus dans la liste, après la tête.

✅ **CONFIRMATION EN DUR de la règle des deux horloges** : juste avant l'appel,
le client fait `if (action < 8 || action - 16 <= 6) frame = 0` — image FORCÉE à 0
en pose debout et assise. Et `acteur+0x38` = action, `+0x3c` = image, exactement
ce qui avait été mesuré au débogueur.

**Refus à reproduire** : `robeId <= 0`, nom Lua vide, poses **64..71** (type
d'action 8), et jobs **4086 / 4087 / 4112** au **féminin**. Les DEUX fichiers
doivent exister, sinon rien n'est dessiné.

**WARP** : aucun des ~120 patchs appliqués ne touche les chemins de robe — le
désassemblage fait donc foi ici, contrairement aux palettes de corps et de tête.

⚠ Deux appels Lua par pantin et par image = >1000 pcall/s au char-select :
**mémo obligatoire** (table de 16 à écrasement circulaire dans `doll.cc`), comme
`HatOrdinalParams` de basic_info.

## Le CORPS s'anime (2026-08-02)

`actor_frame` n'est plus 0 en dur : il défile en **Marche (1)** et **Combat (4)**
via `SpriteFrameIndex` (cadence du `.act`). Ailleurs il reste 0.
- Le corps se résout **DEUX fois** : image 0 pour MESURER, image courante pour
  DESSINER — comme les pièces rapportées. Une seule résolution remettrait la
  boîte englobante sur les quads dessinés ⇒ le pantin respire.
- `actor_frame` **entre** dans la formule des accessoires
  (`image = actor_frame * mult + sous-image`) : les deux horloges se composent.
- Plafonnement par pièce (`min(image, nb - 1)`), comme le site.

⚠ **Ce que la mesure a éliminé** (LogDiag posé exprès, 2026-08-02) : ni
repliement d'action (`pose % nb_actions` — les `.act` ont 104 actions, la marche
n'est jamais repliée), ni plafonnement d'image (8 images partout). Les deux
hypothèses « évidentes » étaient fausses ; c'était l'ANCRAGE.
Cf. [[feedback_re_method]].

**How to apply:** avant de régler une animation ou une géométrie de sprite,
LIRE l'acteur vivant au débogueur ([[reference_x32dbgmcp_bridge]]) ou le `.act`
sur disque en Python. Déduire une règle plausible coûte plus cher que mesurer —
cf. [[feedback_debug_tooling]].

## Arme, bouclier, ordre et cadrage (2026-08-03) — fiche de personnage LIVRÉE

La fiche était le dernier client du moteur de capture. Elle passe sur
`ro::DrawDoll`, et le White Smith — seule classe dont la tête décrochait — est
réglé par ces correctifs. **Plus aucun défaut connu.**

🔴 **Correction d'une mémoire fausse ci-dessus** : « plafonnement par pièce
`min(image, nb-1)`, comme le site » est FAUX pour le client. `Act_GetFrame`
(0x0070f4b0) rend `*begin`, donc l'**image 0**, quand l'index dépasse. Le
plafonnement sur la dernière image venait du site Moonlight, pas du binaire —
et c'est ce qui décrochait la tête des classes dont le `.act` a moins d'images
que le corps.

**Ancrage des pièces TENUES : aucun.**
`CActorSprite_ComputePartAttachOffset` (0x00605170) est un switch à trois cas —
partie 1 (tête) : ancre du corps ; parties 2, 3, 4, 8 (coiffes, cape) : ancre de
la tête ; **default, qui contient 5 arme, 6 traînée, 7 bouclier : zéro**. Elles
sont posées à l'origine de l'acteur, comme le corps. ⚠ J'avais cru l'inverse sur
un bouclier de Novice mal placé — le vrai défaut était l'ANGLE du `.act`.

**L'angle du `.act` s'applique aux pièces tenues, et à elles seules.** Un `.spr`
de bouclier n'a que DEUX images (face et dos) : tout le mouvement du bras est
porté par les transformations du `.act`. Les huit parties composites, elles,
passent par `Actor_SubmitSpriteQuad` qui ignore l'angle.

**Table d'ordre**, partagée avec le monde ([[project_weapon_zorder]]) :
```
DE DOS  : bouclier · CORPS · tête · coiffes · arme · cape
DE FACE : CORPS · tête · coiffes · cape · arme · bouclier
```

**Classe du corps** : `DollLook::body` doit recevoir `g_Own_JobId`
(**0x015fb9c8**) — laissé à zéro, tout le monde s'affiche en Novice, sur
l'avatar comme dans l'aperçu marchand.

**Cadrage — deux options ajoutées, et elles vont ensemble :**
* `fit_span` : enveloppe MAXIMALE, toutes images × 8 orientations. Sans elle,
  l'échelle est mesurée sur l'image affichée et **saute quand on tourne** (arme
  et bouclier s'écartent de face, se replient de dos).
* `min_body_height` : PLANCHER de stature sur la hauteur du CORPS. Beaucoup de
  coiffes de costume posent leur sprite **À CÔTÉ** du personnage (compagnon,
  monture) : tout faire rentrer le réduit de moitié sous un grand vide. Passé le
  plancher on laisse déborder. La fiche demande 0,50 × hauteur du cadre.
* ⚠ `center_on_body` coûte la MOITIÉ du cadre quand une pièce est latérale — la
  demi-largeur est mesurée depuis le centre du corps, donc autant d'espace est
  réservé du côté vide. Avec `fit_span` il ne recale plus que les PIEDS.
