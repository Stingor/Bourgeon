#include "features/fx/skill_range.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>

#include "imgui.h"

#include "features/fx/cell_style.h"  // la table des trois dessins, partagée
#include "features/fx/grey_world.h"  // AddScenePainter : le foyer du dessin au sol
#include "ragnarok/actor.h"          // rag::actor::kJobId
#include "ragnarok/game_scene.h"     // kGmActorMgr : le gestionnaire d'acteurs
#include "ragnarok/globals.h"        // rag::ActiveModeSafe / ActiveModeIfReady
#include "ragnarok/job_ids.h"        // rag::IsMonsterJob
#include "ragnarok/lua.h"            // GetSkillScale, la MÊME table que le natif
#include "ui/color_codec.h"          // ro::ArgbFromPicker : LA conversion ARGB
#include "ui/ro_imgui.h"             // ro::RoCheckbox, ro::RoCombo
#include "ui/ro_widgets.h"           // mui::WheelSliderInt, RoColorSwatch
#include "utils/i18n.h"
#include "utils/memory_patch.h"      // mem::WriteCode

namespace skill_range {
namespace {

Config g_cfg;

// ── 1. Le correctif : pendant l'INCANTATION ─────────────────────────────────

// Le `call Actor_FindByGid` en tête de `EffectApply_SkillScaleEffect`, et lui
// seul. Motif d'origine relevé sur le client 20250716 : `E8 E9 85 08 00`, soit
// un rel32 vers 0x00d806a0. On refuse d'écrire sur toute autre disposition —
// une adresse qui a glissé vaut mieux non patchée que patchée au hasard.
constexpr uintptr_t kFindActorCallSite = 0x00cf80b2;
const uint8_t kStockCall[5] = {0xE8, 0xE9, 0x85, 0x08, 0x00};

// La porte qui, elle, connaît le joueur : `__thiscall(actorMgr, gid)`, et son
// premier geste est `if (gid == g_Account_Aid) return *(this+0x2C)`.
constexpr uintptr_t kFindByGidOrSelf = 0x00a69e70;

// Le remplaçant, à la convention EXACTE de ce qu'il remplace : `Actor_FindByGid`
// est `__stdcall(gid)`, et le site d'appel empile son argument. Il refait le
// même chemin — mode actif, gestionnaire d'acteurs — en changeant la dernière
// porte, puis répond au réglage.
void* __stdcall FindActorOrSelf(uint32_t gid) {
  // ⚠ Hors du `__try` : `ActiveModeSafe` porte déjà le sien, et un bloc qui
  // couvre plusieurs étapes ne dirait plus laquelle a échoué.
  void* mode = rag::ActiveModeSafe();
  if (mode == nullptr) return nullptr;
  __try {
    void* mgr = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mode) +
                                          gamescene::kGmActorMgr);
    if (mgr == nullptr) return nullptr;
    using FindFn = void*(__thiscall*)(void*, uint32_t);
    void* actor = reinterpret_cast<FindFn>(kFindByGidOrSelf)(mgr, gid);
    if (actor == nullptr) return nullptr;

    // ⭐ LE FILTRE TIENT ICI, et nulle part ailleurs. Rendre nullptr fait sortir
    // la native à sa première ligne — exactement ce qu'elle faisait avant le
    // correctif. « Ne pas montrer les sorts des joueurs » n'est donc pas un
    // masquage : c'est le comportement d'origine, rendu à l'identique.
    //
    // ⚠ ON NE TOUCHE PAS AUX MONSTRES. Leur zone marche nativement depuis
    // toujours, et l'éteindre serait retirer au joueur quelque chose qu'il n'a
    // pas demandé — un MVP qui annonce son sort, c'est une information de jeu.
    if (!g_cfg.players) {
      const unsigned job =
          *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(actor) +
                                       rag::actor::kJobId);
      if (!rag::IsMonsterJob(job)) return nullptr;
    }
    return actor;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ── 2. La prévisualisation : pendant le CIBLAGE ─────────────────────────────

// L'état du ciblage armé, dans le CGameMode. Les trois offsets sont ceux que
// QuickCast lit déjà pour émettre un lancement au clavier (cf. quick_cast.cc).
constexpr int kOffTargetingMode  = 0x408;  // 1 sol, 2 cible, 4 soutien
constexpr int kOffTargetingSkill = 0x40c;
constexpr int kOffTargetingLevel = 0x414;

// La cellule sous le curseur : `gamescene::kPickGroundCellAddr`, partagée avec
// QuickCast.

// 🔴 La table des trois textures est COMMUNE avec GreyWorld : elle vit dans
// features/fx/cell_style.h. Ne restent ici que les deux contrats qui lient notre
// énumération persistée à cette table.
static_assert(static_cast<int>(Config::kPatternCount) == cellstyle::kCount,
              "Config::Pattern doit couvrir exactement cellstyle::kTextures");
static_assert(static_cast<int>(Config::kPatternSolid) == cellstyle::kSolid,
              "l'index du carreau plein est persisté dans le yaml : il ne bouge pas");

// `GetSkillScale(id, lv)` -> largeur, hauteur en cases.
//
// ⭐ C'est LA MÊME SOURCE que celle du natif : la table Lua `SKILL_INFO_LIST`.
// La prévisualisation et le rendu d'incantation ne peuvent donc pas se
// contredire, ce qu'une table à nous n'aurait pas garanti.
//
// 🔴 L'appel Lua vit dans sa propre fonction : MSVC refuse un `__try` dans une
// fonction qui construit des objets à dérouler (C2712).
bool SkillScale(int skill_id, int level, int* out_w, int* out_h) {
  bool ok = false;
  __try {
    void* L = lua::State();
    if (L == nullptr) return false;
    lua::CheckStack(L, 4);
    lua::GetField(L, lua::kGlobalsIndex, "GetSkillScale");
    lua::PushNumber(L, static_cast<double>(skill_id));
    lua::PushNumber(L, static_cast<double>(level));
    if (lua::PCall(L, 2, 2, 0) == 0) {
      *out_w = static_cast<int>(lua::ToNumber(L, -2));
      *out_h = static_cast<int>(lua::ToNumber(L, -1));
      ok = (*out_w > 0 && *out_h > 0);
      lua::Pop(L, 2);  // les deux résultats demandés
    } else {
      lua::Pop(L, 1);  // ⚠ l'échec ne laisse QU'UN objet, celui d'erreur :
                       // en dépiler deux entamerait la pile de l'appelant
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  return ok;
}

// Le style demandé à GreyWorld, relu à chaque passe. `texture == nullptr` =
// éteint : ni résolution de texture, ni appel du peintre.
grey_world::PainterStyle PreviewStyle() {
  grey_world::PainterStyle st;
  if (!g_cfg.preview) return st;  // texture nulle : coût nul
  const int pat = (g_cfg.pattern >= 0 && g_cfg.pattern < Config::kPatternCount)
                      ? g_cfg.pattern
                      : Config::kPatternTile;
  const cellstyle::Tex& s = cellstyle::kTextures[pat];
  st.texture = s.texture;
  st.u = s.u;
  st.v = s.v;
  // Le joint n'a de sens que sur un carreau plein (cf. Config::gap).
  const int gap = (g_cfg.gap < 0) ? 0 : (g_cfg.gap > 40 ? 40 : g_cfg.gap);
  st.shrink = (pat == Config::kPatternSolid) ? 1.0f - gap * 0.01f : 1.0f;
  return st;
}

// Appelé par GreyWorld pendant la passe de scène, le seul moment où l'on peut
// peindre. Il ne peint que si un sort de zone attend son clic au sol.
void PaintTargetingPreview(grey_world::CellSink paint) {
  int skill = 0, level = 0, cx = -1, cy = -1;
  bool visee = false;
  __try {
    void* gm = rag::ActiveModeIfReady();
    if (gm == nullptr) return;
    auto* m = reinterpret_cast<char*>(gm);
    // Seul le ciblage AU SOL a une zone à montrer : un sort sur cible suit une
    // entité qui bouge, et le montrer sous le curseur mentirait sur son centre.
    if (*reinterpret_cast<int*>(m + kOffTargetingMode) != 1) return;
    skill = *reinterpret_cast<int*>(m + kOffTargetingSkill);
    level = *reinterpret_cast<int*>(m + kOffTargetingLevel);
    if (skill <= 0 || level <= 0) return;

    using PickGround_t = uint8_t(__thiscall*)(void*, int*, int*);
    visee = (reinterpret_cast<PickGround_t>(
                 gamescene::kPickGroundCellAddr)(gm, &cx, &cy) &
             0xff) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

  // ⚠ Hors du `__try` : l'appel Lua porte le sien, et l'imbriquer ferait perdre
  // de vue laquelle des deux étapes a échoué.
  if (!visee) return;  // le curseur ne désigne pas le sol : rien à montrer
  int w = 0, h = 0;
  if (!SkillScale(skill, level, &w, &h)) return;

  const uint32_t argb = ro::ArgbFromPicker(g_cfg.color);
  // Centrée sur la case visée, exactement comme la native centre la sienne :
  // `i + x - largeur/2`, division entière comprise.
  for (int i = 0; i < w; ++i)
    for (int j = 0; j < h; ++j)
      paint(cx + i - w / 2, cy + j - h / 2, argb);
}

// Tout ce que la construction du plugin a à faire. Groupé ici pour que le
// constructeur n'ait pas à nommer les symboles internes de ce fichier.
void Install() {
  // La prévisualisation ne dépend pas du correctif : elle vaut même si le motif
  // d'octets a bougé, donc elle s'inscrit d'abord.
  grey_world::AddScenePainter(&PaintTargetingPreview, &PreviewStyle);

  if (std::memcmp(reinterpret_cast<const void*>(kFindActorCallSite), kStockCall,
                  sizeof(kStockCall)) != 0)
    return;

  // Un `call` proche est un déplacement relatif à l'instruction SUIVANTE, d'où
  // les cinq octets retranchés.
  const uintptr_t cible = reinterpret_cast<uintptr_t>(&FindActorOrSelf);
  const int32_t rel =
      static_cast<int32_t>(cible - (kFindActorCallSite + sizeof(kStockCall)));

  uint8_t patch[sizeof(kStockCall)] = {0xE8};
  std::memcpy(patch + 1, &rel, sizeof(rel));
  mem::WriteCode(kFindActorCallSite, patch, sizeof(patch));
}

void Tooltip(const char* text) {
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

}  // namespace

// ── API publique ─────────────────────────────────────────────────────────────
Config& cfg() { return g_cfg; }

bool DrawSettings() {
  bool changed = false;

  ImGui::TextWrapped("%s", i18n::Tr(
      "Les sorts de zone dessinent au sol les cases qu'ils toucheront. Le client "
      "sait déjà le faire pour les monstres ; ici tu décides s'il le fait aussi "
      "pour le sort que tu tiens en main, et pour ceux des autres joueurs."));
  ImGui::Spacing();

  // ── Le sort armé ───────────────────────────────────────────────────────────
  if (ro::RoCheckbox(i18n::Tr("Montrer la zone du sort que je tiens en main"),
                     &g_cfg.preview)) {
    changed = true;
  }
  Tooltip(i18n::Tr("Dès qu'un sort de zone est armé et attend ton clic, sa zone "
                   "suit le curseur : tu vois où il tombera avant de le lâcher.\n"
                   "Personne d'autre ne la voit — rien n'est envoyé au serveur."));

  ImGui::BeginDisabled(!g_cfg.preview);
  ImGui::Indent();

  // Le combo « Dessin » et le curseur de joint sont communs avec GreyWorld
  // (features/fx/cell_style.h). Seule l'infobulle du joint nous est propre :
  // ici, à zéro, c'est la ZONE qui devient un aplat d'un seul tenant.
  if (cellstyle::DrawPatternSettings(
          &g_cfg.pattern, &g_cfg.gap,
          i18n::Tr("Largeur de la bordure, en pourcentage du côté de la case. "
                   "À zéro, les carreaux se touchent et la zone devient un "
                   "aplat d'un seul tenant."))) {
    changed = true;
  }

  if (mui::RoColorSwatch(i18n::Tr("Couleur et opacité"), g_cfg.color)) {
    changed = true;
  }
  Tooltip(i18n::Tr("L'opacité est le curseur « A » du sélecteur. À zéro, plus "
                   "rien n'est dessiné — et rien n'est soumis au rendu non plus."));

  ImGui::Unindent();
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // ── L'incantation des autres ──────────────────────────────────────────────
  if (ro::RoCheckbox(i18n::Tr("Montrer la zone des sorts en cours d'incantation"),
                     &g_cfg.players)) {
    changed = true;
  }
  Tooltip(i18n::Tr("Celle des JOUEURS — la tienne comprise — pendant qu'ils "
                   "incantent. C'est le serveur qui l'annonce, et elle est déjà "
                   "visible par tout le monde autour : tu ne dévoiles rien en "
                   "l'affichant.\n"
                   "Les monstres, eux, la montrent déjà et ce réglage n'y change "
                   "rien : un MVP qui annonce son sort est une information de "
                   "jeu, pas un confort."));

  ImGui::Spacing();
  ImGui::TextDisabled("%s", i18n::Tr(
      "Tous les sorts n'ont pas de zone déclarée : c'est le serveur qui dit "
      "lesquels en ont une, et de quelle taille."));

  return changed;
}

}  // namespace skill_range

SkillRangePatch::SkillRangePatch() { skill_range::Install(); }
