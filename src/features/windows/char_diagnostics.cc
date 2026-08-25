#include "features/windows/char_diagnostics.h"
#include "ragnarok/actor.h"  // rag::actor : les offsets de CActorSprite

#include <Windows.h>  // SEH autour des déréférencements d'objets natifs
// 🔴 `timeGetTime` (winmm), pas `GetTickCount` : c'est l'horloge que l'acteur
// LUI-MÊME utilise pour horodater ses animations (+0x8c), et comparer deux
// horloges différentes donnerait un âge d'animation faux de quelques dizaines de
// millisecondes — sur une image de 24 ms, ça se voit.
#include <mmsystem.h>

#include <atomic>  // compteurs d'envois, alimentés depuis la couche réseau
#include <cfloat>  // FLT_MAX (contrainte de taille de la fenêtre)
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "bourgeon.h"
#include "features/patches/pick_quad_tweaks.h"  // capture centrale des minimums
#include "features/staff_gate.h"
#include "imgui.h"
#include "ragnarok/game_scene.h"
#include "ragnarok/globals.h"
#include "ragnarok/own_actor.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"
#include "ui/ui_palette.h"  // ro::pal : la palette de l'UI

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace {

// ── Globales du client (20250716, base 0x400000) ─────────────────────────────
//
// Tout le bloc « personnage local » (identité, stats, sous-stats de combat,
// vitalité, poids, points) vient désormais de `ragnarok/globals.h` : c'est la
// fenêtre qui en lisait le plus, et c'est elle qui en a exhumé le doublonnage.
// Ne restent ici que les globales qu'AUCUN autre module ne lit.
//
// 🔴 Deux d'entre elles méritent leur avertissement :
//   * `rag::kOwnAttackDelayAddr` est LE délai d'attaque en millisecondes, tel que
//     le serveur l'a envoyé. La fenêtre Status n'en montre jamais que l'ASPD
//     dérivée — `rag::AspdFromAmotion` — et c'est cette division qui fait croire
//     à une « statistique » là où il y a un TEMPS. Tout le reste de cette fenêtre
//     en découle.
//   * `rag::kOwnWalkSpeedAddr` est en millisecondes PAR CELLULE (200 = vitesse de
//     base) : plus il est petit, plus le personnage va vite.
constexpr uintptr_t kBodyState     = 0x015fb2ac;  // g_OwnState_BodyState (opt1)
constexpr uintptr_t kHealthState   = 0x015fb2b0;  // g_OwnState_HealthState (opt2)
constexpr uintptr_t kEffectState   = 0x015fb2b4;  // g_OwnState_EffectState (option)

// 🔴 Le PLAFOND de ralentissement de l'animation, en ms (432 dans l'exe livré,
// mais lu vivant : rien ne garantit qu'un patch ne le bouge pas). Au-delà,
// l'animation d'attaque ne s'allonge plus — elle se termine et le personnage
// attend. C'est la borne haute de `Actor_SetMotionSpeedFactor` (0x00c62810) et de
// `CActorSprite_SetAttackMotionFactor` (0x00d3e290).
constexpr uintptr_t kMotionSpeedCap = 0x01228f60;  // g_MotionSpeedCapMs

// La CONSTANTE de calibrage des `.act` : une animation d'attaque est écrite pour
// un délai de 432 ms, et le client met le facteur à `amotion / 432`.
constexpr float kMotionReferenceMs = 432.0f;

// 🔴 Le pas de temps de l'animation, en millisecondes : `CActorSprite_AdvanceAnimState`
// (0x00c46c80) et `CActorSprite_AdvanceAnimFrame` (0x00c464b0) divisent tous deux
// l'écoulé par 24.0 — PAS par les 25 ms des outils de format de fichier. Un
// affichage calé sur 25 se tromperait de 4 %.
constexpr float kAnimTickMs = 24.0f;

// `Job_GetDisplayNameOrResName` : id de classe -> nom lisible (tables Lua du
// client). Même appel que l'inspecteur d'entités ; `sex = -1` laisse le client
// trancher, ce qui vaut en jeu.

// 🔴 `CActorSprite_ResolveDisplayJob(actor, -1)` : LA classe dont le client tire
// le sprite. Elle part de `acteur+0x25c` puis applique les masques d'option —
// monture, mado, magicien changé en poring, invisibilité — et c'est son résultat,
// pas le champ brut, qui décide du fichier chargé. Un déguisement qui passerait
// par une option ne se verrait QUE là.
//
// Sans effet de bord : elle ne fait que lire l'acteur et appeler deux de ses
// accesseurs virtuels. Appelée sous SEH, comme tout le reste ici.
constexpr uintptr_t kResolveDisplayJob = 0x00c43230;
using ResolveJobFn = int(__fastcall*)(void*, void*, int);

// ── Offsets de l'ACTEUR (CActorSprite) ───────────────────────────────────────
// Établis au désassemblage le 2026-08-17 ; chaque champ porte la fonction qui le
// prouve, parce qu'un offset sans témoin est un offset qu'on ne saura pas
// vérifier au prochain client.

// Chaîne jusqu'à l'acteur du joueur, identique à `rag::ReadOwnActorSprites` :
// mode actif -> gestionnaire d'acteurs (+0xcc) -> acteur du joueur (+0x2c).
constexpr int kMgr_OwnActor  = 0x2c;

// ── Offsets de la RESSOURCE .act ─────────────────────────────────────────────
// `Act_GetActionFrames` (0x0070f2c0) : vecteur d'actions à +0x110/+0x114, 12
// octets par entrée, et chaque entrée est elle-même un vecteur d'images de 68
// octets (`Act_GetFrameCount` 0x0070f6b0).
// `Act_GetFrameDelay` (0x0070f3d0) : vecteur de vitesses à +0x12c/+0x130, un
// float par action, valeur de repli 4.0 hors bornes.
constexpr int kAct_ActionsBegin = 0x110;
constexpr int kAct_ActionsEnd   = 0x114;
constexpr int kAct_ActionStride = 12;
constexpr int kAct_FrameStride  = 68;
constexpr int kAct_SpeedsBegin  = 0x12c;
constexpr int kAct_SpeedsEnd    = 0x130;
constexpr float kAct_SpeedFallback = 4.0f;

// Le chemin résolu que `UITextureMgr_Load` garde dans toute ressource, à +0x14.
constexpr int kRes_Path = 0x14;

// ── Opcodes observés ─────────────────────────────────────────────────────────
// Les mêmes que le DPS meter, et pour la même raison : ce sont les seuls paquets
// qui portent amotion et dmotion coup par coup. On s'inscrit quand même de notre
// côté — l'inscription prend le MAXIMUM des longueurs demandées, donc deux
// modules qui observent le même opcode ne se marchent pas dessus.
constexpr uint16_t kOpNotifyAct   = 0x08c8;  // ZC_NOTIFY_ACT (PACKETVER >= 20131223)
constexpr uint16_t kOpNotifySkill = 0x01de;  // ZC_NOTIFY_SKILL

#pragma pack(push, 1)
struct NotifyActPayload {
  int32_t  src_id;
  int32_t  dst_id;
  int32_t  tick;
  int32_t  src_speed;   // amotion de l'attaquant
  int32_t  dst_speed;   // dmotion imposé à la cible
  int32_t  damage;
  int8_t   is_sp_damage;
  uint16_t div;
  uint8_t  type;
  int32_t  damage2;
};

struct NotifySkillPayload {
  uint16_t skill_id;
  uint32_t src_id;
  uint32_t dst_id;
  uint32_t start_time;
  int32_t  attack_mt;    // amotion
  int32_t  attacked_mt;  // dmotion
  int32_t  damage;
  int16_t  level;
  int16_t  count;
  int8_t   action;
};
#pragma pack(pop)

// ── Lectures ─────────────────────────────────────────────────────────────────

// La lecture gardée d'un int : celle de globals.h. Le `using` laisse les
// points d'appel de ce fichier tels quels.
using rag::ReadInt;
// Une valeur 64 bits rangée en deux globales consécutives (lo puis hi).
uint64_t ReadInt64(uintptr_t lo_addr) {
  __try {
    const uint32_t lo = *reinterpret_cast<const uint32_t*>(lo_addr);
    const uint32_t hi = *reinterpret_cast<const uint32_t*>(lo_addr + 4);
    return (static_cast<uint64_t>(hi) << 32) | lo;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

template <typename T>
T ReadField(const void* base, int offset) {
  __try {
    return *reinterpret_cast<const T*>(reinterpret_cast<const char*>(base) +
                                       offset);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return T(); }
}

// L'acteur du joueur, ou nullptr hors jeu.
void* OwnActor() {
  __try {
    void* mode = rag::ActiveModeIfReady();
    if (!mode) return nullptr;
    void* mgr = *reinterpret_cast<void**>(reinterpret_cast<char*>(mode) +
                                          gamescene::kGmActorMgr);
    if (!mgr) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(mgr) +
                                     kMgr_OwnActor);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Le chemin de fichier qu'une ressource chargée garde en elle. Recopié tout de
// suite : garder le pointeur, c'est garder une adresse dans un objet du client.
void ResPath(const void* res, char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  out[0] = '\0';
  if (!res) return;
  __try {
    const char* p = reinterpret_cast<const char*>(res) + kRes_Path;
    size_t i = 0;
    for (; i + 1 < out_size && p[i]; ++i) out[i] = p[i];
    out[i] = '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

// Nombre d'ENTRÉES d'un `.act` chargé (0 si la ressource est illisible).
//
// 🔴 Une « entrée » n'est PAS une action : le fichier range `action × 8 +
// direction`, donc un sprite joueur à 13 actions en compte 104. C'est cet index
// composé que le client passe à `Act_GetFrameCount` (il lui donne acteur+0x38).
int ActActionCount(const void* act) {
  __try {
    const char* a = reinterpret_cast<const char*>(act);
    const uintptr_t begin = *reinterpret_cast<const uintptr_t*>(a + kAct_ActionsBegin);
    const uintptr_t end   = *reinterpret_cast<const uintptr_t*>(a + kAct_ActionsEnd);
    if (end < begin) return 0;
    return static_cast<int>((end - begin) / kAct_ActionStride);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Images d'une ENTRÉE (`action × 8 + direction`). 0 = l'entrée n'existe pas dans
// ce fichier ; le client, lui, rendrait 1 dans ce cas (`Act_GetFrameCount` :
// `if (!result) return 1`), ce qui masquerait justement l'anomalie qu'on cherche
// — une animation d'une seule image se termine tout de suite.
int ActFrameCount(const void* act, int index) {
  if (index < 0 || index >= ActActionCount(act)) return 0;
  __try {
    const char* a = reinterpret_cast<const char*>(act);
    const uintptr_t begin = *reinterpret_cast<const uintptr_t*>(a + kAct_ActionsBegin);
    const char* entry = reinterpret_cast<const char*>(begin) +
                        static_cast<size_t>(index) * kAct_ActionStride;
    const uintptr_t fbeg = *reinterpret_cast<const uintptr_t*>(entry);
    const uintptr_t fend = *reinterpret_cast<const uintptr_t*>(entry + 4);
    if (fend < fbeg) return 0;
    return static_cast<int>((fend - fbeg) / kAct_FrameStride);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Vitesse DÉCLARÉE d'une entrée, en « ticks » d'animation. Même repli que le
// client (4.0) quand l'index sort du tableau. Le client la lit à l'index SANS
// direction (acteur+0x34), là où il compte les images avec (acteur+0x38).
float ActSpeed(const void* act, int index) {
  __try {
    const char* a = reinterpret_cast<const char*>(act);
    const uintptr_t begin = *reinterpret_cast<const uintptr_t*>(a + kAct_SpeedsBegin);
    const uintptr_t end   = *reinterpret_cast<const uintptr_t*>(a + kAct_SpeedsEnd);
    if (index < 0 || end < begin ||
        static_cast<uintptr_t>(index) >= (end - begin) / 4)
      return kAct_SpeedFallback;
    return *(reinterpret_cast<const float*>(begin) + index);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return kAct_SpeedFallback; }
}

// La classe d'affichage résolue par le client, ou 0 si l'appel échoue.
int ResolveDisplayJob(void* actor) {
  __try {
    return reinterpret_cast<ResolveJobFn>(kResolveDisplayJob)(actor, nullptr, -1);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Nom lisible d'une classe (joueur comme monstre). Chaîne dans la code-page du
// CLIENT — elle vient de ses tables Lua, pas du serveur.
// `rag::JobNameMySex` ici, et c'est VOULU : cette fenêtre ne diagnostique que le
// personnage du joueur (`job_real`, `job_shown`, `job_resolved` sont tous les
// siens), donc le sexe du client est le bon. Les autres appelants de la famille
// nomment des TIERS et doivent prendre `rag::JobName` (cf. globals.h).
bool JobDisplayName(int job, char* out, size_t out_size) {
  const char* name = rag::JobNameMySex(job);
  if (!name) return false;
  bool ok = false;
  __try {
    if (*name) {
      lstrcpynA(out, name, static_cast<int>(out_size));
      ok = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  return ok;
}

// ── Étiquettes ───────────────────────────────────────────────────────────────

// Les actions du jeu de sprites JOUEUR. Cette numérotation est celle que le
// projet emploie déjà (sélecteur de pose de la feuille de personnage) et que
// `CActorSprite_MapStateToActionBase` (0x00c42c90) confirme : mort = 8,
// touché = 6, ramasser = 3, en garde = 4.
//
// ⚠ Elle ne vaut PAS pour un sprite de monstre, qui n'a que cinq actions dans un
// autre ordre. C'est exactement ce que la fenêtre doit rendre visible sous
// déguisement — d'où le nombre d'actions affiché à côté.
const char* ActionName(int action) {
  switch (action) {
    case 0:  return "IDLE";
    case 1:  return "WALK";
    case 2:  return "SIT";
    case 3:  return "PICKUP";
    case 4:  return "READYFIGHT";
    case 5:  return "ATTACK1";
    case 6:  return "HURT";
    case 7:  return "FREEZE";
    case 8:  return "DEAD";
    case 9:  return "FREEZE2";
    case 10: return "ATTACK2";
    case 11: return "ATTACK3";
    case 12: return "SKILL";
    default: return nullptr;
  }
}

// 🔴 LE GARDE QUI DÉCIDE DE LA CADENCE DES SKILLS.
// `Actor_ProcessPendingAction_Tick` (0x00d43400) consomme la requête de skill en
// attente et n'appelle `CGameMode::SendMsg(0x45)` — le seul point d'émission de
// CZ_USE_SKILL — QUE si l'état de motion vaut 0, 1, 4, ou >= 16. Tout autre état
// (2 et 9 = attaque, en tête) le fait sortir sans rien consommer : la requête
// est réessayée à la frame suivante.
//
// Conséquence directe, et c'est tout le sujet de cette fenêtre : le temps passé
// en état d'attaque BORNE la cadence des compétences, et ce temps vaut la durée
// de l'animation — donc `images(action × 8 + direction) × délai × 24 ms`.
bool MotionStateBlocksSkill(int state) {
  return !(state == 0 || state == 1 || state == 4 || state >= 16);
}

// Les états de motion dont l'équivalence est ÉTABLIE par la table
// `CActorSprite_MapStateToActionBase`. Les autres ne sont pas étiquetés : il y en
// a une soixantaine, et leur inventer un nom ferait passer une supposition pour
// de la RE.
const char* MotionStateName(int state) {
  switch (state) {
    case 0:  return i18n::Tr("repos");
    case 1:  return i18n::Tr("marche");
    case 2:  return i18n::Tr("attaque (ATTACK2)");
    case 3:  return i18n::Tr("mort");
    case 4:  return i18n::Tr("touché");
    case 5:  return i18n::Tr("ramasse un objet");
    case 9:  return i18n::Tr("attaque (ATTACK3)");
    case 11: return i18n::Tr("en garde");
    case 12: return i18n::Tr("attaque (ATTACK1)");
    default: return nullptr;
  }
}

// Le champ `type` de ZC_NOTIFY_ACT. Valeurs de rAthena (`enum e_damage_type`).
const char* BlowTypeName(int type) {
  switch (type) {
    case 0:  return i18n::Tr("coup normal");
    case 1:  return i18n::Tr("ramassage");
    case 2:  return i18n::Tr("assis");
    case 3:  return i18n::Tr("debout");
    case 4:  return i18n::Tr("coup encaissé (endure)");
    case 8:  return i18n::Tr("coup multiple");
    case 9:  return i18n::Tr("coup multiple critique");
    case 10: return i18n::Tr("critique");
    case 11: return i18n::Tr("raté");
    default: return nullptr;
  }
}

// ── Rendu : une ligne « libellé / valeur » ───────────────────────────────────
// Mêmes couleurs et même alignement que l'inspecteur d'entités : les deux
// fenêtres se lisent souvent côte à côte. L'alignement est en unités de POLICE,
// pas en pixels — les familles proposées par le réglage d'interface n'ont pas la
// même chasse.

void Row(const char* label, const char* value, bool warn = false) {
  ImGui::TextColored(ro::pal::kLabel, "%s", label);
  ImGui::SameLine(ImGui::GetFontSize() * 11.0f);
  ImGui::TextColored(warn ? ro::pal::kWarn : ro::pal::kValue, "%s", value);
}

void RowFmt(const char* label, const char* fmt, ...) {
  char buffer[320];
  va_list args;
  va_start(args, fmt);
  _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
  va_end(args);
  Row(label, buffer);
}

// Ligne d'un rapport texte (presse-papier).
void Line(std::string* out, const char* fmt, ...) {
  char buffer[400];
  va_list args;
  va_start(args, fmt);
  _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
  va_end(args);
  out->append(buffer);
  out->append("\r\n");
}

const char* kStatName[6] = {"STR", "AGI", "VIT", "INT", "DEX", "LUK"};

// Les actions sondées sur leurs huit directions, après l'action jouée : les
// trois attaques du jeu de sprites joueur, puis SKILL.
const int kProbedActions[4] = {5, 10, 11, 12};

// Un `.act` range `action × 8 + direction`.
constexpr int kDirCount = 8;

// La durée d'un cycle d'animation, en millisecondes, telle que le client la
// jouera : images × délai par image × 24 ms.
float CycleMs(int frames, float frame_delay) {
  if (frames <= 0 || frame_delay <= 0.0f) return 0.0f;
  return static_cast<float>(frames) * frame_delay * kAnimTickMs;
}

}  // namespace

// ── Le quadtree de PICKING ───────────────────────────────────────────────────
//
// `TileQuadTree_QueryPoint` (0x00a797b0) est la source du survol natif ; sa
// branche « feuille » donne la disposition d'un quad et la règle de départage :
//
//   q[0] x0 · q[1] y0 · q[3] x1 · q[4] y1   (rectangle ÉCRAN, en pixels)
//   q[2] + q[5]                             (profondeur ; le PLUS GRAND gagne)
//   q[6] AID · q[7] job/classe · q[8] catégorie (0 acteur, 1 NPC, 2 unité…)
//
// Un nœud interne porte ses quatre enfants aux index 5..8 ; une feuille porte à
// l'index 9 la sentinelle d'une liste chaînée dont chaque maillon a le quad en
// [2]. Le nœud racine est l'OBJET à cette adresse, pas un pointeur vers lui —
// même forme que g_UIWindowMgr.

// 🔴 `g_PickQuadMinSizePx` : la taille MINIMALE d'une zone cliquable, en pixels
// (`échelle de scène × 40`, calculée une seule fois au premier quad). Tout quad
// plus petit est élargi symétriquement jusque-là par
// `CActorSprite_SubmitNameplateQuad` — c'est la raison pour laquelle deux petites
// entités voisines se disputent le même clic.
//
// Deux lectures dans TOUT le binaire, toutes deux dans cette construction :
// l'écrire ne touche donc ni aux plaques de nom, ni au rendu, ni au reste.

constexpr int kMaxPickBoxes = 512;   // plafond de sûreté, pas une limite du jeu
constexpr int kMaxPickDepth = 16;

struct PickBox {
  float        x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
  unsigned     aid = 0;
  int          job = 0;
  int          cat = 0;
  const float* quad = nullptr;  // pour reconnaître celui que le survol désigne
};

// Récursion PURE (aucun objet C++ à dérouler) : l'appelant peut donc l'enfermer
// dans un __try, ce que MSVC refuserait si un destructeur traînait ici.
void CollectQuadsRec(const void* node, PickBox* out, int* count, int depth) {
  if (!node || depth > kMaxPickDepth || *count >= kMaxPickBoxes) return;
  const void* const* n = reinterpret_cast<const void* const*>(node);
  if (n[5] != nullptr) {  // nœud interne : quatre enfants
    for (int i = 0; i < 4; ++i) CollectQuadsRec(n[5 + i], out, count, depth + 1);
    return;
  }
  const void* const* list = reinterpret_cast<const void* const*>(n[9]);
  if (!list) return;
  const void* const* it = reinterpret_cast<const void* const*>(list[0]);
  int guard = 0;
  while (it && it != list && guard++ < kMaxPickBoxes) {
    const float* q = reinterpret_cast<const float*>(it[2]);
    if (q && *count < kMaxPickBoxes) {
      PickBox& b = out[(*count)++];
      const int* qi = reinterpret_cast<const int*>(q);
      b.x0 = q[0]; b.y0 = q[1]; b.x1 = q[3]; b.y1 = q[4];
      b.aid = static_cast<unsigned>(qi[6]);
      b.job = qi[7];
      b.cat = qi[8];
      b.quad = q;
    }
    it = reinterpret_cast<const void* const*>(it[0]);
  }
}

int CollectPickBoxes(PickBox* out) {
  int count = 0;
  __try {
    CollectQuadsRec(reinterpret_cast<const void*>(gamescene::kPickQuadTreeAddr), out, &count, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return count;
}

// Le quad que le natif désignerait sous le curseur — donc la cible qu'un clic
// prendrait à cet instant.
const float* HoveredQuad(float* out_mx, float* out_my) {
  __try {
    const float mx = static_cast<float>(*reinterpret_cast<int*>(rag::kMouseScreenXAddr));
    const float my = static_cast<float>(*reinterpret_cast<int*>(rag::kMouseScreenYAddr));
    if (out_mx) *out_mx = mx;
    if (out_my) *out_my = my;
    using QueryPoint_t = float*(__thiscall*)(void*, float, float);
    return reinterpret_cast<QueryPoint_t>(gamescene::kQuadTreeQueryPointAddr)(
        reinterpret_cast<void*>(gamescene::kPickQuadTreeAddr), mx, my);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Une couleur par catégorie de pick : ce qui compte est de distinguer d'un coup
// d'œil un acteur d'un objet au sol ou d'une unité de compétence posée.
//
// 🔴 Ce `switch` écrivait ses catégories EN LITTÉRAUX, avec l'étiquette FAUSSE
// (« NPC de carte » pour la 1). C'était le troisième porteur de la même carte
// périmée, et le seul qu'aucune recherche de constante ne pouvait trouver.
uint32_t PickCatColor(int cat) {
  switch (cat) {
    case gamescene::kPickActor:      return IM_COL32(90, 210, 120, 200);
    case gamescene::kPickGroundItem: return IM_COL32(110, 170, 255, 200);
    case gamescene::kPickSkillUnit:  return IM_COL32(255, 170, 70, 200);
    default:                         return IM_COL32(180, 180, 180, 170);
  }
}

// ── Compteurs d'ENVOIS ───────────────────────────────────────────────────────
//
// Alimentés depuis la couche réseau (`RagConnection::SendPacketHook`), lus par le
// rendu. Deux fils possibles, donc `std::atomic` — et un compteur atomique ne
// coûte rien quand personne ne regarde.
std::atomic<uint32_t> g_sent_skill{0};    // CZ_USE_SKILL + CZ_USE_SKILL_TOGROUND
std::atomic<uint32_t> g_sent_attack{0};   // CZ_REQUEST_ACT
std::atomic<uint32_t> g_sent_since{0};    // timeGetTime du dernier remise à zéro

// ── Cycle de vie ─────────────────────────────────────────────────────────────

void CharDiagnostics::NoteSend(uint16_t opcode) {
  switch (opcode) {
    case char_diag::kCzUseSkill:
    case char_diag::kCzUseSkillGround:
      g_sent_skill.fetch_add(1, std::memory_order_relaxed);
      break;
    case char_diag::kCzRequestAct:
      g_sent_attack.fetch_add(1, std::memory_order_relaxed);
      break;
    default:
      break;
  }
}

CharDiagnostics::CharDiagnostics() {
  // Deux paquets OBSERVÉS, jamais revendiqués : le client continue de les
  // traiter normalement, on ne fait que lire au passage. C'est le seul endroit
  // où amotion et dmotion voyagent coup par coup.
  auto& b = Bourgeon::Instance();
  b.RegisterObserveOpcode(kOpNotifyAct, sizeof(NotifyActPayload));
  b.RegisterObserveOpcode(kOpNotifySkill, sizeof(NotifySkillPayload));
}

void CharDiagnostics::Open() {
  open_ = true;
  need_focus_ = true;
}

void CharDiagnostics::Toggle() {
  if (open_) open_ = false; else Open();
}

void CharDiagnostics::OnModeSwitch(ModeMgr::ModeType, const char*) {
  // Les derniers coups appartenaient à la carte qu'on quitte : les garder
  // afficherait un dmotion vieux d'un warp comme s'il décrivait maintenant.
  dealt_ = Blow();
  taken_ = Blow();
  ResetMeasures();
  net_inbox_.Clear();
}

void CharDiagnostics::ResetMeasures() {
  g_sent_skill.store(0, std::memory_order_relaxed);
  g_sent_attack.store(0, std::memory_order_relaxed);
  g_sent_since.store(timeGetTime(), std::memory_order_relaxed);
  for (int i = 0; i < kStateBuckets; ++i) state_hist_[i] = 0;
  state_samples_  = 0;
  state_blocked_  = 0;
  state_since_ms_ = timeGetTime();
  hit_ring_count_ = 0;
  hit_ring_head_  = 0;
}

void CharDiagnostics::PushHitTime(uint32_t tick) {
  hit_ring_[hit_ring_head_] = tick;
  hit_ring_head_ = (hit_ring_head_ + 1) % kRateRing;
  if (hit_ring_count_ < kRateRing) ++hit_ring_count_;
}

// ── Paquets ──────────────────────────────────────────────────────────────────

void CharDiagnostics::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                   uint16_t len) {
  net_inbox_.Push(opcode, data, len);  // fil RÉSEAU : rien d'autre
}

void CharDiagnostics::HandlePacket(uint16_t opcode, const uint8_t* data,
                                   uint16_t len) {
  const uint32_t me = static_cast<uint32_t>(ReadInt(rag::kOwnAccountIdAddr));
  if (me == 0 || data == nullptr) return;

  Blow blow;
  blow.tick = timeGetTime();

  if (opcode == kOpNotifyAct) {
    if (len < sizeof(NotifyActPayload)) return;
    NotifyActPayload p;
    std::memcpy(&p, data, sizeof(p));
    blow.src_speed = p.src_speed;
    blow.dst_speed = p.dst_speed;
    blow.damage    = p.damage;
    blow.div       = p.div;
    blow.type      = p.type;
    blow.seen      = true;
    if (static_cast<uint32_t>(p.src_id) == me) {
      blow.other_gid = static_cast<uint32_t>(p.dst_id);
      dealt_ = blow;
      PushHitTime(blow.tick);
    } else if (static_cast<uint32_t>(p.dst_id) == me) {
      blow.other_gid = static_cast<uint32_t>(p.src_id);
      taken_ = blow;
    }
    return;
  }

  if (opcode == kOpNotifySkill) {
    if (len < sizeof(NotifySkillPayload)) return;
    NotifySkillPayload p;
    std::memcpy(&p, data, sizeof(p));
    blow.src_speed = p.attack_mt;
    blow.dst_speed = p.attacked_mt;
    blow.damage    = p.damage;
    blow.div       = p.count;
    blow.type      = p.action;
    blow.skill_id  = p.skill_id;
    blow.seen      = true;
    if (p.src_id == me) {
      blow.other_gid = p.dst_id;
      dealt_ = blow;
      PushHitTime(blow.tick);
    } else if (p.dst_id == me) {
      blow.other_gid = p.src_id;
      taken_ = blow;
    }
  }
}

// ── Lecture : les globales de session ────────────────────────────────────────

void CharDiagnostics::Refresh(Snapshot* out) {
  out->aid = static_cast<uint32_t>(ReadInt(rag::kOwnAccountIdAddr));
  out->cid = static_cast<uint32_t>(ReadInt(rag::kOwnCharIdAddr));
  __try {
    lstrcpynA(out->char_name, reinterpret_cast<const char*>(rag::kOwnCharNameAddr),
              static_cast<int>(sizeof(out->char_name)));
  } __except (EXCEPTION_EXECUTE_HANDLER) { out->char_name[0] = '\0'; }

  out->job_real     = ReadInt(rag::kOwnJobIdAddr);
  JobDisplayName(out->job_real, out->job_real_name, sizeof(out->job_real_name));
  out->base_level   = rag::BaseLevel();
  out->job_level    = rag::JobLevel();
  out->status_point = ReadInt(rag::kOwnStatusPointsAddr);
  out->skill_point  = ReadInt(rag::kOwnSkillPointsAddr);
  out->zeny         = rag::Zeny();
  out->manner       = ReadInt(rag::kOwnMannerAddr);
  out->weight       = ReadInt(rag::kWeightCurAddr);
  out->weight_max   = ReadInt(rag::kWeightMaxAddr);
  out->hp           = ReadInt(rag::kOwnHpAddr);
  out->hp_max       = ReadInt(rag::kOwnMaxHpAddr);
  out->sp           = ReadInt(rag::kOwnSpAddr);
  out->sp_max       = ReadInt(rag::kOwnMaxSpAddr);
  out->base_exp      = ReadInt64(rag::kOwnBaseExpAddr);
  out->base_exp_next = ReadInt64(rag::kOwnBaseExpNextAddr);
  out->job_exp       = ReadInt64(rag::kOwnJobExpAddr);
  out->job_exp_next  = ReadInt64(rag::kOwnJobExpNextAddr);
  out->body_state    = ReadInt(kBodyState);
  out->health_state  = ReadInt(kHealthState);
  out->effect_state  = ReadInt(kEffectState);

  for (int i = 0; i < 6; ++i) {
    out->stat_base[i]  = ReadInt(rag::kStatBaseAddr + i * 4);
    out->stat_bonus[i] = ReadInt(rag::kStatBonusAddr + i * 4);
    out->stat_cost[i]  = ReadInt(rag::kStatRaiseCostAddr + i * 4);
  }

  out->atk1      = ReadInt(rag::kOwnAtk1Addr);
  out->atk2      = ReadInt(rag::kOwnAtk2Addr);
  out->matk_min  = ReadInt(rag::kOwnMatkMinAddr);
  out->matk_max  = ReadInt(rag::kOwnMatkMaxAddr);
  out->def_soft  = ReadInt(rag::kOwnDefSoftAddr);
  out->def_hard  = ReadInt(rag::kOwnDefHardAddr);
  out->mdef_soft = ReadInt(rag::kOwnMdefSoftAddr);
  out->mdef_hard = ReadInt(rag::kOwnMdefHardAddr);
  out->hit       = ReadInt(rag::kOwnHitAddr);
  out->flee      = ReadInt(rag::kOwnFleeAddr);
  out->pdodge    = ReadInt(rag::kOwnPerfectDodgeAddr);
  out->crit      = ReadInt(rag::kOwnCritAddr);

  out->amotion    = ReadInt(rag::kOwnAttackDelayAddr);
  out->walk_speed = ReadInt(rag::kOwnWalkSpeedAddr);
  out->motion_cap = ReadInt(kMotionSpeedCap);
}

// ── Lecture : l'acteur et son `.act` ─────────────────────────────────────────

void CharDiagnostics::ReadActor(Snapshot* out) {
  void* actor = OwnActor();
  if (!actor) return;

  out->actor_found = true;
  out->actor_addr  = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(actor));
  out->actor_gid   = ReadField<uint32_t>(actor, rag::actor::kGid);
  out->job_shown   = ReadField<int>(actor, rag::actor::kJobId);
  out->sex         = ReadField<int>(actor, rag::actor::kSex);
  out->motion_state  = ReadField<int>(actor, rag::actor::kMotionState);
  out->action_base   = ReadField<int>(actor, rag::actor::kActionBase);
  out->action_played = ReadField<int>(actor, rag::actor::kActionPlay);
  out->frame_index   = ReadField<int>(actor, rag::actor::kFrameIndex);
  out->frame_delay   = ReadField<float>(actor, rag::actor::kFrameDelay);
  out->motion_factor = ReadField<float>(actor, rag::actor::kMotionFactor);
  out->anim_start    = ReadField<uint32_t>(actor, rag::actor::kAnimStart);
  out->weapon_view   = ReadField<int>(actor, rag::actor::kWeaponView);
  out->shield_view   = ReadField<int>(actor, rag::actor::kShieldView);
  out->pos_x  = ReadField<float>(actor, rag::actor::kPosX);
  out->pos_y  = ReadField<float>(actor, rag::actor::kPosY);
  out->pos_z  = ReadField<float>(actor, rag::actor::kPosZ);
  out->facing = ReadField<float>(actor, rag::actor::kFacing);

  const uint32_t now = timeGetTime();
  out->anim_age = (out->anim_start != 0 && now >= out->anim_start)
                      ? (now - out->anim_start)
                      : 0;

  JobDisplayName(out->job_shown, out->job_shown_name,
                 sizeof(out->job_shown_name));
  out->job_resolved = ResolveDisplayJob(actor);
  JobDisplayName(out->job_resolved, out->job_resolved_name,
                 sizeof(out->job_resolved_name));

  // 🔴 LA ressource `.act` du corps : `Act_GetFrameCount` et `Act_GetFrameDelay`
  // la lisent à +0x108 sur l'acteur, et c'est donc ELLE que l'animation joue —
  // pas celle qu'on déduirait d'un identifiant de classe. Sous déguisement, elle
  // pointe le fichier du monstre.
  void* act = ReadField<void*>(actor, rag::actor::kActRes);
  void* spr = ReadField<void*>(actor, rag::actor::kSprRes);
  ResPath(spr, out->spr_path, sizeof(out->spr_path));
  ResPath(act, out->act_path, sizeof(out->act_path));
  if (!act) return;

  out->act_found        = true;
  out->act_action_count = ActActionCount(act);

  // 🔴 L'ENTRÉE jouée est `acteur+0x38` TELLE QUELLE — c'est ce que le client
  // passe à `Act_GetFrameCount`. La diviser par 8 pour « retrouver l'action »
  // désigne une autre entrée du fichier, et rend un nombre d'images qui a l'air
  // juste sans l'être.
  out->played_index  = out->action_played;
  out->played_frames = ActFrameCount(act, out->played_index);
  out->played_speed  = ActSpeed(act, out->played_index);

  // Sonde 0 = l'action en cours, puis les quatre actions de combat. Chacune sur
  // ses huit directions : c'est la comparaison entre elles qui révèle une
  // animation dont la durée dépend de l'orientation.
  const int actions[5] = {out->action_played / kDirCount, kProbedActions[0],
                          kProbedActions[1], kProbedActions[2],
                          kProbedActions[3]};
  for (int p = 0; p < 5; ++p) {
    Snapshot::ActProbe& probe = out->probes[p];
    probe.action = actions[p];
    probe.speed  = ActSpeed(act, actions[p] * kDirCount);
    for (int d = 0; d < kDirCount; ++d)
      probe.frames[d] = ActFrameCount(act, actions[p] * kDirCount + d);
  }
}

// ── Cadence observée ─────────────────────────────────────────────────────────

float CharDiagnostics::ObservedRate(int* out_samples, float* out_window_s) const {
  if (out_samples) *out_samples = 0;
  if (out_window_s) *out_window_s = 0.0f;
  if (hit_ring_count_ < 2) return 0.0f;

  // Les coups plus vieux que cette fenêtre ne décrivent plus ce qui se passe :
  // une moyenne qui les garde lisse justement le changement qu'on cherche à voir.
  constexpr uint32_t kWindowMs = 6000;
  const uint32_t now = timeGetTime();

  uint32_t newest = 0, oldest = 0;
  int samples = 0;
  for (int i = 0; i < hit_ring_count_; ++i) {
    const int idx = (hit_ring_head_ - 1 - i + kRateRing * 2) % kRateRing;
    const uint32_t t = hit_ring_[idx];
    if (now - t > kWindowMs) break;
    if (samples == 0) newest = t;
    oldest = t;
    ++samples;
  }
  if (samples < 2 || newest <= oldest) return 0.0f;

  const float span_s = static_cast<float>(newest - oldest) / 1000.0f;
  if (out_samples) *out_samples = samples;
  if (out_window_s) *out_window_s = span_s;
  // `samples - 1` intervalles entre `samples` coups : compter les coups
  // donnerait une cadence gonflée d'un cran sur les petites séries.
  return static_cast<float>(samples - 1) / span_s;
}

// ── Rendu ────────────────────────────────────────────────────────────────────

void CharDiagnostics::DrawBlow(const char* title, const Blow& blow,
                               bool incoming) {
  SeparatorText(title);
  if (!blow.seen) {
    ImGui::TextColored(ro::pal::kWarn, "%s",
                       i18n::Tr("Rien depuis l'ouverture de la session."));
    return;
  }

  const uint32_t now = timeGetTime();
  const uint32_t age = (now >= blow.tick) ? (now - blow.tick) : 0;
  RowFmt(i18n::Tr("Vu il y a"), "%.1f s", age / 1000.0f);

  if (blow.skill_id != 0)
    RowFmt(i18n::Tr("Compétence"), "#%d", blow.skill_id);

  const char* type_name = BlowTypeName(blow.type);
  if (type_name)
    RowFmt(i18n::Tr("Genre"), "%d — %s", blow.type, type_name);
  else
    RowFmt(i18n::Tr("Genre"), "%d", blow.type);

  RowFmt(i18n::Tr("Dégâts"), "%d  (× %d)", blow.damage, blow.div);
  RowFmt(i18n::Tr("GID en face"), "%u", blow.other_gid);

  // 🔴 C'est ici que se lit la différence entre les deux temps. Sur un coup que
  // je porte, `src_speed` est MON amotion ; sur un coup que j'encaisse, c'est
  // celui de l'attaquant, et `dst_speed` devient MON dmotion.
  if (incoming) {
    RowFmt(i18n::Tr("amotion de l'attaquant"), "%d ms", blow.src_speed);
    RowFmt(i18n::Tr("dmotion — MON recul"), "%d ms", blow.dst_speed);
  } else {
    RowFmt(i18n::Tr("amotion — MON délai"), "%d ms", blow.src_speed);
    RowFmt(i18n::Tr("dmotion imposé à la cible"), "%d ms", blow.dst_speed);
  }
}

// Une action, sur ses huit directions. C'est LA ligne qui répond à « pourquoi la
// cadence change quand je tourne la caméra » : la direction est choisie par
// l'angle de vue, et chaque direction est une entrée distincte du `.act`.
void CharDiagnostics::DrawProbe(const Snapshot& s, const Snapshot::ActProbe& p) {
  if (p.action < 0) return;

  const char* name = ActionName(p.action);
  char label[56];
  if (name)
    _snprintf_s(label, sizeof(label), _TRUNCATE, "%s (%d)", name, p.action);
  else
    _snprintf_s(label, sizeof(label), _TRUNCATE, i18n::Tr("action %d"), p.action);

  // Les huit nombres d'images, plus le repérage de ce qui cloche : une direction
  // absente du fichier, ou des directions qui ne durent pas la même chose.
  int  present = 0, min_frames = 0, max_frames = 0;
  char frames[80] = {};
  int  written = 0;
  for (int d = 0; d < kDirCount; ++d) {
    const int f = p.frames[d];
    if (f > 0) {
      if (present == 0) { min_frames = f; max_frames = f; }
      if (f < min_frames) min_frames = f;
      if (f > max_frames) max_frames = f;
      ++present;
    }
    written += _snprintf_s(frames + written, sizeof(frames) - written, _TRUNCATE,
                           (d == 0) ? "%d" : " %d", f);
  }

  if (present == 0) {
    Row(label, i18n::Tr("absente de ce fichier"), /*warn=*/true);
    return;
  }

  // La durée est calculée au facteur de motion COURANT : c'est la durée que
  // l'animation aurait MAINTENANT, comparable telle quelle à l'amotion.
  const float delay  = p.speed * s.motion_factor;
  const float ms_min = CycleMs(min_frames, delay);
  const float ms_max = CycleMs(max_frames, delay);
  const bool  uneven = (min_frames != max_frames);

  char value[240];
  if (uneven) {
    _snprintf_s(value, sizeof(value), _TRUNCATE,
                i18n::Tr("images %s  × %.2f → de %.0f à %.0f ms selon la "
                         "DIRECTION"),
                frames, p.speed, ms_min, ms_max);
  } else {
    _snprintf_s(value, sizeof(value), _TRUNCATE,
                i18n::Tr("images %s  × %.2f → %.0f ms"), frames, p.speed,
                ms_min);
  }
  // Deux raisons de signaler : des directions inégales (la cadence dépendra de
  // l'angle de vue), ou une animation plus longue que le délai d'attaque.
  const bool warn =
      uneven || (s.amotion > 0 && ms_max > static_cast<float>(s.amotion) * 1.05f);
  Row(label, value, warn);
  if (uneven) {
    ImGui::SameLine();
    HelpMarker(i18n::Tr(
        "Les huit directions de cette action n'ont pas le même nombre "
        "d'images. Comme la direction jouée est choisie par l'angle de la "
        "CAMÉRA, l'animation dure plus ou moins longtemps selon la façon dont "
        "on regarde le personnage — et comme l'état d'attaque bloque l'envoi "
        "des compétences, la cadence des skills change avec la caméra.\n\n"
        "C'est un défaut du fichier de sprite, pas du serveur."));
  }
}

void CharDiagnostics::DrawBody(const Snapshot& s) {
  // ── Personnage ────────────────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Personnage"));
  {
    const char* utf8 = ro::WireToUtf8(s.char_name);
    Row(i18n::Tr("Nom"), (utf8 && *utf8) ? utf8 : s.char_name);
  }
  RowFmt(i18n::Tr("AID / CID"), "%u / %u", s.aid, s.cid);
  if (s.job_real_name[0]) {
    const char* utf8 = ro::LocalToUtf8(s.job_real_name);
    RowFmt(i18n::Tr("Classe (serveur)"), "%d — %s", s.job_real,
           (utf8 && *utf8) ? utf8 : s.job_real_name);
  } else {
    RowFmt(i18n::Tr("Classe (serveur)"), "%d", s.job_real);
  }
  RowFmt(i18n::Tr("Niveaux"), i18n::Tr("base %d / job %d"), s.base_level,
         s.job_level);
  RowFmt(i18n::Tr("Points"), i18n::Tr("%d de stat / %d de compétence"),
         s.status_point, s.skill_point);
  RowFmt("HP / SP", "%d / %d      %d / %d", s.hp, s.hp_max, s.sp, s.sp_max);
  RowFmt(i18n::Tr("Poids"), "%d / %d  (%d %%)", s.weight, s.weight_max,
         s.weight_max > 0 ? (s.weight * 100 / s.weight_max) : 0);
  RowFmt(i18n::Tr("Zeny"), "%d", s.zeny);
  RowFmt(i18n::Tr("Expérience"), i18n::Tr("base %llu / %llu — job %llu / %llu"),
         s.base_exp, s.base_exp_next, s.job_exp, s.job_exp_next);
  RowFmt(i18n::Tr("Manner"), "%d", s.manner);
  // opt1 / opt2 / option : les trois masques d'état que le serveur envoie et que
  // le client garde tels quels. Ce sont eux qui décident, entre autres, du
  // sprite affiché (monture, mado, invisibilité).
  RowFmt(i18n::Tr("États (opt1/opt2/option)"), "%d / %d / 0x%08X",
         s.body_state, s.health_state,
         static_cast<unsigned>(s.effect_state));

  // ── Stats primaires ───────────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Stats primaires (base + bonus)"));
  for (int i = 0; i < 6; ++i) {
    char value[96];
    _snprintf_s(value, sizeof(value), _TRUNCATE,
                i18n::Tr("%d + %d = %d      (montée : %d point(s))"),
                s.stat_base[i], s.stat_bonus[i],
                s.stat_base[i] + s.stat_bonus[i], s.stat_cost[i]);
    Row(kStatName[i], value);
  }

  // ── Stats dérivées ────────────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Stats dérivées"));
  RowFmt("ATK",  "%d + %d", s.atk1, s.atk2);
  RowFmt("MATK", "%d ~ %d", s.matk_min, s.matk_max);
  RowFmt("DEF",  "%d + %d", s.def_soft, s.def_hard);
  RowFmt("MDEF", "%d + %d", s.mdef_soft, s.mdef_hard);
  RowFmt("HIT",  "%d", s.hit);
  RowFmt("FLEE", "%d + %d", s.flee, s.pdodge);
  RowFmt("CRIT", "%d", s.crit);

  // ── Le cœur : les temps ───────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Temps de combat (ce que le client applique)"));
  if (s.amotion > 0) {
    RowFmt(i18n::Tr("amotion"), i18n::Tr("%d ms — soit %.2f coup(s)/s"),
           s.amotion, 1000.0f / static_cast<float>(s.amotion));
    RowFmt("ASPD", i18n::Tr("%d — c'est (2000 - amotion) / 10, rien de plus"),
           rag::AspdFromAmotion(s.amotion));
  } else {
    Row(i18n::Tr("amotion"), i18n::Tr("0 — pas encore reçu du serveur"), true);
  }
  if (s.walk_speed > 0) {
    RowFmt(i18n::Tr("Vitesse de marche"),
           i18n::Tr("%d ms par cellule — soit %.2f cellule(s)/s"), s.walk_speed,
           1000.0f / static_cast<float>(s.walk_speed));
  }
  // 🔴 LA MESURE, en face de la théorie. Elle vient des paquets de coups, donc
  // de la même horloge que le journal de combat : c'est ce chiffre-là qu'on
  // compare quand on soupçonne le client de brider quelque chose.
  {
    int samples = 0;
    float window_s = 0.0f;
    const float rate = ObservedRate(&samples, &window_s);
    if (rate > 0.0f) {
      char value[200];
      _snprintf_s(value, sizeof(value), _TRUNCATE,
                  i18n::Tr("%.2f coup(s)/s — mesuré sur %d coups en %.1f s"),
                  rate, samples, window_s);
      // L'écart avec la cadence théorique est le signal : sous la moitié de ce
      // que l'amotion autorise, quelque chose bride en amont.
      const bool warn = (s.amotion > 0 &&
                         rate < (1000.0f / static_cast<float>(s.amotion)) * 0.5f);
      Row(i18n::Tr("Cadence observée"), value, warn);
    } else {
      Row(i18n::Tr("Cadence observée"),
          i18n::Tr("— (pas assez de coups récents)"));
    }
    ImGui::SameLine();
    HelpMarker(i18n::Tr(
        "Les coups que VOUS portez, comptés sur les paquets reçus et non sur "
        "les images affichées : le chiffre est donc comparable à ce que montre "
        "le journal de combat, et indépendant du nombre d'images par "
        "seconde.\n\n"
        "Un coup multiple compte pour UN : ce qui est mesuré est le rythme des "
        "envois, pas le nombre de lignes de dégâts."));
  }

  // 🔴 CE QUI PART vs CE QUI REVIENT. Le premier chiffre est ce que le client a
  // VRAIMENT émis, compté sur le hook d'envoi ; le second, plus haut, ce que le
  // serveur a répondu. L'écart entre les deux dit de quel côté est le frein, et
  // c'est le seul relevé qui le dise sans supposition.
  {
    const uint32_t sent_skill  = g_sent_skill.load(std::memory_order_relaxed);
    const uint32_t sent_attack = g_sent_attack.load(std::memory_order_relaxed);
    const uint32_t since       = g_sent_since.load(std::memory_order_relaxed);
    const uint32_t now         = timeGetTime();
    const float span_s = (since != 0 && now > since)
                             ? static_cast<float>(now - since) / 1000.0f
                             : 0.0f;
    char value[220];
    if (span_s > 0.2f) {
      _snprintf_s(value, sizeof(value), _TRUNCATE,
                  i18n::Tr("%u skill(s) + %u attaque(s) — soit %.2f et %.2f "
                           "par seconde"),
                  sent_skill, sent_attack,
                  static_cast<float>(sent_skill) / span_s,
                  static_cast<float>(sent_attack) / span_s);
    } else {
      _snprintf_s(value, sizeof(value), _TRUNCATE, "%u + %u", sent_skill,
                  sent_attack);
    }
    Row(i18n::Tr("Paquets ÉMIS"), value);
    ImGui::SameLine();
    HelpMarker(i18n::Tr(
        "CZ_USE_SKILL (+ variante au sol) et CZ_REQUEST_ACT réellement partis "
        "du client, comptés sur le hook d'envoi — donc avant toute réponse du "
        "serveur.\n\n"
        "· BEAUCOUP d'émis pour PEU de coups reçus : le client fait son "
        "travail, c'est le serveur qui refuse (delay d'action, portée, "
        "cible) ;\n"
        "· PEU d'émis : rien ne part, et le frein est en amont — saisie, "
        "ciblage, ou un garde du client.\n\n"
        "Remis à zéro par le bouton du bas, en même temps que les autres "
        "mesures."));
  }

  // 🔴 LA MESURE QUI DÉSIGNE LE COUPABLE. Si le personnage passe l'essentiel de
  // son temps dans un état qui bloque l'émission, le frein est le CLIENT et son
  // animation. Sinon, la requête pouvait partir : le frein est ailleurs (refus
  // du serveur, portée, ciblage).
  if (state_samples_ > 0) {
    const uint32_t now = timeGetTime();
    const float span_s =
        (state_since_ms_ != 0 && now > state_since_ms_)
            ? static_cast<float>(now - state_since_ms_) / 1000.0f
            : 0.0f;
    const float pct =
        100.0f * static_cast<float>(state_blocked_) / static_cast<float>(state_samples_);
    char value[200];
    _snprintf_s(value, sizeof(value), _TRUNCATE,
                i18n::Tr("%.0f %% du temps — sur %u images en %.1f s"), pct,
                state_samples_, span_s);
    Row(i18n::Tr("États bloquants"), value, pct >= 50.0f);
    ImGui::SameLine();
    HelpMarker(i18n::Tr(
        "Part du temps passée dans un état de motion qui EMPÊCHE le client "
        "d'émettre la compétence en attente (tout sauf 0, 1, 4 et ≥ 16).\n\n"
        "Élevé : c'est le client qui bride, et la cause est la durée de "
        "l'animation.\n"
        "Bas alors que les compétences partent mal quand même : la requête "
        "POUVAIT partir — le frein est ailleurs (refus du serveur, portée, "
        "ciblage).\n\n"
        "Échantillonné une fois par image, tant que cette fenêtre est ouverte. "
        "Le bouton « Remettre les mesures à zéro » redémarre le comptage : à "
        "faire juste avant chaque essai."));

    // Les états dominants, pour savoir DANS QUOI le temps passe.
    int top[3] = {-1, -1, -1};
    for (int b = 0; b < kStateBuckets; ++b) {
      for (int k = 0; k < 3; ++k) {
        if (top[k] < 0 || state_hist_[b] > state_hist_[top[k]]) {
          for (int m = 2; m > k; --m) top[m] = top[m - 1];
          top[k] = b;
          break;
        }
      }
    }
    char line[240];
    int written = 0;
    for (int k = 0; k < 3; ++k) {
      if (top[k] < 0 || state_hist_[top[k]] == 0) continue;
      const char* name = MotionStateName(top[k]);
      written += _snprintf_s(line + written, sizeof(line) - written, _TRUNCATE,
                             "%s%d%s%s %.0f %%", (written == 0) ? "" : "   ",
                             top[k], name ? " " : "", name ? name : "",
                             100.0f * static_cast<float>(state_hist_[top[k]]) /
                                 static_cast<float>(state_samples_));
    }
    if (written > 0) Row(i18n::Tr("États dominants"), line);
  }

  RowFmt(i18n::Tr("Plafond d'animation"),
         i18n::Tr("%d ms — au-delà, l'animation ne ralentit plus"),
         s.motion_cap);
  ImGui::SameLine();
  HelpMarker(i18n::Tr(
      "Le client cale la vitesse de l'animation d'attaque sur "
      "min(amotion, plafond) / 432. Un amotion PLUS GRAND que le plafond ne "
      "rallonge donc plus l'animation : elle se termine, puis le personnage "
      "attend en position de garde jusqu'au coup suivant."));

  DrawBlow(i18n::Tr("Dernier coup que j'ai porté"), dealt_, /*incoming=*/false);
  DrawBlow(i18n::Tr("Dernier coup que j'ai encaissé"), taken_, /*incoming=*/true);

  // ── Acteur ────────────────────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Acteur et animation"));
  if (!s.actor_found) {
    ImGui::TextColored(ro::pal::kWarn, "%s",
                       i18n::Tr("Aucun acteur : hors jeu, ou carte en cours de "
                                "chargement."));
    return;
  }

  RowFmt(i18n::Tr("Adresse"), "0x%08X", s.actor_addr);
  RowFmt(i18n::Tr("GID (+0x110)"), "%u", s.actor_gid);

  // 🔴 LE DÉGUISEMENT SE LIT ICI, et nulle part ailleurs : le serveur ne dit pas
  // « tu es déguisé », il envoie simplement une autre classe pour notre acteur.
  // L'écart entre les deux nombres EST le déguisement.
  {
    const bool disguised = (s.job_shown != s.job_real);
    char value[192];
    if (s.job_shown_name[0]) {
      const char* utf8 = ro::LocalToUtf8(s.job_shown_name);
      _snprintf_s(value, sizeof(value), _TRUNCATE, "%d — %s", s.job_shown,
                  (utf8 && *utf8) ? utf8 : s.job_shown_name);
    } else {
      _snprintf_s(value, sizeof(value), _TRUNCATE, "%d", s.job_shown);
    }
    Row(i18n::Tr("Classe AFFICHÉE (+0x25c)"), value, disguised);

    // La classe RÉSOLUE : +0x25c passé par les masques d'option. C'est elle qui
    // choisit le fichier de sprite, et une transformation qui passerait par une
    // option (monture, mado, poring) ne se verrait QUE sur cette ligne.
    char resolved[192];
    if (s.job_resolved_name[0]) {
      const char* utf8 = ro::LocalToUtf8(s.job_resolved_name);
      _snprintf_s(resolved, sizeof(resolved), _TRUNCATE, "%d — %s",
                  s.job_resolved, (utf8 && *utf8) ? utf8 : s.job_resolved_name);
    } else {
      _snprintf_s(resolved, sizeof(resolved), _TRUNCATE, "%d", s.job_resolved);
    }
    Row(i18n::Tr("Classe RÉSOLUE (sprite)"), resolved,
        s.job_resolved != s.job_real);
    ImGui::SameLine();
    HelpMarker(i18n::Tr(
        "Trois valeurs, trois sources qui ne mentent pas pareil :\n\n"
        "· Classe (serveur) = g_Own_JobId, ce que le serveur dit que vous "
        "ÊTES ;\n"
        "· Classe AFFICHÉE = acteur+0x25c, la classe portée par votre acteur — "
        "c'est elle que change un @disguise ;\n"
        "· Classe RÉSOLUE = la précédente passée par les masques d'option "
        "(monture, mado, invisibilité). C'est elle qui décide du fichier de "
        "sprite.\n\n"
        "Si les trois sont égales alors que le personnage n'a pas son "
        "apparence normale, la transformation ne passe par AUCUNE des trois : "
        "regarder alors le chemin du .act, plus bas — c'est la seule source "
        "qui ne puisse pas se tromper, puisque c'est le fichier réellement "
        "joué."));

    if (disguised || s.job_resolved != s.job_real) {
      ImGui::TextColored(
          ro::pal::kWarn, "%s",
          i18n::Tr("Déguisement actif : le sprite joué n'est pas celui de la "
                   "classe. Les durées d'animation ci-dessous sont celles du "
                   "sprite AFFICHÉ ; l'amotion, lui, reste celui du serveur."));
    }
  }

  RowFmt(i18n::Tr("Sexe"), "%d", s.sex);
  RowFmt(i18n::Tr("Vue arme / bouclier"), "%d / %d", s.weapon_view,
         s.shield_view);
  RowFmt(i18n::Tr("Position"), "%.2f, %.2f, %.2f   (cap %.1f°)", s.pos_x,
         s.pos_y, s.pos_z, s.facing);

  {
    // 🔴 L'état est montré AVEC son effet sur les compétences : c'est lui qui
    // décide si la requête de skill en attente peut partir cette frame.
    const char* state = MotionStateName(s.motion_state);
    const bool  blocks = MotionStateBlocksSkill(s.motion_state);
    char value[160];
    if (state)
      _snprintf_s(value, sizeof(value), _TRUNCATE, "%d — %s%s", s.motion_state,
                  state,
                  blocks ? i18n::Tr("   ⛔ bloque l'envoi des compétences") : "");
    else
      _snprintf_s(value, sizeof(value), _TRUNCATE,
                  i18n::Tr("%d — non étiqueté (pas de RE)%s"), s.motion_state,
                  blocks ? i18n::Tr("   ⛔ bloque l'envoi des compétences") : "");
    Row(i18n::Tr("État de motion (+0x70)"), value, blocks);
    ImGui::SameLine();
    HelpMarker(i18n::Tr(
        "Actor_ProcessPendingAction_Tick (0x00d43400) n'émet CZ_USE_SKILL que "
        "si cet état vaut 0, 1, 4 ou ≥ 16. Dans tout autre état — 2 et 9 en "
        "tête, les attaques — la requête reste en file et est réessayée à la "
        "frame suivante.\n\n"
        "Donc : plus l'animation d'attaque est longue, plus le personnage "
        "reste dans un état bloquant, et moins vite les compétences partent. "
        "La durée de cette animation se lit plus bas, direction par "
        "direction."));
  }

  const int action = s.action_played / 8;
  const int dir    = s.action_played % 8;
  {
    // Le nom d'action n'existe que pour le jeu de sprites JOUEUR : deux formats
    // séparés plutôt qu'un `%s` nourri d'un pointeur nul selon les cas.
    const char* name = ActionName(action);
    if (name)
      RowFmt(i18n::Tr("Action jouée (+0x38)"),
             i18n::Tr("%d = action %d (%s) + direction %d"), s.action_played,
             action, name, dir);
    else
      RowFmt(i18n::Tr("Action jouée (+0x38)"),
             i18n::Tr("%d = action %d + direction %d"), s.action_played, action,
             dir);
  }
  if (s.played_frames > 0)
    RowFmt(i18n::Tr("Image courante (+0x3c)"), "%d / %d", s.frame_index,
           s.played_frames);
  else
    RowFmt(i18n::Tr("Image courante (+0x3c)"), "%d", s.frame_index);
  RowFmt(i18n::Tr("Depuis (+0x8c)"), i18n::Tr("%u ms"), s.anim_age);

  // Le trio qui EST la cadence : facteur de motion, délai par image, durée.
  RowFmt(i18n::Tr("Facteur de motion (+0x64)"),
         i18n::Tr("%.4f — soit un amotion de %.0f ms"), s.motion_factor,
         s.motion_factor * kMotionReferenceMs);
  RowFmt(i18n::Tr("Délai par image (+0x58)"), i18n::Tr("%.4f — soit %.1f ms"),
         s.frame_delay, s.frame_delay * kAnimTickMs);

  const float cycle = CycleMs(s.played_frames, s.frame_delay);
  if (cycle > 0.0f)
    RowFmt(i18n::Tr("Durée du cycle joué"), i18n::Tr("%.0f ms"), cycle);

  // ── Le sprite joué ────────────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Sprite joué (.act du corps)"));
  if (!s.act_found) {
    ImGui::TextColored(ro::pal::kWarn, "%s",
                       i18n::Tr("Aucune ressource .act sur l'acteur."));
  } else {
    // 🔴 Les chemins du VFS sont en CP949 par CONSTRUCTION (les dossiers du GRF
    // sont coréens) : c'est le cas que `Cp949ToUtf8` sert, et pas `LocalToUtf8`,
    // qui suivrait la code-page du client (1252 en Europe) et rendrait les
    // dossiers illisibles.
    //
    // ⚠ Les glyphes coréens ne s'affichent que si l'option « Glyphes coréens »
    // est allumée (juste à côté, dans Staff Tools) — sinon des carrés, ce qui
    // suffit déjà à voir que le fichier a CHANGÉ.
    Row(".spr", s.spr_path[0] ? ro::Cp949ToUtf8(s.spr_path) : "—");
    Row(".act", s.act_path[0] ? ro::Cp949ToUtf8(s.act_path) : "—");

    // 🔴 La MÊME chose, lue par l'autre chemin : le slot 0 du composite. Les
    // deux doivent désigner le même fichier — l'animation compte ses images sur
    // la ressource +0x108, mais c'est le slot 0 que le rendu dessine. S'ils
    // divergent, le personnage est animé sur un sprite et dessiné sur un autre,
    // et c'est une piste en soi.
    char slot0[264] = {};
    if (rag::ActorSlotSpritePath(reinterpret_cast<void*>(
                                     static_cast<uintptr_t>(s.actor_addr)),
                                 0, slot0, sizeof(slot0)) &&
        slot0[0]) {
      const bool same = (std::strcmp(slot0, s.spr_path) == 0);
      Row(i18n::Tr("slot 0 (corps dessiné)"), ro::Cp949ToUtf8(slot0), !same);
      if (!same)
        ImGui::TextColored(ro::pal::kWarn, "%s",
                           i18n::Tr("Le corps DESSINÉ n'est pas celui sur "
                                    "lequel l'animation compte ses images."));
    }
    RowFmt(i18n::Tr("Entrées du fichier"),
           i18n::Tr("%d  (= %d action(s) × 8 directions)"), s.act_action_count,
           s.act_action_count / kDirCount);
    ImGui::SameLine();
    HelpMarker(i18n::Tr(
        "Un .act range action × 8 + direction : les huit directions sont huit "
        "entrées INDÉPENDANTES, chacune avec son propre nombre d'images.\n\n"
        "13 actions = 104 entrées = un jeu de sprites JOUEUR. Beaucoup moins "
        "(5 à 8 actions) = un sprite de MONSTRE, donc un déguisement."));

    // 🔴 L'anomalie qu'on cherche en premier : l'entrée demandée n'existe pas.
    // Le client rend alors 1 image (`Act_GetFrameCount`), l'animation se termine
    // aussitôt, et tout ce qu'elle bloquait repart immédiatement.
    if (s.played_frames <= 0) {
      char warn[240];
      _snprintf_s(warn, sizeof(warn), _TRUNCATE,
                  i18n::Tr("L'entrée %d est demandée alors que le fichier n'en "
                           "a que %d : le client rend 1 image, l'animation se "
                           "termine tout de suite."),
                  s.played_index, s.act_action_count);
      ImGui::TextColored(ro::pal::kWarn, "%s", warn);
    }

    // Les cinq sondes, chacune sur ses huit directions.
    for (int p = 0; p < 5; ++p) DrawProbe(s, s.probes[p]);
  }

  // ── Les autres sprites de l'acteur ────────────────────────────────────────
  // Arme, traînée, bouclier, chariot : lus par le même chemin que le composeur
  // de pantin, donc déjà éprouvés.
  rag::OwnActorSprites parts;
  if (rag::ReadOwnActorSprites(&parts)) {
    SeparatorText(i18n::Tr("Autres emplacements"));
    Row(i18n::Tr("Arme"),     parts.weapon_spr[0] ? parts.weapon_spr : "—");
    Row(i18n::Tr("Traînée"),  parts.trail_spr[0]  ? parts.trail_spr  : "—");
    Row(i18n::Tr("Bouclier"), parts.shield_spr[0] ? parts.shield_spr : "—");
    Row(i18n::Tr("Chariot"),  parts.cart_spr[0]   ? parts.cart_spr   : "—");
  }
}

// ── Rapport texte ────────────────────────────────────────────────────────────

std::string CharDiagnostics::BuildReport(const Snapshot& s) const {
  std::string r;
  r.reserve(2048);
  Line(&r, "=== Bourgeon — diagnostic du personnage ===");
  Line(&r, "Nom            : %s", s.char_name);
  Line(&r, "AID / CID      : %u / %u", s.aid, s.cid);
  Line(&r, "Classe serveur : %d (%s)", s.job_real, s.job_real_name);
  Line(&r, "Niveaux        : base %d / job %d", s.base_level, s.job_level);
  Line(&r, "HP / SP        : %d/%d  %d/%d", s.hp, s.hp_max, s.sp, s.sp_max);
  Line(&r, "Poids          : %d / %d", s.weight, s.weight_max);
  Line(&r, "");
  Line(&r, "-- Stats --");
  for (int i = 0; i < 6; ++i)
    Line(&r, "%-4s %d + %d = %d (cout %d)", kStatName[i], s.stat_base[i],
         s.stat_bonus[i], s.stat_base[i] + s.stat_bonus[i], s.stat_cost[i]);
  Line(&r, "ATK %d + %d | MATK %d ~ %d | DEF %d + %d | MDEF %d + %d",
       s.atk1, s.atk2, s.matk_min, s.matk_max, s.def_soft, s.def_hard,
       s.mdef_soft, s.mdef_hard);
  Line(&r, "HIT %d | FLEE %d + %d | CRIT %d", s.hit, s.flee, s.pdodge, s.crit);
  Line(&r, "");
  Line(&r, "-- Temps --");
  Line(&r, "amotion        : %d ms (ASPD %d)", s.amotion,
       rag::AspdFromAmotion(s.amotion));
  Line(&r, "marche         : %d ms/cellule", s.walk_speed);
  Line(&r, "plafond anim   : %d ms", s.motion_cap);
  if (dealt_.seen)
    Line(&r, "dernier coup porte   : amotion %d / dmotion %d / degats %d",
         dealt_.src_speed, dealt_.dst_speed, dealt_.damage);
  if (taken_.seen)
    Line(&r, "dernier coup encaisse: amotion %d / dmotion(moi) %d / degats %d",
         taken_.src_speed, taken_.dst_speed, taken_.damage);
  Line(&r, "");
  Line(&r, "-- Acteur --");
  if (!s.actor_found) {
    Line(&r, "(aucun acteur)");
    return r;
  }
  Line(&r, "adresse        : 0x%08X  GID %u", s.actor_addr, s.actor_gid);
  Line(&r, "classe affichee: %d (%s)%s", s.job_shown, s.job_shown_name,
       s.job_shown != s.job_real ? "  <-- DEGUISEMENT" : "");
  Line(&r, "classe resolue : %d (%s)", s.job_resolved, s.job_resolved_name);
  Line(&r, "etat motion    : %d%s", s.motion_state,
       MotionStateBlocksSkill(s.motion_state) ? "  (BLOQUE les skills)" : "");
  Line(&r, "action jouee   : entree %d (action %d, direction %d), image %d/%d",
       s.played_index, s.played_index / kDirCount, s.played_index % kDirCount,
       s.frame_index, s.played_frames);
  Line(&r, "facteur motion : %.4f (= amotion %.0f ms)", s.motion_factor,
       s.motion_factor * kMotionReferenceMs);
  Line(&r, "delai/image    : %.4f (= %.1f ms), cycle %.0f ms", s.frame_delay,
       s.frame_delay * kAnimTickMs, CycleMs(s.played_frames, s.frame_delay));
  {
    int samples = 0;
    float window_s = 0.0f;
    const float rate = ObservedRate(&samples, &window_s);
    if (rate > 0.0f)
      Line(&r, "cadence mesuree: %.2f coup/s (%d coups en %.1f s)", rate,
           samples, window_s);
    // 🔴 Le rapport DOIT porter ce que le client a émis : sans lui, on compare
    // deux essais sans savoir si la différence est avant ou après l'envoi.
    const uint32_t sent_skill  = g_sent_skill.load(std::memory_order_relaxed);
    const uint32_t sent_attack = g_sent_attack.load(std::memory_order_relaxed);
    const uint32_t since       = g_sent_since.load(std::memory_order_relaxed);
    const uint32_t now         = timeGetTime();
    const float sent_span = (since != 0 && now > since)
                                ? static_cast<float>(now - since) / 1000.0f
                                : 0.0f;
    if (sent_span > 0.2f)
      Line(&r, "paquets emis   : %u skill (%.2f/s) + %u attaque (%.2f/s) en %.1f s",
           sent_skill, static_cast<float>(sent_skill) / sent_span, sent_attack,
           static_cast<float>(sent_attack) / sent_span, sent_span);
    else
      Line(&r, "paquets emis   : %u skill + %u attaque", sent_skill, sent_attack);
    if (state_samples_ > 0) {
      Line(&r, "etats bloquants: %.0f%% sur %u images",
           100.0f * static_cast<float>(state_blocked_) /
               static_cast<float>(state_samples_),
           state_samples_);
      for (int b = 0; b < kStateBuckets; ++b) {
        if (state_hist_[b] == 0) continue;
        Line(&r, "  etat %-3d : %5.1f%%", b,
             100.0f * static_cast<float>(state_hist_[b]) /
                 static_cast<float>(state_samples_));
      }
    }
  }
  Line(&r, "spr            : %s", s.spr_path);
  Line(&r, "act            : %s  (%d entrees = %d actions x 8)", s.act_path,
       s.act_action_count, s.act_action_count / kDirCount);
  for (int p = 0; p < 5; ++p) {
    const Snapshot::ActProbe& probe = s.probes[p];
    if (probe.action < 0) continue;
    const char* name = ActionName(probe.action);
    char frames[80] = {};
    int written = 0;
    for (int d = 0; d < kDirCount; ++d)
      written += _snprintf_s(frames + written, sizeof(frames) - written,
                             _TRUNCATE, (d == 0) ? "%d" : " %d",
                             probe.frames[d]);
    Line(&r, "%-11s(%2d): images [%s] x %.2f", name ? name : "?", probe.action,
         frames, probe.speed);
  }
  return r;
}

// ── Précision des zones cliquables ───────────────────────────────────────────

void CharDiagnostics::ApplyPickMinSize() {
  // 🔴 Le défaut vient de la capture CENTRALE (pick_quad_tweaks), jamais de la
  // globale elle-même : le diviseur joueur écrit dedans AVANT nous dans la même
  // frame (ordre des plugins), et lire ici capturerait une valeur déjà divisée.
  if (pick_min_default_ <= 0) {
    pick_min_default_ = pick_quad::MinAreaDefault(pick_quad::kFamilyActors);
    if (pick_min_default_ <= 0) return;
    if (pick_min_size_ < 0) pick_min_size_ = pick_min_default_;
  }
  // Calculé HORS du __try (appel de fonction ordinaire, mais autant rester
  // simple dans le bloc SEH) : la valeur à rendre quand le forçage s'éteint.
  // C'est celle de l'étage JOUEUR — son diviseur, s'il est actif, doit
  // réapparaître, pas le défaut brut.
  const int hand_back =
      pick_quad::MinAreaCurrent(pick_quad::kFamilyActors);
  __try {
    int* p = reinterpret_cast<int*>(gamescene::kPickQuadMinSizeAddr);
    // 🔴 Éteint, on NE TOUCHE À RIEN — même pas pour réécrire le défaut. La
    // leçon vient du dézoom : un tweak qui réécrivait « sa » base à chaque tick,
    // option décochée comprise, annulait la commande native et le patch WARP
    // cent millisecondes après la frappe. On restaure donc une seule fois, à la
    // transition, puis on laisse la globale à l'étage du dessous.
    if (!pick_min_enabled_) {
      if (pick_min_written_) {
        *p = (hand_back > 0) ? hand_back : pick_min_default_;
        pick_min_written_ = false;
      }
      return;
    }
    // Allumé, réécrit à chaque image : le diviseur joueur écrit lui aussi par
    // frame, et c'est ce forçage-ci qui doit gagner.
    *p = (pick_min_size_ > 0) ? pick_min_size_ : pick_min_default_;
    pick_min_written_ = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Contour des zones cliquables ─────────────────────────────────────────────

void CharDiagnostics::DrawPickBoxes() {
  PickBox boxes[kMaxPickBoxes];
  const int count = CollectPickBoxes(boxes);
  if (count <= 0) return;

  float mx = 0.0f, my = 0.0f;
  const float* hovered = HoveredQuad(&mx, &my);

  // 🔴 Draw-list de PREMIER PLAN : ces rectangles doivent se voir PAR-DESSUS les
  // fenêtres ImGui, sinon le panneau qu'on garde ouvert pour lire les mesures
  // masquerait justement la zone qu'on observe.
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const uint32_t own_aid = static_cast<uint32_t>(ReadInt(rag::kOwnAccountIdAddr));

  for (int i = 0; i < count; ++i) {
    const PickBox& b = boxes[i];
    if (b.x1 <= b.x0 || b.y1 <= b.y0) continue;

    const bool is_hover = (hovered != nullptr && b.quad == hovered);
    const bool is_self  = (own_aid != 0 && b.aid == own_aid);

    // Le rectangle que le clic prendrait : épais et jaune. C'est la seule
    // information qui compte quand une compétence part sur la mauvaise cible.
    const uint32_t col = is_hover ? IM_COL32(255, 235, 60, 255) : PickCatColor(b.cat);
    dl->AddRect(ImVec2(b.x0, b.y0), ImVec2(b.x1, b.y1), col, 0.0f, 0,
                is_hover ? 2.5f : 1.0f);
    if (is_self)
      dl->AddRect(ImVec2(b.x0 - 1.0f, b.y0 - 1.0f), ImVec2(b.x1 + 1.0f, b.y1 + 1.0f),
                  IM_COL32(255, 120, 220, 160), 0.0f, 0, 1.0f);

    // Étiquette réservée au survolé : tout étiqueter rendrait l'écran illisible
    // dès qu'il y a du monde, et c'est le survolé qu'on cherche à identifier.
    if (is_hover) {
      char tag[96];
      _snprintf_s(tag, sizeof(tag), _TRUNCATE, "AID %u  job %d  cat %d", b.aid,
                  b.job, b.cat);
      const ImVec2 at(b.x0, b.y0 - ImGui::GetFontSize() - 2.0f);
      dl->AddRectFilled(ImVec2(at.x - 2.0f, at.y - 1.0f),
                        ImVec2(at.x + ImGui::CalcTextSize(tag).x + 3.0f,
                               at.y + ImGui::GetFontSize() + 1.0f),
                        IM_COL32(0, 0, 0, 170));
      dl->AddText(at, IM_COL32(255, 235, 60, 255), tag);
    }
  }

  // Le compte, près du curseur : « combien de rectangles se disputent ce clic »
  // est déjà une réponse en soi.
  char hud[120];
  _snprintf_s(hud, sizeof(hud), _TRUNCATE,
              hovered ? i18n::Tr("%d zones — 1 sous le curseur")
                      : i18n::Tr("%d zones — aucune sous le curseur"),
              count);
  dl->AddText(ImVec2(mx + 14.0f, my + 14.0f), IM_COL32(255, 255, 255, 210), hud);
}

// ── Fenêtre ──────────────────────────────────────────────────────────────────

void CharDiagnostics::OnRenderUI() {
  // 🔴 AVANT le test d'ouverture : l'overlay s'observe la fenêtre FERMÉE, sans
  // quoi le panneau recouvrirait la scène qu'on regarde. Gate staff quand même —
  // c'est le même droit que tout le reste de ce module.
  // Le réglage de précision AGIT sur le jeu : il ne s'applique que pour le
  // staff, et il se relit à chaque image (cf. ApplyPickMinSize).
  if (IsStaff()) ApplyPickMinSize();
  if (show_pick_boxes_ && IsStaff()) DrawPickBoxes();

  if (!open_) return;
  // 🔴 Revérifié CHAQUE frame, pas seulement à l'ouverture : un compte qui perd
  // le staff en cours de session ne doit pas garder sous les yeux des adresses
  // mémoire. Même règle que l'inspecteur d'entités et la fenêtre de journal.
  if (!IsStaff()) { open_ = false; return; }

  Snapshot snap;
  Refresh(&snap);
  ReadActor(&snap);

  // Échantillon d'état, une fois par image, tant que la fenêtre est ouverte.
  // C'est le seul relevé de cette fenêtre qui s'ACCUMULE — tout le reste est
  // relu à neuf — parce qu'une proportion de temps ne se lit pas sur un
  // instantané.
  if (snap.actor_found) {
    if (state_since_ms_ == 0) state_since_ms_ = timeGetTime();
    const int bucket = (snap.motion_state >= 0 && snap.motion_state < kStateBuckets)
                           ? snap.motion_state
                           : kStateBuckets - 1;
    ++state_hist_[bucket];
    ++state_samples_;
    if (MotionStateBlocksSkill(snap.motion_state)) ++state_blocked_;
  }

  if (need_focus_) {
    ImGui::SetNextWindowFocus();
    need_focus_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(ro::Px(560.0f), ro::Px(620.0f)),
                           ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(ro::Px(420.0f), ro::Px(240.0f)),
                                      ImVec2(FLT_MAX, FLT_MAX));

  // Suffixe « ### » STABLE et hors de `Tr` : il fige l'identité ImGui (donc la
  // position et la taille dans imgui.ini) d'une langue à l'autre.
  char title[160];
  _snprintf_s(title, sizeof(title), _TRUNCATE, "%s###bourgeon_char_diag",
              i18n::Tr("Diagnostic du personnage"));

  const bool begun = ro::BeginRoWindow(title, &open_);
  if (begun) {
    PushStyleCompact();
    DrawBody(snap);
    PopStyleCompact();

    ImGui::Separator();
    if (ro::RoButton(i18n::Tr("Copier le rapport"))) {
      const std::string report = BuildReport(snap);
      ImGui::SetClipboardText(report.c_str());
    }
    ImGui::SameLine();
    // Les deux mesures qui s'accumulent (cadence et histogramme d'états) ne
    // valent que sur un essai propre : ce bouton en marque le départ.
    if (ro::RoButton(i18n::Tr("Remettre les mesures à zéro"))) ResetMeasures();
    ImGui::SameLine();
    HelpMarker(i18n::Tr(
        "Tout ce que le CLIENT sait du personnage joué, relu à chaque image. "
        "Rien n'est demandé au serveur : les valeurs sont celles sur lesquelles "
        "ce client-ci agit.\n\n"
        "amotion et dmotion des deux « derniers coups » viennent des paquets "
        "eux-mêmes — ce sont des événements, pas un état : l'âge affiché dit "
        "s'ils décrivent encore la situation."));
  }
  // ⚠ EndRoWindow s'appelle TOUJOURS, même repliée ou masquée (règle Begin/End).
  ro::EndRoWindow();
}
