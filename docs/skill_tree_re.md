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
