#pragma once

// ── Globales du client (20250716, base 0x400000) ─────────────────────────────
// Point de vérité UNIQUE pour les adresses d'état de jeu les plus copiées du
// projet. Chacune était redéclarée dans 8 à 13 fichiers, sous deux à trois
// orthographes, dans autant de namespaces anonymes — donc invisibles les unes
// aux autres, et introuvables d'un seul grep au moment de porter le client sur
// une autre version d'exe.
//
// En-tête volontairement MINUSCULE (rien que <cstdint>), sur le modèle de
// ragnarok/uiwnd.h. À NE PAS confondre avec ragnarok/session.h, qui est la
// classe proxy Session (yaml-cpp, item_info, talktype) : un tout autre sujet et
// un en-tête bien plus lourd.

#include <cstdint>

namespace rag {

// ── Session ──────────────────────────────────────────────────────────────────
// ⚠ C'est l'OBJET lui-même, PAS un pointeur vers lui : les 13 fichiers qui
// l'utilisent font tous soit `kSessionAddr + offset` pour lire un champ, soit le
// passent en `this`. Aucun ne déréférence. (Même forme que g_UIWindowMgr.)
constexpr uintptr_t kSessionAddr = 0x015fa3c0;

inline void* Session() { return reinterpret_cast<void*>(kSessionAddr); }

// Champ de la session à l'offset donné, ex. rag::SessionField<int>(0x17d0).
template <typename T>
inline T SessionField(int byte_offset) {
  return *reinterpret_cast<T*>(kSessionAddr + byte_offset);
}

// ── Zeny du joueur ───────────────────────────────────────────────────────────
// ⚠ NE PAS confondre avec `ci::kZeny`, qui est un OFFSET dans une structure
// d'info de personnage. Ici c'est une adresse absolue.
constexpr uintptr_t kZenyAddr = 0x015fba90;

inline int Zeny() { return *reinterpret_cast<int*>(kZenyAddr); }

// ── Gestionnaire de modes, et mode actif ─────────────────────────────────────
// Le projet appelait cet objet de TROIS noms — kModeMgr, kModeArg et kDragMgr —
// sans que rien ne signale qu'il s'agit du même. `kDragMgr` est le plus
// trompeur : il laisse croire à un gestionnaire de drag séparé, alors que
// skill_bar l'a simplement nommé d'après ce qu'il lit dans l'objet rendu (la
// charge du drag en cours, à +0x308).
constexpr uintptr_t kModeMgrAddr = 0x01213338;

// L'emplacement du POINTEUR vers le mode de zone actif (0 si aucun) : à
// DÉRÉFÉRENCER, contrairement aux deux adresses ci-dessus. Neuf fichiers le
// lisent sous les noms kUICmdDisp / kDispatcherPtr — c'est le `this` du
// dispatcher CMode::SendMsg.
constexpr uintptr_t kActiveModePtr = 0x0121333c;

inline void* ActiveMode() { return *reinterpret_cast<void**>(kActiveModePtr); }

// Accesseur natif « objet actif du manager ». Dix fichiers l'appellent, sous les
// noms kGameModeGet, kGetMode et kGetDragObj, TOUJOURS sur kModeMgrAddr.
//
// ⚠ HYPOTHÈSE NON VÉRIFIÉE, à confirmer au désassemblage avant d'agir dessus :
// kActiveModePtr valant exactement kModeMgrAddr + 4, cette fonction pourrait
// n'être qu'un `return *(mgr + 4)` — auquel cas l'appel et la lecture directe
// donnent la même valeur, ce que suggère le commentaire d'un des appelants
// (« *(kDispatcherPtr) = mode zone actif (ou 0) »). Les deux formes sont donc
// CONSERVÉES telles quelles : tant que le corps de 0x00a75340 n'a pas été lu,
// remplacer l'appel par la lecture ferait disparaître un éventuel test de
// nullité ou appel virtuel.
constexpr uintptr_t kModeMgrGetActiveAddr = 0x00a75340;

// ── Stats du personnage, et le TOTAL que le serveur utilise ──────────────────
// Deux blocs de six entiers, pas de pas de 4, dans l'ordre STR AGI VIT INT DEX LUK.
// Déjà lus par la feuille de personnage (donc éprouvés en jeu) — c'est de là qu'ils
// viennent, et ils étaient sur le point d'être recopiés une troisième fois.
//
// 🔴 LE TOTAL EST BASE + BONUS, et c'est cette somme-là qu'il faut pour rejouer un
// calcul serveur : `status->dex` côté rAthena est la stat effective, équipement et
// cartes comprises. Le bloc « base » seul donnerait systématiquement trop bas.
//
// ⚠ NE PAS employer `dex_base_` / `luk_base_` du layout de session (+0x1674/+0x1678) :
// ce sont les bases, et ce fichier de layout porte déjà un offset marqué « CONFIRMED »
// qui a fait planter le client (cf. son entête). Ces deux globales-ci sont lues par du
// code vivant, ce qui est une garantie d'un autre ordre.
constexpr uintptr_t kStatBaseAddr  = 0x015fba24;
constexpr uintptr_t kStatBonusAddr = 0x015fba0c;

enum Stat { kStr = 0, kAgi, kVit, kInt, kDex, kLuk, kStatCount };

inline int StatBase(Stat s) {
  return *reinterpret_cast<int*>(kStatBaseAddr + static_cast<int>(s) * 4);
}
inline int StatBonus(Stat s) {
  return *reinterpret_cast<int*>(kStatBonusAddr + static_cast<int>(s) * 4);
}
// La stat EFFECTIVE — celle que la fenêtre Status affiche comme « base + bonus » et
// que le serveur nomme `status->dex`.
inline int StatTotal(Stat s) { return StatBase(s) + StatBonus(s); }

// ── Niveaux de base et de job ────────────────────────────────────────────────
// Recopiés sous les noms kJobLvl / kBaseLvl (feuille de personnage) et kJobLevel
// (refine, pour les chances de refine).
constexpr uintptr_t kBaseLevelAddr = 0x015fb9f0;
constexpr uintptr_t kJobLevelAddr  = 0x015fb9f8;

inline int BaseLevel() { return *reinterpret_cast<int*>(kBaseLevelAddr); }
inline int JobLevel()  { return *reinterpret_cast<int*>(kJobLevelAddr); }

}  // namespace rag
