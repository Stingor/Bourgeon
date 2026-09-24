#include "features/overlays/camera_hud.h"

#include <Windows.h>  // SEH autour de la lecture des objets natifs

#include <cmath>  // NAN
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/staff_gate.h"
#include "imgui.h"
#include "ragnarok/camera.h"
#include "ragnarok/game_scene.h"
#include "ragnarok/game_settings.h"
#include "ragnarok/globals.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"

namespace {

// ── Les globaux caméra du moteur (.data, client 20250716) ────────────────────
// Relevés dans `Camera_ApplyViewDistanceClamp` 0x00c82340 (arrivée sur une
// carte) et `Camera_DragControl` 0x00c79f90 (glisser au bouton droit). Tous des
// float. Les deux plafonds de zoom sont déjà dans ragnarok/camera.h.
constexpr uintptr_t kZoomMinAddr        = 0x012291bc;  // plancher du zoom (230)
constexpr uintptr_t kTiltWorkAddr       = 0x012291c8;  // tilt en cours de glisser
constexpr uintptr_t kZoomWorkAddr       = 0x012291cc;  // zoom en cours de glisser
// La MÉMOIRE par genre de carte : le moteur la sauve en quittant une carte
// (CGameMode_OnExit) et la recharge en arrivant sur une carte du même genre.
constexpr uintptr_t kTiltSavedIndoorAddr  = 0x012291d0;
constexpr uintptr_t kZoomSavedIndoorAddr  = 0x012291d4;
constexpr uintptr_t kTiltSavedOutdoorAddr = 0x012291d8;
constexpr uintptr_t kZoomSavedOutdoorAddr = 0x012291dc;

// ── Le reste de la pose, dans l'objet caméra ─────────────────────────────────
// Lus dans `Camera_LerpCurrentTowardTarget` 0x00a7ab90 et le builder de vue
// 0x00a7ae20. Le point visé courant rattrape la cible au même lissage que la
// pose ; l'œil est recalculé chaque frame depuis la pose et plaqué au-dessus du
// terrain (axe Y vers le BAS : un œil plus haut a un Y plus petit).
constexpr int kCurLookAt = 0x20;  // x, y, z
constexpr int kTgtLookAt = 0x38;  // x, y, z — la position du personnage suivi
constexpr int kEye       = 0x80;  // x, y, z

// ── Les champs caméra du CGameMode ───────────────────────────────────────────
constexpr int kGmOutdoor      = 0x254;  // int : carte hors d'indoorrswtable
constexpr int kGmHasViewpoint = 0x258;  // int : ligne viewpointtable trouvée
constexpr int kGmViewpoint    = 0x25c;  // int16[9], cf. kViewpointNames
constexpr int kGmIndoorYaw    = 0x288;  // float : ancre de rotation en intérieur

// Les neuf colonnes de `viewpointtable`, dans l'ordre du fichier. Elles
// remplacent les bornes par défaut quand la carte a sa ligne.
constexpr int kViewpointCount = 9;
constexpr const char* kViewpointNames[kViewpointCount] = {
    "range", "scope", "range_IN", "rotFrom", "rotTo",
    "rot_IN", "altFrom", "altTo", "alt_IN"};

// Les deux commandes du client qui agissent sur le rig.
constexpr int kFlagFixedCamera = 0x36;  // /camera : point visé collé, sans lissage
constexpr int kFlagZoomOut     = 0xf1;  // /zoom   : plafond extérieur élargi

// ── Les constantes des bornes, LUES DANS LE CODE et pas recopiées ────────────
// `Camera_DragControl` 0x00c79f90 borne le tilt à [centre - écart, écart - 70]
// (écart - 45 Ctrl tenu), écart 20 dehors / 10 dedans, et la rotation en
// intérieur à ancre ± 20. Ces nombres sont des floats de .rdata que chaque
// instruction désigne par une adresse absolue.
//
// 🔴 Les recopier en constantes était FAUX sur l'exe livré : le patch WARP
// `HighCamAngle` (IncrCamAngle.qjs) redirige l'opérande de l'écart DEHORS vers
// un float neuf à 65 dans sa zone d'allocation — l'IDB, vanilla, dit 20. On lit
// donc l'adresse DANS l'instruction, puis le float qu'elle désigne : ce que le
// client exécute, patché ou non.
//
// Les six sites, tous `F3 0F <op> <modrm> <adresse 32 bits>` : l'adresse est à
// +4. L'opcode attendu est revérifié avant de suivre le pointeur.
struct CodeFloat {
  uintptr_t site;
  uint8_t   opcode;   // 0x10 movss, 0x5C subss
  float     vanilla;  // la valeur de l'IDB, pour signaler un patch
};
constexpr uint8_t kMovss = 0x10;
constexpr uint8_t kSubss = 0x5C;
constexpr CodeFloat kTiltSpanIndoor  = {0x00c7a191, kMovss, 10.0f};
constexpr CodeFloat kTiltSpanOutdoor = {0x00c7a1ae, kMovss, 20.0f};  // HighCamAngle
constexpr CodeFloat kTiltCenter      = {0x00c7a219, kMovss, -45.0f};
constexpr CodeFloat kTiltCeilCtrl    = {0x00c7a256, kSubss, 45.0f};  // plafond = écart - x
constexpr CodeFloat kTiltCeil        = {0x00c7a265, kSubss, 70.0f};
constexpr CodeFloat kYawSpanIndoor   = {0x00c7a106, kSubss, 20.0f};  // ancre ± x

bool g_enabled = false;

// Le float désigné par l'instruction, ou NaN si le site n'a plus la forme
// attendue (un autre patch l'a réécrit) : le HUD l'affiche alors comme tel
// plutôt que d'inventer une borne.
float ReadCodeFloat(const CodeFloat& c) {
  __try {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(c.site);
    if (p[0] != 0xF3 || p[1] != 0x0F || p[2] != c.opcode) return NAN;
    const uintptr_t addr = *reinterpret_cast<const uint32_t*>(p + 4);
    return *reinterpret_cast<const float*>(addr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return NAN;
  }
}

// Tout ce que le HUD affiche, relu d'un bloc chaque frame.
struct Snapshot {
  bool  has_cam = false;
  float cur_pitch, cur_yaw, cur_dist;
  float tgt_pitch, tgt_yaw, tgt_dist;
  float cur_look[3], tgt_look[3], eye[3];

  bool  has_mode = false;  // CGameMode confirmé (sa caméra est la nôtre)
  bool  outdoor = true;
  bool  has_viewpoint = false;
  int16_t viewpoint[kViewpointCount];
  float indoor_yaw;

  float zoom_min, zoom_max_out, zoom_max_in;
  float tilt_work, zoom_work;
  float tilt_saved_in, zoom_saved_in, tilt_saved_out, zoom_saved_out;

  // Lues dans le code (cf. CodeFloat) : NaN si le site est méconnaissable.
  float tilt_span_in, tilt_span_out, tilt_center, tilt_ceil, tilt_ceil_ctrl;
  float yaw_span_in;
};

float G(uintptr_t addr) { return *reinterpret_cast<const float*>(addr); }
float F(const void* base, int off) {
  return *reinterpret_cast<const float*>(static_cast<const char*>(base) + off);
}

// Les globaux sont en .data, toujours lisibles ; la caméra est validée par sa
// vtable (ro::camera::Get). Le CGameMode, lui, n'est retenu que si SA caméra est
// celle qu'on lit : ActiveMode() désigne aussi le mode de login, dont les
// offsets ci-dessus ne veulent rien dire.
bool Capture(Snapshot* s) {
  s->zoom_min       = G(kZoomMinAddr);
  s->zoom_max_out   = G(ro::camera::kZoomMaxOutdoorAddr);
  s->zoom_max_in    = G(ro::camera::kZoomMaxIndoorAddr);
  s->tilt_work      = G(kTiltWorkAddr);
  s->zoom_work      = G(kZoomWorkAddr);
  s->tilt_saved_in  = G(kTiltSavedIndoorAddr);
  s->zoom_saved_in  = G(kZoomSavedIndoorAddr);
  s->tilt_saved_out = G(kTiltSavedOutdoorAddr);
  s->zoom_saved_out = G(kZoomSavedOutdoorAddr);
  s->tilt_span_in   = ReadCodeFloat(kTiltSpanIndoor);
  s->tilt_span_out  = ReadCodeFloat(kTiltSpanOutdoor);
  s->tilt_center    = ReadCodeFloat(kTiltCenter);
  s->tilt_ceil      = ReadCodeFloat(kTiltCeil);
  s->tilt_ceil_ctrl = ReadCodeFloat(kTiltCeilCtrl);
  s->yaw_span_in    = ReadCodeFloat(kYawSpanIndoor);

  void* cam = ro::camera::Get();
  if (!cam) return false;
  __try {
    s->cur_pitch = F(cam, ro::camera::kCurPitch);
    s->cur_yaw   = F(cam, ro::camera::kCurYaw);
    s->cur_dist  = F(cam, ro::camera::kCurDist);
    s->tgt_pitch = F(cam, ro::camera::kTgtPitch);
    s->tgt_yaw   = F(cam, ro::camera::kTgtYaw);
    s->tgt_dist  = F(cam, ro::camera::kTgtDist);
    for (int i = 0; i < 3; ++i) {
      s->cur_look[i] = F(cam, kCurLookAt + i * 4);
      s->tgt_look[i] = F(cam, kTgtLookAt + i * 4);
      s->eye[i]      = F(cam, kEye + i * 4);
    }
    s->has_cam = true;

    const char* gm = static_cast<const char*>(rag::ActiveMode());
    if (gm && *reinterpret_cast<void* const*>(gm + gamescene::kGmCamera) == cam) {
      s->outdoor       = *reinterpret_cast<const int*>(gm + kGmOutdoor) != 0;
      s->has_viewpoint = *reinterpret_cast<const int*>(gm + kGmHasViewpoint) != 0;
      for (int i = 0; i < kViewpointCount; ++i)
        s->viewpoint[i] = reinterpret_cast<const int16_t*>(gm + kGmViewpoint)[i];
      s->indoor_yaw = F(gm, kGmIndoorYaw);
      s->has_mode = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return s->has_cam;
}

// Une ligne du HUD : libellé + valeur, ou un titre de section (value vide).
// Les mêmes lignes servent au dessin et à la copie, pour que le presse-papier
// dise exactement ce que l'écran montre.
struct Row {
  std::string label;
  std::string value;
};

std::string Fmt(const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
  va_end(ap);
  return buf;
}

std::string Vec3(const float v[3]) {
  return Fmt("%.1f, %.1f, %.1f", v[0], v[1], v[2]);
}

void BuildRows(const Snapshot& s, std::vector<Row>* rows) {
  auto section = [&](const char* t) { rows->push_back({t, ""}); };
  auto row = [&](const char* l, std::string v) { rows->push_back({l, std::move(v)}); };

  section(i18n::Tr("Pose (courante → cible)"));
  row(i18n::Tr("Tilt (élévation)"), Fmt("%.2f° → %.2f°", s.cur_pitch, s.tgt_pitch));
  row(i18n::Tr("Rotation"),         Fmt("%.2f° → %.2f°", s.cur_yaw, s.tgt_yaw));
  row(i18n::Tr("Zoom (distance)"),  Fmt("%.2f → %.2f", s.cur_dist, s.tgt_dist));
  row(i18n::Tr("Point visé"),       Vec3(s.cur_look));
  row(i18n::Tr("Point visé (cible)"), Vec3(s.tgt_look));
  row(i18n::Tr("Œil"),              Vec3(s.eye));

  section(i18n::Tr("Bornes du moteur"));
  if (s.has_mode) {
    row(i18n::Tr("Carte"), s.outdoor ? i18n::Tr("extérieur") : i18n::Tr("intérieur"));
    // Ce que `Camera_DragControl` applique vraiment. Une ligne viewpointtable
    // prend le pas sur les bornes par défaut.
    if (s.has_viewpoint) {
      const int16_t* v = s.viewpoint;
      row(i18n::Tr("Zoom"),     Fmt("[%d, %d]", v[0], v[0] + v[1]));
      row(i18n::Tr("Tilt"),     Fmt("[%d, %d]", v[7], v[6]));
      row(i18n::Tr("Rotation"), Fmt("[%d, %d]", v[3], v[4]));
    } else {
      // NaN se propage : un site méconnaissable s'affiche « nan », pas faux.
      const float span = s.outdoor ? s.tilt_span_out : s.tilt_span_in;
      const float max  = s.outdoor ? s.zoom_max_out : s.zoom_max_in;
      row(i18n::Tr("Zoom"), Fmt("[%.0f, %.0f]", s.zoom_min, max));
      row(i18n::Tr("Tilt"), Fmt(i18n::Tr("[%.0f, %.0f] (Ctrl : %.0f)"),
                                s.tilt_center - span, span - s.tilt_ceil,
                                span - s.tilt_ceil_ctrl));
      if (s.outdoor)
        row(i18n::Tr("Rotation"), i18n::Tr("libre"));
      else
        row(i18n::Tr("Rotation"), Fmt("%.0f ± %.0f", s.indoor_yaw, s.yaw_span_in));
    }
  }
  // Les constantes elles-mêmes, et l'écart à l'IDB vanilla quand un patch de
  // l'exe les a changées : c'est ce qui dit POURQUOI une borne surprend.
  auto code_row = [&](const char* label, float live, const CodeFloat& c) {
    if (live == c.vanilla)
      row(label, Fmt("%.1f", live));
    else
      row(label, Fmt(i18n::Tr("%.1f (vanilla %.1f, patché)"), live, c.vanilla));
  };
  code_row(i18n::Tr("Écart de tilt extérieur"), s.tilt_span_out, kTiltSpanOutdoor);
  code_row(i18n::Tr("Écart de tilt intérieur"), s.tilt_span_in, kTiltSpanIndoor);
  code_row(i18n::Tr("Centre du tilt"), s.tilt_center, kTiltCenter);
  code_row(i18n::Tr("Décalage du plafond"), s.tilt_ceil, kTiltCeil);
  code_row(i18n::Tr("Décalage du plafond (Ctrl)"), s.tilt_ceil_ctrl, kTiltCeilCtrl);
  row(i18n::Tr("Zoom min"), Fmt("%.2f", s.zoom_min));
  row(i18n::Tr("Zoom max extérieur"), Fmt("%.2f", s.zoom_max_out));
  row(i18n::Tr("Zoom max intérieur"), Fmt("%.2f", s.zoom_max_in));

  section(i18n::Tr("Mémoire par genre de carte"));
  row(i18n::Tr("Extérieur"), Fmt(i18n::Tr("zoom %.2f · tilt %.2f°"),
                                 s.zoom_saved_out, s.tilt_saved_out));
  row(i18n::Tr("Intérieur"), Fmt(i18n::Tr("zoom %.2f · tilt %.2f°"),
                                 s.zoom_saved_in, s.tilt_saved_in));
  row(i18n::Tr("Glisser en cours"), Fmt(i18n::Tr("zoom %.2f · tilt %.2f°"),
                                        s.zoom_work, s.tilt_work));

  if (s.has_mode) {
    section(i18n::Tr("viewpointtable"));
    if (!s.has_viewpoint) {
      row(i18n::Tr("Ligne"), i18n::Tr("aucune pour cette carte"));
    } else {
      for (int i = 0; i < kViewpointCount; ++i)
        row(kViewpointNames[i], Fmt("%d", s.viewpoint[i]));
    }
  }

  section(i18n::Tr("Commandes"));
  row("/camera", gamesettings::IsOn(kFlagFixedCamera)
                     ? i18n::Tr("ON (point visé collé)")
                     : i18n::Tr("OFF (suivi lissé)"));
  row("/zoom", gamesettings::IsOn(kFlagZoomOut) ? "ON" : "OFF");
}

void CopyRows(const std::vector<Row>& rows) {
  std::string out;
  for (const Row& r : rows) {
    if (r.value.empty()) {
      out += "[" + r.label + "]\n";
    } else {
      out += r.label + " : " + r.value + "\n";
    }
  }
  ImGui::SetClipboardText(out.c_str());
}

}  // namespace

namespace camera_hud {

bool& enabled() { return g_enabled; }

bool DrawSettings() {
  const bool changed = ro::RoCheckbox(i18n::Tr("HUD caméra"), &g_enabled);
  ImGui::SameLine();
  mui::HelpMarker(
      i18n::Tr("Affiche en valeurs tous les réglages de la caméra du monde : "
               "tilt (élévation), rotation et zoom, courants et cibles — le "
               "moteur lisse la pose courante vers la cible —, le point visé et "
               "la position de l'œil.\n\n"
               "Puis les bornes que le moteur applique au glisser et à la "
               "molette (y compris celles d'une ligne viewpointtable), la "
               "mémoire du zoom par genre de carte, et l'état de /camera et "
               "/zoom.\n\n"
               "Lecture seule. Le HUD se déplace à la souris ; clic droit pour "
               "copier les valeurs."));
  return changed;
}

void Draw() {
  if (!g_enabled || !IsStaff()) return;
  if (!Bourgeon::Instance().IsGameActive()) return;

  Snapshot snap;
  if (!Capture(&snap)) return;  // pas encore de caméra : entre-cartes, login

  std::vector<Row> rows;
  rows.reserve(40);
  BuildRows(snap, &rows);

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + ro::Px(16.0f),
                                 vp->WorkPos.y + ro::Px(140.0f)),
                          ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.78f);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("##bourgeon_camera_hud", nullptr, flags)) {
    if (ImGui::BeginTable("##cam", 2, ImGuiTableFlags_SizingFixedFit)) {
      for (const Row& r : rows) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (r.value.empty()) {
          ImGui::TextDisabled("%s", r.label.c_str());
          continue;
        }
        ImGui::TextUnformatted(r.label.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(r.value.c_str());
      }
      ImGui::EndTable();
    }
    if (ImGui::BeginPopupContextWindow("##cam_ctx")) {
      if (ImGui::MenuItem(i18n::Tr("Copier les valeurs"))) CopyRows(rows);
      ImGui::EndPopup();
    }
  }
  ImGui::End();
}

}  // namespace camera_hud
