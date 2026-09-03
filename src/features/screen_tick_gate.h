#pragma once

#include <cstdint>

// ── Gate de tick d'un ÉCRAN plein cadre ──────────────────────────────────────
//
// Les écrans qui remplacent une fenêtre native plein cadre — le menu Échap
// (`game_menu`), les options de jeu (`game_settings`), la table des raccourcis
// (`hotkey_settings`) — ouvraient tous leur `OnTick` par les MÊMES vingt lignes,
// commentaire de justification compris. Ces lignes encodent deux invariants qui
// se sont payés cher, et qu'un quatrième écran écrit demain hériterait par
// copier-coller… ou pas du tout. C'est la raison d'être de ce fichier : ce n'est
// pas la longueur du bloc qui justifiait de le sortir, c'est le fait qu'une
// copie fautive ne se distingue pas d'une bonne à la relecture.
//
// 🔴 INVARIANT 1 — fermer sur le FRONT de chargement, pas sur l'état.
// Un warp / @load referme ces écrans : le client démonte tout son HUD à l'entrée
// dans la carte suivante, et rester ouvert par-dessus la nouvelle carte est une
// survivance de notre côté, pas un comportement du jeu. On regarde donc
// `MapLoadEpoch()`, un COMPTEUR, et non `IsMapLoading()`, qui est un ÉTAT : un
// chargement plus court que le battement d'OnTick (100 ms) tiendrait entre deux
// regards et l'écran resterait ouvert. C'était précisément le bug.
//
// 🔴 INVARIANT 2 — pendant le chargement, ne toucher à RIEN. Pendant
// `CGameMode::EnterWorld` le HUD natif est détruit puis reconstruit, et c'est la
// fenêtre de tir où agir dessus a DÉJÀ COÛTÉ UN USE-AFTER-FREE (cf.
// `Bourgeon::IsMapLoading`). Ce qui est en attente le reste, et partira au
// premier tick d'après le chargement.
//
namespace screengate {

// Rend true si le `OnTick` de l'appelant doit SORTIR immédiatement.
//
// `map_epoch` = le membre `map_epoch_` de l'écran, lu ET MIS À JOUR ici : c'est
// lui qui porte le front. `close_now` dit que l'écran doit se fermer, et il est
// INDÉPENDANT du retour — un changement de carte ferme ET laisse continuer ; le
// chargement qui suit arrêtera de lui-même au tick d'après.
//
// ⚠ Ne teste PAS `imgui_enabled_` : ce test-là reste chez l'appelant, parce que
// `game_settings` intercale entre les deux son cas « panneau ouvert depuis
// l'écran de connexion », où il n'y a ni carte ni HUD à reconstruire.
//
// L'emploi tient en quatre lignes, et c'est la forme à recopier :
//
//   bool close_now = false;
//   const bool stop = screengate::ShouldStopTick(&map_epoch_, &close_now);
//   if (close_now && open_) Close();
//   if (stop) return;
//
bool ShouldStopTick(uint32_t* map_epoch, bool* close_now);

}  // namespace screengate
