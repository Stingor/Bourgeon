#include "plugins/character_sheet.h"

#include <Windows.h>
#include <commdlg.h>  // GetSaveFileNameA (dialogue « Enregistrer sous »)
#include <objbase.h>  // CoInitializeEx pour le thread du dialogue

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "bourgeon.h"        // Bourgeon::Instance().SendPacket / session
#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB
#include "imgui.h"
#include "plugins/basic_info.h"    // RenderPlayerAvatar (avatar plein-corps)
#include "plugins/inventory_viewer.h"  // EquipByPayloadIndex (drag-drop équip)
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
// Explications (tooltip au survol) de chaque stat primaire, même ordre que kStatName.
const char* kStatDesc[6] = {
    "STR — Force : augmente l'ATK physique et le poids max.",
    "AGI — Agilité : augmente la vitesse d'attaque (ASPD) et l'esquive (FLEE).",
    "VIT — Vitalité : augmente les HP max, la DEF et la résistance aux status.",
    "INT — Intelligence : augmente le MATK, les SP max et la MDEF.",
    "DEX — Dextérité : augmente la précision (HIT), l'ATK à distance, réduit le temps de cast.",
    "LUK — Chance : augmente le critique, la perfect dodge, réduit les status.",
};
// Poses proposées dans le combo (sous-ensemble d'animType : on retire mort/gelé/
// touché/ramasser/attaque, peu utiles/moches en avatar). {animType, libellé}.
struct PoseOpt { int anim; const char* label; };
const PoseOpt kPoses[] = {{0, "Repos"}, {1, "Marche"}, {2, "Assis"}, {4, "Combat"}};
const int kPoseCount = 4;
const char* PoseLabel(int anim) {
  for (int i = 0; i < kPoseCount; ++i)
    if (kPoses[i].anim == anim) return kPoses[i].label;
  return "Combat";  // repli
}
constexpr int kAnimCombat = 4;  // en combat, on limite à 4 directions cardinales

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
  // Textures D3DPOOL_DEFAULT : mortes après reset/recréation du device -> vider.
  static unsigned s_epoch = 0;
  const unsigned e = Overlay_DeviceEpoch();
  if (e != s_epoch) { g_icon_cache.clear(); s_epoch = e; }
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
    case 0: return "Head\nbot";
    case 8: return "Head\ntop";
    case 9: return "Head\nmid";
    case 4: return "Armor";
    case 2: return "Cape";
    case 1: return "Weapon";
    case 5: return "Shield";
    case 6: return "Shoes";
    case 3: return "Acc. L";
    case 7: return "Acc. R";
    default: return "";
  }
}

const ImVec4 kBlack(0.0f, 0.0f, 0.0f, 1.0f);  // texte noir (skin RO clair)

//  Deux tailles de fenetre : doll seul (narrow) ou doll+stats (wide). Le drag snap
//  sur la plus proche ; le volet stats est cache si la largeur ne suffit pas (evite
//  la scrollbar "dans le vide").
constexpr float kDollW  = 280.0f;   // largeur zone doll (contenu)
constexpr float kStatsW = 240.0f;   // largeur zone stats (contenu)
struct WinSnap { float narrow = 0.0f, wide = 0.0f; bool valid = false; };
WinSnap g_win_snap;
void SnapCharSheetWidth(ImGuiSizeCallbackData* d) {
  if (!g_win_snap.valid) return;
  const float mid = (g_win_snap.narrow + g_win_snap.wide) * 0.5f;
  d->DesiredSize.x = (d->DesiredSize.x < mid) ? g_win_snap.narrow : g_win_snap.wide;
}

}  // namespace

CharacterSheet::CharacterSheet() {}

void CharacterSheet::DrawSlot(int slot, bool costume, float x, float y, float sz) {
  EquipItem it{};
  const bool has = ReadEquipSlot(slot, costume, &it);

  ImGui::SetCursorPos(ImVec2(x, y));
  ImGui::PushID(slot * 2 + (costume ? 1 : 0));
  // La case = un InvisibleButton (widget À ID) : INDISPENSABLE pour que le drag-drop
  // (source/cible) et les clics fonctionnent — un BeginChild bordé passif ne capte pas
  // l'ActiveId, donc BeginDragDropSource n'y démarre JAMAIS (même pattern que la grille
  // d'inventaire). Fond gris RO + bordure dessinés à la main via le draw list.
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::SetNextItemAllowOverlap();  // slots positionnés en absolu : garde le hit-test/hover
  ImGui::InvisibleButton("slot", ImVec2(sz, sz));
  const ImVec2 p1(p0.x + sz, p0.y + sz);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, IM_COL32(206, 206, 206, 255), 4.0f);  // case grise RO
  dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 80), 4.0f);               // bordure
  if (has) {
    IconTex ic = ResolveIcon(it.nameid);
    if (ic.tex) {
      const float pad = 3.0f;
      dl->AddImage(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(p0.x + pad, p0.y + pad),
                   ImVec2(p1.x - pad, p1.y - pad));
    }
    if (it.refine > 0) {  // overlay "+N" en BAS À DROITE (comme l'inventaire) : noir + liseré blanc
      char rf[8];
      std::snprintf(rf, sizeof(rf), "+%d", it.refine);
      const ImVec2 ts = ImGui::CalcTextSize(rf);
      const ImVec2 rp(p1.x - ts.x - 2, p1.y - ts.y - 1);
      const ImU32 white = IM_COL32(255, 255, 255, 255);
      for (int oy = -1; oy <= 1; ++oy)
        for (int ox = -1; ox <= 1; ++ox)
          if (ox || oy) dl->AddText(ImVec2(rp.x + ox, rp.y + oy), white, rf);
      dl->AddText(rp, IM_COL32(0, 0, 0, 255), rf);
    }
  } else {  // slot vide : abreviation grisee
    const char* ab = SlotAbbrev(slot);
    const ImVec2 ts = ImGui::CalcTextSize(ab);
    dl->AddText(ImVec2(p0.x + (sz - ts.x) * 0.5f, p0.y + (sz - ts.y) * 0.5f),
                IM_COL32(120, 120, 120, 255), ab);
  }

  // Drag-drop. SOURCE (slot occupé) : glisser l'item vers l'inventaire = le déséquiper.
  // Payload "BGN_EQUIP" = index inventaire ; l'inventaire l'accepte -> SendUnequip.
  if (has && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
    int inv = it.invIndex;
    ImGui::SetDragDropPayload("BGN_EQUIP", &inv, sizeof(inv));
    IconTex ic = ResolveIcon(it.nameid);  // aperçu du drag (icône + nom)
    if (ic.tex) {
      ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(24, 24));
      ImGui::SameLine();
    }
    ImGui::TextUnformatted(ItemName(it.nameid));
    ImGui::EndDragDropSource();
  }
  // CIBLE (tout slot) : lâcher un item d'inventaire (payload "INV_ITEM") sur le doll =
  // l'équiper. Le serveur place/swappe automatiquement (pc_equipitem). Ctrl = main gauche.
  if (ImGui::BeginDragDropTarget()) {
    if (ImGui::AcceptDragDropPayload("INV_ITEM")) {  // item d'inventaire lâché sur le doll
      if (auto* iv = Bourgeon::Instance().inventory_viewer())
        iv->EquipDraggedItem(ImGui::GetIO().KeyCtrl);  // Ctrl = main gauche
    }
    ImGui::EndDragDropTarget();
  }

  // Interactions sur la case : survol = tooltip ; clic DROIT = description ; MAJ+clic
  // DROIT = lien de l'item dans le chat (comme l'inventaire) ; double-clic GAUCHE =
  // déséquiper ; glisser = vers l'inventaire (drag-drop ci-dessus). Le clic gauche
  // simple ne fait rien (réservé au démarrage du glisser).
  if (has && ImGui::IsItemHovered()) {
    ro::SetHoverCursor(2);  // main
    const char* hint =
        "(clic droit : desc, Maj+clic droit : lien chat, double-clic : déséquip, glisser : inv.)";
    if (it.refine > 0)
      ImGui::SetTooltip("+%d %s\n%s", it.refine, ItemName(it.nameid), hint);
    else
      ImGui::SetTooltip("%s\n%s", ItemName(it.nameid), hint);
    const ImVec2 mp = ImGui::GetMousePos();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      if (ImGui::GetIO().KeyShift) {  // Maj+clic droit = lien de l'item dans le chat
        if (auto* iv = Bourgeon::Instance().inventory_viewer())
          iv->LinkItemToChat(it.invIndex);
      } else {  // clic droit seul = description
        OpenItemDesc(it.nameid, static_cast<uint16_t>(it.viewId),
                     static_cast<uint32_t>(it.location), static_cast<int>(mp.x),
                     static_cast<int>(mp.y));
      }
    }
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
      SendUnequip(it.invIndex);
  }
  ImGui::PopID();
}

void CharacterSheet::DrawDoll(float avail_w) {
  // En-tete (colonne gauche, CENTRE horizontalement) : pseudo, classe, niveau.
  const float start_x = ImGui::GetCursorPosX();
  auto centered = [&](const char* txt) {
    const float tw = ImGui::CalcTextSize(txt).x;
    ImGui::SetCursorPosX(start_x + std::max(0.0f, (avail_w - tw) * 0.5f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 5.0f);
    ImGui::TextColored(kBlack, "%s", txt);
  };
  const std::string name = Bourgeon::Instance().client().session().GetCharName();
  char lvl[96];
  std::snprintf(lvl, sizeof(lvl), "%s   Nv %d / %d", ClassNameSEH(),
                ReadInt(kBaseLvl), ReadInt(kJobLvl));
  centered(name.empty() ? "(perso)" : name.c_str());
  centered(lvl);
  Stats s{};
  if (ReadStats(&s))
  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "HP: %d/%d SP: %d/%d", s.hp, s.hp_max, s.sp, s.sp_max);
    centered(buf);
  }
  ImGui::Separator();

  // Bloc poupée : 2 colonnes de slots + avatar central, largeur fixe CENTRÉE
  // horizontalement (MÊME méthode que l'en-tête : start_x + marge -> sinon le bloc
  // est collé à gauche). La disposition dépend de l'onglet (branches ci-dessous).
  const float sz = 44.0f, gap = 6.0f, avatar_w = 130.0f;
  const float block_w = sz + gap + avatar_w + gap + sz;
  const float ox = start_x + std::max(0.0f, (avail_w - block_w) * 0.5f);  // centre
  const float y0 = ImGui::GetCursorPosY() + 2.0f;
  const float lx = ox;                              // colonne gauche
  const float ax = ox + sz + gap;                   // avatar
  const float rx = ox + sz + gap + avatar_w + gap;  // colonne droite
  // Hauteur de l'avatar CONSTANTE (4 rangées) sur les deux onglets : le sprite ne
  // change pas de taille en basculant Équipement <-> Costume (pas de re-figeage).
  const float cw = avatar_w;
  const float ch = 4 * (sz + gap) - gap;
  float content_bottom;  // y du bas du contenu doll (base du sélecteur de pose)
  if (costume_) {
    // Onglet COSTUME : seuls les slots costume RÉELS existent (3 têtes + cape) ->
    // aucun cadre vide. Tête mil en haut-droite (comme l'équip) ; les 2 rangées sont
    // centrées verticalement contre l'avatar.
    const int cL[2] = {8, 0};   // tête haut (8), tête bas (0)
    const int cR[2] = {9, 2};   // tête mil (9, haut-droite), cape
    const float block_h = 2 * (sz + gap) - gap;
    const float off = (ch - block_h) * 0.5f;  // centrage vertical vs l'avatar
    for (int i = 0; i < 2; ++i) {
      DrawSlot(cL[i], costume_, lx, y0 + off + i * (sz + gap), sz);
      DrawSlot(cR[i], costume_, rx, y0 + off + i * (sz + gap), sz);
    }
    content_bottom = y0 + ch;
  } else {
    // Onglet ÉQUIPEMENT : 4 rangées de slots + arme/bouclier SOUS le doll.
    const int leftSlots[4]  = {8, 0, 2, 7};   // tête haut (8), tête bas (0), cape, Acc L (acc2)
    const int rightSlots[4] = {9, 4, 6, 3};   // tête mil (9, haut-droite), armure, chauss., Acc R (acc1)
    for (int i = 0; i < 4; ++i) {
      DrawSlot(leftSlots[i], costume_, lx, y0 + i * (sz + gap), sz);
      DrawSlot(rightSlots[i], costume_, rx, y0 + i * (sz + gap), sz);
    }
    // Arme (main DROITE du perso) à GAUCHE, bouclier / arme main gauche à DROITE
    // — vue « miroir » d'un perso qui te fait face, sous les coins bas de l'avatar.
    const float wpn_y = y0 + 4 * (sz + gap);
    DrawSlot(1, costume_, ax, wpn_y, sz);                    // arme -> bas gauche
    DrawSlot(5, costume_, ax + avatar_w - sz, wpn_y, sz);    // bouclier -> bas droite
    content_bottom = wpn_y + sz;
  }
  ImGui::SetCursorPos(ImVec2(ax, y0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(228, 230, 236, 255));
  ImGui::BeginChild("cs_avatar", ImVec2(cw, ch), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  if (auto* bi = Bourgeon::Instance().basic_info()) {
    const ImVec2 rp = ImGui::GetWindowPos();
    const ImVec2 rs = ImGui::GetWindowSize();
    bi->RenderPlayerAvatar(rp.x + 2.0f, rp.y + 2.0f, rs.x - 4.0f, rs.y - 4.0f,
                           avatar_anim_, avatar_dir_, avatar_animate_);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  // Molette sur l'avatar = tourner (comme le preview cashshop : dir 0..7 + wrap,
  // et on CONSOMME la molette pour ne pas scroller la fenetre). Defaut = face.
  if (ImGui::IsItemHovered()) {
    ro::SetHoverCursor(2);
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) {
      if (avatar_anim_ == kAnimCombat)  // combat : 4 dirs cardinales (0=face,2,4=dos,6)
        avatar_dir_ = ((avatar_dir_ & ~1) + (wheel > 0.0f ? 2 : 6)) & 7;
      else
        avatar_dir_ = (avatar_dir_ + (wheel > 0.0f ? 1 : 7)) & 7;  // 8 dirs
      ImGui::GetIO().MouseWheel = 0.0f;  // consommer -> pas de scroll de fenetre
    }
  }

  // Selecteur de pose sous le contenu du doll (la direction = MOLETTE).
  const float sel_y = content_bottom + 8.0f;
  const float fh = ImGui::GetFrameHeightWithSpacing();
  ImGui::SetCursorPos(ImVec2(ox, sel_y));
  ImGui::SetNextItemWidth(block_w);
  if (ro::RoBeginCombo("##cs_pose", PoseLabel(avatar_anim_))) {
    for (int i = 0; i < kPoseCount; ++i)
      if (ImGui::Selectable(kPoses[i].label, avatar_anim_ == kPoses[i].anim)) {
        avatar_anim_ = kPoses[i].anim;
        if (avatar_anim_ == kAnimCombat) avatar_dir_ &= ~1;  // snap dir cardinale
      }
    ro::RoEndCombo();
  }
  // « Animer » n'est utile que pour Marche(1)/Combat(4) : Repos/Assis sont figés
  // (image 0) pour éviter la tête + coiffes qui « regardent autour » en défilant.
  if (avatar_anim_ == 1 || avatar_anim_ == kAnimCombat) {
    ImGui::SetCursorPos(ImVec2(ox, sel_y + fh));
    ro::RoCheckbox("Animer", &avatar_animate_);
  }
  // Génère un GIF animé (fond transparent) de la pose+direction courante. Le clic
  // ouvre un dialogue « Enregistrer sous » sur un thread séparé (non bloquant) ;
  // l'export se fait ici, sur le thread principal, quand le chemin est prêt.
  ImGui::SetCursorPos(ImVec2(ox, sel_y + 2.0f * fh));
  const bool gif_busy = gif_dialog_busy_.load();
  if (gif_busy) ImGui::BeginDisabled();
  if (ro::RoButton("Générer le GIF", block_w)) RequestGifSave();
  if (gif_busy) ImGui::EndDisabled();

  // Résultat du dialogue (déposé par le thread séparé) → export ici, thread
  // principal, car ExportAvatarGif a besoin du device D3D9.
  if (gif_dialog_ready_.exchange(false)) {
    const std::string p = gif_dialog_path_;
    if (!p.empty()) {
      auto* bi = Bourgeon::Instance().basic_info();
      const bool ok =
          bi && bi->ExportAvatarGif(gif_export_anim_, gif_export_dir_, p.c_str());
      const char* fn = std::strrchr(p.c_str(), '\\');
      gif_status_ = ok ? (std::string("GIF OK : ") + (fn ? fn + 1 : p.c_str()))
                       : std::string("Échec GIF (voir log)");
    } else {
      gif_status_ = "Export annulé";
    }
    gif_dialog_busy_.store(false);
  }
  if (!gif_status_.empty()) {
    ImGui::SetCursorPos(ImVec2(ox, sel_y + 3.0f * fh));
    ImGui::PushTextWrapPos(ox + block_w);  // wrap dans la largeur du bloc
    ImGui::TextColored(kBlack, "%s", gif_status_.c_str());
    ImGui::PopTextWrapPos();
  }
}

// Ouvre le dialogue Windows « Enregistrer sous » du GIF sur un THREAD séparé : un
// dialogue modal ne doit PAS bloquer le thread de rendu/réseau du jeu (sinon
// timeout → déconnexion). Le chemin choisi est déposé dans gif_dialog_path_ +
// gif_dialog_ready_ ; le thread principal (DrawDoll) fait l'export une fois prêt.
void CharacterSheet::RequestGifSave() {
  if (gif_dialog_busy_.exchange(true)) return;  // un dialogue est déjà ouvert
  gif_dialog_ready_.store(false);
  gif_export_anim_ = avatar_anim_;  // fige la pose/direction au moment du clic
  gif_export_dir_  = avatar_dir_;

  // Nom par défaut : avatar_<pseudo>_<pose>_d<N>.gif (pseudo assaini).
  std::string name = Bourgeon::Instance().client().session().GetCharName();
  for (char& c : name)
    if (c && std::strchr("\\/:*?\"<>| ", c)) c = '_';
  if (name.empty()) name = "perso";
  char defname[MAX_PATH];
  std::snprintf(defname, sizeof(defname), "avatar_%s_%s_d%d.gif", name.c_str(),
                PoseLabel(gif_export_anim_), gif_export_dir_);

  // Dossier initial proposé : <jeu>\screenshot (créé s'il manque).
  char exe[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  char* slash = std::strrchr(exe, '\\');
  if (slash) *slash = '\0';
  std::string initdir = std::string(exe) + "\\screenshot";
  CreateDirectoryA(initdir.c_str(), nullptr);

  std::thread([this, def = std::string(defname), initdir]() {
    // Apartment COM pour le dialogue moderne (places bar / shell) ; isolé au thread.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    char path[MAX_PATH];
    strncpy_s(path, sizeof(path), def.c_str(), _TRUNCATE);
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = nullptr;  // top-level : pas de propriétaire cross-thread
    ofn.lpstrFilter = "GIF anime (*.gif)\0*.gif\0Tous les fichiers\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = sizeof(path);
    ofn.lpstrInitialDir = initdir.c_str();
    ofn.lpstrDefExt = "gif";
    ofn.lpstrTitle  = "Enregistrer le GIF de l'avatar";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    const bool ok = GetSaveFileNameA(&ofn) != 0;
    gif_dialog_path_ = ok ? std::string(path) : std::string();
    gif_dialog_ready_.store(true);  // release : signale le thread principal
    if (SUCCEEDED(hr)) CoUninitialize();
  }).detach();
}

void CharacterSheet::DrawStatsPanel() {
  Stats s{};
  if (!ReadStats(&s)) return;

  // Stats primaires + boutons de montee. (Pseudo/classe/niveau : colonne gauche.)
  const float step = ImGui::GetFrameHeight();
  const float right = ImGui::GetContentRegionMax().x;  // bord droit local (align +)
  const float start = ImGui::GetCursorPosX();          // colonne LABEL (gauche)
  // Colonne VALEURS alignée : après le plus large label ("Esq.P") + marge. Toutes les
  // valeurs (primaires + dérivées) démarrent à ce x -> chiffres en colonne.
  const float val_x = start + ImGui::CalcTextSize("Esq.P").x + 12.0f;
  for (int i = 0; i < 6; ++i) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kBlack, "%s", kStatName[i]);             // label
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kStatDesc[i]);  // rôle de la stat
    ImGui::SameLine();
    ImGui::SetCursorPosX(val_x);                                // colonne valeurs
    if (s.bonus[i] != 0)
      ImGui::TextColored(kBlack, "%d (+%d)", s.base[i], s.bonus[i]);
    else
      ImGui::TextColored(kBlack, "%d", s.base[i]);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kStatDesc[i]);
    // Bouton "+" + COÛT du prochain point à sa droite (comme le natif). Actif SSI
    // 0 < coût <= points restants.
    const bool can = (s.raise[i] > 0 && s.raise[i] <= s.points);
    const float cost_w = 30.0f;  // largeur réservée au coût, à droite du +
    ImGui::SameLine();
    if (right > step + cost_w) ImGui::SetCursorPosX(right - step - cost_w);
    ImGui::PushID(100 + i);
    if (!can) ImGui::BeginDisabled();
    if (ro::RoButton("+", step, step)) SendStatUp(kStatType[i]);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Monter %s (coût : %d point%s)", kStatName[i], s.raise[i],
                        s.raise[i] > 1 ? "s" : "");
    if (!can) ImGui::EndDisabled();
    ImGui::PopID();
    if (s.raise[i] > 0) {  // coût du prochain point, juste à droite du +
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(can ? kBlack : ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%d", s.raise[i]);
    }
  }
  ImGui::TextColored(kBlack, "Points de statut : %d", s.points);
  ImGui::Separator();

  // Stats derivees : label (gauche) + valeur (colonne val_x alignée). Survol = expl. Le
  // % : DEF/MDEF « 1 » (soft, VIT/INT = réduction en %) + « 2 » (plate) ; CRI/Esq.P = %.
  auto stat = [&](const char* label, const char* value, const char* tip) {
    ImGui::TextColored(kBlack, "%s", label);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine();
    ImGui::SetCursorPosX(val_x);
    ImGui::TextColored(kBlack, "%s", value);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  };
  char b[64];
  std::snprintf(b, sizeof(b), "%d + %d", s.atk1, s.atk2);
  stat("ATK", b, "Attaque physique (arme + statut) : détermine les dégâts des coups physiques.");
  std::snprintf(b, sizeof(b), "%d ~ %d", s.matk_min, s.matk_max);
  stat("MATK", b, "Attaque magique : détermine les dégâts des sorts.");
  std::snprintf(b, sizeof(b), "%d%% + %d", s.def_s, s.def_h);
  stat("DEF", b, "Défense physique : réduction en % (VIT/équip, def1) + réduction plate (def2).");
  std::snprintf(b, sizeof(b), "%d%% + %d", s.mdef_s, s.mdef_h);
  stat("MDEF", b, "Défense magique : réduction en % (INT, mdef1) + réduction plate (mdef2).");
  std::snprintf(b, sizeof(b), "%d", s.hit);
  stat("HIT", b, "Précision : comparée au FLEE de la cible pour déterminer si vous touchez.");
  std::snprintf(b, sizeof(b), "%d", s.flee);
  stat("FLEE", b, "Esquive : comparée au HIT de la cible, réduit la probabilité d'être touché.");
  std::snprintf(b, sizeof(b), "%d%%", s.crit / 10);  // stocké x10 (précision .1)
  stat("CRI", b, "Taux de coup critique (%) : un critique ignore la DEF et ne rate jamais.");
  std::snprintf(b, sizeof(b), "%d", (2000 - s.aspd_raw) / 10);
  stat("ASPD", b, "Vitesse d'attaque : plus elle est haute, plus vous frappez souvent.");
  std::snprintf(b, sizeof(b), "%d%%", s.pdodge / 10);  // stocké x10 (précision .1)
  stat("Esq.P", b, "Esquive parfaite (%, via LUK) : évite totalement une attaque, même critique.");
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
  // Deux tailles possibles : doll seul (narrow) ou doll+stats (wide) ; snap au drag.
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  g_win_snap.narrow = kDollW + chrome_w_;
  g_win_snap.wide   = kDollW + gap + kStatsW + chrome_w_;
  g_win_snap.valid  = true;
  ImGui::SetNextWindowSizeConstraints(ImVec2(g_win_snap.narrow, 450.0f),
                                      ImVec2(g_win_snap.wide, 10000.0f),
                                      SnapCharSheetWidth);
  ImGui::SetNextWindowSize(ImVec2(g_win_snap.wide, 490.0f), ImGuiCond_FirstUseEver);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);

  // Pas de NoCollapse -> le skin RO affiche le bouton minimiser (repli barre de titre),
  // comme l'inventaire/le natif ; le repli est géré par le `if (!begun)` ci-dessous.
  const bool begun =
      ro::BeginRoWindow("Personnage###bourgeon_charsheet", &show_,
                        ImGuiWindowFlags_None);
  bourgeon::CloseWindowOnEscape(show_);
  if (!begun) { ro::EndRoWindow(); ImGui::PopStyleVar(5); return; }

  // Onglet Equipement / Costume.
  if (ImGui::BeginTabBar("cs_tabs")) {
    if (ImGui::BeginTabItem("Equipement")) { costume_ = false; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Costume"))    { costume_ = true;  ImGui::EndTabItem(); }
    ImGui::EndTabBar();
  }

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  chrome_w_ = ImGui::GetWindowWidth() - avail.x;  // mesure pour la contrainte suivante
  // Volet stats seulement si la largeur suffit (sinon cache -> pas de scrollbar vide).
  const bool show_stats =
      avail.x >= kDollW + ImGui::GetStyle().ItemSpacing.x + kStatsW - 6.0f;
  const float doll_w = show_stats ? kDollW : avail.x;

  ImGui::BeginChild("cs_doll", ImVec2(doll_w, 0), true);
  DrawDoll(ImGui::GetContentRegionAvail().x);
  ImGui::EndChild();

  if (show_stats) {
    ImGui::SameLine();
    ImGui::BeginChild("cs_stats", ImVec2(kStatsW, 0), true);
    DrawStatsPanel();
    ImGui::EndChild();
  }

  ro::EndRoWindow();

  // Direction B du drag-drop : relâcher un item ÉQUIPÉ (glissé depuis un slot) sur la
  // fenêtre inventaire = le déséquiper. On détecte NOUS-MÊMES le relâché sur l'inventaire
  // (PointOverViewer couvre TOUTE la fenêtre, y compris les tuiles vides sans case), car
  // un item peut être lâché sur une zone SANS cible ImGui. Le payload "BGN_EQUIP" (posé
  // par la source du slot) sert d'aperçu + porte l'index inventaire.
  {
    static int s_unequip_inv = 0;  // invIndex du drag BGN_EQUIP en cours (0 = aucun)
    const ImGuiPayload* p = ImGui::GetDragDropPayload();
    if (p && p->IsDataType("BGN_EQUIP") && p->Data) {
      s_unequip_inv = *static_cast<const int*>(p->Data);
      if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {  // relâché ce frame
        const ImVec2 m = ImGui::GetMousePos();
        if (auto* iv = Bourgeon::Instance().inventory_viewer())
          if (iv->PointOverViewer(static_cast<int>(m.x), static_cast<int>(m.y)))
            SendUnequip(s_unequip_inv);  // sur l'inventaire -> déséquiper
        s_unequip_inv = 0;
      }
    } else {
      s_unequip_inv = 0;
    }
  }
  ImGui::PopStyleVar(5);
}
