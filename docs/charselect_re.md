# RE — Écran de sélection de personnage (`UINewSelectCharWnd`)

Client `20250716`. UI `select_character_ver3`. Cette page documente la fenêtre de
sélection de perso, sa grille de slots, et **le chemin pour afficher plus de 15
slots par page**.

RTTI : `.?AVUINewSelectCharWnd@@`. Vtable de base : `0x0101d424`.
Global du slot sélectionné : `g_CharSelect_SelectedSlot` = `0x015f8262` (octet).

## Fonctions clés

| Adresse | Nom (renommé Ghidra) | vtbl | Rôle |
|---------|----------------------|------|------|
| `0x0079a030` | `UINewSelectCharWnd_ctor` | — | Constructeur. Fixe `slots_per_page=15`, alloue tous les tableaux. |
| `0x0079b0f0` | `UINewSelectCharWnd_BuildPage` | idx15 `+0x3c` | Construit les objets-slot de la page courante + boutons/labels. |
| `0x0079d170` | `UINewSelectCharWnd_RenderSlots` | idx20 `+0x50` | Rend les sprites persos (paperdoll) dans chaque slot. |
| `0x0079d590` | *(render sélection)* | idx19 `+0x4c` | Rend le halo du slot sélectionné (site du crash off-by-one, cf. plus bas). |
| `0x0079d610` | `UINewSelectCharWnd_OnMsg` | idx37 `+0x94` | Dispatch messages/boutons (sélection, suppression, pagination, nav clavier). |

## Carte des champs (`this + offset`, indices dword `param_1[i]`)

| Offset | dword | Contenu |
|--------|-------|---------|
| `+0x00` | `[0]` | vtable = `0x0101d424` |
| `+0xd0` | `[0x34]` | vecteur de 12 labels de texte latéraux (**hardcodé 12**, sans rapport avec slots/page) |
| `+0xdc` | `[0x37]` | vecteur des labels de nom de perso (taille = slots_per_page) |
| `+0xe8` | `[0x3a]` | **vecteur des objets-slot de la page** (taille = slots_per_page) ← tableau du crash |
| `+0x100`| `[0x40]` | objet « effet sélection » (halo) |
| `+0x108`| `[0x42]` | tableau des enregistrements par slot, **stride `0x15c`**, taille = capacité (`+0x12c`) |
| `+0x10c`| `[0x43]` | tableau d'octets « flags » par slot (init à 1), taille = capacité |
| `+0x110`| `[0x44]` | tableau de `short` par slot (headgear top id), taille = capacité |
| `+0x114`| `[0x45]` | `timeGetTime()` de création (animation) |
| `+0x11c`| `[0x47]` | **page courante** (= `CURSLOT / slots_per_page`) |
| `+0x120`| `[0x48]` | **position sélectionnée dans la page** (= `CURSLOT % slots_per_page`) ← index du crash |
| `+0x124`| `[0x49]` | **nombre de pages** = `ceil(usableSlots / slots_per_page)` |
| `+0x128`| `[0x4a]` | **SLOTS_PER_PAGE = 15** ← champ pilote |
| `+0x12c`| `[0x4b]` | **capacité totale** = `slots_per_page * nb_pages` |
| `+0x130`/`+0x134` | `[0x4c]`/`[0x4d]` | `usableSlots` (2 copies) |
| `+0x138`| `[0x4e]` | slot en cours de renommage (`0xffffffff` = aucun) |
| `+0x13c`| — (octet) | flag « entrée en cours » (verrou anti double-clic) |
| `+0xf4`/`+0xf8` | — | boutons flèche page préc. / suiv. |
| `+0x104`| `[0x41]` | label `"%d / %d"` (num. de page) |

## Le « 15 » : source unique et dérivations

Tout le dimensionnement part d'**une seule instruction** dans le constructeur :

```
0x0079a103:  MOV dword ptr [EDI+0x128], 0xF      ; slots_per_page = 15
             ; bytes: C7 87 28 01 00 00 | 0F 00 00 00   (imm32 à l'offset +6)
```

À partir de `+0x128`, le ctor (`0x0079a030`) dérive **automatiquement** :

- vecteur slots `+0xe8` et vecteur name-labels `+0xdc` → redimensionnés à `slots_per_page` ;
- `usableSlots = max(nbPersos+1, normal+premium+billing)` ;
- `nb_pages (+0x124) = ceil(usableSlots / slots_per_page)` ;
- `capacité (+0x12c) = slots_per_page * nb_pages` ;
- records `+0x108` (stride `0x15c`), flags `+0x10c` (init 1), shorts `+0x110` → tous à la taille `capacité`.

`BuildPage`, `RenderSlots` et `OnMsg` relisent tous `+0x128` dynamiquement à
l'exécution. **Conséquence : augmenter l'imm32 à `0x0079a103` suffit pour que le
client *alloue et gère* N slots par page.** Aucune autre allocation n'est à toucher.

## Le vrai blocage : la grille 5 colonnes codée en dur

La disposition n'est PAS dérivée de `slots_per_page` — elle est **figée à 5
colonnes** dans plusieurs fonctions :

- **`BuildPage`** — position slot `i` : `x = (i % 5) * 0x9d + 10`,
  `y = (i / 5) * 0xc3 + 0x15`. Pitch **`0x9d=157` × `0xc3=195`**, taille slot
  `UIWindow_SetSize(0xa1, 0xc5)` = **161 × 197**. Name-labels :
  `x=(i%5)*0x9d+0x5a`, `y=(i/5)*0xc3+0x9e`.
- **`RenderSlots`** — texte « Billing Service » : `(i % slots_per_page) * 0xa3`.
- **`OnMsg`** — nav clavier haut/bas = `±5` (cases msg `0x12`/`0x13`), et
  repositionnement des labels : `((col)*0x9d - w/2)+0x58`, `row*0xc3+0xbd`.

Avec `slots_per_page=15` → grille **5×3**. La 3ᵉ ligne (idx 10-14) est à
`y = 2*195+21 = 411`, bas à `411+197 = 608`. Les flèches de page sont à `y=0x22e=558`.

**Si on passe à 20 (5×4)** : la 4ᵉ ligne (idx 15-19) tombe à `y = 3*195+21 = 606`,
bas à `803` → **déborde l'écran (~768) et recouvre les flèches / boutons OK/Créer.**

### Deux options pour vraiment afficher > 15 par page

1. **Plus de colonnes** (p. ex. 5→6/8) : patcher chaque `5` (`%5`, `/5`), le
   x-pitch `0x9d`, et la nav `±5` dans `BuildPage` + `RenderSlots` + `OnMsg`.
   Contrainte : slot large de 161 px pour un pitch de 157 ⇒ pour ajouter des
   colonnes il faut réduire le pitch (slots plus serrés/petits) ou élargir le fond,
   la zone utile faisant ~1024 px de large (5×157 ≈ 785 px + marge).
2. **Cellules plus petites** : réduire pitch `0x9d`/`0xc3` **et** la taille
   `UIWindow_SetSize(0xa1,0xc5)` pour caser plus de lignes/colonnes. Retouche plus
   lourde (les sprites paperdoll sont dessinés à taille fixe par `RenderSlots`).

Le plus propre reste : **augmenter le nombre de colonnes** en réduisant un peu le
pitch, plutôt que d'empiler des lignes qui débordent verticalement.

## Côté données : déjà en place (serveur moonlight = 45 persos)

**Le côté données est déjà fait** — pas de modif serveur nécessaire :

- [`src/custom/defines_pre.hpp:19`](../../moonlight/src/custom/defines_pre.hpp) :
  `#define MAX_CHARS 45` (override le défaut 15 de `mmo.hpp`). Avec
  `MAX_CHAR_VIP=6`, `MAX_CHAR_BILLING=0` → `MIN_CHARS = 45-6-0 = 39`,
  `char_per_account = 39`.
- `HC_ACCEPT_ENTER2` (`chclif_mmo_send0272`) envoie :
  `normal=MIN_CHARS`, `premium=chars_vip`, `billing=chars_billing`,
  `producible=char_slots`, `total=MAX_CHARS`.
  → globals client : `g_CharSelect_NormalSlots 0x015ffd60`,
  `g_CharSelect_PremiumSlots 0x015ffd64`, `g_CharSelect_BillingSlots 0x015ffd68`,
  `g_CharSelect_CreatableSlots 0x015ffd6c`.
- **Validé live (x32dbg, 2026-07-21)** : `[015ffd60]=45`, `[015ffd64]=0`,
  `[015ffd68]=0`, `[015ffd6c]=45` → `usableSlots = 45+0+0 = 45`,
  `pages = ceil(45/15) = 3`. Instruction `0x0079a103` lue = `C7 87 28 01 00 00
  0F 00 00 00` (conforme). Le champ `normal` observé vaut ici 45 (et non 39) :
  l'important est que **la somme normal+premium+billing = 45** pilote les pages.
- `HC_CHARLIST_NOTIFY` envoie `p.total = max(char_slots/3, 1)` = un *nombre de
  pages* (hypothèse serveur : 3 slots/page). **Le client l'ignore pour la grille** :
  il recalcule ses propres pages depuis `slots_per_page`.

**Situation actuelle** : le compte a jusqu'à 45 persos → le client calcule
`ceil(45 / 15) = 3 pages de 15`, paginées. **Le travail restant est donc
100 % côté client (mise en page).** Objectif : condenser ces 45 slots en moins de
pages (idéalement une seule) en augmentant `slots_per_page` (`+0x128`) et en
élargissant la grille.

### Exemple concret pour 45 slots sur une page

45 = **9 colonnes × 5 lignes** (ou 5×9, mais 9 lignes débordent). Pour 9 colonnes
il faut, en plus de l'imm32 `0x0079a103` = `45` :
- remplacer les `% 5`/`/ 5` par `% 9`/`/ 9` et la nav `±5` par `±9` (BuildPage,
  RenderSlots, OnMsg) ;
- réduire le x-pitch `0x9d`(157) : 9 col × ~110 px ≈ 990 px (tient dans ~1024) →
  slots plus étroits (réduire aussi `UIWindow_SetSize(0xa1,0xc5)`) ;
- 5 lignes × pitch `0xc3`(195) = 975 px → **déborde encore** (~768). Il faut donc
  aussi réduire le y-pitch (p. ex. ~140) et la hauteur de slot.

En clair : passer à « tout sur une page » impose de **rétrécir sensiblement les
cellules** (sprites paperdoll dessinés à taille fixe par `RenderSlots` → à revoir).
Un compromis raisonnable : **5×5 = 25 / page (2 pages)** ou **6×4 = 24 / page**,
qui tiennent mieux dans l'art existant qu'un 9×5.

## 25 slots/page : approche patch-natif ABANDONNÉE → refaire en ImGui

L'implémentation par patch des immédiats de layout du client natif a été **retirée**
(2026-07-21). Elle fonctionnait sur le fond (25/page, `ceil(45/25)=2 pages`) mais
butait sur un mur **physique** : la carte de slot est un bitmap **fixe ~197 px** que
`SetSize` **coupe** (ne met pas à l'échelle), et 5 lignes ne tiennent pas dans la
hauteur disponible sans **couper ou chevaucher** les cartes. Trop de constantes
couplées (pas + hauteur + offsets nom `0xbd`/sprite `0x9e`, ×3 sites OnMsg) pour un
rendu jugé insuffisant.

**Décision : refaire en overlay ImGui** (grille dessinée par-dessus, propre
pagination, tailles libres → ni bitmap fixe ni coupe). Le code retiré, les valeurs
testées et **tous les sites de patch** sont archivés dans
[archive/charselect_25_per_page_native_patch.md](archive/charselect_25_per_page_native_patch.md).
La carte des champs et fonctions ci-dessus reste la base pour l'implémentation ImGui
(lire la liste de persos via le dispatcher `g_UICommandDispatcher` cmd 8, les
records `+0x108`, les comptes `g_CharSelect_*`).

## Rappel : crash de pagination (déjà corrigé)

Le rendu du slot sélectionné (`0x0079d590`, idx19) lit `slots[+0x120]` **sans**
garde `index < slots_per_page`. En paginant, `+0x120` vaut `slots_per_page`
(sentinelle « aucune sélection ») → lecture 1 cran après la fin du vecteur `+0xe8`
→ crash intermittent (souvent 3ᵉ page). **Corrigé** par `InstallCharSelectPagingFix`
([ragnarok_client.cc](../src/ragnarok/ragnarok_client.cc)) : détour à `0x0079d5e0`
qui réinsère la garde `+0x120 < +0x128`. Ce correctif lit `+0x128` dynamiquement →
il **reste valide** si on augmente slots_per_page. Voir `project_charselect_paging_crash`.
