#include "features/item_cell.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "imgui.h"
#include "ragnarok/item_db.h"
#include "ragnarok/uiwnd.h"
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

using InfoCtor_t     = void(__fastcall*)(void*);
using InfoSetId_t    = void(__thiscall*)(void*, int);
using EnsureLoaded_t = char(__thiscall*)(void*, int);
using DescLookup_t   = void*(__cdecl*)(int, void*);

// Décalage du nom de base dans l'enregistrement de la DB de descriptions.
constexpr int kDescRecName = 0x04;

// Cache de noms, un seul pour tout le client. Il remplace six caches privés qui
// résolvaient et stockaient chacun les mêmes chaînes.
std::unordered_map<uint32_t, std::string> g_name_cache;

// SEH ISOLÉ et POD SEULEMENT : aucune std::string dans cette portée, sinon MSVC
// refuse le __try (C2712, objet à destructeur déroulable). D'où le passage par
// `out` plutôt qu'un retour de chaîne.
void ResolveNameSEH(uint32_t id, char* out, size_t cap) {
  out[0] = '\0';
  __try {
    // La DB n'est peuplée qu'à la demande : sans ce coup de pouce, le premier
    // affichage d'un item jamais consulté rendrait « #<id> ».
    void* cache = *reinterpret_cast<void**>(itemdb::kEnsureCachePtr);
    if (cache)
      reinterpret_cast<EnsureLoaded_t>(itemdb::kEnsureLoadedAddr)(
          cache, static_cast<int>(id));
    void* rec = reinterpret_cast<DescLookup_t>(itemdb::kLookupAddr)(
        static_cast<int>(id), reinterpret_cast<void*>(itemdb::kTableAddr));
    // kNilAddr est la sentinelle de l'arbre : y aboutir signifie « absent », la
    // déréférencer lirait le nœud bidon.
    if (rec && rec != reinterpret_cast<void*>(itemdb::kNilAddr)) {
      const char* nm =
          *reinterpret_cast<char**>(reinterpret_cast<char*>(rec) + kDescRecName);
      if (nm) { std::strncpy(out, nm, cap - 1); out[cap - 1] = '\0'; }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

// ── std::list<ItemSkillInfo> de session : nœud et champs lus ──────────────────
// Nœud MSVC : next@+0, prev@+4, value@+8 — `value` EST l'ItemSkillInfo.
constexpr int kNodeNext  = 0x00;
constexpr int kNodeInfo  = 0x08;
constexpr int kInfoIndex = 0x04;  // int : index client (arg des commandes)
constexpr int kInfoIdStr = 0x2c;  // std::string : l'itemId EN TEXTE (le jeu fait atoi)
constexpr int kInfoIdCap = 0x40;  // capacité SSO (= +0x2c+0x14) ; >15 => heap

// Plafond du parcours : simple garde-fou anti-boucle (la liste s'arrête sur sa
// sentinelle). Une seule valeur pour les trois listes — la plus grosse, celle du
// storage premium, plafonne à 600 côté serveur.
constexpr int kWalkGuard = 4000;

// ItemSkillInfo tel que le natif le passe à OnMsg 0x18. 0x100 octets : la
// structure en fait 0xf8, on arrondit (cf. le memcpy 0x5c..0xf8 du pont `src`).
constexpr size_t kInfoSize = 0x100;

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

int SlotCount(void* info) {
  if (!info) return 0;
  __try {
    return reinterpret_cast<int(__fastcall*)(void*)>(itemdb::kSlotCountAddr)(info);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

const char* NameById(uint32_t id) {
  auto it = g_name_cache.find(id);
  if (it != g_name_cache.end()) return it->second.c_str();
  // 128 et non 64 : la conversion se fait sur des octets CP949, et couper au
  // milieu d'une paire d'octets donnerait une séquence UTF-8 invalide.
  char buf[128];
  ResolveNameSEH(id, buf, sizeof(buf));
  if (buf[0] == '\0') std::snprintf(buf, sizeof(buf), "#%u", id);
  // Les noms de la DB sont en CP949 et ImGui attend de l'UTF-8. C'est identique
  // pour l'immense majorité d'entre eux — sur les 27 219 noms d'itemInfoMerged
  // .lua, 27 199 sont en ASCII, où la conversion ne change pas un octet — mais
  // pas pour les 20 restants (reliquats coréens : « 수라 Shadow Cube »…), que
  // rendre bruts donnerait de l'UTF-8 invalide. Même traitement que
  // MoonlightUi::LoadItemNames, qui lit ce même fichier.
  //
  // Converti ICI, une fois à l'insertion : le tampon de ro::Cp949ToUtf8 est
  // thread-local et ROTATIF sur huit emplacements, la copie dans la std::string
  // le met hors de portée. On mémorise même l'échec — un id absent de la DB le
  // restera, et réessayer à chaque frame relancerait le chargement paresseux
  // pour rien.
  return (g_name_cache[id] = ro::Cp949ToUtf8(buf)).c_str();
}

const char* Label(char* out, size_t out_size, const char* name, int slots) {
  if (!out || out_size == 0) return "";
  const char* base = (name && name[0]) ? name : "(?)";
  if (slots > 0) std::snprintf(out, out_size, "%s [%d]", base, slots);
  else           std::snprintf(out, out_size, "%s", base);
  return out;
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

// ── Ouverture de la fenêtre de description (0x0c) ────────────────────────────

void OpenDescFromInfo(const void* info, int mx, int my) {
  if (!info) return;
  __try {
    void* dwnd = uiwnd::MakeWindow(itemdb::kItemDescWndId);
    if (!dwnd) return;
    // OnMsg 0x18 COPIE l'info dans wnd+0xb8 : on ne cède rien, et l'objet du jeu
    // n'est pas modifié. C'est ce qui rend l'appel sûr sur un nœud vivant.
    uiwnd::OnMsg(dwnd, itemdb::kItemDescMsgSet,
                 static_cast<int>(reinterpret_cast<uintptr_t>(info)), 0, 0, 0);
    uiwnd::SetPos(dwnd, mx, my);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void OpenDescById(uint32_t id, uint16_t view, uint32_t location, int mx, int my,
                  const void* src) {
  if (id == 0) return;
  __try {
    uint8_t info[kInfoSize];
    std::memset(info, 0, sizeof(info));
    reinterpret_cast<InfoCtor_t>(itemdb::kInfoCtorAddr)(info);
    reinterpret_cast<InfoSetId_t>(itemdb::kInfoSetIdAddr)(info, static_cast<int>(id));
    if (src) {
      // Le name-builder natif (0x008a0570) décore le nom à partir du TYPE @0,
      // des cartes @0x1c-0x28, du refine @0x60, du grade @0x88 : sans ces
      // champs il rend le nom NU. On saute les deux std::string (@0x2c id,
      // @0x44 resname) que SetId vient de construire — les recopier ferait
      // partager un tampon heap entre deux ItemSkillInfo.
      const uint8_t* s = reinterpret_cast<const uint8_t*>(src);
      std::memcpy(info + 0x00, s + 0x00, 0x2c);         // type .. cartes
      std::memcpy(info + 0x5c, s + 0x5c, 0xf8 - 0x5c);  // identifié/refine/view/grade/options
    } else {
      *reinterpret_cast<uint32_t*>(info + 0x08) = location;  // equip point : gate « aperçu »
      *reinterpret_cast<uint32_t*>(info + 0x70) = view;      // viewID      : gate « aperçu »
    }
    info[0x5c] = 1;  // identifié : resname/desc lus dans l'enregistrement DB
    // Chargement paresseux du DB : sans ça la description serait vide au premier
    // affichage d'un item jamais consulté.
    void* cache = *reinterpret_cast<void**>(itemdb::kEnsureCachePtr);
    if (cache)
      reinterpret_cast<EnsureLoaded_t>(itemdb::kEnsureLoadedAddr)(cache, static_cast<int>(id));
    void* dwnd = uiwnd::MakeWindow(itemdb::kItemDescWndId);
    if (dwnd) {
      uiwnd::OnMsg(dwnd, itemdb::kItemDescMsgSet,
                   static_cast<int>(reinterpret_cast<uintptr_t>(info)), 0, 0, 0);
      uiwnd::SetPos(dwnd, mx, my);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Parcours des listes de session ───────────────────────────────────────────

void* FindInfoById(uintptr_t list_head, uint32_t id) {
  if (id == 0) return nullptr;
  __try {
    uint8_t* head = *reinterpret_cast<uint8_t**>(list_head);
    if (!head) return nullptr;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
    for (int guard = 0; node && node != head && guard < kWalkGuard; ++guard) {
      uint8_t* info = node + kNodeInfo;
      const uint32_t cap = *reinterpret_cast<uint32_t*>(info + kInfoIdCap);
      const char* ids = (cap > 0xf) ? *reinterpret_cast<char**>(info + kInfoIdStr)
                                    : reinterpret_cast<const char*>(info + kInfoIdStr);
      if (ids && static_cast<uint32_t>(atoi(ids)) == id) return info;
      node = *reinterpret_cast<uint8_t**>(node + kNodeNext);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return nullptr;
}

void* FindInfoByIndex(uintptr_t list_head, int index) {
  __try {
    uint8_t* head = *reinterpret_cast<uint8_t**>(list_head);
    if (!head) return nullptr;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
    for (int guard = 0; node && node != head && guard < kWalkGuard; ++guard) {
      uint8_t* info = node + kNodeInfo;
      if (*reinterpret_cast<int*>(info + kInfoIndex) == index) return info;
      node = *reinterpret_cast<uint8_t**>(node + kNodeNext);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return nullptr;
}

void DrawTile(ImDrawList* draw_list, const ImVec2& p0, const ImVec2& p1,
              float cell, const ro::IconTex& icon, int refine, int amount) {
  if (!draw_list) return;

  if (icon.tex && icon.w > 0 && icon.h > 0) {
    float dw = static_cast<float>(icon.w), dh = static_cast<float>(icon.h);
    if (dw > cell || dh > cell) {  // réduire seulement, jamais agrandir
      const float s = cell / (dw > dh ? dw : dh);
      dw *= s; dh *= s;
    }
    const ImVec2 ip(p0.x + (cell - dw) * 0.5f, p0.y + (cell - dh) * 0.5f);
    draw_list->AddImage(reinterpret_cast<ImTextureID>(icon.tex), ip,
                        ImVec2(ip.x + dw, ip.y + dh), ImVec2(0, 0), ImVec2(1, 1),
                        ro::SkinImageTint());
  } else {
    draw_list->AddText(ImVec2(p0.x + cell * 0.5f - 4, p0.y + cell * 0.5f - 7),
                       ImGui::GetColorU32(ImGuiCol_TextDisabled), "?");
  }

  char badge[16] = {0};
  if (refine > 0)      std::snprintf(badge, sizeof(badge), "+%d", refine);
  else if (amount > 1) std::snprintf(badge, sizeof(badge), "%d", amount);
  if (badge[0]) {
    const ImVec2 ts = ImGui::CalcTextSize(badge);
    const ImVec2 bp(p1.x - ts.x - 2, p1.y - ts.y - 1);
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    for (int oy = -1; oy <= 1; ++oy)      // cerne : les 8 voisins en blanc…
      for (int ox = -1; ox <= 1; ++ox)
        if (ox || oy) draw_list->AddText(ImVec2(bp.x + ox, bp.y + oy), white, badge);
    draw_list->AddText(bp, IM_COL32(0, 0, 0, 255), badge);  // …puis le noir dessus
  }
}

}  // namespace itemcell
