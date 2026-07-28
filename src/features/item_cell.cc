#include "features/item_cell.h"

#include <windows.h>

#include <cstring>

#include "imgui.h"
#include "ragnarok/item_db.h"
#include "ui/ro_imgui.h"

namespace itemcell {
namespace {

// Le `free()` du jeu : BuildDisplayName alloue son vecteur de décalages avec
// l'allocateur du client, on doit le rendre au même.
constexpr uintptr_t kGameFree = 0x00dbbc7f;

// std::vector MSVC tel que le jeu le passe (3 pointeurs).
struct GVec { int* first; int* last; int* end; };

using BuildName_t   = int   (__thiscall*)(void*, void*, int*, GVec*, char**,
                                          size_t*, char**, char, char);
using GetBaseName_t = size_t(__thiscall*)(void*, char*, size_t*, char);
using GameFree_t    = void  (__cdecl*)(void*);

}  // namespace

void BuildDisplayName(void* wnd, void* info, char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  out[0] = '\0';
  // SEH ISOLÉ, et c'est le point important : un item dont BuildDisplayName plante
  // ne doit pas avorter TOUTE l'énumération. Leçon de l'inventaire, où un seul
  // item fautif faisait disparaître la moitié de la liste.
  __try {
    char nbuf[128]; nbuf[0] = '\0';
    char* bufptr = nbuf; size_t ncap = sizeof(nbuf);
    int color_out = 0; char* hl_ptr = nullptr;
    GVec offsets = {nullptr, nullptr, nullptr};
    reinterpret_cast<BuildName_t>(itemdb::kBuildDisplayNameAddr)(
        wnd, info, &color_out, &offsets, &bufptr, &ncap, &hl_ptr, 0, 0);
    size_t k = 0;
    while (k + 1 < out_size && nbuf[k]) { out[k] = nbuf[k]; ++k; }
    out[k] = '\0';
    if (offsets.first) reinterpret_cast<GameFree_t>(kGameFree)(offsets.first);
    if (out[0] == '\0') {
      size_t cap = out_size;
      reinterpret_cast<GetBaseName_t>(itemdb::kBaseNameFallbackAddr)(info, out,
                                                                    &cap, 0);
      out[out_size - 1] = '\0';
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

void DrawTooltip(uint32_t id, const uint32_t* cards, int card_count,
                 const itemdesc::SimpleOpt* opts, int opt_count, int refine,
                 const char* name) {
  if (id == 0) return;
  constexpr float kWidth = 330.0f;  // largeur max (wrap du texte)
  const float edge = ro::DescPanelEdge();
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(255, 255, 255, 255));
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));  // sur fond clair
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(edge, edge));
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(0.0f, 0.0f), ImVec2(kWidth, ImGui::GetIO().DisplaySize.y * 0.8f));
  ImGui::BeginTooltip();
  // Le cadre sysbox se peint DERRIÈRE le texte : on scinde le draw list en deux
  // canaux, le contenu dans le 1, le cadre dans le 0, puis on fusionne.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  itemdesc::RenderSimpleDesc(id, kWidth - 2.0f * edge, cards, card_count, opts,
                             opt_count, refine, name);
  dl->ChannelsSetCurrent(0);
  const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
  ro::DrawDescPanelFrame(dl, pos.x, pos.y, pos.x + size.x, pos.y + size.y, false);
  dl->ChannelsMerge();
  ImGui::EndTooltip();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
}

}  // namespace itemcell
