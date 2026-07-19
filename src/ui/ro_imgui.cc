#include "ui/ro_imgui.h"

#include <Windows.h>

#include <cfloat>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"  // ImGui::GetActiveID, GetCurrentWindow, TitleBarRect

#include "d3d9/d3d9_hook.h"   // Overlay_CreateTextureARGB, Overlay_SetTextureFilter
#include "ui/ro_skin_blobs.hpp"  // dimensions des pièces (pixels chargés du client)
#include "plugins/moonlight_ui.h"  // SliderFloat/SliderInt variants that ALSO adjust on mouse-wheel while hovered

namespace ro {
namespace {

constexpr UINT kCp949 = 949;  // Unified Hangul Code (client wire/text encoding)

// Malgun Gothic ships with every Windows 10/11 SKU regardless of system locale,
// so a French Windows running a Korean client still has it. Covers latin + full
// modern hangul, so a single font handles both the UI and server strings.
constexpr char kKoreanFontPath[] = "C:\\Windows\\Fonts\\malgun.ttf";

// Rotating thread-local scratch so several converted strings can coexist within
// one frame (e.g. two TextCp949 calls in a row) without clobbering each other.
std::string& NextScratch() {
  constexpr int kSlots = 8;
  thread_local std::string bufs[kSlots];
  thread_local int idx = 0;
  std::string& s = bufs[idx];
  idx = (idx + 1) % kSlots;
  return s;
}

// imgui_stdlib-style resize callback so InputText can grow a std::string.
struct InputTextUserData {
  std::string* str;
};

int InputTextResizeCb(ImGuiInputTextCallbackData* data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* ud = static_cast<InputTextUserData*>(data->UserData);
    std::string* str = ud->str;
    IM_ASSERT(data->Buf == str->c_str());
    str->resize(data->BufTextLen);
    data->Buf = str->data();
  }
  return 0;
}

}  // namespace

const char* Cp949ToUtf8(const char* cp949) {
  std::string& out = NextScratch();
  out.clear();
  if (!cp949 || !*cp949) return out.c_str();

  int wlen = MultiByteToWideChar(kCp949, 0, cp949, -1, nullptr, 0);
  if (wlen <= 1) return out.c_str();  // includes the null terminator
  std::wstring wide(wlen, L'\0');
  MultiByteToWideChar(kCp949, 0, cp949, -1, wide.data(), wlen);

  int ulen = WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, nullptr, 0,
                                 nullptr, nullptr);
  if (ulen <= 1) return out.c_str();
  out.resize(ulen - 1);  // drop the trailing null from the buffer size
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, out.data(), ulen, nullptr,
                      nullptr);
  return out.c_str();
}

int Utf8ToCp949(const char* utf8, char* out, size_t out_size) {
  if (!out || out_size == 0) return -1;
  out[0] = '\0';
  if (!utf8 || !*utf8) return 0;

  int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
  if (wlen <= 1) return 0;
  std::wstring wide(wlen, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), wlen);

  // '?' for glyphs that have no CP949 representation, so nothing is silently
  // dropped (the server would otherwise receive a truncated string).
  const char fallback = '?';
  int written = WideCharToMultiByte(kCp949, 0, wide.data(), -1, out,
                                    static_cast<int>(out_size), &fallback,
                                    nullptr);
  if (written <= 0) {
    out[0] = '\0';
    return -1;  // out too small (ERROR_INSUFFICIENT_BUFFER) or failure
  }
  return written - 1;  // exclude the null terminator
}

namespace {
ImFont* g_font_default = nullptr;  // police intégrée ImGui (ProggyClean) = repli
ImFont* g_font_malgun = nullptr;   // Malgun Gothic (null si absente du système)
bool g_font_enabled = true;        // état du toggle (mémorisé même avant load)

// (Re)sélectionne la police active selon le toggle. Immédiat (pris en compte au
// prochain NewFrame), sans rebuild d'atlas.
void ApplyFontSelection() {
  ImGui::GetIO().FontDefault =
      (g_font_enabled && g_font_malgun) ? g_font_malgun : g_font_default;
}
}  // namespace

ImFont* LoadKoreanFont(float size_px) {
  ImGuiIO& io = ImGui::GetIO();
  // Les DEUX polices sont bakées dans l'atlas au init → bascule gratuite ensuite.
  g_font_default = io.Fonts->AddFontDefault();

  if (GetFileAttributesA(kKoreanFontPath) != INVALID_FILE_ATTRIBUTES) {
    ImFontConfig cfg;
    cfg.OversampleH = 1;  // keep the pre-baked hangul atlas within DX7 limits
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    // Explicit ranges = glyphs are baked into the static atlas at build time,
    // which is what the DX7 backend needs (it has no dynamic-texture path).
    g_font_malgun = io.Fonts->AddFontFromFileTTF(kKoreanFontPath, size_px, &cfg,
                                                 io.Fonts->GetGlyphRangesKorean());
  }
  ApplyFontSelection();
  return io.FontDefault;
}

void SetFontEnabled(bool enabled) {
  g_font_enabled = enabled;
  if (ImGui::GetCurrentContext()) ApplyFontSelection();
}

bool IsFontEnabled() { return g_font_enabled; }

void TextCp949(const char* cp949) {
  ImGui::TextUnformatted(Cp949ToUtf8(cp949));
}

// ── Skin RO ───────────────────────────────────────────────────────────────────
namespace {

// (Plus de g_skin_enabled : le skin RO est l'habillage standard, toujours actif.)
int g_skin_colors = 0;  // combien de PushStyleColor à dépiler dans EndRoWindow
int g_skin_vars = 0;

// Type de curseur RO "main" (index d'action du sprite curseur). À CONFIRMER en jeu
// (si ce n'est pas une main, tester d'autres index : 1..6).
constexpr int kRoCursorHand = 2;
int g_hover_cursor = 0;  // curseur RO demandé cette frame par un widget survolé

RoSkinConfig g_cfg;  // leviers de customisation (persistés par l'appelant)

// Applique luminosité (rgb*brightness) + opacité (a*alpha) globales à une couleur.
// Utilisé par TOUTES les pièces dessinées main (images + texte) pour qu'elles
// suivent les réglages, qu'ImGuiStyleVar_Alpha ne touche pas (rendu manuel).
ImU32 ApplySkinTint(ImU32 c) {
  float b = g_cfg.title_brightness;
  if (b < 0.0f) b = 0.0f;
  if (b > 2.0f) b = 2.0f;
  const float a = g_cfg.alpha;
  int r = (int)(((c >> IM_COL32_R_SHIFT) & 0xFF) * b);
  int g = (int)(((c >> IM_COL32_G_SHIFT) & 0xFF) * b);
  int bl = (int)(((c >> IM_COL32_B_SHIFT) & 0xFF) * b);
  int al = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (bl > 255) bl = 255;
  if (al > 255) al = 255;
  return IM_COL32(r, g, bl, al);
}

struct SkinTex {
  void* tex = nullptr;
  int w = 0, h = 0;
  unsigned epoch = 0;  // device epoch sous lequel tex a été créée (cf Overlay_DeviceEpoch)
};
SkinTex g_tl, g_tm, g_tr, g_close, g_close_on, g_mini, g_mini_on;
SkinTex g_base, g_base_on;  // bullet sys_base devant le titre (décoratif ou bouton)
// Bullet cliquable demandé pour la PROCHAINE fenêtre RO (SetNextWindowTitleBullet),
// puis résultat du clic pour la fenêtre courante (TitleBulletClicked).
bool g_next_bullet = false;
const char* g_next_bullet_tip = nullptr;
bool g_bullet_clicked = false;
// Couleur de corps demandée pour la PROCHAINE fenêtre RO (SetNextWindowBodyColor).
bool g_next_body_set = false;
unsigned int g_next_body_col = 0;
SkinTex g_btn_out_l, g_btn_out_m, g_btn_out_r;
SkinTex g_btn_over_l, g_btn_over_m, g_btn_over_r;
SkinTex g_btn_press_l, g_btn_press_m, g_btn_press_r;
SkinTex g_sbtn_out_l, g_sbtn_out_m, g_sbtn_out_r;
SkinTex g_sbtn_over_l, g_sbtn_over_m, g_sbtn_over_r;
SkinTex g_sbtn_press_l, g_sbtn_press_m, g_sbtn_press_r;
SkinTex g_resize;
SkinTex g_tb_btn_a, g_tb_btn_b, g_tb_btn_c;  // bouton flèche du combo (txtbox_btn_*)
SkinTex g_cb0, g_cb1;
SkinTex g_s0up, g_s0down, g_s0mid, g_s0bar_up, g_s0bar_mid, g_s0bar_down;
// Scrollbar HORIZONTALE du client (scroll1*) : piste+flèches et thumb. Sert de
// base au slider RO (RO n'a pas de « slider », son équivalent visuel est celle-ci).
SkinTex g_s1l, g_s1m, g_s1r, g_s1bar_l, g_s1bar_m, g_s1bar_r;
SkinTex g_bar_l, g_bar_m, g_bar_r, g_iconnum;
SkinTex g_up_l, g_up_m, g_up_r;                    // barre de titre desc (skill_upbar)
SkinTex g_sb_lm, g_sb_rm, g_sb_ld, g_sb_md, g_sb_rd;  // cadre boîte desc (sysbox)
SkinTex g_sb_lu, g_sb_mu, g_sb_ru;                    // haut du sysbox (panels sans titre)
bool g_skin_active = false;  // BeginRoWindow a pris la branche skin (pour EndRoWindow)

// ── Loader natif du client (conventions menu_icons.cc / status_tweaks.cc) ──────
// Charge un bmp d'UI depuis le VFS du jeu (GRF + overrides data\) → un joueur qui
// remplace le bmp dans son GRF/data voit son skin custom appliqué, à la RO.
constexpr uintptr_t kTexMgr = 0x00a90350;   // __cdecl() -> tex mgr
constexpr uintptr_t kMakeKey = 0x00a9f030;  // __cdecl(path) -> key
constexpr uintptr_t kLoadTex = 0x00a8d4a0;  // __fastcall(mgr,_,key) -> UITexture*
using TexMgr_t = void*(__cdecl*)();
using MakeKey_t = void*(__cdecl*)(const char*);
using LoadTex_t = void*(__fastcall*)(void*, void*, void*);
constexpr int kOffW = 0x114, kOffH = 0x118, kOffPix = 0x11c;  // UITexture fields
// "유저인터페이스\" en CP949 (bytes verbatim, cf. menu_icons.cc).
const char kUIDir[] = "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA";

// Charge un bmp d'UI (chemin RELATIF sous 유저인터페이스\) via le loader natif,
// décode BGRA->A8R8G8B8 avec magenta #FF00FF -> alpha. null si absent/échec.
void* LoadClientBmp(const char* rel_path, int* out_w, int* out_h) {
  void* mgr = reinterpret_cast<TexMgr_t>(kTexMgr)();
  if (!mgr) return nullptr;
  char full[192];
  std::snprintf(full, sizeof(full), "%s\\%s", kUIDir, rel_path);
  void* key = reinterpret_cast<MakeKey_t>(kMakeKey)(full);
  if (!key) return nullptr;
  void* tex = reinterpret_cast<LoadTex_t>(kLoadTex)(mgr, nullptr, key);
  if (!tex) return nullptr;
  const int w = *reinterpret_cast<int*>(static_cast<char*>(tex) + kOffW);
  const int h = *reinterpret_cast<int*>(static_cast<char*>(tex) + kOffH);
  void* bgra = *reinterpret_cast<void**>(static_cast<char*>(tex) + kOffPix);
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || !bgra) return nullptr;
  std::vector<unsigned char> argb(static_cast<size_t>(w) * h * 4);
  const unsigned char* src = static_cast<const unsigned char*>(bgra);
  for (int i = 0; i < w * h; ++i) {
    const unsigned char b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
    const bool keyed = (r == 0xFF && g == 0x00 && b == 0xFF);
    argb[i * 4 + 0] = b;
    argb[i * 4 + 1] = g;
    argb[i * 4 + 2] = r;
    argb[i * 4 + 3] = keyed ? 0 : 0xFF;
  }
  *out_w = w;
  *out_h = h;
  return Overlay_CreateTextureARGB(argb.data(), w, h);
}

// Charge la pièce depuis les fichiers du client (GRF/data) via le loader natif.
// Les dimensions (b.w/b.h) sont connues d'avance → layout stable même avant que la
// texture soit chargée. Retente tant que la texture n'est pas prête (device pas
// prêt) ; si le fichier manque vraiment côté client, la pièce ne se dessine pas.
void* EnsureTex(const char* rel_path, const skin::Blob& b, SkinTex& out) {
  // Texture D3DPOOL_DEFAULT : morte après reset/recréation du device -> on lâche
  // le handle mort pour forcer un rechargement (sinon BlitStretch/AddImage plante).
  const unsigned dev_e = Overlay_DeviceEpoch();
  if (out.epoch != dev_e) { out.tex = nullptr; out.epoch = dev_e; }
  out.w = b.w;
  out.h = b.h;
  if (out.tex) return out.tex;
  int w = 0, h = 0;
  void* t = LoadClientBmp(rel_path, &w, &h);
  if (t) {
    out.tex = t;
    out.w = w;
    out.h = h;
  }
  return out.tex;
}

// Charge une pièce UNIQUEMENT depuis le client (pas de blob de repli embarqué) :
// pour les ressources natives toujours présentes (txtbox_btn_*). out.tex reste nul
// si le BMP manque -> le widget dessine un repli à plat.
void* EnsureTexClient(const char* rel_path, SkinTex& out) {
  const unsigned dev_e = Overlay_DeviceEpoch();
  if (out.epoch != dev_e) { out.tex = nullptr; out.epoch = dev_e; }  // device changé -> recharge
  if (out.tex) return out.tex;
  int w = 0, h = 0;
  void* t = LoadClientBmp(rel_path, &w, &h);
  if (t) { out.tex = t; out.w = w; out.h = h; }
  return out.tex;
}

// Callback ImDrawList : bascule l'échantillonnage en POINT (pixel-art net).
void ImCb_PointFilter(const ImDrawList*, const ImDrawCmd*) {
  Overlay_SetTextureFilter(false);
}

// Dessine une pièce dans le rect donné (uv plein, teinte optionnelle). Renvoie
// true si la texture est prête.
bool BlitStretch(ImDrawList* dl, const SkinTex& t, ImVec2 p0, ImVec2 p1,
                 ImU32 col = IM_COL32_WHITE) {
  if (!t.tex) return false;
  // Luminosité + opacité globales appliquées à chaque pièce.
  dl->AddImage((ImTextureID)t.tex, p0, p1, ImVec2(0, 0), ImVec2(1, 1),
               ApplySkinTint(col));
  return true;
}

// Bouton système (11x11) dessiné à (cx,cy) top-left. Renvoie true si cliqué.
// Position arrondie à l'entier : sinon (sur une barre de titre de hauteur impaire
// vs bouton pair) le Y tombe en x.5 → sampling POINT → bouton « botched ».
bool SysButton(ImDrawList* dl, const SkinTex& off, const SkinTex& on, ImVec2 tl) {
  tl.x = ImFloor(tl.x);
  tl.y = ImFloor(tl.y);
  const ImVec2 br(tl.x + (float)off.w, tl.y + (float)off.h);
  const bool hovered = ImGui::IsMouseHoveringRect(tl, br, false);
  const SkinTex& t = (hovered && on.tex) ? on : off;
  BlitStretch(dl, t, tl, br);
  return hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

// Peint la scrollbar verticale RO par-dessus la scrollbar ImGui (transparente) et
// possède entièrement l'interaction (drag du thumb + flèches), en écrivant le
// scroll directement (annule la cible ImGui) → autorité totale, thumb immédiat.
void DrawRoScrollbar(ImGuiWindow* w) {
  if (!w || !w->ScrollbarY) return;
  EnsureTex("scroll0up.bmp", skin::kScroll0Up, g_s0up);
  EnsureTex("scroll0down.bmp", skin::kScroll0Down, g_s0down);
  EnsureTex("scroll0mid.bmp", skin::kScroll0Mid, g_s0mid);
  EnsureTex("scroll0bar_up.bmp", skin::kScroll0BarUp, g_s0bar_up);
  EnsureTex("scroll0bar_mid.bmp", skin::kScroll0BarMid, g_s0bar_mid);
  EnsureTex("scroll0bar_down.bmp", skin::kScroll0BarDown, g_s0bar_down);

  const ImRect bb = ImGui::GetWindowScrollbarRect(w, ImGuiAxis_Y);
  const float x0 = bb.Min.x;
  // Largeur visuelle RO fixe (13px). Si ImGui a réservé un slot PLUS LARGE (fenêtre
  // desc : ScrollbarSize inclut l'épaisseur du cadre sysbox), on garde la scrollbar
  // à 13px À GAUCHE du slot : la marge droite est occupée par le cadre -> scrollbar
  // DANS le cadre, pas par-dessus le bord.
  float x1 = bb.Max.x;
  const float kRoScrollW = 13.0f;
  if (x1 - x0 > kRoScrollW) x1 = x0 + kRoScrollW;
  const float y0 = bb.Min.y;
  float y1 = bb.Max.y;
  // Fenêtre principale redimensionnable : on raccourcit la scrollbar en bas pour
  // laisser la place au grip de resize (sinon il est mangé par la scrollbar). Les
  // child/table windows n'ont pas de grip → pas de raccourci.
  const bool has_grip = !(w->Flags & ImGuiWindowFlags_ChildWindow) &&
                        !(w->Flags & ImGuiWindowFlags_NoResize) &&
                        !(w->Flags & ImGuiWindowFlags_AlwaysAutoResize);
  if (has_grip) y1 -= (float)skin::kBtnResize.h;
  const float arrow = (float)skin::kScroll0Up.h;  // 13
  const float track_top = y0 + arrow, track_bot = y1 - arrow;
  const float track_h = track_bot - track_top;
  const float smax = w->ScrollMax.y;
  ImDrawList* dl = w->DrawList;

  // Écrit le scroll immédiatement + annule toute cible ImGui (FLT_MAX = "pas de
  // cible") pour que notre valeur soit autoritaire ce frame et le suivant.
  auto set_scroll = [&](float s) {
    if (s < 0.0f) s = 0.0f;
    if (s > smax) s = smax;
    w->Scroll.y = s;
    w->ScrollTarget.y = FLT_MAX;
  };

  // Taille du thumb, dans la piste ENTRE les flèches.
  float grab_h = track_h;
  const float size_avail = w->InnerRect.GetHeight();
  const float size_contents = w->ContentSize.y + w->WindowPadding.y * 2.0f;
  const bool scrollable = (size_contents > size_avail && track_h > 0.0f);
  if (scrollable) {
    grab_h = track_h * (size_avail / size_contents);
    const float gmin = ImGui::GetStyle().GrabMinSize;
    if (grab_h < gmin) grab_h = gmin;
    if (grab_h > track_h) grab_h = track_h;
  }

  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
  const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  float sratio = smax > 0.0f ? ImSaturate(w->Scroll.y / smax) : 0.0f;
  float grab_y = track_top + sratio * (track_h - grab_h);

  // Drag du thumb (possédé par nous, keyé par fenêtre).
  static ImGuiID s_drag = 0;
  static float s_off = 0.0f;
  if (scrollable && smax > 0.0f && track_h > grab_h) {
    const bool over_thumb = ImGui::IsMouseHoveringRect(
        ImVec2(x0, grab_y), ImVec2(x1, grab_y + grab_h), false);
    if (s_drag == 0 && over_thumb && clicked) {
      s_drag = w->ID;
      s_off = mouse.y - grab_y;
    }
    if (s_drag == w->ID) {
      if (!down) {
        s_drag = 0;
      } else {
        const float r = (mouse.y - s_off - track_top) / (track_h - grab_h);
        set_scroll(ImSaturate(r) * smax);
      }
    }
  }
  // Flèches (clic = 1 pas, maintien = défilement continu).
  const float step = ImGui::GetTextLineHeightWithSpacing() * 3.0f;
  if (scrollable && down &&
      ImGui::IsMouseHoveringRect(ImVec2(x0, y0), ImVec2(x1, y0 + arrow), false))
    set_scroll(w->Scroll.y - (clicked ? step : step * 0.2f));
  if (scrollable && down &&
      ImGui::IsMouseHoveringRect(ImVec2(x0, y1 - arrow), ImVec2(x1, y1), false))
    set_scroll(w->Scroll.y + (clicked ? step : step * 0.2f));

  // Recalcule la position du thumb après interaction.
  sratio = smax > 0.0f ? ImSaturate(w->Scroll.y / smax) : 0.0f;
  grab_y = track_top + sratio * (track_h - grab_h);

  // ── Dessin ──
  dl->PushClipRect(bb.Min, bb.Max, false);
  dl->AddCallback(ImCb_PointFilter, nullptr);
  // Piste : chevauche les flèches de 2px (elles sont peintes par-dessus) → jointure
  // sans trou (seamless).
  BlitStretch(dl, g_s0mid, ImVec2(x0, track_top - 2.0f),
              ImVec2(x1, track_bot + 2.0f));
  BlitStretch(dl, g_s0up, ImVec2(x0, y0), ImVec2(x1, y0 + arrow));
  BlitStretch(dl, g_s0down, ImVec2(x0, y1 - arrow), ImVec2(x1, y1));
  if (scrollable && track_h > grab_h) {
    const float cap = (float)skin::kScroll0BarUp.h;  // 4
    const float gy = ImFloor(grab_y);
    BlitStretch(dl, g_s0bar_up, ImVec2(x0, gy), ImVec2(x1, gy + cap));
    BlitStretch(dl, g_s0bar_down, ImVec2(x0, gy + grab_h - cap),
                ImVec2(x1, gy + grab_h));
    if (grab_h > cap * 2.0f)
      BlitStretch(dl, g_s0bar_mid, ImVec2(x0, gy + cap),
                  ImVec2(x1, gy + grab_h - cap));
  }
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  dl->PopClipRect();

  // Curseur main sur toute la scrollbar (comme le natif).
  if (ImGui::IsMouseHoveringRect(bb.Min, bb.Max, false))
    SetHoverCursor(kRoCursorHand);
}

}  // namespace

RoSkinConfig& SkinConfig() { return g_cfg; }

unsigned int ListBodyColorU32() {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(g_cfg.list_col[0], g_cfg.list_col[1],
                                               g_cfg.list_col[2], g_cfg.list_col[3]));
}

// ── Échap centralisé ──
// On ne stocke QUE des ImGuiWindow* (persistants) — jamais de bool* (souvent un
// local d'OnRenderUI → pendouillant après retour). La fermeture se fait dans
// RegisterEscapeWindow (appelé pendant le Begin, où p_open est valide) au frame
// suivant : ProcessEscapeStack désigne la fenêtre-cible, Register la ferme.
namespace {
std::vector<ImGuiWindow*> g_esc_list;         // fenêtres fermables visibles ce frame
ImGuiWindow* g_esc_close_target = nullptr;    // à fermer au prochain Begin
bool g_esc_any = false;                        // ≥1 ouverte (lu par le WndProc)
bool* g_esc_min_request = nullptr;             // flag « replier » de la fenêtre principale
bool g_esc_suppress = false;                   // un popup modal capte Échap ce frame
}  // namespace

void RegisterEscapeWindow(bool* p_open) {
  if (!p_open || !*p_open) return;
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (!w) return;
  g_esc_list.push_back(w);
  if (w == g_esc_close_target) {  // cible désignée à la frame précédente
    *p_open = false;
    g_esc_close_target = nullptr;
  }
}

void RegisterEscapeMinimizeWindow(bool* p_request_collapse) {
  g_esc_min_request = p_request_collapse;
}

void SuppressEscapeStack() { g_esc_suppress = true; }

void ProcessEscapeStack() {
  // La fenêtre principale (repli) compte comme « ouverte » pour l'avalage, mais reste
  // le tout dernier recours : on ne la minimise que si plus AUCUNE fenêtre fermable.
  g_esc_any = !g_esc_list.empty() || (g_esc_min_request != nullptr);
  // Un popup modal a capté Échap ce frame -> on ne ferme AUCUNE fenêtre RO derrière
  // (sinon Échap fermerait la modale ET la desc). Le flag est consommé ici.
  const bool suppressed = g_esc_suppress;
  g_esc_suppress = false;
  if (!suppressed && g_esc_any &&
      ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false)) {
    if (!g_esc_list.empty()) {
      // Désigne la plus au-dessus (FocusOrder max = plus récemment devant) ; fermée
      // au prochain Begin (p_open valide à ce moment-là).
      ImGuiWindow* top = nullptr;
      for (ImGuiWindow* w : g_esc_list)
        if (!top || w->FocusOrder > top->FocusOrder) top = w;
      g_esc_close_target = top;
    } else if (g_esc_min_request) {
      // Seule la fenêtre principale reste : Échap la MINIMISE (elle se replie +
      // persiste au rendu suivant), puis le prochain Échap ira au jeu.
      *g_esc_min_request = true;
    }
  }
  g_esc_list.clear();
  g_esc_min_request = nullptr;
}

bool AnyEscapeWindowOpen() { return g_esc_any; }

float SkinImageBrightness() {
  float b = g_cfg.title_brightness;
  if (b < 0.0f) b = 0.0f;
  if (b > 2.0f) b = 2.0f;
  return b;
}

void SetHoverCursor(int ro_cursor_type) { g_hover_cursor = ro_cursor_type; }
int TakeHoverCursor() {
  const int t = g_hover_cursor;
  g_hover_cursor = 0;
  return t;
}

// Pousse les 24 couleurs de style communes aux fenêtres RO (corps, texte, onglets,
// scrollbar transparente, table, popups clairs…). Partagé par BeginRoWindow et
// BeginRoDescWindow. Renvoie le nombre de PushStyleColor (à dépiler par End*).
static int PushSkinColors() {
  const ImU32 body = ImGui::ColorConvertFloat4ToU32(
      ImVec4(g_cfg.body_col[0], g_cfg.body_col[1], g_cfg.body_col[2],
             g_cfg.body_col[3]));
  const ImU32 border = ImGui::ColorConvertFloat4ToU32(
      ImVec4(g_cfg.border_col[0], g_cfg.border_col[1], g_cfg.border_col[2],
             g_cfg.border_col[3]));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, body);
  ImGui::PushStyleColor(ImGuiCol_TitleBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Border, border);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertFloat4ToU32(ImVec4(
                                           g_cfg.body_text[0], g_cfg.body_text[1],
                                           g_cfg.body_text[2], g_cfg.body_text[3])));
  ImGui::PushStyleColor(ImGuiCol_ResizeGrip, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, IM_COL32(0, 0, 0, 0));
  const ImU32 inputc = ImGui::ColorConvertFloat4ToU32(
      ImVec4(g_cfg.input_col[0], g_cfg.input_col[1], g_cfg.input_col[2],
             g_cfg.input_col[3]));
  auto lighten = [](ImU32 c, int d) {
    int r = ((c >> IM_COL32_R_SHIFT) & 0xFF) + d;
    int g = ((c >> IM_COL32_G_SHIFT) & 0xFF) + d;
    int b = ((c >> IM_COL32_B_SHIFT) & 0xFF) + d;
    int a = (c >> IM_COL32_A_SHIFT) & 0xFF;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return IM_COL32(r, g, b, a);
  };
  ImGui::PushStyleColor(ImGuiCol_FrameBg, inputc);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, lighten(inputc, 10));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, lighten(inputc, 22));
  const ImU32 tabc = ImGui::ColorConvertFloat4ToU32(
      ImVec4(g_cfg.tab_col[0], g_cfg.tab_col[1], g_cfg.tab_col[2],
             g_cfg.tab_col[3]));
  const ImU32 tabi = ImGui::ColorConvertFloat4ToU32(
      ImVec4(g_cfg.tab_inact[0], g_cfg.tab_inact[1], g_cfg.tab_inact[2],
             g_cfg.tab_inact[3]));
  ImGui::PushStyleColor(ImGuiCol_Tab, tabi);
  ImGui::PushStyleColor(ImGuiCol_TabHovered, lighten(tabi, 14));
  ImGui::PushStyleColor(ImGuiCol_TabSelected, tabc);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_TableHeaderBg,
                        ImGui::ColorConvertFloat4ToU32(ImVec4(
                            g_cfg.header_col[0], g_cfg.header_col[1],
                            g_cfg.header_col[2], g_cfg.header_col[3])));
  ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0x9C, 0xB8, 0xEA, 160));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0x9C, 0xB8, 0xEA, 110));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0x7E, 0xA0, 0xE0, 210));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0xF2, 0xF3, 0xF6, 255));
  return 24;
}

void SetNextWindowTitleBullet(const char* tooltip) {
  g_next_bullet = true;
  g_next_bullet_tip = tooltip;
}

bool TitleBulletClicked() { return g_bullet_clicked; }

void SetNextWindowBodyColor(unsigned int argb) {
  g_next_body_set = true;
  g_next_body_col = argb;
}

bool BeginRoWindow(const char* title, bool* p_open, int imgui_window_flags) {
  // Consommé quoi qu'il arrive : la demande ne doit pas fuiter sur la fenêtre
  // suivante si celle-ci n'est pas peinte (fenêtre masquée…).
  const bool bullet_btn = g_next_bullet;
  const char* bullet_tip = g_next_bullet_tip;
  g_next_bullet = false;
  g_next_bullet_tip = nullptr;
  g_bullet_clicked = false;
  const bool body_set = g_next_body_set;
  const unsigned int body_col = g_next_body_col;
  g_next_body_set = false;

  g_skin_active = true;

  // On garde la mécanique ImGui (drag/resize/collapse) mais on peint nous-mêmes la
  // barre de titre et les boutons système → title bar native transparente, close
  // natif désactivé (on dessine sys_close). p_open est géré manuellement.
  g_skin_colors = PushSkinColors();
  // Corps forcé par l'appelant (SetNextWindowBodyColor) : s'ajoute aux 24 pushes et
  // écrase WindowBg, comme le fait BeginRoDescWindow pour son fond blanc.
  if (body_set) {
    ImGui::PushStyleColor(ImGuiCol_WindowBg, body_col);
    ++g_skin_colors;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  // Arrondi bas fixe ~3px (le haut est couvert par l'art titre).
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_cfg.alpha);  // opacité globale
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);  // inputs arrondis ~3
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 13.0f);  // largeur pièces RO
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 2.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);

  // Hauteur de barre de titre ImGui = FontSize + FramePadding.y*2. On règle
  // FramePadding.y pour qu'elle vaille EXACTEMENT la hauteur de l'art (17px),
  // sinon l'art est étiré verticalement (plus haut que le natif, dégradé déformé).
  float pad_y = ((float)skin::kTitlebarLeft.h - ImGui::GetFontSize()) * 0.5f;
  if (pad_y < 0.0f) pad_y = 0.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      ImVec2(ImGui::GetStyle().FramePadding.x, pad_y));
  g_skin_vars = 9;

  // ⚠ Bullet cliquable : ImGui pose SON bouton de repli à gauche de la barre de
  // titre (title_bar_rect.Min.x + FramePadding.x), c'est-à-dire tout juste sous
  // notre bullet — il capte donc le clic et replie la fenêtre au lieu d'ouvrir la
  // config. On le supprime pour CETTE fenêtre en neutralisant sa position.
  // PAS via ImGuiWindowFlags_NoCollapse : ImGui force alors Collapsed=false a
  // chaque frame, ce qui casserait le bouton minimiser dessine par le skin.
  const ImGuiDir menu_btn_backup = ImGui::GetStyle().WindowMenuButtonPosition;
  if (bullet_btn) ImGui::GetStyle().WindowMenuButtonPosition = ImGuiDir_None;
  const bool open = ImGui::Begin(title, nullptr, imgui_window_flags);
  ImGui::GetStyle().WindowMenuButtonPosition = menu_btn_backup;
  RegisterEscapeWindow(p_open);

  // On dessine la barre de titre RO même quand la fenêtre est repliée (Begin
  // renvoie false dans ce cas) — sinon le titre replié garde le chrome ImGui.
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (w && !w->Hidden) {
    EnsureTex("basic_interface\\titlebar_left.bmp", skin::kTitlebarLeft, g_tl);
    EnsureTex("basic_interface\\titlebar_mid.bmp", skin::kTitlebarMid, g_tm);
    EnsureTex("basic_interface\\titlebar_right.bmp", skin::kTitlebarRight, g_tr);
    EnsureTex("basic_interface\\sys_close_off.bmp", skin::kSysCloseOff, g_close);
    EnsureTex("basic_interface\\sys_close_on.bmp", skin::kSysCloseOn, g_close_on);
    EnsureTex("basic_interface\\sys_mini_off.bmp", skin::kSysMiniOff, g_mini);
    EnsureTex("basic_interface\\sys_mini_on.bmp", skin::kSysMiniOn, g_mini_on);
    EnsureTex("basic_interface\\sys_base_off.bmp", skin::kSysBaseOff, g_base);
    if (bullet_btn)
      EnsureTex("basic_interface\\sys_base_on.bmp", skin::kSysBaseOn, g_base_on);

    // Repliée : le rect visible EST la barre de titre ; sinon TitleBarRect().
    const ImRect tb = w->Collapsed ? w->Rect() : w->TitleBarRect();
    ImDrawList* dl = w->DrawList;
    const float y0 = tb.Min.y, y1 = tb.Max.y;
    const float capL = (float)g_tl.w, capR = (float)g_tr.w;

    // Après Begin, la clip rect du draw list est réduite à la zone de contenu
    // (sous le titre) → tout dessin dans la barre de titre serait découpé.
    // On élargit la clip à la barre de titre le temps de la peindre.
    dl->PushClipRect(tb.Min, tb.Max, false);

    if (!g_tl.tex) {
      // Repli visible : textures pas encore prêtes / échec de création. Barre bleue
      // pleine (≠ chrome sombre par défaut) pour diagnostiquer d'un coup d'œil.
      dl->AddRectFilledMultiColor(tb.Min, tb.Max, IM_COL32(126, 158, 224, 255),
                                  IM_COL32(126, 158, 224, 255),
                                  IM_COL32(86, 122, 200, 255),
                                  IM_COL32(86, 122, 200, 255));
    }
    dl->AddCallback(ImCb_PointFilter, nullptr);
    BlitStretch(dl, g_tl, ImVec2(tb.Min.x, y0), ImVec2(tb.Min.x + capL, y1));
    BlitStretch(dl, g_tr, ImVec2(tb.Max.x - capR, y0), ImVec2(tb.Max.x, y1));
    BlitStretch(dl, g_tm, ImVec2(tb.Min.x + capL, y0), ImVec2(tb.Max.x - capR, y1));

    // Bullet sys_base devant le titre : décoratif (comme le natif RO), ou bouton
    // si SetNextWindowTitleBullet a été appelé — art « on » au survol, curseur
    // main, et le clic est remonté à l'appelant via TitleBulletClicked().
    const float base_sz = (float)g_base.w;  // 11
    const float base_x = tb.Min.x + 5.0f;
    const float base_y = y0 + (tb.GetHeight() - base_sz) * 0.5f;
    const ImVec2 base_tl(base_x, base_y);
    const ImVec2 base_br(base_x + base_sz, base_y + base_sz);
    bool bullet_hovered = false;
    if (bullet_btn) {
      // Cible élargie de 2px : 11px est trop petit pour viser confortablement.
      bullet_hovered = ImGui::IsMouseHoveringRect(
          ImVec2(base_tl.x - 2.0f, base_tl.y - 2.0f),
          ImVec2(base_br.x + 2.0f, base_br.y + 2.0f), false);
      if (bullet_hovered) {
        SetHoverCursor(kRoCursorHand);
        g_bullet_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
      }
    }
    const SkinTex& base_tex =
        (bullet_hovered && g_base_on.tex) ? g_base_on : g_base;
    if (base_tex.tex) BlitStretch(dl, base_tex, base_tl, base_br);
    const float text_x = base_x + base_sz + 4.0f;

    // Titre par-dessus (couleur configurable ; coupe le "##id").
    char nbuf[128];
    const char* end = ImGui::FindRenderedTextEnd(title);
    size_t n = (size_t)(end - title);
    if (n >= sizeof(nbuf)) n = sizeof(nbuf) - 1;
    memcpy(nbuf, title, n);
    nbuf[n] = '\0';
    const ImU32 title_tx = ImGui::ColorConvertFloat4ToU32(
        ImVec4(g_cfg.title_text[0], g_cfg.title_text[1], g_cfg.title_text[2],
               g_cfg.title_text[3] * g_cfg.alpha));  // suit l'opacité
    const ImVec2 ts = ImGui::CalcTextSize(nbuf);
    dl->AddText(ImVec2(text_x, y0 + (tb.GetHeight() - ts.y) * 0.5f - 1.5f),
                title_tx, nbuf);

    // Boutons système à droite : close (seulement si la fenêtre est fermable,
    // p_open != null) collé au bord droit ; mini à sa gauche, masqué si NoCollapse.
    const bool show_mini = !(imgui_window_flags & ImGuiWindowFlags_NoCollapse);
    const float by = y0 + (tb.GetHeight() - (float)g_close.h) * 0.5f;
    float bx = tb.Max.x - 4.0f;  // curseur depuis le bord droit
    bool close_clicked = false;
    if (p_open) {
      ImVec2 close_tl(bx - (float)g_close.w, by);
      close_clicked = SysButton(dl, g_close, g_close_on, close_tl);
      bx = close_tl.x - 2.0f;
    }
    bool mini_clicked = false;
    if (show_mini) {
      ImVec2 mini_tl(bx - (float)g_mini.w, by);
      mini_clicked = SysButton(dl, g_mini, g_mini_on, mini_tl);
    }
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    dl->PopClipRect();

    // Tooltip du bullet APRÈS le PopClipRect : ImGui::SetTooltip ouvre une autre
    // fenêtre (donc une autre draw list) — la peinture du titre doit être finie.
    if (bullet_hovered && bullet_tip && bullet_tip[0])
      ImGui::SetTooltip("%s", bullet_tip);

    if (close_clicked && p_open) *p_open = false;
    if (mini_clicked) ImGui::SetWindowCollapsed(w, !w->Collapsed);

    // Grip de resize RO en bas-à-droite (si redimensionnable). Le grip natif
    // ImGui reste actif pour le drag (juste rendu transparent) ; on peint l'image.
    if (!w->Collapsed && !(w->Flags & ImGuiWindowFlags_NoResize) &&
        !(w->Flags & ImGuiWindowFlags_AlwaysAutoResize)) {
      EnsureTex("btn_resize.bmp", skin::kBtnResize, g_resize);
      const float rw = (float)g_resize.w, rh = (float)g_resize.h;
      const ImVec2 br(w->Pos.x + w->Size.x - 2.0f, w->Pos.y + w->Size.y - 2.0f);
      const ImVec2 tl(br.x - rw, br.y - rh);
      dl->PushClipRect(tl, br, false);
      dl->AddCallback(ImCb_PointFilter, nullptr);
      BlitStretch(dl, g_resize, tl, br);
      dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
      dl->PopClipRect();
      // Curseur main au survol du grip (via le curseur RO natif).
      if (ImGui::IsMouseHoveringRect(tl, br, false))
        SetHoverCursor(kRoCursorHand);
    }
  }
  return open;
}

void EndRoWindow() {
  // Scrollbar RO peinte pendant que la fenêtre est encore courante. On repeint
  // la fenêtre ET ses descendantes (child windows + fenêtres internes de tables
  // ScrollY, ex. le storage) : leur scrollbar ImGui a été rendue transparente par
  // le style poussé, donc sans ça elle serait invisible.
  if (g_skin_active) {
    ImGuiWindow* main = ImGui::GetCurrentWindow();
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (main && g) {
      for (ImGuiWindow* cw : g->Windows) {
        if (cw && cw->Active && cw->ScrollbarY && cw->RootWindow == main)
          DrawRoScrollbar(cw);
      }
    }
    g_skin_active = false;
  }
  ImGui::End();
  if (g_skin_vars) {
    ImGui::PopStyleVar(g_skin_vars);
    g_skin_vars = 0;
  }
  if (g_skin_colors) {
    ImGui::PopStyleColor(g_skin_colors);
    g_skin_colors = 0;
  }
}

// Fenêtre de description : design distinct (barre de titre skill_upbar claire +
// cadre boîte sysbox), même config/couleurs/scrollbar que le reste du skin.
bool BeginRoDescWindow(const char* title, bool* p_open, int imgui_window_flags,
                       unsigned int title_shadow) {
  g_skin_active = true;

  g_skin_colors = PushSkinColors();
  // Desc : fond BLANC + bordure 1px c2c2c2 (continuité avec le titre). Ces 2
  // pushes s'ajoutent aux 24 de PushSkinColors et écrasent WindowBg/Border.
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(255, 255, 255, 255));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0xC2, 0xC2, 0xC2, 255));
  g_skin_colors += 2;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);  // bordure 1px c2c2c2
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_cfg.alpha);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
  // Scrollbar 13px + épaisseur du cadre sysbox (14px) réservée : ImGui garde le
  // contenu à l'écart du cadre ET la scrollbar (dessinée à 13px à gauche du slot,
  // cf. DrawRoScrollbar) se retrouve DANS le cadre au lieu de par-dessus le bord.
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,
                      6.0f + (float)skin::kSysboxLm.w);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 2.0f);
  // Contenu : côtés = épaisseur sysbox +2px, peu en haut/bas.
  const float e = (float)skin::kSysboxLm.w;  // 14
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(e + 2.0f, 7.0f));
  // Barre de titre = hauteur de l'art skill_upbar (20px).
  float pad_y = ((float)skin::kUpbarLeft.h - ImGui::GetFontSize()) * 0.5f;
  if (pad_y < 0.0f) pad_y = 0.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      ImVec2(ImGui::GetStyle().FramePadding.x, pad_y));
  g_skin_vars = 9;

  const bool open = ImGui::Begin(title, nullptr, imgui_window_flags);
  RegisterEscapeWindow(p_open);

  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (w && !w->Hidden) {
    EnsureTex("basic_interface\\skill_upbar_left.bmp", skin::kUpbarLeft, g_up_l);
    EnsureTex("basic_interface\\skill_upbar_mid.bmp", skin::kUpbarMid, g_up_m);
    EnsureTex("basic_interface\\skill_upbar_right.bmp", skin::kUpbarRight, g_up_r);
    EnsureTex("sysbox_lm.bmp", skin::kSysboxLm, g_sb_lm);
    EnsureTex("sysbox_rm.bmp", skin::kSysboxRm, g_sb_rm);
    EnsureTex("sysbox_ld.bmp", skin::kSysboxLd, g_sb_ld);
    EnsureTex("sysbox_md.bmp", skin::kSysboxMd, g_sb_md);
    EnsureTex("sysbox_rd.bmp", skin::kSysboxRd, g_sb_rd);

    const ImRect tb = w->Collapsed ? w->Rect() : w->TitleBarRect();
    const ImRect wr = w->Rect();
    ImDrawList* dl = w->DrawList;
    const float y0 = tb.Min.y, y1 = tb.Max.y;
    const float ucL = (float)g_up_l.w, ucR = (float)g_up_r.w;

    dl->PushClipRect(wr.Min, wr.Max, false);  // couvre toute la fenêtre (titre+cadre)
    dl->AddCallback(ImCb_PointFilter, nullptr);
    // Barre de titre skill_upbar (3-slice).
    BlitStretch(dl, g_up_l, ImVec2(tb.Min.x, y0), ImVec2(tb.Min.x + ucL, y1));
    BlitStretch(dl, g_up_r, ImVec2(tb.Max.x - ucR, y0), ImVec2(tb.Max.x, y1));
    BlitStretch(dl, g_up_m, ImVec2(tb.Min.x + ucL, y0), ImVec2(tb.Max.x - ucR, y1));
    // Cadre sysbox (côtés + bas ; le haut est couvert par le titre).
    if (!w->Collapsed) {
      const float fx0 = wr.Min.x, fx1 = wr.Max.x, fby = wr.Max.y;
      BlitStretch(dl, g_sb_lm, ImVec2(fx0, y1), ImVec2(fx0 + e, fby - e));
      BlitStretch(dl, g_sb_rm, ImVec2(fx1 - e, y1), ImVec2(fx1, fby - e));
      BlitStretch(dl, g_sb_ld, ImVec2(fx0, fby - e), ImVec2(fx0 + e, fby));
      BlitStretch(dl, g_sb_rd, ImVec2(fx1 - e, fby - e), ImVec2(fx1, fby));
      BlitStretch(dl, g_sb_md, ImVec2(fx0 + e, fby - e), ImVec2(fx1 - e, fby));
    }

    // Titre (couleur configurable, coupe le "##id").
    char nbuf[128];
    const char* end = ImGui::FindRenderedTextEnd(title);
    size_t n = (size_t)(end - title);
    if (n >= sizeof(nbuf)) n = sizeof(nbuf) - 1;
    memcpy(nbuf, title, n);
    nbuf[n] = '\0';
    const ImU32 ttx = ImGui::ColorConvertFloat4ToU32(
        ImVec4(g_cfg.title_text[0], g_cfg.title_text[1], g_cfg.title_text[2],
               g_cfg.title_text[3] * g_cfg.alpha));
    const ImVec2 ts = ImGui::CalcTextSize(nbuf);
    const ImVec2 tpos(tb.Min.x + 8.0f,
                      y0 + (tb.GetHeight() - ts.y) * 0.5f - 1.0f);
    // Ombre optionnelle du titre (ex. rouge 0x5050fa pour un item cassé), décalée
    // +1,+1 sous le texte du titre.
    if (title_shadow)
      dl->AddText(ImVec2(tpos.x + 1.0f, tpos.y + 1.0f), title_shadow, nbuf);
    dl->AddText(tpos, ttx, nbuf);

    // Bouton close (seulement si fermable), collé au bord droit du titre.
    bool close_clicked = false;
    if (p_open) {
      EnsureTex("basic_interface\\sys_close_off.bmp", skin::kSysCloseOff, g_close);
      EnsureTex("basic_interface\\sys_close_on.bmp", skin::kSysCloseOn, g_close_on);
      const float by = y0 + (tb.GetHeight() - (float)g_close.h) * 0.5f;
      ImVec2 ctl(tb.Max.x - (float)g_close.w - 5.0f, by);
      close_clicked = SysButton(dl, g_close, g_close_on, ctl);
    }
    // Grip de resize RO dans le coin bas-droite (si redimensionnable). Le grip
    // ImGui natif reste actif pour le drag (rendu transparent) ; on peint l'image.
    if (!w->Collapsed && !(w->Flags & ImGuiWindowFlags_NoResize) &&
        !(w->Flags & ImGuiWindowFlags_AlwaysAutoResize)) {
      EnsureTex("btn_resize.bmp", skin::kBtnResize, g_resize);
      const float rw = (float)g_resize.w, rh = (float)g_resize.h;
      const ImVec2 rbr(wr.Max.x - 4.0f, wr.Max.y - 4.0f);
      const ImVec2 rtl(rbr.x - rw, rbr.y - rh);
      BlitStretch(dl, g_resize, rtl, rbr);
      if (ImGui::IsMouseHoveringRect(rtl, rbr, false))
        SetHoverCursor(kRoCursorHand);
    }
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    dl->PopClipRect();

    if (close_clicked && p_open) *p_open = false;
  }
  return open;
}

void EndRoDescWindow() { EndRoWindow(); }  // même teardown (scrollbar + pop)

// Panneau de description SANS barre de titre : cadre boîte sysbox complet (9-slice
// avec le haut), fond blanc + bordure. Pour les sous-panneaux (cartes, options).
bool BeginRoDescPanel(const char* id, int imgui_window_flags) {
  g_skin_active = true;
  g_skin_colors = PushSkinColors();
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(255, 255, 255, 255));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0xC2, 0xC2, 0xC2, 255));
  g_skin_colors += 2;
  // Bordure 1px ARRONDIE (comme BeginRoDescWindow) EN PLUS du cadre sysbox : donne
  // aux sous-panneaux le même liseré arrondi que la fenêtre de description parente
  // (le sysbox seul rend un cadre carré/parfois non visible sur ces petits panneaux).
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_cfg.alpha);
  const float e = (float)skin::kSysboxLm.w;  // 14
  // Côtés = e+2 (marge intérieure vs cadre sysbox) ; haut/bas = 6px (panneaux
  // cartes/options COMPACTS, demandé).
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(e + 2.0f, 6.0f));
  g_skin_vars = 4;

  const bool open = ImGui::Begin(id, nullptr, imgui_window_flags);
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (open && w && !w->Hidden && !w->Collapsed) {
    EnsureTex("sysbox_lu.bmp", skin::kSysboxLu, g_sb_lu);
    EnsureTex("sysbox_mu.bmp", skin::kSysboxMu, g_sb_mu);
    EnsureTex("sysbox_ru.bmp", skin::kSysboxRu, g_sb_ru);
    EnsureTex("sysbox_lm.bmp", skin::kSysboxLm, g_sb_lm);
    EnsureTex("sysbox_rm.bmp", skin::kSysboxRm, g_sb_rm);
    EnsureTex("sysbox_ld.bmp", skin::kSysboxLd, g_sb_ld);
    EnsureTex("sysbox_md.bmp", skin::kSysboxMd, g_sb_md);
    EnsureTex("sysbox_rd.bmp", skin::kSysboxRd, g_sb_rd);
    const ImRect wr = w->Rect();
    ImDrawList* dl = w->DrawList;
    const float fx0 = wr.Min.x, fy0 = wr.Min.y, fx1 = wr.Max.x, fy1 = wr.Max.y;
    dl->PushClipRect(wr.Min, wr.Max, false);
    dl->AddCallback(ImCb_PointFilter, nullptr);
    BlitStretch(dl, g_sb_lu, ImVec2(fx0, fy0), ImVec2(fx0 + e, fy0 + e));
    BlitStretch(dl, g_sb_ru, ImVec2(fx1 - e, fy0), ImVec2(fx1, fy0 + e));
    BlitStretch(dl, g_sb_ld, ImVec2(fx0, fy1 - e), ImVec2(fx0 + e, fy1));
    BlitStretch(dl, g_sb_rd, ImVec2(fx1 - e, fy1 - e), ImVec2(fx1, fy1));
    BlitStretch(dl, g_sb_mu, ImVec2(fx0 + e, fy0), ImVec2(fx1 - e, fy0 + e));
    BlitStretch(dl, g_sb_md, ImVec2(fx0 + e, fy1 - e), ImVec2(fx1 - e, fy1));
    BlitStretch(dl, g_sb_lm, ImVec2(fx0, fy0 + e), ImVec2(fx0 + e, fy1 - e));
    BlitStretch(dl, g_sb_rm, ImVec2(fx1 - e, fy0 + e), ImVec2(fx1, fy1 - e));
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    dl->PopClipRect();
  }
  return open;
}

void EndRoDescPanel() { EndRoWindow(); }

float DescPanelEdge() { return (float)skin::kSysboxLm.w; }

// Cadre « panneau desc » (fond clair + sysbox 9-slice) dessiné à la main dans un
// rect arbitraire, sur un ImDrawList arbitraire. Même art que BeginRoDescPanel,
// mais sans créer de fenêtre ImGui -> l'appelant peut le poser sur la draw list
// de sa fenêtre parente (=> suit son z-order). L'appelant gère le clip global.
void DrawDescPanelFrame(ImDrawList* dl, float x0, float y0, float x1, float y1,
                        bool fill_bg) {
  if (!dl || x1 <= x0 || y1 <= y0) return;
  const ImVec2 p0(x0, y0), p1(x1, y1);
  // Fond clair (suit alpha/luminosité du skin comme les autres pièces main). Sauté
  // quand l'appelant a déjà peint un fond ARRONDI : ce rect-ci est à angles droits
  // et recouvrirait ses coins.
  if (fill_bg)
    dl->AddRectFilled(p0, p1, ApplySkinTint(IM_COL32(255, 255, 255, 255)), 0.0f);
  EnsureTex("sysbox_lu.bmp", skin::kSysboxLu, g_sb_lu);
  EnsureTex("sysbox_mu.bmp", skin::kSysboxMu, g_sb_mu);
  EnsureTex("sysbox_ru.bmp", skin::kSysboxRu, g_sb_ru);
  EnsureTex("sysbox_lm.bmp", skin::kSysboxLm, g_sb_lm);
  EnsureTex("sysbox_rm.bmp", skin::kSysboxRm, g_sb_rm);
  EnsureTex("sysbox_ld.bmp", skin::kSysboxLd, g_sb_ld);
  EnsureTex("sysbox_md.bmp", skin::kSysboxMd, g_sb_md);
  EnsureTex("sysbox_rd.bmp", skin::kSysboxRd, g_sb_rd);
  const float e = (float)skin::kSysboxLm.w;
  dl->PushClipRect(p0, p1, false);
  dl->AddCallback(ImCb_PointFilter, nullptr);
  BlitStretch(dl, g_sb_lu, ImVec2(x0, y0), ImVec2(x0 + e, y0 + e));
  BlitStretch(dl, g_sb_ru, ImVec2(x1 - e, y0), ImVec2(x1, y0 + e));
  BlitStretch(dl, g_sb_ld, ImVec2(x0, y1 - e), ImVec2(x0 + e, y1));
  BlitStretch(dl, g_sb_rd, ImVec2(x1 - e, y1 - e), ImVec2(x1, y1));
  BlitStretch(dl, g_sb_mu, ImVec2(x0 + e, y0), ImVec2(x1 - e, y0 + e));
  BlitStretch(dl, g_sb_md, ImVec2(x0 + e, y1 - e), ImVec2(x1 - e, y1));
  BlitStretch(dl, g_sb_lm, ImVec2(x0, y0 + e), ImVec2(x0 + e, y1 - e));
  BlitStretch(dl, g_sb_rm, ImVec2(x1 - e, y0 + e), ImVec2(x1, y1 - e));
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  dl->PopClipRect();
}

bool RoButton(const char* label, float w, float h) {
  EnsureTex("basic_interface\\btn_out_left.bmp",    skin::kBtnOutLeft,    g_btn_out_l);
  EnsureTex("basic_interface\\btn_out_mid.bmp",     skin::kBtnOutMid,     g_btn_out_m);
  EnsureTex("basic_interface\\btn_out_right.bmp",   skin::kBtnOutRight,   g_btn_out_r);
  EnsureTex("basic_interface\\btn_over_left.bmp",   skin::kBtnOverLeft,   g_btn_over_l);
  EnsureTex("basic_interface\\btn_over_mid.bmp",    skin::kBtnOverMid,    g_btn_over_m);
  EnsureTex("basic_interface\\btn_over_right.bmp",  skin::kBtnOverRight,  g_btn_over_r);
  EnsureTex("basic_interface\\btn_press_left.bmp",  skin::kBtnPressLeft,  g_btn_press_l);
  EnsureTex("basic_interface\\btn_press_mid.bmp",   skin::kBtnPressMid,   g_btn_press_m);
  EnsureTex("basic_interface\\btn_press_right.bmp", skin::kBtnPressRight, g_btn_press_r);

  const float capL = (float)skin::kBtnOutLeft.w;
  const float capR = (float)skin::kBtnOutRight.w;
  const float nativeH = (float)skin::kBtnOutLeft.h;
  const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
  if (w <= 0.0f) w = ts.x + capL + capR + 12.0f;
  if (h <= 0.0f) h = nativeH;

  ImGui::PushID(label);
  const bool clicked = ImGui::InvisibleButton("##rb", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  if (hovered) SetHoverCursor(kRoCursorHand);
  const ImVec2 p0 = ImGui::GetItemRectMin();
  const ImVec2 p1 = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // Etat desactive (BeginDisabled) : ImGui ne modifie PAS le visuel des widgets
  // dessines main -> on grise nous-memes (art estompe + texte grise).
  const bool disabled =
      ImGui::GetCurrentContext() &&
      (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
  const ImU32 tint = disabled ? IM_COL32(255, 255, 255, 90) : IM_COL32_WHITE;

  const SkinTex *l, *m, *r;
  if (held) { l = &g_btn_press_l; m = &g_btn_press_m; r = &g_btn_press_r; }
  else if (hovered) { l = &g_btn_over_l; m = &g_btn_over_m; r = &g_btn_over_r; }
  else { l = &g_btn_out_l; m = &g_btn_out_m; r = &g_btn_out_r; }

  if (l->tex) {
    dl->AddCallback(ImCb_PointFilter, nullptr);
    BlitStretch(dl, *l, p0, ImVec2(p0.x + capL, p1.y), tint);
    BlitStretch(dl, *r, ImVec2(p1.x - capR, p0.y), p1, tint);
    BlitStretch(dl, *m, ImVec2(p0.x + capL, p0.y), ImVec2(p1.x - capR, p1.y), tint);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  } else {
    dl->AddRectFilled(p0, p1,
                      disabled ? IM_COL32(210, 216, 228, 110)
                               : IM_COL32(210, 216, 228, 255),
                      2.0f);
    dl->AddRect(p0, p1, IM_COL32(96, 112, 152, 255), 2.0f);
  }

  const ImVec2 tp(p0.x + (w - ts.x) * 0.5f,
                  p0.y + (h - ts.y) * 0.5f + (held ? 1.0f : 0.0f));
  dl->AddText(tp,
              disabled ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
                       : ImGui::GetColorU32(ImGuiCol_Text),
              label, ImGui::FindRenderedTextEnd(label));
  ImGui::PopID();
  return clicked;
}
// Petit bouton (ex. pour les + - x ) : même design que RoButton mais plus petit
bool RoSmallButton(const char* label, float w, float h) {
  EnsureTex("basic_interface\\sbtn_out_left.bmp",    skin::ksBtnOutLeft,    g_sbtn_out_l);
  EnsureTex("basic_interface\\sbtn_out_mid.bmp",     skin::ksBtnOutMid,     g_sbtn_out_m);
  EnsureTex("basic_interface\\sbtn_out_right.bmp",   skin::ksBtnOutRight,   g_sbtn_out_r);
  EnsureTex("basic_interface\\sbtn_over_left.bmp",   skin::ksBtnOverLeft,   g_sbtn_over_l);
  EnsureTex("basic_interface\\sbtn_over_mid.bmp",    skin::ksBtnOverMid,    g_sbtn_over_m);
  EnsureTex("basic_interface\\sbtn_over_right.bmp",  skin::ksBtnOverRight,  g_sbtn_over_r);
  EnsureTex("basic_interface\\sbtn_press_left.bmp",  skin::ksBtnPressLeft,  g_sbtn_press_l);
  EnsureTex("basic_interface\\sbtn_press_mid.bmp",   skin::ksBtnPressMid,   g_sbtn_press_m);
  EnsureTex("basic_interface\\sbtn_press_right.bmp", skin::ksBtnPressRight, g_sbtn_press_r);

  const float capL = (float)skin::ksBtnOutLeft.w;
  const float capR = (float)skin::ksBtnOutRight.w;
  const float nativeH = (float)skin::ksBtnOutLeft.h;
  const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
  if (w <= 0.0f) w = ts.x + capL + capR; // +12px pour RoButton, pas pour le petit bouton
  if (h <= 0.0f) h = nativeH;

  ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 3.0f); // décale le bouton vers la gauche
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f); // décale le bouton vers le bas pour mieux centrer le bouton sur la ligne

  ImGui::PushID(label);
  const bool clicked = ImGui::InvisibleButton("##rb", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  if (hovered) SetHoverCursor(kRoCursorHand);
  const ImVec2 p0 = ImGui::GetItemRectMin();
  const ImVec2 p1 = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // Etat desactive (BeginDisabled) : ImGui ne modifie PAS le visuel des widgets
  // dessines main -> on grise nous-memes (art estompe + texte grise).
  const bool disabled =
      ImGui::GetCurrentContext() &&
      (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
  const ImU32 tint = disabled ? IM_COL32(255, 255, 255, 90) : IM_COL32_WHITE;

  const SkinTex *l, *m, *r;
  if (held) { l = &g_sbtn_press_l; m = &g_sbtn_press_m; r = &g_sbtn_press_r; }
  else if (hovered) { l = &g_sbtn_over_l; m = &g_sbtn_over_m; r = &g_sbtn_over_r; }
  else { l = &g_sbtn_out_l; m = &g_sbtn_out_m; r = &g_sbtn_out_r; }

  if (l->tex) {
    dl->AddCallback(ImCb_PointFilter, nullptr);
    BlitStretch(dl, *l, p0, ImVec2(p0.x + capL, p1.y), tint);
    BlitStretch(dl, *r, ImVec2(p1.x - capR, p0.y), p1, tint);
    BlitStretch(dl, *m, ImVec2(p0.x + capL, p0.y), ImVec2(p1.x - capR, p1.y), tint);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  } else {
    dl->AddRectFilled(p0, p1,
                      disabled ? IM_COL32(210, 216, 228, 110)
                               : IM_COL32(210, 216, 228, 255),
                      2.0f);
    dl->AddRect(p0, p1, IM_COL32(96, 112, 152, 255), 2.0f);
  }

  const ImVec2 tp(p0.x + (w - ts.x) * 0.5f,
                  p0.y + (h - ts.y) * 0.5f + (held ? 0.0f : 0.0f) - 2.0f);  // -2 pour centre le texte correctement dans la case
  dl->AddText(tp,
              disabled ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
                       : ImGui::GetColorU32(ImGuiCol_Text),
              label, ImGui::FindRenderedTextEnd(label));
  ImGui::PopID();
  return clicked;
}

bool RoCheckbox(const char* label, bool* v) {
  if (!v) return false;
  EnsureTex("checkbox_0.bmp", skin::kCheckbox0, g_cb0);
  EnsureTex("checkbox_1.bmp", skin::kCheckbox1, g_cb1);
  const float sz = (float)skin::kCheckbox0.w;  // 10x10
  const float gapx = 4.0f;
  ImGui::PushID(label);
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
  const float h = sz > ts.y ? sz : ts.y;
  const bool pressed = ImGui::InvisibleButton("##cb", ImVec2(sz + gapx + ts.x, h));
  if (ImGui::IsItemHovered()) SetHoverCursor(kRoCursorHand);
  if (pressed) *v = !*v;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Position arrondie à l'entier : sinon (h-sz)/2 fractionnaire + sampling POINT
  // coupe la dernière ligne de pixels (bas de la case « manquant »).
  // +2 : la case et le label sont posés sur la ligne de base naturelle (haut de
  // ligne) plutôt que 2px au-dessus, sinon le « (?) » d'un HelpMarker en SameLine
  // (dessiné à start.y) se retrouve décalé sous le label.
  const ImVec2 bmin(ImFloor(start.x), ImFloor(start.y + (h - sz) * 0.5f + 2.0f));
  const ImVec2 bmax(bmin.x + sz, bmin.y + sz);
  if (g_cb0.tex) {
    dl->AddCallback(ImCb_PointFilter, nullptr);
    BlitStretch(dl, *v ? g_cb1 : g_cb0, bmin, bmax);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  } else {
    dl->AddRect(bmin, bmax, IM_COL32(96, 112, 152, 255));
    if (*v) dl->AddRectFilled(ImVec2(bmin.x + 2, bmin.y + 2),
                              ImVec2(bmax.x - 2, bmax.y - 2),
                              IM_COL32(96, 112, 152, 255));
  }
  dl->AddText(ImVec2(start.x + sz + gapx, start.y + (h - ts.y) * 0.5f), // aligné case + HelpMarker (voir bmin)
              ImGui::GetColorU32(ImGuiCol_Text), label,
              ImGui::FindRenderedTextEnd(label));
  ImGui::PopID();
  return pressed;
}

// ── Slider = scrollbar RO horizontale ─────────────────────────────────────────
// RO n'a pas de « slider » : le vocabulaire visuel équivalent est la scrollbar
// HORIZONTALE du client — piste/flèches scroll1left|mid|right + thumb
// scroll1bar_left|mid|right. On réutilise EXACTEMENT ces pièces (aucune rotation :
// elles sont déjà horizontales). Le comportement reste celui d'ImGui
// (SliderBehavior + Ctrl+clic = saisie directe) : seules l'interaction des flèches
// et la peinture sont à nous.
// Les tailles ne sont pas dans ro_skin_blobs (pièces non utilisées par ailleurs) :
// on charge via EnsureTexClient et on lit w/h de la texture, avec un repli 13/4px
// (dimensions de la scrollbar verticale) tant qu'elle n'est pas prête.
static bool RoSliderScalar(const char* label, ImGuiDataType dt, void* p_data,
                           const void* p_min, const void* p_max,
                           const char* format, ImGuiSliderFlags flags,
                           float arrow_step) {
  ImGuiWindow* win = ImGui::GetCurrentWindow();
  if (!win || win->SkipItems) return false;
  ImGuiContext& g = *ImGui::GetCurrentContext();
  const ImGuiStyle& style = g.Style;
  const ImGuiID id = win->GetID(label);

  EnsureTexClient("scroll1left.bmp", g_s1l);
  EnsureTexClient("scroll1mid.bmp", g_s1m);
  EnsureTexClient("scroll1right.bmp", g_s1r);
  EnsureTexClient("scroll1bar_left.bmp", g_s1bar_l);
  EnsureTexClient("scroll1bar_mid.bmp", g_s1bar_m);
  EnsureTexClient("scroll1bar_right.bmp", g_s1bar_r);

  if (format == nullptr) format = ImGui::DataTypeGetInfo(dt)->PrintFmt;
  char value_buf[64];
  const char* value_end =
      value_buf + ImGui::DataTypeFormatString(value_buf, IM_COUNTOF(value_buf),
                                              dt, p_data, format);

  const char* label_end = ImGui::FindRenderedTextEnd(label);
  const ImVec2 label_size = ImGui::CalcTextSize(label, label_end, false);
  // La valeur est affichée À DROITE de la barre (une scrollbar RO fait 13px de
  // haut : le texte n'y tient pas centré comme sur un slider ImGui). Largeur
  // réservée avec un plancher, sinon la barre se raccourcit/rallonge à chaque
  // changement de nombre de chiffres (barre qui « respire »).
  const float value_w =
      ImMax(ImGui::CalcTextSize(value_buf, value_end).x,
            ImGui::CalcTextSize("000000").x);
  const float barh = g_s1l.tex ? (float)g_s1l.h : 13.0f;  // hauteur de l'art
  const float w = ImGui::CalcItemWidth();
  const float frame_h =
      ImMax(barh, label_size.y + style.FramePadding.y * 2.0f);
  // Additions écrites à la main : les opérateurs ImVec2 (IMGUI_DEFINE_MATH_OPERATORS)
  // ne sont pas activés dans cette unité de compilation.
  const ImVec2 pos = win->DC.CursorPos;
  const ImRect frame_bb(pos, ImVec2(pos.x + w, pos.y + frame_h));
  const float label_extra =
      label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f;
  const ImRect total_bb(frame_bb.Min,
                        ImVec2(frame_bb.Max.x + label_extra, frame_bb.Max.y));

  const bool temp_input_allowed = (flags & ImGuiSliderFlags_NoInput) == 0;
  ImGui::ItemSize(total_bb, style.FramePadding.y);
  if (!ImGui::ItemAdd(total_bb, id, &frame_bb,
                      temp_input_allowed ? ImGuiItemFlags_Inputable : 0))
    return false;

  // Géométrie : barre (hors zone valeur) = flèche gauche + piste + flèche droite.
  const float bary = ImFloor(frame_bb.Min.y + (frame_h - barh) * 0.5f);
  const ImRect bar(frame_bb.Min.x, bary,
                   frame_bb.Max.x - value_w - style.ItemInnerSpacing.x,
                   bary + barh);
  const float arrow = g_s1l.tex ? (float)g_s1l.w : barh;
  const bool has_arrows = (bar.GetWidth() > arrow * 2.0f + 8.0f);
  const ImRect track(has_arrows ? bar.Min.x + arrow : bar.Min.x, bar.Min.y,
                     has_arrows ? bar.Max.x - arrow : bar.Max.x, bar.Max.y);

  const bool hovered = ImGui::ItemHoverable(frame_bb, id, g.LastItemData.ItemFlags);
  if (hovered) SetHoverCursor(kRoCursorHand);
  const bool over_left =
      has_arrows && hovered &&
      ImGui::IsMouseHoveringRect(bar.Min, ImVec2(track.Min.x, bar.Max.y), false);
  const bool over_right =
      has_arrows && hovered &&
      ImGui::IsMouseHoveringRect(ImVec2(track.Max.x, bar.Min.y), bar.Max, false);

  // Activation (calquée sur ImGui::SliderScalar), MAIS un clic sur une flèche ne
  // saisit pas le thumb : il ne doit faire qu'un pas.
  bool temp_input_is_active = temp_input_allowed && ImGui::TempInputIsActive(id);
  if (!temp_input_is_active) {
    const bool clicked = hovered && !over_left && !over_right &&
                         ImGui::IsMouseClicked(0, ImGuiInputFlags_None, id);
    const bool make_active = (clicked || g.NavActivateId == id);
    if (make_active && clicked) ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id);
    if (make_active && temp_input_allowed)
      if ((clicked && g.IO.KeyCtrl) ||
          (g.NavActivateId == id && (g.NavActivateFlags & ImGuiActivateFlags_PreferInput)))
        temp_input_is_active = true;
    if (make_active && !temp_input_is_active) {
      ImGui::SetActiveID(id, win);
      ImGui::SetFocusID(id, win);
      ImGui::FocusWindow(win);
      g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
    }
  }
  if (temp_input_is_active) {  // Ctrl+clic : saisie directe
    const bool clamp_enabled = (flags & ImGuiSliderFlags_ClampOnInput) != 0;
    return ImGui::TempInputScalar(frame_bb, id, label, dt, p_data, format,
                                  clamp_enabled ? p_min : nullptr,
                                  clamp_enabled ? p_max : nullptr);
  }

  // Flèches : ajustement À L'UNITÉ (même pas que la molette : 1 pour un entier,
  // 0.01 pour un flottant, sauf pas explicite ; Shift = pas ×10). Clic = 1 pas,
  // maintien = répétition, comme la scrollbar native.
  bool value_changed = false;
  if ((over_left || over_right) &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left, /*repeat=*/true)) {
    const float dir = over_left ? -1.0f : 1.0f;
    const float shift = ImGui::GetIO().KeyShift ? 10.0f : 1.0f;
    if (dt == ImGuiDataType_Float) {
      float* v = (float*)p_data;
      const float lo = *(const float*)p_min, hi = *(const float*)p_max;
      const float st = (arrow_step > 0.0f ? arrow_step : 0.01f) * shift;
      const float nv = ImClamp(*v + dir * st, lo, hi);
      if (nv != *v) { *v = nv; value_changed = true; }
    } else if (dt == ImGuiDataType_S32) {
      int* v = (int*)p_data;
      const int lo = *(const int*)p_min, hi = *(const int*)p_max;
      int st = (int)(arrow_step > 0.0f ? arrow_step : 1.0f) * (int)shift;
      if (st <= 0) st = 1;  // pas mini = l'unité
      const int nv = ImClamp(*v + (int)dir * st, lo, hi);
      if (nv != *v) { *v = nv; value_changed = true; }
    }
  }

  // Comportement slider sur la PISTE (entre les flèches) : le thumb ne passe
  // jamais sous une flèche, comme sur une scrollbar.
  ImRect grab_bb;
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 16.0f);  // thumb « scrollbar »
  if (ImGui::SliderBehavior(track, id, dt, p_data, p_min, p_max, format, flags,
                            &grab_bb))
    value_changed = true;
  ImGui::PopStyleVar();
  if (value_changed) ImGui::MarkItemEdited(id);

  // ── Dessin ──
  ImDrawList* dl = win->DrawList;
  if (g_s1m.tex) {
    dl->AddCallback(ImCb_PointFilter, nullptr);
    // Piste débordant de 2px sous les flèches -> jointure sans trou (cf. la
    // scrollbar verticale).
    BlitStretch(dl, g_s1m, ImVec2(track.Min.x - 2.0f, bar.Min.y),
                ImVec2(track.Max.x + 2.0f, bar.Max.y));
    if (has_arrows) {
      BlitStretch(dl, g_s1l, bar.Min, ImVec2(track.Min.x, bar.Max.y));
      BlitStretch(dl, g_s1r, ImVec2(track.Max.x, bar.Min.y), bar.Max);
    }
    const float cap = g_s1bar_l.tex ? (float)g_s1bar_l.w : 4.0f;
    const float gx0 = ImFloor(grab_bb.Min.x), gx1 = ImFloor(grab_bb.Max.x);
    if (gx1 > gx0) {
      BlitStretch(dl, g_s1bar_l, ImVec2(gx0, bar.Min.y),
                  ImVec2(gx0 + cap, bar.Max.y));
      BlitStretch(dl, g_s1bar_r, ImVec2(gx1 - cap, bar.Min.y),
                  ImVec2(gx1, bar.Max.y));
      if (gx1 - gx0 > cap * 2.0f)
        BlitStretch(dl, g_s1bar_m, ImVec2(gx0 + cap, bar.Min.y),
                    ImVec2(gx1 - cap, bar.Max.y));
    }
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  } else {  // repli : textures pas (encore) disponibles
    dl->AddRectFilled(bar.Min, bar.Max, ImGui::GetColorU32(ImGuiCol_FrameBg));
    dl->AddRect(bar.Min, bar.Max, ImGui::GetColorU32(ImGuiCol_Border));
    if (grab_bb.Max.x > grab_bb.Min.x)
      dl->AddRectFilled(ImVec2(grab_bb.Min.x, bar.Min.y),
                        ImVec2(grab_bb.Max.x, bar.Max.y),
                        ImGui::GetColorU32(ImGuiCol_SliderGrab), 2.0f);
  }

  // Valeur (alignée à droite) puis label, comme un slider ImGui.
  const ImVec2 vs = ImGui::CalcTextSize(value_buf, value_end);
  dl->AddText(ImVec2(frame_bb.Max.x - vs.x,
                     frame_bb.Min.y + (frame_h - vs.y) * 0.5f),
              ImGui::GetColorU32(ImGuiCol_Text), value_buf, value_end);
  if (label_size.x > 0.0f)
    ImGui::RenderText(ImVec2(frame_bb.Max.x + style.ItemInnerSpacing.x,
                             frame_bb.Min.y + style.FramePadding.y),
                      label, label_end, false);
  return value_changed;
}

bool RoSliderFloat(const char* label, float* v, float lo, float hi,
                   const char* format, float arrow_step, int flags) {
  return RoSliderScalar(label, ImGuiDataType_Float, v, &lo, &hi, format, flags,
                        arrow_step);
}

bool RoSliderInt(const char* label, int* v, int lo, int hi, const char* format,
                 int arrow_step, int flags) {
  return RoSliderScalar(label, ImGuiDataType_S32, v, &lo, &hi, format, flags,
                        (float)arrow_step);
}

bool RoBeginCombo(const char* label, const char* preview_value) {
  EnsureTexClient("basic_interface\\txtbox_btn_a.bmp", g_tb_btn_a);
  EnsureTexClient("basic_interface\\txtbox_btn_b.bmp", g_tb_btn_b);
  EnsureTexClient("basic_interface\\txtbox_btn_c.bmp", g_tb_btn_c);

  const float arrow_w = g_tb_btn_a.tex ? (float)g_tb_btn_a.w : 16.0f;
  const float h = g_tb_btn_a.tex ? (float)g_tb_btn_a.h : ImGui::GetFrameHeight();
  const float w = ImGui::CalcItemWidth();

  ImGui::PushID(label);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##rcb", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  if (hovered) SetHoverCursor(kRoCursorHand);
  const ImVec2 p1(p0.x + w, p0.y + h);
  const ImVec2 arrowMin(p1.x - arrow_w, p0.y);
  ImDrawList* dl = ImGui::GetWindowDrawList();

  const RoSkinConfig& c = g_cfg;
  // Etat desactive (BeginDisabled) : ImGui ne modifie PAS les widgets dessines
  // main (ColorConvertFloat4ToU32 ignore style.Alpha) -> on grise nous-memes.
  const bool disabled =
      ImGui::GetCurrentContext() &&
      (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
  const float da = disabled ? 0.4f : 1.0f;
  const auto U32 = [da](const float* a) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(a[0], a[1], a[2], a[3] * da));
  };
  // Champ : fond « input » + bordure (le txtbox natif est une simple boîte bordée).
  dl->AddRectFilled(p0, ImVec2(arrowMin.x, p1.y), U32(c.input_col));
  dl->AddRect(p0, p1, U32(c.border_col));
  // Bouton flèche : texture native txtbox_btn (états normal/survol/pressé), teintée
  // (alpha réduit) quand désactivé pour signaler l'état grisé.
  const ImU32 tint = disabled ? IM_COL32(255, 255, 255, 90) : IM_COL32_WHITE;
  const SkinTex& btn = held ? g_tb_btn_c : (hovered ? g_tb_btn_b : g_tb_btn_a);
  if (btn.tex) {
    dl->AddCallback(ImCb_PointFilter, nullptr);
    BlitStretch(dl, btn, arrowMin, p1, tint);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  } else {  // repli : triangle vers le bas
    const float cx = (arrowMin.x + p1.x) * 0.5f, cy = (p0.y + p1.y) * 0.5f;
    dl->AddTriangleFilled(ImVec2(cx - 3, cy - 2), ImVec2(cx + 3, cy - 2),
                          ImVec2(cx, cy + 3), U32(c.body_text));
  }
  // Texte de sélection (clippé au champ).
  if (preview_value && preview_value[0]) {
    const char* end = ImGui::FindRenderedTextEnd(preview_value);
    const float th = ImGui::CalcTextSize(preview_value, end).y;
    dl->PushClipRect(p0, ImVec2(arrowMin.x, p1.y), true);
    dl->AddText(ImVec2(p0.x + 4.0f, p0.y + (h - th) * 0.5f), U32(c.body_text),
                preview_value, end);
    dl->PopClipRect();
  }

  // Label à droite de la boîte (comme un combo ImGui standard) : partie visible
  // avant « ## », centrée verticalement sur le champ. Dessiné en VRAI item ImGui
  // pour que le layout réserve sa largeur : un SameLine() côté appelant (ex.
  // HelpMarker) démarre alors APRÈS le label et non par-dessus.
  const char* label_end = ImGui::FindRenderedTextEnd(label);
  if (label != label_end) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SameLine(0, style.ItemInnerSpacing.x);
    ImGui::SetCursorScreenPos(ImVec2(
        p1.x + style.ItemInnerSpacing.x,
        p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImVec4(c.body_text[0], c.body_text[1], c.body_text[2],
                                 c.body_text[3] * da));
    ImGui::TextUnformatted(label, label_end);
    ImGui::PopStyleColor();
  }

  if (clicked) ImGui::OpenPopup("##rcb_pop");

  // Liste en popup : sous le champ, largeur mini = champ, fond « corps » RO,
  // sélection bleue (onglet actif) + texte corps foncé.
  ImGui::SetNextWindowPos(ImVec2(p0.x, p1.y + 1.0f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(FLT_MAX, FLT_MAX));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(c.body_col[0], c.body_col[1],
                                                 c.body_col[2], c.body_col[3]));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(c.border_col[0], c.border_col[1],
                                                c.border_col[2], c.border_col[3]));
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(c.tab_col[0], c.tab_col[1],
                                                c.tab_col[2], c.tab_col[3]));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(c.tab_col[0], c.tab_col[1],
                                                       c.tab_col[2], 0.6f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.body_text[0], c.body_text[1],
                                              c.body_text[2], c.body_text[3]));
  const bool open = ImGui::BeginPopup("##rcb_pop");
  if (!open) {
    ImGui::PopStyleColor(5);
    ImGui::PopID();
  }
  return open;
}

void RoEndCombo() {
  ImGui::EndPopup();
  ImGui::PopStyleColor(5);
  ImGui::PopID();
}

bool RoCombo(const char* label, int *current_item, const char* const items[], int items_count) {
  bool changed = false;
  const char* modes[] = {"Aucun", "Pourcentage", "Valeurs", "Les deux"};
  if (ro::RoBeginCombo(label, items[*current_item])) {
    for (int i = 0; i < items_count; ++i) {
      const bool selected = (*current_item == i);
      if (ImGui::Selectable(items[i], selected)) {
        *current_item = i;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    changed = true;
    ro::RoEndCombo();
  }
  return changed;
}

bool ShowRoSkinSettings() {
  bool ch = false;
  ch |= WheelSliderFloat("Luminosité", &g_cfg.title_brightness, 0.5f, 1.5f);
  SameLine(); HelpMarker(
    "N'affecte que les images (barre de titre, boutons, scrollbar, footer,\n"
    "icones) - pas le texte ni les fonds (régles par les couleurs ci-dessous).");
  ch |= WheelSliderFloat("Opacité", &g_cfg.alpha, 0.3f, 1.0f, "%.2f");
  ch |= ColorPicker("Corps", g_cfg.body_col);
  ch |= ColorPicker("Bordure", g_cfg.border_col);
  ch |= ColorPicker("Texte titre", g_cfg.title_text);
  ch |= ColorPicker("Texte corps", g_cfg.body_text);
  ch |= ColorPicker("Onglet actif", g_cfg.tab_col);
  ch |= ColorPicker("Onglet inactif", g_cfg.tab_inact);
  ch |= ColorPicker("Champ de saisie", g_cfg.input_col);
  ch |= ColorPicker("En-tête tableau", g_cfg.header_col);
  ch |= ColorPicker("Fond cases (feuille perso)", g_cfg.slot_col);
  ch |= ColorPicker("Fond doll (feuille perso)", g_cfg.doll_col);
  ch |= ColorPicker("Fond carte item", g_cfg.card_col);
  ch |= ColorPicker("Bandeau carte", g_cfg.card_head_col);
  ch |= ColorPicker("Texte bandeau carte", g_cfg.card_head_text);
  ch |= ColorPicker("Fond fenêtre de liste (storage)", g_cfg.list_col);
  if (ImGui::Button("Réinitialiser le skin")) {
    g_cfg = RoSkinConfig();
    ch = true;
  }
  return ch;
}

void DrawBar(float x0, float y0, float x1, float y1) {
  EnsureTex("basic_interface\\btnbar_left.bmp", skin::kBtnbarLeft, g_bar_l);
  EnsureTex("basic_interface\\btnbar_mid.bmp", skin::kBtnbarMid, g_bar_m);
  EnsureTex("basic_interface\\btnbar_right.bmp", skin::kBtnbarRight, g_bar_r);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cap = (float)skin::kBtnbarLeft.w;  // 21
  dl->AddCallback(ImCb_PointFilter, nullptr);
  BlitStretch(dl, g_bar_l, ImVec2(x0, y0), ImVec2(x0 + cap, y1));
  BlitStretch(dl, g_bar_r, ImVec2(x1 - cap, y0), ImVec2(x1, y1));
  BlitStretch(dl, g_bar_m, ImVec2(x0 + cap, y0), ImVec2(x1 - cap, y1));
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

float DrawIconNum(float x, float y) {
  EnsureTex("inventory\\icon_num.bmp", skin::kIconNum, g_iconnum);
  if (!g_iconnum.tex) return 0.0f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddCallback(ImCb_PointFilter, nullptr);
  BlitStretch(dl, g_iconnum, ImVec2(ImFloor(x), ImFloor(y)),
              ImVec2(ImFloor(x) + g_iconnum.w, ImFloor(y) + g_iconnum.h));
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  return (float)g_iconnum.w;
}

bool InputTextCp949(const char* label, char* cp949_buf, size_t buf_size,
                    int imgui_input_flags) {
  if (!cp949_buf || buf_size == 0) return false;

  // One persistent UTF-8 edit buffer per widget id. Kept across frames so the
  // cursor/selection survive; re-seeded from cp949_buf only while NOT editing so
  // external changes are reflected without stomping in-progress input.
  static std::unordered_map<ImGuiID, std::string> store;
  const ImGuiID id = ImGui::GetID(label);
  std::string& utf8 = store[id];
  if (ImGui::GetActiveID() != id) utf8 = Cp949ToUtf8(cp949_buf);

  InputTextUserData ud{&utf8};
  const bool edited = ImGui::InputText(
      label, utf8.data(), utf8.capacity() + 1,
      imgui_input_flags | ImGuiInputTextFlags_CallbackResize, InputTextResizeCb,
      &ud);

  if (edited) Utf8ToCp949(utf8.c_str(), cp949_buf, buf_size);
  return edited;
}

}  // namespace ro
