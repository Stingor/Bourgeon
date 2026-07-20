#pragma once

// Moniteur de temps de frame — filet de sécurité contre les régressions de perf.
//
// FrameProfiler_Tick() doit être appelé UNE fois par frame, au début de la
// présentation, sur les deux chemins de rendu (DX9 : Hooked_Present ; DX7 :
// Proxy_EndScene). Il entretient une baseline glissante et émet toutes les 10 s un
// résumé (LogDiag → bourgeon.log) : FPS moyen, nombre de pics, ms perdues. Rien
// tant que le jeu n'a pas le focus. Coût : deux QueryPerformanceCounter par frame.
//
// Historique : cet outil a servi à diagnostiquer le freeze de chat en combat (le
// vrai coupable étant un word-wrap quadratique en appels GDI, corrigé par le cache
// de mesure dans chat.cc). Les compteurs mémoire externes s'étaient révélés être un
// mauvais proxy du symptôme — seul le temps de frame le mesurait fidèlement.

void FrameProfiler_Tick();
