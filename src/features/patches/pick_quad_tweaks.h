#pragma once

#include "features/plugin.h"

// ── Zones cliquables : un GID NÉGATIF ne vole plus le clic ───────────────────
//
// Le client range chaque entité cliquable dans un quadtree d'écran
// (`g_NameplatePickQuadTree` 0x012135f0). Le clic ne va PAS à l'entité la plus
// proche du curseur : il va au rectangle le plus « devant », celui dont la
// profondeur est la plus grande, quel que soit son propriétaire. Deux
// rectangles superposés, c'est donc un arbitrage — et le perdant devient
// injoignable.
//
// 🔴 rAthena peut créer un acteur dont le GID est le NÉGATIF d'un vrai id.
// C'est le cas du clone d'`@disguise` sur Moonlight : le serveur envoie à son
// propre joueur un paquet d'apparition portant `-bl->id`, ce qui superpose au
// personnage un second acteur, avec son sprite de monstre et sa propre zone
// cliquable. Ce fantôme est INADRESSABLE : aucune action envoyée vers lui n'a
// de sens pour le serveur, qui ne connaît pas ce GID et jette la requête sans
// un mot. Quand sa zone gagne l'arbitrage, la compétence part « dans le vide »
// et le joueur ne voit qu'une cadence effondrée, sans message d'erreur.
//
// Un GID négatif n'est donc jamais une cible légitime : le protocole n'en
// produit pas, et le serveur n'en accepte aucun. On rétrécit sa zone.
//
// ── Où l'on se branche, et pourquoi là ──────────────────────────────────────
//
// `CActorSprite_SubmitNameplateQuad` (0x00c58bf0) alloue le quad, le remplit,
// l'élargit au minimum cliquable (cf. `g_PickQuadMinSizePx`), puis l'insère :
//
//     c58d0d  push edi                              ; le quad tout juste bâti
//     c58d0e  mov  ecx, offset g_NameplatePickQuadTree
//     c58d13  call NameplateQueue_Insert            ; E8 rel32
//
// 🔴 `NameplateQueue_Insert` (0x00a79610) ouvre sur un prologue SEH
// (`push ebp / mov ebp,esp / push -1 / push offset SEH_A79610`) : pas de
// JMP-hook possible dessus. On patche donc le SITE D'APPEL — un seul `E8
// rel32` réécrit, à un seul endroit, qui ne concerne que les acteurs. Les deux
// autres appelants (PNJ de carte 0x00d1dbf5, unité de compétence 0x00db4eef)
// restent intacts : un clone est un acteur.
//
// Le détour est posé UNE FOIS au démarrage et n'est jamais retiré. Le réglage
// n'est qu'un booléen lu dans le détour : éteint, il repasse la main sans avoir
// touché au quad, donc au comportement d'origine exact. Poser et retirer un
// patch d'octets en cours de partie, pendant que le thread de rendu exécute
// justement ce code, serait le vrai risque — pas le détour permanent.
//
// Layout du quad (40 octets, 10 mots) :
//   [0] x0  [1] y0  [2] profondeur  [3] x1  [4] y1  [5] profondeur
//   [6] GID  [7] job affiché  [8] catégorie (0 acteur / 3 pet / 4 unité)
//   [9] champ acteur +0x2c8
//
// ⚠ Ceci reste un PALLIATIF côté client. Le vrai correctif est serveur :
// ne pas émettre le clone à `-bl->id`, ou passer par `clif_class_change`
// (ZC 0x01b0), qui change le sprite de l'acteur RÉEL sans créer de second
// acteur. Cf. project_char_diagnostics.

namespace pick_quad {

// ── Taille minimale des zones cliquables (réglage JOUEUR) ────────────────────
//
// Le client élargit toute zone cliquable plus petite qu'un minimum calculé UNE
// fois par session depuis la largeur de fenêtre : largeur / 640 × 40 px pour
// les acteurs, × 34 pour les PNJ de carte et les unités de compétence — soit
// 100 et 85 px en 1600×900. C'est la principale cause du ciblage « baveux » :
// un œuf de 25 px se défend sur 100, et deux entités voisines se disputent le
// même clic.
//
// TROIS globales, une par famille (mesuré le 2026-08-18, quand un motif WARP a
// rendu trois hits au lieu d'un) ; chacune n'a que deux lectures dans tout le
// binaire, toutes dans la construction du quad de sa famille — les écrire ne
// touche ni aux plaques de nom ni au rendu.
//
// `min_shift` divise les trois minimums par 2^shift (0 = réglage du client,
// 4 = un seizième), en préservant la proportion à la résolution — le même
// contrat que le patch WARP `TighterClickArea`, pour que les deux se décrivent
// pareil. Les défauts sont relevés AVANT toute écriture (réversible), et le
// réglage restauré ne réécrit plus rien : un patch posé sur l'exe reste alors
// visible. Appliqué chaque frame par PickQuadTweaks::OnRenderUI, AVANT le
// forçage staff de CharDiagnostics (ordre des plugins) — le staff gagne.
enum MinAreaFamily { kFamilyActors = 0, kFamilyNpc = 1, kFamilySkillUnit = 2 };

int& min_shift();                  // 0..4, persisté
int  MinAreaDefault(int family);   // relevé sur le client ; 0 = pas encore mesuré
int  MinAreaCurrent(int family);   // défaut >> shift, plancher 1 (0 si pas mesuré)

// Rétrécir la zone cliquable de tout acteur au GID négatif.
bool& shrink_negative_gid();

// Ce qu'il en reste, en pourcentage de sa taille d'origine. 0 laisse 1 pixel au
// centre — le rectangle reste valide pour le quadtree, mais devient
// pratiquement inatteignable. 100 ne change rien.
int& negative_gid_percent();

// Combien de quads on a rétrécis depuis le lancement. Zéro veut dire qu'aucun
// GID négatif n'est jamais passé par là : c'est la mesure qui dit si ce serveur
// produit des clones, et elle vaut mieux que la conviction.
unsigned int negative_gid_hits();

// Le détour est-il en place ? Faux si les octets du site d'appel n'étaient pas
// ceux attendus — on refuse alors de patcher, et le réglage est sans effet.
bool installed();

// Réglages, dans Staff Tools.
void DrawSettings();

}  // namespace pick_quad

// Pose le détour à la construction ; applique le diviseur des minimums à chaque
// frame. Sans interface propre : le réglage joueur vit dans la section
// « Gameplay » du panneau Moonlight, les réglages staff dans Staff Tools.
class PickQuadTweaks : public Plugin {
 public:
  PickQuadTweaks();

  // Capture les défauts des trois minimums puis applique le diviseur. Passe
  // AVANT CharDiagnostics::OnRenderUI (ordre de construction des plugins), dont
  // le forçage staff écrase donc la globale des acteurs quand il est actif.
  void OnRenderUI() override;

  const char* name() const override { return "PickQuadTweaks"; }
};
