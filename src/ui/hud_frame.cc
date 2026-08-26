#include "ui/hud_frame.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "ui/window_clamp.h"
#include "ui/window_zorder.h"
#include "ui/ro_imgui.h"  // ro::AddTextRelief
#include "ui/ui_palette.h"  // ro::pal : la palette de l'UI

namespace ro {
namespace {

// Bords saisissables, en bitmask.
enum Edge { kEdgeL = 1, kEdgeR = 2, kEdgeT = 4, kEdgeB = 8 };

constexpr float kGrip = 12.0f;           // coin bas-droit : prise généreuse
constexpr float kEdgeThickness = 5.0f;   // épaisseur de prise d'un bord
constexpr float kSnapThreshold = 10.0f;  // rayon d'aimantation entre frères
constexpr int   kTouchTolerance = 2;     // px : « ces deux cadres se touchent »

// 🔴 UN SEUL cadre est déplacé à la fois — c'est une souris, pas dix. L'état de
// geste est donc de fichier plutôt que par cadre : ça évite d'imposer un membre
// à chaque appelant, et ça rend impossible le bug « deux cadres se croient
// attrapés » qu'un état par instance autorise.
int   g_drag_mode  = 0;      // 0 = aucun, 1 = déplacement, 2 = redimensionnement
int   g_drag_edges = 0;      // bords saisis (redimensionnement)
float g_drag_off_x = 0.0f;   // écart curseur -> ancre, figé au début du geste
float g_drag_off_y = 0.0f;
int   g_drag_group = 0;      // bitmask des frères déplacés en bloc (CTRL)
char  g_drag_id[64] = {0};   // quel cadre ; vide = aucun

// Ce que EndHudFrame doit peindre PAR-DESSUS le contenu : la poignée et les
// bords allumés. 🔴 Ils ne peuvent pas être dessinés dans Begin — le contenu
// serait peint après et les recouvrirait (c'est l'ordre qu'a Basic Info, où le
// même dessin vient à la fin). Un seul cadre est ouvert à la fois : ces
// variables n'ont pas besoin d'être une pile.
bool   g_cur_visible = false;
bool   g_cur_locked  = false;
int    g_cur_hl      = 0;
ImVec2 g_cur_p0(0.0f, 0.0f);
ImVec2 g_cur_p1(0.0f, 0.0f);

bool IsDragOwner(const char* id) {
  return g_drag_id[0] != '\0' &&
         std::strncmp(g_drag_id, id, sizeof(g_drag_id) - 1) == 0;
}

// Aimantation d'une coordonnée sur les cadres FRÈRES : même bord, bord opposé,
// juste après, juste avant. On garde le candidat le plus proche sous le seuil.
float SnapToSiblings(float v, float extent, const HudFrameOpts& opts,
                     bool y_axis) {
  float best = v;
  float best_dist = kSnapThreshold;
  for (int j = 0; j < opts.siblings.count; ++j) {
    if (j == opts.index || !opts.siblings.Shown(j)) continue;
    const HudRect* other = opts.siblings.At(j);
    if (!other) continue;
    const float opos = static_cast<float>(y_axis ? other->y : other->x);
    const float oext = static_cast<float>(y_axis ? other->h : other->w);
    const float candidates[4] = {opos, opos + oext - extent, opos + oext,
                                 opos - extent};
    for (int c = 0; c < 4; ++c) {
      float d = candidates[c] - v;
      if (d < 0.0f) d = -d;
      if (d < best_dist) {
        best_dist = d;
        best = candidates[c];
      }
    }
  }
  return best;
}

// Le bloc de cadres qui se TOUCHENT, en partant de `seed` — transitivement :
// A touche B, B touche C, les trois partent ensemble.
int TouchGroup(const HudFrameOpts& opts, int seed) {
  int mask = 1 << seed;
  const HudRect* seed_rect = opts.siblings.At(seed);
  if (!seed_rect) return mask;

  auto touch = [](const HudRect& a, const HudRect& b) {
    const int t = kTouchTolerance;
    return a.x - t < b.x + b.w && b.x - t < a.x + a.w &&
           a.y - t < b.y + b.h && b.y - t < a.y + a.h;
  };

  for (bool added = true; added;) {
    added = false;
    for (int a = 0; a < opts.siblings.count; ++a) {
      if (!(mask & (1 << a))) continue;
      const HudRect* ra = opts.siblings.At(a);
      if (!ra) continue;
      for (int b = 0; b < opts.siblings.count; ++b) {
        if ((mask & (1 << b)) || !opts.siblings.Shown(b)) continue;
        const HudRect* rb = opts.siblings.At(b);
        if (rb && touch(*ra, *rb)) {
          mask |= (1 << b);
          added = true;
        }
      }
    }
  }
  return mask;
}

}  // namespace

bool HudFrameDragging() { return g_drag_mode != 0; }

void HudCenteredText(ImDrawList* draw_list, ImVec2 p0, ImVec2 p1,
                     const char* text, ImU32 color, float px) {
  if (!draw_list || !text || !*text) return;
  ImFont* font = ImGui::GetFont();
  if (px <= 0.0f) px = ImGui::GetFontSize();

  // Mesure À LA TAILLE DE DESSIN : c'est la seule façon d'obtenir un centrage
  // juste quand le texte n'est pas à la taille par défaut.
  ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);

  // Trop long pour le cadre : on TRONQUE plutôt que de déborder. Un texte qui
  // sort de son cadre se superpose à ses voisins, et sur un HUD composé de
  // cadres jointifs c'est immédiatement illisible.
  //
  // 🔴 Trois points ASCII, et surtout PAS le caractère « … » (U+2026) : la
  // police du client ne charge pas les glyphes au-delà de U+00FF, et l'ellipse
  // typographique y sortirait en carré vide.
  char clipped[512];
  const float avail = (p1.x - p0.x) - 4.0f;  // marge : le texte ne touche pas le bord
  if (avail > 0.0f && ts.x > avail) {
    static const char kEllipsis[] = "...";
    const float ellipsis_w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, kEllipsis).x;
    const char* remaining = nullptr;
    // ImGui coupe sur une frontière de caractère : pas de demi-glyphe UTF-8.
    font->CalcTextSizeA(px, (std::max)(0.0f, avail - ellipsis_w), 0.0f, text,
                        nullptr, &remaining);
    size_t kept = remaining ? static_cast<size_t>(remaining - text) : 0;
    if (kept > sizeof(clipped) - sizeof(kEllipsis))
      kept = sizeof(clipped) - sizeof(kEllipsis);
    std::memcpy(clipped, text, kept);
    std::memcpy(clipped + kept, kEllipsis, sizeof(kEllipsis));  // \0 compris
    text = clipped;
    ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
  }

  const ImVec2 tp((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f);
  // L'ombre suit la taille, sinon elle disparaît sous un gros texte.
  const float shadow = (std::max)(1.0f, std::floor(px * 0.08f));
  ro::AddTextRelief(draw_list, font, px, tp, color, text, nullptr,
                    ro::pal::kTextShadow, ImVec2(shadow, shadow));
}

void HudBarText(ImDrawList* draw_list, ImVec2 p0, ImVec2 p1, const char* text) {
  HudCenteredText(draw_list, p0, p1, text, IM_COL32_WHITE,
                  ImGui::GetFontSize());
}

bool BeginHudFrame(const char* id, HudRect* rect, const HudFrameOpts& opts,
                   bool* geometry_changed, HudFrameClicks* clicks) {
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                           ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoBackground |  // le fond est à nous
                           ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_NoMove |    // déplacement conduit ici
                           ImGuiWindowFlags_NoResize |  // idem
                           kBackgroundWindowFlags;      // jamais devant une vraie fenêtre
  // Verrouillé = figé ET clic-traversant… SAUF si le cadre est déclaré
  // cliquable : il reste figé, mais reprend la souris pour agir (cf. l'en-tête).
  if (opts.locked && !opts.clickable) flags |= ImGuiWindowFlags_NoInputs;

  // La fenêtre est ré-épinglée sur la géométrie mémorisée à chaque frame : le
  // dessin, la zone cliquable et la poignée restent en phase.
  ImGui::SetNextWindowPos(ImVec2(static_cast<float>(rect->x),
                                 static_cast<float>(rect->y)), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(rect->w),
                                  static_cast<float>(rect->h)), ImGuiCond_Always);

  // Un cadre de HUD peut être minuscule : le minimum d'ImGui (32×32), son
  // padding et son arrondi imposeraient sinon un plancher de taille.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(4.0f, 4.0f));

  MarkBackgroundWindow(id);
  const bool visible = ImGui::Begin(id, nullptr, flags);
  if (!visible) {
    g_cur_visible = false;
    return false;
  }

  const ImVec2 p0 = ImGui::GetWindowPos();
  const ImVec2 sz = ImGui::GetWindowSize();
  const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
  int highlight_edges = 0;  // bords à allumer cette frame

  if (!opts.locked) {
    // Une seule zone cliquable couvrant tout le cadre : c'est l'endroit SAISI
    // qui décide du geste — un bord (ou le coin bas-droit) redimensionne, le
    // reste déplace.
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::InvisibleButton("##hudframe", sz);

    const ImVec2 m = ImGui::GetIO().MousePos;
    const float rx = p1.x;
    const float by = p1.y;

    int hovered_edges = 0;
    if (m.x <= p0.x + kEdgeThickness) hovered_edges |= kEdgeL;
    if (m.x >= rx - kEdgeThickness)   hovered_edges |= kEdgeR;
    if (m.y <= p0.y + kEdgeThickness) hovered_edges |= kEdgeT;
    if (m.y >= by - kEdgeThickness)   hovered_edges |= kEdgeB;
    if (m.x >= rx - kGrip && m.y >= by - kGrip) hovered_edges |= kEdgeR | kEdgeB;

    if (ImGui::IsItemActivated()) {
      g_drag_edges = hovered_edges;
      g_drag_mode  = hovered_edges ? 2 : 1;
      strncpy_s(g_drag_id, sizeof(g_drag_id), id, _TRUNCATE);
      if (hovered_edges) {  // l'écart au bord saisi, pour qu'il suive le curseur
        g_drag_off_x = (hovered_edges & kEdgeL) ? m.x - p0.x
                     : (hovered_edges & kEdgeR) ? m.x - rx : 0.0f;
        g_drag_off_y = (hovered_edges & kEdgeT) ? m.y - p0.y
                     : (hovered_edges & kEdgeB) ? m.y - by : 0.0f;
      } else {              // déplacement : l'écart au coin haut-gauche
        g_drag_off_x = m.x - p0.x;
        g_drag_off_y = m.y - p0.y;
      }
      // CTRL + déplacement = tout le bloc qui se touche part d'un seul tenant.
      g_drag_group = (!hovered_edges && ImGui::GetIO().KeyCtrl && opts.index >= 0)
                         ? TouchGroup(opts, opts.index)
                         : 0;
    }

    // Retour visuel : les bords TIRÉS pendant un redimensionnement, sinon ceux
    // qui sont simplement survolés.
    if (ImGui::IsItemActive() && g_drag_mode == 2 && IsDragOwner(id))
      highlight_edges = g_drag_edges;
    else if (ImGui::IsItemHovered())
      highlight_edges = hovered_edges;

    if (ImGui::IsItemActive() && IsDragOwner(id)) {
      const ImVec2 ds = ImGui::GetIO().DisplaySize;
      if (g_drag_mode == 1) {
        float nx = m.x - g_drag_off_x;
        float ny = m.y - g_drag_off_y;
        // Aimantation entre frères d'abord, grille ensuite, clamp en dernier :
        // chaque étape peut défaire la précédente, et c'est l'écran qui doit
        // avoir le dernier mot.
        if (opts.sticky && opts.index >= 0) {
          nx = SnapToSiblings(nx, static_cast<float>(rect->w), opts, false);
          ny = SnapToSiblings(ny, static_cast<float>(rect->h), opts, true);
        }
        if (opts.grid) {
          nx = opts.grid->SnapAxis(nx, ds.x);
          ny = opts.grid->SnapAxis(ny, ds.y);
        }
        const ImVec2 in_screen = ClampWindowPosToScreen(
            ImVec2(nx, ny), ImVec2(static_cast<float>(rect->w),
                                   static_cast<float>(rect->h)));
        const int ix = static_cast<int>(in_screen.x + 0.5f);
        const int iy = static_cast<int>(in_screen.y + 0.5f);
        if (ix != rect->x || iy != rect->y) {
          const int dx = ix - rect->x;  // déplacement de CETTE frame
          const int dy = iy - rect->y;
          rect->x = ix;
          rect->y = iy;
          if (geometry_changed) *geometry_changed = true;
          // Bloc CTRL : les frères suivent du MÊME delta. L'aimantation et le
          // clamp ne s'appliquent qu'au cadre saisi — le bloc reste rigide.
          const int self_bit = 1 << opts.index;
          if (g_drag_group & ~self_bit) {
            for (int j = 0; j < opts.siblings.count; ++j) {
              if (j == opts.index || !(g_drag_group & (1 << j))) continue;
              HudRect* other = opts.siblings.At(j);
              if (!other) continue;
              other->x += dx;
              other->y += dy;
            }
          }
        }
      } else if (g_drag_mode == 2) {
        // Seuls les bords saisis bougent ; les opposés restent épinglés. La
        // géométrie retombe des quatre positions de bord.
        float left = p0.x, right = rx, top = p0.y, bottom = by;
        if (g_drag_edges & kEdgeL) {
          left = m.x - g_drag_off_x;
          if (opts.grid) left = opts.grid->SnapAxis(left, ds.x);
          if (left > right - opts.min_w) left = right - opts.min_w;
        } else if (g_drag_edges & kEdgeR) {
          right = m.x - g_drag_off_x;
          if (opts.grid) right = opts.grid->SnapAxis(right, ds.x);
          if (right < left + opts.min_w) right = left + opts.min_w;
        }
        if (g_drag_edges & kEdgeT) {
          top = m.y - g_drag_off_y;
          if (opts.grid) top = opts.grid->SnapAxis(top, ds.y);
          if (top > bottom - opts.min_h) top = bottom - opts.min_h;
        } else if (g_drag_edges & kEdgeB) {
          bottom = m.y - g_drag_off_y;
          if (opts.grid) bottom = opts.grid->SnapAxis(bottom, ds.y);
          if (bottom < top + opts.min_h) bottom = top + opts.min_h;
        }
        const int ix = static_cast<int>(left + 0.5f);
        const int iy = static_cast<int>(top + 0.5f);
        const int nw = static_cast<int>(right - left + 0.5f);
        const int nh = static_cast<int>(bottom - top + 0.5f);
        if (ix != rect->x || iy != rect->y || nw != rect->w || nh != rect->h) {
          rect->x = ix;
          rect->y = iy;
          rect->w = nw;
          rect->h = nh;
          if (geometry_changed) *geometry_changed = true;
        }
      }
    } else if (IsDragOwner(id) && !ImGui::IsItemActive()) {
      g_drag_mode = 0;
      g_drag_edges = 0;
      g_drag_group = 0;
      g_drag_id[0] = '\0';
    }
  } else if (opts.clickable) {
    // Cadre VERROUILLÉ mais actif : une seule zone cliquable couvrant tout, et
    // aucune géométrie qui bouge. Un bouton plutôt qu'un simple test de survol,
    // pour qu'ImGui possède vraiment l'appui — c'est ce qui empêche le clic
    // d'atteindre le jeu par le WndProc, et donc de partir au sol derrière.
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::InvisibleButton("##hudframe_proxy", sz,
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight);
    if (clicks) {
      clicks->hovered = ImGui::IsItemHovered();
      clicks->left    = ImGui::IsItemClicked(ImGuiMouseButton_Left);
      clicks->right   = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    }
    // Le survol s'allume : sans lui, rien ne distingue un cadre qui AGIT d'un
    // cadre qui se contente d'afficher, et le joueur ne peut pas savoir où son
    // clic va partir.
    if (ImGui::IsItemHovered()) highlight_edges = kEdgeL | kEdgeR | kEdgeT | kEdgeB;
    // Le curseur repart d'où il était : tout le contenu se dessine ensuite en
    // coordonnées absolues, mais autant ne rien laisser traîner.
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
  }

  // Fond et liseré, sous le contenu.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (opts.bg) {
    const ImU32 bg = ImGui::ColorConvertFloat4ToU32(
        ImVec4(opts.bg[0], opts.bg[1], opts.bg[2], opts.bg[3]));
    dl->AddRectFilled(p0, p1, bg, opts.rounding);
  }
  if (opts.border) dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 160), opts.rounding);

  // La poignée et les bords allumés se peignent à la FIN (cf. g_cur_*) : ici, le
  // contenu n'est pas encore dessiné et les recouvrirait.
  g_cur_visible = true;
  g_cur_locked  = opts.locked;
  g_cur_hl      = highlight_edges;
  g_cur_p0      = p0;
  g_cur_p1      = p1;
  return true;
}

void EndHudFrame() {
  if (g_cur_visible) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = g_cur_p0;
    const ImVec2 p1 = g_cur_p1;

    // Poignée de redimensionnement, discrète mais présente : sans elle, rien ne
    // dit au joueur que le cadre se retaille — le jeu dessine son propre
    // curseur, donc celui du système ne se voit jamais.
    if (!g_cur_locked) {
      dl->AddTriangleFilled(ImVec2(p1.x, p1.y - kGrip), ImVec2(p1.x, p1.y),
                            ImVec2(p1.x - kGrip, p1.y), IM_COL32(255, 255, 255, 70));
    }

    // Bords allumés — un coin en allume deux.
    if (g_cur_hl) {
      const ImU32 hc = IM_COL32(255, 220, 80, 210);
      const float t = 2.0f;
      if (g_cur_hl & kEdgeL) dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p0.x, p1.y), hc, t);
      if (g_cur_hl & kEdgeR) dl->AddLine(ImVec2(p1.x, p0.y), ImVec2(p1.x, p1.y), hc, t);
      if (g_cur_hl & kEdgeT) dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y), hc, t);
      if (g_cur_hl & kEdgeB) dl->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p1.y), hc, t);
    }
    g_cur_visible = false;
  }
  ImGui::End();
  ImGui::PopStyleVar(4);
}

}  // namespace ro
