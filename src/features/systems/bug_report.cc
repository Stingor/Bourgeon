#include "features/systems/bug_report.h"

#include <Windows.h>

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <vector>

#include "bourgeon.h"        // Bourgeon::Instance().SendPacket / RegisterRecvOpcode
#include "imgui.h"
#include "features/systems/bourgeon_opcodes.h"
#include "ui/ro_imgui.h"     // ro::RoButton (bouton habillé skin RO)

namespace {

constexpr uint32_t kSendThrottleMs = 3000;  // 1 rapport / 3 s côté client (le
                                            // serveur applique aussi son propre
                                            // rate-limit, autoritatif).
constexpr size_t   kMaxMsgBytes    = 500;   // borne message (aligné VARCHAR(512)
                                            // serveur, marge pour l'UTF-8).
constexpr uint32_t kAckToastMs     = 5000;  // durée d'affichage du toast d'accusé.

// Échappe une valeur pour l'insérer dans notre mini-JSON de contexte. On reste
// minimalistes : guillemets, backslash et contrôles. Suffisant pour des noms
// d'objets/NPC (le serveur stocke la chaîne verbatim, il ne la ré-exécute pas).
// Convertit une chaîne UTF-8 (ImGui produit toujours de l'UTF-8) en CP1252.
// TOUT le pipeline serveur (chat in-game, DB, relais Discord) est en latin1 : la
// connexion MySQL du map-server n'émet pas de `SET NAMES`, donc les octets sont
// traités comme latin1. Envoyer de l'UTF-8 brut donnerait un double-encodage
// (« é » -> « Ã© ») sur Discord et le site. On s'aligne donc sur l'encodage du
// client (CP1252, comme le chat). Caractères hors CP1252 (emoji…) -> '?'.
std::string Utf8ToCp1252(const std::string& in) {
  if (in.empty()) return std::string();
  const int wn = MultiByteToWideChar(CP_UTF8, 0, in.c_str(),
                                     static_cast<int>(in.size()), nullptr, 0);
  if (wn <= 0) return in;
  std::wstring w(static_cast<size_t>(wn), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()), &w[0],
                      wn);
  const int cn = WideCharToMultiByte(1252, 0, w.c_str(), wn, nullptr, 0, nullptr,
                                     nullptr);
  if (cn <= 0) return in;
  std::string out(static_cast<size_t>(cn), '\0');
  WideCharToMultiByte(1252, 0, w.c_str(), wn, &out[0], cn, nullptr, nullptr);
  return out;
}

std::string JsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20)
          out += ' ';  // autres contrôles -> espace
        else
          out += c;    // UTF-8 (accents inclus) passe tel quel
    }
  }
  return out;
}

}  // namespace

BugReport::BugReport() {
  // ACK serveur (ZC 0x0F14) : zone custom sûre (>0x0C35) => livré par le
  // reader-hook. cf. bourgeon_opcodes.h.
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kBugReportAck);
}

// --- Constructeurs de contexte ---------------------------------------------

BugReport::Context BugReport::ItemContext(uint32_t item_id,
                                                      const std::string& name,
                                                      int refine) {
  Context c;
  c.category = kItem;
  char lbl[160];
  std::snprintf(lbl, sizeof(lbl), "Objet : %s (#%u)",
                name.empty() ? "?" : name.c_str(), item_id);
  c.label = lbl;
  char js[256];
  if (refine >= 0)
    std::snprintf(js, sizeof(js), "{\"item_id\":%u,\"name\":\"%s\",\"refine\":%d}",
                  item_id, JsonEscape(name).c_str(), refine);
  else
    std::snprintf(js, sizeof(js), "{\"item_id\":%u,\"name\":\"%s\"}",
                  item_id, JsonEscape(name).c_str());
  c.json = js;
  return c;
}

BugReport::Context BugReport::SkillContext(uint32_t skill_id,
                                                       const std::string& name,
                                                       int level) {
  Context c;
  c.category = kSkill;
  char lbl[160];
  std::snprintf(lbl, sizeof(lbl), "Compétence : %s (#%u)",
                name.empty() ? "?" : name.c_str(), skill_id);
  c.label = lbl;
  char js[256];
  if (level >= 0)
    std::snprintf(js, sizeof(js),
                  "{\"skill_id\":%u,\"name\":\"%s\",\"level\":%d}", skill_id,
                  JsonEscape(name).c_str(), level);
  else
    std::snprintf(js, sizeof(js), "{\"skill_id\":%u,\"name\":\"%s\"}", skill_id,
                  JsonEscape(name).c_str());
  c.json = js;
  return c;
}

BugReport::Context BugReport::NpcContext(uint32_t gid,
                                                     const std::string& name) {
  Context c;
  c.category = kNpc;
  char lbl[160];
  std::snprintf(lbl, sizeof(lbl), "PNJ : %s (GID %u)",
                name.empty() ? "?" : name.c_str(), gid);
  c.label = lbl;
  char js[256];
  std::snprintf(js, sizeof(js), "{\"npc_gid\":%u,\"name\":\"%s\"}", gid,
                JsonEscape(name).c_str());
  c.json = js;
  return c;
}

BugReport::Context BugReport::GenericContext() {
  Context c;
  c.category = kGeneric;
  c.label = "Rapport général (aucun contexte spécifique)";
  c.json = "{}";
  return c;
}

// --- API partagée ----------------------------------------------------------

void BugReport::Button(const Context& ctx, const char* imgui_id) {
  if (!enabled_) return;  // opt-out via MoonlightUi
  ImGui::PushID(imgui_id);
  // Bouton habillé RO (pièces btn_* du client) : se fond dans les fenêtres desc /
  // dialogue PNJ où il est posé (toutes des fenêtres RO à fond clair).
  if (ro::RoButton("Signaler un bug")) Open(ctx);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Envoyer un rapport de bug à l'équipe (le contexte est "
                      "joint automatiquement)");
  ImGui::PopID();
}

void BugReport::Open(const Context& ctx) {
  ctx_ = ctx;
  msg_buf_[0] = '\0';
  want_open_ = true;
}

// --- Envoi -----------------------------------------------------------------

void BugReport::SendReport(const Context& ctx, const std::string& message) {
  // CZ_BOURGEON_BUG_REPORT (0x0F13), variable :
  //   [type:2][len:2][category:1][ctx_len:2][ctx:ctx_len][message: reste]
  // Identité/map/position ajoutées côté serveur depuis la session.
  // MESSAGE : saisi dans ImGui -> UTF-8. On le convertit en CP1252 pour matcher le
  // pipeline serveur latin1 (sinon accents en double-encodage « Ã© » sur Discord/
  // site). CP1252 est mono-octet : la troncature ne coupe jamais un caractère.
  std::string msg = Utf8ToCp1252(message);
  if (msg.size() > kMaxMsgBytes) msg.resize(kMaxMsgBytes);
  // CONTEXTE : les noms (item/skill/NPC) proviennent des getters natifs du jeu et
  // sont DÉJÀ en CP1252 (jamais convertis en UTF-8 côté capture) ; la structure JSON
  // est ASCII. On l'envoie donc tel quel — le convertir casserait les noms accentués.
  const std::string& js = ctx.json;
  const uint16_t ctx_len = static_cast<uint16_t>(
      js.size() > 0xFFFF ? 0xFFFF : js.size());

  const size_t total = 7 + ctx_len + msg.size();
  std::vector<uint8_t> pkt(total);
  *reinterpret_cast<uint16_t*>(&pkt[0]) = bopcodes::kBugReport;
  *reinterpret_cast<uint16_t*>(&pkt[2]) = static_cast<uint16_t>(total);
  pkt[4] = ctx.category;
  *reinterpret_cast<uint16_t*>(&pkt[5]) = ctx_len;
  if (ctx_len) std::memcpy(&pkt[7], js.data(), ctx_len);
  if (!msg.empty()) std::memcpy(&pkt[7 + ctx_len], msg.data(), msg.size());

  Bourgeon::Instance().SendPacket(pkt.data(), pkt.size());
  last_send_tick_ = GetTickCount();
}

// --- Rendu -----------------------------------------------------------------

void BugReport::OnRenderUI() {
  // Raccourci global Ctrl+Alt+B : rapport générique (uniquement si le jeu a le
  // focus clavier ImGui et qu'aucune modale n'est déjà ouverte).
  ImGuiIO& io = ImGui::GetIO();
  if (enabled_ && !modal_open_ && io.KeyCtrl && io.KeyAlt &&
      ImGui::IsKeyPressed(ImGuiKey_B, false)) {
    Open(GenericContext());
  }

  if (want_open_) {
    ImGui::OpenPopup("Signaler un bug###bug_report_modal");
    want_open_ = false;
    modal_open_ = true;
  }

  RenderModal();
  RenderAckToast();
}

void BugReport::RenderModal() {
  // Centre la modale au premier affichage.
  ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);

  if (!ImGui::BeginPopupModal("Signaler un bug###bug_report_modal", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    modal_open_ = false;  // fermé -> réarme le raccourci
    return;
  }

  // Tant que la modale est ouverte, elle capte Échap pour elle seule : on neutralise
  // la pile Échap RO ce frame (sinon Échap fermerait aussi la fenêtre desc derrière).
  ro::SuppressEscapeStack();

  // Échap = Annuler. On le gère nous-mêmes car le champ de saisie multiligne
  // consomme Échap (désactivation du champ) et l'empêche de fermer le popup. Le
  // IsKeyPressed « brut » ignore l'ownership du InputText -> marche aussi quand le
  // champ a le focus. CloseCurrentPopup ferme au frame suivant (EndPopup normal).
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    modal_open_ = false;
    ImGui::CloseCurrentPopup();
  }

  ImGui::TextWrapped("Décris brièvement le problème. Le contexte ci-dessous "
                     "est joint automatiquement à ton rapport.");
  ImGui::Separator();

  // Contexte capturé (lecture seule).
  ImGui::TextDisabled("Contexte");
  ImGui::TextWrapped("%s", ctx_.label.c_str());
  ImGui::Spacing();

  ImGui::TextDisabled("Ton message");
  ImGui::InputTextMultiline("##bug_msg", msg_buf_, sizeof(msg_buf_),
                            ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5));
  ImGui::TextDisabled("%zu / %zu caractères", std::strlen(msg_buf_),
                      kMaxMsgBytes);

  ImGui::Separator();

  const bool has_msg = msg_buf_[0] != '\0';
  const uint32_t now = GetTickCount();
  const bool throttled = (now - last_send_tick_) < kSendThrottleMs;

  ImGui::BeginDisabled(!has_msg || throttled);
  if (ImGui::Button("Envoyer", ImVec2(120, 0))) {
    SendReport(ctx_, msg_buf_);
    modal_open_ = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  if (throttled && ImGui::IsItemHovered())
    ImGui::SetTooltip("Patiente quelques secondes avant un nouveau rapport.");

  ImGui::SameLine();
  if (ImGui::Button("Annuler", ImVec2(120, 0))) {
    modal_open_ = false;
    ImGui::CloseCurrentPopup();
  }

  ImGui::EndPopup();
}

void BugReport::RenderAckToast() {
  if (ack_status_ == 0xFF) return;
  const uint32_t now = GetTickCount();
  const uint32_t elapsed = now - ack_tick_;
  if (elapsed > kAckToastMs) {
    ack_status_ = 0xFF;
    return;
  }

  // Fondu entrant (150 ms) + sortant (dernier quart de la durée).
  float alpha = 1.0f;
  const uint32_t fade_start = kAckToastMs * 3 / 4;
  if (elapsed < 150)
    alpha = float(elapsed) / 150.0f;
  else if (elapsed > fade_start)
    alpha = 1.0f - float(elapsed - fade_start) / float(kAckToastMs - fade_start);
  if (alpha < 0.0f) alpha = 0.0f;
  if (alpha > 1.0f) alpha = 1.0f;

  const char* title;
  const char* text;
  ImU32 accent;
  switch (ack_status_) {
    case 0:
      title = "Rapport envoyé";
      text  = "Merci ! Ton rapport de bug a bien été enregistré.";
      accent = IM_COL32(90, 205, 105, 255); break;
    case 1:
      title = "Trop rapide";
      text  = "Patiente un instant avant d'envoyer un nouveau rapport.";
      accent = IM_COL32(238, 190, 90, 255); break;
    default:
      title = "Échec de l'envoi";
      text  = "Le rapport n'a pas pu être enregistré (erreur serveur).";
      accent = IM_COL32(232, 100, 100, 255); break;
  }

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  const float base = ImGui::GetFontSize();
  const float title_sz = base * 1.45f;   // titre bien plus gros que le texte du jeu
  const float body_sz  = base * 1.10f;
  const ImVec2 td = font->CalcTextSizeA(title_sz, FLT_MAX, 0.0f, title);
  const ImVec2 bd = font->CalcTextSizeA(body_sz, FLT_MAX, 0.0f, text);

  const float pad = 18.0f, gap = 7.0f, accent_w = 7.0f;
  const float content_w = (td.x > bd.x ? td.x : bd.x);
  const float box_w = accent_w + pad + content_w + pad;
  const float box_h = pad + td.y + gap + bd.y + pad;

  const ImVec2 vp = ImGui::GetMainViewport()->Size;
  // Centré horizontalement, dans le tiers supérieur (zone de regard, pas au ras du bas).
  const float bx = (vp.x - box_w) * 0.5f;
  const float by = vp.y * 0.15f - (1.0f - alpha) * 10.0f;  // léger glissement à l'entrée
  const ImVec2 p0(bx, by), p1(bx + box_w, by + box_h);
  const int A = int(255 * alpha);

  // Ombre portée -> décolle du décor.
  dl->AddRectFilled(ImVec2(p0.x + 5, p0.y + 6), ImVec2(p1.x + 5, p1.y + 6),
                    IM_COL32(0, 0, 0, int(110 * alpha)), 9.0f);
  // Fond opaque + contour + barre d'accent gauche colorée selon le statut.
  dl->AddRectFilled(p0, p1, IM_COL32(28, 30, 35, int(248 * alpha)), 9.0f);
  dl->AddRect(p0, p1, IM_COL32(255, 255, 255, int(45 * alpha)), 9.0f, 0, 1.5f);
  const ImU32 accent_a = (accent & 0x00FFFFFF) | (ImU32(A) << 24);
  dl->AddRectFilled(p0, ImVec2(p0.x + accent_w, p1.y), accent_a, 9.0f,
                    ImDrawFlags_RoundCornersLeft);

  // Titre (couleur du statut) + détail (blanc cassé).
  const float tx = p0.x + accent_w + pad;
  dl->AddText(font, title_sz, ImVec2(tx, p0.y + pad), accent_a, title);
  dl->AddText(font, body_sz, ImVec2(tx, p0.y + pad + td.y + gap),
              IM_COL32(236, 236, 236, A), text);
}

void BugReport::OnRecvPacket(uint16_t opcode, 
                                   const uint8_t* data,
                                   uint16_t len) {
  if (opcode != bopcodes::kBugReportAck) return;
  // Payload après header [type:2][len:2] : [status:1].
  ack_status_ = (len >= 1) ? data[0] : 2;
  ack_tick_ = GetTickCount();
}
