#include "ragnarok/globals.h"
#include "features/windows/chat_room_window.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bourgeon.h"            // Bourgeon::Instance().SendPacket / session
#include "imgui.h"
#include "features/windows/chat_window.h"  // chatwnd:: — le rendu riche du log
#include "ragnarok/msgstring.h"  // msgstr::Utf8Or (libellés natifs du client)
#include "ragnarok/uiwnd.h"      // uiwnd::SafeFindWindow / SafeCloseWindow
#include "ui/ro_imgui.h"         // skin RO (BeginRoWindow / RoButton / InputTextCp949)
#include "ui/ro_widgets.h"       // enveloppes mui:: (WheelSliderInt, HelpMarker)
#include "utils/i18n.h"
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client

using namespace mui;

// ── Constantes RE (client 20250716, base 0x400000) ───────────────────────────
// Tout est détaillé dans docs/chat_room_re.md ; on ne recopie ici que ce qui sert.
namespace {

// ── Constantes de la SALLE (docs/chat_room_re.md §10 bis) ───────────────────
// Déclaré ICI, dans le premier namespace anonyme, parce que le constructeur et
// OnTick — tous deux plus haut dans le fichier — s'en servent. (Les quatre
// identifiants de fenêtre, eux, sont au foyer.)
//
// Offset où `UIPasswordWnd` range l'identifiant du salon, posé par son `msg 47`
// (relevé dans son OnMsg, 0x008C6A20).
constexpr int kOffPasswordWndChatId = 0xBC;

// Les ZC du salon (docs/opcode_map.csv). Tous OBSERVÉS : les handlers natifs
// gardent leurs devoirs (lignes de chat, globales), on ne lit que les champs.
constexpr uint16_t kOpRefuseEnter  = 0x00DA;  // fixe  3 : entrée refusée
constexpr uint16_t kOpRoomNewEntry = 0x00D7;  // var  17 : panneau d'un salon visible
constexpr uint16_t kOpDestroyRoom = 0x00D8;  // fixe  6 : le salon disparaît
constexpr uint16_t kOpEnterRoom   = 0x00DB;  // var   8 : j'entre — liste complète
constexpr uint16_t kOpMemberNew   = 0x00DC;  // fixe 28 : un membre arrive
constexpr uint16_t kOpMemberExit  = 0x00DD;  // fixe 29 : un membre part
constexpr uint16_t kOpChangeRoom  = 0x00DF;  // var  17 : réglages modifiés
constexpr uint16_t kOpRoleChange  = 0x00E1;  // fixe 30 : nouveau propriétaire

// Les commandes, toutes par CMode::SendMsg — jamais de paquet fabriqué à la main
// (la leçon de SendCreateChatRoom vaut ici aussi).
// `SendMsg(45)` = entrer dans un salon. p1 = identifiant, p2 = mot de passe
// (C-string) — ordre tranché par l'OnMsg de UIPasswordWnd, qui fait
// `SendMsg(45, this[+0xBC], UIEdit_GetTextPtr(this[+0xB4]))`.
// ⚠ Il est gardé par `if (g_UIChatRoomWnd_Slot != 0) return;` — mais dans le BON
// sens pour nous : ce slot vaut zéro puisque nous détruisons la 28, donc la
// commande passe. Le natif porte aussi son anti-rebond (mode+0xFC, 1 s).
constexpr int kModeMsgEnterRoom = 45;  // -> CZ_REQ_ENTER_ROOM 0x00D9

constexpr int kModeMsgGiveOwner = 46;  // -> CZ_REQ_ROLE_CHANGE   0x00E0 (nom, rôle)
constexpr int kModeMsgExpel     = 47;  // -> CZ_REQ_EXPEL_MEMBER  0x00E2 (nom)
constexpr int kModeMsgLeave     = 48;  // -> CZ_EXIT_ROOM         0x00E3
// 🔴 LE PIPELINE D'ENVOI N'EST PLUS ICI. Ni `kCmdHandlerMap`, ni la table slash,
// ni `g_ChatPendingSendText`, ni la substitution d'id de commande : la barre de
// saisie de la CHATBOX se dessine au bas de cette fenêtre et fait tout cela une
// fois pour toutes (chat_window.cc, `NativeSendChatText`, qui porte désormais la
// substitution « /savechat » du salon).
//
// La recette native complète (`UIChatRoomWnd_OnMsg` @0x008823CF, les quatre ids
// TT_*, les trois adresses) reste dans docs/chat_room_re.md §10bis.5 et §13.

// Ids MsgStringTable propres à la salle.
constexpr int kMsgRoomPrivateShort = 89;   // MSI_ROOM_PRIVATE — « Priv. »
constexpr int kMsgRoomPublicShort  = 90;   // MSI_ROOM_PUBLIC  — « Publ. »
constexpr int kMsgGiveRoomPower    = 128;  // MSI_GIVE_GIVE_ROOM_POWER
constexpr int kMsgEnterPasswordPrompt = 15;  // MSI_ENTER_PASSWORD — le libellé natif

// Motifs de refus de `ZC_REFUSE_ENTER_ROOM`, avec l'id MsgStringTable que le
// client leur associe (relevé dans son handler, 0x00CD2700). Le natif les écrit
// en chat et ferme sa fenêtre ; nous les montrons DANS l'invite, qui reste ouverte.
constexpr int kMsgRefuseFull      = 67;   // MSI_TOO_MANY_PEOPLE_IN_ROOM
constexpr int kMsgRefuseWrongPass = 7;    // MSI_INCORRECT_PASSWORD
constexpr int kMsgRefuseKicked    = 68;   // MSI_YOU_HAVE_BANNED_FROM_THE_ROOM
constexpr int kMsgRefuseNoZeny    = 55;   // MSI_INSUFFICIENT_MONEY
constexpr int kMsgRefuseLowLevel  = 432;  // MSI_NOT_ENOUGH_LEVEL
constexpr int kMsgRefuseHighLevel = 433;  // MSI_TOO_HIGH_LEVEL
constexpr int kMsgRefuseJob       = 434;  // MSI_NOT_ACCEPTABLE_JOB

// La couleur que le CLIENT donne aux lignes de salon (`ChatAction(5, txt,
// 0x222222, …)`, relevée dans les handlers ZC). SOMBRE, parce que le corps d'une
// fenêtre RO est CLAIR — le gris pâle qu'il y avait ici était illisible.
constexpr uint32_t kRoomLineRgb = 0x222222u;


constexpr int kMsgChangeRoomSetting = 126;  // MSI_CHANGE_ROOM_SETTING — « Réglages du salon »

// Le module, pour le pont `chatroomwnd::IngestRoomLine` que ChatWindow appelle.
ChatRoomWindow* g_chat_room_window = nullptr;

// ⚠ `uiwnd::kUIChatRoomMakeWnd` ne sert qu'au filet de sécurité d'OnTick : la
// fenêtre elle-même ne survit jamais plus d'un tick.

// Plafonds — ils viennent du SERVEUR (src/map/map.hpp : CHATROOM_TITLE_SIZE 36+1,
// CHATROOM_PASS_SIZE 8+1 ; src/map/chat.hpp : MAX_CHAT_USERS 20). Le client natif
// colle exactement dessus : titre max 36 (UIEdit+0x88), mot de passe max 8, liste
// de limites bornée à 20.
constexpr int kTitleMaxBytes = 36;
constexpr int kPassMaxBytes  = 8;
constexpr int kPassMinBytes  = 4;   // exigé UNIQUEMENT pour un salon privé
constexpr int kLimitMin      = 2;   // la plus petite entrée du déroulant natif
constexpr int kLimitMax      = 20;  // = MAX_CHAT_USERS

// Opcodes. Seul l'ACQUITTEMENT est manipulé ici : la demande part par le chemin
// natif (voir SendCreateChatRoom plus bas), qui sérialise CZ_CREATE_CHATROOM
// 0x00D5 lui-même.
constexpr uint16_t kOpAck = 0x00D6;  // ZC_ACK_CREATE_CHATROOM (fixe 3 o)

// Statuts de 0x00D6 — `e_create_chatroom` côté serveur.
constexpr uint8_t kAckSuccess       = 0;
constexpr uint8_t kAckLimitExceeded = 1;
constexpr uint8_t kAckAlreadyExists = 2;

// Ids MsgStringTable employés par la fenêtre native (docs/chat_room_re.md §6/§9).
// On dit le texte EXACT du client — règle du projet — avec un repli traduit pour
// les ids que `data\msgstringtable.txt` n'a pas (cf. msgstr::Utf8Or).
constexpr int kMsgEnterRoomTitle   = 13;   // « Veuillez saisir le titre du salon. »
constexpr int kMsgBadWord          = 14;   // « Langage inapproprié détecté. »
constexpr int kMsgEnterPassword    = 15;   // « Veuillez saisir le mot de passe. »
constexpr int kMsgPasswordTooShort = 16;   // « ... au moins 4 caractères. »
constexpr int kMsgRoomIsMade       = 64;   // « Le salon a bien été créé. »
constexpr int kMsgTooManyRoom      = 65;   // « Nombre maximal de salons atteint. »
constexpr int kMsgSameRoomTitle    = 66;   // « Un salon du même nom existe déjà. »
constexpr int kMsgMakingRoom       = 125;  // « Créer un salon de chat » (titre)
constexpr int kMsgCountUnitPeople  = 131;  // « pers. »
constexpr int kMsgEnglishOnly      = 190;  // « ... que les caractères anglais. »
constexpr int kMsgCantMakeChatRoom = 661;  // « Vous ne pouvez pas ouvrir de fenêtre de chat. »
constexpr int kMsgRoomPrivate      = 200;  // « Privé »
constexpr int kMsgRoomPublic       = 201;  // « Public »
// Ces cinq-là existent dans la table et sont DÉJÀ traduits, mais aucun code de ce
// client ne les lit : la fenêtre native n'a pas de libellés, elle compte sur son
// fond. Ils sont donc gratuits pour nous.
constexpr int kMsgRoomTitleLabel = 4316;  // « Titre : »
constexpr int kMsgRoomLimitLabel = 4317;  // « Limite : »
constexpr int kMsgRoomPassLabel  = 4320;  // « Mot de passe : »

// `ChatRoom_IsCellBlockedByRoomTitle` (0x00A38C60) — __thiscall(mgr, aid).
// Parcourt la liste des fenêtres du gestionnaire, garde celles dont le RTTI est
// UIChatRoomTitle ou UIMerchantShopTitle, et compare la CELLULE de leur acteur
// propriétaire à la nôtre : « un panneau de salon ou d'échoppe occupe déjà ma
// case ». Purement client — le serveur a sa propre règle (CELL_CHKNOCHAT).
constexpr uintptr_t kIsCellBlockedAddr = 0x00a38c60;

// `BannedWord_Contains` (0x00A85BE0) — __thiscall(table, cp949) : renvoie vrai
// quand la chaîne contient un mot interdit (c'est la négation de ScanClean).
constexpr uintptr_t kBannedWordTableAddr    = 0x0159c2c8;

// `g_ServiceType` — vaut 1 sur Moonlight (MESURÉ en jeu le 2026-08-23, x32dbg).
// À 1, le client natif impose un titre en ASCII 7 bits.
constexpr uintptr_t kServiceTypeAddr = 0x0159b810;
constexpr int       kServiceTypeIntl = 1;

// ── Ponts natifs, tous sous SEH ──────────────────────────────────────────────
// Motif du projet : POD uniquement à l'intérieur du __try (aucun objet C++ à
// dérouler), et un repli qui laisse passer plutôt que de bloquer — un garde-fou
// d'affichage qui échoue ne doit pas empêcher le joueur d'ouvrir un salon, le
// serveur revalide de toute façon.

bool CellBlockedByRoomTitle(uint32_t aid) {
  __try {
    using Fn = char(__fastcall*)(void*, void*, int);
    return reinterpret_cast<Fn>(kIsCellBlockedAddr)(
               uiwnd::Mgr(), nullptr, static_cast<int>(aid)) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool ContainsBannedWord(const char* cp949) {
  if (!cp949 || !*cp949) return false;
  __try {
    using Fn = bool(__fastcall*)(void*, void*, const char*);
    return reinterpret_cast<Fn>(rag::kBannedWordContainsAddr)(
        reinterpret_cast<void*>(kBannedWordTableAddr), nullptr, cp949);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

int ServiceType() {
  __try {
    return *reinterpret_cast<const int*>(kServiceTypeAddr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// « Tous les octets sont < 0x80 » — la réimplémentation exacte de
// `Str_IsPureAscii7` (0x00D71EF0), qui teste `s[i] >= 0` sur un char SIGNÉ. Deux
// lignes, aucune raison d'appeler le natif pour ça ; et surtout on en a besoin
// PENDANT la frappe, pour surligner, pas seulement au moment d'envoyer.
bool IsPureAscii7(const char* s) {
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p)
    if (*p >= 0x80) return false;
  return true;
}

// Le bloc « salon courant » du CGameMode, écrit par `CMode::SendMsg(43/44)` et
// relu par le natif pour intituler sa fenêtre de salle (docs/chat_room_re.md
// §7.2 et §10bis.3). C'est NOTRE source à la création : le propriétaire ne reçoit
// jamais `ZC_ROOM_NEWENTRY` pour son propre salon (AREA_WOSC exclut la source).
constexpr int kModeRoomTitleOff  = 0x3C8;  // std::string
constexpr int kModeRoomPublicOff = 0x3F8;  // int
constexpr int kModeRoomUsersOff  = 0x3FC;  // int
constexpr int kModeRoomLimitOff  = 0x400;  // int

// Lit une std::string MSVC (SSO 15, sinon pointeur) dans un tampon POD.
void ReadStdStringPod(const uint8_t* str, char* out, size_t out_size) {
  out[0] = 0;
  rag::clientstr::CopyTruncating(str, out, static_cast<int>(out_size));
}

// Le bloc, en une lecture SEH. POD only (règle C2712).
struct ModeRoomInfo {
  char title[64];
  int  is_public;
  int  users;
  int  limit;
  bool valid;
};
ModeRoomInfo ReadModeRoomInfo() {
  ModeRoomInfo info{};
  void* mode = nullptr;
  __try {
    mode = rag::ActiveModeIfReady();
  } __except (EXCEPTION_EXECUTE_HANDLER) { return info; }
  if (!mode) return info;
  __try {
    const uint8_t* m = reinterpret_cast<const uint8_t*>(mode);
    ReadStdStringPod(m + kModeRoomTitleOff, info.title, sizeof(info.title));
    info.is_public = *reinterpret_cast<const int*>(m + kModeRoomPublicOff);
    info.users     = *reinterpret_cast<const int*>(m + kModeRoomUsersOff);
    info.limit     = *reinterpret_cast<const int*>(m + kModeRoomLimitOff);
    info.valid     = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { info.valid = false; }
  return info;
}

// Le mot de passe du salon courant. Il n'est sur AUCUN paquet reçu : le serveur
// ne le renvoie jamais. Sa seule trace côté client est ce bloc, où
// `CMode::SendMsg(43)` l'a écrit à la création.
constexpr int kModeRoomPassOff = 0x3E0;  // std::string
void ReadModeRoomPassword(char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  out[0] = 0;
  void* mode = nullptr;
  __try {
    mode = rag::ActiveModeIfReady();
  } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
  if (!mode) return;
  __try {
    ReadStdStringPod(reinterpret_cast<const uint8_t*>(mode) + kModeRoomPassOff,
                     out, out_size);
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
}

// ── MODIFIER les réglages : `CZ_CHANGE_CHATROOM 0x00DE`, en PAQUET BRUT ───────
//
// 🔴 Et c'est le même cas de figure que « Quitter », pas celui de « Créer ».
// `CMode::SendMsg(44)` commence par `if (g_UIChatRoomWnd_Slot == 0) return;`
// (bloc 0x00C8C6AC) : comme nous DÉTRUISONS la fenêtre 28, ce slot vaut toujours
// zéro et la commande serait refusée en silence. Le chemin natif nous est donc
// fermé, et le paquet brut est la seule voie.
//
// ⚠ CE QU'ON PERD, et il faut le savoir : le `case 44` mettait aussi à jour le
// bloc « salon courant » du CGameMode et l'enregistrement de replay. Le premier
// n'a plus de lecteur (la fenêtre native qui s'en servait est morte, et notre
// état se remet à jour sur `ZC_CHANGE_CHATROOM 0x00DF`) ; le second, si, et une
// session de replay enregistrera donc la création mais pas la modification.
//
// Mise en forme STRICTEMENT identique à 0x00D5 (docs/chat_room_re.md §7.3).
#pragma pack(push, 1)
struct ChangeChatRoomHeader {
  uint16_t opcode;
  uint16_t len;          // 15 + longueur du titre
  uint16_t limit;
  uint8_t  type;         // 1 = public, 0 = privé
  char     password[8];  // complété de zéros
};
#pragma pack(pop)
static_assert(sizeof(ChangeChatRoomHeader) == 15, "en-tête CZ_CHANGE_CHATROOM");

bool SendChangeChatRoom(const char* title_cp949, const char* password_cp949,
                        bool is_public, int limit) {
  const size_t title_len = std::strlen(title_cp949);
  const size_t pass_len  = std::strlen(password_cp949);
  if (title_len == 0 || title_len > 36) return false;

  uint8_t buf[sizeof(ChangeChatRoomHeader) + 36] = {0};
  auto* head   = reinterpret_cast<ChangeChatRoomHeader*>(buf);
  head->opcode = 0x00DE;
  head->len    = static_cast<uint16_t>(sizeof(ChangeChatRoomHeader) + title_len);
  head->limit  = static_cast<uint16_t>(limit);
  head->type   = is_public ? 1 : 0;
  if (!is_public)
    std::memcpy(head->password, password_cp949,
                pass_len < 8 ? pass_len : 8);
  std::memcpy(buf + sizeof(ChangeChatRoomHeader), title_cp949, title_len);
  return Bourgeon::Instance().SendPacket(buf, head->len);
}

// ── L'ENVOI : `CMode::SendMsg(43)`, et surtout PAS un paquet brut ────────────
//
// 🔴 LEÇON PAYÉE EN JEU (2026-08-23). La première version montait
// `CZ_CREATE_CHATROOM 0x00D5` à la main : la mise en forme est entièrement connue
// (docs/chat_room_re.md §7.3) et le salon se créait bien. Mais la barre de titre
// de la fenêtre de salon affichait « Publ. : (1/-657931) » — titre vide, limite
// aberrante (-657931 = 0xFFF5F5F5, une constante de couleur lue à côté).
//
// Cause : le `case 43` de `CMode::SendMsg` ne fait pas QUE sérialiser. Il recopie
// d'abord la demande dans un bloc « salon courant » du CGameMode, et c'est CE
// bloc — pas le paquet, pas la réponse serveur — que lit la fenêtre de salon pour
// s'intituler. Le propriétaire ne reçoit d'ailleurs jamais `ZC_ROOM_NEWENTRY`
// pour son propre salon (`clif_dispchat` diffuse en AREA_WOSC, qui exclut la
// source) : cette copie locale est sa SEULE source de vérité.
//
//   CGameMode+0x3C8  std::string titre       CGameMode+0x3F8  int  public
//   CGameMode+0x3E0  std::string mot de passe CGameMode+0x3FC  int  occupants
//                                            CGameMode+0x400  int  limite
//                                            CGameMode+0x404  int  id de salon (-1)
//
// Et il y a un troisième devoir : `ChatRoom_RecordToReplay` (0x00C810A0) relit ce
// même bloc pour écrire la création dans le fichier de replay quand
// l'enregistrement tourne.
//
// D'où la règle générale, qui vaut au-delà de cette fenêtre : **quand un chemin
// natif fait plus qu'émettre, le rejouer à la main est un pari — pas une
// simplification.** Le coût du chemin natif se résume à deux `std::string` du CRT
// du jeu et à leur destructeur ; c'est très peu payé pour ne rien oublier.
//
// La struct attendue en p1 (64 octets, cf. docs/chat_room_re.md §7.1) :
struct CreateChatRoomRequest {
  uint8_t title[24];     // std::string, construite par le CRT du JEU
  uint8_t password[24];  // idem
  int32_t is_public;     // 1 = public
  int32_t users;         // 0 à la création
  int32_t limit;
  int32_t room_id;       // -1
};
static_assert(sizeof(CreateChatRoomRequest) == 0x40, "struct CZ_CREATE_CHATROOM");

constexpr int kModeMsgCreateChatRoom = 43;  // -> CZ_CREATE_CHATROOM 0x00D5

// Le CRT du JEU — jamais celui de la DLL : ces `std::string` sont détruites par du
// code natif, et allouées par un autre tas elles le feraient planter.

// Envoie la demande par le chemin natif. Rend false si le mode de jeu n'est pas
// disponible (transition de carte, char-select) ou si l'appel a levé.
bool SendCreateChatRoom(const char* title_cp949, const char* password_cp949,
                        bool is_public, int limit) {
  // Le GETTER, pas la lecture directe de kActiveModePtr : il rend 0 tant que le
  // manager n'est pas en état 1 (cf. globals.h). Envoyer pendant un changement de
  // carte n'aurait aucun sens.
  void* mode = nullptr;
  __try {
    mode = rag::ActiveModeIfReady();
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  if (!mode) return false;

  __try {
    CreateChatRoomRequest req{};
    using StrCtor_t = void*(__fastcall*)(void*, void*, const char*);
    using StrDtor_t = void(__fastcall*)(void*, void*);
    auto* ctor = reinterpret_cast<StrCtor_t>(rag::kStdStringCtorCStrAddr);
    auto* dtor = reinterpret_cast<StrDtor_t>(rag::kStdStringDtorAddr);

    ctor(req.title, nullptr, title_cp949);
    // Un salon public n'a pas de mot de passe : le natif envoie ce que porte son
    // champ, mais laisser fuiter une chaîne qui ne servira pas est gratuit.
    ctor(req.password, nullptr, is_public ? "" : password_cp949);
    req.is_public = is_public ? 1 : 0;
    req.users     = 0;
    req.limit     = limit;
    req.room_id   = -1;

    // Le premier paramètre transporte l'ADRESSE de la requête — x86, donc un int
    // la porte sans perte (cf. rag::ModeSendMsg).
    rag::ModeSendMsg(mode, kModeMsgCreateChatRoom,
                     static_cast<int>(reinterpret_cast<intptr_t>(&req)));

    // 🔴 Destructeurs OBLIGATOIRES, comme pour la CSkillInfo du 0x71 : le natif
    // COPIE ce qu'il lui faut, il ne prend pas possession de la struct.
    dtor(req.password, nullptr);
    dtor(req.title, nullptr);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

ChatRoomWindow::ChatRoomWindow() {
  // OBSERVATION, pas remplacement : le handler natif de 0x00D6 fait trois choses
  // qu'on veut garder — il remet `CGameMode+0xFC` à zéro, il écrit la ligne de
  // chat (MSI 64/65/66, dans les mêmes couleurs que d'habitude) et, sur succès, il
  // ouvre `UIChatRoomWnd` (id 28), la vraie salle. Nous n'avons besoin que du
  // statut. Le paquet est à longueur FIXE (3 octets) : un seul octet utile après
  // l'opcode.
  Bourgeon::Instance().RegisterObserveOpcode(kOpAck, 1);

  g_chat_room_window = this;

  // Les paquets de la SALLE, tous en observation eux aussi. Les longueurs
  // demandées sont celles des paquets, y compris pour les deux variables : le
  // relais recopie ce nombre d'octets depuis le tampon du client, et nos
  // décodeurs se rebornent ensuite sur la taille ANNONCÉE dans le paquet.
  Bourgeon::Instance().RegisterObserveOpcode(kOpRefuseEnter, 1);
  Bourgeon::Instance().RegisterObserveOpcode(kOpRoomNewEntry, 17 + 36);  // + titre
  Bourgeon::Instance().RegisterObserveOpcode(kOpDestroyRoom, 4);
  Bourgeon::Instance().RegisterObserveOpcode(kOpEnterRoom, 8 + 28 * 20);  // MAX_CHAT_USERS
  Bourgeon::Instance().RegisterObserveOpcode(kOpMemberNew, 26);
  Bourgeon::Instance().RegisterObserveOpcode(kOpMemberExit, 27);
  Bourgeon::Instance().RegisterObserveOpcode(kOpChangeRoom, 17 + 36);  // + titre
  Bourgeon::Instance().RegisterObserveOpcode(kOpRoleChange, 28);
}

void ChatRoomWindow::HandleNativeCreation(void* win) {
  if (!imgui_enabled_) return;  // interface native : on ne touche à rien

  // Masquer AVANT la première frame. On ne peut PAS détruire ici : l'appelant de
  // MakeWindow déréférence encore le retour. `OnTick` détruit ensuite.
  uiwnd::SafeSetVisible(win, false);

  // Bascule, exactement comme `ToggleWindowById` : la native étant détruite à
  // chaque tick, chaque demande d'ouverture repasse ici. ⚠ AVANT la garde de case
  // ci-dessous : FERMER doit marcher en toutes circonstances, y compris si une
  // échoppe est venue se poser sur notre case pendant que le formulaire était
  // ouvert — sinon le raccourci cesserait de refermer sa propre fenêtre.
  if (open_) { Close(); return; }

  // 🔴 « Déjà dans un salon » — la garde du bouton natif, rejouée. `UIBasicInfoWnd`
  // (cmd 214) teste `if (g_UIChatRoomWnd_Slot) break;` avant d'ouvrir la 27 ; ce
  // slot vaut zéro en permanence puisque nous détruisons la fenêtre 28, donc la
  // garde ne jouait plus et le formulaire se rouvrait par-dessus une salle
  // ouverte. Le serveur, lui, refuserait la création en SILENCE
  // (`chat_createpcchat` : `if (sd->chatID) return 0;`).
  //
  // Plutôt qu'un refus muet, on met la salle au premier plan : c'est ce que le
  // joueur cherchait en cliquant « salon de chat », et ça se passe d'explication.
  if (room_open_) {
    room_focus_request_ = true;
    return;
  }

  // 🔴 La garde d'`OnCreate`, rejouée. `OnCreate` a bien tourné, et s'il a refusé
  // il a mis la fenêtre en FILE de destruction (`QueueDestroyWindow`) — mais
  // `MakeWindow` nous la rend quand même. Sans ce test, on ouvrirait notre
  // formulaire alors que le client vient d'écrire « Vous ne pouvez pas ouvrir de
  // fenêtre de chat » en chat. Le message a déjà été affiché par le natif : on se
  // contente de ne rien ouvrir.
  const uint32_t aid = Bourgeon::Instance().client().session().aid();
  if (aid != 0 && CellBlockedByRoomTitle(aid)) return;

  open_       = true;
  need_pos_   = true;
  show_panel_ = true;
  // ⚠ On NE vide PAS `title_` / `password_` / `limit_` / `is_public_`. Rouvrir la
  // fenêtre après un refus — ou après l'avoir fermée par mégarde — retrouve la
  // saisie. C'est le premier pas vers le brouillon persistant du lot 2, et ça ne
  // coûte rien puisque l'état est déjà à nous.
  error_[0]    = '\0';
  error_field_ = 0;
  waiting_ack_ = false;
}

void ChatRoomWindow::Close() {
  open_        = false;
  waiting_ack_ = false;
  error_[0]    = '\0';
  error_field_ = 0;
}

void ChatRoomWindow::OnTick() {
  // Interrupteur éteint : la fenêtre native a repris la main, on ne suit plus rien.
  if (!imgui_enabled_) {
    if (open_) Close();
    return;
  }

  // Sortie du monde (char-select, déconnexion) : la session est finie. Sans ça,
  // `open_` survivrait au changement de personnage et le formulaire se rouvrirait
  // tout seul sur le suivant.
  if (!Bourgeon::Instance().IsGameActive()) {
    if (open_) Close();
    // La SALLE, elle, ne survit pas : sortir du monde, c'est quitter le salon.
    if (room_open_) CloseRoom();
    // Et les panneaux alentour appartenaient à la carte qu'on vient de quitter.
    nearby_rooms_.clear();
    // La saisie du formulaire, en revanche, n'appartient pas au personnage : on
    // la garde d'un personnage à l'autre.
    return;
  }

  // 🔴 DESTRUCTION de la native. La masquer ne suffirait pas : invisible, elle
  // garde le clavier, et son bouton par défaut (commande 184) ENVOIE le paquet de
  // création sur une frappe d'Entrée. Détruite, elle n'existe jamais — ce qui fait
  // aussi du hook `MakeWindow` un point d'interception unique et suffisant.
  if (uiwnd::SafeFindWindow(uiwnd::kUIChatRoomMakeWnd))
    uiwnd::SafeCloseWindow(uiwnd::kUIChatRoomMakeWnd);
  // Idem pour la SALLE : elle déclare elle aussi un bouton par défaut, et le sien
  // ENVOIE le message tapé.
  if (uiwnd::SafeFindWindow(uiwnd::kChatRoomWndId))
    uiwnd::SafeCloseWindow(uiwnd::kChatRoomWndId);

  // L'invite de mot de passe : on lui VOLE son identifiant de salon avant de la
  // détruire. `FindWindow` frais plutôt qu'un pointeur gardé — le manager a pu la
  // reprendre entre-temps.
  if (pw_native_wait_) {
    void* w = uiwnd::SafeFindWindow(uiwnd::kChatRoomPasswordWndId);
    if (w) {
      uint32_t id = 0;
      __try {
        id = *reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(w) + kOffPasswordWndChatId);
      } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
      if (id != 0) {
        pw_chat_id_     = id;
        pw_open_        = true;
        pw_show_panel_  = true;
        pw_reveal_      = false;
        pw_input_[0]    = '\0';
        pw_error_[0]    = '\0';
        pw_native_wait_ = false;
      }
      uiwnd::SafeCloseWindow(uiwnd::kChatRoomPasswordWndId);
    } else {
      // Elle a disparu sans qu'on ait rien pu lire : ne pas rester en attente.
      pw_native_wait_ = false;
    }
  }

  // Filet : un serveur qui ne répond pas à 0x00D5 laisserait le bouton bloqué pour
  // toujours. Le natif, lui, ne s'en apercevrait pas — il a déjà fermé sa fenêtre.
  if (waiting_ack_ && GetTickCount() - sent_tick_ > 5000) {
    waiting_ack_ = false;
    SetError(i18n::Tr("Aucune réponse du serveur. Réessayez."), 0);
  }
}

void ChatRoomWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                  uint16_t len) {
  net_inbox_.Push(opcode, data, len);  // fil RÉSEAU : on copie, rien de plus
}

void ChatRoomWindow::HandlePacket(uint16_t opcode, const uint8_t* data,
                                  uint16_t len) {
  if (opcode != kOpAck) {
    if (imgui_enabled_) HandleRoomPacket(opcode, data, len);
    return;
  }
  if (len < 1) return;
  // Pas d'envoi en cours : cet acquittement ne nous concerne pas (interrupteur
  // éteint, ou salon créé par le chemin natif).
  if (!waiting_ack_) return;
  waiting_ack_ = false;

  switch (data[0]) {
    case kAckSuccess:
      // Le natif ouvre `UIChatRoomWnd` (id 28) et écrit MSI 64 en chat : notre
      // formulaire n'a plus rien à faire à l'écran.
      Close();
      // La saisie ne sert plus à rien non plus — le salon existe.
      title_[0]    = '\0';
      password_[0] = '\0';
      // 🔴 Et c'est ICI qu'on remplit la salle, pas dans le hook MakeWindow.
      // Deux raisons : ce hook tourne sur le FIL RÉSEAU (ce sont les handlers ZC
      // qui fabriquent la fenêtre 28), et il tourne AVANT que le client n'écrive
      // `CGameMode+0x3FC = 1` — on y lirait donc zéro occupant.
      OpenRoomAsOwner();
      break;
    case kAckLimitExceeded:
      SetErrorFromMsgString(kMsgTooManyRoom, 0);
      break;
    case kAckAlreadyExists:
      // LE cas qui justifie tout ce module : un caractère à changer, et la
      // fenêtre native aurait tout jeté.
      SetErrorFromMsgString(kMsgSameRoomTitle, 1);
      break;
    default:
      break;
  }
}

void ChatRoomWindow::SetError(const char* utf8, int field) {
  if (!utf8) utf8 = "";
  std::snprintf(error_, sizeof(error_), "%s", utf8);
  error_field_ = field;
}

void ChatRoomWindow::SetErrorFromMsgString(int msg_id, int field) {
  SetError(msgstr::Utf8Or(msg_id, i18n::Tr("Le serveur a refusé la demande.")),
           field);
}

void ChatRoomWindow::Send() {
  if (waiting_ack_) return;  // un envoi est déjà en vol

  const int title_len = static_cast<int>(std::strlen(title_));
  const int pass_len  = static_cast<int>(std::strlen(password_));

  // ── Validation, dans l'ORDRE EXACT du natif (UIChatRoomMakeWnd_OnMsg, cmd 184).
  // Même ordre, mêmes libellés : une fenêtre Bourgeon et son équivalent natif ne
  // doivent pas dire deux choses différentes pour la même erreur. Seule la FORME
  // change — un message dans la fenêtre au lieu d'une boîte modale.
  const uint32_t aid = Bourgeon::Instance().client().session().aid();
  if (aid != 0 && CellBlockedByRoomTitle(aid)) {
    // Le natif écrit ce message-là en CHAT (et non en boîte) ; nous le mettons
    // sous les yeux du joueur, là où il regarde.
    SetError(msgstr::Utf8Or(kMsgCantMakeChatRoom,
                            i18n::Tr("Vous ne pouvez pas ouvrir de salon ici.")),
             0);
    return;
  }
  if (title_len == 0) { SetErrorFromMsgString(kMsgEnterRoomTitle, 1); return; }
  if (ContainsBannedWord(title_)) { SetErrorFromMsgString(kMsgBadWord, 1); return; }
  if (ServiceType() == kServiceTypeIntl && !IsPureAscii7(title_)) {
    SetErrorFromMsgString(kMsgEnglishOnly, 1);
    return;
  }
  // Ce plafond-là, le natif le fait respecter par son champ de saisie (max 36) ;
  // nous laissons taper plus long pour pouvoir le MONTRER, donc c'est ici qu'il
  // se refuse. Au-delà, le serveur tronquerait sans rien dire.
  if (title_len > kTitleMaxBytes) {
    char msg[128];
    std::snprintf(msg, sizeof(msg), i18n::Tr("Titre trop long : %d octets pour %d au maximum."),
                  title_len, kTitleMaxBytes);
    SetError(msg, 1);
    return;
  }
  if (!is_public_ && pass_len < kPassMinBytes) {
    // Le natif choisit entre deux messages selon que le champ est vide ou non.
    SetErrorFromMsgString(pass_len == 0 ? kMsgEnterPassword : kMsgPasswordTooShort, 2);
    return;
  }
  if (pass_len > kPassMaxBytes) {
    char msg[128];
    std::snprintf(msg, sizeof(msg), i18n::Tr("Mot de passe trop long : %d octets pour %d au maximum."),
                  pass_len, kPassMaxBytes);
    SetError(msg, 2);
    return;
  }

  // ── L'envoi, par le chemin natif (paquet + miroir CGameMode + replay).
  if (!SendCreateChatRoom(title_, password_, is_public_,
                          std::clamp(limit_, kLimitMin, kLimitMax))) {
    SetError(i18n::Tr("Envoi impossible : pas de connexion."), 0);
    return;
  }

  // 🔴 On NE FERME PAS. C'est toute la différence avec le natif : la fenêtre reste
  // à l'écran, saisie intacte, jusqu'à `ZC_ACK_CREATE_CHATROOM`.
  waiting_ack_ = true;
  sent_tick_   = GetTickCount();
  error_[0]    = '\0';
  error_field_ = 0;
}

void ChatRoomWindow::OnRenderUI() {
  // La SALLE est indépendante du formulaire de création : les deux peuvent être
  // à l'écran en même temps (on crée un salon alors qu'on est déjà dans un autre
  // n'arrive pas, mais le formulaire peut rester ouvert sur une erreur pendant
  // que la salle s'ouvre).
  DrawRoom();

  DrawPasswordPrompt();

  if (!open_ || !imgui_enabled_) return;

  if (need_pos_) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    need_pos_ = false;
  }
  // Largeur imposée, hauteur auto (y <= 0 => auto-fit permanent sur cet axe). Le
  // tout à l'ÉCHELLE de l'interface : `ro::Px` est ce qui empêche une fenêtre
  // calée en pixels de rétrécir quand la police grandit.
  ImGui::SetNextWindowSize(ImVec2(ro::Px(330.0f), 0.0f), ImGuiCond_Always);

  // Le titre vient de la table du CLIENT (MSI 125), pas de notre catalogue : il
  // est donc déjà dans la langue du jeu et ne passe pas par `i18n::Tr`. Le suffixe
  // « ### » est ajouté à la main — il fixe l'identité ImGui de la fenêtre pour que
  // sa position survive à un changement de langue du client.
  char window_title[160];
  std::snprintf(window_title, sizeof(window_title), "%s###bourgeon_chatroom",
                msgstr::Utf8Or(kMsgMakingRoom,
                               i18n::Tr("Créer un salon de chat")));

  const bool begun = ro::BeginRoWindow(
      window_title, &show_panel_,
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
  if (!show_panel_) { Close(); show_panel_ = true; }
  if (!begun) { ro::EndRoWindow(); return; }

  const float label_col = ImGui::CalcTextSize(
      msgstr::Utf8Or(kMsgRoomPassLabel, "Mot de passe :")).x + ImGui::GetFontSize();

  // ── Titre ─────────────────────────────────────────────────────────────────
  ImGui::TextUnformatted(msgstr::Utf8Or(kMsgRoomTitleLabel, "Titre :"));
  ImGui::SameLine(label_col);
  ImGui::PushItemWidth(-1.0f);
  {
    const bool bad = (error_field_ == 1);
    if (bad) ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(120, 40, 40, 255));
    // Le tampon est en CP949 : c'est ce qui part sur le fil, donc c'est aussi ce
    // qu'on compte et ce qu'on valide. Entrée valide, comme sur la native.
    // Éditer le champ efface l'erreur qui le surligne : garder le cadre rouge
    // pendant que le joueur corrige donnerait l'impression que la correction ne
    // compte pas. Entrée valide, comme sur la fenêtre native.
    if (ro::InputTextCp949("##chatroom_titre", title_, sizeof(title_),
                           ImGuiInputTextFlags_EnterReturnsTrue))
      Send();
    else if (ImGui::IsItemEdited() && error_field_ == 1)
      SetError("", 0);
    if (bad) ImGui::PopStyleColor();
  }
  ImGui::PopItemWidth();

  // Compteur d'octets. Il n'est pas décoratif : en CP949 un caractère accentué ou
  // coréen coûte DEUX octets, et le plafond du serveur est en octets. Le natif ne
  // dit rien et laisse découvrir la troncature après coup.
  const int title_len = static_cast<int>(std::strlen(title_));
  const bool title_over = title_len > kTitleMaxBytes;
  // SetCursorPosX et NON SameLine : le compteur va SOUS le champ, aligné sur lui.
  // Un SameLine l'écrirait PAR-DESSUS la saisie (le champ occupe toute la largeur).
  ImGui::SetCursorPosX(label_col);
  if (title_over)
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%d/%d", title_len, kTitleMaxBytes);
  else
    ImGui::TextDisabled("%d/%d", title_len, kTitleMaxBytes);

  // L'ASCII imposé (g_ServiceType == 1) se dit PENDANT la frappe, pas après le
  // clic — c'est la seule façon de savoir quel caractère pose problème.
  const bool ascii_required = (ServiceType() == kServiceTypeIntl);
  const bool title_not_ascii = ascii_required && !IsPureAscii7(title_);
  if (title_not_ascii) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
                       i18n::Tr("(caractères non anglais)"));
    ImGui::SameLine();
    mui::HelpMarker(msgstr::Utf8Or(
        kMsgEnglishOnly, "Ce serveur n'accepte que les caractères anglais."));
  }

  mui::Spacing();

  // ── Limite ────────────────────────────────────────────────────────────────
  // Le natif offre SIX valeurs (2, 3, 5, 10, 15, 20) dans un déroulant haut de
  // 48 px, donc trois lignes visibles sur six : 7, 12 ou 18 places sont
  // impossibles alors que le serveur les accepte toutes jusqu'à 20.
  ImGui::TextUnformatted(msgstr::Utf8Or(kMsgRoomLimitLabel, "Limite :"));
  ImGui::SameLine(label_col);
  ImGui::PushItemWidth(-ImGui::GetFontSize() * 4.0f);
  // Format = une VALEUR courte : une phrase dans un slider écrase la piste.
  mui::WheelSliderInt("##chatroom_limite", &limit_, kLimitMin, kLimitMax, "%d");
  ImGui::PopItemWidth();
  ImGui::SameLine();
  ImGui::TextUnformatted(msgstr::Utf8Or(kMsgCountUnitPeople, "pers."));

  mui::Spacing();

  // ── Accès ─────────────────────────────────────────────────────────────────
  // Le déroulant « type » du natif est un leurre : un seul item, dont la valeur
  // ne part JAMAIS dans le paquet. Ce qui décide vraiment du type, c'est ce
  // couple public/privé — et c'est lui qui commande le mot de passe.
  ImGui::TextUnformatted(i18n::Tr("Accès"));
  ImGui::SameLine(label_col);
  if (ImGui::RadioButton(msgstr::Utf8Or(kMsgRoomPublic, "Public"), is_public_))
    is_public_ = true;
  ImGui::SameLine();
  if (ImGui::RadioButton(msgstr::Utf8Or(kMsgRoomPrivate, "Privé"), !is_public_))
    is_public_ = false;

  // ── Mot de passe ──────────────────────────────────────────────────────────
  // Grisé sur un salon public : le natif laisse le champ actif alors que sa
  // valeur ne sert à rien, ce qui invite à le remplir pour rien.
  ImGui::BeginDisabled(is_public_);
  ImGui::TextUnformatted(msgstr::Utf8Or(kMsgRoomPassLabel, "Mot de passe :"));
  ImGui::SameLine(label_col);
  ImGui::PushItemWidth(-(ImGui::GetFontSize() * 2.2f));
  {
    const bool bad = (error_field_ == 2);
    if (bad) ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(120, 40, 40, 255));
    const int flags = show_password_ ? 0 : ImGuiInputTextFlags_Password;
    ro::InputTextCp949("##chatroom_passe", password_, sizeof(password_), flags);
    if (ImGui::IsItemEdited() && error_field_ == 2) SetError("", 0);
    if (bad) ImGui::PopStyleColor();
  }
  ImGui::PopItemWidth();
  ImGui::SameLine();
  // L'œil : le natif masque avec des étoiles et ne permet JAMAIS de relire ce
  // qu'on vient de taper.
  if (ro::RoSmallButton(show_password_ ? "*##chatroom_oeil" : "o##chatroom_oeil"))
    show_password_ = !show_password_;
  mui::Tooltip(i18n::Tr("Afficher / masquer le mot de passe"));

  // La règle des 4 caractères, dite AVANT le clic et non après.
  const int pass_len = static_cast<int>(std::strlen(password_));
  if (!is_public_) {
    ImGui::SetCursorPosX(label_col);  // sous le champ, comme le compteur du titre
    if (pass_len < kPassMinBytes)
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s",
                         i18n::Tr("4 à 8 caractères"));
    else
      ImGui::TextDisabled("%d/%d", pass_len, kPassMaxBytes);
  }
  ImGui::EndDisabled();

  // ── Message d'erreur, DANS la fenêtre ─────────────────────────────────────
  if (error_[0]) {
    mui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
    mui::TextWrapped(error_);
    ImGui::PopStyleColor();
  }

  mui::Spacing();
  mui::Separator();

  // ── Boutons ───────────────────────────────────────────────────────────────
  const bool can_send =
      !waiting_ack_ && title_len > 0 && !title_over && !title_not_ascii &&
      (is_public_ || (pass_len >= kPassMinBytes && pass_len <= kPassMaxBytes));

  // « Envoi... » entre dans le calcul : c'est le libellé que porte le bouton
  // pendant l'attente de l'acquittement, et il est plus large que « Créer ».
  const float bw = ro::MaxButtonWidth({i18n::Tr("Créer"), i18n::Tr("Annuler"),
                                       i18n::Tr("Envoi..."),
                                       i18n::Tr("Appliquer")});
  const float total = bw * 2.0f + ImGui::GetStyle().ItemSpacing.x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                       (std::max)(0.0f, ImGui::GetContentRegionAvail().x - total));

  ImGui::BeginDisabled(!can_send);
  if (ro::RoButton(waiting_ack_ ? i18n::Tr("Envoi...") : i18n::Tr("Créer"), bw))
    Send();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Annuler"), bw)) Close();

  ro::EndRoWindow();
}

// ═════════════════════════════════════════════════════════════════════════════
// LA SALLE — remplacement de `UIChatRoomWnd` (id 28 / 0x1C)
// RE : docs/chat_room_re.md §10 bis.
// ═════════════════════════════════════════════════════════════════════════════

namespace {


// Le nom du personnage courant, en UTF-8. `g_Own_CharName_Plain` est un char[]
// déjà déballé par le client (le même que lit char_diagnostics) : pas d'appel
// natif, donc utilisable depuis n'importe quel fil.
// 🔴 `LocalToUtf8` et NON `Cp949ToUtf8`. Une ligne de salon est composée par le
// CLIENT dans SA code-page (1252 en servicetype européen, pas 949), et elle peut
// porter un msgstring TRADUIT — « (%s) est entré. ». Passée par Cp949ToUtf8 elle
// sortait « (Gettar) est entr? » en jeu. Règle du projet, cf.
// project_utf8_emoji_support.
// Conversion vers UTF-8 dans un tampon À NOUS. Le tampon de `ro::`
// est thread-local et rotatif : on recopie tout de suite. POD only — c'est ce
// qui permet le `__try` (cf. C2712).
bool SafeLocalToUtf8(const char* local, char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = 0;
  __try {
    const char* utf8 = ro::LocalToUtf8(local);
    if (!utf8) return false;
    size_t n = 0;
    for (; n + 1 < out_size && utf8[n]; ++n) out[n] = utf8[n];
    out[n] = 0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool OwnCharName(char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  __try {
    const char* src = reinterpret_cast<const char*>(rag::kOwnCharNameAddr);
    if (src[0] == '\0') return false;
    // Le nom vient de la MÉMOIRE du client : sa code-page, donc.
    const char* utf8 = ro::LocalToUtf8(src);
    if (!utf8) return false;
    size_t n = 0;
    for (; n + 1 < out_size && utf8[n]; ++n) out[n] = utf8[n];
    out[n] = '\0';
    return n > 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Nom d'un membre lu dans un paquet : 24 octets, PAS forcément terminés.
std::string MemberNameUtf8(const uint8_t* raw) {
  char name[25];
  std::memcpy(name, raw, 24);
  name[24] = 0;
  // ⚠ Code-page du FIL ici, pas celle du client : les deux diffèrent (ro_imgui.h).
  const char* utf8 = ro::WireToUtf8(name);
  return std::string(utf8 ? utf8 : "");
}

}  // namespace

// ── Ingestion des lignes ─────────────────────────────────────────────────────

bool chatroomwnd::IngestRoomLine(const char* cp949, uint32_t rgb) {
  if (g_chat_room_window == nullptr) return false;
  return g_chat_room_window->IngestRoomLine(cp949, rgb);
}

bool chatroomwnd::RoomOwnsChatInput() {
  return g_chat_room_window != nullptr && g_chat_room_window->OwnsChatInput();
}

void chatroomwnd::FocusRoom() {
  if (g_chat_room_window != nullptr) g_chat_room_window->FocusRoom();
}

bool chatroomwnd::ClaimPublicChatLine(const char* cp949, uint32_t rgb, int type) {
  if (g_chat_room_window == nullptr) return false;
  return g_chat_room_window->ClaimPublicChatLine(cp949, rgb, type);
}

bool ChatRoomWindow::ClaimPublicChatLine(const char* cp949, uint32_t rgb,
                                        int type) {
  if (!imgui_enabled_ || !room_open_) return false;
  // Type 1 = « Public Chat », le seul que le client aiguille vers le salon quand
  // sa fenêtre 28 existe. Messages système, annonces et compagnie gardent leur
  // chemin vers la chatbox — le natif fait pareil.
  if (type != 1) return false;
  (void)rgb;  // la couleur du chat public serait illisible sur fond clair
  return IngestRoomLine(cp949, kRoomLineRgb);
}

bool ChatRoomWindow::IngestRoomLine(const char* cp949, uint32_t rgb) {
  // Interrupteur éteint : la fenêtre native vit encore et c'est ELLE qui doit
  // recevoir la ligne. Ne rien prendre, ne rien avaler.
  if (!imgui_enabled_) return false;
  if (cp949 == nullptr) return false;

  // 🔴 La ligne part telle quelle chez `ChatWindow` : c'est SON parseur qui
  // convertit l'encodage et découpe le balisage, exactement comme pour le journal.
  // Rien à convertir ni à mémoriser ici — et surtout pas un second rendu.
  //
  // Couleur 0 = le défaut du SALON, `0x222222`, celle que le client lui-même passe
  // à `ChatAction(5, …)`. Sombre, parce que le corps d'une fenêtre RO est CLAIR.
  //
  // ⚠ Fil RÉSEAU possible (les handlers ZC appellent ChatAction directement) —
  // l'ingestion de ChatWindow prend son verrou, comme pour ses autres sources.
  return chatwnd::IngestChatRoomLine(cp949,
                                     (rgb != 0) ? (rgb & 0xFFFFFFu) : kRoomLineRgb);
}

// ── Cycle de vie de la salle ─────────────────────────────────────────────────

void ChatRoomWindow::HandleNativeRoomCreation(void* win) {
  if (!imgui_enabled_) return;
  // Masquer avant la première frame ; OnTick détruit. Comme la 27, cette fenêtre
  // déclare un bouton par défaut (`this[35] = 184`) — ici Entrée ENVOIE le
  // message. Invisible mais vivante, elle enverrait à notre place.
  uiwnd::SafeSetVisible(win, false);

  // 🔴 ET RIEN DE PLUS. Ce hook tourne sur le FIL RÉSEAU : ce sont les handlers
  // ZC (acquittement de création, `ZC_ENTER_ROOM`, `ZC_MEMBER_NEWENTRY`) qui
  // fabriquent cette fenêtre, jamais une action du joueur. Y toucher à l'état de
  // la salle courserait avec le rendu. C'est donc `HandlePacket` — fil principal,
  // via net_inbox_ — qui ouvre et remplit la salle.
}

// Ouvre la salle pour CELUI QUI VIENT DE LA CRÉER. Le propriétaire ne reçoit ni
// `ZC_ROOM_NEWENTRY` (AREA_WOSC exclut la source) ni `ZC_ENTER_ROOM` : sa seule
// source est le bloc que `SendMsg(43)` vient d'écrire dans le CGameMode, et son
// propre nom, que le natif ajoute par un `msg 44` sur une fenêtre qui n'existe
// plus chez nous.
void ChatRoomWindow::OpenRoomAsOwner() {
  RoomJoined();
  const ModeRoomInfo info = ReadModeRoomInfo();
  if (info.valid) {
    char t[128];
    SafeLocalToUtf8(info.title, t, sizeof(t));
    room_title_  = t;
    room_public_ = (info.is_public != 0);
    room_limit_  = info.limit;
  }
  // `CGameMode+0x3FC` ne passera à 1 qu'APRÈS notre passage : on sait de toute
  // façon qu'on est seul dans un salon qu'on vient de créer.
  room_users_      = 1;
  room_i_am_owner_ = true;
  // Le bloc du CGameMode est à jour ICI, et seulement ici : `SendMsg(43)` vient
  // de l'écrire. On en prend copie une fois pour toutes.
  char pass[24];
  ReadModeRoomPassword(pass, sizeof(pass));
  room_password_wire_ = pass;
  char mine[32];
  if (OwnCharName(mine, sizeof(mine))) {
    RoomMember m;
    m.name  = mine;
    m.owner = true;
    room_members_.push_back(std::move(m));
  }
}

void ChatRoomWindow::HandleNativeChangeCreation(void* win) {
  if (!imgui_enabled_) return;
  // Masquer avant la première frame ; OnTick détruit. Elle partage l'`OnCreate`
  // de la 27, donc son bouton par défaut ENVOIE aussi (commande 184).
  uiwnd::SafeSetVisible(win, false);
}

void ChatRoomWindow::HandleNativePasswordCreation(void* win) {
  if (!imgui_enabled_) return;
  uiwnd::SafeSetVisible(win, false);
  // L'identifiant du salon n'est PAS encore posé : son ouvreur le pousse par un
  // `msg 47` juste après ce retour. On relit donc `+0xBC` à la frame suivante.
  pw_native_wait_ = true;
}

void ChatRoomWindow::RoomJoined() {
  room_open_       = true;
  room_show_panel_ = true;
  room_need_pos_   = true;
  room_i_am_owner_ = false;
  room_members_.clear();
  room_context_member_.clear();
  // Un salon n'est pas une conversation qui se poursuit : retrouver les lignes du
  // précédent en ouvrant le suivant n'aurait aucun sens.
  chatwnd::ClearChatRoomLog();
}

void ChatRoomWindow::CloseRoom() {
  room_open_ = false;
  room_members_.clear();
  room_context_member_.clear();
  chatwnd::ClearChatRoomLog();
}

// ── Les appels natifs ARMÉS pendant la frame, joués hors frame ───────────────

void ChatRoomWindow::FlushPending() {
  const Pending action = pending_;
  if (action == Pending::kNone) return;
  pending_ = Pending::kNone;

  switch (action) {
    case Pending::kLeave: {
      // 🔴 ICI, et SEULEMENT ici, le paquet brut est le bon choix — l'inverse de
      // la création. `CMode::SendMsg(48)` (bloc 0x00C8C2D1) ne fait que DEUX
      // choses : `if (g_UIChatRoomWnd_Slot == 0) return;` puis émettre un paquet
      // de 2 octets sans charge utile. Comme nous DÉTRUISONS la fenêtre 28, ce
      // slot vaut toujours 0 et le client refusait la commande en silence : ni
      // « Quitter » ni la croix ne fermaient quoi que ce soit.
      //
      // ➡ La règle complète, dont §12.3 ne donnait qu'une moitié : regarder ce
      // que le chemin natif fait EN PLUS d'émettre. Si c'est un état à tenir à
      // jour (création), il faut y passer ; si ce n'est qu'une garde portant sur
      // une fenêtre qu'on a supprimée, il faut s'en passer.
      const uint16_t exit_room = 0x00E3;  // CZ_EXIT_ROOM, opcode seul
      Bourgeon::Instance().SendPacket(
          reinterpret_cast<const uint8_t*>(&exit_room), sizeof(exit_room));
      // Fermeture IMMÉDIATE, sans attendre l'acquittement : quitter ne peut pas
      // échouer, et le natif détruit lui aussi sa fenêtre sur-le-champ. Attendre
      // `ZC_MEMBER_EXIT` laisserait la salle à l'écran si le serveur ne renvoie
      // pas la sortie à celui qui part.
      CloseRoom();
      break;
    }
    case Pending::kExpel:
      rag::SendToActiveMode(kModeMsgExpel, static_cast<int>(reinterpret_cast<intptr_t>(
                                     pending_member_.c_str())));
      pending_member_.clear();
      break;
    case Pending::kGiveOwner:
      // p1 = nom, p2 = rôle. 0 = « devient propriétaire ».
      rag::SendToActiveMode(kModeMsgGiveOwner,
                  static_cast<int>(reinterpret_cast<intptr_t>(
                      pending_member_.c_str())),
                  0);
      pending_member_.clear();
      break;
    case Pending::kEnterRoom:
      // p1 = identifiant du salon, p2 = mot de passe (C-string).
      rag::SendToActiveMode(kModeMsgEnterRoom, static_cast<int>(pw_chat_id_),
                  static_cast<int>(reinterpret_cast<intptr_t>(
                      pending_text_.c_str())));
      pending_text_.clear();
      break;
    case Pending::kApplySettings:
      if (SendChangeChatRoom(set_title_, set_password_, set_public_,
                             std::clamp(set_limit_, kLimitMin, kLimitMax))) {
        // Le paquet brut ne met pas à jour le bloc du CGameMode : c'est donc à
        // nous de retenir le nouveau mot de passe, sans quoi le volet rouvert
        // afficherait l'ancien (le serveur ne le renvoie jamais).
        room_password_wire_ = set_public_ ? "" : set_password_;
      }
      break;
    default:
      break;
  }
}

// ── Le flux serveur ──────────────────────────────────────────────────────────
// Tout est OBSERVÉ : les handlers natifs gardent leurs devoirs (lignes de chat
// « (X) est entré », globales, fermeture de session). On ne lit que les champs
// dont la fenêtre a besoin, sur le FIL PRINCIPAL (via net_inbox_).

void ChatRoomWindow::HandleRoomPacket(uint16_t opcode, const uint8_t* data,
                                      uint16_t len) {
  switch (opcode) {
    case kOpEnterRoom: {
      // { W size ; L chatId ; { L flag ; char name[24] } * n }  — `data` commence
      // APRÈS l'opcode, donc la taille annoncée est le premier champ.
      // flag 0 = propriétaire, 1 = membre ordinaire (clif_joinchatok).
      if (len < 6) return;
      const uint16_t size = *reinterpret_cast<const uint16_t*>(data);
      if (size < 8) return;
      // La taille annoncée fait foi ; `len` n'est que ce qu'on a demandé à
      // recopier. Prendre le plus petit des deux, sinon on lirait le tampon du
      // client au-delà du paquet.
      const uint16_t usable = (size < len) ? size : len;
      const int n = (usable < 8) ? 0 : (usable - 8) / 28;
      RoomJoined();
      pw_open_ = false;  // on est entré : l'invite n'a plus lieu d'être
      // 🔴 Ce paquet ne porte NI titre, NI limite, NI public/privé — seulement
      // l'identifiant et les membres. Le natif va les relire dans l'objet du
      // panneau planté au-dessus de la tête du propriétaire (`acteur+0x268`,
      // info à `+0x80`) ; nous les avons déjà, tenus depuis `ZC_ROOM_NEWENTRY`.
      const uint32_t chat_id = *reinterpret_cast<const uint32_t*>(data + 2);
      for (const NearbyRoom& r : nearby_rooms_) {
        if (r.id != chat_id) continue;
        room_title_  = r.title;
        room_limit_  = r.limit;
        room_public_ = r.is_public;
        break;
      }
      char mine[32] = {0};
      const bool have_mine = OwnCharName(mine, sizeof(mine));
      for (int i = 0; i < n; ++i) {
        const uint8_t* entry = data + 6 + i * 28;
        if (static_cast<uint16_t>(6 + i * 28 + 28) > usable) break;
        RoomMember m;
        m.owner = (*reinterpret_cast<const uint32_t*>(entry) == 0);
        m.name  = MemberNameUtf8(entry + 4);
        if (m.owner && have_mine && m.name == mine) room_i_am_owner_ = true;
        room_members_.push_back(std::move(m));
      }
      room_users_ = static_cast<int>(room_members_.size());
      return;
    }
    case kOpMemberNew: {
      // { W count ; char name[24] }
      if (len < 26) return;
      room_users_ = *reinterpret_cast<const uint16_t*>(data);
      RoomMember m;
      m.owner = false;
      m.name  = MemberNameUtf8(data + 2);
      // Le propriétaire reçoit aussi son PROPRE nom par ce chemin à la création
      // (l'ACK envoie msg 44 avec Own_GetCharName) — ne pas le doubler.
      for (const RoomMember& e : room_members_)
        if (e.name == m.name) return;
      room_members_.push_back(std::move(m));
      return;
    }
    case kOpMemberExit: {
      // { W count ; char name[24] ; B kicked }
      if (len < 26) return;
      room_users_ = *reinterpret_cast<const uint16_t*>(data);
      const std::string gone = MemberNameUtf8(data + 2);
      char mine[32] = {0};
      if (OwnCharName(mine, sizeof(mine)) && gone == mine) {
        // C'est MOI qui pars (ou qui suis expulsé) : la salle se ferme. Le natif
        // fait de même en détruisant sa fenêtre.
        CloseRoom();
        return;
      }
      for (size_t i = 0; i < room_members_.size(); ++i) {
        if (room_members_[i].name == gone) {
          room_members_.erase(room_members_.begin() + static_cast<long>(i));
          break;
        }
      }
      return;
    }
    case kOpRefuseEnter: {
      // { B raison } — cf. `e_refuse_enter_room`. Le natif écrit le motif en chat
      // et ferme sa fenêtre ; nous gardons l'invite OUVERTE avec le motif dedans,
      // pour que corriger un mot de passe ne demande pas de recliquer le panneau.
      if (len < 1 || !pw_open_) return;
      int msg = 0;
      switch (data[0]) {
        case 0: msg = kMsgRefuseFull;      break;
        case 1: msg = kMsgRefuseWrongPass; break;
        case 2: msg = kMsgRefuseKicked;    break;
        case 4: msg = kMsgRefuseNoZeny;    break;
        case 5: msg = kMsgRefuseLowLevel;  break;
        case 6: msg = kMsgRefuseHighLevel; break;
        case 7: msg = kMsgRefuseJob;       break;
        default: return;  // 3 = succès, il ne passe pas par ce paquet
      }
      std::snprintf(pw_error_, sizeof(pw_error_), "%s",
                    msgstr::Utf8Or(msg, i18n::Tr("Le serveur a refusé la demande.")));
      return;
    }
    case kOpRoleChange: {
      // { L flag ; char name[24] } — flag 0 = devient propriétaire.
      if (len < 28) return;
      const bool becomes_owner = (*reinterpret_cast<const uint32_t*>(data) == 0);
      const std::string who = MemberNameUtf8(data + 4);
      char mine[32] = {0};
      const bool have_mine = OwnCharName(mine, sizeof(mine));
      for (RoomMember& m : room_members_)
        if (m.name == who) m.owner = becomes_owner;
      if (have_mine && who == mine) room_i_am_owner_ = becomes_owner;
      return;
    }
    case kOpChangeRoom: {
      // Même forme que ZC_ROOM_NEWENTRY :
      // { W size ; L owner ; L id ; W limit ; W users ; B type ; char title[] }
      if (len < 17) return;
      const uint16_t size = *reinterpret_cast<const uint16_t*>(data);
      room_limit_  = *reinterpret_cast<const uint16_t*>(data + 10);
      room_users_  = *reinterpret_cast<const uint16_t*>(data + 12);
      room_public_ = (data[14] != 0);
      const int title_len = static_cast<int>(size) - 17;
      if (title_len > 0 && static_cast<uint16_t>(15 + title_len) <= len) {
        char title[64] = {0};
        const int n = title_len < 63 ? title_len : 63;
        std::memcpy(title, data + 15, static_cast<size_t>(n));
        title[n] = 0;
        const char* t = ro::WireToUtf8(title);
        room_title_ = t ? t : "";
      }
      return;
    }
    case kOpRoomNewEntry: {
      // { W size ; L owner ; L id ; W limit ; W users ; B type ; char title[] }
      // Le panneau d'un salon VISIBLE — le nôtre excepté (AREA_WOSC).
      // `type` vient de `clif_chat_status` : 0 = privé, 1 = public, 2/3 = PNJ.
      if (len < 17) return;
      const uint16_t size = *reinterpret_cast<const uint16_t*>(data);
      const uint16_t usable = (size < len) ? size : len;
      if (usable < 17) return;
      NearbyRoom r;
      r.id        = *reinterpret_cast<const uint32_t*>(data + 6);
      r.limit     = *reinterpret_cast<const uint16_t*>(data + 10);
      r.users     = *reinterpret_cast<const uint16_t*>(data + 12);
      r.is_public = (data[14] == 1);
      const int title_len = static_cast<int>(usable) - 17;
      if (title_len > 0) {
        char title[64];
        const int k = title_len < 63 ? title_len : 63;
        std::memcpy(title, data + 15, static_cast<size_t>(k));
        title[k] = 0;
        const char* t = ro::WireToUtf8(title);
        r.title = t ? t : "";
      }
      for (NearbyRoom& e : nearby_rooms_) {
        if (e.id != r.id) continue;
        e = r;
        return;
      }
      nearby_rooms_.push_back(std::move(r));
      return;
    }
    case kOpDestroyRoom: {
      // Le panneau disparaît : radier du registre. ⚠ Ce paquet parle des salons
      // des AUTRES aussi — il ne dit rien de notre appartenance, et c'est
      // `ZC_MEMBER_EXIT` sur notre propre nom qui nous sort.
      if (len >= 4) {
        const uint32_t gone = *reinterpret_cast<const uint32_t*>(data);
        for (size_t i = 0; i < nearby_rooms_.size(); ++i) {
          if (nearby_rooms_[i].id != gone) continue;
          nearby_rooms_.erase(nearby_rooms_.begin() + static_cast<long>(i));
          break;
        }
      }
      return;
    }
    default:
      return;
  }
}

// ── Rendu de la salle ────────────────────────────────────────────────────────

void ChatRoomWindow::DrawRoom() {
  if (!room_open_ || !imgui_enabled_) return;

  if (room_need_pos_) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->GetCenter().x, vp->GetCenter().y + ro::Px(60.0f)),
        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ro::Px(460.0f), ro::Px(280.0f)),
                             ImGuiCond_FirstUseEver);
    room_need_pos_ = false;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(ro::Px(300.0f), ro::Px(160.0f)),
                                      ImVec2(FLT_MAX, FLT_MAX));
  if (room_focus_request_) {
    ImGui::SetNextWindowFocus();
    room_focus_request_ = false;
  }
  // La bascule du volet a eu lieu à la frame précédente : la fenêtre grandit (ou
  // rétrécit) MAINTENANT, avant que le volet ne soit dessiné plus bas. C'est cet
  // ordre-là qui supprime le scintillement de barre de défilement.
  const bool settings_wanted = room_show_settings_ && room_i_am_owner_;
  if (settings_wanted != room_settings_shown_) {
    if (room_last_h_ > 0.0f) {
      const float delta =
          settings_wanted ? SettingsPaneHeight() : -SettingsPaneHeight();
      ImGui::SetNextWindowSize(
          ImVec2(0.0f, (std::max)(ro::Px(160.0f), room_last_h_ + delta)),
          ImGuiCond_Always);
    }
    room_settings_shown_ = settings_wanted;
  }

  // La barre de titre du natif, à l'identique : « Publ. : <titre> (2/20) »
  // (gabarit "%s%s (%d/%d)" @0x010317D4, MSI 89/90 pour le préfixe).
  char title[256];
  std::snprintf(title, sizeof(title), "%s%s (%d/%d)###bourgeon_chatroom_salle",
                msgstr::Utf8Or(room_public_ ? kMsgRoomPublicShort
                                            : kMsgRoomPrivateShort,
                               room_public_ ? "Publ. : " : "Priv. : "),
                room_title_.c_str(), room_users_, room_limit_);

  // 🔴 HORS de la pile Échap. Fermer cette fenêtre, c'est QUITTER le salon (sa
  // croix est le `/q` du natif) : la laisser dans la pile faisait sortir du salon
  // à chaque Échap destiné au menu du jeu, sans un mot. C'est le cas d'école que
  // décrit `SkipNextEscapeWindow` — une fenêtre qu'on garde ouverte en jouant.
  // Elle n'avale donc pas la touche non plus : Échap va bien au menu.
  ro::SkipNextEscapeWindow();
  // Pas de bouton « réduire » : une salle repliée en barre de titre continue de
  // recevoir des messages sans les montrer, alors que le joueur y reste occupant.
  const bool begun =
      ro::BeginRoWindow(title, &room_show_panel_, ImGuiWindowFlags_NoCollapse);
  if (!room_show_panel_) {
    // La croix de la native est libellée « /q » : elle QUITTE le salon, elle ne
    // masque pas une fenêtre. On fait pareil — sinon le joueur croirait être
    // sorti tout en restant occupant aux yeux du serveur.
    pending_ = Pending::kLeave;
    room_show_panel_ = true;
  }
  if (!begun) { ro::EndRoWindow(); return; }

  // ── Les deux volets ────────────────────────────────────────────────────────
  // Le natif donne 20 % de la largeur à la liste des membres (rapport 56/280) ;
  // on garde la proportion, avec un plancher pour qu'un nom tienne toujours.
  // 🔴 Le volet « Réglages » se RÉSERVE sa place avant que les deux listes ne se
  // partagent le reste. Faire grandir la fenêtre ne suffisait pas : les enfants
  // sont dimensionnés sur `GetContentRegionAvail()`, donc ils absorbaient toute
  // la hauteur gagnée et le volet restait sous le bord, hors d'atteinte sans
  // faire défiler.
  // `room_settings_shown_`, PAS `room_show_settings_` : la réservation et le
  // dessin doivent parler du même état, celui de la frame où la fenêtre a déjà la
  // bonne taille.
  const float settings_h =
      room_settings_shown_ ? SettingsPaneHeight() : 0.0f;
  // Le pied : la barre de saisie de la chatbox, puis la rangée de boutons. La
  // barre GRANDIT avec la phrase (cf. ChatWindow::InputRowHeight) : c'est elle
  // qui dit sa hauteur, plafonnée pour laisser aux deux volets trois rangées.
  const float spacing_y = ImGui::GetStyle().ItemSpacing.y;
  const float buttons_h = ImGui::GetFrameHeightWithSpacing();
  const float input_max = ImGui::GetContentRegionAvail().y - settings_h - buttons_h -
                          ImGui::GetFrameHeightWithSpacing() * 3.0f;
  const float footer_h =
      chatwnd::ChatInputRowHeight(input_max) + spacing_y + buttons_h + spacing_y;
  // Plancher : le joueur peut rétrécir la fenêtre sous la somme des trois.
  const float avail_h = (std::max)(
      ImGui::GetFrameHeightWithSpacing(),
      ImGui::GetContentRegionAvail().y - footer_h - settings_h);
  const float total_w  = ImGui::GetContentRegionAvail().x;
  const float members_w =
      (std::max)(ro::Px(96.0f), total_w * 0.2f);
  const float lines_w = total_w - members_w - ImGui::GetStyle().ItemSpacing.x;

  // Volet des messages — dessiné par `ChatWindow`, avec le MÊME rendu que le
  // journal : balises, liens d'objets et de monstres, emotes du jeu et de
  // Discord, icônes `^i[]`, gras/italique, couleurs `^RRGGBB`, et les clics qui
  // vont avec. Le collage au bas est fait là-bas aussi.
  // 🔴 `nullptr` : les liens Maj+cliqués repartent vers la barre principale — qui
  // est justement dessinée au bas de CETTE fenêtre. La redirection n'a plus lieu
  // d'être, elle ne servait qu'à alimenter la saisie propre au salon.
  if (ImGui::BeginChild("##salle_lignes", ImVec2(lines_w, avail_h), true))
    chatwnd::DrawChatRoomLog(nullptr);
  ImGui::EndChild();

  ImGui::SameLine();

  // Volet des membres.
  if (ImGui::BeginChild("##salle_membres", ImVec2(members_w, avail_h), true)) {
    char mine_sel[32] = {0};
    const bool have_mine_sel = OwnCharName(mine_sel, sizeof(mine_sel));
    for (const RoomMember& m : room_members_) {
      // Sélectionnable = « à qui je chuchote ». Soi-même exclu, comme chez le
      // natif — s'écrire à soi n'a pas de sens et le client le refuse.
      // 🔴 Le geste du natif, rendu VISIBLE. Sélectionner un membre y détournait
      // la saisie en chuchotement sans rien afficher (`0x008822AF`) : ici le nom
      // va dans la box destinataire de la barre, sous les yeux du joueur, et s'en
      // retire comme n'importe quel destinataire.
      const bool is_me = have_mine_sel && m.name == mine_sel;
      if (!is_me) {
        if (ImGui::Selectable(("##sel_" + m.name).c_str(), false,
                              ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0.0f, ImGui::GetTextLineHeight())))
          chatwnd::TargetWhisper(ro::Utf8ToWire(m.name.c_str()));
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip(i18n::Tr("Chuchoter à %s."), m.name.c_str());
        ImGui::SameLine(0.0f, 0.0f);
      }
      // Le propriétaire est marqué. Pas de glyphe hors ASCII : la police de
      // l'interface ne garantit rien au-delà de U+00FF.
      if (m.owner)
        // Ambre SOMBRE : le corps d'une fenêtre RO est clair, et l'or pâle qui
        // était ici s'y effaçait. Même raison que la couleur des lignes.
        ImGui::TextColored(ImVec4(0.62f, 0.40f, 0.0f, 1.0f), "* %s",
                           m.name.c_str());
      else
        ImGui::TextUnformatted(m.name.c_str());
      // Clic droit : le menu du propriétaire. Le natif n'en offre aucun — il
      // faut y passer par la fenêtre de réglages.
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && room_i_am_owner_) {
        char mine[32] = {0};
        if (!(OwnCharName(mine, sizeof(mine)) && m.name == mine)) {
          room_context_member_ = m.name;
          ImGui::OpenPopup("##salle_menu_membre");
        }
      }
    }
    if (ImGui::BeginPopup("##salle_menu_membre")) {
      ImGui::TextDisabled("%s", room_context_member_.c_str());
      mui::Separator();
      if (ImGui::MenuItem(i18n::Tr("Expulser"))) {
        pending_        = Pending::kExpel;
        pending_member_ = ro::Utf8ToWire(room_context_member_.c_str());
      }
      if (ImGui::MenuItem(msgstr::Utf8Or(kMsgGiveRoomPower,
                                         i18n::Tr("Céder les droits de chef")))) {
        pending_        = Pending::kGiveOwner;
        pending_member_ = ro::Utf8ToWire(room_context_member_.c_str());
      }
      ImGui::EndPopup();
    }
  }
  ImGui::EndChild();

  // ── Saisie ─────────────────────────────────────────────────────────────────
  // 🔴 LA barre de la chatbox, pas une copie. Dans un salon, tout ce qu'on écrit
  // y part déjà (le serveur route sur `sd->chatID`) : en tenir une seconde ici
  // aurait dédoublé la box destinataire, le sélecteur de mode, les emotes, les
  // chips de liens et l'historique — et le joueur aurait eu deux champs faisant
  // la même chose, dans deux fenêtres. La chatbox, elle, ne la dessine plus tant
  // que nous l'avons (cf. `ChatWindow::DrawsInputRowHere`).
  if (!chatwnd::DrawChatInputRow()) {
    // Chatbox moderne éteinte : le log de ce salon ne s'affiche pas non plus (il
    // vient d'elle). Le dire, plutôt que de laisser une fenêtre muette.
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
                       i18n::Tr("Active la chatbox moderne pour écrire ici."));
  }

  // ── Boutons ────────────────────────────────────────────────────────────────
  const float bw = ro::MaxButtonWidth(
      {i18n::Tr("Quitter"), msgstr::Utf8Or(kMsgChangeRoomSetting, "Réglages")});
  if (ro::RoButton(i18n::Tr("Quitter"), bw)) pending_ = Pending::kLeave;
  if (room_i_am_owner_) {
    ImGui::SameLine();
    // BASCULE d'un volet interne, pas l'ouverture d'une fenêtre détachée : le
    // natif en faisait une fenêtre à part (la 30) faute de mieux, mais des
    // réglages qui appartiennent à CETTE salle n'ont rien à faire ailleurs.
    if (ro::RoToggleButton(msgstr::Utf8Or(kMsgChangeRoomSetting, "Réglages"),
                           room_show_settings_, bw)) {
      room_show_settings_ = !room_show_settings_;
      if (room_show_settings_) LoadSettingsPane();
      // Rien de plus ici : c'est le HAUT de cette fonction qui redimensionnera à
      // la frame suivante, puis laissera le volet apparaître.
    }
  }

  if (room_settings_shown_) DrawSettingsPane();

  room_last_h_ = ImGui::GetWindowSize().y;
  ro::EndRoWindow();
}

// ── L'invite de mot de passe ─────────────────────────────────────────────────
// Remplace `UIPasswordWnd` (id 29). Le natif tient sur une ligne, tronque son
// propre libellé (« Veuillez saisir le mot de pa… ») et se FERME sur un refus,
// obligeant à recliquer le panneau pour retenter.

void ChatRoomWindow::DrawPasswordPrompt() {
  if (!pw_open_ || !imgui_enabled_) return;

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(ro::Px(300.0f), 0.0f), ImGuiCond_Always);

  char title[160];
  std::snprintf(title, sizeof(title), "%s###bourgeon_chatroom_passe",
                msgstr::Utf8Or(kMsgRoomPassLabel, i18n::Tr("Mot de passe :")));
  const bool begun = ro::BeginRoWindow(
      title, &pw_show_panel_,
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
  if (!pw_show_panel_) { pw_open_ = false; pw_show_panel_ = true; }
  if (!begun) { ro::EndRoWindow(); return; }

  // Le libellé du natif, en entier cette fois.
  mui::TextWrapped(msgstr::Utf8Or(kMsgEnterPasswordPrompt,
                                  i18n::Tr("Veuillez saisir le mot de passe.")));
  mui::Spacing();

  ImGui::PushItemWidth(-(ImGui::GetFontSize() * 2.2f));
  const int flags = (pw_reveal_ ? 0 : ImGuiInputTextFlags_Password) |
                    ImGuiInputTextFlags_EnterReturnsTrue;
  const bool submitted =
      ro::InputTextCp949("##chatroom_pw", pw_input_, sizeof(pw_input_), flags);
  ImGui::PopItemWidth();
  ImGui::SameLine();
  if (ro::RoSmallButton(pw_reveal_ ? "*##chatroom_pw_oeil" : "o##chatroom_pw_oeil"))
    pw_reveal_ = !pw_reveal_;
  mui::Tooltip(i18n::Tr("Afficher / masquer le mot de passe"));

  const int len = static_cast<int>(std::strlen(pw_input_));
  if (len > kPassMaxBytes)
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%d/%d", len, kPassMaxBytes);
  else
    ImGui::TextDisabled("%d/%d", len, kPassMaxBytes);

  if (pw_error_[0]) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.15f, 0.15f, 1.0f));
    mui::TextWrapped(pw_error_);
    ImGui::PopStyleColor();
  }

  mui::Separator();
  const float bw =
      ro::MaxButtonWidth({i18n::Tr("Entrer"), i18n::Tr("Annuler")});
  const bool can_send = len > 0 && len <= kPassMaxBytes;
  ImGui::BeginDisabled(!can_send);
  const bool go = ro::RoButton(i18n::Tr("Entrer"), bw);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Annuler"), bw)) { pw_open_ = false; }

  if ((go || submitted) && can_send) {
    // ARMER : `SendMsg(45)` est un appel natif, proscrit pendant une frame ImGui.
    pending_      = Pending::kEnterRoom;
    pending_text_ = pw_input_;
    pw_error_[0]  = '\0';
  }

  ro::EndRoWindow();
}

// ── Le volet « Réglages », DANS la fenêtre de salle ──────────────────────────

// Pré-remplit le volet sur le salon courant. Tout vient de notre état, sauf le
// mot de passe : il n'est sur AUCUN paquet reçu — le serveur ne le renvoie jamais
// — et sa seule trace est le bloc du CGameMode, où `SendMsg(43)` l'a écrit.
void ChatRoomWindow::LoadSettingsPane() {
  const char* wire = ro::Utf8ToWire(room_title_.c_str());
  std::snprintf(set_title_, sizeof(set_title_), "%s", wire ? wire : "");
  set_limit_  = (room_limit_ >= kLimitMin && room_limit_ <= kLimitMax)
                    ? room_limit_
                    : kLimitMax;
  set_public_ = room_public_;
  std::snprintf(set_password_, sizeof(set_password_), "%s",
                room_password_wire_.c_str());
}

// Hauteur du volet « Réglages », pour faire grandir la fenêtre d'autant. Quatre
// lignes de champs, le bouton, et le séparateur.
float ChatRoomWindow::SettingsPaneHeight() {
  return ImGui::GetFrameHeightWithSpacing() * 5.0f +
         ImGui::GetStyle().ItemSpacing.y * 2.0f;
}

void ChatRoomWindow::DrawSettingsPane() {
  mui::Separator();
  const float label_col =
      ImGui::CalcTextSize(msgstr::Utf8Or(kMsgRoomPassLabel, "Mot de passe :")).x +
      ImGui::GetFontSize();

  ImGui::TextUnformatted(msgstr::Utf8Or(kMsgRoomTitleLabel, "Titre :"));
  ImGui::SameLine(label_col);
  ImGui::PushItemWidth(-1.0f);
  ro::InputTextCp949("##salle_set_titre", set_title_, sizeof(set_title_));
  ImGui::PopItemWidth();

  ImGui::TextUnformatted(msgstr::Utf8Or(kMsgRoomLimitLabel, "Limite :"));
  ImGui::SameLine(label_col);
  ImGui::PushItemWidth(-ImGui::GetFontSize() * 4.0f);
  mui::WheelSliderInt("##salle_set_limite", &set_limit_, kLimitMin, kLimitMax, "%d");
  ImGui::PopItemWidth();
  ImGui::SameLine();
  ImGui::TextUnformatted(msgstr::Utf8Or(kMsgCountUnitPeople, "pers."));

  ImGui::TextUnformatted(i18n::Tr("Accès"));
  ImGui::SameLine(label_col);
  if (ImGui::RadioButton(msgstr::Utf8Or(kMsgRoomPublic, "Public"), set_public_))
    set_public_ = true;
  ImGui::SameLine();
  if (ImGui::RadioButton(msgstr::Utf8Or(kMsgRoomPrivate, "Privé"), !set_public_))
    set_public_ = false;

  ImGui::BeginDisabled(set_public_);
  ImGui::TextUnformatted(msgstr::Utf8Or(kMsgRoomPassLabel, "Mot de passe :"));
  ImGui::SameLine(label_col);
  ImGui::PushItemWidth(-1.0f);
  ro::InputTextCp949("##salle_set_passe", set_password_, sizeof(set_password_));
  ImGui::PopItemWidth();
  ImGui::EndDisabled();

  const int t_len = static_cast<int>(std::strlen(set_title_));
  const int p_len = static_cast<int>(std::strlen(set_password_));
  const bool ok = t_len > 0 && t_len <= kTitleMaxBytes &&
                  (set_public_ || (p_len >= kPassMinBytes && p_len <= kPassMaxBytes));
  ImGui::BeginDisabled(!ok);
  if (ro::RoButton(i18n::Tr("Appliquer"))) {
    // ARMER : `SendChangeChatRoom` part sur le fil natif, hors frame.
    pending_ = Pending::kApplySettings;
    room_show_settings_ = false;
  }
  ImGui::EndDisabled();
}
