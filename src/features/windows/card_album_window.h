#pragma once

#include <cstdint>
#include <vector>

#include "features/plugin.h"
#include "features/systems/bourgeon_opcodes.h"
#include "ui/viewer_rect.h"  // ro::ViewerRect : le rect écran, pour le glisser depuis l'inventaire

struct ImDrawList;
struct ImVec2;

// ── L'ALBUM DE CARTES ───────────────────────────────────────────────────────
//
// Un conteneur de cartes qui vit HORS du système de storage. Un entrepôt
// plafonne à 600 slots et ce plafond n'est pas négociable : `struct s_storage`
// voyage entre map-server et char-server par memcpy brut dans un paquet dont la
// longueur tient sur 16 bits, ce qui borne l'architecture entière à 850 slots.
// L'album ne cherche pas à repousser ce plafond, il en sort — une carte n'ayant
// aucun état d'instance, le couple (nameid, amount) la décrit entièrement et le
// conteneur devient une table SQL que le map-server écrit lui-même.
//
// ── LA RÈGLE ────────────────────────────────────────────────────────────────
//
// La PREMIÈRE copie d'une carte est SACRIFIÉE : elle est consommée et débloque
// définitivement l'emplacement de cette carte. Les copies suivantes s'y empilent
// et restent retirables. Une carte jamais sacrifiée ne peut pas être rangée.
//
// 🔴 Un emplacement débloqué dont la réserve est vide est un état NORMAL, et
// c'est pour cela que `Row` porte `unlocked` À CÔTÉ de `amount` : `amount == 0`
// ne dit pas si l'emplacement a été payé. Confondre les deux ferait réclamer un
// second sacrifice pour une carte déjà acquise.
//
// ── LA FORME : UN CLASSEUR ──────────────────────────────────────────────────
//
// La fenêtre est un classeur ouvert : deux pages côte à côte, chacune une grille
// de POCHETTES, une par carte, avec l'illustration `cardBmp` de la carte dedans.
// Une pochette débloquée montre l'illustration en couleur et sa réserve en
// pastille ; une pochette scellée montre une silhouette grise et son cachet.
// Des intercalaires (onglets) classent les pages par emplacement d'équipement
// et une barre suit la complétion.
//
// Les cartes ENTRENT depuis la visionneuse d'inventaire : glissée sur le
// classeur, une carte est déposée dans sa pochette si elle est ouverte, ou
// proposée au sacrifice sinon — et le menu de l'inventaire offre « Vers
// l'album ». Elles en SORTENT en glissant une pochette sur l'inventaire. C'est
// le même routage que l'entrepôt et le chariot (viewer_probes) : la fenêtre
// SOURCE décide au relâché, d'après la fenêtre sous la souris. Il n'y a donc
// pas de « plateau » de cartes en main ici : l'inventaire EST ce plateau.
//
// Ce que ça coûte, et qui a dicté ui/card_thumb.h : neuf cents illustrations de
// 300×400 ne se chargent pas telles quelles. Les vignettes sont réduites,
// bornées, et chargées quelques-unes par frame.
//
// ── CE QUE CETTE FENÊTRE N'EST PAS ──────────────────────────────────────────
//
// 🔴 Elle ne lit RIEN du modèle de session natif. Les trois viewers d'objets
// (inventaire, chariot, entrepôt) extraient leur liste de `g_session+0x16f0` et
// suivants, parce que c'est le client natif qui parse ZC_STORE_ITEMLIST. L'album
// n'existe pas dans le client natif : sa liste vient UNIQUEMENT du ZC 0x0F33.
// Donc pas d'adresse de globals.h ici, pas de kMaxItems, et aucune limite de 700.
//
// L'inventaire, lui, est bien lu du modèle natif — mais seulement pour proposer
// au joueur ce qu'il PEUT sacrifier ou déposer. Le serveur revalide tout.
//
// ── AUTORITÉ ────────────────────────────────────────────────────────────────
//
// Le serveur décide de tout et ne diffuse rien spontanément : on lui demande, il
// répond par l'état COMPLET. Cette fenêtre n'applique donc jamais une opération
// « en avance » sur sa propre liste — un sacrifice est irréversible, et afficher
// un résultat que le serveur n'a pas confirmé serait mentir sur un paiement.
class CardAlbumWindow : public Plugin {
 public:
  CardAlbumWindow();

  const char* name() const override { return "CardAlbumWindow"; }

  void OnRenderUI() override;
  void OnTick() override;
  // 🔴 FIL RÉSEAU : ne fait que copier dans la boîte aux lettres. Le décodage
  // de ZC_BOURGEON_CARD_ALBUM (0x0F33) est dans HandlePacket, sur le fil
  // principal — rows_ et order_ sont lus par le rendu, les remplir depuis le fil
  // réseau était une course avec DrawPage.
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  // Purge au changement de session : un album est lié à un COMPTE, mais la
  // liste en mémoire vaut pour la session de zone qui l'a reçue.
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  bool IsOpen() const { return open_; }
  void Open();
  void Close();
  void Toggle();

  // La souris (coordonnées ÉCRAN) est-elle sur la fenêtre ? Même contrat que
  // `PointOverViewer` des trois viewers : sert à viewer_probes pour router un
  // glisser relâché.
  bool PointOverWindow(int mx, int my) const {
    return open_ && imgui_enabled_ &&
           win_rect_.Contains(static_cast<float>(mx), static_cast<float>(my));
  }

  // L'inventaire offre l'objet à cet INDEX CLIENT : glisser relâché sur le
  // classeur, ou « Vers l'album » de son menu. Dépôt si sa pochette est
  // ouverte (une pile demande combien), confirmation du sacrifice sinon, refus
  // affiché si ce n'est pas une carte de monstre. Le serveur revalide tout.
  void OfferFromInventory(int client_index);

  // Miroir du bit UiCaps::kCardAlbum : éteint, le serveur refuse toute commande
  // d'album. C'est le sens de « réservé à l'interface moderne ».
  bool& imgui_enabled() { return imgui_enabled_; }

  // Court-circuite la confirmation : la première copie d'une carte est
  // sacrifiée dès qu'on l'offre à l'album (glisser, menu). OFF par défaut —
  // c'est un réglage qui AGIT, et sur un geste irréversible.
  bool& auto_sacrifice() { return auto_sacrifice_; }

  // L'icône d'inventaire de la carte devant son nom, sous la pochette. OFF par
  // défaut : la pochette montre déjà l'illustration, et l'icône prend sur la
  // largeur du nom.
  bool& name_icon() { return name_icon_; }

  // Les macarons « O » / « M » au coin d'une pochette : la carte se trouve dans
  // l'Old Card Album (616) ou le Mystical Card Album (12246). ON par défaut —
  // c'est une information PURE (aucune commande ne part d'un macaron, il ouvre
  // une description) et c'est la réponse à « comment j'obtiens celle-là ? », la
  // question que pose une pochette scellée.
  bool& sources() { return sources_; }

  // Le liseré de la pochette dit la NATURE du monstre qui lâche la carte :
  // bleu mini-boss, orange MVP, noir les autres. ON par défaut, comme les
  // macarons, et pour la même raison : ça ne coûte rien et ça répond à une
  // question qu'on se pose devant le classeur.
  bool& rim_boss() { return rim_boss_; }

  // Section du panneau Moonlight. Rend true si un réglage a changé.
  bool DrawSettings();

 private:
  // Une carte du CATALOGUE, telle que le serveur nous la décrit. Le catalogue
  // porte toutes les cartes du jeu, pas seulement celles qu'on possède : c'est
  // ce qui permet la vue de complétion sans que le client ait à deviner la liste.
  struct Row {
    uint32_t id;       // id SERVEUR (le serveur le renvoie tel quel en retrait)
    uint16_t amount;   // copies en réserve, retirables
    bool     unlocked; // l'emplacement a été payé d'un sacrifice
    uint32_t equip;    // masque de l'emplacement CIBLE (le client n'a pas d'item_db)
    // Nature du monstre le plus coriace qui lâche cette carte, miroir de
    // e_mob_bosstype : 0 ordinaire, 1 mini-boss, 2 MVP. Le client n'a pas plus
    // de mob_db que d'item_db ; c'est le catalogue qui l'apporte. Vaut 0 quand
    // le serveur est d'avant ce champ.
    uint8_t  boss;
  };

  // Une carte trouvée dans l'inventaire, candidate au sacrifice ou au dépôt.
  struct InvCard {
    uint32_t id;
    int      index;    // index CLIENT — c'est l'argument attendu par le serveur
    int      amount;
    char     name[96];
  };

  // Une carte de l'inventaire offerte à l'album. Une COPIE, pas un pointeur
  // dans `inv_cards_`, rebalayé toutes les 400 ms.
  struct Offer {
    uint32_t id;
    int      index;
    int      amount;
  };

  // La géométrie d'une double page, recalculée à chaque frame depuis la place
  // disponible. La POCHETTE a une taille fixe (l'illustration y est dessinée
  // 1:1) ; ce sont les colonnes et les rangées qui suivent la fenêtre, et la
  // fenêtre se redimensionne par pochette entière.
  struct BookLayout {
    float page_w = 0, page_h = 0;
    float art_w = 0, art_h = 0;   // l'illustration dans sa pochette
    float cell_w = 0, cell_h = 0; // pochette + nom
    float name_h = 0;
    int   cols = 2;
    int   rows = 1;
    int   per_page = 2;
    int   per_spread = 4;
  };

  // `card_id` n'est pas toujours déductible de `arg` : pour un sacrifice ou un
  // dépôt, `arg` est un INDEX D'INVENTAIRE. On le retient donc à part, faute de
  // quoi le compte rendu ne saurait pas de quelle carte il parle.
  void Send(uint8_t cmd, uint32_t arg, uint16_t amount, uint32_t card_id);
  // Le paquet CZ 0x0F34 sur le fil, sans rien retenir : la forme unique du
  // format, que Send() (commandes avec réponse) et Close() (le verrou rendu,
  // sans réponse) partagent.
  void SendRaw(uint8_t cmd, uint32_t arg, uint16_t amount);
  void RequestRefresh();

  void ScanInventory();
  const Row* Find(uint32_t id) const;
  const InvCard* FindInHand(uint32_t id) const;

  // La carte passe-t-elle la recherche ? Le champ accepte le NOM ou l'ID de
  // l'item : un filtre vide laisse tout passer.
  bool MatchesFilter(uint32_t card_id) const;

  // Reconstruit `order_` : filtrage puis tri. Appelée seulement quand quelque
  // chose a changé — pas à chaque frame, le catalogue faisant ~900 lignes.
  void RebuildOrder();

  void DrawHeader();
  void DrawTabs();
  void DrawCompletion();
  void DrawBook(float height);
  void DrawPage(ImDrawList* dl, const ImVec2& p0, const BookLayout& lay,
                int first, bool left, float alpha);
  void DrawPocket(ImDrawList* dl, const ImVec2& pos, const BookLayout& lay,
                  int order_index, float alpha);
  void PocketMenu(const Row& r, const char* nm);
  void DrawConfirmModal();

  // ── Édition STAFF des deux albums d'objets (IsStaff, gestes au macaron) ───
  // Maintenir Ctrl fait apparaître « +O » / « +M » sur les albums où la carte
  // N'EST PAS ; maintenir Maj change les macarons présents en « −O » / « −M ».
  // Le clic pose un BON DE TRAVAIL (features/windows/card_album_delta.h) : rien
  // ne change dans le jeu, c'est un script qui portera l'intention au YAML
  // serveur puis régénérera le fichier de tirage du client. Un macaron en
  // attente se dessine donc AUTREMENT qu'un macaron acquis — confondre les deux
  // ferait promettre au joueur une carte que l'album ne donne pas encore.
  void DrawRateModal();
  // Écrit le bon de travail et prépare le message qui le dira. `remove` vrai
  // ignore `rate`.
  void PostIntent(uint32_t album, uint32_t card, int rate, bool remove);
  // Annule l'intention posée sur ce couple.
  void ClearIntent(uint32_t album, uint32_t card);
  // Une carte de l'inventaire est offerte à l'album : dépôt si sa pochette est
  // ouverte, sinon le sacrifice.
  void OfferCard(const Offer& c);
  // LE point d'entrée du sacrifice : la confirmation, ou l'envoi direct si le
  // joueur a choisi de s'en passer. Aucun autre chemin n'émet kCmdUnlock.
  // `rest` = les copies de la pile à ranger dans la pochette une fois ouverte.
  void RequestSacrifice(int client_index, uint32_t card_id, int rest);
  // Le glisser d'une pochette vers l'inventaire, relâché : retrait. Même
  // mécanique que les viewers — au relâché, ImGui a déjà oublié le payload, et
  // c'est la dernière position connue qui désigne la cible.
  void RouteDragRelease();
  // Tourne les pages de `delta` doubles pages. Borné.
  void Flip(int delta);
  // Arme une action et, si la pile fait plus d'un exemplaire, le dialogue
  // « combien ? ». Une pile de 1 part directement : demander une quantité pour
  // un seul objet est un clic pour rien.
  void ArmMove(uint8_t cmd, uint32_t arg, uint32_t card_id, int max_amount,
               bool ask_quantity);
  void PumpQuantityPrompt();

  static const char* ResultText(uint8_t result);

  bool open_ = false;
  // 🔴 Écrit par le groupe « Interface moderne » (`kModernGroup`, moonlight_ui.cc),
  // jamais par une case à lui. False comme tout le groupe : l'album n'existe pas
  // en interface native — il reçoit ses cartes de l'inventaire MODERNE.
  bool imgui_enabled_ = false;
  bool auto_sacrifice_ = false;
  bool name_icon_ = false;
  bool sources_ = true;
  // Le liseré de nature (bleu mini-boss, orange MVP). Séparé des macarons : l'un
  // dit où ACHETER sa chance, l'autre ce qu'il faut aller TUER.
  bool rim_boss_ = true;
  // Le serveur a refusé de nous donner l'album : un autre compte de jeu du même
  // compte Moonlight le tient. Les pages restent vides et le disent, plutôt que
  // de montrer une réserve qui n'est pas la nôtre à manipuler.
  bool blocked_ = false;

  // Une seule demande en vol : la fenêtre s'ouvre, demande, et attend. Sans ce
  // drapeau un OnRenderUI par frame réclamerait 10 Ko de catalogue soixante fois
  // par seconde.
  bool asked_ = false;

  std::vector<Row> rows_;      // trié par id, comme le serveur l'envoie
  int unlocked_count_ = 0;     // recalculé à chaque réception
  int64_t total_reserve_ = 0;

  // Indices dans `rows_`, filtrés et triés — ce que les pages montrent.
  std::vector<int> order_;
  bool order_dirty_ = true;
  int  sort_mode_ = 0;     // 0 = catalogue (id), 1 = nom, 2 = réserve, 3 = emplacement

  // Complétion de l'intercalaire actif, hors recherche et hors « débloquées
  // seulement » : c'est la collection qu'on mesure, pas la vue.
  int cat_total_ = 0;
  int cat_unlocked_ = 0;

  // ── Pagination ─────────────────────────────────────────────────────────
  // `first_` = indice dans `order_` de la première pochette de la page de
  // gauche. Un INDICE et non un numéro de page : le nombre de pochettes par
  // page dépend de la taille de la fenêtre, et garder l'indice fait qu'un
  // redimensionnement laisse le joueur devant les mêmes cartes.
  int      first_ = 0;
  int      per_spread_ = 6;     // celui de la dernière frame, pour Flip()
  uint32_t flip_tick_ = 0;      // départ du fondu de la page tournée

  // Les cartes de l'inventaire, pour ce que le menu d'une pochette peut offrir
  // (« Depuis l'inventaire », « Sacrifier une copie ») et pour résoudre un
  // index que l'inventaire nous tend.
  std::vector<InvCard> inv_cards_;
  uint32_t inv_scan_tick_ = 0;

  // Le rect de la fenêtre, capturé au rendu, invalidé quand elle ne se dessine
  // pas : c'est ce que `PointOverWindow` interroge.
  ro::ViewerRect win_rect_;
  float win_w_ = 0.0f;  // sa taille, lue en tête de frame pour le snap
  float win_h_ = 0.0f;

  // Le glisser d'une pochette (payload ALBUM_CARD) : ce qu'on emporte, et la
  // dernière position de la souris — au relâché, ImGui a déjà tout oublié.
  bool     drag_active_ = false;
  uint32_t drag_id_ = 0;
  int      drag_amount_ = 0;
  float    drag_mx_ = 0.0f;
  float    drag_my_ = 0.0f;

  // Dernier compte rendu du serveur, affiché quelques secondes. Le SUCCÈS est
  // affiché lui aussi : un sacrifice consomme définitivement une carte, et n'en
  // dire que « 1 card est supprimée » (le message natif) laisse le joueur sans
  // la seule information qui compte — que l'emplacement est désormais ouvert.
  uint8_t  last_result_ = 0;
  uint32_t result_tick_ = 0;
  uint8_t  last_cmd_ = 0;        // ce qu'on a demandé, pour le formuler
  uint32_t last_card_id_ = 0;    // sur quelle carte
  uint16_t last_amount_ = 0;
  bool     last_chained_ = false;  // ce dépôt suivait un sacrifice : le dire ensemble

  // Une commande est partie et attend sa réponse. Un état reçu SANS commande en
  // vol vient d'ailleurs (@storealbum) et se formule d'après ce qui a changé,
  // pas d'après la dernière carte rangée à la main.
  bool     cmd_in_flight_ = false;
  int64_t  unsolicited_reserve_ = 0;
  int      unsolicited_unlocked_ = 0;

  // Le reste d'une pile offerte à une pochette scellée : sacrifiée pour une,
  // rangée pour les autres dès que le serveur confirme l'ouverture.
  int      chain_index_ = -1;
  uint32_t chain_id_ = 0;
  int      chain_amount_ = 0;
  bool     chained_put_ = false;  // le dépôt de suite est parti

  // La carte que le dernier ordre a touchée : sa pochette est surlignée un
  // moment et on tourne les pages jusqu'à elle, une fois. Sans cela le résultat
  // se perd dans neuf cents pochettes.
  uint32_t highlight_id_ = 0;
  uint32_t highlight_tick_ = 0;
  bool     scroll_to_highlight_ = false;

  // Sacrifice en attente de confirmation. -1 = aucun. Le modal est déclaré UNE
  // fois, hors de la boucle de pochettes : ouvert depuis l'intérieur d'une
  // boucle, un popup se repositionne au curseur de l'élément courant et saute
  // d'une frame à l'autre.
  int      confirm_index_ = -1;
  uint32_t confirm_id_ = 0;
  bool     open_confirm_ = false;

  // ── Action en attente d'une QUANTITÉ ──────────────────────────────────────
  // Même mécanique que les trois viewers d'objets : le menu contextuel ARME
  // l'action, et le dialogue partagé (ui/qty_prompt) est pompé une fois par
  // frame dans la fenêtre. Un ImGui::OpenPopup lancé depuis un menu contextuel
  // ne franchit pas la bonne pile d'ID — c'est précisément le piège que
  // ro::OpenQuantityPrompt évite.
  bool     pend_active_ = false;
  uint8_t  pend_cmd_ = 0;      // kCmdGet ou kCmdPut
  uint32_t pend_arg_ = 0;      // id de carte (GET) ou index inventaire (PUT)
  uint32_t pend_card_id_ = 0;
  int      pend_max_ = 0;
  bool     pend_open_prompt_ = false;

  // ── Bon de travail staff en cours de saisie ──────────────────────────────
  // Le poids de tirage (`Rate` du groupe serveur) se demande à l'ajout : une
  // carte commune et une carte de MVP ne se posent pas au même tarif. La modale
  // est déclarée au niveau de la FENÊTRE, jamais dans la boucle de pochettes —
  // ouverte de l'intérieur, un popup se repositionne au curseur de l'élément
  // courant et saute d'une frame à l'autre.
  bool     rate_open_ = false;   // demande d'ouverture, consommée par DrawRateModal
  uint32_t rate_album_ = 0;
  uint32_t rate_card_ = 0;
  int      rate_value_ = 1;
  // Ce que le dernier geste staff a donné. À part du compte rendu serveur :
  // ici, ce n'est pas le serveur qui a répondu, c'est un fichier qui a été
  // écrit — ou qui ne l'a pas été.
  char     staff_msg_[160] = {};
  uint32_t staff_msg_tick_ = 0;
  bool     staff_msg_ok_ = true;

  int  slot_filter_ = 0;  // 0 = tous ; sinon index dans kSlotFilterMasks
  int  show_filter_ = 0;  // 0 toutes, 1 débloquées, 2 scellées, 3-5 par provenance
  char filter_[64] = {};
};
