# Système de ciblage / sélection d'entité (« target system »)

Rétro-ingénierie du ciblage du client : ce que RO mémorise quand on clique une
entité, combien de temps il le garde, qui l'efface, ce qu'il en affiche — et ce
qu'il faudrait pour en faire une vraie interface de cible (façon *target frame*).

Client `20250716` (Moonlight-Destiny), base `0x00400000`, Ghidra == live.
Vérifié en live avec x32dbg (`CGameMode` `0x2262AF18` puis `0x228F7A20`).

> **TL;DR** — RO **a** une sélection persistante : `CGameMode+0xF4` garde l'AID de
> la dernière entité cliquée. Elle n'est effacée **qu'au changement de map**, par
> aucun autre chemin — donc elle survit à la mort de la cible et devient un
> **fantôme**. Elle a **deux** rendus, et deux seulement : le **nom flottant** et
> le **petit marqueur 3D** au-dessus de l'entité (§4 bis) ; il n'existe ni bandeau,
> ni portrait, ni barre de vie. Le « HP: 63% » visible à l'écran n'est pas une
> donnée client — c'est **le serveur qui l'écrit dans le nom du monstre**.
> Toutes les briques d'un vrai système de cible existent ; il n'en manque que la
> présentation, et un transport propre pour les points de vie.

---

## 1. Les deux champs de ciblage

Deux champs voisins, de durées de vie **très** différentes. Les confondre est
l'erreur qui coûte cher.

| Offset | Décompilé | Rôle | Durée de vie |
|---|---|---|---|
| `CGameMode+0xF0` | `this[60]` | **cible de travail** : celle sur laquelle le clic maintenu agit | très courte, cf. §2 |
| `CGameMode+0xF4` | `this[61]` | **sélection** : AID de la dernière entité cliquée | jusqu'au changement de map, cf. §3 |
| `CGameMode+0xF8` | `this[62]` | 🔴 **geste de souris libre** : le maintien en cours n'a pas été capturé par un acteur (§2 ter) | du clic au relâchement |
| `CGameMode+0x5B0` | `this[364]` | action **forcée** (Ctrl / option `0x6D`) : empêche l'oubli de `+0xF0` | — |
| `CGameMode+0x28` | `this[10]` | 🔴 **engagement souris** : verrou du nom flottant ET de la flèche (§4 bis) | très courte, cf. §2 bis |

État de ciblage d'une compétence armée, posé par `SendMsg(0x48)` (§5) :

| Offset | Contenu |
|---|---|
| `+0x408` | **mode** : `0` aucun · `1` sol · `2` cible offensive · `3` — · `4` soutien · `5` piège |
| `+0x40C` | identifiant de la compétence |
| `+0x410` | `CSkillInfo+0x1C` |
| `+0x414` | niveau |

---

## 2. `+0xF0` — la cible de travail, éphémère

Écrite au clic sur un acteur, puis relue chaque frame par
`GameMode_RouteHoverAndClick` `0x00c756a0`, qui délègue à
**`GameMode_RepeatActorAction` `0x00c77120`**. C'est cette fonction qui **répète**
l'action : à chaque nouvel appui gauche *frais*, elle re-poste
`GameMode_PostActorClickAction` sur la cible déjà mémorisée, sans avoir à la
ré-identifier.

Elle est remise à `0` dans trois cas :

1. **relâchement** du bouton (`g_Mouse_LButtonState == 3`) et `+0x5B0 == 0` ;
2. la **souris bouge** alors que le bouton est relâché, et `+0x5B0 == 0` ;
3. l'acteur **meurt** (`acteur+0x70 == 3`).

> ⚠ Ne **jamais** bâtir une sélection d'interface sur `+0xF0` : un simple
> déplacement de souris la vide.

### Corollaire — pourquoi les sorts de zone ne se répètent pas

Ce mécanisme n'existe **que** pour les cibles. Aucune cellule n'est mémorisée
pour un cast au sol : `GameMode_GroundClick_RequestMove` `0x00c75aa0` exige un
appui *frais* à chaque cast et ne laisse aucun état derrière lui. D'où
l'asymétrie observée en jeu : maintenir la touche d'un sort **ciblé** le relance,
maintenir celle d'un sort de **zone** ne le lance qu'une fois. C'est natif.

---

## 2 bis. `+0x28` — le verrou d'engagement, et ce qu'il coûte

🔴 **Le champ qu'on découvre en dernier, et qui explique tout.**

Il est **levé** par le **message 26** du mode (`0x00C884B9` : `+0x28 = p1`, et si
`p1 == 0`, `+0xEC` et `+0xF0` sont vidés dans la foulée). Ce message part
exactement d'un endroit : le clic sur un acteur, juste avant l'écriture de
`+0xF4`/`+0xF0` — `CursorMgr_UpdateHover` (`0x00C78E5D` et `0x00C79031`) et
`GameMode_RepeatActorAction` (`0x00C77120`).

Il **retombe** dans `GameMode_RouteHoverAndClick` `0x00C757C4`, dès que les trois
conditions sont réunies : aucun acteur sous la souris, `+0xF0` vide, `+0xEC`
vide. C'est-à-dire **au premier mouvement de souris hors de la cible**, bouton
relâché.

Ce qu'il gate :

| Site | Effet |
|---|---|
| `0x00C76C9A` | **Verrou d'entrée** de `GameMode_UpdateSelectedTargetNameLabel` : nul ⇒ retour immédiat sur `SendMsg(5)`, donc **ni nom flottant ni flèche** |
| `0x00C7571E` | `CursorMgr_SetType(this, 5)` **à chaque frame** tant qu'il est levé — le curseur « attaque », partout sur l'écran |
| `0x00C7863D` | bloque l'écriture de `+0xF0` au survol |

> 🔴 **Corollaire — la flèche n'a PAS la durée de vie de la sélection.**
> Elle a celle de l'*engagement souris*. Écrire `+0xF4` (ce que fait le cyclage
> clavier) ne l'allume donc pas : le verrou est nul, la fonction sort avant même
> de lire la sélection.
>
> 🔴 **Et forcer `+0x28` est un remède pire que le mal** : le curseur « attaque »
> se colle sur tout l'écran, réécrit chaque frame. La bonne prise est ailleurs —
> §4 bis, « ce que Bourgeon en fait ».

---

## 2 ter. `+0xF8` — le geste de souris libre

🔴 **Le champ qui n'a rien à voir avec ce que son nom laissait croire**, et dont
l'écriture par imitation a coûté un bug en jeu (2026-08-22).

Il ne dit pas « on vient de cliquer ». Il dit : **le maintien de bouton en cours
n'a pas été capturé par un acteur**. Relevé exhaustif des accès sur
`0x00C60000`-`0x00CA0000` :

| Site | Geste |
|---|---|
| `0x00C76538` `GameMode_ProcessMouseWorldInput` | **= 1** au RELÂCHEMENT du bouton gauche (`g_Mouse_LButtonState == 3`) |
| `0x00C6BE2A` `GameMode_OnEnterMapSetup` | **= 1** à l'entrée de carte |
| `0x00C75641`, `0x00C77622`, `0x00C78E75`, `0x00C79049`, `0x00C791D6`, `0x00C79D42`, `0x00C79DDB` | **= 0** — les sept sites de clic ou de survol **sur acteur** |
| `0x00C76236` `GameMode_GroundClick_RequestMove` | **lu** (cf. ci-dessous) |
| `0x00C785E9` `CursorMgr_UpdateHover` | **lu** : à zéro, la branche d'action est sautée |

### Ce que sa lecture décide : la marche au bouton MAINTENU

Dans `GameMode_GroundClick_RequestMove`, les deux chemins ne se ressemblent pas :

```c
if ( ...+1280 || g_Mouse_LButtonState != 2 ) {   // appui FRAIS
    if ( g_Mouse_LButtonState != 1 ) return param_1;
    v43 = g_Mouse_RButtonState == 2;             // +0xF8 jamais consulté
} else {                                          // bouton MAINTENU
    if ( !v42 ) return param_1;                   // cadence : 600 ms (this+0xD8)
    v43 = *((_DWORD *)this + 62) == 0;            // +0xF8
}
if ( !v43 ) { /* ... la demande de marche ... */ }
```

La marche maintenue n'est donc ré-émise **que si `+0xF8 != 0`** — c'est ce qui
empêche un clic maintenu de continuer à courir vers le sol pendant qu'on tape un
monstre. L'appui **frais** ne le regarde pas : un geste cassé se répare toujours
en relâchant puis recliquant, ce qui masque complètement la cause.

> ⚠ **Leçon** : imiter un chemin natif « à l'identique » n'est sûr que si l'on
> sait ce que chaque écriture VEUT DIRE. Ici les deux écritures voisines
> (`+0xF4` puis `+0xF8`) répondent à deux questions différentes — quelle cible,
> et dans quel état est le geste de souris — et le clavier n'a d'affaire qu'avec
> la première.

---

## 3. `+0xF4` — la sélection, et son unique nettoyage

**Recherche exhaustive** des écritures (`[reg+0F4h],` sur `0x00C60000`-`0x00CA0000`) :

| Adresse | Fonction | Écrit |
|---|---|---|
| **`0x00C6BE20`** | `GameMode_OnEnterMapSetup` | **`0` — le seul nettoyage** |
| `0x00C7761C` | `GameMode_RepeatActorAction` | l'AID (propage `+0xF0`) |
| `0x00C78E67` | `CursorMgr_UpdateHover` | l'AID (clic acteur) |
| `0x00C7903B` | `CursorMgr_UpdateHover` | l'AID (clic, autre branche) |
| `0x00C79D3C` | `sub_C79610` | l'AID (chemin follow / attaque) |

*(`0x00C71617` — `movss [eax+0F4h], xmm0` — appartient à un autre objet : flottant, hors sujet.)*

Que `edi` soit bien le `CGameMode` dans `GameMode_OnEnterMapSetup` est prouvé par
la même fonction, qui remet `+0x408` à zéro quinze instructions plus loin
(`0x00C6BEA6`).

> 🔴 **La sélection n'est effacée qu'au changement de map.** Ni la mort de la
> cible, ni un clic au sol, ni le temps ne la touchent. Après la mort du mob,
> `+0xF4` pointe donc vers un AID qui n'existe plus : un **fantôme**.
> Tout consommateur **doit** revalider par `ActorList_FindByGID` à chaque frame et
> vider son affichage sur `null`.

Mesuré en live : `+0xF4` = `0x002E2837` (≠ notre AID `0x001E8481`) conservé alors
que `+0xF0` valait `0`, sur deux `CGameMode` successifs.

---

## 4. Ce que le client en affiche — et le vrai statut des HP

Le premier rendu de la sélection est **`GameMode_UpdateSelectedTargetNameLabel`**
`0x00c76890` (ex-`UpdateTargetInfoBar`, mislabel corrigé — cf.
`entity_nameplate_re.md`). Elle **réutilise le même `UIActorNameLabel` que le
survol** et l'affiche pour l'entité de `+0xF4`. Il n'y a **ni bandeau, ni
portrait, ni barre de vie**. Le **second** rendu — la petite flèche 3D — est au
§4 bis : c'est la **même** fonction qui l'allume.

Structure du widget (pointeur en `GameMode+0x2AC`, vérifié en live) :

| Offset | Contenu |
|---|---|
| `+0x84` / `+0x88` | **vecteur de lignes** : `std::string` de 24 o (buffer sur le tas dès `cap >= 16`) |
| `+0x90` | couleur (`EntityName_ResolveColorByType`) |
| `+0xa0` | ⚠ `std::string` **vide** — ce n'est **pas** le nom, piège vérifié |

Contenu réel capturé en jeu, ligne 1 (42 caractères) :

```
"10Def 10Mdef Small Norm (Lv. 10 | HP: 63%)"
```

> 🔴 **Les points de vie ne sont pas une donnée du client.** C'est **moonlight qui
> injecte niveau et pourcentage de HP dans le NOM du monstre**, transporté comme
> n'importe quel nom (`CNameDict`). Conséquences :
> - une barre de vie de cible est faisable **aujourd'hui**, sans une ligne de
>   serveur, en parsant la chaîne ;
> - mais c'est **fragile** : dépend du format exact, de la langue, et ne marche
>   que pour les mobs dont le nom porte ces stats ;
> - pour du solide, un opcode custom `{GID, hp, maxhp}` est préférable — le projet
>   en a déjà toute l'infrastructure (`opcode_map.md`).
>
> Les membres de la party font exception : leurs HP arrivent déjà **structurés**
> via `ZC_NOTIFY_HP_TO_GROUPM`.

Masquage : si `acteur+0x2F8 == 1` en PVP/GVG, le nom devient `"??????"`.
Filtres d'affichage : `acteur+0xA0`/`+0xA1` doivent valoir `1`, et
`Option_IsCloak` doit être faux.

---

## 4 bis. Le marqueur 3D de la sélection — la « petite flèche »

🔴 **La sélection a bien un second rendu, dans la scène 3D** : le petit triangle
blanc qui flotte au-dessus de l'entité. Ce n'est ni un effet, ni une fenêtre, ni
un enfant de l'acteur — c'est un **nœud de scène unique et permanent**, le
`CMousePointer`.

| Quoi | Où |
|---|---|
| Objet | **`CGameMode+0xD4`** — un `CMousePointer`, vtable **`0x0108F928`** |
| Construction | `CMousePointer_ctor` **`0x00c3fb80`**, appelé par `CGameMode_EnterWorld` (`0x00c73a41`) et `GameMode_ResetAndRecreateOwnActor` (`0x00c7e64e`), puis `SceneNodeList_PushBack` |
| Ressource | `cursors.spr` / `cursors.act` (`UITextureMgr_Load`), **action 3**, direction 2 |
| OnMsg (vt+8) | **`CMousePointer_OnMsg_SelectionMarker` `0x00c55b20`** |
| Rendu (vt+0xC) | `ChildSprite_DrawIfActive` `0x00c487e0`, gardé par `+0xA0` |
| Animation (vt+4) | `CActorSprite_AdvanceAnimFrame` `0x00c464b0` |

Ses deux messages :

| Msg | Arguments | Effet |
|---|---|---|
| **1** | `{x, z}` en cellules, drapeau couleur | `+0x10` = x, `+0x18` = z, `+0x14` = `Terrain_GetHeightAt(x, z)`, **`+0xA0 = 1`** (visible), `+0x138 = 0`, couleur `+0x98` = `0xFFFF0000` (rouge) si le drapeau est vrai, `0xFFFFFFFF` (blanc) sinon |
| **2** | durée en frames | `+0x138 = durée` ; **`0` ⇒ `+0xA0 = 0`** (masqué) |

`+0x138` est décrémenté à chaque frame par `CActorSprite_AdvanceAnimFrame`, qui
masque le nœud quand il atteint 0 : c'est une durée de vie, pas un booléen.

### Qui l'allume — la même fonction que le nom

Un seul émetteur : **`GameMode_UpdateSelectedTargetNameLabel` `0x00c76890`**,
celle du §4. À chaque frame, **et dans cet ordre** :

1. 🔴 `+0x28` (§2 bis) nul → `CMode::SendMsg(5)` **immédiatement**, sans même
   regarder la sélection ;
2. sinon, sélection `+0xF4` résolue et affichable → `CMode::SendMsg(4, (int)acteur.x, (int)acteur.z, 0)` ;
3. sinon → `CMode::SendMsg(5)`.

Les arguments du message 4 sont la position **monde** de l'acteur (`+0x10` et
`+0x18`), tronquée en entiers, et `p3` est un drapeau de couleur : vrai = rouge,
faux = blanc. Le natif passe toujours faux.

Dispatch du mode : case **4** (`0x00c8a2e6`) → `GameMode_ShowSelectionMarkerAt`
`0x00c82a90` → msg 1 ; case **5** (`0x00c8a316`) → `GameMode_HideSelectionMarker`
`0x00c6b6b0` → msg 2 avec durée 0. Recherche exhaustive des émetteurs de
`SendMsg(4/5)` sur `0x00a00000`-`0x00d50000` : **aucun autre**.

> 🔴 **Le marqueur suit la DERNIÈRE ENTITÉ CLIQUÉE, pas la cible d'attaque.**
> C'est le même `+0xF4` que le nom flottant : il apparaît donc aussi sur un
> joueur ou un NPC simplement cliqué. « Il apparaît quand on attaque » n'est
> qu'un effet de bord : attaquer, c'est cliquer.
>
> ⚠ **Correction du 2026-08-20 — il ne PERSISTE pas.** Une version antérieure de
> cette page l'affirmait, par omission du verrou `+0x28` (§2 bis) : c'est lui, et
> non `+0xF4`, qui décide de l'affichage. La flèche s'éteint donc au premier
> mouvement de souris hors de la cible, alors que `+0xF4`, lui, tient jusqu'au
> changement de map. Deux durées de vie, pas une.

### Ce que Bourgeon en fait

Le HUD de cible (§4 ter) veut une flèche qui dure **aussi longtemps que sa
cible** — y compris posée au clavier, ou sur un cadavre qu'on ressuscite. Trois
voies, une seule tenable :

| Voie | Verdict |
|---|---|
| Forcer `+0x28` | ⛔ curseur « attaque » sur tout l'écran (§2 bis) |
| Poster `SendMsg(4)` depuis notre passe de rendu | ⛔ trop tard dans la frame : la scène 3D est déjà dessinée, et le natif re-masque à la frame suivante avant le rendu |
| **Réécrire le message 5 en message 4** | ✅ |

C'est la troisième qui est livrée. Le hook de `CMode::SendMsg` existe déjà
(`ProcessInput: 0x00c86740`, cf. `configuration.h`) ; le message 5 n'a **qu'un**
émetteur, à un instant de la frame qui est exactement le bon. Quand le HUD suit
une cible, la demande de masquage part en demande de pose sur la position de
cette cible — et rien d'autre n'est touché : aucun champ natif écrit, aucun hook
de plus, et le natif garde la main dès que le HUD n'a plus de cible.

Les filtres du natif sont reproduits avant de poser (`TargetFrame::ReadMarkerPos`) :

| Filtre | Détail |
|---|---|
| Portail | classe `45` : jamais de flèche |
| **Cloak** | 🔴 virtuelle `vtable+0x34` de l'acteur, bit `0x04` (`Option_IsCloak` `0x00D71140` n'est que ce test). **À ne pas sauter** : poser la flèche sur une entité cloakée donnerait la position d'un joueur caché, un avantage que le client vanilla ne donne pas |
| Visibilité | `+0xA0` et `+0xA1` doivent valoir 1... |
| ...sauf | unité spéciale (`+0x314` non nul), que le natif dispense de ce test |

Réglage : **« Flèche de ciblage du jeu sur la cible »** (défaut allumé), dans le
panneau du HUD de cible.

La position posée est celle de la **cellule** (hauteur du terrain sous l'entité) ;
le décalage qui fait flotter le triangle au-dessus du sprite vient des calques de
l'action 3 de `cursors.act`, pas du code.

Mesure live (2026-08-19, x32dbg attaché, jeu en cours, sans pause) :
`CGameMode = 0x4C605CE0` · `+0xD4 = 0x4E0841A8` · vtable `0x0108F928` ·
action `+0x34 = 3` · `+0xA0 = 1` · position `(27.0 ; -45.06 ; -192.0)` ·
sélection `+0xF4 = 0x002DC7AE`.

### Les deux marqueurs voisins — mesurés et écartés

Deux autres mécanismes posent un sprite au-dessus d'une entité. Ils ne sont
**pas** la flèche, mais les confondre coûte cher :

| Mécanisme | Comment | Pourquoi ce n'est pas lui |
|---|---|---|
| **Homoncule / mercenaire** — child sprite `cursors.spr` **action 10** dans `acteur+0x3A8`, posé par le **msg 134 (0x86)** de l'acteur (`0x00c4c0b7`), retiré par le **135 (0x87)** (`0x00c4c138`) ; décision à chaque frame dans `ActorAiClass_UpdatePerFrame` (`0x00d3194d`) | exige `g_Homun_Present` + AID `0x015FF99C`, ou merc `0x015FFA50` + AID `0x015FFA38` | les deux AID valaient **0** pendant l'observation |
| **Marqueur de quête** — child sprite `이팩트\emotion.spr` (actions 80-85) posé par `Actor_UpdateQuestMarkerChildSprite` (`0x00d30a20`) d'après la map `CGameMode+0x1F0`, remplie par **`Recv_ZC_QUEST_NOTIFY_EFFECT`** (`0x00cd23c0` : `{GID, x, y, effect, color}`, `effect == 9999` = retrait ; répercuté sur la minimap par le msg 485) | type de child = `effect + 36` si `effect < 6`, sinon `+ 54` | map **vide** (taille 0) pendant l'observation |

La surbrillance de la cellule sous la souris (`g_cursor_x/y/type/hidden`
`0x01229440`…, posée par `GameMode_GroundClick_RequestMove`) est encore autre
chose : un quad de terrain, pas un sprite.

## 4 ter. Le HUD de cible livré (Bourgeon `TargetFrame`)

Implémentation : `src/features/overlays/target_frame.{h,cc}`. Opt-in
« target_frame », membre du groupe « Interface moderne » (défaut OFF).
Libellé : **« Activer le mode Ciblage + HUD »**, et c'est un **interrupteur
maître** : éteint, le HUD, la flèche, les raccourcis de ciblage et le clic sans
attaque s'arrêtent tous, et le panneau grise ses réglages (`BeginDisabled`).

**Ce n'est pas une fenêtre** : cinq **cadres libres** — portrait, nom+niveau,
barre PV, barre SP, race/élément — chacun avec sa position, sa taille, ses
couleurs et son interrupteur, dans l'esprit des barres de Basic Info et du
portrait du joueur. La mécanique du cadre est factorisée dans
**`src/ui/hud_frame.{h,cc}`**, et elle reprend celle de Basic Info **geste pour
geste** :

* poignée de redimensionnement visible au coin bas-droit (le jeu dessine son
  propre curseur, donc celui du système ne se voit jamais) ;
* bords qui **s'allument** au survol et pendant le tirage, un coin en allumant
  deux ;
* **aimantation entre cadres voisins** (même bord, bord opposé, juste après,
  juste avant — seuil 10 px) ;
* **grille d'alignement** partagée, appliquée au coin déplacé et à chaque bord
  tiré ;
* **CTRL + déplacement** : tout le bloc de cadres qui se touchent suit d'un seul
  tenant ;
* clamp écran en dernier, parce que l'aimantation peut elle-même pousser dehors ;
* texte **centré**, mesuré à la taille de dessin (`CalcTextSizeA`) et non via
  `SetWindowFontScale` — mesurer à une taille et dessiner à une autre décale le
  centrage.

> ⚠ **Dette identifiée** : `BasicInfo::DrawBar` et `DrawPortraitElem` portent
> encore leur propre copie de cette mécanique — elle leur est antérieure. Elles
> doivent migrer sur `ui/hud_frame.h` ; c'est pour ça qu'il existe.

### Un GESTE désigne la cible — surtout pas un sondage

🔴 **Le HUD suivait `+0xF4` frame après frame. C'était faux, et ça s'est vu.**
Une cible qui sortait de l'écran disparaissait bien — son acteur avait vraiment
quitté le monde du client — puis **revenait toute seule** dès qu'elle rentrait
dans AREA_SIZE. Parce que la sélection native, elle, n'est purgée qu'au
changement de map (§3) : le sondage la retrouvait intacte et rallumait un HUD que
personne n'avait redemandé.

Le HUD n'écoute donc plus un **état**, il écoute des **gestes**. Trois, et le
dernier gagne :

| Geste | Signal |
|---|---|
| **clic** sur une entité | `+0xF4` change — ou, si on re-clique la MÊME entité, le **front montant** de l'engagement `+0x28` (§2 bis), seul signal disponible dans ce cas |
| **cyclage** au clavier | `TargetFrame::CycleTarget` |
| **sort qui part** | `TargetFrame::NoteSkillTarget` — y compris sur un cadavre, que `+0xF4` refuse d'enregistrer |

Corollaire, et c'est tout l'intérêt : **une cible perdue est perdue.** Rien ne la
rallume tant qu'on ne l'a pas re-désignée. Il n'y a plus non plus de liste noire
à tenir (l'ancien `dropped_gid_` a disparu) : un refus du serveur éteint, point.

| Extinction | Mécanisme |
|---|---|
| la cible change | un geste désigne quelqu'un d'autre ⇒ tout l'état repart de zéro |
| la cible sort d'AREA_SIZE | `Actor_FindByGid` rend `nullptr` (le serveur a envoyé `ZC_NOTIFY_VANISH`) — et **elle ne revient pas** en rentrant dans la zone |
| la cible **se cache** | `Hiding` (`0x02`), `Cloaking` (`0x04`) ou `@hide` du staff (`0x40`), lus par la virtuelle `vtable+0x34` de l'acteur. 🔴 Le GID est **perdu** : garder celui d'une entité qui vient de se rendre invisible, c'est offrir un détecteur |
| le serveur répond statut 1 | « hors de portée » : c'est lui qui connaît AREA_SIZE |
| le joueur le masque | « Masquer la fenêtre de cible » dans le menu contextuel de l'entité |
| changement de map | `GameMode_OnEnterMapSetup` purge `+0xF4` ; `OnModeSwitch` purge le nôtre |

### Le nom peut arriver APRÈS la cible — et il ne faut pas le réclamer plus fort

On cible plus vite que le serveur ne renvoie le nom : le cadre affiche
« (nom inconnu) » un court instant. C'est normal, et ça se répare tout seul —
à une condition.

**`CNameDict_GetEntryOrRequest` `0x005A1460` n'est pas un accesseur.** Sur un
défaut, il empile le GID dans la file des noms à demander et rend une entrée
**vide et statique**. C'est le tick du dictionnaire, **`0x005A1920`** (appelé à
chaque frame depuis `GameMode_InGame_ProcessFrame`, `0x00C74B06`), qui dépile
**UNE** demande par frame, l'envoie — **CZ_REQNAME `0x0368`, 6 octets** — et note
le GID pendant **10 s** pour ne pas le redemander.

> 🔴 **Le HUD l'appelait QUATRE fois par frame** (nom, party, guilde, rang) : il
> empilait donc quatre demandes par frame pour le même GID, jusqu'à ~240 par
> seconde dans une file vidée à raison d'une par frame. Plus on réclamait le nom,
> plus il tardait. Une seule interrogation par frame, quatre lectures dedans.

**Et ça ne suffisait pas.** La fenêtre anti-répétition de 10 s se retourne contre
nous dès qu'on cible vite : si l'entité a été **recréée** entre-temps (Cloaking,
`@hide`, sortie puis retour dans AREA_SIZE), son entrée de dictionnaire repart
vide alors que l'interdiction, elle, court toujours. Le nom reste alors inconnu
**jusqu'à dix secondes** — et le nameplate du jeu au-dessus du sprite est vide lui
aussi : le client n'a rien, et ne demande rien.

> ✅ Le HUD **redemande lui-même** : `TargetFrame::RequestTargetName` envoie
> `CZ_REQNAME 0x0368` (6 o, `[op:2][gid:4]`, le paquet du client à l'octet près)
> toutes les 700 ms tant que le nom manque. La réponse est traitée par le client
> comme n'importe quelle autre : c'est SON dictionnaire qui se remplit, donc le
> nameplate natif se répare avec le nôtre.

### Ce qu'on ne montre pas d'un JOUEUR

Le HUD sert à jauger un adversaire, pas à l'auditer. Deux retraits volontaires :

| Retiré | Pourquoi |
|---|---|
| **les chiffres de PV** — la jauge se remplit, sans valeurs **ni pourcentage** | connaître au point de vie près ce qui reste à un joueur est un avantage que le jeu ne donne pas |
| **la party et le rang de guilde** — seul le nom de guilde reste | c'est une fenêtre d'adversaire, pas un annuaire |
| **le cadre race / élément / taille**, en entier | il vaut « Neutral 1 · Medium » pour **tout** joueur : un cadre qui ne distingue personne vaut mieux fermé |
| **la barre de SP**, en entier | le serveur ne la transmet qu'à la party et à la guilde : elle serait « inconnus » devant presque tout le monde |

> 🔴 **Conséquence côté serveur : le gate PvP ne porte plus sur les PV.**
> Il les réservait à la party et à la guilde. Les chiffres étant désormais
> masqués pour tout joueur, ce sont **les PV qui partent toujours** ; le **SP** et
> le **niveau**, qui s'affichent en clair, restent gatés.
>
> Sans ce changement la jauge d'un adversaire restait vide **jusqu'au premier
> coup porté** — symptôme observé après un Cloaking : le client ne sait RIEN des
> PV d'un tiers, il ne les déduit que des dégâts qu'il voit passer, et un
> `Cloaking` ou un `@hide` recrée l'acteur en remettant ce compteur à zéro.

⚠ « inconnus » reste affiché quand les PV ne sont **pas** connus (entité sans
`status_data`). Ce n'est pas une fuite, c'est l'inverse — et sans ce mot, une
jauge vide se lirait « il est mort ».

Sans cible, les cadres disparaissent — sauf en **mode placement** (réglage) ou
tant qu'un cadre est saisi : on ne retire pas sous les doigts du joueur ce qu'il
est en train de poser.

### Ce que la sélection native ne voit pas : les cadavres

`+0xF4` n'est écrit qu'à **un seul endroit** du clic (`0x00C79D3C`), et seulement
sur une cible jugée « valide ». Un **cadavre n'en est pas une** : on lui lance une
résurrection sans que rien ne bouge côté client, et le HUD restait donc muet.

Deuxième source, donc : le **paquet qui part**. `CZ_USE_SKILL`
(`char_diag::kCzUseSkill` = `0x0438`, 10 o : `<lv>.W <id>.W <targetGID>.L`) porte
le GID visé, quoi qu'en pense la sélection interne. `RagConnection::SendPacketHook`
l'observe — observation pure — et le passe à `TargetFrame::NoteSkillTarget`.

Opcode vérifié **à l'octet** au site de construction : `0x00C8DE53`,
`mov eax, 438h`, puis `[buf+0] = ax`, `+2` niveau, `+4` id, `+6` GID.

Arbitrage, **le dernier geste gagne** : la cible imposée par un sort tient tant que
`+0xF4` ne bouge pas ; dès que le joueur clique ailleurs, le clic reprend la main.
Un sort sur **soi-même** est ignoré (sinon le moindre soin personnel effacerait ce
qu'on regardait), et une cible imposée qui quitte le monde est relâchée.

> ⚠ Les sorts **au sol** ne passent pas par là, et c'est voulu :
> `CZ_USE_SKILL_TOGROUND` (0x0AF4, 11 o) porte des **coordonnées**, pas un GID. La
> cible affichée reste celle d'avant — un sort de zone ne désigne personne.

### Ciblage au clavier (cyclage)

Le client n'en a **aucun** : rien dans `UIWindowMgr_DispatchHotkeyBehavior` n'y
ressemble, il n'y a donc rien à intercepter — tout est à nous.
`TargetFrame::CycleTarget(forward)` balaie la liste d'acteurs
(`actorMgr+0x10`), retient les **monstres vivants et visibles**, trie par
**distance croissante au joueur**, et passe au suivant / précédent.

| Point | Choix |
|---|---|
| Qui est candidat | filtre sur le **JOB** (`Job_IsMonsterId`), pas sur une plage de GID : les identifiants sont attribués par le serveur et changent de bornes d'une installation à l'autre, la classe dit ce que l'entité **est** |
| Vivant / visible | `acteur+0x70 != 3` (mort) et `acteur+0xA5` (participe au nameplate) |
| À l'écran | `acteur+0xAC/+0xB0` (position écran projetée) dans le viewport. 🔴 **Seule règle, non réglable** : un mode « rayon en cellules » a existé une journée et a été retiré le 2026-08-20 — une cible qu'on ne voit pas ne se jauge pas et ne se décide pas |
| Ordre | tri par distance, pour que deux passages donnent la **même** suite — l'ordre de la liste du client est arbitraire |
| Reprise | si la cible courante n'est plus dans la liste, le cycle repart du plus proche |

`CycleTarget` écrit la sélection **native** (`CGameMode+0xF4`, et **rien d'autre**) :
la cible clavier est donc la même que celle d'un clic pour tout ce qui lit ce champ.

> 🔴🔴 **Correction du 2026-08-22 — `+0xF8` ne doit PAS être écrit.** Cette page
> disait ici qu'on le remettait à zéro « comme le fait le chemin natif du clic ».
> C'était vrai du code et faux du raisonnement : ce champ ne décrit pas le clic,
> il décrit le **geste de souris en cours** (§2 ter). Le mettre à zéro faisait
> croire au client que le maintien du bouton avait été capturé par un acteur —
> et **la marche au clic maintenu s'arrêtait net dès qu'on prenait une cible au
> clavier**, jusqu'à un relâchement suivi d'un nouvel appui. Vu en jeu, et
> introuvable par la lecture : rien, dans le cyclage, ne parle de déplacement.

> ⚠ **Correction du 2026-08-20.** Cette page affirmait ici qu'écrire `+0xF4`
> suffisait à faire suivre la flèche du jeu. **C'est faux**, et ça s'est vu en
> jeu : le rendu du marqueur est gardé par `+0x28` (§2 bis), pas par `+0xF4`, et
> le cyclage clavier ne lève pas ce verrou. La flèche est rendue au ciblage
> clavier par la réécriture du message 5 décrite en §4 bis (« ce que Bourgeon en
> fait »), pas par l'écriture de `+0xF4`.

Deux actions dans le catalogue de raccourcis (`hotkey_actions`) :
`target_cycle_next` et `target_cycle_prev`, **sans touche par défaut** (le joueur
choisit ; rien n'est imposé).

**Tab et Maj+Tab sont assignables** depuis le 2026-08-20. Ils ne l'étaient pas :
l'écran des raccourcis n'offrait aux actions Bourgeon que lettres, chiffres,
F1-F12 et Espace (`CaptureMainVk`), par crainte d'une touche « que le jeu ne
route pas ». Vérification faite dans le binaire, la crainte ne s'appliquait pas à
Tab :

* `Game_MainWndProc` (`0x00db8100`) ne teste **jamais** VK 9 — aucune comparaison
  à 9 dans toute la fonction — donc Tab descend jusqu'à `UIWindowMgr_OnKeyDown`
  (`0x00a471e0`), c'est-à-dire jusqu'au hook de Bourgeon ;
* là, le case 9 tombe dans `UIWindowMgr_DispatchHotkeyBehavior`, le dispatch des
  raccourcis du **client**, où rien n'est lié à Tab ;
* et quand la barre de chat a le focus, `HotkeyDispatch::OnKeyDown` s'efface de
  lui-même (`NativeTextInputHasFocus`) : Tab y retrouve son rôle de saisie.

D'où `hotkeys::CaptureActionVk()` = `CaptureMainVk()` + Tab. Les autres touches
de `kExtendedKeys` restent hors du jeu des actions : les flèches appartiennent au
déplacement, Entrée et Espace au « bouton par défaut » des fenêtres
(`0x00a47317`), Échap au menu et à l'annulation.

### Cibler sans frapper

Réglage **« Le clic cible sans attaquer »** (défaut éteint) : le clic prend la
cible, mais n'engage rien — ni approche, ni coup.

Chaîne complète, du clic au coup :

```
clic sur un monstre  (CursorMgr_UpdateHover, 0x00C78E5D)
   ├── +0xF4 = +0xF0 = AID          <- LE CIBLAGE, écrit AVANT tout le reste
   └── GameMode_PostActorClickAction (0x00C753A0)        ⛔ coupe n°1
          └── acteur du joueur, message 10  (Actor_OnMsg case 10, 0x00D47CF3)
                 └── +0x500 = 1 ou 5 (action EN ATTENTE), +0x514 = GID cible
                        └── Actor_ProcessPendingAction_Tick : distance, cadence
                            de relance (450 ms si +0x500 == 5, sinon 1200 ms)
                               ├── hors de portée -> DEMANDE DE MARCHE
                               │      CMode::SendMsg(0x8a) -> 0x035F (5 o)
                               └── à portée       -> DEMANDE DE COUP
                                      CMode::SendMsg(0x89, action, GID)  ⛔ n°2
                                         └── CZ_REQUEST_ACT 0x0437  0x00C8F807
```

**Deux coupes, parce qu'il y a deux sorties.**

| Coupe | Où | Ce qu'elle empêche |
|---|---|---|
| n°1 | `GameMode_PostActorClickAction`, hookée via `configuration.h` | **l'armement** : sans action en attente, ni approche ni coup |
| n°2 | message `0x89`, actions `0` et `7`, dans le hook `CMode::SendMsg` | le coup, quel que soit ce qui l'a armé (réglage allumé en plein combat) |

> 🔴 **Le DOUBLE-CLIC attaque.** Sans lui, le réglage n'offrait plus qu'un seul
> chemin vers l'attaque de base (le menu contextuel), ce qui est cher payé.
> Deux engagements sur la MÊME entité dans la fenêtre de double-clic **de
> Windows** (`GetDoubleClickTime`, bornée 200-900 ms) = un ordre, pas un ciblage.
> C'est fiable parce que le natif n'appelle `PostActorClickAction` qu'à chaque
> appui **frais** (`g_Mouse_LButtonState == 1`) : un appui, un passage.
> La dispense obtenue est **durable** pour ce GID, comme celle du menu — une
> attaque est une suite de demandes, en laisser passer une ne ferait qu'un coup.
> Le simple clic suivant la referme.

Garde-fous de la coupe n°1, deux et deux seulement :

| Garde | Pourquoi |
|---|---|
| `CGameMode+0x408 == 0` | une **compétence armée** emprunte le même chemin ; la bloquer empêcherait tout lancement au clic |
| `acteur+0x2EC != g_Account_Aid` | `+0x2EC` est l'AID du **maître** : c'est le test du natif lui-même (`0x00C787CC`). Mes pet, homoncule et mercenaire gardent leurs ordres au clic |

> ⚠ **« Attaquer » du menu contextuel emprunte le MÊME chemin natif que le clic**
> (`RunNativeActorClick` appelle `GameMode_PostActorClickAction`) : il tombait
> donc sous la coupe n°1 et ne faisait plus rien. C'est un ordre **explicite**,
> il doit frapper. D'où `TargetFrame::NoteExplicitAttack`, posée juste avant :
> elle ouvre **un seul** passage dans le chemin du clic, et une dispense durable
> pour le COUP sur ce GID — sans quoi seule la première frappe partirait. Le
> prochain clic du joueur sur une entité la referme.

Actions du message `0x89`, quatre sous le même numéro :

| Action | Sens | Sort |
|---|---|---|
| `0` | frapper une fois (`DMG_NORMAL`) | **jetée** |
| `7` | frapper en continu (`DMG_REPEAT`) | **jetée** |
| `2` / `3` | s'asseoir / se relever (`0x00D476B2` / `0x00D476D8`) | intactes |

> 🔴 **C'est le CLIENT qui demande l'approche, et le serveur qui l'exécute.**
> La question « si ça venait du client, une déconnexion devrait me ramener en
> arrière » tranche le débat : il n'y a pas de rollback, et les autres joueurs
> voient bien le déplacement, donc le serveur est au courant. Il l'est par une
> **vraie demande de marche** (`0x035F`), émise par le tick de l'acteur tant que
> la cible est hors de portée. Le client ne bouge pas tout seul : il demande.
>
> C'est aussi pourquoi couper le seul coup ne suffisait pas — mesuré en jeu :
> « ça n'attaque plus, mais ça marche toujours ».

#### 🔴🔴 Trois erreurs payées ici — à ne pas refaire

**1. Filtrer trop bas.** Le paquet d'attaque est la *dernière* étape d'une
machine à états. Couper là laisse tout ce qui la précède — dont l'approche.

**2. `p1` et `p2` de `SendMsg(0x89)` sont dans cet ordre : action, puis GID.**
Les avoir échangés a rendu le filtre parfaitement muet — un GID ne vaut jamais 0
ni 7, donc rien n'était jamais jeté, sans le moindre message d'erreur. L'ordre se
lit dans les `push` des quatre émetteurs (`Actor_OnMsg` `0x00D4765B`,
`0x00D47678`, `0x00D476B2`, `0x00D476D8`), où la constante d'action est poussée
**juste avant** le message. Le dispatcher, lui, charge `p1` dans `edx` et `p2`
dans `ecx` — ce qui, lu à l'envers depuis `0x00C8F807`, donne la conclusion
inverse. **Toujours trancher chez l'ÉMETTEUR.**

**3. `acteur+0x314 != 0` ne veut PAS dire « unité spéciale ».**
`CActorSprite_InitDefaults` (`0x00C45F47`) y écrit **4 par défaut**. Un filtre
« ordinaire = 0 » rejette donc *tout le monde*, en silence.

Les points 2 et 3 ont la même forme : **une condition qui ne filtre rien
ressemble exactement à une fonctionnalité qui n'est pas branchée.**

#### ⚠ Piège d'opcode : le tampon du client n'est PAS le fil

`SendPacketHook` voit le tampon **avant** le brouillage natif du premier mot.
Les constantes de `char_diag` sont donc celles que le client **écrit**, et elles
ne coïncident pas avec le `clif_packetdb.hpp` de moonlight, qui attend `0x088e`
et `0x089b` (bloc `PACKETVER >= 20130320`). **L'écart est normal.**

Exemple qui a coûté cher : `CZ:WalkToXY`. Le serveur le lit en `0x0881`, mais le
client l'**écrit** en `0x035F` (`mov eax, 35Fh`, `0x00C8F865`). Un commentaire de
`keyboard_move.h` annonçait `0x0881` comme la valeur construite : c'était celle du
packet_db. On ne peut rien déduire du serveur pour ce qu'on observe dans
`SendPacketHook`.

> 🔴 **La seule preuve qui vaille est l'octet au site de construction**, dans le
> dispatch de `CGameMode` : `0x00C8DE53` `mov eax, 438h` pour le skill,
> `0x00C8F807` `mov eax, 437h` pour l'attaque. Ni le packet_db du serveur, ni la
> table de longueurs héritée du client (`PacketLenTable_Insert` `0x00AA6C10`, qui
> n'a même pas d'entrée pour `0x0881`) ne tranchent quoi que ce soit.

### Le portrait, et l'apparence d'une entité tierce

Deux moteurs, parce qu'il y a deux natures : un monstre EST un sprite
(`ro::LoadMobSprite` par classe, `is_model = true` pour les 85 classes rendues en
3D), un joueur est un ASSEMBLAGE que `ro::DrawDoll` monte à partir d'un
`DollLook`. Reste à remplir ce `DollLook` depuis un acteur **tiers** — d'où cette
carte, relevée dans `CActorSprite_SetSexAndRebuildLook` `0x00d36280` (qui passe
tous ces champs à `vt+76` dans l'ordre) et recoupée avec les constructeurs de
couches :

| Offset | Champ | Offset | Champ |
|---|---|---|---|
| `+0x048` | clothes color | `+0x43C` | hair color |
| `+0x25C` | classe brute (base job) | `+0x440` | weapon view id |
| `+0x260` | **sexe** (0 = femme) | `+0x444` | weapon 2 / main gauche |
| `+0x438` | hair style | `+0x448` | headgear **top** |
| `+0x4C8` | **body style** (LOOK_BODY2) | `+0x44C` | headgear **mid** |
| `+0x4CC` | classe d'**affichage** (nomme le chemin du corps) | `+0x450` | headgear **low** |
| | | `+0x454` | robe / garment (slot 8) |

> 🔴 **Mislabel corrigé** : `vt+176` s'appelait `CActorSprite_SetSexAndRebuildEquip`
> et n'a rien à voir avec le sexe — elle écrit `+0x4C8` et n'est appelée qu'au
> case `0x0D` (LOOK_BODY2) de `ZC_SPRITE_CHANGE`. Le sexe est en `+0x260` : c'est
> lui qui choisit `g_HairSpriteNum_Male` vs `_Female` dans
> `Job_BuildBodyOrHeadSpritePath_impl` `0x00b433b0`. Renommée
> `CActorSprite_SetBodyStyleAndRebuild`.

Le chemin de corps part de `fx::palette_inject::ActorBodySpritePath` plutôt que d'être
redéduit : sur une 3e ou 4e classe, rejouer `Job_ResolveBodyClass` diverge et
affiche une tenue de base.

### Les données, et leurs deux sources

| Champ | Source |
|---|---|
| nom · party · guilde · rang | `CNameInfo` (`+0x04`, `+0x1c`, `+0x34`, `+0x4c`) — 🔴 pour un **monstre**, `clif_name` détourne ces champs : party = `« Lv. X | HP: Y% »`, guilde = **race**, rang = **élément** |
| PV (repli) | jauges de l'acteur : `+0x300` (`UIMonsterGage`, alimentée par `ZC_HP_INFO` 0x0977) et `+0x488` (`UIPcGage`, msg 34) — PV en `+0xA0`, max en `+0xA4` |
| PV exacts, **SP**, niveau, race/élément/taille/boss | **CZ 0x0F29 → ZC 0x0F2A** (serveur moonlight) |

> 🔴 **Le SP d'une entité tierce n'existe dans AUCUN paquet du jeu.** La jauge
> d'entité sait pourtant l'afficher : `UIMonsterGage` naît en mode **deux
> barres** (`+0x9C = 1`) et `UIPcGage_Paint` `0x008549a0` peint la barre du bas
> depuis `+0xA8`/`+0xAC`. Mais les **trois** appelants de
> `UIPcGage_SetGaugeBottom` `0x008645b0` (`Actor_OnMsg_Base` `0x00d3c651`,
> `sub_D33020` `0x00d33689`, `sub_D3BAA0` `0x00d3bbec`) passent tous `(0, 0)`, et
> un balayage exhaustif du `.text` ne trouve aucune écriture directe de ces deux
> champs hors du constructeur. Côté serveur, le SP ne part que pour soi, son
> homoncule, son mercenaire et son elemental. **Le rendu était prêt, le transport
> manquait** — c'est tout ce qu'ajoute ZC 0x0F2A.

> ⚠ **Un monstre n'a pas de réserve de SP, et le serveur ne peut pas le dire.**
> `status_calc_misc` se termine par `if (!status->max_sp) status->max_sp = 1;` —
> un maximum nul ferait des divisions par zéro partout. Le paquet porte donc
> `1 / 1`, ce qui affiché tel quel donne une barre **pleine** et un chiffre qui ne
> veut rien dire. La barre de SP écrit donc **« No SP »** et ne se remplit pas
> quand le maximum vaut 1 ou moins. Aucun joueur n'y tombe : même un novice de
> niveau 1 a une dizaine de SP.

### Le protocole

`CZ_BOURGEON_TARGET_INFO` (0x0F29, fixe 8) : `[op:2][len:2][gid:4]`.

`ZC_BOURGEON_TARGET_INFO` (0x0F2A, fixe 34) :
`[op:2][len:2][gid:4][status:1][known:1][type:1][level:2][hp:4][maxhp:4][sp:4][maxsp:4][race:1][ele:1][ele_lv:1][size:1][boss:1]`

* `status` : 0 = ok, 1 = introuvable ou hors d'AREA_SIZE (la fenêtre se ferme) ;
* `known` : masque de ce qui est **renseigné** — 1 PV, 2 SP, 4 niveau, 8
  race/élément/taille. Un bit à 0 ⇒ champ à ignorer : « 0 PV » et « PV inconnus »
  ne se ressemblent pas à l'écran ;
* `type` : 1 PC · 2 MOB · 3 NPC · 4 HOM · 5 MER · 6 PET · 7 ELEM.

🔴 **Pas d'abonnement, pas de timer serveur** : c'est le client qui redemande
toutes les 400 ms tant que sa fenêtre est ouverte. Le serveur ne garde donc
aucun état — rien à nettoyer à la déconnexion, au changement de map ou à la
fermeture de la fenêtre.

🔴 **Gate PVP (serveur)** : sur un autre joueur — ou sur son compagnon, en
suivant le **maître** — PV et SP ne partent qu'aux membres de sa party ou de sa
guilde. Un adversaire renvoie son type et rien d'autre.

Handler serveur : `clif_parse_bourgeon_target_info` (`src/map/clif.cpp`),
structures dans `packets_struct.hpp`, enregistrement dans `clif_packetdb.hpp`.

---

## 5. Agir sur la cible

### Armement (100 % client, aucun paquet)

`UIShortCutWnd::OnMsg(0x29)` → `CMode::SendMsg(0x71)` avec la `CSkillInfo`. Le
case `0x71` (`0x00c8d6bd`) envoie d'abord **`0x47`** (purge du ciblage), puis
route selon l'**INF** (`CSkillInfo+0x0C`), une donnée **locale** reçue au login :

| INF | Sens | Action |
|---|---|---|
| `1` | cible | `0x48` mode **2** |
| `2` | sol | `0x48` mode **1** |
| `4` | soi | `0x45` immédiat, puis `0x48` mode `0` |
| `16` | soutien | `0x48` mode **4** |
| `32` | piège | `0x48` mode **5** |

Le case `0x48` (`0x00c8d8df`) n'écrit que les quatre champs du §1 et change le
curseur. **Le serveur n'autorise rien à ce stade** : le paquet ne part qu'au clic,
et c'est seulement là que SP, portée, cooldown et légalité de la cible sont
validés côté serveur.

Garde-fous locaux avant armement : replay actif, `acteur+0x2CC` non nul (état
bloquant → message `MsgString 0x75E`), fenêtre `0xC4` ouverte, `0x0131F7EC` non
nul, plages de cooldown partagé homoncule / mercenaire.

### Lancer sur une cible imposée

`CMode::SendMsg(0x45)` — `arg2` = identifiant de compétence, `arg3` = **GID
cible**, `arg4` = niveau. C'est la voie propre pour caster sur une sélection sans
repasser par la souris.

> ⚠ `0x45` **ne valide pas le type de cible** : lui passer son propre GID envoie
> n'importe quelle compétence sur soi-même, offensives comprises (bug déjà vécu).
> Le filtrage est à la charge de l'appelant.

---

## 5 bis. « Cliquer une entité » tient dans UNE fonction

`GameMode_PostActorClickAction` (`0x00C753A0`) ne fait pas que l'armement
d'attaque du §4 : c'est **elle** qui porte tout le sens du clic gauche sur une
entité, et elle choisit d'après le ciblage armé (`CGameMode+0x408`) :

| `+0x408` | Ce qu'elle poste à l'acteur du joueur |
|---|---|
| `0` (rien d'armé) | message **10** — approche puis attaque (la file `+0x500`/`+0x514` du §4) |
| `2` ou `4` (cible) | messages **41** puis **90** — la compétence `+0x40C` part sur ce GID, au niveau `+0x414` |
| `1`, `3`, `5` | **rien** |

> 🔴 **`param_2` décide UNE FOIS vs EN CONTINU — et les valeurs sont
> INVERSÉES : `0` = continu, `1` = une seule fois.** Il finit dans `+0x500`, que
> `Actor_ProcessPendingAction_Tick` relance alors toutes les **450 ms** au lieu
> de 1200. Règle du natif, relevée au site de clic sur un monstre dans
> `CursorMgr_UpdateHover` :
>
> ```
> continu = !GameSession_IsAgitZone() && (Ctrl tenu || GameSettings_GetFlag(0x6D))
> PostActorClickAction(this, gid, continu ? 0 : 1) ;  puis  this+0x5B0 = continu
> ```
>
> `0x6D` est l'identifiant TALKTYPE de l'option **`/nc`** (no-control). En zone
> de siège (`*(CGameMode+0xCC) + 0x4C`, posé par `ZC_NOTIFY_MAPPROPERTY = 3`) le
> natif REFUSE le continu, Ctrl ou `/nc` ou non.
>
> ⏱ **Piège payé** : le clic rejoué depuis le HUD de cible figeait ce paramètre
> à `1`. Le cadre ignorait donc `/nc` et Ctrl là où le clic sur le sprite les
> respecte — la « copie du sprite » s'arrêtait au GID.

En tête, une seule garde, et elle n'a rien à voir avec le ciblage : au-delà de
**90 % de poids** (`g_Weight` / `g_MaxWeight`), elle affiche `MsgString 0x133`
et **abandonne** — sauf pour les identifiants de compétence `10000` et `40000`.

> 🔴 **Trois conséquences, toutes exploitées.**
>
> 1. La sélection (`+0xF4`/`+0xF0`) est écrite par l'**appelant**, avant cet
>    appel : d'où « cibler sans engager » (§ *Cibler sans frapper*).
> 2. Le mode `1` (au sol) n'y passe pas : un sort de zone est résolu ailleurs,
>    à partir de la **cellule sous le curseur**. Cliquer un monstre avec un sort
>    de zone armé ne le vise pas *lui*, ça vise sa case — et par le chemin du sol.
> 3. **Il n'y a donc rien à imiter.** Rappeler cette fonction avec un GID à nous,
>    c'est produire un clic sur cette entité, à l'identique — y compris nos
>    propres règles, puisque le hook de Bourgeon est posé ici.

### Les cadres du HUD comme COPIE de l'entité

`TargetFrame` (réglage « Les cadres agissent comme la cible ») s'appuie
entièrement là-dessus : un clic gauche sur n'importe lequel des cinq cadres
rappelle `PostActorClickAction` avec le GID suivi. Attaque, approche, compétence
ciblée armée, dispense du double-clic : tout tombe du client, aucune règle n'est
réécrite.

Deux points de mise en œuvre qui ne sont **pas** des détails :

* **Le cadre doit reprendre la souris au jeu.** Un cadre de HUD verrouillé est
  `NoInputs`, donc clic-traversant (`ui/hud_frame.h`) : le clic irait au sol
  derrière. `HudFrameOpts::clickable` lève ce drapeau — le cadre reste figé mais
  possède l'appui, et le WndProc cesse alors de transmettre le clic au jeu.
  La reprise est **totale** (clic droit et molette compris) : c'est pourquoi un
  cadre n'est cliquable que quand une cible est affichée **et** que le clic
  voudrait dire quelque chose. En mode `1` il redevient traversant, exprès —
  un sort de zone vise une case, et le cadre n'en est pas une.
* **L'appel est différé hors frame ImGui** (`Bourgeon::OnGameFrame`) : la garde
  de surcharge ci-dessus ouvre une boîte de message native, qui relance le tick
  du mode. Jamais entre `NewFrame()` et `Render()`.

Après un lancement en mode `2`/`4`, on émet `CMode::SendMsg(0x47)` — le pipeline
souris natif le fait aussi, sans quoi le curseur de visée resterait armé et le
clic suivant relancerait la compétence.

### La cible du HUD comme SOURCE DE VISÉE (sans la souris)

Réglage « Les sorts ciblés partent sur la cible ». Le lancement reste celui de
QuickCast (messages d'acteur du clic natif) ; **seule la réponse à « sur qui ? »
change** : `TargetFrame::SkillTargetGid` la fournit quand le quadtree de picking
ne donne rien.

| Règle | Pourquoi |
|---|---|
| la SOURIS d'abord, le HUD ensuite | viser du curseur est un geste explicite ; le comblement ne doit jamais voler une visée |
| modes `2` et `4` seulement | un sort de zone vise une cellule, pas une entité |
| offensif : jamais soi-même, jamais un joueur, jamais un cadavre | ces règles vivaient dans `CursorMgr_UpdateHover`, qui n'est **pas** emprunté ici (même dette que `QuickCast::PickTargetGid`) |

> 🔴 Le test « le curseur désigne-t-il le monde ? » a dû **quitter**
> `QuickCast::CanCastNow` pour `EmitCast`. Il ne vaut que pour les visées à la
> souris : refuser de lancer sur la cible du HUD parce que le curseur traîne sur
> une fenêtre aurait vidé le réglage de son sens. Le mode `1`, lui, en dépend
> toujours — la cellule n'a pas d'autre source.

### ⏱ Le cercle orphelin — une touche ne doit rien laisser derrière elle

Le natif arme **toujours** au case `0x48`. QuickCast, lui, peut ne pas lancer :
cadence pas écoulée, compétence en cooldown, ou rien d'exploitable sous le
curseur. Le cercle de visée restait alors armé après le relâchement de la touche
— et ce n'est pas un défaut d'affichage : **le clic suivant, qui visait autre
chose, lançait la compétence oubliée**.

Deux réponses, et il faut les deux :

| | |
|---|---|
| la **répétition** est armée même quand le premier lancement échoue | une cadence ou un cooldown qui se lève finit par lancer sans relâcher. Avant, marteler deux touches de suite en **perdait une**, silencieusement : le refus par cadence sortait de `OnEnterTargeting` avant même d'avoir lu l'état de ciblage |
| au **relâchement**, ce qui reste armé est retiré (`QuickCast::UpdateDisarm`) | il faut un état de sortie propre, y compris quand rien n'était visable |

> 🔴 **Uniquement pour une visée armée par une TOUCHE**, et dans un mode que
> QuickCast prend en charge (`ClaimsMode`). Lancer une compétence en **cliquant**
> sa case de barre de raccourcis ne pose aucune touche (`TakePendingKey` rend 0) :
> le déroulé natif « ça arme, je clique ma cible » reste entier. C'est la voie
> qui subsiste pour viser à la main ce que QuickCast refuse de viser tout seul —
> **un JOUEUR en mode offensif**, notamment.

> ⚠ `UpdateDisarm` émet `CMode::SendMsg(0x47)`, c'est-à-dire le dispatcher que
> notre propre hook intercepte : la jouer entre `NewFrame()` et `Render()` ferait
> tourner `OnProcessInput` au milieu d'une frame ImGui. D'où sa place dans
> `Bourgeon::OnGameFrame`, et **pas** dans `QuickCast::Update()`.

---

## 6. Briques disponibles (toutes en lecture seule)

| Rôle | Adresse | Signature / note |
|---|---|---|
| Entité sous le curseur | `0x00a797b0` sur `0x012135f0` | `float* __thiscall(tree, x, y)` ; quad : `[6]` AID, `[7]` job, `[8]` catégorie (`0` acteur, `1` NPC, `2` unité de skill) |
| Acteur par GID | `0x00a69eb0` | `ActorList_FindByGID(scene, gid)` — **le validateur anti-fantôme** |
| Nom / party / guilde / rang / titre | `0x005a1460` | `CNameDict_GetEntryOrRequest(GameMode+0x160, aid)` ; demande au serveur les noms inconnus |
| Couleur par type | `0x00c68eb0` | `EntityName_ResolveColorByType(job, gid)` |
| Classe affichée | `0x00c43230` | `CActorSprite_ResolveDisplayJob(actor, -1)` |
| Emblème de guilde | `0x0061d370` | `CGuildEmblemMgr_GetEmblemPath` |
| Invisibilité | `0x00d71140` | `Option_IsCloak` |
| Fenêtre sous un point | `0x00a336d0` | `UIWindowMgr_GetWindowAtPoint(g_UIWindowMgr, x, y)` — hit-test **pur** |
| Notre AID | `0x015fb9a4` | `g_Account_Aid` |

Champs d'acteur utiles : `+0x70` état (`3` = mort) · `+0xA0`/`+0xA1` visibilité ·
`+0xAC`/`+0xB0` position écran · `+0x110` AID · `+0x25C` classe de base ·
`+0x2F8` masquage PVP · `+0x314` état d'affichage.

---

## 7. Conception proposée (non implémentée)

1. **Sélection propre au plugin**, amorcée sur `+0xF4` mais **jamais** utilisée
   telle quelle : à chaque frame, `ActorList_FindByGID` ; si `null`, on vide.
   Cela règle le fantôme et le rend invisible à l'utilisateur.
2. **Panneau ImGui** : nom, classe, guilde + emblème, couleur par type — tout est
   déjà lisible. Barre de vie **en dernier**, derrière l'arbitrage du §4.
3. **Cast sur la sélection** via `SendMsg(0x45)`, avec filtrage du type de cible
   côté plugin (offensif → pas soi-même).
4. **Ciblage clavier** (type Tab) : à écrire entièrement, aucun *behavior* natif
   ne l'implémente (absent de la table de `UIWindowMgr_DispatchHotkeyBehavior`).
   Source des candidats : la liste d'acteurs, filtrée par distance écran.
5. Réservé au **staff** dans un premier temps, comme `QuickCast` et
   `EntityNames` (`IsStaff()`, cf. `source_layout.md`).

---

## 8. Ce qui reste à rétro-concevoir

Aucun de ces points ne change l'architecture ci-dessus.

- `dword_15E6E88` — canal de sélection **alternatif** lu par
  `GameMode_UpdateSelectedTargetNameLabel` (branche `ActorMgr_FindByVFuncId`).
  Rôle non identifié ; peut-être un meilleur levier que `+0xF4`.
- Existe-t-il une **table de statuts par GID** côté client ? Nécessaire pour
  afficher les buffs/debuffs de la cible.
- Format exact et fiabilité du HP injecté dans le nom, si la voie « parsing » est
  retenue plutôt qu'un opcode.

---

## 9. Méthode — pièges payés

- 🔴 **Un watchpoint sur une donnée du tas meurt à toute recréation de l'objet.**
  Le `CGameMode` est détruit et réalloué à chaque changement de map **et à chaque
  reconnexion** (observé : `0x2262AF18` → `0x228F7A20`) ; l'allocateur recycle
  l'ancien bloc, et le watchpoint se met à capturer un objet étranger. Un
  déclenchement a ainsi été attribué à tort à un buff self-cast.
  ⇒ Après chaque transition : relire `[0x0121333c]`, et si la base a bougé,
  `bphwc <ancienne>` puis réarmer. **Préférer un breakpoint sur le CODE** (adresse
  fixe dans l'exe, cf. §3) — c'est ce qui a finalement tranché la question.
- **Valider la base** avant d'y croire : `[[base] + 0x18] == 0x00c86740`
  (`CMode::SendMsg`) confirme qu'on tient bien un `CGameMode`.
- **Ne pas se fier aux noms de fonctions hérités** : `UpdateTargetInfoBar`
  promettait un bandeau avec HP ; il n'y a ni bandeau, ni HP. Trois affirmations
  de la documentation existante sont tombées en décompilant.
