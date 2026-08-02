#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "features/net_inbox.h"
#include "features/plugin.h"

// ── RodexWindow ──────────────────────────────────────────────────────────────
//
// Remplacement ImGui (skin RO) du courrier RODEX : la fenêtre LISTE (UIRodexWnd,
// id 0x107) et la fenêtre LECTURE (UIRodexReadWnd, id 0x109) natives sont cachées
// et remplacées par une fenêtre unique « Courrier » — liste en haut, contenu du
// courrier sélectionné en bas (corps, zeny, pièces jointes, actions).
//
// RE complète : cf. docs/rodex_re.md (corrigé 2026-07-26 par RE live + IDA).
// Vtables (20250716, base 0x400000, pas de rebase) :
//   UIRodexWnd     (LISTE)   vtable 0x01022170 — id 0x107, OnMsg 0x007d0520
//   UIRodexReadWnd (LECTURE) vtable 0x01021fbc — id 0x109, OnMsg 0x007cb460
//   UIMailWriteWnd (COMPOSE) vtable 0x01021b30 — id 0x108 : ne naît plus
// (l'ancien doc attribuait ces deux vtables à l'envers ; l'id lu live dans
//  g_RodexInboxWnd+0x2c vaut bien 0x107 pour la vtable 0x01022170).
//
// SOURCE DES DONNÉES : aucun parsing de paquet. Tout l'état des trois boîtes est
// déjà agrégé par le natif dans le singleton `g_RodexMgr` (DAT_0131ecdc) sous
// forme de trois std::map<int64 mailID, RodexMail> (Normal +0x20, Compte +0x28,
// Retour +0x30). Le plugin ITÈRE ces maps et ÉMET les mêmes commandes que les
// fenêtres natives — le serveur (fork rAthena moonlight, src/map/rodex.cpp)
// reste seul juge de ce qui est autorisé.
//
// COMMANDES (toutes vérifiées sur les handlers de boutons natifs) :
//   rafraîchir  : purge locale (0x007c72e0) puis CZ 0x0AC1 {3 mailID les + récents}
//   lire        : CMode::SendMsg cmd 0xc2 (mailID_lo, openType)  -> CZ 0x09EA
//   supprimer   : CMode::SendMsg cmd 0xc4 (mailID_lo, openType)  -> CZ 0x09F5
//   réclamer    : CZ 0x09F3 (objets, si type&4) / CZ 0x09F1 (zeny, si type&2)
//   retourner   : CZ 0x0B98 {u32 mailID_lo} (boîte Normal uniquement)
//   écrire      : CMode::SendMsg cmd 0x10c (nom du destinataire, ou 0)
//
// Défaut OFF (opt-in, setting « rodex_imgui » géré par MoonlightUi).

class RodexWindow : public Plugin {
 public:
  RodexWindow();
  const char* name() const override { return "RodexWindow"; }

  void OnTick() override;
  void OnRenderUI() override;
  // ZC_ACK_OPEN_WRITE_MAIL 0x0A12 (dont on a pris la place : c'était le seul
  // créateur de la fenêtre d'écriture native) et ZC_ACK_WRITE_MAIL 0x09ED (observé :
  // le résultat de l'envoi). Fil RÉSEAU — on copie, le décodage repart au tick.
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Appelé pour CHAQUE fenêtre créée (hook MakeWindow de WindowPosTweaks) : masque
  // la native dès sa création, avant son premier rendu — sans ça elle clignote
  // pendant les ~100 ms qui séparent deux OnTick (la lecture naît du handler de
  // ZC 0x0B63, donc entre deux ticks). No-op si le plugin est désactivé.
  //
  // 🔴 Pour la LISTE (0x107), c'est aussi le point de bascule : sa création EST la
  // demande du joueur (icône de menu, PNJ). Elle est détruite au tick suivant, donc
  // n'existe jamais — et le « ferme si elle existe, sinon crée » du client repasse
  // forcément par ici. Masquer ne suffirait pas : un appui sur deux serait avalé, et
  // la native garderait le clavier.
  void HideNativeAtCreation(void* win, int window_id);

  // Attache l'objet d'inventaire `index` (quantité `amount`) au courrier en cours
  // d'écriture — même commande que le drop natif sur la fenêtre d'écriture.
  // Public : cible de drag-drop et menu contextuel de l'InventoryViewer.
  void AttachItem(int index, int amount);

  // True si la fenêtre d'ÉCRITURE ImGui est active. Lu par l'InventoryViewer pour
  // proposer « Joindre au courrier » et router un objet lâché dessus.
  bool composing() const { return imgui_enabled_ && compose_open_; }

  // Ouvre l'écriture d'un courrier avec le destinataire DÉJÀ rempli — ce que fait
  // « Send a mail... » du menu contextuel natif. `recipient` est un nom en CP949
  // (l'encodage du jeu), ou nullptr pour laisser le champ vide.
  // Marche même quand la surcouche ImGui est désactivée : c'est le serveur qui crée
  // la fenêtre d'écriture en réponse à la commande, native ou non.
  // Public : appelé par les listes de joueurs (membres de guilde, amis…).
  void ComposeTo(const char* recipient);

  // Setting PERSISTANT (« rodex_imgui », géré par MoonlightUi). Défaut OFF.
  // Fait partie du groupe « Interface moderne » (SetModernInterface) : le courrier
  // s'active avec l'inventaire, l'entrepôt, les barres et l'échange, parce que ses
  // pièces jointes se glissent depuis l'inventaire ImGui.
  bool imgui_enabled_ = false;

 private:
  // Une pièce jointe d'un courrier (bloc de 60 octets dans le RodexMail, ou slot de
  // rédaction — 0xF8, un vrai ItemSkillInfo de session).
  struct Attach {
    uint32_t id = 0;
    int      amount = 0;
    int      refine = 0;
    bool     identified = true;
    // ISI+0x5d : équipement CASSÉ (nom ombré + icône teintée, cf. itemcell).
    // RE ida 0x00cfd0c0 (Recv_ZC_AckReadRodex_0x0B63) pour le bloc réseau : le natif
    // recopie lui-même l'octet du bloc +7 à cet offset avant d'ouvrir SA fenêtre de
    // lecture — jusqu'ici cet octet n'était pas lu par le plugin.
    bool     damaged = false;
    uint8_t  grade = 0;   // ISI+0x88 : grade d'enchantement (décore le nom natif)
    // ⚠ `type` n'est pas décoratif : le name-builder natif s'en sert pour décider
    // si l'objet mérite des affixes (ItemTitle_IsDecoratedType). Sans lui, une arme
    // cardée sort avec son nom nu. `location`/`view` ne servent, eux, qu'à
    // déverrouiller le bouton « aperçu » de la fenêtre de description.
    int      type = 0;        // ISI+0x00
    uint32_t location = 0;    // ISI+0x08 : masque d'emplacement d'équipement
    uint16_t view = 0;        // ISI+0x70 : viewID
    uint32_t cards[4] = {0, 0, 0, 0};
    // Options aléatoires d'instance (enchants), ISI+0x9c : jusqu'à 5, tassées depuis
    // l'entrée 0 par le serveur. Pour le bloc réseau : +33, stride 5 (même RE).
    struct Opt { int16_t index = 0; int16_t value = 0; uint8_t param = 0; };
    int      opt_count = 0;
    Opt      opts[5] = {};
    // Nom complet (refine + affixes de cartes, name-builder natif) et emplacements
    // totaux : résolus une fois à la lecture par ResolveAttachDisplay, à partir d'un
    // ItemSkillInfo reconstruit (cf. BuildAttachInfo) — jamais du texte inventé.
    char     name[96] = {0};
    int      total_slots = 0;
    // Renseigné UNIQUEMENT pour les pièces jointes d'un courrier en cours
    // d'écriture (elles vivent encore dans l'inventaire du joueur) : c'est cet
    // index que la commande de retrait attend. 0 pour un courrier reçu.
    int      inv_index = 0;
    // Emplacement (0..4) dans les slots de rédaction de la SESSION (kMailAttachSlot) :
    // adresse FIXE qu'on peut passer telle quelle comme `src` à
    // itemcell::DeferDescById, contrairement à un ItemSkillInfo de pile qui ne
    // survivrait pas jusqu'au relâchement du clic. -1 pour un courrier reçu (dans ce
    // cas la description passe par desc_scratch_).
    int      mail_slot = -1;
  };

  // Un courrier, copié depuis la std::map du natif (jamais de pointeur conservé
  // d'une frame à l'autre : le natif peut réallouer/effacer ses nœuds).
  struct Mail {
    int64_t     id = 0;
    uint8_t     box = 0;      // boîte d'origine (0 Normal / 1 Compte / 2 Retour)
    uint8_t     type = 0;     // MAIL_TYPE : &2 = zeny joint, &4 = objets joints
    bool        is_read = false;  // état SERVEUR (mail+0x18) : sert au style « non lu »
    // Le contenu (corps, zeny, pièces jointes) a-t-il transité pendant CETTE session ?
    // Rien dans la structure native ne le dit — le nœud créé par la liste laisse ces
    // champs non initialisés —, c'est le détour du handler ZC 0x0B63 qui l'établit.
    // Tant que c'est faux, on n'affiche RIEN de ce contenu : ce serait du bruit.
    bool        content_ready = false;
    int64_t     expire = 0;
    int64_t     zeny = 0;
    std::string sender;      // affichage (UTF-8)
    // Le même nom dans l'encodage du CLIENT (ANSI) : c'est lui qu'on repasse au
    // natif pour « Répondre », qui construit un paquet — de l'UTF-8 y casserait
    // un nom accentué.
    std::string sender_raw;
    std::string title;
    std::string body;
    std::vector<Attach> items;
  };

  // ── Pièces jointes : nom complet et description ──────────────────────────
  // Une pièce jointe n'est PAS un item d'inventaire : elle vit dans un bloc de 60
  // octets du RodexMail, que rien du client ne sait nommer ni décrire. Le
  // name-builder et la fenêtre de description veulent tous deux un ItemSkillInfo ;
  // on le fabrique donc à partir du bloc, dont la RE (0x00cfd0c0) montre qu'il
  // porte exactement les mêmes champs.
  //
  // `out_info` = tampon d'au moins 0x100 octets. False si la fabrication échoue.
  // ⚠ Les slots de RÉDACTION, eux, sont déjà de vrais ItemSkillInfo de session : on
  // ne reconstruit rien pour eux, on pointe l'original (cf. `mail_slot`).
  static bool BuildAttachInfo(const Attach& attach, void* out_info);
  // Renseigne `name` (nom décoré, UTF-8) et `total_slots`. Repli sur le nom de base
  // quand le builder natif ne rend rien — jamais de champ laissé indéterminé.
  static void ResolveAttachDisplay(Attach* attach);
  // Icône + nom composé + survol + clic droit, pour UNE pièce jointe. Partagée par
  // la lecture et la rédaction : c'est la même cellule, seul « Retirer » diffère.
  void DrawAttachRow(const Attach& attach, bool removable);
  // L'aperçu au survol, dessiné hors de toute fenêtre (c'est un popup).
  void DrawAttachTooltip();

  // Lit les trois boîtes de g_RodexMgr dans mails_ (thread principal, SEH).
  void ReadState();
  // Le courrier sélectionné dans mails_, ou nullptr.
  const Mail* Selected() const;

  // ── Actions (thread principal uniquement) ──
  void RequestRefresh();                  // purge + CZ 0x0AC1
  void OpenMail(const Mail& mail);        // cmd 0xc2 -> CZ 0x09EA (corps + objets)
  void ClaimAttachments(const Mail& mail);// CZ 0x09F3 / 0x09F1 selon `type`
  void DeleteMail(const Mail& mail);      // cmd 0xc4 -> CZ 0x09F5
  void ReturnMail(const Mail& mail);      // CZ 0x0B98 (boîte Normal uniquement)
  void Compose(const char* recipient);    // cmd 0x10c (0 = destinataire vide)
  void CloseAll();                        // referme la session (+ filet natif)
  // Décodage sur le fil PRINCIPAL, drainé depuis OnTick.
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len);
  bourgeon::PacketInbox net_inbox_;
  // Oublie la rédaction côté UI SANS rien émettre. CloseCompose, lui, émet d'abord
  // l'annulation — à ne pas confondre : après un envoi RÉUSSI, annuler serait un
  // contresens (la session serveur est déjà close).
  void ClearComposeState();
  // Oublie la session de boîte aux lettres SANS toucher au natif. Séparé de
  // CloseAll parce qu'on l'appelle depuis le hook de création, où détruire la
  // fenêtre que le client vient de créer serait un usage-après-libération.
  void ResetMailboxState();

  // Rendu des sous-parties (une fenêtre, deux zones) + la fenêtre d'écriture.
  // DrawMailbox porte la fenêtre « Courrier » elle-même : elle est séparée
  // d'OnRenderUI parce que celle-ci doit encore dessiner l'aperçu de description
  // APRÈS, hors de tout Begin/End — ce que les retours anticipés d'ici sautaient.
  void DrawMailbox();
  void DrawMailList();
  void DrawMailDetail();
  void DrawConfirmPopup();   // modale « supprimer / retourner » (actions irréversibles)
  void DrawComposeWindow();

  // ── Écriture (fenêtre native 0x108 masquée, surcouche ImGui) ──
  void ReadComposeState();          // pièces jointes + taxe + destinataire validé
  void RemoveAttachment(int index, int amount);  // CZ 0x0A06
  void CheckRecipient();            // CZ 0x0B97 (confort : niveau/classe du destinataire)
  void PollRecipientCheck();        // consomme la réponse ZC 0x0A51 du détour
  void SendMail();                  // CZ 0x0A6E (construit ici, cf. .cc)
  void CloseCompose();              // émet CZ 0x0A03 : annule la rédaction côté serveur

  bool    open_ = false;       // la liste native (0x107) est ouverte ?
  bool    was_open_ = false;
  bool    show_panel_ = true;  // détection du clic sur le X de la fenêtre ImGui
  bool    need_pos_ = true;
  int     spawn_x_ = 380, spawn_y_ = 150;  // repris de la position native

  int     tab_ = 0;            // 0 Normal / 1 Compte / 2 Retour / 3 Tous
  // Filtre de la liste (expéditeur OU sujet), l'équivalent du bouton « rechercher »
  // natif. Ne touche PAS aux compteurs d'onglets : ils annoncent le contenu réel de
  // chaque boîte, qui ne doit pas fondre pendant qu'on tape.
  char    filter_[64] = {0};
  int64_t selected_id_ = 0;    // courrier sélectionné (0 = aucun)
  uint8_t selected_box_ = 0;   // sa boîte (nécessaire dans l'onglet « Tous »)

  std::vector<Mail> mails_;    // instantané des trois boîtes (ReadState)
  int     unread_ = 0;         // g_RodexMgr+0x18
  // g_RodexMgr+0x1c : « une liste a été reçue ». NE dit PAS si la boîte contient
  // quelque chose (il vaut 1 même sur une boîte vide) — cf. le commentaire du .cc.
  bool     list_received_ = false;
  // Horodatage de la dernière demande de liste : c'est LUI qui distingue une boîte
  // en cours de chargement d'une boîte réellement vide. Sans cette borne, un joueur
  // sans courrier restait devant « Chargement… » indéfiniment.
  uint32_t list_requested_ms_ = 0;

  // Confirmations : le natif passe par une boîte modale pour supprimer/retourner.
  // On garde le même garde-fou, en modale ImGui (0 = aucune demande en cours).
  enum Confirm { kConfirmNone = 0, kConfirmDelete, kConfirmReturn };
  int     confirm_ = kConfirmNone;

  // Ouverture/fermeture du bloc « courrier sélectionné » : la fenêtre s'agrandit
  // de la hauteur du bloc à la lecture, et se rétracte d'autant à la fermeture.
  // On travaille en DELTA sur la hauteur mesurée, pour ne pas écraser un
  // redimensionnement manuel du joueur.
  bool    detail_shown_ = false;
  float   last_width_ = 0.0f;      // taille mesurée à la frame précédente
  float   last_height_ = 0.0f;
  float   pending_height_ = 0.0f;  // > 0 : hauteur à appliquer à cette frame

  // ── Écriture d'un courrier (fenêtre native 0x108 masquée) ──
  bool    compose_open_ = false;   // rédaction en cours (ouverte par ZC 0x0A12)
  bool    compose_pos_ = true;
  char    to_[32] = {0};           // saisie UTF-8 (convertie en ANSI à l'envoi)
  char    subject_[128] = {0};
  char    body_[1400] = {0};
  int64_t attach_zeny_ = 0;
  int64_t tax_ = 0;                // frais d'envoi, recalculés (formule du client)
  uint32_t recipient_char_id_ = 0; // posé par l'ack de vérification de nom (ZC 0x0A51)

  // Réponse du serveur à la vérification du destinataire (ZC 0x0A51). Le char id
  // ne sert qu'en interne — il ne dit rien à un joueur ; ce qui l'aide à confirmer
  // qu'il vise la bonne personne, c'est le MÉTIER et le NIVEAU.
  enum Checked { kCheckNone = 0, kCheckPending, kCheckUnknown, kCheckFound };
  int         check_state_ = kCheckNone;
  long        check_seq_ = 0;        // dernier compteur de réponse consommé
  std::string checked_name_;         // nom tel que le serveur l'a réémis (UTF-8)
  std::string checked_job_;          // métier lisible, résolu par le getter natif
  int         checked_level_ = 0;
  std::vector<Attach> compose_items_;  // pièces jointes lues dans les slots de session
  std::string send_error_;         // dernier refus local (champs invalides)

  // Pièce jointe survolée cette frame (COPIE : les vecteurs se reconstruisent à
  // chaque tick, un pointeur ne passerait pas la frame). `valid` retombe à faux au
  // début de chaque rendu ; c'est lui qui décide si l'aperçu se dessine.
  Attach hover_attach_;
  bool   hover_attach_valid_ = false;
  // ItemSkillInfo fabriqué au clic droit sur une pièce jointe REÇUE, source de la
  // description différée. Membre et non pile : itemcell::DeferDescById n'ouvre la
  // fenêtre qu'au relâchement du bouton, et exige que la source vive jusque-là.
  // Une seule demande en vol (une souris, un geste) : un tampon suffit.
  uint8_t desc_scratch_[0x100] = {0};
};
