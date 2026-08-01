#include "ragnarok/item_db.h"
#include "ragnarok/globals.h"
#include "features/windows/trade_window.h"

// Icônes d'item : ro::ItemIcon (ui/icon_cache.h). Le chargement, le colorkey
// magenta et l'invalidation au reset de device y sont partagés — ce fichier en
// gardait sa propre copie, comme cinq autres plugins.
#include "ui/icon_cache.h"
#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "bourgeon.h"        // Bourgeon::Instance().SendPacket / RegisterObserveOpcode
#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB / Overlay_DeviceEpoch
#include "features/item_cell.h"  // itemcell::NameById (nom d'item par id)
#include "ragnarok/msgstring.h"  // msgstr::Cp949 / msgstr::Utf8
#include "imgui.h"
#include "ui/imgui_escape.h"
#include "features/windows/inventory_viewer.h"  // TradeDraggedItem (cible de drag-drop "INV_ITEM")
#include "ui/ro_imgui.h"    // BeginRoWindow (skin RO)
#include "ui/ro_widgets.h"  // mui::IsLastItemRightClicked

// ── Constantes RE (client 20250716, base 0x400000 ; cf. docs/trade_window_re.md) ──
namespace {

// UIWindowMgr + factory.

// Fenêtre d'échange — RE LIVE 2026-07-23 : c'est la NOUVELLE classe CUIExchangeUI
// (famille « CUI », comme CUIGameSettingsUI 0x271e), et PAS l'ancienne UIExchangeWnd
// (0x01031edc, morte — jamais instanciée). C'est une sous-classe UIWindow composite
// (ctor FUN_009cd970 -> UIWindow_composite_ctor ; OnMsg FUN_009cecd0 ; rebuild
// FUN_009ce450). id MAP pinné 0x271b. Cf. docs/trade_window_re.md.
constexpr uintptr_t kExchangeVTable = 0x010457d8;  // CUIExchangeUI (fenêtre d'échange)
constexpr int       kExchangeId     = 0x271b;      // id map pinné (recouvré au besoin)
constexpr uintptr_t kAcceptVTable   = 0x01033754;  // popup requête (best-effort)
constexpr int       kAcceptId       = 0x20;        // id popup (best-effort)

// Offsets CUIExchangeUI (RE FUN_009ce450 / FUN_009cecd0).
constexpr int kOffMyListW     = 0xE4;   // widget liste MES objets
constexpr int kOffPtListW     = 0xF8;   // widget liste objets PARTENAIRE
constexpr int kOffWidgetLock  = 0xC4;   // (widget liste) octet : côté verrouillé (>0)

// ── Inventaire (modèle SESSION) ──────────────────────────────────────────────
// MES objets offerts ne sont plus lus dans un tableau de deal rempli par le natif :
// le serveur ne me renvoie qu'un ACQUITTEMENT ({index, result}), et l'objet lui-même
// se retrouve par son index dans l'inventaire — qui, lui, ne bouge pas pendant
// l'échange (le serveur ne retire les objets qu'au commit).
constexpr uintptr_t kInvListHead = 0x015fbab0;  // sentinelle std::list<ItemSkillInfo>
// Offsets DANS l'ItemSkillInfo (identiques à character_sheet / inventory_viewer).
// Inventory_DecreaseOrRemoveByInvIndex(session, invIndex, amount) : retire la
// quantité du nœud d'inventaire, détruit le nœud s'il tombe à zéro, puis rafraîchit
// les fenêtres d'objets. __thiscall, this = la session — son `this + 5872` EST
// kInvListHead (0x015FA3C0 + 0x16F0 = 0x015FBAB0), vérifié au désassemblage.
constexpr uintptr_t kInvDecrease = 0x00d57a30;

constexpr int kInfoAmount = 0x10;   // int : quantité possédée
constexpr int kInfoIdStr  = 0x2c;   // std::string SSO : itemId EN TEXTE (atoi)
constexpr int kInfoIdCap  = 0x40;   // capacité SSO (+0x2c+0x14 ; >15 => heap)
constexpr int kInfoDamaged = 0x5d;  // byte : équipement CASSÉ (rendu rouge, cf. itemcell)
constexpr int kInfoRefine = 0x60;   // int : refine

// Bus de commandes = CMode::SendMsg. Réplique GameMode_GetActive(0x1213338) :
// mode courant = *(0x1213338+4) [= *(0x121333c)] SEULEMENT si *(0x1213338+0x58)==1
// (mode actif). vf+0x18 (slot 6) = le dispatcher use/equip/transfert/deal.
// Commandes de l'échange — TOUTES vérifiées live sur les handlers de boutons natifs
// (OK = CUIExchangeUI_OnOkButton 0x009ce140, trade = FUN_009ce040, cancel = FUN_009ce000).
// 🔴 Les cinq commandes d'échange (0x32 ack, 0x33 ajout, 0x34 verrou, 0x35 annuler,
// 0x36 valider) ne sont PLUS utilisées : on émet les paquets nous-mêmes. Le
// désassemblage montre que ce n'étaient que des constructeurs — CMode::SendMsg
// case 52 @0x00C8CE33 tient tout entier dans « mov eax, 0EBh ; SendPacket », idem
// case 53 (0x00ED) et 54 (0x00EF) : aucun accès fenêtre, aucun état. Rien
// n'obligeait donc à garder une native vivante pour agir, et le cmd 0x12 qui
// « poussait » l'ajout n'était qu'une sélection d'UI native, sans objet ici.
//
// « Screenshot Trade » reste un cmd : c'est un SERVICE du client (capture d'écran
// légendée), pas une fenêtre — si la case est cochée, le bouton natif envoie 0x44
// avec le texte MsgStringTable(0x728) AVANT le commit (cf. FUN_009ce040).
constexpr int kCmdScreenshot = 0x44;

// Table de messages localisés du client (JAMAIS de texte en dur : on lit le
// natif) — msgstr::, dans ragnarok/.
constexpr int       kMsgScrLabel    = 0x727;       // libellé de la case « Screenshot Trade »
constexpr int       kMsgScrPayload  = 0x728;       // texte envoyé avec le cmd 0x44
// ── Paquets de l'échange (client 20250716 == serveur moonlight, vérifié des deux
// côtés : packets.hpp / packets_struct.hpp / clif_packetdb.hpp) ────────────────
// data = payload APRÈS l'opcode 2 octets ; len = longueur transmise.
constexpr uint16_t kOpReq      = 0x01f4;  // ZC_REQ_EXCHANGE_ITEM {name[24],targetId:4,targetLv:2}
constexpr uint16_t kOpAck      = 0x01f5;  // ZC_ACK_EXCHANGE_ITEM {result:1,targetId:4,targetLv:2}
constexpr uint16_t kOpAddItem  = 0x0b42;  // ZC_ADD_EXCHANGE_ITEM (le partenaire dépose)
constexpr uint16_t kOpAckAdd   = 0x00ea;  // ZC_ACK_ADD_EXCHANGE_ITEM {index:2,result:1}
constexpr uint16_t kOpConclude = 0x00ec;  // ZC_CONCLUDE_EXCHANGE_ITEM {who:1}
constexpr uint16_t kOpCancel   = 0x00ee;  // ZC_CANCEL_EXCHANGE_ITEM
constexpr uint16_t kOpExec     = 0x00f0;  // ZC_EXEC_EXCHANGE_ITEM {result:1}

// ZC_ADD_EXCHANGE_ITEM : opcode et disposition dépendent du PACKETVER. Le serveur
// est compilé en 20250716, et PACKETVER_RE n'est PAS défini pour cette date (la
// fenêtre RE s'arrête à 20211118) -> branche PACKETVER_MAIN_NUM >= 20200916, donc
// opcode 0x0B42 et refine/grade REJETÉS EN FIN de structure. Payload 60 octets :
constexpr int kAddItemId    = 0;   // u32  id d'objet (0 = c'est du ZENY, cf. clif_tradeadditem)
constexpr int kAddType      = 4;   // u8   type d'objet
constexpr int kAddAmount    = 5;   // i32  quantité — ou le MONTANT en zeny si id == 0
constexpr int kAddIdentif   = 9;   // u8
constexpr int kAddDamaged   = 10;  // u8   équipement cassé
constexpr int kAddCards     = 11;  // u32 ×4 (EQUIPSLOTINFO en u32 depuis 20181121)
constexpr int kAddOptions   = 27;  // {i16 index, i16 value, u8 param} ×5 = 25 o
constexpr int kAddLocation  = 52;  // u32  masque d'équipement (aperçu de description)
constexpr int kAddLook      = 56;  // u16  viewSprite
constexpr int kAddRefine    = 58;  // u8
constexpr int kAddGrade     = 59;  // u8
constexpr int kAddPayload   = 60;

// Envois (CZ). Tous de longueur fixe, tous construits ici.
constexpr uint16_t kCzAck      = 0x00e6;  // {type:1}  3 = accepter, 4 = refuser
constexpr uint16_t kCzAddItem  = 0x00e8;  // {index:2, amount:4} — index 0 = zeny
constexpr uint16_t kCzConclude = 0x00eb;  // (opcode seul) verrouiller
constexpr uint16_t kCzCancel   = 0x00ed;  // (opcode seul) annuler
constexpr uint16_t kCzExec     = 0x00ef;  // (opcode seul) valider

// Codes d'erreur d'ajout PROPRES au client. Les valeurs 0-4 sont celles du serveur
// (ZC_ACK_ADD_EXCHANGE_ITEM) ; celles-ci commencent à 100 pour ne jamais les
// recouvrir. Elles couvrent deux refus que le serveur ne signale PAS : il clampe en
// silence, et sort sans un mot au 11e objet.
constexpr int kErrAlreadyAllOffered = 100;
constexpr int kErrTradeFull         = 101;

// Nombre d'emplacements de cartes réellement occupés. Les valeurs spéciales du
// premier slot ne sont PAS des cartes : elles encodent un objet forgé (0x00FF) ou
// signé/nommé (0x00FE, 0xFF00) — les compter afficherait « [4] » sur une arme forgée.
// Retrait réel d'une quantité du sac, par le chemin natif. Fonction libre : son
// __try ne doit pas cohabiter avec la boucle appelante (objets à déroulement).
void InventoryDecrease(int invIndex, int amount) {
  if (invIndex == 0 || amount < 1) return;
  __try {
    using Fn = int(__thiscall*)(void*, int, int);
    reinterpret_cast<Fn>(kInvDecrease)(
        reinterpret_cast<void*>(rag::kSessionAddr), invIndex, amount);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// L'objet s'empile-t-il ? Réplique exacte de item_data::isStackable() du serveur
// (itemdb.cpp) — l'itemType du paquet vient de sd->inventory_data[i]->type, c'est
// donc bien cette énumération-là. Les cinq exclus sont des INSTANCES uniques : deux
// marteaux identiques restent deux lignes, ils ne font pas « x2 ».
//   4 = ARMOR · 5 = WEAPON · 7 = PETEGG · 8 = PETARMOR · 12 = SHADOWGEAR
bool TypeIsStackable(uint8_t type) {
  return type != 4 && type != 5 && type != 7 && type != 8 && type != 12;
}

// ── Reconstruction d'un ItemSkillInfo à partir d'une entrée d'échange ────────
//
// C'est ce que fait le handler natif lui-même (Recv_ZC_ADD_EXCHANGE_ITEM : ctor,
// SetId, puis remplissage champ par champ) — et c'est indispensable ici, parce que
// l'échange ne manipule aucun nœud d'inventaire : celui du partenaire ne nous
// appartient pas, et le nôtre a quitté le sac dès l'acquittement.
//
// Le tampon est de PILE et le reste : le ctor construit deux std::string en SSO,
// SetId y écrit un id court, donc aucun tas n'est alloué et il n'y a pas de
// destructeur à appeler. Même convention que NpcDialogWindow::OpenItemDescById.
// ⚠ L'OnMsg 0x18 de la fenêtre de description COPIE l'info : la lui passer depuis
// la pile est sûr.
//
// Champs que le name-builder natif lit pour décorer (cf. item_cell.cc) : type @0,
// cartes @0x1c-0x28, refine @0x60, grade @0x88 — sans eux le nom sort NU.
constexpr size_t kInfoSize = 0x100;
void BuildTradeInfo(const TradeWindow::TradeItem& it, uint8_t* info) {
  std::memset(info, 0, kInfoSize);
  __try {
    using InfoCtor_t  = void*(__fastcall*)(void*);
    using InfoSetId_t = void(__thiscall*)(void*, int);
    reinterpret_cast<InfoCtor_t>(itemdb::kInfoCtorAddr)(info);
    reinterpret_cast<InfoSetId_t>(itemdb::kInfoSetIdAddr)(info, static_cast<int>(it.id));
    *reinterpret_cast<int*>(info + 0x00)      = it.type;
    *reinterpret_cast<uint32_t*>(info + 0x08) = it.location;  // gate « aperçu »
    *reinterpret_cast<int*>(info + 0x10)      = it.amount;
    for (int c = 0; c < 4; ++c)
      *reinterpret_cast<uint32_t*>(info + 0x1c + c * 4) = it.cards[c];
    info[0x5c] = it.identified;
    info[0x5d] = it.damaged;
    *reinterpret_cast<int*>(info + 0x60)      = it.refine;
    *reinterpret_cast<uint32_t*>(info + 0x70) = it.look;      // gate « aperçu »
    info[0x88] = it.grade;
    *reinterpret_cast<int*>(info + 0x98) = it.opt_count;
    for (int k = 0; k < it.opt_count && k < 5; ++k) {
      uint8_t* e = info + 0x9c + k * 5;
      *reinterpret_cast<int16_t*>(e)     = it.opts[k].index;
      *reinterpret_cast<int16_t*>(e + 2) = it.opts[k].value;
      e[4] = it.opts[k].param;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int CardSlotCount(const uint32_t* cards) {
  if (cards[0] == 0x00FF || cards[0] == 0x00FE || cards[0] == 0xFF00) return 0;
  int n = 0;
  for (int i = 0; i < 4; ++i) if (cards[i] != 0) ++n;
  return n;
}

// ── Fenêtres natives (SEH-gardé) ──
void* FindWnd(int id) { return uiwnd::SafeFindWindow(id); }
// (Plus de masquage : ce plugin DÉTRUIT. Une native masquée garde le clavier.)
// Détruit une fenêtre native par id (persiste sa position + close), comme le X natif.
void CloseWnd(int id) { uiwnd::SafeCloseWindow(id); }
uintptr_t VTableOf(void* w) { return uiwnd::SafeVTableOf(w); }

// Cherche la fenêtre d'échange (vtable CUIExchangeUI) dans la std::map du window-mgr
// (mgr+8 ; nœud MSVC {L@0, P@4, R@8, color@0xc, isnil@0xd, key@0x10, value@0x14}).
// Renvoie le pointeur (et son id = clé du nœud dans *out_id) ou nullptr. SEH + borné.
void* FindTradeWndInMap(int* out_id) {
  __try {
    uint8_t* mgr = reinterpret_cast<uint8_t*>(uiwnd::kUIWindowMgrAddr);
    void* head = *reinterpret_cast<void**>(mgr + 8);  // _Myhead = sentinelle (= nil)
    if (!head) return nullptr;
    void* root = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(head) + 4);
    if (!root || root == head) return nullptr;
    void* stack[64];
    int sp = 0, guard = 0;
    stack[sp++] = root;
    while (sp > 0 && guard < 256) {
      ++guard;
      uint8_t* n = reinterpret_cast<uint8_t*>(stack[--sp]);
      if (!n || n == head) continue;
      void* val = *reinterpret_cast<void**>(n + 0x14);
      if (val && VTableOf(val) == kExchangeVTable) {
        if (out_id) *out_id = *reinterpret_cast<int*>(n + 0x10);  // clé = id fenêtre
        return val;
      }
      if (sp < 62) {
        void* l = *reinterpret_cast<void**>(n + 0);
        void* r = *reinterpret_cast<void**>(n + 8);
        if (l && l != head) stack[sp++] = l;
        if (r && r != head) stack[sp++] = r;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return nullptr;
}

// Fenêtre d'échange : chemin rapide par id pinné 0x271b, sinon map-walk par VTABLE
// (récupère aussi l'id si jamais il différait). La fenêtre est bien map-based (elle
// vit dans la std::map du mgr, key 0x271b — confirmé live). Renseigne *out_id.
void* FindTradeWnd(int* out_id) {
  void* w = FindWnd(kExchangeId);
  if (w && VTableOf(w) == kExchangeVTable) {
    if (out_id) *out_id = kExchangeId;
    return w;
  }
  return FindTradeWndInMap(out_id);
}

// CMode::SendMsg via le dispatcher [0x0121333c] (vtable+0x18 = slot 6). Thread
// principal UNIQUEMENT (jamais depuis OnRecvPacket).
void ModeCmd(int cmd, int a, int b, int c, int d) {
  __try {
    uint8_t* mgr = reinterpret_cast<uint8_t*>(rag::kModeMgrAddr);
    if (*reinterpret_cast<int*>(mgr + 0x58) != 1) return;  // aucun mode actif
    void* disp = *reinterpret_cast<void**>(mgr + 4);        // = *(0x121333c)
    if (disp) {
      void** vt = *reinterpret_cast<void***>(disp);
      using Fn = int(__thiscall*)(void*, int, int, int, int, int);
      reinterpret_cast<Fn>(vt[6])(disp, cmd, a, b, c, d);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Nom d'item par id : itemcell::NameById (DB de descriptions du client, cache
// partagé). L'échange ne voit que des ids et un refine — le paquet ne porte pas
// d'ItemSkillInfo — d'où le « +N » composé à la main à l'affichage.






}  // namespace

TradeWindow::TradeWindow() {
  // ── Les paquets qui FONT VIVRE l'échange : on prend leur place ───────────────
  //
  // 🔴 REMPLACEMENT. En observant, la popup de requête et la fenêtre d'échange
  // naissaient puis étaient CACHÉES — or une native masquée garde le CLAVIER, et
  // son bouton par défaut agit : sur un échange, Entrée engage des objets. C'est le
  // trou qui avait détruit une arme au refine, avec l'inventaire en jeu cette fois.
  //
  // Le prédicat est relu à chaque paquet : interrupteur éteint, l'échange natif
  // reprend la main entièrement.
  //
  // ⚠ Le contenu de l'échange changeait de SOURCE avec ce remplacement. Il venait
  // de deux tableaux globaux de session (ItemSkillInfo ×10) que les handlers natifs
  // remplissaient ; en prenant leur place on les assèche. Les deux listes se
  // reconstruisent donc ici, depuis les paquets — celle du partenaire par 0x0B42,
  // la mienne par l'acquittement 0x00EA croisé avec l'inventaire.
  const auto claim = [this] { return imgui_enabled_; };
  Bourgeon::Instance().RegisterReplaceOpcode(kOpReq, claim);       // popup de requête
  Bourgeon::Instance().RegisterReplaceOpcode(kOpAck, claim);       // ouvre l'échange
  Bourgeon::Instance().RegisterReplaceOpcode(kOpAddItem, claim);   // dépôt du partenaire
  Bourgeon::Instance().RegisterReplaceOpcode(kOpAckAdd, claim);    // mes dépôts
  Bourgeon::Instance().RegisterReplaceOpcode(kOpConclude, claim);  // verrous
  Bourgeon::Instance().RegisterReplaceOpcode(kOpCancel, claim);    // annulation
  Bourgeon::Instance().RegisterReplaceOpcode(kOpExec, claim);      // fin d'échange
}

// ── Fil RÉSEAU : COPIER, rien de plus ────────────────────────────────────────
//
// 🔴 Aucun état de l'échange n'est touché ici, et c'est la règle du projet, pas une
// précaution locale : le rendu parcourt `partner_items_` sur le fil principal, un
// push_back concurrent qui réalloue lui laisse un pointeur mort. Tous les paquets
// de l'échange sont de longueur FIXE — Push suffit, pas de PushAnnounced.
// Cf. features/net_inbox.h.
void TradeWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  if (!imgui_enabled_) return;
  net_inbox_.Push(opcode, data, len);
}

// ── Fil PRINCIPAL : le décodage, rejoué en phase d'entrée (ordre d'arrivée) ──
void TradeWindow::HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  switch (opcode) {
    case kOpReq: {  // ZC_REQ_EXCHANGE_ITEM : quelqu'un demande un échange.
      if (len < 30) return;
      std::memcpy(req_name_, data, 24);
      req_name_[24] = '\0';
      req_aid_   = *reinterpret_cast<const uint32_t*>(data + 24);
      req_level_ = *reinterpret_cast<const uint16_t*>(data + 28);
      req_open_  = true;
      return;
    }
    case kOpAck: {  // ZC_ACK_EXCHANGE_ITEM : réponse à la requête.
      if (len < 1) return;
      req_open_ = false;
      if (data[0] != 3) { last_result_ = -1; return; }  // 3 = accepté
      // 🔴 C'est CE paquet qui ouvre l'échange, plus l'apparition d'une fenêtre
      // native. Session repartie de zéro : les deux offres, les verrous, le zeny.
      // Sans cette remise à zéro, l'échange précédent transparaîtrait dans le neuf.
      ResetSession();
      open_ = true;
      need_pos_ = true;
      show_panel_ = true;
      last_result_ = -1;
      return;
    }
    case kOpAddItem: {  // ZC_ADD_EXCHANGE_ITEM : ce que le PARTENAIRE dépose.
      if (len < kAddPayload) return;
      const uint32_t id = *reinterpret_cast<const uint32_t*>(data + kAddItemId);
      const int32_t amount = *reinterpret_cast<const int32_t*>(data + kAddAmount);
      if (id == 0) {
        // itemId nul = ZENY, et le montant voyage dans `amount` (clif_tradeadditem
        // envoie une structure vide dont seul ce champ est renseigné).
        partner_zeny_ = amount;
        return;
      }
      TradeItem it;
      it.id         = id;
      it.amount     = amount;
      it.type       = data[kAddType];
      it.identified = data[kAddIdentif];
      it.damaged    = data[kAddDamaged];
      it.refine     = data[kAddRefine];
      it.grade      = data[kAddGrade];
      it.location   = *reinterpret_cast<const uint32_t*>(data + kAddLocation);
      it.look       = *reinterpret_cast<const uint16_t*>(data + kAddLook);
      for (int c = 0; c < 4; ++c)
        it.cards[c] = *reinterpret_cast<const uint32_t*>(data + kAddCards + c * 4);
      // Options aléatoires : 5 entrées {i16 index, i16 value, u8 param}. index 0 =
      // emplacement vide, et elles sont contiguës — on s'arrête au premier trou.
      for (int k = 0; k < 5; ++k) {
        const uint8_t* e = data + kAddOptions + k * 5;
        const int16_t oi = *reinterpret_cast<const int16_t*>(e);
        if (oi == 0) break;
        it.opts[it.opt_count].index = oi;
        it.opts[it.opt_count].value = *reinterpret_cast<const int16_t*>(e + 2);
        it.opts[it.opt_count].param = e[4];
        ++it.opt_count;
      }
      it.slots = CardSlotCount(it.cards);
      // Le nom DÉCORÉ reste vide ici : ResolveNames s'en charge, juste après.
      //
      // ⚠ Le serveur envoie un paquet par AJOUT, avec le DELTA — pas le total : un
      // partenaire qui pose 20 puis réajuste à 49 en émet deux (20, puis 29). On
      // fusionne les piles rigoureusement identiques pour montrer le vrai total.
      //
      // Le natif, lui, ne fusionne PAS : son Session_AddPartnerDealItem prend le
      // premier emplacement libre et copie, d'où deux lignes à l'écran. C'est un
      // écart ASSUMÉ — la somme est la même, l'affichage est simplement lisible.
      //
      // ⚠ Mais SEULEMENT ce qui s'empile. Un équipement est une INSTANCE : deux
      // marteaux identiques sont deux objets, les afficher « x2 » laisserait croire
      // à une pile alors qu'ils occuperont deux emplacements à l'arrivée.
      bool merged = false;
      if (TypeIsStackable(it.type)) {
        for (TradeItem& e : partner_items_) {
          if (e.id != it.id || e.refine != it.refine || e.grade != it.grade ||
              e.identified != it.identified || e.damaged != it.damaged)
            continue;
          if (std::memcmp(e.cards, it.cards, sizeof(e.cards)) != 0) continue;
          e.amount += it.amount;
          merged = true;
          break;
        }
      }
      if (!merged) partner_items_.push_back(it);
      return;
    }
    case kOpAckAdd: {  // ZC_ACK_ADD_EXCHANGE_ITEM {index:2, result:1}
      if (len < 3) return;
      const uint16_t index = *reinterpret_cast<const uint16_t*>(data);
      add_error_ = data[2];  // 0 = ok, sinon surpoids/plein/stack...
      if (add_error_ != 0) { pending_adds_.clear(); return; }
      // Succès : l'objet est bien dans MON offre. Le paquet ne dit QUE l'index —
      // la quantité, on est seuls à la connaître (cf. ResolveMyAdds).
      acked_adds_.push_back(index);
      return;
    }
    case kOpConclude:  // ZC_CONCLUDE_EXCHANGE_ITEM {who:1}
      // who = 0 -> MON offre est verrouillée ; 1 -> celle du partenaire. Vérifié
      // serveur (trade.cpp : lock(*sd,false) à celui qui verrouille, lock(*tsd,true)
      // à l'autre).
      if (len >= 1) { if (data[0] == 0) my_locked_ = true; else partner_locked_ = true; }
      return;
    case kOpCancel:  // ZC_CANCEL_EXCHANGE_ITEM : l'échange est annulé.
    case kOpExec: {  // ZC_EXEC_EXCHANGE_ITEM {result:1} : échange terminé.
      // Fin d'échange : on ferme sans rien renvoyer, le deal est déjà défait côté
      // serveur. Rien à défaire de notre côté non plus : les objets ont quitté le sac
      // dès l'acquittement (cf. ResolveMyAdds) — sur ANNULATION le serveur les rend
      // lui-même par clif_additem, sur COMMIT il ne dit rien, et c'est correct.
      //
      // ⚠ Les acquittements encore en attente sont résolus AVANT de fermer : le
      // serveur peut acquitter un ajout et exécuter l'échange dans le même souffle,
      // et l'objet doit alors quitter le sac — sans quoi il y reste en fantôme.
      // 2 = annulé, distinct de 0 (succès) et 1 (échec).
      const int result = (opcode == kOpCancel) ? 2 : ((len >= 1) ? data[0] : 0);
      if (!acked_adds_.empty()) ResolveMyAdds();
      open_ = false;
      was_open_ = false;
      show_panel_ = true;
      committed_ = false;
      ResetSession();
      last_result_ = result;  // ResetSession n'y touche pas : le verdict survit
      return;
    }
    default:
      return;
  }
}

// ── Actions : paquets CZ construits ICI ──────────────────────────────────────
//
// 🔴 Plus aucun cmd de CMode::SendMsg pour agir. Le désassemblage des trois
// commandes sans argument (cases 52/53/54 @0x00C8CE33) montre qu'elles tiennent
// entières dans « mov eax, <opcode> ; SendPacket » : ce n'étaient que des
// constructeurs, sans accès fenêtre ni état. Les reproduire est donc exact, et ça
// nous affranchit du dernier lien avec le natif.
namespace {
void SendOpcodeOnly(uint16_t opcode) {
  uint8_t pkt[2];
  *reinterpret_cast<uint16_t*>(pkt) = opcode;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}
}  // namespace

void TradeWindow::TradeAck(int type) {
  // CZ_ACK_EXCHANGE_ITEM {type:1} — 3 = accepter, 4 = refuser.
  uint8_t pkt[3];
  *reinterpret_cast<uint16_t*>(pkt) = kCzAck;
  pkt[2] = static_cast<uint8_t>(type);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  req_open_ = false;
  // La popup native ne naît plus ; ce nettoyage ne sert qu'au cas d'un interrupteur
  // allumé alors qu'une requête native était déjà à l'écran.
  CloseWnd(kAcceptId);
}

// CZ_ADD_EXCHANGE_ITEM {index:2, amount:4}. index 0 = ZENY (montant ABSOLU), sinon
// l'index inventaire de l'objet. Le serveur valide tout (trade_tradeadditem).
void TradeWindow::SendAdd(int index, int32_t amount) {
  uint8_t pkt[8];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kCzAddItem;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(index);
  *reinterpret_cast<int32_t*>(pkt + 4)  = amount;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

void TradeWindow::SetZeny(int amount) {
  if (amount < 0) amount = 0;
  const uint32_t zmax = *reinterpret_cast<uint32_t*>(rag::kZenyAddr);
  if (static_cast<uint32_t>(amount) > zmax) amount = static_cast<int>(zmax);
  pending_zeny_ = amount;  // confirmé par l'acquittement (index 0)
  SendAdd(0, amount);
}
void TradeWindow::Lock()   { SendOpcodeOnly(kCzConclude); }
void TradeWindow::Cancel() { SendOpcodeOnly(kCzCancel); }

void TradeWindow::Commit() {
  // Réplique EXACTEMENT le bouton « trade » natif (FUN_009ce040) : si la case
  // Screenshot est cochée, cmd 0x44(texte natif) AVANT le commit, puis cmd 0x36.
  // ⚠ Le serveur n'exécute l'échange que quand les DEUX joueurs ont validé.
  if (screenshot_) {
    // Ce texte PART au natif (cmd 0x44) : il doit rester en CP949, d'où Cp949
    // et non Utf8 — c'est toute la raison d'être des deux entrées.
    const char* txt = msgstr::Cp949(kMsgScrPayload);
    ModeCmd(kCmdScreenshot,
            static_cast<int>(reinterpret_cast<uintptr_t>(txt)), 0, 0, 0);
  }
  SendOpcodeOnly(kCzExec);
  // Le natif grise aussi son bouton « trade » après l'envoi : l'échange ne s'exécute
  // que quand l'AUTRE joueur a validé à son tour. On bascule en « en attente ».
  committed_ = true;
}

// Quantité EN VOL pour cet index : envoyée, pas encore acquittée. Ce qui est déjà
// acquitté, lui, a quitté le modèle de session — il ne faut donc surtout pas le
// compter deux fois, le stock lu dans l'inventaire en tient déjà compte.
int TradeWindow::PendingAmount(int invIndex) const {
  if (invIndex == 0) return 0;
  int total = 0;
  for (const PendingAdd& p : pending_adds_)
    if (p.index == invIndex) total += p.amount;
  return total;
}

// Nombre d'objets DISTINCTS déjà posés (l'échange en accepte 10 au maximum).
int TradeWindow::OfferedSlotCount() const {
  int seen[16];
  int n = 0;
  auto note = [&](int index) {
    for (int k = 0; k < n; ++k) if (seen[k] == index) return;
    if (n < 16) seen[n++] = index;
  };
  for (const TradeItem& e : my_items_) note(e.index);
  for (const PendingAdd& p : pending_adds_) note(p.index);
  return n;
}

void TradeWindow::AddItemToTrade(int invIndex, int amount) {
  if (!imgui_enabled_ || !open_ || my_locked_) return;
  if (amount < 1) amount = 1;
  if (invIndex == 0) return;  // 0 est réservé au zeny : jamais un objet

  // 🔴 Plafond calqué sur celui du serveur (trade.cpp, trade_tradeadditem) :
  //     if (deal.amount + amount > inventory.amount) amount = inventory.amount - deal.amount;
  // Le serveur clampe SILENCIEUSEMENT et répond quand même « succès » — il n'envoie
  // au partenaire que ce qu'on possède vraiment, rien n'est dupliqué. Mais comme
  // l'acquittement ne porte pas la quantité retenue, ne pas clamper ici nous faisait
  // afficher ce qu'on avait DEMANDÉ : reglisser dix fois le même stack de 10 le
  // montrait à 100 dans notre offre, alors que le partenaire en voyait 10.
  //
  // Le natif n'avait pas ce problème pour une autre raison : son handler
  // d'acquittement retirait l'objet du sac (cf. ResolveMyAdds), on ne pouvait donc
  // pas le reglisser. On fait pareil — le stock lu ici est déjà amputé de tout ce
  // qui est acquitté, et un stack entièrement offert n'a même plus de nœud.
  // Ne reste à retrancher que ce qui est EN VOL, pas encore acquitté.
  int stock = 0;
  void* info = itemcell::FindInfoByIndex(kInvListHead, invIndex);
  if (!info) { add_error_ = kErrAlreadyAllOffered; return; }
  __try { stock = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(info) + kInfoAmount); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return; }
  const int in_flight = PendingAmount(invIndex);
  if (in_flight >= stock) { add_error_ = kErrAlreadyAllOffered; return; }
  if (in_flight + amount > stock) amount = stock - in_flight;

  // Plafond de 10 objets distincts. Le serveur, lui, sort SANS RIEN DIRE au 11e
  // (« the client does not allow to add more than 10 items, and will show an error
  // message ») : le message était à la charge du client, donc de nous maintenant.
  bool new_slot = true;
  for (size_t k = 0; k < my_items_.size(); ++k)
    if (my_items_[k].index == invIndex) { new_slot = false; break; }
  if (new_slot && in_flight > 0) new_slot = false;
  if (new_slot && OfferedSlotCount() >= 10) {
    add_error_ = kErrTradeFull;
    return;
  }

  // Retenu jusqu'à l'acquittement : c'est LUI qui confirme, et il ne porte que
  // l'index — la quantité, on est seuls à la connaître.
  add_error_ = -1;
  pending_adds_.push_back(PendingAdd{invIndex, amount});
  SendAdd(invIndex, amount);
}

void TradeWindow::ResetSession() {
  my_items_.clear();
  partner_items_.clear();
  pending_adds_.clear();
  acked_adds_.clear();
  my_locked_ = partner_locked_ = false;
  my_zeny_ = partner_zeny_ = 0;
  pending_zeny_ = -1;
  zeny_input_ = 0;
  screenshot_ = false;
  committed_ = false;
  add_error_ = -1;
}

void TradeWindow::CloseTrade() {
  // Ferme le deal côté serveur : CZ_CANCEL_EXCHANGE_ITEM, le paquet même que le
  // bouton Annuler natif finit par émettre (CMode::SendMsg case 53).
  Cancel();
  PurgeNativeTradeWindows();
  open_ = false;
  was_open_ = false;
  show_panel_ = true;
  ResetSession();
}

// Complète MES ajouts acquittés. Thread PRINCIPAL : on lit le modèle de session de
// l'inventaire, la source que le natif consultait lui aussi — et qui reste peuplée
// pendant tout l'échange, le serveur ne retirant les objets qu'au commit.
void TradeWindow::ResolveMyAdds() {
  for (int index : acked_adds_) {
    // Index 0 = zeny (le serveur renvoie -2, que clif_tradeitemok convertit en 0).
    if (index == 0) {
      if (pending_zeny_ >= 0) { my_zeny_ = pending_zeny_; pending_zeny_ = -1; }
      continue;
    }
    // Quantité : celle qu'on a demandée. L'acquittement ne la porte pas.
    int amount = 1;
    for (size_t k = 0; k < pending_adds_.size(); ++k) {
      if (pending_adds_[k].index != index) continue;
      amount = pending_adds_[k].amount;
      pending_adds_.erase(pending_adds_.begin() + k);
      break;
    }
    void* info = itemcell::FindInfoByIndex(kInvListHead, index);
    if (!info) continue;
    TradeItem it;
    it.index  = index;
    it.amount = amount;
    __try {
      uint8_t* p = reinterpret_cast<uint8_t*>(info);
      const uint32_t cap = *reinterpret_cast<uint32_t*>(p + kInfoIdCap);
      const char* ids = (cap > 15) ? *reinterpret_cast<char**>(p + kInfoIdStr)
                                   : reinterpret_cast<const char*>(p + kInfoIdStr);
      it.id      = ids ? static_cast<uint32_t>(std::atoi(ids)) : 0;
      it.refine  = *reinterpret_cast<int*>(p + kInfoRefine);
      it.damaged = *reinterpret_cast<uint8_t*>(p + kInfoDamaged);
      // Mêmes données d'instance que celles du partenaire, mais lues dans le nœud
      // — pour que les deux colonnes affichent exactement la même chose. À lire
      // MAINTENANT : le nœud disparaît du sac quelques lignes plus bas.
      it.type       = static_cast<uint8_t>(*reinterpret_cast<int*>(p + 0x00));
      it.location   = *reinterpret_cast<uint32_t*>(p + 0x08);
      it.identified = *(p + 0x5c);
      it.look       = static_cast<uint16_t>(*reinterpret_cast<uint32_t*>(p + 0x70));
      it.grade      = *(p + 0x88);
      for (int c = 0; c < 4; ++c)
        it.cards[c] = *reinterpret_cast<uint32_t*>(p + 0x1c + c * 4);
      int nopt = *reinterpret_cast<int*>(p + 0x98);
      if (nopt < 0) nopt = 0;
      if (nopt > 5) nopt = 5;
      it.opt_count = nopt;
      for (int k = 0; k < nopt; ++k) {
        const uint8_t* e = p + 0x9c + k * 5;
        it.opts[k].index = *reinterpret_cast<const int16_t*>(e);
        it.opts[k].value = *reinterpret_cast<const int16_t*>(e + 2);
        it.opts[k].param = e[4];
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
    it.slots = itemcell::SlotCount(info);
    // Même objet redéposé (le serveur autorise l'empilement) : on cumule au lieu de
    // faire deux lignes, comme la liste native.
    bool merged = false;
    for (TradeItem& e : my_items_) {
      if (e.index != index) continue;
      e.amount += amount;
      merged = true;
      break;
    }
    if (!merged && it.id) my_items_.push_back(it);
    // 🔴 RETRAIT RÉEL du sac, exactement là où le natif le fait (son handler
    // d'acquittement appelait Inventory_DecreaseOrRemoveByInvIndex avant de remplir
    // la liste de deal). Ce n'est pas un choix d'affichage, c'est le PROTOCOLE :
    //   - à l'ANNULATION, trade_tradecancel renvoie clif_additem pour rendre les
    //     objets — le serveur tient pour acquis qu'on les avait retirés ; ne pas
    //     l'avoir fait les fait apparaître EN DOUBLE ;
    //   - au COMMIT, pc_delitem(..., type = 1, ...) ne notifie PAS, pour la même
    //     raison — ne pas l'avoir fait laisse un stack fantôme.
    // Les deux bouts du protocole exigent donc le retrait ICI, et lui seul.
    InventoryDecrease(index, amount);
  }
  acked_adds_.clear();
}

// Compose les noms DÉCORÉS encore vides, des deux côtés. Thread PRINCIPAL : le
// name-builder est natif. Une seule fois par entrée (le nom ne change plus).
//
// Le nom nu ne suffisait pas : deux exemplaires du même équipement ne se
// distinguent que par leur refine, leurs cartes et leurs options — c'est
// exactement ce qu'on veut vérifier AVANT de valider un échange.
void TradeWindow::ResolveNames() {
  uint8_t info[kInfoSize];
  for (size_t k = 0; k < my_items_.size(); ++k) {
    if (my_items_[k].name[0] || my_items_[k].id == 0) continue;
    BuildTradeInfo(my_items_[k], info);
    itemcell::BuildDisplayName(nullptr, info, my_items_[k].name,
                               sizeof(my_items_[k].name));
    if (my_items_[k].slots == 0) my_items_[k].slots = itemcell::SlotCount(info);
  }
  for (size_t k = 0; k < partner_items_.size(); ++k) {
    if (partner_items_[k].name[0] || partner_items_[k].id == 0) continue;
    BuildTradeInfo(partner_items_[k], info);
    itemcell::BuildDisplayName(nullptr, info, partner_items_[k].name,
                               sizeof(partner_items_[k].name));
    // Le compte d'emplacements de la DB est plus juste que le nombre de cartes
    // posées : « [4] » doit s'afficher même sur une arme à 4 slots encore vide.
    const int db_slots = itemcell::SlotCount(info);
    if (db_slots > 0) partner_items_[k].slots = db_slots;
  }
}

// Ne fait plus rien : ces fenêtres ne naissent plus (les paquets qui les créaient
// sont remplacés), les cacher serait nuisible — une native invisible garde le
// clavier, et son bouton par défaut valide un échange — et les détruire ici est
// exclu : on est à l'intérieur de MakeWindow, dont l'appelant déréférence le retour.
// La purge d'OnTick s'en charge.
void TradeWindow::HideNativeAtCreation(void* /*win*/, int /*windowID*/) {}

bool TradeWindow::AnyNativeTradeWindow() const {
  int id = -1;
  if (FindTradeWnd(&id)) return true;
  void* a = FindWnd(kAcceptId);
  return a && VTableOf(a) == kAcceptVTable;
}

void TradeWindow::PurgeNativeTradeWindows() {
  int id = -1;
  if (FindTradeWnd(&id) && id >= 0) CloseWnd(id);
  CloseWnd(kAcceptId);
}

void TradeWindow::OnTick() {
  // ── Basculement de l'interrupteur, les DEUX sens ────────────────────────────
  // Basculer ANNULE l'échange, quel que soit le sens. On ne reprend pas une session
  // qu'on n'a pas vue naître (OFF -> ON : aucun paquet n'est passé par nous, les
  // deux offres nous sont inconnues) et on ne laisse pas non plus le natif finir —
  // ce serait garder vivant ce qu'on prétend avoir remplacé. Annuler est ici sans
  // danger : c'est justement l'issue neutre d'un échange.
  if (imgui_enabled_ != prev_imgui_enabled_) {
    prev_imgui_enabled_ = imgui_enabled_;
    if (open_ || req_open_ || AnyNativeTradeWindow()) {
      Cancel();
      PurgeNativeTradeWindows();
    }
    open_ = was_open_ = req_open_ = false;
    // Ce qui reste en file appartient à la session qu'on vient d'abandonner : on le
    // jette, sinon un dépôt du partenaire arrivé juste avant la bascule ressusciterait
    // dans la suivante.
    net_inbox_.Clear();
    ResetSession();
  }
  if (!imgui_enabled_) { open_ = false; was_open_ = false; req_open_ = false; return; }

  if (open_) {
    // Filet du basculement en pleine session : on DÉTRUIT ce qui traîne au lieu de
    // le masquer (masquée, une native garde le clavier).
    PurgeNativeTradeWindows();
    if (!acked_adds_.empty()) ResolveMyAdds();
    ResolveNames();  // noms décorés des entrées neuves (name-builder natif)
  }

  was_open_ = open_;
}

void TradeWindow::OnRenderUI() {
  if (!imgui_enabled_) return;

  // ── Popup de requête « X souhaite échanger » ──
  if (req_open_) {
    // Toujours centrée à l'écran à chaque apparition (pivot au centre de la fenêtre).
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_Appearing);
    bool p_open = true;
    if (ro::BeginRoWindow("Demande d'échange###bourgeon_trade_req", &p_open,
                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
      if (req_level_ > 0)
        ImGui::Text("%s (Niv. %d) souhaite échanger avec vous.", req_name_,
                    req_level_);
      else
        ImGui::Text("%s souhaite échanger avec vous.", req_name_);
      ImGui::Spacing();
      if (ro::RoButton("Accepter", 130.0f, 0.0f)) TradeAck(3);
      ImGui::SameLine();
      if (ro::RoButton("Refuser", 130.0f, 0.0f)) TradeAck(4);
    }
    if (!p_open) TradeAck(4);
    ro::EndRoWindow();
  }

  if (!open_) return;

  if (need_pos_) {
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(spawn_x_),
                                   static_cast<float>(spawn_y_)),
                            ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(460, 380), ImGuiCond_FirstUseEver);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);

  const bool begun = ro::BeginRoWindow("Échange###bourgeon_trade", &show_panel_,
                                       ImGuiWindowFlags_NoCollapse);
  bourgeon::CloseWindowOnEscape(show_panel_);
  if (!show_panel_) {  // clic X = annuler l'échange
    CloseTrade();
    ro::EndRoWindow();
    ImGui::PopStyleVar(4);
    return;
  }
  if (!begun) { ro::EndRoWindow(); ImGui::PopStyleVar(4); return; }

  const ImVec4 kBlack(0.0f, 0.0f, 0.0f, 1.0f);
  const ImVec4 kGreen(0.20f, 0.65f, 0.20f, 1.0f);
  const ImVec4 kRed(0.75f, 0.15f, 0.15f, 1.0f);

  // Objet survolé / cliqué droit cette frame. L'aperçu se peint APRÈS les deux
  // colonnes (DrawTooltip crée son propre popup, donc hors de toute fenêtre), et la
  // description native s'ouvre en toute fin — jamais au milieu d'un enfant ImGui.
  const TradeItem* hover = nullptr;
  const TradeItem* desc_for = nullptr;
  int desc_x = 0, desc_y = 0;

  // Rend une grille d'objets (icône + nom x qté) dans une colonne de largeur `width`.
  // `mine` = ma colonne : elle sert AUSSI de cible de drag-drop (lâcher un item de
  // l'inventaire ImGui l'ajoute à l'échange), tant que mon offre n'est pas verrouillée.
  auto draw_items = [&](const char* child_id,
                        const std::vector<TradeItem>& items, bool locked,
                        float width, bool mine) {
    ImGui::BeginChild(child_id, ImVec2(width, 150), true);
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
      const TradeItem& it = items[i];
      ImGui::PushID(i);
      bool hovered_icon = false, rclick_icon = false;
      ro::IconTex ic = ro::ItemIcon(it.id);
      if (ic.tex) {
        ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(20, 20));
        hovered_icon = ImGui::IsItemHovered();
        rclick_icon  = mui::IsLastItemRightClicked();
        ImGui::SameLine();
      }
      // Nom DÉCORÉ (refine, préfixes/suffixes de cartes) composé par le
      // name-builder natif, plus « [N] » emplacements — même libellé que partout
      // ailleurs dans Bourgeon. Ombre rouge si l'objet est cassé.
      char nb[128];
      const char* nm = itemcell::Label(
          nb, sizeof(nb),
          it.name[0] ? it.name : itemcell::NameById(it.id), it.slots);
      ImGui::PushStyleColor(ImGuiCol_Text, kBlack);
      itemcell::NameText(nm, it.damaged != 0);
      ImGui::PopStyleColor();
      const bool hovered_name = ImGui::IsItemHovered();
      const bool rclick_name  = mui::IsLastItemRightClicked();
      if (it.amount > 1) { ImGui::SameLine(); ImGui::TextDisabled("x%d", it.amount); }
      // Survol -> aperçu RO complet (cartes, options, refine) ; clic droit -> la
      // description native. C'est tout l'objet de la manœuvre : vérifier que l'objet
      // proposé est bien celui qu'on croit, AVANT de verrouiller.
      if (hovered_icon || hovered_name)
        hover = &it;
      if (rclick_icon || rclick_name) {
        const ImVec2 mp = ImGui::GetMousePos();
        desc_for = &it;
        desc_x = static_cast<int>(mp.x);
        desc_y = static_cast<int>(mp.y);
      }
      ImGui::PopID();
    }
    if (items.empty())
      ImGui::TextDisabled(mine && !locked ? "(vide — glissez un objet ici)" : "(vide)");
    ImGui::EndChild();
    // Le child qu'on vient de fermer est le « dernier item » ImGui : il devient donc
    // la cible de dépôt. Payload "INV_ITEM" = convention de l'inventaire ImGui (même
    // que le doll de character_sheet) ; l'InventoryViewer applique sa propre politique
    // de quantité (pile -> prompt, sinon 1 unité).
    if (mine && !locked && ImGui::BeginDragDropTarget()) {
      if (ImGui::AcceptDragDropPayload("INV_ITEM")) {
        if (auto* iv = Bourgeon::Instance().inventory_viewer()) iv->TradeDraggedItem();
      }
      ImGui::EndDragDropTarget();
    }
    ImGui::TextColored(locked ? kGreen : kBlack, locked ? "  Verrouillé" : "  En cours");
  };

  // ── Ma colonne / colonne partenaire ──
  const float col = (ImGui::GetContentRegionAvail().x - 8.0f) * 0.5f;
  ImGui::BeginGroup();
  ImGui::TextColored(kBlack, "Mon offre  (%lldz)", static_cast<long long>(my_zeny_));
  draw_items("trade_mine", my_items_, my_locked_, col, true);
  ImGui::EndGroup();
  ImGui::SameLine();  // (colonne partenaire ci-dessous)
  ImGui::BeginGroup();
  ImGui::TextColored(kBlack, "Partenaire  (%lldz)",
                     static_cast<long long>(partner_zeny_));
  draw_items("trade_partner", partner_items_, partner_locked_, col, false);
  ImGui::EndGroup();

  // Aperçu au survol : le tooltip RO complet, cartes / options / refine compris.
  // Peint ICI, hors des deux enfants — DrawTooltip crée son propre popup.
  if (hover)
    itemcell::DrawTooltip(hover->id, hover->cards, 4, hover->opts,
                          hover->opt_count, hover->refine,
                          hover->name[0] ? hover->name : nullptr,
                          hover->damaged != 0);
  // Clic droit : la fenêtre de description NATIVE, complète. DIFFÉRÉE par la brique
  // partagée, comme les huit autres viewers — elle s'ouvre au RELÂCHEMENT du bouton,
  // pas au tick suivant. Les deux raisons sont dans features/item_cell.h : MakeWindow
  // est une commande native (jamais au milieu d'une frame ImGui), et surtout le focus
  // reste acquis à la fenêtre cliquée tant que le bouton est enfoncé — ouvrir avant le
  // relâchement faisait passer la description DERRIÈRE sur un appui prolongé.
  //
  // ⚠ L'ItemSkillInfo doit vivre jusqu'au flush : on le reconstruit dans le tampon
  // MEMBRE, pas sur la pile. C'est ce qui interdisait d'employer la brique jusqu'ici.
  // Une seule demande en vol (une souris, un geste) : un tampon suffit.
  if (desc_for) {
    static_assert(sizeof(desc_info_) >= kInfoSize, "tampon de description trop petit");
    BuildTradeInfo(*desc_for, desc_info_);
    itemcell::DeferDescById(desc_for->id, desc_for->look, desc_for->location,
                            desc_x, desc_y, desc_info_);
  }

  ImGui::Separator();

  // ── Zeny (mon offre) + option Screenshot ──
  // Le natif applique le zeny AU VERROUILLAGE (OnOkButton = cmd 0x33 index0 PUIS
  // cmd 0x34) : il n'y a pas de « définir » séparé, et un cmd 0x33 isolé reste en
  // attente (rien ne s'affiche tant qu'on n'a pas verrouillé). On calque ce modèle.
  ImGui::BeginDisabled(my_locked_);
  ImGui::TextColored(kBlack, "Zeny à offrir :");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120.0f);
  ImGui::InputInt("##trade_zeny", &zeny_input_, 0, 0);
  if (zeny_input_ < 0) zeny_input_ = 0;
  // Case « Screenshot Trade » du natif — libellé lu dans la table de messages du
  // client (jamais en dur), repli si la table n'est pas encore chargée.
  const char* scr_label = msgstr::Utf8(kMsgScrLabel);
  ro::RoCheckbox(scr_label[0] ? scr_label : "Screenshot Trade", &screenshot_);
  ImGui::EndDisabled();
  ImGui::TextDisabled("Le zeny est validé en cliquant sur « Verrouiller (OK) ».");
  ImGui::TextDisabled(
      "Ajout d'objets : glissez-les depuis l'inventaire vers « Mon offre », ou "
      "clic-droit → « Vers l'échange » (ou Alt+clic droit).");

  ImGui::Separator();

  // ── Boutons d'action ──
  if (my_locked_) ImGui::BeginDisabled();
  if (ro::RoButton(my_locked_ ? "Verrouillé" : "Verrouiller (OK)", 150.0f, 0.0f)) {
    SetZeny(zeny_input_);  // cmd 0x33(index 0) : pose le zeny...
    Lock();                // ...puis cmd 0x34 verrouille (séquence exacte du natif)
  }
  if (my_locked_) ImGui::EndDisabled();
  ImGui::SameLine();
  // Comme le natif (qui grise son bouton « trade ») : commit seulement quand les DEUX
  // côtés sont verrouillés. L'échange n'aboutit que si LES DEUX joueurs cliquent.
  const bool can_commit = my_locked_ && partner_locked_;
  if (committed_) {
    // Déjà validé de mon côté : l'échange ne s'exécute qu'une fois que l'AUTRE joueur
    // a validé aussi. On l'affiche explicitement au lieu d'un bouton qui « ne fait rien ».
    ImGui::BeginDisabled();
    ro::RoButton("En attente...", 150.0f, 0.0f);
    ImGui::EndDisabled();
  } else {
    if (!can_commit) ImGui::BeginDisabled();
    if (ro::RoButton("Échanger", 150.0f, 0.0f)) Commit();
    if (!can_commit) ImGui::EndDisabled();
  }
  ImGui::SameLine();
  if (ro::RoButton("Annuler", 100.0f, 0.0f)) CloseTrade();

  // Ligne d'état : dit toujours ce qu'on attend (verrou, validation, autre joueur).
  if (committed_)
    ImGui::TextColored(kBlack, "Validé — en attente de l'autre joueur...");
  else if (can_commit)
    ImGui::TextColored(kBlack, "Les deux offres sont verrouillées : cliquez « Échanger ».");
  else if (my_locked_)
    ImGui::TextDisabled("En attente du verrouillage de l'autre joueur...");
  else
    ImGui::TextDisabled("Verrouillez votre offre quand elle est prête.");

  // Toasts (résultat / erreur d'ajout).
  if (add_error_ > 0) {
    const char* msg = "Ajout refusé";
    switch (add_error_) {
      case 1: msg = "Surpoids"; break;
      case 2: msg = "Échange annulé"; break;
      case 3: msg = "Inventaire plein"; break;
      case 4: msg = "Quantité de stack dépassée"; break;
      case kErrAlreadyAllOffered: msg = "Tout ce que vous en avez est déjà offert"; break;
      case kErrTradeFull: msg = "Offre pleine (10 objets au maximum)"; break;
    }
    ImGui::TextColored(kRed, "%s", msg);
  }
  if (last_result_ == 0) ImGui::TextColored(kGreen, "Échange réussi.");
  else if (last_result_ == 1) ImGui::TextColored(kRed, "Échange échoué.");

  ro::EndRoWindow();
  ImGui::PopStyleVar(4);
}
