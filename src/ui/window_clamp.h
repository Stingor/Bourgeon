#pragma once

#include "imgui.h"

// ── Fenêtres ImGui : rester dans l'écran de jeu ───────────────────────────────
// ImGui laisse SCIEMMENT une fenêtre sortir presque entièrement de l'écran : son
// clamp interne (ClampWindowPos) borne la position à
// `visibility_rect.Min - window->Size`, c'est-à-dire qu'il ne garantit que
// style.DisplayWindowPadding (19 px par défaut) de fenêtre visible. Un joueur qui
// glisse un panneau vers un bord se retrouve donc avec un moignon de 19 px de
// barre de titre, et la fenêtre est en pratique perdue (rien à cliquer pour la
// ramener, et la position part dans imgui.ini pour la session suivante).
//
// Ici on impose l'invariant « une fenêtre reste ENTIÈREMENT dans l'écran de jeu »
// (le rectangle client, io.DisplaySize — pas le bureau : l'overlay est peint dans
// le back buffer du jeu, tout ce qui dépasse est simplement coupé).
//
// Tout est à appeler depuis le thread principal du jeu (comme le reste d'ImGui).

namespace ro {

// Corrige la position d'un rectangle de fenêtre pour qu'il tienne en entier dans
// l'écran de jeu. Renvoie `window_pos` inchangée si elle est déjà bonne (ou si
// l'écran est de taille nulle : fenêtre minimisée).
//
// Pour les plugins qui pilotent EUX-MÊMES la position de leur fenêtre — HUD
// épinglé chaque frame en SetNextWindowPos(..., ImGuiCond_Always) et déplacé au
// delta souris : barre de skills, icônes de menu, barres d'infos. Le clamp global
// ci-dessous ne peut rien pour eux, puisque leur SetNextWindowPos écrase la
// position corrigée à chaque frame. À appliquer sur la position calculée par le
// glisser, APRÈS l'aimantation à la grille (sinon le snap repousse hors écran).
//
// Une fenêtre plus grande que l'écran sur un axe est alignée sur le bord haut /
// gauche : c'est le coin dont on a besoin (barre de titre, premiers items) et
// c'est ce que fait le clamp d'ImGui pour la taille.
ImVec2 ClampWindowPosToScreen(ImVec2 window_pos, ImVec2 window_size);

// Borne la TAILLE de toutes les fenêtres ImGui de haut niveau à 90 % de l'écran
// de jeu. À appeler juste AVANT `KeepWindowsOnScreen` : une fenêtre rétrécie
// tient dans l'écran plus facilement, l'ordre inverse ferait travailler le clamp
// de position sur une taille qui va changer.
//
// 🔴 C'EST UN FILET, PAS LE MÉCANISME PRINCIPAL — et ce n'est pas un doublon de
// `ConstrainNextWindowToScreen` (ui/ro_imgui.cc). Les deux bornent à 90 %, mais
// pas au même moment, et le moment fait tout :
//
//   · le wrapper RO pose un vrai `SetNextWindowSizeConstraints` AVANT `Begin`,
//     donc la borne entre dans le calcul du glisser et LA POIGNÉE BUTE ;
//   · cette passe-ci tourne après `NewFrame`, donc AVANT les `Begin` — elle
//     arrive une frame trop tôt pendant un redimensionnement et se fait
//     réécrire par `UpdateWindowManualResize`. Elle ne bride pas le geste : elle
//     rattrape ce qui a échappé.
//
// Ce qu'elle rattrape, et qui justifie qu'elle reste : les `ImGui::Begin` qui ne
// passent par aucun wrapper RO, et les tailles démesurées restaurées depuis
// `imgui.ini` — qu'aucune contrainte posée à l'ouverture ne corrigerait, la
// fenêtre étant créée avant son premier `Begin`.
//
// Sans elle, rien n'empêche de tirer une fenêtre plus grande que l'écran : ImGui
// ne borne la taille QUE si l'appelant a posé un `SetNextWindowSizeConstraints`,
// et cinquante de nos fenêtres n'en ont aucun. Le joueur perd alors les bords —
// dont la poignée de redimensionnement, qui est le seul moyen de revenir en
// arrière.
//
// 🔴 CE QUI N'EST PAS BORNÉ, et ce n'est pas un oubli :
//   · les fenêtres qu'ImGui possède (enfants, popups, tooltips, menus) — il les
//     borne déjà lui-même au viewport ;
//   · `AlwaysAutoResize` et `NoResize` — leur taille vient du CONTENU ou du
//     code appelant, pas d'un glisser du joueur. La borner tronquerait ce
//     qu'elles ont à montrer sans que personne n'ait rien demandé.
//
// ⚠ Une contrainte LOCALE plus stricte continue de gagner : elle s'applique dans
// `Begin`, donc après cette passe. Les fenêtres déjà bornées à 80 % le restent.
//
// ⚠ ET C'EST AUSSI CE QUI PROTÈGE LES FENÊTRES PLEIN ÉCRAN, sans qu'elles aient
// à se déclarer : `zone_recorder` (le rectangle de sélection) et l'écran de
// choix de personnage reposent `SetNextWindowSize(DisplaySize)` à CHAQUE frame,
// et `Begin` écrase alors `SizeFull` après nous. Une telle fenêtre ne perd donc
// pas ses 10 % de bord — mais elle le doit à l'ordre des opérations, pas à un
// drapeau. Qui déplacerait cette passe APRÈS les `Begin` les casserait toutes.
void KeepWindowsSizedToScreen();

// Ramène dans l'écran de jeu toutes les fenêtres ImGui de haut niveau. À appeler
// UNE fois par frame, juste APRÈS ImGui::NewFrame() :
//   * NewFrame a déjà appliqué le déplacement de la souris à la fenêtre glissée
//     (UpdateMouseMovingWindowNewFrame) -> la fenêtre bute contre le bord dans la
//     frame même, sans latence d'une frame ni tremblement ;
//   * les Begin() des plugins n'ont pas encore lu window->Pos -> la position
//     corrigée est celle qui sera dessinée.
// Les fenêtres enfants, popups, menus et tooltips sont laissées à ImGui, qui les
// replace de toute façon à chaque Begin (FindBestWindowPosForPopup, déjà borné au
// viewport) : les corriger ici ne servirait à rien.
void KeepWindowsOnScreen();

// ── Aimantation d'une fenêtre sur ses voisines ───────────────────────────────
// Une fenêtre traînée à la souris colle aux bords de l'écran et à ceux de ses
// consœurs dès qu'elle en approche : les chatbox se rangent bord à bord sans
// qu'on ait à viser le pixel, comme les fenêtres natives du client, qui ont leur
// propre gestionnaire d'adjacence (WinSnap).
//
// Rien n'est mémorisé : l'aimant CORRIGE la position de la frame, il ne crée pas
// de lien entre les deux fenêtres. Éloigner la souris décolle donc la fenêtre,
// et déplacer la voisine n'entraîne pas celle qui lui était collée.

// Ouvre/ferme l'aimant (réglage du joueur). Sans effet sur le clamp d'écran, qui
// n'est pas une option.
void SetWindowMagnet(bool on);

// Déclare la fenêtre ImGui COURANTE aimantable, pour cette frame. À appeler juste
// après son Begin. Seules les fenêtres marquées s'aimantent — entre elles et sur
// l'écran : une fenêtre invisible ou un overlay plein écran ferait sinon une
// ligne d'accrochage que le joueur ne voit pas et ne peut pas expliquer.
void MagnetMarkWindow();

// Aimante la fenêtre en cours de déplacement. Même fenêtre de tir que
// KeepWindowsOnScreen — juste après NewFrame, avant le premier Begin — mais
// AVANT lui : l'aimant peut pousser une fenêtre hors de l'écran d'un pixel ou
// deux, et c'est au clamp de dire le dernier mot.
void SnapMovingWindowToPeers();

}  // namespace ro
