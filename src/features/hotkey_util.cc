#include "features/hotkey_util.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "bourgeon.h"
#include "ragnarok/globals.h"  // rag::kStdStringDtorAddr
#include "ragnarok/uiwnd.h"    // uiwnd::kUIWindowMgrAddr (focus d'une saisie native)
#include "features/windows/character_sheet.h"  // EquipPreset (presets d'équipement)
#include "features/gameplay/player_jump.h"      // touche de saut
#include "features/fx/zone_recorder.h"          // touche d'enregistrement de zone

namespace hotkeys {
namespace {

// ── Constantes RE (client 20250716, no-ASLR : addr Ghidra == live) ───────────
constexpr uintptr_t kGetHotKey  = 0x00d80950;  // GetHotKey(out, category, slot) __stdcall RET 0xc
constexpr uintptr_t kOwnCharId  = 0x015fb9a8;  // g_Own_CharId (cf. project_own_session_globals)
using GetHotKey_t = void* (__stdcall*)(void*, int, int);
using StrFree_t   = void (__fastcall*)(void*);

// Catégories de la barre d'action interrogées : onglet 1 = 0, onglet 2 = 3,
// 36 slots chacune (cf. project_shortcut_bar_re).
constexpr int kNativeCats[2]  = {0, 3};
constexpr int kNativeSlots    = 36;

int ReadInt(uintptr_t addr) {
  __try { return *reinterpret_cast<const int*>(addr); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Lit le raccourci natif d'un slot de la barre (via GetHotKey Lua, donc à jour
// des rebinds du joueur) : touche principale + VK du modificateur (0 si aucun).
// SEH (on touche Lua, et il faut libérer les 2 std::string du wrapper).
bool ReadNativeHotkey(int category, int slot, int* main_vk, int* mod_vk) {
  *main_vk = 0;
  *mod_vk  = 0;
  bool ok = false;
  __try {
    alignas(4) uint8_t buf[0x40];
    std::memset(buf, 0, sizeof(buf));
    reinterpret_cast<GetHotKey_t>(kGetHotKey)(buf, category, slot);
    const int key_code1 = *reinterpret_cast<int*>(buf + 0x00);
    const int key_code2 = *reinterpret_cast<int*>(buf + 0x04);
    reinterpret_cast<StrFree_t>(rag::kStdStringDtorAddr)(buf + 0x08);
    reinterpret_cast<StrFree_t>(rag::kStdStringDtorAddr)(buf + 0x20);
    auto is_modifier = [](int k) {
      return k == VK_CONTROL || k == VK_SHIFT || k == VK_MENU;
    };
    if (is_modifier(key_code1))      { *mod_vk = key_code1; *main_vk = key_code2; }
    else if (is_modifier(key_code2)) { *mod_vk = key_code2; *main_vk = key_code1; }
    else                             { *main_vk = key_code1; }
    ok = (*main_vk != 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  return ok;
}

// Frame ImGui du dernier PingCapture ; très négatif = aucune capture connue.
int g_capture_frame = -1000;

}  // namespace

int ImGuiKeyToVk(ImGuiKey key) {
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z)     return 0x41 + (key - ImGuiKey_A);
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9)     return 0x30 + (key - ImGuiKey_0);
  if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12)  return 0x70 + (key - ImGuiKey_F1);
  if (key == ImGuiKey_Space)                      return VK_SPACE;
  return 0;
}

ImGuiKey VkToImGuiKey(int vkey) {
  if (vkey >= 0x41 && vkey <= 0x5A) return static_cast<ImGuiKey>(ImGuiKey_A + (vkey - 0x41));
  if (vkey >= 0x30 && vkey <= 0x39) return static_cast<ImGuiKey>(ImGuiKey_0 + (vkey - 0x30));
  if (vkey >= 0x70 && vkey <= 0x7B) return static_cast<ImGuiKey>(ImGuiKey_F1 + (vkey - 0x70));
  if (vkey == VK_SPACE)             return ImGuiKey_Space;
  return ImGuiKey_None;
}

int CaptureMainVk() {
  for (ImGuiKey k = ImGuiKey_A; k <= ImGuiKey_Z; k = static_cast<ImGuiKey>(k + 1))
    if (ImGui::IsKeyPressed(k, false)) return ImGuiKeyToVk(k);
  for (ImGuiKey k = ImGuiKey_0; k <= ImGuiKey_9; k = static_cast<ImGuiKey>(k + 1))
    if (ImGui::IsKeyPressed(k, false)) return ImGuiKeyToVk(k);
  for (ImGuiKey k = ImGuiKey_F1; k <= ImGuiKey_F12; k = static_cast<ImGuiKey>(k + 1))
    if (ImGui::IsKeyPressed(k, false)) return ImGuiKeyToVk(k);
  if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) return VK_SPACE;
  return 0;
}

void Label(int vkey, bool ctrl, bool alt, bool shift, char* out, int cap) {
  if (cap <= 0) return;
  if (vkey == 0) { std::snprintf(out, cap, "(aucun)"); return; }
  char mods[24] = {0};
  if (ctrl)  std::strncat(mods, "Ctrl+", sizeof(mods) - std::strlen(mods) - 1);
  if (alt)   std::strncat(mods, "Alt+",  sizeof(mods) - std::strlen(mods) - 1);
  if (shift) std::strncat(mods, "Maj+",  sizeof(mods) - std::strlen(mods) - 1);
  // ImGui nomme la barre d'espace « Space » : le seul libellé qu'on traduit.
  if (vkey == VK_SPACE) { std::snprintf(out, cap, "%sEspace", mods); return; }
  const char* key_name = ImGui::GetKeyName(VkToImGuiKey(vkey));
  std::snprintf(out, cap, "%s%s", mods, (key_name && key_name[0]) ? key_name : "?");
}

bool Conflict(int vkey, bool ctrl, bool alt, bool shift, Owner self, int self_index,
              char* what, int cap) {
  if (cap > 0) what[0] = '\0';
  if (vkey == 0) return false;

  // Réservé : Alt+F ouvre/ferme la fiche de personnage elle-même.
  if (vkey == 'F' && alt && !ctrl && !shift) {
    std::snprintf(what, cap, "l'ouverture de la fiche (Alt+F)");
    return true;
  }

  // a) Un preset d'équipement du personnage courant (le sien exclu).
  if (auto* character_sheet = Bourgeon::Instance().character_sheet()) {
    const uint32_t cid = static_cast<uint32_t>(ReadInt(kOwnCharId));
    const std::vector<EquipPreset>& presets = character_sheet->equip_presets();
    for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
      if (self == Owner::kEquipPreset && i == self_index) continue;
      const EquipPreset& preset = presets[i];
      if (preset.cid == cid && preset.hotkey_vk == vkey && preset.hotkey_ctrl == ctrl &&
          preset.hotkey_alt == alt && preset.hotkey_shift == shift) {
        std::snprintf(what, cap, "le preset « %s »", preset.name.c_str());
        return true;
      }
    }
  }

  // b) La touche de saut.
  if (self != Owner::kJump) {
    if (auto* player_jump = Bourgeon::Instance().player_jump()) {
      if (player_jump->key_vk() == vkey && player_jump->key_ctrl() == ctrl &&
          player_jump->key_alt() == alt && player_jump->key_shift() == shift) {
        std::snprintf(what, cap, "le saut");
        return true;
      }
    }
  }

  // c) La touche d'enregistrement de zone (staff). Contrôlée pour tout le monde et
  // pas seulement pour le staff : le niveau de groupe peut changer en cours de
  // session, et un conflit qui n'apparaîtrait qu'à ce moment-là serait
  // incompréhensible pour qui a réglé sa touche la veille.
  if (self != Owner::kZoneRecorder) {
    if (auto* zone_recorder = Bourgeon::Instance().zone_recorder()) {
      if (zone_recorder->key_vk() == vkey && zone_recorder->key_ctrl() == ctrl &&
          zone_recorder->key_alt() == alt && zone_recorder->key_shift() == shift) {
        std::snprintf(what, cap, "l'enregistrement de zone");
        return true;
      }
    }
  }

  // d) Un raccourci natif de la barre de skills/items.
  for (int category : kNativeCats)
    for (int slot = 0; slot < kNativeSlots; ++slot) {
      int main_vk, mod_vk;
      if (!ReadNativeHotkey(category, slot, &main_vk, &mod_vk) || main_vk != vkey) continue;
      const bool native_ctrl  = (mod_vk == VK_CONTROL);
      const bool native_alt   = (mod_vk == VK_MENU);
      const bool native_shift = (mod_vk == VK_SHIFT);
      if (native_ctrl == ctrl && native_alt == alt && native_shift == shift) {
        std::snprintf(what, cap, "un raccourci natif (barre de skills)");
        return true;
      }
    }
  return false;
}

// Offsets du gestionnaire de fenêtres natif (client 20250716) :
//   g_UIWindowMgr+0x24  : saisie de chat active
//   g_UIWindowMgr+0x1a0 : widget qui a le focus (UIWindowMgr_GetFocusedWnd)
//   g_UIWindowMgr+0x1c8 : fenêtre de chat ; +0xbc/+0xc0 = ses UIEditWnd
//   widget+0x10         : fenêtre propriétaire ; +0x28 = visible
bool NativeTextInputHasFocus() {
  bool focused = false;
  __try {
    char* mgr = reinterpret_cast<char*>(uiwnd::kUIWindowMgrAddr);
    void* widget = *reinterpret_cast<void**>(mgr + 0x1a0);
    void* chat   = *reinterpret_cast<void**>(mgr + 0x1c8);
    const bool chat_typing = *reinterpret_cast<uint8_t*>(mgr + 0x24) != 0;
    if (chat_typing && chat && widget) {
      char* c = reinterpret_cast<char*>(chat);
      if (widget == *reinterpret_cast<void**>(c + 0xbc) ||
          widget == *reinterpret_cast<void**>(c + 0xc0))
        focused = true;
    }
    if (!focused && widget) {
      void* owner =
          *reinterpret_cast<void**>(reinterpret_cast<char*>(widget) + 0x10);
      if (owner && owner != chat &&
          *reinterpret_cast<int*>(reinterpret_cast<char*>(owner) + 0x28) != 0)
        focused = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    focused = true;  // lecture impossible : dans le doute, on n'agit pas
  }
  return focused;
}

void PingCapture() {
  if (ImGui::GetCurrentContext() != nullptr) g_capture_frame = ImGui::GetFrameCount();
}

bool CaptureInProgress() {
  if (ImGui::GetCurrentContext() == nullptr) return false;
  // Tolérance d'une frame : OnKeyDown tombe dans la phase d'input, qui n'est pas
  // forcément du même côté du NewFrame que le panneau qui capture.
  return ImGui::GetFrameCount() - g_capture_frame <= 1;
}

}  // namespace hotkeys
