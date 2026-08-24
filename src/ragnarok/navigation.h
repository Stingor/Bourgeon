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
// En-tête volontairement MINUSCULE, comme uiwnd.h : `<cstdint>` et `<excpt.h>`,
// jamais `<Windows.h>` — il le traînerait chez tous ses consommateurs.

#include <cstdint>
#include <excpt.h>  // __try / __except (l'appel gardé, plus bas)

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

// ── L'appel, et ce qui était recopié ────────────────────────────────────────
//
// `SearchRoute` prend le nom de carte en `std::string` PAR VALEUR. Deux fichiers
// — la fiche d'objet et la minimap — construisaient donc à la main la structure
// de 0x18 octets que MSVC pose sur la pile, chacun avec sa copie du layout, de
// son `static_assert` et de la boucle de recopie. C'est la partie délicate de
// cet appel, et c'était la seule vraiment identique.
//
// ⚠ Les QUATRE entiers qui encadrent (x, y) ne sont PAS identiques d'un
// appelant à l'autre, et aucun n'est identifié : la fiche d'objet passe
// (type, 1, 1, …, 0), la minimap (0, 5, 1, …, 1002). Les fondre en un seul jeu
// de valeurs serait une invention. Ils restent donc là où ils ont été OBSERVÉS,
// sous deux points d'entrée qui disent d'où vient la demande.

// La `std::string` MSVC à SSO, telle que le natif la reçoit par valeur (layout
// confirmé par capture live).
struct RouteName {
  char     buf[16];
  uint32_t size;
  uint32_t cap;
};
static_assert(sizeof(RouteName) == 0x18,
              "RouteName doit matcher la std::string MSVC (0x18 octets)");

// ⚠ 15 caractères utiles au plus : au-delà, MSVC passerait un POINTEUR et le
// natif lirait n'importe où. Aucun nom de carte de RO n'approche cette limite.
inline RouteName MakeRouteName(const char* map) {
  RouteName s{};
  int n = 0;
  while (n < 15 && map && map[n]) { s.buf[n] = map[n]; ++n; }
  s.size = static_cast<uint32_t>(n);
  s.cap  = 15;  // SSO : le tampon est DANS la structure
  return s;
}

// L'appel nu. Les noms `a3/a4/a5/a8` sont ceux de la pile décompilée : les
// baptiser sans les avoir identifiés ferait plus de mal qu'un nom neutre.
inline void SearchRouteRaw(const char* map, int a3, int a4, int a5, int x,
                           int y, int a8) {
  __try {
    if (!map || !map[0]) return;
    using SearchRouteFn =
        char(__thiscall*)(void*, RouteName, int, int, int, int, int, int);
    reinterpret_cast<SearchRouteFn>(kSearchRouteAddr)(
        Nav(), MakeRouteName(map), a3, a4, a5, x, y, a8);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Un LIEN de navigation d'objet (fiche d'objet, dialogue NPC) : `type` vient du
// lien lui-même.
inline void RouteToItemLink(const char* map, int x, int y, int type) {
  SearchRouteRaw(map, type, 1, 1, x, y, 0);
}

// La demande venue de la MINIMAP (clic sur une ville de la carte du monde).
inline void RouteFromMinimap(const char* map, int x, int y) {
  SearchRouteRaw(map, 0, 5, 1, x, y, 1002);
}

}  // namespace navi
