# La caméra du monde, et l'écran de veille qui s'en sert

Client 20250716, base `0x400000`, sans ASLR (adresse Ghidra == adresse live).

Deux sujets qu'on ne peut pas séparer : l'écran de veille n'est qu'un client de
la caméra, et la caméra n'avait jusqu'ici aucune fiche — sa connaissance vivait
dans les têtes de `fps_view.cc` et de `screen_fx.cc`, chacune n'en décrivant que
le champ dont elle se servait.

---

## 1. Le rig de caméra

RO est un moteur 3D ordinaire : terrain `.gnd`/`.rsw` et sprites billboardés. La
vue « isométrique » n'est qu'un rig sphérique verrouillé sur un tilt et une
distance. La reparamétrer suffit à obtenir n'importe quel cadrage — rien à
re-rendre, et les billboards continuent de faire face à la caméra.

| Quoi | Où |
|---|---|
| Objet caméra | `*(CGameMode + 0xd0)`, taille `0x1b8` |
| vtable | `g_CCamera_vtable` `0x0104dee4` |
| Constructeur | `CCamera_ctor` `0x00a79b30` |
| Créé par | `CGameMode_EnterWorld` `0x00c733d0` |

### 🔴 Deux poses, et une seule est un levier

L'objet tient une pose **COURANTE** qui **lerpe** chaque frame vers une pose
**CIBLE** (lissage `FUN_00a7ab90`, builder `FUN_00a7ae20` = vtable[4]) :

| | pitch / latitude | yaw / rotation | distance |
|---|---|---|---|
| **COURANT** — écrire ici est VAIN | `+0x2c` | `+0x30` | `+0x34` |
| **CIBLE** — les vrais leviers | `+0x44` | `+0x48` | `+0x4c` |

Écrire dans la pose courante ne tient pas une frame : le lissage la ramène vers
la cible. Ce sont les cibles qui persistent — mesuré en live (x32dbg,
2026-07-01), après une première série d'offsets qui, eux, étaient faux.

Angles en **degrés**. Distance dans l'unité de vue : repos ≈ `278`, plafond
outdoor `400` en vanilla.

### 🔴🔴 Le pitch est NÉGATIF, et se tromper de signe ne se voit pas comme une erreur de signe

`Camera_ApplyViewDistanceClamp` pose la valeur et la borne ainsi (`v18` = 20
dehors, 10 en intérieur) :

```c
if (-45.0 - v18 > v20) v20 = -45.0 - v18;
if (v20 > v18 - 45.0)  v20 = v18 - 45.0;
// => plage [-65, -25] dehors, [-55, -35] en intérieur ; repos -45
```

Le builder pose `eye = lookat + (-dist) * ligne2(RotX(pitch))`, soit
`eye.Y = dist * sin(pitch)` — **sur un axe Y orienté vers le BAS**. Donc :

- pitch **négatif** → `eye.Y < 0` → la caméra MONTE (c'est le cas normal) ;
- pitch **positif** → la caméra descend SOUS le terrain… où le clamp anti-sol de
  ce même builder (`if (eye.Y > Terrain_GetHeightAt(...)) eye.Y = ...`) la
  raccroche au sol.

⚠ **Le symptôme trompe** : avec le mauvais signe, on n'obtient pas une vue
basculée mais une vue **à ras du sol, identique pour TOUTE valeur** — le réglage
a l'air mort, alors qu'il fonctionne parfaitement et se fait écrêter en bout de
chaîne. Une interface qui propose une « hauteur au-dessus de l'horizon »
positive doit donc écrire l'OPPOSÉ.

⚠ `g_cam_rotAngleWork` (`0x012291c8`) est un **faux ami** : malgré son nom, il
alimente le **pitch** (`cam+0x2c` et `cam+0x44`), pas le yaw. Sa valeur live de
`-50` est un pitch, bien dans la plage ci-dessus.

### 🔴 Le yaw se replie tout seul — ne pas le faire à la main

`Camera_LerpCurrentTowardTarget` normalise la CIBLE dans `[0, 360[` puis rejoint
le courant par le **chemin le plus court** (l'écart `j` est ramené dans ±180)
avant de lerper. Une orbite continue n'a donc rien à dérouler : il suffit
d'incrémenter la cible (et de replier son propre accumulateur pour qu'un flottant
ne grandisse pas sans fin). Toucher à la pose courante pour « aider » est inutile.

Le lissage est un dixième de l'écart par frame, sauf pour le point visé, que
l'option `/camera` (`GameSettings_GetFlag(0x36)`) fait coller directement.

Corollaire qui fait tout le confort : **tout mouvement écrit dans les cibles est
lissé par le moteur**. Accélération, décélération et retour en douceur sont
gratuits ; il n'y a jamais à interpoler la pose soi-même — seulement, si l'on
veut, la cible.

### Autres champs

- `+0x20..0x28` : point visé (vec3), recopié de la position du joueur à chaque
  frame. Une orbite reste donc centrée sur le personnage même s'il se déplace,
  sans rien avoir à faire.
- `+0x38..0x40` : copie d'interpolation de la position.
- `+0x80..0x88` : le point de l'œil, tel que passé à `SetView`.

### Atteindre l'objet : une capture, une seule

L'objet n'est à aucune adresse fixe. Le seul accès sûr est de le cueillir dans
un hook : `ragnarok/camera.h` pose un JMP-hook de 5 octets sur
`Camera_ApplyViewDistanceClamp` **`0x00c82340`** (ECX = CGameMode) et lit
`+0xd0`.

🔴 **Un site de 5 octets ne se hooke qu'une fois.** C'est la raison d'être du
module : `FpsView` avait pris le site, et `AfkScreen` l'aurait écrasé sans un
mot. Toute nouvelle envie de caméra passe par `ro::camera::`.

🔴 **`Get()` revalide la vtable à chaque appel.** Le pointeur est mis en cache,
mais l'objet meurt avec son `CGameMode` (changement de carte, retour au choix de
personnage). Or ce qui lit la caméra le fait souvent sans que le joueur touche à
rien — donc sans que le hook ait eu l'occasion de rafraîchir quoi que ce soit.

### ⚠ Le clamp ne tourne pas à chaque frame

`Camera_ApplyViewDistanceClamp` n'est appelée que sur les chemins où le joueur
agit sur la caméra. **En veille, plus personne ne réécrit les cibles** — ce qui
rend un mouvement de caméra automatique remarquablement simple : nos valeurs
tiennent, sans avoir à lutter frame après frame.

C'est aussi pourquoi les globales de distance de vue (`g_cam_viewDistOutdoor`
`0x012291dc`, `g_cam_viewDistIndoor` `0x012291d4`) sont **mortes** hors action du
joueur : elles alimentent le clamp, pas la pose.

### Le plafond de dézoom

`g_cam_zoomMaxOutdoor` **`0x012291c0`** / `Indoor` `0x012291c4`, plancher
`g_cam_zoomMin` `0x012291bc`. ⚠ **Ces globales ne nous appartiennent pas** : le
moteur les réécrit à chaque bascule de `/zoom`
(`GameSettingsCmd_ZoomOut_OnOff` `0x006918c0`, où vit le patch WARP `ZoomMax`).
Se les approprier est l'affaire de `ScreenFx` seul (option « dézoom étendu ») ;
tout le reste **lit** et reste en dessous — au-delà, le far-clip et le brouillard
découvrent les bords du monde.

---

## 2. Effacer toute l'interface du client, sans rien casser

`GameMode_InGame_ProcessFrame` **`0x00c74a80`** dessine dans cet ordre :

```
scene render queue Begin        (g_SceneRenderQueue vt+0x18)
TileQuadTreeNode_Clear
GameMode[52]->vt+0x0C                          ; la scène 3D
0x00c74fc2  call GameMode_DrawMiniMap          ; ← déjà vetoé par Minimap
0x00c74fcc  call sub_A49CC0
0x00c74fd6  call UIWindowMgr_RenderWindows     ; ← le point d'effacement
0x00c74fdb  ...
```

Sauter ce dernier appel efface **d'un coup** les fenêtres du client, les
étiquettes de nom au-dessus des acteurs et les bulles de chat natives — tout ce
que le gestionnaire de fenêtres rend.

`UIWindowMgr_RenderWindows` **`0x00a48fd0`** ne fait QUE dessiner : elle parcourt
sa liste, blitte chaque fenêtre et vide le lot de primitives
(`RenderBatch_FlushPrimitiveListA`). Aucun état de jeu n'en dépend, **le veto est
donc réversible à la frame près**.

⚠ Le détour va sur le **site d'appel**, jamais sur la fonction — même règle que
le radar (`docs/minimap_re.md` §2.0).

### 🔴 Ce qui n'est PAS une UIWindow survit au veto

Le veto n'efface que ce que le gestionnaire de FENÊTRES rend. Tout ce que le
client dessine dans la passe de SCÈNE le traverse intact, et doit être traité un
par un, à la source :

| Élément | Nature | Sort en veille |
|---|---|---|
| Fenêtres, `UIActorNameLabel`, bulles natives | UIWindow | effacés par le veto |
| **Icônes de statut** (buffs/debuffs) | nœuds de sprite de scène — *il n'existe pas de `UIStateIconWnd`* | alpha forcé à 0 dans `RenderIconHook` (features/overlays/status_icon_bar.cc) |
| Curseur | sprite logiciel de scène | `g_cursor_hidden`, voir §3 |
| Numéros de dégâts, effets de sort | scène 3D | **conservés**, volontairement — on veut voir ce qui arrive au personnage |

La règle générale : si l'élément a survécu au veto, c'est qu'il n'est pas une
fenêtre, et son extinction se joue là où sa couleur ou sa visibilité est fixée —
jamais en le détruisant, pour que la frame suivante puisse le redessiner.

### 🔴 Pourquoi PAS le « cacher l'interface » natif (F11)

`UIWindowMgr_ToggleHideAllWindows` **`0x00a47720`** est pourtant tentante :
`Bourgeon::IsNativeUiHidden()` (état à `g_UIWindowMgr + 0x508`, 1 = visible,
2 = caché) est déjà branché sur notre overlay, qui disparaît donc avec elle.
Mais cette fonction :

- **FERME** (`UIWindowMgr_SaveRectAndCloseWindow`) toute fenêtre absente de sa
  liste blanche (`sub_A384C0`) — et une fenêtre fermée ne revient pas ;
- détruit le radar (fenêtre 14) pour le recréer au retour ;
- joue un effet sur l'acteur (`Effect_SpawnA4OnActor`) ;
- **demande deux appels** quand une fenêtre non masquable traîne : la branche
  `case 1` sort sans passer à l'état 2 dès qu'elle a fermé quelque chose. C'est
  le comportement bien connu du premier F11 qui « ne fait rien ».

Tout cela est acceptable pour un geste **volontaire** du joueur. Ça ne l'est pas
pour un basculement **automatique** : partir se faire un café ne doit pas fermer
ce qu'on avait ouvert.

---

## 3. L'écran de veille

`features/gameplay/afk_screen.{cc,h}`. Trois leviers, aucun code de rendu propre.

**L'inactivité** se mesure au seul endroit où toutes les entrées passent : le
hook de WndProc (`ragnarok/ragnarok_client.cc` → `afk::FilterMessage`).
`ProcessPushButton` ne voit que les touches LIÉES à une action, et chaque module
ne voit que ce qui le concerne — une veille bâtie sur l'un d'eux s'endormirait
pendant que le joueur joue.

- Placé **après** le court-circuit des frappes synthétiques (les nôtres, pour
  l'auto-confirm du char-server : les compter retarderait la veille sans que
  personne n'ait rien fait) et **avant** ImGui (pour que le clic de réveil ne
  soit vu par personne).
- `WM_MOUSEMOVE` n'est une entrée que si la position a **changé** : Windows en
  renvoie quand la fenêtre bouge sous une souris immobile.
- Le clic de réveil est **avalé**, son relâchement aussi (masque
  `g_swallowed_buttons`) : un UP vu sans son DOWN est un clic fantôme. Le
  clavier, lui, passe — une touche porte une intention explicite, un clic ne
  vaut que par l'endroit où il tombe, et cet endroit vient de changer.

🔴 **DEUX horodatages, et les confondre casse l'un ou l'autre.** Le réglage
« Réveil » (clavier et souris / clavier seul / souris seule) ne filtre QUE le
second :

| | Ce qu'il retient | Ce qu'il décide |
|---|---|---|
| `LastInputMs()` | toute activité, sans filtre | l'ENDORMISSEMENT |
| `LastWakeInputMs()` | seulement ce qui a le droit de réveiller | le RÉVEIL |

Filtrer le premier ferait s'endormir le client pendant que le joueur promène sa
souris, au seul motif qu'il a demandé un réveil au clavier.

Corollaire assumé : en veille, **ce qui n'a pas le droit de réveiller n'agit pas
non plus**. Une touche qui traverserait sans réveiller lancerait un skill sur un
monde que le joueur ne voit pas, sous un angle qui n'est pas le sien. Le droit
s'accorde à la SOURCE (tout le clavier, ou toute la souris) et non au message :
avaler un appui sans avaler son relâchement laisserait le client croire la touche
encore enfoncée.

🔴 **Un RELÂCHEMENT ne réveille jamais** — c'est la fin d'un geste commencé
avant, pas une intention neuve. Sans cette règle, le raccourci qui LANCE la
veille (action `tool_afk`, catalogue `features/hotkey_actions.cc`) la terminerait
de son propre relâchement une frame plus tard, et passerait pour cassé. Même
raisonnement que le masque `g_swallowed_buttons` côté souris.

**La caméra** avance dans `OnGameFramePulse` (battement de tête de frame), pas
dans `OnRenderUI` : ce dernier est gardé par « interface native masquée » et
« HUD remplacé », sous lesquels la veille doit continuer de tourner.

Le réglage de plongée est offert en degrés **au-dessus de l'horizon** (positif,
comme un joueur le conçoit) et écrit en négatif — voir §1, c'est le piège qui a
coûté une première version où la caméra rasait le sol.

**La teinte** compose une copie de `ScreenFx::fx()` (vignette en `max`, grain en
`max`, désaturation en produit) et la pousse via `D3D9_SetPostFx`. Le réglage du
joueur n'est jamais modifié : le réveil se contente de redemander
`ScreenFx::Apply()`, si bien qu'une veille interrompue de travers ne peut pas
laisser le jeu désaturé. Désaturation par `saturation` et **non** par
`filter = 1` : un filtre est un interrupteur, il ne se fond pas. DX9 uniquement
(`g_imgui_dx7_active`) ; sous le proxy DX7, caméra et masquage fonctionnent, la
teinte est simplement absente.

**Le curseur** — et c'est le piège du chantier. Le drapeau natif
`g_cursor_hidden` **`0x01229448`** semble tout indiqué : c'est bien lui qui décide
du curseur, et `CScene_RenderCellsAndCursor` (`0x00a7b0a0`) le teste avant de
dessiner. **Il ne suffit pas.** En jeu, le curseur est dessiné par un SECOND
chemin :

```asm
0x00c75163  mov  ecx, ebx
0x00c75165  call CursorMgr_RenderSprite   ; ← AUCUNE garde
0x00c7516c  call Actor_DrawDamageNumber
```

Cet appel, dans `GameMode_InGame_ProcessFrame`, est **inconditionnel** : le
drapeau y est ignoré, et l'écrire ne produit rigoureusement rien. Symptôme :
« le curseur reste » alors que le code a l'air juste.

Le vrai levier existait déjà — `Hooked_CursorRender`
(`ragnarok/ragnarok_client.cc`), posé pour le curseur plein écran du login,
**pousse le quad hors du viewport** (offsets `+0x30`/`+0x34` du cursor host,
forcés à `-souris - 4096`) puis restaure avant de rendre la main. Le rendu natif
s'exécute donc normalement — animation, hit-test et notification `UIWindowMgr`
intacts — mais ne se voit pas. La veille se greffe sur cette condition.

⚠ **Et il y en a DEUX.** `DrawROCursorImGui` redessine notre propre copie du
curseur par-dessus les fenêtres ImGui — y compris celles en `NoMouseInputs`.
L'horloge de veille en est une : sans couper aussi ce chemin, il suffirait
qu'elle passe sous la souris pour qu'une flèche réapparaisse au milieu d'un écran
qu'on venait de vider.

**Notre interface** se coupe au dispatch (`Bourgeon::RenderUI`), une fois pour
tous les modules — même règle que la carte du monde : la question « faut-il
dessiner ? » se pose au dispatch, sinon tout module ajouté ensuite oublie de se
la poser.

### Adresses

| Adresse | Nom | Rôle |
|---|---|---|
| `0x00c82340` | `Camera_ApplyViewDistanceClamp` | site de capture de pCam |
| `0x0104dee4` | `g_CCamera_vtable` | valide le pointeur caméra |
| `0x00c74fd6` | *call* `UIWindowMgr_RenderWindows` | site du veto (`E8 F5 3F DD FF`) |
| `0x00c74fdb` | — | l'instruction suivante |
| `0x00a48fd0` | `UIWindowMgr_RenderWindows` | ne fait que dessiner |
| `0x00a47720` | `UIWindowMgr_ToggleHideAllWindows` | F11 — **à ne pas utiliser ici** |
| `0x012291c0` | `g_cam_zoomMaxOutdoor` | plafond de recul (lu, jamais écrit) |
| `0x01229448` | `g_cursor_hidden` | ⚠ ne garde QUE `CScene_RenderCellsAndCursor` — inutile en jeu |
| `0x00c75165` | *call* `CursorMgr_RenderSprite` | le curseur EN JEU, appel inconditionnel |
| `0x00a74410` | `CursorMgr_RenderSprite` | déjà hookée (`Hooked_CursorRender`) |
