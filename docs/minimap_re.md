# Minimap, grande carte et carte du monde — reverse engineering

Client `2025-07-16 Ragexe` (base `0x400000`). Relevé le 2026-08-14, IDB
`2025-07-16_Ragexe_175220998_clientinfo.exe.i64`, vérifications live sur le
client ouvert (x32dbg, écran 1760×990).

> 🔴 **Le piège central de ce sujet** : le petit radar du coin haut-droit n'est
> **pas** dessiné par une `UIWindow`. C'est un quad du render-queue 2D posé par
> `CGameMode` entre la scène 3D et l'interface. La fenêtre `UIMinimapZoomWnd`
> (id **14**) posée par-dessus n'est qu'un **cadre transparent** portant cinq
> boutons et le texte des coordonnées. Chercher le dessin de la carte dans son
> `DrawContent` ne donne rien : il remplit la surface de magenta.

---

## 0. Les trois fenêtres, d'un coup d'œil

| Écran | Classe RTTI | id | slot mgr | ctor (`new`) | Taille |
|---|---|---|---|---|---|
| **Radar** coin haut-droit | `UIMinimapZoomWnd` | **14** (`0x0E`) | `+0x1C0` | `0x0088e340` (`0xC8`) | `SetSize(128, 140)` |
| **Grande carte** (plein carré 512) | `UIMiniMapWnd` | **273** (`0x111`) | `+0x4F0EC` | `0x00960f30` (`0x16C`) | 516 × 559 par défaut |
| **Carte du monde** | `UIRoMapWnd` | **140** (`0x8C`) | `+0x400` | `0x008d7910` (`0x228`) | — |

Les noms sont **contre-intuitifs** : `UIMiniMapWnd` est la **grande** carte,
`UIMinimapZoomWnd` est le **petit** radar. Ils viennent du RTTI (COL à
`vtable-4`), pas d'une supposition.

| Classe | vtable | typedesc |
|---|---|---|
| `UIMinimapZoomWnd` | `0x0103475c` | `0x0123fb30` |
| `UIMiniMapWnd` | `0x0103e880` | `0x01240af8` |
| `UIRoMapWnd` | `0x01038140` | `0x0123ff7c` |
| `UINavigationWnd` | `0x01038728` | `0x012400a4` |

Globaux dédiés (⚠ non nuls ⇔ fenêtre ouverte **en ce moment**) :

```
0x0131f6a8  g_MinimapZoomWnd   fenêtre 14  (radar)
0x0136e5d4  g_MiniMapWnd       fenêtre 273 (grande carte)
```

Vérifié en jeu : `g_MinimapZoomWnd = 0x25DE1D48` (vtable `0x0103475C`,
`+0x2c = 14`, `+0x14/+0x18 = 128/140`, `+0x1c/+0x20 = 1615/17`),
`g_MiniMapWnd = 0` (grande carte fermée).

---

## 1. Le radar — `UIMinimapZoomWnd`, id 14

### 1.1 Création et position

`UIWindowMgr::MakeWindow` case 14 @ `0x00a3caf4` :

```c
new(0xC8) -> UIMinimapZoomWnd_ctor(0x0088e340)
UIWindow_SetSize(wnd, 0x80, 0x8C);                       // 128 × 140
SetPos(*(g_SceneRenderQueue)+0x28 - 0x91, 0x11);         // largeurÉcran-145, y=17
```

`g_SceneRenderQueue` = `0x012515f8` (un **pointeur**) ; `+0x28` = largeur de
l'écran, `+0x2C` = hauteur. Live : `0x6E0 × 0x3DE` = 1760 × 990 → `posX = 1615`. ✓

### 1.2 Ses cinq boutons — `UIMinimapZoomWnd_CreateControls` @ `0x008a9220`

Bitmaps `유저인터페이스\minimap\i_<nom>_1.bmp` / `_2` / `_3` (normal / survol /
enfoncé), tous à **y = 128**, c'est-à-dire juste sous le carré de carte.

| nom | cmd | x | infobulle | effet |
|---|---|---|---|---|
| `object` | 452 | 1 | `MSI_MINIMAP_TIP_POS_ON` (0xB27) « Show information » | bascule `g_MiniMapShowObjects` |
| `plus` | 216 | `w-55` | `MSI_MINIMAP_TIP_ZOOM_IN` (0xB28) | `g_MiniMapZoom *= 4/3`, **plafond 4.0** |
| `minus` | 217 | `w-41` | `MSI_MINIMAP_TIP_ZOOM_OUT` (0xB29) | `g_MiniMapZoom *= 3/4`, **plancher 1.0** |
| `mini` | 189 | `w-27` | `MSI_MINIMAP_TIP_MAP_VIEW` (0xB2A) « Maximize » | bascule la fenêtre **273** |
| `viewon` | 454 | `w-13` | `MSI_MINIMAP_TIP_WORLD_VIEW` (0xB2B) | bascule la fenêtre **140** |

🔴 Le bouton `mini` est **gaté** : si `sub_D71F30(g_UIWindowContextKey)` rend
faux, la grande carte est refusée avec `MSI_MINIMAP_MSG_16` (0xACF) — « In this
map You can't memorize and zoom the minimap ». C'est le drapeau `NOMEMO`/
`NOMINIMAP` de la carte côté serveur.

`CreateControls` finit par un `OnMsg(6, 502)` sur elle-même, puis pose un bouton
par objet de la carte (`UIMinimapZoomWnd_AddObjectMarkerButton` @ `0x008d1fb0`).

**`OnMsg` @ `0x008c5080`** (`UIMinimapZoomWnd_OnMsg`) traite en plus :

- **502** — applique `TT_SHOW_MINIMAP_BUTTON_ONOFF` (**index 223**) à tous les
  boutons de `this+47/48`. C'est le point d'entrée de
  `GameSettingsCmd_ShowMinimapBtn_OnOff` @ `0x00693410`.
- **453 / 455** — survol / sortie d'un bouton d'objet (poussé par le moteur, cf. §3).
- **défaut ≥ 1000** — `CNavigation_SearchRoute` vers l'objet d'index `cmd-1000`.

### 1.3 `DrawContent` @ `0x008b4ff0` — il ne dessine PAS la carte

```c
UIWindow_ClearSurface(this, 0xFFFF00FF);       // magenta = couleur de transparence
Scene_WorldPosToCellXY(...)                    // cellule du joueur
UIWindow_DrawText(this, 19, 128, " %d  %d", …, 0x000000);   // ombre
UIWindow_DrawText(this, 20, 128, " %d  %d", …, 0xFFFFFF);   // texte
```

C'est tout. Le fond de carte et les marqueurs viennent d'ailleurs → §2.

---

## 2. Le vrai rendu du radar — `GameMode_DrawMiniMap` @ `0x00c66ab0`

Appelée **une fois par frame** depuis `GameMode_InGame_ProcessFrame`
(`0x00c74fc2`), dans cet ordre :

```
scene render queue Begin        (g_SceneRenderQueue vt+0x18)
TileQuadTreeNode_Clear
GameMode[52]->vt+0x0C           (rendu de la scène 3D)
GameMode_DrawMiniMap(this)      ← ICI
sub_A49CC0(mgr)
UIWindowMgr_RenderWindows(mgr)  ← les fenêtres passent PAR-DESSUS
```

🔴 **Garde d'entrée : `if (g_MinimapZoomWnd)`.** Pas de fenêtre 14 ⇒ pas de
radar du tout, même si tout le reste est prêt.

### 🔴 2.0 Le supprimer sans clignotement : vetoer le SITE D'APPEL

Fermer la fenêtre 14 depuis un battement en tête de frame **ne suffit pas**, et
ce n'est pas une question de cadence : le client la recrée au MILIEU de sa frame
(traitement des paquets d'entrée de carte) et dessine le radar plus loin dans
CETTE MÊME frame. Quel que soit le battement, il est déjà passé. Il restait donc
une frame pleine de radar natif à chaque téléport — bien visible au sortir d'un
écran de chargement.

Le seul point qui soit à coup sûr **après toute création et avant tout dessin**
est l'appel lui-même, `0x00c74fc2`. Bourgeon y pose un JMP-hook (5 octets,
`E8 E9 1A FF FF`) et saute l'appel quand sa minimap remplace le radar. Trois
raisons pour lesquelles c'est sans risque :

- sauter l'appel équivaut **exactement** à la garde d'entrée ci-dessus, que le
  client prend lui-même dès que la fenêtre 14 n'existe pas ;
- **tous** les marqueurs partent d'ici : les xrefs de
  `GameMode_DrawMiniMapMarker` et de `GameMode_DrawMiniMapPartyGuildQuestMarkers`
  ne mènent nulle part ailleurs ;
- la fonction ne fait que dessiner — aucun état de jeu n'en dépend.

⚠ **Le détour va sur le site d'appel, PAS sur la fonction** : le prologue de
`GameMode_DrawMiniMap` installe un cadre SEH (`push -1` / `push handler` /
`mov eax, fs:0`), que le JMP-hook ne sait pas relayer.

Reste le cadre de la fenêtre 14 (coordonnées + cinq boutons), rendu **après**
ce point par `UIWindowMgr_RenderWindows` : le détour le masque au passage
(`+0x28 = 0`, une simple écriture de champ). Sa destruction, elle, est une
commande du client et attend le battement de frame suivant.

### 2.1 Géométrie et cadrage

```c
mapW = *(carte + 0x110);   mapH = *(carte + 0x114);   // en CELLULES
Scene_WorldPosToCellXY(this, acteur->x, acteur->z, &px, &py);

demi = 1.0f / (g_MiniMapZoom * 2.0f);
u0 = px/mapW - demi;   u1 = px/mapW + demi;
v0 = (mapH-py)/mapH - demi;   v1 = (mapH-py)/mapH + demi;
// bornage à [0,1] EN CONSERVANT la largeur 2*demi (on ne rétrécit jamais la vue,
// on la décale — sauf si 2*demi > 1, auquel cas on colle à [0,1])
```

- `g_MiniMapZoom` = **1.0** ⇒ `demi = 0.5` ⇒ la carte **entière** tient dans le
  carré. C'est le zoom par défaut, vérifié en jeu.
- `g_MiniMapZoom` = 4.0 (maximum) ⇒ un huitième de la carte en largeur.

Rect écran du quad : **`x ∈ [largeurÉcran-144, largeurÉcran-16]`,
`y ∈ [16, 144]`** — soit 128 × 128. La fenêtre 14, elle, est en
`(largeurÉcran-145, 17)` : le quad est décalé de **(+1, −1)** par rapport à
l'origine de la fenêtre.

### 2.2 La texture

```c
sauve = g_TextureDownscaleFactor;  g_TextureDownscaleFactor = 1;
tex = SpriteRes_GetOrLoadByName(&g_SpriteTexFactoryCache, GameMode+0x90, 0, 0);
…
g_TextureDownscaleFactor = sauve;
```

🔴 Le facteur de réduction de texture est **forcé à 1** le temps du chargement :
la minimap ignore volontairement le réglage de qualité de texture. `GameMode+0x90`
porte le nom de fichier (une `std::string`), rempli à l'entrée de carte.

Le quad part au render-queue par `sub_A74E90(rect, rotation, z, couleur, tex, u0,v0,u1,v1)` :

- `z = 0.0998f` pour le fond de carte, `0.0999f` pour les marqueurs (tri par
  profondeur du bucket 2D, cf. `project_effect_zorder_render_bucket`) ;
- `couleur = g_MiniMapTintColor` (`0x0131f544`). **Relevé en jeu :
  `0xAAFFFFFF`** — alpha 170/255, d'où la semi-transparence du radar. L'IDB ne
  montre qu'une **lecture** de ce global : l'écriture n'est pas résolue, ne pas
  supposer qu'il vaut `0xFFFFFFFF`.

### 2.3 Les marqueurs — `GameMode_DrawMiniMapMarker` @ `0x00c685c0`

⚠ Le prototype de l'IDB décale les arguments (`this` est bien `ECX` =
`CGameMode`, et la `std::string` passée **par valeur** occupe 0x18 octets de
pile). Signature réelle, relevée au désassemblage :

```c
void __thiscall CGameMode::DrawMiniMapMarker(
    float uv[4], int cellX, int cellY, int demiTaille,
    std::string bmp, int genre, D3DCOLOR couleur, bool collerAuBord);
```

| genre | rendu | poussé vers la grande carte |
|---|---|---|
| 0 | bitmap simple | — |
| 1 | pastille pleine (halo `0xAAFFFFFF` de rayon+1, puis `couleur`) via `MiniMap_DrawFilledDot` `0x00c67130` | `OnMsg 483` index **1** |
| 2 | carré plein bordé de `0x99FFFFFF` | `OnMsg 483` index **0** |
| 3 | croix (deux quads de 4×`demiTaille`) | — |
| 4 | bitmap **pivoté** de `180 − angle(acteur+0x4C)` — la flèche du joueur, dessinée deux fois (ombre `0x77333333` puis `couleur`) | `OnMsg 481` (x,y) quand la cellule change |
| 5 | bitmap simple | `OnMsg 482` (x,y) quand la cellule change |
| 6 | rien en propre | `OnMsg 453/455` à la **fenêtre 14** pour placer/masquer un bouton d'objet |

🔴 `collerAuBord = 1` : un marqueur sorti du cadre est ramené sur le bord **et
son bitmap est remplacé** par `유저인터페이스\basic_interface\quest_arrow%d.bmp`
(`%d` = le côté, 1..4) — sauf pour le genre 5. C'est le mécanisme de la flèche
directionnelle de quête.

### 🔴 Le piège de la rotation (genre 4)

`sub_A74E90` place les coins d'un quad pivoté ainsi :

```
x = cx + u·sin(θ) − v·cos(θ)
y = cy + u·cos(θ) + v·sin(θ)
```

**Ce n'est pas la rotation d'écran usuelle** : elle vaut une rotation de
**`90° − θ`**, pas de `θ`. Recopiée telle quelle dans une surface Y-vers-le-bas
(ImGui, par exemple), elle **miroite** le sprite — l'échange `θ ↔ 90−θ` est une
réflexion dans l'espace des angles.

Le symptôme est reconnaissable et se vérifie au chiffre près : la flèche ne
pointe juste qu'au **nord-est et au sud-ouest**. Avec la formule du moteur elle
vise `(−cos A, −sin A)`, avec la rotation usuelle `(sin A, cos A)` ; les deux ne
coïncident que si `−cos A = sin A`, soit `A = 135°` et `A = 315°`. Un demi-tour,
lui, ne laisserait **aucune** direction correcte — c'est ce qui distingue un
miroir d'une inversion, et ce qui permet de trancher sans debugger.

À reprendre : l'**angle** du natif (`θ = 180 − acteur+0x4C`), mais une rotation
d'écran normale. Constaté en jeu le 2026-08-15 sur la minimap Bourgeon.

### 2.4 Ce que la fonction dessine, dans l'ordre

| # | source | genre | demi | bitmap / couleur |
|---|---|---|---|---|
| 1 | joueur (`Scene_WorldPosToCellXY`) | 4 | 5 | `유저인터페이스\map\map_arrow.bmp` |
| 2 | liste `0x015fffc8` | 2 | 2 | rouge `0xFFFF0000`, compteur décrémenté par frame, nœud détruit à 0 |
| 3 | `GameMode+0x5CC` ⇒ `GameMode+0x5C4/0x5C8` | 5 | 6 | `유저인터페이스\map\bossmonster.bmp`, collé au bord |
| 4 | objets de carte (si `g_MiniMapShowObjects`) | 6 | 6 | index = couleur, collé au bord |
| 5 | `GameMode_DrawMiniMapPartyGuildQuestMarkers` (`0x00c66060`) | voir §2.5 | | |
| 6 | `GameMode+0x1C8` | 3 | 1 | clignote 500 ms sur 1000, **expire à 15 s** (`timeGetTime`) |

### 2.5 Groupe, guilde, quêtes — `0x00c66060`

| champ `CGameMode` | contenu | genre | message vers la grande carte |
|---|---|---|---|
| `+0x1B4` (`[109]`) | positions de **groupe** | 2 | `483` index 0 |
| `+0x1B8` (`[110]`) | leur nombre | — | `484` index 0 (redimensionne) |
| `+0x1BC` (`[111]`) | positions de **guilde** | 1 | `483` index 1 |
| `+0x1C0` (`[112]`) | leur nombre | — | `484` index 1 |
| `+0x1F0` (`[124]`) | marqueurs de **quête** | 0, collé au bord | via `485` (cf. §3.3) |
| `0x015fff88` / `0x015fff8c` | cellule du marqueur « recherche de boutique » | 0, collé au bord | — |

### 2.5 bis Les repères du serveur — `viewpoint` / `ZC_COMPASS`

La liste clignotante de `GameMode_DrawMiniMap` est celle que remplit le paquet
**`ZC_COMPASS` (0x0144)**, c'est-à-dire la commande de script rAthena
`viewpoint <type>,<x>,<y>,<id>,<color>`.

```
CGameMode + 0x1C4   tête de std::map<id, Viewpoint>
CGameMode + 0x1C8   taille
  nœud + 0x10  id (la clé — c'est le 4ᵉ argument de `viewpoint`)
  nœud + 0x14  cellule X
  nœud + 0x18  cellule Y
  nœud + 0x1C  int : 0 = éphémère, non nul = permanent
  nœud + 0x20  D3DCOLOR (ARGB) — le 5ᵉ argument de `viewpoint`
  nœud + 0x24  DWORD : `timeGetTime()` à la pose
```

- **Clé confirmée** par `0x00c65fe0`, l'`erase` par id (le `type = 1` du paquet).
- Rendu : genre **3** (croix), demi-étendue **1**, couleur du nœud, **clignotant
  500 ms sur 1000** — la phase part de l'horodatage de pose, donc deux repères
  posés à des instants différents clignotent en décalé.
- 🔴 Un repère éphémère est **détruit au bout de 15 s par le dessin lui-même**.
  Un module externe qui les affiche doit donc **filtrer** les périmés, jamais les
  retirer : la liste appartient au client, et y toucher pendant son itération la
  casserait.

Bitmaps : quêtes `유저인터페이스\basic_interface\quest_%d.bmp`,
boutique `유저인터페이스\basic_interface\search_store.bmp`.

### 2.6 Structures du monde utilisées

```
CGameMode + 0xCC  ([51]) = le monde / la scène
   monde + 0x2C  = acteur du joueur   (+0x10 = x, +0x18 = z, +0x4C = angle)
   monde + 0x30  = info de carte      (+0x110 = largeur cellules,
                                       +0x114 = hauteur cellules,
                                       +0x118 = taille de cellule)
```

`Scene_WorldPosToCellXY` @ `0x00c6ac10` :
`cellX = floor(largeur/2 + x/taille)`, `cellY = floor(hauteur/2 + z/taille)`.
Raccourcis : `0x00c6aeb0` = hauteur en cellules, `0x00c6aef0` = cellule du joueur.

---

## 3. La grande carte — `UIMiniMapWnd`, id 273

Ouverte par le bouton `mini` du radar, par l'icône de menu « map » (cmd `0xDB`)
ou par `MakeWindow(273)`. Preuve de l'id : `SaveRectAndCloseWindow(mgr, 273)` sur
la commande de fermeture `0xC9` dans son `OnMsg`.

### 3.1 Mise à l'échelle — `UIMiniMapWnd_CreateControls` @ `0x00962010`

```c
this[46] = 512.0f / max(largeurCarte, hauteurCarte);   // échelle cellule → pixel
this[47] = 2.0f;      // bord gauche  (décalé si la carte est plus HAUTE que large)
this[48] = 531.0f;    // bord bas     (décalé si la carte est plus LARGE que haute)
```

Le décalage vaut `|largeur-hauteur| * 0.5 * échelle` : la carte est **centrée**
dans le carré de 512.

Position d'un marqueur :
`x = cellX*this[46] − marge + this[47]`, `y = this[48] − (cellY*this[46] + marge)`.
(⚠ l'axe Y est **inversé**.)

`UIMiniMapWnd_ApplyDefaultRect` @ `0x0096c680` : rect par défaut
`x = largeurÉcran − 536`, `y = 190`, `516 × 559`, les trois bascules à 1.
Appliqué depuis `OnMsg 34` quand le rect sauvegardé sort de l'écran.

### 3.2 Contrôles

| contrôle | cmd | libellé / infobulle |
|---|---|---|
| bascule « Quête » `this[71]` | 361 (`0x169`) | `MSI_MINIMAP_MSG_02/03` — Quest / Show Quest icon |
| bascule « Commodité » `this[72]` | 194 (`0xC2`) | `MSI_MINIMAP_MSG_04/05` — Facility / Show Facility icon |
| bascule « Guilde/Groupe » `this[73]` | 373 (`0x175`) | `MSI_MINIMAP_MSG_06/07` — Guild/Party |
| fermer | 201 (`0xC9`) | `MSI_MINIMAP_MSG_01` — Close |
| mémo `\minimap\memo_ready` | 356 | `MSI_MINIMAP_MSG_08` — Record |
| boss `\minimap\boss` `this[75]` | 205 | `MSI_MINIMAP_MSG_09` — Boss Monster |
| 20 points mémo `\minimap\memopoint` `this[49..68]` | 600..619 | — |

| marqueur du joueur `this[74]` | 200 | `MSI_MINIMAP_MSG_10` — « I » |

Les trois bascules sont posées en bas (`y = hauteur-19`) aux `x` 20 / 120 / 220.

### 🔴 Les noms de bitmap du tableau ci-dessus sont des BASES, pas des fichiers

`UIMiniMapWnd_CreateBitmapButton` @ `0x00895750` reçoit `"minimap\memopoint"` et
compose **trois** chemins complets :

```
유저인터페이스\  +  <base>  +  "_1" | "_2" | "_3"  +  ".bmp"
        (si le drapeau param_6 est posé)      normal / survol / enfoncé
```

Donc le fichier réel est `유저인터페이스\minimap\memopoint_1.bmp`, jamais
`memopoint.bmp`. Même chose pour `minimap\boss`, `minimap\memo_ready`,
`minimap\memo_ok`, et pour les cinq boutons du radar (`minimap\i_<nom>_1.bmp`,
cf. §1.2 — ceux-là passent par `UIMinimapZoomWnd_CreateControls`, qui fait la
même composition à la main).

Le symptôme d'un oubli est explicite en jeu : `Resource File Loading fail`, puis
le chemin tenté préfixé de `texture\` par le gestionnaire de textures.

### 3.3 `OnMsg` @ `0x00962a30` — le protocole moteur → fenêtre

| msg | arguments | effet |
|---|---|---|
| 481 | `(cellX, cellY)` | déplace le marqueur « I » `this[74]` |
| 482 | `(cellX, cellY)` | déplace le marqueur de boss `this[75]` |
| 483 | `(idxListe 0..2, rang, cellX, cellY)` | pose un point : **0 = groupe, 1 = guilde, 2 = warp** |
| 484 | `(idxListe, taille)` | redimensionne la liste (insère / tronque) |
| 485 | `(type, sousType, cellX, cellY)` | crée un bouton PNJ de quête `\minimap\Quest_%d_%d.bmp`, nom `MSI_MINIMAP_MSG_12` |
| 486 | — | purge **tous** les marqueurs |
| 34 | rect sauvegardé | applique la position, ou `ApplyDefaultRect` si hors écran |

Le seed des boutons 485 se fait par `sub_CAE760` @ `0x00cae760`, appelé depuis
`CreateControls`, qui rejoue `GameMode+0x1F0` (les marqueurs de quête).

### 3.4 `DrawContent` @ `0x00962370`

```c
titre = format("%s ( %s ) %d %d", nomCarte, nomAffiché, cellX, cellY);
UIWindow_FillRectClamped(this, 2, 19, 512, 512, 0xFFAAAAAA);
blit(유저인터페이스\map\<carte>.bmp, 2, 19);
```

Puis, dans cet ordre :

| vecteur | texture | condition |
|---|---|---|
| `this+88/89` | `유저인터페이스\minimap\warp.bmp` | **toujours** |
| `this+85/86` | `유저인터페이스\minimap\guild.bmp` | si la bascule `this[73]` est enfoncée |
| `this+82/83` | `유저인터페이스\minimap\Party.bmp` | idem |

Les trois vecteurs sont construits par le `eh vector constructor iterator` du
ctor (`this+82`, 12 octets × 3) et adressés par `this + 3*idx + 82` dans le
message 483 → **idx 0 = groupe, 1 = guilde, 2 = warp**, ce qui recoupe l'ordre
des textures ci-dessus.

### 3.4 bis La table des objets de carte — `0x00a81e00`

C'est la source des icônes de **PNJ, commodités et warps**, pour la grande carte
comme pour le radar.

```c
// __thiscall ; la table est *(0x0159C08C), un POINTEUR à déréférencer.
void GetMapObject(void* table, MapObject* out, std::string map /*PAR VALEUR*/, int index);
```

- `map` est passée **par valeur** (24 octets poussés sur la pile) ; la fonction
  retaille elle-même le nom à son premier `.`, donc l'extension est tolérée.
- `MapObject` fait **60 octets** :

| offset | champ |
|---|---|
| `+0x00` | `int` type — **8 = warp** |
| `+0x04` | `std::string` nom affiché (l'infobulle du bouton natif) |
| `+0x1C` | `int` cellule X |
| `+0x20` | `int` cellule Y |
| `+0x24` | `std::string` chemin du bitmap |

- **Fin de liste = le second `std::string` (le bitmap) est VIDE.** C'est le test
  des deux appelants (`if (!v75[4]) break;`), pas une valeur de retour.
- 🔴 Appeler cette fonction n'est **pas** une lecture mémoire : elle *construit*
  deux `std::string` dans le tampon de sortie et, sur le chemin « pas trouvé »,
  passe par `sub_A809C0(out)`. L'appelant doit donc fournir des `std::string`
  déjà valides (SSO vide) et libérer celles qui débordent des 15 caractères, avec
  l'allocateur du CLIENT. Un `memset` sur le tampon ne suffit pas.

**⇒ Ne pas l'appeler : lire la table.** `0x0159C08C` est le singleton
`CTownInfoMgr`, et sa table est une `std::map<std::string, std::vector<Rec60>>`
tout à fait ordinaire :

```
mgr + 0x08          nœud sentinelle de la map
  nœud + 0x10       std::string  = le nom de carte (la clé)
  nœud + 0x28/0x2C  vecteur : _Myfirst / _Mylast, pas de 60
```

La parcourir et comparer les clés soi-même évite jusqu'au `find` natif — qui
prendrait lui aussi sa clé par valeur.

### Les dix icônes, et d'où elles viennent

Le champ `bitmap` de l'enregistrement n'est pas dans le `.lub` : `sub_A80E30`
(le constructeur d'enregistrement, appelé par `TownInfoLua_AddTownInfo`
`0x00a82260`) le copie depuis un **tableau statique de dix `std::string` à
`0x0159C090`, pas de 24, indexé par le TYPE** :

| type | icône | libellé du `.lub` |
|---|---|---|
| 0 | `유저인터페이스\Information\Store.bmp` | Tool Dealer |
| 1 | `…\Weaponshop.bmp` | Weapon Dealer |
| 2 | `…\ArmorShops.bmp` | Armor Dealer |
| 3 | `…\Smithy.bmp` | Smith |
| 4 | `…\Guide.bmp` | Guide |
| 5 | `…\Inn.bmp` | Inn |
| 6 | `…\kafra.bmp` | Kafra Employee |
| 7 | `…\style.bmp` | Styling Shop |
| 8 | `…\warp.bmp` | (portail — pas dans Towninfo.lub) |
| 9 | `유저인터페이스\minimap\Quest.bmp` | (quête — idem) |

Source Lua : `System\Towninfo.lub` (chaîne `0x0104E264`), chargé par `sub_A81600`,
qui enregistre la fonction Lua `AddTownInfo(carte, nom, x, y, type)`. Le client
lit en réalité `SystemEN\` quand cette langue est active, et `Towninfo_C.lub`
fusionne ses ajouts par-dessus via `F_ROTP`. **Raison de plus pour lire la table
plutôt que le fichier** : elle porte déjà le résultat de tout ça.

### 3.5 Les objets de carte et les warps — `UIMiniMapWnd_BuildMapObjectButtons` @ `0x00961350`

(Anciennement mal nommée `BuildMemoPointButtons` dans l'IDB — corrigé.)

Parcourt la table d'objets de la carte courante (`sub_A81E00(nomCarte, i)`) :

- **type 8 = warp** → la position écran est empilée dans `this+88/89`, la liste
  que `DrawContent` blitte avec `warp.bmp` ;
- **autres types** → un `UIBitmapButton` de commande **1000 + i**, rangé dans la
  map `this+76` (les icônes de « commodité », que la bascule `0xC2` montre ou
  cache). Cliquer déclenche `CNavigation_SearchRoute` vers cet objet.

La map `this+78` reçoit de même les icônes de quête (bascule `0x169`).

### 3.6 Les points mémo

`UIMiniMapWnd_LoadMemoTextButtons` @ `0x00961890` : replace les 20 boutons
`this[49..68]` hors écran (−100,−100) puis relit le fichier.

- chemin : format `%s\Memo\$$%s.txt` (`0x0103e85c`) ;
- lecture : `fgets` + `strtok` sur `"\t ,\t\r\n"`, **40 lignes au plus** ;
- écriture (suppression d'un mémo) : `%d\t%d\t%s\r\n` (`0x0103e870`) — cellule X,
  cellule Y, libellé, en mode `"wb"`.

La suppression passe par `OnMsg 6` sur les commandes des boutons mémo, avec une
boîte modale de confirmation `MSI_MINIMAP_MSG_11` (0xABF) ; le fichier est
réécrit **entièrement** puis rechargé.

---

## 4. La carte du monde — `UIRoMapWnd`, id 140

- `MakeWindow` case 140 @ `0x00a3f15d` : `new(0x228)` → `UIRoMapWnd_ctor` `0x008d7910`,
  publiée dans `mgr+0x400`.
- vtable `0x01038140` : `CreateControls` `0x008eb450`, `DrawContent` `0x008f3d40`,
  `OnMsg` `0x008feeb0`.
- Ouverte / fermée par le bouton `viewon` du radar (cmd **454**) :
  `if (!SaveRectAndCloseWindow(mgr, 140)) MakeWindow(mgr, 140);` — donc **une
  bascule**, pas une ouverture.
- `uiwnd.h` la connaît déjà sous `kWorldMapWndId = 0x8c`, et `IsHudReplaced()`
  s'en sert comme test « une UI plein écran remplace le HUD ». Ce test reste
  valable.
- `CreateControls` référence le libellé `ctrl+\`` et des cases à cocher
  `checkbox_0/1.bmp`, ainsi que les noms `.rsw` des cartes.

---

## 4 bis. Les bitmaps de carte sont GÉNÉRÉS, pas dessinés

Relevé sur les 877 fichiers de
`data\texture\유저인터페이스\map\` (client Moonlight, 2026-08-16) :

| dimensions | nombre |
|---|---|
| 512 × 512 | 823 |
| 1 × 1 | 28 (cartes sans minimap — des bouchons) |
| 12 × 12 | 5 |
| **512 × 511** | 4 |
| 981 × 981 | 3 |
| **512 × 513** | 2 |

834 des 877 sont en **8 bits par pixel** (palettisés).

Deux détails tranchent la question : le **canevas fixe à 512** quelle que soit la
forme de la carte, et surtout les **512×511 / 512×513** — un arrondi à un pixel
près. Personne ne dessine à la main une image de 512×511 ; c'est la signature
d'une chaîne de mise à l'échelle automatique. Le contenu est vraisemblablement
rasterisé depuis le `.gat` (la grille de collision : un type par cellule), ce que
confirme le rendu — les bâtiments d'une ville sont des TROUS, c'est-à-dire des
cellules bloquées, pas des murs tracés.

🔴 **Conséquence pour tout portage** : le canevas étant carré et la carte non, le
contenu est **étiré** de façon non uniforme pour le remplir. C'est exactement ce
que suppose le natif, dont les UV couvrent tout le bitmap (`u = px/largeur`,
`v = (hauteur−py)/hauteur`) — il n'y a AUCUN calage à faire, et vouloir
« préserver le rapport de forme » du bitmap décalerait les marqueurs.

## 5. Navigation (voisinage)

- `CNavigation_SearchRoute` @ `0x00b314f0`, `CNavigation_SearchByName` @ `0x00b31980`,
  objet `0x015c3090`. Déjà utilisé côté Bourgeon (`features\windows\item_desc_window.cc`).
- `Navigation_LoadLuaFiles` @ `0x00d77e10` charge, par jeu de trois variantes
  (`_krpri`, `_krSak`, `_krLoc`) :
  `Lua Files\Navigation\Navi_f`, `Navi_Map`, `Navi_Npc`, `Navi_Mob`, `Navi_Link`,
  `Navi_LinkDistance`, `navi_picknpc`, `Navi_Scroll`, `Navi_NpcDistance`.
- 🔴 **La fenêtre de navigation est l'id `0xCB` (203), classe `UINavigationV4Wnd`**
  (vtable `0x00FD95EC`). MESURÉ en jeu, pas déduit : `CNavigation_SearchRoute`
  publie la fenêtre ouverte dans **`0x0136E57C`** et, quand ce global est nul,
  ouvre `MakeWindow(0xCB)`. Lire `*(0x0136E57C)+0x2C` pendant qu'elle est
  affichée donne l'id sans ambiguïté.
  ⚠ Le relevé RTTI de 2026-07-10 attribuait `0x9c` à « UINaviSearchWnd » : c'est
  faux — **`0x9c` ouvre une fenêtre de réglages de raccourcis** (elle passe par
  `UIWindowMgr_MakeWindowFromLuaInfo`, la fabrique générique que partagent aussi
  `0x271E` et consorts). Une classe devinée par balayage ne vaut pas une mesure.
- Classes RTTI voisines : `UINavigationWnd`, `UINavigationV4Wnd`,
  `UINavigationHelpWnd`, `UINavigationroadiconWnd`, `UINavigationRuideWnd`,
  `UIListBoxNavi`, `UIListBox_NaviSearch`, `UINaviLinkButton`, `UIScrollBar_Navi`.
  `UINaviSearchWnd` = id `0x9c` (relevé antérieur).

---

## 6. Options et persistance

| clé | valeur | où |
|---|---|---|
| `TT_SHOW_MINIMAP_BUTTON_ONOFF` | index **223** (`0xDF`) de la table `TT_*` `0x01008120` | `GameSettings_GetFlag` `0x0068ea70` / `SetFlagRaw` `0x0068fd50` ; applique via `OnMsg 502` sur `g_MinimapZoomWnd` — `GameSettingsCmd_ShowMinimapBtn_OnOff` `0x00693410` |
| `m_minimapZoomWnd` | `REG_DWORD` sous `HKLM\<SubKey>` | lu `0x00a34090`, écrit `UIConfig_SaveWindowRectsToRegistry` `0x00a4d760` (voisins : `M_ISDRAGALL`, `M_ISDRAWCOMPASS`) |
| position/taille des fenêtres | tables Lua `TblPos` / `TblSize` | `0x009a91f0` (⚠ mal nommée `Minimap_SaveTblPosSize` dans l'IDB : elle est **générique**) |

`g_MiniMapZoom` et `g_MiniMapShowObjects` ne sont **pas** persistés par ces
chemins : ils repartent à 1.0 et à leur valeur par défaut à chaque session.

---

## 7. Globaux — récapitulatif

```
0x0131f538  g_MiniMapZoom          float, [1.0 .. 4.0]   (live : 1.0)
0x0131f53c  g_MiniMapShowObjects   bool                  (live : 1)
0x0131f544  g_MiniMapTintColor     D3DCOLOR              (live : 0xAAFFFFFF)
0x0131f6a8  g_MinimapZoomWnd       UIMinimapZoomWnd*     (fenêtre 14)
0x0136e5d4  g_MiniMapWnd           UIMiniMapWnd*         (fenêtre 273)
0x015fffc8  liste de marqueurs rouges temporaires (compteur par frame)
0x015fff88  cellule X du marqueur « recherche de boutique » (−1 = aucun)
0x015fff8c  cellule Y idem
0x015e817c/80  dernière cellule poussée en OnMsg 482 (boss)
0x015e8184/88  dernière cellule poussée en OnMsg 481 (joueur)
0x012515f8  g_SceneRenderQueue (pointeur ; +0x28 largeur, +0x2C hauteur écran)
0x0122b3d8  g_TextureDownscaleFactor
```

## 8. Fonctions — récapitulatif

```
0x0088e340  UIMinimapZoomWnd_ctor
0x008a9220  UIMinimapZoomWnd_CreateControls      (vt+0x3c)
0x008b4ff0  UIMinimapZoomWnd_DrawContent         (vt+0x50)
0x008c5080  UIMinimapZoomWnd_OnMsg               (vt+0x94)
0x008d1fb0  UIMinimapZoomWnd_AddObjectMarkerButton
0x00960f30  UIMiniMapWnd_ctor
0x00962010  UIMiniMapWnd_CreateControls          (vt+0x3c)
0x00962370  UIMiniMapWnd_DrawContent             (vt+0x50)
0x00962a30  UIMiniMapWnd_OnMsg                   (vt+0x94)
0x00961350  UIMiniMapWnd_BuildMapObjectButtons
0x00961890  UIMiniMapWnd_LoadMemoTextButtons
0x0096c680  UIMiniMapWnd_ApplyDefaultRect
0x00cae760  (rejoue les marqueurs de quête via OnMsg 485)
0x008d7910  UIRoMapWnd_ctor
0x008eb450  UIRoMapWnd_CreateControls
0x008feeb0  UIRoMapWnd_OnMsg
0x00c66ab0  GameMode_DrawMiniMap
0x00c685c0  GameMode_DrawMiniMapMarker
0x00c66060  GameMode_DrawMiniMapPartyGuildQuestMarkers
0x00c67130  MiniMap_DrawFilledDot
0x00c6ac10  Scene_WorldPosToCellXY
0x00c74a80  GameMode_InGame_ProcessFrame  (appel du radar @ 0x00c74fc2)
```

## 9. Pièges relevés (noms de l'IDB corrigés le 2026-08-14)

- `UIMiniMapWnd_OnCreate` `0x008a9220` désignait en fait le **radar** →
  `UIMinimapZoomWnd_CreateControls`.
- `UIWorldMapWnd_OnMsg` `0x008c5080` désignait le **radar**, pas la carte du
  monde → `UIMinimapZoomWnd_OnMsg`.
- `UIMiniMapWnd_BuildMemoPointButtons` `0x00961350` construit les **objets de
  carte et les warps**, pas les points mémo → `UIMiniMapWnd_BuildMapObjectButtons`.
- 🔴 `CActorSprite_SubmitMiniMapMarker` `0x00c58780` est **du code mort** :
  aucune référence dans le binaire, et le drapeau qu'il teste (index `0x77`) est
  `TT_LIGHTMAP_ON_OFF`, pas un réglage de minimap. **Ne pas s'en servir comme
  source d'offsets.**
- `MiniMap_BuildEmblemSyncStream` `0x00c7ebd0` n'a rien à voir avec la minimap :
  elle assemble des blocs typés 13000..13005 (emblèmes de guilde).
- `Minimap_SaveTblPosSize` `0x009a91f0` est le dump **générique** `TblPos`/`TblSize`
  de toutes les fenêtres.

Ces cinq points portent désormais un commentaire dans l'IDB, en plus de ce
document.
