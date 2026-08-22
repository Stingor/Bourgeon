#include "features/hotkey_actions.h"

#include <Windows.h>

#include <cstring>

#include "bourgeon.h"
#include "features/overlays/target_frame.h"  // ciblage clavier
#include "features/windows/bank_window.h"
#include "features/windows/craft_atlas.h"
#include "features/windows/game_menu.h"
#include "features/windows/hotkey_settings.h"
#include "features/windows/navigation_window.h"
#include "features/staff_gate.h"  // IsStaff (actions réservées)
#include "features/systems/bug_report.h"  // la modale du rapport générique
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

// ⚠ PRESQUE aucun raccourci par DÉFAUT n'est proposé ({} = aucune touche). Poser
// des défauts demande de vérifier qu'aucun ne marche déjà sur une touche du
// CLIENT, et un défaut qui écrase silencieusement un raccourci du jeu serait
// exactement le genre de panne qu'on ne relie jamais à sa cause.
//
// 🔴 LA SEULE EXCEPTION EST LE RAPPORT DE BUG, et c'est un défaut CONSTATÉ, pas
// choisi : Ctrl+Alt+B était déjà livré, câblé en dur dans `BugReport`. Le porter
// ici sans son combo l'aurait RETIRÉ aux joueurs qui s'en servent
// (feedback_ui_conventions : changer un défaut livré demanderait de renommer la
// clé — ici on le préserve, justement pour n'avoir rien à renommer).
const Action kActions[] = {
    // ── Fenêtres ────────────────────────────────────────────────────────────
    {"win_inventory",    "Inventaire",              ActionGroup::kWindows, kWndInventory,   {}},
    {"win_cart",         "Cart",                    ActionGroup::kWindows, kWndCart,        {}},
    {"win_storage",      "Storage",                 ActionGroup::kWindows, kWndStorage,     {}},
    {"win_sheet_stats",  "Fiche : caractéristiques", ActionGroup::kWindows, kWndStatus,     {}},
    {"win_sheet_equip",  "Fiche : équipement",      ActionGroup::kWindows, kWndEquipment,   {}},
    {"win_sheet_skills", "Fiche : compétences",     ActionGroup::kWindows, kWndSkills,      {}},
    {"win_rodex",        "Courrier",                ActionGroup::kWindows, kWndRodex,       {}},
    {"win_achievements", "Succès",                  ActionGroup::kWindows, kWndAchievements,{}},
    {"win_quests",       "Journal de quêtes",       ActionGroup::kWindows, kWndQuestJournal,{}},
    {"win_worldmap",     "Carte du monde",          ActionGroup::kWindows, kWndWorldMap,    {}},
    // ── Sans équivalent natif : traitées dans Invoke ────────────────────────
    {"win_bank",         "Banque",                  ActionGroup::kWindows, 0, {}},
    {"win_game_menu",    "Menu du jeu",             ActionGroup::kWindows, 0, {}},
    {"win_hotkeys",      "Raccourcis clavier",      ActionGroup::kTools,   0, {}},
    // Navigation. ⚠ Elle reste à 0 alors que sa native (203) EST routée, et ce
    // n'est pas un oubli : `native_window_id` passe par `MakeWindow`, dont notre
    // hook fait une BASCULE. Or le panneau ne s'ouvre que si l'interface moderne
    // est active ; éteinte, `MakeWindow(203)` rendrait la fenêtre native, et ce
    // raccourci-ci n'a alors rien à ouvrir. Le chemin direct marche dans les deux
    // cas — cf. `Invoke`, qui refuse proprement quand le panneau est absent.
    {"win_navigation",   "Navigation",              ActionGroup::kWindows, 0, {}},
    {"tool_craft_atlas", "Atlas des recettes",      ActionGroup::kTools,   0, {}},
    {"tool_palette",     "Style du personnage",     ActionGroup::kTools,   0, {}},
    // Ciblage clavier. 🔴 AUCUN défaut n'est proposé (`{}`), et c'est délibéré :
    // la touche qui vient à l'esprit — Tab — sert déjà au client (bascule du
    // chat), et poser un défaut qui vole une touche du jeu est le genre de cadeau
    // qu'on passe sa vie à retirer. Le joueur choisit.
    {"target_cycle_next", "Cible suivante",         ActionGroup::kTools,   0, {}},
    {"target_cycle_prev", "Cible précédente",       ActionGroup::kTools,   0, {}},
    // Rapport de bug. 🔴 SEULE ACTION À PORTER UN DÉFAUT : Ctrl+Alt+B est le combo
    // sous lequel elle a été livrée, et le catalogue le reprend tel quel plutôt que
    // de le laisser en dur dans `BugReport` — invisible à l'écran des raccourcis,
    // donc introuvable et surtout impossible à déplacer quand il tombe sur la
    // touche d'autre chose. Le contrôle de collision le voit maintenant comme
    // Et l'action qu'on refait trente fois par combat : engager le plus proche,
    // sans rien parcourir. Le cyclage sert à EXPLORER, celle-ci à ENGAGER — les
    // confondre obligeait à deviner où le cycle en était resté.
    {"target_nearest",    "Cible la plus proche",   ActionGroup::kTools,   0, {}},
    // n'importe quelle autre liaison.
    {"tool_bug_report",   "Signaler un bug",       ActionGroup::kTools,   0,
     {'B', /*ctrl=*/true, /*alt=*/true, /*shift=*/false}},
    // Établi du staff. Le seul membre du catalogue à être gaté : il ne s'affiche
    // même pas dans l'écran des raccourcis d'un joueur ordinaire.
    {"tool_staff",       "Staff Tools",             ActionGroup::kTools,   0, {}, true},
};

constexpr int kActionCount = static_cast<int>(sizeof(kActions) / sizeof(kActions[0]));

// Liaisons du joueur, indexées comme `kActions`. Volontairement PARALLÈLE au
// catalogue plutôt que rangée dedans : le catalogue est const, et l'index n'est
// jamais la clé de persistance (c'est `id`), donc réordonner les actions ne
// déplace aucun raccourci.
//
// 🔴 AMORCÉES SUR LES DÉFAUTS DÈS LE CHARGEMENT, et pas seulement à la lecture du
// yaml. `ReadBourgeonHotkeys` n'est PAS un passage obligé : `LoadSettings` sort
// avant lui quand le fichier n'existe pas encore (premier lancement) ou n'a pas
// de section `moonlight_ui`. Ne compter que sur lui laisserait un joueur neuf
// sans le Ctrl+Alt+B du rapport de bug — c'est-à-dire sans le raccourci qu'on est
// justement en train de rendre réglable.
// `kActions` est un agrégat de littéraux, donc prêt avant tout initialiseur
// dynamique de cette unité : la boucle ci-dessous le lit sans course.
struct BindingTable {
  Binding items[kActionCount];
  BindingTable() {
    for (int i = 0; i < kActionCount; ++i) items[i] = kActions[i].default_binding;
  }
};
BindingTable g_bindings;

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
  // Hors bornes : « aucune touche », et surtout PAS la première entrée — ce serait
  // attribuer à un index inconnu le raccourci de la première action.
  static const Binding kNone;
  if (index < 0 || index >= kActionCount) return kNone;
  return g_bindings.items[index];
}

void SetBinding(int index, const Binding& binding) {
  if (index < 0 || index >= kActionCount) return;
  g_bindings.items[index] = binding;
}

void ResetBindingsToDefaults() {
  for (int i = 0; i < kActionCount; ++i) g_bindings.items[i] = kActions[i].default_binding;
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
  if (std::strcmp(id, "win_navigation") == 0) {
    if (auto* navigation = bourgeon.navigation_window()) {
      navigation->Toggle();
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
  // Ciblage clavier : le HUD de cible porte la mécanique (il tient déjà la cible
  // courante), et il pose la sélection NATIVE — donc la flèche du jeu suit.
  // `false` quand il n'y a rien à cibler : le raccourci reste silencieux plutôt
  // que de faire semblant.
  if (std::strcmp(id, "target_cycle_next") == 0) {
    if (auto* target_frame = bourgeon.target_frame())
      return target_frame->CycleTarget(true);
    return false;
  }
  if (std::strcmp(id, "target_cycle_prev") == 0) {
    if (auto* target_frame = bourgeon.target_frame())
      return target_frame->CycleTarget(false);
    return false;
  }
  // Rapport de bug. `Open` ne fait que poser une demande traitée au frame suivant
  // (aucune commande native), mais on passe quand même par le tick comme tout le
  // monde. `enabled()` est l'opt-out global : coupé, l'action ne doit pas rouvrir
  // par le clavier ce que le joueur a fermé dans les réglages.
  if (std::strcmp(id, "tool_bug_report") == 0) {
    auto* bug_report = bourgeon.bug_report();
    if (!bug_report || !bug_report->enabled()) return false;
    bug_report->Open(BugReport::GenericContext());
    return true;
  }
  if (std::strcmp(id, "tool_staff") == 0) {
    if (auto* staff_tools = bourgeon.staff_tools()) { staff_tools->Toggle(); return true; }
    return false;
  }
  return false;
}

}  // namespace hotkeys
  if (std::strcmp(id, "target_nearest") == 0) {
    if (auto* target_frame = bourgeon.target_frame())
      return target_frame->TargetNearest();
    return false;
  }
