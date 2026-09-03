#include "features/screen_tick_gate.h"

#include "bourgeon.h"

// Le pourquoi des deux invariants est dans l'en-tête. Ici, seulement l'ORDRE des
// trois questions, qui compte autant que les questions elles-mêmes.

namespace screengate {

bool ShouldStopTick(uint32_t* map_epoch, bool* close_now) {
  *close_now = false;

  // 1. Hors jeu (char-select, déconnexion) : l'écran n'a plus d'objet.
  //
  // 🔴 On sort AVANT de lire l'époque, et il ne faut pas « corriger » cela : le
  // membre garde ainsi sa valeur, si bien que la prochaine entrée en jeu se lira
  // comme un front et refermera un écran qui aurait survécu. Lire l'époque ici
  // la mettrait à jour en douce et mangerait ce front.
  if (!Bourgeon::Instance().IsGameActive()) {
    *close_now = true;
    return true;
  }

  // 2. Le FRONT (invariant 1). Fermer ne suffit pas à sortir : c'est le
  //    chargement, question suivante, qui décide si le tick continue.
  const uint32_t epoch = Bourgeon::Instance().MapLoadEpoch();
  if (epoch != *map_epoch) {
    *map_epoch = epoch;
    *close_now = true;
  }

  // 3. L'ÉTAT (invariant 2).
  return Bourgeon::Instance().IsMapLoading();
}

}  // namespace screengate
