#include "plugins/moonlight_ui.h"

#include "plugins/moonlight_ui/internal.h"  // panneaux extraits (dossier privé)

#include <Windows.h>
#include <algorithm>
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
#include "ui/ro_imgui.h"
#include "plugins/chat.h"
#include "plugins/discord_relay.h"
#include "plugins/basic_info.h"
#include "plugins/dps_meter.h"
#include "plugins/menu_icons.h"
#include "plugins/status_icon_tweaks.h"
#include "plugins/quest_tracker_tweaks.h"
#include "plugins/settings_tweaks.h"
#include "plugins/entity_names.h"
#include "plugins/skill_bar_tweaks.h"
#include "plugins/storage_tweaks.h"
#include "plugins/inventory_viewer.h"
#include "plugins/cashshop_tweaks.h"
#include "plugins/shop_tweaks.h"
#include "plugins/trade_tweaks.h"
#include "plugins/npc_dialog_tweaks.h"
#include "plugins/bug_report.h"
#include "plugins/character_sheet.h"
#include "plugins/login_parade.h"
#include "plugins/doom_tweaks.h"
#include "plugins/roggle_tweaks.h"
#include "plugins/rojeweled_tweaks.h"
#include "plugins/keyboard_move.h"
#include "plugins/player_jump.h"
#include "plugins/status_tweaks.h"
#include "plugins/equip_tweaks.h"
#include "plugins/window_pos_tweaks.h"
#include "plugins/weapon_dual_sprites.h"
#include "plugins/spr_effect_lab.h"
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
  if (n["list"]) UnpackCol(n["list"].as<unsigned>(0), c.list_col);
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

// Niveau de groupe serveur du compte courant, reçu au login via le setting id 26
// (ZC_BOURGEON_SETTINGS), rempli depuis pc_get_group_level(sd). Non persisté :
// autoritatif serveur, rafraîchi à chaque login. File-static (pas un membre de
// MoonlightUi) pour ne pas changer le layout de la classe. Le seuil « staff »
// est appliqué dans IsStaff() ci-dessous.
static int g_staff_level = 0;

// Seuil de niveau de groupe serveur à partir duquel un compte est considéré
// « staff ». 80 pour ce serveur (des groupes non-staff peuvent avoir un level
// > 0 ; les vrais pouvoirs GM y sont gatés vers 60/90).
static constexpr int kStaffMinGroupLevel = 80;

// Staff = niveau de groupe serveur >= seuil (reçu au login via le setting id 26).
// Gate PUREMENT serveur : si le serveur n'envoie pas l'id 26 (sources pas à
// jour), la fonctionnalité reste masquée, y compris sur un poste dev.
bool IsStaff() { return g_staff_level >= kStaffMinGroupLevel; }

using ItemDescWndFn = int (__fastcall*)(void*, void*, uint32_t, int, int*, int, int, int);
static ItemDescWndFn g_item_desc_wnd_orig  = nullptr;
// Non-static : lu par le panneau Autoloots (moonlight_ui/panel_commands.cc) pour
// proposer l'item survolé. Déclaré extern dans moonlight_ui/internal.h.
uint32_t             g_last_viewed_item    = 0;
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

// « Tout-ImGui ou tout-natif » (cf. déclaration dans moonlight_ui.h) : synchronise
// les 4 fenêtres modernes interdépendantes en un point unique (inventaire, storage,
// barres de skill, échange). Chaque plugin garde son propre flag, mais il n'est plus
// jamais basculé isolément.
void SetModernInterface(bool on) {
  if (auto* iv  = Bourgeon::Instance().inventory_viewer()) iv->imgui_enabled_ = on;
  if (auto* stg = Bourgeon::Instance().storage_tweaks())   stg->imgui_enabled_ = on;
  if (auto* sb  = Bourgeon::Instance().skill_bar())        sb->enabled_ = on;
  if (auto* tt  = Bourgeon::Instance().trade_tweaks())     tt->imgui_enabled_ = on;
}

// ── Settings persistence ──────────────────────────────────────────────────

void MoonlightUi::LoadSettings() {
  const std::string path = GetSettingsPath();
  // Horodatage de COMPILATION, gardé volontairement (une ligne par login) : le
  // déploiement POST_BUILD est best-effort et SILENCIEUSEMENT sauté quand le jeu tient
  // ddraw.dll ouvert (cf. src/CMakeLists.txt) — le build passe au vert sans rien
  // déployer. Cette ligne dit immédiatement quelle DLL tourne réellement.
  LogInfo("[Bourgeon] build " __DATE__ " " __TIME__);
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

    // « Sol uni » du SPR Lab (fond de capture) : couleur en ARGB hex, même convention
    // que les autres couleurs persistées ici.
    spr_lab::ground_paint_enabled() = ui["ground_paint"].as<bool>(false);
    {
      const std::string hex = ui["ground_paint_color"].as<std::string>("");
      if (hex.size() == 8)
        PickerFromArgb(spr_lab::ground_color(),
                       static_cast<uint32_t>(std::stoul(hex, nullptr, 16)));
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
    if (auto* wds = Bourgeon::Instance().weapon_dual_sprites())
      wds->enabled() = ui["weapon_dual_sprites"].as<bool>(false);
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
      eb->visible_   = ui["expbar_visible"].as<bool>(false);
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
    // (« ro_skin » : clé abandonnée — le skin RO est désormais toujours actif. Une
    // ancienne valeur false dans le yaml est simplement ignorée.)
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
      load_col("ro_skin_list", sc.list_col);
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
    if (auto* iv = Bourgeon::Instance().inventory_viewer()) {
      iv->imgui_enabled_ = ui["inventory_imgui"].as<bool>(iv->imgui_enabled_);
      iv->show_filter()   = ui["inventory_filter"].as<bool>(iv->show_filter());
      iv->desc_tooltip()  =
          ui["inventory_desc_tooltip"].as<bool>(iv->desc_tooltip());
      iv->tabs_vertical() =
          ui["inventory_tabs_vertical"].as<bool>(iv->tabs_vertical());
      iv->lock_size()   = ui["inventory_lock_size"].as<bool>(iv->lock_size());
      iv->free_layout() = ui["inventory_free_layout"].as<bool>(iv->free_layout());
      // Placement libre : map nameid -> index de case (client-side, comme les
      // favoris du storage).
      if (const YAML::Node lay = ui["inventory_layout"]) {
        iv->layout_.clear();
        for (auto it = lay.begin(); it != lay.end(); ++it) {
          const uint32_t id = it->first.as<uint32_t>(0);
          const int cell = it->second.as<int>(-1);
          if (id != 0 && cell >= 0) iv->layout_[id] = cell;
        }
      }
    }
    if (auto* stg = Bourgeon::Instance().storage_tweaks()) {
      stg->imgui_enabled_ = ui["storage_imgui"].as<bool>(stg->imgui_enabled_);
      stg->desc_tooltip() =
          ui["storage_desc_tooltip"].as<bool>(stg->desc_tooltip());
      stg->show_filter()    = ui["storage_filter"].as<bool>(stg->show_filter());
      stg->tabs_vertical()  = ui["storage_tabs_vertical"].as<bool>(stg->tabs_vertical());
      stg->tab_images()     = ui["storage_tab_images"].as<bool>(stg->tab_images());
      stg->show_index_col() = ui["storage_col_index"].as<bool>(stg->show_index_col());
      stg->show_id_col()    = ui["storage_col_id"].as<bool>(stg->show_id_col());
      stg->show_slots_col() = ui["storage_col_slots"].as<bool>(stg->show_slots_col());
      stg->show_value_col() = ui["storage_col_value"].as<bool>(stg->show_value_col());
      stg->show_total_value() =
          ui["storage_total_value"].as<bool>(stg->show_total_value());
      stg->cur_tab()        = ui["storage_tab"].as<int>(stg->cur_tab());
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
    if (auto* tt = Bourgeon::Instance().trade_tweaks())
      tt->imgui_enabled_ = ui["trade_imgui"].as<bool>(tt->imgui_enabled_);
    if (auto* nd = Bourgeon::Instance().npc_dialog_tweaks()) {
      nd->imgui_enabled_ = ui["npc_dialog_imgui"].as<bool>(nd->imgui_enabled_);
      nd->menu_search_ = ui["npc_menu_search"].as<bool>(nd->menu_search_);
    }
    if (auto* cse = Bourgeon::Instance().character_sheet()) {
      cse->imgui_enabled_ = ui["charsheet_imgui"].as<bool>(cse->imgui_enabled_);
      cse->set_open(ui["charsheet_open"].as<bool>(cse->is_open()));
      // Pose de l'avatar (pose/direction/animation) — persistee par personne.
      cse->avatar_anim()    = ui["charsheet_pose"].as<int>(cse->avatar_anim());
      cse->avatar_dir()     = ui["charsheet_dir"].as<int>(cse->avatar_dir());
      cse->avatar_animate() = ui["charsheet_pose_anim"].as<bool>(cse->avatar_animate());
    }
    if (auto* lp = Bourgeon::Instance().login_parade()) {
      lp->enabled_ = ui["login_parade"].as<bool>(lp->enabled_);
    }
    if (auto* sb = Bourgeon::Instance().skill_bar()) {
      sb->enabled_    = ui["skillbar_enabled"].as<bool>(sb->enabled_);
      sb->locked_     = ui["skillbar_locked"].as<bool>(sb->locked_);
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

    // « Tout-ImGui ou tout-natif » : ces 4 fenêtres (inventaire/storage/barres/
    // échange) s'activent ensemble. Un yaml antérieur au regroupement pouvait être
    // mixé — on réconcilie en OR (au moins une moderne => toutes modernes ; tout
    // natif sinon), puis les cases restent synchronisées à l'exécution.
    {
      auto* iv  = Bourgeon::Instance().inventory_viewer();
      auto* stg = Bourgeon::Instance().storage_tweaks();
      auto* sb2 = Bourgeon::Instance().skill_bar();
      auto* tt2 = Bourgeon::Instance().trade_tweaks();
      const bool modern = (iv  && iv->imgui_enabled_)  ||
                          (stg && stg->imgui_enabled_) ||
                          (sb2 && sb2->enabled_)       ||
                          (tt2 && tt2->imgui_enabled_);
      SetModernInterface(modern);
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
      c.icon_size      = ui["statusicon_icon_size"].as<int>(c.icon_size);
      c.time_place     = ui["statusicon_time_place"].as<int>(c.time_place);
      c.time_anchor    = ui["statusicon_time_anchor"].as<int>(c.time_anchor);
      c.time_bold      = ui["statusicon_time_bold"].as<bool>(c.time_bold);
      if (ui["statusicon_time_text"])
        UnpackCol(ui["statusicon_time_text"].as<unsigned>(0), c.col_time_text);
      if (ui["statusicon_time_shadow"])
        UnpackCol(ui["statusicon_time_shadow"].as<unsigned>(0), c.col_time_shadow);
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

    if (auto* en = Bourgeon::Instance().entity_names()) {
      en->enabled()       = ui["entnames_enabled"].as<bool>(false);
      en->show_players()  = ui["entnames_players"].as<bool>(true);
      en->show_monsters() = ui["entnames_monsters"].as<bool>(false);
      en->show_npcs()     = ui["entnames_npcs"].as<bool>(false);
      en->show_self()     = ui["entnames_self"].as<bool>(false);
      en->outline()       = ui["entnames_outline"].as<bool>(true);
      en->y_offset()      = ui["entnames_yoffset"].as<int>(2);
      en->font_scale()    = ui["entnames_fontscale"].as<float>(1.0f);
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

void MoonlightUi::WriteSettingsFile() {
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
  char ground_hex[9];
  std::snprintf(ground_hex, sizeof(ground_hex), "%08X",
                ArgbFromPicker(spr_lab::ground_color()));
  out     << YAML::Key << "ground_paint"         << YAML::Value
              << spr_lab::ground_paint_enabled()
        << YAML::Key << "ground_paint_color"   << YAML::Value << ground_hex
        << YAML::Key << "ui_collapsed"          << YAML::Value << ui_collapsed_
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
        << YAML::Key << "weapon_dual_sprites"  << YAML::Value
            << (Bourgeon::Instance().weapon_dual_sprites()
                    ? Bourgeon::Instance().weapon_dual_sprites()->enabled()
                    : false)
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
  out << YAML::Key << "expbar_visible"  << YAML::Value << (eb ? eb->visible_ : false)
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
        << YAML::Key << "statusicon_icon_alpha"     << YAML::Value << c.icon_alpha
        << YAML::Key << "statusicon_icon_size"      << YAML::Value << c.icon_size
        << YAML::Key << "statusicon_time_place"     << YAML::Value << c.time_place
        << YAML::Key << "statusicon_time_anchor"    << YAML::Value << c.time_anchor
        << YAML::Key << "statusicon_time_bold"      << YAML::Value << c.time_bold
        << YAML::Key << "statusicon_time_text"      << YAML::Value << PackCol(c.col_time_text)
        << YAML::Key << "statusicon_time_shadow"    << YAML::Value << PackCol(c.col_time_shadow);
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
    auto* en = Bourgeon::Instance().entity_names();
    out << YAML::Key << "entnames_enabled"   << YAML::Value << (en ? en->enabled() : false)
        << YAML::Key << "entnames_players"   << YAML::Value << (en ? en->show_players() : true)
        << YAML::Key << "entnames_monsters"  << YAML::Value << (en ? en->show_monsters() : false)
        << YAML::Key << "entnames_npcs"      << YAML::Value << (en ? en->show_npcs() : false)
        << YAML::Key << "entnames_self"      << YAML::Value << (en ? en->show_self() : false)
        << YAML::Key << "entnames_outline"   << YAML::Value << (en ? en->outline() : true)
        << YAML::Key << "entnames_yoffset"   << YAML::Value << (en ? en->y_offset() : 2)
        << YAML::Key << "entnames_fontscale" << YAML::Value << (en ? en->font_scale() : 1.0f);
  }

  {
    out << YAML::Key << "malgun_font" << YAML::Value << ro::IsFontEnabled();
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
      out << YAML::Key << "ro_skin_list" << YAML::Value << pk(sc.list_col);
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
      out << YAML::Key << "list" << YAML::Value << PackCol(p.cfg.list_col);
      out << YAML::EndMap;
    }
    out << YAML::EndSeq;
    auto* iv = Bourgeon::Instance().inventory_viewer();
    out << YAML::Key << "inventory_imgui" << YAML::Value << (iv ? iv->imgui_enabled_ : false);
    out << YAML::Key << "inventory_filter" << YAML::Value << (iv ? iv->show_filter() : true);
    out << YAML::Key << "inventory_desc_tooltip" << YAML::Value
        << (iv ? iv->desc_tooltip() : false);
    out << YAML::Key << "inventory_tabs_vertical" << YAML::Value
        << (iv ? iv->tabs_vertical() : true);
    out << YAML::Key << "inventory_lock_size" << YAML::Value << (iv ? iv->lock_size() : false);
    out << YAML::Key << "inventory_free_layout" << YAML::Value << (iv ? iv->free_layout() : false);
    // Placement libre : nameid -> case. Trié pour un yaml stable (pas de diff parasite).
    out << YAML::Key << "inventory_layout" << YAML::Value << YAML::Flow << YAML::BeginMap;
    if (iv) {
      std::vector<std::pair<uint32_t, int>> lay(iv->layout_.begin(), iv->layout_.end());
      std::sort(lay.begin(), lay.end());
      for (const auto& e : lay) out << YAML::Key << e.first << YAML::Value << e.second;
    }
    out << YAML::EndMap;
    auto* stg = Bourgeon::Instance().storage_tweaks();
    out << YAML::Key << "storage_imgui" << YAML::Value << (stg ? stg->imgui_enabled_ : false);
    out << YAML::Key << "storage_desc_tooltip" << YAML::Value
        << (stg ? stg->desc_tooltip() : false);
    out << YAML::Key << "storage_filter"    << YAML::Value << (stg ? stg->show_filter() : true);
    out << YAML::Key << "storage_tabs_vertical" << YAML::Value << (stg ? stg->tabs_vertical() : false);
    out << YAML::Key << "storage_tab_images" << YAML::Value << (stg ? stg->tab_images() : true);
    out << YAML::Key << "storage_col_index" << YAML::Value << (stg ? stg->show_index_col() : false);
    out << YAML::Key << "storage_col_id"    << YAML::Value << (stg ? stg->show_id_col() : false);
    out << YAML::Key << "storage_col_slots" << YAML::Value << (stg ? stg->show_slots_col() : false);
    out << YAML::Key << "storage_col_value" << YAML::Value << (stg ? stg->show_value_col() : true);
    out << YAML::Key << "storage_total_value" << YAML::Value << (stg ? stg->show_total_value() : true);
    out << YAML::Key << "storage_tab"       << YAML::Value << (stg ? stg->cur_tab() : 0);
    // Favoris storage (ids d'items, triés pour un yaml stable = pas de diff parasite).
    out << YAML::Key << "storage_favorites" << YAML::Value << YAML::Flow << YAML::BeginSeq;
    if (stg) {
      std::vector<uint32_t> favs(stg->favorites_.begin(), stg->favorites_.end());
      std::sort(favs.begin(), favs.end());
      for (uint32_t id : favs) out << id;
    }
    out << YAML::EndSeq;
    auto* cs = Bourgeon::Instance().cashshop_tweaks();
    out << YAML::Key << "cashshop_imgui" << YAML::Value << (cs ? cs->imgui_enabled_ : false);
    auto* sh = Bourgeon::Instance().shop_tweaks();
    out << YAML::Key << "shop_imgui" << YAML::Value << (sh ? sh->imgui_enabled_ : false);
    auto* tt = Bourgeon::Instance().trade_tweaks();
    out << YAML::Key << "trade_imgui" << YAML::Value << (tt ? tt->imgui_enabled_ : false);
    auto* nd = Bourgeon::Instance().npc_dialog_tweaks();
    out << YAML::Key << "npc_dialog_imgui" << YAML::Value
        << (nd ? nd->imgui_enabled_ : false);
    out << YAML::Key << "npc_menu_search" << YAML::Value
        << (nd ? nd->menu_search_ : true);
    auto* cse = Bourgeon::Instance().character_sheet();
    out << YAML::Key << "charsheet_imgui" << YAML::Value
        << (cse ? cse->imgui_enabled_ : false);
    out << YAML::Key << "charsheet_open" << YAML::Value
        << (cse ? cse->is_open() : true);
    // Pose de l'avatar (pose/direction/animation).
    out << YAML::Key << "charsheet_pose" << YAML::Value
        << (cse ? cse->avatar_anim() : 4);
    out << YAML::Key << "charsheet_dir" << YAML::Value
        << (cse ? cse->avatar_dir() : 0);
    out << YAML::Key << "charsheet_pose_anim" << YAML::Value
        << (cse ? cse->avatar_animate() : true);
    auto* lp = Bourgeon::Instance().login_parade();
    out << YAML::Key << "login_parade" << YAML::Value
        << (lp ? lp->enabled_ : true);
  }

  {
    auto* sb = Bourgeon::Instance().skill_bar();
    out << YAML::Key << "skillbar_enabled"  << YAML::Value << (sb ? sb->enabled_    : false)
        << YAML::Key << "skillbar_locked"   << YAML::Value << (sb ? sb->locked_     : true)
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

  // bourgeon_settings.yaml est PARTAGÉ : AutoLogin (section « auto_login »), CharSelect
  // (« char_select ») et MoonlightAuth (« moonlight_auth ») y lisent chacun leur propre
  // section racine et ne la réécrivent JAMAIS. Écrire `out` directement tronquait donc le
  // fichier et DÉTRUISAIT ces sections : la première case cochée en jeu effaçait les
  // identifiants d'auto-login. On relit le document existant et on n'y remplace QUE la
  // clé « moonlight_ui ».
  // ⚠ yaml-cpp ne conserve pas les commentaires : ceux écrits à la main dans le fichier
  // disparaissent à la première sauvegarde. Toutes les VALEURS sont préservées.
  YAML::Node settings_root;
  try {
    settings_root = YAML::LoadFile(path);
  } catch (const std::exception&) {
    // Fichier absent (premier lancement) ou illisible : on repart d'un document vide
    // plutôt que de renoncer à sauvegarder nos propres réglages.
  }
  if (!settings_root.IsMap()) settings_root = YAML::Node(YAML::NodeType::Map);

  // `emitted_doc` doit rester vivant jusqu'à l'écriture : après l'assignation,
  // settings_root référence sa mémoire.
  YAML::Node emitted_doc;
  try {
    emitted_doc = YAML::Load(out.c_str());
  } catch (const std::exception& e) {
    LogError("[MoonlightUi] re-parse of emitted settings failed, not saving: {}", e.what());
    return;
  }
  settings_root["moonlight_ui"] = emitted_doc["moonlight_ui"];

  // Écriture ATOMIQUE : on remplit un fichier temporaire voisin, on le ferme (donc
  // on le vide sur disque), puis on le déplace PAR-DESSUS la cible. MoveFileEx avec
  // MOVEFILE_REPLACE_EXISTING est atomique sur un même volume — et le .tmp est dans
  // le même dossier, donc c'est bien le cas. À aucun instant bourgeon_settings.yaml
  // n'existe à moitié écrit.
  // Ce n'est pas de la prudence gratuite : le fichier porte AUSSI les sections
  // auto_login, char_select et moonlight_auth (cf. plus haut). Une écriture
  // tronquée par un crash, une coupure ou un disque plein n'emporterait donc pas
  // seulement nos réglages, mais les identifiants d'auto-login du joueur.
  const std::string tmp_path = path + ".tmp";
  {
    std::ofstream f(tmp_path, std::ios::trunc);
    if (!f) {
      LogError("[MoonlightUi] failed to open {}", tmp_path);
      return;
    }
    f << settings_root;
    f.flush();
    if (!f) {
      // Disque plein ou erreur d'écriture : on garde le fichier précédent INTACT.
      LogError("[MoonlightUi] write failed for {}, keeping previous settings", tmp_path);
      f.close();
      DeleteFileA(tmp_path.c_str());
      return;
    }
  }  // fermeture du flux (donc flush complet) AVANT le remplacement
  if (!MoveFileExA(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    LogError("[MoonlightUi] failed to replace {} (GetLastError={})", path,
             GetLastError());
    DeleteFileA(tmp_path.c_str());
  }
  // LogInfo("[MoonlightUi] saved chat backgrounds to {}", path);
}

// ── Anti-rebond de la sauvegarde ──────────────────────────────────────────
// Les ~40 sites d'appel de SaveSettings sont des widgets ImGui évalués à chaque
// frame. Un slider ou un color picker renvoie true à CHAQUE frame de glissement :
// le motif `if (changed) SaveSettings()` déclenchait donc une sérialisation YAML
// complète (~330 clés, ~140 std::string, 36 lectures mémoire client sous SEH) plus
// un cycle create/truncate/write/close, par frame — ~120 pour un drag de 2 s, sur
// le thread de rendu. On ne marque désormais que l'intention ; l'écriture a lieu
// une fois l'utilisateur stabilisé. Les appelants n'ont rien à changer.

void MoonlightUi::SaveSettings() {
  settings_dirty_    = true;
  settings_dirty_ms_ = GetTickCount();
}

void MoonlightUi::FlushSettings() {
  if (!settings_dirty_) return;
  settings_dirty_ = false;
  WriteSettingsFile();
}

void MoonlightUi::OnTick() {
  // OnTick tourne ~10 Hz même panneau fermé : le flush arrive donc aussi pour les
  // réglages poussés par les plugins frères hors de notre fenêtre.
  if (!settings_dirty_) return;
  if (GetTickCount() - settings_dirty_ms_ < kSettingsFlushDelayMs) return;
  FlushSettings();
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

  if (in_game_ && !was_in_game) {
    // Symétrique de la sortie : on écrit ce qui traîne AVANT de recharger, sinon un
    // réglage touché sur l'écran de login/char-select serait écrasé par LoadSettings
    // puis reperdu au flush suivant.
    FlushSettings();
    LoadSettings();
  }

  if (!in_game_ && was_in_game) {
    // Sortie du jeu (retour login / char-select) : on écrit TOUT DE SUITE, sans
    // attendre la fenêtre d'anti-rebond, sinon un réglage touché dans les dernières
    // centaines de ms serait perdu. Impérativement AVANT le clear : aloot_ids_ est
    // persisté, et flusher après sauvegarderait une liste @autolootid vide.
    FlushSettings();
    aloot_ids_.clear();
  }

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
      case kSettingStaff:
        // Niveau de groupe serveur (pc_get_group_level). > 0 => staff/GM ; active
        // les fonctionnalités réservées (IsStaff), sans édition manuelle du yaml.
        g_staff_level = static_cast<int>(value);
        // LogInfo("[MoonlightUi] staff_level={}", g_staff_level);
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

// HelpMarker, WheelSliderFloat/Int, PushStyleCompact/PopStyleCompact ont été
// déplacés dans ui/ro_widgets.cc ; AlignGrid::Draw et HudReplaced dans
// ui/align_grid.cc — ce sont des widgets d'usage général, pas du panneau de
// réglages. Ce fichier les consomme via les en-têtes correspondants.

// Ouvre le panneau directement sur une section d'« Interface de jeu ». Ne dessine
// rien : pose l'état que le prochain OnRenderUI consomme (déplier la fenêtre +
// ouvrir l'en-tête + sélectionner l'entrée). Sûr à appeler pendant le rendu d'une
// AUTRE fenêtre (le bullet de barre de titre du storage, p. ex.).
void MoonlightUi::OpenInterfaceSection(int section) {
  // Borne sur kIfaceCount plutôt que sur la dernière section nommée : ajouter une
  // entrée à l'enum suffit, il n'y a plus rien à penser ici.
  if (section < 0 || section >= kIfaceCount) return;
  iface_nav_ = section;
  iface_jump_ = true;
  // Fenêtre repliée : la déplier, sinon le saut serait invisible. Même chemin que
  // la restauration au login (apply_collapse_ -> SetNextWindowCollapsed).
  if (ui_collapsed_) {
    ui_collapsed_ = false;
    apply_collapse_ = true;
  }
  ImGui::SetWindowFocus("Moonlight-Destiny");
}

// ── ImGui panel ───────────────────────────────────────────────────────────
void MoonlightUi::OnRenderUI() {
  if (!in_game_) return;

  // Global alignment grid (shared HUD overlay). Drawn here on the background
  // list so it shows even with the bars hidden; suppressed while a full-screen
  // UI (world map) replaces the HUD, matching the bars/icons.
  if (grid_.show && !ro::HudReplaced()) grid_.Draw();

  // SPR Effect Lab : reconcile spawn + overlay au centre (foreground drawlist, indépendant
  // de la fenêtre principale). Inerte tant qu'aucun effet n'est demandé.
  spr_lab::RenderFrame();

  // Persist bars geometry once, the frame after the user finishes a drag.
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

  // Persist the collapsed state of the main window (set on any collapse/expand).
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

  // Default window style (rounded corners, thin border, rounded sliders/knobs).
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
  if (!is_collapsed) ro::RegisterEscapeMinimizeWindow(&collapse_requested_);

  if (!is_collapsed) {
    moonlight_ui::DrawRules();

    // ── DPS Meter ────────────────────────────────────────────────────────
    if (CollapsingHeader("DPS Meter")) {
      PushStyleCompact();
      if (auto* dps = Bourgeon::Instance().dps_meter()) {
        bool changed = false;
        changed |= ro::RoCheckbox("Afficher", &dps->visible_);
        changed |= ro::RoCheckbox("Verrouiller (fige + clic-traversant)", &dps->locked_);
        SameLine(); HelpMarker("Fige la fenêtre DPS (position/taille) et laisse passer les clics au jeu en dessous.");
        changed |= ColorPicker("Couleur texte",  dps->text_color_);
        changed |= ColorPicker("Couleur graphe", dps->plot_color_);

        PushItemWidth(160.0f); // sliders are narrow to fit the window
        changed |= WheelSliderFloat("Opacité fond", &dps->bg_alpha_, 0.0f, 1.0f);

        int slot_ms = dps->slot_ms_;
        if (WheelSliderInt("Résolution (ms/slot)", &slot_ms, 50, 2000)) {
          dps->slot_ms_ = slot_ms;
          dps->ResetHistory();
          changed = true;
        }
        SameLine(); HelpMarker("Largeur de chaque colonne du graphique en millisecondes.\nValeur plus basse = graphique plus précis mais moins smooth.");

        int win = dps->dps_window_secs_;
        if (WheelSliderInt("Fenêtre DPS (s)", &win, 1, 30)) {
          dps->dps_window_secs_ = win;
          changed = true;
        }
        SameLine(); HelpMarker("Fenêtre de temps pour calculer le DPS courant affiché.");

        int timeout = dps->combat_timeout_secs_;
        if (WheelSliderInt("Timeout combat (s)", &timeout, 1, 15)) {
          dps->combat_timeout_secs_ = timeout;
          changed = true;
        }
        SameLine(); HelpMarker("Secondes sans dégâts avant de quitter le mode combat.");

        PopItemWidth(); // restore default item width

        if (ro::RoButton("Reset graphique")) dps->ResetHistory();

        Separator();
        changed |= ro::RoCheckbox("Afficher dommages de sorts de zone dans le chat", &dps->show_ground_dmg_in_chat_);
        SameLine(); HelpMarker(
            "Affiche chaque coup de Storm Gust / Meteor Storm / LoV etc. dans le chat.\n"
            "Message custom Bourgeon — le serveur ne montre pas ces dégâts dans le chat habituel.");

            // Persist all DPS settings if any changed.
        if( changed ) SaveSettings();
      }
      PopStyleCompact();
    }

    // ── Interface de jeu  ────────────────────────────────────────────────────
    // NB: la vue caméra FPS (FpsViewTweaks) reste dans le code (toggle F9) mais
    // n'est plus exposée dans ce menu (expérimental, retiré à la demande).
    if (CollapsingHeader("Mini-jeux")) {
      SeparatorText("DOOM");
      if (auto* doom = Bourgeon::Instance().doom()) {
        bool on = doom->enabled();
        if (ro::RoCheckbox("Lancer DOOM (1993) dans Ragnarok", &on))
          doom->SetEnabled(on);
        SameLine(); HelpMarker(
            "Le vrai DOOM (moteur doomgeneric embarqué), rendu dans une fenêtre "
            "par-dessus le jeu.\n\n"
            "Nécessite doom1.wad (shareware) à côté de l'exe du client.\n"
            "Clique la fenêtre DOOM pour capturer le clavier : ZQSD (AZERTY), "
            "WASD ou flèches pour bouger, Ctrl tirer, Espace/E ouvrir, Shift "
            "courir, Échap menu.\n"
            "Décocher = pause. Quitter depuis le menu DOOM = définitif "
            "jusqu'au redémarrage du client.");
        GrayText("État : %s", doom->StatusText());
      } else
        GrayText("Indisponible.");

      SeparatorText("Parade de Porings (login)");
      if (auto* lp = Bourgeon::Instance().login_parade()) {
        bool on = lp->enabled_;
        if (ro::RoCheckbox("Porings sur l'écran de login", &on)) {
          lp->enabled_ = on;
          SaveSettings();
        }
        SameLine(); HelpMarker(
            "Fait flâner une petite bande de monstres de la famille Poring sur "
            "l'écran de login : ils sautillent d'un bord à l'autre, font des "
            "pauses, et sursautent (avec un son) si tu cliques dessus.\n\n"
            "Purement cosmétique. Ils s'estompent au-dessus du panneau de login "
            "pour ne pas gêner la saisie. Visible uniquement à l'écran de login.");
      } else
        GrayText("Indisponible.");

      SeparatorText("Roggle");
      if (auto* roggle = Bourgeon::Instance().roggle()) {
        bool on = roggle->enabled();
        if (ro::RoCheckbox("Ouvrir Roggle", &on))
          roggle->SetEnabled(on);
        SameLine(); HelpMarker(
            "Mini-jeu façon Peggle, dessiné en ImGui par-dessus le jeu.\n\n"
            "Vise à la souris depuis le canon en haut, clique pour tirer la "
            "bille. Dégomme tous les pegs ORANGE pour gagner ; le seau vert en "
            "bas rattrape la bille = bille gratuite.\n"
            "Fermer la fenêtre ou décocher = masquer (la partie est conservée).");
      } else
        GrayText("Indisponible.");

      SeparatorText("Rojeweled");
      if (auto* rj = Bourgeon::Instance().rojeweled()) {
        bool on = rj->enabled();
        if (ro::RoCheckbox("Ouvrir Rojeweled", &on))
          rj->SetEnabled(on);
        SameLine(); HelpMarker(
            "Match-3 façon Bejeweled dont les gemmes sont de vrais sprites de "
            "monstres RO (famille Poring : Poring, Drops, Metaling, Poporing, "
            "Marin, Deviling).\n\n"
            "Clique deux monstres voisins pour les échanger ; aligne-en 3+ pour "
            "les faire disparaître (les cascades rapportent plus). DX9 requis "
            "(sinon tuiles colorées).");
      } else
        GrayText("Indisponible.");

      SeparatorText("Saut (barre espace)");
      if (auto* pj = Bourgeon::Instance().player_jump()) {
        bool on = pj->enabled();
        if (ro::RoCheckbox("Sauter avec Espace", &on))
          pj->SetEnabled(on);
        SameLine(); HelpMarker(
            "Appuie sur Espace pour faire bondir ton personnage : un petit arc "
            "parabolique (montée puis retombée) purement visuel.\n\n"
            "Le serveur ne voit rien — c'est un simple décalage de hauteur du "
            "sprite, ré-appliqué chaque frame (tu peux même sauter en marchant). "
            "Taper une espace dans le chat ne déclenche PAS de saut.");
        // Réglages fins de l'arc de saut : réservés au staff (cf. IsStaff, group
        // level serveur >= 80). Mal réglés ils donnent un saut grotesque ou
        // invisible — les valeurs par défaut restent actives pour tout le monde.
        // Live, non persistés (comme FpsView).
        if (on && IsStaff()) {
          PushItemWidth(160.0f);
          WheelSliderFloat("Hauteur", pj->p_height(), 2.0f, 40.0f);
          WheelSliderInt("Durée (ms)", pj->p_duration_ms(), 200, 1500);
          PopItemWidth();
        }
      } else
        GrayText("Indisponible.");

      SeparatorText("Déplacement au clavier");
      if (auto* km = Bourgeon::Instance().keyboard_move()) {
        bool on = km->enabled();
        if (ro::RoCheckbox("Marcher avec ZQSD / flèches", &on))
          km->SetEnabled(on);
        SameLine(); HelpMarker(
            "Déplace ton personnage au clavier : Z/S pour avancer et reculer, "
            "Q/D pour aller à gauche et à droite (les flèches font pareil). "
            "Deux touches ensemble = diagonale.\n\n"
            "Rien n'est simulé côté client : le plugin envoie la MÊME demande de "
            "marche que le clic au sol, donc le serveur reste maître du "
            "déplacement (murs, vitesse, blocages). Taper dans le chat ne fait "
            "pas courir le personnage.\n\n"
            "Attention si tu as des raccourcis de compétence sur Z, Q, S ou D : "
            "ils se déclencheront aussi.");
        if (on) {
          ro::RoCheckbox("Suivre la rotation de la caméra", km->p_camera_relative());
          SameLine(); HelpMarker(
              "« Haut » = le haut de l'écran, même après avoir fait pivoter la "
              "caméra. Décoché : les directions restent celles de la carte.");
          ro::RoCheckbox("S'arrêter au relâchement", km->p_stop_on_release());
          SameLine(); HelpMarker(
              "Coupe la marche dès que tu lâches la touche, au lieu de laisser "
              "le personnage finir le trajet demandé.");
          // Réglages fins du protocole de marche : réservés au staff (cf.
          // IsStaff, group level serveur >= 80). Mal réglés ils dégradent le
          // ressenti ou spamment le serveur — les valeurs par défaut restent
          // actives pour tout le monde. Live, non persistés (comme FpsView).
          if (IsStaff()) {
            PushItemWidth(160.0f);
            WheelSliderInt("Anticipation (cases)", km->p_look_ahead(), 1, 6);
            WheelSliderInt("Cadence (ms)", km->p_refresh_ms(), 60, 400);
            PopItemWidth();
          }
        }
      } else
        GrayText("Indisponible.");
    }

    // Saut demandé (bullet de barre de titre d'une fenêtre Bourgeon) : on force
    // l'en-tête ouvert et on scrolle dessus, une seule fois.
    const bool iface_jump = iface_jump_;
    iface_jump_ = false;
    if (iface_jump) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (CollapsingHeader("Interface de jeu")) {
      if (iface_jump) ImGui::SetScrollHereY(0.0f);
      PushStyleCompact();
      bool changed = false;
      changed |= ro::RoCheckbox("Grille d'alignement", &grid_.show);
      SameLine(); HelpMarker(
          "Affiche une grille plein écran pour aligner ton interface "
          "(comme les add-ons d'interface de WoW).");
      ImGui::SetNextItemWidth(160.0f);
      changed |= WheelSliderInt("Taille grille", &grid_.size, 4, 128);
      changed |= ro::RoCheckbox("Aimanter à la grille", &grid_.snap);
      SameLine(); HelpMarker(
          "Les barres et les icônes s'alignent sur les cellules de la grille "
          "pendant le déplacement et le redimensionnement.");
      changed |= ColorPicker("Couleur grille", grid_.color);

      // (Inventaire et Storage : tout est regroupé dans leurs sections dédiées.)

      // (Storage : tout est regroupé dans la section « Storage » ci-dessous.)

      // Cash shop : redraw ImGui moderne OU fenêtre native
      if (auto* cs = Bourgeon::Instance().cashshop_tweaks()) {
        changed |= ro::RoCheckbox("Cash Shop Moonlight®", &cs->imgui_enabled_);
        SameLine(); HelpMarker(
            "ON : cash shop ImGui moderne (icônes, catégories, panier) et la "
            "fenêtre native est cachée.\nOFF : cash shop natif classique.");
      }

      // Shop NPC : fenêtre achat/vente ImGui unifiée OU natif
      if (auto* sh = Bourgeon::Instance().shop_tweaks()) {
        changed |= ro::RoCheckbox("Shop NPC Moonlight®", &sh->imgui_enabled_);
        SameLine(); HelpMarker(
            "ON : fenêtre boutique ImGui unifiée (onglets Acheter/Vendre, saut "
            "du choix Acheter/Vendre natif).\nOFF : boutique NPC native classique.");
      }

      // Feuille de personnage (agrege Status + Equipement)
      if (auto* cse = Bourgeon::Instance().character_sheet()) {
        changed |= ro::RoCheckbox("Feuille de perso Moonlight® (Alt+F)", &cse->imgui_enabled_);
        SameLine(); HelpMarker(
            "Fenêtre façon WoW : avatar + slots équipement/costume + stats, en "
            "COMPLÉMENT des fenêtres natives (conservées). Ouvre/ferme avec Alt+F.\n"
            "Clic gauche slot = description, clic droit = desequiper, boutons +stat.");
      }

      // Bouton « Signaler un bug » (desc item/skill + dialogue PNJ + raccourci).
      if (auto* br = Bourgeon::Instance().bug_report()) {
        changed |= ro::RoCheckbox("Afficher le bouton « Signaler un bug »", &br->enabled());
        SameLine(); HelpMarker(
            "Affiche le bouton de rapport de bug dans les fenêtres de "
            "description (item/skill) et le dialogue PNJ, et active le "
            "raccourci Ctrl+Alt+B. Décoche pour tout désactiver.");
      }

      if (changed) SaveSettings();

      // Navigation latérale (liste à gauche, contenu à droite). L'entrée active est
      // un MEMBRE (iface_nav_) : OpenInterfaceSection la pilote depuis le bullet de
      // barre de titre d'une autre fenêtre Bourgeon.
      // Source UNIQUE des sections : chaque ligne porte son identifiant d'enum ET
      // son libellé. Insérer/déplacer une entrée ne peut donc plus désaligner
      // silencieusement le libellé et le contenu — la panne muette que produisait
      // la paire « enum + tableau de chaînes » maintenue à la main.
      struct IfaceEntry { IfaceSection id; const char* label; };
      static constexpr IfaceEntry kIfaceSections[] = {
          {kIfaceSkillBar,    "Barre d'action"},
          {kIfaceBasicInfo,   "Basic Info"},
          {kIfaceChat,        "Chat"},
          {kIfaceMenuIcons,   "Icônes du menu"},
          {kIfaceStatusIcons, "Icônes de statut"},
          {kIfaceQuest,       "Suivi de quête"},
          {kIfaceDesc,        "Descriptions"},
          {kIfaceSkin,        "Skin RO"},
          {kIfaceNpc,         "Fenêtre NPC"},
          {kIfaceStorage,     "Storage"},
          {kIfaceInventory,   "Inventaire"},
      };
      static_assert(IM_ARRAYSIZE(kIfaceSections) == kIfaceCount,
                    "kIfaceSections doit couvrir exactement l'enum IfaceSection");

      // Dimensions dérivées du texte/style (pas de pixels fixes) : la liste garde
      // la largeur de sa plus longue entrée, bornée à 40 % de la place dispo pour
      // rester lisible sur fenêtre étroite.
      // La mesure des 11 libellés ne dépend que de la POLICE : on la garde en cache
      // au lieu de refaire 11 CalcTextSize à chaque frame, et on la réinvalide quand
      // la police change (bascule du skin RO, taille de police).
      const ImGuiStyle& st = ImGui::GetStyle();
      static float s_labels_w    = 0.0f;   // largeur du plus long libellé, en px
      static ImFont* s_labels_font = nullptr;
      static float s_labels_size = 0.0f;
      if (s_labels_font != ImGui::GetFont() || s_labels_size != ImGui::GetFontSize()) {
        s_labels_w = 0.0f;
        for (const IfaceEntry& entry : kIfaceSections)
          s_labels_w = (std::max)(s_labels_w, ImGui::CalcTextSize(entry.label).x);
        s_labels_font = ImGui::GetFont();
        s_labels_size = ImGui::GetFontSize();
      }
      float nav_w = s_labels_w + st.WindowPadding.x * 2.0f + st.FramePadding.x * 2.0f;
      const float nav_w_min = ImGui::GetFontSize() * 5.0f;
      const float nav_w_max =
          (std::max)(nav_w_min, ImGui::GetContentRegionAvail().x * 0.4f);
      nav_w = (std::min)((std::max)(nav_w, nav_w_min), nav_w_max);
      const float nav_h = ImGui::GetTextLineHeightWithSpacing() * kIfaceCount
                        + st.WindowPadding.y * 2.0f;

      ImGui::BeginChild("iface_nav", ImVec2(nav_w, nav_h), ImGuiChildFlags_Borders);
      for (const IfaceEntry& entry : kIfaceSections)
        if (ImGui::Selectable(entry.label, iface_nav_ == entry.id)) iface_nav_ = entry.id;
      ImGui::EndChild();

      SameLine();
      // Hauteur libre (AutoResizeY) : le panneau prend exactement la place de son
      // contenu et c'est la fenêtre parente qui scrolle — plus de dépendance à la
      // taille de la fenêtre ni de scrollbar imbriquée.
      ImGui::BeginChild("iface_content", ImVec2(0.0f, 0.0f),
                        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
      ImGui::PushTextWrapPos(0.0f);  // wrap le texte à la largeur du child
      {
        PushItemWidth(160.0f);

        // ── Barre d'action ───────────────────────────────────────────────────
        if (iface_nav_ == kIfaceSkillBar)
        {
          if (auto* sb = Bourgeon::Instance().skill_bar())
            sb->DrawSettings();
          else
            GrayText("(plugin indisponible)");
        }

        // ── Barres d'info (HUD bars + alignment grid) ────────────────────────
        // ── Status Portrait (head + pseudo + classe + niveau, indépendants) ──
        if (iface_nav_ == kIfaceBasicInfo) {
          bool changed = false;
          if (auto* eb = Bourgeon::Instance().basic_info()) {
            PushStyleCompact();

            changed |= ro::RoCheckbox("Masquer la fenêtre Basic Info d'origine", &eb->portrait_hide_basic_info_);
            SameLine(); HelpMarker("Masque la fenêtre native \"Basic Info\".");

            SeparatorText("Barres d'info");
            changed |= ro::RoCheckbox("Afficher les barres", &eb->visible_);
            ImGui::BeginDisabled(!eb->visible_);
            Indent();
              for (int i = 0; i < BasicInfoTweaks::kBarCount; ++i) {
                if (i) SameLine();
                changed |= ro::RoCheckbox(BasicInfoTweaks::kBarLabels[i], &eb->bars_[i].show);
              }
              SameLine(); HelpMarker("Affiche/cache chaque barre indépendamment.");
            Unindent();
            ImGui::EndDisabled();

            changed |= ro::RoCheckbox("Verrouiller les barres", &eb->locked_);
            SameLine(); HelpMarker(
                "Verrouillée : les barres ne bougent plus et laissent passer les clics au jeu.\n"
                "Déverrouillée : glissez-les pour les déplacer, tirez le coin pour redimensionner.");

            changed |= ro::RoCheckbox("Aimanter les barres (snap)", &eb->sticky_);
            SameLine(); HelpMarker(
                "Quand tu glisses une barre près d'une autre, ses bords s'alignent "
                "et se collent automatiquement (~10px).\nÉloigne-la pour la "
                "détacher. Les barres restent indépendantes.");

            changed |= ro::RoCheckbox("Vertical", &eb->vertical_);
            SameLine(); HelpMarker(
                "Remplissage vertical des barres. \n"
                "Décoche pour les barres horizontales.");

            changed |= ro::RoCheckbox("Bordure des barres", &eb->border_);
            SameLine(); HelpMarker(
                "Trait sombre 1px autour de chaque barre (HP/SP/EXP...). \n"
                "Décoche pour des barres sans contour.");

            const char* modes[] = {"Aucun", "Pourcentage", "Valeurs", "Les deux"};
            changed |= ro::RoCombo("Texte des barres", &eb->text_mode_, modes, IM_ARRAYSIZE(modes));
            SameLine(); HelpMarker(
                "Ce qui est écrit sur les barres : rien, le pourcentage, les "
                "valeurs brutes (courant / max) ou les deux.");
            changed |= WheelSliderFloat("Arrondi", &eb->rounding_, 0.0f, 16.0f);
            SameLine(); HelpMarker("Arrondi des coins des barres.");

            for (int i = 0; i < BasicInfoTweaks::kBarCount; ++i) {
              char lbl[32];
              std::snprintf(lbl, sizeof(lbl), "Couleur %s", BasicInfoTweaks::kBarLabels[i]);
              changed |= ColorPicker(lbl, eb->bars_[i].fill);
            }
            changed |= ColorPicker("Fond / Opacité", eb->bg_color_);

            TextUnformatted("Tailles rapides de barres (toutes) :");
            auto preset = [&](const char* label, int w, int h) {
              SameLine();
              if (ro::RoButton(label)) {
                for (int j = 0; j < BasicInfoTweaks::kBarCount; ++j) {
                  eb->bars_[j].w = w;
                  eb->bars_[j].h = h;
                }
                eb->force_apply_ = true;  // re-apply size even while unlocked
                changed = true;
              }
            };
            preset("XS", 200, 9);
            preset("S", 400, 16);
            preset("M", 600, 22);
            preset("L", 800, 30);

            SeparatorText("Portrait personnage");
            changed |= ro::RoCheckbox("Afficher le portrait et les étiquettes", &eb->portrait_visible_);
            SameLine(); HelpMarker(
                "Portrait de statut : la tête du personnage, le pseudo, la classe "
                "et le niveau sont des éléments INDÉPENDANTS — chacun déplaçable, "
                "redimensionnable, avec sa couleur/opacité de fond et son arrondi.");

            ImGui::BeginDisabled(!eb->portrait_visible_);

            changed |= ro::RoCheckbox("Verrouiller le portrait", &eb->portrait_locked_);
            Tooltip("Si les éléments sont déverrouillés et en contact les uns avec les autres, ils sont déplaçables en maintenant Ctrl.");
            SameLine(); HelpMarker(
                "Verrouillé : les éléments ne bougent plus et laissent passer les clics au jeu.\n"
                "Déverrouillé : glisse pour déplacer, tire un bord/coin pour redimensionner (aimantage à la grille d'alignement).");

            changed |= ro::RoCheckbox("Tête seule (sans le corps)", &eb->portrait_head_only_);
            SameLine(); HelpMarker(
                "Ne garde ne génère que la tête (visage/cheveux/coiffes) et retire le corps.\n"
                "Décoche pour le personnage entier.");

            changed |= ro::RoCheckbox("Cape / garment", &eb->portrait_show_garment_);
            SameLine(); HelpMarker(
                "Affiche la cape/garment équipée (seulement en mode corps "
                "entier — décoche \"Tête seule\" pour la voir).");

            changed |= WheelSliderFloat("Zoom", &eb->portrait_head_zoom_, 0.10f, 2.0f);
            SameLine(); HelpMarker("Ajuster avec le zoom.");

            changed |= WheelSliderFloat("Décalage horiz.", &eb->portrait_head_offx_, -1.5f, 1.5f);
            SameLine(); HelpMarker(
                "Décale le portrait horizontalement (0 = centré).\n"
                "Sert à cadrer la tête/le corps ; le zoom reste centré.");

            changed |= WheelSliderFloat("Décalage vert.", &eb->portrait_head_offy_, -1.5f, 1.5f);
            SameLine(); HelpMarker(
                "Décale le portrait verticalement (0 = centré).\n"
                "Optionnel — le zoom reste centré ; laisse à 0 si tu n'en as pas besoin.");

            static const char* kLabelsAnim[] = { "Repos", "Marche", "Assis", "Ramasser", "Combat", "Attaque", "Touché", "Gelé", "Mort" };
            changed |= ro::RoCombo("Animation", &eb->portrait_anim_, kLabelsAnim, IM_ARRAYSIZE(kLabelsAnim));
            SameLine(); HelpMarker(
                "Pose animée du portrait (Combat = posture prête au combat).\n"
                "Le nombre d'images de l'animation s'ajuste automatiquement.");

            static const char* kLabelsDir[] = { "Face", "Profil-Gauche", "Gauche", "Arrière-Gauche", "Dos", "Arrière-Droite", "Droite", "Profil-Droite" };
            changed |= ro::RoCombo("Direction", &eb->portrait_dir_, kLabelsDir, IM_ARRAYSIZE(kLabelsDir));
            SameLine(); HelpMarker(
                "Oriente le portrait. 0 = face. Essaie les valeurs pour trouver "
                "l'angle voulu (le rendu se met à jour en direct).");

            changed |= ro::RoCheckbox("Animer", &eb->portrait_animate_);
            SameLine(); HelpMarker(
                "Joue les images de l'animation (ex. le balayage de la posture "
                "Combat). Décoche pour figer une pose calme (image 0).");

            SeparatorText("Couleurs et arrondis du portrait et des étiquettes");
            changed |= ro::RoCheckbox("Bordure", &eb->portrait_border_);
            SameLine(); HelpMarker("Trait 1px autour du cadre et des étiquettes.");

            // Per-element config: show / background colour+opacity / rounding /
            // text colour.  Each element is independent.
            for (int i = 0; i < BasicInfoTweaks::kPortCount; ++i) {
              auto& e = eb->ports_[i];
              ImGui::PushID(i);
              changed |= ro::RoCheckbox(BasicInfoTweaks::kPortLabels[i], &e.show);
              Indent();
              changed |= ColorPicker("Fond / Opacité", e.bg);
              if (i != BasicInfoTweaks::kPortHead) {
                SameLine();
                changed |= ColorPicker("Texte", e.fg);
              }
              changed |= WheelSliderFloat("Arrondi", &e.rounding, 0.0f, 16.0f, "%.0f", 1.0f);
              Unindent();
              ImGui::PopID();
            }
            PopStyleCompact();

            ImGui::EndDisabled(); // eb->portrait_visible_
          }
          if (changed) SaveSettings();
        }

        // ── Chat Settings ────────────────────────────────────────────────────
        if (iface_nav_ == kIfaceChat) {
          bool changed = false;
          if (auto* eb = Bourgeon::Instance().basic_info()) {
            PushStyleCompact();

            SeparatorText("Réglages généraux");
            if (ro::RoCheckbox("Chat Discord (Gonryun only)", &discord_chat_)) {
              UpdateRelay();
              SendSetting(kSettingDiscordChat, discord_chat_ ? 1 : 0);
            }

            if (ro::RoCheckbox("Largeur du chat", &chat_width_enabled_)) {
              chat::SetCustomWidth(chat_width_enabled_, chat_width_px_);
              changed = true;
            }

            if (chat_width_enabled_) {
              // Le slider bouge à 60 Hz, mais chat::SetCustomWidth relance le relayout
              // natif ET RebuildFromHistory sur TOUS les onglets — le chemin exact du
              // freeze de word-wrap déjà corrigé côté mesure. On ne l'applique donc
              // qu'au RELÂCHEMENT du slider, pas à chaque frame de glissement (même
              // logique que le color picker du fond de chat, plus bas).
              // `moved && !IsItemActive()` = ajustement à la MOLETTE (WheelSliderInt la
              // traite hors du slider, sans jamais « désactiver » l'item) : on applique
              // tout de suite. `IsItemDeactivatedAfterEdit()` = fin de drag ou fin de
              // saisie Ctrl+clic. Pendant le drag l'item est actif : on ne fait rien.
              const bool moved = WheelSliderInt("Largeur (px)", &chat_width_px_, 320, 1200);
              if ((moved && !ImGui::IsItemActive()) || ImGui::IsItemDeactivatedAfterEdit()) {
                chat::SetCustomWidth(true, chat_width_px_);
                changed = true;
              }
            }

            if (ro::RoCheckbox("Horodatage du chat", &chat_timestamps_)) {
              chat::SetTimestamps(chat_timestamps_);
              changed = true;
            }

            if (ro::RoCheckbox("Icônes d'objets", &chat_item_icons_)) {
              chat::SetItemIcons(chat_item_icons_);
              changed = true;
            }

            // Clear chat history (all channels of the main chat window)
            if (ro::RoButton("Effacer l'historique du chat")) chat::ClearHistory();
            SameLine(); HelpMarker(
                "Vide l'historique de tous les canaux de la fenêtre de chat principale "
                "(historique brut effacé + affichage vidé). Les nouveaux messages "
                "réapparaissent normalement ensuite.");

            SeparatorText("Couleurs du chat");
            // Chat Background Colours (Main / Detached / Whisper)
            // One independent colour+opacity picker per group, persisted locally.
            auto render_chatbg = [&](ChatBgGroup& g) {
              if (g.instrs.empty()) return;
              ImGui::PushID(g.yaml_key);
              const ImVec4 swatch(g.color[0], g.color[1], g.color[2], g.color[3]);
              if (ImGui::ColorButton("##btn", swatch, ImGuiColorEditFlags_AlphaPreview, ImVec2(20, 20)))
              OpenPopup("picker");
              SameLine();
              TextUnformatted(g.label);

              if (BeginPopup("picker")) {
                // ── Shared user presets ─────────────────────────────────────────
                if (!chat_bg_presets_.empty()) {
                  TextUnformatted("Presets:");
                  int delete_idx = -1;
                  // Display each preset as a colour swatch + name + delete button.
                  for (int i = 0; i < static_cast<int>(chat_bg_presets_.size()); ++i) {
                    const auto& p = chat_bg_presets_[i];
                    const ImVec4 col(((p.argb >> 16) & 0xFF) / 255.0f,
                                    ((p.argb >>  8) & 0xFF) / 255.0f,
                                    ( p.argb        & 0xFF) / 255.0f,
                                    ((p.argb >> 24) & 0xFF) / 255.0f);
                    ImGui::PushID(i);
                    // Clicking a preset swatch updates the picker to match the preset, applies it to the chat background, and saves the settings.
                    if (ImGui::ColorButton("##swatch", col,
                                          ImGuiColorEditFlags_AlphaPreview |
                                          ImGuiColorEditFlags_NoTooltip,
                                          ImVec2(18, 18))) {
                      PickerFromArgb(g.color, p.argb); // update the picker to match the preset
                      ApplyChatBg(g, p.argb, true);
                      changed = true;
                    }
                    SameLine();
                    TextUnformatted(p.name.c_str());
                    SameLine();
                    if (ro::RoSmallButton("x")) delete_idx = i;
                    ImGui::PopID();
                  }
                  if (delete_idx >= 0) {
                    chat_bg_presets_.erase(chat_bg_presets_.begin() + delete_idx);
                    changed = true;
                  }
                  SeparatorText("Sauvegarder une couleur comme preset");
                }
                // ── Save current colour as a preset ─────────────────────────────
                ImGui::SetNextItemWidth(120.0f);
                ImGui::InputTextWithHint("##preset_name", "Preset name", preset_name_buf_, sizeof(preset_name_buf_));
                SameLine();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 3.0f); // vertically align the button with the input text
                if (ro::RoButton("Save preset") && preset_name_buf_[0] != '\0') {
                  chat_bg_presets_.push_back({preset_name_buf_, ArgbFromPicker(g.color)});
                  preset_name_buf_[0] = '\0';
                  changed = true;
                }
                SeparatorText("Choisir une couleur");
                if (ColorPicker("##pick", g.color)) {
                  ApplyChatBg(g, ArgbFromPicker(g.color), false);
                  g.editing = true;
                }
                if (g.editing && ImGui::IsMouseReleased(0)) {
                  ApplyChatBg(g, ArgbFromPicker(g.color), true);
                  changed = true;
                  g.editing = false;
                }
                if (ro::RoButton("Close")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
              }
              ImGui::PopID();
            };

            if (chat_bg_found_) {
              render_chatbg(chat_bg_[kChatBgMain]);
              // Quick preset switcher toggle, on the same line as the main chat picker.
              SameLine();
              changed |= ro::RoCheckbox("Preset bar", &mainchat_preset_bar_);
              render_chatbg(chat_bg_[kChatBgDetached]);
              render_chatbg(chat_bg_[kChatBgWhisper]);
            } else GrayText("(chat background patch unavailable)");

            PopStyleCompact();
          }
          if (changed) SaveSettings();
        }

        // ── Menu icons (ImGui replacement) ───────────────────────────────────
        if (iface_nav_ == kIfaceMenuIcons) {
          bool changed = false;
          if (auto* mi = Bourgeon::Instance().menu_icons()) {
            SeparatorText("Réglages généraux");
            changed |= ro::RoCheckbox("Rendre les icônes déplaçables", &mi->enabled_);
            SameLine(); HelpMarker("Cache la grille native et recrée les icônes fonctionnelles.");

            ImGui::BeginDisabled(!mi->enabled_);

            changed |= ro::RoCheckbox("Mode édition (glisser pour déplacer)", &mi->edit_mode_);
            SameLine(); HelpMarker(
                "En mode édition : glisse chaque icône pour la repositionner.\n"
                "Aimantage aux autres icônes et à la grille d'alignement.\n"
                "Désactive le mode pour cliquer les icônes normalement.");

            // Per-icon show/hide. icons() is populated once in-game.
            SeparatorText("Icônes");
            auto& icons = mi->icons();
            if (icons.empty()) {
              GrayText("(disponible une fois en jeu)");
            } else {
              for (auto& ic : icons) {
                bool shown = !ic.hidden;
                ImGui::PushID(ic.cmd_id);
                if (ro::RoCheckbox(ic.name, &shown)) {
                  ic.hidden = !shown;
                  mi->saved_[ic.name] = {ic.x, ic.y, ic.hidden, true};
                  changed = true;
                }
                ImGui::PopID();
              }
            }

            ImGui::EndDisabled();
          }
          if (changed) SaveSettings();
        }

        // ── Status icons (StatusIconTweaks) ──────────────────────────────────
        if (iface_nav_ == kIfaceStatusIcons) {
          if (auto* si = Bourgeon::Instance().status_icons())
            si->DrawSettings();
          else
            GrayText("(plugin indisponible)");
        }

        // ── Suivi de quête (QuestTrackerTweaks) ──────────────────────────────
        if (iface_nav_ == kIfaceQuest) {
          if (auto* qt = Bourgeon::Instance().quest_tracker())
            qt->DrawSettings();
          else
            GrayText("(plugin indisponible)");
        }

        // ── Descriptions (ItemDescTweaks : panneaux techniques item/skill) ───
        if (iface_nav_ == kIfaceDesc) {
          bool changed = false;
          if (auto* idt = Bourgeon::Instance().item_desc()) {
            TextUnformatted("Descriptions modernes des items et skills.");

            changed |= ro::RoCheckbox("Panneau technique des items", &idt->show_item_panel());
            SameLine(); HelpMarker(
                "Affiche le panneau enrichi description d'un ITEM.\n"
                "Clic droit item");

            changed |= ro::RoCheckbox("Panneau technique des skills", &idt->show_skill_panel());
            SameLine(); HelpMarker(
                "Affiche le panneau enrichi à côté de la description d'un SKILL.\n"
                "Clic droit dans le grimoire");

            changed |= ro::RoCheckbox("Ouvrir près de la souris", &idt->desc_spawn_at_cursor());
            SameLine(); HelpMarker(
                "ON : la description apparaît près du curseur à chaque ouverture.\n"
                "OFF : elle réapparaît à sa dernière position connue.");

            if (idt->desc_spawn_at_cursor()) {
              Indent();
                const char* kAnchors[] = {"Haut-gauche", "Haut-droite", "Bas-gauche","Bas-droite", "Centre"};
                changed |= ro::RoCombo("Ancrage", &idt->desc_anchor(), kAnchors, 5);
                changed |= WheelSliderInt("Offset X", &idt->desc_offset_x(), -400, 400, "%d px");
                changed |= WheelSliderInt("Offset Y", &idt->desc_offset_y(), -400, 400, "%d px");
                SameLine(); HelpMarker("Décalage depuis le curseur (molette au survol pour ajuster).");
              Unindent();
            }
          } else {
            GrayText("(plugin indisponible)");
          }
          if (changed) SaveSettings();
        }

        // ── Skin RO (police + habillage des fenêtres ImGui) ──────────────────
        if (iface_nav_ == kIfaceSkin) {
          bool changed = false;
          bool font_on = ro::IsFontEnabled();
          if (ro::RoCheckbox("Police Malgun (UI)", &font_on)) {
            ro::SetFontEnabled(font_on);
            changed = true;
          }
          SameLine(); HelpMarker(
              "ON : police Malgun Gothic pour toute l'UI ImGui (latin + coreen).\n"
              "OFF : police integree d'ImGui (ProggyClean).");
          // (Le skin RO n'est plus optionnel : c'est l'habillage standard des
          // fenêtres ImGui Bourgeon. Seuls ses réglages restent configurables.)
          changed |= ro::ShowRoSkinSettings();

          // ── Presets : jeux de couleurs sauvegardés ────────────────────────
          SeparatorText("Presets");
          const int npreset = static_cast<int>(g_ro_presets.size());
          const bool valid_sel = g_ro_preset_sel >= 0 && g_ro_preset_sel < npreset;
          const char* preview = valid_sel ? g_ro_presets[g_ro_preset_sel].name.c_str()
                                          : "(choisir)";
          if (ro::RoBeginCombo("##ro_preset", preview)) {
            for (int i = 0; i < npreset; ++i)
              if (ImGui::Selectable(g_ro_presets[i].name.c_str(), g_ro_preset_sel == i))
                g_ro_preset_sel = i;
            ro::RoEndCombo();
          }
          SameLine();
          if (ro::RoButton("Appliquer") && valid_sel) {
            ro::SkinConfig() = g_ro_presets[g_ro_preset_sel].cfg;
            changed = true;
          }
          SameLine();
          if (ro::RoButton("Supprimer") && valid_sel) {
            g_ro_presets.erase(g_ro_presets.begin() + g_ro_preset_sel);
            g_ro_preset_sel = -1;
            changed = true;
          }
          static char preset_name[32] = "";
          ImGui::InputText("##ro_preset_name", preset_name, sizeof(preset_name));
          SameLine();
          if (ro::RoButton("Sauvegarder") && preset_name[0]) {
            bool found = false;
            for (auto& p : g_ro_presets)
              if (p.name == preset_name) { p.cfg = ro::SkinConfig(); found = true; break; }
            if (!found) g_ro_presets.push_back({preset_name, ro::SkinConfig()});
            changed = true;
            preset_name[0] = '\0';
          }
          SameLine(); HelpMarker(
            "Sauvegarde les couleurs/luminosite/opacite actuelles sous un nom.\n"
            "'Appliquer' recharge un preset ; les joueurs peuvent se faire plusieurs themes.");
          if (changed) SaveSettings();
        }

        // ── Fenêtre NPC (dialogue / menu / prompt ImGui) ─────────────────────
        if (iface_nav_ == kIfaceNpc) {
          if (auto* nd = Bourgeon::Instance().npc_dialog_tweaks()) {
            if (ro::RoCheckbox("Dialogue NPC ImGui", &nd->imgui_enabled_))
              SaveSettings();
            SameLine(); HelpMarker(
                "Remplace le dialogue / menu / prompt NPC natif par un overlay ImGui "
                "(texte en couleur, menu à navigation clavier : flèches + Entrée, "
                "touches 1-9). Opt-in ; la fenêtre native est cachée quand c'est actif.");
            ImGui::BeginDisabled(!nd->imgui_enabled_);
            if (ro::RoCheckbox("Barre de recherche du menu", &nd->menu_search_))
              SaveSettings();
            SameLine(); HelpMarker(
                "Affiche un champ de recherche au-dessus des longs menus (plus de 8 "
                "choix) pour filtrer les options. Décoche pour un menu épuré.");
            ImGui::EndDisabled();
          }
        }

        // ── Storage (StorageTweaks : viewer ImGui + colonnes/filtre/survol) ───
        if (iface_nav_ == kIfaceStorage) {
          if (auto* stg = Bourgeon::Instance().storage_tweaks()) {
            bool changed = false;
            // Interrupteur GLOBAL synchronisé : bascule aussi l'inventaire et les
            // barres d'action (tout-ImGui ou tout-natif, plus de mixe).
            if (ro::RoCheckbox("Interface moderne (inventaire + storage + barres + échange)",
                               &stg->imgui_enabled_)) {
              SetModernInterface(stg->imgui_enabled_);
              changed = true;
            }
            SameLine(); HelpMarker(
                "Interrupteur GLOBAL : inventaire, storage, barres d'action et "
                "échange modernes s'activent ENSEMBLE — pas de mixe (tout ImGui ou tout "
                "natif). Les cases des sections Inventaire et Barre d'action "
                "reflètent le même état.\n\n"
                "ON : storage ImGui moderne (icônes, onglets, tri, drag-drop) "
                "et la fenêtre native est cachée.\nOFF : storage natif classique, aucun "
                "viewer.");

            ImGui::BeginDisabled(!stg->imgui_enabled_);

            changed |= ro::RoCheckbox("Description au survol", &stg->desc_tooltip());
            SameLine(); HelpMarker(
                "Survoler un item affiche un aperçu SIMPLIFIÉ (nom, illustration, "
                "texte) dans un panneau au skin RO, posé au curseur et effacé dès "
                "que la souris quitte la ligne.\n"
                "La description COMPLÈTE reste accessible au Ctrl + clic droit / "
                "menu contextuel.");

            changed |= ro::RoCheckbox("Onglets verticaux (à gauche)", &stg->tabs_vertical());
            SameLine(); HelpMarker(
                "Dispose les catégories en liste verticale à gauche, comme la "
                "fenêtre native.\nOFF (défaut) : onglets horizontaux en haut.");

            changed |= ro::RoCheckbox("Images d'onglet", &stg->tab_images());
            SameLine(); HelpMarker(
                "ON : tuiles images du client — jeu tab_* en disposition verticale, "
                "tabh_* en horizontale (all/use/wea/ammo/card/fav/cash/cos/etc). Les "
                "catégories sans art propre réutilisent celui de leur famille et "
                "portent alors un sigle (Am, Cs, Et).\n"
                "OFF : onglets texte — TabBar classique en horizontal, libellé écrit "
                "à la verticale en vertical.");

            changed |= ro::RoCheckbox("Champ de filtre", &stg->show_filter());
            SameLine(); HelpMarker(
                "Affiche la barre de recherche par nom au-dessus de la liste.\n"
                "Décoche pour gagner une ligne (le filtre est alors vidé).");

            changed |= ro::RoCheckbox("Valeur estimée du storage", &stg->show_total_value());
            SameLine(); HelpMarker(
                "Somme des prix de revente NPC (× quantité) des items AFFICHÉS "
                "— elle suit donc l'onglet, le sous-type et le filtre.");

            SeparatorText("Colonnes");
            changed |= ro::RoCheckbox("Index", &stg->show_index_col());
            SameLine(); HelpMarker(
                "Index storage (slot) — un item récemment ajouté a un index élevé.");
            changed |= ro::RoCheckbox("ID d'item", &stg->show_id_col());
            SameLine(); HelpMarker("Colonne avec l'id numérique de l'item.");
            changed |= ro::RoCheckbox("Slots", &stg->show_slots_col());
            SameLine(); HelpMarker("Colonne avec le nombre de slots de carte.");
            changed |= ro::RoCheckbox("Prix de revente", &stg->show_value_col());
            SameLine(); HelpMarker(
                "Colonne avec le prix de revente NPC × la quantité du stack.");

            ImGui::EndDisabled();
            if (changed) SaveSettings();
          } else {
            GrayText("(plugin indisponible)");
          }
        }

        // ── Inventaire (InventoryViewer : viewer ImGui + filtre/onglets) ──────
        if (iface_nav_ == kIfaceInventory) {
          if (auto* iv = Bourgeon::Instance().inventory_viewer()) {
            bool changed = false;
            // Interrupteur GLOBAL synchronisé : bascule aussi le storage et les
            // barres d'action (tout-ImGui ou tout-natif, plus de mixe).
            if (ro::RoCheckbox("Interface moderne (inventaire + storage + barres + échange)",
                               &iv->imgui_enabled_)) {
              SetModernInterface(iv->imgui_enabled_);
              changed = true;
            }
            SameLine(); HelpMarker(
                "Interrupteur GLOBAL : inventaire, storage, barres d'action et "
                "échange modernes s'activent ENSEMBLE — pas de mixe (tout ImGui ou tout "
                "natif). Les cases des sections Storage et Barre d'action reflètent "
                "le même état.\n\n"
                "ON : inventaire ImGui moderne (grille d'icônes, onglets, recherche, "
                "double-clic utiliser/équiper, clic-droit, drag) et la fenêtre native "
                "est cachée.\nOFF (défaut) : inventaire natif classique, aucun viewer.\n\n"
                "Inclut la fenêtre de SERTISSAGE de cartes (double-clic sur une carte) : "
                "elle remplace le popup natif « Insert Card ». La liste des équipements "
                "compatibles reste calculée par le serveur, donc identique au natif.");

            ImGui::BeginDisabled(!iv->imgui_enabled_);

            changed |= ro::RoCheckbox("Description au survol", &iv->desc_tooltip());
            SameLine(); HelpMarker(
                "Survoler un item affiche un aperçu SIMPLIFIÉ (nom, illustration, "
                "texte, cartes et options) dans un panneau au skin RO, à la place du "
                "petit tooltip nom + quantité.\n"
                "La description COMPLÈTE reste accessible au Ctrl + clic droit / "
                "menu contextuel.");

            changed |= ro::RoCheckbox("Champ de filtre", &iv->show_filter());
            SameLine(); HelpMarker(
                "Affiche la barre de recherche par nom au-dessus de la grille.\n"
                "Décoche pour gagner une ligne (le filtre est alors vidé).");

            changed |= ro::RoCheckbox("Onglets verticaux (à gauche)", &iv->tabs_vertical());
            SameLine(); HelpMarker(
                "ON (défaut) : onglets en colonne à gauche de la grille, comme la "
                "fenêtre native (images tab_*).\n"
                "OFF : rangée horizontale au-dessus de la grille (images tabh_*).");

            changed |= ro::RoCheckbox("Verrouiller la taille", &iv->lock_size());
            SameLine(); HelpMarker(
                "La fenêtre ne peut plus être redimensionnée (elle reste déplaçable).");

            // Le placement libre EXIGE la taille verrouillée : une case est un index
            // absolu (ligne × colonnes), donc changer la largeur change le nombre de
            // colonnes et mélangerait toutes les positions mémorisées.
            ImGui::BeginDisabled(!iv->lock_size());
            Indent();
              changed |= ro::RoCheckbox("Placement libre des items", &iv->free_layout());
              SameLine(); HelpMarker(
                  "Glisse un item sur une case vide pour l'y fixer ; sur une case "
                  "occupée, les deux s'échangent. Les items sans case attribuée "
                  "remplissent les trous restants, donc un nouvel objet ramassé ne "
                  "bouscule plus ta disposition.\n\n"
                  "L'onglet « Tout » n'est PAS concerné : il mélange les catégories, "
                  "donc une case n'y désigne pas le même emplacement que dans "
                  "l'onglet d'origine de l'item. Il garde le remplissage automatique.\n\n"
                  "Nécessite « Verrouiller la taille » : les cases sont repérées par "
                  "un index absolu, qu'un changement de largeur décalerait.");
            Unindent();
            ImGui::EndDisabled();

            ImGui::EndDisabled();
            if (changed) SaveSettings();
          } else {
            GrayText("(plugin indisponible)");
          }
        }
        PopItemWidth();
      }
      ImGui::PopTextWrapPos();
      ImGui::EndChild();
      PopStyleCompact();
    }
    // ── Graphismes (color grading post-process, SettingsTweaks plugin) ───────
    if (CollapsingHeader("Graphismes")) {
      PushStyleCompact();
      if (auto* st = Bourgeon::Instance().settings_tweaks())
        st->DrawSettings();

      if (auto* wds = Bourgeon::Instance().weapon_dual_sprites()) {
        if (ro::RoCheckbox("Sprites d'armes doubles", &wds->enabled()))
          SaveSettings();
        SameLine(); HelpMarker(
            "Affiche le sprite/l'animation PROPRE à chaque arme quand tu portes "
            "deux armes (assassin, kagerou/oboro) ou une seule arme en main "
            "gauche.\n\nOFF (défaut) : le client fond les deux armes en un sprite "
            "générique. ON : chaque arme garde son apparence d'origine.");
      }
      PopStyleCompact();
    }

    // ── Staff Tools (réservé group level serveur >= 80, cf. IsStaff) ──────────
    // Regroupe les fonctionnalités réservées au staff : affichage permanent des
    // noms d'entités + SPR Lab. Gaté PUREMENT sur le group level reçu au login
    // (setting id 26). Toute la section disparaît pour un non-staff, et l'overlay
    // des noms reste inerte (OnRenderUI vérifie IsStaff).
    if (IsStaff() && CollapsingHeader("Staff Tools")) {
      PushStyleCompact();

      SeparatorText("Noms des entités");
      if (auto* en = Bourgeon::Instance().entity_names())
        en->DrawSettings();

      SeparatorText("SPR Lab");
      spr_lab::DrawDebugControls();

      PopStyleCompact();
    }

    // ── Commands Settings ────────────────────────────────────────────────────
    DrawCommandsPanel();
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
        TextUnformatted(itv->second.c_str());
      else
        ImGui::Text("[%u]", g_last_viewed_item);

      int ov_idx = -1;
      for (int k = 0; k < static_cast<int>(aloot_ids_.size()); ++k)
        if (aloot_ids_[k] == g_last_viewed_item) { ov_idx = k; break; }

      SameLine();
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
      SameLine();
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
          SameLine();
          TextUnformatted(p.name.c_str());
          ImGui::PopID();
          SameLine();
        }
      }
      PopStyleCompact();
    }
    ImGui::End();
    ImGui::PopStyleVar(6);
    // Closing is done by un-ticking the "Preset bar" checkbox (no title-bar [x]).
  }
}
