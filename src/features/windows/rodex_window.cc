#include "ragnarok/item_db.h"
#include "ragnarok/globals.h"
#include "features/windows/rodex_window.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>

#include "bourgeon.h"        // Bourgeon::Instance().SendPacket
#include "d3d9/d3d9_hook.h"  // Overlay_DeviceEpoch (invalidation des textures)
#include "features/item_cell.h"  // itemcell::NameById (nom d'item par id)
#include "imgui.h"
#include "features/windows/inventory_viewer.h"  // MailDraggedItem (cible de drag-drop « INV_ITEM »)
#include "ragnarok/uiwnd.h"  // uiwnd::FindWindow / uiwnd::Mgr
#include "ui/game_texture.h" // ro::TextureFromGameFile (icônes .bmp du GRF)
#include "ui/icon_cache.h"   // ro::ItemIcon (chargement + epoch de device partagés)
#include "ui/ro_imgui.h"     // BeginRoWindow / RoButton (skin RO)
#include "ui/ro_widgets.h"   // mui::IsLastItemRightClicked (clic droit = description)
#include "utils/hooking/hook_manager.h"  // détour du handler de contenu ZC 0x0B63
#include "utils/log_console.h"
#include "utils/i18n.h"

// ── Constantes RE (client 20250716, base 0x400000 ; cf. docs/rodex_re.md) ──
namespace {

// Fenêtres natives à masquer + fermeture propre (persiste la position, comme le X).

constexpr int kInboxId = 0x107;  // UIRodexWnd     — la LISTE
constexpr int kReadId  = 0x109;  // UIRodexReadWnd — la LECTURE
constexpr uintptr_t kInboxVTable = 0x01022170;  // vérifiée live (g_RodexInboxWnd+0)
constexpr uintptr_t kReadVTable  = 0x01021fbc;
constexpr int kOffWndPosX = 0x1c;  // position écran (reprise pour placer l'ImGui)
constexpr int kOffWndPosY = 0x20;

// ── Fenêtre d'ÉCRITURE (UIMailWriteWnd 0x108) : elle ne naît PLUS ───────────
// 🔴 Elle est le seul cas de la campagne qu'il ne fallait SURTOUT pas détruire :
// elle émet CZ_REQ_CANCEL_WRITE_MAIL en se fermant (UIMailWriteWnd_OnMsg
// @0x007ca3fd), ce dont le plugin se servait justement pour annuler. La détruire au
// tick aurait annulé la rédaction une frame après son ouverture. On l'empêche donc
// de NAÎTRE, en prenant la place de son unique créateur (ZC 0x0A12 ci-dessous).
constexpr uintptr_t kWriteWndPtr = 0x0131f940;  // g_MailWriteWnd (filet : doit rester nul)
constexpr int kWriteId = 0x108;
constexpr uintptr_t kWriteVTable = 0x01021b30;

// ZC_ACK_OPEN_WRITE_MAIL 0x0A12 (27 o) : {op:2, name[24], result:1}. SEUL créateur de
// la fenêtre d'écriture (Recv_ZC_RodexBeginWriteResult 0x00cfcc80, result lu en
// paquet+0x1a). En régime « replace », `data` = paquet+2 : nom en 0, result en 24.
constexpr uint16_t kZcBeginWriteAck   = 0x0A12;
constexpr int      kBeginWriteResult  = 24;
constexpr int      kBeginWriteNameLen = 24;

// ⚠ DEVOIRS CACHÉS de ce handler, à rejouer AVANT d'ouvrir notre rédaction — les
// deux sont __thiscall(session) et ils NE SONT PAS cosmétiques :
//   0x00d7f380 : pour chacun des 5 emplacements encore garni, REND l'objet à
//                l'inventaire (Inventory_AddOrStackItem) puis rafraîchit les fenêtres
//                d'items. Sans lui, une rédaction abandonnée fait disparaître ses
//                pièces jointes du modèle jusqu'au prochain envoi serveur.
//   0x00d7f480 : remet à zéro l'index d'inventaire des 5 emplacements (+ un u16).
constexpr uintptr_t kMailReturnAttachments = 0x00d7f380;
constexpr uintptr_t kMailClearAttachSlots  = 0x00d7f480;
using SessionVoidFn_t = void(__thiscall*)(void*);

// ZC_ACK_WRITE_MAIL 0x09ED (3 o) : {op:2, result:1}, 0 = envoyé. OBSERVÉ et non
// remplacé — le handler natif fait aussi le ménage des emplacements après un envoi
// réussi, et lui retirer ce travail ferait réapparaître les objets envoyés à la
// rédaction suivante (via le « rend à l'inventaire » ci-dessus).
constexpr uint16_t kZcSendResult = 0x09ED;

// Frais d'envoi. Le client les calculait dans le DrawContent de sa fenêtre
// (sub_7C8FC0) ; la formule est reproduite telle quelle, avec SES constantes :
//   frais = nbPiècesJointes * 2500  +  (zeny * 2) / 100
// soit `imul eax, 9C4h` @0x007c9395 et `__alldiv(zeny*2, 100)` @0x007c942f.
// Recoupé avec l'observation en jeu : 666666 z + 3 objets = 20833.
constexpr uintptr_t kMailAttachCountFn = 0x00d80140;  // __thiscall(session) -> int
using MailAttachCount_t = int(__thiscall*)(void*);
constexpr int64_t kFeePerItem  = 2500;
constexpr int64_t kFeeZenyNum  = 2;
constexpr int64_t kFeeZenyDen  = 100;

// CZ_REQ_CANCEL_WRITE_MAIL : 2 octets, sans charge utile. Longueur confirmée dans la
// table du SERVEUR (moonlight, clif_packetdb.hpp : parseable_packet(0x0A03,2,…)).
constexpr uint16_t kCzCancelWrite = 0x0A03;

// Pièces jointes du courrier en cours d'écriture : 5 ItemSkillInfo dans la SESSION
// (vérifié live : slot 0 à 0x01600008, index inventaire +4, quantité +0x10, itemId
// en TEXTE +0x2c). Même source que la fenêtre native, qui les recopie dans sa liste
// à chaque msg 0x17 ; c'est aussi ce que `sub_D7F480` remet à zéro à chaque lecture.
constexpr uintptr_t kMailAttachSlot = rag::kSessionAddr + 23624;
constexpr int kAttachStride = 248;  // 0xF8 : taille d'un ItemSkillInfo
constexpr int kAttachSlots  = 5;    // MAIL_MAX_ITEM côté serveur
// Ces offsets sont ceux de l'ItemSkillInfo, la structure COMMUNE au reste du client
// (inventaire, chariot, entrepôt) : un slot de rédaction porte donc, tel quel, tout
// ce qu'un item d'inventaire porte — cartes et enchants compris.
constexpr int kInfoType   = 0x00;   // int : type d'item
constexpr int kInfoIndex  = 0x04;   // int : index d'inventaire
constexpr int kInfoLoc    = 0x08;   // int : masque d'emplacement d'équipement
constexpr int kInfoAmount = 0x10;   // int : quantité (< 1 => slot vide)
constexpr int kInfoCard0  = 0x1c;   // 4 × u32 : cartes / enchantements
constexpr int kInfoIdStr  = 0x2c;   // std::string SSO : itemId EN TEXTE
constexpr int kInfoIdent  = 0x5c;   // byte : identifié
constexpr int kInfoDamaged = 0x5d;  // byte : équipement CASSÉ
constexpr int kInfoRefine = 0x60;   // int : refine
constexpr int kInfoView   = 0x70;   // int : viewID
constexpr int kInfoGrade  = 0x88;   // i16 : grade d'enchantement
constexpr int kInfoOptCnt = 0x98;   // int : nombre d'options aléatoires
constexpr int kInfoOpts   = 0x9c;   // entrées de 5 octets
constexpr size_t kInfoSize = 0x100;  // ItemSkillInfo (0xf8), arrondi

// Singleton d'état CRodexSystemMgr (POINTEUR vers l'objet de 0x38 octets).
constexpr uintptr_t kRodexMgrPtr = 0x0131ecdc;
constexpr int kMgrSelMailId  = 0x08;  // int64 : courrier sélectionné/ouvert
constexpr int kMgrOpenType   = 0x10;  // byte  : boîte courante (0/1/2)
constexpr int kMgrUnread     = 0x18;  // int   : compteur de non-lus (badge menu)
// ⚠ +0x1c ne dit PAS si la boîte contient du courrier : le handler de liste y écrit
// 1 dès qu'une liste arrive, en recopiant `paquet+4` — un champ que rAthena remplit
// toujours avec 1 (`WFIFOB(fd, 4) = 1; // Unknown`). Sur une boîte vide il vaut donc
// 1 lui aussi. S'en servir pour choisir entre « Chargement… » et « Aucun courrier »
// laissait le joueur devant une attente qui n'aboutissait jamais.
constexpr int kMgrListReceived = 0x1c;  // byte : une liste a été reçue (toujours 1)
// Les trois std::map<int64, RodexMail> : tête (nœud sentinelle) + taille.
constexpr int kMgrBoxHead[3] = {0x20, 0x28, 0x30};  // Normal / Compte / Retour

// Purge les courriers marqués obsolètes (+0x1a8 == 1) dans les trois maps. Le
// bouton « rafraîchir » natif l'appelle avant de redemander la liste au serveur.
constexpr uintptr_t kPurgeStaleMails = 0x007c72e0;  // __thiscall(CRodexSystemMgr*)
using PurgeStaleMails_t = void(__thiscall*)(void*);

// ── Offsets DANS un nœud de map. Le RodexMail commence à nœud+0x18, mais tous
// les accès natifs sont écrits relativement au NŒUD : on garde cette convention.
// Nœud MSVC = {Left@0, Parent@4, Right@8, color@0xc, isnil@0xd}, clé int64 @0x10.
constexpr int kMailId        = 0x10;   // int64 : mailID (= la clé de la map)
// ⚠ +0x18 = IsRead, PAS « contenu reçu ». C'est le champ que le SERVEUR envoie dans
// chaque entrée de la liste (ZC 0x09F0, paquet+8), recopié tel quel par le
// constructeur du RodexMail (Recv_ZC_MailList_0x09F0 0x00cfc070). Un courrier lu
// lors d'une session précédente arrive donc avec 1 alors que son corps, son zeny et
// ses pièces jointes n'ont JAMAIS transité — s'en servir comme condition de lecture
// affichait des objets fantômes (cf. « contenu reçu » plus bas).
constexpr int kMailIsRead    = 0x18;   // byte  : 1 = déjà lu (état SERVEUR)
constexpr int kMailType      = 0x19;   // byte  : MAIL_TYPE — &2 zeny, &4 objets
constexpr int kMailSender    = 0x1c;   // std::string (24 o)
// Date d'EXPIRATION : time_t 32 bits à +0x38, et pas l'int64 qu'on croyait lire à
// +0x34. UIRodexWnd_DrawRowExpiry (0x007d20f0) reçoit les deux dwords mais ne se
// sert que du second : `difftime32(+0x38, maintenant)` = le temps restant (il jette
// le résultat du calcul sur +0x34). Vérifié live : +0x38 tombe à ~16 jours, +0x34
// donnerait une date en 2032.
constexpr int kMailSentAt    = 0x34;   // time_t 32 bits, non utilisé par le client
constexpr int kMailExpire    = 0x38;   // time_t 32 bits : date d'expiration
constexpr int kMailTitle     = 0x3c;   // std::string (24 o)
constexpr int kMailBody      = 0x54;   // std::string (24 o) — vide avant lecture
// ⚠ TOUT ce qui suit (compteur, blocs d'objets, zeny) n'est écrit QUE par le handler
// du contenu, ZC 0x0B63. Le constructeur du RodexMail, lui, n'initialise que
// l'en-tête et un corps std::string vide : de +0x6c à +0x1a8 le nœud garde des
// octets de pile résiduels tant que le courrier n'a pas été ouvert DANS CETTE
// SESSION. Aucun champ natif ne dit « le contenu est là » — le client n'en a pas
// besoin, sa fenêtre de lecture n'étant ouverte que par ce handler. On tient donc
// l'information nous-mêmes (registre `g_content_ids`, alimenté par le détour).
constexpr int kMailItemCount = 0x6c;   // byte : nombre de pièces jointes (<= 5)
constexpr int kMailItems     = 0x6d;   // 1er bloc d'objet ; stride 60
constexpr int kMailZeny      = 0x1a0;  // int64 : zeny joint
constexpr int kMailNodeSize  = 0x1b0;
constexpr int kItemStride    = 60;
// ── Champs d'un bloc objet (offsets relatifs au bloc) ────────────────────────
// Relevés UN À UN dans la recopie que le handler ZC 0x0B63 fait vers un
// ItemSkillInfo (0x00cfd0c0, désassemblage 0xcfd2ef-0xcfd46d) : chaque `mov` y
// apparie un offset de bloc et un offset d'ItemSkillInfo. Le bloc porte donc TOUT
// ce qu'il faut pour un nom complet et une description — y compris ce que le
// plugin ignorait jusqu'ici : cassé, emplacement, type, viewID, enchants, grade.
constexpr int kItAmount   = 0;   // u16 -> ISI+0x10
constexpr int kItId       = 2;   // u32 -> ItemSkillInfo_SetId
constexpr int kItIdentify = 6;   // u8  -> ISI+0x5c
constexpr int kItDamaged  = 7;   // u8  -> ISI+0x5d : équipement CASSÉ
constexpr int kItCard0    = 8;   // 4 × u32 -> ISI+0x1c
constexpr int kItLocation = 24;  // u32 -> ISI+0x08 : emplacement d'équipement
constexpr int kItType     = 28;  // u8  -> ISI+0x00 : type (le name-builder le lit)
constexpr int kItView     = 29;  // u16 -> ISI+0x70 : viewID
constexpr int kItOpts     = 33;  // 5 × {i16 index, i16 value, u8 param} -> ISI+0x9c
constexpr int kItOptStride = 5;
constexpr int kItOptMax    = 5;
constexpr int kItRefine   = 58;  // u8  -> ISI+0x60
constexpr int kItGrade    = 59;  // u8  -> ISI+0x88
// Les 5 blocs d'objets tiennent entre l'en-tête et le zeny : c'est ce qui rend la
// lecture bornée sans test par itération (et la taille de nœud le confirme).
static_assert(kMailItems + 5 * kItemStride <= kMailZeny,
              "les blocs d'objets déborderaient sur le zeny du courrier");
static_assert(kMailZeny + 8 <= kMailNodeSize, "zeny hors du nœud de map");

// Opcodes CZ construits en clair par les fenêtres natives (tailles pinnées).
constexpr uint16_t kCzOpenBox   = 0x0AC1;  // 26 o : {3 mailID les plus récents}
constexpr uint16_t kCzClaimZeny = 0x09F1;  // 11 o : {int64 mailID, u8 openType}
constexpr uint16_t kCzClaimItem = 0x09F3;  // 11 o : idem
constexpr uint16_t kCzReturn    = 0x0B98;  //  6 o : {u32 mailID_lo} (32 bits bas !)
// Écriture. Structures relues dans le SERVEUR (fork rAthena moonlight, clif.cpp) :
//   0x0B97 CZ_CHECKNAME2      27 o : {char name[24]; char own_char}
//   0x0A06 CZ_REQ_REMOVE_ITEM  6 o : {u16 index; u16 amount}
//   0x0A6E CZ_REQ_WRITE_MAIL2 VAR  : cf. BuildSendMailPacket
constexpr uint16_t kCzCheckName  = 0x0B97;
constexpr uint16_t kCzRemoveItem = 0x0A06;
constexpr uint16_t kCzSendMail   = 0x0A6E;
// Bornes du SERVEUR (common/mmo.hpp) — le client natif en autorise davantage, mais
// c'est le serveur qui tronque : autant refuser franchement plutôt que d'envoyer un
// sujet coupé au milieu. Longueurs annoncées = caractères + le '\0' final.
constexpr int kMailTitleMax = 39;   // MAIL_TITLE_LENGTH 40, terminaison comprise
constexpr int kMailBodyMax  = 499;  // MAIL_BODY_LENGTH 500, idem

// Bus de commandes = CMode::SendMsg, réplique de GameMode_GetActive(0x1213338) :
// le mode courant est *(0x1213338+4), et seulement si *(0x1213338+0x58) == 1.
// vtable+0x18 (slot 6) est le dispatcher de commandes (identique à TradeWindow).
constexpr int kCmdReadMail   = 0xc2;   // (mailID_lo, openType) -> CZ_REQ_READ_RODEX 0x09EA
constexpr int kCmdDeleteMail = 0xc4;   // (mailID_lo, openType) -> CZ_REQ_DELETE_MAIL 0x09F5
constexpr int kCmdBeginWrite = 0x10c;  // (char* destinataire ou 0) -> CZ 0x0A08
constexpr int kCmdAttach     = 0xc3;   // (index, amount) -> CZ_REQ_ADD_ITEM_TO_MAIL 0x0A04
// Le drop natif (UIMailWriteWnd OnMsg case 0x26) enchaîne cmd 0xc3 puis cmd 0x12 :
// sans ce second appel l'objet n'est pas réellement poussé — même piège que l'ajout
// à l'échange (cf. TradeWindow::AddItemToTrade).
constexpr int kCmdApply      = 0x12;

// ── Registre « le contenu de ce courrier est arrivé » ────────────────────────
// Seul le handler ZC 0x0B63 remplit le corps, le zeny et les pièces jointes d'un
// nœud ; rien dans la structure ne permet ensuite de savoir si c'est fait (cf. le
// commentaire de kMailIsRead). On le note donc au passage, en détournant le
// handler lui-même : c'est le seul point qui marque exactement l'événement, et il
// couvre aussi les lectures déclenchées par le natif.
//
// Anneau POD à taille fixe : le détour s'exécute au fil de la réception, dans la
// boucle de jeu (le même thread que OnTick), et n'a donc ni à allouer ni à
// verrouiller — mais il reste sur un chemin chaud. Au-delà de kContentRing ouverts
// dans une même session, le plus ancien retombe en « à rouvrir » — sans risque, on
// réaffiche juste l'invite « ouvre le courrier ».
constexpr uintptr_t kRecvAckReadRodex = 0x00cfd0c0;  // Recv_ZC_AckReadRodex_0x0B63
using RecvAckReadRodex_t = char(__stdcall*)(int);
RecvAckReadRodex_t g_orig_ack_read = nullptr;

constexpr int kContentRing = 128;
volatile int64_t g_content_ids[kContentRing] = {0};
volatile long    g_content_count = 0;
// Manager auquel se rapporte le registre : il est recréé à chaque entrée en jeu
// (CRodexSystemMgr_CreateInstance), donc un pointeur différent = nouvelles maps,
// contenus perdus. Sans ça, un mailID recyclé rouvrirait la porte aux fantômes.
const void* g_content_owner = nullptr;

char __stdcall Detour_AckReadRodex(int packet) {
  const char result = g_orig_ack_read ? g_orig_ack_read(packet) : 0;
  __try {
    // ZC_ACK_READ_RODEX : +4 openType, +5 mailID (int64). Le nœud est rempli par
    // l'original, donc à ce point le contenu est bien en place.
    const int64_t mail_id =
        *reinterpret_cast<const int64_t*>(reinterpret_cast<const uint8_t*>(packet) + 5);
    if (mail_id != 0) {
      // Modulo NON signé : après 2^31 lectures le compteur repasse négatif, et un
      // index négatif écrirait hors du tableau.
      const unsigned long slot =
          static_cast<unsigned long>(InterlockedIncrement(&g_content_count) - 1);
      g_content_ids[slot % kContentRing] = mail_id;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return result;
}

bool ContentReceived(int64_t mail_id) {
  if (mail_id == 0) return false;
  const long count = g_content_count;
  const int n = (count < kContentRing) ? static_cast<int>(count) : kContentRing;
  for (int i = 0; i < n; ++i)
    if (g_content_ids[i] == mail_id) return true;
  return false;
}

void ForgetAllContent() {
  g_content_count = 0;
  for (int i = 0; i < kContentRing; ++i) g_content_ids[i] = 0;
}

// ── Courriers supprimés ──────────────────────────────────────────────────────
// Le handler natif de l'ack de suppression (Recv_ZC_RodexDeleteResult) retire bien
// le nœud… mais seulement dans la map de la boîte **Normal** (`g_RodexMgr+0x20`),
// quelle que soit la boîte réelle du courrier, et seulement si sa recherche de clé
// aboutit. Résultat : le courrier reste dans la map et donc dans notre liste, alors
// que le serveur l'a bien effacé — jusqu'au prochain rafraîchissement, qui le fait
// disparaître pour de bon. On tient donc notre propre liste des suppressions
// CONFIRMÉES et on filtre dessus, sans dépendre du ménage du natif.
//
// ⚠ Confirmées seulement : sur refus (pièces jointes non récupérées, par ex.) le
// courrier doit rester visible. Le natif affiche déjà la raison exacte (modale +
// toast MSI 0xf1a), inutile de la redire.
constexpr uintptr_t kRecvDeleteResult = 0x00cf8b10;
using RecvDeleteResult_t = int(__stdcall*)(int, int);
RecvDeleteResult_t g_orig_delete_result = nullptr;

constexpr int kDeletedRing = 64;
volatile long g_deleted_ids[kDeletedRing] = {0};  // 32 bits bas du mailID
volatile long g_deleted_count = 0;

int __stdcall Detour_DeleteResult(int mail_id_low, int result) {
  __try {
    if (result == 0 && mail_id_low != 0) {  // 0 = suppression acceptée
      const unsigned long slot =
          static_cast<unsigned long>(InterlockedIncrement(&g_deleted_count) - 1);
      g_deleted_ids[slot % kDeletedRing] = mail_id_low;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  if (g_orig_delete_result) return g_orig_delete_result(mail_id_low, result);
  return 0;
}

// Le protocole de suppression ne transporte que les 32 bits bas du mailID (comme
// la commande native) : la comparaison se fait donc sur cette moitié.
bool WasDeleted(int64_t mail_id) {
  const long low = static_cast<long>(mail_id & 0xffffffff);
  if (low == 0) return false;
  const long count = g_deleted_count;
  const int n = (count < kDeletedRing) ? static_cast<int>(count) : kDeletedRing;
  for (int i = 0; i < n; ++i)
    if (g_deleted_ids[i] == low) return true;
  return false;
}

void ForgetAllDeleted() {
  g_deleted_count = 0;
  for (int i = 0; i < kDeletedRing; ++i) g_deleted_ids[i] = 0;
}

// ── Vérification du destinataire (ZC_CHECKNAME 0x0A51) ───────────────────────
// Le serveur renvoie {CharId int32, Class int16, BaseLevel int16, Name[24]} et
// n'a PAS de code d'erreur : le char-server met 0/0/0 quand le `SELECT` sur la
// table des personnages ne trouve rien, donc **CharId == 0 = nom inexistant**.
// Le client range le tout dans les widgets de la fenêtre d'écriture ; on détourne
// plutôt le point qui reçoit les valeurs brutes (0x00d00010, appelé depuis
// RecvLoop_DispatchPackets), ce qui évite d'aller relire des libellés déjà
// formatés. L'original reste appelé : c'est lui qui affiche le refus du serveur
// dans le chat (MSI 0xA37, « The recipient's name does not exist. »).
constexpr uintptr_t kApplyCheckNameAck = 0x00d00010;  // __stdcall, retn 0x10
using ApplyCheckNameAck_t = int(__stdcall*)(int, int, int, int);
ApplyCheckNameAck_t g_orig_checkname_ack = nullptr;

volatile long g_check_seq   = 0;  // ++ à chaque réponse : le plugin détecte le delta
volatile long g_check_cid   = 0;  // 0 = personnage inexistant
volatile long g_check_class = 0;
volatile long g_check_level = 0;
char          g_check_name[24] = {0};

int __stdcall Detour_ApplyCheckNameAck(int char_id, int job_class, int base_level,
                                       int name_ptr) {
  __try {
    g_check_cid   = char_id;
    g_check_class = job_class;
    g_check_level = base_level;
    g_check_name[0] = '\0';
    if (name_ptr) {
      const char* name = reinterpret_cast<const char*>(name_ptr);
      std::strncpy(g_check_name, name, sizeof(g_check_name) - 1);
      g_check_name[sizeof(g_check_name) - 1] = '\0';
    }
    InterlockedIncrement(&g_check_seq);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  if (g_orig_checkname_ack)
    return g_orig_checkname_ack(char_id, job_class, base_level, name_ptr);
  return 0;
}

// Nom de métier lisible (« Lord Knight »…) depuis l'id de classe. Getter natif —
// jamais de table en dur : elle est chargée des .lub au boot et suit la langue du
// client. Le pointeur rendu appartient au client (ne pas libérer, ne pas garder).
constexpr uintptr_t kJobDisplayName = 0x00d5bb40;  // __thiscall(ctx, classId, sex)
using JobDisplayName_t = const char*(__thiscall*)(void*, unsigned int, int);

bool JobNameAnsi(int job_class, char* out, size_t cap) {
  out[0] = '\0';
  __try {
    const char* name = reinterpret_cast<JobDisplayName_t>(kJobDisplayName)(
        reinterpret_cast<void*>(rag::kSessionAddr), static_cast<unsigned int>(job_class), 99);
    if (!name || !*name) return false;
    std::strncpy(out, name, cap - 1);
    out[cap - 1] = '\0';
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; return false; }
}

// ── Icônes natives de la colonne « reste à récupérer » ───────────────────────
// Le natif a exactement les trois cas du masque MAIL_TYPE ; on reprend ses .bmp
// plutôt que d'inventer des lettres, que rien n'explique au joueur.
//
// Les chemins sont lus DANS l'exe, jamais recopiés : les strings RODEX y sont
// stockées SANS le dossier de tête (le code natif le prépend), on emprunte donc
// celui du btnbar — le même préfixe CP949 que l'inventaire ImGui utilise déjà.
constexpr uintptr_t kUiPrefixPath  = 0x010357b8;  // « 유저인터페이스\basic_interface\btnbar_left.bmp »
constexpr uintptr_t kIcoZenyPath   = 0x01021e9e;  // « \…\rodexsystem\renewal\icon_zeny.bmp »
constexpr uintptr_t kIcoItemPath   = 0x01022ad6;  // « …\icon_item.bmp »
constexpr uintptr_t kIcoBothPath   = 0x01022a8e;  // « …\icon_zeny_n_item.bmp »

// `<dossier de tête de la string btnbar> + <chemin RODEX>`. Le préfixe s'arrête
// AVANT son séparateur, puisque les chemins RODEX commencent déjà par un « \ ».
void RodexTexPath(uintptr_t leaf_string, char* out, size_t cap) {
  out[0] = '\0';
  __try {
    const char* base = reinterpret_cast<const char*>(kUiPrefixPath);
    const char* sep = std::strchr(base, '\\');
    const size_t n = sep ? static_cast<size_t>(sep - base) : 0;
    if (n == 0 || n >= cap) return;
    std::memcpy(out, base, n);
    std::snprintf(out + n, cap - n, "%s", reinterpret_cast<const char*>(leaf_string));
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

ro::GameTexture g_ico_zeny, g_ico_item, g_ico_both;
bool     g_icons_tried = false;
unsigned g_icons_epoch = 0;

// Les textures vivent en D3DPOOL_DEFAULT : elles meurent à un reset de device
// (ALT-TAB en plein écran). Les réutiliser après coup ferait planter le rendu.
void EnsureAttachIcons() {
  const unsigned epoch = Overlay_DeviceEpoch();
  if (epoch != g_icons_epoch) {
    g_icons_epoch = epoch;
    g_ico_zeny = g_ico_item = g_ico_both = ro::GameTexture{};
    g_icons_tried = false;
  }
  if (g_icons_tried) return;
  g_icons_tried = true;
  char path[192];
  RodexTexPath(kIcoZenyPath, path, sizeof(path));
  g_ico_zeny = ro::TextureFromGameFile(path);
  RodexTexPath(kIcoItemPath, path, sizeof(path));
  g_ico_item = ro::TextureFromGameFile(path);
  RodexTexPath(kIcoBothPath, path, sizeof(path));
  g_ico_both = ro::TextureFromGameFile(path);
}

// Recherche insensible à la casse, sur des octets UTF-8. Le repli ASCII suffit ici :
// les noms de personnage n'en sortent pas, et pour un sujet accentué les octets
// non-ASCII sont comparés tels quels — « é » trouve « é », mais pas « É ».
// (ImGuiTextFilter aurait été plus court, mais il est sensible à la casse : taper
// « gettar » ne trouverait pas « Gettar ».)
bool ContainsNoCase(const char* haystack, const char* needle) {
  if (!needle || !*needle) return true;
  if (!haystack || !*haystack) return false;
  for (const char* start = haystack; *start; ++start) {
    const char* a = start;
    const char* b = needle;
    while (*a && *b) {
      unsigned char ca = static_cast<unsigned char>(*a);
      unsigned char cb = static_cast<unsigned char>(*b);
      if (ca >= 'A' && ca <= 'Z') ca += 32;
      if (cb >= 'A' && cb <= 'Z') cb += 32;
      if (ca != cb) break;
      ++a;
      ++b;
    }
    if (!*b) return true;
  }
  return false;
}

// Nombre de courriers lus par boîte en une passe. Une boîte RODEX en contient
// rarement plus de quelques dizaines ; la borne existe pour qu'un arbre corrompu
// ne puisse pas transformer une frame de rendu en boucle infinie.
constexpr int kMaxNodesPerBox = 256;

// ── Miroir PLAT d'un courrier ────────────────────────────────────────────────
// Tout ce qui touche la mémoire du jeu est lu sous SEH, et MSVC interdit __try
// dans une fonction qui a des objets C++ à dérouler (C2712) : la lecture se fait
// donc en POD pur, la conversion en std::string se faisant chez l'appelant.
struct RawAttach {
  uint32_t id;
  int      amount;
  int      refine;
  int      identified;
  int      damaged;
  int      grade;
  uint32_t location;
  int      type;
  uint16_t view;
  uint32_t cards[4];
  int      opt_count;
  struct { int16_t index; int16_t value; uint8_t param; } opts[kItOptMax];
};
struct RawMail {
  int64_t   id;
  int64_t   expire;
  int64_t   zeny;
  int       is_read;        // état serveur (nœud +0x18)
  int       content_ready;  // contenu reçu dans CETTE session (registre du détour)
  int       type;
  int       item_count;
  char      sender[64];
  char      title[128];
  char      body[1200];
  RawAttach items[5];
};

// ── Accès natifs, tous SEH-gardés (les structures peuvent disparaître entre
// deux frames : changement de carte, déconnexion, fermeture par le serveur).
void* FindWnd(int id) { return uiwnd::SafeFindWindow(id); }
uintptr_t VTableOf(void* w) { return uiwnd::SafeVTableOf(w); }
void SetWndVisible(void* w, int visible) {
  uiwnd::SafeSetVisible(w, visible != 0);
}
void HideWnd(void* w) { SetWndVisible(w, 0); }
void CloseWnd(int id) { uiwnd::SafeCloseWindow(id); }
uint8_t* RodexMgr() {
  __try { return *reinterpret_cast<uint8_t**>(kRodexMgrPtr); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// Fenêtre d'écriture native (masquée) : c'est elle qui porte l'état que le natif
// tient à jour pour nous. nullptr si aucune écriture n'est en cours.
uint8_t* ComposeWnd() {
  __try {
    uint8_t* w = *reinterpret_cast<uint8_t**>(kWriteWndPtr);
    if (w && *reinterpret_cast<uintptr_t*>(w) == kWriteVTable) return w;
    return nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// Position écran d'une fenêtre native, pour poser l'ImGui au même endroit.
bool WndScreenPos(void* w, int* out_x, int* out_y) {
  __try {
    uint8_t* base = reinterpret_cast<uint8_t*>(w);
    const int x = *reinterpret_cast<int*>(base + kOffWndPosX);
    const int y = *reinterpret_cast<int*>(base + kOffWndPosY);
    if (x <= -2000 || y <= -2000) return false;  // pièce de HUD parquée hors écran
    *out_x = x;
    *out_y = y;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void SetOpenType(int open_type) {
  uint8_t* mgr = RodexMgr();
  if (!mgr) return;
  __try { *(mgr + kMgrOpenType) = static_cast<uint8_t>(open_type); }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void ClearSelectedMailId() {
  uint8_t* mgr = RodexMgr();
  if (!mgr) return;
  __try { *reinterpret_cast<int64_t*>(mgr + kMgrSelMailId) = 0; }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void PurgeStaleMails() {
  uint8_t* mgr = RodexMgr();
  if (!mgr) return;
  __try { reinterpret_cast<PurgeStaleMails_t>(kPurgeStaleMails)(mgr); }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// CMode::SendMsg (thread principal UNIQUEMENT). No-op si aucun mode n'est actif.
void ModeCmd(int cmd, int a, int b, int c, int d) {
  __try {
    uint8_t* mgr = reinterpret_cast<uint8_t*>(rag::kModeMgrAddr);
    if (*reinterpret_cast<int*>(mgr + 0x58) != 1) return;  // aucun mode actif
    void* disp = *reinterpret_cast<void**>(mgr + 4);
    if (disp) {
      void** vt = *reinterpret_cast<void***>(disp);
      using Fn = int(__thiscall*)(void*, int, int, int, int, int);
      reinterpret_cast<Fn>(vt[6])(disp, cmd, a, b, c, d);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// std::string MSVC : {buf[16] | ptr, size @+0x10, capacité @+0x14}. Au-delà de
// 15 caractères le texte part sur le tas et les 4 premiers octets sont alors le
// pointeur. Buffer vidé sur structure incohérente, plutôt que de lire au hasard.
// Renvoie le nombre d'octets copiés : une std::string du client peut contenir des
// NUL INTERNES (le corps d'un courrier multi-lignes en met un par saut de ligne),
// que `strlen` ne verrait pas.
size_t CopyStdString(const uint8_t* base, char* out, size_t cap) {
  out[0] = '\0';
  size_t copied = 0;
  __try {
    const uint32_t size = *reinterpret_cast<const uint32_t*>(base + 0x10);
    const uint32_t capacity = *reinterpret_cast<const uint32_t*>(base + 0x14);
    if (size == 0 || capacity < size || size > 0x4000) return 0;
    const char* text = (capacity > 15) ? *reinterpret_cast<const char* const*>(base)
                                       : reinterpret_cast<const char*>(base);
    if (!text) return 0;
    size_t n = size;
    if (n > cap - 1) n = cap - 1;
    std::memcpy(out, text, n);
    out[n] = '\0';
    copied = n;
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; return 0; }
  return copied;
}

// Copie plate d'un nœud de map vers un RawMail. False = nœud illisible.
bool ReadRawMail(const uint8_t* node, RawMail* out) {
  std::memset(out, 0, sizeof(*out));
  __try {
    out->id      = *reinterpret_cast<const int64_t*>(node + kMailId);
    out->is_read = *(node + kMailIsRead) != 0;
    out->type    = *(node + kMailType);
    out->expire  = *reinterpret_cast<const int32_t*>(node + kMailExpire);
    // ⚠ Zeny, compteur et blocs d'objets n'existent QUE si le contenu a été reçu
    // pendant cette session : hors de ce cas ces octets n'ont jamais été écrits et
    // les lire produisait des objets fantômes (ids et quantités aberrants). Le
    // drapeau « lu » du serveur ne dit RIEN là-dessus — un courrier lu hier arrive
    // à 1 avec un nœud vierge —, seul notre registre fait foi.
    // ⚠ Bloc CONDITIONNEL, surtout pas un `return` anticipé : l'en-tête textuel
    // (expéditeur, titre) est lu plus bas, hors de ce __try, et le natif l'affiche
    // dès la liste. Court-circuiter la fonction ici laissait toute la colonne
    // « Expéditeur » sur « (inconnu) » tant que le courrier n'était pas ouvert.
    out->content_ready = ContentReceived(out->id);
    if (out->content_ready) {
      out->zeny = *reinterpret_cast<const int64_t*>(node + kMailZeny);
      int count = *(node + kMailItemCount);
      if (count < 0) count = 0;
      if (count > 5) count = 5;
      out->item_count = count;
      for (int i = 0; i < count; ++i) {
        const uint8_t* blk = node + kMailItems + i * kItemStride;
        RawAttach& attach = out->items[i];
        attach.amount     = *reinterpret_cast<const uint16_t*>(blk + kItAmount);
        attach.id         = *reinterpret_cast<const uint32_t*>(blk + kItId);
        attach.refine     = *(blk + kItRefine);
        attach.identified = *(blk + kItIdentify) != 0;
        attach.damaged    = *(blk + kItDamaged) != 0;
        attach.grade      = *(blk + kItGrade);
        attach.location   = *reinterpret_cast<const uint32_t*>(blk + kItLocation);
        attach.type       = *(blk + kItType);
        attach.view       = *reinterpret_cast<const uint16_t*>(blk + kItView);
        for (int c = 0; c < 4; ++c)
          attach.cards[c] = *reinterpret_cast<const uint32_t*>(blk + kItCard0 + 4 * c);
        // Enchantements. Le natif, lui, COMPTE les entrées d'index non nul (cascade
        // de `cmovz` en 0xcfd3a1) puis recopie les N PREMIÈRES — deux choses
        // différentes, qui ne coïncident que parce que le serveur tasse ses options
        // depuis l'entrée 0. On s'arrête donc au premier index nul : même résultat
        // sur un paquet tassé, et pas d'option fantôme si jamais il ne l'est pas.
        attach.opt_count = 0;
        for (int o = 0; o < kItOptMax; ++o) {
          const uint8_t* opt = blk + kItOpts + o * kItOptStride;
          const int16_t index = *reinterpret_cast<const int16_t*>(opt);
          if (index == 0) break;
          attach.opts[o].index = index;
          attach.opts[o].value = *reinterpret_cast<const int16_t*>(opt + 2);
          attach.opts[o].param = opt[4];
          ++attach.opt_count;
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  // Les trois textes sont lus hors du __try principal : CopyStdString a son
  // propre garde, et un titre illisible ne doit pas jeter tout le courrier.
  CopyStdString(node + kMailSender, out->sender, sizeof(out->sender));
  CopyStdString(node + kMailTitle,  out->title,  sizeof(out->title));
  // Le corps stocke ses sauts de ligne comme des NUL (vérifié live :
  // « qsdfqsdf\0qsdfqsdf », 18 octets). Sans cette reprise, tout affichage s'arrête
  // à la première ligne — et l'expéditeur croit qu'on lui a tronqué son message.
  const size_t body_len = CopyStdString(node + kMailBody, out->body, sizeof(out->body));
  for (size_t i = 0; i + 1 < body_len; ++i)
    if (out->body[i] == '\0') out->body[i] = '\n';
  return true;
}

// Collecte les nœuds d'une boîte (parcours itératif de l'arbre rouge-noir MSVC).
// Renvoie le nombre de nœuds écrits dans `out`.
int CollectBoxNodes(const uint8_t* mgr, int box, const uint8_t** out, int max_nodes) {
  int found = 0;
  __try {
    const uint8_t* head = *reinterpret_cast<const uint8_t* const*>(mgr + kMgrBoxHead[box]);
    if (!head) return 0;
    const uint8_t* root = *reinterpret_cast<const uint8_t* const*>(head + 4);
    if (!root || root == head) return 0;
    const uint8_t* stack[64];
    int sp = 0, guard = 0;
    stack[sp++] = root;
    while (sp > 0 && found < max_nodes && guard < 4 * kMaxNodesPerBox) {
      ++guard;
      const uint8_t* node = stack[--sp];
      if (!node || node == head || *(node + 0xd) != 0) continue;  // sentinelle
      out[found++] = node;
      if (sp < 62) {
        const uint8_t* left  = *reinterpret_cast<const uint8_t* const*>(node + 0);
        const uint8_t* right = *reinterpret_cast<const uint8_t* const*>(node + 8);
        if (left  && left  != head) stack[sp++] = left;
        if (right && right != head) stack[sp++] = right;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { /* boîte illisible : on garde l'acquis */ }
  return found;
}

// mailID le plus récent d'une boîte = clé du nœud le plus à DROITE (les maps sont
// indexées par mailID croissant). 0 si la boîte est vide. C'est ce que le paquet
// d'ouverture 0x0AC1 annonce au serveur pour qu'il n'envoie que ce qui manque.
int64_t NewestMailId(const uint8_t* mgr, int box) {
  __try {
    const uint8_t* head = *reinterpret_cast<const uint8_t* const*>(mgr + kMgrBoxHead[box]);
    if (!head) return 0;
    const uint8_t* rightmost = *reinterpret_cast<const uint8_t* const*>(head + 8);
    if (!rightmost || rightmost == head) return 0;
    return *reinterpret_cast<const int64_t*>(rightmost + kMailId);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ── Lecture de l'état d'écriture (POD, SEH) ─────────────────────────────────
// Un slot de pièce jointe du courrier : ItemSkillInfo standard, comme l'échange et
// l'inventaire. Renvoie false si le slot est vide.
struct RawAttachSlot {
  int      inv_index;
  uint32_t item_id;
  int      amount;
  int      refine;
  int      identified;
  int      damaged;
  uint32_t cards[4];
  int      opt_count;
  struct { int16_t index; int16_t value; uint8_t param; } opts[kItOptMax];
};
// Adresse du slot `slot` dans la session. FIXE pour toute la partie : c'est ce qui
// permet de la passer telle quelle comme `src` à itemcell::DeferDescById, dont le
// contrat exige que la source survive jusqu'au relâchement du bouton.
const uint8_t* AttachSlotAddr(int slot) {
  return reinterpret_cast<const uint8_t*>(
      kMailAttachSlot + static_cast<uintptr_t>(slot) * kAttachStride);
}
bool ReadAttachSlot(int slot, RawAttachSlot* out) {
  std::memset(out, 0, sizeof(*out));
  __try {
    const uint8_t* base = AttachSlotAddr(slot);
    const int amount = *reinterpret_cast<const int*>(base + kInfoAmount);
    if (amount < 1) return false;
    // itemId en TEXTE (std::string SSO : au-delà de 15 caractères, les 4 premiers
    // octets sont un pointeur) — même décodage que l'échange et l'inventaire.
    const char* sso = reinterpret_cast<const char*>(base + kInfoIdStr);
    const uint32_t capacity = *reinterpret_cast<const uint32_t*>(base + kInfoIdStr + 0x14);
    const char* text = (capacity > 15) ? *reinterpret_cast<const char* const*>(sso) : sso;
    out->item_id   = text ? static_cast<uint32_t>(std::atoi(text)) : 0;
    out->amount    = amount;
    out->inv_index = *reinterpret_cast<const int*>(base + kInfoIndex);
    out->refine    = *reinterpret_cast<const int*>(base + kInfoRefine);
    out->identified = *(base + kInfoIdent) != 0;
    out->damaged    = *(base + kInfoDamaged) != 0;
    for (int c = 0; c < 4; ++c)
      out->cards[c] = *reinterpret_cast<const uint32_t*>(base + kInfoCard0 + 4 * c);
    int nopt = *reinterpret_cast<const int*>(base + kInfoOptCnt);
    if (nopt < 0) nopt = 0;
    if (nopt > kItOptMax) nopt = kItOptMax;
    out->opt_count = nopt;
    for (int o = 0; o < nopt; ++o) {
      const uint8_t* opt = base + kInfoOpts + o * kItOptStride;
      out->opts[o].index = *reinterpret_cast<const int16_t*>(opt);
      out->opts[o].value = *reinterpret_cast<const int16_t*>(opt + 2);
      out->opts[o].param = opt[4];
    }
    return out->item_id != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Nombre de pièces jointes en cours, par le getter NATIF de la session — celui-là
// même qu'utilisait la fenêtre d'écriture pour ses frais.
int MailAttachCount() {
  __try {
    return reinterpret_cast<MailAttachCount_t>(kMailAttachCountFn)(
        reinterpret_cast<void*>(rag::kSessionAddr));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Frais d'envoi, formule du client (cf. les constantes plus haut). Le zeny joint est
// le NÔTRE (c'est notre champ de saisie qui remplace l'UIEdit natif).
int64_t ComputeMailFee(int64_t attached_zeny) {
  if (attached_zeny < 0) attached_zeny = 0;
  return static_cast<int64_t>(MailAttachCount()) * kFeePerItem +
         (attached_zeny * kFeeZenyNum) / kFeeZenyDen;
}

// Les deux devoirs du handler de ZC 0x0A12 dont on a pris la place : rendre les
// pièces jointes restées d'une rédaction précédente, puis vider les emplacements.
// Ordre du natif, conservé — l'inverse jetterait les objets.
void ResetMailAttachments() {
  __try {
    void* session = reinterpret_cast<void*>(rag::kSessionAddr);
    reinterpret_cast<SessionVoidFn_t>(kMailReturnAttachments)(session);
    reinterpret_cast<SessionVoidFn_t>(kMailClearAttachSlots)(session);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool ReadMgrCounters(const uint8_t* mgr, int* out_unread, bool* out_has_mail) {
  __try {
    *out_unread   = *reinterpret_cast<const int*>(mgr + kMgrUnread);
    *out_has_mail = *(mgr + kMgrListReceived) != 0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Le serveur encode ses textes en ANSI (CP_ACP = CP1252 sur Windows fr) et ImGui
// veut de l'UTF-8 — même conversion que les dialogues PNJ, sinon les accents
// tapés par les joueurs cassent dans les sujets et les corps de courrier.
std::string AnsiToUtf8(const char* in) {
  if (!in || !*in) return std::string();
  const int len = static_cast<int>(std::strlen(in));
  const int wn = MultiByteToWideChar(CP_ACP, 0, in, len, nullptr, 0);
  if (wn <= 0) return std::string(in);
  std::wstring w(static_cast<size_t>(wn), L'\0');
  MultiByteToWideChar(CP_ACP, 0, in, len, &w[0], wn);
  const int un = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wn, nullptr, 0, nullptr,
                                     nullptr);
  if (un <= 0) return std::string(in);
  std::string out(static_cast<size_t>(un), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wn, &out[0], un, nullptr, nullptr);
  return out;
}

// Sens inverse pour ce qui PART sur le fil : la saisie ImGui est en UTF-8, le
// serveur (comme le client) parle ANSI. Renvoie le nombre d'octets écrits, '\0'
// non compris. Les caractères non représentables deviennent '?'.
int Utf8ToAnsi(const char* utf8, char* out, size_t out_size) {
  out[0] = '\0';
  if (!utf8 || !*utf8) return 0;
  const int len = static_cast<int>(std::strlen(utf8));
  const int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, len, nullptr, 0);
  if (wn <= 0) return 0;
  std::wstring w(static_cast<size_t>(wn), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8, len, &w[0], wn);
  const int written = WideCharToMultiByte(CP_ACP, 0, w.c_str(), wn, out,
                                          static_cast<int>(out_size) - 1, "?", nullptr);
  const int n = (written > 0) ? written : 0;
  out[n] = '\0';
  return n;
}

// Nom d'item par id : itemcell::NameById (DB de descriptions du client, cache
// partagé).
//
// ⚠ Cette copie-ci faisait passer le nom par AnsiToUtf8, contrairement aux cinq
// autres fenêtres. Elle avait raison de convertir et tort sur le codage :
// AnsiToUtf8 (CP_ACP, donc 1252 ici) est ce qu'il faut pour ce qui arrive PAR LE
// FIL — expéditeur, titre, corps, le serveur parle ANSI — mais la DB item, elle,
// est en CP949. Sur les 27 219 noms de SystemEN\itemInfoMerged.lua, 20 sont
// non-ASCII : ni la conversion 1252 ni la lecture brute ne les rendaient. C'est
// itemcell::NameById qui convertit désormais, en CP949.

// Date d'expiration -> « J-3 » / « expiré ». La valeur est un horodatage Unix ;
// à 0 (courrier sans expiration) on n'affiche rien plutôt qu'une date absurde.
std::string ExpiryLabel(int64_t expire) {
  if (expire <= 0) return std::string();
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const int64_t left = expire - now;
  if (left <= 0) return std::string("expir\xC3\xA9");  // « expiré »
  const int64_t days = left / 86400;
  if (days >= 1) return "J-" + std::to_string(days);
  const int64_t hours = left / 3600;
  return std::to_string(hours > 0 ? hours : 1) + " h";
}

// Fabrication d'un ItemSkillInfo : mêmes appels que itemcell::OpenDescById, à ceci
// près que la source est un bloc de courrier et non un item vivant.
using InfoCtor_t  = void(__fastcall*)(void*);
using InfoSetId_t = void(__thiscall*)(void*, int);

}  // namespace

// ── Pièces jointes : du bloc de 60 octets à un item que le client sait nommer ──
//
// Ni le name-builder ni la fenêtre de description ne savent partir d'un id : ils
// veulent un ItemSkillInfo. Une pièce jointe n'en est pas un — c'est un bloc de 60
// octets dans le RodexMail — mais la RE du handler de contenu (0x00cfd0c0) montre
// que le natif fait exactement cette conversion avant d'afficher sa fenêtre de
// lecture, champ par champ. On la refait donc à l'identique.
//
// ⚠ Aucune fuite : ItemSkillInfo_SetId n'écrit qu'un id NUMÉRIQUE dans la
// std::string +0x2c (10 caractères au plus, donc SSO) et le ctor ne construit que
// des chaînes vides. Le tampon peut mourir au retour, comme celui d'OpenDescById.
bool RodexWindow::BuildAttachInfo(const Attach& attach, void* out_info) {
  std::memset(out_info, 0, kInfoSize);
  if (attach.id == 0) return false;
  __try {
    uint8_t* info = static_cast<uint8_t*>(out_info);
    reinterpret_cast<InfoCtor_t>(itemdb::kInfoCtorAddr)(info);
    reinterpret_cast<InfoSetId_t>(itemdb::kInfoSetIdAddr)(info,
                                                          static_cast<int>(attach.id));
    *reinterpret_cast<int*>(info + kInfoType)    = attach.type;
    *reinterpret_cast<uint32_t*>(info + kInfoLoc) = attach.location;
    *reinterpret_cast<int*>(info + kInfoAmount)  = attach.amount;
    for (int c = 0; c < 4; ++c)
      *reinterpret_cast<uint32_t*>(info + kInfoCard0 + 4 * c) = attach.cards[c];
    info[kInfoIdent]   = attach.identified ? 1 : 0;
    info[kInfoDamaged] = attach.damaged ? 1 : 0;
    *reinterpret_cast<int*>(info + kInfoRefine) = attach.refine;
    *reinterpret_cast<int*>(info + kInfoView)   = attach.view;
    *reinterpret_cast<int16_t*>(info + kInfoGrade) = attach.grade;
    *reinterpret_cast<int*>(info + kInfoOptCnt) = attach.opt_count;
    for (int o = 0; o < attach.opt_count && o < kItOptMax; ++o) {
      uint8_t* opt = info + kInfoOpts + o * kItOptStride;
      *reinterpret_cast<int16_t*>(opt)     = attach.opts[o].index;
      *reinterpret_cast<int16_t*>(opt + 2) = attach.opts[o].value;
      opt[4] = attach.opts[o].param;
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void RodexWindow::ResolveAttachDisplay(Attach* attach) {
  attach->name[0] = '\0';
  attach->total_slots = 0;
  if (attach->id == 0) return;
  // Nom de base : il sert de repli, et son effet de bord compte autant — c'est lui
  // qui déclenche le chargement paresseux de la DB, sans quoi le name-builder ne
  // trouverait rien pour un objet jamais consulté de la session.
  const char* base_name = itemcell::NameById(attach->id);

  // Slot de RÉDACTION : l'ItemSkillInfo existe déjà en session, à une adresse fixe.
  // Le reconstruire serait moins fidèle (on perdrait ce que le natif y écrit).
  uint8_t scratch[kInfoSize];
  const void* info = nullptr;
  if (attach->mail_slot >= 0) {
    info = AttachSlotAddr(attach->mail_slot);
  } else if (BuildAttachInfo(*attach, scratch)) {
    info = scratch;
  }
  if (!info) {
    std::snprintf(attach->name, sizeof(attach->name), "%s", base_name);
    return;
  }

  // ⚠ Contexte `this` du builder = nullptr, et c'est délibéré : les trois fenêtres
  // RODEX natives sont détruites, il n'y a plus de fenêtre à lui donner. Le natif ne
  // s'en sert que pour mettre en file l'attente du nom d'un FORGERON non résolu
  // (UIWindowMgr_FindOrQueueNameRequest) ; partout ailleurs il l'ignore. Le SEH
  // interne de BuildDisplayName couvre ce seul cas, avec repli sur le nom de base.
  char decorated[96];
  itemcell::BuildDisplayName(const_cast<void*>(info), decorated,
                             sizeof(decorated));
  attach->total_slots = itemcell::SlotCount(const_cast<void*>(info));
  // Le builder rend du texte dans la code-page du CLIENT ; ImGui veut de l'UTF-8.
  std::snprintf(attach->name, sizeof(attach->name), "%s",
                decorated[0] ? ro::LocalToUtf8(decorated) : base_name);
}

// ── Lecture de l'état natif ─────────────────────────────────────────────────
// Parcours des trois std::map du manager. Les nœuds sont COPIÉS dans mails_ : on
// ne garde jamais un pointeur de nœud d'une frame à l'autre, le natif effaçant et
// réallouant les siens à chaque ack serveur.
void RodexWindow::ReadState() {
  mails_.clear();
  const uint8_t* mgr = RodexMgr();
  if (!mgr) return;
  // Manager recréé (nouvelle entrée en jeu, changement de personnage) : les maps
  // sont neuves, aucun contenu n'a transité pour elles. Oublier le registre évite
  // qu'un mailID recyclé fasse à nouveau lire des octets non initialisés.
  if (mgr != g_content_owner) {
    ForgetAllContent();
    ForgetAllDeleted();
    g_content_owner = mgr;
  }
  if (!ReadMgrCounters(mgr, &unread_, &list_received_)) return;

  const uint8_t* nodes[kMaxNodesPerBox];
  for (int box = 0; box < 3; ++box) {
    const int count = CollectBoxNodes(mgr, box, nodes, kMaxNodesPerBox);
    for (int i = 0; i < count; ++i) {
      RawMail raw;
      if (!ReadRawMail(nodes[i], &raw)) continue;
      // Suppression confirmée par le serveur mais nœud encore présent : le ménage
      // natif ne fouille que la boîte Normal. Sans ce filtre, le courrier resterait
      // affiché jusqu'au prochain rafraîchissement.
      if (WasDeleted(raw.id)) continue;
      Mail mail;
      mail.id     = raw.id;
      mail.box    = static_cast<uint8_t>(box);
      mail.type   = static_cast<uint8_t>(raw.type);
      mail.is_read       = raw.is_read != 0;
      mail.content_ready = raw.content_ready != 0;
      mail.expire = raw.expire;
      mail.zeny   = raw.zeny;
      mail.sender     = AnsiToUtf8(raw.sender);
      mail.sender_raw = raw.sender;
      mail.title  = AnsiToUtf8(raw.title);
      mail.body   = AnsiToUtf8(raw.body);
      for (int k = 0; k < raw.item_count; ++k) {
        const RawAttach& src = raw.items[k];
        if (!src.id || src.amount <= 0) continue;
        Attach attach;
        attach.id         = src.id;
        attach.amount     = src.amount;
        attach.refine     = src.refine;
        attach.identified = src.identified != 0;
        attach.damaged    = src.damaged != 0;
        attach.grade      = static_cast<uint8_t>(src.grade);
        attach.type       = src.type;
        attach.location   = src.location;
        attach.view       = src.view;
        for (int c = 0; c < 4; ++c) attach.cards[c] = src.cards[c];
        attach.opt_count = src.opt_count;
        for (int o = 0; o < src.opt_count && o < kItOptMax; ++o) {
          attach.opts[o].index = src.opts[o].index;
          attach.opts[o].value = src.opts[o].value;
          attach.opts[o].param = src.opts[o].param;
        }
        ResolveAttachDisplay(&attach);
        mail.items.push_back(attach);
      }
      mails_.push_back(std::move(mail));
    }
  }

  // Le plus récent d'abord : les mailID sont croissants, donc décroissant = du
  // plus récent au plus ancien, comme la première page de la liste native.
  std::sort(mails_.begin(), mails_.end(),
            [](const Mail& a, const Mail& b) { return a.id > b.id; });
}

const RodexWindow::Mail* RodexWindow::Selected() const {
  if (!selected_id_) return nullptr;
  for (const Mail& mail : mails_)
    if (mail.id == selected_id_ && mail.box == selected_box_) return &mail;
  return nullptr;
}

// ── Actions ─────────────────────────────────────────────────────────────────

void RodexWindow::RequestRefresh() {
  const uint8_t* mgr = RodexMgr();
  if (!mgr) return;
  // Même séquence que le bouton « rafraîchir » natif (OnMsg 0x19f) : purge des
  // courriers marqués obsolètes, puis demande de la liste en annonçant, pour
  // chacune des trois boîtes, le mailID le plus récent déjà connu.
  PurgeStaleMails();

  uint8_t packet[26] = {0};
  *reinterpret_cast<uint16_t*>(packet) = kCzOpenBox;
  *reinterpret_cast<int64_t*>(packet + 2)  = NewestMailId(mgr, 0);  // Normal
  *reinterpret_cast<int64_t*>(packet + 10) = NewestMailId(mgr, 2);  // Retour
  *reinterpret_cast<int64_t*>(packet + 18) = NewestMailId(mgr, 1);  // Compte
  Bourgeon::Instance().SendPacket(packet, sizeof(packet));
  list_requested_ms_ = GetTickCount();
}

void RodexWindow::OpenMail(const Mail& mail) {
  // Le natif ne transmet que les 32 bits bas du mailID (clic sur le sujet d'une
  // ligne, OnMsg 0x62 / ctrl 0x143) : on réplique à l'identique.
  ModeCmd(kCmdReadMail, static_cast<int>(mail.id & 0xffffffff), mail.box, 0, 0);
}

void RodexWindow::ClaimAttachments(const Mail& mail) {
  // Réplique de Rodex_ClaimAttachments (0x007d0110) : les objets d'abord, le zeny
  // ensuite, et uniquement si le type du courrier annonce la pièce jointe
  // correspondante — le serveur rejetterait les autres demandes.
  uint8_t packet[11] = {0};
  *reinterpret_cast<uint32_t*>(packet + 2) =
      static_cast<uint32_t>(mail.id & 0xffffffff);
  *reinterpret_cast<uint32_t*>(packet + 6) =
      static_cast<uint32_t>(static_cast<uint64_t>(mail.id) >> 32);
  packet[10] = mail.box;
  if (mail.type & 4) {
    *reinterpret_cast<uint16_t*>(packet) = kCzClaimItem;
    Bourgeon::Instance().SendPacket(packet, sizeof(packet));
  }
  if (mail.type & 2) {
    *reinterpret_cast<uint16_t*>(packet) = kCzClaimZeny;
    Bourgeon::Instance().SendPacket(packet, sizeof(packet));
  }
}

void RodexWindow::DeleteMail(const Mail& mail) {
  ModeCmd(kCmdDeleteMail, static_cast<int>(mail.id & 0xffffffff), mail.box, 0, 0);
  if (selected_id_ == mail.id) selected_id_ = 0;
}

void RodexWindow::ReturnMail(const Mail& mail) {
  // CZ 0x0B98 n'emporte que les 32 bits bas du mailID : c'est le paquet natif,
  // pas un raccourci de notre part.
  uint8_t packet[6] = {0};
  *reinterpret_cast<uint16_t*>(packet) = kCzReturn;
  *reinterpret_cast<uint32_t*>(packet + 2) =
      static_cast<uint32_t>(mail.id & 0xffffffff);
  Bourgeon::Instance().SendPacket(packet, sizeof(packet));
  if (selected_id_ == mail.id) selected_id_ = 0;
}

void RodexWindow::ComposeTo(const char* recipient) {
  // Rien de plus que Compose : l'intérêt est d'ouvrir cette porte aux autres plugins
  // sans exposer toute la mécanique d'écriture.
  Compose(recipient && recipient[0] ? recipient : nullptr);
}

void RodexWindow::Compose(const char* recipient) {
  // On demande au SERVEUR d'ouvrir une session de rédaction (cmd 0x10c -> CZ 0x0A08) :
  // sans son accord, `sd->state.mail_writing` reste faux et l'envoi serait ignoré EN
  // SILENCE. Sa réponse (ZC 0x0A12) ouvre notre fenêtre — elle ne crée plus la native.
  // `recipient` non nul pré-remplit le destinataire (bouton « Répondre ») ; le serveur
  // nous le renvoie dans l'ack, on n'a donc rien à mémoriser ici.
  ModeCmd(kCmdBeginWrite, static_cast<int>(reinterpret_cast<uintptr_t>(recipient)),
          0, 0, 0);
}

// ── Écriture d'un courrier ──────────────────────────────────────────────────

void RodexWindow::ReadComposeState() {
  compose_items_.clear();
  if (!compose_open_) return;
  // Frais recalculés à chaque tick : ils dépendent du zeny saisi ET du nombre de
  // pièces jointes, qui bouge à chaque ajout ou retrait.
  tax_ = ComputeMailFee(attach_zeny_);
  for (int slot = 0; slot < kAttachSlots; ++slot) {
    RawAttachSlot raw;
    if (!ReadAttachSlot(slot, &raw)) continue;  // slot vide : les suivants peuvent être pleins
    Attach attach;
    attach.id        = raw.item_id;
    attach.amount    = raw.amount;
    attach.refine    = raw.refine;
    attach.identified = raw.identified != 0;
    attach.damaged    = raw.damaged != 0;
    attach.inv_index = raw.inv_index;  // porté par l'objet, pas par sa position
    attach.mail_slot = slot;           // l'ItemSkillInfo vivant est à cette adresse
    for (int c = 0; c < 4; ++c) attach.cards[c] = raw.cards[c];
    attach.opt_count = raw.opt_count;
    for (int o = 0; o < raw.opt_count && o < kItOptMax; ++o) {
      attach.opts[o].index = raw.opts[o].index;
      attach.opts[o].value = raw.opts[o].value;
      attach.opts[o].param = raw.opts[o].param;
    }
    // type/location/view/grade ne sont pas recopiés : le nom et la description
    // partent de l'ItemSkillInfo VIVANT du slot (cf. mail_slot), qui les porte déjà.
    ResolveAttachDisplay(&attach);
    compose_items_.push_back(attach);
  }
}

void RodexWindow::AttachItem(int index, int amount) {
  if (!imgui_enabled_ || !compose_open_) return;
  if (amount < 1) amount = 1;
  // Séquence EXACTE du drop natif (OnMsg case 0x26) : la commande d'ajout puis la
  // commande d'application. Le serveur (clif_parse_Mail_setattach) borne lui-même
  // la quantité et refuse ce qui n'est pas envoyable.
  ModeCmd(kCmdAttach, index, amount, 0, 0);
  ModeCmd(kCmdApply, 0, 0, 0, 0);
}

void RodexWindow::RemoveAttachment(int index, int amount) {
  if (index <= 0) return;
  uint8_t packet[6] = {0};
  *reinterpret_cast<uint16_t*>(packet) = kCzRemoveItem;
  *reinterpret_cast<uint16_t*>(packet + 2) = static_cast<uint16_t>(index);
  *reinterpret_cast<uint16_t*>(packet + 4) = static_cast<uint16_t>(amount < 1 ? 1 : amount);
  Bourgeon::Instance().SendPacket(packet, sizeof(packet));
}

void RodexWindow::CheckRecipient() {
  char name[24] = {0};
  Utf8ToAnsi(to_, name, sizeof(name));
  if (name[0] == '\0') return;
  uint8_t packet[27] = {0};
  *reinterpret_cast<uint16_t*>(packet) = kCzCheckName;
  std::memcpy(packet + 2, name, sizeof(name));
  packet[26] = 0;  // own_char : le serveur ne s'en sert pas pour la réponse
  Bourgeon::Instance().SendPacket(packet, sizeof(packet));
  recipient_char_id_ = 0;  // la réponse repeuplera le champ de la fenêtre native
  // « En attente » et pas « inconnu » : deux cas ne donnent AUCUNE réponse (carte
  // avec le flag NORODEX, char-server injoignable), et annoncer « n'existe pas »
  // sur un simple silence serait un mensonge.
  check_state_ = kCheckPending;
  checked_name_.clear();
  checked_job_.clear();
  checked_level_ = 0;
}

// Consomme la dernière réponse de vérification, s'il en est arrivé une depuis le
// dernier passage. Appelé sur le thread principal (OnTick).
void RodexWindow::PollRecipientCheck() {
  const long seq = g_check_seq;
  if (seq == check_seq_) return;
  check_seq_ = seq;

  checked_name_ = AnsiToUtf8(g_check_name);
  if (g_check_cid == 0) {
    // Pas de code d'erreur dans le protocole : le char-server renvoie 0/0/0 quand
    // le nom est introuvable en base. C'est le seul signal disponible.
    check_state_ = kCheckUnknown;
    checked_job_.clear();
    checked_level_ = 0;
    return;
  }
  check_state_   = kCheckFound;
  checked_level_ = static_cast<int>(g_check_level);
  // Le char id venait de la fenêtre native (+0xcc), que le natif remplissait depuis
  // CE même ack. On le prend directement à la source, la fenêtre n'existant plus.
  recipient_char_id_ = g_check_cid;
  char job[64] = {0};
  if (JobNameAnsi(static_cast<int>(g_check_class), job, sizeof(job)))
    checked_job_ = AnsiToUtf8(job);
  else
    checked_job_.clear();
}

void RodexWindow::SendMail() {
  send_error_.clear();
  // Conversion en ANSI d'abord : c'est la longueur ANSI qui compte pour le serveur
  // (un « é » fait 2 octets en UTF-8 et 1 en CP1252).
  char to_ansi[24] = {0};
  char title[64] = {0};
  char body[1024] = {0};
  Utf8ToAnsi(to_, to_ansi, sizeof(to_ansi));
  const int title_len = Utf8ToAnsi(subject_, title, sizeof(title));
  const int body_len  = Utf8ToAnsi(body_, body, sizeof(body));

  if (to_ansi[0] == '\0') { send_error_ = i18n::Tr("Indique un destinataire."); return; }
  if (title_len == 0)     { send_error_ = i18n::Tr("Le sujet ne peut pas être vide."); return; }
  if (title_len > kMailTitleMax) {
    send_error_ = i18n::Tr("Sujet trop long (39 caractères maximum).");
    return;
  }
  if (body_len > kMailBodyMax) {
    send_error_ = i18n::Tr("Message trop long (499 caractères maximum).");
    return;
  }

  // CZ_REQ_WRITE_MAIL2 0x0A6E, structure lue dans le serveur (clif_parse_Mail_send) :
  //   +0 op | +2 longueur | +4 destinataire[24] | +28 expéditeur[24] (ignoré)
  //   +52 zeny (u64) | +60 longueur du titre | +62 longueur du corps
  //   +64 char id | +68 titre puis corps.
  // Les deux longueurs INCLUENT le '\0' final : le serveur les passe à safestrncpy,
  // qui écrit au plus len-1 caractères — un titre annoncé sans son zéro perdrait sa
  // dernière lettre.
  const int title_field = title_len + 1;
  const int body_field  = body_len + 1;
  const int total = 68 + title_field + body_field;
  uint8_t packet[68 + 64 + 1024] = {0};
  *reinterpret_cast<uint16_t*>(packet)      = kCzSendMail;
  *reinterpret_cast<uint16_t*>(packet + 2)  = static_cast<uint16_t>(total);
  std::memcpy(packet + 4, to_ansi, sizeof(to_ansi));
  // +28 expéditeur : le serveur l'ignore (il connaît la session) — laissé à zéro.
  *reinterpret_cast<int64_t*>(packet + 52)  = attach_zeny_ < 0 ? 0 : attach_zeny_;
  *reinterpret_cast<uint16_t*>(packet + 60) = static_cast<uint16_t>(title_field);
  *reinterpret_cast<uint16_t*>(packet + 62) = static_cast<uint16_t>(body_field);
  *reinterpret_cast<uint32_t*>(packet + 64) = recipient_char_id_;
  std::memcpy(packet + 68, title, static_cast<size_t>(title_field));
  std::memcpy(packet + 68 + title_field, body, static_cast<size_t>(body_field));
  Bourgeon::Instance().SendPacket(packet, static_cast<size_t>(total));

  // On ne présume PAS de la réussite : c'est l'ack ZC_ACK_WRITE_MAIL (0x09ED, observé)
  // qui referme la fenêtre s'il vaut 0, et affiche un refus sinon. Le handler natif de
  // cet ack tourne toujours — c'est lui qui vide les emplacements de pièces jointes
  // après un envoi réussi, un ménage qu'on ne veut surtout pas lui retirer.
}

// Annulation d'une rédaction. C'est ce que faisait la fermeture de la fenêtre native :
// elle émettait CZ_REQ_CANCEL_WRITE_MAIL, le serveur clôt la session et rend les
// objets attachés. On l'émet donc nous-mêmes, puisqu'elle n'existe plus.
void RodexWindow::CloseCompose() {
  if (ComposeWnd()) {
    // Filet : si une native traîne, la fermer émet l'annulation pour nous — ne pas
    // envoyer les deux, le serveur verrait une seconde annulation sans session.
    CloseWnd(kWriteId);
  } else {
    uint8_t packet[2] = {0};
    *reinterpret_cast<uint16_t*>(packet) = kCzCancelWrite;
    Bourgeon::Instance().SendPacket(packet, sizeof(packet));
  }
  ClearComposeState();
}

// Oublie la rédaction côté UI, sans rien émettre. Utilisé par l'annulation (après
// son paquet) comme par l'accusé d'envoi réussi (où annuler serait un contresens).
void RodexWindow::ClearComposeState() {
  compose_open_ = false;
  compose_items_.clear();
  send_error_.clear();
  tax_ = 0;
}

// ── Réception ───────────────────────────────────────────────────────────────
// Fil RÉSEAU : on COPIE, rien de plus (cf. features/net_inbox.h).
void RodexWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

// Fil PRINCIPAL : le décodage, rejoué au tick dans l'ordre d'arrivée.
void RodexWindow::HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  // ZC_ACK_OPEN_WRITE_MAIL (revendiqué) : le serveur autorise une rédaction. Le
  // prédicat a déjà écarté les refus, qui restent au natif pour son message d'erreur.
  if (opcode == kZcBeginWriteAck) {
    if (len < kBeginWriteResult + 1) return;
    if (compose_open_) return;  // même garde que le natif (g_MailWriteWnd == 0)
    // Les devoirs du handler remplacé, AVANT tout le reste : rendre puis vider.
    ResetMailAttachments();

    compose_open_ = true;
    compose_pos_ = true;
    compose_items_.clear();
    send_error_.clear();
    subject_[0] = '\0';
    body_[0] = '\0';
    check_state_ = kCheckNone;  // le résultat du courrier précédent n'a plus cours
    checked_name_.clear();
    checked_job_.clear();
    checked_level_ = 0;
    recipient_char_id_ = 0;
    attach_zeny_ = 0;
    tax_ = 0;
    // Destinataire : le serveur nous le renvoie dans l'ack (c'est celui qu'on lui a
    // demandé pour « Répondre », vide sinon). On le lisait avant dans l'UIEdit natif.
    char name[kBeginWriteNameLen + 1] = {0};
    std::memcpy(name, data, kBeginWriteNameLen);
    name[kBeginWriteNameLen] = '\0';
    const std::string utf8 = AnsiToUtf8(name);
    std::snprintf(to_, sizeof(to_), "%s", utf8.c_str());
    return;
  }

  // ZC_ACK_WRITE_MAIL (observé) : résultat de l'envoi. 0 = parti. Le handler natif
  // tourne toujours — c'est lui qui fait le ménage des emplacements après un succès.
  if (opcode == kZcSendResult) {
    if (len < 1) return;
    if (data[0] == 0) {
      ClearComposeState();  // surtout PAS CloseCompose : ce serait annuler après coup
    } else {
      send_error_ = i18n::Tr("Le serveur a refusé l'envoi.");
    }
  }
}

// Oublie la session de boîte aux lettres, sans toucher au natif. Efface aussi le
// courrier sélectionné DANS LE MANAGER, comme le faisait le bouton de fermeture natif.
void RodexWindow::ResetMailboxState() {
  ClearSelectedMailId();
  open_ = false;
  was_open_ = false;
  show_panel_ = true;
  selected_id_ = 0;
  confirm_ = kConfirmNone;
  mails_.clear();
}

void RodexWindow::CloseAll() {
  // Les deux natives sont normalement déjà détruites (OnTick) : ces fermetures sont
  // des filets, pour le cas où l'une aurait survécu à un tick manqué. Lecture AVANT
  // liste, l'ordre du natif. Ni l'une ni l'autre n'émet de paquet en se fermant
  // (vérifié : aucun envoi hors de leurs OnMsg de boutons).
  if (FindWnd(kReadId)) CloseWnd(kReadId);
  if (FindWnd(kInboxId)) CloseWnd(kInboxId);
  ResetMailboxState();
}

// ── Masquage du natif ───────────────────────────────────────────────────────

RodexWindow::RodexWindow() {
  // 🔴 On prend la place du SEUL créateur de la fenêtre d'écriture. Le prédicat voit
  // le paquet : on ne revendique que les ACCORDS (result == 1). Les refus repartent
  // au handler natif, qui affiche son message d'erreur — le remplacer entièrement
  // aurait fait disparaître ce message, la fenêtre n'étant de toute façon pas créée
  // dans ce cas-là.
  Bourgeon::Instance().RegisterReplaceOpcode(
      kZcBeginWriteAck, [this](const uint8_t* data, uint16_t len) {
        if (!imgui_enabled_ || !data || len < kBeginWriteResult + 1) return false;
        return data[kBeginWriteResult] == 1;
      });
  // Résultat d'envoi : OBSERVÉ, pas remplacé. Le handler natif doit continuer de
  // tourner (c'est lui qui vide les emplacements de pièces jointes après un succès) ;
  // on ne fait qu'écouter pour refermer notre fenêtre au bon moment.
  Bourgeon::Instance().RegisterObserveOpcode(kZcSendResult, 1);
}

// Une des trois natives vient de naître. On la masque SUR-LE-CHAMP (sans quoi une
// frame native passe à l'écran) ; c'est OnTick qui la détruira, le natif la
// manipulant encore ici.
//
// 🔴 DÉTRUIRE et pas seulement masquer : une native masquée reste vivante et garde
// le CLAVIER — Entrée/Espace valident son bouton par défaut invisible (cf.
// reference_native_window_toggle_router). Pour la LISTE s'ajoute la bascule
// « ferme si elle existe, sinon crée » : détruite, elle n'existe jamais, donc toute
// demande d'ouverture repasse forcément ici.
//
// Vérifié avant de détruire aussi tôt : la liste (`UIRodexWnd_ctor` 0x007cda3b) émet
// sa demande de courriers DÈS SA CONSTRUCTION — la requête est donc déjà partie quand
// on arrive ici — et ni sa fermeture ni celle de la fenêtre de lecture n'émet quoi
// que ce soit.
//
// 🔴 L'ÉCRITURE (0x108) ne doit JAMAIS être détruite : elle émet CZ 0x0A03 en se
// fermant, c'est-à-dire l'annulation de la rédaction. Elle ne naît d'ailleurs plus du
// tout (on a pris la place de son créateur, ZC 0x0A12) ; si elle apparaissait quand
// même — bascule de mode en pleine rédaction — on se contente de la masquer, et
// CloseCompose la fermera au bon moment, ce qui émettra l'annulation comme il faut.
void RodexWindow::HideNativeAtCreation(void* win, int window_id) {
  if (!win || !imgui_enabled_) return;
  if (window_id != kInboxId && window_id != kReadId && window_id != kWriteId) return;
  const uintptr_t vt = VTableOf(win);
  if (vt != kInboxVTable && vt != kReadVTable && vt != kWriteVTable)
    return;  // id réutilisé par une autre classe : on s'abstient
  HideWnd(win);
  if (window_id != kInboxId) return;

  // Reprend la position native : le client restaure la sienne à la création, la
  // fenêtre ImGui apparaît donc là où le joueur avait laissé sa boîte.
  if (WndScreenPos(win, &spawn_x_, &spawn_y_)) need_pos_ = true;

  // Reconstruction du HUD au changement de carte : ce n'est pas le joueur qui
  // demande, on ne touche donc pas à l'état.
  if (Bourgeon::Instance().IsMapLoading()) return;

  // C'est NOUS qui portons la bascule, la native étant détruite : le client ne la
  // voit jamais exister et redemande donc une création à chaque appui.
  if (open_) {
    // Deuxième appui : le joueur referme. On ne détruit PAS ici — le natif manipule
    // encore la fenêtre qu'il vient de créer ; OnTick s'en charge à la frame suivante.
    ResetMailboxState();
    return;
  }
  open_ = true;
  show_panel_ = true;
  selected_id_ = 0;
  confirm_ = kConfirmNone;
  // Le natif vient de demander la liste (son constructeur) : on date SA requête, pas
  // la nôtre, pour que « Chargement… » couvre bien cet aller-retour-là.
  list_requested_ms_ = GetTickCount();
}

void RodexWindow::OnTick() {
  // (Le décodage des paquets ne se fait plus ici : Bourgeon draine la file de tous
  // les modules à chaque frame, cf. Bourgeon::DrainNetInboxes. Au tick, l'accord de
  // rédaction pouvait attendre 100 ms avant d'être vu par le rendu.)

  if (!imgui_enabled_) {
    // Retour au natif. Aucune des trois n'existe : le client les recréera à la
    // prochaine demande. Une rédaction en cours, elle, n'a plus de fenêtre pour la
    // porter — on l'annule comme le ferait la croix, sinon le serveur resterait à
    // nous attendre avec les objets déjà mis de côté.
    if (compose_open_) CloseCompose();
    open_ = false;
    was_open_ = false;
    compose_open_ = false;
    mails_.clear();
    compose_items_.clear();
    return;
  }

  // Détour du handler de contenu, posé à la première activation : c'est lui qui
  // nous dit quels courriers ont réellement reçu leur corps/zeny/pièces jointes.
  // Sans lui on n'affiche aucun contenu — jamais de valeurs inventées.
  static bool hook_done = false;
  if (!hook_done) {
    hook_done = true;
    g_orig_ack_read = reinterpret_cast<RecvAckReadRodex_t>(
        hooking::HookManager::Instance().SetHook(
            hooking::HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(kRecvAckReadRodex),
            reinterpret_cast<uint8_t*>(&Detour_AckReadRodex)));
    if (!g_orig_ack_read)
      LogError("[Rodex] détour ZC 0x0B63 (0x%08X) NON installé : le contenu des "
               "courriers ne pourra pas être affiché.",
               static_cast<unsigned int>(kRecvAckReadRodex));
    g_orig_checkname_ack = reinterpret_cast<ApplyCheckNameAck_t>(
        hooking::HookManager::Instance().SetHook(
            hooking::HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(kApplyCheckNameAck),
            reinterpret_cast<uint8_t*>(&Detour_ApplyCheckNameAck)));
    if (!g_orig_checkname_ack)
      LogError("[Rodex] détour ZC 0x0A51 (0x%08X) NON installé : le métier et le "
               "niveau du destinataire ne seront pas affichés.",
               static_cast<unsigned int>(kApplyCheckNameAck));
    g_orig_delete_result = reinterpret_cast<RecvDeleteResult_t>(
        hooking::HookManager::Instance().SetHook(
            hooking::HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(kRecvDeleteResult),
            reinterpret_cast<uint8_t*>(&Detour_DeleteResult)));
    if (!g_orig_delete_result)
      LogError("[Rodex] détour de l'ack de suppression (0x%08X) NON installé : un "
               "courrier supprimé restera affiché jusqu'au rafraîchissement.",
               static_cast<unsigned int>(kRecvDeleteResult));
  }

  // Réponse de vérification du destinataire : arrive entre deux ticks.
  PollRecipientCheck();

  // Rédaction : l'état est désormais le NÔTRE (HandlePacket l'ouvre sur ZC 0x0A12 et
  // le referme sur ZC 0x09ED). La native ne naît plus ; si elle apparaissait quand
  // même, on se contente de la masquer — la DÉTRUIRE émettrait l'annulation.
  if (uint8_t* compose = ComposeWnd()) HideWnd(compose);
  if (compose_open_) ReadComposeState();

  // 🔴 La liste native ne porte PLUS l'état « la boîte est ouverte » : c'est
  // HideNativeAtCreation qui l'a adopté, et on la détruit ici. Elle n'a rien à faire
  // vivante — le handler de liste (ZC 0x0AC2) ne la cherche qu'en fin de parcours,
  // par un FindWindow qui rend nul sans rien perdre du modèle.
  void* inbox = FindWnd(kInboxId);
  if (inbox && VTableOf(inbox) == kInboxVTable) CloseWnd(kInboxId);

  // Idem pour la lecture, recréée par le serveur à CHAQUE lecture (handler ZC 0x0B63
  // -> MakeWindow 0x109). On la laisse naître — c'est ce handler-là qui remplit le
  // courrier dans la map, on ne peut pas s'en passer — mais on la détruit aussitôt :
  // son contenu est déjà lisible dans le manager, et masquée elle volerait le clavier.
  void* read = FindWnd(kReadId);
  if (read && VTableOf(read) == kReadVTable) CloseWnd(kReadId);

  was_open_ = open_;
  if (!open_) return;

  ReadState();
}

// ── Rendu ───────────────────────────────────────────────────────────────────

void RodexWindow::OnRenderUI() {
  if (!imgui_enabled_) return;
  // Recalculé par les cellules de pièce jointe des DEUX fenêtres : une seule peut
  // être survolée à la fois, un seul état suffit donc.
  hover_attach_valid_ = false;
  // La fenêtre d'écriture vit sa vie : elle peut être ouverte sans la boîte (réponse
  // depuis un courrier, puis fermeture de la liste) et se dessine donc à part.
  DrawComposeWindow();
  DrawMailbox();
  // L'aperçu de description est un TOOLTIP : il doit passer AU-DESSUS des fenêtres,
  // et se dessine donc après elles, hors de tout Begin/End (même règle que les
  // autres viewers). C'est aussi pourquoi la boîte est sortie d'ici : ses retours
  // anticipés sautaient cette ligne.
  DrawAttachTooltip();
}

void RodexWindow::DrawMailbox() {
  if (!open_) return;

  if (need_pos_) {
    // FirstUseEver et pas Appearing : la position native ne sert que d'amorce à
    // la toute première ouverture — ensuite c'est ImGui qui mémorise l'endroit où
    // le joueur a posé la fenêtre (comme l'échange et la boutique).
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(spawn_x_),
                                   static_cast<float>(spawn_y_)),
                            ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  // Hauteur de départ = la liste seule ; le bloc de lecture l'agrandit à la volée
  // (voir plus bas), et se rétracte d'autant quand on referme le courrier.
  ImGui::SetNextWindowSize(ImVec2(560, 268), ImGuiCond_FirstUseEver);

  // Ouverture/fermeture du bloc de lecture : on ajoute (ou retire) sa hauteur à
  // celle mesurée à la frame précédente. En DELTA, pour qu'un joueur qui a
  // redimensionné sa fenêtre garde sa taille — on ne fait que lui rendre la place
  // nécessaire à la lecture, sans jamais la réduire sous ce qu'il avait choisi.
  const bool want_detail = (selected_id_ != 0);
  if (want_detail != detail_shown_) {
    constexpr float kDetailHeight = 232.0f;  // hauteur du bloc « courrier ouvert »
    if (last_height_ > 0.0f)
      pending_height_ = want_detail ? last_height_ + kDetailHeight
                                    : last_height_ - kDetailHeight;
    detail_shown_ = want_detail;
  }
  if (pending_height_ > 0.0f && last_width_ > 0.0f) {
    // La largeur doit être REPASSÉE telle quelle : une composante à 0 demanderait à
    // ImGui un auto-fit sur cet axe, ce qui écraserait la largeur choisie.
    ImGui::SetNextWindowSize(ImVec2(last_width_, pending_height_), ImGuiCond_Always);
  }
  pending_height_ = 0.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);

  char title[96];
  if (unread_ > 0)
    std::snprintf(title, sizeof(title), "Courrier (%d non lu%s)###bourgeon_rodex",
                  unread_, unread_ > 1 ? "s" : "");
  else
    std::snprintf(title, sizeof(title), "Courrier###bourgeon_rodex");

  // BeginRoWindow inscrit lui-même la fenêtre dans la pile Échap (imgui_escape.h) :
  // pas de CloseWindowOnEscape ici, ce serait un doublon dans la pile.
  const bool begun = ro::BeginRoWindow(title, &show_panel_, ImGuiWindowFlags_NoCollapse);
  if (!show_panel_) {  // clic sur le X : on ferme la boîte comme le natif
    CloseAll();
    ro::EndRoWindow();
    ImGui::PopStyleVar(3);
    return;
  }
  if (!begun) {
    ro::EndRoWindow();
    ImGui::PopStyleVar(3);
    return;
  }

  DrawMailList();
  if (selected_id_) {
    ImGui::Separator();
    DrawMailDetail();
  } else {
    ImGui::TextDisabled("%s", i18n::Tr("Clique sur un courrier pour le lire."));
  }

  // Mesure de fin de frame : c'est elle qui sert de base au calcul d'agrandissement
  // ci-dessus, et elle suit donc naturellement les redimensionnements manuels.
  last_width_  = ImGui::GetWindowWidth();
  last_height_ = ImGui::GetWindowHeight();

  ro::EndRoWindow();
  ImGui::PopStyleVar(3);
}

void RodexWindow::DrawMailList() {
  const ImVec4 kBlack(0.0f, 0.0f, 0.0f, 1.0f);
  EnsureAttachIcons();

  // Actions de BOÎTE en tête : elles ne dépendent pas de l'onglet courant, et les
  // placer avant la barre d'onglets marque cette portée — tout ce qui suit les
  // onglets, lui, appartient à la boîte sélectionnée.
  if (ro::RoButton(i18n::Tr("Rafraîchir"))) RequestRefresh();
  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Écrire"))) Compose(nullptr);
  ImGui::SameLine();
  ImGui::TextDisabled(i18n::Tr("%d courrier%s"), static_cast<int>(mails_.size()),
                      mails_.size() > 1 ? "s" : "");

  // Onglets = les trois boîtes du manager, plus une vue agrégée. Changer d'onglet
  // pose aussi g_RodexMgr+0x10 : les handlers natifs (ack de lecture, de
  // suppression) s'en servent pour savoir dans quelle map écrire. Vraie barre
  // d'onglets ImGui : le skin RO l'habille déjà (ImGuiCol_Tab / TabSelected sont
  // poussés par BeginRoWindow) et l'onglet actif se lit d'un coup d'œil.
  int counts[4] = {0, 0, 0, 0};
  for (const Mail& mail : mails_) {
    if (mail.box < 3) ++counts[mail.box];
    ++counts[3];
  }

  // L'onglet « Compte » (MAIL_INBOX_ACCOUNT) n'est alimenté par AUCUN chemin du
  // serveur : ni mail_send ni le char-server ne posent ce type, seul un INSERT SQL
  // avec type = 1 le remplirait. Il resterait donc éternellement à (0). On le
  // masque tant qu'il est vide — mais on le ressort dès qu'il contient quelque
  // chose, sinon un courrier réellement reçu deviendrait inaccessible.
  const bool show_account = counts[1] > 0;
  if (!show_account && tab_ == 1) {
    tab_ = 0;  // dernier courrier du compte récupéré : l'onglet s'efface sous nous
    selected_id_ = 0;
    confirm_ = kConfirmNone;
    SetOpenType(0);
  }

  static const char* kTabs[4] = {"Reçus", "Compte", "Retournés", "Tous"};
  if (ImGui::BeginTabBar("rodex_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
    for (int tab = 0; tab < 4; ++tab) {
      if (tab == 1 && !show_account) continue;
      // ### : le compteur change à chaque courrier reçu, l'identité de l'onglet
      // ne doit pas changer avec lui (sinon ImGui perd l'onglet sélectionné).
      char label[64];
      std::snprintf(label, sizeof(label), "%s (%d)###rodex_tab%d", i18n::Tr(kTabs[tab]),
                    counts[tab], tab);
      if (ImGui::BeginTabItem(label)) {
        if (tab_ != tab) {
          tab_ = tab;
          selected_id_ = 0;
          confirm_ = kConfirmNone;
          if (tab < 3) SetOpenType(tab);
        }
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }

  // Filtre : équivalent du bouton « rechercher » natif. Il porte sur l'expéditeur
  // ET le sujet — c'est par l'un ou l'autre qu'on retrouve un courrier, et obliger
  // à choisir la colonne ne ferait qu'ajouter un clic.
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##rodex_filter", i18n::Tr("Filtrer par expéditeur ou sujet..."),
                           filter_, sizeof(filter_));
  const bool filtering = filter_[0] != '\0';

  // Courrier ouvert : la liste garde une hauteur fixe et le bloc de lecture occupe
  // la place gagnée par l'agrandissement. Sinon elle prend tout (0 = reste dispo).
  ImGui::BeginChild("rodex_list", ImVec2(0, selected_id_ ? 190.0f : 0.0f), true);
  if (mails_.empty()) {
    // « Chargement… » est borné dans le temps : au-delà, soit la boîte est
    // réellement vide, soit la réponse s'est perdue — dans les deux cas, une attente
    // sans fin ne dit rien au joueur. Le bouton « Rafraîchir » reste à sa portée.
    const bool loading =
        list_requested_ms_ != 0 && (GetTickCount() - list_requested_ms_) < 3000;
    ImGui::TextDisabled("%s", loading ? "Chargement..." : i18n::Tr("Aucun courrier."));
  } else if (ImGui::BeginTable("rodex_table", 4,
                               ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 26.0f);
    ImGui::TableSetupColumn(i18n::Tr("Expéditeur"), ImGuiTableColumnFlags_WidthStretch, 0.34f);
    ImGui::TableSetupColumn(i18n::Tr("Sujet"), ImGuiTableColumnFlags_WidthStretch, 0.50f);
    ImGui::TableSetupColumn(i18n::Tr("Expire"), ImGuiTableColumnFlags_WidthFixed, 52.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    int shown = 0;
    for (const Mail& mail : mails_) {
      if (tab_ < 3 && mail.box != tab_) continue;
      if (filtering && !ContainsNoCase(mail.sender.c_str(), filter_) &&
          !ContainsNoCase(mail.title.c_str(), filter_))
        continue;
      ++shown;
      ImGui::TableNextRow();
      ImGui::PushID(static_cast<int>(mail.id));

      // Colonne 1 : ce que le courrier transporte encore. Le type est un masque
      // que le serveur remet à jour à chaque récupération (&4 objets, &2 zeny),
      // c'est donc bien « reste à récupérer » et pas « contenait ».
      ImGui::TableNextColumn();
      const ro::GameTexture* icon = nullptr;
      const char* mark = "-";      // repli si le .bmp du jeu est introuvable
      const char* what = nullptr;
      if ((mail.type & 4) && (mail.type & 2)) {
        icon = &g_ico_both;
        mark = "#";
        what = i18n::Tr("Objets et zeny à récupérer");
      } else if (mail.type & 4) {
        icon = &g_ico_item;
        mark = "#";
        what = i18n::Tr("Objets à récupérer");
      } else if (mail.type & 2) {
        icon = &g_ico_zeny;
        mark = "z";
        what = i18n::Tr("Zeny à récupérer");
      }
      if (icon && icon->tex) {
        // Hauteur alignée sur la ligne de texte, largeur au ratio : ces .bmp ne
        // font pas tous la même taille et un carré forcé les déformerait.
        const float h = ImGui::GetTextLineHeight();
        const float w = (icon->h > 0) ? h * icon->w / icon->h : h;
        ImGui::Image(reinterpret_cast<ImTextureID>(icon->tex), ImVec2(w, h));
      } else if (what) {
        ImGui::TextColored(kBlack, "%s", mark);
      } else {
        ImGui::TextDisabled("%s", mark);
      }
      if (what && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", what);

      ImGui::TableNextColumn();
      const bool selected = (selected_id_ == mail.id && selected_box_ == mail.box);
      if (ImGui::Selectable(mail.sender.empty() ? "(inconnu)" : mail.sender.c_str(),
                            selected, ImGuiSelectableFlags_SpanAllColumns)) {
        selected_id_  = mail.id;
        selected_box_ = mail.box;
        confirm_ = kConfirmNone;
        // Le contenu n'arrive qu'à la demande, et il ne survit pas à la session :
        // un courrier lu hier revient avec un nœud vierge. On le réclame donc dès
        // qu'il n'est pas déjà arrivé, pas seulement quand il est « non lu ».
        if (!mail.content_ready) OpenMail(mail);
      }

      ImGui::TableNextColumn();
      // Sujet grisé = déjà lu, comme la liste native. C'est bien l'état SERVEUR qui
      // compte ici (il traverse les sessions), pas la présence locale du contenu.
      if (mail.is_read) ImGui::TextDisabled("%s", mail.title.c_str());
      else              ImGui::TextColored(kBlack, "%s", mail.title.c_str());

      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", ExpiryLabel(mail.expire).c_str());
      ImGui::PopID();
    }
    ImGui::EndTable();
    // Table vide alors que la boîte ne l'est pas : sans un mot, on croit avoir tout
    // perdu. On distingue « le filtre ne trouve rien » de « cet onglet est vide ».
    if (shown == 0)
      ImGui::TextDisabled("%s", filtering ? i18n::Tr("Aucun courrier ne correspond.") : i18n::Tr("Aucun courrier dans cet onglet."));
  }
  ImGui::EndChild();
}

// ── Une pièce jointe : icône + nom complet, survol, clic droit ───────────────
// Même cellule pour la lecture et la rédaction — c'est le même objet, seul le
// bouton « Retirer » distingue les deux. Le groupe englobe icône, nom et quantité
// pour que la zone sensible couvre toute la ligne et pas le seul libellé.
void RodexWindow::DrawAttachRow(const Attach& attach, bool removable) {
  const ImVec4 kBlack(0.0f, 0.0f, 0.0f, 1.0f);

  ImGui::BeginGroup();
  const ro::IconTex icon = ro::ItemIcon(attach.id, attach.identified ? 1 : 0);
  if (icon.tex) {
    ImGui::Image(reinterpret_cast<ImTextureID>(icon.tex), ImVec2(20, 20));
    ImGui::SameLine();
  }
  // Nom COMPLET : refine et affixes de cartes viennent du name-builder natif
  // (déjà dans `name`), le suffixe « [N] » est ajouté ici — le builder ne le
  // compose pas. L'ombre rouge marque l'équipement cassé, comme partout ailleurs.
  char label[128];
  itemcell::Label(label, sizeof(label), attach.name, attach.total_slots);
  ImGui::PushStyleColor(ImGuiCol_Text, kBlack);
  itemcell::NameText(label, attach.damaged);
  ImGui::PopStyleColor();
  if (attach.amount > 1) {
    ImGui::SameLine();
    ImGui::TextDisabled("x%d", attach.amount);
  }
  ImGui::EndGroup();

  if (ImGui::IsItemHovered()) {
    // L'aperçu est un popup : on note seulement quoi afficher, le tooltip sera
    // dessiné hors de toute fenêtre (cf. DrawAttachTooltip).
    hover_attach_ = attach;
    hover_attach_valid_ = true;
  }
  if (mui::IsLastItemRightClicked()) {
    POINT pt;
    if (GetCursorPos(&pt)) {
      // `src` doit survivre jusqu'au relâchement du bouton (contrat de
      // DeferDescById) : le slot de rédaction est à une adresse de session fixe,
      // une pièce jointe reçue passe par le tampon membre. Quand `src` est fourni,
      // view/location du paramètre sont ignorés — ils sont dans la structure.
      if (attach.mail_slot >= 0)
        itemcell::DeferDescById(attach.id, attach.view, attach.location, pt.x, pt.y,
                                AttachSlotAddr(attach.mail_slot));
      else if (BuildAttachInfo(attach, desc_scratch_))
        itemcell::DeferDescById(attach.id, attach.view, attach.location, pt.x, pt.y,
                                desc_scratch_);
    }
  }

  if (removable) {
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Retirer"))) RemoveAttachment(attach.inv_index, attach.amount);
  }
}

void RodexWindow::DrawAttachTooltip() {
  if (!hover_attach_valid_) return;
  const Attach& attach = hover_attach_;
  itemdesc::SimpleOpt opts[kItOptMax];
  for (int i = 0; i < attach.opt_count && i < kItOptMax; ++i) {
    opts[i].index = attach.opts[i].index;
    opts[i].value = attach.opts[i].value;
    opts[i].param = attach.opts[i].param;
  }
  // Sur un item FORGÉ, les quatre mots de cartes portent en fait les données du
  // forgeron (charid scindé, star crumbs, élément) : les afficher comme des cartes
  // inventerait quatre objets. Même critère que l'inventaire et la description.
  const bool forged = (attach.cards[0] != 0 && attach.cards[0] <= 500);
  itemcell::DrawTooltip(attach.id, attach.cards, forged ? 0 : 4, opts,
                        attach.opt_count, attach.refine, attach.name,
                        attach.damaged);
}

void RodexWindow::DrawMailDetail() {
  const ImVec4 kBlack(0.0f, 0.0f, 0.0f, 1.0f);
  const Mail* mail = Selected();
  if (!mail) {
    // Le courrier sélectionné peut disparaître entre deux frames (suppression,
    // retour à l'expéditeur) : plus de cible pour la confirmation, et la fenêtre se
    // rétracte d'elle-même à la frame suivante puisque plus rien n'est sélectionné.
    confirm_ = kConfirmNone;
    selected_id_ = 0;
    return;
  }

  ImGui::TextColored(kBlack, i18n::Tr("De : %s"), mail->sender.c_str());
  const std::string expiry = ExpiryLabel(mail->expire);
  if (!expiry.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled(i18n::Tr("(expire : %s)"), expiry.c_str());
  }
  ImGui::TextColored(kBlack, i18n::Tr("Sujet : %s"), mail->title.c_str());

  ImGui::BeginChild("rodex_body", ImVec2(0, 86), true);
  if (!mail->content_ready)
    ImGui::TextDisabled(i18n::Tr("Chargement du message..."));
  else if (mail->body.empty())
    ImGui::TextDisabled(i18n::Tr("(message vide)"));
  else
    ImGui::TextWrapped("%s", mail->body.c_str());
  ImGui::EndChild();

  // ── Pièces jointes ──
  const bool has_zeny  = (mail->type & 2) != 0;
  const bool has_items = (mail->type & 4) != 0;
  if (has_zeny || has_items) {
    // Le DÉTAIL (montant, objets) n'existe qu'une fois le contenu reçu : avant ça,
    // seul le type nous dit qu'il transporte quelque chose. On annonce donc la pièce
    // jointe sans inventer de valeur — les octets du nœud sont du bruit de pile.
    if (has_zeny && mail->content_ready)
      ImGui::TextColored(kBlack, i18n::Tr("Zeny joint : %lld z"),
                         static_cast<long long>(mail->zeny));
    else if (has_zeny)
      ImGui::TextDisabled("%s", i18n::Tr("Zeny joint (ouvre le courrier pour voir le montant)"));
    for (size_t i = 0; i < mail->items.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      DrawAttachRow(mail->items[i], false);
      ImGui::PopID();
    }
    if (has_items && !mail->content_ready)
      ImGui::TextDisabled("%s", i18n::Tr("Objets joints (ouvre le courrier pour les voir)"));
    if (ro::RoButton(i18n::Tr("Tout récupérer"), 150.0f, 0.0f)) ClaimAttachments(*mail);
  }

  // ── Actions ──
  if (ro::RoButton(i18n::Tr("Répondre"), 110.0f, 0.0f))
    Compose(mail->sender_raw.empty() ? nullptr : mail->sender_raw.c_str());
  ImGui::SameLine();
  // Retourner à l'expéditeur : le natif ne le propose que dans la boîte de
  // réception (un courrier de compte ou déjà retourné n'a pas d'émetteur joignable).
  const bool can_return = (mail->box == 0);
  if (!can_return) ImGui::BeginDisabled();
  if (ro::RoButton(i18n::Tr("Retourner"), 110.0f, 0.0f)) confirm_ = kConfirmReturn;
  if (!can_return) ImGui::EndDisabled();
  ImGui::SameLine();
  // Comme le natif : suppression interdite tant qu'il reste une pièce jointe (le
  // serveur refuserait de toute façon, avec un message d'erreur dans le chat).
  const bool can_delete = (mail->type & 6) == 0;
  if (!can_delete) ImGui::BeginDisabled();
  if (ro::RoButton(i18n::Tr("Supprimer"), 110.0f, 0.0f)) confirm_ = kConfirmDelete;
  if (!can_delete) ImGui::EndDisabled();
  if (!can_delete) ImGui::TextDisabled(i18n::Tr("Récupère d'abord les pièces jointes."));

  DrawConfirmPopup();
}

// ── Écriture d'un courrier ──────────────────────────────────────────────────
// Surcouche de la fenêtre native (masquée) : la saisie est à nous, mais l'ouverture,
// les pièces jointes et l'annulation restent des commandes natives — le serveur ne
// distingue pas notre client d'un client vanilla.
void RodexWindow::DrawComposeWindow() {
  if (!compose_open_) return;

  const ImVec4 kBlack(0.0f, 0.0f, 0.0f, 1.0f);
  const ImVec4 kRed(0.75f, 0.15f, 0.15f, 1.0f);

  if (compose_pos_) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    compose_pos_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(430, 470), ImGuiCond_FirstUseEver);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);

  bool open = true;
  const bool begun = ro::BeginRoWindow(i18n::Tr("Écrire un courrier###bourgeon_rodex_write"),
                                       &open, ImGuiWindowFlags_NoCollapse);
  if (!open) {  // clic sur le X = annuler l'écriture (le natif libère les objets)
    CloseCompose();
    ro::EndRoWindow();
    ImGui::PopStyleVar(3);
    return;
  }
  if (!begun) {
    ro::EndRoWindow();
    ImGui::PopStyleVar(3);
    return;
  }

  // ── Destinataire ──
  ImGui::TextColored(kBlack, i18n::Tr("Destinataire"));
  ImGui::SetNextItemWidth(200.0f);
  ImGui::InputText("##rodex_to", to_, sizeof(to_));
  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Vérifier"))) CheckRecipient();

  // Métier + niveau du destinataire : c'est ce qui permet de reconnaître la bonne
  // personne avant d'envoyer des objets. Le char id, lui, ne dit rien au joueur.
  switch (check_state_) {
    case kCheckPending:
      ImGui::TextDisabled("%s", i18n::Tr("Vérification en cours..."));
      break;
    case kCheckUnknown:
      ImGui::TextColored(ImVec4(0.75f, 0.15f, 0.15f, 1.0f), "%s",
                         i18n::Tr("Ce personnage n'existe pas."));
      break;
    case kCheckFound: {
      // Le serveur réémet le nom qu'il a trouvé : l'afficher plutôt que la saisie
      // permet de repérer une coquille (majuscule, espace) sans relire son champ.
      const char* who = checked_name_.empty() ? to_ : checked_name_.c_str();
      if (!checked_job_.empty())
        ImGui::TextColored(kBlack, i18n::Tr("%s — %s, niveau %d"), who, checked_job_.c_str(),
                           checked_level_);
      else
        ImGui::TextColored(kBlack, i18n::Tr("%s — niveau %d"), who, checked_level_);
      break;
    }
    default:
      break;
  }

  // ── Sujet / message ──
  ImGui::TextColored(kBlack, i18n::Tr("Sujet"));
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputText("##rodex_subject", subject_, sizeof(subject_));
  ImGui::TextColored(kBlack, i18n::Tr("Message"));
  ImGui::InputTextMultiline("##rodex_body", body_, sizeof(body_),
                            ImVec2(-1.0f, 130.0f));

  // ── Zeny joint + frais ──
  ImGui::TextColored(kBlack, i18n::Tr("Zeny à joindre"));
  ImGui::SetNextItemWidth(160.0f);
  ImGui::InputScalar("##rodex_zeny", ImGuiDataType_S64, &attach_zeny_);
  if (attach_zeny_ < 0) attach_zeny_ = 0;
  // Frais d'envoi : on AFFICHE ce que le client a calculé (fenêtre native +0xf8),
  // on ne le recalcule pas — le taux et le prix par pièce jointe sont de la config
  // SERVEUR (mail_zeny_fee, mail_attachment_price), qu'on n'a pas à dupliquer ici.
  // Tant que le natif n'a pas eu à le calculer, on se contente de prévenir.
  ImGui::SameLine();
  if (tax_ > 0) ImGui::TextDisabled(i18n::Tr("frais : %lld z"), static_cast<long long>(tax_));
  else          ImGui::TextDisabled(i18n::Tr("(des frais d'envoi s'appliquent)"));

  // ── Pièces jointes : cible de dépôt de l'inventaire ImGui ──
  ImGui::TextColored(kBlack, i18n::Tr("Pièces jointes (%d/%d)"),
                     static_cast<int>(compose_items_.size()), kAttachSlots);
  ImGui::BeginChild("rodex_attach", ImVec2(0, 92), true);
  for (size_t i = 0; i < compose_items_.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    DrawAttachRow(compose_items_[i], true);
    ImGui::PopID();
  }
  if (compose_items_.empty())
    ImGui::TextDisabled("%s", i18n::Tr("(vide — glisse un objet de l'inventaire ici)"));
  ImGui::EndChild();
  // Le child qu'on vient de fermer est le « dernier item » ImGui : c'est lui la
  // cible de dépôt. Payload « INV_ITEM » = convention de l'inventaire ImGui (même
  // que l'échange et le doll de la feuille de perso) ; l'InventoryViewer applique sa
  // propre politique de quantité (pile -> prompt, sinon 1 unité).
  if (ImGui::BeginDragDropTarget()) {
    if (ImGui::AcceptDragDropPayload("INV_ITEM")) {
      if (auto* inventory = Bourgeon::Instance().inventory_viewer())
        inventory->MailDraggedItem();
    }
    ImGui::EndDragDropTarget();
  }

  ImGui::Separator();
  if (ro::RoButton(i18n::Tr("Envoyer"), 130.0f, 0.0f)) SendMail();
  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Annuler"), 110.0f, 0.0f)) {
    CloseCompose();
    ro::EndRoWindow();
    ImGui::PopStyleVar(3);
    return;
  }
  if (!send_error_.empty()) ImGui::TextColored(kRed, "%s", send_error_.c_str());

  ro::EndRoWindow();
  ImGui::PopStyleVar(3);
}

// Confirmation modale partagée par « Supprimer » et « Retourner » (le natif en
// passe une aussi : ces deux actions sont irréversibles).
void RodexWindow::DrawConfirmPopup() {
  const Mail* mail = Selected();
  if (!mail) return;
  if (confirm_ != kConfirmNone) {
    ImGui::OpenPopup(i18n::Tr("Confirmation###rodex_confirm"));
    ro::SuppressEscapeStack();  // Échap ferme la modale, pas la boîte derrière
  }
  if (ro::BeginRoPopupModal(i18n::Tr("Confirmation###rodex_confirm"))) {
    ImGui::TextUnformatted(confirm_ == kConfirmDelete
                               ? i18n::Tr("Supprimer définitivement ce courrier ?") : i18n::Tr("Retourner ce courrier à son expéditeur ?"));
    ImGui::Spacing();
    if (ro::RoButton(i18n::Tr("Confirmer"), 110.0f, 0.0f)) {
      if (confirm_ == kConfirmDelete) DeleteMail(*mail);
      else                            ReturnMail(*mail);
      confirm_ = kConfirmNone;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler"), 110.0f, 0.0f)) {
      confirm_ = kConfirmNone;
      ImGui::CloseCurrentPopup();
    }
    ro::EndRoPopupModal();
  }
}
