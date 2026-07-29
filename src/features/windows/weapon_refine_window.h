#pragma once

#include <climits>  // INT_MIN : sentinelle « position jamais mémorisée »
#include <cstdint>
#include <string>
#include <vector>

#include "features/plugin.h"
#include "imgui.h"  // ImVec2 (traces de géométrie de la modale)

// ── WeaponRefineWindow ───────────────────────────────────────────────────────
//
// Remplacement ImGui de la fenêtre native « Upgradeable weapons »
// (UIWeaponRefineWnd, id 111), celle qu'ouvre le skill Whitesmith
// WS_WEAPONREFINE. RE complète : docs/weapon_refine_re.md (mémoire
// project_weapon_refine_re).
//
// Modèle « capture recv → état → overlay → send », comme NpcDialogWindow :
// OnRecvPacket ne fait QUE bâtir le modèle, jamais d'envoi ni d'opération de
// fenêtre.
//
// ── Pourquoi ce plugin lit le PAQUET et pas la fenêtre native ────────────────
// Chaque entrée de ZC_NOTIFY_WEAPONITEMLIST (0x0221) fait 23 octets :
//     u16 index ; u32 itemId ; u8 refine ; u32 card[4]
// et le client n'en lit QUE `index` et `itemId` — le niveau de refine et les
// quatre cartes sont jetés. Ses deux vecteurs (fenêtre +0xB8 / +0xC4) ne les ont
// donc pas : la seule source est le paquet lui-même. C'est ce qui permet
// d'afficher ici « +7 » et les cartes, ce que le natif ne peut pas faire.
//
// ── Les trois pièges du natif, tous reproduits fidèlement ────────────────────
//  1. `index` est déjà décalé côté serveur (`client_index(i)` = i+2). On le
//     renvoie TEL QUEL dans CZ_REQ_WEAPONREFINE — aucun ±2 à faire.
//  2. Fermer sans envoyer cmd 182 avec `index = -1` laisse le `menuskill` ARMÉ
//     côté serveur (clif_parse_WeaponRefine n'appelle clif_menuskill_clear que
//     s'il reçoit le paquet). Annuler DOIT donc envoyer, comme le bouton natif.
//  3. Un échec DÉTRUIT l'arme (`pc_delitem`), et MsgString 911 (succès) et 912
//     (échec) portent le MÊME texte côté client. D'où la confirmation et
//     l'historique coloré ici.
//
// ── La modale native « liste vide » ──────────────────────────────────────────
// Quand le serveur renvoie une liste vide, le handler natif affiche une modale
// BLOQUANTE portant MsgString 424 (« You can't create items yet. »), un libellé
// recyclé de la fabrication qui ne parle ni d'arme ni de minerai. On l'escamote
// par un détour ONE-SHOT sur UIWndMgr_ShowMessageBoxModal, armé uniquement à la
// réception d'un 0x0221 vide et désarmé au premier appel — cf. le commentaire du
// .cc, qui détaille pourquoi c'est sûr malgré les ~250 sites d'appel.
//
// OPT-IN : membre du groupe « Interface moderne » (SetModernInterface), jamais
// basculé isolément.

class WeaponRefineWindow : public Plugin {
 public:
  WeaponRefineWindow();

  const char* name() const override { return "WeaponRefineWindow"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  // Quitter le monde de jeu (warp de serveur, retour au char-select) referme la
  // session : le `menuskill` serveur ne survit pas, notre fenêtre ne doit pas le
  // laisser croire.
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Rejoue les actions natives (envoi cmd 182, fermeture de la fenêtre 111,
  // relance du skill) HORS de toute frame ImGui. Appelé par
  // Bourgeon::OnProcessInput, comme VendingWindow::FlushPending : ces chemins
  // natifs peuvent déclencher une modale bloquante, et la déclencher entre
  // NewFrame() et Render() gèlerait le client (cf. docs/weapon_refine_re.md §6).
  void FlushPending();

  // Appelé par le hook MakeWindow (WindowPosTweaks) à la création de la fenêtre
  // 111 : masque la native (win+0x28 = 0) AVANT son premier rendu. Le seul
  // OnTick laisserait passer une frame native — la fenêtre est créée par le
  // handler de paquet, donc entre deux ticks. No-op si le plugin est désactivé.
  void HideNativeAtCreation(void* win);

  // Le hook de WndProc demande ici s'il doit AVALER la touche Entrée avant que
  // le jeu ne la voie (elle y ouvre la saisie de chat).
  //
  // La réponse ne dépend QUE de la présence de la fenêtre, jamais de l'état du
  // bouton. Calquée sur l'action, elle laissait passer la touche dans tous les
  // creux du cycle — tentative en vol, liste consommée, relance en cours — si
  // bien qu'enchaîner les refines à coups d'Entrée ouvrait et refermait le chat
  // sans arrêt. C'est exactement la règle de ro::AnyEscapeWindowOpen() pour
  // Échap : une fenêtre RO ouverte s'approprie la touche, point.
  //
  // Contrepartie assumée : pas de chat à Entrée tant que la fenêtre est là. Elle
  // se ferme d'un clic, et le champ de chat reste cliquable.
  bool WantsEnterKey() const;

  // Contenu de la section « Refine » du panneau Moonlight. Rend true si un
  // réglage a changé (l'appelant sauvegarde une seule fois).
  bool DrawSettings();

  // ── Settings PERSISTANTS (bourgeon_settings.yaml, gérés par MoonlightUi) ──
  // « refine_imgui » : basculé en GROUPE par SetModernInterface. Défaut OFF.
  bool imgui_enabled_ = false;
  // « refine_confirm » : demander confirmation avant de jouer une arme.
  bool& confirm()      { return confirm_; }
  // « refine_show_cards » : colonne cartes/emplacements.
  bool& show_cards()   { return show_cards_; }
  // « refine_filter » : champ de filtre au-dessus de la liste.
  bool& show_filter()  { return show_filter_; }
  // « refine_desc_tooltip » : aperçu de la description au survol.
  bool& desc_tooltip() { return desc_tooltip_; }
  // « refine_history » : journal de session sous la liste. OPT-IN.
  bool& show_history() { return show_history_; }
  // « refine_log_time » : horodatage des lignes du journal.
  bool& log_time()     { return log_time_; }

  // « refine_pos_x » / « refine_pos_y » : position ÉCRAN de la fenêtre, retenue
  // d'une session à l'autre.
  //
  // INT_MIN = jamais posée — on se cale alors sur la fenêtre native, comme au
  // premier lancement. C'est la convention déjà en place pour game_option_pos_*,
  // et elle vaut mieux qu'un -1 : une fenêtre tirée à cheval sur le bord gauche
  // a une abscisse négative parfaitement légitime.
  //
  // Persistées en entiers : un pixel est un entier, et le yaml reste lisible.
  int& pos_x() { return pos_x_; }
  int& pos_y() { return pos_y_; }
  // « refine_auto_recast » : relancer seul la COMPÉTENCE après chaque tentative,
  // pour rouvrir la liste sans repasser par la barre de skills. OPT-IN.
  //
  // ⚠ Ça ne raffine RIEN tout seul : ça ne fait que redemander la liste au
  // serveur. Le choix de l'arme et le déclenchement restent des clics. La
  // distinction est le cœur du garde-fou — cf. le pavé dans le .cc.
  bool& auto_recast()  { return auto_recast_; }

 private:
  // Une entrée de ZC_NOTIFY_WEAPONITEMLIST, telle qu'elle arrive sur le fil.
  struct Entry {
    int      index  = 0;   // u16 du paquet, DÉJÀ i+2 (à renvoyer tel quel)
    uint32_t nameid = 0;
    int      refine = 0;   // que le natif jette
    uint32_t card[4] = {0, 0, 0, 0};  // que le natif jette aussi
  };

  // Une ligne du journal de session.
  struct LogLine {
    std::string text;
    uint32_t    color = 0;  // IM_COL32
    // Horodatée à l'INSERTION, et toujours — l'affichage seul est optionnel.
    // Ne la composer que si l'option est active priverait de leur heure toutes
    // les lignes déjà écrites au moment où on l'allume.
    char        time[9] = {0};  // « HH:MM:SS »
  };

  // Actions différées vers FlushPending (jamais exécutées pendant une frame).
  enum PendingAction { kActNone, kActRefine, kActCancel, kActRecast };

  void ResetSession();          // vide le modèle (fermeture, warp, changement de perso)
  void DrawList(float list_h);  // le tableau des armes
  void DrawOreLinks();          // les 3 minerais en liens d'item cliquables
  void DrawFooter();            // minerais + plafond + boutons
  void DrawHistory(float h);    // journal de session
  void DrawHoverTooltip();      // aperçu au survol — HORS de la fenêtre (cf. .cc)
  // Ferme NOTRE fenêtre, en désarmant le menuskill serveur si besoin.
  void RequestClose();

  // LE point d'entrée de « le joueur demande un refine sur cet index ». Bouton,
  // double-clic et Entrée y passent tous : chacun avait sa copie des conditions,
  // et seul le bouton portait la garde anti-rafale — d'où une touche Entrée
  // maintenue qui expédiait deux tentatives pour une seule liste, la seconde
  // partant dans le vide (le serveur a déjà effacé son menuskill et ne répond
  // alors RIEN). Ouvre la confirmation ou pose l'action, jamais les deux.
  void RequestRefine(int inventory_index);

  // Nombre d'exemplaires du minerai `nameid` en inventaire (somme des piles).
  int OreCount(uint32_t nameid) const;
  // Niveau APPRIS de WS_WEAPONREFINE (0 si introuvable) = plafond de refine.
  static int RefineSkillLevel();

  // Arme (ou refuse) une relance automatique de la compétence après un résultat.
  // N'envoie rien : c'est FlushPending qui joue le chemin natif.
  void ScheduleAutoRecast(int result);

  void PushLog(const char* text, uint32_t color);
  // Journalise le résultat d'un ZC_ACK_WEAPONREFINE avec le libellé EXACT du
  // client (MsgStringTable 911..914), pas une paraphrase.
  void LogServerResult(int result, uint32_t nameid);

  // ── L'arme VISÉE par la tentative en cours ────────────────────────────────
  // Le paquet de résultat ne porte qu'un `itemId`, pas l'index. Résoudre le nom
  // par cet id retombait sur le PREMIER objet de ce type en inventaire : avec
  // quatre Knife, le journal citait toujours le même, jamais celui joué.
  //
  // On retient donc l'index envoyé, et le nom composé JUSTE AVANT l'envoi — le
  // seul instant où l'arme est encore sûre d'exister, un échec la détruisant.
  // Au moment de journaliser : l'index d'abord (état à jour, refine incrémenté),
  // ce nom-là en repli quand l'objet n'est plus là.
  int  sent_index_ = -1;
  char sent_name_[128] = {0};

  // ── Modèle (bâti par OnRecvPacket, lu par OnRenderUI) ──
  std::vector<Entry>   entries_;
  std::vector<LogLine> history_;

  bool open_      = false;  // la fenêtre native 111 est ouverte ce frame ?
  bool was_open_  = false;  // front montant (placement + premier plan)
  bool need_focus_ = false;

  // ⚠ NOTRE fenêtre ne suit PAS la présence de la native, et c'est le point
  // central du plugin. Le natif DÉTRUIT la fenêtre 111 dès la tentative envoyée
  // (OnMsg case 6 enchaîne SendMsg puis SaveRectAndCloseWindow) : se calquer
  // dessus ferait disparaître notre fenêtre à l'instant précis où le joueur a
  // besoin de voir le résultat et de cliquer « Relancer ». Or c'est exactement
  // ce que le natif oblige à faire à la main, et donc l'apport principal ici.
  // `ui_open_` s'ouvre à la réception de 0x0221 et ne se referme que sur
  // demande explicite (bouton, X, sortie du monde de jeu).
  bool ui_open_ = false;
  // Les entrées ont été consommées (tentative partie) : la liste affichée
  // n'existe plus côté serveur, on n'a plus qu'un résultat à montrer.
  bool consumed_ = false;

  // Ligne survolée au frame courant. L'aperçu de description doit être peint
  // HORS de toute fenêtre ImGui (il crée son propre popup), donc APRÈS
  // EndRoWindow : on mémorise ici ce qu'il faut, on peint ensuite.
  bool     hover_valid_ = false;
  uint32_t hover_id_ = 0;
  int      hover_refine_ = 0;
  uint32_t hover_cards_[4] = {0, 0, 0, 0};
  int      hover_card_count_ = 0;
  char     hover_name_[128] = {0};

  int  sel_index_ = -1;     // index INVENTAIRE sélectionné (pas un rang de liste)
  char filter_[64] = {0};

  // 🔴 La sélection est-elle AFFICHÉE ? Écrit par DrawList à chaque frame, à
  // partir des lignes réellement rendues (filtrées + triées).
  //
  // Ce n'est pas une commodité d'affichage, c'est un verrou de sûreté. `entries_`
  // porte TOUTE la liste du serveur, alors que le filtre n'en montre qu'une
  // partie : chercher la sélection dans `entries_` laissait Entrée — et le bouton
  // « Refine » — jouer une arme masquée, invisible à l'écran. Sur une action qui
  // DÉTRUIT l'arme en cas d'échec, c'est le pire défaut possible. Tout ce qui
  // déclenche un refine doit passer par ce drapeau, jamais par sel_index_ seul.
  bool sel_visible_ = false;

  // Le tri n'a plus d'état ici : il vit dans les ImGuiTableSortSpecs de la table,
  // c'est-à-dire dans l'en-tête sur lequel le joueur clique. Sans aucun tri
  // demandé (mode tri-état), l'ordre affiché est celui du serveur.

  // Une tentative est partie, on attend le ZC 0x0223. Sert à griser les boutons
  // et à expliquer l'attente plutôt que de laisser la fenêtre inerte.
  bool     awaiting_result_ = false;
  unsigned awaiting_since_  = 0;

  // Dernier 0x0221 reçu VIDE : on affiche notre propre explication à la place de
  // la modale native escamotée.
  bool empty_list_ = false;

  PendingAction pending_ = kActNone;
  int           pending_index_ = 0;

  // Confirmation en cours (index inventaire visé), pour la modale RO.
  int  confirm_index_ = -1;
  bool confirm_open_  = false;
  // Frame ImGui où la modale a été ouverte : Entrée n'y est acceptée qu'ENSUITE,
  // sinon la frappe qui l'ouvre la valide dans la foulée.
  int  confirm_frame_ = -1;

  // Taille STABILISÉE de la modale, retenue d'une ouverture à l'autre pour la
  // centrer dès sa première frame dessinée. La frame d'apparition ne la connaît
  // pas (auto-dimensionnement en cours) et `Appearing` est déjà faux à la
  // suivante : sans ce report, il n'y a aucun moment où l'on tienne à la fois la
  // vraie taille ET le droit de placer sans salir l'affichage.
  ImVec2 confirm_size_{0.0f, 0.0f};

  // ⚠ La modale ne retient PAS sa position, et c'est délibéré. Un popup ImGui
  // n'est pas une fenêtre persistante : il réapparaît à chaque OpenPopup, et ses
  // chemins de placement automatique reprennent la main dès qu'une demande
  // conditionnelle ne mord pas. Trois tentatives de mémorisation ont échoué
  // là-dessus. Elle est simplement RECENTRÉE à l'écran à chaque ouverture, ce qui
  // est déterministe et suffisant pour un dialogue de confirmation — cf. le pavé
  // dans le .cc.

  // Position persistée (cf. pos_x()/pos_y()) et son drapeau « à écrire ». On ne
  // sauvegarde PAS à chaque frame de glissement : le yaml serait réécrit des
  // dizaines de fois par déplacement. Le vidage se fait à la fermeture.
  int  pos_x_ = INT_MIN;
  int  pos_y_ = INT_MIN;
  bool pos_dirty_ = false;
  // Sauve la position si elle a bougé. Appelée à la fermeture de la fenêtre.
  void FlushWindowPos();

  // C'est NOUS qui avons baissé le flag de visibilité de la native ? Sert à ne
  // le remettre qu'une fois, quand imgui_enabled_ repasse à false.
  bool native_hidden_ = false;
  bool prev_enabled_  = false;

  // Anti-double-envoi (un double-clic enverrait deux tentatives).
  unsigned last_send_tick_ = 0;  // cf. RequestRefine / kMinSendIntervalMs

  // Relance automatique : échéance (GetTickCount, 0 = rien d'armé) et nombre de
  // relances enchaînées depuis la dernière action manuelle.
  //
  // Le compteur ne borne RIEN — il n'y a rien à borner, une relance ne pouvant
  // suivre qu'un refine, lui-même déclenché par un clic. Il sert seulement à
  // l'affichage, et à savoir si une chaîne était en cours quand la liste revient
  // vide (auquel cas c'est la fin de la chaîne, pas une liste vide ordinaire).
  unsigned auto_recast_at_ = 0;
  int      auto_chain_     = 0;
  // Pourquoi la chaîne s'est arrêtée, à afficher tel quel. Vide = pas d'arrêt.
  const char* auto_stop_reason_ = nullptr;

  bool confirm_      = true;
  bool show_cards_   = true;
  bool show_filter_  = true;
  bool desc_tooltip_ = true;
  bool show_history_ = false;
  bool log_time_     = true;
  bool auto_recast_  = false;
};
