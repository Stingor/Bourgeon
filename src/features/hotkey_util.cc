#include "features/hotkey_util.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "bourgeon.h"
#include "ragnarok/globals.h"      // rag::kOwnCharIdAddr (clé des presets d'équipement)
#include "ragnarok/uiwnd.h"        // uiwnd::kUIWindowMgrAddr (focus d'une saisie native)
#include "ragnarok/user_hotkey.h"  // raccourcis du CLIENT (les quatre catégories)
#include "features/hotkey_actions.h"            // actions Bourgeon liables
#include "features/windows/character_sheet.h"  // EquipPreset (presets d'équipement)
#include "features/gameplay/keyboard_move.h"    // déplacement ZQSD
#include "features/gameplay/player_jump.h"      // touche de saut
#include "features/fx/zone_recorder.h"          // touche d'enregistrement de zone
#include "utils/i18n.h"

namespace hotkeys {
namespace {

// La lecture gardée d'un int : celle de globals.h. Le `using` laisse les
// points d'appel de ce fichier tels quels.
using rag::ReadInt;
// Frame ImGui du dernier PingCapture ; très négatif = aucune capture connue.
int g_capture_frame = -1000;

// Posé quand une action de Bourgeon prend la frappe en cours, relevé aussitôt
// après par le hook clavier. Pas d'atomique : les deux vivent dans le même
// appel, sur le fil du message (`ProcessPushButtonHook`).
bool g_key_claimed = false;

// ── Touches hors lettres / chiffres / F1-F12 / Espace ────────────────────────
// L'INVERSE de la table du backend Win32 d'ImGui (VK -> ImGuiKey). Elle est donc
// indépendante de la disposition du clavier : le backend classe par code VIRTUEL,
// et repasser par la même correspondance rend exactement le VK d'origine — celui
// que `UserKeys.lua` stocke. Le NOM affiché, lui, vient du client (`GetKeyDes`),
// qui sait l'adapter à la disposition.
//
// ⚠ Ni Échap ni les modificateurs : voir `CaptureAnyVk`.
struct KeyVk {
  ImGuiKey key;
  int      vk;
};
const KeyVk kExtendedKeys[] = {
    {ImGuiKey_F13, VK_F13}, {ImGuiKey_F14, VK_F14}, {ImGuiKey_F15, VK_F15},
    {ImGuiKey_F16, VK_F16}, {ImGuiKey_F17, VK_F17}, {ImGuiKey_F18, VK_F18},
    {ImGuiKey_F19, VK_F19}, {ImGuiKey_F20, VK_F20}, {ImGuiKey_F21, VK_F21},
    {ImGuiKey_F22, VK_F22}, {ImGuiKey_F23, VK_F23}, {ImGuiKey_F24, VK_F24},
    // Édition et navigation.
    {ImGuiKey_Tab, VK_TAB},           {ImGuiKey_Enter, VK_RETURN},
    {ImGuiKey_Backspace, VK_BACK},    {ImGuiKey_Insert, VK_INSERT},
    {ImGuiKey_Delete, VK_DELETE},     {ImGuiKey_Home, VK_HOME},
    {ImGuiKey_End, VK_END},           {ImGuiKey_PageUp, VK_PRIOR},
    {ImGuiKey_PageDown, VK_NEXT},     {ImGuiKey_LeftArrow, VK_LEFT},
    {ImGuiKey_RightArrow, VK_RIGHT},  {ImGuiKey_UpArrow, VK_UP},
    {ImGuiKey_DownArrow, VK_DOWN},
    // Verrous et touches système.
    {ImGuiKey_CapsLock, VK_CAPITAL},  {ImGuiKey_ScrollLock, VK_SCROLL},
    {ImGuiKey_NumLock, VK_NUMLOCK},   {ImGuiKey_Pause, VK_PAUSE},
    {ImGuiKey_PrintScreen, VK_SNAPSHOT},
    // Ponctuation (codes OEM : le backend les classe par VK, pas par caractère).
    {ImGuiKey_Apostrophe, VK_OEM_7},     {ImGuiKey_Comma, VK_OEM_COMMA},
    {ImGuiKey_Minus, VK_OEM_MINUS},      {ImGuiKey_Period, VK_OEM_PERIOD},
    {ImGuiKey_Slash, VK_OEM_2},          {ImGuiKey_Semicolon, VK_OEM_1},
    {ImGuiKey_Equal, VK_OEM_PLUS},       {ImGuiKey_LeftBracket, VK_OEM_4},
    {ImGuiKey_Backslash, VK_OEM_5},      {ImGuiKey_RightBracket, VK_OEM_6},
    {ImGuiKey_GraveAccent, VK_OEM_3},
    // Pavé numérique.
    {ImGuiKey_Keypad0, VK_NUMPAD0}, {ImGuiKey_Keypad1, VK_NUMPAD1},
    {ImGuiKey_Keypad2, VK_NUMPAD2}, {ImGuiKey_Keypad3, VK_NUMPAD3},
    {ImGuiKey_Keypad4, VK_NUMPAD4}, {ImGuiKey_Keypad5, VK_NUMPAD5},
    {ImGuiKey_Keypad6, VK_NUMPAD6}, {ImGuiKey_Keypad7, VK_NUMPAD7},
    {ImGuiKey_Keypad8, VK_NUMPAD8}, {ImGuiKey_Keypad9, VK_NUMPAD9},
    {ImGuiKey_KeypadDecimal, VK_DECIMAL},  {ImGuiKey_KeypadDivide, VK_DIVIDE},
    {ImGuiKey_KeypadMultiply, VK_MULTIPLY},{ImGuiKey_KeypadSubtract, VK_SUBTRACT},
    {ImGuiKey_KeypadAdd, VK_ADD},          {ImGuiKey_KeypadEnter, VK_RETURN},
};

}  // namespace

int ImGuiKeyToVk(ImGuiKey key) {
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z)     return 0x41 + (key - ImGuiKey_A);
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9)     return 0x30 + (key - ImGuiKey_0);
  if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12)  return 0x70 + (key - ImGuiKey_F1);
  if (key == ImGuiKey_Space)                      return VK_SPACE;
  for (const KeyVk& entry : kExtendedKeys)
    if (entry.key == key) return entry.vk;
  return 0;
}

ImGuiKey VkToImGuiKey(int vkey) {
  if (vkey >= 0x41 && vkey <= 0x5A) return static_cast<ImGuiKey>(ImGuiKey_A + (vkey - 0x41));
  if (vkey >= 0x30 && vkey <= 0x39) return static_cast<ImGuiKey>(ImGuiKey_0 + (vkey - 0x30));
  if (vkey >= 0x70 && vkey <= 0x7B) return static_cast<ImGuiKey>(ImGuiKey_F1 + (vkey - 0x70));
  if (vkey == VK_SPACE)             return ImGuiKey_Space;
  for (const KeyVk& entry : kExtendedKeys)
    if (entry.vk == vkey) return entry.key;
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

int CaptureActionVk() {
  if (const int vkey = CaptureMainVk()) return vkey;
  // Tab : routée par le jeu, libre de tout usage natif hors saisie. Cf. le
  // header pour la démonstration.
  if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) return VK_TAB;
  return 0;
}

int CaptureAnyVk() {
  if (const int vkey = CaptureMainVk()) return vkey;
  for (const KeyVk& entry : kExtendedKeys) {
    // 🔴 IMPR. ÉCRAN N'EST PAS AFFECTABLE, et ce n'est pas un manque : WINDOWS la
    // prend avant le jeu. Sur Windows 11 elle ouvre l'Outil Capture d'écran, et
    // elle n'émet même pas de WM_KEYDOWN — seulement le WM_KEYUP, ce qui est déjà
    // la raison pour laquelle le client la traite dans `Game_MainWndProc` sur
    // `case WM_KEYUP`. Un raccourci posé dessus se déclencherait EN PLUS de la
    // capture système, sans qu'on puisse jamais le lui reprendre. Elle reste dans
    // la table ci-dessus pour être NOMMÉE, pas pour être choisie.
    if (entry.key == ImGuiKey_PrintScreen) continue;
    if (ImGui::IsKeyPressed(entry.key, false)) return entry.vk;
  }
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

// Ajoute un propriétaire à la liste. Le compteur avance MÊME quand la liste est
// pleine : l'appelant apprend ainsi qu'il en reste, plutôt que d'en laisser un
// debout en croyant les avoir tous libérés.
static void AddConflictOwner(ConflictOwner* out, int max_out, int* count, Owner owner,
                             int index, const char* what) {
  if (out && *count < max_out) {
    ConflictOwner& entry = out[*count];
    entry = ConflictOwner();
    entry.owner = owner;
    entry.index = index;
    std::snprintf(entry.what, sizeof(entry.what), "%s", (what && what[0]) ? what : "?");
  }
  ++*count;
}

int FindConflicts(int vkey, bool ctrl, bool alt, bool shift, Owner self, int self_index,
                  ConflictOwner* out, int max_out) {
  int count = 0;
  if (vkey == 0) return 0;
  if (max_out < 0) max_out = 0;

  // Réservé : Alt+F ouvre/ferme la fiche de personnage elle-même. Seul cas NON
  // libérable — cette touche n'appartient à aucune ligne d'aucune table, il n'y a
  // donc rien à délier, et rien à voler.
  if (vkey == 'F' && alt && !ctrl && !shift) {
    AddConflictOwner(out, max_out, &count, Owner::kNone, -1,
                     i18n::Tr("l'ouverture de la fiche (Alt+F)"));
    if (out && count <= max_out) out[count - 1].releasable = false;
    return count;
  }

  // a) Un preset d'équipement du personnage courant (le sien exclu).
  if (auto* character_sheet = Bourgeon::Instance().character_sheet()) {
    const uint32_t cid = static_cast<uint32_t>(ReadInt(rag::kOwnCharIdAddr));
    const std::vector<EquipPreset>& presets = character_sheet->equip_presets();
    for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
      if (self == Owner::kEquipPreset && i == self_index) continue;
      const EquipPreset& preset = presets[i];
      if (preset.cid == cid && preset.hotkey_vk == vkey && preset.hotkey_ctrl == ctrl &&
          preset.hotkey_alt == alt && preset.hotkey_shift == shift) {
        char what[128];
        std::snprintf(what, sizeof(what), i18n::Tr("le preset « %s »"),
                      preset.name.c_str());
        AddConflictOwner(out, max_out, &count, Owner::kEquipPreset, i, what);
      }
    }
  }

  // b) La touche de saut.
  if (self != Owner::kJump) {
    if (auto* player_jump = Bourgeon::Instance().player_jump()) {
      if (player_jump->key_vk() == vkey && player_jump->key_ctrl() == ctrl &&
          player_jump->key_alt() == alt && player_jump->key_shift() == shift) {
        AddConflictOwner(out, max_out, &count, Owner::kJump, -1, i18n::Tr("le saut"));
      }
    }
  }

  // c) Les TROIS touches de l'enregistreur de zone (staff) : celle qui filme,
  // celle qui capture une image fixe, celle qui retrace la zone. Contrôlées pour
  // tout le monde et pas seulement pour
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
        {kZoneRecKeyShot, zone_recorder->shot_key_vk(), zone_recorder->shot_key_ctrl(),
         zone_recorder->shot_key_alt(), zone_recorder->shot_key_shift(),
         i18n::Tr("la capture d'image de la zone")},
    };
    for (const auto& zone_key : zone_keys) {
      if (self == Owner::kZoneRecorder && zone_key.index == self_index) continue;
      if (zone_key.vkey == vkey && zone_key.ctrl == ctrl && zone_key.alt == alt &&
          zone_key.shift == shift) {
        AddConflictOwner(out, max_out, &count, Owner::kZoneRecorder, zone_key.index,
                         zone_key.what);
      }
    }
  }

  // d) Le déplacement au clavier, SI le joueur l'a activé.
  //
  // ⚠ On ne le contrôle QUE lorsqu'il est actif : ses touches par défaut sont des
  // lettres courantes, et les interdire à tout le monde les retirerait à ceux qui
  // ne s'en servent pas. Et seulement pour un combo SANS modificateur, puisque le
  // déplacement refuse lui-même de marcher dès qu'un modificateur est enfoncé —
  // « Ctrl+Z » ne peut donc pas entrer en conflit avec lui.
  if (!ctrl && !alt && !shift) {
    if (auto* keyboard_move = Bourgeon::Instance().keyboard_move()) {
      if (keyboard_move->enabled()) {
        for (int slot = 0; slot < KeyboardMove::kMoveKeyCount; ++slot) {
          if (self == Owner::kKeyboardMove && slot == self_index) continue;
          if (keyboard_move->keys_[slot] != vkey) continue;
          AddConflictOwner(out, max_out, &count, Owner::kKeyboardMove, slot,
                           i18n::Tr("le déplacement au clavier"));
        }
      }
    }
  }

  // e) Une action Bourgeon (la sienne exclue). Contrôlée AVANT les raccourcis
  // natifs parce que c'est la seule qu'on sache nommer précisément au joueur.
  for (int i = 0; i < ActionCount(); ++i) {
    if (self == Owner::kAction && i == self_index) continue;
    if (!BindingAt(i).Matches(vkey, ctrl, alt, shift)) continue;
    char what[128];
    std::snprintf(what, sizeof(what), i18n::Tr("l'action « %s »"),
                  i18n::Tr(ActionAt(i).label_fr));
    AddConflictOwner(out, max_out, &count, Owner::kAction, i, what);
  }

  // f) Un raccourci du CLIENT — les QUATRE catégories, pas seulement les deux
  // barres de raccourcis.
  //
  // 🔴 CE CONTRÔLE TRAVERSE LES DEUX MONDES, et il le doit. Les commandes
  // d'interface du client (Alt+E, Alt+Q…) partent par le même chemin clavier que
  // nos raccourcis : n'inspecter que les barres de skills laissait poser une
  // touche déjà prise par une commande du jeu, ce qui donnait deux actions sur
  // une frappe sans que rien ne l'annonce. Coût : une passe Lua sur ~200 lignes,
  // payée UNE fois, au moment où le joueur presse la touche à affecter.
  //
  // 🔴 UNE EXEMPTION, CELLE DU NATIF : les catégories 0 et 3 sont les deux PAGES
  // de la même barre de raccourcis, permutées par une option — elles ont le droit
  // de partager une touche, et `UIHotKeyWnd_ValidateKeyCombo` (0x008DC890) les
  // saute explicitement l'une pour l'autre. La refuser ici rendrait impossible ce
  // que la fenêtre du jeu autorise (docs/game_option_re.md §4.9).
  const int self_category =
      (self == Owner::kClientCommand) ? self_index / kClientSelfScale : -1;
  const int self_command =
      (self == Owner::kClientCommand) ? self_index % kClientSelfScale : -1;
  auto is_skill_bar = [](int category) {
    return category == userhotkey::kSkillBar1 || category == userhotkey::kSkillBar2;
  };

  for (int category = 0; category < userhotkey::kCategoryCount; ++category) {
    if (self_category >= 0 && category != self_category && is_skill_bar(self_category) &&
        is_skill_bar(category))
      continue;
    const int row_count = userhotkey::RowCount(category);
    for (int row = 0; row < row_count; ++row) {
      userhotkey::Binding binding;
      if (!userhotkey::ReadBinding(category, row, &binding)) continue;
      // 🔴 `GetHotKey` REND DÉJÀ LA TOUCHE EFFECTIVE : la touche d'origine quand
      // `UserKeys.lua` n'a pas d'entrée, la surcharge quand il en a une, et RIEN
      // quand la commande a été déliée (le fichier porte alors une entrée sans
      // `KEY1`, ce que fait l'Échap du natif comme notre effacement).
      // ⚠ Ne PAS retomber sur `GetOriginalHotKeyInfo` ici : ce serait déclarer
      // occupée la touche d'origine d'une commande que le joueur vient justement
      // de délier pour s'en servir ailleurs. Erreur commise puis corrigée le
      // 2026-08-14, sur une lecture trop rapide d'un `USERKEY_2` vide.
      if (!binding.assigned) continue;
      if (category == self_category && binding.command_index == self_command) continue;
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
      char what[128];
      std::snprintf(what, sizeof(what), i18n::Tr("le raccourci du jeu « %s »"),
                    binding.label[0] ? binding.label : "?");
      AddConflictOwner(out, max_out, &count, Owner::kClientCommand,
                       ClientSelf(category, binding.command_index), what);
      // 🔴 Le VOL a besoin de la commande DÉCOMPOSÉE et de son champ `EXE` : c'est
      // ce couple-là qu'attend `ChangeUserHotKey` pour écrire l'entrée sans `KEY1`
      // qui la délie. Le `self_index` encodé ne suffirait pas — le libellé n'en
      // fait pas partie, et sans lui le Lua n'identifie pas la ligne.
      if (out && count <= max_out) {
        ConflictOwner& entry = out[count - 1];
        entry.category = category;
        entry.command_index = binding.command_index;
        std::snprintf(entry.label, sizeof(entry.label), "%s", binding.label);
      }
    }
  }
  return count;
}

bool Conflict(int vkey, bool ctrl, bool alt, bool shift, Owner self, int self_index,
              char* what, int cap) {
  if (cap > 0) what[0] = '\0';
  ConflictOwner first;
  if (FindConflicts(vkey, ctrl, alt, shift, self, self_index, &first, 1) <= 0) return false;
  if (cap > 0) std::snprintf(what, cap, "%s", first.what);
  return true;
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

void ClaimKey() { g_key_claimed = true; }

bool TakeKeyClaim() {
  const bool claimed = g_key_claimed;
  g_key_claimed = false;
  return claimed;
}

bool CaptureInProgress() {
  if (ImGui::GetCurrentContext() == nullptr) return false;
  // Tolérance d'une frame : OnKeyDown tombe dans la phase d'input, qui n'est pas
  // forcément du même côté du NewFrame que le panneau qui capture.
  return ImGui::GetFrameCount() - g_capture_frame <= 1;
}

}  // namespace hotkeys
