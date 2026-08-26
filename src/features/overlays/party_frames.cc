#include "features/overlays/party_frames.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"  // SaveSettings + AlignGrid partagée
#include "features/systems/bourgeon_opcodes.h"   // bopcodes:: (catalogue partagé)
#include "imgui.h"
#include "ragnarok/globals.h"
#include "ragnarok/uiwnd.h"
#include "ui/game_texture.h"  // ro::CachedTextureFromGameFile (icône de classe)
#include "ui/ro_imgui.h"
#include "utils/i18n.h"

namespace {

// Du HUD de groupe (`uiwnd::kUIMiniPartyWnd`) on ne masque que le CONTENEUR :
// `UIMiniParty_PopulateAllMembers` (0x00a339c0) continue de le peupler depuis le
// handler de la liste de groupe, et on ne touche à rien de tout cela.

// Le couple custom qui porte le SP (cf. l'en-tête) : les MÊMES opcodes que la
// fenêtre de cible, pris dans le catalogue partagé plutôt que recopiés — inutile
// d'en inventer d'autres, le serveur répond déjà pour un membre du même groupe.
constexpr uint16_t kOpReqTargetInfo = bopcodes::kReqTargetInfo;  // CZ, on demande
constexpr uint16_t kOpTargetInfo    = bopcodes::kTargetInfo;     // ZC, il répond

// Cadence des demandes de SP : un membre toutes les 250 ms. Le SP bouge vite en
// combat, mais pas au point de justifier un paquet par frame et par membre.
constexpr unsigned kPollIntervalMs = 250;
// Au-delà de ce silence, un SP connu est considéré comme PÉRIMÉ : le membre s'est
// éloigné (le serveur ne répond plus) et sa barre doit s'éteindre plutôt que de
// figer une vieille valeur.
constexpr unsigned kVitalsStaleMs = 3000;

// Masque/rend une fenêtre native sans lever si elle n'existe pas.
void SetNativeVisible(int window_id, bool visible) {
  __try {
    void* w = uiwnd::FindWindow(window_id);
    if (w) uiwnd::SafeSetVisible(w, visible);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

ImU32 Col(const float rgba[4]) {
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]));
}

// La même couleur, à l'alpha forcé — pour un fond de barre qui doit rester
// lisible sous une barre semi-transparente.
ImU32 ColA(const float rgba[4], float alpha) {
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(rgba[0], rgba[1], rgba[2], alpha));
}

}  // namespace

PartyFrames::PartyFrames() {
  // L'opcode de réponse. `target_frame` l'enregistre déjà de son côté, mais
  // l'enregistrement passe par un `unordered_set` : le refaire est sans effet, et
  // ça garantit qu'on reçoit le SP même si la fenêtre de cible venait à disparaître.
  Bourgeon::Instance().RegisterRecvOpcode(kOpTargetInfo);
}

void PartyFrames::SyncNativeHud() {
  const int want_hidden = enabled_ ? 1 : 0;
  // Le natif RECRÉE ses sous-fenêtres à chaque changement de composition : on
  // repose donc l'état à chaque tick tant qu'il est « caché », sans quoi une
  // arrivée de membre ferait réapparaître le HUD d'origine par-dessus le nôtre.
  if (want_hidden == 0 && native_hidden_ == 0) return;
  SetNativeVisible(uiwnd::kUIMiniPartyWnd, !enabled_);
  native_hidden_ = want_hidden;
}

void PartyFrames::OnTick() {
  if (Bourgeon::Instance().IsMapLoading()) return;
  SyncNativeHud();
  if (enabled_ && show_sp_) PollVitals();
}

// ── Le SP : une demande par tick, en rotation ───────────────────────────────
//
// Le serveur ne pousse rien de lui-même (pas d'abonnement) : c'est au client de
// demander. On ne le fait que pour UN membre à la fois — sur un groupe de 24,
// interroger tout le monde à chaque tick ferait 240 paquets par seconde pour une
// information qui bouge lentement.
void PartyFrames::PollVitals() {
  const unsigned now = GetTickCount();
  if (last_poll_ms_ != 0 && (now - last_poll_ms_) < kPollIntervalMs) return;

  // La liste de rendu n'existe qu'en frame : on relit ici, c'est peu coûteux
  // (une marche de liste chaînée) et ça évite de dépendre du rendu — le HUD peut
  // être masqué par une fenêtre sans que les demandes s'arrêtent.
  std::vector<rag::social::Entry> members;
  rag::social::ReadParty(members);
  if (members.size() <= 1) return;

  const uint32_t me = rag::social::OwnAid();
  // Un tour complet cherche le prochain membre INTERROGEABLE : ni moi (mon SP est
  // dans mes propres globales), ni un hors-ligne (le serveur répondrait status=1).
  for (size_t tried = 0; tried < members.size(); ++tried) {
    if (poll_cursor_ >= members.size()) poll_cursor_ = 0;
    const rag::social::Entry& m = members[poll_cursor_++];
    if (m.gid == 0 || m.gid == me || m.offline) continue;
    uint8_t packet[8];  // [op:2][len:2][gid:4]
    *reinterpret_cast<uint16_t*>(packet + 0) = kOpReqTargetInfo;
    *reinterpret_cast<uint16_t*>(packet + 2) = static_cast<uint16_t>(sizeof(packet));
    *reinterpret_cast<uint32_t*>(packet + 4) = m.gid;
    Bourgeon::Instance().SendPacket(packet, sizeof(packet));
    last_poll_ms_ = now;
    return;
  }
}

// 🔴 FIL RÉSEAU : copier seulement.
void PartyFrames::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                               uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

void PartyFrames::HandlePacket(uint16_t opcode, const uint8_t* data,
                               uint16_t len) {
  if (opcode != kOpTargetInfo) return;
  // `data` commence APRÈS [opcode:2][len:2] : convention de RegisterRecvOpcode.
  // [gid:4][status:1][known:1][type:1][level:2][hp:4][maxhp:4][sp:4][maxsp:4]...
  if (len < 25) return;

  uint32_t gid = 0;
  std::memcpy(&gid, data, 4);
  if (gid == 0) return;

  // status != 0 : hors de portée ou entité disparue. On OUBLIE ce qu'on savait —
  // garder l'ancienne valeur afficherait un SP figé sans que rien ne le signale.
  if (data[4] != 0) {
    vitals_.erase(gid);
    return;
  }
  // `known` dit ce qui est RENSEIGNÉ : bit 2 = SP. Un adversaire (hors groupe)
  // reçoit son type et rien d'autre — d'où ce masque, qui distingue « 0 SP » de
  // « SP inconnu ».
  if ((data[5] & bopcodes::kKnownSp) == 0) {
    vitals_.erase(gid);
    return;
  }

  Vitals v;
  std::memcpy(&v.sp,    data + 17, 4);
  std::memcpy(&v.maxsp, data + 21, 4);
  v.stamp = GetTickCount();
  vitals_[gid] = v;
}

void PartyFrames::OnRenderUI() {
  if (!enabled_) return;

  rag::social::ReadParty(members_);
  // Un groupe d'une seule personne, c'est le joueur seul : aucun HUD à montrer.
  if (members_.size() <= 1) return;

  const uint32_t me = rag::social::OwnAid();

  // Les membres à afficher. On filtre AVANT de calculer la grille : une tuile
  // vide au milieu casserait la lecture.
  std::vector<const rag::social::Entry*> shown;
  shown.reserve(members_.size());
  for (const rag::social::Entry& m : members_) {
    if (!show_self_ && m.gid == me) continue;
    if (!show_offline_ && m.offline) continue;
    shown.push_back(&m);
  }
  if (shown.empty()) return;

  const int cols = std::max(1, std::min(columns_, static_cast<int>(shown.size())));
  const int rows = (static_cast<int>(shown.size()) + cols - 1) / cols;
  const float gap    = ro::Px(static_cast<float>(std::max(0, gap_)));
  const float tile_w = ro::Px(static_cast<float>(std::max(60, tile_w_)));
  const float tile_h = ro::Px(static_cast<float>(std::max(18, tile_h_)));
  const float pad    = ro::Px(3.0f);

  // 🔴 C'est la TUILE qui commande la taille du cadre, pas l'inverse : un raid
  // frame se règle en « telle taille de case », pas en tirant un coin jusqu'à
  // tomber juste. Le cadre reste déplaçable ; son redimensionnement à la souris
  // est donc sans effet ici, et c'est voulu.
  rect_.w = static_cast<int>(cols * tile_w + (cols - 1) * gap + pad * 2);
  rect_.h = static_cast<int>(rows * tile_h + (rows - 1) * gap + pad * 2);

  ro::HudFrameOpts opts;
  // 🔴 MAJ déverrouille temporairement : on replace un HUD figé sans aller
  // décocher sa case, puis on relâche et il redevient transparent aux clics.
  //
  // ⚠ SEULEMENT quand le curseur est SUR le cadre. La touche seule ne suffit pas :
  // en RO, MAJ+clic est l'ATTAQUE FORCÉE, et un cadre déverrouillé reprend la
  // souris au jeu. Tenir MAJ pour frapper aurait rendu muette toute la surface du
  // HUD, où qu'on clique — le geste du jeu doit rester intact partout ailleurs.
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool over_frame =
      mouse.x >= static_cast<float>(rect_.x) &&
      mouse.y >= static_cast<float>(rect_.y) &&
      mouse.x <  static_cast<float>(rect_.x + rect_.w) &&
      mouse.y <  static_cast<float>(rect_.y + rect_.h);
  const bool unlock_override = ImGui::GetIO().KeyShift && over_frame;
  opts.locked   = locked_ && !unlock_override;
  opts.border   = false;
  opts.rounding = ro::Px(3.0f);
  // Le fond du cadre : sans lui, des tuiles sombres sur une carte sombre ne se
  // distinguent plus du décor.
  opts.bg       = col_frame_bg_;
  // La grille d'alignement appartient à MoonlightUi ; `ui/` ne doit rien savoir
  // de `features/`, d'où son passage en paramètre plutôt qu'une lecture là-bas.
  // Le HUD s'aligne donc sur les mêmes lignes que Basic Info, les icônes de menu
  // ou la barre de statuts — c'est tout l'intérêt d'une grille PARTAGÉE : poser
  // ses éléments d'interface au même pas, sans les caler à l'œil les uns après
  // les autres.
  if (auto* mui = Bourgeon::Instance().moonlight_ui()) opts.grid = &mui->grid_;
  opts.min_w    = ro::Px(60.0f);
  opts.min_h    = ro::Px(20.0f);

  if (ro::BeginHudFrame("##party_frames", &rect_, opts, &geometry_dirty_)) {
    // 🔴 L'origine vient de la FENÊTRE, pas du curseur ImGui — comme le font
    // basic_info et target_frame. Déverrouillé, `BeginHudFrame` pose sur toute sa
    // surface un `InvisibleButton` qui laisse le curseur EN DESSOUS du cadre :
    // des tuiles posées à `GetCursorScreenPos()` tombaient hors du clip et
    // disparaissaient dès qu'on tenait MAJ, ne laissant que le fond.
    const ImVec2 win = ImGui::GetWindowPos();
    const ImVec2 origin(win.x + pad, win.y + pad);
    for (size_t i = 0; i < shown.size(); ++i) {
      const int col = static_cast<int>(i) % cols;
      const int row = static_cast<int>(i) / cols;
      const ImVec2 p0(origin.x + col * (tile_w + gap),
                      origin.y + row * (tile_h + gap));
      const ImVec2 p1(p0.x + tile_w, p0.y + tile_h);
      DrawTile(*shown[i], p0, p1, shown[i]->gid == me);
    }
    // (Aucun `Dummy` à réserver : la taille du cadre est imposée plus haut par
    // `rect_`, que `BeginHudFrame` ré-épingle à chaque frame.)
  }
  ro::EndHudFrame();

  if (geometry_dirty_) {
    geometry_dirty_ = false;
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}

void PartyFrames::DrawTile(const rag::social::Entry& m, ImVec2 p0, ImVec2 p1,
                           bool is_me) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float rounding = ro::Px(2.0f);
  const float pad = ro::Px(3.0f);

  dl->AddRectFilled(p0, p1, Col(col_tile_bg_), rounding);

  // ── La barre de vie EST le fond de la tuile ──────────────────────────────
  // C'est le principe du raid frame : on ne lit pas un nombre, on voit combien
  // de vert il reste. Quand les PV sont inconnus, on ne peint RIEN — une tuile
  // vide se lirait comme « mort », alors que le membre est seulement trop loin.
  const bool has_hp = !m.offline && m.has_hp && m.max_hp > 0;
  if (has_hp) {
    float frac = static_cast<float>(m.hp) / static_cast<float>(m.max_hp);
    frac = std::max(0.0f, std::min(1.0f, frac));
    const int pct = static_cast<int>(frac * 100.0f);
    const float* fill = (pct <= hp_low_pct_) ? col_hp_low_
                      : (pct <= hp_mid_pct_) ? col_hp_mid_
                                             : col_hp_high_;
    if (frac > 0.0f) {
      dl->AddRectFilled(p0, ImVec2(p0.x + (p1.x - p0.x) * frac, p1.y), Col(fill),
                        rounding);
    }
  }

  // ── Le SP, résolu AVANT de dessiner quoi que ce soit d'autre ─────────────
  //
  // 🔴 L'ordre compte : la barre de SP occupe toute la largeur en bas de tuile,
  // donc elle mange la hauteur disponible. Tant qu'on la dessinait après l'icône,
  // celle-ci était calée sur la hauteur TOTALE et se faisait recouvrir par le
  // bas. On détermine donc d'abord si une barre existe, puis tout le reste se
  // range dans ce qu'elle laisse.
  //
  // Pour MOI, le SP est dans mes propres globales — toujours juste, sans réseau.
  // Pour les autres, il vient de ZC 0x0F2A, et n'existe que tant qu'ils sont à
  // portée : sans réponse fraîche, on ne peint RIEN (une barre de SP figée
  // vaudrait moins que pas de barre du tout).
  int sp = 0, maxsp = 0;
  if (show_sp_ && !m.offline) {
    if (is_me) {
      sp    = rag::ReadInt(rag::kOwnSpAddr);
      maxsp = rag::ReadInt(rag::kOwnMaxSpAddr);
    } else {
      auto it = vitals_.find(m.gid);
      if (it != vitals_.end() &&
          (GetTickCount() - it->second.stamp) <= kVitalsStaleMs) {
        sp    = it->second.sp;
        maxsp = it->second.maxsp;
      }
    }
  }
  const float sp_h = (maxsp > 0)
                         ? ro::Px(static_cast<float>(std::max(2, sp_bar_h_)))
                         : 0.0f;
  // Le bas de la zone de CONTENU : tout (icône et texte) se range au-dessus.
  const float bottom = p1.y - sp_h;

  // ── L'icône de classe, à gauche ──────────────────────────────────────────
  // Le vrai art du client (`renewalparty\icon_jobs_<job>.bmp`), carré et calé sur
  // la hauteur DISPONIBLE. C'est ce qui rend une grille lisible d'un coup d'œil :
  // on reconnaît le soigneur à sa silhouette, pas à son nom.
  float text_x = p0.x + pad;
  if (show_job_icon_) {
    char path[160];
    rag::social::JobIconPath(m.job, path, sizeof(path));
    const ro::GameTexture icon = ro::CachedTextureFromGameFile(path);
    if (icon.tex) {
      // La hauteur utile, barre de SP déduite. Sur une tuile basse avec une
      // grosse barre, il ne reste presque rien : l'icône rétrécit jusqu'à
      // disparaître plutôt que de déborder sur ce qui l'entoure.
      const float avail = bottom - p0.y - pad;
      const float side = std::min(avail, ro::Px(28.0f));
      if (side >= ro::Px(8.0f)) {
        const ImVec2 i0(p0.x + pad, p0.y + ((bottom - p0.y) - side) * 0.5f);
        const ImVec2 i1(i0.x + side, i0.y + side);
        // Hors ligne : la même icône, assombrie — comme la fenêtre Amis/Groupe.
        const ImU32 tint = m.offline ? IM_COL32(140, 140, 140, 220)
                                     : IM_COL32_WHITE;
        dl->AddImage(reinterpret_cast<ImTextureID>(icon.tex), i0, i1,
                     ImVec2(0, 0), ImVec2(1, 1), tint);
        text_x = i1.x + pad;
      }
    }
  }

  // ── La barre de SP, en bas de la tuile ───────────────────────────────────
  if (maxsp > 0) {
    float frac = static_cast<float>(sp) / static_cast<float>(maxsp);
    frac = std::max(0.0f, std::min(1.0f, frac));
    const ImVec2 s0(p0.x, bottom);
    dl->AddRectFilled(s0, p1, ColA(col_tile_bg_, 0.95f));
    if (frac > 0.0f) {
      dl->AddRectFilled(s0, ImVec2(s0.x + (p1.x - s0.x) * frac, p1.y),
                        Col(col_sp_));
    }
  }

  dl->AddRect(p0, p1, is_me ? Col(col_me_) : IM_COL32(0, 0, 0, 190), rounding, 0,
              is_me ? 2.0f : 1.0f);

  // ── Le texte : nom, puis l'état ou les PV ────────────────────────────────
  const bool dim = m.offline || !has_hp;
  const ImU32 text = dim ? ColA(col_text_, col_text_[3] * 0.62f) : Col(col_text_);

  char first[96];
  if (show_level_) {
    std::snprintf(first, sizeof(first), "%s%s Lv.%u", m.is_leader ? "* " : "",
                  ro::LocalToUtf8(m.name.c_str()), m.level);
  } else {
    std::snprintf(first, sizeof(first), "%s%s", m.is_leader ? "* " : "",
                  ro::LocalToUtf8(m.name.c_str()));
  }

  char second[96] = {0};
  if (m.offline) {
    std::snprintf(second, sizeof(second), "%s", i18n::Tr("Hors ligne"));
  } else if (!has_hp) {
    std::snprintf(second, sizeof(second), "%s", i18n::Tr("Hors de portée"));
  } else {
    // Le pourcentage est arrondi VERS LE HAUT tant qu'il reste un point de vie :
    // afficher « 0 % » sur quelqu'un de vivant ferait renoncer à le soigner.
    const int pct = (m.hp > 0)
        ? std::max(1, static_cast<int>((static_cast<float>(m.hp) /
                                        static_cast<float>(m.max_hp)) * 100.0f))
        : 0;
    switch (hp_text_mode_) {
      case kHpTextNumbers:
        std::snprintf(second, sizeof(second), "%d/%d", m.hp, m.max_hp);
        break;
      case kHpTextPercent:
        std::snprintf(second, sizeof(second), "%d %%", pct);
        break;
      case kHpTextBoth:
        std::snprintf(second, sizeof(second), "%d/%d (%d %%)", m.hp, m.max_hp, pct);
        break;
      default:  // kHpTextNone
        break;
    }
  }

  // ── La taille du texte est un RÉGLAGE ────────────────────────────────────
  // Une grille se lit en périphérie de l'écran : sa police n'a pas de raison de
  // suivre celle des fenêtres, qu'on lit de face. `AddText` prend la taille en
  // paramètre, donc rien à empiler ni à dépiler.
  const float fsz  = ro::Px(static_cast<float>(std::max(8, text_px_)));
  const float line = fsz;
  ImFont* font = ImGui::GetFont();

  // Le bloc de texte est centré verticalement dans ce qui reste sous la barre de
  // SP : une tuile basse n'affiche que le nom, une tuile haute les deux lignes.
  const bool two_lines = (second[0] != '\0') && ((bottom - p0.y) >= line * 2.0f);
  const float block_h = two_lines ? line * 2.0f : line;
  float ty = p0.y + ((bottom - p0.y) - block_h) * 0.5f;
  if (ty < p0.y) ty = p0.y;

  dl->AddText(font, fsz, ImVec2(text_x, ty), text, first);
  if (two_lines) {
    dl->AddText(font, fsz, ImVec2(text_x, ty + line), text, second);
  }
}
