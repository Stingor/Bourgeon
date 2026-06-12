import bourgeon
from ragnarok_client import Mode
import ctypes
import struct

G_SESSION = 0x015fa3c0
_in_game = False
_scanned = False

def on_mode_switch(mode, _map):
    global _in_game, _scanned
    _in_game = (mode == Mode.Game)
    _scanned = False

def on_tick():
    global _scanned
    if not _in_game or _scanned:
        return
    _scanned = True

    try:
        block = ctypes.string_at(G_SESSION, 0x10000)
    except Exception as e:
        bourgeon.log(f"read failed: {e}")
        return

    # Search for stat values 111, 222 adjacent (STR, AGI, ...)
    bourgeon.log("=== searching for stats (111/222/333) ===")
    for off in range(0, len(block) - 8, 4):
        v0 = struct.unpack_from("<i", block, off)[0]
        if v0 == 111:
            v1 = struct.unpack_from("<i", block, off + 4)[0]
            if v1 == 222:
                bourgeon.log(f"found at +{hex(off)}:")
                for i in range(-4, 12):
                    o = off + i * 4
                    if 0 <= o < len(block):
                        v = struct.unpack_from("<i", block, o)[0]
                        if v != 0:
                            marker = " <--" if 0 <= i <= 5 else ""
                            bourgeon.log(f"  [+{hex(o)}]={v}{marker}")
                break
    else:
        bourgeon.log("111/222 pair not found")

    # HP/SP area
    bourgeon.log("=== 0x5540-0x55C0 ===")
    for off in range(0x5540, 0x55C0, 4):
        v = struct.unpack_from("<i", block, off)[0]
        if v != 0:
            bourgeon.log(f"[+{hex(off)}]={v}")

bourgeon.register_callback("OnModeSwitch", on_mode_switch)
bourgeon.register_callback("OnTick", on_tick)
