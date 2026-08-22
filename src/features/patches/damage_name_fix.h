#pragma once

#include "features/plugin.h"

// ── « Quelqu'un subit N points de dégâts » ───────────────────────────────────
//
// Le client n'affiche pas les dégâts à la réception du paquet : il empile un
// message DATÉ dans la file de la cible et le rejoue à l'échéance
// (`ActorMgr_FlushTimedMessageQueue` 0x00C48060). Ce différé vaut au minimum le
// temps de vol du projectile — 192 ms en natif, davantage si un module le
// ralentit.
//
// 🔴 Le nom est résolu **au moment du rejeu**, pas quand le message est empilé :
//
//     GameMode_CopyEntityName(mode, sortie, gid)     -> chaîne VIDE si l'entité
//                                                       n'est plus dans le
//                                                       dictionnaire CGameMode+0x160
//     repli CNameDict_GetEntryOrRequest              -> entrée vide STATIQUE
//                                                       (et une demande de nom
//                                                       de plus, pour un GID mort)
//     -> msgstring 1604 MSI_WHO_IS = « Quelqu'un »
//
// Conséquence : **toutes** les lignes encore en attente quand la cible meurt
// perdent son nom. Leur nombre est `différé / amotion`, donc il EXPLOSE avec
// l'ASPD — une ligne à 600 ms d'amotion, la totalité du log à 10 ms. Mesuré en
// jeu sur cinq points, cf. docs/wand_ranged_attack.md §11.
//
// Ce n'est pas un défaut d'affichage : le client redemande sincèrement le nom au
// serveur. À cet instant, pour lui, cette entité n'a pas de nom.
//
// ── Le correctif ────────────────────────────────────────────────────────────
//
// Deux détours, sur des sites d'appel (`E8 rel32`), rien d'autre :
//
//   • **On apprend** les deux noms AU MOMENT DU COUP
//     (`CActorSprite_ProcessDamageAction`, site unique 0x00C4D0A7) — la cible
//     est alors vivante et le dictionnaire la connaît encore. C'est ce qui sauve
//     le cas extrême, où le monstre meurt avant que la MOINDRE ligne ne sorte :
//     il n'y a alors aucun affichage réussi dont on aurait pu apprendre.
//
//   • **On restitue** au moment du rejeu, sur les DEUX appels de
//     `GameMode_CopyEntityName` internes au flush (0x00C480FD la cible,
//     0x00C48122 l'attaquant) — et sur eux seuls : la fonction a 23 sites
//     d'appel dans le client, les 21 autres n'ont rien à voir.
//
// On n'écrit que dans une chaîne **vide et en petit tampon** : aucune
// allocation, donc rien à libérer, et le client la détruit comme il l'aurait
// fait. Corollaire assumé : seuls les noms d'au plus 15 caractères sont
// restitués. Tous les noms de monstres y tiennent ; un nom de joueur long est
// tronqué — ce qui reste très au-dessus de « Quelqu'un » en information.
//
// Le correctif vaut pour **tout** message différé, pas seulement pour le
// projectile de baguette qui l'a révélé.
class DamageNameFix : public Plugin {
 public:
  DamageNameFix();

  const char* name() const override { return "DamageNameFix"; }
};
