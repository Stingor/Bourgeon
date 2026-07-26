#include "plugins/character_sheet.h"
#include "ui/game_texture.h"

// Icônes d'item : ro::ItemIcon (ui/icon_cache.h) — cache partagé. Les caches
// de skill et d'emblème restent locaux : chemins et clés différents.
#include "ui/icon_cache.h"
#include "ragnarok/uiwnd.h"
#include "utils/game_paths.h"
#include <Windows.h>
#include <commdlg.h>  // GetSaveFileNameA (dialogue « Enregistrer sous »)
#include <objbase.h>  // CoInitializeEx pour le thread du dialogue

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "bourgeon.h"        // Bourgeon::Instance().SendPacket / session
#include "plugins/bourgeon_opcodes.h"  // kStatBonus (ZC 0x0F10)
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

//  Munition : PAS un slot du tableau equip -> invIndex dans un global dédié (cf. RE 2026-07-12).
//  On lit l'item par cet invIndex dans la liste inventaire (in-place, donne la quantité).
constexpr uintptr_t kAmmoInvIndex = 0x015fba8c;  // g_AmmoEquippedInvIndex (0 = aucune)
constexpr int keAmount = 0x10;   // quantité (item d'inventaire) ; == present pour un equip

//  Compagnons : CZ_BOURGEON_COMPANION (bopcodes::kCompanion 0x0F15) {kind, action, arg}.
//  Miroir des enums serveur e_bourgeon_companion_kind / _action.
enum { kCompCart = 0, kCompPeco = 1, kCompFalcon = 2 };
enum { kCompOff = 0, kCompOn = 1, kCompDeco = 2 };

// Toggles de config natifs (fenêtre équip case 0xd5) via le dispatcher CMode *(0x0121333c)->vf+0x18.
// RE live 2026-07-11 : cmd 0xFD = « Show Equip » (config 0), cmd 0x148 = « View Costumes » (config 5).
constexpr uintptr_t kUICmdDisp       = 0x0121333c;  // *ptr = dispatcher CMode (en jeu)
constexpr int       kVfDispCmd       = 0x18;
constexpr int       kCmdShowEquip    = 0xFD;   // config 0 : montrer l'équip aux autres
constexpr int       kCmdViewCostume  = 0x148;  // config 5 : voir les costumes
constexpr uintptr_t kShowEquipFlag   = 0x015ffd14;  // 1 = équip visible des autres (validé live)
constexpr uintptr_t kCostumeHideFlag = 0x016024c0;  // 0 = costumes affichés (validé live)
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
// Icône de SKILL (case compagnon) : le .bmp est nommé par l'identifiant Lua du skill
// (ex. "MC_PUSHCART"), pas par l'id numérique. Lua_GetSkillIdName(id) -> idname, puis
// "유저인터페이스\item\<idname>.bmp" (source native, indép. de l'appris ; cf. skill_bar_tweaks).
constexpr uintptr_t kGetSkillIdNameLua = 0x0073a140;  // char* GetSkillIdName(int) __cdecl
using GetSkillIdNameLua_t = char*(__cdecl*)(int);
const char kUIDir[] = "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA";  // CP949 유저인터페이스

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

//  Munition équipée : lue par son invIndex (global g_AmmoEquippedInvIndex), retrouvée
//  dans la liste inventaire (node next@+0, ItemSkillInfo@+8, mêmes offsets). La munition
//  reste un item d'inventaire (consommé au tir) -> présente dans la liste, quantité @+0x10.
struct AmmoItem {
  bool     present = false;
  uint32_t nameid = 0;
  int      invIndex = 0;
  int      amount = 0;
  int      viewId = 0;
  int      location = 0;
};
bool ReadEquippedAmmo(AmmoItem* out) {
  __try {
    const int ammoIdx = *reinterpret_cast<const int*>(kAmmoInvIndex);
    if (ammoIdx == 0) return false;  // aucune munition équipée
    void* sentinel = *reinterpret_cast<void* const*>(kInvListHead);
    const int count = *reinterpret_cast<const int*>(kInvCount);
    if (!sentinel || count <= 0) return false;
    void* node = *reinterpret_cast<void* const*>(sentinel);  // sentinelle->next = 1er noeud
    for (int i = 0; i < count && node && node != sentinel; ++i) {
      const uint8_t* info = reinterpret_cast<const uint8_t*>(node) + 8;
      if (*reinterpret_cast<const int*>(info + keInvIndex) == ammoIdx) {
        out->present  = true;
        out->invIndex = ammoIdx;
        out->amount   = *reinterpret_cast<const int*>(info + keAmount);
        out->location = *reinterpret_cast<const int*>(info + keLocation);
        out->viewId   = *reinterpret_cast<const int*>(info + keView);
        const uint32_t cap = *reinterpret_cast<const uint32_t*>(info + keResCap);
        const char* rn = (cap > 15) ? *reinterpret_cast<const char* const*>(info + keResname)
                                    : reinterpret_cast<const char*>(info + keResname);
        out->nameid = (rn && rn[0]) ? static_cast<uint32_t>(std::atoi(rn)) : 0;
        return out->nameid != 0;
      }
      node = *reinterpret_cast<void* const*>(node);  // node->next
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return false;
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
bool ReadEquipLite(int slot, InvItemLite* out, bool costume = false) {
  __try {
    const uint8_t* e = reinterpret_cast<const uint8_t*>(
        kSession + (costume ? kCostumeBase : kEquipBase) + slot * kSlotStride);
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


// ── Icône de skill (case compagnon) ──────────────────────────────────────────
std::unordered_map<uint32_t, ro::IconTex> g_skill_icon_cache;
// Le .bmp d'icône de skill est nommé par l'idname Lua (rejet des sentinelles
// "Unknown"/"Zero Skill" qui spamment la console de chargement).
bool BuildSkillIconPathSafe(int skillId, char* out, int n) {
  out[0] = '\0';
  __try {
    const char* idn = reinterpret_cast<GetSkillIdNameLua_t>(kGetSkillIdNameLua)(skillId);
    if (!idn || !idn[0]) return false;
    if (std::strstr(idn, "nknown") || std::strcmp(idn, "Zero Skill") == 0) return false;
    std::snprintf(out, n, "%s\\item\\%s.bmp", kUIDir, idn);
    return out[0] != '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; return false; }
}
ro::IconTex LoadSkillIcon(int skillId) {
  char path[192];
  if (!BuildSkillIconPathSafe(skillId, path, sizeof(path))) return {};
  return ro::TextureFromGameFile(path);
}
ro::IconTex ResolveSkillIcon(int skillId) {
  if (skillId <= 0) return {};
  static unsigned s_epoch = 0;
  const unsigned e = Overlay_DeviceEpoch();
  if (e != s_epoch) { g_skill_icon_cache.clear(); s_epoch = e; }
  const uint32_t k = static_cast<uint32_t>(skillId);
  auto it = g_skill_icon_cache.find(k);
  if (it != g_skill_icon_cache.end()) return it->second;
  return g_skill_icon_cache[k] = LoadSkillIcon(skillId);
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
ro::IconTex LoadEmblemFromFile(const char* fullPath) {
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
ro::IconTex ResolveEmblem(int guildId) {
  static unsigned s_epoch = 0;
  struct Entry { ro::IconTex tex; DWORD lastTry = 0; };
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
  // chemin complet : <dossier du jeu>\_tmpEmblem\<nom>.ebm
  const std::string full = paths::InGameDir("_tmpEmblem\\") + rel;
  en.tex = LoadEmblemFromFile(full.c_str());
  return en.tex;
}

// Ouvre la fenetre de description native (id 0xc) pour l'item `id` a (mx,my). `src` = l'item
// SOURCE (ItemSkillInfo du slot equip) : ses cartes/refine/grade/options aleatoires sont
// COPIEES dans l'info, sinon la description montre l'item de BASE (sans cartes/enchants).
void OpenItemDesc(uint32_t id, uint16_t view, uint32_t location, int mx, int my,
                  const void* src = nullptr) {
  if (id == 0) return;
  __try {
    uint8_t info[0x100];
    std::memset(info, 0, sizeof(info));
    reinterpret_cast<InfoCtor_t>(kInfoCtor)(info);
    reinterpret_cast<InfoSetId_t>(kInfoSetId)(info, static_cast<int>(id));
    if (src) {
      // Copie TOUS les champs non-string du vrai item : le name-builder natif
      // (ItemSkillInfo_BuildDisplayName 0x008a0570) décore le nom (préfixe/suffixe) à partir de
      // type@0, cartes@0x1c-0x28, refine@0x60, grade@0x88, forge/options + IsDecoratedType. Sans
      // ces champs (surtout le TYPE) il rend le nom NU. On saute les 2 std::string @0x2c (id) /
      // @0x44 (resname) que InfoSetId construit -> pas de partage/corruption de heap.
      const uint8_t* s = reinterpret_cast<const uint8_t*>(src);
      std::memcpy(info + 0x00, s + 0x00, 0x2c);         // type..cartes (avant l'id-string @0x2c)
      std::memcpy(info + 0x5c, s + 0x5c, 0xf8 - 0x5c);  // identified/refine/view/grade/options...
    } else {
      *reinterpret_cast<uint32_t*>(info + 0x8)  = location;  // equip point
      *reinterpret_cast<uint32_t*>(info + 0x70) = view;      // viewID
    }
    info[0x5c] = 1;                                                  // identifie
    void* cache = *reinterpret_cast<void**>(kEnsureCache);
    if (cache)
      reinterpret_cast<EnsureLoaded_t>(kEnsureLoaded)(cache, static_cast<int>(id));
    void* dwnd = reinterpret_cast<MakeWindow_t>(kMakeWindow)(
        uiwnd::Mgr(), nullptr,
        reinterpret_cast<void*>(kWinItemDesc));
    if (dwnd) {
      Vf<OnMsg_t>(dwnd, kVfOnMsg)(dwnd, nullptr, 0, kMsgSetItem,
                                  static_cast<int>(reinterpret_cast<uintptr_t>(info)),
                                  0, 0, 0);
      Vf<SetPos_t>(dwnd, kVfSetPos)(dwnd, nullptr, mx, my);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Nom d'affichage COMPLET (refine + [slots] + préfixes/suffixes de cartes/enchant/forge) via le
// name-builder natif BuildDisplayName, SEH ISOLÉ (repli GetBaseName). `info` = ItemSkillInfo
// source (slot equip). ItemName() ne rend que le nom de BASE ; ceci décore comme la description.
constexpr uintptr_t kBuildName    = 0x008a0570;  // ItemSkillInfo_BuildDisplayName
constexpr uintptr_t kGetBaseName  = 0x006a2b50;  // repli nom de base
constexpr uintptr_t kGameFree     = 0x00dbbc7f;  // libère le std::vector<int> alloué par le jeu
constexpr uintptr_t kInvWndGlobal = 0x0131f6bc;  // *ptr = fenêtre inventaire native (contexte)
struct GVec { int* first; int* last; int* end; };  // std::vector MSVC (jeu)
using BuildName_t   = int(__thiscall*)(void*, void*, int*, GVec*, char**, size_t*, char**, char,
                                       char);
using GetBaseName_t = size_t(__thiscall*)(void*, char*, size_t*, char);
using GameFree_t    = void(__cdecl*)(void*);
void DecoratedItemName(const void* info, char* out, size_t outsz) {
  if (outsz == 0) return;
  out[0] = '\0';
  __try {
    void* wnd = *reinterpret_cast<void**>(kInvWndGlobal);  // contexte (repli créateur ; peut être null)
    char nbuf[128];
    nbuf[0] = '\0';
    char* bufptr = nbuf;
    size_t ncap = sizeof(nbuf);
    int colorOut = 0;
    char* hlptr = nullptr;
    GVec off = {nullptr, nullptr, nullptr};
    reinterpret_cast<BuildName_t>(kBuildName)(wnd, const_cast<void*>(info), &colorOut, &off,
                                              &bufptr, &ncap, &hlptr, 0, 0);
    size_t k = 0;
    while (k + 1 < outsz && nbuf[k]) { out[k] = nbuf[k]; ++k; }
    out[k] = '\0';
    if (off.first) reinterpret_cast<GameFree_t>(kGameFree)(off.first);
    if (out[0] == '\0') {  // repli : nom de base
      size_t cap = outsz;
      reinterpret_cast<GetBaseName_t>(kGetBaseName)(const_cast<void*>(info), out, &cap, 0);
      out[outsz - 1] = '\0';
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
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
// Bascule un toggle de config natif (Show Equip / View Costumes) via le dispatcher, EXACTEMENT
// comme la fenêtre équip (case 0xd5). `value` = nouvel état de la case ; le serveur répond
// ZC_CONFIG qui applique le flag + rafraîchit le sprite. SEH (appel natif via vtable).
using DispCmd_t = void*(__thiscall*)(void*, int, int, int, int, int);
void SendConfigToggle(int cmd, int value) {
  __try {
    void* d = *reinterpret_cast<void**>(kUICmdDisp);
    if (d) Vf<DispCmd_t>(d, kVfDispCmd)(d, cmd, value, 0, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// amount = nb de points à monter en UN paquet (le serveur pc_statusup clampe au coût
// abordable + au plafond de la stat). L'octet amount est lu NON signé (RFIFOB) -> [1,255].
void SendStatUp(int statType, int amount = 1) {
  if (amount < 1) return;
  if (amount > 255) amount = 255;
  uint8_t pkt[5];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpStatUp;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(statType); //
  pkt[4] = static_cast<uint8_t>(amount);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}
// Invoquer/basculer un compagnon (chariot/peco/faucon) : CZ_BOURGEON_COMPANION (0x0F15),
// paquet FIXE 7 o {op:2, len:2, kind:1, action:1, arg:1}. Le serveur re-valide le skill.
void SendCompanionPkt(int kind, int action, int arg) {
  uint8_t pkt[7];
  *reinterpret_cast<uint16_t*>(pkt + 0) = bopcodes::kCompanion;
  *reinterpret_cast<uint16_t*>(pkt + 2) = 7;
  pkt[4] = static_cast<uint8_t>(kind);
  pkt[5] = static_cast<uint8_t>(action);
  pkt[6] = static_cast<uint8_t>(arg);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// ── Titres d'achievement (cf. project_achievement_title_re, docs/achievement_title_re.md) ────
// Un titre = un simple entier (id 1000..1046). Le libellé est 100% client (Lua TitleTable.lub).
constexpr uintptr_t kOwnTitleId    = 0x016004fc;  // g_Own_TitleId : titre ÉQUIPÉ (0 = aucun)
constexpr uintptr_t kOwnTitleBegin = 0x01600500;  // std::vector<int> g_OwnTitleList : begin (possédés)
constexpr uintptr_t kOwnTitleEnd   = 0x01600504;  // .. end
// Title_GetStringById : __thiscall(this=session 0x015fa3c0, out_str, titleId) -> std::string* (out).
// Résout l'id en libellé via l'appel Lua global GetTitleString. RET 0x8 (thiscall, 2 args pile).
constexpr uintptr_t kTitleGetStr   = 0x00d89ed0;
constexpr uintptr_t kStrDtor       = 0x004f08f0;  // std::string dtor (__thiscall, libère le heap SSO+)
constexpr uint16_t  kOpChangeTitle = 0x0A2E;      // CZ_REQ_CHANGE_TITLE {op, title_id.L} (équiper)
using TitleGetStr_t = void*(__fastcall*)(void* thisSession, void* edx, void* out, int titleId);
using StrDtor_t     = void(__fastcall*)(void* thisStr, void* edx);

// Titres possédés + titre équipé, lus LIVE des globals (SEH/POD).
struct OwnedTitles {
  int equipped = 0;     // g_Own_TitleId (0 = aucun)
  int ids[128];         // titres possédés (dérivés des achievements complétés côté client)
  int count = 0;
};
bool ReadOwnedTitles(OwnedTitles* o) {
  __try {
    o->equipped = *reinterpret_cast<const int*>(kOwnTitleId);
    const int* b = *reinterpret_cast<const int* const*>(kOwnTitleBegin);
    const int* e = *reinterpret_cast<const int* const*>(kOwnTitleEnd);
    o->count = 0;
    if (b && e && e > b) {
      int n = static_cast<int>(e - b);
      if (n > 128) n = 128;
      for (int i = 0; i < n; ++i) o->ids[i] = b[i];
      o->count = n;
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Cache id -> libellé (statique : TitleTable.lub ne change pas). Résolu via le natif Lua.
std::unordered_map<int, std::string> g_title_cache;
void ResolveTitleSEH(int id, char* out, size_t cap) {
  out[0] = '\0';
  __try {
    // std::string MSVC : 16o buffer SSO @+0, size @+0x10, capacité @+0x14. Le natif écrase
    // tout le buffer (init incluse) ; on lit puis on DÉTRUIT (libère si la chaîne dépasse la SSO).
    uint8_t sbuf[0x18];
    std::memset(sbuf, 0, sizeof(sbuf));
    reinterpret_cast<TitleGetStr_t>(kTitleGetStr)(
        reinterpret_cast<void*>(kSession), nullptr, sbuf, id);
    const uint32_t scap = *reinterpret_cast<const uint32_t*>(sbuf + 0x14);
    const char* p = (scap > 15) ? *reinterpret_cast<char* const*>(sbuf)
                                : reinterpret_cast<const char*>(sbuf);
    if (p) { size_t i = 0; for (; i + 1 < cap && p[i]; ++i) out[i] = p[i]; out[i] = '\0'; }
    reinterpret_cast<StrDtor_t>(kStrDtor)(sbuf, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}
const char* TitleName(int id) {
  auto it = g_title_cache.find(id);
  if (it != g_title_cache.end()) return it->second.c_str();
  char buf[96];
  ResolveTitleSEH(id, buf, sizeof(buf));
  if (buf[0] == '\0') std::snprintf(buf, sizeof(buf), "Titre #%d", id);
  return (g_title_cache[id] = buf).c_str();
}
// Équipe un titre : CZ_REQ_CHANGE_TITLE (0x0A2E) {title_id.L}. title_id=0 => retirer le titre.
// Le serveur re-valide (refuse si ∉ sd->titles) et répond ZC 0x0A2F -> maj g_Own_TitleId.
void SendChangeTitle(int titleId) {
  uint8_t pkt[6];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpChangeTitle;
  *reinterpret_cast<uint32_t*>(pkt + 2) = static_cast<uint32_t>(titleId);
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

// Fonds réglables via le skin RO (ro::SkinConfig, persistés par MoonlightUi) : couleur des
// cases d'équipement (slot_col) et du panneau doll/avatar (doll_col). Lues à chaque frame ->
// changement de skin/preset appliqué à chaud, sans état local.
ImU32 SlotBgCol() {
  const float* c = ro::SkinConfig().slot_col;
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3]));
}
ImU32 DollBgCol() {
  const float* c = ro::SkinConfig().doll_col;
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3]));
}

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
  dl->AddRectFilled(p0, p1, SlotBgCol(), 3.0f);
  dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 80), 3.0f);
  ro::IconTex ic = ro::ItemIcon(pi.nameid);
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

CharacterSheet::CharacterSheet() {
  // Apport équip/cartes poussé par le serveur à chaque status_calc_pc. Opcode zone
  // custom sûre (>0x0C35) => livré par le reader-hook. cf. bourgeon_opcodes.h.
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kStatBonus);
  // État des compagnons (chariot/peco/faucon) poussé par le serveur au login + à chaque
  // changement (pc_setcart/riding/falcon). Gate/affiche les cases sans RE côté client.
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kCompanionState);
}

// Payload de ZC_BOURGEON_STAT_BONUS APRÈS le header [type:2][len:2] (le reader-hook
// nous passe data = octets après le header, len = payload seul). Miroir exact de
// PACKET_ZC_BOURGEON_STAT_BONUS côté moonlight.
#pragma pack(push, 1)
struct StatBonusPayload {   // bloc FIXE (miroir de PACKET_ZC_BOURGEON_STAT_BONUS sans le header)
  int16_t param_equip[6];  // apport ÉQUIPEMENT (STR..LUK)
  int16_t param_bonus[6];  // apport CARTES
  int32_t eatk;            // ATK issu de l'équip
  int32_t ematk;           // MATK issu de l'équip
  int32_t melee_pct;       // % dégât mêlée non-armé
  int32_t ranged_pct;      // % dégât à distance
  int32_t crit_dmg_pct;    // % dégât critique
  int32_t hp_add;          // PV max ajoutés par l'équip
  int32_t sp_add;          // SP max ajoutés par l'équip
  int32_t aspd_add;        // ASPD plate
  int32_t vcast_pct;       // cast variable n/100 (<0 = réduction)
  int32_t fcast_pct;       // cast fixe (<0 = réduction)
  // Lot A — offensif
  int32_t atk_pct, matk_pct;
  int32_t dmg_ret_melee, dmg_ret_ranged, dmg_ret_magic;
  int32_t double_pct, perfect_hit;
  // Lot B — survie
  int32_t hp_pct, sp_pct, hp_regen_pct, sp_regen_pct;
  int32_t crit_def_pct, hp_on_kill, sp_on_kill, unbreak_pct;
  // Lot C — utilitaire
  int32_t pot_hp_pct, pot_sp_pct, heal_up_pct, delay_pct;
  int32_t add_vcast_ms, add_fcast_ms, steal_pct;
  // Lot E — réduction par type d'attaque + splash
  int32_t def_melee_pct, def_ranged_pct, def_magic_pct, def_misc_pct;
  int32_t splash, splash_add;
  // Lot F — vol de vie
  int32_t hp_drain_pct, sp_drain_pct;
  // Lot G — très niche
  int32_t break_weapon_pct, break_armor_pct, zeny_bonus_pct, classchange_pct;
  int32_t dmg_ret_reduce, magic_hp_gain, magic_sp_gain;
  // Part du raffinage dans l'ATK / la DEF
  int32_t refine_atk, refine_def;
};
struct CondWire {          // miroir de PACKET_BOURGEON_STAT_COND
  uint16_t code;
  int16_t  idx;
  int32_t  value;
};
struct SkillWire {         // miroir de PACKET_BOURGEON_STAT_SKILL
  uint16_t code;
  uint16_t skill_id;
  int16_t  lv;
  int32_t  value;
  uint16_t aux;
};
struct ItemWire {          // miroir de PACKET_BOURGEON_STAT_ITEM
  uint16_t code;
  uint32_t nameid;
  int32_t  rate;
};
#pragma pack(pop)

// Résolveur de nom de skill localisé (wrapper Lua natif, cf. skill_bar_tweaks) :
// char* GetSkillName(int id) — renvoie « Unknown-Skill » si l'id est inconnu.
constexpr uintptr_t kGetSkillNameLua = 0x0073a1f0;
using GetSkillNameLua_t = char* (__cdecl*)(int);

// Résolution du nom de STATUT (EFST) via le global Lua GetStateIconDescript(efst),
// appelé par l'API C Lua 5.1 BRUTE (nom statique => pas de std::string BYVAL détruit
// par le wrapper varargs, cf. item_desc_tweaks ResolveOptName). RE : le tooltip natif
// FUN_00c93cb0 utilise ce même global via Lua_CallGlobal_va.
constexpr uintptr_t kLuaState    = 0x015ffd78;  // *=M ; **=vrai lua_State
constexpr uintptr_t kLuaGetField = 0x00519df0;  // lua_getfield(L,idx,k)
constexpr uintptr_t kLuaPushNum  = 0x0051a4b0;  // lua_pushnumber(L,double)
constexpr uintptr_t kLuaPCall    = 0x0051a290;  // lua_pcall(L,nargs,nres,errf)->int(0=ok)
constexpr uintptr_t kLuaToLStr   = 0x0051aca0;  // lua_tolstring(L,idx,&len)->const char*
constexpr uintptr_t kLuaSetTop   = 0x0051aab0;  // lua_settop(L,idx)
constexpr int       kLuaGlobals  = -10002;      // LUA_GLOBALSINDEX (5.1)
using LuaGetField_t = void        (__cdecl*)(void*, int, const char*);
using LuaPushNum_t  = void        (__cdecl*)(void*, double);
using LuaPCall_t    = int         (__cdecl*)(void*, int, int, int);
using LuaToLStr_t   = const char* (__cdecl*)(void*, int, size_t*);
using LuaSetTop_t   = void        (__cdecl*)(void*, int);

// GetStateIconDescript(efst) renvoie la desc du statut (multi-ligne, markup ^RRGGBB) ;
// on garde la 1re ligne nettoyée = le nom. SEH-gardé, caché. Repli « Statut #id ».
std::unordered_map<uint16_t, std::string> g_status_name_cache;
const char* StatusName(uint16_t efst) {
  auto it = g_status_name_cache.find(efst);
  if (it != g_status_name_cache.end()) return it->second.c_str();
  char raw[256] = {0};
  __try {
    void* M = *reinterpret_cast<void**>(kLuaState);
    void* L = M ? *reinterpret_cast<void**>(M) : nullptr;  // **(0x015ffd78)
    if (L) {
      reinterpret_cast<LuaGetField_t>(kLuaGetField)(L, kLuaGlobals, "GetStateIconDescript");
      reinterpret_cast<LuaPushNum_t>(kLuaPushNum)(L, static_cast<double>(efst));
      if (reinterpret_cast<LuaPCall_t>(kLuaPCall)(L, 1, 1, 0) == 0) {
        const char* s = reinterpret_cast<LuaToLStr_t>(kLuaToLStr)(L, -1, nullptr);
        if (s && s[0]) std::strncpy(raw, s, sizeof(raw) - 1);
      }
      reinterpret_cast<LuaSetTop_t>(kLuaSetTop)(L, -2);  // pop résultat/erreur
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { raw[0] = '\0'; }
  // 1re ligne, codes couleur ^RRGGBB retirés.
  char clean[128];
  int o = 0;
  for (int i = 0; raw[i] && raw[i] != '\n' && raw[i] != '\r' && o < (int)sizeof(clean) - 1;) {
    if (raw[i] == '^' && i + 6 < 255) {
      bool hex6 = true;
      for (int k = 1; k <= 6; ++k) {
        const char c = raw[i + k];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
          hex6 = false; break;
        }
      }
      if (hex6) { i += 7; continue; }
    }
    clean[o++] = raw[i++];
  }
  clean[o] = '\0';
  while (o > 0 && clean[o - 1] == ' ') clean[--o] = '\0';  // trim fin
  if (!clean[0]) std::snprintf(clean, sizeof(clean), "Statut #%u", efst);
  return (g_status_name_cache[efst] = clean).c_str();
}

// Codes des bonus conditionnels — MIROIR de e_bourgeon_stat_cond (moonlight
// packets_struct.hpp). Toute évolution ici DOIT être coordonnée avec le serveur.
enum : uint16_t {
  kBscSubEle = 1, kBscSubRace = 2, kBscSubSize = 3,
  kBscAddEle = 4, kBscAddRace = 5, kBscAddSize = 6,
  kBscMAddEle = 7, kBscMAddRace = 8, kBscMAddSize = 9,
  kBscCritRace = 10, kBscIgnDefRace = 11, kBscIgnMdefRace = 12, kBscSubdefEle = 13,
  kBscSubClass = 14, kBscSubRace2 = 15,
  kBscExpRace = 16, kBscExpClass = 17, kBscDropRace = 18, kBscDropClass = 19,
  kBscDefsetRace = 20, kBscMdefsetRace = 21, kBscHpVanishRace = 22, kBscSpVanishRace = 23,
  kBscComaRace = 24, kBscComaClass = 25, kBscIgnResRace = 26, kBscIgnMresRace = 27,
  kBscMAddRace2 = 28, kBscIgnMdefRace2 = 29, kBscSpGainRace = 30,
};
// Codes des bonus liés à un skill — MIROIR de e_bourgeon_stat_skill (serveur).
enum : uint16_t {
  kBskAutospell = 1, kBskAutospellHit = 2, kBskSkillAtk = 3,
  kBskAddeff = 4, kBskAddeffHit = 5,  // skill_id porte un EFST (résolu via StatusName)
  kBskReseff = 6, kBskSubskill = 7, kBskAutospellSkill = 8,
  kBskSkillSprate = 9, kBskSkillSpcost = 10, kBskSkillVcastrate = 11, kBskSkillFcastrate = 12,
  kBskSkillVcast = 13, kBskSkillFcast = 14, kBskSkillCooldown = 15, kBskSkillDelay = 16,
  kBskSkillHeal = 17, kBskSkillHeal2 = 18, kBskSkillBlown = 19,
};
// Codes des bonus liés à un item — MIROIR de e_bourgeon_stat_item (serveur).
enum : uint16_t {
  kBsiAddDrop = 1, kBsiAddDropGroup = 2,
};

// Noms FR pour libeller les conditionnels (index = ELE_*/RC_*/SZ_* côté serveur).
static const char* const kEleName[] = {
    "Neutral", "Water", "Earth", "Fire", "Wind",
    "Poison", "Holy", "Shadow", "Ghost", "Undead",
    "all elements",  // index 10 = ELE_ALL (résist./dégâts « tous éléments »)
};
// e_race : … Dragon(9), Player(10), Doram(11), RC_ALL(12), RC_MAX=13 (pas de Boss ici !).
static const char* const kRaceName[] = {
    "Formless", "Undead", "Brute", "Plant", "Insect", "Fish",
    "Demon", "Demi-human", "Angel", "Dragon",
    "Player", "Doram Player", "all races",
};
// e_size : Small(0), Medium(1), Large(2), SZ_ALL(3), SZ_MAX=4.
static const char* const kSizeName[] = {"Small", "Medium", "Large", "all sizes"};
// Classe de monstre (e_aegis_monsterclass) : index 3 = trou, 6 = CLASS_ALL.
static const char* const kClassName[] = {
    "Normal", "Boss", "Gardien", "?",
    "Battlefield", "Event", "all classes",
};
// Groupes de monstres RC2 (e_race2) — communs libellés, le reste = « groupe #N ».
static const char* const kRace2Name[] = {
    "", "Goblin", "Kobold", "Orc", "Golem", "Gardian", "Ninja", "GvG",
    "Battlefield", "Treasure", "Biolab", "Manuk", "Splendid", "Scaraba",
};

void CharacterSheet::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  // État des compagnons (ZC 0x0F16) : 8 octets APRÈS le header (le reader-hook nous
  // passe data = post-header, len = payload). Miroir de PACKET_ZC_BOURGEON_COMPANION_STATE.
  if (opcode == bopcodes::kCompanionState) {
#pragma pack(push, 1)
    struct CompStatePayload {
      uint8_t  pushcart_lv, changecart_lv, riding_lv, falcon_lv;
      uint8_t  cart_active, riding_active, falcon_active, cart_deco_max;
      uint16_t pushcart_id, riding_id, falcon_id;  // ids skills pour l'icône
    };
#pragma pack(pop)
    if (len < sizeof(CompStatePayload)) return;
    const auto* p = reinterpret_cast<const CompStatePayload*>(data);
    companion_.valid         = true;
    companion_.pushcart_lv   = p->pushcart_lv;
    companion_.changecart_lv = p->changecart_lv;
    companion_.riding_lv     = p->riding_lv;
    companion_.falcon_lv     = p->falcon_lv;
    companion_.cart_active   = p->cart_active;
    companion_.riding_active = p->riding_active != 0;
    companion_.falcon_active = p->falcon_active != 0;
    companion_.cart_deco_max = p->cart_deco_max > 0 ? p->cart_deco_max : 1;
    companion_.pushcart_id   = p->pushcart_id;
    companion_.riding_id     = p->riding_id;
    companion_.falcon_id     = p->falcon_id;
    if (p->cart_active > 0) last_cart_type_ = p->cart_active;  // pour « rallumer » au même type
    return;
  }
  if (opcode != bopcodes::kStatBonus) return;
  if (len < sizeof(StatBonusPayload)) return;  // bloc fixe tronqué : ignore
  const auto* p = reinterpret_cast<const StatBonusPayload*>(data);
  for (int i = 0; i < 6; ++i) {
    bonus_.equip[i] = p->param_equip[i];
    bonus_.card[i]  = p->param_bonus[i];
  }
  bonus_.eatk         = p->eatk;
  bonus_.ematk        = p->ematk;
  bonus_.melee_pct    = p->melee_pct;
  bonus_.ranged_pct   = p->ranged_pct;
  bonus_.crit_dmg_pct = p->crit_dmg_pct;
  bonus_.hp_add       = p->hp_add;
  bonus_.sp_add       = p->sp_add;
  bonus_.aspd_add     = p->aspd_add;
  bonus_.vcast_pct    = p->vcast_pct;
  bonus_.fcast_pct    = p->fcast_pct;
  bonus_.atk_pct        = p->atk_pct;
  bonus_.matk_pct       = p->matk_pct;
  bonus_.dmg_ret_melee  = p->dmg_ret_melee;
  bonus_.dmg_ret_ranged = p->dmg_ret_ranged;
  bonus_.dmg_ret_magic  = p->dmg_ret_magic;
  bonus_.double_pct     = p->double_pct;
  bonus_.perfect_hit    = p->perfect_hit;
  bonus_.hp_pct         = p->hp_pct;
  bonus_.sp_pct         = p->sp_pct;
  bonus_.hp_regen_pct   = p->hp_regen_pct;
  bonus_.sp_regen_pct   = p->sp_regen_pct;
  bonus_.crit_def_pct   = p->crit_def_pct;
  bonus_.hp_on_kill     = p->hp_on_kill;
  bonus_.sp_on_kill     = p->sp_on_kill;
  bonus_.unbreak_pct    = p->unbreak_pct;
  bonus_.pot_hp_pct     = p->pot_hp_pct;
  bonus_.pot_sp_pct     = p->pot_sp_pct;
  bonus_.heal_up_pct    = p->heal_up_pct;
  bonus_.delay_pct      = p->delay_pct;
  bonus_.add_vcast_ms   = p->add_vcast_ms;
  bonus_.add_fcast_ms   = p->add_fcast_ms;
  bonus_.steal_pct      = p->steal_pct;
  bonus_.def_melee_pct  = p->def_melee_pct;
  bonus_.def_ranged_pct = p->def_ranged_pct;
  bonus_.def_magic_pct  = p->def_magic_pct;
  bonus_.def_misc_pct   = p->def_misc_pct;
  bonus_.splash         = p->splash;
  bonus_.splash_add     = p->splash_add;
  bonus_.hp_drain_pct   = p->hp_drain_pct;
  bonus_.sp_drain_pct   = p->sp_drain_pct;
  bonus_.break_weapon_pct = p->break_weapon_pct;
  bonus_.break_armor_pct  = p->break_armor_pct;
  bonus_.zeny_bonus_pct   = p->zeny_bonus_pct;
  bonus_.classchange_pct  = p->classchange_pct;
  bonus_.dmg_ret_reduce   = p->dmg_ret_reduce;
  bonus_.magic_hp_gain    = p->magic_hp_gain;
  bonus_.magic_sp_gain    = p->magic_sp_gain;
  bonus_.refine_atk       = p->refine_atk;
  bonus_.refine_def       = p->refine_def;

  // Queue variable : [cond_count:2] + CondWire[] puis [skill_count:2] + SkillWire[].
  // Toutes les longueurs bornées par len (paquet potentiellement tronqué/ancien).
  bonus_.cond.clear();
  bonus_.skills.clear();
  size_t off = sizeof(StatBonusPayload);
  if (len >= off + 2) {
    const int16_t n = *reinterpret_cast<const int16_t*>(data + off);
    off += 2;
    for (int i = 0; i < n && off + sizeof(CondWire) <= len; ++i) {
      const auto* e = reinterpret_cast<const CondWire*>(data + off);
      bonus_.cond.push_back({e->code, e->idx, e->value});
      off += sizeof(CondWire);
    }
  }
  if (len >= off + 2) {
    const int16_t n = *reinterpret_cast<const int16_t*>(data + off);
    off += 2;
    for (int i = 0; i < n && off + sizeof(SkillWire) <= len; ++i) {
      const auto* e = reinterpret_cast<const SkillWire*>(data + off);
      bonus_.skills.push_back({e->code, e->skill_id, e->lv, e->value, e->aux});
      off += sizeof(SkillWire);
    }
  }
  bonus_.items.clear();
  if (len >= off + 2) {
    const int16_t n = *reinterpret_cast<const int16_t*>(data + off);
    off += 2;
    for (int i = 0; i < n && off + sizeof(ItemWire) <= len; ++i) {
      const auto* e = reinterpret_cast<const ItemWire*>(data + off);
      bonus_.items.push_back({e->code, e->nameid, e->rate});
      off += sizeof(ItemWire);
    }
  }
  bonus_.valid = true;
}

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

// « Tout nu » : desequipe tous les slots portes, en paquets bruts envoyes d'un coup (meme
// chemin que le desequip de masse d'ApplyPreset -> instantane cote serveur). Couvre l'equip
// normal ET les costumes (tableau session separe kCostumeBase : seuls 3 tetes + cape existent).
int CharacterSheet::UnequipAll(bool with_costumes) {
  int freed = 0;
  auto strip = [&](int slot, bool costume) {
    InvItemLite li{};
    if (!ReadEquipLite(slot, &li, costume)) return;  // slot vide
    SendUnequip(li.index);
    ++freed;
  };
  for (int s = 0; s < kNormalSlots; ++s) strip(s, false);
  if (with_costumes) {
    const int kCostumeSlots[4] = {8, 0, 9, 2};  // tete haut, tete bas, tete mil, cape
    for (int s : kCostumeSlots) strip(s, true);
  }
  preset_status_ = freed > 0 ? "Tout déséquipé (" + std::to_string(freed) + " pièce(s))"
                             : "Rien à déséquiper";
  return freed;
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

  // Action globale : se mettre « tout nu » (independant des presets). Les costumes vivent dans
  // un tableau session distinct -> bouton separe pour tout retirer, costumes compris.
  if (ro::RoButton("Tout déséquiper", bw("Tout déséquiper"))) UnequipAll(false);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Retire l'équipement porté (garde les costumes)");
  ImGui::SameLine(0.0f, 4.0f);
  if (ro::RoButton("+ costumes", bw("+ costumes"))) UnequipAll(true);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Retire aussi les costumes (têtes + cape)");
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

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

// Onglet Titres : liste des titres possédés (dérivés des achievements complétés), le titre
// équipé mis en évidence + coché ; clic = équiper (CZ 0x0A2E), « Aucun titre » = retirer. Un
// champ de filtre permet de retrouver un titre par son libellé quand la liste est longue.
void CharacterSheet::DrawTitlesTab() {
  OwnedTitles ot{};
  ReadOwnedTitles(&ot);

  const ImVec4 kGray(0.35f, 0.35f, 0.42f, 1.0f);
  const ImVec4 kGreen(0.15f, 0.55f, 0.20f, 1.0f);

  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(kBlack, "Titre équipé :");
  ImGui::SameLine();
  if (ot.equipped != 0)
    ImGui::TextColored(kGreen, "%s", TitleName(ot.equipped));
  else
    ImGui::TextColored(kGray, "aucun");

  ImGui::Spacing();
  // Filtre par libellé (pratique quand beaucoup de titres décrochés).
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
  ImGui::InputTextWithHint("##cs_title_filter", "filtrer par nom…", title_filter_buf_,
                           sizeof(title_filter_buf_));
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  int to_equip = -1;  // titre à équiper APRÈS le rendu (0 = retirer, -1 = rien)

  // Entrée « Aucun titre » (retire le titre équipé) : visible seulement sans filtre actif.
  const bool filtering = title_filter_buf_[0] != '\0';
  if (!filtering) {
    const bool sel_none = ot.equipped == 0;
    if (ImGui::Selectable("Aucun titre", sel_none)) to_equip = 0;
    ImGui::Spacing();
  }

  if (ot.count == 0) {
    ImGui::TextColored(kGray,
                       "Aucun titre décroché. Complète des succès qui récompensent un titre.");
  }

  // Comparaison insensible à la casse pour le filtre.
  auto icontains = [](const char* hay, const char* needle) {
    if (!needle[0]) return true;
    for (const char* h = hay; *h; ++h) {
      const char *a = h, *b = needle;
      while (*a && *b && std::tolower((unsigned char)*a) == std::tolower((unsigned char)*b)) {
        ++a; ++b;
      }
      if (!*b) return true;
    }
    return false;
  };

  for (int i = 0; i < ot.count; ++i) {
    const int id = ot.ids[i];
    const char* label = TitleName(id);
    if (filtering && !icontains(label, title_filter_buf_)) continue;
    ImGui::PushID(id);
    const bool equipped = (id == ot.equipped);
    // Ligne sélectionnable pleine largeur : « ✓ » (équipé) puis le libellé.
    char row[128];
    std::snprintf(row, sizeof(row), "%s%s", equipped ? "> " : "   ", label);
    if (ImGui::Selectable(row, equipped)) to_equip = id;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(equipped ? "Titre actuellement équipé (clic : garder)"
                                 : "Clic : équiper ce titre");
    ImGui::PopID();
  }

  if (to_equip >= 0 && to_equip != ot.equipped) SendChangeTitle(to_equip);
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
  dl->AddRectFilled(p0, p1, SlotBgCol(), 4.0f);     // fond de case (réglable skin RO)
  dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 80), 4.0f);  // bordure
  if (has) {
    ro::IconTex ic = ro::ItemIcon(it.nameid);
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
    ro::IconTex ic = ro::ItemIcon(it.nameid);  // aperçu du drag (icône + nom complet)
    if (ic.tex) {
      ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(24, 24));
      ImGui::SameLine();
    }
    const uintptr_t dsrc = kSession + (costume ? kCostumeBase : kEquipBase) +
                           static_cast<uintptr_t>(slot) * kSlotStride;
    char dnm[128];
    DecoratedItemName(reinterpret_cast<const void*>(dsrc), dnm, sizeof(dnm));
    ImGui::TextUnformatted(dnm[0] ? dnm : ItemName(it.nameid));
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
    // Nom COMPLET (refine + [slots] + cartes/enchant/forge) via BuildDisplayName, comme la
    // description ; ItemName seul rendrait le nom NU. Repli sur ItemName si vide.
    const uintptr_t hsrc = kSession + (costume ? kCostumeBase : kEquipBase) +
                           static_cast<uintptr_t>(slot) * kSlotStride;
    char nm[128];
    DecoratedItemName(reinterpret_cast<const void*>(hsrc), nm, sizeof(nm));
    if (nm[0] == '\0') std::snprintf(nm, sizeof(nm), "%s", ItemName(it.nameid));
    ImGui::SetTooltip("%s\n%s", nm, hint);
    const ImVec2 mp = ImGui::GetMousePos();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      if (ImGui::GetIO().KeyShift) {  // Maj+clic droit = lien de l'item dans le chat
        if (auto* iv = Bourgeon::Instance().inventory_viewer())
          iv->LinkItemToChat(it.invIndex);
      } else {  // clic droit seul = description (avec cartes/enchants/options du slot source)
        const uintptr_t src = kSession + (costume ? kCostumeBase : kEquipBase) +
                              static_cast<uintptr_t>(slot) * kSlotStride;
        OpenItemDesc(it.nameid, static_cast<uint16_t>(it.viewId),
                     static_cast<uint32_t>(it.location), static_cast<int>(mp.x),
                     static_cast<int>(mp.y), reinterpret_cast<const void*>(src));
      }
    }
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
      SendUnequip(it.invIndex);
  }
  ImGui::PopID();
}

// Case MUNITION (à côté du bouclier). La munition n'est pas un slot du tableau equip :
// on la lit par son invIndex global. Interactions comme un slot (drop = équiper via le
// chemin partagé, double-clic = déséquiper, clic droit = description, survol = tooltip).
void CharacterSheet::DrawAmmoSlot(float x, float y, float sz) {
  AmmoItem am{};
  const bool has = ReadEquippedAmmo(&am);

  ImGui::SetCursorPos(ImVec2(x, y));
  ImGui::PushID(1000);  // id unique hors plage des slots (slot*2+costume)
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("ammo", ImVec2(sz, sz));
  const ImVec2 p1(p0.x + sz, p0.y + sz);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, SlotBgCol(), 4.0f);
  dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 80), 4.0f);
  if (has) {
    ro::IconTex ic = ro::ItemIcon(am.nameid);
    if (ic.tex) {
      const float pad = 3.0f;
      dl->AddImage(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(p0.x + pad, p0.y + pad),
                   ImVec2(p1.x - pad, p1.y - pad));
    }
    if (am.amount > 1) {  // quantité restante, bas à droite (comme l'inventaire)
      char q[12];
      std::snprintf(q, sizeof(q), "%d", am.amount);
      const ImVec2 ts = ImGui::CalcTextSize(q);
      const ImVec2 rp(p1.x - ts.x - 2, p1.y - ts.y - 1);
      const ImU32 white = IM_COL32(255, 255, 255, 255);
      for (int oy = -1; oy <= 1; ++oy)
        for (int ox = -1; ox <= 1; ++ox)
          if (ox || oy) dl->AddText(ImVec2(rp.x + ox, rp.y + oy), white, q);
      dl->AddText(rp, IM_COL32(0, 0, 0, 255), q);
    }
  } else {
    const char* ab = "Ammo";
    const ImVec2 ts = ImGui::CalcTextSize(ab);
    dl->AddText(ImVec2(p0.x + (sz - ts.x) * 0.5f, p0.y + (sz - ts.y) * 0.5f),
                IM_COL32(120, 120, 120, 255), ab);
  }

  // CIBLE drop : lâcher une munition (payload "INV_ITEM") = l'équiper (chemin partagé,
  // le serveur route par type d'item -> cmd 0x57). Fonctionne pour flèche/balle/grenade/jet.
  if (ImGui::BeginDragDropTarget()) {
    if (ImGui::AcceptDragDropPayload("INV_ITEM")) {
      if (auto* iv = Bourgeon::Instance().inventory_viewer()) iv->EquipDraggedItem(false);
    }
    ImGui::EndDragDropTarget();
  }

  if (ImGui::IsItemHovered()) {
    ro::SetHoverCursor(2);
    if (has) {
      ImGui::SetTooltip("%s  x%d\n(clic droit : desc, double-clic : déséquiper, glisser ici : équiper)",
                        ItemName(am.nameid), am.amount);
      const ImVec2 mp = ImGui::GetMousePos();
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        OpenItemDesc(am.nameid, static_cast<uint16_t>(am.viewId),
                     static_cast<uint32_t>(am.location), static_cast<int>(mp.x),
                     static_cast<int>(mp.y), nullptr);
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        SendUnequip(am.invIndex);
    } else {
      ImGui::SetTooltip("Munition\n(glissez une flèche/balle/grenade/arme de jet ici pour l'équiper)");
    }
  }
  ImGui::PopID();
}

// Colonne COMPAGNONS (à gauche de l'arme) : chariot/peco/faucon, chacun affiché SEULEMENT
// si son skill est appris (état poussé par le serveur). Renvoie le nombre de cases dessinées.
int CharacterSheet::DrawCompanions(float x, float y0, float sz, float gap) {
  if (!companion_.valid) return 0;
  const int kinds[3] = {kCompCart, kCompPeco, kCompFalcon};
  const int lv[3]    = {companion_.pushcart_lv, companion_.riding_lv, companion_.falcon_lv};
  int drawn = 0;
  for (int i = 0; i < 3; ++i) {
    if (lv[i] <= 0) continue;
    DrawCompanionCase(kinds[i], x, y0 + drawn * (sz + gap), sz);
    ++drawn;
  }
  return drawn;
}

// Une case compagnon : fond vert si actif. Clic gauche = basculer (invoquer/ranger).
// Chariot : clic droit = menu {Ouvrir, Changer la déco (si MC_CHANGECART), Retirer}.
void CharacterSheet::DrawCompanionCase(int kind, float x, float y, float sz) {
  bool active = false;
  const char* label = "";
  const char* name = "";
  int skillId = 0;  // skill dont on affiche l'icône (id envoyé par le serveur)
  switch (kind) {
    case kCompCart:   active = companion_.cart_active > 0; label = "Cart";   name = "Chariot";        skillId = companion_.pushcart_id; break;
    case kCompPeco:   active = companion_.riding_active;   label = "Peco";   name = "Monture (Peco)"; skillId = companion_.riding_id;   break;
    case kCompFalcon: active = companion_.falcon_active;   label = "Falcon"; name = "Faucon";         skillId = companion_.falcon_id;   break;
  }

  ImGui::SetCursorPos(ImVec2(x, y));
  ImGui::PushID(2000 + kind);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("comp", ImVec2(sz, sz));
  const ImVec2 p1(p0.x + sz, p0.y + sz);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 bg = active ? IM_COL32(120, 200, 120, 255) : SlotBgCol();
  dl->AddRectFilled(p0, p1, bg, 4.0f);
  dl->AddRect(p0, p1, active ? IM_COL32(30, 110, 30, 220) : IM_COL32(0, 0, 0, 80), 4.0f, 0,
              active ? 1.5f : 1.0f);
  // Icône du skill (chariot/peco/faucon) ; repli sur le libellé texte si absente.
  // Grisée quand le compagnon est inactif (tint alpha réduit via le canal de couleur).
  ro::IconTex ic = ResolveSkillIcon(skillId);
  if (ic.tex) {
    const float pad = 4.0f;
    const ImU32 tint = active ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 140);
    dl->AddImage(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(p0.x + pad, p0.y + pad),
                 ImVec2(p1.x - pad, p1.y - pad), ImVec2(0, 0), ImVec2(1, 1), tint);
  } else {
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p0.x + (sz - ts.x) * 0.5f, p0.y + (sz - ts.y) * 0.5f),
                active ? IM_COL32(0, 40, 0, 255) : IM_COL32(90, 90, 90, 255), label);
  }

  const bool hov = ImGui::IsItemHovered();
  if (hov) ro::SetHoverCursor(2);

  // Clic gauche = basculer.
  if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (kind == kCompCart) {
      if (active) SendCompanionPkt(kCompCart, kCompOff, 0);
      else        SendCompanionPkt(kCompCart, kCompOn, last_cart_type_ > 0 ? last_cart_type_ : 1);
    } else {
      SendCompanionPkt(kind, active ? kCompOff : kCompOn, 0);
    }
  }

  // Menu contextuel (chariot uniquement).
  if (kind == kCompCart) {
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("cart_ctx");
    if (ImGui::BeginPopup("cart_ctx")) {
      if (ImGui::MenuItem("Ouvrir le chariot", nullptr, false, active)) OpenCartWindow();
      const bool canDeco = companion_.changecart_lv > 0 && active;
      if (ImGui::MenuItem("Changer la décoration", nullptr, false, canDeco)) {
        int next = companion_.cart_active + 1;
        if (next > companion_.cart_deco_max || next < 1) next = 1;
        SendCompanionPkt(kCompCart, kCompDeco, next);
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Retirer le chariot", nullptr, false, active))
        SendCompanionPkt(kCompCart, kCompOff, 0);
      ImGui::EndPopup();
    }
  }

  // Tooltip (pas pendant que le menu est ouvert).
  if (hov && !ImGui::IsPopupOpen("cart_ctx")) {
    if (kind == kCompCart)
      ImGui::SetTooltip("%s — %s\n(clic gauche : %s, clic droit : menu)", name,
                        active ? "actif" : "inactif", active ? "ranger" : "invoquer");
    else
      ImGui::SetTooltip("%s — %s\n(clic gauche : %s)", name, active ? "actif" : "inactif",
                        active ? "renvoyer" : "invoquer");
  }
  ImGui::PopID();
}

// Ouvre la fenêtre d'inventaire du chariot. MakeWindow crée/affiche par id (RE 2026-07-12 :
// id 0x28 = UIMerchantItemWnd, vtable 0x0103d538 ; même appel que la fenêtre de description).
// Le case a un gate de contexte UI (IsWindowAllowedInContext) qui passe en jeu normal ;
// OnCreate ne dépend pas de l'état cart (au pire fenêtre vide), le serveur pousse le contenu.
void CharacterSheet::OpenCartWindow() {
  constexpr int kCartWndId = 0x28;  // UIMerchantItemWnd (fenêtre inventaire chariot)
  __try {
    reinterpret_cast<MakeWindow_t>(kMakeWindow)(
        uiwnd::Mgr(), nullptr,
        reinterpret_cast<void*>(static_cast<uintptr_t>(kCartWndId)));
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
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
    ro::IconTex em = ResolveEmblem(gi.guildId);
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
    DrawAmmoSlot(rx, wpn_y, sz);                             // munition -> à droite du bouclier
    // Compagnons (chariot/peco/faucon) à GAUCHE de l'arme, empilés vers le bas. Seules les
    // cases dont le skill est appris apparaissent (état poussé par le serveur).
    const int nComp = DrawCompanions(lx, wpn_y, sz, gap);
    content_bottom = wpn_y + sz;  // arme/bouclier/munition
    if (nComp > 0)
      content_bottom = std::max(content_bottom, wpn_y + (nComp - 1) * (sz + gap) + sz);
  }
  ImGui::SetCursorPos(ImVec2(ax, y0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, DollBgCol());
  ImGui::BeginChild("cs_avatar", ImVec2(cw, ch), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  // Costumes : TOUJOURS affichés dans la vue Costume ; dans la vue Équipement, seulement si
  // « Voir les costumes » est coché (flag 0x016024c0 == 0). Sinon on rend l'équipement RÉEL.
  avatar_show_costume_ = costume_ || (ReadInt(kCostumeHideFlag) == 0);
  if (auto* bi = Bourgeon::Instance().basic_info()) {
    const ImVec2 rp = ImGui::GetWindowPos();
    const ImVec2 rs = ImGui::GetWindowSize();
    bi->RenderPlayerAvatar(rp.x + 2.0f, rp.y + 2.0f, rs.x - 4.0f, rs.y - 4.0f,
                           avatar_anim_, avatar_dir_, avatar_animate_, avatar_show_costume_);
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
      if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();  // persister la direction
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
        if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();  // persister la pose
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
          bi && bi->ExportAvatarGif(gif_export_anim_, gif_export_dir_, p.c_str(),
                                    gif_export_show_costume_);
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

  // Case config native SOUS le combo de pose, selon l'onglet (bascule via le dispatcher, le
  // serveur répond ZC_CONFIG qui applique + rafraîchit le sprite) :
  //   Costume -> « Voir les costumes » (flag 0x016024c0 : 0=affiché) ;
  //   Équipement -> « Montrer mon équipement » aux autres (flag 0x015ffd14 : 1=visible).
  const float cfg_y = sel_y + fh + (gif_status_.empty() ? 0.0f : fh) + 4.0f;
  if (costume_) {
    bool show = ReadInt(kCostumeHideFlag) == 0;
    const float cw = ImGui::CalcTextSize("Voir les costumes").x + 42.0f;
    ImGui::SetCursorPos(ImVec2(start_x + std::max(0.0f, (avail_w - cw) * 0.5f), cfg_y));
    if (ro::RoCheckbox("Voir les costumes", &show))
      SendConfigToggle(kCmdViewCostume, show ? 1 : 0);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Affiche ou masque les costumes sur ton personnage");
  } else {
    bool pub = ReadInt(kShowEquipFlag) != 0;
    const float cw = ImGui::CalcTextSize("Montrer mon équipement").x + 42.0f;
    ImGui::SetCursorPos(ImVec2(start_x + std::max(0.0f, (avail_w - cw) * 0.5f), cfg_y));
    if (ro::RoCheckbox("Montrer mon équipement", &pub))
      SendConfigToggle(kCmdShowEquip, pub ? 1 : 0);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Rend ton équipement visible (ou non) aux autres joueurs");
  }
  // Effet costume (.str) : rendu 100 % automatique et toujours actif (aucun réglage UI).
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
  gif_export_show_costume_ = avatar_show_costume_;  // fige aussi l'état costume

  // Nom par défaut : avatar_<pseudo>_<pose>_d<N>.gif (pseudo assaini).
  std::string name = Bourgeon::Instance().client().session().GetCharName();
  for (char& c : name)
    if (c && std::strchr("\\/:*?\"<>| ", c)) c = '_';
  if (name.empty()) name = "perso";
  char defname[MAX_PATH];
  std::snprintf(defname, sizeof(defname), "avatar_%s_%s_d%d.gif", name.c_str(),
                PoseLabel(gif_export_anim_), gif_export_dir_);

  // Dossier initial proposé : <jeu>\screenshot (créé s'il manque).
  const std::string initdir = paths::InGameDir("screenshot");
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

  // Tooltip enrichi d'une stat primaire : rôle + split équip/carte quand le serveur
  // l'a poussé (ZC_BOURGEON_STAT_BONUS). Le natif ne donne que le TOTAL ; ici on
  // détaille l'origine. buf doit vivre jusqu'à l'appel ImGui (pile de l'appelant).
  auto primaryTip = [&](int i, char* buf, int cap) -> const char* {
    if (bonus_.valid && (bonus_.equip[i] != 0 || bonus_.card[i] != 0))
      std::snprintf(buf, cap, "%s\nÉquipement : %+d   Cartes : %+d",
                    kStatDesc[i], bonus_.equip[i], bonus_.card[i]);
    else
      std::snprintf(buf, cap, "%s", kStatDesc[i]);
    return buf;
  };

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
    if (ImGui::IsItemHovered()) { char tb[256]; ImGui::SetTooltip("%s", primaryTip(i, tb, sizeof(tb))); }  // rôle + split équip/carte
    ImGui::SameLine();
    ImGui::SetCursorPosX(val_x);                                // colonne valeurs
    if (s.bonus[i] != 0)
      ImGui::TextColored(kBlack, "— %d (+%d)", s.base[i], s.bonus[i]);
    else
      ImGui::TextColored(kBlack, "— %d", s.base[i]);
    if (ImGui::IsItemHovered()) { char tb[256]; ImGui::SetTooltip("%s", primaryTip(i, tb, sizeof(tb))); }
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
  char b[112];
  // Accole « (label ±X) » à la valeur courante (équip, refine…) si non nul.
  auto append = [&](const char* label, int contrib) {
    if (bonus_.valid && contrib != 0) {
      const size_t n = std::strlen(b);
      std::snprintf(b + n, sizeof(b) - n, "  (%s %+d)", label, contrib);
    }
  };
  auto appendEquip = [&](int contrib) { append("équip", contrib); };
  // Variante % (ex. DEF de refine, qui alimente la réduction en %).
  auto appendPct = [&](const char* label, int contrib) {
    if (bonus_.valid && contrib != 0) {
      const size_t n = std::strlen(b);
      std::snprintf(b + n, sizeof(b) - n, "  (%s %+d%%)", label, contrib);
    }
  };
  std::snprintf(b, sizeof(b), "%d + %d", s.atk1, s.atk2);
  appendEquip(bonus_.eatk);
  append("refine", bonus_.refine_atk);
  stat("ATK", b, "Attaque physique (arme + statut) : détermine les dégâts des coups physiques.");
  std::snprintf(b, sizeof(b), "%d ~ %d", s.matk_min, s.matk_max);
  appendEquip(bonus_.ematk);
  stat("MATK", b, "Attaque magique : détermine les dégâts des sorts.");
  std::snprintf(b, sizeof(b), "%d%% + %d", s.def_s, s.def_h);
  appendPct("refine", bonus_.refine_def);
  stat("DEF", b, "Défense physique : réduction en % (VIT/équip, def1) + réduction plate (def2). « refine » = part du raffinage des armures (dans la réduction %).");
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

  // ── Bonus d'équipement/cartes (poussés par le serveur, ZC_BOURGEON_STAT_BONUS) ──
  // Origines que le natif n'expose pas : bonus plats + conditionnels vs cible.
  // Libellé + valeur en colonne val_x (comme les dérivées) ; fond gris LIMITÉ au
  // libellé. Un libellé conditionnel trop long décale sa valeur juste après lui (pas
  // de chevauchement). N'affiche que les entrées non nulles.
  if (bonus_.valid) {
    // Calqué sur le lambda `stat` : fond sous le seul libellé, valeur alignée à val_x.
    auto bonusStat = [&](const char* label, const char* value, const char* tip) {
      const float start_x = ImGui::GetCursorPosX();
      const ImVec2 rp = ImGui::GetCursorScreenPos();
      const float nw = ImGui::CalcTextSize(label).x;  // fond limité au libellé
      ImGui::GetWindowDrawList()->AddRectFilled(
          ImVec2(rp.x - 3.0f, rp.y - 1.0f),
          ImVec2(rp.x + nw + 5.0f, rp.y + ImGui::GetTextLineHeight() + 1.0f), kRowBg, 4.0f);
      ImGui::TextColored(kBlack, "%s", label);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
      ImGui::SameLine();
      const float after = start_x + nw + ImGui::GetStyle().ItemSpacing.x;
      ImGui::SetCursorPosX(after > val_x ? after : val_x);  // aligné, sauf libellé trop long
      ImGui::TextColored(kBlack, "%s", value);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    char vb[24];
    auto pct = [&](const char* label, int v, const char* tip) {
      if (v == 0) return;
      std::snprintf(vb, sizeof(vb), "%+d%%", v);
      bonusStat(label, vb, tip);
    };
    auto flat = [&](const char* label, int v, const char* tip) {
      if (v == 0) return;
      std::snprintf(vb, sizeof(vb), "%+d", v);
      bonusStat(label, vb, tip);
    };
    const bool any_flat =
        bonus_.melee_pct || bonus_.ranged_pct || bonus_.crit_dmg_pct || bonus_.hp_add ||
        bonus_.sp_add || bonus_.aspd_add || bonus_.vcast_pct || bonus_.fcast_pct ||
        bonus_.atk_pct || bonus_.matk_pct || bonus_.dmg_ret_melee || bonus_.dmg_ret_ranged ||
        bonus_.dmg_ret_magic || bonus_.double_pct || bonus_.perfect_hit || bonus_.hp_pct ||
        bonus_.sp_pct || bonus_.hp_regen_pct || bonus_.sp_regen_pct || bonus_.crit_def_pct ||
        bonus_.hp_on_kill || bonus_.sp_on_kill || bonus_.unbreak_pct || bonus_.pot_hp_pct ||
        bonus_.pot_sp_pct || bonus_.heal_up_pct || bonus_.delay_pct || bonus_.add_vcast_ms ||
        bonus_.add_fcast_ms || bonus_.steal_pct || bonus_.def_melee_pct || bonus_.def_ranged_pct ||
        bonus_.def_magic_pct || bonus_.def_misc_pct || bonus_.splash || bonus_.splash_add ||
        bonus_.hp_drain_pct || bonus_.sp_drain_pct || bonus_.break_weapon_pct ||
        bonus_.break_armor_pct || bonus_.zeny_bonus_pct || bonus_.classchange_pct ||
        bonus_.dmg_ret_reduce || bonus_.magic_hp_gain || bonus_.magic_sp_gain;
    ImGui::Separator();
    // Sections repliables (la fiche peut être bien fournie) ; ouvertes par défaut.
    constexpr ImGuiTreeNodeFlags kSec = ImGuiTreeNodeFlags_DefaultOpen;
    if (any_flat && ImGui::CollapsingHeader("Bonus d'équipement", kSec)) {
    pct("Mêlée", bonus_.melee_pct, "Dégâts de mêlée à mains nues (%).");
    pct("Distance", bonus_.ranged_pct, "Dégâts des attaques à distance (%).");
    pct("Dég. crit.", bonus_.crit_dmg_pct, "Dégâts des coups critiques (%).");
    flat("PV max", bonus_.hp_add, "PV max ajoutés par l'équipement.");
    flat("SP max", bonus_.sp_add, "SP max ajoutés par l'équipement.");
    flat("ASPD", bonus_.aspd_add, "Vitesse d'attaque ajoutée (valeur plate).");
    pct("Cast var.", bonus_.vcast_pct, "Temps de cast variable (%). Négatif = réduction.");
    pct("Cast fixe", bonus_.fcast_pct, "Temps de cast fixe (%). Négatif = réduction.");
    // Lot A — offensif
    pct("ATK %", bonus_.atk_pct, "Bonus d'ATK physique global (%).");
    pct("MATK %", bonus_.matk_pct, "Bonus d'ATK magique global (%).");
    pct("Renvoi mêlée", bonus_.dmg_ret_melee, "Renvoie une part des dégâts de mêlée reçus (%).");
    pct("Renvoi dist.", bonus_.dmg_ret_ranged, "Renvoie une part des dégâts à distance reçus (%).");
    pct("Renvoi mag.", bonus_.dmg_ret_magic, "Renvoie une part des dégâts magiques reçus (%).");
    pct("Double att.", bonus_.double_pct, "Chance de frapper deux fois (%).");
    pct("Coup parfait", bonus_.perfect_hit, "Chance de coup parfait (%) : ignore FLEE et DEF.");
    // Lot B — survie
    pct("PV max %", bonus_.hp_pct, "Bonus de PV maximum (%).");
    pct("SP max %", bonus_.sp_pct, "Bonus de SP maximum (%).");
    pct("Régén. PV", bonus_.hp_regen_pct, "Récupération naturelle de PV (%).");
    pct("Régén. SP", bonus_.sp_regen_pct, "Récupération naturelle de SP (%).");
    pct("Réduc. crit", bonus_.crit_def_pct, "Réduit la probabilité de subir un critique (%).");
    flat("PV/kill", bonus_.hp_on_kill, "PV récupérés en tuant un ennemi.");
    flat("SP/kill", bonus_.sp_on_kill, "SP récupérés en tuant un ennemi.");
    pct("Incassable", bonus_.unbreak_pct, "Chance d'éviter la casse d'un équipement (%).");
    // Lot C — utilitaire
    pct("Potions PV", bonus_.pot_hp_pct, "Efficacité des objets de soin PV (%).");
    pct("Potions SP", bonus_.pot_sp_pct, "Efficacité des objets de soin SP (%).");
    pct("Soin donné", bonus_.heal_up_pct, "Puissance des soins que vous prodiguez (%).");
    pct("Délai skill", bonus_.delay_pct, "After-cast delay (%). Négatif = réduction.");
    flat("Cast var. ms", bonus_.add_vcast_ms, "Ajout/retrait au cast variable, en millisecondes.");
    flat("Cast fixe ms", bonus_.add_fcast_ms, "Ajout/retrait au cast fixe, en millisecondes.");
    pct("Vol", bonus_.steal_pct, "Taux de vol d'objets (%).");
    // Lot E — réduction par type d'attaque + splash
    pct("Réduc. mêlée", bonus_.def_melee_pct, "Réduit les dégâts de mêlée reçus (%).");
    pct("Réduc. distance", bonus_.def_ranged_pct, "Réduit les dégâts à distance reçus (%).");
    pct("Réduc. magie", bonus_.def_magic_pct, "Réduit les dégâts magiques reçus (%).");
    pct("Réduc. divers", bonus_.def_misc_pct, "Réduit les dégâts divers reçus (%).");
    flat("Splash", bonus_.splash, "Portée de la zone d'effet de vos attaques (cases).");
    flat("Splash+", bonus_.splash_add, "Portée de splash additionnelle (cases).");
    // Lot F — vol de vie
    pct("Vol PV", bonus_.hp_drain_pct, "PV volés à chaque attaque (% des dégâts).");
    pct("Vol SP", bonus_.sp_drain_pct, "SP volés à chaque attaque (% des dégâts).");
    // Lot G — très niche
    pct("Casse arme", bonus_.break_weapon_pct, "Chance de casser l'arme de la cible (%).");
    pct("Casse armure", bonus_.break_armor_pct, "Chance de casser l'armure de la cible (%).");
    pct("Zeny bonus", bonus_.zeny_bonus_pct, "Bonus de Zeny obtenu sur les monstres (%).");
    pct("Transforme", bonus_.classchange_pct, "Chance de transformer la cible en un autre monstre (%).");
    pct("Réduc. renvoi", bonus_.dmg_ret_reduce, "Réduit les dégâts que vous subissez du renvoi (%).");
    flat("PV au sort", bonus_.magic_hp_gain, "PV récupérés en lançant un sort.");
    flat("SP au sort", bonus_.magic_sp_gain, "SP récupérés en lançant un sort.");
    }  // ── fin « Bonus d'équipement »

    // Conditionnels : (code, idx) -> libellé via les tables de noms.
    auto nameOf = [](const char* const* tbl, int n, int idx) -> const char* {
      return (idx >= 0 && idx < n) ? tbl[idx] : "?";
    };
    if (!bonus_.cond.empty() && ImGui::CollapsingHeader("Conditionnels", kSec))
    for (const auto& c : bonus_.cond) {
      const char* kind = "Bonus";
      const char* who = "?";
      char who_buf[24];  // repli pour les groupes RC2 sans libellé
      auto rc2 = [&](int idx) -> const char* {
        if (idx >= 0 && idx < IM_ARRAYSIZE(kRace2Name) && kRace2Name[idx][0]) return kRace2Name[idx];
        std::snprintf(who_buf, sizeof(who_buf), "groupe #%d", idx);
        return who_buf;
      };
      switch (c.code) {
        case kBscSubEle:  kind = "Résist. vs"; who = nameOf(kEleName, IM_ARRAYSIZE(kEleName), c.idx); break;
        case kBscSubRace: kind = "Résist. vs"; who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscSubSize: kind = "Résist. vs"; who = nameOf(kSizeName, IM_ARRAYSIZE(kSizeName), c.idx); break;
        case kBscAddEle:  kind = "Dégâts vs";  who = nameOf(kEleName, IM_ARRAYSIZE(kEleName), c.idx); break;
        case kBscAddRace: kind = "Dégâts vs";  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscAddSize: kind = "Dégâts vs";  who = nameOf(kSizeName, IM_ARRAYSIZE(kSizeName), c.idx); break;
        case kBscMAddEle:  kind = "Dég. mag. vs"; who = nameOf(kEleName, IM_ARRAYSIZE(kEleName), c.idx); break;
        case kBscMAddRace: kind = "Dég. mag. vs"; who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscMAddSize: kind = "Dég. mag. vs"; who = nameOf(kSizeName, IM_ARRAYSIZE(kSizeName), c.idx); break;
        case kBscCritRace:    kind = "Crit vs";       who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscIgnDefRace:  kind = "Ignore DEF vs";  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscIgnMdefRace: kind = "Ignore MDEF vs"; who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscSubdefEle:   kind = "Résist. arme";   who = nameOf(kEleName, IM_ARRAYSIZE(kEleName), c.idx); break;
        case kBscSubClass:    kind = "Réduc. vs";      who = nameOf(kClassName, IM_ARRAYSIZE(kClassName), c.idx); break;
        case kBscSubRace2:  kind = "Réduc. vs";  who = rc2(c.idx); break;
        case kBscExpRace:   kind = "EXP vs";  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscExpClass:  kind = "EXP vs";  who = nameOf(kClassName, IM_ARRAYSIZE(kClassName), c.idx); break;
        case kBscDropRace:  kind = "Drop vs"; who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscDropClass: kind = "Drop vs"; who = nameOf(kClassName, IM_ARRAYSIZE(kClassName), c.idx); break;
        // Très niche
        case kBscDefsetRace:   kind = "DEF fixée vs";  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscMdefsetRace:  kind = "MDEF fixée vs"; who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscHpVanishRace: kind = "Vanish PV vs";  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscSpVanishRace: kind = "Vanish SP vs";  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscComaRace:     kind = "Coma vs";  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscComaClass:    kind = "Coma vs";  who = nameOf(kClassName, IM_ARRAYSIZE(kClassName), c.idx); break;
        case kBscIgnResRace:   kind = "Ignore RES vs";  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscIgnMresRace:  kind = "Ignore MRES vs"; who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscMAddRace2:      kind = "Dég. mag. vs";  who = rc2(c.idx); break;
        case kBscIgnMdefRace2:   kind = "Ignore MDEF vs"; who = rc2(c.idx); break;
        case kBscSpGainRace:   kind = "SP/kill vs";  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        default: break;
      }
      char label[64];
      std::snprintf(label, sizeof(label), "%s %s", kind, who);
      std::snprintf(vb, sizeof(vb), "%+d%%", c.value);
      bonusStat(label, vb, "Bonus conditionnel : ne s'applique que contre ce type de cible.");
    }

    // Bonus liés à un skill : nom résolu via le wrapper Lua natif (localisé).
    auto skillName = [](uint16_t id) -> const char* {
      const char* n = reinterpret_cast<GetSkillNameLua_t>(kGetSkillNameLua)(id);
      return (n && *n) ? n : "?";
    };
    if (!bonus_.skills.empty() && ImGui::CollapsingHeader("Skills & statuts", kSec))
    for (const auto& sk : bonus_.skills) {
      char label[96];
      const char* tip = "Bonus lié à un skill.";
      switch (sk.code) {
        case kBskAutospell:
        case kBskAutospellHit: {
          const char* pre = (sk.code == kBskAutospellHit) ? "Riposte" : "Autocast";
          const char* nm = skillName(sk.skill_id);
          if (sk.lv > 0)
            std::snprintf(label, sizeof(label), "%s %s Niv %d", pre, nm, sk.lv);
          else
            std::snprintf(label, sizeof(label), "%s %s", pre, nm);
          std::snprintf(vb, sizeof(vb), "%d,%d%%", sk.value / 10, sk.value % 10);  // ‰ -> %
          tip = (sk.code == kBskAutospellHit)
                    ? "Chance de lancer ce sort automatiquement quand vous êtes touché."
                    : "Chance de lancer ce sort automatiquement en attaquant.";
          break;
        }
        case kBskSkillAtk:
          std::snprintf(label, sizeof(label), "Dégâts %s", skillName(sk.skill_id));
          std::snprintf(vb, sizeof(vb), "%+d%%", sk.value);
          tip = "Bonus de dégâts sur ce skill précis (%).";
          break;
        case kBskAddeff:
        case kBskAddeffHit: {
          // skill_id porte l'EFST du statut, résolu en nom via GetStateIconDescript.
          const char* pre = (sk.code == kBskAddeffHit) ? "Riposte statut" : "Inflige";
          std::snprintf(label, sizeof(label), "%s %s", pre, StatusName(sk.skill_id));
          std::snprintf(vb, sizeof(vb), "%d,%02d%%", sk.value / 100, std::abs(sk.value) % 100);  // 1/100% -> %
          tip = (sk.code == kBskAddeffHit)
                    ? "Chance d'infliger ce statut à l'attaquant quand vous êtes touché."
                    : "Chance d'infliger ce statut à la cible en attaquant.";
          break;
        }
        case kBskReseff: {
          // skill_id porte l'EFST du statut (résolu via GetStateIconDescript).
          std::snprintf(label, sizeof(label), "Résist. %s", StatusName(sk.skill_id));
          std::snprintf(vb, sizeof(vb), "%d,%02d%%", sk.value / 100, std::abs(sk.value) % 100);  // 1/100% -> %
          tip = "Résistance à ce statut (chance/durée réduite).";
          break;
        }
        case kBskSubskill:
          std::snprintf(label, sizeof(label), "Réduc. %s", skillName(sk.skill_id));
          std::snprintf(vb, sizeof(vb), "%+d%%", sk.value);
          tip = "Réduit les dégâts subis de ce skill (%).";
          break;
        case kBskAutospellSkill: {
          // Deux noms de skill (casté + déclencheur) : copier le 1er avant le 2e appel.
          char cast[48];
          std::strncpy(cast, skillName(sk.skill_id), sizeof(cast) - 1);
          cast[sizeof(cast) - 1] = '\0';
          const char* trig = sk.aux ? skillName(sk.aux) : "?";
          if (sk.lv > 0)
            std::snprintf(label, sizeof(label), "Autocast %s Niv %d sur %s", cast, sk.lv, trig);
          else
            std::snprintf(label, sizeof(label), "Autocast %s sur %s", cast, trig);
          std::snprintf(vb, sizeof(vb), "%d,%d%%", sk.value / 10, sk.value % 10);  // ‰ -> %
          tip = "Chance de lancer ce sort en utilisant le skill déclencheur.";
          break;
        }
        case kBskSkillSprate: case kBskSkillSpcost:
        case kBskSkillVcastrate: case kBskSkillFcastrate:
        case kBskSkillVcast: case kBskSkillFcast:
        case kBskSkillCooldown: case kBskSkillDelay:
        case kBskSkillHeal: case kBskSkillHeal2: case kBskSkillBlown: {
          // Modificateur d'un skill précis. prefix + unité selon le code.
          const char* pre = "";
          int unit = 0;  // 0=% 1=ms 2=plat
          switch (sk.code) {
            case kBskSkillSprate:    pre = "Coût SP";   unit = 0; break;
            case kBskSkillSpcost:    pre = "Coût SP";   unit = 2; break;
            case kBskSkillVcastrate: pre = "Cast var."; unit = 0; break;
            case kBskSkillFcastrate: pre = "Cast fixe"; unit = 0; break;
            case kBskSkillVcast:     pre = "Cast var."; unit = 1; break;
            case kBskSkillFcast:     pre = "Cast fixe"; unit = 1; break;
            case kBskSkillCooldown:  pre = "Cooldown";  unit = 1; break;
            case kBskSkillDelay:     pre = "Délai";     unit = 0; break;
            case kBskSkillHeal:      pre = "Soin";      unit = 0; break;
            case kBskSkillHeal2:     pre = "Soin reçu"; unit = 0; break;
            case kBskSkillBlown:     pre = "Knockback"; unit = 2; break;
          }
          std::snprintf(label, sizeof(label), "%s %s", pre, skillName(sk.skill_id));
          if (unit == 0)      std::snprintf(vb, sizeof(vb), "%+d%%", sk.value);
          else if (unit == 1) std::snprintf(vb, sizeof(vb), "%+d ms", sk.value);
          else                std::snprintf(vb, sizeof(vb), "%+d", sk.value);
          tip = "Modificateur appliqué à ce skill précis.";
          break;
        }
        default:
          std::snprintf(label, sizeof(label), "%s", skillName(sk.skill_id));
          std::snprintf(vb, sizeof(vb), "%+d", sk.value);
          break;
      }
      bonusStat(label, vb, tip);
    }

    // Bonus liés à un item : nom résolu via le DB item (ItemName, caché + SEH).
    if (!bonus_.items.empty() && ImGui::CollapsingHeader("Objets", kSec))
    for (const auto& it : bonus_.items) {
      char label[96];
      if (it.code == kBsiAddDropGroup)  // nameid porte l'id de GROUPE, pas d'item
        std::snprintf(label, sizeof(label), "Drop groupe #%u", it.nameid);
      else
        std::snprintf(label, sizeof(label), "Drop %s", ItemName(it.nameid));
      std::snprintf(vb, sizeof(vb), "%d,%02d%%", it.rate / 100, std::abs(it.rate) % 100);  // 1~10000 -> %
      bonusStat(label, vb, "Chance de drop bonus de cet objet en tuant un monstre.");
    }
  }
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
    if (ImGui::BeginTabItem("Titres"))     { tab_ = 3; ImGui::EndTabItem(); }
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
  } else if (tab_ == 3) {
    // Onglet Titres : pleine largeur, liste des titres possédés + titre équipé.
    ImGui::BeginChild("cs_titles", ImVec2(0, 0), true);
    DrawTitlesTab();
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
