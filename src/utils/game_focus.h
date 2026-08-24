#pragma once

// ── « Le jeu est-il au premier plan ? » ───────────────────────────────────────
//
// La question se pose dès qu'on lit le clavier PHYSIQUE ou qu'on mesure le
// temps, deux choses que Windows continue de faire tourner quand le joueur a
// alt-tabbé ailleurs :
//
//   - `GetAsyncKeyState` rend l'état physique d'une touche sans se soucier du
//     focus. Une touche restée enfoncée au moment d'un alt-tab répond donc
//     encore « oui » pendant que le joueur tape dans une autre fenêtre — la
//     répétition de QuickCast tournait dans son dos, et sur un objet elle
//     viderait son sac.
//   - Sans focus, le jeu et Windows brident le rendu : toutes les frames
//     deviennent longues et le profileur les compterait comme des pics.
//
// Le test ne demande PAS le HWND du jeu (RagnarokClient::GameWindow) : il
// compare le PROCESSUS de la fenêtre de premier plan au nôtre, ce qui couvre
// aussi nos propres fenêtres et ne dépend d'aucune initialisation. C'est
// pourquoi il vit ici et non dans ragnarok_client.h, dont l'inclusion
// entraînerait yaml-cpp et tout le client pour cinq lignes de Win32.
//
// Deux fichiers en portaient leur copie, l'une nommée `GameHasFocus` et l'autre
// `HasFocus` ; celle de QuickCast portait même le commentaire « (même patron que
// utils/frame_profiler) ». La dette était donc déjà déclarée dans le code.

namespace win {

// True si la fenêtre de premier plan appartient à CE processus. False s'il n'y
// en a aucune (session verrouillée, bascule de bureau).
bool GameHasFocus();

}  // namespace win
