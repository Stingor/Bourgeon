#include "plugins/skill_bar_tweaks.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bourgeon.h"
#include "imgui.h"
#include "utils/log_console.h"

#pragma comment(lib, "winmm.lib")  // timeGetTime (matche l'horloge cooldown du jeu)

// ===========================================================================
// Barre d'action ImGui remplaçant UIShortCutWnd (id 0x24). DESSIN PUR (pas de
// texture du jeu). 20250716 client / Moonlight-Destiny.exe, base 0x400000
// (no-ASLR : adresses Ghidra == live). RE : memory project_shortcut_bar_re.md.
//
// Invariant clé (validé par revue adverse) : DESSIN et ACTIVATION partagent la
// même source d'index. On lit chaque slot via this+0xc4[i] (pointeur vers le
// record 7 octets dans les globals -> données toujours fraîches) et on active
// par OnMsg(0,0x29,i%9,i/9) (la voie exacte des touches F1-F9). this+0xc4 est
// reconstruit par OnMsg(0x17) au moment où l'on cache la barre + au changement
// d'onglet ; les pointeurs restant valides, les données se rafraîchissent seules.
// ===========================================================================

namespace {

// ---- gestionnaire de fenêtres / instance singleton -------------------------
constexpr uintptr_t kUIWindowMgr    = 0x0131f4e8;  // g_UIWindowMgr
constexpr int       kMgrShortCutPtr = 0x1e8;       // mgr+0x1e8 = instance UIShortCutWnd cachée
constexpr uintptr_t kMakeWindow     = 0x00a39340;  // UIWindowMgr_MakeWindow(mgr, id) -> wnd (idempotent)
constexpr int       kShortCutId     = 0x24;

// ---- skill manager / données slots -----------------------------------------
constexpr uintptr_t kSkillInfoMgr = 0x015fa3c0;    // g_SkillInfoMgr (this des setters)
constexpr uintptr_t kSetShortCut  = 0x00d96c20;    // SkillMgr_SetShortCutSlot
constexpr uintptr_t kGetOption    = 0x008e1d50;    // SkillInfoMgr_GetOption(mgr,key) ; key10=onglet courant

// ---- vtable / champs UIShortCutWnd -----------------------------------------
constexpr int kVfSetVisible = 0x38;   // FUN_009030c0(this, vis) -> this+0x28
constexpr int kVfOnMsg      = 0x94;   // UIShortCutWnd::OnMsg
constexpr int kSlotArr      = 0xc4;   // this+0xc4 = 36 pointeurs de record
constexpr int kMsgUseSlot   = 0x29;   // OnMsg : active le slot (p3=col, p4=row)
constexpr int kMsgRebuild   = 0x17;   // OnMsg : reconstruit this+0xc4 depuis les globals
constexpr int kMaxSlots     = 36;     // 0x24

// ---- cooldown (g_ShortCutCooldownList 0x015ff7e0) --------------------------
constexpr uintptr_t kCooldownList  = 0x015ff7e0;   // sentinelle (la liste EST la sentinelle)
constexpr uintptr_t kUseGameClock  = 0x015beecc;   // 0 -> timeGetTime(), sinon horloge jeu
constexpr uintptr_t kGameClockGet  = 0x00b1fac0;   // -> ptr, DWORD horloge à +0x20

using OnMsg_t      = int   (__fastcall*)(void*, void*, int, int, int, int, int, int);
using SetVisible_t = void  (__fastcall*)(void*, void*, int);
using MakeWindow_t = void* (__fastcall*)(void*, void*, void*);
using SetSlot_t    = void  (__fastcall*)(void*, void*, int, int, int, int, int);
using GetOption_t  = int   (__thiscall*)(void*, int);
using GameClock_t  = void* (__cdecl*)();

struct CooldownNode {       // 0x24 octets
  CooldownNode* next;       // +0x00
  CooldownNode* prev;       // +0x04
  uint32_t      skillId;    // +0x08
  uint32_t      endTick;    // +0x0C = clock + duration
  uint32_t      duration;   // +0x10 (ms)
  // ... listes d'animation imbriquées +0x14.. (inutiles ici)
};

struct SlotRec { bool valid; uint8_t type; uint32_t id; int16_t level; };

inline void* ShortCutWnd() {
  return *reinterpret_cast<void**>(kUIWindowMgr + kMgrShortCutPtr);
}
template <typename Fn>
inline Fn Vf(void* self, int off) {
  return reinterpret_cast<Fn>((*reinterpret_cast<uintptr_t**>(self))[off / 4]);
}

void EnsureCreated() {
  if (ShortCutWnd()) return;  // déjà créée (cas normal) ou recréée ci-dessous
  reinterpret_cast<MakeWindow_t>(kMakeWindow)(
      reinterpret_cast<void*>(kUIWindowMgr), nullptr,
      reinterpret_cast<void*>(kShortCutId));
}
void RebuildSlotPtrs(void* w) {  // OnMsg 0x17 : reconstruit this+0xc4 depuis les globals
  Vf<OnMsg_t>(w, kVfOnMsg)(w, nullptr, 0, kMsgRebuild, 0, 0, 0, 0);
}
void HideNative(void* w) {  // this+0x28 = 0 + délink draw-list ; ne détruit PAS l'objet
  Vf<SetVisible_t>(w, kVfSetVisible)(w, nullptr, 0);
}
void ShowNative(void* w) {
  Vf<SetVisible_t>(w, kVfSetVisible)(w, nullptr, 1);
}
int CurrentTab() {
  return reinterpret_cast<GetOption_t>(kGetOption)(
             reinterpret_cast<void*>(kSkillInfoMgr), 10) ? 1 : 0;
}

// Lit le slot logique i (0..35) via this+0xc4[i] -> record 7 octets dans les globals.
SlotRec ReadSlot(void* w, int i) {
  SlotRec s{false, 0, 0, 0};
  if (i < 0 || i >= kMaxSlots) return s;
  uint8_t* rec = *reinterpret_cast<uint8_t**>(
      reinterpret_cast<uint8_t*>(w) + kSlotArr + i * 4);
  if (!rec) return s;
  uint32_t id;
  std::memcpy(&id, rec + 1, 4);  // id NON aligné dans le record packé
  if (id == 0) return s;         // slot vide (le natif teste id!=0)
  int16_t lvl;
  std::memcpy(&lvl, rec + 5, 2);
  s = {true, rec[0], id, lvl};
  return s;
}

// Active le slot logique i exactement comme une touche F (OnMsg case 0x29).
void ActivateSlot(void* w, int i) {
  Vf<OnMsg_t>(w, kVfOnMsg)(w, nullptr, 0, kMsgUseSlot, i % 9, i / 9, 0, 0);
}
// Vide le slot logique i de l'onglet tab (écrit directement les globals).
void ClearSlot(int i, int tab) {
  reinterpret_cast<SetSlot_t>(kSetShortCut)(
      reinterpret_cast<void*>(kSkillInfoMgr), nullptr, /*type*/0, /*id=0 -> clear*/0,
      /*level*/0, /*slot*/i, /*tab*/tab);
}

DWORD ShortCutNow() {  // même sélection d'horloge que OnDraw/OnMsg 0x8f
  if (*reinterpret_cast<int*>(kUseGameClock) == 0) return timeGetTime();
  void* clk = reinterpret_cast<GameClock_t>(kGameClockGet)();
  return clk ? *reinterpret_cast<DWORD*>(reinterpret_cast<uint8_t*>(clk) + 0x20)
             : timeGetTime();
}
// Fraction de cooldown restante [0,1] (0 = prêt). Match exact par id (les
// pseudo-ids de groupe 0x241/9999 = polish v2).
float CooldownFraction(uint32_t skillId) {
  CooldownNode* sentinel = reinterpret_cast<CooldownNode*>(kCooldownList);
  const DWORD now = ShortCutNow();
  for (CooldownNode* n = sentinel->next; n && n != sentinel; n = n->next) {
    if (n->skillId != skillId) continue;
    if (n->duration == 0) return 0.0f;
    const int remain = static_cast<int>(n->endTick - now);   // unsigned-safe
    if (remain <= 0) return 0.0f;                            // expiré
    DWORD r = static_cast<DWORD>(remain);
    if (r > n->duration) r = n->duration;
    return static_cast<float>(r) / static_cast<float>(n->duration);
  }
  return 0.0f;
}

}  // namespace

void SkillBarTweaks::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);
  if (!in_game_) { native_hidden_ = false; last_tab_ = -1; }
}

void SkillBarTweaks::OnKeyDown(unsigned long vkey, int, int) {
  if (vkey == VK_OEM_3) panel_visible_ = !panel_visible_;  // touche ²/~ : panneau
}

void SkillBarTweaks::OnRenderUI() {
  if (!in_game_) return;

  void* w = ShortCutWnd();

  // ── Bascule cacher / restaurer la barre native selon l'activation ──────────
  const bool want_hidden = enabled_ &&
      Bourgeon::Instance().client().session().aid() != 0;
  if (want_hidden && !native_hidden_) {
    EnsureCreated();
    w = ShortCutWnd();
    if (w) { HideNative(w); RebuildSlotPtrs(w); native_hidden_ = true; last_tab_ = CurrentTab(); }
  } else if (!want_hidden && native_hidden_) {
    if (w) ShowNative(w);
    native_hidden_ = false;
  }
  // Resync this+0xc4 au changement d'onglet (sinon les slots montrés/lancés diffèrent).
  if (native_hidden_ && w) {
    const int tab = CurrentTab();
    if (tab != last_tab_) { RebuildSlotPtrs(w); last_tab_ = tab; }
  }

  if (panel_visible_) DrawPanel();
  if (native_hidden_ && w) DrawBar(w);
}

// ---- panneau de configuration ----------------------------------------------
void SkillBarTweaks::DrawPanel() {
  ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
  bool open = true;
  if (!ImGui::Begin("Skill Bar###SkillBarPanel", &open)) { ImGui::End(); if(!open) panel_visible_=false; return; }
  if (!open) panel_visible_ = false;

  ImGui::Checkbox("Activer (barres ImGui)", &enabled_);
  ImGui::SameLine();
  ImGui::BeginDisabled(!enabled_);
  ImGui::Checkbox("Edition (deplacer)", &edit_mode_);
  ImGui::EndDisabled();

  ImGui::BeginDisabled(!enabled_);
  ImGui::SliderInt("Colonnes", &columns_, 1, 12);
  ImGui::SliderInt("Nb slots", &slot_count_, 1, kMaxSlots);
  ImGui::SliderInt("1er slot", &first_slot_, 0, kMaxSlots - 1);
  if (first_slot_ + slot_count_ > kMaxSlots) slot_count_ = kMaxSlots - first_slot_;
  ImGui::SliderFloat("Taille", &icon_size_, 16.0f, 64.0f, "%.0f px");
  ImGui::SliderFloat("Espacement", &spacing_, 0.0f, 12.0f, "%.0f px");
  ImGui::Text("Position : %d, %d  (onglet %d)", bar_x_, bar_y_, native_hidden_ ? last_tab_ : 0);
  ImGui::EndDisabled();

  ImGui::Separator();
  ImGui::TextDisabled("Clic G = utiliser, Clic D = vider.");
  ImGui::TextDisabled("Bleu = skill, Vert = objet. Toggle panneau : 2/~");
  ImGui::TextDisabled("TODO v2 : icones, multi-barres, persistance yaml.");
  ImGui::End();
}

// ---- la barre d'action elle-même -------------------------------------------
void SkillBarTweaks::DrawBar(void* w) {
  const int cols = std::max(1, columns_);
  const int rows = (slot_count_ + cols - 1) / cols;
  const float step = icon_size_ + spacing_;
  const float pad  = edit_mode_ ? 4.0f : 0.0f;
  const float winw = cols * step - spacing_ + pad * 2;
  const float winh = rows * step - spacing_ + pad * 2;

  ImGui::SetNextWindowPos(ImVec2((float)bar_x_, (float)bar_y_),
                          edit_mode_ ? ImGuiCond_Once : ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(winw, winh), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav;
  if (!edit_mode_) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground;

  ImGui::Begin("##SkillActionBar", nullptr, flags);

  // En édition, on suit le déplacement de la fenêtre pour persister la position.
  if (edit_mode_) {
    const ImVec2 wp = ImGui::GetWindowPos();
    bar_x_ = (int)(wp.x + 0.5f);
    bar_y_ = (int)(wp.y + 0.5f);
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int tab = native_hidden_ ? last_tab_ : 0;

  for (int k = 0; k < slot_count_; ++k) {
    const int slot = first_slot_ + k;
    if (slot >= kMaxSlots) break;
    const int cx = k % cols, cy = k / cols;

    ImGui::PushID(k);
    ImGui::SetCursorPos(ImVec2(pad + cx * step, pad + cy * step));
    const bool clicked = ImGui::InvisibleButton("s", ImVec2(icon_size_, icon_size_));
    const bool rclick  = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();

    const SlotRec r = ReadSlot(w, slot);

    // Fond color-codé : vide / skill (bleu) / objet (vert).
    ImU32 bg = IM_COL32(30, 30, 36, 200);
    if (r.valid) bg = (r.type == 0) ? IM_COL32(40, 80, 150, 220)
                                    : IM_COL32(40, 130, 70, 220);
    dl->AddRectFilled(p0, p1, bg, 3.0f);

    if (r.valid) {
      // Texte : id (decimal) centré, niveau en bas-droite.
      char idbuf[16];
      std::snprintf(idbuf, sizeof(idbuf), "%u", r.id);
      const ImVec2 ts = ImGui::CalcTextSize(idbuf);
      dl->AddText(ImVec2(p0.x + (icon_size_ - ts.x) * 0.5f,
                         p0.y + (icon_size_ - ts.y) * 0.5f),
                  IM_COL32(235, 235, 235, 255), idbuf);
      if (r.level > 0) {
        char lv[8];
        std::snprintf(lv, sizeof(lv), "%d", r.level);
        const ImVec2 ls = ImGui::CalcTextSize(lv);
        dl->AddText(ImVec2(p1.x - ls.x - 1, p1.y - ls.y - 1),
                    IM_COL32(255, 230, 120, 255), lv);
      }
      // Overlay de cooldown (skills) : voile sombre du bas vers le haut.
      if (r.type == 0) {
        const float f = CooldownFraction(r.id);
        if (f > 0.0f) {
          const float h = icon_size_ * f;
          dl->AddRectFilled(ImVec2(p0.x, p1.y - h), p1, IM_COL32(0, 0, 0, 150), 3.0f);
        }
      }
    }

    // Bordure (plus claire au survol / en édition).
    const ImU32 border = (hovered || edit_mode_) ? IM_COL32(255, 220, 120, 230)
                                                 : IM_COL32(0, 0, 0, 200);
    dl->AddRect(p0, p1, border, 3.0f);

    if (!edit_mode_) {
      if (clicked && r.valid) ActivateSlot(w, slot);
      if (rclick  && r.valid) ClearSlot(slot, tab);
      if (hovered && r.valid)
        ImGui::SetTooltip("%s id %u%s", r.type == 0 ? "Skill" : "Objet", r.id,
                          r.level > 0 ? "" : "");
    }
    ImGui::PopID();
  }

  ImGui::End();
  ImGui::PopStyleVar(2);
}
