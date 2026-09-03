#include "features/overlays/dps_meter.h"

#include <Windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "bourgeon.h"
#include "imgui.h"
#include "ui/imgui_escape.h"
#include "ragnarok/ui_window_mgr.h"
#include "ragnarok/packets.h"  // rag::zc : les paquets de notification de coup
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/log_console.h"
#include "utils/i18n.h"
#include "utils/text.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)
// Les dispositions des paquets de coup sont partagées avec l'autre lecteur de
// ces opcodes (cf. ragnarok/packets.h). L'alias garde les points d'appel courts.
namespace zc = rag::zc;


// action bytes that carry real damage (exclude miss, pickup, sit, stand)
static bool IsDamageAction(uint8_t action) {
  return action != 0x01 && action != 0x02 && action != 0x03 && action != 0x0b;
}

DpsMeter::DpsMeter() {
  auto& b = Bourgeon::Instance();
  b.RegisterObserveOpcode(zc::kNotifyAct,    sizeof(zc::NotifyActPayload));
  b.RegisterObserveOpcode(zc::kNotifySkill,  sizeof(zc::NotifySkillPayload));
  b.RegisterObserveOpcode(zc::kNotifySkill2, sizeof(zc::NotifySkill2Payload));
  b.RegisterRecvOpcode(kOpcodeSkillUnitDmg);
}

void DpsMeter::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);
  if (!in_game_) {
    ResetHistory();
    chat_queue_.clear();
  }
}

// Fil RÉSEAU : on copie, rien de plus (cf. features/net_inbox.h).
void DpsMeter::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

// Fil PRINCIPAL : le décodage, rejoué à chaque frame, dans l'ordre d'arrivée.
void DpsMeter::HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  const uint32_t player_aid = Bourgeon::Instance().client().session().aid();
  uint32_t src_id = 0;
  int      damage = 0;
  uint8_t  action = 0;

  if (opcode == zc::kNotifyAct && len >= sizeof(zc::NotifyActPayload)) {
    const auto* p = reinterpret_cast<const zc::NotifyActPayload*>(data);
    src_id = static_cast<uint32_t>(p->src_id);
    damage = p->damage;
    action = p->type;
  } else if (opcode == zc::kNotifySkill &&
             len >= sizeof(zc::NotifySkillPayload)) {
    const auto* p = reinterpret_cast<const zc::NotifySkillPayload*>(data);
    src_id = p->src_id;
    damage = p->damage;
    action = static_cast<uint8_t>(p->action);
  } else if (opcode == zc::kNotifySkill2 &&
             len >= sizeof(zc::NotifySkill2Payload)) {
    const auto* p = reinterpret_cast<const zc::NotifySkill2Payload*>(data);
    src_id = p->src_id;
    damage = p->damage;
    action = p->type;
  } else if (opcode == kOpcodeSkillUnitDmg && len >= 8) {
    // ZC_BOURGEON_SKILL_DMG: [src_aid:4][damage:4] — sent SELF by server
    // for skill-unit hits (Storm Gust, Meteor Storm, LoV…).
    src_id = *reinterpret_cast<const uint32_t*>(data);
    damage = *reinterpret_cast<const int32_t*>(data + 4);
    action = 0;  // treated as normal hit — passes IsDamageAction
  } else {
    return;
  }

  if (src_id != player_aid || !IsDamageAction(action) || damage <= 0)
    return;

  RecordDamage(damage);

  if (opcode == kOpcodeSkillUnitDmg && show_ground_dmg_in_chat_) {
    // Queue for delivery in OnRenderUI — never call UIWindowMgr::SendMsg from
    // inside the recv dispatch loop; under heavy AoE (Storm Gust/Meteor Storm)
    // it generates dozens of packets per frame and stalls the recv loop.
    if (static_cast<int>(chat_queue_.size()) < kMaxChatQueueSize) {
      char msg[64];
      std::snprintf(msg, sizeof(msg), i18n::Tr("Ground skill : %d dmg"), damage);
      chat_queue_.emplace_back(msg);
    }
  }
}

void DpsMeter::ResetHistory() {
  events_.clear();
  plot_buf_.fill(0.0f);
  plot_offset_      = 0;
  last_slot_tick_   = 0;
  last_damage_tick_ = 0;
  combat_start_tick_= 0;
  in_combat_        = false;
  total_damage_     = 0;
  current_dps_      = 0.0f;
  peak_dps_         = 0.0f;
}

void DpsMeter::RecordDamage(int damage) {
  const DWORD now = GetTickCount();

  if (!in_combat_ || (now - last_damage_tick_) > static_cast<DWORD>(combat_timeout_secs_ * 1000)) {
    // New combat — reset totals but keep plot history visible
    combat_start_tick_ = now;
    in_combat_         = true;
    total_damage_      = 0;
    peak_dps_          = 0.0f;
    events_.clear();
    plot_buf_.fill(0.0f);
    plot_offset_    = 0;
    last_slot_tick_ = now;
  }

  last_damage_tick_ = now;
  total_damage_    += damage;
  events_.push_back({now, damage});
}

void DpsMeter::FlushChatQueue() {
  int flushed = 0;
  while (!chat_queue_.empty() && flushed < kMaxChatFlushPerFrame) {
    const std::string& msg = chat_queue_.front();
    UIWindowMgr::SendMsg(UIMessage::UIM_PUSHINTOCHATHISTORY,
                         reinterpret_cast<int>(msg.c_str()), 0xFFAA00, 0, 0);
    chat_queue_.erase(chat_queue_.begin());
    ++flushed;
  }
}

void DpsMeter::UpdatePlotSlot(DWORD now) {
  if (last_slot_tick_ == 0) return;

  const DWORD sms = static_cast<DWORD>(slot_ms_);
  while (now - last_slot_tick_ >= sms) {
    float slot_dmg = 0.0f;
    for (const auto& e : events_) {
      if (e.tick_ms >= last_slot_tick_ && e.tick_ms < last_slot_tick_ + sms)
        slot_dmg += static_cast<float>(e.damage);
    }
    const float slot_dps = slot_dmg * (1000.0f / sms);
    plot_buf_[plot_offset_] = slot_dps;
    plot_offset_ = (plot_offset_ + 1) % kPlotSlots;
    last_slot_tick_ += sms;
  }

  // Prune events older than the full plot window
  const DWORD cutoff = now - kPlotSlots * sms;
  while (!events_.empty() && events_.front().tick_ms < cutoff)
    events_.pop_front();
}

// ── Peinture ─────────────────────────────────────────────────────────────────
namespace {

// Le gros nombre, en multiples de la police courante. Assez grand pour se lire
// en périphérie, pendant qu'on regarde le combat et pas le compteur.
constexpr float kBigTextScale   = 1.85f;
constexpr float kUnitTextScale  = 0.80f;   // le « DPS » qui suit le nombre
constexpr float kLabelTextScale = 0.72f;   // les intitulés des tuiles du bas

// Gris des textes secondaires. Clair, parce que le fond peut être presque
// transparent : le TextDisabled d'ImGui (~0.5) s'y effaçait.
constexpr ImU32 kSubCol = IM_COL32(200, 200, 205, 255);

// La couleur du réglage, reprise à des opacités différentes — c'est ce qui donne
// son unité de ton au compteur. `mul` multiplie l'alpha choisi par le joueur.
inline ImU32 Tint(const float rgba[4], float mul) {
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(rgba[0], rgba[1], rgba[2], rgba[3] * mul));
}

// Texte posé à la main, avec son ombre portée : le compteur flotte au-dessus du
// monde, et sans ombre un chiffre clair disparaît sur un décor clair. L'ombre
// suit la TAILLE du texte — un décalage d'1 px sous un nombre de 28 px ne se
// voit plus.
//
// 🔴 Position ARRONDIE : un glyphe à coordonnée fractionnaire est flou.
void PaintText(ImDrawList* dl, ImFont* font, float px, ImVec2 pos, ImU32 col,
               const char* txt) {
  const ImVec2 p(std::floor(pos.x), std::floor(pos.y));
  const float sh = std::max(1.0f, px * 0.055f);
  dl->AddText(font, px, ImVec2(p.x + sh, p.y + sh), IM_COL32(0, 0, 0, 170), txt);
  dl->AddText(font, px, p, col, txt);
}

// 🔴 Mesurer À LA TAILLE DE DESSIN, jamais à celle de la police courante :
// mesurer 15 px pour dessiner 28 décale tous les alignements à droite.
inline float TextW(ImFont* font, float px, const char* txt) {
  return font->CalcTextSizeA(px, FLT_MAX, 0.0f, txt).x;
}

// « 12 345 » tant que le nombre se lit, « 123,4 k » puis « 12,34 M » au-delà.
// Un compteur de dégâts franchit vite le million, et sept chiffres qui défilent
// ne se lisent pas — à ce moment-là on veut l'ORDRE DE GRANDEUR.
//
// Les gabarits passent par le catalogue : le séparateur décimal est une virgule
// en français, un point ailleurs.
void FormatBig(long long v, char* out, size_t n) {
  if (v < 0) v = 0;
  if (v < 100000LL) {
    text::GroupThousands(v, out, n);
  } else if (v < 1000000LL) {
    const long long tenths = (v + 50) / 100;  // dixièmes de millier
    std::snprintf(out, n, i18n::Tr("%lld,%lld k"), tenths / 1000, (tenths / 100) % 10);
  } else {
    const long long cents = (v + 5000) / 10000;  // centièmes de million
    std::snprintf(out, n, i18n::Tr("%lld,%02lld M"), cents / 100, cents % 100);
  }
}

}  // namespace

// ── L'en-tête : l'état du combat, et le chiffre qu'on vient lire ─────────────
void DpsMeter::DrawHeader(float width, float height, DWORD now) {
  ImDrawList* dl   = ImGui::GetWindowDrawList();
  const ImVec2 org = ImGui::GetCursorScreenPos();
  ImFont* reg      = ImGui::GetFont();
  ImFont* bold     = ro::FontBold() ? ro::FontBold() : reg;
  const float base_px  = ImGui::GetFontSize();
  const float big_px   = base_px * kBigTextScale;
  const float small_px = base_px * kUnitTextScale;

  // La pastille d'état. En combat elle BAT — le regard apprend l'état sans lire,
  // ce qui est tout l'intérêt d'un HUD qu'on regarde du coin de l'œil.
  const float dot_r = ro::Px(3.5f);
  const ImVec2 dot(org.x + dot_r, org.y + height * 0.5f);
  if (in_combat_) {
    constexpr float kPulseMs = 1100.0f;
    const float phase = static_cast<float>(now % static_cast<DWORD>(kPulseMs)) / kPulseMs;
    const float t = 0.5f + 0.5f * std::sin(phase * 6.2831853f);
    dl->AddCircleFilled(dot, dot_r * (1.0f + 1.1f * t),
                        IM_COL32(255, 95, 60, 20 + static_cast<int>(45.0f * (1.0f - t))));
    dl->AddCircleFilled(dot, dot_r, IM_COL32(255, 110, 70, 255));
  } else {
    dl->AddCircleFilled(dot, dot_r, IM_COL32(125, 125, 130, 150));
  }

  // Le nombre. Éteint hors combat : ce qu'il affiche alors est un zéro qui vient
  // de tomber, pas une mesure.
  char num[32];
  FormatBig(static_cast<long long>(current_dps_ + 0.5f), num, sizeof(num));
  const float num_x = dot.x + dot_r + ro::Px(7.0f);
  const float num_y = org.y + (height - big_px) * 0.5f;
  PaintText(dl, bold, big_px, ImVec2(num_x, num_y),
            Tint(text_color_, in_combat_ ? 1.0f : 0.45f), num);

  // L'unité, calée sur la LIGNE DE BASE du nombre et non sur son sommet — c'est
  // ce qui la fait lire comme un suffixe plutôt que comme un second texte.
  const char* unit = i18n::Tr("DPS");
  PaintText(dl, reg, small_px,
            ImVec2(num_x + TextW(bold, big_px, num) + ro::Px(4.0f),
                   num_y + big_px - small_px - ro::Px(1.0f)),
            Tint(text_color_, in_combat_ ? 0.60f : 0.30f), unit);

  // À droite, la durée du combat en cours — ou son absence.
  char right[48];
  if (in_combat_) {
    const float elapsed = static_cast<float>(now - combat_start_tick_) / 1000.0f;
    std::snprintf(right, sizeof(right), i18n::Tr("%.0f s"), elapsed);
  } else {
    std::snprintf(right, sizeof(right), "%s", i18n::Tr("hors combat"));
  }
  PaintText(dl, reg, base_px,
            ImVec2(org.x + width - TextW(reg, base_px, right),
                   org.y + (height - base_px) * 0.5f),
            in_combat_ ? kSubCol : IM_COL32(150, 150, 155, 210), right);

  // Le filet de séparation, dégradé vers la droite : il pose l'en-tête sans
  // couper la fenêtre en deux comme le ferait un trait plein.
  const float ly = org.y + height - ro::Px(1.0f);
  dl->AddRectFilledMultiColor(ImVec2(org.x, ly), ImVec2(org.x + width, ly + ro::Px(1.0f)),
                              Tint(text_color_, 0.50f), Tint(text_color_, 0.0f),
                              Tint(text_color_, 0.0f), Tint(text_color_, 0.50f));

  ImGui::Dummy(ImVec2(width, height));
}

// ── La courbe : une AIRE, pas un fil ─────────────────────────────────────────
// Une ligne d'un pixel sur un fond translucide se perd dans le décor. L'aire
// dégradée donne la masse — on voit la forme du combat (montée, plateau, chute)
// avant même d'avoir lu un chiffre.
void DpsMeter::DrawGraph(float width, float height) {
  ImDrawList* dl  = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + width, p0.y + height);
  const float round = ro::Px(3.0f);

  dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 55), round);

  // L'échelle suit la plus haute colonne VISIBLE, avec un peu d'air au-dessus :
  // le sommet collé au bord se lirait comme une valeur tronquée. Elle est propre
  // au graphique — les colonnes couvrent `slot_ms_`, une fenêtre bien plus
  // courte que celle du DPS affiché, donc elles montent plus haut que lui.
  float vmax = 1.0f;
  for (float v : plot_buf_) vmax = std::max(vmax, v);
  const float scale_max = vmax * 1.18f;

  // Trois repères horizontaux. Assez pâles pour ne jamais concurrencer la courbe.
  for (int g = 1; g <= 3; ++g) {
    const float y = p0.y + height * (static_cast<float>(g) / 4.0f);
    dl->AddLine(ImVec2(p0.x + round, y), ImVec2(p1.x - round, y),
                IM_COL32(255, 255, 255, 13));
  }

  const float step = width / static_cast<float>(kPlotSlots);
  const ImU32 col_top = Tint(plot_color_, 0.55f);
  const ImU32 col_bot = Tint(plot_color_, 0.05f);
  ImVec2 line[kPlotSlots];
  for (int i = 0; i < kPlotSlots; ++i) {
    // Le tampon est circulaire : `plot_offset_` est la colonne la plus ANCIENNE.
    const float v = plot_buf_[(plot_offset_ + i) % kPlotSlots];
    const float x0 = p0.x + step * static_cast<float>(i);
    const float y  = std::max(p0.y, p1.y - (v / scale_max) * height);
    if (v > 0.0f) {
      dl->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(x0 + step, p1.y),
                                  col_top, col_top, col_bot, col_bot);
    }
    line[i] = ImVec2(x0 + step * 0.5f, y);
  }
  dl->AddPolyline(line, kPlotSlots, Tint(plot_color_, 0.95f), 0,
                  std::max(1.0f, ro::Px(1.3f)));

  // Le pic, en pointillés : c'est le trait qui RELIE la tuile « pic » du bas à
  // la courbe. Il n'apparaît que s'il tombe dans l'échelle — le pic est mesuré
  // sur la fenêtre glissante, une salve courte peut le dépasser.
  if (peak_dps_ > 0.0f && peak_dps_ <= scale_max) {
    const float y = p1.y - (peak_dps_ / scale_max) * height;
    const float dash = ro::Px(4.0f), gap = ro::Px(3.0f);
    for (float x = p0.x; x < p1.x; x += dash + gap) {
      dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + dash, p1.x), y),
                  IM_COL32(255, 255, 255, 60));
    }
  }

  // La tête de courbe : le point où l'on en est. Un halo, parce que c'est le
  // seul endroit du graphique qui bouge et qu'on doit le retrouver d'un regard.
  if (in_combat_) {
    const ImVec2 head = line[kPlotSlots - 1];
    dl->AddCircleFilled(head, ro::Px(3.5f), Tint(plot_color_, 0.22f));
    dl->AddCircleFilled(head, ro::Px(1.8f), Tint(plot_color_, 1.0f));
  }

  dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 20), round);

  // L'item qui porte le survol — et qui avance le curseur de la fenêtre.
  ImGui::InvisibleButton("##dps_graph", ImVec2(width, height));
  if (ImGui::IsItemHovered()) {
    const float t = std::clamp((ImGui::GetIO().MousePos.x - p0.x) / width,
                               0.0f, 0.9999f);
    const int i = static_cast<int>(t * kPlotSlots);
    char b[32];
    FormatBig(static_cast<long long>(plot_buf_[(plot_offset_ + i) % kPlotSlots] + 0.5f),
              b, sizeof(b));
    ImGui::SetTooltip(i18n::Tr("%s DPS  ·  il y a %.1f s"), b,
                      static_cast<float>((kPlotSlots - 1 - i) * slot_ms_) / 1000.0f);
  }
}

// ── Les trois tuiles du bas ──────────────────────────────────────────────────
// Le chiffre qui compte est celui d'en haut ; ceux-ci sont le bilan. Ils sont
// donc plus petits, sous un intitulé plus petit encore — la hiérarchie tient à
// la taille, pas à la couleur, qui reste celle du réglage.
void DpsMeter::DrawStatsRow(float width, float height, DWORD now) {
  ImDrawList* dl   = ImGui::GetWindowDrawList();
  const ImVec2 org = ImGui::GetCursorScreenPos();
  ImFont* reg      = ImGui::GetFont();
  ImFont* bold     = ro::FontBold() ? ro::FontBold() : reg;
  const float base_px  = ImGui::GetFontSize();
  const float label_px = base_px * kLabelTextScale;

  // Hors combat, la durée retenue est celle du DERNIER coup porté : c'est le
  // bilan du combat qui vient de finir, pas un compteur qui continue de courir
  // pendant qu'on ne tape plus.
  const DWORD end = in_combat_ ? now : last_damage_tick_;
  const float elapsed = (combat_start_tick_ != 0 && end > combat_start_tick_)
                            ? static_cast<float>(end - combat_start_tick_) / 1000.0f
                            : 0.0f;
  const float avg = elapsed > 0.0f
                        ? static_cast<float>(total_damage_) / elapsed
                        : 0.0f;

  struct Cell { const char* label; long long value; };
  const Cell cells[3] = {
      {i18n::Tr("PIC"),   static_cast<long long>(peak_dps_ + 0.5f)},
      {i18n::Tr("MOY"),   static_cast<long long>(avg + 0.5f)},
      {i18n::Tr("TOTAL"), static_cast<long long>(total_damage_)},
  };

  const float gap = ro::Px(4.0f);
  const float tw  = (width - gap * 2.0f) / 3.0f;
  const float pad = ro::Px(3.0f);
  for (int i = 0; i < 3; ++i) {
    const ImVec2 c0(org.x + (tw + gap) * static_cast<float>(i), org.y);
    const ImVec2 c1(c0.x + tw, org.y + height);
    dl->AddRectFilled(c0, c1, IM_COL32(255, 255, 255, 11), ro::Px(3.0f));

    PaintText(dl, reg, label_px,
              ImVec2(c0.x + (tw - TextW(reg, label_px, cells[i].label)) * 0.5f,
                     c0.y + pad),
              IM_COL32(165, 165, 172, 235), cells[i].label);

    char v[32];
    FormatBig(cells[i].value, v, sizeof(v));
    PaintText(dl, bold, base_px,
              ImVec2(c0.x + (tw - TextW(bold, base_px, v)) * 0.5f,
                     c0.y + pad + label_px + ro::Px(1.0f)),
              Tint(text_color_, 0.90f), v);
  }

  ImGui::Dummy(ImVec2(width, height));
}

void DpsMeter::OnRenderUI() {
  FlushChatQueue();
  if (!in_game_) return;

  const DWORD now = GetTickCount();

  // Expire combat after timeout
  if (in_combat_ && (now - last_damage_tick_) > static_cast<DWORD>(combat_timeout_secs_ * 1000))
    in_combat_ = false;

  if (in_combat_)
    UpdatePlotSlot(now);

  // Rolling DPS over configured window
  {
    const DWORD window_ms    = static_cast<DWORD>(dps_window_secs_ * 1000);
    const DWORD window_start = now - window_ms;
    int window_dmg = 0;
    for (const auto& e : events_)
      if (e.tick_ms >= window_start)
        window_dmg += e.damage;
    current_dps_ = static_cast<float>(window_dmg) / static_cast<float>(dps_window_secs_);
    // Peak = the highest the rolling-window DPS ever reached this fight, so it
    // stays consistent with the "current DPS" readout. (The old per-slot peak
    // used the short slot window scaled by 1000/slot_ms, so one burst hit in a
    // 200ms slot inflated it ~5x and never matched the displayed DPS.)
    if (in_combat_ && current_dps_ > peak_dps_) peak_dps_ = current_dps_;
  }

  if (!visible_) return;

  // Le titre porte le DPS même repliée ; `###DPS Meter` fige l'identifiant de la
  // fenêtre pour que sa position survive au changement de libellé.
  char title_dps[32];
  FormatBig(static_cast<long long>(current_dps_ + 0.5f), title_dps, sizeof(title_dps));
  char title[80];
  std::snprintf(title, sizeof(title), i18n::Tr("DPS Meter — %s DPS###DPS Meter"),
                title_dps);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ro::Px(8.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ro::Px(3.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, ro::Px(6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ro::Px(9.0f), ro::Px(8.0f)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, ro::Px(5.0f)));

  // ── Le chrome, accordé à la couleur du joueur ──────────────────────────────
  // La fenêtre est un HUD, pas un panneau : fond sombre (le doré du compteur ne
  // tient pas sur le corps clair du skin RO) et barre de titre teintée de la
  // couleur choisie, pour qu'elle appartienne visiblement au compteur.
  const ImVec4 acc(text_color_[0], text_color_[1], text_color_[2], 1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.055f, 0.070f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_TitleBg,
                        ImVec4(acc.x * 0.16f, acc.y * 0.16f, acc.z * 0.16f, bg_alpha_));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive,
                        ImVec4(acc.x * 0.26f, acc.y * 0.26f, acc.z * 0.26f, bg_alpha_));
  ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed,
                        ImVec4(acc.x * 0.16f, acc.y * 0.16f, acc.z * 0.16f, bg_alpha_ * 0.85f));
  // Le liseré suit l'opacité du fond : une bordure pleine autour d'une fenêtre
  // presque transparente dessinerait un rectangle flottant dans le décor.
  ImGui::PushStyleColor(ImGuiCol_Border,
                        ImVec4(acc.x * 0.55f, acc.y * 0.55f, acc.z * 0.55f, bg_alpha_ * 0.45f));
  constexpr int kStyleColors = 5;
  constexpr int kStyleVars   = 6;
  ImGuiWindowFlags flags = 0;
  if (locked_) {
    flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;  // freeze
    // Stay click-through over the body, but keep the collapse arrow clickable:
    // only add NoInputs when the mouse is NOT over the title bar (rect captured
    // last frame). With NoMove, the title bar's only live control is the arrow.
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const bool over_title =
        mp.x >= lock_title_rect_[0] && mp.x < lock_title_rect_[2] &&
        mp.y >= lock_title_rect_[1] && mp.y < lock_title_rect_[3];
    if (!over_title) flags |= ImGuiWindowFlags_NoInputs;  // body = click-through
  }
  ImGui::SetNextWindowSize(ImVec2(ro::Px(286.0f), ro::Px(196.0f)), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(bg_alpha_);
  bool open = true;
  ImGui::Begin(title, locked_ ? nullptr : &open, flags);
  if (!locked_) bourgeon::CloseWindowOnEscape(open);
  if (!open) {
    visible_ = false;
    ImGui::End();
    ImGui::PopStyleColor(kStyleColors);
    ImGui::PopStyleVar(kStyleVars);
    return;
  }
  // Record the title-bar rect for next frame's locked collapse-arrow hit-test.
  if (locked_) {
    const ImVec2 wp = ImGui::GetWindowPos();
    lock_title_rect_[0] = wp.x;
    lock_title_rect_[1] = wp.y;
    lock_title_rect_[2] = wp.x + ImGui::GetWindowSize().x;
    lock_title_rect_[3] = wp.y + ImGui::GetFrameHeight();
  }

  // ── Répartition de la hauteur ──────────────────────────────────────────────
  // L'en-tête et les tuiles ont une hauteur DICTÉE par leur texte ; la courbe
  // prend tout ce qui reste. C'est elle qu'on agrandit en tirant la fenêtre, et
  // c'est bien elle qu'on veut plus grande.
  const float base_px  = ImGui::GetFontSize();
  const float spacing  = ImGui::GetStyle().ItemSpacing.y;
  // Plancher de largeur : le graphique se soumet en `InvisibleButton`, dont
  // ImGui refuse une taille nulle — et une fenêtre resserrée jusqu'à sa taille
  // minimale peut rendre une région disponible négative une fois les marges
  // déduites.
  const float width    = std::max(ImGui::GetContentRegionAvail().x, ro::Px(24.0f));
  const float avail_h  = ImGui::GetContentRegionAvail().y;
  const float header_h = base_px * kBigTextScale + ro::Px(6.0f);
  const float stats_h  = base_px * (1.0f + kLabelTextScale) + ro::Px(8.0f);

  // Les tuiles s'effacent quand la fenêtre devient trop basse pour les porter
  // SANS écraser la courbe : un compteur réduit à sa plus simple expression doit
  // garder son chiffre et sa forme, pas trois bilans sur un trait.
  const float graph_min = ro::Px(30.0f);
  const bool with_stats =
      (avail_h - header_h - stats_h - spacing * 2.0f) >= graph_min;
  float graph_h = avail_h - header_h - spacing -
                  (with_stats ? stats_h + spacing : 0.0f);
  graph_h = std::max(graph_h, ro::Px(18.0f));

  DrawHeader(width, header_h, now);
  DrawGraph(width, graph_h);
  if (with_stats) DrawStatsRow(width, stats_h, now);

  ImGui::End();
  ImGui::PopStyleColor(kStyleColors);
  ImGui::PopStyleVar(kStyleVars);
}

// ── Panneau de réglages ──────────────────────────────────────────────────────
// Il vivait dans moonlight_ui/panel_fun.cc, à quarante-huit lignes de ses neuf
// membres — ce sont eux que le commentaire « read/written by MoonlightUi » du
// header décrivait. Le plugin dessine maintenant sa propre section ; MoonlightUi
// n'en garde que la place dans la fenêtre et la sauvegarde.
bool DpsMeter::DrawSettings() {
  bool changed = false;

  changed |= ro::RoCheckbox(i18n::Tr("Afficher"), &visible_);
  changed |= ro::RoCheckbox(i18n::Tr("Verrouiller (fige + clic-traversant)"), &locked_);
  SameLine();
  HelpMarker(i18n::Tr("Fige la fenêtre DPS (position/taille) et laisse passer les clics "
             "au jeu en dessous."));
  changed |= ColorEdit4WithAlphaBar(i18n::Tr("Couleur texte"),  text_color_);
  changed |= ColorEdit4WithAlphaBar(i18n::Tr("Couleur graphe"), plot_color_);

  ImGui::PushItemWidth(160.0f);  // sliders étroits, pour tenir dans la fenêtre
  changed |= WheelSliderFloat(i18n::Tr("Opacité fond"), &bg_alpha_, 0.0f, 1.0f);

  // slot_ms_ passe par une copie : le changer invalide l'historique, et
  // ResetHistory ne doit être appelée qu'une fois, sur un vrai changement.
  int slot_ms = slot_ms_;
  if (WheelSliderInt(i18n::Tr("Résolution (ms/slot)"), &slot_ms, 50, 2000)) {
    slot_ms_ = slot_ms;
    ResetHistory();
    changed = true;
  }
  SameLine();
  HelpMarker(i18n::Tr("Largeur de chaque colonne du graphique en millisecondes.\n"
             "Valeur plus basse = graphique plus précis mais moins smooth."));

  int window_secs = dps_window_secs_;
  if (WheelSliderInt(i18n::Tr("Fenêtre DPS (s)"), &window_secs, 1, 30)) {
    dps_window_secs_ = window_secs;
    changed = true;
  }
  SameLine();
  HelpMarker(i18n::Tr("Fenêtre de temps pour calculer le DPS courant affiché."));

  int combat_timeout = combat_timeout_secs_;
  if (WheelSliderInt(i18n::Tr("Timeout combat (s)"), &combat_timeout, 1, 15)) {
    combat_timeout_secs_ = combat_timeout;
    changed = true;
  }
  SameLine();
  HelpMarker(i18n::Tr("Secondes sans dégâts avant de quitter le mode combat."));

  ImGui::PopItemWidth();

  if (ro::RoButton(i18n::Tr("Reset graphique"))) ResetHistory();

  ImGui::Separator();
  changed |= ro::RoCheckbox(i18n::Tr("Afficher dommages de sorts de zone dans le chat"),
                            &show_ground_dmg_in_chat_);
  SameLine();
  HelpMarker(i18n::Tr("Affiche chaque coup de Storm Gust / Meteor Storm / LoV etc. dans "
             "le chat.\nMessage custom Bourgeon — le serveur ne montre pas ces "
             "dégâts dans le chat habituel."));

  return changed;
}
