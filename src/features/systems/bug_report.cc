#include "features/systems/bug_report.h"

#include <Windows.h>

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <vector>

#include "bourgeon.h"        // Bourgeon::Instance().SendPacket / RegisterRecvOpcode
#include "imgui.h"
#include "features/systems/bourgeon_opcodes.h"
#include "ui/ro_imgui.h"     // ro::RoButton / ro::RoSmallButton (skin RO)
#include "ui/ro_skin_blobs.hpp"  // dimensions sbtn_* / sys_close (bouton de titre)
#include "utils/i18n.h"
#include "ui/ui_palette.h"  // ro::pal : la palette de l'UI

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

// ── Le corps commun des contextes « une entité + un nom » ───────────────────
// Objet, compétence et PNJ composaient le même contexte : catégorie, libellé
// traduit « <Quoi> : <nom> (<id>) », et un json `{clé:id, name:"…"}`.
//
// 🔴 Ce qui compte ici n'est pas la longueur économisée, ce sont les DEUX RÈGLES
// que chaque copie devait se rappeler, et qu'un quatrième contexte aurait pu
// oublier sans que rien ne le signale :
//   · un nom vide devient « ? » dans le libellé — sinon « PNJ :  (GID 42) » ;
//   · le nom part ÉCHAPPÉ dans le json — un guillemet dans un nom d'objet
//     casserait le rapport côté serveur, silencieusement.
//
// `extra` est un fragment json DÉJÀ formaté (« ,"refine":7 ») ou vide : c'est la
// seule chose qui varie au-delà des trois paramètres, et l'écrire chez
// l'appelant garde la clé visible là où elle a un sens.
//
// ⚠ `label_fmt` vient de `i18n::Tr` : sa séquence de spécificateurs (%s puis %u)
// est un CONTRAT que le chargeur de traductions vérifie déjà.
BugReport::Context MakeEntityContext(uint8_t category, const char* label_fmt,
                                     const char* json_key, uint32_t id,
                                     const std::string& name,
                                     const char* extra = nullptr) {
  BugReport::Context c;
  c.category = category;
  char lbl[160];
  std::snprintf(lbl, sizeof(lbl), label_fmt, name.empty() ? "?" : name.c_str(), id);
  c.label = lbl;
  char js[256];
  std::snprintf(js, sizeof(js), "{\"%s\":%u,\"name\":\"%s\"%s}", json_key, id,
                JsonEscape(name).c_str(), extra ? extra : "");
  c.json = js;
  return c;
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
  char extra[32] = {0};
  if (refine >= 0) std::snprintf(extra, sizeof(extra), ",\"refine\":%d", refine);
  return MakeEntityContext(kItem, i18n::Tr("Objet : %s (#%u)"), "item_id",
                           item_id, name, extra);
}

BugReport::Context BugReport::SkillContext(uint32_t skill_id,
                                                       const std::string& name,
                                                       int level) {
  char extra[32] = {0};
  if (level >= 0) std::snprintf(extra, sizeof(extra), ",\"level\":%d", level);
  return MakeEntityContext(kSkill, i18n::Tr("Compétence : %s (#%u)"), "skill_id",
                           skill_id, name, extra);
}

BugReport::Context BugReport::NpcContext(uint32_t gid,
                                                     const std::string& name) {
  return MakeEntityContext(kNpc, i18n::Tr("PNJ : %s (GID %u)"), "npc_gid", gid,
                           name);
}

BugReport::Context BugReport::GenericContext() {
  Context c;
  c.category = kGeneric;
  c.label = i18n::Tr("Rapport général (aucun contexte spécifique)");
  c.json = "{}";
  return c;
}

// --- API partagée ----------------------------------------------------------

void BugReport::Button(const Context& ctx, const char* imgui_id) {
  if (!enabled_) return;  // opt-out via MoonlightUi
  ImGui::PushID(imgui_id);
  // Bouton habillé RO (pièces btn_* du client) : se fond dans les fenêtres desc /
  // dialogue PNJ où il est posé (toutes des fenêtres RO à fond clair).
  if (ro::RoButton(i18n::Tr("Signaler un bug"))) Open(ctx);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(i18n::Tr("Envoyer un rapport de bug à l'équipe (le contexte est "
                      "joint automatiquement)"));
  ImGui::PopID();
}

void BugReport::TitleBarButton(const Context& ctx) {
  if (!enabled_) return;  // opt-out via MoonlightUi
  // 🔴 Le littéral reste NU : il est `static`, donc construit au chargement de la
  // DLL — bien avant que le catalogue n'existe. Un `Tr` posé ici serait figé en
  // français pour toujours, sans la moindre erreur.
  static const char kLabel[] = "Signaler un bug";
  // ⚠ Traduit UNE fois, et la même chaîne sert à MESURER et à DESSINER. Mesurer le
  // français pour dessiner l'anglais donnerait un bouton mal placé — la largeur
  // calculée plus bas positionne le bouton dans la barre de titre.
  const char* label = i18n::Tr(kLabel);

  const ImVec2 wp = ImGui::GetWindowPos();  // = coin haut-gauche de la barre
  const float ww = ImGui::GetWindowWidth();
  const float title_h = ImGui::GetFrameHeight();  // hauteur de l'art de la barre
  // Largeur donnée EXPLICITEMENT (même formule que le mode auto de RoSmallButton)
  // pour la connaître avant de placer le curseur : la mesurer après coup ferait
  // sauter le bouton d'une frame à chaque ouverture — le défaut qu'on corrige.
  //
  // 🔴 LES CAPS PASSENT PAR `ro::Px`, EXACTEMENT COMME DANS RoSmallButton. Cette
  // formule DOIT rendre le même nombre que le mode auto du bouton : c'est elle
  // qui le positionne. Mesurée sur l'art non mis à l'échelle pendant que le
  // bouton, lui, se dessinait à l'échelle, elle le faisait déborder de la barre
  // de titre — le décalage visible dès que le réglage quittait 100 %.
  const float bw = ImGui::CalcTextSize(label).x +
                   ro::Px(static_cast<float>(ro::skin::ksBtnOutLeft.w +
                                             ro::skin::ksBtnOutRight.w));
  // La croix de fermeture vit à 5 px du bord droit : on lui laisse sa largeur plus
  // 4 px de respiration.
  const float bx = wp.x + ww -
                   ro::Px(static_cast<float>(ro::skin::kSysCloseOff.w) + 9.0f) - bw;
  // RoSmallButton peint son art 3 px SOUS le haut de son item : on remonte d'autant
  // pour que ce soit l'ART, et non l'item, qui soit centré dans la barre. Le +1 est
  // un ajustement optique — centré au pixel près, le bouton paraît haut.
  // (Le décalage de 3 px est `art_drop_y` chez RoSmallButton, à l'échelle lui
  // aussi : les deux valeurs doivent rester la même.)
  const float by =
      wp.y +
      (title_h - ro::Px(static_cast<float>(ro::skin::ksBtnOutLeft.h))) * 0.5f -
      ro::Px(3.0f) + ro::Px(1.0f);

  // 🔴 Le clip rect du corps EXCLUT la barre de titre. Sans ce PushClipRect, le
  // bouton ne serait pas seulement invisible : il serait INERTE — ImGui::ItemAdd
  // rejette tout ce qui tombe hors du clip, donc ni survol ni clic. C'est aussi ce
  // qui empêche le clic de partir dans le déplacement de la fenêtre : tant que le
  // bouton est survolé, HoveredId != 0 et ImGui ne démarre pas le drag de titre.
  ImGui::PushClipRect(wp, ImVec2(wp.x + ww, wp.y + title_h), false);
  ImGui::SetCursorScreenPos(ImVec2(bx, by));
  const bool clicked = ro::RoSmallButton(label, bw);
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopClipRect();
  // 🔴 On NE restaure PAS le curseur, et c'est pour ça que cet appel doit être le
  // DERNIER de la fenêtre. Un SetCursorPos() final arme `DC.IsSetPos` ; si aucun
  // item ne suit, End() lève « Code uses SetCursorPos() to extend window/parent
  // boundaries » (l'encadré rose « MESSAGE FROM DEAR IMGUI »). Ici le bouton EST le
  // dernier item soumis, et ItemSize() a déjà désarmé le drapeau.

  // Infobulle APRÈS le PopClipRect : elle ouvre une autre fenêtre ImGui, elle ne
  // doit pas hériter du clip de la barre de titre.
  if (hovered)
    ImGui::SetTooltip(i18n::Tr("Envoyer un rapport de bug à l'équipe (le contexte est "
                      "joint automatiquement)"));
  if (clicked) Open(ctx);
}

void BugReport::Open(const Context& ctx) {
  // 🔴 UNE MODALE DÉJÀ OUVERTE NE SE REMPLACE PAS : `Open` efface `msg_buf_`, donc
  // un second appel jetterait le message que le joueur est en train d'écrire. Le
  // raccourci portait ce test chez lui (`!modal_open_`) tant qu'il était câblé
  // dans la frame ; il est ici depuis qu'il vient du catalogue d'actions, ce qui
  // le rend valable pour TOUS les appelants.
  if (modal_open_) return;
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
  // 🔴 PLUS DE RACCOURCI CÂBLÉ ICI. Ctrl+Alt+B se lisait dans cette frame, en dur :
  // il ne figurait donc dans aucune liste, ne se déplaçait pas, et le contrôle de
  // collision de l'écran des raccourcis ne le voyait pas — une touche donnée à
  // autre chose partait avec lui. Le combo est passé au catalogue
  // (`hotkeys::tool_bug_report`, même défaut Ctrl+Alt+B), qui l'affiche et le
  // remappe comme les autres et appelle `Open` par le dispatch clavier.
  if (want_open_) {
    ImGui::OpenPopup(i18n::Tr("Signaler un bug###bug_report_modal"));
    want_open_ = false;
    modal_open_ = true;
  }

  RenderModal();
  RenderAckToast();
}

void BugReport::RenderModal() {
  ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);

  // 🔴 Modale habillée RO — barre de titre 3-slice titlebar_* + corps clair, comme
  // les confirmations du char-select et le prompt de quantité. Elle est restée
  // longtemps en fenêtre ImGui NUE : skinner ses deux boutons ne suffisait pas, il
  // manquait tout le cadre autour. `ro::BeginRoPopupModal` centre lui-même à
  // l'apparition (d'où la disparition du SetNextWindowPos qui était ici) et pousse
  // les couleurs RO pour le contenu ; ImGui garde la modalité et le voile.
  // ⚠ EndRoPopupModal UNIQUEMENT si Begin a rendu true (règle EndPopup d'ImGui).
  if (!ro::BeginRoPopupModal(i18n::Tr("Signaler un bug###bug_report_modal"))) {
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

  // 🔴 Le corps d'une fenêtre RO est CLAIR : `ImGui::TextDisabled` y est calibré
  // pour un fond sombre et s'y efface presque entièrement. Les trois libellés
  // secondaires de cette modale passent donc par un gris SOMBRE explicite — la
  // palette que la feuille de personnage emploie déjà sur le même fond.

  ImGui::TextWrapped(i18n::Tr("Décris brièvement le problème. Le contexte ci-dessous "
                     "est joint automatiquement à ton rapport."));
  ImGui::Separator();

  // Contexte capturé (lecture seule).
  ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Contexte"));
  ImGui::TextWrapped("%s", ctx_.label.c_str());
  ImGui::Spacing();

  ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Ton message"));
  ImGui::InputTextMultiline("##bug_msg", msg_buf_, sizeof(msg_buf_),
                            ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5));
  ImGui::TextColored(ro::pal::kLabel, i18n::Tr("%zu / %zu caractères"), std::strlen(msg_buf_),
                     kMaxMsgBytes);

  ImGui::Separator();

  const bool has_msg = msg_buf_[0] != '\0';
  const uint32_t now = GetTickCount();
  const bool throttled = (now - last_send_tick_) < kSendThrottleMs;

  ImGui::BeginDisabled(!has_msg || throttled);
  // Les deux boutons de la modale sont les derniers du fichier à être restés nus
  // (le « Signaler un bug » de la fenêtre de description est skinné depuis
  // toujours) : c'était l'incohérence la plus visible, puisque c'est la fenêtre
  // qu'on ouvre justement pour signaler ce qui cloche.
  // ⚠ RoButton prend DEUX floats, pas un ImVec2.
  if (ro::RoButton(i18n::Tr("Envoyer"), 120.0f, 0.0f)) {
    SendReport(ctx_, msg_buf_);
    modal_open_ = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  // 🔴 `AllowWhenDisabled` : sans lui, un bouton grisé ne compte pas comme
  // survolé — or c'est précisément là qu'il faut dire POURQUOI. L'infobulle du
  // throttle ne pouvait donc jamais sortir (défaut antérieur au skinning).
  if (throttled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip(i18n::Tr("Patiente quelques secondes avant un nouveau rapport."));

  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Annuler"), 120.0f, 0.0f)) {
    modal_open_ = false;
    ImGui::CloseCurrentPopup();
  }

  ro::EndRoPopupModal();
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
      title = i18n::Tr("Rapport envoyé");
      text  = i18n::Tr("Merci ! Ton rapport de bug a bien été enregistré.");
      accent = IM_COL32(90, 205, 105, 255); break;
    case 1:
      title = i18n::Tr("Trop rapide");
      text  = i18n::Tr("Patiente un instant avant d'envoyer un nouveau rapport.");
      accent = IM_COL32(238, 190, 90, 255); break;
    default:
      title = i18n::Tr("Échec de l'envoi");
      text  = i18n::Tr("Le rapport n'a pas pu être enregistré (erreur serveur).");
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
