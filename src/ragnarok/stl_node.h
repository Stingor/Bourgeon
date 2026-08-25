#pragma once

#include <cstdint>

// ═══════════════════════════════════════════════════════════════════════════
//  Les nœuds des conteneurs MSVC 32 bits que le client promène partout
// ═══════════════════════════════════════════════════════════════════════════
//
// 🔴 CE FICHIER EXISTE PARCE QUE CES OFFSETS SONT INVISIBLES À TOUT RELEVÉ.
// Trop petits pour un balayage d'adresses (qui commence à 0x400000), trop courts
// pour une comparaison de corps de fonction, et l'expression qui les emploie
// diffère juste assez d'un fichier à l'autre pour échapper au relevé
// d'expressions. Mesuré avant écriture : la valeur d'une `std::list` était
// déclarée dans DIX fichiers sous SIX noms — `kNodeValue`, `kSkNodeValue`,
// `kNode_Actor`, `kNodeActor`, `kNode_Data`, `kNodeInfo` — et celle d'un arbre
// dans CINQ sous TROIS conventions (`kNodeLeft`, `kNode_Left`, `kNodeIsNilOff`).
//
// 🔴🔴 DEUX ESPACES DE NOMS, ET JAMAIS UN PRÉFIXE COMMUN. `kValue` vaut **0x10**
// dans un arbre et **0x08** dans une liste. Les réunir sous un seul `kNode*`
// referait exactement le piège qu'on vient de fermer sur `ItemSkillInfo`, où deux
// structures partageaient un préfixe et où `+0x04` voulait dire « index
// d'inventaire » ici et « fiche utilisable » là. Écrire `treenode::` ou
// `listnode::` OBLIGE à dire de quel conteneur on parle : c'est tout l'intérêt.

namespace rag {

// ── Nœud de `std::_Tree` (map / set) ────────────────────────────────────────
//
//   { _Left, _Parent, _Right, _Color:1o, _Isnil:1o, [2o de bourrage], _Myval }
//
// ⚠ `_Isnil` marque la SENTINELLE, et une `std::map` MSVC en a une : le parcours
// s'arrête dessus, pas sur un pointeur nul. Un parcours qui teste `!= nullptr`
// tourne jusqu'à son garde-fou.
//
// ⚠ Pour une `map`, `_Myval` est une `std::pair` — la CLÉ est donc en tête, à
// `kValue`, et la donnée derrière elle.
namespace treenode {
constexpr int kLeft   = 0x00;
constexpr int kParent = 0x04;
constexpr int kRight  = 0x08;
constexpr int kColor  = 0x0c;  // 1 octet
constexpr int kIsNil  = 0x0d;  // 1 octet : la sentinelle
constexpr int kValue  = 0x10;
}  // namespace treenode

// ── Nœud de `std::list` ─────────────────────────────────────────────────────
//
//   { _Next, _Prev, _Myval }
//
// ⚠ Les listes du client sont CIRCULAIRES, et leur sentinelle est ce que le
// global CONTIENT, pas le global lui-même : comparer au global fait boucler
// jusqu'au garde-fou (cf. `itemcell::CountById`, qui porte le piège en
// commentaire).
namespace listnode {
constexpr int kNext  = 0x00;
constexpr int kPrev  = 0x04;
constexpr int kValue = 0x08;
}  // namespace listnode

}  // namespace rag
