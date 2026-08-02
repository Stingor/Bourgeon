#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "features/plugin.h"
#include "ui/mob_sprite.h"

// ── MonsterInfoWindow ────────────────────────────────────────────────────────
//
// Fiche détaillée d'un monstre : le pendant « monstre » de la fiche d'item.
// Elle remplace la fenêtre native « Monster Info » (UIMonsterInfoWnd, id 0x4D)
// qu'ouvre le skill Sense (WZ_ESTIMATION, id 93) — RE complète dans
// docs/monster_info_re.md.
//
// ── Pourquoi elle ne se contente PAS du paquet de Sense ──────────────────────
// ZC_MONSTER_INFO (0x018C) ne porte que neuf champs et neuf résistances, et
// SURTOUT aucun nom : le natif va chercher celui-ci dans le dictionnaire de noms
// d'entité, à la clé d'un global écrit par l'ENVOI du skill. Quand un coéquipier
// lance Sense, le serveur diffuse le paquet à tout le groupe et ce global est
// périmé chez les autres — la fenêtre native affiche alors le bon monstre avec
// le mauvais nom. Cf. docs/monster_info_re.md §6.1.
//
// On demande donc la fiche au serveur (CZ 0x0F1F -> ZC 0x0F20), qui a mob_db :
// nom, EXP, ATK/MATK, stats de base, modes, drops, cartes de spawn, skills. Le
// relevé de Sense sert de complément : il porte l'état RÉEL du monstre croisé,
// qui peut différer de la base (WoE, buffs, modificateurs de serveur).
//
// ── Deux points d'entrée ─────────────────────────────────────────────────────
//  1. le skill Sense — on REVENDIQUE le paquet 0x018C, donc la fenêtre native ne
//     naît jamais. Sans dette protocolaire : la classe native n'émet aucun
//     paquet, ni à la création ni à la destruction (docs §7.1) ;
//  2. `Open(mob_id)` — depuis n'importe où : la table des drops d'une fiche
//     d'item, un futur bestiaire, une plaque de nom…
//
// OPT-IN : membre du groupe « Interface moderne ». Coupée, le paquet 0x018C
// repart au handler natif et la fenêtre d'origine réapparaît.

class MonsterInfoWindow : public Plugin {
 public:
  MonsterInfoWindow();

  const char* name() const override { return "MonsterInfoWindow"; }

  void OnRenderUI() override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Ouvre (ou ré-ouvre) la fiche du monstre `mob_id` et demande les données au
  // serveur si elles ne sont pas déjà en cache.
  //
  // `by_view` : l'id est une classe de SPRITE et non un id de mob_db — le cas du
  // skill Sense, dont le paquet porte `md->vd->look[LOOK_BASE]`. C'est le SERVEUR
  // qui fait la correspondance inverse (il a mob_db, le client non).
  void Open(uint32_t mob_id, bool by_view = false);

  bool IsOpen() const { return open_; }

  // Rejoue HORS frame ImGui l'ouverture de la description d'une compétence
  // (chemin natif MakeWindow + OnMsg). Appelée par Bourgeon::OnProcessInput,
  // comme WeaponRefineWindow::FlushPending — cf.
  // [[feedback_no_native_cmd_during_imgui_frame]].
  void FlushPending();

  // Contenu de la section « Fiche de monstre » du panneau Moonlight.
  // Rend true si un réglage a changé.
  bool DrawSettings();

  // « monsterinfo_imgui » : basculé en GROUPE par SetModernInterface. Défaut OFF.
  bool imgui_enabled_ = false;
  // « monsterinfo_animate » : faire tourner l'animation idle du sprite. Le natif,
  // lui, fige la première image (docs §4.4). Défaut ON — c'est l'intérêt.
  bool& animate() { return animate_; }
  // « monsterinfo_guardians » : lever la liste noire des cinq Gardiens de
  // forteresse que le client refuse d'afficher pendant la GdE. Défaut OFF : ce
  // filtre est une règle de jeu, pas un défaut d'interface (docs §5).
  bool& show_guardians() { return show_guardians_; }

 private:
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // ── Modèle ────────────────────────────────────────────────────────────────
  enum class Fetch { kIdle, kPending, kReady, kUnknown };

  struct Drop {
    uint32_t    nameid = 0;
    uint32_t    rate   = 0;   // 1/100 de %
    uint8_t     kind   = 0;   // 0 = drop normal, 1 = récompense MVP
    std::string name;
  };
  struct Spawn {
    uint16_t    qty = 0;
    std::string map;
  };
  struct MobSkill {
    uint16_t    id = 0;
    uint16_t    lv = 0;
    // Nom résolu par le client (Lua `GetSkillName`), en UTF-8. 🔴 Une entrée
    // dont le nom ne se résout PAS n'entre jamais dans cette liste : afficher
    // « compétence #482 » n'apprend rien au joueur et ne fait que l'interroger.
    // La résolution a donc lieu au décodage du paquet, pas au rendu — c'est elle
    // qui décide du contenu de l'onglet, donc de son compteur et de sa présence.
    std::string name;
  };

  // La fiche renvoyée par ZC 0x0F20. Les champs suivent l'ordre du paquet.
  struct MobInfo {
    Fetch    state = Fetch::kIdle;
    uint32_t requested_tick = 0;

    uint32_t sprite_class = 0;
    uint16_t level = 0;
    uint32_t hp = 0, sp = 0;
    uint32_t base_exp = 0, job_exp = 0, mvp_exp = 0;
    uint16_t atk_min = 0, atk_max = 0, matk_min = 0, matk_max = 0;
    uint16_t def = 0, def2 = 0, mdef = 0, mdef2 = 0;
    uint16_t str = 0, agi = 0, vit = 0, int_ = 0, dex = 0, luk = 0;
    uint16_t range_atk = 0;
    uint8_t  size = 0, race = 0, element = 0, element_lv = 0;
    uint8_t  boss = 0, klass = 0;
    uint32_t mode = 0;
    // Temps de marche d'UNE case, en millisecondes (plus petit = plus rapide).
    // La fiche le retourne en cases/seconde, l'unité dans laquelle un joueur
    // pense sa fuite.
    uint16_t speed = 0;
    int16_t  resist[10] = {0};   // % encaissé par élément d'ATTAQUE, SIGNÉ
    std::string name;
    std::vector<Drop>     drops;
    std::vector<Spawn>    spawns;
    std::vector<MobSkill> skills;
  };

  // Relevé du skill Sense : l'état RÉEL du monstre croisé, à opposer à mob_db.
  // Pas d'id de mob_db ici — le paquet ne porte qu'une classe de sprite.
  struct SenseSnapshot {
    bool     valid = false;
    uint32_t sprite_class = 0;
    uint16_t level = 0, size = 0;
    uint32_t hp = 0;
    uint16_t def = 0, race = 0, mdef = 0, element = 0;
    uint8_t  resist[9] = {0};
  };

  void RequestInfo(uint32_t mob_id, bool by_view);
  // Le monstre affiché ce frame, ou nullptr tant que rien n'est arrivé.
  MobInfo* Current();

  void DrawHeader(MobInfo& mob);       // sprite + nom + badges
  void DrawStatsTab(MobInfo& mob);
  void DrawResistTab(MobInfo& mob);
  void DrawDropsTab(MobInfo& mob);
  void DrawSpawnsTab(MobInfo& mob);
  void DrawSkillsTab(MobInfo& mob);
  void DrawSenseNote(MobInfo& mob);    // écarts base <-> relevé Sense

  // Cache par id DEMANDÉ (celui qu'on a envoyé), pas par id résolu : c'est la
  // clé dont on dispose au moment d'apparier la réponse, qui la renvoie en écho.
  std::unordered_map<uint32_t, MobInfo> cache_;

  uint32_t current_id_ = 0;   // id demandé pour la fiche affichée
  bool     open_       = false;
  bool     need_focus_ = false;

  SenseSnapshot sense_;

  // Ressources .spr/.act du monstre affiché (rechargées quand il change).
  ro::MobSpriteRes sprite_;

  // Description de compétence demandée pendant le rendu, jouée par FlushPending.
  // 0 = rien en attente ; une seule demande en vol (une souris, un geste).
  int pending_skill_desc_ = 0;
  int pending_skill_x_    = 0;
  int pending_skill_y_    = 0;

  bool animate_        = true;
  bool show_guardians_ = false;
};
