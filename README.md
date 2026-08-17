Bourgeon
========

A **hard fork** of [L1nkZ/Bourgeon](https://github.com/L1nkZ/Bourgeon). Upstream is a
clean C++17 framework for writing plugins for Ragnarok Online clients. This fork kept
the name, the `ddraw.dll` proxy trick and the MIT licence — then grew into something
else entirely: the client half of one specific private server, **Moonlight Destiny**.

If you came here looking for a general-purpose RO plugin library, go to upstream.
This tree will not really help you.

*Note: still a work in progress, and always will be.*

What it actually is now
-----------------------

- **~143 000 lines of C++17, 93 modules, one DLL.** There is no runtime plugin
  loading and no scripting layer: every module is compiled in and registered
  statically in `Bourgeon::LoadPlugins()`. The old flat `src/plugins/` folder is
  gone — sources are now grouped by *what the module does to the native client*
  (see `docs/source_layout.md`).
- **667 hardcoded native addresses across 877 call sites**, resolved against exactly
  one binary: Ragexe **20250716**, image base `0x400000` (`docs/address_manifest.md`).
- **Most of it is not "tweaks".** Whole native windows are hidden and rewritten in
  ImGui — storage, cart, trade, RODEX, NPC shop and dialogue, vending, bank, crafting,
  refine, char-select, the Escape menu and both settings screens, pet info, monster
  info, navigation, cash shop, achievements. Several of those natives are simply
  **dead** in this client and had to be replaced rather than repaired.
- **46 reverse-engineering documents** in `docs/`, which are the real value of this
  repository. They are written in French.

Why you cannot run it standalone
--------------------------------

This is the part the old README got wrong. The coupling to the Moonlight server is
structural, not cosmetic:

| | |
|---|---|
| **Login** | `MoonlightAuth` authenticates against a phpBB web account over HTTPS, receives the list of linked RO accounts, and has the site mint a one-shot OTP before delegating to the native login sequence. With no endpoint configured, the modern login screen is inert. |
| **Protocol** | ~40 custom opcodes in the `0x0F00+` range (`CZ_BOURGEON_*` / `ZC_BOURGEON_*`, see `src/features/systems/bourgeon_opcodes.h`). They must be mirrored in the server's `packets_struct.hpp`, `clif_packetdb.hpp` and handlers. Client and server ship together or not at all. |
| **Integrity** | `IntegrityCheck` sends this DLL's SHA-256, the Windows MachineGuid and the rpatchur patch level on map entry. Enforcement — kick, admin report, dev bypass — lives entirely server-side. |
| **Content** | Crafting recipes, refine rates and weapon levels are read from `SystemEN\bourgeon_recipes.yaml`, generated from the server database. Several gameplay fixes this client assumes live on the rAthena side, not here. |
| **Binary** | The target exe is a WARP-patched `Moonlight-Destiny.exe` — **110 active patches**. Some modules depend on the patched behaviour (data-folder-first loading, raised inventory limit, body palette tables). A vanilla Ragexe will not behave the same, and the IDA database of the vanilla exe does *not* show any of it (`docs/warp_patches.md`). |

In short: "getting Bourgeon to run" means either playing on Moonlight, or forking and
rebuilding an entire rAthena server next to it.

About the code
--------------

I (Stingor) am not a developer. This was written almost entirely by Claude, driven
through Ghidra MCP, IDA MCP and x32dbg. It is what people call vibe coding — at
143k lines, with the failure modes you would expect from that. It works, on one
client, against one server. Comments and documentation are in French.

Layout
------

```
src/
  bourgeon.{h,cc}   module registry, event dispatch, feature gates
  main.cc           DLL entry point
  features/         everything that adds or changes something for the player
    windows/          replaces a native window (native hidden, ImGui takes its place)
    patches/          retouches the native in place (hook, vtable swap, byte patch)
    overlays/         draws on top of the game, replaces nothing
    gameplay/         input and camera
    fx/               sprite, effect and final-image rendering
    systems/          cross-cutting services (net, security, session, meta)
    minigames/        embedded games (DOOM, Roggle, Rojeweled)
  ui/               "RO-styled" ImGui toolkit (skin, widgets, textures, z-order)
  ragnarok/         binding to the native client (session, modes, packets, layouts)
  utils/            game-agnostic tooling (hooking, logs, paths, GIF)
  ddraw/ d3d9/ imgui/   the three overlay rendering backends (DX7 and DX9 paths)
thirdparty/         imgui, spdlog, yaml-cpp, freetype, doomgeneric, json
tools/lang/         translation catalogs (en, es) + the 4347 client msgstring entries
docs/               reverse-engineering notes — read `source_layout.md` first
```

One dependency rule: **`ui/` never knows about `features/`.**

Requirements
------------

- Windows, **32-bit build only** (the RO client is x86; CMake refuses x64).
- Visual Studio 2022 or newer with the C++ toolchain, CMake ≥ 3.15.
- The 20250716 client, WARP-patched. Anything else and the addresses are wrong.

Build
-----

```
git clone --recursive https://github.com/Stingor/Bourgeon.git
cd Bourgeon
cmake -S . -B build -A Win32
cmake --build build --config Release
```

`build_configure.bat` is the local shortcut (NMake generator, Release).

The build emits `ddraw.dll` plus a `ddraw.dll.sha256` sidecar — that hash goes into
the server's `conf/bourgeon_integrity.conf`. Setting `-DRO_DEPLOY_DIR=<game folder>`
copies the DLL and the language catalogs straight into the client.

To install by hand: drop `ddraw.dll` next to the client executable and launch the game.

Credits
-------

- **L1nkZ** — the original Bourgeon, whose framework and proxy still carry all of this. /bow
- **CrazyBebop and the WARP team** — the patcher this client is built on.
- MIT licence, inherited from upstream.
