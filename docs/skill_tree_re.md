# Skill tree / grimoire — reverse engineering + the FPS "over-render" bug

> Ragnarok client (`Moonlight-Destiny.exe`). All addresses are VAs of the loaded
> image (Ghidra == x32dbg, image base `0x00400000`, no ASLR). RE done 2026-07-11
> (Ghidra, read-only) + live profiling from the DLL (Bourgeon `skill_tree_tweaks`).

This document covers the skill window ("grimoire"), the fact that **"legacy view" and "modern view"
are two MODES of a single window** (not two windows), and **why the "modern" mode tanks FPS** —
diagnosed by live timing, fixed by repainting only when the grid changes.

---

## 0. TL;DR

- The "Skill" button / hotkey **always** opens `UINewSkillListWnd` (window id **`0x25`**). No runtime
  choice of window class.
- "legacy view" vs "modern view" = **internal display mode**, driven by the option
  **`Simplicity_SkillList`** = `DAT_015fa454` (`0x015FA454`, default `0`). Toggle button inside the
  window (`OnMsg` case `0xCA`). `0` = 560×400 icon grid ("modern"); `!= 0` = ~330 px compact list
  ("legacy").
- **Root cause of the FPS drop (measured live)**: the grid's `DrawContent` (`0x00977E80`) runs **every
  frame** (≈ the FPS) and costs **9–15 ms per call, scaling with the number of skill icons** in the
  tab — it drags 144 Hz down to ~50 on a full tab. It rebuilds the entire grid in **immediate mode**
  every frame: per skill node it builds a texture key + `TextureMgr_Load` + an **O(n) `GetSkillInfoAt`
  walk-and-copy** → **O(n²) + hundreds of heap allocations, every frame** — even though the grid is
  static unless you hover / scroll / switch tab / spend a point.
- **The animated avatar was a red herring.** Removing it (`FUN_00974700`) changed the FPS by **nothing**
  — it is real per-frame work but tiny next to the per-node loop. Do not chase it.
- **Fix (implemented & validated — `skill_tree_tweaks` plugin)**: hook `DrawContent` and **repaint only
  when a render input changed** (dirty byte + tab/hover/scroll/points/size snapshot); skip the rebuild
  on static frames. Result in-game: **FPS back to native (144), display correct**.

---

## 1. Overview — one window, two modes

```
                    "Skill" menu button (cmd 0xC4)  ┐
                    skill hotkey (behavior 0x66)    ┴─► FUN_00812e60(0x25)  (toggle open/close)
                                                           │
                                                           ▼
                                      UINewSkillListWnd  (window id 0x25)
                                      instance cached @ mgr+0x2C4 = 0x0131F7AC
                                                           │
                     DrawContent 0x00977E80 reads  DAT_015fa454 (Simplicity_SkillList)
                                                           │
              ┌────────────────────────────────┴────────────────────────────────┐
      DAT_015fa454 == 0  ("MODERN")                              DAT_015fa454 != 0  ("LEGACY")
      window 560×400                                             window ~330 px (compact)
      FUN_00975730  grid 42 cells + per-node                     FUN_009750d0  list + per-node
      + FUN_00974700  animated class avatar (cheap)              (no avatar, no grid)
```

| | **"modern view"** (default) | **"legacy view"** |
|---|---|---|
| `Simplicity_SkillList` (`DAT_015fa454`) | `0` | `!= 0` |
| window size | 560 × 400 | ~330 px compact |
| render body | `FUN_00975730` (grid) + `FUN_00974700` avatar | `FUN_009750d0` (list) |
| per-frame cost | high (grid 42 quads + per-node O(n²)) | lower (fewer nodes, no grid bg) |

Both modes are immediate-mode and share the expensive per-node loop; the grid mode draws more (42 cell
quads + a bigger window + more icons), so it is the one that visibly tanks FPS.

> ⚠️ There is **also** a genuine, separate legacy class `UISkillListWnd` (id **`0x105`**), but **no
> menu/hotkey path opens it** in this client (superseded — see §7). Do not confuse it with the "legacy"
> *mode* above.

---

## 2. ⭐ The FPS bug — measured, not guessed

### 2.1 What the profiler showed

A timing hook on `DrawContent` (`0x00977E80`), logged once/second while the grid was open on a full tab:

```
[SkillTree] DrawContent: 48 calls/s, avg 15.24 ms, max 19.49 ms
[SkillTree] DrawContent: 54 calls/s, avg 13.36 ms, max 15.52 ms
...
```

Three facts fall out:
1. **It is called every frame.** `calls/s` tracks the live FPS (41–72), so the window repaints
   continuously while open — the engine drives it, it is not event-gated.
2. **Each call is enormous.** 9–15 ms, i.e. most of a 16.6 ms (60 Hz) frame in a single function. At
   144 Hz native, one 15 ms call alone caps the frame rate around 50.
3. **It scales with the number of skill icons** in the active tab (confirmed by the player: the more
   skills in the tab, the lower the FPS). So the cost is inside the per-node loop.

### 2.2 Where the time goes — immediate-mode rebuild, every frame

`DrawContent` starts by clearing the whole render-node list (`FUN_00a1cb30`); nothing is retained across
frames. Then the grid body `FUN_00975730` walks the tab's skill list and, **for each node**:

- `FUN_00d5e3a0` — sprintf the icon path.
- `UITexture_MakeKeyFromPath 0x00A9F030` — **NOT a cheap hash**: ~4–5 `std::string` allocations + path
  normalization.
- `UITextureMgr_Load 0x00A8D4A0` — `EnterCriticalSection` + more allocs + cache lookup (cached, so no
  disk I/O, but heavy per call); up to 3 loads/node (icon + fallback + "learned" overlay).
- **`GetSkillInfoAt 0x00976230`** — inits a temp SkillInfo, **linearly walks the tab list** to find the
  node by grid-index, **copies the full record (heap alloc)**, then frees it. Called once per node while
  iterating that same list → **O(n²) + one alloc/copy/free per node**. (It re-finds the very node the
  loop already holds — pure waste; see §6.)
- Several skill-DB map lookups + `UIText_MeasureWidth` + colored text draws.

For a 4th-class tab (~40+ skills) that is, **every single frame**: dozens of `EnterCriticalSection`,
hundreds of `std::string` allocations, an O(n²) list walk, and dozens of texture-cache lookups. That is
the 9–15 ms.

### 2.3 The avatar was a red herring

The grid mode also renders an animated class avatar (`FUN_00974700`: `.act` animation + sprite/palette
loads + a per-frame Lua call). It *looked* like the culprit (an animated sprite suggests forced
repaints). It is not: **NOP-ing its call recovered exactly 0 FPS.** The window repaints every frame
regardless of the avatar (the engine drives `DrawContent`, not the animation), and the avatar's own cost
is small next to the per-node loop. Lesson: **measure before blaming.**

---

## 3. The window: `UINewSkillListWnd` (id `0x25`, vtable `0x0103F660`)

- RTTI `.?AVUINewSkillListWnd@@` @ `0x01240C78` (COL `0x010C6C8C` / type-desc `0x01240C70`).
- **Factory**: byte-table `0x00A42CA8[0x25] = 0x22` → jump-table `0x00A42904[0x22]` → case
  `@0x00A3A55B` (`new 0x2B0` / ctor `0x00974060` / caches the instance at **`mgr+0x2C4` = `0x0131F7AC`**
  / register ; `mgr = 0x0131F4E8`). Object size `0x2B0` (688 bytes).

### 3.1 Notable virtual slots

| slot (+off) | address | role |
|---|---|---|
| dtor (+0x00) | `0x00974420` | scalar-deleting dtor |
| OnCreate (+0x3C) | `0x00976D70` | build layout (once; self-sends msg `0x17`) |
| **DrawContent (+0x50)** | **`0x00977E80`** | **paint (hot path, §2)** |
| OnLButtonDown (+0x64) | `0x009783E0` | begin-drag skill → action bar (cat 8) |
| OnLButtonUp (+0x68) | `0x009782B0` | click → msg `0x3A` (open desc) |
| OnMouseMove (+0x70) | `0x00978600` | hit-test + hover |
| OnRButtonDown (+0x7C) | `0x00978520` | level-up / reserve a point |
| dblclick (+0x84) | `0x009786F0` | MakeWindow(`0x2E`) + msg `0x3D` (detail popup) |
| OnMouseWheel (+0x8C) | `0x00978970` | scroll (self msg 7) |
| **OnMsg (+0x94)** | **`0x00979270`** | dispatcher (see §3.3) |

### 3.2 Instance offsets

| offset | field |
|---|---|
| `+0x14` / `+0x18` | live width / height |
| `+0x24C` | anchor/parent |
| `+0x250` | hovered index |
| `+0x254` | **active tab** |
| `+0x258` | **scroll row** (`this+600` in the render body) |
| `+0x25C` | grid columns |
| `+0x260` | rows/page |
| `+0x264`/`+0x268` | top/bottom insets |
| `+0x26C` | "has skill points" flag |
| `+0x271` | **dirty** byte (set by input handlers, cleared at end of `DrawContent`) |
| `+0x274` | avatar animation timer (`timeGetTime`) |
| `+0x278 + tab*8` | **4 skill linked lists** (one per tab) |
| `+0x298` | hover list |
| `+0x2A0` | reserved-levels list |

Skill node record (linked list): `+0x04` id, `+0x06` max-lvl, `+0x07` learned-lvl, `+0x0B` grid-index,
`+0x10` icon-id, `+0x00` next.

Grid (modern mode): cell `0x44 × 0x38`, origin `(0x4C, 0x2C)`, **42 cells** (`0x2A`).

### 3.3 `OnMsg 0x00979270` — useful cases

| msg / id | effect |
|---|---|
| **6 / `0xCA`** (@`0x00979520`) | **view toggle**: `DAT_015fa454 = (== 0)` then resize `0x14a=330` (compact) or `0x230=560`×400 (grid) |
| 6 / `0xD5` (@`0x0097956F`) | `DAT_015fa458 = (==0)` = **Show_SkillDescript** |
| 6 / `0xC9` | `SaveWindowRect(0x25)` (self-close) |
| 6 / `0xD8` | cycle tab |
| **6 / `0x10F`** (@`0x0097945F`) | **`btn_apply`** — commit the reserved points (see §3.4) |
| 6 / `0x16B` (@`0x009794D4`) | **`btn_reset`** — drop the reservations (`sub_976B60` + self-msg `0x17`) |
| 6 / `0xEF..0xF8` | the 10 per-row `skill_up_*.bmp` "+" buttons (compact mode) → **reserve** a point |
| 6 / `0x00..0x29` · `0x2A..0x53` | the 42 grid cells (level-up / level-down) → `sub_738570(skill, lvl)` |
| `0x0E` | resize (clamps height ≤ 400 in grid mode) |
| `0x16` | change active tab (`this+0x254`) |
| **`0x17`** | rebuild (FUN_00976b60 + FUN_00737ce0(DB) + relayout + repaint) |
| **`0x3C`** | job-change (FUN_009765f0) |
| `0x22` | (re)anchor to parent (`+0x24C`) + initial resize |
| `0x3A` | open desc (dispatcher cmd `0x71`) · `0xA1` re-fit tab |

### 3.4 Spending skill points — reserve, then apply

Points are **staged client-side first**, then committed in one go. Both view modes share the
same two buttons, created in `OnCreate` with `btn_apply_*.bmp` / `btn_reset_*.bmp` at
`(width-100, height-27)` and `(width-50, height-27)`.

1. **Reserve** — `sub_979BA0(this, skillId)` (`0x00979BA0`), reached from the "+" buttons
   (`OnMsg 6/0xEF..0xF8`) or from `OnRButtonDown 0x00978520`. Bails out returning `0` when
   `this+0x26C` (remaining points) `<= 0`, when `this+0x2A8` is set, when the skill isn't in
   the active tab's list, or when the node is already at max (`node+0x18 >= node+0x30`).
   On success it bumps the node level, decrements `this+0x26C` and the caller writes
   `*(WORD*)(this+0x271) = 0x0101`.
2. **Enable** — `sub_978EB0` (`0x00978EB0`) runs after every action and does
   `btn[0xAC] = (*(BYTE*)(this+0x272) == 0)` for **both** buttons. `+0xAC` is the *disabled*
   flag, so Apply/Reset are greyed until at least one point is reserved.
3. **Confirm** — `OnMsg` case `271` calls `UIWndMgr_ShowMessageBoxModal 0x00A31A30`
   (text `MsgStringTable` **`0x561`**) and **requires the return to be `0xBB`**; anything else
   jumps to `loc_9798DF` and the click is silently dropped.
4. **Commit** — `sub_974530` (`0x00974530`) diffs the window's list against the skill DB and
   emits `CGameMode::SendMsg(msg **67 / 0x43**, skillId)` once per pending level, which the
   client serialises as **`CZ_UPGRADE_SKILLLEVEL 0x0112`** (4 bytes: `12 01 <skillId:u16>`).

⚠️ **Trap (cost a debugging session).** Step 3 is the only fragile link: `0x00A31A30` returns
`185` (`0xB9`) — *without ever showing a box* — when a message box is already open, when
`g_pCurrentMode+0x624 == 1`, **or when something detours it**. Bourgeon's `char_select`
plugin does exactly that (`Detour_ShowModal`, suppresses modals hidden under the ImGui
char-select). Its `g_cover_active` flag used to stay stuck at `true` after entering the world,
which killed *every* native modal in game — Apply included. Fixed by also requiring
`!Bourgeon::IsGameActive()`. **If a native confirmation ever "does nothing", check
`0x00A31A30` for a `jmp` into `ddraw.dll` before suspecting the window.**

---

## 4. The toggle & the option (config)

### 4.1 Opening (always `0x25`)

- "Skill" menu icon = **command `0xC4`** (`UIMenuIconWnd_BuildIconList 0x00812FB0`; click `0x008271F0`
  → `g_UICommandDispatcher` obj `0x0121333C`, vtable `+0x18` = `0x00C86740` `CGameMode::SendMsg`).
- Skill hotkey = `UIWindowMgr_DispatchHotkeyBehavior 0x00A457E4` **case `0x66`** → menu `OnMsg(6, 0xC4)`.
- Menu dispatcher **`FUN_00814A70`** maps `0xC4` → **`FUN_00812E60(0x25)`** (open-if-closed / close-if-open
  toggle helper). No flag test; `0x105` never referenced → always the "modern" `0x25`.

### 4.2 The `Simplicity_SkillList` option

- `DAT_015fa454` (`0x015FA454`) = field `+0x94` of the OptionInfo/CSession singleton at base `0x015FA3C0`.
- Loaded (**default `0`**) in `OptionInfo_LoadAndApplyAll 0x00D759F0` (`@0x00D75B47`); saved in
  `OptionInfo_SaveToFile 0x00D78970` (`@0x00D78D38`).
- Secondary `Show_SkillDescript` = `DAT_015fa458` (`+0x98`, default `0`, button `0xD5`).

---

## 5. Populating the tree (data side) — event-driven, not per-frame

- **`FUN_00D70D60`**: Lua `GetInheritJob` + `InitSkillTreeView` loop (`FUN_00D96790`) → msg **`0x3C`**
  to the window (`mgr+0x2C4`). Triggered by **`FUN_00DA8ED0`** (job change).
- **`FUN_00D95E60`**: msg **`0x17`** (refresh) → the window, on skill events (learned / leveled).
- Lua modules `Lua Files\SkillInfoz\skilltreeview` + `Lua Files\cls\skilltreeview` loaded by
  `FUN_0171E200`; Lua fn `InitSkillTreeView` (string `0x0109BCC0`).

> The data rebuild is event-driven → not the FPS cause. The cost is 100% in the per-frame **drawing**.

---

## 6. Fix — repaint only when the grid changes (implemented & validated)

The grid is static unless the player hovers / scrolls / switches tab / spends a point, yet `DrawContent`
rebuilds it every frame. **`skill_tree_tweaks`** (`src/plugins/skill_tree_tweaks.cc`) hooks `DrawContent`
and skips the heavy original when no render input changed since the last real paint:

```
watch: this+0x271 (game dirty byte) | tab 0x254 | hovered 0x250 | scroll 0x258
       | points 0x26C | size 0x14/0x18 | skill-point level 0x015FB9FC
if unchanged -> return (keep last frame's render nodes); else -> run original.
```

Because the engine does NOT recreate the render node empty each frame, skipping keeps the previously
built geometry on screen. **Measured after the fix: FPS back to native (144 Hz), display and hover/
scroll behaviour correct.** The gate fails toward *painting* (any watched change repaints); if a rare
update path is ever found to leave the grid stale, add its field to the watch set.

Other options (not needed once the gate works, but each is a real win if you ever keep the per-frame
rebuild):

| # | Optimization | Effect |
|---|---|---|
| A | **Drop the redundant `GetSkillInfoAt`** in the draw loop — it re-finds and copies the node the loop already holds; read the field off the node directly (max-lvl is `node+0x06`) | removes the O(n²) walk + N heap allocs/frame |
| B | **Cache the icon texture per skill** (keyed by `node+0x10` icon-id) instead of `MakeKeyFromPath`+`Load` every node every frame | removes the per-node string-alloc + critical-section churn |
| C | Force `Simplicity_SkillList = 1` (compact mode) — fewer nodes, smaller window | player-side quick-win, changes the look |

> The animated avatar (`FUN_00974700`) is **left ON** — it costs ~nothing and removing it did not help.
> Note it only animates while the window repaints, so with the gate it animates during interaction and
> is static at rest.

---

## 7. The genuine old `UISkillListWnd` (id `0x105`) — superseded

A real, separate class, but **no menu/hotkey path opens it**. RTTI `.?AVUISkillListWnd@@` @ `0x01240678`;
vtable `0x0103F3EC`; ctor `0x00970D30` (factory case `0x105` only); OnMsg `0x00971560`; DrawContent
`0x00971120`; RebuildList `0x00971A20` (a vertical list of cached `UIItemLinkBtn`, built **once** — a
retained-widget design, nearly free per frame). Registry key `SKILLLISTWNDINFO.*` (`@0x0104BB04`).
Remaining role: skill **reordering** (packet **`0x9FB`**). Live check: bp on `0x00970D30` while opening
the grimoire → should never hit.

---

## 8. Gotchas / conventions

- `vtable+0x94` = `OnMsg` (`__thiscall`, 6 stack args); `+0x50` = `DrawContent`; `+0x3C` = `OnCreate`.
- Don't confuse the ids: skill/item **desc** = `0xC`; **action bar** = `0x24`; **grimoire** (the real
  window) = **`0x25`**; **old** grimoire = `0x105`.
- The "window instance" globals `DAT_0131F6xx`/`DAT_0131F7xx` are **fields of the window manager**
  `0x0131F4E8` (skill window = `mgr+0x2C4`), written indirectly by the factory (no direct write xref).
- `DAT_015fa454` = **`Simplicity_SkillList`** (compact mode), **NOT** a "reservation/sim" mode.
- `DrawContent` runs **every frame while the window is open** — the FPS lever is *how often* it runs
  (the gate), not the avatar.

---

### Address cheat-sheet

| Symbol | VA |
|---|---|
| Skill window `UINewSkillListWnd` id / vtable / ctor / size | `0x25` / `0x0103F660` / `0x00974060` / `0x2B0` |
| cached instance | `0x0131F7AC` (`mgr+0x2C4`, mgr `0x0131F4E8`) |
| **DrawContent (hot path, gate target)** | **`0x00977E80`** |
| grid body / list body | `0x00975730` / `0x009750D0` |
| `GetSkillInfoAt` (O(n)+copy, redundant) | `0x00976230` |
| animated avatar (red herring) | `0x00974700` |
| `OnMsg` (+ view toggle case 0xCA @0x00979520) | `0x00979270` |
| option `Simplicity_SkillList` / `Show_SkillDescript` | `0x015FA454` / `0x015FA458` |
| OptionInfo Load / Save | `0x00D759F0` / `0x00D78970` |
| open: menu cmd `0xC4` → | `FUN_00814A70` → `FUN_00812E60(0x25)` |
| `UITextureMgr_Load` / `MakeKeyFromPath` | `0x00A8D4A0` / `0x00A9F030` |
| Old `UISkillListWnd` id / vtable / ctor | `0x105` / `0x0103F3EC` / `0x00970D30` |

---
---

# Partie II — Le MODÈLE DE DONNÉES du grimoire (pour le réimplémenter)

> **Révision 2026-07-28** — RE complémentaire (IDA, décompilation fonction par fonction) mené
> pour écrire le **remplaçant ImGui Moonlight** (onglet « Grimoire » de la feuille de personnage).
> La partie I ci-dessus décrit la FENÊTRE native et son bug de FPS ; cette partie-ci décrit **d'où
> viennent les données**, ce qui permet d'afficher l'arbre **sans la fenêtre native du tout**.
> À partir d'ici la rédaction est en français (convention des docs récentes).

## 9. La vraie source : `CPlayerSkillBundle` @ `0x015FA3CC`

La fenêtre `UINewSkillListWnd` **ne possède rien** : elle recopie ses 4 listes d'onglets depuis un
objet global. Cet objet est le **champ `+0x0C` de la session** `0x015FA3C0`, donc à l'adresse fixe
**`0x015FA3CC`**. Le nom de classe vient du RTTI croisé dans `sub_738590`
(`CPlayerSkillBundle::lcEXCInfo`) — on l'appellera **`g_SkillBundle`**.

```
  g_SkillBundle = 0x015FA3CC
   +0x00  std::list<CSkillInfo>          (liste secondaire, cf. sub_737D00)
   +0x0C  std::map<int tab, std::list<CSkillInfo>>   <- LES 4 ONGLETS  (GetTabList)
   +0x14  std::list<CSkillInfo>          <- liste PLATE = le DERNIER onglet (= 0x015FA3E0)
   +0x28  std::list<...>                 (annexe, vidée avec l'arbre)
   +0x30  std::map<...>                  (annexe)
   +0x38/+0x3C  std::vector<int>         (tampon de travail de sub_738590)
   +0x44  int* g_UseSkillLevel           <- TABLEAU des « niveaux d'utilisation », indexé par skill id
```

- **`GetTabList(tab)` = `0x00738370`** — `__thiscall(ECX = g_SkillBundle, push tab)` -> `std::list*`
  (renvoie une liste vide statique `0x01318A28` si l'onglet n'existe pas). C'est **la** porte d'entrée.
- `sub_00976B60` (le « reset ») recopie ces 4 listes dans la fenêtre (`+0x278 + tab*8`) et remet
  `this+0x26C = g_Own_SkillPoints` (`0x015FB9FC`). **La copie est identique** : lire le bundle
  directement donne exactement ce que la fenêtre affiche.
- Le **dernier onglet** (index `nbOnglets-1`) n'affiche PAS une liste du map mais la liste plate
  `0x015FA3E0` (= `bundle+0x14`), dont les index de grille sont réattribués séquentiellement par
  `sub_00737CE0`.

### 9.1 `CSkillInfo` — 0x44 octets, vtable `0x01012968`

C'est LA structure : les nœuds de liste sont des `std::list` MSVC (`{next, prev, valeur}`), donc
**valeur = nœud + 8**.

| offset | type | champ | écrit par |
|---|---|---|---|
| `+0x00` | ptr | vtable `CSkillInfo` (`0x01012968`) | ctor `0x00739780` |
| `+0x04` | int | **valide** (1) — testé partout avant usage | ctor / `c_AddSkillList` |
| `+0x08` | int | **skill id** | Lua / paquet |
| `+0x0C` | int | `inf` (masque `skill_get_inf` ; 0 = passif) | paquet serveur |
| `+0x10` | int | **niveau appris** (0 = non apprise) | paquet serveur (`level`) |
| `+0x14` | int | coût SP au niveau courant | paquet serveur |
| `+0x18` | int | `upgradable` (le serveur dit « il reste du niveau ») | paquet serveur |
| `+0x1C` | int | portée (`range2`) | paquet serveur |
| `+0x20` | char* | **idname** (`"SM_BASH"`) -> nom du .bmp d'icône | Lua `GetSkillIdName` |
| `+0x24` | int | **index de case** dans la grille (`-1` = non placée) | Lua (`skillPos`) |
| `+0x28` | int | **NIVEAU MAX** | Lua (`MaxLv`) |
| `+0x2C` | int | **`UserUpgradable`** — `<= 0` => le joueur ne peut pas la monter | Lua |
| `+0x30` | int16 | `level2` = niveau appris (vérité serveur, jamais bidouillée en local) | paquet serveur |
| `+0x34` | int | coût en AP (Lua `GetLevelUseApAmount`) | Lua |
| `+0x38` | vector | **`std::vector<{u32 skillId, u32 level}>` = PRÉREQUIS** | Lua `c_AddNeedSkillList` |

⚠️ **`+0x10` vs `+0x30`** : les deux valent le niveau appris à l'arrivée du paquet. La différence est
qu'en **vue compacte** la réservation d'un point incrémente `+0x10` *en local* (dans la copie de la
fenêtre), pas `+0x30`. D'où :
- `sub_00737FA0(id)` = `+0x10 > +0x30` -> affiche la pastille « montable » `item_skill.bmp` ;
- `sub_00974530` (Apply, vue compacte) renvoie la différence des deux.

En **vue grille**, la réservation ne touche à rien : elle vit dans une liste séparée (§11.1).

⚠️ `+0x20` est un **`char*` rendu par Lua** — il peut pendre. Pour notre code : rappeler
`GetSkillIdName(id)` soi-même plutôt que déréférencer ce pointeur.

### 9.2 Accesseurs natifs prêts à l'emploi (`ECX = g_SkillBundle`)

| adresse | prototype | rôle |
|---|---|---|
| `0x00738370` | `list* GetTabList(int tab)` | la liste d'un onglet |
| `0x00738320` | `CSkillInfo* FindInTree(int id)` | cherche dans **tous** les onglets du map |
| `0x00737CB0` | `CSkillInfo* FindInFlat(int id)` | cherche dans la liste plate `+0x14` |
| `0x00738550` | `int GetLevel(int id)` | `record+0x10` (0 si absente) |
| `0x00737FA0` | `bool CanShowUpMark(int id)` | `record+0x10 > record+0x30` |
| `0x00738500` | `int GetTabCount(int tab)` | taille de la liste de l'onglet (0 => onglet masqué) |
| `0x00738570` | `void SetUseLevel(int id, int lv)` | écrit `g_UseSkillLevel[id]` (id dans [1, 7998]) |
| `0x00D5E3C0` | `int GetUseLevel(int id)` | `__thiscall(ECX = 0x015FA3C0 !)` — lit le même tableau |

> `SetUseLevel` prend `ECX = bundle (0x015FA3CC)` et `GetUseLevel` prend `ECX = session
> (0x015FA3C0)` : **c'est le même tableau** (`bundle+0x44` == `session+0x50`), pas une incohérence.

### 9.3 Wrappers Lua utiles

| adresse | Lua appelé | renvoie |
|---|---|---|
| `0x0073A1F0` | `GetSkillName(id)` | nom localisé, `"Unknown-Skill"` sinon (`__cdecl(id)`) |
| `0x0073A090` | `GetSkillIdName()` | idname (`"SM_BASH"`), `"Zero Skill"` sinon — **`ECX = &CSkillInfo`**, lit `+0x08` |
| `0x0073ADB0` | `IsLevelUseSkill(id)` | vrai si l'effet dépend du niveau => affichage « n / m » |
| `0x00739E20` | `GetLevelUseApAmount(lv)` | coût AP |
| `0x00739FC0` | `GetSkillName(rec+0x08)` | idem, mais **`ECX = &CSkillInfo`** (+ remap 5042->5028, 5043->5036) |

Chemin d'icône : `sprintf(buf, "유저인터페이스\\item\\%s.bmp", idname)` (`0x00D5E3A0`, format
`0x00FE07D4`). Repli du natif si le .bmp manque : `유저인터페이스\item\am_berserkpitcher.bmp`
(oui, en dur — `0x01039364`).

## 10. D'où vient l'arbre : le Lua, pas le serveur

Le serveur n'envoie que **ce que le personnage a appris** (`ZC_SKILLINFO_LIST 0x010F` / `0x0B32`,
`ZC_ADD_SKILL 0x0111` / `0x0B31`). **La forme de l'arbre — cases, niveaux max, prérequis — vient
des fichiers Lua du client.**

### 10.1 Construction (changement de job -> `sub_00D96790`)

```
FUN_00DA8ED0 (changement de job)
  +- FUN_00D70D60
       +- sub_00D96790 :
            n = Lua GetInheritJob()             -- profondeur de la chaîne d'héritage
            vide le bundle (0x007382F0 / 0x00737BA0 / 0x007377C0)
            pour i = n .. 1 :
                job = InheritJob[i]
                tab = 0 si i est le sommet de la chaîne (ou job 4218/4220)
                      2 si aura spéciale / 4239..4242 / 4304..4307
                      3 si 4e classe / 4302 / 4303
                      1 sinon (dont 4308)
                Lua InitSkillTreeView(job, tab)
       +- msg 0x3C à la fenêtre (relayout des onglets)
```

`InitSkillTreeView` (Lua, `data\luafiles514\lua files\skillinfoz\skillinfo_f.lub`) itère
`SKILL_TREEVIEW_FOR_JOB[job]` et appelle, pour chaque entrée, **deux bindings C** :

| binding | adresse | signature réelle |
|---|---|---|
| **`c_AddSkillList`** | `0x00A9CC30` | `(tab, skillID, strSkillID, skillPos, MaxLv, UserUpgradable)` |
| **`c_AddNeedSkillList`** | `0x00A9CB80` | `(skillID, needSkillID, needLevel)` |

`c_AddSkillList` monte un `CSkillInfo` neuf (`+0x04=1`, `+0x08=id`, `+0x20=idname`, `+0x24=skillPos`,
`+0x28=MaxLv`, `+0x2C=UserUpgradable`, `+0x10..+0x1F` à zéro) et l'insère dans l'onglet `tab`
(`sub_007381D0`). Il **jette** l'entrée si l'idname est vide ou si `skillPos`/`MaxLv` valent `-1`.
`c_AddNeedSkillList` retrouve la fiche (`sub_00D871B0`) et pousse `{needSkillID, needLevel}` dans le
vecteur `+0x38`.

### 10.2 Les fichiers Lua (LISIBLES, non compilés dans ce client)

`E:\...\Moonlight-Destiny\data\luafiles514\lua files\skillinfoz\` :

```lua
-- skilltreeview.lub : la GRILLE, index de case -> skill
SKILL_TREEVIEW_FOR_JOB = {
  [JOBID.JT_SWORDMAN] = { [1] = SKID.SM_SWORD, [3] = SKID.SM_BASH, [8] = SKID.SM_TWOHAND, ... }
}

-- skillinfolist.lub : la FICHE de chaque skill
SKILL_INFO_LIST = {
  [SKID.SM_MAGNUM] = {
    "SM_MAGNUM",                       -- [1] = idname (icône)
    SkillName   = "Magnum Break",
    MaxLv       = 10,
    SpAmount    = { 30, 30, ... },     -- coût SP PAR NIVEAU
    AttackRange = { 1, 1, ... },
    bSeperateLv = false,
    _NeedSkillList = { { SKID.SM_BASH, 5 } },   -- PRÉREQUIS
  },
}
```

Autres champs vus : `IsPassive`, `ApAmount`, `Type`, `Quest`, `Soul`, `SkillScale`, `UserUpgradable`.
`skilldescript.lub` porte les descriptions. **`UserUpgradable` n'est défini par aucune entrée du
fichier livré** -> toutes les compétences sont montables par le joueur sur ce serveur.

> Conséquence pratique : tout ce qu'affiche le grimoire est lisible **soit** dans le bundle en
> mémoire (rapide, déjà fusionné avec l'état serveur), **soit** dans ces .lub (utile pour un outil
> hors-jeu). Le plugin lit le bundle.

## 11. Interactions — ce que fait le natif, exactement

### 11.1 Monter un niveau = RÉSERVER puis APPLIQUER

Les deux vues partagent les boutons `btn_apply` (cmd **271** = `0x10F`) et `btn_reset`
(cmd **363** = `0x16B`), posés à `(w-100, h-27)` et `(w-50, h-27)`.

**Réserver — `sub_00979BA0(this, skillId)`** — sort tout de suite si `this+0x26C <= 0` (plus de
points) ou si `this+0x2A8` (épinglage) est posé. Puis **deux algorithmes distincts** :

- **Vue GRILLE (`Simplicity_SkillList == 0`)** — parcourt la liste `this+0x298` (le skill survolé
  **et ses prérequis directs**, cf. §11.2) et, pour chacun :
  - cible = `niveau requis` du prérequis, ou `max(appris, réservé) + 1` pour le skill cliqué ;
  - refuse si `record+0x30 >= record+0x28` (déjà au max) ;
  - refuse si `record+0x2C <= 0` -> message de chat `MsgStringTable[0xDCB]` avec le nom du skill ;
  - dépense `min(cible - courant, points restants)` et écrit `{id, niveau}` dans la liste des
    **réservations `this+0x2A0`**.

  => **cliquer sur un skill verrouillé réserve automatiquement ses prérequis directs**, dans la
  limite des points disponibles. C'est le comportement à reproduire.
- **Vue COMPACTE** — trouve le nœud dans la copie locale de l'onglet et fait `node+0x10 += 1` ;
  refuse si `node+0x10 >= node+0x28`. Aucune liste de réservation.

**Appliquer — `sub_00974530`** — pour chaque réservation `{id, cible}` : envoie
`CGameMode::SendMsg(msg 67 = 0x43, id)` **une fois par niveau** entre `record+0x30 + 1` et `cible`,
puis vide la liste. Le client sérialise ça en **`CZ_UPGRADE_SKILLLEVEL 0x0112` (4 o :
`12 01 <skillId:u16>`)**. La vue compacte fait la même chose en diffant sa copie locale.

Le modal de confirmation (`MsgStringTable[0x561]`, retour attendu `0xBB`) et son piège de détour
sont décrits en partie I §3.4.

**Bouton Reset (`363`)** = `sub_00976B60` : recopie les 4 listes depuis le bundle, remet les points
et jette les réservations.

`sub_00978EB0` grise/dégrise Apply et Reset : `bouton+0xAC = (this+0x272 == 0)`.

### 11.2 Survol -> liste des prérequis (`this+0x298`)

`sub_009789C0`, appelé quand l'index survolé change (`OnMouseMove 0x00978600`) et au double-clic :
1. vide `this+0x298` ;
2. remet à `-1` la couleur de chaque onglet ;
3. trouve le nœud dont `+0x24` == index survolé ;
4. y pousse `{id, 0}`, puis **chaque entrée du vecteur `+0x38`** (`{prereqId, niveauRequis}`), en
   dédupliquant (niveau max gagné) ;
5. pour chaque prérequis, **colorie l'onglet où il vit** (`0xE1E7FC`) — l'onglet est deviné par
   **plage d'id, en dur** : `1..54`, `142..157`, `411..444`, `500..544` -> onglet 0 ;
   `2000..2532` -> onglet 2 ; tout le reste -> onglet 1.

⚠️ **Un seul niveau de profondeur** : le natif ne remonte pas la chaîne récursivement.

### 11.3 Le « niveau d'utilisation » (les `+` / `−` de chaque case)

Chaque case porte deux boutons : ids **0..41** (`+`) et **42..83** (`−`), gérés par le `default:` du
`case 6` de `OnMsg` :

```
SkillMgr_SetOption(session, 7, 1, 0)              -- marque « à sauvegarder »
n = GetUseLevel(id) +/- 1  ->  borné à [1, niveau appris]
SetUseLevel(id, n)                                 -- 0x00738570
```

Ce niveau **ne va pas au serveur** : il est persisté dans le JSON par personnage
(`CCharacterLinkedUserDataMgr`, clés **`UseSkillInfo` / `SKID` / `SKLv`**, écrit par `sub_005C4DA0`,
relu par `sub_005C49F0`), et c'est lui que la barre de raccourcis envoie au lancement du sort.
La case n'affiche « n / m » que si **`IsLevelUseSkill(id)`** est vrai ; sinon un seul nombre.

### 11.4 Le reste des gestes

| geste | code natif | effet |
|---|---|---|
| clic gauche | `0x009782B0` -> msg `0x3A` | ouvre la description (dispatcher cmd `0x71`) |
| clic droit | `0x00978520` | **réserve** un point (gate : `+0x04` et `+0x2C`) |
| double-clic | `0x009786F0` | épingle la surbrillance des prérequis **et** ouvre/ferme la fenêtre `0x2E` via `OnMsg(0, 0x3D, id, upgradable, niveau, 1)` |
| glisser | `0x009783E0` | pose le skill dans la barre d'action (catégorie 8) |
| survol | `0x00978600` | infobulle = **nom du skill** seulement |
| molette | `0x00978970` | scroll (`this+0x258` en LIGNES) |

### 11.5 Les onglets

`sub_009765F0` (aussi appelé par `OnMsg 0x3C` = changement de job) :
1. Lua **`JobSkillTab_GetTabName(0)`** -> 4 noms par défaut ; puis
   `JobSkillTab_GetTabName(indexClasseDeBase)` -> 4 noms spécifiques (`in_1sttab`..`in_4thtab`) ;
   l'index de classe vient d'une grosse table `jobid -> 0..25` (montures et 3e/4e classes repliées
   sur leur classe de base).
2. pour `tab` de 0 à 3 : si `GetTabCount(tab) > 0`, ajoute l'onglet (nom spécifique, sinon défaut).
3. à la **première** construction seulement, ajoute un **dernier onglet** nommé
   `MsgStringTable[0x445]` — c'est celui qui affiche la **liste plate** `0x015FA3E0`.

Donc : **au plus 4 onglets de job (ceux qui ont des skills) + 1 onglet « divers »**.

## 12. Recette de réimplémentation ImGui (onglet Grimoire de `character_sheet`)

**Lecture — 100 % passive, aucune fenêtre native requise :**

```
pour tab de 0 à 3 :
    head = GetTabList(0x00738370)(g_SkillBundle, tab)      -- ECX = 0x015FA3CC
    pour chaque nœud de la std::list (nœud->valeur = nœud + 8) :
        id      = *(int*)  (v + 0x08)
        inf     = *(int*)  (v + 0x0C)      -- 0 = passif
        appris  = *(int16*)(v + 0x30)      -- vérité serveur
        sp      = *(int*)  (v + 0x14)
        portee  = *(int*)  (v + 0x1C)
        pos     = *(int*)  (v + 0x24)      -- index de case (tri d'affichage)
        maxlv   = *(int*)  (v + 0x28)
        gate    = *(int*)  (v + 0x2C)      -- 0 => non montable par le joueur
        prereqs = vector<{u32 id, u32 lv}> à (v + 0x38) .. (v + 0x3C)
    -- + la liste plate 0x015FA3E0 pour l'onglet « divers »
points = *(int*)0x015FB9FC  (g_Own_SkillPoints)
nom    = GetSkillName(0x0073A1F0)(id)   ·   icône = GetSkillIdName -> ...\item\<idname>.bmp
```

Tout sous SEH, POD uniquement, **jamais** de `std::string`/vecteur natif recopié.

**Écriture — un seul paquet, revalidé serveur :**
`CZ_UPGRADE_SKILLLEVEL 0x0112` = `{u16 0x0112, u16 skillId}`, **un paquet par niveau**, envoyé via
`Bourgeon::SendPacket` (même voie que les autres plugins). Le serveur applique `pc_skillup`, puis
renvoie `ZC_SKILLINFO_UPDATE` + les points restants : **aucun état local à maintenir**, on peut
donc se passer complètement du mécanisme « réserver puis appliquer » (qui n'existait que pour
grouper les envois) — ou le reproduire pour garder l'ergonomie « je prépare, je valide ».

`SetUseLevel(0x00738570)` reste appelable tel quel pour le niveau d'utilisation (ECX = bundle).

**Ce qu'on ne réimplémente PAS** : la fenêtre `0x2E` (double-clic) — la description passe par le
chemin déjà en place dans Bourgeon (`MakeWindow(0xC)` + `OnMsg 0x18`, cf.
`project_skill_description_window_re`), enrichi par `item_desc_tweaks`.

**Cohabitation avec le natif** : quand « Interface moderne » est ON, `window_pos_tweaks`
(hook `MakeWindow`) masque la fenêtre `0x25` dès sa création (`win+0x28 = 0`) et ouvre la feuille
de personnage sur l'onglet Grimoire — même schéma que l'inventaire `8` ou l'entrepôt `0x21`.
Le plugin `skill_tree_tweaks` (gate de repaint) reste utile pour le mode natif.

### Fiche d'adresses — partie II

| symbole | VA |
|---|---|
| `g_SkillBundle` (session+0x0C) | **`0x015FA3CC`** |
| liste plate (dernier onglet) | `0x015FA3E0` (= bundle+0x14) |
| `g_UseSkillLevel` (int*) | `0x015FA410` (= bundle+0x44 = session+0x50) |
| `g_Own_SkillPoints` | `0x015FB9FC` |
| `GetTabList(tab)` | `0x00738370` |
| `FindInTree(id)` / `FindInFlat(id)` | `0x00738320` / `0x00737CB0` |
| `GetLevel(id)` / `GetTabCount(tab)` | `0x00738550` / `0x00738500` |
| `SetUseLevel(id,lv)` / `GetUseLevel(id)` | `0x00738570` / `0x00D5E3C0` |
| `GetSkillName` / `GetSkillIdName` / `IsLevelUseSkill` | `0x0073A1F0` / `0x0073A090` / `0x0073ADB0` |
| `c_AddSkillList` / `c_AddNeedSkillList` | `0x00A9CC30` / `0x00A9CB80` |
| construction de l'arbre (job change) | `0x00D96790` (<- `0x00D70D60` <- `0x00DA8ED0`) |
| maj d'un skill depuis un paquet | `0x00D7E730` |
| handler `ZC_SKILLINFO_LIST` (0x010F/0x0B32) | `0x00C9C6E0` (entrée de 15 o) |
| réserver / appliquer / reset | `0x00979BA0` / `0x00974530` / `0x00976B60` |
| liste des prérequis (survol) | `0x009789C0` -> `this+0x298` |
| noms d'onglets | `0x009765F0` (Lua `JobSkillTab_GetTabName`) |
| vtable `CSkillInfo` / ctor / copie / dtor | `0x01012968` / `0x00739780` / `0x007368F0` / `0x00739CD0` |

### §13 — Gestes et réglages de l'onglet Grimoire (livré, 2026-07-28)

Ce que le remplaçant ImGui expose, et **pourquoi** ça diverge du natif quand ça diverge.

| geste | effet | natif correspondant |
|---|---|---|
| clic gauche (sans glisser) | **réserve un point** + ses prérequis directs manquants | clic DROIT (`0x00978520`) |
| Ctrl + clic gauche | réserve **jusqu'au niveau max** (dans la limite des points) | aucun |
| clic droit | menu : monter, lancer, niveau d'utilisation ±, description | aucun (le natif n'a pas de menu) |
| Ctrl + clic droit | ouvre la **description** (`MakeWindow 0xC`) | clic GAUCHE (`0x009782B0` -> msg `0x3A`) |
| glisser | charge utile `BGN_SKILL` vers une barre d'action | glisser natif vers `UIShortCutWnd` |
| survol | infobulle + **flèches** de prérequis (ambre) et de suites (bleu, en chaîne) | surbrillance `0x009789C0` |

Les boutons gauche/droite sont donc **inversés par rapport au natif**, exprès : le clic gauche est le
geste courant (dépenser un point), et Ctrl + clic droit reprend le standard déjà en place dans
l'inventaire, l'entrepôt et le cart (« Ctrl + clic droit = description »). Un clic gauche qui
ouvrirait une fenêtre est par ailleurs incompatible avec le glisser vers la barre : c'est pour ça que
la réservation est testée **au relâché** et seulement si `GetMouseDragDelta` vaut encore `(0,0)`.

⚠ **Pas de bouton « + » par case.** Il en a existé un, dessiné au survol par-dessus la case : la case
étant soumise AVANT lui, elle captait le clic (il fallait `SetNextItemAllowOverlap`, et le curseur
repassait en flèche pendant l'appui). Le clic gauche direct remplace tout ça. La vue LISTE garde son
« + », qui a sa propre cellule de tableau — aucun recouvrement.

**Flèches en chaîne, dans les deux sens.** Ambre (« ce qu'il faut avant ») et bleu (« ce que ça
ouvre ») se propagent en **largeur** sur toute la branche, pas seulement au rang voisin : c'est le
chemin entier qu'on cherche en survolant une compétence. Un seul parcours paramétré par le sens
(`walk_chain(upstream)`) — chaque case développée une fois (le graphe a des raccourcis), profondeur
bornée à 6, trait qui pâlit/s'affine avec la distance. La flèche va **toujours du prérequis vers ce
qu'il débloque**, quel que soit le sens de parcours : c'est le sens de lecture de l'arbre, pas celui
du survol. Chaque arête passe par la **réduction transitive** ci-dessus — la liste de prérequis du
client est aplatie, la tracer telle quelle doublerait chaque chemin. Le test de réduction est
identique dans les deux sens : c'est la même arête.

**Vue LISTE = le même arbre, lu comme un arbre.** Profondeur = 1 + celle du prérequis le plus profond
**présent dans l'onglet**, calculée par passes successives (bornées par le nombre de nœuds : ça
converge en 3-4 tours et ça protège d'un cycle), puis tri par profondeur. Un prérequis est donc
toujours affiché AVANT ce qu'il débloque, ce qui permet de tracer les **coudes** de liaison dans la
gouttière — l'ancre du parent est déjà connue. Même dispositif que l'onglet Compétences de guilde,
plus la réduction transitive (sans elle, la liste aplatie du client doublerait les chemins).

⚠ **L'indentation est plafonnée à 5 niveaux** (pas de 14 px). Mesure faite sur les `.lub` du client,
arbres **1re + 2e classe fusionnés** :

| classe | profondeur max | classe | profondeur max |
|---|---|---|---|
| Bard / Dancer | 2 | Alchemist | 5 |
| Knight, Wizard, Assassin, Blacksmith | 3 | **Rogue** | **8** |
| Crusader, Priest, Sage, Hunter | 4 | **Monk** | **9** |

11 classes pré-renewal sur 13 tiennent en ≤ 5 niveaux ; Monk et Rogue partent loin, mais avec **une
seule compétence par palier profond**. Indenter linéairement mangerait la colonne du nom pour une
poignée de lignes — au-delà du plafond le décalage se fige et c'est le coude qui dit le parent.
(Hors sujet ici mais bon à savoir : le Doram monte à **15**, et les 4e classes à 6-7.)

**Onglets fusionnés + séparateur.** « 1re classe » et « 2e classe » partagent un onglet ; chaque arbre
gardant ses propres index de case (ils viennent du Lua), le second est décalé d'un nombre entier de
lignes et un **trait rouge titré** marque la frontière, avec sa propre bande de 22 px (sans quoi le
trait mordrait sur les icônes). 3e classe, 4e classe et « divers » restent séparés.

**Lisibilité du niveau.** Le « n / max » sous l'icône est écrit en **noir** dès que la compétence est
apprise (le vert « niveau max » se noyait dans le fond clair du skin), gris sinon, ambre foncé si un
point est réservé — et toujours avec un **liseré blanc de 1 px** (4 passes décalées), parce que le
texte déborde parfois sur le bas d'une icône.

**La même grille sert aux compétences de GUILDE** (onglet Guilde > Compétences), sans onglets de
classe. Différence de fond : elles n'ont **pas d'index de case** — le Lua du client ne les connaît
pas — donc la grille est construite depuis l'arbre lui-même : **une ligne par palier de profondeur**,
les compétences d'un même palier côte à côte. C'est la disposition que l'indentation de la liste
dessinait déjà, en deux dimensions. Autre différence, de sécurité : **Ctrl est EXIGÉ** pour monter
(`Ctrl + clic gauche`), parce qu'une compétence de guilde n'a pas de mécanisme de réservation — le
paquet part et le point est dépensé, alors que dans le Grimoire un clic ne fait que réserver.

**Lissage des icônes (opt-in).** Les .bmp d'icônes font 24 px et sont agrandis à 40 px : le natif ne
filtre rien, on garde ce défaut. Case « Lisser les icônes » en haut de page (yaml
`charsheet_grimoire_bilinear`), qui pose un `ImDrawList::AddCallback` -> `Overlay_SetTextureFilter(...)`
avant la grille — même schéma que `skill_bar_tweaks`. La case n'apparaît **qu'en vue grille** (en
liste l'icône fait une hauteur de ligne, le filtre ne s'y voit pas) et sert aux deux onglets.
⚠ Le callback est posé **dans les deux cas**, pas seulement quand le lissage est demandé : l'état
ambiant d'une draw list ImGui est **LINEAR** (le backend DX9 le remet dans `SetupRenderState`,
imgui_impl_dx9.cpp:139). Ne rien faire = icônes déjà lissées, et la case à cocher semble morte —
c'est le mode NET qui doit être imposé. Même raison pour la restauration : un
`ImDrawCallback_ResetRenderState` rendrait la main en LINEAR et ramollirait les blits de skin suivants.
