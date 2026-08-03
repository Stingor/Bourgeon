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

  // ── Une AUTRE session de fabrication vient de s'ouvrir ────────────────────
  //
  // 🔴 Le serveur ne garde QU'UN `menuskill` par personnage. Une liste de
  // fabrication (Mini Furnace, marteau, kit…) écrase donc `menuskill_id` et détruit
  // la session de refine **en silence** : la fenêtre resterait à l'écran avec sa
  // liste et son bouton actifs, sur une session qui n'existe plus. Le joueur
  // cliquerait « Refine » dans le vide.
  //
  // 🔴 Et on ENVOIE le `182 / -1`, contrairement à ce qu'une première rédaction
  // supposait. « La fabrication a écrasé notre menuskill » n'est vrai que si SA liste
  // est pleine (le serveur n'arme que `if (count > 0)`). Et elle arrive vide
  // précisément dans ce cas de figure, par un enchaînement circulaire :
  // `skill_can_produce_mix` écarte toute recette dont le `req_skill` ne correspond
  // pas à un `menuskill_id` positif déjà en place. Notre refine encore armé vidait
  // donc la liste, qui n'armait rien, qui ne nous désarmait pas — blocage définitif
  // des deux compétences. ⏱ Constaté en jeu, deux fois.
  //
  // ⚠ Personne d'autre ne peut le faire : l'annulation de la fabrication (CZ 0x018E,
  // itemId 0) sort par le `default:` de `clif_parse_ProduceMix` sans rien effacer.
  // Seul `clif_parse_WeaponRefine` efface un `WS_WEAPONREFINE`.
  //
  // Appelée par MakeItemWindow à l'arrivée de sa liste. L'inverse n'existe pas : le
  // serveur refuse déjà de lancer le refine pendant une session de fabrication.
  void CloseForOtherCraft();

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

  // « refine_auto_refine » : la chaîne COMPLÈTE, sans le moindre clic — le refine
  // de la première arme de la liste part tout seul, puis la compétence est
  // relancée, et ainsi de suite tant que le personnage a le SP de la relancer.
  // OPT-IN, défaut OFF, et le seul réglage du plugin qui AGISSE à la place du
  // joueur.
  //
  // 🔴 Chaque tentative peut DÉTRUIRE l'arme, et celle-ci part sans confirmation :
  // c'est bien une automatisation, pas un raccourci d'affichage. Elle implique la
  // relance de compétence (sans elle, la liste ne revient jamais) et s'arrête
  // d'elle-même sur la première borne atteinte — cf. AutoStopCause dans le .cc.
  bool& auto_refine()  { return auto_refine_; }
  bool& enter_key()    { return enter_key_; }

 private:
  // 🔴 Le décodage, sur le FIL PRINCIPAL. OnRecvPacket (fil réseau) ne fait que
  // copier : `entries_` était vidée puis re-remplie pendant que le rendu la
  // parcourait — sur une fenêtre où un clic détruit une arme.
  // Cf. features/net_inbox.h.
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

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
  // `sp_cost` (optionnel) reçoit le coût SP au niveau courant, que le SERVEUR
  // envoie avec la fiche de compétence (CSkillInfo+0x14) — donc jamais une
  // constante à nous. 0 = inconnu, et l'appelant doit alors se taire.
  static int RefineSkillLevel(int* sp_cost = nullptr);

  // Arme (ou refuse) une relance automatique de la compétence après un résultat.
  // N'envoie rien : c'est FlushPending qui joue le chemin natif.
  void ScheduleAutoRecast(int result);

  // Arme le refine AUTOMATIQUE de la liste qui vient d'arriver. N'envoie rien non
  // plus : pose une échéance que OnTick transforme en action.
  void ScheduleAutoRefine();

  // Une chaîne automatique est-elle demandée ? Le refine automatique IMPLIQUE la
  // relance de compétence : sans elle le serveur n'enverrait plus jamais de liste
  // et la chaîne s'arrêterait au premier tour. Un seul endroit pour cette règle,
  // sinon elle se perd dans les cinq tests qui la consultent.
  bool AutoChain() const { return auto_recast_ || auto_refine_; }

  // La chaîne peut-elle continuer ? Rend le motif d'arrêt (à afficher tel quel) ou
  // nullptr. Consultée à l'armement ET juste avant l'envoi : le SP, lui, peut
  // avoir fondu entre les deux.
  const char* AutoStopCause() const;

  // Ré-arme une relance dont le LANCEMENT n'a rien donné — la compétence a été jetée
  // par le délai de cast, ou rien n'est revenu. Distinct de ScheduleAutoRecast, qui
  // traite le RÉSULTAT d'une tentative ; ici il n'y a jamais eu de tentative.
  void RetryRecast();

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
  //
  // Le refine d'AVANT est retenu lui aussi : sans lui, une réussite ne pouvait
  // qu'énoncer un état, jamais un passage. « Refined weapon: +4 Knife » se lit
  // « on a raté, elle est restée à +4 » alors que l'arme est à +5 — l'ambiguïté
  // venait de là. Il faut les DEUX bornes, et le paquet n'en porte aucune.
  int  sent_index_ = -1;
  char sent_name_[128] = {0};
  int  sent_refine_ = -1;   // -1 = inconnu (ne rien affirmer plutôt que « +0 »)

  // ── Modèle (bâti par OnRecvPacket, lu par OnRenderUI) ──
  std::vector<Entry>   entries_;
  std::vector<LogLine> history_;

  // Dernier résultat de tentative, affiché en pied jusqu'à la liste suivante.
  // Il n'existait que dans `history_`, panneau OPT-IN et replié par défaut :
  // sur une chaîne de relances, « ÉCHEC — arme détruite » était donc la seule
  // information vraiment importante… et la seule qui ne s'affichait pas.
  std::string last_result_;
  uint32_t    last_result_color_ = 0;

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

  // ── « Le SERVEUR attend-il encore une réponse ? » ─────────────────────────
  //
  // 🔴 Un seul drapeau pour cette question, et il ne parle QUE du serveur — pas de
  // notre fenêtre, pas de la fenêtre native, pas de ce qui est affiché. C'est lui,
  // et lui seul, qui décide si la fermeture doit envoyer le `182 / -1` qui désarme
  // le `menuskill`.
  //
  // Trois bugs sont venus d'avoir répondu à cette question autrement :
  //   1. par la présence de la fenêtre NATIVE (`RefineWnd()`) — cassé net dès
  //      qu'on a empêché cette fenêtre de naître : plus de -1, personnage bloqué ;
  //   2. par `entries_.empty()` — un parseur qui échoue rend un vecteur vide sur un
  //      paquet peuplé, et prétend donc que rien n'est armé ;
  //   3. par rien du tout à la bascule de l'interrupteur « Interface moderne », qui
  //      jetait la session sans prévenir le serveur.
  //
  // Posé sur le COMPTE du paquet (`count > 0`, exactement le critère de
  // `clif_upgrade_list`), retiré dès qu'une tentative part (le serveur fait alors
  // son propre `clif_menuskill_clear`) ou qu'une annulation est envoyée.
  //
  // ⚠ NON remis à zéro par ResetSession : celui-ci referme NOTRE interface, ce qui
  // ne dit rien de l'état du serveur. C'est précisément la confusion qui a produit
  // le bug n°3.
  bool session_armed_ = false;

  // ── Une relance de compétence est-elle EN VOL ? ────────────────────────────
  // Tick du dernier CZ_USE_SKILL de relance, 0 = aucune en vol. Remis à zéro dès
  // qu'une liste revient : c'est la preuve que la compétence est bien partie.
  //
  // 🔴 Sans ce suivi, une relance jetée par le serveur (délai de cast, `canact_tick`)
  // arrêtait la chaîne SANS UN MOT : `auto_recast_at_` déjà consommé, `pending_`
  // revenu à `kActNone`, `consumed_` toujours vrai — fenêtre grisée sur « Session
  // terminée » alors que l'arme était intacte et les conditions réunies. ⏱ Constaté
  // en jeu, et seulement avec Entrée MAINTENUE : `IsKeyPressed` répète ~20 fois par
  // seconde, c'est le seul régime assez rapide pour que la relance tombe dans le
  // délai de cast de la tentative précédente.
  unsigned recast_sent_at_ = 0;
  int      recast_retries_ = 0;  // relances consécutives sans liste en retour

  // ── Natif -> moderne avec la native DÉJÀ ouverte : AUCUN drapeau ───────────
  //
  // Cette fenêtre tient une session que le JOUEUR a lancée et que nous n'avons
  // jamais vue (le paquet est passé quand nous étions coupés) : nous ne pouvons donc
  // pas l'afficher, faute de données. On PILOTE son bouton Annuler — `OnMsg(6, 185)`
  // — le seul chemin qui désarme sans que nous ayons à connaître le protocole de
  // réponse : la fenêtre, elle, le connaît, elle envoie son paquet puis se détruit.
  //
  // 🔴 Et cela se fait SANS ÉTAT, à chaque `FlushPending`. Un drapeau posé depuis
  // `OnTick` — limité à 100 ms — est une course perdue d'avance face à un
  // FlushPending par frame : c'est ce qui a cassé la fabrication (la fenêtre était
  // escamotée avant que le tick ne l'ait vue vivante). Il n'y a rien à retenir : une
  // native vivante pendant que nous sommes actifs est forcément une session qui n'est
  // pas la nôtre, puisque le remplacement du 0x0221 empêche toute naissance sous
  // notre garde.

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

  // ── Ce que le refine AUTOMATIQUE a le droit de viser ──────────────────────
  //
  // Index de la PREMIÈRE ligne rendue (filtre et tri appliqués), -1 si aucune.
  // Écrit par DrawList, remis à -1 à chaque nouvelle liste — et c'est ce dernier
  // point qui en fait un garde-fou plutôt qu'un cache : tant que la liste n'a pas
  // été DESSINÉE au moins une fois, la chaîne automatique n'a aucune cible et
  // attend. On ne détruit jamais une arme qui n'est pas passée sous les yeux du
  // joueur.
  int  first_visible_index_ = -1;
  // La liste courante a-t-elle été rendue depuis son arrivée ? Distingue « pas
  // encore dessinée » (on attend) de « dessinée, mais le filtre ne laisse rien
  // voir » (on arrête, en le disant).
  bool list_drawn_ = false;

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

  // (Plus de `native_hidden_` : on ne masque plus jamais la native, donc il n'y a
  // plus de visibilité à lui restituer. Elle est ANNULÉE, pas escamotée.)
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

  // Échéance du refine automatique (GetTickCount, 0 = rien d'armé) et nombre de
  // tentatives que la chaîne a déclenchées seule — celui-là compte les ARMES
  // jouées, là où auto_chain_ compte les relances de compétence.
  unsigned auto_refine_at_    = 0;
  int      auto_refine_count_ = 0;

  // ── Le joueur a demandé l'ARRÊT ────────────────────────────────────────────
  // Un bouton d'arrêt qui se contenterait de désarmer l'échéance ne servirait à
  // rien : la liste suivante réarmerait la chaîne dans la demi-seconde. Ce drapeau
  // la tient en respect jusqu'à un geste MANUEL (bouton « Refine », « Relancer »)
  // ou une nouvelle session — sans toucher au réglage persistant, qui appartient
  // au panneau d'options et pas à un bouton de fenêtre.
  bool auto_paused_ = false;

  bool confirm_      = true;
  bool show_cards_   = true;
  bool show_filter_  = true;
  bool desc_tooltip_ = true;
  bool show_history_ = false;
  bool log_time_     = true;
  bool auto_recast_  = false;
  bool auto_refine_  = false;
  // Entrée déclenche-t-elle le refine ? Décoché (défaut) = la touche n'est PAS
  // confisquée et le chat reste accessible pendant qu'on refine. ⚠ La modale de
  // confirmation, elle, garde la touche quoi qu'il arrive (cf. WantsEnterKey) :
  // « Entrée = OK » y est la convention, et elle valide une action destructrice.
  bool enter_key_    = false;
};
