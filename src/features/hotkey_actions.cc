#include "features/hotkey_actions.h"

#include <Windows.h>

#include <cstring>

#include "bourgeon.h"
#include "features/windows/bank_window.h"
#include "features/windows/craft_atlas.h"
#include "features/windows/game_menu.h"
#include "features/windows/hotkey_settings.h"
#include "features/staff_gate.h"  // IsStaff (actions réservées)
#include "features/windows/palette_editor.h"
#include "features/windows/staff_tools.h"
#include "ragnarok/uiwnd.h"

namespace hotkeys {
namespace {

// ── Identifiants de fenêtres natives ─────────────────────────────────────────
// Ces demandes d'ouverture sont INTERCEPTÉES par nos hooks quand l'interface
// moderne est active (cf. window_pos_tweaks) : elles atterrissent sur nos
// panneaux. Interface native = elles ouvrent la fenêtre du client, ce qui reste
// le comportement correct.
constexpr int kWndInventory   = 0x08;
constexpr int kWndEquipment    = 0x0a;   // -> onglet mannequin de la fiche
constexpr int kWndStatus       = 0x0b;   // -> onglet stats de la fiche
constexpr int kWndSkills       = 0x25;   // -> onglet grimoire de la fiche
constexpr int kWndStorage      = 0x21;
constexpr int kWndCart         = 0x28;
constexpr int kWndRodex        = 0x107;
constexpr int kWndAchievements = 0x10e;
constexpr int kWndQuestJournal = 0x141;
constexpr int kWndWorldMap     = 0x8c;

// ⚠ Aucun raccourci par DÉFAUT n'est proposé pour l'instant (0/0). Poser des
// défauts demanderait de vérifier qu'aucun ne marche déjà sur une touche du
// CLIENT — le contrôle de conflit traversant les deux mondes n'existe pas encore,
// et un défaut qui écrase silencieusement un raccourci du jeu serait exactement
// le genre de panne qu'on ne relie jamais à sa cause.
const Action kActions[] = {
    // ── Fenêtres ────────────────────────────────────────────────────────────
    {"win_inventory",    "Inventaire",              ActionGroup::kWindows, kWndInventory,   0, 0},
    {"win_cart",         "Cart",                    ActionGroup::kWindows, kWndCart,        0, 0},
    {"win_storage",      "Storage",                 ActionGroup::kWindows, kWndStorage,     0, 0},
    {"win_sheet_stats",  "Fiche : caractéristiques", ActionGroup::kWindows, kWndStatus,     0, 0},
    {"win_sheet_equip",  "Fiche : équipement",      ActionGroup::kWindows, kWndEquipment,   0, 0},
    {"win_sheet_skills", "Fiche : compétences",     ActionGroup::kWindows, kWndSkills,      0, 0},
    {"win_rodex",        "Courrier",                ActionGroup::kWindows, kWndRodex,       0, 0},
    {"win_achievements", "Succès",                  ActionGroup::kWindows, kWndAchievements,0, 0},
    {"win_quests",       "Journal de quêtes",       ActionGroup::kWindows, kWndQuestJournal,0, 0},
    {"win_worldmap",     "Carte du monde",          ActionGroup::kWindows, kWndWorldMap,    0, 0},
    // ── Sans équivalent natif : traitées dans Invoke ────────────────────────
    {"win_bank",         "Banque",                  ActionGroup::kWindows, 0, 0, 0},
    {"win_game_menu",    "Menu du jeu",             ActionGroup::kWindows, 0, 0, 0},
    {"win_hotkeys",      "Raccourcis clavier",      ActionGroup::kTools,   0, 0, 0},
    {"tool_craft_atlas", "Atlas des recettes",      ActionGroup::kTools,   0, 0, 0},
    {"tool_palette",     "Style du personnage",     ActionGroup::kTools,   0, 0, 0},
    // Établi du staff. Le seul membre du catalogue à être gaté : il ne s'affiche
    // même pas dans l'écran des raccourcis d'un joueur ordinaire.
    {"tool_staff",       "Staff Tools",             ActionGroup::kTools,   0, 0, 0, true},
};

constexpr int kActionCount = static_cast<int>(sizeof(kActions) / sizeof(kActions[0]));

// Liaisons du joueur, indexées comme `kActions`. Volontairement PARALLÈLE au
// catalogue plutôt que rangée dedans : le catalogue est const, et l'index n'est
// jamais la clé de persistance (c'est `id`), donc réordonner les actions ne
// déplace aucun raccourci.
Binding g_bindings[kActionCount];

}  // namespace

int ActionCount() { return kActionCount; }

const Action& ActionAt(int index) {
  if (index < 0 || index >= kActionCount) return kActions[0];
  return kActions[index];
}

const Action* FindAction(const char* id) {
  const int index = IndexOf(id);
  return index < 0 ? nullptr : &kActions[index];
}

int IndexOf(const char* id) {
  if (!id || !*id) return -1;
  for (int i = 0; i < kActionCount; ++i)
    if (std::strcmp(kActions[i].id, id) == 0) return i;
  return -1;
}

const Binding& BindingAt(int index) {
  // Hors bornes : « aucune touche », et surtout PAS `g_bindings[0]` — ce serait
  // attribuer à un index inconnu le raccourci de la première action.
  static const Binding kNone;
  if (index < 0 || index >= kActionCount) return kNone;
  return g_bindings[index];
}

void SetBinding(int index, const Binding& binding) {
  if (index < 0 || index >= kActionCount) return;
  g_bindings[index] = binding;
}

bool Invoke(const char* id) {
  const Action* action = FindAction(id);
  if (!action) return false;
  // Droit relu ICI, à l'exécution : une liaison posée par un compte staff reste
  // dans le yaml quand le niveau de groupe change, et ne doit alors plus rien
  // déclencher.
  if (action->staff_only && !IsStaff()) return false;

  // Le cas général : demander l'ouverture au client, et laisser nos hooks router
  // vers le panneau moderne. Un seul chemin, qui reste correct en interface
  // native.
  if (action->native_window_id > 0) {
    uiwnd::MakeWindow(action->native_window_id);
    return true;
  }

  Bourgeon& bourgeon = Bourgeon::Instance();

  if (std::strcmp(id, "win_bank") == 0) {
    if (auto* bank = bourgeon.bank_window()) { bank->ToggleFromUi(); return true; }
    return false;
  }
  if (std::strcmp(id, "win_game_menu") == 0) {
    if (auto* menu = bourgeon.game_menu()) { menu->ToggleFromUi(); return true; }
    return false;
  }
  if (std::strcmp(id, "win_hotkeys") == 0) {
    if (auto* hotkeys_window = bourgeon.hotkey_settings()) {
      hotkeys_window->OpenFromMenu();
      return true;
    }
    return false;
  }
  if (std::strcmp(id, "tool_craft_atlas") == 0) {
    if (auto* atlas = bourgeon.craft_atlas()) { atlas->Toggle(); return true; }
    return false;
  }
  if (std::strcmp(id, "tool_palette") == 0) {
    if (auto* palette = bourgeon.palette_editor()) { palette->Toggle(); return true; }
    return false;
  }
  if (std::strcmp(id, "tool_staff") == 0) {
    if (auto* staff_tools = bourgeon.staff_tools()) { staff_tools->Toggle(); return true; }
    return false;
  }
  return false;
}

}  // namespace hotkeys
