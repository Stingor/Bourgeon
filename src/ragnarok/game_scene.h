#pragma once

// ── La SCÈNE : du mode de jeu à un acteur, et de son GID à son nom ───────────
// (client 20250716, base 0x400000 ; RE complète dans docs/entity_nameplate_re.md)
//
// Tout ce qui veut parler d'une entité du monde emprunte la même chaîne :
//
//   rag::ActiveMode()            -> CGameMode
//     + kGmActorMgr   (0xcc)     -> gestionnaire d'acteurs
//         + kAmListHead  (0x10)  -> sentinelle de la std::list<Actor*>
//         + kAmOwnPlayer (0x2c)  -> notre propre acteur (HORS de la liste !)
//     + kGmNameDict   (0x160)    -> dictionnaire des noms (objet EMBARQUÉ)
//
// C'était le gisement le plus copié du projet, et le plus invisible : ce sont des
// OFFSETS, qu'aucun relevé d'adresses ne voit passer. `0xcc` était redéclaré dans
// ONZE fichiers sous CINQ noms (kOffActorMgr, kOffScene, kGm_ActorMgr,
// kMode_ActorMgr, kOffActorMgr) et `0x160` dans SIX sous trois.
//
// 🔴 NE PAS RELEVER CES OFFSETS PAR LEUR VALEUR. `0xcc` nomme aussi le vecteur de
// tailles d'un contrôle à onglets (inventory_tweaks), le champ de saisie du nom
// d'échoppe (vending_window) et un octet de quête (quest_tracker) : trois
// structures sans aucun rapport. C'est le NOM, dans son fichier, qui dit de quoi
// on parle — et c'est pour ça que ceux-ci sont qualifiés `gamescene::`.
//
// En-tête volontairement MINUSCULE (`<cstdint>` seul), sur le modèle d'uiwnd.h.

#include <cstdint>

namespace gamescene {

// ── CGameMode ────────────────────────────────────────────────────────────────
constexpr int kGmActorMgr = 0xcc;   // *(gm+0xcc) = gestionnaire d'acteurs
constexpr int kGmNameDict = 0x160;  //  gm+0x160  = dictionnaire, objet EMBARQUÉ
                                    //             (pas un pointeur à déréférencer)

// ── Gestionnaire d'acteurs ───────────────────────────────────────────────────
// ⚠ NOTRE acteur n'est PAS dans la liste : il a son propre emplacement. Un
// parcours qui ne prend que la liste nous oublie — c'est le défaut que
// chat_balloon documente (« hors liste ! ») et que tous les parcourants doivent
// traiter.
constexpr int kAmListHead  = 0x10;  // *(mgr+0x10) = sentinelle std::list<Actor*>
constexpr int kAmOwnPlayer = 0x2c;  // *(mgr+0x2c) = acteur du joueur local
constexpr int kNodeActor   = 0x08;  //  node+8     = valeur du nœud (Actor*)

// ── CNameInfo : ce que rend le dictionnaire ──────────────────────────────────
// Trois std::string MSVC côte à côte. Les offsets ci-dessous sont relatifs au
// BLOC ; ceux de la std::string elle-même (taille, capacité) sont relatifs au
// CHAMP — d'où les deux familles, à ne pas additionner de travers.
constexpr int kNameStr   = 0x04;  // le pseudo
constexpr int kNameParty = 0x1c;  // le nom de son GROUPE
constexpr int kNameGuild = 0x34;  // le nom de sa GUILDE

constexpr int kStrSize = 0x10;  // std::string : _Mysize, relatif au champ
constexpr int kStrCap  = 0x14;  // std::string : _Myres  ; >= 0x10 => tas

// ── Fonctions natives ────────────────────────────────────────────────────────
// `CNameDict_GetEntryOrRequest(dict, gid)` __thiscall : rend le CNameInfo si le
// nom est connu ET valide ; sinon met le GID en file de demande au serveur et
// rend une entrée vide STATIQUE.
//
// 🔴 CE N'EST PAS UN SIMPLE GETTER : c'est lui qui fait ARRIVER les noms encore
// inconnus, exactement comme le survol natif. Un appelant qui l'interroge par
// entité et par frame émet donc des paquets — d'où les throttles côté appelants.
// `kNameDictContainsAddr` répond « est-il connu ? » SANS rien demander : c'est
// la question à poser quand on ne veut pas déclencher de trafic.
constexpr uintptr_t kNameDictGetEntryOrRequestAddr = 0x005a1460;
constexpr uintptr_t kNameDictContainsAddr          = 0x005a18e0;

// Retrouver un acteur par son GID. DEUX portes, et le choix n'est pas
// indifférent :
//
//   kActorListFindByGidAddr  __thiscall(actorMgr, gid) -> Actor*
//       il faut déjà tenir le gestionnaire ; c'est la voie de qui vient de
//       parcourir la scène et l'a sous la main.
//   kFindActorByGidAddr      __stdcall(gid) -> Actor*
//       la commodité globale : elle retrouve le gestionnaire toute seule. C'est
//       elle qu'il faut quand on n'a qu'un GID venu d'un paquet ou d'une liste
//       sociale, et c'est la plus employée du projet (cinq fichiers).
//
// 🔴 Les deux rendent nullptr quand l'acteur n'est pas EN VUE — ce n'est pas
// « il n'existe pas ». C'est la raison pour laquelle les PV d'un membre de
// groupe trop loin sont inconnus plutôt que nuls.
constexpr uintptr_t kActorListFindByGidAddr = 0x00a69eb0;
constexpr uintptr_t kFindActorByGidAddr     = 0x00d806a0;

// `PostActorClickAction` __thiscall(gm, aid, flag) : la suite que le client
// donne à un clic sur un acteur (approche, ciblage, ouverture). On la rejoue
// plutôt que d'en refaire la logique — c'est la règle du projet.
constexpr uintptr_t kPostActorClickActionAddr = 0x00c753a0;

// ── Le quadtree de PICKING ──────────────────────────────────────────────────
// 🔴 SON NOM A DÉJÀ MENTI UNE FOIS. Un fichier l'appelait
// « kNameplateQuadTreeAddr », ce qui laissait croire à une structure au service
// des étiquettes de noms. C'est le quadtree de PICKING : il est reconstruit à
// chaque frame par le rendu des sprites, et c'est la source du survol natif.
// Les étiquettes n'en sont qu'un consommateur parmi d'autres.
//
// `QueryPoint` __thiscall(tree, float x, float y) -> quad (dix floats) ou
// nullptr. Le quad porte, en dwords : +6 l'AID, +7 le job (qui discrimine
// joueur et monstre), +8 la CATÉGORIE de pick.
//
// ⚠ La catégorie n'est pas décorative : 0 = acteur, 1 = OBJET AU SOL, 3 = pet.
// Un code qui traite tout quad comme un acteur ramasse les objets au sol.
constexpr uintptr_t kPickQuadTreeAddr       = 0x012135f0;
constexpr uintptr_t kQuadTreeQueryPointAddr = 0x00a797b0;
constexpr int kQuadAid = 6;  // dword : AID de l'acteur
constexpr int kQuadJob = 7;  // dword : job/classe
constexpr int kQuadCat = 8;  // dword : catégorie de pick (0 acteur, 1 sol, 3 pet)

// Taille minimale d'une cellule du quadtree de picking, en unités de monde.
// C'est un réglage du client, pas une constante de code : le diagnostic de
// personnage l'affiche et un correctif de picking l'ajuste.
constexpr uintptr_t kPickQuadMinSizeAddr = 0x015e5b40;

// `World_PositionToTile` : une position monde (float) -> la cellule de carte.
// Employée par le déplacement au clavier (pour viser la case voisine) et par
// l'inspecteur d'entité (pour dire où se tient un acteur).
constexpr uintptr_t kWorldToTileAddr = 0x00c6aa80;

}  // namespace gamescene
