#pragma once

#include "features/plugin.h"

// ── Jaillissement des objets lâchés au sol ───────────────────────────────────
//
// Dans le client natif, un objet lâché par un monstre TOMBE : il naît 15 unités
// au-dessus de sa case et descend à la verticale en ~420 ms. Rien ne le fait
// jaillir du monstre — sa position horizontale est figée dès la naissance.
//
// Ce module remplace cette chute par un ARC : l'objet part à côté de sa case,
// s'élève, retombe dessus, puis rebondit deux fois. Purement visuel : la case
// où le serveur a posé l'objet, celle qu'on ramasse, ne change pas d'un pouce.
//
// ── Ce que fait le natif, et où l'on s'y branche ─────────────────────────────
//
// `ZC_ITEM_FALL_ENTRY` (0x009E) et sa variante 0x084B construisent un `CItem`
// puis appellent `CItem_InitFromItemSpawn` (0x00d1d390, vt+0x08). Quand le champ
// « chute » du paquet vaut 1 — et LUI SEUL distingue un objet qui vient d'être
// lâché d'un objet déjà posé, annoncé par `ZC_ITEM_ENTRY` (0x009D) à l'arrivée
// sur la carte — l'init arme trois champs :
//
//     CItem+0x14  (hauteur) = sol - 15.0     l'objet naît en l'air
//     CItem+0x184 (float)   = -0.6           vitesse verticale initiale
//     CItem+0x180 (int)     = 1              « chute en cours »
//     CItem+0x188 (float)   = sol - 15.0     origine de la parabole
//
// et l'intégrateur `sub_D1D240` (vt+0x04, 0x00d1d240) rejoue à chaque frame :
//
//     t = (timeGetTime() - CItem+0x8C) / 24
//     si CItem+0x180 == 0 : il sort AUSSITÔT
//     hauteur = (0.083 * t + v0) * t + origine
//     si hauteur > sol : hauteur = sol ; CItem+0x180 = 0     (atterri)
//
// 🔴 C'est ce slot 1 que l'on détourne, et rien d'autre. Trois raisons :
//   · il est l'`OnTick` par frame de la classe — le slot 1 de `CActorSprite` est
//     `Actor_OnTick_PerFrame` (0x00d43190), même rôle, même rang ;
//   · `sub_D1D240` n'a qu'UNE SEULE référence dans tout l'exe : cette entrée de
//     vtable. Aucun appel ne peut donc contourner le détour, et aucune autre
//     classe ne le partage ;
//   · poser `CItem+0x180 = 0` suffit à faire lâcher prise à l'intégrateur natif
//     dès sa première ligne utile : plus personne n'écrit la hauteur, on peut
//     l'écrire nous-mêmes. Le reste de sa besogne (le scintillement de l'objet,
//     `CItem+0x9F`, calculé AVANT ce test) continue intact — on l'appelle.
//
// ── Le sol, sans un seul appel natif ─────────────────────────────────────────
// L'arc doit se terminer EXACTEMENT sur le sol, sinon l'objet flotte. Or à la
// naissance le natif a déjà écrit `hauteur = sol - 15.0`, où `sol` est le
// terrain sous la case d'arrivée : `sol = hauteur + 15.0` est donc juste au
// flottant près, et gratuit. Rappeler `Terrain_GetHeightAt` chaque frame comme
// le fait le natif n'aurait rien apporté d'autre qu'une chaîne de pointeurs à
// tenir (mode → monde → terrain) dans un détour appelé pour chaque objet visible.
//
// ── D'où l'objet jaillit : de l'ENTITÉ, pas de sa case ───────────────────────
// Le paquet ne dit pas quel monstre a lâché quoi, et aucun octet de réseau n'a
// été ajouté pour le dire : l'entité se retrouve dans la scène, à la naissance
// de l'objet. Deux faits mesurés la rendent trouvable —
//   · le serveur pose le PREMIER objet sur la case même du monstre
//     (`mob_process_drop_list`, `DIR_CENTER`) et les suivants sur les trois
//     cases voisines : la source est à une case, jamais plus ;
//   · le monstre est ENCORE LÀ quand le paquet arrive. rAthena envoie les drops
//     sans délai (`delay_battle_damage: yes`) et ne retire l'unité qu'à
//     +250 ms — l'ordre joue en notre faveur, il aurait pu être l'inverse.
// L'entité la plus proche du point de chute est donc celle qui a lâché l'objet,
// et cela vaut aussi pour un objet qu'un joueur jette à ses pieds.
//
// À défaut d'entité dans le rayon, l'objet part d'à côté de sa case, dans une
// direction tirée de son AID (`CItem+0x17C`) : stable d'une frame à l'autre,
// différente d'un objet au suivant, donc une pluie de butin part en gerbe au
// lieu de se superposer. C'est le repli, pas le régime normal.
//
// ── Ce que le détour ne peut pas garantir ────────────────────────────────────
// ⚠ Le natif ne déplace JAMAIS un objet au sol horizontalement — sa chute est
// verticale. Nous écrivons donc X et Z (`+0x10`/`+0x18`) sur un objet dont le
// moteur n'a jamais eu à re-projeter la position à l'écran. Le pipeline le fait
// pour tout acteur mobile ; si un jour un objet paraissait glisser sans que son
// sprite bouge, c'est cette hypothèse-là qu'il faudrait mesurer, et le repli
// serait de n'animer que la hauteur.
class ItemDropArc : public Plugin {
 public:
  ItemDropArc();
  ~ItemDropArc() override;

  const char* name() const override { return "ItemDropArc"; }

  // Les `CItem` sont détruits et recréés au changement de carte : les arcs en
  // cours parlent d'objets qui n'existent plus. On oublie tout — une adresse
  // réattribuée à un nouvel objet est de toute façon rattrapée par le contrôle
  // d'AID du détour, mais autant ne pas la laisser traîner.
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Réglage persisté (clé yaml « item_drop_arc »). ON par défaut : c'est un
  // embellissement sans effet de jeu, sans touche à soi et sans réseau.
  bool  enabled() const { return enabled_; }
  bool& enabled() { return enabled_; }

  // Réglages fins, LIVE et non persistés — réservés au staff dans le panneau,
  // comme ceux du saut : mal réglés ils donnent un butin qui décolle ou qui ne
  // bouge plus, alors que les défauts valent pour tout le monde.
  float* p_height()      { return &height_; }       // crête de l'arc (unités monde)
  float* p_kick()        { return &kick_; }         // écart du point de départ
  int*   p_duration_ms() { return &duration_ms_; }  // durée du vol, hors rebonds

 private:
  bool  enabled_     = true;
  // 20 unités : le double du saut d'un personnage (10, cf. PlayerJump) et un
  // tiers de plus que la hauteur de naissance du natif (15). Réglé à l'œil en
  // jeu — c'est ce qui se lit comme un jaillissement plutôt qu'une chute.
  float height_      = 20.0f;
  // De combien l'objet part À CÔTÉ de sa case : c'est ce qui fait pencher l'arc,
  // et la direction vient de l'AID pour qu'une salve s'ouvre en éventail. À 0
  // l'objet monte et retombe droit. Même unité que la hauteur — l'unité de monde
  // du moteur, celle des 15 de la naissance native, PAS une fraction de case
  // (l'échelle d'une cellule est un champ du terrain, lu à l'exécution).
  float kick_        = 12.0f;
  int   duration_ms_ = 750;    // le natif met ~420 ms à tomber tout droit
};
