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
// 🔴 CE QUI BORNE LA RÈGLE : un code n'est rejouable que s'il lit sa matière
// dans `CGameMode` — c'est-à-dire dans ce que NOUS posons (`gm+0x2E0`, le
// vecteur). Le code **34** (« Afficher le GID dans le chat ») ne le fait pas :
// il formate son nom depuis une std::string GLOBALE de travail (`0x015FF8DC`,
// partagée avec les signets du livre et d'autres écrans), que seule la
// construction du LIBELLÉ par `GameMode_ShowEntityContextMenu` remplit avec le
// bon nom — or ce menu-là, on le tue. Rejoué, il aurait affiché le bon AID sous
// le nom qu'un autre écran avait laissé là. Mesuré au désassemblage, consigné
// dans docs §10.4. À vérifier pour tout code qu'on ajoute ici.
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
// 🔴 Les unités de compétence, le pet d'autrui et les entités non classées
// n'ont AUCUNE action de jeu : il n'en reste que de l'identité brute (GID, quad
// de pick). Ce menu-là est un outil de débogage, pas une fonctionnalité de
// joueur — il ne s'ouvre que sous le réglage staff, et les identifiants
// (AID/GID) ne figurent que dans la section staff, quelle que soit la cible.
//
// ── Le SOUS-MENU « Outils du staff » ─────────────────────────────────────────
// Tout ce qui est réservé au staff vit dans UN sous-menu, pas à plat : il y a
// désormais une vingtaine d'entrées, et les mêler aux six actions de joueur
// aurait noyé « Proposer un échange » au milieu de « Bannir le compte ». Le
// sous-menu se replie tout seul pour qui n'est pas staff — il n'est même pas
// construit (cf. `IsStaff()` dans BuildItems).
//
// ── Outillage JOUEUR : ce que faisait le NPC `#gmclicdroit` ──────────────────
// 🔴 Moonlight détournait CZ 0x02d6 (« voir l'équipement ») : au-delà du niveau
// de groupe 80, le serveur n'affichait pas l'équipement mais ouvrait un NPC
// caché, `#gmclicdroit`, dont le dialogue servait de menu GM. Deux dégâts : le
// staff était le SEUL à ne pas pouvoir regarder un équipement, et le menu vivait
// dans un script que trois commandes disparues (`getuserid`, `getvote`,
// `getuniqueid`) avaient de toute façon fini par casser.
//
// Ces actions sont maintenant des entrées du sous-menu staff, et le paquet
// 0x02d6 a retrouvé son rôle. Elles partent en **CZ 0x0F2B**, où le SERVEUR
// résout le nom depuis l'AID puis rejoue l'atcommand correspondante : la
// permission de groupe (conf/groups.yml) reste la seule autorité, et son verdict
// — réussite comme refus — revient dans le chat par le canal des atcommands.
//
// ⚠ « Rendre muet » et « rendre la parole » sont DEUX entrées, pas une bascule.
// Le client ne sait pas si la cible porte SC_NOCHAT, et un joueur muet reste dans
// le monde à côté de celui qui clique : une bascule aurait rendu la parole à qui
// venait d'être puni par quelqu'un d'autre. `@mute 60` n'est d'ailleurs pas
// l'inverse d'`@unmute` — elle AJOUTE 60 minutes au compteur de manières.
//
// 🔴 La PRISON, en revanche, est UNE seule entrée. `pc_jail` téléporte sur
// `MAP_JAIL` : un joueur emprisonné n'est visible que depuis la prison, et on ne
// clique droit que sur ce qu'on a à l'écran. Dans le monde la cible n'est jamais
// emprisonnée, dans `sec_pri` elle l'est toujours — une entrée sur deux aurait
// donc toujours été morte. Le serveur lit SC_JAILED et rejoue `@jail` ou
// `@unjail`, chacune avec sa propre permission de groupe.
//
// « Asseoir / relever » est une bascule pour une autre raison encore : c'est
// l'atcommand maison qui l'est, et se tromper d'état n'y coûte rien.
//
// ⚠ Pas d'entrée « expulser » en 0x0F2B : le menu natif en a déjà une (code 28),
// et `clif_parse_GMKick` rejoue le même `@kick` avec les mêmes droits. On la
// rejoue plutôt que d'ouvrir un second chemin vers la même commande.
//
// ── Outillage NPC de l'ADMINISTRATEUR (niveau de groupe >= 99) ───────────────
// Trois actions de plus quand le NPC est visé par un compte admin : recharger le
// fichier de script d'où il vient, le décharger, le poser sur notre case. Ce sont
// `@reloadnpcfile`, `@unloadnpc` et `@npcmove` — mais sans avoir à savoir le nom
// de ce qu'on regarde, ni le fichier d'où il sort.
//
// 🔴 Elles partent en CZ 0x0F25, PAS en commande @ rejouée par le chat. Les trois
// atcommands résolvent leur cible par `npc_name2id`, dont la clé est `exname` —
// le nom UNIQUE du serveur. Le client, lui, ne connaît que le nom AFFICHÉ de la
// plaque, et les deux diffèrent dès qu'il y a un `#suffixe` ou un duplicate :
// une commande @ aurait échoué sur « This NPC doesn't exist » précisément là où
// l'outil sert le plus. Le GID, lui, est ce qu'on a sous le curseur. Quant au
// rechargement, il n'était même pas exprimable : ce qui se recharge est le
// FICHIER, dont le chemin ne vit que dans `nd->path`, côté serveur.
//
// 🔴 Seuil 99, pas 80 (`IsAdmin()`, pas `IsStaff()`). L'inspecteur de propriétés
// AFFICHE ; ces trois-là MODIFIENT le monde pour tous les joueurs connectés. Le
// serveur refait le test de son côté — le bouton absent n'est pas une garde.
//
// Le compte rendu vient du SERVEUR (`clif_displaymessage`), mot pour mot, dans le
// chat : c'est lui qui sait si le script s'est rechargé.
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

  // Ouvre le menu sur une entité désignée AUTREMENT QUE PAR LA SOURIS du monde :
  // aujourd'hui, le clic droit sur un cadre du HUD de cible, qui se comporte
  // comme l'entité elle-même. `cat` est la catégorie de pick (0 = acteur).
  //
  // 🔴 Saute les gardes de contexte du clic droit du monde (état des boutons,
  // quad, neutralisation) : elles servent à décider si un clic nous appartient,
  // et celui-ci nous appartient par construction — ImGui l'a déjà retiré au jeu.
  // Saute aussi « toutes les entités » : sur le HUD, le geste est sans
  // ambiguïté, et le refuser laisserait un clic droit mort sans dire pourquoi.
  //
  // 🔴 À appeler HORS frame ImGui : elle lit des structures natives et construit
  // les entrées. Rend true si un menu va s'ouvrir.
  bool OpenForEntity(void* game_mode, uint32_t aid, uint32_t job, int cat);

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
    // 🔴 La catégorie 1 du quad de pick signifie « objet au sol », et rien
    // d'autre (RE live 2026-08-19) : seul `CItem_SubmitNameplateQuad`
    // (0x00d1da70) l'écrit — les vrais NPC passent par le producteur de
    // CActorSprite (cat 0/3/4). Ce que la doc appelait
    // « NpcActor_SubmitNameplateQuad » était en réalité la méthode de CItem :
    // classé `kNpc`, un objet au sol recevait « Interagir » et l'outillage
    // admin des NPC.
    kGroundItem,  // un objet au sol (CItem)
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
    // Faire de cette entité la CIBLE, sans l'attaquer.
    //
    // 🔴 Passe par le ciblage CLAVIER de TargetFrame et non par un clic rejoué :
    // `CGameMode+0xF4` n'est écrite qu'au clic sur une cible « valide », et un
    // ALLIÉ n'en est pas une — le client ne cible pas les membres de son propre
    // groupe à la souris. C'est d'ailleurs pour ça que cette entrée existe : elle
    // comble un geste que le jeu ne sait pas faire.
    kTargetEntity,
    // ── Les gestes de GROUPE et d'AMITIÉ ────────────────────────────────────
    // Ils vivent ici et non dans la fenêtre Groupe/Amis : ce sont des actions
    // sur un JOUEUR, elles ont leur place là où l'on clique un joueur. Les avoir
    // laissées ailleurs obligeait la fenêtre à ouvrir son propre menu juste pour
    // les proposer — une étape de plus pour arriver au même endroit.
    // L'exécution est déléguée à PartyFriendWindow, qui possède déjà les
    // commandes et leurs gardes.
    kPartyMakeLeader,  // céder le commandement à ce membre
    kPartyKick,        // l'expulser du groupe
    kFriendRemove,     // le retirer de la liste d'amis
    kInspect,       // ouvre l'inspecteur de propriétés (staff)
    // Les deux gestes « chuchoter », séparés parce qu'ils n'ont pas la même
    // portée : le premier PRÉPARE l'envoi dans la barre de chat, le second ouvre
    // une conversation à part. Tous deux retombent sur le code natif 20 quand le
    // chat moderne est éteint — c'est pour ça qu'ils portent AUSSI ce code.
    kWhisperBar,    // écrit le nom dans la box « Pseudo » de la barre principale
    kWhisperWindow, // ouvre la conversation 1:1
    // ── Poser un lien dans la barre de chat ─────────────────────────────────
    // « Linker » ce qu'on a sous le curseur. Les deux ne portent PAS la même
    // balise, parce qu'ils ne désignent pas la même chose :
    //  · un MONSTRE se désigne lui-même (`<MOBL>`, avec son nom et son rang) —
    //    le lecteur ouvre sa fiche, voit ses drops, ses spawns ;
    //  · un PNJ n'a pas d'identité qui voyage : ni fiche, ni id stable d'un
    //    redémarrage à l'autre. Ce qu'on partage de lui, c'est OÙ LE TROUVER,
    //    donc une recherche de navigation (`[PNJ: …]`).
    // Aucune des deux n'envoie quoi que ce soit : la balise est posée dans la
    // barre, le joueur relit et valide — comme le « Share » de la navigation.
    kChatLinkMob,
    kChatLinkNpc,
    // ── Outillage NPC, niveau de groupe >= 99 (cf. l'en-tête) ────────────────
    // Trois CZ 0x0F25, une par action. Elles ne portent AUCUN code natif : le
    // client n'a jamais rien su faire de tel.
    kNpcReloadFile, // recharger le fichier de script d'où vient ce NPC
    kNpcUnload,     // décharger ce NPC et ses duplicates (confirmation)
    kNpcMoveHere,   // le poser sur notre case
    // ── Outillage JOUEUR du staff (cf. l'en-tête) ───────────────────────────
    // Un CZ 0x0F2B par action, sans code natif : le client n'a jamais rien su
    // faire de tel — c'était un dialogue NPC. L'ordre suit celui du menu, du
    // plus anodin au plus définitif.
    kPlayerComeHere,    // le faire marcher jusqu'à nous
    kPlayerSitStand,    // @sitstand : bascule assis / debout
    kPlayerEventPoints, // ouvre la saisie du delta, puis l'envoie
    kPlayerMute,        // @mute 60
    kPlayerUnmute,      // @unmute
    kPlayerJail,        // @jail / @unjail, le serveur choisit le sens
    kPlayerNuke,        // @nuke
    kPlayerBlock,       // @block : bannit le COMPTE (confirmation)
    // ── Objet au sol ─────────────────────────────────────────────────────────
    // Le natif n'a JAMAIS eu de menu ici (son clic droit ne fait rien sur un
    // CItem) : tout est local. Les entrées à IDENTITÉ (description, lien,
    // alootid, commandes @) sont grisées tant que l'objet n'est pas identifié —
    // un équipement tombé d'un monstre ne dit pas encore ce qu'il est, et le
    // menu n'a pas à le dire à sa place.
    kPickupItem,    // rejoue le clic gauche natif : approche puis ramassage
    kItemDesc,      // fenêtre de description (différée, comme un lien de chat)
    kChatLinkItem,  // pose la balise <ITEML> dans la barre de chat
    kAlootToggle,   // case « ramassage automatique » (liste @alootid du compte)
    kCmdItemInfo,   // @iteminfo <id> par le pipeline complet du chat
    kCmdWhoDrops,   // @whodrops <id> — idem
    // ── Fenêtre de cible ─────────────────────────────────────────────────────
    // Masque la fenêtre de cible POUR CETTE ENTITÉ. Elle revient d'elle-même au
    // prochain changement de cible : c'est un « pas celle-ci », pas un
    // interrupteur global (celui-là est dans les réglages).
    kHideTargetFrame,
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
    bool  danger    = false;          // rouge : l'action retire quelque chose au monde
    // Passe par la modale de confirmation au lieu d'être jouée tout de suite.
    bool  confirm   = false;
    bool  disabled  = false;          // grisée : l'action n'a pas de sens ici
    // Case à cocher plutôt qu'une action : elle bascule un ÉTAT et ne ferme donc
    // pas le menu (on veut pouvoir cocher puis parler au NPC dans la foulée).
    bool  toggle    = false;
    bool  checked   = false;
    // Cette ligne va dans le SOUS-MENU « Outils du staff » au lieu du corps du
    // menu. Les entrées marquées se suivent sans trou : le rendu ouvre le
    // sous-menu à la première et le referme à la dernière.
    bool  submenu   = false;
    std::string tip;                  // infobulle, vide = aucune
  };

  // Dessine la ligne `index`. Rend true si le menu doit se FERMER (une action a
  // été choisie) — une case à cocher, elle, ne ferme rien.
  // Extraite pour que le corps du menu et le sous-menu staff dessinent leurs
  // lignes avec le MÊME code : deux copies auraient divergé au premier ajout.
  bool DrawItem(size_t index);

  // Construit `items_` d'après la cible déjà retenue (kind_, target_*).
  void BuildItems();
  Kind ClassifyTarget(void* game_mode, uint32_t aid, uint32_t job, int category) const;

  // Le corps commun aux deux entrées (clic droit du monde, clic droit du HUD) :
  // relever ce qu'il faut savoir de la cible, construire les entrées, demander
  // l'ouverture. Extrait pour que les deux chemins ne divergent pas.
  void FillTargetAndOpen(void* game_mode, uint32_t aid, uint32_t job, int cat,
                         Kind kind);
  void Choose(const Item& item);
  // Le popup du menu lui-même. Extrait d'OnRenderUI pour que la modale de
  // confirmation, elle, soit dessinée à CHAQUE frame — y compris celles où le
  // menu est fermé, c'est-à-dire toutes celles où la question est posée.
  void DrawPopup();
  // Le titre-identifiant de la modale de confirmation, pour la question `which`.
  //
  // 🔴 Le titre VISIBLE dépend de la question, l'identité ImGui NON : seul ce qui
  // suit `###` est haché. Une seule modale sert donc les trois questions, et les
  // trois endroits qui la nomment (ouverture, dessin, test « déjà ouverte »)
  // s'accordent sans avoir à passer le même libellé.
  //
  // Membre et non fonction libre : elle nomme `Local`, qui est privé.
  static const char* ConfirmModalId(Local which);
  // La confirmation des actions destructrices. Dessinée à la RACINE de la frame,
  // pas dans le popup du menu : celui-ci est fermé au moment du clic, et une
  // modale ouverte depuis un popup mourant avec lui ne s'afficherait jamais.
  void DrawConfirmModal();
  // Bascule le blocage de la cible courante, le dit dans le chat et demande la
  // sauvegarde des réglages.
  void ToggleNpcBlock(uint32_t gid);
  // Bascule la présence de cet item dans la liste de ramassage automatique
  // (@alootid). La liste vit dans MoonlightUi, qui la tient synchronisée avec le
  // serveur — la même que l'overlay de description et le menu des liens.
  void ToggleAloot(uint32_t nameid);

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
  // Objet au sol : relevés dans le CItem à l'ouverture (liste actorMgr+0x18).
  // `0` = introuvable — l'objet a pu disparaître entre le pick et l'ouverture.
  uint32_t target_item_id_        = 0;      // nameid (CItem+0x178)
  bool     target_item_identified_ = false; // CItem+0x174
  std::vector<Item> items_;

  // ── Action en attente, rejouée par FlushPending ───────────────────────────
  int      pending_code_  = 0;       // code natif à rejouer, 0 = aucun
  uint32_t pending_aid_   = 0;
  Local    pending_local_ = Local::kNone;
  uint32_t pending_arg_   = 0;       // job (fiche de monstre), aid (parler)…
  // Le champ `param` du CZ 0x0F2A. Signé, contrairement à `pending_arg_` : le
  // seul usage à ce jour est un delta de points d'event, qui se retire autant
  // qu'il se donne.
  int32_t  pending_param_ = 0;

  // ── Confirmation en attente ───────────────────────────────────────────────
  // La cible est recopiée ici : le menu peut se rouvrir sur autre chose pendant
  // que la modale est à l'écran, et c'est bien le NPC affiché dans la question
  // qui doit être déchargé.
  bool        confirm_request_ = false;  // ouvrir la modale à cette frame
  Local       confirm_local_   = Local::kNone;
  uint32_t    confirm_aid_     = 0;
  std::string confirm_name_;
  // Le delta saisi dans la modale des points d'event. Remis à zéro à chaque
  // ouverture : un don ne se répète pas par inadvertance parce que le champ
  // avait gardé la valeur du précédent.
  int         confirm_points_  = 0;

  bool all_entities_  = false;
  bool self_menu_     = true;
  bool staff_extras_  = true;
  bool npc_block_enabled_ = true;
};
