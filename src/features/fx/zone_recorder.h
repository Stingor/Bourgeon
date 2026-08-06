#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "features/plugin.h"

// ── Enregistreur de zone → GIF (staff) ───────────────────────────────────────
// L'outil de capture de Windows, en jeu : on trace une zone à la souris une fois,
// elle est mémorisée, et une touche enregistre ce qui s'y passe pendant quelques
// secondes. Le résultat est un GIF animé dans <jeu>\screenshot, destiné aux
// tutoriels qui présentent les fonctionnalités du serveur.
//
// 🔴 LA CAPTURE CONTIENT NOTRE INTERFACE. Elle est prise dans Present, APRÈS le
// rendu de l'overlay ImGui (cf. D3D9_SetPostFrameCallback) : c'est tout l'intérêt
// pour un tutoriel — les fenêtres Bourgeon sont le sujet. Conséquence directe et
// non négociable : tout ce que CE module dessine pendant l'enregistrement (cadre,
// témoin, décompte) atterrirait dans l'image. Rien n'est donc dessiné à
// l'intérieur de la zone ; le cadre la borde par l'extérieur et le témoin cherche
// une marge libre autour (PlaceBadge). Sans marge, on n'affiche rien plutôt que
// de polluer la capture.
//
// 🔴 MÉMOIRE PRÉ-ALLOUÉE. Le client est un processus 32 bits déjà chargé : une
// allocation qui échoue au milieu d'un enregistrement laisserait un GIF tronqué,
// au mieux. Tous les tampons d'images sont donc réservés AVANT le départ, et un
// échec se dit tout de suite. Ils sont alloués image par image, pas en un bloc
// unique de plusieurs dizaines de mégaoctets : l'espace d'adressage d'un client RO
// en pleine partie est trop fragmenté pour un tel bloc.
//
// L'encodage part sur un THREAD dédié (quantification + LZW de centaines de
// milliers de pixels : plusieurs secondes, qui gèleraient le rendu). Le thread ne
// touche que les images qu'on lui a cédées ; l'état reste à kEncoding tant qu'il
// n'a pas rendu la main, ce qui interdit tout second enregistrement entre-temps.
//
// DX9 uniquement : sous le proxy DX7 il n'y a pas de backbuffer à relire, et le
// panneau le dit au lieu de laisser une touche muette.
class ZoneRecorder : public Plugin {
 public:
  ZoneRecorder();
  ~ZoneRecorder() override;

  const char* name() const override { return "ZoneRecorder"; }

  void OnKeyDown(unsigned long vkey, int new_key, int accurate_key) override;
  void OnRenderUI() override;
  void OnTick() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Contrôles du panneau « Staff Tools » (pas de fenêtre propre).
  void DrawSettings();

  // ── Accès pour la persistance (moonlight_ui) ──────────────────────────────
  int& zone_x()   { return zone_x_; }
  int& zone_y()   { return zone_y_; }
  int& zone_w()   { return zone_w_; }
  int& zone_h()   { return zone_h_; }
  int& fps()      { return fps_; }
  int& duration_s()    { return duration_s_; }
  int& max_width()     { return max_width_; }
  int& start_delay_s() { return start_delay_s_; }
  int&  key_vk()    { return key_vk_; }
  bool& key_ctrl()  { return key_ctrl_; }
  bool& key_alt()   { return key_alt_; }
  bool& key_shift() { return key_shift_; }

  // Appelé depuis le hook Present (fil de rendu), une fois l'image finie.
  void OnPostFrame();

 private:
  enum class State {
    kIdle,       // rien en cours
    kSelecting,  // tracé de la zone à la souris
    kCountdown,  // zone armée, on laisse le temps de se placer
    kRecording,  // captures en cours
    kEncoding,   // le thread écrit le GIF
  };

  // Transitions d'état, hors du fil de rendu (rendu ET tick y passent : un
  // changement de carte suspend OnRenderUI, pas OnTick).
  void PumpState();

  bool ArmRecording();   // valide, pré-alloue, passe au décompte. false = refusé
  void BeginRecording();
  void FinishRecording();  // fin des captures -> lance l'encodage
  void CancelAll();        // retour à kIdle en abandonnant ce qui est en cours

  void DrawSelectionOverlay();
  void DrawRecordingOverlay();

  // Taille du GIF : la zone, réduite à `max_width_` en gardant ses proportions.
  void ComputeOutputSize(int* out_w, int* out_h) const;
  size_t EstimatedBytes() const;
  bool   IsZoneValid() const { return zone_w_ >= 32 && zone_h_ >= 32; }

  // ── Réglages persistés ────────────────────────────────────────────────────
  int zone_x_ = 0, zone_y_ = 0, zone_w_ = 0, zone_h_ = 0;  // pixels écran
  int fps_           = 10;   // 5..20
  int duration_s_    = 6;    // 1..15
  int max_width_     = 640;  // largeur max du GIF (0 = taille réelle)
  int start_delay_s_ = 3;    // décompte avant le départ (0..5)

  int  key_vk_    = 0;  // 0 = aucun raccourci
  bool key_ctrl_  = false;
  bool key_alt_   = false;
  bool key_shift_ = false;

  // ── Capture de la touche dans le panneau ──────────────────────────────────
  bool        key_capturing_ = false;
  std::string key_conflict_msg_;

  // ── Tracé de la zone ──────────────────────────────────────────────────────
  bool  dragging_ = false;
  float drag_ax_ = 0.0f, drag_ay_ = 0.0f;  // coin d'origine
  float drag_bx_ = 0.0f, drag_by_ = 0.0f;  // coin courant
  std::string select_hint_;                // « zone trop petite », etc.

  // ── Enregistrement ────────────────────────────────────────────────────────
  State state_ = State::kIdle;
  std::vector<std::vector<uint32_t>> frames_;  // réservées avant le départ
  int           frame_count_       = 0;
  int           capture_w_         = 0;
  int           capture_h_         = 0;
  unsigned      interval_ms_       = 100;
  unsigned long next_capture_tick_ = 0;
  unsigned long record_start_tick_ = 0;
  unsigned long countdown_end_tick_ = 0;

  // ── Encodage ──────────────────────────────────────────────────────────────
  std::thread       encoder_thread_;
  std::atomic<bool> encode_finished_{false};
  std::atomic<bool> encode_ok_{false};
  std::string       encode_path_;   // lu par le thread, figé avant son départ
  int               encode_frames_ = 0;

  // ── Dernier résultat (affiché dans le panneau) ────────────────────────────
  std::string   last_status_;
  std::string   last_saved_path_;
  unsigned long last_status_tick_ = 0;
};
