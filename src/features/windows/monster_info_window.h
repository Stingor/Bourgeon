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

  // Aperçu AU SURVOL d'un lien de monstre : une infobulle compacte — sprite,
  // niveau, race, élément, taille, PV/SP, vitesse. Le pendant de la description
  // simple d'un objet, et le même rôle : décider si l'on ouvre la fiche.
  //
  // La demande au serveur part d'ici si la fiche n'est pas connue (une seule
  // fois par monstre, `RequestInfo` porte sa propre garde) ; en attendant,
  // l'infobulle le dit. N'ouvre NI ne change la fiche affichée — survoler n'est
  // pas cliquer.
  void DrawHoverPreview(uint32_t mob_id);

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
    // Nom affiché, UTF-8, résolu au décodage du paquet. Trois sources par ordre
    // de préférence : le client (Lua `GetSkillName`, localisé), le serveur
    // (skill_db `desc`, qui voyage dans le paquet), l'id brut. 🔴 Aucune entrée
    // n'est écartée : le client ne sait nommer que les compétences de JOUEUR, et
    // les filtrer là-dessus faisait disparaître toutes les `NPC_*` de la fiche.
    std::string name;
    // Le client sait-il NOMMER cette compétence (skillinfolist.lub) ? Décide
    // seulement de la préférence de nom — le client est localisé, le serveur non.
    bool        client_named = false;
    // Le client a-t-il une DESCRIPTION pour elle (skilldescript.lub, via le
    // global Lua `GetSkillDescript`) ? 🔴 C'est un AUTRE fichier que le nom :
    // une compétence peut avoir l'un sans l'autre, et c'est le cas courant des
    // `NPC_*`. Seule cette réponse décide si la ligne est cliquable.
    bool        has_desc = false;
  };

  // La fiche renvoyée par ZC 0x0F20. Les champs suivent l'ordre du paquet.
  struct MobInfo {
    Fetch    state = Fetch::kIdle;
    uint32_t requested_tick = 0;
    // Comment cet id a été demandé la première fois. Un rafraîchissement doit
    // reposer la MÊME question : un id de classe de sprite (Sense) redemandé en
    // `by_view = 0` désignerait un autre monstre, ou aucun.
    bool     by_view = false;

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

    // ── Identité : de QUEL monstre cette fiche parle-t-elle ──────────────────
    // Beaucoup de monstres partagent exactement le nom affiché et l'apparence
    // d'un autre (versions d'événement, d'invocation, d'instance). Le joueur qui
    // ouvre l'un d'eux voit un monstre familier sans butin ni spawn et croit à
    // un bug de la fiche. Ces champs lui donnent de quoi comprendre.
    //
    // 🔴 Ce sont des FAITS envoyés par le serveur, jamais une déduction. Le
    // préfixe de l'AegisName N'EST PAS un discriminant : des noms légitimes
    // commencent par ORC_/KOBOLD_/GOBLIN_/THIEF_, de vraies variantes portent au
    // contraire un underscore FINAL (FABRE_, CHONCHON_), et 88 des homonymes de
    // ce mob_db (GOBLIN_2..5, DIMIK_1..4, VENATU_1..4…) sont des monstres à part
    // entière, spawnés et avec butin. L'absence de butin n'en est pas un non plus :
    // 51 monstres de BASE n'en ont aucun.
    std::string aegis;            // AegisName : l'identité unique, elle
    bool     summoned = false;    // cible d'un NPC_SUMMONSLAVE (les G_* pour la plupart)
    uint8_t  namesake_count = 1;  // combien de monstres portent ce nom affiché (lui compris)
    uint32_t namesake_ref = 0;    // le plus petit id qui le porte (= lui si c'est lui)

    // ── Ce que ce monstre rapporte à CE personnage ───────────────────────────
    // L'EXP de mob_db ci-dessus est la même pour tout le monde ; celle-ci passe
    // par le calcul complet du serveur (mob_estimate_exp_gain) : malus de haut
    // niveau, cartes bExpAddRace / bExpAddClass, Battle Manual, VIP, event EXP.
    // C'est le chiffre que la warp agent annonce déjà dans ses listes de chasse.
    //
    // 🔴 INSTANTANÉ, pas une donnée de base : il ne vaut que pour l'état du
    // personnage au moment de la demande. `exp_base_level` / `exp_job_level`
    // gardent cet état pour que l'ouverture suivante sache qu'il a vieilli — le
    // reste de la fiche, lui, ne bouge jamais.
    bool     exp_valid = false;
    uint32_t est_base_exp = 0, est_job_exp = 0;
    uint32_t next_base_exp = 0, next_job_exp = 0;
    bool     max_base_lv = false, max_job_lv = false;
    int      exp_base_level = 0, exp_job_level = 0;

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
  // Bandeau d'homonymie : combien de monstres portent ce nom, lequel est
  // celui-ci, et un raccourci vers le plus ancien. N'affiche rien quand le nom
  // est unique. Cf. le commentaire de MobInfo::aegis pour le pourquoi.
  void DrawNamesakeNote(MobInfo& mob);
  // Clic sur le sprite : on titille le monstre. Arme l'animation ponctuelle
  // (dégât, ou attaque une fois sur huit) et joue sa voix.
  void PokeSprite();
  void DrawStatsTab(MobInfo& mob);
  // « Ce qu'il vous rapporte » : l'EXP estimée par le serveur pour CE personnage,
  // en pour cent de la barre et en nombre de monstres. N'affiche rien tant que le
  // serveur ne l'a pas envoyée (serveur plus ancien, ou fiche encore en vol).
  void DrawExpGain(MobInfo& mob);
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

  // ── Le sprite se manipule ──────────────────────────────────────────────────
  // Molette au-dessus = on tourne le monstre : le .act range ses actions en
  // motion * 8 + direction, donc on prend la POSE orientée (jamais un miroir,
  // qui déplacerait l'ombre du mauvais côté — cf. features/overlays/login_parade.cc).
  // Clic = on le titille : animation de dégât + le son de l'image + un sursaut.
  // Il riposte de temps en temps, et finit par faire le mort si on insiste.
  //
  // 🔴 L'orientation SURVIT au changement de monstre (c'est un réglage de
  // lecture), l'animation ponctuelle non — son numéro d'action ne vaut que pour
  // le .act qui l'a produite.
  int   sprite_dir_    = 0;    // 0 = sud, comme la fenêtre native
  int   poke_action_   = -1;   // action ponctuelle en cours, -1 = aucune
  float poke_start_    = 0.0f; // horloge ImGui au moment du clic
  float poke_anim_end_ = 0.0f; // fin de l'ANIMATION : elle ne BOUCLE pas
  // Fin de la RÉACTION. Vaut `poke_anim_end_`, sauf pour la mort : le monstre
  // reste à terre (dernière image figée) un moment avant de se relever.
  float poke_end_      = 0.0f;
  bool  poke_freeze_   = false;  // tenir une image fixe jusqu'à `poke_end_`
  // Horloge d'animation à tenir pendant ce gel : elle vise la dernière image
  // VISIBLE de l'action, car une animation de mort se termine sur des images
  // vides (le corps disparaît). Calculée au clic, valable pour ce .act.
  float poke_freeze_clock_ = 0.0f;
  // Prochain clic accepté. Sans ce temps mort, un clic répété relance
  // l'animation à chaque image et empile les sons.
  float poke_ready_at_ = 0.0f;
  int   poke_count_    = 0;    // chatouilles cumulées -> riposte, puis mort
  // Dernière image dessinée pendant la réaction. Le son du .act appartient à
  // l'IMAGE, pas à l'action : on ne le déclenche qu'au CHANGEMENT d'image, sur
  // celle qui est vraiment à l'écran. -1 = aucune image encore dessinée.
  int   poke_frame_  = -1;
  // Le son de coup de repli reste à jouer, sur l'image portant le marqueur
  // « atk » — le cas d'une action marquée mais sans wav.
  bool  poke_hit_pending_ = false;

  // Description de compétence demandée pendant le rendu, jouée par FlushPending.
  // 0 = rien en attente ; une seule demande en vol (une souris, un geste).
  int pending_skill_desc_ = 0;
  int pending_skill_x_    = 0;
  int pending_skill_y_    = 0;

  bool animate_        = true;
  bool show_guardians_ = false;
};
