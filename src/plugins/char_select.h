#pragma once

#include <cstdint>

#include "plugins/plugin.h"

class MoonlightAuth;

// CharSelect — remplacement ImGui de l'écran de sélection de personnage.
//
// Fait partie du front « lobby » unifié (login + char-select), cf.
// docs/login_auth_imgui_design.md et docs/charselect_re.md. Au char-select on est
// déjà connecté au char-server : on lit les persos localement (dispatcher cmd 8 ->
// CHARACTER_INFO) et on envoie les paquets CH_* via Bourgeon::SendPacket ; le recv
// natif (LoginCharMode_RecvDispatch 0x00d27560) traite les réponses HC_* (maj
// liste, entrée map -> jeu seamless).
//
// INCRÉMENT 1 : lecture de la liste + grille + sélection -> entrée en jeu.
// INCRÉMENT 2 : rendu du DOLL (paperdoll) par slot (BasicInfoTweaks::RenderDoll).
// INCRÉMENT 3 (ici) : reskin « scène banquet » — un décor plein écran (BMP chargé
//   par le loader natif) et les personnages assis aux PLACES d'une table. Les 25
//   places sont pilotées par données (table kSeats, coords normalisées extraites
//   d'une image numérotée) : le slot i occupe la place n°(i+1), donc le 1er perso
//   est intronisé. CRUD via le chemin natif (DriveNativeCtrl) : créer ouvre la
//   fenêtre native, programmer/annuler la suppression = CZ 0x0827/0x082B, supprimer
//   = confirm natif. AUCUN paquet fabriqué à la main.
// SUITE : rename/move de perso, et éventuellement des dialogues create/delete
//   entièrement en ImGui (aujourd'hui on pilote le natif, cf. docs/charselect_re.md).
//
// La lecture/réseau est indépendante de la mise en page : re-placer les sièges ou
// changer le décor ne touche pas à ReadSlot/EnterGame.
class CharSelect : public Plugin {
 public:
  // `auth` (non-owning) sert à réserver l'UI ImGui au parcours de login Moonlight
  // (règle produit : login natif => char-select natif).
  explicit CharSelect(MoonlightAuth* auth);

  const char* name() const override { return "Char Select"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRenderLoginUI() override;

  bool enabled() const { return enabled_; }

 private:
  // Vue décodée d'un slot (depuis CHARACTER_INFO, offsets dans charselect_re.md).
  struct CharView {
    bool occupied = false;
    int slot = 0;
    uint32_t gid = 0;
    char name[25] = {0};
    int base_level = 0;
    int job_level = 0;
    int job = 0;
    long long hp = 0, max_hp = 0, sp = 0, max_sp = 0;
    int zeny = 0;
    char map[17] = {0};
    int str = 0, agi = 0, vit = 0, dex = 0, intel = 0, luk = 0;
    int sex = 0;           // BRUT (+0xae) : 0=F, 1=M, 99=sexe du compte
    int sex_eff = 0;       // sexe EFFECTIF (99 -> g_Account_Sex) — pour le doll
    int del_rev_date = 0;  // >0 = suppression en cours (délai restant, secondes)
    // Apparence (paperdoll) — mêmes champs que ceux que le char-select natif passe
    // à Actor_Init (UINewSelectCharWnd_RenderSlots 0x0079d170). Ni arme (+0x5a) ni
    // bouclier (+0x62) : le natif ne les affiche pas au char-select.
    int body = 0;           // +0x58 body style (ID de classe côté serveur)
    int hair = 0;           // +0x56
    int hair_color = 0;     // +0x68
    int clothes_color = 0;  // +0x6a
    int head_low = 0;       // +0x60 head_bottom
    int head_top = 0;       // +0x64 head_top
    int head_mid = 0;       // +0x66 head_mid
    int garment = 0;        // +0xa2 robe
    int moves_avail = 0;    // +0xa6 changements de slot restants (coupon change-slot)
    int rename_avail = 0;   // +0xaa renommages restants (coupon rename)
  };

  // Nombre de slots à parcourir (capacité runtime, jamais codée en dur : suit un
  // futur MAX_CHARS serveur 45->60 sans changement).
  int SlotCapacity() const;
  // Lit + décode le slot ; renvoie occupied=false si le slot est vide.
  bool ReadSlot(int slot, CharView* out) const;
  // Entre en jeu avec ce slot, en PILOTANT la séquence native (voir .cc) — surtout
  // pas en envoyant CH_SELECT_CHAR à la main : le natif fait bien plus que ça.
  void EnterGame(int slot);

  // Dessine le paperdoll du slot ancré sur son siège : pieds au point (cx, chair_y),
  // corps de hauteur `box_h` (px écran) étendu vers le haut, centré en X. Moteur de
  // capture partagé (BasicInfoTweaks::RenderDoll) ; placeholder si la capture n'est
  // pas prête (budget par frame) ou échoue.
  void DrawDollAt(const CharView& v, float cx, float chair_y, float box_h);

  // Dessine l'aperçu doll de CRÉATION (look Novice construit depuis create_*) dans le
  // rect écran [x,y]-[x+w,y+h], pieds en bas, centré. Même moteur que DrawDollAt.
  void DrawCreateDoll(float x, float y, float w, float h);

  // Dessine l'icône de coiffure (id `hair`, sexe courant `create_sex_`) façon natif —
  // frame du .spr — ajustée à une case carrée `sz` au point (x,y). Pour la grille.
  void DrawHairIcon(int hair, float x, float y, float sz, int color_override = -1);

  // Pilote un contrôle de la fenêtre native UINewSelectCharWnd (OnMsg 0x0079d610,
  // msg 6 / `ctrl`) après avoir posé g_CharSelect_SelectedSlot = `slot`. C'est le
  // chemin natif : on n'émet AUCUN paquet nous-mêmes (le natif les construit).
  //   0xB8 = entrer en jeu ; 0x1A0 = créer (ouvre la fenêtre native 0xC8) ;
  //   0x197 = programmer la suppression (CZ 0x0827) ; 0x198 = annuler (CZ 0x082B) ;
  //   0xD3 = supprimer maintenant (confirm + code natif). Voir docs/charselect_re.md.
  void DriveNativeCtrl(int ctrl, int slot);

  // Envoie une commande au MODE courant (dispatcher *(0x0121333c), vtbl+0x18) — même
  // point d'entrée que ReadSlot (cmd 8). Sert aux deux issues de l'écran :
  //   10011 (0x271B) = déconnexion + retour à l'écran de connexion ;
  //   2              = quitter le jeu (arrêt des sous-systèmes + fin de boucle).
  // On ne passe pas par le « Cancel » natif (ctrl 185) : sa msgbox de confirmation
  // est supprimée sous notre UI, il conclurait « annulé ». Cf. char_select.cc.
  void DriveModeCmd(int cmd);

  MoonlightAuth* auth_ = nullptr;  // non-owning ; gate « parcours Moonlight »
  bool enabled_ = true;   // activé par défaut ; opt-out via char_select.imgui:false
  bool force_ = false;    // dev : ignore le gate Moonlight (tester via login natif)
  bool active_ = false;   // on est bien sur l'écran char-select (persos chargés)
  int selected_ = -1;     // slot sélectionné dans la scène
  int page_ = 0;          // page des bancs (0 = persos 0-24 ; d'honneur fixes)
  // Filet : bascule vers le char-select NATIF pour la session (l'ImGui se masque ->
  // plus de capture). Réarmé à chaque retour sur l'écran de login.
  bool native_fallback_ = false;
  // Opération native ÉPHÉMÈRE (création / suppression définitive) : on se découvre
  // le temps du dialogue natif (fenêtre 0xC8 ou confirm), puis on se re-couvre
  // automatiquement quand la liste change (nfilled) ou sur clic « Revenir ».
  bool native_op_ = false;
  int  op_prev_nfilled_ = -1;
  // Edge-trigger de l'entrée en jeu : la séquence native pose un état de mode qui
  // serait ré-armé à chaque frame si on la rejouait. Réarmé au retour à l'écran.
  bool entering_ = false;
  unsigned long enter_tick_ = 0;  // GetTickCount() à l'instant de l'entrée en jeu (fondu)
  // Fermeture du jeu demandée (bouton « Quitter le jeu ») : même principe que
  // `entering_`, on masque la table derrière un fondu au noir le temps que la boucle
  // principale sorte.
  bool quitting_ = false;
  unsigned long quit_tick_ = 0;
  // Écran quitté (« Revenir au login » / fin de transition) : on ne dessine plus rien
  // jusqu'à ce qu'un NOUVEAU char-select natif s'ouvre. Nécessaire parce qu'un
  // re-login ne change pas de MODE : OnModeSwitch ne repasse pas, et les
  // CHARACTER_INFO restent lisibles un moment après la déconnexion.
  bool left_ = false;
  // Éditeur de sièges/layout : glisser les marqueurs pour caler les positions sur le
  // décor, molette = taille, bouton « Dump layout » -> bourgeon.log. Outil d'auteur.
  // ⚠ DÉSACTIVÉ : le layout est calé, plus aucun déclencheur ne met ce flag à true
  // (le bloc F10 est commenté en bas de OnRenderLoginUI) -> tout le chemin d'édition
  // est du code mort, conservé pour un futur recalage.
  bool seat_edit_ = false;
  // Résultat de « Programmer suppression ». Le serveur répond HC_DELETE_CHAR3_RESERVED
  // (0x0828 ; result 1=ok, 4=guilde, 5=groupe, 3=db, 6=échoppe, 0=déjà en file). On
  // DÉTOURNE le handler natif Net_OnDeleteCharReserveAck 0x00d21210 (cf. .cc) : sur
  // échec on supprime sa msgbox modale (invisible sous notre UI) et on récupère le
  // code EXACT -> bandeau ImGui distinguant guilde/groupe/etc.
  unsigned long del_reject_until_ = 0;     // affiche le bandeau de refus jusqu'à ce tick
  int del_reject_reason_ = 0;              // code result du dernier refus (4/5/3/6/0…)
  unsigned long del_reject_seq_seen_ = 0;  // dernier n° de refus traité (détecte un nouveau)
  // Email du compte saisi pour la suppression DÉFINITIVE. moonlight a
  // char_del_option=1 (= EMAIL dans rAthena) : le natif envoie CH_DELETE_CHAR 0x01fb
  // avec l'email dans key[50], et le serveur exige l'email EXACT du compte (pas
  // d'"a@a.com" par défaut). Partagé entre persos (même compte). Cf. char_select.cc.
  char del_email_[64] = {0};
  // Cible du popup de confirmation de suppression (capturée au clic « Supprimer »
  // pour ne pas dépendre de selected_ / de la liste pendant que le popup est ouvert).
  uint32_t del_popup_gid_ = 0;
  int      del_popup_slot_ = -1;  // slot -> pose g_CharSelect_SelectedSlot avant l'envoi
  char del_popup_name_[25] = {0};

  // ── Création : popup ImGui + aperçu doll live (envoi CH_MAKE_CHAR 0xa39) ────────
  // Déclenchée en cliquant un siège LIBRE. On construit un look Novice (job/body 0)
  // depuis ces champs pour l'aperçu, et on envoie le paquet NOUS-MÊMES (comme la
  // suppression) au lieu de piloter la fenêtre native 0xC8.
  int  create_slot_ = -1;       // slot du siège libre cliqué (-1 = pas de création)
  char create_name_[24] = {0};  // NAME_LENGTH côté serveur = 24
  int  create_hair_ = 1;        // style de cheveux (>=1 ; bornes douces, l'aperçu guide)
  int  create_hair_color_ = 0;  // couleur de cheveux (>=0)
  int  create_sex_ = 1;         // 0=F, 1=M (défaut = sexe du compte, posé à l'ouverture)
  int  create_dir_ = 0;         // direction de l'aperçu (0..7), pilotée par les flèches
  // Attente du résultat de création : le char-server pousse le perso au slot en cas de
  // succès ; en cas de refus (nom pris/invalide) il envoie HC_REFUSE_MAKECHAR 0x006e
  // que notre recv NE voit PAS (dispatch char-server). On DÉDUIT donc : slot rempli =
  // succès -> on ferme ; rien après un court délai = échec -> message dans le popup.
  bool          create_pending_ = false;
  unsigned long create_sent_tick_ = 0;
  // Séquence de modales natives supprimées, capturée à l'envoi : si elle bouge pendant
  // l'attente, le serveur a refusé (nom pris) via une msgbox qu'on a supprimée -> échec
  // IMMÉDIAT, sans attendre le timeout (le refus HC_REFUSE_MAKECHAR 0x6e est hors recv).
  unsigned long create_modal_base_ = 0;
  // Hauteur du groupe formulaire (droite) mesurée à la frame N-1 : sert à CENTRER
  // verticalement l'aperçu doll dans la colonne gauche (immédiat -> décalage d'1 frame,
  // invisible car le popup persiste). 0 la 1re frame -> aperçu à sa taille nominale.
  float         create_form_h_ = 0.0f;
  bool          create_failed_ = false;
  // Le clic « siège libre » est DANS ImGui::PushID(i) (boucle des sièges) -> on ne peut
  // pas y appeler OpenPopup (ID scopé au siège, ne matcherait pas BeginRoPopupModal au
  // niveau racine). On lève ce drapeau et on ouvre le popup APRÈS la boucle.
  bool          create_open_req_ = false;

  // ── Coupons : Renommer (12790) / Changer de slot (12786) ─────────────────────
  // Menu contextuel (clic droit sur un perso) + popups ImGui. Dispo = FLAG dans
  // CHARACTER_INFO (moves_avail/rename_avail), pas l'inventaire (absent au char-select).
  // On envoie les paquets NOUS-MÊMES (comme create/delete) : rename CH_REQ_CHANGE_CHARNAME
  // 0x08fc (PACKETVER≥20111101 : nom dans le paquet, pas besoin du 0x028d) ; move slot
  // CH_REQ_CHANGE_CHARACTER_SLOT 0x08d4. Résultat déduit (ack char-server hors recv).
  int           ctx_slot_ = -1;          // slot ciblé par le menu contextuel (-1 = aucun)
  bool          ctx_open_req_ = false;   // ouverture différée (clic droit dans PushID)
  // Popup Renommer
  bool          rename_open_req_ = false;
  int           rename_slot_ = -1;
  uint32_t      rename_gid_ = 0;
  char          rename_buf_[24] = {0};
  char          rename_old_[25] = {0};   // nom d'origine -> détecte le succès (slot renommé)
  bool          rename_pending_ = false;
  unsigned long rename_sent_tick_ = 0;
  unsigned long rename_modal_base_ = 0;  // échec instantané (modale native supprimée)
  bool          rename_failed_ = false;
  // Popup Changer de slot
  bool          move_open_req_ = false;
  int           move_from_ = -1;         // slot source
  uint32_t      move_gid_ = 0;
  int           move_to_ = -1;           // slot cible choisi (siège libre)
};
