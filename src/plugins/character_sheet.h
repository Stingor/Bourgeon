#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "plugins/plugin.h"

// Un item d'un preset d'equipement : identite suffisante pour le RETROUVER en
// inventaire lors de l'application (nameid + refine + cartes + grade). leftHand =
// arme equipee en main gauche (dual-wield, slot bouclier).
struct EquipPresetItem {
  uint32_t nameid = 0;
  int      refine = 0;
  uint32_t cards[4] = {0, 0, 0, 0};
  int      grade = 0;
  bool     leftHand = false;
};
// Un preset = un jeu d'equipement nomme, propre a UN personnage (cid = g_Own_CharId).
struct EquipPreset {
  uint32_t                     cid = 0;
  std::string                  name;
  std::vector<EquipPresetItem> items;
  // Raccourci clavier optionnel (VK Windows + modificateurs). hotkeyVk==0 => aucun.
  int  hotkeyVk = 0;
  bool hkCtrl = false, hkAlt = false, hkShift = false;
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
  // Reçoit ZC_BOURGEON_STAT_BONUS (0x0F10) : apport équip/cartes compilé par
  // status_calc_pc côté serveur, poussé à chaque recalc.
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Setting PERSISTANT (bourgeon_settings.yaml "charsheet_imgui", gere par
  // MoonlightUi). Defaut OFF : opt-in. Quand ON, Alt+F bascule la fenetre.
  bool imgui_enabled_ = false;

  // Presets d'equipement (loadouts nommes), persistes par MoonlightUi dans le yaml
  // (noeud "equip_presets"). Tous personnages confondus ; filtres par cid a l'affichage.
  std::vector<EquipPreset>& equip_presets() { return equip_presets_; }

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
    // Part du raffinage dans l'ATK / la DEF
    int  refine_atk = 0, refine_def = 0;
    std::vector<CondBonus>  cond;    // conditionnels non nuls (vs race/élément/taille)
    std::vector<SkillBonus> skills;  // bonus liés à un skill (autocast, +dégâts skill)
    std::vector<ItemBonus>  items;   // bonus liés à un item (drop)
  };
  BonusBreakdown bonus_;

  // État des COMPAGNONS (chariot / peco / faucon), poussé par le serveur
  // (ZC_BOURGEON_COMPANION_STATE 0x0F16) : niveaux des skills requis + états actifs.
  // La feuille n'affiche/gate les cases QUE d'après ceci — aucune lecture côté client
  // d'IDs de skills ni du bitmask option (ce dernier ne reflète pas le cart sous NEW_CARTS).
  struct CompanionState {
    bool valid = false;         // au moins un paquet reçu
    int  pushcart_lv = 0;       // MC_PUSHCART   (0 = non appris -> pas de case chariot)
    int  changecart_lv = 0;     // MC_CHANGECART (0 = non appris -> pas de « changer déco »)
    int  riding_lv = 0;         // KN_RIDING     (0 = non appris -> pas de case peco)
    int  falcon_lv = 0;         // HT_FALCON     (0 = non appris -> pas de case faucon)
    int  cart_active = 0;       // type de chariot courant (0 = aucun)
    bool riding_active = false; // sur peco/monture
    bool falcon_active = false; // faucon présent
    int  cart_deco_max = 1;     // type de déco max (cycle) autorisé par le niveau de base
    int  pushcart_id = 0;       // ids AEGIS des skills (envoyés par le serveur) pour l'icône
    int  riding_id = 0;
    int  falcon_id = 0;
  };
  CompanionState companion_;
  int last_cart_type_ = 1;      // dernier type de chariot actif (pour « rallumer » au même)

  std::vector<EquipPreset> equip_presets_;
  char preset_name_buf_[24] = {};  // saisie du nom (sauvegarde / renommage)
  std::string preset_status_;      // retour UI (ex. « Applique », « 2 item(s) manquant(s) »)
  int  hk_capturing_ = -1;         // index (dans equip_presets_) du preset en capture de touche
  std::string hk_conflict_msg_;    // message de conflit affiché pendant la capture
  bool show_    = true;   // fenetre visible (bascule Alt+F)
  int  tab_     = 0;      // onglet actif : 0=Equipement, 1=Costume, 2=Presets
  bool costume_ = false;  // == (tab_==1) ; garde pour DrawDoll/DrawSlot
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
  // Sauve l'equipement porte actuellement comme preset nomme (perso courant).
  void SaveCurrentEquipAsPreset(const char* name);
  // Applique un preset : desequipe les slots hors preset, equipe les items manquants.
  void ApplyPreset(const EquipPreset& p);
  // Declenche l'application d'un preset dont le raccourci clavier est pressé (en jeu, hors
  // saisie texte). Actif même fenêtre fermée.
  void ProcessPresetHotkeys();
  // Conflit du combo (vk+mods) avec un AUTRE preset (self exclu) ou un raccourci natif de la
  // barre de skills/items. Renvoie true + décrit le conflit dans `what`.
  bool HotkeyConflict(int vk, bool ctrl, bool alt, bool shift, int selfIdx, char* what, int cap);
  // Dessine un slot d'equipement a (x,y) taille sz dans le draw courant.
  void DrawSlot(int slot, bool costume, float x, float y, float sz);
  // Case MUNITION (à côté du bouclier) : lit la munition équipée (invIndex global, hors
  // tableau equip), affiche icône + quantité, drop = équiper, double-clic = déséquiper.
  void DrawAmmoSlot(float x, float y, float sz);
  // Colonne COMPAGNONS (à gauche de l'arme) : cases chariot/peco/faucon, gated par l'état
  // serveur. Renvoie le nombre de cases dessinées (pour étendre la hauteur du contenu).
  int  DrawCompanions(float x, float y0, float sz, float gap);
  // Une case compagnon (kind 0=cart 1=peco 2=falcon) : toggle clic-G + menu contextuel cart.
  void DrawCompanionCase(int kind, float x, float y, float sz);
  // Ouvre la fenêtre d'inventaire du chariot (MakeWindow natif).
  void OpenCartWindow();
  // Ouvre le dialogue "Enregistrer sous" du GIF (thread séparé, non bloquant).
  void RequestGifSave();
};
