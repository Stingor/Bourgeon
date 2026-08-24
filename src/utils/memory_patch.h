#pragma once

// ── Écrire dans le code du client ────────────────────────────────────────────
//
// Le bloc « VirtualProtect / écrire / restituer / FlushInstructionCache » était
// recopié DIX fois, sous quatre noms concurrents — WriteCode(), PatchValue(),
// BIPatchPtr(), PatchSite() — chacun dans le namespace anonyme de son .cc.
//
// 🔴 Et l'un d'eux ne se comportait déjà plus comme les autres : la copie de
// `menu_icons.cc` avait perdu son `FlushInstructionCache`. Même nom, même
// signature, comportement différent — c'est précisément ce que la recopie finit
// toujours par produire, et pourquoi ces quatre lignes méritent un seul foyer.
//
// Ce qui N'A PAS sa place ici, et qu'on a laissé sur place à dessein :
//   • `screen_fx.cc:PatchFloatRO` — garde une comparaison préalable (« ne rien
//     écrire si la valeur est déjà bonne ») qui lui évite deux VirtualProtect
//     toutes les 100 ms. C'est le point de la fonction, pas un détail.
//   • `weapon_dual_sprites.cc:RepointCall` — vide le cache d'instructions sur
//     les CINQ octets de l'instruction, pas sur les quatre qu'il écrit.
//   • `hook_manager.cc`, `chat_bg.cc` — posent une protection qu'ils ne
//     restituent pas, ou installent un saut de cinq octets. Autre métier.

#include <Windows.h>

#include <cstdint>
#include <cstring>

namespace mem {

// Écrit `n` octets à `addr`, la page ouverte le temps de l'écriture seulement.
// Rend false si la page n'a pas pu être ouverte — l'écriture n'a alors PAS eu
// lieu, et l'appelant peut renoncer à son correctif plutôt que de croire le jeu
// modifié.
inline bool WriteCode(uintptr_t addr, const void* bytes, size_t n) {
  void* dst = reinterpret_cast<void*>(addr);
  DWORD old_protect = 0;
  if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old_protect)) return false;
  std::memcpy(dst, bytes, n);
  VirtualProtect(dst, n, old_protect, &old_protect);
  FlushInstructionCache(GetCurrentProcess(), dst, n);
  return true;
}

// Écrit une valeur typée à `addr` : l'immédiat d'une instruction, une entrée de
// vtable, une constante de .rdata. Sans retour — aucun appelant n'a jamais
// vérifié celui des six copies d'origine, et un correctif de démarrage qui
// échoue se voit au comportement du jeu, pas à un booléen.
template <typename T>
void PatchValue(uintptr_t addr, T value) {
  WriteCode(addr, &value, sizeof(T));
}

}  // namespace mem
