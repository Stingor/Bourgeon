#include "plugins/basic_info.h"

#include <cstdint>
#include <cstdio>

#include "bourgeon.h"
#include "imgui.h"
#include "plugins/moonlight_ui.h"  // shared AlignGrid (snap + draw)

// ── Live value sources (20250716 client) ─────────────────────────────────────
// EXP confirmed by RE of UIBasicInfoWnd::OnCreate (UIINT64BarGraph_SetCurMax) —
// INT64.  HP/SP confirmed by UIBasicInfoWnd::DrawContent, which reads them as
// (int) — INT32.  `max` may sit below `cur` (job exp); that's fine.
namespace {
constexpr float kSnapThreshold = 10.0f;  // px magnetism radius for sticky snap

struct Src {
  uintptr_t cur, max;
  bool      wide;       // true = INT64, false = INT32
  const char* label;
  const char* win_id;   // ImGui window id (### keeps it stable)
};
const Src kSrc[BasicInfoTweaks::kBarCount] = {
  {0x015fb9d0, 0x015fb9d8, true,  "Base", "###BIBaseExp"},
  {0x015fb9e8, 0x015fb9e0, true,  "Job",  "###BIJobExp"},
  {0x015ff908, 0x015ff90c, false, "HP",   "###BIHp"},
  {0x015ff910, 0x015ff914, false, "SP",   "###BISp"},
};

inline long long RDval(uintptr_t addr, bool wide) {
  return wide ? *reinterpret_cast<const volatile int64_t*>(addr)
              : static_cast<long long>(
                    *reinterpret_cast<const volatile int32_t*>(addr));
}

inline float ExpFrac(long long cur, long long max) {
  if (max <= 0) return 0.0f;
  const double f = static_cast<double>(cur) / static_cast<double>(max);
  if (f < 0.0) return 0.0f;
  if (f > 1.0) return 1.0f;
  return static_cast<float>(f);
}

// A full-screen UI (world map = window id 0x8c) replaces the in-game HUD; hide
// the bars + alignment grid while it is open. FindWindow @0x00a47b90 on the
// window manager @0x0131f4e8; the world-map window is destroyed on close, so
// this tracks its open-state exactly. (Mirrors MenuIconTweaks::HudReplaced.)
bool HudReplaced() {
  using FindWindowFn = void* (__thiscall*)(void*, int);
  return reinterpret_cast<FindWindowFn>(0x00a47b90)(
             reinterpret_cast<void*>(0x0131f4e8), 0x8c) != nullptr;
}
}  // namespace

void BasicInfoTweaks::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);
}

float BasicInfoTweaks::SnapValue(float v, float ext, int self_id,
                                 bool y_axis) const {
  float best = v, best_dist = kSnapThreshold;
  for (int j = 0; j < kBarCount; ++j) {
    if (j == self_id || !bars_[j].show) continue;
    const float opos = y_axis ? static_cast<float>(bars_[j].y)
                              : static_cast<float>(bars_[j].x);
    const float oext = y_axis ? static_cast<float>(bars_[j].h)
                              : static_cast<float>(bars_[j].w);
    // align-near, align-far, just-after, just-before
    const float cands[4] = {opos, opos + oext - ext, opos + oext, opos - ext};
    for (int c = 0; c < 4; ++c) {
      float d = cands[c] - v;
      if (d < 0.0f) d = -d;
      if (d < best_dist) { best_dist = d; best = cands[c]; }
    }
  }
  return best;
}

bool BasicInfoTweaks::DrawBar(BarId id, long long cur, long long max) {
  Bar& bar = bars_[id];
  const bool frozen = locked_;

  // Shared alignment grid (owned by MoonlightUi). SnapAxis is a no-op when grid
  // snapping is off, so we can call it unconditionally when present.
  const AlignGrid* grid = nullptr;
  if (auto* mui = Bourgeon::Instance().moonlight_ui()) grid = &mui->grid_;

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                           ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoBackground |  // we draw our own bg
                           ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoFocusOnAppearing;
  if (frozen) {
    flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
             ImGuiWindowFlags_NoInputs;  // freeze + click-through
  }

  // We drive move/resize ourselves (NoMove|NoResize) and pin the window exactly
  // to the stored geometry every frame, so the drawn bar, the hit-test rect and
  // the drag handle never desync (no fighting with ImGui's internal move).
  flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
  ImGui::SetNextWindowPos(ImVec2(static_cast<float>(bar.x),
                                 static_cast<float>(bar.y)), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(bar.w),
                                  static_cast<float>(bar.h)), ImGuiCond_Always);

  // Allow very small bars: ImGui's default 32x32 window minimum AND the corner
  // rounding both impose a height floor, so drop them for these windows.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(8.0f, 8.0f));

  bool changed = false;
  if (ImGui::Begin(kSrc[id].win_id, nullptr, flags)) {
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 sz = ImGui::GetWindowSize();
    int hl_edges = 0;  // edge(s) to highlight this frame (hover/drag feedback)

    // Custom move/resize via one full-window invisible button.  The grab spot
    // decides the mode: a window edge (within kEdge, or the generous bottom-right
    // corner grip) = resize that edge/corner, anywhere else = move.  We update the
    // stored geometry (which pins the window next frame), and snap the moved
    // position to other bars — graphics + hit-test stay in lockstep.
    if (!frozen) {
      const float kGrip = 12.0f;  // generous bottom-right corner grab zone
      const float kEdge = 5.0f;   // edge grab thickness
      ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
      ImGui::InvisibleButton("##bihandle", sz);
      const ImVec2 m = ImGui::GetIO().MousePos;
      const float rx = p0.x + sz.x, by = p0.y + sz.y;  // right / bottom edges

      // Edge hit-test under the cursor, shared by activation and the hover
      // highlight (no OS resize cursor here — the game draws its own).
      int hov = 0;
      if (m.x <= p0.x + kEdge) hov |= kEdgeL;
      if (m.x >= rx   - kEdge) hov |= kEdgeR;
      if (m.y <= p0.y + kEdge) hov |= kEdgeT;
      if (m.y >= by   - kEdge) hov |= kEdgeB;
      // Keep the original bottom-right corner grip generous.
      if (m.x >= rx - kGrip && m.y >= by - kGrip) hov |= kEdgeR | kEdgeB;

      if (ImGui::IsItemActivated()) {
        drag_edges_ = hov;
        drag_mode_  = hov ? 2 : 1;
        if (hov) {  // resize: offset from the grabbed edge so it tracks the cursor
          drag_off_x_ = (hov & kEdgeL) ? m.x - p0.x : (hov & kEdgeR) ? m.x - rx : 0.0f;
          drag_off_y_ = (hov & kEdgeT) ? m.y - p0.y : (hov & kEdgeB) ? m.y - by : 0.0f;
        } else {  // move: offset from the top-left
          drag_off_x_ = m.x - p0.x;
          drag_off_y_ = m.y - p0.y;
        }
      }
      // Highlight the dragged edges while resizing, else the hovered edge(s).
      if (ImGui::IsItemActive() && drag_mode_ == 2) hl_edges = drag_edges_;
      else if (ImGui::IsItemHovered())              hl_edges = hov;

      if (ImGui::IsItemActive()) {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        if (drag_mode_ == 1) {  // move
          float nx = m.x - drag_off_x_, ny = m.y - drag_off_y_;
          if (sticky_) {
            nx = SnapValue(nx, static_cast<float>(bar.w), id, false);
            ny = SnapValue(ny, static_cast<float>(bar.h), id, true);
          }
          // Snap the top-left corner to the visible grid lines.
          if (grid) { nx = grid->SnapAxis(nx, ds.x); ny = grid->SnapAxis(ny, ds.y); }
          const int ix = static_cast<int>(nx + 0.5f);
          const int iy = static_cast<int>(ny + 0.5f);
          if (ix != bar.x || iy != bar.y) {
            bar.x = ix; bar.y = iy; changed = true;
          }
        } else if (drag_mode_ == 2) {  // resize the grabbed edge(s)/corner
          // Move only the grabbed edges; the opposite ones stay pinned. Each
          // dragged edge snaps to the visible grid (lands on a line), then the
          // x/y/w/h fall out of the four edge positions.
          float left = p0.x, right = rx, top = p0.y, bottom = by;
          const float kMin = 5.0f;  // minimum bar width/height (px)
          if (drag_edges_ & kEdgeL) {
            left = m.x - drag_off_x_;
            if (grid) left = grid->SnapAxis(left, ds.x);
            if (left > right - kMin) left = right - kMin;
          } else if (drag_edges_ & kEdgeR) {
            right = m.x - drag_off_x_;
            if (grid) right = grid->SnapAxis(right, ds.x);
            if (right < left + kMin) right = left + kMin;
          }
          if (drag_edges_ & kEdgeT) {
            top = m.y - drag_off_y_;
            if (grid) top = grid->SnapAxis(top, ds.y);
            if (top > bottom - kMin) top = bottom - kMin;
          } else if (drag_edges_ & kEdgeB) {
            bottom = m.y - drag_off_y_;
            if (grid) bottom = grid->SnapAxis(bottom, ds.y);
            if (bottom < top + kMin) bottom = top + kMin;
          }
          const int ix = static_cast<int>(left   + 0.5f);
          const int iy = static_cast<int>(top    + 0.5f);
          const int nw = static_cast<int>(right  - left + 0.5f);
          const int nh = static_cast<int>(bottom - top  + 0.5f);
          if (ix != bar.x || iy != bar.y || nw != bar.w || nh != bar.h) {
            bar.x = ix; bar.y = iy; bar.w = nw; bar.h = nh; changed = true;
          }
        }
      }
    }

    const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = rounding_;  // AddRect* clamps to half-dimension

    const ImU32 bg = ImGui::ColorConvertFloat4ToU32(
        ImVec4(bg_color_[0], bg_color_[1], bg_color_[2], bg_color_[3]));
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(
        ImVec4(bar.fill[0], bar.fill[1], bar.fill[2], bar.fill[3]));
    const ImU32 border = IM_COL32(0, 0, 0, 160);

    const float f = ExpFrac(cur, max);

    dl->AddRectFilled(p0, p1, bg, rounding);
    // Fill: skip when essentially empty (avoids a rounded sliver at 0%), and
    // round only the trailing corners so the progress front stays a clean edge.
    if (vertical_) {
      const float fillpx = (p1.y - p0.y) * f;  // fill upward from bottom
      if (fillpx >= 1.0f) {
        const ImDrawFlags fl = (f >= 0.999f) ? ImDrawFlags_RoundCornersAll
                                             : ImDrawFlags_RoundCornersBottom;
        dl->AddRectFilled(ImVec2(p0.x, p1.y - fillpx), p1, fill, rounding, fl);
      }
    } else {
      const float fillpx = (p1.x - p0.x) * f;  // fill rightward from left
      if (fillpx >= 1.0f) {
        const ImDrawFlags fl = (f >= 0.999f) ? ImDrawFlags_RoundCornersAll
                                             : ImDrawFlags_RoundCornersLeft;
        dl->AddRectFilled(p0, ImVec2(p0.x + fillpx, p1.y), fill, rounding, fl);
      }
    }
    dl->AddRect(p0, p1, border, rounding);

    if (text_mode_ != 0) {
      const char* label = kSrc[id].label;
      char buf[96];
      if (text_mode_ == 1)
        std::snprintf(buf, sizeof(buf), "%s %.2f%%", label, f * 100.0f);
      else if (text_mode_ == 2)
        std::snprintf(buf, sizeof(buf), "%s %lld / %lld", label, cur, max);
      else
        std::snprintf(buf, sizeof(buf), "%s %lld / %lld (%.2f%%)", label, cur,
                      max, f * 100.0f);
      const ImVec2 ts = ImGui::CalcTextSize(buf);
      const ImVec2 tp((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f);
      dl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), IM_COL32(0, 0, 0, 200), buf);
      dl->AddText(tp, IM_COL32(255, 255, 255, 255), buf);
    }

    // Faint resize hint in the bottom-right corner (unlocked only).
    if (!frozen) {
      const float kGrip = 12.0f;
      dl->AddTriangleFilled(ImVec2(p1.x, p1.y - kGrip), ImVec2(p1.x, p1.y),
                            ImVec2(p1.x - kGrip, p1.y),
                            IM_COL32(255, 255, 255, 70));
    }

    // Edge-resize feedback: glow the hovered/dragged edge(s) (a corner lights
    // two). Replaces the OS resize cursor, which the game's own cursor hides.
    if (hl_edges) {
      const ImU32 hc = IM_COL32(255, 220, 80, 210);
      const float t  = 2.0f;
      if (hl_edges & kEdgeL) dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p0.x, p1.y), hc, t);
      if (hl_edges & kEdgeR) dl->AddLine(ImVec2(p1.x, p0.y), ImVec2(p1.x, p1.y), hc, t);
      if (hl_edges & kEdgeT) dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y), hc, t);
      if (hl_edges & kEdgeB) dl->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p1.y), hc, t);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(4);
  return changed;
}

void BasicInfoTweaks::OnRenderUI() {
  if (!in_game_) return;
  if (HudReplaced()) return;  // world map / full-screen UI replaces the HUD
  // The alignment grid is drawn by MoonlightUi (shared overlay), not here.

  if (!visible_) return;
  // Globals are only populated once a character is in the world.
  if (Bourgeon::Instance().client().session().aid() == 0) return;

  bool changed = false;
  for (int i = 0; i < kBarCount; ++i) {
    if (!bars_[i].show) continue;
    const long long cur = RDval(kSrc[i].cur, kSrc[i].wide);
    const long long max = RDval(kSrc[i].max, kSrc[i].wide);
    changed |= DrawBar(static_cast<BarId>(i), cur, max);
  }

  force_apply_ = false;  // one-shot: consumed after all bars drew

  if (changed) drag_pending_ = true;
  // Persist exactly once, when the user releases the mouse after moving/resizing.
  if (drag_pending_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    drag_pending_   = false;
    geometry_dirty_ = true;  // drained by MoonlightUi (saves the YAML)
  }
}
