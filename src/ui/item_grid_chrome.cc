#include "ui/item_grid_chrome.h"

#include <cstdio>
#include <cstring>

#include "d3d9/d3d9_hook.h"  // Overlay_DeviceEpoch : les textures d'un device mort
#include "ui/ro_imgui.h"  // ro::Px (échelle), ro::SkinImageTint (teinte du skin)
#include "ui/ro_widgets.h"
#include "utils/i18n.h"   // i18n::Tr : l'indice du champ de filtre"

namespace ro::grid {
namespace {

Chrome   g_chrome;
bool g_loaded = false;
Overlay_DeviceEpochWatch g_watch;

SnapState g_snap;

// Épaisseur de repli du strip quand aucune image d'onglet n'a pu être chargée.
constexpr float kTabStripFallbackPx = 22.0f;
// Tampons de composition des chemins d'image (les trois copies employaient déjà
// ces tailles).
constexpr size_t kPathMax = 160;
constexpr size_t kNameMax = 48;
// « tab » : ce que le nom du jeu horizontal remplace par « tabh ».
constexpr int kTabPrefixLen = 3;
// Côté d'une case : la taille NATIVE du client, mise à l'échelle par `ro::Px`.
constexpr float kCellPx = 32.0f;
// Ce que le bandeau du bas prend en plus de ses lignes de texte.
constexpr float kFooterPadPx = 6.0f;
// Le pont de l'onglet actif : de combien il rentre dans l'onglet, et de combien
// il mord sur la grille au-delà du bord.
constexpr float kBridgeInsetPx = 1.0f;
constexpr float kBridgeOverlapPx = 2.0f;

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
  // ⚠ Le témoin est consulté AVANT le court-circuit, jamais après : `Changed()`
  // consomme le changement, et le laisser derrière le `&&` le sauterait au tout
  // premier appel — l'époque ne serait pas relevée, et le tout prochain reset
  // passerait pour deux.
  const bool device_changed = g_watch.Changed();
  if (g_loaded && !device_changed) return g_chrome;
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

float TabStripThickness(const GameTexture* set, int count, bool horizontal) {
  float thick = 0.0f;
  for (int i = 0; i < count * 2; ++i) {  // [catégorie][actif, inactif], à plat
    const float v = static_cast<float>(horizontal ? set[i].h : set[i].w);
    if (v > thick) thick = v;
  }
  // 🔴 TOUJOURS `ro::Px` : le strip suit l'échelle de l'interface. C'est le
  // correctif qui n'avait pas atteint les trois copies.
  return ro::Px(thick > 0.0f ? thick : kTabStripFallbackPx);
}

void LoadTabTextures(const char* base, GameTexture vert[2], GameTexture horz[2]) {
  if (base == nullptr) return;
  char path[kPathMax], name[kNameMax];
  std::snprintf(name, sizeof(name), "%s1.bmp", base);
  BasicInterfacePath(name, path, sizeof(path));
  vert[0] = ro::TextureFromGameFile(path);
  std::snprintf(name, sizeof(name), "%s2.bmp", base);
  BasicInterfacePath(name, path, sizeof(path));
  vert[1] = ro::TextureFromGameFile(path);
  // Jeu HORIZONTAL : le même nom avec un « h » après « tab » (tab_use ->
  // tabh_use). Les trois copies sautaient les trois premiers caractères ; c'est
  // la longueur de « tab », et le préfixe est garanti par les tables d'onglets.
  char hbase[kNameMax];
  std::snprintf(hbase, sizeof(hbase), "tabh%s", base + kTabPrefixLen);
  std::snprintf(name, sizeof(name), "%s1.bmp", hbase);
  BasicInterfacePath(name, path, sizeof(path));
  horz[0] = ro::TextureFromGameFile(path);
  std::snprintf(name, sizeof(name), "%s2.bmp", hbase);
  BasicInterfacePath(name, path, sizeof(path));
  horz[1] = ro::TextureFromGameFile(path);
}

void DrawNameFilter(const char* imgui_id, ImGuiTextFilter* filter, bool visible) {
  if (visible) {
    ImGui::SetNextItemWidth(-1.0f);
    // 1er argument = l'ID du champ, JAMAIS traduit ; seul l'indice l'est.
    if (ImGui::InputTextWithHint(imgui_id, i18n::Tr("Filtrer..."), filter->InputBuf,
                                 IM_ARRAYSIZE(filter->InputBuf)))
      filter->Build();
  } else if (filter->InputBuf[0]) {
    filter->Clear();
  }
}

Metrics Measure(int footer_lines) {
  Metrics m;
  m.line_h   = ImGui::GetTextLineHeight();
  m.footer_h = footer_lines * m.line_h + kFooterPadPx;
  m.main_w   = ImGui::GetWindowWidth();
  m.main_h   = ImGui::GetWindowHeight();
  m.child_h  = -(m.footer_h + ImGui::GetStyle().ItemSpacing.y);
  return m;
}

Grid BeginItemGrid(const char* imgui_id, bool tabs_vertical, const Metrics& m,
                   const GameTexture& bg) {
  const ImGuiStyle& style = ImGui::GetStyle();
  if (tabs_vertical) {
    ImGui::SameLine(0.0f, 0.0f);
  } else {
    // Collée à la rangée d'onglets : on reprend l'ItemSpacing vertical que
    // l'enfant du strip vient d'insérer.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - style.ItemSpacing.y);
  }
  Grid g;
  g.menu_pad     = style.WindowPadding;
  g.menu_spacing = style.ItemSpacing;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  // En horizontal, la rangée d'onglets a déjà consommé sa hauteur : la grille
  // prend le reste (hauteur négative = « la place restante moins le bandeau »).
  ImGui::BeginChild(imgui_id, ImVec2(0.0f, m.child_h), true,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar);
  // Cases jointives (sans padding) et À L'ÉCHELLE de l'interface, sinon la
  // grille reste minuscule pendant que le texte des onglets et du bandeau
  // grandit autour d'elle. Le pavage de fond suit le même facteur, donc les
  // tuiles du fond restent alignées sur les cases.
  g.cell = ro::Px(kCellPx);
  g.gap  = 0.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(g.gap, g.gap));
  const float availw = ImGui::GetContentRegionAvail().x;  // exclut la scrollbar
  g.avail_h = ImGui::GetContentRegionAvail().y;
  // Mesure du chrome (fenêtre - zone grille) pour le snap de resize (frame +1).
  Snap().cell = g.cell;
  Snap().gap  = g.gap;
  Snap().chromew = m.main_w - availw;
  Snap().chromeh = m.main_h - g.avail_h;
  Snap().valid = true;
  g.cols = static_cast<int>((availw + g.gap) / (g.cell + g.gap));
  if (g.cols < 1) g.cols = 1;
  g.dl = ImGui::GetWindowDrawList();
  {  // Fond PAVÉ, ALIGNÉ sur la grille des cases (même marge qu'elles).
    const ImVec2 origin = ImGui::GetCursorScreenPos();  // = la 1re case
    const ImVec2 mn = ImGui::GetWindowPos();
    const ImVec2 sz = ImGui::GetWindowSize();
    DrawTiledBg(g.dl, bg, origin, mn, ImVec2(mn.x + sz.x, mn.y + sz.y));
  }
  return g;
}

void EndItemGrid() {
  ImGui::PopStyleVar();  // ItemSpacing (cases jointives)
  ImGui::EndChild();
  ImGui::PopStyleVar();  // WindowPadding (grille)
}

void DrawActiveTabBridge(ImDrawList* dl, ImVec2 tab_min, ImVec2 tab_max,
                         bool tabs_vertical, ImU32 color) {
  if (tabs_vertical)
    dl->AddRectFilled(
        ImVec2(tab_max.x - kBridgeInsetPx, tab_min.y + kBridgeInsetPx),
        ImVec2(tab_max.x + kBridgeOverlapPx, tab_max.y - kBridgeInsetPx), color);
  else
    dl->AddRectFilled(
        ImVec2(tab_min.x + kBridgeInsetPx, tab_max.y - kBridgeInsetPx),
        ImVec2(tab_max.x - kBridgeInsetPx, tab_max.y + kBridgeOverlapPx), color);
}

}  // namespace ro::grid
