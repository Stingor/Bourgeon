#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "features/plugin.h"

// ── TradeWindow ──────────────────────────────────────────────────────────────
//
// Remplacement ImGui (skin RO) de l'échange joueur-joueur (« Deal »). En
// REMPLACEMENT du natif : les fenêtres natives sont cachées dès leur création
// (par VTABLE, cf. ci-dessous) et on affiche une fenêtre ImGui unifiée (2 grilles
// d'objets + zeny + boutons Verrouiller / Échanger / Annuler) plus une petite
// popup de requête « X souhaite échanger ».
//
// RE complète : cf. docs/trade_window_re.md et la mémoire project_trade_window_re.
// ⚠ La fenêtre VIVANTE est la NOUVELLE classe CUIExchangeUI (RE live 2026-07-23),
// PAS l'ancienne UIExchangeWnd (0x01031edc, morte). Vtables (RTTI live, 20250716) :
//   CUIExchangeUI        vtable 0x010457d8  — fenêtre d'échange, id MAP pinné 0x271b.
//   UIExchangeAcceptWnd  vtable 0x01033754  — popup requête id 0x20 (best-effort).
//
// SOURCE DES DONNÉES : la fenêtre native est CACHÉE (pas détruite) et continue de
// traiter les paquets serveur. Les OBJETS sont lus dans les tableaux GLOBAUX de la
// session (indépendant de la fenêtre) : ItemSkillInfo ×10 stride 0xF8 à 0x015fe250
// (moi) / 0x015fec00 (partenaire). Verrous = octet +0xC4 des widgets liste +0xE4/
// +0xF8 ; zeny du deal DAT_015ff5b0/DAT_015ff5b4. Aucun parsing de paquet requis.
//
// ACTIONS : émises via CMode::SendMsg (GameMode_GetActive(0x1213338) -> vf+0x18)
// avec les mêmes commandes que la fenêtre native (0x33/0x34 confirmés live) :
//   cmd 0x32(3/4) = accepter/refuser la requête  -> CZ_ACK_EXCHANGE_ITEM 0x00e6
//   cmd 0x33(index, amount) = ajouter (index 0 = zeny) -> CZ_ADD_EXCHANGE_ITEM 0x00e8
//   cmd 0x34 = verrouiller (OK)                  -> CZ_CONCLUDE_EXCHANGE_ITEM 0x00eb
//   cmd 0x35 = annuler                           -> CZ_CANCEL_EXCHANGE_ITEM 0x00ed
//   cmd 0x36 = valider (commit)                  -> CZ_EXEC_EXCHANGE_ITEM 0x00ef
// Le serveur (moonlight src/map/trade.cpp) valide TOUT : aucun exploit possible.
//
// Défaut OFF (opt-in, setting « trade_imgui » géré par MoonlightUi) : on n'impose
// pas un changement de gameplay.

class TradeWindow : public Plugin {
 public:
  TradeWindow();  // enregistre les opcodes observés (0x01f4/0x01f5/0x00ea/0x00f0)

  const char* name() const override { return "TradeWindow"; }

  // Un objet offert (résolu depuis la liste native). Public : la lecture de liste
  // (fonction libre) et l'intégration inventaire y accèdent.
  struct TradeItem {
    uint32_t id = 0;
    int      amount = 0;
    int      slots = 0;
    int      refine = 0;
  };

  void OnTick() override;
  void OnRenderUI() override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Appelé pour CHAQUE fenêtre créée (hook MakeWindow, sans filtre d'id — l'id de
  // la fenêtre principale n'est pas connu statiquement). Détecte les 3 vtables de
  // l'échange : capture l'objet + l'id runtime de UIExchangeWnd, et cache le natif
  // (+0x28=0). No-op si le viewer est désactivé.
  void HideNativeAtCreation(void* win, int windowID);

  // Ajoute `amount` unités de l'item à l'index inventaire `invIndex` au deal (cmd
  // 0x33). Public pour l'intégration avec l'InventoryViewer (clic/drag « ajouter à
  // l'échange »). L'ajout de zeny passe par le champ dédié de la fenêtre.
  // `invIndex` = index ItemSkillInfo (info+4), MÊME convention que les commandes
  // use/équiper/transfert de l'InventoryViewer ; un vrai item ne vaut jamais 0
  // (l'index 0 est réservé au zeny), donc aucune confusion possible.
  void AddItemToTrade(int invIndex, int amount);

  // True si l'échange ImGui est actif (viewer activé + fenêtre d'échange ouverte).
  // Lu par l'InventoryViewer pour proposer « Vers l'échange ».
  bool active() const { return imgui_enabled_ && open_; }

  // Setting PERSISTANT (« trade_imgui », géré par MoonlightUi). Défaut OFF.
  bool imgui_enabled_ = false;

 private:
  // Actions -> CMode::SendMsg.
  void TradeAck(int type);          // 3 = accepter, 4 = refuser
  void SetZeny(int amount);         // cmd 0x33(index 0, amount) — zeny absolu
  void Lock();                      // cmd 0x34 (verrou)
  void Cancel();                    // cmd 0x35 (annuler)
  void Commit();                    // cmd 0x36 (valider l'échange)

  // Lit l'état résolu de la fenêtre native cachée `w` (listes + verrous). SEH/POD.
  void ReadNativeState(void* w);
  // Ferme proprement : envoie cmd 0x35 (annule) pour débloquer l'état dialogue
  // client, puis réinitialise. Appelé au clic X ou sur ZC_CANCEL/EXEC.
  void CloseTrade();

  bool     open_ = false;        // fenêtre d'échange principale active ?
  bool     was_open_ = false;
  bool     req_open_ = false;    // popup de requête active ?
  bool     need_pos_ = true;
  bool     show_panel_ = true;   // détection du clic sur le X (fenêtre principale)
  int      spawn_x_ = 140, spawn_y_ = 120;

  int      main_id_ = -1;        // id runtime de UIExchangeWnd (capturé au 1er create)
  void*    main_win_ = nullptr;  // pointeur live de UIExchangeWnd (hook OU map-walk)

  // Requête entrante (popup) : nom + niveau du demandeur (ZC_REQ 0x01f4).
  char     req_name_[25] = {0};
  int      req_level_ = 0;
  uint32_t req_aid_ = 0;

  // État lu de la fenêtre native.
  std::vector<TradeItem> my_items_;
  std::vector<TradeItem> partner_items_;
  bool     my_locked_ = false;       // +0xCC (mon zeny/objets verrouillés)
  bool     partner_locked_ = false;  // +0xD0 (partenaire verrouillé)
  int64_t  my_zeny_ = 0;             // DAT_015ff5b0
  int64_t  partner_zeny_ = 0;        // DAT_015ff5b4

  int      zeny_input_ = 0;          // champ de saisie zeny (ImGui) — appliqué au verrou
  bool     screenshot_ = false;      // case « Screenshot Trade » (cmd 0x44 au commit)
  bool     committed_ = false;       // j'ai validé (cmd 0x36) -> on attend l'autre joueur
  int      last_result_ = -1;        // ZC_EXEC 0x00f0 (0 = succès, 1 = échec)
  int      add_error_ = -1;          // ZC_ACK_ADD 0x00ea (dernier code d'erreur)
};
