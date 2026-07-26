#include "plugins/quest_tracker_tweaks.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>

#include "imgui.h"
#include "bourgeon.h"
#include "plugins/moonlight_ui.h"
#include "ui/ro_imgui.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// ===========================================================================
// On-screen quest tracker — native hide + ImGui reimplementation.
// See quest_tracker_tweaks.h / project_quest_tracking_re for the design & RE.
// ===========================================================================

namespace {

// ---- engine addresses ------------------------------------------------------
constexpr uintptr_t kDrawContent = 0x009137a0;  // QuestTracker_DrawContent (hooked)
constexpr uintptr_t kQuestMgrPtr = 0x01254d90;  // void** -> quest manager

// std::_Tree node offsets (MSVC, 32-bit).
constexpr int kNodeLeft   = 0x00;
constexpr int kNodeParent = 0x04;
constexpr int kNodeRight  = 0x08;
constexpr int kNodeIsNil  = 0x0d;

// Quest record (== std::_Tree node) field offsets.  The Quest struct proper
// begins at node+0x18: the native DrawContent reads id/active/title/tracked
// via the node pointer directly, but the objective and hunt fields via a
// cached (node+0x18) base — so their node-absolute offset is 0x18 + their
// Quest-relative offset.  All confirmed against QuestTracker_DrawContent.
constexpr int kRecId       = 0x10;
constexpr int kRecActive   = 0x18;   // byte  (Quest+0x00)
constexpr int kRecTitle    = 0x1c;   // std::string (Quest+0x04)
constexpr int kRecObjective= 0x64;   // std::string (Quest+0x4c = node+0x18+0x4c)
constexpr int kRecTracked  = 0xcc;   // byte  (Quest+0xb4)
constexpr int kRecHuntCnt  = 0xea;   // signed byte (Quest+0xd2)
constexpr int kRecMobId    = 0xec;   // int[i]  (Quest+0xd4)
constexpr int kRecCur      = 0xf8;   // short[i] (Quest+0xe0)
constexpr int kRecTgt      = 0x198;  // short[i] (Quest+0x180)
constexpr int kMaxHunt     = 3;

// std::string (MSVC, 32-bit): _Bx(16) + _Mysize(0x10) + _Myres(0x14).
constexpr int kStrSize = 0x10;
constexpr int kStrCap  = 0x14;
constexpr uint32_t kStrSso = 16;

// __thiscall(this) emulated as __fastcall + dummy edx (project convention).
using DrawFn_t = void(__fastcall*)(void* self, void* edx);
DrawFn_t g_draw_orig = nullptr;

QuestTrackerConfig g_cfg;
bool g_in_game    = false;
bool g_needs_save = false;

// ---- data model (POD only; filled under SEH) -------------------------------
struct HuntLine { char name[64]; int cur; int tgt; };
struct QuestEntry {
  int  id;
  char title[128];
  char objective[192];
  HuntLine hunts[kMaxHunt];
  int  hunt_count;
};
constexpr int kMaxCollect = 16;

// Copy a game std::string at address `s` into `out` (SEH-guarded, POD only).
void ReadStdString(const void* s, char* out, int cap) {
  out[0] = '\0';
  __try {
    const uint8_t* S = reinterpret_cast<const uint8_t*>(s);
    uint32_t len = *reinterpret_cast<const uint32_t*>(S + kStrSize);
    uint32_t res = *reinterpret_cast<const uint32_t*>(S + kStrCap);
    const char* src =
        (res >= kStrSso) ? *reinterpret_cast<const char* const*>(S)
                         : reinterpret_cast<const char*>(S);
    if (!src) return;
    if (len > static_cast<uint32_t>(cap - 1)) len = cap - 1;
    for (uint32_t i = 0; i < len; ++i) out[i] = src[i];
    out[len] = '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = '\0';
  }
}

inline uint8_t B(const uint8_t* p, int off) { return *(p + off); }
inline int16_t I16(const uint8_t* p, int off) {
  return *reinterpret_cast<const int16_t*>(p + off);
}

// Walk the active-quest std::_Tree, collecting tracked+active quests into `out`.
// Iterative DFS bounded by the tree size; POD-only so it stays SEH-safe.
int CollectQuests(QuestEntry* out, int cap) {
  int n = 0;
  __try {
    void* mgr = *reinterpret_cast<void**>(kQuestMgrPtr);
    if (!mgr) return 0;
    uint8_t* head = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(mgr) + 4);
    if (!head) return 0;

    uint8_t* stack[64];
    int sp = 0;
    uint8_t* root = *reinterpret_cast<uint8_t**>(head + kNodeParent);
    if (root && root != head) stack[sp++] = root;

    while (sp > 0 && n < cap) {
      uint8_t* node = stack[--sp];
      if (!node || B(node, kNodeIsNil) != 0) continue;

      if (B(node, kRecActive) == 1 && B(node, kRecTracked) == 1) {
        QuestEntry& q = out[n];
        q.id = *reinterpret_cast<int*>(node + kRecId);
        ReadStdString(node + kRecTitle, q.title, sizeof(q.title));
        ReadStdString(node + kRecObjective, q.objective, sizeof(q.objective));
        int sc = *reinterpret_cast<signed char*>(node + kRecHuntCnt);
        if (sc < 0) sc = 0;
        if (sc > kMaxHunt) sc = kMaxHunt;
        q.hunt_count = sc;
        for (int i = 0; i < sc; ++i) {
          // Monster name isn't a plain record field (the native builds it from
          // level-range / format-string logic); left empty until REd against a
          // live hunt quest, so the line renders as "( cur / tgt )".
          q.hunts[i].name[0] = '\0';
          q.hunts[i].cur = I16(node, kRecCur + i * 2);
          q.hunts[i].tgt = I16(node, kRecTgt + i * 2);
        }
        ++n;
      }

      uint8_t* l = *reinterpret_cast<uint8_t**>(node + kNodeLeft);
      uint8_t* r = *reinterpret_cast<uint8_t**>(node + kNodeRight);
      if (sp < 62) {
        if (l && B(l, kNodeIsNil) == 0) stack[sp++] = l;
        if (r && B(r, kNodeIsNil) == 0) stack[sp++] = r;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return n;
  }
  return n;
}

// Stable ascending sort by quest id (small n) so ordering is deterministic.
void SortById(QuestEntry* a, int n) {
  for (int i = 1; i < n; ++i) {
    QuestEntry key = a[i];
    int j = i - 1;
    while (j >= 0 && a[j].id > key.id) { a[j + 1] = a[j]; --j; }
    a[j + 1] = key;
  }
}

inline ImU32 RgbToImU32(int rgb, int alpha = 255) {
  return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, alpha);
}
inline void RgbToF3(int rgb, float* f) {
  f[0] = ((rgb >> 16) & 0xff) / 255.0f;
  f[1] = ((rgb >> 8) & 0xff) / 255.0f;
  f[2] = (rgb & 0xff) / 255.0f;
}
inline int F3ToRgb(const float* f) {
  int r = static_cast<int>(f[0] * 255.0f + 0.5f);
  int g = static_cast<int>(f[1] * 255.0f + 0.5f);
  int b = static_cast<int>(f[2] * 255.0f + 0.5f);
  return (r << 16) | (g << 8) | b;
}

// Native DrawContent replacement.  When the custom overlay is on we draw
// nothing (the ImGui window replaces it); otherwise forward to stock.
void __fastcall DrawContentHook(void* self, void* edx) {
  if (g_cfg.enabled && g_in_game) return;  // hide native; ImGui draws instead
  if (g_draw_orig) g_draw_orig(self, edx);
}

}  // namespace

QuestTrackerTweaks::QuestTrackerTweaks() {
  // Verify the SEH prologue (55 8B EC 6A FF = push ebp; mov ebp,esp; push -1)
  // before hooking so a client mismatch fails safe.
  const auto* p = reinterpret_cast<const uint8_t*>(kDrawContent);
  if (!(p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && p[3] == 0x6A &&
        p[4] == 0xFF)) {
    LogError("[QuestTracker] DrawContent prologue mismatch; hook skipped");
    return;
  }
  g_draw_orig = reinterpret_cast<DrawFn_t>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kDrawContent),
          reinterpret_cast<uint8_t*>(&DrawContentHook)));
  if (g_draw_orig)
    /* LogInfo("[QuestTracker] native tracker draw hook installed") */;
  else
    LogError("[QuestTracker] failed to install draw hook");
}

void QuestTrackerTweaks::OnModeSwitch(ModeMgr::ModeType mode_type,
                                      const char* /*map_name*/) {
  g_in_game = (mode_type == ModeMgr::ModeType::kGame);
}

void QuestTrackerTweaks::OnRenderUI() {
  if (!g_cfg.enabled || !g_in_game) return;

  QuestEntry quests[kMaxCollect];
  int cap = g_cfg.max_quests;
  if (cap < 1) cap = 1;
  if (cap > kMaxCollect) cap = kMaxCollect;
  const int n = CollectQuests(quests, cap);
  SortById(quests, n);

  // Movable via the title bar, or via the body when borderless+unlocked.
  const bool movable = g_cfg.show_titlebar || !g_cfg.locked;

  // Pure borderless overlay with nothing to show: skip (nothing to draw, not
  // draggable). With a title bar we always draw so the header + close stay.
  if (!g_cfg.show_titlebar && g_cfg.locked && n == 0) return;

  const float w = static_cast<float>(g_cfg.width < 80 ? 80 : g_cfg.width);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_AlwaysAutoResize;
  if (!g_cfg.show_titlebar) flags |= ImGuiWindowFlags_NoTitleBar;
  if (!movable) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;

  ImGui::SetNextWindowPos(ImVec2(static_cast<float>(g_cfg.pos_x),
                                 static_cast<float>(g_cfg.pos_y)),
                          movable ? ImGuiCond_Once : ImGuiCond_Always);
  // Width is driven by the text wrap position below; AlwaysAutoResize handles
  // height (and clamps width to ~wrap).  So no explicit SetNextWindowSize.
  ImGui::SetNextWindowBgAlpha(g_cfg.show_bg ? (g_cfg.bg_alpha / 100.0f) : 0.0f);

  const ImU32 titleCol = RgbToImU32(g_cfg.title_rgb);
  const ImU32 descCol  = RgbToImU32(g_cfg.desc_rgb);
  const ImU32 huntCol  = RgbToImU32(g_cfg.hunt_rgb);

  // Title bar shows the tracked-quest count; fixed ### id keeps the window
  // stable while the visible label changes. The (X) close button turns the
  // custom tracker off (re-enable from the settings panel).
  char titlebuf[64];
  snprintf(titlebuf, sizeof(titlebuf), "Suivi de quete (%d)###quest_tracker", n);
  bool open = true;
  bool* p_open = g_cfg.show_titlebar ? &open : nullptr;

  if (ImGui::Begin(titlebuf, p_open, flags)) {
    ImGui::SetWindowFontScale(g_cfg.font_scale / 100.0f);

    // Persist a drag once it settles (movable modes only).
    if (movable) {
      const ImVec2 wp = ImGui::GetWindowPos();
      if (static_cast<int>(wp.x) != g_cfg.pos_x ||
          static_cast<int>(wp.y) != g_cfg.pos_y) {
        g_cfg.pos_x = static_cast<int>(wp.x);
        g_cfg.pos_y = static_cast<int>(wp.y);
        g_needs_save = true;
      }
    }

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    if (n == 0) {
      ImGui::TextDisabled("(aucune quete suivie)");
    }
    for (int i = 0; i < n; ++i) {
      const QuestEntry& q = quests[i];
      if (i > 0) ImGui::Spacing();

      ImGui::PushStyleColor(ImGuiCol_Text, titleCol);
      ImGui::TextWrapped("%s", q.title[0] ? q.title : "?");
      ImGui::PopStyleColor();

      if (g_cfg.show_objective && q.objective[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, descCol);
        ImGui::TextWrapped("%s", q.objective);
        ImGui::PopStyleColor();
      }

      for (int h = 0; h < q.hunt_count; ++h) {
        const HuntLine& hl = q.hunts[h];
        ImGui::PushStyleColor(ImGuiCol_Text, huntCol);
        if (hl.name[0])
          ImGui::TextWrapped("%s ( %d / %d )", hl.name, hl.cur, hl.tgt);
        else
          ImGui::TextWrapped("( %d / %d )", hl.cur, hl.tgt);
        ImGui::PopStyleColor();
      }
    }
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(1.0f);
  }
  ImGui::End();

  // (X) close button: turn the custom tracker off (native returns; re-enable
  // from the Moonlight settings panel).
  if (g_cfg.show_titlebar && !open) {
    g_cfg.enabled = false;
    g_needs_save = true;
  }

  // Flush a settled edit/drag.
  if (g_needs_save && !ImGui::IsAnyItemActive()) {
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
    g_needs_save = false;
  }
}

void QuestTrackerTweaks::DrawSettings() {
  g_needs_save |= ro::RoCheckbox("Suivi de quête personnalisé", &g_cfg.enabled);
  SameLine(); HelpMarker("Activé = overlay personnalisable\nDésactivé = overlay d'origine.");

  ImGui::BeginDisabled(!g_cfg.enabled);
  g_needs_save |= ImGui::Checkbox("Barre de titre (nom + bouton de fermeture)", &g_cfg.show_titlebar);
  SameLine(); HelpMarker(
    "Affiche une barre de titre avec le nombre de quêtes suivies.\n"
    "Le panneau se déplace en glissant la barre.");

  // Borderless mode only: lock = click-through overlay; unlock = drag the body.
  ImGui::BeginDisabled(g_cfg.show_titlebar);
  bool unlocked = !g_cfg.locked;
  if (ImGui::Checkbox("Déverrouiller (glisser pour déplacer)", &unlocked)) {
    g_cfg.locked = !unlocked;
    g_needs_save = true;
  }
  SameLine(); HelpMarker("Déverrouille = glisser le corps du panneau\nVerrouille = overlay non cliquable.");
  ImGui::EndDisabled();

  g_needs_save |= ro::RoCheckbox("Afficher l'objectif", &g_cfg.show_objective);
  g_needs_save |= WheelSliderInt("Position X", &g_cfg.pos_x, 0, 1920);
  g_needs_save |= WheelSliderInt("Position Y", &g_cfg.pos_y, 0, 1080);
  g_needs_save |= WheelSliderInt("Largeur", &g_cfg.width, 120, 600, "%d px");
  g_needs_save |= WheelSliderInt("Quetes max", &g_cfg.max_quests, 1, kMaxCollect);
  g_needs_save |= WheelSliderInt("Taille police", &g_cfg.font_scale, 60, 200, "%d%%");

  SeparatorText("Couleurs");
  float tc[3], dc[3], hc[3];
  RgbToF3(g_cfg.title_rgb, tc);
  RgbToF3(g_cfg.desc_rgb, dc);
  RgbToF3(g_cfg.hunt_rgb, hc);
  if (ColorEdit4WithAlphaBar("Couleur titre", tc)) {
    g_cfg.title_rgb = F3ToRgb(tc);
    g_needs_save = true;
  }
  if (ColorEdit4WithAlphaBar("Couleur objectif", dc)) {
    g_cfg.desc_rgb = F3ToRgb(dc);
    g_needs_save = true;
  }
  if (ColorEdit4WithAlphaBar("Couleur chasse", hc)) {
    g_cfg.hunt_rgb = F3ToRgb(hc);
    g_needs_save = true;
  }

  g_needs_save |= ImGui::Checkbox("Fond translucide", &g_cfg.show_bg);
  ImGui::BeginDisabled(!g_cfg.show_bg);
  g_needs_save |= WheelSliderInt("Opacite du fond", &g_cfg.bg_alpha, 0, 100, "%d%%");
  ImGui::EndDisabled();

  if (ro::RoButton("Reinitialiser")) {
    g_cfg = QuestTrackerConfig{};
    g_cfg.enabled = true;  // keep it on after a reset from the panel
    g_needs_save = true;
  }

  ImGui::EndDisabled();
}

QuestTrackerConfig& QuestTrackerTweaks::config() { return g_cfg; }
