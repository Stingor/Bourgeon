#pragma once

#include <cstdint>

// ═══════════════════════════════════════════════════════════════════════════
//  CSkillInfo — la fiche d'UNE compétence, telle que le client la promène
// ═══════════════════════════════════════════════════════════════════════════
//
// 🔴🔴 NE PAS CONFONDRE AVEC `rag::itemlist` (ragnarok/item_info.h). L'IDB donne
// le MÊME nom — `ItemSkillInfo` — à deux structures qui n'ont rien de commun, et
// c'est la confusion la plus coûteuse de ce coin du client :
//
//   · celle-ci se construit par `0x00739700`, porte une **vtable `CSkillInfo`**
//     en +0 et un `const char*` « Unknown-Skill » en +0x20, et se détruit par
//     `itemdb::kFilledInfoDtorAddr` (0x00739cd0) ;
//   · l'autre se construit par `0x006a1b20`, porte **deux `std::string`** en
//     +0x2c et +0x44, et se détruit par `rag::itemlist::kInfoDtorAddr`
//     (0x005a4300).
//
// Les apparier de travers écrit un pointeur de vtable par-dessus le premier
// champ. Et leurs offsets se ressemblent assez pour que la confusion passe :
// +0x04 vaut « fiche utilisable » ici et « index d'inventaire » là-bas, +0x10
// « niveau appris » ici et « quantité » là-bas. Aucun compilateur ne le dira.
//
// 🔴 Cette disposition était écrite dans QUATRE fichiers sous TROIS conventions —
// `kSkOff*` (character_sheet), `kOff*` (homunculus, player_skills) et
// `kSkillInfo*` (homunculus, skill_bar). `homunculus.cc` en déclarait même DEUX
// jeux, dans le même fichier : `kOffValid` et `kSkillInfoFound` sont le même
// +0x04, `kOffLevel` et `kSkillInfoLevel` le même +0x10.
//
// ⚠ Les offsets sont comptés DEPUIS LA FICHE. Quand elle vient d'une `std::list`
// (le bundle du personnage, la liste de l'homoncule), il faut d'abord ajouter
// `rag::listnode::kValue`.

namespace rag::skillinfo {

constexpr int kValid    = 0x04;  // int : 1 = fiche utilisable (« trouvée »)
constexpr int kId       = 0x08;  // int : l'id de compétence
constexpr int kInf      = 0x0c;  // int : masque `skill_get_inf` ; 0 = passive
constexpr int kLevel    = 0x10;  // int : niveau appris — la vue compacte native le bidouille
constexpr int kSp       = 0x14;  // int : coût SP au niveau courant
constexpr int kUpgrade  = 0x18;  // int : le serveur dit « il reste du niveau »
constexpr int kRange    = 0x1c;  // int : portée
constexpr int kResName  = 0x20;  // const char* — le ctor y met « Unknown-Skill »
constexpr int kPos      = 0x24;  // int : case dans la grille (-1 = non placée)
constexpr int kMaxLevel = 0x28;  // int : niveau MAX (source : Lua)
constexpr int kUserUp   = 0x2c;  // int : <= 0 = le joueur ne peut pas la monter
constexpr int kLearned  = 0x30;  // int16 : VÉRITÉ SERVEUR, jamais modifiée en local
constexpr int kNeedVec  = 0x38;  // std::vector<{u32 id, u32 niveau}> : les prérequis

// ⚠ `kLevel` et `kLearned` ne disent PAS la même chose, et c'est le piège de
// cette structure. La vue compacte native écrase `kLevel` pour son affichage ;
// seul `kLearned` reflète ce que le serveur sait. Lire le premier pour décider
// si une compétence est apprise donne un résultat juste la plupart du temps.

// Corroboré au désassembleur (ctor 0x00739700) : il met à zéro +0x04, +0x08,
// +0x10, +0x28, +0x30 (en WORD — d'où l'int16 de `kLearned`) puis +0x34, +0x38,
// +0x3c, +0x40 — soit exactement les trois pointeurs d'un `std::vector` à +0x38.

}  // namespace rag::skillinfo
