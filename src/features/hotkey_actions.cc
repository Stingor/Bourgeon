#include "features/hotkey_actions.h"

#include <Windows.h>

#include <cstring>

#include "bourgeon.h"
#include "features/gameplay/afk_screen.h"    // StartNow (écran de veille)
#include "features/hotkey_util.h"           // VkToImGuiKey
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
#include "imgui.h"
// 🔴 `ConfigNavWindowingKey*` vit dans `ImGuiContext`, pas dans `ImGuiIO` : sans
// cet en-tête, le champ n'existe pas de notre côté de la bibliothèque.
#include "imgui_internal.h"
#include "ragnarok/uiwnd.h"

namespace hotkeys {
namespace {

// ⚠ Les demandes d'ouverture de fenêtre native (les `uiwnd::kUI…Wnd` plus bas)
// sont INTERCEPTÉES par nos hooks quand l'interface moderne est active (cf.
// window_pos_tweaks) : elles atterrissent sur nos panneaux. Interface native =
// elles ouvrent la fenêtre du client, ce qui reste le comportement correct.

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
    {"win_inventory",    "Inventaire",              ActionGroup::kWindows, uiwnd::kUIInventoryWnd,    {}},
    {"win_cart",         "Cart",                    ActionGroup::kWindows, uiwnd::kUICartWnd,         {}},
    {"win_storage",      "Storage",                 ActionGroup::kWindows, uiwnd::kUIItemStoreWnd,    {}},
    {"win_sheet_stats",  "Fiche : caractéristiques", ActionGroup::kWindows, uiwnd::kUIStatusWnd,      {}},
    {"win_sheet_equip",  "Fiche : équipement",      ActionGroup::kWindows, uiwnd::kUIEquipWnd,        {}},
    {"win_sheet_skills", "Fiche : compétences",     ActionGroup::kWindows, uiwnd::kUINewSkillListWnd, {}},
    {"win_rodex",        "Courrier",                ActionGroup::kWindows, uiwnd::kUIRodexWnd,        {}},
    {"win_achievements", "Succès",                  ActionGroup::kWindows, uiwnd::kUIAchievementWnd,  {}},
    {"win_quests",       "Journal de quêtes",       ActionGroup::kWindows, uiwnd::kQuestJournalWndId, {}},
    {"win_worldmap",     "Carte du monde",          ActionGroup::kWindows, uiwnd::kUIRoMapWnd,        {}},
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
    // Et l'action qu'on refait trente fois par combat : engager le plus proche,
    // sans rien parcourir. Le cyclage sert à EXPLORER, celle-ci à ENGAGER — les
    // confondre obligeait à deviner où le cycle en était resté.
    {"target_nearest",    "Cible la plus proche",   ActionGroup::kTools,   0, {}},
    // Rapport de bug. 🔴 SEULE ACTION À PORTER UN DÉFAUT : Ctrl+Alt+B est le combo
    // sous lequel elle a été livrée, et le catalogue le reprend tel quel plutôt que
    // de le laisser en dur dans `BugReport` — invisible à l'écran des raccourcis,
    // donc introuvable et surtout impossible à déplacer quand il tombe sur la
    // touche d'autre chose. Le contrôle de collision le voit maintenant comme
    // n'importe quelle autre liaison.
    {"tool_bug_report",   "Signaler un bug",       ActionGroup::kTools,   0,
     {'B', /*ctrl=*/true, /*alt=*/true, /*shift=*/false}},
    // Cyclage des fenêtres de Bourgeon. 🔴 SECONDE ACTION À DÉFAUT, et pour la même
    // raison que le rapport de bug : Ctrl+Tab est un combo LIVRÉ, pas choisi. ImGui
    // le tenait en dur (`NavUpdateWindowing`, actif même sans `NavEnableKeyboard`),
    // donc invisible dans l'écran des raccourcis, exclu du contrôle de collision et
    // impossible à déplacer. Le reprendre tel quel ne change rien pour qui s'en
    // sert, et rend enfin la touche effaçable à qui la subit
    // (feedback_ui_conventions : c'est justement pour n'avoir aucune clé à renommer).
    //
    // 🔴 Elle n'est pas exécutée par `Invoke` : `imgui_windowing` dit que son combo
    // repart chez ImGui (cf. `ApplyImGuiWindowingChord`). Maj inverse le sens du
    // cycle, ce qui est le geste d'origine et n'a donc pas de ligne à lui.
    {"ui_cycle_windows", "Cycler entre les fenêtres", ActionGroup::kTools, 0,
     {VK_TAB, /*ctrl=*/true, /*alt=*/false, /*shift=*/false}, /*staff_only=*/false,
     /*imgui_windowing=*/true},
    // Écran de veille, lancé à la main. 🔴 AUCUN défaut, comme le ciblage : la
    // touche évidente (Pause) sert déjà, et poser un défaut qui vole une touche du
    // jeu est le genre de cadeau qu'on passe sa vie à retirer.
    //
    // ⚠ Le libellé est le MÊME que le titre de la sous-section des réglages, donc
    // la même clé de catalogue — c'est voulu : le joueur qui cherche « Écran de
    // veille » doit tomber sur le même mot aux deux endroits.
    {"tool_afk",         "Écran de veille",         ActionGroup::kTools,   0, {}},
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

void ApplyImGuiWindowingChord() {
  ImGuiContext* context = ImGui::GetCurrentContext();
  // Le tick peut battre avant qu'ImGui existe : il n'y a alors rien à écrire, et
  // le passage suivant reprendra l'état courant.
  if (!context) return;

  for (int i = 0; i < kActionCount; ++i) {
    if (!kActions[i].imgui_windowing) continue;
    const Binding& binding = g_bindings.items[i];
    const ImGuiKey key = VkToImGuiKey(binding.vk);
    const bool has_modifier = binding.ctrl || binding.alt || binding.shift;
    // Aucune touche, touche qu'ImGui ne connaît pas, ou combo sans modificateur :
    // on COUPE le cycleur au lieu de le laisser sur son défaut. Le dernier cas est
    // refusé en amont par l'écran de réglage, mais un yaml écrit à la main peut
    // très bien porter « Tab » tout court — et ImGui assérerait dessus.
    if (key == ImGuiKey_None || !has_modifier) {
      context->ConfigNavWindowingKeyNext = 0;
      context->ConfigNavWindowingKeyPrev = 0;
      return;
    }
    ImGuiKeyChord chord = static_cast<ImGuiKeyChord>(key);
    if (binding.ctrl)  chord |= ImGuiMod_Ctrl;
    if (binding.alt)   chord |= ImGuiMod_Alt;
    if (binding.shift) chord |= ImGuiMod_Shift;
    context->ConfigNavWindowingKeyNext = chord;
    // Maj inverse le sens — sauf quand le joueur a DÉJÀ mis Maj dans son combo :
    // « Prev » serait alors identique à « Next », et deux raccourcis identiques ne
    // s'opposent pas (ImGui verrait les deux, et « Next » gagnerait à chaque fois).
    // On renonce au sens inverse plutôt que de le laisser manger l'aller.
    context->ConfigNavWindowingKeyPrev = binding.shift ? 0 : (chord | ImGuiMod_Shift);
    return;
  }
}

bool Invoke(const char* id) {
  const Action* action = FindAction(id);
  if (!action) return false;
  // Droit relu ICI, à l'exécution : une liaison posée par un compte staff reste
  // dans le yaml quand le niveau de groupe change, et ne doit alors plus rien
  // déclencher.
  if (action->staff_only && !IsStaff()) return false;
  // Rien à déclencher : c'est ImGui qui consomme le combo. Le dispatch ne devrait
  // même pas nous appeler pour celle-là, mais un appel direct par identifiant ne
  // doit pas tomber dans la cascade ci-dessous et en ressortir au hasard.
  if (action->imgui_windowing) return false;

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
  if (std::strcmp(id, "target_nearest") == 0) {
    if (auto* target_frame = bourgeon.target_frame())
      return target_frame->TargetNearest();
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
  // Écran de veille. `StartNow` ne fait rien si la veille est déjà en cours, et
  // n'exige PAS que la mise en veille automatique soit cochée : c'est un geste
  // explicite. Le relâchement de la touche ne la terminera pas — un relâchement
  // ne réveille jamais (cf. `afk::FilterMessage`).
  if (std::strcmp(id, "tool_afk") == 0) {
    if (auto* afk = bourgeon.afk_screen()) { afk->StartNow(); return true; }
    return false;
  }
  if (std::strcmp(id, "tool_staff") == 0) {
    if (auto* staff_tools = bourgeon.staff_tools()) { staff_tools->Toggle(); return true; }
    return false;
  }
  return false;
}

}  // namespace hotkeys
