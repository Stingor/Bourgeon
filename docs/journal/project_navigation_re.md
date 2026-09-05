# Navigation (fenêtre 203) — RE et remplacement ImGui

> Journal du chantier. La fiche de mémoire `project_navigation_re` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

RE complète le 2026-08-16, fenêtre ouverte en jeu + x32dbg attaché.
Doc : `docs/navigation_re.md`. IDB renommée et commentée.

🔴 **Deux générations, une seule vivante.** `UINaviWnd` / `UINavigationWnd`
(skin `navigation_interface\`, boutons `NaviSearch/NaviStart/NaviRoute/NaviList`,
`OnCreate 0x008E7990`) est **morte** : son ctor `0x008DB750` n'a aucun appelant
dans `MakeWindow`. La vivante est **`UINavigationV4Wnd`, id 203 (`0xCB`)**,
skin `navigation_interface3\`, vtable `0x00FD95EC`, OnCreate `0x005A65F0`,
OnMsg `0x005A9410`, global `0x0136E57C`. Satellites : **314** itinéraire
(x+272), **306** choix d'icône (x+113/y+211), **229** aide — toutes trois
positionnées **une seule fois**, à leur création, d'après la position de 203.

🔴 **Il n'y a pas de « globaux » de navigation.** `0x015C3090` = `g_Navigation`
(CNavigation, statique) et tous les `byte_15C42xx` / `dword_15C43xx` sont ses
**champs** (`0x015C4348 − 0x015C3090 = 0x12B8`). Cartes en `+0x00/+0x04`
(1301 ✅live), résultats en `+0x10F0/+0x10F4` (40 o/élément), groupes en
`+0x12B8/BC`, filtre `+0x1260`, terme `+0x1264`, options `+0x127C/D/E`.
Un seul objet à connaître pour tout piloter.

🔴 **Le `flag` du paquet `ZC_NAVIGATION_ACTIVE 0x08E2` est un masque de bits
écrit en DÉCIMAL** : centaine → service Kafra, dizaine → avion, unité → scroll.
Le toggle unique de la fenêtre (cmd 213) écrit **service et scroll ensemble et
force avion à 0** — ✅live : `1 / 0 / 1`. Deux des trois options du moteur sont
donc inatteignables depuis l'interface.

✅ **Les données sont SAINES : les `.lub` du GRF SONT la génération serveur.**
Les six `navi_map/npc/mob/link/linkdistance/npcdistance` de `moonlight.grf` ont
des **md5 identiques** à
`moonlight/generated/clientside/data/luafiles514/lua files/navigation/`
(`navi_create_lists()`, `src/map/navi.cpp`). Ils sont en **Lua TEXTE**, ce qui
est justement la signature de la génération. Non générés, donc encore kRO :
`navi_scroll` (bytecode et **vide** : `{"NULL"}` ⇒ l'option scroll est sans
effet), `navi_picknpc`, et `navi_f` (traduction ROenglishRE). Seul risque réel :
la **péremption** après une campagne de scripts `moon/` — vérifier par `md5sum`.

🔴🔴 **PIÈGE DE MÉTHODE, commis puis corrigé dans cette session** : j'ai d'abord
« démontré » que les mobs étaient faux (« Baphomet annoncé à `gef_dun03` au lieu
de `prt_maze03` », 582 introuvables). **C'était mon extraction qui était fausse**
— un `awk` qui n'acceptait un spawn que si le 4ᵉ champ commençait par un **id
numérique**, alors que les spawns custom Moonlight s'écrivent avec la
**constante** du mob et **sans coordonnées**
(`moon/mobs/mvps.npc` : `gef_dun03  boss_monster  Baphomet  BAPHOMET,1,...`),
la ligne rAthena concurrente étant en plus **commentée**. Règle à retenir :
**quand un diff oppose un fichier GÉNÉRÉ PAR LE SERVEUR à un `grep` sur les
scripts, c'est le `grep` qui est suspect** — le générateur lit `m->moblist[]` en
mémoire vive et voit ce qui est *réellement chargé*. Trois indices auraient dû
alerter : cartes 1300/1301, warps 21/21 coordonnée par coordonnée, et le nœud
`prontera` portant exactement les 81 NPC / 21 liens du fichier.

✅ **Sans danger à détruire** : aucun appel à `CRagConnection_SendPacket` dans
toute la plage de la classe (`0x005A3F00`–`0x005AC000`) — la navigation est
**entièrement locale**. Trois chemins d'ouverture (menu d'icônes cmd 430,
raccourci `DispatchHotkeyBehavior` @`0x00A45DB6` — qui sert d'abord la carte du
monde si elle est ouverte —, et le moteur lui-même), **tous** via
`MakeWindow(0xCB)` : point d'interception unique, recette
[[reference_native_window_toggle_router]].

L'itinéraire est peint sur la **carte du monde** (`UIRoMapWnd` id 140,
`0x00903BC0`) — ⚠ mais **pas seulement** : voir plus bas, il l'est aussi sur la
minimap et au sol. Le type `301` de `Navi_Mob` marque les **MVP**
(72 entrées) → synergie directe avec [[project_mvp_tracker]] : le tracker sait
*quand*, la navigation sait *où*.

**État du chantier (2026-08-16)** : `src/features/windows/navigation_window.{h,cc}`
écrite — **pas encore compilée ni essayée en jeu**. Phase de **coexistence** : la
native 203 n'est PAS routée, le panneau s'ouvre par l'action `win_navigation`.
Deux pièges qui ont failli passer :
🔴 **`0x00B35F80` n'est PAS un arrêt** mais `SelectResult` (il pose la cible
depuis les index `+0x12C4`/`+0x12C8`) ; l'arrêt du guidage = sa **branche
d'index invalide** (`+0x125C = 0`, `+0x12C4 = -1`, puis appel), ce que fait le
`cmd 284`. 🔴 **Le 2ᵉ champ d'un résultat est un POINTEUR d'objet**, pas un id :
nom du NPC/mob par le slot `+0x14`, **sa carte** par le slot `+0x20` — sans quoi
la fenêtre affiche juste et navigue faux. Piège MSVC : `__try` interdit dans une
fonction qui déroule des objets C++ (C2712) ⇒ appels natifs isolés dans des
enveloppes sans `std::string`.

🔴 **`CNavi_Object` (le nœud d'un NPC / monstre, `0x7C`, ctor `0x00B24BF0`) : ses
deux derniers champs CHANGENT DE SENS** — `+0x44`/`+0x48` = **x/y** pour un NPC,
mais **niveau / stats empaquetées** pour un monstre (un monstre n'a AUCUNE
position : le `.lub` n'en donne pas). Les afficher en « (x, y) » donne un couple
absurde. Autres champs : `+0x08` sous-type (`101`/`102` boutique · `300`/`301`
**MVP**), `+0x0C` = `quantité << 16 | sprite`, `+0x40` nœud carte.
⚠ Le **slot 4** (`+0x10`) rend **la quantité**, pas le niveau (c'est `this[7]` en
`u16`) — le natif n'en tire qu'une tranche de densité (msg `0x991`..`0x995`).
Stats = `((ele_lv*20+def_ele)<<16) | (size<<8) | race`.

🔴🔴 **Le 3ᵉ champ d'un `Navi_Link` (le `200`) est un TYPE, et le pathfinder
REFUSE une arête sur ce seul critère** (`CNavigation_PathFind` `0x00B21F9B`,
`vt+8` = GetType) : `200` warp et `201` script passent toujours, **`204` exige
l'option « services »** (`+0x127C`), `205` l'option « avion » (`+0x127D`),
`400` (scroll, fabriqué en dur par le loader) passe. Or **rAthena n'émet que le
type 200** — sa boucle ne récolte que `subtype == NPCTYPE_WARP` — donc **tout
lieu qu'on n'atteint qu'en PARLANT à un PNJ est absent du graphe** : mesuré
**745 composantes connexes** pour 1301 cartes, 709 cartes isolées, la plus
grosse à 413. **`gonryun`, le hub de Moonlight, n'en atteint que 6.** Ce n'est
donc pas un bug de données mais une limite du générateur.

Le remède dormait dans rAthena : **`naviregisterwarp("<nom>","<carte>",x,y)`**
pousse une arête dans `nd->links`, que le générateur écrit ET fait participer
aux tables de distance. Moonlight ne l'appelait NULLE PART ; le seul usage
amont est `npc/custom/warper.txt` (459 appels sur le warper universel) —
justement le cas à fuir, il ramène tout trajet à **un saut**.
🔴 **La conciliation tient dans le type 204** : l'arête est invisible au
pathfinder tant que « services » est éteint, donc le graphe pédestre reste
intact et passer par le warper devient un choix. Laisser le **nom du lien vide**
fait générer un identifiant unique (`<src>_<dst>_<id>`), seule option correcte
quand 38 duplicates partagent le script.

**Livré serveur le 2026-08-16** (dépôt `moonlight`, branche `main`) : champ
`type` dans `struct navi_link` (**NSDMI à 0** : `npc_data::navi` n'est pas
value-initialisé, sinon on écrit des ordures dans le `.lub`), `write_warp` qui
l'honore, 5ᵉ paramètre optionnel de `naviregisterwarp` (`"ssii?"`, 200-205), et
un `OnInit` dans `moon/warp_agent.npc` déclarant **88 destinations** en 204.
🔴 Le bloc est posé **AVANT** le garde `strnpcinfo(3) == "warp_agent"` : les 38
duplicates **partagent la `label_list`** (`npc.cpp` : `nd->u.scr.label_list =
dnd->u.scr.label_list`) et exécutent donc ce `OnInit` chacun avec son `oid` —
un seul bloc suffit pour 38 points de départ. ✅ **RÉSULTAT FINAL (2026-08-17), avec 413 destinations déclarées** :
`navi_linkdistance` **2,15 Mio**, `navi_npcdistance` 4,54 Mio, `navi_link`
1,63 Mio — **9,2 Mio au total**, alors que la seule table de liens pesait déjà
12,2 Mio avant. Deux leviers cumulés : `--no-comment` (68 % / 47 % du volume
n'étaient que des annotations) et le **codage par plages** (83 % des passes sont
des ids consécutifs à coût identique : 153 k lignes → 2,2 k).
🔴🔴 **Mesuré en jeu : le temps de chargement du client n'a PAS bougé.** Le
goulot n'est donc pas le volume de texte mais les **~185 000 appels
`Lua_CallGlobal_va`**, un par entrée, que rien de tout cela ne réduit — le client
itère jusqu'à recevoir `nil`. ⇒ **Ne pas rechercher d'autre gain de chargement
du côté du format** ; seul un hook du chargeur côté client changerait la donne.
Le gain acquis est réel mais porte sur la taille du GRF, pas sur le démarrage.

Coût initial mesuré : `navi_linkdistance` 1,2 → **12,2 Mio**, `navi_npcdistance`
6,1 Mio — mais **68 % / 47 % ne sont que des annotations** `-- (...)` de fin de
ligne, que le client **relit comme du Lua à chaque démarrage** (un
`Lua_CallGlobal_va` par entrée). D'où l'option **`--no-comment`** ajoutée au
générateur (`gen_options`, exposée par `map.hpp` ; c'est un MODIFICATEUR, il ne
doit pas satisfaire seul le contrôle « au moins une option ») :
**18,3 → 7,1 Mio, −61 %**. ⚠ Elle allège l'écriture, le GRF et le chargement
client — **pas** le calcul, dominé par les A* de `navi_path_search` (un par
paire lien entrant × sortant et par carte). Temps de génération encore à
chronométrer. Le fichier `moon/warp_agent.npc` est en
**CP1252 / LF** — à préserver ; générateur rejouable :
`scratchpad/gen_navi_warpagent.py`.

🔴 **Déclarer une destination sans coordonnées : `(0,0)` ne « casse » pas, il
rend les coûts IMPRÉVISIBLES.** Vérifié dans `navi_path_search` (`navi.cpp`) :
la cellule de DÉPART n'est pas testée — le contrôle est **commenté en clair**
(« Do not check starting cell as that would get you stuck »). L'A* part donc du
coin de carte et, selon la carte, soit meurt file vide (aucune passe `E`), soit
**réussit avec des distances mesurées depuis un coin**. Comme le pathfinder du
client choisit au moindre coût, ces nombres faux lui feraient préférer ou éviter
le lien en silence. Côté client en revanche **rien ne casse** : un lien sans
passes reste valable comme DESTINATION (PathFind s'arrête dès qu'un lien de la
file mène à la carte visée), seulement pas comme étape de transit.
⇒ résoudre en une cellule praticable, par **balayage déterministe en anneaux
depuis le centre**. 🔴 **JAMAIS `map_search_freecell()`** avec un rayon négatif :
il tire **50 coordonnées AU HASARD** (échoue sur les cartes encombrées —
`ve_fild06` — et surtout rend la génération **non déterministe**, deux runs
donnant des `.lub` différents pour les mêmes scripts).

🔴 **Les deux libellés que rAthena renseigne mal**, visibles dès que
l'itinéraire se dicte : `Navi_Link[5]` est le nom AFFICHÉ (« Talk to … ») — le
laisser vide fait écrire `<src>_<dst>_<id>` ; passer **`strnpcinfo(1)`**.
L'unicité est portée par l'**id**, pas par ce champ (le 6ᵉ, « unique name »,
reste vide) : le laisser vide « pour l'unicité » troque du lisible contre rien.
Et `write_map` écrivait `m->name` **deux fois**, d'où « move to [ payon[ payon ] ] »
⇒ **`mapindex_idx2displayname(m->index)`** ; `db/map_index.yml` porte un champ
`Name:` sur 1014/1322 entrées et l'accesseur **retombe seul** sur le nom interne
pour les ~300 cartes techniques. Ce défaut-là préexistait au chantier 204.

🔴🔴 **`SearchRoute` type `0` ≠ type `1`** — piège vécu : notre « Y aller » ne
démarrait RIEN pendant que le « Find » natif marchait. Le type `0` vise une
**cellule** : `CNavigation_PrepareDestination` (`0x00B39030`) charge la `.gat` de
la carte visée et exige que `(x,y)` soit **praticable** (`sub_A784C0`) ; sinon
`BuildRoute` (`0x00B30070`) rend **-97**, `SearchRoute` rend **0**, et un
appelant qui ignore ce retour voit un bouton muet. `(0,0)` n'est praticable sur
AUCUNE carte ⇒ **sans coordonnées, c'est le type `1`** (sa branche ne lit jamais
x/y). Même règle côté serveur : `clif_navigateTo` n'émet `0` que si
`x > 0 && y > 0`. Corollaire de méthode : **lire le VERDICT de la native** —
elle rend 0/1 — au lieu de se contenter de « ça n'a pas planté »
(cf. [[feedback_native_replacement]]).

🔴🔴 **LE TRACÉ EXACT EXISTE, CELLULE PAR CELLULE** (mesuré 2026-08-18,
IDB renommée/commentée, `docs/navigation_re.md` §10). Correction d'une
affirmation antérieure de moi : « le chemin à l'intérieur d'une carte n'existe
nulle part sous forme de liste » était une **supposition**, pas une mesure.
`CNavigation_BuildCellPath` **`0x00B2FC30`** lance l'A★ de déplacement du client
(`Pathfind_AStarSearch` `0x00A777B0`) sur la **.gat de la carte courante** et
remplit **deux** listes de `g_Navigation` :
· **`+0x1164/+0x1168/+0x116C`** = `vector<PathCell>` de **16 o** :
  `{int x; int y; int dir 0..7; int t_ms}`, ordonné **départ → arrivée**
  (`Pathfind_ReconstructPath` `0x00A77660` remonte les parents et écrit à
  l'envers) ; `dir` est la direction qui **MÈNE À** la cellule, **impair =
  diagonale** ; `+0x117C` = bool « trace active », c'est LE garde.
· **`+0x1294/+0x1298/+0x129C`** = les mêmes cellules déjà projetées en pixels
  **minimap 128×128** (`mx = x*s+offX`, `my = 126 - (y*s+offY)`,
  `s = 124/max(w,h)` via `Minimap_FitScaleAndOffset` `0x00B343D0`) ⇒ **le natif
  DESSINE bien l'itinéraire sur sa minimap**. Inutilisable au zoom (quantifié)
  ⇒ Bourgeon repart des CELLULES.
Le rendu au sol `CNavigation_RenderGroundTrail` **`0x00B31C40`** est appelé
**chaque frame par `CScene_RenderCellsAndCursor` `0x00A7B0A0`** — la scène 3D,
**pas une fenêtre** : tuer les quatre natives ne l'éteint pas. Il ne dessine
qu'**une cellule sur deux** et **seulement à ±10 du joueur** — d'où « la trace
aide à SUIVRE, jamais à TROUVER ». S'il ne voit plus aucune cellule, il relance
le calcul depuis la position courante (seul recalcul en marche).

🔴 **Une destination sur la CARTE COURANTE n'a AUCUNE étape** : les étapes sont les LIENS à franchir entre cartes, donc `GetStepCount` rend **0** alors que le chemin en cellules est calculé et `+0x117C` allumé. Conclure à l'échec là-dessus affichait « aucun chemin » par-dessus une trace au sol vivante (bug gonryun). Seul verdict fiable = **l'état du moteur** : `+0x117C` + suivi `+0x125C` + nombre d'étapes. ⚠ **PAS** le nom visé `+0x110C` (un échec ne le remet pas à zéro) ; éteindre `+0x117C` AVANT la tentative pour qu'il ne témoigne que d'elle.

🔴 **ARRÊT du guidage = `CNavigation_ClearRoute` `0x00B2F080`
`__thiscall(nav, bool full)`** (le natif : `+0x125C = 0`, `+0x1254 = 0`, puis
`ClearRoute(1)`, site `0x005AA21F`). ⚠ **`full = 1` détruit AUSSI le vecteur de
résultats `+0x10F0`** ⇒ Bourgeon passe **0**. À ne pas confondre avec
`SelectResult 0x00B35F80`, qui n'efface que la CIBLE : la trace au sol lui
survivait.

🔴 **PIÈGE IDA, coûteux** : `find type=immediate` ne voit **que les vrais
opérandes immédiats**, jamais un déplacement mémoire — `mov [ecx+1344h], eax` est
**invisible**. J'en avais conclu à tort que personne ne lisait `+0x1180`.
Utiliser **`search_text` sur le listing, borné par plage** (rapide et complet ;
sans bornes il expire). Et se rappeler que le compilateur replie les index :
`this + 24*dir + 4480` devient `this + 8*(3*dir + 560)`, où `4480` n'apparaît
plus. Le bon fil était la liste des **xrefs à `g_Navigation`** (170), qui
contient directement le rendu de scène.

## 2026-09-05 — le dièse d'un nom de PNJ, coupé côté RECHERCHE

Signalé en jeu : « linker un PNJ transmet aussi le `Market Group Guide#info`, or
chercher `Market Group Guide#info` dans la navigation échoue à trouver
`Market Group Guide` ».

rAthena nomme ses PNJ **`Visible#interne`** — le dièse distingue deux homonymes
dans les scripts, et le client n'affiche jamais ce qui suit. Mais le lien de chat
relaie le nom BRUT de la plaque de l'acteur (`EntityName`,
`entity_context_menu.cc` → `AppendNaviSearchLink`), et rien ne le tronquait sur
tout le trajet : ni `links::FromNaviSearch` (qui ne refuse que `<` et `>`), ni la
fenêtre de navigation. Or le moteur natif découpe la saisie en mots sur les
**espaces** : le dièse n'y sépare rien, donc le terme ne rencontrait aucune entrée
des `.lub`, qui ne portent que la partie visible.

Coupe donc au dièse, **dièse compris**, dans `StripHiddenName` — et du côté
RECEVEUR, délibérément : ça rattrape aussi bien un lien déjà posté qu'un terme
tapé à la main, sans rien supposer de qui a fabriqué la chaîne. Deux appels, les
deux seuls passages obligés :
- `RunSearch()`, au dernier moment avant `SafeRunSearch` — quelle que soit la
  porte d'entrée (barre de recherche, lien, `/navi`) ;
- `NpcSpriteClassOnMap`, qui échouait pour la même raison : l'aperçu au survol
  d'un lien PNJ compare le nom du lien aux objets du nœud de carte, et ne trouvait
  pas de sprite à montrer.

⚠ Reste NON traité, et c'est un choix : le LIBELLÉ du lien
(`links::NaviSearchTermShown`, `link_gesture.cc`) affiche encore la partie cachée
— `[PNJ: Market Group Guide#info (prontera)]`.

Related : [[reference_minimap_re]], [[reference_ui_richtext_link_system]],
[[reference_native_window_toggle_router]], [[project_mvp_tracker]],
[[feedback_absence_needs_measurement]]
