#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "features/plugin.h"
#include "features/windows/item_desc_window.h"  // itemdesc::SimpleOpt

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
// 🔴 Les fenêtres natives ne NAISSENT PLUS : les paquets qui les créaient sont
// REMPLACÉS (RegisterReplaceOpcode, révocable par l'interrupteur). Elles étaient
// auparavant créées puis CACHÉES — or une native masquée garde le CLAVIER, et son
// bouton par défaut agit : sur un échange, Entrée engage des objets. C'est le trou
// qui avait détruit une arme au refine, avec l'inventaire en jeu cette fois.
//
// SOURCE DES DONNÉES — elle a changé de nature avec le remplacement. Les objets
// venaient de deux tableaux GLOBAUX de session (ItemSkillInfo ×10 à 0x015fe250 et
// 0x015fec00) que les handlers natifs remplissaient ; en prenant leur place on les
// assèche. Les deux offres se reconstruisent donc depuis les paquets :
//   ZC_REQ_EXCHANGE_ITEM   0x01f4 {name[24],targetId:4,targetLv:2} -> popup requête
//   ZC_ACK_EXCHANGE_ITEM   0x01f5 {result:1,targetId:4,targetLv:2} -> result 3 OUVRE
//     l'échange, et le serveur l'envoie AUX DEUX joueurs (trade.cpp, trade_tradeack).
//   ZC_ADD_EXCHANGE_ITEM   0x0b42 (60 o) -> ce que le PARTENAIRE dépose ; itemId 0
//     signifie du ZENY, le montant voyageant dans `amount`.
//   ZC_ACK_ADD_EXCHANGE_ITEM 0x00ea {index:2,result:1} -> MES dépôts : l'acquittement
//     ne porte que l'index, l'objet se retrouve dans l'inventaire (que le serveur ne
//     vide qu'au commit) et la quantité n'est connue que de nous.
//   ZC_CONCLUDE_EXCHANGE_ITEM 0x00ec {who:1} -> verrous ; who 0 = moi, 1 = l'autre.
//   ZC_CANCEL 0x00ee / ZC_EXEC 0x00f0 {result:1} -> fin d'échange.
// ⚠ L'opcode et la disposition de 0x0b42 dépendent du PACKETVER : le serveur est en
// 20250716, pour laquelle PACKETVER_RE n'est PAS défini (la fenêtre RE s'arrête à
// 20211118) -> branche MAIN >= 20200916, refine et grade rejetés en FIN de structure.
//
// ACTIONS : paquets CZ construits ici, plus aucun cmd de CMode::SendMsg.
//   CZ_ACK_EXCHANGE_ITEM     0x00e6 {type:1}            3 = accepter, 4 = refuser
//   CZ_ADD_EXCHANGE_ITEM     0x00e8 {index:2,amount:4}  index 0 = zeny (absolu)
//   CZ_CONCLUDE_EXCHANGE_ITEM 0x00eb (opcode seul)      verrouiller
//   CZ_CANCEL_EXCHANGE_ITEM  0x00ed (opcode seul)       annuler
//   CZ_EXEC_EXCHANGE_ITEM    0x00ef (opcode seul)       valider
// Les cinq commandes natives correspondantes (0x32-0x36) n'étaient QUE des
// constructeurs : CMode::SendMsg case 52 @0x00C8CE33 tient dans « mov eax, 0EBh ;
// SendPacket », idem 53 et 54 — aucun accès fenêtre, aucun état. Rien n'obligeait
// donc à garder une native vivante pour agir.
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
    uint8_t  damaged = 0;     // équipement cassé (rendu rouge)
    uint8_t  identified = 1;
    uint8_t  grade = 0;
    uint8_t  type = 0;        // item_types du serveur : décide de l'empilabilité
    uint32_t cards[4] = {0, 0, 0, 0};  // partenaire : reçues telles quelles (0x0B42)
    uint16_t look = 0;        // viewSprite (aperçu de description)
    uint32_t location = 0;    // masque d'équipement (aperçu de description)
    int      index = 0;       // MON côté : index inventaire (0 pour le partenaire)
    // Options aléatoires d'INSTANCE. Le paquet les porte (5 entrées de 5 octets) :
    // sans elles on ne saurait pas distinguer deux exemplaires du même équipement,
    // et c'est précisément ce qu'on veut vérifier avant de valider un échange.
    itemdesc::SimpleOpt opts[5];
    int      opt_count = 0;
    // Nom DÉCORÉ (refine + préfixes/suffixes de cartes), composé par le
    // name-builder natif sur un ItemSkillInfo reconstruit — cf. BuildTradeInfo.
    char     name[96] = {0};
  };

  void OnTick() override;
  void OnRenderUI() override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

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
  // Actions : paquets CZ construits ici (plus aucun cmd de CMode::SendMsg).
  void TradeAck(int type);              // CZ 0x00e6 : 3 = accepter, 4 = refuser
  void SendAdd(int index, int32_t amount);  // CZ 0x00e8 (index 0 = zeny)
  void SetZeny(int amount);             // CZ 0x00e8 index 0 — montant absolu
  void Lock();                          // CZ 0x00eb (verrou)
  void Cancel();                        // CZ 0x00ed (annuler)
  void Commit();                        // CZ 0x00ef (valider l'échange)

  // Quantité EN VOL pour cet index (envoyée, pas encore acquittée). Ce qui est
  // acquitté a déjà quitté le modèle de session — le compter à nouveau doublerait.
  int PendingAmount(int invIndex) const;
  // Nombre d'objets DISTINCTS déjà posés : l'échange en accepte 10, et c'est au
  // CLIENT de le dire (le serveur sort sans un mot au 11e).
  int OfferedSlotCount() const;
  // Complète mes ajouts acquittés (0x00EA) depuis le modèle de session de
  // l'inventaire. Thread PRINCIPAL uniquement.
  void ResolveMyAdds();
  // Compose les noms décorés (refine + affixes de cartes) encore vides, des deux
  // côtés. Thread PRINCIPAL : le name-builder est natif.
  void ResolveNames();
  // Remet la session d'échange à zéro (les deux offres, verrous, zeny, attentes).
  void ResetSession();
  // Une fenêtre native d'échange traîne-t-elle ? Sert à reconnaître un échange
  // NATIF en cours au moment où l'on allume l'interface moderne — la seule façon de
  // le savoir, puisque aucun paquet de cette session n'est passé par nous.
  bool AnyNativeTradeWindow() const;
  // Détruit les fenêtres natives résiduelles. On les DÉTRUIT plutôt que de les
  // masquer : masquée, une native garde le clavier, et son bouton par défaut engage
  // des objets.
  void PurgeNativeTradeWindows();
  // Ferme proprement : envoie CZ_CANCEL pour défaire le deal côté serveur, puis
  // réinitialise. Appelé au clic X et au bouton Annuler.
  void CloseTrade();

  bool     open_ = false;        // fenêtre d'échange principale active ?
  bool     was_open_ = false;
  bool     req_open_ = false;    // popup de requête active ?
  bool     need_pos_ = true;
  bool     show_panel_ = true;   // détection du clic sur le X (fenêtre principale)
  int      spawn_x_ = 140, spawn_y_ = 120;

  // Détection du basculement de l'interrupteur. Basculer ANNULE l'échange, dans les
  // deux sens : la fenêtre native qui reprenait la main autrefois ne naît plus, et
  // on ne reprend jamais une session qu'on n'a pas vue naître.
  bool     prev_imgui_enabled_ = false;

  // Mes ajouts : envoyés, puis acquittés. L'acquittement ne porte que l'index, la
  // quantité n'est connue que de nous — d'où la file d'attente.
  struct PendingAdd { int index = 0; int amount = 0; };
  std::vector<PendingAdd> pending_adds_;
  std::vector<int>        acked_adds_;   // index acquittés, à résoudre au tick
  int                     pending_zeny_ = -1;  // montant envoyé, en attente d'ack

  // ── Passage du fil RÉSEAU au fil PRINCIPAL ──────────────────────────────────
  // 🔴 OnRecvPacket ne fait que COPIER les octets ; le décodage entier est rejoué
  // en phase d'entrée par HandlePacket, sur le fil principal. Écrire
  // `partner_items_` depuis le fil réseau pendant que le rendu le parcourt, c'est
  // un pointeur mort dès que le push_back réalloue. Cf. features/net_inbox.h.
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Description au clic droit : ARMÉE ici, jouée par itemcell::FlushDeferredDesc au
  // RELÂCHEMENT du bouton (cf. features/item_cell.h). L'ItemSkillInfo reconstruit
  // doit survivre jusque-là — d'où ce tampon MEMBRE, et pas une variable de pile.
  // Taille = kInfoSize (0x100), vérifié par static_assert à l'usage.
  uint8_t   desc_info_[0x100] = {0};

  // Requête entrante (popup) : nom + niveau du demandeur (ZC_REQ 0x01f4).
  char     req_name_[25] = {0};
  int      req_level_ = 0;
  uint32_t req_aid_ = 0;

  // État de la session, reconstruit depuis les paquets. 🔴 Fil PRINCIPAL EXCLUSIVEMENT
  // (écrit par DrainNet / ResolveMyAdds, lu au rendu) : rien de tout ceci n'est touché
  // depuis OnRecvPacket, c'est la raison d'être de la file ci-dessus.
  std::vector<TradeItem> my_items_;
  std::vector<TradeItem> partner_items_;
  bool     my_locked_ = false;       // mon offre verrouillée (ZC_CONCLUDE who = 0)
  bool     partner_locked_ = false;  // celle du partenaire      (who = 1)
  int64_t  my_zeny_ = 0;             // confirmé par l'acquittement d'index 0
  int64_t  partner_zeny_ = 0;        // ZC_ADD 0x0b42 avec itemId 0

  int      zeny_input_ = 0;          // champ de saisie zeny (ImGui) — appliqué au verrou
  bool     screenshot_ = false;      // case « Screenshot Trade » (cmd 0x44 au commit)
  bool     committed_ = false;       // j'ai validé (cmd 0x36) -> on attend l'autre joueur
  int      last_result_ = -1;        // ZC_EXEC 0x00f0 (0 = succès, 1 = échec)
  int      add_error_ = -1;          // ZC_ACK_ADD 0x00ea (dernier code d'erreur)
};
