#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "features/plugin.h"
#include "ragnarok/social.h"
#include "ui/hud_frame.h"

// ── PartyFrames ──────────────────────────────────────────────────────────────
//
// Le HUD de groupe, en GRILLE de tuiles — la forme des « raid frames » (WoW,
// Grid), pas la liste verticale du client.
//
// Ce n'est PAS un portage de `UIMiniPartyWnd` (0x12d) : cette fenêtre native
// empile des barres de 174×52 et laisse détacher chaque membre à la souris. On
// vise autre chose — un bloc dense qu'on lit d'un coup d'œil, où chaque tuile
// DIT l'état de son membre par sa couleur. À ce titre le module vit dans
// `overlays/` et non `windows/` : il ne remplace pas une fenêtre, il occupe une
// place que le client ne couvre pas (même raisonnement que `skill_bar`).
//
// Le HUD natif est simplement MASQUÉ tant que celui-ci est actif — pas détruit :
// il ne porte aucun bouton par défaut, donc aucun piège clavier (contrairement
// aux popups d'invitation), et le laisser vivant rend la bascule instantanée.
//
// SOURCE DES DONNÉES : `rag::social::ReadParty`, la même que la fenêtre
// Amis/Groupe — le manager de session, relu à chaque frame.
//
// 🔴 Les PV ne sont connus que des membres dont l'ACTEUR est chargé (cf.
// docs/party_friend_re.md §3). Une tuile « hors de portée » le DIT, elle
// n'affiche pas une barre vide qui se lirait comme « ce joueur est à zéro ».
//
// ⚠ PAS ENCORE : le clic pour CIBLER, qui est la moitié de l'intérêt d'un raid
// frame. Le chemin natif est piégeux (la cible vit dans `CGameMode+0xF4`, gatée
// par `+0x28`, et écrire `+0xF8` coupe la marche au clic maintenu — cf. la
// mémoire project_target_system_re). Il mérite sa propre passe de RE.

class PartyFrames : public Plugin {
 public:
  PartyFrames();  // enregistre l'opcode de réponse (SP des membres)

  const char* name() const override { return "PartyFrames"; }

  void OnTick() override;      // masque le HUD natif, interroge le SP
  void OnRenderUI() override;  // dessine la grille

  // ── Le SP : il faut le DEMANDER au serveur ────────────────────────────────
  //
  // 🔴 Aucun paquet du protocole ne transporte le SP d'un tiers, et le client ne
  // remplit JAMAIS les champs SP de ses jauges (+0xA8/+0xAC restent à zéro).
  // C'est le trou que le couple custom CZ 0x0F29 -> ZC 0x0F2A comble déjà pour la
  // fenêtre de cible ; sa gate serveur autorise explicitement les membres du MÊME
  // GROUPE, donc rien à ajouter côté Moonlight.
  //
  // ⚠ Même limite de portée que la cible : le serveur répond `status = 1` pour un
  // membre sur une autre carte ou hors d'AREA_SIZE. Ce paquet ne comble donc PAS
  // le trou « hors de portée ».
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // ── Settings PERSISTANTS (bourgeon_settings.yaml, via MoonlightUi) ────────
  bool enabled_ = false;
  bool locked_  = false;

  // Géométrie. La taille du CADRE est CALCULÉE à partir de ces trois-là : c'est
  // la tuile qui commande, pas l'inverse — un raid frame se règle en « telle
  // taille de case », pas en tirant un coin jusqu'à tomber juste.
  int columns_ = 1;    // 1 = liste (proche du natif), 2-3 = grille compacte
  int tile_w_  = 180;  // largeur d'une tuile, en pixels d'interface
  int tile_h_  = 34;   // hauteur d'une tuile
  int gap_     = 2;    // espace entre deux tuiles

  // Contenu d'une tuile.
  bool show_self_     = true;
  bool show_offline_  = true;
  bool show_job_icon_ = true;   // icône de classe du client
  bool show_level_    = false;  // « Lv.N » devant le nom
  // Comment les PV s'écrivent sous le nom. Une grille très dense n'a la place
  // pour aucun chiffre, une grille large peut tout porter — et le pourcentage
  // seul est souvent le plus lisible en combat, où l'on compare des membres
  // entre eux plutôt que de lire des totaux.
  enum HpText { kHpTextNone = 0, kHpTextNumbers, kHpTextPercent, kHpTextBoth };
  int hp_text_mode_ = kHpTextNumbers;

  // Taille du texte des tuiles, en pixels d'interface. Indépendante de la police
  // du reste de l'UI : une grille se lit en périphérie de l'écran, pas au centre.
  int text_px_ = 13;
  bool show_sp_       = true;   // barre de SP en bas de tuile
  int  sp_bar_h_      = 6;      // sa hauteur, en pixels d'interface

  // ── Couleurs (RGBA 0..1, persistées en hex ARGB) ──────────────────────────
  //
  // 🔴 `col_frame_bg_` existe parce que sans lui la grille flottait SUR le jeu
  // sans rien pour la détacher du décor : tuiles sombres sur fond de carte
  // sombre, on ne distinguait plus rien. Un fond de cadre, même discret, rend le
  // bloc lisible partout.
  float col_frame_bg_[4] = {0.05f, 0.05f, 0.07f, 0.72f};
  float col_tile_bg_[4]  = {0.09f, 0.09f, 0.11f, 0.90f};
  float col_hp_high_[4]  = {0.25f, 0.78f, 0.28f, 1.0f};
  float col_hp_mid_[4]   = {0.84f, 0.75f, 0.24f, 1.0f};
  float col_hp_low_[4]   = {0.81f, 0.26f, 0.22f, 1.0f};
  float col_sp_[4]       = {0.27f, 0.51f, 0.86f, 1.0f};
  float col_text_[4]     = {0.94f, 0.94f, 0.94f, 1.0f};
  float col_me_[4]       = {1.0f,  0.85f, 0.47f, 0.86f};  // liseré de MA tuile

  // Seuils de bascule de couleur, en pourcentage de PV.
  int hp_mid_pct_ = 55;
  int hp_low_pct_ = 25;

  // Accesseurs pour la table de réglages (MLUI_FIELD veut une lvalue).
  ro::HudRect& rect() { return rect_; }
  int& columns() { return columns_; }
  int& tile_w()  { return tile_w_; }
  int& tile_h()  { return tile_h_; }
  int& gap()     { return gap_; }
  int& sp_bar_h() { return sp_bar_h_; }
  int& hp_text_mode() { return hp_text_mode_; }
  int& text_px()      { return text_px_; }
  int& hp_mid_pct() { return hp_mid_pct_; }
  int& hp_low_pct() { return hp_low_pct_; }

 private:
  void DrawTile(const rag::social::Entry& member, ImVec2 p0, ImVec2 p1,
                bool is_me);
  void SyncNativeHud();
  void PollVitals();

  // Le SP d'un membre, tel que le serveur l'a renvoyé. Clé = GID.
  // `stamp` sert à oublier une valeur qui n'a pas été rafraîchie : un membre qui
  // s'éloigne cesse de répondre, et afficher son dernier SP connu indéfiniment
  // serait pire que ne rien afficher.
  struct Vitals {
    int      sp    = 0;
    int      maxsp = 0;
    unsigned stamp = 0;  // GetTickCount de la dernière réponse
  };
  std::unordered_map<uint32_t, Vitals> vitals_;
  size_t   poll_cursor_  = 0;  // interrogation en rotation
  unsigned last_poll_ms_ = 0;

  ro::HudRect rect_{40, 200, 190, 120};
  std::vector<rag::social::Entry> members_;
  // Dernier état appliqué au HUD natif : évite de rejouer SetVisible à chaque
  // tick (appel natif) alors que rien n'a changé.
  int  native_hidden_ = -1;
  bool geometry_dirty_ = false;
};
