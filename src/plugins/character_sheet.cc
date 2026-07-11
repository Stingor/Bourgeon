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
#include "plugins/inventory_viewer.h"  // LinkItemToChat / EquipDraggedItem (drag-drop, chat)
#include "plugins/moonlight_ui.h"      // SaveSettings (persistance des presets)
#include "plugins/imgui_escape.h"
#include "ui/ro_imgui.h"
#include "utils/tinf_inflate.h"  // inflate zlib pour les emblèmes de guilde (.ebm)

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
constexpr int keType     = 0x00;   // type d'item (equip 4/5/8/9/0xb-0xf)
constexpr int keCards    = 0x1c;   // 4 u32 (nameid des cartes / forge)
constexpr int keGrade    = 0x88;   // i16 : grade d'enchant
constexpr int keWear     = 0x0c;   // etat porte (!=0 => equipe)
constexpr int kNormalSlots = 10;   // slots equip normaux 0..9 (cf. disposition doll)
constexpr int kEqpHandR    = 0x2;  // masque EQP main droite (arme) -> detecte le dual-wield
constexpr int kMaxPresetsPerChar = 5;  // plafond de presets par personnage

//  Presets d'equipement : CID (clef par perso) + liste inventaire (session)
constexpr uintptr_t kOwnCharId  = 0x015fb9a8;  // g_Own_CharId (cf. project_own_session_globals)
constexpr uintptr_t kInvListHead = 0x015fbab0;  // std::list<ItemSkillInfo> (session+0x16f0)
constexpr uintptr_t kInvCount    = 0x015fbab4;  // _Mysize

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
constexpr uint16_t kOpEquip   = 0x0998;  // CZ_REQ_WEAR_EQUIP_V5 {op, invIndex, position:4}
constexpr uint32_t kEqpHandL  = 0x20;    // EQP main GAUCHE (bouclier / arme dual-wield)
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
struct PoseOpt { int anim; bool animate; const char* label; };
const PoseOpt kPoses[] = {
    {0, false, "Repos"},          {1, false, "Marche"}, {1, true, "Marche (animé)"},
    {2, false, "Assis"},          {4, false, "Combat"}, {4, true, "Combat (animé)"},
};
const int kPoseCount = 6;
// Libellé de BASE d'une pose (1er match par anim, sans « (animé) ») : noms de fichier GIF.
const char* PoseLabel(int anim) {
  for (int i = 0; i < kPoseCount; ++i)
    if (kPoses[i].anim == anim) return kPoses[i].label;
  return "Combat";  // repli
}
// Libellé COMPLET de la pose courante (anim + animé) : aperçu du combo.
const char* PoseLabelFull(int anim, bool animate) {
  for (int i = 0; i < kPoseCount; ++i)
    if (kPoses[i].anim == anim && kPoses[i].animate == animate) return kPoses[i].label;
  return PoseLabel(anim);
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

//  Vue POD d'un item d'inventaire (pour resoudre un preset -> index courant). Volontairement
//  POD (aucun objet a unwinding) car remplie sous __try : cf. C2712.
struct InvItemLite {
  int      index;
  uint32_t nameid;
  int      refine;
  int      type;
  int      loc;
  uint32_t cards[4];
  int      grade;
  bool     equipped;
};
//  Parcourt la std::list session (kInvListHead) et remplit out[] (POD). Renvoie le nombre lu.
//  MSVC std::list : *(head) = sentinelle ; node : next@+0, valeur(ItemSkillInfo)@+8.
int ReadInventoryLite(InvItemLite* out, int cap) {
  int n = 0;
  __try {
    void* sentinel = *reinterpret_cast<void* const*>(kInvListHead);
    const int count = *reinterpret_cast<const int*>(kInvCount);
    if (!sentinel || count <= 0) return 0;
    void* node = *reinterpret_cast<void* const*>(sentinel);  // sentinelle->next = 1er noeud
    for (int i = 0; i < count && n < cap && node && node != sentinel; ++i) {
      const uint8_t* info = reinterpret_cast<const uint8_t*>(node) + 8;
      InvItemLite& it = out[n];
      it.type     = *reinterpret_cast<const int*>(info + keType);
      it.index    = *reinterpret_cast<const int*>(info + keInvIndex);
      it.loc      = *reinterpret_cast<const int*>(info + keLocation);
      it.equipped = *reinterpret_cast<const int*>(info + keWear) != 0;
      it.refine   = *reinterpret_cast<const int*>(info + keRefine);
      it.grade    = *reinterpret_cast<const short*>(info + keGrade);
      for (int c = 0; c < 4; ++c)
        it.cards[c] = *reinterpret_cast<const uint32_t*>(info + keCards + c * 4);
      const uint32_t capS = *reinterpret_cast<const uint32_t*>(info + keResCap);
      const char* rn = (capS > 15) ? *reinterpret_cast<const char* const*>(info + keResname)
                                   : reinterpret_cast<const char*>(info + keResname);
      it.nameid = (rn && rn[0]) ? static_cast<uint32_t>(std::atoi(rn)) : 0;
      ++n;
      node = *reinterpret_cast<void* const*>(node);  // node->next
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return n;
}
//  Cartes/grade d'un item d'inventaire par index (pour completer l'identite au save).
bool FindInvLiteByIndex(const InvItemLite* inv, int n, int index, InvItemLite* out) {
  for (int i = 0; i < n; ++i)
    if (inv[i].index == index) { if (out) *out = inv[i]; return true; }
  return false;
}
//  Lit un slot du tableau equip (session+0x17d0+slot*0xf8, meme layout ItemSkillInfo) dans une
//  InvItemLite. Sert a AJOUTER les items PORTES aux candidats de resolution : la liste inventaire
//  ne restitue pas toujours les items equipes -> un item commun deja porte serait declare
//  « manquant » a tort. SEH/POD.
bool ReadEquipLite(int slot, InvItemLite* out) {
  __try {
    const uint8_t* e = reinterpret_cast<const uint8_t*>(kSession + kEquipBase + slot * kSlotStride);
    if (*reinterpret_cast<const int*>(e + keInvIndex) == 0 ||
        *reinterpret_cast<const int*>(e + kePresent) != 1)
      return false;
    out->index    = *reinterpret_cast<const int*>(e + keInvIndex);
    out->type     = *reinterpret_cast<const int*>(e + keType);
    out->loc      = *reinterpret_cast<const int*>(e + keLocation);
    out->equipped = true;
    out->refine   = *reinterpret_cast<const int*>(e + keRefine);
    out->grade    = *reinterpret_cast<const short*>(e + keGrade);
    for (int c = 0; c < 4; ++c)
      out->cards[c] = *reinterpret_cast<const uint32_t*>(e + keCards + c * 4);
    const uint32_t cap = *reinterpret_cast<const uint32_t*>(e + keResCap);
    const char* rn = (cap > 15) ? *reinterpret_cast<const char* const*>(e + keResname)
                                : reinterpret_cast<const char*>(e + keResname);
    out->nameid = (rn && rn[0]) ? static_cast<uint32_t>(std::atoi(rn)) : 0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
//  Resout un item de preset -> index inventaire courant (-1 si absent). Priorite au match
//  EXACT (refine+cartes+grade) ; a defaut, 1er item de meme nameid.
int ResolvePresetItem(const EquipPresetItem& pi, const InvItemLite* inv, int n) {
  int fallback = -1;
  for (int i = 0; i < n; ++i) {
    if (inv[i].nameid != pi.nameid || pi.nameid == 0) continue;
    const bool exact = inv[i].refine == pi.refine && inv[i].grade == pi.grade &&
                       inv[i].cards[0] == pi.cards[0] && inv[i].cards[1] == pi.cards[1] &&
                       inv[i].cards[2] == pi.cards[2] && inv[i].cards[3] == pi.cards[3];
    if (exact) return inv[i].index;
    if (fallback < 0) fallback = inv[i].index;
  }
  return fallback;
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

//  Infos de GUILDE (RE live x32dbg 2026-07-11). CGuild @0x0159c188 : +0 = nom de guilde
//  (std::string), +0xdc = tête de la liste des membres (sentinelle). Niveau @0x0159c1e8,
//  est-maître @0x0159c23c, guildId @0x0159c230 (clé emblème = acteur+0x2e8 aussi). La
//  POSITION du joueur = son enregistrement dans le roster : payload à node+8 (AID @+0,
//  nom @+8, NOM DE POSTE std::string @+0x34), on matche par AID.
constexpr uintptr_t kGuildObj      = 0x0159c188;
constexpr int       kGuildListHead = 0xdc;
constexpr uintptr_t kGuildLevel    = 0x0159c1e8;
constexpr uintptr_t kGuildIsMaster = 0x0159c23c;
constexpr uintptr_t kGuildIdAddr   = 0x0159c230;
constexpr int       kMemAid    = 0x08;   // node+8    = AID du membre
constexpr int       kMemPosStr = 0x3c;   // node+0x3c = nom de poste (std::string)

// Copie une std::string MSVC (SSO/heap) de `addr` vers un buffer C (null-terminé). SEH.
void ReadStdStringSEH(uintptr_t addr, char* out, int outCap) {
  out[0] = '\0';
  __try {
    const uint8_t* s = reinterpret_cast<const uint8_t*>(addr);
    const uint32_t cap = *reinterpret_cast<const uint32_t*>(s + 0x14);  // capacité SSO
    const char* p = (cap > 15) ? *reinterpret_cast<const char* const*>(s)
                               : reinterpret_cast<const char*>(s);
    if (p) {
      int i = 0;
      for (; i < outCap - 1 && p[i]; ++i) out[i] = p[i];
      out[i] = '\0';
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

struct GuildInfo {
  bool present = false;
  char name[32] = {0};
  char pos[32] = {0};   // nom de poste (« rang »)
  int  level = 0;
  bool master = false;
  int  guildId = 0;
};
bool ReadGuild(GuildInfo* g) {
  __try {
    g->guildId = *reinterpret_cast<const int*>(kGuildIdAddr);
    ReadStdStringSEH(kGuildObj, g->name, sizeof(g->name));  // CGuild+0 = nom
    if (g->guildId <= 0 || g->name[0] == '\0') return false;  // pas en guilde
    g->level   = *reinterpret_cast<const int*>(kGuildLevel);
    g->master  = *reinterpret_cast<const int*>(kGuildIsMaster) != 0;
    g->present = true;
    // Position : parcourir le roster (liste chaînée) et matcher mon AID.
    const int aid = Bourgeon::Instance().client().session().aid();
    const uint8_t* sentinel =
        *reinterpret_cast<uint8_t* const*>(kGuildObj + kGuildListHead);
    if (sentinel && aid != 0) {
      const uint8_t* node = *reinterpret_cast<uint8_t* const*>(sentinel);  // ->next
      for (int guard = 0; node && node != sentinel && guard < 512; ++guard) {
        if (*reinterpret_cast<const int*>(node + kMemAid) == aid) {
          ReadStdStringSEH(reinterpret_cast<uintptr_t>(node) + kMemPosStr, g->pos,
                           sizeof(g->pos));
          break;
        }
        node = *reinterpret_cast<uint8_t* const*>(node);  // ->next
      }
    }
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

// ── Emblème de guilde ────────────────────────────────────────────────────────
// L'emblème est un fichier <jeu>\_tmpEmblem\<nom>_<guildId>_<ver>.ebm = un BMP 24x24
// 24-bit COMPRESSÉ ZLIB. Le TexMgr générique ne le décompresse pas -> on lit le fichier,
// on inflate le zlib (tinf) et on parse le BMP nous-mêmes. GetEmblemPath(g_CGuildMgr,
// &path, guildId) renvoie le nom relatif du .ebm ET déclenche le download si absent.
// RE 2026-07-11 ; voir [[project_guild_window_re]].
constexpr uintptr_t kCGuildMgrPtr  = 0x01254d70;  // *ptr = g_CGuildMgr (CGuildEmblemMgr)
constexpr uintptr_t kGetEmblemPath = 0x0061d370;  // __thiscall(this, out_str, guildId)
using GetEmblemPath_t = void*(__thiscall*)(void*, void*, void*);
// GetEmblemPath isolé dans un helper POD : le __try ne peut pas cohabiter avec un objet
// à unwinding (les std::vector de LoadEmblemFromFile) dans la même fonction (C2712).
void GetEmblemPathSafe(int guildId, char* out, int outCap) {
  out[0] = '\0';
  __try {
    void* mgr = *reinterpret_cast<void* const*>(kCGuildMgrPtr);
    if (!mgr) return;
    uint8_t sbuf[0x18];  // std::string non-init (GetEmblemPath l'écrase sans la lire)
    reinterpret_cast<GetEmblemPath_t>(kGetEmblemPath)(
        mgr, reinterpret_cast<void*>(sbuf), reinterpret_cast<void*>(guildId));
    const uint32_t cap = *reinterpret_cast<const uint32_t*>(sbuf + 0x14);  // capacité SSO
    const char* p = (cap > 15) ? *reinterpret_cast<char* const*>(sbuf)
                               : reinterpret_cast<const char*>(sbuf);
    if (p) { int i = 0; for (; i < outCap - 1 && p[i]; ++i) out[i] = p[i]; out[i] = '\0'; }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}
// Lit un .ebm (BMP 24x24 24-bit compressé zlib) et le convertit en texture ImGui.
IconTex LoadEmblemFromFile(const char* fullPath) {
  FILE* fp = nullptr;
  if (fopen_s(&fp, fullPath, "rb") != 0 || !fp) return {};  // pas encore téléchargé
  std::fseek(fp, 0, SEEK_END);
  const long fsz = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  if (fsz <= 2 || fsz > (1 << 20)) { std::fclose(fp); return {}; }
  std::vector<uint8_t> comp(static_cast<size_t>(fsz));
  const size_t rd = std::fread(comp.data(), 1, static_cast<size_t>(fsz), fp);
  std::fclose(fp);
  if (rd != static_cast<size_t>(fsz)) return {};
  std::vector<uint8_t> bmp;
  if (!tinf::zlib_uncompress(comp.data(), comp.size(), bmp) || bmp.size() < 54) return {};
  if (bmp[0] != 'B' || bmp[1] != 'M') return {};
  const int32_t  w       = *reinterpret_cast<const int32_t*>(&bmp[0x12]);
  const int32_t  hraw    = *reinterpret_cast<const int32_t*>(&bmp[0x16]);
  const int16_t  bpp     = *reinterpret_cast<const int16_t*>(&bmp[0x1c]);
  const uint32_t dataOff = *reinterpret_cast<const uint32_t*>(&bmp[0x0a]);
  const int  h        = (hraw < 0) ? -hraw : hraw;
  const bool bottomUp = hraw > 0;  // BMP standard = bottom-up
  if (w <= 0 || w > 64 || h <= 0 || h > 64 || bpp != 24) return {};  // 24-bit uniquement
  const size_t rowSize = static_cast<size_t>((w * 3 + 3) & ~3);  // lignes alignées 4 octets
  if (static_cast<size_t>(dataOff) + rowSize * h > bmp.size()) return {};
  std::vector<uint8_t> argb(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    const int srcY = bottomUp ? (h - 1 - y) : y;
    const uint8_t* row = &bmp[dataOff + static_cast<size_t>(srcY) * rowSize];
    for (int x = 0; x < w; ++x) {
      const uint8_t b = row[x * 3], g = row[x * 3 + 1], r = row[x * 3 + 2];
      const bool ck = (r == 0xFF && g == 0 && b == 0xFF);  // magenta -> transparent
      const size_t o = (static_cast<size_t>(y) * w + x) * 4;
      argb[o] = b; argb[o + 1] = g; argb[o + 2] = r; argb[o + 3] = ck ? 0 : 0xFF;
    }
  }
  return {Overlay_CreateTextureARGB(argb.data(), w, h), w, h};
}
IconTex ResolveEmblem(int guildId) {
  static unsigned s_epoch = 0;
  struct Entry { IconTex tex; DWORD lastTry = 0; };
  static std::unordered_map<int, Entry> s_cache;
  const unsigned e = Overlay_DeviceEpoch();
  if (e != s_epoch) { s_cache.clear(); s_epoch = e; }
  if (guildId <= 0) return {};
  Entry& en = s_cache[guildId];
  if (en.tex.tex) return en.tex;  // déjà chargé
  const DWORD now = GetTickCount();
  // Retry throttlé (3 s) : l'.ebm peut ne pas être encore téléchargé au 1er appel.
  if (en.lastTry != 0 && now - en.lastTry < 3000) return {};
  en.lastTry = now;
  char rel[264] = {0};
  GetEmblemPathSafe(guildId, rel, sizeof(rel));  // nom relatif du .ebm (+ déclenche download)
  if (!rel[0]) return {};
  char exe[MAX_PATH] = {0};  // chemin complet : <dossier du jeu>\_tmpEmblem\<nom>.ebm
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  if (char* slash = std::strrchr(exe, '\\')) *slash = '\0';
  char full[MAX_PATH];
  std::snprintf(full, sizeof(full), "%s\\_tmpEmblem\\%s", exe, rel);
  en.tex = LoadEmblemFromFile(full);
  return en.tex;
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
// Équipe l'item d'index `invIndex` à la position EQP `position`, PAQUET BRUT (comme le natif :
// CZ_REQ_WEAR_EQUIP_V5 0x0998 en clair ; l'obfuscation XOR est appliquée après par le client).
// Contrairement au dispatcher (throttlé à 1/frame), le serveur traite un LOT d'un coup.
void SendEquip(int invIndex, uint32_t position) {
  if (invIndex <= 0) return;
  uint8_t pkt[8];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpEquip;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(invIndex);
  *reinterpret_cast<uint32_t*>(pkt + 4) = position;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}
// amount = nb de points à monter en UN paquet (le serveur pc_statusup clampe au coût
// abordable + au plafond de la stat). L'octet amount est lu NON signé (RFIFOB) -> [1,255].
void SendStatUp(int statType, int amount = 1) {
  if (amount < 1) return;
  if (amount > 255) amount = 255;
  uint8_t pkt[5];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpStatUp;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(statType);
  pkt[4] = static_cast<uint8_t>(amount);
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

// ── Raccourcis clavier de preset ─────────────────────────────────────────────
constexpr uintptr_t kGetHotKey = 0x00d80950;  // GetHotKey(out, category, slot) __stdcall RET 0xc
constexpr uintptr_t kStrFree   = 0x004f08f0;  // libère une std::string MSVC (ecx=base)
using GetHotKey_t = void* (__stdcall*)(void*, int, int);
using StrFree_t   = void (__fastcall*)(void*);

// Conversions ImGuiKey <-> VK Windows (lettres, chiffres, F1-F12 = combos réalistes de preset).
int ImGuiKeyToVk(ImGuiKey k) {
  if (k >= ImGuiKey_A && k <= ImGuiKey_Z)    return 0x41 + (k - ImGuiKey_A);
  if (k >= ImGuiKey_0 && k <= ImGuiKey_9)    return 0x30 + (k - ImGuiKey_0);
  if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12) return 0x70 + (k - ImGuiKey_F1);
  return 0;
}
ImGuiKey VkToImGuiKey(int vk) {
  if (vk >= 0x41 && vk <= 0x5A) return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 0x41));
  if (vk >= 0x30 && vk <= 0x39) return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - 0x30));
  if (vk >= 0x70 && vk <= 0x7B) return static_cast<ImGuiKey>(ImGuiKey_F1 + (vk - 0x70));
  return ImGuiKey_None;
}
// Première touche PRINCIPALE pressée cette frame (hors modificateurs), en VK. 0 si aucune.
int CaptureMainVk() {
  for (ImGuiKey k = ImGuiKey_A; k <= ImGuiKey_Z; k = static_cast<ImGuiKey>(k + 1))
    if (ImGui::IsKeyPressed(k, false)) return ImGuiKeyToVk(k);
  for (ImGuiKey k = ImGuiKey_0; k <= ImGuiKey_9; k = static_cast<ImGuiKey>(k + 1))
    if (ImGui::IsKeyPressed(k, false)) return ImGuiKeyToVk(k);
  for (ImGuiKey k = ImGuiKey_F1; k <= ImGuiKey_F12; k = static_cast<ImGuiKey>(k + 1))
    if (ImGui::IsKeyPressed(k, false)) return ImGuiKeyToVk(k);
  return 0;
}
// Étiquette d'un combo : "Ctrl+Maj+F1" / "(aucun)".
void HotkeyLabel(const EquipPreset& ep, char* out, int cap) {
  if (ep.hotkeyVk == 0) { std::snprintf(out, cap, "(aucun)"); return; }
  char mods[24] = {0};
  if (ep.hkCtrl)  std::strncat(mods, "Ctrl+", sizeof(mods) - std::strlen(mods) - 1);
  if (ep.hkAlt)   std::strncat(mods, "Alt+",  sizeof(mods) - std::strlen(mods) - 1);
  if (ep.hkShift) std::strncat(mods, "Maj+",  sizeof(mods) - std::strlen(mods) - 1);
  const char* kn = ImGui::GetKeyName(VkToImGuiKey(ep.hotkeyVk));
  std::snprintf(out, cap, "%s%s", mods, (kn && kn[0]) ? kn : "?");
}
// Lit le raccourci natif d'un slot de la barre (via GetHotKey Lua, rebind-aware). mainVk + le VK
// du modificateur (0 si aucun). SEH (touche Lua + libère les 2 std::string du wrapper).
bool ReadNativeHotkey(int cat, int slot, int* mainVk, int* modVk) {
  *mainVk = 0; *modVk = 0;
  bool ok = false;
  __try {
    alignas(4) uint8_t buf[0x40];
    std::memset(buf, 0, sizeof(buf));
    reinterpret_cast<GetHotKey_t>(kGetHotKey)(buf, cat, slot);
    const int kc1 = *reinterpret_cast<int*>(buf + 0x00);
    const int kc2 = *reinterpret_cast<int*>(buf + 0x04);
    reinterpret_cast<StrFree_t>(kStrFree)(buf + 0x08);
    reinterpret_cast<StrFree_t>(kStrFree)(buf + 0x20);
    auto isMod = [](int k) { return k == VK_CONTROL || k == VK_SHIFT || k == VK_MENU; };
    if (isMod(kc1))      { *modVk = kc1; *mainVk = kc2; }
    else if (isMod(kc2)) { *modVk = kc2; *mainVk = kc1; }
    else                 { *mainVk = kc1; }
    ok = (*mainVk != 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  return ok;
}

// Mini-icone d'un item de preset (icone + refine, survol = nom). Affichage seul.
void DrawPresetItemIcon(const EquipPresetItem& pi, float sz) {
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(sz, sz));  // reserve la place + hit-test pour le survol
  const bool hov = ImGui::IsItemHovered();
  const ImVec2 p1(p0.x + sz, p0.y + sz);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, IM_COL32(206, 206, 206, 255), 3.0f);
  dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 80), 3.0f);
  IconTex ic = ResolveIcon(pi.nameid);
  if (ic.tex)
    dl->AddImage(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(p0.x + 2, p0.y + 2),
                 ImVec2(p1.x - 2, p1.y - 2));
  if (pi.refine > 0) {  // "+N" bas-droite, noir cerne blanc
    char rf[8];
    std::snprintf(rf, sizeof(rf), "+%d", pi.refine);
    const ImVec2 ts = ImGui::CalcTextSize(rf);
    const ImVec2 rp(p1.x - ts.x - 2, p1.y - ts.y - 1);
    for (int oy = -1; oy <= 1; ++oy)
      for (int ox = -1; ox <= 1; ++ox)
        if (ox || oy) dl->AddText(ImVec2(rp.x + ox, rp.y + oy), IM_COL32(255, 255, 255, 255), rf);
    dl->AddText(rp, IM_COL32(0, 0, 0, 255), rf);
  }
  if (hov) {
    if (pi.refine > 0) ImGui::SetTooltip("+%d %s", pi.refine, ItemName(pi.nameid));
    else               ImGui::SetTooltip("%s", ItemName(pi.nameid));
  }
}

}  // namespace

CharacterSheet::CharacterSheet() {}

// ── Presets d'equipement ─────────────────────────────────────────────────────
void CharacterSheet::SaveCurrentEquipAsPreset(const char* name) {
  EquipPreset p;
  p.cid  = static_cast<uint32_t>(ReadInt(kOwnCharId));
  p.name = name;
  for (int s = 0; s < kNormalSlots; ++s) {
    InvItemLite li{};  // identite COMPLETE lue directement du tableau equip (cartes/grade inclus)
    if (!ReadEquipLite(s, &li)) continue;  // slot vide
    EquipPresetItem pi{};
    pi.nameid = li.nameid;
    pi.refine = li.refine;
    pi.grade  = li.grade;
    for (int c = 0; c < 4; ++c) pi.cards[c] = li.cards[c];
    // Slot 5 = bouclier / main gauche : une ARME (loc EQP_HAND_R) qui y siege = dual-wield.
    pi.leftHand = (s == 5) && (li.loc & kEqpHandR) != 0;
    p.items.push_back(pi);
  }
  // Ecrase un preset existant de MEME nom pour ce perso, sinon ajoute.
  bool replaced = false;
  for (auto& ex : equip_presets_)
    if (ex.cid == p.cid && ex.name == p.name) { ex = std::move(p); replaced = true; break; }
  if (!replaced) {
    int cnt = 0;  // plafond par perso (kMaxPresetsPerChar)
    for (const auto& ex : equip_presets_) if (ex.cid == p.cid) ++cnt;
    if (cnt >= kMaxPresetsPerChar) { preset_status_ = "Limite de 5 presets atteinte"; return; }
    equip_presets_.push_back(std::move(p));
  }
  preset_status_ = "Preset enregistré";
  if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
}

void CharacterSheet::ApplyPreset(const EquipPreset& p) {
  InvItemLite inv[512];
  int n = ReadInventoryLite(inv, 512);
  // 1) Items PORTES : on relève leur index (pour le déséquip) ET on les AJOUTE aux candidats
  //    de résolution (la liste inventaire ne restitue pas toujours les items équipés) -> un
  //    item commun déjà porté est ainsi résolu au lieu d'être déclaré « manquant ».
  int equipped[kNormalSlots]; int ne = 0;
  for (int s = 0; s < kNormalSlots; ++s) {
    InvItemLite li{};
    if (!ReadEquipLite(s, &li)) continue;
    equipped[ne++] = li.index;
    // Remplace l'entrée existante de même index par la copie équip (identité fiable : la copie
    // liste peut avoir un nameid vide pour un item porté), sinon l'ajoute.
    int existing = -1;
    for (int i = 0; i < n; ++i) if (inv[i].index == li.index) { existing = i; break; }
    if (existing >= 0) inv[existing] = li;
    else if (n < 512) inv[n++] = li;
  }
  // 2) Resoudre chaque item du preset -> index cible + position EQP a envoyer (non deja porte).
  int targets[32]; int nt = 0;
  struct ToEquip { int index; uint32_t pos; } toEquip[32]; int neq = 0;
  int missing = 0;
  for (const EquipPresetItem& pi : p.items) {
    const int idx = ResolvePresetItem(pi, inv, n);
    if (idx < 0) { ++missing; continue; }
    if (nt < 32) targets[nt++] = idx;
    bool eq = false;
    for (int k = 0; k < ne; ++k) if (equipped[k] == idx) eq = true;
    if (!eq && neq < 32) {
      InvItemLite li{};
      if (FindInvLiteByIndex(inv, n, idx, &li)) {
        // Position = masque EQP de l'item (info+8, comme le natif) ; forcee a la main GAUCHE
        // pour une arme dual-wield (sinon le serveur pre-renewal la remettrait a droite).
        const uint32_t pos = pi.leftHand ? kEqpHandL : static_cast<uint32_t>(li.loc);
        toEquip[neq++] = {idx, pos};
      }
    }
  }
  // 3) Tout en PAQUETS BRUTS, envoyes d'un coup (le serveur traite le lot) : desequips d'abord
  //    (0x00AB), puis equips (0x0998). Comme le desequip de masse, c'est instantane.
  for (int k = 0; k < ne; ++k) {
    bool keep = false;
    for (int t = 0; t < nt; ++t) if (targets[t] == equipped[k]) keep = true;
    if (!keep) SendUnequip(equipped[k]);
  }
  for (int e = 0; e < neq; ++e) SendEquip(toEquip[e].index, toEquip[e].pos);
  preset_status_ = (missing > 0)
                       ? "Appliqué (" + std::to_string(missing) + " item(s) manquant(s))"
                       : "Preset appliqué";
}

bool CharacterSheet::HotkeyConflict(int vk, bool ctrl, bool alt, bool shift, int selfIdx,
                                    char* what, int cap) {
  if (cap > 0) what[0] = '\0';
  if (vk == 0) return false;
  // Réservé : Alt+F ouvre/ferme la fiche elle-même.
  if (vk == 0x46 && alt && !ctrl && !shift) {
    std::snprintf(what, cap, "l'ouverture de la fiche (Alt+F)");
    return true;
  }
  const uint32_t cid = static_cast<uint32_t>(ReadInt(kOwnCharId));
  // a) Un AUTRE preset du même perso a déjà ce combo.
  for (int i = 0; i < static_cast<int>(equip_presets_.size()); ++i) {
    if (i == selfIdx) continue;
    const EquipPreset& e = equip_presets_[i];
    if (e.cid == cid && e.hotkeyVk == vk && e.hkCtrl == ctrl && e.hkAlt == alt &&
        e.hkShift == shift) {
      std::snprintf(what, cap, "le preset « %s »", e.name.c_str());
      return true;
    }
  }
  // b) Un raccourci natif de la barre de skills (onglet 1 = cat 0, onglet 2 = cat 3, 36 slots).
  const int cats[2] = {0, 3};
  for (int ci = 0; ci < 2; ++ci)
    for (int s = 0; s < 36; ++s) {
      int mvk, modvk;
      if (!ReadNativeHotkey(cats[ci], s, &mvk, &modvk) || mvk != vk) continue;
      const bool nCtrl = (modvk == VK_CONTROL), nAlt = (modvk == VK_MENU),
                 nShift = (modvk == VK_SHIFT);
      if (nCtrl == ctrl && nAlt == alt && nShift == shift) {
        std::snprintf(what, cap, "un raccourci natif (barre de skills)");
        return true;
      }
    }
  return false;
}

void CharacterSheet::ProcessPresetHotkeys() {
  if (hk_capturing_ >= 0) return;  // en pleine capture d'un combo : ne pas déclencher
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput) return;    // saisie de texte (chat…) : ne pas déclencher
  const uint32_t cid = static_cast<uint32_t>(ReadInt(kOwnCharId));
  for (const EquipPreset& ep : equip_presets_) {
    if (ep.cid != cid || ep.hotkeyVk == 0) continue;
    if (io.KeyCtrl != ep.hkCtrl || io.KeyAlt != ep.hkAlt || io.KeyShift != ep.hkShift) continue;
    const ImGuiKey k = VkToImGuiKey(ep.hotkeyVk);
    if (k != ImGuiKey_None && ImGui::IsKeyPressed(k, false)) { ApplyPreset(ep); break; }
  }
}

// Onglet Presets : pour chaque preset du perso, son nom + les ICÔNES de ses items (survol =
// nom) + Charger/Suppr ; en bas, saisie du nom + Sauver l'équipement porté (cap 5).
void CharacterSheet::DrawPresetsTab() {
  const uint32_t cid = static_cast<uint32_t>(ReadInt(kOwnCharId));
  std::vector<int> mine;  // indices (dans equip_presets_) des presets du perso courant
  for (int i = 0; i < static_cast<int>(equip_presets_.size()); ++i)
    if (equip_presets_[i].cid == cid) mine.push_back(i);

  auto bw = [](const char* t) {
    return ImGui::CalcTextSize(t).x + ImGui::GetStyle().FramePadding.x * 2.0f + 10.0f;
  };
  const ImVec4 kGray(0.35f, 0.35f, 0.42f, 1.0f);
  const float load_w = bw("Charger"), del_w = bw("Suppr");
  const float icon = 30.0f, igap = 3.0f;

  if (mine.empty()) {
    ImGui::TextColored(kGray, "Aucun preset enregistré pour ce personnage.");
    ImGui::Spacing();
  }

  // On applique/supprime APRÈS le rendu (ne pas invalider equip_presets_ en cours d'itération).
  int to_load = -1, to_delete = -1;
  for (int mi = 0; mi < static_cast<int>(mine.size()); ++mi) {
    const EquipPreset& ep = equip_presets_[mine[mi]];
    ImGui::PushID(mi);
    // Ligne titre : nom (gauche) + Charger/Suppr (droite).
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kBlack, "%s", ep.name.c_str());
    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, avail - load_w - del_w - 6.0f));  // boutons à droite
    if (ro::RoButton("Charger", load_w)) to_load = mine[mi];
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Rééquipe exactement ce jeu (déséquipe le reste)");
    ImGui::SameLine(0.0f, 4.0f);
    if (ro::RoButton("Suppr", del_w)) to_delete = mine[mi];
    // Rangée d'icônes des items (wrap selon la largeur disponible).
    if (ep.items.empty()) {
      ImGui::TextColored(kGray, "(vide)");
    } else {
      const float availw = ImGui::GetContentRegionAvail().x;
      const int perRow = std::max(1, static_cast<int>((availw + igap) / (icon + igap)));
      for (int k = 0; k < static_cast<int>(ep.items.size()); ++k) {
        if (k % perRow != 0) ImGui::SameLine(0.0f, igap);
        DrawPresetItemIcon(ep.items[k], icon);
      }
    }
    // Ligne raccourci clavier : libellé + Définir/Effacer, ou mode capture.
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kGray, "Raccourci :");
    ImGui::SameLine();
    if (hk_capturing_ == mine[mi]) {
      ImGui::TextColored(kBlack, "appuie sur une touche…  (Échap : annuler)");
      ImGuiIO& io = ImGui::GetIO();
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        hk_capturing_ = -1;
        hk_conflict_msg_.clear();
      } else if (const int vk = CaptureMainVk()) {
        const bool c = io.KeyCtrl, a = io.KeyAlt, sh = io.KeyShift;
        char what[64];
        if (HotkeyConflict(vk, c, a, sh, mine[mi], what, sizeof(what))) {
          hk_conflict_msg_ = std::string("Déjà utilisé par ") + what + " — choisis un autre combo";
        } else {  // libre : on assigne + persiste
          EquipPreset& e = equip_presets_[mine[mi]];
          e.hotkeyVk = vk; e.hkCtrl = c; e.hkAlt = a; e.hkShift = sh;
          hk_capturing_ = -1;
          hk_conflict_msg_.clear();
          if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
        }
      }
      if (!hk_conflict_msg_.empty()) {
        ImGui::TextColored(ImVec4(0.80f, 0.20f, 0.20f, 1.0f), "%s", hk_conflict_msg_.c_str());
      }
    } else {
      char hkl[48];
      HotkeyLabel(ep, hkl, sizeof(hkl));
      ImGui::TextColored(kBlack, "%s", hkl);
      ImGui::SameLine(0.0f, 6.0f);
      if (ro::RoButton("Définir", bw("Définir"))) {
        hk_capturing_ = mine[mi];
        hk_conflict_msg_.clear();
      }
      if (ep.hotkeyVk != 0) {
        ImGui::SameLine(0.0f, 4.0f);
        if (ro::RoButton("Effacer", bw("Effacer"))) {
          EquipPreset& e = equip_presets_[mine[mi]];
          e.hotkeyVk = 0; e.hkCtrl = e.hkAlt = e.hkShift = false;
          if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
        }
      }
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::PopID();
  }

  // Section sauvegarde (bas de l'onglet).
  ImGui::TextColored(kBlack, "Enregistrer l'équipement porté (%d/%d)",
                     static_cast<int>(mine.size()), kMaxPresetsPerChar);
  const float save_w = bw("Sauver l'actuel");
  ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - save_w - 8.0f));
  ImGui::InputTextWithHint("##cs_pname", "nom du preset", preset_name_buf_,
                           sizeof(preset_name_buf_));
  ImGui::SameLine(0.0f, 4.0f);
  // Cap à 5 : on autorise quand même l'ÉCRASEMENT d'un preset existant de même nom.
  bool name_exists = false;
  for (int idx : mine) if (equip_presets_[idx].name == preset_name_buf_) name_exists = true;
  const bool at_cap = static_cast<int>(mine.size()) >= kMaxPresetsPerChar && !name_exists;
  const bool can_save = preset_name_buf_[0] != '\0' && !at_cap;
  if (!can_save) ImGui::BeginDisabled();
  if (ro::RoButton("Sauver l'actuel", save_w)) {
    SaveCurrentEquipAsPreset(preset_name_buf_);
    preset_name_buf_[0] = '\0';
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip(at_cap
                          ? "Limite de 5 presets atteinte (renomme un existant ou supprime-en un)"
                          : "Enregistre l'équipement porté actuellement sous ce nom");
  if (!can_save) ImGui::EndDisabled();
  if (!preset_status_.empty()) ImGui::TextColored(kGray, "%s", preset_status_.c_str());

  // Actions différées (les indices mine[] restent valides : SaveCurrentEquipAsPreset n'ajoute
  // qu'en fin de vecteur ou écrase en place -> aucun décalage des indices déjà capturés).
  if (to_load >= 0) ApplyPreset(equip_presets_[to_load]);
  if (to_delete >= 0) {
    equip_presets_.erase(equip_presets_.begin() + to_delete);
    preset_status_.clear();
    hk_capturing_ = -1;  // l'indice capturé peut être invalidé par l'erase
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
  }
}

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
  const ImVec2 hdr_p0 = ImGui::GetCursorScreenPos();  // coin haut-gauche (emblème)
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
  // Ligne guilde : « Nom de guilde [Poste] » (le poste = le « rang »). Lue live du CGuild
  // + roster ; rien affiché hors guilde.
  GuildInfo gi;
  const bool has_guild = ReadGuild(&gi);
  if (has_guild) {
    char gline[80];
    if (gi.pos[0]) std::snprintf(gline, sizeof(gline), "%s [%s]", gi.name, gi.pos);
    else           std::snprintf(gline, sizeof(gline), "%s", gi.name);
    centered(gline);
  }
  Stats s{};
  if (ReadStats(&s))
  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "HP: %d/%d SP: %d/%d", s.hp, s.hp_max, s.sp, s.sp_max);
    centered(buf);
  }
  // Bas du TEXTE de l'en-tête (avant le séparateur, qui ajoute son propre espacement) : sert à
  // centrer l'emblème verticalement. Le 1er texte démarre ~5px au-dessus de hdr_p0 (centered()).
  const float hdr_top = hdr_p0.y - 5.0f;
  const float hdr_h   = ImGui::GetCursorScreenPos().y - hdr_top;
  ImGui::Separator();
  // Emblème de guilde dans l'espace libre du coin gauche : CENTRÉ verticalement sur la hauteur
  // du texte, dimensionné pour laisser une marge égale en haut et en bas (draw list après coup
  // -> n'affecte ni le curseur ni le centrage du texte).
  if (has_guild) {
    IconTex em = ResolveEmblem(gi.guildId);
    if (em.tex) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const float esz = std::clamp(hdr_h - 12.0f, 24.0f, 24.0f);  // ~6px de marge haut/bas
      const ImVec2 e0(hdr_p0.x, hdr_top + (hdr_h - esz) * 0.5f);
      const ImVec2 e1(e0.x + esz, e0.y + esz);
      dl->AddRectFilled(e0, e1, IM_COL32(0, 0, 0, 30), 4.0f);          // léger fond
      dl->AddImage(reinterpret_cast<ImTextureID>(em.tex), e0, e1);     // emblème
      dl->AddRect(e0, e1, IM_COL32(90, 90, 110, 220), 4.0f, 0, 1.5f);  // cadre
    }
  }

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

  // Sélecteur de pose + bouton GIF sur la MÊME ligne (la direction = MOLETTE). Les poses
  // « (animé) » jouent l'animation (Marche/Combat) ; les autres sont figées à l'image 0.
  // Combo ET bouton s'ajustent à la largeur de leur texte.
  const float sel_y = content_bottom + 8.0f;
  const float fh = ImGui::GetFrameHeightWithSpacing();
  float combo_w = 0.0f;  // largeur combo = plus large libellé + flèche + padding
  for (int i = 0; i < kPoseCount; ++i)
    combo_w = std::max(combo_w, ImGui::CalcTextSize(kPoses[i].label).x);
  // Largeurs/positions ARRONDIES au pixel entier : des bordures RO sub-pixel
  // provoquaient un léger glitch visuel à gauche du bouton.
  combo_w = static_cast<float>(static_cast<int>(
      combo_w + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f + 6.0f));
  const float gif_w = static_cast<float>(static_cast<int>(
      ImGui::CalcTextSize("Générer le GIF").x + ImGui::GetStyle().FramePadding.x * 2.0f + 12.0f));
  const float line_gap = 8.0f;  // écart net combo / bouton
  const float line_w = combo_w + line_gap + gif_w;
  const float line_x = static_cast<float>(static_cast<int>(
      start_x + std::max(0.0f, (avail_w - line_w) * 0.5f)));
  ImGui::SetCursorPos(ImVec2(line_x, sel_y));
  ImGui::SetNextItemWidth(combo_w);
  if (ro::RoBeginCombo("##cs_pose", PoseLabelFull(avatar_anim_, avatar_animate_))) {
    for (int i = 0; i < kPoseCount; ++i) {
      const bool sel =
          (avatar_anim_ == kPoses[i].anim && avatar_animate_ == kPoses[i].animate);
      if (ImGui::Selectable(kPoses[i].label, sel)) {
        avatar_anim_    = kPoses[i].anim;
        avatar_animate_ = kPoses[i].animate;
        if (avatar_anim_ == kAnimCombat) avatar_dir_ &= ~1;  // snap dir cardinale
      }
    }
    ro::RoEndCombo();
  }
  // Bouton « Générer le GIF » sur la même ligne, largeur ajustée à son texte. Ouvre un
  // dialogue « Enregistrer sous » (thread séparé, non bloquant) ; l'export se fait ici.
  ImGui::SameLine(0.0f, line_gap);
  const bool gif_busy = gif_dialog_busy_.load();
  if (gif_busy) ImGui::BeginDisabled();
  if (ro::RoButton("Générer le GIF", gif_w)) RequestGifSave();
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
    ImGui::SetCursorPos(ImVec2(ox, sel_y + fh));
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
  const float cost_w = 30.0f;  // largeur réservée au coût, à droite du +
  const float max_w = ImGui::CalcTextSize("Max").x + ImGui::GetStyle().FramePadding.x * 2.0f + 4.0f;
  // Petit fond arrondi gris léger derrière le NOM de chaque stat (limité au libellé).
  const ImU32 kRowBg = IM_COL32(165, 170, 180, 55);  // gris léger
  for (int i = 0; i < 6; ++i) {
    const ImVec2 rp = ImGui::GetCursorScreenPos();  // haut de la rangée
    const float nw = ImGui::CalcTextSize(kStatName[i]).x;  // largeur du nom
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(rp.x - 3.0f, rp.y), ImVec2(rp.x + nw + 5.0f, rp.y + step), kRowBg, 4.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kBlack, "%s", kStatName[i]);             // label
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kStatDesc[i]);  // rôle de la stat
    ImGui::SameLine();
    ImGui::SetCursorPosX(val_x);                                // colonne valeurs
    if (s.bonus[i] != 0)
      ImGui::TextColored(kBlack, "— %d (+%d)", s.base[i], s.bonus[i]);
    else
      ImGui::TextColored(kBlack, "— %d", s.base[i]);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kStatDesc[i]);
    // Boutons de montée (actifs SSI on peut se payer >=1 point). « Max » ajoute le
    // MAXIMUM possible ; « + » = +1, ou MAJ+clic = jusqu'au prochain palier de 10 (qui
    // donne un bonus de stat). Le serveur (pc_statusup) clampe le montant envoyé.
    const bool can = (s.raise[i] > 0 && s.raise[i] <= s.points);
    // Bouton « Max » à GAUCHE du +.
    ImGui::SameLine();
    if (right > step + cost_w + max_w)
      ImGui::SetCursorPosX(right - step - cost_w - max_w - 4.0f);
    ImGui::PushID(200 + i);
    if (!can) ImGui::BeginDisabled();
    // 2×255 = 510 pts en un clic : le serveur traite les paquets EN ORDRE, donc les
    // montées s'empilent (couvre le plafond 360 ; il clampe au cap + aux points dispo).
    if (ro::RoButton("Max", max_w, step))
      for (int k = 0; k < 2; ++k) SendStatUp(kStatType[i], 255);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Ajouter le MAXIMUM de points possible dans %s", kStatName[i]);
    if (!can) ImGui::EndDisabled();
    ImGui::PopID();
    // Bouton « + » (clic = +1 ; Maj+clic = palier de 10).
    ImGui::SameLine();
    if (right > step + cost_w) ImGui::SetCursorPosX(right - step - cost_w);
    ImGui::PushID(100 + i);
    if (!can) ImGui::BeginDisabled();
    if (ro::RoButton("+", step, step)) {
      // Palier = prochain multiple de 10 du TOTAL (base + bonus d'items) : c'est le total
      // qui déclenche le bonus. Ex. base 2 +7 = 9 -> il ne manque qu'1 pt (total 10), pas 8.
      const int total = s.base[i] + s.bonus[i];
      const int to_step = 10 - (total % 10);  // points jusqu'au prochain multiple de 10
      SendStatUp(kStatType[i], ImGui::GetIO().KeyShift ? to_step : 1);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip(
          "Monter %s (coût : %d point%s)\nMaj+clic : jusqu'au prochain palier de 10",
          kStatName[i], s.raise[i], s.raise[i] > 1 ? "s" : "");
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
    const ImVec2 rp = ImGui::GetCursorScreenPos();
    const float nw = ImGui::CalcTextSize(label).x;  // fond limité au libellé
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(rp.x - 3.0f, rp.y - 1.0f),
        ImVec2(rp.x + nw + 5.0f, rp.y + ImGui::GetTextLineHeight() + 1.0f), kRowBg, 4.0f);
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
  std::snprintf(b, sizeof(b), "%d,%d%%", s.crit / 10, s.crit % 10);  // x10 -> virgule, précision .1
  stat("CRI", b, "Taux de coup critique (%) : un critique ignore la DEF et ne rate jamais.");
  std::snprintf(b, sizeof(b), "%d", (2000 - s.aspd_raw) / 10);
  stat("ASPD", b, "Vitesse d'attaque : plus elle est haute, plus vous frappez souvent.");
  std::snprintf(b, sizeof(b), "%d,%d%%", s.pdodge / 10, s.pdodge % 10);  // x10 -> virgule, précision .1
  stat("Esq.P", b, "Esquive parfaite (%, via LUK) : évite totalement une attaque, même critique.");
}

void CharacterSheet::OnRenderUI() {
  if (!imgui_enabled_) return;

  // Hotkey Alt+F : bascule la fenetre (ImGui recoit l'input clavier du client, donc
  // ne fire que quand le jeu a le focus). VERIFIER live que Alt+F est libre.
  if (ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F, false))
    show_ = !show_;

  // Raccourcis de presets : actifs EN JEU même fenêtre fermée (swap rapide sans ouvrir).
  const bool in_game = ReadInt(kBaseLvl) > 0;
  if (in_game) ProcessPresetHotkeys();

  if (!show_) return;
  // Rendu de la fenêtre seulement en jeu (evite d'afficher des stats a zero au login).
  if (!in_game) return;

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

  // Onglets Equipement / Costume / Presets.
  if (ImGui::BeginTabBar("cs_tabs")) {
    if (ImGui::BeginTabItem("Équipement")) { tab_ = 0; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Costume"))    { tab_ = 1; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Presets"))    { tab_ = 2; ImGui::EndTabItem(); }
    ImGui::EndTabBar();
  }
  costume_ = (tab_ == 1);

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  chrome_w_ = ImGui::GetWindowWidth() - avail.x;  // mesure pour la contrainte suivante
  if (tab_ == 2) {
    // Onglet Presets : pleine largeur (pas de doll/stats), liste avec icônes des items.
    ImGui::BeginChild("cs_presets", ImVec2(0, 0), true);
    DrawPresetsTab();
    ImGui::EndChild();
  } else {
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
