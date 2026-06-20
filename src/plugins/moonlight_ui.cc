#include "plugins/moonlight_ui.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "bourgeon.h"
#include "imgui.h"
#include "plugins/discord_relay.h"
#include "ragnarok/ui_window_mgr.h"
#include "spdlog/fmt/fmt.h"
#include "utils/byte_pattern.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"
#include "yaml-cpp/yaml.h"

// ── Item description window hook ──────────────────────────────────────────
// Hooks FUN_008c18b0 (__thiscall, 20250716 client) to capture the nameid of
// whichever item the player right-clicks to inspect.
//
// The function is a UI window message handler.  Message 0x18 means "set item":
//   param_3 is the item data struct; the item's nameid is stored as a
//   MSVC std::string (SSO layout) at byte offset 0x2C (= param_3[11]):
//     [0..3]  char* ptr  (or inline buf when small)
//     [4..7]  inline buf continued
//     [8..11] inline buf continued
//     [12]    _Mysize (string length)
//     [16]    _Myres  (capacity - 1)  <- checked against 15 to detect heap
//   If capacity-1 > 15 the string is heap-allocated and param_3[11] is a ptr;
//   otherwise the 16-byte inline buffer starts at param_3+0x2C.
//   atoi() on that text gives the numeric nameid.

using ItemDescWndFn = int (__fastcall*)(void*, void*, uint32_t, int, int*, int, int, int);
static ItemDescWndFn g_item_desc_wnd_orig  = nullptr;
static uint32_t      g_last_viewed_item    = 0;
static POINT         g_item_desc_cursor    = {0, 0};
static bool          g_item_desc_visible   = false;  // set on 0x18, cleared on close msg
static void*         g_item_desc_wnd_ptr   = nullptr; // ecx of the desc window object

static int __fastcall ItemDescWndHook(void* ecx, void* /*edx*/,
                                       uint32_t p1, int p2, int* p3,
                                       int p4, int p5, int p6) {
  if (p2 == 0x18 && p3 != nullptr) {
    // Ignore 0x18 from secondary windows (e.g. equipment comparison window).
    // Lock onto the first ecx that sends 0x18 while no tooltip is open.
    if (g_item_desc_visible && ecx != g_item_desc_wnd_ptr)
      return g_item_desc_wnd_orig(ecx, nullptr, p1, p2, p3, p4, p5, p6);

    const int* sso = p3 + 11;  // std::string at byte offset 0x2C
    const char* str;
    if (static_cast<uint32_t>(sso[5]) > 15u)
      str = *reinterpret_cast<const char* const*>(sso);
    else
      str = reinterpret_cast<const char*>(sso);
    if (str) {
      const long id = std::atol(str);
      if (id > 0) {
        if (g_item_desc_visible && static_cast<uint32_t>(id) == g_last_viewed_item) {
          LogInfo("[ItemDescWnd] same-item 0x18: id={} ecx=0x{:08X} -> toggle-close",
                  id, reinterpret_cast<uint32_t>(ecx));
          g_item_desc_visible = false;
          g_item_desc_wnd_ptr = nullptr;
        } else {
          g_last_viewed_item  = static_cast<uint32_t>(id);
          g_item_desc_visible = true;
          g_item_desc_wnd_ptr = ecx;
          GetCursorPos(&g_item_desc_cursor);
        }
      }
    }
  } else if (p2 == 0x06 && ecx == g_item_desc_wnd_ptr) {
    // X button close — also reset ptr for the same reason.
    g_item_desc_visible = false;
    g_item_desc_wnd_ptr = nullptr;
  }
  return g_item_desc_wnd_orig(ecx, nullptr, p1, p2, p3, p4, p5, p6);
}

// Returns the path to bourgeon_settings.yaml next to the game executable.
static std::string GetSettingsPath() {
  char buf[MAX_PATH];
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string path(buf);
  const auto sep = path.find_last_of("\\/");
  if (sep != std::string::npos) path.resize(sep + 1);
  return path + "bourgeon_settings.yaml";
}

void MoonlightUi::LoadItemNames() {
  char buf[MAX_PATH];
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string base(buf);
  const auto sep = base.find_last_of("\\/");
  if (sep != std::string::npos) base.resize(sep + 1);

  // Try common RO client layouts in order.
  static const char* kCandidates[] = {
    "System\\itemInfoMerged.lua",
    "SystemEN\\itemInfoMerged.lua",
    "System\\itemInfo.lua",
    "SystemEN\\itemInfo.lua",
  };
  std::ifstream f;
  std::string path;
  for (const char* cand : kCandidates) {
    path = base + cand;
    f.open(path);
    if (f) break;
    f.clear();
  }
  if (!f) {
    LogError("[MoonlightUi] itemInfoMerged.lua not found (tried System\\ and SystemEN\\)");
    return;
  }
  LogInfo("[MoonlightUi] loading item names from {}", path);

  uint32_t current_id = 0;
  std::string line;
  while (std::getline(f, line)) {
    // Match item ID line: \t[501] = {
    // Only treat as item ID when the bracket content is purely numeric.
    // Lines like  identifiedDisplayName = "Horn Card [Shield]",  contain
    // non-numeric brackets and must fall through to the Name check below.
    const auto lb = line.find('[');
    if (lb != std::string::npos) {
      const auto rb = line.find(']', lb + 1);
      if (rb != std::string::npos) {
        const auto between = line.substr(lb + 1, rb - lb - 1);
        const bool all_digits = !between.empty() &&
            std::all_of(between.begin(), between.end(),
                        [](unsigned char c){ return std::isdigit(c) != 0; });
        if (all_digits) {
          try {
            current_id = static_cast<uint32_t>(std::stoul(between));
          } catch (...) { current_id = 0; }
          continue;  // item ID line fully consumed
        }
        // Non-numeric bracket (e.g. "[Shield]"): fall through to Name check.
      }
    }
    // Match: Name = "Red Potion"  (compact format used by this client)
    // Also handles legacy: identifiedDisplayName = "Sleipnir",
    if (current_id > 0 &&
        (line.find("Name =") != std::string::npos)) {
      const auto q1 = line.find('"');
      const auto q2 = line.rfind('"');  // rfind: last quote handles names with "
      if (q1 != std::string::npos && q2 != std::string::npos && q1 < q2)
        item_names_[current_id] = line.substr(q1 + 1, q2 - q1 - 1);
    }
  }
  LogInfo("[MoonlightUi] loaded {} item names", item_names_.size());
}

MoonlightUi::MoonlightUi() {
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodeFromServer);
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodePresetList);
  // Observe the standard map-move packet to learn the current map name.
  Bourgeon::Instance().RegisterObserveOpcode(kOpcodeMapMove, kMapNameLen);
  FindChatBgInstruction();
  LoadItemNames();

  // Hook the item description window to capture the nameid when right-clicking.
  g_item_desc_wnd_orig = reinterpret_cast<ItemDescWndFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kItemDescWndAddr),
          reinterpret_cast<uint8_t*>(ItemDescWndHook)));
  if (!g_item_desc_wnd_orig) {
    LogError("[MoonlightUi] failed to hook item desc wnd at 0x{:08X}",
             kItemDescWndAddr);
  }
}

// ── Chat background color ─────────────────────────────────────────────────

void MoonlightUi::FindChatBgInstruction() {
  // Locate the .text section via the PE header of the main module.
  const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandle(nullptr));
  const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const auto* sec  = IMAGE_FIRST_SECTION(nt);

  uint8_t* text_start = nullptr;
  size_t   text_size  = 0;
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    if (std::memcmp(sec->Name, ".text", 5) == 0) {
      text_start = const_cast<uint8_t*>(base) + sec->VirtualAddress;
      text_size  = sec->Misc.VirtualSize;
      break;
    }
  }

  if (!text_start) {
    LogError("[MoonlightUi] chat_bg: .text section not found");
    return;
  }

  // Pattern: C7 ?? D8 00 00 00 ?? ?? ?? ??
  //          ^^^^^^^^^^^^^^^^^^^^  ─ MOV [reg+0xD8], imm32
  //                                ^─────────^ ─ the ARGB value we patch (+6)
  // Wildcards on the ModRM byte (any register) and the 4-byte ARGB immediate
  // so the pattern works whether or not a WARP binary patch was already applied.
  BytePattern pat(
      {0xC7, 0x00, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      "x?xxxx????"
  );

  auto* found = static_cast<uint8_t*>(pat.Search(text_start, text_size));
  if (!found) {
    LogError("[MoonlightUi] chat_bg: init instruction not found in .text");
    return;
  }

  // The 4-byte ARGB immediate starts 6 bytes into the instruction
  // (1 opcode + 1 ModRM + 4 displacement bytes).
  chat_bg_instr_ = reinterpret_cast<uint32_t*>(found + 6);

  // Make the immediate field writable (one VirtualProtect, never restored).
  DWORD old_protect;
  VirtualProtect(chat_bg_instr_, sizeof(uint32_t),
                 PAGE_EXECUTE_READWRITE, &old_protect);

  // Seed the ImGui picker from whatever color is currently in the instruction
  // (works with the original 0x66000000 or a WARP-patched value).
  const uint32_t argb = *chat_bg_instr_;
  chat_bg_color_[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f; // R
  chat_bg_color_[1] = static_cast<float>((argb >>  8) & 0xFF) / 255.0f; // G
  chat_bg_color_[2] = static_cast<float>( argb        & 0xFF) / 255.0f; // B
  chat_bg_color_[3] = static_cast<float>((argb >> 24) & 0xFF) / 255.0f; // A

  LogInfo("[MoonlightUi] chat_bg: instruction at VA 0x{:08X}, initial color 0x{:08X}",
          reinterpret_cast<uint32_t>(found), argb);
}

void MoonlightUi::PatchInstruction(uint32_t argb) {
  if (!chat_bg_instr_) return;
  *chat_bg_instr_ = argb;
  FlushInstructionCache(GetCurrentProcess(), chat_bg_instr_, sizeof(uint32_t));
}

void MoonlightUi::PatchExistingObjects(uint32_t argb) {
  HANDLE heap = GetProcessHeap();
  if (!heap) return;
  if (!HeapLock(heap)) return;

  PROCESS_HEAP_ENTRY entry = {};
  int count = 0;
  while (HeapWalk(heap, &entry)) {
    if (!(entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
    if (entry.cbData < kChatBgColorOff + sizeof(uint32_t)) continue;

    // Chat window objects start with their vtable pointer.
    const auto* vtable_ptr = static_cast<const uint32_t*>(entry.lpData);
    if (*vtable_ptr != kChatWinVtable) continue;

    auto* color_field = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(entry.lpData) + kChatBgColorOff);
    *color_field = argb;
    ++count;
  }

  HeapUnlock(heap);
  LogInfo("[MoonlightUi] chat_bg: patched {} existing window object(s)", count);
}

uint32_t MoonlightUi::PickerToArgb() const {
  const uint32_t r = static_cast<uint32_t>(chat_bg_color_[0] * 255.0f + 0.5f) & 0xFF;
  const uint32_t g = static_cast<uint32_t>(chat_bg_color_[1] * 255.0f + 0.5f) & 0xFF;
  const uint32_t b = static_cast<uint32_t>(chat_bg_color_[2] * 255.0f + 0.5f) & 0xFF;
  const uint32_t a = static_cast<uint32_t>(chat_bg_color_[3] * 255.0f + 0.5f) & 0xFF;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

// ── Settings persistence ──────────────────────────────────────────────────

void MoonlightUi::LoadSettings() {
  const std::string path = GetSettingsPath();
  std::ifstream f(path);
  if (!f) return;  // first run — no file yet

  try {
    const YAML::Node root = YAML::Load(f);
    const YAML::Node ui = root["moonlight_ui"];
    if (!ui) return;

    const std::string hex = ui["chat_bg"].as<std::string>("");
    if (hex.size() == 8) {
      const uint32_t argb = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
      chat_bg_color_[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
      chat_bg_color_[1] = static_cast<float>((argb >>  8) & 0xFF) / 255.0f;
      chat_bg_color_[2] = static_cast<float>( argb        & 0xFF) / 255.0f;
      chat_bg_color_[3] = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
      if (chat_bg_instr_) {
        PatchInstruction(argb);
        PatchExistingObjects(argb);
      }
      LogInfo("[MoonlightUi] loaded chat_bg 0x{:08X}", argb);
    }

    ui_collapsed_         = ui["ui_collapsed"].as<bool>(false);
    show_alootid_overlay_ = ui["alootid_overlay"].as<bool>(false);
    apply_collapse_ = true;
  } catch (const std::exception& e) {
    LogError("[MoonlightUi] failed to parse {}: {}", path, e.what());
  }
}

void MoonlightUi::SaveSettings() {
  char hex[9];
  std::snprintf(hex, sizeof(hex), "%08X", PickerToArgb());

  YAML::Emitter out;
  out << YAML::BeginMap
      << YAML::Key << "moonlight_ui"
      << YAML::Value << YAML::BeginMap
        << YAML::Key << "chat_bg"          << YAML::Value << hex
        << YAML::Key << "ui_collapsed"    << YAML::Value << ui_collapsed_
        << YAML::Key << "alootid_overlay" << YAML::Value << show_alootid_overlay_
      << YAML::EndMap
      << YAML::EndMap;

  const std::string path = GetSettingsPath();
  std::ofstream f(path);
  if (!f) {
    LogError("[MoonlightUi] failed to write {}", path);
    return;
  }
  f << out.c_str();
  LogInfo("[MoonlightUi] saved chat_bg {} to {}", hex, path);
}

// ── Server settings sync ──────────────────────────────────────────────────

void MoonlightUi::UpdateRelay() {
  if (auto* relay = Bourgeon::Instance().discord_relay()) {
    relay->set_chat_active(discord_chat_ && in_gonryun_);
  }
}

void MoonlightUi::OnModeSwitch(ModeMgr::ModeType mode_type,
                               const char* map_name) {
  const bool was_in_game = in_game_;
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);

  // Only update in_gonryun_ when we have a real map name. OnUpdateHook fires
  // FireModeSwitch(kGame, "") on every tick for in_game_ tracking; that empty
  // call must not override the map we learned from a real CModeMgr::Switch.
  if (map_name && map_name[0] != '\0') {
    in_gonryun_ = in_game_ && (strncmp(map_name, kDiscordMap, sizeof(kDiscordMap) - 1) == 0);
  } else if (!in_game_) {
    in_gonryun_ = false;
  }

  if (in_game_ && !was_in_game)
    LoadSettings();

  if (!in_game_ && was_in_game)
    aloot_ids_.clear();

  UpdateRelay();
}

// ZC packet layout (data points past [opcode:2][total_len:2]):
//   [char_id:4][count:2][{id:2, value:4} * count]
void MoonlightUi::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                               uint16_t len) {
  if (opcode == kOpcodeMapMove) {
    // 0x0091 ZC_NPCACK_MAPMOVE: data points at mapname[16] (e.g. "gonryun.gat").
    const char* map_name = reinterpret_cast<const char*>(data);
    in_gonryun_ = in_game_ &&
                  (strncmp(map_name, kDiscordMap, sizeof(kDiscordMap) - 1) == 0);
    LogInfo("[MoonlightUi] map move -> '{}' in_gonryun={}",
            std::string(map_name, strnlen(map_name, len)), in_gonryun_);
    UpdateRelay();
    return;
  }

  if (opcode == kOpcodePresetList) {
    // ZC_BOURGEON_PRESET_LIST: [active_no:1][count:1][{no:1,autoload:1,namelen:1,name:var}...]
    // data points past [opcode:2][len:2], so data[0]=active_no, data[1]=count.
    if (len < 2) return;
    alootid_active_preset_   = data[0];
    alootid_selected_preset_ = data[0];  // select active preset in combo by default
    const uint8_t count = data[1];
    alootid_presets_.clear();
    uint16_t off = 2;
    for (uint8_t i = 0; i < count && off + 3 <= len; ++i) {
      AlootPreset p;
      p.no       = data[off];
      p.autoload = data[off + 1] != 0;
      const uint8_t namelen = data[off + 2];
      off += 3;
      if (off + namelen > len) break;
      p.name.assign(reinterpret_cast<const char*>(data + off), namelen);
      off += namelen;
      alootid_presets_.push_back(std::move(p));
    }
    // Auto-fill the save input with the active preset's name so "Sauvegarder"
    // updates it directly instead of creating a new slot.
    if (alootid_active_preset_ != 0) {
      for (const auto& p : alootid_presets_) {
        if (p.no == alootid_active_preset_) {
          std::strncpy(alootid_preset_input_, p.name.c_str(),
                       sizeof(alootid_preset_input_) - 1);
          alootid_preset_input_[sizeof(alootid_preset_input_) - 1] = '\0';
          break;
        }
      }
    }
    // Snapshot the current list as "saved state" — the server sends this packet
    // after every save/load/delete, so the snapshot stays in sync automatically.
    alootid_saved_ids_ = aloot_ids_;
    return;
  }

  if (opcode != kOpcodeFromServer) return;
  // Layout after the [opcode:2][len:2] header: [char_id:4][count:2][{id,value}*].
  if (len < 6) return;

  const uint16_t count = *reinterpret_cast<const uint16_t*>(data + 4);
  const uint16_t expected_len = static_cast<uint16_t>(6 + count * 6);
  if (len < expected_len) {
    LogError("[MoonlightUi] ZC_BOURGEON_SETTINGS truncated: len={} count={}", len, count);
    return;
  }

  for (uint16_t i = 0; i < count; ++i) {
    const uint16_t id    = *reinterpret_cast<const uint16_t*>(data + 6 + i * 6);
    const uint32_t value = *reinterpret_cast<const uint32_t*>(data + 6 + i * 6 + 2);
    switch (id) {
      case kSettingShowExp:
        show_exp_ = (value != 0);
        LogInfo("[MoonlightUi] show_exp={}", show_exp_);
        break;
      case kSettingShowZeny:
        show_zeny_ = (value != 0);
        LogInfo("[MoonlightUi] show_zeny={}", show_zeny_);
        break;
      case kSettingShowMobInfo:
        show_mob_info_ = (value != 0);
        LogInfo("[MoonlightUi] show_mob_info={}", show_mob_info_);
        break;
      case kSettingSeparate:
        separate_ = (value != 0);
        LogInfo("[MoonlightUi] separate={}", separate_);
        break;
      case kSettingBlockExp:
        block_exp_ = (value != 0);
        LogInfo("[MoonlightUi] block_exp={}", block_exp_);
        break;
      case kSettingAlootRare:
        aloot_rare_ = (value != 0);
        LogInfo("[MoonlightUi] aloot_rare={}", aloot_rare_);
        break;
      case kSettingAlootRate:
        aloot_rate_ = static_cast<int>(value);
        LogInfo("[MoonlightUi] aloot_rate={}", aloot_rate_);
        break;
      case kSettingAlootPognon:
        aloot_pognon_ = static_cast<int>(value) * 100;
        LogInfo("[MoonlightUi] aloot_pognon={}", aloot_pognon_);
        break;
      case kSettingAlootType:
        aloot_type_mask_ = static_cast<int>(value);
        LogInfo("[MoonlightUi] aloot_type_mask=0x{:04X}", aloot_type_mask_);
        break;
      case kSettingDiscordChat:
        discord_chat_ = (value != 0);
        LogInfo("[MoonlightUi] discord_chat={}", discord_chat_);
        UpdateRelay();
        break;
      case kSettingShowDelay:
        show_delay_ = (value != 0);
        LogInfo("[MoonlightUi] show_delay={}", show_delay_);
        break;
      case kSettingShowSpeed:
        show_speed_ = (value != 0);
        LogInfo("[MoonlightUi] show_speed={}", show_speed_);
        break;
      case kSettingSellStuff:
        sell_stuff_ = (value != 0);
        LogInfo("[MoonlightUi] sell_stuff={}", sell_stuff_);
        break;
      case kSettingSellItem:
        sell_item_ = (value != 0);
        LogInfo("[MoonlightUi] sell_item={}", sell_item_);
        break;
      case kSettingNoAsk:
        no_ask_ = (value != 0);
        LogInfo("[MoonlightUi] no_ask={}", no_ask_);
        break;
      case kSettingNoks:
        noks_ = static_cast<int>(value);
        LogInfo("[MoonlightUi] noks={}", noks_);
        break;
      case kSettingWings:
        wings_ = (value != 0);
        LogInfo("[MoonlightUi] wings={}", wings_);
        break;
      case kSettingAlootMvp:
        aloot_mvp_ = (value != 0);
        LogInfo("[MoonlightUi] aloot_mvp={}", aloot_mvp_);
        break;
      case kSettingAlootMvpRwd:
        aloot_mvp_rwd_ = (value != 0);
        LogInfo("[MoonlightUi] aloot_mvp_rwd={}", aloot_mvp_rwd_);
        break;
      case kSettingTriInv:
        tri_inv_ = static_cast<int>(value);
        LogInfo("[MoonlightUi] tri_inv={}", tri_inv_);
        break;
      case kSettingTriCart:
        tri_cart_ = static_cast<int>(value);
        LogInfo("[MoonlightUi] tri_cart={}", tri_cart_);
        break;
      case kSettingTriStorage:
        tri_storage_ = static_cast<int>(value);
        LogInfo("[MoonlightUi] tri_storage={}", tri_storage_);
        break;
      case kSettingTriGstorage:
        tri_gstorage_ = static_cast<int>(value);
        LogInfo("[MoonlightUi] tri_gstorage={}", tri_gstorage_);
        break;
      case kSettingAlootId:
        if (value == 0) {
          aloot_ids_.clear();
          LogInfo("[MoonlightUi] aloot_ids cleared");
        } else {
          bool found = false;
          for (uint32_t x : aloot_ids_) if (x == value) { found = true; break; }
          if (!found) aloot_ids_.push_back(value);
          LogInfo("[MoonlightUi] aloot_id added={}", value);
        }
        break;
      case kSettingAlootIdRemove:
        break;
      default:
        LogInfo("[MoonlightUi] unknown setting id={} value={}", id, value);
        break;
    }
  }
}

void MoonlightUi::SendSetting(uint16_t id, uint32_t value) {
  uint8_t buf[10];
  *reinterpret_cast<uint16_t*>(buf)     = kOpcodeToServer;
  *reinterpret_cast<uint16_t*>(buf + 2) = 10;
  *reinterpret_cast<uint16_t*>(buf + 4) = id;
  *reinterpret_cast<uint32_t*>(buf + 6) = value;
  Bourgeon::Instance().SendPacket(buf, sizeof(buf));
}

void MoonlightUi::SendPresetCmd(uint8_t cmd, uint8_t no, const char* name) {
  const uint16_t namelen = name ? static_cast<uint16_t>(strnlen(name, 50)) : 0;
  const uint16_t total   = static_cast<uint16_t>(6 + namelen);
  std::vector<uint8_t> buf(total);
  *reinterpret_cast<uint16_t*>(buf.data())     = kOpcodePresetCmd;
  *reinterpret_cast<uint16_t*>(buf.data() + 2) = total;
  buf[4] = cmd;
  buf[5] = no;
  if (namelen > 0) std::memcpy(buf.data() + 6, name, namelen);
  Bourgeon::Instance().SendPacket(buf.data(), total);
}

// Helper to display a little (?) mark which shows a tooltip when hovered.
// In your own code you may want to display an actual icon if you are using a merged icon fonts (see docs/FONTS.md)
static void HelpMarker(const char* desc)
{
  ImGui::TextDisabled("(?)");
  if (ImGui::BeginItemTooltip())
  {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
    ImGui::TextUnformatted(desc);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// Make the UI compact because there are so many fields
static void PushStyleCompact()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, (float)(int)(style.FramePadding.y * 0.60f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, (float)(int)(style.ItemSpacing.y * 0.60f)));
}

static void PopStyleCompact()
{
    ImGui::PopStyleVar(2);
}

// ── ImGui panel ───────────────────────────────────────────────────────────

void MoonlightUi::OnRenderUI() {
  if (!in_game_) return;

  if (apply_collapse_) {
    ImGui::SetNextWindowCollapsed(ui_collapsed_, ImGuiCond_Always);
    apply_collapse_ = false;
  }

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
  ImGui::Begin("Moonlight-Destiny");

  const bool is_collapsed = ImGui::IsWindowCollapsed();
  if (is_collapsed != ui_collapsed_) {
    ui_collapsed_ = is_collapsed;
    SaveSettings();
  }
  ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (!is_collapsed) {

    if (ImGui::CollapsingHeader("Règles du serveur"))
    {
      ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "CES RÈGLEMENTS S'APPLIQUENT PARTOUT SUR MOONLIGHT-DESTINY !");
      if (ImGui::TreeNode("Règlements généraux"))
      {
        ImGui::Text("Les règles du serveur doivent être appliquées à la lettre.\nToute personne ne respectant pas la charte sera sanctionnée dans les plus brefs délais.");
        ImGui::Spacing();
        ImGui::BulletText("Les joueurs doivent se respecter et garder un langage propre et courtois.");
        ImGui::BulletText("Les propos visant à rejeter un nouveau joueur sont interdits.");
        ImGui::BulletText("L'utilisation de programmes tels que bots ou hacks = ban définitif sans hésitation.");
        ImGui::BulletText("Le flood est strictement interdit.");
        ImGui::BulletText("Vous êtes entièrement responsable de votre compte.");
        ImGui::BulletText("Le staff ne rend pas les items perdus (vente NPC, deslotage raté, refine raté).");
        ImGui::BulletText("Le staff peut exceptionnellement rendre un item perdu si les logs prouvent un bug serveur.");
        ImGui::BulletText("Ne partagez jamais votre compte ou votre mot de passe.");
        ImGui::BulletText("La demande de support pour créer un serveur privé est non recommandée.");
        ImGui::BulletText("Le plagiat volontaire d'un membre du staff est puni.");
        ImGui::BulletText("Tout ce qui se rapporte au serveur est la propriété exclusive des administrateurs.");
        ImGui::BulletText("Le langage SMS est à proscrire.");
        ImGui::BulletText("L'exploitation d'un bug ou abus = sanction. Prévenez immédiatement un administrateur.");
        ImGui::BulletText("Si vous abusez du cashshop en votant avec plusieurs comptes forum… \ngare à vous c'est comme avec les impôts, \ntant qu'on est pas contrôlé c'est la fête, mais quand ils vous tombent dessus...");
        ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Sur le serveur de jeu"))
      {
        ImGui::BulletText("Insultes et vols de drop (Looting) = INTERDITS.");
        ImGui::BulletText("Heal ou buff un monstre qui ne vous appartient pas sans accord = puni.");
        ImGui::BulletText("Si vous êtes banni définitivement, tous les comptes liés à votre IP/PC le seront aussi.");
        ImGui::BulletText("Les sanctions (mute, jail, kick, ban) sont à la discrétion du staff.");
        ImGui::BulletText("Le Kill Steal est strictement interdit (voir définition). Utilisez @noks pour vous protéger.");
        ImGui::Spacing();
        ImGui::BulletText("Les MVPs sont FFA :");
        ImGui::Indent();
        ImGui::Text("Vous pouvez les attaquer même si quelqu'un est dessus.");
        ImGui::Text("(À vous de voir si vous voulez passer pour un gros connard selfish en KSant le MVP)");
        ImGui::Text("Si vous ne voulez pas vous faire KS, faites @noks <3");
        ImGui::Unindent();
        ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Le staff"))
      {
        ImGui::BulletText("Si vous cassez les couilles du staff ban/delete non temporaire.");
        ImGui::BulletText("Aucun membre du staff ne vous demandera votre mot de passe.");
        ImGui::BulletText("Aucun membre du staff ne vous demandera votre login.");
        ImGui::BulletText("Aucun membre du staff ne vous demandera votre email.");
        ImGui::BulletText("Seuls les admins peuvent rendre des items perdus suite à un bug serveur.");
        ImGui::BulletText("Le staff ne rend pas les items prêtés à un joueur disparu/banni.");
        ImGui::BulletText("Le staff ne donne pas d'items (hors events).");
        ImGui::BulletText("Les membres du staff ne sont pas des robots. Soyez courtois, cherchez avant de demander.");
        ImGui::BulletText("Les questions dont la réponse est sur une database = évitez.");
        ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Règlements dans les endroits spécifiques"))
      {
        if (ImGui::TreeNode("Salle de duel"))
        {
          ImGui::BulletText("Ce n'est pas un salon de thé");
          ImGui::BulletText("Si vous regardez, ok. Sinon, laissez la place.");
          ImGui::BulletText("Utilisez : @duel, @invite, @accept, @reject, @leave.");
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("Carnage Room"))
        {
          ImGui::BulletText("Loi du plus fort.");
          ImGui::BulletText("Amusez‑vous dans le respect.");
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("PVP Room"))
        {
          ImGui::BulletText("Free Kill interdit.");
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("DB Room"))
        {
          ImGui::BulletText("Kill Steal STRICTEMENT interdit.");
          ImGui::BulletText("Si la personne meurt ou se hide les mobs sont à vous.");
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("Guild Dungeon"))
        {
          ImGui::BulletText("Libre de tuer les guildiens adverses.");
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("WoE Castles"))
        {
          ImGui::BulletText("Interdiction d'apporter de l'aide via un perso non participant (multi-account/perso).");
          ImGui::BulletText("Les ententes entre guildes sont informelles, non officielles, non sanctionnables.");
          ImGui::BulletText("Elles doivent être discutées entre guildes dominantes, dans le respect.");
          ImGui::TreePop();
        }
        ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Logiciels tiers"))
      {
        ImGui::Text("Autorisations :");
        ImGui::Indent();
          ImGui::BulletText("Je vais être clair : oui, j'autorise les scripts AHK, les macros clavier/souris, les trucs qui bouclent un sort… tant que ça reste :");
          ImGui::BulletText("SIMPLE");
          ImGui::BulletText("BASIQUE");
          ImGui::BulletText("Pas un tableau de bord de la NASA");
          ImGui::BulletText("Vous bouclez le spell, éventuellement un clic en plus pour les AOE type Storm Gust, et basta.");
        ImGui::Unindent();
        ImGui::Text("Quality of Life :");
        ImGui::Indent();
          ImGui::BulletText("Le but, c'est du Q.O.L");
          ImGui::BulletText("Vous préservez votre clavier, votre souris, vos doigts, vos poignets, vos oreilles, et celles de vos voisins qui n'ont rien demandé.");
          ImGui::BulletText("Bref : du confort, pas du cheat.");
        ImGui::Unindent();
        ImGui::Text("Les trucs interdits (et je rigole zéro) :");
        ImGui::Indent();
          ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Ne me prenez pas pour un jambon.");
          ImGui::Text("Si vous me sortez :");
          ImGui::Indent();
            ImGui::BulletText("un auto-buffer");
            ImGui::BulletText("un auto-pot");
            ImGui::BulletText("un super TP/SG de physicien quantique");
            ImGui::BulletText("un script qui ferait rougir Tony Stark");
          ImGui::Unindent();
          ImGui::Text("Alors là :");
          ImGui::Indent();
            ImGui::BulletText("Je vous fais le fion.");
            ImGui::BulletText("Je m'en bats les couilles.");
            ImGui::BulletText("Je vous dégage plus vite que Thanos avec son finger snap. *Snap*");
          ImGui::Unindent();
          ImGui::Text("Les excuses bidon :");
          ImGui::Indent();
            ImGui::BulletText("\"Mais les autres serveurs le font...\"");
            ImGui::BulletText("\"Mais j'étais pas AFK, je regardais Naruto à côté...\"");
          ImGui::Unindent();
          ImGui::Text("Résultat :");
          ImGui::Indent();
            ImGui::BulletText("Pouf.");
            ImGui::BulletText("Vous étiez sur Moon.");
            ImGui::BulletText("Vous ne l'êtes plus.");
            ImGui::BulletText("Et il ne restera de vous que des ruines numériques sur Wayback Machine.");
          ImGui::Unindent();
        ImGui::Unindent();
        ImGui::TreePop();
      }
    }

    // ── Chat Box Settings ────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Chat Settings"))
    {
      PushStyleCompact();
      if (ImGui::Checkbox("Chat Discord (Gonryun only)", &discord_chat_)) {
        UpdateRelay();
        SendSetting(kSettingDiscordChat, discord_chat_ ? 1 : 0);
      }

        ImGui::SameLine();
      // ── Chat Background Color ─────────────────────────────────────────────
      if (chat_bg_instr_) {
        // Color swatch — click to open the picker popup.
        const ImVec4 swatch(chat_bg_color_[0], chat_bg_color_[1],
                            chat_bg_color_[2], chat_bg_color_[3]);
        if (ImGui::ColorButton("##chatbg_btn", swatch,
                              ImGuiColorEditFlags_AlphaPreview, ImVec2(20, 20)))
          ImGui::OpenPopup("chatbg_picker");

        ImGui::SameLine();
        ImGui::TextUnformatted("Background Chat Color");

        // Popup with full picker + explicit Close button.
        if (ImGui::BeginPopup("chatbg_picker")) {
          if (ImGui::ColorPicker4("##chatbg", chat_bg_color_,
                                  ImGuiColorEditFlags_AlphaBar |
                                  ImGuiColorEditFlags_NoSidePreview)) {
            PatchInstruction(PickerToArgb());
            picker_was_editing_ = true;
          }
          if (picker_was_editing_ && ImGui::IsMouseReleased(0)) {
            const uint32_t argb = PickerToArgb();
            PatchExistingObjects(argb);
            SaveSettings();
            picker_was_editing_ = false;
          }
          ImGui::Separator();
          if (ImGui::Button("Close", ImVec2(-1.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
        }
      } else {
        ImGui::TextDisabled("(chat background patch unavailable)");
      }
      PopStyleCompact();
    }
    // ── Commands Settings ────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Commands Settings"))
    {
      PushStyleCompact();
      ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_None;
      if (ImGui::BeginTabBar("CommandsSettingsTabs", tab_bar_flags))
      {
        if (ImGui::BeginTabItem("Général"))
        {
          if (ImGui::BeginTable("split", 2)) // Toggles settings
          {
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Show EXP gain", &show_exp_)) SendSetting(kSettingShowExp, show_exp_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Affiche le gain d'EXP dans le chat log. (@showexp)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Show Zeny gain", &show_zeny_)) SendSetting(kSettingShowZeny, show_zeny_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Affiche le gain de Zeny dans le chat log. (@showzeny)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Show mob info", &show_mob_info_)) SendSetting(kSettingShowMobInfo, show_mob_info_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Affiche la RACE et l'ELEMENT des monstres,\nsous leur nom. (Thx Doo - @showmobinfo)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Separate Kills", &separate_)) SendSetting(kSettingSeparate, separate_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Affiche un séparateur dans le chat log entre chaque kill de mobs. (Demandez à Spider - @separate)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Block EXP Gain", &block_exp_)) SendSetting(kSettingBlockExp, block_exp_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Bloque le gain d'EXP. (@blockexp)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Show Skill Delay", &show_delay_)) SendSetting(kSettingShowDelay, show_delay_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Affiche un message dans le chat quand un skill\néchoue à cause du cooldown. (@showdelay)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Show Speed", &show_speed_)) SendSetting(kSettingShowSpeed, show_speed_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Affiche la valeur de vitesse de déplacement et d'attaque\ndans le chat lors d'un changement comme après\nun buff style AgiUP ou Card. (@showspeed)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Sell Stuff", &sell_stuff_)) SendSetting(kSettingSellStuff, sell_stuff_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Permet la vente d'items améliorés (refine > 0),\ncartes, munitions et items slotés chez les PNJ marchands.\nDésactiver pour protéger ces items. (@sellstuff)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Sell Item", &sell_item_)) SendSetting(kSettingSellItem, sell_item_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker(
              "Permet la vente des items du groupe IG_SELLITEM chez les PNJ marchands.\nDésactiver pour les protéger. (@sellitem)\n\n"
              "Groupe SELLITEM :\nGreen Potion (506)\nWhite Slim Potion (547)\nLucky Candy (570)\n"
              "Old Blue Box (603)\nYggdrasil Berry (607)\nYggdrasil Seed (608)\nOld Card Album (616)\n"
              "Old Violet Box (617)\nGift Box (644)\nPoison Bottle (678)\nGold (969)\n"
              "Temporal Crystal (6607)\nCoagulated Spell (6608)\nJitterbug's Tooth (6719)\n"
              "Fire Bottle (7135)\nAcid Bottle (7136)\nCoating Bottle (7139)\n"
              "Fragment of Agony (7436)\nFragment of Misery (7437)\nFragment of Hatred (7438)\nPiece of Memory Red (7439)\n"
              "Ice Scale (7562)\nCursed Water (12020)\nElemental Converter Fire (12114)\nElemental Converter Water (12115)\n"
              "Elemental Converter Earth (12116)\nElemental Converter Wind (12117)\nMystical Card Album (12246)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("No Ask", &no_ask_)) SendSetting(kSettingNoAsk, no_ask_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Refuse automatiquement les invitations\nde trade, de guilde et d'alliance. (@noask)");
            ImGui::TableNextColumn(); if (ImGui::Checkbox("Wings", &wings_)) SendSetting(kSettingWings, wings_ ? 1 : 0);
            ImGui::SameLine(); HelpMarker("Active ou désactive le sprite alternatif des Angel wings et Devil wings (Moonlight 2005 vibe - @wings)");
            ImGui::EndTable();
          }
          // @noks — combo 4 options (off / self / party / guild)
          {
            static const char* kNoksLabels[] = { "Off", "Self", "Party", "Guild" };
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::BeginCombo("@noks", kNoksLabels[noks_ < 4 ? noks_ : 0])) {
              for (int i = 0; i < 4; ++i) {
                const bool selected = (noks_ == i);
                if (ImGui::Selectable(kNoksLabels[i], selected)) {
                  noks_ = i;
                  SendSetting(kSettingNoks, static_cast<uint16_t>(i));
                }
                if (selected) ImGui::SetItemDefaultFocus();
              }
              ImGui::EndCombo();
            }
            ImGui::SameLine(); HelpMarker("Kill Steal Protection — empêche d'autres joueurs de voler vos kills MVP.\nSelf = toi seulement, Party = ta party, Guild = ta guilde. (@noks)");
          }
          // Tri inventaires — combo 7 options (0=Par ID … 6=Aucun)
          {
            static const char* kTriLabels[] = { "Par ID", "Par type", "Par quantité", "Par poids", "Par prix", "Par nom", "Aucun" };
            auto TriCombo = [&](const char* label, int& value, uint16_t setting_id) {
              ImGui::SetNextItemWidth(130.0f);
              if (ImGui::BeginCombo(label, kTriLabels[value >= 0 && value < 7 ? value : 0])) {
                for (int i = 0; i < 7; ++i) {
                  const bool selected = (value == i);
                  if (ImGui::Selectable(kTriLabels[i], selected)) {
                    value = i;
                    SendSetting(setting_id, static_cast<uint16_t>(i));
                  }
                  if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
              }
            };
            TriCombo("Tri Inventaire", tri_inv_, kSettingTriInv);
            ImGui::SameLine(); HelpMarker("Tri automatique de l'inventaire.");
            TriCombo("Tri Chariot",    tri_cart_, kSettingTriCart);
            ImGui::SameLine(); HelpMarker("Tri automatique du chariot.");
            TriCombo("Tri Coffre",     tri_storage_, kSettingTriStorage);
            ImGui::SameLine(); HelpMarker("Tri automatique du coffre personnel à la prochaine ouverture.");
            TriCombo("Tri Coffre Guilde", tri_gstorage_, kSettingTriGstorage);
            ImGui::SameLine(); HelpMarker("Tri automatique du coffre de guilde à la prochaine ouverture.");
          }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Autoloots"))
        {
          ImGui::Spacing();
          {// @autoloot
            int rate = aloot_rate_;
            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::SliderInt("@autoloot", &rate, 0, 100, "%d%%")) {
              aloot_rate_ = rate;
              SendSetting(kSettingAlootRate, static_cast<uint16_t>(rate));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##rate")) {
              aloot_rate_ = 0;
              SendSetting(kSettingAlootRate, 0);
            }
          }
          ImGui::Separator();
          { // @autolootpognon
            int pognon = aloot_pognon_;
            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::InputInt("@autolootpognon (z)", &pognon, 100, 10000)) {
              if (pognon < 0) pognon = 0;
              if (pognon > 1000000) pognon = 1000000;
              pognon = (pognon / 100) * 100;
              aloot_pognon_ = pognon;
              SendSetting(kSettingAlootPognon, static_cast<uint16_t>(pognon / 100));
            }
          ImGui::SameLine(); HelpMarker("Autoloot des items ayant au minimum le prix de revente configuré.");
            auto apply_pognon_delta = [this](int delta) {
              int v = aloot_pognon_ + delta;
              if (v < 0) v = 0;
              if (v > 1000000) v = 1000000;
              v = (v / 100) * 100;
              aloot_pognon_ = v;
              SendSetting(kSettingAlootPognon, static_cast<uint16_t>(v / 100));
            };
            if (ImGui::Button("-10kz"))  apply_pognon_delta(-10000);
            ImGui::SameLine();
            if (ImGui::Button("-1kz"))   apply_pognon_delta(-1000);
            ImGui::SameLine();
            if (ImGui::Button("+1kz"))   apply_pognon_delta(1000);
            ImGui::SameLine();
            if (ImGui::Button("+10kz"))  apply_pognon_delta(10000);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##pognon")) {
              aloot_pognon_ = 0;
              SendSetting(kSettingAlootPognon, 0);
            }
          }
          ImGui::Separator();
          if (ImGui::TreeNode("@autoloottype")) {// @autoloottype
            ImGui::TextUnformatted("@autoloottype :");
            ImGui::SameLine(); HelpMarker("Cochez les types d'items à lootter automatiquement.\nHealing=0 Usable=2 Etc=3 Armor=4 Weapon=5\nCard=6 PetEgg=7 PetArmor=8 Ammo=10 Cash=11");
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##type")) {
              aloot_type_mask_ = 0;
              SendSetting(kSettingAlootType, 0);
            }
            static const struct { const char* label; int bit; } kAlootTypes[] = {
              {"Healing",   1 << 0},  {"Usable",    1 << 2},
              {"Etc",       1 << 3},  {"Armor",     1 << 4},
              {"Weapon",    1 << 5},  {"Card",      1 << 6},
              {"Pet Egg",   1 << 7},  {"Pet Armor", 1 << 8},
              {"Ammo",      1 << 10}, {"Cash",     1 << 11},
            };
            if (ImGui::BeginTable("aloottype", 2)) {
              for (const auto& t : kAlootTypes) {
                ImGui::TableNextColumn();
                bool checked = (aloot_type_mask_ & t.bit) != 0;
                if (ImGui::Checkbox(t.label, &checked)) {
                  if (checked) aloot_type_mask_ |=  t.bit;
                  else         aloot_type_mask_ &= ~t.bit;
                  SendSetting(kSettingAlootType, static_cast<uint16_t>(aloot_type_mask_));
                }
              }
              ImGui::EndTable();
            }
            ImGui::TreePop();
          }
          ImGui::Separator();
          {// @autolootrare
          if (ImGui::Checkbox("Autoloot rares", &aloot_rare_)) SendSetting(kSettingAlootRare, aloot_rare_ ? 1 : 0);
          ImGui::SameLine(); HelpMarker(
            "Autolooting: Toutes les Cards\nOld Blue Box (603)\nYggdrasil Berry (607)\nYggdrasil Seed (608)\nOld Card Album (616)\nOld Purple Box (617)\nGift Box (644)\nGold (969)\n"
            "Temporal Crystal (6607)\nCoagulated Spell (6608)\nJitterbug's Tooth (6719)\nFragment of Agony (7436)\nFragment of Misery (7437)\nFragment of Hatred (7438)\n"
            "Piece_Of_Memory_Red (7439)\nTreasure Box (7444)\nCursed Water (12020)\nElemental Converter Fire (12114)\nElemental Converter Water (12115)\n"
            "Elemental Converter Earth (12116)\nElemental Converter Wind (12117)\nMystical Card Album (12246)\nSentimental Fragment (22687)\nCursed Fragment (23016)");
          }
          ImGui::Separator();
          {// @autolootmvp / @autolootmvpreward
          if (ImGui::Checkbox("Autoloot MVP cards", &aloot_mvp_)) SendSetting(kSettingAlootMvp, aloot_mvp_ ? 1 : 0);
          ImGui::SameLine(); HelpMarker("Loot automatiquement les cartes MVP\nquelque soit leur taux de drop. (@autolootmvp)");
          if (ImGui::Checkbox("Autoloot MVP rewards (actif par défaut)", &aloot_mvp_rwd_)) SendSetting(kSettingAlootMvpRwd, aloot_mvp_rwd_ ? 1 : 0);
          ImGui::SameLine(); HelpMarker("Les drops de récompense des MVP sont lootés\nautomatiquement par défaut.\nDécocher pour désactiver. (@autolootmvpreward)");
          }
          ImGui::Separator();
          if (ImGui::TreeNode("@autolootid")) {// @autolootid
            ImGui::TextUnformatted("@autolootid :");
            ImGui::SameLine(); HelpMarker("Loot automatiquement les items par ID.\nMax 50 IDs. (@autolootid <id>)");
            ImGui::SameLine();
            if (ImGui::Checkbox("Overlay", &show_alootid_overlay_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker("Affiche un bouton Add/Remove Alootid\nprès du curseur au clic droit sur un item.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##alootid")) {
              aloot_ids_.clear();
              SendSetting(kSettingAlootId, 0);
            }
            ImGui::SetNextItemWidth(100.0f);
            ImGui::InputInt("##alootid_input", &aloot_id_input_, 0, 0);
            if (aloot_id_input_ < 0) aloot_id_input_ = 0;
            ImGui::SameLine();
            const bool can_add = (aloot_id_input_ > 0 && aloot_ids_.size() < 50);
            if (!can_add) ImGui::BeginDisabled();
            if (ImGui::Button("Add##alootid")) {
              const uint32_t id = static_cast<uint32_t>(aloot_id_input_);
              bool found = false;
              for (uint32_t x : aloot_ids_) if (x == id) { found = true; break; }
              if (!found) {
                aloot_ids_.push_back(id);
                SendSetting(kSettingAlootId, id);
              }
            }
            if (!can_add) ImGui::EndDisabled();
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
            {
              // Detect unsaved changes: compare current list vs snapshot (order-independent).
              auto sorted_copy = [](const std::vector<uint32_t>& v) {
                auto s = v; std::sort(s.begin(), s.end()); return s;
              };
              const bool dirty = (alootid_active_preset_ != 0) &&
                                 (sorted_copy(aloot_ids_) != sorted_copy(alootid_saved_ids_));

              static char hdr[96];
              if (alootid_active_preset_ != 0) {
                const char* preset_name = nullptr;
                char no_buf[8];
                for (const auto& p : alootid_presets_) {
                  if (p.no == alootid_active_preset_) {
                    preset_name = p.name.empty()
                      ? (std::snprintf(no_buf, sizeof(no_buf), "#%u", p.no), no_buf)
                      : p.name.c_str();
                    break;
                  }
                }
                if (dirty)
                  std::snprintf(hdr, sizeof(hdr), "%s (non sauvegardé)",
                                preset_name ? preset_name : "?");
                else
                  std::snprintf(hdr, sizeof(hdr), "%s",
                                preset_name ? preset_name : "?");
              } else {
                std::strncpy(hdr, "Liste courante", sizeof(hdr));
              }
              ImGui::TextUnformatted(hdr);
            }
            ImGui::BeginChild("##alootid_list", ImVec2(0, 160), true);
            if (ImGui::BeginTable("##alootid_tbl", 2, ImGuiTableFlags_SizingStretchSame)) {
              for (int i = 0; i < static_cast<int>(aloot_ids_.size()); ++i) {
                ImGui::TableNextColumn();
                const uint32_t id = aloot_ids_[i];
                char lbl[32];
                std::snprintf(lbl, sizeof(lbl), "x##alootid_%d", i);
                if (ImGui::SmallButton(lbl)) {
                  SendSetting(kSettingAlootIdRemove, aloot_ids_[i]);
                  aloot_ids_.erase(aloot_ids_.begin() + i);
                  --i;
                  continue;
                }
                ImGui::SameLine();
                const auto it = item_names_.find(id);
                if (it != item_names_.end())
                  ImGui::Text("%s [%u]", it->second.c_str(), id);
                else
                  ImGui::Text("[%u]", id);
              }
              ImGui::EndTable();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            // Quick-add/remove from the last right-clicked item description window.
            if (g_last_viewed_item != 0) {
              ImGui::Separator();
              const auto itv = item_names_.find(g_last_viewed_item);
              if (itv != item_names_.end())
                ImGui::Text("Vu: [%u] %s", g_last_viewed_item, itv->second.c_str());
              else
                ImGui::Text("Vu: [%u]", g_last_viewed_item);
              ImGui::SameLine();
              int vu_idx = -1;
              for (int k = 0; k < static_cast<int>(aloot_ids_.size()); ++k)
                if (aloot_ids_[k] == g_last_viewed_item) { vu_idx = k; break; }
              if (vu_idx >= 0) {
                if (ImGui::SmallButton("Remove##alootid_vu")) {
                  SendSetting(kSettingAlootIdRemove, aloot_ids_[vu_idx]);
                  aloot_ids_.erase(aloot_ids_.begin() + vu_idx);
                }
              } else {
                const bool can_add_vu = (aloot_ids_.size() < 50);
                if (!can_add_vu) ImGui::BeginDisabled();
                if (ImGui::SmallButton("Add##alootid_vu")) {
                  aloot_ids_.push_back(g_last_viewed_item);
                  SendSetting(kSettingAlootId, g_last_viewed_item);
                }
                if (!can_add_vu) ImGui::EndDisabled();
              }
            }
            // ── Presets (server-backed, DB table `alootid`) ──
            ImGui::Separator();
            ImGui::TextUnformatted("Presets :");
            // Autoload indicator + toggle, on the same line as the label.
            {
              const AlootPreset* autoload_preset = nullptr;
              for (const auto& p : alootid_presets_)
                if (p.autoload) { autoload_preset = &p; break; }
              ImGui::SameLine();
              // Find the selected preset to know what toggling autoload does.
              const AlootPreset* sel_for_al = nullptr;
              for (const auto& p : alootid_presets_)
                if (p.no == alootid_selected_preset_) { sel_for_al = &p; break; }
              bool al = sel_for_al && sel_for_al->autoload;
              if (!sel_for_al) ImGui::BeginDisabled();
              if (ImGui::Checkbox("Autoload##preset", &al))
                SendPresetCmd(5, al ? alootid_selected_preset_ : 0);
              if (!sel_for_al) ImGui::EndDisabled();
              ImGui::SameLine();
              if (autoload_preset) {
                if (autoload_preset->name.empty())
                  ImGui::TextDisabled("(#%u)", autoload_preset->no);
                else
                  ImGui::TextDisabled("(%s)", autoload_preset->name.c_str());
              } else {
                ImGui::TextDisabled("(aucun)");
              }
            }
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputText("##preset_name", alootid_preset_input_,
                             sizeof(alootid_preset_input_));
            ImGui::SameLine();
            ImGui::Checkbox("Renommer##preset_toggle", &alootid_rename_open_);
            if (alootid_rename_open_) {
              ImGui::SameLine();
              ImGui::SetNextItemWidth(120.0f);
              ImGui::InputText("##preset_rename", alootid_rename_input_,
                               sizeof(alootid_rename_input_));
            }
            ImGui::SameLine();
            {
              const AlootPreset* sel_for_rename = nullptr;
              for (const auto& p : alootid_presets_)
                if (p.no == alootid_selected_preset_) { sel_for_rename = &p; break; }

              // Find existing preset matching the typed name (for delete-on-empty).
              const AlootPreset* named_preset = nullptr;
              for (const auto& p : alootid_presets_)
                if (p.name == alootid_preset_input_) { named_preset = &p; break; }

              const bool is_rename = alootid_rename_open_;
              const bool list_empty = aloot_ids_.empty();
              // Rename: need a new name + a selected preset.
              // Save: need a non-empty name; list empty → delete the named preset.
              const bool can_act = is_rename
                ? (alootid_rename_input_[0] != '\0' && sel_for_rename != nullptr)
                : (alootid_preset_input_[0] != '\0' &&
                   (!list_empty || named_preset != nullptr));

              if (!can_act) ImGui::BeginDisabled();
              const char* btn_label = (!is_rename && list_empty && named_preset)
                ? "Supprimer##preset_save" : "Sauvegarder##preset";
              if (ImGui::SmallButton(btn_label)) {
                if (is_rename && sel_for_rename) {
                  SendPresetCmd(6, alootid_selected_preset_, alootid_rename_input_);
                  alootid_rename_open_ = false;
                } else if (!is_rename && list_empty && named_preset) {
                  SendPresetCmd(4, named_preset->no);
                } else {
                  uint8_t save_no = 0;
                  bool used[11] = {};
                  for (const auto& p : alootid_presets_) {
                    if (p.name == alootid_preset_input_) { save_no = p.no; break; }
                    if (p.no <= 10) used[p.no] = true;
                  }
                  if (save_no == 0)
                    for (uint8_t n = 1; n <= 10; ++n) if (!used[n]) { save_no = n; break; }
                  if (save_no > 0)
                    SendPresetCmd(2, save_no, alootid_preset_input_);
                }
              }
              if (!can_act) ImGui::EndDisabled();
            }

            // Combo: select preset by name
            {
              const AlootPreset* sel_preset = nullptr;
              for (const auto& p : alootid_presets_)
                if (p.no == alootid_selected_preset_) { sel_preset = &p; break; }

              auto preset_label = [](const AlootPreset& p, char* buf, size_t sz, bool mark_active) {
                if (p.name.empty())
                  std::snprintf(buf, sz, "#%u%s", p.no, mark_active ? " *" : "");
                else
                  std::snprintf(buf, sz, "%s%s", p.name.c_str(), mark_active ? " *" : "");
              };
              char preview_buf[66];
              const char* preview;
              if (sel_preset) {
                preset_label(*sel_preset, preview_buf, sizeof(preview_buf), false);
                preview = preview_buf;
              } else {
                preview = "-- choisir --";
              }
              ImGui::SetNextItemWidth(120.0f);
              if (ImGui::BeginCombo("##preset_select", preview)) {
                for (const auto& p : alootid_presets_) {
                  const bool sel = (p.no == alootid_selected_preset_);
                  char label[66];
                  preset_label(p, label, sizeof(label), p.no == alootid_active_preset_);
                  if (ImGui::Selectable(label, sel))
                    alootid_selected_preset_ = p.no;
                  if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
              }
              ImGui::SameLine();
              const bool has_sel = sel_preset != nullptr;
              if (!has_sel) ImGui::BeginDisabled();
              if (ImGui::SmallButton("Charger##preset"))
                SendPresetCmd(3, alootid_selected_preset_);
              ImGui::SameLine();
              if (ImGui::SmallButton("Supprimer##preset"))
                SendPresetCmd(4, alootid_selected_preset_);
              if (!has_sel) ImGui::EndDisabled();

            }
            ImGui::TreePop();
          }
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
      PopStyleCompact();
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(4);

  // ── Alootid floating overlay ──────────────────────────────────────────────
  // Detect silent tooltip close (e.g. comparison→non-comparison): the game
  // zeroes kItemDescWndGlobalPtr without sending our hook a close message.
  if (g_item_desc_visible &&
      *reinterpret_cast<const uintptr_t*>(kItemDescWndGlobalPtr) == 0) {
    g_item_desc_visible = false;
    g_item_desc_wnd_ptr = nullptr;
  }

  if (show_alootid_overlay_ && g_last_viewed_item != 0 && g_item_desc_visible) {
    // Try to read the tooltip window position from the stored object pointer.
    // Offsets found via CheatEngine: [ptr+0x18]=Y, [ptr+0x20]=X.
    // If the pointer doesn't match the right object the values will be garbage
    // and we fall back to the cursor position captured at open time.
    float overlay_x = static_cast<float>(g_item_desc_cursor.x) + 12.0f;
    float overlay_y = static_cast<float>(g_item_desc_cursor.y) + 12.0f;
    if (g_item_desc_wnd_ptr != nullptr) {
      const auto* base = static_cast<const uint8_t*>(g_item_desc_wnd_ptr);
      const int wx = *reinterpret_cast<const int*>(base + 0x1C);  // X (confirmed)
      const int wy = *reinterpret_cast<const int*>(base + 0x20);  // Y (confirmed)
      if (wx > 0 && wx < 4096 && wy > 0 && wy < 4096) {
        overlay_x = static_cast<float>(wx);
        overlay_y = static_cast<float>(wy) - 24.0f;
      }
    }
    ImGui::SetNextWindowPos(ImVec2(overlay_x, overlay_y), ImGuiCond_Always);  // live-track
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    constexpr ImGuiWindowFlags kOverlayFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##alootid_overlay", nullptr, kOverlayFlags)) {
      const auto itv = item_names_.find(g_last_viewed_item);
      if (itv != item_names_.end())
        ImGui::TextUnformatted(itv->second.c_str());
      else
        ImGui::Text("[%u]", g_last_viewed_item);

      int ov_idx = -1;
      for (int k = 0; k < static_cast<int>(aloot_ids_.size()); ++k)
        if (aloot_ids_[k] == g_last_viewed_item) { ov_idx = k; break; }

      ImGui::SameLine();
      if (ov_idx >= 0) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.18f, 0.18f, 1.0f));
        if (ImGui::SmallButton("- alootid")) {
          SendSetting(kSettingAlootIdRemove, aloot_ids_[ov_idx]);
          aloot_ids_.erase(aloot_ids_.begin() + ov_idx);
        }
        ImGui::PopStyleColor();
      } else {
        const bool can_add = (aloot_ids_.size() < 50);
        if (!can_add) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.18f, 1.0f));
        if (ImGui::SmallButton("+ alootid")) {
          aloot_ids_.push_back(g_last_viewed_item);
          SendSetting(kSettingAlootId, g_last_viewed_item);
        }
        ImGui::PopStyleColor();
        if (!can_add) ImGui::EndDisabled();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("x"))
        g_item_desc_visible = false;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }
}
