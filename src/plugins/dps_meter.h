#pragma once

#include <Windows.h>
#include <array>
#include <deque>
#include <cstdint>
#include <string>
#include <vector>

#include "plugins/plugin.h"

class DpsMeter : public Plugin {
 public:
  DpsMeter();

  const char* name() const override { return "DPS Meter"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void OnRenderUI() override;

 public:
  // ── Settings (read/written by MoonlightUi) ────────────────────────────────
  bool visible_                 = true;
  bool locked_                  = false;  // freeze window + click-through
  float text_color_[4] = {1.00f, 0.80f, 0.20f, 1.0f};  // DPS value text colour
  float plot_color_[4] = {1.00f, 0.80f, 0.20f, 1.0f};  // graph line colour
  float bg_alpha_      = 0.90f;                         // window background alpha
  bool show_ground_dmg_in_chat_ = true;   // push ground-skill hits to chat log
  int  slot_ms_                 = 200;    // ms per plot slot (50–2000)
  int  dps_window_secs_         = 10;     // rolling DPS window in seconds (1–30)
  int  combat_timeout_secs_     = 5;      // seconds without damage → out of combat (1–15)

  void ResetHistory();  // call after changing slot_ms_

 private:
  static constexpr uint16_t kOpcodeNotifyAct    = 0x08c8;
  static constexpr uint16_t kOpcodeNotifySkill  = 0x01de;
  static constexpr uint16_t kOpcodeNotifySkill2 = 0x0115;
  static constexpr uint16_t kOpcodeSkillUnitDmg = 0x0C22;  // ZC_BOURGEON_SKILL_DMG

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
