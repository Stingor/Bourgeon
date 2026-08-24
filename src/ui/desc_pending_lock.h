#pragma once

// ── Le verrou « description en vol » ─────────────────────────────────────────
//
// Anti-flicker de l'aperçu au survol, dans les fenêtres à liste d'objets.
//
// Le menu contextuel masque l'aperçu tant qu'il est ouvert. Au clic sur
// « Description » le menu se ferme AVANT que la fenêtre de description
// n'apparaisse : le curseur retombe sur la case, et l'aperçu se rouvrait pour
// quelques frames puis disparaissait. On le bloque donc dès la DEMANDE de
// description, jusqu'au prochain VRAI mouvement du curseur — le geste
// souris/menu est alors terminé — avec un garde-fou de temps au cas où la
// fenêtre n'arriverait jamais.
//
// L'inventaire et l'entrepôt en portaient chacun leur copie : quatre membres et
// deux méthodes identiques à l'octet près, les deux seuils compris. Le chariot
// n'en a pas (il n'ouvre pas de description), mais le jour où il en ouvrira une
// il aura le verrou sans le réécrire.
//
// ⚠ À appeler une fois par frame et pas davantage : `BlocksHover()` n'est pas un
// simple accesseur, il DÉSARME le verrou quand une de ses deux conditions de
// sortie est remplie. Deux appels dans la même frame et le second répond non.

#include <cstdint>

namespace ro {

class DescPendingLock {
 public:
  // Arme le verrou. À appeler au moment où l'utilisateur DEMANDE la description
  // (menu contextuel ou Ctrl+clic droit), pas quand la fenêtre arrive.
  void Arm();

  // Met à jour le verrou et rend true si l'aperçu doit rester masqué.
  bool BlocksHover();

 private:
  bool     armed_ = false;
  float    x_ = 0.0f, y_ = 0.0f;  // position du curseur au moment de l'armement
  uint32_t tick_ = 0;             // GetTickCount() au moment de l'armement
};

}  // namespace ro
