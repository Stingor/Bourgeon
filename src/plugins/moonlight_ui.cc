#include "plugins/moonlight_ui.h"

#include <Windows.h>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "bourgeon.h"
#include "plugins/item_desc_tweaks.h"
#include "imgui.h"
#include "ui/ro_imgui.h"
#include "plugins/chat.h"
#include "plugins/discord_relay.h"
#include "plugins/basic_info.h"
#include "plugins/dps_meter.h"
#include "plugins/menu_icons.h"
#include "plugins/status_icon_tweaks.h"
#include "plugins/quest_tracker_tweaks.h"
#include "plugins/settings_tweaks.h"
#include "plugins/skill_bar_tweaks.h"
#include "plugins/storage_tweaks.h"
#include "plugins/inventory_viewer.h"
#include "plugins/cashshop_tweaks.h"
#include "plugins/shop_tweaks.h"
#include "plugins/npc_dialog_tweaks.h"
#include "plugins/bug_report.h"
#include "plugins/character_sheet.h"
#include "plugins/doom_tweaks.h"
#include "plugins/roggle_tweaks.h"
#include "plugins/rojeweled_tweaks.h"
#include "plugins/status_tweaks.h"
#include "plugins/equip_tweaks.h"
#include "plugins/window_pos_tweaks.h"
#include "ragnarok/ui_window_mgr.h"
#include "spdlog/fmt/fmt.h"
#include "utils/byte_pattern.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"
#include "yaml-cpp/yaml.h"

// ── Presets de skin RO (jeux de couleurs nommés, sauvegardés dans le yaml) ──────
namespace {
struct RoPreset {
  std::string name;
  ro::RoSkinConfig cfg;
};
std::vector<RoPreset> g_ro_presets;
int g_ro_preset_sel = -1;

unsigned PackCol(const float* c) {
  return static_cast<unsigned>(
      ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3])));
}
void UnpackCol(unsigned v, float* c) {
  ImVec4 f = ImGui::ColorConvertU32ToFloat4(v);
  c[0] = f.x; c[1] = f.y; c[2] = f.z; c[3] = f.w;
}
ro::RoSkinConfig ReadSkinCfg(const YAML::Node& n) {
  ro::RoSkinConfig c;
  c.title_brightness = n["bright"].as<float>(c.title_brightness);
  c.alpha = n["alpha"].as<float>(c.alpha);
  if (n["body"]) UnpackCol(n["body"].as<unsigned>(0), c.body_col);
  if (n["border"]) UnpackCol(n["border"].as<unsigned>(0), c.border_col);
  if (n["titletx"]) UnpackCol(n["titletx"].as<unsigned>(0), c.title_text);
  if (n["bodytx"]) UnpackCol(n["bodytx"].as<unsigned>(0), c.body_text);
  if (n["tab"]) UnpackCol(n["tab"].as<unsigned>(0), c.tab_col);
  if (n["tabinact"]) UnpackCol(n["tabinact"].as<unsigned>(0), c.tab_inact);
  if (n["input"]) UnpackCol(n["input"].as<unsigned>(0), c.input_col);
  if (n["header"]) UnpackCol(n["header"].as<unsigned>(0), c.header_col);
  if (n["slot"]) UnpackCol(n["slot"].as<unsigned>(0), c.slot_col);
  if (n["doll"]) UnpackCol(n["doll"].as<unsigned>(0), c.doll_col);
  if (n["card"]) UnpackCol(n["card"].as<unsigned>(0), c.card_col);
  if (n["cardhead"]) UnpackCol(n["cardhead"].as<unsigned>(0), c.card_head_col);
  if (n["cardtx"]) UnpackCol(n["cardtx"].as<unsigned>(0), c.card_head_text);
  return c;
}
}  // namespace

// Item-link icon injection moved to plugins/chat.cc (ChatTweaks).

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
          // Same item re-clicked while visible = toggle-close (no native 0x06).
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
  const int ret = g_item_desc_wnd_orig(ecx, nullptr, p1, p2, p3, p4, p5, p6);
  // Enriched descriptions (Option A) : cacher la fenêtre native DÈS qu'elle est
  // posée/affichée (msg 0x18 set-item, 0x22 restore-pos) -> l'utilisateur ne voit
  // jamais la native (sinon flicker de ~100ms le temps du OnTick throttlé).
  if (p2 == 0x18 || p2 == 0x22) {
    if (auto* idt = Bourgeon::Instance().item_desc())
      idt->HideNativeDescWindows();  // item 0xc + comparaison 0xea (déjà créée)
  }
  return ret;
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
  // LogInfo("[MoonlightUi] loading item names from {}", path);

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
  // LogInfo("[MoonlightUi] loaded {} item names", item_names_.size());
}

MoonlightUi::MoonlightUi() {
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodeFromServer);
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodePresetList);
  // Observe the standard map-move packet to learn the current map name.
  Bourgeon::Instance().RegisterObserveOpcode(kOpcodeMapMove, kMapNameLen);
  FindChatBgSites();
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
  // NB : hook de la fenêtre skill (0x2e) RETIRÉ — il crashait le chemin natif
  // du message 0x3d. Repro skill désactivée (kSkillWindowEnabled) en attendant
  // une approche sûre (inspection live du rich-text natif).
}

// ── Chat background colours ───────────────────────────────────────────────

namespace {
// Static description of every chat-background site we patch.  Wildcards cover
// the 4-byte ARGB immediate so the search works whether or not a WARP binary
// patch was already applied to the exe.
struct ChatBgSiteDesc {
  int                  group;        // ChatBgGroupId
  std::vector<uint8_t> bytes;
  const char*          mask;
  size_t               imm_off;      // offset of the 4-byte ARGB inside the match
  uint32_t             heap_vtable;  // 0 = no heap recolour for this site
  uint32_t             heap_field;
};

const ChatBgSiteDesc kChatBgSites[] = {
  // ── Main chat panel ──────────────────────────────────────────────────────
  // Site 1: UINewChatWnd ctor  MOV [reg+0xD8], imm32   (colour stored in obj+0xD8)
  {0, {0xC7, 0x00, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      "x?xxxx????", 6, 0x01037F80, 0xD8},
  // Site 2: selected-tab colour  CMP / MOV ECX,imm32 / MOV EAX,0x99000000 / CMOVZ
  {0, {0x3B, 0x9E, 0x14, 0x01, 0x00, 0x00, 0xB9, 0x00, 0x00, 0x00, 0x00,
       0xB8, 0x00, 0x00, 0x00, 0x99, 0x0F, 0x44, 0xC1},
      "xxxxxxx????xxxxxxxx", 7, 0, 0},
  // ── Detached chat windows ────────────────────────────────────────────────
  // Site 3: outer border  MOV EAX,[ESI+0xEC] / PUSH imm32 / PUSH [ESI+0xE8]
  {1, {0x8B, 0x86, 0xEC, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xB6, 0xE8, 0x00, 0x00, 0x00},
      "xxxxxxx????xxxxxx", 7, 0, 0},
  // Site 8: UISubChatHisWnd ctor  MOV [ESI+0xD4], imm32 / MOV [ESI+0xC4], EAX
  {1, {0xC7, 0x86, 0xD4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x89, 0x86, 0xC4, 0x00, 0x00, 0x00},
      "xxxxxx????xxxxxx", 6, 0x01037EA8, 0xD4},
  // ── Whisper (1:1) window ──────────────────────────────────────────────────
  // Site 6: MOV EAX,[ESI+0x14] / PUSH imm32 / PUSH [ESI+0x18] / SUB EAX,2
  {2, {0x8B, 0x46, 0x14, 0x68, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x76, 0x18,
       0x83, 0xE8, 0x02},
      "xxxx????xxxxxx", 4, 0, 0},
};
}  // namespace

void MoonlightUi::FindChatBgSites() {
  chat_bg_[kChatBgMain].label        = "Main chat";
  chat_bg_[kChatBgMain].yaml_key     = "chat_bg";          // kept for back-compat
  chat_bg_[kChatBgDetached].label    = "Detached windows";
  chat_bg_[kChatBgDetached].yaml_key = "chat_bg_detached";
  chat_bg_[kChatBgWhisper].label     = "Whisper (1:1)";
  chat_bg_[kChatBgWhisper].yaml_key  = "chat_bg_whisper";

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

  // LogInfo("[MoonlightUi] chat_bg: scanning .text [0x{:08X} .. +0x{:X}] for {} site(s)",
          // reinterpret_cast<uint32_t>(text_start), text_size,
          // static_cast<int>(sizeof(kChatBgSites) / sizeof(kChatBgSites[0])));

  int site_idx = 0;
  for (const ChatBgSiteDesc& d : kChatBgSites) {
    const char* gname = chat_bg_[d.group].label;
    BytePattern pat(d.bytes, d.mask);
    auto* found = static_cast<uint8_t*>(pat.Search(text_start, text_size));
    if (!found) {
      LogError("[MoonlightUi] chat_bg: site #{} (group {} '{}') NOT FOUND",
               site_idx, d.group, gname);
      ++site_idx;
      continue;
    }
    auto* imm = reinterpret_cast<uint32_t*>(found + d.imm_off);

    // Make the immediate field writable (one VirtualProtect, never restored).
    DWORD old_protect;
    VirtualProtect(imm, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &old_protect);

    ChatBgGroup& g = chat_bg_[d.group];
    g.instrs.push_back(imm);
    if (d.heap_vtable) g.heap.push_back({d.heap_vtable, d.heap_field});
    chat_bg_found_ = true;

    // LogInfo("[MoonlightUi] chat_bg: site #{} (group {} '{}') found @ VA 0x{:08X}, "
            // "imm @ +{} = 0x{:08X}{}",
            // site_idx, d.group, gname, reinterpret_cast<uint32_t>(found),
            // d.imm_off, *imm,
            // d.heap_vtable ? " [+heap recolour]" : "");
    ++site_idx;
  }

  // Seed each picker from the colour currently in its first immediate, and log a
  // per-group summary so missing sites are obvious in the log.
  for (int i = 0; i < kChatBgCount; ++i) {
    ChatBgGroup& g = chat_bg_[i];
    if (!g.instrs.empty()) PickerFromArgb(g.color, *g.instrs.front());
    // LogInfo("[MoonlightUi] chat_bg group {} '{}': {} instr site(s), {} heap target(s)",
            // i, g.label, static_cast<int>(g.instrs.size()),
            // static_cast<int>(g.heap.size()));
  }
}

void MoonlightUi::ApplyChatBg(ChatBgGroup& g, uint32_t argb, bool walk_heap) {
  for (uint32_t* p : g.instrs) {
    *p = argb;
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(uint32_t));
  }
  if (walk_heap && !g.heap.empty()) PatchChatBgObjects(g, argb);
}

void MoonlightUi::PatchChatBgObjects(const ChatBgGroup& g, uint32_t argb) {
  HANDLE heap = GetProcessHeap();
  if (!heap || !HeapLock(heap)) return;

  PROCESS_HEAP_ENTRY entry = {};
  int count = 0;
  while (HeapWalk(heap, &entry)) {
    if (!(entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
    // Window objects start with their vtable pointer.
    const auto* vtable_ptr = static_cast<const uint32_t*>(entry.lpData);
    for (const ChatBgHeapTarget& t : g.heap) {
      if (entry.cbData < t.field_off + sizeof(uint32_t)) continue;
      if (*vtable_ptr != t.vtable) continue;
      *reinterpret_cast<uint32_t*>(
          static_cast<uint8_t*>(entry.lpData) + t.field_off) = argb;
      ++count;
    }
  }

  HeapUnlock(heap);
  // LogInfo("[MoonlightUi] chat_bg[{}]: recoloured {} live object(s)", g.yaml_key, count);
}

uint32_t MoonlightUi::ArgbFromPicker(const float c[4]) {
  const uint32_t r = static_cast<uint32_t>(c[0] * 255.0f + 0.5f) & 0xFF;
  const uint32_t g = static_cast<uint32_t>(c[1] * 255.0f + 0.5f) & 0xFF;
  const uint32_t b = static_cast<uint32_t>(c[2] * 255.0f + 0.5f) & 0xFF;
  const uint32_t a = static_cast<uint32_t>(c[3] * 255.0f + 0.5f) & 0xFF;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

void MoonlightUi::PickerFromArgb(float c[4], uint32_t argb) {
  c[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f; // R
  c[1] = static_cast<float>((argb >>  8) & 0xFF) / 255.0f; // G
  c[2] = static_cast<float>( argb        & 0xFF) / 255.0f; // B
  c[3] = static_cast<float>((argb >> 24) & 0xFF) / 255.0f; // A
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

    for (ChatBgGroup& g : chat_bg_) {
      const std::string hex = ui[g.yaml_key].as<std::string>("");
      if (hex.size() != 8) continue;
      const uint32_t argb = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
      PickerFromArgb(g.color, argb);
      if (!g.instrs.empty()) ApplyChatBg(g, argb, true);
      // LogInfo("[MoonlightUi] loaded {} 0x{:08X}", g.yaml_key, argb);
    }

    ui_collapsed_         = ui["ui_collapsed"].as<bool>(false);
    show_alootid_overlay_ = ui["alootid_overlay"].as<bool>(false);
    if (auto* idt = Bourgeon::Instance().item_desc()) {
      idt->show_item_panel()  = ui["itemdesc_show_item"].as<bool>(true);
      idt->show_skill_panel() = ui["itemdesc_show_skill"].as<bool>(true);
      idt->cmp_show_equipped() = ui["itemdesc_compare"].as<bool>(true);
      idt->desc_spawn_at_cursor() =
          ui["itemdesc_spawn_cursor"].as<bool>(true);
      idt->desc_anchor()   = ui["itemdesc_anchor"].as<int>(0);
      idt->desc_offset_x() = ui["itemdesc_off_x"].as<int>(12);
      idt->desc_offset_y() = ui["itemdesc_off_y"].as<int>(12);
    }
    if (auto* br = Bourgeon::Instance().bug_report())
      br->enabled() = ui["bugreport_button"].as<bool>(true);
    mainchat_preset_bar_  = ui["mainchat_preset_bar"].as<bool>(false);
    log_level_            = ui["log_level"].as<std::string>("info");

    chat_width_enabled_ = ui["chat_width_enabled"].as<bool>(false);
    chat_width_px_      = ui["chat_width"].as<int>(800);
    if (chat_width_px_ < 320)  chat_width_px_ = 320;
    if (chat_width_px_ > 1200) chat_width_px_ = 1200;
    chat::SetCustomWidth(chat_width_enabled_, chat_width_px_);
    chat_timestamps_ = ui["chat_timestamps"].as<bool>(false);
    chat::SetTimestamps(chat_timestamps_);
    chat_item_icons_ = ui["chat_item_icons"].as<bool>(true);
    chat::SetItemIcons(chat_item_icons_);
    LogConsole::instance().SetLevel(log_level_);
    apply_collapse_ = true;

    if (auto* dps = Bourgeon::Instance().dps_meter()) {
      dps->show_ground_dmg_in_chat_ = ui["dps_ground_dmg_chat"].as<bool>(true);
      dps->locked_ = ui["dps_locked"].as<bool>(false);
      dps->bg_alpha_ = ui["dps_bg_alpha"].as<float>(0.90f);
      auto load_dps_col = [&](const char* key, float c[4]) {
        const std::string hex = ui[key].as<std::string>("");
        if (hex.size() == 8)
          PickerFromArgb(c, static_cast<uint32_t>(std::stoul(hex, nullptr, 16)));
      };
      load_dps_col("dps_text_color", dps->text_color_);
      load_dps_col("dps_plot_color", dps->plot_color_);
      dps->visible_             = ui["dps_visible"].as<bool>(true);
      dps->slot_ms_             = ui["dps_slot_ms"].as<int>(200);
      dps->dps_window_secs_     = ui["dps_window_secs"].as<int>(10);
      dps->combat_timeout_secs_ = ui["dps_combat_timeout_secs"].as<int>(5);
    }

    if (auto* eb = Bourgeon::Instance().basic_info()) {
      eb->visible_   = ui["expbar_visible"].as<bool>(true);
      eb->locked_    = ui["expbar_locked"].as<bool>(false);
      eb->sticky_    = ui["expbar_sticky"].as<bool>(false);
      eb->text_mode_ = ui["expbar_text"].as<int>(1);
      eb->vertical_  = ui["expbar_vertical"].as<bool>(false);
      eb->border_    = ui["expbar_border"].as<bool>(true);
      eb->rounding_  = ui["expbar_rounding"].as<float>(4.0f);
      auto load_color = [&](const std::string& key, float c[4]) {
        const std::string hex = ui[key].as<std::string>("");
        if (hex.size() == 8)
          PickerFromArgb(c, static_cast<uint32_t>(std::stoul(hex, nullptr, 16)));
      };
      for (int i = 0; i < BasicInfoTweaks::kBarCount; ++i) {
        const std::string p =
            std::string("expbar_") + BasicInfoTweaks::kBarKeys[i] + "_";
        auto& b = eb->bars_[i];
        b.show = ui[p + "show"].as<bool>(true);
        b.x = ui[p + "x"].as<int>(b.x);
        b.y = ui[p + "y"].as<int>(b.y);
        b.w = ui[p + "w"].as<int>(b.w);
        b.h = ui[p + "h"].as<int>(b.h);
        load_color(p + "color", b.fill);
      }
      load_color("expbar_bg_color", eb->bg_color_);

      // Status portrait (part of the Basic Info tweaks): per-element layout.
      eb->portrait_visible_         = ui["portrait_visible"].as<bool>(false);
      eb->portrait_locked_          = ui["portrait_locked"].as<bool>(false);
      eb->portrait_hide_basic_info_ = ui["portrait_hide_basic_info"].as<bool>(false);
      eb->portrait_border_          = ui["portrait_border"].as<bool>(false);
      eb->portrait_head_sprite_     = ui["portrait_head_sprite"].as<bool>(true);
      eb->portrait_head_only_       = ui["portrait_head_only"].as<bool>(true);
      eb->portrait_debug_log_       = ui["portrait_debug_log"].as<bool>(false);
      eb->portrait_head_zoom_       = ui["portrait_head_zoom"].as<float>(1.0f);
      eb->portrait_head_offx_       = ui["portrait_head_offx"].as<float>(0.0f);
      eb->portrait_head_offy_       = ui["portrait_head_offy"].as<float>(0.0f);
      eb->portrait_anim_            = ui["portrait_anim"].as<int>(4);
      eb->portrait_dir_             = ui["portrait_dir"].as<int>(0);
      eb->portrait_animate_         = ui["portrait_animate"].as<bool>(true);
      eb->portrait_show_garment_    = ui["portrait_show_garment"].as<bool>(true);
      // Hat effects (.str) : rendu automatique et toujours actif (aucun réglage persisté).
      for (int i = 0; i < BasicInfoTweaks::kPortCount; ++i) {
        const std::string p =
            std::string("portrait_") + BasicInfoTweaks::kPortKeys[i] + "_";
        auto& e = eb->ports_[i];
        e.show     = ui[p + "show"].as<bool>(e.show);
        e.x        = ui[p + "x"].as<int>(e.x);
        e.y        = ui[p + "y"].as<int>(e.y);
        e.w        = ui[p + "w"].as<int>(e.w);
        e.h        = ui[p + "h"].as<int>(e.h);
        e.rounding = ui[p + "rounding"].as<float>(e.rounding);
        load_color(p + "bg", e.bg);
        load_color(p + "fg", e.fg);
      }
    }

    // Global alignment grid. Reads grid_*, falling back to the legacy
    // expbar_grid_* keys so existing settings files keep working (they get
    // rewritten under the new keys on the next save).
    grid_.show = ui["grid_show"].as<bool>(ui["expbar_grid_show"].as<bool>(false));
    grid_.snap = ui["grid_snap"].as<bool>(ui["expbar_grid_snap"].as<bool>(false));
    grid_.size = ui["grid_size"].as<int>(ui["expbar_grid_size"].as<int>(32));
    {
      std::string hex = ui["grid_color"].as<std::string>("");
      if (hex.size() != 8) hex = ui["expbar_grid_color"].as<std::string>("");
      if (hex.size() == 8)
        PickerFromArgb(grid_.color,
                       static_cast<uint32_t>(std::stoul(hex, nullptr, 16)));
    }

    // STATUS window saved position (applied by StatusTweaks' msg-handler hook).
    StatusTweaks_SetSavedPos(ui["status_pos_x"].as<int>(INT_MIN),
                             ui["status_pos_y"].as<int>(INT_MIN));
    // EQUIP window saved position (applied by EquipTweaks' msg-handler hook).
    EquipTweaks_SetSavedPos(ui["equip_pos_x"].as<int>(INT_MIN),
                            ui["equip_pos_y"].as<int>(INT_MIN));
    // Generic per-window saved positions (WindowPosTweaks table: achievement,
    // bank, mail, ...). One "<key>_pos_x/y" pair each; applied on the next tick.
    for (int i = 0; i < WindowPosTweaks_Count(); ++i) {
      const std::string k = WindowPosTweaks_Key(i);
      WindowPosTweaks_SetSavedPos(i, ui[k + "_pos_x"].as<int>(INT_MIN),
                                  ui[k + "_pos_y"].as<int>(INT_MIN));
    }

    if (auto* mi = Bourgeon::Instance().menu_icons()) {
      mi->enabled_   = ui["menu_icons_enabled"].as<bool>(mi->enabled_);
      mi->edit_mode_ = ui["menu_icons_edit"].as<bool>(false);
      mi->saved_.clear();
      // Per-icon saved position/visibility under "menu_icons: { <name>: {...} }".
      // Stored here because the live icon list only exists once in-game; applied
      // later in MenuIconTweaks::BuildIconList.
      if (const YAML::Node icons = ui["menu_icons"]) {
        for (auto it = icons.begin(); it != icons.end(); ++it) {
          const std::string nm = it->first.as<std::string>("");
          if (nm.empty()) continue;
          MenuIconTweaks::IconSave s;
          s.x      = it->second["x"].as<int>(-1);
          s.y      = it->second["y"].as<int>(-1);
          s.hidden = it->second["hidden"].as<bool>(false);
          s.valid  = true;
          mi->saved_[nm] = s;
        }
      }
    }

    ro::SetFontEnabled(ui["malgun_font"].as<bool>(ro::IsFontEnabled()));
    ro::SetSkinEnabled(ui["ro_skin"].as<bool>(ro::IsSkinEnabled()));
    {
      auto& sc = ro::SkinConfig();
      sc.title_brightness = ui["ro_skin_bright"].as<float>(sc.title_brightness);
      sc.rounding = ui["ro_skin_rounding"].as<float>(sc.rounding);
      sc.alpha = ui["ro_skin_alpha"].as<float>(sc.alpha);
      auto load_col = [&](const char* key, float* c) {
        if (ui[key]) {
          ImVec4 f = ImGui::ColorConvertU32ToFloat4(ui[key].as<unsigned int>(0));
          c[0] = f.x; c[1] = f.y; c[2] = f.z; c[3] = f.w;
        }
      };
      load_col("ro_skin_body", sc.body_col);
      load_col("ro_skin_border", sc.border_col);
      load_col("ro_skin_titletx", sc.title_text);
      load_col("ro_skin_bodytx", sc.body_text);
      load_col("ro_skin_tab", sc.tab_col);
      load_col("ro_skin_tabinact", sc.tab_inact);
      load_col("ro_skin_input", sc.input_col);
      load_col("ro_skin_header", sc.header_col);
      load_col("ro_skin_slot", sc.slot_col);
      load_col("ro_skin_doll", sc.doll_col);
      load_col("ro_skin_card", sc.card_col);
      load_col("ro_skin_cardhead", sc.card_head_col);
      load_col("ro_skin_cardtx", sc.card_head_text);
    }
    g_ro_presets.clear();
    if (const YAML::Node ps = ui["ro_skin_presets"]) {
      for (auto it = ps.begin(); it != ps.end(); ++it) {
        RoPreset p;
        p.name = (*it)["name"].as<std::string>("");
        if (p.name.empty()) continue;
        p.cfg = ReadSkinCfg(*it);
        g_ro_presets.push_back(std::move(p));
      }
    }
    // Starter set au 1er lancement (aucun preset sauvegardé) : donne des thèmes
    // de départ que les joueurs peuvent Appliquer puis modifier.
    if (g_ro_presets.empty()) {
      auto setc = [](float* c, int r, int g, int b) {
        c[0] = r / 255.f; c[1] = g / 255.f; c[2] = b / 255.f; c[3] = 1.f;
      };
      g_ro_presets.push_back({"RO Classique", ro::RoSkinConfig{}});  // défauts natifs
      {
        ro::RoSkinConfig d;
        d.title_brightness = 0.90f;
        setc(d.body_col, 44, 46, 54);
        setc(d.border_col, 90, 94, 110);
        setc(d.body_text, 226, 228, 235);
        setc(d.title_text, 255, 255, 255);
        setc(d.tab_col, 90, 120, 190);
        setc(d.tab_inact, 70, 74, 86);
        setc(d.input_col, 64, 66, 76);
        setc(d.header_col, 58, 60, 70);
        setc(d.slot_col, 64, 66, 76);
        setc(d.doll_col, 54, 56, 66);
        setc(d.card_col, 54, 56, 66);
        setc(d.card_head_col, 34, 36, 44);
        setc(d.card_head_text, 226, 228, 235);
        g_ro_presets.push_back({"Sombre", d});
      }
      {
        ro::RoSkinConfig d;
        setc(d.body_col, 244, 236, 218);
        setc(d.border_col, 176, 150, 110);
        setc(d.body_text, 60, 44, 24);
        setc(d.title_text, 40, 28, 12);
        setc(d.tab_col, 196, 166, 120);
        setc(d.tab_inact, 226, 214, 190);
        setc(d.input_col, 232, 222, 200);
        setc(d.header_col, 224, 210, 184);
        setc(d.slot_col, 232, 222, 200);
        setc(d.doll_col, 240, 232, 214);
        setc(d.card_col, 250, 244, 228);
        setc(d.card_head_col, 150, 120, 80);
        setc(d.card_head_text, 250, 244, 230);
        g_ro_presets.push_back({"Sepia", d});
      }
    }
    if (auto* iv = Bourgeon::Instance().inventory_viewer())
      iv->imgui_enabled_ = ui["inventory_imgui"].as<bool>(iv->imgui_enabled_);
    if (auto* stg = Bourgeon::Instance().storage_tweaks()) {
      stg->imgui_enabled_ = ui["storage_imgui"].as<bool>(stg->imgui_enabled_);
      // Favoris storage (client-side, keyés par id d'item).
      if (const YAML::Node favs = ui["storage_favorites"]) {
        stg->favorites_.clear();
        for (const YAML::Node& f : favs) {
          const uint32_t id = f.as<uint32_t>(0);
          if (id != 0) stg->favorites_.insert(id);
        }
      }
    }
    if (auto* cs = Bourgeon::Instance().cashshop_tweaks())
      cs->imgui_enabled_ = ui["cashshop_imgui"].as<bool>(cs->imgui_enabled_);
    if (auto* sh = Bourgeon::Instance().shop_tweaks())
      sh->imgui_enabled_ = ui["shop_imgui"].as<bool>(sh->imgui_enabled_);
    if (auto* nd = Bourgeon::Instance().npc_dialog_tweaks()) {
      nd->imgui_enabled_ = ui["npc_dialog_imgui"].as<bool>(nd->imgui_enabled_);
      nd->menu_search_ = ui["npc_menu_search"].as<bool>(nd->menu_search_);
    }
    if (auto* cse = Bourgeon::Instance().character_sheet())
      cse->imgui_enabled_ = ui["charsheet_imgui"].as<bool>(cse->imgui_enabled_);
    if (auto* sb = Bourgeon::Instance().skill_bar()) {
      sb->enabled_    = ui["skillbar_enabled"].as<bool>(sb->enabled_);
      sb->bilinear_   = ui["skillbar_bilinear"].as<bool>(sb->bilinear_);
      sb->clickthrough_ = ui["skillbar_clickthrough"].as<bool>(sb->clickthrough_);
      sb->show_keys_  = ui["skillbar_show_keys"].as<bool>(sb->show_keys_);
      sb->bold_text_  = ui["skillbar_bold_text"].as<bool>(sb->bold_text_);
      const float legacy_scale = ui["skillbar_text_scale"].as<float>(1.0f);  // ancienne clé unique (repli)
      sb->key_scale_   = ui["skillbar_key_scale"].as<float>(legacy_scale);
      sb->count_scale_ = ui["skillbar_count_scale"].as<float>(legacy_scale);
      // 3 barres fixes (0=Onglet1, 1=Onglet2, 2=Items) : clés skillbarN_*
      for (int b = 0; b < SkillBarTweaks::kBarCount; ++b) {
        auto& bc = sb->bars_[b];
        const std::string p = "skillbar" + std::to_string(b) + "_";
        bc.visible    = ui[p + "visible"].as<bool>(bc.visible);
        bc.x          = ui[p + "x"].as<int>(bc.x);
        bc.y          = ui[p + "y"].as<int>(bc.y);
        bc.columns    = ui[p + "columns"].as<int>(bc.columns);
        bc.first_slot = ui[p + "first"].as<int>(bc.first_slot);
        bc.slot_count = ui[p + "slots"].as<int>(bc.slot_count);
        bc.icon_size  = ui[p + "size"].as<float>(bc.icon_size);
        bc.spacing    = ui[p + "spacing"].as<float>(bc.spacing);
      }
      for (int i = 0; i < SkillBarTweaks::kItemSlotMax; ++i)  // contenu persisté barre d'items (nameids)
        sb->item_slots_[i] = ui["skillbar_item" + std::to_string(i)].as<uint32_t>(sb->item_slots_[i]);
      auto load_sbcol = [&](const char* key, float c[4]) {
        const std::string hex = ui[key].as<std::string>("");
        if (hex.size() == 8)
          PickerFromArgb(c, static_cast<uint32_t>(std::stoul(hex, nullptr, 16)));
      };
      load_sbcol("skillbar_col_frame",    sb->col_frame_);
      load_sbcol("skillbar_col_skill",    sb->col_skill_);
      load_sbcol("skillbar_col_item",     sb->col_item_);
      load_sbcol("skillbar_col_empty",    sb->col_empty_);
      load_sbcol("skillbar_col_border",   sb->col_border_);
      load_sbcol("skillbar_col_borderhi", sb->col_borderhi_);
      load_sbcol("skillbar_col_keytext",  sb->col_keytext_);
      load_sbcol("skillbar_col_count",    sb->col_count_);
      load_sbcol("skillbar_col_textout",  sb->col_textout_);
    }

    if (auto* si = Bourgeon::Instance().status_icons()) {
      StatusIconConfig& c = si->config();
      c.enabled        = ui["statusicon_enabled"].as<bool>(c.enabled);
      c.corner         = ui["statusicon_corner"].as<int>(c.corner);
      c.margin_x       = ui["statusicon_margin_x"].as<int>(c.margin_x);
      c.margin_y       = ui["statusicon_margin_y"].as<int>(c.margin_y);
      c.step_dir       = ui["statusicon_step_dir"].as<int>(c.step_dir);
      c.wrap_dir       = ui["statusicon_wrap_dir"].as<int>(c.wrap_dir);
      c.per_line       = ui["statusicon_per_line"].as<int>(c.per_line);
      c.icon_pitch     = ui["statusicon_icon_pitch"].as<int>(c.icon_pitch);
      c.line_pitch     = ui["statusicon_line_pitch"].as<int>(c.line_pitch);
      c.sort_mode      = ui["statusicon_sort_mode"].as<int>(c.sort_mode);
      c.show_remaining = ui["statusicon_show_remaining"].as<bool>(c.show_remaining);
      c.time_bg        = ui["statusicon_time_bg"].as<bool>(c.time_bg);
      c.icon_alpha     = ui["statusicon_icon_alpha"].as<int>(c.icon_alpha);
      si->MarkDirty();
    }

    if (auto* qt = Bourgeon::Instance().quest_tracker()) {
      QuestTrackerConfig& c = qt->config();
      c.enabled        = ui["questtracker_enabled"].as<bool>(c.enabled);
      c.show_titlebar  = ui["questtracker_show_titlebar"].as<bool>(c.show_titlebar);
      c.locked         = ui["questtracker_locked"].as<bool>(c.locked);
      c.pos_x          = ui["questtracker_pos_x"].as<int>(c.pos_x);
      c.pos_y          = ui["questtracker_pos_y"].as<int>(c.pos_y);
      c.width          = ui["questtracker_width"].as<int>(c.width);
      c.max_quests     = ui["questtracker_max_quests"].as<int>(c.max_quests);
      c.title_rgb      = ui["questtracker_title_rgb"].as<int>(c.title_rgb);
      c.desc_rgb       = ui["questtracker_desc_rgb"].as<int>(c.desc_rgb);
      c.hunt_rgb       = ui["questtracker_hunt_rgb"].as<int>(c.hunt_rgb);
      c.font_scale     = ui["questtracker_font_scale"].as<int>(c.font_scale);
      c.show_bg        = ui["questtracker_show_bg"].as<bool>(c.show_bg);
      c.bg_alpha       = ui["questtracker_bg_alpha"].as<int>(c.bg_alpha);
      c.show_objective = ui["questtracker_show_objective"].as<bool>(c.show_objective);
    }

    if (auto* st = Bourgeon::Instance().settings_tweaks()) {
      D3D9PostFx& g = st->fx();
      g.enabled     = ui["fx_enabled"].as<bool>(g.enabled);
      g.brightness  = ui["fx_brightness"].as<float>(g.brightness);
      g.contrast    = ui["fx_contrast"].as<float>(g.contrast);
      g.gamma       = ui["fx_gamma"].as<float>(g.gamma);
      g.saturation  = ui["fx_saturation"].as<float>(g.saturation);
      g.temperature = ui["fx_temperature"].as<float>(g.temperature);
      g.filter      = ui["fx_filter"].as<int>(g.filter);
      g.vignette    = ui["fx_vignette"].as<float>(g.vignette);
      g.grain       = ui["fx_grain"].as<float>(g.grain);
      g.aberration  = ui["fx_aberration"].as<float>(g.aberration);
      g.sharpen     = ui["fx_sharpen"].as<float>(g.sharpen);
      g.fxaa        = ui["fx_fxaa"].as<bool>(g.fxaa);
      g.fxaa_strength = ui["fx_fxaa_strength"].as<float>(g.fxaa_strength);
      st->fps_overlay() = ui["fps_overlay"].as<bool>(false);
      st->zoom_enabled() = ui["cam_zoom_enabled"].as<bool>(false);
      st->zoom_factor()  = ui["cam_zoom_factor"].as<float>(1.0f);
      st->zoom_speed()   = ui["cam_zoom_speed"].as<float>(1.0f);
      st->tex_filter()   = ui["tex_filter"].as<int>(0);
      st->gopt_x()       = ui["game_option_pos_x"].as<int>(INT_MIN);
      st->gopt_y()       = ui["game_option_pos_y"].as<int>(INT_MIN);
      st->esc_x()        = ui["esc_option_pos_x"].as<int>(INT_MIN);
      st->esc_y()        = ui["esc_option_pos_y"].as<int>(INT_MIN);
      st->Apply();  // push to the d3d9 post-process layer
    }

    chat_bg_presets_.clear();
    if (const YAML::Node presets = ui["chat_bg_presets"]) {
      for (const YAML::Node& p : presets) {
        const std::string name  = p["name"].as<std::string>("");
        const std::string color = p["color"].as<std::string>("");
        if (name.empty() || color.size() != 8) continue;
        const uint32_t argb = static_cast<uint32_t>(std::stoul(color, nullptr, 16));
        chat_bg_presets_.push_back({name, argb});
      }
    }

    // Presets d'équipement (loadouts nommés, par CID) — possédés par CharacterSheet.
    if (auto* cse = Bourgeon::Instance().character_sheet()) {
      auto& presets = cse->equip_presets();
      presets.clear();
      if (const YAML::Node eps = ui["equip_presets"]) {
        for (const YAML::Node& pn : eps) {
          EquipPreset ep;
          ep.cid  = pn["cid"].as<uint32_t>(0);
          ep.name = pn["name"].as<std::string>("");
          if (ep.name.empty()) continue;
          ep.hotkeyVk = pn["hkvk"].as<int>(0);
          ep.hkCtrl   = pn["hkc"].as<bool>(false);
          ep.hkAlt    = pn["hka"].as<bool>(false);
          ep.hkShift  = pn["hks"].as<bool>(false);
          if (const YAML::Node items = pn["items"]) {
            for (const YAML::Node& it : items) {
              EquipPresetItem pi;
              pi.nameid   = it["id"].as<uint32_t>(0);
              pi.refine   = it["refine"].as<int>(0);
              pi.grade    = it["grade"].as<int>(0);
              pi.leftHand = it["left"].as<bool>(false);
              if (const YAML::Node cards = it["cards"])
                for (int c = 0; c < 4 && c < static_cast<int>(cards.size()); ++c)
                  pi.cards[c] = cards[c].as<uint32_t>(0);
              ep.items.push_back(pi);
            }
          }
          presets.push_back(std::move(ep));
        }
      }
    }
  } catch (const std::exception& e) {
    LogError("[MoonlightUi] failed to parse {}: {}", path, e.what());
  }
}

void MoonlightUi::SaveSettings() {
  auto* dps = Bourgeon::Instance().dps_meter();
  char dps_text_col[9] = "FFFFCC33", dps_plot_col[9] = "FFFFCC33";
  if (dps) {
    std::snprintf(dps_text_col, sizeof(dps_text_col), "%08X",
                  ArgbFromPicker(dps->text_color_));
    std::snprintf(dps_plot_col, sizeof(dps_plot_col), "%08X",
                  ArgbFromPicker(dps->plot_color_));
  }

  auto* eb = Bourgeon::Instance().basic_info();
  char eb_bg_col[9] = "B30D0D12";
  if (eb)
    std::snprintf(eb_bg_col, sizeof(eb_bg_col), "%08X",
                  ArgbFromPicker(eb->bg_color_));
  // Global alignment grid colour (owned by MoonlightUi, not basic_info).
  char grid_col[9];
  std::snprintf(grid_col, sizeof(grid_col), "%08X",
                ArgbFromPicker(grid_.color));

  // ItemDescTweaks toggles (owned by the plugin) — panels + Comparer + placement.
  bool itemdesc_show_item = true, itemdesc_show_skill = true;
  bool itemdesc_compare = true, itemdesc_spawn_cursor = true;
  int  itemdesc_anchor = 0, itemdesc_off_x = 12, itemdesc_off_y = 12;
  if (auto* idt = Bourgeon::Instance().item_desc()) {
    itemdesc_show_item    = idt->show_item_panel();
    itemdesc_show_skill   = idt->show_skill_panel();
    itemdesc_compare      = idt->cmp_show_equipped();
    itemdesc_spawn_cursor = idt->desc_spawn_at_cursor();
    itemdesc_anchor       = idt->desc_anchor();
    itemdesc_off_x        = idt->desc_offset_x();
    itemdesc_off_y        = idt->desc_offset_y();
  }
  bool bugreport_button = true;
  if (auto* br = Bourgeon::Instance().bug_report())
    bugreport_button = br->enabled();

  YAML::Emitter out;
  out << YAML::BeginMap
      << YAML::Key << "moonlight_ui"
      << YAML::Value << YAML::BeginMap;
  for (const ChatBgGroup& g : chat_bg_) {
    char hex[9];
    std::snprintf(hex, sizeof(hex), "%08X", ArgbFromPicker(g.color));
    out   << YAML::Key << g.yaml_key << YAML::Value << hex;
  }
  out     << YAML::Key << "ui_collapsed"          << YAML::Value << ui_collapsed_
        << YAML::Key << "log_level"            << YAML::Value << log_level_
        << YAML::Key << "alootid_overlay"      << YAML::Value << show_alootid_overlay_
        << YAML::Key << "itemdesc_show_item"   << YAML::Value << itemdesc_show_item
        << YAML::Key << "itemdesc_show_skill"  << YAML::Value << itemdesc_show_skill
        << YAML::Key << "itemdesc_compare"     << YAML::Value << itemdesc_compare
        << YAML::Key << "itemdesc_spawn_cursor" << YAML::Value << itemdesc_spawn_cursor
        << YAML::Key << "itemdesc_anchor"      << YAML::Value << itemdesc_anchor
        << YAML::Key << "itemdesc_off_x"       << YAML::Value << itemdesc_off_x
        << YAML::Key << "itemdesc_off_y"       << YAML::Value << itemdesc_off_y
        << YAML::Key << "bugreport_button"     << YAML::Value << bugreport_button
        << YAML::Key << "mainchat_preset_bar"  << YAML::Value << mainchat_preset_bar_
        << YAML::Key << "chat_width_enabled"   << YAML::Value << chat_width_enabled_
        << YAML::Key << "chat_width"           << YAML::Value << chat_width_px_
        << YAML::Key << "chat_timestamps"      << YAML::Value << chat_timestamps_
        << YAML::Key << "chat_item_icons"      << YAML::Value << chat_item_icons_
        << YAML::Key << "dps_ground_dmg_chat"  << YAML::Value
            << (Bourgeon::Instance().dps_meter()
                    ? Bourgeon::Instance().dps_meter()->show_ground_dmg_in_chat_
                    : true)
        << YAML::Key << "dps_locked" << YAML::Value
            << (Bourgeon::Instance().dps_meter()
                    ? Bourgeon::Instance().dps_meter()->locked_
                    : false)
        << YAML::Key << "dps_bg_alpha"   << YAML::Value << (dps ? dps->bg_alpha_ : 0.90f)
        << YAML::Key << "dps_text_color" << YAML::Value << dps_text_col
        << YAML::Key << "dps_plot_color" << YAML::Value << dps_plot_col
        << YAML::Key << "dps_visible"             << YAML::Value << (dps ? dps->visible_ : true)
        << YAML::Key << "dps_slot_ms"             << YAML::Value << (dps ? dps->slot_ms_ : 200)
        << YAML::Key << "dps_window_secs"         << YAML::Value << (dps ? dps->dps_window_secs_ : 10)
        << YAML::Key << "dps_combat_timeout_secs" << YAML::Value << (dps ? dps->combat_timeout_secs_ : 5);

  // EXP/HP/SP bar settings (BasicInfoTweaks)
  out << YAML::Key << "expbar_visible"  << YAML::Value << (eb ? eb->visible_ : true)
      << YAML::Key << "expbar_locked"   << YAML::Value << (eb ? eb->locked_ : false)
      << YAML::Key << "expbar_sticky"   << YAML::Value << (eb ? eb->sticky_ : false)
      << YAML::Key << "expbar_text"     << YAML::Value << (eb ? eb->text_mode_ : 1)
      << YAML::Key << "expbar_vertical" << YAML::Value << (eb ? eb->vertical_ : false)
      << YAML::Key << "expbar_border"   << YAML::Value << (eb ? eb->border_ : true)
      << YAML::Key << "expbar_rounding" << YAML::Value << (eb ? eb->rounding_ : 4.0f)
      << YAML::Key << "expbar_bg_color" << YAML::Value << eb_bg_col
      << YAML::Key << "grid_show"  << YAML::Value << grid_.show
      << YAML::Key << "grid_snap"  << YAML::Value << grid_.snap
      << YAML::Key << "grid_size"  << YAML::Value << grid_.size
      << YAML::Key << "grid_color" << YAML::Value << grid_col
      << YAML::Key << "status_pos_x" << YAML::Value << StatusTweaks_SavedX()
      << YAML::Key << "status_pos_y" << YAML::Value << StatusTweaks_SavedY()
      << YAML::Key << "equip_pos_x" << YAML::Value << EquipTweaks_SavedX()
      << YAML::Key << "equip_pos_y" << YAML::Value << EquipTweaks_SavedY();
  // Generic per-window saved positions (WindowPosTweaks table).
  for (int i = 0; i < WindowPosTweaks_Count(); ++i) {
    const std::string k = WindowPosTweaks_Key(i);
    out << YAML::Key << (k + "_pos_x") << YAML::Value << WindowPosTweaks_X(i)
        << YAML::Key << (k + "_pos_y") << YAML::Value << WindowPosTweaks_Y(i);
  }
  if (eb) {
    for (int i = 0; i < BasicInfoTweaks::kBarCount; ++i) {
      const std::string p =
          std::string("expbar_") + BasicInfoTweaks::kBarKeys[i] + "_";
      const auto& b = eb->bars_[i];
      char col[9];
      std::snprintf(col, sizeof(col), "%08X", ArgbFromPicker(b.fill));
      out << YAML::Key << (p + "show")  << YAML::Value << b.show
          << YAML::Key << (p + "x")     << YAML::Value << b.x
          << YAML::Key << (p + "y")     << YAML::Value << b.y
          << YAML::Key << (p + "w")     << YAML::Value << b.w
          << YAML::Key << (p + "h")     << YAML::Value << b.h
          << YAML::Key << (p + "color") << YAML::Value << col;
    }
  }

  // Status portrait settings (part of BasicInfoTweaks): per-element layout.
  if (eb) {
    out << YAML::Key << "portrait_visible"         << YAML::Value << eb->portrait_visible_
        << YAML::Key << "portrait_locked"          << YAML::Value << eb->portrait_locked_
        << YAML::Key << "portrait_hide_basic_info" << YAML::Value << eb->portrait_hide_basic_info_
        << YAML::Key << "portrait_border"          << YAML::Value << eb->portrait_border_
        << YAML::Key << "portrait_head_sprite"     << YAML::Value << eb->portrait_head_sprite_
        << YAML::Key << "portrait_head_only"       << YAML::Value << eb->portrait_head_only_
        << YAML::Key << "portrait_debug_log"       << YAML::Value << eb->portrait_debug_log_
        << YAML::Key << "portrait_head_zoom"       << YAML::Value << eb->portrait_head_zoom_
        << YAML::Key << "portrait_head_offx"       << YAML::Value << eb->portrait_head_offx_
        << YAML::Key << "portrait_head_offy"       << YAML::Value << eb->portrait_head_offy_
        << YAML::Key << "portrait_anim"            << YAML::Value << eb->portrait_anim_
        << YAML::Key << "portrait_dir"             << YAML::Value << eb->portrait_dir_
        << YAML::Key << "portrait_animate"         << YAML::Value << eb->portrait_animate_
        << YAML::Key << "portrait_show_garment"    << YAML::Value << eb->portrait_show_garment_;
    for (int i = 0; i < BasicInfoTweaks::kPortCount; ++i) {
      const std::string p =
          std::string("portrait_") + BasicInfoTweaks::kPortKeys[i] + "_";
      const auto& e = eb->ports_[i];
      char bg[9], fg[9];
      std::snprintf(bg, sizeof(bg), "%08X", ArgbFromPicker(e.bg));
      std::snprintf(fg, sizeof(fg), "%08X", ArgbFromPicker(e.fg));
      out << YAML::Key << (p + "show")     << YAML::Value << e.show
          << YAML::Key << (p + "x")        << YAML::Value << e.x
          << YAML::Key << (p + "y")        << YAML::Value << e.y
          << YAML::Key << (p + "w")        << YAML::Value << e.w
          << YAML::Key << (p + "h")        << YAML::Value << e.h
          << YAML::Key << (p + "rounding") << YAML::Value << e.rounding
          << YAML::Key << (p + "bg")       << YAML::Value << bg
          << YAML::Key << (p + "fg")       << YAML::Value << fg;
    }
  }

  {
    auto* mi = Bourgeon::Instance().menu_icons();
    out << YAML::Key << "menu_icons_enabled"
        << YAML::Value << (mi ? mi->enabled_ : false);
    out << YAML::Key << "menu_icons_edit"
        << YAML::Value << (mi ? mi->edit_mode_ : false);
    out << YAML::Key << "menu_icons" << YAML::Value << YAML::BeginMap;
    if (mi) {
      for (const auto& kv : mi->saved_) {
        out << YAML::Key << kv.first << YAML::Value << YAML::BeginMap
            << YAML::Key << "x"      << YAML::Value << kv.second.x
            << YAML::Key << "y"      << YAML::Value << kv.second.y
            << YAML::Key << "hidden" << YAML::Value << kv.second.hidden
            << YAML::EndMap;
      }
    }
    out << YAML::EndMap;
  }

  {
    auto* si = Bourgeon::Instance().status_icons();
    const StatusIconConfig c = si ? si->config() : StatusIconConfig{};
    out << YAML::Key << "statusicon_enabled"        << YAML::Value << c.enabled
        << YAML::Key << "statusicon_corner"         << YAML::Value << c.corner
        << YAML::Key << "statusicon_margin_x"       << YAML::Value << c.margin_x
        << YAML::Key << "statusicon_margin_y"       << YAML::Value << c.margin_y
        << YAML::Key << "statusicon_step_dir"       << YAML::Value << c.step_dir
        << YAML::Key << "statusicon_wrap_dir"       << YAML::Value << c.wrap_dir
        << YAML::Key << "statusicon_per_line"       << YAML::Value << c.per_line
        << YAML::Key << "statusicon_icon_pitch"     << YAML::Value << c.icon_pitch
        << YAML::Key << "statusicon_line_pitch"     << YAML::Value << c.line_pitch
        << YAML::Key << "statusicon_sort_mode"      << YAML::Value << c.sort_mode
        << YAML::Key << "statusicon_show_remaining" << YAML::Value << c.show_remaining
        << YAML::Key << "statusicon_time_bg"        << YAML::Value << c.time_bg
        << YAML::Key << "statusicon_icon_alpha"     << YAML::Value << c.icon_alpha;
  }

  {
    auto* qt = Bourgeon::Instance().quest_tracker();
    const QuestTrackerConfig c = qt ? qt->config() : QuestTrackerConfig{};
    out << YAML::Key << "questtracker_enabled"        << YAML::Value << c.enabled
        << YAML::Key << "questtracker_show_titlebar"  << YAML::Value << c.show_titlebar
        << YAML::Key << "questtracker_locked"         << YAML::Value << c.locked
        << YAML::Key << "questtracker_pos_x"          << YAML::Value << c.pos_x
        << YAML::Key << "questtracker_pos_y"          << YAML::Value << c.pos_y
        << YAML::Key << "questtracker_width"          << YAML::Value << c.width
        << YAML::Key << "questtracker_max_quests"     << YAML::Value << c.max_quests
        << YAML::Key << "questtracker_title_rgb"      << YAML::Value << c.title_rgb
        << YAML::Key << "questtracker_desc_rgb"       << YAML::Value << c.desc_rgb
        << YAML::Key << "questtracker_hunt_rgb"       << YAML::Value << c.hunt_rgb
        << YAML::Key << "questtracker_font_scale"     << YAML::Value << c.font_scale
        << YAML::Key << "questtracker_show_bg"        << YAML::Value << c.show_bg
        << YAML::Key << "questtracker_bg_alpha"       << YAML::Value << c.bg_alpha
        << YAML::Key << "questtracker_show_objective" << YAML::Value << c.show_objective;
  }

  {
    auto* st = Bourgeon::Instance().settings_tweaks();
    const D3D9PostFx g = st ? st->fx() : D3D9PostFx{};
    out << YAML::Key << "fx_enabled"     << YAML::Value << g.enabled
        << YAML::Key << "fx_brightness"  << YAML::Value << g.brightness
        << YAML::Key << "fx_contrast"    << YAML::Value << g.contrast
        << YAML::Key << "fx_gamma"       << YAML::Value << g.gamma
        << YAML::Key << "fx_saturation"  << YAML::Value << g.saturation
        << YAML::Key << "fx_temperature" << YAML::Value << g.temperature
        << YAML::Key << "fx_filter"      << YAML::Value << g.filter
        << YAML::Key << "fx_vignette"    << YAML::Value << g.vignette
        << YAML::Key << "fx_grain"       << YAML::Value << g.grain
        << YAML::Key << "fx_aberration"  << YAML::Value << g.aberration
        << YAML::Key << "fx_sharpen"     << YAML::Value << g.sharpen
        << YAML::Key << "fx_fxaa"        << YAML::Value << g.fxaa
        << YAML::Key << "fx_fxaa_strength" << YAML::Value << g.fxaa_strength
        << YAML::Key << "fps_overlay"    << YAML::Value << (st ? st->fps_overlay() : false)
        << YAML::Key << "cam_zoom_enabled" << YAML::Value << (st ? st->zoom_enabled() : false)
        << YAML::Key << "cam_zoom_factor"  << YAML::Value << (st ? st->zoom_factor() : 1.0f)
        << YAML::Key << "cam_zoom_speed"   << YAML::Value << (st ? st->zoom_speed() : 1.0f)
        << YAML::Key << "tex_filter"       << YAML::Value << (st ? st->tex_filter() : 0)
        << YAML::Key << "game_option_pos_x" << YAML::Value << (st ? st->gopt_x() : INT_MIN)
        << YAML::Key << "game_option_pos_y" << YAML::Value << (st ? st->gopt_y() : INT_MIN)
        << YAML::Key << "esc_option_pos_x"  << YAML::Value << (st ? st->esc_x() : INT_MIN)
        << YAML::Key << "esc_option_pos_y"  << YAML::Value << (st ? st->esc_y() : INT_MIN);
  }

  {
    out << YAML::Key << "malgun_font" << YAML::Value << ro::IsFontEnabled();
    out << YAML::Key << "ro_skin" << YAML::Value << ro::IsSkinEnabled();
    {
      auto& sc = ro::SkinConfig();
      auto pk = [](const float* c) {
        return (unsigned int)ImGui::ColorConvertFloat4ToU32(
            ImVec4(c[0], c[1], c[2], c[3]));
      };
      out << YAML::Key << "ro_skin_bright" << YAML::Value << sc.title_brightness;
      out << YAML::Key << "ro_skin_rounding" << YAML::Value << sc.rounding;
      out << YAML::Key << "ro_skin_alpha" << YAML::Value << sc.alpha;
      out << YAML::Key << "ro_skin_body" << YAML::Value << pk(sc.body_col);
      out << YAML::Key << "ro_skin_border" << YAML::Value << pk(sc.border_col);
      out << YAML::Key << "ro_skin_titletx" << YAML::Value << pk(sc.title_text);
      out << YAML::Key << "ro_skin_bodytx" << YAML::Value << pk(sc.body_text);
      out << YAML::Key << "ro_skin_tab" << YAML::Value << pk(sc.tab_col);
      out << YAML::Key << "ro_skin_tabinact" << YAML::Value << pk(sc.tab_inact);
      out << YAML::Key << "ro_skin_input" << YAML::Value << pk(sc.input_col);
      out << YAML::Key << "ro_skin_header" << YAML::Value << pk(sc.header_col);
      out << YAML::Key << "ro_skin_slot" << YAML::Value << pk(sc.slot_col);
      out << YAML::Key << "ro_skin_doll" << YAML::Value << pk(sc.doll_col);
      out << YAML::Key << "ro_skin_card" << YAML::Value << pk(sc.card_col);
      out << YAML::Key << "ro_skin_cardhead" << YAML::Value << pk(sc.card_head_col);
      out << YAML::Key << "ro_skin_cardtx" << YAML::Value << pk(sc.card_head_text);
    }
    out << YAML::Key << "ro_skin_presets" << YAML::Value << YAML::BeginSeq;
    for (const auto& p : g_ro_presets) {
      out << YAML::BeginMap << YAML::Key << "name" << YAML::Value << p.name;
      // Réutilise EmitSkinCfg sauf le BeginMap/EndMap déjà ouverts ici : on inline.
      out << YAML::Key << "bright" << YAML::Value << p.cfg.title_brightness;
      out << YAML::Key << "alpha" << YAML::Value << p.cfg.alpha;
      out << YAML::Key << "body" << YAML::Value << PackCol(p.cfg.body_col);
      out << YAML::Key << "border" << YAML::Value << PackCol(p.cfg.border_col);
      out << YAML::Key << "titletx" << YAML::Value << PackCol(p.cfg.title_text);
      out << YAML::Key << "bodytx" << YAML::Value << PackCol(p.cfg.body_text);
      out << YAML::Key << "tab" << YAML::Value << PackCol(p.cfg.tab_col);
      out << YAML::Key << "tabinact" << YAML::Value << PackCol(p.cfg.tab_inact);
      out << YAML::Key << "input" << YAML::Value << PackCol(p.cfg.input_col);
      out << YAML::Key << "header" << YAML::Value << PackCol(p.cfg.header_col);
      out << YAML::Key << "slot" << YAML::Value << PackCol(p.cfg.slot_col);
      out << YAML::Key << "doll" << YAML::Value << PackCol(p.cfg.doll_col);
      out << YAML::Key << "card" << YAML::Value << PackCol(p.cfg.card_col);
      out << YAML::Key << "cardhead" << YAML::Value << PackCol(p.cfg.card_head_col);
      out << YAML::Key << "cardtx" << YAML::Value << PackCol(p.cfg.card_head_text);
      out << YAML::EndMap;
    }
    out << YAML::EndSeq;
    auto* iv = Bourgeon::Instance().inventory_viewer();
    out << YAML::Key << "inventory_imgui" << YAML::Value << (iv ? iv->imgui_enabled_ : false);
    auto* stg = Bourgeon::Instance().storage_tweaks();
    out << YAML::Key << "storage_imgui" << YAML::Value << (stg ? stg->imgui_enabled_ : true);
    // Favoris storage (ids d'items, triés pour un yaml stable = pas de diff parasite).
    out << YAML::Key << "storage_favorites" << YAML::Value << YAML::Flow << YAML::BeginSeq;
    if (stg) {
      std::vector<uint32_t> favs(stg->favorites_.begin(), stg->favorites_.end());
      std::sort(favs.begin(), favs.end());
      for (uint32_t id : favs) out << id;
    }
    out << YAML::EndSeq;
    auto* cs = Bourgeon::Instance().cashshop_tweaks();
    out << YAML::Key << "cashshop_imgui" << YAML::Value << (cs ? cs->imgui_enabled_ : true);
    auto* sh = Bourgeon::Instance().shop_tweaks();
    out << YAML::Key << "shop_imgui" << YAML::Value << (sh ? sh->imgui_enabled_ : false);
    auto* nd = Bourgeon::Instance().npc_dialog_tweaks();
    out << YAML::Key << "npc_dialog_imgui" << YAML::Value
        << (nd ? nd->imgui_enabled_ : false);
    out << YAML::Key << "npc_menu_search" << YAML::Value
        << (nd ? nd->menu_search_ : true);
    auto* cse = Bourgeon::Instance().character_sheet();
    out << YAML::Key << "charsheet_imgui" << YAML::Value
        << (cse ? cse->imgui_enabled_ : false);
  }

  {
    auto* sb = Bourgeon::Instance().skill_bar();
    out << YAML::Key << "skillbar_enabled"  << YAML::Value << (sb ? sb->enabled_    : false)
        << YAML::Key << "skillbar_bilinear" << YAML::Value << (sb ? sb->bilinear_   : false)
        << YAML::Key << "skillbar_clickthrough" << YAML::Value << (sb ? sb->clickthrough_ : false)
        << YAML::Key << "skillbar_show_keys" << YAML::Value << (sb ? sb->show_keys_ : true)
        << YAML::Key << "skillbar_bold_text" << YAML::Value << (sb ? sb->bold_text_ : false)
        << YAML::Key << "skillbar_key_scale" << YAML::Value << (sb ? sb->key_scale_ : 1.0f)
        << YAML::Key << "skillbar_count_scale" << YAML::Value << (sb ? sb->count_scale_ : 1.0f);
    if (sb) {
      // 3 barres fixes (0=Onglet1, 1=Onglet2, 2=Items)
      for (int b = 0; b < SkillBarTweaks::kBarCount; ++b) {
        const auto& bc = sb->bars_[b];
        const std::string p = "skillbar" + std::to_string(b) + "_";
        out << YAML::Key << (p + "visible") << YAML::Value << bc.visible
            << YAML::Key << (p + "x")       << YAML::Value << bc.x
            << YAML::Key << (p + "y")       << YAML::Value << bc.y
            << YAML::Key << (p + "columns") << YAML::Value << bc.columns
            << YAML::Key << (p + "first")   << YAML::Value << bc.first_slot
            << YAML::Key << (p + "slots")   << YAML::Value << bc.slot_count
            << YAML::Key << (p + "size")    << YAML::Value << bc.icon_size
            << YAML::Key << (p + "spacing") << YAML::Value << bc.spacing;
      }
      sb->SnapshotItemSlots();  // capture le contenu live de la barre d'items -> yaml (persistance client)
      for (int i = 0; i < SkillBarTweaks::kItemSlotMax; ++i)
        out << YAML::Key << ("skillbar_item" + std::to_string(i)) << YAML::Value << sb->item_slots_[i];
      char cf[9], cs[9], ci[9], ce[9], cb[9], ch[9], ck[9], cn[9], co[9];
      std::snprintf(cf, sizeof(cf), "%08X", ArgbFromPicker(sb->col_frame_));
      std::snprintf(cs, sizeof(cs), "%08X", ArgbFromPicker(sb->col_skill_));
      std::snprintf(ci, sizeof(ci), "%08X", ArgbFromPicker(sb->col_item_));
      std::snprintf(ce, sizeof(ce), "%08X", ArgbFromPicker(sb->col_empty_));
      std::snprintf(cb, sizeof(cb), "%08X", ArgbFromPicker(sb->col_border_));
      std::snprintf(ch, sizeof(ch), "%08X", ArgbFromPicker(sb->col_borderhi_));
      std::snprintf(ck, sizeof(ck), "%08X", ArgbFromPicker(sb->col_keytext_));
      std::snprintf(cn, sizeof(cn), "%08X", ArgbFromPicker(sb->col_count_));
      std::snprintf(co, sizeof(co), "%08X", ArgbFromPicker(sb->col_textout_));
      out << YAML::Key << "skillbar_col_frame"    << YAML::Value << cf
          << YAML::Key << "skillbar_col_skill"    << YAML::Value << cs
          << YAML::Key << "skillbar_col_item"     << YAML::Value << ci
          << YAML::Key << "skillbar_col_empty"    << YAML::Value << ce
          << YAML::Key << "skillbar_col_border"   << YAML::Value << cb
          << YAML::Key << "skillbar_col_borderhi" << YAML::Value << ch
          << YAML::Key << "skillbar_col_keytext"  << YAML::Value << ck
          << YAML::Key << "skillbar_col_count"    << YAML::Value << cn
          << YAML::Key << "skillbar_col_textout"  << YAML::Value << co;
    }
  }

  out << YAML::Key << "chat_bg_presets" << YAML::Value << YAML::BeginSeq;
  for (const auto& p : chat_bg_presets_) {
    char pbuf[9];
    std::snprintf(pbuf, sizeof(pbuf), "%08X", p.argb);
    out << YAML::BeginMap
        << YAML::Key << "name"  << YAML::Value << p.name
        << YAML::Key << "color" << YAML::Value << pbuf
        << YAML::EndMap;
  }
  out << YAML::EndSeq;

  // Presets d'équipement (loadouts par CID) — possédés par CharacterSheet.
  out << YAML::Key << "equip_presets" << YAML::Value << YAML::BeginSeq;
  if (auto* cse = Bourgeon::Instance().character_sheet()) {
    for (const auto& ep : cse->equip_presets()) {
      out << YAML::BeginMap
          << YAML::Key << "cid"  << YAML::Value << ep.cid
          << YAML::Key << "name" << YAML::Value << ep.name
          << YAML::Key << "hkvk" << YAML::Value << ep.hotkeyVk
          << YAML::Key << "hkc"  << YAML::Value << ep.hkCtrl
          << YAML::Key << "hka"  << YAML::Value << ep.hkAlt
          << YAML::Key << "hks"  << YAML::Value << ep.hkShift
          << YAML::Key << "items" << YAML::Value << YAML::BeginSeq;
      for (const auto& pi : ep.items) {
        out << YAML::BeginMap
            << YAML::Key << "id"     << YAML::Value << pi.nameid
            << YAML::Key << "refine" << YAML::Value << pi.refine
            << YAML::Key << "grade"  << YAML::Value << pi.grade
            << YAML::Key << "left"   << YAML::Value << pi.leftHand
            << YAML::Key << "cards"  << YAML::Value << YAML::Flow << YAML::BeginSeq;
        for (int c = 0; c < 4; ++c) out << pi.cards[c];
        out << YAML::EndSeq << YAML::EndMap;
      }
      out << YAML::EndSeq << YAML::EndMap;
    }
  }
  out       << YAML::EndSeq
      << YAML::EndMap
      << YAML::EndMap;

  const std::string path = GetSettingsPath();
  std::ofstream f(path);
  if (!f) {
    LogError("[MoonlightUi] failed to write {}", path);
    return;
  }
  f << out.c_str();
  // LogInfo("[MoonlightUi] saved chat backgrounds to {}", path);
}

// ── Server settings sync ──────────────────────────────────────────────────

void MoonlightUi::UpdateRelay() {
  if (auto* relay = Bourgeon::Instance().discord_relay()) {
    relay->set_chat_active(discord_chat_ && in_gonryun_);
  }
}

// Called by ModeMgr::Switch (and OnUpdateHook for in_game_ tracking).
void MoonlightUi::OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) {
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
void MoonlightUi::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  if (opcode == kOpcodeMapMove) {
    // 0x0091 ZC_NPCACK_MAPMOVE: data points at mapname[16] (e.g. "gonryun.gat").
    const char* map_name = reinterpret_cast<const char*>(data);
    in_gonryun_ = in_game_ &&
                  (strncmp(map_name, kDiscordMap, sizeof(kDiscordMap) - 1) == 0);
    // LogInfo("[MoonlightUi] map move -> '{}' in_gonryun={}",
            // std::string(map_name, strnlen(map_name, len)), in_gonryun_);
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
  // 32-bit math: a uint16_t cast here would truncate (e.g. count=0xFFFF wraps
  // 393216 -> 0), defeating the length check and allowing an OOB read.
  const uint32_t expected_len = 6u + static_cast<uint32_t>(count) * 6u;
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
        // LogInfo("[MoonlightUi] show_exp={}", show_exp_);
        break;
      case kSettingShowZeny:
        show_zeny_ = (value != 0);
        // LogInfo("[MoonlightUi] show_zeny={}", show_zeny_);
        break;
      case kSettingShowMobInfo:
        show_mob_info_ = (value != 0);
        // LogInfo("[MoonlightUi] show_mob_info={}", show_mob_info_);
        break;
      case kSettingSeparate:
        separate_ = (value != 0);
        // LogInfo("[MoonlightUi] separate={}", separate_);
        break;
      case kSettingBlockExp:
        block_exp_ = (value != 0);
        // LogInfo("[MoonlightUi] block_exp={}", block_exp_);
        break;
      case kSettingAlootRare:
        aloot_rare_ = (value != 0);
        // LogInfo("[MoonlightUi] aloot_rare={}", aloot_rare_);
        break;
      case kSettingAlootRate:
        aloot_rate_ = static_cast<int>(value);
        // LogInfo("[MoonlightUi] aloot_rate={}", aloot_rate_);
        break;
      case kSettingAlootPognon:
        aloot_pognon_ = static_cast<int>(value) * 100;
        // LogInfo("[MoonlightUi] aloot_pognon={}", aloot_pognon_);
        break;
      case kSettingAlootType:
        aloot_type_mask_ = static_cast<int>(value);
        // LogInfo("[MoonlightUi] aloot_type_mask=0x{:04X}", aloot_type_mask_);
        break;
      case kSettingDiscordChat:
        discord_chat_ = (value != 0);
        // LogInfo("[MoonlightUi] discord_chat={}", discord_chat_);
        UpdateRelay();
        break;
      case kSettingShowDelay:
        show_delay_ = (value != 0);
        // LogInfo("[MoonlightUi] show_delay={}", show_delay_);
        break;
      case kSettingShowSpeed:
        show_speed_ = (value != 0);
        // LogInfo("[MoonlightUi] show_speed={}", show_speed_);
        break;
      case kSettingSellStuff:
        sell_stuff_ = (value != 0);
        // LogInfo("[MoonlightUi] sell_stuff={}", sell_stuff_);
        break;
      case kSettingSellItem:
        sell_item_ = (value != 0);
        // LogInfo("[MoonlightUi] sell_item={}", sell_item_);
        break;
      case kSettingNoAsk:
        no_ask_ = (value != 0);
        // LogInfo("[MoonlightUi] no_ask={}", no_ask_);
        break;
      case kSettingNoks:
        noks_ = static_cast<int>(value);
        // LogInfo("[MoonlightUi] noks={}", noks_);
        break;
      case kSettingWings:
        wings_ = (value != 0);
        // LogInfo("[MoonlightUi] wings={}", wings_);
        break;
      case kSettingAlootMvp:
        aloot_mvp_ = (value != 0);
        // LogInfo("[MoonlightUi] aloot_mvp={}", aloot_mvp_);
        break;
      case kSettingAlootMvpRwd:
        aloot_mvp_rwd_ = (value != 0);
        // LogInfo("[MoonlightUi] aloot_mvp_rwd={}", aloot_mvp_rwd_);
        break;
      case kSettingTriInv:
        tri_inv_ = static_cast<int>(value);
        // LogInfo("[MoonlightUi] tri_inv={}", tri_inv_);
        break;
      case kSettingTriCart:
        tri_cart_ = static_cast<int>(value);
        // LogInfo("[MoonlightUi] tri_cart={}", tri_cart_);
        break;
      case kSettingTriStorage:
        tri_storage_ = static_cast<int>(value);
        // LogInfo("[MoonlightUi] tri_storage={}", tri_storage_);
        break;
      case kSettingTriGstorage:
        tri_gstorage_ = static_cast<int>(value);
        // LogInfo("[MoonlightUi] tri_gstorage={}", tri_gstorage_);
        break;
      case kSettingAlootId:
        if (value == 0) {
          aloot_ids_.clear();
          // LogInfo("[MoonlightUi] aloot_ids cleared");
        } else {
          bool found = false;
          for (uint32_t x : aloot_ids_) if (x == value) { found = true; break; }
          if (!found) aloot_ids_.push_back(value);
          // LogInfo("[MoonlightUi] aloot_id added={}", value);
        }
        break;
      case kSettingAlootIdRemove:
        break;
      default:
        // LogInfo("[MoonlightUi] unknown setting id={} value={}", id, value);
        break;
    }
  }
}

// Send a setting change to the server.
// The server will echo it back in a ZC_BOURGEON_SETTINGS packet, which is how we know the change was accepted.
void MoonlightUi::SendSetting(uint16_t id, uint32_t value) {
  uint8_t buf[10];
  *reinterpret_cast<uint16_t*>(buf)     = kOpcodeToServer;
  *reinterpret_cast<uint16_t*>(buf + 2) = 10;
  *reinterpret_cast<uint16_t*>(buf + 4) = id;
  *reinterpret_cast<uint32_t*>(buf + 6) = value;
  Bourgeon::Instance().SendPacket(buf, sizeof(buf));
}

// API autolootid partagée (utilisée par le panneau de description enrichi) 
bool MoonlightUi::IsAlootId(uint32_t id) const {
  for (uint32_t v : aloot_ids_)
    if (v == id) return true;
  return false;
}

// Add/remove an item id to the autoloot list. Returns true if the list changed.
bool MoonlightUi::AddAlootId(uint32_t id) {
  if (id == 0 || aloot_ids_.size() >= 50 || IsAlootId(id)) return false;
  aloot_ids_.push_back(id);
  SendSetting(kSettingAlootId, id);
  return true;
}

// Remove an item id from the autoloot list. Returns true if the list changed.
bool MoonlightUi::RemoveAlootId(uint32_t id) {
  for (size_t k = 0; k < aloot_ids_.size(); ++k) {
    if (aloot_ids_[k] == id) {
      SendSetting(kSettingAlootIdRemove, id);
      aloot_ids_.erase(aloot_ids_.begin() + k);
      return true;
    }
  }
  return false;
}

// Return the name of an item id, or nullptr if unknown. Used by the autolootid panel.
const char* MoonlightUi::ItemName(uint32_t id) const {
  const auto it = item_names_.find(id);
  return (it != item_names_.end()) ? it->second.c_str() : nullptr;
}

// Send a preset command to the server (save/load/delete).
// The server will echo it back in a ZC_BOURGEON_PRESET_CMD packet.
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
void HelpMarker(const char* desc) {
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
    ImGui::TextUnformatted(desc);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// SliderFloat/SliderInt variants that ALSO adjust on mouse-wheel while hovered
// (fine-tuning without grabbing the handle). SetItemKeyOwner(MouseWheelY) claims
// the wheel so the settings window doesn't scroll at the same time. Step defaults
// to 0.01 (float) / 1 (int).
bool WheelSliderFloat(const char* label, float* v, float lo, float hi, const char* fmt, float step) {
  bool changed = ImGui::SliderFloat(label, v, lo, hi, fmt);
  if (ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY)) {
    const float w = ImGui::GetIO().MouseWheel;
    if (w != 0.0f) {
      if (step <= 0.0f) step = 0.01f;
      float nv = *v + w * step;
      if (nv < lo) nv = lo;
      if (nv > hi) nv = hi;
      if (nv != *v) { *v = nv; changed = true; }
    }
  }
  return changed;
}

bool WheelSliderInt(const char* label, int* v, int lo, int hi, const char* fmt, int step) {
  bool changed = ImGui::SliderInt(label, v, lo, hi, fmt);
  if (ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY)) {
    const float w = ImGui::GetIO().MouseWheel;
    if (w != 0.0f) {
      if (step <= 0) step = 1;  // unit precision by default ("à l'unité près")
      int nv = *v + (w > 0.0f ? step : -step);
      if (nv < lo) nv = lo;
      if (nv > hi) nv = hi;
      if (nv != *v) { *v = nv; changed = true; }
    }
  }
  return changed;
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

namespace {
// A full-screen UI (world map = window id 0x8c) replaces the in-game HUD; hide
// the alignment grid while it is open. (Mirrors BasicInfoTweaks/MenuIconTweaks.)
bool HudReplaced() {
  using FindWindowFn = void* (__thiscall*)(void*, int);
  return reinterpret_cast<FindWindowFn>(0x00a47b90)(
             reinterpret_cast<void*>(0x0131f4e8), 0x8c) != nullptr;
}
}  // namespace

void AlignGrid::Draw() const {
  const ImVec2 ds = ImGui::GetIO().DisplaySize;
  const float step = static_cast<float>(cell());
  ImDrawList* dl = ImGui::GetBackgroundDrawList();  // over game, under windows
  const ImU32 col = ImGui::ColorConvertFloat4ToU32(
      ImVec4(color[0], color[1], color[2], color[3]));
  // Anchor the grid on the screen centre: offset the first line so a grid line
  // falls exactly on the centre, keeping the mesh symmetric left/right and
  // top/bottom (the bright centre cross then lands right on a line).
  const float ox = std::fmod(ds.x * 0.5f, step);
  const float oy = std::fmod(ds.y * 0.5f, step);
  for (float x = ox; x <= ds.x; x += step)
    dl->AddLine(ImVec2(x, 0.0f), ImVec2(x, ds.y), col);
  for (float y = oy; y <= ds.y; y += step)
    dl->AddLine(ImVec2(0.0f, y), ImVec2(ds.x, y), col);
  // Brighter centre cross for quick centring.
  float ca = color[3] * 2.5f;
  if (ca > 1.0f) ca = 1.0f;
  const ImU32 cc = ImGui::ColorConvertFloat4ToU32(
      ImVec4(color[0], color[1], color[2], ca));
  dl->AddLine(ImVec2(ds.x * 0.5f, 0.0f), ImVec2(ds.x * 0.5f, ds.y), cc, 1.5f);
  dl->AddLine(ImVec2(0.0f, ds.y * 0.5f), ImVec2(ds.x, ds.y * 0.5f), cc, 1.5f);
}

// ── ImGui panel ───────────────────────────────────────────────────────────

void MoonlightUi::OnRenderUI() {
  if (!in_game_) return;


  // Global alignment grid (shared HUD overlay). Drawn here on the background
  // list so it shows even with the bars hidden; suppressed while a full-screen
  // UI (world map) replaces the HUD, matching the bars/icons.
  if (grid_.show && !HudReplaced()) grid_.Draw();

  // Persist EXP-bar geometry once, the frame after the user finishes a drag.
  if (auto* eb = Bourgeon::Instance().basic_info(); eb && eb->geometry_dirty_) {
    eb->geometry_dirty_ = false;
    SaveSettings();
  }
  // Same for menu-icon positions (set on drag-end in MenuIconTweaks).
  if (auto* mi = Bourgeon::Instance().menu_icons(); mi && mi->geometry_dirty_) {
    mi->geometry_dirty_ = false;
    SaveSettings();
  }
  // Skill-bar config (set on any panel change / drag-end in SkillBarTweaks).
  if (auto* sb = Bourgeon::Instance().skill_bar(); sb && sb->dirty_) {
    sb->dirty_ = false;
    SaveSettings();
  }

  if (apply_collapse_) {
    ImGui::SetNextWindowCollapsed(ui_collapsed_, ImGuiCond_Always);
    apply_collapse_ = false;
  }
  // Échap a demandé le repli (fenêtre principale = dernière avant le jeu) : on force le
  // repli ce frame ; la détection is_collapsed ci-dessous met à jour ui_collapsed_ + persiste.
  if (collapse_requested_) {
    collapse_requested_ = false;
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_Always);
  }

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
  // Skin RO (toggleable : BeginRoWindow retombe sur ImGui::Begin si skin off).
  ro::BeginRoWindow("Moonlight-Destiny");

  const bool is_collapsed = ImGui::IsWindowCollapsed();
  if (is_collapsed != ui_collapsed_) {
    ui_collapsed_ = is_collapsed;
    SaveSettings();
  }
  // Fenêtre principale = cible « minimiser » d'Échap, en DERNIER recours (seulement
  // dépliée, seulement s'il ne reste aucune autre fenêtre fermable) : Échap la replie
  // avant d'être rendu au jeu pour ses fenêtres natives.
  if (!is_collapsed)
    ro::RegisterEscapeMinimizeWindow(&collapse_requested_);
  ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (!is_collapsed) {

    if (ImGui::CollapsingHeader("Règles du serveur"))
    {
      auto BulletWrapped = [](const char* text) {
        ImGui::Bullet(); ImGui::SameLine(); ImGui::TextWrapped("%s", text);
      };
      ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "CES RÈGLEMENTS S'APPLIQUENT PARTOUT SUR MOONLIGHT-DESTINY !");
      if (ImGui::TreeNode("Règlements généraux"))
      {
        ImGui::TextWrapped("Les règles du serveur doivent être appliquées à la lettre.\nToute personne ne respectant pas la charte sera sanctionnée dans les plus brefs délais.");
        ImGui::Spacing();
        BulletWrapped("Les joueurs doivent se respecter et garder un langage propre et courtois.");
        BulletWrapped("Les propos visant à rejeter un nouveau joueur sont interdits.");
        BulletWrapped("L'utilisation de programmes tels que bots ou hacks = ban définitif sans hésitation.");
        BulletWrapped("Le flood est strictement interdit.");
        BulletWrapped("Vous êtes entièrement responsable de votre compte.");
        BulletWrapped("Le staff ne rend pas les items perdus (vente NPC, deslotage raté, refine raté).");
        BulletWrapped("Le staff peut exceptionnellement rendre un item perdu si les logs prouvent un bug serveur.");
        BulletWrapped("Ne partagez jamais votre compte ou votre mot de passe.");
        BulletWrapped("La demande de support pour créer un serveur privé est non recommandée.");
        BulletWrapped("Le plagiat volontaire d'un membre du staff est puni.");
        BulletWrapped("Tout ce qui se rapporte au serveur est la propriété exclusive des administrateurs.");
        BulletWrapped("Le langage SMS est à proscrire.");
        BulletWrapped("L'exploitation d'un bug ou abus = sanction. Prévenez immédiatement un administrateur.");
        BulletWrapped("Si vous abusez du cashshop en votant avec plusieurs comptes forum… \ngare à vous c'est comme avec les impôts, \ntant qu'on est pas contrôlé c'est la fête, mais quand ils vous tombent dessus...");
        ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Sur le serveur de jeu"))
      {
        BulletWrapped("Insultes et vols de drop (Looting) = INTERDITS.");
        BulletWrapped("Heal ou buff un monstre qui ne vous appartient pas sans accord = puni.");
        BulletWrapped("Si vous êtes banni définitivement, tous les comptes liés à votre IP/PC le seront aussi.");
        BulletWrapped("Les sanctions (mute, jail, kick, ban) sont à la discrétion du staff.");
        BulletWrapped("Le Kill Steal est strictement interdit (voir définition). Utilisez @noks pour vous protéger.");
        ImGui::Spacing();
        BulletWrapped("Les MVPs sont FFA :");
        ImGui::Indent();
        ImGui::TextWrapped("Vous pouvez les attaquer même si quelqu'un est dessus.");
        ImGui::TextWrapped("(À vous de voir si vous voulez passer pour un gros connard selfish en KSant le MVP)");
        ImGui::Text("Si vous ne voulez pas vous faire KS, faites @noks <3");
        ImGui::Unindent();
        ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Le staff"))
      {
        BulletWrapped("Si vous cassez les couilles du staff ban/delete non temporaire.");
        BulletWrapped("Aucun membre du staff ne vous demandera votre mot de passe.");
        BulletWrapped("Aucun membre du staff ne vous demandera votre login.");
        BulletWrapped("Aucun membre du staff ne vous demandera votre email.");
        BulletWrapped("Seuls les admins peuvent rendre des items perdus suite à un bug serveur.");
        BulletWrapped("Le staff ne rend pas les items prêtés à un joueur disparu/banni.");
        BulletWrapped("Le staff ne donne pas d'items (hors events).");
        BulletWrapped("Les membres du staff ne sont pas des robots. Soyez courtois, cherchez avant de demander.");
        BulletWrapped("Les questions dont la réponse est sur une database = évitez.");
        ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Règlements dans les endroits spécifiques"))
      {
        if (ImGui::TreeNode("Salle de duel"))
        {
          BulletWrapped("Ce n'est pas un salon de thé");
          BulletWrapped("Si vous regardez, ok. Sinon, laissez la place.");
          BulletWrapped("Utilisez : @duel, @invite, @accept, @reject, @leave.");
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("Carnage Room"))
        {
          BulletWrapped("Loi du plus fort.");
          BulletWrapped("Amusez‑vous dans le respect.");
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("PVP Room"))
        {
          BulletWrapped("Free Kill interdit.");
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("DB Room"))
        {
          BulletWrapped("Kill Steal STRICTEMENT interdit.");
          BulletWrapped("Si la personne meurt ou se hide les mobs sont à vous.");
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("Guild Dungeon"))
        {
          BulletWrapped("Libre de tuer les guildiens adverses.");
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("WoE Castles"))
        {
          BulletWrapped("Interdiction d'apporter de l'aide via un perso non participant (multi-account/perso).");
          BulletWrapped("Les ententes entre guildes sont informelles, non officielles, non sanctionnables.");
          BulletWrapped("Elles doivent être discutées entre guildes dominantes, dans le respect.");
          ImGui::TreePop();
        }
        ImGui::TreePop();
      }
      ImGui::Spacing();
      if (ImGui::TreeNode("Logiciels tiers"))
      {
        ImGui::Text("Autorisations :");
        ImGui::Indent();
          BulletWrapped("Je vais être clair : oui, j'autorise les scripts AHK, les macros clavier/souris, les trucs qui bouclent un sort… tant que ça reste :");
          BulletWrapped("SIMPLE");
          BulletWrapped("BASIQUE");
          BulletWrapped("Pas un tableau de bord de la NASA");
          BulletWrapped("Vous bouclez le spell, éventuellement un clic en plus pour les AOE type Storm Gust, et basta.");
        ImGui::Unindent();
        ImGui::Text("Quality of Life :");
        ImGui::Indent();
          BulletWrapped("Le but, c'est du Q.O.L");
          BulletWrapped("Vous préservez votre clavier, votre souris, vos doigts, vos poignets, vos oreilles, et celles de vos voisins qui n'ont rien demandé.");
          BulletWrapped("Bref : du confort, pas du cheat.");
        ImGui::Unindent();
        ImGui::Text("Les trucs interdits (et je rigole zéro) :");
        ImGui::Indent();
          ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Ne me prenez pas pour un jambon.");
          ImGui::Text("Si vous me sortez :");
          ImGui::Indent();
            BulletWrapped("un auto-buffer");
            BulletWrapped("un auto-pot");
            BulletWrapped("un super TP/SG de physicien quantique");
            BulletWrapped("un script qui ferait rougir Tony Stark");
          ImGui::Unindent();
          ImGui::Text("Alors là :");
          ImGui::Indent();
            BulletWrapped("Je vous fais le fion.");
            BulletWrapped("Je m'en bats les couilles.");
            BulletWrapped("Je vous dégage plus vite que Thanos avec son finger snap. *Snap*");
          ImGui::Unindent();
          ImGui::Text("Les excuses bidon :");
          ImGui::Indent();
            BulletWrapped("\"Mais les autres serveurs le font...\"");
            BulletWrapped("\"Mais j'étais pas AFK, je regardais Naruto à côté...\"");
          ImGui::Unindent();
          ImGui::Text("Résultat :");
          ImGui::Indent();
            BulletWrapped("Pouf.");
            BulletWrapped("Vous étiez sur Moon.");
            BulletWrapped("Vous ne l'êtes plus.");
            BulletWrapped("Et il ne restera de vous que des ruines numériques sur Wayback Machine.");
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

      // ── Main-chat width (the stock chat resizes height only; applied by the
      //    ChatTweaks plugin via the WndProc relayout hook) ───────────────────
      if (ImGui::Checkbox("Largeur du chat", &chat_width_enabled_)) {
        chat::SetCustomWidth(chat_width_enabled_, chat_width_px_);
        SaveSettings();
      }
      if (chat_width_enabled_) {
        bool changed = WheelSliderInt("Largeur (px)", &chat_width_px_, 320, 1200);
        // Mouse-wheel fine-tuning while hovering the slider (Shift = x10 step).
        // Claim the wheel for this item so it only adjusts the value and does NOT
        // also scroll the settings window / its scrollbar(s).
        const bool hovered = ImGui::IsItemHovered();
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        const float wheel = hovered ? ImGui::GetIO().MouseWheel : 0.0f;
        if (wheel != 0.0f) {
          const int dir  = wheel > 0.0f ? 1 : -1;
          const int step = ImGui::GetIO().KeyShift ? 10 : 1;
          chat_width_px_ += dir * step;
          if (chat_width_px_ < 320)  chat_width_px_ = 320;
          if (chat_width_px_ > 1200) chat_width_px_ = 1200;
          changed = true;
        }
        if (changed) chat::SetCustomWidth(true, chat_width_px_);
        if (ImGui::IsItemDeactivatedAfterEdit() || wheel != 0.0f) SaveSettings();
      }

      // ── Chat line timestamps ([HH:MM:SS] prefix, stored in raw history) ────
      if (ImGui::Checkbox("Horodatage du chat", &chat_timestamps_)) {
        chat::SetTimestamps(chat_timestamps_);
        SaveSettings();
      }

      // ── Native item icons on <ITEML> chat links ──────────────────────────
      if (ImGui::Checkbox("Icônes d'objets", &chat_item_icons_)) {
        chat::SetItemIcons(chat_item_icons_);
        SaveSettings();
      }

      // ── Clear chat history (all channels of the main chat window) ─────────
      if (ImGui::Button("Effacer l'historique du chat")) chat::ClearHistory();
      ImGui::SameLine(); HelpMarker(
          "Vide l'historique de tous les canaux de la fenêtre de chat principale "
          "(historique brut effacé + affichage vidé). Les nouveaux messages "
          "réapparaissent normalement ensuite.");

      // ── Chat Background Colours (Main / Detached / Whisper) ───────────────
      // One independent colour+opacity picker per group, persisted locally.
      auto render_chatbg = [&](ChatBgGroup& g) {
        if (g.instrs.empty()) return;
        ImGui::PushID(g.yaml_key);
        const ImVec4 swatch(g.color[0], g.color[1], g.color[2], g.color[3]);
        if (ImGui::ColorButton("##btn", swatch,
                               ImGuiColorEditFlags_AlphaPreview, ImVec2(20, 20)))
          ImGui::OpenPopup("picker");
        ImGui::SameLine();
        ImGui::TextUnformatted(g.label);

        if (ImGui::BeginPopup("picker")) {
          // ── Shared user presets ─────────────────────────────────────────
          if (!chat_bg_presets_.empty()) {
            ImGui::TextUnformatted("Presets:");
            int delete_idx = -1;
            for (int i = 0; i < static_cast<int>(chat_bg_presets_.size()); ++i) {
              const auto& p = chat_bg_presets_[i];
              const ImVec4 col(((p.argb >> 16) & 0xFF) / 255.0f,
                               ((p.argb >>  8) & 0xFF) / 255.0f,
                               ( p.argb        & 0xFF) / 255.0f,
                               ((p.argb >> 24) & 0xFF) / 255.0f);
              ImGui::PushID(i);
              if (ImGui::ColorButton("##swatch", col,
                                     ImGuiColorEditFlags_AlphaPreview |
                                     ImGuiColorEditFlags_NoTooltip,
                                     ImVec2(18, 18))) {
                PickerFromArgb(g.color, p.argb);
                ApplyChatBg(g, p.argb, true);
                SaveSettings();
              }
              if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", p.name.c_str());
              ImGui::SameLine();
              ImGui::TextUnformatted(p.name.c_str());
              ImGui::SameLine();
              if (ImGui::SmallButton("x"))
                delete_idx = i;
              ImGui::PopID();
            }
            if (delete_idx >= 0) {
              chat_bg_presets_.erase(chat_bg_presets_.begin() + delete_idx);
              SaveSettings();
            }
            ImGui::Separator();
          }
          // ── Save current colour as a preset ─────────────────────────────
          ImGui::SetNextItemWidth(120.0f);
          ImGui::InputText("##preset_name", preset_name_buf_, sizeof(preset_name_buf_));
          ImGui::SameLine();
          if (ImGui::Button("Save preset") && preset_name_buf_[0] != '\0') {
            chat_bg_presets_.push_back({preset_name_buf_, ArgbFromPicker(g.color)});
            preset_name_buf_[0] = '\0';
            SaveSettings();
          }
          ImGui::Separator();
          if (ImGui::ColorPicker4("##pick", g.color,
                                  ImGuiColorEditFlags_AlphaBar |
                                  ImGuiColorEditFlags_NoSidePreview)) {
            ApplyChatBg(g, ArgbFromPicker(g.color), false);
            g.editing = true;
          }
          if (g.editing && ImGui::IsMouseReleased(0)) {
            ApplyChatBg(g, ArgbFromPicker(g.color), true);
            SaveSettings();
            g.editing = false;
          }
          ImGui::Separator();
          if (ImGui::Button("Close", ImVec2(-1.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
        }
        ImGui::PopID();
      };

      if (chat_bg_found_) {
        render_chatbg(chat_bg_[kChatBgMain]);
        // Quick preset switcher toggle, on the same line as the main chat picker.
        ImGui::SameLine();
        if (ImGui::Checkbox("Preset bar", &mainchat_preset_bar_))
          SaveSettings();
        render_chatbg(chat_bg_[kChatBgDetached]);
        render_chatbg(chat_bg_[kChatBgWhisper]);
      } else {
        ImGui::TextDisabled("(chat background patch unavailable)");
      }
      PopStyleCompact();
    }
    // ── DPS Meter ────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("DPS Meter")) {
      if (auto* dps = Bourgeon::Instance().dps_meter()) {
        if (ImGui::Checkbox("Afficher", &dps->visible_))
          SaveSettings();
        if (ImGui::Checkbox("Verrouiller (fige + clic-traversant)", &dps->locked_))
          SaveSettings();
        ImGui::SameLine(); HelpMarker(
            "Fige la fenêtre DPS (position/taille) et laisse passer les clics "
            "au jeu en dessous.");

        if (ImGui::ColorEdit4("Couleur texte", dps->text_color_,
                              ImGuiColorEditFlags_NoInputs))
          SaveSettings();
        if (ImGui::ColorEdit4("Couleur graphe", dps->plot_color_,
                              ImGuiColorEditFlags_NoInputs))
          SaveSettings();
        ImGui::SetNextItemWidth(160.0f);
        if (WheelSliderFloat("Opacité fond", &dps->bg_alpha_, 0.0f, 1.0f, "%.2f"))
          SaveSettings();

        ImGui::SetNextItemWidth(160.0f);
        int slot_ms = dps->slot_ms_;
        if (WheelSliderInt("Résolution (ms/slot)", &slot_ms, 50, 2000)) {
          dps->slot_ms_ = slot_ms;
          dps->ResetHistory();
          SaveSettings();
        }
        ImGui::SameLine(); HelpMarker("Largeur de chaque colonne du graphique en millisecondes.\nValeur plus basse = graphique plus précis mais moins smooth.");

        ImGui::SetNextItemWidth(160.0f);
        int win = dps->dps_window_secs_;
        if (WheelSliderInt("Fenêtre DPS (s)", &win, 1, 30)) {
          dps->dps_window_secs_ = win;
          SaveSettings();
        }
        ImGui::SameLine(); HelpMarker("Fenêtre de temps pour calculer le DPS courant affiché.");

        ImGui::SetNextItemWidth(160.0f);
        int timeout = dps->combat_timeout_secs_;
        if (WheelSliderInt("Timeout combat (s)", &timeout, 1, 15)) {
          dps->combat_timeout_secs_ = timeout;
          SaveSettings();
        }
        ImGui::SameLine(); HelpMarker("Secondes sans dégâts avant de quitter le mode combat.");

        if (ImGui::Button("Reset graphique"))
          dps->ResetHistory();

        ImGui::Separator();
        if (ImGui::Checkbox("Afficher dommages de sorts de zone dans le chat", &dps->show_ground_dmg_in_chat_))
          SaveSettings();
        ImGui::SameLine(); HelpMarker(
            "Affiche chaque coup de Storm Gust / Meteor Storm / LoV etc. dans le chat.\n"
            "Message custom Bourgeon — le serveur ne montre pas ces dégâts dans le chat habituel.");
      }
    }

    // ── Interface de jeu  ────────────────────────────────────────────────────
    // NB: la vue caméra FPS (FpsViewTweaks) reste dans le code (toggle F9) mais
    // n'est plus exposée dans ce menu (expérimental, retiré à la demande).
    if (ImGui::CollapsingHeader("Mini-jeux")) {
      // ── DOOM ──
      ImGui::SeparatorText("DOOM");
      if (auto* doom = Bourgeon::Instance().doom()) {
        bool on = doom->enabled();
        if (ImGui::Checkbox("Lancer DOOM (1993) dans Ragnarok", &on))
          doom->SetEnabled(on);
        ImGui::SameLine(); HelpMarker(
            "Le vrai DOOM (moteur doomgeneric embarqué), rendu dans une fenêtre "
            "par-dessus le jeu.\n\n"
            "Nécessite doom1.wad (shareware) à côté de l'exe du client.\n"
            "Clique la fenêtre DOOM pour capturer le clavier : ZQSD (AZERTY), "
            "WASD ou flèches pour bouger, Ctrl tirer, Espace/E ouvrir, Shift "
            "courir, Échap menu.\n"
            "Décocher = pause. Quitter depuis le menu DOOM = définitif "
            "jusqu'au redémarrage du client.");
        ImGui::TextDisabled("État : %s", doom->StatusText());
      } else {
        ImGui::TextDisabled("Indisponible.");
      }

      // ── Roggle ──
      ImGui::SeparatorText("Roggle");
      if (auto* roggle = Bourgeon::Instance().roggle()) {
        bool on = roggle->enabled();
        if (ImGui::Checkbox("Ouvrir Roggle", &on))
          roggle->SetEnabled(on);
        ImGui::SameLine(); HelpMarker(
            "Mini-jeu façon Peggle, dessiné en ImGui par-dessus le jeu.\n\n"
            "Vise à la souris depuis le canon en haut, clique pour tirer la "
            "bille. Dégomme tous les pegs ORANGE pour gagner ; le seau vert en "
            "bas rattrape la bille = bille gratuite.\n"
            "Fermer la fenêtre ou décocher = masquer (la partie est conservée).");
      } else {
        ImGui::TextDisabled("Indisponible.");
      }

      // ── Rojeweled ──
      ImGui::SeparatorText("Rojeweled");
      if (auto* rj = Bourgeon::Instance().rojeweled()) {
        bool on = rj->enabled();
        if (ImGui::Checkbox("Ouvrir Rojeweled", &on))
          rj->SetEnabled(on);
        ImGui::SameLine(); HelpMarker(
            "Match-3 façon Bejeweled dont les gemmes sont de vrais sprites de "
            "monstres RO (famille Poring : Poring, Drops, Metaling, Poporing, "
            "Marin, Deviling).\n\n"
            "Clique deux monstres voisins pour les échanger ; aligne-en 3+ pour "
            "les faire disparaître (les cascades rapportent plus). DX9 requis "
            "(sinon tuiles colorées).");
      } else {
        ImGui::TextDisabled("Indisponible.");
      }
    }
    if (ImGui::CollapsingHeader("Interface de jeu")) {
      PushStyleCompact();
      if (ImGui::Checkbox("Grille d'alignement", &grid_.show))
        SaveSettings();
      ImGui::SameLine(); HelpMarker(
          "Affiche une grille plein écran pour aligner ton interface "
          "(comme les add-ons d'interface de WoW).");
      ImGui::SetNextItemWidth(160.0f);
      {
        WheelSliderInt("Taille grille", &grid_.size, 4, 128);
        // Mouse-wheel fine-tuning while hovering the slider (Shift = x10 step).
        // Claim the wheel for this item so it adjusts the cell size and does NOT
        // also scroll the settings window.
        const bool hovered = ImGui::IsItemHovered();
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        const float wheel = hovered ? ImGui::GetIO().MouseWheel : 0.0f;
        if (wheel != 0.0f) {
          const int dir  = wheel > 0.0f ? 1 : -1;
          const int step = ImGui::GetIO().KeyShift ? 10 : 1;
          grid_.size += dir * step;
          if (grid_.size < 4)   grid_.size = 4;
          if (grid_.size > 128) grid_.size = 128;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() || wheel != 0.0f) SaveSettings();
      }
      if (ImGui::Checkbox("Aimanter à la grille", &grid_.snap))
        SaveSettings();
      ImGui::SameLine(); HelpMarker(
          "Les barres et les icônes s'alignent sur les cellules de la grille "
          "pendant le déplacement et le redimensionnement.");
      if (ImGui::ColorEdit4("Couleur grille", grid_.color,
                            ImGuiColorEditFlags_NoInputs |
                                ImGuiColorEditFlags_AlphaBar))
        SaveSettings();
      // ── Entrepôt : viewer ImGui moderne OU fenêtre native (pas de cohabitation) ──
      // ── Inventaire : viewer ImGui moderne (grille) OU fenêtre native (opt-in) ──
      if (auto* iv = Bourgeon::Instance().inventory_viewer()) {
        if (ImGui::Checkbox("Inventaire ImGui", &iv->imgui_enabled_))
          SaveSettings();
        ImGui::SameLine(); HelpMarker(
            "ON : inventaire ImGui moderne (grille d'icônes, onglets, recherche, "
            "double-clic utiliser/équiper, clic-droit, drag) et la fenêtre native "
            "est cachée.\nOFF (défaut) : inventaire natif classique, aucun viewer.");
      }
      if (auto* stg = Bourgeon::Instance().storage_tweaks()) {
        if (ImGui::Checkbox("Storage ImGui", &stg->imgui_enabled_))
          SaveSettings();
        ImGui::SameLine(); HelpMarker(
            "ON : storage ImGui moderne (icônes, onglets, tri, drag-drop) "
            "et la fenêtre native est cachée.\nOFF : storage natif classique, aucun "
            "viewer. Pas de cohabitation.");
      }
      // ── Cash shop : redraw ImGui moderne OU fenêtre native ──
      if (auto* cs = Bourgeon::Instance().cashshop_tweaks()) {
        if (ImGui::Checkbox("Cash Shop ImGui", &cs->imgui_enabled_))
          SaveSettings();
        ImGui::SameLine(); HelpMarker(
            "ON : cash shop ImGui moderne (icônes, catégories, panier) et la "
            "fenêtre native est cachée.\nOFF : cash shop natif classique.");
      }
      // ── Shop NPC : fenêtre achat/vente ImGui unifiée OU natif ──
      // (Le dialogue NPC ImGui a sa propre section « Fenêtre NPC ».)
      if (auto* sh = Bourgeon::Instance().shop_tweaks()) {
        if (ImGui::Checkbox("Shop NPC ImGui", &sh->imgui_enabled_))
          SaveSettings();
        ImGui::SameLine(); HelpMarker(
            "ON : fenêtre boutique ImGui unifiée (onglets Acheter/Vendre, saut "
            "du choix Acheter/Vendre natif).\nOFF : boutique NPC native classique.");
      }
      // ── Feuille de personnage (agrege Status + Equipement, en plus) ──────
      if (auto* cse = Bourgeon::Instance().character_sheet()) {
        if (ImGui::Checkbox("Feuille de perso (Alt+F)", &cse->imgui_enabled_))
          SaveSettings();
        ImGui::SameLine(); HelpMarker(
            "Fenêtre façon WoW : avatar + slots équipement/costume + stats, en "
            "COMPLÉMENT des fenêtres natives (conservées). Ouvre/ferme avec Alt+F.\n"
            "Clic gauche slot = description, clic droit = desequiper, boutons +stat.");
      }
      // ── Navigation latérale (liste à gauche, contenu à droite) ───────────
      // Remplace l'ancienne barre d'onglets : plus scalable quand les
      // catégories se multiplient (noms entiers, scroll vertical naturel).
      static int s_iface_nav = 0;
      static const char* kIfaceCats[] = {
          "Barres d'info", "Portrait",         "Barre d'action", "Icônes du menu",
          "Icônes de statut", "Suivi de quête", "Descriptions", "Skin RO", "Fenêtre NPC"};
      const float kNavH = 360.0f;
      ImGui::BeginChild("iface_nav", ImVec2(150.0f, kNavH), true);
      for (int i = 0; i < IM_ARRAYSIZE(kIfaceCats); ++i)
        if (ImGui::Selectable(kIfaceCats[i], s_iface_nav == i))
          s_iface_nav = i;
      ImGui::EndChild();
      ImGui::SameLine();
      ImGui::BeginChild("iface_content", ImVec2(0.0f, kNavH), false);
      ImGui::PushTextWrapPos(0.0f);  // wrap le texte à la largeur du child
      {
        // ── Barres d'info (HUD bars + alignment grid) ────────────────────────
        if (s_iface_nav == 0)
        {
          if (auto* eb = Bourgeon::Instance().basic_info()) {
            PushStyleCompact();
            if (ImGui::Checkbox("Afficher les barres", &eb->visible_))
              SaveSettings();
            ImGui::Indent();
            for (int i = 0; i < BasicInfoTweaks::kBarCount; ++i) {
              if (i) ImGui::SameLine();
              if (ImGui::Checkbox(BasicInfoTweaks::kBarLabels[i], &eb->bars_[i].show))
                SaveSettings();
            }
            ImGui::SameLine(); HelpMarker("Affiche/cache chaque barre indépendamment.");
            ImGui::Unindent();

            if (ImGui::Checkbox("Verrouiller (fige position/taille + clic-traversant)",
                                &eb->locked_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Verrouillée : les barres ne bougent plus et laissent passer les "
                "clics au jeu.\nDéverrouillée : glissez-les pour les déplacer, "
                "tirez le coin pour redimensionner.");

            if (ImGui::Checkbox("Aimanter les barres (snap)", &eb->sticky_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Quand tu glisses une barre près d'une autre, ses bords s'alignent "
                "et se collent automatiquement (~10px).\nÉloigne-la pour la "
                "détacher. Les barres restent indépendantes.");

            if (ImGui::Checkbox("Vertical", &eb->vertical_)) SaveSettings();
            ImGui::SameLine();
            if (ImGui::Checkbox("Bordure", &eb->border_)) SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Trait sombre 1px autour de chaque barre (HP/SP/EXP...). "
                "Décoche pour des barres sans contour.");

            const char* modes[] = {"Aucun", "Pourcentage", "Valeurs", "Les deux"};
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo("Texte", &eb->text_mode_, modes, IM_ARRAYSIZE(modes)))
              SaveSettings();

            ImGui::SetNextItemWidth(160.0f);
            if (WheelSliderFloat("Arrondi", &eb->rounding_, 0.0f, 16.0f, "%.0f", 1.0f))
              SaveSettings();
            ImGui::SameLine(); HelpMarker("Arrondi des coins des barres.");

            for (int i = 0; i < BasicInfoTweaks::kBarCount; ++i) {
              char lbl[32];
              std::snprintf(lbl, sizeof(lbl), "Couleur %s",
                            BasicInfoTweaks::kBarLabels[i]);
              if (ImGui::ColorEdit4(lbl, eb->bars_[i].fill,
                                    ImGuiColorEditFlags_NoInputs))
                SaveSettings();
            }
            if (ImGui::ColorEdit4("Fond / Opacité", eb->bg_color_,
                                  ImGuiColorEditFlags_NoInputs |
                                      ImGuiColorEditFlags_AlphaBar))
              SaveSettings();

            ImGui::TextUnformatted("Tailles rapides (toutes) :");
            auto preset = [&](const char* label, int w, int h) {
              ImGui::SameLine();
              if (ImGui::Button(label)) {
                for (int j = 0; j < BasicInfoTweaks::kBarCount; ++j) {
                  eb->bars_[j].w = w;
                  eb->bars_[j].h = h;
                }
                eb->force_apply_ = true;  // re-apply size even while unlocked
                SaveSettings();
              }
            };
            preset("XS", 200, 9);
            preset("S", 400, 16);
            preset("M", 600, 22);
            preset("L", 800, 30);
            PopStyleCompact();
          }
        }
        // ── Status Portrait (head + pseudo + classe + niveau, indépendants) ──
        if (s_iface_nav == 1)
        {
          if (auto* eb = Bourgeon::Instance().basic_info()) {
            PushStyleCompact();
            if (ImGui::Checkbox("Afficher le portrait", &eb->portrait_visible_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Portrait de statut : la tête du personnage, le pseudo, la classe "
                "et le niveau sont des éléments INDÉPENDANTS — chacun déplaçable, "
                "redimensionnable, avec sa couleur/opacité de fond et son arrondi.\n"
                "(Le sprite de tête arrive bientôt.)");

            if (ImGui::Checkbox("Verrouiller (fige + clic-traversant)",
                                &eb->portrait_locked_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Verrouillé : les éléments ne bougent plus et laissent passer les "
                "clics au jeu.\nDéverrouillé : glisse pour déplacer, tire un bord/"
                "coin pour redimensionner (aimantage à la grille d'alignement).");

            if (ImGui::Checkbox("Sprite de tête (sinon placeholder)",
                                &eb->portrait_head_sprite_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Régénère la tête du personnage via le moteur de sprites du jeu "
                "et l'affiche dans l'élément Portrait.");
            if (ImGui::Checkbox("Tête seule (sans le corps)",
                                &eb->portrait_head_only_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Ne garde que les couches de la tête (visage/cheveux/coiffes) et "
                "retire le corps. Décoche pour le personnage entier.");
            ImGui::SameLine();
            if (ImGui::Checkbox("Bordure", &eb->portrait_border_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker("Trait noir 1px autour de chaque cadre.");

            // Live framing of the head sprite (zoom + vertical focus).
            ImGui::SetNextItemWidth(160.0f);
            if (WheelSliderFloat("Zoom tête", &eb->portrait_head_zoom_, 0.10f,
                                   2.0f, "%.2f", 0.01f))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Zoom dans la tête (1 = corps entier). Ajuste avec le décalage "
                "vertical pour cadrer le visage.");
            ImGui::SetNextItemWidth(160.0f);
            if (WheelSliderFloat("Décalage horiz.", &eb->portrait_head_offx_,
                                   -1.5f, 1.5f, "%.2f", 0.01f))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Décale le portrait horizontalement (0 = centré). Sert à aligner "
                "la tête/le corps ; le zoom reste centré.");
            ImGui::SetNextItemWidth(160.0f);
            if (WheelSliderFloat("Décalage vert.", &eb->portrait_head_offy_,
                                   -1.5f, 1.5f, "%.2f", 0.01f))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Décale le portrait verticalement (0 = centré). Optionnel — le "
                "zoom reste centré ; laisse à 0 si tu n'en as pas besoin.");

            // Animation pose (animType): the frame count auto-adapts per action.
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo("Animation", &eb->portrait_anim_,
                             "Repos\0Marche\0Assis\0Ramasser\0Combat\0Attaque\0"
                             "Touché\0Gelé\0Mort\0"))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Pose animée du portrait (Combat = posture prête au combat). "
                "Le nombre d'images de l'animation s'ajuste automatiquement.");
            // Facing direction (low 3 bits of the pose) + play/freeze toggle.
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo("Direction", &eb->portrait_dir_,
                             "Face (0)\0Diag. 1\0Côté (2)\0Diag. 3\0Dos (4)\0"
                             "Diag. 5\0Côté (6)\0Diag. 7\0"))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Oriente le portrait. 0 = face. Essaie les valeurs pour trouver "
                "l'angle voulu (le rendu se met à jour en direct).");
            if (ImGui::Checkbox("Animer", &eb->portrait_animate_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Joue les images de l'animation (ex. le balayage de la posture "
                "Combat). Décoche pour figer une pose calme (image 0).");
            ImGui::SameLine();
            if (ImGui::Checkbox("Cape / garment", &eb->portrait_show_garment_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Affiche la cape/garment équipée (seulement en mode corps "
                "entier — décoche \"Tête seule\" pour la voir).");

            ImGui::Separator();
            // Per-element config: show / background colour+opacity / rounding /
            // text colour.  Each element is independent.
            for (int i = 0; i < BasicInfoTweaks::kPortCount; ++i) {
              auto& e = eb->ports_[i];
              ImGui::PushID(i);
              if (ImGui::Checkbox(BasicInfoTweaks::kPortLabels[i], &e.show))
                SaveSettings();
              ImGui::Indent();
              if (ImGui::ColorEdit4("Fond / Opacité", e.bg,
                                    ImGuiColorEditFlags_NoInputs |
                                        ImGuiColorEditFlags_AlphaBar))
                SaveSettings();
              if (i != BasicInfoTweaks::kPortHead) {
                ImGui::SameLine();
                if (ImGui::ColorEdit4("Texte", e.fg, ImGuiColorEditFlags_NoInputs))
                  SaveSettings();
              }
              ImGui::SetNextItemWidth(160.0f);
              if (WheelSliderFloat("Arrondi", &e.rounding, 0.0f, 16.0f, "%.0f", 1.0f))
                SaveSettings();
              ImGui::Unindent();
              ImGui::PopID();
            }

            ImGui::Separator();
            if (ImGui::Checkbox("Masquer la fenêtre Basic Info d'origine",
                                &eb->portrait_hide_basic_info_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Masque la fenêtre native \"Basic Info\" (déplacée hors écran) une "
                "fois ton portrait en place. Décoche pour la restaurer.");
            PopStyleCompact();
          }
        }
        // ── Barre d'action (skill bar ImGui : 3 barres fixes Onglet1/Onglet2/Items) ──
        if (s_iface_nav == 2)
        {
          if (auto* sb = Bourgeon::Instance().skill_bar())
            sb->DrawSettingsContent();
        }
        // ── Menu icons (ImGui replacement) ───────────────────────────────────
        if (s_iface_nav == 3)
        {
          if (auto* mi = Bourgeon::Instance().menu_icons()) {
            if (ImGui::Checkbox("Remplacer par des icônes ImGui", &mi->enabled_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Cache la grille native et recrée les icônes fonctionnelles en "
                "ImGui (cliquables + tooltip + masquage par icône).");

            if (ImGui::Checkbox("Mode édition (glisser pour déplacer)",
                                &mi->edit_mode_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "En mode édition : glisse chaque icône pour la repositionner.\n"
                "Aimantage aux autres icônes + à la grille d'alignement (réglages "
                "Interface de jeu : grille/snap).\nDésactive le mode pour cliquer "
                "les icônes normalement.");

            // Per-icon show/hide. icons() is populated once in-game.
            if (ImGui::TreeNode("Afficher / masquer les icônes")) {
              auto& icons = mi->icons();
              if (icons.empty()) {
                ImGui::TextDisabled("(disponible une fois en jeu)");
              } else {
                for (auto& ic : icons) {
                  bool shown = !ic.hidden;
                  ImGui::PushID(ic.cmd_id);
                  if (ImGui::Checkbox(ic.name, &shown)) {
                    ic.hidden = !shown;
                    mi->saved_[ic.name] = {ic.x, ic.y, ic.hidden, true};
                    SaveSettings();
                  }
                  ImGui::PopID();
                }
              }
              ImGui::TreePop();
            }
          }
        }
        // ── Status icons (StatusIconTweaks) ──────────────────────────────────
        if (s_iface_nav == 4)
        {
          if (auto* si = Bourgeon::Instance().status_icons())
            si->DrawSettings();
          else
            ImGui::TextDisabled("(plugin indisponible)");
        }
        // ── Suivi de quête (QuestTrackerTweaks) ──────────────────────────────
        if (s_iface_nav == 5)
        {
          if (auto* qt = Bourgeon::Instance().quest_tracker())
            qt->DrawSettings();
          else
            ImGui::TextDisabled("(plugin indisponible)");
        }
        // ── Descriptions (ItemDescTweaks : panneaux techniques item/skill) ────
        if (s_iface_nav == 6)
        {
          if (auto* idt = Bourgeon::Instance().item_desc()) {
            ImGui::TextUnformatted(
                "Panneaux d'infos techniques affichés à côté des fenêtres de "
                "description natives (item et skill).");
            if (ImGui::Checkbox("Panneau technique des items",
                                &idt->show_item_panel()))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Affiche le panneau enrichi description d'un ITEM "
                "(clic droit item).");
            if (ImGui::Checkbox("Panneau technique des skills",
                                &idt->show_skill_panel()))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Affiche le panneau enrichi à côté de la description d'un SKILL "
                "(clic droit dans le grimoire).");
            ImGui::Separator();
            if (ImGui::Checkbox("Ouvrir près de la souris",
                                &idt->desc_spawn_at_cursor()))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "ON : la description apparaît près du curseur à chaque ouverture.\n"
                "OFF : elle réapparaît à sa dernière position connue.");
            if (idt->desc_spawn_at_cursor()) {
              ImGui::Indent();
              // Ancrage : quel coin/point de la fenêtre se pose sur le curseur.
              const char* kAnchors[] = {"Haut-gauche", "Haut-droite", "Bas-gauche",
                                        "Bas-droite", "Centre"};
              ImGui::SetNextItemWidth(160.0f);
              if (ImGui::Combo("Ancrage", &idt->desc_anchor(), kAnchors, 5))
                SaveSettings();
              // Sliders X/Y MOLETTABLES : WheelSliderInt claime la molette au survol
              // (SetItemKeyOwner(MouseWheelY)) -> la molette n'ajuste QUE le slider, la
              // fenêtre de réglages ne défile pas en même temps.
              ImGui::SetNextItemWidth(160.0f);
              if (WheelSliderInt("Offset X", &idt->desc_offset_x(), -400, 400, "%d px"))
                SaveSettings();
              ImGui::SetNextItemWidth(160.0f);
              if (WheelSliderInt("Offset Y", &idt->desc_offset_y(), -400, 400, "%d px"))
                SaveSettings();
              ImGui::SameLine(); HelpMarker(
                  "Décalage depuis le curseur (molette au survol pour ajuster).");
              ImGui::Unindent();
            }
          } else {
            ImGui::TextDisabled("(plugin indisponible)");
          }
          // Bouton « Signaler un bug » (desc item/skill + dialogue PNJ + raccourci).
          if (auto* br = Bourgeon::Instance().bug_report()) {
            ImGui::Separator();
            if (ImGui::Checkbox("Bouton « Signaler un bug »", &br->enabled()))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Affiche le bouton de rapport de bug dans les fenêtres de "
                "description (item/skill) et le dialogue PNJ, et active le "
                "raccourci Ctrl+Alt+B. Décoche pour tout désactiver.");
          }
        }
        // ── Skin RO (police + habillage des fenêtres ImGui) ──────────────────
        if (s_iface_nav == 7)
        {
          bool font_on = ro::IsFontEnabled();
          if (ImGui::Checkbox("Police Malgun (UI)", &font_on)) {
            ro::SetFontEnabled(font_on);
            SaveSettings();
          }
          ImGui::SameLine(); HelpMarker(
              "ON : police Malgun Gothic pour toute l'UI ImGui (latin + coreen).\n"
              "OFF : police integree d'ImGui (ProggyClean).");
          bool skin_on = ro::IsSkinEnabled();
          if (ImGui::Checkbox("Skin RO (fenêtres claires)", &skin_on)) {
            ro::SetSkinEnabled(skin_on);
            SaveSettings();
          }
          ImGui::SameLine(); HelpMarker(
              "ON : les fenêtres ImGui 'RO' utilisent la barre de titre et les "
              "boutons du client.\nOFF : chrome ImGui standard.");
          ImGui::Separator();
          if (ro::ShowRoSkinSettings()) SaveSettings();

          // ── Presets : jeux de couleurs sauvegardés ────────────────────────
          ImGui::Separator();
          ImGui::TextUnformatted("Presets");
          const int npreset = static_cast<int>(g_ro_presets.size());
          const bool valid_sel = g_ro_preset_sel >= 0 && g_ro_preset_sel < npreset;
          const char* preview = valid_sel ? g_ro_presets[g_ro_preset_sel].name.c_str()
                                          : "(choisir)";
          ImGui::SetNextItemWidth(160.0f);
          if (ImGui::BeginCombo("##ro_preset", preview)) {
            for (int i = 0; i < npreset; ++i)
              if (ImGui::Selectable(g_ro_presets[i].name.c_str(), g_ro_preset_sel == i))
                g_ro_preset_sel = i;
            ImGui::EndCombo();
          }
          ImGui::SameLine();
          if (ImGui::Button("Appliquer") && valid_sel) {
            ro::SkinConfig() = g_ro_presets[g_ro_preset_sel].cfg;
            SaveSettings();
          }
          ImGui::SameLine();
          if (ImGui::Button("Supprimer") && valid_sel) {
            g_ro_presets.erase(g_ro_presets.begin() + g_ro_preset_sel);
            g_ro_preset_sel = -1;
            SaveSettings();
          }
          static char preset_name[32] = "";
          ImGui::SetNextItemWidth(160.0f);
          ImGui::InputText("##ro_preset_name", preset_name, sizeof(preset_name));
          ImGui::SameLine();
          if (ImGui::Button("Sauvegarder") && preset_name[0]) {
            bool found = false;
            for (auto& p : g_ro_presets)
              if (p.name == preset_name) { p.cfg = ro::SkinConfig(); found = true; break; }
            if (!found) g_ro_presets.push_back({preset_name, ro::SkinConfig()});
            SaveSettings();
            preset_name[0] = '\0';
          }
          ImGui::SameLine();
          HelpMarker("Sauvegarde les couleurs/luminosite/opacite actuelles sous un "
                     "nom. 'Appliquer' recharge un preset ; les joueurs peuvent se "
                     "faire plusieurs themes.");
        }
        // ── Fenêtre NPC (dialogue / menu / prompt ImGui) ─────────────────────
        if (s_iface_nav == 8)
        {
          if (auto* nd = Bourgeon::Instance().npc_dialog_tweaks()) {
            if (ImGui::Checkbox("Dialogue NPC ImGui", &nd->imgui_enabled_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Remplace le dialogue / menu / prompt NPC natif par un overlay ImGui "
                "(texte en couleur, menu à navigation clavier : flèches + Entrée, "
                "touches 1-9). Opt-in ; la fenêtre native est cachée quand c'est actif.");
            ImGui::BeginDisabled(!nd->imgui_enabled_);
            if (ImGui::Checkbox("Barre de recherche du menu", &nd->menu_search_))
              SaveSettings();
            ImGui::SameLine(); HelpMarker(
                "Affiche un champ de recherche au-dessus des longs menus (plus de 8 "
                "choix) pour filtrer les options. Décoche pour un menu épuré.");
            ImGui::EndDisabled();
          }
        }
      }
      ImGui::PopTextWrapPos();
      ImGui::EndChild();
      PopStyleCompact();
    }
    // ── Graphismes (color grading post-process, SettingsTweaks plugin) ───────
    if (ImGui::CollapsingHeader("Graphismes")) {
      PushStyleCompact();
      if (auto* st = Bourgeon::Instance().settings_tweaks())
        st->DrawSettings();
      PopStyleCompact();
    }

    // ── Commands Settings ────────────────────────────────────────────────────
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
            TriCombo("Tri Storages",     tri_storage_, kSettingTriStorage);
            ImGui::SameLine(); HelpMarker("Tri automatique des Storages personnel à la prochaine ouverture.");
            TriCombo("Tri Storage Guilde", tri_gstorage_, kSettingTriGstorage);
            ImGui::SameLine(); HelpMarker("Tri automatique du Storage de guilde à la prochaine ouverture.");
          }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Autoloots"))
        {
          ImGui::Spacing();
          {// @autoloot
            int rate = aloot_rate_;
            ImGui::SetNextItemWidth(130.0f);
            if (WheelSliderInt("@autoloot", &rate, 0, 100, "%d%%")) {
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
          if (ImGui::Checkbox("Autoloot MVP", &aloot_mvp_)) SendSetting(kSettingAlootMvp, aloot_mvp_ ? 1 : 0);
          ImGui::SameLine(); HelpMarker("Loot automatiquement les MVP\nquelque soit leur taux de drop. (@autolootmvp)");
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
  ro::EndRoWindow();
  ImGui::PopStyleVar(4);

  // ── Alootid floating overlay ───────────────────────────────────────────────
  // Detect silent tooltip close (e.g. comparison→non-comparison): the game
  // zeroes kItemDescWndGlobalPtr without sending our hook a close message.
  if (g_item_desc_visible &&
      *reinterpret_cast<const uintptr_t*>(kItemDescWndGlobalPtr) == 0) {
    g_item_desc_visible = false;
    g_item_desc_wnd_ptr = nullptr;
  }

  // Quand les descriptions enrichies (Option A) sont actives, la fenêtre native
  // est cachée -> l'overlay alootid autonome n'aurait pas d'ancrage cohérent et
  // fera doublon avec le bouton qui sera réintégré DANS le cadre enrichi. On le
  // désactive donc tant que le panneau item enrichi est activé.
  bool enriched_item = false;
  if (auto* idt = Bourgeon::Instance().item_desc())
    enriched_item = idt->show_item_panel();

  if (show_alootid_overlay_ && !enriched_item &&
      g_last_viewed_item != 0 && g_item_desc_visible) {
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

  // ── Main-chat quick preset switcher (compact, draggable, resizable) ────────
  if (mainchat_preset_bar_ && chat_bg_found_) {
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowSize(ImVec2(80.0f, 10.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(40.0f, 1.0f), ImVec2(8000.0f, 8000.0f));
    // Match the main Moonlight-Destiny window's frame/grab/window rounding.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    // Lower the per-window minimum size (default 32x32) so this bar can be made
    // as thin as a single preset row.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(40.0f, 1.0f));
    // No title bar (minimalist). Still draggable from the body and resizable.
    if (ImGui::Begin("Chat presets", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoNav)) {
      PushStyleCompact();
      ChatBgGroup& g = chat_bg_[kChatBgMain];
      if (chat_bg_presets_.empty()) {
        ImGui::TextDisabled("No presets yet.");
        ImGui::TextDisabled("Add some in the");
        ImGui::TextDisabled("Main chat picker.");
      } else {
        for (int i = 0; i < static_cast<int>(chat_bg_presets_.size()); ++i) {
          const auto& p = chat_bg_presets_[i];
          const ImVec4 col(((p.argb >> 16) & 0xFF) / 255.0f,
                           ((p.argb >>  8) & 0xFF) / 255.0f,
                           ( p.argb        & 0xFF) / 255.0f,
                           ((p.argb >> 24) & 0xFF) / 255.0f);
          ImGui::PushID(i);
          if (ImGui::ColorButton("##sw", col,
                                 ImGuiColorEditFlags_AlphaPreview |
                                 ImGuiColorEditFlags_NoTooltip,
                                 ImVec2(14, 14))) {
            PickerFromArgb(g.color, p.argb);
            ApplyChatBg(g, p.argb, true);
            SaveSettings();
          }
          ImGui::SameLine();
          ImGui::TextUnformatted(p.name.c_str());
          ImGui::PopID();
          ImGui::SameLine();
        }
      }
      PopStyleCompact();
    }
    ImGui::End();
    ImGui::PopStyleVar(6);
    // Closing is done by un-ticking the "Preset bar" checkbox (no title-bar [x]).
  }
}
