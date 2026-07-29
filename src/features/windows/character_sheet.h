#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "features/plugin.h"

// Un item d'un preset d'equipement : identite suffisante pour le RETROUVER en
// inventaire lors de l'application (nameid + refine + cartes + grade). left_hand =
// arme equipee en main gauche (dual-wield, slot bouclier).
struct EquipPresetItem {
  uint32_t nameid = 0;
  int      refine = 0;
  uint32_t cards[4] = {0, 0, 0, 0};
  int      grade = 0;
  bool     left_hand = false;
};
// Un preset = un jeu d'equipement nomme, propre a UN personnage (cid = g_Own_CharId).
struct EquipPreset {
  uint32_t                     cid = 0;
  std::string                  name;
  std::vector<EquipPresetItem> items;
  // Raccourci clavier optionnel (VK Windows + modificateurs). hotkey_vk==0 => aucun.
  int  hotkey_vk = 0;
  bool hotkey_ctrl = false, hotkey_alt = false, hotkey_shift = false;
};

// Feuille de personnage facon WoW : un avatar central entoure des slots
// d'equipement (+ costume), avec un volet stats a droite. AGREGE les fenetres
// natives Status (id 0xb) + Equipement (id 0xa), qui restent CONSERVEES : ceci est
// un COMPLEMENT opt-in (setting charsheet_imgui, defaut OFF), ouvert via Alt+F.
//
// Interactif : survol slot = tooltip (nom+refine), clic-gauche slot = fenetre de
// description native, clic-droit slot = DESEQUIPER (CZ 0x00AB), boutons +STR/+AGI/...
// (CZ 0x00BB, actifs seulement si le cout de montee <= points de statut restants).
// Onglet Equipement / Costume pour les deux jeux de slots.
//
// Donnees lues LIVE des globals session (cf. project_character_sheet). L'avatar
// central reutilise le moteur de capture-sprite de basic_info (Phase 2 ; placeholder
// pour l'instant). Skin RO via ro_imgui.
class CharacterSheet : public Plugin {
 public:
  CharacterSheet();

  const char* name() const override { return "CharacterSheet"; }

  void OnRenderUI() override;
  // Suit l'état de la fenêtre native du grimoire (id 0x25) pour que l'icône « Skill »
  // et Alt+S pilotent l'onglet Grimoire : ouverture -> on bascule dessus, fermeture ->
  // on referme la feuille si c'est bien cet onglet qu'on regardait.
  void OnTick() override;
  // Reçoit ZC_BOURGEON_STAT_BONUS (0x0F10) : apport équip/cartes compilé par
  // status_calc_pc côté serveur, poussé à chaque recalc.
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Setting PERSISTANT (bourgeon_settings.yaml "charsheet_imgui", gere par
  // MoonlightUi). Defaut OFF : opt-in. Quand ON, Alt+F bascule la fenetre.
  // Bascule en GROUPE par SetModernInterface (moonlight_ui.h) — PLUS de case isolee
  // dans le panneau : la feuille recoit les objets glisses depuis l'inventaire ImGui
  // et son onglet Presets equipe en s'appuyant dessus.
  bool imgui_enabled_ = false;

  // Presets d'equipement (loadouts nommes), persistes par MoonlightUi dans le yaml
  // (noeud "equip_presets"). Tous personnages confondus ; filtres par cid a l'affichage.
  std::vector<EquipPreset>& equip_presets() { return equip_presets_; }

  // Etat ouvert/ferme de la fenetre, persiste par MoonlightUi (yaml "charsheet_open")
  // pour ne pas se rouvrir a chaque connexion si le joueur l'a fermee.
  bool  is_open() const { return show_; }
  void  set_open(bool v) { show_ = v; }
  // Même champ par référence, pour la table de persistance de MoonlightUi : elle
  // décrit chaque réglage par l'ADRESSE de sa valeur, comme pour tous les autres
  // plugins. is_open/set_open restent l'API des appelants ordinaires.
  bool& open() { return show_; }

  // Ouvre la feuille sur l'onglet GRIMOIRE. Appelée quand le joueur demande le
  // grimoire (icône « Skill » ou Alt+S) alors que l'interface moderne est active :
  // la fenêtre native 0x25 est masquée et c'est cet onglet qui la remplace.
  void OpenSkillsTab();
  // Masque la fenêtre native du grimoire (UINewSkillListWnd, id 0x25) DÈS sa
  // création — même schéma que l'inventaire ou l'entrepôt (cf. window_pos_tweaks) :
  // sans ça une frame native passerait à l'écran avant le premier OnRenderUI.
  // No-op quand l'interface moderne est éteinte.
  void HideSkillWndAtCreation(void* win);

  // Pose de l'avatar (pose + direction + animation on/off), persistee par MoonlightUi
  // (yaml "charsheet_pose"/"charsheet_dir"/"charsheet_pose_anim") pour retrouver le
  // meme cadrage a chaque ouverture. Accesseurs par reference (lecture + ecriture).
  int&  avatar_anim()    { return avatar_anim_; }
  int&  avatar_dir()     { return avatar_dir_; }
  bool& avatar_animate() { return avatar_animate_; }

  // Lissage bilinéaire des icônes de l'onglet Grimoire (yaml
  // « charsheet_grimoire_bilinear »). Défaut OFF : le natif ne filtre rien, les
  // icônes 24 px agrandies restent nettes.
  bool& skill_bilinear() { return skill_bilinear_; }

 private:
  // Apport ÉQUIPEMENT + CARTES aux stats, poussé par le serveur (ZC 0x0F10),
  // compilé par status_calc_pc. Le natif ne donne que le TOTAL par stat primaire ;
  // ceci fournit le SPLIT équip/carte + l'ATK/MATK issus de l'équip. Phase 1.
  // Un bonus conditionnel : (catégorie, index élément/race/taille, valeur %).
  // Miroir de PACKET_BOURGEON_STAT_COND ; libellé résolu à l'affichage.
  struct CondBonus {
    uint16_t code = 0;
    int16_t  idx = 0;
    int32_t  value = 0;
  };
  // Un bonus lié à un skill (autocast / +dégâts skill). Nom résolu à l'affichage via
  // GetSkillName. Miroir de PACKET_BOURGEON_STAT_SKILL.
  struct SkillBonus {
    uint16_t code = 0;
    uint16_t skill_id = 0;
    int16_t  lv = 0;
    int32_t  value = 0;
    uint16_t aux = 0;  // skill déclencheur (autospell3) ; 0 sinon
  };
  // Un bonus lié à un item (drop). Nom résolu à l'affichage via le DB item.
  struct ItemBonus {
    uint16_t code = 0;
    uint32_t nameid = 0;
    int32_t  rate = 0;
  };
  struct BonusBreakdown {
    bool valid = false;   // au moins un paquet reçu
    int  equip[6] = {};   // apport ÉQUIPEMENT par stat primaire (STR..LUK)
    int  card[6]  = {};   // apport CARTES
    int  eatk = 0;        // ATK issu de l'équip
    int  ematk = 0;       // MATK issu de l'équip
    int  melee_pct = 0;   // % dégât mêlée non-armé
    int  ranged_pct = 0;  // % dégât à distance
    int  crit_dmg_pct = 0;// % dégât critique
    int  hp_add = 0;      // PV max ajoutés par l'équip
    int  sp_add = 0;      // SP max ajoutés par l'équip
    int  aspd_add = 0;    // ASPD plate
    int  vcast_pct = 0;   // cast variable n/100 (<0 = réduction)
    int  fcast_pct = 0;   // cast fixe (<0 = réduction)
    // Lot A — offensif
    int  atk_pct = 0, matk_pct = 0;
    int  dmg_ret_melee = 0, dmg_ret_ranged = 0, dmg_ret_magic = 0;
    int  double_pct = 0, perfect_hit = 0;
    // Lot B — survie
    int  hp_pct = 0, sp_pct = 0, hp_regen_pct = 0, sp_regen_pct = 0;
    int  crit_def_pct = 0, hp_on_kill = 0, sp_on_kill = 0, unbreak_pct = 0;
    // Lot C — utilitaire
    int  pot_hp_pct = 0, pot_sp_pct = 0, heal_up_pct = 0, delay_pct = 0;
    int  add_vcast_ms = 0, add_fcast_ms = 0, steal_pct = 0;
    // Lot E — réduction par type d'attaque + splash
    int  def_melee_pct = 0, def_ranged_pct = 0, def_magic_pct = 0, def_misc_pct = 0;
    int  splash = 0, splash_add = 0;
    // Lot F — vol de vie
    int  hp_drain_pct = 0, sp_drain_pct = 0;
    // Lot G — très niche
    int  break_weapon_pct = 0, break_armor_pct = 0, zeny_bonus_pct = 0, classchange_pct = 0;
    int  dmg_ret_reduce = 0, magic_hp_gain = 0, magic_sp_gain = 0;
    // Part du refine dans l'ATK / la DEF
    int  refine_atk = 0, refine_def = 0;
    std::vector<CondBonus>  cond;    // conditionnels non nuls (vs race/élément/taille)
    std::vector<SkillBonus> skills;  // bonus liés à un skill (autocast, +dégâts skill)
    std::vector<ItemBonus>  items;   // bonus liés à un item (drop)
  };
  BonusBreakdown bonus_;

  // État des COMPAGNONS (cart / peco / faucon), poussé par le serveur
  // (ZC_BOURGEON_COMPANION_STATE 0x0F16) : niveaux des skills requis + états actifs.
  // La feuille n'affiche/gate les cases QUE d'après ceci — aucune lecture côté client
  // d'IDs de skills ni du bitmask option (ce dernier ne reflète pas le cart sous NEW_CARTS).
  struct CompanionState {
    bool valid = false;         // au moins un paquet reçu
    int  pushcart_lv = 0;       // MC_PUSHCART   (0 = non appris -> pas de case cart)
    int  changecart_lv = 0;     // MC_CHANGECART (0 = non appris -> pas de « changer déco »)
    int  riding_lv = 0;         // KN_RIDING     (0 = non appris -> pas de case peco)
    int  falcon_lv = 0;         // HT_FALCON     (0 = non appris -> pas de case faucon)
    int  cart_active = 0;       // type de cart courant (0 = aucun)
    bool riding_active = false; // sur peco/monture
    bool falcon_active = false; // faucon présent
    int  cart_deco_max = 1;     // type de déco max (cycle) autorisé par le niveau de base
    int  pushcart_id = 0;       // ids AEGIS des skills (envoyés par le serveur) pour l'icône
    int  riding_id = 0;
    int  falcon_id = 0;
  };
  CompanionState companion_;
  int last_cart_type_ = 1;      // dernier type de cart actif (pour « rallumer » au même)

  std::vector<EquipPreset> equip_presets_;
  char preset_name_buf_[24] = {};  // saisie du nom (sauvegarde / renommage)
  std::string preset_status_;      // retour UI (ex. « Applique », « 2 item(s) manquant(s) »)
  int  hk_capturing_ = -1;         // index (dans equip_presets_) du preset en capture de touche
  std::string hk_conflict_msg_;    // message de conflit affiché pendant la capture
  bool show_    = true;   // fenetre visible (bascule Alt+F)
  // onglet actif : 0=Equipement, 1=Costume, 2=Presets, 3=Titres, 4=Guilde, 5=Grimoire
  int  tab_     = 0;
  // Onglet demandé pour la PROCHAINE frame (OpenSkillsTab) : ImGui choisit l'onglet
  // au moment où il le dessine, on ne peut donc pas le forcer depuis un hook.
  int  tab_request_ = -1;

  // ── Onglet Grimoire (arbre de compétences, remplace la fenêtre native 0x25) ──
  int  skill_tab_ = 0;              // onglet de job actif (0..3), 4 = « divers » (liste plate)
  bool skill_grid_ = true;          // grille d'icônes (vue « moderne ») / liste détaillée
  char skill_filter_buf_[32] = {};  // filtre par nom
  uint16_t skill_hover_ = 0;        // compétence survolée à la frame PRÉCÉDENTE (surlignage)
  // Points RÉSERVÉS mais pas encore envoyés : {id, niveau cible}. Comme le natif, on
  // prépare puis on valide en un coup — un clic ne dépense donc jamais tout seul.
  std::vector<std::pair<uint16_t, int>> skill_pending_;
  std::string skill_status_;        // retour UI de la dernière action (Appliquer, refus…)
  bool skill_wnd_was_open_ = false; // état de la fenêtre native 0x25 au tick précédent
  // Lissage des icônes de la grille (OFF = pixels nets, comme le natif qui ne filtre
  // rien). Persisté par MoonlightUi (yaml « charsheet_grimoire_bilinear »).
  bool skill_bilinear_ = false;

  // ── Onglet Guilde (fenêtre de guilde native refaite en ImGui) ──────────────
  int      guild_sub_tab_ = 0;      // 0 = Membres, 1 = Relations
  uint32_t guild_sel_cid_ = 0;      // membre sélectionné dans la table (char id)
  unsigned long guild_last_req_ = 0;  // GetTickCount de la dernière demande (CZ 0x014f)
  bool     guild_notice_edit_ = false;  // édition de l'annonce en cours (maître)
  char     guild_notice_subj_[60] = {};
  char     guild_notice_body_[120] = {};
  char     guild_invite_buf_[24] = {};  // nom du joueur à inviter
  char     guild_reason_buf_[40] = {};  // motif de départ / d'expulsion
  bool     guild_expel_ask_ = false;    // ouverture différée du modal d'expulsion
  uint32_t guild_expel_aid_ = 0, guild_expel_cid_ = 0;
  char     guild_expel_name_[32] = {};
  // Nom de guilde retapé pour confirmer la dissolution : @breakguild ne demande AUCUNE
  // confirmation côté serveur, la seule barrière est celle qu'on met ici.
  char     guild_break_confirm_[32] = {};
  std::string guild_status_;            // retour UI de la dernière action guilde
  // Création de guilde (sans guilde) : nom saisi + dernier code renvoyé par le serveur
  // (ZC 0x0167 ; -1 = pas de réponse en attente).
  char guild_create_buf_[24] = {};
  int  guild_create_result_ = -1;
  // Rupture d'une relation (alliance/hostilité) : cible en attente de confirmation.
  bool guild_rel_del_ask_ = false;      // ouverture différée du modal
  int  guild_rel_del_id_ = 0;           // guild id de l'autre guilde
  int  guild_rel_del_kind_ = 0;         // 0 = allié, 1 = ennemi
  char guild_rel_del_name_[32] = {};
  // Changement d'emblème : les .bmp sont pris dans <jeu>\emblem\, comme le fait la
  // fenêtre native. La liste (avec ses aperçus) vit dans le .cc ; ici l'état d'UI.
  bool guild_emblem_ask_ = false;   // ouverture différée du modal (depuis un bouton)
  int  guild_emblem_sel_ = -1;      // index du fichier choisi dans la liste
  std::string guild_emblem_error_;  // refus local (dimensions, taille, format…)
  // Compte rendu du dernier envoi, affiché DANS le modal : le serveur jette un
  // emblème sans rien journaliser quand l'expéditeur n'est pas maître de guilde,
  // et la console du jeu n'est pas toujours accessible.
  std::string guild_emblem_diag_;
  bool guild_emblem_goto_paint_ = false;  // basculer sur l'onglet « Dessiner » au prochain frame
  // Clic sur l'emblème de l'en-tête : charger l'emblème EN PLACE dans l'éditeur à
  // l'ouverture du modal (le bouton « Changer l'emblème… », lui, garde le dessin en cours).
  bool guild_emblem_load_current_ = false;
  int  guild_emblem_item_id_ = 0;         // item dont l'icône sert de base au dessin
  bool guild_emblem_gallery_ = false;     // galerie des icônes de l'inventaire dépliée

  // Postes de guilde. Le client ne les conserve QUE dans la fenêtre native (liste
  // interne à UIGuildPositionManageWnd) : on lit donc nous-mêmes ZC_POSITION_ID_NAME_INFO
  // (0x0166), ZC_POSITION_INFO (0x0160) et ZC_ACK_CHANGE_GUILD_POSITIONINFO (0x0174).
  static constexpr int kGuildPositionSlots = 20;  // MAX_GUILDPOSITION côté serveur
  struct GuildPositionRow {
    char name[24] = {};
    int  mode = 0;      // droits : 0x001 inviter, 0x010 expulser, 0x100 entrepôt
    int  pay_rate = 0;  // part d'exp (%)
    bool has_name = false;  // un nom est arrivé (0x0166/0x0174)
    bool has_info = false;  // droits + part d'exp sont arrivés (0x0160/0x0174)
  };
  GuildPositionRow guild_positions_[kGuildPositionSlots];       // dernier état serveur
  GuildPositionRow guild_positions_edit_[kGuildPositionSlots];  // copie éditée
  bool guild_positions_editing_ = false;  // saisie en cours : la copie ne se resynchronise plus

  // Compétences de guilde (ZC_GUILD_SKILLINFO 0x0162, type 3 de CZ_REQ_GUILD_MENUINTERFACE).
  // Le paquet ne porte pas le niveau MAX, seulement `upgradable` (1 = maître ET max non
  // atteint) : c'est lui qui active le bouton, pas une table de maxima recopiée ici.
  struct GuildSkillRow {
    uint16_t id = 0;
    int      inf = 0;         // masque skill_get_inf (passif / actif…)
    int      level = 0;
    int      sp = 0;          // coût au niveau courant
    int      range = 0;
    char     name[24] = {};   // nom TECHNIQUE (« GD_APPROVAL ») ; l'affichage passe par Lua
    bool     upgradable = false;
  };
  std::vector<GuildSkillRow> guild_skills_;
  int  guild_skill_points_ = 0;
  bool guild_skills_known_ = false;  // un 0x0162 est arrivé (sinon « en attente »)

  // Arbre des compétences de guilde, lu dans skilltreeguild.lub (miroir client de
  // db/guild_skill_tree.yml, généré par tools/gen/gen_guild_skill_tree_lub.py).
  // Indispensable pour afficher ce que le serveur ne dit PAS : il n'envoie ni le
  // niveau max, ni les compétences dont les prérequis manquent.
  struct GuildSkillReq { uint16_t id = 0; int level = 0; };
  struct GuildSkillTreeNode {
    uint16_t id = 0;
    int      max_level = 0;
    int      depth = 0;      // 0 = racine ; calculé depuis les prérequis, pour l'affichage
    char     name[40] = {};  // libellé lisible : le Lua du client ne nomme pas les GD_,
                             // et une compétence verrouillée n'a aucun paquet à nommer
    std::vector<GuildSkillReq> need;
  };
  std::vector<GuildSkillTreeNode> guild_skill_tree_;
  int guild_skill_tree_state_ = 0;  // 0 = pas encore tenté, 1 = chargé, -1 = indisponible
  // Compétence survolée à la frame PRÉCÉDENTE : quand on apprend qu'une ligne est
  // survolée, ses prérequis sont déjà dessinés au-dessus, trop tard pour les allumer.
  uint16_t guild_skill_hover_ = 0;
  // Vue du sous-onglet Compétences : grille d'icônes (défaut, comme le Grimoire) ou
  // liste détaillée. L'arbre de guilde n'a pas d'index de case côté Lua : la grille
  // est construite depuis la profondeur (une ligne par palier de prérequis).
  bool guild_skill_grid_ = true;

  // Expulsions passées (ZC_BAN_LIST 0x0b7c, type 4). Le serveur ne conserve ni date
  // ni auteur : seulement qui, et pourquoi.
  struct GuildBanRow {
    uint32_t char_id = 0;
    char     name[24] = {};
    char     reason[41] = {};
  };
  std::vector<GuildBanRow> guild_bans_;
  bool guild_bans_known_ = false;
  bool costume_ = false;  // == (tab_==1) ; garde pour DrawDoll/DrawSlot
  char title_filter_buf_[32] = {};  // filtre de recherche de l'onglet Titres (par libellé)
  bool need_pos_ = true;  // 1er placement de la fenetre
  float chrome_w_ = 20.0f;  // largeur du chrome (fenetre - contenu), mesuree/frame
  // Pose de l'avatar (selecteur sous l'avatar).
  int  avatar_anim_ = 4;      // animType : 0=Repos..4=Combat..8=Mort (def Combat)
  int  avatar_dir_ = 0;       // direction 0..7 (0=face)
  bool avatar_animate_ = true;
  bool avatar_show_costume_ = true;  // costumes affichés (Costume tab, ou Équip + config on)
  std::string gif_status_;    // retour UI du dernier export GIF (nom / erreur)

  // Dialogue « Enregistrer sous » du GIF : ouvert sur un THREAD séparé (ne pas
  // bloquer le rendu/réseau du jeu), résultat consommé par le thread principal.
  std::atomic<bool> gif_dialog_busy_{false};   // un dialogue est en cours
  std::atomic<bool> gif_dialog_ready_{false};  // le dialogue a rendu un résultat
  std::string gif_dialog_path_;                // chemin choisi (vide = annulé)
  int gif_export_anim_ = 4, gif_export_dir_ = 0;  // pose/dir figées au clic
  bool gif_export_show_costume_ = true;           // état costume figé au clic (GIF)

  void DrawStatsPanel();
  void DrawDoll(float avail_w);
  // Onglet Presets : liste des presets du perso (icones des items) + sauvegarde.
  void DrawPresetsTab();
  // Onglet Titres : titres d'achievement possedes + titre equipe ; clic = equiper (CZ 0x0A2E).
  void DrawTitlesTab();
  // Onglet Grimoire : l'arbre de compétences du personnage, lu dans CPlayerSkillBundle
  // (docs/skill_tree_re.md partie II). Grille 7 colonnes comme la vue « moderne » native,
  // ou liste détaillée. Monter un niveau se RÉSERVE puis s'applique (CZ 0x0112).
  void DrawSkillsTab();
  // Réserve un point sur `id` et, comme le natif, sur ses PRÉREQUIS DIRECTS manquants
  // (dans la limite des points disponibles). `to_max` monte d'un coup jusqu'au niveau
  // maximum au lieu d'un seul niveau (Ctrl + clic). Renvoie false + remplit
  // skill_status_ si rien n'a pu être réservé.
  bool ReserveSkillPoint(uint16_t id, bool to_max = false);
  // Niveau réservé pour `id` (0 = aucune réservation).
  int  PendingLevel(uint16_t id) const;
  // Onglet Guilde : infos, annonce, roster (tri + actions) et relations, tout en ImGui.
  // Lecture des globals CGuild/g_GuildInfo_* ; actions en paquets bruts (0x014f/0x0155/
  // 0x0159/0x015b/0x016e/0x0916), revalidées par le serveur.
  void DrawGuildTab();
  // Sous-onglet « Postes » : nom, droits et part d'exp des 20 postes. Édition réservée
  // au maître (le serveur exige le drapeau gmaster) ; envoi groupé en CZ 0x0161.
  void DrawGuildPositionsTab(bool can_edit);
  // Sous-onglet « Compétences » : liste ZC 0x0162 + montée (CZ 0x0112, maître seul).
  void DrawGuildSkillsTab();
  // Charge une fois skilltreeguild.lub et remplit guild_skill_tree_. Sans lui, l'onglet
  // se limite à ce que le serveur envoie (pas de niveau max, pas de verrouillés).
  void EnsureGuildSkillTree();
  // Cooldown restant en ms (0 = prête), lu dans la table partagée
  // ragnarok/skill_cooldowns.h.
  unsigned long SkillCooldownRemaining(uint16_t skill_id) const;
  // Sous-onglet « Expulsions » : historique des membres exclus (ZC 0x0b7c).
  void DrawGuildBansTab();
  // Modal « Changer l'emblème » : choix d'un .bmp de <jeu>\emblem\ (aperçu + contrôles
  // locaux) puis envoi du BMP compressé zlib en CZ_REGISTER_GUILD_EMBLEM_IMG (0x0153).
  void DrawGuildEmblemModal(int guildId, bool is_master);
  // Onglet « Dessiner » du même modal : canvas 24x24 peint en jeu, converti en BMP
  // 24 bits au moment de l'envoi (aucun fichier requis).
  void DrawGuildEmblemPaintTab(int guildId);
  // Sauve l'equipement porte actuellement comme preset nomme (perso courant).
  void SaveCurrentEquipAsPreset(const char* name);
  // Applique un preset : desequipe les slots hors preset, equipe les items manquants.
  void ApplyPreset(const EquipPreset& p);
  // Desequipe TOUT l'equipement porte (« tout nu »), costumes inclus si demande.
  // Renvoie le nombre de pieces retirees.
  int UnequipAll(bool with_costumes);
  // Declenche l'application d'un preset dont le raccourci clavier est pressé (en jeu, hors
  // saisie texte). Actif même fenêtre fermée.
  void ProcessPresetHotkeys();
  // Dessine un slot d'equipement a (x,y) taille sz dans le draw courant.
  void DrawSlot(int slot, bool costume, float x, float y, float sz);
  // Case MUNITION (à côté du bouclier) : lit la munition équipée (invIndex global, hors
  // tableau equip), affiche icône + quantité, drop = équiper, double-clic = déséquiper.
  void DrawAmmoSlot(float x, float y, float sz);
  // Colonne COMPAGNONS (à gauche de l'arme) : cases cart/peco/faucon, gated par l'état
  // serveur. Renvoie le nombre de cases dessinées (pour étendre la hauteur du contenu).
  int  DrawCompanions(float x, float y0, float sz, float gap);
  // Une case compagnon (kind 0=cart 1=peco 2=falcon) : toggle clic-G + menu contextuel cart.
  void DrawCompanionCase(int kind, float x, float y, float sz);
  // Ouvre la fenêtre d'inventaire du cart (MakeWindow natif).
  void OpenCartWindow();
  // Ouvre le dialogue "Enregistrer sous" du GIF (thread séparé, non bloquant).
  void RequestGifSave();
};
