#include "plugins/item_desc_tweaks.h"

#include <Windows.h>
#include <cstdlib>

#include "bourgeon.h"
#include "imgui.h"
#include "utils/log_console.h"

// ── Constantes RE (cf. mémoire project_item_skill_desc_window_re) ───────────
namespace {

// Pointeur live de la fenêtre de description active (g_UIWindowMgr+0x218).
// Écrit par le jeu à l'ouverture, remis à 0 à la fermeture. null = fermée.
constexpr uintptr_t kDescWndLivePtr = 0x0131f700;

// Offsets dans l'objet fenêtre desc (id 0xc, vtable 0x01032aac, base UIWindow).
constexpr uintptr_t kOffWidth   = 0x14;   // int  largeur
constexpr uintptr_t kOffHeight  = 0x18;   // int  hauteur
constexpr uintptr_t kOffPosX    = 0x1c;   // int  x écran
constexpr uintptr_t kOffPosY    = 0x20;   // int  y écran
constexpr uintptr_t kOffIdStr   = 0xe4;   // std::string id (SSO)
constexpr uintptr_t kOffIdCap   = 0xf8;   // capacité de la std::string id
constexpr uintptr_t kOffIsSkill = 0x114;  // byte : 1 = mode skill

// ── Couche serveur (DÉSACTIVÉE — squelette non-intrusif) ────────────────────
// Mettre à true + enregistrer l'opcode pour activer la récupération réelle.
constexpr bool     kEnableServerFetch = false;
// Opcodes custom PROPOSÉS (libres : actuels 0x0BFD/0x0BFE/0x0C20..0x0C22 ;
// table dispatch recv max 0x0C35 — cf. mémoire project_opcode_system).
constexpr uint16_t kOpcodeReqTechData = 0x0C23;  // client -> serveur (requête)
constexpr uint16_t kOpcodeTechData    = 0x0C24;  // serveur -> client (réponse)

// Lecture read-only, SEH-gardée, de l'état de la fenêtre desc native.
// Renvoie false si la lecture a fauté (pointeur invalide) — on ignore alors.
struct DescSnapshot {
  bool     open = false;
  uint32_t id = 0;
  bool     is_skill = false;
  int      x = 0, y = 0, w = 0, h = 0;
};

bool ReadDescSnapshot(DescSnapshot* out) {
  __try {
    auto* wnd = *reinterpret_cast<uint8_t**>(kDescWndLivePtr);
    if (wnd == nullptr) return true;  // open reste false

    const char* idstr;
    if (*reinterpret_cast<uint32_t*>(wnd + kOffIdCap) > 0xf)
      idstr = *reinterpret_cast<char**>(wnd + kOffIdStr);
    else
      idstr = reinterpret_cast<const char*>(wnd + kOffIdStr);

    const long id = (idstr != nullptr) ? std::atol(idstr) : 0;
    if (id <= 0) return true;

    out->open     = true;
    out->id       = static_cast<uint32_t>(id);
    out->is_skill = *reinterpret_cast<uint8_t*>(wnd + kOffIsSkill) != 0;
    out->x        = *reinterpret_cast<int*>(wnd + kOffPosX);
    out->y        = *reinterpret_cast<int*>(wnd + kOffPosY);
    out->w        = *reinterpret_cast<int*>(wnd + kOffWidth);
    out->h        = *reinterpret_cast<int*>(wnd + kOffHeight);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

}  // namespace

ItemDescTweaks::ItemDescTweaks() {
  // Non-intrusif : on n'enregistre l'opcode de réception que si la couche
  // serveur est activée (branche morte tant que kEnableServerFetch == false).
  if (kEnableServerFetch) {
    Bourgeon::Instance().RegisterRecvOpcode(kOpcodeTechData);
  }
}

void ItemDescTweaks::OnTick() {
  DescSnapshot snap;
  if (!ReadDescSnapshot(&snap)) return;  // lecture fautée -> on garde l'état

  desc_open_ = snap.open;
  if (!snap.open) {
    current_id_ = 0;
    return;
  }

  desc_x_ = snap.x;  desc_y_ = snap.y;
  desc_w_ = snap.w;  desc_h_ = snap.h;

  // Nouvel item/skill affiché -> déclenche (à terme) une requête serveur.
  if (snap.id != current_id_ || snap.is_skill != current_is_skill_) {
    current_id_       = snap.id;
    current_is_skill_ = snap.is_skill;
    RequestTechData(current_id_, current_is_skill_);
  }
}

void ItemDescTweaks::RequestTechData(uint32_t id, bool is_skill) {
  auto& entry = cache_[id];
  if (entry.state == FetchState::kReady || entry.state == FetchState::kPending)
    return;  // déjà en cache / en vol
  entry.is_skill = is_skill;

  if (!kEnableServerFetch) {
    entry.state = FetchState::kNone;  // squelette : aucune requête envoyée
    return;
  }

  // TODO (couche serveur) : [opcode:2][total_len:2][id:4][is_skill:1]
  uint8_t pkt[9];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpcodeReqTechData;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(sizeof(pkt));
  *reinterpret_cast<uint32_t*>(pkt + 4) = id;
  pkt[8] = is_skill ? 1 : 0;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  entry.state          = FetchState::kPending;
  entry.requested_tick = GetTickCount();
}

void ItemDescTweaks::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                  uint16_t len) {
  if (opcode != kOpcodeTechData) return;
  // TODO (couche serveur) : parser la vraie structure technique.
  // Format provisoire attendu : [id:4][payload...].
  if (len < 4) return;
  const uint32_t id = *reinterpret_cast<const uint32_t*>(data);
  auto& entry = cache_[id];
  entry.state = FetchState::kReady;
  entry.raw.assign(reinterpret_cast<const char*>(data + 4), len - 4);
}

void ItemDescTweaks::OnRenderUI() {
  if (!enabled_ || !desc_open_ || current_id_ == 0) return;

  // Ancré à DROITE de la fenêtre native (coordonnées backbuffer = mêmes que
  // celles de l'ImGui). C'est ce panneau qui, à terme, remplacera/augmentera
  // la fenêtre native — pour l'instant il ne fait que SE SUPERPOSER.
  ImGui::SetNextWindowPos(
      ImVec2(static_cast<float>(desc_x_ + desc_w_ + 8),
             static_cast<float>(desc_y_)),
      ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.92f);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("Infos techniques (placeholder)##itemdesc", nullptr, flags)) {
    ImGui::Text("%s id : %u", current_is_skill_ ? "Skill" : "Item", current_id_);
    ImGui::Separator();

    const auto it = cache_.find(current_id_);
    FetchState st = (it != cache_.end()) ? it->second.state : FetchState::kNone;

    switch (st) {
      case FetchState::kPending:
        ImGui::TextDisabled("Chargement depuis le serveur...");
        break;
      case FetchState::kReady:
        // TODO : afficher les vrais champs techniques parsés.
        ImGui::TextUnformatted("Données serveur reçues (parsing à implémenter).");
        break;
      case FetchState::kFailed:
        ImGui::TextDisabled("Echec de la requete serveur.");
        break;
      case FetchState::kNone:
      default:
        ImGui::TextDisabled("Couche serveur desactivee (squelette).");
        break;
    }

    ImGui::Separator();
    // Lignes techniques PLACEHOLDER (à remplacer par les données réelles).
    if (current_is_skill_) {
      ImGui::Text("Cout SP / niveau : --");
      ImGui::Text("Cast / Cooldown  : --");
      ImGui::Text("Formule degats   : --");
    } else {
      ImGui::Text("ATK / MATK reels : --");
      ImGui::Text("Bonus script     : --");
      ImGui::Text("Sources de drop  : --");
    }
  }
  ImGui::End();
}
