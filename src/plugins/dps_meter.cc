#include "plugins/dps_meter.h"

#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <numeric>

#include "bourgeon.h"
#include "imgui.h"
#include "utils/log_console.h"

// ── Packet layouts (after stripping the 2-byte opcode) ───────────────────────
//
// ZC_NOTIFY_ACT (0x08c8, PACKETVER >= 20131223) — 32 bytes:
//   [0..3]  src_id, [4..7] dst_id, [8..11] tick,
//   [12..15] src_speed, [16..19] dst_speed,
//   [20..23] damage (int32), [24] is_sp_dmg, [25..26] div,
//   [27] type  (0=atk,4=endure,8=multi,10=crit,11=miss), [28..31] damage2
//
// ZC_NOTIFY_SKILL (0x01de, PACKETVER >= 3) — 31 bytes:
//   [0..1] skill_id, [2..5] src_id, [6..9] dst_id, [10..13] start_time,
//   [14..17] attack_mt, [18..21] attacked_mt,
//   [22..25] damage (int32), [26..27] level, [28..29] count, [30] action
//
// ZC_NOTIFY_SKILL legacy (0x0115) — 33 bytes:
//   [0..1] skill_id, [2..5] src_id, [6..9] dst_id, [10..13] tick,
//   [14..17] sdelay, [18..21] ddelay, [22..23] dst_x, [24..25] dst_y,
//   [26..27] damage (int16), [28..29] skill_lv, [30..31] div, [32] type

#pragma pack(push, 1)
struct NotifyActPayload {
  int32_t  src_id;
  int32_t  dst_id;
  int32_t  tick;
  int32_t  src_speed;
  int32_t  dst_speed;
  int32_t  damage;
  int8_t   is_sp_damage;
  uint16_t div;
  uint8_t  type;
  int32_t  damage2;
};

struct NotifySkillPayload {
  uint16_t skill_id;
  uint32_t src_id;
  uint32_t dst_id;
  uint32_t start_time;
  int32_t  attack_mt;
  int32_t  attacked_mt;
  int32_t  damage;
  int16_t  level;
  int16_t  count;
  int8_t   action;
};

struct NotifySkill2Payload {
  uint16_t skill_id;
  uint32_t src_id;
  uint32_t dst_id;
  uint32_t tick;
  uint32_t sdelay;
  uint32_t ddelay;
  uint16_t dst_x;
  uint16_t dst_y;
  int16_t  damage;
  uint16_t skill_lv;
  uint16_t div;
  uint8_t  type;
};
#pragma pack(pop)

// action bytes that carry real damage (exclude miss, pickup, sit, stand)
static bool IsDamageAction(uint8_t action) {
  return action != 0x01 && action != 0x02 && action != 0x03 && action != 0x0b;
}

DpsMeter::DpsMeter() {
  auto& b = Bourgeon::Instance();
  b.RegisterObserveOpcode(kOpcodeNotifyAct,    sizeof(NotifyActPayload));
  b.RegisterObserveOpcode(kOpcodeNotifySkill,  sizeof(NotifySkillPayload));
  b.RegisterObserveOpcode(kOpcodeNotifySkill2, sizeof(NotifySkill2Payload));
  b.RegisterRecvOpcode(kOpcodeSkillUnitDmg);
}

void DpsMeter::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  if (mode_type != ModeMgr::ModeType::kGame)
    ResetHistory();
}

void DpsMeter::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  const uint32_t player_aid = Bourgeon::Instance().client().session().aid();
  uint32_t src_id = 0;
  int      damage = 0;
  uint8_t  action = 0;

  if (opcode == kOpcodeNotifyAct && len >= sizeof(NotifyActPayload)) {
    const auto* p = reinterpret_cast<const NotifyActPayload*>(data);
    src_id = static_cast<uint32_t>(p->src_id);
    damage = p->damage;
    action = p->type;
  } else if (opcode == kOpcodeNotifySkill && len >= sizeof(NotifySkillPayload)) {
    const auto* p = reinterpret_cast<const NotifySkillPayload*>(data);
    src_id = p->src_id;
    damage = p->damage;
    action = static_cast<uint8_t>(p->action);
  } else if (opcode == kOpcodeNotifySkill2 && len >= sizeof(NotifySkill2Payload)) {
    const auto* p = reinterpret_cast<const NotifySkill2Payload*>(data);
    src_id = p->src_id;
    damage = p->damage;
    action = p->type;
  } else if (opcode == kOpcodeSkillUnitDmg && len >= 8) {
    // ZC_BOURGEON_SKILL_DMG: [src_aid:4][damage:4] — sent SELF by server
    // for skill-unit hits (Storm Gust, Meteor Storm, LoV…).
    src_id = *reinterpret_cast<const uint32_t*>(data);
    damage = *reinterpret_cast<const int32_t*>(data + 4);
    action = 0;  // treated as normal hit — passes IsDamageAction
  } else {
    return;
  }

  if (src_id != player_aid || !IsDamageAction(action) || damage <= 0)
    return;

  RecordDamage(damage);
}

void DpsMeter::ResetHistory() {
  events_.clear();
  plot_buf_.fill(0.0f);
  plot_offset_      = 0;
  last_slot_tick_   = 0;
  last_damage_tick_ = 0;
  combat_start_tick_= 0;
  in_combat_        = false;
  total_damage_     = 0;
  current_dps_      = 0.0f;
  peak_dps_         = 0.0f;
}

void DpsMeter::RecordDamage(int damage) {
  const DWORD now = GetTickCount();

  if (!in_combat_ || (now - last_damage_tick_) > static_cast<DWORD>(combat_timeout_secs_ * 1000)) {
    // New combat — reset totals but keep plot history visible
    combat_start_tick_ = now;
    in_combat_         = true;
    total_damage_      = 0;
    peak_dps_          = 0.0f;
    events_.clear();
    plot_buf_.fill(0.0f);
    plot_offset_    = 0;
    last_slot_tick_ = now;
  }

  last_damage_tick_ = now;
  total_damage_    += damage;
  events_.push_back({now, damage});
}

void DpsMeter::UpdatePlotSlot(DWORD now) {
  if (last_slot_tick_ == 0) return;

  const DWORD sms = static_cast<DWORD>(slot_ms_);
  while (now - last_slot_tick_ >= sms) {
    float slot_dmg = 0.0f;
    for (const auto& e : events_) {
      if (e.tick_ms >= last_slot_tick_ && e.tick_ms < last_slot_tick_ + sms)
        slot_dmg += static_cast<float>(e.damage);
    }
    const float slot_dps = slot_dmg * (1000.0f / sms);
    plot_buf_[plot_offset_] = slot_dps;
    if (slot_dps > peak_dps_) peak_dps_ = slot_dps;
    plot_offset_ = (plot_offset_ + 1) % kPlotSlots;
    last_slot_tick_ += sms;
  }

  // Prune events older than the full plot window
  const DWORD cutoff = now - kPlotSlots * sms;
  while (!events_.empty() && events_.front().tick_ms < cutoff)
    events_.pop_front();
}

void DpsMeter::OnRenderUI() {
  const DWORD now = GetTickCount();

  // Expire combat after timeout
  if (in_combat_ && (now - last_damage_tick_) > static_cast<DWORD>(combat_timeout_secs_ * 1000))
    in_combat_ = false;

  if (in_combat_)
    UpdatePlotSlot(now);

  // Rolling DPS over configured window
  {
    const DWORD window_ms    = static_cast<DWORD>(dps_window_secs_ * 1000);
    const DWORD window_start = now - window_ms;
    int window_dmg = 0;
    for (const auto& e : events_)
      if (e.tick_ms >= window_start)
        window_dmg += e.damage;
    current_dps_ = static_cast<float>(window_dmg) / static_cast<float>(dps_window_secs_);
    // peak_dps_ is updated per-slot in UpdatePlotSlot (not smoothed)
  }

  if (!visible_) return;

  // Title shows current DPS even when collapsed; ###DPS Meter keeps the window ID stable.
  char title[64];
  std::snprintf(title, sizeof(title), "DPS Meter | %.0f DPS###DPS Meter", current_dps_);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
  ImGui::SetNextWindowSize(ImVec2(260, 160), ImGuiCond_FirstUseEver);
  bool open = true;
  ImGui::Begin(title, &open);
  if (!open) { visible_ = false; ImGui::End(); ImGui::PopStyleVar(4); return; }

  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%.0f DPS", current_dps_);
  ImGui::SameLine();
  ImGui::TextDisabled("  peak %.0f", peak_dps_);

  // Plot height = available space minus one text line at the bottom.
  const float line_h   = ImGui::GetTextLineHeightWithSpacing();
  const float plot_h   = std::max(ImGui::GetContentRegionAvail().y - line_h, 20.0f);

  // Plot: oldest → newest, use plot_offset_ as the starting slot
  ImGui::PlotLines("##dps", plot_buf_.data(), kPlotSlots, plot_offset_,
                   nullptr, 0.0f, std::max(peak_dps_ * 1.2f, 1.0f),
                   ImVec2(-1, plot_h));
  if (ImGui::IsItemHovered()) {
    const float t = std::clamp(
        (ImGui::GetIO().MousePos.x - ImGui::GetItemRectMin().x) /
        (ImGui::GetItemRectMax().x - ImGui::GetItemRectMin().x), 0.0f, 0.9999f);
    const int idx = ((int)(t * kPlotSlots) + plot_offset_) % kPlotSlots;
    ImGui::SetTooltip("%.0f DPS", plot_buf_[idx]);
  }

  if (in_combat_) {
    const float elapsed = static_cast<float>(now - combat_start_tick_) / 1000.0f;
    const float avg_dps = elapsed > 0.0f ? static_cast<float>(total_damage_) / elapsed : 0.0f;
    ImGui::TextDisabled("avg %.0f  |  %d dmg  |  %.0fs", avg_dps, total_damage_, elapsed);
  } else {
    ImGui::TextDisabled("(pas en combat)");
  }

  ImGui::End();
  ImGui::PopStyleVar(4);
}
