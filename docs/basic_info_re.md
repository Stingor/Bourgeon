# Fenêtre « Basic Info » native (`UIBasicInfoWnd`) + grille d'icônes de menu

Rétro-ingénierie de la fenêtre HUD « Basic Info » du personnage (pseudo, classe,
niveaux, HP/SP, Zeny, poids, barres d'exp) et de la grille d'icônes de menu qui
se docke juste en dessous : constructeurs, vtables, layout, dispatch des messages,
sources des valeurs live, et comment Bourgeon masque/réimplémente le tout.

Client `20250716` (Moonlight-Destiny), base `0x00400000`, **pas de rebase ASLR**
(Ghidra == live). Adresses/offsets renommés + commentés dans Ghidra, plusieurs
points vérifiés en live avec x32dbg. Réimplémentation ImGui côté client dans
[`src/plugins/basic_info.cc`](../src/plugins/basic_info.cc) et
[`src/plugins/menu_icons.cc`](../src/plugins/menu_icons.cc).

> **TL;DR** — La fenêtre est la `UIWindow` **ID 0** (`vtbl 0x0103e35c`). Son
> `DrawContent` (`0x0095e620`) lit directement des **globals de session** (HP/SP,
> Zeny, poids, exp, niveaux) et se dessine en deux hauteurs (repliée = barre une
> ligne / dépliée = panneau vertical), avec des variantes **4e classe** et des
> variantes **localisées**. Elle **docke la grille d'icônes** (`ID 0x133`) à
> `(x, y+hauteur)` sur déplacement. Bourgeon la masque **pré-rendu** en
> détournant son handler `OnMsg` (msg `0x22`) pour la pousser hors-écran, sans
> corrompre sa position sauvegardée.

---

## 1. `UIBasicInfoWnd` — la fenêtre Basic Info

### Identité & création
- Window-manager **ID 0** ; pointeur singleton à `*(g_UIWindowMgr 0x0131f4e8 + 0x1dc)`
  = **`0x0131f6c4`** (null tant que le HUD n'est pas créé).
- `MakeWindow` case ID 0 @ `0x00a394d6` : `operator new(0x118)` → objet de **280 octets**.
- RTTI `.?AVUIBasicInfoWnd@@` (typedesc `0x0124084c`, COL `0x010c6564`).
- **vtable `vtbl_UIBasicInfoWnd` @ `0x0103e35c`** (slot 1 = `UIWindow_OnDraw_Base`,
  `DrawContent` = slot 20, `OnMsg` = vtable+0x94).
- ctor `UIBasicInfoWnd_ctor` @ `0x0095d060`. Deux jeux de tailles choisis par
  `JobId_Is4thClass(charJob)` (`0x00d734f0`, vrai pour job `0x109C`–`0x10E9`) :

  | | extraY | h repliée | h dépliée |
  |---|---|---|---|
  | normal | 13 | 55 (`0x37`) | 134 (`0x86`) |
  | 4e classe | 28 | 71 | 149 |

- Champs de l'objet : `+0x14` largeur, `+0x18` hauteur courante, `+0xb4` h repliée,
  `+0xb8` h dépliée, `+0xf8` extra-Y. Position à `+0x1c`/`+0x20` (X/Y).
- **Live** : objet `@0x14620b80` avait vtable `0x0103e35c`, largeur `0xDC`,
  hauteur `0x86` (dépliée), `+0xb4=0x37`, `+0xb8=0x86` (Gunslinger = non-4e classe).

### Rendu — `UIBasicInfoWnd_DrawContent` @ `0x0095e620`
Dispatche selon la **hauteur courante** :
- **dépliée** (`hauteur == +0xb8`) : panneau vertical complet — titre « Basic Info »,
  HP/SP, `Base Lv.%d`, `Job Lv.%d`, `Zeny:%s`, poids.
- **repliée** (`hauteur == +0xb4`) : barre une ligne « Lv / classe / Lv / Exp% ».
- **4e classe** : chemins dédiés `UIBasicInfoWnd_DrawExpanded_4thJob` @ `0x0095d300`
  et `UIBasicInfoWnd_DrawCollapsed_4thJob` @ `0x0095da30`.
- **Localisation** : libellés alternatifs (« Peso » / « Classe » / « Base. ») quand
  `DAT_0159b810 == 0xc`.

Nom de classe (comme `DrawContent`) : `jobid = FUN_00d5b580(session)` puis
`name = FUN_00d5bb40(session, jobid, -1)` (les deux `__thiscall`, this = session
`0x015fa3c0`).

### Sources live des valeurs (globals de session)
Confirmées par RE de `DrawContent`/`OnCreate` (le ctor pousse les max via
`UIINT64BarGraph_SetCurMax`) :

| Valeur | cur | max | Type |
|---|---|---|---|
| Base EXP | `0x015fb9d0` | `0x015fb9d8` | INT64 |
| Job EXP | `0x015fb9e8` | `0x015fb9e0` | INT64 |
| HP | `0x015ff908` | `0x015ff90c` | INT32 |
| SP | `0x015ff910` | `0x015ff914` | INT32 |
| Zeny | `0x015fba90` | — (cap INT32) | INT32 |
| Poids | `0x015fbaa0` | `0x015fba9c` | INT32 |
| Base Lv. | `0x015fb9f0` | | INT32 |
| Job Lv. | `0x015fb9f8` | | INT32 |

- Le Zeny n'a **pas de max en mémoire** : le client le stocke signed 32-bit, donc
  la jauge se remplit relativement au cap dur `INT32_MAX = 2 147 483 647` et
  l'affiche groupé par milliers. Le poids est une vraie fraction `cur / max`.
- `max` peut être **sous** `cur` (job exp) — c'est normal.

### Déplacement / dock / tooltip
- `UIBasicInfoWnd_OnMove_DockMenuIcons` @ `0x0095f240` : `FindWindow(0x133)` puis
  re-docke la grille d'icônes à `(x, y+hauteur)`.
- `UIBasicInfoWnd_OnMouseMove_WeightTooltip` @ `0x0095f190` : tooltip « Weight %d%% ».
- `UIBasicInfoWnd_OnMove` @ `0x0095f170`, `UIBasicInfoWnd_OnResize` @ `0x0095f140`.

---

## 2. `UIMenuIconWnd` — la grille d'icônes de menu

- Window-manager **ID 0x133** (byte-table `[0x133]=0xcb` → jumptbl `[203]=0xa412bf` ;
  appel ctor @ `0xa412ec`). Dockée sous Basic Info.
- RTTI `.?AVUIMenuIconWnd@@` (typedesc `0x0123e2d4`, COL `0x010c076c`).
  **vtable `vtbl_UIMenuIconWnd` @ `0x010281b0`** (`DrawContent` slot 20 =
  `0x00814150` = `UIMenuIconWnd_RebuildNodes`).
- ctor `UIMenuIconWnd_ctor` @ `0x00812930` : map d'icônes `@+0xb4`, map dup `@+0xbc`,
  liste `@+0xc4` ; 3 boutons d'en-tête `@+0xcc/+0xd0/+0xd4`.
- `UIMenuIconWnd_BuildIconList` @ `0x00812fb0` (vtbl slot 15) : **25 icônes**,
  bitmaps `\menu_icon\bt_<name>.bmp` (`/_press`/`/_new`), grille **5 par ligne**,
  cellule 42×43 (`col = pos%5*0x2a+9`, `row = pos/5*0x2b+0x17`) ; `pos` n'avance que
  pour les icônes affichées. Largeur `0xC9` repliée / `0x1FE` dépliée selon
  `g_MenuIconWnd_Collapsed` (`0x0160227c`).

### Nom d'icône → command id (posé dans `UIMenuIcon+0x2c`)
```
status=0xC0  equip=0xC3  item=0xC2  skill=0xC4  booking=0x17B  party=0xC7
guild=0x175  battle=0x178  quest=0x169  map=0xDB  navigation=0x1AE  option=0xC1
bank=0x1CD  rec=0x18F  mail=0x1DC  achievement=0x1D9  tip=0x1FF  shop=0x200
keyboard=0x172  sns=0x206  attendance=0x21C  adventurerAgency=0x220
repute=0x237  adventureguide=0x245  probability=0x24B
```
(`status` → `status_doram` pour la race Doram.) `BuildIconList` **masque** via
`switch break` : `0x17B, 0x18F, 0x1FF, 0x206, 0x21C, 0x220, 0x237, 0x245, 0x24B, 0x257`.

### `UIMenuIcon` (une icône)
- **vtable `vtbl_UIMenuIcon` @ `0x010280d4`** ; cmd id `@+0x2c`
  (`UIControl_SetCommandId` @ `0x005aa6b0`), tooltip `@+0x94`. ctor base `FUN_008172b0`,
  bitmaps d'état via `UIBitmapButton_SetStateBitmap` @ `0x0082dac0`.
- Clic `UIMenuIcon_OnLButtonDown` @ `0x008271f0`, gaté par **DEUX** conditions
  (sinon no-op) : (1) `*(char*)(icon+0xac)==0` ; (2) l'icône est la fenêtre active du
  mgr : `*(mgr+0x19c)==icon` (`mgr+0x19c = 0x0131f684`). Dans les bornes → pressed
  (`+0x30=1`) puis dispatch :
  - **PATH 1** (`icon+0x10 != 0`, toutes les vraies icônes ; `+0x10` = parent = la
    fenêtre `0x133`) : `FUN_00a38b40(mgr, 3, parent+0x2c)` — table getter
    `*(mgr+(action+id*8)*4+0xa34)` → handler enregistré, tail-jump.
  - **PATH 2** (`icon+0x10 == 0`, non utilisé ici) :
    `g_UICommandDispatcher(*0x0121333c)->vfunc[0x18](0, icon+0x2c, icon+0xb0, 0,0)`
    → gros processeur de commandes `0x00c86740` (jumptbl `@0xc930f0`, idx = sel-1).
- `UIMenuIcon_SetHelpTextByCmdId` @ `0x00814550` (cmdId → help-msg id :
  status `0xC0→0x69`, equip `0xC3→0x68`, item `0xC2→0x6A`, skill `0xC4→0x11B`,
  party `0xC7→0x67`…). Tooltip survol : `UIMenuIcon_OnMouseEnter_Tooltip` @ `0x008274a0`.

### Table cmd → action = `FUN_00814a70`
Handler PATH 1 (atteint via `OnLButtonDown → FUN_00a38b40`). Chaque `case <cmd>:`
ouvre une fenêtre par id via `FUN_00812e60(<winId>)` ou lance un sélecteur du
dispatcher. Notables :
```
map 0xDB → fenêtre 0x8c (carte du monde)     cash shop 0x200 → sélecteur 0x143
option 0xC1 → 0x9b   item 0xC2 → 8   equip 0xC3 → 0xa   skill 0xC4 → 0x25
status 0xC0 → 0xb    quest 0x169 → 0x2718
```

### Visibilité des icônes = patch WARP (`NewButtonVisibility`)
Les icônes masquées ne sont **pas** un feature-gate naturel : elles sont désactivées
par le patcher WARP (`WARP0716\Scripts\Patches\NewButtonVisibility.qjs`, wrappers
`HideNewButtons`/`ShowNewButtons`). WARP localise `BuildIconList` (`0x00812fb0`) via
la chaîne `"status_doram"`, puis patche une table d'octets de visibilité par bouton.
- Dispatch @ `0x0081385d` : `id -= 0x178; if id > 0xDF → toujours créer; else
  MOVZX al,[0x00814064+idx]; JMP [0x0081405c+al*4]`. Table
  `MenuIcon_VisibilityTable_WARP` @ `0x00814064` (224 octets, idx = id-0x178) :
  octet 1 = visible (`0x00813879` create), octet 0 = caché (`0x00813d74` skip).
- Seuls les boutons « new » (id ≥ `0x178`) sont togglables ; les icônes classiques
  (status/item/equip/skill/party/guild/quest/map/option/keyboard, id < `0x178`)
  contournent la table et sont toujours créées.

---

## 3. Réimplémentation & interaction côté Bourgeon

### Masquer la fenêtre native — pré-rendu via `OnMsg` (msg `0x22`)
`MakeWindow` (case ID 0) appelle `OnMsg` (vtable+0x94) avec **msg `0x22`**
(layout-restore) **pendant** la création, avant la 1ʳᵉ frame. Bourgeon détourne ce
slot de vtable (`0x0103e35c + 0x94`) au chargement de la DLL : après l'appel
original, si l'option est active, il pousse la fenêtre **hors-écran** par écriture
brute de `+0x1c/+0x20` à `-10000`. Attraper à la création évite le flicker de login.
- **Pourquoi hors-écran et pas « caché sur place » ** : une fenêtre cachée sur place
  reste une **cible de snap/dock** (fantôme). Hors-écran → plus une cible
  (live-vérifié).
- **Ne corrompt PAS la position sauvegardée** : `BASICINFOWNDINFO.X/Y` persiste dans
  un store séparé du window-manager (`mgr+0x514`) que l'écriture brute ne touche
  jamais (LIVE-VÉRIFIÉ x32dbg : pos live → -10000 tandis que `mgr+0x514` restait à
  (0,0)). La vraie position est capturée une fois avant l'override pour restauration.
- ABI du handler identique à Equip/Status (`ret 0x18`, this en ECX).
- Filet `OnTick` : re-cache si le jeu ré-affiche, et restaure la position quand
  l'option est désactivée (via le singleton `*(0x0131f6c4)`).

### Masquer la grille native + le fantôme
- Grille cachée en swappant `DrawContent` (`vtbl 0x010281b0 + 0x50`, `= 0x00814150`
  `RebuildNodes`) par un no-op. **⚠ `RebuildNodes` n'est PAS per-frame** (seulement
  au load/refresh/mode) — ne pas s'en servir comme signal de visibilité (flicker).
  La map d'icônes (`window+0xb4`) reste peuplée (FindIconByCmd marche encore).
- **Fantôme composité** : cacher via le swap laisse les pixels déjà compositée à
  l'écran. Fix (`menu_icons.cc`) : à chaque transition on/off, `pending_refresh_`
  drainé en `OnTick` → `RequestServerRefresh()` envoie CZ `0x0BFD` id
  `BOURGEON_SETTING_REFRESH = 25` → moonlight `clif_refresh(sd)` → ZC `0x91` →
  le client re-composite. Voir [`../src/plugins/menu_icons.cc`](../src/plugins/menu_icons.cc).

### Déclencher une commande d'icône depuis l'ImGui (MenuIconTweaks)
1. `FindWindow(0x133)` (`UIWindowMgr_FindWindow` @ `0x00a47b90`), puis parcourir la
   map `wnd+0xb4` (MSVC `_Tree` : key `@+0x10`, valeur `UIMenuIcon*` `@+0x14`).
2. Ouvrir les 2 gates de `OnLButtonDown` puis restaurer :
   `sa=*(mgr+0x19c); *(mgr+0x19c)=icon; sf=*(icon+0xac); *(icon+0xac)=0;
   OnLButtonDown(icon,1,1); *(icon+0xac)=sf; *(mgr+0x19c)=sa;`.
3. **Dispatcher depuis `OnTick` (phase UPDATE), JAMAIS `OnRenderUI` (Present)** —
   ouvrir une fenêtre lourde depuis le hook Present est le mauvais contexte. Pattern :
   enfiler la cmd au clic (`pending_cmd_`), dispatcher au tick suivant.
- La carte du monde (`0x8c`) a un crash **intermittent** dans son propre init
  (`FUN_005f4700 → sélecteur 9 @0x00c8bec3` → `strlen` sur ptr null @ `0x005f3e96`) —
  même code que le bouton natif. La déférence à `OnTick` réduit sans éliminer.
- Cacher l'ImGui quand le HUD est remplacé (carte monde) : `FindWindow(0x8c) != null`
  est un signal per-frame fiable (la fenêtre est détruite à la fermeture).

---

## Portée & suivi
Voir la mémoire projet `project_basic_info_menu_icons` (source de cette doc). La
réimplémentation Basic Info (barres exp/HUD, portrait/avatar sprite, aperçu d'items)
vit dans [`basic_info.cc`](../src/plugins/basic_info.cc) ; le portrait capture le
sprite du perso via le renderer d'acteur natif (hook du submit de quad `0x00a1b7c0`)
— documenté séparément avec le système sprite/rendu.

Docs liées : [`charselect_re.md`](charselect_re.md),
[`entity_nameplate_re.md`](entity_nameplate_re.md),
[`sprite_rendering_re.md`](sprite_rendering_re.md).
