#pragma once

// ── qty_prompt : la petite modale « Quantité » partagée ──────────────────────
// Le dialogue « combien ? » qui précède un transfert de PILE (inventaire, storage,
// chariot) était recopié à l'identique dans les trois plugins, en widgets ImGui
// bruts — donc en chrome ImGui bleu au milieu de fenêtres habillées RO. Une seule
// implémentation ici, habillée avec le toolkit (ro::BeginRoPopupModal + boutons
// RO), et les trois appelants n'en tiennent plus que le sens de l'action.
//
// Usage (à appeler CHAQUE frame, dans la fenêtre RO concernée) :
//   if (clic_sur_une_pile) ro::OpenQuantityPrompt(this);   // de n'importe où
//   ...
//   bool cancelled = false;
//   const int qty = ro::QuantityPrompt(this, "Vers le storage", max, &cancelled);
//   if (qty > 0)        Transferer(qty);   // le joueur a validé
//   else if (cancelled) /* action abandonnée */;
//
// `owner` identifie la fenêtre appelante (son `this` suffit) : inventaire, storage
// et cart sont souvent ouverts ENSEMBLE et appellent tous QuantityPrompt à chaque
// frame — sans propriétaire, la première fenêtre rendue s'approprierait le prompt
// demandé par une autre. Un seul prompt à la fois (la modale bloque), donc un seul
// état global. Thread principal du jeu uniquement, comme tout le reste de l'ImGui.

namespace ro {

// Demande l'ouverture du prompt pour `owner`. Appelable de N'IMPORTE OÙ (menu
// contextuel, drag & drop, raccourci) : l'ouverture ImGui réelle est faite par
// QuantityPrompt, donc toujours dans la bonne pile d'ID — c'est justement le
// piège qu'un ImGui::OpenPopup depuis un menu contextuel ne franchit pas.
void OpenQuantityPrompt(const void* owner);

// Rend le prompt de `owner` et renvoie la quantité choisie (> 0) la seule frame où
// le joueur valide (OK / Tout / Entrée) ; 0 sinon. `cancelled`, s'il est fourni,
// passe à true la seule frame où le joueur abandonne (Annuler, Échap). Ne fait
// rien tant que le prompt en cours appartient à quelqu'un d'autre.
// `action_label` est le début de la phrase (« Jeter », « Vers le cart »…),
// `max_amount` la taille de la pile — une pile de 1 renvoie 1 directement, sans
// ouvrir de dialogue.
int QuantityPrompt(const void* owner, const char* action_label, int max_amount,
                   bool* cancelled = nullptr);

}  // namespace ro
