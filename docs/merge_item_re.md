# `CMergeItemWnd` (227) — la fusion d'objets, relevé complet

> Relevé du **2026-09-02**. Client `2025-07-16_Ragexe` (IDA, base 0x400000),
> serveur `moonlight` (fork rAthena, `PACKETVER 20250716`).
>
> Solde la « candidate n°1 » désignée par
> [unexplored_systems.md](unexplored_systems.md) §5.
>
> **Verdict : le système est complet et correct des deux côtés — client,
> serveur, protocole et traductions. Le seul défaut est ailleurs : son unique
> ouvreur est posé sur une carte où aucun warp ne mène.**

## 1. À quoi sert cette fenêtre

Un objet empilable marqué `UniqueId` reçoit un identifiant unique à la
génération. Deux lots du même objet portant des identifiants différents **ne
s'empilent pas** : l'inventaire se remplit de piles jumelles. `CMergeItemWnd`
est l'outil qui les recolle en une seule.

Ce n'est donc pas un gadget : sur Moonlight, **1315 entrées** de
`db/import/item_group_db.yml` distribuent des objets avec `UniqueId: true`
(`Fire_Bottle`, `Acid_Bottle`, `Siege_White_Potion`…). Le problème que la
fenêtre résout existe bel et bien.

⚠ La clé YAML est **`UniqueId`**, sous `Flags:` — pas `Guid`, malgré le
`item->flag.guid` du code C++ (`itemdb.cpp:683`). Chercher `Guid:` dans `db/`
rend zéro et fait conclure à tort que le système est vide.

## 2. Les quatre opcodes — correspondance champ par champ

| opcode | dir | handler client | fonction rAthena | corps |
|---|---|---|---|---|
| `0x096D` | ZC | `ZC_MergeItemOpen_Handler` 0x00d036a0 (case **2413**) | `clif_merge_item_open` | `+2` longueur (W), puis `{ index.W }*` |
| `0x096E` | CZ | `CMergeItemWnd_SendMergeRequest` 0x007a62e0 | `clif_parse_merge_item_req` | `+2` longueur (W), puis `{ index.W }*` |
| `0x096F` | ZC | `ZC_MergeItemAck_Handler` 0x00cfc8c0 (case **2415**) | `clif_merge_item_ack` | `+2` index (W), `+4` total (W), `+6` motif (B) — 7 octets |
| `0x0974` | CZ | émis par la fenêtre (`2420`) | `clif_parse_merge_item_cancel` | 2 octets, **sans effet côté serveur** (`return;`) |

Aucune divergence de format. Les tailles annoncées par
[opcode_map.md](opcode_map.md) (VAR/VAR/7/2) correspondent.

## 3. Le parcours complet

### 3.1 Ouverture — `ZC_MergeItemOpen_Handler` 0x00d036a0

```
count = (packetLen - 4) / 2
```

- **`count != 0`** : recopie les index dans un `vector<uint8>` **local**
  (`ByteVector_InsertRange` 0x007a47d0), appelle
  `UIWindowMgr_MakeWindow(mgr, 0xE3)` — `0xE3` = **227** — puis l'`OnMsg` de la
  fenêtre (vtable **+0x94**) avec `msg = 34`, le **pointeur sur le tampon** et
  `count`. Le tampon est libéré dans la foulée : le `case 34` doit tout recopier,
  et c'est ce qu'il fait.
- **`count == 0`** : renvoie immédiatement `CZ 0x0974` sans rien ouvrir. Branche
  purement défensive — `clif_merge_item_open` n'émet le paquet qu'à partir de
  **deux** entrées, et affiche sinon `MSI_NOT_EXIST_MERGE_ITEM`.

### 3.2 Remplissage — `CMergeItemWnd_OnMsg` case 34

Un `UIToggleButton` de **120 × 12** par entrée ; l'index d'inventaire est rangé
dans le bouton en **+0xEC**, son état coché en **+0x30**. Les boutons vont dans
le `vector` de la fenêtre `[+0xEC, +0xF0)`.

### 3.3 Sélection — `CMergeItemWnd_OnToggleCtrlSelectSame` 0x007a4dc0

`GetAsyncKeyState(17)` = **Ctrl**. Ctrl+clic coche (ou décoche) d'un coup **tous
les boutons portant le même `nameid`** — le client compare les identifiants
d'objet obtenus par `Session_GetEquipInfoByInvIndex`. Sans Ctrl, le clic est
unitaire.

### 3.4 Envoi — `CMergeItemWnd_SendMergeRequest` 0x007a62e0

Parcourt le `vector`, empile le `+0xEC` de chaque bouton **coché**, puis :

```
si (nb_cochés < this[+0xDC])   ->  message 0x87A, RIEN n'est envoyé
sinon                          ->  <0x096E>.W <len>.W { index.W }*
```

🔴 **`this+0xDC` vaut 2 et c'est un MINIMUM, pas un drapeau.** Hex-Rays rend le
test `if ( a1[55] )` — ce qui laisse croire que la branche d'erreur est prise
systématiquement, puisque le constructeur y écrit 2. Le désassemblage dit
autre chose :

```
0x7A6380  sub  edi, esi          ; edi = fin - début (octets)
0x7A6382  sar  edi, 1            ; -> nombre d'index cochés
0x7A6384  cmp  edi, [ecx+0DCh]   ; contre le minimum (2)
0x7A638A  jnb  short loc_7A63B8  ; >= 2 : on envoie
```

⚠ **L'ordre compte** : le serveur traite `indices[0]` comme le slot **qui
survit** et absorbe tous les autres dedans (`idx_main`, `clif.cpp:27018`).

### 3.5 Accusé — `ZC_MergeItemAck_Handler` 0x00cfc8c0

| `reason` | enum rAthena | message client | suite |
|---|---|---|---|
| 0 | `MERGE_ITEM_SUCCESS` | `0x87B` = 2171 | `FindWindow(227)` → `OnMsg` **msg 60**, puis fermeture |
| 1 | `MERGE_ITEM_FAILED_NOT_MERGE` | `0x87C` = 2172 | fermeture |
| 2 | `MERGE_ITEM_FAILED_MAX_COUNT` | `0x87D` = 2173 | fermeture |
| ≥ 3 | — | *aucun* | fermeture silencieuse |

🔴 **Le `msg 60` est indispensable, et c'est le seul.** Le serveur envoie un
`clif_delitem` pour chaque slot **absorbé**, mais **rien** pour le slot
survivant : sa nouvelle quantité n'arrive que par ce `0x096F`. Le `case 60`
retire l'entrée puis la ré-ajoute avec le total
(`Inventory_DecreaseOrRemoveByInvIndex` + `Inventory_AddOrStackItem`).

## 4. La classe `CMergeItemWnd`

Constructeur `0x007a4a60`, vtable `0x0101ea1c` (53 slots), taille **0x118**,
dérivée de `UIWindow_composite_ctor`.

| offset | valeur au ctor | rôle |
|---|---|---|
| +0xB4 | 35 | pas de la grille (px) |
| +0xB8 / +0xBC / +0xC0 / +0xC4 | 20 / 10 / 10 / 30 | marges haut / gauche / droite / bas |
| +0xC8, +0xCC | 5, 2 | décalage de l'icône dans la case |
| +0xD0, +0xD4 | 0, 20 | décalage du bouton à bascule |
| +0xD8 | 10 | — |
| **+0xDC** | **2** | **nombre minimum d'objets à cocher** |
| +0xE0 / +0xE4 / +0xE8 | — | boutons **184** (fusionner), **185** (annuler), fermeture (**201**) |
| +0xEC / +0xF0 / +0xF4 | — | `vector<UIToggleButton*>` begin / end / capacité |
| +0xF8 / +0xFC | — | poignée de redimensionnement (mode 7) / barre de défilement |
| +0x100 … +0x114 | — | total, colonnes, lignes, cases visibles, défilement, défilement max |

Méthodes virtuelles utiles : `OnCreate` +0x3C (`0x007a5660`), `OnPaint` +0x50
(`0x007a5680`), `OnLButtonDown` +0x64, `OnMouseStay` +0x70, `OnMouseWheel`
+0x8C, **`OnMsg` +0x94** (`0x007a6010`).

`CMergeItemWnd_ClampSizeToGrid` 0x007a5e90 borne la fenêtre entre **5 et 10
colonnes** et **3 et 6 lignes**, toujours sur un multiple du pas de 35 px.

### Les messages `OnMsg`

| msg | rôle |
|---|---|
| 2 | un bouton a basculé → sélection groupée si Ctrl |
| 6 | bouton : **184** = fusionner, **185** et **201** = annuler (`CZ 0x0974` + fermeture) |
| 7 | molette (`+0x110 += delta`) |
| 14 | param **7** = redimensionnement |
| 34 | remplissage depuis `ZC 0x096D` |
| 60 | succès de fusion depuis `ZC 0x096F` |

## 5. Les messages affichés — sept clés, toutes traduites

| id | clé | émetteur |
|---|---|---|
| 2169 | `MSI_MERGE_ITEM` | titre de la fenêtre (`OnPaint`) |
| 2170 | `MSI_SELECT_ITEM_TO_MERGE` | client, moins de 2 objets cochés |
| 2171 | `MSI_MERGE_ITEM_SUCCESS` | client, `reason = 0` |
| 2172 | `MSI_MERGE_ITEM_FAILED_NOT_MERGE` | client, `reason = 1` |
| 2173 | `MSI_MERGE_ITEM_FAILED_MAX_COUNT` | client, `reason = 2` |
| 2182 | `MSI_MERGE_ITEM_FAILED_NOT_EXIST` | *personne* |
| 2183 | `MSI_NOT_EXIST_MERGE_ITEM` | **serveur**, `clif_msg` |

✅ Les sept sont présentes en français **et** en espagnol dans
`tools/lang/msgstring.fr.yaml` / `.es.yaml`. Rien à traduire.

### 🔴 Le piège des deux clés voisines — vérifié, sans conséquence ici

`src/map/clif.hpp` définit `MSI_NOT_EXIST_MERGE_ITEM` **deux fois** : `2184`
(ligne 640) et `2183` (ligne 658). La première est gardée par
`#if (PACKETVER >= 20130807 && PACKETVER <= 20130814)` ; Moonlight compile avec
`PACKETVER 20250716` (`src/custom/defines_pre.hpp`), donc c'est **2183** qui
s'applique — exactement l'index du client. Aucun décalage.

Le contrôle vaut la peine d'être refait sur tout message envoyé par `clif_msg` :
un `id` msgstring est un **index dans la table de l'exe**, et une garde
`PACKETVER` mal tombée afficherait la phrase d'à côté sans la moindre erreur.

## 6. 🔴 Ce qui bloque réellement : l'ouvreur est hors d'atteinte

Le client n'a **aucun** raccourci, aucun bouton et aucune commande qui ouvre la
fenêtre 227 : le seul chemin est le `ZC 0x096D`. Côté serveur, un seul émetteur —
`clif_merge_item_open`, appelé par la commande de script **`mergeitem`**
(`script.cpp:24245`), elle-même utilisée par un seul NPC :

```
moon/rathena/other/item_merge.txt:20
    itemmall,35,75,3  script  Mergician#mall  1_M_WIZARD,{
```

Le fichier **est bien chargé** (`moon/scripts_moon.conf` ligne 369, et c'est
`map.cpp:4383` qui fixe ce fichier comme LA liste de scripts). La carte
`itemmall` existe (`db/map_index.txt:679`, `conf/maps_athena.conf:790`).

**Mais rien ne mène à `itemmall`.** Sur tout l'arbre `moon/`, le seul fichier qui
mentionne cette carte est `item_merge.txt` lui-même. Les scripts d'upstream qui
y warpent (`npc/re/merchants/cashmall.txt`, `rgsr_in.txt`) sont dans `npc/`,
**qui n'est pas chargé**. Un joueur ne peut donc atteindre le Mergician que par
`@warp itemmall`.

⚠ Le même constat vaut pour les deux autres candidates de
[unexplored_systems.md](unexplored_systems.md) : le *Devil Enchant Master*
(`enchan_upg.txt`) et l'*Equipment Reform PR Agent* (`rgsr_in.txt`) sont eux
aussi sur `itemmall` — et leurs fichiers ne sont pas dans `scripts_moon.conf`.
**`itemmall` est la carte-entrepôt de ces trois systèmes, et elle est murée.**

### Le remède, si l'utilisateur veut ouvrir le système

Une ligne. Le script porte déjà la variante décommentable :

```
//prontera,146,95,3  script  Mergician#pron  1_M_WIZARD,{
itemmall,35,75,3     script  Mergician#mall  1_M_WIZARD,{
```

Déplacer le NPC sur une carte fréquentée (ou dupliquer l'en-tête) suffit : tout
le reste — protocole, fenêtre, messages, traductions — fonctionne déjà.
**Aucun `feature.*` à activer**, contrairement au barter.

## 7. Ce que Bourgeon aurait à faire — rien d'urgent

La fenêtre native est **saine** : elle se peint, se redimensionne, défile, gère
la sélection groupée au Ctrl et applique correctement le résultat. Elle n'entre
dans aucune des listes de natives à remplacer.

Si elle devait passer en ImGui un jour, les points à reprendre sont :
1. le `msg 60` — sans lui, le slot survivant affiche l'ancienne quantité
   jusqu'au prochain rechargement d'inventaire ;
2. l'**ordre** des index envoyés : `indices[0]` est le survivant ;
3. le minimum de 2 objets, sinon `MSI_SELECT_ITEM_TO_MERGE` ;
4. le `CZ 0x0974` à la fermeture (sans effet serveur, mais c'est le contrat).

## 8. Annexe — pièges rencontrés pendant ce relevé

1. 🔴🔴 **Décompiler une adresse de `case` fait décompiler tout le
   dispatcher.** `0x00ca89e0` appartient à `RecvLoop_DispatchPackets`
   (`0x00c9df00`, **0xc3dd octets**) : la demande a fait **expirer le pont MCP**
   et l'a laissé occupé plusieurs minutes. Sur un corps de `case`, désassembler
   une plage bornée — jamais `decompile`.
2. 🔴🔴 **Hex-Rays a menti deux fois, dans les deux sens :**
   - au **site d'appel** de l'`OnMsg`, il affiche `0` en 3ᵉ argument là où le
     désassemblage pousse `[ebp+Block]` — j'ai failli annoncer un
     déréférencement nul à l'ouverture de la fenêtre ;
   - dans `CMergeItemWnd_SendMergeRequest`, il réduit un `cmp edi, [ecx+0DCh]`
     à `if ( a1[55] )` — j'ai failli annoncer que le bouton « fusionner »
     n'envoyait jamais rien.
   Les deux fois, le `retn 18h` et le désassemblage ont tranché en une minute.
   Cf. [[feedback_native_hooking]].
3. 🔴 **Deux `grep` vides, deux fois pour une mauvaise clé** : `Guid:` au lieu
   de `UniqueId:` dans `db/`, puis un identifiant numérique au lieu de la clé
   `MSI_*` dans `msgstring.fr.yaml`. Les deux auraient produit une conclusion
   fausse et confortable (« aucun objet fusionnable », « rien n'est traduit »).
   Le témoin positif — chercher une valeur dont on est **sûr** qu'elle existe —
   les a démasqués tout de suite.
   Cf. [[feedback_absence_needs_measurement]].

---

Voir aussi : [unexplored_systems.md](unexplored_systems.md),
[native_window_dispatch.md](native_window_dispatch.md) §8,
[opcode_map.md](opcode_map.md), [memorial_dungeon_re.md](memorial_dungeon_re.md).
