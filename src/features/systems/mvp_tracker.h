#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "features/plugin.h"

// ── MvpTracker ───────────────────────────────────────────────────────────────
//
// Volet CLIENT du carnet de chasse MVP partagé : l'état réseau, sans une ligne
// de rendu. La fenêtre (MvpTrackerWindow) et la couche de minimap le lisent.
//
// ── Trois objets, et pas un de plus ─────────────────────────────────────────
//  · un CRÉNEAU (Slot) : une occasion récurrente, clé stable `(mob_id, carte)`.
//    Le catalogue arrive une fois par session, sur demande ;
//  · une OBSERVATION (Obs) : ce que le GROUPE sait d'un créneau. Jamais « l'état
//    du MVP » — elle porte qui l'a dite, quand, et avec quelle précision ;
//  · le GROUPE : des comptes Moonlight qui partagent ce qu'ils voient.
//
// ── Ce que le serveur ne dit PAS, et c'est voulu ────────────────────────────
// Le serveur connaît l'instant exact du retour de chaque MVP à la milliseconde.
// Il ne l'envoie que là où un joueur l'a PAYÉ avec un Convex Mirror. Partout
// ailleurs, `exact_respawn` vaut 0 et on ne dessine pas un point mais une
// FENÊTRE, déduite de la loi publique (`delay1`, `delay2`) : ce sont les chiffres
// écrits dans le script de spawn, que le site publiait déjà.
//
// ── L'heure ──────────────────────────────────────────────────────────────────
// Chaque paquet d'état porte `server_time`. On en tire un DÉCALAGE une fois, et
// tous les comptes à rebours en sont dérivés à l'affichage — jamais un compteur
// entretenu, jamais un fuseau. Le prototype PHP s'est fait piéger exactement là.
//
// Sur le fil : CZ 0x0F30 (toutes les commandes), ZC 0x0F31 (catalogue,
// instantané, delta, favoris), ZC 0x0F32 (groupe, invitation, refus).
// Conception : docs/mvp_tracker.md ; implémentation : docs/mvp_tracker_blueprint.md.

namespace mvp {

enum class SlotKind : uint8_t {
  kBossSpawn = 0,      // boss_monster : le spawn_timer du map-server
  kScriptTimer,        // Bio Lab : timer de NPC, le mob change à chaque cycle
  kScriptInvasion,     // Lord of Death : timer de NPC, position tirée
  kSummonLock,         // Thanatos : pas un respawn, une disponibilité
};

// ⚠ L'ORDRE EST L'ORDRE DE PRÉCISION, le serveur s'en sert pour arbitrer.
enum class Source : uint8_t {
  kManual = 0,  // ce qu'un joueur affirme
  kTomb,        // une tombe lue sur place
  kKill,        // un membre l'a tué : le serveur l'a vu
  kMirror,      // Convex Mirror d'un membre : l'instant vrai, payé
};

struct Slot {
  uint16_t slot_id   = 0;
  uint16_t mob_id    = 0;   // 0 = créneau scripté, le mob varie
  SlotKind kind      = SlotKind::kBossSpawn;
  uint32_t delay1_ms = 0;   // la LOI : au plus tôt après la mort
  uint32_t delay2_ms = 0;   // amplitude du tirage
  // 🔴 Taille de la carte EN CELLULES, envoyée par le serveur parce que le
  // client ne mesure que la carte où il se trouve. C'est ce qui permet de poser
  // une tombe au bon endroit sur le plan d'une carte lointaine : la fraction de
  // carte, et surtout PAS « un texel du bitmap = une cellule ».
  uint16_t map_xs    = 0;
  uint16_t map_ys    = 0;
  char     map[16]   = {};
  char     name[24]  = {};  // vide pour un créneau scripté
};

struct Obs {
  Source   source        = Source::kManual;
  uint16_t mob_id        = 0;
  int64_t  kill_time     = 0;
  int64_t  exact_respawn = 0;   // 0 = NON MÉRITÉ : dessiner une fenêtre
  int16_t  tomb_x        = -1;  // -1 = inconnue. JAMAIS 0,0, qui est une cellule valide
  int16_t  tomb_y        = -1;
  uint32_t by_user_id    = 0;
  int64_t  reported_at   = 0;   // l'ÂGE fait partie de l'information
};

struct Member {
  uint32_t user_id = 0;
  int16_t  level   = 0;
  bool     online  = false;
  char     name[24] = {};
};

// Ce que le serveur répond à une commande de groupe.
//
// 🔴 MIROIR EXACT de `e_mvp_group_result` (moonlight/src/map/mvp_tracker.hpp) :
// la valeur voyage dans le champ `result` de ZC_BOURGEON_MVP_GROUP. Toute
// entrée s'AJOUTE EN FIN des deux côtés — insérer au milieu décalerait tous les
// messages suivants, et le joueur lirait une raison qui n'est pas la sienne.
enum class Result : uint8_t {
  kOk = 0,
  kNoAccount,        // compte de jeu non rattaché à un compte Moonlight
  kAlreadyMember,    // vous êtes déjà dans un groupe
  kNotMember,        // vous n'êtes dans aucun groupe
  kNotOwner,
  kNoSuchUser,       // personnage introuvable
  kSelf,
  kFull,
  kBadName,
  kNoInvite,
  kTargetInGroup,    // la cible appartient à un AUTRE groupe
  kSql,
  kTargetSameGroup,  // la cible est déjà dans le vôtre (autre tête du compte)
  kNotInvitable,     // existe, mais hors guilde et hors amis
};

struct Group {
  uint32_t group_id      = 0;   // 0 = dans aucun groupe
  uint32_t owner_user_id = 0;
  char     name[32]      = {};
  std::vector<Member> members;
};

}  // namespace mvp

struct MvpTrackerConfig {
  // Interrupteur maître. C'est LUI que le masque UiCaps annonce au serveur :
  // éteint, le serveur cesse de diffuser les deltas à ce joueur — qui continue
  // pourtant d'ALIMENTER son groupe par ses kills.
  bool enabled       = false;
  // Alerte quand un FAVORI entre dans sa fenêtre. Le favori lui-même est en SQL
  // (il suit le compte) ; seuls le préavis et le son sont locaux — ce sont des
  // préférences d'interface, pas de la donnée.
  bool alert_sound   = true;
  int  alert_lead_min = 5;
  // Couche de tombes sur la minimap. Réglage PROPRE, pas un détournement de
  // `minimap_show_boss` : les deux marqueurs n'ont ni la même source ni la même
  // durée de vie.
  bool show_tombs    = true;
  // Le sprite du MVP dans son rang. Allumé par défaut : une table de 81 lignes
  // de texte se lit mal, et reconnaître un monstre à sa silhouette est plus
  // rapide que lire son nom anglais.
  bool show_sprites  = true;
  // 🔴 Pas de réglage de TAILLE, et c'est une décision mesurée, pas un oubli.
  //
  // Le côté du sprite fixe la hauteur de rang, donc le nombre de lignes que le
  // clipper affiche, donc le nombre de sprites ouverts en même temps — et un
  // sprite de MVP décodé pèse ~4,9 Mo (relevé en jeu). La taille commande donc
  // la pression mémoire, à l'envers de l'intuition :
  //   24 px -> ~12 lignes -> ~59 Mo, AU-DESSUS du budget du cache (48 Mo) :
  //            éviction permanente, défilement en accordéon ;
  //   64 px -> ~5 lignes  -> ~25 Mo, confortablement en dessous.
  // Un curseur laissait donc le joueur choisir une valeur qui casse l'affichage.
  // La taille est figée à `kMvpSpritePx` (features/windows/mvp_tracker_window.cc).
  // Animer le sprite du rang. Éteint par défaut, et c'est un choix : quatre-vingts
  // silhouettes qui bougent en même temps font de la table un aquarium, et le
  // regard n'y accroche plus la ligne qu'on cherche.
  bool animate_sprites = false;
  // Champ de filtre au-dessus de la table. Éteint par défaut : il mange une
  // ligne de hauteur, et on ne cherche pas un MVP précis à chaque ouverture.
  bool show_filter   = false;
  // ── Les LIGNES DÉTACHÉES ───────────────────────────────────────────────────
  // Une ligne sortie du carnet par glisser-déposer vit seule à l'écran et
  // survit à la fermeture du carnet. Ces deux couleurs les habillent ; leur
  // canal ALPHA est l'opacité, ce qui permet un fond presque transparent sous
  // un texte resté lisible — c'est ce qu'on veut pour « discret », un texte
  // effacé en même temps que son fond ne servirait plus à rien.
  float line_text_col[4] = {0.96f, 0.93f, 0.82f, 1.00f};
  float line_bg_col[4]   = {0.04f, 0.04f, 0.06f, 0.55f};
  // Le sprite dans une ligne détachée. Réglage À PART de celui de la table, et
  // c'est voulu : une ligne posée sur le décor du jeu cherche la discrétion,
  // là où la table cherche à faire reconnaître un monstre d'un coup d'œil.
  bool  line_show_sprite = true;
};

class MvpTracker : public Plugin {
 public:
  MvpTracker();

  const char* name() const override { return "MvpTracker"; }

  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  MvpTrackerConfig& config() { return cfg_; }
  const MvpTrackerConfig& config() const { return cfg_; }

  const std::vector<mvp::Slot>& slots() const { return slots_; }
  const std::unordered_map<uint16_t, mvp::Obs>& observations() const { return obs_; }
  const std::vector<uint16_t>& favorites() const { return favorites_; }
  const mvp::Group& group() const { return group_; }

  const mvp::Slot* FindSlot(uint16_t slot_id) const;
  // Le créneau désigné par sa CLÉ STABLE `(mob_id, carte)` — la seule qui
  // survive à un redémarrage du serveur, `slot_id` n'étant qu'un rang dans un
  // registre reconstruit. C'est donc elle que transporte tout ce qui sort du
  // client : une ligne détachée, un favori en base, un lien `<MVPL>` de chat.
  // `mob_id` à 0 = créneau scripté, où la carte suffit à désigner.
  const mvp::Slot* FindSlotFor(uint16_t mob_id, const char* map) const;
  const mvp::Obs*  FindObs(uint16_t slot_id) const;
  bool IsFavorite(uint16_t slot_id) const;

  // Heure du SERVEUR, dérivée du décalage relevé au dernier paquet. Toute
  // durée affichée passe par ici.
  int64_t ServerNow() const;
  bool clock_known() const { return clock_known_; }

  // La fenêtre d'un créneau, en instants UNIX serveur. `exact` non nul écrase
  // les deux bornes : le miroir a été payé. Rend false si rien n'est connu.
  bool Window(uint16_t slot_id, int64_t* from, int64_t* to, bool* exact) const;

  // Invitation en attente (0 = aucune) et le nom du groupe invitant.
  uint32_t pending_invite() const { return invite_id_; }
  const char* pending_invite_name() const { return invite_name_; }
  void clear_pending_invite() { invite_id_ = 0; invite_name_[0] = '\0'; }

  // Dernier code de retour reçu, et l'instant local de sa réception : la fenêtre
  // l'affiche quelques secondes puis l'oublie.
  uint8_t last_result() const { return last_result_; }
  unsigned last_result_ms() const { return last_result_ms_; }

  // Le catalogue a-t-il été reçu ? Tant que non, la fenêtre le dit au lieu de
  // montrer une table vide, qui voudrait dire « rien à chasser ».
  bool catalog_known() const { return catalog_known_; }

  // ── Commandes ─────────────────────────────────────────────────────────────
  void RequestSnapshot();
  void CreateGroup(const char* name_utf8);
  void DissolveGroup();
  void InviteMember(const char* char_name_utf8);
  void AcceptInvite();
  void DeclineInvite();
  void LeaveGroup();
  void KickMember(const char* char_name_utf8);
  void SetFavorite(uint16_t slot_id, bool on);
  // La SAISIE : ce qu'un joueur affirme. `tomb_x/y` à -1 quand on ne sait pas
  // où il est mort — c'est le cas de la saisie à la main, mais pas de l'import
  // d'un lien `<MVPL>`, qui transporte la tombe quand son auteur l'avait.
  void ReportManual(uint16_t slot_id, int64_t kill_time, int16_t tomb_x = -1,
                    int16_t tomb_y = -1);

 private:
  void Send(uint8_t cmd, uint32_t a, uint32_t b, const char* text_utf8);
  void HandleState(const uint8_t* data, uint16_t len);
  void HandleGroup(const uint8_t* data, uint16_t len);
  void NoteServerTime(int64_t server_time);

  MvpTrackerConfig cfg_;

  std::vector<mvp::Slot> slots_;
  std::unordered_map<uint16_t, mvp::Obs> obs_;
  std::vector<uint16_t> favorites_;
  mvp::Group group_;

  bool     catalog_known_ = false;
  bool     clock_known_   = false;
  int64_t  clock_offset_  = 0;   // heure serveur - heure locale, en secondes

  uint32_t invite_id_     = 0;
  char     invite_name_[32] = {};

  uint8_t  last_result_    = 0;
  unsigned last_result_ms_ = 0;
};
