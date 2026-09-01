#include "ragnarok/lua.h"
#include "ragnarok/equip_slots.h"  // rag::equip : les pieces PORTEES
#include "ragnarok/item_db.h"
#include "ragnarok/globals.h"
#include "ragnarok/social.h"  // rag::social::JobName (cache + repli)
#include "features/windows/character_sheet.h"
#include "ragnarok/game_settings.h"  // IsOn : la bordure d'emblème
#include "ragnarok/player_skills.h"
#include "ui/game_texture.h"

// Icônes d'item : ro::ItemIcon (ui/icon_cache.h) — cache partagé. Les caches
// de skill et d'emblème restent locaux : chemins et clés différents.
#include "ui/icon_cache.h"
#include "ui/head_icon.h"  // miniature de tête des membres de guilde
#include "ragnarok/uiwnd.h"
#include "ragnarok/msgstring.h"  // msgstr::Utf8 : libellés EXACTS du client (onglet Homoncule)
#include "ragnarok/homunculus.h"  // état + compétences de l'homoncule (partagé avec skill_bar)
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
#include "features/windows/mvp_tracker_window.h"  // DrawMvpInviteMenuItem
#include "features/windows/palette_editor.h"  // bouton « Mes couleurs »
#include "features/systems/bourgeon_opcodes.h"  // bopcodes::kStatBonus (ZC 0x0F10)
#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB
#include "imgui.h"
#include "features/item_cell.h"              // itemcell::OpenDescById (description au clic droit)
#include "features/overlays/basic_info.h"    // RenderPlayerAvatar (avatar plein-corps)
#include "features/windows/inventory_viewer.h"  // LinkItemToChat / EquipDraggedItem (drag-drop, chat)
#include "features/windows/chat_window.h"       // OpenWhisperWindowByAid : « Chuchoter » sur un membre
#include "features/windows/rodex_window.h"      // ComposeTo : « Envoyer un courrier » sur un membre
#include "features/moonlight_ui/moonlight_ui.h"      // SaveSettings (persistance des presets)
#include "features/staff_gate.h"        // IsStaff : le volet staff du mannequin
#include "features/hotkey_util.h"       // capture/libellé/conflit d'un raccourci
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"  // mui::LastItemWheel (verrou molette anti-défilement)
#include "utils/tinf_inflate.h"  // inflate zlib pour les emblèmes de guilde (.ebm)
#include "utils/log_console.h"   // LogDiag : échecs de chargement de l'arbre de guilde
#include "utils/i18n.h"
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "ragnarok/stl_node.h"  // rag::listnode : le nœud du conteneur
#include "ragnarok/skill_info.h"  // rag::skillinfo
#include "features/craft_data.h"  // craftdata::kMaxRefine
#include "ui/ui_palette.h"  // ro::pal : la palette de l'UI

//  Constantes RE (client 20250716, base 0x400000 ; cf. project_character_sheet)
namespace {

// La disposition d'une pièce PORTÉE — base du tableau, pas de slot, offsets
// d'une entrée, données d'instance — vit dans `rag::equip` (ragnarok/equip_slots.h).
constexpr int kNormalSlots = 10;   // slots equip normaux 0..9 (cf. disposition doll)
// Le tableau costume a la meme forme que celui de l'equip, mais SEULES ces quatre
// positions y existent : tete haut, tete bas, tete milieu, cape. Les indices ne sont
// pas contigus — c'est la meme numerotation de slots que l'equipement normal.
constexpr int kCostumeSlots[4] = {8, 0, 9, 2};
constexpr int kMaxPresetsPerChar = 5;  // plafond de presets par personnage

//  Presets d'equipement : CID (clef par perso) + liste inventaire (session)

//  Globals stats (cf. project_status_tweaks_plugin)

//  Opcodes (raw, envoyes via Bourgeon::SendPacket comme le shop)
constexpr uint16_t kOpUnequip = 0x00AB;  // CZ_REQ_TAKEOFF_EQUIP {op, invIndex}
constexpr uint16_t kOpEquip   = 0x0998;  // CZ_REQ_WEAR_EQUIP_V5 {op, invIndex, position:4}

//  Munition : PAS un slot du tableau equip -> invIndex dans un global dédié (cf. RE 2026-07-12).
//  On lit l'item par cet invIndex dans la liste inventaire (in-place, donne la quantité).
constexpr int kOffEquipAmount = 0x10;   // quantité (item d'inventaire) ; == present pour un equip

//  Compagnons : CZ_BOURGEON_COMPANION (bopcodes::kCompanion 0x0F15) {kind, action, arg}.
//  Miroir des enums serveur e_bourgeon_companion_kind / _action.
enum { kCompCart = 0, kCompPeco = 1, kCompFalcon = 2 };
enum { kCompOff = 0, kCompOn = 1, kCompDeco = 2 };

// Toggles de config natifs (fenêtre équip case 0xd5) via le dispatcher CMode *(0x0121333c)->vf+0x18.
// RE live 2026-07-11 : cmd 0xFD = « Show Equip » (config 0), cmd 0x148 = « View Costumes » (config 5).
constexpr int       kCmdShowEquip    = 0xFD;   // config 0 : montrer l'équip aux autres
constexpr int       kCmdViewCostume  = 0x148;  // config 5 : voir les costumes
// cmd 0x71 = « lancer la compétence du slot » : c'est LUI qui lit l'INF et décide entre
// envoi immédiat (self) et passage en mode ciblage (cible / sol / support / piège).
// Struct d'info compétence remplie par le natif : __stdcall(out, skillId). Champs utiles
// +0x04 trouvée, +0x08 id, +0x0C INF, +0x10 niveau appris. À DÉTRUIRE (2 std::string).
constexpr int       kSkillEntryFound = 0x04;
constexpr int       kSkillEntryLevel = 0x10;
// g_Own_AccountId : notre AID, qui est aussi le GID de notre acteur (cible d'un self-cast).
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
    if (kPoses[i].anim == anim) return i18n::Tr(kPoses[i].label);
  return "Combat";  // repli
}
// Libellé COMPLET de la pose courante (anim + animé) : aperçu du combo.
const char* PoseLabelFull(int anim, bool animate) {
  for (int i = 0; i < kPoseCount; ++i)
    if (kPoses[i].anim == anim && kPoses[i].animate == animate) return i18n::Tr(kPoses[i].label);
  return PoseLabel(anim);
}
constexpr int kAnimCombat = 4;  // en combat, on limite à 4 directions cardinales

//  Icone d'item (item\<resname>.bmp)
// Icône de SKILL (case compagnon) : le .bmp est nommé par l'identifiant Lua du skill
// (ex. "MC_PUSHCART"), pas par l'id numérique. Lua_GetSkillIdName(id) -> idname, puis
// "유저인터페이스\item\<idname>.bmp" (source native, indép. de l'appris ; cf. skill_bar).

// La lecture gardée d'un int : celle de globals.h. Le `using` laisse les
// points d'appel de ce fichier tels quels.
using rag::ReadInt;
//  Lecture in-place d'un slot equipe (SEH, POD)
// ⚠ `viewId` et `location` sont typés à leur largeur SÉMANTIQUE, pas à celle du
// champ natif : un viewID tient sur 16 bits, `location` est un masque EQP_* et
// n'a pas de signe. C'est ce qu'attendent les consommateurs (itemcell::
// OpenDescById, l'aperçu porté), donc la conversion se fait UNE fois ici, à la
// frontière avec le natif, plutôt qu'à chaque site d'appel.
struct EquipItem {
  bool     present = false;
  uint32_t nameid = 0;
  int      invIndex = 0;
  int      refine = 0;
  uint16_t viewId = 0;
  uint32_t location = 0;
};
bool ReadEquipSlot(int slot, bool costume, EquipItem* out) {
  __try {
    const uintptr_t base =
        rag::kSessionAddr + (costume ? rag::equip::kOwnCostumeBase : rag::equip::kOwnEquipBase) + slot * rag::equip::kSlotStride;
    const uint8_t* e = reinterpret_cast<const uint8_t*>(base);
    const int invIndex = *reinterpret_cast<const int*>(e + rag::equip::kOffInvIndex);
    const int present  = *reinterpret_cast<const int*>(e + rag::equip::kOffPresent);
    if (invIndex == 0 || present != 1) return false;  // slot vide
    out->present  = true;
    out->invIndex = invIndex;
    out->location = *reinterpret_cast<const uint32_t*>(e + rag::equip::kOffLocation);
    out->refine   = *reinterpret_cast<const int*>(e + rag::equip::kOffRefine);
    // 16 bits SUFFISENT : le champ natif en fait 4, mais tout ce qui consomme un
    // viewID le tronque à 16 — autant le dire ici. Little-endian : mêmes bits.
    out->viewId   = *reinterpret_cast<const uint16_t*>(e + rag::equip::kOffView);
    const char* rn = rag::clientstr::Data(e + rag::equip::kOffResname);
    out->nameid = (rn && rn[0]) ? static_cast<uint32_t>(std::atoi(rn)) : 0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

//  Munition équipée : lue par son invIndex (global g_AmmoEquippedInvIndex), retrouvée
//  dans la liste inventaire (node next@+0, ItemSkillInfo@+8, mêmes offsets). La munition
//  reste un item d'inventaire (consommé au tir) -> présente dans la liste, quantité @+0x10.
struct AmmoItem {  // largeurs sémantiques, cf. EquipItem
  bool     present = false;
  uint32_t nameid = 0;
  int      invIndex = 0;
  int      amount = 0;
  uint16_t viewId = 0;
  uint32_t location = 0;
  // ItemSkillInfo VIVANT du nœud d'inventaire, pour l'aperçu de description au
  // survol (cartes/options d'instance). ⚠ Valable la frame courante seulement :
  // le nœud meurt quand la munition est consommée. Ne pas mémoriser.
  const void* info = nullptr;
};
bool ReadEquippedAmmo(AmmoItem* out) {
  __try {
    const int ammoIdx = *reinterpret_cast<const int*>(rag::kAmmoEquippedInvIndexAddr);
    if (ammoIdx == 0) return false;  // aucune munition équipée
    void* sentinel = *reinterpret_cast<void* const*>(rag::kInventoryListAddr);
    const int count = *reinterpret_cast<const int*>(rag::kInventoryCountAddr);
    if (!sentinel || count <= 0) return false;
    void* node = *reinterpret_cast<void* const*>(sentinel);  // sentinelle->next = 1er noeud
    for (int i = 0; i < count && node && node != sentinel; ++i) {
      const uint8_t* info = reinterpret_cast<const uint8_t*>(node) + 8;
      if (*reinterpret_cast<const int*>(info + rag::equip::kOffInvIndex) == ammoIdx) {
        out->present  = true;
        out->info     = info;
        out->invIndex = ammoIdx;
        out->amount   = *reinterpret_cast<const int*>(info + kOffEquipAmount);
        out->location = *reinterpret_cast<const uint32_t*>(info + rag::equip::kOffLocation);
        out->viewId   = *reinterpret_cast<const uint16_t*>(info + rag::equip::kOffView);
        const char* rn = rag::clientstr::Data(info + rag::equip::kOffResname);
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
//  Parcourt la std::list session (rag::kInventoryListAddr) et remplit out[] (POD). Renvoie le nombre lu.
//  MSVC std::list : *(head) = sentinelle ; node : next@+0, valeur(ItemSkillInfo)@+8.
int ReadInventoryLite(InvItemLite* out, int cap) {
  int n = 0;
  __try {
    void* sentinel = *reinterpret_cast<void* const*>(rag::kInventoryListAddr);
    const int count = *reinterpret_cast<const int*>(rag::kInventoryCountAddr);
    if (!sentinel || count <= 0) return 0;
    void* node = *reinterpret_cast<void* const*>(sentinel);  // sentinelle->next = 1er noeud
    for (int i = 0; i < count && n < cap && node && node != sentinel; ++i) {
      const uint8_t* info = reinterpret_cast<const uint8_t*>(node) + 8;
      InvItemLite& it = out[n];
      it.type     = *reinterpret_cast<const int*>(info + rag::equip::kOffType);
      it.index    = *reinterpret_cast<const int*>(info + rag::equip::kOffInvIndex);
      it.loc      = *reinterpret_cast<const int*>(info + rag::equip::kOffLocation);
      it.equipped = *reinterpret_cast<const int*>(info + rag::equip::kOffWear) != 0;
      it.refine   = *reinterpret_cast<const int*>(info + rag::equip::kOffRefine);
      it.grade    = *reinterpret_cast<const short*>(info + rag::equip::kOffGrade);
      for (int c = 0; c < 4; ++c)
        it.cards[c] = *reinterpret_cast<const uint32_t*>(info + rag::equip::kOffCards + c * 4);
      const char* rn = rag::clientstr::Data(info + rag::equip::kOffResname);
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
        rag::kSessionAddr + (costume ? rag::equip::kOwnCostumeBase : rag::equip::kOwnEquipBase) + slot * rag::equip::kSlotStride);
    if (*reinterpret_cast<const int*>(e + rag::equip::kOffInvIndex) == 0 ||
        *reinterpret_cast<const int*>(e + rag::equip::kOffPresent) != 1)
      return false;
    out->index    = *reinterpret_cast<const int*>(e + rag::equip::kOffInvIndex);
    out->type     = *reinterpret_cast<const int*>(e + rag::equip::kOffType);
    out->loc      = *reinterpret_cast<const int*>(e + rag::equip::kOffLocation);
    out->equipped = true;
    out->refine   = *reinterpret_cast<const int*>(e + rag::equip::kOffRefine);
    out->grade    = *reinterpret_cast<const short*>(e + rag::equip::kOffGrade);
    for (int c = 0; c < 4; ++c)
      out->cards[c] = *reinterpret_cast<const uint32_t*>(e + rag::equip::kOffCards + c * 4);
    const char* rn = rag::clientstr::Data(e + rag::equip::kOffResname);
    out->nameid = (rn && rn[0]) ? static_cast<uint32_t>(std::atoi(rn)) : 0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
//  L'item d'inventaire `it` appartient-il a la famille de `kind` ? Un costume porte un bit
//  EQP costume, une munition le bit 0x8000, une piece d'equipement ni l'un ni l'autre. Le
//  test est PERMISSIF pour l'equipement : un `loc` lu a zero y passe, ce qui preserve le
//  comportement des presets d'avant, ou la famille n'existait pas.
bool KindMatches(PresetKind kind, const InvItemLite& it) {
  const uint32_t loc = static_cast<uint32_t>(it.loc);
  switch (kind) {
    case PresetKind::kCostume: return (loc & rag::equip::kEqpCostumeMask) != 0;
    case PresetKind::kAmmo:    return (loc & rag::equip::kEqpAmmo) != 0;
    case PresetKind::kEquip:
    default:                   return (loc & (rag::equip::kEqpCostumeMask | rag::equip::kEqpAmmo)) == 0;
  }
}
//  Resout un item de preset -> index inventaire courant (-1 si absent). Priorite au match
//  EXACT (refine+cartes+grade) ; a defaut, 1er item de meme nameid.
int ResolvePresetItem(const EquipPresetItem& pi, const InvItemLite* inv, int n) {
  int fallback = -1;
  for (int i = 0; i < n; ++i) {
    if (inv[i].nameid != pi.nameid || pi.nameid == 0) continue;
    if (!KindMatches(pi.kind, inv[i])) continue;
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
      s->base[i]  = *reinterpret_cast<const int*>(rag::kStatBaseAddr + i * 4);
      s->bonus[i] = *reinterpret_cast<const int*>(rag::kStatBonusAddr + i * 4);
      s->raise[i] = *reinterpret_cast<const int*>(rag::kStatRaiseCostAddr + i * 4);
    }
    s->points   = *reinterpret_cast<const int*>(rag::kOwnStatusPointsAddr);
    s->atk1     = *reinterpret_cast<const int*>(rag::kOwnAtk1Addr);
    s->atk2     = *reinterpret_cast<const int*>(rag::kOwnAtk2Addr);
    s->def_s    = *reinterpret_cast<const int*>(rag::kOwnDefSoftAddr);
    s->def_h    = *reinterpret_cast<const int*>(rag::kOwnDefHardAddr);
    s->matk_min = *reinterpret_cast<const int*>(rag::kOwnMatkMinAddr);
    s->matk_max = *reinterpret_cast<const int*>(rag::kOwnMatkMaxAddr);
    s->mdef_s   = *reinterpret_cast<const int*>(rag::kOwnMdefSoftAddr);
    s->mdef_h   = *reinterpret_cast<const int*>(rag::kOwnMdefHardAddr);
    s->hit      = *reinterpret_cast<const int*>(rag::kOwnHitAddr);
    s->crit     = *reinterpret_cast<const int*>(rag::kOwnCritAddr);
    s->flee     = *reinterpret_cast<const int*>(rag::kOwnFleeAddr);
    s->pdodge   = *reinterpret_cast<const int*>(rag::kOwnPerfectDodgeAddr);
    s->aspd_raw = *reinterpret_cast<const int*>(rag::kOwnAttackDelayAddr);
    s->base_lvl = rag::BaseLevel();
    s->job_lvl  = rag::JobLevel();
    s->hp       = rag::OwnHp();
    s->hp_max   = rag::OwnMaxHp();
    s->sp       = rag::OwnSp();
    s->sp_max   = rag::OwnMaxSp();
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

//  Infos de GUILDE (RE live x32dbg 2026-07-11). CGuild @0x0159c188 : +0 = nom de guilde
//  (std::string), +0xdc = tête de la liste des membres (sentinelle). Niveau @0x0159c1e8,
//  est-maître @0x0159c23c, guildId @0x0159c230 (clé emblème = acteur+0x2e8 aussi). La
//  POSITION du joueur = son enregistrement dans le roster : payload à node+8 (AID @+0,
//  nom @+8, NOM DE POSTE std::string @+0x34), on matche par AID.
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

// (L'invitation dans le groupe passe par ChatWindow::QueueNameAction : elle est
// jouée hors frame et par le chemin NATIF, seul moyen de ne pas parier sur
// l'opcode — `clif_parse_PartyInvite2` est un paquet SHUFFLE côté serveur.)
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
//  cf. storage_window) ; pour 20250716 c'est bien le chat, confirmé des DEUX côtés : table
//  de longueurs du client (docs/opcode_map.csv) et clif_parse_GlobalMessage serveur.
constexpr uint16_t kOpChatMessage     = 0x00f3;
constexpr char     kCmdGuildStorage[] = "@guildstorage";
//  ⚠ Sans argument NI confirmation serveur : appelle guild_break() immédiatement, et
//  refuse si le joueur n'a pas gmaster_flag. Toute la prudence est donc côté client.
constexpr char     kCmdBreakGuild[]   = "@breakguild";

constexpr int kMaxGuildMembers = 128;  // MAX_GUILD serveur = 76 ; marge confortable

// Adaptateur d'ADRESSE, et rien de plus : les globales de guilde sont désignées
// par des `uintptr_t` nus dans tout ce fichier, là où la lecture partagée prend
// un pointeur. Le décodage de la std::string, lui, n'est plus ici.
void ReadStdStringSEH(uintptr_t addr, char* out, int outCap) {
  rag::clientstr::CopyTruncating(reinterpret_cast<const void*>(addr), out, outCap);
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
    g->guildId = rag::OwnGuildId();
    ReadStdStringSEH(rag::kGuildObjAddr, g->name, sizeof(g->name));  // CGuild+0 = nom
    if (g->guildId <= 0 || g->name[0] == '\0') return false;  // pas en guilde
    g->level   = *reinterpret_cast<const int*>(rag::kGuildLevelAddr);
    g->master  = *reinterpret_cast<const int*>(rag::kGuildIsMasterAddr) != 0;
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
        *reinterpret_cast<uint8_t* const*>(rag::kGuildObjAddr + rag::kGuildListHeadOff);
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
        *reinterpret_cast<uint8_t* const*>(rag::kGuildObjAddr + rag::kGuildListHeadOff);
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
// CZ_REQ_CHANGE_MEMBERPOS : en-tête 4 + UNE entrée de 12 {aid, cid, position}.
//
// 🔴 UN SEUL PAQUET POUR DEUX GESTES, et le commentaire de `SendGuildChangeMaster`
// le disait déjà — « même paquet que ci-dessus, avec position 0 » — tout en le
// recopiant octet pour octet. Ce qui empêchait la réutilisation n'était pas le
// format mais le GARDE : `SendGuildChangePosition` refuse `position <= 0`, donc
// elle ne pouvait pas servir au transfert de direction, qui vaut justement 0.
// Le format vit ici, sans garde ; les deux gestes gardent le leur, qui est
// précisément ce qui les distingue.
void SendGuildMemberPos(uint32_t aid, uint32_t cid, uint32_t positionId) {
  uint8_t pkt[16];
  *reinterpret_cast<uint16_t*>(pkt + 0)  = kOpGuildChangePos;
  *reinterpret_cast<uint16_t*>(pkt + 2)  = 16;  // longueur totale (en-tête 4 + 1 entrée de 12)
  *reinterpret_cast<uint32_t*>(pkt + 4)  = aid;
  *reinterpret_cast<uint32_t*>(pkt + 8)  = cid;
  *reinterpret_cast<uint32_t*>(pkt + 12) = positionId;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}
// Change le poste d'UN membre. position 0 est INTERDIT ici : côté serveur il
// déclenche guild_gm_change (transfert de la direction de la guilde), qui n'a rien
// à faire dans un menu contextuel.
void SendGuildChangePosition(uint32_t aid, uint32_t cid, int positionId) {
  if (positionId <= 0) return;
  SendGuildMemberPos(aid, cid, static_cast<uint32_t>(positionId));
}
// Transfère la DIRECTION de la guilde. Même paquet que ci-dessus, avec position 0 :
// c'est ce zéro que le serveur traduit en guild_gm_change (clif_parse_GuildChangeMemberPosition).
// Il n'exige que le drapeau maître et le CHAR id de la cible — l'account id est ignoré
// sur ce chemin, mais le natif le remplit : on fait pareil. Aucun besoin que le membre
// soit connecté. Refus possibles : guerre de guildes (guild_leaderchange_woe: no),
// dernier transfert trop récent (guild_leaderchange_delay) et instance de guilde en
// cours ; les deux premiers reviennent en clif_msg, le dernier est MUET.
void SendGuildChangeMaster(uint32_t aid, uint32_t cid) {
  if (cid == 0) return;
  SendGuildMemberPos(aid, cid, 0);  // 0 = transfert de la direction
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
  *reinterpret_cast<uint32_t*>(pkt + 2) = static_cast<uint32_t>(ReadInt(rag::kOwnCharIdAddr));
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

// Nom de classe d'un job id ARBITRAIRE — le ROSTER DE GUILDE : le tri et la
// colonne « Classe ».
//
// 🔴 DEUX défauts soldés d'un coup le 2026-08-24, et le second a permis de
// trouver le premier :
//
//  · ce fichier portait son propre cache et son propre repli « Classe %d », à
//    l'identique de `rag::social::JobName` — dont le commentaire disait déjà
//    « comme le fait character_sheet pour les membres de guilde ». La dette
//    était DÉCLARÉE dans le code sans être soldée. Un roster de guilde et un
//    roster de groupe posent la même question ; il n'y a plus qu'un cache.
//
//  · la version d'ici passait `sex = -1` au résolveur natif, c'est-à-dire le
//    sexe du joueur LOCAL, et se trompait donc de libellé sur tout membre de
//    guilde du sexe opposé. Exactement le défaut trouvé dans la fenêtre de
//    groupe. `rag::social::JobName` passe 99 — le nom de la classe de BASE, sans
//    variante de sexe. Cf. ragnarok/globals.h.
const char* JobName(int jobId) { return rag::social::JobName(jobId); }

// Le nom NU d'un item par id passe par itemcell::NameById (cache partagé). Pour
// un item ÉQUIPÉ dont on tient le slot, préférer itemcell::BuildDisplayName :
// elle compose refine et cartes, que le nom nu ne porte pas.

// ═══ Homoncule ══════════════════════════════════════════════════════════════
// Adresses, drapeaux, lecture d'état et parcours de la liste de compétences vivent
// dans `ragnarok/homunculus.h` — partagés avec la barre de raccourcis, qui doit
// reconnaître un skill d'homoncule posé dans une case et le lancer par le bon
// chemin. Ne restent ici que les ÉMETTEURS et les libellés de cet onglet.
// RE complet : docs/homunculus_re.md.

// CZ_COMMAND_MER : {op, type.W = 0, command.B}. command 0 = info (le serveur ne fait
// RIEN), 1 = nourrir, 2 = supprimer. Le natif n'envoie jamais le 0.
constexpr uint16_t kOpHomunMenu   = 0x022d;
constexpr uint8_t  kHomunCmdFeed  = 1;
constexpr uint8_t  kHomunCmdDelete = 2;
constexpr uint16_t kOpHomunRename = 0x0231;  // {op, name[24]}
constexpr uint32_t kConfigHomunAutoFeed = 3;

// Ids MsgStringTable — on affiche le libellé EXACT du client, jamais une paraphrase.
// (kMsiHomunInfo / kMsiName : plus utilisés — l'onglet porte déjà le titre, et le nom
//  est son propre bouton, sans libellé « Name : » qui coûterait une ligne à 280 px.)
constexpr int kMsiLevel         = 0x198;
constexpr int kMsiHomunExp      = 0x3fb;
constexpr int kMsiHunger        = 0x249;
constexpr int kMsiIntimacy      = 0x24a;
constexpr int kMsiAutoFeeding   = 0xccf;
constexpr int kMsiAutoFeedInfo  = 0xcd4;
constexpr int kMsiDeleteHomun   = 0x3bb;
constexpr int kMsiNameTooLong   = 0x3aa;
constexpr int kMsiHomunHungry   = 0x3fa;  // « Your Homunculus is starving… »

// 🔴 Ces deux mises en forme ont DÉMÉNAGÉ dans `msgstr` (ragnarok/msgstring.h),
// à côté de la table dont elles traitent les libellés : la fiche de pet en avait
// exactement le même besoin, et une deuxième copie était une copie de trop. Les
// noms locaux restent pour ne pas toucher aux appels de ce fichier — la logique,
// elle, n'existe plus qu'à un seul endroit.
const char* FlattenLabel(const char* src) { return msgstr::Flatten(src); }
const char* StripRoColors(const char* src) { return msgstr::StripColors(src); }

// Paliers d'intimité, repris tels quels de UIHomunInfoWnd::OnRender (0x0087E7B0).
// Bornes IDENTIQUES à hom_intimacy_intimacy2grade côté serveur.
int HomunIntimacyMsgId(int intimacy) {
  if (intimacy <= 3)    return 0x3fe;  // Hate with a Passion
  if (intimacy <= 10)   return 0x3fd;  // Hate
  if (intimacy <= 100)  return 0x2a0;  // Awkward
  if (intimacy <= 250)  return 0x2a1;  // Shy
  if (intimacy <= 750)  return 0x29d;  // Neutral
  if (intimacy <= 910)  return 0x2a2;  // Cordial
  if (intimacy <= 1000) return 0x2a3;  // Loyal
  return 0x2a4;                        // Unknown
}

// CZ_COMMAND_MER (5 o). `type` reste à 0 comme le natif ; seul `command` compte.
void SendHomunMenu(uint8_t command) {
  uint8_t pkt[5];
  std::memset(pkt, 0, sizeof(pkt));
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpHomunMenu;
  pkt[4] = command;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}
// CZ_RENAME_MER (26 o) : nom sur 24 octets, tronqué à 23 + terminateur comme le natif.
void SendHomunRename(const char* name) {
  uint8_t pkt[26];
  std::memset(pkt, 0, sizeof(pkt));
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpHomunRename;
  if (name && name[0]) std::strncpy(reinterpret_cast<char*>(pkt + 2), name, 23);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}
// CZ_CONFIG (10 o), type 3 = auto-alimentation de l'homoncule. Le serveur répond par
// un ZC_CONFIG que le client applique lui-même sur son global (0x00DA8740) : on
// n'écrit RIEN localement, on attend l'accusé — sinon la case mentirait sur un refus.
void SendHomunAutoFeed(bool on) {
  uint8_t pkt[10];
  *reinterpret_cast<uint16_t*>(pkt + 0) = Bourgeon::kOpConfig;
  *reinterpret_cast<uint32_t*>(pkt + 2) = kConfigHomunAutoFeed;
  *reinterpret_cast<uint32_t*>(pkt + 6) = on ? 1u : 0u;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// ── Icône de skill (case compagnon) ──────────────────────────────────────────
std::unordered_map<uint32_t, ro::IconTex> g_skill_icon_cache;
// Le .bmp d'icône de skill est nommé par l'idname Lua (rejet des sentinelles
// "Unknown"/"Zero Skill" qui spamment la console de chargement).
bool BuildSkillIconPathSafe(int skillId, char* out, int n) {
  out[0] = '\0';
  __try {
    const char* idn = lua::SkillIdName(skillId);
    if (!idn || !idn[0]) return false;
    if (std::strstr(idn, "nknown") || std::strcmp(idn, "Zero Skill") == 0) return false;
    std::snprintf(out, n, "%s\\item\\%s.bmp", ro::uipath::kUiRoot, idn);
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

// Voile de cooldown sur une icône de grille (Grimoire et compétences de guilde),
// exactement la convention de la barre de raccourcis : un fond noir qui MONTE du bas
// à mesure que le temps passe, plus le décompte au centre. Le voile seul ne dit rien
// d'un cooldown de guilde de plusieurs minutes — il bouge d'un pixel par seconde.
// Table alimentée par ZC_SKILL_POSTDELAY (0x043D), cf. ragnarok/skill_cooldowns.h.
void DrawSkillCooldownOverlay(ImDrawList* dl, uint16_t skillId, const ImVec2& icon_tl,
                              float icon_size) {
  const float fraction = ro::SkillCooldownFraction(skillId);
  if (fraction <= 0.0f) return;
  const float veil_h = icon_size * fraction;
  dl->AddRectFilled(ImVec2(icon_tl.x, icon_tl.y + icon_size - veil_h),
                    ImVec2(icon_tl.x + icon_size, icon_tl.y + icon_size),
                    IM_COL32(0, 0, 0, 150), 3.0f);
  // ms == 0 alors que le voile est là = cooldown issu du repli natif, sans durée
  // exploitable : on garde le voile, sans chiffre.
  const unsigned long ms = ro::SkillCooldownRemainingMs(skillId);
  if (ms == 0) return;
  char left[16];
  if (ms >= 60000) std::snprintf(left, sizeof(left), "%lu:%02lu", ms / 60000, (ms / 1000) % 60);
  else             std::snprintf(left, sizeof(left), "%lu", (ms + 999) / 1000);
  const ImVec2 sz = ImGui::CalcTextSize(left);
  const ImVec2 at(icon_tl.x + (icon_size - sz.x) * 0.5f,
                  icon_tl.y + (icon_size - sz.y) * 0.5f);
  ro::AddTextRelief(dl, at, IM_COL32(255, 235, 150, 255), left,
                    ro::pal::kTextShadow, ImVec2(1.0f, 1.0f));
}

// Filtre d'échantillonnage des icônes du grimoire, posé par un callback de draw list
// comme la barre de raccourcis (cf. skill_bar). Le natif ne filtre RIEN (les
// .bmp d'icônes font 24 px et sont blités tels quels) : le mode NET est donc le
// défaut, et c'est LUI qu'il faut imposer — l'état ambiant d'une draw list ImGui est
// LINEAR (le backend DX9 le remet dans SetupRenderState, imgui_impl_dx9.cpp:139).
// ⚠ Même raison pour la restauration : un ImDrawCallback_ResetRenderState rendrait
// la main en LINEAR et ramollirait les blits de skin dessinés ensuite.
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
constexpr uintptr_t kSkillSetUseLevel = 0x00738570;  // __thiscall(bundle, id, lv)
constexpr uintptr_t kSkillGetUseLevel = 0x00d5e3c0;  // __thiscall(SESSION, id) — même tableau
constexpr uintptr_t kIsLevelUseSkill  = 0x0073adb0;  // __cdecl(id) : l'effet dépend-il du niveau ?
using GetTabList_t  = void* (__fastcall*)(void*, void*, int);
using SetUseLevel_t = void  (__fastcall*)(void*, void*, int, int);
using GetUseLevel_t = int   (__fastcall*)(void*, void*, int);
using IsLevelUse_t  = char  (__cdecl*)(int);

// ⚠ L'emplacement mgr+0x2C4 qui portait la fenêtre du grimoire ne sert plus : on
// ne SUIT plus son existence, on la détruit.
// ── Les trois fenêtres natives que cette feuille REMPLACE ───────────────────
// Leurs identifiants et vtables sont au foyer : `uiwnd::kUIEquipWnd`,
// `kUIStatusWnd`, `kUINewSkillListWnd`. Relevés en live dans la map du
// gestionnaire (mgr 0x0131f4e8) — c'est ce relevé partagé qui les y a menés.

// ── Guilde : DEUX fenêtres, choisies selon qu'on a une guilde ou non ─────────
// Les deux chemins d'ouverture le font pareil, et sur le MÊME critère —
// `g_Own_GuildId` (0x0159c230, label Ghidra « ActiveTabIndex » FAUX) :
//   - raccourci  : UIWindowMgr_DispatchHotkeyBehavior @0x00a455cf
//   - icône menu : UIMenuIconWnd_OnMsg cmd 373 @0x00814c6d (`cmovl`)
// guildId != 0 -> bascule 0x3B ; guildId == 0 -> bascule 0xD4 (fenêtre « pas de
// guilde » / création). Notre onglet Guilde couvre les deux cas, création comprise.
// Panneaux enfants du conteneur (0x3c TotalInfo, MemberManage, PositionManage,
// Skill, AllyGuild, InfoPopup, Banished) : id = 0x3c + rang. Le conteneur les crée
// lui-même, il faut donc les détruire avec lui — chacun est une native de plus, et
// une native masquée garde le clavier.
constexpr int       kTabGuild       = 4;     // onglet « Guilde » de la feuille
constexpr int       kTabHomun       = 6;     // onglet « Homoncule » (CONDITIONNEL)

// ── Homoncule : DEUX natives remplacées, qui mènent au MÊME onglet ───────────
// 113 = UIHomunInfoWnd (fiche d'état, raccourci Alt+R) et 114 = UISkillListWnd en
// mode homoncule (arbre de compétences, ouvert par le bouton « btn_skill » de la
// 113). Notre onglet les fusionne, comme il fusionne déjà Status + Équipement.
//
// ⚠ La 113 ne naît QUE si le client connaît la classe de l'homoncule
// (`MakeWindow` case 113 teste `g_Homun_Class != -1`) : sans homoncule, le
// raccourci ne produit aucune création — donc rien à router, et notre onglet est
// absent de la barre pour la même raison.
//
// ⚠ Les détruire est SANS RISQUE pour les paquets : le décodeur de
// ZC_PROPERTY_HOMUN écrit les globals d'abord et ne touche aux fenêtres que sous
// `if (instance)` (0x00CD1ED0, et `Homun_InvalidateInfoWnd` 0x00D70D30 pareil).
// Les données restent donc à jour, fenêtres ou pas. Cf. docs/homunculus_re.md.

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
    uint8_t* list_obj = reinterpret_cast<uint8_t*>(rag::kSkillFlatListAddr);
    if (tab >= 0)
      list_obj = reinterpret_cast<uint8_t*>(reinterpret_cast<GetTabList_t>(rag::kSkillGetTabListAddr)(
          reinterpret_cast<void*>(rag::kSkillBundleAddr), nullptr, tab));
    if (!list_obj) return 0;
    uint8_t* head = *reinterpret_cast<uint8_t**>(list_obj);
    if (!head) return 0;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head);
    int guard = 0;
    while (node && node != head && n < cap && guard++ < kSkillMaxNodes) {
      const uint8_t* v = node + rag::listnode::kValue;
      node = *reinterpret_cast<uint8_t**>(node);  // avancer AVANT de lire la valeur
      if (*reinterpret_cast<const int*>(v + rag::skillinfo::kValid) == 0) continue;
      const int id = *reinterpret_cast<const int*>(v + rag::skillinfo::kId);
      if (id <= 0) continue;
      // ⚠ UNE LISTE D'ONGLET PEUT CONTENIR DEUX FOIS LA MÊME COMPÉTENCE. L'insertion
      // native (sub_007381D0, appelée par c_AddSkillList) ne cherche JAMAIS l'id avant
      // d'empiler, et la construction (sub_00D96790) rejoue InitSkillTreeView pour
      // CHAQUE job de la chaîne d'héritage — plusieurs d'entre eux peuvent viser le
      // même onglet (le sommet de chaîne et les jobs 4218/4220 vont tous les deux dans
      // l'onglet 0 : cas du Doram). Le doublon donnait deux cases pour une compétence
      // et deux widgets ImGui de MÊME ID (« 2 visible items with conflicting ID »).
      // On garde la PREMIÈRE fiche : c'est aussi celle que le natif retient, sa
      // recherche par id (FindInTree 0x00738320) rendant la main au premier match —
      // donc celle que les mises à jour du serveur ont suivie. Les copies suivantes ne
      // servent qu'à COMBLER : une case si la première n'en avait pas, et le niveau
      // appris s'il n'a été écrit que sur l'une d'elles.
      int dup = -1;
      for (int k = 0; k < n && dup < 0; ++k)
        if (out[k].id == id) dup = k;
      if (dup >= 0) {
        const int dpos = *reinterpret_cast<const int*>(v + rag::skillinfo::kPos);
        if (out[dup].pos < 0 && dpos >= 0) out[dup].pos = dpos;
        int dlearned = *reinterpret_cast<const int16_t*>(v + rag::skillinfo::kLearned);
        if (dlearned <= 0) dlearned = *reinterpret_cast<const int*>(v + rag::skillinfo::kLevel);
        if (dlearned > out[dup].learned) out[dup].learned = dlearned;
        continue;
      }
      SkillRaw& s = out[n];
      s.id         = id;
      s.inf        = *reinterpret_cast<const int*>(v + rag::skillinfo::kInf);
      s.sp         = *reinterpret_cast<const int*>(v + rag::skillinfo::kSp);
      s.range      = *reinterpret_cast<const int*>(v + rag::skillinfo::kRange);
      s.pos        = *reinterpret_cast<const int*>(v + rag::skillinfo::kPos);
      s.maxlv      = *reinterpret_cast<const int*>(v + rag::skillinfo::kMaxLevel);
      s.user_up    = *reinterpret_cast<const int*>(v + rag::skillinfo::kUserUp);
      s.upgradable = *reinterpret_cast<const int*>(v + rag::skillinfo::kUpgrade);
      // Niveau appris : +0x30 fait foi (le serveur), +0x10 sert de repli.
      s.learned    = *reinterpret_cast<const int16_t*>(v + rag::skillinfo::kLearned);
      if (s.learned <= 0) s.learned = *reinterpret_cast<const int*>(v + rag::skillinfo::kLevel);
      s.need_count = 0;
      const uint8_t* first = *reinterpret_cast<const uint8_t* const*>(v + rag::skillinfo::kNeedVec);
      const uint8_t* last  = *reinterpret_cast<const uint8_t* const*>(v + rag::skillinfo::kNeedVec + 4);
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
  __try { return *reinterpret_cast<const int*>(rag::kOwnSkillPointsAddr); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// « Niveau d'utilisation » : le niveau auquel la compétence part quand on la lance.
// Purement CLIENT (persisté dans le JSON du personnage), et c'est ce que la barre de
// raccourcis envoie. Écriture par le bundle, lecture par la session : c'est le même
// tableau (bundle+0x44 == session+0x50), pas une incohérence.
int GetUseLevelSEH(int skillId) {
  __try {
    return reinterpret_cast<GetUseLevel_t>(kSkillGetUseLevel)(
        reinterpret_cast<void*>(rag::kSessionAddr), nullptr, skillId);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void SetUseLevelSEH(int skillId, int level) {
  __try {
    reinterpret_cast<SetUseLevel_t>(kSkillSetUseLevel)(
        reinterpret_cast<void*>(rag::kSkillBundleAddr), nullptr, skillId, level);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// L'effet de la compétence dépend-il du niveau ? Le natif n'affiche « n / m » que
// dans ce cas (sinon un seul nombre) : même règle ici.
bool IsLevelUseSkillSEH(int skillId) {
  __try { return reinterpret_cast<IsLevelUse_t>(kIsLevelUseSkill)(skillId) != 0; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Niveau de lancement EFFECTIF. ⚠ Le getter renvoie 0 tant que le joueur n'a JAMAIS
// touché au réglage — 0 veut dire « pas réglé », pas « niveau 1 ». Le borner par
// max(1, ...) faisait partir et GLISSER les compétences au niveau 1 ; le défaut est le
// niveau APPRIS, c'est-à-dire le maximum. Même repli pour un réglage devenu trop haut
// (compétence montée puis rebaissée par un changement de classe).
int EffectiveUseLevelSEH(int skillId, int learned) {
  int use = GetUseLevelSEH(skillId);
  if (use <= 0 || (learned > 0 && use > learned)) use = learned;
  return use < 1 ? 1 : use;
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
    rag::clientstr::CopyTruncating(sbuf, out, outCap);
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

// ── Le cadre d'emblème, que le CLIENT ne dessine jamais ─────────────────────
//
// Le réglage « Bordure d'emblème » (`TT_EMBLEM_FRAME_ON_OFF` 0xF3) existe, son
// bitmap est bien dans `data.grf` (`유저인터페이스\emblem_frame.bmp`, 28×28), et
// le client a le code pour le peindre — `Guild_DrawEmblemOnPartyHUD`
// (`0x00825160`) blitte le cadre puis décale l'emblème de 2 px.
//
// 🔴 CE CADRE NE S'AFFICHE JAMAIS EN JEU, ET LA CAUSE EST ÉTABLIE — désassemblage
// ET expérience en direct (x32dbg, 2026-08-15). L'étiquette de nom d'un acteur
// est construite par `GameMode_BuildActorNameLabel` (0x00C6D720), qui calcule le
// drapeau de cadre par une CONJONCTION :
//
//     cadre  =  ( *(CGameMode+0xCC) + 0x4C  ==  1 )   ET   ( drapeau /frame )
//
// Ce premier terme — `GameSession_GetField4c()` — vaut **0** sur ce client. La
// conjonction retombe donc à zéro quoi que dise `/frame`, le drapeau part à
// `UIActorNameLabel_SetEmblem` (0x0082DCC0) qui l'écrit en `this[0xBD]`, et
// `sub_825510` saute sa branche de cadre (`cmp byte [edi+0BDh], 1`).
// Preuve : forcer ce champ à 1 en mémoire FAIT APPARAÎTRE le cadre.
//
// Le même `field4c` garde aussi `Guild_DrawEmblemOnPartyHUD` (0x00825160), le
// second consommateur du bitmap. Les deux chemins tombent par la même cause.
//
// 🔴 ET CE CHAMP EST « JE SUIS SUR UNE CARTE DE SIÈGE ». Il est posé par
// `ZC_NOTIFY_MAPPROPERTY` de valeur **3 = `MAPPROPERTY_AGITZONE`** (0x00CA6426),
// que rAthena envoie quand `mapdata_flag_gvg(map)` est vrai (clif.cpp:14669) —
// et la même branche force `TT_MIN_EFFECT_ON_OFF`, la signature d'une carte de
// siège. Le champ est donc renommé `GameSession_IsAgitZone`.
//
// ➡ **Le cadre d'emblème est une fonction de WoE.** Le client ne l'affiche que
// là où distinguer les guildes d'un coup d'œil sert à quelque chose. Ailleurs
// il ne peut PAS apparaître, et `/frame` seul n'y changera jamais rien : ce
// n'est pas une panne, c'est la conception.
//
// C'est aussi pourquoi on ne force rien : ce champ dit au client sur quel type
// de carte il est. Le mentir pour un liseré, c'est mentir aussi aux étiquettes
// de nom, au clic sur les acteurs et au survol du curseur, qui le lisent tous.
//
// ➡ On l'honore donc là où NOUS dessinons l'emblème. Le bitmap est celui du
// client, chargé par son propre TexMgr : il suit le skin actif comme n'importe
// quelle autre pièce d'interface, et son magenta devient transparent tout seul.
//
// Géométrie reprise du natif, en proportions : le cadre occupe la boîte entière,
// l'emblème est rentré de 2/28ᵉ de chaque côté. C'est ce qui garde le rendu juste
// à toutes les tailles — l'en-tête l'affiche en 48 px, la poupée en 24.
constexpr float kEmblemFrameSide  = 28.0f;
constexpr float kEmblemFrameInset = 2.0f;
// `TT_EMBLEM_FRAME_ON_OFF` — l'identifiant du réglage, celui que la commande
// `/frame` bascule. (Elle existe : son nom est déclaré dans la TABLE DES
// MESSAGES, `MSI_DRAW_EMBLEM_FRAME_ONOFF` = 3532, et non comme littéral de
// l'exécutable — une recherche d'octets dans l'exe ne la trouve donc pas.)
constexpr int kTtEmblemFrame = 0xf3;

ro::GameTexture EmblemFrameTexture() {
  // Composé au format sur la racine partagée plutôt que collé à un littéral :
  // l'octet `\xBA` qui termine le préfixe CP949 n'a plus de `\` à avaler.
  char path[128];
  std::snprintf(path, sizeof(path), "%s\\emblem_frame.bmp", ro::uipath::kUiRoot);
  return ro::CachedTextureFromGameFile(path);
}

// Dessine l'emblème dans la boîte donnée, encadré si le joueur l'a demandé.
// Rend true si le cadre du client a été posé — l'appelant sait alors qu'il n'a
// pas à ajouter sa propre décoration par-dessus.
bool DrawEmblemBoxed(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1,
                     void* emblem_tex) {
  if (!emblem_tex) return false;
  if (!gamesettings::IsOn(kTtEmblemFrame)) {
    dl->AddImage(reinterpret_cast<ImTextureID>(emblem_tex), p0, p1);
    return false;
  }
  const ro::GameTexture frame = EmblemFrameTexture();
  if (!frame.tex) {
    // Cadre demandé mais introuvable : afficher l'emblème nu plutôt que rien.
    dl->AddImage(reinterpret_cast<ImTextureID>(emblem_tex), p0, p1);
    return false;
  }
  const float inset = (p1.x - p0.x) * (kEmblemFrameInset / kEmblemFrameSide);
  dl->AddImage(reinterpret_cast<ImTextureID>(frame.tex), p0, p1);
  dl->AddImage(reinterpret_cast<ImTextureID>(emblem_tex),
               ImVec2(p0.x + inset, p0.y + inset),
               ImVec2(p1.x - inset, p1.y - inset));
  return true;
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

// Octets BMP de l'emblème que porte ACTUELLEMENT la guilde : le .ebm du client, une
// fois inflaté, est exactement le BMP 24x24 d'origine. C'est ce qui permet d'ouvrir
// l'éditeur sur l'emblème en place au lieu d'une page blanche. Vide si le fichier
// n'est pas encore descendu (GetEmblemPathSafe déclenche alors le téléchargement) ou
// s'il n'est pas décompressible.
std::vector<uint8_t> CurrentEmblemBmp(int guildId) {
  std::vector<uint8_t> bmp;
  if (guildId <= 0) return bmp;
  char rel[264] = {0};
  GetEmblemPathSafe(guildId, rel, sizeof(rel));
  if (!rel[0]) return bmp;
  const std::string full = paths::InGameDir("_tmpEmblem\\") + rel;
  const std::vector<uint8_t> comp = ReadWholeFile(full.c_str(), 1 << 20);
  if (comp.empty()) return bmp;
  if (!tinf::zlib_uncompress(comp.data(), comp.size(), bmp)) bmp.clear();
  return bmp;
}

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
  if (bmp.size() < 54) return fail(i18n::Tr("Fichier trop court pour un BMP."));
  if (bmp[0] != 'B' || bmp[1] != 'M') return fail(i18n::Tr("Ce n'est pas un BMP (signature « BM » absente)."));
  const uint32_t declared = *reinterpret_cast<const uint32_t*>(&bmp[2]);
  // Le serveur compare bfSize à la taille réellement reçue : un en-tête menteur
  // (fréquent après une conversion) est rejeté sans le moindre message en jeu.
  if (declared != bmp.size()) return fail(i18n::Tr("En-tête BMP incohérent (taille déclarée != taille du fichier)."));
  const uint32_t dataOff = *reinterpret_cast<const uint32_t*>(&bmp[0x0a]);
  if (dataOff >= bmp.size()) return fail(i18n::Tr("En-tête BMP incohérent (offset des pixels hors fichier)."));
  const int32_t w    = *reinterpret_cast<const int32_t*>(&bmp[0x12]);
  const int32_t hraw = *reinterpret_cast<const int32_t*>(&bmp[0x16]);
  const int32_t h    = (hraw < 0) ? -hraw : hraw;
  if (w != kEmblemSide || h != kEmblemSide) {
    if (why) {
      char text[96];
      std::snprintf(text, sizeof(text), i18n::Tr("Dimensions %ldx%ld : le jeu n'affiche que du %dx%d."),
                    static_cast<long>(w), static_cast<long>(h), kEmblemSide, kEmblemSide);
      *why = text;
    }
    return false;
  }
  const int16_t bpp = *reinterpret_cast<const int16_t*>(&bmp[0x1c]);
  if (bpp != 24 && bpp != 8) return fail(i18n::Tr("Profondeur non gérée : utilise du 24 bits ou du 256 couleurs."));
  const uint32_t compression = *reinterpret_cast<const uint32_t*>(&bmp[0x1e]);
  if (compression != 0) return fail(i18n::Tr("BMP compressé (RLE) : enregistre-le sans compression."));
  if (bmp.size() > kEmblemMaxRawBytes) {
    if (why) {
      char text[96];
      std::snprintf(text, sizeof(text), i18n::Tr("Fichier de %zu octets : le serveur en accepte %zu au plus."),
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
      cand.why = i18n::Tr("Fichier illisible ou vide.");
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
  // Vide au départ, PAS pré-rempli : le nom par défaut se traduit (cf. le repli
  // i18n::Tr("mon_embleme") côté envoi) et un initialiseur statique serait figé en
  // français, avant même que la langue soit choisie. Le champ l'annonce en indice.
  char     save_name[32] = "";
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

// ── L'empreinte du PINCEAU, une fois ─────────────────────────────────────────
// Le carré N×N du crayon, ses bornes de canevas et son miroir vertical étaient
// écrits DEUX fois : une pour peindre (`EmblemCanvasPaint`), une pour marquer un
// masque de forme (`MaskSet`, plus bas). Même géométrie, seule l'action à chaque
// cellule change — exactement le motif de `BresenhamLine` juste en dessous.
//
// ⚠ Le miroir appelle `plot` une SECONDE fois, sur la colonne symétrique. Une
// action non idempotente le verrait donc passer deux fois sur la colonne
// centrale d'un canevas de largeur impaire — sans conséquence pour les deux
// usages actuels (écrire une couleur, poser un booléen).
template <typename Plot>
void BrushCells(int cx, int cy, Plot plot) {
  int lo = 0, hi = 0;
  BrushExtent(&lo, &hi);
  for (int dy = lo; dy <= hi; ++dy) {
    for (int dx = lo; dx <= hi; ++dx) {
      const int x = cx + dx, y = cy + dy;
      if (x < 0 || x >= kEmblemSide || y < 0 || y >= kEmblemSide) continue;
      plot(x, y);
      if (g_emblem_canvas.mirror) plot(kEmblemSide - 1 - x, y);
    }
  }
}

void EmblemCanvasPaint(int cx, int cy, uint32_t color) {
  // ⚠ La révision monte UNE fois par geste, pas par cellule : c'est elle qui dit
  // à l'aperçu que le canevas a changé.
  ++g_emblem_canvas.revision;
  BrushCells(cx, cy, [color](int x, int y) {
    g_emblem_canvas.pixel[y * kEmblemSide + x] = color;
  });
}

// ── Bresenham, UNE fois ──────────────────────────────────────────────────────
// L'algorithme était écrit DEUX fois dans ce fichier, à l'identique : une pour
// peindre (`EmblemCanvasStroke`), une pour marquer un masque (`MaskLine`, plus
// bas). Seule différait l'action à chaque cellule. Un algorithme recopié est un
// algorithme qu'on ne peut plus corriger qu'à moitié.
//
// `plot` reçoit (x, y). Le gabarit n'a aucun coût : l'appelant passe une lambda,
// le compilateur l'incorpore.
template <typename Plot>
void BresenhamLine(int x0, int y0, int x1, int y1, Plot plot) {
  int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
  int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
  const int sx = (x0 < x1) ? 1 : -1;
  const int sy = (y0 < y1) ? 1 : -1;
  dy = -dy;
  int err = dx + dy;
  for (;;) {
    plot(x0, y0);
    if (x0 == x1 && y0 == y1) break;
    const int err2 = 2 * err;
    if (err2 >= dy) { err += dy; x0 += sx; }
    if (err2 <= dx) { err += dx; y0 += sy; }
  }
}

// Relie deux cellules : à 60 images/s la souris saute plusieurs pixels entre deux
// frames, et un trait rapide laisserait sinon des pointillés.
void EmblemCanvasStroke(int x0, int y0, int x1, int y1, uint32_t color) {
  BresenhamLine(x0, y0, x1, y1,
                [color](int x, int y) { EmblemCanvasPaint(x, y, color); });
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
// Même carré N×N que le crayon, et le même miroir : c'est ce qui garantit que
// l'aperçu d'une forme et sa peinture couvrent EXACTEMENT les mêmes cellules.
void MaskSet(bool* mask, int cx, int cy) {
  BrushCells(cx, cy, [mask](int x, int y) { mask[y * kEmblemSide + x] = true; });
}
void MaskLine(bool* mask, int x0, int y0, int x1, int y1) {
  BresenhamLine(x0, y0, x1, y1, [mask](int x, int y) { MaskSet(mask, x, y); });
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
  // Toutes les validations sont passées : le dessin en cours va être remplacé, on le
  // met dans la pile d'annulation (comme l'import d'icône d'item) plutôt que de le
  // perdre — l'éditeur peut s'ouvrir directement sur l'emblème de la guilde, et il ne
  // faut pas que ce chargement automatique jette un travail commencé.
  EmblemCanvasPushUndo();
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

// La description passe par itemcell::OpenDescById. La fiche de perso est le cas
// MIXTE : elle a de vrais items — les slots d'équipement de la session — mais pas
// de nœud de liste à passer. Elle utilise donc le paramètre `src` d'OpenDescById,
// qui recopie cartes/refine/grade/options du slot source ; sans lui la fenêtre
// montrerait l'item de BASE, nu.

// Description d'un SKILL : fenêtre 0x2e (≠ 0xc, qui est celle des objets), pilotée par
// l'id BRUT — pas par un ItemSkillInfo. Re-clic sur le même skill = referme, comme le
// natif (l'id affiché vit à +0x104).

// Le nom d'affichage COMPLET (refine, « [N] », préfixes/suffixes de cartes,
// forge) était composé ICI par une copie du name-builder natif — mêmes typedefs,
// même SEH, même repli, à 92 % identique à `itemcell::BuildDisplayName`.
//
// 🔴 Et la copie a coûté ce que coûtent les copies : quand la conversion en UTF-8
// est descendue dans `itemcell::BuildDisplayName`, celle-ci ne l'a pas reçue. Le
// nom du pantin d'équipement et celui de l'aperçu au survol sortaient donc en
// code-page client, droit dans ImGui — le défaut de « Bien-aiméPoring Egg », mais
// dans une fenêtre que le relevé de doublons désignait depuis le début.
//
// Supprimée : `itemcell::BuildDisplayName` fait exactement la même chose, en
// UTF-8, et résout le même contexte natif (le GESTIONNAIRE, jamais une fenêtre —
// c'est lui qui porte la liste de requêtes de noms à +0x18C).

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
void SendConfigToggle(int cmd, int value) {
  __try {
    void* d = rag::ActiveMode();
    if (d) rag::ModeSendMsg(d, cmd, value, 0, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Lance une compétence par le chemin NATIF, EXACTEMENT celui d'une touche de la barre de
// raccourcis : cmd 0x71 du dispatcher, avec la struct d'info de la compétence et le niveau
// — c'est la réplique de la branche SKILL de UIShortCutWnd::OnMsg 0x29 (0x00901310), qui
// fait `ItemMgr_GetInvItemById(&info, id)` puis `SendMsg(0x71, &info, niveau, 0, 0)`.
//
// ⚠ POURQUOI PAS 0x45 : le 0x45 lance sur une CIBLE DONNÉE (arg3 = GID) — on lui passait
// notre propre GID, donc TOUTE compétence partait sur soi-même, ciblées comprises (Arrow
// Vulcan lancé sur le lanceur, sans curseur de visée). Le tri par INF est fait par le
// 0x71 (RE 0x00c8d6bd) : il lit info+0x0C et route vers
//   INF 4  (soi)     -> 0x45 lui-même, avec le bon GID (soi, ou l'homoncule/mercenaire)
//   INF 1  (cible)   -> 0x48 mode 2   |  INF 2  (sol)   -> 0x48 mode 1
//   INF 16 (support) -> 0x48 mode 4   |  INF 8          -> 0x48 mode 3
//   INF 32 (piège)   -> 0x48 mode 5
// 0x48 = « entrer en mode ciblage » (curseur de visée, le clic suivant choisit la cible),
// 0x48 mode 0 = annuler. Le natif détruit la struct dès le retour : le mode ciblage copie
// ce dont il a besoin, on fait pareil.
//
// Repli sur l'ancien 0x45-sur-soi si le natif ne connaît pas l'id (compétences de guilde,
// hors de la liste apprise) — c'est le cas de l'onglet Guilde, qui marchait ainsi.
void SendUseSkill(uint16_t skillId, int level) {
  __try {
    void* d = rag::ActiveMode();
    if (!d) return;
    bool dispatched = false;
    {
      alignas(8) uint8_t entry[0xC0] = {};
      reinterpret_cast<void(__stdcall*)(void*, int)>(itemdb::kFillInfoByIdAddr)(entry, skillId);
      const int found = *reinterpret_cast<const int*>(entry + kSkillEntryFound);
      const int owned = *reinterpret_cast<const int*>(entry + kSkillEntryLevel);
      // Le natif refuse de lancer au-dessus du niveau appris (garde `owned >= niveau`
      // de OnMsg 0x29) : on borne au lieu de refuser, l'appelant a déjà borné au appris.
      int lv = level < 1 ? 1 : level;
      if (owned > 0 && lv > owned) lv = owned;
      if (found) {
        rag::ModeSendMsg(d, rag::kCmdUseSkillSlot,
                                     static_cast<int>(reinterpret_cast<uintptr_t>(entry)),
                                     lv, 0, 0);
        dispatched = true;
      }
      reinterpret_cast<void(__fastcall*)(void*)>(itemdb::kFilledInfoDtorAddr)(entry);
    }
    if (!dispatched) {
      // GID de notre acteur = notre AID : les compétences de guilde se lancent sur soi.
      const uint32_t self = rag::OwnAccountId();
      if (self)
        rag::ModeSendMsg(d, rag::kCmdUseSkill, skillId, static_cast<int>(self),
                                     level, 0);
    }
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
        reinterpret_cast<void*>(rag::kSessionAddr), nullptr, sbuf, id);
    rag::clientstr::CopyTruncating(sbuf, out, static_cast<int>(cap));
    reinterpret_cast<StrDtor_t>(rag::kStdStringDtorAddr)(sbuf, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}
const char* TitleName(int id) {
  auto it = g_title_cache.find(id);
  if (it != g_title_cache.end()) return it->second.c_str();
  char buf[96];
  ResolveTitleSEH(id, buf, sizeof(buf));
  if (buf[0] == '\0') std::snprintf(buf, sizeof(buf), i18n::Tr("Titre #%d"), id);
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
    case 2: return "Garment";
    case 1: return "Weapon";
    case 5: return "Shield";
    case 6: return "Shoes";
    case 3: return "Acc. L";
    case 7: return "Acc. R";
    default: return "";
  }
}


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
//
// 🔴 CES DEUX LARGEURS SE MESURENT — elles ne sont plus écrites en dur (280 et 240
// px). Elles l'étaient, calibrées sur la police d'alors ; le joueur peut changer la
// police de l'interface, et la police intégrée d'ImGui (ProggyClean) est la plus
// large du menu. Avec elle, la valeur d'une stat passait sous ses propres boutons
// et « Générer le GIF » sortait du cadre du mannequin.
//
// Tout ce qui borne la largeur est donc mesuré à la police COURANTE : bloc du
// mannequin, ligne « pose + GIF », case à cocher, colonne des valeurs et boutons de
// montée. Les anciennes valeurs restent le PLANCHER — une police étroite garde
// exactement la mise en page d'origine, et la fenêtre ne rétrécit jamais sous ce
// qu'elle a toujours mesuré.
//
// ⚠ Aucune de ces mesures ne dépend de la largeur de la FENÊTRE : elles ne lisent
// que la police et le style. C'est ce qui les rend utilisables dans la contrainte
// de taille (appelée AVANT Begin) sans boucle de rétroaction d'une frame à l'autre.
// 🔴 CES CONSTANTES SONT EN PIXELS D'ART, PAS EN PIXELS D'ÉCRAN. Elles décrivent
// l'image (une case fait 44 unités de large), et chaque LECTURE les convertit par
// `ro::Px` — l'échelle de l'interface est un réglage du joueur, elle change en
// cours de session, une constante ne peut pas la porter.
//
// La distinction avec le reste de ce bloc est nette et vaut d'être tenue : tout
// ce qui se MESURE sur la police (les `CalcTextSize` ci-dessous) suit déjà
// l'échelle tout seul, puisque la police elle-même la suit. Repasser ces
// mesures-là par `Px` les ferait grossir au carré.
constexpr float kDollWMin  = 280.0f;  // planchers = les largeurs historiques
constexpr float kStatsWMin = 240.0f;
constexpr float kSlotSz    = 44.0f;   // côté d'une case d'équipement
constexpr float kSlotGap   = 6.0f;    // écart entre deux cases
constexpr float kAvatarW   = 130.0f;  // largeur de l'avatar central
// Le bloc du mannequin : 2 colonnes de cases encadrant l'avatar. En PIXELS D'IMAGE
// (cases et sprite), donc insensible à la police — mais pas à l'échelle.
constexpr float kDollBlockW  = kSlotSz + kSlotGap + kAvatarW + kSlotGap + kSlotSz;
constexpr float kPoseLineGap = 8.0f;  // écart net entre le combo de pose et le bouton GIF
constexpr float kStatValGap  = 12.0f; // écart libellé de stat -> colonne des valeurs

// Largeur du combo de pose : le plus large libellé TRADUIT (une traduction plus
// longue que le français élargit le combo, elle ne le déborde pas) + la flèche.
float PoseComboW() {
  float w = 0.0f;
  for (int i = 0; i < kPoseCount; ++i)
    w = std::max(w, ImGui::CalcTextSize(i18n::Tr(kPoses[i].label)).x);
  // Largeurs ARRONDIES au pixel entier : des bordures RO sub-pixel provoquaient un
  // léger glitch visuel à gauche du bouton voisin.
  return std::floor(w + ImGui::GetFrameHeight() +
                    ImGui::GetStyle().FramePadding.x * 2.0f + ro::Px(6.0f));
}
float GifButtonW() {
  return std::floor(ImGui::CalcTextSize(i18n::Tr("Générer le GIF")).x +
                    ImGui::GetStyle().FramePadding.x * 2.0f + ro::Px(12.0f));
}
// Largeur de la case à cocher sous le mannequin : les DEUX libellés possibles
// (Équipement / Costume), pour que la fenêtre ne change pas de taille en changeant
// d'onglet.
float DollCheckW() {
  return std::max(ImGui::CalcTextSize(i18n::Tr("Montrer mon équipement")).x,
                  ImGui::CalcTextSize(i18n::Tr("Voir les costumes")).x) + ro::Px(42.0f);
}
// Le plus large des trois contenus du volet mannequin.
float DollPaneW() {
  const float content =
      std::max(std::max(ro::Px(kDollBlockW),
                        PoseComboW() + ro::Px(kPoseLineGap) + GifButtonW()),
               DollCheckW());
  return std::max(ro::Px(kDollWMin),
                  std::ceil(content + ImGui::GetStyle().WindowPadding.x * 2.0f));
}
// Largeur réservée au coût du prochain point, à droite du « + » (3 chiffres suffisent :
// le coût d'une stat plafonne bien avant 999).
float StatCostW() { return ImGui::CalcTextSize("999").x + ro::Px(6.0f); }
// Volet des stats. La rangée la plus contrainte est celle d'une stat PRIMAIRE : sa
// valeur est écrite au fil du texte alors que « Max », « + » et le coût sont collés au
// bord droit — trop étroit, la valeur passe SOUS les boutons. Les gabarits sont donc
// généreux (999 de base, cinq chiffres de bonus) ; au-delà, seul le détail entre
// parenthèses d'une dérivée déborde, et lui passe à la ligne (cf. DrawStatsPanel).
float StatsPaneW() {
  const ImGuiStyle& st = ImGui::GetStyle();
  const float val_x  = ImGui::CalcTextSize(i18n::Tr("Esq.P")).x + ro::Px(kStatValGap);
  const float max_w  = ImGui::CalcTextSize(i18n::Tr("Max")).x + st.FramePadding.x * 2.0f +
                       ro::Px(4.0f);
  const float prim   = val_x + ImGui::CalcTextSize("- 999 (+99999)").x + st.ItemSpacing.x +
                       max_w + ro::Px(4.0f) + ImGui::GetFrameHeight() + StatCostW();
  const float deriv  = val_x + ImGui::CalcTextSize("999999 ~ 999999").x;
  char pts[64];
  std::snprintf(pts, sizeof(pts), i18n::Tr("Points de statut : %d"), 9999);
  const float points = ImGui::CalcTextSize(pts).x;
  const float content = std::max(std::max(prim, deriv), points);
  // + la scrollbar : le volet en a une dès que les bonus d'équipement se déplient,
  // et elle prend sa largeur SUR le contenu.
  return std::max(ro::Px(kStatsWMin),
                  std::ceil(content + st.WindowPadding.x * 2.0f + st.ScrollbarSize));
}
// ── Volet STAFF : largeur et catalogue des classes ───────────────────────────
// Le plancher couvre les deux contrôles qui ne se mesurent pas sur un libellé —
// le menu déroulant des classes et le slider de vitesse, tous deux étirés à la
// largeur du volet. Sous ce plancher, le nom d'une classe ne tiendrait plus.
constexpr float kStaffWMin = 200.0f;
// Bornes du slider `@speed`. 20 et 150 sont celles du SERVEUR (MIN_WALK_SPEED et
// DEFAULT_WALK_SPEED, common/mmo.hpp) : sous 20 le serveur clampe en silence, et
// proposer des crans morts ferait croire à un réglage qui n'arrive pas. Le plafond
// affiché s'arrête à 500 — le serveur accepte 1000, mais on marche déjà au pas à
// 500 et l'autre moitié de la course ne servirait qu'à rendre le curseur imprécis.
constexpr int kSpeedMin     = 20;
constexpr int kSpeedDefault = 150;
constexpr int kSpeedUiMax   = 500;

// Raffinage maximal. 🔴 10 et non 20 : `MAX_REFINE` vaut 20 sous RENEWAL et 10 sinon
// (moonlight, src/map/status.hpp), et ce serveur est PRE-RENEWAL — `RENEWAL` est
// commenté dans src/config/renewal.hpp. Le serveur clampe de toute façon ; borner ici
// évite juste de proposer une course dont la moitié ne fait rien.

// Plafonds de SAISIE des rangées d'ajustement. Ce ne sont pas des règles de jeu — le
// serveur clampe déjà tout (`pc_maxparameter`, `pc_maxbaselv`, `MAX_ZENY`) — mais des
// garde-fous de frappe : ils empêchent un zéro de trop de partir en commande, et
// gardent la saisie dans un ordre de grandeur qui a du sens pour la rangée.
constexpr int kStatStepMax  = 9999;
constexpr int kLevelStepMax = 999;
constexpr int kZenyStepMax  = 1000000000;  // MAX_ZENY vaut INT_MAX ; un milliard suffit

// Les emplacements que `@refine` accepte : un MASQUE EQP_* (moonlight,
// common/mmo.hpp), et 0 pour « toutes les pièces portées » — c'est la valeur que
// `ACMD_FUNC(refine)` traite comme « pas de filtre ».
//
// 🔴 Les libellés restent les TERMES DU JEU en anglais, comme les abréviations des
// cases du mannequin (SlotAbbrev) : c'est la convention du projet, et le staff doit
// pouvoir apparier d'un coup d'œil la ligne du combo et la case du mannequin. Seule
// la première entrée est une phrase, donc la seule à traduire (cf. RefinePosLabel).
struct RefinePos { uint32_t mask; const char* label; };
const RefinePos kRefinePos[] = {
    {    0, "Toutes les pièces"},
    {    2, "Weapon"},
    {   32, "Shield"},
    {   16, "Armor"},
    {    4, "Garment"},
    {   64, "Shoes"},
    {  256, "Head top"},
    {  512, "Head mid"},
    {    1, "Head bot"},
    {    8, "Acc. R"},
    {  128, "Acc. L"},
};
constexpr int kRefinePosCount = static_cast<int>(sizeof(kRefinePos) / sizeof(kRefinePos[0]));
const char* RefinePosLabel(int i) {
  // Seul l'index 0 passe par le catalogue : traduire « Weapon » y ferait entrer les
  // termes du jeu dans la liste des textes manquants, un par emplacement.
  return i == 0 ? i18n::Tr(kRefinePos[0].label) : kRefinePos[i].label;
}

// Les drapeaux du TROISIÈME paramètre de `@option` (l'état « visuel » : OPTION_* de
// rAthena). Les deux premiers paramètres portent opt1/opt2 — les vrais statuts
// (pétrifié, gelé, maudit…) — et la modale les met à 0 : le staff qui pose un état
// visuel veut être propre, pas gelé.
//
// ⚠ Le niveau de CART n'est pas ici : ses cinq valeurs (8, 128, 256, 512, 1024)
// s'excluent, c'est un combo et non une case (cf. kOptionCartBits).
struct OptionFlag { int bit; const char* label; };
const OptionFlag kOptionFlags[] = {
    {   32, "Riding"},     // peco / monture
    {   16, "Falcon"},
    {    1, "Sight"},
    {    2, "Hiding"},
    {    4, "Cloaking"},
    {   64, "Invisible"},
    { 2048, "Orc Head"},
    { 4096, "Wedding"},
    { 8192, "Ruwach"},
    {16384, "Chasewalk"},
};
constexpr int kOptionFlagCount = static_cast<int>(sizeof(kOptionFlags) / sizeof(kOptionFlags[0]));
// Index = niveau de cart (0 = aucun), valeur = bit OPTION_CART*.
const int kOptionCartBits[6] = {0, 8, 128, 256, 512, 1024};

// Les classes que `@job` accepte SUR CE SERVEUR. RECOPIÉE de l'aide que la commande
// publie elle-même (moonlight, conf/atcommands.yml, entrée « jobchange ») : c'est la
// seule liste qui fasse foi, et elle omet déjà les classes FANTÔMES — Knight2 et
// Crusader2 (montures), tenues de mariage/Noël/été/hanbok — que `ACMD_FUNC(jobchange)`
// refuse nommément. Les proposer n'offrirait qu'un refus.
//
// 🔴 Le LIBELLÉ affiché vient du CLIENT (table Lua des classes, déjà dans sa langue,
// cf. JobName). Celui d'ici n'est qu'un repli pour les ids que le Lua ne connaît pas —
// les classes récentes, absentes des Lua d'un client 2016.
//
// 🔴 `group` est un libellé de table STATIQUE : il se traduit à la LECTURE (i18n::Tr
// dans le menu), jamais ici — un Tr posé sur un initialiseur statique serait évalué
// avant le chargement du catalogue et resterait français pour toujours.
struct JobEntry { int id; const char* group; const char* fallback; };
const JobEntry kJobList[] = {
    {   0, "Novice / 1re classe", "Novice"},
    {   1, "Novice / 1re classe", "Swordman"},
    {   2, "Novice / 1re classe", "Magician"},
    {   3, "Novice / 1re classe", "Archer"},
    {   4, "Novice / 1re classe", "Acolyte"},
    {   5, "Novice / 1re classe", "Merchant"},
    {   6, "Novice / 1re classe", "Thief"},
    {   7, "2e classe", "Knight"},
    {   8, "2e classe", "Priest"},
    {   9, "2e classe", "Wizard"},
    {  10, "2e classe", "Blacksmith"},
    {  11, "2e classe", "Hunter"},
    {  12, "2e classe", "Assassin"},
    {  14, "2e classe", "Crusader"},
    {  15, "2e classe", "Monk"},
    {  16, "2e classe", "Sage"},
    {  17, "2e classe", "Rogue"},
    {  18, "2e classe", "Alchemist"},
    {  19, "2e classe", "Bard"},
    {  20, "2e classe", "Dancer"},
    {4001, "Trans. 1re classe", "Novice High"},
    {4002, "Trans. 1re classe", "Swordman High"},
    {4003, "Trans. 1re classe", "Magician High"},
    {4004, "Trans. 1re classe", "Archer High"},
    {4005, "Trans. 1re classe", "Acolyte High"},
    {4006, "Trans. 1re classe", "Merchant High"},
    {4007, "Trans. 1re classe", "Thief High"},
    {4008, "Trans. 2e classe", "Lord Knight"},
    {4009, "Trans. 2e classe", "High Priest"},
    {4010, "Trans. 2e classe", "High Wizard"},
    {4011, "Trans. 2e classe", "Whitesmith"},
    {4012, "Trans. 2e classe", "Sniper"},
    {4013, "Trans. 2e classe", "Assassin Cross"},
    {4015, "Trans. 2e classe", "Paladin"},
    {4016, "Trans. 2e classe", "Champion"},
    {4017, "Trans. 2e classe", "Professor"},
    {4018, "Trans. 2e classe", "Stalker"},
    {4019, "Trans. 2e classe", "Creator"},
    {4020, "Trans. 2e classe", "Clown"},
    {4021, "Trans. 2e classe", "Gypsy"},
    {4054, "3e classe", "Rune Knight"},
    {4055, "3e classe", "Warlock"},
    {4056, "3e classe", "Ranger"},
    {4057, "3e classe", "Arch Bishop"},
    {4058, "3e classe", "Mechanic"},
    {4059, "3e classe", "Guillotine Cross"},
    {4066, "3e classe", "Royal Guard"},
    {4067, "3e classe", "Sorcerer"},
    {4068, "3e classe", "Minstrel"},
    {4069, "3e classe", "Wanderer"},
    {4070, "3e classe", "Sura"},
    {4071, "3e classe", "Genetic"},
    {4072, "3e classe", "Shadow Chaser"},
    {4060, "3e classe (trans.)", "Rune Knight T"},
    {4061, "3e classe (trans.)", "Warlock T"},
    {4062, "3e classe (trans.)", "Ranger T"},
    {4063, "3e classe (trans.)", "Arch Bishop T"},
    {4064, "3e classe (trans.)", "Mechanic T"},
    {4065, "3e classe (trans.)", "Guillotine Cross T"},
    {4073, "3e classe (trans.)", "Royal Guard T"},
    {4074, "3e classe (trans.)", "Sorcerer T"},
    {4075, "3e classe (trans.)", "Minstrel T"},
    {4076, "3e classe (trans.)", "Wanderer T"},
    {4077, "3e classe (trans.)", "Sura T"},
    {4078, "3e classe (trans.)", "Genetic T"},
    {4079, "3e classe (trans.)", "Shadow Chaser T"},
    {4252, "4e classe", "Dragon Knight"},
    {4253, "4e classe", "Meister"},
    {4254, "4e classe", "Shadow Cross"},
    {4255, "4e classe", "Arch Mage"},
    {4256, "4e classe", "Cardinal"},
    {4257, "4e classe", "Windhawk"},
    {4258, "4e classe", "Imperial Guard"},
    {4259, "4e classe", "Biolo"},
    {4260, "4e classe", "Abyss Chaser"},
    {4261, "4e classe", "Elemental Master"},
    {4262, "4e classe", "Inquisitor"},
    {4263, "4e classe", "Troubadour"},
    {4264, "4e classe", "Trouvere"},
    {  23, "Classes étendues", "Super Novice"},
    {  24, "Classes étendues", "Gunslinger"},
    {  25, "Classes étendues", "Ninja"},
    {4045, "Classes étendues", "Super Baby"},
    {4046, "Classes étendues", "Taekwon"},
    {4047, "Classes étendues", "Star Gladiator"},
    {4049, "Classes étendues", "Soul Linker"},
    {4190, "Classes étendues", "Ex. Super Novice"},
    {4191, "Classes étendues", "Ex. Super Baby"},
    {4211, "Classes étendues", "Kagerou"},
    {4212, "Classes étendues", "Oboro"},
    {4215, "Classes étendues", "Rebellion"},
    {4218, "Classes étendues", "Summoner"},
    {4239, "Classes étendues", "Star Emperor"},
    {4240, "Classes étendues", "Soul Reaper"},
    {4302, "Classes étendues", "Sky Emperor"},
    {4303, "Classes étendues", "Soul Ascetic"},
    {4304, "Classes étendues", "Shinkiro"},
    {4305, "Classes étendues", "Shiranui"},
    {4306, "Classes étendues", "Night Watch"},
    {4307, "Classes étendues", "Hyper Novice"},
    {4308, "Classes étendues", "Spirit Handler"},
    {4023, "Baby / 1re classe", "Baby Novice"},
    {4024, "Baby / 1re classe", "Baby Swordman"},
    {4025, "Baby / 1re classe", "Baby Magician"},
    {4026, "Baby / 1re classe", "Baby Archer"},
    {4027, "Baby / 1re classe", "Baby Acolyte"},
    {4028, "Baby / 1re classe", "Baby Merchant"},
    {4029, "Baby / 1re classe", "Baby Thief"},
    {4030, "Baby 2e classe", "Baby Knight"},
    {4031, "Baby 2e classe", "Baby Priest"},
    {4032, "Baby 2e classe", "Baby Wizard"},
    {4033, "Baby 2e classe", "Baby Blacksmith"},
    {4034, "Baby 2e classe", "Baby Hunter"},
    {4035, "Baby 2e classe", "Baby Assassin"},
    {4037, "Baby 2e classe", "Baby Crusader"},
    {4038, "Baby 2e classe", "Baby Monk"},
    {4039, "Baby 2e classe", "Baby Sage"},
    {4040, "Baby 2e classe", "Baby Rogue"},
    {4041, "Baby 2e classe", "Baby Alchemist"},
    {4042, "Baby 2e classe", "Baby Bard"},
    {4043, "Baby 2e classe", "Baby Dancer"},
    {4096, "Baby 3e classe", "Baby Rune Knight"},
    {4097, "Baby 3e classe", "Baby Warlock"},
    {4098, "Baby 3e classe", "Baby Ranger"},
    {4099, "Baby 3e classe", "Baby Arch Bishop"},
    {4100, "Baby 3e classe", "Baby Mechanic"},
    {4101, "Baby 3e classe", "Baby Guillotine Cross"},
    {4102, "Baby 3e classe", "Baby Royal Guard"},
    {4103, "Baby 3e classe", "Baby Sorcerer"},
    {4104, "Baby 3e classe", "Baby Minstrel"},
    {4105, "Baby 3e classe", "Baby Wanderer"},
    {4106, "Baby 3e classe", "Baby Sura"},
    {4107, "Baby 3e classe", "Baby Genetic"},
    {4108, "Baby 3e classe", "Baby Shadow Chaser"},
    {4220, "Baby étendues", "Baby Summoner"},
    {4222, "Baby étendues", "Baby Ninja"},
    {4223, "Baby étendues", "Baby Kagerou"},
    {4224, "Baby étendues", "Baby Oboro"},
    {4225, "Baby étendues", "Baby Taekwon"},
    {4226, "Baby étendues", "Baby Star Gladiator"},
    {4227, "Baby étendues", "Baby Soul Linker"},
    {4228, "Baby étendues", "Baby Gunslinger"},
    {4229, "Baby étendues", "Baby Rebellion"},
    {4241, "Baby étendues", "Baby Star Emperor"},
    {4242, "Baby étendues", "Baby Soul Reaper"},
};
constexpr int kJobListCount = static_cast<int>(sizeof(kJobList) / sizeof(kJobList[0]));

// Libellé d'une entrée de la table : le nom que le CLIENT donne à cette classe, ou
// le repli de la table quand il n'en a pas. Résolu UNE fois — `rag::JobName` traverse
// le Lua, et le menu déroulant en redemanderait 160 par frame tant qu'il est ouvert.
const char* JobListLabel(int idx) {
  static std::vector<std::string> cache;
  if (cache.empty()) {
    cache.reserve(kJobListCount);
    for (int i = 0; i < kJobListCount; ++i) {
      const char* n = rag::JobName(kJobList[i].id);
      cache.emplace_back((n && n[0]) ? n : kJobList[i].fallback);
    }
  }
  return cache[static_cast<size_t>(idx)].c_str();
}

// Largeur du volet staff : la plus large de ses rangées. La rangée de stat
// (« - » / saisie / « + ») et celle des niveaux (libellé devant) sont les deux
// candidates, le plancher couvrant le reste.
float StaffPaneW() {
  const ImGuiStyle& st = ImGui::GetStyle();
  const float btn   = ImGui::GetFrameHeight();          // les carrés « - » et « + »
  // Le champ est mesuré sur le PLUS GRAND montant saisissable — le zeny — et non sur
  // celui des stats : une largeur par rangée ferait respirer la colonne, et un gabarit
  // à cinq chiffres coupait la saisie d'un million de zeny en deux. La rangée reste
  // très en dessous du plancher du volet : la fenêtre, elle, ne bouge pas.
  const float input = ImGui::CalcTextSize("999999999").x + st.FramePadding.x * 2.0f;
  const float stat_row = btn * 2.0f + input + st.ItemSpacing.x * 2.0f;
  // Colonne des libellés des rangées d'ajustement NOMMÉES (Base / Job / Zeny) : la
  // même mesure que dans DrawStaffPanel, sur les mêmes trois libellés.
  const float lvl_lbl  = std::max(std::max(ImGui::CalcTextSize(i18n::Tr("Base")).x,
                                           ImGui::CalcTextSize(i18n::Tr("Job")).x),
                                  ImGui::CalcTextSize("Zeny").x);
  const float lvl_row  = lvl_lbl + st.ItemSpacing.x + stat_row;
  const float acts = ro::MaxButtonWidth({i18n::Tr("Reset"), i18n::Tr("Max")}) * 2.0f +
                     st.ItemSpacing.x;
  // Les boutons pleine largeur : ils s'étirent, mais la fenêtre doit d'abord leur
  // donner de quoi écrire leur libellé — sinon RoButton le rétrécit puis le coupe.
  const float full = ro::MaxButtonWidth({i18n::Tr("Vitesse normale"), i18n::Tr("Réparer tout"),
                                         i18n::Tr("Vider l'inventaire"), i18n::Tr("Soigner"),
                                         i18n::Tr("État visuel"), i18n::Tr("Invisible")});
  const float content = std::max(std::max(std::max(stat_row, lvl_row), acts), full);
  return std::max(ro::Px(kStaffWMin),
                  std::ceil(content + st.WindowPadding.x * 2.0f + st.ScrollbarSize));
}

// Trois largeurs de fenêtre possibles, et non plus deux : mannequin seul, +stats,
// +staff. `xwide` n'existe que pour le staff — pour tout le monde elle vaut `wide`,
// et le repli redevient la bascule à deux crans d'avant.
struct WinSnap {
  float narrow = 0.0f, wide = 0.0f, xwide = 0.0f;
  bool  valid = false;
  bool  force_wide = false;  // onglets pleine largeur (Guilde) : pas de repli étroit
};
WinSnap g_win_snap;
void SnapCharSheetWidth(ImGuiSizeCallbackData* d) {
  if (!g_win_snap.valid) return;
  if (g_win_snap.force_wide) { d->DesiredSize.x = g_win_snap.wide; return; }
  // Cran le plus proche de la largeur demandée. Les crans sont croissants et
  // `xwide == wide` quand il n'y a pas de volet staff : le test s'annule alors de
  // lui-même et la bascule redevient narrow/wide.
  const float w = d->DesiredSize.x;
  float best = g_win_snap.narrow;
  if (std::abs(w - g_win_snap.wide)  < std::abs(w - best)) best = g_win_snap.wide;
  if (std::abs(w - g_win_snap.xwide) < std::abs(w - best)) best = g_win_snap.xwide;
  d->DesiredSize.x = best;
}

// ── Raccourcis clavier de preset ─────────────────────────────────────────────
// Conversions VK <-> ImGuiKey, capture, libellé et contrôle de conflit vivent
// dans features/hotkey_util.h : ils sont partagés avec la touche de saut, qui est
// elle aussi remappable — c'est ce qui permet aux deux de se refuser mutuellement
// un combo déjà pris.

// Mini-icone d'un item de preset (icone + refine, survol = nom). Affichage seul.
void DrawPresetItemIcon(const EquipPresetItem& pi, float sz) {
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(sz, sz));  // reserve la place + hit-test pour le survol
  const bool hov = ImGui::IsItemHovered();
  const ImVec2 p1(p0.x + sz, p0.y + sz);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Arrondis, épaisseurs de cadre et marge de l'icône : tout suit la case, qui
  // suit l'échelle. Une icône collée à 2 px de bord dans une case deux fois plus
  // grande laisserait un cadre vide tout autour d'elle.
  const float round = ro::Px(3.0f);
  const float inset = ro::Px(2.0f);
  dl->AddRectFilled(p0, p1, SlotBgCol(), round);
  // Le cadre dit la FAMILLE : une rangée mélange équipement, costumes et munition, et
  // l'icône seule ne les distingue pas — un chapeau et son costume ont la même image.
  switch (pi.kind) {
    case PresetKind::kCostume:
      dl->AddRect(p0, p1, IM_COL32(150, 90, 190, 220), round, 0, inset); break;
    case PresetKind::kAmmo:
      dl->AddRect(p0, p1, IM_COL32(70, 130, 70, 220), round, 0, inset); break;
    default:
      dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 80), round); break;
  }
  ro::IconTex ic = ro::ItemIcon(pi.nameid);
  if (ic.tex)
    dl->AddImage(reinterpret_cast<ImTextureID>(ic.tex),
                 ImVec2(p0.x + inset, p0.y + inset),
                 ImVec2(p1.x - inset, p1.y - inset));
  if (pi.refine > 0) {  // "+N" bas-droite, noir cerne blanc
    char rf[8];
    std::snprintf(rf, sizeof(rf), "+%d", pi.refine);
    const ImVec2 ts = ImGui::CalcTextSize(rf);
    const ImVec2 rp(p1.x - ts.x - inset, p1.y - ts.y - ro::Px(1.0f));
    ro::AddTextHalo(dl, rp, IM_COL32_BLACK, rf,
                    IM_COL32_WHITE);
  }
  if (hov) {
    // La famille sur sa PROPRE ligne, sans espace de bord dans la clé de traduction :
    // une espace en tête ou en queue se perd au premier aller-retour de catalogue, et
    // le texte se décale d'un cran sans que rien ne le signale.
    char nm[192];
    if (pi.refine > 0)
      std::snprintf(nm, sizeof(nm), "+%d %s", pi.refine, itemcell::NameById(pi.nameid));
    else
      std::snprintf(nm, sizeof(nm), "%s", itemcell::NameById(pi.nameid));
    const char* family = (pi.kind == PresetKind::kCostume) ? i18n::Tr("costume")
                       : (pi.kind == PresetKind::kAmmo)    ? i18n::Tr("munition")
                                                           : nullptr;
    if (family) ImGui::SetTooltip("%s\n(%s)", nm, family);
    else        ImGui::SetTooltip("%s", nm);
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
  // Part du refine dans l'ATK / la DEF
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

// Résolveur de nom de skill localisé (wrapper Lua natif, cf. skill_bar) :
// char* GetSkillName(int id) — renvoie « Unknown-Skill » si l'id est inconnu.

// Résolution du nom de STATUT (EFST) via le global Lua GetStateIconDescript(efst),
// appelé par l'API C Lua 5.1 BRUTE (nom statique => pas de std::string BYVAL détruit
// par le wrapper varargs, cf. item_desc_window ResolveOptName). RE : le tooltip natif
// FUN_00c93cb0 utilise ce même global via Lua_CallGlobal_va.
using LuaSetTop_t   = void        (__cdecl*)(void*, int);

// ── Grimoire : noms des onglets ──────────────────────────────────────────────
// Lua JobSkillTab_GetTabName(classe) rend QUATRE chaînes — « 1st » / « 2nd »… pour la
// plupart des classes, « Ninja », « Gunslinger », « Summoner »… pour celles qui ont
// leur propre libellé (skilltreeview.lub, appels ChangeSkillTabName). C'est la source
// du natif (sub_9765F0) ; l'appelant traduit ce qui reste générique.
void ReadSkillTabNamesSEH(int jobId, char out[4][32]) {
  for (int i = 0; i < 4; ++i) out[i][0] = '\0';
  __try {
    void* L = lua::State();
    if (!L) return;
    lua::GetField(L, lua::kGlobalsIndex, "JobSkillTab_GetTabName");
    lua::PushNumber(L, static_cast<double>(jobId));
    if (lua::PCall(L, 1, 4, 0) == 0) {
      for (int i = 0; i < 4; ++i) {
        const char* s = lua::ToLString(L, -4 + i, nullptr);
        if (s && s[0]) { std::strncpy(out[i], s, 31); out[i][31] = '\0'; }
      }
      lua::SetTop(L, -5);  // dépile les 4 résultats
    } else {
      lua::SetTop(L, -2);  // dépile le message d'erreur
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Job du personnage (le même getter que ClassNameSEH), clé de la table des libellés.
int OwnJobIdSEH() { return rag::OwnDisplayedJobId(); }

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
using LuaExecFile_t = char(__thiscall*)(void*, const char*, char, char);
constexpr char kGuildTreeLuaFile[] = "Lua Files\\SkillInfoz\\skilltreeguild";

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
    void* L = lua::State();
    if (!L) return kTreeNoLua;
    // ⚠ kExecFileAddr est __thiscall sur le GESTIONNAIRE Lua (lua::Manager()),
    // pas sur le lua_State : les deux ne sont pas interchangeables.
    if (!reinterpret_cast<LuaExecFile_t>(lua::kExecFileAddr)(
            lua::Manager(), kGuildTreeLuaFile, 1, 0))
      return kTreeNoFile;
    lua::GetField(L, lua::kGlobalsIndex, "GdDump");
    if (!lua::ToBoolean(L, -1)) {  // nil = pas de fonction
      lua::SetTop(L, -2);
      return kTreeNoDumper;
    }
    if (lua::PCall(L, 0, 1, 0) != 0) {
      const char* msg = lua::ToLString(L, -1, nullptr);
      if (msg && msg[0]) std::strncpy(err, msg, err_cap - 1);
      lua::SetTop(L, -2);
      return kTreeCallFailed;
    }
    const char* s = lua::ToLString(L, -1, nullptr);
    if (s && s[0]) std::strncpy(out, s, cap - 1);
    lua::SetTop(L, -2);  // dépile le résultat
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
    void* L = lua::State();
    if (L) {
      lua::GetField(L, lua::kGlobalsIndex, "GetStateIconDescript");
      lua::PushNumber(L, static_cast<double>(efst));
      if (lua::PCall(L, 1, 1, 0) == 0) {
        const char* s = lua::ToLString(L, -1, nullptr);
        if (s && s[0]) std::strncpy(raw, s, sizeof(raw) - 1);
      }
      lua::SetTop(L, -2);  // pop résultat/erreur
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
  if (!clean[0]) std::snprintf(clean, sizeof(clean), i18n::Tr("Statut #%u"), efst);
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
  kBscIgnDefClass = 31, kBscIgnMdefClass = 32,  // bonus1 bitmask replié en 100 % par le serveur
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

// Fil RÉSEAU : on copie, rien de plus (cf. features/net_inbox.h). Les paquets de
// guilde sont STANDARD et à longueur variable : leur décodage relit la longueur
// annoncée et parcourt les entrées bien au-delà des octets transmis, d'où
// PushAnnounced. Les customs (compagnons, bonus) tiennent dans `len`.
void CharacterSheet::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  switch (opcode) {
    case kOpGuildEmblemImg:
    case kOpGuildSkills:
    case kOpGuildBanList:
    case kOpPositionNames:
    case kOpPositionInfo:
    case kOpPositionChanged:
      net_inbox_.PushAnnounced(opcode, data, len);
      break;
    default:
      net_inbox_.Push(opcode, data, len);
      break;
  }
}

// Fil PRINCIPAL : le décodage, rejoué à chaque frame, dans l'ordre d'arrivée.
void CharacterSheet::HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) {

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
  p.cid  = static_cast<uint32_t>(ReadInt(rag::kOwnCharIdAddr));
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
    pi.left_hand = (s == 5) && (li.loc & rag::equip::kEqpWeapon) != 0;
    p.items.push_back(pi);
  }
  // ── Costumes ───────────────────────────────────────────────────────────────
  // Tableau session DISTINCT, et seules quatre positions y existent. On les capture
  // TOUJOURS, meme si `with_costumes` est faux : l'information ne coute rien a garder,
  // et cocher la case plus tard doit suffire — sans quoi il faudrait re-enregistrer le
  // preset en portant la bonne apparence, ce que personne ne devinerait.
  for (int s : kCostumeSlots) {
    InvItemLite li{};
    if (!ReadEquipLite(s, &li, /*costume=*/true)) continue;
    EquipPresetItem pi{};
    pi.nameid = li.nameid;
    pi.refine = li.refine;
    pi.grade  = li.grade;
    for (int c = 0; c < 4; ++c) pi.cards[c] = li.cards[c];
    pi.kind = PresetKind::kCostume;
    p.items.push_back(pi);
  }
  // ── Munition ───────────────────────────────────────────────────────────────
  // Ni slot d'equip ni costume : un item d'inventaire ordinaire designe par un global.
  // Elle n'a ni refine ni cartes — seul le nameid la caracterise.
  {
    AmmoItem am{};
    if (ReadEquippedAmmo(&am) && am.nameid != 0) {
      EquipPresetItem pi{};
      pi.nameid = am.nameid;
      pi.kind   = PresetKind::kAmmo;
      p.items.push_back(pi);
    }
  }
  // Ecrase un preset existant de MEME nom pour ce perso, sinon ajoute — en gardant SON
  // reglage de costumes et son raccourci : re-enregistrer une tenue ne doit pas defaire
  // ce que le joueur a coche a cote.
  bool replaced = false;
  for (auto& ex : equip_presets_)
    if (ex.cid == p.cid && ex.name == p.name) {
      p.with_costumes = ex.with_costumes;
      p.hotkey_vk     = ex.hotkey_vk;
      p.hotkey_ctrl   = ex.hotkey_ctrl;
      p.hotkey_alt    = ex.hotkey_alt;
      p.hotkey_shift  = ex.hotkey_shift;
      ex = std::move(p);
      replaced = true;
      break;
    }
  if (!replaced) {
    int cnt = 0;  // plafond par perso (kMaxPresetsPerChar)
    for (const auto& ex : equip_presets_) if (ex.cid == p.cid) ++cnt;
    if (cnt >= kMaxPresetsPerChar) { preset_status_ = i18n::Tr("Limite de 5 presets atteinte"); return; }
    equip_presets_.push_back(std::move(p));
  }
  preset_status_ = i18n::Tr("Preset enregistré");
  if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
}

void CharacterSheet::ApplyPreset(const EquipPreset& p) {
  InvItemLite inv[512];
  int n = ReadInventoryLite(inv, 512);
  // Ajoute une piece PORTEE aux candidats de resolution : la liste inventaire ne restitue
  // pas toujours les items equipes (nameid vide), donc un item commun deja porte serait
  // declare « manquant » a tort. La copie equip fait foi, elle porte l'identite complete.
  auto add_candidate = [&](const InvItemLite& li) {
    for (int i = 0; i < n; ++i)
      if (inv[i].index == li.index) { inv[i] = li; return; }
    if (n < 512) inv[n++] = li;
  };

  // 1) Ce qui est porté, par famille. Les trois familles ne se déséquipent pas aux mêmes
  //    conditions, d'où trois listes plutôt qu'une : l'équipement suit le preset, le
  //    costume n'est touché que si le preset le pilote, la munition ne se retire jamais.
  int equipped[kNormalSlots]; int ne = 0;
  for (int s = 0; s < kNormalSlots; ++s) {
    InvItemLite li{};
    if (!ReadEquipLite(s, &li)) continue;
    equipped[ne++] = li.index;
    add_candidate(li);
  }
  int worn_costume[4]; int nwc = 0;
  for (int s : kCostumeSlots) {
    InvItemLite li{};
    if (!ReadEquipLite(s, &li, /*costume=*/true)) continue;
    worn_costume[nwc++] = li.index;
    add_candidate(li);
  }
  int worn_ammo = 0;
  {
    AmmoItem am{};
    if (ReadEquippedAmmo(&am)) worn_ammo = am.invIndex;
  }

  // 2) Resoudre chaque item du preset -> index cible + position EQP a envoyer (non deja porte).
  int targets[48]; int nt = 0;          // pieces d'EQUIPEMENT a garder sur soi
  int costume_targets[8]; int nct = 0;  // idem, costumes
  struct ToEquip { int index; uint32_t pos; } toEquip[48]; int neq = 0;
  int missing = 0;
  for (const EquipPresetItem& pi : p.items) {
    // Un preset qui ne pilote pas les costumes ignore les siens de bout en bout : ni
    // rééquipés, ni comptés manquants. Sans ce filtre, une apparence rangée dans le
    // storage ferait apparaître « 2 items manquants » sur un preset de combat.
    if (pi.kind == PresetKind::kCostume && !p.with_costumes) continue;

    const int idx = ResolvePresetItem(pi, inv, n);
    if (idx < 0) { ++missing; continue; }
    if (pi.kind == PresetKind::kCostume) {
      if (nct < 8) costume_targets[nct++] = idx;
    } else if (pi.kind == PresetKind::kEquip) {
      if (nt < 48) targets[nt++] = idx;
    }
    // Déjà en place ? Rien à envoyer. La munition a son propre porteur (un global), pas
    // un slot du tableau equip.
    bool eq = (pi.kind == PresetKind::kAmmo) ? (worn_ammo == idx) : false;
    if (pi.kind == PresetKind::kEquip)
      for (int k = 0; k < ne; ++k) if (equipped[k] == idx) eq = true;
    if (pi.kind == PresetKind::kCostume)
      for (int k = 0; k < nwc; ++k) if (worn_costume[k] == idx) eq = true;

    if (!eq && neq < 48) {
      InvItemLite li{};
      if (FindInvLiteByIndex(inv, n, idx, &li)) {
        // Position = masque EQP de l'item (info+8, comme le natif) ; forcee a la main GAUCHE
        // pour une arme dual-wield (sinon le serveur pre-renewal la remettrait a droite).
        // Les costumes et la munition portent DEJA leur position dans ce masque (bits
        // EQP_COSTUME_*, 0x8000) : le serveur route dessus, rien de special a faire.
        const uint32_t pos = pi.left_hand ? rag::equip::kEqpShield : static_cast<uint32_t>(li.loc);
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
  // Les costumes ne bougent QUE si le preset les pilote — sinon on n'y touche pas du tout.
  if (p.with_costumes) {
    for (int k = 0; k < nwc; ++k) {
      bool keep = false;
      for (int t = 0; t < nct; ++t) if (costume_targets[t] == worn_costume[k]) keep = true;
      if (!keep) SendUnequip(worn_costume[k]);
    }
  }
  // 🔴 La munition ne se RETIRE jamais, elle se remplace. Un consommable de combat n'a rien
  // d'une piece d'apparence ou de stats : la retirer parce que le preset n'en portait pas au
  // moment de l'enregistrement ne rend service a personne, et l'archer ne s'en apercevrait
  // qu'au premier tir. Tous les presets d'avant sont dans ce cas.
  for (int e = 0; e < neq; ++e) SendEquip(toEquip[e].index, toEquip[e].pos);
  preset_status_ = (missing > 0)
                       ? i18n::Tr("Appliqué (") + std::to_string(missing) + " item(s) manquant(s))"
                       : i18n::Tr("Preset appliqué");
}

// « Tout nu » : desequipe tous les slots portes, en paquets bruts envoyes d'un coup (meme
// chemin que le desequip de masse d'ApplyPreset -> instantane cote serveur). Couvre l'equip
// normal ET les costumes (tableau session separe rag::equip::kOwnCostumeBase : seuls 3 tetes + cape existent).
int CharacterSheet::UnequipAll(bool with_costumes) {
  int freed = 0;
  auto strip = [&](int slot, bool costume) {
    InvItemLite li{};
    if (!ReadEquipLite(slot, &li, costume)) return;  // slot vide
    SendUnequip(li.index);
    ++freed;
  };
  for (int s = 0; s < kNormalSlots; ++s) strip(s, false);
  if (with_costumes)
    for (int s : kCostumeSlots) strip(s, true);
  preset_status_ = freed > 0 ? i18n::Tr("Tout déséquipé (") + std::to_string(freed) + i18n::Tr(" pièce(s))")
                             : i18n::Tr("Rien à déséquiper");
  return freed;
}

void CharacterSheet::ProcessPresetHotkeys() {
  // Une capture de combo est en cours (ici ou ailleurs : touche de saut) : la
  // touche pressée sert à remapper, elle ne doit rien déclencher.
  if (hk_capturing_ >= 0 || hotkeys::CaptureInProgress()) return;
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput) return;    // saisie de texte (chat…) : ne pas déclencher
  const uint32_t cid = static_cast<uint32_t>(ReadInt(rag::kOwnCharIdAddr));
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
  const uint32_t cid = static_cast<uint32_t>(ReadInt(rag::kOwnCharIdAddr));
  std::vector<int> mine;  // indices (dans equip_presets_) des presets du perso courant
  for (int i = 0; i < static_cast<int>(equip_presets_.size()); ++i)
    if (equip_presets_[i].cid == cid) mine.push_back(i);

  // Largeur des boutons : ro::ButtonWidth et PAS la formule d'ImGui (FramePadding×2),
  // qui vaut ~6 px de moins que ce dont le bouton RO a besoin (ses deux caps + sa
  // marge). Les libellés espagnols, plus longs, débordaient donc de leur art — ici
  // rien ne contraint la largeur, les boutons n'ont qu'à grandir.
  const float load_w = ro::ButtonWidth(i18n::Tr("Charger"));
  const float del_w = ro::ButtonWidth(i18n::Tr("Suppr"));
  // Cases d'items d'un preset, à l'échelle de l'interface : elles encadrent des
  // icônes d'art, pas du texte.
  const float icon = ro::Px(30.0f), igap = ro::Px(3.0f);

  // Action globale : se mettre « tout nu » (independant des presets). Les costumes vivent dans
  // un tableau session distinct -> bouton separe pour tout retirer, costumes compris.
  if (ro::RoButton(i18n::Tr("Tout déséquiper"),
                   ro::ButtonWidth(i18n::Tr("Tout déséquiper"))))
    UnequipAll(false);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", i18n::Tr("Retire l'équipement porté (garde les costumes)"));
  ImGui::SameLine(0.0f, 4.0f);
  if (ro::RoButton(i18n::Tr("+ costumes"), ro::ButtonWidth(i18n::Tr("+ costumes"))))
    UnequipAll(true);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", i18n::Tr("Retire aussi les costumes (têtes + cape)"));
  // Style du personnage : c'est ici que le joueur regarde son apparence, donc
  // ici que le raccourci a du sens. Alt+P reste, mais un raccourci qu'on ne
  // découvre nulle part n'existe pas.
  ImGui::SameLine(0.0f, 4.0f);
  if (ro::RoButton(i18n::Tr("Mon style"),
                   ro::ButtonWidth(i18n::Tr("Mon style")))) {
    if (PaletteEditor* editeur = Bourgeon::Instance().palette_editor())
      editeur->Toggle();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", i18n::Tr("Couleurs du corps, coiffure et couleur de "
                                     "cheveux (Alt+P)"));
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  if (mine.empty()) {
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Aucun preset enregistré pour ce personnage."));
    ImGui::Spacing();
  }

  // On applique/supprime APRÈS le rendu (ne pas invalider equip_presets_ en cours d'itération).
  int to_load = -1, to_delete = -1;
  for (int mi = 0; mi < static_cast<int>(mine.size()); ++mi) {
    const EquipPreset& ep = equip_presets_[mine[mi]];
    ImGui::PushID(mi);
    // Ligne titre : nom (gauche) + Charger/Suppr (droite).
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ro::pal::kBlack, "%s", ep.name.c_str());
    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, avail - load_w - del_w - 6.0f));  // boutons à droite
    if (ro::RoButton(i18n::Tr("Charger"), load_w)) to_load = mine[mi];
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", i18n::Tr("Rééquipe exactement ce jeu (déséquipe le reste)"));
    ImGui::SameLine(0.0f, 4.0f);
    if (ro::RoButton(i18n::Tr("Suppr"), del_w)) to_delete = mine[mi];
    // Rangée d'icônes des items (wrap selon la largeur disponible).
    if (ep.items.empty()) {
      ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("(vide)"));
    } else {
      const float availw = ImGui::GetContentRegionAvail().x;
      const int perRow = std::max(1, static_cast<int>((availw + igap) / (icon + igap)));
      for (int k = 0; k < static_cast<int>(ep.items.size()); ++k) {
        if (k % perRow != 0) ImGui::SameLine(0.0f, igap);
        DrawPresetItemIcon(ep.items[k], icon);
      }
    }
    // Les costumes ne sont pilotés que sur demande : un preset de combat ne doit pas
    // déshabiller l'apparence. Décoché, ceux du preset sont ignorés de bout en bout —
    // ni rééquipés, ni comptés manquants — et ceux qu'on porte restent en place.
    {
      bool wc = ep.with_costumes;
      if (ro::RoCheckbox(i18n::Tr("Piloter aussi les costumes"), &wc)) {
        equip_presets_[mine[mi]].with_costumes = wc;
        if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", i18n::Tr(
                  "Coché, le preset rétablit exactement l'apparence enregistrée : il remet ses "
                  "costumes et retire ceux qui n'en font pas partie.\n\n"
                  "Décoché (défaut), il ne touche pas aux costumes portés.\n\n"
                  "Les costumes portés au moment de l'enregistrement sont gardés dans le preset "
                  "quoi qu'il arrive : cocher la case plus tard suffit, sans le ré-enregistrer."));
    }

    // Ligne raccourci clavier : libellé + Définir/Effacer, ou mode capture.
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Raccourci :"));
    ImGui::SameLine();
    if (hk_capturing_ == mine[mi]) {
      hotkeys::PingCapture();  // gèle les raccourcis (saut compris) le temps du choix
      ImGui::TextColored(ro::pal::kBlack, "%s", i18n::Tr("appuie sur une touche…  (Échap : annuler)"));
      ImGuiIO& io = ImGui::GetIO();
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        hk_capturing_ = -1;
        hk_conflict_msg_.clear();
      } else if (const int vk = hotkeys::CaptureMainVk()) {
        const bool c = io.KeyCtrl, a = io.KeyAlt, sh = io.KeyShift;
        char what[64];
        if (hotkeys::Conflict(vk, c, a, sh, hotkeys::Owner::kEquipPreset, mine[mi], what,
                              sizeof(what))) {
          hk_conflict_msg_ = std::string(i18n::Tr("Déjà utilisé par ")) + what + i18n::Tr(" — choisis un autre combo");
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
      ImGui::TextColored(ro::pal::kBlack, "%s", hkl);
      ImGui::SameLine(0.0f, 6.0f);
      if (ro::RoButton(i18n::Tr("Définir"), ro::ButtonWidth(i18n::Tr("Définir")))) {
        hk_capturing_ = mine[mi];
        hk_conflict_msg_.clear();
      }
      if (ep.hotkey_vk != 0) {
        ImGui::SameLine(0.0f, 4.0f);
        if (ro::RoButton(i18n::Tr("Effacer"), ro::ButtonWidth(i18n::Tr("Effacer")))) {
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
  ImGui::TextColored(ro::pal::kBlack, i18n::Tr("Enregistrer l'équipement porté (%d/%d)"),
                     static_cast<int>(mine.size()), kMaxPresetsPerChar);
  const float save_w = ro::ButtonWidth(i18n::Tr("Sauver l'actuel"));
  ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - save_w - 8.0f));
  ImGui::InputTextWithHint("##cs_pname", i18n::Tr("nom du preset"), preset_name_buf_,
                           sizeof(preset_name_buf_));
  ImGui::SameLine(0.0f, 4.0f);
  // Cap à 5 : on autorise quand même l'ÉCRASEMENT d'un preset existant de même nom.
  bool name_exists = false;
  for (int idx : mine) if (equip_presets_[idx].name == preset_name_buf_) name_exists = true;
  const bool at_cap = static_cast<int>(mine.size()) >= kMaxPresetsPerChar && !name_exists;
  const bool can_save = preset_name_buf_[0] != '\0' && !at_cap;
  if (!can_save) ImGui::BeginDisabled();
  if (ro::RoButton(i18n::Tr("Sauver l'actuel"), save_w)) {
    SaveCurrentEquipAsPreset(preset_name_buf_);
    preset_name_buf_[0] = '\0';
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip(at_cap
                          ? i18n::Tr("Limite de 5 presets atteinte (renomme un existant ou supprime-en un)") : i18n::Tr("Enregistre l'équipement porté actuellement sous ce nom"));
  if (!can_save) ImGui::EndDisabled();
  if (!preset_status_.empty()) ImGui::TextColored(ro::pal::kLabel, "%s", preset_status_.c_str());

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

  // ⚠ Nuance locale, distincte de ro::pal::kGreen (0.10/0.50/0.15).
  const ImVec4 kGreen(0.15f, 0.55f, 0.20f, 1.0f);

  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(ro::pal::kBlack, "%s", i18n::Tr("Titre équipé :"));
  ImGui::SameLine();
  if (ot.equipped != 0)
    ImGui::TextColored(kGreen, "%s", TitleName(ot.equipped));
  else
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("aucun"));

  ImGui::Spacing();
  // Filtre par libellé (pratique quand beaucoup de titres décrochés).
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
  ImGui::InputTextWithHint("##cs_title_filter", i18n::Tr("filtrer par nom…"), title_filter_buf_,
                           sizeof(title_filter_buf_));
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  int to_equip = -1;  // titre à équiper APRÈS le rendu (0 = retirer, -1 = rien)

  // Entrée « Aucun titre » (retire le titre équipé) : visible seulement sans filtre actif.
  const bool filtering = title_filter_buf_[0] != '\0';
  if (!filtering) {
    const bool sel_none = ot.equipped == 0;
    if (ImGui::Selectable(i18n::Tr("Aucun titre"), sel_none)) to_equip = 0;
    ImGui::Spacing();
  }

  if (ot.count == 0) {
    ImGui::TextColored(ro::pal::kLabel,
                       "%s", i18n::Tr("Aucun titre décroché. Complète des succès qui récompensent un titre."));
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
      ImGui::SetTooltip(equipped ? i18n::Tr("Titre actuellement équipé (clic : garder)") : i18n::Tr("Clic : équiper ce titre"));
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
  if (!find(id, target)) { skill_status_ = i18n::Tr("Compétence introuvable dans l'arbre."); return false; }

  int spent = 0;
  for (const auto& p : skill_pending_) {
    SkillRaw fiche{};
    if (find(p.first, fiche)) spent += p.second - fiche.learned;
  }
  int left = SkillPointsSEH() - spent;
  if (left <= 0) { skill_status_ = i18n::Tr("Plus de point de compétence disponible."); return false; }

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

  // 1) TOUTE la chaîne de prérequis, pas seulement le rang précédent.
  // La liste portée par une compétence n'est PAS la fermeture transitive : Cart
  // Termination réclame Cart Boost, qui réclame Pushcart 5, qui réclame Enlarge Weight
  // Limit 5 — et cette dernière n'apparaît nulle part dans la liste de Cart Termination.
  // S'arrêter au rang direct (ce que fait le natif) laissait donc des trous dans l'arbre.
  // Ils passent quand même côté serveur, parce que `player_skillfree: yes` y désactive
  // toute vérification de prérequis (pc_calc_skilltree) — raison de plus pour que ce soit
  // le client qui tienne l'arbre cohérent.
  struct PendingReq { int id; int lvl; int depth; };
  PendingReq chain[64];
  int chain_count = 0;
  // Parcours en largeur des prérequis. Un même prérequis peut être réclamé par
  // plusieurs branches : on garde le niveau LE PLUS HAUT et la profondeur LA PLUS
  // GRANDE (c'est elle qui décide de l'ordre de réservation).
  auto push_req = [&](int req_id, int req_lvl, int depth) {
    for (int i = 0; i < chain_count; ++i) {
      if (chain[i].id != req_id) continue;
      chain[i].lvl = std::max(chain[i].lvl, req_lvl);
      chain[i].depth = std::max(chain[i].depth, depth);
      return;
    }
    if (chain_count < 64) chain[chain_count++] = {req_id, req_lvl, depth};
  };
  for (int i = 0; i < target.need_count; ++i)
    push_req(target.need_id[i], target.need_lv[i], 1);
  // `chain_count` grandit pendant la boucle : c'est le parcours lui-même. Profondeur
  // bornée à 8 — aucun arbre de job n'est si profond, et un cycle dans les données ne
  // doit pas faire tourner l'UI en rond.
  for (int i = 0; i < chain_count; ++i) {
    if (chain[i].depth >= 8) continue;
    SkillRaw fiche{};
    if (!find(static_cast<uint16_t>(chain[i].id), fiche)) continue;
    for (int k = 0; k < fiche.need_count; ++k)
      push_req(fiche.need_id[k], fiche.need_lv[k], chain[i].depth + 1);
  }
  // Réservation des PLUS PROFONDS d'abord : les points restants doivent aller à la
  // base de la chaîne, sinon on paie le sommet et le pied reste verrouillé.
  int max_depth = 0;
  for (int i = 0; i < chain_count; ++i) max_depth = std::max(max_depth, chain[i].depth);
  for (int d = max_depth; d >= 1; --d) {
    for (int i = 0; i < chain_count; ++i) {
      if (chain[i].depth != d) continue;
      SkillRaw req{};
      if (!find(static_cast<uint16_t>(chain[i].id), req)) continue;
      if (req.user_up <= 0) continue;  // pas montable avec des points (quête / lien)
      reserve(req, chain[i].lvl);
    }
  }
  // 2) la compétence demandée elle-même.
  if (target.user_up <= 0) {
    skill_status_ = i18n::Tr("Cette compétence ne se monte pas avec des points.");
    return false;
  }
  const int current = std::max(target.learned, PendingLevel(id));
  if (current >= target.maxlv) { skill_status_ = i18n::Tr("Déjà au niveau maximum."); return false; }
  // `reserve` borne déjà au niveau max ET aux points restants : viser le max revient
  // à demander tout ce qui reste, sans boucler ni recompter les prérequis.
  if (reserve(target, to_max ? target.maxlv : current + 1) == 0) {
    skill_status_ = i18n::Tr("Points insuffisants pour les prérequis.");
    return false;
  }
  skill_status_.clear();
  return true;
}

void CharacterSheet::DrawSkillsTab() {
  // ⚠ Ambre CLAIR, distinct de ro::pal::kWarn (0.55/0.33/0.08).
  const ImVec4 kAmber(0.85f, 0.65f, 0.20f, 1.0f);
  // ⚠ Vert CLAIR, distinct de ro::pal::kGreen — ne pas aligner sans le voir.
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
  // 🔴 Table STATIQUE, donc NUE : un `Tr` posé ici serait évalué au chargement de
  // la DLL, avant le catalogue, et resterait français pour toujours. La traduction
  // se fait à la LECTURE, dans tab_label.
  static const char* kFallback[4] = {"1re classe", "2e classe", "3e classe", "4e classe"};
  // 🔴 Traduire ICI et pas à l'affichage : ces libellés sont ensuite COMPOSÉS par
  // snprintf (« %s / %s » pour les deux premières classes fusionnées). Après la
  // composition il n'y a plus de clé de catalogue à chercher — « 1re classe / 2e
  // classe » n'en est pas une, et ne le sera jamais.
  auto tab_label = [&](int t) -> const char* {
    if (t >= kSkillJobTabs) return i18n::Tr("Divers");
    const char* n = lua_names[t];
    if (!n[0]) return i18n::Tr(kFallback[t]);
    if (std::strcmp(n, "1st") == 0) return i18n::Tr(kFallback[0]);
    if (std::strcmp(n, "2nd") == 0) return i18n::Tr(kFallback[1]);
    if (std::strcmp(n, "3rd") == 0) return i18n::Tr(kFallback[2]);
    if (std::strcmp(n, "4th") == 0) return i18n::Tr(kFallback[3]);
    // Nom SPÉCIFIQUE à la classe, donné par le Lua du client (« Ninja »…) : il
    // vient déjà de la langue du client, on n'y touche pas.
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

  ImGui::TextColored(points_left > 0 ? kGreen : ro::pal::kLabel, i18n::Tr("Points : %d"), points_left);
  if (reserved_points > 0) {
    ImGui::SameLine();
    ImGui::TextColored(kAmber, i18n::Tr("(%d réservé%s)"), reserved_points,
                       reserved_points > 1 ? "s" : "");
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ro::Px(120.0f));
  ImGui::InputTextWithHint("##skfilter", i18n::Tr("Rechercher…"), skill_filter_buf_,
                           sizeof(skill_filter_buf_));
  ImGui::SameLine();
  if (ro::RoButton(skill_grid_ ? i18n::Tr("Grille") : i18n::Tr("Liste"), 58.0f, 0.0f))
    skill_grid_ = !skill_grid_;
  mui::Tooltip(i18n::Tr("Bascule entre la grille d'icônes (vue « moderne » du client) et la\n"
               "liste détaillée (niveau, SP, portée, prérequis)."));

  if (!skill_pending_.empty()) {
    if (ro::RoSmallButton(i18n::Tr("Appliquer"), 80.0f, 0.0f)) {
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
      skill_status_ = sent > 0 ? i18n::Tr("Envoyé au serveur.") : i18n::Tr("Rien à envoyer.");
    }
    ImGui::SameLine();
    if (ro::RoSmallButton(i18n::Tr("Annuler"), 70.0f, 0.0f)) {
      skill_pending_.clear();
      skill_status_.clear();
    }
    mui::Tooltip(i18n::Tr("Abandonne les points réservés (rien n'a encore été envoyé)."));
  }
  if (!skill_status_.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(ro::pal::kLabel, "%s", skill_status_.c_str());
  }

  // ── Seconde ligne : rappel des gestes + lissage des icônes ────────────────
  // « Raccourcis » en texte discret plutôt qu'un pavé permanent : les gestes se
  // découvrent une fois, la place au-dessus de la grille sert tous les jours.
  ImGui::TextDisabled("%s", i18n::Tr("Raccourcis"));
  mui::Tooltip(
      i18n::Tr("Clic gauche          réserve un point (et ses prérequis manquants)\n"
      "Ctrl + clic gauche   réserve jusqu'au niveau maximum\n"
      "Double-clic          lance la compétence (aucun point réservé)\n"
      "Clic droit           menu (monter, lancer, niveau d'utilisation, description)\n"
      "Ctrl + clic droit    description de la compétence\n"
      "Glisser              pose la compétence sur une barre d'action\n"
      "Survol               flèches de prérequis (ambre) et de suites (bleu)\n"
      "\n"
      "Rien n'est envoyé au serveur tant que « Appliquer » n'est pas cliqué."));
  // Le lissage ne se voit QUE sur les grosses icônes de la grille : en liste elles
  // font une hauteur de ligne, la case n'y servirait qu'à encombrer l'en-tête.
  if (skill_grid_) {
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ro::RoCheckbox(i18n::Tr("Lisser les icônes"), &skill_bilinear_)) {
      if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
    }
    mui::Tooltip(i18n::Tr("Filtrage bilinéaire des icônes de la grille.\n"
                 "Décoché (défaut) = pixels nets, comme le client natif, qui ne filtre pas."));
  }

  // ── Ligne STAFF : les atcommands qui refont l'arbre ───────────────────────
  // 🔴 Gate relu à CHAQUE frame (IsStaff), comme celui du volet staff du mannequin
  // et celui de StaffTools : un droit retiré en cours de session doit faire
  // disparaître la ligne sans attendre une reconnexion.
  //
  // Ces commandes court-circuitent la mécanique de réservation juste au-dessus :
  // elles ne passent pas par `skill_pending_`, elles agissent tout de suite. On
  // vide donc la réservation en cours, qui porterait sur un arbre qui n'existe
  // plus — un point réservé sur une compétence que `@resetskill` vient d'effacer
  // se ferait jeter au tri de la frame suivante, mais après avoir compté pour rien.
  if (IsStaff()) {
    ImGui::TextDisabled("%s", i18n::Tr("Staff"));
    ImGui::SameLine();
    if (ro::RoSmallButton(i18n::Tr("Reset"), 0.0f, 0.0f)) {
      SendAtCommand("@resetskill");
      skill_pending_.clear();
      skill_status_.clear();
    }
    mui::Tooltip(i18n::Tr("@resetskill : oublie toutes les compétences et rend les points."));
    ImGui::SameLine();
    if (ro::RoSmallButton(i18n::Tr("Tout apprendre"), 0.0f, 0.0f)) {
      SendAtCommand("@allskills");
      skill_pending_.clear();
      skill_status_.clear();
    }
    mui::Tooltip(i18n::Tr("@allskills : apprend toutes les compétences de la classe."));
    ImGui::SameLine();
    if (ro::RoSmallButton(i18n::Tr("Cooldowns"), 0.0f, 0.0f)) SendAtCommand("@resetcooltime");
    mui::Tooltip(i18n::Tr("@resetcooltime : remet à zéro le temps de recharge de toutes "
                          "les compétences (homoncule et mercenaire compris)."));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("99999").x +
                            ImGui::GetStyle().FramePadding.x * 2.0f);
    ImGui::InputInt("##skpoint", &staff_skpoint_, 0, 0);  // step=0 : pas de flèches ImGui
    if (staff_skpoint_ < 1)   staff_skpoint_ = 1;
    if (staff_skpoint_ > 9999) staff_skpoint_ = 9999;
    ImGui::SameLine();
    if (ro::RoSmallButton(i18n::Tr("Points"), 0.0f, 0.0f)) {
      char line[32];
      std::snprintf(line, sizeof(line), "@skpoint %d", staff_skpoint_);
      SendAtCommand(line);
    }
    mui::Tooltip(i18n::Tr("@skpoint : donne le nombre de points de compétence saisi."));
  }

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
  add_group(i18n::Tr("Divers"), nullptr, -1, kNoSource);  // liste plate

  if (group_count == 0) {
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Aucune compétence."));
    return;
  }
  if (ro::RoBeginTabBar("cs_skill_tabs")) {
    for (int g = 0; g < group_count; ++g) {
      char label[96];
      std::snprintf(label, sizeof(label), i18n::Tr("%s###skgrp%d"), groups[g].label, g);
      if (ImGui::BeginTabItem(label)) { skill_tab_ = g; ImGui::EndTabItem(); }
    }
    ro::RoEndTabBar();
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
    int n = ReadSkillTabSEH(groups[skill_tab_].src[gi], nodes + count,
                            kSkillMaxNodes - count);
    if (n <= 0) continue;
    // Doublon d'une source à l'autre : la lecture dédoublonne DANS une liste, mais
    // « 1re / 2e classe » sont FUSIONNÉES ici et la même compétence peut vivre dans
    // les deux onglets natifs. On garde la première (celle du tronc), sinon elle
    // occuperait deux cases et ImGui verrait deux items de même ID.
    {
      int kept = 0;
      for (int i = 0; i < n; ++i) {
        bool already_seen = false;
        for (int k = 0; k < count && !already_seen; ++k)
          already_seen = nodes[k].id == nodes[count + i].id;
        if (already_seen) continue;
        if (kept != i) nodes[count + kept] = nodes[count + i];
        ++kept;
      }
      n = kept;
    }
    if (n <= 0) continue;
    // `count > 0` et pas `gi > 0` : si la source précédente était vide, celle-ci est
    // la première à l'écran — un trait au-dessus de la toute première ligne n'aurait
    // rien à séparer.
    if (count > 0 && split_count < 1) {
      const int src = groups[skill_tab_].src[gi];
      split_row[split_count]   = base / kSkillGridCols;
      split_label[split_count] = (src >= 0) ? tab_label(src) : i18n::Tr("Divers");
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
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Aucune compétence dans cet onglet."));
    return;
  }

  // Filtre par nom (le libellé localisé, pas l'idname).
  const bool filtering = skill_filter_buf_[0] != '\0';
  auto skill_name = [](int id) -> const char* {
    const char* n = lua::SkillName(id);
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
      tip += i18n::Tr("\nNiveau ") + std::to_string(effective) + " / " +
             std::to_string(s.maxlv);
      const int pending = PendingLevel(static_cast<uint16_t>(s.id));
      if (pending > 0) tip += "  (+" + std::to_string(pending - s.learned) + i18n::Tr(" réservé)");
    }
    tip += s.inf == 0 ? i18n::Tr("\nPassive (toujours active)") : i18n::Tr("\nActive");
    if (s.learned > 0 && s.sp > 0)    tip += i18n::Tr("\nSP : ") + std::to_string(s.sp);
    if (s.learned > 0 && s.range > 0) tip += i18n::Tr("\nPortée : ") + std::to_string(s.range);
    const unsigned long cd_ms = SkillCooldownRemaining(static_cast<uint16_t>(s.id));
    if (cd_ms > 0)
      tip += i18n::Tr("\nEncore ") + std::to_string((cd_ms + 999) / 1000) +
             i18n::Tr(" s de cooldown");
    if (s.need_count > 0) {
      tip += i18n::Tr("\nRequiert : ");
      for (int i = 0; i < s.need_count; ++i) {
        if (i) tip += ", ";
        tip += skill_name(s.need_id[i]);
        tip += i18n::Tr(" Niv ");
        tip += std::to_string(s.need_lv[i]);
        // Un prérequis peut vivre dans un AUTRE onglet (une 3e classe en réclame
        // souvent une de 2e) : aucune flèche ne peut alors le désigner, autant le
        // dire — c'est ce que le natif signale en coloriant l'onglet concerné.
        bool here = false;
        for (int k = 0; k < count && !here; ++k) here = nodes[k].id == s.need_id[i];
        if (!here) tip += i18n::Tr(" (autre onglet)");
      }
    }
    if (s.user_up <= 0) tip += i18n::Tr("\n\nNe se monte pas avec des points (quête / lien).");
    if (s.user_up > 0 && effective < s.maxlv)
      tip += i18n::Tr("\n\nClic : réserver un point — Ctrl + clic : jusqu'au max");
    else
      tip += "\n";
    if (s.learned > 0 && s.inf != 0) tip += i18n::Tr("\nDouble-clic : lancer");
    if (s.learned > 1 && IsLevelUseSkillSEH(s.id))
      tip += i18n::Tr("\nMolette : niveau de lancement (") +
             std::to_string(EffectiveUseLevelSEH(s.id, s.learned)) + " / " +
             std::to_string(s.learned) + ")";
    tip += i18n::Tr("\nClic droit : menu — Ctrl + clic droit : description");
    if (s.learned > 0 && s.inf != 0) tip += i18n::Tr("\nGlisser : poser sur une barre d'action");
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
        itemdb::OpenSkillDesc(s.id, static_cast<int>(mp.x), static_cast<int>(mp.y));
      } else {
        ImGui::OpenPopup("skctx");
      }
    }
    if (!ImGui::BeginPopup("skctx")) return;
    const bool can_raise = s.user_up > 0 && effective < s.maxlv;
    if (ImGui::MenuItem(i18n::Tr("Monter d'un niveau"), nullptr, false, can_raise && points_left > 0))
      ReserveSkillPoint(static_cast<uint16_t>(s.id));
    if (ImGui::MenuItem(i18n::Tr("Lancer"), nullptr, false, s.learned > 0 && s.inf != 0))
      SendUseSkill(static_cast<uint16_t>(s.id), EffectiveUseLevelSEH(s.id, s.learned));
    // Niveau d'utilisation : réglage 100 % client (le natif l'expose par les
    // « + / − » de chaque case), borné au niveau APPRIS, et c'est lui que la barre
    // de raccourcis envoie au lancement.
    if (s.learned > 0 && IsLevelUseSkillSEH(s.id)) {
      ImGui::Separator();
      const int use = EffectiveUseLevelSEH(s.id, s.learned);
      ImGui::TextColored(ro::pal::kLabel, i18n::Tr("Lancer au niveau %d / %d"), use, s.learned);
      // « - » ASCII, pas le signe moins U+2212 : la police de l'UI ne le porte pas
      // et il sortait en tofu dans le menu.
      if (ImGui::MenuItem(i18n::Tr("  niveau -"), nullptr, false, use > 1))
        SetUseLevelSEH(s.id, use - 1);
      if (ImGui::MenuItem(i18n::Tr("  niveau +"), nullptr, false, use < s.learned))
        SetUseLevelSEH(s.id, use + 1);
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::Tr("Description"))) {
      const ImVec2 mp = ImGui::GetIO().MousePos;
      itemdb::OpenSkillDesc(s.id, static_cast<int>(mp.x), static_cast<int>(mp.y));
    }
    ImGui::EndPopup();
  };

  // Pastille « niveau de lancement » au centre de l'icône, GRILLE ET LISTE : elle ne
  // paraît que si le réglage est BRIDÉ sous le niveau appris (cas anormal), sinon rien —
  // le lancement, le double-clic et le glisser partent tous à ce niveau-là et il faut
  // pouvoir le voir sans ouvrir le menu. `size` = côté de l'icône déjà dessinée.
  // `always` = pendant un glisser : là on montre le niveau MÊME au maximum, c'est celui
  // qui va être posé sur la barre et la molette le règle à la volée.
  auto draw_use_level_badge = [](ImDrawList* dl, const SkillRaw& s, const ImVec2& ip,
                                 float size, bool always = false) {
    if (s.learned <= 0) return;
    // IsLevelUseSkill n'est interrogé qu'après le test de bridage : c'est un appel
    // natif, inutile de le payer sur chaque case de la grille.
    const int use_lv = EffectiveUseLevelSEH(s.id, s.learned);
    if ((use_lv >= s.learned && !always) || !IsLevelUseSkillSEH(s.id)) return;
    char use_txt[8];
    std::snprintf(use_txt, sizeof(use_txt), "%d", use_lv);
    const ImVec2 usz = ImGui::CalcTextSize(use_txt);
    const ImVec2 up(ip.x + (size - usz.x) * 0.5f, ip.y + (size - usz.y) * 0.5f);
    dl->AddRectFilled(ImVec2(up.x - 4.0f, up.y - 2.0f),
                      ImVec2(up.x + usz.x + 4.0f, up.y + usz.y + 2.0f),
                      IM_COL32(15, 15, 20, 205), 4.0f);
    dl->AddRect(ImVec2(up.x - 4.0f, up.y - 2.0f),
                ImVec2(up.x + usz.x + 4.0f, up.y + usz.y + 2.0f),
                IM_COL32(255, 205, 105, 220), 4.0f, 0, 1.0f);
    dl->AddText(up, IM_COL32(255, 215, 130, 255), use_txt);
  };

  // Gestes partagés par la case et la ligne : ils suivent le DERNIER widget soumis.
  auto common_item_actions = [&](const SkillRaw& s, int effective, ro::IconTex ic) {
    // ── Molette = niveau de lancement ───────────────────────────────────────────
    // AU SURVOL et PENDANT LE GLISSER : dans les deux cas c'est cette case qui doit
    // posséder la molette, sinon le grimoire défile sous le curseur au lieu de régler
    // le niveau. `LastItemWheel` couvre les deux et il faut l'appeler ICI, tant que le
    // dernier item soumis est bien la case : après BeginDragDropSource ce serait
    // l'infobulle de glisser. Au survol il impose son verrou anti-défilement (parcourir
    // le grimoire ne doit rien régler) ; pendant le glisser la case est « engagée » et
    // garde la molette sans délai, curseur sorti ou non. Voir ui/ro_widgets.h.
    // Réservée aux compétences qu'on peut vraiment doser (effet dépendant du niveau ET
    // au moins 2 niveaux appris) : ailleurs la molette garde son rôle de défilement.
    const bool level_tunable = s.learned > 1 && IsLevelUseSkillSEH(s.id);
    const bool dragging_this = ImGui::IsItemActive();
    const bool hovering_this =
        ImGui::IsItemHovered() && ImGui::GetDragDropPayload() == nullptr;
    if (level_tunable && (hovering_this || dragging_this)) {
      const float wheel = mui::LastItemWheel(/*engaged=*/dragging_this);
      if (wheel != 0.0f) {
        const int use = EffectiveUseLevelSEH(s.id, s.learned);
        // Un cran = un niveau, quel que soit le pas de la molette (les souris à
        // défilement libre envoient des fractions : on ne veut pas 0 niveau de plus).
        const int next = std::min(s.learned, std::max(1, use + (wheel > 0.0f ? 1 : -1)));
        if (next != use) SetUseLevelSEH(s.id, next);
      }
    }
    if (s.learned > 0 && s.inf != 0 && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
      // Même charge utile que l'onglet Guilde : la barre d'action ImGui l'accepte déjà.
      // Niveau EFFECTIF, RELU À CHAQUE FRAME : la molette peut encore le changer en plein
      // vol, et sans réglage explicite on pose la compétence au MAXIMUM appris (le getter
      // renvoie 0 dans ce cas, et un max(1, 0) la posait au niveau 1).
      const int payload[2] = {s.id, EffectiveUseLevelSEH(s.id, s.learned)};
      ImGui::SetDragDropPayload("BGN_SKILL", payload, sizeof(payload));
      if (ic.tex) {
        // Pastille sur l'icône de l'infobulle : on voit le niveau qui sera posé AVANT
        // de lâcher (affichée même au maximum, contrairement à la grille).
        const ImVec2 drag_ip = ImGui::GetCursorScreenPos();
        ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(24.0f, 24.0f));
        draw_use_level_badge(ImGui::GetWindowDrawList(), s, drag_ip, 24.0f, true);
        ImGui::SameLine();
      }
      ImGui::TextUnformatted(skill_name(s.id));
      if (level_tunable) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", i18n::Tr("(molette : niveau)"));
      }
      ImGui::EndDragDropSource();
    }
    if (hovering_this) {
      hovered_now = static_cast<uint16_t>(s.id);
      tooltip_for(s, effective);
    }
    // ── Clic simple = réserver, double-clic = LANCER ────────────────────────────
    // Les deux gestes commencent pareil : il faut donc ATTENDRE de savoir. Sans ça,
    // un double-clic pour lancer Vending réserve d'abord un point au passage (vécu).
    // `IsMouseReleasedWithDelay` + `MouseClickedLastCount == 1` est l'idiome ImGui
    // prévu exactement pour cette levée d'ambiguïté (cf. imgui.h) : l'action simple
    // ne part qu'une fois le délai de double-clic écoulé sans second clic.
    const ImGuiIO& io = ImGui::GetIO();
    // Cellule où le bouton a été ENFONCÉ : l'action différée arrive 0,3 s plus tard,
    // le curseur peut avoir bougé, et c'est bien la case visée qui doit agir.
    static uint16_t press_id = 0;
    static bool press_clean = false;  // relâchée sans avoir glissé (sinon c'était un drag)
    static bool press_ctrl  = false;  // Ctrl AU MOMENT du clic, pas 0,3 s après
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      press_id = static_cast<uint16_t>(s.id);
      // Pas au SECOND clic d'un double : il effacerait le verdict du premier relâché,
      // dont le double-clic a justement besoin pour savoir qu'on n'a pas glissé.
      if (io.MouseClickedLastCount[ImGuiMouseButton_Left] <= 1) press_clean = false;
    }
    if (press_id == static_cast<uint16_t>(s.id)) {
      // ⚠ GetMouseDragDelta ne vaut plus rien après la frame du relâché : on retient
      // le verdict ICI, pas au moment de l'action différée.
      if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const ImVec2 travel = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        press_clean = travel.x == 0.0f && travel.y == 0.0f &&
                      ImGui::GetDragDropPayload() == nullptr;
        press_ctrl = io.KeyCtrl;
      }
      const bool castable = s.learned > 0 && s.inf != 0;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && press_clean) {
        // Lancer au niveau d'utilisation choisi, comme la barre de raccourcis.
        if (castable) SendUseSkill(static_cast<uint16_t>(s.id),
                                   EffectiveUseLevelSEH(s.id, s.learned));
        else          ReserveSkillPoint(static_cast<uint16_t>(s.id), press_ctrl);
        press_id = 0;
      } else if (ImGui::IsMouseReleasedWithDelay(ImGuiMouseButton_Left,
                                                 io.MouseDoubleClickTime) &&
                 io.MouseClickedLastCount[ImGuiMouseButton_Left] == 1 && press_clean) {
        ReserveSkillPoint(static_cast<uint16_t>(s.id), press_ctrl);
        press_id = 0;
      }
    }
    context_menu(s, effective);
  };

  // ── Graphe de prérequis : prédicats partagés par la grille ET la liste ───────
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
  // RÉDUCTION TRANSITIVE, pour un lien `prereq_id -> dependent`. La liste de prérequis
  // du client contient des RACCOURCIS : 136 arêtes redondantes mesurées dans
  // skillinfolist.lub, dont Bowling Bash qui réclame Bash ET Magnum Break, alors que
  // Magnum Break réclame déjà Bash. Tracé tel quel, le même chemin serait doublé. On
  // saute donc le trait direct quand un AUTRE prérequis de `dependent` réclame déjà
  // `prereq_id`. (⚠ Ces raccourcis ne font PAS de la liste une fermeture transitive —
  // cf. ReserveSkillPoint, qui doit remonter la chaîne lui-même.)
  auto redundant_edge = [&](const SkillRaw& dependent, int prereq_id) {
    for (int k = 0; k < dependent.need_count; ++k) {
      if (dependent.need_id[k] == prereq_id) continue;
      if (requires_skill(find_node(dependent.need_id[k]), prereq_id)) return true;
    }
    return false;
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

    // Filtre des icônes : un seul basculement pour toute la grille (le callback coupe
    // le lot de draws en deux, autant ne pas le faire par case).
    // ⚠ Callback posé dans les DEUX cas, pas seulement quand le lissage est demandé :
    // l'état ambiant à cet endroit de la draw list est LINEAR (le backend DX9 le remet
    // dans SetupRenderState), donc ne rien faire = icônes déjà lissées et la case à
    // cocher semblait morte. C'est le mode NET qui doit être imposé.
    g_skill_icon_bilinear = skill_bilinear_;
    dl->AddCallback(CbSkillIconFilter, nullptr);

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
      // Cooldown en cours : même voile montant + décompte que la barre de raccourcis.
      if (learned) DrawSkillCooldownOverlay(dl, static_cast<uint16_t>(s.id), ip, icon);

      draw_use_level_badge(dl, s, ip, icon);

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
    dl->AddCallback(CbSkillIconFilterOff, nullptr);

    // ── Séparateur entre deux arbres fusionnés (trait rouge + libellé) ────────
    for (int k = 0; k < split_count; ++k) {
      const float y  = row_y(split_row[k]) - split_gap * 0.5f;
      const float x0 = origin.x;
      const float x1 = origin.x + kSkillGridCols * cell_w - pad;
      const ImU32 line_col = IM_COL32(200, 60, 60, 200);
      const char* txt = split_label[k] ? split_label[k] : i18n::Tr("Suite");
      const ImVec2 tsz = ImGui::CalcTextSize(txt);
      const float tx = x0 + 14.0f;
      dl->AddLine(ImVec2(x0, y), ImVec2(tx - 6.0f, y), line_col, 2.0f);
      dl->AddLine(ImVec2(tx + tsz.x + 6.0f, y), ImVec2(x1, y), line_col, 2.0f);
      dl->AddText(ImVec2(tx, y - tsz.y * 0.5f), IM_COL32(170, 40, 40, 255), txt);
    }

    // ── Flèches de dépendance, seulement autour de la case survolée ──────────
    // Les tracer en permanence ferait un plat de spaghettis ; au survol, elles
    // répondent exactement à la question qu'on se pose à ce moment-là : « d'où vient
    // cette compétence, et qu'ouvre-t-elle ? ». Ambre = la BRANCHE qui y mène (toute
    // la chaîne de prérequis, pas seulement le rang précédent), bleu = la branche
    // qu'elle ouvre. Les deux sont tracées par le même parcours, en sens inverse.
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
      // Parcours de la BRANCHE, en largeur, depuis la case survolée — en amont
      // (« ce qu'il faut avant ») comme en aval (« ce que ça ouvre »). S'arrêter au
      // premier rang ne montrait qu'un bout du chemin, or c'est le chemin ENTIER
      // qu'on cherche en survolant une compétence. Chaque case n'est développée
      // qu'une fois : le graphe a des raccourcis, sans marquage une même case serait
      // redéveloppée à chaque profondeur. Le trait pâlit et s'affine avec la
      // distance, pour que l'ordre de la chaîne se lise d'un coup d'œil.
      // La flèche va TOUJOURS du prérequis vers ce qu'il débloque, quel que soit le
      // sens de parcours : c'est le sens de lecture de l'arbre, pas celui du survol.
      static int  bfs_queue[kSkillMaxNodes];
      static int  bfs_depth[kSkillMaxNodes];
      static bool bfs_seen[kSkillMaxNodes];
      auto walk_chain = [&](bool upstream, int cr, int cg, int cb, int alpha0,
                            float thick0) {
        for (int i = 0; i < count; ++i) bfs_seen[i] = false;
        int head_q = 0, tail_q = 0;
        bfs_queue[tail_q] = hover_idx;
        bfs_depth[tail_q++] = 0;
        bfs_seen[hover_idx] = true;
        while (head_q < tail_q) {
          const int cur   = bfs_queue[head_q];
          const int depth = bfs_depth[head_q];
          ++head_q;
          if (depth >= 6) continue;  // garde-fou : aucun arbre de job n'est si profond
          for (int i = 0; i < count; ++i) {
            if (i == cur) continue;
            // Amont : `i` est un prérequis de `cur`. Aval : `i` réclame `cur`.
            const int dep = upstream ? cur : i;  // celui qui réclame
            const int req = upstream ? i : cur;  // celui qui est réclamé
            if (!requires_skill(&nodes[dep], nodes[req].id)) continue;
            if (redundant_edge(nodes[dep], nodes[req].id)) continue;
            if (cell_drawn[dep] && cell_drawn[req])
              draw_arrow(cell_center[req], cell_center[dep],
                         IM_COL32(cr, cg, cb, std::max(85, alpha0 - depth * 35)),
                         std::max(1.2f, thick0 - depth * 0.25f));
            if (!bfs_seen[i]) {
              bfs_seen[i] = true;
              bfs_queue[tail_q] = i;
              bfs_depth[tail_q++] = depth + 1;
            }
          }
        }
      };
      walk_chain(true, 255, 205, 105, 235, 2.5f);   // ambre : ce qu'il faut avant
      walk_chain(false, 120, 160, 255, 200, 2.0f);  // bleu  : ce que ça ouvre
    }

    // Réserver la hauteur consommée : la grille est dessinée en absolu, ImGui ne
    // connaîtrait sinon aucune étendue et le scroll serait mort.
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(kSkillGridCols * cell_w,
                        (used_max_row + 1) * cell_h + split_count * split_gap));
  } else {
    // ── Vue LISTE : le même arbre, mais lu comme un arbre ─────────────────────
    // Profondeur = 1 + celle du prérequis le plus profond PRÉSENT dans l'onglet, puis
    // tri par profondeur : un prérequis est donc toujours affiché AVANT ce qu'il
    // débloque, ce qui permet de tracer les coudes de liaison (même dispositif que
    // l'onglet Compétences de guilde) — l'ancre du parent est déjà connue.
    //
    // ⚠ L'INDENTATION EST PLAFONNÉE. Mesuré sur les .lub du client, arbres 1re + 2e
    // classe fusionnés : 11 des 13 classes pré-renewal tiennent en 5 niveaux, mais
    // Monk monte à 9 et Rogue à 8 (une seule compétence à chaque palier profond).
    // Indenter linéairement mangerait la colonne du nom pour une poignée de lignes ;
    // au-delà du plafond le décalage se fige et c'est le COUDE qui dit le parent.
    constexpr int   kTreeIndentCap = 5;
    constexpr float kTreeStep      = 14.0f;
    constexpr float kTreeGutter    = 14.0f;  // place des traits, à gauche de l'icône
    static int   depth_of[kSkillMaxNodes];
    static int   need_idx[kSkillMaxNodes][kSkillMaxNeed];  // prérequis -> index local
    static int   order[kSkillMaxNodes];
    static ImVec2 anchor[kSkillMaxNodes];
    // Table de correspondance construite UNE fois : sans elle, chaque passe de calcul
    // de profondeur relancerait une recherche linéaire par prérequis (O(n²) par passe).
    for (int i = 0; i < count; ++i) {
      depth_of[i] = 0;
      order[i] = i;
      anchor[i] = ImVec2(-1.0f, -1.0f);
      for (int k = 0; k < nodes[i].need_count; ++k) {
        need_idx[i][k] = -1;
        for (int j = 0; j < count; ++j)
          if (nodes[j].id == nodes[i].need_id[k]) { need_idx[i][k] = j; break; }
      }
    }
    // Passes successives, bornées par le nombre de nœuds : ça converge en 3-4 tours
    // et ça protège d'un cycle si les données en contenaient un.
    for (int pass = 0; pass < count; ++pass) {
      bool changed = false;
      for (int i = 0; i < count; ++i) {
        int d = 0;
        for (int k = 0; k < nodes[i].need_count; ++k) {
          const int j = need_idx[i][k];
          if (j >= 0 && depth_of[j] + 1 > d) d = depth_of[j] + 1;
        }
        if (d != depth_of[i]) { depth_of[i] = d; changed = true; }
      }
      if (!changed) break;
    }
    std::stable_sort(order, order + count, [&](int a, int b) {
      if (depth_of[a] != depth_of[b]) return depth_of[a] < depth_of[b];
      return disp_pos[a] < disp_pos[b];  // à profondeur égale, l'ordre de la grille
    });

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                  ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("cs_skill_tbl", 5, flags)) {
      ImGui::TableSetupColumn(i18n::Tr("Compétence"), ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn(i18n::Tr("Niveau"), ImGuiTableColumnFlags_WidthFixed, ro::Px(56.0f));
      ImGui::TableSetupColumn(i18n::Tr("SP"), ImGuiTableColumnFlags_WidthFixed, ro::Px(40.0f));
      ImGui::TableSetupColumn(i18n::Tr("Portée"), ImGuiTableColumnFlags_WidthFixed, ro::Px(46.0f));
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ro::Px(28.0f));
      ImGui::TableHeadersRow();
      const float icon = ImGui::GetTextLineHeight();
      for (int oi = 0; oi < count; ++oi) {
        const int i = order[oi];
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
        const float row_indent =
            kTreeGutter + std::min(depth_of[i], kTreeIndentCap) * kTreeStep;
        ImGui::Indent(row_indent);
        const ro::IconTex ic = ResolveSkillIcon(s.id);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        anchor[i] = ImVec2(p.x, p.y + icon * 0.5f);
        // Coude vers chaque prérequis (réduction transitive appliquée : la liste du
        // client est aplatie, tout tracer doublerait les chemins). La verticale passe
        // à un demi-pas à GAUCHE de l'icône du parent, dans la gouttière — sous son
        // centre elle traverserait les icônes des lignes de même profondeur.
        ImDrawList* row_dl = ImGui::GetWindowDrawList();
        for (int k = 0; k < s.need_count; ++k) {
          const int src = need_idx[i][k];
          if (src < 0 || anchor[src].x < 0.0f) continue;
          if (redundant_edge(s, s.need_id[k])) continue;
          const bool hot = focus != 0 && (static_cast<int>(focus) == s.id ||
                                          static_cast<int>(focus) == s.need_id[k]);
          const ImU32 col = hot ? IM_COL32(255, 205, 105, 255) : IM_COL32(150, 155, 190, 110);
          const float thickness = hot ? 2.5f : 1.0f;
          const float x = anchor[src].x - kTreeStep * 0.5f;
          row_dl->AddLine(ImVec2(anchor[src].x - 2.0f, anchor[src].y), ImVec2(x, anchor[src].y),
                          col, thickness);
          row_dl->AddLine(ImVec2(x, anchor[src].y), ImVec2(x, anchor[i].y), col, thickness);
          row_dl->AddLine(ImVec2(x, anchor[i].y), ImVec2(anchor[i].x - 2.0f, anchor[i].y),
                          col, thickness);
          row_dl->AddCircleFilled(ImVec2(anchor[i].x - 3.0f, anchor[i].y), hot ? 3.0f : 2.0f, col);
        }
        // Selectable posé EN PREMIER et sur TOUTE la ligne, ICÔNE COMPRISE : c'est lui
        // qui porte le survol, le clic, le glisser et le menu. Avec l'icône dessinée
        // AVANT lui puis un Selectable réduit au nom, l'icône était une zone morte —
        // survolée, elle ne donnait ni infobulle ni molette, alors que c'est justement
        // là qu'on vise. Le visuel se pose PAR-DESSUS en ImDrawList, jamais en
        // repositionnant le curseur.
        ImGui::Selectable("##row", false, ImGuiSelectableFlags_None, ImVec2(0.0f, icon));
        if (ic.tex)
          row_dl->AddImage(
              reinterpret_cast<ImTextureID>(ic.tex), p, ImVec2(p.x + icon, p.y + icon),
              ImVec2(0, 0), ImVec2(1, 1),
              effective > 0 ? IM_COL32_WHITE : IM_COL32(110, 110, 110, 160));
        draw_use_level_badge(row_dl, s, p, icon);  // même repère qu'en grille
        const ImU32 name_col =
            effective == 0 ? ImGui::GetColorU32(ro::pal::kLabel) : ImGui::GetColorU32(ImGuiCol_Text);
        row_dl->AddText(ImVec2(p.x + icon + ImGui::GetStyle().ItemSpacing.x,
                               p.y + (icon - ImGui::GetTextLineHeight()) * 0.5f),
                        name_col, skill_name(s.id));
        common_item_actions(s, effective, ic);
        ImGui::Unindent(row_indent);

        ImGui::TableNextColumn();
        if (pending > 0) ImGui::TextColored(kAmber, "%d/%d", effective, s.maxlv);
        else if (effective > 0) ImGui::Text("%d/%d", effective, s.maxlv);
        else ImGui::TextColored(ro::pal::kLabel, "-/%d", s.maxlv);

        ImGui::TableNextColumn();
        if (s.inf == 0)          ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("passif"));
        else if (s.learned > 0)  ImGui::Text("%d", s.sp);
        else                     ImGui::TextColored(ro::pal::kLabel, "-");

        ImGui::TableNextColumn();
        if (s.learned > 0 && s.range > 0) ImGui::Text("%d", s.range);
        else                              ImGui::TextColored(ro::pal::kLabel, "-");

        ImGui::TableNextColumn();
        if (s.user_up > 0 && effective < s.maxlv && points_left > 0) {
          if (ro::RoSmallButton("+", 22.0f, 0.0f))
            ReserveSkillPoint(static_cast<uint16_t>(s.id), ImGui::GetIO().KeyCtrl);
          mui::Tooltip(i18n::Tr("Réserve un point (et ses prérequis) ; Ctrl = jusqu'au niveau\n"
                       "maximum. « Appliquer » valide."));
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  }
  ImGui::EndChild();
  skill_hover_ = hovered_now;  // consommé à la frame suivante (surlignage des liens)
}

// ═══ Onglet Homoncule ═══════════════════════════════════════════════════════
// Les DEUX fenêtres natives de l'homoncule en une : l'état (UIHomunInfoWnd, id 113,
// raccourci Alt+R) et l'arbre de compétences (UISkillListWnd, id 114, bouton
// « btn_skill » de la première). En interface moderne elles sont REMPLACÉES —
// détruites à la naissance, leur demande routée ici (HandleReplacedNativeCreation).
// Tout est lu LIVE dans les globals plats et la std::list de compétences ; les
// actions partent en paquets bruts, exactement comme le natif.
//
// 🔴 Combler les manques du natif, pas seulement l'imiter : la fiche native
// n'affiche NI l'exp courante (elle imprime l'exp requise), NI l'état « déjà
// renommé », NI si une compétence peut encore monter — elle laisse cliquer et le
// serveur jette en silence. Les trois sont ici.
// RE complet : docs/homunculus_re.md.
void CharacterSheet::DrawHomunTab() {

  rag::homun::State h{};
  if (!rag::homun::ReadState(&h)) {
    // Classe == -1 : le client ne sait rien de l'homoncule — c'est exactement le cas
    // où MakeWindow(113) refuse de créer la fenêtre native.
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Aucun homoncule."));
    ImGui::Spacing();
    ImGui::TextWrapped("%s", i18n::Tr(
              "Un Alchimiste ayant appris Bioéthique peut en invoquer un avec une Embryon. "
              "Cet onglet reprend la fiche d'état (Alt+R) et l'arbre de compétences."));
    return;
  }

  const bool resting = (h.flags & rag::homun::kFlagResting) != 0;
  const bool alive   = (h.flags & rag::homun::kFlagAlive) != 0;
  const bool renamed = (h.flags & rag::homun::kFlagRenamed) != 0;

  // ── En-tête ───────────────────────────────────────────────────────────────
  // Tout l'onglet est calibré pour tenir dans la feuille SANS le volet stats
  // (DollPaneW(), au moins 280 px de contenu) : c'est la largeur de repli, et rien
  // ici ne justifie d'imposer la large. D'où les libellés courts et les boutons
  // ajustés au texte plutôt qu'à une largeur ronde.
  const float kFullW = ImGui::GetContentRegionAvail().x;

  if (homun_rename_edit_) {
    // `homun_name_buf_` fait 24 octets, soit 23 caractères + terminateur : la BORNE
    // du natif (MSI_HOMUN_NAME_IN23, « no longer than 23 letters ») est donc imposée
    // par la saisie elle-même, on ne peut pas taper de nom refusable.
    // 2 boutons de 60 + les 3 espacements : le champ prend tout le reste.
    ImGui::SetNextItemWidth(kFullW - 120.0f - 3.0f * ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputText("##homun_name", homun_name_buf_, sizeof(homun_name_buf_));
    ImGui::SameLine();
    if (ro::RoSmallButton(i18n::Tr("Valider"), 60.0f, 0.0f) && homun_name_buf_[0]) {
      SendHomunRename(homun_name_buf_);
      homun_status_ = i18n::Tr("Renommage envoyé.");
      homun_rename_edit_ = false;
    }
    ImGui::SameLine();
    if (ro::RoSmallButton(i18n::Tr("Annuler"), 60.0f, 0.0f)) homun_rename_edit_ = false;
    ImGui::PushStyleColor(ImGuiCol_Text, ro::pal::kLabel);
    ImGui::TextWrapped("%s", StripRoColors(msgstr::Utf8(kMsiNameTooLong)));
    ImGui::PopStyleColor();
  } else {
    // Le NOM EST le bouton de renommage — tant que le serveur l'accepte encore.
    // Une fois le droit consommé il redevient du texte : pas de bouton qui ne
    // saurait que se faire refuser.
    //
    // ⚠ Sur MOONLIGHT ce verrou n'existe pas : `conf/import/battle_conf.txt` pose
    // `hom_rename: yes` (le défaut rAthena est `no`), et `clif_hominfo` n'envoie le
    // bit 0 que si `!battle_config.hom_rename` — il ne vient donc JAMAIS, et
    // `hom_change_name` ne teste pas non plus le drapeau. Renommer est libre et
    // illimité, pour tout le monde : ce n'est PAS une permission de groupe, aucun
    // test de droit n'intervient dans ce chemin. La branche « déjà renommé »
    // ci-dessous n'est donc pas morte pour autant — elle reprend du service si la
    // config repasse à `no`.
    const char* shown = h.name[0] ? h.name : (h.job[0] ? h.job : "?");
    if (!renamed) {
      // Suffixe « ## » : RoSmallButton tire son identifiant du libellé (PushID) et
      // n'affiche que ce qui précède les deux dièses. Sans lui, l'identifiant du
      // bouton changerait avec le nom de l'homoncule.
      char btn[64];
      std::snprintf(btn, sizeof(btn), "%s##homun_rename", shown);
      if (ro::RoSmallButton(btn, 0.0f, 0.0f)) {
        std::snprintf(homun_name_buf_, sizeof(homun_name_buf_), "%s", h.name);
        homun_rename_edit_ = true;
      }
      // Pas de « une seule fois » ici : sur ce serveur c'est faux. Le rappel ne
      // s'affiche que dans la branche où le serveur a réellement posé le verrou.
      mui::Tooltip(i18n::Tr("Cliquer pour renommer."));
    } else {
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(ro::pal::kValue, "%s", shown);
      mui::Tooltip(i18n::Tr("Le serveur n'accepte le renommage qu'UNE fois."));
    }
    // « Abandonner » remonte ici, collé au bord droit : il est IRRÉVERSIBLE, on le
    // veut loin de la case auto-alimentation qu'on coche et décoche sans y penser.
    const float w = 100.0f;
    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    if (ro::RoSmallButton(i18n::Tr("Abandonner…"), w, 0.0f)) {
      homun_del_ask_ = true;
      homun_del_confirm_[0] = '\0';
    }
  }

  // Espèce, niveau et état sur UNE ligne — trois informations courtes qui n'ont pas
  // besoin d'une ligne chacune à 280 px.
  ImGui::TextColored(ro::pal::kLabel, "%s  ·  %s %d", h.job[0] ? h.job : "?", msgstr::Utf8(kMsiLevel),
                     h.level);
  if (resting || !alive) {
    ImGui::SameLine();
    if (resting) ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Au repos."));
    else         ImGui::TextColored(ro::pal::kRed,  "%s", i18n::Tr("Hors de combat (0 PV)."));
  }
  ImGui::Separator();

  // ── Jauges PV / SP / EXP / satiété ────────────────────────────────────────
  auto gauge = [](const char* label, long long cur, long long max, const ImVec4& col) {
    char text[80];
    const float ratio = max > 0 ? std::clamp(static_cast<float>(static_cast<double>(cur) /
                                                               static_cast<double>(max)),
                                             0.0f, 1.0f)
                                : 0.0f;
    if (max > 0) std::snprintf(text, sizeof(text), "%s  %lld / %lld  (%.1f %%)", label, cur,
                               max, ratio * 100.0f);
    else         std::snprintf(text, sizeof(text), "%s  %lld", label, cur);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    // Libellé vide : on le dessine NOUS-MÊMES juste après. Celui d'ImGui suit le bord
    // du remplissage — il finit donc sur le fond sombre de la jauge, en gris de texte,
    // et devient illisible. Ici : blanc plein, ombre portée (le texte traverse deux
    // fonds de luminosité opposée dès qu'une jauge est à moitié pleine), centré, et
    // remonté de 2 px — le centrage d'ImGui prend la boîte de la police, dont le
    // jambage descendant fait paraître le texte trop bas.
    ImGui::ProgressBar(ratio, ImVec2(-1.0f, 15.0f), "");
    ImGui::PopStyleColor();
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 at((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f - 2.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ro::AddTextRelief(dl, at, IM_COL32_WHITE, text,
                      ro::pal::kTextShadow, ImVec2(1.0f, 1.0f));
  };
  gauge("PV", h.hp, h.max_hp, ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
  gauge("SP", h.sp, h.max_sp, ImVec4(0.20f, 0.35f, 0.70f, 1.0f));
  // ⚠ La fenêtre native imprime ICI l'exp REQUISE (qword_15FF988), pas l'exp courante —
  // seule sa jauge montre le rapport. On affiche les deux, c'est l'information utile.
  if (h.exp_next > 0) {
    gauge(msgstr::Utf8(kMsiHomunExp), h.exp, h.exp_next, ImVec4(0.55f, 0.45f, 0.15f, 1.0f));
  } else {
    ImGui::TextColored(ro::pal::kGreen, "%s : %lld  —  %s", msgstr::Utf8(kMsiHomunExp), h.exp,
                       i18n::Tr("niveau maximum"));
  }
  // Satiété : maximum 100 en dur, comme le natif (et comme le serveur).
  gauge(msgstr::Utf8(kMsiHunger), h.hunger, 100,
        h.hunger <= 10 ? ImVec4(0.75f, 0.20f, 0.20f, 1.0f)
                       : ImVec4(0.35f, 0.60f, 0.25f, 1.0f));
  if (h.hunger <= 10) {
    // Message natif long : replié, sinon il sort de la fenêtre.
    ImGui::PushStyleColor(ImGuiCol_Text, ro::pal::kRed);
    ImGui::TextWrapped("%s", StripRoColors(msgstr::Utf8(kMsiHomunHungry)));
    ImGui::PopStyleColor();
  }

  // Intimité : le palier suffit, la valeur brute passe en infobulle.
  ImGui::TextColored(ro::pal::kValue, "%s :", msgstr::Utf8(kMsiIntimacy));
  ImGui::SameLine();
  ImGui::TextUnformatted(msgstr::Utf8(HomunIntimacyMsgId(h.intimacy)));
  {
    char it[48];
    std::snprintf(it, sizeof(it), "%d / 1000", h.intimacy);
    mui::Tooltip(it);
  }

  // ── Actions : nourrir / auto-alimentation ─────────────────────────────────
  // Nourrir n'a de sens que sur un homoncule ACTIF : le serveur refuse sur un
  // homoncule au repos (hom_is_active), et le natif jette la demande de la même façon.
  ImGui::BeginDisabled(resting);
  if (ro::RoSmallButton(i18n::Tr("Nourrir"), 62.0f, 0.0f)) {
    SendHomunMenu(kHomunCmdFeed);
    homun_status_ = i18n::Tr("Demande de nourrissage envoyée.");
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  bool auto_feed = h.auto_feed;
  if (ro::RoCheckbox(FlattenLabel(msgstr::Utf8(kMsiAutoFeeding)), &auto_feed))
    SendHomunAutoFeed(auto_feed);
  mui::Tooltip(msgstr::Utf8(kMsiAutoFeedInfo));

  if (homun_del_ask_) {
    ImGui::Spacing();
    // ⚠ Ce libellé porte DEUX ^ff0000 : sans StripRoColors ils s'affichent bruts.
    ImGui::PushStyleColor(ImGuiCol_Text, ro::pal::kRed);
    ImGui::TextWrapped("%s", StripRoColors(msgstr::Utf8(kMsiDeleteHomun)));
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, ro::pal::kValue);
    ImGui::TextWrapped("%s", i18n::Tr("Retape le nom de l'homoncule pour confirmer :"));
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##homun_del", homun_del_confirm_, sizeof(homun_del_confirm_));
    const char* expected = h.name[0] ? h.name : h.job;
    const bool ok = expected && expected[0] && std::strcmp(homun_del_confirm_, expected) == 0;
    ImGui::BeginDisabled(!ok);
    if (ro::RoSmallButton(i18n::Tr("Abandonner"), 100.0f, 0.0f)) {
      SendHomunMenu(kHomunCmdDelete);
      rag::homun::NotifyDeleted();  // le ménage que faisait la fenêtre native
      homun_status_ = i18n::Tr("Suppression envoyée.");
      homun_del_ask_ = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ro::RoSmallButton(i18n::Tr("Annuler"), 70.0f, 0.0f)) homun_del_ask_ = false;
  }
  if (!homun_status_.empty()) ImGui::TextColored(ro::pal::kLabel, "%s", homun_status_.c_str());

  ImGui::Spacing();
  ImGui::Separator();

  // ── Statistiques ──────────────────────────────────────────────────────────
  // Mêmes huit lignes que la colonne du natif, dans le même ordre.
  //
  // 🔴 CES HUIT NOMBRES NE SONT PAS CE QU'ILS SEMBLENT. `clif_hominfo` compose
  // certains d'entre eux, et Moonlight étant PRÉ-RENEWAL les DEF/MDEF n'ont pas la
  // sémantique renewal. Vérifié source en main (clif.cpp, battle.cpp branche
  // `#ifndef RENEWAL`, conf/battle) — le détail est dans les infobulles, parce que
  // deux de ces lignes sont carrément trompeuses :
  //   • CRI : `hom_setting` vaut 0x3D, donc HOMSET_DISPLAY_LUK est ACTIF -> le
  //     serveur envoie `LUK/3 + 1`, pas un taux de critique. Et `enable_critical`
  //     vaut 17 (BL_PC|BL_MER) : BL_HOM en est EXCLU, l'homoncule ne critique
  //     JAMAIS. Cette ligne est un indicateur de LUK, rien d'autre.
  //   • DEF : le serveur envoie `DEF1 + VIT`, la somme d'un POURCENTAGE et d'une
  //     statistique. Les deux agissent, mais séparément et pas du tout pareil.
  if (ImGui::BeginTable("cs_homun_stats", 4,
                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
    // Deux paires « libellé / valeur » par ligne. Les libellés prennent la largeur du
    // texte, les valeurs le reste : à 280 px les quatre colonnes se serrent au lieu
    // d'étaler quatre quarts égaux à moitié vides.
    ImGui::TableSetupColumn("l0", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("v0", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("l1", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("v1", ImGuiTableColumnFlags_WidthStretch);
    //
    // Une SEULE de ces valeurs porte son unité : MDEF. C'est la seule dont le
    // nombre envoyé EST directement le pourcentage appliqué (branche
    // `magic_defense_type == 0`, battle.cpp). DEF, elle, est une somme mixte
    // (% + stat) : lui coller un « % » mentirait. CRI n'est pas un taux du tout.
    struct Row { const char* label; int value; const char* tip; bool pct; };
    const Row rows[] = {
        {"ATK", h.atk,
         i18n::Tr("Attaque de base + attaque d'arme (batk + atk2), plafonnée à 32767.\n"
                  "C'est le dégât AVANT défense de la cible.")},
        {"MATK", h.matk,
         i18n::Tr("Attaque magique MAXIMALE. Sur ce serveur HOMSET_SAME_MATK est actif :\n"
                  "le minimum est égal au maximum, il n'y a donc pas de fourchette.")},
        {"HIT", h.hit,
         i18n::Tr("Précision = min(niveau, 600) + DEX.\n"
                  "Chance de toucher = 80 + HIT - FLEE de la cible, bornée à 5..100 %.")},
        {"CRI", h.crit,
         i18n::Tr("/!\\ CE N'EST PAS UN TAUX DE CRITIQUE. Le serveur envoie ici LUK/3 + 1\n"
                  "(option HOMSET_DISPLAY_LUK). Un homoncule ne fait JAMAIS de critique :\n"
                  "enable_critical ne couvre que les joueurs et les mercenaires.\n"
                  "À lire comme un indicateur de LUK — donc ici LUK vaut environ (valeur - 1) x 3.")},
        {"DEF", h.def,
         i18n::Tr("Somme de DEUX choses de natures différentes : DEF dure + VIT.\n"
                  "• DEF dure = réduction en POURCENTAGE (dégâts x (100 - DEF) / 100).\n"
                  "• VIT alimente la DEF douce, une réduction PLATE soustraite ensuite,\n"
                  "  avec une part aléatoire : DEF2 + rnd(0, (DEF2/20)²).\n"
                  "Le nombre affiché ne se lit donc ni en % ni en plat — c'est un total\n"
                  "d'affichage hérité d'Aegis.")},
        {"MDEF", h.mdef,
         i18n::Tr("MDEF dure SEULE, en POURCENTAGE (dégâts magiques x (100 - MDEF) / 100).\n"
                  "/!\\ La MDEF douce (INT + VIT/2), soustraite à plat après, n'est PAS\n"
                  "comptée ici — la résistance réelle est supérieure à ce chiffre."),
         true},
        {"FLEE", h.flee,
         i18n::Tr("Esquive = min(niveau, 600) + AGI. Elle se retranche directement de la\n"
                  "précision de l'attaquant. -10 % par assaillant au-delà du deuxième.\n"
                  "L'esquive parfaite (LUK) n'est pas transmise pour un homoncule.")},
        {"ASPD", rag::AspdFromAmotion(h.amotion), nullptr},  // infobulle bâtie plus bas (valeur brute)
    };
    // ASPD : absente du paquet, le client la CALCULE — son infobulle se bâtit ici.
    char amo[176];
    std::snprintf(amo, sizeof(amo),
                  i18n::Tr("Vitesse d'attaque = (2000 - amotion) / 10, calculée par le "
                           "client.\nDélai d'attaque brut : %d ms — c'est LUI que le "
                           "serveur envoie."),
                  h.amotion);
    for (int i = 0; i < 8; ++i) {
      if (i % 2 == 0) ImGui::TableNextRow();
      const char* tip = rows[i].tip ? rows[i].tip : amo;
      // Libellé collé à sa valeur (colonne au texte), valeur alignée à DROITE de sa
      // moitié : sans ça le libellé étirait sa colonne et laissait un trou au milieu.
      ImGui::TableNextColumn();
      ImGui::TextColored(ro::pal::kLabel, "%s", rows[i].label);
      mui::Tooltip(tip);
      ImGui::TableNextColumn();
      char val[16];
      std::snprintf(val, sizeof(val), rows[i].pct ? "%d %%" : "%d", rows[i].value);
      const float w = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(val).x;
      if (w > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w);
      ImGui::TextUnformatted(val);
      mui::Tooltip(tip);
    }
    ImGui::EndTable();
  }
  {
    char rng[128];
    std::snprintf(rng, sizeof(rng), i18n::Tr("Portée d'attaque : %d"), h.range);
    ImGui::TextColored(ro::pal::kLabel, "%s", rng);
    mui::Tooltip(i18n::Tr("En cases. 1 = corps à corps ; au-delà, l'homoncule frappe à "
                          "distance sans se déplacer."));
  }

  ImGui::Spacing();
  ImGui::Separator();

  // ── Compétences (fenêtre native 114) ──────────────────────────────────────
  static rag::homun::Skill skills[rag::homun::kMaxSkills];
  const int count = rag::homun::ReadSkills(skills, rag::homun::kMaxSkills);

  // Section REPLIABLE : une fois les points dépensés et les compétences posées dans
  // la barre de raccourcis, il n'y a plus rien à venir y faire. L'état est persisté
  // (yaml « charsheet_homun_skills »), sinon la replier ne servirait qu'une session.
  //
  // Le titre porte le compteur de points : replié, c'est la seule chose qui puisse
  // rappeler qu'il reste quelque chose à dépenser.
  char sec[96];
  if (h.skill_points > 0)
    std::snprintf(sec, sizeof(sec), i18n::Tr("Compétences  -  %d point(s)###homun_skills"),
                  h.skill_points);
  else
    std::snprintf(sec, sizeof(sec), i18n::Tr("Compétences###homun_skills"));
  // 🔴 `SetNextItemOpen` à CHAQUE frame, sinon ImGui garde son propre état interne et
  // notre booléen persisté ne pilote rien. Le retour de `CollapsingHeader` le remet à
  // jour : c'est LUI qui fait foi, y compris pour la sauvegarde.
  ImGui::SetNextItemOpen(homun_skills_open_);
  const bool open = ImGui::CollapsingHeader(sec);
  if (open != homun_skills_open_) {
    homun_skills_open_ = open;
    // Le repli est un choix durable, pas un état de frame : on l'écrit tout de suite.
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
  }
  if (!open) return;

  if (count == 0) {
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Aucune compétence reçue du serveur."));
    return;
  }

  auto skill_name = [](int id) -> const char* {
    const char* n = lua::SkillName(id);
    return (n && n[0]) ? n : "?";
  };

  // ⚠ Pas de callback de filtre d'échantillonnage ici : un ImGui table découpe sa
  // draw list en canaux, et une commande de callback n'y garde pas sa place dans
  // l'ordre. La vue liste du Grimoire fait pareil (le filtre n'est posé que sur la
  // grille, dessinée hors table).
  ImGui::BeginChild("cs_homun_skills", ImVec2(0, 0), true);
  // Trois colonnes seulement : le nom, le niveau, le bouton « + ». SP, portée et
  // nature (active / passive) tiennent dans l'infobulle du nom — ce sont des
  // informations qu'on consulte, pas qu'on balaie du regard, et à 280 px chaque
  // colonne se paie sur la place du nom.
  if (ImGui::BeginTable("cs_homun_skill_rows", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                            ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn(i18n::Tr("Compétence"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(i18n::Tr("Niv"));
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();

    for (int i = 0; i < count; ++i) {
      const rag::homun::Skill& s = skills[i];
      ImGui::PushID(s.id);
      ImGui::TableNextRow();

      ImGui::TableNextColumn();
      const ro::IconTex ic = ResolveSkillIcon(s.id);
      const bool active = s.inf != 0 && s.level > 0;
      // Calculé AVANT de soumettre quoi que ce soit : la même valeur doit valoir pour
      // l'icône et pour le nom, sinon celui des deux qui passe après verrait le
      // glisser que l'autre vient d'ouvrir.
      const bool dragging = ImGui::GetDragDropPayload() != nullptr;

      // Les gestes s'attachent au DERNIER widget soumis : cette lambda est donc
      // rappelée après l'icône ET après le nom, pour que la cellule entière réagisse
      // — on attrape une compétence par son icône aussi naturellement que par son nom.
      auto gestures = [&]() {
        // Glisser une compétence ACTIVE vers la barre de raccourcis — même charge
        // « BGN_SKILL » {id, niveau} que le Grimoire et les compétences de guilde. La
        // barre reconnaît l'id d'homoncule et le lance par le chemin qui convient
        // (cf. ragnarok/homunculus.h) ; une passive n'irait nulle part, on ne la
        // laisse donc pas partir.
        //
        // 🔴 `SourceAllowNullID` est OBLIGATOIRE : ni `Image` ni `TextUnformatted` ne
        // déposent d'identifiant ImGui, et `BeginDragDropSource` en exige un — sans ce
        // drapeau il rend false sans rien dire (l'assertion qui l'expliquerait est
        // compilée hors du binaire en Release). Le drapeau en fabrique un depuis la
        // position dans la fenêtre : icône et nom étant à deux endroits, chacun a le
        // sien, et les deux sources ne se marchent pas dessus.
        if (active && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
          const int payload[2] = {s.id, s.level};
          ImGui::SetDragDropPayload("BGN_SKILL", payload, sizeof(payload));
          if (ic.tex) {
            ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(24.0f, 24.0f));
            ImGui::SameLine();
          }
          ImGui::TextUnformatted(skill_name(s.id));
          ImGui::EndDragDropSource();
        }
        if (!ImGui::IsItemHovered()) return;
        // Clic droit = description, double-clic = lancer : la convention des CELLULES
        // du projet. ⚠ Rien ne se déclenche pendant un glisser, sinon relâcher sur la
        // barre lancerait AUSSI la compétence.
        if (!dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
          const ImVec2 mp = ImGui::GetIO().MousePos;
          itemdb::OpenSkillDesc(s.id, static_cast<int>(mp.x), static_cast<int>(mp.y));
        }
        if (active && !dragging && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
          rag::homun::LaunchSkill(s.id, s.level);
        // Une seule infobulle, qui porte TOUT ce que les colonnes ne montrent plus :
        // nature, coût en SP, portée, puis les gestes possibles.
        char tip[256], sp[24], rg[24];
        if (s.sp > 0)    std::snprintf(sp, sizeof(sp), "%d", s.sp);
        else             std::snprintf(sp, sizeof(sp), "-");
        if (s.range > 0) std::snprintf(rg, sizeof(rg), "%d", s.range);
        else             std::snprintf(rg, sizeof(rg), "-");
        std::snprintf(tip, sizeof(tip),
                      active ? i18n::Tr("%s  ·  SP %s  ·  portée %s\n"
                                        "Double-clic : lancer  ·  Glisser : poser dans la barre\n"
                                        "Clic droit : description")
                             : i18n::Tr("%s  ·  SP %s  ·  portée %s\n"
                                        "Clic droit : description"),
                      s.inf == 0 ? i18n::Tr("passif") : i18n::Tr("actif"), sp, rg);
        ImGui::SetTooltip("%s", tip);
      };

      if (ic.tex) {
        ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(20.0f, 20.0f));
        gestures();
        ImGui::SameLine();
      }
      ImGui::TextUnformatted(skill_name(s.id));
      gestures();

      ImGui::TableNextColumn();
      ImGui::Text("%d", s.level);
      ImGui::TableNextColumn();
      // « upgradable » vient du serveur (level < max du tronc de l'espèce) : c'est LA
      // condition, avec les points restants. Le natif, lui, laisse cliquer et le
      // serveur jette en silence. Rien d'affiché quand elle est au maximum : une
      // colonne de « max » ne dit que ce que le bouton absent dit déjà.
      if (s.upgradable && h.skill_points > 0) {
        if (ro::RoSmallButton("+", 22.0f, 0.0f)) {
          SendSkillUp(static_cast<uint16_t>(s.id));  // CZ 0x0112, aiguillé sur hom_skillup
          homun_status_ = i18n::Tr("Montée envoyée.");
        }
        mui::Tooltip(i18n::Tr("Dépense un point (envoi immédiat, comme le natif).\n"
                              "Le serveur exige en plus un niveau d'homoncule minimum, "
                              "que le client ne connaît pas."));
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndChild();
}

// Onglet Guilde : la fenêtre de guilde native (les 7 panneaux UIGuildWnd) refaite en
// ImGui dans la feuille de perso. Tout est lu LIVE des globals du client (CGuild +
// g_GuildInfo_*, cf. project_guild_window_re) ; les actions partent en paquets bruts,
// exactement comme le natif, et le serveur revalide chaque droit.
void CharacterSheet::DrawGuildTab() {

  GuildInfo gi{};
  if (!ReadGuild(&gi)) {
    // Sans guilde : même service que le « Guild Companion » natif (Alt+G), à savoir
    // la création directe, sans passer par ses deux fenêtres.
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Tu n'appartiens à aucune guilde."));
    ImGui::Spacing();
    ImGui::TextWrapped(
        "%s", i18n::Tr("Rejoins-en une (invitation d'un maître de guilde) ou crée la tienne "
              "ici : cet onglet affichera ensuite membres, postes et relations."));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(ro::pal::kBlack, "%s", i18n::Tr("Créer une guilde"));
    ImGui::TextColored(ro::pal::kLabel,
                       "%s", i18n::Tr("Un Emperium dans l'inventaire est nécessaire, et la carte ne "
                             "doit pas interdire les guildes."));
    ImGui::SetNextItemWidth(ro::Px(220.0f));
    const bool name_submitted =
        ro::InputTextCp949("##cs_guild_create", guild_create_buf_, sizeof(guild_create_buf_),
                           ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool create_clicked = ro::RoButton(i18n::Tr("Créer"));
    if ((name_submitted || create_clicked) && guild_create_buf_[0]) {
      SendCreateGuild(guild_create_buf_);
      guild_create_result_ = -1;  // en attente de la réponse serveur
      guild_status_ = std::string(i18n::Tr("Demande envoyée : ")) + guild_create_buf_;
    }
    // Retour du serveur (ZC 0x0167). Le client affiche déjà sa propre boîte ; on
    // double l'information ici pour ne pas laisser l'onglet muet.
    if (guild_create_result_ >= 0) {
      const char* result_text = i18n::Tr("Résultat inconnu.");
      switch (guild_create_result_) {
        case 0: result_text = i18n::Tr("Guilde créée."); break;
        case 1: result_text = i18n::Tr("Tu es déjà dans une guilde."); break;
        case 2: result_text = i18n::Tr("Ce nom de guilde est déjà pris."); break;
        case 3: result_text = i18n::Tr("Il te faut un Emperium pour créer une guilde."); break;
        default: break;
      }
      ImGui::TextColored(guild_create_result_ == 0 ? ImVec4(0.10f, 0.50f, 0.15f, 1.0f)
                                                   : ImVec4(0.60f, 0.12f, 0.12f, 1.0f),
                         "%s", result_text);
    } else if (!guild_status_.empty()) {
      ImGui::TextColored(ro::pal::kLabel, "%s", guild_status_.c_str());
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
    if (ro::RoButton(i18n::Tr("Actualiser"), refresh_width, 0.0f)) guild_last_req_ = 0;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", i18n::Tr("Redemande au serveur membres, postes et infos de guilde."));
    ImGui::SetCursorPos(saved_cursor);
  }
  const float  emblem_size = 48.0f;
  ro::IconTex  emblem = ResolveEmblem(gi.guildId);
  const ImVec2 emblem_min(header_pos.x, header_pos.y + 2.0f);
  const ImVec2 emblem_max(emblem_min.x + emblem_size, emblem_min.y + emblem_size);
  if (emblem.tex) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(emblem_min, emblem_max, IM_COL32(0, 0, 0, 30), 4.0f);
    // Notre liseré ne se pose QUE si le cadre du client n'est pas là : deux
    // bordures concentriques feraient un empilement que personne n'a demandé.
    if (!DrawEmblemBoxed(dl, emblem_min, emblem_max, emblem.tex))
      dl->AddRect(emblem_min, emblem_max, IM_COL32(90, 90, 110, 220), 4.0f, 0, 1.5f);
  }
  // L'emblème lui-même ouvre le changement d'emblème (réservé au maître, comme côté
  // serveur). Bouton posé AVANT les lignes de texte puis curseur restauré : l'en-tête
  // se dispose ensuite comme si la zone cliquable n'existait pas.
  if (is_master) {
    const ImVec2 saved_cursor = ImGui::GetCursorPos();
    ImGui::SetCursorScreenPos(emblem_min);
    if (ImGui::InvisibleButton("##cs_guild_emblem_click", ImVec2(emblem_size, emblem_size))) {
      guild_emblem_ask_ = true;
      // Cliquer l'emblème, c'est vouloir MODIFIER celui-là : l'éditeur s'ouvre dessus
      // (chargé plus bas, à l'ouverture du modal), pas sur une toile vide.
      guild_emblem_load_current_ = true;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::GetWindowDrawList()->AddRect(emblem_min, emblem_max, IM_COL32(255, 220, 120, 255),
                                          4.0f, 0, 2.0f);
      ImGui::SetTooltip("%s", i18n::Tr("Modifier cet emblème dans l'éditeur…"));
    }
    ImGui::SetCursorPos(saved_cursor);
  }
  ImGui::Indent(emblem_size + 10.0f);
  ImGui::TextColored(ro::pal::kBlack, "%s", gi.name);
  ImGui::TextColored(ro::pal::kLabel, i18n::Tr("Niveau %d  ·  Maître : %s"), gi.level,
                     gi.master_name[0] ? gi.master_name : "?");
  // Membres : le total vient du roster (une entrée par membre), le nombre de
  // connectés du paquet d'infos (connect_member).
  ImGui::TextColored(ro::pal::kLabel, i18n::Tr("Membres : %d / %d  ·  En ligne : %d  ·  Niveau moyen : %d"),
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
      if (rights[0] == '\0') std::snprintf(rights, sizeof(rights), i18n::Tr("aucun droit"));
    } else {
      std::snprintf(rights, sizeof(rights), i18n::Tr("droits inconnus"));
    }
    ImGui::TextColored(ro::pal::kLabel, i18n::Tr("Ton poste : %s  ·  %s"),
                       my_position ? my_position : "?", rights);
  }
  ImGui::Unindent(emblem_size + 10.0f);
  if (gi.land[0]) ImGui::TextColored(ro::pal::kLabel, i18n::Tr("Territoire : %s"), gi.land);

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
    ImGui::TextColored(ro::pal::kBlack, "%s", i18n::Tr("Annonce de la guilde — titre, puis message"));
    // Deux champs identiques l'un au-dessus de l'autre : rien ne disait lequel est le
    // titre. L'indice le dit là où on tape, sans voler une ligne de libellé.
    ImGui::SetNextItemWidth(-1.0f);
    ro::InputTextCp949WithHint("##cs_guild_subj", i18n::Tr("Titre de l'annonce"),
                               guild_notice_subj_, sizeof(guild_notice_subj_));
    ImGui::SetNextItemWidth(-1.0f);
    ro::InputTextCp949WithHint("##cs_guild_body", i18n::Tr("Contenu du message"),
                               guild_notice_body_, sizeof(guild_notice_body_));
    if (ro::RoButton(i18n::Tr("Enregistrer"), 110.0f, 0.0f)) {
      SendGuildNotice(gi.guildId, guild_notice_subj_, guild_notice_body_);
      guild_notice_edit_ = false;
      guild_status_ = i18n::Tr("Annonce envoyée.");
    }
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler"), 90.0f, 0.0f)) guild_notice_edit_ = false;
  } else if (gi.notice_subject[0] || gi.notice_body[0]) {
    ImGui::TextColored(ro::pal::kBlue, "%s", gi.notice_subject[0] ? gi.notice_subject : "Annonce");
    if (gi.notice_body[0]) ImGui::TextWrapped("%s", gi.notice_body);
  }
  if (!guild_notice_edit_ && is_master) {
    if (ro::RoButton(i18n::Tr("Modifier l'annonce"))) {
      std::strncpy(guild_notice_subj_, gi.notice_subject, sizeof(guild_notice_subj_) - 1);
      guild_notice_subj_[sizeof(guild_notice_subj_) - 1] = '\0';
      std::strncpy(guild_notice_body_, gi.notice_body, sizeof(guild_notice_body_) - 1);
      guild_notice_body_[sizeof(guild_notice_body_) - 1] = '\0';
      guild_notice_edit_ = true;
    }
    ImGui::SameLine();
    // Doublon volontaire du clic sur l'emblème : personne ne devine qu'une image est
    // cliquable, et c'est ici que se trouvent les autres actions de maître.
    if (ro::RoButton(i18n::Tr("Changer l'emblème…"))) guild_emblem_ask_ = true;
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
    if (ro::RoButton(i18n::Tr("Storage de guilde"))) {
      SendAtCommand(kCmdGuildStorage);
      guild_status_ = i18n::Tr("Storage de guilde : ouverture demandée.");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(may_storage
                            ? i18n::Tr("Ouvre — ou referme — le Storage de guilde (@guildstorage).") : i18n::Tr("Ton poste n'a pas le droit « storage »."));
  }

  ImGui::Separator();

  // ── Sous-onglets Membres / Postes / Relations ─────────────────────────────
  if (ro::RoBeginTabBar("cs_guild_sub")) {
    if (ImGui::BeginTabItem(i18n::Tr("Membres")))     { guild_sub_tab_ = 0; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(i18n::Tr("Postes")))      { guild_sub_tab_ = 2; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(i18n::Tr("Compétences"))) { guild_sub_tab_ = 3; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(i18n::Tr("Relations")))   { guild_sub_tab_ = 1; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(i18n::Tr("Expulsions")))  { guild_sub_tab_ = 4; ImGui::EndTabItem(); }
    ro::RoEndTabBar();
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
      ImGui::TableSetupColumn(i18n::Tr("Nom"), ImGuiTableColumnFlags_WidthStretch |
                                         ImGuiTableColumnFlags_DefaultSort);
      ImGui::TableSetupColumn(i18n::Tr("Classe"), ImGuiTableColumnFlags_WidthFixed, ro::Px(88.0f));
      ImGui::TableSetupColumn(i18n::Tr("Nv"), ImGuiTableColumnFlags_WidthFixed, ro::Px(32.0f));
      ImGui::TableSetupColumn(i18n::Tr("Poste"), ImGuiTableColumnFlags_WidthFixed, ro::Px(84.0f));
      ImGui::TableSetupColumn(i18n::Tr("Contrib."), ImGuiTableColumnFlags_WidthFixed, ro::Px(70.0f));
      ImGui::TableSetupColumn(i18n::Tr("Connexion"), ImGuiTableColumnFlags_WidthFixed, ro::Px(104.0f));
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
                           head_box, m.hair, m.sex, m.hair_color,
                           /*allow_upscale=*/false, m.job);

        // Menu contextuel : actions sur CE membre (le serveur revérifie les droits).
        if (ImGui::BeginPopupContextItem("cs_guild_member_ctx")) {
          guild_sel_cid_ = m.cid;
          ImGui::TextColored(ro::pal::kLabel, "%s", m.name);
          ImGui::Separator();
          if (ImGui::MenuItem(i18n::Tr("Copier le nom"))) ImGui::SetClipboardText(m.name);
          {
            const bool self = !own_name.empty() && _stricmp(m.name, own_name.c_str()) == 0;
            // ── Chuchoter, en DEUX gestes ────────────────────────────────────
            // Réservés aux membres EN LIGNE : chuchoter à un déconnecté ne fait
            // qu'un message d'erreur. Aucun paquet, aucune commande native dans
            // un cas comme dans l'autre.
            //
            // Le premier ne fait que préparer l'envoi — le nom part dans la box
            // destinataire de la barre de chat. Le second, juste en dessous,
            // ouvre la conversation à part : c'est le geste engageant, et il se
            // demande.
            if (m.online && !self && ImGui::MenuItem(i18n::Tr("Chuchoter…"))) {
              ChatWindow* chat = Bourgeon::Instance().chat_window();
              const bool armed = chat != nullptr && chat->TargetWhisper(m.name);
              // Le refus a une seule cause : le chat moderne est éteint (ou sa
              // barre de saisie masquée), donc rien ne serait dessiné. On le dit
              // plutôt que de laisser croire à un clic sans effet.
              guild_status_ = armed
                                  ? std::string(i18n::Tr("Chuchotement à ")) + m.name
                                  : std::string(i18n::Tr("Activez le chat moderne et sa "
                                                "barre de saisie."));
            }
            if (m.online && !self &&
                ImGui::MenuItem(i18n::Tr("Chuchoter dans une fenêtre…"))) {
              ChatWindow* chat = Bourgeon::Instance().chat_window();
              const bool opened =
                  chat != nullptr && chat->OpenWhisperWindowByAid(m.name, m.aid);
              guild_status_ = opened
                                  ? std::string(i18n::Tr("Conversation avec ")) + m.name
                                  : std::string(i18n::Tr("Activez le chat moderne pour "
                                                "les conversations séparées."));
            }
            // ── Inviter dans le groupe ───────────────────────────────────────
            // Grisée plutôt que cachée quand je n'ai pas de groupe : l'entrée
            // reste visible avec sa raison, sinon le joueur cherche pourquoi elle
            // n'y est pas. ⚠ Le serveur exige aussi d'en être le CHEF, ce que le
            // client ne sait pas dire — son refus arrivera avec son propre
            // message, qui est plus précis que tout ce qu'on inventerait.
            ChatWindow* chat_for_invite = Bourgeon::Instance().chat_window();
            if (m.online && !self && chat_for_invite != nullptr) {
              const bool in_party = chat_for_invite->InParty();
              if (!in_party) ImGui::BeginDisabled();
              if (ImGui::MenuItem(i18n::Tr("Inviter dans le groupe"))) {
                // Armée, pas envoyée : le chemin natif est proscrit pendant une
                // frame ImGui. `m.name` est déjà dans la code-page du fil — il
                // sort brut des structures du client.
                chat_for_invite->QueueNameAction(
                    ChatWindow::NameAction::kPartyInvite, m.name);
                guild_status_ = std::string(i18n::Tr("Invitation envoyée à ")) + m.name;
              }
              if (!in_party) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                  ImGui::SetTooltip(
                      "%s", i18n::Tr("Il faut être dans un groupe — et en être le chef — pour "
                            "inviter quelqu'un."));
              }
            }
            // ── Carnet de chasse MVP ─────────────────────────────────────────
            // Même entrée que dans les deux autres menus joueur du projet, et la
            // MÊME implémentation : le grisage et ses raisons vivent en un seul
            // endroit, sinon les trois divergent au premier changement de règle.
            //
            // ⚠ Contrairement à l'invitation de groupe juste au-dessus, celle-ci
            // veut de l'UTF-8 : `m.name` sort brut des structures du client, donc
            // dans la code-page du fil.
            if (!self) DrawMvpInviteMenuItem(ro::WireToUtf8(m.name));

            // « Envoyer un courrier », comme le « Send a mail... » du menu natif :
            // le destinataire part déjà rempli. Jamais vers soi-même (refusé).
            if (!self && ImGui::MenuItem(i18n::Tr("Envoyer un courrier…"))) {
              if (RodexWindow* rodex = Bourgeon::Instance().rodex_window())
                rodex->ComposeTo(m.name);
              guild_status_ = std::string(i18n::Tr("Courrier à ")) + m.name;
            }
          }
          if (is_master && known_count > 0 && m.position_id != 0) {
            if (ImGui::BeginMenu(i18n::Tr("Changer de poste"))) {
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
          // « Transférer la direction » : le seul geste qui envoie la position 0 de
          // CZ 0x0155 — c'est un transfert de guilde, pas un changement de rang, d'où
          // son absence du sous-menu « Changer de poste » (qui part de l'id 1).
          // Le poste 0 identifie le maître aussi sûrement que son nom : master_name
          // peut manquer tant que ZC_GUILD_INFO n'est pas arrivé.
          if (is_master && !target_is_master && !target_is_self && m.position_id != 0) {
            ImGui::Separator();
            // Ouverture DIFFÉRÉE comme l'expulsion : ici la pile d'ID est celle du menu
            // contextuel, un OpenPopup n'y matcherait pas le modal ouvert par l'onglet.
            if (ImGui::MenuItem(i18n::Tr("Transférer la direction…"))) {
              guild_gm_aid_ = m.aid;
              guild_gm_cid_ = m.cid;
              std::strncpy(guild_gm_name_, m.name, sizeof(guild_gm_name_) - 1);
              guild_gm_name_[sizeof(guild_gm_name_) - 1] = '\0';
              guild_gm_confirm_[0] = '\0';
              guild_gm_ask_ = true;
              ImGui::CloseCurrentPopup();
            }
          }
          if (can_expel && !target_is_master && !target_is_self) {
            ImGui::Separator();
            // L'ouverture du modal est DIFFÉRÉE : ici la pile d'ID est celle du menu
            // contextuel (+ le PushID de la ligne), donc un OpenPopup ne matcherait pas
            // le BeginRoPopupModal ouvert au niveau de l'onglet.
            if (ImGui::MenuItem(i18n::Tr("Expulser…"))) {
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
        ImGui::TextColored(ro::pal::kBlack, "%s", JobName(m.job));
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(ro::pal::kBlack, "%d", m.level);
        ImGui::TableSetColumnIndex(3);
        const char* position_label = GuildPositionLabel(m.position_id, m.position);
        if (m.position_id == 0)
          ImGui::TextColored(ro::pal::kBlue, "%s", position_label ? position_label : i18n::Tr("Maître"));
        else
          ImGui::TextColored(ro::pal::kBlack, "%s", position_label ? position_label : "—");
        ImGui::TableSetColumnIndex(4);
        ImGui::TextColored(ro::pal::kBlack, "%d", m.contribution);
        ImGui::TableSetColumnIndex(5);
        if (m.online) {
          ImGui::TextColored(ro::pal::kGreen, "%s", i18n::Tr("En ligne"));
        } else {
          char seen[32] = "—";
          if (m.last_login != 0) {
            const time_t stamp = static_cast<time_t>(m.last_login);
            struct tm local_time = {};
            if (localtime_s(&local_time, &stamp) == 0)
              std::strftime(seen, sizeof(seen), "%d/%m/%y %H:%M", &local_time);
          }
          ImGui::TextColored(ro::pal::kLabel, "%s", seen);
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
      ImGui::TextColored(pass == 0 ? ro::pal::kGreen : ro::pal::kRed, pass == 0 ? i18n::Tr("Alliés") : "Ennemis");
      int shown = 0;
      for (int i = 0; i < relations.count; ++i) {
        const GuildRelation& rel = relations.entries[i];
        if (rel.relation != pass) continue;
        const char* break_label =
            (pass == 0) ? i18n::Tr("Rompre l'alliance…") : i18n::Tr("Retirer l'hostilité…");
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
            ImGui::TextColored(ro::pal::kLabel, "%s", rel.name);
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
      if (shown == 0) ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("   aucune"));
      ImGui::Spacing();
    }
    ImGui::EndChild();
  }

  // ── Barre d'actions ───────────────────────────────────────────────────────
  // (« Actualiser » vit en haut à droite de l'en-tête, pas ici.)
  // Invitation : soumise au droit « inviter » du poste, comme côté serveur.
  if (can_invite) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ro::pal::kBlack, "%s", i18n::Tr("Inviter :"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ro::Px(130.0f));
    ro::InputTextCp949("##cs_guild_invite", guild_invite_buf_, sizeof(guild_invite_buf_));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", i18n::Tr("Nom exact du personnage à inviter (il doit être connecté)."));
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Inviter")) && guild_invite_buf_[0]) {
      SendGuildInvite(guild_invite_buf_);
      guild_status_ = std::string(i18n::Tr("Invitation envoyée à ")) + guild_invite_buf_;
      guild_invite_buf_[0] = '\0';
    }
    ImGui::SameLine();
  }
  // Libellé différent du titre du modal : bouton et popup partagent sinon le même ID.
  if (ro::RoButton(i18n::Tr("Quitter…"))) {
    guild_reason_buf_[0] = '\0';
    ImGui::OpenPopup(i18n::Tr("Quitter la guilde###bourgeon_guild_leave"));
  }
  // Dissolution : réservée au maître, comme la commande (gmaster_flag côté serveur).
  if (is_master) {
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Dissoudre…"))) {
      guild_break_confirm_[0] = '\0';
      ImGui::OpenPopup(i18n::Tr("Dissoudre la guilde###bourgeon_guild_disband"));
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", i18n::Tr("Supprime définitivement la guilde (@breakguild)."));
  }
  if (!guild_status_.empty()) ImGui::TextColored(ro::pal::kLabel, "%s", guild_status_.c_str());

  // ── Confirmations ─────────────────────────────────────────────────────────
  // Demande d'expulsion venue du menu contextuel : on ouvre ICI, au niveau de
  // l'onglet, pour que l'ID matche celui du modal.
  if (guild_expel_ask_) {
    guild_expel_ask_ = false;
    ImGui::OpenPopup(i18n::Tr("Expulser de la guilde###bourgeon_guild_expel"));
  }
  if (guild_rel_del_ask_) {
    guild_rel_del_ask_ = false;
    ImGui::OpenPopup(i18n::Tr("Rompre la relation###bourgeon_guild_relation"));
  }
  if (guild_gm_ask_) {
    guild_gm_ask_ = false;
    ImGui::OpenPopup(i18n::Tr("Transférer la direction###bourgeon_guild_gm"));
  }
  // Le dossier est relu à CHAQUE ouverture : on y dépose justement un fichier juste
  // avant de venir le choisir.
  if (guild_emblem_ask_) {
    guild_emblem_ask_ = false;
    ScanEmblemFolder();
    guild_emblem_sel_ = -1;
    guild_emblem_error_.clear();
    // Ouverture par un clic sur l'emblème : on repart de celui qui est en place. Un
    // échec (fichier pas encore descendu, format inattendu) laisse le dessin en cours
    // et le dit, plutôt que d'ouvrir un éditeur muet.
    if (guild_emblem_load_current_) {
      guild_emblem_load_current_ = false;
      const std::vector<uint8_t> current = CurrentEmblemBmp(gi.guildId);
      if (!current.empty() && EmblemCanvasLoadBmp(current)) {
        guild_emblem_goto_paint_ = true;  // sinon le modal s'ouvre sur la liste de fichiers
        guild_emblem_diag_ = i18n::Tr("Emblème actuel de la guilde chargé dans l'éditeur.");
      } else {
        guild_emblem_diag_ =
            i18n::Tr("Emblème actuel illisible (pas encore téléchargé ?) : le dessin en cours est gardé.");
      }
    }
    ImGui::OpenPopup(i18n::Tr("Changer l'emblème###bourgeon_guild_emblem"));
  }
  DrawGuildEmblemModal(gi.guildId, is_master);

  if (ro::BeginRoPopupModal(i18n::Tr("Rompre la relation###bourgeon_guild_relation"))) {
    if (guild_rel_del_kind_ == 0)
      ImGui::Text(i18n::Tr("Rompre l'alliance avec %s ?"), guild_rel_del_name_);
    else
      ImGui::Text(i18n::Tr("Retirer %s de la liste des ennemis ?"), guild_rel_del_name_);
    ImGui::TextColored(ro::pal::kLabel,
                       "%s", i18n::Tr("Refusé par le serveur pendant une guerre de guildes\n"
                             "et sur les cartes verrouillées."));
    ImGui::Spacing();
    if (ro::RoButton(guild_rel_del_kind_ == 0 ? i18n::Tr("Rompre") : i18n::Tr("Retirer"), 110.0f, 0.0f)) {
      SendGuildDeleteRelation(guild_rel_del_id_, guild_rel_del_kind_);
      guild_status_ = std::string(guild_rel_del_name_) + i18n::Tr(" : demande envoyée.");
      guild_last_req_ = 0;  // rafraîchit la liste des relations
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler"), 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  if (ro::BeginRoPopupModal(i18n::Tr("Quitter la guilde###bourgeon_guild_leave"))) {
    ImGui::TextUnformatted(i18n::Tr("Quitter définitivement la guilde ?"));
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Il faudra une nouvelle invitation pour y revenir."));
    ImGui::Spacing();
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Motif (facultatif) :"));
    ImGui::SetNextItemWidth(ro::Px(240.0f));
    ro::InputTextCp949("##cs_guild_leave_reason", guild_reason_buf_,
                       sizeof(guild_reason_buf_));
    ImGui::Spacing();
    if (ro::RoButton(i18n::Tr("Quitter"), 110.0f, 0.0f)) {
      SendGuildLeaveOrExpel(kOpGuildLeave, gi.guildId,
                            static_cast<uint32_t>(
                                Bourgeon::Instance().client().session().aid()),
                            static_cast<uint32_t>(ReadInt(rag::kOwnCharIdAddr)), guild_reason_buf_);
      guild_status_ = i18n::Tr("Départ envoyé.");
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler"), 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  // ⚠ @breakguild n'a NI argument NI confirmation : il appelle guild_break() sur-le-champ.
  // Le serveur ne posera donc aucune question — le garde-fou du nom retapé est le seul
  // qui existe, à l'image de ce que demande la fenêtre native pour dissoudre.
  if (ro::BeginRoPopupModal(i18n::Tr("Dissoudre la guilde###bourgeon_guild_disband"))) {
    ImGui::TextColored(ro::pal::kRed, i18n::Tr("Dissoudre « %s » ?"), gi.name);
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Irréversible : la guilde, ses postes, son storage et ses\n"
                                    "compétences disparaissent."));
    ImGui::Spacing();
    // Conditions RÉELLES de guild_break() : les dire AVANT évite un clic qui échoue,
    // d'autant que deux des trois refus sont peu bavards côté client.
    ImGui::TextColored(ro::pal::kBlack, "%s", i18n::Tr("Le serveur refusera si :"));
    ImGui::BulletText("%s", i18n::Tr("tu n'es pas le maître de guilde ;"));
    ImGui::BulletText("%s", i18n::Tr("il reste un autre membre — il faut être SEUL ;"));
    ImGui::BulletText("%s", i18n::Tr("la carte interdit les modifications de guilde\n(mapflag guildlock) ;"));
    ImGui::BulletText("%s", i18n::Tr("une instance de guilde est en cours."));
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("L'instance fait échouer la dissolution SANS aucun message."));
    ImGui::Spacing();
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Retape le nom de la guilde pour confirmer :"));
    ImGui::SetNextItemWidth(ro::Px(240.0f));
    // Indice STATIQUE, pas gi.name : le nom vient du client en CP949, et l'indice est
    // rendu en UTF-8. La comparaison, elle, se fait bien CP949 contre CP949.
    ro::InputTextCp949WithHint("##cs_guild_break", i18n::Tr("Nom exact de la guilde"),
                               guild_break_confirm_, sizeof(guild_break_confirm_));
    ImGui::Spacing();
    const bool name_matches = gi.name[0] && std::strcmp(guild_break_confirm_, gi.name) == 0;
    ImGui::BeginDisabled(!name_matches);
    if (ro::RoButton(i18n::Tr("Dissoudre"), 110.0f, 0.0f)) {
      SendAtCommand(kCmdBreakGuild);
      guild_status_ = i18n::Tr("Dissolution demandée.");
      guild_break_confirm_[0] = '\0';
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler"), 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  if (ro::BeginRoPopupModal(i18n::Tr("Expulser de la guilde###bourgeon_guild_expel"))) {
    ImGui::Text(i18n::Tr("Expulser %s de la guilde ?"), guild_expel_name_);
    ImGui::Spacing();
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Motif (facultatif) :"));
    ImGui::SetNextItemWidth(ro::Px(240.0f));
    ro::InputTextCp949("##cs_guild_expel_reason", guild_reason_buf_,
                       sizeof(guild_reason_buf_));
    ImGui::Spacing();
    if (ro::RoButton(i18n::Tr("Expulser"), 110.0f, 0.0f)) {
      SendGuildLeaveOrExpel(kOpGuildExpel, gi.guildId, guild_expel_aid_, guild_expel_cid_,
                            guild_reason_buf_);
      guild_status_ = std::string(guild_expel_name_) + i18n::Tr(" : expulsion envoyée.");
      guild_last_req_ = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler"), 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  // Le transfert est SANS RETOUR depuis le client : le paquet parti, on n'est plus
  // maître, et seul le nouveau peut rendre la direction. Même garde-fou que pour la
  // dissolution — le nom retapé.
  if (ro::BeginRoPopupModal(i18n::Tr("Transférer la direction###bourgeon_guild_gm"))) {
    ImGui::TextColored(ro::pal::kRed, i18n::Tr("Faire de %s le maître de « %s » ?"),
                       guild_gm_name_, gi.name);
    ImGui::TextColored(ro::pal::kLabel,
                       "%s", i18n::Tr("Tu perds le poste de maître sur-le-champ : postes, emblème,\n"
                             "annonce, invitations et dissolution passent à ce membre.\n"
                             "Lui seul pourra te rendre la direction."));
    ImGui::Spacing();
    ImGui::TextColored(ro::pal::kBlack, "%s", i18n::Tr("Le serveur refusera si :"));
    ImGui::BulletText("%s", i18n::Tr("une guerre de guildes est en cours ;"));
    ImGui::BulletText("%s", i18n::Tr("le précédent transfert est trop récent (délai serveur) ;"));
    ImGui::BulletText("%s", i18n::Tr("une instance de guilde est en cours — refus SANS message."));
    ImGui::Spacing();
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Retape le nom du membre pour confirmer :"));
    ImGui::SetNextItemWidth(ro::Px(240.0f));
    // Indice STATIQUE, pas guild_gm_name_ : le nom vient du client en CP949 et l'indice
    // est rendu en UTF-8. La comparaison, elle, reste CP949 contre CP949.
    ro::InputTextCp949WithHint("##cs_guild_gm", i18n::Tr("Nom exact du membre"),
                               guild_gm_confirm_, sizeof(guild_gm_confirm_));
    ImGui::Spacing();
    const bool gm_name_matches =
        guild_gm_name_[0] && std::strcmp(guild_gm_confirm_, guild_gm_name_) == 0;
    ImGui::BeginDisabled(!gm_name_matches);
    if (ro::RoButton(i18n::Tr("Transférer"), 110.0f, 0.0f)) {
      SendGuildChangeMaster(guild_gm_aid_, guild_gm_cid_);
      guild_status_ =
          std::string(guild_gm_name_) + i18n::Tr(" : transfert de la direction envoyé.");
      guild_gm_confirm_[0] = '\0';
      guild_last_req_ = 0;  // le roster ET le drapeau maître changent
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler"), 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }
}

// Sous-onglet « Expulsions » : les exclusions mémorisées par le serveur (ZC 0x0b7c).
// Purement informatif : rien ne permet de réintégrer quelqu'un depuis le client.
void CharacterSheet::DrawGuildBansTab() {
  if (!guild_bans_known_) {
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Liste non encore reçue — clic sur « Actualiser »."));
    return;
  }
  if (guild_bans_.empty()) {
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Aucune expulsion enregistrée."));
    return;
  }

  const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                      ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
  if (!ImGui::BeginTable("cs_guild_bans_tbl", 2, table_flags)) return;
  ImGui::TableSetupColumn(i18n::Tr("Personnage"), ImGuiTableColumnFlags_WidthFixed, ro::Px(130.0f));
  ImGui::TableSetupColumn(i18n::Tr("Motif"), ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableHeadersRow();
  for (const GuildBanRow& ban : guild_bans_) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(ban.name[0] ? ban.name : "?");
    ImGui::TableNextColumn();
    if (ban.reason[0]) ImGui::TextUnformatted(ban.reason);
    else               ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("(aucun motif)"));
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
  EnsureGuildSkillTree();
  if (!guild_skills_known_) {
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Compétences non encore reçues — clic sur « Actualiser »."));
    return;
  }

  ImGui::Text(i18n::Tr("Points de compétence : %d"), guild_skill_points_);
  ImGui::SameLine();
  if (ro::RoButton(guild_skill_grid_ ? i18n::Tr("Grille") : i18n::Tr("Liste"), 58.0f, 0.0f))
    guild_skill_grid_ = !guild_skill_grid_;
  mui::Tooltip(i18n::Tr("Bascule entre la grille d'icônes et la liste détaillée\n"
               "(niveau, SP, lancer, monter) — comme le Grimoire."));
  ImGui::SameLine();
  ImGui::TextDisabled("%s", i18n::Tr("Raccourcis"));
  mui::Tooltip(
      i18n::Tr("Clic gauche      monter d'un niveau (envoyé aussitôt au serveur)\n"
      "Double-clic      lancer la compétence\n"
      "Clic droit       description de la compétence\n"
      "Glisser          pose la compétence sur une barre d'action\n"
      "Survol           flèches de prérequis (ambre) et de suites (bleu)\n"
      "\n"
      "Pas de « Appliquer » ici : contrairement au Grimoire, une compétence de\n"
      "guilde se monte immédiatement."));
  // Même préférence que le Grimoire (une seule « icônes de skill »), et seulement en
  // grille : en liste l'icône fait une hauteur de ligne, le filtre ne se voit pas.
  if (guild_skill_grid_) {
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ro::RoCheckbox(i18n::Tr("Lisser les icônes"), &skill_bilinear_)) {
      if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
    }
    mui::Tooltip(i18n::Tr("Filtrage bilinéaire des icônes de la grille.\n"
                 "Décoché (défaut) = pixels nets, comme le client natif, qui ne filtre pas."));
  }

  // Sans l'arbre on ne peut montrer que ce que le serveur envoie ; avec, les
  // verrouillées apparaissent aussi, donc la liste n'est jamais vide.
  if (guild_skill_tree_state_ != 1 && guild_skills_.empty()) {
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Aucune compétence disponible (prérequis non remplis)."));
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

  auto tree_node = [this](uint16_t id) -> const GuildSkillTreeNode* {
    for (const GuildSkillTreeNode& node : guild_skill_tree_)
      if (node.id == id) return &node;
    return nullptr;
  };
  // Nom : d'abord le Lua du client (localisé), qui ne connaît PAS les compétences de
  // guilde ; puis le libellé du fichier d'arbre — seule source pour une verrouillée,
  // dont aucun paquet n'arrive ; enfin le nom technique du paquet.
  auto skill_label = [&](uint16_t id, const char* packet_name) -> const char* {
    const char* lua_name = lua::SkillName(id);
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
      text += i18n::Tr(" Niv ");
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
  // Infobulle commune aux deux vues : la grille n'affiche que l'icône, tout le reste
  // (niveau, portée, prérequis manquants, nature) se lit ici.
  auto tooltip_for = [&](const SkillRowView& row, const GuildSkillRow* live, int level,
                         bool locked) {
    const char* label = skill_label(row.id, live ? live->name : nullptr);
    const bool active_skill = live && live->inf != 0 && level > 0;
    const bool can_use = active_skill && (live->inf & 0x04) != 0;
    std::string tip = label;
    if (live && live->name[0]) { tip += "  ("; tip += live->name; tip += ")"; }
    if (row.max_level > 0) tip += i18n::Tr("\nNiveau ") + std::to_string(level) + " / " +
                                  std::to_string(row.max_level);
    if (live && live->range > 0) tip += i18n::Tr("\nPortée : ") + std::to_string(live->range);
    const unsigned long cd_ms = SkillCooldownRemaining(row.id);
    if (cd_ms > 0)
      tip += i18n::Tr("\nEncore ") + std::to_string((cd_ms + 999) / 1000) +
             i18n::Tr(" s (lancer une compétence de guilde les bloque toutes les quatre)");
    // Les prérequis sont ce qui manque justement à une verrouillée : les dire ICI,
    // là où le joueur regarde quand il se demande pourquoi elle est grisée.
    const std::string reqs = requirements_text(row.node);
    if (!reqs.empty()) tip += i18n::Tr("\nRequiert : ") + reqs;
    if (live)
      tip += live->inf == 0 ? i18n::Tr("\nPassive (toujours active)") : i18n::Tr("\nActive");
    tip += locked       ? i18n::Tr("\n\nVerrouillée : prérequis non remplis.")
         : can_use      ? i18n::Tr("\n\nDouble-clic : lancer — clic droit : description — glisser vers une barre")
         : active_skill ? i18n::Tr("\n\nClic droit : description — glisser vers une barre") : i18n::Tr("\n\nClic droit : description");
    if (live && live->upgradable && guild_skill_points_ > 0)
      tip += i18n::Tr("\nClic gauche : monter d'un niveau");
    ImGui::SetTooltip("%s", tip.c_str());
  };

  // ── Vue GRILLE : la même que le Grimoire, sans onglets de classe ─────────────
  // Les compétences de guilde n'ont PAS d'index de case (le Lua du client ne les
  // connaît pas) : la grille est donc construite depuis l'arbre lui-même — une ligne
  // par palier de profondeur, les compétences d'un même palier côte à côte. C'est la
  // disposition que l'indentation de la liste dessinait déjà, en deux dimensions.
  if (guild_skill_grid_) {
    // Zone à part, comme le Grimoire : la grille est peinte en coordonnées absolues,
    // c'est l'enfant qui lui donne un cadre et un scroll.
    ImGui::BeginChild("cs_guild_skill_grid", ImVec2(0, 0), false);
    const float cell_w = 72.0f, cell_h = 78.0f, icon_sz = 40.0f, pad = 10.0f, icon_y = 7.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const int n = static_cast<int>(rows.size());
    std::vector<ImVec2> center(rows.size(), ImVec2(-1.0f, -1.0f));
    int hover_idx = -1, used_rows = 0;

    // Filtre d'échantillonnage : même réglage que le Grimoire (une seule préférence
    // « icônes de skill »), et posé dans les deux cas — l'ambiant est LINEAR.
    g_skill_icon_bilinear = skill_bilinear_;
    dl->AddCallback(CbSkillIconFilter, nullptr);

    int col_of_depth[16] = {};
    for (int i = 0; i < n; ++i) {
      const SkillRowView& row = rows[i];
      const GuildSkillRow* live = row.live;
      const bool locked = live == nullptr;
      const int  level  = live ? live->level : 0;
      const int  d      = (row.depth >= 0 && row.depth < 16) ? row.depth : 15;
      const int  col    = col_of_depth[d]++;
      used_rows = std::max(used_rows, d + 1);
      const ImVec2 p(origin.x + col * cell_w, origin.y + d * cell_h);
      const ImVec2 q(p.x + cell_w - pad, p.y + cell_h - pad);
      center[i] = ImVec2((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);

      ImGui::PushID(static_cast<int>(row.id));
      ImGui::SetCursorScreenPos(p);
      ImGui::InvisibleButton("gcell", ImVec2(cell_w - pad, cell_h - pad));
      const bool hot = ImGui::IsItemHovered();
      if (hot) hover_idx = i;
      if (hot) dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, 28), 4.0f);
      if (row.id == focus)              dl->AddRect(p, q, IM_COL32(255, 205, 105, 220), 4.0f, 2.0f);
      else if (linked_to_focus(row.id)) dl->AddRect(p, q, IM_COL32(120, 160, 255, 200), 4.0f, 2.0f);
      else if (live && live->upgradable && guild_skill_points_ > 0)
        dl->AddRect(p, q, IM_COL32(120, 200, 130, 170), 4.0f, 1.5f);

      const ro::IconTex ic = ResolveSkillIcon(row.id);
      const ImVec2 ip(p.x + (cell_w - pad - icon_sz) * 0.5f, p.y + icon_y);
      if (ic.tex)
        dl->AddImage(reinterpret_cast<ImTextureID>(ic.tex), ip,
                     ImVec2(ip.x + icon_sz, ip.y + icon_sz), ImVec2(0, 0), ImVec2(1, 1),
                     locked ? IM_COL32(105, 105, 115, 165) : IM_COL32_WHITE);
      // Cooldown en cours (les compétences de guilde se bloquent toutes les quatre
      // ensemble, plusieurs minutes durant : c'est LE cas où le voile sert).
      if (!locked) DrawSkillCooldownOverlay(dl, row.id, ip, icon_sz);

      char lvl[24];
      if (row.max_level > 0) std::snprintf(lvl, sizeof(lvl), "%d/%d", level, row.max_level);
      else                   std::snprintf(lvl, sizeof(lvl), "%d", level);
      const ImVec2 lsz = ImGui::CalcTextSize(lvl);
      const ImVec2 lp(p.x + (cell_w - pad - lsz.x) * 0.5f, p.y + icon_y + icon_sz + 2.0f);
      const ImU32 halo = IM_COL32(255, 255, 255, 190);
      const ImU32 lvl_col = level > 0 ? IM_COL32(20, 20, 25, 255) : IM_COL32(110, 110, 122, 255);
      dl->AddText(ImVec2(lp.x - 1.0f, lp.y), halo, lvl);
      dl->AddText(ImVec2(lp.x + 1.0f, lp.y), halo, lvl);
      dl->AddText(ImVec2(lp.x, lp.y - 1.0f), halo, lvl);
      dl->AddText(ImVec2(lp.x, lp.y + 1.0f), halo, lvl);
      dl->AddText(lp, lvl_col, lvl);

      const bool active_skill = live && live->inf != 0 && level > 0;
      const bool can_use = active_skill && (live->inf & 0x04) != 0;
      if (active_skill && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        const int payload[2] = {static_cast<int>(row.id), level};
        ImGui::SetDragDropPayload("BGN_SKILL", payload, sizeof(payload));
        if (ic.tex) { ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(24.0f, 24.0f));
                      ImGui::SameLine(); }
        ImGui::TextUnformatted(skill_label(row.id, live ? live->name : nullptr));
        ImGui::EndDragDropSource();
      }
      if (hot && ImGui::GetDragDropPayload() == nullptr) {
        hovered_now = row.id;
        tooltip_for(row, live, level, locked);
      }
      // Clic simple = monter d'un niveau, double-clic = lancer. Ici le « monter » part
      // AUSSITÔT au serveur (pas de réservation comme au Grimoire), donc la levée
      // d'ambiguïté n'est pas un confort mais une nécessité : sans elle, un double-clic
      // pour lancer dépenserait un point au passage. `IsMouseReleasedWithDelay` +
      // `MouseClickedLastCount == 1` est l'idiome ImGui prévu pour ça.
      const ImGuiIO& gio = ImGui::GetIO();
      static uint16_t g_press_id = 0;
      static bool g_press_clean = false;
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        g_press_id = row.id;
        if (gio.MouseClickedLastCount[ImGuiMouseButton_Left] <= 1) g_press_clean = false;
      }
      if (g_press_id == row.id) {
        // GetMouseDragDelta ne vaut plus rien passé la frame du relâché : verdict pris ici.
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
          const ImVec2 travel = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
          g_press_clean = travel.x == 0.0f && travel.y == 0.0f &&
                          ImGui::GetDragDropPayload() == nullptr;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && g_press_clean) {
          if (can_use) SendUseSkill(row.id, level);
          g_press_id = 0;
        } else if (ImGui::IsMouseReleasedWithDelay(ImGuiMouseButton_Left,
                                                   gio.MouseDoubleClickTime) &&
                   gio.MouseClickedLastCount[ImGuiMouseButton_Left] == 1 && g_press_clean && live &&
                   live->upgradable && guild_skill_points_ > 0) {
          SendSkillUp(row.id);
          guild_last_req_ = 0;  // on laisse le 0x0162 suivant corriger niveau et points
          g_press_id = 0;
        }
      }
      // Clic droit = description (avec ou sans Ctrl : ici il n'y a pas de menu
      // contextuel à départager, contrairement au Grimoire).
      if (hot && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        itemdb::OpenSkillDesc(row.id, static_cast<int>(mp.x), static_cast<int>(mp.y));
      }
      ImGui::PopID();
    }
    dl->AddCallback(CbSkillIconFilterOff, nullptr);

    // ── Flèches au survol, en chaîne dans les deux sens (comme le Grimoire) ────
    if (hover_idx >= 0) {
      auto draw_arrow = [&](const ImVec2& from, const ImVec2& to, ImU32 col, float thickness) {
        ImVec2 d(to.x - from.x, to.y - from.y);
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        const float trim = (cell_w - pad) * 0.45f;
        if (len <= trim * 2.0f + 8.0f) return;
        d.x /= len;
        d.y /= len;
        const ImVec2 a(from.x + d.x * trim, from.y + d.y * trim);
        const ImVec2 b(to.x - d.x * trim, to.y - d.y * trim);
        dl->AddLine(a, b, col, thickness);
        const float head = 8.0f;
        const ImVec2 nrm(-d.y, d.x);
        dl->AddTriangleFilled(
            b,
            ImVec2(b.x - d.x * head + nrm.x * head * 0.5f, b.y - d.y * head + nrm.y * head * 0.5f),
            ImVec2(b.x - d.x * head - nrm.x * head * 0.5f, b.y - d.y * head - nrm.y * head * 0.5f),
            col);
      };
      // Réduction transitive : le trait direct saute si un autre prérequis du même
      // nœud réclame déjà celui-là — le chemin est de toute façon à l'écran.
      auto redundant_edge = [&](uint16_t dep_id, uint16_t req_id) {
        const GuildSkillTreeNode* node = tree_node(dep_id);
        if (!node) return false;
        for (const GuildSkillReq& other : node->need) {
          if (other.id == req_id) continue;
          if (depends_on(other.id, req_id)) return true;
        }
        return false;
      };
      std::vector<int> queue, depth_q;
      std::vector<bool> seen(rows.size(), false);
      auto walk_chain = [&](bool upstream, int cr, int cg, int cb, int alpha0, float thick0) {
        std::fill(seen.begin(), seen.end(), false);
        queue.clear();
        depth_q.clear();
        queue.push_back(hover_idx);
        depth_q.push_back(0);
        seen[hover_idx] = true;
        for (size_t qi = 0; qi < queue.size(); ++qi) {
          const int cur = queue[qi];
          const int depth = depth_q[qi];
          if (depth >= 6) continue;
          for (int i = 0; i < n; ++i) {
            if (i == cur) continue;
            const int dep = upstream ? cur : i;  // celui qui réclame
            const int req = upstream ? i : cur;  // celui qui est réclamé
            if (!depends_on(rows[dep].id, rows[req].id)) continue;
            if (redundant_edge(rows[dep].id, rows[req].id)) continue;
            draw_arrow(center[req], center[dep],
                       IM_COL32(cr, cg, cb, std::max(85, alpha0 - depth * 35)),
                       std::max(1.2f, thick0 - depth * 0.25f));
            if (!seen[i]) {
              seen[i] = true;
              queue.push_back(i);
              depth_q.push_back(depth + 1);
            }
          }
        }
      };
      walk_chain(true, 255, 205, 105, 235, 2.5f);   // ambre : ce qu'il faut avant
      walk_chain(false, 120, 160, 255, 200, 2.0f);  // bleu  : ce que ça ouvre
    }

    // Étendue réservée : la grille est peinte en absolu, sans ça pas de scroll.
    ImGui::SetCursorScreenPos(origin);
    int widest = 1;
    for (int d = 0; d < 16; ++d) widest = std::max(widest, col_of_depth[d]);
    ImGui::Dummy(ImVec2(widest * cell_w, used_rows * cell_h));
    ImGui::EndChild();
    guild_skill_hover_ = hovered_now;
    return;
  }

  // ── Vue LISTE : l'arbre indenté, avec ses coudes de liaison ─────────────────
  const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                      ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
  // Pas de colonne « Requiert » : les prérequis sont déjà dans le tooltip et dessinés
  // en liens ; une 3e redite volait la largeur au nom, qui se retrouvait tronqué.
  if (!ImGui::BeginTable("cs_guild_skills_tbl", 5, table_flags)) return;
  ImGui::TableSetupColumn(i18n::Tr("Compétence"), ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn(i18n::Tr("Level"), ImGuiTableColumnFlags_WidthFixed, ro::Px(54.0f));
  // Assez large pour « Passif » : cette colonne porte le coût OU la nature de la
  // compétence, exactement comme le natif qui écrit « Passive » à la place du SP.
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ro::Px(56.0f));
  // Colonne « lancer » élargie : elle porte aussi le décompte de cooldown (« 4:12 »).
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ro::Px(46.0f));  // lancer
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ro::Px(30.0f));  // monter
  ImGui::TableHeadersRow();

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
    if (locked) ImGui::PushStyleColor(ImGuiCol_Text, ro::pal::kLabel);
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
    if (mui::IsLastItemRightClicked()) {
      const ImVec2 mp = ImGui::GetIO().MousePos;
      itemdb::OpenSkillDesc(row.id, static_cast<int>(mp.x), static_cast<int>(mp.y));
    }
    if (ImGui::IsItemHovered()) {
      hovered_now = row.id;  // consommé à la frame suivante (liens + surlignage)
      tooltip_for(row, live, level, locked);
    }
    ImGui::Unindent(row_indent);

    // ── Niveau : « 3/10 » dès que le max est connu (il vient du fichier, pas du paquet) ──
    ImGui::TableNextColumn();
    if (row.max_level > 0) {
      if (level > 0) ImGui::Text("%d/%d", level, row.max_level);
      else           ImGui::TextColored(ro::pal::kLabel, "-/%d", row.max_level);
    } else if (level > 0) {
      ImGui::Text("%d", level);
    } else {
      ImGui::TextColored(ro::pal::kLabel, "-");
    }

    // ── SP, ou « Passif » ──────────────────────────────────────────────────────
    // `inf` (skill_get_inf, envoyé par le serveur) vaut 0 pour une passive. C'était
    // jusqu'ici invisible : rien ne distinguait une passive d'une active, il fallait
    // ouvrir la description ou tenter le drag pour le découvrir.
    ImGui::TableNextColumn();
    if (live && live->inf == 0)    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Passif"));
    else if (live && live->sp > 0) ImGui::Text("%d", live->sp);
    else                           ImGui::TextColored(ro::pal::kLabel, "-");

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
        ImGui::SetTooltip(i18n::Tr("Encore %lu s.\nLancer une compétence de guilde les bloque toutes les quatre."),
                          (cd_ms + 999) / 1000);
      else if (can_use)                 ImGui::SetTooltip("%s", i18n::Tr("Lancer (ou double-clic sur le nom)."));
      else if (live && live->inf == 0)  ImGui::SetTooltip("%s", i18n::Tr("Compétence passive : rien à lancer."));
      else if (locked || level == 0)    ImGui::SetTooltip("%s", i18n::Tr("Non apprise."));
      else                              ImGui::SetTooltip("%s", i18n::Tr("Se lance sur une cible : à glisser dans une barre."));
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
      if (can_up)                        ImGui::SetTooltip("%s", i18n::Tr("Monter d'un niveau."));
      else if (locked)                   ImGui::SetTooltip("%s", i18n::Tr("Prérequis non remplis."));
      else if (guild_skill_points_ <= 0) ImGui::SetTooltip("%s", i18n::Tr("Aucun point de compétence disponible."));
      else                               ImGui::SetTooltip("%s", i18n::Tr("Niveau maximum, ou réservé au maître de guilde."));
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

  if (!guild_positions_editing_) {
    for (int i = 0; i < kGuildPositionSlots; ++i) guild_positions_edit_[i] = guild_positions_[i];
  }

  bool any_info = false;
  for (int i = 0; i < kGuildPositionSlots; ++i)
    if (guild_positions_[i].has_name || guild_positions_[i].has_info) { any_info = true; break; }
  if (!any_info) {
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Postes non encore reçus — clic sur « Actualiser »."));
    return;
  }

  if (can_edit)
    ImGui::TextColored(ro::pal::kLabel,
                       i18n::Tr("Nom, droits et part d'exp de chaque poste. Le serveur plafonne "
                       "la part d'exp à %d %%."), kGuildPayRateMax);
  else
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Seul le maître de guilde peut modifier les postes."));

  const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
  // Place réservée aux boutons du bas (uniquement pour le maître).
  const float rows_h = std::max(80.0f, ImGui::GetContentRegionAvail().y -
                                           (can_edit ? ImGui::GetFrameHeightWithSpacing() + 4.0f
                                                     : 0.0f));
  if (ImGui::BeginTable("cs_guild_positions_tbl", 6, table_flags, ImVec2(0.0f, rows_h))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, ro::Px(22.0f));
    ImGui::TableSetupColumn(i18n::Tr("Nom"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(i18n::Tr("Inviter"), ImGuiTableColumnFlags_WidthFixed, ro::Px(54.0f));
    ImGui::TableSetupColumn(i18n::Tr("Expulser"), ImGuiTableColumnFlags_WidthFixed, ro::Px(60.0f));
    ImGui::TableSetupColumn(i18n::Tr("Storage"), ImGuiTableColumnFlags_WidthFixed, ro::Px(60.0f));
    ImGui::TableSetupColumn(i18n::Tr("Part exp"), ImGuiTableColumnFlags_WidthFixed, ro::Px(96.0f));
    ImGui::TableHeadersRow();

    for (int id = 0; id < kGuildPositionSlots; ++id) {
      GuildPositionRow& row = guild_positions_edit_[id];
      // Une ligne n'est éditable que si ses DROITS sont connus : sans le paquet
      // 0x0160, envoyer la ligne écraserait le masque de droits par 0.
      const bool row_editable = can_edit && guild_positions_[id].has_info;
      ImGui::PushID(id);
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextColored(ro::pal::kBlack, "%d", id);

      ImGui::TableSetColumnIndex(1);
      if (row_editable) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ro::InputTextCp949("##nom", row.name, sizeof(row.name)))
          guild_positions_editing_ = true;
      } else {
        ImGui::TextColored(ro::pal::kBlack, "%s", row.name[0] ? row.name : "—");
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
          ImGui::TextColored(ro::pal::kBlack, "%s", on ? i18n::Tr("oui") : "-");
        }
        ImGui::PopID();
      }

      ImGui::TableSetColumnIndex(5);
      if (row_editable) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ro::RoSliderInt("##part", &row.pay_rate, 0, kGuildPayRateMax, "%d %%"))
          guild_positions_editing_ = true;
      } else {
        ImGui::TextColored(ro::pal::kBlack, "%d %%", row.pay_rate);
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
  if (ro::RoButton(i18n::Tr("Enregistrer les postes"))) {
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
    std::snprintf(done, sizeof(done), i18n::Tr("%d poste(s) envoyé(s)."), count);
    guild_status_ = done;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!guild_positions_editing_);
  if (ro::RoButton(i18n::Tr("Annuler les modifications"))) guild_positions_editing_ = false;
  ImGui::EndDisabled();
}

// Choix d'un nouvel emblème parmi les .bmp de <jeu>\emblem\ — le dossier où la
// fenêtre native va lire les siens, pour que les deux voient les mêmes fichiers.
// L'envoi est réservé au maître (le serveur exige gmaster_flag) et refusé pendant
// une guerre de guildes selon la configuration du serveur.
void CharacterSheet::DrawGuildEmblemModal(int guildId, bool is_master) {
  if (!ro::BeginRoPopupModal(i18n::Tr("Changer l'emblème###bourgeon_guild_emblem"))) return;
  const std::string dir = paths::InGameDir("emblem\\");

  ImGui::TextColored(ro::pal::kLabel, i18n::Tr("Format envoyé : %dx%d, magenta pur (255, 0, 255) = transparent."),
                     kEmblemSide, kEmblemSide);
  // Le serveur (clif_parse_GuildChangeEmblem) sort SANS RIEN DIRE quand l'expéditeur
  // n'a pas le drapeau gmaster : autant l'annoncer avant de laisser dessiner.
  if (!is_master)
    ImGui::TextColored(ro::pal::kRed, "%s", i18n::Tr("Tu n'es pas maître de guilde : le serveur ignorera l'envoi."));
  ImGui::Spacing();
  if (!ro::RoBeginTabBar("cs_emblem_tabs")) {
    ro::EndRoPopupModal();
    return;
  }
  // « Reprendre au dessin » a chargé un fichier dans le canvas : l'onglet doit suivre,
  // sinon le clic n'a aucun effet visible (ImGui ne bascule pas tout seul).
  const ImGuiTabItemFlags paint_flags =
      guild_emblem_goto_paint_ ? ImGuiTabItemFlags_SetSelected : 0;
  guild_emblem_goto_paint_ = false;
  if (ImGui::BeginTabItem(i18n::Tr("Dessiner"), nullptr, paint_flags)) {
    DrawGuildEmblemPaintTab(guildId);
    ImGui::EndTabItem();
  }
  if (!ImGui::BeginTabItem(i18n::Tr("Choisir un fichier"))) {
    ro::RoEndTabBar();
    ImGui::Separator();
    if (ro::RoButton(i18n::Tr("Fermer"), 90.0f, 0.0f)) ImGui::CloseCurrentPopup();
    if (!guild_emblem_diag_.empty()) ImGui::TextColored(ro::pal::kLabel, "%s", guild_emblem_diag_.c_str());
    ro::EndRoPopupModal();
    return;
  }

  // Chemin affiché avec des « / » : la police du client est une police CP949, où
  // l'octet 0x5C (l'antislash) se dessine comme le symbole won coréen ₩.
  std::string shown_dir = dir;
  std::replace(shown_dir.begin(), shown_dir.end(), '\\', '/');
  ImGui::TextColored(ro::pal::kLabel, i18n::Tr("Fichiers .bmp de %s"), shown_dir.c_str());
  ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Attendu : 24 bits ou 256 couleurs, non compressé."));
  ImGui::Spacing();

  if (g_emblem_files.empty()) {
    ImGui::TextColored(ro::pal::kRed, "%s", i18n::Tr("Aucun .bmp dans ce dossier."));
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
                    cand.usable ? "" : i18n::Tr("   (refusé)"));
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
    ImGui::TextColored(ro::pal::kLabel, i18n::Tr("%zu octets, prêt à être envoyé."), chosen->bmp.size());
    if (chosen->transparency > kEmblemTransparencyWarn)
      ImGui::TextColored(ro::pal::kRed, i18n::Tr("Transparence ~%d %% : au-delà de %d %% le serveur refuse."),
                         chosen->transparency, kEmblemTransparencyWarn);
  } else if (!guild_emblem_error_.empty()) {
    ImGui::TextColored(ro::pal::kRed, "%s", guild_emblem_error_.c_str());
  } else {
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Choisis un fichier dans la liste."));
  }

  ImGui::Spacing();
  const bool can_send = chosen && chosen->usable && guildId > 0;
  ImGui::BeginDisabled(!can_send);
  // Envoi par le chemin NATIF (service web) : le seul qui fonctionne sur ce serveur.
  if (ro::RoButton(i18n::Tr("Envoyer"), 110.0f, 0.0f)) {
    const bool started = RequestEmblemUploadSEH(guildId, chosen->name.c_str());
    guild_emblem_diag_ = started
                             ? i18n::Tr("Envoi au service web lancé (") + chosen->name + ")."
                             : i18n::Tr("Refusé : un envoi est déjà en cours, ou service indisponible.");
    if (started) guild_status_ = i18n::Tr("Emblème envoyé : ") + chosen->name;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Relire le dossier"), 140.0f, 0.0f)) {
    ScanEmblemFolder();
    guild_emblem_sel_ = -1;
    guild_emblem_error_.clear();
  }
  ImGui::SameLine();
  // Passerelle vers l'éditeur : retoucher un emblème existant plutôt que de le refaire.
  ImGui::BeginDisabled(!chosen || chosen->bmp.empty());
  if (ro::RoButton(i18n::Tr("Reprendre au dessin"), 160.0f, 0.0f)) {
    if (EmblemCanvasLoadBmp(chosen->bmp)) {
      guild_emblem_goto_paint_ = true;  // bascule sur l'éditeur, sinon rien ne se voit
      guild_emblem_error_.clear();
      guild_status_ = i18n::Tr("Dessin repris de ") + chosen->name;
    } else {
      guild_emblem_error_ = i18n::Tr("Ce fichier n'est pas reprenable (dimensions ou profondeur).");
    }
  }
  ImGui::EndDisabled();
  ImGui::EndTabItem();
  ro::RoEndTabBar();

  ImGui::Separator();
  if (ro::RoButton(i18n::Tr("Fermer"), 90.0f, 0.0f)) ImGui::CloseCurrentPopup();
  ImGui::SameLine();
  ImGui::SameLine();
  ImGui::TextColored(ro::pal::kLabel,
                     "%s", i18n::Tr("L'envoi passe par le service web du serveur, comme la fenêtre native.\n"
                           "L'emblème se met à jour dès que le serveur a publié la nouvelle version."));
  // Compte rendu du dernier envoi (aussi écrit dans bourgeon.log et la console).
  if (!guild_emblem_diag_.empty()) ImGui::TextColored(ro::pal::kLabel, "%s", guild_emblem_diag_.c_str());
  ro::EndRoPopupModal();
}

// Canvas 24x24 : clic gauche = couleur courante, clic droit = gomme. Le rendu est un
// simple ImDrawList (576 rectangles) plutôt qu'une texture — pas de cache à invalider
// au reset du device, et le damier de fond montre où l'emblème sera transparent.
// Le statut de maître n'est PAS un paramètre : l'avertissement « tu n'es pas maître »
// est affiché une fois pour tout le modal, et le serveur reste seul juge de l'envoi.
void CharacterSheet::DrawGuildEmblemPaintTab(int guildId) {
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
  ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Outil :"));
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
    if (ro::RoToggleButton(i18n::Tr(tool_names[t]), g_emblem_canvas.tool == t, 80.0f, 0.0f))
      g_emblem_canvas.tool = t;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr(tool_hints[t]));
    ImGui::PopID();
    if (t != 2 && t != 5) ImGui::SameLine();
  }
  ImGui::SameLine();
  ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("(clic droit = efface)"));

  ImGui::SetNextItemWidth(ro::Px(140.0f));
  ro::RoSliderInt(i18n::Tr("Épaisseur"), &g_emblem_canvas.brush, 1, 3, "%d px");
  ImGui::SameLine();
  ro::RoCheckbox(i18n::Tr("Symétrie"), &g_emblem_canvas.mirror);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", i18n::Tr("Chaque trait est répété en miroir gauche/droite."));
  ImGui::SameLine();
  // Ne concerne que le rectangle et l'ellipse : grisé ailleurs plutôt que caché, pour
  // que la barre d'outils ne saute pas d'un outil à l'autre.
  ImGui::BeginDisabled(g_emblem_canvas.tool != kToolRect && g_emblem_canvas.tool != kToolEllipse);
  ro::RoCheckbox(i18n::Tr("Forme pleine"), &g_emblem_canvas.filled);
  ImGui::EndDisabled();

  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Couleur :"));
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
  ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Transparence :"));
  ImGui::SameLine();
  const ImVec4 magenta(1.0f, 0.0f, 1.0f, 1.0f);
  const float swatch_h = ImGui::GetFrameHeight();
  if (ImGui::ColorButton("##cs_emblem_transparent", magenta,
                         ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                         ImVec2(swatch_h * 1.6f, swatch_h)))
    g_emblem_canvas.color_clear = true;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", i18n::Tr("Magenta pur (255, 0, 255) : ces pixels sont transparents en jeu.\n"
                            "S'utilise avec n'importe quel outil (remplissage, formes…)."));
  if (g_emblem_canvas.color_clear) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.10f, 0.35f, 0.70f, 1.0f), "%s", i18n::Tr("couleur active"));
  }
  ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Palette :"));
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
  if (ro::RoButton(i18n::Tr("Annuler"), 90.0f, 0.0f)) {
    std::copy(g_emblem_canvas.undo.back().begin(), g_emblem_canvas.undo.back().end(),
              g_emblem_canvas.pixel);
    g_emblem_canvas.undo.pop_back();
    ++g_emblem_canvas.revision;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Tout effacer"), 110.0f, 0.0f)) {
    EmblemCanvasPushUndo();
    EmblemCanvasClear();
  }
  ImGui::SameLine();
  // Reprendre l'emblème en place sans refermer le modal : le clic sur l'emblème de
  // l'en-tête fait la même chose, mais seulement à l'ouverture.
  ImGui::BeginDisabled(guildId <= 0);
  if (ro::RoButton(i18n::Tr("Emblème actuel"), 150.0f, 0.0f)) {
    const std::vector<uint8_t> current = CurrentEmblemBmp(guildId);
    if (!current.empty() && EmblemCanvasLoadBmp(current))
      guild_emblem_diag_ = i18n::Tr("Emblème actuel de la guilde chargé dans l'éditeur.");
    else
      guild_emblem_diag_ = i18n::Tr("Emblème actuel illisible (pas encore téléchargé ?).");
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", i18n::Tr("Repart de l'emblème que porte la guilde (annulable)."));

  // ── Point de départ : une icône d'item ────────────────────────────────────
  // Les icônes d'inventaire du client font 24x24, la taille exacte d'un emblème :
  // elles font d'excellentes bases (potion, carte, arme…) à retoucher ensuite.
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Partir d'une icône d'item :"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ro::Px(90.0f));
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
  if (ro::RoButton(i18n::Tr("Importer"), 110.0f, 0.0f)) {
    if (EmblemCanvasLoadItemIcon(static_cast<uint32_t>(guild_emblem_item_id_))) {
      guild_emblem_diag_.clear();
    } else {
      guild_emblem_diag_ = i18n::Tr("Icône introuvable pour cet item.");
    }
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", i18n::Tr("Remplace le dessin par l'icône de l'item (annulable)."));
  if (guild_emblem_item_id_ > 0 && !preview_icon.tex) {
    ImGui::SameLine();
    ImGui::TextColored(ro::pal::kRed, "%s", i18n::Tr("aucune icône"));
  }
  ImGui::SameLine();
  if (ro::RoToggleButton("Inventaire", guild_emblem_gallery_, 110.0f, 0.0f))
    guild_emblem_gallery_ = !guild_emblem_gallery_;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", i18n::Tr("Choisir l'icône dans son sac, sans connaître les numéros."));

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
      ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Aucun item dans le sac."));
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
          const char* nm = itemcell::NameById(id);
          ImGui::SetTooltip("%s (%u)", (nm && nm[0]) ? nm : "?", id);
        }
        if (clicked) {
          guild_emblem_item_id_ = static_cast<int>(id);
          if (EmblemCanvasLoadItemIcon(id)) guild_emblem_diag_.clear();
          else guild_emblem_diag_ = i18n::Tr("Icône introuvable pour cet item.");
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

  ImGui::TextColored(ro::pal::kLabel, i18n::Tr("%zu octets (BMP %dx%d, 24 bits)."), bmp.size(), kEmblemSide,
                     kEmblemSide);
  // Le serveur refuse un emblème trop vide (inter_config.emblem_transparency_limit) :
  // mieux vaut le dire pendant qu'on dessine qu'après un envoi rejeté.
  if (s_transparency > kEmblemTransparencyWarn)
    ImGui::TextColored(ro::pal::kRed, i18n::Tr("Transparence ~%d %% : au-delà de %d %% le serveur refuse — "
                             "remplis davantage le fond."),
                       s_transparency, kEmblemTransparencyWarn);
  const bool can_send = has_ink && guildId > 0;
  // Nom du .bmp, commun aux deux boutons : ce que le joueur a saisi, sinon le défaut
  // que le champ annonce en indice — d'où le repli ici plutôt qu'un champ pré-rempli.
  // 🔴 Sa traduction doit rester en ASCII PUR : ce n'est pas un libellé d'écran mais
  // un NOM DE FICHIER, écrit dans <jeu>\emblem\ puis passé tel quel à l'upload natif,
  // qui manipule le chemin dans la code-page du client. Un accent suffirait à faire
  // échouer l'écriture ou à produire du mojibake.
  const std::string file_name = std::string(g_emblem_canvas.save_name[0]
                                                ? g_emblem_canvas.save_name
                                                : i18n::Tr("mon_embleme")) + ".bmp";
  ImGui::BeginDisabled(!can_send);
  // Le dessin part par le chemin NATIF : on l'écrit d'abord dans <jeu>\emblem\, car
  // l'upload du jeu prend un CHEMIN de fichier (curl lit le fichier lui-même).
  if (ro::RoButton(i18n::Tr("Envoyer ce dessin"), 160.0f, 0.0f)) {
    if (!WriteEmblemFile(file_name.c_str(), bmp)) {
      guild_emblem_diag_ = i18n::Tr("Écriture impossible : emblem/") + file_name;
    } else {
      g_emblem_preview_cache.erase(file_name);  // la vignette du fichier a changé
      const bool started = RequestEmblemUploadSEH(guildId, file_name.c_str());
      guild_emblem_diag_ = started
                               ? i18n::Tr("Envoi au service web lancé (") + file_name + ")."
                               : i18n::Tr("Refusé : un envoi est déjà en cours, ou service indisponible.");
      if (started) guild_status_ = i18n::Tr("Emblème dessiné envoyé (") + file_name + ").";
    }
  }
  ImGui::EndDisabled();
  // Bouton grisé : dire POURQUOI plutôt que de laisser deviner.
  if (!can_send) {
    ImGui::SameLine();
    const char* why = has_ink ? i18n::Tr("guilde inconnue du client") : i18n::Tr("dessin vide");
    ImGui::TextColored(ro::pal::kRed, i18n::Tr("Envoi impossible : %s."), why);
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ro::Px(150.0f));
  ro::InputTextCp949WithHint("##cs_emblem_savename", i18n::Tr("mon_embleme"),
                             g_emblem_canvas.save_name,
                             sizeof(g_emblem_canvas.save_name));
  ImGui::SameLine();
  // Garder le .bmp sous la main : il rejoint le dossier que lit aussi la fenêtre native.
  if (ro::RoButton(i18n::Tr("Enregistrer dans emblem"), 190.0f, 0.0f)) {
    const std::string path = paths::InGameDir("emblem\\") + file_name;
    CreateDirectoryA(paths::InGameDir("emblem").c_str(), nullptr);
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") == 0 && fp) {
      std::fwrite(bmp.data(), 1, bmp.size(), fp);
      std::fclose(fp);
      // Nom seul, sans le chemin : la police CP949 du client dessine l'antislash en ₩.
      guild_status_ = i18n::Tr("Emblème enregistré : emblem/") + file_name;
      // Réécrire sous un nom déjà vu : la vignette en cache montrerait l'ancien dessin.
      g_emblem_preview_cache.erase(file_name);
      ScanEmblemFolder();
    } else {
      guild_emblem_error_ = i18n::Tr("Écriture impossible : emblem/") + file_name;
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
  dl->AddRectFilled(p0, p1, SlotBgCol(), ro::Px(4.0f));     // fond (skin RO)
  dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 80), ro::Px(4.0f));  // bordure
  if (has) {
    ro::IconTex ic = ro::ItemIcon(it.nameid);
    if (ic.tex) {
      // La marge suit la case : à l'échelle, une icône collée à un pad de 3 px
      // dans une case deux fois plus grande laisserait un cadre disproportionné.
      const float pad = ro::Px(3.0f);
      dl->AddImage(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(p0.x + pad, p0.y + pad),
                   ImVec2(p1.x - pad, p1.y - pad));
    }
    if (it.refine > 0) {  // overlay "+N" en BAS À DROITE (comme l'inventaire) : noir + liseré blanc
      char rf[8];
      std::snprintf(rf, sizeof(rf), "+%d", it.refine);
      const ImVec2 ts = ImGui::CalcTextSize(rf);
      const ImVec2 rp(p1.x - ts.x - ro::Px(2.0f), p1.y - ts.y - ro::Px(1.0f));
      const ImU32 white = IM_COL32_WHITE;
      ro::AddTextHalo(dl, rp, IM_COL32_BLACK, rf, white);
    }
  } else {  // slot vide : abreviation grisee
    // 🔴 Le libellé doit tenir DANS la case, il est donc MESURÉ puis rétréci s'il
    // déborde. La case est en pixels d'art (44) et la police de l'interface est un
    // réglage du joueur : les deux ne se suivent pas, aucune taille écrite en dur ne
    // peut garantir que « Garment » rentre — il sortait des deux côtés. On garde le
    // terme du jeu (Garment, pas Cloak) et c'est le rendu qui cède.
    const char* ab = SlotAbbrev(slot);
    ImFont* font = ImGui::GetFont();
    const float base  = ImGui::GetFontSize();
    const float avail = sz - ro::Px(4.0f);  // marge intérieure symétrique
    ImVec2 ts = ImGui::CalcTextSize(ab);
    float fs = base;
    if (ts.x > avail && ts.x > 0.0f) {
      fs = std::max(base * (avail / ts.x), ro::Px(6.0f));
      ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, ab);
    }
    dl->AddText(font, fs, ImVec2(p0.x + (sz - ts.x) * 0.5f, p0.y + (sz - ts.y) * 0.5f),
                IM_COL32(120, 120, 120, 255), ab);
  }

  // Drag-drop. SOURCE (slot occupé) : glisser l'item vers l'inventaire = le déséquiper.
  // Payload "BGN_EQUIP" = index inventaire ; l'inventaire l'accepte -> SendUnequip.
  // Pas de glisser quand Maj est enfoncé : ce geste-là poste le LIEN dans le chat
  // (cf. plus bas), et sans ce garde un simple frémissement de souris déséquipait.
  if (has && !ImGui::GetIO().KeyShift &&
      ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
    int inv = it.invIndex;
    ImGui::SetDragDropPayload("BGN_EQUIP", &inv, sizeof(inv));
    ro::IconTex ic = ro::ItemIcon(it.nameid);  // aperçu du drag (icône + nom complet)
    if (ic.tex) {
      ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(24, 24));
      ImGui::SameLine();
    }
    const uintptr_t dsrc = rag::kSessionAddr + (costume ? rag::equip::kOwnCostumeBase : rag::equip::kOwnEquipBase) +
                           static_cast<uintptr_t>(slot) * rag::equip::kSlotStride;
    char dnm[128];
    itemcell::BuildDisplayName(reinterpret_cast<void*>(dsrc), dnm, sizeof(dnm));
    ImGui::TextUnformatted(dnm[0] ? dnm : itemcell::NameById(it.nameid));
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

  // Interactions sur la case : survol = description ; clic DROIT = description
  // (fenêtre) ; MAJ+clic GAUCHE = lien de l'item dans le chat — le MÊME geste que
  // l'inventaire et que le natif (UIInventoryWnd_OnLButtonDown, branche SHIFT) ;
  // double-clic GAUCHE = déséquiper ; glisser = vers l'inventaire (drag-drop
  // ci-dessus). Le clic gauche simple ne fait rien.
  if (has && ImGui::IsItemHovered()) {
    ro::SetHoverCursor(2);  // main
    // Aperçu RO au survol, exactement comme l'inventaire, le chariot et le storage :
    // on RETIENT la case ici, l'aperçu se dessine après la fenêtre (DrawHoverDesc) —
    // c'est un popup, il doit passer AU-DESSUS d'elle. Pas pendant un glisser :
    // l'aperçu masquerait la cible du drop.
    const uintptr_t hsrc = rag::kSessionAddr + (costume ? rag::equip::kOwnCostumeBase : rag::equip::kOwnEquipBase) +
                           static_cast<uintptr_t>(slot) * rag::equip::kSlotStride;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        ImGui::GetDragDropPayload() == nullptr)
      CaptureHoverDesc(reinterpret_cast<const void*>(hsrc), it.nameid);
    const ImVec2 mp = ImGui::GetMousePos();
    // Maj+clic GAUCHE = lien de l'item dans l'input du chat (geste de l'inventaire).
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyShift) {
      if (auto* iv = Bourgeon::Instance().inventory_viewer())
        iv->LinkItemToChat(it.invIndex);
    }
    // Clic droit = description (avec cartes/enchants/options du slot source).
    // DIFFÉRÉE au relâchement (itemcell::FlushDeferredDesc) : ouverte dès le
    // clic, un appui PROLONGÉ faisait passer la description DERRIÈRE nous.
    // `src` survit au différé : c'est une adresse FIXE du bloc session.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      const uintptr_t src = rag::kSessionAddr + (costume ? rag::equip::kOwnCostumeBase : rag::equip::kOwnEquipBase) +
                            static_cast<uintptr_t>(slot) * rag::equip::kSlotStride;
      itemcell::DeferDescById(it.nameid, it.viewId, it.location,
                              static_cast<int>(mp.x), static_cast<int>(mp.y),
                              reinterpret_cast<const void*>(src));
    }
    // Maj enfoncée, le double-clic reste un envoi de lien : il ne doit pas déséquiper.
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyShift)
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
      const ImVec2 rp(p1.x - ts.x - ro::Px(2.0f), p1.y - ts.y - ro::Px(1.0f));
      const ImU32 white = IM_COL32_WHITE;
      ro::AddTextHalo(dl, rp, IM_COL32_BLACK, q, white);
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
      // Même aperçu de description que les cases d'équipement (cf. DrawSlot).
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
          ImGui::GetDragDropPayload() == nullptr)
        CaptureHoverDesc(am.info, am.nameid);
      const ImVec2 mp = ImGui::GetMousePos();
      const bool shift = ImGui::GetIO().KeyShift;
      // Maj+clic GAUCHE = lien de l'item dans le chat, comme les slots d'équipement.
      if (shift && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (auto* iv = Bourgeon::Instance().inventory_viewer())
          iv->LinkItemToChat(am.invIndex);
      }
      // Différée au relâchement, comme les slots d'équipement ci-dessus.
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        itemcell::DeferDescById(am.nameid, am.viewId, am.location,
                                static_cast<int>(mp.x), static_cast<int>(mp.y));
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !shift)
        SendUnequip(am.invIndex);
    } else {
      ImGui::SetTooltip("%s", i18n::Tr("Munition\n(glissez une flèche/balle/grenade/arme de jet ici pour l'équiper)"));
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
    const ImU32 tint = active ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 140);
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
      if (ImGui::MenuItem(i18n::Tr("Ouvrir le cart"), nullptr, false, active)) OpenCartWindow();
      const bool canDeco = companion_.changecart_lv > 0 && active;
      if (ImGui::MenuItem(i18n::Tr("Changer la décoration"), nullptr, false, canDeco)) {
        int next = companion_.cart_active + 1;
        if (next > companion_.cart_deco_max || next < 1) next = 1;
        SendCompanionPkt(kCompCart, kCompDeco, next);
      }
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::Tr("Retirer le cart"), nullptr, false, active))
        SendCompanionPkt(kCompCart, kCompOff, 0);
      ImGui::EndPopup();
    }
  }

  // Tooltip (pas pendant que le menu est ouvert).
  if (hov && !ImGui::IsPopupOpen("cart_ctx")) {
    if (kind == kCompCart)
      ImGui::SetTooltip(i18n::Tr("%s — %s\n(clic gauche : %s, clic droit : menu)"), name,
                        active ? "actif" : "inactif", active ? "ranger" : "invoquer");
    else
      ImGui::SetTooltip(i18n::Tr("%s — %s\n(clic gauche : %s)"), name, active ? "actif" : "inactif",
                        active ? "renvoyer" : "invoquer");
  }
  ImGui::PopID();
}

// Ouvre la fenêtre d'inventaire du cart. MakeWindow crée/affiche par id (RE 2026-07-12 :
// id 0x28 = UIMerchantItemWnd, vtable 0x0103d538 ; même appel que la fenêtre de description).
// Le case a un gate de contexte UI (IsWindowAllowedInContext) qui passe en jeu normal ;
// OnCreate ne dépend pas de l'état cart (au pire fenêtre vide), le serveur pousse le contenu.
void CharacterSheet::OpenCartWindow() {
  __try {
    uiwnd::MakeWindow(uiwnd::kUICartWnd);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Relève les données d'INSTANCE de l'item survolé. Le nom passe par le name-builder
// natif (itemcell::BuildDisplayName), tout le reste se lit à même l'ItemSkillInfo, sous SEH :
// un slot à moitié initialisé ne doit pas tuer le client pour un simple survol.
void CharacterSheet::CaptureHoverDesc(const void* info, uint32_t id) {
  hover_desc_ = HoverDesc{};
  if (!info || id == 0) return;
  hover_desc_.id = id;
  itemcell::BuildDisplayName(const_cast<void*>(info), hover_desc_.name,
                             sizeof(hover_desc_.name));  // SEH intégré, rend de l'UTF-8
  __try {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(info);
    hover_desc_.refine  = *reinterpret_cast<const int*>(p + rag::equip::kOffRefine);
    hover_desc_.damaged = *(p + rag::equip::kOffDamaged) != 0;
    // ⚠ Sur un item FORGÉ/CRÉÉ, +0x1c ne porte pas des cartes mais les données du
    // forgeron (charid scindé, star crumbs, élément) : même critère que l'inventaire
    // et que la fenêtre de description (première entrée <= 500).
    const uint32_t c0 = *reinterpret_cast<const uint32_t*>(p + rag::equip::kOffCards);
    hover_desc_.forged = (c0 != 0 && c0 <= 500);
    if (!hover_desc_.forged)
      for (int k = 0; k < 4; ++k)
        hover_desc_.cards[k] =
            *reinterpret_cast<const uint32_t*>(p + rag::equip::kOffCards + k * 4);
    int nopt = *reinterpret_cast<const int*>(p + rag::equip::kOffOptCount);
    if (nopt < 0) nopt = 0;
    if (nopt > 5) nopt = 5;
    hover_desc_.opt_count = nopt;
    for (int k = 0; k < nopt; ++k) {
      const uint8_t* e = p + rag::equip::kOffOpts + k * 5;
      hover_desc_.opts[k].index = *reinterpret_cast<const int16_t*>(e);
      hover_desc_.opts[k].value = *reinterpret_cast<const int16_t*>(e + 2);
      hover_desc_.opts[k].param = e[4];
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// L'aperçu lui-même. HORS de toute fenêtre ImGui (cf. itemcell::DrawTooltip) : appelé
// juste après ro::EndRoWindow, il passe ainsi AU-DESSUS de la fiche.
void CharacterSheet::DrawHoverDesc() {
  if (hover_desc_.id == 0) return;
  itemdesc::SimpleOpt sopts[5];
  for (int k = 0; k < hover_desc_.opt_count && k < 5; ++k) {
    sopts[k].index = hover_desc_.opts[k].index;
    sopts[k].value = hover_desc_.opts[k].value;
    sopts[k].param = hover_desc_.opts[k].param;
  }
  itemcell::DrawTooltip(hover_desc_.id, hover_desc_.forged ? nullptr : hover_desc_.cards,
                        hover_desc_.forged ? 0 : 4, sopts, hover_desc_.opt_count,
                        hover_desc_.refine,
                        hover_desc_.name[0] ? hover_desc_.name : nullptr,
                        hover_desc_.damaged);
}

void CharacterSheet::DrawDoll(float avail_w) {
  // En-tete (colonne gauche, CENTRE horizontalement) : pseudo, classe, niveau.
  const float start_x = ImGui::GetCursorPosX();
  const ImVec2 hdr_p0 = ImGui::GetCursorScreenPos();  // coin haut-gauche (emblème)
  auto centered = [&](const char* txt) {
    const float tw = ImGui::CalcTextSize(txt).x;
    ImGui::SetCursorPosX(start_x + std::max(0.0f, (avail_w - tw) * 0.5f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 5.0f);
    ImGui::TextColored(ro::pal::kBlack, "%s", txt);
  };
  const std::string name = Bourgeon::Instance().client().session().GetCharName();
  char lvl[96];
  std::snprintf(lvl, sizeof(lvl), "%s   Nv %d / %d", rag::OwnClassName(),
                ReadInt(rag::kBaseLevelAddr), ReadInt(rag::kJobLevelAddr));
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
    // PV et SP sur une seule ligne, ou sur DEUX si elle ne tient pas dans la colonne :
    // un personnage à six chiffres de PV (et une police large) débordait sinon du cadre,
    // et c'est le SP qui disparaissait — la moitié de l'information, sans rien qui le dise.
    char hp[40], sp[40], both[80];
    std::snprintf(hp, sizeof(hp), "HP: %d/%d", s.hp, s.hp_max);
    std::snprintf(sp, sizeof(sp), "SP: %d/%d", s.sp, s.sp_max);
    std::snprintf(both, sizeof(both), "%s %s", hp, sp);
    if (ImGui::CalcTextSize(both).x <= avail_w) {
      centered(both);
    } else {
      centered(hp);
      centered(sp);
    }
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
  const float sz = ro::Px(kSlotSz), gap = ro::Px(kSlotGap),
              avatar_w = ro::Px(kAvatarW);
  const float block_w = ro::Px(kDollBlockW);
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
  // Molette sur l'avatar = tourner (comme le preview cashshop : dir 0..7 + wrap).
  // Elle passe par le verrou anti-défilement, qui la retient tant que la fiche est
  // en train de défiler : sinon parcourir la page fait pivoter le perso au passage.
  // Defaut = face.
  if (ImGui::IsItemHovered()) {
    ro::SetHoverCursor(2);
    const float wheel = mui::LastItemWheel();
    if (wheel != 0.0f) {
      if (avatar_anim_ == kAnimCombat)  // combat : 4 dirs cardinales (0=face,2,4=dos,6)
        avatar_dir_ = ((avatar_dir_ & ~1) + (wheel > 0.0f ? 2 : 6)) & 7;
      else
        avatar_dir_ = (avatar_dir_ + (wheel > 0.0f ? 1 : 7)) & 7;  // 8 dirs
      if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();  // persister la direction
    }
  }

  // Sélecteur de pose + bouton GIF sur la MÊME ligne (la direction = MOLETTE). Les poses
  // « (animé) » jouent l'animation (Marche/Combat) ; les autres sont figées à l'image 0.
  // Combo ET bouton s'ajustent à la largeur de leur texte.
  const float sel_y = content_bottom + 8.0f;
  const float fh = ImGui::GetFrameHeightWithSpacing();
  const float combo_w = PoseComboW();
  const float gif_w   = GifButtonW();
  const float line_gap = ro::Px(kPoseLineGap);  // écart net combo / bouton
  // La colonne est dimensionnée pour cette ligne (cf. DollPaneW), mais elle peut
  // malgré tout être trop étroite : le joueur a rétréci la fenêtre, ou une scrollbar
  // verticale est apparue et a pris sa largeur sur le contenu. Dans ce cas le bouton
  // passe SOUS le combo plutôt que de sortir du cadre.
  const bool  one_line = (combo_w + line_gap + gif_w) <= avail_w;
  const float line_w = one_line ? combo_w + line_gap + gif_w : combo_w;
  const float line_x = static_cast<float>(static_cast<int>(
      start_x + std::max(0.0f, (avail_w - line_w) * 0.5f)));
  ImGui::SetCursorPos(ImVec2(line_x, sel_y));
  ImGui::SetNextItemWidth(combo_w);
  if (ro::RoBeginCombo("##cs_pose", PoseLabelFull(avatar_anim_, avatar_animate_))) {
    for (int i = 0; i < kPoseCount; ++i) {
      const bool sel =
          (avatar_anim_ == kPoses[i].anim && avatar_animate_ == kPoses[i].animate);
      // Le libellé TRADUIT, comme l'aperçu du combo (PoseLabelFull) : la liste
      // déroulante restait en français dans les autres langues.
      if (ImGui::Selectable(i18n::Tr(kPoses[i].label), sel)) {
        avatar_anim_    = kPoses[i].anim;
        avatar_animate_ = kPoses[i].animate;
        if (avatar_anim_ == kAnimCombat) avatar_dir_ &= ~1;  // snap dir cardinale
        if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();  // persister la pose
      }
    }
    ro::RoEndCombo();
  }
  // Bouton « Générer le GIF », largeur ajustée à son texte : à droite du combo quand la
  // colonne est assez large, sur la ligne suivante sinon. Ouvre un dialogue « Enregistrer
  // sous » (thread séparé, non bloquant) ; l'export se fait ici.
  if (one_line) {
    ImGui::SameLine(0.0f, line_gap);
  } else {
    ImGui::SetCursorPos(ImVec2(static_cast<float>(static_cast<int>(
                                   start_x + std::max(0.0f, (avail_w - gif_w) * 0.5f))),
                               sel_y + fh));
  }
  const bool gif_busy = gif_dialog_busy_.load();
  if (gif_busy) ImGui::BeginDisabled();
  if (ro::RoButton(i18n::Tr("Générer le GIF"), gif_w)) RequestGifSave();
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
      gif_status_ = ok ? (std::string(i18n::Tr("GIF OK : ")) + (fn ? fn + 1 : p.c_str()))
                       : std::string(i18n::Tr("Échec GIF (voir log)"));
    } else {
      gif_status_ = i18n::Tr("Export annulé");
    }
    gif_dialog_busy_.store(false);
  }
  // Bas de la ligne pose/GIF : une rangée, ou deux quand le bouton est passé dessous.
  const float pose_bottom = sel_y + fh * (one_line ? 1.0f : 2.0f);
  if (!gif_status_.empty()) {
    ImGui::SetCursorPos(ImVec2(ox, pose_bottom));
    ImGui::PushTextWrapPos(ox + block_w);  // wrap dans la largeur du bloc
    ImGui::TextColored(ro::pal::kBlack, "%s", gif_status_.c_str());
    ImGui::PopTextWrapPos();
  }

  // Case config native SOUS le combo de pose, selon l'onglet (bascule via le dispatcher, le
  // serveur répond ZC_CONFIG qui applique + rafraîchit le sprite) :
  //   Costume -> « Voir les costumes » (flag 0x016024c0 : 0=affiché) ;
  //   Équipement -> « Montrer mon équipement » aux autres (flag 0x015ffd14 : 1=visible).
  const float cfg_y = pose_bottom + (gif_status_.empty() ? 0.0f : fh) + 4.0f;
  if (costume_) {
    bool show = ReadInt(kCostumeHideFlag) == 0;
    const float cw = ImGui::CalcTextSize(i18n::Tr("Voir les costumes")).x + 42.0f;
    ImGui::SetCursorPos(ImVec2(start_x + std::max(0.0f, (avail_w - cw) * 0.5f), cfg_y));
    if (ro::RoCheckbox(i18n::Tr("Voir les costumes"), &show))
      SendConfigToggle(kCmdViewCostume, show ? 1 : 0);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", i18n::Tr("Affiche ou masque les costumes sur ton personnage"));
  } else {
    bool pub = ReadInt(kShowEquipFlag) != 0;
    const float cw = ImGui::CalcTextSize(i18n::Tr("Montrer mon équipement")).x + 42.0f;
    ImGui::SetCursorPos(ImVec2(start_x + std::max(0.0f, (avail_w - cw) * 0.5f), cfg_y));
    if (ro::RoCheckbox(i18n::Tr("Montrer mon équipement"), &pub))
      SendConfigToggle(kCmdShowEquip, pub ? 1 : 0);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", i18n::Tr("Rend ton équipement visible (ou non) aux autres joueurs"));
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
    ofn.lpstrFilter = i18n::Tr("GIF anime (*.gif)\0*.gif\0Tous les fichiers\0*.*\0");
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = sizeof(path);
    ofn.lpstrInitialDir = initdir.c_str();
    ofn.lpstrDefExt = "gif";
    ofn.lpstrTitle  = i18n::Tr("Enregistrer le GIF de l'avatar");
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    const bool ok = GetSaveFileNameA(&ofn) != 0;
    gif_dialog_path_ = ok ? std::string(path) : std::string();
    gif_dialog_ready_.store(true);  // release : signale le thread principal
    if (SUCCEEDED(hr)) CoUninitialize();
  }).detach();
}

void CharacterSheet::DrawStatsPanel() {
  Stats s{};
  stat_rows_valid_ = false;  // pas de relevé tant que la lecture n'a pas abouti
  if (!ReadStats(&s)) return;

  // Haut du CONTENU de ce volet : toutes les ordonnées relevées plus bas lui sont
  // soustraites, pour que le volet staff aligne ses commandes sur les rangées sans
  // dépendre du scroll ni de la position de la fenêtre (cf. stat_row_dy_).
  const float pane_top = ImGui::GetCursorPosY();

  // Tooltip enrichi d'une stat primaire : rôle + split équip/carte quand le serveur
  // l'a poussé (ZC_BOURGEON_STAT_BONUS). Le natif ne donne que le TOTAL ; ici on
  // détaille l'origine. buf doit vivre jusqu'à l'appel ImGui (pile de l'appelant).
  auto primaryTip = [&](int i, char* buf, int cap) -> const char* {
    if (bonus_.valid && (bonus_.equip[i] != 0 || bonus_.card[i] != 0))
      std::snprintf(buf, cap, i18n::Tr("%s\nÉquipement : %+d   Cartes : %+d"),
                    i18n::Tr(kStatDesc[i]), bonus_.equip[i], bonus_.card[i]);
    else
      std::snprintf(buf, cap, "%s", i18n::Tr(kStatDesc[i]));
    return buf;
  };

  // Stats primaires + boutons de montee. (Pseudo/classe/niveau : colonne gauche.)
  const float step = ImGui::GetFrameHeight();
  const float right = ImGui::GetContentRegionMax().x;  // bord droit local (align +)
  const float start = ImGui::GetCursorPosX();          // colonne LABEL (gauche)
  // Colonne VALEURS alignée : après le plus large label ("Esq.P") + marge. Toutes les
  // valeurs (primaires + dérivées) démarrent à ce x -> chiffres en colonne.
  const float val_x =
      start + ImGui::CalcTextSize(i18n::Tr("Esq.P")).x + ro::Px(kStatValGap);
  const float cost_w = StatCostW();  // largeur réservée au coût, à droite du +
  const float max_w = ImGui::CalcTextSize(i18n::Tr("Max")).x + ImGui::GetStyle().FramePadding.x * 2.0f + 4.0f;
  // Petit fond arrondi gris léger derrière le NOM de chaque stat (limité au libellé).
  const ImU32 kRowBg = IM_COL32(165, 170, 180, 55);  // gris léger
  for (int i = 0; i < 6; ++i) {
    stat_row_dy_[i] = ImGui::GetCursorPosY() - pane_top;  // repère pour le volet staff
    const ImVec2 rp = ImGui::GetCursorScreenPos();  // haut de la rangée
    const float nw = ImGui::CalcTextSize(kStatName[i]).x;  // largeur du nom
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(rp.x - 3.0f, rp.y), ImVec2(rp.x + nw + 5.0f, rp.y + step), kRowBg, 4.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ro::pal::kBlack, "%s", kStatName[i]);             // label
    if (ImGui::IsItemHovered()) { char tb[256]; ImGui::SetTooltip("%s", primaryTip(i, tb, sizeof(tb))); }  // rôle + split équip/carte
    ImGui::SameLine();
    ImGui::SetCursorPosX(val_x);                                // colonne valeurs
    // 🔴 TIRET ASCII, PAS un cadratin « — ». La police intégrée d'ImGui (ProggyClean)
    // ne bake que la plage 0x20-0xFF : U+2014 y sort en « ? ». Un joueur qui choisit
    // cette police lisait donc « STR ? 999 » sur chacune de ses six stats.
    if (s.bonus[i] != 0)
      ImGui::TextColored(ro::pal::kBlack, "- %d (+%d)", s.base[i], s.bonus[i]);
    else
      ImGui::TextColored(ro::pal::kBlack, "- %d", s.base[i]);
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
    if (ro::RoButton(i18n::Tr("Max"), max_w, step))
      for (int k = 0; k < 2; ++k) SendStatUp(kStatType[i], 255);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip(i18n::Tr("Ajouter le MAXIMUM de points possible dans %s"), kStatName[i]);
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
          i18n::Tr("Monter %s (coût : %d point%s)\nMaj+clic : jusqu'au prochain palier de 10"),
          kStatName[i], s.raise[i], s.raise[i] > 1 ? "s" : "");
    if (!can) ImGui::EndDisabled();
    ImGui::PopID();
    if (s.raise[i] > 0) {  // coût du prochain point, juste à droite du +
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(can ? ro::pal::kBlack : ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%d", s.raise[i]);
    }
  }
  stat_points_dy_ = ImGui::GetCursorPosY() - pane_top;  // repère pour le volet staff
  ImGui::TextColored(ro::pal::kBlack, i18n::Tr("Points de statut : %d"), s.points);
  ImGui::Separator();

  // Stats derivees : label (gauche) + valeur (colonne val_x alignée). Survol = expl. Le
  // % : DEF/MDEF « 1 » (soft, VIT/INT = réduction en %) + « 2 » (plate) ; CRI/Esq.P = %.
  //
  // Le DÉTAIL entre parenthèses — « (équip +N) », « (refine +N%) » — suit la valeur, ou
  // PASSE À LA LIGNE (indenté sur la colonne des valeurs) quand il n'y tient plus. Le
  // volet est dimensionné sur la valeur seule : un ATK à cinq chiffres avec une police
  // large sortait sinon du cadre, coupé net au milieu d'un mot. Élargir la fenêtre pour
  // le pire cas la rendrait énorme pour tout le monde ; la replier ne coûte qu'une ligne,
  // et seulement aux personnages qui en ont besoin.
  char sfx[64] = "";  // détail de la stat COURANTE, consommé (et vidé) par stat()
  auto stat = [&](const char* label, const char* value, const char* tip) {
    const ImVec2 rp = ImGui::GetCursorScreenPos();
    const float nw = ImGui::CalcTextSize(label).x;  // fond limité au libellé
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(rp.x - 3.0f, rp.y - 1.0f),
        ImVec2(rp.x + nw + 5.0f, rp.y + ImGui::GetTextLineHeight() + 1.0f), kRowBg, 4.0f);
    ImGui::TextColored(ro::pal::kBlack, "%s", label);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine();
    ImGui::SetCursorPosX(val_x);
    ImGui::TextColored(ro::pal::kBlack, "%s", value);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    if (sfx[0]) {
      const float after =
          val_x + ImGui::CalcTextSize(value).x + ImGui::GetStyle().ItemSpacing.x;
      if (after + ImGui::CalcTextSize(sfx).x <= right) ImGui::SameLine();
      else                                             ImGui::SetCursorPosX(val_x);
      ImGui::TextColored(ro::pal::kBlack, "%s", sfx);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
      sfx[0] = '\0';  // consommé : la stat suivante repart sans détail
    }
  };
  char b[112];
  // Accole « (label ±X) » au détail de la valeur courante (équip, refine…) si non nul.
  auto append = [&](const char* label, int contrib) {
    if (!bonus_.valid || contrib == 0) return;
    const size_t n = std::strlen(sfx);
    if (n) std::snprintf(sfx + n, sizeof(sfx) - n, "  (%s %+d)", label, contrib);
    else   std::snprintf(sfx, sizeof(sfx), "(%s %+d)", label, contrib);
  };
  auto appendEquip = [&](int contrib) { append(i18n::Tr("équip"), contrib); };
  // Variante % (ex. DEF de refine, qui alimente la réduction en %).
  auto appendPct = [&](const char* label, int contrib) {
    if (!bonus_.valid || contrib == 0) return;
    const size_t n = std::strlen(sfx);
    if (n) std::snprintf(sfx + n, sizeof(sfx) - n, "  (%s %+d%%)", label, contrib);
    else   std::snprintf(sfx, sizeof(sfx), "(%s %+d%%)", label, contrib);
  };
  atk_row_dy_ = ImGui::GetCursorPosY() - pane_top;  // repère pour le volet staff
  std::snprintf(b, sizeof(b), "%d + %d", s.atk1, s.atk2);
  appendEquip(bonus_.eatk);
  append("refine", bonus_.refine_atk);
  stat("ATK", b, i18n::Tr("Attaque physique (arme + statut) : détermine les dégâts des coups physiques."));
  std::snprintf(b, sizeof(b), "%d ~ %d", s.matk_min, s.matk_max);
  appendEquip(bonus_.ematk);
  stat("MATK", b, i18n::Tr("Attaque magique : détermine les dégâts des sorts."));
  std::snprintf(b, sizeof(b), "%d%% + %d", s.def_s, s.def_h);
  appendPct("refine", bonus_.refine_def);
  stat("DEF", b, i18n::Tr("Défense physique : réduction en % (VIT/équip, def1) + réduction plate (def2). « refine » = part du refine des armures (dans la réduction %)."));
  std::snprintf(b, sizeof(b), "%d%% + %d", s.mdef_s, s.mdef_h);
  stat("MDEF", b, i18n::Tr("Défense magique : réduction en % (INT, mdef1) + réduction plate (mdef2)."));
  // 🔴 HIT et FLEE valent « niveau de base + DEX/AGI », et la part du NIVEAU
  // s'arrête à 600 (battle_config.maxstatlevelcalc, conf/import/battle_conf.txt).
  // Rien ne le signale en jeu : un personnage niveau 999 a 400 points de HIT et
  // de FLEE qui ne sont jamais arrivés. C'est ce que ces deux bulles disent.
  //
  // ⚠ 600 est écrit en dur : c'est une valeur de conf serveur. Si elle change
  // là-bas, ces deux textes mentent — les mettre à jour en même temps.
  std::snprintf(b, sizeof(b), "%d", s.hit);
  stat("HIT", b,
       i18n::Tr("Précision : comparée au FLEE de la cible pour déterminer si vous "
       "touchez.\n\nHIT = niveau de base + DEX. La part du niveau s'arrête au "
       "niveau 600 : au-delà, seule la DEX fait encore monter le HIT."));
  std::snprintf(b, sizeof(b), "%d", s.flee);
  stat("FLEE", b,
       i18n::Tr("Esquive : comparée au HIT de la cible, réduit la probabilité d'être "
       "touché.\n\nFLEE = niveau de base + AGI. La part du niveau s'arrête au "
       "niveau 600 : au-delà, seule l'AGI fait encore monter le FLEE."));
  // 🔴 CRI et Esq.P sont des POUR CENT ENTIERS, PAS des dixièmes. Les diviser
  // encore par 10 divisait le vrai taux par dix.
  //
  // Preuve par la formule, indépendante de toute lecture d'adresse : en pré-RE
  // `cri = 10 + LUK*10/3` et `flee2 = LUK + 10`, tous deux en dixièmes de %.
  // À LUK 399 ça fait 1340 et 409 dixièmes, soit 134 % et 40,9 %. Le serveur
  // envoie `cri/10` et `flee2/10` (clif.cpp, SP_CRITICAL / SP_FLEE2) : la
  // globale vaut donc 134 et 40. L'affichage en « 13,4 % » et « 4,0 % » était
  // faux d'un facteur 10 — et un joueur à 134 % de critique en fait bien un à
  // chaque coup, ce qu'un « 13,4 % » ne laissait pas voir.
  //
  // ⚠ Oui, un taux peut dépasser 100 % : le jet est `rnd()%1000 < cri`, tout
  // ce qui est au-dessus est simplement toujours vrai.
  std::snprintf(b, sizeof(b), "%d%%", s.crit);
  stat("CRI", b, i18n::Tr("Taux de coup critique (%) : un critique ignore la DEF et ne rate jamais."));
  std::snprintf(b, sizeof(b), "%d", rag::AspdFromAmotion(s.aspd_raw));
  stat("ASPD", b, i18n::Tr("Vitesse d'attaque : plus elle est haute, plus vous frappez souvent."));
  std::snprintf(b, sizeof(b), "%d%%", s.pdodge);
  stat("Esq.P", b, i18n::Tr("Esquive parfaite (%, via LUK) : évite totalement une attaque, même critique."));
  // Toutes les rangées que le volet staff sait viser ont été relevées : il peut
  // aligner ses commandes cette frame.
  stat_rows_valid_ = true;

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
      ImGui::TextColored(ro::pal::kBlack, "%s", label);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
      ImGui::SameLine();
      const float after = start_x + nw + ImGui::GetStyle().ItemSpacing.x;
      ImGui::SetCursorPosX(after > val_x ? after : val_x);  // aligné, sauf libellé trop long
      ImGui::TextColored(ro::pal::kBlack, "%s", value);
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
    if (any_flat && ImGui::CollapsingHeader(i18n::Tr("Bonus d'équipement"), kSec)) {
    pct(i18n::Tr("Mêlée"), bonus_.melee_pct, i18n::Tr("Dégâts de mêlée à mains nues (%)."));
    pct(i18n::Tr("Distance"), bonus_.ranged_pct, i18n::Tr("Dégâts des attaques à distance (%)."));
    pct(i18n::Tr("Dég. crit."), bonus_.crit_dmg_pct, i18n::Tr("Dégâts des coups critiques (%)."));
    flat(i18n::Tr("PV max"), bonus_.hp_add, i18n::Tr("PV max ajoutés par l'équipement."));
    flat(i18n::Tr("SP max"), bonus_.sp_add, i18n::Tr("SP max ajoutés par l'équipement."));
    flat(i18n::Tr("ASPD"), bonus_.aspd_add, i18n::Tr("Vitesse d'attaque ajoutée (valeur plate)."));
    pct(i18n::Tr("Cast var."), bonus_.vcast_pct, i18n::Tr("Temps de cast variable (%). Négatif = réduction."));
    pct(i18n::Tr("Cast fixe"), bonus_.fcast_pct, i18n::Tr("Temps de cast fixe (%). Négatif = réduction."));
    // Lot A — offensif
    pct(i18n::Tr("ATK %"), bonus_.atk_pct, i18n::Tr("Bonus d'ATK physique global (%)."));
    pct(i18n::Tr("MATK %"), bonus_.matk_pct, i18n::Tr("Bonus d'ATK magique global (%)."));
    pct(i18n::Tr("Renvoi mêlée"), bonus_.dmg_ret_melee, i18n::Tr("Renvoie une part des dégâts de mêlée reçus (%)."));
    pct(i18n::Tr("Renvoi dist."), bonus_.dmg_ret_ranged, i18n::Tr("Renvoie une part des dégâts à distance reçus (%)."));
    pct(i18n::Tr("Renvoi mag."), bonus_.dmg_ret_magic, i18n::Tr("Renvoie une part des dégâts magiques reçus (%)."));
    pct(i18n::Tr("Double att."), bonus_.double_pct, i18n::Tr("Chance de frapper deux fois (%)."));
    pct(i18n::Tr("Coup parfait"), bonus_.perfect_hit, i18n::Tr("Chance de coup parfait (%) : ignore FLEE et DEF."));
    // Lot B — survie
    pct(i18n::Tr("PV max %"), bonus_.hp_pct, i18n::Tr("Bonus de PV maximum (%)."));
    pct(i18n::Tr("SP max %"), bonus_.sp_pct, i18n::Tr("Bonus de SP maximum (%)."));
    pct(i18n::Tr("Régén. PV"), bonus_.hp_regen_pct, i18n::Tr("Récupération naturelle de PV (%)."));
    pct(i18n::Tr("Régén. SP"), bonus_.sp_regen_pct, i18n::Tr("Récupération naturelle de SP (%)."));
    pct(i18n::Tr("Réduc. crit"), bonus_.crit_def_pct, i18n::Tr("Réduit la probabilité de subir un critique (%)."));
    flat("PV/kill", bonus_.hp_on_kill, i18n::Tr("PV récupérés en tuant un ennemi."));
    flat("SP/kill", bonus_.sp_on_kill, i18n::Tr("SP récupérés en tuant un ennemi."));
    pct(i18n::Tr("Incassable"), bonus_.unbreak_pct, i18n::Tr("Chance d'éviter la casse d'un équipement (%)."));
    // Lot C — utilitaire
    pct(i18n::Tr("Potions PV"), bonus_.pot_hp_pct, i18n::Tr("Efficacité des objets de soin PV (%)."));
    pct(i18n::Tr("Potions SP"), bonus_.pot_sp_pct, i18n::Tr("Efficacité des objets de soin SP (%)."));
    pct(i18n::Tr("Soin donné"), bonus_.heal_up_pct, i18n::Tr("Puissance des soins que vous prodiguez (%)."));
    pct(i18n::Tr("Délai skill"), bonus_.delay_pct, i18n::Tr("After-cast delay (%). Négatif = réduction."));
    flat(i18n::Tr("Cast var. ms"), bonus_.add_vcast_ms, i18n::Tr("Ajout/retrait au cast variable, en millisecondes."));
    flat(i18n::Tr("Cast fixe ms"), bonus_.add_fcast_ms, i18n::Tr("Ajout/retrait au cast fixe, en millisecondes."));
    pct(i18n::Tr("Vol"), bonus_.steal_pct, i18n::Tr("Taux de vol d'objets (%)."));
    // Lot E — réduction par type d'attaque + splash
    pct(i18n::Tr("Réduc. mêlée"), bonus_.def_melee_pct, i18n::Tr("Réduit les dégâts de mêlée reçus (%)."));
    pct(i18n::Tr("Réduc. distance"), bonus_.def_ranged_pct, i18n::Tr("Réduit les dégâts à distance reçus (%)."));
    pct(i18n::Tr("Réduc. magie"), bonus_.def_magic_pct, i18n::Tr("Réduit les dégâts magiques reçus (%)."));
    pct(i18n::Tr("Réduc. divers"), bonus_.def_misc_pct, i18n::Tr("Réduit les dégâts divers reçus (%)."));
    flat(i18n::Tr("Splash"), bonus_.splash, i18n::Tr("Portée de la zone d'effet de vos attaques (cases)."));
    flat(i18n::Tr("Splash+"), bonus_.splash_add, i18n::Tr("Portée de splash additionnelle (cases)."));
    // Lot F — vol de vie
    pct(i18n::Tr("Vol PV"), bonus_.hp_drain_pct, i18n::Tr("PV volés à chaque attaque (% des dégâts)."));
    pct(i18n::Tr("Vol SP"), bonus_.sp_drain_pct, i18n::Tr("SP volés à chaque attaque (% des dégâts)."));
    // Lot G — très niche
    pct(i18n::Tr("Casse arme"), bonus_.break_weapon_pct, i18n::Tr("Chance de casser l'arme de la cible (%)."));
    pct(i18n::Tr("Casse armure"), bonus_.break_armor_pct, i18n::Tr("Chance de casser l'armure de la cible (%)."));
    pct(i18n::Tr("Zeny bonus"), bonus_.zeny_bonus_pct, i18n::Tr("Bonus de Zeny obtenu sur les monstres (%)."));
    pct(i18n::Tr("Transforme"), bonus_.classchange_pct, i18n::Tr("Chance de transformer la cible en un autre monstre (%)."));
    pct(i18n::Tr("Réduc. renvoi"), bonus_.dmg_ret_reduce, i18n::Tr("Réduit les dégâts que vous subissez du renvoi (%)."));
    flat(i18n::Tr("PV au sort"), bonus_.magic_hp_gain, i18n::Tr("PV récupérés en lançant un sort."));
    flat(i18n::Tr("SP au sort"), bonus_.magic_sp_gain, i18n::Tr("SP récupérés en lançant un sort."));
    }  // ── fin « Bonus d'équipement »

    // Conditionnels : (code, idx) -> libellé via les tables de noms.
    auto nameOf = [](const char* const* tbl, int n, int idx) -> const char* {
      return (idx >= 0 && idx < n) ? tbl[idx] : "?";
    };
    if (!bonus_.cond.empty() && ImGui::CollapsingHeader(i18n::Tr("Conditionnels"), kSec))
    for (const auto& c : bonus_.cond) {
      const char* kind = "Bonus";
      const char* who = "?";
      char who_buf[24];  // repli pour les groupes RC2 sans libellé
      auto rc2 = [&](int idx) -> const char* {
        if (idx >= 0 && idx < IM_ARRAYSIZE(kRace2Name) && kRace2Name[idx][0]) return kRace2Name[idx];
        std::snprintf(who_buf, sizeof(who_buf), i18n::Tr("groupe #%d"), idx);
        return who_buf;
      };
      switch (c.code) {
        case kBscSubEle:  kind = i18n::Tr("Résist. vs"); who = nameOf(kEleName, IM_ARRAYSIZE(kEleName), c.idx); break;
        case kBscSubRace: kind = i18n::Tr("Résist. vs"); who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscSubSize: kind = i18n::Tr("Résist. vs"); who = nameOf(kSizeName, IM_ARRAYSIZE(kSizeName), c.idx); break;
        case kBscAddEle:  kind = i18n::Tr("Dégâts vs");  who = nameOf(kEleName, IM_ARRAYSIZE(kEleName), c.idx); break;
        case kBscAddRace: kind = i18n::Tr("Dégâts vs");  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscAddSize: kind = i18n::Tr("Dégâts vs");  who = nameOf(kSizeName, IM_ARRAYSIZE(kSizeName), c.idx); break;
        case kBscMAddEle:  kind = i18n::Tr("Dég. mag. vs"); who = nameOf(kEleName, IM_ARRAYSIZE(kEleName), c.idx); break;
        case kBscMAddRace: kind = i18n::Tr("Dég. mag. vs"); who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscMAddSize: kind = i18n::Tr("Dég. mag. vs"); who = nameOf(kSizeName, IM_ARRAYSIZE(kSizeName), c.idx); break;
        case kBscCritRace:    kind = i18n::Tr("Crit vs");       who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscIgnDefRace:  kind = i18n::Tr("Ignore DEF vs");  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscIgnMdefRace: kind = i18n::Tr("Ignore MDEF vs"); who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscIgnDefClass:  kind = i18n::Tr("Ignore DEF vs");  who = nameOf(kClassName, IM_ARRAYSIZE(kClassName), c.idx); break;
        case kBscIgnMdefClass: kind = i18n::Tr("Ignore MDEF vs"); who = nameOf(kClassName, IM_ARRAYSIZE(kClassName), c.idx); break;
        case kBscSubdefEle:   kind = i18n::Tr("Résist. arme");   who = nameOf(kEleName, IM_ARRAYSIZE(kEleName), c.idx); break;
        case kBscSubClass:    kind = i18n::Tr("Réduc. vs");      who = nameOf(kClassName, IM_ARRAYSIZE(kClassName), c.idx); break;
        case kBscSubRace2:  kind = i18n::Tr("Réduc. vs");  who = rc2(c.idx); break;
        case kBscExpRace:   kind = i18n::Tr("EXP vs");  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscExpClass:  kind = i18n::Tr("EXP vs");  who = nameOf(kClassName, IM_ARRAYSIZE(kClassName), c.idx); break;
        case kBscDropRace:  kind = i18n::Tr("Drop vs"); who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscDropClass: kind = i18n::Tr("Drop vs"); who = nameOf(kClassName, IM_ARRAYSIZE(kClassName), c.idx); break;
        // Très niche
        case kBscDefsetRace:   kind = i18n::Tr("DEF fixée vs");  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscMdefsetRace:  kind = i18n::Tr("MDEF fixée vs"); who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscHpVanishRace: kind = i18n::Tr("Vanish PV vs");  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscSpVanishRace: kind = i18n::Tr("Vanish SP vs");  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscComaRace:     kind = i18n::Tr("Coma vs");  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscComaClass:    kind = i18n::Tr("Coma vs");  who = nameOf(kClassName, IM_ARRAYSIZE(kClassName), c.idx); break;
        case kBscIgnResRace:   kind = i18n::Tr("Ignore RES vs");  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscIgnMresRace:  kind = i18n::Tr("Ignore MRES vs"); who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        case kBscMAddRace2:      kind = i18n::Tr("Dég. mag. vs");  who = rc2(c.idx); break;
        case kBscIgnMdefRace2:   kind = i18n::Tr("Ignore MDEF vs"); who = rc2(c.idx); break;
        case kBscSpGainRace:   kind = i18n::Tr("SP/kill vs");  who = nameOf(kRaceName, IM_ARRAYSIZE(kRaceName), c.idx); break;
        default: break;
      }
      char label[64];
      std::snprintf(label, sizeof(label), "%s %s", kind, who);
      std::snprintf(vb, sizeof(vb), "%+d%%", c.value);
      bonusStat(label, vb, i18n::Tr("Bonus conditionnel : ne s'applique que contre ce type de cible."));
    }

    // Bonus liés à un skill : nom résolu via le wrapper Lua natif (localisé).
    auto skillName = [](uint16_t id) -> const char* {
      const char* n = lua::SkillName(id);
      return (n && *n) ? n : "?";
    };
    if (!bonus_.skills.empty() && ImGui::CollapsingHeader(i18n::Tr("Skills & statuts"), kSec))
    for (const auto& sk : bonus_.skills) {
      char label[96];
      const char* tip = i18n::Tr("Bonus lié à un skill.");
      switch (sk.code) {
        case kBskAutospell:
        case kBskAutospellHit: {
          const char* pre = (sk.code == kBskAutospellHit) ? "Riposte" : "Autocast";
          const char* nm = skillName(sk.skill_id);
          if (sk.lv > 0)
            std::snprintf(label, sizeof(label), i18n::Tr("%s %s Niv %d"), pre, nm, sk.lv);
          else
            std::snprintf(label, sizeof(label), "%s %s", pre, nm);
          std::snprintf(vb, sizeof(vb), "%d,%d%%", sk.value / 10, sk.value % 10);  // ‰ -> %
          tip = (sk.code == kBskAutospellHit)
                    ? i18n::Tr("Chance de lancer ce sort automatiquement quand vous êtes touché.") : i18n::Tr("Chance de lancer ce sort automatiquement en attaquant.");
          break;
        }
        case kBskSkillAtk:
          std::snprintf(label, sizeof(label), i18n::Tr("Dégâts %s"), skillName(sk.skill_id));
          std::snprintf(vb, sizeof(vb), "%+d%%", sk.value);
          tip = i18n::Tr("Bonus de dégâts sur ce skill précis (%).");
          break;
        case kBskAddeff:
        case kBskAddeffHit: {
          // skill_id porte l'EFST du statut, résolu en nom via GetStateIconDescript.
          const char* pre = (sk.code == kBskAddeffHit) ? i18n::Tr("Riposte statut") : "Inflige";
          std::snprintf(label, sizeof(label), "%s %s", pre, StatusName(sk.skill_id));
          std::snprintf(vb, sizeof(vb), "%d,%02d%%", sk.value / 100, std::abs(sk.value) % 100);  // 1/100% -> %
          tip = (sk.code == kBskAddeffHit)
                    ? i18n::Tr("Chance d'infliger ce statut à l'attaquant quand vous êtes touché.") : i18n::Tr("Chance d'infliger ce statut à la cible en attaquant.");
          break;
        }
        case kBskReseff: {
          // skill_id porte l'EFST du statut (résolu via GetStateIconDescript).
          std::snprintf(label, sizeof(label), i18n::Tr("Résist. %s"), StatusName(sk.skill_id));
          std::snprintf(vb, sizeof(vb), "%d,%02d%%", sk.value / 100, std::abs(sk.value) % 100);  // 1/100% -> %
          tip = i18n::Tr("Résistance à ce statut (chance/durée réduite).");
          break;
        }
        case kBskSubskill:
          std::snprintf(label, sizeof(label), i18n::Tr("Réduc. %s"), skillName(sk.skill_id));
          std::snprintf(vb, sizeof(vb), "%+d%%", sk.value);
          tip = i18n::Tr("Réduit les dégâts subis de ce skill (%).");
          break;
        case kBskAutospellSkill: {
          // Deux noms de skill (casté + déclencheur) : copier le 1er avant le 2e appel.
          char cast[48];
          std::strncpy(cast, skillName(sk.skill_id), sizeof(cast) - 1);
          cast[sizeof(cast) - 1] = '\0';
          const char* trig = sk.aux ? skillName(sk.aux) : "?";
          if (sk.lv > 0)
            std::snprintf(label, sizeof(label), i18n::Tr("Autocast %s Niv %d sur %s"), cast, sk.lv, trig);
          else
            std::snprintf(label, sizeof(label), i18n::Tr("Autocast %s sur %s"), cast, trig);
          std::snprintf(vb, sizeof(vb), "%d,%d%%", sk.value / 10, sk.value % 10);  // ‰ -> %
          tip = i18n::Tr("Chance de lancer ce sort en utilisant le skill déclencheur.");
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
            case kBskSkillSprate:    pre = i18n::Tr("Coût SP");   unit = 0; break;
            case kBskSkillSpcost:    pre = i18n::Tr("Coût SP");   unit = 2; break;
            case kBskSkillVcastrate: pre = i18n::Tr("Cast var."); unit = 0; break;
            case kBskSkillFcastrate: pre = i18n::Tr("Cast fixe"); unit = 0; break;
            case kBskSkillVcast:     pre = i18n::Tr("Cast var."); unit = 1; break;
            case kBskSkillFcast:     pre = i18n::Tr("Cast fixe"); unit = 1; break;
            case kBskSkillCooldown:  pre = "Cooldown";  unit = 1; break;
            case kBskSkillDelay:     pre = i18n::Tr("Délai");     unit = 0; break;
            case kBskSkillHeal:      pre = "Soin";      unit = 0; break;
            case kBskSkillHeal2:     pre = i18n::Tr("Soin reçu"); unit = 0; break;
            case kBskSkillBlown:     pre = "Knockback"; unit = 2; break;
          }
          std::snprintf(label, sizeof(label), "%s %s", pre, skillName(sk.skill_id));
          if (unit == 0)      std::snprintf(vb, sizeof(vb), "%+d%%", sk.value);
          else if (unit == 1) std::snprintf(vb, sizeof(vb), i18n::Tr("%+d ms"), sk.value);
          else                std::snprintf(vb, sizeof(vb), "%+d", sk.value);
          tip = i18n::Tr("Modificateur appliqué à ce skill précis.");
          break;
        }
        default:
          std::snprintf(label, sizeof(label), "%s", skillName(sk.skill_id));
          std::snprintf(vb, sizeof(vb), "%+d", sk.value);
          break;
      }
      bonusStat(label, vb, tip);
    }

    // Bonus liés à un item : nom résolu via le DB item (itemcell::NameById).
    if (!bonus_.items.empty() && ImGui::CollapsingHeader(i18n::Tr("Objets"), kSec))
    for (const auto& it : bonus_.items) {
      char label[96];
      if (it.code == kBsiAddDropGroup)  // nameid porte l'id de GROUPE, pas d'item
        std::snprintf(label, sizeof(label), i18n::Tr("Drop groupe #%u"), it.nameid);
      else
        std::snprintf(label, sizeof(label), i18n::Tr("Drop %s"), itemcell::NameById(it.nameid));
      std::snprintf(vb, sizeof(vb), "%d,%02d%%", it.rate / 100, std::abs(it.rate) % 100);  // 1~10000 -> %
      bonusStat(label, vb, i18n::Tr("Chance de drop bonus de cet objet en tuant un monstre."));
    }
  }
}

// ── Volet STAFF ──────────────────────────────────────────────────────────────
// Le pendant ACTIF du volet stats : chaque commande y est posée en face de la
// valeur qu'elle change — les six ajusteurs devant STR..LUK, la remise à zéro
// devant les points de statut, la classe devant l'ATK.
//
// 🔴 Tout part par SendAtCommand, c'est-à-dire par le canal de chat, exactement
// comme la commande tapée à la main : mêmes droits de groupe, mêmes refus, même
// journalisation serveur. RIEN n'est validé ici — pas même l'existence de la
// classe choisie. Ce volet est une SAISIE, le serveur reste le juge, et son refus
// arrive dans le chat comme d'habitude.
//
// ⚠ TOUT ce volet n'est pas ouvert au même niveau. conf/import/groups.yml accorde
// au groupe 80 (le seuil d'IsStaff) `jobchange`, `allstats`, `allskill`, `resetstat`,
// `resetskill`, `speed`, `blvl`, `jlvl`, `refine`, `heal`, `repairall`, `hide`,
// `zeny`, `itemreset` et `option` — mais PAS les six ajusteurs de stat (`@str`…),
// ni `@stpoint`, `@skpoint` et `@resetcooltime`, qui demandent le 99.
//
// Ils restent VISIBLES pour tout le staff, et c'est délibéré : le gate d'affichage
// suit IsStaff, et la liste des commandes d'un groupe est une donnée SERVEUR qui
// peut changer sans que la DLL le sache — la recopier ici la ferait mentir au
// premier ajout. Un staff de niveau 80 qui clique reçoit le refus habituel.
void CharacterSheet::DrawStaffPanel() {
  const float top = ImGui::GetCursorPosY();
  // Pose le curseur sur la rangée `dy` du volet stats. Jamais EN ARRIÈRE : un
  // contrôle plus haut qu'une rangée de texte (un combo fait une hauteur de cadre,
  // une dérivée une hauteur de ligne) déborde forcément sur la rangée suivante, et
  // reculer le curseur le ferait chevaucher le précédent. L'alignement est donc au
  // mieux — exact sur les six stats primaires, qui font la même hauteur des deux
  // côtés, approché ensuite.
  auto alignTo = [&](float dy) {
    if (!stat_rows_valid_) return;
    const float y = top + dy;
    if (y > ImGui::GetCursorPosY()) ImGui::SetCursorPosY(y);
  };

  const ImGuiStyle& st = ImGui::GetStyle();
  const float btn   = ImGui::GetFrameHeight();  // carrés « - » et « + »
  const float input = ImGui::CalcTextSize("999999999").x + st.FramePadding.x * 2.0f;

  // Une rangée d'ajustement : « - », saisie du pas, « + ». `cmd` est la commande NUE
  // (« str », « blvl »…) ; le signe vient du bouton, jamais de la saisie — un « -5 »
  // tapé dans le champ puis un clic sur « + » enverrait « @str +-5 ».
  //
  // `hi` borne la saisie, et il n'est pas le même partout : une stat plafonne à
  // quelques centaines là où le zeny se compte en millions. Un plafond unique aurait
  // silencieusement rogné les montants de l'un ou laissé l'autre écrire n'importe quoi.
  auto adjustRow = [&](const char* id, const char* cmd, int* step, int hi, const char* tip) {
    ImGui::PushID(id);
    if (*step < 1)  *step = 1;
    if (*step > hi) *step = hi;
    char line[64];
    if (ro::RoButton("-", btn, btn)) {
      std::snprintf(line, sizeof(line), "@%s -%d", cmd, *step);
      SendAtCommand(line);
    }
    mui::Tooltip(tip);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(input);
    ImGui::InputInt("##v", step, 0, 0);  // step=0 : pas de flèches ImGui (non skinnées)
    mui::Tooltip(tip);
    ImGui::SameLine();
    if (ro::RoButton("+", btn, btn)) {
      std::snprintf(line, sizeof(line), "@%s +%d", cmd, *step);
      SendAtCommand(line);
    }
    mui::Tooltip(tip);
    ImGui::PopID();
  };

  // ── Six ajusteurs, en face de STR..LUK ──────────────────────────────────────
  char tip[192];
  for (int i = 0; i < 6; ++i) {
    alignTo(stat_row_dy_[i]);
    // Nom de la commande = le nom de la stat en minuscules, dans le MÊME ordre que
    // rAthena (parameter_names[] : str, agi, vit, int, dex, luk).
    char cmd[4];
    for (int k = 0; k < 3; ++k)
      cmd[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(kStatName[i][k])));
    cmd[3] = '\0';
    std::snprintf(tip, sizeof(tip),
                  i18n::Tr("@%s : ajoute ou retire le montant saisi à %s.\n"
                           "C'est un AJUSTEMENT, pas une valeur cible."),
                  cmd, kStatName[i]);
    adjustRow(kStatName[i], cmd, &staff_stat_step_[i], kStatStepMax, tip);
  }

  // ── Remise à zéro / tout au max, en face des points de statut ───────────────
  alignTo(stat_points_dy_);
  const float act_w = ro::MaxButtonWidth({i18n::Tr("Reset"), i18n::Tr("Max")});
  if (ro::RoButton(i18n::Tr("Reset"), act_w, 0.0f)) SendAtCommand("@resetstat");
  mui::Tooltip(i18n::Tr("@resetstat : remet les six stats à leur base et rend tous les "
                        "points de statut dépensés."));
  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Max"), act_w, 0.0f)) SendAtCommand("@allstat");
  mui::Tooltip(i18n::Tr("@allstat : monte les six stats à leur maximum."));

  // ── Classe, en face de l'ATK ────────────────────────────────────────────────
  alignTo(atk_row_dy_);
  // Le job COURANT sert d'aperçu tant que rien n'a été choisi dans la liste : le
  // menu montre alors ce que le personnage est, pas une case vide.
  const int own_job = OwnJobIdSEH();
  // 🔴 L'aperçu montre la classe COURANTE, jamais le dernier choix cliqué : le
  // serveur a pu le refuser (classe factice, `pcdb_checkid` négatif), et la classe
  // peut changer par bien d'autres chemins — monture, déguisement, quête de job.
  // Afficher ce qu'on a demandé plutôt que ce qu'on est ferait mentir le volet.
  // `JobName` répond pour TOUT id, y compris ceux que la liste ne propose pas.
  const char* preview = JobName(own_job);
  ImGui::SetNextItemWidth(-1.0f);
  if (ro::RoBeginCombo("##cs_staff_job", preview)) {
    const bool just_opened = ImGui::IsWindowAppearing();
    // Filtre REMIS À ZÉRO à chaque ouverture : gardé d'une fois sur l'autre, il
    // rouvrirait la liste presque vide sans que rien ne dise pourquoi.
    if (just_opened) {
      staff_job_filter_[0] = '\0';
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##jobfilter", i18n::Tr("Rechercher…"), staff_job_filter_,
                             sizeof(staff_job_filter_));
    // Filtre insensible à la casse sur le libellé AFFICHÉ (celui du client) et sur
    // l'id : « 4054 » comme « rune » trouvent le Rune Knight.
    char needle[sizeof(staff_job_filter_)];
    for (size_t k = 0; k < sizeof(needle); ++k)
      needle[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(staff_job_filter_[k])));
    const char* last_group = nullptr;
    for (int i = 0; i < kJobListCount; ++i) {
      const char* label = JobListLabel(i);
      if (needle[0]) {
        char hay[80];
        std::snprintf(hay, sizeof(hay), "%s %d", label, kJobList[i].id);
        for (char* p = hay; *p; ++p)
          *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
        if (!std::strstr(hay, needle)) continue;
      }
      if (last_group != kJobList[i].group) {  // en-tête de famille (pointeurs de la table)
        last_group = kJobList[i].group;
        ImGui::TextDisabled("%s", i18n::Tr(last_group));
      }
      char row[80];
      std::snprintf(row, sizeof(row), "%s (%d)", label, kJobList[i].id);
      const bool sel = (kJobList[i].id == own_job);
      if (ImGui::Selectable(row, sel)) {
        char line[32];
        std::snprintf(line, sizeof(line), "@job %d", kJobList[i].id);
        SendAtCommand(line);
        ImGui::CloseCurrentPopup();
      }
      if (sel && just_opened) ImGui::SetScrollHereY(0.5f);
    }
    ro::RoEndCombo();
  }
  mui::Tooltip(
      i18n::Tr("@job : change de classe.\n\n"
               "⚠ Sur ce serveur la commande enchaîne d'elle-même @blvl 999, "
               "@jlvl 100, @allskills et @allstats — changer de classe MAXE le "
               "personnage, ce n'est pas un simple changement d'apparence."));

  // ── Vitesse de marche ───────────────────────────────────────────────────────
  // La vitesse est un réglage SERVEUR (registre « gmspeed », reposé à la connexion) :
  // ce curseur ne fait que la MONTRER. On le recale sur chaque valeur NOUVELLE venue
  // du serveur — celle du login, ou celle d'un `@speed` tapé à la main.
  //
  // 🔴 Un front, et non un alignement à chaque frame : après notre propre envoi le
  // serveur renvoie la valeur qu'on affiche déjà, donc le curseur ne saute jamais en
  // arrière le temps de l'aller-retour.
  if (auto* mu = Bourgeon::Instance().moonlight_ui()) {
    const int server_speed = mu->walk_speed();
    if (server_speed != speed_seen_)
      speed_seen_ = staff_speed_ = staff_speed_sent_ = server_speed;
  }
  // 🔴 Envoyée au RELÂCHEMENT, jamais pendant le drag : chaque @speed est un
  // message de chat ET un status_calc_bl côté serveur, un par frame de drag
  // noierait le chat et ferait travailler le serveur pour rien.
  ImGui::SetNextItemWidth(-1.0f);
  ro::RoSliderInt("##cs_staff_speed", &staff_speed_, kSpeedMin, kSpeedUiMax, "%d");
  const bool speed_released = ImGui::IsItemDeactivatedAfterEdit();
  mui::Tooltip(
      i18n::Tr("@speed : vitesse de marche. PLUS BAS = PLUS RAPIDE ; 150 est la "
               "vitesse normale, 20 le plancher du serveur.\n"
               "Envoyé au relâchement du curseur."));
  if (speed_released && staff_speed_ != staff_speed_sent_) {
    char line[24];
    std::snprintf(line, sizeof(line), "@speed %d", staff_speed_);
    SendAtCommand(line);
    staff_speed_sent_ = staff_speed_;
  }
  // 🔴 Largeur EXPLICITE : `RoButton` traite un w négatif comme « taille auto », pas
  // comme le « remplis la place » d'ImGui — un -1 y donnerait un bouton au format du
  // libellé, pas un bouton pleine largeur.
  if (ro::RoButton(i18n::Tr("Vitesse normale"), ImGui::GetContentRegionAvail().x, 0.0f)) {
    // Une valeur NÉGATIVE, et non « 150 » : c'est ce que le serveur attend pour
    // relâcher le verrou permanent_speed qu'il pose dès que la vitesse diffère du
    // défaut. Envoyer 150 remettrait la bonne valeur mais garderait le verrou.
    SendAtCommand("@speed -1");
    staff_speed_ = staff_speed_sent_ = kSpeedDefault;
  }
  mui::Tooltip(i18n::Tr("@speed -1 : rend au personnage sa vitesse par défaut (150)."));

  // ── Niveaux, points et zeny ─────────────────────────────────────────────────
  // Ces trois rangées portent un LIBELLÉ devant l'ajusteur, contrairement aux six
  // stats dont le nom est déjà écrit en face, dans le volet stats. Une colonne
  // commune aux trois : sans elle, « Zeny » décalerait son « - » d'une dizaine de
  // pixels par rapport à ceux de « Base » et « Job ».
  const float lvl_lbl =
      std::max(std::max(ImGui::CalcTextSize(i18n::Tr("Base")).x,
                        ImGui::CalcTextSize(i18n::Tr("Job")).x),
               ImGui::CalcTextSize("Zeny").x);
  // Écrit le libellé puis pose le curseur en début de colonne d'ajustement.
  auto labelledRow = [&](const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ro::pal::kBlack, "%s", label);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + lvl_lbl -
                         ImGui::CalcTextSize(label).x + st.ItemSpacing.x);
  };
  labelledRow(i18n::Tr("Base"));
  adjustRow("blvl", "blvl", &staff_blvl_step_, kLevelStepMax,
            i18n::Tr("@blvl : gagne ou perd le nombre de niveaux de BASE saisi."));
  labelledRow(i18n::Tr("Job"));
  adjustRow("jlvl", "jlvl", &staff_jlvl_step_, kLevelStepMax,
            i18n::Tr("@jlvl : gagne ou perd le nombre de niveaux de JOB saisi."));
  // « Zeny » est un terme du jeu : il ne passe pas par le catalogue.
  labelledRow("Zeny");
  adjustRow("zeny", "zeny", &staff_zeny_step_, kZenyStepMax,
            i18n::Tr("@zeny : donne ou retire le montant saisi. Un retrait plus grand que "
                     "la bourse la vide sans passer en négatif."));

  // Points de statut : une SAISIE et un bouton, pas un ajusteur — `@stpoint` donne
  // un nombre de points, il n'y a pas de « retirer » qui ait un sens en face.
  ImGui::SetNextItemWidth(input);
  ImGui::InputInt("##cs_staff_points", &staff_stpoint_, 0, 0);
  if (staff_stpoint_ < 1)     staff_stpoint_ = 1;
  if (staff_stpoint_ > 30000) staff_stpoint_ = 30000;
  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Points"), ImGui::GetContentRegionAvail().x, 0.0f)) {
    char line[32];
    std::snprintf(line, sizeof(line), "@stpoint %d", staff_stpoint_);
    SendAtCommand(line);
  }
  mui::Tooltip(i18n::Tr("@stpoint : donne le nombre de points de statut saisi — de quoi "
                        "essayer les boutons « + » du volet stats."));

  ImGui::Separator();

  // ── Raffinage ───────────────────────────────────────────────────────────────
  // Un COMBO d'emplacement plutôt qu'un geste sur la case du mannequin : le clic
  // droit y ouvre déjà la description et le double-clic déséquipe. Inventer un
  // troisième geste caché sur la même case ne se découvrirait pas, et se
  // tromperait un jour de bouton sur une pièce qu'on ne voulait pas toucher.
  ImGui::SetNextItemWidth(-1.0f);
  if (ro::RoBeginCombo("##cs_staff_refpos", RefinePosLabel(staff_refine_pos_))) {
    for (int i = 0; i < kRefinePosCount; ++i)
      if (ImGui::Selectable(RefinePosLabel(i), i == staff_refine_pos_))
        staff_refine_pos_ = i;
    ro::RoEndCombo();
  }
  mui::Tooltip(i18n::Tr("Emplacement visé par le raffinage. « Toutes les pièces » couvre "
                        "tout ce qui est porté (c'est la position 0 de @refine)."));
  {
    // Même rangée que les stats, mais la commande porte DEUX arguments : le masque
    // d'emplacement puis l'ajustement. `adjustRow` n'en sait faire qu'un, on écrit
    // donc la rangée à la main plutôt que de tordre le lambda pour un seul appelant.
    ImGui::PushID("refine");
    if (staff_refine_step_ < 1)          staff_refine_step_ = 1;
    if (staff_refine_step_ > craftdata::kMaxRefine) staff_refine_step_ = craftdata::kMaxRefine;
    const uint32_t mask = kRefinePos[staff_refine_pos_].mask;
    char line[48];
    if (ro::RoButton("-", btn, btn)) {
      std::snprintf(line, sizeof(line), "@refine %u -%d", mask, staff_refine_step_);
      SendAtCommand(line);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(input);
    ImGui::InputInt("##rv", &staff_refine_step_, 0, 0);
    ImGui::SameLine();
    if (ro::RoButton("+", btn, btn)) {
      std::snprintf(line, sizeof(line), "@refine %u +%d", mask, staff_refine_step_);
      SendAtCommand(line);
    }
    mui::Tooltip(i18n::Tr("@refine : ajoute ou retire des niveaux de raffinage sur "
                          "l'emplacement choisi. Le maximum est +10 sur ce serveur "
                          "(pré-renewal)."));
    ImGui::PopID();
  }

  // ── Objets et état ──────────────────────────────────────────────────────────
  const float full_w = ImGui::GetContentRegionAvail().x;
  if (ro::RoButton(i18n::Tr("Soigner"), full_w, 0.0f)) SendAtCommand("@heal");
  mui::Tooltip(i18n::Tr("@heal : rend tous les PV et les SP."));
  if (ro::RoButton(i18n::Tr("Réparer tout"), full_w, 0.0f)) SendAtCommand("@repairall");
  mui::Tooltip(i18n::Tr("@repairall : répare toutes les pièces endommagées de l'inventaire."));
  // `@hide` est une BASCULE sans argument : le serveur regarde `pc_isinvisible` et
  // inverse. On ne tient donc aucun état ici — un état local se désynchroniserait au
  // premier `@hide` tapé à la main, et il n'y a rien à afficher qui soit sûr.
  if (ro::RoButton(i18n::Tr("Invisible"), full_w, 0.0f)) SendAtCommand("@hide");
  mui::Tooltip(i18n::Tr("@hide : bascule l'invisibilité GM. Retaper la commande (ou "
                        "recliquer) redevient visible."));

  if (ro::RoButton(i18n::Tr("État visuel"), full_w, 0.0f)) {
    // Amorçage sur ce que le CLIENT sait de l'état (les paquets de compagnon) :
    // `@option` REMPLACE l'état complet, une case oubliée retirerait la monture, le
    // cart ou le faucon sans que personne l'ait demandé.
    for (int i = 0; i < kOptionFlagCount; ++i) {
      const int bit = kOptionFlags[i].bit;
      staff_opt_flags_[i] = (bit == 32 && companion_.riding_active) ||
                            (bit == 16 && companion_.falcon_active);
    }
    staff_opt_cart_ = companion_.cart_active > 0 && companion_.cart_active <= 5
                          ? companion_.cart_active : 0;
    ImGui::OpenPopup("##cs_staff_option");
  }
  mui::Tooltip(i18n::Tr("@option : pose l'état visuel du personnage (monture, faucon, "
                        "Hiding, Ruwach…).\n\n"
                        "⚠ La commande REMPLACE l'état complet : ce qui n'est pas coché "
                        "est RETIRÉ. Les cases sont amorcées sur l'état connu du client "
                        "pour ne pas perdre la monture ou le cart en chemin.\n"
                        "Elle remet aussi à zéro les statuts (pétrifié, gelé, maudit…)."));
  if (ImGui::BeginPopup("##cs_staff_option")) {
    ImGui::TextDisabled("%s", i18n::Tr("Coché = présent. Le reste est retiré."));
    ImGui::Separator();
    ImGui::SetNextItemWidth(ro::Px(110.0f));
    char cart_label[24];
    if (staff_opt_cart_ == 0) std::snprintf(cart_label, sizeof(cart_label), "%s", i18n::Tr("aucun"));
    else                      std::snprintf(cart_label, sizeof(cart_label), "Cart %d", staff_opt_cart_);
    if (ro::RoBeginCombo("Cart##cs_staff_optcart", cart_label)) {
      for (int lv = 0; lv <= 5; ++lv) {
        char row[24];
        if (lv == 0) std::snprintf(row, sizeof(row), "%s", i18n::Tr("aucun"));
        else         std::snprintf(row, sizeof(row), "Cart %d", lv);
        if (ImGui::Selectable(row, lv == staff_opt_cart_)) staff_opt_cart_ = lv;
      }
      ro::RoEndCombo();
    }
    for (int i = 0; i < kOptionFlagCount; ++i)
      ro::RoCheckbox(kOptionFlags[i].label, &staff_opt_flags_[i]);
    ImGui::Separator();
    if (ro::RoButton(i18n::Tr("Appliquer"), 0.0f, 0.0f)) {
      int mask = kOptionCartBits[staff_opt_cart_];
      for (int i = 0; i < kOptionFlagCount; ++i)
        if (staff_opt_flags_[i]) mask |= kOptionFlags[i].bit;
      char line[32];
      // Les deux premiers paramètres à 0 : opt1/opt2 portent les vrais statuts, et
      // `ACMD_FUNC(option)` les écrase avec ce qu'on lui donne. 0 = « aucun ».
      std::snprintf(line, sizeof(line), "@option 0 0 %d", mask);
      SendAtCommand(line);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler"), 0.0f, 0.0f)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // ── Vider l'inventaire ──────────────────────────────────────────────────────
  // 🔴 EN DEUX TEMPS. `@itemreset` supprime TOUT l'inventaire et le serveur ne
  // demande aucune confirmation : le clic est irréversible. Même règle que la
  // suppression d'un homoncule et que la dissolution de guilde.
  if (!staff_itemreset_ask_) {
    if (ro::RoButton(i18n::Tr("Vider l'inventaire"), full_w, 0.0f))
      staff_itemreset_ask_ = true;
    mui::Tooltip(i18n::Tr("@itemreset : SUPPRIME tous les objets de l'inventaire. "
                          "Irréversible, et le serveur ne demande rien — la "
                          "confirmation est ici."));
  } else {
    const float half = (full_w - st.ItemSpacing.x) * 0.5f;
    if (ro::RoButton(i18n::Tr("Confirmer"), half, 0.0f)) {
      SendAtCommand("@itemreset");
      staff_itemreset_ask_ = false;
    }
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler"), half, 0.0f)) staff_itemreset_ask_ = false;
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

// Équipement : l'onglet du même nom, celui du mannequin.
void CharacterSheet::OpenEquipTab() {
  show_ = true;
  tab_ = 0;
  tab_request_ = 0;
}

// Status : le même onglet, mais avec le VOLET STATS déplié. Ce volet n'a pas de
// drapeau propre — il s'affiche quand la fenêtre est assez large (cf. show_stats
// dans OnRenderUI). On demande donc la largeur « wide » pour une frame, ce que
// `want_wide_` fait en forçant la contrainte de taille au rendu suivant.
void CharacterSheet::OpenStatusTab() {
  OpenEquipTab();
  want_wide_ = true;
}

// Guilde : l'onglet qui refait la fenêtre de guilde native (infos, roster, postes,
// relations, emblème). Il est déjà large d'office (`force_wide` sur tab_ == 4), donc
// rien à forcer ici.
void CharacterSheet::OpenGuildTab() {
  show_ = true;
  tab_ = kTabGuild;
  tab_request_ = kTabGuild;
}

// Homoncule : l'onglet qui refait les DEUX natives (fiche d'état 113 + arbre de
// compétences 114). Les deux demandes y mènent, comme les deux fenêtres de guilde
// mènent à l'onglet Guilde.
void CharacterSheet::OpenHomunTab() {
  show_ = true;
  tab_ = kTabHomun;
  tab_request_ = kTabHomun;
}

// Ferme une fenêtre native comme le ferait son X : UIWindowMgr_Close enregistre son
// rectangle puis la DÉTRUIT. Hors frame ImGui uniquement (appelée depuis OnTick),
// cf. la règle « pas de commande native pendant une frame ImGui ».
namespace {
void DestroyNativeWindow(int window_id) {
  __try {
    uiwnd::CloseWindow(window_id);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
}  // namespace

// Une des fenêtres natives que cette feuille remplace vient de naître : on la
// masque SUR-LE-CHAMP (sans quoi une frame native passe à l'écran) et on route la
// demande vers l'onglet correspondant. La destruction, elle, revient à OnTick — le
// natif manipule encore la fenêtre qu'il vient de créer.
void CharacterSheet::HandleReplacedNativeCreation(void* win, int window_id) {
  if (!win || !imgui_enabled_) return;
  __try {
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + uiwnd::kOffVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  // Une création survenue PENDANT un changement de map n'est pas une demande du
  // joueur : c'est l'interface qui se reconstruit et rouvre les fenêtres qui
  // l'étaient. On masque la native (toujours), mais on ne touche pas à l'état de
  // la feuille — sinon elle s'ouvrirait toute seule à chaque warp.
  if (Bourgeon::Instance().IsMapLoading()) return;
  // Ces entrées sont des BASCULEURS : redemander la vue qu'on regarde déjà la
  // referme. C'est nous qui portons cette bascule maintenant, puisque la native
  // est détruite aussitôt — le natif, lui, ne la voit jamais exister.
  switch (window_id) {
    case uiwnd::kUINewSkillListWnd:
      if (show_ && tab_ == 5) { show_ = false; return; }
      OpenSkillsTab();
      return;
    case uiwnd::kUIStatusWnd:
      // Le volet stats est la marque de cette vue-là : ouverte SANS lui, la
      // demande le déplie au lieu de tout refermer.
      if (show_ && tab_ == 0 && stats_panel_shown_) { show_ = false; return; }
      OpenStatusTab();
      return;
    case uiwnd::kUIEquipWnd:
      if (show_ && tab_ == 0) { show_ = false; return; }
      OpenEquipTab();
      return;
    case uiwnd::kUIGuildWnd:
    case uiwnd::kGuildNoneWndId:
      // Les deux mènent au même onglet : il montre la guilde quand on en a une, et
      // la création quand on n'en a pas — exactement le partage que fait le client.
      if (show_ && tab_ == kTabGuild) { show_ = false; return; }
      OpenGuildTab();
      return;
    case uiwnd::kUIHomunInfoWnd:
    case uiwnd::kHomunSkillWndId:
      // Idem : la fiche d'état (Alt+R) et l'arbre de compétences sont deux natives,
      // un seul onglet chez nous. La 114 n'arrive en pratique jamais ici — son
      // unique créateur était le bouton « btn_skill » de la 113, qui n'existe plus —
      // mais on la route pareil, au cas où le mode moderne serait activé alors
      // qu'elle est déjà à l'écran.
      if (show_ && tab_ == kTabHomun) { show_ = false; return; }
      OpenHomunTab();
      return;
    default:
      return;
  }
}

// Les trois natives remplacées sont DÉTRUITES, pas seulement masquées, et c'est ce
// qui fait marcher le routage des raccourcis comme des boutons du menu d'icônes :
// UIWindowMgr_ToggleWindowById (0x00812e60), leur chemin commun, FERME la fenêtre
// si elle existe et ne la crée que sinon. Une native laissée vivante — même
// invisible — avalerait donc un appui sur deux sans repasser par MakeWindow, en
// plus de garder le clavier (Entrée/Espace activent son bouton par défaut).
void CharacterSheet::OnTick() {
  if (!imgui_enabled_) return;
  // Pendant un changement de map le HUD natif est démonté puis reconstruit : on ne
  // touche à aucune fenêtre native. L'état d'avant le warp est conservé tel quel.
  if (Bourgeon::Instance().IsMapLoading()) return;
  // Filet : ces fenêtres ne devraient plus exister passé le hook de création, mais
  // elles renaissent par des chemins qui ne demandent rien au joueur — la
  // reconstruction de l'interface, ou l'activation du mode moderne alors qu'elles
  // sont déjà à l'écran. Les détruire ici couvre les deux cas.
  for (const int id : {uiwnd::kUINewSkillListWnd, uiwnd::kUIStatusWnd,
                       uiwnd::kUIEquipWnd, uiwnd::kUIGuildWnd, uiwnd::kGuildNoneWndId,
                       uiwnd::kUIHomunInfoWnd, uiwnd::kHomunSkillWndId})
    if (uiwnd::SafeFindWindow(id)) DestroyNativeWindow(id);
  // Les panneaux d'onglet de la fenêtre de guilde : le conteneur en crée un à
  // l'ouverture (et un autre à chaque clic d'onglet). Ils ne portent aucune donnée
  // qui nous manquerait — roster, relations et bannis vivent dans des globals, et
  // les POSTES, seule donnée que le client ne gardait que dans sa fenêtre, sont
  // parsés par cet onglet depuis les paquets (cf. guild_positions_).
  for (int id = uiwnd::kUIGuildPanelFirst; id <= uiwnd::kUIGuildPanelLast; ++id)
    if (uiwnd::SafeFindWindow(id)) DestroyNativeWindow(id);
}

void CharacterSheet::OnRenderUI() {
  if (!imgui_enabled_) return;

  // Hotkey Alt+F : bascule la fenetre (ImGui recoit l'input clavier du client, donc
  // ne fire que quand le jeu a le focus). VERIFIER live que Alt+F est libre.
  if (ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F, false))
    show_ = !show_;

  // Raccourcis de presets : actifs EN JEU même fenêtre fermée (swap rapide sans ouvrir).
  const bool in_game = ReadInt(rag::kBaseLevelAddr) > 0;
  if (in_game) ProcessPresetHotkeys();

  if (!show_) return;
  // Rendu de la fenêtre seulement en jeu (evite d'afficher des stats a zero au login).
  if (!in_game) return;

  // Aperçu de description : reposé par la case survolée pendant le rendu. Remis à zéro
  // ICI, faute de quoi la description de la dernière case survolée resterait à l'écran.
  hover_desc_.id = 0;

  if (need_pos_) {
    ImGui::SetNextWindowPos(ImVec2(240, 140), ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  // Trois tailles possibles : doll seul (narrow), doll+stats (wide) et, pour le
  // STAFF SEUL, doll+stats+staff (xwide) ; snap au drag. Le droit est relu à chaque
  // frame — comme celui de StaffTools, il doit pouvoir se retirer en cours de
  // session, et la fenêtre reperd alors son troisième cran.
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const float doll_pane_w  = DollPaneW();   // mesurées à la police courante
  const float stats_pane_w = StatsPaneW();
  const bool  staff_pane_allowed = IsStaff();
  const float staff_pane_w = staff_pane_allowed ? StaffPaneW() : 0.0f;
  g_win_snap.narrow = doll_pane_w + chrome_w_;
  g_win_snap.wide   = doll_pane_w + gap + stats_pane_w + chrome_w_;
  // Sans droit staff, le troisième cran se confond avec le second : `SnapCharSheetWidth`
  // retrouve alors sa bascule à deux crans sans avoir à connaître le gate.
  g_win_snap.xwide  = staff_pane_allowed ? g_win_snap.wide + gap + staff_pane_w
                                         : g_win_snap.wide;
  g_win_snap.valid  = true;
  // Les onglets Guilde (table des membres) et Grimoire (grille de 7 colonnes) ont
  // besoin de toute la largeur : on y interdit le repli étroit plutôt que de laisser
  // le contenu déborder.
  // `want_wide_` (posé par OpenStatusTab) élargit la fenêtre UNE frame, le temps
  // que le volet stats repasse au-dessus du seuil d'affichage. Le joueur peut la
  // rétrécir juste après : on ne fait qu'imposer la largeur d'ouverture.
  g_win_snap.force_wide = (tab_ == 4 || tab_ == 5) || want_wide_;
  want_wide_ = false;
  // Plafond = le cran le plus large DISPONIBLE : sans droit staff, `xwide` vaut
  // `wide` et la contrainte est celle d'avant.
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(g_win_snap.force_wide ? g_win_snap.wide : g_win_snap.narrow,
             ro::Px(450.0f)),
      ImVec2(g_win_snap.xwide, 10000.0f), SnapCharSheetWidth);
  ImGui::SetNextWindowSize(ImVec2(g_win_snap.wide, ro::Px(490.0f)),
                           ImGuiCond_FirstUseEver);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);

  // Pas de NoCollapse -> le skin RO affiche le bouton minimiser (repli barre de titre),
  // comme l'inventaire/le natif ; le repli est géré par le `if (!begun)` ci-dessous.
  ro::SetNextWindowPinnable();  // épingle : Échap ne referme plus la fiche
  const bool begun =
      ro::BeginRoWindow(i18n::Tr("Personnage###bourgeon_charsheet"), &show_,
                        ImGuiWindowFlags_None);
  // 🔴 PAS de CloseWindowOnEscape ici : BeginRoWindow inscrit déjà la fenêtre
  // (cf. ui/imgui_escape.h). Le doublon était inoffensif tant que les deux
  // inscriptions disaient la même chose — il ne l'est plus depuis l'épingle, qui
  // RETIRE la fenêtre de la pile : la seconde inscription l'y remettait, et
  // Échap refermait une fiche épinglée.
  if (!begun) { ro::EndRoWindow(); ImGui::PopStyleVar(5); return; }

  // Onglets Equipement / Costume / Presets / Titres / Guilde / Grimoire (+ Homoncule).
  // `tab_request_` (posé par OpenSkillsTab) force la sélection UNE frame : ImGui
  // choisit l'onglet au moment où il le dessine, un hook ne peut pas l'imposer.
  const bool homun_tab_visible = rag::homun::Present();
  if (ro::RoBeginTabBar("cs_tabs")) {
    auto flag = [this](int idx) {
      return tab_request_ == idx ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
    if (ImGui::BeginTabItem(i18n::Tr("Équipement"), nullptr, flag(0))) { tab_ = 0; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(i18n::Tr("Costume"),    nullptr, flag(1))) { tab_ = 1; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(i18n::Tr("Presets"),    nullptr, flag(2))) { tab_ = 2; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(i18n::Tr("Titres"),     nullptr, flag(3))) { tab_ = 3; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(i18n::Tr("Guilde"),     nullptr, flag(4))) { tab_ = 4; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(i18n::Tr("Grimoire"),   nullptr, flag(5))) { tab_ = 5; ImGui::EndTabItem(); }
    // Onglet Homoncule : présent seulement quand il y en a un — MÊME garde que le
    // raccourci natif Alt+R (comportement 124, `g_Homun_Present != 0`). Un Alchimiste
    // sans homoncule invoqué n'a rien à y voir, et le natif refuse d'ouvrir sa fenêtre.
    if (homun_tab_visible &&
        ImGui::BeginTabItem(i18n::Tr("Homoncule"), nullptr, flag(kTabHomun))) {
      tab_ = kTabHomun;
      ImGui::EndTabItem();
    }
    ro::RoEndTabBar();
  }
  tab_request_ = -1;
  // L'homoncule a disparu (repos, suppression, changement de perso) alors qu'on
  // regardait son onglet : retour au mannequin plutôt qu'une page vide sans onglet.
  if (tab_ == kTabHomun && !homun_tab_visible) tab_ = 0;
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
  } else if (tab_ == kTabHomun) {
    // Onglet Homoncule : pleine largeur (fiche d'état + liste de compétences).
    ImGui::BeginChild("cs_homun", ImVec2(0, 0), true);
    DrawHomunTab();
    ImGui::EndChild();
  } else {
    // Volet stats seulement si la largeur suffit (sinon cache -> pas de scrollbar vide).
    const bool show_stats =
        avail.x >= doll_pane_w + ImGui::GetStyle().ItemSpacing.x + stats_pane_w - 6.0f;
    // Mémorisé pour la bascule du raccourci Status : c'est la présence de ce volet
    // qui distingue « vue Status » de « vue Équipement », les deux partageant
    // l'onglet du mannequin.
    stats_panel_shown_ = show_stats;
    // Volet staff : un cran de plus, à la même règle et dans le même ordre —
    // mannequin, stats, staff. Il ne peut apparaître QUE derrière le volet stats,
    // dont il aligne les rangées : sans lui il n'aurait rien à viser.
    const bool show_staff =
        staff_pane_allowed && show_stats &&
        avail.x >= doll_pane_w + gap + stats_pane_w + gap + staff_pane_w - 6.0f;
    const float doll_w = show_stats ? doll_pane_w : avail.x;

    ImGui::BeginChild("cs_doll", ImVec2(doll_w, 0), true);
    DrawDoll(ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();

    if (show_stats) {
      ImGui::SameLine();
      ImGui::BeginChild("cs_stats", ImVec2(stats_pane_w, 0), true);
      DrawStatsPanel();
      ImGui::EndChild();
    }
    if (show_staff) {
      // 🔴 APRÈS le volet stats, jamais avant : c'est DrawStatsPanel qui relève, à
      // cette frame, les ordonnées sur lesquelles DrawStaffPanel s'aligne.
      ImGui::SameLine();
      ImGui::BeginChild("cs_staff", ImVec2(staff_pane_w, 0), true);
      DrawStaffPanel();
      ImGui::EndChild();
    }
  }

  ro::EndRoWindow();

  // Aperçu de description de la case survolée : APRÈS la fenêtre (il crée son propre
  // popup, il doit passer au-dessus d'elle) et hors de tout Begin/End — même place que
  // dans l'inventaire, le chariot et le storage.
  DrawHoverDesc();

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
