#pragma once

#include "ui/ro_imgui.h"

namespace bourgeon {

// Inscrit la fenêtre ImGui courante dans la pile Échap centralisée (ui/ro_imgui).
// À appeler juste après ImGui::Begin(...), avec la variable `open` liée au Begin.
// Échap ferme alors la fenêtre la PLUS AU-DESSUS (z-order), une par une, et est
// avalé pour le jeu tant qu'une fenêtre est ouverte (voir ProcessEscapeStack).
// Les fenêtres passant par ro::BeginRoWindow/BeginRoDescWindow s'inscrivent déjà
// seules — ne pas doubler l'appel pour elles.
inline void CloseWindowOnEscape(bool& open) { ro::RegisterEscapeWindow(&open); }

}  // namespace bourgeon
