#pragma once

// ── La NAVIGATION du client ──────────────────────────────────────────────────
// (client 20250716, base 0x400000 ; RE complète dans docs/navigation_re.md)
//
// Tout le sous-système tient dans un seul objet global : la table des cartes,
// celle des liens, le graphe et le résultat du dernier calcul en sont des
// CHAMPS. Il n'y a pas de « manager » séparé à retrouver — d'où l'unique
// constante ci-dessous, dont trois fichiers gardaient chacun leur copie
// (kNaviMgr, kNaviMgr, kNavigation).
//
// En-tête volontairement MINUSCULE (`<cstdint>` seul), comme uiwnd.h.

#include <cstdint>

namespace navi {

// L'OBJET `g_Navigation`, pas un pointeur vers lui.
constexpr uintptr_t kNavigationAddr = 0x015c3090;

inline void* Nav() { return reinterpret_cast<void*>(kNavigationAddr); }

// `CNavigation::SearchRoute` — l'API publique « va là ». C'est elle que le
// bouton natif appelle, et c'est par elle qu'il faut passer plutôt que de
// rejouer le pathfinder : elle pose aussi l'itinéraire courant DANS l'objet, ce
// dont dépendent la minimap et le suivi pas-à-pas.
//
// 🔴 ELLE PEUT ÉCHOUER SUR UNE CARTE PARFAITEMENT ACCESSIBLE. Le troisième champ
// d'un lien est un TYPE de passage, et le pathfinder REFUSE certains types : le
// graphe livré se scinde ainsi en 745 composantes. Un « pas de route » n'est
// donc pas une preuve qu'il n'y en a pas — cf. docs/navigation_re.md et le
// remède serveur `naviregisterwarp`.
constexpr uintptr_t kSearchRouteAddr = 0x00b314f0;

}  // namespace navi
