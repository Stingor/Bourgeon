#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "features/plugin.h"

// ── EntityContextMenu ────────────────────────────────────────────────────────
//
// Le menu du CLIC DROIT sur une entité du monde, en ImGui. Remplace celui du
// client — RE complète dans docs/entity_context_menu_re.md.
//
// ── Ce qu'on intercepte, et pourquoi CELUI-LÀ ────────────────────────────────
// Le menu natif est une fenêtre générique, `UIMenuWnd` id 0x12, que DIX autres
// fenêtres utilisent pour leurs propres menus (groupe, guilde, chat, quêtes,
// sélection de personnage…). La tuer casserait les leurs. Le point d'entrée
// propre est donc `GameMode_ShowEntityContextMenu` (0x00c6e990) : une SEULE
// xref, appelée chaque frame par la passe souris du monde, et c'est elle — et
// elle seule — qui produit le menu des entités. Détournée, le menu natif du
// monde ne naît jamais ; tous les autres menus du client continuent de marcher.
//
// ── Rejouer plutôt que réécrire ──────────────────────────────────────────────
// 🔴 On ne refabrique AUCUN paquet. Le client range un « code d'action » par
// ligne de menu dans un `std::vector<int>` à `CGameMode+0x1CC`, et le clic
// revient par `CMode::SendMsg(24, index)` qui relit ce vecteur. Pour exécuter
// une entrée on reproduit donc les trois gestes du natif :
//     gm[0x2E0] = aid;                       // la cible
//     gm[0x1D0] = gm[0x1CC];                 // vider le vecteur
//     StdVectorInt_PushBack(gm+0x1CC, code); // une seule entrée
//     SendMsg(gm, 24, 0);                    // « la ligne 0 a été cliquée »
// Toutes les gardes natives restent jouées : dialogue NPC en cours, surcharge,
// droits de guilde, boîtes de confirmation (nourrir le pet), consentement PvP.
// Le jour où le client change un paquet, on suit sans rien toucher.
//
// 🔴 HORS frame ImGui. Certaines de ces actions ouvrent une modale BLOQUANTE
// (nourrir le pet / l'homoncule) qui relance le tick du mode : l'action choisie
// est empilée pendant le rendu et rejouée depuis `Bourgeon::OnProcessInput`,
// comme WeaponRefineWindow et MakeItemWindow.
//
// ── 🔴 Le but n'est PAS de reproduire le natif ───────────────────────────────
// C'est de combler ses manques. « Le client faisait comme ça » n'est donc jamais
// une justification à soi seule : c'est une observation, dont il faut ensuite
// décider si elle sert le joueur. Là où le natif laissait cliquer une action que
// le serveur allait refuser, on grise et on dit pourquoi (invitation à quelqu'un
// déjà en groupe ou en guilde, alliance avec un joueur sans guilde, ami déjà
// dans la liste). Là où il ne disait pas QUI était visé, on l'affiche.
//
// ── Ce que le natif ne faisait pas ───────────────────────────────────────────
// Le client n'ouvrait de menu que sur un JOUEUR, son pet, son homoncule ou son
// mercenaire. Mobs, NPC, unités de compétence et objets au sol n'en ont jamais
// eu. C'est l'espace qu'occupe l'option « toutes les entités » : interagir avec
// un NPC, fiche de monstre, copier le nom.
//
// 🔴 Les unités de compétence, les objets au sol et les entités non classées
// n'ont AUCUNE action de jeu : il n'en reste que de l'identité brute (GID, quad
// de pick). Ce menu-là est un outil de débogage, pas une fonctionnalité de
// joueur — il ne s'ouvre que sous le réglage staff, et les identifiants
// (AID/GID) ne figurent que dans la section staff, quelle que soit la cible.
//
// ── Rendre un NPC de service SOURD au clic gauche ────────────────────────────
// Les NPC de la capitale (kafra, job master, styliste, warpers…) se tiennent au
// milieu du passage : on les clique en voulant marcher, et le dialogue s'ouvre.
// Moonlight répondait à ça côté SERVEUR avec `@npcblock` (moon/atcommands.npc :
// un masque de bits `#BlockMoonNpc` que CINQ scripts relisaient à la main). La
// base des identifiants fixes (`moon/npc_fixed_id.yml`) permet de rendre ça au
// client : ces NPC gardent le même GID d'un boot à l'autre, dans la plage
// réservée 3000000..3000099, donc le client peut les reconnaître tout seul.
//
// Une case à cocher apparaît alors dans le menu, et elle SEULE décide : rien ne
// part au serveur. Le blocage se joue dans `GameMode_RouteHoverAndClick`
// (0x00c756a0), sur les seules frames où un bouton AGIT, et il y fait DEUX
// choses — l'une ne suffisait pas :
//   · un quad de pick NUL, pour que le natif ne route rien vers le NPC : ni
//     dialogue, ni ordre d'approche. Filtrer CZ_CONTACTNPC à l'envoi n'aurait
//     PAS suffi — le clic sur un NPC de catégorie 1 passe par `OnMsg(18)` de
//     l'IA, qui fait MARCHER le personnage jusqu'au NPC AVANT d'émettre : le
//     joueur aurait traversé la place pour n'obtenir rien ;
//   · un retour « clic consommé » (non nul et ≠ 2), la convention que teste
//     `GameMode_GroundClick_RequestMove` (`param_1 && param_1 != 2 -> return`),
//     pour que le clic ne se transforme pas non plus en déplacement.
// Résultat : le clic gauche sur un NPC bloqué n'émet STRICTEMENT AUCUN paquet.
//
//   · le survol n'est pas touché (états 0 et 3) : plaque de nom et curseur
//     restent ceux du client — y compris le curseur « dialogue », qui était déjà
//     là du temps de `@npcblock` et n'est donc pas un défaut à corriger ;
//   · le ciblage AU SOL est exclu (`CGameMode+0x408 == 1`) : viser une zone et
//     cliquer la case d'un NPC bloqué lance bien le sort ;
//   · le menu du clic droit, lui, est construit AVANT dans la même passe souris
//     (`ShowEntityContextMenu` précède `RouteHoverAndClick`) : il continue de
//     s'ouvrir, ce qui est indispensable — c'est de là qu'on décoche.
//
// ⚠ Purement CLIENT : un script à `OnTouch` se déclenchera toujours en marchant
// dessus, et la case ne suit pas le joueur d'une machine à l'autre.
//
// OPT-IN : « ctxmenu_imgui », membre du groupe « Interface moderne » (défaut
// OFF, comme les autres). Coupé, le détour repasse la main au natif au premier
// clic — rien n'est masqué ni détruit, donc aucun état à restaurer.

class EntityContextMenu : public Plugin {
 public:
  EntityContextMenu();

  const char* name() const override { return "EntityContextMenu"; }

  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Rejoue hors frame ImGui l'action choisie (cf. l'en-tête). Appelée par
  // Bourgeon::OnProcessInput.
  void FlushPending();

  // Contenu de la section « Menu contextuel » du panneau Moonlight.
  // Rend true si un réglage a changé.
  bool DrawSettings();

  // « ctxmenu_imgui » : basculé en GROUPE par SetModernInterface. Défaut OFF.
  bool imgui_enabled_ = false;
  // « ctxmenu_all_entities » : ouvrir aussi sur les entités que le natif
  // ignorait (monstres, NPC). Défaut OFF — c'est un changement de comportement
  // de jeu, pas un habillage.
  bool& all_entities() { return all_entities_; }
  // « ctxmenu_self » : ouvrir le menu quand la cible, c'est SOI. Sous-réglage de
  // « toutes les entités » — le client n'ouvrait rien sur son propre personnage,
  // c'est ce lot-là qui l'a rendu possible. Défaut ON (le comportement livré),
  // mais le menu sur soi ne porte presque rien : décoché, ce clic droit repart
  // au client INTACT plutôt que d'être avalé par un menu d'une ligne.
  bool& self_menu() { return self_menu_; }
  // « ctxmenu_staff_extras » : ajouter la section staff — dont « Propriétés… »,
  // qui ouvre EntityInspector — ET ouvrir le menu sur TOUTES les entités, sans
  // attendre « toutes les entités » : monstre, NPC, unité de compétence, objet
  // au sol, non classée. Ce second effet n'est pas un raccourci de confort :
  // l'inspecteur sert d'abord sur les cibles inattendues, et le laisser derrière
  // un réglage de JOUEUR le rendait introuvable — clic droit sans effet, sans
  // rien qui dise pourquoi.
  // Sans effet pour un compte non-staff (IsStaff() garde de toute façon le
  // rendu). Défaut ON : le gate serveur suffit, l'interrupteur ne sert qu'à
  // dégonfler le menu.
  bool& staff_extras() { return staff_extras_; }

  // Appelé par le détour de GameMode_ShowEntityContextMenu. `quad` est le quad
  // de picking (ou nullptr), `blocked` le retour de la passe souris (non nul si
  // une fenêtre native est sous le curseur). Rend true si NOUS prenons la main
  // (le natif ne doit alors rien construire).
  bool OnNativeContextMenu(void* game_mode, const int* quad, int blocked);

  // ── Blocage du clic sur un NPC à identifiant fixe (cf. l'en-tête) ──────────

  // Ce GID est-il celui d'un NPC épinglé par `moon/npc_fixed_id.yml` ? La plage
  // est celle que le serveur se réserve (FIXED_NPC_NUM..FIXED_NPC_NUM_LAST dans
  // src/map/npc.hpp) et sous laquelle `npc_get_new_npc_id` n'alloue jamais : un
  // GID d'ici ne peut donc être ni un mob, ni un pet, ni un NPC dynamique. C'est
  // ce qui rend le test sûr sans rien demander au serveur.
  static bool IsFixedIdNpc(uint32_t gid);
  bool IsNpcClickBlocked(uint32_t gid) const;

  // Appelé par le détour de GameMode_RouteHoverAndClick : ce quad désigne-t-il
  // une entité dont l'action monde doit être ignorée CETTE frame ?
  // ⚠ Non const : c'est ici qu'est émis le rappel « ce NPC, c'est vous qui
  // l'avez bloqué », au premier clic avalé de la session.
  bool ShouldIgnoreWorldClick(const int* quad);

  // « ctxmenu_npc_block » : proposer la case, et l'appliquer. Défaut ON — c'est
  // le remplaçant de `@npcblock`, et sans NPC coché il ne fait rien.
  bool& npc_block_enabled() { return npc_block_enabled_; }

  // GID -> nom relevé au moment du blocage (UTF-8, purement informatif : il ne
  // sert qu'à nommer la ligne du panneau et le message de chat). Persisté par
  // moonlight_ui::Read/WriteBlockedNpcs.
  std::map<uint32_t, std::string> blocked_npcs_;

  // 🔴 NPC déjà expliqués CETTE session. Un blocage survit au fichier de
  // réglages : des semaines plus tard, le joueur clique une kafra qui ne répond
  // pas et n'a plus aucune raison de faire le lien avec une case cochée un soir
  // de mars. Le premier clic avalé de la session lui redit donc pourquoi — une
  // fois par NPC, sinon la ligne partirait à chaque clic.
  // Vidé en quittant le monde (login / choix de personnage), pas au changement
  // de carte : « une fois par session » veut dire une fois.
  std::set<uint32_t> warned_this_session_;

 private:
  // Ce que désigne le curseur, tel que le pick l'a rendu.
  enum class Kind {
    kNone,
    kSelf,        // soi-même
    kPlayer,      // un autre joueur
    kMonster,     // un monstre
    kNpc,         // un NPC de map (ou un portail)
    kPet,         // SON pet
    kHomunculus,  // SON homoncule
    kMercenary,   // SON mercenaire
    kSkillUnit,   // une unité de compétence posée
    kGroundItem,  // un objet au sol
    kOther,
  };

  // Une ligne du menu. `code` est le code d'action NATIF à rejouer (cf. l'en-tête) ;
  // `local` couvre ce que le client ne sait pas faire.
  enum class Local {
    kNone,
    kMonsterInfo,   // ouvre notre fiche de monstre
    kCopyName,      // presse-papier
    kCopyId,        // presse-papier
    kCopyPickInfo,  // AID + job + catégorie de pick (staff)
    kTalkToNpc,     // CZ_CONTACTNPC 0x0090
    kAttack,        // GameMode_PostActorClickAction
    kInspect,       // ouvre l'inspecteur de propriétés (staff)
    // Les deux gestes « chuchoter », séparés parce qu'ils n'ont pas la même
    // portée : le premier PRÉPARE l'envoi dans la barre de chat, le second ouvre
    // une conversation à part. Tous deux retombent sur le code natif 20 quand le
    // chat moderne est éteint — c'est pour ça qu'ils portent AUSSI ce code.
    kWhisperBar,    // écrit le nom dans la box « Pseudo » de la barre principale
    kWhisperWindow, // ouvre la conversation 1:1
  };

  // Le nom de la famille de la cible, tel qu'il s'affiche en tête du menu — et
  // tel qu'il part à l'inspecteur, qui ne reclasse donc pas la cible une seconde
  // fois avec une seconde logique.
  static const char* KindLabel(Kind kind);

  struct Item {
    std::string label;
    int   code      = 0;              // code natif, 0 = aucun
    Local local     = Local::kNone;
    bool  separator = false;          // séparateur AVANT cette ligne
    bool  staff     = false;          // affichée en couleur staff
    bool  disabled  = false;          // grisée : l'action n'a pas de sens ici
    // Case à cocher plutôt qu'une action : elle bascule un ÉTAT et ne ferme donc
    // pas le menu (on veut pouvoir cocher puis parler au NPC dans la foulée).
    bool  toggle    = false;
    bool  checked   = false;
    std::string tip;                  // infobulle, vide = aucune
  };

  // Construit `items_` d'après la cible déjà retenue (kind_, target_*).
  void BuildItems();
  Kind ClassifyTarget(void* game_mode, uint32_t aid, uint32_t job, int category) const;
  void Choose(const Item& item);
  // Bascule le blocage de la cible courante, le dit dans le chat et demande la
  // sauvegarde des réglages.
  void ToggleNpcBlock(uint32_t gid);

  // ── État du menu affiché ──────────────────────────────────────────────────
  bool     open_        = false;
  bool     request_open_ = false;   // ouvrir le popup à la prochaine frame
  // Horodatage de cette demande. La passe souris qui l'arme tourne même quand la
  // passe UI des modules ne tourne pas (interface masquée par F11) : sans
  // péremption, le menu resurgirait tout seul en réaffichant l'interface, sur une
  // entité cliquée il y a longtemps.
  unsigned request_tick_ = 0;
  uint32_t target_aid_  = 0;
  uint32_t target_job_  = 0;
  int      target_cat_  = -1;
  Kind     kind_        = Kind::kNone;
  std::string target_name_;
  // Ce que la cible est déjà, relevé à l'ouverture (joueurs uniquement) : sert à
  // GRISER les invitations sans objet plutôt qu'à les faire disparaître.
  bool     target_in_party_  = false;  // dans NOTRE groupe
  bool     target_has_party_ = false;  // dans un groupe, le nôtre ou un autre
  bool     target_is_friend_ = false;  // déjà dans notre liste d'amis
  bool     target_chat_blocked_ = false;  // déjà dans notre liste d'ignorés
  uint32_t target_guild_id_  = 0;
  std::vector<Item> items_;

  // ── Action en attente, rejouée par FlushPending ───────────────────────────
  int      pending_code_  = 0;       // code natif à rejouer, 0 = aucun
  uint32_t pending_aid_   = 0;
  Local    pending_local_ = Local::kNone;
  uint32_t pending_arg_   = 0;       // job (fiche de monstre), aid (parler)…

  bool all_entities_  = false;
  bool self_menu_     = true;
  bool staff_extras_  = true;
  bool npc_block_enabled_ = true;
};
