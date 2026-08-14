#pragma once

#include <cstdint>
#include <vector>

#include "features/plugin.h"
#include "ragnarok/user_hotkey.h"

// ── HotkeySettings ───────────────────────────────────────────────────────────
//
// La table des raccourcis (« Shortcut Settings », `UIHotKeyWnd` id **156**) en
// ImGui. RE complète : docs/game_option_re.md §4.
//
// ⚠ ÉTAT : LECTURE SEULE, et c'est délibéré. Les deux ponts Lua d'écriture
// (`ChangeUserHotKey`, `SaveUserHotKeys2`) et le contrôle de collision
// (MsgStringTable 1489, « This key is already registered to [[%s]] ») ne sont pas
// encore RE'd. Plutôt que de deviner leur convention d'appel — ce qui écrirait
// des raccourcis faux dans un fichier que le serveur relit au login — le
// remappage reste confié à la fenêtre NATIVE, qu'un bouton ouvre explicitement.
// C'est le chantier suivant (docs §5.8).
//
// CE QUE CETTE VUE APPORTE DÉJÀ SUR LE NATIF :
//   - les quatre onglets, mais SANS la pagination à 36 lignes du natif (deux
//     colonnes de 18, boutons Prev/Next) : tout tient dans une liste défilante ;
//   - une RECHERCHE, que le natif n'a pas — trouver « Hotkey 3-7 » ou la commande
//     liée à une touche donnée demandait de feuilleter les pages ;
//   - les lignes non affectées sont visibles d'un coup d'œil.
//
// INTERCEPTION — même recette que le menu Échap : masquer à la naissance dans le
// hook de `MakeWindow`, DÉTRUIRE au tick. La fenêtre n'existant jamais, toute
// demande d'ouverture repasse par la fabrique.
//
// 🔴 DÉTRUIRE LA NATIVE LIBÈRE AUSSI LE CLAVIER. Tant qu'elle existe, le handler
// clavier racine (`UIWindowMgr_OnKeyDown` @0x00A47201) détourne **toute** la
// frappe vers elle et la consomme : plus aucun raccourci du jeu ne part. C'est
// voulu chez le natif (il attend la touche à affecter), mais ce serait un gel
// clavier inexplicable au-dessus d'une vue en lecture seule.
//
// Membre du groupe « Interface moderne » (SetModernInterface) : le menu Échap est
// son seul chemin d'ouverture, et il est lui-même du groupe.

class HotkeySettings : public Plugin {
 public:
  const char* name() const override { return "HotkeySettings"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Hook de `MakeWindow` pour l'id 156 : masque la native (détruite au tick) et
  // bascule notre panneau. No-op quand c'est NOUS qui ouvrons la native pour
  // laisser le joueur remapper.
  void HandleNativeCreation(void* win);

  // Ouvre le panneau depuis le menu Échap (bouton « Configuration des raccourcis »).
  void OpenFromMenu();

  bool IsOpen() const { return open_; }

  // ── Settings PERSISTANTS (bourgeon_settings.yaml, via MoonlightUi) ──────────
  // « hotkeys_imgui » : basculé en GROUPE par SetModernInterface. Défaut OFF.
  bool imgui_enabled_ = false;

 private:
  void Close();
  void RefreshRows();
  void OpenNativeForEditing();

  bool open_ = false;
  bool need_pos_ = false;
  bool show_panel_ = true;

  // Grâce sur la pile Échap : la frappe qui ouvre ne doit pas refermer aussitôt.
  // Même piège que le menu Échap, cf. docs/game_option_re.md §5.6 point 10.
  int esc_grace_frames_ = 0;

  // Vrai pendant qu'on fabrique la native nous-mêmes (remappage) : le hook de
  // création doit alors l'ignorer au lieu de basculer le panneau.
  bool routing_ = false;

  // 🔴 La native est VIVANTE et c'est voulu : le joueur est en train de remapper.
  // `OnTick` ne la détruit donc pas tant que ce drapeau tient, et le baisse quand
  // elle disparaît (OK / cancel / close) — moment où les raccourcis ont pu
  // changer, donc où il faut relire.
  bool native_editing_ = false;

  // Demande d'ouverture de la native, posée au rendu et consommée au TICK :
  // ouvrir une fenêtre native pendant une frame ImGui gèle le client en silence
  // (feedback_no_native_cmd_during_imgui_frame).
  bool pending_open_native_ = false;

  // Onglet AFFICHÉ (≠ catégorie, cf. userhotkey::CategoryForTab). La valeur
  // kTabAll fusionne les quatre en une seule liste — vue que le natif n'a pas, et
  // qui ne devient vraiment utile qu'avec la recherche.
  static constexpr int kTabAll = userhotkey::kCategoryCount;  // = 4
  // Défaut = « Tout », et il est affiché EN PREMIER : c'est la vue la plus utile
  // avec la recherche (on cherche une touche sans savoir sa catégorie), et les
  // quatre onglets du client restent à côté pour qui veut les parcourir.
  int tab_ = kTabAll;

  // Une ligne affichée, et l'onglet d'où elle vient (colonne « Onglet » du mode
  // « Tout », sans quoi deux commandes homonymes de catégories différentes
  // seraient indiscernables).
  struct Row {
    userhotkey::Binding binding;
    int tab = 0;
  };

  // Lignes de l'onglet courant — les quatre catégories quand « Tout » est actif.
  // Relues à l'ouverture, au changement d'onglet et au retour de la fenêtre
  // native ; jamais par frame, chaque ligne coûtant un appel Lua et deux
  // std::string.
  //
  // ⚠ Quand le REMAPPAGE arrivera, ce cache devra porter les QUATRE catégories en
  // permanence : le contrôle de collision doit voir les commandes des autres
  // onglets, sinon il laissera affecter une touche déjà prise ailleurs.
  std::vector<Row> rows_;
  bool rows_dirty_ = true;

  char filter_[64] = {0};

  // ── Battle Mode ────────────────────────────────────────────────────────────
  // 🔴 Le seul RÉGLAGE que porte cette fenêtre, et il ne se résume PAS à écrire
  // g_ChangeChatMode : en le DÉSACTIVANT, le natif déploie la barre de saisie du
  // chat, refait son bandeau d'onglets et purge ses liens d'objets (cinq appels
  // dont la signature n'est pas établie). Écrire le booléen seul laisserait la
  // fenêtre de chat dans un état incohérent.
  // On route donc la bascule vers le OnMsg natif (message 213), comme le menu
  // Échap route « Sélection du personnage ». -1 = rien en attente.
  int pending_battle_mode_ = -1;
  void DriveBattleMode(bool on);
};
