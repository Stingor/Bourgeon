#include "features/overlays/party_frames.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"  // SaveSettings + AlignGrid partagée
#include "features/overlays/target_frame.h"      // cibler par le chemin du HUD
#include "features/systems/bourgeon_opcodes.h"   // bopcodes:: (catalogue partagé)
#include "features/windows/party_friend_window.h"  // les gestes de groupe
#include "imgui.h"
#include "ragnarok/game_scene.h"  // gamescene::FindActorByGid (cible d'un sort)
#include "ragnarok/globals.h"
#include "ragnarok/uiwnd.h"
#include "features/status_cell.h"  // le rendu d'UNE case d'état
#include "features/systems/status_effects.h"  // les buffs, lus au fil du réseau
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

// Bornes du nombre de colonnes. La même valeur que le slider du panneau : le
// grip de redimensionnement et le réglage doivent s'arrêter au même endroit,
// sinon tirer le cadre produirait une valeur que le panneau refuse d'afficher.
constexpr int kMaxColumns = 6;

// La cible COURANTE du jeu. Lue par `TargetFrame::CurrentSelectionGid` plutôt
// qu'ici : l'offset `CGameMode+0xF4` avait DEUX lecteurs, un par surface, et la
// liste Groupe/Amis en aurait fait un troisième.
uint32_t CurrentTargetGid() { return TargetFrame::CurrentSelectionGid(); }

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

bool PartyFrames::MemberSp(uint32_t gid, int* sp, int* maxsp) const {
  // 🔴 MOI D'ABORD, et ce n'est pas une optimisation : `PollVitals` SAUTE mon
  // propre GID — mon SP est dans mes globales, on ne le demande pas au serveur.
  // Le cache ne me contient donc JAMAIS, et sans ce cas ma ligne serait la seule
  // sans barre de SP. C'est le pendant exact du piège des PV : le joueur n'est
  // nulle part là où sont les autres.
  if (gid != 0 && gid == rag::social::OwnAid()) {
    const int s = rag::ReadInt(rag::kOwnSpAddr);
    const int m = rag::ReadInt(rag::kOwnMaxSpAddr);
    if (m <= 0) return false;
    if (sp)    *sp    = s;
    if (maxsp) *maxsp = m;
    return true;
  }
  auto it = vitals_.find(gid);
  if (it == vitals_.end()) return false;
  if ((GetTickCount() - it->second.stamp) > kVitalsStaleMs) return false;
  if (it->second.maxsp <= 0) return false;
  if (sp)    *sp    = it->second.sp;
  if (maxsp) *maxsp = it->second.maxsp;
  return true;
}

void PartyFrames::OnTick() {
  if (Bourgeon::Instance().IsMapLoading()) return;
  SyncNativeHud();
  // On interroge dès que QUELQU'UN affiche du SP : le HUD lui-même, ou une autre
  // surface qui vient de le demander. Sans cette seconde condition, la barre de
  // SP de la fenêtre Amis/Groupe resterait vide dès que la grille est éteinte.
  const bool want_sp = (enabled_ && show_sp_) || sp_wanted_by_other_;
  sp_wanted_by_other_ = false;  // demande VIVANTE : elle se redemande chaque frame
  if (want_sp) PollVitals();

  // Les buffs ont leur propre registre et leur propre sondage : on ne fait que
  // dire qu'on les affiche. Demande VIVANTE, redemandee a chaque tick — la
  // couper suffit a arreter le trafic quand la grille s'eteint.
  if (enabled_ && show_buffs_) {
    if (auto* fx = Bourgeon::Instance().status_effects()) fx->RequestPolling();
  }
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

// ── L'infobulle d'une tuile ─────────────────────────────────────────────────
//
// Elle redonne ce que la tuile porte, mais ENTIER — le texte d'une case étant
// découpé à ses bords — et y ajoute ce qui n'y tient jamais : la classe et la
// carte. C'est aussi le seul endroit où l'on peut expliquer pourquoi une barre
// manque.
void PartyFrames::DrawTooltip(const rag::social::Entry& m, bool is_me) {
  ImGui::BeginTooltip();

  ImGui::TextUnformatted(ro::LocalToUtf8(m.name.c_str()));
  ImGui::SameLine();
  ImGui::TextDisabled("Lv.%u", m.level);
  if (m.is_leader) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", i18n::Tr("(chef)"));
  }
  ImGui::Separator();

  ImGui::TextDisabled("%s", ro::LocalToUtf8(rag::social::JobName(m.job)));

  if (m.offline) {
    ImGui::TextDisabled("%s", i18n::Tr("Hors ligne"));
    ImGui::EndTooltip();
    return;
  }

  // La carte, sous son nom lisible — le champ brut est un identifiant.
  if (!m.map.empty()) {
    char bare[64] = {0};
    char pretty[64] = {0};
    size_t i = 0;
    for (; i + 1 < sizeof(bare) && m.map[i] && m.map[i] != '.'; ++i)
      bare[i] = m.map[i];
    bare[i] = '\0';
    if (rag::MapDisplayName(bare, pretty, sizeof(pretty)) && pretty[0])
      ImGui::TextDisabled("%s", ro::LocalToUtf8(pretty));
    else
      ImGui::TextDisabled("%s", ro::LocalToUtf8(m.map.c_str()));
  }

  if (m.has_hp && m.max_hp > 0) {
    const int pct = (m.hp > 0)
        ? std::max(1, static_cast<int>((static_cast<float>(m.hp) /
                                        static_cast<float>(m.max_hp)) * 100.0f))
        : 0;
    ImGui::Text("%s %d/%d (%d %%)", i18n::Tr("PV"), m.hp, m.max_hp, pct);
  } else {
    // On DIT pourquoi il n'y a rien, plutôt que de laisser une ligne vide.
    ImGui::TextDisabled("%s", i18n::Tr("PV inconnus : hors de portée"));
  }

  if (show_sp_) {
    int sp = 0, maxsp = 0;
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
    // On DIT pourquoi, comme pour les PV — et les DEUX causes ne sont pas la
    // même. Hors de portée, le serveur refuse de répondre ; à portée, c'est que
    // la rotation des demandes n'est pas encore passée par ce membre (elle
    // n'interroge qu'un membre à la fois, pour ne pas partir en rafale).
    // Afficher « hors de portée » dans le second cas serait un mensonge, et
    // ferait chercher un problème là où il n'y a qu'une attente.
    if (maxsp > 0) {
      ImGui::Text("%s %d/%d", i18n::Tr("SP"), sp, maxsp);
    } else if (!m.has_hp || m.max_hp <= 0) {
      ImGui::TextDisabled("%s", i18n::Tr("SP inconnus : hors de portée"));
    } else {
      ImGui::TextDisabled("%s", i18n::Tr("SP inconnus : réponse en attente"));
    }
  }

  ImGui::EndTooltip();
}

// ── Le menu d'une tuile ─────────────────────────────────────────────────────
//
// Les mêmes gestes que le menu contextuel de la fenêtre Amis/Groupe, et pour
// cause : c'est ELLE qui les exécute. On ne recopie ni commande ni paquet — on
// arme chez elle, son `FlushPending` émet. Mêmes gardes des deux côtés.
void PartyFrames::DrawMemberMenu() {
  if (open_menu_) {
    ImGui::OpenPopup("##party_tile_menu");
    open_menu_ = false;
  }
  if (!ImGui::BeginPopup("##party_tile_menu")) return;

  auto* pfw = Bourgeon::Instance().party_friend_window();
  const bool is_me = (menu_gid_ == rag::social::OwnAid());

  ImGui::TextDisabled("%s", ro::LocalToUtf8(menu_name_.c_str()));
  ImGui::Separator();

  if (pfw == nullptr) {
    ImGui::TextDisabled("%s", i18n::Tr("Indisponible."));
    ImGui::EndPopup();
    return;
  }

  // 🔴 HORS LIGNE : la seule chose qui garde un sens est de l'EXPULSER. On ne
  // chuchote pas à quelqu'un de déconnecté, et lui confier le commandement
  // laisserait le groupe sans chef présent. Proposer ces entrées serait promettre
  // des gestes qui échoueraient en silence côté serveur.
  if (!menu_offline_) {
    // ⚠ On n'arrive ici QUE sans sprite (membre sur une autre carte) ou sur
    // soi-même : avec un acteur, le clic droit a déjà ouvert le menu du client.
    // Le chuchotement voyage PAR NOM et n'a besoin d'aucune entité — c'est
    // précisément ce que ce menu-ci est le seul à pouvoir offrir.
    if (!is_me && ImGui::Selectable(i18n::Tr("Chuchoter"))) {
      pfw->RequestWhisper(menu_gid_);
    }
    if (pfw->IsPartyLeader() && !is_me &&
        ImGui::Selectable(i18n::Tr("Nommer chef de groupe"))) {
      pfw->RequestMakeLeader(menu_gid_);
    }
  }
  // Expulser reste offert dans les deux cas — c'est même le geste attendu sur un
  // membre déconnecté depuis longtemps.
  if (pfw->IsPartyLeader() && !is_me &&
      ImGui::Selectable(i18n::Tr("Expulser du groupe"))) {
    pfw->RequestKick(menu_gid_);
  }
  ImGui::EndPopup();
}

// ── Les clics, rejoués HORS de la frame ImGui ───────────────────────────────
void PartyFrames::FlushPending() {
  const uint32_t target = pending_target_gid_;
  const uint32_t menu   = pending_menu_gid_;
  pending_target_gid_ = 0;
  pending_menu_gid_   = 0;

  // Clic GAUCHE : cibler, comme un clic sur le sprite. On passe par le HUD de
  // cible, qui possède déjà ce chemin (et ses gardes) — écrire `CGameMode+0xF4`
  // à la main serait le piège que la mémoire project_target_system_re décrit.
  // Sans effet si le mode Ciblage du joueur est éteint : c'est SON réglage qui
  // décide qu'une cible existe.
  if (target != 0) {
    // 🔴 Pas d'acteur, pas de ciblage. Un membre HORS LIGNE n'en a évidemment
    // aucun, un membre hors de portée non plus : le cibler quand même ouvrait un
    // HUD de cible VIDE, puisqu'il n'y a rien à y montrer. Mieux vaut que le clic
    // ne fasse rien que d'installer une fenêtre creuse.
    //
    // ⚠ MOI excepté : mon acteur n'est pas dans la liste que parcourt
    // `FindActorByGid` (le natif le range en `actorMgr+0x2C`, cf.
    // `SkillTargetGid`). Sans cette exception, ma propre tuile serait la seule à
    // ne pas pouvoir se cibler.
    const bool self = (target == rag::social::OwnAid());
    if (self || gamescene::FindActorByGid(target) != nullptr) {
      if (auto* tf = Bourgeon::Instance().target_frame()) {
        // Rend false quand le mode Ciblage est éteint — on ne le signale pas :
        // le réglage du panneau dit déjà que le geste en dépend.
        tf->RequestTargetFromProxy(target);
      }
    }
  }

  // Clic DROIT : le menu. Il s'ouvre à la frame suivante (`open_menu_`), donc
  // rien de natif n'est touché ici — on ne fait que retenir sur QUI il porte.
  if (menu != 0) {
    menu_gid_ = menu;
    menu_name_.clear();
    menu_offline_ = false;
    // On fige l'état du membre AU MOMENT DU CLIC : la liste est relue à chaque
    // frame, et un membre peut se déconnecter pendant que le menu est déplié.
    for (const rag::social::Entry& m : members_) {
      if (m.gid == menu) {
        menu_name_    = m.name;
        menu_offline_ = m.offline;
        break;
      }
    }
    // 🔴 Quand un sprite représente ce membre, on n'ouvre PAS de menu à nous :
    // on arme celui du client. Le nôtre n'aurait plus porté qu'une entrée —
    // « Menu du personnage » — soit un menu dont le seul rôle était d'en ouvrir
    // un autre. Chef de groupe, expulsion et retrait d'ami y sont désormais.
    auto* pfw = Bourgeon::Instance().party_friend_window();
    const bool is_me = (menu == rag::social::OwnAid());
    if (pfw != nullptr && !is_me && !menu_offline_ &&
        gamescene::FindActorByGid(menu) != nullptr) {
      pfw->RequestEntityMenu(menu);
      return;
    }
    open_menu_ = true;
  }
}

// ── Ce que la grille propose à QuickCast ────────────────────────────────────
//
// Contrat identique à `TargetFrame::SkillTargetGid` : rendre le GID à viser pour
// ce mode de ciblage, ou 0. Modes 2 (offensif) et 4 (soutien) seulement — un sort
// AU SOL vise une cellule, pas une entité.
//
// 🔴 UNE DIFFÉRENCE ASSUMÉE avec le HUD de cible : on autorise à se viser
// SOI-MÊME. `ValidSkillTarget` le refuse, et c'est juste pour une cible externe ;
// mais se soigner en cliquant sa propre case est le geste n°1 d'un raid frame.
// La restriction n'aurait ici aucun sens.
//
// Le reste des règles est le même, et ce ne sont pas des filtres de confort :
// les validations du chemin du CLIC (CursorMgr_UpdateHover) ne vivent PAS dans
// les messages d'acteur qu'émet QuickCast. Ce qu'on laisse passer part.
uint32_t PartyFrames::SkillTargetGid(int targeting_mode) const {
  if (!CastsOnTile() || hovered_gid_ == 0) return 0u;
  if (targeting_mode != 2 && targeting_mode != 4) return 0u;
  // Les offsets sont ceux de CE build : ailleurs, on ne propose rien.
  if (Bourgeon::Instance().client().timestamp() != 20250716) return 0u;

  // 🔴 En OFFENSIF, la grille ne propose rien. Elle ne contient que des membres
  // du groupe : viser un allié avec un sort d'attaque relève du PVP, que le
  // client réserve au clic manuel (mêmes règles Maj/PVP/GVG que
  // `ValidSkillTarget`). Un raid frame sert à soutenir.
  if (targeting_mode == 2) return 0u;

  // L'acteur doit être là : c'est lui que le message d'acteur vise. Un membre
  // hors de portée n'a pas d'acteur chargé — le sort ne partirait sur personne.
  //
  // 🔴 SAUF MOI. Mon propre acteur n'est PAS dans la liste que parcourt
  // `FindActorByGid` : le natif le range à part, en `actorMgr+0x2C`, et c'est
  // exactement pour ça qu'il possède une fonction séparée —
  // `ActorMgr_FindByGidOrSelf` (0x00a69e70) commence par
  // `if (gid == g_Account_Aid) return mgr[+0x2C]` avant de parcourir quoi que ce
  // soit. Exiger la liste rendait MA tuile inerte : impossible de s'y soigner ou
  // de s'y buffer, alors que c'est le geste n°1 d'un raid frame.
  if (hovered_gid_ != rag::social::OwnAid() &&
      gamescene::FindActorByGid(hovered_gid_) == nullptr) {
    return 0u;
  }
  return hovered_gid_;
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

  // ── La géométrie du cadre découle des tuiles… sauf pendant qu'on la tire ──
  //
  // La taille se DÉDUIT du nombre de colonnes et de la taille des cases : un raid
  // frame se règle ainsi, pas en tirant un coin jusqu'à tomber juste. Mais le
  // cadre porte une poignée de redimensionnement, et une poignée qui ne fait rien
  // est une promesse non tenue — on lui donne donc un rôle : ÉLARGIR LE CADRE
  // AJOUTE DES COLONNES.
  //
  // Pendant le tirage on laisse donc `rect_.w` suivre la souris ; à la frame
  // suivante il reprend sa valeur canonique, calculée depuis le nombre de
  // colonnes qu'on vient d'en déduire. La hauteur, elle, est toujours imposée :
  // elle découle du nombre de rangées, qui découle des membres présents.
  const int want_w =
      static_cast<int>(cols * tile_w + (cols - 1) * gap + pad * 2);
  if (!ro::HudFrameDragging()) rect_.w = want_w;
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
  // ⚠ `|| HudFrameDragging()` : un geste EN COURS garde la main même si le
  // curseur sort du cadre. Sans cela, tirer un bord vers l'extérieur — ce qu'est
  // un agrandissement — faisait sortir la souris du rectangle, donc reverrouiller
  // le cadre, donc interrompre le geste : la poignée apparaissait et ne servait
  // à rien.
  const bool unlock_override =
      ImGui::GetIO().KeyShift && (over_frame || ro::HudFrameDragging());
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
  // ⚠ Cliquable = la grille PREND la souris au jeu sur toute sa surface (molette
  // et clic droit compris). C'est le prix des gestes sur les tuiles, d'où
  // l'opt-in : sans lui, le cadre reste clic-traversant.
  opts.clickable = clickable_;

  // Le membre survolé est relevé À CHAQUE frame, et remis à zéro d'abord : une
  // valeur qui survit à la frame ferait viser quelqu'un que le curseur a quitté.
  hovered_gid_ = 0;
  target_gid_  = CurrentTargetGid();

  ro::HudFrameClicks clicks;
  if (ro::BeginHudFrame("##party_frames", &rect_, opts, &geometry_dirty_,
                        &clicks)) {
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
      // 🔴 Test de rectangle À LA MAIN, et non `IsItemHovered` : le cadre est
      // clic-traversant quand il est verrouillé (`NoInputs`), donc ImGui ne le
      // survole jamais. C'est justement ce qu'on veut — la grille répond « sur
      // qui ? » sans prendre le clic au jeu.
      if (mouse.x >= p0.x && mouse.x < p1.x &&
          mouse.y >= p0.y && mouse.y < p1.y) {
        hovered_gid_ = shown[i]->gid;
      }
      DrawTile(*shown[i], p0, p1, shown[i]->gid == me);
    }
    // (Aucun `Dummy` à réserver : la taille du cadre est imposée plus haut par
    // `rect_`, que `BeginHudFrame` ré-épingle à chaque frame.)

    // ── Les clics, MIS EN ATTENTE ────────────────────────────────────────────
    // `hovered_gid_` vient d'être relevé au-dessus : il dit sur QUELLE tuile le
    // clic est tombé. Rien n'agit ici — cibler rejoue du code natif et ouvrir un
    // menu lit le dictionnaire de noms, deux choses à ne pas faire entre
    // NewFrame() et Render().
    if (clicks.left && hovered_gid_ != 0) {
      pending_target_gid_ = hovered_gid_;
    }
    if (clicks.right && hovered_gid_ != 0) {
      pending_menu_gid_ = hovered_gid_;
    }
  }
  ro::EndHudFrame();

  // L'infobulle et le menu vivent HORS du cadre : ouverts à l'intérieur, ils
  // seraient découpés par lui — il fait la taille d'une tuile.
  // ⚠ AUCUNE infobulle tant qu'un popup est ouvert. Elle se dessine par-dessus
  // tout — y compris par-dessus le menu contextuel qu'on vient d'ouvrir sur
  // cette même tuile, qu'elle rendait illisible. On teste N'IMPORTE QUEL popup
  // plutôt que le nôtre par son nom : une modale de confirmation mérite la même
  // paix.
  const bool popup_open = ImGui::IsPopupOpen(
      nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  if (show_tooltip_ && hovered_gid_ != 0 && !popup_open) {
    const uint32_t me_gid = rag::social::OwnAid();
    for (const rag::social::Entry& m : members_) {
      if (m.gid != hovered_gid_) continue;
      DrawTooltip(m, m.gid == me_gid);
      break;
    }
  }
  DrawMemberMenu();

  if (geometry_dirty_) {
    geometry_dirty_ = false;
    // La largeur que le joueur vient de tirer se traduit en NOMBRE DE COLONNES.
    // Un simple déplacement passe ici aussi, mais il ne change pas la largeur :
    // le calcul retombe alors sur la valeur courante et rien ne bouge.
    const float unit = tile_w + gap;
    if (unit > 1.0f) {
      int new_cols = static_cast<int>(
          ((static_cast<float>(rect_.w) - pad * 2 + gap) / unit) + 0.5f);
      new_cols = std::max(1, std::min(new_cols, kMaxColumns));
      columns_ = new_cols;
    }
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}

// ── Les icônes d'état d'un membre ───────────────────────────────────────────
//
// De droite à gauche : les buffs les plus RÉCENTS restent au bord, à la même
// place d'une frame à l'autre. Empiler par la gauche aurait fait glisser toute
// la rangée à chaque buff qui tombe.
float PartyFrames::DrawTileEffects(uint32_t gid, float right, float top,
                                   float bottom) {
  auto* fx = Bourgeon::Instance().status_effects();
  if (fx == nullptr) return right;

  std::vector<StatusEffects::Entry> list;
  if (!fx->Effects(gid, &list) || list.empty()) return right;

  const int rows = std::max(1, buff_rows_);
  const float gap = ro::Px(1.0f);
  // ⚠ La hauteur se PARTAGE entre les lignes : sans cette division, deux rangées
  // d'icônes pleine taille débordaient sous la tuile, par-dessus la suivante.
  const float side =
      std::min(ro::Px(static_cast<float>(std::max(6, buff_px_))),
               (bottom - top - gap * (rows - 1)) / static_cast<float>(rows));
  if (side < ro::Px(6.0f)) return right;  // tuile trop basse : rien à y mettre

  // Le compte maximum se répartit sur les lignes — six icônes sur deux lignes
  // font trois par ligne, et non six puis six.
  const int per_row = (std::max(1, buff_max_) + rows - 1) / rows;

  float x = right;
  float leftmost = right;
  int drawn = 0;

  // 🔴 On parcourt À REBOURS : `Effects` rend les états dans l'ordre où ils sont
  // arrivés, et quand il y en a plus que la place, ce sont les PLUS RÉCENTS
  // qu'il faut garder — un buff qui vient de tomber sur un allié est ce qu'on
  // regarde, pas celui qui dure depuis dix minutes.
  for (size_t i = list.size(); i-- > 0 && drawn < std::max(1, buff_max_);) {
    // ⚠ Pas d'infobulle ici : la tuile a déjà la sienne, qui porte tout le
    // membre. Une seconde par-dessus se disputerait le même survol.
    statuscell::Style st;
    st.sweep       = buff_sweep_;
    st.sweep_color = IM_COL32(0, 0, 0, 140);
    // La moitié de l'icône, plancher à 7 px : un texte de taille fixe débordait
    // sur la rangée du dessous dès qu'on réduisait les icônes.
    st.time_px     = buff_time_ ? std::max(ro::Px(7.0f), side * 0.5f) : 0.0f;
    const int row = drawn / per_row;
    // Nouvelle ligne : on repart du bord droit, une rangée plus bas.
    if (drawn > 0 && drawn % per_row == 0) x = right;
    const float y = top + row * (side + gap);
    if (!statuscell::Draw(list[i], ImVec2(x - side, y), ImVec2(x, y + side), st,
                          false))
      continue;
    x -= side + gap;
    if (x < leftmost) leftmost = x;
    ++drawn;
  }

  // La limite rendue est le point le plus à GAUCHE de TOUTES les lignes : le
  // texte se découpe sur la plus longue, sinon il passerait sous la seconde.
  return (drawn > 0) ? (leftmost - gap) : right;
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

  // ── Le liseré, par ordre de priorité ─────────────────────────────────────
  //
  // Trois états peuvent l'allumer, et l'ordre traduit l'urgence de l'information :
  //   1. SURVOLÉ — sur qui le prochain sort partira. C'est du présent immédiat,
  //      donc ça prime sur tout le reste.
  //   2. CIBLÉ — la cible courante du jeu, celle que le reste de l'interface
  //      montre. Un blanc plus discret : c'est un état, pas une intention.
  //   3. MOI — repère permanent, la couleur réglable.
  const bool hovered  = (hovered_gid_ != 0 && hovered_gid_ == m.gid);
  const bool targeted = (target_gid_ != 0 && target_gid_ == m.gid);
  if (hovered) {
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 210), rounding, 0, 2.0f);
  } else if (targeted) {
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 165), rounding, 0, 2.0f);
  } else {
    dl->AddRect(p0, p1, is_me ? Col(col_me_) : IM_COL32(0, 0, 0, 190), rounding,
                0, is_me ? 2.0f : 1.0f);
  }

  // ── Les buffs, calés à droite ────────────────────────────────────────────
  //
  // Placés AVANT le texte pour lui rendre sa limite : le nom se découpe sur ce
  // que les icônes laissent, au lieu de passer dessous.
  float text_right = p1.x - pad;
  if (show_buffs_ && !m.offline)
    text_right = DrawTileEffects(m.gid, p1.x - pad, p0.y + pad, bottom - pad);

  // ── Le texte : nom, puis l'état ou les PV ────────────────────────────────
  //
  // Trois couleurs, parce qu'il y a trois situations et qu'en confondre deux
  // trompe le lecteur : présent (couleur normale), présent mais HORS DE PORTÉE
  // (bleu pâle — il est là, on ne sait juste pas ses PV), et HORS LIGNE (gris
  // terne — il n'y a rien à en attendre).
  const ImU32 text = m.offline ? Col(col_offline_)
                   : !has_hp   ? Col(col_far_)
                               : Col(col_text_);

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

  // 🔴 Le texte est DÉCOUPÉ à la tuile. Un nom long débordait sur la tuile
  // voisine — et sur une grille dense, un nom qui déborde se lit comme s'il
  // appartenait à la case d'à côté : pire qu'un nom tronqué. `AddText` accepte un
  // rectangle de découpe FIN (au pixel), ce qui évite d'avoir à mesurer et à
  // couper la chaîne nous-mêmes.
  //
  // Ce qui dépasse est donc perdu : c'est à ça que sert l'infobulle, qui redonne
  // la ligne entière.
  const ImVec4 clip(text_x, p0.y, text_right, bottom);
  dl->AddText(font, fsz, ImVec2(text_x, ty), text, first, nullptr, 0.0f, &clip);
  if (two_lines) {
    dl->AddText(font, fsz, ImVec2(text_x, ty + line), text, second, nullptr,
                0.0f, &clip);
  }
}
