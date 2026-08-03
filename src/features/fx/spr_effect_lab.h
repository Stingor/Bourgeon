#pragma once

// ── SPR Effect Lab ────────────────────────────────────────────────────────────
// Banc d'essai MINIMAL et PROPRE pour rendre un « hat effect » de type .spr/EZ
// (système EZ-effect procédural : nœud primitif vtbl 0x01088de8 -> enfant EZ vtbl
// 0x01088c48 -> ~170 sous-renderers) AILLEURS que sur le sprite du joueur en jeu.
// Objectif d'apprentissage : afficher l'effet AU CENTRE DE L'ÉCRAN, découplé.
//
// Principe (volontairement simple, cf. session 2026-07-18) :
//  1. On SPAWN notre propre effet via le chemin natif (Actor_ToggleEffectId) — le
//     jeu crée le nœud, l'enfant EZ, anime, dessine : on ne réécrit RIEN du système.
//  2. On CAPTURE uniquement les quads de NOTRE effet (match par id concret sur le
//     nœud primitif) au puits commun RenderQueue_InsertPrimitive — hooks CHAÎNÉS,
//     donc coexistent avec ceux de basic_info sans conflit (le HookManager relocalise
//     un JMP déjà posé). Plus de matching fragile par propriétaire parmi 50 effets.
//  3. On REDESSINE ces quads au centre de l'écran (ImDrawList). Option : SUPPRIMER
//     le rendu in-world (le hook ne chaîne pas pour nos quads) -> visible SEULEMENT
//     dans l'overlay.
//
// Ce module ne dépend PAS de basic_info et ne le modifie pas. Il est piloté depuis
// le debug UI (moonlight_ui) : DrawDebugControls() pour les boutons, RenderFrame()
// chaque frame (reconcile spawn + overlay).
namespace spr_lab {

// Installe les hooks de capture (idempotent, sûr à appeler plusieurs fois).
void EnsureInstalled();

// À appeler CHAQUE frame de rendu UI (entre NewFrame et Render) : reconcilie le
// spawn/despawn de l'effet demandé et dessine l'overlay au centre de l'écran.
void RenderFrame();

// Contrôles ImGui du banc d'essai (à placer dans un panneau/onglet debug existant).
void DrawDebugControls();

// Le « Sol uni » (fond de capture) a longtemps vécu ici, sans jamais rien partager
// avec la capture EZ : c'est maintenant features/fx/ground_paint.h, piloté depuis
// « Staff Tools ».

}  // namespace spr_lab
