#include "plugins/character_sheet.h"
#include "ui/game_texture.h"

// Icônes d'item : ro::ItemIcon (ui/icon_cache.h) — cache partagé. Les caches
// de skill et d'emblème restent locaux : chemins et clés différents.
#include "ui/icon_cache.h"
#include "ui/head_icon.h"  // miniature de tête des membres de guilde
#include "ragnarok/uiwnd.h"
#include "ragnarok/skill_cooldowns.h"  // table de cooldowns partagée (ZC 0x043D)
#include "utils/game_paths.h"
#include <Windows.h>
#include <commdlg.h>  // GetSaveFileNameA (dialogue « Enregistrer sous »)
#include <objbase.h>  // CoInitializeEx pour le thread du dialogue

#include <algorithm>
#include <cctype>
#include <cmath>  // longueur d'un segment : flèches de prérequis du grimoire
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bourgeon.h"        // Bourgeon::Instance().SendPacket / session
#include "plugins/bourgeon_opcodes.h"  // kStatBonus (ZC 0x0F10)
#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB
#include "imgui.h"
#include "plugins/basic_info.h"    // RenderPlayerAvatar (avatar plein-corps)
#include "plugins/inventory_viewer.h"  // LinkItemToChat / EquipDraggedItem (drag-drop, chat)
#include "plugins/rodex_tweaks.h"      // ComposeTo : « Envoyer un courrier » sur un membre
#include "plugins/moonlight_ui.h"      // SaveSettings (persistance des presets)
#include "plugins/hotkey_util.h"       // capture/libellé/conflit d'un raccourci
#include "plugins/imgui_escape.h"
#include "ui/ro_imgui.h"
#include "utils/tinf_inflate.h"  // inflate zlib pour les emblèmes de guilde (.ebm)
#include "utils/log_console.h"   // LogDiag : échecs de chargement de l'arbre de guilde

//  Constantes RE (client 20250716, base 0x400000 ; cf. project_character_sheet)
namespace {

constexpr uintptr_t kSession = 0x015fa3c0;

//  Tableau equip : session + base + slot*0xf8 (in-place, aucun appel C++)
constexpr int kEquipBase   = 0x17d0;   // equipement normal
constexpr int kCostumeBase = 0x2b30;   // costume
constexpr int kSlotStride  = 0xf8;
// Offsets d'une entree (ItemSkillInfo).
constexpr int kOffEquipInvIndex = 0x04;   // index inventaire (a envoyer pour desequiper)
constexpr int kOffEquipLocation = 0x08;   // masque EQP
constexpr int kOffEquipPresent  = 0x10;   // ==1 si occupe
constexpr int kOffEquipResname  = 0x2c;   // std::string (SSO) : itemId en texte
constexpr int kOffEquipResCap   = 0x40;   // capacite SSO (>15 => heap)
constexpr int kOffEquipRefine   = 0x60;
constexpr int kOffEquipView     = 0x70;
constexpr int kOffEquipType     = 0x00;   // type d'item (equip 4/5/8/9/0xb-0xf)
constexpr int kOffEquipCards    = 0x1c;   // 4 u32 (nameid des cartes / forge)
constexpr int kOffEquipGrade    = 0x88;   // i16 : grade d'enchant
constexpr int kOffEquipWear     = 0x0c;   // etat porte (!=0 => equipe)
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
constexpr int kOffEquipAmount = 0x10;   // quantité (item d'inventaire) ; == present pour un equip

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
constexpr int       kCmdUseSkill     = 0x45;   // lancer une compétence sur soi / toggle
// g_Own_AccountId : notre AID, qui est aussi le GID de notre acteur (cible d'un self-cast).
constexpr uintptr_t kOwnAccountId    = 0x015fb9a4;
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
    const int invIndex = *reinterpret_cast<const int*>(e + kOffEquipInvIndex);
    const int present  = *reinterpret_cast<const int*>(e + kOffEquipPresent);
    if (invIndex == 0 || present != 1) return false;  // slot vide
    out->present  = true;
    out->invIndex = invIndex;
    out->location = *reinterpret_cast<const int*>(e + kOffEquipLocation);
    out->refine   = *reinterpret_cast<const int*>(e + kOffEquipRefine);
    out->viewId   = *reinterpret_cast<const int*>(e + kOffEquipView);
    const uint32_t cap = *reinterpret_cast<const uint32_t*>(e + kOffEquipResCap);
    const char* rn = (cap > 15) ? *reinterpret_cast<const char* const*>(e + kOffEquipResname)
                                : reinterpret_cast<const char*>(e + kOffEquipResname);
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
      if (*reinterpret_cast<const int*>(info + kOffEquipInvIndex) == ammoIdx) {
        out->present  = true;
        out->invIndex = ammoIdx;
        out->amount   = *reinterpret_cast<const int*>(info + kOffEquipAmount);
        out->location = *reinterpret_cast<const int*>(info + kOffEquipLocation);
        out->viewId   = *reinterpret_cast<const int*>(info + kOffEquipView);
        const uint32_t cap = *reinterpret_cast<const uint32_t*>(info + kOffEquipResCap);
        const char* rn = (cap > 15) ? *reinterpret_cast<const char* const*>(info + kOffEquipResname)
                                    : reinterpret_cast<const char*>(info + kOffEquipResname);
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
      it.type     = *reinterpret_cast<const int*>(info + kOffEquipType);
      it.index    = *reinterpret_cast<const int*>(info + kOffEquipInvIndex);
      it.loc      = *reinterpret_cast<const int*>(info + kOffEquipLocation);
      it.equipped = *reinterpret_cast<const int*>(info + kOffEquipWear) != 0;
      it.refine   = *reinterpret_cast<const int*>(info + kOffEquipRefine);
      it.grade    = *reinterpret_cast<const short*>(info + kOffEquipGrade);
      for (int c = 0; c < 4; ++c)
        it.cards[c] = *reinterpret_cast<const uint32_t*>(info + kOffEquipCards + c * 4);
      const uint32_t capS = *reinterpret_cast<const uint32_t*>(info + kOffEquipResCap);
      const char* rn = (capS > 15) ? *reinterpret_cast<const char* const*>(info + kOffEquipResname)
                                   : reinterpret_cast<const char*>(info + kOffEquipResname);
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
    if (*reinterpret_cast<const int*>(e + kOffEquipInvIndex) == 0 ||
        *reinterpret_cast<const int*>(e + kOffEquipPresent) != 1)
      return false;
    out->index    = *reinterpret_cast<const int*>(e + kOffEquipInvIndex);
    out->type     = *reinterpret_cast<const int*>(e + kOffEquipType);
    out->loc      = *reinterpret_cast<const int*>(e + kOffEquipLocation);
    out->equipped = true;
    out->refine   = *reinterpret_cast<const int*>(e + kOffEquipRefine);
    out->grade    = *reinterpret_cast<const short*>(e + kOffEquipGrade);
    for (int c = 0; c < 4; ++c)
      out->cards[c] = *reinterpret_cast<const uint32_t*>(e + kOffEquipCards + c * 4);
    const uint32_t cap = *reinterpret_cast<const uint32_t*>(e + kOffEquipResCap);
    const char* rn = (cap > 15) ? *reinterpret_cast<const char* const*>(e + kOffEquipResname)
                                : reinterpret_cast<const char*>(e + kOffEquipResname);
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

//  ── Guilde : globals étendus + roster complet (RE 2026-07-26, IDA) ───────────
//  Le serveur (moonlight, PACKETVER 20250716) envoie ZC_GUILD_INFO 0x0b7b et
//  ZC_MEMBERMGR_INFO 0x0b7d ; côté client ce sont GuildNet_OnGuildInfoEx2
//  (0x00ce1f40) et GuildNet_OnMemberList_v58 (0x00ce37c0) qui les décodent. Les
//  labels Ghidra/IDA de ces globals sont DÉCALÉS d'un champ (ils viennent du
//  parseur legacy 0x01b6, qui lit deux fois userNum) : la carte ci-dessous est
//  celle des handlers réellement appelés, recoupée avec
//  UIGuildTotalInfoWnd_DrawContent (0x00923a10).
constexpr uintptr_t kGuildMasterName = 0x0159c1a0;  // std::string : nom du maître
constexpr uintptr_t kGuildOnlineNum  = 0x0159c1f0;  // connect_member (membres en ligne)
constexpr uintptr_t kGuildMemberMax  = 0x0159c1f4;  // max_member
constexpr uintptr_t kGuildAvgLevel   = 0x0159c1f8;  // niveau moyen
constexpr uintptr_t kGuildManageLand = 0x0159c1fc;  // std::string : territoire (nb de forts)
constexpr uintptr_t kGuildExp        = 0x0159c214;  // exp courante
constexpr uintptr_t kGuildNextExp    = 0x0159c218;  // exp du niveau suivant
constexpr uintptr_t kGuildNoticeSubj = 0x0159c1b8;  // std::string : sujet de l'annonce (ZC 0x016f)
constexpr uintptr_t kGuildNoticeBody = 0x0159c1d0;  // std::string : corps de l'annonce
constexpr uintptr_t kGuildRelHead    = 0x0159c26c;  // CGuild+0xe4 : liste alliés/ennemis

//  Enregistrement d'un membre, offsets NODE-relatifs (payload = node+8 ; cf.
//  GuildNet_OnMemberList_v58 -> CGuild_AppendMemberRecord).
constexpr int kMemCid       = 0x0c;  // char id
constexpr int kMemName      = 0x10;  // std::string : nom du personnage
constexpr int kMemHair      = 0x28;  // coiffure (style)
constexpr int kMemHairColor = 0x2c;  // couleur de cheveux (palette)
constexpr int kMemJob       = 0x30;  // classe (job id)
constexpr int kMemSex       = 0x34;  // genre (0 = femme, 1 = homme)
constexpr int kMemPosId     = 0x38;  // id de poste (0 = maître de guilde)
constexpr int kMemLevel     = 0x54;  // niveau de base
constexpr int kMemContrib   = 0x74;  // exp contribuée à la guilde
constexpr int kMemOnline    = 0x78;  // état de connexion (!=0 = en ligne)
constexpr int kMemLastLogin = 0x7c;  // dernière connexion (timestamp Unix)

//  Relation (ZC_MYGUILD_BASIC_INFO 0x014c) : node+8 = guildId, node+0xc = relation
//  (0 = allié, 1 = ennemi), node+0x10 = nom de la guilde (std::string).
constexpr int kRelGuildId  = 0x08;
constexpr int kRelRelation = 0x0c;
constexpr int kRelName     = 0x10;

//  Opcodes guilde (paquets bruts, envoyés comme les autres via SendPacket ; les
//  structures sont celles de moonlight/src/map/packets.hpp pour ce PACKETVER).
constexpr uint16_t kOpGuildRequest   = 0x014F;  // CZ_REQ_GUILD_MENUINTERFACE {op, type.L}
constexpr uint16_t kOpGuildLeave     = 0x0159;  // CZ_REQ_LEAVE_GUILD {op, gid, aid, cid, msg[40]}
constexpr uint16_t kOpGuildExpel     = 0x015B;  // CZ_REQ_BAN_GUILD, même forme
constexpr uint16_t kOpGuildChangePos = 0x0155;  // CZ_REQ_CHANGE_MEMBERPOS {op, len, {aid,cid,pos}*}
constexpr uint16_t kOpGuildInvite    = 0x0916;  // CZ_REQ_JOIN_GUILD2 {op, name[24]}
constexpr uint16_t kOpGuildNotice    = 0x016E;  // CZ_GUILD_NOTICE {op, gid, sujet[60], texte[120]}
constexpr uint16_t kOpGuildSetPos    = 0x0161;  // CZ_REG_CHANGE_GUILD_POSITIONINFO (var, 40 o/poste)
constexpr uint16_t kOpGuildDelRel    = 0x0183;  // CZ_REQ_DELETE_RELATED_GUILD {op, gid.L, relation.L}
constexpr uint16_t kOpGuildCreate    = 0x0165;  // CZ_REQ_MAKE_GUILD {op, cid.L, nom[24]} (30 o)
constexpr uint16_t kOpGuildCreateAck = 0x0167;  // ZC_RESULT_MAKE_GUILD {op, résultat.B}
//  Image d'emblème envoyée par le serveur (ZC_GUILD_EMBLEM_IMG). Le client natif
//  l'écrit en _tmpEmblem\<nom>_<guilde>_<version>.ebm ; on l'observe pour jeter notre
//  texture en cache. Elle n'arrive QUE sur les serveurs qui utilisent l'ancien chemin
//  (0x0153) — d'où le suivi de version, qui couvre les deux protocoles.
constexpr uint16_t kOpGuildEmblemImg = 0x0152;  // {op, len, guildId.L, emblemId.L, données}

//  Contraintes de l'emblème. Le service web accepte jusqu'à 50 ko, mais le jeu ne
//  rend que du 24x24 (UIGuildTotalInfoWnd_OnMsg vérifie les dimensions avant l'envoi)
//  et le contrôle de transparence du serveur porte sur des pixels 24 bits. Un BMP
//  24x24 24 bits fait 1782 octets : le plafond ci-dessous ne sert qu'à écarter un
//  fichier manifestement hors format.
constexpr size_t kEmblemMaxRawBytes  = 1800;  // BMP 24x24 24 bits = 1782
constexpr int    kEmblemSide         = 24;    // seule taille rendue par le client

//  Postes de guilde REÇUS (observés dans le flux : le client ne les stocke que dans
//  la fenêtre native). Toutes ces listes couvrent les MAX_GUILDPOSITION postes.
constexpr uint16_t kOpPositionNames   = 0x0166;  // ZC_POSITION_ID_NAME_INFO, 28 o/entrée
constexpr uint16_t kOpPositionInfo    = 0x0160;  // ZC_POSITION_INFO, 16 o/entrée
constexpr uint16_t kOpPositionChanged = 0x0174;  // ZC_ACK_CHANGE_GUILD_POSITIONINFO, 40 o/entrée

//  Droits d'un poste (e_guild_permission côté serveur ; masqués par GUILD_PERM_ALL).
constexpr int kGuildPermInvite  = 0x001;
constexpr int kGuildPermExpel   = 0x010;
constexpr int kGuildPermStorage = 0x100;
//  Part d'exp maximale : battle_config.guild_exp_limit (50 par défaut ; le serveur
//  clampe de toute façon).
constexpr int kGuildPayRateMax = 50;

//  Types de rafraîchissement de CZ_REQ_GUILD_MENUINTERFACE (clif_parse_GuildRequestInfo).
enum {
  kGuildReqBasic = 0, kGuildReqMembers = 1, kGuildReqPositions = 2,
  kGuildReqSkills = 3, kGuildReqBans = 4
};

//  Liste des expulsions (ZC_BAN_LIST). L'opcode ET la forme de l'entrée dépendent du
//  PACKETVER : en 20250716 c'est 0x0b7c, {charId.L, raison[40], nom[24]}.
constexpr uint16_t kOpGuildBanList = 0x0b7c;
constexpr int      kGuildBanEntry  = 68;

//  Compétences de guilde : liste reçue (variable, 37 o/entrée après [len.W][points.W]) et
//  montée de niveau. 0x0112 est le MÊME paquet que pour les skills du perso — c'est le
//  serveur qui aiguille sur guild_skillup quand l'id est dans la plage guilde (>= 10000).
constexpr uint16_t kOpGuildSkills = 0x0162;  // ZC_GUILD_SKILLINFO
constexpr uint16_t kOpSkillUp     = 0x0112;  // CZ_UPGRADE_SKILLLEVEL {op, skillId.W}
constexpr int      kGuildSkillEntry = 37;    // id.W inf.L lv.W sp.W range.W name[24] up.B

//  Chat (CZ_GlobalMessage, variable) : [op.W][longueur TOTALE.W][« nom : texte »\0].
//  ⚠ 0x00f3 a servi à autre chose sur d'anciens packetvers (déplacement vers l'entrepôt,
//  cf. storage_tweaks) ; pour 20250716 c'est bien le chat, confirmé des DEUX côtés : table
//  de longueurs du client (docs/opcode_map.csv) et clif_parse_GlobalMessage serveur.
constexpr uint16_t kOpChatMessage     = 0x00f3;
constexpr char     kCmdGuildStorage[] = "@guildstorage";
//  ⚠ Sans argument NI confirmation serveur : appelle guild_break() immédiatement, et
//  refuse si le joueur n'a pas gmaster_flag. Toute la prudence est donc côté client.
constexpr char     kCmdBreakGuild[]   = "@breakguild";

constexpr int kMaxGuildMembers = 128;  // MAX_GUILD serveur = 76 ; marge confortable

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
  char pos[32] = {0};   // nom de poste (« rang ») du joueur
  int  level = 0;
  bool master = false;
  int  guildId = 0;
  // Champs étendus (onglet Guilde) — cf. la carte des globals ci-dessus.
  char master_name[32] = {0};
  char land[32] = {0};          // territoire (« N forts »)
  char notice_subject[64] = {0};
  char notice_body[128] = {0};
  int  online = 0, member_max = 0, avg_level = 0;
  int  exp = 0, next_exp = 0;
  int  position_id = 0;         // id de poste du joueur (0 = maître)
  bool position_found = false;  // vrai si le joueur a été retrouvé dans le roster
};
bool ReadGuild(GuildInfo* g) {
  __try {
    g->guildId = *reinterpret_cast<const int*>(kGuildIdAddr);
    ReadStdStringSEH(kGuildObj, g->name, sizeof(g->name));  // CGuild+0 = nom
    if (g->guildId <= 0 || g->name[0] == '\0') return false;  // pas en guilde
    g->level   = *reinterpret_cast<const int*>(kGuildLevel);
    g->master  = *reinterpret_cast<const int*>(kGuildIsMaster) != 0;
    g->online     = *reinterpret_cast<const int*>(kGuildOnlineNum);
    g->member_max = *reinterpret_cast<const int*>(kGuildMemberMax);
    g->avg_level  = *reinterpret_cast<const int*>(kGuildAvgLevel);
    g->exp        = *reinterpret_cast<const int*>(kGuildExp);
    g->next_exp   = *reinterpret_cast<const int*>(kGuildNextExp);
    ReadStdStringSEH(kGuildMasterName, g->master_name, sizeof(g->master_name));
    ReadStdStringSEH(kGuildManageLand, g->land, sizeof(g->land));
    ReadStdStringSEH(kGuildNoticeSubj, g->notice_subject, sizeof(g->notice_subject));
    ReadStdStringSEH(kGuildNoticeBody, g->notice_body, sizeof(g->notice_body));
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
          g->position_id = *reinterpret_cast<const int*>(node + kMemPosId);
          g->position_found = true;
          break;
        }
        node = *reinterpret_cast<uint8_t* const*>(node);  // ->next
      }
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

//  ── Guilde : roster complet, relations et commandes ──────────────────────────
struct GuildMember {
  uint32_t aid = 0, cid = 0;
  char     name[32] = {0};
  char     position[32] = {0};
  int      job = 0, level = 0, position_id = 0, contribution = 0;
  int      hair = 0, hair_color = 0, sex = 0;  // apparence (miniature de tête)
  bool     online = false;
  uint32_t last_login = 0;
};
struct GuildRoster {
  GuildMember members[kMaxGuildMembers];
  int         count = 0;
};
// Parcourt la liste chaînée du roster (CGuild+0xdc) et copie chaque membre. POD +
// SEH : aucun objet à destructeur ici (contrainte C2712).
void ReadGuildRosterSEH(GuildRoster* out) {
  out->count = 0;
  __try {
    const uint8_t* sentinel =
        *reinterpret_cast<uint8_t* const*>(kGuildObj + kGuildListHead);
    if (!sentinel) return;
    const uint8_t* node = *reinterpret_cast<uint8_t* const*>(sentinel);
    for (int guard = 0; node && node != sentinel && guard < kMaxGuildMembers; ++guard) {
      GuildMember& m = out->members[out->count];
      const uintptr_t base = reinterpret_cast<uintptr_t>(node);
      m.aid          = *reinterpret_cast<const uint32_t*>(node + kMemAid);
      m.cid          = *reinterpret_cast<const uint32_t*>(node + kMemCid);
      m.job          = *reinterpret_cast<const int*>(node + kMemJob);
      m.hair         = *reinterpret_cast<const int*>(node + kMemHair);
      m.hair_color   = *reinterpret_cast<const int*>(node + kMemHairColor);
      m.sex          = *reinterpret_cast<const int*>(node + kMemSex);
      m.position_id  = *reinterpret_cast<const int*>(node + kMemPosId);
      m.level        = *reinterpret_cast<const int*>(node + kMemLevel);
      m.contribution = *reinterpret_cast<const int*>(node + kMemContrib);
      m.online       = *reinterpret_cast<const int*>(node + kMemOnline) != 0;
      m.last_login   = *reinterpret_cast<const uint32_t*>(node + kMemLastLogin);
      ReadStdStringSEH(base + kMemName, m.name, sizeof(m.name));
      ReadStdStringSEH(base + kMemPosStr, m.position, sizeof(m.position));
      if (m.aid != 0 || m.name[0]) ++out->count;
      node = *reinterpret_cast<uint8_t* const*>(node);  // ->next
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Postes connus (id -> libellé). La liste des membres (ZC_MEMBERMGR_INFO) recrée
// chaque enregistrement avec un nom de poste VIDE : seul le paquet « noms de postes »
// (ZC_POSITION_ID_NAME_INFO) le remplit, et rien ne garantit qu'il arrive après.
// On mémorise donc le dernier libellé vu pour chaque id et on s'en sert en repli,
// pour que la colonne Poste ne clignote pas à chaque rafraîchissement.
std::unordered_map<int, std::string> g_guild_position_names;
void RememberGuildPosition(int positionId, const char* label) {
  if (positionId < 0 || !label || !label[0]) return;
  g_guild_position_names[positionId] = label;
}
// Libellé à afficher : celui de l'enregistrement s'il est rempli, sinon le mémorisé,
// sinon nullptr (l'appelant affiche un tiret).
const char* GuildPositionLabel(int positionId, const char* live) {
  if (live && live[0]) return live;
  auto it = g_guild_position_names.find(positionId);
  return (it != g_guild_position_names.end()) ? it->second.c_str() : nullptr;
}

struct GuildRelation {
  int  guild_id = 0;
  int  relation = 0;  // 0 = allié, 1 = ennemi
  char name[32] = {0};
};
struct GuildRelations {
  GuildRelation entries[64];
  int           count = 0;
};
void ReadGuildRelationsSEH(GuildRelations* out) {
  out->count = 0;
  __try {
    const uint8_t* sentinel = *reinterpret_cast<uint8_t* const*>(kGuildRelHead);
    if (!sentinel) return;
    const uint8_t* node = *reinterpret_cast<uint8_t* const*>(sentinel);
    for (int guard = 0; node && node != sentinel && guard < 64; ++guard) {
      GuildRelation& r = out->entries[out->count];
      r.guild_id = *reinterpret_cast<const int*>(node + kRelGuildId);
      r.relation = *reinterpret_cast<const int*>(node + kRelRelation);
      ReadStdStringSEH(reinterpret_cast<uintptr_t>(node) + kRelName, r.name, sizeof(r.name));
      if (r.name[0]) ++out->count;
      node = *reinterpret_cast<uint8_t* const*>(node);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Demande au serveur un rafraîchissement (infos de base, liste des membres…).
void SendGuildRequest(int type) {
  uint8_t pkt[6];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpGuildRequest;
  *reinterpret_cast<uint32_t*>(pkt + 2) = static_cast<uint32_t>(type);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}
// Monte d'un niveau une compétence (de guilde ici) : CZ_UPGRADE_SKILLLEVEL.
void SendSkillUp(uint16_t skillId) {
  uint8_t pkt[4];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpSkillUp;
  *reinterpret_cast<uint16_t*>(pkt + 2) = skillId;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}
// Quitter la guilde / expulser un membre : MÊME forme de paquet (54 o), seul
// l'opcode change. `reason` est tronqué à 39 caractères + terminateur.
void SendGuildLeaveOrExpel(uint16_t opcode, int guildId, uint32_t aid, uint32_t cid,
                           const char* reason) {
  uint8_t pkt[54];
  std::memset(pkt, 0, sizeof(pkt));
  *reinterpret_cast<uint16_t*>(pkt + 0)  = opcode;
  *reinterpret_cast<uint32_t*>(pkt + 2)  = static_cast<uint32_t>(guildId);
  *reinterpret_cast<uint32_t*>(pkt + 6)  = aid;
  *reinterpret_cast<uint32_t*>(pkt + 10) = cid;
  if (reason && reason[0])
    std::strncpy(reinterpret_cast<char*>(pkt + 14), reason, 39);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}
// Change le poste d'UN membre. position 0 est INTERDIT ici : côté serveur il
// déclenche guild_gm_change (transfert de la direction de la guilde), qui n'a rien
// à faire dans un menu contextuel.
void SendGuildChangePosition(uint32_t aid, uint32_t cid, int positionId) {
  if (positionId <= 0) return;
  uint8_t pkt[16];
  *reinterpret_cast<uint16_t*>(pkt + 0)  = kOpGuildChangePos;
  *reinterpret_cast<uint16_t*>(pkt + 2)  = 16;  // longueur totale (en-tête 4 + 1 entrée de 12)
  *reinterpret_cast<uint32_t*>(pkt + 4)  = aid;
  *reinterpret_cast<uint32_t*>(pkt + 8)  = cid;
  *reinterpret_cast<uint32_t*>(pkt + 12) = static_cast<uint32_t>(positionId);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}
// Crée une guilde. Le serveur ignore le char id transmis (il utilise la session) mais
// le natif le remplit : on fait pareil. Il exige un Emperium en inventaire
// (battle_config.guild_emperium_check) et refuse sur une carte « guildlock » ; la
// réponse arrive en ZC_RESULT_MAKE_GUILD (0x0167).
void SendCreateGuild(const char* guildName) {
  if (!guildName || !guildName[0]) return;
  uint8_t pkt[30];
  std::memset(pkt, 0, sizeof(pkt));
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpGuildCreate;
  *reinterpret_cast<uint32_t*>(pkt + 2) = static_cast<uint32_t>(ReadInt(kOwnCharId));
  std::strncpy(reinterpret_cast<char*>(pkt + 6), guildName, 23);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// Rompt une alliance (relation 0) ou une hostilité (relation 1) avec une autre guilde.
// Réservé au maître côté serveur, refusé pendant la WoE et sur carte « guildlock » ;
// la réponse ZC_DELETE_RELATED_GUILD (0x0184) retire l'entrée de la liste du client.
void SendGuildDeleteRelation(int otherGuildId, int relation) {
  if (otherGuildId <= 0) return;
  uint8_t pkt[10];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpGuildDelRel;
  *reinterpret_cast<uint32_t*>(pkt + 2) = static_cast<uint32_t>(otherGuildId);
  *reinterpret_cast<uint32_t*>(pkt + 6) = static_cast<uint32_t>(relation);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// Invite un joueur PAR SON NOM (le serveur résout le nick ; le joueur doit être en ligne).
void SendGuildInvite(const char* charName) {
  if (!charName || !charName[0]) return;
  uint8_t pkt[26];
  std::memset(pkt, 0, sizeof(pkt));
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpGuildInvite;
  std::strncpy(reinterpret_cast<char*>(pkt + 2), charName, 23);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}
// Une entrée de CZ_REG_CHANGE_GUILD_POSITIONINFO, telle qu'elle part sur le fil :
// {id.L, droits.L, rang.L, part d'exp.L, nom[24]} = 40 octets. Le serveur ignore
// `ranking` (il relit la part d'exp en +12) mais le natif y remet l'id : on fait pareil.
#pragma pack(push, 1)
struct GuildPositionWire {
  int32_t id;
  int32_t mode;
  int32_t ranking;
  int32_t pay_rate;
  char    name[24];
};
#pragma pack(pop)
static_assert(sizeof(GuildPositionWire) == 40, "entrée de poste = 40 octets");
// Envoie les postes MODIFIÉS (le serveur applique chaque entrée telle quelle et
// rediffuse un ZC 0x0174 ; il exige le drapeau maître de guilde).
void SendGuildPositions(const GuildPositionWire* rows, int count) {
  if (!rows || count <= 0) return;
  uint8_t pkt[4 + 20 * sizeof(GuildPositionWire)];
  const int total = 4 + count * static_cast<int>(sizeof(GuildPositionWire));
  if (total > static_cast<int>(sizeof(pkt))) return;
  std::memset(pkt, 0, sizeof(pkt));
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpGuildSetPos;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(total);
  std::memcpy(pkt + 4, rows, count * sizeof(GuildPositionWire));
  Bourgeon::Instance().SendPacket(pkt, static_cast<size_t>(total));
}

// Met à jour l'annonce de guilde (réservé au maître côté serveur).
void SendGuildNotice(int guildId, const char* subject, const char* body) {
  uint8_t pkt[186];
  std::memset(pkt, 0, sizeof(pkt));
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpGuildNotice;
  *reinterpret_cast<uint32_t*>(pkt + 2) = static_cast<uint32_t>(guildId);
  if (subject) std::strncpy(reinterpret_cast<char*>(pkt + 6), subject, 59);
  if (body)    std::strncpy(reinterpret_cast<char*>(pkt + 66), body, 119);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
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

// Nom de classe d'un job id ARBITRAIRE (membres de guilde) : même résolveur natif
// que ClassNameSEH, qui prend le job en paramètre. Cache local (le résolveur passe
// par la table Lua des classes).
std::unordered_map<int, std::string> g_job_name_cache;
const char* JobNameSEH(int jobId) {
  __try {
    using GetClassName_t = const char* (__fastcall*)(void*, void*, unsigned, int);
    const char* n = reinterpret_cast<GetClassName_t>(0x00d5bb40)(
        reinterpret_cast<void*>(kSession), nullptr, static_cast<unsigned>(jobId), -1);
    return n ? n : "";
  } __except (EXCEPTION_EXECUTE_HANDLER) { return ""; }
}
const char* JobName(int jobId) {
  auto it = g_job_name_cache.find(jobId);
  if (it != g_job_name_cache.end()) return it->second.c_str();
  const char* n = JobNameSEH(jobId);
  char buf[64];
  if (n && n[0]) { std::strncpy(buf, n, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0'; }
  else           std::snprintf(buf, sizeof(buf), "Classe %d", jobId);
  return (g_job_name_cache[jobId] = buf).c_str();
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

// Filtre d'échantillonnage des icônes du grimoire. Le natif ne filtre RIEN (les
// .bmp d'icônes font 24 px et sont blités tels quels) ; agrandies à 40 px dans la
// grille, elles restent donc crénelées. Le lissage est un OPT-IN, posé par un
// callback de draw list comme la barre de raccourcis (cf. skill_bar_tweaks).
// ⚠ Restaurer POINT après coup, pas ImDrawCallback_ResetRenderState : le backend
// DX9 y remet LINEAR, ce qui ramollirait les blits de skin dessinés ensuite.
bool g_skill_icon_bilinear = false;
void CbSkillIconFilter(const ImDrawList*, const ImDrawCmd*) {
  Overlay_SetTextureFilter(g_skill_icon_bilinear);
}
void CbSkillIconFilterOff(const ImDrawList*, const ImDrawCmd*) {
  Overlay_SetTextureFilter(false);
}

// ═══ Grimoire (arbre de compétences) ════════════════════════════════════════
// La fenêtre native 0x25 ne POSSÈDE rien : elle recopie ses quatre listes d'onglets
// depuis CPlayerSkillBundle, un objet global (= session+0x0C). On lit donc la MÊME
// source qu'elle, ce qui rend l'onglet Grimoire indépendant du natif — fenêtre
// fermée ou masquée, les données sont là. Détail complet du modèle et de chaque
// offset : docs/skill_tree_re.md, partie II.
constexpr uintptr_t kSkillBundle      = 0x015fa3cc;  // CPlayerSkillBundle (session+0x0C)
constexpr uintptr_t kSkillFlatList    = 0x015fa3e0;  // bundle+0x14 : l'onglet « divers »
constexpr uintptr_t kSkillGetTabList  = 0x00738370;  // __thiscall(bundle, tab) -> std::list*
constexpr uintptr_t kSkillSetUseLevel = 0x00738570;  // __thiscall(bundle, id, lv)
constexpr uintptr_t kSkillGetUseLevel = 0x00d5e3c0;  // __thiscall(SESSION, id) — même tableau
constexpr uintptr_t kIsLevelUseSkill  = 0x0073adb0;  // __cdecl(id) : l'effet dépend-il du niveau ?
constexpr uintptr_t kSkillPointsAddr  = 0x015fb9fc;  // g_Own_SkillPoints
using GetTabList_t  = void* (__fastcall*)(void*, void*, int);
using SetUseLevel_t = void  (__fastcall*)(void*, void*, int, int);
using GetUseLevel_t = int   (__fastcall*)(void*, void*, int);
using IsLevelUse_t  = char  (__cdecl*)(int);

// Offsets dans CSkillInfo. ⚠ Les nœuds sont des std::list MSVC {next, prev, valeur} :
// la VALEUR commence au nœud + 8.
constexpr int kSkNodeValue    = 0x08;
constexpr int kSkOffValid     = 0x04;  // 1 = fiche utilisable
constexpr int kSkOffId        = 0x08;
constexpr int kSkOffInf       = 0x0c;  // masque skill_get_inf ; 0 = passive
constexpr int kSkOffLvLocal   = 0x10;  // niveau appris (la vue compacte native le bidouille)
constexpr int kSkOffSp        = 0x14;  // coût SP au niveau courant
constexpr int kSkOffUpgrade   = 0x18;  // le serveur dit « il reste du niveau »
constexpr int kSkOffRange     = 0x1c;
constexpr int kSkOffPos       = 0x24;  // index de case dans la grille (-1 = non placée)
constexpr int kSkOffMaxLv     = 0x28;  // niveau MAX (source : Lua)
constexpr int kSkOffUserUp    = 0x2c;  // UserUpgradable : <= 0 = le joueur ne peut pas la monter
constexpr int kSkOffLearned   = 0x30;  // int16, VÉRITÉ SERVEUR (jamais modifiée en local)
constexpr int kSkOffNeedVec   = 0x38;  // std::vector<{u32 id, u32 niveau}> = prérequis

// Fenêtre native du grimoire (UINewSkillListWnd id 0x25) : le gestionnaire garde son
// instance à mgr+0x2C4 et VIDE cet emplacement à la fermeture — c'est donc la source
// fiable de « le joueur a-t-il demandé le grimoire ? ». +0x28 = drapeau de visibilité.
constexpr uintptr_t kSkillWndSlot   = 0x0131f7ac;  // mgr(0x0131f4e8)+0x2C4
constexpr int       kWndVisibleFlag = 0x28;

constexpr int kSkillJobTabs  = 4;    // onglets de job ; le 5e (« divers ») = liste plate
constexpr int kSkillGridCols = 7;    // la grille native fait 7 x 6 = 42 cases
constexpr int kSkillMaxNodes = 256;  // garde-fou de parcours
constexpr int kSkillMaxNeed  = 6;    // prérequis retenus par compétence (le jeu en a <= 3)

// Fiche lue, POD pur : remplie sous SEH, consommée à l'extérieur.
struct SkillRaw {
  int id, inf, learned, sp, range, pos, maxlv, user_up, upgradable;
  int need_count;
  int need_id[kSkillMaxNeed], need_lv[kSkillMaxNeed];
};

// Parcourt la liste d'un onglet ; `tab < 0` = la liste plate (onglet « divers »).
// POD only (C2712) et SEH : un nœud abîmé arrête le parcours sans perdre les
// précédents, exactement comme le fait l'extracteur d'inventaire.
int ReadSkillTabSEH(int tab, SkillRaw* out, int cap) {
  int n = 0;
  __try {
    uint8_t* list_obj = reinterpret_cast<uint8_t*>(kSkillFlatList);
    if (tab >= 0)
      list_obj = reinterpret_cast<uint8_t*>(reinterpret_cast<GetTabList_t>(kSkillGetTabList)(
          reinterpret_cast<void*>(kSkillBundle), nullptr, tab));
    if (!list_obj) return 0;
    uint8_t* head = *reinterpret_cast<uint8_t**>(list_obj);
    if (!head) return 0;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head);
    int guard = 0;
    while (node && node != head && n < cap && guard++ < kSkillMaxNodes) {
      const uint8_t* v = node + kSkNodeValue;
      node = *reinterpret_cast<uint8_t**>(node);  // avancer AVANT de lire la valeur
      if (*reinterpret_cast<const int*>(v + kSkOffValid) == 0) continue;
      const int id = *reinterpret_cast<const int*>(v + kSkOffId);
      if (id <= 0) continue;
      SkillRaw& s = out[n];
      s.id         = id;
      s.inf        = *reinterpret_cast<const int*>(v + kSkOffInf);
      s.sp         = *reinterpret_cast<const int*>(v + kSkOffSp);
      s.range      = *reinterpret_cast<const int*>(v + kSkOffRange);
      s.pos        = *reinterpret_cast<const int*>(v + kSkOffPos);
      s.maxlv      = *reinterpret_cast<const int*>(v + kSkOffMaxLv);
      s.user_up    = *reinterpret_cast<const int*>(v + kSkOffUserUp);
      s.upgradable = *reinterpret_cast<const int*>(v + kSkOffUpgrade);
      // Niveau appris : +0x30 fait foi (le serveur), +0x10 sert de repli.
      s.learned    = *reinterpret_cast<const int16_t*>(v + kSkOffLearned);
      if (s.learned <= 0) s.learned = *reinterpret_cast<const int*>(v + kSkOffLvLocal);
      s.need_count = 0;
      const uint8_t* first = *reinterpret_cast<const uint8_t* const*>(v + kSkOffNeedVec);
      const uint8_t* last  = *reinterpret_cast<const uint8_t* const*>(v + kSkOffNeedVec + 4);
      for (const uint8_t* p = first; p && p + 8 <= last && s.need_count < kSkillMaxNeed;
           p += 8) {
        s.need_id[s.need_count] = *reinterpret_cast<const int*>(p);
        s.need_lv[s.need_count] = *reinterpret_cast<const int*>(p + 4);
        ++s.need_count;
      }
      ++n;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return n;
}

// Points de compétence restants (globale de session, la même que lit le natif).
int SkillPointsSEH() {
  __try { return *reinterpret_cast<const int*>(kSkillPointsAddr); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// « Niveau d'utilisation » : le niveau auquel la compétence part quand on la lance.
// Purement CLIENT (persisté dans le JSON du personnage), et c'est ce que la barre de
// raccourcis envoie. Écriture par le bundle, lecture par la session : c'est le même
// tableau (bundle+0x44 == session+0x50), pas une incohérence.
int GetUseLevelSEH(int skillId) {
  __try {
    return reinterpret_cast<GetUseLevel_t>(kSkillGetUseLevel)(
        reinterpret_cast<void*>(kSession), nullptr, skillId);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void SetUseLevelSEH(int skillId, int level) {
  __try {
    reinterpret_cast<SetUseLevel_t>(kSkillSetUseLevel)(
        reinterpret_cast<void*>(kSkillBundle), nullptr, skillId, level);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// L'effet de la compétence dépend-il du niveau ? Le natif n'affiche « n / m » que
// dans ce cas (sinon un seul nombre) : même règle ici.
bool IsLevelUseSkillSEH(int skillId) {
  __try { return reinterpret_cast<IsLevelUse_t>(kIsLevelUseSkill)(skillId) != 0; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// (Les noms d'onglets viennent du Lua : ReadSkillTabNamesSEH est plus bas, après les
//  constantes de l'API Lua brute dont il dépend.)

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
// Décode un BMP d'emblème (24x24) en texture ImGui. 24-bit et 8-bit palettisé : le
// premier est ce que produit l'éditeur d'emblème habituel, le second ce que rendent
// beaucoup de convertisseurs — et le serveur accepte les deux (clif_validate_emblem
// ne regarde que l'en-tête BMP). Le magenta pur devient transparent, comme dans le jeu.
ro::IconTex DecodeEmblemBmp(const uint8_t* bmp, size_t size) {
  if (size < 54 || bmp[0] != 'B' || bmp[1] != 'M') return {};
  const int32_t  w       = *reinterpret_cast<const int32_t*>(bmp + 0x12);
  const int32_t  hraw    = *reinterpret_cast<const int32_t*>(bmp + 0x16);
  const int16_t  bpp     = *reinterpret_cast<const int16_t*>(bmp + 0x1c);
  const uint32_t dataOff = *reinterpret_cast<const uint32_t*>(bmp + 0x0a);
  const int  h        = (hraw < 0) ? -hraw : hraw;
  const bool bottomUp = hraw > 0;  // BMP standard = bottom-up
  if (w <= 0 || w > 64 || h <= 0 || h > 64) return {};
  if (bpp != 24 && bpp != 8) return {};
  // Palette (8-bit) : BGRA0 x 256, juste après l'en-tête d'info de 40 octets.
  const uint8_t* palette = nullptr;
  if (bpp == 8) {
    if (size < 54u + 256u * 4u) return {};
    palette = bmp + 54;
  }
  const size_t rowSize = static_cast<size_t>((w * (bpp / 8) + 3) & ~3);  // lignes alignées 4
  if (static_cast<size_t>(dataOff) + rowSize * h > size) return {};
  std::vector<uint8_t> argb(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    const int srcY = bottomUp ? (h - 1 - y) : y;
    const uint8_t* row = bmp + dataOff + static_cast<size_t>(srcY) * rowSize;
    for (int x = 0; x < w; ++x) {
      uint8_t b, g, r;
      if (bpp == 24) {
        b = row[x * 3]; g = row[x * 3 + 1]; r = row[x * 3 + 2];
      } else {
        const uint8_t* entry = palette + static_cast<size_t>(row[x]) * 4;
        b = entry[0]; g = entry[1]; r = entry[2];
      }
      const bool ck = (r == 0xFF && g == 0 && b == 0xFF);  // magenta -> transparent
      const size_t o = (static_cast<size_t>(y) * w + x) * 4;
      argb[o] = b; argb[o + 1] = g; argb[o + 2] = r; argb[o + 3] = ck ? 0 : 0xFF;
    }
  }
  return {Overlay_CreateTextureARGB(argb.data(), w, h), w, h};
}
// Lit un fichier entier (petit : emblèmes et .ebm). Vide si absent ou trop gros.
std::vector<uint8_t> ReadWholeFile(const char* fullPath, long maxBytes) {
  std::vector<uint8_t> out;
  FILE* fp = nullptr;
  if (fopen_s(&fp, fullPath, "rb") != 0 || !fp) return out;
  std::fseek(fp, 0, SEEK_END);
  const long fsz = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  if (fsz <= 2 || fsz > maxBytes) { std::fclose(fp); return out; }
  out.resize(static_cast<size_t>(fsz));
  const size_t rd = std::fread(out.data(), 1, out.size(), fp);
  std::fclose(fp);
  if (rd != out.size()) out.clear();
  return out;
}
// Lit un .ebm (BMP 24x24 compressé zlib) et le convertit en texture ImGui.
ro::IconTex LoadEmblemFromFile(const char* fullPath) {
  const std::vector<uint8_t> comp = ReadWholeFile(fullPath, 1 << 20);  // pas encore téléchargé
  if (comp.empty()) return {};
  std::vector<uint8_t> bmp;
  if (!tinf::zlib_uncompress(comp.data(), comp.size(), bmp)) return {};
  return DecodeEmblemBmp(bmp.data(), bmp.size());
}
// Cache d'emblèmes par guilde. Hors de ResolveEmblem : après un changement d'emblème
// il faut pouvoir jeter l'entrée pour que le nouveau .ebm soit relu (le nom du fichier
// change à chaque version, mais la texture déjà chargée, elle, ne se périme pas seule).
struct EmblemCacheEntry { ro::IconTex tex; DWORD lastTry = 0; int version = -1; };
std::unordered_map<int, EmblemCacheEntry> g_emblem_cache;

// Version d'emblème connue du gestionnaire natif (map guildId -> version, remplie
// quelle que soit la voie empruntée : image reçue en ZC 0x0152 comme téléchargement
// par le service web). C'est le seul indicateur fiable qu'un emblème a changé —
// guetter un paquet précis raterait l'autre chemin.
constexpr uintptr_t kGetEmblemVersion = 0x0061d560;  // __thiscall(this, guildId) -> version
using GetEmblemVersion_t = int(__thiscall*)(void*, unsigned);
int EmblemVersionSEH(int guildId) {
  __try {
    void* mgr = *reinterpret_cast<void* const*>(kCGuildMgrPtr);
    if (!mgr) return 0;
    return reinterpret_cast<GetEmblemVersion_t>(kGetEmblemVersion)(
        mgr, static_cast<unsigned>(guildId));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

ro::IconTex ResolveEmblem(int guildId) {
  static unsigned s_epoch = 0;
  const unsigned e = Overlay_DeviceEpoch();
  if (e != s_epoch) { g_emblem_cache.clear(); s_epoch = e; }
  if (guildId <= 0) return {};
  EmblemCacheEntry& en = g_emblem_cache[guildId];
  // Nouvelle version côté jeu = notre texture est périmée, quel que soit le chemin
  // par lequel l'emblème est arrivé.
  const int live_version = EmblemVersionSEH(guildId);
  if (live_version != en.version) {
    en.version = live_version;
    en.tex = {};
    en.lastTry = 0;
  }
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
void ForgetEmblem(int guildId) { g_emblem_cache.erase(guildId); }

// ── Changement d'emblème ─────────────────────────────────────────────────────
// Le chemin historique (CZ 0x0153, BMP compressé zlib) est UN CUL-DE-SAC sur ce
// serveur : le paquet part bien (vérifié octet par octet dans le journal) mais rien
// ne se passe, alors que la fenêtre native change l'emblème aussitôt. Or elle
// n'envoie PAS 0x0153 : elle passe par le service web du serveur
// (CEmblemDataMgr_RequestUpload 0x005c8950 -> POST multipart), après quoi le client
// notifie lui-même le map-server. On emprunte donc exactement ce chemin — cf.
// RequestEmblemUploadSEH plus bas. RE + essais en jeu 2026-07-26.

// Taux de transparence tel que le SERVEUR le calcule (clif_validate_emblem) : il
// compte les triplets de pixels magenta consécutifs et compare
// `transcount * 300 / taille_des_pixels` à `inter_config.emblem_transparency_limit`.
// La formule est approximative côté serveur ; on la reproduit telle quelle, sinon
// l'avertissement ne correspondrait pas au verdict réel.
// Le service web applique EXACTEMENT le même contrôle que le map-server
// (emblem_controller.cpp reprend la formule mot pour mot) : l'avertissement reste
// valable maintenant que l'envoi passe par le web.
constexpr int kEmblemTransparencyWarn = 80;  // valeur de conf/inter_athena.conf (moonlight)
int EmblemTransparencyPercent(const std::vector<uint8_t>& bmp) {
  if (bmp.size() < 54) return 0;
  const uint32_t offset = *reinterpret_cast<const uint32_t*>(&bmp[0x0a]);
  if (offset >= bmp.size()) return 0;
  int transcount = 1;
  int32_t window[3] = {0, 0, 0};
  for (size_t i = offset; i + 4 <= bmp.size(); ++i) {
    const int slot = static_cast<int>(i % 3);  // indexé sur i ABSOLU, comme le serveur
    window[slot] = *reinterpret_cast<const int32_t*>(&bmp[i]);
    if (slot == 2 && window[0] == static_cast<int32_t>(0xFFFF00FF) && window[1] == 0xFFFF00 &&
        window[2] == static_cast<int32_t>(0xFF00FFFF))
      ++transcount;
  }
  return (transcount * 300) / static_cast<int>(bmp.size() - offset);
}

// Contrôles locaux sur un BMP candidat, dans l'ordre où le serveur (ou le jeu) le
// refuserait. `why` reçoit le motif exact — un emblème rejeté en silence par le
// serveur est indiscernable d'un serveur muet.
bool EmblemBmpIsUsable(const std::vector<uint8_t>& bmp, std::string* why) {
  auto fail = [why](const char* text) { if (why) *why = text; return false; };
  if (bmp.size() < 54) return fail("Fichier trop court pour un BMP.");
  if (bmp[0] != 'B' || bmp[1] != 'M') return fail("Ce n'est pas un BMP (signature « BM » absente).");
  const uint32_t declared = *reinterpret_cast<const uint32_t*>(&bmp[2]);
  // Le serveur compare bfSize à la taille réellement reçue : un en-tête menteur
  // (fréquent après une conversion) est rejeté sans le moindre message en jeu.
  if (declared != bmp.size()) return fail("En-tête BMP incohérent (taille déclarée ≠ taille du fichier).");
  const uint32_t dataOff = *reinterpret_cast<const uint32_t*>(&bmp[0x0a]);
  if (dataOff >= bmp.size()) return fail("En-tête BMP incohérent (offset des pixels hors fichier).");
  const int32_t w    = *reinterpret_cast<const int32_t*>(&bmp[0x12]);
  const int32_t hraw = *reinterpret_cast<const int32_t*>(&bmp[0x16]);
  const int32_t h    = (hraw < 0) ? -hraw : hraw;
  if (w != kEmblemSide || h != kEmblemSide) {
    if (why) {
      char text[96];
      std::snprintf(text, sizeof(text), "Dimensions %ldx%ld : le jeu n'affiche que du %dx%d.",
                    static_cast<long>(w), static_cast<long>(h), kEmblemSide, kEmblemSide);
      *why = text;
    }
    return false;
  }
  const int16_t bpp = *reinterpret_cast<const int16_t*>(&bmp[0x1c]);
  if (bpp != 24 && bpp != 8) return fail("Profondeur non gérée : utilise du 24 bits ou du 256 couleurs.");
  const uint32_t compression = *reinterpret_cast<const uint32_t*>(&bmp[0x1e]);
  if (compression != 0) return fail("BMP compressé (RLE) : enregistre-le sans compression.");
  if (bmp.size() > kEmblemMaxRawBytes) {
    if (why) {
      char text[96];
      std::snprintf(text, sizeof(text), "Fichier de %zu octets : le serveur en accepte %zu au plus.",
                    bmp.size(), kEmblemMaxRawBytes);
      *why = text;
    }
    return false;
  }
  if (why) why->clear();
  return true;
}

// ── Envoi par le chemin NATIF (service web) ──────────────────────────────────
// C'est ce que fait le bouton « Emblem » de la fenêtre de guilde : pas de paquet
// 0x0153, mais un POST multipart vers le service web (AID/AuthToken/WorldName/GDID/
// ImgType/IMG), après quoi le client notifie lui-même le map-server de la nouvelle
// version. Vérifié en jeu : ce chemin fonctionne là où 0x0153 reste sans effet.
//
// On appelle donc la MÊME fonction que la fenêtre native — token d'authentification,
// nom de monde et URL du service sont déjà dans le manager, rien à reconstituer.
// UIGuildTotalInfoWnd_OnMsg (case 39) fait exactement : RequestUpload(guildId,
// std::string("emblem\\<fichier>")). Le fichier doit exister dans <jeu>\emblem\.
constexpr uintptr_t kEmblemDataMgrPtr    = 0x012517b8;  // *ptr = CEmblemDataMgr
constexpr uintptr_t kEmblemRequestUpload = 0x005c8950;  // __thiscall(this, guildId, std::string)
constexpr uintptr_t kStdStringFromFmt    = 0x00a94930;  // (dst, fmt, …) -> std::string du jeu
// std::string MSVC telle que la passe le natif : 16 octets de SSO, taille, capacité.
// Construite PAR LE JEU (kStdStringFromFmt) pour que son allocateur soit le bon : le
// callee la détruit lui-même en sortie.
struct MsvcString24 { uint8_t raw[24]; };
using StrFromFmt_t    = void*(__cdecl*)(void*, const char*, ...);
using RequestUpload_t = bool(__thiscall*)(void*, int, MsvcString24);
bool RequestEmblemUploadSEH(int guildId, const char* fileName) {
  __try {
    void* mgr = *reinterpret_cast<void* const*>(kEmblemDataMgrPtr);
    if (!mgr || guildId <= 0 || !fileName || !fileName[0]) return false;
    MsvcString24 path;
    std::memset(&path, 0, sizeof(path));
    reinterpret_cast<StrFromFmt_t>(kStdStringFromFmt)(&path, "emblem\\%s", fileName);
    // Renvoie false quand un envoi est DÉJÀ en cours (le manager n'en accepte qu'un).
    return reinterpret_cast<RequestUpload_t>(kEmblemRequestUpload)(mgr, guildId, path);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Écrit le BMP dans <jeu>\emblem\<nom>.bmp — l'upload natif prend un CHEMIN, pas des
// octets : le fichier doit être sur le disque avant l'appel.
bool WriteEmblemFile(const char* fileName, const std::vector<uint8_t>& bmp) {
  if (!fileName || !fileName[0] || bmp.empty()) return false;
  CreateDirectoryA(paths::InGameDir("emblem").c_str(), nullptr);
  const std::string full = paths::InGameDir("emblem\\") + fileName;
  FILE* fp = nullptr;
  if (fopen_s(&fp, full.c_str(), "wb") != 0 || !fp) return false;
  const size_t written = std::fwrite(bmp.data(), 1, bmp.size(), fp);
  std::fclose(fp);
  return written == bmp.size();
}

// Un .bmp du dossier <jeu>\emblem\, prêt à être présenté dans le modal.
struct EmblemCandidate {
  std::string name;          // nom de fichier seul
  std::vector<uint8_t> bmp;  // contenu brut (petit : 1782 octets au plus)
  ro::IconTex preview;       // aperçu, ou texture nulle si indécodable
  bool        usable = false;
  std::string why;           // motif de refus quand !usable
  int         transparency = 0;  // % de magenta, au sens du contrôle serveur
};
std::vector<EmblemCandidate> g_emblem_files;
// Aperçus gardés PAR NOM DE FICHIER, pour qu'un re-scan ne recrée pas une texture à
// chaque fois (rien ne les libère). Vidé au changement de device, comme tous les
// caches de textures du plugin, sinon on dessine des handles morts.
std::unordered_map<std::string, ro::IconTex> g_emblem_preview_cache;
ro::IconTex EmblemPreview(const std::string& name, const std::vector<uint8_t>& bmp) {
  static unsigned s_epoch = 0;
  const unsigned e = Overlay_DeviceEpoch();
  if (e != s_epoch) { g_emblem_preview_cache.clear(); s_epoch = e; }
  auto it = g_emblem_preview_cache.find(name);
  if (it != g_emblem_preview_cache.end()) return it->second;
  return g_emblem_preview_cache[name] = DecodeEmblemBmp(bmp.data(), bmp.size());
}

// Scanne <jeu>\emblem\*.bmp — le même dossier que la fenêtre native, qui y cherche
// aussi des .gif (réservés au service web, inutilisables par ce chemin : ignorés).
void ScanEmblemFolder() {
  g_emblem_files.clear();
  const std::string dir = paths::InGameDir("emblem\\");
  WIN32_FIND_DATAA fd{};
  HANDLE h = FindFirstFileA((dir + "*.bmp").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    EmblemCandidate cand;
    cand.name = fd.cFileName;
    cand.bmp = ReadWholeFile((dir + cand.name).c_str(), 1 << 16);
    if (cand.bmp.empty()) {
      cand.why = "Fichier illisible ou vide.";
    } else {
      cand.usable = EmblemBmpIsUsable(cand.bmp, &cand.why);
      cand.preview = EmblemPreview(cand.name, cand.bmp);
      cand.transparency = EmblemTransparencyPercent(cand.bmp);
    }
    g_emblem_files.push_back(std::move(cand));
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  std::sort(g_emblem_files.begin(), g_emblem_files.end(),
            [](const EmblemCandidate& a, const EmblemCandidate& b) {
              return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
            });
}

// ── Éditeur d'emblème (canvas 24x24 dessiné en jeu) ──────────────────────────
// Pas de fichier à préparer : on peint les 576 pixels, on fabrique le BMP 24 bits
// en mémoire et on l'envoie par le même chemin que les fichiers du dossier. Le
// magenta pur est la couleur « vide » — c'est la teinte que le jeu rend transparente.
constexpr uint32_t kEmblemClear = 0xFF00FFu;  // magenta pur = transparent en jeu
constexpr int kEmblemPixels = kEmblemSide * kEmblemSide;

// Outils. Les trois premiers peignent au fil du geste, les trois suivants se tirent
// d'un point à l'autre et ne s'appliquent qu'au relâchement (aperçu entre-temps).
enum {
  kToolPencil = 0,
  kToolEraser,
  kToolFill,
  kToolLine,
  kToolRect,
  kToolEllipse,
};

struct EmblemCanvas {
  uint32_t pixel[kEmblemPixels];   // 0xRRGGBB, kEmblemClear = transparent
  bool     started = false;        // canvas déjà initialisé (sinon : tout vide)
  int      tool = kToolPencil;     // cf. kToolPencil…kToolEllipse
  int      brush = 1;              // épaisseur du trait (1..3)
  bool     mirror = false;         // symétrie gauche/droite pendant le tracé
  bool     filled = false;         // rectangle / ellipse pleins plutôt qu'en contour
  float    color[3] = {0.85f, 0.15f, 0.15f};
  // La transparence est une COULEUR, pas un outil : sans ça, « remplir de vide » ou
  // « tracer une forme vide » obligeraient à passer par la gomme, qui ne sait que
  // peindre à main levée.
  bool     color_clear = false;
  bool     stroke_open = false;    // un coup de souris est en cours (pour l'annulation)
  int      last_x = -1, last_y = -1;  // dernière cellule peinte (pour relier le trait)
  // Forme en cours de tirage : rien n'est peint tant que le bouton n'est pas relâché.
  bool     shape_active = false;
  int      shape_x0 = 0, shape_y0 = 0;
  bool     shape_erase = false;    // tirée au clic droit = efface
  int      revision = 0;           // incrémenté à chaque modification (cache du BMP)
  std::vector<std::vector<uint32_t>> undo;  // états précédents (plafonnés)
  char     save_name[32] = "mon_embleme";
};
EmblemCanvas g_emblem_canvas;

uint32_t PackColor(const float rgb[3]) {
  auto to8 = [](float v) {
    return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
  };
  return (to8(rgb[0]) << 16) | (to8(rgb[1]) << 8) | to8(rgb[2]);
}

// Couleur qu'appliquent le crayon, le remplissage et les formes : la teinte choisie,
// ou le « vide » quand la transparence est la couleur courante.
uint32_t CurrentInk() {
  return g_emblem_canvas.color_clear ? kEmblemClear : PackColor(g_emblem_canvas.color);
}

void EmblemCanvasClear() {
  for (int i = 0; i < kEmblemPixels; ++i) g_emblem_canvas.pixel[i] = kEmblemClear;
  g_emblem_canvas.started = true;
  ++g_emblem_canvas.revision;
}

// Empile l'état courant avant un coup de pinceau : « Annuler » remonte coup par coup
// (et non pixel par pixel), ce qui est le comportement attendu d'un éditeur.
void EmblemCanvasPushUndo() {
  if (!g_emblem_canvas.started) return;
  g_emblem_canvas.undo.emplace_back(g_emblem_canvas.pixel, g_emblem_canvas.pixel + kEmblemPixels);
  if (g_emblem_canvas.undo.size() > 40) g_emblem_canvas.undo.erase(g_emblem_canvas.undo.begin());
}

// Étendue du pinceau autour de la cellule pointée, pour que « N px » dessine bien un
// carré de N pixels de côté : un rayon symétrique donnerait 2N-1 (1, 3, 5…), ce qui
// ne correspond plus au réglage dès qu'il dépasse 1. Pour une épaisseur PAIRE le
// carré ne peut pas être centré : il déborde d'un pixel vers la droite et le bas.
void BrushExtent(int* lo, int* hi) {
  const int size = std::clamp(g_emblem_canvas.brush, 1, 3);
  *lo = -((size - 1) / 2);
  *hi = size / 2;
}

void EmblemCanvasPaint(int cx, int cy, uint32_t color) {
  ++g_emblem_canvas.revision;
  int lo = 0, hi = 0;
  BrushExtent(&lo, &hi);
  for (int dy = lo; dy <= hi; ++dy) {
    for (int dx = lo; dx <= hi; ++dx) {
      const int x = cx + dx, y = cy + dy;
      if (x < 0 || x >= kEmblemSide || y < 0 || y >= kEmblemSide) continue;
      g_emblem_canvas.pixel[y * kEmblemSide + x] = color;
      if (g_emblem_canvas.mirror) {
        const int mx = kEmblemSide - 1 - x;
        g_emblem_canvas.pixel[y * kEmblemSide + mx] = color;
      }
    }
  }
}

// Relie deux cellules (Bresenham) : à 60 images/s la souris saute plusieurs pixels
// entre deux frames, et un trait rapide laisserait sinon des pointillés.
void EmblemCanvasStroke(int x0, int y0, int x1, int y1, uint32_t color) {
  int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
  int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
  const int sx = (x0 < x1) ? 1 : -1;
  const int sy = (y0 < y1) ? 1 : -1;
  dy = -dy;
  int err = dx + dy;
  for (;;) {
    EmblemCanvasPaint(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    const int err2 = 2 * err;
    if (err2 >= dy) { err += dy; x0 += sx; }
    if (err2 <= dx) { err += dx; y0 += sy; }
  }
}

// Remplissage par proximité (4-connexité), sur la couleur pointée.
void EmblemCanvasFill(int sx, int sy, uint32_t color) {
  const uint32_t target = g_emblem_canvas.pixel[sy * kEmblemSide + sx];
  if (target == color) return;
  ++g_emblem_canvas.revision;
  std::vector<int> stack{sy * kEmblemSide + sx};
  while (!stack.empty()) {
    const int idx = stack.back();
    stack.pop_back();
    if (g_emblem_canvas.pixel[idx] != target) continue;
    g_emblem_canvas.pixel[idx] = color;
    const int x = idx % kEmblemSide, y = idx / kEmblemSide;
    if (x > 0)                 stack.push_back(idx - 1);
    if (x < kEmblemSide - 1)   stack.push_back(idx + 1);
    if (y > 0)                 stack.push_back(idx - kEmblemSide);
    if (y < kEmblemSide - 1)   stack.push_back(idx + kEmblemSide);
  }
}

// ── Formes (ligne, rectangle, ellipse) ───────────────────────────────────────
// Une forme est d'abord calculée en MASQUE de cellules : le même masque sert à
// l'aperçu pendant le tirage et à la peinture au relâchement, donc ce qu'on voit
// est exactement ce qu'on obtient.
void MaskSet(bool* mask, int cx, int cy) {
  int lo = 0, hi = 0;
  BrushExtent(&lo, &hi);  // même carré N×N que le crayon
  for (int dy = lo; dy <= hi; ++dy) {
    for (int dx = lo; dx <= hi; ++dx) {
      const int x = cx + dx, y = cy + dy;
      if (x < 0 || x >= kEmblemSide || y < 0 || y >= kEmblemSide) continue;
      mask[y * kEmblemSide + x] = true;
      if (g_emblem_canvas.mirror) mask[y * kEmblemSide + (kEmblemSide - 1 - x)] = true;
    }
  }
}
void MaskLine(bool* mask, int x0, int y0, int x1, int y1) {
  int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
  int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
  const int sx = (x0 < x1) ? 1 : -1;
  const int sy = (y0 < y1) ? 1 : -1;
  dy = -dy;
  int err = dx + dy;
  for (;;) {
    MaskSet(mask, x0, y0);
    if (x0 == x1 && y0 == y1) break;
    const int err2 = 2 * err;
    if (err2 >= dy) { err += dy; x0 += sx; }
    if (err2 <= dx) { err += dx; y0 += sy; }
  }
}
// `tool` vaut kToolLine / kToolRect / kToolEllipse ; (x0,y0)-(x1,y1) = coins tirés.
void EmblemShapeMask(int tool, bool filled, int x0, int y0, int x1, int y1, bool* mask) {
  std::fill(mask, mask + kEmblemPixels, false);
  if (tool == kToolLine) {
    MaskLine(mask, x0, y0, x1, y1);
    return;
  }
  const int left = std::min(x0, x1), right = std::max(x0, x1);
  const int top = std::min(y0, y1), bottom = std::max(y0, y1);
  if (tool == kToolRect) {
    if (filled) {
      for (int y = top; y <= bottom; ++y)
        for (int x = left; x <= right; ++x) MaskSet(mask, x, y);
    } else {
      MaskLine(mask, left, top, right, top);
      MaskLine(mask, left, bottom, right, bottom);
      MaskLine(mask, left, top, left, bottom);
      MaskLine(mask, right, top, right, bottom);
    }
    return;
  }
  // Ellipse inscrite dans le rectangle tiré. Le contour = les cases DANS l'ellipse
  // dont un voisin est dehors : sur 24x24 c'est plus net qu'un tracé paramétrique.
  const float cx = (left + right) * 0.5f, cy = (top + bottom) * 0.5f;
  const float rx = std::max((right - left) * 0.5f, 0.5f);
  const float ry = std::max((bottom - top) * 0.5f, 0.5f);
  auto inside = [&](int x, int y) {
    const float nx = (x - cx) / rx, ny = (y - cy) / ry;
    return nx * nx + ny * ny <= 1.0f;
  };
  for (int y = top; y <= bottom; ++y) {
    for (int x = left; x <= right; ++x) {
      if (!inside(x, y)) continue;
      if (filled || !inside(x - 1, y) || !inside(x + 1, y) || !inside(x, y - 1) ||
          !inside(x, y + 1))
        MaskSet(mask, x, y);
    }
  }
}
void EmblemApplyMask(const bool* mask, uint32_t color) {
  ++g_emblem_canvas.revision;
  for (int i = 0; i < kEmblemPixels; ++i)
    if (mask[i]) g_emblem_canvas.pixel[i] = color;
}

// Importe une icône d'item dans le canvas. Les icônes d'inventaire du client font
// 24x24 — exactement la taille d'un emblème —, donc la copie est pixel pour pixel ;
// une icône d'un autre format est simplement centrée. Les pixels transparents
// (magenta color-key) deviennent du vide, pas du noir.
bool EmblemCanvasLoadItemIcon(uint32_t nameid) {
  std::vector<uint8_t> argb;
  int w = 0, h = 0;
  if (!ro::ItemIconPixels(nameid, &argb, &w, &h) || w <= 0 || h <= 0) return false;
  EmblemCanvasPushUndo();
  for (int i = 0; i < kEmblemPixels; ++i) g_emblem_canvas.pixel[i] = kEmblemClear;
  const int offset_x = (kEmblemSide - w) / 2;  // négatif si l'icône est plus grande
  const int offset_y = (kEmblemSide - h) / 2;
  for (int y = 0; y < h; ++y) {
    const int dst_y = y + offset_y;
    if (dst_y < 0 || dst_y >= kEmblemSide) continue;
    for (int x = 0; x < w; ++x) {
      const int dst_x = x + offset_x;
      if (dst_x < 0 || dst_x >= kEmblemSide) continue;
      const uint8_t* px = &argb[(static_cast<size_t>(y) * w + x) * 4];
      if (px[3] == 0) continue;  // transparent : on laisse le vide
      g_emblem_canvas.pixel[dst_y * kEmblemSide + dst_x] =
          (static_cast<uint32_t>(px[2]) << 16) | (static_cast<uint32_t>(px[1]) << 8) | px[0];
    }
  }
  g_emblem_canvas.started = true;
  ++g_emblem_canvas.revision;
  return true;
}

// Reprend un BMP existant (24 ou 8 bits) dans le canvas, pour retoucher un emblème
// déjà fait plutôt que de repartir d'une page blanche.
bool EmblemCanvasLoadBmp(const std::vector<uint8_t>& bmp) {
  if (bmp.size() < 54 || bmp[0] != 'B' || bmp[1] != 'M') return false;
  const int32_t  w       = *reinterpret_cast<const int32_t*>(&bmp[0x12]);
  const int32_t  hraw    = *reinterpret_cast<const int32_t*>(&bmp[0x16]);
  const int16_t  bpp     = *reinterpret_cast<const int16_t*>(&bmp[0x1c]);
  const uint32_t dataOff = *reinterpret_cast<const uint32_t*>(&bmp[0x0a]);
  const int  h        = (hraw < 0) ? -hraw : hraw;
  const bool bottomUp = hraw > 0;
  if (w != kEmblemSide || h != kEmblemSide || (bpp != 24 && bpp != 8)) return false;
  const uint8_t* palette = (bpp == 8) ? bmp.data() + 54 : nullptr;
  if (bpp == 8 && bmp.size() < 54u + 256u * 4u) return false;
  const size_t rowSize = static_cast<size_t>((w * (bpp / 8) + 3) & ~3);
  if (static_cast<size_t>(dataOff) + rowSize * h > bmp.size()) return false;
  for (int y = 0; y < kEmblemSide; ++y) {
    const int srcY = bottomUp ? (kEmblemSide - 1 - y) : y;
    const uint8_t* row = bmp.data() + dataOff + static_cast<size_t>(srcY) * rowSize;
    for (int x = 0; x < kEmblemSide; ++x) {
      uint8_t b, g, r;
      if (bpp == 24) {
        b = row[x * 3]; g = row[x * 3 + 1]; r = row[x * 3 + 2];
      } else {
        const uint8_t* entry = palette + static_cast<size_t>(row[x]) * 4;
        b = entry[0]; g = entry[1]; r = entry[2];
      }
      g_emblem_canvas.pixel[y * kEmblemSide + x] =
          (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
    }
  }
  g_emblem_canvas.started = true;
  g_emblem_canvas.undo.clear();
  ++g_emblem_canvas.revision;
  return true;
}

// Fabrique le BMP 24 bits bottom-up attendu par le serveur : 54 octets d'en-tête +
// 24 lignes de 72 octets (déjà alignées sur 4) = 1782, sous le plafond de 1800.
std::vector<uint8_t> BuildEmblemBmp() {
  constexpr uint32_t kRowSize   = kEmblemSide * 3;
  constexpr uint32_t kPixelSize = kRowSize * kEmblemSide;
  constexpr uint32_t kFileSize  = 54 + kPixelSize;
  std::vector<uint8_t> bmp(kFileSize, 0);
  bmp[0] = 'B'; bmp[1] = 'M';
  *reinterpret_cast<uint32_t*>(&bmp[2])    = kFileSize;   // bfSize (le serveur le compare !)
  *reinterpret_cast<uint32_t*>(&bmp[0x0a]) = 54;          // bfOffBits
  *reinterpret_cast<uint32_t*>(&bmp[0x0e]) = 40;          // biSize
  *reinterpret_cast<int32_t*>(&bmp[0x12])  = kEmblemSide; // biWidth
  *reinterpret_cast<int32_t*>(&bmp[0x16])  = kEmblemSide; // biHeight (>0 = bottom-up)
  *reinterpret_cast<uint16_t*>(&bmp[0x1a]) = 1;           // biPlanes
  *reinterpret_cast<uint16_t*>(&bmp[0x1c]) = 24;          // biBitCount
  *reinterpret_cast<uint32_t*>(&bmp[0x22]) = kPixelSize;  // biSizeImage
  for (int y = 0; y < kEmblemSide; ++y) {
    uint8_t* row = &bmp[54 + static_cast<size_t>(kEmblemSide - 1 - y) * kRowSize];
    for (int x = 0; x < kEmblemSide; ++x) {
      const uint32_t c = g_emblem_canvas.pixel[y * kEmblemSide + x];
      row[x * 3 + 0] = static_cast<uint8_t>(c & 0xFF);         // B
      row[x * 3 + 1] = static_cast<uint8_t>((c >> 8) & 0xFF);  // G
      row[x * 3 + 2] = static_cast<uint8_t>((c >> 16) & 0xFF); // R
    }
  }
  return bmp;
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

// Description d'un SKILL : fenêtre 0x2e (≠ 0xc, qui est celle des objets), pilotée par
// l'id BRUT — pas par un ItemSkillInfo. Re-clic sur le même skill = referme, comme le
// natif (l'id affiché vit à +0x104).
constexpr int kWinSkillDesc    = 0x2e;
constexpr int kMsgSetSkill     = 0x3d;
constexpr int kSkillWinShownId = 0x104;
constexpr uintptr_t kCloseWindow = 0x00a2e770;  // UIWindowMgr_Close(mgr, edx, id)
using CloseWindow_t = void (__fastcall*)(void*, void*, int);

void OpenSkillDesc(int skillId, int mx, int my) {
  if (skillId <= 0) return;
  __try {
    void* mgr = uiwnd::Mgr();
    void* wnd = reinterpret_cast<MakeWindow_t>(kMakeWindow)(
        mgr, nullptr, reinterpret_cast<void*>(kWinSkillDesc));
    if (!wnd) return;
    if (*reinterpret_cast<int*>(reinterpret_cast<char*>(wnd) + kSkillWinShownId) == skillId) {
      reinterpret_cast<CloseWindow_t>(kCloseWindow)(mgr, nullptr, kWinSkillDesc);
      return;
    }
    Vf<OnMsg_t>(wnd, kVfOnMsg)(wnd, nullptr, 0, kMsgSetSkill, skillId, 0, 0, 0);
    Vf<SetPos_t>(wnd, kVfSetPos)(wnd, nullptr, mx, my);
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
// Lance une compétence par le chemin NATIF, celui du bouton « use » de la fenêtre de
// compétences : cmd 0x45 du même dispatcher. Le client fait ses propres contrôles (SP,
// cooldowns partagés), affiche la barre de cast, puis envoie CZ_USE_SKILL 0x0438
// {op, niv, id, cibleGID}. Fabriquer ce paquet nous-mêmes sauterait tout cela — une
// compétence à 10 s de cast (GD_RESTORE) partirait sans le moindre retour à l'écran.
// RE du bloc : 0x00c8d9ad..0x00c8de53 ; arguments (cmd, skillId, cibleGID, niveau, 0).
void SendUseSkill(uint16_t skillId, int level) {
  __try {
    void* d = *reinterpret_cast<void**>(kUICmdDisp);
    // GID de notre acteur = notre AID : les compétences de guilde se lancent sur soi.
    const uint32_t self = *reinterpret_cast<const uint32_t*>(kOwnAccountId);
    if (d && self)
      Vf<DispCmd_t>(d, kVfDispCmd)(d, kCmdUseSkill, skillId, static_cast<int>(self),
                                   level, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Envoie une commande @ par le canal de chat (CZ_GlobalMessage 0x00f3), c'est-à-dire
// EXACTEMENT ce que fait le joueur en la tapant : mêmes droits de groupe, mêmes refus,
// même journalisation. Sert à l'entrepôt de guilde, que le serveur n'expose par AUCUN
// paquet — storage_guild_storageopen() n'est atteignable que par script NPC, par
// @guildstorage, ou par la réponse du char-server.
void SendAtCommand(const char* command) {
  // ⚠ rAthena EXIGE « <nom du perso> : <texte> » (clif_process_message) : un nom qui
  // ne correspond pas est traité comme un client trafiqué et COUPE la session.
  const std::string own = Bourgeon::Instance().client().session().GetCharName();
  if (own.empty() || !command) return;
  char text[128];
  const int text_len =
      std::snprintf(text, sizeof(text), "%s : %s", own.c_str(), command);
  if (text_len <= 0 || text_len >= static_cast<int>(sizeof(text))) return;
  uint8_t pkt[4 + sizeof(text)];
  const uint16_t total = static_cast<uint16_t>(4 + text_len + 1);  // + le zéro final
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpChatMessage;
  *reinterpret_cast<uint16_t*>(pkt + 2) = total;  // paquet variable : longueur TOTALE
  std::memcpy(pkt + 4, text, static_cast<size_t>(text_len) + 1);
  Bourgeon::Instance().SendPacket(pkt, total);
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
// Invoquer/basculer un compagnon (cart/peco/faucon) : CZ_BOURGEON_COMPANION (0x0F15),
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
struct WinSnap {
  float narrow = 0.0f, wide = 0.0f;
  bool  valid = false;
  bool  force_wide = false;  // onglets pleine largeur (Guilde) : pas de repli étroit
};
WinSnap g_win_snap;
void SnapCharSheetWidth(ImGuiSizeCallbackData* d) {
  if (!g_win_snap.valid) return;
  if (g_win_snap.force_wide) { d->DesiredSize.x = g_win_snap.wide; return; }
  const float mid = (g_win_snap.narrow + g_win_snap.wide) * 0.5f;
  d->DesiredSize.x = (d->DesiredSize.x < mid) ? g_win_snap.narrow : g_win_snap.wide;
}

// ── Raccourcis clavier de preset ─────────────────────────────────────────────
// Conversions VK <-> ImGuiKey, capture, libellé et contrôle de conflit vivent
// dans plugins/hotkey_util.h : ils sont partagés avec la touche de saut, qui est
// elle aussi remappable — c'est ce qui permet aux deux de se refuser mutuellement
// un combo déjà pris.

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
  // État des compagnons (cart/peco/faucon) poussé par le serveur au login + à chaque
  // changement (pc_setcart/riding/falcon). Gate/affiche les cases sans RE côté client.
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kCompanionState);
  // Postes de guilde : le client ne les garde QUE dans la fenêtre native (liste
  // interne à UIGuildPositionManageWnd), donc on lit les paquets nous-mêmes. Ce sont
  // des paquets à longueur variable : on ne demande que le champ longueur (2 o) et on
  // parcourt les entrées dans le buffer live (même approche que la liste du cash shop).
  Bourgeon::Instance().RegisterObserveOpcode(kOpPositionNames, 2);  // ZC 0x0166
  Bourgeon::Instance().RegisterObserveOpcode(kOpPositionInfo, 2);   // ZC 0x0160
  Bourgeon::Instance().RegisterObserveOpcode(kOpPositionChanged, 2);// ZC 0x0174
  // Résultat d'une création de guilde (1 octet) : le client affiche déjà sa propre
  // boîte, on veut en plus le retour dans l'onglet.
  Bourgeon::Instance().RegisterObserveOpcode(kOpGuildCreateAck, 1);  // ZC 0x0167
  // Image d'emblème renvoyée par le serveur : c'est l'accusé de réception d'un
  // changement (guild_emblem_changed la pousse à tous les membres). Paquet variable :
  // on ne demande que le champ longueur et on lit le reste dans le buffer live.
  Bourgeon::Instance().RegisterObserveOpcode(kOpGuildEmblemImg, 2);  // ZC 0x0152
  // Compétences de guilde : même situation que les postes (rien de conservé hors de la
  // fenêtre native), paquet variable -> on ne demande que le champ longueur.
  Bourgeon::Instance().RegisterObserveOpcode(kOpGuildSkills, 2);   // ZC 0x0162
  Bourgeon::Instance().RegisterObserveOpcode(kOpGuildBanList, 2);  // ZC 0x0b7c
  // Les cooldowns (ZC 0x043D) sont observés par le service partagé
  // ragnarok/skill_cooldowns.h, installé avant les plugins.
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

// ── Grimoire : noms des onglets ──────────────────────────────────────────────
// Lua JobSkillTab_GetTabName(classe) rend QUATRE chaînes — « 1st » / « 2nd »… pour la
// plupart des classes, « Ninja », « Gunslinger », « Summoner »… pour celles qui ont
// leur propre libellé (skilltreeview.lub, appels ChangeSkillTabName). C'est la source
// du natif (sub_9765F0) ; l'appelant traduit ce qui reste générique.
void ReadSkillTabNamesSEH(int jobId, char out[4][32]) {
  for (int i = 0; i < 4; ++i) out[i][0] = '\0';
  __try {
    void* M = *reinterpret_cast<void**>(kLuaState);
    void* L = M ? *reinterpret_cast<void**>(M) : nullptr;  // **(0x015ffd78)
    if (!L) return;
    reinterpret_cast<LuaGetField_t>(kLuaGetField)(L, kLuaGlobals, "JobSkillTab_GetTabName");
    reinterpret_cast<LuaPushNum_t>(kLuaPushNum)(L, static_cast<double>(jobId));
    if (reinterpret_cast<LuaPCall_t>(kLuaPCall)(L, 1, 4, 0) == 0) {
      for (int i = 0; i < 4; ++i) {
        const char* s = reinterpret_cast<LuaToLStr_t>(kLuaToLStr)(L, -4 + i, nullptr);
        if (s && s[0]) { std::strncpy(out[i], s, 31); out[i][31] = '\0'; }
      }
      reinterpret_cast<LuaSetTop_t>(kLuaSetTop)(L, -5);  // dépile les 4 résultats
    } else {
      reinterpret_cast<LuaSetTop_t>(kLuaSetTop)(L, -2);  // dépile le message d'erreur
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Job du personnage (le même getter que ClassNameSEH), clé de la table des libellés.
int OwnJobIdSEH() {
  __try {
    using GetJobId_t = int (__fastcall*)(void*, void*);
    return reinterpret_cast<GetJobId_t>(0x00d5b580)(reinterpret_cast<void*>(kSession),
                                                    nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ── Arbre des compétences de guilde (fichier client) ─────────────────────────
// Le serveur ne l'envoie pas : ZC 0x0162 omet le niveau max, et clif_guild_skillinfo
// filtre par guild_check_skill_require — une compétence verrouillée n'arrive JAMAIS.
// skilltreeguild.lub (dans le GRF, généré depuis db/guild_skill_tree.yml) comble ce
// trou. Il est purement informatif : le serveur reste seul juge d'un skillup.
//
// Lua_ExecuteScriptFile(this=holder, nom, sousData, sansPrefixe) : `sousData`=1 ajoute
// « data\ », `sansPrefixe`=0 ajoute « LuaFiles514\ » — soit
// data\luafiles514\lua files\skillinfoz\skilltreeguild.lub, et la lecture passe par le
// VFS (disque puis GRF). Rend 1 si le fichier a été chargé ET exécuté.
constexpr uintptr_t kLuaExecFile = 0x00a9bc90;
using LuaExecFile_t = char(__thiscall*)(void*, const char*, char, char);
constexpr char kGuildTreeLuaFile[] = "Lua Files\\SkillInfoz\\skilltreeguild";

constexpr uintptr_t kLuaToBool = 0x0051abf0;  // lua_toboolean(L,idx) : nil/false -> 0
using LuaToBool_t = int(__cdecl*)(void*, int);

// Charge le fichier puis appelle son GdDump() ; `out` reçoit la table sérialisée
// (« id,maxLv,prereq:lvl|prereq:lvl; » répété), `err` le message Lua en cas d'échec.
// POD only : SEH (C2712). Codes distincts pour que la console dise QUELLE étape a
// lâché — « absent » et « erreur d'exécution » se soignent différemment.
enum {
  kTreeOk = 1,          // table lue
  kTreeNoFile = 0,      // Lua_ExecuteScriptFile a rendu 0 : introuvable, ou erreur Lua à l'exécution
  kTreeNoLua = -1,      // état Lua pas encore prêt -> retenter
  kTreeNoDumper = -2,   // fichier chargé mais GdDump absent (fichier d'une autre version ?)
  kTreeCallFailed = -3, // GdDump a levé -> `err`
  kTreeEmpty = -4,      // appel OK mais chaîne vide
};
int GuildTreeDumpSEH(char* out, size_t cap, char* err, size_t err_cap) {
  out[0] = '\0';
  err[0] = '\0';
  __try {
    void* M = *reinterpret_cast<void**>(kLuaState);
    void* L = M ? *reinterpret_cast<void**>(M) : nullptr;  // **(0x015ffd78)
    if (!L) return kTreeNoLua;
    if (!reinterpret_cast<LuaExecFile_t>(kLuaExecFile)(M, kGuildTreeLuaFile, 1, 0))
      return kTreeNoFile;
    reinterpret_cast<LuaGetField_t>(kLuaGetField)(L, kLuaGlobals, "GdDump");
    if (!reinterpret_cast<LuaToBool_t>(kLuaToBool)(L, -1)) {  // nil = pas de fonction
      reinterpret_cast<LuaSetTop_t>(kLuaSetTop)(L, -2);
      return kTreeNoDumper;
    }
    if (reinterpret_cast<LuaPCall_t>(kLuaPCall)(L, 0, 1, 0) != 0) {
      const char* msg = reinterpret_cast<LuaToLStr_t>(kLuaToLStr)(L, -1, nullptr);
      if (msg && msg[0]) std::strncpy(err, msg, err_cap - 1);
      reinterpret_cast<LuaSetTop_t>(kLuaSetTop)(L, -2);
      return kTreeCallFailed;
    }
    const char* s = reinterpret_cast<LuaToLStr_t>(kLuaToLStr)(L, -1, nullptr);
    if (s && s[0]) std::strncpy(out, s, cap - 1);
    reinterpret_cast<LuaSetTop_t>(kLuaSetTop)(L, -2);  // dépile le résultat
    return out[0] ? kTreeOk : kTreeEmpty;
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; return kTreeNoFile; }
}

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

// Temps restant sur une compétence. La table vit dans ragnarok/skill_cooldowns.h :
// la barre d'action moderne l'affiche aussi, et une seconde copie du même paquet
// dérivait dès qu'un des deux consommateurs manquait un envoi.
unsigned long CharacterSheet::SkillCooldownRemaining(uint16_t skill_id) const {
  return ro::SkillCooldownRemainingMs(skill_id);
}

void CharacterSheet::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  // ZC_SKILL_POSTDELAY (0x043D) n'est PAS traité ici : la table de cooldowns est
  // partagée et remplie en amont, dans Bourgeon::FireRecvPacket.

  // Résultat d'une demande de création de guilde (codes documentés côté serveur :
  // 0 créée, 1 déjà en guilde, 2 nom pris, 3 Emperium manquant).
  if (opcode == kOpGuildCreateAck) {
    if (len >= 1) guild_create_result_ = data[0];
    return;
  }

  // Emblème renvoyé par le serveur (ZC 0x0152) : preuve que le changement est passé.
  // Le client natif écrit alors _tmpEmblem\<nom>_<guilde>_<version>.ebm ; on jette
  // notre texture pour que l'en-tête relise le fichier de la NOUVELLE version.
  if (opcode == kOpGuildEmblemImg) {
    // Même piège que plus bas : l'observation ne transmet que 2 octets (`len`), le
    // reste vit dans le buffer live. Un test sur `len` ici tuait le handler.
    if (len < 2) return;
    const int packet_len = *reinterpret_cast<const uint16_t*>(data);
    if (packet_len < 12) return;
    ForgetEmblem(*reinterpret_cast<const int32_t*>(data + 2));
    return;
  }

  // Compétences de guilde (ZC 0x0162) : [len.W][points.W] puis 37 o/entrée. La liste
  // envoyée est déjà FILTRÉE par le serveur (guild_check_skill_require) : ce qui n'y
  // est pas n'a pas ses prérequis, on remplace donc la liste entière à chaque paquet.
  if (opcode == kOpGuildSkills) {
    // `len` = les octets DEMANDÉS à l'observation (2 ici), pas la taille du paquet :
    // tout le reste se lit dans le buffer live, borné par la longueur annoncée.
    if (len < 2) return;
    const int packet_len = *reinterpret_cast<const uint16_t*>(data);
    if (packet_len < 6) return;
    guild_skill_points_ = *reinterpret_cast<const int16_t*>(data + 2);
    guild_skills_known_ = true;
    guild_skills_.clear();
    for (int off = 6; off + kGuildSkillEntry <= packet_len; off += kGuildSkillEntry) {
      const uint8_t* entry = data + (off - 2);
      GuildSkillRow row;
      row.id    = *reinterpret_cast<const uint16_t*>(entry);
      row.inf   = *reinterpret_cast<const int32_t*>(entry + 2);
      row.level = *reinterpret_cast<const uint16_t*>(entry + 6);
      row.sp    = *reinterpret_cast<const uint16_t*>(entry + 8);
      row.range = *reinterpret_cast<const uint16_t*>(entry + 10);
      std::strncpy(row.name, reinterpret_cast<const char*>(entry + 12), sizeof(row.name) - 1);
      row.upgradable = entry[36] != 0;
      if (row.id != 0) guild_skills_.push_back(row);
    }
    return;
  }

  // Expulsions passées (ZC 0x0b7c) : [len.W] puis 68 o/entrée.
  if (opcode == kOpGuildBanList) {
    if (len < 2) return;
    const int packet_len = *reinterpret_cast<const uint16_t*>(data);
    guild_bans_known_ = true;
    guild_bans_.clear();
    for (int off = 4; off + kGuildBanEntry <= packet_len; off += kGuildBanEntry) {
      const uint8_t* entry = data + (off - 2);
      GuildBanRow row;
      row.char_id = *reinterpret_cast<const uint32_t*>(entry);
      std::strncpy(row.reason, reinterpret_cast<const char*>(entry + 4), sizeof(row.reason) - 1);
      std::strncpy(row.name, reinterpret_cast<const char*>(entry + 44), sizeof(row.name) - 1);
      guild_bans_.push_back(row);
    }
    return;
  }

  // ── Postes de guilde (paquets STANDARD observés) ───────────────────────────
  // Pour un opcode observé, `data` pointe juste après l'opcode : ici sur le champ
  // longueur du paquet. Les trois paquets sont à longueur variable, donc on relit
  // cette longueur et on parcourt les entrées directement dans le buffer live ; les
  // décalages ci-dessous sont ceux du PAQUET moins 2 (l'opcode).
  if (opcode == kOpPositionNames || opcode == kOpPositionInfo ||
      opcode == kOpPositionChanged) {
    if (len < 2) return;
    const int packet_len = *reinterpret_cast<const uint16_t*>(data);
    const int entry_size = (opcode == kOpPositionNames)   ? 28
                           : (opcode == kOpPositionInfo)  ? 16
                                                          : 40;
    // Garde-fou : au plus MAX_GUILDPOSITION entrées, quelle que soit la longueur
    // annoncée (on lit dans le buffer live, pas dans une copie bornée).
    int parsed = 0;
    for (int off = 4; off + entry_size <= packet_len && parsed < kGuildPositionSlots;
         off += entry_size, ++parsed) {
      const uint8_t* entry = data + (off - 2);
      const int id = *reinterpret_cast<const int32_t*>(entry);
      if (id < 0 || id >= kGuildPositionSlots) continue;
      GuildPositionRow& row = guild_positions_[id];
      if (opcode == kOpPositionNames) {
        std::strncpy(row.name, reinterpret_cast<const char*>(entry + 4), sizeof(row.name) - 1);
        row.name[sizeof(row.name) - 1] = '\0';
        row.has_name = true;
      } else if (opcode == kOpPositionInfo) {
        row.mode     = *reinterpret_cast<const int32_t*>(entry + 4);
        row.pay_rate = *reinterpret_cast<const int32_t*>(entry + 12);
        row.has_info = true;
      } else {  // 0x0174 : nom + droits + part d'exp d'un poste modifié
        row.mode     = *reinterpret_cast<const int32_t*>(entry + 4);
        row.pay_rate = *reinterpret_cast<const int32_t*>(entry + 12);
        std::strncpy(row.name, reinterpret_cast<const char*>(entry + 16), sizeof(row.name) - 1);
        row.name[sizeof(row.name) - 1] = '\0';
        row.has_name = row.has_info = true;
      }
      // Alimente aussi le repli id -> libellé de la colonne « Poste » du roster.
      if (row.has_name) RememberGuildPosition(id, row.name);
    }
    return;
  }

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
    pi.left_hand = (s == 5) && (li.loc & kEqpHandR) != 0;
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
        const uint32_t pos = pi.left_hand ? kEqpHandL : static_cast<uint32_t>(li.loc);
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

void CharacterSheet::ProcessPresetHotkeys() {
  // Une capture de combo est en cours (ici ou ailleurs : touche de saut) : la
  // touche pressée sert à remapper, elle ne doit rien déclencher.
  if (hk_capturing_ >= 0 || hotkeys::CaptureInProgress()) return;
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput) return;    // saisie de texte (chat…) : ne pas déclencher
  const uint32_t cid = static_cast<uint32_t>(ReadInt(kOwnCharId));
  for (const EquipPreset& ep : equip_presets_) {
    if (ep.cid != cid || ep.hotkey_vk == 0) continue;
    if (io.KeyCtrl != ep.hotkey_ctrl || io.KeyAlt != ep.hotkey_alt || io.KeyShift != ep.hotkey_shift) continue;
    const ImGuiKey k = hotkeys::VkToImGuiKey(ep.hotkey_vk);
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
      hotkeys::PingCapture();  // gèle les raccourcis (saut compris) le temps du choix
      ImGui::TextColored(kBlack, "appuie sur une touche…  (Échap : annuler)");
      ImGuiIO& io = ImGui::GetIO();
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        hk_capturing_ = -1;
        hk_conflict_msg_.clear();
      } else if (const int vk = hotkeys::CaptureMainVk()) {
        const bool c = io.KeyCtrl, a = io.KeyAlt, sh = io.KeyShift;
        char what[64];
        if (hotkeys::Conflict(vk, c, a, sh, hotkeys::Owner::kEquipPreset, mine[mi], what,
                              sizeof(what))) {
          hk_conflict_msg_ = std::string("Déjà utilisé par ") + what + " — choisis un autre combo";
        } else {  // libre : on assigne + persiste
          EquipPreset& e = equip_presets_[mine[mi]];
          e.hotkey_vk = vk; e.hotkey_ctrl = c; e.hotkey_alt = a; e.hotkey_shift = sh;
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
      hotkeys::Label(ep.hotkey_vk, ep.hotkey_ctrl, ep.hotkey_alt, ep.hotkey_shift, hkl,
                     sizeof(hkl));
      ImGui::TextColored(kBlack, "%s", hkl);
      ImGui::SameLine(0.0f, 6.0f);
      if (ro::RoButton("Définir", bw("Définir"))) {
        hk_capturing_ = mine[mi];
        hk_conflict_msg_.clear();
      }
      if (ep.hotkey_vk != 0) {
        ImGui::SameLine(0.0f, 4.0f);
        if (ro::RoButton("Effacer", bw("Effacer"))) {
          EquipPreset& e = equip_presets_[mine[mi]];
          e.hotkey_vk = 0; e.hotkey_ctrl = e.hotkey_alt = e.hotkey_shift = false;
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

// ═══ Onglet Grimoire ════════════════════════════════════════════════════════
// Remplaçant de la fenêtre native UINewSkillListWnd (id 0x25) — sa vue « moderne »,
// celle en grille d'icônes. Les données viennent du MÊME objet que celui que le natif
// recopie (CPlayerSkillBundle, cf. docs/skill_tree_re.md partie II), donc rien ici ne
// dépend de la fenêtre native : elle peut rester masquée.

int CharacterSheet::PendingLevel(uint16_t id) const {
  for (const auto& p : skill_pending_)
    if (p.first == id) return p.second;
  return 0;
}

// Réserve un point sur `id` (ou tout ce qui reste jusqu'au niveau max si `to_max`).
// Comme la vue grille native (sub_979BA0), on réserve AUSSI les prérequis directs qui
// manquent : cliquer sur une compétence verrouillée prépare la chaîne au lieu de
// refuser sèchement. Rien n'est envoyé ici — c'est « Appliquer » qui parle au serveur.
bool CharacterSheet::ReserveSkillPoint(uint16_t id, bool to_max) {
  // Tampon PROPRE à cette fonction : DrawSkillsTab l'appelle en plein parcours de SON
  // tableau, et partager le même buffer statique invaliderait la fiche qu'il tient.
  static SkillRaw scan[kSkillMaxNodes];
  // Chercher la fiche dans TOUS les onglets : un prérequis vit souvent dans un autre.
  auto find = [](uint16_t want, SkillRaw& out) -> bool {
    for (int tab = -1; tab < kSkillJobTabs; ++tab) {
      const int n = ReadSkillTabSEH(tab, scan, kSkillMaxNodes);
      for (int i = 0; i < n; ++i)
        if (scan[i].id == static_cast<int>(want)) { out = scan[i]; return true; }
    }
    return false;
  };
  SkillRaw target{};
  if (!find(id, target)) { skill_status_ = "Compétence introuvable dans l'arbre."; return false; }

  int spent = 0;
  for (const auto& p : skill_pending_) {
    SkillRaw fiche{};
    if (find(p.first, fiche)) spent += p.second - fiche.learned;
  }
  int left = SkillPointsSEH() - spent;
  if (left <= 0) { skill_status_ = "Plus de point de compétence disponible."; return false; }

  // Poser (ou relever) une réservation ; renvoie ce qui a réellement été dépensé.
  auto reserve = [&](const SkillRaw& fiche, int target_level) -> int {
    const int current = std::max(fiche.learned, PendingLevel(static_cast<uint16_t>(fiche.id)));
    if (target_level > fiche.maxlv) target_level = fiche.maxlv;
    if (target_level <= current) return 0;
    const int take = std::min(target_level - current, left);
    if (take <= 0) return 0;
    const int lvl = current + take;
    for (auto& p : skill_pending_)
      if (static_cast<int>(p.first) == fiche.id) { p.second = lvl; left -= take; return take; }
    skill_pending_.emplace_back(static_cast<uint16_t>(fiche.id), lvl);
    left -= take;
    return take;
  };

  // 1) les prérequis directs manquants, dans l'ordre où le Lua les a posés ;
  for (int i = 0; i < target.need_count; ++i) {
    SkillRaw req{};
    if (!find(static_cast<uint16_t>(target.need_id[i]), req)) continue;
    if (req.user_up <= 0) continue;  // le joueur ne peut pas la monter (compétence de quête)
    reserve(req, target.need_lv[i]);
  }
  // 2) la compétence demandée elle-même.
  if (target.user_up <= 0) {
    skill_status_ = "Cette compétence ne se monte pas avec des points.";
    return false;
  }
  const int current = std::max(target.learned, PendingLevel(id));
  if (current >= target.maxlv) { skill_status_ = "Déjà au niveau maximum."; return false; }
  // `reserve` borne déjà au niveau max ET aux points restants : viser le max revient
  // à demander tout ce qui reste, sans boucler ni recompter les prérequis.
  if (reserve(target, to_max ? target.maxlv : current + 1) == 0) {
    skill_status_ = "Points insuffisants pour les prérequis.";
    return false;
  }
  skill_status_.clear();
  return true;
}

void CharacterSheet::DrawSkillsTab() {
  const ImVec4 kGray(0.35f, 0.35f, 0.42f, 1.0f);
  const ImVec4 kAmber(0.85f, 0.65f, 0.20f, 1.0f);
  const ImVec4 kGreen(0.30f, 0.75f, 0.35f, 1.0f);

  // ── Onglets de job : le natif ne montre que ceux qui ont des compétences ──
  static SkillRaw nodes[kSkillMaxNodes];
  int counts[kSkillJobTabs + 1] = {};
  for (int t = 0; t < kSkillJobTabs; ++t) counts[t] = ReadSkillTabSEH(t, nodes, kSkillMaxNodes);
  counts[kSkillJobTabs] = ReadSkillTabSEH(-1, nodes, kSkillMaxNodes);  // liste plate

  // Libellés : le Lua donne « 1st »/« 2nd »… (ou « Ninja »… pour quelques classes).
  // On traduit le générique, on garde tel quel ce qui est spécifique à la classe.
  char lua_names[4][32];
  ReadSkillTabNamesSEH(OwnJobIdSEH(), lua_names);
  static const char* kFallback[4] = {"1re classe", "2e classe", "3e classe", "4e classe"};
  auto tab_label = [&](int t) -> const char* {
    if (t >= kSkillJobTabs) return "Divers";
    const char* n = lua_names[t];
    if (!n[0]) return kFallback[t];
    if (std::strcmp(n, "1st") == 0) return kFallback[0];
    if (std::strcmp(n, "2nd") == 0) return kFallback[1];
    if (std::strcmp(n, "3rd") == 0) return kFallback[2];
    if (std::strcmp(n, "4th") == 0) return kFallback[3];
    return n;
  };

  // ── En-tête : points, réservations, filtre, mode d'affichage ──
  // Au passage, on JETTE les réservations devenues sans objet : compétence absente de
  // l'arbre (changement de job) ou déjà montée par le serveur. Sans ça une réservation
  // périmée resterait à compter des points pour rien.
  int reserved_points = 0;
  for (size_t k = 0; k < skill_pending_.size();) {
    int learned = -1;
    for (int t = -1; t < kSkillJobTabs && learned < 0; ++t) {
      const int n = ReadSkillTabSEH(t, nodes, kSkillMaxNodes);
      for (int i = 0; i < n; ++i)
        if (nodes[i].id == static_cast<int>(skill_pending_[k].first)) {
          learned = nodes[i].learned;
          break;
        }
    }
    if (learned < 0 || skill_pending_[k].second <= learned) {
      skill_pending_.erase(skill_pending_.begin() + static_cast<int>(k));
      continue;
    }
    reserved_points += skill_pending_[k].second - learned;
    ++k;
  }
  const int points_total = SkillPointsSEH();
  const int points_left  = points_total - reserved_points;

  ImGui::TextColored(points_left > 0 ? kGreen : kGray, "Points : %d", points_left);
  if (reserved_points > 0) {
    ImGui::SameLine();
    ImGui::TextColored(kAmber, "(%d réservé%s)", reserved_points,
                       reserved_points > 1 ? "s" : "");
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120.0f);
  ImGui::InputTextWithHint("##skfilter", "Rechercher…", skill_filter_buf_,
                           sizeof(skill_filter_buf_));
  ImGui::SameLine();
  if (ro::RoButton(skill_grid_ ? "Grille" : "Liste", 58.0f, 0.0f))
    skill_grid_ = !skill_grid_;
  mui::Tooltip("Bascule entre la grille d'icônes (vue « moderne » du client) et la\n"
               "liste détaillée (niveau, SP, portée, prérequis).");

  if (!skill_pending_.empty()) {
    if (ro::RoSmallButton("Appliquer", 80.0f, 0.0f)) {
      // Un paquet CZ_UPGRADE_SKILLLEVEL PAR NIVEAU, exactement comme le natif
      // (sub_974530) ; le serveur revalide chaque montée (pc_skillup).
      int sent = 0;
      for (const auto& p : skill_pending_) {
        SkillRaw fiche{};
        bool ok = false;
        for (int t = -1; t < kSkillJobTabs && !ok; ++t) {
          const int n = ReadSkillTabSEH(t, nodes, kSkillMaxNodes);
          for (int i = 0; i < n; ++i)
            if (nodes[i].id == static_cast<int>(p.first)) { fiche = nodes[i]; ok = true; break; }
        }
        if (!ok) continue;
        for (int lv = fiche.learned + 1; lv <= p.second; ++lv) { SendSkillUp(p.first); ++sent; }
      }
      skill_pending_.clear();
      skill_status_ = sent > 0 ? "Envoyé au serveur." : "Rien à envoyer.";
    }
    ImGui::SameLine();
    if (ro::RoSmallButton("Annuler", 70.0f, 0.0f)) {
      skill_pending_.clear();
      skill_status_.clear();
    }
    mui::Tooltip("Abandonne les points réservés (rien n'a encore été envoyé).");
  }
  if (!skill_status_.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(kGray, "%s", skill_status_.c_str());
  }

  // ── Seconde ligne : rappel des gestes + lissage des icônes ────────────────
  // « Raccourcis » en texte discret plutôt qu'un pavé permanent : les gestes se
  // découvrent une fois, la place au-dessus de la grille sert tous les jours.
  ImGui::TextDisabled("Raccourcis");
  mui::Tooltip(
      "Clic gauche          réserve un point (et ses prérequis manquants)\n"
      "Ctrl + clic gauche   réserve jusqu'au niveau maximum\n"
      "Clic droit           menu (monter, lancer, niveau d'utilisation, description)\n"
      "Ctrl + clic droit    description de la compétence\n"
      "Glisser              pose la compétence sur une barre d'action\n"
      "Survol               flèches de prérequis (ambre) et de suites (bleu)\n"
      "\n"
      "Rien n'est envoyé au serveur tant que « Appliquer » n'est pas cliqué.");
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  if (ro::RoCheckbox("Lisser les icônes", &skill_bilinear_)) {
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
  }
  mui::Tooltip("Filtrage bilinéaire des icônes de la grille.\n"
               "Décoché (défaut) = pixels nets, comme le client natif, qui ne filtre pas.");

  // ── Groupes d'onglets ────────────────────────────────────────────────────────
  // Le natif en fait un par palier de classe ; on FUSIONNE la 1re et la 2e, qui
  // tiennent largement sur un écran et qu'on consulte ensemble (une 2e classe se lit
  // toujours à la lumière de la 1re). La 3e, la 4e et « divers » restent à part :
  // ce sont des arbres entiers, les mélanger ne ferait qu'un mur d'icônes.
  constexpr int kNoSource = -99;  // « ce groupe n'a pas de seconde source »
  struct SkillGroup { char label[72]; int src[2]; int nsrc; };
  SkillGroup groups[4] = {};
  int group_count = 0;
  auto add_group = [&](const char* a, const char* b, int s0, int s1) {
    SkillGroup& g = groups[group_count];
    g.nsrc = 0;
    if (counts[s0 < 0 ? kSkillJobTabs : s0] > 0) g.src[g.nsrc++] = s0;
    if (s1 != kNoSource && counts[s1] > 0)       g.src[g.nsrc++] = s1;
    if (g.nsrc == 0) return;
    // Libellé : les deux sources quand elles sont là toutes les deux, sinon la seule.
    if (g.nsrc == 2) std::snprintf(g.label, sizeof(g.label), "%s / %s", a, b);
    else             std::snprintf(g.label, sizeof(g.label), "%s",
                                   g.src[0] == s0 ? a : b);
    ++group_count;
  };
  add_group(tab_label(0), tab_label(1), 0, 1);        // 1re + 2e classe fusionnées
  add_group(tab_label(2), nullptr, 2, kNoSource);     // 3e classe
  add_group(tab_label(3), nullptr, 3, kNoSource);     // 4e classe
  add_group("Divers", nullptr, -1, kNoSource);        // liste plate

  if (group_count == 0) {
    ImGui::TextColored(kGray, "Aucune compétence.");
    return;
  }
  if (ImGui::BeginTabBar("cs_skill_tabs")) {
    for (int g = 0; g < group_count; ++g) {
      char label[96];
      std::snprintf(label, sizeof(label), "%s###skgrp%d", groups[g].label, g);
      if (ImGui::BeginTabItem(label)) { skill_tab_ = g; ImGui::EndTabItem(); }
    }
    ImGui::EndTabBar();
  }
  if (skill_tab_ >= group_count) skill_tab_ = 0;  // le groupe retenu a disparu (job change)

  // Lecture du groupe : chaque source garde SES index de case (ils viennent du Lua),
  // donc on décale la suivante d'un multiple de la largeur de grille — sinon deux
  // arbres fusionnés s'écriraient l'un sur l'autre.
  static int disp_pos[kSkillMaxNodes];
  int count = 0;
  int base = 0;
  // Frontière entre deux arbres fusionnés : la ligne où commence la source suivante,
  // et son libellé. Sans ce repère, « 1re classe » et « 2e classe » se lisent comme un
  // seul arbre continu (cf. la vue native, qui les met dans deux onglets distincts).
  int split_row[1] = {};
  const char* split_label[1] = {};
  int split_count = 0;
  for (int gi = 0; gi < groups[skill_tab_].nsrc; ++gi) {
    const int n = ReadSkillTabSEH(groups[skill_tab_].src[gi], nodes + count,
                                  kSkillMaxNodes - count);
    if (n <= 0) continue;
    // `count > 0` et pas `gi > 0` : si la source précédente était vide, celle-ci est
    // la première à l'écran — un trait au-dessus de la toute première ligne n'aurait
    // rien à séparer.
    if (count > 0 && split_count < 1) {
      const int src = groups[skill_tab_].src[gi];
      split_row[split_count]   = base / kSkillGridCols;
      split_label[split_count] = (src >= 0) ? tab_label(src) : "Divers";
      ++split_count;
    }
    int local_max = -1;
    for (int i = 0; i < n; ++i) local_max = std::max(local_max, nodes[count + i].pos);
    // Les compétences sans case (index -1 : le Lua ne les a pas placées) sont rangées
    // à la suite de la dernière ligne occupée, plutôt que disparaître comme au natif.
    int next_free = (local_max < 0) ? 0 : ((local_max / kSkillGridCols) + 1) * kSkillGridCols;
    int group_max = 0;
    for (int i = 0; i < n; ++i) {
      const int p = (nodes[count + i].pos >= 0) ? nodes[count + i].pos : next_free++;
      disp_pos[count + i] = base + p;
      group_max = std::max(group_max, p);
    }
    base += ((group_max / kSkillGridCols) + 1) * kSkillGridCols;
    count += n;
  }
  if (count == 0) {
    ImGui::TextColored(kGray, "Aucune compétence dans cet onglet.");
    return;
  }

  // Filtre par nom (le libellé localisé, pas l'idname).
  const bool filtering = skill_filter_buf_[0] != '\0';
  auto skill_name = [](int id) -> const char* {
    const char* n = reinterpret_cast<GetSkillNameLua_t>(kGetSkillNameLua)(id);
    return (n && n[0]) ? n : "?";
  };
  auto icontains = [](const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return true;
    for (const char* h = hay; *h; ++h) {
      const char* a = h;
      const char* b = needle;
      while (*a && *b && std::tolower(static_cast<unsigned char>(*a)) ==
                             std::tolower(static_cast<unsigned char>(*b))) { ++a; ++b; }
      if (!*b) return true;
    }
    return false;
  };

  // Infobulle commune aux deux vues : tout ce que le natif éparpille entre la case,
  // son survol et sa fenêtre de description.
  const uint16_t focus = skill_hover_;
  uint16_t hovered_now = 0;
  auto tooltip_for = [&](const SkillRaw& s, int effective) {
    std::string tip = skill_name(s.id);
    if (s.maxlv > 0) {
      tip += "\nNiveau " + std::to_string(effective) + " / " + std::to_string(s.maxlv);
      const int pending = PendingLevel(static_cast<uint16_t>(s.id));
      if (pending > 0) tip += "  (+" + std::to_string(pending - s.learned) + " réservé)";
    }
    tip += s.inf == 0 ? "\nPassive (toujours active)" : "\nActive";
    if (s.learned > 0 && s.sp > 0)    tip += "\nSP : " + std::to_string(s.sp);
    if (s.learned > 0 && s.range > 0) tip += "\nPortée : " + std::to_string(s.range);
    if (s.need_count > 0) {
      tip += "\nRequiert : ";
      for (int i = 0; i < s.need_count; ++i) {
        if (i) tip += ", ";
        tip += skill_name(s.need_id[i]);
        tip += " Niv ";
        tip += std::to_string(s.need_lv[i]);
        // Un prérequis peut vivre dans un AUTRE onglet (une 3e classe en réclame
        // souvent une de 2e) : aucune flèche ne peut alors le désigner, autant le
        // dire — c'est ce que le natif signale en coloriant l'onglet concerné.
        bool here = false;
        for (int k = 0; k < count && !here; ++k) here = nodes[k].id == s.need_id[i];
        if (!here) tip += " (autre onglet)";
      }
    }
    if (s.user_up <= 0) tip += "\n\nNe se monte pas avec des points (quête / lien).";
    if (s.user_up > 0 && effective < s.maxlv)
      tip += "\n\nClic : réserver un point — Ctrl + clic : jusqu'au max";
    else
      tip += "\n";
    tip += "\nClic droit : menu — Ctrl + clic droit : description";
    if (s.learned > 0 && s.inf != 0) tip += "\nGlisser : poser sur une barre d'action";
    ImGui::SetTooltip("%s", tip.c_str());
  };

  // Menu contextuel commun : monter, lancer, niveau d'utilisation, description.
  auto context_menu = [&](const SkillRaw& s, int effective) {
    // Ouverture MANUELLE (au lieu de BeginPopupContextItem, qui ouvre sur tout clic
    // droit) : Ctrl + clic droit va droit à la description, comme dans l'inventaire,
    // l'entrepôt et le cart — sans quoi le menu s'ouvrirait DERRIÈRE elle.
    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
      if (ImGui::GetIO().KeyCtrl) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        OpenSkillDesc(s.id, static_cast<int>(mp.x), static_cast<int>(mp.y));
      } else {
        ImGui::OpenPopup("skctx");
      }
    }
    if (!ImGui::BeginPopup("skctx")) return;
    const bool can_raise = s.user_up > 0 && effective < s.maxlv;
    if (ImGui::MenuItem("Monter d'un niveau", nullptr, false, can_raise && points_left > 0))
      ReserveSkillPoint(static_cast<uint16_t>(s.id));
    if (ImGui::MenuItem("Lancer", nullptr, false, s.learned > 0 && s.inf != 0))
      SendUseSkill(static_cast<uint16_t>(s.id), std::max(1, GetUseLevelSEH(s.id)));
    // Niveau d'utilisation : réglage 100 % client (le natif l'expose par les
    // « + / − » de chaque case), borné au niveau APPRIS, et c'est lui que la barre
    // de raccourcis envoie au lancement.
    if (s.learned > 0 && IsLevelUseSkillSEH(s.id)) {
      ImGui::Separator();
      int use = GetUseLevelSEH(s.id);
      if (use <= 0 || use > s.learned) use = s.learned;
      ImGui::TextColored(kGray, "Lancer au niveau %d / %d", use, s.learned);
      if (ImGui::MenuItem("  niveau −", nullptr, false, use > 1))
        SetUseLevelSEH(s.id, use - 1);
      if (ImGui::MenuItem("  niveau +", nullptr, false, use < s.learned))
        SetUseLevelSEH(s.id, use + 1);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Description")) {
      const ImVec2 mp = ImGui::GetIO().MousePos;
      OpenSkillDesc(s.id, static_cast<int>(mp.x), static_cast<int>(mp.y));
    }
    ImGui::EndPopup();
  };

  // Gestes partagés par la case et la ligne : ils suivent le DERNIER widget soumis.
  auto common_item_actions = [&](const SkillRaw& s, int effective, ro::IconTex ic) {
    if (s.learned > 0 && s.inf != 0 && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
      // Même charge utile que l'onglet Guilde : la barre d'action ImGui l'accepte déjà.
      const int payload[2] = {s.id, std::max(1, GetUseLevelSEH(s.id))};
      ImGui::SetDragDropPayload("BGN_SKILL", payload, sizeof(payload));
      if (ic.tex) { ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(24.0f, 24.0f));
                    ImGui::SameLine(); }
      ImGui::TextUnformatted(skill_name(s.id));
      ImGui::EndDragDropSource();
    }
    if (ImGui::IsItemHovered() && ImGui::GetDragDropPayload() == nullptr) {
      hovered_now = static_cast<uint16_t>(s.id);
      tooltip_for(s, effective);
      // Clic gauche = réserver un point (Ctrl = jusqu'au niveau max). Testé au
      // RELÂCHÉ et seulement si la souris n'a PAS voyagé : le même bouton sert à
      // attraper l'icône pour la barre d'action, et un test au pressé réserverait à
      // chaque début de glisser. GetMouseDragDelta reste à (0,0) tant que le seuil
      // de glisser n'est pas franchi, et vaut encore le déplacement à la frame du
      // relâché — c'est exactement le test voulu.
      const ImVec2 travel = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
      if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
          travel.x == 0.0f && travel.y == 0.0f)
        ReserveSkillPoint(static_cast<uint16_t>(s.id), ImGui::GetIO().KeyCtrl);
    }
    context_menu(s, effective);
  };

  // Une compétence est-elle prérequis (ou suite) de celle qui est survolée ?
  auto linked_to_focus = [&](const SkillRaw& s) {
    if (focus == 0 || s.id == static_cast<int>(focus)) return false;
    for (int i = 0; i < s.need_count; ++i)
      if (s.need_id[i] == static_cast<int>(focus)) return true;
    for (int i = 0; i < count; ++i) {
      if (nodes[i].id != static_cast<int>(focus)) continue;
      for (int k = 0; k < nodes[i].need_count; ++k)
        if (nodes[i].need_id[k] == s.id) return true;
    }
    return false;
  };

  ImGui::BeginChild("cs_skill_body", ImVec2(0, 0), false);
  if (skill_grid_) {
    // ── Vue GRILLE : 7 colonnes, la disposition du client (l'index de case vient du
    //    Lua, SKILL_TREEVIEW_FOR_JOB). ICÔNES SEULES : le nom sous chaque case était
    //    plus large qu'elle et les voisines se chevauchaient — il est dans l'infobulle,
    //    là où on va le chercher. Pas de fond de case non plus : l'icône se suffit,
    //    seuls le survol et les liserés d'état posent de la couleur. ──
    // Taille des cases : la fenêtre ne descend pas sous la largeur « doll + stats »
    // (l'onglet Grimoire force le mode large), soit ~520 px de contenu — 7 colonnes de
    // 72 px les remplissent, autant en profiter pour de grosses icônes bien aérées.
    const float cell_w = 72.0f;
    const float cell_h = 78.0f;
    const float icon   = 40.0f;  // le .bmp fait 24 px : au-delà ça ramollit visiblement
    const float pad    = 10.0f;  // marge entre la case dessinée et sa voisine
    const float icon_y = 7.0f;   // hauteur du haut de l'icône dans la case
    const float split_gap = 22.0f;  // hauteur réservée au trait de séparation
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    // Ordonnée d'une ligne : les arbres fusionnés sont écartés par un trait, qui a
    // besoin de sa propre bande — sinon il mordrait sur les icônes voisines.
    auto row_y = [&](int row) {
      float y = origin.y + row * cell_h;
      for (int k = 0; k < split_count; ++k)
        if (row >= split_row[k]) y += split_gap;
      return y;
    };
    // Centre de chaque case dessinée + case survolée : les FLÈCHES de prérequis sont
    // tracées après la boucle (elles doivent passer par-dessus les icônes, et une
    // flèche relie deux cases dont l'une peut n'être dessinée que plus tard).
    static ImVec2 cell_center[kSkillMaxNodes];
    static bool   cell_drawn[kSkillMaxNodes];
    int hover_idx = -1;
    for (int i = 0; i < count; ++i) cell_drawn[i] = false;
    int used_max_row = 0;

    // Lissage des icônes : un seul basculement pour toute la grille (le callback
    // coupe le lot de draws en deux, autant ne pas le faire par case).
    g_skill_icon_bilinear = skill_bilinear_;
    if (skill_bilinear_) dl->AddCallback(CbSkillIconFilter, nullptr);

    for (int i = 0; i < count; ++i) {
      const SkillRaw& s = nodes[i];
      if (filtering && !icontains(skill_name(s.id), skill_filter_buf_)) continue;
      const int pos = disp_pos[i];
      used_max_row = std::max(used_max_row, pos / kSkillGridCols);
      const int col = pos % kSkillGridCols;
      const int row = pos / kSkillGridCols;
      const ImVec2 p(origin.x + col * cell_w, row_y(row));

      const int pending   = PendingLevel(static_cast<uint16_t>(s.id));
      const int effective = std::max(s.learned, pending);
      const bool learned  = effective > 0;
      const bool raisable = s.user_up > 0 && effective < s.maxlv && points_left > 0;

      ImGui::PushID(s.id);
      ImGui::SetCursorScreenPos(p);
      // La case EST le widget : c'est elle qui prend le clic (réserver), le clic droit
      // (menu / description) et le glisser. Rien ne se superpose plus à elle, donc
      // IsItemHovered() suffit — et lui, contrairement à IsMouseHoveringRect, sait
      // qu'un menu ouvert par-dessus n'est pas la case.
      ImGui::InvisibleButton("cell", ImVec2(cell_w - pad, cell_h - pad));
      const ImVec2 q(p.x + cell_w - pad, p.y + cell_h - pad);
      const bool hot = ImGui::IsItemHovered();
      cell_drawn[i]  = true;
      cell_center[i] = ImVec2((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);
      if (hot) hover_idx = i;
      // Le survol pose un voile léger — c'est un retour de curseur, pas un fond.
      if (hot) dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, 28), 4.0f);
      // ⚠ AddRect ici, c'est (rounding, thickness) — le paramètre `flags` vient APRÈS
      // (l'ordre inverse est l'ancienne signature, gardée en surcharge obsolète).
      if (s.id == static_cast<int>(focus)) dl->AddRect(p, q, IM_COL32(255, 205, 105, 220), 4.0f, 2.0f);
      else if (linked_to_focus(s))         dl->AddRect(p, q, IM_COL32(120, 160, 255, 200), 4.0f, 2.0f);
      else if (raisable)                   dl->AddRect(p, q, IM_COL32(120, 200, 130, 170), 4.0f, 1.5f);

      // Icône centrée, assombrie tant que la compétence n'est pas apprise — c'est
      // ce que fait le natif (mode de blit grisé quand le niveau vaut 0).
      const ro::IconTex ic = ResolveSkillIcon(s.id);
      const ImVec2 ip(p.x + (cell_w - pad - icon) * 0.5f, p.y + icon_y);
      if (ic.tex)
        dl->AddImage(reinterpret_cast<ImTextureID>(ic.tex), ip,
                     ImVec2(ip.x + icon, ip.y + icon), ImVec2(0, 0), ImVec2(1, 1),
                     learned ? IM_COL32_WHITE : IM_COL32(105, 105, 115, 165));

      // Niveau sous l'icône : la seule information qu'on ne peut pas deviner du dessin.
      // NOIR dès qu'elle est apprise (le vert « niveau max » se noyait dans le fond
      // clair du skin), gris quand elle ne l'est pas, ambre quand un point est réservé.
      // Dans tous les cas un LISERÉ clair de 1 px : le texte passe parfois sur le bas
      // d'une icône, et sans contour il devient illisible pile à cet endroit.
      char lvl[24];
      if (s.maxlv > 0) std::snprintf(lvl, sizeof(lvl), "%d/%d", effective, s.maxlv);
      else             std::snprintf(lvl, sizeof(lvl), "%d", effective);
      const ImVec2 lsz = ImGui::CalcTextSize(lvl);
      const ImVec2 lp(p.x + (cell_w - pad - lsz.x) * 0.5f, p.y + icon_y + icon + 2.0f);
      const ImU32 lvl_col = pending > 0      ? IM_COL32(150, 95, 0, 255)
                            : effective > 0  ? IM_COL32(20, 20, 25, 255)
                                             : IM_COL32(110, 110, 122, 255);
      const ImU32 halo = IM_COL32(255, 255, 255, 190);
      dl->AddText(ImVec2(lp.x - 1.0f, lp.y), halo, lvl);
      dl->AddText(ImVec2(lp.x + 1.0f, lp.y), halo, lvl);
      dl->AddText(ImVec2(lp.x, lp.y - 1.0f), halo, lvl);
      dl->AddText(ImVec2(lp.x, lp.y + 1.0f), halo, lvl);
      dl->AddText(lp, lvl_col, lvl);

      common_item_actions(s, effective, ic);
      // Plus de bouton « + » ici : le clic gauche sur la case réserve désormais un
      // point (Ctrl = jusqu'au max). Un bouton posé PAR-DESSUS la case obligeait à
      // AllowOverlap, et son clic passait quand même à la case en dessous.
      ImGui::PopID();
    }

    // Restaurer le filtre net : la suite (skin, scrollbar) est dessinée dans la même
    // draw list et hériterait sinon du lissage.
    if (skill_bilinear_) dl->AddCallback(CbSkillIconFilterOff, nullptr);

    // ── Séparateur entre deux arbres fusionnés (trait rouge + libellé) ────────
    for (int k = 0; k < split_count; ++k) {
      const float y  = row_y(split_row[k]) - split_gap * 0.5f;
      const float x0 = origin.x;
      const float x1 = origin.x + kSkillGridCols * cell_w - pad;
      const ImU32 line_col = IM_COL32(200, 60, 60, 200);
      const char* txt = split_label[k] ? split_label[k] : "Suite";
      const ImVec2 tsz = ImGui::CalcTextSize(txt);
      const float tx = x0 + 14.0f;
      dl->AddLine(ImVec2(x0, y), ImVec2(tx - 6.0f, y), line_col, 2.0f);
      dl->AddLine(ImVec2(tx + tsz.x + 6.0f, y), ImVec2(x1, y), line_col, 2.0f);
      dl->AddText(ImVec2(tx, y - tsz.y * 0.5f), IM_COL32(170, 40, 40, 255), txt);
    }

    // ── Flèches de dépendance, seulement autour de la case survolée ──────────
    // Les tracer en permanence ferait un plat de spaghettis ; au survol, elles
    // répondent exactement à la question qu'on se pose à ce moment-là : « d'où vient
    // cette compétence, et qu'ouvre-t-elle ? ». Ambre = ce qu'il FAUT avant (le
    // prérequis pointe vers la survolée), bleu = ce qu'elle ouvre.
    if (hover_idx >= 0) {
      // La flèche part du BORD des cases, pas de leur centre : sous l'icône elle
      // serait cachée, et sa pointe doit rester lisible.
      auto draw_arrow = [&](const ImVec2& from, const ImVec2& to, ImU32 col,
                            float thickness) {
        ImVec2 d(to.x - from.x, to.y - from.y);
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        const float trim = (cell_w - pad) * 0.45f;  // rayon approché de la case
        if (len <= trim * 2.0f + 8.0f) return;      // cases voisines : rien à tracer
        d.x /= len;
        d.y /= len;
        const ImVec2 a(from.x + d.x * trim, from.y + d.y * trim);
        const ImVec2 b(to.x - d.x * trim, to.y - d.y * trim);
        dl->AddLine(a, b, col, thickness);
        const float head = 8.0f;
        const ImVec2 n(-d.y, d.x);  // normale, pour écarter les deux ailes
        dl->AddTriangleFilled(
            b, ImVec2(b.x - d.x * head + n.x * head * 0.5f, b.y - d.y * head + n.y * head * 0.5f),
            ImVec2(b.x - d.x * head - n.x * head * 0.5f, b.y - d.y * head - n.y * head * 0.5f),
            col);
      };
      const SkillRaw& h = nodes[hover_idx];
      auto find_node = [&](int id) -> const SkillRaw* {
        for (int i = 0; i < count; ++i)
          if (nodes[i].id == id) return &nodes[i];
        return nullptr;
      };
      auto requires_skill = [&](const SkillRaw* n, int id) {
        if (!n) return false;
        for (int k = 0; k < n->need_count; ++k)
          if (n->need_id[k] == id) return true;
        return false;
      };
      // RÉDUCTION TRANSITIVE. La liste de prérequis du client n'est pas limitée aux
      // liens directs : elle est aplatie (c'est ce qui permet au natif de réserver
      // toute une chaîne d'un coup, et pourquoi il la déduplique en gardant le niveau
      // le plus haut). Tracée telle quelle, Bowling Bash pointerait vers Bash à la fois
      // directement et via Magnum Break. On saute donc P -> survolée quand un AUTRE
      // prérequis de la survolée réclame déjà P : le chemin est déjà à l'écran.
      auto redundant_before = [&](int prereq_id) {
        for (int a = 0; a < h.need_count; ++a) {
          if (h.need_id[a] == prereq_id) continue;
          if (requires_skill(find_node(h.need_id[a]), prereq_id)) return true;
        }
        return false;
      };
      // Même raisonnement dans l'autre sens, pour un lien `prereq_id -> dependent` :
      // si `dependent` réclame aussi une compétence qui réclame déjà `prereq_id`, le
      // trait direct doublerait un chemin déjà tracé.
      auto redundant_after = [&](const SkillRaw& dependent, int prereq_id) {
        for (int k = 0; k < dependent.need_count; ++k) {
          if (dependent.need_id[k] == prereq_id) continue;
          if (requires_skill(find_node(dependent.need_id[k]), prereq_id)) return true;
        }
        return false;
      };
      // 1) ce qu'il faut AVANT elle : chaque prérequis pointe vers la survolée.
      for (int k = 0; k < h.need_count; ++k) {
        if (redundant_before(h.need_id[k])) continue;
        for (int i = 0; i < count; ++i) {
          if (!cell_drawn[i] || nodes[i].id != h.need_id[k]) continue;
          draw_arrow(cell_center[i], cell_center[hover_idx], IM_COL32(255, 205, 105, 235), 2.5f);
          break;
        }
      }
      // 2) ce qu'elle OUVRE, en CHAÎNE : la survolée pointe vers celles qui la
      //    réclament, puis celles-ci vers leurs propres suites, etc. S'arrêter au
      //    premier rang ne montrait qu'un bout de la branche — or c'est justement la
      //    suite du chemin qu'on cherche en survolant une compétence de départ.
      //    Parcours en largeur, chaque case n'étant développée qu'une fois (le graphe
      //    a des raccourcis : sans marquage, une même case serait redéveloppée à
      //    chaque profondeur). Le trait pâlit et s'affine avec la distance, pour que
      //    l'ordre de la chaîne se lise d'un coup d'œil.
      static int  bfs_queue[kSkillMaxNodes];
      static int  bfs_depth[kSkillMaxNodes];
      static bool bfs_seen[kSkillMaxNodes];
      for (int i = 0; i < count; ++i) bfs_seen[i] = false;
      int head_q = 0, tail_q = 0;
      bfs_queue[tail_q] = hover_idx;
      bfs_depth[tail_q++] = 0;
      bfs_seen[hover_idx] = true;
      while (head_q < tail_q) {
        const int cur   = bfs_queue[head_q];
        const int depth = bfs_depth[head_q];
        ++head_q;
        if (depth >= 6) continue;  // garde-fou : un arbre de job n'est jamais si profond
        for (int i = 0; i < count; ++i) {
          if (i == cur || !requires_skill(&nodes[i], nodes[cur].id)) continue;
          if (redundant_after(nodes[i], nodes[cur].id)) continue;
          if (cell_drawn[i] && cell_drawn[cur]) {
            const int fade = depth * 35;
            draw_arrow(cell_center[cur], cell_center[i],
                       IM_COL32(120, 160, 255, std::max(90, 200 - fade)),
                       std::max(1.2f, 2.0f - depth * 0.25f));
          }
          if (!bfs_seen[i]) {
            bfs_seen[i] = true;
            bfs_queue[tail_q] = i;
            bfs_depth[tail_q++] = depth + 1;
          }
        }
      }
    }

    // Réserver la hauteur consommée : la grille est dessinée en absolu, ImGui ne
    // connaîtrait sinon aucune étendue et le scroll serait mort.
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(kSkillGridCols * cell_w,
                        (used_max_row + 1) * cell_h + split_count * split_gap));
  } else {
    // ── Vue LISTE : tout ce que la grille doit résumer, en clair ──
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                  ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("cs_skill_tbl", 5, flags)) {
      ImGui::TableSetupColumn("Compétence", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Niveau", ImGuiTableColumnFlags_WidthFixed, 56.0f);
      ImGui::TableSetupColumn("SP", ImGuiTableColumnFlags_WidthFixed, 40.0f);
      ImGui::TableSetupColumn("Portée", ImGuiTableColumnFlags_WidthFixed, 46.0f);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 28.0f);
      ImGui::TableHeadersRow();
      const float icon = ImGui::GetTextLineHeight();
      for (int i = 0; i < count; ++i) {
        const SkillRaw& s = nodes[i];
        if (filtering && !icontains(skill_name(s.id), skill_filter_buf_)) continue;
        const int pending   = PendingLevel(static_cast<uint16_t>(s.id));
        const int effective = std::max(s.learned, pending);
        ImGui::PushID(s.id);
        ImGui::TableNextRow();
        if (s.id == static_cast<int>(focus))           ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                                            IM_COL32(120, 95, 35, 90));
        else if (linked_to_focus(s)) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                                            IM_COL32(70, 75, 120, 80));
        ImGui::TableNextColumn();
        const ro::IconTex ic = ResolveSkillIcon(s.id);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ic.tex)
          ImGui::GetWindowDrawList()->AddImage(
              reinterpret_cast<ImTextureID>(ic.tex), p, ImVec2(p.x + icon, p.y + icon),
              ImVec2(0, 0), ImVec2(1, 1),
              effective > 0 ? IM_COL32_WHITE : IM_COL32(110, 110, 110, 160));
        ImGui::Dummy(ImVec2(icon, icon));
        ImGui::SameLine();
        if (effective == 0) ImGui::PushStyleColor(ImGuiCol_Text, kGray);
        // Selectable (widget À ID) plutôt qu'un simple texte : c'est lui qui donne
        // l'ActiveId nécessaire au glisser et la zone de clic du menu contextuel.
        ImGui::Selectable(skill_name(s.id));
        if (effective == 0) ImGui::PopStyleColor();
        common_item_actions(s, effective, ic);

        ImGui::TableNextColumn();
        if (pending > 0) ImGui::TextColored(kAmber, "%d/%d", effective, s.maxlv);
        else if (effective > 0) ImGui::Text("%d/%d", effective, s.maxlv);
        else ImGui::TextColored(kGray, "-/%d", s.maxlv);

        ImGui::TableNextColumn();
        if (s.inf == 0)          ImGui::TextColored(kGray, "passif");
        else if (s.learned > 0)  ImGui::Text("%d", s.sp);
        else                     ImGui::TextColored(kGray, "-");

        ImGui::TableNextColumn();
        if (s.learned > 0 && s.range > 0) ImGui::Text("%d", s.range);
        else                              ImGui::TextColored(kGray, "-");

        ImGui::TableNextColumn();
        if (s.user_up > 0 && effective < s.maxlv && points_left > 0) {
          if (ro::RoSmallButton("+", 22.0f, 0.0f))
            ReserveSkillPoint(static_cast<uint16_t>(s.id), ImGui::GetIO().KeyCtrl);
          mui::Tooltip("Réserve un point (et ses prérequis) ; Ctrl = jusqu'au niveau\n"
                       "maximum. « Appliquer » valide.");
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  }
  ImGui::EndChild();
  skill_hover_ = hovered_now;  // consommé à la frame suivante (surlignage des liens)
}

// Onglet Guilde : la fenêtre de guilde native (les 7 panneaux UIGuildWnd) refaite en
// ImGui dans la feuille de perso. Tout est lu LIVE des globals du client (CGuild +
// g_GuildInfo_*, cf. project_guild_window_re) ; les actions partent en paquets bruts,
// exactement comme le natif, et le serveur revalide chaque droit.
void CharacterSheet::DrawGuildTab() {
  const ImVec4 kGray(0.35f, 0.35f, 0.42f, 1.0f);
  const ImVec4 kGreen(0.10f, 0.50f, 0.15f, 1.0f);
  const ImVec4 kRed(0.60f, 0.12f, 0.12f, 1.0f);
  const ImVec4 kBlue(0.15f, 0.25f, 0.60f, 1.0f);

  GuildInfo gi{};
  if (!ReadGuild(&gi)) {
    // Sans guilde : même service que le « Guild Companion » natif (Alt+G), à savoir
    // la création directe, sans passer par ses deux fenêtres.
    ImGui::TextColored(kGray, "Tu n'appartiens à aucune guilde.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Rejoins-en une (invitation d'un maître de guilde) ou crée la tienne "
        "ici : cet onglet affichera ensuite membres, postes et relations.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(kBlack, "Créer une guilde");
    ImGui::TextColored(kGray,
                       "Un Emperium dans l'inventaire est nécessaire, et la carte ne "
                       "doit pas interdire les guildes.");
    ImGui::SetNextItemWidth(220.0f);
    const bool name_submitted =
        ro::InputTextCp949("##cs_guild_create", guild_create_buf_, sizeof(guild_create_buf_),
                           ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool create_clicked = ro::RoButton("Créer");
    if ((name_submitted || create_clicked) && guild_create_buf_[0]) {
      SendCreateGuild(guild_create_buf_);
      guild_create_result_ = -1;  // en attente de la réponse serveur
      guild_status_ = std::string("Demande envoyée : ") + guild_create_buf_;
    }
    // Retour du serveur (ZC 0x0167). Le client affiche déjà sa propre boîte ; on
    // double l'information ici pour ne pas laisser l'onglet muet.
    if (guild_create_result_ >= 0) {
      const char* result_text = "Résultat inconnu.";
      switch (guild_create_result_) {
        case 0: result_text = "Guilde créée."; break;
        case 1: result_text = "Tu es déjà dans une guilde."; break;
        case 2: result_text = "Ce nom de guilde est déjà pris."; break;
        case 3: result_text = "Il te faut un Emperium pour créer une guilde."; break;
        default: break;
      }
      ImGui::TextColored(guild_create_result_ == 0 ? ImVec4(0.10f, 0.50f, 0.15f, 1.0f)
                                                   : ImVec4(0.60f, 0.12f, 0.12f, 1.0f),
                         "%s", result_text);
    } else if (!guild_status_.empty()) {
      ImGui::TextColored(kGray, "%s", guild_status_.c_str());
    }
    return;
  }

  // La liste des membres n'est PAS poussée spontanément par le serveur : elle
  // arrive sur demande (CZ_REQ_GUILD_MENUINTERFACE), comme quand on ouvre la
  // fenêtre native. On la redemande à l'ouverture de l'onglet puis toutes les 30 s.
  // Les membres d'abord, les postes ENSUITE : la liste des membres recrée les
  // enregistrements avec un nom de poste vide, que le type 2 (noms + droits des
  // postes) vient justement remplir. Le type 0 termine par les infos de base.
  const unsigned long now_tick = GetTickCount();
  if (guild_last_req_ == 0 || now_tick - guild_last_req_ > 30000) {
    SendGuildRequest(kGuildReqMembers);
    SendGuildRequest(kGuildReqPositions);
    SendGuildRequest(kGuildReqSkills);
    SendGuildRequest(kGuildReqBans);
    SendGuildRequest(kGuildReqBasic);
    guild_last_req_ = now_tick;
  }

  static GuildRoster roster;  // POD (~76 entrées) relu à chaque frame : lecture pure
  ReadGuildRosterSEH(&roster);
  for (int i = 0; i < roster.count; ++i)
    RememberGuildPosition(roster.members[i].position_id, roster.members[i].position);

  // ── Droits du joueur ──────────────────────────────────────────────────────
  // Le serveur ne regarde PAS le drapeau maître pour expulser/inviter, mais le
  // masque de droits du POSTE occupé (guild_has_permission). On reproduit la même
  // règle ; tant que les droits ne sont pas connus, on retombe sur « maître ».
  // Le drapeau natif 0x0159c23c est « collant » (posé une fois, jamais remis à 0),
  // donc on préfère comparer les noms quand le maître est connu.
  const std::string own_name = Bourgeon::Instance().client().session().GetCharName();
  const bool is_master = (gi.master_name[0] && !own_name.empty())
                             ? _stricmp(own_name.c_str(), gi.master_name) == 0
                             : gi.master;
  const bool my_mode_known = gi.position_found && gi.position_id >= 0 &&
                             gi.position_id < kGuildPositionSlots &&
                             guild_positions_[gi.position_id].has_info;
  const int  my_mode   = my_mode_known ? guild_positions_[gi.position_id].mode : 0;
  const bool can_expel = my_mode_known ? (my_mode & kGuildPermExpel) != 0 : is_master;
  const bool can_invite = my_mode_known ? (my_mode & kGuildPermInvite) != 0 : is_master;

  // ── En-tête : emblème + identité + jauge d'expérience ──────────────────────
  const ImVec2 header_pos = ImGui::GetCursorScreenPos();
  // « Actualiser » ancré en haut à droite : on pose le bouton avant l'en-tête puis
  // on remet le curseur où il était, si bien que l'en-tête se dessine ensuite comme
  // si le bouton n'occupait aucune place.
  {
    const float  refresh_width = 90.0f;
    const ImVec2 saved_cursor = ImGui::GetCursorPos();
    ImGui::SetCursorPosX(saved_cursor.x + ImGui::GetContentRegionAvail().x - refresh_width);
    if (ro::RoButton("Actualiser", refresh_width, 0.0f)) guild_last_req_ = 0;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Redemande au serveur membres, postes et infos de guilde.");
    ImGui::SetCursorPos(saved_cursor);
  }
  const float  emblem_size = 48.0f;
  ro::IconTex  emblem = ResolveEmblem(gi.guildId);
  const ImVec2 emblem_min(header_pos.x, header_pos.y + 2.0f);
  const ImVec2 emblem_max(emblem_min.x + emblem_size, emblem_min.y + emblem_size);
  if (emblem.tex) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(emblem_min, emblem_max, IM_COL32(0, 0, 0, 30), 4.0f);
    dl->AddImage(reinterpret_cast<ImTextureID>(emblem.tex), emblem_min, emblem_max);
    dl->AddRect(emblem_min, emblem_max, IM_COL32(90, 90, 110, 220), 4.0f, 0, 1.5f);
  }
  // L'emblème lui-même ouvre le changement d'emblème (réservé au maître, comme côté
  // serveur). Bouton posé AVANT les lignes de texte puis curseur restauré : l'en-tête
  // se dispose ensuite comme si la zone cliquable n'existait pas.
  if (is_master) {
    const ImVec2 saved_cursor = ImGui::GetCursorPos();
    ImGui::SetCursorScreenPos(emblem_min);
    if (ImGui::InvisibleButton("##cs_guild_emblem_click", ImVec2(emblem_size, emblem_size)))
      guild_emblem_ask_ = true;
    if (ImGui::IsItemHovered()) {
      ImGui::GetWindowDrawList()->AddRect(emblem_min, emblem_max, IM_COL32(255, 220, 120, 255),
                                          4.0f, 0, 2.0f);
      ImGui::SetTooltip("Changer l'emblème de la guilde…");
    }
    ImGui::SetCursorPos(saved_cursor);
  }
  ImGui::Indent(emblem_size + 10.0f);
  ImGui::TextColored(kBlack, "%s", gi.name);
  ImGui::TextColored(kGray, "Niveau %d  ·  Maître : %s", gi.level,
                     gi.master_name[0] ? gi.master_name : "?");
  // Membres : le total vient du roster (une entrée par membre), le nombre de
  // connectés du paquet d'infos (connect_member).
  ImGui::TextColored(kGray, "Membres : %d / %d  ·  En ligne : %d  ·  Niveau moyen : %d",
                     roster.count, gi.member_max, gi.online, gi.avg_level);
  // Poste occupé + droits qui en découlent : explique la présence (ou l'absence)
  // des actions plus bas, au lieu de laisser le serveur refuser en silence.
  if (gi.position_found) {
    const char* my_position = GuildPositionLabel(gi.position_id, gi.pos);
    char rights[80];
    if (my_mode_known) {
      std::snprintf(rights, sizeof(rights), "%s%s%s",
                    (my_mode & kGuildPermInvite) ? "inviter " : "",
                    (my_mode & kGuildPermExpel) ? "expulser " : "",
                    (my_mode & kGuildPermStorage) ? "storage" : "");
      if (rights[0] == '\0') std::snprintf(rights, sizeof(rights), "aucun droit");
    } else {
      std::snprintf(rights, sizeof(rights), "droits inconnus");
    }
    ImGui::TextColored(kGray, "Ton poste : %s  ·  %s",
                       my_position ? my_position : "?", rights);
  }
  ImGui::Unindent(emblem_size + 10.0f);
  if (gi.land[0]) ImGui::TextColored(kGray, "Territoire : %s", gi.land);

  // Jauge d'EXP de guilde (exp / exp du niveau suivant).
  if (gi.next_exp > 0) {
    char exp_label[64];
    const float ratio = std::clamp(static_cast<float>(gi.exp) /
                                       static_cast<float>(gi.next_exp), 0.0f, 1.0f);
    std::snprintf(exp_label, sizeof(exp_label), "EXP %d / %d  (%.1f %%)", gi.exp,
                  gi.next_exp, ratio * 100.0f);
    ImGui::ProgressBar(ratio, ImVec2(-1.0f, 14.0f), exp_label);
  }

  // ── Annonce (sujet + message), éditable par le maître ─────────────────────
  ImGui::Spacing();
  if (guild_notice_edit_) {
    // Le rappel « titre, puis message » vaut aussi en RÉÉDITION : les indices ne
    // s'affichent que sur un champ vide, donc ils ne diraient rien sur une annonce
    // déjà remplie — exactement le cas où l'on hésite.
    ImGui::TextColored(kBlack, "Annonce de la guilde — titre, puis message");
    // Deux champs identiques l'un au-dessus de l'autre : rien ne disait lequel est le
    // titre. L'indice le dit là où on tape, sans voler une ligne de libellé.
    ImGui::SetNextItemWidth(-1.0f);
    ro::InputTextCp949WithHint("##cs_guild_subj", "Titre de l'annonce",
                               guild_notice_subj_, sizeof(guild_notice_subj_));
    ImGui::SetNextItemWidth(-1.0f);
    ro::InputTextCp949WithHint("##cs_guild_body", "Contenu du message",
                               guild_notice_body_, sizeof(guild_notice_body_));
    if (ro::RoButton("Enregistrer", 110.0f, 0.0f)) {
      SendGuildNotice(gi.guildId, guild_notice_subj_, guild_notice_body_);
      guild_notice_edit_ = false;
      guild_status_ = "Annonce envoyée.";
    }
    ImGui::SameLine();
    if (ro::RoButton("Annuler", 90.0f, 0.0f)) guild_notice_edit_ = false;
  } else if (gi.notice_subject[0] || gi.notice_body[0]) {
    ImGui::TextColored(kBlue, "%s", gi.notice_subject[0] ? gi.notice_subject : "Annonce");
    if (gi.notice_body[0]) ImGui::TextWrapped("%s", gi.notice_body);
  }
  if (!guild_notice_edit_ && is_master) {
    if (ro::RoButton("Modifier l'annonce")) {
      std::strncpy(guild_notice_subj_, gi.notice_subject, sizeof(guild_notice_subj_) - 1);
      guild_notice_subj_[sizeof(guild_notice_subj_) - 1] = '\0';
      std::strncpy(guild_notice_body_, gi.notice_body, sizeof(guild_notice_body_) - 1);
      guild_notice_body_[sizeof(guild_notice_body_) - 1] = '\0';
      guild_notice_edit_ = true;
    }
    ImGui::SameLine();
    // Doublon volontaire du clic sur l'emblème : personne ne devine qu'une image est
    // cliquable, et c'est ici que se trouvent les autres actions de maître.
    if (ro::RoButton("Changer l'emblème…")) guild_emblem_ask_ = true;
  }

  // ── Entrepôt de guilde ─────────────────────────────────────────────────────
  // Alias de @guildstorage, et rien d'autre : le serveur n'a AUCUN paquet pour
  // l'ouvrir, et la commande est déjà accordée au groupe 0 (conf/import/groups.yml).
  // Ce bouton ne donne donc aucun droit nouveau — il évite juste d'aller taper.
  // Comme la commande, il BASCULE : un 2e appel referme l'entrepôt ouvert.
  if (!guild_notice_edit_) {
    // Droits inconnus (paquet de postes pas encore reçu) : on laisse cliquer, le
    // serveur revérifie GUILD_PERM_STORAGE et répond son propre refus.
    const bool may_storage = !my_mode_known || (my_mode & kGuildPermStorage) != 0;
    ImGui::BeginDisabled(!may_storage);
    ImGui::SameLine();
    if (ro::RoButton("Storage de guilde")) {
      SendAtCommand(kCmdGuildStorage);
      guild_status_ = "Storage de guilde : ouverture demandée.";
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(may_storage
                            ? "Ouvre — ou referme — le Storage de guilde (@guildstorage)."
                            : "Ton poste n'a pas le droit « storage ».");
  }

  ImGui::Separator();

  // ── Sous-onglets Membres / Postes / Relations ─────────────────────────────
  if (ImGui::BeginTabBar("cs_guild_sub")) {
    if (ImGui::BeginTabItem("Membres"))     { guild_sub_tab_ = 0; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Postes"))      { guild_sub_tab_ = 2; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Compétences")) { guild_sub_tab_ = 3; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Relations"))   { guild_sub_tab_ = 1; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Expulsions"))  { guild_sub_tab_ = 4; ImGui::EndTabItem(); }
    ImGui::EndTabBar();
  }

  // Hauteur laissée à la liste : tout sauf la barre d'actions du bas.
  const float actions_h = ImGui::GetFrameHeightWithSpacing() +
                          ImGui::GetTextLineHeightWithSpacing() + 6.0f;
  const float list_h = std::max(80.0f, ImGui::GetContentRegionAvail().y - actions_h);

  if (guild_sub_tab_ == 0) {
    // Postes proposés dans le menu « Changer de poste », pris dans la mémoire des
    // libellés vus (cf. g_guild_position_names). Le poste 0 (maître) en est EXCLU :
    // côté serveur, l'affecter déclenche guild_gm_change, c'est-à-dire le TRANSFERT
    // de la direction de la guilde — pas un simple changement de rang.
    struct KnownPosition { int id; const char* label; };
    KnownPosition known_positions[32];
    int known_count = 0;
    for (int id = 1; id < kGuildPositionSlots && known_count < 32; ++id) {
      if (!guild_positions_[id].has_name || !guild_positions_[id].name[0]) continue;
      known_positions[known_count++] = {id, guild_positions_[id].name};
    }
    if (known_count == 0) {  // repli : aucun paquet de postes reçu pour l'instant
      for (const auto& entry : g_guild_position_names) {
        if (entry.first <= 0 || known_count >= 32) continue;
        known_positions[known_count++] = {entry.first, entry.second.c_str()};
      }
    }
    std::sort(known_positions, known_positions + known_count,
              [](const KnownPosition& a, const KnownPosition& b) { return a.id < b.id; });

    // Vue triable (indices sur le roster) : le tri suit les en-têtes de colonnes.
    static std::vector<int> order;
    order.resize(roster.count);
    for (int i = 0; i < roster.count; ++i) order[i] = i;

    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("cs_guild_members", 6, table_flags, ImVec2(0.0f, list_h))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Nom", ImGuiTableColumnFlags_WidthStretch |
                                         ImGuiTableColumnFlags_DefaultSort);
      ImGui::TableSetupColumn("Classe", ImGuiTableColumnFlags_WidthFixed, 88.0f);
      ImGui::TableSetupColumn("Nv", ImGuiTableColumnFlags_WidthFixed, 32.0f);
      ImGui::TableSetupColumn("Poste", ImGuiTableColumnFlags_WidthFixed, 84.0f);
      ImGui::TableSetupColumn("Contrib.", ImGuiTableColumnFlags_WidthFixed, 70.0f);
      ImGui::TableSetupColumn("Connexion", ImGuiTableColumnFlags_WidthFixed, 104.0f);
      ImGui::TableHeadersRow();

      if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsCount > 0) {
          const int  column = specs->Specs[0].ColumnIndex;
          const bool ascending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
          std::sort(order.begin(), order.end(), [&](int lhs, int rhs) {
            const GuildMember& a = roster.members[lhs];
            const GuildMember& b = roster.members[rhs];
            int cmp = 0;
            switch (column) {
              case 1: cmp = std::strcmp(JobName(a.job), JobName(b.job)); break;
              case 2: cmp = a.level - b.level; break;
              case 3: cmp = a.position_id - b.position_id; break;
              case 4: cmp = (a.contribution > b.contribution) - (a.contribution < b.contribution);
                      break;
              case 5: cmp = (a.online != b.online)
                                ? (a.online ? 1 : -1)
                                : ((a.last_login > b.last_login) - (a.last_login < b.last_login));
                      break;
              default: cmp = _stricmp(a.name, b.name); break;
            }
            if (cmp == 0) cmp = _stricmp(a.name, b.name);
            return ascending ? cmp < 0 : cmp > 0;
          });
        }
      }

      // Comme le natif : ligne teintée en vert et miniature de tête pour les membres
      // CONNECTÉS uniquement (le rendu natif d'une ligne fait les deux sous le même
      // test `record+0x70 == 1`).
      const float head_box = ImGui::GetTextLineHeight() + 6.0f;
      for (int slot = 0; slot < static_cast<int>(order.size()); ++slot) {
        const GuildMember& m = roster.members[order[slot]];
        ImGui::PushID(static_cast<int>(m.cid ? m.cid : m.aid));
        ImGui::TableNextRow(ImGuiTableRowFlags_None, head_box);
        if (m.online)
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(198, 232, 198, 160));
        ImGui::TableSetColumnIndex(0);
        const ImVec2 name_cell = ImGui::GetCursorScreenPos();
        const bool selected = (guild_sel_cid_ != 0 && guild_sel_cid_ == m.cid);
        // Retrait en tête du libellé : la tête est peinte PAR-DESSUS en coordonnées
        // écran (jamais de SetCursorPos sur un Selectable large).
        char row_label[64];
        std::snprintf(row_label, sizeof(row_label), "%s%s", m.online ? "      " : "",
                      m.name);
        if (ImGui::Selectable(row_label, selected, ImGuiSelectableFlags_SpanAllColumns))
          guild_sel_cid_ = m.cid;
        if (m.online)
          ro::DrawHeadIcon(ImGui::GetWindowDrawList(), name_cell.x, name_cell.y - 2.0f,
                           head_box, m.hair, m.sex, m.hair_color);

        // Menu contextuel : actions sur CE membre (le serveur revérifie les droits).
        if (ImGui::BeginPopupContextItem("cs_guild_member_ctx")) {
          guild_sel_cid_ = m.cid;
          ImGui::TextColored(kGray, "%s", m.name);
          ImGui::Separator();
          if (ImGui::MenuItem("Copier le nom")) ImGui::SetClipboardText(m.name);
          // « Envoyer un courrier », comme le « Send a mail... » du menu natif : le
          // destinataire part déjà rempli. Jamais vers soi-même (le serveur refuse).
          {
            const bool self = !own_name.empty() && _stricmp(m.name, own_name.c_str()) == 0;
            if (!self && ImGui::MenuItem("Envoyer un courrier…")) {
              if (RodexTweaks* rodex = Bourgeon::Instance().rodex_tweaks())
                rodex->ComposeTo(m.name);
              guild_status_ = std::string("Courrier à ") + m.name;
            }
          }
          if (is_master && known_count > 0 && m.position_id != 0) {
            if (ImGui::BeginMenu("Changer de poste")) {
              for (int k = 0; k < known_count; ++k) {
                // PushID sur l'id du poste : deux postes peuvent porter le MÊME
                // libellé, et MenuItem dérive son ID du libellé (conflit d'ID ImGui).
                ImGui::PushID(known_positions[k].id);
                const bool current = known_positions[k].id == m.position_id;
                if (ImGui::MenuItem(known_positions[k].label, nullptr, current) && !current) {
                  SendGuildChangePosition(m.aid, m.cid, known_positions[k].id);
                  guild_status_ = std::string(m.name) + " -> " + known_positions[k].label;
                  guild_last_req_ = 0;  // force un rafraîchissement à la frame suivante
                }
                ImGui::PopID();
              }
              ImGui::EndMenu();
            }
          }
          // « Expulser » n'apparaît que si le POSTE du joueur porte le droit
          // correspondant (le serveur teste guild_has_permission, pas le drapeau
          // maître), et jamais sur le maître de guilde (refusé) ni sur soi-même
          // (c'est « Quitter » qu'il faut, pas une auto-expulsion).
          const bool target_is_master =
              gi.master_name[0] && _stricmp(m.name, gi.master_name) == 0;
          const bool target_is_self = !own_name.empty() && _stricmp(m.name, own_name.c_str()) == 0;
          if (can_expel && !target_is_master && !target_is_self) {
            ImGui::Separator();
            // L'ouverture du modal est DIFFÉRÉE : ici la pile d'ID est celle du menu
            // contextuel (+ le PushID de la ligne), donc un OpenPopup ne matcherait pas
            // le BeginRoPopupModal ouvert au niveau de l'onglet.
            if (ImGui::MenuItem("Expulser…")) {
              guild_expel_aid_ = m.aid;
              guild_expel_cid_ = m.cid;
              std::strncpy(guild_expel_name_, m.name, sizeof(guild_expel_name_) - 1);
              guild_expel_name_[sizeof(guild_expel_name_) - 1] = '\0';
              guild_reason_buf_[0] = '\0';
              guild_expel_ask_ = true;
              ImGui::CloseCurrentPopup();
            }
          }
          ImGui::EndPopup();
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(kBlack, "%s", JobName(m.job));
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(kBlack, "%d", m.level);
        ImGui::TableSetColumnIndex(3);
        const char* position_label = GuildPositionLabel(m.position_id, m.position);
        if (m.position_id == 0)
          ImGui::TextColored(kBlue, "%s", position_label ? position_label : "Maître");
        else
          ImGui::TextColored(kBlack, "%s", position_label ? position_label : "—");
        ImGui::TableSetColumnIndex(4);
        ImGui::TextColored(kBlack, "%d", m.contribution);
        ImGui::TableSetColumnIndex(5);
        if (m.online) {
          ImGui::TextColored(kGreen, "En ligne");
        } else {
          char seen[32] = "—";
          if (m.last_login != 0) {
            const time_t stamp = static_cast<time_t>(m.last_login);
            struct tm local_time = {};
            if (localtime_s(&local_time, &stamp) == 0)
              std::strftime(seen, sizeof(seen), "%d/%m/%y %H:%M", &local_time);
          }
          ImGui::TextColored(kGray, "%s", seen);
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  } else if (guild_sub_tab_ == 2) {
    ImGui::BeginChild("cs_guild_positions", ImVec2(0.0f, list_h), true);
    DrawGuildPositionsTab(is_master);
    ImGui::EndChild();
  } else if (guild_sub_tab_ == 3) {
    ImGui::BeginChild("cs_guild_skills", ImVec2(0.0f, list_h), true);
    DrawGuildSkillsTab();
    ImGui::EndChild();
  } else if (guild_sub_tab_ == 4) {
    ImGui::BeginChild("cs_guild_bans", ImVec2(0.0f, list_h), true);
    DrawGuildBansTab();
    ImGui::EndChild();
  } else {
    // ── Relations : alliés et ennemis (même liste, champ `relation`) ─────────
    static GuildRelations relations;
    ReadGuildRelationsSEH(&relations);
    ImGui::BeginChild("cs_guild_rel", ImVec2(0.0f, list_h), true);
    // Rompre une relation exige le drapeau maître côté serveur : la croix et le menu
    // contextuel n'apparaissent donc que pour le maître (comme le « Delete » du natif).
    const bool can_break = is_master;
    for (int pass = 0; pass < 2; ++pass) {
      ImGui::TextColored(pass == 0 ? kGreen : kRed, pass == 0 ? "Alliés" : "Ennemis");
      int shown = 0;
      for (int i = 0; i < relations.count; ++i) {
        const GuildRelation& rel = relations.entries[i];
        if (rel.relation != pass) continue;
        const char* break_label =
            (pass == 0) ? "Rompre l'alliance…" : "Retirer l'hostilité…";
        // Mémorise la cible et demande la confirmation (ouverture différée du modal :
        // ici la pile d'ID est celle de la ligne / du menu contextuel).
        auto ask_break = [&] {
          guild_rel_del_id_   = rel.guild_id;
          guild_rel_del_kind_ = pass;
          std::strncpy(guild_rel_del_name_, rel.name, sizeof(guild_rel_del_name_) - 1);
          guild_rel_del_name_[sizeof(guild_rel_del_name_) - 1] = '\0';
          guild_rel_del_ask_ = true;
        };
        ImGui::PushID(i);
        const float cross_w = 20.0f;
        const float row_w = std::max(60.0f, ImGui::GetContentRegionAvail().x -
                                                (can_break ? cross_w + 6.0f : 0.0f));
        ImGui::Selectable(rel.name, false, 0, ImVec2(row_w, 0.0f));
        if (can_break) {
          if (ImGui::BeginPopupContextItem("cs_guild_rel_ctx")) {
            ImGui::TextColored(kGray, "%s", rel.name);
            ImGui::Separator();
            if (ImGui::MenuItem(break_label)) {
              ask_break();
              ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
          }
          ImGui::SameLine();
          if (ro::RoSmallButton("x", cross_w, 0.0f)) ask_break();
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", break_label);
        }
        ImGui::PopID();
        ++shown;
      }
      if (shown == 0) ImGui::TextColored(kGray, "   aucune");
      ImGui::Spacing();
    }
    ImGui::EndChild();
  }

  // ── Barre d'actions ───────────────────────────────────────────────────────
  // (« Actualiser » vit en haut à droite de l'en-tête, pas ici.)
  // Invitation : soumise au droit « inviter » du poste, comme côté serveur.
  if (can_invite) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kBlack, "Inviter :");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    ro::InputTextCp949("##cs_guild_invite", guild_invite_buf_, sizeof(guild_invite_buf_));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Nom exact du personnage à inviter (il doit être connecté).");
    ImGui::SameLine();
    if (ro::RoButton("Inviter") && guild_invite_buf_[0]) {
      SendGuildInvite(guild_invite_buf_);
      guild_status_ = std::string("Invitation envoyée à ") + guild_invite_buf_;
      guild_invite_buf_[0] = '\0';
    }
    ImGui::SameLine();
  }
  // Libellé différent du titre du modal : bouton et popup partagent sinon le même ID.
  if (ro::RoButton("Quitter…")) {
    guild_reason_buf_[0] = '\0';
    ImGui::OpenPopup("Quitter la guilde");
  }
  // Dissolution : réservée au maître, comme la commande (gmaster_flag côté serveur).
  if (is_master) {
    ImGui::SameLine();
    if (ro::RoButton("Dissoudre…")) {
      guild_break_confirm_[0] = '\0';
      ImGui::OpenPopup("Dissoudre la guilde");
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Supprime définitivement la guilde (@breakguild).");
  }
  if (!guild_status_.empty()) ImGui::TextColored(kGray, "%s", guild_status_.c_str());

  // ── Confirmations ─────────────────────────────────────────────────────────
  // Demande d'expulsion venue du menu contextuel : on ouvre ICI, au niveau de
  // l'onglet, pour que l'ID matche celui du modal.
  if (guild_expel_ask_) {
    guild_expel_ask_ = false;
    ImGui::OpenPopup("Expulser de la guilde");
  }
  if (guild_rel_del_ask_) {
    guild_rel_del_ask_ = false;
    ImGui::OpenPopup("Rompre la relation");
  }
  // Le dossier est relu à CHAQUE ouverture : on y dépose justement un fichier juste
  // avant de venir le choisir.
  if (guild_emblem_ask_) {
    guild_emblem_ask_ = false;
    ScanEmblemFolder();
    guild_emblem_sel_ = -1;
    guild_emblem_error_.clear();
    ImGui::OpenPopup("Changer l'emblème");
  }
  DrawGuildEmblemModal(gi.guildId, is_master);

  if (ro::BeginRoPopupModal("Rompre la relation")) {
    if (guild_rel_del_kind_ == 0)
      ImGui::Text("Rompre l'alliance avec %s ?", guild_rel_del_name_);
    else
      ImGui::Text("Retirer %s de la liste des ennemis ?", guild_rel_del_name_);
    ImGui::TextColored(kGray,
                       "Refusé par le serveur pendant une guerre de guildes\n"
                       "et sur les cartes verrouillées.");
    ImGui::Spacing();
    if (ro::RoButton(guild_rel_del_kind_ == 0 ? "Rompre" : "Retirer", 110.0f, 0.0f)) {
      SendGuildDeleteRelation(guild_rel_del_id_, guild_rel_del_kind_);
      guild_status_ = std::string(guild_rel_del_name_) + " : demande envoyée.";
      guild_last_req_ = 0;  // rafraîchit la liste des relations
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton("Annuler", 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  if (ro::BeginRoPopupModal("Quitter la guilde")) {
    ImGui::TextUnformatted("Quitter définitivement la guilde ?");
    ImGui::TextColored(kGray, "Il faudra une nouvelle invitation pour y revenir.");
    ImGui::Spacing();
    ImGui::TextColored(kGray, "Motif (facultatif) :");
    ImGui::SetNextItemWidth(240.0f);
    ro::InputTextCp949("##cs_guild_leave_reason", guild_reason_buf_,
                       sizeof(guild_reason_buf_));
    ImGui::Spacing();
    if (ro::RoButton("Quitter", 110.0f, 0.0f)) {
      SendGuildLeaveOrExpel(kOpGuildLeave, gi.guildId,
                            static_cast<uint32_t>(
                                Bourgeon::Instance().client().session().aid()),
                            static_cast<uint32_t>(ReadInt(kOwnCharId)), guild_reason_buf_);
      guild_status_ = "Départ envoyé.";
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton("Annuler", 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  // ⚠ @breakguild n'a NI argument NI confirmation : il appelle guild_break() sur-le-champ.
  // Le serveur ne posera donc aucune question — le garde-fou du nom retapé est le seul
  // qui existe, à l'image de ce que demande la fenêtre native pour dissoudre.
  if (ro::BeginRoPopupModal("Dissoudre la guilde")) {
    ImGui::TextColored(kRed, "Dissoudre « %s » ?", gi.name);
    ImGui::TextColored(kGray, "Irréversible : la guilde, ses postes, son storage et ses\n"
                              "compétences disparaissent.");
    ImGui::Spacing();
    // Conditions RÉELLES de guild_break() : les dire AVANT évite un clic qui échoue,
    // d'autant que deux des trois refus sont peu bavards côté client.
    ImGui::TextColored(kBlack, "Le serveur refusera si :");
    ImGui::BulletText("tu n'es pas le maître de guilde ;");
    ImGui::BulletText("il reste un autre membre — il faut être SEUL ;");
    ImGui::BulletText("la carte interdit les modifications de guilde\n(mapflag guildlock) ;");
    ImGui::BulletText("une instance de guilde est en cours.");
    ImGui::TextColored(kGray, "L'instance fait échouer la dissolution SANS aucun message.");
    ImGui::Spacing();
    ImGui::TextColored(kGray, "Retape le nom de la guilde pour confirmer :");
    ImGui::SetNextItemWidth(240.0f);
    // Indice STATIQUE, pas gi.name : le nom vient du client en CP949, et l'indice est
    // rendu en UTF-8. La comparaison, elle, se fait bien CP949 contre CP949.
    ro::InputTextCp949WithHint("##cs_guild_break", "Nom exact de la guilde",
                               guild_break_confirm_, sizeof(guild_break_confirm_));
    ImGui::Spacing();
    const bool name_matches = gi.name[0] && std::strcmp(guild_break_confirm_, gi.name) == 0;
    ImGui::BeginDisabled(!name_matches);
    if (ro::RoButton("Dissoudre", 110.0f, 0.0f)) {
      SendAtCommand(kCmdBreakGuild);
      guild_status_ = "Dissolution demandée.";
      guild_break_confirm_[0] = '\0';
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ro::RoButton("Annuler", 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  if (ro::BeginRoPopupModal("Expulser de la guilde")) {
    ImGui::Text("Expulser %s de la guilde ?", guild_expel_name_);
    ImGui::Spacing();
    ImGui::TextColored(kGray, "Motif (facultatif) :");
    ImGui::SetNextItemWidth(240.0f);
    ro::InputTextCp949("##cs_guild_expel_reason", guild_reason_buf_,
                       sizeof(guild_reason_buf_));
    ImGui::Spacing();
    if (ro::RoButton("Expulser", 110.0f, 0.0f)) {
      SendGuildLeaveOrExpel(kOpGuildExpel, gi.guildId, guild_expel_aid_, guild_expel_cid_,
                            guild_reason_buf_);
      guild_status_ = std::string(guild_expel_name_) + " : expulsion envoyée.";
      guild_last_req_ = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton("Annuler", 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }
}

// Sous-onglet « Expulsions » : les exclusions mémorisées par le serveur (ZC 0x0b7c).
// Purement informatif : rien ne permet de réintégrer quelqu'un depuis le client.
void CharacterSheet::DrawGuildBansTab() {
  const ImVec4 kGray(0.35f, 0.35f, 0.42f, 1.0f);
  if (!guild_bans_known_) {
    ImGui::TextColored(kGray, "Liste non encore reçue — clic sur « Actualiser ».");
    return;
  }
  if (guild_bans_.empty()) {
    ImGui::TextColored(kGray, "Aucune expulsion enregistrée.");
    return;
  }

  const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                      ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
  if (!ImGui::BeginTable("cs_guild_bans_tbl", 2, table_flags)) return;
  ImGui::TableSetupColumn("Personnage", ImGuiTableColumnFlags_WidthFixed, 130.0f);
  ImGui::TableSetupColumn("Motif", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableHeadersRow();
  for (const GuildBanRow& ban : guild_bans_) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(ban.name[0] ? ban.name : "?");
    ImGui::TableNextColumn();
    if (ban.reason[0]) ImGui::TextUnformatted(ban.reason);
    else               ImGui::TextColored(kGray, "(aucun motif)");
  }
  ImGui::EndTable();
}

// Charge l'arbre UNE fois par session (un échec est mémorisé : sans le fichier dans le
// GRF, l'onglet retombe simplement sur ce que le serveur envoie).
void CharacterSheet::EnsureGuildSkillTree() {
  if (guild_skill_tree_state_ != 0) return;
  char dump[4096];
  char err[512];
  const int read = GuildTreeDumpSEH(dump, sizeof(dump), err, sizeof(err));
  if (read == kTreeNoLua) return;  // pas encore prêt : on retentera à la frame suivante
  if (read != kTreeOk) {
    guild_skill_tree_state_ = -1;
    switch (read) {
      case kTreeNoFile:
        LogDiag("[Guilde] {} : introuvable dans le VFS, ou erreur Lua à l'exécution "
                "(SKID absent de cet état ?) — arbre désactivé.", kGuildTreeLuaFile);
        break;
      case kTreeNoDumper:
        LogDiag("[Guilde] fichier chargé mais GdDump() absent — version obsolète du .lub ?");
        break;
      case kTreeCallFailed:
        LogDiag("[Guilde] GdDump() a échoué : {}", err);
        break;
      default:
        LogDiag("[Guilde] GdDump() a rendu une table vide.");
        break;
    }
    return;
  }

  char head[160] = {};  // début du dump BRUT, avant découpage : sert au diagnostic
  std::strncpy(head, dump, sizeof(head) - 1);

  guild_skill_tree_.clear();
  int link_count = 0;
  for (char* cursor = dump; *cursor;) {
    char* const entry_start = cursor;
    // Découpage en champs ',' jusqu'au ';'. Le NOMBRE de champs distingue les deux
    // formats du dumper : 4 = « id,maxLv,nom,prérequis », 3 = ancien « id,maxLv,prérequis ».
    // Le .lub voyage par patch, il peut être en retard d'une version sur la DLL : lu de
    // travers, l'ancien format fait passer les prérequis pour un nom, et TOUS les liens
    // disparaissent en silence.
    char* field[4] = {cursor, nullptr, nullptr, nullptr};
    int fields = 1;
    while (*cursor && *cursor != ';') {
      if (*cursor == ',' && fields < 4) {
        *cursor = '\0';
        field[fields++] = cursor + 1;
      }
      ++cursor;
    }
    if (*cursor == ';') *cursor++ = '\0';

    GuildSkillTreeNode node;
    node.id = static_cast<uint16_t>(std::strtoul(field[0], nullptr, 10));
    if (fields >= 2) node.max_level = static_cast<int>(std::strtol(field[1], nullptr, 10));
    const char* name = (fields >= 4) ? field[2] : "";
    const char* reqs = (fields >= 4) ? field[3] : (fields == 3 ? field[2] : "");
    std::strncpy(node.name, name, sizeof(node.name) - 1);
    for (const char* r = reqs; *r;) {  // prérequis : « id:lvl » séparés par |
      char* end = nullptr;
      GuildSkillReq req;
      req.id = static_cast<uint16_t>(std::strtoul(r, &end, 10));
      if (end == r) break;  // rien de lisible : champ vide ou corrompu
      r = end;
      if (*r == ':') ++r;
      req.level = static_cast<int>(std::strtol(r, &end, 10));
      r = end;
      if (req.id != 0) { node.need.push_back(req); ++link_count; }
      if (*r == '|') ++r;
    }
    if (node.id != 0) guild_skill_tree_.push_back(node);
    if (cursor == entry_start) break;  // aucune entrée consommée : chaîne inexploitable
  }

  // Profondeur = 1 + celle du prérequis le plus profond. Résolu par passes successives
  // (l'ordre de pairs() côté Lua est arbitraire) ; le nombre de nœuds borne les passes,
  // ce qui protège aussi d'un cycle si la DB serveur en contenait un.
  for (size_t pass = 0; pass < guild_skill_tree_.size(); ++pass) {
    bool changed = false;
    for (GuildSkillTreeNode& node : guild_skill_tree_) {
      int depth = 0;
      for (const GuildSkillReq& req : node.need)
        for (const GuildSkillTreeNode& other : guild_skill_tree_)
          if (other.id == req.id && other.depth + 1 > depth) depth = other.depth + 1;
      if (depth != node.depth) { node.depth = depth; changed = true; }
    }
    if (!changed) break;
  }
  std::stable_sort(guild_skill_tree_.begin(), guild_skill_tree_.end(),
                   [](const GuildSkillTreeNode& a, const GuildSkillTreeNode& b) {
                     if (a.depth != b.depth) return a.depth < b.depth;
                     return a.id < b.id;
                   });
  guild_skill_tree_state_ = 1;
  // Un arbre SANS aucun lien n'existe pas dans la DB : c'est forcément le dump qui n'a
  // pas été compris. Montrer son début plutôt que d'afficher une liste plate en silence.
  if (link_count == 0)
    LogDiag("[Guilde] aucun prérequis lu — .lub d'une autre version ? dump : « {} »", head);
}

// Sous-onglet « Compétences » : ce que le serveur a envoyé en ZC 0x0162. Le bouton
// « + » suit `upgradable` (déjà restreint au maître côté serveur) ET les points
// restants : inutile d'y remettre un test de maître, le serveur a tranché.
void CharacterSheet::DrawGuildSkillsTab() {
  const ImVec4 kGray(0.35f, 0.35f, 0.42f, 1.0f);
  EnsureGuildSkillTree();
  if (!guild_skills_known_) {
    ImGui::TextColored(kGray, "Compétences non encore reçues — clic sur « Actualiser ».");
    return;
  }

  ImGui::Text("Points de compétence : %d", guild_skill_points_);
  // Sans l'arbre on ne peut montrer que ce que le serveur envoie ; avec, les
  // verrouillées apparaissent aussi, donc la liste n'est jamais vide.
  if (guild_skill_tree_state_ != 1 && guild_skills_.empty()) {
    ImGui::TextColored(kGray, "Aucune compétence disponible (prérequis non remplis).");
    return;
  }

  // Lignes affichées : l'ARBRE quand il est disponible (il contient aussi les
  // compétences verrouillées, que le serveur n'envoie pas), sinon la seule liste
  // serveur. `live` = état réel reçu, nul pour une compétence encore verrouillée.
  struct SkillRowView {
    uint16_t id;
    int depth;
    int max_level;                // 0 = inconnu (pas d'arbre)
    const GuildSkillRow*      live;
    const GuildSkillTreeNode* node;
  };
  std::vector<SkillRowView> rows;
  auto find_live = [this](uint16_t id) -> const GuildSkillRow* {
    for (const GuildSkillRow& sk : guild_skills_)
      if (sk.id == id) return &sk;
    return nullptr;
  };
  if (guild_skill_tree_state_ == 1) {
    for (const GuildSkillTreeNode& node : guild_skill_tree_)
      rows.push_back({node.id, node.depth, node.max_level, find_live(node.id), &node});
  } else {
    for (const GuildSkillRow& sk : guild_skills_)
      rows.push_back({sk.id, 0, 0, &sk, nullptr});
  }

  const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                      ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
  // Pas de colonne « Requiert » : les prérequis sont déjà dans le tooltip et dessinés
  // en liens ; une 3e redite volait la largeur au nom, qui se retrouvait tronqué.
  if (!ImGui::BeginTable("cs_guild_skills_tbl", 5, table_flags)) return;
  ImGui::TableSetupColumn("Compétence", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 54.0f);
  // Assez large pour « Passif » : cette colonne porte le coût OU la nature de la
  // compétence, exactement comme le natif qui écrit « Passive » à la place du SP.
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 56.0f);
  // Colonne « lancer » élargie : elle porte aussi le décompte de cooldown (« 4:12 »).
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 46.0f);  // lancer
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30.0f);  // monter
  ImGui::TableHeadersRow();

  auto tree_node = [this](uint16_t id) -> const GuildSkillTreeNode* {
    for (const GuildSkillTreeNode& node : guild_skill_tree_)
      if (node.id == id) return &node;
    return nullptr;
  };
  // Nom : d'abord le Lua du client (localisé), qui ne connaît PAS les compétences de
  // guilde ; puis le libellé du fichier d'arbre — seule source pour une verrouillée,
  // dont aucun paquet n'arrive ; enfin le nom technique du paquet.
  auto skill_label = [&](uint16_t id, const char* packet_name) -> const char* {
    const char* lua_name = reinterpret_cast<GetSkillNameLua_t>(kGetSkillNameLua)(id);
    if (lua_name && *lua_name && std::strcmp(lua_name, "Unknown-Skill") != 0) return lua_name;
    const GuildSkillTreeNode* node = tree_node(id);
    if (node && node->name[0]) return node->name;
    return (packet_name && packet_name[0]) ? packet_name : "?";
  };
  // « Battle Orders Niv 1, Guild Extension Niv 2 » — même texte en colonne et en
  // tooltip, pour ne pas décrire deux fois la même chose de deux façons.
  auto requirements_text = [&](const GuildSkillTreeNode* node) -> std::string {
    std::string text;
    if (!node) return text;
    for (const GuildSkillReq& req : node->need) {
      if (!text.empty()) text += ", ";
      text += skill_label(req.id, nullptr);
      text += " Niv ";
      text += std::to_string(req.level);
    }
    return text;
  };

  const float icon = ImGui::GetTextLineHeight();
  constexpr float kTreeStep   = 20.0f;  // décalage par niveau de profondeur
  constexpr float kTreeGutter = 16.0f;  // marge de gauche commune : la place des liens
  // Liens de dépendance, tracés dans la gouttière d'indentation : le prérequis est
  // toujours dessiné AVANT sa suite (tri par profondeur), donc son ancre est connue.
  const uint16_t focus = guild_skill_hover_;
  uint16_t hovered_now = 0;
  auto depends_on = [&](uint16_t id, uint16_t req_id) {
    const GuildSkillTreeNode* node = tree_node(id);
    if (!node) return false;
    for (const GuildSkillReq& req : node->need)
      if (req.id == req_id) return true;
    return false;
  };
  auto linked_to_focus = [&](uint16_t id) {
    return focus != 0 && id != focus && (depends_on(id, focus) || depends_on(focus, id));
  };
  // Ancre = bord gauche de l'icône, milieu vertical. x < 0 : ligne pas encore dessinée.
  std::vector<ImVec2> anchors(rows.size(), ImVec2(-1.0f, -1.0f));
  auto row_index = [&](uint16_t id) -> int {
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].id == id) return static_cast<int>(i);
    return -1;
  };
  for (size_t i = 0; i < rows.size(); ++i) {
    const SkillRowView& row = rows[i];
    const GuildSkillRow* live = row.live;
    const bool locked = live == nullptr;  // prérequis non remplis : jamais envoyée
    const int  level  = live ? live->level : 0;
    ImGui::PushID(static_cast<int>(row.id));
    ImGui::TableNextRow();
    // Survol : la ligne pointée et ses voisines directes (prérequis / suites) ressortent.
    if (row.id == focus)
      ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(120, 95, 35, 90));
    else if (linked_to_focus(row.id))
      ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(70, 75, 120, 80));

    // ── Colonne 1 : profondeur -> indentation, c'est l'arbre lui-même ──
    ImGui::TableNextColumn();
    // La marge de gauche N'EST PAS décorative : c'est la place des liens. Sans elle la
    // verticale tombait à 1 px du bord de la colonne, confondue avec la bordure du
    // tableau — des traits bien présents, mais invisibles.
    const float row_indent = kTreeGutter + row.depth * kTreeStep;
    ImGui::Indent(row_indent);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    anchors[i] = ImVec2(p.x, p.y + icon * 0.5f);
    // Coude vers chaque prérequis : dire le lien plutôt que le laisser deviner de
    // l'indentation, et allumer la branche quand un de ses deux bouts est survolé.
    if (row.node) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      for (const GuildSkillReq& req : row.node->need) {
        const int src = row_index(req.id);
        if (src < 0 || anchors[src].x < 0.0f) continue;
        const bool hot = focus != 0 && (focus == row.id || focus == req.id);
        const ImU32 col = hot ? IM_COL32(255, 205, 105, 255) : IM_COL32(150, 155, 190, 100);
        const float thickness = hot ? 2.5f : 0.1f;
        // Descente À GAUCHE de l'icône source, pas sous son centre : entre les deux
        // lignes il y a des voisines de même profondeur, dont la verticale traverserait
        // l'icône. Décalée d'un demi-pas, elle passe dans leur gouttière.
        const float x = anchors[src].x - kTreeStep * 0.5f;
        dl->AddLine(ImVec2(anchors[src].x - 2.0f, anchors[src].y), ImVec2(x, anchors[src].y),
                    col, thickness);  // amorce, pour que le lien parte visiblement du parent
        dl->AddLine(ImVec2(x, anchors[src].y), ImVec2(x, anchors[i].y), col, thickness);
        dl->AddLine(ImVec2(x, anchors[i].y), ImVec2(anchors[i].x - 2.0f, anchors[i].y),
                    col, thickness);
        dl->AddCircleFilled(ImVec2(anchors[i].x - 3.0f, anchors[i].y), hot ? 3.0f : 2.0f, col);
      }
    }
    const ro::IconTex ic = ResolveSkillIcon(row.id);
    if (ic.tex) {
      // Verrouillée : icône assombrie, comme le natif grise ce qui n'est pas accessible.
      ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(ic.tex), p,
                                           ImVec2(p.x + icon, p.y + icon), ImVec2(0, 0),
                                           ImVec2(1, 1),
                                           locked ? IM_COL32(110, 110, 110, 160)
                                                  : IM_COL32_WHITE);
    }
    ImGui::Dummy(ImVec2(icon, icon));
    ImGui::SameLine();
    const char* label = skill_label(row.id, live ? live->name : nullptr);
    if (locked) ImGui::PushStyleColor(ImGuiCol_Text, kGray);
    // Selectable (widget À ID) plutôt qu'un simple texte : c'est ce qui donne l'ActiveId
    // nécessaire au drag, et la zone cliquable pour le clic droit.
    ImGui::Selectable(label, false, ImGuiSelectableFlags_AllowDoubleClick);
    if (locked) ImGui::PopStyleColor();
    // `inf` = skill_get_inf : 0 = PASSIF, donc rien à mettre dans une barre de raccourcis.
    const bool active_skill = live && live->inf != 0 && level > 0;
    // Bit 0x04 = INF_SELF_SKILL : lançable sur soi, sans curseur de ciblage. Toutes les
    // compétences de guilde le sont ; on ne propose « lancer » que dans ce cas, faute de
    // quoi il faudrait entrer dans le mode ciblage natif (cmd 0x48), une autre histoire.
    const bool can_use = active_skill && (live->inf & 0x04) != 0;
    // Double-clic = lancer, comme un objet dans l'inventaire.
    if (can_use && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
      SendUseSkill(row.id, level);
    if (active_skill && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
      const int payload[2] = {static_cast<int>(row.id), level};
      ImGui::SetDragDropPayload("BGN_SKILL", payload, sizeof(payload));
      if (ic.tex) {
        ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(24.0f, 24.0f));
        ImGui::SameLine();
      }
      ImGui::TextUnformatted(label);
      ImGui::EndDragDropSource();
    }
    // Clic droit = description native (fenêtre 0x2e), au curseur, comme dans la barre.
    // Vaut aussi pour une compétence verrouillée : savoir ce qu'elle fait aide à décider.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      const ImVec2 mp = ImGui::GetIO().MousePos;
      OpenSkillDesc(row.id, static_cast<int>(mp.x), static_cast<int>(mp.y));
    }
    if (ImGui::IsItemHovered()) {
      hovered_now = row.id;  // consommé à la frame suivante (liens + surlignage)
      std::string tip = label;
      if (live && live->name[0]) { tip += "  ("; tip += live->name; tip += ")"; }
      if (row.max_level > 0) tip += "\nNiveau " + std::to_string(level) + " / " +
                                    std::to_string(row.max_level);
      if (live && live->range > 0) tip += "\nPortée : " + std::to_string(live->range);
      // Les prérequis sont ce qui manque justement à une verrouillée : les dire ICI,
      // là où le joueur regarde quand il se demande pourquoi elle est grisée.
      const std::string reqs = requirements_text(row.node);
      if (!reqs.empty()) tip += "\nRequiert : " + reqs;
      if (live) tip += live->inf == 0 ? "\nPassive (toujours active)" : "\nActive";
      tip += locked      ? "\n\nVerrouillée : prérequis non remplis."
           : can_use     ? "\n\nDouble-clic : lancer — clic droit : description — glisser vers une barre"
           : active_skill ? "\n\nClic droit : description — glisser vers une barre"
                          : "\n\nClic droit : description";
      ImGui::SetTooltip("%s", tip.c_str());
    }
    ImGui::Unindent(row_indent);

    // ── Niveau : « 3/10 » dès que le max est connu (il vient du fichier, pas du paquet) ──
    ImGui::TableNextColumn();
    if (row.max_level > 0) {
      if (level > 0) ImGui::Text("%d/%d", level, row.max_level);
      else           ImGui::TextColored(kGray, "-/%d", row.max_level);
    } else if (level > 0) {
      ImGui::Text("%d", level);
    } else {
      ImGui::TextColored(kGray, "-");
    }

    // ── SP, ou « Passif » ──────────────────────────────────────────────────────
    // `inf` (skill_get_inf, envoyé par le serveur) vaut 0 pour une passive. C'était
    // jusqu'ici invisible : rien ne distinguait une passive d'une active, il fallait
    // ouvrir la description ou tenter le drag pour le découvrir.
    ImGui::TableNextColumn();
    if (live && live->inf == 0)    ImGui::TextColored(kGray, "Passif");
    else if (live && live->sp > 0) ImGui::Text("%d", live->sp);
    else                           ImGui::TextColored(kGray, "-");

    // ── Lancer : l'équivalent du bouton « use » de la fenêtre native ───────────
    // Sous cooldown, le bouton porte le décompte plutôt qu'un « > » mort : c'est là que
    // le joueur clique, donc là qu'il faut lui dire pourquoi ça ne part pas.
    ImGui::TableNextColumn();
    const unsigned long cd_ms = SkillCooldownRemaining(row.id);
    char use_label[16] = ">";
    if (cd_ms > 0) {
      const unsigned long secs = (cd_ms + 999) / 1000;  // arrondi au-dessus : jamais « 0s »
      if (secs >= 60) std::snprintf(use_label, sizeof(use_label), "%lu:%02lu", secs / 60, secs % 60);
      else            std::snprintf(use_label, sizeof(use_label), "%lus", secs);
    }
    ImGui::BeginDisabled(!can_use || cd_ms > 0);
    if (ro::RoSmallButton(use_label, 40.0f, 0.0f)) SendUseSkill(row.id, level);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
      if (cd_ms > 0)
        ImGui::SetTooltip("Encore %lu s.\nLancer une compétence de guilde les bloque toutes les quatre.",
                          (cd_ms + 999) / 1000);
      else if (can_use)                 ImGui::SetTooltip("Lancer (ou double-clic sur le nom).");
      else if (live && live->inf == 0)  ImGui::SetTooltip("Compétence passive : rien à lancer.");
      else if (locked || level == 0)    ImGui::SetTooltip("Non apprise.");
      else                              ImGui::SetTooltip("Se lance sur une cible : à glisser dans une barre.");
    }

    ImGui::TableNextColumn();
    const bool can_up = live && live->upgradable && guild_skill_points_ > 0;
    ImGui::BeginDisabled(!can_up);
    if (ro::RoSmallButton("+", 24.0f, 0.0f)) {
      SendSkillUp(row.id);
      // Le serveur renvoie un 0x0162 complet après guild_skillupack : on le laisse
      // corriger niveau et points plutôt que de les avancer à l'aveugle ici.
      guild_last_req_ = 0;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
      if (can_up)                        ImGui::SetTooltip("Monter d'un niveau.");
      else if (locked)                   ImGui::SetTooltip("Prérequis non remplis.");
      else if (guild_skill_points_ <= 0) ImGui::SetTooltip("Aucun point de compétence disponible.");
      else                               ImGui::SetTooltip("Niveau maximum, ou réservé au maître de guilde.");
    }
    ImGui::PopID();
  }
  guild_skill_hover_ = hovered_now;
  ImGui::EndTable();
}

// Sous-onglet « Postes » : les 20 postes de la guilde (nom, droits, part d'exp).
// Le maître peut tout éditer d'un coup et envoyer le lot (CZ 0x0161) ; les autres
// voient la grille en lecture seule. Tant qu'une saisie est en cours, la copie
// éditée n'est plus resynchronisée sur les paquets reçus (sinon la frappe serait
// écrasée par le prochain rafraîchissement).
void CharacterSheet::DrawGuildPositionsTab(bool can_edit) {
  const ImVec4 kGray(0.35f, 0.35f, 0.42f, 1.0f);

  if (!guild_positions_editing_) {
    for (int i = 0; i < kGuildPositionSlots; ++i) guild_positions_edit_[i] = guild_positions_[i];
  }

  bool any_info = false;
  for (int i = 0; i < kGuildPositionSlots; ++i)
    if (guild_positions_[i].has_name || guild_positions_[i].has_info) { any_info = true; break; }
  if (!any_info) {
    ImGui::TextColored(kGray, "Postes non encore reçus — clic sur « Actualiser ».");
    return;
  }

  if (can_edit)
    ImGui::TextColored(kGray,
                       "Nom, droits et part d'exp de chaque poste. Le serveur plafonne "
                       "la part d'exp à %d %%.", kGuildPayRateMax);
  else
    ImGui::TextColored(kGray, "Seul le maître de guilde peut modifier les postes.");

  const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
  // Place réservée aux boutons du bas (uniquement pour le maître).
  const float rows_h = std::max(80.0f, ImGui::GetContentRegionAvail().y -
                                           (can_edit ? ImGui::GetFrameHeightWithSpacing() + 4.0f
                                                     : 0.0f));
  if (ImGui::BeginTable("cs_guild_positions_tbl", 6, table_flags, ImVec2(0.0f, rows_h))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 22.0f);
    ImGui::TableSetupColumn("Nom", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Inviter", ImGuiTableColumnFlags_WidthFixed, 54.0f);
    ImGui::TableSetupColumn("Expulser", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Storage", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Part exp", ImGuiTableColumnFlags_WidthFixed, 96.0f);
    ImGui::TableHeadersRow();

    for (int id = 0; id < kGuildPositionSlots; ++id) {
      GuildPositionRow& row = guild_positions_edit_[id];
      // Une ligne n'est éditable que si ses DROITS sont connus : sans le paquet
      // 0x0160, envoyer la ligne écraserait le masque de droits par 0.
      const bool row_editable = can_edit && guild_positions_[id].has_info;
      ImGui::PushID(id);
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextColored(kBlack, "%d", id);

      ImGui::TableSetColumnIndex(1);
      if (row_editable) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ro::InputTextCp949("##nom", row.name, sizeof(row.name)))
          guild_positions_editing_ = true;
      } else {
        ImGui::TextColored(kBlack, "%s", row.name[0] ? row.name : "—");
      }

      // Droits : un bit chacun (0x001 inviter, 0x010 expulser, 0x100 Storage).
      const int perm_bits[3] = {kGuildPermInvite, kGuildPermExpel, kGuildPermStorage};
      for (int p = 0; p < 3; ++p) {
        ImGui::TableSetColumnIndex(2 + p);
        bool on = (row.mode & perm_bits[p]) != 0;
        ImGui::PushID(p);
        if (row_editable) {
          if (ro::RoCheckbox("##droit", &on)) {
            row.mode = on ? (row.mode | perm_bits[p]) : (row.mode & ~perm_bits[p]);
            guild_positions_editing_ = true;
          }
        } else {
          ImGui::TextColored(kBlack, "%s", on ? "oui" : "-");
        }
        ImGui::PopID();
      }

      ImGui::TableSetColumnIndex(5);
      if (row_editable) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ro::RoSliderInt("##part", &row.pay_rate, 0, kGuildPayRateMax, "%d %%"))
          guild_positions_editing_ = true;
      } else {
        ImGui::TextColored(kBlack, "%d %%", row.pay_rate);
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  if (!can_edit) return;

  // Envoi : uniquement les lignes qui DIFFÈRENT de l'état serveur (le serveur
  // rediffuse un ZC 0x0174 par poste modifié, qui remettra la table à jour).
  const bool has_changes = [&] {
    for (int i = 0; i < kGuildPositionSlots; ++i) {
      const GuildPositionRow& live = guild_positions_[i];
      const GuildPositionRow& edited = guild_positions_edit_[i];
      if (!live.has_info) continue;  // ligne non éditable (droits inconnus)
      if (live.mode != edited.mode || live.pay_rate != edited.pay_rate ||
          std::strncmp(live.name, edited.name, sizeof(live.name)) != 0)
        return true;
    }
    return false;
  }();

  ImGui::BeginDisabled(!has_changes);
  if (ro::RoButton("Enregistrer les postes")) {
    GuildPositionWire rows[kGuildPositionSlots];
    int count = 0;
    for (int i = 0; i < kGuildPositionSlots; ++i) {
      const GuildPositionRow& live = guild_positions_[i];
      const GuildPositionRow& edited = guild_positions_edit_[i];
      if (!live.has_info) continue;  // droits inconnus : ne jamais réécrire cette ligne
      if (live.mode == edited.mode && live.pay_rate == edited.pay_rate &&
          std::strncmp(live.name, edited.name, sizeof(live.name)) == 0)
        continue;
      GuildPositionWire& wire = rows[count++];
      std::memset(&wire, 0, sizeof(wire));
      wire.id       = i;
      wire.mode     = edited.mode & (kGuildPermInvite | kGuildPermExpel | kGuildPermStorage);
      wire.ranking  = i;
      wire.pay_rate = std::clamp(edited.pay_rate, 0, kGuildPayRateMax);
      std::strncpy(wire.name, edited.name, sizeof(wire.name) - 1);
    }
    SendGuildPositions(rows, count);
    guild_positions_editing_ = false;
    char done[64];
    std::snprintf(done, sizeof(done), "%d poste(s) envoyé(s).", count);
    guild_status_ = done;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!guild_positions_editing_);
  if (ro::RoButton("Annuler les modifications")) guild_positions_editing_ = false;
  ImGui::EndDisabled();
}

// Choix d'un nouvel emblème parmi les .bmp de <jeu>\emblem\ — le dossier où la
// fenêtre native va lire les siens, pour que les deux voient les mêmes fichiers.
// L'envoi est réservé au maître (le serveur exige gmaster_flag) et refusé pendant
// une guerre de guildes selon la configuration du serveur.
void CharacterSheet::DrawGuildEmblemModal(int guildId, bool is_master) {
  if (!ro::BeginRoPopupModal("Changer l'emblème")) return;
  const ImVec4 kGray(0.35f, 0.35f, 0.42f, 1.0f);
  const ImVec4 kRed(0.60f, 0.12f, 0.12f, 1.0f);
  const std::string dir = paths::InGameDir("emblem\\");

  ImGui::TextColored(kGray, "Format envoyé : %dx%d, magenta pur (255, 0, 255) = transparent.",
                     kEmblemSide, kEmblemSide);
  // Le serveur (clif_parse_GuildChangeEmblem) sort SANS RIEN DIRE quand l'expéditeur
  // n'a pas le drapeau gmaster : autant l'annoncer avant de laisser dessiner.
  if (!is_master)
    ImGui::TextColored(kRed, "Tu n'es pas maître de guilde : le serveur ignorera l'envoi.");
  ImGui::Spacing();
  if (!ImGui::BeginTabBar("cs_emblem_tabs")) {
    ro::EndRoPopupModal();
    return;
  }
  // « Reprendre au dessin » a chargé un fichier dans le canvas : l'onglet doit suivre,
  // sinon le clic n'a aucun effet visible (ImGui ne bascule pas tout seul).
  const ImGuiTabItemFlags paint_flags =
      guild_emblem_goto_paint_ ? ImGuiTabItemFlags_SetSelected : 0;
  guild_emblem_goto_paint_ = false;
  if (ImGui::BeginTabItem("Dessiner", nullptr, paint_flags)) {
    DrawGuildEmblemPaintTab(guildId);
    ImGui::EndTabItem();
  }
  if (!ImGui::BeginTabItem("Choisir un fichier")) {
    ImGui::EndTabBar();
    ImGui::Separator();
    if (ro::RoButton("Fermer", 90.0f, 0.0f)) ImGui::CloseCurrentPopup();
    if (!guild_emblem_diag_.empty()) ImGui::TextColored(kGray, "%s", guild_emblem_diag_.c_str());
    ro::EndRoPopupModal();
    return;
  }

  // Chemin affiché avec des « / » : la police du client est une police CP949, où
  // l'octet 0x5C (l'antislash) se dessine comme le symbole won coréen ₩.
  std::string shown_dir = dir;
  std::replace(shown_dir.begin(), shown_dir.end(), '\\', '/');
  ImGui::TextColored(kGray, "Fichiers .bmp de %s", shown_dir.c_str());
  ImGui::TextColored(kGray, "Attendu : 24 bits ou 256 couleurs, non compressé.");
  ImGui::Spacing();

  if (g_emblem_files.empty()) {
    ImGui::TextColored(kRed, "Aucun .bmp dans ce dossier.");
  } else {
    const float row_h = std::max(ImGui::GetTextLineHeight() + 8.0f, 32.0f);
    // Hauteur EXACTE du contenu (lignes + interlignes + marges + bordure) : la calculer
    // « au jugé » faisait apparaître une barre de défilement dès la première ligne, ce
    // qui décalait tout le contenu vers le bas.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float row_item_h = row_h - 4.0f;  // hauteur du Selectable d'une ligne
    const int   visible_rows =
        std::min(6, static_cast<int>(g_emblem_files.size()));
    const float list_h = visible_rows * row_item_h +
                         (visible_rows - 1) * style.ItemSpacing.y +
                         style.WindowPadding.y * 2.0f + style.ChildBorderSize * 2.0f;
    // Largeur 0 = tout l'espace restant : le modal est déjà dimensionné par l'éditeur.
    ImGui::BeginChild("cs_emblem_list", ImVec2(0.0f, list_h), true);
    const float thumb = row_h - 10.0f;        // vignette carrée
    const float text_x = thumb + 12.0f;       // le texte commence APRÈS la vignette
    for (int i = 0; i < static_cast<int>(g_emblem_files.size()); ++i) {
      const EmblemCandidate& cand = g_emblem_files[i];
      ImGui::PushID(i);
      const ImVec2 row_pos = ImGui::GetCursorScreenPos();
      // Ligne = un Selectable VIDE occupant toute la largeur (donc cliquable partout) ;
      // vignette et nom sont peints par-dessus, chacun à sa place. Indenter le libellé
      // avec des espaces ne marchait pas : la largeur d'un espace n'a rien à voir avec
      // celle de la vignette, qui finissait par recouvrir le nom.
      if (ImGui::Selectable("##ligne", guild_emblem_sel_ == i, 0, ImVec2(0.0f, row_h - 4.0f))) {
        guild_emblem_sel_ = i;
        guild_emblem_error_ = cand.usable ? std::string() : cand.why;
      }
      if (!cand.usable && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", cand.why.c_str());
      ImDrawList* rdl = ImGui::GetWindowDrawList();
      if (cand.preview.tex) {
        const ImVec2 p0(row_pos.x + 4.0f, row_pos.y + (row_h - 4.0f - thumb) * 0.5f);
        rdl->AddImage(reinterpret_cast<ImTextureID>(cand.preview.tex), p0,
                      ImVec2(p0.x + thumb, p0.y + thumb));
        rdl->AddRect(p0, ImVec2(p0.x + thumb, p0.y + thumb), IM_COL32(90, 90, 110, 160));
      }
      char label[160];
      std::snprintf(label, sizeof(label), "%s%s", cand.name.c_str(),
                    cand.usable ? "" : "   (refusé)");
      const float text_y = row_pos.y + (row_h - 4.0f - ImGui::GetTextLineHeight()) * 0.5f;
      rdl->AddText(ImVec2(row_pos.x + text_x, text_y),
                   ImGui::GetColorU32(cand.usable ? ImGuiCol_Text : ImGuiCol_TextDisabled), label);
      ImGui::PopID();
    }
    ImGui::EndChild();
  }

  // Détail du fichier retenu : ce que le serveur va réellement recevoir.
  const EmblemCandidate* chosen =
      (guild_emblem_sel_ >= 0 && guild_emblem_sel_ < static_cast<int>(g_emblem_files.size()))
          ? &g_emblem_files[guild_emblem_sel_]
          : nullptr;
  if (chosen && chosen->usable) {
    ImGui::TextColored(kGray, "%zu octets, prêt à être envoyé.", chosen->bmp.size());
    if (chosen->transparency > kEmblemTransparencyWarn)
      ImGui::TextColored(kRed, "Transparence ~%d %% : au-delà de %d %% le serveur refuse.",
                         chosen->transparency, kEmblemTransparencyWarn);
  } else if (!guild_emblem_error_.empty()) {
    ImGui::TextColored(kRed, "%s", guild_emblem_error_.c_str());
  } else {
    ImGui::TextColored(kGray, "Choisis un fichier dans la liste.");
  }

  ImGui::Spacing();
  const bool can_send = chosen && chosen->usable && guildId > 0;
  ImGui::BeginDisabled(!can_send);
  // Envoi par le chemin NATIF (service web) : le seul qui fonctionne sur ce serveur.
  if (ro::RoButton("Envoyer", 110.0f, 0.0f)) {
    const bool started = RequestEmblemUploadSEH(guildId, chosen->name.c_str());
    guild_emblem_diag_ = started
                             ? "Envoi au service web lancé (" + chosen->name + ")."
                             : "Refusé : un envoi est déjà en cours, ou service indisponible.";
    if (started) guild_status_ = "Emblème envoyé : " + chosen->name;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ro::RoButton("Relire le dossier", 140.0f, 0.0f)) {
    ScanEmblemFolder();
    guild_emblem_sel_ = -1;
    guild_emblem_error_.clear();
  }
  ImGui::SameLine();
  // Passerelle vers l'éditeur : retoucher un emblème existant plutôt que de le refaire.
  ImGui::BeginDisabled(!chosen || chosen->bmp.empty());
  if (ro::RoButton("Reprendre au dessin", 160.0f, 0.0f)) {
    if (EmblemCanvasLoadBmp(chosen->bmp)) {
      guild_emblem_goto_paint_ = true;  // bascule sur l'éditeur, sinon rien ne se voit
      guild_emblem_error_.clear();
      guild_status_ = "Dessin repris de " + chosen->name;
    } else {
      guild_emblem_error_ = "Ce fichier n'est pas reprenable (dimensions ou profondeur).";
    }
  }
  ImGui::EndDisabled();
  ImGui::EndTabItem();
  ImGui::EndTabBar();

  ImGui::Separator();
  if (ro::RoButton("Fermer", 90.0f, 0.0f)) ImGui::CloseCurrentPopup();
  ImGui::SameLine();
  ImGui::SameLine();
  ImGui::TextColored(kGray,
                     "L'envoi passe par le service web du serveur, comme la fenêtre native.\n"
                     "L'emblème se met à jour dès que le serveur a publié la nouvelle version.");
  // Compte rendu du dernier envoi (aussi écrit dans bourgeon.log et la console).
  if (!guild_emblem_diag_.empty()) ImGui::TextColored(kGray, "%s", guild_emblem_diag_.c_str());
  ro::EndRoPopupModal();
}

// Canvas 24x24 : clic gauche = couleur courante, clic droit = gomme. Le rendu est un
// simple ImDrawList (576 rectangles) plutôt qu'une texture — pas de cache à invalider
// au reset du device, et le damier de fond montre où l'emblème sera transparent.
// Le statut de maître n'est PAS un paramètre : l'avertissement « tu n'es pas maître »
// est affiché une fois pour tout le modal, et le serveur reste seul juge de l'envoi.
void CharacterSheet::DrawGuildEmblemPaintTab(int guildId) {
  const ImVec4 kGray(0.35f, 0.35f, 0.42f, 1.0f);
  const ImVec4 kRed(0.60f, 0.12f, 0.12f, 1.0f);
  if (!g_emblem_canvas.started) EmblemCanvasClear();

  // Palette de départ : les teintes franches passent mieux sur 24x24 qu'un dégradé.
  // Le magenta n'y figure PAS : il vaut « transparent » et a son sélecteur dédié.
  static const uint32_t kSwatches[] = {
      0x000000, 0x404040, 0x808080, 0xC0C0C0, 0xFFFFFF, 0x7F0000, 0xD92B2B, 0xFF7F27,
      0xFFC90E, 0xFFF200, 0x0F5A0F, 0x22B14C, 0x7FE817, 0x0B2C6B, 0x2B6FD9, 0x59C7F0,
      0x4B0082, 0x9B30FF, 0xD94BC0, 0x8B5A2B, 0xC69C6D, 0x5A3A1A,
  };
  const float cell = 14.0f;  // 24 x 14 = 336 px de côté
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 canvas_size(cell * kEmblemSide, cell * kEmblemSide);
  ImGui::InvisibleButton("##cs_emblem_canvas", canvas_size,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (int y = 0; y < kEmblemSide; ++y) {
    for (int x = 0; x < kEmblemSide; ++x) {
      const uint32_t c = g_emblem_canvas.pixel[y * kEmblemSide + x];
      const ImVec2 p0(origin.x + x * cell, origin.y + y * cell);
      const ImVec2 p1(p0.x + cell, p0.y + cell);
      if (c == kEmblemClear) {
        // Damier = « rien ici » ; c'est exactement ce que le jeu rendra transparent.
        const bool dark = ((x + y) & 1) != 0;
        dl->AddRectFilled(p0, p1, dark ? IM_COL32(150, 150, 156, 255) : IM_COL32(190, 190, 196, 255));
      } else {
        dl->AddRectFilled(p0, p1, IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255));
      }
    }
  }
  // Grille tous les 4 pixels + cadre : repères pour centrer un motif à la main.
  for (int i = 0; i <= kEmblemSide; i += 4) {
    const float p = i * cell;
    dl->AddLine(ImVec2(origin.x + p, origin.y), ImVec2(origin.x + p, origin.y + canvas_size.y),
                IM_COL32(0, 0, 0, 40));
    dl->AddLine(ImVec2(origin.x, origin.y + p), ImVec2(origin.x + canvas_size.x, origin.y + p),
                IM_COL32(0, 0, 0, 40));
  }
  dl->AddRect(origin, ImVec2(origin.x + canvas_size.x, origin.y + canvas_size.y),
              IM_COL32(60, 60, 70, 220));

  // Tracé. Un « coup » = de l'appui au relâchement : une seule entrée d'annulation.
  // On peint tant que le bouton reste enfoncé APRÈS un appui dans le canvas (IsItemActive),
  // et non tant que la souris le survole : sortir d'un pixel du cadre en pleine ligne
  // ne doit pas couper le trait.
  const bool left = ImGui::IsMouseDown(ImGuiMouseButton_Left);
  const bool right = ImGui::IsMouseDown(ImGuiMouseButton_Right);
  const bool drawing = ImGui::IsItemActive() || (hovered && (left || right));
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const int cx = std::clamp(static_cast<int>((mouse.x - origin.x) / cell), 0, kEmblemSide - 1);
  const int cy = std::clamp(static_cast<int>((mouse.y - origin.y) / cell), 0, kEmblemSide - 1);
  const bool shape_tool = g_emblem_canvas.tool >= kToolLine;
  const uint32_t ink = right ? kEmblemClear : CurrentInk();

  if (!shape_tool) {
    if (drawing && (left || right)) {
      if (!g_emblem_canvas.stroke_open) {
        EmblemCanvasPushUndo();
        g_emblem_canvas.stroke_open = true;
        g_emblem_canvas.last_x = cx;
        g_emblem_canvas.last_y = cy;
      }
      if (g_emblem_canvas.tool == kToolFill && !right) {
        EmblemCanvasFill(cx, cy, ink);
      } else {
        const uint32_t stroke_ink =
            (g_emblem_canvas.tool == kToolEraser || right) ? kEmblemClear : ink;
        EmblemCanvasStroke(g_emblem_canvas.last_x, g_emblem_canvas.last_y, cx, cy, stroke_ink);
      }
      g_emblem_canvas.last_x = cx;
      g_emblem_canvas.last_y = cy;
    }
    if (!left && !right) g_emblem_canvas.stroke_open = false;
  } else {
    // Formes : on mémorise le point de départ, on montre l'aperçu, on peint au
    // relâchement. Tant que le bouton est tenu, le dessin sous-jacent est intact.
    if (!g_emblem_canvas.shape_active && drawing && (left || right)) {
      g_emblem_canvas.shape_active = true;
      g_emblem_canvas.shape_erase = right;
      g_emblem_canvas.shape_x0 = cx;
      g_emblem_canvas.shape_y0 = cy;
    }
    if (g_emblem_canvas.shape_active) {
      static bool mask[kEmblemPixels];
      EmblemShapeMask(g_emblem_canvas.tool, g_emblem_canvas.filled, g_emblem_canvas.shape_x0,
                      g_emblem_canvas.shape_y0, cx, cy, mask);
      if (!left && !right) {  // relâché : la forme devient définitive
        EmblemCanvasPushUndo();
        EmblemApplyMask(mask, g_emblem_canvas.shape_erase ? kEmblemClear
                                                          : CurrentInk());
        g_emblem_canvas.shape_active = false;
      } else {
        // Aperçu : couleur visée en semi-transparent + liseré, pour rester lisible
        // sur un fond de la même teinte. Une forme « transparente » s'y montre en
        // damier, comme le sera le résultat — surtout pas en magenta, que l'éditeur
        // n'affiche jamais tel quel.
        const uint32_t c = g_emblem_canvas.shape_erase ? kEmblemClear
                                                       : CurrentInk();
        for (int i = 0; i < kEmblemPixels; ++i) {
          if (!mask[i]) continue;
          const int x = i % kEmblemSide, y = i / kEmblemSide;
          const ImVec2 p0(origin.x + x * cell, origin.y + y * cell);
          const ImVec2 p1(p0.x + cell, p0.y + cell);
          if (c == kEmblemClear) {
            const bool dark = ((x + y) & 1) != 0;
            dl->AddRectFilled(p0, p1, dark ? IM_COL32(150, 150, 156, 200)
                                           : IM_COL32(190, 190, 196, 200));
            dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 120));
            continue;
          }
          dl->AddRectFilled(p0, p1,
                            IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 170));
          dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 120));
        }
      }
    }
  }

  // Aperçu 1:1 (ce que verront les autres joueurs) à droite du canvas.
  const ImVec2 preview(origin.x + canvas_size.x + 16.0f, origin.y);
  for (int y = 0; y < kEmblemSide; ++y) {
    for (int x = 0; x < kEmblemSide; ++x) {
      const uint32_t c = g_emblem_canvas.pixel[y * kEmblemSide + x];
      if (c == kEmblemClear) continue;
      dl->AddRectFilled(ImVec2(preview.x + x, preview.y + y),
                        ImVec2(preview.x + x + 1, preview.y + y + 1),
                        IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255));
    }
  }
  dl->AddRect(ImVec2(preview.x - 1, preview.y - 1),
              ImVec2(preview.x + kEmblemSide + 1, preview.y + kEmblemSide + 1),
              IM_COL32(60, 60, 70, 220));

  // ── Outils ────────────────────────────────────────────────────────────────
  ImGui::Spacing();
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(kGray, "Outil :");
  ImGui::SameLine();
  const char* tool_names[6] = {"Crayon", "Gomme", "Remplir", "Ligne", "Rectangle", "Ellipse"};
  const char* tool_hints[6] = {
      "Peint à main levée.",
      "Efface (rend transparent).",
      "Remplit la zone de même couleur.",
      "Tire une ligne d'un point à l'autre.",
      "Tire un rectangle entre deux coins.",
      "Tire une ellipse inscrite dans le rectangle tiré.",
  };
  for (int t = 0; t < 6; ++t) {
    ImGui::PushID(t);
    // L'outil courant reste enfoncé, libellé en gras (ro::RoToggleButton).
    if (ro::RoToggleButton(tool_names[t], g_emblem_canvas.tool == t, 80.0f, 0.0f))
      g_emblem_canvas.tool = t;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tool_hints[t]);
    ImGui::PopID();
    if (t != 2 && t != 5) ImGui::SameLine();
  }
  ImGui::SameLine();
  ImGui::TextColored(kGray, "(clic droit = efface)");

  ImGui::SetNextItemWidth(140.0f);
  ro::RoSliderInt("Épaisseur", &g_emblem_canvas.brush, 1, 3, "%d px");
  ImGui::SameLine();
  ro::RoCheckbox("Symétrie", &g_emblem_canvas.mirror);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Chaque trait est répété en miroir gauche/droite.");
  ImGui::SameLine();
  // Ne concerne que le rectangle et l'ellipse : grisé ailleurs plutôt que caché, pour
  // que la barre d'outils ne saute pas d'un outil à l'autre.
  ImGui::BeginDisabled(g_emblem_canvas.tool != kToolRect && g_emblem_canvas.tool != kToolEllipse);
  ro::RoCheckbox("Forme pleine", &g_emblem_canvas.filled);
  ImGui::EndDisabled();

  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(kGray, "Couleur :");
  ImGui::SameLine();
  if (ImGui::ColorEdit3("##cs_emblem_color", g_emblem_canvas.color, ImGuiColorEditFlags_NoInputs)) {
    g_emblem_canvas.color_clear = false;  // choisir une teinte, c'est quitter le « vide »
    if (g_emblem_canvas.tool == kToolEraser) g_emblem_canvas.tool = kToolPencil;
  }
  ImGui::SameLine();
  // « Transparence » = le magenta pur, et c'est une COULEUR comme une autre : on doit
  // pouvoir en remplir une zone ou en tracer une forme, ce que la gomme (à main levée)
  // ne permet pas. Elle laisse donc l'outil courant tel quel.
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(kGray, "Transparence :");
  ImGui::SameLine();
  const ImVec4 magenta(1.0f, 0.0f, 1.0f, 1.0f);
  const float swatch_h = ImGui::GetFrameHeight();
  if (ImGui::ColorButton("##cs_emblem_transparent", magenta,
                         ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                         ImVec2(swatch_h * 1.6f, swatch_h)))
    g_emblem_canvas.color_clear = true;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Magenta pur (255, 0, 255) : ces pixels sont transparents en jeu.\n"
                      "S'utilise avec n'importe quel outil (remplissage, formes…).");
  if (g_emblem_canvas.color_clear) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.10f, 0.35f, 0.70f, 1.0f), "couleur active");
  }
  ImGui::TextColored(kGray, "Palette :");
  ImGui::SameLine();
  // Nuancier : une case pose la couleur courante (et sort du « vide » comme de la gomme).
  const float swatch = ImGui::GetFrameHeight() - 2.0f;
  const int swatch_count = static_cast<int>(sizeof(kSwatches) / sizeof(kSwatches[0]));
  for (int i = 0; i < swatch_count; ++i) {
    const uint32_t c = kSwatches[i];
    ImGui::PushID(1000 + i);
    if (i % 11 != 0) ImGui::SameLine();
    const ImVec2 sp = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##sw", ImVec2(swatch, swatch))) {
      if (g_emblem_canvas.tool == kToolEraser) g_emblem_canvas.tool = kToolPencil;
      g_emblem_canvas.color_clear = false;
      g_emblem_canvas.color[0] = ((c >> 16) & 0xFF) / 255.0f;
      g_emblem_canvas.color[1] = ((c >> 8) & 0xFF) / 255.0f;
      g_emblem_canvas.color[2] = (c & 0xFF) / 255.0f;
    }
    ImDrawList* sdl = ImGui::GetWindowDrawList();
    const ImVec2 sp1(sp.x + swatch, sp.y + swatch);
    sdl->AddRectFilled(sp, sp1, IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255));
    sdl->AddRect(sp, sp1, IM_COL32(40, 40, 48, 220));
    ImGui::PopID();
  }

  ImGui::Spacing();
  ImGui::BeginDisabled(g_emblem_canvas.undo.empty());
  if (ro::RoButton("Annuler", 90.0f, 0.0f)) {
    std::copy(g_emblem_canvas.undo.back().begin(), g_emblem_canvas.undo.back().end(),
              g_emblem_canvas.pixel);
    g_emblem_canvas.undo.pop_back();
    ++g_emblem_canvas.revision;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ro::RoButton("Tout effacer", 110.0f, 0.0f)) {
    EmblemCanvasPushUndo();
    EmblemCanvasClear();
  }

  // ── Point de départ : une icône d'item ────────────────────────────────────
  // Les icônes d'inventaire du client font 24x24, la taille exacte d'un emblème :
  // elles font d'excellentes bases (potion, carte, arme…) à retoucher ensuite.
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(kGray, "Partir d'une icône d'item :");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.0f);
  ImGui::InputInt("##cs_emblem_itemid", &guild_emblem_item_id_, 0, 0);
  if (guild_emblem_item_id_ < 0) guild_emblem_item_id_ = 0;
  // Aperçu à côté du champ : on voit ce qu'on va importer avant de perdre le dessin.
  ro::IconTex preview_icon =
      guild_emblem_item_id_ > 0
          ? ro::ItemIcon(static_cast<uint32_t>(guild_emblem_item_id_))
          : ro::IconTex{};
  ImGui::SameLine();
  const float icon_box = ImGui::GetFrameHeight();
  const ImVec2 icon_pos = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(icon_box, icon_box));
  if (preview_icon.tex) {
    ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(preview_icon.tex), icon_pos,
                                         ImVec2(icon_pos.x + icon_box, icon_pos.y + icon_box));
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!preview_icon.tex);
  if (ro::RoButton("Importer", 110.0f, 0.0f)) {
    if (EmblemCanvasLoadItemIcon(static_cast<uint32_t>(guild_emblem_item_id_))) {
      guild_emblem_diag_.clear();
    } else {
      guild_emblem_diag_ = "Icône introuvable pour cet item.";
    }
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Remplace le dessin par l'icône de l'item (annulable).");
  if (guild_emblem_item_id_ > 0 && !preview_icon.tex) {
    ImGui::SameLine();
    ImGui::TextColored(kRed, "aucune icône");
  }
  ImGui::SameLine();
  if (ro::RoToggleButton("Inventaire", guild_emblem_gallery_, 110.0f, 0.0f))
    guild_emblem_gallery_ = !guild_emblem_gallery_;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Choisir l'icône dans son sac, sans connaître les numéros.");

  if (guild_emblem_gallery_) {
    // Un balayage par seconde suffit : le sac bouge (loot, vente), mais pas à la
    // fréquence d'affichage — et ReadInventoryLite parcourt toute la liste native.
    static std::vector<uint32_t> s_gallery_ids;
    static DWORD s_gallery_scan = 0;
    const DWORD now = GetTickCount();
    if (s_gallery_scan == 0 || now - s_gallery_scan > 1000) {
      s_gallery_scan = now;
      InvItemLite inv[512];
      const int inv_count = ReadInventoryLite(inv, 512);
      s_gallery_ids.clear();
      for (int i = 0; i < inv_count; ++i) {
        const uint32_t id = inv[i].nameid;
        // Une pile de 50 potions, ou la même arme en double, c'est UNE icône.
        if (id && std::find(s_gallery_ids.begin(), s_gallery_ids.end(), id) == s_gallery_ids.end())
          s_gallery_ids.push_back(id);
      }
      // Trié par id : l'ordre de la liste native change au fil des ramassages, et une
      // grille qui se réorganise sous le curseur est impossible à parcourir.
      std::sort(s_gallery_ids.begin(), s_gallery_ids.end());
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float cell = 32.0f;  // icônes 24x24 agrandies : cliquables sans loupe
    const float inner_w = ImGui::GetContentRegionAvail().x - style.WindowPadding.x * 2.0f -
                          style.ChildBorderSize * 2.0f;
    int per_row = static_cast<int>((inner_w + style.ItemSpacing.x) / (cell + style.ItemSpacing.x));
    if (per_row < 1) per_row = 1;
    const int rows_visible = 3;
    const float child_h = rows_visible * cell + (rows_visible - 1) * style.ItemSpacing.y +
                          style.WindowPadding.y * 2.0f + style.ChildBorderSize * 2.0f;
    ImGui::BeginChild("##cs_emblem_gallery", ImVec2(0.0f, child_h), true);
    if (s_gallery_ids.empty()) {
      ImGui::TextColored(kGray, "Aucun item dans le sac.");
    } else {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      for (size_t i = 0; i < s_gallery_ids.size(); ++i) {
        const uint32_t id = s_gallery_ids[i];
        if ((i % static_cast<size_t>(per_row)) != 0) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(id));
        const ImVec2 cell_pos = ImGui::GetCursorScreenPos();
        const ImVec2 cell_end(cell_pos.x + cell, cell_pos.y + cell);
        const bool clicked = ImGui::InvisibleButton("##ic", ImVec2(cell, cell));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) dl->AddRectFilled(cell_pos, cell_end, IM_COL32(255, 255, 255, 40));
        const ro::IconTex ic = ro::ItemIcon(id);
        if (ic.tex)
          dl->AddImage(reinterpret_cast<ImTextureID>(ic.tex), cell_pos, cell_end);
        else
          dl->AddRect(cell_pos, cell_end, IM_COL32(120, 120, 120, 120));
        if (hovered) {
          const char* nm = ItemName(id);
          ImGui::SetTooltip("%s (%u)", (nm && nm[0]) ? nm : "?", id);
        }
        if (clicked) {
          guild_emblem_item_id_ = static_cast<int>(id);
          if (EmblemCanvasLoadItemIcon(id)) guild_emblem_diag_.clear();
          else guild_emblem_diag_ = "Icône introuvable pour cet item.";
        }
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
  }

  // ── Envoi / enregistrement ────────────────────────────────────────────────
  // Le BMP n'est refabriqué que quand le dessin a bougé : le modal est redessiné à
  // chaque frame, et rien ne justifie de le reconstruire 60 fois par seconde.
  static int s_built_revision = -1;
  static std::vector<uint8_t> s_bmp;
  static bool s_has_ink = false;
  static int s_transparency = 0;
  if (s_built_revision != g_emblem_canvas.revision) {
    s_built_revision = g_emblem_canvas.revision;
    s_bmp = BuildEmblemBmp();
    s_transparency = EmblemTransparencyPercent(s_bmp);
    s_has_ink = false;
    for (int i = 0; i < kEmblemPixels && !s_has_ink; ++i)
      s_has_ink = g_emblem_canvas.pixel[i] != kEmblemClear;
  }
  const std::vector<uint8_t>& bmp = s_bmp;
  const bool has_ink = s_has_ink;

  ImGui::TextColored(kGray, "%zu octets (BMP %dx%d, 24 bits).", bmp.size(), kEmblemSide,
                     kEmblemSide);
  // Le serveur refuse un emblème trop vide (inter_config.emblem_transparency_limit) :
  // mieux vaut le dire pendant qu'on dessine qu'après un envoi rejeté.
  if (s_transparency > kEmblemTransparencyWarn)
    ImGui::TextColored(kRed, "Transparence ~%d %% : au-delà de %d %% le serveur refuse — "
                             "remplis davantage le fond.",
                       s_transparency, kEmblemTransparencyWarn);
  const bool can_send = has_ink && guildId > 0;
  ImGui::BeginDisabled(!can_send);
  // Le dessin part par le chemin NATIF : on l'écrit d'abord dans <jeu>\emblem\, car
  // l'upload du jeu prend un CHEMIN de fichier (curl lit le fichier lui-même).
  if (ro::RoButton("Envoyer ce dessin", 160.0f, 0.0f)) {
    const std::string file_name = std::string(g_emblem_canvas.save_name[0]
                                                  ? g_emblem_canvas.save_name
                                                  : "mon_embleme") + ".bmp";
    if (!WriteEmblemFile(file_name.c_str(), bmp)) {
      guild_emblem_diag_ = "Écriture impossible : emblem/" + file_name;
    } else {
      g_emblem_preview_cache.erase(file_name);  // la vignette du fichier a changé
      const bool started = RequestEmblemUploadSEH(guildId, file_name.c_str());
      guild_emblem_diag_ = started
                               ? "Envoi au service web lancé (" + file_name + ")."
                               : "Refusé : un envoi est déjà en cours, ou service indisponible.";
      if (started) guild_status_ = "Emblème dessiné envoyé (" + file_name + ").";
    }
  }
  ImGui::EndDisabled();
  // Bouton grisé : dire POURQUOI plutôt que de laisser deviner.
  if (!can_send) {
    ImGui::SameLine();
    const char* why = has_ink ? "guilde inconnue du client" : "dessin vide";
    ImGui::TextColored(kRed, "Envoi impossible : %s.", why);
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(150.0f);
  ro::InputTextCp949("##cs_emblem_savename", g_emblem_canvas.save_name,
                     sizeof(g_emblem_canvas.save_name));
  ImGui::SameLine();
  // Garder le .bmp sous la main : il rejoint le dossier que lit aussi la fenêtre native.
  if (ro::RoButton("Enregistrer dans emblem", 190.0f, 0.0f) && g_emblem_canvas.save_name[0]) {
    const std::string path =
        paths::InGameDir("emblem\\") + g_emblem_canvas.save_name + std::string(".bmp");
    CreateDirectoryA(paths::InGameDir("emblem").c_str(), nullptr);
    FILE* fp = nullptr;
    const std::string file_name = std::string(g_emblem_canvas.save_name) + ".bmp";
    if (fopen_s(&fp, path.c_str(), "wb") == 0 && fp) {
      std::fwrite(bmp.data(), 1, bmp.size(), fp);
      std::fclose(fp);
      // Nom seul, sans le chemin : la police CP949 du client dessine l'antislash en ₩.
      guild_status_ = "Emblème enregistré : emblem/" + file_name;
      // Réécrire sous un nom déjà vu : la vignette en cache montrerait l'ancien dessin.
      g_emblem_preview_cache.erase(file_name);
      ScanEmblemFolder();
    } else {
      guild_emblem_error_ = "Écriture impossible : emblem/" + file_name;
    }
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

// Colonne COMPAGNONS (à gauche de l'arme) : cart/peco/faucon, chacun affiché SEULEMENT
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
// Cart : clic droit = menu {Ouvrir, Changer la déco (si MC_CHANGECART), Retirer}.
void CharacterSheet::DrawCompanionCase(int kind, float x, float y, float sz) {
  bool active = false;
  const char* label = "";
  const char* name = "";
  int skillId = 0;  // skill dont on affiche l'icône (id envoyé par le serveur)
  switch (kind) {
    case kCompCart:   active = companion_.cart_active > 0; label = "Cart";   name = "Cart";        skillId = companion_.pushcart_id; break;
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
  // Icône du skill (cart/peco/faucon) ; repli sur le libellé texte si absente.
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

  // Menu contextuel (cart uniquement).
  if (kind == kCompCart) {
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("cart_ctx");
    if (ImGui::BeginPopup("cart_ctx")) {
      if (ImGui::MenuItem("Ouvrir le cart", nullptr, false, active)) OpenCartWindow();
      const bool canDeco = companion_.changecart_lv > 0 && active;
      if (ImGui::MenuItem("Changer la décoration", nullptr, false, canDeco)) {
        int next = companion_.cart_active + 1;
        if (next > companion_.cart_deco_max || next < 1) next = 1;
        SendCompanionPkt(kCompCart, kCompDeco, next);
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Retirer le cart", nullptr, false, active))
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

// Ouvre la fenêtre d'inventaire du cart. MakeWindow crée/affiche par id (RE 2026-07-12 :
// id 0x28 = UIMerchantItemWnd, vtable 0x0103d538 ; même appel que la fenêtre de description).
// Le case a un gate de contexte UI (IsWindowAllowedInContext) qui passe en jeu normal ;
// OnCreate ne dépend pas de l'état cart (au pire fenêtre vide), le serveur pousse le contenu.
void CharacterSheet::OpenCartWindow() {
  constexpr int kCartWndId = 0x28;  // UIMerchantItemWnd (fenêtre inventaire cart)
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
    // Repli sur le dernier libellé de poste connu : la liste des membres remet ce
    // champ à vide tant que les noms de postes ne sont pas revenus (cf. DrawGuildTab).
    const char* my_position = GuildPositionLabel(gi.position_id, gi.pos);
    if (my_position) std::snprintf(gline, sizeof(gline), "%s [%s]", gi.name, my_position);
    else             std::snprintf(gline, sizeof(gline), "%s", gi.name);
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
    // Compagnons (cart/peco/faucon) à GAUCHE de l'arme, empilés vers le bas. Seules les
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

// Ouvre (ou déplie) la feuille sur l'onglet Grimoire. Appelée depuis le hook
// MakeWindow quand le joueur demande le grimoire natif : on ne fait que POSER la
// demande, la sélection d'onglet se joue au rendu suivant.
void CharacterSheet::OpenSkillsTab() {
  show_ = true;
  tab_ = 5;
  tab_request_ = 5;
}

// Masque la fenêtre native du grimoire (UINewSkillListWnd, id 0x25) dès sa création,
// avant son premier rendu — sinon une frame native passe à l'écran. Le flag de
// visibilité vit à window+0x28 (UIWnd_SetVisible 0x009030c0 l'y écrit) ; on le pose
// nous-mêmes plutôt que d'appeler la vtable, comme le font l'inventaire et l'entrepôt.
// La fenêtre reste ENREGISTRÉE : le natif la « rouvrira » (donc la re-masquera) et
// c'est notre onglet qui s'affiche à la place.
void CharacterSheet::HideSkillWndAtCreation(void* win) {
  if (!win || !imgui_enabled_) return;
  __try {
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + kWndVisibleFlag) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  OpenSkillsTab();
}

// L'icône « Skill » et Alt+S ne font qu'ouvrir/fermer la fenêtre native 0x25 : on
// suit son existence (le gestionnaire vide son emplacement à la fermeture) pour que
// ces deux entrées pilotent NOTRE onglet. Le masquage est reposé à chaque tick, comme
// pour l'inventaire : un relayout natif peut remettre le drapeau de visibilité à 1.
void CharacterSheet::OnTick() {
  if (!imgui_enabled_) { skill_wnd_was_open_ = false; return; }
  void* wnd = nullptr;
  __try {
    wnd = *reinterpret_cast<void**>(kSkillWndSlot);
    if (wnd)
      *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(wnd) + kWndVisibleFlag) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { wnd = nullptr; }
  const bool open = wnd != nullptr;
  if (open && !skill_wnd_was_open_) {
    OpenSkillsTab();
  } else if (!open && skill_wnd_was_open_ && tab_ == 5) {
    show_ = false;  // le joueur vient de refermer le grimoire : la feuille suit
  }
  skill_wnd_was_open_ = open;
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
  // Les onglets Guilde (table des membres) et Grimoire (grille de 7 colonnes) ont
  // besoin de toute la largeur : on y interdit le repli étroit plutôt que de laisser
  // le contenu déborder.
  g_win_snap.force_wide = (tab_ == 4 || tab_ == 5);
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(g_win_snap.force_wide ? g_win_snap.wide : g_win_snap.narrow, 450.0f),
      ImVec2(g_win_snap.wide, 10000.0f), SnapCharSheetWidth);
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

  // Onglets Equipement / Costume / Presets / Titres / Guilde / Grimoire.
  // `tab_request_` (posé par OpenSkillsTab) force la sélection UNE frame : ImGui
  // choisit l'onglet au moment où il le dessine, un hook ne peut pas l'imposer.
  if (ImGui::BeginTabBar("cs_tabs")) {
    auto flag = [this](int idx) {
      return tab_request_ == idx ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
    if (ImGui::BeginTabItem("Équipement", nullptr, flag(0))) { tab_ = 0; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Costume",    nullptr, flag(1))) { tab_ = 1; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Presets",    nullptr, flag(2))) { tab_ = 2; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Titres",     nullptr, flag(3))) { tab_ = 3; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Guilde",     nullptr, flag(4))) { tab_ = 4; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Grimoire",   nullptr, flag(5))) { tab_ = 5; ImGui::EndTabItem(); }
    ImGui::EndTabBar();
  }
  tab_request_ = -1;
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
  } else if (tab_ == 4) {
    // Onglet Guilde : pleine largeur (infos + roster + relations).
    ImGui::BeginChild("cs_guild", ImVec2(0, 0), true);
    DrawGuildTab();
    ImGui::EndChild();
  } else if (tab_ == 5) {
    // Onglet Grimoire : pleine largeur (grille 7 colonnes ou liste détaillée).
    ImGui::BeginChild("cs_skills", ImVec2(0, 0), true);
    DrawSkillsTab();
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
