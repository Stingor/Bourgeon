#pragma once

// ── MsgStringTable : les libellés et messages d'erreur du client ─────────────
// (client 20250716, base 0x400000)
//
// La table que le client charge depuis `data\msgstringtable.txt` : chaque
// message de l'interface y porte un numéro (les MSI_* du code natif). C'est la
// source à laquelle on prend nos textes quand on refait une fenêtre en ImGui —
// règle du projet : on affiche le libellé EXACT du client, jamais une
// paraphrase, sans quoi une fenêtre Bourgeon et son équivalent natif diraient
// deux choses différentes pour la même erreur.
//
// Une adresse et un SEH, redéclarés dans SIX fichiers sous DEUX orthographes
// (`kMsgStringGet` : bank, make_item, trade, weapon_refine ; `kGetMsg` :
// menu_icons, status_tweaks), avec le repli CP949 -> UTF-8 réécrit à l'identique
// dans trois d'entre eux.

#include <cstdint>

namespace msgstr {

// __cdecl(id) -> const char* (CP949). L'adresse reste exposée : menu_icons et
// status_tweaks REPASSENT le pointeur au dessinateur de texte natif, qui veut
// justement du CP949 — leur convertir la chaîne serait une régression.
constexpr uintptr_t kGetAddr = 0x00a9ed30;

// Libellé brut, tel que le client le stocke (CP949). Jamais nul : « » quand la
// table n'est pas encore chargée ou que l'id n'existe pas — la table est peuplée
// au démarrage du client, donc un appel très tôt peut légitimement rendre vide.
// SEH-gardé.
const char* Cp949(int id);

// Le même, converti pour ImGui. Jamais nul : « » si absent.
//
// ⚠ Le tampon rendu est celui de ro::LocalToUtf8 : thread-local et ROTATIF sur
// huit emplacements. Bon pour un affichage immédiat, à recopier si la chaîne
// doit survivre à la frame.
const char* Utf8(int id);

}  // namespace msgstr
