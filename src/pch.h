#pragma once

// ── En-tête précompilé ───────────────────────────────────────────────────────
// RÈGLE ABSOLUE : uniquement du TIERS STABLE. Rien qui appartienne à Bourgeon.
//
// Un PCH n'accélère la compilation que s'il ne change (presque) jamais : dès qu'un
// en-tête qu'il contient est modifié, le PCH est reconstruit ET les 66 unités de
// compilation sont recompilées. Y mettre bourgeon.h, plugin.h, ro_imgui.h ou
// moonlight_ui.h annulerait donc tout le bénéfice — ce sont précisément les fichiers
// qui bougent (moonlight_ui.h : 34 commits). NE RIEN AJOUTER D'INTERNE ICI.
//
// Ce qui rend l'opération rentable : mesuré sur le projet, le Windows SDK et la STL
// représentent ~94,6 % du volume préprocessé, contre ~1,5 % pour tous les en-têtes
// de src/plugins réunis. Le coût de compilation est presque entièrement ici.
//
// Le PCH est injecté par CMake (target_precompile_headers) : aucun fichier source
// n'a besoin de l'inclure, et les `#include` existants restent en place — ils sont
// nécessaires à la lisibilité et au fait que chaque fichier reste autonome si le PCH
// est désactivé (-DBOURGEON_USE_PCH=OFF).

// Windows — inclus nu dans 57 des 66 unités de compilation. NOMINMAX est déjà défini
// pour toute la cible (src/CMakeLists.txt), donc pas de collision min/max.
#include <Windows.h>

// STL : les en-têtes réellement partagés par la majorité des fichiers.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// Tiers verrouillés dans thirdparty/ : ne bougent qu'à une mise à jour délibérée.
#include "imgui.h"
#include "spdlog/fmt/fmt.h"
#include "yaml-cpp/yaml.h"
