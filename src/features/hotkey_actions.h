#pragma once

#include <cstdint>

// ── hotkey_actions : le catalogue des actions de BOURGEON liables à une touche ─
//
// Complément de `hotkey_util` (qui sait capturer un combo et détecter un conflit)
// et pendant de `ragnarok/user_hotkey` (les raccourcis du CLIENT, stockés dans
// `UserKeys.lua`). Ici : ce que Bourgeon sait faire et que le client ne connaît
// pas — donc ce pour quoi `UserKeys.lua` n'a aucune case.
//
// 🔴 POURQUOI DEUX SYSTÈMES, ET PAS UN SEUL. Le partage n'est pas arbitraire, il
// suit qui STOCKE :
//   - les actions du JEU restent dans `UserKeys.lua`. Le client les téléverse
//     lui-même par compte à chaque sortie propre (`/userconfig/save`, cf.
//     project_external_settings_re) : les déplacer chez nous ferait PERDRE une
//     synchronisation qui marche déjà, et les noms de touches layout-aware ;
//   - les actions de BOURGEON n'ont pas de place dans cette charge et relèvent
//     donc de notre stockage.
// L'écran de raccourcis, lui, montrera les deux — c'est l'affichage qui unifie,
// pas le stockage.
//
// ── LE POINT DE CONCEPTION : `native_window_id` ───────────────────────────────
// La plupart de nos fenêtres n'ont AUCUN point d'entrée public : elles naissent
// quand notre hook de `MakeWindow` intercepte la demande du client. Une action
// qui « ouvre l'inventaire » n'a donc pas à connaître `InventoryViewer` — il lui
// suffit de demander `MakeWindow(8)` et de laisser l'interception faire son
// travail. Un seul chemin d'ouverture, aucune duplication, et le jour où une
// fenêtre repasse au natif l'action continue de marcher.
// `native_window_id == 0` désigne les actions qui n'ont pas d'équivalent natif :
// celles-là sont traitées explicitement dans `Invoke`.

namespace hotkeys {

// Regroupement d'affichage, sans effet sur le comportement.
enum class ActionGroup {
  kWindows,   // fenêtres de l'interface moderne
  kOverlays,  // surcouches (barres, compteurs)
  kTools,     // outils et confort
};

// Ce que le joueur a choisi, par-dessus le catalogue. `vk == 0` = aucune touche.
// Déclaré AVANT `Action` parce que celui-ci s'en sert pour porter son défaut.
struct Binding {
  int  vk    = 0;
  bool ctrl  = false;
  bool alt   = false;
  bool shift = false;

  bool Matches(int other_vk, bool other_ctrl, bool other_alt, bool other_shift) const {
    return vk != 0 && vk == other_vk && ctrl == other_ctrl && alt == other_alt &&
           shift == other_shift;
  }
};

struct Action {
  // 🔴 Identifiant STABLE : c'est la CLÉ de persistance. Il ne se traduit pas, ne
  // se renomme pas — le renommer efface le raccourci du joueur en silence.
  const char* id;
  // Libellé source en français : c'est aussi la clé du catalogue i18n.
  const char* label_fr;
  ActionGroup group;
  // > 0 : l'action se résume à `MakeWindow(id)`, que nos hooks interceptent.
  //   0 : action sans équivalent natif, traitée dans `Invoke`.
  int native_window_id;
  // Raccourci proposé par défaut, `vk == 0` = aucun. C'est un `Binding` entier et
  // non un couple touche/modificateur : le rapport de bug est livré sur Ctrl+Alt+B,
  // donc DEUX modificateurs, ce qu'un champ unique perdait en silence.
  Binding default_binding;
  // 🔴 Réservé au STAFF : l'action ne s'exécute pas et ne s'AFFICHE pas chez un
  // joueur ordinaire. Le droit est relu à chaque fois, jamais mémorisé — le
  // niveau de groupe arrive au login et peut changer en cours de session.
  // Dernier champ, avec un défaut : les entrées existantes n'ont pas à le citer.
  bool staff_only = false;
  // 🔴 L'ACTION N'EST PAS EXÉCUTÉE PAR NOUS : son combo est REMIS À IMGUI, qui le
  // consomme lui-même (cf. `ApplyImGuiWindowingChord` plus bas). `Invoke` n'a donc
  // rien à en faire, et le dispatch ne lui réserve pas de créneau — il se contente
  // de confisquer la frappe au client, comme pour n'importe quelle autre liaison.
  // C'est un DRAPEAU et non une comparaison d'identifiant, pour que le dispatch et
  // l'écran de réglage n'aient pas à connaître la chaîne.
  bool imgui_windowing = false;
};

int           ActionCount();
const Action& ActionAt(int index);
// nullptr si l'identifiant est inconnu (raccourci d'une version antérieure).
const Action* FindAction(const char* id);
// Position dans le catalogue, -1 si l'identifiant est inconnu.
int           IndexOf(const char* id);

// ── Liaisons ─────────────────────────────────────────────────────────────────
// Ce que le joueur a choisi, par-dessus le catalogue. `vk == 0` = aucune touche.
// Persistées dans bourgeon_settings.yaml sous « bourgeon_hotkeys » (paire
// Read/WriteBourgeonHotkeys de settings_containers).
//
// 🔴 AUCUN AMORÇAGE DEPUIS `UserKeys.lua`, ET C'EST UNE DÉCISION. Recopier ici la
// touche que le client donne déjà à « Inventory » (Alt+E…) DOUBLERAIT l'action :
// le raccourci natif appelle `MakeWindow`, que nos hooks interceptent déjà pour
// ouvrir le panneau moderne. La même frappe ouvrirait donc par le chemin natif et
// basculerait par le nôtre — soit un aller-retour, c'est-à-dire un raccourci qui
// ne fait RIEN, sans rien pour l'expliquer. Nos liaisons ne servent qu'à ce que
// le client ne sait pas déclencher.
//
// 🔴 UNE ACTION À DÉFAUT S'ÉCRIT MÊME QUAND ELLE N'A PLUS DE TOUCHE. Le yaml ne
// portait que les liaisons non vides, ce qui suffisait tant que le catalogue ne
// proposait rien : absent voulait dire « rien ». Dès qu'un défaut existe, absent
// veut dire « le défaut », et l'effacement du joueur ressusciterait au
// redémarrage. `WriteBourgeonHotkeys` écrit donc aussi la ligne vide de ces
// actions-là — c'est la seule façon de dire « non, vraiment aucune touche ».
const Binding& BindingAt(int index);
void           SetBinding(int index, const Binding& binding);
// Remet TOUTES les liaisons sur ce que propose le catalogue. Point d'entrée de la
// lecture du yaml : le fichier ne contient que les écarts, jamais l'état complet.
void           ResetBindingsToDefaults();

// Exécute l'action. Renvoie false si l'identifiant est inconnu ou si le module
// concerné est absent.
//
// ⚠ NE PAS APPELER DEPUIS UNE FRAME IMGUI : les actions ouvrent des fenêtres
// natives, ce qui gèle le client en silence depuis `OnRenderUI`
// (feedback_no_native_cmd_during_imgui_frame). L'appelant diffère au tick.
//
// ⚠ Sans effet sur une action `imgui_windowing` : celle-là n'a rien à déclencher,
// c'est ImGui qui consomme son combo. `Invoke` rend false, comme pour un module
// absent.
bool Invoke(const char* id);

// ── Le cycleur de fenêtres d'ImGui, rendu au catalogue ───────────────────────
//
// 🔴 CTRL+TAB EST ACTIF DANS IMGUI MÊME SANS `NavEnableKeyboard` — le flag n'est
// pas posé ici, et pourtant le raccourci marche : `NavUpdateWindowing` le dit en
// toutes lettres (« Note: enabled even without NavEnableKeyboard! »). C'était donc
// une touche prise au client par une bibliothèque, que rien n'affichait et que
// rien ne pouvait déplacer. Elle rejoint le catalogue, sur son combo d'origine.
//
// Le combo n'est pas dispatché par nous : on le REPOUSSE dans le contexte ImGui
// (`ConfigNavWindowingKeyNext`/`Prev`, qui vivent dans `ImGuiContext`, pas dans
// `ImGuiIO`). Aucun effet si le contexte n'existe pas encore.
//
// 🔴 UN MODIFICATEUR EST OBLIGATOIRE, et ce n'est pas un goût : ImGui « tient » le
// cycle tant que le modificateur partagé par Next et Prev reste enfoncé, et il
// ASSÈRE (`IM_ASSERT(shared_mods != 0)`) si le combo n'en porte aucun. L'écran de
// réglage refuse donc ces combos-là ; ici, on se contente de couper.
//
// Appelée à CHAQUE TICK (`HotkeyDispatch::OnTick`, donc dès l'écran de login) et
// pas aux seuls moments où la liaison change : idempotent, deux écritures tous les
// ~100 ms, et surtout jamais désynchronisé — ni après la lecture du yaml, qui
// court avant qu'ImGui existe, ni après un changement dans l'écran de réglage.
void ApplyImGuiWindowingChord();

}  // namespace hotkeys
