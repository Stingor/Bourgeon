#pragma once

#include <climits>  // INT_MIN : sentinelle « position jamais mémorisée »
#include <cstdint>
#include <string>
#include <vector>

#include "features/plugin.h"

// ── MakeItemWindow ───────────────────────────────────────────────────────────
//
// Remplacement ImGui des fenêtres de FABRICATION. RE complète :
// docs/make_item_list_re.md (mémoire project_make_item_list_re).
//
// ── Ce n'est pas UNE fenêtre native, c'en est TROIS ──────────────────────────
// Le joueur voit un titre « LIST » et croit à une fenêtre unique. Le client en a
// trois, avec trois ids, trois layouts et trois protocoles :
//
//   94  UIMakingArrowListWnd    « LIST »               ZC 0x01AD / ZC 0x025A
//   79  UIMakeTargetListWnd     « Manufacturing List » ZC 0x018D
//   80  UIMakeTargetProcessWnd  (3 matériaux de forge) ouverte PAR la 79
//
// Ce plugin remplace les listes (94 et 79). Il ne touche PAS la 80 — voir plus
// bas, ce n'est pas un oubli.
//
// ── Pourquoi ce plugin lit les PAQUETS et pas les fenêtres natives ───────────
// Deux raisons, chacune suffisante :
//  1. le client DÉTRUIT la fenêtre dès l'envoi (SaveRectAndCloseWindow), donc
//     caler la durée de vie de l'UI sur celle de la native la ferait disparaître
//     au moment précis où le résultat arrive — c'est-à-dire l'apport principal ;
//  2. la fenêtre 94 range ses entrées dans une std::list de ItemSkillInfo dont
//     l'id est stocké en TEXTE (atoi sur info+0x2C) : repartir du paquet est plus
//     simple, plus sûr et plus rapide que de parcourir ça à chaque frame.
//
// ── Les quatre pièges du natif, tous reproduits fidèlement ───────────────────
//  1. Le protocole de réponse dépend de la fenêtre ET du mk_type :
//       94 + mk_type == 2 -> CMode::SendMsg(153, id)      = CZ_REQ_MAKINGARROW 0x01AE
//       94 + mk_type != 2 -> CMode::SendMsg(207, id, mk)  = CZ_REQ_MAKINGITEM  0x025B
//       79                -> CMode::SendMsg(130, id, m[3])= CZ_REQMAKINGITEM   0x018E
//  2. ANNULER ENVOIE UN PAQUET (id = -1, ou 0 pour le 130). Sans lui, le
//     `menuskill_id` reste ARMÉ côté serveur : tous les parseurs n'appellent
//     clif_menuskill_clear que s'ils reçoivent la réponse.
//  3. `OnMsg(0x1F)` de la fenêtre 94 reçoit un POINTEUR sur l'entrée du paquet,
//     pas l'itemId — la 79, elle, reçoit la valeur. On ne pilote aucun de ces
//     OnMsg ici, mais l'asymétrie explique pourquoi on ne factorise pas.
//  4. Une fabrication = UN lancement de compétence (clif_menuskill_clear en fin
//     de parseur), exactement comme le refine d'arme.
//
// ── Ce que le natif ne dit pas, et qu'on ajoute ──────────────────────────────
// Le natif affiche une icône et un nom, quatre lignes à la fois (columns=1,
// rows=4) dans 200x200 px. Il ne dit NI l'id, NI le stock possédé, NI la
// description — alors que tout est déjà dans la DB client. Cas réel : les quatre
// convertisseurs élémentaires 12114/12115/12116/12117 portent le MÊME
// `Name = "Elemental Converter"` -> quatre lignes rigoureusement identiques.
// D'où l'id affiché en colonne : c'est le seul discriminant qui ne dépende pas
// d'un itemInfo bien renseigné.
//
// ── Ce qui n'est PAS fait, et pourquoi ───────────────────────────────────────
//  - La RECETTE (matériaux, quantités, « combien puis-je en faire ») n'est PAS
//    affichée : elle vit dans db/pre-re/produce_db.txt et n'atteint jamais le
//    client. ZC 0x018D a bien trois champs material[3] — le serveur les remplit
//    de ZÉROS et le client les jette. La coder en dur ici contredirait la règle
//    « jamais de données codées en dur » et mentirait dès que le serveur ajuste
//    sa table. Ça demande un opcode custom.
//  - Le TAUX DE RÉUSSITE (make_per) : même raison.
//  - (RÉSOLU) La fenêtre 80 était laissée NATIVE, au motif qu'elle seule savait
//    porter les Star Crumb et les pierres élémentaires. C'était faux : la
//    décompilation de la 79 montre `SendMsg(130, itemId, ItemSkillInfo mats[3])`
//    — les matériaux voyagent en PARAMÈTRE, la 80 ne fait que les collecter. On
//    ne l'ouvre donc plus du tout, et c'est plus sûr qu'elle : ses emplacements
//    sortent réellement les objets de l'inventaire client (son Annuler les rend
//    un par un), si bien que toute sortie non prévue — warp, fermeture, plantage
//    — perd leur affichage. En envoyant nous-mêmes, rien ne bouge côté client :
//    c'est le serveur qui consomme.
//  (La MODALE « liste vide » de 0x018D n'apparaît PLUS DU TOUT, et il n'y a plus
//   rien à escamoter : elle était affichée par le handler natif du 0x018D, que le
//   remplacement d'opcode empêche de tourner. Le module `ui/native_modal` qui la
//   supprimait a donc été retiré — son mécanisme reste consigné dans
//   docs/make_item_list_re.md §3.1 bis.)
//
// ── La relance automatique, et ce qu'elle a coûté à établir ──────────────────
// Une fabrication = un lancement. Relancer suppose de savoir CE QUI a ouvert la
// liste — or ce n'est dans AUCUN paquet : un même ZC 0x01AD sert AC_MAKINGARROW,
// SA_CREATECON, GC_POISONINGWEAPON et NC_MAGICDECOY, et le client force
// `mk_type = 2` pour les quatre (vérifié en live sur deux compétences
// différentes). D'où DEUX observations, chacune sur son chemin :
//   - compétence : hook de CMode::SendMsg (0x45 / 0x71) -> NotifySkillCast ;
//   - OBJET      : hook d'envoi de CZ_USE_ITEM 0x0439   -> NotifyItemUse.
// Sans envoi de compétence récent, la liste vient forcément d'un script d'objet
// (`produce N;` du Mini Furnace, des marteaux…). Les deux relances sont opt-in et
// séparées, parce qu'elles n'ont pas le même prix : la compétence coûte du SP,
// l'OBJET est DÉTRUIT à chaque usage — `pc_useitem` fait `pc_delitem(...,
// LOG_TYPE_CONSUME)` AVANT d'exécuter le script, donc chaque ouverture de liste
// consomme un exemplaire, y compris si le joueur annule ou rate. Ce n'est pas une
// raison d'interdire NI de brider — vider une pile de 30 marteaux d'un trait est
// un choix légitime, et la chaîne est illimitée par défaut. C'en est une de le
// DIRE clairement et d'afficher le compteur en permanence ; un plafond est
// simplement OFFERT à qui veut s'en poser un. Dans les deux cas ce qui se relance
// est l'OUVERTURE de la liste, jamais la fabrication : choisir le produit et
// déclencher restent des clics.
//
// OPT-IN : membre du groupe « Interface moderne » (SetModernInterface), jamais
// basculé isolément.

class MakeItemWindow : public Plugin {
 public:
  MakeItemWindow();

  const char* name() const override { return "MakeItemWindow"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Rejoue hors frame ImGui tout ce qui touche au natif (CMode::SendMsg,
  // MakeWindow(80)). ⚠ Appelée depuis Bourgeon::OnProcessInput, JAMAIS depuis
  // OnRenderUI : ces chemins peuvent déclencher une modale BLOQUANTE, qui relance
  // le tick/rendu du mode et gèle le client si on l'atteint entre NewFrame() et
  // Render() (cf. feedback_no_native_cmd_during_imgui_frame).
  void FlushPending();

  // Masque la fenêtre native DÈS sa création (win+0x28 = 0), depuis le hook
  // MakeWindow de window_pos_tweaks. Sans ça une frame native passe : ces
  // fenêtres naissent dans un handler de paquet, donc ENTRE deux OnTick.
  void HideNativeAtCreation(void* win);

  // Contenu de la section « Fabrication » du panneau Moonlight. Renvoie true si
  // un réglage a changé (l'appelant persiste alors le fichier).
  bool DrawSettings();

  // Notifiée par le hook de CMode::SendMsg à chaque lancement de compétence.
  // ⚠ C'est la SEULE façon de savoir ce qui a ouvert la liste : la compétence
  // n'est dans AUCUN des paquets, et un même ZC 0x01AD sert quatre métiers
  // (vérifié en live : Create Arrow et Elemental Converter donnent tous deux
  // mk_type = 2). Sans elle, pas de relance automatique possible.
  // ⚠ `skill_lv` est indispensable, pas décoratif : le serveur le range dans
  // `menuskill_val`, et c'est LUI qui décide du contenu de la liste. Relancer à
  // 1 une compétence connue à 5 appauvrirait silencieusement ce qui est proposé.
  void NotifySkillCast(int skill_id, int skill_lv);

  // Notifiée par le hook d'envoi de CZ_USE_ITEM (0x0439), avec l'INDEX
  // d'inventaire — le seul champ du paquet. Pendant de NotifySkillCast pour les
  // listes ouvertes par un SCRIPT D'OBJET : c'est la seule façon de savoir QUEL
  // objet relancer.
  // ⚠ La résolution index -> identifiant se fait ICI, à l'instant de l'usage :
  // l'index devient caduc dès que le serveur a consommé l'exemplaire. Et elle
  // n'emprunte PAS Session::GetItemInfoById, dont l'offset de liste est faux sur
  // ce build (crash prouvé) — cf. le commentaire d'InvIdByIndex dans le .cc.
  void NotifyItemUse(unsigned item_index);

  // Le WndProc consulte ceci pour avaler Entrée pendant que la fenêtre est
  // ouverte (sinon la touche ouvrirait la saisie de chat par-dessus). Prédicat
  // SANS état de rendu : un drapeau posé à la frame précédente resterait vrai
  // pendant un chargement de carte et confisquerait la touche pour rien.
  bool WantsEnterKey() const;

  // Réglages persistés (moonlight_ui SettingsTable).
  bool& imgui_enabled() { return imgui_enabled_; }
  bool& show_owned()    { return show_owned_; }
  bool& show_filter()   { return show_filter_; }
  bool& desc_tooltip()  { return desc_tooltip_; }
  bool& show_history()  { return show_history_; }
  bool& log_time()      { return log_time_; }
  bool& enter_key()     { return enter_key_; }
  bool& auto_recast()   { return auto_recast_; }
  bool& auto_reuse_item() { return auto_reuse_item_; }
  int&  auto_reuse_max()  { return auto_reuse_max_; }
  int&  pos_x()         { return pos_x_; }
  int&  pos_y()         { return pos_y_; }

  bool imgui_enabled_ = true;

 private:
  // 🔴 Le décodage, sur le FIL PRINCIPAL. OnRecvPacket (fil réseau) ne fait que
  // copier : la table des recettes, le journal et le recomptage des matériaux (qui
  // parcourt le modèle d'inventaire du client) n'ont rien à faire sur ce fil-là.
  // Cf. features/net_inbox.h.
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Valeur d'`imgui_enabled_` au tick précédent : sert UNIQUEMENT à détecter la
  // BASCULE de l'interrupteur « Interface moderne », qui doit désarmer le
  // `menuskill` serveur avant de jeter la session (cf. OnTick).
  bool prev_enabled_ = true;
  // (Pas de drapeau « annulation native à faire » : FlushPending rend toute session
  // native vivante à CHAQUE frame, sans état. Un drapeau posé depuis OnTick — limité
  // à 100 ms — arrivait toujours trop tard, la fenêtre étant déjà détruite. C'était
  // le bogue « natif -> moderne casse la Mini Furnace ».)

  // ── Une relance est-elle EN VOL ? ──────────────────────────────────────────
  // Tick du dernier lancement de relance (compétence OU usage d'objet), 0 = aucun en
  // vol. Remis à zéro dès qu'une liste revient : c'est la preuve que la relance a
  // abouti. Sans ce suivi, une relance jetée par le délai de lancement arrête la
  // chaîne SANS UN MOT — cf. RetryRelaunch et le bogue constaté sur le refine.
  unsigned relaunch_sent_at_  = 0;
  int      relaunch_retries_  = 0;  // relances consécutives sans liste en retour
  // Stock de l'objet d'origine relevé JUSTE AVANT de le ré-utiliser. C'est ce qui
  // permet de distinguer « l'usage a été refusé, rien n'est perdu » (on peut
  // réessayer) de « l'exemplaire est parti sans résultat » (il faut s'arrêter).
  // -1 = non relevé.
  int      reuse_owned_before_ = -1;

  // Ré-arme une relance dont le lancement n'a rien donné. Distinct de
  // ScheduleAutoRecast, qui traite le RÉSULTAT d'une fabrication.
  void RetryRelaunch();

  // ── Le protocole de réponse, déduit du paquet qui a ouvert la liste ────────
  // C'est la SEULE chose qui distingue les trois chemins une fois la liste
  // reçue : même modèle, même UI, trois envois différents.
  enum class Proto {
    kNone,
    kArrow,    // fenêtre 94, mk_type == 2  -> SendMsg(153, id)
    kMaking,   // fenêtre 94, mk_type != 2  -> SendMsg(207, id, mk_type)
    kProduce,  // fenêtre 79                -> SendMsg(130, id, mats[3])
  };

  // Un matériau de recette, RÉSOLU. La recette client n'est que du texte
  // (« 10 Green Live », §6.1) : on la parse une seule fois, à l'arrivée de la
  // liste, pour en tirer une quantité et un id d'objet — sans quoi on ne pourrait
  // ni afficher d'icône, ni ouvrir une description, ni compter quoi que ce soit.
  struct Material {
    uint32_t id  = 0;   // 0 = nom non résolu dans la DB client
    int      qty = 0;
    // « Doit être possédé, n'est PAS consommé » — les guides de fabrication.
    // Marqueur d'origine : une quantité de 0 dans produce_db. Conséquence à ne
    // pas rater : ce matériau ne borne PAS le nombre de fabrications possibles.
    bool     not_consumed = false;
    // Ce que la fabrication PRODUIT, et non ce qu'elle exige (quantité négative
    // dans le fichier). Sert aux recettes de flèches, dont la clé est le matériau
    // et le rendement l'information utile. N'entre JAMAIS dans la faisabilité.
    bool     is_output    = false;
    char     name[64] = {0};  // le nom TEL QU'ÉCRIT dans la recette, pour le repli
  };

  static constexpr int kMaxMaterials = 8;

  struct Entry {
    uint32_t id     = 0;
    char     name[128] = {0};  // nom composé (DB client), repli sur l'id
    int      owned  = 0;       // stock en inventaire — la DB client le sait
    // Nombre de fabrications possibles avec l'inventaire courant, ou -1 quand on
    // ne peut PAS conclure (recette absente du fichier client, ou nom de matériau
    // non résolu). -1 s'affiche par un tiret : mieux vaut se taire que mentir.
    int      craftable = -1;
    Material mats[kMaxMaterials];
    int      mat_count = 0;
    bool     mats_resolved = false;  // toutes les lignes ont donné un id
  };

  struct LogLine {
    std::string text;
    uint32_t    color = 0;
  };

  // Actions différées vers le natif (rejouées par FlushPending).
  enum class Pending { kNone, kConfirm, kCancel, kRecast, kReuseItem };

  void  ScheduleAutoRecast();
  void  SendRecast();
  // Ré-utilise l'OBJET qui a ouvert la liste. Ne réutilise JAMAIS l'index capté :
  // il est périmé (l'exemplaire précédent vient d'être consommé, l'inventaire a
  // pu se compacter). On repart de l'identifiant et on laisse le natif retrouver
  // l'index courant — sinon on consommerait un objet AUTRE que celui voulu.
  void  SendReuseItem();

  void  RebuildOwnedCounts();
  // Parse la recette TEXTE du client et en résout les matériaux en (id, quantité).
  // Fait UNE fois, à l'arrivée de la liste : c'est ce qui permet ensuite l'icône,
  // la description au clic, et le comptage.
  void  ResolveMaterials(Entry& e) const;
  // Combien de fois ce produit est fabricable avec l'inventaire courant, ou -1
  // si on ne peut pas conclure. Se tait dès qu'il rend 0 — le serveur ayant déjà
  // validé qu'au moins une fabrication est possible.
  int   ComputeCraftable(const Entry& e) const;
  void  RequestMake();                 // point d'entrée UNIQUE (bouton, double-clic, Entrée)
  void  CloseAndCancel();
  void  LogResult(int result, uint32_t nameid);
  void  Log(const std::string& text, uint32_t color);
  void  DrawList();
  // Recette du produit sélectionné, lue dans la table CLIENT
  // (MetalProcessItemList.txt, cf. docs/make_item_list_re.md §6.1). Le natif ne la
  // montre qu'APRÈS validation, dans la fenêtre 80 — et jamais du tout sur le
  // chemin de la « LIST ». L'afficher dès la sélection est le gain le plus direct
  // de ce portage, et il ne coûte aucun paquet.
  void  DrawRecipe();
  // Chance de réussite du produit sélectionné — l'autre information que le natif ne
  // donne jamais. 🔴 Sur une liste ouverte par un OBJET c'est un 50 % PLAT : le
  // serveur appelle `skill_produce_mix(sd, 0, …)`, donc aucune case du switch ne
  // matche et le `default:` impose `make_per = 5000`. Ni stats ni maîtrises n'y
  // entrent. Détails et limites dans le corps de la fonction.
  void  DrawSuccessChance(const Entry& chosen);
  // Les trois emplacements de matériaux OPTIONNELS de la forge — ce que la
  // fenêtre native 80 était seule à proposer. On la remplace intégralement :
  // `CMode::SendMsg(130, itemId, ItemSkillInfo[3])` prend ces matériaux en
  // PARAMÈTRE, la 80 ne faisait que les collecter avant d'appeler la même
  // commande. Et ne pas l'ouvrir est plus sûr qu'elle : ses emplacements sortent
  // les objets de l'inventaire client, qu'une sortie non prévue perd d'affichage.
  void  DrawForgeSlots();
  void  DrawFooter();
  void  DrawHistory();
  bool  SelectionVisible() const { return sel_visible_; }
  const char* ProtoTitle() const;

  // Envois natifs, tous sous SEH et tous appelés depuis FlushPending.
  void  SendConfirm(uint32_t item_id);
  void  SendCancel();

  // ── Modèle ────────────────────────────────────────────────────────────────
  std::vector<Entry> entries_;
  // Dernière liste NON VIDE, gardée quand la suivante arrive vide. Le serveur ne
  // dit jamais pourquoi il n'a rien proposé ; cet ensemble-là, lui, était
  // réalisable il y a un instant — c'est donc le bon à examiner pour nommer ce
  // qui manque. Périmé dès qu'une liste non vide arrive.
  std::vector<Entry> stale_entries_;
  Proto    proto_      = Proto::kNone;
  uint16_t mk_type_    = 0;
  bool     empty_list_ = false;
  // 🔴 « Le serveur honore-t-il encore cette liste ? » — À NE PAS CONFONDRE avec
  // « la liste est-elle affichée ». La première rédaction les confondait : elle
  // VIDAIT `entries_` à l'envoi pour empêcher une seconde demande sur une liste
  // que le serveur venait d'oublier (`clif_menuskill_clear`). Ça marchait, mais
  // au prix d'un clignotement à chaque fabrication — la table disparaissait, la
  // fenêtre se rétractait, puis tout revenait ~500 ms plus tard, sans laisser le
  // temps de lire le résultat.
  //
  // Or interdire l'ENVOI n'oblige pas à effacer l'AFFICHAGE. Ce drapeau porte la
  // règle serveur, `entries_` ne porte plus que le contenu à l'écran.
  bool     list_armed_ = false;
  // La liste énumère-t-elle des MATÉRIAUX à transformer plutôt que des produits ?
  // Vrai pour `AC_MAKINGARROW` seulement. Deux conséquences : l'espace de clés des
  // recettes (biaisé, cf. kArrowRecipeKeyBias) et le masquage de la colonne
  // « Faisable », qui y répéterait mot pour mot « Possédé » — une fabrication
  // consomme exactement un matériau, donc les deux colonnes sont le même nombre.
  // ⚠ Posé à l'ARRIVÉE du paquet, avant toute résolution de recette.
  bool     arrow_list_  = false;

  // ── UI ────────────────────────────────────────────────────────────────────
  // ── Survol : on MÉMORISE, on peint APRÈS ─────────────────────────────────
  // `itemcell::DrawTooltip` crée son propre popup et doit donc être appelé HORS
  // de toute fenêtre ImGui (c'est écrit tel quel dans features/item_cell.h).
  // Appelé depuis la boucle du tableau, il ne s'affichait pas du tout.
  bool     hover_valid_ = false;
  uint32_t hover_id_    = 0;
  char     hover_name_[128] = {0};

  // Matériaux optionnels choisis pour la PROCHAINE fabrication (0 = vide), et le
  // produit auquel ils se rapportent — changer de produit doit les remettre à
  // zéro, sinon on emporterait un Star Crumb sur une arme qu'on n'a pas visée.
  // Vidés après CHAQUE envoi : ils décrivent une fabrication, pas un réglage.
  uint32_t forge_slot_[3] = {0, 0, 0};
  int      forge_for_id_  = -1;

  bool ui_open_     = false;   // ouverte par le PAQUET, fermée sur demande
  int  sel_id_      = -1;      // id d'objet sélectionné (pas un index : la liste se retrie)
  bool sel_visible_ = false;   // recalculé chaque frame sur les lignes RÉELLEMENT rendues
  // (Plus de `need_focus_` : il servait à donner le focus clavier au champ de
  // filtre à chaque liste reçue. Cette fenêtre s'ouvrant sur une action DE JEU et
  // non sur un geste d'interface, cela confisquait les touches de déplacement — et
  // se répétait à chaque tour d'une chaîne de relance. Cf. DrawList.)
  char filter_[64]  = {0};

  // ── Fabrication en série ──────────────────────────────────────────────────
  // Le serveur n'accorde qu'UNE fabrication par liste : « en faire 20 » se
  // traduit donc en 20 tours complets (relance → liste → demande), pas en un
  // paquet qui porterait une quantité. `batch_left_` est ce qui reste à faire.
  //
  // Différence avec Entrée maintenue, qui produit le même effet : ici la cible
  // est CHIFFRÉE et connue d'avance. On peut lâcher le clavier, et la série
  // s'arrête toute seule au compte demandé plutôt qu'au moment où l'on pense à
  // relâcher. Volontairement NON persisté : « 50 » est une intention du moment,
  // pas un réglage — le retrouver coché au prochain lancement serait un piège.
  int      batch_target_ = 1;
  int      batch_left_   = 0;
  bool     batch_fire_   = false;  // une liste vient d'arriver, relancer la demande

  // Stock du produit visé AU MOMENT de l'envoi. 🔴 C'est notre seul recours quand
  // le serveur ne répond pas : `skill_produce_mix` n'appelle `clif_produceeffect`
  // que pour RK_RUNEMASTERY, GN_MIX_COOKING, GN_MAKEBOMB, GN_S_PHARMACY,
  // MT_M_MACHINE et BO_BIONIC_PHARMACY. SA_CREATECON, AM_PHARMACY, la forge et
  // les flèches n'y sont PAS : l'objet est créé (`pc_additem`) et AUCUN 0x018F ne
  // part. Le natif ne le voyait pas — il referme sa fenêtre à l'envoi.
  int      sent_owned_      = -1;
  // ── L'autre moitié de la sonde : le MATÉRIAU ───────────────────────────────
  // Le stock du produit ne monte qu'au succès. Les matériaux, eux, sont consommés
  // que la fabrication réussisse ou non : leur baisse prouve donc que le serveur a
  // TRAITÉ la demande, et un produit inchangé dit alors que le tirage a raté.
  //
  // C'est ce qui permet de conclure à un ÉCHEC immédiatement, au lieu d'attendre
  // le délai de garde et d'annoncer « pas de réponse » — un message doublement
  // faux, puisque le serveur avait répondu (en actes) et que rien n'était refusé.
  // 0 / -1 = pas de sonde (recette non résolue, ou faite de guides seulement).
  uint32_t sent_mat_id_     = 0;
  int      sent_mat_owned_  = -1;
  bool     awaiting_result_ = false;  // une demande est partie, on attend 0x018F
  unsigned sent_at_         = 0;      // garde anti-rafale
  uint32_t last_sent_id_    = 0;
  char     last_sent_name_[128] = {0};

  std::vector<LogLine> history_;

  // Dernier résultat reçu, affiché en pied tant qu'aucune nouvelle liste n'est
  // arrivée. C'est ce qui donne un sens à la fenêtre APRÈS une fabrication : le
  // natif, lui, compose ce message puis le jette (ZC 0x018F, §3.7). Vidé à
  // l'arrivée d'une liste.
  std::string last_result_;
  uint32_t    last_result_color_ = 0;

  // ── Maîtrise culinaire (`COOK_MASTERY`, plage [0,1999]) ───────────────────
  // -1 = pas encore reçue. Poussée par le serveur en ZC 0x0F1C au login vérifié
  // puis à chaque changement.
  //
  // 🔴 C'est la SEULE donnée du calcul de réussite d'un plat que le client ne peut
  // pas déduire : char reg serveur, aucun chemin vanilla ne la sort. Et son poids
  // n'est pas marginal — le terme aléatoire qu'elle gouverne vaut [600,2900] à 0
  // et [3000,4900] à 1999, soit une vingtaine de points de pourcentage d'écart en
  // moyenne. Sans elle on se TAIT plutôt que d'annoncer une fourchette inutile.
  //
  // Elle vaut aussi d'être montrée pour elle-même : elle monte à chaque plat
  // réussi et redescend à chaque échec, d'un pas croissant avec la difficulté, et
  // le jeu ne l'affiche NULLE PART.
  int cook_mastery_ = -1;

  // Dernière compétence lancée, captée sur les envois de CMode::SendMsg. Elle
  // n'est dans aucun paquet (§2.2) : c'est la seule source.
  int         skill_id_      = 0;
  int         skill_lv_      = 1;
  unsigned    skill_cast_at_ = 0;
  // 🔴 La liste vient-elle d'un OBJET plutôt que d'une compétence ? Le Mini
  // Furnace et les marteaux de forge sont des objets `Usable` dont le script fait
  // `produce N;` — et un `Usable` est CONSOMMÉ à chaque usage
  // (`pc_delitem(..., LOG_TYPE_CONSUME)` dans `pc_useitem`). Relancer
  // automatiquement dans ce cas brûlerait le stock du joueur en silence : la
  // relance est donc REFUSÉE, et la raison affichée.
  bool        from_item_     = false;
  // Dernier OBJET consommé par le joueur, capté sur l'envoi de CZ_USE_ITEM.
  // `source_item_id_` est celui retenu comme AYANT OUVERT la liste courante (il
  // ne vaut que si `from_item_`), figé à l'arrivée du paquet : c'est lui qu'on
  // ré-utilise, et le nom sert aux messages — un compteur qui dirait « 3 objets
  // consommés » sans nommer lequel serait inquiétant plutôt qu'informatif.
  uint32_t    item_id_        = 0;
  unsigned    item_use_at_    = 0;
  // Index brut du dernier CZ_USE_ITEM observé, et relevé du premier nœud
  // d'inventaire quand sa résolution a ÉCHOUÉ. Servent au diagnostic affiché
  // dans le journal : `item_index_` (node+0x10) est le seul champ de ce parcours
  // qui soit déduit et non vérifié en jeu, et une résolution muette ne dirait pas
  // si l'objet n'a pas été vu ou si l'offset est faux.
  unsigned    item_use_index_ = 0;
  char        item_probe_[192] = {0};
  uint32_t    source_item_id_ = 0;
  char        source_item_name_[128] = {0};
  // Exemplaires consommés PAR NOUS depuis le début de la chaîne. Affiché en
  // permanence tant que la chaîne tourne : une dépense décidée par le client doit
  // se voir pendant qu'elle a lieu, pas se découvrir après coup dans l'inventaire.
  int         auto_items_used_ = 0;
  // « La liste qui arrive est-elle la conséquence de NOTRE relance ? » Posé à
  // l'envoi, consommé à l'arrivée. Sans lui, impossible de distinguer une chaîne
  // qui se poursuit d'une liste que le joueur vient d'ouvrir lui-même.
  bool        auto_ours_      = false;
  unsigned    auto_recast_at_ = 0;
  int         auto_chain_     = 0;
  std::string auto_stop_reason_;

  // ── Réglages ──────────────────────────────────────────────────────────────
  bool show_owned_   = true;
  bool show_filter_  = true;
  bool desc_tooltip_ = true;
  bool show_history_ = false;
  bool log_time_     = true;
  // Relance automatique de la COMPÉTENCE — pas de la fabrication. Ce qui se
  // relance seul, c'est le seul geste que le serveur oblige à refaire entre deux
  // fabrications (`clif_menuskill_clear`) ; le choix du produit et le
  // déclenchement restent des clics. Opt-in, comme le refine.
  // Entrée déclenche-t-elle la fabrication ? Décoché = la touche n'est PAS
  // confisquée et le chat reste accessible pendant qu'on fabrique. Défaut OFF
  // depuis la série ×N : enchaîner ne demande plus de marteler Entrée, donc
  // confisquer une touche que le joueur utilise pour parler coûtait plus qu'elle
  // ne rapportait. Le réglage reste offert pour qui préfère l'ancien geste.
  bool enter_key_    = false;
  bool auto_recast_  = false;
  // 🔴 Relance quand la liste vient d'un OBJET. Séparée d'`auto_recast_` À DESSEIN :
  // cocher « relancer la compétence » ne doit PAS autoriser à consommer du stock.
  // L'objet est détruit par `pc_useitem` AVANT l'exécution du script, donc chaque
  // relance coûte un exemplaire même si le joueur annule ensuite. Opt-in explicite,
  // plafonné, compté à l'écran. Défaut OFF.
  bool auto_reuse_item_ = false;
  // Plafond d'exemplaires consommés par chaîne. **0 = ILLIMITÉ**, et c'est le
  // défaut : vider une pile entière est un choix légitime du joueur, pas quelque
  // chose à lui refuser. Le plafond reste offert pour l'unique cas qu'il couvre
  // vraiment — l'opt-in qu'on a oublié coché.
  //
  // Une chaîne illimitée ne peut PAS s'emballer : chaque tour exige un résultat
  // serveur pour armer le suivant, et si le serveur ignore l'usage (objet en
  // cooldown, condition non remplie) aucune liste n'arrive, donc rien ne se
  // reprogramme — la chaîne CALE, elle ne tourne pas. Les vraies conditions
  // d'arrêt (stock épuisé, liste vide, refus serveur, fermeture) suffisent.
  int  auto_reuse_max_  = 0;
  // Dernier plafond CHOISI, gardé pendant que « sans limite » est coché : sans
  // lui, décocher puis recocher effacerait le réglage du joueur. Non persisté —
  // c'est une commodité d'édition, pas un réglage.
  int  auto_reuse_cap_  = 10;
  int  pos_x_ = INT_MIN;        // INT_MIN = « jamais posée » — PAS -1 : une
  int  pos_y_ = INT_MIN;        // fenêtre à cheval sur le bord gauche a un x < 0

  Pending  pending_       = Pending::kNone;
  uint32_t pending_id_    = 0;
  bool     pos_dirty_     = false;

  // (L'ouverture de description différée au relâchement — diagnostiquée sur
  //  cette fenêtre — vit désormais dans itemcell::DeferDescById /
  //  FlushDeferredDesc, partagée par tous les viewers.)
};
