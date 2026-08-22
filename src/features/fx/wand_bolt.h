#pragma once

#include "features/plugin.h"

// ── Projectile de baguette : la flèche devient une boulette ──────────────────
//
// Quand une arme « tire », le client fabrique un `CArrowEffect` et lui passe le
// **job de l'attaquant** ; c'est ce job, et lui seul, qui choisit le sprite du
// projectile :
//
//     CActorSprite_SetMotion 0x00d41df0, case 2 (attaque)
//       -> Lua IsItemUsingArrow(classe d'arme)   <- BowTypeList, weapontable.lub
//       -> Arrow_SpawnProjectileToTarget(pos, gid, retardDépart, JOB)  0x00c6d9d0
//       -> message 14 -> CArrowEffect_OnMsg 0x00db01b0 : switch sur le job
//
// Pour un joueur, le client passe **0** : hors de la plage [1410, 22176], donc
// sprite par défaut = la flèche générique (`몬스터\skel_archer_arrow`). En
// remplaçant ce 0 par le job d'un tireur du jeu, on hérite de SON projectile —
// sans toucher au sprite, au chargement, ni à la trajectoire :
//
//     1495 STONE_SHOOTER   -> STONE_SHOOTER_BULLET      (le défaut ici)
//     4218 Doram           -> su_soulattack             (boulette d'énergie)
//     1410 LIVE_PEACH_TREE -> PEACH_TREE_BULLET
//     1498 WOOTAN_SHOOTER  -> WOOTAN_SHOOTER_BULLET
//     1664..1667           -> CANON_BULLET, _1, _2, _3
//     2364 -> PETAL_BULLET   2475 -> MG_CORRUPTION_ROOT_BULLET
//
// 🔴 `Arrow_SpawnProjectileToTarget` ouvre sur un **prologue SEH** : pas de
// JMP-hook possible dessus. On réécrit donc les SITES D'APPEL — trois `E8 rel32`
// (les xrefs en donnent quatre, dont un seul n'est pas un tir d'arme).
//
// Le détour ne change rien tant que l'attaquant ne porte pas une baguette : il
// lit la classe d'arme de l'acteur source, donc les archers gardent leurs
// flèches et les bardes leurs notes.
//
// ── Notre propre sprite ─────────────────────────────────────────────────────
// Le job emprunté ne sert qu'à ENTRER dans la fabrique : une fois le projectile
// né, son sprite est remplacé par `몬스터aguette_shot.spr` / `.act`. Le tireur
// dont on emprunte le job garde donc le sien — ce qu'une réécriture de la chaîne
// en .rdata (0x0109CC30) n'aurait pas permis.
//
// 🔴 Le job seul ne nous distingue pas : un vrai STONE_SHOOTER tire sous le même.
// C'est le JETON posé au tir (`g_tir_baguette`) qui fait la différence, et il est
// seulement LU dans OnMsg — son consommateur tourne plus loin dans le même appel
// de `CActorSprite_ProcessDamageAction`. La marque, la lenteur et la rotation
// suivent le même jeton : le mob emprunté n'est touché par rien.
//
// ── Le vol : vitesse et orientation ─────────────────────────────────────────
//
// Une fois le sprite emprunté, deux choses restent réglées pour une flèche.
// Toutes deux se corrigent depuis la **vtable de CArrowEffect** (0x0109C3E8),
// dont les slots 1 et 2 sont `Update` et `OnMsg` : deux écritures de quatre
// octets, aucun code du client réécrit.
//
//   • **Vitesse.** `CArrowEffect_Update` ne lit le temps que par
//     `(timeGetTime() - [+0x8C]) / 24`, et tout en dépend : la position vaut
//     `origine + t × vitesse`, la fin de vie tombe à `t > 8 / échelle`. Reculer
//     l'instant de départ le temps de l'appel divise donc ce `t` unique — le vol
//     devient N fois plus lent ET N fois plus long, sans qu'une moitié puisse se
//     désynchroniser de l'autre. Le retard AVANT départ (`+0x160`) se compte dans
//     la même unité : il suit donc le même facteur d'office, et le tir ralentit
//     d'un bloc, geste compris. `kFacteurRetard` permet de l'en dissocier.
//
//   • **Orientation.** Un projectile RO n'a pas huit directions dessinées : il
//     en a **une seule**, que le moteur fait pivoter. `Actor_ComputeLayerQuad`
//     additionne la rotation d'écran de l'acteur (`+0x7C`, en degrés, recalculée
//     à chaque image) et celle de la couche du `.act`. Un sprite dessiné selon un
//     autre axe que la flèche générique arrive donc de travers, et l'écart se
//     rattrape par un simple décalage en degrés.
//
// Pour distinguer NOTRE boulette d'une flèche ordinaire, `OnMsg` marque l'objet
// à `+0x16C` : le constructeur s'arrête à `+0x168` alors que l'allocation fait
// 0x170, et aucune méthode de la classe ne touche ce dernier mot.
//
// ⚠ Le pendant côté données est `BowTypeList` (`data\luafiles514\lua files\
// datainfo\weapontable.lub`) : sans les classes de bâton dans cette liste, le
// client n'appelle jamais la fabrique et ce module n'a rien à détourner.
// Voir docs/wand_ranged_attack.md.
class WandBolt : public Plugin {
 public:
  WandBolt();

  const char* name() const override { return "WandBolt"; }
};
