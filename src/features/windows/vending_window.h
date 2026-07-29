#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "features/windows/item_desc_window.h"  // itemdesc::SimpleOpt / RenderSimpleDesc
#include "features/plugin.h"

// ── VendingWindow ────────────────────────────────────────────────────────────
//
// Remplacement ImGui de la fenêtre de COMPOSITION d'échoppe joueur, dans ses
// DEUX modes — vente (« Opening a stall ») et achat (« Buying Store Window »).
// Les deux sortent de la MÊME classe native `UIMerchantShopMakeWnd` ; seul le
// champ +0x130 change (0 = vente, 1 = achat), d'où un seul plugin pour les deux.
// RE complète : docs/vending_window_re.md, mémoire project_vending_window_re.
//
//   Fenêtre                              vente   achat
//   UIMerchantShopMakeWnd (composition)  0x29    0xAE   <- REMPLACÉE
//   UIMerchantMirrorItemWnd (grille)     0x2A    0xAF   <- REMPLACÉE aussi
//   UIMerchantItemMyShopWnd (« My Shop ») 0x2D   0xB0   <- REMPLACÉE aussi
//   UIMerchantItemLogWnd (historique)     0x101  0x102  <- REMPLACÉE aussi
//
// Les DEUX natives sont cachées et fusionnées dans UNE fenêtre ImGui : le client
// en ouvrait deux détachées (la grille des objets disponibles d'un côté, le
// formulaire de l'autre), ce qui n'a plus lieu d'être quand on redessine tout.
//
// ── Pourquoi on pilote le natif au lieu de refaire le protocole ───────────────
//
// L'ouverture ne se fait PAS en envoyant un paquet : elle passe par le bus
// `CMode::SendMsg` (cmd 82 en vente / 271 en achat), et c'est LUI qui pose l'état
// client. Le chemin du bouton OK natif enchaîne en plus une dizaine de contrôles
// (prix non numérique, prix nul, plafond 1e9, quantité > 9999, mot interdit dans
// le nom, dépassement du plafond de zeny, confirmations « safe check »...), tous
// avec leurs chaînes localisées. Les réimplémenter, c'est se condamner à les
// désynchroniser au premier changement de client.
//
// Donc : notre fenêtre ImGui ÉCRIT dans les contrôles natifs (nom, prix, qté) puis
// déclenche `OnMsg(msg 6, cmd 184)` — le clic sur OK natif. Toute la validation,
// la taxe et l'envoi restent au client. Idem pour Annuler (cmd 185) et la case
// « safe check » (cmd 213). Aucun paquet n'est fabriqué par ce plugin.
//
// ⚠ Le mode « échoppe posée sur une case choisie » (+0x188, cf. §6bis de la doc)
// est géré TOUT SEUL par ce chemin : si le serveur l'a activé (paquets ZC 0x0A7E /
// 0x0A93), le natif masque ses fenêtres et attend un clic au sol — on n'a rien à
// faire de spécial, justement parce qu'on passe par son OnMsg.
//
// ── Poser / retirer un objet SANS glisser-déposer ────────────────────────────
//
// Cacher la grille native supprime le seul moyen natif de composer l'échoppe : le
// glisser. Le simuler serait pénible (le drag natif transporte un enregistrement
// d'objet COMPLET dans gameMode+0x308, pas juste un type et un index), et surtout
// inutile — le handler de dépôt natif ne fait, au bout du compte, qu'appeler l'API
// de session. On appelle donc cette API directement, comme lui.
//
// Toutes ces fonctions sont __thiscall avec `ecx` = g_session (0x015FA3C0) ;
// conventions relevées sur des sites d'appel réels, pas déduites :
//   VendingAvail_GetCount(session)                         0x00D5CE60
//   VendingAvail_GetAt(session, rec, index)                0x00D5C160
//   VendingShop_GetAt(session, rec, index)                 0x00D5BEA0
//   VendingAvail_GetAmountBySrcIndex(session, srcIdx)      0x00D5C200
//   VendingShop_GetPlacedAmountBySrcIndex(session, srcIdx) 0x00D5BF40
//   VendingShop_AddOrMergeItem(session, rec, noMerge, 1)   0x00D54C40
//   VendingAvail_ConsumeItem(session, rec, wholeNode, 1)   0x00D57C60
//   VendingAvail_AddOrMergeItem(session, rec, noMerge, 1)  0x00D54D80
//   VendingShop_ConsumeItem(session, rec, wholeNode)       0x00D57AC0
// `rec` = un ItemSkillInfo de 0x100 octets sur la pile ; il porte deux std::string
// (+0x2C et +0x44) qu'il FAUT détruire après usage — le natif le fait aussi.
//
// ⚠ Poser = AddOrMerge(échoppe) PUIS Consume(disponibles) ; retirer = l'inverse.
// AddOrMerge n'est PAS un « puis-je ? » : il ajoute déjà, et renvoie 0 quand il a
// fusionné avec une pile existante. Conditionner le second appel à son retour
// laisse les deux listes désynchronisées (l'objet s'empile dans l'échoppe sans
// quitter le stock, ou revient au stock sans quitter l'échoppe) et fait exploser
// les quantités. En vente les deux appels s'enchaînent donc SANS condition, comme
// dans le handler de dépôt natif.
//
// Les listes, elles, sont lues en POD dans les fenêtres natives cachées (jamais par
// appel natif à chaque frame) : liste des disponibles à mirror+0xE8, liste des
// objets posés à wnd+0x148. Après chaque mutation on renvoie msg 23 aux deux
// fenêtres pour que leurs listes — donc nos lectures — restent à jour.
//
// ── Import ───────────────────────────────────────────────────────────────────
// Le bouton « Import » (cmd 560) recharge le dernier shop du personnage. On
// clique simplement le bouton natif : il charge le snapshot, repose les objets
// dans la session et remplit ses propres edits. Nos prix, eux, sont lus à la
// MÊME source que lui — le vecteur snapshot `g_VendingSnapshot_begin..end`
// (pas de 0xF8 ; +0x10 quantité, +0x14 prix) et le nom `g_VendingSnapshot_ShopName`.
// Savoir relire le texte d'un UIEdit n'était donc jamais nécessaire.
//
// ⚠ MAIS il faut lire le vecteur AVANT de cliquer : `ImportSavedShop` s'en fait
// une copie locale, s'en sert, puis le VIDE en sortant (`sub_5E2A60` pose
// `end = begin`). Lire après le clic ne ramène que des zéros. Le nom, lui, vit
// dans une variable distincte qui survit — d'où le symptôme trompeur « les
// objets et le nom sont importés, les prix restent à 0 ».
//
// ── Côté ACHETEUR (cliquer sur l'échoppe d'un autre joueur) ──────────────────
// Deux fenêtres natives : 0x2B UIMerchantItemShopWnd (l'offre du vendeur) et
// 0x2C UIMerchantItemPurchaseWnd (le panier + Total/buy/cancel). Ids et classes
// confirmés par RTTI sur session vivante.
//
// Modèle NpcShopWindow (shop NPC) : on LIT la liste déjà résolue par le natif
// (noms, slots, prix), et on ÉMET la transaction soi-même. Le panier natif n'est
// jamais touché — pas besoin de simuler un glisser-déposer, dont la charge utile
// vit dans gameMode+0x308 et n'est pas reproductible proprement.
//
//   CZ_PC_PURCHASE_ITEMLIST_FROMMC 0x0801, relevé sur le constructeur natif :
//     +0 opcode:2 | +2 len:2 | +4 AID:4 | +8 UniqueID:4 | [amount:2, index:2]*
//   len = 12 + 4*n, refusé au-delà de 0x800 (même garde ici). `index` est
//   l'index d'ÉCHOPPE (nœud+0x0C), pas un index d'inventaire.
//
// ⚠ Après un achat, le serveur ne renvoie AUCUNE liste mise à jour : seulement
// ZC 0x0135 {index, amount, result}. Le client natif s'en sort en FERMANT
// l'échoppe dès le clic « buy » (rAthena : « this was automatically done by the
// client »). Comme on garde la fenêtre ouverte pour enchaîner, on redemande la
// liste soi-même avec **CZ_REQ_BUY_FROMMC 0x0130 {AID:4}** — TCP garantit que le
// serveur a traité l'achat avant, donc la liste reçue est déjà la bonne. Si elle
// revient vide après un achat, on ferme, comme le natif.
//
// ⚠ Chaque ligne porte DEUX prix : nœud+0x1C = base, nœud+0x20 = effectif
// (Discount appliqué). Le natif affiche « base -> effectif » quand ils diffèrent
// et facture l'EFFECTIF. Lire +0x1C afficherait un prix trop élevé et un total
// faux — c'est le champ que lisent les autres vues, où les deux coïncident.
//
// ── Limites assumées de cette v1 ─────────────────────────────────────────────
//  - La taxe n'est pas affichée (le total montré est brut) : `Vending_GetTaxPercent`
//    prend un prix 64 bits et sa convention d'appel n'est pas encore vérifiée.
//  - Le bouton « close » natif de « Ma boutique » (cmd 201) ne se contente PAS de
//    fermer la fenêtre : il dispatche CMode::SendMsg 81 (vente) / 270 (achat),
//    c'est-à-dire qu'il MET FIN à la boutique. Notre bouton porte donc un libellé
//    explicite et une confirmation — que le clic vienne du bouton ou de la croix.
//  - Côté acheteur, seul le mode « j'achète chez un vendeur » est repris. Le mode
//    « je vends à une échoppe d'achat » passe par CZ 0x0819 (entrées de 8 octets,
//    itemId lu par atoi sur la chaîne du nœud) : pas encore décodé, donc les
//    fenêtres natives sont RENDUES au joueur plutôt que remplacées à moitié.

class VendingWindow : public Plugin {
 public:
  const char* name() const override { return "VendingWindow"; }

  void OnTick() override;
  void OnRenderUI() override;

  // Cache la fenêtre de composition dès sa création (hook MakeWindow de
  // WindowPosTweaks, ids 0x29 et 0xAE) -> pas de frame native visible.
  void HideNativeAtCreation(void* win);

  // Une échoppe est en cours de COMPOSITION (fenêtre 0x29 ou 0xAE présente).
  //
  // Côté serveur ça correspond à `sd->state.prevend`, posé par le cast de la
  // compétence Vending (skill_vending.cpp) et effacé à l'ouverture ou à
  // l'annulation. Tant qu'il est levé, le serveur REFUSE en silence :
  //   - inventaire <-> cart     (pc_putitemtocart / pc_getitemfromcart)
  //   - cart       <-> storage  (storage_storageaddfromcart / gettocart)
  // …et tout ce qui passe par pc_cant_act2(), qui inclut prevend.
  // En revanche inventaire <-> storage n'est PAS bloqué par le serveur : le
  // griser aussi est un choix CLIENT, pour ne rien laisser bouger sous une
  // composition en cours. Les tooltips distinguent les deux cas.
  //
  // Volontairement indépendant de `imgui_enabled_` et sans état : la règle vaut
  // aussi quand la composition est affichée en natif.
  bool IsComposing() const;

  // ⚠⚠ RÉ-ENTRANCE DU RENDU — à lire avant de câbler le moindre bouton.
  //
  // `UIWndMgr_ShowMessageBoxModal` (0x00A31A30) est une modale VRAIMENT
  // bloquante : elle ne rend pas la main, elle tourne dans une boucle interne
  // qui RELANCE le tick/rendu du mode courant jusqu'à ce que le joueur réponde
  //     while (!mgr[1]) { if (mode) (*(mode_vtable+16))(mode); Sleep(~20); }
  //
  // Or nos OnRenderUI s'exécutent ENTRE ImGui::NewFrame() et ImGui::Render().
  // Déclencher de là une commande native qui ouvre une modale relance donc le
  // rendu en pleine frame ImGui : NewFrame() est appelé une seconde fois sans
  // Render(), et le client se fige DANS d3d9.dll — sans crash, sans exception,
  // et sans une seule frame Bourgeon dans la pile (diagnostiqué en live).
  //
  // Reproduction : un objet à prix 0 (confirmation en vente, refus en achat).
  //
  // D'où cette file : toute commande native émise depuis l'UI est EMPILÉE, puis
  // rejouée ici — appelé depuis la phase d'input du jeu (Bourgeon::
  // OnProcessInput), donc hors de toute frame ImGui. Même motif que
  // MenuIcons::FlushPending.
  void FlushPending();

  // Setting PERSISTANT (bourgeon_settings.yaml « vending_imgui »), basculé en
  // GROUPE par SetModernInterface — PAS de case isolée dans le panneau. L'échoppe
  // se monte à partir du CHARIOT, qui est déjà du lot : un formulaire moderne
  // au-dessus d'un cart natif serait le mixe qu'on a supprimé.
  // Public pour le chargement/sauvegarde par MoonlightUi.
  bool imgui_enabled_ = false;  // OPT-IN : formulaire natif par défaut

  // Ce qu'il faut pour décrire un objet : ni la DB ni l'id ne suffisent, cartes,
  // refine et options d'instance vivent dans l'ItemSkillInfo du nœud. Mêmes
  // champs que les autres viewers, pour alimenter itemdesc::RenderSimpleDesc.
  // Public seulement pour que le remplisseur local au .cc puisse y écrire.
  struct DescInfo {
    uint32_t id = 0;
    // nœud+0x0c (= ItemSkillInfo+0x04) : l'index de l'objet DANS SA LISTE. C'est
    // lui, et non l'id, qui distingue deux exemplaires du même objet — trois
    // Knife dont une sertie, une +1 et une +3 partagent le même id, et une
    // recherche par id seul rendrait toujours la première pour les trois.
    int      index = 0;
    uint32_t cards[4] = {0, 0, 0, 0};
    int      refine = 0;
    int      opt_count = 0;
    itemdesc::SimpleOpt opts[5];
    // Nom d'AFFICHAGE composé par le client (BuildDisplayName) : « +10 Hydra
    // Sword [3] ». Le nom de la DB, lui, ignore refine, cartes et slots — il
    // ne sert que de repli si la composition échoue.
    char     name[64] = {0};
  };

 private:
  // Une ligne de l'échoppe = un objet déjà posé. Extrait en POD sous SEH pour un
  // rendu hors __try (même contrainte que les autres viewers).
  struct Row {
    uint32_t id = 0;      // nœud+0x34 : l'itemId en TEXTE ("714") -> atoi
    int      index = 0;   // nœud+0x0c : index source (cart en vente)
    int      amount = 0;  // nœud+0x18 : quantité posée
    int      slots = 0;   // nœud+0x90
    int      price = 0;   // nœud+0x1c : prix unitaire (renseigné une fois le shop
                          // ouvert ; 0 pendant la composition)
    DescInfo desc;        // survol -> aperçu, clic gauche -> description complète
  };
  static constexpr int kMaxRows = 13;  // plafond natif en vente (5 en achat)
  // Les objets DISPONIBLES ne sont pas plafonnés par les emplacements : c'est
  // tout le stock proposable (le cart, ou l'inventaire en échoppe d'achat).
  static constexpr int kMaxAvail = 128;

  // Relit les DEUX listes depuis les fenêtres natives cachées (POD, sous SEH).
  void Refresh();
  // Écrit nos prix/quantités/nom dans les contrôles natifs, puis clique OK.
  // ⚠ Ne JAMAIS appeler depuis OnRenderUI : passer par QueueSubmit (cf.
  // FlushPending — le OK natif peut ouvrir une modale bloquante).
  void SubmitToNative();
  // Déclenche un bouton natif (msg 6) : 184 = OK, 185 = Annuler, 213 = safe check.
  // ⚠ Même règle : depuis l'UI, passer par QueueCommand.
  void FireCommand(int cmd);

  // ── File des commandes natives différées ───────────────────────────────────
  // On mémorise l'IDENTIFIANT de la fenêtre, pas son pointeur : le client
  // détruit ses fenêtres, et rejouer la commande une frame plus tard sur un
  // pointeur libéré serait un usage après libération.
  struct PendingCmd {
    int win_id = 0;
    int cmd = 0;
  };
  static constexpr int kMaxPending = 8;
  void QueueCommand(int win_id, int cmd);
  void QueueSubmit() { pending_submit_ = true; }
  PendingCmd pending_[kMaxPending];
  int  pending_count_ = 0;
  bool pending_submit_ = false;
  // Renvoie msg 23 aux deux fenêtres natives : elles reconstruisent leurs listes
  // depuis la session, donc nos lectures POD suivent la mutation.
  void RefreshNativeLists();
  // Pose l'objet n° `avail_index` de la liste des disponibles (quantité `qty`).
  void PlaceItem(int avail_index, int qty);
  // Retire l'objet n° `row` de l'échoppe (et décale nos prix/quantités).
  void TakeBackItem(int row);

  // ── « Ma boutique » (UIMerchantItemMyShopWnd, id 0x2D / 0xB0) ─────────────
  // Fenêtre SÉPARÉE, avec son propre cycle de vie : elle apparaît une fois
  // l'échoppe ouverte, quand celles de composition ont déjà disparu. Elle a sa
  // propre liste de session (une troisième), et s'auto-redimensionne au nombre
  // d'objets restants.
  void RefreshMyShop();

  // ── « Item Sell History » (UIMerchantItemLogWnd, 0x101 / 0x102) ────────────
  // Ouverte par le bouton « close » de « Ma boutique ». Son propre close (cmd 201
  // aussi) est en revanche une fermeture PURE : aucune commande de jeu derrière.
  void RefreshSellLog();

  // ── Description d'objet (commune à toutes les listes) ──────────────────────
  // Survol = aperçu ; clic gauche = fenêtre de description complète du client.
  // L'aperçu est un tooltip : il doit être créé HORS de toute fenêtre ImGui, donc
  // on mémorise l'objet survolé pendant le rendu et on le dessine à la toute fin.
  void ItemHover(const DescInfo& desc, void* wnd, int list_off);
  // Icône + nom d'une cellule de liste, survol et clic gauche câblés. `wnd` et
  // `list_off` désignent la liste native où retrouver le nœud pour la
  // description complète (nullptr = aperçu au survol seulement).
  void DrawItemCell(const DescInfo& desc, int slots, void* wnd, int list_off);
  void DrawHoverDesc();
  DescInfo hover_desc_;
  bool     hover_valid_ = false;

  void* wnd_ = nullptr;     // UIMerchantShopMakeWnd natif (0x29 ou 0xAE)
  void* mirror_ = nullptr;  // UIMerchantMirrorItemWnd natif (0x2A ou 0xAF)
  bool  buying_ = false;  // mode : false = vente, true = échoppe d'achat
  int   slots_ = 0;       // +0x12c : nombre de lignes autorisées par le serveur
  int   count_ = 0;       // +0x14c : nombre d'objets posés

  bool open_ = false;       // fenêtre native présente ce frame ?
  bool was_open_ = false;   // front montant -> réinitialisation du formulaire
  bool show_panel_ = true;  // clic sur le X de notre fenêtre

  std::vector<Row> rows_;   // objets posés dans l'échoppe (wnd_+0x148)
  std::vector<Row> avail_;  // objets disponibles à poser (mirror_+0xE8)
  // Quantité à poser, par ligne de `avail_`. Indexée comme `avail_` et recalée à
  // chaque Refresh (la liste bouge à chaque pose/retrait).
  std::vector<int> avail_qty_;
  int  prices_[kMaxRows] = {0};   // prix unitaire saisi, par ligne
  int  amounts_[kMaxRows] = {0};  // quantité voulue (échoppe d'achat uniquement)
  // Import en cours : le natif recharge le snapshot (lecture disque) et repose les
  // objets, ce qui peut ne pas être visible dès la frame du clic. On réessaie donc
  // de récupérer prix/quantités pendant quelques frames, puis on abandonne — plutôt
  // que de supposer que le chargement est synchrone.
  int  import_frames_ = 0;
  // Copie du snapshot prise AVANT le clic sur Import. Indispensable : le handler
  // natif vide le vecteur en sortant (sub_5E2A60), il n'y a plus rien à relire
  // une fois les objets reposés. Appariée aux lignes par item id, pas par indice.
  uint32_t import_ids_[kMaxRows] = {0};
  int      import_prices_[kMaxRows] = {0};
  int      import_amounts_[kMaxRows] = {0};
  int      import_count_ = 0;
  char     import_name_[64] = {0};
  // Import déjà servi pour ce montage. Le natif désactive son propre bouton après
  // usage, et pour cause : le vecteur étant vidé, un second import ne reposerait
  // RIEN et laisserait le shop vide. Remis à false quand la fenêtre se referme.
  bool     import_used_ = false;
  int  zeny_limit_ = 0;           // « Purchase Zeny Limit » (achat uniquement)
  char name_[64] = {0};           // nom de la boutique (CP949)

  // ── État de « Ma boutique » ────────────────────────────────────────────────
  void* myshop_wnd_ = nullptr;
  bool  myshop_open_ = false;
  bool  myshop_panel_ = true;   // clic sur le X de NOTRE fenêtre
  int   myshop_zeny_ = 0;       // +0xF0 : encaissé (vente) OU reste à dépenser
                                //         (buying store) — cf. myshop_buying_
  bool  myshop_buying_ = false; // +0x100 != 0 : c'est une échoppe d'ACHAT
  std::vector<Row> myshop_;

  // ── État de l'historique des ventes ────────────────────────────────────────
  void* log_wnd_ = nullptr;
  bool  log_open_ = false;
  bool  log_panel_ = true;
  // Panneau déjà présenté pour CE shop. La fenêtre native, elle, vit depuis la
  // première vente : sans ce garde, le panneau se rouvrirait à chaque frame une
  // fois refermé.
  bool  log_shown_ = false;
  // Fenêtre 0x102 (échoppe d'ACHAT) plutôt que 0x101 : les lignes sont alors des
  // achats. Même classe, même layout — seul le vocabulaire change.
  bool  log_buying_ = false;
  std::vector<Row> log_;

  // ── Côté ACHETEUR : on clique sur l'échoppe d'un autre joueur ──────────────
  // Deux fenêtres natives : 0x2B = l'offre du vendeur, 0x2C = le panier.
  // Modèle NpcShopWindow : on LIT la liste résolue par le natif (noms, slots et
  // prix déjà calculés) et on ÉMET la transaction nous-mêmes (CZ 0x0801). Le
  // panier natif n'est donc jamais touché — le nôtre vit dans `basket_`.
  // ⚠ Les deux structures sont déclarées AVANT les méthodes qui les prennent en
  // paramètre : un type imbriqué doit être complet au point de la déclaration
  // (le « complete-class context » ne couvre que les CORPS de fonctions).
  struct BuyRow {
    uint32_t id = 0;
    int      index = 0;   // index d'échoppe -> c'est LUI qui part dans le paquet
    int      stock = 0;
    int      slots = 0;
    int      price = 0;      // effectif (nœud+0x20) : ce qu'on paie réellement
    int      base_price = 0; // nœud+0x1C : affiché « base -> effectif » si différent
    DescInfo desc;
  };
  struct BasketLine {
    uint32_t id = 0;
    int      index = 0;
    int      amount = 1;
    int      price = 0;
    int      max = 1;
    DescInfo desc;
  };

  void RefreshVendorShop();
  // Construit et envoie CZ_PC_PURCHASE_ITEMLIST_FROMMC depuis `basket_`.
  // Le serveur revalide tout (zeny, poids, stock) : un panier périmé échoue
  // proprement, il n'y a rien à exploiter côté client.
  void SendVendingBuy();
  // Achat d'UNE ligne sans passer par le panier (Ctrl+clic).
  void QuickBuy(const BuyRow& offer, int qty);
  // Émission commune. Enchaîne systématiquement sur RequestVendorList().
  void SendPurchase(const BasketLine* lines, int count);
  // CZ_REQ_BUY_FROMMC 0x0130 : redemande la liste de l'échoppe. Indispensable
  // après un achat — le serveur ne pousse aucune mise à jour de lui-même.
  void RequestVendorList();

  void* vendor_wnd_ = nullptr;  // 0x2B : offre du vendeur
  void* basket_wnd_ = nullptr;  // 0x2C : panier natif (source du GID/UniqueID)
  bool  vendor_open_ = false;
  bool  vendor_panel_ = true;
  // Un achat au moins a été émis dans cette session. Sert à distinguer « liste
  // vide parce qu'on a tout acheté » (-> on ferme) de « liste pas encore
  // arrivée » (-> on attend).
  bool  bought_once_ = false;
  char  vendor_name_[32] = {0};
  uint32_t vendor_gid_ = 0;  // AID du vendeur   (0x2C +0xF8)
  uint32_t vendor_uid_ = 0;  // UniqueID échoppe (0x2C +0x104)
  std::vector<BuyRow>     offers_;
  std::vector<BasketLine> basket_;
  std::vector<int>        offer_qty_;  // quantité saisie, indexée comme `offers_`

  // ── Côté VENDEUR : je vends à l'échoppe d'ACHAT d'un autre joueur ──────────
  // Trois fenêtres natives (0xB1 recherche, 0xB2 vente, 0xB3 stock proposable),
  // mêmes CLASSES que le mode achat sous d'autres identifiants.
  //
  // Modèle différent de l'achat, et c'est délibéré : ici on PILOTE le natif au
  // lieu d'émettre le paquet. L'index qui part dans CZ 0x0819 est un index
  // d'INVENTAIRE, tenu par les listes de session que seul le natif remplit ; le
  // panier affiché est donc le sien, relu à chaque frame.
  void RefreshBuyingStoreSell();
  // Ce que l'acheteur veut encore de la ligne `avail_index`, borné par le stock
  // possédé, ce qui est déjà en vente et le plafond natif de 30000.
  int  BsSellableQty(int avail_index) const;
  // Prix unitaire offert pour cet objet, 0 s'il n'est pas recherché.
  int  BsPriceOf(uint32_t item_id) const;
  void BsAddToSellList(int avail_index, int qty);
  void BsRemoveFromSellList(int sell_index);

  void* bs_wanted_ = nullptr;  // 0xB1 : « Wanted items - <acheteur> »
  void* bs_sell_   = nullptr;  // 0xB2 : « Selling Items » + Total + sell/cancel
  void* bs_mirror_ = nullptr;  // 0xB3 : « Available items: »
  bool  bs_open_  = false;
  bool  bs_panel_ = true;
  char  bs_buyer_[32] = {0};
  int   bs_zeny_left_ = 0;     // 0xB1+0x108 : ce que l'acheteur peut ENCORE payer
  std::vector<BuyRow> bs_wanted_rows_;
  std::vector<Row>    bs_avail_;
  std::vector<Row>    bs_sell_rows_;
  std::vector<int>    bs_qty_;  // quantité saisie, indexée comme `bs_avail_`
};
