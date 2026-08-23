#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "features/plugin.h"

// ── ChatRoomWindow ───────────────────────────────────────────────────────────
//
// « Create Chat Room » (Alt+C, /chat, bouton de Basic Info) en ImGui, skin RO, en
// REMPLACEMENT de la fenêtre native `UIChatRoomMakeWnd` (id 27 / 0x1B) : celle-ci
// est masquée dès sa création puis DÉTRUITE au tick suivant.
//
// RE complète : docs/chat_room_re.md (mémoire project_chat_room_window_re).
//
// 🔴 POURQUOI DÉTRUIRE ET NON MASQUER. `OnCreate` termine par `this[35] = 184`,
// c'est-à-dire « bouton par défaut = OK ». `UIWindow_OnMsg_Default` (0x008841D0)
// traduit la touche Entrée en `OnMsg(6, this[35])` : une native invisible mais
// vivante CRÉERAIT donc un salon sur une frappe d'Entrée destinée à notre champ de
// saisie. Exactement le piège payé sur la banque (docs/bank_zeny_re.md) et sur le
// refine. Vérifié avant de trancher : ni le ctor (0x0088D160), ni `OnCreate`
// (0x008A1C30), ni le destructeur n'émettent de paquet — la destruction précoce est
// donc sans effet de bord, contrairement à la fenêtre d'écriture RODEX.
//
// 🔴 CE QUE NOUS CORRIGEONS, ET QUI EST LA RAISON D'ÊTRE DE CE MODULE. Le natif
// envoie `CZ_CREATE_CHATROOM 0x00D5` puis ferme sa fenêtre DANS LA FOULÉE, sans
// attendre la réponse. Quand le serveur refuse — `ZC_ACK_CREATE_CHATROOM 0x00D6`
// code 1 « trop de salons », code 2 « un salon du même nom existe déjà », le refus
// le plus courant — il ne reste au joueur qu'une ligne de chat, et son titre, son
// mot de passe et ses réglages sont perdus. Ici la fenêtre RESTE OUVERTE jusqu'à
// l'acquittement : sur refus elle affiche l'erreur à l'intérieur et surligne le
// champ fautif, la saisie est intacte, un caractère suffit à corriger.
//
// POINT D'INTERCEPTION UNIQUE. Les trois chemins d'ouverture — `/chat`, le bouton
// 214 de `UIBasicInfoWnd`, et l'action `TalkType 0x17` (`ChatRoom_OpenMakeWnd`
// 0x00692D70, atteinte par le raccourci) — convergent tous sur `MakeWindow(0x1B)`.
// Le hook de `MakeWindow` (features/patches/window_pos_tweaks.cc) suffit donc, et
// les gardes de ces appelants (échoppe en cours, déjà dans un salon) ont DÉJÀ joué
// quand il nous appelle : il n'y a rien à rejouer de ce côté.
//
// La seule garde à rejouer est celle qui vit DANS `OnCreate` — la case occupée par
// un panneau de salon ou d'échoppe — parce que `OnCreate` a bien tourné mais que sa
// fermeture est mise en FILE (`QueueDestroyWindow`) : `MakeWindow` nous rend quand
// même la fenêtre, et sans ce test on ouvrirait notre formulaire alors que le
// client vient de refuser. Le message de chat, lui, a déjà été affiché par le natif.
//
// ENVOI — par le chemin NATIF `CMode::SendMsg(43)`, et surtout pas par un paquet
// brut. 🔴 La première version sérialisait `CZ_CREATE_CHATROOM 0x00D5` à la main :
// le salon se créait, mais sa fenêtre s'intitulait « Publ. : (1/-657931) ». Le
// `case 43` ne fait pas que sérialiser — il recopie la demande dans un bloc
// « salon courant » du CGameMode (+0x3C8 titre, +0x3E0 mot de passe, +0x3F8
// public, +0x3FC occupants, +0x400 limite, +0x404 id), et c'est CE bloc que lit la
// fenêtre de salon pour son titre. Le propriétaire ne reçoit jamais
// `ZC_ROOM_NEWENTRY` pour son propre salon (`clif_dispchat` diffuse en AREA_WOSC,
// qui exclut la source) : cette copie locale est sa seule source. Le même bloc
// alimente aussi l'enregistrement de replay (`ChatRoom_RecordToReplay` 0x00C810A0).
// Détail complet dans le .cc, au-dessus de `SendCreateChatRoom`.
//
// PLAFONDS — ils viennent du SERVEUR, ne pas les élargir : `CHATROOM_TITLE_SIZE`
// = 36+1, `CHATROOM_PASS_SIZE` = 8+1, `MAX_CHAT_USERS` = 20. Le client natif colle
// exactement à ces trois valeurs ; en proposer davantage serait un mensonge
// d'interface.
//
// ── LA SALLE (`UIChatRoomWnd`, id 28 / 0x1C) ─────────────────────────────────
//
// Même module, deuxième fenêtre : une fois le salon créé (ou rejoint), c'est elle
// qui porte les messages, la liste des membres et la ligne de saisie. RE complète
// en §10 bis de docs/chat_room_re.md.
//
// 🔴 LES LIGNES SE PERDENT SI PERSONNE NE LES PREND. `UIWindowMgr_ChatAction`
// action 5 (`UIM_PUSH_INTO_CHATROOM`) fait `if (mgr[159]) mgr[159]->OnMsg(37, …)`
// et RIEN d'autre : pas de file d'attente, contrairement à l'action 1 du chat
// principal. Détruire la fenêtre 28 ne fait donc fuir aucune mémoire — mais rend
// la salle MUETTE tant qu'on n'intercepte pas. L'interception passe par le filtre
// que `ChatWindow` a déjà posé sur `0x00A4AD20` (il n'y a qu'un jeu d'octets à
// cette adresse : un second détour tuerait le premier en silence), qui nous
// appelle via `chatroomwnd::IngestRoomLine`.
//
// ENVOI. Copie fidèle du chemin natif (`UIChatRoomWnd_OnMsg` @0x008823CF) :
// `ChatCmd_TryRegisteredHandler`, puis `ChatCmd_LookupSlashCommandTable`, puis
// `Chat_SetPendingSendText`, et enfin `CMode::SendMsg(0x2A, cmdId, args)` — avec
// LA subtilité qui distingue la salle du chat principal : **`cmdId 0` devient
// `0x32`** (`TT_NORMAL_FROM_CHATROOM`) et `0x33` devient `0x34`. C'est cette
// substitution, et elle seule, qui envoie le texte au salon plutôt qu'à la carte.
// Côté serveur c'est le MÊME paquet `CZ_REQUEST_CHAT 0x008C` : `clif_parse_GlobalMessage`
// route en `CHAT_WOS` dès que `sd->chatID` est posé — « without self », donc
// l'écho de nos propres lignes est LOCAL, d'où le passage par `SendMsg`.
//
// 🔴 Ces appels natifs ne sont JAMAIS joués pendant une frame ImGui (ils peuvent
// ouvrir des modales qui relancent le rendu) : le rendu ARME, `FlushPending` joue,
// depuis `Bourgeon::OnProcessInput`. Même discipline que `ChatWindow`.
//
// LOT 5 non fait : `UIChatRoomChangeWnd` (id 30) reste NATIVE. Le bouton
// « Réglages » l'ouvre telle quelle.
class ChatRoomWindow : public Plugin {
 public:
  ChatRoomWindow();

  const char* name() const override { return "ChatRoomWindow"; }

  void OnTick() override;      // détruit la native, purge l'état hors du jeu
  void OnRenderUI() override;  // dessine le formulaire

  // ZC_ACK_CREATE_CHATROOM 0x00D6, en OBSERVATION (pas en remplacement) : le
  // handler natif garde ses devoirs — remise à zéro de `CGameMode+0xFC`, ligne de
  // chat, et ouverture de `UIChatRoomWnd` (id 28) sur succès. On ne lit que le
  // statut, pour savoir s'il faut fermer notre fenêtre ou y afficher l'erreur.
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Appelé par le hook `MakeWindow` quand le client fabrique la fenêtre 27.
  // Bascule, comme `ToggleWindowById` : la native étant détruite à chaque tick,
  // elle n'existe jamais et TOUTE demande d'ouverture repasse forcément ici.
  void HandleNativeCreation(void* win);

  // Idem pour la fenêtre 28 (la SALLE). ⚠ Pas une bascule, celle-là : le client
  // la crée sur un ÉVÉNEMENT (acquittement de création, arrivée d'un membre),
  // jamais sur une demande du joueur. La refermer sur un second appel perdrait la
  // salle à l'arrivée du deuxième occupant.
  void HandleNativeRoomCreation(void* win);

  // Idem pour la fenêtre 30 (« Réglages du salon »). Elle est NATIVE-morte elle
  // aussi : c'est notre formulaire, en mode « modifier », qui la remplace.
  void HandleNativeChangeCreation(void* win);

  // Et la petite fenêtre « Veuillez saisir le mot de passe » (`UIPasswordWnd`,
  // id 29 / 0x1D), celle qui s'ouvre au clic sur le panneau d'un salon PRIVÉ.
  //
  // ⚠ Elle ne porte pas encore l'identifiant du salon quand `MakeWindow` rend la
  // main : son ouvreur (`UIChatRoomTitle`, vtable +0x68 @0x00826BE0) le lui pose
  // JUSTE APRÈS, par un `msg 47`. On se contente donc de la masquer ici, et on
  // relit `+0xBC` à la frame suivante avant de la détruire.
  void HandleNativePasswordCreation(void* win);

  // La salle est-elle ouverte ? Alors c'est ELLE qui porte la barre de saisie de
  // la chatbox — celle-ci cède la place, et l'envoi natif applique la
  // substitution d'id de commande du salon. `imgui_enabled_` compte : en
  // interface native, la salle est la fenêtre 28 du client et rien ne bouge.
  bool OwnsChatInput() const { return imgui_enabled_ && room_open_; }

  // Ramène la salle au premier plan (la chatbox y renvoie le joueur).
  void FocusRoom() { if (room_open_) room_focus_request_ = true; }

  // Joue les appels natifs armés pendant la frame. Appelé par
  // `Bourgeon::OnProcessInput`, donc HORS frame ImGui.
  void FlushPending();

  // Une ligne de salon, prise au filtre `ChatAction` action 5 de ChatWindow.
  // `cp949` est le texte tel que le client l'a monté. Rend true quand nous
  // l'avons prise : l'appelant doit alors l'avaler (sans quoi le natif la
  // pousserait dans une fenêtre 28 qui n'existe plus — sans effet, mais autant
  // être explicite).
  bool IngestRoomLine(const char* cp949, uint32_t rgb);

  // 🔴 LA ligne de chat ordinaire, quand un salon est ouvert. Le client route
  // lui-même sur `g_UIChatRoomWnd_Slot != 0` : salon (action 5, couleur 0x222222)
  // OU chat principal (action 1). Comme nous DÉTRUISONS la fenêtre 28, ce slot
  // est toujours nul et il envoie tout au chat global — un salon privé dont le
  // contenu sort du salon. On reprend donc l'aiguillage : tant que notre salle est
  // ouverte, une ligne de chat public est une ligne de SALON.
  //
  // C'est sûr, et pas une heuristique : le serveur envoie le message d'un salon
  // en `CHAT_WOS` et le message public en `AREA_CHAT_WOC` — « hearable area,
  // WITHOUT CHATROOMS ». Un occupant de salon ne reçoit donc PAS le chat public de
  // la carte : tout ce qui arrive pendant qu'on est en salon vient du salon.
  //
  // Rend true quand la ligne a été prise (l'appelant doit alors l'avaler).
  bool ClaimPublicChatLine(const char* cp949, uint32_t rgb, int type);

  // ── Setting PERSISTANT (bourgeon_settings.yaml, chargé/sauvé par MoonlightUi)
  // « chatroom_imgui ». HORS du groupe « Interface moderne », et ON par défaut,
  // pour la même raison que le menu Échap et la liste de macros : ce formulaire
  // ne dépend d'AUCUNE autre fenêtre moderne — il n'échange pas d'objets et ne
  // lit pas le modèle d'inventaire ; il route sa demande par le chemin natif du
  // client. Il rend donc le même service dans les deux modes, et le priver de
  // ceux qui jouent en interface native n'aurait servi personne. Mettre la clé à
  // `false` rend intégralement la fenêtre native.
  bool imgui_enabled_ = true;

 private:
  // Ferme notre fenêtre et remet l'état au repos. Aucun paquet : le serveur ne
  // tient aucun état tant que 0x00D5 n'est pas parti, exactement comme le bouton
  // Annuler natif qui se contente d'un `SaveRectAndCloseWindow(27)`.
  void Close();

  // Valide dans l'ORDRE du natif puis envoie. Pose `error_` et `error_field_`
  // au lieu d'ouvrir une boîte modale — c'est tout l'intérêt de la fenêtre.
  void Send();

  void SetError(const char* utf8, int field);
  void SetErrorFromMsgString(int msg_id, int field);

  // État de session. `open_` est à NOUS : il ne peut plus être déduit de la
  // présence de la fenêtre native, qui ne survit pas à un tick.
  bool open_       = false;
  bool need_pos_   = false;  // (re)centrer à l'ouverture
  bool show_panel_ = true;   // transitoire : détection du clic sur le X

  // Saisie, en CP949 — c'est l'encodage du FIL, et `ro::InputTextCp949` édite
  // directement dans cette forme. Les tampons sont volontairement plus grands que
  // le plafond du serveur : on préfère laisser le joueur taper trop long, le lui
  // MONTRER (compteur d'octets qui passe au rouge) et bloquer l'envoi, plutôt que
  // de tronquer en silence comme le ferait un tampon calé au plafond.
  char title_[64]    = {0};  // plafond réel : 36 octets
  char password_[24] = {0};  // plafond réel : 8 octets
  int  limit_        = 20;   // = MAX_CHAT_USERS, et le défaut du natif
  bool is_public_    = true;
  bool show_password_ = false;  // l'œil : le natif masque sans jamais révéler

  // ── Le volet « Réglages » de la salle ───────────────────────────────────────
  // Un volet DANS la fenêtre, pas une fenêtre détachée : le natif en faisait une
  // fenêtre à part (la 30) uniquement parce qu'il n'avait pas mieux. Champs à
  // eux, pour qu'ouvrir les réglages ne marche pas sur le brouillon de création.
  // Demande de mise au premier plan de la salle : posée quand le joueur redemande
  // « créer un salon » alors qu'il est déjà dans un — la bonne réponse est de lui
  // montrer sa salle, pas de refuser en silence.
  bool room_focus_request_ = false;

  bool room_show_settings_ = false;
  char set_title_[64]      = {0};  // CP949
  char set_password_[24]   = {0};  // CP949
  int  set_limit_          = 20;
  bool set_public_         = true;

  // 🔴 NOTRE copie du mot de passe du salon (octets du FIL), et la seule qui
  // vaille. Le serveur ne le renvoie sur aucun paquet, et le bloc du CGameMode
  // — qui le portait à la création — n'est plus mis à jour depuis que
  // « Appliquer » part en paquet brut (`SendMsg(44)` est gardé par un slot que
  // nous mettons à zéro). Le relire là rendait donc l'ANCIEN mot de passe après
  // un changement.
  std::string room_password_wire_;

  // ── L'invite de mot de passe (remplace `UIPasswordWnd`, id 29) ─────────────
  void DrawPasswordPrompt();

  bool     pw_open_        = false;   // notre invite est à l'écran
  bool     pw_show_panel_  = true;
  bool     pw_native_wait_ = false;   // la native est née, son id n'est pas encore lu
  uint32_t pw_chat_id_     = 0;       // l'identifiant du salon visé
  char     pw_input_[24]   = {0};     // CP949, plafond 8 comme le natif
  bool     pw_reveal_      = false;
  char     pw_error_[192]  = {0};     // motif de refus, DANS l'invite

  // 🔴 L'ÉTAT AFFICHÉ du volet, qui suit `room_show_settings_` avec UNE FRAME de
  // retard — et l'ordre compte. ImGui ne redimensionne pas tout seul une fenêtre
  // dont le contenu grandit : il faut donc que la fenêtre grandisse D'ABORD, et
  // que le volet n'apparaisse qu'ENSUITE. L'inverse (volet dessiné tout de suite,
  // fenêtre agrandie à la frame suivante) fait déborder le contenu pendant une
  // frame, et ImGui sort une barre de défilement qui disparaît aussitôt : le
  // scintillement observé en jeu.
  bool  room_settings_shown_ = false;
  float room_last_h_         = 0.0f;

  // Paquet parti, acquittement attendu. C'est CE drapeau qui garde la fenêtre
  // ouverte le temps de la réponse — et qui bloque un second envoi.
  bool          waiting_ack_ = false;
  unsigned long sent_tick_   = 0;

  // Message d'erreur affiché DANS la fenêtre, et le champ à surligner
  // (0 = aucun, 1 = titre, 2 = mot de passe).
  char error_[192]  = {0};
  int  error_field_ = 0;

  // ── La SALLE ───────────────────────────────────────────────────────────────
  void HandleRoomPacket(uint16_t opcode, const uint8_t* data, uint16_t len);
  void CloseRoom();
  void DrawRoom();
  void LoadSettingsPane();  // pré-remplit le volet sur le salon courant
  void DrawSettingsPane();  // le volet « Réglages », DANS la fenêtre de salle
  static float SettingsPaneHeight();  // de combien la fenêtre doit grandir
  void RoomJoined();       // remise à zéro à l'entrée dans un salon
  void OpenRoomAsOwner();  // ouverture par CELUI qui vient de créer le salon

  struct RoomMember {
    std::string name;   // UTF-8
    bool        owner;  // porte les droits
  };

  bool room_open_       = false;
  bool room_show_panel_ = true;
  bool room_need_pos_   = false;
  bool room_i_am_owner_ = false;

  // L'état du salon, miroir de ce que le natif garde à `UIChatRoomWnd+0xB4`.
  std::string room_title_;   // UTF-8
  bool        room_public_ = true;
  int         room_users_  = 0;
  int         room_limit_  = 0;

  // 🔴 Les LIGNES ne sont plus ici. Elles vivent dans le modèle de `ChatWindow`,
  // sous un tag de conversation réservé, et c'est LUI qui les dessine — avec son
  // rendu riche complet (balises, liens d'objets, emotes, icônes, gras/italique,
  // couleurs). Voir `chatwnd::IngestChatRoomLine` / `DrawChatRoomLog`.
  //
  // En écrire un second ici en aurait fait une sixième copie du même rendu, ce que
  // `project_link_label_widget_todo` demande précisément d'arrêter.
  //
  // Les membres et l'état, eux, ne viennent que de `HandlePacket` — fil principal,
  // donc pas de verrou.
  std::vector<RoomMember> room_members_;

  // Les salons VISIBLES autour de nous, tenus depuis `ZC_ROOM_NEWENTRY 0x00D7`
  // (le panneau au-dessus de la tête du propriétaire) et `ZC_DESTROY_ROOM 0x00D8`.
  //
  // 🔴 C'est la seule source du titre et de la limite quand on REJOINT le salon
  // d'un autre : `ZC_ENTER_ROOM` ne porte que l'identifiant et les membres. Le
  // natif, lui, va les relire dans l'objet du panneau (`acteur+0x268`, info à
  // `+0x80`) — même donnée, autre chemin. Ce registre est aussi la matière du
  // futur annuaire des salons proches (lot 3 bis).
  struct NearbyRoom {
    uint32_t    id = 0;
    std::string title;   // UTF-8
    int         limit = 0;
    int         users = 0;
    bool        is_public = true;
  };
  std::vector<NearbyRoom> nearby_rooms_;

  // 🔴 PAS DE SAISIE ICI. C'est la barre de la CHATBOX qui se dessine au bas de
  // cette fenêtre (`chatwnd::DrawChatInputRow`), parce que dans un salon elle est
  // déjà le seul chemin vers les autres : le serveur route un message ordinaire
  // vers le salon dès que `sd->chatID` est posé. Une saisie propre au salon
  // faisait donc exactement la même chose que celle d'à côté, avec ses liens, son
  // historique et ses préfixes à re-câbler un par un.
  //
  // Ce qui a disparu avec elle : `room_input_`, `room_pending_insert_` (les liens
  // Maj+cliqués retournent à la barre, qui est ici), `room_whisper_to_` (le
  // chuchotement passe par la box destinataire de la barre, où le joueur le VOIT)
  // et l'envoi natif `SendRoomChatText`.
  std::string room_context_member_;  // membre visé par le menu contextuel

  // Appels natifs ARMÉS pendant la frame, joués par FlushPending.
  enum class Pending {
    kNone, kLeave, kExpel, kGiveOwner, kApplySettings, kEnterRoom
  };
  Pending     pending_        = Pending::kNone;
  std::string pending_text_;    // CP949 (kEnterRoom : le mot de passe)
  std::string pending_member_;  // CP949 (kExpel / kGiveOwner)
};

// Pont pour le filtre `ChatAction` de ChatWindow (action 5). Défini dans
// chat_room_window.cc, appelé depuis chat_window.cc : les deux modules ne
// s'incluent pas l'un l'autre, seule cette fonction traverse.
namespace chatroomwnd {
bool IngestRoomLine(const char* cp949, uint32_t rgb);
bool ClaimPublicChatLine(const char* cp949, uint32_t rgb, int type);

// Un salon est-il ouvert, et porte-t-il donc la barre de saisie ? La chatbox le
// demande UNE fois par frame pour savoir si elle doit dessiner sa barre ou céder
// la place ; `NativeSendChatText` s'en sert aussi pour la substitution d'id de
// commande du natif. La réponse ne dépend pas de l'ordre de rendu : elle vient
// des paquets.
bool RoomOwnsChatInput();

// Ramène la fenêtre du salon devant. La chatbox l'appelle quand le joueur clique
// le mot qui a remplacé sa barre.
void FocusRoom();
}  // namespace chatroomwnd
