#include "features/windows/party_friend_window.h"

#include <cfloat>  // FLT_MAX
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"  // OpenInterfaceSection (bullet)
#include "features/overlays/party_frames.h"      // le cache de SP, partagé
#include "features/overlays/target_frame.h"      // cibler par le chemin clavier
#include "features/windows/entity_context_menu.h"  // le menu du personnage
#include "imgui.h"
#include "ragnarok/actor.h"      // rag::actor::kJobId (le job vient de l'ACTEUR)
#include "ragnarok/game_scene.h"
#include "ragnarok/globals.h"
#include "ragnarok/stl_node.h"  // treenode:: (le std::map des positions de groupe)
#include "ragnarok/msgstring.h"      // msgstr::Utf8Or (libellés exacts du client)
#include "ragnarok/ui_window_mgr.h"  // UIM_MAKE_WHISPER_WINDOW (ouverture d'un 1:1)
#include "ragnarok/uiwnd.h"
#include "ui/game_texture.h"  // ro::CachedTextureFromGameFile (icônes du client)
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"
#include "ragnarok/social.h"  // lecture des listes, noms de classe, icones

namespace {

// (`g_Own_InParty` 0x015FF804 existe et garde les cases 0x3D / 0x3E du switch
// natif, mais il reste à 0 pour un membre qui a REJOINT un groupe — inutilisable
// pour savoir si l'on est en groupe. Voir DrawPartyTab.)

// ── Les commandes du client ──────────────────────────────────────────────────
//
// `CMode::SendMsg(cmd, p2..p5)` sur le mode de zone courant, vtbl+0x18. Rend
// false si aucun mode n'est actif (login, changement de carte).
// (Ce petit pont est déjà recopié dans chat_window et game_settings ; il mériterait
// la factorisation qu'a reçue `uiwnd.h`. Hors périmètre de ce chantier.)

// Les commandes reprises du hub natif (docs/party_friend_re.md §6).
constexpr int kCmdMakeLeader   = 0x104;  // -> CZ_PARTY_CHANGE_LEADER 0x07DA (AID)
constexpr int kCmdRemoveFriend = 0x0b0;  // -> CZ_DELETE_FRIENDS      0x0203 (AID, CID)
constexpr int kCmdLeaveParty   = 0x03d;  // -> CZ_REQ_LEAVE_GROUP     0x0100
// Invitation PAR NOM. C'est le chemin que le menu contextuel d'entité emprunte
// déjà (chat_window) : le case 0x3B compose CZ_PARTY_JOIN_REQ 0x02C4 {nom[24]}.
constexpr int kCmdPartyInvite  = 0x03b;
// Réponses aux demandes REÇUES (boutons 184 = oui / 185 = non des natives).
constexpr int kCmdAnswerParty  = 0x03c;  // -> CZ 0x02C7 (partyid, oui/non)
constexpr int kCmdAnswerFriend = 0x0af;  // -> CZ 0x0208 (AID, CID, oui/non)

// ── Les réglages du groupe ───────────────────────────────────────────────────
//
// `SendMsg(0x103, exp, pickup, share)` -> CZ 0x07D7 { exp:4 @2, pickup:1 @6,
// share:1 @7 }. C'est l'appel EXACT de la fenêtre native (@0x008c684e), dont les
// trois arguments sont ses sélections `this+0xBC / +0xC8 / +0xD4`, comparées aux
// trois globales ci-dessous.
//
// 🔴 Le serveur (moonlight `clif_parse_PartyChangeOption`) EXIGE d'être chef et
// recompose `itemflag = pickup | share<<1`. Un non-chef qui envoie ce paquet est
// simplement ignoré — d'où le grisage côté interface, qui évite un clic sans effet.
constexpr int kCmdPartyOptions = 0x103;
// L'état COURANT, tenu par les handlers de paquets du client.
// ⚠ L'ordre a été TRANCHÉ par les clés msgstring des radios, pas deviné :
// `MSI_EXPDIV` / `MSI_ITEMCOLLECT` (= ramassage) / `MSI_ITEMDIV` (= partage).
constexpr uintptr_t kOptExpAddr    = 0x015ff840;
constexpr uintptr_t kOptPickupAddr = 0x015ff844;
constexpr uintptr_t kOptShareAddr  = 0x015ff848;
// Les libellés des radios, dans l'ordre où OnCreate les crée.
constexpr int kMsgExpEach     = 0x11f;  // « Each Take »  -> individuel
constexpr int kMsgExpShared   = 0x120;  // « Even Share »
constexpr int kMsgPickEach    = 0x121;  // « Each Take »
constexpr int kMsgPickShared  = 0x122;  // « Party Share »
constexpr int kMsgDivEach     = 0x2e3;  // « Individual »
constexpr int kMsgDivShared   = 0x2e4;  // « Shared »

// Les deux paquets qui DEMANDENT quelque chose au joueur.
// ⚠ 0x02C6 et non 0x00FE : le serveur bascule dès PACKETVER >= 20070821.
constexpr uint16_t kOpPartyJoinReq = 0x02c6;  // { partyid:4, groupName[24] }
constexpr uint16_t kOpFriendReq    = 0x0207;  // { AID:4, CID:4, name[24] }

// Les libellés EXACTS du client, ceux que les natives composaient :
//   0x5E  suit le nom du groupe (« … vous invite… »)
//   0x332 le message complet de la demande d'amitié
constexpr int kMsgPartyInviteText  = 0x5e;
constexpr int kMsgFriendRequestText = 0x332;

// `FriendList_AddByName(nom24)` __stdcall : construit et envoie CZ_ADD_FRIENDS
// (0x0202, nom sur 24 octets). Passer par elle évite d'avoir à trancher entre les
// opcodes que le serveur accepte — et ce chemin est déjà éprouvé en jeu.
// ⚠ ELLE LIT 24 OCTETS D'AFFILÉE : le tampon doit les porter.

void AddFriendSEH(const char* name24) {
  __try {
    using FriendAddFn = int(__stdcall*)(const void*);
    reinterpret_cast<FriendAddFn>(rag::social::kFriendListAddByNameAddr)(name24);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── L'art du client, tel que le natif le compose ─────────────────────────────
//
// Racine des bitmaps d'interface, en CP949 (유저인터페이스), en octets verbatim :
// ce fichier est en UTF-8 et le client attend SA code-page.

// L'icône de classe. Le natif la pose sur ses 40 boutons de job (50×50) au msg
// 0x17 : `sprintf("%sicon_jobs_%d.bmp", "\renewalparty\", job)` @0x0070622a, où
// le job est lu en `[esi+50h]` — nœud+0x50, donc data+0x48. La variante `_die`
// existe (mort), pilotée par un flag distinct de « hors ligne » que nous n'avons
// pas encore identifié : on ne l'utilise donc pas.
// (Le natif a aussi `\renewalparty\icon_party_me|on|off.bmp` pour la pastille de
// statut. On ne s'en sert PAS : ~11 px de haut, ces bitmaps deviennent illisibles
// dès qu'on les met à l'échelle de l'interface. La pastille est dessinée — voir
// DrawPartyRow.)

// Taille à laquelle on affiche l'icône de classe. Le natif dimensionne ses boutons
// à 50×50 ; on suit, mis à l'échelle de l'interface.
constexpr float kJobIconSize = 40.0f;

// Le champ map d'une entrée arrive tel que le serveur l'a envoyé — en pratique
// « prontera.gat ». `rag::MapDisplayName` veut le nom NU : elle recolle « .rsw »
// elle-même avant d'interroger la table, et un « prontera.gat.rsw » n'y trouve
// rien (échec SILENCIEUX, elle rend juste false). On coupe donc à la première
// extension, quelle qu'elle soit.
void StripMapExtension(const char* in, char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!in) return;
  size_t i = 0;
  for (; i + 1 < cap && in[i] && in[i] != '.'; ++i) out[i] = in[i];
  out[i] = '\0';
}

// ── La pastille de statut (ME / ON / OFF) ────────────────────────────────────
//
// 🔴 DESSINÉE, pas blittée — le seul endroit où l'on s'écarte de l'art du client.
// `icon_party_me|on|off.bmp` fait ~11 px de haut : mis à l'échelle de l'interface
// (facteur non entier) et posé à une position fractionnaire, le filtrage bilinéaire
// mange les jambages — le « M » de ME en ressortait déformé. Un texte reste net à
// n'importe quelle échelle, et le rôle de la pastille (se repérer d'un coup d'œil)
// prime ici sur la fidélité au pixel.
//
// Se pose seule à droite de la ligne courante, comme le natif qui la met à 80 % de
// la largeur.
void DrawStatusBadge(const char* txt, ImU32 bg, const char* tooltip) {
  ImGui::SameLine();
  // ⚠ Mesurer la place restante APRÈS le SameLine : avant, on obtiendrait la
  // largeur sous le bloc, pas celle qui reste sur la ligne.
  const float rest   = ImGui::GetContentRegionAvail().x;
  const ImVec2 ts    = ImGui::CalcTextSize(txt);
  const float pad_x  = ro::Px(5.0f);
  const float pad_y  = ro::Px(1.0f);
  const ImVec2 size(ts.x + pad_x * 2.0f, ts.y + pad_y * 2.0f);
  const float margin = ro::Px(4.0f);
  if (rest > size.x + margin)
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + rest - size.x - margin);
  // Position arrondie au pixel : un rectangle et un texte posés sur une demi-frame
  // baveraient exactement comme le bitmap qu'on vient d'abandonner.
  ImVec2 p = ImGui::GetCursorScreenPos();
  p.x = static_cast<float>(static_cast<int>(p.x));
  p.y = static_cast<float>(static_cast<int>(p.y));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, ro::Px(3.0f));
  dl->AddText(ImVec2(p.x + pad_x, p.y + pad_y), IM_COL32_WHITE, txt);
  ImGui::Dummy(size);
  if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
}

// Placement par défaut de la fenêtre. Le natif se dimensionne à 273×326 quand il
// est maximisé (champ +0x268) ; on part de là, en un peu plus large puisqu'une
// table ImGui porte des en-têtes de colonnes que le natif n'a pas.
constexpr float kSpawnX = 420.0f;
constexpr float kSpawnY = 120.0f;
constexpr float kSpawnW = 330.0f;
constexpr float kSpawnH = 326.0f;

}  // namespace

PartyFriendWindow::PartyFriendWindow() {
  // 🔴 REMPLACEMENT, pas observation : ces deux paquets sont les SEULS créateurs
  // des fenêtres natives 35 et 109. En prenant leur place, ces fenêtres ne
  // naissent jamais — au lieu de naître, d'être masquées, puis détruites au tick.
  // La différence n'est pas cosmétique : une native masquée garde le CLAVIER et
  // son bouton par défaut est « Accepter ». Une frappe d'Entrée destinée au chat
  // rejoindrait un groupe ou accepterait un inconnu (même piège que le refine et
  // l'échange, docs/make_item_list_re.md §12.5).
  //
  // Le prédicat est relu à CHAQUE paquet : interrupteur éteint, les popups
  // natives reprennent la main entièrement.
  const auto claim = [this] { return imgui_enabled_; };
  Bourgeon::Instance().RegisterReplaceOpcode(kOpPartyJoinReq, claim);
  Bourgeon::Instance().RegisterReplaceOpcode(kOpFriendReq, claim);
}

// 🔴 FIL RÉSEAU : on ne fait que copier. Le décodage a lieu dans HandlePacket,
// sur le fil principal (cf. features/net_inbox.h).
void PartyFriendWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                     uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

void PartyFriendWindow::HandlePacket(uint16_t opcode, const uint8_t* data,
                                     uint16_t len) {
  // `data` commence APRÈS les 2 octets d'opcode (régime RegisterReplaceOpcode).
  char name[32] = {0};
  if (opcode == kOpPartyJoinReq) {
    // ZC_PARTY_JOIN_REQ { partyid:4, groupName[24] } — le nom est celui du
    // GROUPE, pas celui de qui invite ; le natif affiche « <groupe> <msg 0x5E> ».
    if (len < 4 + 24) return;
    invite_ = InviteRequest{};
    std::memcpy(&invite_.party_id, data, 4);
    std::memcpy(name, data + 4, 24);
    name[24] = '\0';
    invite_.is_friend = false;
  } else if (opcode == kOpFriendReq) {
    // ZC_REQ_ADD_FRIENDS { AID:4, CID:4, name[24] } — ici c'est bien le nom du
    // JOUEUR qui demande.
    if (len < 4 + 4 + 24) return;
    invite_ = InviteRequest{};
    std::memcpy(&invite_.aid, data, 4);
    std::memcpy(&invite_.cid, data + 4, 4);
    std::memcpy(name, data + 8, 24);
    name[24] = '\0';
    invite_.is_friend = true;
  } else {
    return;
  }
  invite_.name       = name;   // code-page du CLIENT ; convertie à l'affichage
  invite_.active     = true;
  open_invite_popup_ = true;
  // ⚠ On n'ouvre PAS la fenêtre Amis/Groupe au passage : la popup se dessine
  // indépendamment d'elle (cf. OnRenderUI), et le natif ne faisait pas plus —
  // il n'affichait qu'un dialogue. Ouvrir une grande fenêtre par-dessus le jeu
  // parce que quelqu'un vous invite serait plus intrusif que ce qu'on remplace.
}

// ── Lecture des listes ───────────────────────────────────────────────────────

void PartyFriendWindow::ReadList(bool party, std::vector<rag::social::Entry>& out) {
  // Les PV des membres visibles sont remplis au passage, comme le fait
  // UpdateMemberHpGauges : c'est `social` qui sait ou les prendre.
  if (party)
    rag::social::ReadParty(out);
  else
    rag::social::ReadFriends(out);
  // Suis-je le chef ? C'est ce qui ouvre « nommer chef » et « expulser ». On le
  // recalcule à chaque relecture : le commandement peut changer sans nous
  // prévenir (le chef part, le serveur le transmet).
  if (party) {
    const uint32_t mine = rag::social::OwnAid();
    i_am_leader_ = false;
    for (const rag::social::Entry& r : out) {
      if (r.gid == mine) { i_am_leader_ = r.is_leader; break; }
    }
  }
}

// ── Les gestes demandés par une AUTRE surface ───────────────────────────────
//
// Le HUD en grille n'a qu'un GID ; le nom, lui, est nécessaire (le chuchotement
// et l'expulsion voyagent PAR NOM). On le retrouve dans la liste courante — et
// s'il n'y est plus, on n'arme rien : agir sur un membre qui vient de partir
// n'aurait pas de sens.
bool PartyFriendWindow::ArmForGid(uint32_t gid, Action action) {
  // 🔴 On RELIT la liste au lieu de consulter `party_` : ce membre-là n'est
  // rempli que par le RENDU de cette fenêtre. L'appelant est le HUD en grille,
  // qui sert précisément quand la fenêtre est FERMÉE — `party_` y serait vide,
  // et l'action échouerait sans un mot.
  rag::social::Entry row;
  if (!rag::social::FindPartyMember(gid, &row)) return false;
  pending_      = action;
  pending_gid_  = row.gid;
  pending_id2_  = row.id2;
  pending_name_ = row.name;
  return true;
}

void PartyFriendWindow::RequestWhisper(uint32_t gid) {
  ArmForGid(gid, Action::kWhisper);
}

void PartyFriendWindow::RequestMakeLeader(uint32_t gid) {
  // Les mêmes droits que le menu de la fenêtre : le serveur refuserait de toute
  // façon, mais mieux vaut ne pas émettre une commande qu'on sait vaine.
  // ⚠ `IsPartyLeader()` et non `i_am_leader_` : ce membre n'est à jour que
  // pendant le rendu de la fenêtre, et l'appelant est le HUD en grille.
  if (!IsPartyLeader()) return;
  ArmForGid(gid, Action::kMakeLeader);
}

void PartyFriendWindow::RequestKick(uint32_t gid) {
  if (!IsPartyLeader()) return;
  ArmForGid(gid, Action::kKick);
}

void PartyFriendWindow::RequestRemoveFriend(uint32_t gid) {
  // Même raison qu'`ArmForGid` de relire : `friends_` n'est rempli que par le
  // rendu de cette fenêtre, et l'appelant est le menu contextuel du monde.
  std::vector<rag::social::Entry> list;
  rag::social::ReadFriends(list);
  for (const rag::social::Entry& f : list) {
    if (f.gid != gid) continue;
    pending_      = Action::kRemoveFriend;
    pending_gid_  = f.gid;
    pending_id2_  = f.id2;
    pending_name_ = f.name;
    return;
  }
}

void PartyFriendWindow::RequestEntityMenu(uint32_t gid) {
  // Pas d'`ArmForGid` ici : ce geste ne vise pas un membre de la LISTE mais une
  // entité du monde, et il n'a besoin d'aucun nom — `FlushPending` relit le job
  // sur l'acteur lui-même.
  if (gid == 0 || gamescene::FindActorByGid(gid) == nullptr) return;
  pending_     = Action::kEntityMenu;
  pending_gid_ = gid;
}

void PartyFriendWindow::FlushPending() {
  // Les lignes de chat d'abord : elles rejouent du code natif, donc jamais depuis
  // la frame ImGui. L'aiguillage de Bourgeon les route vers la chatbox moderne.
  for (const std::string& line : chat_queue_) {
    UIWindowMgr::SendMsg(UIMessage::UIM_PUSHINTOCHATHISTORY,
                         reinterpret_cast<int>(line.c_str()), 0x88CCFF, 0, 0);
  }
  chat_queue_.clear();

  const Action act = pending_;
  if (act == Action::kNone) return;
  // Désarmer AVANT d'agir : une commande native qui ouvre une modale peut nous
  // faire repasser ici, et rejouer l'action une seconde fois.
  pending_ = Action::kNone;

  switch (act) {
    case Action::kWhisper: {
      // Le chemin du natif : `UIM_MAKE_WHISPER_WINDOW`. ChatWindow le détourne
      // quand la chatbox moderne est active et ouvre SA fenêtre ; sinon le client
      // ouvre la sienne. Un seul appel couvre donc les deux modes.
      // ⚠ Le nom part dans la code-page du CLIENT, pas en UTF-8.
      UIWindowMgr::SendMsg(UIMessage::UIM_MAKE_WHISPER_WINDOW,
                           reinterpret_cast<int>(pending_name_.c_str()), 0, 0, 0);
      break;
    }
    case Action::kMakeLeader:
      rag::SendToActiveMode(kCmdMakeLeader, static_cast<int>(pending_gid_));
      break;
    case Action::kRemoveFriend:
      rag::SendToActiveMode(kCmdRemoveFriend, static_cast<int>(pending_gid_),
                  static_cast<int>(pending_id2_));
      break;
    case Action::kLeaveParty:
      // 🔴 On passe par la COMMANDE, pas par le paquet : le case 0x3D ne se
      // contente pas d'envoyer 0x0100, il cherche d'abord un membre en ligne sur
      // ma carte et lui TRANSFÈRE le leadership. Envoyer le paquet nu laisserait
      // le groupe sans chef, là où le client officiel passe la main.
      rag::SendToActiveMode(kCmdLeaveParty);
      break;
    case Action::kKick: {
      // CZ_REQ_EXPEL_GROUP_MEMBER 0x0103 : { opcode:2, AID:4, nom:24 } = 30 o.
      // Construit ici plutôt que par la commande native 0x3E, dont l'ordre des
      // paramètres n'a pas été établi ; la forme du paquet, elle, est confirmée
      // par le désassemblage ET par Hercules (pRemovePartyMember, 2, 6).
      // Le serveur revalide tout (droits de chef compris) : rien à exploiter.
      uint8_t pkt[30] = {0};
      const uint16_t op = 0x0103;
      std::memcpy(pkt + 0, &op, 2);
      std::memcpy(pkt + 2, &pending_gid_, 4);
      // Le nom est un champ FIXE de 24 octets, complété de zéros ; on tronque
      // sans jamais déborder, et le reste du tampon est déjà à zéro.
      size_t n = pending_name_.size();
      if (n > 24) n = 24;
      std::memcpy(pkt + 6, pending_name_.data(), n);
      Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
      break;
    }
    case Action::kCreateParty: {
      // CZ_MAKE_GROUP2 0x01E8 : { opcode:2, nom[24] @+2, exp:1 @+26, item:1 @+27 }
      // = 28 o. Relevé sur le case 168 (0xA8) du switch @0x00c8d066, qui compose
      // exactement ces champs ; Hercules confirme l'opcode (pCreateParty2).
      // Les deux drapeaux valent 0 = partage INDIVIDUEL, le défaut du jeu ; le
      // groupe se règle ensuite par la fenêtre de réglages (lot 2).
      uint8_t pkt[28] = {0};
      const uint16_t op = 0x01e8;
      std::memcpy(pkt + 0, &op, 2);
      size_t n = pending_name_.size();
      if (n > 24) n = 24;
      std::memcpy(pkt + 2, pending_name_.data(), n);
      Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
      new_party_name_[0] = '\0';
      break;
    }
    case Action::kInviteParty:
      // Le client sait inviter PAR NOM ; on lui passe le pointeur, comme le fait
      // déjà le menu contextuel d'entité.
      rag::SendToActiveMode(kCmdPartyInvite,
                  static_cast<int>(reinterpret_cast<intptr_t>(pending_name_.c_str())));
      invite_name_[0] = '\0';
      break;
    case Action::kAddFriend: {
      // ⚠ 24 octets D'AFFILÉE : on recopie dans un tampon qui les porte, plutôt
      // que de donner le buffer d'une std::string dont la capacité peut être
      // plus courte (SSO de 15 caractères).
      char name24[32] = {0};
      std::strncpy(name24, pending_name_.c_str(), sizeof(name24) - 1);
      AddFriendSEH(name24);
      friend_name_[0] = '\0';
      break;
    }
    case Action::kAnswerParty:
      // Exactement ce que fait le bouton 184/185 de la native : la commande 0x3C
      // avec le partyid et 1 (accepter) ou 0 (refuser).
      rag::SendToActiveMode(kCmdAnswerParty, static_cast<int>(pending_gid_),
                  pending_accept_ ? 1 : 0);
      break;
    case Action::kAnswerFriend:
      rag::SendToActiveMode(kCmdAnswerFriend, static_cast<int>(pending_gid_),
                  static_cast<int>(pending_id2_), pending_accept_ ? 1 : 0);
      break;
    case Action::kPartyOptions:
      // Même appel que la fenêtre native (@0x008c684e) : les trois sélections
      // dans l'ordre exp / ramassage / partage.
      rag::SendToActiveMode(kCmdPartyOptions, opt_exp_, opt_pickup_, opt_share_);
      break;
    case Action::kTargetMember:
      // Par le chemin CLAVIER de TargetFrame : un allié n'est pas une cible
      // « valide » au sens du clic natif, donc rejouer un clic ne ferait rien.
      if (auto* tf = Bourgeon::Instance().target_frame())
        tf->RequestTargetFromProxy(pending_gid_);
      break;
    case Action::kEntityMenu: {
      // Le menu contextuel du CLIENT sur ce personnage, comme sur son sprite.
      // C'est `EntityContextMenu` qui décide de son contenu et de ce qu'il grise
      // — on ne lui donne que la cible.
      void* gm = rag::ActiveModeSafe();
      void* actor = gamescene::FindActorByGid(pending_gid_);
      if (gm != nullptr && actor != nullptr) {
        // 🔴 Le job vient de l'ACTEUR, jamais de l'entrée sociale. Une entrée
        // d'AMI n'en porte pas — le serveur n'envoie que nom, AID et CID pour
        // cette liste — et un job à 0 fait conclure au menu qu'il s'agit d'un
        // MONSTRE : il proposait « Attaquer », « Fiche du monstre »… sur un ami.
        uint32_t job = 0;
        __try {
          job = static_cast<uint32_t>(*reinterpret_cast<const int32_t*>(
              reinterpret_cast<const uint8_t*>(actor) + rag::actor::kJobId));
        } __except (EXCEPTION_EXECUTE_HANDLER) { job = 0; }
        if (auto* ctx = Bourgeon::Instance().entity_context_menu()) {
          ctx->OpenForEntity(gm, pending_gid_, job, gamescene::kPickActor);
        }
      }
      break;
    }
    case Action::kNone:
      break;
  }

  pending_gid_ = 0;
  pending_id2_ = 0;
  pending_name_.clear();
}

// ── Bascule natif / ImGui ────────────────────────────────────────────────────

void PartyFriendWindow::KillNative(bool adopt_open_state) {
  if (!uiwnd::FindWindow(uiwnd::kMessengerGroupWndId)) return;
  // Sa présence PROUVE que le joueur avait la fenêtre ouverte : on adopte l'état
  // avant de détruire, sinon activer le mode moderne la ferait disparaître.
  if (adopt_open_state && !open_) {
    open_ = true;
    need_pos_ = true;
  }
  // 🔴 DÉTRUIRE, pas masquer : toute bascule du client fait « ferme si elle
  // existe, sinon crée ». Une native seulement masquée existe encore, donc la
  // demande suivante la fermerait sans repasser par MakeWindow — un appui sur
  // deux avalé — et elle garderait le clavier.
  __try {
    uiwnd::CloseWindow(uiwnd::kMessengerGroupWndId);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void PartyFriendWindow::ToggleFromUi() {
  if (!imgui_enabled_) return;
  open_ = !open_;
  if (open_) need_pos_ = true;
}

void PartyFriendWindow::HandleNativeCreation(void* win, bool user_gesture) {
  if (!imgui_enabled_) return;

  // Masquer AVANT sa première frame : OnTick la détruira, mais il ne passe que
  // toutes les ~100 ms et elle serait visible d'ici là.
  uiwnd::SafeSetVisible(win, false);

  if (user_gesture) {
    // Geste du joueur. Le client fait « ferme si elle existe, sinon crée » : la
    // native étant détruite, elle n'existe jamais et la demande arrive toujours
    // ici — c'est donc à nous de porter la bascule.
    open_ = !open_;
    if (open_) need_pos_ = true;
    return;
  }
  // Le CLIENT fabrique la fenêtre de lui-même (création de groupe, jonction) : il
  // veut l'afficher, jamais la fermer. Une bascule ici la faisait disparaître sous
  // le joueur au moment précis où elle devenait intéressante.
  if (!open_) {
    open_ = true;
    need_pos_ = true;
  }
}

void PartyFriendWindow::OnTick() {
  const bool mode_changed = (imgui_enabled_ != prev_imgui_enabled_);
  prev_imgui_enabled_ = imgui_enabled_;

  if (!imgui_enabled_) {
    // Retour au natif : notre fenêtre s'efface et la native reprend son service à
    // la prochaine demande (elle n'existe plus, donc le client la recréera).
    open_ = false;
    return;
  }
  if (Bourgeon::Instance().IsMapLoading()) return;
  KillNative(mode_changed);
  // Les réglages du groupe peuvent changer sans nous : c'est ici qu'on s'en
  // aperçoit, fenêtre ouverte ou non.
  PollPartyOptions();
}

// ── Rendu ────────────────────────────────────────────────────────────────────

void PartyFriendWindow::OnRenderUI() {
  if (!imgui_enabled_) return;

  // 🔴 HORS de la fenêtre, et AVANT elle : une demande reçue doit pouvoir être
  // répondue même quand la fenêtre Amis/Groupe est fermée ou REPLIÉE — dans ces
  // deux cas `BeginRoWindow` rend false, et une popup dessinée à l'intérieur
  // n'existerait tout simplement pas. Le joueur resterait avec une demande sans
  // aucun moyen d'y répondre, et l'inviteur sans réponse.
  DrawInvitePopup();

  if (!open_) return;

  if (need_pos_) {
    // FirstUseEver : simple DÉFAUT de première ouverture ; ensuite ImGui garde la
    // position déplacée par le joueur. Ce défaut se lisait sur la fenêtre native,
    // qui ne naît plus — on le fixe, rabattu dans l'écran en petite résolution.
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(
        ImVec2(std::min(kSpawnX, std::max(0.0f, screen.x - kSpawnW)),
               std::min(kSpawnY, std::max(0.0f, screen.y - kSpawnH))),
        ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(kSpawnW, kSpawnH), ImGuiCond_FirstUseEver);
  // 🔴 UN PLANCHER, sinon la fenêtre descend aux 32x32 d'ImGui — il n'y reste
  // que deux icônes et plus rien de lisible. Le plafond est posé ailleurs (cf.
  // ro::BeginRoWindow) ; le minimum, lui, n'avait jamais existé.
  // La valeur tient au contenu : les DEUX onglets côte à côte, et une ligne de
  // membre — icône de job de 40 px, nom, niveau. En dessous, la fenêtre ne
  // montre plus ce pour quoi on l'ouvre.
  ImGui::SetNextWindowSizeConstraints(ImVec2(ro::Px(260.0f), ro::Px(150.0f)),
                                      ImVec2(FLT_MAX, FLT_MAX));

  // Bullet de la barre de titre = raccourci vers la config de CETTE fenêtre.
  ro::SetNextWindowTitleBullet(i18n::Tr("Réglages Groupe / Amis"));
  const bool begun = ro::BeginRoWindow(i18n::Tr("Groupe / Amis"), &open_);
  if (ro::TitleBulletClicked()) {
    if (auto* mu = Bourgeon::Instance().moonlight_ui())
      mu->OpenInterfaceSection(MoonlightUi::kIfacePartyFriend);
  }
  if (begun) {
    // Les deux onglets du natif, dans le même ordre et avec le même sens de
    // `cur_tab_` que son champ +0x28C (0 = amis, 1 = groupe).
    if (ro::RoToggleButton(i18n::Tr("Groupe"), cur_tab_ == 1)) cur_tab_ = 1;
    ImGui::SameLine();
    if (ro::RoToggleButton(i18n::Tr("Amis"), cur_tab_ == 0)) cur_tab_ = 0;
    ImGui::Separator();

    // Relu à CHAQUE frame, comme le fait DrawContent : le manager est la source de
    // vérité et rien ne nous prévient qu'il a changé.
    if (cur_tab_ == 1) {
      ReadList(true, party_);
      DrawPartyTab();
    } else {
      ReadList(false, friends_);
      DrawFriendTab();
    }
    // La confirmation vit AU NIVEAU DE LA FENÊTRE, hors du PushID des lignes.
    DrawConfirmPopup();
  }
  ro::EndRoWindow();
}

void PartyFriendWindow::DrawPartyTab() {
  // ── Créer / Quitter ───────────────────────────────────────────────────────
  //
  // Un seul bouton, qui change de rôle selon qu'on est déjà dans un groupe.
  //
  // 🔴 NE PAS se fier à `g_Own_InParty` (0x015FF804) ici, bien que ce soit le
  // drapeau que le natif teste avant d'autoriser « quitter » et « expulser ».
  // MESURÉ EN JEU le 2026-08-23 : il reste à **0** pour un membre qui a REJOINT un
  // groupe (il n'est vrai que pour celui qui l'a créé, semble-t-il) — le bouton
  // « Quitter » n'apparaissait donc jamais pour un simple membre. Six fonctions
  // l'écrivent (0x00cb6146, 0x00ced926, 0x00cf2790, 0x00cba855, 0x00cbb6e4,
  // 0x00cbfbdd) : soit celle qui le pose n'est pas déclenchée sur ce chemin, soit
  // le serveur n'envoie pas le paquet qui la déclenche. Non tranché.
  //
  // On s'appuie donc sur la MÊME source que la liste affichée juste en dessous :
  // si des membres sont à l'écran, le bouton dit « Quitter ». L'interface reste
  // ainsi cohérente avec elle-même, quoi que vaille le drapeau du client.
  if (party_.empty()) {
    ImGui::SetNextItemWidth(ro::Px(150.0f));
    // ⚠ Le nom part sur le fil : on le saisit dans la CODE-PAGE DU CLIENT.
    ImGui::InputText("##partyname", new_party_name_, sizeof(new_party_name_));
    ImGui::SameLine();
    const bool named = (new_party_name_[0] != '\0');
    if (!named) ImGui::BeginDisabled();
    if (ro::RoButton(i18n::Tr("Créer un groupe"))) {
      pending_      = Action::kCreateParty;
      pending_name_ = new_party_name_;
    }
    if (!named) ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::TextDisabled("%s", i18n::Tr("Vous n'êtes dans aucun groupe."));
    return;
  }

  // Inviter quelqu'un, par NOM. On ne grise pas le bouton pour les non-chefs :
  // c'est le SERVEUR qui tranche selon la configuration du groupe, et prétendre
  // le savoir ici reviendrait à interdire ce qu'il aurait autorisé.
  ImGui::SetNextItemWidth(ro::Px(150.0f));
  const bool invite_sent =
      ImGui::InputText("##invitename", invite_name_, sizeof(invite_name_),
                       ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  const bool can_invite = (invite_name_[0] != '\0');
  if (!can_invite) ImGui::BeginDisabled();
  const bool invite_clicked = ro::RoButton(i18n::Tr("Inviter"));
  if (!can_invite) ImGui::EndDisabled();
  if (can_invite && (invite_clicked || invite_sent)) {
    pending_      = Action::kInviteParty;
    pending_name_ = invite_name_;
  }

  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Quitter le groupe"))) {
    confirm_      = Action::kLeaveParty;
    confirm_gid_  = rag::social::OwnAid();
    confirm_name_.clear();
    open_confirm_ = true;
  }

  DrawPartyOptions();
  ImGui::Separator();

  // 🔴 L'espace entre lignes est l'ESPACEMENT D'ImGui, pas un bloc ajouté après
  // coup : un `Dummy` s'AJOUTAIT à `ItemSpacing.y`, si bien que le réglage à zéro
  // laissait encore ~4 px — il ne faisait donc pas ce que son nom promet. En
  // poussant le style, zéro veut vraiment dire « les lignes se touchent ».
  ImGui::PushStyleVar(
      ImGuiStyleVar_ItemSpacing,
      ImVec2(ImGui::GetStyle().ItemSpacing.x,
             ro::Px(static_cast<float>(std::max(0, row_spacing_)))));
  for (size_t i = 0; i < party_.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    DrawPartyRow(party_[i]);
    ImGui::PopID();
  }
  ImGui::PopStyleVar();

  // ── Le compteur du bas, comme le natif ────────────────────────────────────
  // `sprintf("%d/%d", PartyMemberCount(), 0x0C)` @0x00704820 : le maximum est
  // écrit EN DUR dans le client (12 = MAX_PARTY). On reprend sa constante plutôt
  // que d'en inventer une : si le serveur en autorisait moins, c'est quand même
  // ce nombre-là que le client montrerait partout ailleurs.
  // (Pas d'équivalent pour les amis : le client n'y affiche aucun plafond, et
  // MAX_FRIENDS est une valeur SERVEUR qu'on ne peut pas lire d'ici.)
  ImGui::TextDisabled("%s %d/%d", msgstr::Utf8Or(0xC9F, i18n::Tr("Membres :")),
                      static_cast<int>(party_.size()), rag::social::kMaxPartyMembers);
}

void PartyFriendWindow::DrawFriendTab() {
  // Ajouter un ami, par NOM. Le destinataire reçoit une demande à accepter — ce
  // n'est pas un ajout unilatéral, d'où l'absence de confirmation de notre côté.
  ImGui::SetNextItemWidth(ro::Px(150.0f));
  const bool add_sent =
      ImGui::InputText("##friendname", friend_name_, sizeof(friend_name_),
                       ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  const bool can_add = (friend_name_[0] != '\0');
  if (!can_add) ImGui::BeginDisabled();
  const bool add_clicked = ro::RoButton(i18n::Tr("Ajouter un ami"));
  if (!can_add) ImGui::EndDisabled();
  if (can_add && (add_clicked || add_sent)) {
    pending_      = Action::kAddFriend;
    pending_name_ = friend_name_;
  }
  ImGui::Separator();

  if (friends_.empty()) {
    ImGui::TextDisabled("%s", i18n::Tr("Votre liste d'amis est vide."));
    return;
  }
  for (size_t i = 0; i < friends_.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    DrawFriendRow(friends_[i]);
    ImGui::PopID();
  }

  // Le plafond vient du SERVEUR (MAX_FRIENDS), le client l'ignore : c'est un
  // ajout, pas une reprise du natif — lui n'affiche aucun compteur ici.
  ImGui::Separator();
  ImGui::TextDisabled("%s %d/%d", i18n::Tr("Amis :"),
                      static_cast<int>(friends_.size()), rag::social::kMaxFriends);
}

// ── Une ligne de GROUPE, calquée sur le natif ────────────────────────────────
//
//   [icône de classe]  Lv.N Nom(Carte)              [statut]
//                      [====barre de vie====] N/N
//
// Le natif compose exactement ces morceaux (DrawContent 0x00703d10) : bouton
// d'icône 50×50 à gauche, « Lv.%d » suivi de « %s(%s) » (nom, carte), la jauge
// enfant, puis « %d/%d » juste à droite d'elle, et la pastille de statut à 80 %
// de la largeur.
void PartyFriendWindow::DrawPartyRow(const rag::social::Entry& row) {
  const float icon = ro::Px(static_cast<float>(std::max(12, icon_px_)));
  char path[160];

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  // Largeur de la ligne, capturée AVANT le premier SameLine (elle sert au trait
  // de séparation, tout en bas, où le curseur est en fin de ligne).
  const float row_w = ImGui::GetContentRegionAvail().x;

  // ── L'icône de classe ──────────────────────────────────────────────────────
  //
  // 🔴 Sa PLACE est reservée ici, mais elle est DESSINÉE en fin de ligne : on ne
  // peut pas la centrer verticalement sur une hauteur qu'on ne connaît pas
  // encore. Posée d'emblée, elle restait collée en haut — et l'écart sautait aux
  // yeux dès qu'on changeait sa taille ou qu'on empilait les jauges.
  rag::social::JobIconPath(row.job, path, sizeof(path));
  const ro::GameTexture job_icon =
      show_job_icon_ ? ro::CachedTextureFromGameFile(path) : ro::GameTexture{};
  if (show_job_icon_) {
    ImGui::Dummy(ImVec2(icon, icon));
  }
  // Le natif pose le nom de classe sur son bouton d'icône
  // (`UITextButton_SetName`, DrawContent) : on le rend en infobulle, seul endroit
  // où il tient sans encombrer la ligne.
  // ⚠ Seulement quand l'infobulle de LIGNE est éteinte : elle porte déjà la
  // classe, et deux infobulles sur la même ligne se disputeraient l'affichage.
  if (show_job_icon_ && !show_tooltip_ && ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", ro::LocalToUtf8(rag::social::JobName(row.job)));
  if (show_job_icon_) ImGui::SameLine();

  ImGui::BeginGroup();

  // ── Ligne 1 : « Lv.N Nom(Carte) », précédé de la couronne du chef ──────────
  if (row.is_leader) {
    // Le natif dessine `ico_partyCrown.bmp` devant le nom du chef.
    std::snprintf(path, sizeof(path), "%s\\renewalparty\\ico_partyCrown.bmp",
                  ro::uipath::kUiRoot);
    const ro::GameTexture crown = ro::CachedTextureFromGameFile(path);
    if (crown.tex) {
      const float h = ImGui::GetTextLineHeight();
      ImGui::Image(reinterpret_cast<ImTextureID>(crown.tex), ImVec2(h, h));
      ImGui::SameLine(0.0f, 3.0f);
    }
  }

  char label[192];
  // Le texte du client arrive dans SA code-page : LocalToUtf8, jamais Cp949ToUtf8.
  const char* name_utf8 = ro::LocalToUtf8(row.name.c_str());
  char pretty[64] = {0};
  if (!row.map.empty()) {
    char bare[64] = {0};
    StripMapExtension(row.map.c_str(), bare, sizeof(bare));
    if (!rag::MapDisplayName(bare, pretty, sizeof(pretty)) || !pretty[0])
      std::snprintf(pretty, sizeof(pretty), "%s", row.map.c_str());
  }
  // Nom de carte COURT : ce qui précède la première virgule. Le client écrit
  // « Gonryun, the Hermit Land (Kunlun) » là où « Gonryun » suffit à se repérer,
  // et le complet pousse le reste de la ligne hors d'une fenêtre étroite.
  if (map_mode_ == kMapShort) {
    for (char* p = pretty; *p; ++p) {
      if (*p == ',' || (*p == ' ' && p[1] == '(')) { *p = '\0'; break; }
    }
  }
  // Le natif n'affiche la carte que pour un membre EN LIGNE qui en a une.
  char lvl[16] = {0};
  if (show_level_) std::snprintf(lvl, sizeof(lvl), "Lv.%u ", row.level);
  if (pretty[0] && !row.offline)
    std::snprintf(label, sizeof(label), "%s%s(%s)", lvl, name_utf8,
                  ro::LocalToUtf8(pretty));
  else
    std::snprintf(label, sizeof(label), "%s%s", lvl, name_utf8);

  if (row.offline) ImGui::TextDisabled("%s", label);
  else             ImGui::TextUnformatted(label);

  // ── Ligne 2 : la barre de vie, ou l'ÉTAT quand il n'y a pas de PV ─────────
  //
  // 🔴 PAS de barre quand les PV sont inconnus. Les PV viennent du `CPc` de
  // l'ACTEUR : un membre hors de portée n'en a aucun. Une jauge vide se lirait
  // comme « ce joueur est à zéro » — un contresens — et le « %d/%d » qui
  // l'accompagnait pouvait afficher des valeurs périmées quand l'acteur traînait
  // encore une vieille jauge. Un texte grisé dit exactement ce qu'il en est.
  // (Y remédier vraiment demanderait d'écouter ZC_NOTIFY_HP_TO_GROUPM 0x0106.)
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (row.offline) {
    ImGui::TextDisabled("%s", i18n::Tr("Hors ligne"));
  } else if (!row.has_hp || row.max_hp <= 0) {
    ImGui::TextDisabled("%s", i18n::Tr("Hors de portée"));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(
          "%s", i18n::Tr("Le client ne connaît les PV que des membres visibles."));
  } else {
    // ── Le SP, résolu AVANT de dessiner : il décide si les jauges se collent ──
    // Elle vient du HUD en grille, seul module à interroger le serveur (CZ
    // 0x0F29) : le faire une deuxième fois d'ici doublerait le trafic pour la
    // même information. On lui DÉCLARE le besoin, il s'occupe du reste.
    int sp = 0, maxsp = 0;
    if (show_sp_) {
      if (auto* frames = Bourgeon::Instance().party_frames()) {
        frames->RequestSpPolling();
        frames->MemberSp(row.gid, &sp, &maxsp);
      }
    }
    const bool has_sp = (maxsp > 0);
    // Collées : la jauge de PV ne doit pas pousser son texte à côté, sinon la
    // suivante ne tomberait pas juste dessous.
    const bool stack = bars_stacked_ && has_sp && show_hp_bar_;

    // 🔴 « Collées » ne tient pas au seul `Dummy` : entre deux widgets, ImGui
    // insère `ItemSpacing.y` (~4 px). Sans l'annuler, les jauges restaient
    // séparées d'un blanc et le réglage ne semblait rien faire.
    if (stack) {
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                          ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    }
    if (show_hp_bar_) {
      DrawRowBar(row.hp, row.max_hp, IM_COL32(64, 200, 72, 255), hp_text_mode_,
                 stack);
    } else if (hp_text_mode_ != kHpTextNone) {
      // Sans jauge, le chiffre reste : c'est lui qu'on est venu lire.
      const float f =
          static_cast<float>(row.hp) / static_cast<float>(row.max_hp);
      const int pct = (row.hp > 0) ? std::max(1, static_cast<int>(f * 100.0f)) : 0;
      if (hp_text_mode_ == kHpTextPercent)      ImGui::Text("%d %%", pct);
      else if (hp_text_mode_ == kHpTextBoth)    ImGui::Text("%d/%d (%d %%)", row.hp, row.max_hp, pct);
      else                                      ImGui::Text("%d/%d", row.hp, row.max_hp);
    }

    if (has_sp) {
      DrawRowBar(sp, maxsp, IM_COL32(70, 130, 220, 255), sp_text_mode_, false);
    }
    if (stack) ImGui::PopStyleVar();
  }
  (void)dl;

  ImGui::EndGroup();

  // ── L'icône, CENTRÉE sur la hauteur réelle de la ligne ───────────────────
  // Sa place a été réservée plus haut ; maintenant que le contenu est posé, on
  // connaît la hauteur sur laquelle la centrer. La ligne fait au moins la
  // hauteur de l'icône (le `Dummy` la garantit), d'où le `max`.
  if (show_job_icon_ && job_icon.tex) {
    const float content_h = std::max(icon, ImGui::GetItemRectSize().y);
    const ImVec2 i0(origin.x, origin.y + (content_h - icon) * 0.5f);
    // Hors ligne : la même icône, assombrie — le natif grise toute la ligne.
    const ImU32 tint = row.offline ? IM_COL32(140, 140, 140, 220) : IM_COL32_WHITE;
    ImGui::GetWindowDrawList()->AddImage(
        reinterpret_cast<ImTextureID>(job_icon.tex), i0,
        ImVec2(i0.x + icon, i0.y + icon), ImVec2(0, 0), ImVec2(1, 1), tint);
  }

  // ── La pastille de statut, calée à droite de la ligne ─────────────────────
  // Même arbitrage que le natif : hors ligne -> OFF, moi -> ME (comparaison à
  // g_Account_Aid, pas à un champ de l'entrée), tout le reste -> ON.
  const bool is_me = (row.gid == rag::social::OwnAid());
  DrawStatusBadge(row.offline ? "OFF" : (is_me ? "ME" : "ON"),
                  row.offline ? IM_COL32(105, 105, 105, 255)
                  : is_me     ? IM_COL32(58, 110, 190, 255)
                              : IM_COL32(54, 138, 64, 255),
                  row.offline ? i18n::Tr("Hors ligne")
                  : is_me     ? i18n::Tr("Votre personnage")
                              : i18n::Tr("En ligne"));

  // Sépare les lignes comme le natif, qui peint une bande par entrée. La largeur
  // est celle capturée en HAUT de la ligne : ici, le curseur est en fin de ligne
  // et la place restante ne vaudrait plus rien.
  const float y = ImGui::GetCursorScreenPos().y;
  dl->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + row_w, y),
              IM_COL32(0, 0, 0, 40));

  // ── Clic droit n'importe où sur la ligne -> menu contextuel ───────────────
  // On teste le RECTANGLE de la ligne plutôt que de poser un bouton invisible :
  // un bouton couvrant volerait le survol de l'icône et des PV, qui portent
  // chacun leur infobulle. Le natif ouvre son menu sur le même geste
  // (`_OnRButtonDown` 0x007057a0 -> OnMsg 0x31).
  const bool row_hovered =
      ImGui::IsMouseHoveringRect(origin, ImVec2(origin.x + row_w, y));
  if (row_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    OnRowRightClick(row);
  }
  // ⚠ Pas d'infobulle tant qu'un popup est ouvert : elle passerait DEVANT le
  // menu contextuel qu'on vient d'ouvrir sur cette même ligne. On teste
  // N'IMPORTE QUEL popup et non le nôtre par son nom — les modales de
  // confirmation méritent la même paix.
  if (show_tooltip_ && row_hovered &&
      !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                       ImGuiPopupFlags_AnyPopupLevel)) {
    DrawRowTooltip(row);
  }
  DrawRowContextMenu(row, true);
  // (Aucun espace ajouté ici : il vient du `ItemSpacing` que DrawPartyTab pousse
  // autour de la boucle — sans quoi il s'AJOUTERAIT à celui d'ImGui.)
}

// ── Les réglages du groupe ──────────────────────────────────────────────────
//
// Trois choix binaires, exactement ceux de `UIPartySettingWnd`, avec SES libellés.
// Le serveur n'accepte le changement que du CHEF : on grise le reste pour ne pas
// promettre un effet qui n'arrivera pas.
// Relit les trois globales et ANNONCE tout changement. Appelée à chaque tick,
// fenêtre ouverte ou non.
//
// Le client natif fait déjà cette annonce dans son handler (`sub_CD3060` compose
// trois lignes et les passe à ChatAction) — mais elle n'arrive pas jusqu'à la
// chatbox moderne. Plutôt que de courir après ce chemin, on émet la nôtre : elle
// part de l'ÉTAT, donc elle est juste quelle que soit l'origine du changement
// (nous, le chef, ou un autre client).
void PartyFriendWindow::PollPartyOptions() {
  const int exp    = rag::ReadInt(kOptExpAddr);
  const int pickup = rag::ReadInt(kOptPickupAddr);
  const int share  = rag::ReadInt(kOptShareAddr);
  if (exp == seen_exp_ && pickup == seen_pickup_ && share == seen_share_) return;

  const bool was_known = opts_known_;
  seen_exp_ = exp; seen_pickup_ = pickup; seen_share_ = share;
  // Recaler la sélection affichée : les valeurs viennent du serveur, elles font
  // autorité sur ce que le joueur avait pu cocher sans valider.
  opt_exp_ = exp; opt_pickup_ = pickup; opt_share_ = share;
  opts_known_ = true;

  // Rien à annoncer au tout premier relevé, ni hors groupe — quitter un groupe
  // remet ces globales à zéro, et « Expérience : Individuelle » à ce moment-là
  // n'aurait aucun sens.
  if (!was_known || rag::social::PartyMemberCount() <= 0) return;
  QueuePartyOptionsMessage();
}

void PartyFriendWindow::QueuePartyOptionsMessage() {
  // Les libellés EXACTS du client, comme ses propres lignes.
  const char* exp_txt = (seen_exp_ == 1)
      ? msgstr::Utf8Or(kMsgExpShared, i18n::Tr("Partagée"))
      : msgstr::Utf8Or(kMsgExpEach, i18n::Tr("Individuelle"));
  const char* pick_txt = (seen_pickup_ == 1)
      ? msgstr::Utf8Or(kMsgPickShared, i18n::Tr("Groupe"))
      : msgstr::Utf8Or(kMsgPickEach, i18n::Tr("Individuel"));
  const char* share_txt = (seen_share_ == 1)
      ? msgstr::Utf8Or(kMsgDivShared, i18n::Tr("Partagé"))
      : msgstr::Utf8Or(kMsgDivEach, i18n::Tr("Individuel"));

  // ⚠ `msgstr::Utf8Or` rend un tampon ROTATIF (quatre emplacements) : on compose
  // TOUT DE SUITE dans une chaîne à nous, avant qu'un autre appel ne l'écrase.
  // ⚠ Pas de glyphe au-delà de U+00FF ici (ni tiret cadratin, ni puce) : la police
  // de l'interface ne les porte pas toujours, et un glyphe manquant s'affiche en
  // carré. « · » (U+00B7) passerait, mais autant rester en ASCII pur.
  char line[256];
  std::snprintf(line, sizeof(line), "%s - %s : %s | %s : %s | %s : %s",
                i18n::Tr("Groupe"),
                i18n::Tr("Expérience"), exp_txt,
                i18n::Tr("Ramassage"), pick_txt,
                i18n::Tr("Partage"), share_txt);
  chat_queue_.emplace_back(line);
}

void PartyFriendWindow::DrawPartyOptions() {
  if (!ImGui::CollapsingHeader(i18n::Tr("Réglages du groupe"))) return;

  const bool leader = i_am_leader_;
  if (!leader) {
    ImGui::TextDisabled("%s", i18n::Tr("Seul le chef de groupe peut les modifier."));
    ImGui::BeginDisabled();
  }

  // 🔴 UN PushID PAR GROUPE, obligatoire : les trois libellés « premier choix »
  // du client (MSI_EXPDIV1 / MSI_ITEMCOLLECT1 / MSI_ITEMDIV1) sont TOUS traduits
  // par « Individuel ». Or ImGui dérive l'identité d'un widget de son libellé :
  // sans ce préfixe, les trois radios partagent le même ID, et cliquer sur l'une
  // agit sur une autre (ImGui le signale par « conflicting ID »).
  ImGui::TextUnformatted(i18n::Tr("Expérience"));
  ImGui::PushID("exp");
  ImGui::RadioButton(msgstr::Utf8Or(kMsgExpEach, i18n::Tr("Individuelle")), &opt_exp_, 0);
  ImGui::SameLine();
  ImGui::RadioButton(msgstr::Utf8Or(kMsgExpShared, i18n::Tr("Partagée")), &opt_exp_, 1);
  ImGui::PopID();

  ImGui::TextUnformatted(i18n::Tr("Ramassage"));
  ImGui::PushID("pickup");
  ImGui::RadioButton(msgstr::Utf8Or(kMsgPickEach, i18n::Tr("Individuel")), &opt_pickup_, 0);
  ImGui::SameLine();
  ImGui::RadioButton(msgstr::Utf8Or(kMsgPickShared, i18n::Tr("Groupe")), &opt_pickup_, 1);
  ImGui::PopID();

  ImGui::TextUnformatted(i18n::Tr("Partage des objets"));
  ImGui::PushID("share");
  ImGui::RadioButton(msgstr::Utf8Or(kMsgDivEach, i18n::Tr("Individuel")), &opt_share_, 0);
  ImGui::SameLine();
  ImGui::RadioButton(msgstr::Utf8Or(kMsgDivShared, i18n::Tr("Partagé")), &opt_share_, 1);
  ImGui::PopID();

  // Le bouton n'apparaît QUE si quelque chose a changé — comme le natif, qui
  // compare ses trois sélections aux globales avant d'envoyer quoi que ce soit.
  const bool dirty = (opt_exp_ != seen_exp_) || (opt_pickup_ != seen_pickup_) ||
                     (opt_share_ != seen_share_);
  if (dirty) {
    ImGui::Spacing();
    if (ro::RoButton(i18n::Tr("Appliquer"))) pending_ = Action::kPartyOptions;
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler"))) {
      opt_exp_    = seen_exp_;
      opt_pickup_ = seen_pickup_;
      opt_share_  = seen_share_;
    }
  }

  if (!leader) ImGui::EndDisabled();
}

// ── Le menu contextuel d'une ligne ──────────────────────────────────────────
//
// Rien n'agit ici : chaque entrée ARME `pending_` (ou demande une confirmation).
// Cf. FlushPending pour le pourquoi.
namespace {

// ── La position d'un membre, pour la mini-carte de l'infobulle ──────────────
//
// Le client tient un `std::map<GID, {int x, int y, D3DCOLOR}>` en `CGameMode+0x1B4`
// — c'est LUI qui alimente les carrés de groupe de la minimap, et c'est la seule
// source : l'entrée sociale porte le nom de carte, jamais les coordonnées.
//
// On cherche par CLÉ plutôt que de tout collecter comme le fait la minimap : on
// ne veut qu'un membre, et la clé est en tête de la paire.
constexpr int kGm_PartyMap = 0x1b4;
constexpr int kPos_X = rag::treenode::kValue + 0x4;
constexpr int kPos_Y = rag::treenode::kValue + 0x8;

// 🔴 La taille de la carte en CELLULES, et non les pixels du bitmap.
//
// Une position de groupe est une CELLULE ; le bitmap de minimap est une image
// dont la résolution n'a aucun rapport (pvp_n_5-5 : 128 cellules pour un bitmap
// bien plus large). Diviser la cellule par la largeur en pixels mélange deux
// unités — le point tombait alors n'importe où, en bas à gauche d'une position
// qui se lisait pourtant juste en chiffres.
//
// Mêmes offsets que la minimap, qui fait exactement cette division.
constexpr int kGm_World       = 0x0cc;
constexpr int kWorld_MapInfo  = 0x30;
constexpr int kMapInfo_Width  = 0x110;  // int, largeur en CELLULES
constexpr int kMapInfo_Height = 0x114;  // int, hauteur en CELLULES

bool CurrentMapCells(int* out_w, int* out_h) {
  __try {
    void* gm = rag::ActiveModeSafe();
    if (!gm) return false;
    void* world = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(gm) + kGm_World);
    if (!world) return false;
    void* info = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(world) + kWorld_MapInfo);
    if (!info) return false;
    const int w = *reinterpret_cast<int*>(
        reinterpret_cast<uint8_t*>(info) + kMapInfo_Width);
    const int h = *reinterpret_cast<int*>(
        reinterpret_cast<uint8_t*>(info) + kMapInfo_Height);
    if (w <= 0 || h <= 0) return false;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool PartyMemberCell(uint32_t gid, int* out_x, int* out_y) {
  __try {
    void* gm = rag::ActiveModeSafe();
    if (!gm || gid == 0) return false;
    uint8_t* head = *reinterpret_cast<uint8_t**>(
        reinterpret_cast<uint8_t*>(gm) + kGm_PartyMap);
    if (!head) return false;
    uint8_t* stack[64];
    int sp = 0;
    uint8_t* root =
        *reinterpret_cast<uint8_t**>(head + rag::treenode::kParent);
    if (root && root != head) stack[sp++] = root;
    while (sp > 0) {
      uint8_t* node = stack[--sp];
      if (!node || *(node + rag::treenode::kIsNil) != 0) continue;
      if (*reinterpret_cast<uint32_t*>(node + rag::treenode::kValue) == gid) {
        if (out_x) *out_x = *reinterpret_cast<int*>(node + kPos_X);
        if (out_y) *out_y = *reinterpret_cast<int*>(node + kPos_Y);
        return true;
      }
      if (sp + 2 < 64) {
        stack[sp++] = *reinterpret_cast<uint8_t**>(node + rag::treenode::kLeft);
        stack[sp++] = *reinterpret_cast<uint8_t**>(node + rag::treenode::kRight);
      }
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

}  // namespace

// ── Une jauge de ligne (PV ou SP) ───────────────────────────────────────────
//
// Un seul endroit pour les deux : elles partagent tous les réglages (largeur,
// hauteur, texte dedans ou dehors, taille de police), et deux copies auraient
// divergé au premier ajustement.
//
// `stacked_next` dit qu'une autre jauge vient se COLLER dessous : on n'avance
// alors pas le curseur d'ImGui de la hauteur d'une ligne de texte, sans quoi les
// deux barres seraient séparées par un blanc.
void PartyFriendWindow::DrawRowBar(int cur, int max, ImU32 fill, int text_mode,
                                   bool stacked_next) {
  if (max <= 0) return;
  float frac = static_cast<float>(cur) / static_cast<float>(max);
  frac = std::max(0.0f, std::min(1.0f, frac));

  // Le pourcentage est arrondi VERS LE HAUT tant qu'il reste un point : afficher
  // « 0 % » sur quelqu'un de vivant ferait renoncer à le soigner.
  const int pct = (cur > 0) ? std::max(1, static_cast<int>(frac * 100.0f)) : 0;
  char txt[64] = {0};
  switch (text_mode) {
    case kHpTextNumbers: std::snprintf(txt, sizeof(txt), "%d/%d", cur, max); break;
    case kHpTextPercent: std::snprintf(txt, sizeof(txt), "%d %%", pct); break;
    case kHpTextBoth:
      std::snprintf(txt, sizeof(txt), "%d/%d (%d %%)", cur, max, pct);
      break;
    default: break;  // kHpTextNone : la jauge parle d'elle-même
  }

  const float bw = ro::Px(static_cast<float>(std::max(20, bar_w_)));
  const float bh = ro::Px(static_cast<float>(std::max(3, bar_h_)));
  const float fsz = (text_px_ > 0) ? ro::Px(static_cast<float>(text_px_))
                                   : ImGui::GetFontSize();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + bw, p0.y + bh);

  dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 24, 200));
  if (frac > 0.0f) dl->AddRectFilled(p0, ImVec2(p0.x + bw * frac, p1.y), fill);
  dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 180));

  // 🔴 Empilées, le texte passe DEDANS d'office. À côté, il pousse le curseur
  // d'une hauteur de LIGNE DE TEXTE et non de jauge : la barre suivante ne
  // tomberait pas collée mais séparée par un blanc, ce qui est exactement ce
  // qu'on cherchait à supprimer.
  const bool inside = text_in_bars_ || stacked_next;
  if (txt[0] && inside) {
    // Centré DANS la jauge, avec une ombre : le texte passe sur du vert clair
    // comme sur du fond sombre, il lui faut ce détachement pour rester lisible.
    ImFont* font = ImGui::GetFont();
    const ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, txt);
    const ImVec2 at(p0.x + (bw - ts.x) * 0.5f, p0.y + (bh - ts.y) * 0.5f);
    const ImVec4 clip(p0.x, p0.y, p1.x, p1.y);
    dl->AddText(font, fsz, ImVec2(at.x + 1.0f, at.y + 1.0f),
                IM_COL32(0, 0, 0, 180), txt, nullptr, 0.0f, &clip);
    dl->AddText(font, fsz, at, IM_COL32_WHITE, txt, nullptr, 0.0f, &clip);
  }

  ImGui::Dummy(ImVec2(bw, bh));
  if (txt[0] && !inside) {
    ImGui::SameLine(0.0f, ro::Px(6.0f));
    ImGui::Text("%s", txt);
  }
}

// ── L'infobulle d'une ligne ─────────────────────────────────────────────────
//
// Ce que la ligne ne peut pas porter : la classe, la carte ENTIÈRE (même en mode
// court), et surtout OÙ se trouve le membre — une mini-carte vaut mieux qu'un
// couple de coordonnées que personne ne sait situer.
void PartyFriendWindow::DrawRowTooltip(const rag::social::Entry& row) {
  ImGui::BeginTooltip();

  ImGui::TextUnformatted(ro::LocalToUtf8(row.name.c_str()));
  ImGui::SameLine();
  ImGui::TextDisabled("Lv.%u", row.level);
  ImGui::Separator();
  ImGui::TextDisabled("%s", ro::LocalToUtf8(rag::social::JobName(row.job)));

  if (row.offline) {
    ImGui::TextDisabled("%s", i18n::Tr("Hors ligne"));
    ImGui::EndTooltip();
    return;
  }

  // La carte, en NOM COMPLET ici : l'infobulle a la place que la ligne n'a pas.
  char bare[64] = {0};
  char pretty[64] = {0};
  if (!row.map.empty()) {
    StripMapExtension(row.map.c_str(), bare, sizeof(bare));
    if (!rag::MapDisplayName(bare, pretty, sizeof(pretty)) || !pretty[0])
      std::snprintf(pretty, sizeof(pretty), "%s", bare);
    ImGui::TextDisabled("%s", ro::LocalToUtf8(pretty));
  }

  if (row.has_hp && row.max_hp > 0) {
    ImGui::Text("%s %d/%d", i18n::Tr("PV"), row.hp, row.max_hp);
  } else {
    ImGui::TextDisabled("%s", i18n::Tr("PV inconnus : hors de portée"));
  }

  // ── La mini-carte ────────────────────────────────────────────────────────
  // Le même bitmap que la minimap du jeu, avec un point sur la position du
  // membre. Rien à calculer de savant : les cartes de RO se dessinent en
  // proportion, la cellule (x, y) tombant sur la fraction correspondante de
  // l'image — y INVERSÉ, l'origine du monde étant en bas.
  int cx = 0, cy = 0;
  if (bare[0] && PartyMemberCell(row.gid, &cx, &cy)) {
    char path[160];
    std::snprintf(path, sizeof(path), "%s\\map\\%s.bmp", ro::uipath::kUiRoot,
                  bare);
    const ro::GameTexture map_tex = ro::CachedTextureFromGameFile(path);
    if (map_tex.tex && map_tex.w > 0 && map_tex.h > 0) {
      const float side = ro::Px(128.0f);
      const float w = side;
      const float h = side * (static_cast<float>(map_tex.h) /
                              static_cast<float>(map_tex.w));
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      ImGui::Image(reinterpret_cast<ImTextureID>(map_tex.tex), ImVec2(w, h));
      // Le bitmap couvre la carte entière : la cellule se ramène en fraction des
      // dimensions en CELLULES — jamais en pixels du bitmap, qui n'ont aucun
      // rapport avec elles. On borne : une position aberrante ne doit pas
      // dessiner hors de l'image.
      int cells_w = 0, cells_h = 0;
      if (CurrentMapCells(&cells_w, &cells_h)) {
        const float fx = std::min(1.0f, std::max(0.0f,
            static_cast<float>(cx) / static_cast<float>(cells_w)));
        // Y INVERSÉ : l'origine du monde RO est en bas, celle de l'image en haut.
        const float fy = std::min(1.0f, std::max(0.0f,
            static_cast<float>(cells_h - cy) / static_cast<float>(cells_h)));
        const ImVec2 at(p0.x + w * fx, p0.y + h * fy);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(at, ro::Px(3.0f), IM_COL32(255, 255, 255, 230));
        dl->AddCircleFilled(at, ro::Px(2.0f), IM_COL32(230, 90, 60, 255));
      }
    }
    ImGui::TextDisabled("%d, %d", cx, cy);
  }

  ImGui::EndTooltip();
}

// Le clic droit sur une ligne. Quand un sprite représente ce personnage, il
// n'ouvre AUCUN menu à nous : il arme directement celui du client.
//
// 🔴 Le nôtre n'aurait porté qu'une entrée — « Menu du personnage » — c'est-à-dire
// un menu dont l'unique rôle était d'en ouvrir un autre. Les gestes qui le
// justifiaient encore (nommer chef, expulser, retirer un ami) ont rejoint le
// menu d'entité, où ils sont à leur place : ce sont des actions sur un JOUEUR.
void PartyFriendWindow::OnRowRightClick(const rag::social::Entry& row) {
  // MOI excepté : mon acteur n'est pas dans la liste que parcourt
  // `FindActorByGid` (le natif le range en `actorMgr+0x2C`), et le menu
  // d'entité n'a de toute façon rien à proposer sur soi-même.
  const bool is_me = (row.gid == rag::social::OwnAid());
  if (!is_me && !row.offline && gamescene::FindActorByGid(row.gid) != nullptr) {
    pending_      = Action::kEntityMenu;
    pending_gid_  = row.gid;
    pending_name_ = row.name;
    return;
  }
  // Pas de sprite à cliquer — sur une autre carte, typiquement. C'est justement
  // là que chuchoter ou inviter rend service : ces demandes voyagent PAR NOM et
  // n'ont besoin d'aucune entité. On les propose alors nous-mêmes.
  ImGui::OpenPopup("##rowmenu");
}

void PartyFriendWindow::DrawRowContextMenu(const rag::social::Entry& row, bool party) {
  if (!ImGui::BeginPopup("##rowmenu")) return;

  const bool is_me = (row.gid == rag::social::OwnAid());

  ImGui::TextDisabled("%s", ro::LocalToUtf8(row.name.c_str()));
  ImGui::Separator();

  // ── Ce que le MENU DU PERSONNAGE fait déjà ───────────────────────────────
  //
  // 🔴 On ne REPRODUIT pas le menu du client : il porte déjà chuchoter, échange,
  // guilde, équipement, copier le nom — et bien plus que ce qu'on écrirait ici.
  // Quand l'acteur est là, on s'efface donc devant lui et l'on ne garde que ce
  // qu'il n'a PAS : cibler, et les actions de groupe/amis plus bas.
  //
  // Mais il n'existe QUE si l'acteur est chargé. Hors de portée — sur une autre
  // carte, typiquement — il n'y a pas de sprite à cliquer, et c'est justement là
  // que chuchoter ou inviter rend service : ces demandes voyagent PAR NOM et
  // n'ont besoin d'aucune entité. On les propose alors nous-mêmes.
  //
  // ⚠ MOI excepté pour le ciblage : mon acteur n'est pas dans la liste que
  // parcourt `FindActorByGid` (le natif le range en `actorMgr+0x2C`).
  // ⚠ Ce menu ne s'ouvre PLUS quand l'acteur est chargé : `OnRowRightClick`
  // arme alors directement celui du client. On n'arrive donc ici que pour MOI,
  // ou pour un membre qu'aucun sprite ne représente.
  if (is_me) {
    // Sur MOI, le menu d'entité n'a pas de sens : on garde le seul geste utile.
    if (!row.offline && ImGui::Selectable(i18n::Tr("Cibler"))) {
      pending_     = Action::kTargetMember;
      pending_gid_ = row.gid;
    }
  } else {
    // Pas d'acteur : le menu du client est hors d'atteinte, on comble.
    if (!is_me && !row.offline && ImGui::Selectable(i18n::Tr("Chuchoter"))) {
      pending_      = Action::kWhisper;
      pending_gid_  = row.gid;
      pending_name_ = row.name;
    }
    // Le pont entre les deux onglets : un membre de groupe n'est pas forcément
    // un ami, un ami n'est pas dans le groupe. Mêmes gardes que partout — on ne
    // propose pas une demande que le serveur refusera.
    if (party && !is_me && !rag::social::IsFriendByName(row.name.c_str()) &&
        ImGui::Selectable(i18n::Tr("Ajouter à mes amis"))) {
      pending_      = Action::kAddFriend;
      pending_name_ = row.name;
    }
    if (!party && !row.offline &&
        !rag::social::IsPartyMemberByName(row.name.c_str()) &&
        rag::social::PartyMemberCount() > 0 &&
        ImGui::Selectable(i18n::Tr("Inviter dans le groupe"))) {
      pending_      = Action::kInviteParty;
      pending_name_ = row.name;
    }
    if (ImGui::Selectable(i18n::Tr("Copier le nom"))) {
      ImGui::SetClipboardText(ro::LocalToUtf8(row.name.c_str()));
    }
  }
  ImGui::Separator();

  if (party) {
    // Réservé au chef, et jamais sur soi-même — mêmes gardes que le natif.
    if (i_am_leader_ && !is_me) {
      if (ImGui::Selectable(i18n::Tr("Nommer chef de groupe"))) {
        confirm_      = Action::kMakeLeader;
        confirm_gid_  = row.gid;
        confirm_name_ = row.name;
        open_confirm_ = true;
      }
      if (ImGui::Selectable(i18n::Tr("Expulser du groupe"))) {
        confirm_      = Action::kKick;
        confirm_gid_  = row.gid;
        confirm_name_ = row.name;
        open_confirm_ = true;
      }
    }
    if (is_me && ImGui::Selectable(i18n::Tr("Quitter le groupe"))) {
      confirm_      = Action::kLeaveParty;
      confirm_gid_  = row.gid;
      confirm_name_ = row.name;
      open_confirm_ = true;
    }
  } else {
    if (ImGui::Selectable(i18n::Tr("Retirer de mes amis"))) {
      confirm_      = Action::kRemoveFriend;
      confirm_gid_  = row.gid;
      confirm_id2_  = row.id2;
      confirm_name_ = row.name;
      open_confirm_ = true;
    }
  }

  ImGui::EndPopup();
}

// ── La confirmation des actions sans retour en arrière ──────────────────────
//
// Une popup ImGui, pas la modale NATIVE que le hub utilise (msgstring 0x164 /
// 0x165) : celle-ci bloquerait la boucle du client depuis notre frame.
void PartyFriendWindow::DrawConfirmPopup() {
  // ⚠ MÊME identifiant des deux côtés : `BeginRoPopupModal` prend le titre pour
  // ID, donc `OpenPopup` doit recevoir exactement la même chaîne.
  // Et cet appel a lieu ICI, hors du PushID(ligne) du menu contextuel : ouvert
  // depuis la ligne, l'identifiant aurait été celui de la ligne, et la popup ne
  // se serait jamais ouverte. D'où le drapeau `open_confirm_`.
  if (open_confirm_) {
    ImGui::OpenPopup(i18n::Tr("Confirmation"));
    open_confirm_ = false;
  }
  if (!ro::BeginRoPopupModal(i18n::Tr("Confirmation"))) return;

  const char* question = i18n::Tr("Confirmer ?");
  switch (confirm_) {
    case Action::kMakeLeader:
      question = i18n::Tr("Céder le commandement du groupe à ce joueur ?");
      break;
    case Action::kKick:
      question = i18n::Tr("Expulser ce joueur du groupe ?");
      break;
    case Action::kLeaveParty:
      question = i18n::Tr("Quitter le groupe ?");
      break;
    case Action::kRemoveFriend:
      question = i18n::Tr("Retirer ce joueur de votre liste d'amis ?");
      break;
    default:
      break;
  }
  ImGui::TextUnformatted(question);
  if (!confirm_name_.empty() && confirm_ != Action::kLeaveParty) {
    ImGui::Spacing();
    ImGui::TextUnformatted(ro::LocalToUtf8(confirm_name_.c_str()));
  }
  if (confirm_ == Action::kLeaveParty) {
    ImGui::Spacing();
    // Ce n'est pas une supposition : c'est ce que fait le case 0x3D avant de
    // partir (cf. docs/party_friend_re.md §6).
    ImGui::TextDisabled("%s",
        i18n::Tr("Si vous êtes chef, le commandement passera à un membre proche."));
  }

  ImGui::Spacing();
  if (ro::RoButton(i18n::Tr("Confirmer"))) {
    pending_      = confirm_;
    pending_gid_  = confirm_gid_;
    pending_id2_  = confirm_id2_;
    pending_name_ = confirm_name_;
    confirm_      = Action::kNone;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Annuler"))) {
    confirm_ = Action::kNone;
    ImGui::CloseCurrentPopup();
  }
  ro::EndRoPopupModal();
}

// ── La demande REÇUE : « rejoindre ce groupe ? » / « accepter cet ami ? » ────
//
// Remplace les fenêtres natives 35 et 109, qui ne naissent plus (leurs paquets
// sont revendiqués, cf. le constructeur). On reprend leurs libellés EXACTS via
// msgstr, pour que le joueur lise la même phrase qu'avant.
void PartyFriendWindow::DrawInvitePopup() {
  if (open_invite_popup_) {
    ImGui::OpenPopup(i18n::Tr("Demande reçue"));
    open_invite_popup_ = false;
  }
  if (!ro::BeginRoPopupModal(i18n::Tr("Demande reçue"))) return;

  if (!invite_.active) {
    // Plus rien à décider (répondu ailleurs, ou changement de personnage).
    ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
    return;
  }

  const char* who = ro::LocalToUtf8(invite_.name.c_str());
  if (invite_.is_friend) {
    // Le natif compose msgstring 0x332 avec le nom ; on garde les deux, le nom
    // en évidence au-dessus de la phrase du client.
    ImGui::TextUnformatted(who);
    ImGui::Spacing();
    ImGui::TextUnformatted(
        msgstr::Utf8Or(kMsgFriendRequestText,
                       i18n::Tr("souhaite vous ajouter à sa liste d'amis.")));
  } else {
    ImGui::TextUnformatted(who);
    ImGui::Spacing();
    ImGui::TextUnformatted(
        msgstr::Utf8Or(kMsgPartyInviteText,
                       i18n::Tr("vous invite à rejoindre ce groupe.")));
  }

  ImGui::Spacing();
  if (ro::RoButton(i18n::Tr("Accepter"))) {
    pending_        = invite_.is_friend ? Action::kAnswerFriend
                                        : Action::kAnswerParty;
    pending_gid_    = invite_.is_friend ? invite_.aid : invite_.party_id;
    pending_id2_    = invite_.cid;
    pending_accept_ = true;
    invite_.active  = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Refuser"))) {
    pending_        = invite_.is_friend ? Action::kAnswerFriend
                                        : Action::kAnswerParty;
    pending_gid_    = invite_.is_friend ? invite_.aid : invite_.party_id;
    pending_id2_    = invite_.cid;
    pending_accept_ = false;
    invite_.active  = false;
    ImGui::CloseCurrentPopup();
  }
  ro::EndRoPopupModal();
}

void PartyFriendWindow::DrawFriendRow(const rag::social::Entry& row) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float row_w = ImGui::GetContentRegionAvail().x;

  // Les amis n'ont ni carte ni PV côté client, mais le natif signale bien la
  // CONNEXION : sa branche amis ne blitte son icône que si `+0x40` est nul. On
  // rend la même information avec la pastille du groupe, pour que l'état « en
  // ligne » se lise pareil dans les deux onglets.
  if (row.offline) ImGui::TextDisabled("%s", ro::LocalToUtf8(row.name.c_str()));
  else             ImGui::TextUnformatted(ro::LocalToUtf8(row.name.c_str()));

  DrawStatusBadge(row.offline ? "OFF" : "ON",
                  row.offline ? IM_COL32(105, 105, 105, 255)
                              : IM_COL32(54, 138, 64, 255),
                  row.offline ? i18n::Tr("Hors ligne") : i18n::Tr("En ligne"));

  const float y = ImGui::GetCursorScreenPos().y;
  if (ImGui::IsMouseHoveringRect(origin, ImVec2(origin.x + row_w, y)) &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    OnRowRightClick(row);
  }
  DrawRowContextMenu(row, false);
}
