#pragma once

#include <cstdint>
#include <string>

#include "features/plugin.h"

// ── CharDiagnostics : la fiche technique du personnage JOUÉ ──────────────────
//
// Ce que le CLIENT sait de nous, et rien d'autre. Pas une deuxième feuille de
// personnage : la feuille (Alt+F) montre au joueur ce qu'il gagne à monter une
// stat ; celle-ci montre au staff les NOMBRES sur lesquels le client agit — le
// délai d'attaque en millisecondes, la vitesse de déplacement, la cadence du
// `.act` en train d'être joué, l'action et l'image courantes.
//
// 🔴 AUCUNE VALEUR SERVEUR N'EST DEMANDÉE. Tout ce qui s'affiche est lu dans la
// mémoire du client ou décodé des paquets qu'il a reçus. C'est délibéré : la
// question à laquelle cette fenêtre répond est « qu'est-ce que MON client croit
// subir », pas « qu'est-ce que le serveur a calculé ». Les deux divergent — un
// déguisement en est le cas d'école — et c'est précisément l'écart qu'on veut
// voir.
//
// ── Les trois horloges de l'attaque ──────────────────────────────────────────
// Elles portent des noms voisins et ne mesurent pas la même chose :
//
//   1. `amotion` — le délai d'attaque décidé par le SERVEUR, en ms. Le client le
//      garde dans `g_Own_AttackDelay` (0x015fba54) et n'en montre au joueur que
//      l'ASPD dérivée, `(2000 - amotion) / 10`. C'est lui qui borne la cadence
//      réelle des coups.
//   2. `dmotion` — le temps pendant lequel on RECULE en encaissant. Il ne vit
//      dans aucune globale : il arrive coup par coup, dans `ZC_NOTIFY_ACT`
//      (`dst_speed`). D'où l'observation de paquets ci-dessous : c'est le seul
//      moyen de le connaître, et la valeur affichée est donc celle du DERNIER
//      coup, pas un état permanent.
//   3. la cadence du `.act` joué — combien de temps dure vraiment l'animation.
//      Le client la calcule ainsi (RE 2026-08-17, `CActorSprite_StartAction`
//      0x00c55c30 et `CActorSprite_AdvanceAnimState` 0x00c46c80) :
//
//        facteur    = min(amotion, g_MotionSpeedCapMs) / 432   (acteur+0x64)
//        délai/img  = vitesse déclarée de l'action × facteur   (acteur+0x58)
//        durée      = nb d'images × délai/img × 24 ms
//
//      🔴 Les 24 ms sont bien celles du client (`elapsed / 24.0`), PAS les 25 ms
//      des outils de format de fichier. Et le plafond de 432 ms est un vrai
//      plafond : au-delà, l'animation ne ralentit plus — elle finit et le
//      personnage attend le coup suivant.
//
// C'est cette chaîne-là qui répond à la question du déguisement : sous un sprite
// de monstre, le nombre d'images et la vitesse déclarée sont celles du `.act` du
// MONSTRE. La fenêtre montre donc côte à côte l'amotion du serveur et la durée
// que le sprite joué lui donne réellement.
//
// ── Gate ─────────────────────────────────────────────────────────────────────
// `IsStaff()` (niveau de groupe serveur >= 80), revérifié à CHAQUE frame comme
// dans l'inspecteur d'entités : perdre le staff en cours de session referme la
// fenêtre au lieu de laisser des adresses mémoire à l'écran. Ouverte depuis
// « Staff Tools », et de là seulement.

class CharDiagnostics : public Plugin {
 public:
  CharDiagnostics();

  // 🔴 Ce qui SORT, compté à la source. `RagConnection::SendPacketHook` est le
  // seul endroit qui voie partir un paquet du client, en clair et avant le XOR
  // natif ; nos propres envois le contournent, donc ce qui passe ici est un
  // geste du JOUEUR.
  //
  // C'est la mesure qui sépare les deux moitiés du problème : comparer le nombre
  // de CZ_USE_SKILL ÉMIS au nombre de coups REÇUS dit si le frein est avant
  // l'envoi (saisie, ciblage, garde client) ou après (refus du serveur).
  //
  // Statique et sans état de plugin : appelée depuis la couche réseau, qui n'a
  // pas à connaître l'instance. Sans coût quand la fenêtre est fermée.
  static void NoteSend(uint16_t opcode);

  const char* name() const override { return "CharDiagnostics"; }

  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  // 🔴 FIL RÉSEAU : on copie, rien de plus (cf. features/net_inbox.h).
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  void Open();
  void Toggle();
  bool IsOpen() const { return open_; }

  // ── Contour des zones cliquables ─────────────────────────────────────────
  //
  // 🔴 CE QUE LA SOURIS DÉSIGNE VRAIMENT. Le client tient un quadtree de
  // rectangles ÉCRAN, reconstruit à chaque image par le rendu des sprites, et
  // c'est LUI — pas la position au sol — qui décide de la cible d'un clic. Le
  // rectangle le plus « devant » gagne, quelle que soit l'entité à qui il
  // appartient.
  //
  // D'où cet overlay : quand une compétence ne part pas alors que le curseur
  // semble sur le monstre, la seule façon de le voir est de dessiner les
  // rectangles et de regarder lequel est au-dessus.
  //
  // Vit hors de la fenêtre : elle peut être fermée pendant qu'on observe.
  bool& show_pick_boxes() { return show_pick_boxes_; }

  // ── Précision des zones cliquables ───────────────────────────────────────
  //
  // 🔴 Le client ÉLARGIT tout quad plus petit qu'un minimum, symétriquement, à
  // `échelle × 40` pixels — pour qu'un petit sprite reste attrapable. C'est de
  // là que vient l'essentiel du « flou » du ciblage : un poring de 30 px se
  // défend sur 40, et deux entités voisines finissent par se recouvrir.
  //
  // Cette valeur ne sert QU'À ÇA (deux lectures dans tout le binaire, toutes
  // deux dans la construction du quad), donc l'abaisser resserre le picking
  // sans toucher aux plaques de nom ni au rendu.
  //
  // ⚠ Ça ne rétrécit PAS un GROS sprite : son rectangle vient de son dessin.
  // Le clone de déguisement restera large — c'est un autre problème.
  // Interrupteur : DÉCOCHÉ, on ne touche à rien du tout. C'est l'état qui
  // permet de mesurer le client tel quel — ou de vérifier ce que fait un patch
  // posé sur l'exécutable, que nos écritures masqueraient sinon.
  bool& pick_min_enabled() { return pick_min_enabled_; }
  int& pick_min_size() { return pick_min_size_; }
  int  pick_min_default() const { return pick_min_default_; }

 private:
  // ── Ce qui se relit à CHAQUE frame ────────────────────────────────────────
  //
  // Champs bruts, jamais de `std::string` : le remplissage tourne sous SEH, et
  // MSVC refuse un `__except` dans une fonction qui doit dérouler des objets
  // C++ (C2712). Même contrainte, même forme que l'inspecteur d'entités.
  struct Snapshot {
    // Identité
    uint32_t aid = 0, cid = 0;
    char     char_name[64] = {};
    int      job_real = 0;      // g_Own_JobId — ce que le serveur dit qu'on EST
    char     job_real_name[96] = {};
    int      base_level = 0, job_level = 0;
    int      status_point = 0, skill_point = 0;
    int      zeny = 0, manner = 0;
    int      weight = 0, weight_max = 0;
    int      hp = 0, hp_max = 0, sp = 0, sp_max = 0;
    // 🔴 Sur 64 bits, en DEUX globales (lo/hi) : l'expérience des hauts niveaux
    // déborde depuis longtemps d'un entier signé 32 bits, et n'en lire que la
    // moitié basse afficherait des sauts absurdes.
    uint64_t base_exp = 0, base_exp_next = 0;
    uint64_t job_exp = 0, job_exp_next = 0;
    int      body_state = 0, health_state = 0, effect_state = 0;

    // Stats : base, bonus, coût de la prochaine montée
    int stat_base[6] = {}, stat_bonus[6] = {}, stat_cost[6] = {};

    // Dérivées
    int atk1 = 0, atk2 = 0, matk_min = 0, matk_max = 0;
    int def_soft = 0, def_hard = 0, mdef_soft = 0, mdef_hard = 0;
    int hit = 0, flee = 0, pdodge = 0, crit = 0;
    int amotion = 0;      // 0x015fba54 — le délai d'attaque du SERVEUR, en ms
    int walk_speed = 0;   // g_Own_Speed — ms par cellule
    int motion_cap = 0;   // g_MotionSpeedCapMs (0x01228f60), lu vivant

    // ── Acteur ───────────────────────────────────────────────────────────────
    bool     actor_found = false;
    uint32_t actor_addr = 0, actor_gid = 0;
    int      job_shown = 0;     // acteur+0x25c — ce que le monde VOIT de nous
    char     job_shown_name[96] = {};
    // La classe que `CActorSprite_ResolveDisplayJob` rend : +0x25c passé par les
    // masques d'option. C'est ELLE qui choisit le fichier de sprite.
    int      job_resolved = 0;
    char     job_resolved_name[96] = {};
    int      sex = 0;
    int      motion_state = 0;  // acteur+0x70
    int      action_base = 0;   // acteur+0x34 (action × 8, sans la direction)
    int      action_played = 0; // acteur+0x38 (action × 8 + direction)
    int      frame_index = 0;   // acteur+0x3c
    float    frame_delay = 0.0f;    // acteur+0x58, en « ticks » de 24 ms
    float    motion_factor = 0.0f;  // acteur+0x64 = amotion / 432
    uint32_t anim_start = 0;    // acteur+0x8c (timeGetTime du dernier départ)
    uint32_t anim_age = 0;      // ms écoulées depuis
    int      weapon_view = 0, shield_view = 0;  // acteur+0x440 / +0x444
    float    pos_x = 0.0f, pos_y = 0.0f, pos_z = 0.0f;
    float    facing = 0.0f;     // acteur+0x4c, en degrés

    // ── Le `.act` du corps, tel que l'animation le consomme ──────────────────
    //
    // 🔴 UN `.act` EST INDEXÉ PAR `action × 8 + direction`, pas par action : les
    // huit directions sont huit ENTRÉES INDÉPENDANTES, chacune avec son propre
    // nombre d'images. Un fichier de 104 entrées, ce sont 13 actions × 8. Lire
    // l'entrée `action` au lieu de `action × 8 + direction` donne un nombre
    // plausible et FAUX — c'est le premier défaut qu'a eu cette fenêtre.
    bool  act_found = false;
    char  act_path[264] = {};
    char  spr_path[264] = {};
    int   act_action_count = 0;   // nb d'ENTRÉES du fichier (= actions × 8)
    int   played_index = 0;       // l'entrée réellement jouée (= acteur+0x38)
    int   played_frames = 0;      // ses images (0 = entrée absente du fichier)
    float played_speed = 0.0f;    // sa vitesse déclarée

    // Une action observée sur ses HUIT directions. C'est ce tableau qui répond à
    // « pourquoi la cadence change quand je tourne la caméra » : si les huit
    // valeurs ne sont pas égales, la durée de l'animation — donc le temps passé
    // dans un état qui BLOQUE l'envoi du skill suivant — dépend de l'orientation.
    struct ActProbe {
      int   action = -1;    // 0..12, -1 = sonde vide
      int   frames[8] = {}; // par direction ; 0 = entrée hors du fichier
      float speed = 0.0f;   // vitesse déclarée de l'entrée `action × 8`
    };
    ActProbe probes[5];  // action jouée, puis ATTACK1, ATTACK2, ATTACK3, SKILL
  };

  // ── Le dernier coup vu passer ────────────────────────────────────────────
  // Un `ZC_NOTIFY_ACT` n'est pas un état : c'est un événement. On garde le
  // dernier de chaque sens, avec l'heure, et l'affichage dit son âge — un
  // nombre de 40 secondes ne décrit plus rien.
  struct Blow {
    bool     seen = false;
    uint32_t tick = 0;       // timeGetTime local à la réception
    int32_t  src_speed = 0;  // amotion de l'attaquant
    int32_t  dst_speed = 0;  // dmotion imposé à la cible
    int32_t  damage = 0;
    int      div = 0;
    int      type = 0;
    uint32_t other_gid = 0;  // l'autre bout du coup
    int      skill_id = 0;   // 0 = attaque normale (ZC_NOTIFY_ACT)
  };

  void Refresh(Snapshot* out);          // globales de session (sans SEH lourd)
  void ReadActor(Snapshot* out);        // l'acteur + son `.act` (sous SEH)
  void DrawBody(const Snapshot& snap);
  void DrawBlow(const char* title, const Blow& blow, bool incoming);
  void DrawProbe(const Snapshot& snap, const Snapshot::ActProbe& probe);
  std::string BuildReport(const Snapshot& snap) const;

  // Cadence RÉELLEMENT observée, en coups par seconde, mesurée sur les derniers
  // coups portés — ou 0 si l'on n'en a pas assez ou s'ils sont trop vieux.
  //
  // 🔴 Elle vient des PAQUETS, pas d'un échantillonnage par frame : c'est la même
  // horloge que le journal de combat, donc le chiffre est comparable à ce que le
  // joueur compte à l'écran, et il ne dépend pas du nombre d'images par seconde.
  float ObservedRate(int* out_samples, float* out_window_s) const;

  // Horodate un coup PORTÉ. Un coup multiple (`div` > 1) ne compte que pour un :
  // ce qu'on mesure est le rythme des envois, pas le nombre de dégâts.
  void PushHitTime(uint32_t tick);

  void DrawPickBoxes();
  void ApplyPickMinSize();

  bool open_ = false;
  bool need_focus_ = false;
  bool show_pick_boxes_ = false;
  // -1 = jamais réglé : on laisse au client SA valeur, et on la relève au
  // passage pour la proposer comme point de départ au curseur.
  bool pick_min_enabled_ = false;
  int  pick_min_size_    = -1;
  int  pick_min_default_ = 0;
  // Avons-nous écrit dans la globale ? Sert à la restaurer UNE fois, à la
  // transition, au lieu de la réécrire indéfiniment.
  bool pick_min_written_ = false;

  Blow dealt_;     // dernier coup que J'AI porté
  Blow taken_;     // dernier coup que J'AI encaissé

  // Anneau des horodatages des coups portés (timeGetTime), pour la cadence.
  static constexpr int kRateRing = 24;
  uint32_t hit_ring_[kRateRing] = {};
  int      hit_ring_count_ = 0;
  int      hit_ring_head_  = 0;

  // ── Histogramme des états de motion ──────────────────────────────────────
  //
  // 🔴 LA MESURE QUI TRANCHE. `Actor_ProcessPendingAction_Tick` n'émet le skill
  // en attente que dans certains états ; si le personnage passe l'essentiel de
  // son temps dans un état BLOQUANT, la cadence est bornée par le client et par
  // rien d'autre. Si au contraire il est presque toujours dans un état qui
  // laisse passer, alors le frein est ailleurs — serveur, portée, ou ciblage.
  //
  // Échantillonné une fois par image, donc dépendant du nombre d'images par
  // seconde : c'est une PROPORTION de temps, à lire comme telle, pas un compte
  // d'événements.
  static constexpr int kStateBuckets = 64;
  uint32_t state_hist_[kStateBuckets] = {};
  uint32_t state_samples_ = 0;   // images échantillonnées
  uint32_t state_blocked_ = 0;   // …dont dans un état qui bloque les skills
  uint32_t state_since_ms_ = 0;  // début de la mesure (timeGetTime)

  void ResetMeasures();
};

// Opcodes SORTANTS comptés (cf. CharDiagnostics::NoteSend). Ceux du chantier
// « latence des compétences » : le skill sur cible, le skill au sol, l'attaque.
namespace char_diag {
constexpr uint16_t kCzUseSkill       = 0x0438;  // CZ_USE_SKILL
constexpr uint16_t kCzUseSkillGround = 0x0af4;  // CZ_USE_SKILL_TOGROUND
constexpr uint16_t kCzRequestAct     = 0x0437;  // CZ_REQUEST_ACT (attaque)
}  // namespace char_diag
