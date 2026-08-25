#include "features/overlays/target_frame.h"
#include "ragnarok/actor.h"  // rag::actor : les offsets de CActorSprite

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cfloat>
#include <cstring>

#include "imgui.h"

#include "bourgeon.h"
#include "features/fx/palette_inject.h"     // ActorBodySpritePath (3e/4e classes)
#include "features/moonlight_ui/moonlight_ui.h"  // grille d'alignement partagée
#include "features/systems/bourgeon_opcodes.h"
#include "features/windows/entity_context_menu.h"  // clic droit sur un cadre
#include "ragnarok/game_scene.h"
#include "ragnarok/game_settings.h"  // gamesettings::IsOn (le drapeau /nc)
#include "ragnarok/globals.h"
#include "ui/doll.h"        // portrait d'un JOUEUR (pantin composé)
#include "ui/hud_frame.h"   // le cadre libre, commun aux HUD
#include "ui/mob_sprite.h"  // portrait d'un MONSTRE
#include "ui/ro_imgui.h"    // WireToUtf8
#include "ui/ro_widgets.h"
#include "utils/i18n.h"
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "ragnarok/job_ids.h"  // rag::IsPlayerJob / IsMonsterJob
#include "ragnarok/stl_node.h"  // rag::listnode : le nœud du conteneur

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Adresses et offsets natifs (client 20250716, no-ASLR) ───────────────────
// Documenté dans docs/target_system_re.md et docs/entity_nameplate_re.md.
namespace {

// `Actor_FindByGid(gid)` __stdcall : raccourci global qui résout lui-même le mode
// actif (GameMode_GetActive puis ActorList_FindByGID). Il rend nullptr proprement
// hors jeu, là où lire le pointeur de mode à la main tomberait sur celui du login.

// `GameMode_PostActorClickAction(this, aid, type)` __thiscall : TOUT ce que le
// clic gauche sur une entité produit. C'est elle qui traduit « on a cliqué ce
// GID » en action, selon le ciblage armé (`CGameMode+0x408`) — armement d'une
// approche/attaque quand rien n'est armé, lancement de la compétence sur la
// cible en modes 2 et 4. Rien à imiter : on la rappelle avec notre GID.
// ── Le `type` du clic : UNE FOIS, ou EN CONTINU ─────────────────────────────
// 🔴 Ce paramètre n'est pas décoratif : il finit dans l'action en attente de
// l'acteur (`+0x500`), que `Actor_ProcessPendingAction_Tick` relance ensuite
// toutes les 450 ms au lieu de 1200. C'est LUI qui fait la différence entre
// « je frappe une fois » et « je frappe tant que la cible tient debout ».
//
// ⏱ Il était figé à 1 (une fois) : le clic sur un cadre ignorait donc `/nc` et
// Ctrl, alors que le clic sur le sprite les respecte. La règle du natif, relevée
// au site de clic sur un monstre dans `CursorMgr_UpdateHover` :
//
//     continu = !GameSession_IsAgitZone() && (Ctrl tenu || /nc actif)
//     PostActorClickAction(this, gid, continu ? 0 : 1)
//
// ⚠ Et les valeurs sont INVERSÉES par rapport à l'intuition : **0 = continu**,
// **1 = une seule fois**.
// Actions du message `0x89`, telles que le SERVEUR les lit :
// `unit_attack(&sd, id, action_type != 0)` — 0 = UN coup, 7 = sans fin.
constexpr int kActionAttackOnce    = 0;
constexpr int kClickTypeContinuous = 0;
constexpr int kClickTypeOnce       = 1;

// `/nc` (no-control) dans la table d'options du client : identifiant TALKTYPE
// que le natif interroge par `GameSettings_GetFlag(0x6D)` à chaque clic.
// (`gamesettings::IsOn` rend la valeur AFFICHÉE ; `0x6D` ne fait pas partie des
// cinq options rangées à l'envers, les deux coïncident donc.)
constexpr int kOptNoCtrl = 0x6d;

// Carte de SIÈGE : `*(CGameMode+0xCC) + 0x4C`, posé par ZC_NOTIFY_MAPPROPERTY
// de valeur 3 (MAPPROPERTY_AGITZONE). Le natif y refuse l'attaque continue,
// Ctrl ou `/nc` ou non. Lu plutôt qu'appelé (`GameSession_IsAgitZone` 0x00D8EC00
// ne fait que cette déréférence) : une lecture ne peut pas se tromper d'ABI.
constexpr int kScene_AgitZone = 0x4c;

// `CMode::SendMsg`, par la vtable du mode (index 6 = vtable+0x18). Le message
// 0x47 quitte le mode ciblage : le pipeline souris natif l'émet après un
// lancement, et notre clic de cadre doit faire de même — sinon le curseur de
// visée reste armé et le clic suivant relance la compétence.
constexpr int kSendMsgLeaveTarget = 0x47;

// `CNameDict_GetEntryOrRequest(dict, gid)` __thiscall : rend l'entrée si le nom
// est connu, sinon met le GID en file de demande au serveur et rend une entrée
// vide statique. C'est aussi ce qui fait ARRIVER les noms encore inconnus.

// CGameMode
// Mode de ciblage d'une compétence armée : 0 = aucune.
constexpr int kGm_SkillTargetMode = 0x408;
// 🔴 Engagement SOURIS : levé par le message 26 du mode (`0x00C884B9`), émis au
// CLIC sur un acteur, et retombé au premier mouvement hors de la cible bouton
// relâché (`0x00C757C4`). Son FRONT MONTANT est notre signal « le joueur vient
// de désigner quelqu'un » — le seul disponible quand il re-clique la MÊME
// entité, cas où `+0xF4` ne bouge pas d'un octet.
constexpr int kGm_Engaged = 0x28;

constexpr int kGm_Selection = 0x0f4;  // AID de la DERNIÈRE ENTITÉ CLIQUÉE

// CNameInfo : cinq `std::string` de 0x18 octets à la suite (taille +0x10,
// capacité +0x14 DU CHAMP). Pour un MONSTRE, le serveur détourne trois d'entre
// elles — party = « Lv. X | HP: Y% », guilde = race, rang = élément.

// Balayage des acteurs, pour le cyclage au clavier.

// Le natif refuse le marqueur sur un PORTAIL, et lui seul.
// Catégorie du quad de picking d'un ACTEUR (1 = objet au sol, 2 = unité de
// compétence, 3 = pet). Notre cible en est toujours un : le HUD ne suit que des
// entités du monde.
constexpr int kPickCategoryActor = 0;

// Type de curseur « attaque » (l'épée) dans `cursors.act`. C'est celui que le
// natif pose lui-même sur une entité attaquable : `CursorMgr_SetType(this, 5)`
// en `0x00C7571E`, relevé pendant la RE de l'engagement souris. On le REDEMANDE
// au survol d'un cadre actif — un cadre qui se comporte comme le monstre doit
// aussi en avoir l'air, sans quoi rien ne distingue à l'œil un cadre qui frappe
// d'un cadre qui affiche.
constexpr int kRoCursorAttack = 5;
// Options d'état de l'entité, lues par la virtuelle vtable+0x34 de l'acteur.
// `Option_IsCloak` (0x00D71140) n'est qu'un `& 4` sur ce mot.
constexpr int      kActorVt_GetOptions = 0x34;
// Masques d'options, identiques côté client et côté moonlight.
constexpr unsigned kOptionHide      = 0x02;  // Hiding
constexpr unsigned kOptionCloak     = 0x04;  // Cloaking
constexpr unsigned kOptionInvisible = 0x40;  // @hide du staff
// 🔴 Une cible qui porte l'un de ces bits n'existe plus pour nous. Le natif
// applique déjà les deux premiers au nom flottant (`Option_IsHide` `& 2`,
// `Option_IsCloak` `& 4`).
constexpr unsigned kOptionHiddenMask =
    kOptionHide | kOptionCloak | kOptionInvisible;

// Combien de cibles au plus dans un cycle. Un écran n'en montre jamais autant ;
// la borne existe pour qu'un balayage ne puisse pas dégénérer si la liste du
// client est corrompue.
constexpr int kMaxCycleTargets = 128;

// Types d'entité tels qu'ils voyagent dans ZC 0x0F2A (e_bourgeon_target_type).
constexpr uint8_t kTypePc  = 1;
constexpr uint8_t kTypeMob = 2;
constexpr uint8_t kTypeNpc = 3;

// Bits de `known` (e_bourgeon_target_known).
constexpr uint8_t kKnownHp    = 1;
constexpr uint8_t kKnownSp    = 2;
constexpr uint8_t kKnownLevel = 4;
constexpr uint8_t kKnownKind  = 8;

// Cadence des requêtes serveur. 400 ms : une barre de PV qui se met à jour deux
// fois et demie par seconde se lit comme continue, et ça reste deux ordres de
// grandeur sous le trafic d'un combat ordinaire. La requête ne part QUE quand le
// HUD est réellement affiché.
constexpr unsigned kPollMs = 400;

// CZ_REQNAME : « qui est ce GID ? ». 6 octets, [op:2][gid:4] — c'est exactement
// ce que le client construit lui-même en `0x005A1A4E` et envoie en `0x005A1A5F`.
constexpr uint16_t kCzReqName = 0x0368;
// Notre propre cadence de relance. Plus lente qu'une frame, plus rapide que la
// fenêtre d'interdiction du client (10 s) : c'est tout l'intérêt.
constexpr unsigned kNameRetryMs = 700;

// ── Termes de jeu : en anglais, comme partout dans l'interface ──────────────
// (Les libellés d'interface, eux, passent par i18n::Tr.)
const char* const kRaceNames[] = {
    "Formless", "Undead", "Brute",  "Plant",  "Insect", "Fish", "Demon",
    "Demi-Human", "Angel", "Dragon", "Player", "Doram",  "All"};
const char* const kEleNames[] = {"Neutral", "Water", "Earth",  "Fire",  "Wind",
                                 "Poison",  "Holy",  "Shadow", "Ghost", "Undead"};
const char* const kSizeNames[] = {"Small", "Medium", "Large"};

// Disposition d'origine des cinq cadres — la même que l'initialisation du
// header, mais ATTEIGNABLE : « Réinitialiser » ne peut pas construire un
// TargetFrame pour la relire (son constructeur enregistre un opcode, et un
// plugin fantôme s'inscrirait au passage).
struct DefaultRect { int x, y, w, h; };
constexpr DefaultRect kDefaultRects[TargetFrame::kElemCount] = {
    {20, 20, 72, 72},    // portrait
    {96, 20, 190, 34},   // nom
    {96, 56, 190, 16},   // PV
    {96, 74, 190, 16},   // SP
    {20, 96, 266, 18},   // race / élément
};

const char* NameFromTable(const char* const* table, size_t count, unsigned idx) {
  return (idx < count) ? table[idx] : nullptr;
}

// Ajoute un morceau à une énumération « a · b · c », en posant le séparateur
// seulement s'il y a déjà quelque chose. Un morceau vide ne laisse aucune trace
// — pas de « · » orphelin quand le serveur ignore un champ.
void AppendPart(char* dst, size_t dst_size, const char* part) {
  if (!part || !*part) return;
  if (dst[0] != '\0') strncat_s(dst, dst_size, " · ", _TRUNCATE);
  strncat_s(dst, dst_size, part, _TRUNCATE);
}

// Un champ à un offset : la lecture est celle de tout le monde (globals.h),
// et le `using` garde les points d'appel de ce fichier tels quels.
using rag::Read;

// ⚠ SEH ⇒ AUCUN objet C++ dans les fonctions ci-dessous (C2712).

// Les options d'état de l'entité (virtuelle `vtable+0x34` de l'acteur).
unsigned ActorOptions(void* actor) {
  __try {
    if (!actor) return 0u;
    using GetOptionsFn = unsigned(__fastcall*)(void* ecx, void* edx);
    const void* vtable = *reinterpret_cast<const void* const*>(actor);
    if (!vtable) return 0u;
    const GetOptionsFn get_options = *reinterpret_cast<const GetOptionsFn*>(
        reinterpret_cast<const uint8_t*>(vtable) + kActorVt_GetOptions);
    if (!get_options) return 0u;
    return get_options(actor, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
}

// (Le seuil de double-clic vit désormais dans le WndProc, avec la détection :
// c'est la règle de Windows elle-même, délai ET tolérance de déplacement.)

// L'engagement souris du mode (`+0x28`), lu sous SEH comme tout le reste.
bool ReadEngaged(void* game_mode) {
  __try {
    return game_mode && Read<int32_t>(game_mode, kGm_Engaged) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Position MONDE d'un acteur, sous la forme exacte que le marqueur de sélection
// attend : deux flottants tronqués en entiers. Reproduit aussi les filtres du
// natif, pour ne pas poser une flèche là où le jeu, lui, n'en poserait pas.
bool ReadMarkerPos(void* actor, int* out_x, int* out_z) {
  __try {
    if (!actor) return false;
    if (Read<uint32_t>(actor, rag::actor::kJobId) == rag::kJobPortal) return false;
    // 🔴 Une entité CLOAKÉE n'a droit à rien, et ce n'est pas un détail
    // d'affichage : poser la flèche sur elle donnerait la position d'un joueur
    // caché — un avantage que le client vanilla ne donne pas, donc une triche.
    // On est ici STRICTEMENT plus prudent que le natif, qui ne teste le cloak
    // que sur une partie des acteurs.
    if (ActorOptions(actor) & kOptionHiddenMask) return false;
    // ⚠ Et RIEN d'autre. Une première version filtrait aussi sur `+0x314 == 0`
    // (« unité ordinaire ») et sur les octets de visibilité `+0xA0`/`+0xA1` :
    // c'était bâti sur une lecture fausse du natif. `CActorSprite_InitDefaults`
    // (`0x00C45F47`) pose `+0x314 = 4` PAR DÉFAUT — « non nul » n'y désigne donc
    // pas une unité spéciale, et le test ne filtrait rien.
    *out_x = static_cast<int>(Read<float>(actor, rag::actor::kPosX));
    *out_z = static_cast<int>(Read<float>(actor, rag::actor::kPosZ));
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Ce clic doit-il être neutralisé ? Voir `TargetFrame::SuppressClickEngage`
// pour le raisonnement ; ici, seulement les lectures natives.
bool ClickEngagesAnAttack(void* game_mode, void* actor) {
  __try {
    if (!game_mode || !actor) return false;
    if (Read<int32_t>(game_mode, kGm_SkillTargetMode) != 0) return false;
    const uint32_t own = rag::OwnAccountIdSafe();
    if (own != 0 && Read<uint32_t>(actor, rag::actor::kOwnerAid) == own) return false;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uint32_t ReadSelection(void* game_mode) {
  __try {
    return game_mode ? Read<uint32_t>(game_mode, kGm_Selection) : 0u;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
}

// Le mode de ciblage armé (`+0x408`) : 0 aucun · 1 sol · 2 cible offensive ·
// 4 soutien · 3 et 5 gardent le ciblage natif.
int ReadSkillTargetMode(void* game_mode) {
  __try {
    return game_mode ? Read<int32_t>(game_mode, kGm_SkillTargetMode) : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Rejoue le clic natif sur une entité, avec NOTRE GID. Cf. gamescene::kPostActorClickActionAddr :
// c'est la fonction qui porte tout le sens du clic, y compris le lancement d'une
// compétence armée. Rend true si l'appel a eu lieu.
bool RunActorClick(void* game_mode, uint32_t gid, int click_type) {
  __try {
    if (!game_mode || gid == 0) return false;
    using ClickFn = int(__thiscall*)(void*, uint32_t, int);
    reinterpret_cast<ClickFn>(gamescene::kPostActorClickActionAddr)(game_mode, gid, click_type);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Sommes-nous sur une carte de siège ? Cf. kScene_AgitZone.
bool OnAgitZone(void* game_mode) {
  __try {
    if (!game_mode) return false;
    void* scene = Read<void*>(game_mode, gamescene::kGmActorMgr);
    return scene && Read<int32_t>(scene, kScene_AgitZone) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Quitte le mode ciblage, comme le fait le pipeline souris natif après un
// lancement. Sans lui, le curseur de visée resterait armé.
void LeaveSkillTargeting(void* game_mode) {
  __try {
    rag::ModeSendMsg(game_mode, kSendMsgLeaveTarget);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

int ReadActorInt(void* actor, int off) {
  __try {
    return actor ? Read<int32_t>(actor, off) : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Ce GID est-il une cible LÉGALE pour une compétence armée dans ce mode ?
//
// 🔴 Les règles sont ici parce qu'elles ne sont nulle part ailleurs : le chemin
// natif qui les portait (`CursorMgr_UpdateHover`) n'est pas emprunté quand la
// visée ne vient pas de la souris. Ce sont exactement celles de
// `QuickCast::PickTargetGid`, plus le refus du cadavre — la sélection du HUD,
// elle, peut en garder un (un sort de résurrection le désigne).
uint32_t ValidSkillTarget(uint32_t gid, int mode) {
  __try {
    void* actor = gamescene::FindActorByGid(gid);
    if (!actor) return 0u;
    if (ActorOptions(actor) & kOptionHiddenMask) return 0u;
    const uint32_t own = rag::OwnAccountIdSafe();
    if (own != 0 && gid == own) return 0u;
    if (mode == 2) {  // offensif
      if (!rag::IsMonsterJob(static_cast<unsigned>(Read<uint32_t>(actor, rag::actor::kJobId))))
        return 0u;    // un joueur reste au clic manuel (PVP/GVG)
      if (Read<int32_t>(actor, rag::actor::kMotionState) == 3) return 0u;  // cadavre
    }
    return gid;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
}

// Recopie une `std::string` du client, SSO comprise. Rend false si le champ est
// vide ou aberrant — le cas normal pour la guilde d'un monstre quand le serveur
// n'y met rien.
// Un champ du CNameInfo de ce GID. `field` est l'un des kName_*.
// L'entrée du dictionnaire de noms pour ce GID. 🔴 `CNameDict_GetEntryOrRequest`
// n'est PAS un simple accesseur : sur un défaut, il empile le GID dans la file
// des noms à demander et rend une entrée VIDE et statique. C'est ainsi que le
// nom finit par arriver — le tick du dictionnaire (`0x005A1920`, appelé chaque
// frame depuis `GameMode_InGame_ProcessFrame`) dépile UNE demande par frame,
// l'envoie (CZ_REQNAME `0x0368`, 6 octets) et note le GID pendant 10 s pour ne
// pas le redemander.
//
// ⚠ D'où UN SEUL appel par frame, et non un par champ : on l'appelait quatre
// fois (nom, party, guilde, rang), donc on empilait quatre demandes par frame
// pour le même GID — jusqu'à 240 par seconde dans une file que le client ne vide
// qu'à raison d'une par frame. La cible mettait d'autant plus longtemps à
// recevoir son nom qu'on le réclamait plus fort.

// Un champ de cette entrée, recopié. L'entrée statique rendue sur un défaut a
// tous ses champs vides, et le tampon de sortie est vidé dans tous les cas :
// garder l'ancien nom serait un mensonge muet le temps qu'arrive le vrai.
bool ReadNameField(void* entry, int field, char* out, size_t out_size) {
  if (out_size) out[0] = '\0';
  if (!entry) return false;
  return rag::clientstr::Copy(reinterpret_cast<const uint8_t*>(entry) + field, out,
                          out_size);
}

// PV lus dans l'une des deux jauges de l'acteur. False si aucune ne porte de
// maximum utilisable.
bool ReadActorGauge(void* actor, uint32_t* hp, uint32_t* maxhp) {
  __try {
    if (!actor) return false;
    const int slots[2] = {rag::actor::kMonsterGage, rag::actor::kHeadGage};
    for (int i = 0; i < 2; ++i) {
      void* gage = Read<void*>(actor, slots[i]);
      if (!gage) continue;
      const uint32_t cur = Read<uint32_t>(gage, rag::actor::kGageHp);
      const uint32_t max = Read<uint32_t>(gage, rag::actor::kGageHpMax);
      if (max == 0) continue;
      *hp = cur;
      *maxhp = max;
      return true;
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ── La plaque de nom d'un monstre, telle que le serveur la fabrique ─────────
//
// `clif_name` écrit « Lv. 42 | HP: 63% » (ou « HP: 1234/2000 ») dans le champ
// PARTY quand show_mob_info est actif. On en tire un repli pour le niveau et les
// PV — utile avant que la première réponse serveur n'arrive, et sur un serveur
// qui n'aurait pas ZC 0x0F2A.
//
// Parseur DÉLIBÉRÉMENT tolérant : chaque morceau est cherché indépendamment, et
// ce qui manque reste absent. Le format dépend d'une option serveur et de la
// langue — il ne mérite pas qu'on lui fasse confiance, seulement qu'on en
// profite quand il est là.
struct MobPlate {
  bool     has_level = false;
  int      level     = 0;
  bool     has_pct   = false;
  int      hp_pct    = 0;
  bool     has_abs   = false;
  uint32_t hp        = 0;
  uint32_t maxhp     = 0;
};

MobPlate ParseMobPlate(const char* plate) {
  MobPlate out;
  if (!plate || !*plate) return out;

  const char* lv = strstr(plate, "Lv. ");
  if (lv) {
    int value = 0;
    if (sscanf_s(lv + 4, "%d", &value) == 1 && value > 0) {
      out.has_level = true;
      out.level = value;
    }
  }
  const char* hp = strstr(plate, "HP: ");
  if (hp) {
    unsigned cur = 0, max = 0;
    int pct = 0;
    if (sscanf_s(hp + 4, "%u/%u", &cur, &max) == 2 && max > 0) {
      out.has_abs = true;
      out.hp = cur;
      out.maxhp = max;
    } else if (sscanf_s(hp + 4, "%d%%", &pct) == 1 && pct >= 0) {
      out.has_pct = true;
      out.hp_pct = pct;
    }
  }
  return out;
}

// Texte posé dans un cadre, à l'échelle demandée, sans déborder.
void FrameText(const char* text, const float rgba[4], float wrap_width = 0.0f) {
  if (!text || !*text) return;
  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]));
  if (wrap_width > 0.0f) {
    ImGui::PushTextWrapPos(wrap_width);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
  } else {
    ImGui::TextUnformatted(text);
  }
  ImGui::PopStyleColor();
}

}  // namespace

// ── Cycle de vie ────────────────────────────────────────────────────────────

TargetFrame::TargetFrame() {
  // Notre réponse custom. Au-dessus de l'opcode max du client (0x0C35), donc
  // livrée par le reader-hook et non par la table de dispatch.
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kTargetInfo);
}

void TargetFrame::OnModeSwitch(ModeMgr::ModeType mode_type,
                               const char* map_name) {
  // Changer de map purge la sélection NATIVE (`GameMode_OnEnterMapSetup` en est
  // l'unique nettoyage) : la nôtre part avec, sinon le HUD se rallumerait sur un
  // GID de l'ancienne carte le temps d'une frame.
  Reset(0);
  pending_gesture_gid_ = 0;
  last_native_sel_ = 0;
  was_engaged_ = false;
  explicit_attack_gid_ = 0;
  explicit_engage_pending_ = false;
  explicit_attack_once_ = false;
}

void TargetFrame::Reset(uint32_t gid) {
  gid_ = gid;
  hidden_ = false;
  last_poll_ = 0;
  name_retry_ms_ = 0;
  name_[0] = party_[0] = guild_[0] = rank_[0] = '\0';
  is_mob_ = is_player_ = false;
  srv_valid_ = false;
  srv_known_ = 0;
  srv_type_ = 0;
  srv_level_ = 0;
  srv_hp_ = srv_maxhp_ = srv_sp_ = srv_maxsp_ = 0;
  srv_race_ = srv_ele_ = srv_ele_lv_ = srv_size_ = srv_boss_ = 0;
}

// ── Cyclage au clavier ──────────────────────────────────────────

namespace {

// Un candidat au cyclage : son GID et sa distance au joueur (au carré, la racine
// ne servirait qu'à ralentir un tri).
struct CycleCandidate {
  uint32_t gid;
  float    dist2;
};

// Balaie la liste d'acteurs du client et retient les monstres CIBLABLES : vivants,
// participant au nameplate, et À L'ÉCRAN. Rend leur nombre (0 si rien).
//
// 🔴 Le filtre est le JOB, pas une plage de GID. Les identifiants sont attribués
// par le serveur et changent de bornes d'une installation à l'autre ; la classe,
// elle, dit ce que l'entité EST.
//
// ⚠ SEH ⇒ aucun objet C++ ici. Le balayage ne fait que LIRE : la liste du client
// n'est jamais modifiée.
int CollectScreenTargets(void* gm, CycleCandidate* out, int max) {
  int count = 0;
  __try {
    void* actor_mgr = Read<void*>(gm, gamescene::kGmActorMgr);
    if (!actor_mgr) return 0;
    void* own = Read<void*>(actor_mgr, gamescene::kAmOwnPlayer);
    if (!own) return 0;
    const uint32_t own_gid = Read<uint32_t>(own, rag::actor::kGid);
    const float own_x = Read<float>(own, rag::actor::kPosX);
    const float own_z = Read<float>(own, rag::actor::kPosZ);

    const ImGuiIO& io = ImGui::GetIO();
    const float screen_w = io.DisplaySize.x;
    const float screen_h = io.DisplaySize.y;
    void* sentinel = Read<void*>(actor_mgr, gamescene::kAmListHead);
    if (!sentinel) return 0;
    void* node = Read<void*>(sentinel, 0);  // premier nœud
    int guard = 0;
    while (node && node != sentinel && count < max && ++guard < 4096) {
      void* actor = Read<void*>(node, rag::listnode::kValue);
      node = Read<void*>(node, 0);
      if (!actor) continue;

      const unsigned job = Read<uint32_t>(actor, rag::actor::kJobId);
      if (!rag::IsMonsterJob(job)) continue;
      // Vivant, et visible : un cadavre ou un monstre masqué ne se cible pas au
      // clavier (le sort sur cadavre a son propre chemin, cf. NoteSkillTarget).
      if (Read<int32_t>(actor, rag::actor::kMotionState) == 3) continue;
      if (Read<uint8_t>(actor, rag::actor::kNameplate) == 0) continue;

      // À L'ÉCRAN, et rien d'autre. La position écran projetée de l'acteur est
      // déjà calculée par le client à chaque frame : elle coûte deux lectures et
      // dit exactement ce que le joueur voit.
      const int sx = Read<int32_t>(actor, rag::actor::kScreenX);
      const int sy = Read<int32_t>(actor, rag::actor::kScreenY);
      if (sx <= 0 || sy <= 0 || sx >= static_cast<int>(screen_w) ||
          sy >= static_cast<int>(screen_h))
        continue;

      const float dx = Read<float>(actor, rag::actor::kPosX) - own_x;
      const float dz = Read<float>(actor, rag::actor::kPosZ) - own_z;

      out[count].gid = Read<uint32_t>(actor, rag::actor::kGid);
      out[count].dist2 = dx * dx + dz * dz;
      if (out[count].gid != 0 && out[count].gid != own_gid) ++count;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
  return count;
}

// Tri par distance croissante. Le cycle part TOUJOURS du plus proche, et l'ordre
// ne dépend pas de celui, arbitraire, de la liste du client — sans quoi deux
// passages successifs ne donneraient pas la même suite.
void SortByDistance(CycleCandidate* a, int count) {
  for (int i = 1; i < count; ++i) {
    const CycleCandidate key = a[i];
    int j = i - 1;
    while (j >= 0 && a[j].dist2 > key.dist2) {
      a[j + 1] = a[j];
      --j;
    }
    a[j + 1] = key;
  }
}

// 🔴 On pose la sélection NATIVE. C'est elle que lit
// `GameMode_UpdateSelectedTargetNameLabel` à chaque frame pour afficher le nom
// flottant et la flèche ; l'écrire donne donc à la cible clavier exactement le
// même retour visuel qu'un clic.
//
// 🔴🔴 MAIS ON N'ÉCRIT PAS `+0xF8`, ET CE N'EST PAS UN OUBLI. Le chemin natif du
// clic le remet à zéro juste après `+0xF4` (0x00C79D3C) ; l'avoir imité a coûté un
// bug que rien ne reliait au ciblage — **prendre une cible au clavier COUPAIT LA
// MARCHE au clic maintenu**, et il fallait relâcher puis recliquer pour repartir
// (remonté en jeu le 2026-08-22).
//
// Ce champ n'est pas le « drapeau remis à 0 au clic » que cette page et la doc
// croyaient : il dit **« le maintien de souris en cours n'a PAS été capturé par un
// acteur »**. Relevé exhaustif de ses accès sur 0x00C60000-0x00CA0000 :
//   · mis à **1** au RELÂCHEMENT du bouton gauche (`GameMode_ProcessMouseWorldInput`
//     0x00C76538, `g_Mouse_LButtonState == 3`) et à l'entrée de carte (0x00C6BE2A) ;
//   · mis à **0** par les sept sites de clic ou de survol SUR ACTEUR ;
//   · **lu** par le chemin « bouton maintenu » de `GameMode_GroundClick_RequestMove`
//     (0x00C76236) : la demande de marche n'est ré-émise que si `+0xF8 != 0`. Le
//     chemin « appui FRAIS » ne le consulte jamais — d'où la réparation au reclic,
//     qui est ce qui rendait le symptôme si difficile à rattacher à sa cause.
//
// Un cyclage clavier ne capture aucun geste de souris. Il n'a donc rien à écrire
// là : le seul champ qui porte la CIBLE est `+0xF4`.
bool WriteNativeSelection(void* gm, uint32_t gid) {
  __try {
    *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(gm) + kGm_Selection) = gid;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

}  // namespace

// Pose `gid` comme cible clavier : sélection native + geste de ciblage.
//
// Un cyclage EST un geste de ciblage, au même titre qu'un clic : il désigne, et
// c'est cette désignation — pas la sélection native — qui allume le HUD.
bool TargetFrame::ApplyKeyboardTarget(void* gm, uint32_t gid) {
  if (gid == 0) return false;
  if (!WriteNativeSelection(gm, gid)) return false;
  pending_gesture_gid_ = gid;
  last_cycle_ms_ = GetTickCount();
  return true;
}

bool TargetFrame::TargetNearest() {
  // Le mode éteint, le raccourci ne fait rien : c'est le prix d'un interrupteur
  // maître honnête, et le panneau le dit noir sur blanc.
  if (!enabled_) return false;
  void* gm = rag::ActiveModeSafe();
  if (!gm) return false;

  CycleCandidate found[kMaxCycleTargets];
  const int count = CollectScreenTargets(gm, found, kMaxCycleTargets);
  if (count == 0) return false;

  // Un seul candidat à trouver : le minimum suffit, un tri complet serait du
  // travail jeté.
  int best = 0;
  for (int i = 1; i < count; ++i)
    if (found[i].dist2 < found[best].dist2) best = i;
  return ApplyKeyboardTarget(gm, found[best].gid);
}

bool TargetFrame::CycleTarget(bool forward) {
  // Le mode éteint, le raccourci ne fait rien : c'est le prix d'un interrupteur
  // maître honnête, et le panneau le dit noir sur blanc.
  if (!enabled_) return false;
  void* gm = rag::ActiveModeSafe();
  if (!gm) return false;

  CycleCandidate found[kMaxCycleTargets];
  const int count = CollectScreenTargets(gm, found, kMaxCycleTargets);
  if (count == 0) return false;
  SortByDistance(found, count);

  // ── Une pause REMET LE CYCLE À ZÉRO ────────────────────────────────
  // Le cyclage sert à DEUX choses qui ne demandent pas la même règle : parcourir
  // ce qu'il y a autour (appuis rapprochés, il faut avancer), et prendre une
  // cible (appui isolé, on veut la plus proche). Sans remise à zéro, le second
  // usage héritait de la position laissée par le premier : on rappuyait après
  // un combat et on repartait au cinquième monstre.
  //
  // Le délai fait la différence entre les deux, et lui seul. À 0, jamais de
  // remise à zéro : le cycle se souvient indéfiniment, comme avant.
  const unsigned now = GetTickCount();
  const bool restart = cycle_reset_ms_ > 0 &&
                       (last_cycle_ms_ == 0 ||
                        (now - last_cycle_ms_) >
                            static_cast<unsigned>(cycle_reset_ms_));

  // Où en est-on ? La cible courante est celle du HUD ; si elle n'est plus dans
  // la liste (morte, hors écran, jamais ciblée), on repart du plus proche.
  int current = -1;
  if (!restart) {
    const uint32_t shown = gid_;
    for (int i = 0; i < count; ++i) {
      if (found[i].gid == shown) { current = i; break; }
    }
  }

  int next;
  if (restart) {
    // ⚠ Le plus proche, quel que soit le SENS demandé. « Précédente » après une
    // pause veut dire « reprends la main », pas « donne-moi le plus lointain ».
    next = 0;
  } else if (current < 0) {
    next = forward ? 0 : count - 1;
  } else {
    next = current + (forward ? 1 : -1);
    if (next >= count) {
      if (!cycle_wrap_) return false;
      next = 0;
    } else if (next < 0) {
      if (!cycle_wrap_) return false;
      next = count - 1;
    }
  }

  return ApplyKeyboardTarget(gm, found[next].gid);
}

void TargetFrame::NoteExplicitAttack(uint32_t gid, bool once) {
  explicit_attack_gid_     = gid;
  explicit_engage_pending_ = (gid != 0);
  explicit_attack_once_    = once;
}

bool TargetFrame::SuppressClickEngage(void* game_mode, uint32_t target_aid) {
  if (!enabled_) return false;
  // L'ordre explicite (menu contextuel, cadre du HUD, double-clic rejoué) passe,
  // et il ne consomme qu'un seul passage : le clic ordinaire qui suivra, fût-il
  // sur la même entité, retombe sous la règle commune.
  if (explicit_engage_pending_) {
    explicit_engage_pending_ = false;
    return false;
  }

  if (!click_no_attack_ || target_aid == 0) return false;
  // Les offsets sont ceux de CE build : ailleurs, on ne s'interpose pas.
  if (Bourgeon::Instance().client().timestamp() != 20250716) return false;
  if (!ClickEngagesAnAttack(game_mode, gamescene::FindActorByGid(target_aid))) return false;

  // ── 🔴 Le double-clic ne se détecte PAS ici, et ne le peut pas ─────────
  // ⏱ Mesuré : un double-clic sur une entité ne produit qu'UN SEUL passage dans
  // cette fonction. Le second appui arrive pourtant intact au WndProc
  // (`DOWN, UP, DOWN, UP`), mais la machine à états de souris du client n'en fait
  // pas un appui frais et n'appelle donc pas `PostActorClickAction`. Compter les
  // passages revenait à apparier les PREMIERS appuis de deux double-clics
  // successifs — exactement le « il faut en faire deux » rapporté en jeu, et
  // aucun réglage de seuil n'y pouvait rien.
  //
  // La détection vit donc dans le WndProc, seul endroit où les deux appuis
  // existent (cf. `NoteWorldDoubleClick`).
  //
  // Il ne reste qu'une règle ici : un clic ordinaire CIBLE, et referme la
  // dispense en cours — le joueur a repris la main.
  explicit_attack_gid_  = 0;
  explicit_attack_once_ = false;
  return true;
}

// ── Un coup, ou sans fin ? ───────────────────────────────────────────
// La règle du natif, relevée au site de clic sur un monstre dans
// `CursorMgr_UpdateHover`. Elle vaut pour TOUS nos gestes — cadre du HUD comme
// double-clic rejoué : le geste dit « attaque-le », le réglage dit comment.
bool TargetFrame::WantsContinuousAttack(void* game_mode) {
  if (OnAgitZone(game_mode)) return false;  // zone de siège : jamais de continu
  return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
         gamesettings::IsOn(kOptNoCtrl);
}

// Engage l'attaque sur `gid`, avec la durée que le réglage commande. Le point de
// passage COMMUN du clic sur un cadre et du double-clic rejoué : deux copies de
// ce geste auraient divergé au premier correctif.
void TargetFrame::EngageAttack(void* game_mode, uint32_t gid, bool continuous) {
  // 🔴 `once` referme la dispense sur la demande qu'elle laisse passer, et
  // `FilterBasicAttack` réécrit alors l'action à 0 : sans ça le client envoie 7
  // (DMG_REPEAT) et le serveur frappe sans fin.
  NoteExplicitAttack(gid, /*once=*/!continuous);
  RunActorClick(game_mode, gid,
                continuous ? kClickTypeContinuous : kClickTypeOnce);
}

// Rejoue le double-clic détecté par le WndProc. Le client n'ayant pas fait du
// second appui un appui frais, c'est à nous d'en faire une attaque — par le
// MÊME chemin que le clic sur un cadre du HUD, `/nc` compris.
void TargetFrame::FlushWorldDoubleClick() {
  auto& bourgeon = Bourgeon::Instance();
  // Sans « le clic cible sans attaquer », le premier appui a déjà engagé : il n'y
  // a rien à rattraper, et rejouer ferait un doublon.
  if (!enabled_ || !click_no_attack_) return;
  if (bourgeon.client().timestamp() != 20250716) return;
  if (bourgeon.IsMapLoading() || !bourgeon.IsGameActive()) return;

  void* gm = rag::ActiveModeSafe();
  if (!gm) return;
  // La cible est celle que le PREMIER appui vient d'écrire (`+0xF4`).
  const uint32_t sel = ReadSelection(gm);
  void* actor = sel != 0 ? gamescene::FindActorByGid(sel) : nullptr;
  if (!actor) return;
  // Une compétence armée a son propre chemin, et un compagnon garde ses ordres.
  if (ReadSkillTargetMode(gm) != 0) return;
  if (!ClickEngagesAnAttack(gm, actor)) return;
  // Dispense déjà ouverte pour cette cible : ne pas rejouer un clic de plus.
  // (Cas d'un client qui enverrait AUSSI un `WM_LBUTTONDBLCLK` : les deux voies
  // se déclencheraient, et une seule doit agir.)
  if (explicit_attack_gid_ == sel) return;

  EngageAttack(gm, sel, WantsContinuousAttack(gm));
}

int TargetFrame::FilterBasicAttack(uint32_t target_gid, int action) {
  if (target_gid == 0 || target_gid != explicit_attack_gid_)
    return (enabled_ && click_no_attack_) ? -1 : action;

  const bool single = explicit_attack_once_;
  if (single) {
    explicit_attack_gid_  = 0;
    explicit_attack_once_ = false;
  }
  // 🔴 C'est ICI que « un seul coup » devient vrai, et nulle part ailleurs. Le
  // client a déjà mis 7 (DMG_REPEAT) dans le paquet ; le laisser partir tel quel
  // fait frapper le serveur sans fin, quel que soit le nombre de paquets qu'on
  // autorise — `unit_attack(&sd, id, action_type != 0)`.
  return single ? kActionAttackOnce : action;
}

bool TargetFrame::WantsSelectionMarker(int* world_x, int* world_z) {
  if (!enabled_ || !native_marker_) return false;
  if (gid_ == 0 || hidden_) return false;
  // 🔴 Les MÊMES deux gardes que le rendu des plugins (`Bourgeon::RenderUI`), et
  // pour la même raison : c'est `DrawHud` qui tient `gid_` à jour, et il ne
  // tourne ni pendant un chargement de carte, ni hors du monde. Sans elles, une
  // cible périmée resterait fléchée alors que plus rien ne la revalide.
  auto& bourgeon = Bourgeon::Instance();
  if (bourgeon.IsMapLoading() || !bourgeon.IsGameActive()) return false;
  // Mêmes offsets, même garde qu'au rendu : sur un autre client, on ne touche à
  // rien plutôt que de lire au hasard.
  if (bourgeon.client().timestamp() != 20250716) return false;
  return ReadMarkerPos(gamescene::FindActorByGid(gid_), world_x, world_z);
}

void TargetFrame::NoteSkillTarget(uint32_t gid) {
  if (gid == 0) return;
  // Un sort sur SOI-MÊME ne change pas de cible : sinon le moindre soin
  // personnel effacerait ce que le joueur regardait.
  const uint32_t own = rag::OwnAccountIdSafe();
  if (own != 0 && gid == own) return;
  pending_gesture_gid_ = gid;
}

void TargetFrame::HideForGid(uint32_t gid) {
  if (gid != 0 && gid == gid_) hidden_ = true;
}

bool TargetFrame::IsShownFor(uint32_t gid) const {
  return enabled_ && gid != 0 && gid == gid_ && !hidden_;
}

// ── Réseau ──────────────────────────────────────────────────────────────────

void TargetFrame::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                               uint16_t len) {
  net_inbox_.Push(opcode, data, len);  // fil RÉSEAU : rien d'autre
}

void TargetFrame::HandlePacket(uint16_t opcode, const uint8_t* data,
                               uint16_t len) {
  if (opcode != bopcodes::kTargetInfo) return;
  // `data` commence APRÈS [opcode:2][len:2] : convention de RegisterRecvOpcode.
  // [gid:4][status:1][known:1][type:1][level:2][hp:4][maxhp:4][sp:4][maxsp:4]
  // [race:1][ele:1][ele_lv:1][size:1][boss:1] = 30 octets.
  if (len < 30) return;

  uint32_t gid = 0;
  memcpy(&gid, data, 4);
  // Une réponse qui arrive après un reciblage décrit l'ANCIENNE entité : la
  // montrer sous le nouveau nom serait un mensonge silencieux.
  if (gid == 0 || gid != gid_) return;

  const uint8_t status = data[4];
  if (status != 0) {
    // Le serveur fait autorité sur « hors de portée » : c'est LUI qui connaît
    // AREA_SIZE. Le HUD s'éteint, et ne se rallumera qu'au prochain GESTE — donc
    // pas de liste noire à tenir : rien ne rallume tout seul.
    Reset(0);
    return;
  }

  srv_known_ = data[5];
  srv_type_  = data[6];
  memcpy(&srv_level_, data + 7, 2);
  memcpy(&srv_hp_,    data + 9, 4);
  memcpy(&srv_maxhp_, data + 13, 4);
  memcpy(&srv_sp_,    data + 17, 4);
  memcpy(&srv_maxsp_, data + 21, 4);
  srv_race_   = data[25];
  srv_ele_    = data[26];
  srv_ele_lv_ = data[27];
  srv_size_   = data[28];
  srv_boss_   = data[29];
  srv_valid_  = true;
}

// Redemande le nom de la cible au serveur.
//
// 🔴 Pourquoi le refaire nous-mêmes alors que le client le fait déjà : parce
// qu'il ne le refait PAS. `CNameDict_Tick_FlushNameRequests` (`0x005A1920`) note
// chaque GID demandé et **s'interdit de le redemander pendant 10 000 ms**. Or on
// peut cibler plus vite que la réponse n'arrive — et si l'entité a été recréée
// entre-temps (Cloaking, `@hide`, sortie puis retour dans AREA_SIZE), son entrée
// de dictionnaire est repartie vide alors que l'interdiction, elle, court
// toujours. Le nom reste alors « inconnu » jusqu'à dix secondes, et le nameplate
// du jeu au-dessus du sprite est vide lui aussi : le client n'a rien, et ne
// demande rien.
//
// Le paquet est celui du client, à l'octet près. La réponse (ZC_ACK_REQNAME) est
// traitée par le client comme n'importe quelle autre : c'est SON dictionnaire qui
// se remplit, et le nameplate natif se répare avec le nôtre.
void TargetFrame::RequestTargetName() {
  const unsigned now = GetTickCount();
  if (name_retry_ms_ != 0 && (now - name_retry_ms_) < kNameRetryMs) return;
  name_retry_ms_ = now;

  uint8_t packet[6];
  *reinterpret_cast<uint16_t*>(packet + 0) = kCzReqName;
  *reinterpret_cast<uint32_t*>(packet + 2) = gid_;
  Bourgeon::Instance().SendPacket(packet, sizeof(packet));
}

void TargetFrame::PollServer() {
  const unsigned now = GetTickCount();
  if (last_poll_ != 0 && (now - last_poll_) < kPollMs) return;
  last_poll_ = now;

  uint8_t packet[8];  // [op:2][len:2][gid:4]
  *reinterpret_cast<uint16_t*>(packet + 0) = bopcodes::kReqTargetInfo;
  *reinterpret_cast<uint16_t*>(packet + 2) = static_cast<uint16_t>(sizeof(packet));
  *reinterpret_cast<uint32_t*>(packet + 4) = gid_;
  Bourgeon::Instance().SendPacket(packet, sizeof(packet));
}

// ── Rendu ───────────────────────────────────────────────────────────────────

void TargetFrame::OnRenderUI() {
  if (!enabled_) return;
  // Les offsets ci-dessus sont ceux de CE build. Sur un autre client, on ne
  // dessine rien plutôt que de lire au hasard.
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;
  __try {
    DrawHud();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Une entité à moitié construite ou libérée a fait faute : on saute cette
    // frame. Rien de natif n'a été modifié.
  }
}

void TargetFrame::DrawHud() {
  void* gm = rag::ActiveModeSafe();
  if (!gm) { gid_ = 0; return; }

  // ── Un GESTE désigne la cible, pas un sondage ─────────────────────────────
  // 🔴 Le HUD suivait `+0xF4` frame après frame. Conséquence vue en jeu : une
  // cible qui sortait de l'écran disparaissait — son acteur avait vraiment quitté
  // le monde du client — puis **revenait toute seule** dès qu'elle rentrait dans
  // AREA_SIZE, parce que la sélection native, elle, n'est purgée qu'au changement
  // de map. Un HUD qui se rallume sans qu'on ait rien demandé.
  //
  // On ne suit donc plus un ÉTAT, on écoute des GESTES. Trois seulement, et le
  // dernier gagne :
  //   · un CLIC — soit `+0xF4` change, soit l'engagement souris `+0x28` monte
  //     (c'est le seul signal quand on re-clique la MÊME entité) ;
  //   · un CYCLAGE au clavier (`CycleTarget`) ;
  //   · un SORT qui part (`NoteSkillTarget`) — y compris sur un cadavre, que la
  //     sélection native refuse d'enregistrer.
  // Une fois la cible perdue, plus rien ne la rallume : il faut la re-désigner.
  const uint32_t native_sel = ReadSelection(gm);
  const bool engaged = ReadEngaged(gm);
  if (native_sel != last_native_sel_) {
    last_native_sel_ = native_sel;
    if (native_sel != 0) pending_gesture_gid_ = native_sel;
  } else if (engaged && !was_engaged_ && native_sel != 0) {
    pending_gesture_gid_ = native_sel;
  }
  was_engaged_ = engaged;

  if (pending_gesture_gid_ != 0) {
    if (pending_gesture_gid_ != gid_) Reset(pending_gesture_gid_);
    hidden_ = false;  // re-désigner rallume ce qu'on avait masqué à la main
    pending_gesture_gid_ = 0;
  }

  void* actor = nullptr;
  if (gid_ != 0 && !hidden_) {
    actor = gamescene::FindActorByGid(gid_);
    // 🔴 Trois façons de PERDRE la cible, et toutes les trois lui coûtent son
    // GID : elle quitte le monde du client (mort, ou sortie d'AREA_SIZE via
    // ZC_NOTIFY_VANISH), ou elle se CACHE — Hiding, Cloaking, @hide du staff.
    // Sur ce dernier point il n'y a pas à hésiter : garder le GID d'une entité
    // qui vient de se rendre invisible, c'est offrir un détecteur.
    if (!actor || (ActorOptions(actor) & kOptionHiddenMask)) {
      Reset(0);
      actor = nullptr;
    }
  }

  // Sans cible, les cadres ne s'affichent qu'en mode placement — ou tant qu'un
  // cadre est SAISI : on ne retire pas sous les doigts du joueur ce qu'il est en
  // train de poser.
  if (!actor && !layout_mode_ && !ro::HudFrameDragging()) return;

  if (actor) {
    // Ce que le client sait tout seul. Une seule interrogation du dictionnaire,
    // quatre lectures dedans — cf. NameEntry pour la raison, qui n'est pas
    // qu'une question de coût.
    void* name_entry = gamescene::NameDictEntry(gm, gid_);
    ReadNameField(name_entry, gamescene::kNameStr,   name_,  sizeof(name_));
    ReadNameField(name_entry, gamescene::kNameParty, party_, sizeof(party_));
    ReadNameField(name_entry, gamescene::kNameGuild, guild_, sizeof(guild_));
    ReadNameField(name_entry, gamescene::kNameRank,  rank_,  sizeof(rank_));
    // Toujours rien ? On le redemande, parce que le client ne le fera pas avant
    // dix secondes. Cf. RequestTargetName.
    if (name_[0] == '\0') RequestTargetName();

    const unsigned job = static_cast<unsigned>(ReadActorInt(actor, rag::actor::kJobId));
    is_mob_ = rag::IsMonsterJob(job);
    is_player_ = !is_mob_ && rag::IsPlayerJob(job);
    // Dès que le serveur a parlé, c'est lui qui classe : il connaît le type réel,
    // là où la classe affichée ment sur un monstre déguisé ou un NPC de type PC.
    if (srv_valid_) {
      is_mob_ = (srv_type_ == kTypeMob);
      is_player_ = (srv_type_ == kTypePc);
    }
    PollServer();
  }

  DrawElements(gm, actor);
}

void TargetFrame::DrawElements(void* game_mode, void* actor) {
  const AlignGrid* grid = nullptr;
  if (auto* mui = Bourgeon::Instance().moonlight_ui()) grid = &mui->grid_;

  // La plaque d'un monstre porte le niveau et les PV en clair. Repli seulement :
  // dès que ZC 0x0F2A répond, ses chiffres l'emportent.
  MobPlate plate;
  if (actor && is_mob_) plate = ParseMobPlate(party_);

  int level = 0;
  if (srv_valid_ && (srv_known_ & kKnownLevel)) level = srv_level_;
  else if (plate.has_level) level = plate.level;

  const bool is_npc = srv_valid_ && srv_type_ == kTypeNpc;

  // Les cinq cadres sont FRÈRES : ils s'aimantent les uns aux autres et partent
  // en bloc sous CTRL. `shown` suit les interrupteurs — un cadre éteint ne sert
  // ni d'aimant ni de compagnon de bloc.
  bool shown[kElemCount];
  for (int j = 0; j < kElemCount; ++j) shown[j] = elems_[j].show;
  ro::HudFrameSiblings siblings;
  siblings.first  = &elems_[0].rect;
  siblings.shown  = shown;
  siblings.count  = kElemCount;
  siblings.stride = static_cast<int>(sizeof(Elem));

  // Taille de texte bornée AU DESSIN, pas seulement au curseur du panneau : une
  // valeur aberrante venue d'un yaml édité à la main ne doit produire ni une
  // police de 300 px ni une taille nulle.
  float scale = text_scale_;
  if (scale < 0.5f) scale = 0.5f;
  if (scale > 3.0f) scale = 3.0f;
  const float font_px = ImGui::GetFontSize() * scale;

  // ── Les cadres agissent-ils comme la cible, CETTE frame ? ─────────────────
  // Quatre conditions, et la dernière est la moins évidente : le clic ne prend
  // la main que si CLIQUER L'ENTITÉ voudrait dire quelque chose. Un sort AU SOL
  // (mode 1) vise une case, résolue sous le curseur — un cadre n'en est pas
  // une, il redevient donc clic-traversant et le clic part au sol comme avant.
  // Les modes 3 et 5 gardent le ciblage natif, même raisonnement.
  //
  // 🔴 Verrouillé seulement : cadres déverrouillés, le clic sert à les POSER.
  const int skill_mode = ReadSkillTargetMode(game_mode);
  const bool proxy_frames =
      proxy_click_ && actor != nullptr && locked_ && gid_ != 0 &&
      (skill_mode == 0 || skill_mode == 2 || skill_mode == 4);

  for (int i = 0; i < kElemCount; ++i) {
    Elem& elem = elems_[i];
    if (!elem.show) continue;
    // Un NPC n'a pas de jauges qui veuillent dire quelque chose : deux barres
    // « inconnues » sous son nom feraient croire à une donnée manquante, alors
    // qu'il n'y a rien à mesurer. On les tait — mais seulement quand le serveur
    // a CONFIRMÉ le type, jamais sur une supposition de classe.
    if (is_npc && (i == kElemHp || i == kElemSp)) continue;
    // Sur un JOUEUR, deux cadres ne disent rien et se ferment :
    //   · race / élément / taille valent « Neutral 1 · Medium » pour TOUT le
    //     monde -- un cadre qui ne distingue personne vaut mieux fermé ;
    //   · le SP, que le serveur ne transmet qu'à la party et à la guilde : il
    //     serait « inconnus » devant presque tout le monde, et un chiffre exact
    //     sur un adversaire n'a de toute façon pas à s'afficher.
    if (is_player_ && (i == kElemKind || i == kElemSp)) continue;

    ro::HudFrameOpts opts;
    opts.locked   = locked_;
    opts.border   = border_;
    opts.rounding = rounding_;
    opts.bg       = elem.bg;
    opts.grid     = grid;
    opts.siblings = siblings;
    opts.index    = i;
    opts.sticky   = sticky_;
    opts.clickable = proxy_frames;

    bool moved = false;
    ro::HudFrameClicks clicks;
    char id[64];
    snprintf(id, sizeof(id), "##bourgeon_target_%s", kElemKeys[i]);

    // 🔴 C'est `elem.rect` LUI-MÊME qui est passé : le cadre écrit dedans, et
    // écrit aussi dans celui des frères en déplacement de bloc. Recopier dans un
    // temporaire ferait perdre le mouvement des frères.
    if (ro::BeginHudFrame(id, &elem.rect, opts, &moved, &clicks)) {
      const ImVec2 p0 = ImGui::GetWindowPos();
      const ImVec2 sz = ImGui::GetWindowSize();
      const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImU32 fg = ImGui::ColorConvertFloat4ToU32(
          ImVec4(elem.fg[0], elem.fg[1], elem.fg[2], elem.fg[3]));

      switch (i) {
        // ── Portrait ────────────────────────────────────────────────────────
        // Deux moteurs, parce qu'il y a deux natures : un monstre EST un sprite
        // (une seule ressource), un joueur est un ASSEMBLAGE (corps, tête,
        // coiffes, cape) que le composeur de pantin sait monter.
        case kElemPortrait: {
          if (!actor) break;
          const float pad = 3.0f;
          const ImVec2 q0(p0.x + pad, p0.y + pad);
          const ImVec2 q1(p1.x - pad, p1.y - pad);
          const float clock = portrait_animate_
                                  ? static_cast<float>(GetTickCount()) / 1000.0f
                                  : -1.0f;
          if (is_player_) {
            ro::DollLook look;
            look.sex           = ReadActorInt(actor, rag::actor::kSex) != 0 ? 1 : 0;
            look.job           = ReadActorInt(actor, rag::actor::kJobId);
            look.body          = ReadActorInt(actor, rag::actor::kDisplayClass);
            look.hair          = ReadActorInt(actor, rag::actor::kHairStyle);
            look.hair_color    = ReadActorInt(actor, rag::actor::kHairColor);
            look.clothes_color = ReadActorInt(actor, rag::actor::kClothesColor);
            look.head_top      = ReadActorInt(actor, rag::actor::kHeadTop);
            look.head_mid      = ReadActorInt(actor, rag::actor::kHeadMid);
            look.head_low      = ReadActorInt(actor, rag::actor::kHeadLow);
            look.garment       = ReadActorInt(actor, rag::actor::kGarment);
            // 🔴 Le chemin de corps RÉEL plutôt que la déduction : sur une 3e ou
            // 4e classe, rejouer `Job_ResolveBodyClass` diverge et affiche une
            // tenue de base. L'acteur, lui, porte le chemin que le client a
            // résolu.
            char body_path[160];
            if (fx::palette_inject::ActorBodySpritePath(gid_, body_path,
                                                        sizeof(body_path)))
              look.body_spr_override = body_path;
            ro::DrawDoll(dl, look, q0.x, q0.y, q1.x - q0.x, q1.y - q0.y,
                         portrait_dir_, portrait_anim_, clock);
          } else {
            ro::MobSpriteRes res;
            const int cls = ReadActorInt(actor, rag::actor::kJobId);
            if (ro::LoadMobSprite(cls, &res)) {
              ro::DrawMobSprite(dl, res, q0, q1, clock < 0.0f ? 0.0f : clock,
                                static_cast<unsigned>(portrait_anim_), 130.0f,
                                true);
            } else if (res.is_model) {
              // Emperium, gardiens, coffres : ces classes n'ont AUCUN sprite,
              // elles sont rendues par un modèle 3D. Le dire vaut mieux que
              // laisser un cadre vide qui passerait pour un bug.
              ro::HudCenteredText(dl, p0, p1, i18n::Tr("(modèle 3D)"), fg, font_px);
            }
          }
          break;
        }

        // ── Nom, niveau, appartenance ───────────────────────────────────────
        case kElemName: {
          if (!actor) break;
          // Ligne sociale d'un JOUEUR : sa GUILDE, et rien d'autre. La plaque
          // de nom du jeu y met aussi « (party) » et « [rang] » ; le HUD ne les
          // reprend pas — c'est une fenêtre d'adversaire, pas un annuaire.
          // (Sur un monstre, ces mêmes champs portent les stats, pas une
          // appartenance : ils sont lus ailleurs, cf. ParseMobPlate.)
          char social[200];
          social[0] = '\0';
          if (is_player_ && guild_[0])
            snprintf(social, sizeof(social), "%s", ro::WireToUtf8(guild_));

          // Une ligne, ou deux : le nom prend tout le cadre quand il est seul, la
          // bande haute quand une ligne sociale le suit.
          const bool two_lines = social[0] != '\0';
          const float split = two_lines ? (p0.y + sz.y * 0.55f) : p1.y;

          // 🔴 La place du niveau est RÉSERVÉE AVANT de centrer le nom. Centrer
          // le nom sur toute la largeur puis coller « Lv. N » à droite produit
          // les deux défauts qu'on a vus : un nom long passe SOUS le niveau, et
          // un cadre élargi laisse un vide à gauche pendant que le texte mord à
          // droite. Le nom se centre donc dans ce qui lui revient vraiment.
          char lv[32];
          lv[0] = '\0';
          float lv_width = 0.0f;
          ImFont* font = ImGui::GetFont();
          if (level > 0) {
            snprintf(lv, sizeof(lv), "Lv. %d", level);
            lv_width = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, lv).x + 8.0f;
          }
          const float name_right = p1.x - lv_width;

          const char* shown_name = name_[0] ? ro::WireToUtf8(name_)
                                            : i18n::Tr("(nom inconnu)");
          // HudCenteredText tronque de lui-même ce qui ne rentre pas : un nom de
          // monstre porte parfois ses stats entières (« 10Def 10Mdef Small… »).
          ro::HudCenteredText(dl, p0, ImVec2(name_right, split), shown_name, fg,
                              font_px);
          if (two_lines) {
            const ImU32 dim = ImGui::ColorConvertFloat4ToU32(
                ImVec4(elem.fg[0] * 0.75f, elem.fg[1] * 0.75f,
                       elem.fg[2] * 0.75f, elem.fg[3]));
            ro::HudCenteredText(dl, ImVec2(p0.x, split), p1, social, dim,
                                font_px * 0.85f);
          }
          if (lv[0]) {
            // Centré dans la bande réservée, pas collé au bord : le niveau garde
            // la même marge que le nom, quelle que soit la largeur du cadre.
            ro::HudCenteredText(dl, ImVec2(name_right, p0.y),
                                ImVec2(p1.x, split), lv, fg, font_px);
          }
          break;
        }

        // ── Barres ──────────────────────────────────────────────────────────
        // Trois sources de PV, par fiabilité décroissante : le serveur, la jauge
        // de l'acteur (ZC_HP_INFO, donc un monstre qu'on a frappé), puis le
        // pourcentage écrit dans la plaque de nom. Pour le SP, une seule source
        // possible : le serveur.
        case kElemHp:
        case kElemSp: {
          const bool is_hp = (i == kElemHp);
          bool   known = false;
          double cur = 0.0, max = 0.0;

          if (is_hp) {
            if (srv_valid_ && (srv_known_ & kKnownHp)) {
              known = true; cur = srv_hp_; max = srv_maxhp_;
            } else if (actor) {
              uint32_t gauge_hp = 0, gauge_max = 0;
              if (ReadActorGauge(actor, &gauge_hp, &gauge_max)) {
                known = true; cur = gauge_hp; max = gauge_max;
              } else if (plate.has_abs) {
                known = true; cur = plate.hp; max = plate.maxhp;
              } else if (plate.has_pct) {
                known = true; cur = plate.hp_pct; max = 100.0;
              }
            }
          } else if (srv_valid_ && (srv_known_ & kKnownSp)) {
            known = true; cur = srv_sp_; max = srv_maxsp_;
          }

          // 🔴 Un monstre n'a PAS de réserve de SP, et le serveur ne peut pas
          // le dire autrement qu'en envoyant 1 : `status_calc_misc` termine par
          // `if (!status->max_sp) status->max_sp = 1;`, parce qu'un maximum nul
          // ferait des divisions par zéro partout. Afficher « 1 / 1 » avec une
          // barre pleine serait donc doublement faux — ni le chiffre ni le
          // remplissage ne veulent dire quoi que ce soit. Aucun joueur ne tombe
          // dans ce cas : même un novice de niveau 1 a une dizaine de SP.
          const bool no_sp_pool = !is_hp && known && max <= 1.0;

          double ratio = (known && max > 0.0) ? cur / max : 0.0;
          if (no_sp_pool) ratio = 0.0;  // pas de réserve, donc rien à remplir
          if (ratio < 0.0) ratio = 0.0;
          if (ratio > 1.0) ratio = 1.0;

          // Remplissage : coins arrondis seulement à l'arrière, pour que le front
          // de la jauge reste une arête franche (comme les barres de Basic Info).
          if (ratio > 0.0) {
            const float fill_px = static_cast<float>(sz.x * ratio);
            if (fill_px >= 1.0f) {
              const ImU32 fill = ImGui::ColorConvertFloat4ToU32(ImVec4(
                  elem.fg[0], elem.fg[1], elem.fg[2], elem.fg[3]));
              const ImDrawFlags corners = (ratio >= 0.999)
                                              ? ImDrawFlags_RoundCornersAll
                                              : ImDrawFlags_RoundCornersLeft;
              dl->AddRectFilled(p0, ImVec2(p0.x + fill_px, p1.y), fill,
                                rounding_, corners);
            }
          }

          // 🔴 Les PV d'un JOUEUR ne se chiffrent pas. Connaître au point de vie
          // près ce qui reste à un adversaire est un avantage que le jeu ne donne
          // pas : la jauge se remplit, et c'est tout — ni valeurs, ni pourcentage.
          const bool numbers_forbidden = is_hp && is_player_ && known;

          if (text_mode_ != 0 && !numbers_forbidden) {
            char text[96];
            // « inconnus » plutôt qu'un zéro : une jauge vide et une jauge qu'on
            // ne connaît pas ne disent pas la même chose.
            if (!known) {
              snprintf(text, sizeof(text), "%s", i18n::Tr("inconnus"));
            } else if (no_sp_pool) {
              // Terme de jeu, donc en anglais comme « HP » et « SP » eux-mêmes,
              // et indépendant du mode de texte : il n'y a ni valeur ni
              // pourcentage à décliner.
              snprintf(text, sizeof(text), "No SP");
            } else if (text_mode_ == 1) {
              snprintf(text, sizeof(text), "%.0f %%", ratio * 100.0);
            } else if (text_mode_ == 2) {
              snprintf(text, sizeof(text), "%llu / %llu",
                       static_cast<unsigned long long>(cur),
                       static_cast<unsigned long long>(max));
            } else {
              snprintf(text, sizeof(text), "%llu / %llu (%.0f %%)",
                       static_cast<unsigned long long>(cur),
                       static_cast<unsigned long long>(max), ratio * 100.0);
            }
            ro::HudCenteredText(dl, p0, p1, text, IM_COL32(255, 255, 255, 255),
                                font_px);
          }
          break;
        }

        // ── Race · élément · taille ─────────────────────────────────────────
        // Le serveur les donne en indices (fiables, indépendants des options
        // d'affichage) ; la plaque de nom d'un monstre porte les mêmes notions en
        // toutes lettres — race dans le champ guilde, élément dans le rang.
        case kElemKind: {
          if (!actor) break;
          char kind[192];
          kind[0] = '\0';
          const bool from_server =
              srv_valid_ && (srv_known_ & kKnownKind) && srv_type_ != kTypeNpc;
          if (from_server) {
            const char* race = NameFromTable(
                kRaceNames, sizeof(kRaceNames) / sizeof(*kRaceNames), srv_race_);
            const char* size = NameFromTable(
                kSizeNames, sizeof(kSizeNames) / sizeof(*kSizeNames), srv_size_);
            const char* ele = NameFromTable(
                kEleNames, sizeof(kEleNames) / sizeof(*kEleNames), srv_ele_);
            // Le niveau élémentaire se colle à l'élément (« Neutral 1 »), comme
            // partout ailleurs dans le jeu.
            char ele_part[48];
            ele_part[0] = '\0';
            if (ele) {
              if (srv_ele_lv_ > 0)
                snprintf(ele_part, sizeof(ele_part), "%s %u", ele, srv_ele_lv_);
              else
                snprintf(ele_part, sizeof(ele_part), "%s", ele);
            }
            AppendPart(kind, sizeof(kind), race);
            AppendPart(kind, sizeof(kind), ele_part);
            AppendPart(kind, sizeof(kind), size);
            // `boss` est la classe rAthena : 1 = MVP, 2 = gardien de forteresse.
            if (srv_boss_ == 1) AppendPart(kind, sizeof(kind), "MVP");
            else if (srv_boss_ == 2) AppendPart(kind, sizeof(kind), "Guardian");
          } else if (is_mob_) {
            AppendPart(kind, sizeof(kind), guild_[0] ? ro::WireToUtf8(guild_) : nullptr);
            AppendPart(kind, sizeof(kind), rank_[0] ? ro::WireToUtf8(rank_) : nullptr);
          }
          if (kind[0]) ro::HudCenteredText(dl, p0, p1, kind, fg, font_px);
          break;
        }
        default:
          break;
      }
    }
    ro::EndHudFrame();

    // Le clic est MIS EN ATTENTE, jamais joué ici : rejouer le clic natif est un
    // appel qui peut ouvrir une boîte de message (surcharge de poids) et
    // relancer le tick du mode — interdit entre NewFrame et Render. Cf.
    // OnGameFramePulse.
    // Le curseur du jeu prend l'épée : un cadre qui se comporte comme le monstre
    // doit aussi en avoir l'air. Consommé par le hook de rendu du curseur à la
    // frame suivante, comme pour tous les widgets du toolkit.
    if (clicks.hovered) ro::SetHoverCursor(kRoCursorAttack);

    if (clicks.left)  proxy_click_gid_ = gid_;
    // Le clic DROIT ouvre le menu contextuel de l'entité, comme sur son sprite.
    // Même différé, et pour une raison de plus : la construction du menu lit le
    // dictionnaire de noms et la liste d'amis.
    if (clicks.right) proxy_menu_gid_ = gid_;

    if (moved) geometry_dirty_ = true;  // MoonlightUi persiste, une fois
  }
}

// ── Le clic reçu par un cadre, rejoué hors frame ImGui ──────────────────────

void TargetFrame::OnGameFramePulse() {
  if (world_dblclick_) {
    world_dblclick_ = false;
    FlushWorldDoubleClick();
  }
  if (proxy_click_gid_ == 0 && proxy_menu_gid_ == 0) return;
  const uint32_t gid  = proxy_click_gid_;
  const uint32_t menu = proxy_menu_gid_;
  proxy_click_gid_ = 0;
  proxy_menu_gid_  = 0;

  if (!enabled_ || !proxy_click_) return;
  auto& bourgeon = Bourgeon::Instance();
  if (bourgeon.client().timestamp() != 20250716) return;
  // Mêmes gardes que partout ailleurs : c'est `DrawHud` qui tient `gid_` à
  // jour, et il ne tourne ni pendant un chargement de carte, ni hors du monde.
  if (bourgeon.IsMapLoading() || !bourgeon.IsGameActive()) return;

  void* gm = rag::ActiveModeSafe();
  if (!gm) return;

  // ── Clic DROIT : le menu contextuel de l'entité ───────────────────────────
  // Le cadre est une copie du sprite, donc il en porte aussi le menu. C'est
  // `EntityContextMenu` qui décide de ce qu'il contient et de ce qu'il grise —
  // on ne lui donne que la cible.
  if (menu != 0) {
    void* actor = gamescene::FindActorByGid(menu);
    if (actor) {
      if (auto* ctx = bourgeon.entity_context_menu())
        ctx->OpenForEntity(gm, menu,
                           static_cast<uint32_t>(ReadActorInt(actor, rag::actor::kJobId)),
                           kPickCategoryActor);
    }
  }

  if (gid == 0) return;
  // La cible a pu disparaître entre le clic et ce battement (elle est morte, on
  // l'a perdue de vue) : cliquer un GID qui n'existe plus n'a pas de sens.
  if (!gamescene::FindActorByGid(gid)) return;

  // Le ciblage est relu MAINTENANT, pas celui de la frame du clic : c'est lui
  // que le natif va consulter. Il n'a de toute façon pas pu changer entre les
  // deux — rien n'arme une compétence pendant que la souris descend.
  const int skill_mode = ReadSkillTargetMode(gm);

  // ── Une fois, ou en continu ? ─────────────────────────────────────────────
  const bool continuous = WantsContinuousAttack(gm);

  // 🔴 Sur un CADRE, un seul clic suffit — même sous « le clic cible sans
  // attaquer ». Ce réglage existe parce qu'un clic DANS LE MONDE est ambigu : on
  // désigne souvent une entité sans vouloir l'engager, et le double-clic sert à
  // lever le doute. Ici il n'y a aucun doute à lever — la cible est déjà
  // désignée, et il a fallu viser un rectangle de HUD pour l'atteindre.
  //
  // ⚠ Seulement quand rien n'est armé : une compétence a son propre chemin dans
  // `PostActorClickAction`, que le réglage ne bloque de toute façon pas. Le
  // `type` du clic, lui, ne décide pas de la durée (cf. kClickTypeContinuous) :
  // on le passe à l'identique du natif pour que le cadre soit vraiment une copie
  // du sprite.
  if (skill_mode == 0) {
    EngageAttack(gm, gid, continuous);
  } else if (!RunActorClick(gm, gid, continuous ? kClickTypeContinuous
                                                : kClickTypeOnce)) {
    return;
  }

  // Compétence lancée : on désarme le ciblage, comme le fait le pipeline souris
  // natif après un lancement. Sans cela, le curseur de visée resterait armé et
  // le clic suivant relancerait la compétence.
  if (skill_mode == 2 || skill_mode == 4) LeaveSkillTargeting(gm);
}

uint32_t TargetFrame::SkillTargetGid(int targeting_mode) const {
  if (!enabled_ || !cast_on_target_) return 0u;
  if (gid_ == 0 || hidden_) return 0u;
  if (targeting_mode != 2 && targeting_mode != 4) return 0u;
  // Les offsets sont ceux de CE build : ailleurs, on ne propose rien.
  if (Bourgeon::Instance().client().timestamp() != 20250716) return 0u;
  return ValidSkillTarget(gid_, targeting_mode);
}

// ── Panneau de réglages ─────────────────────────────────────────────────────

bool TargetFrame::DrawSettings() {
  bool changed = false;

  changed |= ro::RoCheckbox(i18n::Tr("Activer le mode Ciblage + HUD"), &enabled_);
  SameLine();
  HelpMarker(i18n::Tr(
      "La cible est la dernière entité cliquée. Le HUD s'allume dessus et y "
      "reste quand on cesse de l'attaquer ; la flèche de ciblage du jeu la suit "
      "aussi longtemps. Tout s'éteint si la cible change, si elle s'éloigne "
      "hors de vue, ou par « Masquer la fenêtre de cible » dans le menu "
      "contextuel de l'entité.\n\nDécoché, TOUT ce qui suit s'arrête : le HUD, "
      "la flèche, les raccourcis de ciblage, le clic sans attaque, les cadres "
      "qui agissent et les sorts qui partent sur la cible."));

  // 🔴 Un interrupteur MAÎTRE grise ce qu'il commande. Sans cela, un réglage
  // resterait cliquable et sans effet — le pire des deux mondes : on croit
  // l'avoir posé, il ne fait rien, et rien ne le dit.
  ImGui::BeginDisabled(!enabled_);

  changed |= ro::RoCheckbox(i18n::Tr("Verrouiller (fige + clic-traversant)"), &locked_);
  SameLine();
  HelpMarker(i18n::Tr(
      "Fige les cadres et laisse passer les clics vers le jeu. À décocher pour "
      "les déplacer : on saisit un cadre n'importe où, ses BORDS le "
      "redimensionnent."));

  changed |= ro::RoCheckbox(i18n::Tr("Mode placement (garder les cadres sans cible)"),
                            &layout_mode_);
  SameLine();
  HelpMarker(i18n::Tr(
      "Sans cible il n'y a rien à attraper : ce mode garde les cadres à l'écran "
      "le temps de les poser. À décocher une fois le HUD en place."));

  changed |= ro::RoCheckbox(i18n::Tr("Liseré autour des cadres"), &border_);
  changed |= ro::RoCheckbox(i18n::Tr("Aimanter les cadres entre eux"), &sticky_);
  SameLine();
  HelpMarker(i18n::Tr(
      "Pendant un déplacement, les bords s'alignent sur ceux des autres cadres. "
      "CTRL + déplacement emmène en bloc tous les cadres qui se touchent."));

  ImGui::PushItemWidth(160.0f);
  changed |= WheelSliderFloat(i18n::Tr("Arrondi"), &rounding_, 0.0f, 16.0f);
  changed |= WheelSliderFloat(i18n::Tr("Taille du texte"), &text_scale_, 0.5f, 3.0f);

  // Termes de jeu en anglais dans les barres ; les libellés du combo, eux, sont
  // du français traduit à la lecture par RoCombo.
  static const char* const kTextModes[] = {"Aucun", "Pourcentage", "Valeurs",
                                           "Valeurs et pourcentage"};
  changed |= ro::RoCombo(i18n::Tr("Texte des barres"), &text_mode_, kTextModes, 4);
  ImGui::PopItemWidth();

  Separator();
  SeparatorText(i18n::Tr("Portrait"));
  ImGui::PushItemWidth(160.0f);
  changed |= WheelSliderInt(i18n::Tr("Orientation"), &portrait_dir_, 0, 7);
  SameLine();
  HelpMarker(i18n::Tr("0 = de face, comme le portrait du personnage."));
  changed |= WheelSliderInt(i18n::Tr("Action"), &portrait_anim_, 0, 8);
  ImGui::PopItemWidth();
  changed |= ro::RoCheckbox(i18n::Tr("Animer"), &portrait_animate_);
  SameLine();
  HelpMarker(i18n::Tr(
      "Un monstre est un sprite, un joueur est un assemblage (corps, tête, "
      "coiffes, cape) monté par le composeur de pantin. Les classes rendues par "
      "un modèle 3D (Emperium, gardiens, coffres) n'ont pas de sprite : le cadre "
      "le dit."));

  Separator();
  SeparatorText(i18n::Tr("Ciblage"));
  changed |= ro::RoCheckbox(i18n::Tr("Flèche de ciblage du jeu sur la cible"),
                            &native_marker_);
  SameLine();
  HelpMarker(i18n::Tr(
      "Le petit triangle blanc au-dessus de l'entité. Le jeu ne l'affiche que "
      "tant que la souris engage la cible : il disparaît dès qu'on regarde "
      "ailleurs, et le ciblage au clavier ne l'allume pas du tout. Coché, il "
      "suit la cible du HUD aussi longtemps qu'elle."));

  changed |= ro::RoCheckbox(i18n::Tr("Le clic cible sans attaquer"),
                            &click_no_attack_);
  SameLine();
  HelpMarker(i18n::Tr(
      "Cliquer une entité la prend pour cible, mais n'engage rien : le "
      "personnage ne se déplace pas vers elle et ne frappe pas.\n\nPour "
      "attaquer, DOUBLE-CLIQUE — ou passe par « Attaquer » du menu contextuel. "
      "Le délai du double-clic est celui de Windows.\n\nLes compétences ne sont "
      "pas concernées, ni les ordres au pet, à l'homoncule ou au mercenaire, ni "
      "s'asseoir."));

  changed |= ro::RoCheckbox(i18n::Tr("Les sorts ciblés partent sur la cible"),
                            &cast_on_target_);
  SameLine();
  HelpMarker(i18n::Tr(
      "Une compétence ciblée demande normalement deux gestes : la touche arme "
      "la visée, le clic désigne qui. Coché, c'est la cible du HUD qui répond — "
      "la souris n'a plus à être sur l'entité.\n\nElle garde la priorité quand "
      "elle désigne vraiment quelqu'un : ce réglage comble un vide, il ne "
      "détourne pas une visée. Les sorts de zone ne sont pas concernés, ils "
      "visent une case.\n\nComme tout QuickCast, maintenir la touche relance la "
      "compétence sur la cible."));

  changed |= ro::RoCheckbox(i18n::Tr("Les cadres agissent comme la cible"),
                            &proxy_click_);
  SameLine();
  HelpMarker(i18n::Tr(
      "Cliquer un cadre du HUD revient à cliquer l'entité elle-même : attaque, "
      "compétence ciblée armée — c'est le même chemin du client qui est "
      "rejoué.\n\nUn SEUL clic attaque, même si « le clic cible sans attaquer » "
      "est coché : ce réglage lève l'ambiguïté d'un clic dans le monde, et il "
      "n'y en a aucune sur un cadre — la cible est déjà désignée.\n\n⚠ Un cadre "
      "qui agit REPREND LA SOURIS au jeu tant que le curseur est dessus : ni "
      "clic vers le sol derrière, ni rotation de caméra. Il ne le fait que "
      "quand il y a une cible ET que le clic aurait un sens — un sort de zone "
      "vise une case, le cadre lui laisse alors le passage.\n\nSans effet tant "
      "que les cadres ne sont pas verrouillés : le clic sert alors à les "
      "poser."));

  // 🔴 Coché et sans effet, c'est exactement ce que le reste de ce panneau
  // s'interdit. Tant que les cadres ne sont pas verrouillés, le clic leur sert
  // à se faire poser : on le dit ici plutôt que dans une infobulle qu'on
  // n'ouvre qu'en cas de doute. Ocre d'avertissement du projet.
  if (proxy_click_ && !locked_) {
    ImGui::TextColored(ImVec4(166 / 255.0f, 102 / 255.0f, 0.0f, 1.0f),
                       i18n::Tr("Sans effet : les cadres ne sont pas "
                                "verrouillés, le clic sert à les déplacer."));
  }

  Separator();
  SeparatorText(i18n::Tr("Ciblage au clavier"));
  TextWrapped(i18n::Tr(
      "Trois actions à lier dans l'écran des raccourcis : « Cible suivante », "
      "« Cible précédente » et « Cible la plus proche ». Les deux premières "
      "passent d'un monstre à l'autre parmi ceux qui sont à l'écran, du plus "
      "proche au plus loin ; la troisième prend directement le plus proche, sans "
      "rien parcourir. Tab et Maj+Tab conviennent bien, et restent au chat tant "
      "que la barre de saisie a le focus."));
  changed |= ro::RoCheckbox(i18n::Tr("Boucler après la dernière cible"),
                            &cycle_wrap_);

  ImGui::PushItemWidth(160.0f);
  changed |= WheelSliderInt(i18n::Tr("Reprendre au plus proche après (ms)"),
                            &cycle_reset_ms_, 0, 5000);
  ImGui::PopItemWidth();
  SameLine();
  HelpMarker(i18n::Tr(
      "Passé ce délai sans cycler, le prochain appui repart du monstre le plus "
      "proche au lieu de continuer où le cycle en était.\n\nC'est ce qui sépare "
      "les deux usages du cyclage : enchaîner les appuis pour PARCOURIR ce qu'il "
      "y a autour, ou appuyer une fois pour PRENDRE une cible. Sans ce délai, le "
      "second héritait de la position laissée par le premier — on rappuyait "
      "après un combat et on repartait au cinquième monstre.\n\n0 = jamais : le "
      "cycle se souvient indéfiniment."));

  Separator();
  SeparatorText(i18n::Tr("Cadres"));
  for (int i = 0; i < kElemCount; ++i) {
    Elem& elem = elems_[i];
    ImGui::PushID(i);
    changed |= ro::RoCheckbox(i18n::Tr(kElemLabels[i]), &elem.show);
    SameLine();
    changed |= ColorEdit4WithAlphaBar(i18n::Tr("Fond"), elem.bg);
    SameLine();
    // Pour les deux barres, `fg` est le REMPLISSAGE, pas le texte : le texte y
    // est toujours clair, parce qu'il se lit sur la couleur choisie autant que
    // sur le fond.
    changed |= ColorEdit4WithAlphaBar(
        (i == kElemHp || i == kElemSp) ? i18n::Tr("Remplissage") : i18n::Tr("Texte"),
        elem.fg);
    ImGui::PopID();
  }

  if (ro::RoButton(i18n::Tr("Réinitialiser la disposition"))) {
    for (int i = 0; i < kElemCount; ++i) {
      elems_[i].rect.x = kDefaultRects[i].x;
      elems_[i].rect.y = kDefaultRects[i].y;
      elems_[i].rect.w = kDefaultRects[i].w;
      elems_[i].rect.h = kDefaultRects[i].h;
    }
    changed = true;
  }
  SameLine();
  HelpMarker(i18n::Tr("Repose les cinq cadres à leur place d'origine, en haut à "
                      "gauche. Les couleurs et les interrupteurs ne bougent pas."));

  ImGui::EndDisabled();
  return changed;
}
