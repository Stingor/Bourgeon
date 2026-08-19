# Navigation (recherche de carte / NPC / monstre + itinéraire) — reverse engineering

Client `2025-07-16 Ragexe` (base `0x400000`), IDB
`2025-07-16_Ragexe_175220998_clientinfo.exe.i64`. Relevé du 2026-08-16, la
fenêtre étant **ouverte en jeu** pendant l'étude.
Serveur : `moonlight` (fork rAthena, **pre-renewal**).

> 🔴 **Le piège central de ce sujet** : il y a **deux** générations de fenêtre de
> navigation dans le binaire. `UINaviWnd` / `UINavigationWnd` (dossier de skin
> `navigation_interface\`, boutons `NaviSearch/NaviStart/NaviRoute/NaviList`) est
> **morte** : son constructeur `0x008db750` n'a **aucun** appelant dans
> `UIWindowMgr_MakeWindow`, seulement la référence de sa vtable. Décompiler
> `UINaviWnd_OnCreate` (`0x008e7990`) — la plus grosse et la plus tentante des
> deux — documente du code que le joueur ne verra jamais. La fenêtre vivante est
> **`UINavigationV4Wnd`, id 203 (`0xCB`)**, skin `navigation_interface3\`.

---

## 0. Les quatre fenêtres vivantes, d'un coup d'œil

| Rôle | Classe RTTI | id | vtable | ctor (`new`) | `SetSize` | Position |
|---|---|---|---|---|---|---|
| **Navigation** (principale) | `UINavigationV4Wnd` | **203** (`0xCB`) | `0x00FD95EC` | `0x005A4010` (`0x124`) | 272 × 338 | rect restauré (`mgr+0x4F098`) |
| **Itinéraire** (liste des étapes) | `UINavigationRuideWnd` | **314** (`0x13A`) | `0x00FD943C` | `0x005A3FB0` (`0xB8`) | 150 × 338 | `navi.x + 272`, `navi.y` — collée à droite |
| **Choix d'icône de route** | `UINavigationroadiconWnd` | **306** (`0x132`) | `0x00FD9364` | `0x005A40B0` (`0xB8`) | 158 × 102 | `navi.x + 113`, `navi.y + 211` |
| **Aide** | `UINavigationHelpWnd` | **229** (`0xE5`) | `0x01038800` | `0x008D7510` (`0xBC`) | 310 × 200 | (100, 100) fixe |

Génération morte (aucun `case` dans `MakeWindow`) :

| Classe | vtable | OnCreate | Statut |
|---|---|---|---|
| `UINavigationWnd` (v1) | `0x01038728` | `0x008E7990` | 🔴 **morte** — ctor `0x008DB750` sans appelant |

Les cases de `UIWindowMgr_MakeWindow` (`0x00A39340`) :

```
0x00a3e8a5  case 203  new(0x124) -> 0x005A4010  SetSize(272,338)  slot mgr +0x4F094
0x00a3e923  case 229  new(0x0BC) -> 0x008D7510  SetSize(310,200)  SetPos(100,100)
0x00a3e994  case 306  new(0x0B8) -> 0x005A40B0  SetSize(158,102)  SetPos(navi.x+113, navi.y+211)
0x00a3ea2d  case 314  new(0x0B8) -> 0x005A3FB0  SetSize(150,338)  SetPos(navi.x+272, navi.y)
```

Globaux dédiés (⚠ non nuls ⇔ fenêtre ouverte **en ce moment**) :

```
0x0136E57C  g_NavigationWnd    = mgr(0x0131F4E8) + 0x4F094   fenêtre 203
0x0136E580  rect sauvegardé de la fenêtre 203 (mgr + 0x4F098)
0x0136E668  g_PrivateAirplaneWnd (fenêtre « avion privé », ouverte par le cmd 499)
```

Les 306 et 314 sont **positionnées relativement à la 203 au moment de leur
création** (elles lisent `[navi+0x1C]` / `[navi+0x20]`), et **jamais
repositionnées ensuite** : déplacer la fenêtre principale laisse ses deux
satellites en arrière. C'est le premier défaut structurel du natif (§6).

Slots de vtable surchargés par `UINavigationV4Wnd` (les autres viennent de la
base commune) :

| Slot | Adresse | Rôle |
|---|---|---|
| `+0x00` | `0x005A4790` | destructeur scalaire |
| `+0x2C` | `0x005AB7B0` | (surchargé — non identifié) |
| `+0x3C` | `0x005A65F0` | **OnCreate** (construit tous les enfants) |
| `+0x4C` | `0x005A89F0` | (surchargé — non identifié) |
| `+0x50` | `0x005A7A40` | **DrawContent** (candidat principal) |
| `+0x7C` | `0x005A86E0` | (surchargé — non identifié) |
| `+0x94` | `0x005A9410` | **OnMsg** |

---

## 1. Anatomie de la fenêtre 203 — `UINavigationV4Wnd_OnCreate` @ `0x005A65F0`

Les enfants sont rangés dans l'objet à des offsets fixes (`this[n]` = `this+4n`).
Le troisième nombre est l'argument du slot `vt+0xB4`
(`UIControl_SetCommandId`) : **c'est l'identifiant que l'OnMsg reçoit** dans
`ArgList` lorsqu'on clique le contrôle.

| Champ | Offset | Type | cmd | Bitmaps / libellé (`MsgString` id) |
|---|---|---|---|---|
| `this[45]` | `+0xB4` | bouton fermer (helper) | — | `AddCloseButton`, nom `0xBE5` |
| `this[46]` | `+0xB8` | `UIBitmapButton` | **201** | `btn_close_*` — **fermeture logique** (arrête la navigation) |
| `this[47]` | `+0xBC` | bouton minimiser (helper) | — | `AddMinimizeButton`, nom `0x89C` |
| `this[48]` | `+0xC0` | `UIBitmapButton` | **343** | `NaviHelp_off/on` — ouvre/ferme l'**aide** (229) |
| `this[49]` | `+0xC4` | `UIBitmapButton` | **200** | `btn_maximize_*` — sort du mode réduit |
| `this[50]` | `+0xC8` | `UIBitmapButton` | **189** | icône de route courante — ouvre le **sélecteur d'icônes** (306) |
| `this[51]` | `+0xCC` | `UIBitmapButton` | **191** | `btn_Navidata_Open_*` — ouvre la liste des fichiers `Navigationdata\*.txt` |
| `this[52]` | `+0xD0` | `UIBitmapButton` | **284** | `bt_back` — **« Find »** (`MSI_NAVIGATION_SEARCHTORUITE`, `0xC88`) |
| `this[53]` | `+0xD4` | `UIToggleButton` 110×12 | — | « Minimize » |
| `this[54]` | `+0xD8` | `UIBitmapButton` | **442** | `btn_roadlistwnd_*` — ouvre/ferme l'**itinéraire** (314) |
| `this[55]` | `+0xDC` | combo (`0x00835300`) | — | filtre **Tout / Carte / NPC / Monstre** (msg 39) |
| `this[56]` | `+0xE0` | `UIBitmapEditCtrl` 113×18 | **516** | champ de **recherche**, longueur max 30 |
| `this[57]` | `+0xE4` | `UIBitmapButton` | **321** | `btn_searchbar_*` — **lancer la recherche** |
| `this[58]` | `+0xE8` | (`0x008188D0`) 100×20 | — | libellé de résultat (`0xC87`) |
| `this[59]` | `+0xEC` | `UIListBox_NaviSearch` 138×244 | **336** | **liste des résultats** (blanc 255/255/255) |
| `this[60]` | `+0xF0` | `UIListBox_NaviSearch` 132×80 | **441** | **liste secondaire** (240/244/253) |
| `this[61]` | `+0xF4` | `UIToggleButton` 110×12 | **213** | « service » — voir §4.3 |
| `this[62]` | `+0xF8` | `UIBitmapButton` | **354** | **« Share »** (`MSI_NAVIGATION_SHARETIP`, `0xC86`) |
| `this[63]` | `+0xFC` | `UIBitmapButton` | **430** | **« MOVE »** — aller à la cible (`MSI_NAVIGATION_TARGETGO`, `0x895`) |
| `this[64]` | `+0x100` | `UIBitmapButton` | **499** | **avion privé** (`ico_destination`) |
| `this[65..66]` | `+0x104/+0x108` | `std::vector<std::string>` | — | noms des fichiers `Navigationdata\*.txt` |
| `this[68]` | `+0x110` | — | — | position reçue au msg 34 (drag) |
| `this[70]` | `+0x118` | `DWORD` | — | échéance `timeGetTime()+5000` (temporisation du GO) |
| `this[14]/[15]` | `+0x38/+0x3C` | alpha, `timeGetTime()` | — | fondu du mode réduit (255 plein / 170 réduit) |

Le combo `this[55]` est peuplé des quatre entrées `MsgString` `0x89E`, `0x89F`,
`0x8A0`, `0x8A1` (Tout / Carte / NPC / Monstre) ; l'index sélectionné va dans
`dword_015C42F0`.

---

## 2. `UINavigationV4Wnd_OnMsg` @ `0x005A9410`

Signature : `OnMsg(this, src, msg, arg, p2, p3, p4)`.

| msg | Effet |
|---|---|
| **0** | *Entrée* dans le champ de recherche : si `src == this[56]` et texte non vide → se ré-émet un `msg 6 / cmd 321` (donc **Entrée ≡ clic sur la loupe**) |
| **6** | **Commande de contrôle** — `arg` = le cmd du tableau §1 ; table détaillée ci-dessous |
| **34** | Déplacement : `this[68] = arg`, `SetPos(arg[0], arg[1])`, redessin |
| **39** | Changement du combo : `dword_015C42F0 = arg`, `CNavigation::0x00B36200`, puis **refill de la liste** (`0x005AB170`) |
| **40** | `0x005AB450` — application de l'icône de route choisie dans la 306 |

### Table des commandes (`msg 6`)

| cmd | Nom naturel | Ce que ça fait vraiment |
|---|---|---|
| **189** | icône de route | bascule la fenêtre 306 (`SaveRectAndClose(306)` sinon `MakeWindow(0x132)`) |
| **191** | ouvrir un fichier | vide `this[65..66]`, liste `%s\Navigationdata\*.txt` (`0x00A93CA0`), crée la **liste déroulante générique 0x1A**, y pousse chaque nom (msg 31), la dimensionne `160 × min(16·n, 160)`, lui donne l'**owner 203** (msg 83) et la pose à `navi.x - 165`, y=36 |
| **200** | agrandir | `byte_015C42E4 = 0`, alpha 255, `byte_015C42D8 = 0`, relayout |
| **201** | fermer | si `dword_015C42E8 == 2` : **confirmation** (`MsgBox` texte `0x8BF`, 2 boutons) → arrêt de la navigation + fermeture de 314 ; sinon arrêt direct, fermeture de **314 et 203** |
| **202** | réduire | `byte_015C42E4 = 1`, alpha 170, relayout |
| **213** | service | `byte_015C430C = byte_015C430E = (p2 != 0)`, `byte_015C430D = 0` (§4.3) |
| **284** | **Find** | lit la cible (`0x00B2EFC0`) et **bascule** le suivi : en route → arrêt, sinon démarrage (`0x00B39460`) |
| **321** | **rechercher** | si le champ n'est pas vide **et** qu'on n'est pas en mode réduit : mémorise le terme (`0x005A45E0`), lance `CNavigation::Search` (`0x00B34970`), vide+redessine la liste secondaire, `0x00B2F400`, `0x00B37F50`, **refill** (`0x005AB170`), rétablit le layout |
| **336** | clic dans la liste des résultats | prend l'index (`this[59]+0x94`), `0x005AAD90`, remet à zéro `dword_015C4358`, arrête la navigation en cours, recopie la sélection dans la liste secondaire |
| **343** | aide | bascule la fenêtre 229 |
| **354** | **Share** | si une route existe (`0x00B2EEF0` non vide) et compte ≥ 2 étapes → **écrit une balise dans la barre de chat** (`0x005AB550`, §4.4) ; sinon passe en état 2 et arme `this[70] = timeGetTime() + 5000` |
| **430** | **MOVE** | `0x00B38E80` ; sinon calcule l'itinéraire vers `(dest.x, dest.y)` via `0x00B30070`, passe en état 1, démarre le suivi (`0x00B39460`), notifie la 314 (msg 60) |
| **441** | clic dans la liste secondaire | `0x005AB730` avec l'index, redessin |
| **442** | itinéraire | bascule la fenêtre 314 — **mais seulement si `0x00B39660` dit qu'une route existe** |
| **499** | **avion privé** | crée `UIPrivateAirplaneWnd` (208×110, centrée) et **vérifie la carte courante** : trois refus possibles, messages `0xD03`, `0xD04`, `0xD19`, plus `0xD1A` si l'état du joueur vaut 3 |

⚠ Les cmd **336** et **441** sautent le `Replay_RecordUIEvent` d'entrée (ils sont
traités avant, `goto LABEL_74`) et enregistrent eux-mêmes l'index sélectionné :
en relecture de replay, `p2` porte déjà l'index et le code ne relit pas la
listbox. Tout remplacement doit conserver cette dissymétrie s'il veut rester
compatible avec l'enregistrement `.brw` ([[reference_replay_reassembly_re]]).

---

## 3. Le moteur : `CNavigation` @ `0x015C3090`

L'objet est **statique et unique** (pas de `new`), en `.data`. Toutes les
fonctions du module vivent dans `0x00B2xxxx`–`0x00B39xxx`.

### 3.1 Champs identifiés

⚠ Offsets recalculés depuis les index `DWORD` du décompilé (`this[n]` = `+4n`) et
recoupés avec les formes `(char*)this + N` de la même fonction ; les deux
colonnes doivent toujours concorder.

| Offset | Index `DWORD` | Contenu |
|---|---|---|
| `+0x0000` / `+0x0004` | `[0]` / `[1]` | **vecteur des cartes** (`begin` / `end`) — parcouru par `0x00B20CE0` ✅live : 1301 entrées |
| `+0x10F0` / `+0x10F4` | `[1084]` / `[1085]` | **vecteur des résultats bruts**, éléments de 40 octets (§3.5) |
| `+0x1124` | `[1097]` (`this+4388`) | **type de la cible courante** : `0` = carte, `2` = NPC, autre = brut |
| `+0x1128` | `[1098]` (`this+4392`) | **nœud** de la cible courante (0 = aucune) |
| `+0x112C` / `+0x1130` | `[1099]` / `[1100]` | **x / y** de la destination — passés à `0x00B30070` par le cmd 430 |
| `+0x1134` | `[1101]` | `std::string` nom de carte de la destination |
| `+0x1124..+0x1137` | — | ce que `0x00B2EFC0` recopie d'un bloc : `{type, nœud, x, y, std::string map}` |
| `+0x1244` | `[1169]` | nœud de `prontera`, résolu à la fin du chargement |
| `+0x1248` | `[1170]` (`this+4680`) | drapeau « la fenêtre est déjà ouverte / ne pas l'ouvrir » |
| `+0x125C` | `[1175]` | passe à `1` quand un itinéraire vient d'être calculé |
| `+0x1260` | `[1176]` | **filtre de recherche** (0 = tout, sinon le `type` voulu) ; > 4 ⇒ recherche refusée |
| `+0x1264` | `[1177]` (`this+4708`) | `std::string` **terme de recherche** |
| `+0x127C` / `+0x127D` / `+0x127E` | — (`this+4732/33/34`) | les **trois options d'itinéraire** (§4.3), copiées depuis le `flag` du paquet |
| `+0x1280` | `[1184]` (`this+4736`) | 6ᵉ argument du pathfinder `0x00B21E70` |
| `+0x1150` | `[1108]` (`this+4432`) | bloc passé au pathfinder ; son résultat est rangé en `+0x114C` (`this+4428`) |
| `+0x12B8` / `+0x12BC` | `[1198]` / `[1199]` | **vecteur des groupes** = `0x015C4348` / `0x015C434C` (§3.5) |

🔴 **Les « globaux » de la navigation n'en sont pas.** `0x015C3090` est la base
de `CNavigation` : tout ce que le décompilé nomme `byte_15C42xx` / `dword_15C43xx`
est en réalité un **champ de cet objet**, ce que la soustraction vérifie
(`0x015C4348 − 0x015C3090 = 0x12B8 = this[1198]`, exactement le vecteur de
groupes construit par `0x00B36200`). D'où l'équivalence complète :

| Nom du décompilé | = champ | Contenu |
|---|---|---|
| `byte_15C42D8` | `+0x1248` `[1170]` | « ne pas (r)ouvrir la fenêtre » |
| `byte_15C42E4` | `+0x1254` `[1173]` | mode **réduit** (1 = réduit) |
| `dword_15C42E8` | `+0x1258` `[1174]` | `2` = itinéraire en cours (déclenche la confirmation de fermeture, et le surlignage de la carte du monde) |
| `dword_15C42EC` | `+0x125C` `[1175]` | **état du suivi** : 0 arrêté / 1 en route / 2 en attente |
| `dword_15C42F0` | `+0x1260` `[1176]` | **filtre** (index du combo) |
| `dword_15C42F4` | `+0x1264` `[1177]` | **terme de recherche** (`std::string`) |
| `byte_15C430C/D/E` | `+0x127C/D/E` | les **trois options** service / avion / scroll |
| `dword_15C4348/4C` | `+0x12B8/BC` | vecteur des **groupes** de résultats |
| `dword_15C4354` | `+0x12C4` | **groupe sélectionné** (−1 = aucun) |
| `dword_15C4358` | `+0x12C8` | **étape courante** |

C'est pourquoi `OnMsg` peut écrire `dword_15C42EC = 1` là où `SearchRoute` écrit
`this[1175] = 1` : **c'est le même octet**. Pour le remplacement, il n'y a donc
qu'un seul objet à connaître, pas une constellation de globaux.

Vecteur global des **résultats de recherche** :

```
0x015C4348   début   (éléments de 12 octets)
0x015C434C   fin
```

`0x005AB170` (« refill ») itère ce vecteur, appelle `0x00B2E700(&out, *elem)`
pour obtenir `{type, id}`, met en forme via `0x005A8B00`, et pousse
`"[%d]%s"` dans la listbox `this[59]`.

Le seul global **extérieur** à l'objet — tout le reste est un champ, voir la
table d'équivalence ci-dessus :

```
0x015FB9AC  nom de la carte courante (C-string), comparé par 0x00B30070
```

### 3.2 Chargement des données — `Navigation_LoadLuaFiles` @ `0x00D77E10`

Appelé par `Lua_LoadAllScriptFiles` (`0x00D646C0`). Le suffixe dépend de
`g_ServerType` (`0x0159B814`) : `krpri` (0), `krSak` (1), `krLoc` (≥2).
Moonlight tourne en `krpri`. Neuf scripts sont chargés depuis
`Lua Files\Navigation\` :

```
Navi_f_<s>            Navi_Map_<s>        Navi_Npc_<s>        Navi_Mob_<s>
Navi_Link_<s>         Navi_LinkDistance_<s>
Navi_NpcDistance_<s>  Navi_Scroll_<s>     navi_picknpc_<s>
```

Ils sont **présents dans `moonlight.grf`** (18 fichiers, `krpri` + `krSak`) :

```
data\luafiles514\lua files\navigation\navi_map_krpri.lub            58 587 o
data\luafiles514\lua files\navigation\navi_npc_krpri.lub           519 333 o
data\luafiles514\lua files\navigation\navi_mob_krpri.lub           234 271 o
data\luafiles514\lua files\navigation\navi_link_krpri.lub          369 536 o
data\luafiles514\lua files\navigation\navi_linkdistance_krpri.lub 1 236 168 o
data\luafiles514\lua files\navigation\navi_npcdistance_krpri.lub  2 089 613 o
data\luafiles514\lua files\navigation\navi_picknpc_krpri.lub        18 433 o
data\luafiles514\lua files\navigation\navi_scroll_krpri.lub            314 o
data\luafiles514\lua files\navigation\navi_f_krpri.lub               5 413 o
```

🔴 **Ces neuf fichiers ne sont pas du bytecode : sept d'entre eux sont du Lua
texte, en clair** — et c'est le signe qu'ils sont **générés par le serveur
Moonlight**, pas repris de kRO. Vérifié par `md5sum` : les six fichiers
`navi_map / npc / mob / link / linkdistance / npcdistance` du GRF sont
**identiques** à ceux de
`moonlight/generated/clientside/data/luafiles514/lua files/navigation/`,
produits par `navi_create_lists()` (`src/map/navi.cpp`).

Trois fichiers **ne sont pas générés** et restent d'origine kRO :
`navi_scroll` et `navi_picknpc`, restés compilés (`LuaQ` — le chemin
`c:\PatchTemp\1761812954_20251105_production_patch_kr_ro1_live` traîne encore
dans leur en-tête), et `navi_f`, la table de traduction communautaire
(`zackdreaver / ROenglishRE`). ⚠ `navi_scroll` est **vide**
(`Navi_Scroll = { "NULL" }`, 314 octets) : aucune arête « scroll » n'existe.

Formats exacts (chaque colonne recoupée avec
`write_map / write_npc / write_warp / write_spawn` de `src/map/navi.cpp`, qui
les écrit) :

| Table | Ligne | Champs |
|---|---|---|
| `Navi_Map` | `{"alb_ship", "alb_ship", 5005, 200, 200}` | nom, nom affiché, **type** (`5001` simple, `5003` segmentée, `5005` segmentée + mobs), largeur, hauteur |
| `Navi_Npc` | `{ "alb2trea", 11984, 102, 83, "Tool Dealer", "", 87, 65}` | carte, id navi, **`101`** normal / **`102`** boutique, sprite (`class_`), nom visible (tronqué au `#`), `""`, x, y |
| `Navi_Link` | `{"alb_ship", 13350, 200, 99999, "alb_ship_alberta_13350", "", 26, 166, "alberta", 170, 168}` | carte, gid, **`200`** warp / **`201`** script, sprite (`99999` = portail), nom, `""`, x, y, carte dest., x dest., y dest. |
| `Navi_Mob` | `{"alb_ship", 17104, 300, 1640457, "Suspicious Mouse", "E_CRAMP", 1, 1376515}` | carte, index, **`300`** normal / **`301`** MVP (`mexp != 0`), **`quantité << 16 \| sprite`**, nom, constante de sprite, niveau, **`((ele_lv*20+def_ele) << 16) \| (size << 8) \| race`** |

Les deux champs empaquetés se décodent donc ainsi — vérifié sur l'exemple :
`1640457 = 0x190BC9` → 25 exemplaires, sprite 3017 ; `1376515 = 0x150103` →
élément 21 (eau, niv. 1), taille 1, race 3. Le type **`301` marque les MVP** :
le fichier en compte **72**, à rapprocher des 73 labels de
[[project_mvp_tracker]].

Puis `0x00B36920` **construit le graphe** en rappelant des fonctions Lua
globales (via `Lua_CallGlobal_va`, `0x00A9A7D0`), avec la signature de
marshalling en 3ᵉ argument :

| Fonction Lua | Signature | Ce qu'elle rend |
|---|---|---|
| `queryNavi_MapInfo(i)` | `d>ssddd` | carte : nom, nom affiché, 3 entiers — nœud de `0x68` octets (`0x00B24AC0`) |
| `queryNavi_NpcInfo(i)` | `d>sdddssdd` | NPC — nœud de `0x7C` octets (`0x00B24BF0`) |
| `queryNavi_MobInfo(i)` | `d>sdddssdd` | monstre — même nœud `0x7C` |
| `queryNavi_LinkInfo(i)` | `d>sdddssddsdd` | **warp** (arête) — nœud de `0x98` octets (`0x00B24790`) |
| `queryNavi_PickNpc(i)` | `d>sddddddddddd` | 9 entiers supplémentaires écrits en `[22..30]` du nœud NPC |
| `queryNavi_Distance_Map(i)` / `_Link(i,j)` / `_Pass(i,j,k)` | `d>sd` / `dd>d` / `ddd>sdd` | matrice des distances **entre cartes** ; le champ texte vaut `"P"` (passage), `"E"` (fin) ou `"NULL"` |
| `queryNavi_NpcDistance_Map/_Link/_Pass` | idem | matrice des distances **jusqu'aux NPC** |
| `queryNavi_Scroll(i)` / `_Scroll_Pass(i,j)` | `d>sddsdd` / `dd>dd` | destinations de **scrolls** (arêtes de coût 400) |

Les bornes de boucle sont en dur : 9 999 cartes, 99 999 NPC / mobs / liens,
999 999 itérations de distance, 200 scrolls. Un échec de lecture ouvre une
**`MessageBoxA` bloquante** (« Error », « Map … / Npc … / Link … »), ce qui, en
plein écran, gèle le client — cf. mémoire projet sur les dialogues bloquants.

### 3.3 API utile

| Adresse | Nom donné | Rôle |
|---|---|---|
| `0x00B34970` | `CNavigation::Search` | exécute la recherche texte + filtre, remplit `0x015C4348` |
| `0x00B31980` | `CNavigation_SearchByName` | API publique « cherche ce nom » ; ouvre la fenêtre 203 si besoin |
| `0x00B314F0` | `CNavigation_SearchRoute` | API publique « va là » ; **14 appelants** (§5) |
| `0x00B30070` | `CNavigation::BuildRoute(x, y)` | calcule l'itinéraire ; rend `11` si on est déjà à destination, `-96/-97/-98` en échec, sinon le coût |
| `0x00B30C60` | — | publie le résultat / le message d'état (`-94` = cible introuvable) |
| `0x00B39460` | — | **démarre** le suivi |
| `0x00B35F80` | `CNavigation::SelectResult` | 🔴 **PAS un arrêt** — piège de nommage. Lit les index `+0x12C4` (groupe) / `+0x12C8` (membre) et **pose la cible**. Index hors bornes ⇒ branche de **réinitialisation** : c'est ainsi que le `cmd 284` arrête le guidage |
| `0x00B38E80` | — | route déjà active ? |
| `0x00B39660` | `CNavigation::GetStepCount` | **nombre d'étapes** de l'itinéraire (le cmd 442 s'en sert comme d'un booléen, mais `0x00903BC0` l'itère bel et bien) |
| `0x00B2EE00` | `CNavigation::GetStep(i)` | l'étape `i` de l'itinéraire |
| `0x00B2F050` | — | nœud de **destination finale** |
| `0x00B2EEF0` | — | rend le `std::string` de l'étape courante |
| `0x00B2EFC0` | — | rend la **destination** `{d, d, x, y, std::string map}` |
| `0x00B2E700` | `CNavigation::GetResult` | remplit un bloc de 40 octets `{type, ptr, x, y, std::string}`. 🔴 Le 2ᵉ champ est un **pointeur d'objet**, pas un identifiant |
| `0x00B26CF0` | — | nom d'un nœud |
| `0x00B20CE0` | — | **résout une carte par nom** (`"prontera"` → nœud) |
| `0x00B26BD0` | — | résout un NPC par (carte, index) |
| `0x00B21E70` | — | le **pathfinder** proprement dit (reçoit les 3 options) |

### 3.4 Structures mesurées **✅live**

Relevé dans le processus (x32dbg attaché, fenêtre 203 ouverte, aucune recherche
lancée) :

```
[0x0136E57C]            = 0x4A504290    la fenêtre 203 est instanciée
[0x4A504290 + 0x00]     = 0x00FD95EC    vtable UINavigationV4Wnd      ✓
[0x4A504290 + 0x14/+18] = 272 / 338     largeur / hauteur              ✓ = SetSize
[0x4A504290 + 0x1C/+20] = 630 / 395     x / y  (ce que lisent les satellites)
[0x4A504290 + 0x2C]     = 0xCB = 203    id de fenêtre                  ✓
[0x0136E580]            = {630, 395, 272}  rect sauvegardé
```

Vecteur des **cartes** du graphe, en `CNavigation+0x00 / +0x04` :

```
begin = 0x11E44040   end = 0x11E45494   -> (0x1454)/4 = 1301 cartes chargées
```

exactement le nombre de lignes de `navi_map_krpri.lub`. Le **nœud carte**
(`0x68` octets, ctor `0x00B24AC0`) se lit ainsi — premier nœud, `alb_ship` :

| Offset | Valeur lue | Champ |
|---|---|---|
| `+0x00` | `0` | index |
| `+0x04` | `5005` | **type de carte** (3ᵉ colonne de `Navi_Map`) |
| `+0x08` | `"alb_ship"` | `std::string` nom interne (SSO : 16 o + taille `+0x18` + capacité `+0x1C`) |
| `+0x20` | `"alb_ship"` | `std::string` nom **affiché** |
| `+0x38` / `+0x3C` | `200` / `200` | largeur / hauteur de la carte |

Le nom se lit toujours par `0x00B26CF0` (`CNaviNode_GetName`) : `node+0x08`,
déréférencé si `node[7] >= 16` (SSO MSVC standard).

**Le nœud carte complet** (ctor `0x00B24AC0`), relu sur `prontera` — dont
`g_Navigation+0x1244` garde le pointeur, ce qui donne un point d'entrée gratuit
✅live :

| Offset | `prontera` | Champ |
|---|---|---|
| `+0x00` | `193` | index |
| `+0x04` | `5003` | type de carte (3ᵉ colonne de `Navi_Map`) |
| `+0x08` | `"prontera"` | `std::string` nom interne (taille `+0x18`, capacité `+0x1C`) |
| `+0x20` | `"prontera"` | `std::string` nom affiché (taille `+0x30`, capacité `+0x34`) |
| `+0x38` / `+0x3C` | `312` / `392` | largeur / hauteur |
| `+0x40` | `0` | octet de marquage (pathfinder) |
| `+0x44` / `+0x48` / `+0x4C` | `0x10AE75A8` / `0x10AE76EC` / … | **`std::vector` des NPC** de la carte → `(0x144)/4 =` **81** |
| `+0x50` / `+0x54` / `+0x58` | `0` / `0` / `0` | **`std::vector` des monstres** → vide (prontera n'en a pas) |
| `+0x5C` / `+0x60` / `+0x64` | `0x12DA6858` / `0x12DA68AC` / … | **`std::vector` des liens** (warps) → `(0x54)/4 =` **21** |

Les trois vecteurs sont alimentés par `0x00B25E80` (NPC, `+68`), `0x00B25F00`
(monstres, `+80`) et `0x00B25EC0` (liens, `+92`), appelés pendant la
construction du graphe.

🔴 **Ces trois compteurs recoupent exactement les fichiers** : `navi_npc_krpri.lub`
donne **81** NPC pour `prontera`, `navi_link_krpri.lub` **21** warps sortants —
et les 21 destinations correspondent aux warps du serveur coordonnée par
coordonnée. La chaîne *fichier → graphe en mémoire → serveur* est donc
**vérifiée de bout en bout**, ce qui valide du même coup la lecture des
structures.

**Conséquence pour l'interface** : on peut **énumérer** le contenu d'une carte
(ses NPC, ses monstres, ses sorties) directement depuis son nœud, sans passer
par la recherche native. C'est ce qui rend possible une vue « qu'y a-t-il
ici ? » que le natif n'offre pas.

**Le nœud NPC / monstre** — `CNavi_Object`, `0x7C` octets, ctor `0x00B24BF0`,
vtable `0x01087E30`. Les offsets viennent du constructeur **recoupés avec l'ordre
des colonnes du `.lub`** (le chargeur passe les valeurs de `queryNavi_NpcInfo` /
`queryNavi_MobInfo` dans cet ordre) :

| Offset | NPC | Monstre |
|---|---|---|
| `+0x00` | vtable `CNavi_Object` | idem |
| `+0x04` | `-1` à la construction | idem |
| `+0x08` | **sous-type** : `101` normal, `102` **boutique** | `300` normal, `301` **MVP** |
| `+0x0C` | sprite (`class_`) | **`quantité << 16 \| sprite`** |
| `+0x10` | `std::string` **nom affiché** (24 o) | idem |
| `+0x28` | `std::string` constante de sprite | idem (`"PORING"`, …) |
| `+0x40` | **nœud de la carte** qui le porte | idem |
| `+0x44` | **x** | 🔴 **niveau** |
| `+0x48` | **y** | 🔴 **statistiques empaquetées** |
| `+0x4C`…`+0x78` | 12 champs, dont `[22..30]` remplis par `queryNavi_PickNpc` | idem |

🔴 **Les deux derniers champs changent de sens selon le type** — exactement comme
les deux dernières colonnes du `.lub`. Les lire comme des coordonnées pour un
monstre affiche son niveau et ses stats sous la forme d'un « (x, y) » absurde.
Un monstre **n'a pas de position** : le fichier n'en donne aucune.

Les statistiques se décodent comme les écrit `write_spawn` :
`((ele_lv * 20 + def_ele) << 16) | (size << 8) | race`.

Slots virtuels utiles (vérifiés un par un) :

| Slot | Offset | Rend |
|---|---|---|
| 4 | `+0x10` | ⚠ **la quantité** (`this[7]` en `u16` = le haut de `+0x0C`), **pas** le niveau — c'est ce que `Navi_FormatMemberLabel` convertit en tranche de densité via les messages `0x991`…`0x995` |
| 5 | `+0x14` | le **nom affiché** (la `std::string` de `+0x10`) |
| 7 | `+0x1C` | le **point** `{x, y}` (`+0x44`/`+0x48`) — le natif ne l'appelle que pour un NPC |
| 8 | `+0x20` | le **nœud carte** (`+0x40`) |

Le nom d'un `CNavi_Object` ne se lit donc **pas** par `CNaviNode_GetName` mais par
son slot 5 — c'est la distinction que fait `Navi_FormatResultLabel`
(`0x005A8B00`) : `type <= 1` → `CNaviNode_GetName` (nœud carte), `type == 2` ou
`3` → slot 5.

**Ce que l'interface peut en tirer**, et que le natif n'affiche pas : le niveau
exact, le nombre d'exemplaires du spawn, l'élément, la taille, la race, le
marqueur **MVP** (`301`) et le marqueur **boutique** (`102`).

---

### 3.7 🔴 Le **type** d'un lien décide s'il est empruntable — et coupe le monde en 745 morceaux

Mesuré le 2026-08-16, à la demande « faire marcher les destinations hors
gonryun ».

Le 3ᵉ champ d'une entrée `Navi_Link` (le `200` omniprésent) n'est pas décoratif.
`CNaviLink_ctor` (`0x00B24790`) le range en `+0x08`, et **`CNavigation_PathFind`
(`0x00B21F9B`) filtre chaque arête dessus**, à deux endroits — l'amorçage puis
la détente de l'A* :

```c
v18 = (*(int (__thiscall **)(_DWORD))(*(_DWORD *)*v16 + 8))(*v16);  // vt+8 = GetType
switch ( v18 ) {
  case 204: if ( (_BYTE)a4 ) goto LABEL_21; break;   // sinon l'arête est REFUSÉE
  case 205: if ( (_BYTE)a6 ) goto LABEL_21; break;
  case 202: case 203:
    if ( (_BYTE)a4 == 1 ) { v20 = (v18 == 202) ? (a7 == 2) : (a7 == 1); ... }
    break;
  default: LABEL_21: ...                              // 200, 201, 400 : toujours pris
}
```

`a4` et `a6` sont les octets d'options que `CNavigation_SearchRoute` vient
d'écrire depuis son masque (§4.3) :

```c
*((_BYTE *)this + 4732) = v12 & 1;          // +0x127C service → autorise 202/203/204
*((_BYTE *)this + 4733) = (v12 & 2) != 0;   // +0x127D avion   → autorise 205
*((_BYTE *)this + 4734) = (v12 & 4) != 0;   // +0x127E scroll
```

La nomenclature est donnée par un commentaire de `write_warp`
(`moonlight/src/map/navi.cpp:360`) : **200** warp · **201** script NPC ·
**202** Kafra Dungeon Warp · **203** Cool Event Dungeon Warp ·
**204** Kafra/Cool Event/Alberta warp · **205** aéroport. S'y ajoute **400**,
que le bloc « scroll » de `CNavigation_BuildGraphFromLua` fabrique en dur
(`sub_B24790(400, ...)`) et que le `default` laisse passer.

**Le défaut de données.** rAthena n'émet que le premier type : sa boucle ne
récolte que les NPC de `subtype == NPCTYPE_WARP`. Tout lieu qu'on n'atteint
qu'en **parlant** à quelqu'un est donc absent du graphe. Mesure sur
`navi_link_krpri.lub` (3738 arêtes, 1301 cartes) : **745 composantes connexes**,
dont **709 cartes totalement isolées**. La plus grosse en compte 413. `gonryun`
— le hub de Moonlight, où tous les évènements renvoient les joueurs — n'en
atteint que six : `gon_in`, `gon_fild01`, `gon_dun01/02/03`, `ggpro`. Même
enfermement pour Louyang, Lasagna, Malaya, Malangdo, Harboro, Turtle Island…

**Le remède existait déjà, inutilisé.** `naviregisterwarp("<nom>","<carte>",x,y)`
(`script.cpp`, `BUILDIN_FUNC(naviregisterwarp)`) pousse une arête
supplémentaire dans `nd->links`, que `navi_create_lists()` écrit et fait
participer aux tables de distance. Moonlight ne l'appelait **nulle part**
(`moon/` : zéro occurrence) ; le seul usage en amont est
`npc/custom/warper.txt`, **459 appels sur le warper universel** — exactement le
cas à ne pas reproduire, puisqu'il ramènerait n'importe quel trajet à un saut.

**La sortie tient dans le type 204.** Un lien 204 est ignoré par le pathfinder
tant que l'option « services » est éteinte : le graphe pédestre reste intact, et
demander le warper devient un choix explicite du joueur. C'est le levier que la
fenêtre native rend inatteignable en écrivant service et scroll ensemble (§4.3),
et que notre remplacement expose séparément.

Livré côté serveur le 2026-08-16 : champ `type` dans `struct navi_link`
(initialisé à 0 — `npc_data::navi` n'est pas value-initialisé), écriture dans
`write_warp`, 5ᵉ paramètre optionnel de `naviregisterwarp` (`"ssii?"`, validé
200-205), et un `OnInit` dans `moon/warp_agent.npc` déclarant **88 destinations
distinctes** (35 villes, 16 donjons de guilde, 37 directes) en type 204. Le bloc
est posé **avant** le garde `strnpcinfo(3) == "warp_agent"`, car les 38
duplicates partagent la `label_list` (`npc.cpp` : `nd->u.scr.label_list =
dnd->u.scr.label_list`) et exécutent donc ce `OnInit` chacun avec son propre
`oid` — c'est ce qui donne les 38 points de départ pour un seul bloc.

**Les deux champs d'AFFICHAGE que rAthena renseigne mal.** Une fois l'itinéraire
construit, le client dicte les étapes — et deux libellés viennent crus des `.lub` :

| Ce que le joueur lit | Champ | Défaut d'origine | Correctif |
|---|---|---|---|
| `Talk to gonryun_payon_16060` | `Navi_Link[5]` | nom du lien laissé **vide** ⇒ le générateur fabrique `<src>_<dst>_<id>` | passer `strnpcinfo(1)`, le nom **affiché** du PNJ |
| `move to [ payon[ payon ] ]` | `Navi_Map[2]` | `write_map` écrit `m->name` **deux fois** | `mapindex_idx2displayname(m->index)` |

Le 5ᵉ champ n'a pas besoin d'être unique — c'est l'**id** du lien qui identifie
l'arête, et le 6ᵉ (`""`, commenté « unique name ») n'est jamais rempli. Le laisser
vide « pour l'unicité » ne fait que troquer du lisible contre rien.

Pour les cartes, `db/map_index.yml` porte déjà un champ `Name:` (« Display name
for the map »), renseigné sur **1014 des 1322** entrées, et
`mapindex_idx2displayname()` **retombe seule sur le nom interne** quand il
manque — les ~300 cartes techniques (`nguild_*`, `siege_test`…) gardent donc
exactement l'ancien comportement. Aucun nom ne contient de guillemet, rien à
échapper. ⚠ Ce défaut-là n'est pas propre au chantier 204 : il touchait déjà
tous les trajets, y compris les warps ordinaires.

**Le coût, mesuré.** Les tables de distance ont enflé comme prévu :
`navi_linkdistance` est passé de 1,2 à **12,2 Mio**, `navi_npcdistance` à
6,1 Mio. Mais **68 %** du premier et **47 %** du second ne sont que des
annotations de fin de ligne (`-- ReachableFromDst warp (alberta, 15, 234)`),
écrites pour un lecteur humain — et **relues comme du Lua par le client à chaque
démarrage**, une entrée par appel `Lua_CallGlobal_va`. D'où l'option
`--no-comment` ajoutée au générateur : **18,3 Mio → 7,1 Mio, −61 %**, sans rien
perdre d'exploitable.

⚠ À ne pas confondre : cette option allège l'**écriture**, la taille du GRF et le
**chargement client**. Elle ne raccourcit pas le calcul, qui est dominé par les
A* de `navi_path_search` — un par paire *lien entrant × lien sortant* et par
carte. Le temps de génération, lui, reste à chronométrer.

⚠ Non encore mesuré : le reste du coût de génération. `navi_linkdistance` fait déjà
1,2 Mio et le générateur calcule un chemin A* par paire *lien entrant × lien
sortant* sur chaque carte ; 88 sorties nouvelles sur 38 cartes très fréquentées
feront grossir la table plus que linéairement. À chiffrer à la première
régénération.

---

## 4. Le protocole serveur

### 4.1 `ZC_NAVIGATION_ACTIVE` (`0x08E2`) — handler `0x00D058C0`

27 octets, décodés champ par champ par le handler :

| Offset | Taille | Champ | Usage mesuré |
|---|---|---|---|
| `+0` | 2 | opcode `0x08E2` | — |
| `+2` | 1 | `type` | passé tel quel en 7ᵉ argument de `SearchRoute` |
| `+3` | 1 | `flag` | **décimal codé bit à bit** (§4.3) |
| `+4` | 1 | `hideWindow` | passé en 9ᵉ argument |
| `+5` | 16 | `map[16]` | nom de carte, lu en `std::string` |
| `+21` | 2 | `x` | — |
| `+23` | 2 | `y` | — |
| `+25` | 2 | `mob_id` | — |

C'est exactement la structure `PACKET_ZC_NAVIGATION_ACTIVE` de rAthena
(`clif_navigateTo`), donc la commande de script `navigateto` fonctionne telle
quelle sur Moonlight.

### 4.2 Les types de cible (`type`)

Lus dans le `switch` de `CNavigation_SearchRoute` :

| `type` | Résolution de la cible |
|---|---|
| `0` | une **cellule** de la carte : la `.gat` est chargée et `(x, y)` doit être **praticable** |
| `1` | la **carte seule** — `x`/`y` ne sont jamais lus |
| `2` | **NPC / coordonnées** : `0x00B26BD0(x, y)` |
| `3` | **monstre** : `0x00B26940(mob_id)` |
| `> 4` | rejeté (`SearchRoute` rend 0 immédiatement) |

🔴🔴 **`0` et `1` ne sont PAS interchangeables** — cette table l'a d'abord laissé
croire, et c'est ce qui a fait qu'un bouton « Y aller » ne démarrait aucune
navigation pendant que le « Find » natif y arrivait. La distinction se joue dans
`CNavigation_PrepareDestination` (`0x00B39030`), sur `this+0x1124` :

```c
else if ( v5 == 1 ) { v12 = this[1098]; }        // carte seule : x/y JAMAIS lus
else {
  if ( v5 ) return 0;
  param_6 = a2; param_7 = a3;                     // ── type 0 ──
  ...
  if ( !sub_A78840(...)                           // charge la .gat de la cible
    || !sub_A784C0(a2, a3) )  return 0;           // ⚠ la CELLULE est-elle praticable ?
}
```

Un `return 0` ici fait rendre **`-97`** à `BuildRoute` (`0x00B30070`), donc **`0`**
à `SearchRoute`, et l'appelant qui ne lit pas ce retour observe un bouton qui
« ne fait rien ». Or `(0, 0)` n'est praticable sur **aucune** carte de RO :
demander le type `0` sans coordonnées, c'est demander un trajet vers un coin de
mur. Le serveur applique exactement la même règle — `clif_navigateTo` n'émet le
type `0` que `if (x > 0 && y > 0)`, et bascule sur `1` sinon.

**Règle** : pas de coordonnées sûres ⇒ type `1`. Et comme une coordonnée de NPC
tirée du `.lub` peut tomber sur une case bloquée, le repli de `0` vers `1` en cas
de refus vaut mieux qu'un « aucun chemin » mensonger.

### 4.3 🔴 Le champ `flag` est un masque de bits **écrit en décimal**

Le handler le décode ainsi (mesuré sur `0x00D058C0`) :

```
b = 0
si flag >= 100 : b |= 1 ; flag -= 100     → autoriser le SERVICE (Kafra)
si (reste) >= 10 : b |= 2 ; flag -= 10    → autoriser l'AVION
si (reste) >= 1  : b |= 4                 → autoriser les SCROLLS
```

Les trois bits atterrissent respectivement en `CNavigation+0x1280`, `+0x1281`,
`+0x1282` et sont passés au pathfinder `0x00B21E70`. Ce sont **les mêmes trois
options** que les trois cases à cocher de la fenêtre morte v1
(`MSI_NAVIGATION_USESERVICE` / `USEPLANE` / `USESCROLL`) — mais la fenêtre
vivante 203 n'en expose **plus qu'une seule** (le toggle `this[61]`, cmd 213),
et ce toggle écrit `service` **et** `scroll` ensemble en forçant `avion` à 0 :

```
cmd 213 :  byte_15C430D = 0 ;  byte_15C430E = (p2 != 0) ;  byte_15C430C = (p2 != 0)
```

**Deux des trois options du moteur sont donc devenues inatteignables depuis
l'interface** ; seul le serveur peut encore les régler, via le `flag` du paquet.
C'est un défaut à corriger (§6).

### 4.4 « Share » — la balise de chat, et l'import de `Navigationdata`

**Partager** (`cmd 354` → `0x005AB550`) ne parle pas au serveur : il
**pré-remplit la barre de saisie du chat**. Le geste exact, dans l'ordre :

1. il compose `<NAVIL>` + nom de carte + `x` + `y` + `</NAVIL>` ;
2. les deux coordonnées sont encodées en **base 62** (`0123456789abc…XYZ`) par
   `0x007FBCE0`, **poids faible d'abord**, toujours sur **2 caractères** (la
   boucle `do…while` en émet un, plus un final) ;
3. si la barre de saisie est repliée, il la **déploie**
   (`UINewChatWnd_ToggleInputBar`) ;
4. il envoie le texte à la fenêtre focalisée par `OnMsg(0, 153, texte)`.

Le joueur n'a plus qu'à valider. À l'arrivée, la balise est un lien cliquable :
`UIRichTextBox_OnMsg` la reconnaît et rappelle `CNavigation_SearchRoute` (§5).
⚠ Ce qui est partagé est la **destination courante de l'itinéraire**, pas la
position du joueur.

Les deux fragments sont des `std::string` globales bâties au démarrage :
`0x011DF260` = `<NAVIL>` (7 caractères, `0x0040C620`) et `0x011DF278` =
`</NAVIL>` (8 caractères, `0x0040C5C0`).

🔴 **Rappeler cette fonction depuis ImGui demande une précaution.** Elle écrit
dans la fenêtre **focalisée** (`UIWindowMgr_GetFocusedWnd` = `mgr + 416`), ce qui
va de soi quand c'est l'`OnMsg` de la fenêtre 203 qui l'appelle — celle-ci a le
focus. Depuis un clic dans notre overlay, **aucune fenêtre native n'est
focalisée** : le champ vaut `0` et l'appel déréférence un pointeur nul. Le
symptôme observé n'est pas un plantage mais **« le bouton ne fait rien »**,
l'exception étant absorbée par le `__try/__except` qui entoure nos appels natifs.

La parade tient en une ligne avant l'appel : donner le focus à la barre de saisie
du chat, `UIWindowMgr_SetFocusedWnd` (`0x00A4B760`, `__thiscall(mgr, wnd)`) sur
`g_pNewChatWnd` (`0x0131F6B0`) enfant `+0xBC`. La native retrouve alors l'état
qu'elle attend et fait tout le reste, dépliage de la barre compris.

**Le bouton « dossier »** (`cmd 191`) liste `%s\Navigationdata\*.txt` et charge
le fichier choisi (`0x00902A00`) : ouverture en `"rb"`, découpage par `strtok`
sur tabulation / espace / virgule / fin de ligne, chaque jeton alimentant un
itinéraire (`0x00B2B5F0`), puis un message `911` si le résultat n'est pas vide.

🔴 **Le client ne sait PAS enregistrer ces fichiers.** Les deux seuls accès au
dossier dans tout le binaire (`0x005AA8D0` et `0x00902A00`) ouvrent en `"rb"` :
il n'existe **aucun export**. Ce sont des itinéraires préparés hors du jeu, à
déposer à la main. Un « enregistrer l'itinéraire » serait donc une fonction
**nouvelle**, pas une reprise du natif — et elle serait relue par le natif
lui-même, puisque c'est lui qui sait charger.

---

## 5. Qui déclenche une navigation ?

Appelants de `CNavigation_SearchRoute` (`0x00B314F0`) :

| Appelant | Contexte |
|---|---|
| `0x00D058C0` | **paquet serveur** `ZC_NAVIGATION_ACTIVE` |
| `Chat_HandleChatMessage` (2×) | commandes de chat |
| `UIMiniMapWnd_OnMsg` (2×) | clic sur la **grande carte** (273) |
| `UIMinimapZoomWnd_OnMsg` | clic sur le **radar** (14) |
| `UIRichTextBox_OnMsg` | clic sur un **lien `<NAVI>`** du chat / d'un dialogue |
| `0x007C2200`, `0x00803F50`, `0x00981DF0`, `0x0099A030`, `0x0099A200`, `0x00C65370` | autres points d'entrée (journal de quête, agence, etc. — à qualifier) |

Appelants de `CNavigation_SearchByName` (`0x00B31980`) : `0x00803F50`,
`UIRichTextBox_OnMsg`, `0x008F72F0`, `0x008FEEB0` (2×), `0x00981DF0`.

Le rendu des liens `<NAVI>` du texte riche est déjà cartographié :
`UIRichTextBox_ParseNaviLinkSegment` (`0x0084B6B0`) et
`UIRichTextBox_LayoutNaviTagLinks` (`0x0083EA20`) — voir
[[project_ui_richtext_link_system]].

---

## 6. Les défauts du natif — cahier des charges du remplacement

Établis par lecture du code ; ceux marqués ✅live ont été confirmés en jeu.

1. ✅ **Les données sont saines — ce « défaut » n'en est pas un.** Corrigé le
   2026-08-16 après une fausse alerte, dont le récit est en §7 : il vaut la
   lecture, car l'erreur était méthodologique. Les six fichiers
   `navi_map / npc / mob / link / linkdistance / npcdistance` du GRF sont
   **exactement** ceux que `navi_create_lists()` a produits depuis le serveur
   Moonlight — **md5 identiques** à
   `moonlight/generated/clientside/data/luafiles514/lua files/navigation/`.
   Ils font autorité : le générateur lit la mémoire vive du map-server
   (`m->moblist[]`, `nd->navi`), il ne recopie rien de kRO.

   Ce qui reste **non généré**, donc encore d'origine kRO :

   | Fichier | État |
   |---|---|
   | `navi_scroll_krpri.lub` | bytecode kRO et **vide** (`Navi_Scroll = { "NULL" }`) ⇒ aucune arête « scroll » n'existe, et l'option correspondante est **sans effet** |
   | `navi_picknpc_krpri.lub` | bytecode kRO (18 Ko) — renseigne `[22..30]` des nœuds NPC |
   | `navi_f_krpri.lub` | table de traduction communautaire (`zackdreaver / ROenglishRE`) |

   Le seul risque résiduel est la **fraîcheur** : les `.lub` sont un instantané
   des spawns et des NPC à l'instant de la génération. Toute modification de
   `moon/` les périme **en silence** — ni le client ni le serveur ne s'en
   plaignent. D'où l'intérêt de rejouer le protocole de §7 après chaque
   campagne de scripts.
2. **Deux des trois options d'itinéraire sont inatteignables** (§4.3) : le
   joueur ne peut pas dire « pas d'avion » ou « pas de scroll » séparément.
3. **Les fenêtres satellites ne suivent pas la principale** : 306 et 314 sont
   positionnées une fois, à leur création, d'après la position de 203.
4. **Trois fenêtres pour une tâche** (recherche, itinéraire, choix d'icône) là
   où un seul panneau suffirait.
5. **Le filtre est un combo à 4 entrées** (Tout/Carte/NPC/Monstre) : pas de
   recherche incrémentale, pas de tri par distance, pas d'historique visible —
   `0x015C42F4` mémorise pourtant le dernier terme.
6. **Aucune tolérance de saisie** : la recherche est exacte, sur un champ limité
   à 30 caractères, et le client ne connaît que les libellés des `.lub`.
7. **Un échec de chargement ouvre une `MessageBoxA` bloquante** (§3.2).
8. **Le libellé des résultats est `"[%d]%s"`** — l'identifiant brut est montré
   au joueur.
9. La liste secondaire (`this[60]`, 132×80) n'affiche que quelques étapes ;
   l'itinéraire complet exige d'ouvrir la 314.

### Ce qu'il faut **garder** du natif

Le remplacement ImGui ne doit surtout pas réimplémenter le moteur :
`CNavigation` (`0x015C3090`) porte le graphe, le pathfinder, le suivi et le
rendu de la route sur la carte. Le plan est de piloter ce moteur
(`Search` / `BuildRoute` / start / stop) et de ne remplacer que la **surface**,
comme pour les autres fenêtres déjà converties.

---

## 7. Protocole de mesure des données (reproductible)

### 7.1 🔴 La fausse alerte — à lire avant de refaire le diff

Ce document a d'abord affirmé, mesures à l'appui, que les monstres du fichier
étaient ceux de kRO : « Baphomet est annoncé à `gef_dun03` alors qu'il *spawne*
à `prt_maze03` », 582 couples introuvables, 19 % de quantités fausses.
**C'était faux, et c'est la méthode qui l'était.**

Trois indices auraient dû alerter plus tôt :

- les cartes (1300/1301) et les warps de `prontera` (21/21, coordonnée par
  coordonnée) correspondaient **parfaitement** au serveur — un fichier kRO
  n'aurait jamais ce niveau d'accord ;
- le nœud `prontera` en mémoire portait **exactement** 81 NPC et 21 liens, les
  comptes du fichier ;
- `navi_map` annonçait 1301 cartes, et le client en avait chargé 1301.

La vérification qui tranche est immédiate : **`md5sum`** entre les `.lub` du GRF
et `moonlight/generated/clientside/data/luafiles514/lua files/navigation/`.
Ils sont **identiques** — le GRF *contient déjà* la génération serveur.

L'erreur était dans la « vérité serveur » à laquelle je comparais : mon
extraction n'acceptait un spawn que si le 4ᵉ champ commençait par un **id
numérique** (`awk '$4 ~ /^[0-9]+,/'`). Or les spawns custom de Moonlight
s'écrivent avec la **constante** du mob et **sans coordonnées** :

```
moon/mobs/mvps.npc:151
gef_dun03  boss_monster  Baphomet    BAPHOMET,1,7200000,600000,"classement::OnMvpDead"
                                     ^^^^^^^^ constante, pas 1039

npc/pre-re/mobs/dungeons/gef_dun.txt:77
//gef_dun03,0,0  monster  Baphomet   1039,1,...     <- et la ligne rAthena est COMMENTÉE
```

Le fichier avait donc raison sur toute la ligne : sur Moonlight, Baphomet
*spawne* bien à `gef_dun03`. Mes « 582 introuvables » et « 39 fantômes » étaient
les spawns que **mon** filtre jetait.

**Leçon** : le générateur lit `m->moblist[]` dans la mémoire du map-server — il
voit les spawns *effectivement chargés*, quelle que soit leur écriture. Un
`grep` sur les sources, lui, ne voit que du texte : il rate les constantes et
les fichiers non chargés, et compte les lignes commentées. Quand un diff oppose
un fichier **généré par le serveur** à un `grep` sur les scripts, **c'est le
`grep` qui est suspect**. Cf. [[feedback_absence_needs_measurement]] et la règle
« une contradiction = une prémisse fausse » de [[feedback_re_method]].

### 7.2 Le protocole, corrigé

À rejouer après chaque campagne de scripts, pour détecter la **péremption** des
`.lub` — le seul risque réel (§6, défaut n°1) :

1. **Comparer les empreintes d'abord.** `md5sum` entre les six fichiers du GRF
   et ceux de `generated/clientside/…`. S'ils diffèrent, le GRF est **périmé** :
   c'est la seule conclusion à tirer, et elle suffit.
2. **Extraire les `.lub` du GRF** s'il faut inspecter : `moonlight.grf` est un
   GRF 2.0 standard (en-tête 46 octets, table en `46 + offset` compressée zlib,
   entrées `nom\0` + `csz, csz_align, usz, flags, off`) ; les neuf fichiers sont
   sous `data\luafiles514\lua files\navigation\`. ⚠ `data.grf` **n'est pas**
   lisible ainsi (son magic est `Event Horizon`, pas `Master of Magic`) — mais
   `DATA.INI` donne la priorité à `moonlight.grf`, qui les contient tous.
3. **Parser le Lua texte** avec les motifs de §3.2 : ce sont des listes
   d'accolades à plat, une regex par table suffit.
4. **Ne pas reconstruire la « vérité serveur » au `grep`.** Si une comparaison
   sémantique est vraiment nécessaire, **relancer la génération** et diffuser les
   deux sorties : c'est la seule référence qui voie ce que le serveur voit.

## 8. Reste à mesurer

- [ ] `DrawContent` (`0x005A7A40`) : dessin exact du panneau, et ce que fait le
      mode réduit (`byte_015C42E4`).
- [ ] Les slots surchargés `+0x2C`, `+0x4C`, `+0x7C`.
- [x] ~~Structure des nœuds carte (`0x68`)~~ — **faite et vérifiée live** (§3.4).
- [x] ~~Nœud NPC/mob (`0x7C`)~~ — **fait** (§3.4) : structure recoupée avec
      l'ordre des colonnes du `.lub` et les quatre slots virtuels utiles.
      🔴 `+0x44`/`+0x48` changent de sens selon le type.
- [ ] Nœud lien / warp (`0x98`, ctor `0x00B24790`).
- [x] ~~Format d'un élément de groupe~~ — c'est un `std::vector<int>` de 12 octets (§3.5).
- [ ] Le rendu de la route **sur la minimap** et la flèche à l'écran
      (`0x00903BC0` / `0x00903D70` / `0x00903E20`, appelés avec `dword_0131F8E8`).
- [ ] Le rôle exact de `UINavigationroadiconWnd` (306) et de ses
      `btn_roadIocn_select%d`.
- [ ] Les six appelants non qualifiés de `SearchRoute` (§5).
- [x] ~~Le contenu réel de `navi_scroll_krpri.lub`~~ — **vide** :
      `Navi_Scroll = { "NULL" }`, donc aucune arête « scroll » n'existe et
      l'option correspondante est sans effet sur cette version des données.
- [ ] La touche exacte du raccourci clavier (§9.1) : le `case` de
      `UIWindowMgr_DispatchHotkeyBehavior` @ `0x00A45D8B` n'a pas été relevé.
- [ ] `sub_8FEEB0` / `sub_8FB8C0`, qui appellent aussi `MakeWindow(0xCB)`.

---

## 9. Blueprint du remplacement ImGui

### 9.1 Interception — trois chemins, un seul point de passage

La fenêtre 203 s'ouvre par **trois** routes, et **toutes** appellent
`UIWindowMgr_MakeWindow(0xCB)` :

| Route | Site |
|---|---|
| bouton du menu d'icônes (cmd **430**) | `UIMenuIconWnd_OnMsg` @ `0x00814CD6` |
| **raccourci clavier** | `UIWindowMgr_DispatchHotkeyBehavior` @ `0x00A45DB6` — ⚠ le même *behavior* ouvre d'abord la **carte du monde** si elle est déjà ouverte (`dword_0131F8E8` non nul → il lui envoie `cmd 200` et s'arrête) |
| moteur | `CNavigation_SearchByName` @ `0x00B31A04` et `CNavigation_SearchRoute` @ `0x00B317FF` — c'est ce qui ouvre la fenêtre quand un **lien `<NAVI>`**, la **grande carte** ou le **serveur** demandent une navigation |

Donc la recette éprouvée du projet s'applique telle quelle
([[reference_native_window_toggle_router]]) : **masquer `+0x28` dans le hook
`MakeWindow`, puis détruire au tick** via `uiwnd::CloseWindow(203)`.

✅ **Vérifié : cette fenêtre ne parle pas au serveur.** Aucun appel à
`CRagConnection_SendPacket` (`0x00C14920`) dans toute la plage de la classe
(`0x005A3F00`–`0x005AC000`) — la navigation est **entièrement locale**. Il n'y a
donc ni devoir de naissance ni devoir de mort à rejouer, contrairement au
courrier `0x108` de [[project_rodex_re]]. Détruire tôt est sans danger.

⚠ Penser aux **satellites** : détruire 203 doit aussi fermer 306 et 314 (et 229),
sans quoi elles restent à l'écran, orphelines et sans parent pour les
repositionner.

### 9.2 Ce qu'on pilote, ce qu'on redessine

**Ne rien réimplémenter du moteur.** Le graphe, le pathfinder, le suivi et le
surlignage de la carte du monde restent natifs ; on ne remplace que la surface.
Les points d'attache, tous `__thiscall` sur `CNavigation = 0x015C3090` :

| Geste de l'interface | Appel natif |
|---|---|
| chercher | écrire le filtre en `+0x1260` et le terme (`std::string`) en `+0x1264`, puis `0x00B34970` |
| lire les résultats | `0x00B2E700(&out, i)` sur `[0x015C4348, 0x015C434C)` (§3.5) |
| aller à (x, y) | `0x00B30070(x, y)` puis `0x00B39460` |
| arrêter | `0x00B35F80` |
| étapes de l'itinéraire | `0x00B39660()` puis `0x00B2EE00(i)` |
| les trois options | écrire `0x015C430C` / `0x015C430D` / `0x015C430E` |
| API « va là » complète | `CNavigation_SearchRoute` (`0x00B314F0`) — même entrée que le serveur |

🔴 **Ne pas appeler ces fonctions depuis la frame ImGui** : les empiler dans une
file d'actions consommée au tick, comme partout ailleurs dans Bourgeon
([[feedback_imgui_pitfalls]]).

### 9.3 Ce que l'interface doit corriger

Reprise point par point des défauts de §6 :

| Défaut | Correction |
|---|---|
| 2 — deux options mortes | **trois cases à cocher** distinctes (service Kafra / avion / scroll), écrites dans les trois octets ; afficher « sans effet » sur *scroll* tant que `Navi_Scroll` est vide |
| 3 — satellites qui ne suivent pas | **un seul panneau**, résultats et itinéraire côte à côte |
| 4 — trois fenêtres | idem ; l'aide devient une infobulle |
| 5 — combo à 4 entrées | **filtres en pastilles** (Tout / Cartes / NPC / Monstres) + **recherche incrémentale** (le moteur est local, relancer à chaque frappe ne coûte pas de réseau) + **historique** (le natif garde déjà le dernier terme en `0x015C42F4`) |
| 6 — recherche exacte, 30 caractères | filtrage tolérant côté Bourgeon (sous-chaîne, insensible aux accents et à la casse) sur les résultats du moteur |
| 7 — `MessageBoxA` bloquante au chargement | ne rien changer au chargement, mais **journaliser** l'état des tables au premier affichage ([[feedback_debug_tooling]] : une preuve, une seule fois) |
| 8 — libellé `"[%d]%s"` | une **table** : nom, type, carte, coordonnées, nombre d'occurrences |
| 9 — étapes tronquées | l'itinéraire complet, toujours visible, avec l'étape courante mise en évidence |
| 1 — données | **rien à corriger** : les `.lub` du GRF sont la génération serveur (§6). L'interface peut s'y fier — y compris pour le niveau, la race et l'élément des monstres, déjà présents dans `Navi_Mob` |

Apports propres au remplacement :

- **Distinguer les MVP** : le type `301` est déjà dans les données (72 entrées) —
  les marquer, et croiser avec [[project_mvp_tracker]] qui connaît, lui, les
  temps de réapparition **réels** du serveur. C'est la synergie la plus
  intéressante du chantier : le tracker sait *quand*, la navigation sait *où*.
- **Clic droit sur un résultat** : aller / marquer / copier le nom — geste déjà
  standard dans nos cellules ([[feedback_ui_conventions]]).
- Respecter le skin RO ([[project_ro_skinning]]) et les accents français.

### 9.4 Ordre de bataille proposé

✅ Étape 0 — **déjà faite** : les `.lub` sont générés depuis le serveur et
publiés dans `moonlight.grf` (md5 vérifiés le 2026-08-16). Le socle de données
est sain ; il ne reste qu'à le maintenir à jour après les campagnes de scripts
(§7.2).

✅ Étape 1 — **écrite** (2026-08-16, pas encore compilée ni essayée en jeu) :
`src/features/windows/navigation_window.{h,cc}`, enregistrée dans
`Bourgeon::LoadPlugins`, ouverte par l'action `win_navigation` du catalogue de
raccourcis. **Coexistence** : la native 203 n'est pas routée. Le panneau cherche,
liste, guide et arrête ; il expose les trois options séparément.

Deux pièges rencontrés en l'écrivant, qui valent pour tout futur appelant :

- 🔴 **`0x00B35F80` n'arrête rien** — c'est `SelectResult` (§3.3). L'arrêt du
  guidage, c'est sa *branche d'index invalide* : mettre `+0x125C` à 0, `+0x12C4`
  à −1, puis l'appeler. C'est exactement ce que fait le `cmd 284`.
- 🔴 **Le 2ᵉ champ d'un résultat est un POINTEUR d'objet**, pas un identifiant :
  pour un NPC ou un monstre, son nom vient du slot `+0x14` et **sa carte** du
  slot `+0x20`. Le prendre pour un entier donne une fenêtre qui affiche juste et
  navigue faux — le nom d'un NPC n'est pas un nom de carte.

Et un piège de compilateur : MSVC refuse `__try` dans une fonction qui déroule
des objets C++ (C2712). Tous les appels natifs sont donc isolés dans des
enveloppes sans `std::string` ni `std::vector`.

2. Router `MakeWindow(203)` (+ 306 / 314 / 229) vers le panneau.
3. Le confort : tri par distance, historique, marquage des MVP (type `301`)
   croisé avec [[project_mvp_tracker]].

## 10. Le TRACÉ AU SOL — et la liste de cellules qu'il cache

> **Ce chapitre corrige une affirmation antérieure de ce document.** On y lisait
> que le chemin à l'intérieur d'une carte « n'existe nulle part sous forme de
> liste » et qu'un itinéraire n'était qu'une suite de CARTES. C'était une
> supposition, pas une mesure : la liste existe, elle est cellule par cellule, et
> `CNavigation` la garde toute prête.

### 10.1 Comment on l'a trouvée

Le fil conducteur était le jeu d'icônes de la trace (§ précédent, `+0x1344`).
Ses huit chemins d'animation vivent en `+0x1180`, mais **aucune recherche
d'immédiat `0x1180` ne trouve leur lecteur** — piège à connaître :

- `find type=immediate` d'IDA ne voit **que les vrais opérandes immédiats**,
  jamais un déplacement de référence mémoire. `mov [ecx+1344h], eax` est
  invisible à cette recherche ; `search_text` sur le listing, borné par plage,
  les voit tous et va vite ;
- et de toute façon le lecteur n'écrit pas `0x1180` : le compilateur a replié
  `this + 24*dir + 4480` en `this + 8*(3*dir + 560)`.

Le vrai chemin d'accès était la liste des références à `g_Navigation`
(`0x015C3090`, 170 xrefs) : elle contient `CScene_RenderCellsAndCursor`
(`0x00A7B0A0`), c'est-à-dire le rendu de la SCÈNE 3D, pas une fenêtre.

### 10.2 Les trois fonctions

| Adresse | Nom donné | Rôle |
|---|---|---|
| `0x00B31C40` | `CNavigation_RenderGroundTrail` | pose les traces au sol, **chaque frame**, appelée par `CScene_RenderCellsAndCursor` |
| `0x00B2FC30` | `CNavigation_BuildCellPath` | lance l'A★ de déplacement du client et **remplit les deux listes** |
| `0x00B2D760` | `CNavigation_RefreshOnMapEnter` | recalcule tout à l'entrée d'une carte |

🔴 Le rendu de la trace ne dépend d'**aucune fenêtre**. Tuer les quatre natives
(§2) ne l'éteint pas — c'est pourquoi la trace au sol continue de fonctionner
sous l'interface moderne, et pourquoi il faut un vrai `ClearRoute` pour l'arrêter.

### 10.3 `CNavigation_BuildCellPath` — la source du tracé

```
bool __thiscall CNavigation_BuildCellPath(nav, int sx, int sy, int dx, int dy, bool fill_minimap)
```

Appelle `Pathfind_AStarSearch` (`0x00A777B0`) sur la **.gat de la carte
courante** — le même A★ que le déplacement du personnage, donc le tracé suit
réellement le relief et contourne les obstacles — puis remplit **deux** listes :

**a) le chemin en CELLULES**, `nav+0x1164 / +0x1168 / +0x116C` (begin / end / cap).
Élément de **16 octets**, layout mesuré sur `Pathfind_ReconstructPath`
(`0x00A77660`) :

| Offset | Type | Sens |
|---|---|---|
| `+0` | `int` | `x` de la cellule |
| `+4` | `int` | `y` de la cellule |
| `+8` | `int` | `dir` **0..7** · `0`=+Y `1`=−X+Y `2`=−X `3`=−X−Y `4`=−Y `5`=+X−Y `6`=+X `7`=+X+Y · **impair = diagonale** |
| `+12` | `int` | temps de marche cumulé (ms) — inexploitable ici, le guidage passe une vitesse de `1` |

La reconstruction remonte la chaîne des parents depuis l'ARRIVÉE en écrivant à
l'envers : **la suite est ordonnée départ → arrivée**. `dir` est la direction qui
MÈNE À la cellule, d'où le décalage à l'affichage (ci-dessous).

Le conteneur n'est pas un simple `vector` : `+0x1170` (int, remis à 0),
`+0x1174 / +0x1178` (float) = la position **sous-cellule** du départ, et
`+0x117C` (bool) = **la trace est active**. C'est ce dernier qui garde tout.

**b) le chemin en pixels MINIMAP**, `nav+0x1294 / +0x1298 / +0x129C`.
Élément de 8 octets `{int mx; int my;}`, doublons supprimés, calculé par

```
s = Minimap_FitScaleAndOffset(&offX, &offY, node.width, node.height, 124.0f)
mx = (int)(x * s + offX)
my = (int)(126.0f - (y * s + offY))
```

puis `(*(vt+152))(minimap_wnd)` pour la faire redessiner.

🔴 **Le natif dessine donc bien l'itinéraire sur sa minimap**, cellule par
cellule. Mais dans un carré de 128 pixels figé : au zoom, la liste est
inutilisable. C'est pourquoi Bourgeon repart des CELLULES (`+0x1164`).

### 10.4 `CNavigation_RenderGroundTrail` — ce que le natif en fait

- gardée par `+0x117C` ;
- **une cellule sur deux** (`index += 2`) ;
- la flèche posée en `k` porte la direction de `k+1` — sinon la dernière
  pointerait dans le vide ;
- 🔴 **ne dessine que les cellules à ±10 du joueur.** C'est LE défaut : la trace
  aide à *suivre* un chemin, jamais à le *trouver* ;
- animation de chasse : `(timeGetTime()%1000)/100 == index/2` → alpha `0xEF` au
  lieu de `0x9A` ;
- si **aucune** cellule n'est visible, relance `CNavigation_BuildCellPath` depuis
  la position courante — c'est le seul recalcul en cours de marche. Le début du
  tracé traîne donc jusqu'à dix cellules derrière le joueur : normal, pas un
  défaut de lecture.

### 10.5 L'ARRÊT du guidage

`CNavigation_ClearRoute` @ `0x00B2F080`, `__thiscall(nav, bool full)`.
Vide le chemin de cartes (`+0x1150/+0x1154`), le chemin de cellules (`+0x1164`),
la liste minimap (`+0x1294`), remet `+0x117C` à 0 et détache le rendu au sol
(`+0x1140`).

Le bouton d'annulation du natif (`0x005AA21F`) fait :

```
dword [g_Navigation+0x125C] = 0    // état de suivi
byte  [g_Navigation+0x1254] = 0    // témoin lu par la carte du monde
ClearRoute(g_Navigation, 1)
```

⚠ **`full = 1` détruit AUSSI le vecteur de résultats de recherche** (`+0x10F0`,
celui de `CNavigation_GetResult` / `CNavigation_Search` / `SelectResult` /
`RebuildResultGroups`). La listbox native se recharge, la nôtre non : Bourgeon
passe donc **`full = 0`**, ce qui arrête le guidage sans effacer la recherche du
joueur sous ses yeux.

### 10.6 Ce que Bourgeon en fait

`NavigationWindow::RouteCellPath(out, max)` recopie le chemin **décimé aux
changements de direction** — premier point, dernier point, et chaque coin. Entre
deux coins la trajectoire est une droite, que la ligne brisée restitue au pixel
près : un chemin de huit cents cellules tient en une trentaine de points.

La minimap ImGui (`features/overlays/minimap.cc`) le trace en entier, sous sa
propre transformation cellule → écran, donc **exact à tous les niveaux de zoom**
et **visible sur toute sa longueur**, là où le natif n'en montre que dix cellules.
