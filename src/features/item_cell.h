#pragma once

// ── Briques partagées de la « cellule d'item » ───────────────────────────────
// Icône + nom composé + aperçu au survol + clic droit vers la description : ce
// motif est réécrit dans chaque viewer (inventaire, chariot, entrepôt, échoppe,
// boutique NPC, cash shop, fiche de perso). `VendingWindow::DrawItemCell` en est
// la version la plus aboutie et sert de modèle ; ce fichier généralise ses
// briques, une par une, à mesure qu'on les vérifie identiques.
//
// Pourquoi ici et pas dans `ui/` : le tooltip s'appuie sur
// `itemdesc::RenderSimpleDesc`, qui vit dans features/windows/item_desc_window.h.
// Le mettre dans `ui/` créerait l'inversion de couches que docs/source_layout.md
// interdit (ui/ ne connaît pas features/). Il vit donc à la racine de features/,
// comme plugin.h, staff_gate.h et hotkey_util.h.
//
// ⚠ Ce qui n'est PAS ici, délibérément : l'ouverture de la description. Elle
// existe en deux familles irréductibles — l'une repasse sur la liste VIVANTE
// pour transmettre l'ItemSkillInfo complet rempli par le serveur, l'autre en
// reconstruit un faute de nœud (et n'obtient alors qu'un id, sans texte). Les
// unifier demande de trancher ce que chaque appelant possède ; c'est la tranche
// suivante.

#include <cstddef>
#include <cstdint>

#include "features/windows/item_desc_window.h"  // itemdesc::SimpleOpt

namespace itemcell {

// Nom d'AFFICHAGE composé par le name-builder natif : raffinage, préfixes de
// cartes, forge. `wnd` est la fenêtre native qui sert de contexte au builder,
// `info` l'ItemSkillInfo de l'objet. Écrit toujours une chaîne terminée dans
// `out` — vide si tout échoue, jamais d'indéterminé.
//
// ⚠ Le builder n'ajoute PAS le suffixe d'emplacements « [N] » : l'appelant le
// compose lui-même (comme le fait itemdesc::RenderSimpleDesc, pour la même
// raison). ⚠ Repli intégré : si la composition rend une chaîne vide, on retombe
// sur le nom de base. L'ensemble est sous SEH — un ItemSkillInfo à moitié
// initialisé ne doit pas tuer le client.
void BuildDisplayName(void* wnd, void* info, char* out, size_t out_size);

// Aperçu RO au survol : tooltip fond blanc + cadre sysbox peint derrière par un
// split de canaux. À appeler HORS de toute fenêtre ImGui (il crée son popup).
//
// `cards` (4 max, 0 = vide) et `opts` sont des données d'INSTANCE : elles ne
// sont pas dans la DB client, l'appelant les lit dans SON ItemSkillInfo.
// `name` = nom déjà composé (cf. BuildDisplayName) ; nullptr = repli sur la DB.
void DrawTooltip(uint32_t id, const uint32_t* cards, int card_count,
                 const itemdesc::SimpleOpt* opts, int opt_count,
                 int refine = 0, const char* name = nullptr);

}  // namespace itemcell
