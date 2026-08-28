#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "features/plugin.h"
#include "features/status_cell.h"  // les modes de grisage
#include "ragnarok/social.h"
#include "ui/hud_frame.h"

// ── PartyFrames ──────────────────────────────────────────────────────────────
//
// Le HUD de groupe, en GRILLE de tuiles — la forme des « raid frames » (WoW,
// Grid), pas la liste verticale du client.
//
// Ce n'est PAS un portage de `UIMiniPartyWnd` (0x12d) : cette fenêtre native
// empile des barres de 174×52 et laisse détacher chaque membre à la souris. On
// vise autre chose — un bloc dense qu'on lit d'un coup d'œil, où chaque tuile
// DIT l'état de son membre par sa couleur. À ce titre le module vit dans
// `overlays/` et non `windows/` : il ne remplace pas une fenêtre, il occupe une
// place que le client ne couvre pas (même raisonnement que `skill_bar`).
//
// Le HUD natif est simplement MASQUÉ tant que celui-ci est actif — pas détruit :
// il ne porte aucun bouton par défaut, donc aucun piège clavier (contrairement
// aux popups d'invitation), et le laisser vivant rend la bascule instantanée.
//
// SOURCE DES DONNÉES : `rag::social::ReadParty`, la même que la fenêtre
// Amis/Groupe — le manager de session, relu à chaque frame.
//
// 🔴 Les PV ne sont connus que des membres dont l'ACTEUR est chargé (cf.
// docs/party_friend_re.md §3). Une tuile « hors de portée » le DIT, elle
// n'affiche pas une barre vide qui se lirait comme « ce joueur est à zéro ».
//
// ⚠ PAS ENCORE : le clic pour CIBLER, qui est la moitié de l'intérêt d'un raid
// frame. Le chemin natif est piégeux (la cible vit dans `CGameMode+0xF4`, gatée
// par `+0x28`, et écrire `+0xF8` coupe la marche au clic maintenu — cf. la
// mémoire project_target_system_re). Il mérite sa propre passe de RE.

class PartyFrames : public Plugin {
 public:
  PartyFrames();  // enregistre l'opcode de réponse (SP des membres)

  const char* name() const override { return "PartyFrames"; }

  void OnTick() override;      // masque le HUD natif, interroge le SP
  void OnRenderUI() override;  // dessine la grille

  // ── Le SP : il faut le DEMANDER au serveur ────────────────────────────────
  //
  // 🔴 Aucun paquet du protocole ne transporte le SP d'un tiers, et le client ne
  // remplit JAMAIS les champs SP de ses jauges (+0xA8/+0xAC restent à zéro).
  // C'est le trou que le couple custom CZ 0x0F29 -> ZC 0x0F2A comble déjà pour la
  // fenêtre de cible ; sa gate serveur autorise explicitement les membres du MÊME
  // GROUPE, donc rien à ajouter côté Moonlight.
  //
  // ⚠ Même limite de portée que la cible : le serveur répond `status = 1` pour un
  // membre sur une autre carte ou hors d'AREA_SIZE. Ce paquet ne comble donc PAS
  // le trou « hors de portée ».
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // ── Settings PERSISTANTS (bourgeon_settings.yaml, via MoonlightUi) ────────
  bool enabled_ = false;
  bool locked_  = false;

  // Géométrie. La taille du CADRE est CALCULÉE à partir de ces trois-là : c'est
  // la tuile qui commande, pas l'inverse — un raid frame se règle en « telle
  // taille de case », pas en tirant un coin jusqu'à tomber juste.
  int columns_ = 1;    // 1 = liste (proche du natif), 2-3 = grille compacte
  int tile_w_  = 180;  // largeur d'une tuile, en pixels d'interface
  int tile_h_  = 34;   // hauteur d'une tuile
  int gap_     = 2;    // espace entre deux tuiles

  // Contenu d'une tuile.
  bool show_self_     = true;
  bool show_offline_  = true;
  bool show_job_icon_ = true;   // icône de classe du client
  bool show_level_    = false;  // « Lv.N » devant le nom
  // Comment les PV s'écrivent sous le nom. Une grille très dense n'a la place
  // pour aucun chiffre, une grille large peut tout porter — et le pourcentage
  // seul est souvent le plus lisible en combat, où l'on compare des membres
  // entre eux plutôt que de lire des totaux.
  enum HpText { kHpTextNone = 0, kHpTextNumbers, kHpTextPercent, kHpTextBoth };
  int hp_text_mode_ = kHpTextNumbers;

  // Taille du texte des tuiles, en pixels d'interface. Indépendante de la police
  // du reste de l'UI : une grille se lit en périphérie de l'écran, pas au centre.
  int text_px_ = 13;

  // ── Lancer un sort sur la tuile survolée ──────────────────────────────────
  //
  // C'est ce qui fait qu'un raid frame sert à quelque chose : viser un membre
  // sans le chercher à l'écran. Le client demande deux gestes pour une
  // compétence ciblée — la touche arme le mode, puis le clic désigne QUI ; ici,
  // c'est la tuile sous le curseur qui répond à la seconde question.
  //
  // 🔴 On ne prend PAS le clic au jeu : le cadre reste clic-traversant, on se
  // contente de tester le rectangle. Le joueur garde donc son clic pour marcher
  // ou frapper, même curseur sur la grille.
  //
  // Le lancement lui-même reste ENTIÈREMENT celui de QuickCast (les messages
  // d'acteur du clic natif, puis `SendMsg(0x47)` pour désarmer) : animation,
  // barre de cast, cooldowns et paquet restent natifs, et le serveur reste seul
  // juge de la légalité (SP, portée, cible autorisée).
  bool cast_on_tile_ = true;

  // Le GID à viser pour ce mode de ciblage, ou 0 si la grille n'a rien à
  // proposer. Contrat identique à `TargetFrame::SkillTargetGid`.
  uint32_t SkillTargetGid(int targeting_mode) const;

  // ── Le SP des membres, partagé ────────────────────────────────────────────
  //
  // Ce module est le seul à interroger le serveur pour le SP (CZ 0x0F29), et il
  // n'y a aucune raison qu'une deuxième surface refasse les mêmes requêtes : le
  // trafic doublerait pour la même information. La fenêtre Amis/Groupe lit donc
  // CE cache — et déclare son besoin par `RequestSpPolling`, sans quoi le HUD
  // éteint cesserait d'interroger et sa barre de SP resterait vide.
  //
  // Rend false quand le SP est inconnu (membre hors de portée, réponse périmée,
  // ou serveur qui n'a rien voulu dire).
  bool MemberSp(uint32_t gid, int* sp, int* maxsp) const;
  // À appeler à chaque frame par qui affiche du SP. Le drapeau retombe seul :
  // une surface qui cesse d'en demander cesse d'être servie.
  void RequestSpPolling() { sp_wanted_by_other_ = true; }
  // Opt-in, pour que QuickCast sache s'il doit s'autoriser à travailler.
  bool CastsOnTile() const { return enabled_ && cast_on_tile_; }

  // ── Les clics sur une tuile ───────────────────────────────────────────────
  //
  // 🔴 CE RÉGLAGE PREND LE CLIC AU JEU sur la surface de la grille, et c'est
  // inévitable : un cadre ImGui qui reçoit la souris ne la laisse plus passer,
  // molette et clic droit compris. Marcher en cliquant SOUS la grille devient
  // donc impossible — d'où l'opt-in, et le défaut à FAUX. C'est le compromis
  // habituel d'un raid frame : on gagne des gestes sur les membres, on perd une
  // zone de clic sur le monde.
  //
  //   · gauche = CIBLER, comme un clic sur le sprite. Sans effet si le mode
  //     Ciblage du joueur est éteint — c'est lui qui décide qu'une cible existe.
  //   · droit  = le menu de groupe (chuchoter, nommer chef, expulser…), celui
  //     de la fenêtre Amis/Groupe, dont les entrées suivent les mêmes droits.
  bool clickable_ = false;

  // Rejoue le clic mis en attente, HORS de la frame ImGui. Appelée par Bourgeon
  // depuis OnProcessInput, comme FlushPending de la fenêtre : cibler rejoue du
  // code natif, et ouvrir un menu lit le dictionnaire de noms.
  void FlushPending();
  bool show_sp_       = true;   // barre de SP en bas de tuile
  // Infobulle au survol. Elle redonne ce que la tuile porte, mais ENTIER : le
  // texte d'une case est découpé à ses bords, et sur une grille serrée il ne
  // reste parfois que les premières lettres d'un nom.
  bool show_tooltip_  = false;
  int  sp_bar_h_      = 6;      // sa hauteur, en pixels d'interface

  // ── Buffs et debuffs ──────────────────────────────────────────────────────
  //
  // Les icones d'etat du membre, calees a DROITE de la tuile. La source est
  // StatusEffects, qui ecoute le fil : le client recoit ces etats mais ne les
  // garde pas.
  //
  // 🔴 Rien ne s'affiche pour un membre hors de la vue. Le serveur ne diffuse
  // ces paquets qu'en AREA, donc un membre sur une autre carte n'en emet aucun,
  // et une tuile sans icone veut dire « on ne sait pas », jamais « aucun buff ».
  bool show_buffs_    = true;
  int  buff_px_       = 14;  // cote d'une icone, en pixels d'interface
  int  buff_max_      = 8;   // combien au plus, avant de rogner la place du nom
  // Le temps restant sous l'icône, et le grisage de la part écoulée. Mêmes
  // notions que la barre d'états de la cible, réglées à part : une icône de
  // tuile fait la moitié de la sienne, et ce qui s'y lit n'est pas le même.
  //
  // ⚠ La taille du texte SUIT celle de l'icône (la moitié, sans descendre sous
  // 7 px) : un compte à rebours fixe débordait sur la ligne du dessous dès qu'on
  // réduisait les icônes.
  // Sur combien de LIGNES étaler les icônes.
  //
  // Une rangée unique s'allonge jusqu'à manger la place du nom ; en deux lignes
  // le même nombre d'états tient sur moitié moins de largeur. Le compte maximum
  // se répartit entre elles — six icônes sur deux lignes font trois par ligne.
  int  buff_rows_     = 1;
  bool buff_time_  = true;
  int  buff_sweep_ = statuscell::kSweepRadial;

  // ── Couleurs (RGBA 0..1, persistées en hex ARGB) ──────────────────────────
  //
  // 🔴 `col_frame_bg_` existe parce que sans lui la grille flottait SUR le jeu
  // sans rien pour la détacher du décor : tuiles sombres sur fond de carte
  // sombre, on ne distinguait plus rien. Un fond de cadre, même discret, rend le
  // bloc lisible partout.
  float col_frame_bg_[4] = {0.05f, 0.05f, 0.07f, 0.72f};
  float col_tile_bg_[4]  = {0.09f, 0.09f, 0.11f, 0.90f};
  float col_hp_high_[4]  = {0.25f, 0.78f, 0.28f, 1.0f};
  float col_hp_mid_[4]   = {0.84f, 0.75f, 0.24f, 1.0f};
  float col_hp_low_[4]   = {0.81f, 0.26f, 0.22f, 1.0f};
  float col_sp_[4]       = {0.27f, 0.51f, 0.86f, 1.0f};
  float col_text_[4]     = {0.94f, 0.94f, 0.94f, 1.0f};
  float col_me_[4]       = {1.0f,  0.85f, 0.47f, 0.86f};  // liseré de MA tuile

  // 🔴 DEUX absences, et elles ne se ressemblent pas :
  //   · HORS LIGNE — le membre n'est pas connecté. Il n'y a rien à en attendre,
  //     donc la tuile s'efface presque : un gris terne, très atténué.
  //   · HORS DE PORTÉE — il est EN JEU, simplement trop loin pour que le client
  //     connaisse ses PV. Il peut revenir à portée d'un instant à l'autre, et
  //     c'est une information vivante pour qui soigne. Un bleu pâle, lisible.
  // Les confondre dans le même gris faisait passer un allié présent pour un
  // absent.
  float col_offline_[4]  = {0.45f, 0.45f, 0.48f, 1.0f};
  float col_far_[4]      = {0.58f, 0.72f, 0.90f, 1.0f};

  // Seuils de bascule de couleur, en pourcentage de PV.
  int hp_mid_pct_ = 55;
  int hp_low_pct_ = 25;

  // Accesseurs pour la table de réglages (MLUI_FIELD veut une lvalue).
  ro::HudRect& rect() { return rect_; }
  int& columns() { return columns_; }
  int& tile_w()  { return tile_w_; }
  int& tile_h()  { return tile_h_; }
  int& gap()     { return gap_; }
  int& sp_bar_h() { return sp_bar_h_; }
  int& buff_px()  { return buff_px_; }
  int& buff_max() { return buff_max_; }
  int& buff_sweep() { return buff_sweep_; }
  int& buff_rows()  { return buff_rows_; }
  int& hp_text_mode() { return hp_text_mode_; }
  int& text_px()      { return text_px_; }
  int& hp_mid_pct() { return hp_mid_pct_; }
  int& hp_low_pct() { return hp_low_pct_; }

 private:
  // Dessine la rangee d'icones d'etat, de DROITE a gauche depuis `right`.
  // Rend l'abscisse la plus a gauche atteinte : c'est la limite que le texte du
  // nom ne doit pas franchir, sinon il passerait sous les icones.
  float DrawTileEffects(uint32_t gid, float right, float top, float bottom);

  void DrawTile(const rag::social::Entry& member, ImVec2 p0, ImVec2 p1,
                bool is_me);
  void SyncNativeHud();
  void PollVitals();

  // Le SP d'un membre, tel que le serveur l'a renvoyé. Clé = GID.
  // `stamp` sert à oublier une valeur qui n'a pas été rafraîchie : un membre qui
  // s'éloigne cesse de répondre, et afficher son dernier SP connu indéfiniment
  // serait pire que ne rien afficher.
  struct Vitals {
    int      sp    = 0;
    int      maxsp = 0;
    unsigned stamp = 0;  // GetTickCount de la dernière réponse
  };
  std::unordered_map<uint32_t, Vitals> vitals_;
  size_t   poll_cursor_  = 0;  // interrogation en rotation
  unsigned last_poll_ms_ = 0;
  // Une AUTRE surface affiche du SP cette frame (la fenêtre Amis/Groupe). Remis
  // à faux à chaque tick : c'est une demande vivante, pas un réglage.
  bool     sp_wanted_by_other_ = false;

  ro::HudRect rect_{40, 200, 190, 120};
  std::vector<rag::social::Entry> members_;
  // Le membre sous le curseur, relevé au rendu (0 = aucun). C'est lui que la
  // grille propose à QuickCast, et c'est lui qu'on surligne.
  uint32_t hovered_gid_ = 0;
  // La cible COURANTE du jeu, relevée au rendu. Sa tuile porte un liseré, comme
  // le reste de l'interface la montre.
  uint32_t target_gid_ = 0;
  // Clics en attente de rejeu (0 = rien). Un seul de chaque : deux appuis dans
  // la même frame, ça n'existe pas.
  uint32_t pending_target_gid_ = 0;  // clic gauche -> cibler
  uint32_t pending_menu_gid_   = 0;  // clic droit  -> menu de groupe
  // Le membre dont le menu est ouvert, et son nom au moment du clic (la liste
  // peut changer sous nos pieds pendant que le menu est déplié).
  uint32_t menu_gid_ = 0;
  std::string menu_name_;
  // État FIGÉ au clic : un membre peut se déconnecter pendant que le menu est
  // déplié, et les entrées ne doivent pas changer sous le curseur.
  bool menu_offline_ = false;
  bool     open_menu_ = false;

  void DrawMemberMenu();
  // L'infobulle du membre survolé : les mêmes informations que sa tuile, mais
  // sans découpe, plus celles qui n'y tiennent pas (classe, carte).
  void DrawTooltip(const rag::social::Entry& m, bool is_me);
  // Dernier état appliqué au HUD natif : évite de rejouer SetVisible à chaque
  // tick (appel natif) alors que rien n'a changé.
  int  native_hidden_ = -1;
  bool geometry_dirty_ = false;
};
