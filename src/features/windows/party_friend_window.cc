#include "features/windows/party_friend_window.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

#include "bourgeon.h"
#include "imgui.h"
#include "ragnarok/globals.h"
#include "ragnarok/msgstring.h"      // msgstr::Utf8Or (libellés exacts du client)
#include "ragnarok/ui_window_mgr.h"  // UIM_MAKE_WHISPER_WINDOW (ouverture d'un 1:1)
#include "ragnarok/uiwnd.h"
#include "ui/game_texture.h"  // ro::CachedTextureFromGameFile (icônes du client)
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"

namespace {

// ── Le manager social : des champs de la SESSION ─────────────────────────────
// Les deux listes sont des std::list circulaires dont la session ne garde que le
// POINTEUR de sentinelle ; `next` est le premier dword du nœud et la donnée
// commence à `nœud+8`. Confirmé par trois chemins indépendants : les accesseurs
// (0x00d5a0d0 / 0x00d5da80), le vidage de session (0x00d70220) et le départ de
// groupe (0x00d56530). Cf. docs/party_friend_re.md §2.
constexpr int kSes_PartyListPtr  = 0x17bc;
constexpr int kSes_PartyCount    = 0x17c0;
constexpr int kSes_FriendListPtr = 0x17c4;
constexpr int kSes_FriendCount   = 0x17c8;

// ── L'entrée sociale (0x50 octets, MÊME type pour les amis et le groupe) ─────
constexpr int kNode_Data   = 0x08;  // la donnée commence après {next, prev}
constexpr int kEnt_Gid     = 0x04;
constexpr int kEnt_Id2     = 0x08;
constexpr int kEnt_Name    = 0x0c;  // std::string
constexpr int kEnt_Map     = 0x24;  // std::string
constexpr int kEnt_Leader  = 0x3c;  // 🔴 0 = CHEF (le natif dessine la couronne si == 0)
constexpr int kEnt_Offline = 0x40;
constexpr int kEnt_Color   = 0x44;
constexpr int kEnt_Job     = 0x48;  // u16
constexpr int kEnt_Level   = 0x4a;  // u16 — prouvé au désassemblage (cf. l'en-tête)

// std::string MSVC : buffer SSO de 16 o, puis taille et capacité.
// (7e exemplaire de ce lecteur dans le dépôt — il mériterait la factorisation
// qu'a reçue `uiwnd.h` pour les fenêtres. Hors périmètre de ce chantier.)
constexpr int kStr_Cap = 0x14;

// ── L'acteur, pour le HP ─────────────────────────────────────────────────────
// `Actor_FindByGid(gid)` __stdcall : raccourci global qui résout le mode lui-même
// (le même que target_frame). La `UIPcGage` de +0x488 est celle que le client pose
// justement pour LES MEMBRES DE PARTY ; PV courants en +0xA0, maximum en +0xA4.
constexpr uintptr_t kActorFindByGid = 0x00d806a0;
constexpr int       kAct_PcGage     = 0x488;
constexpr int       kGage_Hp        = 0x0a0;
constexpr int       kGage_MaxHp     = 0x0a4;

// Mes propres PV : le natif les lit dans ces deux globales plutôt que sur mon
// acteur (cf. UpdateMemberHpGauges, branche `aid == g_Account_Aid`).
constexpr uintptr_t kOwnHp    = 0x015ff908;
constexpr uintptr_t kOwnMaxHp = 0x015ff90c;
constexpr uintptr_t kOwnAid   = 0x015fb9a4;
// (`g_Own_InParty` 0x015FF804 existe et garde les cases 0x3D / 0x3E du switch
// natif, mais il reste à 0 pour un membre qui a REJOINT un groupe — inutilisable
// pour savoir si l'on est en groupe. Voir DrawPartyTab.)

// Résolveur de nom de classe, même convention d'appel que celle déjà éprouvée par
// character_sheet : __fastcall(session /*ecx*/, nullptr /*edx*/, jobId, -1).
constexpr uintptr_t kJobDisplayName = 0x00d5bb40;

constexpr int kWinMessengerGroup = 0x45;

// Le natif dimensionne 40 jauges et 40 boutons de job : c'est sa borne de lignes.
// On garde la même, avec une marge, pour ne jamais boucler sans fin sur une liste
// corrompue.
constexpr int kMaxRows = 64;

// 🔴 MAX_PARTY = 24 SUR MOONLIGHT (`src/common/mmo.hpp:99`), PAS 12.
//
// Le client, lui, pousse `0Ch` en littéral dans le compteur de sa fenêtre native
// (@0x00704820) : il affiche donc « N/12 » et se trompera dès le 13e membre. On ne
// reproduit pas ce plafond-là — on prend celui du SERVEUR, qui est la vraie limite.
//
// ⚠ Ne pas déduire cette valeur d'un autre émulateur : Hercules dit 12, rAthena
// amont aussi. C'est bien le dépôt Moonlight qui fait foi (miroir local
// `D:\Mes documents\GitHub\moonlight`, serveur `~/moonlight` en SSH).
constexpr int kMaxPartyMembers = 24;

// MAX_FRIENDS, même source : Moonlight `src/common/mmo.hpp:168`.
// ⚠ Contrairement à MAX_PARTY, le CLIENT ne connaît pas cette valeur — elle est
// purement serveur, et nous la recopions ici. Si elle change côté serveur, ce
// nombre-là ment jusqu'à ce qu'on le suive : c'est le seul endroit à corriger.
constexpr int kMaxFriends = 40;

// ── Lectures brutes, toutes sous SEH ─────────────────────────────────────────
// 🔴 Aucune de ces fonctions ne manipule d'objet à destructeur : MSVC refuse
// `__try` dans une fonction qui demande du déroulement. La conversion vers des
// std::string se fait chez l'appelant, hors du bloc protégé.

struct RawRow {
  uint32_t gid = 0, id2 = 0, color = 0;
  uint16_t job = 0, level = 0;
  bool     leader = false;
  bool     offline = false;
  char     name[64] = {0};
  char     map[32]  = {0};
};

void ReadStdStringSEH(uintptr_t addr, char* out, int cap) {
  out[0] = '\0';
  __try {
    const uint8_t* s = reinterpret_cast<const uint8_t*>(addr);
    const uint32_t capacity = *reinterpret_cast<const uint32_t*>(s + kStr_Cap);
    const char* p = (capacity > 15) ? *reinterpret_cast<const char* const*>(s)
                                    : reinterpret_cast<const char*>(s);
    if (p) {
      int i = 0;
      for (; i < cap - 1 && p[i]; ++i) out[i] = p[i];
      out[i] = '\0';
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

// Collecte les NŒUDS de la liste (pointeurs seulement). Renvoie le nombre lu.
// Trois bornes, et c'est voulu : la sentinelle (fin normale du tour), le compteur
// que la session tient à côté de la liste, et `cap`. Le compteur seul ne suffirait
// pas — il peut être en avance ou en retard d'un élément le temps d'un ajout — et
// la sentinelle seule laisserait boucler sans fin sur une liste remaniée pendant
// qu'on la parcourt.
int CollectNodesSEH(int list_ptr_offset, int count_offset, const void** nodes,
                    int cap) {
  int n = 0;
  __try {
    const uintptr_t sentinel = rag::SessionField<uintptr_t>(list_ptr_offset);
    if (!sentinel) return 0;
    const int announced = rag::SessionField<int>(count_offset);
    int limit = cap;
    if (announced > 0 && announced < limit) limit = announced;
    uintptr_t node = *reinterpret_cast<const uintptr_t*>(sentinel);
    while (node && node != sentinel && n < limit) {
      nodes[n++] = reinterpret_cast<const void*>(node);
      node = *reinterpret_cast<const uintptr_t*>(node);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { /* liste en cours de remaniement */ }
  return n;
}

bool ReadNodeSEH(const void* node, RawRow& out) {
  uintptr_t data = 0;
  __try {
    data = reinterpret_cast<uintptr_t>(node) + kNode_Data;
    out.gid     = *reinterpret_cast<const uint32_t*>(data + kEnt_Gid);
    out.id2     = *reinterpret_cast<const uint32_t*>(data + kEnt_Id2);
    out.color   = *reinterpret_cast<const uint32_t*>(data + kEnt_Color);
    out.job     = *reinterpret_cast<const uint16_t*>(data + kEnt_Job);
    // 🔴 Le natif code le chef par ZÉRO, pas par un.
    out.leader  = (*reinterpret_cast<const uint32_t*>(data + kEnt_Leader) == 0);
    out.offline = (*reinterpret_cast<const uint32_t*>(data + kEnt_Offline) != 0);
    out.level   = *reinterpret_cast<const uint16_t*>(data + kEnt_Level);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  ReadStdStringSEH(data + kEnt_Name, out.name, sizeof(out.name));
  ReadStdStringSEH(data + kEnt_Map,  out.map,  sizeof(out.map));
  return true;
}

// PV d'un membre. Rend false quand l'acteur n'est pas chargé — ce qui n'est PAS
// une erreur : c'est l'état normal d'un membre hors de portée, et le client
// officiel n'affiche rien non plus dans ce cas.
bool ReadHpSEH(uint32_t gid, int* hp, int* max_hp) {
  __try {
    if (gid && gid == *reinterpret_cast<const uint32_t*>(kOwnAid)) {
      *hp     = *reinterpret_cast<const int*>(kOwnHp);
      *max_hp = *reinterpret_cast<const int*>(kOwnMaxHp);
      return *max_hp > 0;
    }
    using FindActorFn = void* (__stdcall*)(uint32_t);
    void* actor = reinterpret_cast<FindActorFn>(kActorFindByGid)(gid);
    if (!actor) return false;
    void* gage = *reinterpret_cast<void* const*>(
        reinterpret_cast<const uint8_t*>(actor) + kAct_PcGage);
    if (!gage) return false;
    const uint8_t* g = reinterpret_cast<const uint8_t*>(gage);
    const int cur = *reinterpret_cast<const int*>(g + kGage_Hp);
    const int max = *reinterpret_cast<const int*>(g + kGage_MaxHp);
    if (max <= 0) return false;
    *hp = cur;
    *max_hp = max;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ── Les commandes du client ──────────────────────────────────────────────────
//
// `CMode::SendMsg(cmd, p2..p5)` sur le mode de zone courant, vtbl+0x18. Rend
// false si aucun mode n'est actif (login, changement de carte).
// (Ce petit pont est déjà recopié dans chat_window et game_settings ; il mériterait
// la factorisation qu'a reçue `uiwnd.h`. Hors périmètre de ce chantier.)
constexpr uintptr_t kCurrentModePtr = 0x0121333c;
constexpr int       kSendMsgVtOff   = 0x18;

bool ModeSendMsg(int cmd, int p2 = 0, int p3 = 0, int p4 = 0, int p5 = 0) {
  __try {
    void* mode = *reinterpret_cast<void**>(kCurrentModePtr);
    if (mode == nullptr) return false;
    using SendMsg_t = int(__thiscall*)(void*, int, int, int, int, int);
    void** vt = *reinterpret_cast<void***>(mode);
    reinterpret_cast<SendMsg_t>(vt[kSendMsgVtOff / 4])(mode, cmd, p2, p3, p4, p5);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

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

// Nombre de membres, directement au manager — la même fonction que chat_window et
// emotion_hotkey utilisent déjà. Sert de garde « suis-je en groupe ? » hors rendu,
// là où la liste relue par frame n'est pas disponible.
constexpr uintptr_t kPartyMemberCountFn = 0x00d5cf50;  // __thiscall(session)

int PartyMemberCountSEH() {
  __try {
    using Fn = int(__thiscall*)(void*);
    return reinterpret_cast<Fn>(kPartyMemberCountFn)(
        reinterpret_cast<void*>(rag::kSessionAddr));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

int ReadOptSEH(uintptr_t addr) {
  __try {
    return *reinterpret_cast<const int*>(addr);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

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
constexpr uintptr_t kFriendListAddByName = 0x00a2c600;

void AddFriendSEH(const char* name24) {
  __try {
    using FriendAddFn = int(__stdcall*)(const void*);
    reinterpret_cast<FriendAddFn>(kFriendListAddByName)(name24);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Mon AID. C'est ce que le natif compare pour choisir `icon_party_me`.
uint32_t OwnAidSEH() {
  __try {
    return *reinterpret_cast<const uint32_t*>(kOwnAid);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

const char* JobNameSEH(int job_id) {
  __try {
    using GetClassName_t = const char* (__fastcall*)(void*, void*, unsigned, int);
    const char* n = reinterpret_cast<GetClassName_t>(kJobDisplayName)(
        reinterpret_cast<void*>(rag::kSessionAddr), nullptr,
        static_cast<unsigned>(job_id), -1);
    return n ? n : "";
  } __except (EXCEPTION_EXECUTE_HANDLER) { return ""; }
}

// Le résolveur passe par la table Lua des classes : on met en cache, comme le fait
// character_sheet pour les membres de guilde.
const char* JobName(int job_id) {
  static std::unordered_map<int, std::string> cache;
  auto it = cache.find(job_id);
  if (it != cache.end()) return it->second.c_str();
  const char* n = JobNameSEH(job_id);
  char buf[64];
  if (n && n[0]) {
    std::strncpy(buf, n, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
  } else {
    std::snprintf(buf, sizeof(buf), i18n::Tr("Classe %d"), job_id);
  }
  return (cache[job_id] = buf).c_str();
}

// ── L'art du client, tel que le natif le compose ─────────────────────────────
//
// Racine des bitmaps d'interface, en CP949 (유저인터페이스), en octets verbatim :
// ce fichier est en UTF-8 et le client attend SA code-page.
constexpr char kUiRoot[] = "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA";

// L'icône de classe. Le natif la pose sur ses 40 boutons de job (50×50) au msg
// 0x17 : `sprintf("%sicon_jobs_%d.bmp", "\renewalparty\", job)` @0x0070622a, où
// le job est lu en `[esi+50h]` — nœud+0x50, donc data+0x48. La variante `_die`
// existe (mort), pilotée par un flag distinct de « hors ligne » que nous n'avons
// pas encore identifié : on ne l'utilise donc pas.
constexpr char kJobIconFmt[] = "%s\\renewalparty\\icon_jobs_%u.bmp";
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
  dl->AddText(ImVec2(p.x + pad_x, p.y + pad_y), IM_COL32(255, 255, 255, 255), txt);
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

void PartyFriendWindow::ReadList(bool party, std::vector<SocialRow>& out) {
  out.clear();
  const void* nodes[kMaxRows] = {nullptr};
  const int n = CollectNodesSEH(party ? kSes_PartyListPtr : kSes_FriendListPtr,
                                party ? kSes_PartyCount : kSes_FriendCount,
                                nodes, kMaxRows);
  out.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    RawRow raw;
    if (!ReadNodeSEH(nodes[i], raw)) continue;
    SocialRow row;
    row.gid       = raw.gid;
    row.id2       = raw.id2;
    row.name      = raw.name;
    row.map       = raw.map;
    row.is_leader = raw.leader;
    row.offline   = raw.offline;
    row.color     = raw.color;
    row.job       = raw.job;
    row.level     = raw.level;
    if (party) FillHp(row);
    out.push_back(std::move(row));
  }
  // Suis-je le chef ? C'est ce qui ouvre « nommer chef » et « expulser ». On le
  // recalcule à chaque relecture : le commandement peut changer sans nous
  // prévenir (le chef part, le serveur le transmet).
  if (party) {
    const uint32_t mine = OwnAidSEH();
    i_am_leader_ = false;
    for (const SocialRow& r : out) {
      if (r.gid == mine) { i_am_leader_ = r.is_leader; break; }
    }
  }
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
      ModeSendMsg(kCmdMakeLeader, static_cast<int>(pending_gid_));
      break;
    case Action::kRemoveFriend:
      ModeSendMsg(kCmdRemoveFriend, static_cast<int>(pending_gid_),
                  static_cast<int>(pending_id2_));
      break;
    case Action::kLeaveParty:
      // 🔴 On passe par la COMMANDE, pas par le paquet : le case 0x3D ne se
      // contente pas d'envoyer 0x0100, il cherche d'abord un membre en ligne sur
      // ma carte et lui TRANSFÈRE le leadership. Envoyer le paquet nu laisserait
      // le groupe sans chef, là où le client officiel passe la main.
      ModeSendMsg(kCmdLeaveParty);
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
      ModeSendMsg(kCmdPartyInvite,
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
      ModeSendMsg(kCmdAnswerParty, static_cast<int>(pending_gid_),
                  pending_accept_ ? 1 : 0);
      break;
    case Action::kAnswerFriend:
      ModeSendMsg(kCmdAnswerFriend, static_cast<int>(pending_gid_),
                  static_cast<int>(pending_id2_), pending_accept_ ? 1 : 0);
      break;
    case Action::kPartyOptions:
      // Même appel que la fenêtre native (@0x008c684e) : les trois sélections
      // dans l'ordre exp / ramassage / partage.
      ModeSendMsg(kCmdPartyOptions, opt_exp_, opt_pickup_, opt_share_);
      break;
    case Action::kNone:
      break;
  }

  pending_gid_ = 0;
  pending_id2_ = 0;
  pending_name_.clear();
}

void PartyFriendWindow::FillHp(SocialRow& row) const {
  int hp = 0, max_hp = 0;
  // Un membre hors ligne n'a pas d'acteur : le natif masque sa jauge sans même
  // chercher. On fait pareil, pour ne pas afficher les PV d'un homonyme chargé.
  if (row.offline) { row.has_hp = false; return; }
  row.has_hp = ReadHpSEH(row.gid, &hp, &max_hp);
  row.hp = hp;
  row.max_hp = max_hp;
}

// ── Bascule natif / ImGui ────────────────────────────────────────────────────

void PartyFriendWindow::KillNative(bool adopt_open_state) {
  if (!uiwnd::FindWindow(kWinMessengerGroup)) return;
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
    uiwnd::CloseWindow(kWinMessengerGroup);
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

  if (ro::BeginRoWindow(i18n::Tr("Groupe / Amis"), &open_)) {
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
    confirm_gid_  = OwnAidSEH();
    confirm_name_.clear();
    open_confirm_ = true;
  }

  DrawPartyOptions();
  ImGui::Separator();

  for (size_t i = 0; i < party_.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    DrawPartyRow(party_[i]);
    ImGui::PopID();
  }

  // ── Le compteur du bas, comme le natif ────────────────────────────────────
  // `sprintf("%d/%d", PartyMemberCount(), 0x0C)` @0x00704820 : le maximum est
  // écrit EN DUR dans le client (12 = MAX_PARTY). On reprend sa constante plutôt
  // que d'en inventer une : si le serveur en autorisait moins, c'est quand même
  // ce nombre-là que le client montrerait partout ailleurs.
  // (Pas d'équivalent pour les amis : le client n'y affiche aucun plafond, et
  // MAX_FRIENDS est une valeur SERVEUR qu'on ne peut pas lire d'ici.)
  ImGui::TextDisabled("%s %d/%d", msgstr::Utf8Or(0xC9F, i18n::Tr("Membres :")),
                      static_cast<int>(party_.size()), kMaxPartyMembers);
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
                      static_cast<int>(friends_.size()), kMaxFriends);
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
void PartyFriendWindow::DrawPartyRow(const SocialRow& row) {
  const float icon = ro::Px(kJobIconSize);
  char path[160];

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  // Largeur de la ligne, capturée AVANT le premier SameLine (elle sert au trait
  // de séparation, tout en bas, où le curseur est en fin de ligne).
  const float row_w = ImGui::GetContentRegionAvail().x;

  // ── L'icône de classe ──────────────────────────────────────────────────────
  std::snprintf(path, sizeof(path), kJobIconFmt, kUiRoot,
                static_cast<unsigned>(row.job));
  const ro::GameTexture job_icon = ro::CachedTextureFromGameFile(path);
  if (job_icon.tex) {
    // Hors ligne : la même icône, assombrie — le natif grise toute la ligne.
    const ImVec4 tint = row.offline ? ImVec4(0.55f, 0.55f, 0.55f, 1.0f)
                                    : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // ⚠ `Image()` n'accepte plus de teinte depuis ImGui 1.91.9 : elle a déménagé
    // dans `ImageWithBg` (fond transparent ici, on ne veut que la teinte).
    ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(job_icon.tex),
                       ImVec2(icon, icon), ImVec2(0, 0), ImVec2(1, 1),
                       ImVec4(0, 0, 0, 0), tint);
  } else {
    // Pas d'art (job inconnu du dossier, ou texture perdue au reset de device) :
    // on garde la place pour que les lignes restent alignées.
    ImGui::Dummy(ImVec2(icon, icon));
  }
  // Le natif pose le nom de classe sur son bouton d'icône
  // (`UITextButton_SetName`, DrawContent) : on le rend en infobulle, seul endroit
  // où il tient sans encombrer la ligne.
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", ro::LocalToUtf8(JobName(row.job)));
  ImGui::SameLine();

  ImGui::BeginGroup();

  // ── Ligne 1 : « Lv.N Nom(Carte) », précédé de la couronne du chef ──────────
  if (row.is_leader) {
    // Le natif dessine `ico_partyCrown.bmp` devant le nom du chef.
    std::snprintf(path, sizeof(path), "%s\\renewalparty\\ico_partyCrown.bmp",
                  kUiRoot);
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
  // Le natif n'affiche la carte que pour un membre EN LIGNE qui en a une.
  if (pretty[0] && !row.offline)
    std::snprintf(label, sizeof(label), "Lv.%u %s(%s)", row.level, name_utf8,
                  ro::LocalToUtf8(pretty));
  else
    std::snprintf(label, sizeof(label), "Lv.%u %s", row.level, name_utf8);

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
    const float bar_w = ro::Px(96.0f);
    const float bar_h = ro::Px(7.0f);
    const ImVec2 bar_pos = ImGui::GetCursorScreenPos();
    const ImVec2 bar_end(bar_pos.x + bar_w, bar_pos.y + bar_h);
    dl->AddRectFilled(bar_pos, bar_end, IM_COL32(24, 24, 24, 200));
    float frac = static_cast<float>(row.hp) / static_cast<float>(row.max_hp);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    dl->AddRectFilled(bar_pos, ImVec2(bar_pos.x + bar_w * frac, bar_end.y),
                      IM_COL32(64, 200, 72, 255));
    dl->AddRect(bar_pos, bar_end, IM_COL32(0, 0, 0, 180));
    ImGui::Dummy(ImVec2(bar_w, bar_h));
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::Text("%d/%d", row.hp, row.max_hp);
  }

  ImGui::EndGroup();

  // ── La pastille de statut, calée à droite de la ligne ─────────────────────
  // Même arbitrage que le natif : hors ligne -> OFF, moi -> ME (comparaison à
  // g_Account_Aid, pas à un champ de l'entrée), tout le reste -> ON.
  const bool is_me = (row.gid == OwnAidSEH());
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
  if (ImGui::IsMouseHoveringRect(origin, ImVec2(origin.x + row_w, y)) &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    ImGui::OpenPopup("##rowmenu");
  }
  DrawRowContextMenu(row, true);

  ImGui::Spacing();
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
  const int exp    = ReadOptSEH(kOptExpAddr);
  const int pickup = ReadOptSEH(kOptPickupAddr);
  const int share  = ReadOptSEH(kOptShareAddr);
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
  if (!was_known || PartyMemberCountSEH() <= 0) return;
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
void PartyFriendWindow::DrawRowContextMenu(const SocialRow& row, bool party) {
  if (!ImGui::BeginPopup("##rowmenu")) return;

  const bool is_me = (row.gid == OwnAidSEH());

  ImGui::TextDisabled("%s", ro::LocalToUtf8(row.name.c_str()));
  ImGui::Separator();

  // Chuchoter : à quelqu'un d'autre, et seulement s'il est joignable.
  if (!is_me && !row.offline &&
      ImGui::Selectable(i18n::Tr("Chuchoter"))) {
    pending_      = Action::kWhisper;
    pending_gid_  = row.gid;
    pending_name_ = row.name;
  }

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

void PartyFriendWindow::DrawFriendRow(const SocialRow& row) {
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
    ImGui::OpenPopup("##rowmenu");
  }
  DrawRowContextMenu(row, false);
}
