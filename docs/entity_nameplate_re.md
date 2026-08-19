# Affichage des noms d'entités (pseudo / guilde / party / rang / titre / mobs / NPC)

Rétro-ingénierie du système qui affiche le nom flottant au-dessus des entités
(joueurs, mobs, NPC, unités) : d'où viennent les chaînes, comment elles sont
stockées, comment le label est déclenché, composé, coloré et dessiné.

Client `20250716` (Moonlight-Destiny), base `0x00400000`, Ghidra == live.
Vérifié en live avec x32dbg (GameMode `0x1634e010` capturé sur breakpoint).

> **TL;DR** — Le nom **n'est pas** dessiné par la passe 3D des sprites. Chaque
> acteur ne fait qu'**insérer un rectangle de picking** dans un quadtree écran.
> Le **texte visible** est un widget UI (`UIActorNameLabel`) construit **au
> survol** (ou pour la cible) à partir d'un **dictionnaire de noms indexé par
> GID** (`GameMode+0x160`). Les lignes « pseudo (…) » et « Guilde [Rang] » sont
> composées par `UIActorNameLabel_SetNameFromInfo`.

---

## 1. Vue d'ensemble — trois briques

| Brique | Rôle | Emplacement |
|---|---|---|
| **Dictionnaire de noms** (`CNameInfo`) | stocke par **GID** : nom, party, guilde, rang, titre | `std::map` à `GameMode+0x160` |
| **Quadtree de picking** | rectangles-écran des labels → hit-test souris + anti-chevauchement | `g_NameplatePickQuadTree` `0x012135f0` |
| **Widget `UIActorNameLabel`** | rend le TEXTE multi-ligne visible (nom/guilde/titre + emblème) | créé paresseusement, réf. `GameMode+0x2ac` |

Le flux par frame (dans `GameMode_InGame_ProcessFrame`) :

```
(begin scene)
TileQuadTreeNode_Clear(0x012135f0)                 ; on vide le quadtree
param_1[0x34]->vtable+0xc  = CScene_RenderCellsAndCursor
   └─ rend les acteurs ; chaque acteur, en fin de rendu de sprite
      (CActorSprite_RenderLayered), appelle vtable+0x14 =
      *_SubmitNameplateQuad  → INSÈRE son rect de picking (pas de texte)
...
FUN_00c76400 (passe pick/curseur) :
   pfVar6 = TileQuadTree_QueryPoint(0x012135f0, sourisX, sourisY)   ; entité survolée
   FUN_00c712b0 = GameMode_ShowHoverNameLabel(GameMode, pfVar6[6]=AID)
      └─ construit/rafraîchit le widget UIActorNameLabel  → TEXTE visible
UIWindowMgr_RenderWindows(...)                      ; dessine le widget
```

---

## 2. Le dictionnaire de noms `CNameInfo` (`GameMode+0x160`)

`std::map<uint GID, CNameInfo>` (rouge-noir MSVC). L'objet conteneur a :

| Offset (conteneur) | Champ |
|---|---|
| `+0x00` | vtable `0x00fd9188` |
| `+0x04` | tête rbtree (`_Myhead`) |
| `+0x08` | count (live : 4 entrées) |
| `+0x0c` | table de hachage des **GID en attente de nom** (requêtes serveur) |

**Layout d'une valeur `CNameInfo`** (bloc `node+0x14`, vtable `0x00fd9180`) —
confirmé par `CNameInfo_CopyCtor` (`0x00c634a0`) et
`UIActorNameLabel_SetNameFromInfo` :

| Offset (value) | Type | Contenu |
|---|---|---|
| `+0x00` | vtable | `0x00fd9180` |
| `+0x04` | `std::string` | **NOM** (pseudo / nom de mob / nom de NPC) |
| `+0x1c` | `std::string` | **PARTY** (RO stock ZC_0A30) — rendu en `"nom (…)"` |
| `+0x34` | `std::string` | **GUILDE** |
| `+0x4c` | `std::string` | **POSITION / RANG** de guilde |
| `+0x64` | `std::string` | **TITRE** (résolu depuis titleId via `Title_GetStringById`) |
| `+0x7c` | `std::string` | 6ᵉ chaîne (clan/extra) |
| `+0x94` | `int` | **titleId** |
| `+0x98` | `byte` | valid/résolu (1 = nom connu) |

### Méthodes du dictionnaire (`CNameDict_*`)

| Adresse | Nom | Rôle |
|---|---|---|
| `0x005a0d30` | `CNameDict_GetOrCreateEntry` | lower_bound + insert nœud (0xb0), init via `FUN_005a0180` |
| `0x005a1460` | `CNameDict_GetEntryOrRequest` | renvoie l'entrée si valide (`+0x98==1`) ; sinon **met le GID en file de requête** et renvoie une entrée vide statique `LAB_01251678` |
| `0x005a1640` | `CNameDict_GetName` | copie la chaîne `+0x04` (le NOM) |
| `0x005a18e0` | `CNameDict_Contains` | true si le GID est présent |
| `0x005a1b00` | `CNameDict_SetFullNameInfo` | pose nom+party+guilde+position+titre + titleId (`+0x94`) + valid (`+0x98=1`) |
| `0x005a25b0` | `CNameDict_SetName` | pose le NOM seul (`+0x04`) — utilisé pour les NPC |
| `0x005a2650` | `CNameDict_SetGuildPosition` | pose `+0x4c` |
| `0x005a26f0` | `CNameDict_SetTitleString` | pose `+0x64` |

`GameMode_CopyEntityName` (`0x00c68e50`) = wrapper qui renvoie le nom (utilisé
pour le whisper / menu contextuel).

---

## 3. Assignation depuis les paquets serveur

| Paquet | Handler | Effet |
|---|---|---|
| **ZC 0x0A30** (`ZC_ACK_REQNAMEALL2`) | `Actor_ApplyNameAllWithTitle_ZC0A30` `0x00cf2a50` | (GID, nom, party, guilde, position, **titleID**) → `CNameDict_SetFullNameInfo(GameMode+0x160)`. Si GID==`g_Account_Aid` : mémorise `g_Own_TitleId` `0x016004fc`. |
| **Spawn avec nom** (`ZC_NOTIFY_STANDENTRY` nommé, NPC) | `GameMode_OnRecv_ActorSpawn_Named` `0x00cc99a0` | nom variable à `pkt+0x4b` → `CNameDict_SetName`. Crée/maj l'acteur (OnMsg 0x7a/0x7b). |

Le client demande un nom manquant via `CNameDict_GetEntryOrRequest` (met le GID
dans la file `+0x0c`, envoyée en `CZ_REQNAME`). Les stubs `CNameDict_SetName` /
`SetGuildPosition` / `SetTitleString` correspondent aux ACK partiels du serveur
(nom seul, changement de rang, changement de titre).

---

## 4. Le quadtree de picking `g_NameplatePickQuadTree` (`0x012135f0`)

Arbre spatial écran à **4 enfants** (offsets nœud `0x14`/`0x18`/`0x1c`/`0x20`,
liste terminale `+0x24`/`+0x28`). Rempli chaque frame par les acteurs, vidé au
début du rendu (`TileQuadTreeNode_Clear`), interrogé pour le hit-test souris.

Trois variantes de `SubmitNameplateQuad` (slot **vtable +0x14** selon la classe
d'acteur) — **chacune n'insère qu'un rectangle**, aucun texte :

| Adresse | Classe | id (quad[6]) | catégorie pick (quad[8]) |
|---|---|---|---|
| `0x00c588b0` | `CActorSprite_SubmitNameplateQuad` (joueur/mob/**NPC** — CNpc 0x010939D4 la porte aussi) | `this+0x110` (AID) | 0 / 3 / 4 |
| `0x00d1da70` | `CItem_SubmitNameplateQuad` — 🔴 **OBJET AU SOL** (corrigé 2026-08-19 ; l'ancien nom `NpcActor_…` était faux) | `this+0x17c` (AID du flooritem) ; job = constante **0x7D03** | 1 |
| `0x00db4d60` | `SkillUnitActor_SubmitNameplateQuad` | `this+0x410` | 2 |

**Quad** = 10 floats : `[0..5]` rect écran (clampé à une taille mini
`DAT_015e5b40`), `[6]` = AID, `[7]` = display job, `[8]` = catégorie de pick,
`[9]` = `this+0x2c8`. Inséré par `NameplateQueue_Insert` (= `TileQuadTree_Insert`,
`0x00a79610`).

Côté lecture (`FUN_00c76400`, passe pick/curseur) :
`pfVar6 = TileQuadTree_QueryPoint(0x012135f0, DAT_011e40d4, DAT_011e40d8)` →
la catégorie `[8]` et le job `[7]` pilotent le type de curseur, et
`GameMode_ShowHoverNameLabel(GameMode, pfVar6[6])` affiche le nom.

> `SHIFT` (`GetAsyncKeyState(0x10)` → `DAT_015e6e94`) inhibe l'affichage/pick
> (déplacement forcé). L'option **`OptionInfo 0x2f`** conditionne l'affichage du
> nom des **joueurs** au survol.

---

## 5. Déclenchement du label : `GameMode_ShowHoverNameLabel` (`0x00c712b0`)

`this = GameMode`, `param_1 = AID` (celui renvoyé par le quadtree). Debounce via
`GameMode+0x2d8` (dernier AID survolé).

- acteur trouvé dans la liste → `GameMode_BuildActorNameLabel(this, actor)` ;
- `AID == g_Account_Aid` (soi-même) → idem sur l'acteur propre
  (`ActorMgr+0x2c`) — **c'est le cas du screenshot** (survol de son perso) ;
- entité « nom réseau » hors liste (`ActorMgr_FindByAid`) → lit `actor+0x158`
  (nom brut), couleur `0x95EFFF`.

Le widget cible est `GameMode+0x2ac` (créé paresseusement par
`GameMode_EnsureNameLabelWidget` `0x00c714b0`). Positionné par `FUN_00c72ce0`,
rendu par le `UIWindowMgr`.

### `GameMode_BuildActorNameLabel` (`0x00c6d1d0`)

1. `CNameDict_GetEntryOrRequest(GameMode+0x160, actor.GID)` → `CNameInfo`.
2. joueur masqué en WoE → nom `"??????"`.
3. `SetName` (widget vtable **+0xd8** = `UIActorNameLabel_SetNameFromInfo`).
4. **Emblème de guilde** : `SetEmblem` (widget vtable **+0xd4**) via
   `CGuildEmblemMgr_GetEmblemPath` (option `0xf3` = emblème pendant le siège,
   `0x40` = affichage emblème).
5. **Couleur** : `widget+0x90 = EntityName_ResolveColorByType(job, GID)` +
   overrides (cf. §7).
6. `FUN_00c72ce0(widget, actor)` finalise/positionne.

Variantes : `GameMode_BuildActorNameLabel_Alt` `0x00c6db30`, et
`GameMode_UpdateSelectedTargetNameLabel` `0x00c76890`.

> 🔴 **CORRECTION (2026-08-01, décompilé + vérifié en live).** Cette dernière était
> décrite ici comme « le **bandeau nom+HP+emblème de la CIBLE**, widgets
> `GameMode+0xac`/`+0xab` » — **faux sur tous les points**, et l'erreur a essaimé
> jusque dans les notes du projet.
> Il n'existe **aucun bandeau de cible** et **aucun HP** dans ce client : la
> fonction se contente de réafficher **le même `UIActorNameLabel`** que le survol,
> simplement pour la **dernière entité cliquée** (`CGameMode+0xF4`) au lieu de
> l'entité survolée. Les offsets étaient eux aussi erronés (`0xac`/`0xab`
> confondent un index de dword avec un offset) : le pointeur du widget vérifié en
> live est **`GameMode+0x2AC`** (un second pointeur voisin, `+0x2B0`, est manipulé
> par la même fonction — non départagé, ne pas s'y fier sans mesure).
> Le « HP » que l'on voit à l'écran ne vient pas du client : c'est **le serveur qui
> l'injecte dans le NOM du mob** (cf. `docs/target_system_re.md` §4).

---

## 6. Composition du texte : `UIActorNameLabel_SetNameFromInfo` (`0x0082e1d0`)

Widget : ctor `0x00818250`, base `0x00817170`, **vtable `0x010292f8`**, objet
0xC0 octets. Champs : `+0x90` couleur (défaut `0xFFFFFF`), `+0xa0` `std::string`,
`+0x84/+0x88` **vecteur de lignes** (`std::string` 0x18, push via `FUN_006c8330`),
`+0xb8` largeur, `+0xbc` style emblème.

`SetNameFromInfo(widget, CNameInfo* info, char isPlayer, char emblemStyle)`
reconstruit le vecteur de lignes (mode multi-ligne `DAT_0159b8a2==0`) :

1. si **titre** `info+0x64` non vide → format **`"[%s] %s"`** (préfixe titre
   devant le pseudo) ;
2. **ligne NOM** : si `info+0x1c` (party/ID) non vide → **`"%s (%s)"`** =
   `nom (info+0x1c)` → **« Stingor (7599999995) »** ; sinon nom seul ;
3. **ligne GUILDE** : si guilde `info+0x34` non vide → **`"%s [%s]"`** =
   `guilde [position info+0x4c]` → **« Moonlight-Destiny [Handyman] »**.

Chaque champ passe par le **filtre/substitution de noms** `LAB_0159c2c8`
(`FUN_00a85be0` contains / `FUN_00a84f10` get) — censure/renommage. Puis chaque
ligne est mesurée (`UIText_MeasureWidth`, hauteur `DAT_01212d0c`), le widget est
redimensionné (vtable+4), l'emblème éventuel (`emblemStyle` 1/2 → texture
`LAB_0102abcc`/`LAB_0102abec`) ajoute sa largeur, puis repaint (vtable+0x98).

> `isPlayer` choisit la forme du séparateur ; `g_ServiceType` bascule
> `"%s (%s) "` ↔ `"%s [%s]"` / `"%s (%s)"` selon la région.

---

## 7. Couleur du nom par type : `EntityName_ResolveColorByType` (`0x00c68eb0`)

`ResolveColorByType(displayJob, GID) -> 0xRRGGBB` :

| Condition | Couleur | Sens |
|---|---|---|
| `IsGidInActorFilterList(GID)` | `0x00FFFF` jaune | entité marquée/filtrée |
| `FUN_00a72820()` != 0 | (valeur) | override global |
| `EntityName_IsHostileOrSpecialUnit(GID,job)==1` | `0xF7B895` saumon | unité hostile/spéciale (homon/merc…) |
| job ∈ [1001..3998] ou plage spéciale | `0xC3C3FF` bleu clair | **MONSTRE** |
| sinon | `0xFFFFFF` blanc | **JOUEUR** |

Overrides appliqués dans `GameMode_BuildActorNameLabel` :

- plage d'ID GM (`g_ServiceType==0xc`, ids `0x189111…`) → `0x646464` gris ;
- écart de niveau `g_Own_BaseLevel - actor.level` : `< -10` → `0xFF` (rouge),
  `>= 16` → `0x9B9B9B` (gris) ;
- soi-même (`g_Account_Aid == actor.GID`) → `0xC8C8C7` (blanc cassé) ;
- entité « nom réseau » hors liste → `0x95EFFF`.

`EntityName_IsHostileOrSpecialUnit` (`0x00d9d220`) : vrai si l'état acteur
`+0x314 ∈ {1,6,0xC}`, ou selon `FUN_00d71ec0(job)` (catégorie summon/pet).

---

## 8. NPC, mobs, unités — spécificités

- **Mobs** : nom stocké dans le dictionnaire (via spawn ou ZC_REQNAME).
  Couleur bleu clair `0xC3C3FF`. *Confirmé live* : entrée dict GID `0x068ebc57`
  = « `100Def-Mdef Large Boss` ».
- **NPC** : nom fourni au spawn (`GameMode_OnRecv_ActorSpawn_Named` →
  `CNameDict_SetName`). 🔴 Pick via `CActorSprite_SubmitNameplateQuad`
  (catégorie **0**, corrigé 2026-08-19) — la catégorie 1 est celle des OBJETS AU
  SOL. Certains NPC « cliquables » reçoivent un `Actor_AttachFloatingWidget`.
- **Objets au sol** : classe `CItem` (vtable `0x010932AC`), leur propre liste à
  `actorMgr+0x18` ; nameid complet à `CItem+0x178`, identifié à `+0x174`, AID à
  `+0x17C`. Détail : `entity_context_menu_re.md` §3 (encadré 2026-08-19).
- **Unités de skill / warp** : `SkillUnitActor_SubmitNameplateQuad` (catégorie 2).
- **Joueurs masqués (WoE)** : le builder force le nom `"??????"`.

Le **menu contextuel clic-droit** (`GameMode_ShowEntityContextMenu` `0x00c6e990`)
utilise le même dictionnaire (`CNameDict_GetName`) pour le titre + entrées
whisper/party/guilde/trade (via `MsgStringTable`).

---

## 9. Confirmations live (x32dbg)

- GameMode `0x1634e010` (capturé sur bp `FUN_00c76400`, `ecx`).
- Dictionnaire `GameMode+0x160` = `0x1634e170` : vtable `0xfd9188`, count **4**.
- Entrée racine : GID `0x068ebc57`, `CNameInfo` vtable `0x00fd9180`, nom heap
  `0x22da05d8` = « 100Def-Mdef Large Boss ».
- Acteur propre `0x22947b48`, vtable **`0x01094810`** (`OwnPlayerActor`),
  AID `this+0x110` = `0x001e8481`, textures `+0x104`/`+0x108`.
- Widget nom `GameMode+0x2ac` : NULL hors survol (création paresseuse).
- Renderer monde `GameMode+0xd0` → vtable+0xc = `CScene_RenderCellsAndCursor`
  `0x00a7b0a0`.
- Chemin de pick confirmé : bp sur `CActorSprite_SubmitNameplateQuad` (0x00c588b0)
  → appelé depuis `CActorSprite_RenderLayered` (`0x0060466d`) sur l'acteur propre.

---

## 10. Table récapitulative des adresses

| Adresse | Symbole |
|---|---|
| `0x012135f0` | `g_NameplatePickQuadTree` (quadtree de picking) |
| `0x00a79610` | `NameplateQueue_Insert` (= `TileQuadTree_Insert`) |
| `0x00c588b0` | `CActorSprite_SubmitNameplateQuad` (joueur/mob, vtable+0x14) |
| `0x00d1da70` | `CItem_SubmitNameplateQuad` (ex-`NpcActor_…`, corrigé 2026-08-19) |
| `0x00db4d60` | `SkillUnitActor_SubmitNameplateQuad` |
| `GameMode+0x160` | dictionnaire `std::map<GID, CNameInfo>` (conteneur vtable `0xfd9188`) |
| `0x005a0d30` | `CNameDict_GetOrCreateEntry` |
| `0x005a1460` | `CNameDict_GetEntryOrRequest` |
| `0x005a1640` | `CNameDict_GetName` |
| `0x005a18e0` | `CNameDict_Contains` |
| `0x005a1b00` | `CNameDict_SetFullNameInfo` |
| `0x005a25b0` | `CNameDict_SetName` |
| `0x005a2650` | `CNameDict_SetGuildPosition` |
| `0x005a26f0` | `CNameDict_SetTitleString` |
| `0x00c634a0` | `CNameInfo_CopyCtor` (layout de l'entrée) |
| `0x00cf2a50` | `Actor_ApplyNameAllWithTitle_ZC0A30` (paquet ZC 0x0A30) |
| `0x00cc99a0` | `GameMode_OnRecv_ActorSpawn_Named` (spawn NPC nommé) |
| `0x00c76400` | passe pick/curseur (QueryPoint + ShowHoverNameLabel) |
| `0x00c712b0` | `GameMode_ShowHoverNameLabel` |
| `0x00c6d1d0` | `GameMode_BuildActorNameLabel` |
| `0x00c6db30` | `GameMode_BuildActorNameLabel_Alt` |
| `0x00c76890` | `GameMode_UpdateSelectedTargetNameLabel` — ⚠ **pas** un bandeau, pas de HP : même `UIActorNameLabel`, pour la sélection `CGameMode+0xF4` (cf. §5) |
| `0x00c68eb0` | `EntityName_ResolveColorByType` (palette des couleurs) |
| `0x00d9d220` | `EntityName_IsHostileOrSpecialUnit` |
| `0x00c714b0` | `GameMode_EnsureNameLabelWidget` |
| `0x00818250` | `UIActorNameLabel_ctor` (vtable `0x010292f8`) |
| `0x0082e1d0` | `UIActorNameLabel_SetNameFromInfo` (vtable+0xd8) |
| `0x0082dcc0` | `UIActorNameLabel_SetEmblem` (vtable+0xd4) |
| `0x00c68e50` | `GameMode_CopyEntityName` |
| `0x00c6e990` | `GameMode_ShowEntityContextMenu` (menu clic-droit) |

### Options concernées (`OptionInfo_GetValue`)

`0x2f` (afficher noms joueurs), `0x5c` (style « NameBaloon »), `0x40` (emblème
guilde), `0xf3` (emblème en siège), `0x109` (autres joueurs semi-transparents),
`0xc9` (tooltip d'icônes de statut).
