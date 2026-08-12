#include "ui/game_texture.h"
#include "features/overlays/menu_icons.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"

#include "ragnarok/msgstring.h"  // msgstr:: (libellés natifs du client)
#include "ragnarok/uiwnd.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "bourgeon.h"
#include "d3d9/d3d9_hook.h"
#include "imgui.h"
#include "features/systems/bourgeon_opcodes.h"
#include "features/moonlight_ui/moonlight_ui.h"  // shared AlignGrid (snap)
#include "ui/window_clamp.h"  // ClampWindowPosToScreen (icônes déplacées à la main)
#include "utils/log_console.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace {
// ── Game texture loader (conventions per status_tweaks.cc) ─────────────────
using TexMgr_t  = void* (__cdecl*)();
using MakeKey_t = void* (__cdecl*)(const char*);
using LoadTex_t = void* (__fastcall*)(void*, void*, void*);

// UITexture field offsets: +0x114 width, +0x118 height, +0x11c BGRA32 top-down.
constexpr int kOffW = 0x114, kOffH = 0x118, kOffPix = 0x11c;

// UI bitmaps live under "유저인터페이스\" (CP949). Verbatim bytes from the
// client's own format string @0x01028384 ("유저인터페이스\menu_icon\bt_%s.bmp").
const char kUIDir[] = "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA";

// ── Native menu-icon window draw hook (hide the native grid) ───────────────
// GridClear replaces the native grid's DrawContent (UIMenuIconWnd_RebuildNodes):
// it empties the window's render-node list (window+4 = std::list head, window+8 =
// size) and does NOT rebuild from the icon map (+0xb4, which stays populated for
// dispatch). Unlike a plain no-op, this also drops the ALREADY-built nodes, so
// the grid vanishes immediately instead of lingering until the next relayout
// (map reload). The orphaned nodes leak once per enable (~25 * 0x18 bytes —
// negligible; the game rebuilds them when the replacement is disabled).
constexpr uintptr_t kGridDrawSlot = 0x01028200;  // UIMenuIconWnd vtbl +0x50
constexpr uintptr_t kGridDrawOrig = 0x00814150;  // UIMenuIconWnd_RebuildNodes
void __fastcall GridClear(void* self, void* /*edx*/) {
  __try {
    char* w = reinterpret_cast<char*>(self);
    int* head = *reinterpret_cast<int**>(w + 4);  // std::list sentinel node
    if (!head) return;
    head[0] = reinterpret_cast<int>(head);         // next = self (empty list)
    head[1] = reinterpret_cast<int>(head);         // prev = self
    *reinterpret_cast<int*>(w + 8) = 0;            // size = 0
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Command dispatch ───────────────────────────────────────────────────────
// A native icon click routes (OnLButtonDown -> FUN_00a38b40 table lookup ->
// tail-jump) into the menu-icon window's command handler FUN_00814a70, invoked
// as (this=window, 0, action=6, cmdId, 0, 0, 0). We call that handler DIRECTLY:
// it only needs the window + cmd id, so we avoid synthesising a click — the old
// path poked the window-manager's active-window slot (mgr+0x19c) and the icon's
// +0xac then restored them, which corrupted the game's active-window tracking
// and crashed when a window was opened/closed repeatedly (spam).
constexpr int       kMenuIconWndId = 0x133;
constexpr uintptr_t kCmdHandler    = 0x00814a70;  // UIMenuIconWnd command handler
using CmdHandlerFn = void  (__thiscall*)(void*, int, int, int, int, int, int);

// La carte du monde (et les UI plein écran) remplacent tout le HUD : la grille
// d'icônes native cesse de se dessiner, donc nos icônes ImGui doivent se cacher
// et refuser les clics. C'est uiwnd::IsHudReplaced() qui porte ce test — il
// était copié ici et dans basic_info.cc, à l'identique.

// ── Functional-icon test: native grid only shows ids >= 0x178 per the WARP
// visibility table @0x814064 (byte 1 = shown); classic ids < 0x178 always show.
constexpr uintptr_t kVisTable = 0x00814064;  // indexed by (cmdId - 0x178), 0..0xDF

bool IconShown(int cmd_id) {
  if (cmd_id < 0x178) return true;
  const int idx = cmd_id - 0x178;
  if (idx < 0 || idx > 0xDF) return true;
  return *reinterpret_cast<uint8_t*>(kVisTable + idx) == 1;
}

// ── Signalement « nouveau » (le N rouge du courrier) ───────────────────────
// Le natif ne peint pas une pastille par-dessus l'icône : `BuildIconList`
// @0x00812fb0 crée une SECONDE UIMenuIcon (bitmaps bt_<name>_new.bmp /
// _new_press.bmp) pour les cinq commandes qui en ont une — status 0xC0,
// item 0xC2, skill 0xC4, achievement 0x1D9, mail 0x1DC — puis
// `RebuildNodes` @0x00814150 montre l'une OU l'autre.
// Ce qui décide, c'est une std::list<int> des commandes à signaler portée par
// la fenêtre elle-même : sentinelle à +0xC4, taille à +0xC8, nœuds
// {next, prev, valeur}. Elle est alimentée par son propre OnMsg (action 6,
// commande 286, arg = id d'icône, arg suivant = 1 allumer / 0 éteindre) —
// c'est ce qu'appellent les handlers RODEX : allumage à l'arrivée d'un
// courrier non lu, extinction dans 0x00cfb6b0 quand le compteur de non-lus
// (g_RodexMgr+0x18) retombe à zéro.
// On LIT cette liste au lieu de rejouer la règle métier : le signalement suit
// donc exactement le natif, pour les cinq icônes et sans code par icône. Elle
// reste tenue à jour alors même que la grille native est vidée (GridClear ne
// touche qu'à la liste de nœuds de RENDU, +0x4).
constexpr int kOffBadgeList = 0xC4;  // std::list<int> : sentinelle
constexpr int kOffBadgeSize = 0xC8;  // taille de cette liste

// Relève les commandes signalées dans `out` (au plus `cap`). Renvoie le nombre
// lu, 0 si la fenêtre est absente ou la liste illisible. Isolée du reste pour
// garder le __try loin de tout objet à destructeur.
int ReadBadgeCmdIds(void* wnd, int* out, int cap) {
  if (!wnd) return 0;
  int n = 0;
  __try {
    char* w = reinterpret_cast<char*>(wnd);
    int* head = *reinterpret_cast<int**>(w + kOffBadgeList);
    const int count = *reinterpret_cast<int*>(w + kOffBadgeSize);
    if (!head || count <= 0 || count > cap) return 0;
    int* node = reinterpret_cast<int*>(head[0]);  // premier élément
    while (n < count && node && node != head) {
      out[n++] = node[2];                         // nœud : next, prev, valeur
      node = reinterpret_cast<int*>(node[0]);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
  return n;
}

void* LoadGameTexture(const char* path) {
  void* mgr = reinterpret_cast<TexMgr_t>(ro::texmgr::kGet)();
  if (!mgr) return nullptr;
  void* key = reinterpret_cast<MakeKey_t>(ro::texmgr::kMakeKey)(path);
  if (!key) return nullptr;
  return reinterpret_cast<LoadTex_t>(ro::texmgr::kLoad)(mgr, nullptr, key);
}

// Loads \menu_icon\bt_<name>.bmp via the game, decodes BGRA->A8R8G8B8 with
// magenta colorkey, uploads to a D3D9 texture. Returns the texture or nullptr
// (and fills w/h when known).
void* LoadIconTexture(const char* name, int* out_w, int* out_h) {
  std::string path = std::string(kUIDir) + "\\menu_icon\\bt_" + name + ".bmp";
  void* tex = LoadGameTexture(path.c_str());
  if (!tex) return nullptr;
  const int w = *reinterpret_cast<int*>(static_cast<char*>(tex) + kOffW);
  const int h = *reinterpret_cast<int*>(static_cast<char*>(tex) + kOffH);
  void* bgra  = *reinterpret_cast<void**>(static_cast<char*>(tex) + kOffPix);
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || !bgra) return nullptr;
  std::vector<unsigned char> argb(static_cast<size_t>(w) * h * 4);
  const unsigned char* src = static_cast<const unsigned char*>(bgra);
  for (int i = 0; i < w * h; ++i) {
    const unsigned char b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
    const bool key = (r == 0xFF && g == 0x00 && b == 0xFF);
    argb[i * 4 + 0] = b;
    argb[i * 4 + 1] = g;
    argb[i * 4 + 2] = r;
    argb[i * 4 + 3] = key ? 0 : 0xFF;
  }
  *out_w = w;
  *out_h = h;
  // Route through the renderer-agnostic helper so icons upload as a DirectDraw
  // surface in DX7 mode and a D3D9 texture in DX9 mode (else they're invisible
  // in whichever renderer doesn't match the upload path).
  return Overlay_CreateTextureARGB(argb.data(), w, h);
}

template <typename T>
void PatchValue(uintptr_t addr, T value) {
  DWORD old;
  if (VirtualProtect(reinterpret_cast<void*>(addr), sizeof(T),
                     PAGE_EXECUTE_READWRITE, &old)) {
    *reinterpret_cast<T*>(addr) = value;
    VirtualProtect(reinterpret_cast<void*>(addr), sizeof(T), old, &old);
  }
}

// name, cmd_id, tooltip msg_id (from UIMenuIcon_SetHelpTextByCmdId @0x00814550).
struct IconDef { const char* name; int cmd_id; int msg_id; };
const IconDef kIconTable[] = {
    {"status", 0xC0, 0x69},   {"option", 0xC1, 0x109},  {"item", 0xC2, 0x6A},
    {"equip", 0xC3, 0x68},    {"skill", 0xC4, 0x11B},   {"party", 0xC7, 0x67},
    {"map", 0xDB, 0xB2B},     {"quest", 0x169, 0x525},  {"keyboard", 0x172, 0xC38},
    {"guild", 0x175, 0xC3E},  {"battle", 0x178, 0x7D3}, {"booking", 0x17B, 0x1103},
    {"rec", 0x18F, 0x92E},    {"navigation", 0x1AE, 0x931}, {"bank", 0x1CD, 0xC3F},
    {"achievement", 0x1D9, 0xA56}, {"mail", 0x1DC, 0xC40}, {"tip", 0x1FF, 0xCA7},
    {"shop", 0x200, 0xC41},   {"sns", 0x206, 0xB1D},    {"attendance", 0x21C, 0xD91},
    {"adventurerAgency", 0x220, 0xDBA}, {"repute", 0x237, 0xEF3},
    {"adventureguide", 0x245, 0xFD5},   {"probability", 0x24B, 0x1017},
};

// Ask the server to clif_refresh us (reuses the CZ 0x0F04 settings packet with a
// REFRESH id). The client receives the resulting ZC_NPCACK_MAPMOVE (0x91) in its
// recv loop and re-composites the UI — this drops the stale native menu-icon
// "ghost" that lingers after GridClear empties the node list (the already-
// composited pixels aren't re-blitted until a relayout). @refresh proved this
// safe in-game. Sent from OnTick (never mid-Present). Server side: moonlight
// clif_parse_bourgeon_setting case BOURGEON_SETTING_REFRESH -> clif_refresh(sd).
constexpr uint16_t kCzBourgeonSetting      = bopcodes::kSetting;  // 0x0F04 CZ_BOURGEON_SETTING
constexpr uint16_t kBourgeonSettingRefresh = 25;  // matches moonlight e_bourgeon_setting
void RequestServerRefresh() {
  uint8_t buf[10];
  *reinterpret_cast<uint16_t*>(buf)     = kCzBourgeonSetting;
  *reinterpret_cast<uint16_t*>(buf + 2) = 10;
  *reinterpret_cast<uint16_t*>(buf + 4) = kBourgeonSettingRefresh;
  *reinterpret_cast<uint32_t*>(buf + 6) = 0;
  Bourgeon::Instance().SendPacket(buf, sizeof(buf));
}
}  // namespace

void MenuIcons::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);
}

void MenuIcons::BuildIconList() {
  icons_.clear();
  int shown = 0;
  for (const auto& d : kIconTable) {
    if (!IconShown(d.cmd_id)) continue;  // skip native-hidden / non-functional
    Icon ic;
    ic.name = d.name;
    ic.cmd_id = d.cmd_id;
    ic.msg_id = d.msg_id;
    ic.x = 24 + (shown % 6) * 40;   // default grid
    ic.y = 160 + (shown / 6) * 40;
    // Apply any persisted position / visibility for this icon.
    const auto it = saved_.find(d.name);
    if (it != saved_.end() && it->second.valid) {
      if (it->second.x >= 0) ic.x = it->second.x;
      if (it->second.y >= 0) ic.y = it->second.y;
      ic.hidden = it->second.hidden;
    }
    icons_.push_back(ic);
    ++shown;
  }
  icons_built_ = true;
}

// Recopie dans les icônes le signalement « nouveau » tenu par la fenêtre native.
// Appelée depuis OnTick (phase update, ~100 ms) : un badge n'a pas besoin de la
// fréquence d'affichage, et on évite un FindWindow par frame.
void MenuIcons::RefreshBadges() {
  constexpr int kMaxFlagged = 32;
  int flagged[kMaxFlagged];
  const int n = ReadBadgeCmdIds(uiwnd::SafeFindWindow(kMenuIconWndId), flagged,
                                kMaxFlagged);
  for (Icon& ic : icons_) {
    bool on = false;
    for (int i = 0; i < n && !on; ++i) on = (flagged[i] == ic.cmd_id);
    if (!on) ic.new_fail = 0;  // réarme le chargement pour le prochain allumage
    ic.badge = on;
  }
}

float MenuIcons::SnapIcon(float v, float ext, int self, bool y_axis) const {
  constexpr float kSnap = 10.0f;  // px magnetism radius
  float best = v, best_dist = kSnap;
  for (int j = 0; j < static_cast<int>(icons_.size()); ++j) {
    if (j == self || icons_[j].hidden) continue;
    const float opos = y_axis ? static_cast<float>(icons_[j].y)
                              : static_cast<float>(icons_[j].x);
    const float oext = y_axis ? static_cast<float>(icons_[j].h)
                              : static_cast<float>(icons_[j].w);
    const float cands[4] = {opos, opos + oext - ext, opos + oext, opos - ext};
    for (float c : cands) {
      float d = c - v;
      if (d < 0.0f) d = -d;
      if (d < best_dist) { best_dist = d; best = c; }
    }
  }
  return best;
}

void MenuIcons::HideNativeGrid(bool hide) {
  if (hide && !grid_hidden_) {
    if (*reinterpret_cast<uintptr_t*>(kGridDrawSlot) == kGridDrawOrig) {
      PatchValue<void*>(kGridDrawSlot, reinterpret_cast<void*>(&GridClear));
      grid_hidden_ = true;
      // Drop the already-built render nodes now so the native grid disappears
      // immediately (otherwise it lingers until the next relayout / map reload).
      if (void* wnd = uiwnd::FindWindow(kMenuIconWndId))
        GridClear(wnd, nullptr);
      // GridClear empties the node list but the already-composited pixels linger
      // until a relayout — ask the server to clif_refresh so the client
      // re-composites and the ghost vanishes (drained in OnTick, never mid-Present).
      pending_refresh_ = true;
    }
  } else if (!hide && grid_hidden_) {
    PatchValue<void*>(kGridDrawSlot, reinterpret_cast<void*>(kGridDrawOrig));
    grid_hidden_ = false;
    // Symmetric to enable: refresh so the restored native grid re-composites and
    // reappears immediately (otherwise it waits for the next natural relayout).
    pending_refresh_ = true;
  }
}

void MenuIcons::DispatchCommand(int cmd_id) {
  void* wnd = uiwnd::FindWindow(kMenuIconWndId);
  if (!wnd) {
    LogDiag("[MenuIcons] menu-icon window not found for cmd 0x{:X}", cmd_id);
    return;
  }
  // action 6 = button-click command; the handler reads the cmd id from arg3.
  reinterpret_cast<CmdHandlerFn>(kCmdHandler)(wnd, 0, 6, cmd_id, 0, 0, 0);
}

void MenuIcons::FlushPending() {
  if (pending_cmd_ == 0) return;
  const int cmd = pending_cmd_;
  pending_cmd_ = 0;
  // Driven from the game's input phase (ProcessInput, every frame) so the
  // command runs with native click timing/context — never from the Present hook,
  // and never while the HUD is replaced (world map, etc.).
  if (enabled_ && in_game_ && !uiwnd::IsHudReplaced()) DispatchCommand(cmd);
}

// Fallback only: OnTick is throttled to ~100ms, so ProcessInput (which runs
// first each frame) normally drains pending_cmd_ before this ever sees it.
void MenuIcons::OnTick() {
  // Drain a queued server-refresh request here (update phase, never mid-Present)
  // so the client re-composites and the stale native-grid ghost vanishes.
  if (pending_refresh_ && in_game_) {
    pending_refresh_ = false;
    RequestServerRefresh();
  }
  if (enabled_ && in_game_ && icons_built_) RefreshBadges();
  FlushPending();
}

// ── Section « MenuIcons » du panneau Moonlight ──────────────────────────
// Déplacée depuis moonlight_ui/panel_interface.cc : ces widgets ne pilotent
// que l'état de CE plugin. MoonlightUi ne garde que l'appel et la décision
// de sauvegarder. Rend true si un réglage a changé.
bool MenuIcons::DrawSettings() {
  bool changed = false;
  SeparatorText(i18n::Tr("Réglages généraux"));
  changed |= ro::RoCheckbox(i18n::Tr("Rendre les icônes déplaçables"), &enabled_);
  SameLine(); HelpMarker(i18n::Tr("Cache la grille native et recrée les icônes fonctionnelles."));

  ImGui::BeginDisabled(!enabled_);

  changed |= ro::RoCheckbox(i18n::Tr("Mode édition (glisser pour déplacer)"), &edit_mode_);
  SameLine(); HelpMarker(
      i18n::Tr("En mode édition : glisse chaque icône pour la repositionner.\n"
      "Aimantage aux autres icônes et à la grille d'alignement.\n"
      "Désactive le mode pour cliquer les icônes normalement."));

  // Per-icon show/hide. icons() is populated once in-game.
  SeparatorText(i18n::Tr("Icônes"));
  // `icon_list` et non `icons` : la locale masquerait l'accesseur icons() dont
  // elle est issue (elle le faisait, via mi->icons() une fois le préfixe retiré).
  auto& icon_list = icons();
  if (icon_list.empty()) {
    ImGui::TextDisabled(i18n::Tr("(disponible une fois en jeu)"));
  } else {
    for (auto& ic : icon_list) {
      bool shown = !ic.hidden;
      ImGui::PushID(ic.cmd_id);
      if (ro::RoCheckbox(ic.name, &shown)) {
        ic.hidden = !shown;
        saved_[ic.name] = {ic.x, ic.y, ic.hidden, true};
        changed = true;
      }
      ImGui::PopID();
    }
  }

  ImGui::EndDisabled();
  return changed;
}

void MenuIcons::OnRenderUI() {
  if (!enabled_ || !in_game_) {
    HideNativeGrid(false);  // restore native grid when disabled
    return;
  }
  if (Bourgeon::Instance().client().session().aid() == 0) return;

  HideNativeGrid(true);
  if (!icons_built_) BuildIconList();

  // A full-screen UI (world map, etc.) replaces the HUD: hide our icons too
  // (clicks are also rejected in OnTick).

  // Textures D3DPOOL_DEFAULT : mortes après reset/recréation du device -> on nulle
  // les handles cachés pour forcer le rechargement paresseux (sinon draw = crash).
  {
    static unsigned s_epoch = 0;
    const unsigned e = Overlay_DeviceEpoch();
    if (e != s_epoch) {
      for (Icon& ic : icons_) { ic.tex = nullptr; ic.tex_new = nullptr; ic.new_fail = 0; }
      s_epoch = e;
    }
  }

  for (int i = 0; i < static_cast<int>(icons_.size()); ++i) {
    Icon& ic = icons_[i];
    if (ic.hidden) continue;  // user-hidden via the MoonlightUi list
    if (!ic.tex) ic.tex = LoadIconTexture(ic.name, &ic.w, &ic.h);  // lazy/retry
    if (!ic.tex) continue;

    // Icône signalée : on charge son bitmap « _new » (celui qui porte le N) à la
    // première frame où le natif l'allume. Trois échecs et on renonce jusqu'au
    // prochain allumage : la plupart des icônes n'ont PAS de bt_<name>_new.bmp,
    // et une recherche GRF par frame pour un fichier absent se paierait cher.
    if (ic.badge && !ic.tex_new && ic.new_fail < 3) {
      char new_name[64];
      std::snprintf(new_name, sizeof(new_name), "%s_new", ic.name);
      ic.tex_new = LoadIconTexture(new_name, &ic.nw, &ic.nh);
      if (!ic.tex_new) ++ic.new_fail;
    }
    // Le bitmap « _new » déborde vers le haut (le natif le pose 6 px plus haut
    // que l'icône normale, même x). On aligne les BAS sur la hauteur MESURÉE
    // plutôt que de recopier ce 6 : la fenêtre grandit d'autant, sinon ImGui
    // rognerait le badge à son bord. (ic.x, ic.y) reste l'ancre de l'icône
    // normale, donc la position enregistrée ne bouge pas quand le badge s'allume.
    const bool  badge  = ic.badge && ic.tex_new != nullptr;
    const int   draw_w = badge ? ic.nw : ic.w;
    const int   draw_h = badge ? ic.nh : ic.h;
    const float over_y = static_cast<float>(draw_h - ic.h);  // débordement vers le haut

    char id[32];
    std::snprintf(id, sizeof(id), "##micon_%X", ic.cmd_id);
    // Pin the window to the stored position every frame (NoMove) and drive any
    // drag ourselves, so the drawn icon and its hit-rect never desync.
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(ic.x),
                                   static_cast<float>(ic.y) - over_y),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(draw_w),
                                    static_cast<float>(draw_h)), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags f =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;
    ImGui::Begin(id, nullptr, f);
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 p1(p0.x + draw_w, p0.y + draw_h);
    // Coin haut-gauche de l'icône elle-même : décalé du débordement du badge.
    // C'est LUI qui sert au glisser et à l'aimantage, qui raisonnent tous sur
    // (ic.x, ic.y) et sur la taille de l'icône normale.
    const ImVec2 icon_p0(p0.x, p0.y + over_y);

    // Frameless: a transparent full-window button (interaction) + the bitmap
    // drawn directly, so there's no ImGui button frame around the icon.
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    const bool clicked = ImGui::InvisibleButton(
        "b", ImVec2(static_cast<float>(draw_w), static_cast<float>(draw_h)));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddImage((ImTextureID)(uintptr_t)(badge ? ic.tex_new : ic.tex), p0, p1);

    if (edit_mode_) {
      const ImVec2 mp = ImGui::GetIO().MousePos;
      if (ImGui::IsItemActivated()) {
        dragging_   = i;
        drag_off_x_ = mp.x - icon_p0.x;
        drag_off_y_ = mp.y - icon_p0.y;
      }
      if (dragging_ == i && ImGui::IsItemActive()) {
        float nx = mp.x - drag_off_x_, ny = mp.y - drag_off_y_;
        nx = SnapIcon(nx, static_cast<float>(ic.w), i, false);  // magnetic to icons
        ny = SnapIcon(ny, static_cast<float>(ic.h), i, true);
        if (auto* mui = Bourgeon::Instance().moonlight_ui()) {  // shared grid snap
          const ImVec2 ds = ImGui::GetIO().DisplaySize;
          nx = mui->grid_.SnapAxis(nx, ds.x);
          ny = mui->grid_.SnapAxis(ny, ds.y);
        }
        // L'icône reste entièrement dans l'écran de jeu. APRÈS les deux aimantations
        // (icônes voisines + grille), qui peuvent elles-mêmes pousser dehors ; le clamp
        // global de ui/window_clamp.h ne peut rien ici, la fenêtre étant réépinglée à
        // (ic.x,ic.y) en Cond_Always chaque frame.
        const ImVec2 in_screen = ro::ClampWindowPosToScreen(
            ImVec2(nx, ny),
            ImVec2(static_cast<float>(ic.w), static_cast<float>(ic.h)));
        nx = in_screen.x;
        ny = in_screen.y;
        ic.x = static_cast<int>(nx + 0.5f);
        ic.y = static_cast<int>(ny + 0.5f);
      }
      if (dragging_ == i && ImGui::IsItemDeactivated()) {
        dragging_ = -1;
        saved_[ic.name] = {ic.x, ic.y, ic.hidden, true};
        geometry_dirty_ = true;  // MoonlightUi persists once on release
      }
      // Edit-mode highlight so the draggable cells are visible.
      dl->AddRectFilled(p0, p1, IM_COL32(255, 220, 80, hovered ? 70 : 35));
      dl->AddRect(p0, p1, IM_COL32(255, 220, 80, 220));
    } else {
      if (clicked) pending_cmd_ = ic.cmd_id;  // dispatched from OnTick/input phase
      if (hovered) {
        // Utf8 et non Cp949 : cette infobulle est dessinee par ImGui, pas par
        // le moteur de texte natif.
        const char* tip = msgstr::Utf8(ic.msg_id);
        if (*tip) ImGui::SetTooltip("%s", tip);
      }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }
}
