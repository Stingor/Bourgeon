#include "features/hotkey_util.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "bourgeon.h"
#include "ragnarok/uiwnd.h"        // uiwnd::kUIWindowMgrAddr (focus d'une saisie native)
#include "ragnarok/user_hotkey.h"  // raccourcis du CLIENT (les quatre catégories)
#include "features/hotkey_actions.h"            // actions Bourgeon liables
#include "features/windows/character_sheet.h"  // EquipPreset (presets d'équipement)
#include "features/gameplay/player_jump.h"      // touche de saut
#include "features/fx/zone_recorder.h"          // touche d'enregistrement de zone
#include "utils/i18n.h"

namespace hotkeys {
namespace {

// ── Constantes RE (client 20250716, no-ASLR : addr Ghidra == live) ───────────
constexpr uintptr_t kOwnCharId = 0x015fb9a8;  // g_Own_CharId (cf. project_own_session_globals)

int ReadInt(uintptr_t addr) {
  __try { return *reinterpret_cast<const int*>(addr); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
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
  if (vkey == 0) { std::snprintf(out, cap, i18n::Tr("(aucun)")); return; }
  char mods[24] = {0};
  if (ctrl)  std::strncat(mods, "Ctrl+", sizeof(mods) - std::strlen(mods) - 1);
  if (alt)   std::strncat(mods, "Alt+",  sizeof(mods) - std::strlen(mods) - 1);
  if (shift) std::strncat(mods, "Maj+",  sizeof(mods) - std::strlen(mods) - 1);
  // ImGui nomme la barre d'espace « Space » : le seul libellé qu'on traduit.
  if (vkey == VK_SPACE) { std::snprintf(out, cap, i18n::Tr("%sEspace"), mods); return; }
  const char* key_name = ImGui::GetKeyName(VkToImGuiKey(vkey));
  std::snprintf(out, cap, "%s%s", mods, (key_name && key_name[0]) ? key_name : "?");
}

bool Conflict(int vkey, bool ctrl, bool alt, bool shift, Owner self, int self_index,
              char* what, int cap) {
  if (cap > 0) what[0] = '\0';
  if (vkey == 0) return false;

  // Réservé : Alt+F ouvre/ferme la fiche de personnage elle-même.
  if (vkey == 'F' && alt && !ctrl && !shift) {
    std::snprintf(what, cap, i18n::Tr("l'ouverture de la fiche (Alt+F)"));
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
        std::snprintf(what, cap, i18n::Tr("le preset « %s »"), preset.name.c_str());
        return true;
      }
    }
  }

  // b) La touche de saut.
  if (self != Owner::kJump) {
    if (auto* player_jump = Bourgeon::Instance().player_jump()) {
      if (player_jump->key_vk() == vkey && player_jump->key_ctrl() == ctrl &&
          player_jump->key_alt() == alt && player_jump->key_shift() == shift) {
        std::snprintf(what, cap, i18n::Tr("le saut"));
        return true;
      }
    }
  }

  // c) Les DEUX touches de l'enregistreur de zone (staff) : celle qui filme et
  // celle qui retrace la zone. Contrôlées pour tout le monde et pas seulement pour
  // le staff : le niveau de groupe peut changer en cours de session, et un conflit
  // qui n'apparaîtrait qu'à ce moment-là serait incompréhensible pour qui a réglé
  // sa touche la veille.
  if (auto* zone_recorder = Bourgeon::Instance().zone_recorder()) {
    // Table LOCALE, donc construite à chaque appel : les i18n::Tr y sont évalués
    // après le chargement du catalogue (une table statique les figerait en
    // français, cf. la règle de migration).
    const struct {
      int         index;
      int         vkey;
      bool        ctrl, alt, shift;
      const char* what;
    } zone_keys[] = {
        {kZoneRecKeyRecord, zone_recorder->key_vk(), zone_recorder->key_ctrl(),
         zone_recorder->key_alt(), zone_recorder->key_shift(),
         i18n::Tr("l'enregistrement de zone")},
        {kZoneRecKeySelect, zone_recorder->sel_key_vk(), zone_recorder->sel_key_ctrl(),
         zone_recorder->sel_key_alt(), zone_recorder->sel_key_shift(),
         i18n::Tr("le tracé de la zone à enregistrer")},
    };
    for (const auto& zone_key : zone_keys) {
      if (self == Owner::kZoneRecorder && zone_key.index == self_index) continue;
      if (zone_key.vkey == vkey && zone_key.ctrl == ctrl && zone_key.alt == alt &&
          zone_key.shift == shift) {
        std::snprintf(what, cap, "%s", zone_key.what);
        return true;
      }
    }
  }

  // d) Une action Bourgeon (la sienne exclue). Contrôlée AVANT les raccourcis
  // natifs parce que c'est la seule qu'on sache nommer précisément au joueur.
  for (int i = 0; i < ActionCount(); ++i) {
    if (self == Owner::kAction && i == self_index) continue;
    if (!BindingAt(i).Matches(vkey, ctrl, alt, shift)) continue;
    std::snprintf(what, cap, i18n::Tr("l'action « %s »"), i18n::Tr(ActionAt(i).label_fr));
    return true;
  }

  // e) Un raccourci du CLIENT — les QUATRE catégories, pas seulement les deux
  // barres de raccourcis.
  //
  // 🔴 CE CONTRÔLE TRAVERSE LES DEUX MONDES, et il le doit. Les commandes
  // d'interface du client (Alt+E, Alt+Q…) partent par le même chemin clavier que
  // nos raccourcis : n'inspecter que les barres de skills laissait poser une
  // touche déjà prise par une commande du jeu, ce qui donnait deux actions sur
  // une frappe sans que rien ne l'annonce. Coût : une passe Lua sur ~200 lignes,
  // payée UNE fois, au moment où le joueur presse la touche à affecter.
  for (int category = 0; category < userhotkey::kCategoryCount; ++category) {
    const int row_count = userhotkey::RowCount(category);
    for (int row = 0; row < row_count; ++row) {
      userhotkey::Binding binding;
      if (!userhotkey::ReadBinding(category, row, &binding) || !binding.assigned) continue;
      auto is_modifier = [](int key_code) {
        return key_code == VK_CONTROL || key_code == VK_SHIFT || key_code == VK_MENU;
      };
      int main_vk = binding.key_code1, mod_vk = 0;
      if (is_modifier(binding.key_code1)) { mod_vk = binding.key_code1; main_vk = binding.key_code2; }
      else if (is_modifier(binding.key_code2)) { mod_vk = binding.key_code2; }
      if (main_vk != vkey) continue;
      if ((mod_vk == VK_CONTROL) != ctrl || (mod_vk == VK_MENU) != alt ||
          (mod_vk == VK_SHIFT) != shift)
        continue;
      // Le libellé du client est déjà en UTF-8 et layout-aware : on le rend tel
      // quel plutôt qu'un vague « un raccourci natif », pour que le joueur sache
      // QUOI aller changer.
      std::snprintf(what, cap, i18n::Tr("le raccourci du jeu « %s »"), binding.label);
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
