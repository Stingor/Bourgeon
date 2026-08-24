#pragma once

// ── Lire un fichier du client, GRF ou disque ─────────────────────────────────
// (client 20250716, base 0x400000 ; cf. reference_resource_io_loaders)
//
// Le gestionnaire de fichiers du client résout indifféremment un chemin sur
// disque et une entrée de GRF — y compris d'un GRF chiffré, puisqu'il travaille
// sur la table déjà montée. C'est la seule façon correcte de lire une ressource
// du jeu : ouvrir le fichier soi-même marche pour le disque et échoue en silence
// pour tout ce qui est packé.
//
// Trois fichiers déclaraient ce trio, sous deux orthographes chacun
// (kFileMgr/kFileMgrAddr, kLoadToMemory/kLoadToMemoryAddr,
// kFreeBuffer/kFreeBufferAddr).
//
// ⚠ La LISTE de ce que contiennent les archives est un autre sujet, plus lourd :
// ragnarok/grf_index.h.

#include <cstdint>

namespace filemgr {

// L'OBJET g_FileMgr, pas un pointeur vers lui : il se passe tel quel en `this`.
constexpr uintptr_t kFileMgrAddr = 0x0159d410;

inline void* Mgr() { return reinterpret_cast<void*>(kFileMgrAddr); }

// `FileMgr_LoadToMemory` __thiscall(mgr, path, DWORD* out_size, char) -> void*
// Alloue et rend le contenu ; `out_size` reçoit la taille. nullptr si le chemin
// n'existe ni sur disque ni dans un GRF.
//
// 🔴 Le chemin est en CP949, pas en UTF-8 : les dossiers du GRF sont coréens
// (`유저인터페이스\`, `몬스터\`). Un littéral écrit dans nos sources — qui sont en
// UTF-8 — ne désigne aucune entrée. Lire le gabarit DANS le binaire du client,
// ou convertir.
constexpr uintptr_t kLoadToMemoryAddr = 0x00a88ab0;

// `FileMgr_FreeBuffer` __stdcall(void*) : rend le tampon ci-dessus.
//
// ⚠ C'est CET allocateur-là, pas le `free` de notre DLL ni l'operator delete du
// client (`rag::kGameOperatorDeleteAddr`) : le tampon vient d'un troisième
// chemin, et le rendre au mauvais corrompt le tas.
constexpr uintptr_t kFreeBufferAddr = 0x00a892c0;

}  // namespace filemgr
