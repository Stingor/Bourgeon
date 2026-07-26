#include "ui/align_grid.h"

#include "imgui.h"

void AlignGrid::Draw() const {
  const ImVec2 ds = ImGui::GetIO().DisplaySize;
  const float step = static_cast<float>(cell_size_px_clamped());
  ImDrawList* dl = ImGui::GetBackgroundDrawList();  // over game, under windows
  const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(color[0], color[1], color[2], color[3]));
  // Anchor the grid on the screen center: offset the first line so a grid line
  // falls exactly on the centre, keeping the mesh symmetric left/right and
  // top/bottom (the bright centre cross then lands right on a line).
  const float ox = std::fmod(ds.x * 0.5f, step);
  const float oy = std::fmod(ds.y * 0.5f, step);
  for (float x = ox; x <= ds.x; x += step)
    dl->AddLine(ImVec2(x, 0.0f), ImVec2(x, ds.y), col);
  for (float y = oy; y <= ds.y; y += step)
    dl->AddLine(ImVec2(0.0f, y), ImVec2(ds.x, y), col);
  // Brighter center cross for quick centering.
  float ca = color[3] * 2.5f;
  if (ca > 1.0f) ca = 1.0f;
  const ImU32 cc = ImGui::ColorConvertFloat4ToU32(ImVec4(color[0], color[1], color[2], ca));
  dl->AddLine(ImVec2(ds.x * 0.5f, 0.0f), ImVec2(ds.x * 0.5f, ds.y), cc, 1.5f);
  dl->AddLine(ImVec2(0.0f, ds.y * 0.5f), ImVec2(ds.x, ds.y * 0.5f), cc, 1.5f);
}
