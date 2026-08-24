#include "ui/item_grid_chrome.h"

#include <cstdio>
#include <cstring>

#include "d3d9/d3d9_hook.h"  // Overlay_DeviceEpoch : les textures d'un device mort
#include "ui/ro_imgui.h"  // ro::Px (échelle), ro::SkinImageTint (teinte du skin)
#include "ui/ro_widgets.h"

namespace ro::grid {
namespace {

Chrome   g_chrome;
bool     g_loaded = false;
unsigned g_epoch  = 0;

SnapState g_snap;

}  // namespace

void BasicInterfacePath(const char* file, char* out, size_t out_sz) {
  ro::uipath::WithFileName(ro::uipath::kUiRootSample, file, out, out_sz);
}

const Chrome& Assets() {
  // 🔴 L'épreuve de l'ÉPOQUE DE DEVICE est ici et non chez l'appelant : une
  // texture créée sur un device perdu ne se dessine plus, et les deux fenêtres
  // devaient jusqu'ici penser chacune à purger ces quatre bitmaps-là. Une
  // ressource partagée se recharge toute seule, sinon la première qui oublie
  // laisse l'autre avec un décor vide.
  const unsigned epoch = Overlay_DeviceEpoch();
  if (g_loaded && epoch == g_epoch) return g_chrome;
  g_epoch  = epoch;
  g_loaded = true;
  g_chrome = Chrome{};
  char path[160];
  const char* names[3] = {"btnbar_left3.bmp", "btnbar_mid3.bmp",
                          "btnbar_right3.bmp"};
  for (int i = 0; i < 3; ++i) {
    BasicInterfacePath(names[i], path, sizeof(path));
    g_chrome.bar[i] = ro::TextureFromGameFile(path);
  }
  BasicInterfacePath("itemwin_mid.bmp", path, sizeof(path));
  g_chrome.tile = ro::TextureFromGameFile(path);
  g_chrome.weight =
      ro::TextureFromGameFile(reinterpret_cast<const char*>(ro::uipath::kIconWeight));
  g_chrome.num =
      ro::TextureFromGameFile(reinterpret_cast<const char*>(ro::uipath::kIconNum));
  return g_chrome;
}

void DrawFooterBar(ImDrawList* dl, float x0, float y0, float x1, float bar_h) {
  const Chrome& c = Assets();
  const float y1 = y0 + bar_h;
  const float lw = c.bar[0].w > 0 ? static_cast<float>(c.bar[0].w) : 0.0f;
  const float rw = c.bar[2].w > 0 ? static_cast<float>(c.bar[2].w) : 0.0f;
  if (c.bar[1].tex) {
    const ImU32 t = ro::SkinImageTint();
    if (c.bar[0].tex)
      dl->AddImage(TexId(c.bar[0].tex), ImVec2(x0, y0), ImVec2(x0 + lw, y1),
                   ImVec2(0, 0), ImVec2(1, 1), t);
    dl->AddImage(TexId(c.bar[1].tex), ImVec2(x0 + lw, y0), ImVec2(x1 - rw, y1),
                 ImVec2(0, 0), ImVec2(1, 1), t);  // étiré
    if (c.bar[2].tex)
      dl->AddImage(TexId(c.bar[2].tex), ImVec2(x1 - rw, y0), ImVec2(x1, y1),
                   ImVec2(0, 0), ImVec2(1, 1), t);
  } else {
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                      ImGui::GetColorU32(ImGuiCol_FrameBg), 2.0f);
  }
}

float DrawFooterIcon(ImDrawList* dl, const GameTexture& icon, float x, float cy) {
  if (!icon.tex || icon.w <= 0 || icon.h <= 0) return 0.0f;
  dl->AddImage(TexId(icon.tex), ImVec2(x, cy - icon.h * 0.5f),
               ImVec2(x + icon.w, cy + icon.h * 0.5f), ImVec2(0, 0), ImVec2(1, 1),
               ro::SkinImageTint());
  return static_cast<float>(icon.w);
}

void DrawTiledBg(ImDrawList* dl, const GameTexture& tile, ImVec2 origin,
                 ImVec2 mn, ImVec2 mx) {
  if (!tile.tex || tile.w <= 0 || tile.h <= 0) {
    dl->AddRectFilled(mn, mx, ImGui::GetColorU32(ImGuiCol_FrameBg));
    return;
  }
  // À l'échelle, comme les cases qu'elles pavent.
  const float tw = ro::Px(static_cast<float>(tile.w));
  const float th = ro::Px(static_cast<float>(tile.h));
  auto floorTo = [](float v, float o, float step) {
    int n = static_cast<int>((v - o) / step);
    if (o + n * step > v) --n;
    return o + n * step;
  };
  const float sx = floorTo(mn.x, origin.x, tw);
  const float sy = floorTo(mn.y, origin.y, th);
  dl->PushClipRect(mn, mx, true);
  for (float y = sy; y < mx.y; y += th)
    for (float x = sx; x < mx.x; x += tw)
      dl->AddImage(TexId(tile.tex), ImVec2(x, y), ImVec2(x + tw, y + th),
                   ImVec2(0, 0), ImVec2(1, 1), ro::SkinImageTint());
  dl->PopClipRect();
}

SnapState& Snap() { return g_snap; }

void SnapWindowSize(ImGuiSizeCallbackData* d) {
  const SnapState& s = g_snap;
  const float step = s.cell + s.gap;
  int cols = static_cast<int>((d->DesiredSize.x - s.chromew + s.gap) / step + 0.5f);
  int rows = static_cast<int>((d->DesiredSize.y - s.chromeh + s.gap) / step + 0.5f);
  if (cols < 5) cols = 5;
  if (rows < 5) rows = 5;
  d->DesiredSize.x = s.chromew + cols * step - s.gap;
  d->DesiredSize.y = s.chromeh + rows * step - s.gap;
}

}  // namespace ro::grid
