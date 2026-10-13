#include "features/overlays/instance_info.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "imgui.h"
#include "bourgeon.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace {

// ── Les paquets standard, OBSERVÉS (data = juste après l'opcode) ─────────────
// 02CB <nom>.61B <position>.W                      (clif_instance_create)
// 02CC <position>.W ; 0xFFFF = réservation annulée  (clif_instance_changewait)
// 02CD <nom>.61B <fin de vie>.L <fermeture à vide>.L (clif_instance_status)
// 02CE <type>.L <limite>.L                          (clif_instance_changestatus)
constexpr uint16_t kOpCreated = 0x02CB;
constexpr uint16_t kOpWait    = 0x02CC;
constexpr uint16_t kOpStatus  = 0x02CD;
constexpr uint16_t kOpChange  = 0x02CE;
constexpr int kNameBytes      = 61;
constexpr uint16_t kCreatedLen = kNameBytes + 2;
constexpr uint16_t kWaitLen    = 2;
constexpr uint16_t kStatusLen  = kNameBytes + 8;
constexpr uint16_t kChangeLen  = 8;
constexpr uint16_t kWaitCancelled = 0xFFFF;
// Type 0 de 02CE : la fermeture à vide change. Les autres détruisent.
constexpr uint32_t kChangeNotify = 0;

// ── ZC_MOONLIGHT_INSTANCE_INFO 0x0F40 (data = après [opcode:2][len:2]) ───────
// server_time.L duration.L idle_timeout.L hardmode.W turbo.B mode.B
// destroyable.B may_destroy.B on_instance_map.B players_inside.W owner.24B
constexpr uint16_t kInfoLen       = 45;
constexpr int kOwnerBytes         = 24;

// Les modes d'instance du serveur (e_instance_mode).
constexpr uint8_t kOwnerChar  = 1;
constexpr uint8_t kOwnerParty = 2;
constexpr uint8_t kOwnerGuild = 3;
constexpr uint8_t kOwnerClan  = 4;

// Les niveaux du menu de difficulté, et leur nom.
struct HardModeEntry { int level; const char* name; };
const HardModeEntry kHardModeNames[] = {
    {1, "Facile"}, {2, "Normal"}, {3, "Moyen"},
    {5, "Difficile"}, {7, "Très difficile"}, {10, "Malade"},
};
constexpr int kHardModeEasiest = 1;
constexpr int kHardModeHardest = 10;

constexpr int64_t kSecondsPerMinute = 60;
constexpr int64_t kSecondsPerHour   = 3600;
// Sous ce seuil, une échéance se lit en rouge.
constexpr int64_t kUrgentSeconds    = 5 * kSecondsPerMinute;

const ImVec4 kEasyColor{0.35f, 0.80f, 0.35f, 1.0f};
const ImVec4 kHardColor{0.95f, 0.25f, 0.20f, 1.0f};
const ImVec4 kTurboColor{1.00f, 0.70f, 0.15f, 1.0f};
const ImVec4 kQuietColor{0.60f, 0.60f, 0.60f, 1.0f};

constexpr float kPanelWidth = 300.0f;
constexpr float kGaugeLines = 0.5f;

inline uint16_t U16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t U32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline int16_t I16(const uint8_t* p) { int16_t v; std::memcpy(&v, p, 2); return v; }

// Un nom fixe du fil, borné, en UTF-8.
std::string FixedName(const uint8_t* p, int bytes) {
  char buf[kNameBytes + 1] = {};
  std::memcpy(buf, p, static_cast<size_t>(std::min(bytes, kNameBytes)));
  return ro::WireToUtf8(buf);
}

const char* HardModeName(int level) {
  for (const HardModeEntry& e : kHardModeNames)
    if (e.level == level) return i18n::Tr(e.name);
  return nullptr;
}

ImVec4 HardModeColor(int level) {
  const float span = static_cast<float>(kHardModeHardest - kHardModeEasiest);
  const float t = std::clamp(static_cast<float>(level - kHardModeEasiest) / span, 0.0f, 1.0f);
  return ImVec4(kEasyColor.x + (kHardColor.x - kEasyColor.x) * t,
                kEasyColor.y + (kHardColor.y - kEasyColor.y) * t,
                kEasyColor.z + (kHardColor.z - kEasyColor.z) * t, 1.0f);
}

void FormatDuration(int64_t seconds, char* out, size_t n) {
  const int64_t s = std::max<int64_t>(0, seconds);
  const int h = static_cast<int>(s / kSecondsPerHour);
  const int m = static_cast<int>((s % kSecondsPerHour) / kSecondsPerMinute);
  const int r = static_cast<int>(s % kSecondsPerMinute);
  if (h > 0)      std::snprintf(out, n, i18n::Tr("%d h %02d min %02d s"), h, m, r);
  else if (m > 0) std::snprintf(out, n, i18n::Tr("%d min %02d s"), m, r);
  else            std::snprintf(out, n, i18n::Tr("%d s"), r);
}

// Une échéance et sa jauge, qui se vide à mesure que le temps passe.
void Countdown(int64_t left, uint32_t total) {
  char text[64];
  FormatDuration(left, text, sizeof(text));
  if (left < kUrgentSeconds) ImGui::TextColored(kHardColor, "%s", text);
  else                       ImGui::TextUnformatted(text);
  if (total == 0) return;
  const float ratio = std::clamp(static_cast<float>(left) / static_cast<float>(total), 0.0f, 1.0f);
  ImGui::ProgressBar(ratio, ImVec2(-1.0f, ImGui::GetTextLineHeight() * kGaugeLines), "");
}

void Label(const char* text, float column) {
  ImGui::TextUnformatted(text);
  ImGui::SameLine(column);
}

}  // namespace

InstanceInfo::InstanceInfo() {
  auto& b = Bourgeon::Instance();
  b.RegisterRecvOpcode(kOpInstanceInfo);
  b.RegisterObserveOpcode(kOpCreated, kCreatedLen);
  b.RegisterObserveOpcode(kOpWait, kWaitLen);
  b.RegisterObserveOpcode(kOpStatus, kStatusLen);
  b.RegisterObserveOpcode(kOpChange, kChangeLen);
}

// Fil RÉSEAU : on copie, HandlePacket décode (cf. features/net_inbox.h).
void InstanceInfo::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

void InstanceInfo::Arrive(const std::string& name, Phase phase) {
  // Une instance qui paraît, s'ouvre ou en remplace une autre rouvre le panneau ;
  // le rappel du serveur à chaque carte, non.
  if (phase != phase_ || name != name_) open_ = true;
  if (name != name_) detailed_ = false;
  phase_ = phase;
  name_ = name;
}

void InstanceInfo::Reset() {
  phase_ = Phase::kNone;
  name_.clear();
  wait_position_ = 0;
  keep_limit_s_ = idle_limit_s_ = 0;
  detailed_ = false;
  open_ = false;
}

void InstanceInfo::HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  switch (opcode) {
    case kOpCreated:
      if (len < kCreatedLen) return;
      Arrive(FixedName(data, kNameBytes), Phase::kWaiting);
      wait_position_ = U16(data + kNameBytes);
      keep_limit_s_ = idle_limit_s_ = 0;
      return;
    case kOpWait:
      if (len < kWaitLen || phase_ == Phase::kNone) return;
      if (U16(data) == kWaitCancelled) Reset();
      else wait_position_ = U16(data);
      return;
    case kOpStatus:
      if (len < kStatusLen) return;
      Arrive(FixedName(data, kNameBytes), Phase::kOpen);
      wait_position_ = 0;
      keep_limit_s_ = U32(data + kNameBytes);
      idle_limit_s_ = U32(data + kNameBytes + 4);
      return;
    case kOpChange:
      if (len < kChangeLen || phase_ == Phase::kNone) return;
      if (U32(data) == kChangeNotify) idle_limit_s_ = U32(data + 4);
      else Reset();
      return;
    case kOpInstanceInfo: {
      if (len < kInfoLen) return;
      const int64_t now = static_cast<int64_t>(std::time(nullptr));
      server_ahead_s_ = static_cast<int64_t>(U32(data)) - now;
      duration_s_ = U32(data + 4);
      idle_timeout_s_ = U32(data + 8);
      hard_mode_ = I16(data + 12);
      turbo_ = data[14] != 0;
      owner_kind_ = data[15];
      destroyable_ = data[16] != 0;
      may_destroy_ = data[17] != 0;
      on_instance_map_ = data[18] != 0;
      players_inside_ = U16(data + 19);
      owner_name_ = FixedName(data + 21, kOwnerBytes);
      detailed_ = true;
      return;
    }
    default:
      return;
  }
}

void InstanceInfo::OnRenderUI() {
  if (phase_ == Phase::kNone || !open_) return;
  const int64_t server_now = static_cast<int64_t>(std::time(nullptr)) + server_ahead_s_;

  ImGui::SetNextWindowSize(ImVec2(kPanelWidth, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, 120.0f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.0f));
  char title[96];
  std::snprintf(title, sizeof(title), "%s###instance_info", i18n::Tr("Détail de l'instance"));
  if (ImGui::Begin(title, &open_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
    ImGui::TextUnformatted(name_.c_str());
    if (phase_ == Phase::kWaiting)
      ImGui::TextColored(kTurboColor, i18n::Tr("En préparation — place dans la file : %d"), wait_position_);
    else
      ImGui::TextColored(kEasyColor, "%s", i18n::Tr("Ouverte"));
    ImGui::Separator();

    const float column = ImGui::CalcTextSize(i18n::Tr("Joueurs présents :")).x +
                         ImGui::GetStyle().ItemSpacing.x * 2.0f;
    if (detailed_) {
      Label(i18n::Tr("Difficulté :"), column);
      if (const char* named = HardModeName(hard_mode_))
        ImGui::TextColored(HardModeColor(hard_mode_), i18n::Tr("HM %d · %s"), hard_mode_, named);
      else
        ImGui::TextColored(HardModeColor(hard_mode_), i18n::Tr("HM %d"), hard_mode_);
      HelpTooltip(i18n::Tr("Les monstres de l'instance sont renforcés selon ce niveau, et ses "
                           "récompenses croissent avec lui."));

      Label(i18n::Tr("Mode turbo :"), column);
      if (turbo_) ImGui::TextColored(kTurboColor, "%s", i18n::Tr("activé"));
      else        ImGui::TextColored(kQuietColor, "%s", i18n::Tr("désactivé"));
      HelpTooltip(i18n::Tr("En mode turbo, la plupart des dialogues de l'instance sont sautés."));

      const char* owner = i18n::Tr("Propriétaire");
      switch (owner_kind_) {
        case kOwnerChar:  owner = i18n::Tr("Personnage"); break;
        case kOwnerParty: owner = i18n::Tr("Groupe"); break;
        case kOwnerGuild: owner = i18n::Tr("Guilde"); break;
        case kOwnerClan:  owner = i18n::Tr("Clan"); break;
        default: break;
      }
      Label(owner, column);
      if (owner_name_.empty()) ImGui::TextColored(kQuietColor, "%s", i18n::Tr("inconnu"));
      else                     ImGui::TextUnformatted(owner_name_.c_str());

      if (phase_ == Phase::kOpen) {
        Label(i18n::Tr("Joueurs présents :"), column);
        ImGui::Text("%d", players_inside_);
      }
    }

    if (phase_ == Phase::kOpen) {
      Label(i18n::Tr("Temps restant :"), column);
      if (keep_limit_s_ == 0) {
        ImGui::TextColored(kQuietColor, "%s", i18n::Tr("sans limite"));
      } else {
        ImGui::BeginGroup();
        Countdown(static_cast<int64_t>(keep_limit_s_) - server_now, duration_s_);
        ImGui::EndGroup();
      }
      if (idle_limit_s_ != 0) {
        Label(i18n::Tr("Fermeture :"), column);
        ImGui::BeginGroup();
        Countdown(static_cast<int64_t>(idle_limit_s_) - server_now, idle_timeout_s_);
        ImGui::EndGroup();
        HelpTooltip(i18n::Tr("Personne n'est dans l'instance : elle sera détruite si personne "
                             "n'y entre avant la fin de ce délai."));
      }
    }

    // Le bouton reste celui de la fenêtre native (Alt+B) ; ici, on dit s'il
    // marchera — le serveur refuse en silence.
    if (detailed_ && destroyable_) {
      ImGui::Separator();
      const bool allowed = may_destroy_ && !on_instance_map_;
      const char* verdict =
          !may_destroy_ ? i18n::Tr("Seul le chef du groupe, le maître de la guilde ou le "
                                   "propriétaire peut détruire l'instance.")
          : on_instance_map_ ? i18n::Tr("Sortez de l'instance pour pouvoir la détruire.")
                             : i18n::Tr("Vous pouvez détruire l'instance depuis sa fenêtre (Alt+B).");
      ImGui::PushStyleColor(ImGuiCol_Text, allowed ? kEasyColor : kQuietColor);
      ImGui::TextWrapped("%s", verdict);
      ImGui::PopStyleColor();
    }
  }
  ImGui::End();
}
