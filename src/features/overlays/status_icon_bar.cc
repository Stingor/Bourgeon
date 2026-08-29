#include "features/overlays/status_icon_bar.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "bourgeon.h"
#include "features/gameplay/afk_screen.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/status_cell.h"
#include "features/systems/status_effects.h"
#include "imgui.h"
#include "ragnarok/globals.h"
#include "ragnarok/social.h"  // rag::social::OwnAid
#include "ui/hud_frame.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/hooking/hook_manager.h"
#include "utils/i18n.h"
#include "utils/log_console.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ===========================================================================
// La barre de MES états (client 20250716 / Moonlight-Destiny.exe, base 0x400000)
//
// Le dessin est à nous (`statuscell`) ; il ne reste du client que sa LISTE et
// deux détours, décrits dans l'en-tête. Voir status_icon_bar.h pour le pourquoi.
// ===========================================================================

namespace {

// ---- adresses du client ----------------------------------------------------
//
// La liste des états actifs : un `std::vector` global de la session, élément de
// 20 octets dont l'id est le premier int et l'échéance le suivant.
constexpr uintptr_t kVecBegin  = 0x0136e6c8;
constexpr uintptr_t kVecEnd    = 0x0136e6cc;
constexpr int kElemStride  = 0x14;
constexpr int kElemEndTick = 0x04;

// La construction / disposition de la barre du client. Détournée pour qu'elle
// n'émette plus rien quand la nôtre est allumée.
constexpr uintptr_t kBuildFn = 0x00bd4230;
constexpr int kSceneForce = 0x174;  // drapeau « refaire »
constexpr int kSceneGate  = 0x178;  // porte / sentinelle
constexpr int kClearVtbl  = 0x40;   // slot de vtable : efface les nœuds d'icône
constexpr uint32_t kGateSentinel = 0x5f5e0fe;

// Le rendu d'un sprite d'effet de type 0. Lit `node+0x1c0` comme couleur du
// quad, APRÈS que l'animation de fondu du client l'a réécrite — c'est le seul
// point où forcer notre alpha atteint vraiment les sommets. Réservé au marqueur
// `node+0x3e8 == 'b'`, posé uniquement sur les icônes d'état.
constexpr uintptr_t kRenderType0 = 0x00b5ed20;
constexpr int     kNodeMark    = 0x3e8;
constexpr uint8_t kNodeMarkVal = 0x62;
constexpr int     kNodeColor   = 0x1c0;

// __thiscall émulé en __fastcall + edx factice (la convention du projet).
using BuildFn_t  = void (__fastcall*)(void* scene, void* edx);
using RenderFn_t = void (__fastcall*)(void* node);
using ClearFn_t  = void (__fastcall*)(void* scene, void* edx, int, int, int, int,
                                      int, int, int, int, int);

BuildFn_t  g_build_orig  = nullptr;
RenderFn_t g_render_orig = nullptr;
void*      g_scene       = nullptr;
bool       g_in_game     = false;
bool       g_needs_save  = false;

// La barre ImGui est-elle celle qui commande ? Lue depuis les détours, qui
// tournent hors de notre contrôle — d'où une copie tenue à jour au tick plutôt
// qu'un accès au plugin depuis le fil de rendu du client.
bool g_ours = false;

inline int& SceneI(void* s, int off) {
  return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(s) + off);
}

uint8_t g_alpha_byte = 255;  // l'opacité, pré-calculée pour le détour de rendu

// ── Détour n°1 : le rendu d'une icône NATIVE ────────────────────────────────
//
// Ne sert plus que lorsque la barre du client est celle qu'on garde. Deux
// usages, et le second n'a pas d'autre point d'entrée :
//   · l'opacité réglable ;
//   · 🔴 l'ÉCRAN DE VEILLE. Son effacement d'interface veto le rendu des
//     FENÊTRES — or ces icônes n'en sont pas : ce sont des nœuds de sprite de la
//     scène, qui traversaient donc l'écran de veille intacts. On écrit 0 dans
//     l'alpha que le rendu s'apprête à lire, et la frame suivante les redessine
//     comme si de rien n'était.
void __fastcall RenderIconHook(void* node) {
  const AfkScreen* afk = Bourgeon::Instance().afk_screen();
  const bool afk_hides = (afk != nullptr) && afk->hiding_ui();
  if ((afk_hides || g_alpha_byte < 255) && node) {
    __try {
      auto* B = reinterpret_cast<uint8_t*>(node);
      if (*reinterpret_cast<uint8_t*>(B + kNodeMark) == kNodeMarkVal) {
        auto* col = reinterpret_cast<uint32_t*>(B + kNodeColor);
        const uint32_t a = afk_hides ? 0u : g_alpha_byte;
        *col = (*col & 0x00ffffffu) | (a << 24);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  if (g_render_orig) g_render_orig(node);
}

// ── Détour n°2 : la construction de la barre du client ──────────────────────
//
// 🔴 ON REJOUE SA PORTE, ET ON N'ÉMET RIEN. La porte n'est pas décorative :
// c'est elle qui appelle l'EFFACEMENT des nœuds déjà posés (slot de vtable
// 0x40). Sauter la fonction entière laisserait les icônes de la frame précédente
// figées à l'écran pour toujours ; l'appeler telle quelle les reconstruirait.
// Rejouer la porte sans rien émettre est le seul chemin qui laisse la machine
// d'état du client intacte ET l'écran propre.
void __fastcall BuildHook(void* scene, void* edx) {
  g_scene = scene;
  if (!g_ours) {
    if (g_build_orig) g_build_orig(scene, edx);
    return;
  }
  __try {
    int gate = SceneI(scene, kSceneGate);
    if (static_cast<uint32_t>(gate) == kGateSentinel) {
      SceneI(scene, kSceneGate) = 0;
      gate = 0;
    }
    if (SceneI(scene, kSceneForce) != 0) {
      SceneI(scene, kSceneForce) = 0;
      SceneI(scene, kSceneGate)  = 0;
      void** vtbl = *reinterpret_cast<void***>(scene);
      reinterpret_cast<ClearFn_t>(vtbl[kClearVtbl / 4])(
          scene, nullptr, 0, 0x19, 0, 100, 0, 0, 0, 0, 0);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Demande au client de refaire sa barre — pour qu'il efface la sienne au moment
// où l'on prend la main, et qu'il la repose quand on la rend.
void ForceRebuild() {
  if (!g_scene) return;
  __try {
    SceneI(g_scene, kSceneForce) = 1;
    BuildHook(g_scene, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

ImU32 Col(const float rgba[4]) {
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]));
}

}  // namespace

namespace statusicons {

// 🔴 Les adresses de la liste vivent ICI parce que c'est ce fichier qui l'a
// trouvée. La lire ailleurs aurait recopié trois constantes de plus — et
// `StatusEffects` s'en sert pour MON propre GID, sans les connaître.
bool ReadOwn(std::vector<Active>* out) {
  if (out == nullptr) return false;
  out->clear();
  __try {
    uint8_t* begin = *reinterpret_cast<uint8_t**>(kVecBegin);
    uint8_t* end   = *reinterpret_cast<uint8_t**>(kVecEnd);
    if (begin == nullptr || end == nullptr || end < begin) return false;
    // Une borne de sûreté : un vecteur incohérent (device perdu, mode en cours
    // de bascule) ne doit pas nous faire parcourir la moitié du processus.
    if (static_cast<size_t>(end - begin) > kElemStride * 256u) return false;
    for (uint8_t* e = begin; e != end; e += kElemStride) {
      Active a;
      a.id = static_cast<uint16_t>(*reinterpret_cast<int*>(e));
      a.end_tick = *reinterpret_cast<uint32_t*>(e + kElemEndTick);
      if (a.id != 0) out->push_back(a);
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->clear();
    return false;
  }
}

}  // namespace statusicons

StatusIconBar::StatusIconBar() {
  // La construction de la barre du client (FUN_00bd4230) : prologue
  // 55 8B EC 83 EC 28. Vérifié avant de poser le détour, pour qu'un client
  // différent échoue proprement au lieu de sauter dans du code déplacé.
  const auto* p = reinterpret_cast<const uint8_t*>(kBuildFn);
  if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && p[3] == 0x83 &&
      p[4] == 0xEC && p[5] == 0x28) {
    g_build_orig = reinterpret_cast<BuildFn_t>(
        hooking::HookManager::Instance().SetHook(
            hooking::HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(kBuildFn),
            reinterpret_cast<uint8_t*>(&BuildHook)));
    if (!g_build_orig)
      LogError("[StatusIcons] échec du détour de construction");
  } else {
    LogError("[StatusIcons] prologue FUN_00bd4230 inattendu ; détour ignoré");
  }

  // Le rendu d'une icône (FUN_00b5ed20) : prologue 55 8B EC 83 EC 1C.
  const auto* r = reinterpret_cast<const uint8_t*>(kRenderType0);
  if (r[0] == 0x55 && r[1] == 0x8B && r[2] == 0xEC && r[3] == 0x83 &&
      r[4] == 0xEC && r[5] == 0x1C) {
    g_render_orig = reinterpret_cast<RenderFn_t>(
        hooking::HookManager::Instance().SetHook(
            hooking::HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(kRenderType0),
            reinterpret_cast<uint8_t*>(&RenderIconHook)));
    if (!g_render_orig)
      LogError("[StatusIcons] échec du détour d'opacité");
  } else {
    LogError("[StatusIcons] prologue FUN_00b5ed20 inattendu ; détour ignoré");
  }
}

void StatusIconBar::OnTick() {
  const bool want = enabled_ && g_in_game;
  // Une BASCULE, et seulement elle : c'est le moment où le client doit effacer
  // sa barre (on prend la main) ou la reposer (on la rend). Le redemander à
  // chaque tick ferait reconstruire pour rien.
  if (want != g_ours) {
    g_ours = want;
    ForceRebuild();
  }
  const int a = std::max(10, std::min(100, alpha_));
  g_alpha_byte = static_cast<uint8_t>(a * 255 / 100);
}

void StatusIconBar::OnModeSwitch(ModeMgr::ModeType mode_type,
                                 const char* /*map_name*/) {
  g_in_game = (mode_type == ModeMgr::ModeType::kGame);
  // Ne pas garder une scène du monde précédent : le détour la déréférencerait.
  if (!g_in_game) {
    g_scene = nullptr;
    g_ours = false;
  }
}

// ── Mes états ───────────────────────────────────────────────────────────────
//
// 🔴 PAR LE REGISTRE, ET NON PAR UNE LECTURE À NOUS. `StatusEffects::Effects`
// sert MON propre GID depuis cette même liste du client, et il y ajoute deux
// choses qu'une lecture brute n'a pas :
//
//   · la CONVERSION D'HORLOGE — l'échéance de la liste se compare à
//     `GetTickCount`, tout le reste travaille en `timeGetTime` ; ce sont des
//     compteurs SÉPARÉS, et confondre les deux donne un restant faux ;
//   · la DURÉE D'ORIGINE, que la liste ne porte PAS. Le registre la reconstitue
//     en gardant le plus grand restant observé.
//
// 🔴 C'est ce second point qui a coûté le grisage : sans total, `statuscell` ne
// peut pas calculer de part écoulée et ne voile RIEN. La première version de ce
// fichier relisait la liste elle-même et posait `total_ms = 0` faute de mieux —
// le voile marchait sur les faux états, qui ont un total, et sur eux seuls.
void StatusIconBar::Collect(std::vector<StatusEffects::Entry>* out) const {
  out->clear();
  auto* fx = Bourgeon::Instance().status_effects();
  const uint32_t me = rag::social::OwnAid();
  if (fx != nullptr && me != 0) fx->Effects(me, out);

  if (preview_) statuscell::AppendPreview(out, max_icons_);
}

void StatusIconBar::OnRenderUI() {
  if (g_needs_save && !ImGui::IsAnyItemActive()) {
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
    g_needs_save = false;
  }
  if (!enabled_ || !g_in_game) return;
  if (Bourgeon::Instance().IsMapLoading()) return;

  // L'écran de veille efface l'interface : notre barre en fait partie, et elle
  // n'a pas besoin du détour de rendu pour se taire.
  const AfkScreen* afk = Bourgeon::Instance().afk_screen();
  if (afk != nullptr && afk->hiding_ui()) return;

  std::vector<StatusEffects::Entry> list;
  Collect(&list);

  // MAJ + curseur sur la barre la fait apparaître le temps de la déplacer.
  // 🔴 MAJ SEULE NE SUFFIT PAS : en RO, MAJ+clic est l'attaque forcée, et une
  // barre vide qui surgirait à chaque appui clignoterait pendant tout un combat.
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool over =
      mouse.x >= static_cast<float>(rect_.x) &&
      mouse.y >= static_cast<float>(rect_.y) &&
      mouse.x <  static_cast<float>(rect_.x + rect_.w) &&
      mouse.y <  static_cast<float>(rect_.y + rect_.h);
  const bool grabbing =
      ImGui::GetIO().KeyShift && (over || ro::HudFrameDraggingIs("##status_icon_bar"));
  const bool placing = !locked_ || grabbing;

  // Rien à montrer, rien à l'écran — surtout pas un cadre vide qu'on prendrait
  // pour une fonction en panne.
  if (list.empty() && !placing) return;

  ro::HudFrameOpts opts;
  opts.locked   = locked_ && !grabbing;
  opts.border   = border_;
  opts.rounding = ro::Px(3.0f);
  opts.bg       = col_bg_;
  if (auto* mui = Bourgeon::Instance().moonlight_ui()) opts.grid = &mui->grid_;
  opts.min_w    = ro::Px(24.0f);
  opts.min_h    = ro::Px(16.0f);

  if (ro::BeginHudFrame("##status_icon_bar", &rect_, opts, &geometry_dirty_)) {
    const float pad  = ro::Px(3.0f);
    const float side = ro::Px(static_cast<float>(std::max(8, icon_px_)));
    const float gap  = ro::Px(static_cast<float>(std::max(0, gap_px_)));
    const float fsz  = ro::Px(static_cast<float>(std::max(7, time_px_)));

    // 🔴 L'origine vient de la FENÊTRE, pas du curseur ImGui : déverrouillé,
    // `BeginHudFrame` pose un `InvisibleButton` sur toute sa surface qui laisse
    // le curseur EN DESSOUS, et des cases posées à `GetCursorScreenPos()`
    // tombaient hors du clip dès qu'on tenait MAJ.
    const ImVec2 win = ImGui::GetWindowPos();

    statuscell::Style st;
    st.sweep       = sweep_;
    st.sweep_color = Col(col_sweep_);
    st.time_px     = show_time_ ? fsz : 0.0f;
    // 🔴 L'opacité passe par le STYLE, pas par `ImGuiStyleVar_Alpha` : ce
    // dernier n'est appliqué que par les widgets qui le lisent, et la case ne
    // dessine que des primitives de `ImDrawList`, qui l'ignorent. Le
    // `PushStyleVar` qui était ici n'avait aucun effet — et rien ne le signalait.
    st.alpha = static_cast<float>(std::max(10, std::min(100, alpha_))) / 100.0f;

    // Les quatre bords utiles du cadre, une fois la marge retirée.
    const float left   = win.x + pad;
    const float right  = win.x + static_cast<float>(rect_.w) - pad;
    const float top    = win.y + pad;
    const float bottom = win.y + static_cast<float>(rect_.h) - pad;

    statuscell::RowOpts row;
    row.side  = side;
    row.gap   = gap;
    row.max   = max_icons_;
    row.rows  = rows_;
    row.sort  = sort_;
    row.rtl   = (anchor_ == 1 || anchor_ == 3);
    row.up    = (anchor_ == 2 || anchor_ == 3);
    // La limite est le bord OPPOSÉ à l'ancrage : c'est celui qu'on ne doit pas
    // franchir, et il change avec le sens.
    row.limit = row.rtl ? left : right;

    const ImVec2 origin(row.rtl ? right : left, row.up ? bottom : top);
    statuscell::DrawRow(list, origin, row, st, true);
  }
  ro::EndHudFrame();

  if (geometry_dirty_) {
    geometry_dirty_ = false;
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}

// ── Les réglages (sans fenêtre) — hébergés par le panneau moonlight_ui ──────
void StatusIconBar::DrawSettings() {
  bool changed = false;

  changed |= ro::RoCheckbox(i18n::Tr("Barre d'états moderne"), &enabled_);
  SameLine(); HelpMarker(i18n::Tr(
      "Remplace la barre d'icônes du client par la nôtre : durée, grisage de la "
      "part écoulée, infobulle complète, tri et lignes.\n\n"
      "Éteinte, la barre d'origine revient telle quelle."));

  changed |= WheelSliderInt(i18n::Tr("Opacité des icônes"), &alpha_, 10, 100, "%d%%");
  SameLine(); HelpMarker(i18n::Tr(
      "Vaut pour les DEUX barres : la moderne comme celle du client."));

  ImGui::BeginDisabled(!enabled_);

  // ⚠ L'aperçu ne compte pas dans `changed` : il ne se persiste pas, donc il n'y
  // a rien à sauver — et le marquer modifierait le fichier de réglages à chaque
  // fois qu'on le coche.
  ro::RoCheckbox(i18n::Tr("Aperçu (faux statuts)"), &preview_);
  SameLine(); HelpMarker(i18n::Tr(
      "Remplit la barre de faux états, aux durées étagées, le temps de la "
      "régler — sans attendre d'en avoir de vrais.\n\n"
      "Ne se garde pas d'une session à l'autre."));

  changed |= ro::RoCheckbox(i18n::Tr("Verrouiller la position"), &locked_);
  SameLine(); HelpMarker(i18n::Tr(
      "Verrouillée, la barre est figée et laisse passer les clics. Maintenez "
      "MAJ au-dessus d'elle pour la déplacer sans la déverrouiller."));
  changed |= ro::RoCheckbox(i18n::Tr("Bordure"), &border_);
  changed |= mui::RoColorSwatch(i18n::Tr("Fond"), col_bg_);

  SeparatorText(i18n::Tr("Icônes"));
  changed |= WheelSliderInt(i18n::Tr("Taille des icônes"), &icon_px_, 8, 48, "%d px");
  changed |= WheelSliderInt(i18n::Tr("Espacement"), &gap_px_, 0, 12, "%d px");
  changed |= WheelSliderInt(i18n::Tr("Nombre maximum"), &max_icons_, 1, 100);
  SameLine(); HelpMarker(i18n::Tr(
      "Au-delà, les états en trop sont écartés — ce sont les DERNIERS du "
      "rangement ci-dessous, qui décide donc de ce qu'on perd."));
  {
    // ⚠ Items NUS : RoCombo traduit à la lecture.
    const char* kAnchors[] = {"Haut gauche", "Haut droite", "Bas gauche",
                              "Bas droite"};
    changed |= ro::RoCombo(i18n::Tr("Ancrage"), &anchor_, kAnchors,
                           IM_ARRAYSIZE(kAnchors));
  }
  SameLine(); HelpMarker(i18n::Tr(
      "Le coin du cadre d'où les icônes se rangent, et donc le sens dans lequel "
      "la barre grandit.\n\n"
      "⚠ Ancrée du mauvais côté, la rangée RECULE quand un état tombe et toutes "
      "les icônes glissent. Une barre posée à droite de l'écran veut pousser "
      "vers la gauche, celle du bas vers le haut."));
  changed |= WheelSliderInt(i18n::Tr("Lignes"), &rows_, 1, 6);
  SameLine(); HelpMarker(i18n::Tr(
      "La barre se replie DÉJÀ sur sa largeur ; ce réglage force en plus un "
      "nombre fixe par rangée, pour une barre haute et étroite plutôt que "
      "longue et plate."));
  {
    // ⚠ Items NUS : RoCombo traduit à la lecture — les envelopper les ferait
    // passer DEUX fois par Tr, et la panne serait invisible en français.
    const char* kSorts[] = {"Ordre d'arrivée", "Bientôt fini d'abord",
                            "Plus long d'abord"};
    changed |= ro::RoCombo(i18n::Tr("Rangement"), &sort_, kSorts,
                           IM_ARRAYSIZE(kSorts));
  }
  SameLine(); HelpMarker(i18n::Tr(
      "« Bientôt fini d'abord » met en tête ce qu'il faudra relancer.\n\n"
      "⚠ Un état sans durée n'a pas de place dans un tri par durée : il va "
      "toujours en fin, quel que soit le sens."));

  SeparatorText(i18n::Tr("Écoulement"));
  changed |= ro::RoCheckbox(i18n::Tr("Temps restant sous l'icône"), &show_time_);
  ImGui::BeginDisabled(!show_time_);
  Indent();
    changed |= WheelSliderInt(i18n::Tr("Taille du texte"), &time_px_, 7, 20, "%d px");
  Unindent();
  ImGui::EndDisabled();
  {
    const char* kSweeps[] = {"Aucun", "Balayage horaire", "Voile descendant"};
    changed |= ro::RoCombo(i18n::Tr("Grisage de la case"), &sweep_, kSweeps,
                           IM_ARRAYSIZE(kSweeps));
  }
  SameLine(); HelpMarker(i18n::Tr(
      "La case s'assombrit à mesure que l'état s'écoule : on voit d'un coup "
      "d'œil ce qui est près de tomber.\n\n"
      "⚠ Un état sans échéance n'est jamais voilé — il n'a pas de part "
      "écoulée, et le griser à moitié le ferait croire à mi-course."));
  ImGui::BeginDisabled(sweep_ == statuscell::kSweepNone);
  Indent();
    changed |= mui::RoColorSwatch(i18n::Tr("Voile"), col_sweep_);
  Unindent();
  ImGui::EndDisabled();

  ImGui::EndDisabled();

  if (changed) g_needs_save = true;
}
