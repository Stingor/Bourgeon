#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "features/plugin.h"

// ── PartyFriendWindow ────────────────────────────────────────────────────────
//
// Remplacement ImGui (skin RO) de la fenêtre Amis / Groupe — `UIMessengerGroupWnd`,
// window id 0x45. UNE classe native à deux onglets rend les deux listes ; on la
// remplace par une fenêtre ImGui à deux onglets, même découpage.
//
// RE complète : docs/party_friend_re.md (mémoire project_party_friend_window_re).
//
// 🔴 IL N'Y A QUE DEUX MODES. `this+0x28C` vaut 0 (amis) ou 1 (groupe), jamais plus :
// la bascule d'onglet (case 0xd7 du hub) boucle sur `this+0xBC`..`this+0xC4`, soit
// exactement deux entrées, et y range l'indice de boucle. Une note de RE plus
// ancienne mentionnait un troisième mode « liste de sorts » — c'était un contresens
// né du faux nom `g_SkillInfoMgr` donné au manager. Ne pas le réintroduire.
//
// ── POURQUOI ON LIT LE NATIF, ET PAS LES PAQUETS ─────────────────────────────
// Le cas inverse de `trade_window`. Là-bas on REMPLAÇAIT les handlers d'opcodes,
// donc les tableaux globaux qu'ils remplissaient s'asséchaient et il fallait
// reconstruire l'état depuis le fil. Ici on ne touche à aucun handler : les deux
// listes vivent dans le manager de session (`rag::kSessionAddr`), alimentées par
// les handlers natifs, et `UIMessengerGroupWnd::DrawContent` les relit LUI-MÊME à
// chaque frame — sa liste interne `+0xFC` n'est qu'un miroir reconstruit au msg
// 0x17. Détruire la fenêtre n'assèche donc rien, et notre fenêtre lit exactement la
// même source, au même endroit, avec les mêmes accesseurs.
//
// Reconstruire tout cela depuis les paquets aurait voulu dire réimplémenter la
// machine à états complète — join, leave, expel, changement de chef, position,
// connexion/déconnexion d'ami — pour aboutir au même contenu que ce que le client
// tient déjà à jour à côté.
//
// ── CE QUE LE NATIF NE SAIT PAS FAIRE (à combler, pas à reproduire) ──────────
// 🔴 Le HP d'un membre vient du `CPc` de l'ACTEUR, jamais d'un cache réseau :
// `UpdateMemberHpGauges` (0x00705b40) fait `dynamic_cast<CPc*>(ActorList_FindByGID)`
// et masque la jauge quand l'acteur manque. Hors de portée ⇒ AUCUN HP, y compris
// dans le client officiel. C'est une limite du CLIENT, pas de la fenêtre.
// La combler demande d'écouter ZC_NOTIFY_HP_TO_GROUPM (0x0106) via net_inbox.h et
// de tenir notre propre cache — hors périmètre du premier temps, prévu ensuite.
//
// ── DÉCOUPAGE DU CHANTIER ────────────────────────────────────────────────────
//   Temps 1 (ici)  : lecture seule — les deux listes, aucun paquet émis.
//   Temps 2        : les actions (menu contextuel), cf. « ACTIONS » plus bas.
//   Lot 2 (séparé) : les dialogues (invitation, options d'amis, réglages groupe).
//   Chantier à part: le HUD mini-party (UIMiniPartyWnd 0x12d, UIDragMiniPartyWnd).
//
// ── ACTIONS (temps 2) — les 3 commandes du hub, vérifiées contre Hercules ────
//   CMode::SendMsg 0x104 -> CZ 0x07DA (AID)       passer chef
//   CMode::SendMsg 0x0B0 -> CZ 0x0203 (AID, CID)  retirer un ami
//   CMode::SendMsg 0x03D -> CZ 0x0100             quitter le groupe
// 🔴 Le 0x3D n'est PAS un simple envoi (0x00c8d44e) : il cherche d'abord un autre
// membre EN LIGNE sur MA carte et lui TRANSFÈRE le leadership (`SendMsg(0x104, …)`),
// sinon il passe par une modale (msgstring 0xCB9, on ne continue que si la réponse
// vaut 0xBB), puis envoie 0x0100. Sans l'étape de transfert, un chef qui quitte
// laisse le groupe sans chef là où le client officiel passe la main.
// Les invitations, elles, sont déléguées : le hub natif ouvre un dialogue
// (`MakeWindow`) et c'est LE DIALOGUE qui émet le CZ. Elles restent donc au natif
// jusqu'au lot 2.

class PartyFriendWindow : public Plugin {
 public:
  PartyFriendWindow();

  const char* name() const override { return "PartyFriendWindow"; }

  void OnTick() override;      // détruit la native, tient l'état d'ouverture
  void OnRenderUI() override;  // dessine la fenêtre ImGui

  // ── Les deux demandes REÇUES ──────────────────────────────────────────────
  //
  // 🔴 On REMPLACE les handlers de ces deux paquets (RegisterReplaceOpcode), on ne
  // les observe pas : leurs fenêtres natives ne doivent pas NAÎTRE. Une native
  // masquée reste vivante et garde le clavier — et sur ces deux-là, le bouton par
  // défaut est « Accepter ». Une frappe d'Entrée destinée au chat suffirait à
  // rejoindre un groupe ou à accepter un inconnu.
  //
  //   ZC_PARTY_JOIN_REQ  0x02C6 { partyid:4, groupName[24] }        (30 o)
  //   ZC_REQ_ADD_FRIENDS 0x0207 { AID:4, CID:4, name[24] }          (34 o)
  //
  // ⚠ 0x02C6 et non 0x00FE : le serveur bascule sur 0x02C6 dès PACKETVER
  // >= 20070821 (moonlight `packets_struct.hpp`), et le nôtre est 20250716.
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // 🔴 Exécute l'action choisie dans le menu contextuel, HORS de la frame ImGui.
  // Appelée par Bourgeon depuis OnProcessInput, comme ChatWindow et VendingWindow.
  //
  // Ce détour n'est pas une précaution de principe : `CMode::SendMsg` 0x3D et 0xB0
  // ouvrent des MODALES NATIVES (`UIWndMgr_ShowMessageBoxModal`), et une modale
  // déclenchée entre NewFrame() et Render() ne rend pas la main — c'est le même
  // piège qui a déjà mordu l'échoppe joueur et la chatbox. Le menu se contente
  // donc d'ARMER l'action ; c'est ici qu'elle part.
  void FlushPending();

  // Ouvre / referme depuis notre propre interface.
  void ToggleFromUi();
  bool IsOpen() const { return open_; }

  // 🔴 LE point d'ouverture, appelé par le hook de MakeWindow (window_pos_tweaks)
  // au **`case 0x22`** — l'id de FABRIQUE, pas 0x45. Voir le commentaire de
  // window_pos_tweaks : 0x45 n'est qu'un point d'entrée qui rappelle
  // MakeWindow(0x22) et sort par le `default` sans rendre de fenêtre.
  // C'est le seul point nécessaire : le constructeur (0x00701fc0) n'a qu'UN
  // appelant dans tout le binaire, donc aucune ouverture ne peut l'éviter.
  // On masque la native à la naissance (elle serait visible une frame) et on
  // bascule notre fenêtre à sa place.
  //
  // 🔴 `user_gesture` DÉCIDE SI L'ON BASCULE, et ce n'est pas un détail : le client
  // fabrique aussi cette fenêtre TOUT SEUL — à la création d'un groupe, et quand on
  // en rejoint un. Basculer sur ces appels-là fermait la fenêtre sous le joueur,
  // qui devait ensuite appuyer DEUX fois pour la revoir (le premier appui ne
  // faisait que remettre l'état d'accord avec l'écran).
  //   · `MakeWindow(0x45)` = le point d'entrée du JOUEUR (bouton, raccourci). Il
  //     rappelle 0x22 en interne, donc l'appel 0x22 y est IMBRIQUÉ -> bascule.
  //   · `MakeWindow(0x22)` seul = le client veut la fenêtre pour la peupler
  //     -> on l'OUVRE si elle est fermée, et on n'y touche pas si elle l'est déjà.
  void HandleNativeCreation(void* win, bool user_gesture);

  // ── Settings PERSISTANTS (bourgeon_settings.yaml, via MoonlightUi) ─────────
  // « partyfriend_imgui » : basculé en GROUPE par SetModernInterface, jamais
  // isolément. Défaut OFF, comme tout le groupe.
  bool imgui_enabled_ = false;

  // Onglet courant. Mêmes valeurs que le champ natif `+0x28C`, pour que le sens se
  // lise pareil des deux côtés : 0 = amis, 1 = groupe.
  int& cur_tab() { return cur_tab_; }

 private:
  // ── Une entrée sociale, telle que le manager la stocke ────────────────────
  //
  // Le natif emploie LE MÊME type des deux côtés (amis et groupe) : structure de
  // 0x50 octets, copy-ctor 0x00701df0, rangée dans une liste chaînée circulaire
  // dont la donnée est à `nœud+8`. On ne recopie ici que ce qu'on affiche.
  //
  // Le NIVEAU (+0x4A) est prouvé depuis le désassemblage : le site du « Lv.%d »
  // (0x0070433d) pousse `movzx eax, word ptr [esi+52h]`, soit nœud+0x52 = data+0x4A.
  // Le décompilateur ne le montrait pas — l'argument passe par un sprintf
  // variadique — d'où le passage par le désassemblage.
  // ⚠ Reste non identifié : +0x4C (u32).
  struct SocialRow {
    uint32_t    gid    = 0;      // +0x04 — GID/AID, la clé des acteurs
    uint32_t    id2    = 0;      // +0x08 — second id (char id), clé du getter 0xd5d740
    std::string name;            // +0x0C
    std::string map;             // +0x24 — nom brut, à passer par MapDisplayName
    bool        is_leader = false;  // +0x3C — 🔴 le natif code 0 = CHEF
    bool        offline   = false;  // +0x40
    // +0x44 — couleur du nom. L'OFFSET est sûr (le natif la passe directement à
    // DrawText), mais son ENCODAGE ne l'est pas : le moteur du client compose ses
    // couleurs en BGR à plusieurs endroits. Lue et transportée, PAS encore
    // affichée — à trancher avant de s'en servir, sous peine de noms illisibles.
    uint32_t    color  = 0;
    uint16_t    job    = 0;      // +0x48 — alimente \renewalparty\icon_jobs_<job>.bmp
    uint16_t    level  = 0;      // +0x4A — le « Lv.%d » du natif
    // HP : renseigné SEULEMENT si l'acteur est chargé (cf. bandeau). `has_hp` faux
    // veut dire « hors de portée », pas « mort ».
    bool        has_hp = false;
    int         hp     = 0;
    int         max_hp = 0;
  };

  // Relit une liste entière depuis le manager. `party` choisit la liste (groupe ou
  // amis) ; c'est le seul endroit qui connaît les offsets.
  // (Pas `const` : pour la liste de groupe, elle met aussi `i_am_leader_` à jour.)
  void ReadList(bool party, std::vector<SocialRow>& out);
  // Complète `hp`/`max_hp` en interrogeant l'acteur, comme UpdateMemberHpGauges.
  void FillHp(SocialRow& row) const;
  // Détruit la fenêtre native si elle traîne (cf. « DÉTRUIRE, pas masquer »).
  void KillNative(bool adopt_open_state);

  void DrawPartyTab();
  void DrawFriendTab();
  // Une ligne de GROUPE, calquée sur le natif : icône de classe à gauche, puis
  // « Lv.N Nom(Carte) » et la barre de vie avec ses PV, et le pastille de statut
  // (moi / en ligne / hors ligne) à droite.
  void DrawPartyRow(const SocialRow& row);
  void DrawFriendRow(const SocialRow& row);
  // Le menu contextuel d'une ligne (clic droit), et la demande de confirmation
  // des actions irréversibles. Ni l'un ni l'autre n'agit : ils ARMENT `pending_`.
  void DrawRowContextMenu(const SocialRow& row, bool party);
  void DrawConfirmPopup();
  // La demande reçue (groupe ou amitié), en remplacement des fenêtres natives
  // 35 (UIJoinPartyAcceptWnd) et 109 (UIJoinFriendAcceptWnd).
  void DrawInvitePopup();

  // Une demande en attente de réponse. Le natif n'en tient qu'une à la fois (une
  // fenêtre), on fait pareil : une nouvelle demande remplace la précédente.
  struct InviteRequest {
    bool        active    = false;
    bool        is_friend = false;  // false = invitation de groupe
    uint32_t    party_id  = 0;      // groupe
    uint32_t    aid       = 0;      // amitié
    uint32_t    cid       = 0;      // amitié
    std::string name;               // nom du GROUPE, ou du joueur demandeur
  };
  InviteRequest invite_;
  bool          open_invite_popup_ = false;

  // ── Ce que le menu contextuel sait faire ──────────────────────────────────
  //
  // Les invitations n'y sont PAS : chez le natif, elles ouvrent un dialogue
  // (`MakeWindow`) qui émet lui-même le CZ. Elles restent donc au natif jusqu'au
  // lot 2, où ces dialogues seront portés à leur tour.
  enum class Action {
    kNone,
    kWhisper,       // UIM_MAKE_WHISPER_WINDOW (0x0E) — le chemin du natif, que
                    // ChatWindow intercepte quand la chatbox moderne est active
    kMakeLeader,    // CMode::SendMsg 0x104 -> CZ_PARTY_CHANGE_LEADER 0x07DA
    kKick,          // CZ_REQ_EXPEL_GROUP_MEMBER 0x0103, construit ici
    kLeaveParty,    // CMode::SendMsg 0x3D -> CZ_REQ_LEAVE_GROUP 0x0100
    kRemoveFriend,  // CMode::SendMsg 0x0B0 -> CZ_DELETE_FRIENDS 0x0203
    kCreateParty,   // CZ_MAKE_GROUP2 0x01E8, construit ici
    kInviteParty,   // CMode::SendMsg 0x3B -> CZ_PARTY_JOIN_REQ 0x02C4 (par NOM)
    kAddFriend,     // FriendList_AddByName -> CZ_ADD_FRIENDS 0x0202 (par NOM)
    kAnswerParty,   // CMode::SendMsg 0x3C -> CZ 0x02C7 (partyid, oui/non)
    kAnswerFriend,  // CMode::SendMsg 0xAF -> CZ 0x0208 (AID, CID, oui/non)
    kPartyOptions,  // CMode::SendMsg 0x103 -> CZ 0x07D7 (exp, ramassage, partage)
  };

  // Les trois réglages du groupe, tels que le joueur les a réglés dans la section
  // « Réglages » — distincts des globales du client, qui portent l'état COURANT.
  //
  // 🔴 La détection de changement vit dans OnTick, PAS dans le rendu : un membre
  // doit être prévenu quand le chef change les règles, même si sa fenêtre
  // Amis/Groupe est fermée — ce qui est le cas le plus fréquent.
  void PollPartyOptions();
  void DrawPartyOptions();
  // Compose la ligne de chat qui annonce l'état courant des trois réglages.
  void QueuePartyOptionsMessage();
  int  opt_exp_    = 0;
  int  opt_pickup_ = 0;
  int  opt_share_  = 0;
  // Dernières valeurs LUES dans le client : elles servent à détecter un changement
  // venu du serveur (le chef modifie, tout le monde doit suivre) sans écraser une
  // sélection en cours d'édition.
  int  seen_exp_    = -1;
  int  seen_pickup_ = -1;
  int  seen_share_  = -1;
  // Faux tant qu'on n'a jamais lu ces globales : le tout premier relevé ne doit
  // rien annoncer (ce n'est pas un changement, c'est une prise de connaissance).
  bool opts_known_  = false;

  // Lignes de chat en attente. Comme les actions, elles partent HORS de la frame
  // ImGui (FlushPending) — écrire dans le chat rejoue du code natif.
  std::vector<std::string> chat_queue_;

  // Réponse portée par kAnswerParty / kAnswerFriend.
  bool pending_accept_ = false;

  // Noms saisis. ⚠ Tous dans la CODE-PAGE DU CLIENT : ils partent tels quels sur
  // le fil, dans des champs de 24 octets.
  // 🔴 Taille ≥ 24 OBLIGATOIRE pour `friend_name_` : `FriendList_AddByName`
  // (0x00a2c600) lit 24 octets D'AFFILÉE, quelle que soit la longueur du nom.
  char new_party_name_[32] = {0};
  char invite_name_[32]    = {0};
  char friend_name_[32]    = {0};

  // L'action armée par le menu, consommée par FlushPending.
  Action      pending_ = Action::kNone;
  uint32_t    pending_gid_ = 0;
  uint32_t    pending_id2_ = 0;
  // ⚠ Le nom est gardé dans la CODE-PAGE DU CLIENT, telle qu'elle sort du
  // manager : il repart vers le client (whisper) ou sur le fil (expulsion), et
  // le convertir en UTF-8 le rendrait méconnaissable des deux côtés. La
  // conversion n'a lieu qu'à l'AFFICHAGE (ro::LocalToUtf8).
  std::string pending_name_;

  // Confirmation en attente, pour les trois actions sans retour en arrière.
  Action      confirm_        = Action::kNone;
  uint32_t    confirm_gid_    = 0;
  uint32_t    confirm_id2_    = 0;
  std::string confirm_name_;
  bool        open_confirm_   = false;  // demande d'ouverture de la popup

  // Listes relues à chaque frame de rendu (comme le fait le natif). Membres plutôt
  // que locales pour ne pas réallouer 40 std::string par frame.
  std::vector<SocialRow> party_;
  std::vector<SocialRow> friends_;

  bool open_       = false;
  bool need_pos_   = false;  // (re)placer la fenêtre à la première ouverture
  int  cur_tab_    = 1;      // le natif démarre sur GROUPE (OnCreate écrit 1)
  // Suis-je le chef ? Recalculé à chaque relecture de la liste : c'est lui qui
  // décide des entrées de menu réservées au chef (passer chef, expulser).
  bool i_am_leader_ = false;

  // Bascule de mode : mémorisé pour détecter le passage ImGui <-> natif, comme
  // cart_viewer, afin d'adopter l'état de la native avant de la détruire.
  bool prev_imgui_enabled_ = false;
};
