#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "features/plugin.h"

// ── EntityInspector ──────────────────────────────────────────────────────────
//
// « Propriétés » d'une entité du monde : l'outil du STAFF pour répondre à la
// question « qu'est-ce que je suis en train de regarder, exactement ». Il
// s'ouvre depuis la section staff du menu contextuel
// (features/windows/entity_context_menu.{h,cc}), sur N'IMPORTE QUELLE cible —
// joueur, monstre, NPC, pet, homoncule, mercenaire, unité de compétence, objet
// au sol, entité non classée. C'est justement sur les cas qu'on n'attend pas
// qu'un inspecteur sert.
//
// ── Ce qu'il montre, et d'où ça vient ────────────────────────────────────────
// Quatre sources, séparées à l'écran parce qu'elles ne mentent pas de la même
// façon :
//   1. le QUAD DE PICKING — ce que le hit-test souris a désigné : GID, classe,
//      catégorie. C'est la vérité de l'instant du clic, pas de maintenant ;
//   2. la PLAQUE DE NOM (`CNameInfo`, dictionnaire `GameMode+0x160`) — nom,
//      groupe, guilde, rang, titre, id de titre. Elle vient du SERVEUR
//      (ZC 0x0A30) et peut manquer : le client demande alors le nom et
//      l'affiche plus tard. Un champ vide ici veut dire « pas encore reçu »,
//      pas « vide côté serveur » ;
//   3. l'ACTEUR (`ActorList_FindByGID`) — l'objet C++ vivant : son adresse, sa
//      vtable, son état, sa position monde. Il peut ne pas exister alors que le
//      pick a réussi (entité détruite entre le clic et la lecture) ;
//   4. le SERVEUR, sur demande explicite — voir le volet plus bas.
//
// 🔴 Les trois premières sont RELUES À CHAQUE FRAME. Un inspecteur qui fige un
// instantané ne sert à rien pour observer une entité qui bouge — et c'est
// précisément ce qu'on lui demande (voir une position dériver, un état d'acteur
// changer). Seuls le GID, la classe et la catégorie restent ceux du clic : ils
// identifient la cible. Le volet serveur, lui, ne bouge qu'à la demande.
//
// ⚠ AUCUNE ÉCRITURE, nulle part. Cette fenêtre lit — la mémoire du client, puis
// la réponse du serveur — et n'écrit rien, ni en mémoire ni en base. Les deux
// seuls paquets qu'elle puisse produire sont des DEMANDES, toutes deux bornées :
// le `CZ_REQNAME` que le client fait déjà tout seul en survolant (throttlé, et
// seulement tant que le nom manque — voir `kNameRetryMs`), et le CZ 0x0F22
// ci-dessous, qui ne part que sur clic.
//
// ── Le volet SERVEUR : CZ 0x0F22 -> ZC 0x0F23 ────────────────────────────────
// Le bouton « Demander au serveur » COMPLÈTE le même panneau — il n'ouvre pas
// une seconde fenêtre. Ce qu'il rapporte est ce que le client ne peut PAS
// savoir : de quel fichier de script vient ce NPC, quel spawn a posé ce monstre,
// qui a lancé cette unité de compétence, à qui est réservé cet objet au sol.
//
// 🔴 La demande est MANUELLE. Ouvrir l'inspecteur n'émet aucun paquet : on
// l'ouvre souvent pour lire un GID ou une adresse, et un panneau qui interroge
// le serveur à chaque clic droit ferait du bruit pour rien.
//
// 🔴 La réponse est une LISTE CLÉ/VALEUR, pas une structure : ce qu'il y a à
// dire dépend du type de l'entité, et le client n'a rien à en connaître — il
// dessine ce qu'il reçoit (valeur vide = titre de section). Ajouter une
// propriété est donc un changement SERVEUR seul : rien à recompiler ici.
// Corollaire assumé : ces libellés-là s'affichent tels quels, sans passer par le
// catalogue de traduction — comme les messages serveur relayés au chat.
//
// ⚠ Le gate staff est SERVEUR (niveau de groupe >= 80) : le bouton absent ne
// protège rien, un client se modifie. Un compte non-staff reçoit « refusé ».
//
// ── Gate ─────────────────────────────────────────────────────────────────────
// `IsStaff()` (niveau de groupe serveur >= 80), revérifié à CHAQUE frame et pas
// seulement à l'ouverture : perdre le staff en cours de session doit refermer la
// fenêtre, pas la laisser ouverte sur des adresses mémoire.

class EntityInspector : public Plugin {
 public:
  EntityInspector();

  const char* name() const override { return "EntityInspector"; }

  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  // 🔴 FIL RÉSEAU : on copie, rien de plus (cf. features/net_inbox.h).
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Ouvre (ou recible) l'inspecteur. Les trois premiers arguments viennent du
  // quad de picking ; `family` est le classement du menu contextuel (« Joueur »,
  // « Monstre »…), affiché tel quel — c'est LUI qui a tranché, on ne reclasse
  // pas la cible une deuxième fois avec une deuxième logique.
  void Open(uint32_t gid, uint32_t job, int category, const char* family);

  bool IsOpen() const { return open_; }

 private:
  // Tout ce qui se relit à chaque frame, en une passe. Champs BRUTS (tableaux de
  // caractères, pas de std::string) : le remplissage tourne sous SEH, et une
  // fonction qui doit dérouler des objets C++ ne peut pas porter de __try.
  struct Snapshot {
    // ── Plaque de nom ────────────────────────────────────────────────────────
    bool in_dict   = false;  // le GID a une entrée dans le dictionnaire
    bool resolved  = false;  // …et elle est renseignée (CNameInfo+0x98 == 1)
    char name[64]  = {};     // encodage du FIL, à convertir à l'affichage
    char party[64] = {};
    char guild[64] = {};
    char rank[64]  = {};
    char title[64] = {};
    char extra[64] = {};
    int  title_id  = 0;

    // ── Acteur ───────────────────────────────────────────────────────────────
    bool     actor_found = false;
    uint32_t actor_addr  = 0;
    uint32_t actor_vt    = 0;
    uint32_t actor_gid   = 0;   // acteur+0x110, pour recouper avec le pick
    uint8_t  actor_type  = 0;   // acteur+0x314
    uint32_t guild_id    = 0;   // vtable+0xC4
    float    pos_x = 0.0f, pos_y = 0.0f, pos_z = 0.0f;
    float    height_off = 0.0f;  // acteur+0x3f4 (le levier du saut)
    bool     cell_ok = false;
    int      cell_x = 0, cell_y = 0;
    bool     own_cell_ok = false;
    int      own_cell_x = 0, own_cell_y = 0;
  };

  // Le dictionnaire de noms (la seule source qui puisse produire un paquet).
  void  Refresh(Snapshot* out);
  // L'acteur vivant. Séparée de `Refresh` — et de tout ce qui manipule un objet
  // C++ — parce qu'elle porte un __try : MSVC refuse (C2712) un __except dans
  // une fonction qui doit dérouler des objets.
  void  ReadActor(Snapshot* out);
  void  DrawBody(const Snapshot& snap);
  // La section « Serveur » : le bouton, l'attente, le refus, ou les paires.
  void  DrawServerSection();
  // Émet CZ 0x0F22. Sans effet si une demande est déjà en vol depuis moins de
  // 5 s — un bouton se clique deux fois.
  void  RequestServer();
  // Le panneau, en texte brut, pour le presse-papier — un rapport qui se colle
  // dans un ticket ou dans IDA. Le volet serveur y est inclus quand il est là.
  std::string BuildReport(const Snapshot& snap) const;

  bool open_       = false;
  bool need_focus_ = false;

  // La cible, telle que le PICK l'a désignée. Immuable jusqu'au prochain Open().
  uint32_t    gid_ = 0;
  uint32_t    job_ = 0;
  int         cat_ = -1;
  std::string family_;

  // Le nom a-t-il déjà été résolu une fois ? Tant que non, la lecture du
  // dictionnaire passe par le chemin qui DEMANDE au serveur, d'où le throttle.
  bool     name_resolved_    = false;
  unsigned last_name_request_ = 0;

  // ── Le volet serveur (ZC 0x0F23) ──────────────────────────────────────────
  enum class Fetch {
    kIdle,     // rien demandé — l'état d'ouverture, et c'est voulu
    kPending,  // demande en vol
    kReady,    // paires reçues
    kMissing,  // le serveur ne connaît aucune entité sous ce GID
    kDenied,   // refusé : le compte n'est pas staff CÔTÉ SERVEUR
  };
  // Une propriété telle que le serveur l'a écrite. `value` vide = titre de
  // section : c'est la convention du paquet, et elle vit ici telle quelle.
  struct Prop {
    std::string key;
    std::string value;
  };
  Fetch             server_state_ = Fetch::kIdle;
  std::vector<Prop> server_props_;
  // Le GID auquel répondent `server_props_`. Une réponse qui arrive après un
  // reciblage ne doit pas s'afficher sous la nouvelle cible.
  uint32_t          server_gid_ = 0;
  unsigned          server_requested_tick_ = 0;
};
