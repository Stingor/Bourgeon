import sys
sys.path.insert(0, './plugins/config')
try:
    from discord_config import BOT_TOKEN, GUILD_NAME, CHANNELS_TO_WATCH, WEBHOOK_URL
except ImportError as e:
    raise ImportError(f"discord_config.py missing or incomplete: {e}. Required: BOT_TOKEN, GUILD_NAME, CHANNELS_TO_WATCH, WEBHOOK_URL")
try:
    from discord_config import CHAR_NAME as _CHAR_NAME_FALLBACK
except ImportError:
    _CHAR_NAME_FALLBACK = ""

import bourgeon
import ragnarok_client as ro_client
from ragnarok_client import Mode

import threading
import queue
import json
import http.client
import urllib.parse

API_HOST = "discord.com"
DISCORD_COLOR = 0x7289DA

_msg_queue = queue.Queue()
_watched_channels = []
_cursors = {}
_in_game = False
_init_done = False
_init_started = False


def _api_get(path):
    try:
        conn = http.client.HTTPSConnection(API_HOST, timeout=5)
        conn.request("GET", "/api/v10" + path,
                     headers={"Authorization": f"Bot {BOT_TOKEN}"})
        r = conn.getresponse()
        data = r.read()
        conn.close()
        if r.status == 200:
            return json.loads(data)
        bourgeon.log(f"Discord API {r.status}: {data[:120]}")
        return None
    except Exception as e:
        bourgeon.log(f"Discord API exception: {e}")
        return None


def _api_post(url, payload):
    try:
        parsed = urllib.parse.urlparse(url)
        body = json.dumps(payload).encode()
        conn = http.client.HTTPSConnection(parsed.netloc, timeout=5)
        conn.request("POST", parsed.path + ("?" + parsed.query if parsed.query else ""),
                     body=body,
                     headers={"Content-Type": "application/json",
                               "Content-Length": str(len(body))})
        conn.getresponse().read()
        conn.close()
    except Exception:
        pass


def _pull_messages():
    for ch in _watched_channels:
        cid = ch["id"]
        if cid not in _cursors:
            msgs = _api_get(f"/channels/{cid}/messages?limit=1") or []
            _cursors[cid] = msgs[0]["id"] if msgs else "0"
            bourgeon.log(f"Discord #{ch['name']} cursor set to {_cursors[cid]}")
            continue
        msgs = _api_get(f"/channels/{cid}/messages?after={_cursors[cid]}&limit=50")
        if msgs is None:
            bourgeon.log(f"Discord #{ch['name']} poll returned None")
            continue
        bourgeon.log(f"Discord #{ch['name']} poll: {len(msgs)} new msgs")
        for msg in reversed(msgs):
            _cursors[cid] = msg["id"]  # always advance past this message
            if msg.get("webhook_id"):
                continue  # skip our own webhook echoes
            content = msg.get("content", "").strip()
            if not content:
                continue
            display = (msg.get('member', {}).get('nick')
                       or msg['author'].get('global_name')
                       or msg['author']['username'])
            _msg_queue.put(f"(#{ch['name']}) {display}: {content}")
            bourgeon.log(f"Discord queued: {display}: {content[:30]}")


def _poll_loop():
    import time
    while True:
        if _in_game:
            _pull_messages()
        time.sleep(5)


def _do_init():
    global _init_done
    guilds = _api_get("/users/@me/guilds") or []
    guild = next((g for g in guilds if g["name"] == GUILD_NAME), None)
    if not guild:
        bourgeon.log(f"Discord: server '{GUILD_NAME}' not found")
        return
    channels = _api_get(f"/guilds/{guild['id']}/channels") or []
    found = [c for c in channels if c["type"] == 0 and c["name"] in CHANNELS_TO_WATCH]
    if not found:
        bourgeon.log("Discord: no matching channels found")
        return
    _watched_channels.extend(found)
    bourgeon.log("Discord watching: " + ", ".join(f"#{c['name']}" for c in found))
    threading.Thread(target=_poll_loop, daemon=True).start()
    _init_done = True


_tick_count = 0

def on_tick():
    global _init_started, _tick_count
    _tick_count += 1

    if not _init_started and _tick_count > 30:
        _init_started = True
        threading.Thread(target=_do_init, daemon=True).start()

    if not _init_done:
        return

    while not _msg_queue.empty():
        try:
            msg = _msg_queue.get_nowait()
            bourgeon.log(f"Discord display: {repr(msg[:50])}")
            ro_client.print_in_chat(msg, DISCORD_COLOR, 0)
        except queue.Empty:
            break
        except Exception as e:
            bourgeon.log(f"Discord print_in_chat error: {e}")


def on_mode_switch(mode, _map):
    global _in_game
    _in_game = (mode == Mode.Game)


_CHAT_PREFIXES = ('/', '@', '$', '%', '#', '!', '^')

def on_talk_type(chat_buffer: str):
    if not WEBHOOK_URL or not _in_game:
        return
    text = str(chat_buffer).strip()
    if not text or text.startswith(_CHAT_PREFIXES):
        return
    try:
        char = ro_client.get_char_name() or _CHAR_NAME_FALLBACK
    except Exception:
        char = _CHAR_NAME_FALLBACK
    threading.Thread(
        target=_api_post,
        args=(WEBHOOK_URL, {"username": char or "Unknown", "content": text}),
        daemon=True,
    ).start()


bourgeon.register_callback("OnTick", on_tick)
bourgeon.register_callback("OnModeSwitch", on_mode_switch)
if WEBHOOK_URL:
    bourgeon.register_callback("OnTalkType", on_talk_type)
