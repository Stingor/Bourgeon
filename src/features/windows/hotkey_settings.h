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
// CE QUE CETTE VUE APPORTE SUR LE NATIF :
//   - les quatre onglets, mais SANS la pagination à 36 lignes du natif (deux
//     colonnes de 18, boutons Prev/Next) : tout tient dans une liste défilante ;
//   - une RECHERCHE, que le natif n'a pas — trouver « Hotkey 3-7 » ou la commande
//     liée à une touche donnée demandait de feuilleter les pages ;
//   - les lignes non affectées sont visibles d'un coup d'œil ;
//   - un onglet « Bourgeon » et une vue « Tout » qui réunissent les commandes du
//     JEU et les actions de l'interface moderne, que le client ne connaît pas.
//
// ── L'ÉCRITURE ──────────────────────────────────────────────────────────────
// Elle passe par `userhotkey::WriteBinding` + `Save` pour les commandes du
// client, et par `hotkeys::SetBinding` pour les nôtres. L'écran unifie, le
// STOCKAGE reste séparé — cf. features/hotkey_actions.h.
//
// 🔴 ON NE RAPPELLE PAS `UIHotKeyWnd_ValidateKeyCombo` : il exige une instance de
// la fenêtre native, lit les globales du handler clavier et ouvre une modale
// bloquante. Le contrôle de collision se fait donc chez nous
// (`hotkeys::Conflict`), en rejouant sa règle : les catégories 0 et 3 sont deux
// pages de la même barre et ont le droit de partager une touche.
//
// ⚠ DIVERGENCE ASSUMÉE : sur collision, le natif propose de VOLER la touche à son
// propriétaire (MsgStringTable 1489) ; nous refusons en le nommant. Voler efface
// une affectation que le joueur ne regardait pas — le reste de Bourgeon (saut,
// presets d'équipement) refuse déjà, et un écran de raccourcis n'est pas
// l'endroit où inventer une seconde convention.
//
// ⚠ CE QUE LE NATIF SAIT ENCORE FAIRE ET PAS NOUS : remettre les touches par
// DÉFAUT (son bouton Reset, `StageDefaultBindings`). D'où le bouton qui l'ouvre.
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

  // Onglets qui n'existent pas chez le client : « Tout » fusionne tout, et
  // « Bourgeon » porte nos propres actions. Publics parce que le rendu des
  // libellés d'onglet vit dans une fonction libre du .cc.
  static constexpr int kTabAll      = userhotkey::kCategoryCount;      // = 4
  static constexpr int kTabBourgeon = userhotkey::kCategoryCount + 1;  // = 5

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
  // (feedback_no_native_cmd_during_imgui_frame). Le bouton ne sert plus qu'à
  // atteindre le [Reset] du client, seule fonction qu'on ne sache pas rejouer.
  bool pending_open_native_ = false;

  // Onglet AFFICHÉ (≠ catégorie, cf. userhotkey::CategoryForTab).
  // Défaut = « Tout », et il est affiché EN PREMIER : c'est la vue la plus utile
  // avec la recherche (on cherche une touche sans savoir sa catégorie), et les
  // onglets du client restent à côté pour qui veut les parcourir.
  int tab_ = kTabAll;

  // Une ligne affichée, et l'onglet d'où elle vient (colonne « Onglet » du mode
  // « Tout », sans quoi deux commandes homonymes de catégories différentes
  // seraient indiscernables).
  //
  // Les deux mondes partagent la même struct de rendu : une action Bourgeon
  // remplit `binding.label` / `binding.key_name` comme le ferait le client. Une
  // seule boucle de dessin, un seul chemin de recherche.
  struct Row {
    userhotkey::Binding binding;
    int tab = 0;
    // Commande du CLIENT : sa catégorie Lua. -1 pour une action Bourgeon.
    int category = -1;
    // Action BOURGEON : son index dans hotkey_actions. -1 pour le client.
    int action_index = -1;
    // La touche affichée vient de la table par DÉFAUT du client, faute de
    // surcharge dans `UserKeys.lua` — cas le plus courant, le fichier ne portant
    // que les surcharges. Elle agit exactement comme une autre : le marquer sert
    // à dire au joueur qu'il n'a rien choisi là, pas que la ligne est inerte.
    bool from_default = false;
  };

  // Lignes de l'onglet courant — tout quand « Tout » est actif. Relues à
  // l'ouverture, au changement d'onglet et après chaque écriture ; jamais par
  // frame, chaque ligne du client coûtant un appel Lua et deux std::string.
  std::vector<Row> rows_;
  bool rows_dirty_ = true;

  // ── Capture d'une touche ───────────────────────────────────────────────────
  // La ligne en cours de capture est désignée par ce qui l'IDENTIFIE et non par
  // son rang : `rows_` est reconstruit après chaque écriture, et un index y
  // survivrait à la ligne qu'il désignait.
  bool capturing_ = false;
  int  capture_category_ = -1;  // commande du client
  int  capture_command_  = -1;
  int  capture_action_   = -1;  // action Bourgeon
  char capture_error_[224] = {0};

  // Menu contextuel de la cellule des touches : effacer, ou remettre la touche
  // que le client donne par défaut à CETTE commande — le [Reset] pris ligne à
  // ligne, là où le natif ne sait que réinitialiser les quatre catégories.
  // Renvoie true si une écriture a été demandée.
  bool DrawRowMenu(const Row& row);

  bool IsCapturing(const Row& row) const;
  void BeginCapture(const Row& row);
  void CancelCapture();
  // Traite la frappe de la frame pour la ligne en capture. Renvoie true si une
  // écriture a été demandée (donc si `rows_` doit être relu).
  bool RunCapture(const Row& row);

  // ── Écriture DIFFÉRÉE des commandes du client ──────────────────────────────
  // `ChangeUserHotKey` et `SaveUserHotKeys2` sont des appels Lua du client :
  // comme toute commande native, ils ne se lancent pas depuis une frame ImGui
  // (feedback_no_native_cmd_during_imgui_frame). Posée au rendu, consommée au
  // tick. Les liaisons Bourgeon, elles, sont du C++ pur et s'écrivent sur place.
  struct PendingWrite {
    bool valid = false;
    int  category = -1;
    int  command_index = -1;
    int  key1 = 0;  // touche principale, VK Windows ; 0 = effacer
    int  key2 = 0;  // modificateur, 0 s'il n'y en a pas
    char label[128] = {0};  // le champ EXE, repassé tel quel au Lua
  };
  PendingWrite pending_write_;

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
  // Le retour au chat de `/bm`, que la commande 213 n'émet pas d'elle-même.
  void SayBattleMode(bool on);

  // ── Réinitialisation aux touches du client ─────────────────────────────────
  // Le [Reset] du natif, rejoué par ses propres commandes : 363 met les défauts
  // en attente, 184 les commet. Différé au tick pour la même raison que la
  // bascule ci-dessus, et confirmé d'abord — il efface TOUS les raccourcis du
  // joueur, et le client, lui, ne demande rien.
  bool confirm_reset_ = false;
  bool pending_reset_ = false;
  void DriveResetDefaults();
};
