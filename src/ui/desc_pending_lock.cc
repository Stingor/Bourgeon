#include "ui/desc_pending_lock.h"

#include <Windows.h>  // GetTickCount — le garde-fou de temps

#include <cmath>

#include "imgui.h"

namespace ro {

void DescPendingLock::Arm() {
  const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
  armed_ = true;
  x_ = mouse_pos.x;
  y_ = mouse_pos.y;
  tick_ = GetTickCount();
}

bool DescPendingLock::BlocksHover() {
  if (!armed_) return false;
  constexpr float kMoveThreshold = 6.0f;   // px : ignore le micro-jitter de la souris
  constexpr uint32_t kMaxHoldMs = 1500;    // garde-fou : jamais bloqué indéfiniment
  const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
  const bool moved = std::fabs(mouse_pos.x - x_) + std::fabs(mouse_pos.y - y_) >
                     kMoveThreshold;
  if (moved || GetTickCount() - tick_ > kMaxHoldMs) armed_ = false;
  return armed_;
}

}  // namespace ro
