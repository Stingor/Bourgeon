#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "plugins/plugin.h"

// ── RodexTweaks ──────────────────────────────────────────────────────────────
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
//   UIMailWriteWnd (COMPOSE) vtable 0x01021b30 — id 0x108 : laissée NATIVE
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

class RodexTweaks : public Plugin {
 public:
  const char* name() const override { return "RodexTweaks"; }

  void OnTick() override;
  void OnRenderUI() override;

  // Appelé pour CHAQUE fenêtre créée (hook MakeWindow de WindowPosTweaks) :
  // cache la liste (0x107) et la lecture (0x109) dès leur création, avant leur
  // premier rendu — sans ça la fenêtre native clignote pendant les ~100 ms qui
  // séparent deux OnTick (la lecture est créée par le handler du paquet 0x0B63,
  // donc entre deux ticks). No-op si le plugin est désactivé.
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
  // Une pièce jointe d'un courrier (bloc de 60 octets dans le RodexMail).
  struct Attach {
    uint32_t id = 0;
    int      amount = 0;
    int      refine = 0;
    bool     identified = true;
    uint32_t cards[4] = {0, 0, 0, 0};
    // Renseigné UNIQUEMENT pour les pièces jointes d'un courrier en cours
    // d'écriture (elles vivent encore dans l'inventaire du joueur) : c'est cet
    // index que la commande de retrait attend. 0 pour un courrier reçu.
    int      inv_index = 0;
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
  void CloseAll();                        // ferme liste + lecture natives

  // Rendu des sous-parties (une fenêtre, deux zones) + la fenêtre d'écriture.
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
  void CloseCompose();              // ferme la fenêtre native -> le natif annule côté serveur

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
  bool    compose_open_ = false;   // la fenêtre native d'écriture existe ?
  bool    compose_pos_ = true;
  char    to_[32] = {0};           // saisie UTF-8 (convertie en ANSI à l'envoi)
  char    subject_[128] = {0};
  char    body_[1400] = {0};
  int64_t attach_zeny_ = 0;
  int64_t tax_ = 0;                // frais d'envoi, lus dans la fenêtre native (+0xf8)
  uint32_t recipient_char_id_ = 0; // posé par l'ack de vérification (fenêtre +0xcc)

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
};
