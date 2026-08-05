#pragma once

#include <cstdint>
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
  // « ctxmenu_staff_extras » : ajouter la section staff, ET ouvrir le menu sur
  // les entités purement diagnostiques (unité de compétence, objet au sol, non
  // classée). Sans effet pour un compte non-staff (IsStaff() garde de toute
  // façon le rendu). Défaut ON : le gate serveur suffit, l'interrupteur ne sert
  // qu'à dégonfler le menu.
  bool& staff_extras() { return staff_extras_; }

  // Appelé par le détour de GameMode_ShowEntityContextMenu. `quad` est le quad
  // de picking (ou nullptr), `blocked` le retour de la passe souris (non nul si
  // une fenêtre native est sous le curseur). Rend true si NOUS prenons la main
  // (le natif ne doit alors rien construire).
  bool OnNativeContextMenu(void* game_mode, const int* quad, int blocked);

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
  };

  struct Item {
    std::string label;
    int   code      = 0;              // code natif, 0 = aucun
    Local local     = Local::kNone;
    bool  separator = false;          // séparateur AVANT cette ligne
    bool  staff     = false;          // affichée en couleur staff
    bool  disabled  = false;          // grisée : l'action n'a pas de sens ici
    std::string tip;                  // infobulle, vide = aucune
  };

  // Construit `items_` d'après la cible déjà retenue (kind_, target_*).
  void BuildItems();
  Kind ClassifyTarget(void* game_mode, uint32_t aid, uint32_t job, int category) const;
  void Choose(const Item& item);

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
  bool     target_in_party_ = false;
  uint32_t target_guild_id_ = 0;
  std::vector<Item> items_;

  // ── Action en attente, rejouée par FlushPending ───────────────────────────
  int      pending_code_  = 0;       // code natif à rejouer, 0 = aucun
  uint32_t pending_aid_   = 0;
  Local    pending_local_ = Local::kNone;
  uint32_t pending_arg_   = 0;       // job (fiche de monstre), aid (parler)…

  bool all_entities_  = false;
  bool staff_extras_  = true;
};
