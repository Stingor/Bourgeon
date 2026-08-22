#pragma once

#include <cstdint>

#include "features/plugin.h"
#include "ui/hud_frame.h"  // ro::HudRect — la géométrie est celle du cadre

// ── TargetFrame ──────────────────────────────────────────────────────────────
//
// Le HUD de CIBLE : qui est l'entité que le joueur a désignée en dernier, et
// dans quel état elle est. Portrait, nom (avec party / guilde / rang, comme la
// plaque de nom du jeu), niveau, PV et SP en barres, race, élément et taille.
//
// ── Ce n'est pas une fenêtre, c'est un HUD ──────────────────────────────────
//
// Cinq CADRES LIBRES, dans l'esprit des barres de Basic Info et du portrait du
// joueur : chacun a sa position, sa taille, ses couleurs et son interrupteur, et
// se pose à la souris. Aucun n'a de barre de titre — on ne « range » pas un HUD,
// on le compose. Le verrou commun les fige et les rend transparents aux clics,
// l'aimantation les aligne sur la grille partagée.
//
// La mécanique du cadre est commune : `ui/hud_frame.h`.
//
// ── Ce qu'il suit, et pourquoi ce n'est PAS « ce qu'on attaque » ────────────
//
// La source est `CGameMode+0xF4` : l'AID de la DERNIÈRE ENTITÉ CLIQUÉE. C'est le
// champ qui pilote déjà, côté natif, le nom flottant ET la petite flèche blanche
// au-dessus de l'entité (le `CMousePointer` en `CGameMode+0xD4`, cf.
// docs/target_system_re.md §4 bis). Se brancher dessus donne pour rien la règle
// voulue : le HUD s'allume au clic et il RESTE quand on cesse d'attaquer —
// parce que la sélection, elle, reste.
//
// ⚠ La flèche du jeu, elle, ne reste PAS : elle est gardée par `CGameMode+0x28`,
// l'engagement SOURIS, qui retombe au premier mouvement hors de la cible. C'est
// le HUD qui la lui rend (`WantsSelectionMarker`), et non l'inverse.
//
// 🔴 NE JAMAIS utiliser `CGameMode+0xF0` à la place : c'est la cible de TRAVAIL,
// vidée dès que la souris bouge bouton relâché. Un HUD bâti dessus clignoterait
// à chaque geste.
//
// ── Ce qui l'éteint ─────────────────────────────────────────────────────────
//
//   · la cible CHANGE            -> le HUD suit la nouvelle (et se rallume s'il
//                                   avait été masqué à la main) ;
//   · la cible SORT D'AREA_SIZE  -> son acteur disparaît de la liste du client
//                                   (le serveur a envoyé ZC_NOTIFY_VANISH), et
//                                   le serveur répond « hors de portée » à notre
//                                   requête. 🔴 Elle est alors PERDUE : revenir
//                                   dans la zone ne la rallume pas, il faut la
//                                   re-désigner ;
//   · la cible SE CACHE          -> Hiding, Cloaking, ou @hide du staff. Même
//                                   verdict, et pour une raison plus forte :
//                                   garder le GID d'une entité invisible, c'est
//                                   offrir un détecteur ;
//   · le JOUEUR le masque        -> « Masquer la fenêtre de cible » dans le menu
//                                   contextuel de l'entité, pour CETTE cible.
//
// 🔴 La sélection native n'est purgée qu'au changement de map : elle survit à la
// mort de la cible et devient un FANTÔME. D'où la revalidation par
// `Actor_FindByGid` À CHAQUE FRAME — sans quoi le HUD resterait allumé sur un
// GID qui n'existe plus.
//
// ── D'où viennent les données ───────────────────────────────────────────────
//
//   1. LE CLIENT, gratuitement. Le dictionnaire de noms (`CGameMode+0x160`)
//      porte nom / party / guilde / rang. Pour un MONSTRE, le serveur détourne
//      ces champs (cf. `clif_name`) : party = « Lv. X | HP: Y% », guilde = race,
//      rang = élément. L'apparence du portrait, elle, se lit sur l'acteur.
//   2. LE SERVEUR, par CZ 0x0F29 -> ZC 0x0F2A. Il apporte ce que le client ne
//      peut pas savoir : le **SP** d'une entité tierce — aucun paquet du
//      protocole ne le transporte, et la barre du bas de la jauge d'entité
//      (`UIPcGage_SetGaugeBottom`) n'est jamais appelée qu'avec (0, 0) — plus
//      des PV EXACTS là où la plaque de nom ne donne qu'un pourcentage.
//
// 🔴 Aucun abonnement côté serveur : c'est nous qui redemandons, lentement, tant
// que le HUD est allumé. Rien à nettoyer quand le joueur se déconnecte.
//
// 🔴 GATE PVP, décidé côté serveur, et il ne porte plus sur les PV :
//   · les PV partent TOUJOURS — le HUD n'en montre qu'une JAUGE quand la cible
//     est un joueur, sans valeurs ni pourcentage. Sans cet envoi la jauge d'un
//     adversaire restait vide jusqu'au premier coup porté : le client ne sait
//     RIEN des PV d'un tiers, il ne les déduit que des dégâts qu'il voit passer,
//     et un Cloaking ou un @hide recrée l'acteur et remet ce compteur à zéro ;
//   · le SP et le NIVEAU, qui s'affichent EN CLAIR, restent réservés à la party
//     et à la guilde. Les barres restent alors vides — et le disent, plutôt que
//     d'afficher un zéro qui se lirait « il est mort ».
//
// OPT-IN, et INTERRUPTEUR MAÎTRE : « Activer le mode Ciblage + HUD ». Éteint,
// tout s'arrête — le HUD, la flèche, les raccourcis de ciblage, le clic sans
// attaque, les cadres qui agissent, les sorts qui partent sur la cible — et le
// panneau grise ses réglages. Un réglage cliquable et sans effet
// est le pire des deux mondes : on croit l'avoir posé, rien ne le dit.
// Clé de persistance « target_frame », membre du groupe « Interface moderne »
// (défaut OFF, comme tout le groupe).

class TargetFrame : public Plugin {
 public:
  TargetFrame();

  const char* name() const override { return "Target Frame"; }

  void OnRenderUI() override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Rejoue le clic reçu par un cadre. 🔴 Appelée par `Bourgeon::OnGameFrame`,
  // donc HORS frame ImGui : c'est un appel natif, qui peut ouvrir une boîte de
  // message (surcharge de poids) et relancer le tick du mode.
  void OnGameFramePulse();

  // ── Les cinq cadres ───────────────────────────────────────────────────────
  // Chacun est indépendant : position, taille, couleurs, interrupteur. Comme les
  // barres de Basic Info, dont ils partagent la mécanique et l'esprit.
  enum ElemId {
    kElemPortrait = 0,
    kElemName,
    kElemHp,
    kElemSp,
    kElemKind,
    kElemCount
  };

  // Suffixe de persistance + libellé, indexés par ElemId.
  static constexpr const char* kElemKeys[kElemCount] = {"portrait", "name", "hp",
                                                        "sp", "kind"};
  static constexpr const char* kElemLabels[kElemCount] = {
      "Portrait", "Nom", "PV", "SP", "Race / élément"};

  struct Elem {
    bool        show;
    // 🔴 La géométrie EST un `ro::HudRect` — pas quatre entiers qui lui
    // ressemblent : `ui/hud_frame` reçoit son adresse pour l'aimantation entre
    // frères et le déplacement en bloc, et une réinterprétation de champs
    // voisins serait une bombe à retardement au premier champ ajouté.
    ro::HudRect rect;
    float       bg[4];  // fond du cadre
    float       fg[4];  // texte, ou remplissage pour les deux barres
  };

  // ── Réglages ──────────────────────────────────────────────────────────────
  // Publics parce que la table de persistance de MoonlightUi décrit chaque
  // réglage par l'ADRESSE de sa valeur. Le panneau, lui, est ici : DrawSettings.
  bool  enabled_     = false;  // opt-in ; basculé en groupe par SetModernInterface
  bool  locked_      = false;  // fige les cadres + clics traversants
  bool  border_      = false;  // liseré autour de chaque cadre
  bool  sticky_      = true;   // aimantation des cadres entre eux, au glissement
  float rounding_    = 4.0f;   // arrondi des cadres (0..16)
  int   text_mode_   = 2;      // barres : 0 aucun · 1 pourcentage · 2 valeurs · 3 les deux
  float text_scale_  = 1.0f;   // taille du texte, × la police UI
  // Garder les cadres à l'écran SANS cible. C'est le mode « je place mon HUD » :
  // sinon il n'y a rien à attraper tant qu'on n'a rien ciblé, et le réglage se
  // ferait à l'aveugle.
  bool  layout_mode_ = false;

  // ── Cyclage au clavier ────────────────────────────────────────────────────
  // Le client n'a AUCUN ciblage clavier : rien dans
  // `UIWindowMgr_DispatchHotkeyBehavior` n'y ressemble, il n'y a donc rien à
  // imiter ni à intercepter — tout est à nous.
  // 🔴 Le cycle ne prend QUE ce qui est à l'écran, et ce n'est pas réglable : une
  // cible qu'on ne voit pas n'est pas une cible — on ne peut ni la jauger, ni
  // décider de l'attaquer, et le HUD se remplirait pour rien.
  bool  cycle_wrap_ = true;         // revenir au premier après le dernier

  // ── La flèche de ciblage du jeu ───────────────────────────────────────────
  // 🔴 Le petit triangle blanc n'est PAS piloté par la seule sélection `+0xF4`.
  // `GameMode_UpdateSelectedTargetNameLabel` commence par un verrou :
  // `CGameMode+0x28` nul ⇒ retour immédiat sur `SendMsg(5)`, c'est-à-dire
  // « masquer » (`0x00C76C9A`). Or ce champ n'est levé que par le **message 26**
  // du mode (`0x00C884B9`), émis au CLIC sur un acteur, et il retombe dès que la
  // souris quitte l'entité bouton relâché (`0x00C757C4`). D'où la flèche qui ne
  // survit ni au cyclage clavier, ni au simple fait de regarder ailleurs.
  //
  // Forcer `+0x28` serait pire que le mal : le natif y lit un engagement et colle
  // le curseur « attaque » sur tout l'écran, à chaque frame (`0x00C7571E`). On
  // laisse donc le jeu demander le masquage, et on RÉÉCRIT sa demande au passage
  // — cf. `WantsSelectionMarker`.
  bool  native_marker_ = true;

  // ── Le clic cible, sans frapper ───────────────────────────────────────────
  // Le clic prend bien la cible — `+0xF4` est écrit par le pipeline souris,
  // avant tout le reste — mais la DEMANDE D'ACTION est supprimée en vol, dans le
  // hook de `CMode::SendMsg` (message `0x89`, actions 0 et 7).
  //
  // 🔴 Mais jeter le coup NE SUFFIT PAS À IMMOBILISER : mesuré en jeu, le
  // personnage courait encore sur le monstre. L'approche découle de l'ACTION EN
  // ATTENTE que le clic arme sur l'acteur du joueur (`+0x500` = 1 ou 5,
  // `+0x514` = GID, posés par `GameMode_PostActorClickAction`), et pas de la
  // demande de coup, qui n'arrive qu'ensuite. Il faut donc couper aux DEUX
  // étages : l'armement (`SuppressClickEngage`) et le coup
  // (`SuppressBasicAttack`).
  //
  // ⚠ Le DOUBLE-CLIC, lui, ATTAQUE : deux engagements sur la même entité dans la
  // fenêtre de double-clic de Windows, c'est un ordre et non un ciblage. Même
  // dispense que « Attaquer » du menu contextuel, et de même durée — l'attaque
  // est une suite de demandes, pas une seule.
  bool  click_no_attack_ = false;

  // ── Les cadres SONT la cible ──────────────────────────────────────────────
  // Cliquer un cadre du HUD revient à cliquer l'entité elle-même. Ce n'est pas
  // une imitation : le clic rejoue `GameMode_PostActorClickAction`
  // (`0x00C753A0`), la fonction que le clic natif sur une entité appelle, avec
  // notre GID. Tout ce qui en découle vient donc du client, y compris nos
  // propres règles — le réglage « cibler sans attaquer », sa dispense au
  // double-clic, l'approche puis le coup.
  //
  // C'est cette fonction, et elle seule, qui décide de ce que « cliquer une
  // entité » veut dire, selon le ciblage armé (`CGameMode+0x408`) :
  //   · 0 (rien d'armé)     -> message d'acteur 10 : approche + attaque ;
  //   · 2 / 4 (cible)       -> messages 41 puis 90 : la compétence part sur ce
  //                            GID, exactement comme si on avait cliqué le
  //                            sprite ;
  //   · 1 (au sol)          -> RIEN. Un sort de zone vise une CASE, résolue
  //                            ailleurs (sous le curseur). Un cadre n'est pas
  //                            une case : il redevient clic-traversant dans ce
  //                            mode, et le clic part au sol comme avant.
  //   · 3 / 5               -> idem, le ciblage natif garde la main.
  //
  // 🔴 Un cadre qui agit REPREND LA SOURIS au jeu tant que le curseur est
  // dessus — clic droit et molette compris, comme toute fenêtre ImGui. D'où
  // l'opt-in, et d'où le fait qu'un cadre ne devienne cliquable QUE quand il
  // y a une cible et que le clic aurait un sens.
  //
  // ⚠ L'action est mise en attente et rejouée dans `OnGameFramePulse`, hors de
  // la frame ImGui : c'est un appel NATIF, qui peut ouvrir une boîte de message
  // (surcharge de poids) et relancer le tick du mode.
  bool  proxy_click_ = false;

  // ── Les sorts ciblés partent sur la cible, sans la souris ─────────────────
  // Le client demande DEUX gestes pour une compétence ciblée : la touche arme
  // le mode ciblage, puis le clic désigne QUI. Coché, c'est la cible du HUD qui
  // répond à la seconde question — la souris n'a plus à être sur l'entité, ni
  // même sur le monde.
  //
  // 🔴 La souris garde la PRIORITÉ quand elle désigne vraiment quelqu'un : ce
  // réglage COMBLE un vide (rien sous le curseur), il ne détourne pas une visée
  // explicite. Un sort AU SOL n'est pas concerné — il vise une case.
  //
  // Le lancement lui-même reste celui de QuickCast (messages d'acteur du clic
  // natif, puis `SendMsg(0x47)` pour désarmer), y compris sa répétition tant que
  // la touche est tenue : la seule chose qui change est la RÉPONSE à « sur
  // qui ? ». Cf. `SkillTargetGid`.
  bool  cast_on_target_ = false;

  // Portrait : mêmes leviers que le portrait du joueur.
  int   portrait_dir_     = 0;     // orientation 0..7 (0 = de face)
  int   portrait_anim_    = 0;     // type d'action (0 = repos)
  bool  portrait_animate_ = true;  // jouer les images de l'action

  Elem elems_[kElemCount] = {
      /* portrait */ {true, {20, 20, 72, 72}, {0.05f, 0.05f, 0.07f, 0.78f},
                      {1.00f, 1.00f, 1.00f, 1.00f}},
      /* nom      */ {true, {96, 20, 190, 34}, {0.05f, 0.05f, 0.07f, 0.78f},
                      {1.00f, 1.00f, 1.00f, 1.00f}},
      /* PV       */ {true, {96, 56, 190, 16}, {0.05f, 0.05f, 0.07f, 0.78f},
                      {0.85f, 0.27f, 0.27f, 1.00f}},
      /* SP       */ {true, {96, 74, 190, 16}, {0.05f, 0.05f, 0.07f, 0.78f},
                      {0.30f, 0.62f, 0.95f, 1.00f}},
      /* race/ele */ {true, {20, 96, 266, 18}, {0.05f, 0.05f, 0.07f, 0.78f},
                      {0.78f, 0.78f, 0.85f, 1.00f}},
  };

  // Posé quand un cadre a bougé ; MoonlightUi le draine pour n'écrire le YAML
  // qu'une fois, au lieu d'à chaque frame de glissement.
  bool geometry_dirty_ = false;

  // Section « Fenêtre de cible » du panneau Moonlight. Renvoie true si un
  // réglage a changé — l'appelant décide alors de sauvegarder.
  bool DrawSettings();

  // ── Ce que les raccourcis appellent ───────────────────────────────────────
  // Passe au monstre suivant (`forward`) ou précédent, parmi ceux qui sont à
  // l'ÉCRAN, triés du plus PROCHE au plus loin. Renvoie false
  // quand il n'y a rien à cibler — l'appelant peut alors se taire plutôt que de
  // laisser croire à un raccourci mort.
  //
  // Elle écrit la sélection NATIVE (`CGameMode+0xF4`), pas seulement la nôtre :
  // la cible clavier est ainsi la même que celle d'un clic pour tout ce qui lit
  // ce champ.
  //
  // ⚠ Cela ne suffit PAS à allumer la petite flèche blanche du jeu — une version
  // antérieure l'a cru, et ça s'est vu en jeu. Le marqueur est gardé par
  // `CGameMode+0x28` (l'engagement SOURIS), pas par `+0xF4` : c'est
  // `WantsSelectionMarker` qui le rend au ciblage clavier.
  bool CycleTarget(bool forward);

  // ── Ce que le dispatch de messages du mode appelle ────────────────────────
  // Vrai si la flèche de ciblage du jeu doit être posée sur NOTRE cible ; remplit
  // alors sa position monde, tronquée en entiers exactement comme le fait le
  // natif (`0x00C770F5`).
  //
  // 🔴 Appelée depuis le hook de `CMode::SendMsg`, sur le message 5 (« masquer le
  // marqueur »). Ce message n'a qu'UN émetteur dans tout le binaire — la fonction
  // du nom flottant — et il tombe à chaque frame où le joueur n'a pas la souris
  // sur sa cible. Le convertir en message 4 est donc le seul geste qui donne à la
  // flèche la durée de vie de la SÉLECTION plutôt que celle de l'engagement
  // souris, et il ne coûte aucun hook de plus.
  //
  // Renvoie false si le HUD est éteint : la flèche est un rendu du HUD de cible,
  // pas un réglage à part. Elle suit `gid_`, donc aussi le ciblage au clavier et
  // celui d'un sort lancé sur un cadavre.
  bool WantsSelectionMarker(int* world_x, int* world_z);

  // ── Ce que le hook du clic appelle ────────────────────────────────────────
  // Vrai si ce clic ne doit RIEN armer sur l'acteur du joueur. Le ciblage, lui,
  // est déjà posé par l'appelant (`+0xF4`/`+0xF0`, `0x00C78E67`) : il survit.
  //
  // Deux garde-fous, et deux seulement — chacun vérifié plutôt que supposé :
  //   · une COMPÉTENCE armée (`CGameMode+0x408 != 0`) emprunte le même chemin,
  //     la bloquer empêcherait tout lancement au clic ;
  //   · MES compagnons (`acteur+0x2EC == g_Account_Aid`, le test du natif
  //     lui-même en `0x00C787CC`) se commandent au clic : pet, homoncule,
  //     mercenaire gardent leurs ordres.
  //
  // ⚠ Rien de plus. Un troisième filtre « unité ordinaire = `acteur+0x314 == 0` »
  // a existé : il rejetait TOUT, parce que `CActorSprite_InitDefaults`
  // (`0x00C45F47`) écrit `+0x314 = 4` par défaut. Une condition qui ne filtre
  // rien ressemble exactement à une fonctionnalité non branchée.
  bool SuppressClickEngage(void* game_mode, uint32_t target_aid);

  // ── Ce que le menu contextuel appelle ─────────────────────────────────────
  // « Attaquer » est un ordre EXPLICITE : il doit frapper même quand le réglage
  // est coché, sinon l'entrée de menu ne servirait plus à rien. Elle emprunte
  // pourtant le MÊME chemin natif que le clic (`GameMode_PostActorClickAction`),
  // d'où cette dispense, à poser juste avant.
  //
  // Elle dure tant que dure l'attaque : un seul passage est ouvert dans le
  // chemin du clic (le nôtre), mais la dispense du COUP reste valable pour ce
  // GID — sans quoi seule la première frappe partirait. Le prochain clic du
  // joueur sur une entité la referme.
  void NoteExplicitAttack(uint32_t gid);

  // ── Ce que le hook de CMode::SendMsg appelle ──────────────────────────────
  // Vrai si une demande d'attaque de base doit être jetée en vol. Aucun autre
  // garde-fou n'est nécessaire : seules les actions 0 et 7 du message `0x89` sont
  // filtrées, et elles ne peuvent être qu'une attaque de base — une compétence a
  // son propre message, un ordre à un compagnon passe par l'acteur du compagnon.
  //
  // Comme tout le reste du module, subordonnée à `enabled_` : l'interrupteur
  // « Activer le mode Ciblage + HUD » est un vrai maître, et le panneau grise ce
  // qu'il commande plutôt que de laisser croire à un réglage vivant.
  bool SuppressBasicAttack(uint32_t target_gid) const {
    if (target_gid != 0 && target_gid == explicit_attack_gid_) return false;
    return enabled_ && click_no_attack_;
  }

  // Le GID visé par un sort qui part (CZ_USE_SKILL). 🔴 C'est le SEUL moyen de
  // suivre une cible que le client refuse de sélectionner : sa sélection
  // (`CGameMode+0xF4`) n'est écrite qu'au clic sur une cible « valide »
  // (`0x00C79D3C`), et un CADAVRE n'en est pas une — on peut lui lancer une
  // résurrection sans que rien ne bouge côté client. Le paquet, lui, porte le
  // GID.
  //
  // C'est un GESTE de ciblage comme un autre : le dernier gagne.
  //
  // ⚠ Un sort AU SOL n'appelle pas ceci : son paquet porte des coordonnées, pas
  // un GID. La cible affichée ne change donc pas — ce qui est juste, un sort de
  // zone ne désignant personne.
  void NoteSkillTarget(uint32_t gid);

  // ── Ce que QuickCast appelle ──────────────────────────────────────────────
  // Le HUD propose-t-il sa cible aux compétences armées au clavier ? Sert de
  // simple test d'opt-in : QuickCast s'autorise alors à travailler même quand la
  // souris ne désigne pas le monde, puisqu'elle n'a plus rien à désigner.
  bool CastsOnHudTarget() const { return enabled_ && cast_on_target_; }

  // Le GID à viser pour ce mode de ciblage, ou 0 si le HUD n'a rien à proposer.
  // Ne répond que pour les modes 2 (offensif) et 4 (soutien) : un sort au sol
  // vise une case, pas une entité.
  //
  // 🔴 Les mêmes règles que la visée à la souris de QuickCast, parce qu'elles ne
  // vivent PLUS dans le client : le chemin natif qui les portait
  // (`CursorMgr_UpdateHover`) n'est pas emprunté. En offensif : jamais soi-même,
  // jamais un joueur (PVP/GVG restent au clic manuel), jamais un cadavre.
  uint32_t SkillTargetGid(int targeting_mode) const;

  // ── Ce que le menu contextuel de l'entité appelle ─────────────────────────
  // Masque le HUD pour la cible COURANTE (il revient au prochain changement de
  // cible). `gid` sert de garde : masquer depuis le menu d'une entité qui n'est
  // plus la cible ne doit rien faire.
  void HideForGid(uint32_t gid);
  // Vrai si le HUD est actuellement affiché pour ce GID — le menu ne propose
  // l'entrée que dans ce cas.
  bool IsShownFor(uint32_t gid) const;

 private:
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Le corps du rendu, appelé sous __try/__except : il lit des structures
  // natives par offsets, et une entité à demi construite ne doit pas emporter
  // le client.
  void DrawHud();
  // Les cinq cadres, une fois les données rassemblées. `actor` peut être nul :
  // c'est le mode « placement » (aucune cible, on pose son HUD). `game_mode`
  // sert à lire le ciblage armé, dont dépend le fait qu'un cadre soit cliquable.
  void DrawElements(void* game_mode, void* actor);

  // Repart de zéro sur une nouvelle cible : on ne garde RIEN de l'ancienne, pas
  // même ses PV — les afficher sous le nouveau nom serait un mensonge muet.
  void Reset(uint32_t gid);

  // Émet CZ 0x0F29 si le délai est écoulé.
  void PollServer();

  // Redemande le nom de la cible (CZ_REQNAME). 🔴 Le client s'interdit de
  // redemander un GID pendant 10 s, et on peut cibler plus vite que la réponse
  // n'arrive — surtout si l'entité vient d'être recréée (Cloaking, @hide,
  // retour dans AREA_SIZE). Sans ce rappel, le nom reste « inconnu » jusqu'à dix
  // secondes, y compris pour le nameplate du jeu.
  void RequestTargetName();

  // ── Cible suivie ──────────────────────────────────────────────────────────
  uint32_t gid_        = 0;      // 0 = aucune cible affichée
  bool     hidden_     = false;  // masqué à la main POUR CE GID
  // GID que le SERVEUR a refusé (« hors de portée »). 🔴 Sans cette mémoire, le
  // HUD se rallumerait à la frame suivante — la sélection native n'est purgée
  // qu'au changement de map — et on repartirait pour une requête, un refus, un
  // rallumage, en boucle. L'abandon se lève dès que l'entité a quitté le monde
  // du client : ce qui reviendra ensuite sera un nouvel acteur.
  unsigned last_poll_   = 0;     // GetTickCount du dernier CZ 0x0F29
  unsigned name_retry_ms_ = 0;   // GetTickCount du dernier CZ_REQNAME de rappel

  // ── Détection du GESTE de ciblage ─────────────────────────────────────────
  // Posé par un clic, un cyclage clavier ou un sort qui part ; consommé au rendu
  // suivant. 🔴 C'est le SEUL chemin par lequel une cible s'allume : le HUD ne
  // sonde plus `+0xF4`, sans quoi une cible sortie de l'écran se rallumait toute
  // seule en revenant (la sélection native n'est purgée qu'au changement de map).
  uint32_t pending_gesture_gid_ = 0;
  uint32_t last_native_sel_     = 0;  // pour voir CHANGER la sélection native
  bool     was_engaged_         = false;  // front montant de `CGameMode+0x28`

  // Dispense accordée à « Attaquer » du menu contextuel (cf. NoteExplicitAttack)
  // et au DOUBLE-CLIC (cf. SuppressClickEngage).
  uint32_t explicit_attack_gid_     = 0;
  bool     explicit_engage_pending_ = false;
  // Dernier engagement au clic, pour reconnaître le double.
  uint32_t last_click_aid_ = 0;
  unsigned last_click_ms_  = 0;

  // Clic reçu par un cadre, en attente d'être rejoué hors frame ImGui (cf.
  // `OnGameFramePulse`). 0 = rien en attente. Un seul clic mémorisé : deux
  // appuis dans la même frame, ça n'existe pas.
  uint32_t proxy_click_gid_ = 0;

  // ── Ce que le client sait, relu à chaque frame ────────────────────────────
  char     name_[64]  = {0};
  char     party_[64] = {0};
  char     guild_[64] = {0};
  char     rank_[64]  = {0};
  bool     is_mob_    = false;
  bool     is_player_ = false;

  // ── Dernière réponse serveur, pour CE gid_ ────────────────────────────────
  bool     srv_valid_  = false;  // une réponse est arrivée pour cette cible
  uint8_t  srv_known_  = 0;      // masque : 1 = PV, 2 = SP, 4 = niveau, 8 = kind
  uint8_t  srv_type_   = 0;      // 1 PC · 2 MOB · 3 NPC · 4 HOM · 5 MER · 6 PET · 7 ELEM
  int16_t  srv_level_  = 0;
  uint32_t srv_hp_     = 0;
  uint32_t srv_maxhp_  = 0;
  uint32_t srv_sp_     = 0;
  uint32_t srv_maxsp_  = 0;
  uint8_t  srv_race_   = 0;
  uint8_t  srv_ele_    = 0;
  uint8_t  srv_ele_lv_ = 0;
  uint8_t  srv_size_   = 0;
  uint8_t  srv_boss_   = 0;
};
