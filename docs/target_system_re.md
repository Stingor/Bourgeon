# Système de ciblage / sélection d'entité (« target system »)

Rétro-ingénierie du ciblage du client : ce que RO mémorise quand on clique une
entité, combien de temps il le garde, qui l'efface, ce qu'il en affiche — et ce
qu'il faudrait pour en faire une vraie interface de cible (façon *target frame*).

Client `20250716` (Moonlight-Destiny), base `0x00400000`, Ghidra == live.
Vérifié en live avec x32dbg (`CGameMode` `0x2262AF18` puis `0x228F7A20`).

> **TL;DR** — RO **a** une sélection persistante : `CGameMode+0xF4` garde l'AID de
> la dernière entité cliquée. Elle n'est effacée **qu'au changement de map**, par
> aucun autre chemin — donc elle survit à la mort de la cible et devient un
> **fantôme**. Son unique rendu est le **nom flottant** : il n'existe ni bandeau,
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
| `CGameMode+0xF8` | `this[62]` | drapeau remis à 0 à chaque clic sur acteur | — |
| `CGameMode+0x5B0` | `this[364]` | action **forcée** (Ctrl / option `0x6D`) : empêche l'oubli de `+0xF0` | — |

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

L'unique rendu de la sélection est **`GameMode_UpdateSelectedTargetNameLabel`**
`0x00c76890` (ex-`UpdateTargetInfoBar`, mislabel corrigé — cf.
`entity_nameplate_re.md`). Elle **réutilise le même `UIActorNameLabel` que le
survol** et l'affiche pour l'entité de `+0xF4`. Il n'y a **ni bandeau, ni
portrait, ni barre de vie**.

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
