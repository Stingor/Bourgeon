#pragma once

// ── La CASE d'un état (buff / debuff) ───────────────────────────────────────
//
// Le pendant de `item_cell` pour les EFST : un seul endroit qui sait à quoi
// ressemble un état à l'écran, et tout ce qui en affiche passe par là.
//
// 🔴 POURQUOI CE FOYER EXISTE. Trois surfaces dessinaient la même icône, chacune
// avec sa boucle : les tuiles de la grille de groupe, les lignes de la fenêtre
// Groupe/Amis, la barre d'états de la cible. Elles ne différaient que par leur
// placement — et la troisième avait déjà pris de l'avance (grisage, compte à
// rebours) que les deux autres n'avaient pas. Ajouter l'infobulle aurait fait
// une quatrième écriture du même geste, et scellé la divergence.
//
// Le contrat est donc : l'APPELANT place, `status_cell` peint. Il donne un
// rectangle et des options ; il ne sait rien des textures, du Lua, ni de la
// façon dont une durée se dessine.

#include <cstdint>

#include "features/systems/status_effects.h"
#include "imgui.h"

namespace statuscell {

// Le grisage de la part ÉCOULÉE.
//
// ⚠ Ces valeurs partent dans les réglages : ne PAS les intercaler.
enum Sweep {
  kSweepNone = 0,
  kSweepRadial,    // le balayage horaire des jeux de rôle, depuis midi
  kSweepVertical,  // le voile descend depuis le haut, plus lisible en petit
};

struct Style {
  int   sweep = kSweepNone;
  ImU32 sweep_color = IM_COL32(0, 0, 0, 140);
  // Le compte à rebours sous l'icône. `time_px` à 0 = pas de texte.
  float time_px = 0.0f;
  // Grisée quand l'entité est hors ligne, comme les icônes de classe.
  bool  dim = false;
};

// Peint UNE case dans `p0..p1`, et rend son infobulle si le curseur est dessus.
//
// Rend faux quand rien n'a été peint — cet EFST n'a pas d'icône, ou sa texture
// n'est pas encore chargée. L'appelant qui compte ses cases doit en tenir
// compte : une case qui ne se dessine pas ne doit pas prendre de place.
//
// ⚠ `tooltip` ouvre une infobulle ImGui : à n'activer que sur une surface qui a
// le droit d'en poser une. Une tuile de grille dont l'infobulle porte déjà tout
// le membre n'en veut pas une seconde par-dessus.
bool Draw(const StatusEffects::Entry& e, ImVec2 p0, ImVec2 p1,
          const Style& style, bool tooltip);

// Le NOM de l'état, tel que le client l'écrit dans sa propre infobulle.
// Chaîne vide s'il n'en a pas. Mémorisé — c'est un appel Lua.
const char* Name(uint16_t efst);

// L'infobulle seule, pour un appelant qui a déjà peint sa case autrement (une
// tuile de grille, par exemple, dont le survol est testé sur toute la ligne).
void Tooltip(const StatusEffects::Entry& e);

}  // namespace statuscell
