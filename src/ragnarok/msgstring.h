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

// 🔴 Le même, mais qui retombe sur `fallback` quand le client N'A PAS le libellé.
//
// Constaté en jeu le 2026-08-14 sur Moonlight : les onglets « Other » et
// « Login Notification » de Game Settings s'affichaient **« NO MSG »**. Cause —
// le client charge `data\msgstringtable.txt`, qui n'a que **4023 entrées** ici,
// alors que le `.csv` du même GRF en a 4361. Tout id ≥ 4023 est donc absent, et
// c'est INVISIBLE au RE : la table qu'on lit pour trouver un id n'est pas celle
// que le client charge.
//
// `MsgStringTable_GetById` (0x00A9ED30) a deux sentinelles, toutes deux prises
// en charge ici :
//   - id connu de la table mais sans texte  -> exactement « NO MSG » ;
//   - id ≥ 4355 (hors bornes dures)         -> « NO MSG : <id> ».
//
// ⚠ À PRÉFÉRER À `Utf8` pour tout libellé d'une fenêtre qu'on remplace : sans ce
// repli, l'échec est MUET et sort tel quel à l'écran. La règle du projet — dire
// le texte EXACT du client — vaut quand ce texte existe ; quand il n'existe pas,
// la question ne se pose plus et notre traduction est le seul choix honnête.
const char* Utf8Or(int id, const char* fallback);

// ── Deux mises en forme que TOUTE fenêtre ImGui doit à ces libellés ─────────
// Elles vivaient dans l'anonyme de character_sheet.cc, qui les a découvertes en
// premier ; la fiche de pet en a exactement le même besoin. Une seule
// implémentation, ici — le .cc de la feuille de personnage n'en garde que le
// renvoi, sous ses noms d'origine.

// Remplace les sauts de ligne par des espaces. Certains libellés en portent
// parce qu'ils sont peints sur un bouton bitmap de deux lignes (« Auto \nFeeding ») :
// sur une case à cocher ImGui, le saut casse la mise en page ET entre dans
// l'identifiant du widget. ⚠ Tampon thread-local UNIQUE : un seul vivant à la fois.
const char* Flatten(const char* src);

// Retire les codes couleur `^RRGGBB` du client. Plusieurs libellés natifs en
// portent — MSI_DELETE_HOMUN en a DEUX d'affilée — parce que la boîte de dialogue
// du jeu les interprète ; ImGui, lui, les afficherait tels quels.
// ⚠ Tampon ROTATIF sur quatre emplacements, comme `Utf8` : bon pour un affichage
// immédiat, à recopier si la chaîne doit survivre à la frame.
const char* StripColors(const char* src);

}  // namespace msgstr
