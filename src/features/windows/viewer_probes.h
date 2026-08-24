#pragma once

// ── « Que fait la fenêtre d'à côté ? » ───────────────────────────────────────
//
// L'inventaire, le chariot et l'entrepôt se posent sans arrêt des questions les
// uns sur les autres : est-il ouvert (pour proposer un transfert) ? la souris
// est-elle au-dessus (pour router un dépôt par glisser) ? une composition
// d'échoppe est-elle en cours (pour refuser le transfert) ?
//
// Chacun portait ses propres réponses, et les TROIS s'étaient donné des noms
// différents pour la même question :
//
//     souris sur l'inventaire   OverInventory      —              MouseOverInventory
//     souris sur l'entrepôt     OverStorage        StorageViewerOver   —
//     souris sur le chariot     —                  MouseOverCart   MouseOverCart
//
// 🔴 Et l'une des sept avait DIVERGÉ. `storage_window::CartOpen()` cherchait la
// fenêtre NATIVE du chariot (`uiwnd::FindWindow(0x28)`) là où l'inventaire
// interroge notre visionneuse. Or la native ne naît plus en interface moderne —
// cart_viewer.h le dit explicitement : « à interroger par les AUTRES modules au
// lieu de chercher la fenêtre native ». L'entrepôt ne proposait donc JAMAIS
// « Vers le cart », alors que le serveur autorise ce transfert-là.
//
// Sept questions, un seul endroit qui y répond.

namespace viewers {

// ── Est-elle ouverte ? ──────────────────────────────────────────────────────
// ⚠ « Ouverte » et non « visible » : en interface moderne la fenêtre native est
// détruite alors que la visionneuse est bel et bien à l'écran.
bool InventoryOpen();
bool CartOpen();
bool StorageOpen();

// Une composition d'échoppe est-elle en cours ? Elle gèle les transferts (le
// serveur refuserait), sans empêcher d'utiliser ni d'équiper.
bool VendingComposing();

// ── La souris est-elle au-dessus ? ──────────────────────────────────────────
// Coordonnées ÉCRAN. Sert à router un dépôt par glisser vers la bonne fenêtre.
bool MouseOverInventory(float x, float y);
bool MouseOverCart(float x, float y);
bool MouseOverStorage(float x, float y);

}  // namespace viewers
