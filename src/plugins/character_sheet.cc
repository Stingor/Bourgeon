#include "plugins/character_sheet.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "bourgeon.h"        // Bourgeon::Instance().SendPacket / session
#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB
#include "imgui.h"
#include "plugins/imgui_escape.h"
#include "ui/ro_imgui.h"

//  Constantes RE (client 20250716, base 0x400000 ; cf. project_character_sheet)
namespace {

constexpr uintptr_t kSession = 0x015fa3c0;

//  Tableau equip : session + base + slot*0xf8 (in-place, aucun appel C++)
constexpr int kEquipBase   = 0x17d0;   // equipement normal
constexpr int kCostumeBase = 0x2b30;   // costume
constexpr int kSlotStride  = 0xf8;
// Offsets d'une entree (ItemSkillInfo).
constexpr int keInvIndex = 0x04;   // index inventaire (a envoyer pour desequiper)
constexpr int keLocation = 0x08;   // masque EQP
constexpr int kePresent  = 0x10;   // ==1 si occupe
constexpr int keResname  = 0x2c;   // std::string (SSO) : itemId en texte
constexpr int keResCap   = 0x40;   // capacite SSO (>15 => heap)
constexpr int keRefine   = 0x60;
constexpr int keView     = 0x70;

//  Globals stats (cf. project_status_tweaks_plugin)
constexpr uintptr_t kStatBase    = 0x015fba24;  // STR..LUK base (step 4)
constexpr uintptr_t kStatBonus   = 0x015fba0c;  // bonus
constexpr uintptr_t kRaiseCost   = 0x015fba3c;  // cout de montee
constexpr uintptr_t kStatusPoint = 0x015fb9f4;
constexpr uintptr_t kAtk1 = 0x015fba58, kAtk2 = 0x015fba6c;
constexpr uintptr_t kDefSoft = 0x015fba64, kDefHard = 0x015fba68;
constexpr uintptr_t kMatkMax = 0x015fba70, kMatkMin = 0x015fba74;
constexpr uintptr_t kMdefSoft = 0x015fba5c, kMdefHard = 0x015fba78;
constexpr uintptr_t kHit = 0x015fba7c, kCrit = 0x015fba84;
constexpr uintptr_t kFlee = 0x015fba80, kPdodge = 0x015fba88, kAspdRaw = 0x015fba54;
constexpr uintptr_t kBaseLvl = 0x015fb9f0, kJobLvl = 0x015fb9f8;
constexpr uintptr_t kHp = 0x015ff908, kHpMax = 0x015ff90c;
constexpr uintptr_t kSp = 0x015ff910, kSpMax = 0x015ff914;

//  Opcodes (raw, envoyes via Bourgeon::SendPacket comme le shop)
constexpr uint16_t kOpUnequip = 0x00AB;  // CZ_REQ_TAKEOFF_EQUIP {op, invIndex}
constexpr uint16_t kOpStatUp  = 0x00BB;  // CZ_STATUS_CHANGE {op, statType, amount}
const int   kStatType[6] = {0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12};  // STR..LUK
const char* kStatName[6] = {"STR", "AGI", "VIT", "INT", "DEX", "LUK"};

//  Description d'item : MakeWindow(0xc) + OnMsg 0x18 (cf. cashshop_tweaks)
constexpr uintptr_t kUIWindowMgr = 0x0131f4e8;
constexpr uintptr_t kMakeWindow  = 0x00a39340;
constexpr int kWinItemDesc = 0xc, kMsgSetItem = 0x18, kVfOnMsg = 0x94, kVfSetPos = 0x10;
constexpr uintptr_t kInfoCtor  = 0x006a1b20;
constexpr uintptr_t kInfoSetId = 0x006a6570;
constexpr uintptr_t kEnsureLoaded = 0x006a06b0, kEnsureCache = 0x0125510c;
constexpr uintptr_t kDescDbLookup = 0x006a0d40, kDescDb = 0x01255130, kDescDbNil = 0x01255138;
using MakeWindow_t   = void* (__fastcall*)(void*, void*, void*);
using OnMsg_t        = int (__fastcall*)(void*, void*, int, int, int, int, int, int);
using SetPos_t       = void(__fastcall*)(void*, void*, int, int);
using InfoCtor_t     = void(__fastcall*)(void*);
using InfoSetId_t    = void(__thiscall*)(void*, int);
using EnsureLoaded_t = char (__thiscall*)(void*, int);
using DescLookup_t   = void*(__cdecl*)(int, void*);

//  Icone d'item (item\<resname>.bmp)
constexpr uintptr_t kBuildIconPath = 0x00d5a720;
constexpr uintptr_t kTexMgr = 0x00a90350, kMakeKey = 0x00a9f030, kLoadTex = 0x00a8d4a0;
constexpr int kTexW = 0x114, kTexH = 0x118, kTexPix = 0x11c;
using BuildIconPath_t = void*(__stdcall*)(const char*, char*, int);
using TexMgr_t  = void*(__cdecl*)();
using MakeKey_t = void*(__cdecl*)(const char*);
using LoadTex_t = void*(__fastcall*)(void*, void*, void*);

template <typename Fn>
inline Fn Vf(void* self, int off) {
  return reinterpret_cast<Fn>((*reinterpret_cast<uintptr_t**>(self))[off / 4]);
}

int ReadInt(uintptr_t addr) {
  __try { return *reinterpret_cast<const int*>(addr); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

//  Lecture in-place d'un slot equipe (SEH, POD)
struct EquipItem {
  bool     present = false;
  uint32_t nameid = 0;
  int      invIndex = 0;
  int      refine = 0;
  int      viewId = 0;
  int      location = 0;
};
bool ReadEquipSlot(int slot, bool costume, EquipItem* out) {
  __try {
    const uintptr_t base =
        kSession + (costume ? kCostumeBase : kEquipBase) + slot * kSlotStride;
    const uint8_t* e = reinterpret_cast<const uint8_t*>(base);
    const int invIndex = *reinterpret_cast<const int*>(e + keInvIndex);
    const int present  = *reinterpret_cast<const int*>(e + kePresent);
    if (invIndex == 0 || present != 1) return false;  // slot vide
    out->present  = true;
    out->invIndex = invIndex;
    out->location = *reinterpret_cast<const int*>(e + keLocation);
    out->refine   = *reinterpret_cast<const int*>(e + keRefine);
    out->viewId   = *reinterpret_cast<const int*>(e + keView);
    const uint32_t cap = *reinterpret_cast<const uint32_t*>(e + keResCap);
    const char* rn = (cap > 15) ? *reinterpret_cast<const char* const*>(e + keResname)
                                : reinterpret_cast<const char*>(e + keResname);
    out->nameid = (rn && rn[0]) ? static_cast<uint32_t>(std::atoi(rn)) : 0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

//  Lecture de toutes les stats en un bloc (SEH)
struct Stats {
  int base[6], bonus[6], raise[6], points;
  int atk1, atk2, def_s, def_h, matk_min, matk_max, mdef_s, mdef_h;
  int hit, crit, flee, pdodge, aspd_raw, base_lvl, job_lvl;
  int hp, hp_max, sp, sp_max;
};
bool ReadStats(Stats* s) {
  __try {
    for (int i = 0; i < 6; ++i) {
      s->base[i]  = *reinterpret_cast<const int*>(kStatBase + i * 4);
      s->bonus[i] = *reinterpret_cast<const int*>(kStatBonus + i * 4);
      s->raise[i] = *reinterpret_cast<const int*>(kRaiseCost + i * 4);
    }
    s->points   = *reinterpret_cast<const int*>(kStatusPoint);
    s->atk1     = *reinterpret_cast<const int*>(kAtk1);
    s->atk2     = *reinterpret_cast<const int*>(kAtk2);
    s->def_s    = *reinterpret_cast<const int*>(kDefSoft);
    s->def_h    = *reinterpret_cast<const int*>(kDefHard);
    s->matk_min = *reinterpret_cast<const int*>(kMatkMin);
    s->matk_max = *reinterpret_cast<const int*>(kMatkMax);
    s->mdef_s   = *reinterpret_cast<const int*>(kMdefSoft);
    s->mdef_h   = *reinterpret_cast<const int*>(kMdefHard);
    s->hit      = *reinterpret_cast<const int*>(kHit);
    s->crit     = *reinterpret_cast<const int*>(kCrit);
    s->flee     = *reinterpret_cast<const int*>(kFlee);
    s->pdodge   = *reinterpret_cast<const int*>(kPdodge);
    s->aspd_raw = *reinterpret_cast<const int*>(kAspdRaw);
    s->base_lvl = *reinterpret_cast<const int*>(kBaseLvl);
    s->job_lvl  = *reinterpret_cast<const int*>(kJobLvl);
    s->hp       = *reinterpret_cast<const int*>(kHp);
    s->hp_max   = *reinterpret_cast<const int*>(kHpMax);
    s->sp       = *reinterpret_cast<const int*>(kSp);
    s->sp_max   = *reinterpret_cast<const int*>(kSpMax);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Nom de classe (comme UIBasicInfoWnd), SEH-garde (renvoie POD const char*).
const char* ClassNameSEH() {
  __try {
    using GetJobId_t     = int (__fastcall*)(void*, void*);
    using GetClassName_t = const char* (__fastcall*)(void*, void*, unsigned, int);
    const int jobid = reinterpret_cast<GetJobId_t>(0x00d5b580)(
        reinterpret_cast<void*>(kSession), nullptr);
    const char* n = reinterpret_cast<GetClassName_t>(0x00d5bb40)(
        reinterpret_cast<void*>(kSession), nullptr, static_cast<unsigned>(jobid), -1);
    return n ? n : "";
  } __except (EXCEPTION_EXECUTE_HANDLER) { return ""; }
}

//  Cache nom d'item (id -> nom)
std::unordered_map<uint32_t, std::string> g_name_cache;
void ResolveNameSEH(uint32_t id, char* out, size_t cap) {
  out[0] = '\0';
  __try {
    void* cache = *reinterpret_cast<void**>(kEnsureCache);
    if (cache)
      reinterpret_cast<EnsureLoaded_t>(kEnsureLoaded)(cache, static_cast<int>(id));
    void* rec = reinterpret_cast<DescLookup_t>(kDescDbLookup)(
        static_cast<int>(id), reinterpret_cast<void*>(kDescDb));
    if (rec && rec != reinterpret_cast<void*>(kDescDbNil)) {
      const char* nm = *reinterpret_cast<char**>(reinterpret_cast<char*>(rec) + 4);
      if (nm) { std::strncpy(out, nm, cap - 1); out[cap - 1] = '\0'; }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}
const char* ItemName(uint32_t id) {
  auto it = g_name_cache.find(id);
  if (it != g_name_cache.end()) return it->second.c_str();
  char buf[64];
  ResolveNameSEH(id, buf, sizeof(buf));
  if (buf[0] == '\0') std::snprintf(buf, sizeof(buf), "#%u", id);
  return (g_name_cache[id] = buf).c_str();
}

//  Icones ImGui (cache id -> texture)
struct IconTex { void* tex = nullptr; int w = 0, h = 0; };
std::unordered_map<uint32_t, IconTex> g_icon_cache;
struct RawTex { const uint8_t* bgra; int w; int h; };
bool GetRawTex(const char* path, RawTex* out) {
  __try {
    void* mgr = reinterpret_cast<TexMgr_t>(kTexMgr)();
    if (!mgr) return false;
    void* key = reinterpret_cast<MakeKey_t>(kMakeKey)(path);
    if (!key) return false;
    void* t = reinterpret_cast<LoadTex_t>(kLoadTex)(mgr, nullptr, key);
    if (!t) return false;
    const int w = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexW);
    const int h = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexH);
    const uint8_t* bgra =
        *reinterpret_cast<const uint8_t**>(static_cast<char*>(t) + kTexPix);
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || !bgra) return false;
    out->bgra = bgra; out->w = w; out->h = h;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool BuildIconPathSafe(uint32_t id, char* out) {
  char idstr[16];
  std::snprintf(idstr, sizeof(idstr), "%u", id);
  out[0] = '\0';
  __try {
    reinterpret_cast<BuildIconPath_t>(kBuildIconPath)(idstr, out, 1);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
IconTex LoadItemIcon(uint32_t id) {
  char path[192];
  if (!BuildIconPathSafe(id, path)) return {};
  RawTex rt{};
  if (!GetRawTex(path, &rt)) return {};
  std::vector<uint8_t> argb(static_cast<size_t>(rt.w) * rt.h * 4);
  for (int i = 0; i < rt.w * rt.h; ++i) {
    const uint8_t b = rt.bgra[i * 4], g = rt.bgra[i * 4 + 1], r = rt.bgra[i * 4 + 2];
    const bool ck = (r == 0xFF && g == 0 && b == 0xFF);  // magenta -> transparent
    argb[i * 4] = b; argb[i * 4 + 1] = g; argb[i * 4 + 2] = r;
    argb[i * 4 + 3] = ck ? 0 : 0xFF;
  }
  return {Overlay_CreateTextureARGB(argb.data(), rt.w, rt.h), rt.w, rt.h};
}
IconTex ResolveIcon(uint32_t id) {
  auto it = g_icon_cache.find(id);
  if (it != g_icon_cache.end()) return it->second;
  return g_icon_cache[id] = LoadItemIcon(id);
}

// Ouvre la fenetre de description native (id 0xc) pour l'item `id` a (mx,my).
void OpenItemDesc(uint32_t id, uint16_t view, uint32_t location, int mx, int my) {
  if (id == 0) return;
  __try {
    uint8_t info[0x100];
    std::memset(info, 0, sizeof(info));
    reinterpret_cast<InfoCtor_t>(kInfoCtor)(info);
    reinterpret_cast<InfoSetId_t>(kInfoSetId)(info, static_cast<int>(id));
    info[0x5c] = 1;                                                  // identifie
    *reinterpret_cast<uint32_t*>(info + 0x8)  = location;            // equip point
    *reinterpret_cast<uint32_t*>(info + 0x70) = view;                // viewID
    void* cache = *reinterpret_cast<void**>(kEnsureCache);
    if (cache)
      reinterpret_cast<EnsureLoaded_t>(kEnsureLoaded)(cache, static_cast<int>(id));
    void* dwnd = reinterpret_cast<MakeWindow_t>(kMakeWindow)(
        reinterpret_cast<void*>(kUIWindowMgr), nullptr,
        reinterpret_cast<void*>(kWinItemDesc));
    if (dwnd) {
      Vf<OnMsg_t>(dwnd, kVfOnMsg)(dwnd, nullptr, 0, kMsgSetItem,
                                  static_cast<int>(reinterpret_cast<uintptr_t>(info)),
                                  0, 0, 0);
      Vf<SetPos_t>(dwnd, kVfSetPos)(dwnd, nullptr, mx, my);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

//  Envois (raw, via Bourgeon::SendPacket)
void SendUnequip(int invIndex) {
  if (invIndex <= 0) return;
  uint8_t pkt[4];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpUnequip;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(invIndex);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}
void SendStatUp(int statType) {
  uint8_t pkt[5];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpStatUp;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(statType);
  pkt[4] = 1;  // +1
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// Abreviation d'un slot vide (pour l'afficher grise dans la case).
const char* SlotAbbrev(int slot) {
  switch (slot) {
    case 0: return "Tete\nhaut";
    case 8: return "Tete\nmil.";
    case 9: return "Tete\nbas";
    case 4: return "Armure";
    case 2: return "Cape";
    case 1: return "Arme";
    case 5: return "Bouclier";
    case 6: return "Chauss.";
    case 3: return "Acc. 1";
    case 7: return "Acc. 2";
    default: return "";
  }
}

const ImVec4 kBlack(0.0f, 0.0f, 0.0f, 1.0f);  // texte noir (skin RO clair)

}  // namespace

CharacterSheet::CharacterSheet() {}

void CharacterSheet::DrawSlot(int slot, bool costume, float x, float y, float sz) {
  EquipItem it{};
  const bool has = ReadEquipSlot(slot, costume, &it);

  ImGui::SetCursorPos(ImVec2(x, y));
  ImGui::PushID(slot * 2 + (costume ? 1 : 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(206, 206, 206, 255));  // case grise RO
  ImGui::BeginChild("slot", ImVec2(sz, sz), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  const ImVec2 wp = ImGui::GetWindowPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (has) {
    IconTex ic = ResolveIcon(it.nameid);
    if (ic.tex) {
      const float pad = 3.0f;
      ImGui::SetCursorPos(ImVec2(pad, pad));
      ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex),
                   ImVec2(sz - 2 * pad, sz - 2 * pad));
    }
    if (it.refine > 0) {  // overlay "+N" en haut a gauche
      char rf[8];
      std::snprintf(rf, sizeof(rf), "+%d", it.refine);
      dl->AddText(ImVec2(wp.x + 2, wp.y + 1), IM_COL32(255, 220, 60, 255), rf);
    }
  } else {  // slot vide : abreviation grisee
    const char* ab = SlotAbbrev(slot);
    const ImVec2 ts = ImGui::CalcTextSize(ab);
    dl->AddText(ImVec2(wp.x + (sz - ts.x) * 0.5f, wp.y + (sz - ts.y) * 0.5f),
                IM_COL32(120, 120, 120, 255), ab);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  // Interactions sur la case : survol = tooltip, clic-gauche = desc, clic-droit =
  // desequiper.
  if (has && ImGui::IsItemHovered()) {
    ro::SetHoverCursor(2);  // main
    if (it.refine > 0)
      ImGui::SetTooltip("+%d %s\n(clic gauche : description, clic droit : desequiper)",
                        it.refine, ItemName(it.nameid));
    else
      ImGui::SetTooltip("%s\n(clic gauche : description, clic droit : desequiper)",
                        ItemName(it.nameid));
    const ImVec2 mp = ImGui::GetMousePos();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      OpenItemDesc(it.nameid, static_cast<uint16_t>(it.viewId),
                   static_cast<uint32_t>(it.location), static_cast<int>(mp.x),
                   static_cast<int>(mp.y));
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
      SendUnequip(it.invIndex);
  }
  ImGui::PopID();
}

void CharacterSheet::DrawDoll(float avail_w) {
  // En-tete (colonne gauche) : pseudo, classe, niveau.
  const std::string name = Bourgeon::Instance().client().session().GetCharName();
  ImGui::TextColored(kBlack, "%s", name.empty() ? "(perso)" : name.c_str());
  ImGui::TextColored(kBlack, "%s   Nv %d / %d", ClassNameSEH(),
                     ReadInt(kBaseLvl), ReadInt(kJobLvl));
  ImGui::Separator();

  // Colonnes de slots facon poupee : gauche (tete/armure/cape), droite
  // (arme/bouclier/chaussures/accessoires), avatar au centre.
  const int leftSlots[5]  = {0, 8, 9, 4, 2};   // tete haut/mil/bas, armure, cape
  const int rightSlots[5] = {1, 5, 6, 3, 7};   // arme, bouclier, chauss., acc1, acc2
  const float sz = 44.0f, gap = 6.0f;
  const float y0 = ImGui::GetCursorPosY() + 2.0f;
  const float lx = 4.0f;
  const float rx = avail_w - sz - 4.0f;
  for (int i = 0; i < 5; ++i) {
    DrawSlot(leftSlots[i], costume_, lx, y0 + i * (sz + gap), sz);
    DrawSlot(rightSlots[i], costume_, rx, y0 + i * (sz + gap), sz);
  }
  // Avatar central (placeholder Phase 1 ; RenderPlayerAvatar en Phase 2).
  const float cx = lx + sz + gap;
  const float cw = std::max(60.0f, rx - cx - gap);
  const float ch = 5 * (sz + gap) - gap;
  ImGui::SetCursorPos(ImVec2(cx, y0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(228, 230, 236, 255));
  ImGui::BeginChild("cs_avatar", ImVec2(cw, ch), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  const char* ph = "Avatar\n(Phase 2)";
  const ImVec2 ts = ImGui::CalcTextSize(ph);
  ImGui::SetCursorPos(ImVec2((cw - ts.x) * 0.5f, (ch - ts.y) * 0.5f));
  ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.45f, 1.0f), "%s", ph);
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void CharacterSheet::DrawStatsPanel() {
  Stats s{};
  if (!ReadStats(&s)) return;

  // Stats primaires + boutons de montee. (Pseudo/classe/niveau : colonne gauche.)
  const float step = ImGui::GetFrameHeight();
  const float right = ImGui::GetContentRegionMax().x;  // bord droit local (align +)
  for (int i = 0; i < 6; ++i) {
    ImGui::AlignTextToFramePadding();
    if (s.bonus[i] != 0)
      ImGui::TextColored(kBlack, "%s  %d (+%d)", kStatName[i], s.base[i], s.bonus[i]);
    else
      ImGui::TextColored(kBlack, "%s  %d", kStatName[i], s.base[i]);
    // Bouton "+" a droite, actif seulement si 0 < cout <= points.
    const bool can = (s.raise[i] > 0 && s.raise[i] <= s.points);
    ImGui::SameLine();
    if (right > step) ImGui::SetCursorPosX(right - step);
    ImGui::PushID(100 + i);
    if (!can) ImGui::BeginDisabled();
    if (ro::RoButton("+", step, step)) SendStatUp(kStatType[i]);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Monter %s (cout : %d point%s)", kStatName[i], s.raise[i],
                        s.raise[i] > 1 ? "s" : "");
    if (!can) ImGui::EndDisabled();
    ImGui::PopID();
  }
  ImGui::TextColored(kBlack, "Points de statut : %d", s.points);
  ImGui::Separator();

  // Stats derivees.
  ImGui::TextColored(kBlack, "ATK   %d + %d", s.atk1, s.atk2);
  ImGui::TextColored(kBlack, "MATK  %d ~ %d", s.matk_min, s.matk_max);
  ImGui::TextColored(kBlack, "DEF   %d + %d", s.def_s, s.def_h);
  ImGui::TextColored(kBlack, "MDEF  %d + %d", s.mdef_s, s.mdef_h);
  ImGui::TextColored(kBlack, "HIT   %d", s.hit);
  ImGui::TextColored(kBlack, "FLEE  %d", s.flee);
  ImGui::TextColored(kBlack, "CRI   %d", s.crit);
  ImGui::TextColored(kBlack, "ASPD  %d", (2000 - s.aspd_raw) / 10);
  ImGui::TextColored(kBlack, "Esq.P %d", s.pdodge);
  ImGui::Separator();
  ImGui::TextColored(kBlack, "HP  %d / %d", s.hp, s.hp_max);
  ImGui::TextColored(kBlack, "SP  %d / %d", s.sp, s.sp_max);
}

void CharacterSheet::OnRenderUI() {
  if (!imgui_enabled_) return;

  // Hotkey Alt+F : bascule la fenetre (ImGui recoit l'input clavier du client, donc
  // ne fire que quand le jeu a le focus). VERIFIER live que Alt+F est libre.
  if (ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F, false))
    show_ = !show_;
  if (!show_) return;

  // En jeu seulement (perso charge) : evite d'afficher des stats a zero au login.
  if (ReadInt(kBaseLvl) <= 0) return;

  if (need_pos_) {
    ImGui::SetNextWindowPos(ImVec2(240, 140), ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(600, 380), ImGuiCond_FirstUseEver);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);

  const bool begun =
      ro::BeginRoWindow("Personnage###bourgeon_charsheet", &show_,
                        ImGuiWindowFlags_NoCollapse);
  bourgeon::CloseWindowOnEscape(show_);
  if (!begun) { ro::EndRoWindow(); ImGui::PopStyleVar(5); return; }

  // Onglet Equipement / Costume.
  if (ImGui::BeginTabBar("cs_tabs")) {
    if (ImGui::BeginTabItem("Equipement")) { costume_ = false; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Costume"))    { costume_ = true;  ImGui::EndTabItem(); }
    ImGui::EndTabBar();
  }

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float stats_w = 240.0f;
  const float doll_w = std::max(220.0f, avail.x - stats_w - 8.0f);

  ImGui::BeginChild("cs_doll", ImVec2(doll_w, 0), true);
  DrawDoll(ImGui::GetContentRegionAvail().x);
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("cs_stats", ImVec2(stats_w, 0), true);
  DrawStatsPanel();
  ImGui::EndChild();

  ro::EndRoWindow();
  ImGui::PopStyleVar(5);
}
