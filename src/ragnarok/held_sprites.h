#pragma once

// ── Arme et bouclier d'une APPARENCE, sans acteur en scène ───────────────────
//
// `ragnarok/own_actor.h` lit les chemins que le client a DÉJÀ résolus sur
// l'acteur du joueur. C'est la bonne source en jeu — mais au char-select il n'y
// a pas d'acteur, et les personnages de la liste ne sont que des lignes de
// `CHARACTER_INFO`. Leur arme et leur bouclier y figurent pourtant (+0x5A et
// +0x62), et le char-select natif se contente de ne pas les afficher.
//
// Ici, on demande la résolution au client lui-même : les fonctions qui
// construisent ces chemins ne dépendent PAS d'un acteur, seulement de la classe,
// du sexe et des identifiants d'équipement. On ne recopie donc aucune règle —
// ni la classe d'arme, ni le suffixe de nom, ni le repli. C'est le même réflexe
// que pour own_actor : la vérité vit dans le client.
//
// ⚠ Ce que ce module NE fait PAS, faute d'acteur pour les porter :
//   * la « traînée » d'arme (slot 6) et ses cas de montures ;
//   * les seaux de bouclier particuliers à quelques classes
//     (`CActorSprite_ResolveShieldBucket`), qui interrogent l'acteur.
// Un chemin manquant se traduit par « rien à dessiner », jamais par une erreur.

namespace rag {

// Chemins VFS **sans extension**, prêts pour `ro::DollHeldPiece`. Vide = rien à
// dessiner. Les .spr et .act sont séparés : rien ne garantit qu'ils partagent
// leur base (c'est déjà faux pour la cape).
struct HeldSpritePaths {
  char weapon_spr[260] = {0};
  char weapon_act[260] = {0};
  char shield_spr[260] = {0};
  char shield_act[260] = {0};
};

// Résout arme et bouclier pour (classe, sexe, ids d'équipement).
//
// `weapon_id` / `shield_id` sont les identifiants d'OBJET tels que les porte
// `CHARACTER_INFO` — la conversion en classe d'arme et en vue de bouclier est
// faite ici, par les fonctions du client.
//
// Rend false si rien n'a pu être résolu (mains nues, ou client pas prêt) ;
// `out` est alors entièrement vidé, donc exploitable sans test.
bool ResolveHeldSprites(int job, int sex, int weapon_id, int shield_id,
                        HeldSpritePaths* out);

}  // namespace rag
