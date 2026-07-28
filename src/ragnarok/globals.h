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

}  // namespace rag
