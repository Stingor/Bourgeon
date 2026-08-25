#include "ragnarok/item_db.h"
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "features/moonlight_ui/internal.h"

#include <Windows.h>

#include "bourgeon.h"
#include "imgui.h"
#include "features/windows/item_desc_window.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "ui/ro_imgui.h"
#include "ragnarok/uiwnd.h"  // uiwnd::kItemDescWndSlot
#include "ui/ro_widgets.h"
#include "utils/hooking/hook_manager.h"
#include "utils/i18n.h"
#include "utils/log_console.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Sonde de la fenêtre de description d'item ────────────────────────────────
// Détour de la fenêtre native pour capter le nameid de l'item affiché, plus
// l'overlay flottant « alootid » qui s'y accroche. Extraits d'OnRenderUI et du
// constructeur. L'état de la sonde reste PRIVÉ à ce fichier ; seul
// g_last_viewed_item en sort (le panneau Autoloots le lit pour proposer l'item
// survolé) — déclaré extern dans internal.h.

// Messages reçus par l'OnMsg de la fenêtre de description native.
constexpr int kMsgClose       = 0x06;  // bouton X
constexpr int kMsgRestorePos  = 0x22;  // repositionnement après restauration

using ItemDescWndFn = int (__fastcall*)(void*, void*, uint32_t, int, int*, int, int, int);
static ItemDescWndFn g_item_desc_wnd_orig  = nullptr;
// Non-static : lu par le panneau Autoloots (moonlight_ui/panel_commands.cc) pour
// proposer l'item survolé. Déclaré extern dans moonlight_ui/internal.h.
uint32_t             g_last_viewed_item    = 0;
static POINT         g_item_desc_cursor    = {0, 0};
static bool          g_item_desc_visible   = false;  // set on 0x18, cleared on close msg
static void*         g_item_desc_wnd_ptr   = nullptr; // ecx of the desc window object

// Les paramètres s'appelaient p1..p6 — des noms Ghidra promus en identifiants
// C++, ce que la convention du projet interdit : ils appartiennent aux
// COMMENTAIRES, où ils font le pont avec le décompilé. Le sens de chacun était
// déjà documenté juste à côté ; il est maintenant DANS le nom. (Ghidra :
// p1=param_3, p2=param_4, p3=param_5, p4..p6=param_6..8.)
static int __fastcall ItemDescWndHook(void* ecx, void* /*edx*/,
                                      uint32_t wparam, int msg_id, int* item_data,
                                      int arg4, int arg5, int arg6) {
  if (msg_id == itemdb::kItemDescMsgSet && item_data != nullptr) {
    // Ignore 0x18 from secondary windows (e.g. equipment comparison window).
    // Lock onto the first ecx that sends 0x18 while no tooltip is open.
    if (g_item_desc_visible && ecx != g_item_desc_wnd_ptr)
      return g_item_desc_wnd_orig(ecx, nullptr, wparam, msg_id, item_data,
                                  arg4, arg5, arg6);

    // std::string MSVC à l'offset 0x2C (soit `item_data + 11` mots). La règle
    // SSO vient du foyer : elle était écrite ici indexée en MOTS (`[5]` = +0x14),
    // une forme qu'aucune recherche par décalage ne pouvait trouver.
    const int* nameid_msvc_string = item_data + 11;
    const char* nameid_text = rag::clientstr::Data(nameid_msvc_string);
    if (nameid_text) {
      const long id = std::atol(nameid_text);
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
          // Vraie ouverture (ou chargement d'un AUTRE objet dans une desc déjà
          // ouverte) : la description doit repasser devant. C'est ici, et nulle part
          // ailleurs, que toutes les ouvertures convergent — clic droit natif comme
          // itemcell::OpenDesc* de nos fenêtres ImGui, lesquelles ont le focus et
          // recouvriraient la desc. Un simple drapeau, consommé au rendu.
          if (auto* item_desc = Bourgeon::Instance().item_desc())
            item_desc->RaiseItemWindow();
        }
      }
    }
  } else if (msg_id == kMsgClose && ecx == g_item_desc_wnd_ptr) {
    // X button close — also reset ptr for the same reason.
    g_item_desc_visible = false;
    g_item_desc_wnd_ptr = nullptr;
  }
  const int ret = g_item_desc_wnd_orig(ecx, nullptr, wparam, msg_id, item_data,
                                       arg4, arg5, arg6);
  // Enriched descriptions (Option A) : cacher la fenêtre native DÈS qu'elle est
  // posée/affichée (msg 0x18 set-item, 0x22 restore-pos) -> l'utilisateur ne voit
  // jamais la native (sinon flicker de ~100ms le temps du OnTick throttlé).
  if (msg_id == itemdb::kItemDescMsgSet || msg_id == kMsgRestorePos) {
    if (auto* item_desc = Bourgeon::Instance().item_desc())
      item_desc->HideNativeDescWindows();  // item 0xc + comparaison 0xea (déjà créée)
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
  // zeroes uiwnd::kItemDescWndSlot without sending our hook a close message.
  if (g_item_desc_visible &&
      *reinterpret_cast<const uintptr_t*>(uiwnd::kItemDescWndSlot) == 0) {
    g_item_desc_visible = false;
    g_item_desc_wnd_ptr = nullptr;
  }

  // Quand les descriptions enrichies (Option A) sont actives, la fenêtre native
  // est cachée -> l'overlay alootid autonome n'aurait pas d'ancrage cohérent et
  // fera doublon avec le bouton qui sera réintégré DANS le cadre enrichi. On le
  // désactive donc tant que le panneau item enrichi est activé.
  bool enriched_item_panel_on = false;
  if (auto* item_desc = Bourgeon::Instance().item_desc())
    enriched_item_panel_on = item_desc->show_item_panel();

  if (show_alootid_overlay_ && !enriched_item_panel_on &&
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
    const void* live_wnd = *reinterpret_cast<void* const*>(uiwnd::kItemDescWndSlot);
    if (g_item_desc_wnd_ptr != nullptr && g_item_desc_wnd_ptr == live_wnd) {
      const auto* desc_wnd_bytes = static_cast<const uint8_t*>(g_item_desc_wnd_ptr);
      const int wnd_x = *reinterpret_cast<const int*>(desc_wnd_bytes + 0x1C);
      const int wnd_y = *reinterpret_cast<const int*>(desc_wnd_bytes + 0x20);
      if (wnd_x > 0 && wnd_x < 4096 && wnd_y > 0 && wnd_y < 4096) {
        overlay_x = static_cast<float>(wnd_x);
        overlay_y = static_cast<float>(wnd_y) - 24.0f;
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
      // ⚠ Le rouge et le vert qui teintaient ces deux boutons ont été RETIRÉS
      // avec la conversion : `RoSmallButton` peint son 9-slice lui-même et
      // IGNORE `ImGuiCol_Button` — la couleur serait devenue un `PushStyleColor`
      // mort. Ce n'est pas une perte : c'est le LIBELLÉ qui porte l'état, « -
      // alootid » face à « + alootid », et il le dit mieux qu'une teinte.
      if (ov_idx >= 0) {
        if (ro::RoSmallButton(i18n::Tr("- alootid"))) {
          SendSetting(kSettingAlootIdRemove, aloot_ids_[ov_idx]);
          aloot_ids_.erase(aloot_ids_.begin() + ov_idx);
        }
      } else {
        const bool can_add = (aloot_ids_.size() < 50);
        if (!can_add) ImGui::BeginDisabled();
        if (ro::RoSmallButton(i18n::Tr("+ alootid"))) {
          aloot_ids_.push_back(g_last_viewed_item);
          SendSetting(kSettingAlootId, g_last_viewed_item);
        }
        if (!can_add) ImGui::EndDisabled();
      }
      SameLine();
      if (ro::RoSmallButton("x"))
        g_item_desc_visible = false;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }
}
