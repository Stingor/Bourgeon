#pragma once

#include <cstdint>

#include "features/plugin.h"

// ── GameMenu ─────────────────────────────────────────────────────────────────
//
// Le menu Échap (« Game Options », `UIEscOptionWnd` id **155**) en ImGui, en
// REMPLACEMENT de la native. RE complète : docs/game_option_re.md §2.
//
// 🔴 POURQUOI CE MODULE EST LE PREMIER DU CHANTIER. Recherche d'octets sur toute
// l'image : le seul `MakeWindow(0x271E)` du client est la commande 458 de CE
// menu, et le seul `MakeWindow(0x9C)` sa commande 370. Ni la barre d'icônes ni
// `DispatchHotkeyBehavior` ne les référencent. Tuer ce menu coupe donc l'accès à
// Game Settings ET à Shortcut Settings : les deux modules suivants n'auront aucun
// détour à poser sur elles.
//
// INTERCEPTION — un seul point, et il suffit. `UIEscOptionWnd_ctor` n'a qu'un
// appelant : `MakeWindow` case 155 (0x00A3F24D). Comme on DÉTRUIT la fenêtre au
// tick, elle n'existe jamais, donc toute demande d'ouverture (touche Échap comme
// bouton 193 de la barre d'icônes) repasse forcément par la fabrique — où
// `HandleNativeCreation` bascule notre panneau. C'est le raisonnement de
// reference_native_window_toggle_router, et il tient ici sans réserve.
//
// ⚠ Ne PAS masquer sans détruire : le case 155 réutilise `mgr+0x408` s'il est non
// nul, donc une native masquée mais vivante ferait rater un appui sur deux — et
// une native masquée garde le clavier (Entrée/Espace cliquent son bouton par
// défaut, ici « Character Select »).
//
// LES TROIS DISPOSITIONS (docs §2.4, vérifiées en jeu) — relues à chaque
// ouverture, jamais mises en cache : le joueur peut mourir panneau ouvert.
//   vivant                      -> 5 boutons
//   mort                        -> Retour au point de sauvegarde + Retour au jeu
//   mort + jeton de résurrection-> Résurrection en plus, en tête
//
// LES LIBELLÉS SONT À NOUS, et c'est un gain net : ceux du natif sont PEINTS dans
// `esc_XXa/b/c.bmp` (aucun `SetName`, aucune MsgStringTable), donc intraduisibles
// sans redessiner des bitmaps. En ImGui ils passent par `i18n::Tr` — le menu
// Échap devient français gratuitement. Seule exception, la confirmation du retour
// au point de sauvegarde : elle vient de la MsgStringTable (id 1548), parce que
// c'est un message du CLIENT et que la règle du projet est d'afficher son libellé
// exact.
//
// LES ACTIONS SONT DIFFÉRÉES AU TICK, jamais exécutées dans la frame ImGui : elles
// envoient des paquets et détruisent des fenêtres natives, ce qui gèle le client
// en silence depuis un `OnRenderUI` (feedback_no_native_cmd_during_imgui_frame).
//
// « Character Select » est la seule action ROUTÉE VERS LE NATIF plutôt que
// réimplémentée : son branchement fait deux purges locales
// (`sub_5C4DA0(*0x012517A8)`, `sub_5AEB80(*0x01251750)`) dont la nature n'est pas
// établie. On crée donc la native le temps d'un `OnMsg(6, 371)` et on la détruit —
// le handler d'origine fait tout, purges et fermetures de 155/164/269 comprises.
// Réimplémenter en devinant la convention d'appel de deux fonctions inconnues
// serait un risque de crash certain contre un risque d'état résiduel supposé.
//
// Membre du groupe « Interface moderne » (SetModernInterface, moonlight_ui.h) :
// pas de case isolée. Un menu Échap moderne qui ouvrirait des sous-fenêtres
// natives — ou l'inverse — serait exactement le mixe que le groupe supprime.

class GameMenu : public Plugin {
 public:
  const char* name() const override { return "GameMenu"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Appelé par le hook de `MakeWindow` (window_pos_tweaks) pour l'id 155 : masque
  // la native — que `OnTick` détruira — et BASCULE notre panneau, exactement comme
  // le faisait `ToggleWindowById`. No-op quand c'est NOUS qui pilotons la native
  // (routage de « Character Select »).
  void HandleNativeCreation(void* win);

  // Ouvre / referme le panneau depuis notre propre UI (barre d'icônes Bourgeon,
  // raccourci maison). Même sémantique de bascule que la touche Échap.
  void ToggleFromUi();

  bool IsOpen() const { return open_; }

  // ── Settings PERSISTANTS (bourgeon_settings.yaml, via MoonlightUi) ──────────
  // 🔴 « escmenu_imgui » : ON PAR DÉFAUT, et HORS du groupe « Interface moderne ».
  // Ce menu ne remplace pas un morceau de HUD, il remplace un MENU — et le natif,
  // simple `UIWindow`, se retrouve enterré sous les fenêtres du jeu. Le nôtre est
  // dessiné au-dessus de tout, ce qui vaut aussi pour qui joue en interface
  // native. Il ne dépend d'ailleurs d'aucun autre morceau moderne : toutes ses
  // commandes partent par le `OnMsg` du client.
  bool imgui_enabled_ = true;

 private:
  // Ce que le joueur a cliqué, à exécuter au TICK. Les valeurs ne sont PAS les ids
  // de commande natifs : ceux-ci sont locaux à la fenêtre native et n'ont pas à
  // fuiter ici (185 vaut « Retour au jeu » dans le menu Échap et « cancel » dans
  // Shortcut Settings — cf. docs §0).
  enum class Action {
    kNone,
    kCharSelect,     // -> OnMsg natif 371 (purges + CZ_RESTART type 1 + 3 fermetures)
    kSavePoint,      // -> CMode::SendMsg(25, 0) = CZ_RESTART type 0
    kResurrect,      // -> CMode::SendMsg(250)   = CZ_STANDING_RESURRECTION 0x0292
    kExitToWindows,  // -> CMode::SendMsg(88)    -> 128 = CZ_REQ_DISCONNECT 0x018A
    // Ouvertures de fenêtres NATIVES. Elles passent par la file d'actions et non
    // par un `MakeWindow` en pleine frame : fabriquer une native pendant le rendu
    // ImGui gèle le client sans un mot (feedback_no_native_cmd_during_imgui_frame).
    kOpenChatMacros,        // fenêtre 86 — celle d'Alt+M
    kOpenGameSettings,  // fenêtre 0x271E, en attendant notre panneau
    kOpenHotkeyNative,  // fenêtre 156, quand notre table est désactivée seule
  };

  // La disposition, telle que le ctor natif la calcule (docs §2.4).
  enum class Layout { kAlive, kDeadNoToken, kDeadWithToken };

  void Close();
  void RunPendingAction();
  void DriveNativeCommand(int native_cmd);

  // Relit l'état « mort » et le jeton, et en déduit `layout_`.
  //
  // 🔴 APPELÉE À L'OUVERTURE, et pas seulement au tick. Sinon la PREMIÈRE frame
  // dessine la disposition périmée — celle de la fois précédente — et le tick la
  // corrige jusqu'à 100 ms plus tard : le joueur voit le menu normal se
  // transformer sous ses yeux en menu de mort (constaté en jeu). Le titre en
  // découlant, il sautait aussi.
  //
  // Pas d'appel par FRAME en revanche : `Own_HasResurrectionToken` fait cinq
  // recherches d'inventaire avec construction de std::string. Ouverture + tick
  // suffisent — mourir panneau ouvert reste reflété en ≤ 100 ms, et le panneau est
  // alors déjà à l'écran, donc rien ne « saute » à l'apparition.
  void RefreshLayout();

  bool open_ = false;
  // Dernière transition de carte vue par `OnTick` (cf. Bourgeon::MapLoadEpoch).
  // Un écart signifie qu'un warp / @load a eu lieu : l'écran se referme avec le
  // HUD natif, comme le fait la fenêtre du client.
  uint32_t map_epoch_ = 0;
  bool need_pos_ = false;
  bool show_panel_ = true;  // transitoire : détection du clic sur le X

  // Confirmation du retour au point de sauvegarde. Popup ImGui, PAS la modale
  // native : celle-ci bloque le fil principal (feedback_no_blocking_dialog_main_thread).
  bool confirm_savepoint_ = false;

  // Vrai pendant `DriveNativeCommand` : dit au hook de création d'ignorer la
  // native qu'on vient de fabriquer nous-mêmes, sinon elle basculerait le panneau.
  bool routing_ = false;

  Action pending_ = Action::kNone;
  Layout layout_ = Layout::kAlive;

  // 🔴 Frames de grâce sur la pile Échap centralisée, sans quoi ce panneau
  // s'ouvrait et se refermait dans la même frappe (constaté en jeu).
  //
  // La touche Échap est vue DEUX FOIS sur la frappe d'ouverture : le jeu la traite
  // (son handler @0x00A44C40 fait close-si-existe / crée-sinon -> notre hook ouvre
  // le panneau) ET ImGui la reçoit, si bien que `ro::ProcessEscapeStack` ferme
  // aussitôt la fenêtre du dessus — la nôtre, qui vient de naître. Le WndProc
  // n'avale Échap pour le jeu que si une fenêtre RO est DÉJÀ ouverte : à
  // l'ouverture, personne ne l'avale.
  //
  // On neutralise donc la pile pendant les deux premières frames du panneau
  // (`ro::SuppressEscapeStack` par frame). Ensuite l'enchaînement est cohérent de
  // bout en bout : panneau ouvert => le WndProc avale Échap pour le jeu, ImGui le
  // reçoit, la pile ferme le panneau, et le natif n'est jamais recréé.
  //
  // Deux frames et non une : selon l'instant où l'entrée est consommée, `IsKeyPressed`
  // peut ne devenir vrai qu'à la frame suivante. À ~30 ms, aucune frappe humaine ne
  // se glisse dans cet intervalle.
  int esc_grace_frames_ = 0;
};
