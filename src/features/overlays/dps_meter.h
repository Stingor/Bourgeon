#pragma once

#include <Windows.h>
#include <array>
#include <deque>
#include <cstdint>
#include <string>
#include <vector>

#include "features/systems/bourgeon_opcodes.h"
#include "features/plugin.h"

class DpsMeter : public Plugin {
 public:
  DpsMeter();

  const char* name() const override { return "DPS Meter"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void OnRenderUI() override;

 public:
  // ── Réglages ──────────────────────────────────────────────────────────────
  // Publics parce que la table de persistance de MoonlightUi décrit chaque
  // réglage par l'ADRESSE de sa valeur. Le PANNEAU, lui, est ici : DrawSettings.
  bool visible_                 = true;
  bool locked_                  = false;  // fige la fenêtre + clics traversants
  float text_color_[4] = {1.00f, 0.80f, 0.20f, 1.0f};  // texte de la valeur DPS
  float plot_color_[4] = {1.00f, 0.80f, 0.20f, 1.0f};  // courbe du graphique
  float bg_alpha_      = 0.90f;                        // opacité du fond
  bool show_ground_dmg_in_chat_ = true;   // pousse les coups de sorts de zone au chat
  int  slot_ms_                 = 200;    // ms par colonne du graphique (50–2000)
  int  dps_window_secs_         = 10;     // fenêtre glissante du DPS, en s (1–30)
  int  combat_timeout_secs_     = 5;      // s sans dégâts avant de quitter le combat (1–15)

  void ResetHistory();  // à appeler après un changement de slot_ms_

  // Section « DPS Meter » du panneau Moonlight. Renvoie true si un réglage a
  // changé — l'appelant décide alors de sauvegarder.
  bool DrawSettings();

 private:
  // 🔴 Le fil RÉSEAU ne fait que copier les octets ; le décodage est rejoué en
  // phase d'entrée. C'est le module le plus exposé de tous : sous une compétence de
  // zone, chaque coup est un paquet, et `events_` (deque) comme `chat_queue_`
  // étaient écrites pendant que le rendu les parcourait. Cf. features/net_inbox.h.
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  static constexpr uint16_t kOpcodeNotifyAct    = 0x08c8;
  static constexpr uint16_t kOpcodeNotifySkill  = 0x01de;
  static constexpr uint16_t kOpcodeNotifySkill2 = 0x0115;
  static constexpr uint16_t kOpcodeSkillUnitDmg = bopcodes::kSkillDmg;  // ZC_BOURGEON_SKILL_DMG

  // Maximum ring-buffer size (slots). Actual history = slot_ms_ * kPlotSlots ms.
  static constexpr int kPlotSlots = 150;

  struct DmgEvent {
    DWORD tick_ms;
    int   damage;
  };

  // Raw damage events within the last kPlotSlots seconds.
  std::deque<DmgEvent> events_;

  // Circular buffer fed once per second for PlotLines display.
  std::array<float, kPlotSlots> plot_buf_ = {};
  int    plot_offset_  = 0;   // next write position (oldest visible slot)
  DWORD  last_slot_tick_ = 0; // tick_ms when the current slot started

  // Combat state
  DWORD last_damage_tick_ = 0;
  DWORD combat_start_tick_ = 0;
  bool  in_combat_         = false;
  int   total_damage_      = 0;
  float current_dps_       = 0.0f;
  float peak_dps_          = 0.0f;
  bool  in_game_           = false;
  // When locked: the title-bar rect (x0,y0,x1,y1) captured the previous frame,
  // so the collapse arrow stays clickable while the body is click-through.
  float lock_title_rect_[4] = {0, 0, 0, 0};

  void RecordDamage(int damage);
  void UpdatePlotSlot(DWORD now);
  void FlushChatQueue();

  // Messages queued from OnRecvPacket to avoid calling UIWindowMgr::SendMsg
  // from within the recv dispatch loop (causes freeze under heavy AoE spam).
  // Drained in OnRenderUI() — at most kMaxChatFlushPerFrame per frame.
  static constexpr int kMaxChatQueueSize   = 64;
  static constexpr int kMaxChatFlushPerFrame = 4;
  std::vector<std::string> chat_queue_;
};
