# Fenêtre « Basic Info » native (`UIBasicInfoWnd`) + grille d'icônes de menu

Rétro-ingénierie de la fenêtre HUD « Basic Info » du personnage (pseudo, classe,
niveaux, HP/SP, Zeny, poids, barres d'exp) et de la grille d'icônes de menu qui
se docke juste en dessous : constructeurs, vtables, layout, dispatch des messages,
sources des valeurs live, et comment Bourgeon masque/réimplémente le tout.

Le **§4** documente en plus le bouton « cash shop » posé près de la minimap
(`UInCash_CallWnd`, ID 190) : ce n'est pas une icône de la grille, mais son
réglage vit dans la même section du panneau Moonlight.

Client `20250716` (Moonlight-Destiny), base `0x00400000`, **pas de rebase ASLR**
(Ghidra == live). Adresses/offsets renommés + commentés dans Ghidra, plusieurs
points vérifiés en live avec x32dbg. Réimplémentation ImGui côté client dans
[`src/features/overlays/basic_info.cc`](../src/features/overlays/basic_info.cc) et
[`src/features/overlays/menu_icons.cc`](../src/features/overlays/menu_icons.cc).

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

### Jauges d'EXP — `UIBasicInfoWnd_LayoutChildren` @ `0x0095dfb0`
Les barres Base/Job EXP sont des **contrôles enfants** (`UIINT64BarGraph`, `this+0xf0` /
`this+0xf4`), créés dans `OnCreate` @ `0x0095e1d0` — `DrawContent` ne les dessine pas.
C'est `LayoutChildren` qui décide de leur sort, et il ne les **cache pas** : il les
**pousse hors du cadre** (`x = -200`) quand il croit le personnage au niveau max :

```c
if ( Job_GetMaxBaseLevel(job) == g_Own_BaseLevel )  MoveWindow(barreBase, -200, 78);
else                                                MoveWindow(barreBase, 85, extraY + 76);
if ( g_Own_JobLevel < Job_GetMaxJobLevel(job) )     MoveWindow(barreJob, 85, extraY + 88);
else                                                MoveWindow(barreJob, -200, 88);
```

- `Job_GetMaxBaseLevel` @ `0x00d99ca0` et `Job_GetMaxJobLevel` @ `0x00d99d30` renvoient les
  plafonds de `MaxLevelTable` d'**ExternalSettings_kr.lub** (globals `g_ES_Max*`
  `0x01602288`+, lus une fois au boot). Un job sans branche dédiée — dont **High Wizard
  (4010)** — retombe sur `g_ES_MaxBaseLevel` = **99** ; les 2e classes transcendantes sur
  `g_ES_MaxJobLevel2nd` = **70**.
- Moonlight, lui, monte à **999 / 80** (`db/import/job_exp.yml`, `MAX_LEVEL 999`). D'où le
  bug observé : la barre de base disparaît **pile au niveau 99** et revient au 100 (le test
  est une **égalité stricte**) ; la barre de job disparaît dès le **job 70** et ne revient
  jamais (test `>=`). LIVE-VÉRIFIÉ x32dbg au base 99 / job 80 : les deux barres à
  `+0x1c = -200`, `+0x20 = 78` et `88`.
- ⚠ **Le lub ne peut pas porter le correctif** : ses plafonds sont par **catégorie** de job
  (les ~9 branches codées en dur des getters : novice / 2e / 3e / 4e / transcendant / Doram…)
  alors que Moonlight les définit par **groupe de jobs** dans `db/import/job_exp.yml`, bien
  plus fin — `MaxJobLevel` y vaut 10, 50, 52, 60, 70, 80, 99 ou 111 selon la classe. Aucune
  valeur unique par catégorie ne serait juste.
  *Effet de bord évité au passage* : `g_ES_MaxBaseLevel` sert AUSSI, en égalité stricte, à
  `CActorSprite_ApplyLevelJobAura` @ `0x00c41950` pour l'**aura de niveau 99** — le relever
  l'aurait déplacée au niveau 999. Sans conséquence sur Moonlight (auras de niveau non
  affichées), mais le détour laisse ce chemin intact.
- **Correctif Bourgeon** (`basic_info.cc`) : détour des deux **getters**, qui ne sont appelés
  que par la Basic Info (`Job_GetMaxBaseLevel` : `LayoutChildren` + `DrawCollapsed_4thJob` ;
  `Job_GetMaxJobLevel` : `LayoutChildren`).
  Le vrai plafond vient du **serveur** : `pc_nextbaseexp`/`pc_nextjobexp` (moonlight
  `src/map/pc.cpp`) renvoient une **sentinelle** au niveau max, `MAX_LEVEL_BASE_EXP`
  = 99 999 999 / `MAX_LEVEL_JOB_EXP` = 999 999 999, au lieu du coût du palier suivant.
  Le détour rend le niveau courant si la sentinelle est là (rejoue la branche « masquer »
  du natif), sinon un plafond hors d'atteinte. Aucune des deux sentinelles n'est un palier
  réel de la table Moonlight → pas de faux positif.
- `LayoutChildren` n'est rejoué qu'à la **création**, au **repli/dépli** et au **changement
  de classe** (`sub_D70D60` @ `0x00d70d60`) : un simple passage de niveau ne repositionne
  rien. Bourgeon rejoue donc le layout depuis `OnTick` quand l'état « au max serveur »
  bascule.

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
  le client re-composite. Voir [`../src/features/overlays/menu_icons.cc`](../src/features/overlays/menu_icons.cc).

### Déclencher une commande d'icône depuis l'ImGui (MenuIcons)
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

## 4. Le bouton « cash shop » près de la minimap — `UInCash_CallWnd` (ID 190)

Le petit bouton carré posé au coin haut-droit, contre la minimap, qui ouvre le
cash shop. **Ce n'est pas une icône de la grille du §2** : c'est une `UIWindow` à
elle seule, et elle a son propre interrupteur — la commande de chat `/cashshop`.

### Identité & création
- Window-manager **ID 190 (`0xBE`)** ; singleton caché à `g_UIWindowMgr+0x24C`
  (= `0x0131f734`). RTTI/vtable **`??_7UInCash_CallWnd@@6B@` @ `0x010349e4`**.
- ctor **`UInCash_CallWnd_ctor` @ `0x0088f870`** — `UIWindow_composite_ctor`,
  `operator new(0xB4)` = objet de 180 octets.
- `MakeWindow` case 190 @ **`0x00a3d3f7`** : `SetSize(43, 43)` (`0x2B`) puis
  `SetPos(largeur_écran − 187, 16)` — la largeur venant de
  `*(g_SceneRenderQueue 0x012515f8) + 0x28`.
- **Seul créateur : `GameMode_OnEnterMapSetup` @ `0x00c6bbad`**, à CHAQUE entrée
  de carte, sauf quand la map courante est `new_event.rsw`. La création n'est
  **pas** gardée par l'option (contrairement à sa voisine, la fenêtre `0x9F`,
  gardée par `OptionInfo_GetValue(0x10E)`) : c'est la queue commune du case,
  ci-dessous, qui applique le réglage.
- Slots de vtable utiles : **`+0x38` = `UIWindow_SetVisible` (`0x005aad80`)**,
  `+0x3c` = `OnCreate`, `+0x94` = `OnMsg`. `DrawContent` (`+0x50`) est un **stub** :
  la fenêtre ne peint rien, elle n'est que le porteur de son bouton enfant.

### Contenu & clic
- **`UInCash_CallWnd_OnCreate` @ `0x008b2790`** crée l'**unique** enfant, un
  `UIBitmapButton` dont les TROIS états (normal / survol / pressé) pointent le
  **même** fichier : `유저인터페이스\basic_interface\NC_CashShop.bmp`. Command id
  posé à **192 (`0xC0`)**, position enfant `(0, 0)`.
- **`UInCash_CallWnd_OnMsg` @ `0x008cf7c0`** — action 6, commande 192 :
  - Steam/Stove (`g_IsStoveLive` `0x015ffcb2`) : `GameMode::SendMsg(cmd 331, 2)` ;
  - sinon : `GameMode::SendMsg(cmd 323, 0)` → envoi de **CZ_SE_CASHSHOP_OPEN2
    `0x0B6D`**, donc le même chemin d'ouverture que décrit dans le RE du cash shop.

### L'interrupteur : la commande `/cashshop`
Les commandes de jeu sont indexées par la **table `TT_*` @ `0x01008120`** (296
entrées, `TT_GetIndexByName` @ `0x0068d140`), publiée en globales Lua par
`TT_SerializeTableToLua` @ `0x0068e510`. Celle qui nous intéresse est
**`TT_SHOW_CASHSHOP_BTN_ON_OFF` = index 218 (`0xDA`)**.

Deux chemins écrivent cette option, et ils font la même chose :

| Chemin | Adresse | Geste |
|---|---|---|
| Commande de chat `/cashshop` | `Chat_HandleChatMessage` case 218 @ `0x00c7c83b` | **BASCULE** : `FindWindow(190)` (sort si nul), `OptionInfo_SetValue(218, !valeur)`, puis `wnd->vtbl[0x38](valeur)` |
| Menu « Game Settings » | `GameSettingsCmd_ShowCashShopBtn_OnOff` @ `0x00693300` (enregistré par `CGameSettingsMgr_Init_Func` @ `0x00691c20`) | **IMPOSE** une valeur, même paire `OptionInfo_SetValue` + `SetVisible` |

- Accesseurs de l'option : **`OptionInfo_GetValue` @ `0x0068ea70`** et
  **`OptionInfo_SetValue` @ `0x0068fd50`**, tous deux `__cdecl(indexTT[, valeur])`,
  sur la table de hachage `0x012515fc`. `GetValue` rend le booléen dans l'octet
  faible d'EAX, et **0 quand la clé est absente**.
- **Persistance : le client s'en charge.** `OptionInfo_SaveToFile` @ `0x00d78970`
  (appelée depuis `WinMainCRTStartup_Run`, donc à la fermeture propre du jeu)
  appelle la globale Lua `SaveToFileCmdOnOffValueEx` qui déverse la liste on/off
  dans `SaveData\OptionInfo.lua` ; au démarrage, `OptionInfo_LoadAndApplyAll`
  la relit via `SetCmdOnOffList` → `c_SetCmdOnOffList` @ `0x00a9ce50`.
- **La visibilité survit aux changements de carte toute seule** : la queue commune
  du case 190 @ **`0x00a3d473`** (atteinte aussi bien par la branche « neuve » que
  par la branche « singleton déjà là ») refait
  `SetVisible(wnd, OptionInfo_GetValue(218))`.

### Voisins de la même famille (mêmes gestes, autres fenêtres)
| Commande | Index TT | Cible |
|---|---|---|
| `TT_SHOW_CASHSHOP_BTN_ON_OFF` | 218 | fenêtre **190** — `GameSettingsCmd_ShowCashShopBtn_OnOff` `0x00693300` |
| `TT_SHOW_GOLDPCCAFE_ON_OFF` | 219 | fenêtre **267** (`0x10B`) — `0x00693360` |
| `TT_SHOW_ROULETTE_BTN_ON_OFF` | 220 | `0x00693470` |
| `TT_SHOW_MINIMAP_BUTTON_ONOFF` | 223 | pas de fenêtre dédiée : `OnMsg 502` sur `dword_131f6a8` — `0x00693410` |

### Côté Bourgeon — l'interrupteur
Case à cocher **« Bouton du cash shop »** dans la section *Menu Icons* du panneau
Moonlight ([`menu_icons.cc`](../src/features/overlays/menu_icons.cc),
`MenuIcons::DrawSettings`). Elle est **hors** du `BeginDisabled` de la section :
ce bouton est une fenêtre du client, il reste réglable que la grille ImGui
remplace la native ou non.

Elle écrit la même option que la commande de chat — `OptionInfo_SetValue(218, v)` —
à une nuance près : elle **impose** la valeur au lieu de basculer, sinon la case
et le jeu pourraient diverger. Conséquences voulues :

- **rien à persister côté Bourgeon** pour l'état on/off (pas de clé YAML) :
  l'option est celle du client, écrite dans `SaveData\OptionInfo.lua` ;
- taper `/cashshop` en jeu met la case à jour toute seule, puisqu'elle **lit**
  l'option à chaque frame plutôt que de tenir un état à elle ;
- hors carte, quand la fenêtre n'existe pas encore, la case fonctionne quand même
  (l'option suffit, la prochaine création l'appliquera) — là où la commande
  native, elle, sort sans rien faire sur un `FindWindow(190)` nul.

### Côté Bourgeon — le rendre déplaçable
Le bouton est une **entrée de plein droit de `icons_`**, la liste du §3 : il hérite
donc du mode édition, de l'aimantage aux icônes voisines et à la grille
d'alignement, du clamp à l'écran et de la persistance par nom (clé `NC_CashShop`
sous `menu_icons:` dans le YAML). Le choix était « bouger la fenêtre native » ou
« la masquer et la redessiner » : c'est le second, parce que c'est déjà toute
l'architecture du fichier — aucune mécanique de glisser à réécrire.

Deux champs neufs de `Icon` portent les seules différences avec une icône de grille :

| Champ | Icônes de la grille | Bouton du cash shop |
|---|---|---|
| `dir` (dossier + préfixe sous `유저인터페이스\`) | `menu_icon\bt_` | `basic_interface\` |
| `wnd_id` (destinataire du clic) | `0x133` → handler `FUN_00814a70` | `190` → son propre `OnMsg(6, 192)` |

`wnd_id` — et non `cmd_id` — sépare les deux familles **partout** : le client a
DEUX commandes `0xC0`, « status » et « cash shop ». Trois endroits en dépendent :

- `RefreshBadges` : sans le test, le badge « nouveau » de *status* allumerait le
  bouton du cash shop, qui chercherait ensuite un `NC_CashShop_new.bmp` inexistant ;
- le **nom de la fenêtre ImGui** porteuse est `##micon_<wnd>_<cmd>` : deux fenêtres
  ImGui homonymes n'en feraient qu'une, la seconde héritant de la position et de la
  zone cliquable de la première ;
- la liste show/hide des réglages saute l'entrée (sa case est celle ci-dessus).

`MenuIcons::SyncCashShopButton` est le **seul** endroit qui décide qui, du natif ou
de notre copie, se voit. Elle tourne depuis `OnTick` — pas seulement aux bascules —
parce que le client **recrée sa fenêtre visible à chaque entrée de carte**. Règle :
le natif ne se montre que si l'option est allumée **et** que notre copie ne prend
pas le relais, « prendre le relais » exigeant que son bitmap soit *effectivement*
chargé (sinon : première frame, ou texture perdue à une remise à zéro du device,
et il n'y aurait plus aucun bouton).

**Infobulle = `MSI_CASHSHOP` (`0xC41`)**, le nom que le SERVEUR donne à sa
boutique : moonlight a rebaptisé toute la famille dans sa propre msgstringtable,
donc la ligne vaut **« Vote Shop »** — la monnaie n'y est pas de l'argent mais les
votes des joueurs. La lire au lieu de l'écrire en dur fait suivre d'office une
prochaine retouche de la table, et c'est déjà ce que fait l'icône « shop » de la
grille (`{"shop", 0x200, 0xC41}`), comme le titre de la fenêtre dans
[`cashshop_window.cc`](../src/features/windows/cashshop_window.cc).

> ⚠ **Ne PAS prendre `MSI_OUTSIDE_CASHSHOP_BTN_TOOLTIP` (id 3582)**, dont le nom
> promet pourtant l'infobulle de ce bouton exact : le natif ne s'en sert nulle
> part et la ligne n'a jamais été remplie — elle vaut « Title », comme sa voisine
> 3580 vaut « Material ». Vérifier le TEXTE d'un MSI avant de s'y fier :
> `D:\Mes documents\GitHub\references\msgstringtable.csv`, n° de ligne = id (0-based).

**Nommage à l'écran : « Vote Shop », jamais « cash shop ».** Le nom natif ne
survit que dans les commentaires, les noms de symboles et le nom de la commande
`/cashshop` ; toute chaîne visible du joueur dit « Vote Shop ».

---

## Portée & suivi
Voir la mémoire projet `project_basic_info_menu_icons` (source de cette doc). La
réimplémentation Basic Info (barres exp/HUD, portrait/avatar sprite, aperçu d'items)
vit dans [`basic_info.cc`](../src/features/overlays/basic_info.cc) ; le portrait capture le
sprite du perso via le renderer d'acteur natif (hook du submit de quad `0x00a1b7c0`)
— documenté séparément avec le système sprite/rendu.

Docs liées : [`charselect_re.md`](charselect_re.md),
[`entity_nameplate_re.md`](entity_nameplate_re.md),
[`sprite_rendering_re.md`](sprite_rendering_re.md).
