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
  // Raccourci proposé par défaut, 0 = aucun. `default_mod` est un VK de
  // modificateur (VK_CONTROL / VK_SHIFT / VK_MENU) ou 0.
  int default_vk;
  int default_mod;
  // 🔴 Réservé au STAFF : l'action ne s'exécute pas et ne s'AFFICHE pas chez un
  // joueur ordinaire. Le droit est relu à chaque fois, jamais mémorisé — le
  // niveau de groupe arrive au login et peut changer en cours de session.
  // Dernier champ, avec un défaut : les entrées existantes n'ont pas à le citer.
  bool staff_only = false;
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

const Binding& BindingAt(int index);
void           SetBinding(int index, const Binding& binding);

// Exécute l'action. Renvoie false si l'identifiant est inconnu ou si le module
// concerné est absent.
//
// ⚠ NE PAS APPELER DEPUIS UNE FRAME IMGUI : les actions ouvrent des fenêtres
// natives, ce qui gèle le client en silence depuis `OnRenderUI`
// (feedback_no_native_cmd_during_imgui_frame). L'appelant diffère au tick.
bool Invoke(const char* id);

}  // namespace hotkeys
