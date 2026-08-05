#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "features/plugin.h"

// ── NpcDialogWindow ──────────────────────────────────────────────────────────
//
// Ré-implémentation ImGui des INTERACTIONS NPC (dialogue / menu / prompts),
// calquée sur NpcShopWindow (project_npc_dialog_re.md, doc docs/npc_dialog_re.md).
//
// Modèle « capture recv → état → overlay → send » :
//  - CAPTURE (REMPLACEMENT : le handler natif ne tourne PAS) : ZC_SAY_DIALOG 0xB4
//    / WAIT 0xB5 / CLOSE 0xB6 / MENU_LIST 0xB7 / OPEN_EDITDLG 0x142 / EDITDLGSTR
//    0x1D4 / CLEAR 0x8D6 / SAY2 0x972 / WAIT2 0x973. Les cinq fenêtres natives ne
//    naissent donc plus du tout, au lieu de naître puis d'être masquées — une
//    native masquée garde le clavier. On ne fait que bâtir le modèle dans
//    OnRecvPacket (jamais de send/opération fenêtre depuis le recv), plus les deux
//    écritures de CGameMode que faisait le handler remplacé (SetDialogActiveNative).
//    Le régime est révocable : le prédicat vaut imgui_enabled_, relu à chaque
//    paquet, donc l'interrupteur rend la main au dialogue natif entier.
//  - RENDU : overlay ImGui (texte riche ^RRGGBB + liens ; menu avec recherche +
//    touches 1-9 ; input nombre/texte). Meilleur que le natif sur typo/UX.
//    🔴 Le rendu est PAR PAGE, pas par paquet : une page de dialogue arrive en
//    plusieurs ZC_SAY suivis de son terminateur (WAIT/CLOSE/MENU/prompt), et elle
//    n'est publiée qu'à ce terminateur — texte, menu et boutons d'un seul tenant, à
//    leur taille définitive, défilement en HAUT. Cf. `pending_lines_`/CommitPage.
//  - SEND (thread principal) : requêtes brutes CZ_REQ_NEXT_SCRIPT 0xB9 /
//    CZ_CHOOSE_MENU 0xB8 / CZ_INPUT_EDITDLG 0x143 / CZ_INPUT_EDITDLGSTR 0x1D5.
//    FERMETURE via CMode::SendMsg cmd 0x28 (débloque l'état dialogue CLIENT ;
//    CZ_CLOSE_DIALOG seul laisse le perso bloqué — cf. NpcShopWindow::CloseNativeShop).
//
// OPT-IN : imgui_enabled_ = false par défaut ; quand ON, on cache les fenêtres
// natives (0x10/0x11/0x38/0x64/0xE2, flag wnd+0x28) et on rend notre overlay.
//
// P2 (non couvert ici) : icônes ^i / émotes ^e inline (texture atlas), gras/
// italique rendus (font atlas), wiring clic liens item/navi/quest, dialogue
// secondaire 0xE2 (SAY2/WAIT2 capturés mais non distingués), quest/monolog
// dialog, honorer ALIGN/SIZE/POS. Validation mots-interdits de l'input texte
// (le serveur re-valide de toute façon).

class NpcDialogWindow : public Plugin {
 public:
  NpcDialogWindow();

  const char* name() const override { return "NpcDialogWindow"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Settings persistants (la SÉRIALISATION reste chez MoonlightUi).
  bool imgui_enabled_ = false;  // OPT-IN : dialogue natif par défaut.
  bool menu_search_ = true;     // barre de recherche au-dessus des longs menus (>8 choix)

  // Contenu de la section « Fenêtre NPC » du panneau Moonlight. Rend true si un
  // réglage a changé — c'est l'appelant qui décide de sauvegarder, une seule fois.
  //
  // Ces widgets vivaient dans panel_interface.cc, à sept cents lignes du code
  // qu'ils pilotent : quelqu'un qui travaille sur NpcDialogWindow ne les y
  // trouvait pas. Même patron que SkillBar::DrawSettings et les quatre
  // autres plugins qui délèguent déjà.
  bool DrawSettings();

  bool& show_panel() { return show_panel_; }

  // Appelé par SendPacketHook : vrai si l'overlay ImGui est actif ET l'opcode est
  // un CZ de dialogue NPC (next/menu/nombre/texte/close). On JETTE alors l'envoi,
  // car il vient forcément de la fenêtre NATIVE résiduelle (nos propres CZ passent
  // par SendPacketRef et contournent ce hook). Neutralise « got 1, valid [1..0] ».
  bool ShouldSuppressNativeDialogSend(uint16_t opcode) const;

  // Appelé par le hook WndProc (ragnarok_client) : vrai si cette touche pilote le
  // dialogue ImGui et doit être avalée avant le jeu (Entrée/Espace/Échap, plus
  // flèches et 1-9 quand un menu est affiché). Volontairement CIBLÉ : F1-F9 et le
  // reste du clavier passent au jeu -> skillbar/hotkeys utilisables pendant un
  // dialogue, comme en natif. (msg, wparam) = paramètres bruts du WndProc.
  static bool EatsKey(unsigned msg, unsigned long wparam);

 private:
  enum InputMode { kInputNone, kInputNumber, kInputString };

  // Segment de texte riche (résultat du parseur d'une ligne). `text` est DÉJÀ en
  // UTF-8 : les scripts serveur sont en ANSI, et convertir au rendu refaisait deux
  // appels Win32 par segment et par frame.
  struct Run {
    std::string text;
    uint32_t    color;      // IM_COL32 (0 = couleur de texte par défaut)
    bool        bold = false;
    bool        italic = false;
    int         link = 0;    // 0=aucun ; sinon cmd de lien (URL/ITEM/NAVI/QUEST)
    std::string link_arg;    // URL brute, ou id d'item (<INFO>) pour ouvrir la desc
    int         icon_id = 0; // ^i[id] : icône d'item inline (0 = pas une icône)
  };

  // Une option de menu, préparée à la RÉCEPTION une fois pour toutes : texte riche,
  // clé de recherche, et RANG (1-based parmi les options NON VIDES — c'est ce que le
  // serveur attend, cf. HandlePacket). Le rendu ne re-parse donc plus trente options
  // par frame, et surtout le nombre d'options AFFICHÉES devient connaissable AVANT
  // de dessiner quoi que ce soit : la hauteur du menu se calcule avant celle du corps.
  struct MenuOption {
    std::vector<Run> runs;
    std::string      search;  // texte brut, minuscules ASCII (filtre de recherche)
    int              rank = 0;
  };

  // Un fragment de texte DÉJÀ PLACÉ par le word-wrap : un mot, ou une icône inline.
  // Coordonnées LOCALES au corps (origine = coin haut gauche du contenu), pour que
  // le placement survive au défilement et n'ait à être recalculé qu'au changement
  // de page ou de largeur.
  struct Frag {
    float       x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    float       ux0 = 0.0f;   // départ du soulignement d'un lien (espaces de tête compris)
    std::string text;         // UTF-8 ; vide si c'est une icône
    std::string link_arg;     // argument du lien (id d'item ou URL)
    uint32_t    color = 0;
    int         link = 0;
    int         icon_id = 0;
    bool        bold = false;
  };

  void Reset();                       // vide le modèle (fermeture/warp)
  void PushText(const char* s);       // ajoute une ligne à la page en RÉCEPTION
  // Publie la page reçue : corps, menu, prompt et boutons apparaissent dans la MÊME
  // frame. Appelée par le paquet qui TERMINE la page (WAIT/CLOSE/MENU/prompt), et en
  // dernier recours par le filet à frames muettes d'OnRenderUI.
  void CommitPage();
  static void ParseLine(const std::string& raw, std::vector<Run>* out);
  // (Re)calcule le placement de TOUTES les lignes pour une largeur donnée.
  void BuildLayout(float wrap, float font_size, float line_h);
  void DrawRichLines();               // rendu word-wrap multi-couleur (ImDrawList)
  void DrawMenu(float group_h);       // liste de choix (hauteur bornée) : recherche + touches 1-9
  // Options qui passent le filtre de recherche COURANT. Recalculé, et non repris de
  // la frame précédente : la boîte doit avoir sa taille définitive dès la frame où
  // le joueur tape, sinon elle se redimensionne avec un temps de retard.
  size_t MenuVisibleCount() const;
  // Hauteur que le groupe menu VOUDRAIT occuper : ses options, plafonnées à dix
  // lignes (au-delà la liste scrolle) plus la barre de recherche si elle est là.
  // C'est OnRenderUI qui la rabote ensuite pour garder de la place au texte du NPC.
  float MenuNaturalHeight() const;
  void DrawInput();                   // prompt nombre / texte

  // Envois (thread principal uniquement).
  void SendNext();
  void SendMenuChoice(int one_based);
  void SendMenuCancel();
  void SendNumber(int value);
  void SendString(const char* text);
  void CloseDialog();                 // cmd 0x28 (débloque client) + détruit natif
  void OpenItemDescById(uint32_t id); // clic sur un lien <ITEM> -> fenêtre desc 0xc

  bool DialogActiveNative() const;    // lit CGameMode+0x24C (flag dialogue actif)
  // L'une des cinq fenêtres natives est-elle à l'écran ? Complète le flag ci-dessus
  // pour reconnaître un dialogue natif en cours au moment où l'on allume
  // l'interface moderne — le flag, lui, n'est pas armé par tous les chemins.
  bool AnyNativeDialogWindow() const;
  // Détruit les fenêtres natives 0x10/0x11/0x38/0x64/0xE2 si l'une traîne. Elles ne
  // naissent plus (leurs handlers de paquet sont remplacés) : il n'en reste que
  // lorsqu'on allume l'interface moderne en plein dialogue natif. On les DÉTRUIT au
  // lieu de les masquer — masquée, une native garde le clavier.
  void PurgeNativeDialogWindows();

  // 🔴 Le décodage des paquets, sur le FIL PRINCIPAL. OnRecvPacket (fil réseau) ne
  // fait que copier les octets : `lines_`, `menu_opts_` et le cache de noms étaient
  // reconstruits pendant que le rendu les parcourait. Cf. features/net_inbox.h.
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // ── Modèle (bâti par HandlePacket, lu par OnRenderUI) ──
  std::vector<std::string> lines_;      // page AFFICHÉE (texte NPC, ANSI brut)
  std::vector<MenuOption>  menu_opts_;  // menu courant (vide = pas de menu)

  // ── Page en cours de RÉCEPTION ─────────────────────────────────────────────
  // 🔴 Le serveur envoie une page en PLUSIEURS paquets : un ZC_SAY par `mes`, puis
  // celui qui la TERMINE (WAIT `next` / CLOSE / MENU / prompt). Les afficher au fil
  // de l'eau faisait grandir le corps sur toute la hauteur de la fenêtre, puis le
  // RABOTER quand le menu arrivait — un réarrangement d'autant plus tardif et
  // visible que la page est bavarde (une liste de deux cents monstres, c'est deux
  // cents paquets avant le menu). On met donc la page en attente et on la publie
  // ENTIÈRE d'un coup : la hauteur du menu est connue AVANT de dimensionner le
  // corps. C'est aussi ce que fait le natif, dont la liste de choix est une fenêtre
  // SÉPARÉE — elle ne rabote jamais le texte, d'où son effet « sans couture ».
  std::vector<std::string> pending_lines_;
  bool pending_replaces_ = false;   // la page en attente REMPLACE l'affichée (saut de page)
  bool scroll_top_ = false;         // remonter le corps en haut (nouvelle page publiée)
  // Horodatage du dernier paquet de dialogue reçu (GetTickCount). Sert au SEUL filet
  // de la mise en attente : une page dont le terminateur n'arrive jamais.
  //
  // 🔴 Ce filet se mesure en SILENCE, pas en durée totale, et il doit être LARGE.
  // Première version : « deux frames sans paquet » — beaucoup trop court. L'agent de
  // warp envoie ~700 `mes` d'affilée (trois cartes de donjon, monstre par monstre,
  // moon/warp_agent.npc:794) ; une quarantaine de kilo-octets, que TCP livre en
  // plusieurs rafales avec des trous d'une ou deux frames. Le filet se déclenchait
  // donc EN PLEIN milieu de la page : elle s'affichait à moitié, sur toute la
  // hauteur, puis le menu arrivait et raboterait le texte — le réarrangement était
  // toujours là. Un transfert lent mais RÉGULIER ne le déclenche plus (chaque paquet
  // repousse l'échéance) ; seul un vrai silence le fait, c'est-à-dire un script qui
  // écrit puis dort, ou qui s'arrête sur `end`.
  unsigned long last_dialog_ms_ = 0;

  // ── Cache de mise en page du corps ─────────────────────────────────────────
  // Le word-wrap (parsing des balises, conversion ANSI->UTF-8, mesure de chaque mot)
  // coûte proportionnellement au nombre de lignes, et il était refait à CHAQUE
  // frame : une page de deux cents lignes payait donc son propre découpage soixante
  // fois par seconde, pendant qu'elle arrivait, ce qui la faisait apparaître plus
  // lentement que chez le natif (dont UIRichTextCtrl::AddLine ne place la ligne
  // qu'une fois). Ici : calculé UNE fois par page et par largeur, et le rendu ne
  // dessine que les fragments réellement visibles.
  std::vector<Frag> frags_;
  float    layout_wrap_ = -1.0f;    // largeur pour laquelle frags_ vaut
  float    layout_font_ = -1.0f;    // taille de police idem
  float    layout_h_    = 0.0f;     // hauteur totale du contenu
  unsigned page_gen_    = 0;        // génération de la page (incrémentée à chaque publication)
  unsigned frags_gen_   = 0xFFFFFFFFu;

  bool       has_next_  = false;      // bouton [Next] demandé (WAIT)
  bool       has_close_ = false;      // bouton [Close] demandé (CLOSE)
  bool       start_fresh_ = true;     // prochain SAY = nouvelle conversation (vide le texte)
  InputMode  input_mode_ = kInputNone;
  bool       input_need_focus_ = false;  // focus clavier du champ à sa 1re frame
  uint32_t   gid_ = 0;                 // GID du NPC en cours

  std::unordered_map<uint32_t, std::string> npc_names_;  // GID -> nom (titre)

  // UI transitoire.
  char  num_buf_[16]  = {0};
  char  str_buf_[128] = {0};
  char  menu_filter_[64] = {0};
  int         pending_link_cmd_ = 0;  // type de lien cliqué (0x1D0 item / 0x1B5 url ; 0=aucun)
  std::string pending_link_arg_;      // argument du lien (id d'item ou url), traité au prochain OnTick
  int   menu_hot_ = -1;               // choix focus clavier (index VISIBLE ; -1 = auto-focus #1)
  unsigned menu_gen_ = 0;             // génération du menu (incr. à chaque ZC_MENU_LIST)
  unsigned menu_answered_gen_ = 0xFFFFFFFFu;  // génération déjà répondue (anti double-envoi)

  // Un envoi est parti, la réponse du serveur n'est pas encore là (un aller-retour
  // complet = plusieurs frames). On GARDE alors la page telle quelle — bouton
  // Suivant, liste de choix, champ de saisie — simplement DÉSACTIVÉE, au lieu de
  // l'effacer tout de suite. L'effacer faisait sauter le footer : le bouton
  // principal disparaissait, le bouton de rapport de bug glissait à sa place et son
  // infobulle s'ouvrait sous le curseur, pour quelques frames. Remis à false par le
  // premier paquet de dialogue qui suit (cf. HandlePacket).
  bool  awaiting_reply_ = false;

  bool  open_ = false;                // interaction NPC active ?
  bool  was_open_ = false;
  // La fenêtre a-t-elle déjà été rendue au moins une frame dans CETTE conversation ?
  // Tant que non, un modèle vide = rien à montrer (on ne monte pas de cadre vide) ;
  // une fois montée, un modèle vide est un simple creux entre deux paquets et la
  // fenêtre RESTE — sinon elle clignote. Remis à false par Reset().
  bool  rendered_ = false;
  bool  need_pos_ = false;            // (re)placer la fenêtre à l'ouverture
  bool  show_panel_ = true;
  bool  map_changed_ = false;         // warp reçu (fermer au tick)
  bool  pending_reset_ = false;       // reset modèle au prochain tick (thread sûr)
  bool  prev_imgui_enabled_ = false;  // détection changement du toggle -> close propre
};
