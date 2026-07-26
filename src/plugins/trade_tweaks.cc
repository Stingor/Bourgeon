#include "plugins/trade_tweaks.h"

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
#include "imgui.h"
#include "plugins/imgui_escape.h"
#include "plugins/inventory_viewer.h"  // TradeDraggedItem (cible de drag-drop "INV_ITEM")
#include "ui/ro_imgui.h"  // BeginRoWindow (skin RO)

// ── Constantes RE (client 20250716, base 0x400000 ; cf. docs/trade_window_re.md) ──
namespace {

// UIWindowMgr + factory.
constexpr uintptr_t kCloseWindow = 0x00a2e770;  // SaveWindowRect+close (mgr, edx, id)
using CloseWindow_t = void (__fastcall*)(void*, void*, int);

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
constexpr int kOffVisible     = 0x28;   // flag visibilité (gate du render mgr)
constexpr int kOffMyListW     = 0xE4;   // widget liste MES objets
constexpr int kOffPtListW     = 0xF8;   // widget liste objets PARTENAIRE
constexpr int kOffWidgetLock  = 0xC4;   // (widget liste) octet : côté verrouillé (>0)

// Objets du deal : tableau ItemSkillInfo (stride 0xF8, 10 slots) DANS la session
// (g_session 0x015fa3c0), lu par les getters FUN_00d59cd0 (moi) / FUN_00d5cfd0
// (partenaire) : ItemSkillInfo_CopyFull(out, session + slot*0xF8 + base). Source
// UI-INDÉPENDANTE (adresses globales fixes), bien plus robuste que les widgets.
constexpr uintptr_t kSession     = 0x015fa3c0;
constexpr uintptr_t kMyDealItems = kSession + 0x3e90;  // = 0x015fe250
constexpr uintptr_t kPtDealItems = kSession + 0x4840;  // = 0x015fec00
constexpr int kDealStride = 0xF8;
constexpr int kDealSlots  = 10;
// Offsets DANS l'ItemSkillInfo (identiques à character_sheet / inventory_viewer).
constexpr int kInfoAmount = 0x10;   // int : quantité (< 1 => slot vide)
constexpr int kInfoIdStr  = 0x2c;   // std::string SSO : itemId EN TEXTE (atoi)
constexpr int kInfoIdCap  = 0x40;   // capacité SSO (+0x2c+0x14 ; >15 => heap)
constexpr int kInfoRefine = 0x60;   // int : refine

// Zeny du deal (globals) + zeny du joueur.
constexpr uintptr_t kMyDealZeny = 0x015ff5b0;  // int32 : zeny que j'offre
constexpr uintptr_t kPtDealZeny = 0x015ff5b4;  // int32 : zeny offert par le partenaire
constexpr uintptr_t kPlayerZeny = 0x015fba90;  // uint32 : mon zeny total

// Bus de commandes = CMode::SendMsg. Réplique GameMode_GetActive(0x1213338) :
// mode courant = *(0x1213338+4) [= *(0x121333c)] SEULEMENT si *(0x1213338+0x58)==1
// (mode actif). vf+0x18 (slot 6) = le dispatcher use/equip/transfert/deal.
constexpr uintptr_t kModeMgr = 0x01213338;
// Commandes de l'échange — TOUTES vérifiées live sur les handlers de boutons natifs
// (OK = CUIExchangeUI_OnOkButton 0x009ce140, trade = FUN_009ce040, cancel = FUN_009ce000).
constexpr int kCmdAck      = 0x32; // (type 3=accept / 4=reject) -> CZ_ACK 0x00e6
constexpr int kCmdAdd      = 0x33; // (index, amount) index 0=zeny -> CZ_ADD 0x00e8
constexpr int kCmdConclude = 0x34; // (verrou/OK)                -> CZ_CONCLUDE 0x00eb
constexpr int kCmdCancel   = 0x35; // (annuler)                  -> CZ_CANCEL 0x00ed
constexpr int kCmdExec     = 0x36; // (valider/commit)           -> CZ_EXEC 0x00ef
// Envoyé juste APRÈS un cmd 0x33 d'OBJET par le drop natif (OnMsg case 0x26) : sans lui
// l'objet n'est pas réellement poussé dans le deal (c'était le bug « ajout sans effet »).
constexpr int kCmdApplyAdd = 0x12;
// « Screenshot Trade » : si la case est cochée, le bouton trade natif envoie cmd 0x44
// avec le texte MsgStringTable(0x728) AVANT le cmd 0x36 (cf. FUN_009ce040).
constexpr int kCmdScreenshot = 0x44;

// Table de messages localisés du client (JAMAIS de texte en dur : on lit le natif).
constexpr uintptr_t kMsgStringGet   = 0x00a9ed30;  // __cdecl(id) -> const char*
constexpr int       kMsgScrLabel    = 0x727;       // libellé de la case « Screenshot Trade »
constexpr int       kMsgScrPayload  = 0x728;       // texte envoyé avec le cmd 0x44
using MsgStringGet_t = const char* (__cdecl*)(int);

// Libellé natif par id (SEH : la table peut ne pas être chargée). Renvoie "" si absent.
const char* MsgString(int id) {
  __try {
    const char* s = reinterpret_cast<MsgStringGet_t>(kMsgStringGet)(id);
    return s ? s : "";
  } __except (EXCEPTION_EXECUTE_HANDLER) { return ""; }
}

// Opcodes observés (vanilla ; handler natif intact -> on OBSERVE, jamais Register-
// RecvOpcode). data = payload après l'opcode 2 octets ; len = forward_len.
constexpr uint16_t kOpReq    = 0x01f4;  // ZC_REQ_EXCHANGE_ITEM {name[24],targetId:4,targetLv:2}
constexpr uint16_t kOpAck    = 0x01f5;  // ZC_ACK_EXCHANGE_ITEM {result:1,targetId:4,targetLv:2}
constexpr uint16_t kOpAckAdd = 0x00ea;  // ZC_ACK_ADD_EXCHANGE_ITEM {index:2,result:1}
constexpr uint16_t kOpCancel = 0x00ee;  // ZC_CANCEL_EXCHANGE_ITEM
constexpr uint16_t kOpExec   = 0x00f0;  // ZC_EXEC_EXCHANGE_ITEM {result:1}

// ── Fenêtres natives (SEH-gardé) ──
void* FindWnd(int id) {
  if (id < 0) return nullptr;
  __try {
    return uiwnd::FindWindow(id);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void HideWnd(void* w) {
  __try {
    if (w) *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(w) + kOffVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Détruit une fenêtre native par id (persiste sa position + close), comme le X natif.
void CloseWnd(int id) {
  __try {
    reinterpret_cast<CloseWindow_t>(kCloseWindow)(
        uiwnd::Mgr(), nullptr, id);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
uintptr_t VTableOf(void* w) {
  __try { return w ? *reinterpret_cast<uintptr_t*>(w) : 0; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

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
    uint8_t* mgr = reinterpret_cast<uint8_t*>(kModeMgr);
    if (*reinterpret_cast<int*>(mgr + 0x58) != 1) return;  // aucun mode actif
    void* disp = *reinterpret_cast<void**>(mgr + 4);        // = *(0x121333c)
    if (disp) {
      void** vt = *reinterpret_cast<void***>(disp);
      using Fn = int(__thiscall*)(void*, int, int, int, int, int);
      reinterpret_cast<Fn>(vt[6])(disp, cmd, a, b, c, d);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Résolution nom d'item (id -> nom) — identique à ShopTweaks ──
constexpr uintptr_t kDescDbLookup = 0x006a0d40;
constexpr uintptr_t kDescDb       = 0x01255130;
constexpr uintptr_t kDescDbNil    = 0x01255138;
constexpr uintptr_t kEnsureLoaded = 0x006a06b0;
constexpr uintptr_t kEnsureCache  = 0x0125510c;
using DescLookup_t   = void*(__cdecl*)(int, void*);
using EnsureLoaded_t = char (__thiscall*)(void*, int);

std::unordered_map<uint32_t, std::string> g_name_cache;

void ResolveNameSEH(uint32_t id, char* out, size_t cap) {
  out[0] = '\0';
  __try {
    void* cache = *reinterpret_cast<void**>(kEnsureCache);
    if (cache)
      reinterpret_cast<EnsureLoaded_t>(kEnsureLoaded)(cache, static_cast<int>(id));
    void* rec = reinterpret_cast<DescLookup_t>(kDescDbLookup)(
        static_cast<int>(id), reinterpret_cast<void*>(kDescDb));
    if (rec && rec != reinterpret_cast<void*>(kDescDbNil)) {
      const char* nm = *reinterpret_cast<char**>(reinterpret_cast<char*>(rec) + 4);
      if (nm) { std::strncpy(out, nm, cap - 1); out[cap - 1] = '\0'; }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

const char* ItemName(uint32_t id) {
  auto it = g_name_cache.find(id);
  if (it != g_name_cache.end()) return it->second.c_str();
  char buf[64];
  ResolveNameSEH(id, buf, sizeof(buf));
  if (buf[0] == '\0') std::snprintf(buf, sizeof(buf), "#%u", id);
  return (g_name_cache[id] = buf).c_str();
}

// ── Icônes ImGui (cache id -> texture) — identique à ShopTweaks ──
struct IconTex { void* tex = nullptr; int w = 0; int h = 0; };
std::unordered_map<uint32_t, IconTex> g_icon_cache;

constexpr uintptr_t kBuildIconPath = 0x00d5a720;  // __stdcall(id_str, out[128], identified)
constexpr uintptr_t kTexMgr  = 0x00a90350;
constexpr uintptr_t kMakeKey = 0x00a9f030;
constexpr uintptr_t kLoadTex = 0x00a8d4a0;
constexpr int kTexW = 0x114, kTexH = 0x118, kTexPix = 0x11c;
using BuildIconPath_t = void*(__stdcall*)(const char*, char*, int);
using TexMgr_t  = void*(__cdecl*)();
using MakeKey_t = void*(__cdecl*)(const char*);
using LoadTex_t = void*(__fastcall*)(void*, void*, void*);

struct RawTex { const uint8_t* bgra; int w; int h; };
bool GetRawTex(const char* path, RawTex* out) {
  __try {
    void* mgr = reinterpret_cast<TexMgr_t>(kTexMgr)();
    if (!mgr) return false;
    void* key = reinterpret_cast<MakeKey_t>(kMakeKey)(path);
    if (!key) return false;
    void* t = reinterpret_cast<LoadTex_t>(kLoadTex)(mgr, nullptr, key);
    if (!t) return false;
    const int w = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexW);
    const int h = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexH);
    const uint8_t* bgra =
        *reinterpret_cast<const uint8_t**>(static_cast<char*>(t) + kTexPix);
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || !bgra) return false;
    out->bgra = bgra; out->w = w; out->h = h;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool BuildIconPathSafe(uint32_t id, char* out) {
  char idstr[16];
  std::snprintf(idstr, sizeof(idstr), "%u", id);
  out[0] = '\0';
  __try {
    reinterpret_cast<BuildIconPath_t>(kBuildIconPath)(idstr, out, 1);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

IconTex LoadItemIcon(uint32_t id) {
  char path[192];
  if (!BuildIconPathSafe(id, path)) return {};
  RawTex rt{};
  if (!GetRawTex(path, &rt)) return {};
  std::vector<uint8_t> argb(static_cast<size_t>(rt.w) * rt.h * 4);
  for (int i = 0; i < rt.w * rt.h; ++i) {
    const uint8_t b = rt.bgra[i * 4], g = rt.bgra[i * 4 + 1], r = rt.bgra[i * 4 + 2];
    const bool ck = (r == 0xFF && g == 0 && b == 0xFF);  // magenta -> transparent
    argb[i * 4] = b; argb[i * 4 + 1] = g; argb[i * 4 + 2] = r;
    argb[i * 4 + 3] = ck ? 0 : 0xFF;
  }
  return {Overlay_CreateTextureARGB(argb.data(), rt.w, rt.h), rt.w, rt.h};
}

IconTex ResolveIcon(uint32_t id) {
  static unsigned s_epoch = 0;
  const unsigned e = Overlay_DeviceEpoch();  // device reset -> textures mortes
  if (e != s_epoch) { g_icon_cache.clear(); s_epoch = e; }
  auto it = g_icon_cache.find(id);
  if (it != g_icon_cache.end()) return it->second;
  return g_icon_cache[id] = LoadItemIcon(id);
}

// Lit le tableau d'objets du deal (ItemSkillInfo ×10) à l'adresse globale `arrayBase`.
void ReadDealItems(uintptr_t arrayBase, std::vector<TradeTweaks::TradeItem>* out);

}  // namespace

// Rendu ici pour accéder au type privé TradeTweaks::TradeItem.
namespace {
void ReadDealItems(uintptr_t arrayBase,
                   std::vector<TradeTweaks::TradeItem>* out) {
  out->clear();
  __try {
    for (int slot = 0; slot < kDealSlots; ++slot) {
      uint8_t* e = reinterpret_cast<uint8_t*>(arrayBase +
                                              static_cast<uintptr_t>(slot) * kDealStride);
      const int amount = *reinterpret_cast<int*>(e + kInfoAmount);
      if (amount < 1) continue;  // slot vide (ItemSkillInfo+0x10 < 1)
      TradeTweaks::TradeItem it;
      it.amount = amount;
      // itemId EN TEXTE : std::string SSO à +0x2c (heap si capacité +0x40 > 15) -> atoi.
      const char* sbase = reinterpret_cast<const char*>(e + kInfoIdStr);
      const uint32_t cap = *reinterpret_cast<const uint32_t*>(e + kInfoIdCap);
      const char* str = (cap > 15) ? *reinterpret_cast<const char* const*>(sbase)
                                   : sbase;
      it.id     = str ? static_cast<uint32_t>(std::atoi(str)) : 0;
      it.refine = *reinterpret_cast<int*>(e + kInfoRefine);
      if (it.id) out->push_back(it);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { /* array incohérent : on garde l'acquis */ }
}
}  // namespace

TradeTweaks::TradeTweaks() {
  // TOUT en OBSERVE (jamais RegisterRecvOpcode) : le handler natif doit TOUJOURS
  // tourner (il peuple la fenêtre native qu'on lit, et le natif reste correct quand
  // le toggle est OFF). On lit juste ces paquets pour l'ouverture/fermeture et les
  // toasts ; les listes d'objets sont lues depuis la fenêtre native cachée.
  Bourgeon::Instance().RegisterObserveOpcode(kOpReq, 30);    // name[24]+targetId+targetLv
  Bourgeon::Instance().RegisterObserveOpcode(kOpAck, 7);     // result+targetId+targetLv
  Bourgeon::Instance().RegisterObserveOpcode(kOpAckAdd, 3);  // index+result
  Bourgeon::Instance().RegisterObserveOpcode(kOpCancel, 1);  // (pas de payload utile)
  Bourgeon::Instance().RegisterObserveOpcode(kOpExec, 1);    // result
}

void TradeTweaks::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  if (!imgui_enabled_) return;
  // Thread recv : on ne fait qu'écrire des membres (jamais de SendPacket / CMode ici).
  switch (opcode) {
    case kOpReq: {  // ZC_REQ_EXCHANGE_ITEM : quelqu'un demande un échange.
      if (len < 30) return;
      std::memcpy(req_name_, data, 24);
      req_name_[24] = '\0';
      req_aid_   = *reinterpret_cast<const uint32_t*>(data + 24);
      req_level_ = *reinterpret_cast<const uint16_t*>(data + 28);
      req_open_  = true;
      break;
    }
    case kOpAck: {  // ZC_ACK_EXCHANGE_ITEM : réponse à la requête.
      if (len < 1) return;
      const int result = data[0];  // 3 = accepté (l'échange démarre), sinon échec.
      req_open_ = false;
      if (result != 3) last_result_ = -1;  // requête refusée/échouée (pas un commit)
      break;
    }
    case kOpAckAdd:  // ZC_ACK_ADD_EXCHANGE_ITEM {index:2, result:1}
      if (len >= 3) add_error_ = data[2];  // 0 = ok, sinon surpoids/plein/stack...
      break;
    case kOpCancel:  // ZC_CANCEL_EXCHANGE_ITEM : l'échange est annulé.
      last_result_ = 2;   // marqueur "annulé" (distinct de 0=ok / 1=échec)
      committed_ = false; // l'attente est terminée
      break;
    case kOpExec:  // ZC_EXEC_EXCHANGE_ITEM {result:1} : échange terminé.
      last_result_ = (len >= 1) ? data[0] : 0;
      committed_ = false;
      break;
    default:
      break;
  }
}

// ── Actions (CMode::SendMsg — thread principal uniquement) ──
void TradeTweaks::TradeAck(int type) {
  // Réplique la réponse native : SaveWindowRect(0x20) qui persiste ET FERME la popup
  // native (qu'on avait seulement cachée), puis cmd 0x32(type). Sans cette fermeture,
  // OnTick retrouve la fenêtre native 0x20 et rouvre la popup ImGui (échange refusé
  // = fenêtre qui reste ouverte).
  CloseWnd(kAcceptId);
  ModeCmd(kCmdAck, type, 0, 0, 0);
  req_open_ = false;
}
void TradeTweaks::SetZeny(int amount) {
  if (amount < 0) amount = 0;
  const uint32_t zmax = *reinterpret_cast<uint32_t*>(kPlayerZeny);
  if (static_cast<uint32_t>(amount) > zmax) amount = static_cast<int>(zmax);
  // index 0 = zeny ; le serveur pose deal.zeny = amount (valeur ABSOLUE, clampée).
  ModeCmd(kCmdAdd, 0, amount, 0, 0);
}
void TradeTweaks::Lock()   { ModeCmd(kCmdConclude, 0, 0, 0, 0); }
void TradeTweaks::Cancel() { ModeCmd(kCmdCancel, 0, 0, 0, 0); }

void TradeTweaks::Commit() {
  // Réplique EXACTEMENT le bouton « trade » natif (FUN_009ce040) : si la case
  // Screenshot est cochée, cmd 0x44(texte natif) AVANT le commit, puis cmd 0x36.
  // ⚠ Le serveur n'exécute l'échange que quand les DEUX joueurs ont validé.
  if (screenshot_) {
    const char* txt = MsgString(kMsgScrPayload);
    ModeCmd(kCmdScreenshot,
            static_cast<int>(reinterpret_cast<uintptr_t>(txt)), 0, 0, 0);
  }
  ModeCmd(kCmdExec, 0, 0, 0, 0);
  // Le natif grise aussi son bouton « trade » après l'envoi : l'échange ne s'exécute
  // que quand l'AUTRE joueur a validé à son tour. On bascule en « en attente ».
  committed_ = true;
}

void TradeTweaks::AddItemToTrade(int invIndex, int amount) {
  if (!imgui_enabled_ || !open_ || my_locked_) return;
  if (amount < 1) amount = 1;
  // Le drop natif (OnMsg case 0x26) fait cmd 0x33 PUIS cmd 0x12 : sans le 0x12
  // l'objet n'est pas poussé dans le deal (ajout resté sans effet).
  ModeCmd(kCmdAdd, invIndex, amount, 0, 0);  // index != 0 -> objet
  ModeCmd(kCmdApplyAdd, 0, 0, 0, 0);
}

void TradeTweaks::CloseTrade() {
  // Ferme le deal côté serveur ET débloque l'état dialogue client : cmd 0x35 (annule)
  // = exactement ce que fait le bouton Annuler natif.
  Cancel();
  // Détruit la fenêtre native (qu'on cachait) : sinon elle persiste le temps du
  // round-trip serveur et OnTick la re-détecte (map-walk) -> le viewer se rouvre.
  if (main_id_ >= 0) CloseWnd(main_id_);
  main_win_ = nullptr;  // pointeur invalidé (fenêtre détruite)
  open_ = false;
  was_open_ = false;
  show_panel_ = true;
  my_items_.clear();
  partner_items_.clear();
  my_locked_ = partner_locked_ = false;
  zeny_input_ = 0;
  committed_ = false;
}

void TradeTweaks::ReadNativeState(void* w) {
  // Objets : tableaux ItemSkillInfo globaux de la session (indépendant de la fenêtre).
  ReadDealItems(kMyDealItems, &my_items_);
  ReadDealItems(kPtDealItems, &partner_items_);
  // Verrous : octet +0xC4 du widget liste (moi +0xE4 / partenaire +0xF8), exactement
  // ce que teste FUN_009cecd0 pour (dé)griser « Échanger » (commit ssi les deux >0).
  __try {
    uint8_t* base = reinterpret_cast<uint8_t*>(w);
    uint8_t* myw = *reinterpret_cast<uint8_t**>(base + kOffMyListW);
    uint8_t* ptw = *reinterpret_cast<uint8_t**>(base + kOffPtListW);
    my_locked_      = myw && *(myw + kOffWidgetLock) != 0;
    partner_locked_ = ptw && *(ptw + kOffWidgetLock) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  __try {
    my_zeny_      = *reinterpret_cast<int32_t*>(kMyDealZeny);
    partner_zeny_ = *reinterpret_cast<int32_t*>(kPtDealZeny);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void TradeTweaks::HideNativeAtCreation(void* win, int windowID) {
  if (!win || !imgui_enabled_) return;
  const uintptr_t vt = VTableOf(win);
  if (vt == kExchangeVTable) {
    main_id_ = windowID;  // id MAP capturé au runtime (inconnu statiquement)
    main_win_ = win;      // pointeur live (accélère OnTick ; sinon map-walk le retrouve)
    open_ = true;
    need_pos_ = true;
    show_panel_ = true;
    last_result_ = -1;
    add_error_ = -1;
    HideWnd(win);
  } else if (vt == kAcceptVTable) {
    HideWnd(win);  // popup requête (0x20) — best-effort
  }
}

void TradeTweaks::OnTick() {
  if (!imgui_enabled_) { open_ = false; was_open_ = false; req_open_ = false; return; }

  // Popup de requête : présente tant que la fenêtre native 0x20 existe.
  void* accept = FindWnd(kAcceptId);
  if (accept && VTableOf(accept) == kAcceptVTable) {
    HideWnd(accept);
    req_open_ = true;
  } else if (req_open_ && !accept) {
    req_open_ = false;  // la popup native a disparu (répondu / timeout)
  }

  // Fenêtre d'échange principale : on privilégie le pointeur du hook s'il est encore
  // valide, sinon on la (re)cherche dans la map par VTABLE (indépendant du hook et de
  // l'id) — c'est ça qui garantit l'ouverture même si le hook MakeWindow ne l'a pas
  // capturée. La recherche récupère aussi l'id (clé du nœud) pour la fermeture propre.
  void* w = (main_win_ && VTableOf(main_win_) == kExchangeVTable)
                ? main_win_
                : FindTradeWnd(&main_id_);
  if (w) {
    main_win_ = w;
    open_ = true;
    HideWnd(w);
    if (!was_open_) {
      need_pos_ = true;
      zeny_input_ = 0;
      screenshot_ = false;
      committed_ = false;
      // Le zeny de deal (global natif) n'est PAS remis à zéro entre deux échanges :
      // sans ça « Mon offre » affiche le montant du trade précédent, et le verrou le
      // renvoyait au serveur. On repart de 0 pour ce nouvel échange.
      __try { *reinterpret_cast<int32_t*>(kMyDealZeny) = 0; }
      __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    ReadNativeState(w);
  } else if (open_) {
    // La fenêtre native a disparu (commit / annulation / warp) : on ferme le viewer.
    open_ = false;
    main_win_ = nullptr;
    show_panel_ = true;
    my_items_.clear();
    partner_items_.clear();
    my_locked_ = partner_locked_ = false;
  }
  was_open_ = open_;
}

void TradeTweaks::OnRenderUI() {
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
      IconTex ic = ResolveIcon(it.id);
      if (ic.tex) {
        ImGui::Image(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(20, 20));
        ImGui::SameLine();
      }
      if (it.refine > 0)
        ImGui::TextColored(kBlack, "+%d %s", it.refine, ItemName(it.id));
      else
        ImGui::TextColored(kBlack, "%s", ItemName(it.id));
      if (it.amount > 1) { ImGui::SameLine(); ImGui::TextDisabled("x%d", it.amount); }
      if (it.slots > 0) { ImGui::SameLine(); ImGui::TextDisabled("[%d]", it.slots); }
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
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextColored(kBlack, "Partenaire  (%lldz)",
                     static_cast<long long>(partner_zeny_));
  draw_items("trade_partner", partner_items_, partner_locked_, col, false);
  ImGui::EndGroup();

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
  const char* scr_label = MsgString(kMsgScrLabel);
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
    }
    ImGui::TextColored(kRed, "%s", msg);
  }
  if (last_result_ == 0) ImGui::TextColored(kGreen, "Échange réussi.");
  else if (last_result_ == 1) ImGui::TextColored(kRed, "Échange échoué.");

  ro::EndRoWindow();
  ImGui::PopStyleVar(4);
}
