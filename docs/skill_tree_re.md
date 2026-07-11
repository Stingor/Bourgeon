# Skill tree / grimoire — reverse engineering + the FPS "over-render" bug

> Ragnarok client (`Moonlight-Destiny.exe`). All addresses are VAs of the loaded image
> (Ghidra == x32dbg, image base `0x00400000`, no ASLR). RE done 2026-07-11 (Ghidra, read-only —
> attaching x32dbg *normally* crashes this client due to anti-debug; validated in-game by
> behavior). Decompiler output cited for every claim.

This document covers the skill window ("grimoire"), the fact that **"legacy view" and "modern view"
are two MODES of a single window** (not two windows), and **why the "modern" mode tanks FPS** — an
**over-render** bug (immediate-mode geometry rebuilt every frame + an animated avatar that forces a
continuous repaint loop).

---

## 0. TL;DR

- The "Skill" button / hotkey **always** opens `UINewSkillListWnd` (window id **`0x25`**). There is
  **no** runtime choice of window class.
- "legacy view" vs "modern view" = **internal display mode**, driven by the option
  **`Simplicity_SkillList`** = `DAT_015fa454` (`0x015FA454`, default `0`). The toggle button lives
  **inside** the window (`OnMsg` case `0xCA`).
- **`Simplicity_SkillList == 0`** (default) → **icon grid 560×400 + ANIMATED class avatar** =
  "modern", **heavy**.
- **`Simplicity_SkillList != 0`** → **compact ~330 px list, no avatar, no grid background** =
  "legacy", **light**.
- **Root cause of the FPS bug**: the grid mode renders in **immediate mode** (all geometry thrown away
  and rebuilt every paint) **and** draws an **animated `.act` sprite** (the avatar) that **forces a
  continuous repaint** → the whole heavy `DrawContent` (42 grid quads + an **O(n²)** per-node loop) is
  re-run **every frame, in a loop**. The compact mode has no avatar → it only repaints on interaction.

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
      + FUN_00974700  ANIMATED CLASS AVATAR  ◄── FPS culprit      (no avatar, no grid)
      → CONTINUOUS repaint (animation)                            → repaint on interaction only
```

| | **"modern view"** (default) | **"legacy view"** |
|---|---|---|
| `Simplicity_SkillList` (`DAT_015fa454`) | `0` | `!= 0` |
| window size | 560 × 400 | ~330 px compact |
| render body | `FUN_00975730` (grid) **+ `FUN_00974700` animated avatar** | `FUN_009750d0` (list, no avatar/grid) |
| cost / frame | grid 42 blits + per-node O(n²) + **animated sprite + Lua/frame** | per-node O(n²) only |
| repaint | **continuous** (avatar animation forces it) | on interaction only |

> ⚠️ There is **also** a genuine, separate legacy class `UISkillListWnd` (id **`0x105`**), but **no
> menu/hotkey path opens it** in this client (superseded — see §7). Do not confuse it with the "legacy"
> *mode* above.

---

## 2. ⭐ The FPS "over-render" bug (the core)

### 2.1 Context — immediate mode, nothing retained

`DrawContent` (vtable `+0x50`) = **`0x00977E80`**. On **every paint** it starts with:

```c
// FUN_00977e80 (excerpt)
FUN_00a1cb30(param_1, 0xffff00ff);   // FULL reset of the window's render-node list (this+0x24 -> method +0x2c)
```

`FUN_00a1cb30` clears the window's entire render-node list. The base `UIWindow_OnDraw_Base 0x00A245C0`
even **recreates** the render node (`this+0x24`) on each `OnDraw`. So **no geometry is retained across
frames**: everything is re-emitted in full on each paint. This is immediate mode.

Then the body is picked based on the option:

```c
if (DAT_015fa454 == 0) {          // "modern": grid
    ... SP counter -> std::string alloc/frame ...
    FUN_00975730(param_1);        // grid body (42 cells + per-node)
    FUN_00974700();               // ANIMATED CLASS AVATAR  ◄── grid mode only
} else {                          // "legacy": compact list
    ... header FUN_00a9ed30(0x11c) ...
    FUN_009750d0(param_1);        // list body (per-node, no avatar, no grid)
}
*(uint8_t*)(param_1 + 0x271) = 0; // clear dirty
```

### 2.2 Cost SHARED by both modes — the O(n²) per-node loop

Both `FUN_00975730` (grid) **and** `FUN_009750d0` (list) do, **for each skill node** of the active tab
(linked list at `this + tab*8 + 0x278`):

1. `FUN_00d5e3a0(id, buf)` → **`sprintf`** the icon path (stack buffer).
2. `UITexture_MakeKeyFromPath 0x00A9F030` → **NOT a cheap hash**: ~4–5 `std::string` allocations + path
   normalization + service-type prefix handling.
3. `UITextureMgr_Load 0x00A8D4A0` → **`EnterCriticalSection`** + normalization + extension parse +
   several allocs + cache lookup. (Textures **are** cached → no disk I/O on repeat, but the **per-call
   CPU cost is still large**.) Up to **3 loads/node** (icon + fallback `0x01039364` + "learned" overlay
   `0x0103F937` in grid mode).
4. **`GetSkillInfoAt 0x00976230`** (the worst offender):
   ```c
   // init a temp SkillInfo, THEN linearly walk the whole tab list:
   FUN_00739700(param_1);
   piVar2 = FUN_00976590(this, this->tab, ...);      // list head
   do { ... if (node->gridIndex == wanted) FUN_008da950(out, node+2);  // FULL RECORD COPY (alloc)
        node = node->next; } while (...);
   // caller then FUN_00739cd0(temp) -> free
   ```
   → **O(n) walk + one alloc/copy/free of the record PER node** → across the whole draw loop this is
   **O(n²) + N heap allocations** per frame.
5. Several DB lookups (`DAT_015fa3cc`) + `UIText_MeasureWidth` + colored text draws (`FUN_00a26e30` /
   `UIWindow_DrawText` / `FUN_00a240e0`) for the "cur/max level" and the skill name.

For a 4th-class character (~40+ skills/tab), that alone is **thousands of ops + hundreds of heap
allocations + dozens of critical sections per frame**, just for the per-node loop.

### 2.3 Cost EXCLUSIVE to grid mode — why it loses "so much" FPS

On top of the loop above, grid mode (`DAT_015fa454 == 0`) adds:

**(a) 42 grid-background quads**, re-emitted every frame:
```c
// FUN_00975730 (start)
piVar5 = UITextureMgr_Load(...);       // grid-cell texture (1 load, fine)
iVar12 = 0;
do { UIWindow_BlitImageToNode(param_1, col*0x44+0x4c, row*0x38+0x2c, piVar5, 1);
     iVar12++; } while (iVar12 < 0x2a);  // 0x2a = 42 unconditional blits
```

**(b) THE ANIMATED CLASS AVATAR — `FUN_00974700`** (the real culprit). Called **only** in grid mode, it
renders the character (job) sprite that **plays its animation**:
```c
// FUN_00974700 (summary):
Job_GetBodySpritePath(...)   -> std::string  (alloc)   \
Job_GetHeadSpritePath(...)   -> std::string  (alloc)    |  several body/head/palette paths,
Job_GetBodyPalettePath(...)  -> std::string  (alloc)    /  one alloc/free each, EVERY frame
UITextureMgr_Load(...) x several             // sprites + palettes
Act_GetFrameCount(spr, 0x17); timeGetTime(); // ── .act animation:
frame = (now - this[0x274]) / frameDur;      //    advances the anim timer (this+0x274)
Act_GetFrame(...); Act_GetFrameLayer(...);   //    layer composition
SpriteAtlas_GetCachedTexture(...) / SpriteAtlas_BuildTexture(...)
Lua_CallGlobal_va(g_UILuaState, "OffsetItemPos_GetOffsetForDoram");  // ── 1 Lua call PER frame
```

An **animated sprite forces a continuous repaint**: for the avatar to "breathe", the window must
repaint every frame. But repainting = re-running **all** of the heavy `DrawContent` (§2.1 + §2.2 +
§2.3a). Grid mode is therefore in **permanent over-render**: it re-pays the O(n²) + avatar + Lua cost
**every frame, in a loop, as long as the window is open** — even when idle.

The compact mode (`!= 0`) does **not** call `FUN_00974700` and does **not** blit the 42 cells → no
animation → **no continuous repaint** → near-zero cost at idle. Hence the massive FPS gap.

### 2.4 One-sentence diagnosis

> The "modern" mode of the skill window is **immediate-mode with no caching**, whose paint cost is
> **O(n²) + an animated sprite avatar + one Lua call**, and an **animated avatar forces it to repaint
> every frame** → over-render → massive FPS drop. The "legacy" mode removes the avatar and grid, hence
> the continuous repaint, hence the bug.

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
| `+0x24C` | anchor/parent |
| `+0x250` | hovered index |
| `+0x254` | **active tab** |
| `+0x25C` | grid columns |
| `+0x260` | rows/page |
| `+0x264`/`+0x268` | top/bottom insets |
| `+0x26C` | "has skill points" flag |
| `+0x271` | **dirty** (set to 1 by input handlers, cleared at end of `DrawContent`) |
| `+0x274` | **avatar animation timer** (`timeGetTime`) |
| `+0x278 + tab*8` | **4 skill linked lists** (one per tab) |
| `+0x298` | hover list |
| `+0x2A0` | reserved-levels list |

Skill node record (in the linked list): `+0x04` id, `+0x06` max-lvl, `+0x07` learned-lvl, `+0x0B`
grid-index, `+0x10` icon-id, `+0x00` next.

Grid (modern mode): cell `0x44 × 0x38`, origin `(0x4C, 0x2C)`, **42 cells** (`0x2A`).

### 3.3 `OnMsg 0x00979270` — useful cases

| msg / id | effect |
|---|---|
| **6 / `0xCA`** (@`0x00979520`) | **THE view toggle**: `DAT_015fa454 = (DAT_015fa454 == 0)` then resize `0x14a=330` (compact) or `0x230=560`×400 (grid) |
| 6 / `0xD5` (@`0x0097956F`) | `DAT_015fa458 = (==0)` = **Show_SkillDescript** (secondary option) |
| 6 / `0xC9` | `SaveWindowRect(0x25)` (self-close) |
| 6 / `0xD8` | cycle tab |
| 6 / `0x16B` | reset + refresh |
| 6 / `0x10F` | confirm popup (reset all) |
| 6 / `0xEF..0xF8` | direct level-up of a slot (if `this+0x26C`) |
| 6 / default | +/- level (SkillMgr_SetOption 7) |
| `0x0E` | resize (clamps height ≤ 400 in grid mode) |
| `0x16` | change active tab (`this+0x254`) |
| **`0x17`** | rebuild (FUN_00976b60 + FUN_00737ce0(DB) + relayout + repaint) |
| **`0x3C`** | job-change (FUN_009765f0) |
| `0x22` | (re)anchor to parent (`+0x24C`) + initial resize |
| `0x3A` | open desc (dispatcher cmd `0x71`) |
| `0xA1` | re-fit tab |

---

## 4. The toggle & the option (config)

### 4.1 Opening (always `0x25`)

- "Skill" menu icon = **command `0xC4`** (`UIMenuIconWnd_BuildIconList 0x00812FB0` ; click `0x008271F0`
  → `g_UICommandDispatcher` obj `0x0121333C`, vtable `+0x18` = `0x00C86740` `CGameMode::SendMsg`).
- Skill hotkey = `UIWindowMgr_DispatchHotkeyBehavior 0x00A457E4` **case `0x66`** → menu `OnMsg(6, 0xC4)`.
- The menu dispatcher **`FUN_00814A70`** maps `0xC4` → **`FUN_00812E60(0x25)`**:
  ```c
  bool FUN_00812e60(id) {                       // generic toggle helper
      bool wasOpen = UIWindowMgr_SaveWindowRect(&g_UIWindowMgr, id);  // true if already open -> close
      if (!wasOpen) UIWindowMgr_MakeWindow(&g_UIWindowMgr, id);       // else open
      return !wasOpen;
  }
  ```
  **No flag test; `0x105` is never referenced.** → always the "modern" `0x25`.

### 4.2 The `Simplicity_SkillList` option

- `DAT_015fa454` (`0x015FA454`) = field `+0x94` of the OptionInfo/CSession singleton at base `0x015FA3C0`.
- Loaded (**default `0`**) in `OptionInfo_LoadAndApplyAll 0x00D759F0` (read `@0x00D75B47`, writes
  `[singleton+0x94]` `@0x00D75B55`).
- Saved in `OptionInfo_SaveToFile 0x00D78970` (`@0x00D78D38`).
- Secondary `Show_SkillDescript` = `DAT_015fa458` (`+0x98`, default `0`, button `0xD5`).
- Outside the skill window, the only reader of `DAT_015fa454` is the mgr relayout `FUN_00A46380`
  (`@0x00A47044`), which merely redraws the cached window (`mgr+0x2C4`) when the flag is 0 — it never
  selects a window class.

---

## 5. Populating the tree (data side) — event-driven, not per-frame

- **`FUN_00D70D60`**: Lua `GetInheritJob` + `InitSkillTreeView` loop (`FUN_00D96790`) → sends msg
  **`0x3C`** to the window (`mgr+0x2C4`) + relayouts basic-info. Triggered by **`FUN_00DA8ED0`** (job
  change, writes `this+0x1608`).
- **`FUN_00D95E60`**: sends msg **`0x17`** (refresh) to the window + `DAT_0131f6d0` + `FindWindow(0x9F)`.
  Called by ~10 skill events (learned / leveled up).
- Lua modules `Lua Files\SkillInfoz\skilltreeview` + `Lua Files\cls\skilltreeview` loaded by
  `FUN_0171E200` (static init); Lua function `InitSkillTreeView` (string `0x0109BCC0`).

> So the **data** rebuild is **event-driven** (job/skill change) → **NOT** the cause of the FPS drop.
> The cause is 100% in the **drawing** (§2).

---

## 6. Fix ideas (not implemented)

| # | Fix | Effort | Effect |
|---|---|---|---|
| A | **Force `Simplicity_SkillList = 1`** by default (compact mode) | trivial | removes avatar + grid + continuous repaint; compact UI |
| B | **Decouple the avatar animation from the repaint**: only render `FUN_00974700` when the window is already dirty; don't invalidate *for* the avatar | medium | keeps modern grid but kills the idle over-render |
| C | **Gate the rebuild** on the dirty flag (`this+0x271`) + cache node geometry across frames | medium/high | removes the per-frame rebuild; needs confirming whether the mgr calls `DrawContent` unconditionally or on invalidation |
| D | **Hoist `GetSkillInfoAt` out of the draw loop** (max-lvl is already in the node, `+6`) and **hoist** `MakeKeyFromPath`/`Load` (same keys every frame) | medium | removes the O(n²) + redundant allocs/lookups; **benefits BOTH modes** |
| E | (heavy) Redirect the skill open to the old `UISkillListWnd 0x105` (retained widgets, near-free) | high | very different UI (link list), evaluate first |

> Recommended: **B + D** (keep the modern look, kill the over-render), or **A** as a player-side
> quick-win. **D** is an unconditional net win.
> Confirm live before B/C: add a counter/log inside `DrawContent 0x00977E80` to verify it is called
> every frame while the window is open and idle (the animated avatar strongly implies it is).

---

## 7. The genuine old `UISkillListWnd` (id `0x105`) — superseded

A real, separate class, but **no menu/hotkey path opens it** in this client.

- RTTI `.?AVUISkillListWnd@@` @ `0x01240678` ; vtable **`0x0103F3EC`**.
- ctor **`0x00970D30`** (object `0xD4`), called **only** by the factory case `0x105`
  (`@0x00A40127`: `PUSH 0x105 … FindWindow … CALL 0x00970D30`).
- OnMsg **`0x00971560`** ; DrawContent **`0x00971120`** ; RebuildList **`0x00971A20`** (a vertical list
  of cached `UIItemLinkBtn`, `y = 0xE6 + i*0x14`, built **once**).
- Registry key `SKILLLISTWNDINFO.*` (`@0x0104BB04`).
- Remaining role: skill **reordering** (packet **`0x9FB`** via `OnMsg` case 6 / id `0xB8`) and opening
  the desc (case `0x62` → `MakeWindow(0xC)` + `OnMsg 0x18`).
- **Retained** architecture (widgets built once, `DrawContent` nearly empty) → a good reference model if
  a lightweight grimoire were ever wanted.
- Live check: bp on `0x00970D30` while opening the grimoire → **should never hit**.

---

## 8. Gotchas / conventions

- `vtable+0x94` = `OnMsg` (`__thiscall`, 6 stack args) ; `+0x50` = `DrawContent` ; `+0x3C` = `OnCreate`.
- Don't confuse the ids: skill/item **desc** = `0xC` ; **action bar** = `0x24` ; **grimoire** (the real
  window) = **`0x25`** ; **old** grimoire = `0x105`.
- The "window instance" globals `DAT_0131F6xx`/`DAT_0131F7xx` are **fields of the window manager**
  `0x0131F4E8` (skill window = `mgr+0x2C4`), written **indirectly** by the factory (no direct write
  xref).
- `DAT_015fa454` = **`Simplicity_SkillList`** (compact mode), **NOT** a "reservation/sim" mode (initial
  RE mistake, corrected). `DAT_015fa3c0` = OptionInfo/CSession base ; `DAT_015fa3cc` = skill-tree DB map.
- Textures: `UITextureMgr_Load 0x00A8D4A0` (key-cached, `EnterCriticalSection`), key built by
  `UITexture_MakeKeyFromPath 0x00A9F030` (several `std::string` allocs, expensive).

---

### Address cheat-sheet

| Symbol | VA |
|---|---|
| Skill window `UINewSkillListWnd` id | `0x25` |
| vtable | `0x0103F660` |
| ctor | `0x00974060` (size `0x2B0`) |
| cached instance | `0x0131F7AC` (`mgr+0x2C4`, mgr `0x0131F4E8`) |
| **DrawContent (hot path)** | **`0x00977E80`** |
| grid body (modern) | `0x00975730` |
| **animated avatar (FPS culprit)** | **`0x00974700`** |
| list body (legacy/compact) | `0x009750D0` |
| `GetSkillInfoAt` (O(n)+copy) | `0x00976230` |
| render-node reset | `0x00A1CB30` |
| `OnMsg` (+ toggle case 0xCA @0x00979520) | `0x00979270` |
| OnCreate | `0x00976D70` |
| option `Simplicity_SkillList` | `0x015FA454` (default 0) |
| option `Show_SkillDescript` | `0x015FA458` |
| OptionInfo Load / Save | `0x00D759F0` / `0x00D78970` |
| open: menu cmd `0xC4` → | `FUN_00814A70` → `FUN_00812E60(0x25)` (`0x00812E60`) |
| hotkey dispatch (case 0x66) | `0x00A457E4` |
| populate (job-change / refresh) | `0x00D70D60` / `0x00D95E60` |
| `UITextureMgr_Load` / `MakeKeyFromPath` | `0x00A8D4A0` / `0x00A9F030` |
| Old `UISkillListWnd` id / vtable / ctor / OnMsg | `0x105` / `0x0103F3EC` / `0x00970D30` / `0x00971560` |
