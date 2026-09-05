#pragma once

#include <string>
#include <vector>

#include "features/plugin.h"
#include "ui/gif_anim.h"

// ── TutorialWindow — « Découvrir Bourgeon » ──────────────────────────────────
//
// Une visite guidée de ce que Bourgeon ajoute au client d'origine : une page par
// nouveauté, chacune avec un GIF qui MONTRE le geste et un texte qui l'explique.
// Elle ne remplace aucune fenêtre native — le client de Gravity n'a rien de tel —
// et ne touche à rien : c'est une fenêtre ImGui de plus, qui se ferme.
//
// ── LE CONTENU EST UNE DONNÉE, PAS DU CODE ───────────────────────────────────
// Les pages vivent dans `data\bourgeon\tutorial\tutorial.yaml`, à côté de leurs
// gifs, et sont LIVRÉES PAR LE PATCHER comme le reste des données de client —
// donc DANS LE GRF chez le joueur, et en fichiers libres chez l'auteur. Les deux
// se lisent par le VFS du client (`ro::spract::ReadFile`, disque puis archives),
// jamais par `fopen` : cf. le commentaire de `kTutorialDir` dans le .cc.
// Trois raisons, dans l'ordre où elles comptent :
//   1. ajouter une page ne demande pas de recompiler la DLL — donc le tutoriel
//      suit les nouveautés au rythme où elles sortent, pas au rythme des builds ;
//   2. le même fichier peut être relu par le SITE pour publier la page web du
//      tutoriel : une seule source, deux rendus, aucune divergence à surveiller ;
//   3. traduire, c'est déposer `tutorial.en.yaml` à côté — le catalogue i18n ne
//      convient pas ici, il est fait pour des littéraux du code.
//
// ── LES GIFS VIENNENT DE NOTRE PROPRE ENREGISTREUR ───────────────────────────
// `features/fx/zone_recorder` a été écrit pour ça (cf. son en-tête : « destination,
// les tutoriels du serveur », d'où le choix d'inclure notre UI dans la capture).
// Ses défauts — 6 s, 10 images/s, 640 px de large — sont exactement le format que
// cette fenêtre accepte, et ce n'est pas une coïncidence : les plafonds de
// `kGifLimits` (tutorial_window.cc) en sont déduits.
//
// 🔴 UNE SEULE ANIMATION EN MÉMOIRE À LA FOIS. Un gif de 64 images coûte ~19 Mio
// de pixels ET autant de VRAM ; en garder trois par confort de navigation
// tripleraient la note dans un processus 32 bits. La page qu'on quitte est donc
// déchargée, et celle qu'on ouvre se charge en tâche de fond — d'où le court
// « chargement… » au changement de page, qui est un choix et non une lenteur.
//
// ── LE TEXTE EST DYNAMIQUE, ET C'EST TOUT L'INTÉRÊT DU IN-GAME ───────────────
// Une page web ne peut écrire que « appuyez sur la touche configurée ». Ici, le
// corps de page peut contenir `{touche:character_sheet}` : ce qui s'affiche est
// le raccourci RÉEL de ce joueur, relu à chaque frame — s'il l'a changé, la
// phrase change avec lui. `{perso}` donne de même le nom du personnage. Le reste
// du balisage est minimal et volontairement pauvre : `**gras**`, rien d'autre.
//
// ── OUVERTURE ────────────────────────────────────────────────────────────────
// À la PREMIÈRE entrée en jeu tant que le joueur n'a pas vu la version courante
// du contenu (`version:` du yaml), puis à la demande depuis le panneau Bourgeon.
// La page atteinte est mémorisée : rouvrir reprend là où on s'était arrêté.
//
// ⚠ L'ouverture automatique est décidée au TICK, pas dans `OnModeSwitch` : les
// réglages (donc « déjà vu ») sont chargés par MoonlightUi sur cette MÊME
// transition, et rien ne garantit l'ordre entre deux modules. Tester au tick
// suivant, c'est tester après.

class TutorialWindow : public Plugin {
 public:
  const char* name() const override { return "TutorialWindow"; }

  void OnRenderUI() override;
  void OnTick() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Ouvre la fenêtre (et relit le contenu : un auteur qui vient de corriger son
  // yaml n'a pas à relancer le jeu — le VFS regarde le disque avant le GRF, donc
  // son fichier posé dans `data\` prime sur celui de l'archive).
  void Open();
  void Close();
  // Bascule : c'est ce qu'attend un raccourci clavier, qui doit aussi refermer.
  void Toggle() { if (open_) Close(); else Open(); }
  bool IsOpen() const { return open_; }

  // Panneau de réglages, dessiné par moonlight_ui dans sa section « Interface ».
  bool DrawSettings();

  // ── Réglages persistés (bourgeon_settings.yaml, via settings_table) ────────
  // Version du contenu déjà vue par ce joueur. Le yaml de contenu porte un
  // `version:` que l'auteur incrémente quand il ajoute des pages : c'est ce qui
  // fait rouvrir le tutoriel chez ceux qui avaient déjà vu l'ancien.
  int&         seen_version()   { return seen_version_; }
  // Identifiant de la dernière page ouverte — un ID de page, jamais un index :
  // insérer une page en tête décalerait tous les index et renverrait le joueur
  // sur une page qu'il n'a pas demandée.
  std::string& last_page()      { return last_page_; }
  // Le joueur peut refuser l'ouverture automatique.
  bool&        auto_open()      { return auto_open_; }

 private:
  // Une page du tutoriel, telle qu'elle est écrite dans le yaml.
  struct Page {
    std::string              id;       // stable : clé de reprise
    std::string              title;
    std::string              gif;      // nom de fichier, vide = page sans image
    std::string              body;     // balisage minimal, cf. l'en-tête
    std::vector<std::string> bullets;
    std::string              tip;      // encadré « astuce », facultatif
  };

  bool LoadContent();          // relit le yaml ; false et `load_error_` si raté
  void ShowPage(int index);    // change de page ET (dé)charge le gif
  void DrawPage(const Page& page, float content_width);
  void DrawSummary(float width);
  // Rend le texte avec ses substitutions et son balisage. C'est ici que
  // `{touche:...}` devient le raccourci du joueur.
  void DrawRichText(const std::string& text, float wrap_width);

  bool  open_        = false;
  bool  check_auto_  = false;  // « décider de l'ouverture auto au prochain tick »
  bool  in_game_     = false;
  int   page_        = 0;
  int   content_version_ = 0;
  std::string     load_error_;   // vide si le contenu a été lu
  std::string     content_lang_; // langue du fichier chargé, pour le relire au changement
  std::vector<Page> pages_;

  ro::GifAnim anim_;
  int         anim_page_ = -1;   // page dont `anim_` porte le gif

  // Réglages persistés.
  int         seen_version_ = 0;
  std::string last_page_;
  bool        auto_open_ = true;
};
