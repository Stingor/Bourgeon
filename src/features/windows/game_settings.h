#pragma once

#include <cstdint>
#include <vector>

#include "features/plugin.h"
#include "ragnarok/game_settings.h"

// ── GameSettings ─────────────────────────────────────────────────────────────
//
// La fenêtre « Game Settings » (`CUIGameSettingsUI`, id **0x271E** = 10014) en
// ImGui. RE complète : docs/game_option_re.md §3.
//
// ── CE QUI EST PORTÉ, ET CE QUI RESTE AU NATIF ──────────────────────────────
// Portés : **Effets**, **Contrôles**, **Divers** — les trois onglets que le
// client construit à partir d'une TABLE (62 lignes lues dans `GameSettings.lub`),
// donc les seuls qui se rejouent sans rien figer — et **Basique** : son groupe
// Audio, ses deux bascules `TALKTYPE`, puis le **skin** et la **priorité du
// processus**, qu'il a fallu RE séparément parce qu'aucun ne passe par la table
// d'options (docs §3.9).
//
// Reste au natif, derrière un bouton : **Graphismes** seulement (résolution,
// carte, filtrage). C'est un reset de device qu'on ne sait pas déclencher depuis
// l'extérieur, et que `Setup.exe` couvre déjà.
//
// ⛔ Le groupe **Courrier (RODEX)** du natif n'est PAS repris : il est mort sur
// Moonlight, le serveur ne mappant pas le paquet que le client émet. Détail et
// marche à suivre dans le .cc, à l'endroit où il aurait pris place.
//
// ── POURQUOI CE PANNEAU EST PLUS UTILE QUE LE NATIF ─────────────────────────
//   - une RECHERCHE, et une vue « Tout » qui fusionne les trois onglets : le
//     natif oblige à savoir d'avance dans quel onglet vit un réglage ;
//   - la DESCRIPTION de chaque option en infobulle — le natif la met dans une
//     colonne tronquée à droite, et sa fenêtre d'aide demande un second clic ;
//   - la COMMANDE SLASH équivalente affichée à côté du libellé : c'est ce que le
//     champ `Tooltip` du Lua contient réellement, et le natif ne le montre pas ;
//   - les options revenues à leur DÉFAUT sont distinguées de celles que le joueur
//     a changées, ce que le natif ne dit nulle part.
//
// ── CE QUI EST ROUTÉ VERS LE CLIENT, ET NE SERA JAMAIS RECOPIÉ ──────────────
// La liste des options, leurs libellés, leurs défauts et l'écriture des valeurs
// passent tous par `ragnarok/game_settings.h`, donc par le manager natif. Le
// panneau ne connaît aucun identifiant d'option — sauf les deux du groupe Audio,
// que le CLIENT lui-même code en dur dans son groupe (docs §3.3).
//
// ── INTERCEPTION ────────────────────────────────────────────────────────────
// Deux chemins, et le premier suffit en usage normal :
//   1. notre menu Échap ouvre CE panneau au lieu de fabriquer la native ;
//   2. filet de sécurité — si le joueur a remis le menu Échap natif, son bouton
//      « game settings » fabrique la 0x271E : le hook de `MakeWindow` la masque et
//      bascule ici, et `OnTick` la détruit.
// Vérifié par recherche d'octets : `MakeWindow(0x271E)` n'existe qu'à un seul
// endroit dans l'image, la commande 458 du menu Échap (docs §5.2).
//
// 🔴 DÉTRUIRE, PAS MASQUER : une native laissée vivante mais invisible avale un
// appui sur deux (le schéma du client est *ferme-si-existe / crée-sinon*) et garde
// le clavier. Cf. reference_native_window_toggle_router.
//
// Comme le menu Échap et la table des raccourcis, ce panneau est **hors du groupe
// « Interface moderne » et ON par défaut** : il ne remplace pas un morceau de HUD
// mais un écran de réglages, il n'a besoin de rien de moderne pour fonctionner —
// tout passe par le manager du client — et il est dessiné au-dessus de tout, là
// où le natif s'enterre sous les fenêtres du jeu.

class GameSettings : public Plugin {
 public:
  const char* name() const override { return "GameSettings"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Hook de `MakeWindow` pour l'id 0x271E : masque la native (détruite au tick) et
  // bascule notre panneau. No-op quand c'est NOUS qui l'ouvrons (bouton
  // « réglages graphiques »).
  void HandleNativeCreation(void* win);

  // Ouvre le panneau depuis le menu Échap (bouton « Réglages du jeu »).
  void OpenFromMenu();

  bool IsOpen() const { return open_; }

  // Onglet supplémentaire, que le client n'a pas : les trois listes réunies.
  // C'est lui qui donne son sens à la recherche — on cherche un réglage sans
  // savoir dans quel onglet le client l'a rangé.
  // 🔴 NÉGATIFS, et c'est une correction de bug. Ces deux-là sont NOS onglets, pas
  // ceux du client : ils doivent être hors de la numérotation du Lua, qui n'est
  // ni contiguë ni bornée (EFFECT=1, CONTROL=2, GRAPHIC=3, ETC=4 — un client plus
  // récent peut en ajouter). `kTabBasic` valait 4 et est entré en collision avec
  // `ETC` le jour où celui-ci a été lu correctement.
  static constexpr int kTabAll   = -1;
  static constexpr int kTabBasic = -2;

  // ── Settings PERSISTANTS (bourgeon_settings.yaml, via MoonlightUi) ──────────
  bool imgui_enabled_ = true;

 private:
  void Close();
  void RefreshRows();
  void OpenNativeForGraphics();

  void DrawBasicTab();
  void DrawListTab(int tab);

  bool open_ = false;
  bool need_pos_ = false;
  bool show_panel_ = true;

  // Grâce sur la pile Échap : la frappe qui ouvre ne doit pas refermer aussitôt.
  // Même piège que le menu Échap, docs/game_option_re.md §5.6 point 10.
  int esc_grace_frames_ = 0;

  // Vrai pendant qu'on fabrique la native nous-mêmes : le hook de création doit
  // alors l'ignorer au lieu de basculer le panneau.
  bool routing_ = false;

  // 🔴 La native est VIVANTE et c'est voulu : le joueur règle ses graphismes.
  // `OnTick` ne la détruit pas tant que ce drapeau tient, et le baisse quand elle
  // disparaît.
  bool native_open_ = false;

  // Demande d'ouverture de la native, posée au rendu et consommée au TICK :
  // fabriquer une fenêtre native pendant une frame ImGui gèle le client sans un
  // mot (feedback_no_native_cmd_during_imgui_frame).
  bool pending_open_native_ = false;

  // Écritures différées pour la même raison : `SetOption` traverse le handler
  // natif de l'option, qui peut recréer des fenêtres et repeindre le monde.
  struct PendingWrite {
    bool valid = false;
    int  id = 0;
    bool on = false;
    bool exec = false;  // ligne de type EXE : exécuter au lieu d'écrire
    // Option que le client ne sait PAS écrire par son id, faute de clé dans sa
    // table de drapeaux : on passe alors par le nom de sa commande de chat, seul
    // chemin qui insère (cf. gamesettings::SetOnByCommand). Littéral statique,
    // jamais alloué — sa durée de vie dépasse celle de la structure.
    const char* slash = nullptr;
  };
  // 🔴 UNE FILE, et un affichage OPTIMISTE par-dessus.
  //
  // Les écritures sont différées au tick (elles traversent un handler natif), et
  // l'affichage relit l'état à chaque frame : entre le clic et le tick, la case
  // se redessinait donc dans son ANCIEN état puis basculait — un clignotement,
  // exactement celui qu'avait le Battle Mode de la table des raccourcis.
  // `PendingValue` fait afficher ce que le joueur vient de demander tant que
  // l'écriture n'a pas eu lieu.
  //
  // Une FILE et non une seule case : deux clics rapprochés tiennent dans le même
  // tick, et la seconde demande écrasait la première sans que rien ne le dise.
  std::vector<PendingWrite> pending_writes_;

  // La valeur à AFFICHER pour cette option : celle en attente s'il y en a une,
  // sinon celle du client. La dernière demande gagne — c'est le dernier clic.
  bool PendingValue(int id, bool actual) const;

  bool pending_reset_ = false;
  bool confirm_reset_ = false;

  // Changement de skin demandé, appliqué au TICK. Il ne rejoint pas la file
  // ci-dessus : ce n'est pas une bascule d'option, et surtout il fait bien plus
  // qu'écrire un drapeau — le client PURGE toutes ses textures .bmp, ce qui tue
  // aussi celles que nos propres caches gardent.
  //
  // 🔴 Différé pour DEUX raisons cumulées, dont chacune suffirait : c'est une
  // commande native (freeze muet si elle tombe dans une frame ImGui), et elle
  // relâche des textures que la frame en cours est en train de dessiner
  // (feedback_texture_release_defer_frame).
  static constexpr int kNoPendingSkin = -2;  // ≠ kSkinDefault, qui vaut -1
  int pending_skin_ = kNoPendingSkin;

  // Une ligne affichée. On RECOPIE la description du client au lieu de pointer
  // dedans : le vecteur source peut être réalloué, et une infobulle qui survit à
  // la frame lirait alors de la mémoire libérée.
  struct Row {
    gamesettings::Option option;
    bool value = false;  // relu à chaque frame, il est le sujet de l'écran
  };

  // Lignes de la table, relues à l'ouverture et après chaque écriture — jamais
  // par frame : chaque ligne coûte trois conversions de chaîne.
  //
  // 🔴 Relues à l'OUVERTURE et pas seulement au tick : la première frame doit
  // montrer l'état vrai, sinon le panneau s'affiche avec les valeurs de la fois
  // précédente et se corrige sous les yeux du joueur (docs §5.6 point 11).
  std::vector<Row> rows_;
  bool rows_dirty_ = true;

  int  tab_ = kTabAll;
  char filter_[64] = {0};

  // Cache l'option dont l'infobulle est ouverte, pour ne pas rechercher sa
  // description à chaque frame de survol.
  int hovered_id_ = 0;
};
