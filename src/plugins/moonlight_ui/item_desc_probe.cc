#include "plugins/moonlight_ui/internal.h"

#include <Windows.h>

#include "bourgeon.h"
#include "imgui.h"
#include "plugins/item_desc_tweaks.h"
#include "plugins/moonlight_ui.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// ── Sonde de la fenêtre de description d'item ────────────────────────────────
// Détour de la fenêtre native pour capter le nameid de l'item affiché, plus
// l'overlay flottant « alootid » qui s'y accroche. Extraits d'OnRenderUI et du
// constructeur. L'état de la sonde reste PRIVÉ à ce fichier ; seul
// g_last_viewed_item en sort (le panneau Autoloots le lit pour proposer l'item
// survolé) — déclaré extern dans internal.h.

using ItemDescWndFn = int (__fastcall*)(void*, void*, uint32_t, int, int*, int, int, int);
static ItemDescWndFn g_item_desc_wnd_orig  = nullptr;
// Non-static : lu par le panneau Autoloots (moonlight_ui/panel_commands.cc) pour
// proposer l'item survolé. Déclaré extern dans moonlight_ui/internal.h.
uint32_t             g_last_viewed_item    = 0;
static POINT         g_item_desc_cursor    = {0, 0};
static bool          g_item_desc_visible   = false;  // set on 0x18, cleared on close msg
static void*         g_item_desc_wnd_ptr   = nullptr; // ecx of the desc window object

static int __fastcall ItemDescWndHook(void* ecx, void* /*edx*/,
                                       uint32_t p1, int p2, int* p3,
                                       int p4, int p5, int p6) {
  if (p2 == 0x18 && p3 != nullptr) {
    // Ignore 0x18 from secondary windows (e.g. equipment comparison window).
    // Lock onto the first ecx that sends 0x18 while no tooltip is open.
    if (g_item_desc_visible && ecx != g_item_desc_wnd_ptr)
      return g_item_desc_wnd_orig(ecx, nullptr, p1, p2, p3, p4, p5, p6);

    const int* sso = p3 + 11;  // std::string at byte offset 0x2C
    const char* str;
    if (static_cast<uint32_t>(sso[5]) > 15u)
      str = *reinterpret_cast<const char* const*>(sso);
    else
      str = reinterpret_cast<const char*>(sso);
    if (str) {
      const long id = std::atol(str);
      if (id > 0) {
        if (g_item_desc_visible && static_cast<uint32_t>(id) == g_last_viewed_item) {
          // Same item re-clicked while visible = toggle-close (no native 0x06).
          g_item_desc_visible = false;
          g_item_desc_wnd_ptr = nullptr;
        } else {
          g_last_viewed_item  = static_cast<uint32_t>(id);
          g_item_desc_visible = true;
          g_item_desc_wnd_ptr = ecx;
          GetCursorPos(&g_item_desc_cursor);
        }
      }
    }
  } else if (p2 == 0x06 && ecx == g_item_desc_wnd_ptr) {
    // X button close — also reset ptr for the same reason.
    g_item_desc_visible = false;
    g_item_desc_wnd_ptr = nullptr;
  }
  const int ret = g_item_desc_wnd_orig(ecx, nullptr, p1, p2, p3, p4, p5, p6);
  // Enriched descriptions (Option A) : cacher la fenêtre native DÈS qu'elle est
  // posée/affichée (msg 0x18 set-item, 0x22 restore-pos) -> l'utilisateur ne voit
  // jamais la native (sinon flicker de ~100ms le temps du OnTick throttlé).
  if (p2 == 0x18 || p2 == 0x22) {
    if (auto* idt = Bourgeon::Instance().item_desc())
      idt->HideNativeDescWindows();  // item 0xc + comparaison 0xea (déjà créée)
  }
  return ret;
}

// Pose le détour. Appelé une fois depuis le constructeur.
// MÉTHODE MEMBRE : kItemDescWndAddr est une constante PRIVÉE de la classe.
void MoonlightUi::InstallItemDescProbe() {
// Hook the item description window to capture the nameid when right-clicking.
g_item_desc_wnd_orig = reinterpret_cast<ItemDescWndFn>(
    hooking::HookManager::Instance().SetHook(
        hooking::HookType::kJmpHook,
        reinterpret_cast<uint8_t*>(kItemDescWndAddr),
        reinterpret_cast<uint8_t*>(ItemDescWndHook)));
if (!g_item_desc_wnd_orig) {
  LogError("[MoonlightUi] failed to hook item desc wnd at 0x{:08X}",
           kItemDescWndAddr);
}
}

// Overlay flottant « alootid », dessiné APRÈS la fenêtre principale : il ne vit
// pas dedans, il s'ancre sur la fenêtre de description native.
void MoonlightUi::DrawAlootOverlay() {
  // ── Alootid floating overlay ───────────────────────────────────────────────
  // Detect silent tooltip close (e.g. comparison→non-comparison): the game
  // zeroes kItemDescWndGlobalPtr without sending our hook a close message.
  if (g_item_desc_visible &&
      *reinterpret_cast<const uintptr_t*>(kItemDescWndGlobalPtr) == 0) {
    g_item_desc_visible = false;
    g_item_desc_wnd_ptr = nullptr;
  }

  // Quand les descriptions enrichies (Option A) sont actives, la fenêtre native
  // est cachée -> l'overlay alootid autonome n'aurait pas d'ancrage cohérent et
  // fera doublon avec le bouton qui sera réintégré DANS le cadre enrichi. On le
  // désactive donc tant que le panneau item enrichi est activé.
  bool enriched_item = false;
  if (auto* idt = Bourgeon::Instance().item_desc())
    enriched_item = idt->show_item_panel();

  if (show_alootid_overlay_ && !enriched_item &&
      g_last_viewed_item != 0 && g_item_desc_visible) {
    // Position de la fenêtre native, lue dans l'objet dont on a gardé le pointeur.
    // Offsets confirmés : [ptr+0x1C] = X, [ptr+0x20] = Y. (Le commentaire d'avant
    // annonçait « +0x18=Y, +0x20=X » — il ne correspondait pas au code, qui lui
    // était juste.) À défaut, on retombe sur la position du curseur au moment de
    // l'ouverture.
    float overlay_x = static_cast<float>(g_item_desc_cursor.x) + 12.0f;
    float overlay_y = static_cast<float>(g_item_desc_cursor.y) + 12.0f;
    // Le pointeur est conservé D'UNE FRAME À L'AUTRE : avant de le déréférencer,
    // vérifier qu'il DÉSIGNE TOUJOURS la fenêtre de description vivante. Le garde
    // du dessus ne couvre que la fermeture (global remis à zéro) ; si le jeu
    // alloue une autre fenêtre à la place, le global redevient non nul mais
    // pointe ailleurs — on lisait alors +0x1C/+0x20 dans un objet étranger. Le
    // test de plausibilité qui suit valide les VALEURS, jamais le pointeur.
    const void* live_wnd = *reinterpret_cast<void* const*>(kItemDescWndGlobalPtr);
    if (g_item_desc_wnd_ptr != nullptr && g_item_desc_wnd_ptr == live_wnd) {
      const auto* base = static_cast<const uint8_t*>(g_item_desc_wnd_ptr);
      const int wx = *reinterpret_cast<const int*>(base + 0x1C);  // X (confirmé)
      const int wy = *reinterpret_cast<const int*>(base + 0x20);  // Y (confirmé)
      if (wx > 0 && wx < 4096 && wy > 0 && wy < 4096) {
        overlay_x = static_cast<float>(wx);
        overlay_y = static_cast<float>(wy) - 24.0f;
      }
    }
    ImGui::SetNextWindowPos(ImVec2(overlay_x, overlay_y), ImGuiCond_Always);  // live-track
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    constexpr ImGuiWindowFlags kOverlayFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##alootid_overlay", nullptr, kOverlayFlags)) {
      const auto itv = item_names_.find(g_last_viewed_item);
      if (itv != item_names_.end())
        TextUnformatted(itv->second.c_str());
      else
        ImGui::Text("[%u]", g_last_viewed_item);

      int ov_idx = -1;
      for (int k = 0; k < static_cast<int>(aloot_ids_.size()); ++k)
        if (aloot_ids_[k] == g_last_viewed_item) { ov_idx = k; break; }

      SameLine();
      if (ov_idx >= 0) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.18f, 0.18f, 1.0f));
        if (ImGui::SmallButton("- alootid")) {
          SendSetting(kSettingAlootIdRemove, aloot_ids_[ov_idx]);
          aloot_ids_.erase(aloot_ids_.begin() + ov_idx);
        }
        ImGui::PopStyleColor();
      } else {
        const bool can_add = (aloot_ids_.size() < 50);
        if (!can_add) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.18f, 1.0f));
        if (ImGui::SmallButton("+ alootid")) {
          aloot_ids_.push_back(g_last_viewed_item);
          SendSetting(kSettingAlootId, g_last_viewed_item);
        }
        ImGui::PopStyleColor();
        if (!can_add) ImGui::EndDisabled();
      }
      SameLine();
      if (ImGui::SmallButton("x"))
        g_item_desc_visible = false;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }
}
