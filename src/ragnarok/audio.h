#pragma once

// ── Le son du client ─────────────────────────────────────────────────────────
// (client 20250716, base 0x400000 ; moteur Miles, cf. reference_audio_miles)
//
// Cinq fichiers déclaraient le gestionnaire de son, sous trois noms
// (kSoundMgrPtr, kSoundMgrAddr, et deux fois en littéral au milieu d'une
// expression — donc invisibles au grep). Deux d'entre eux avaient chacun
// redécouvert, à leurs frais, le piège de la convention d'appel ci-dessous.
//
// En-tête volontairement MINUSCULE (`<cstdint>` seul), comme uiwnd.h.

#include <cstdint>

namespace audio {

// L'emplacement d'un POINTEUR vers le gestionnaire : à DÉRÉFÉRENCER une fois.
// Nul tant que le moteur n'est pas monté.
constexpr uintptr_t kSoundMgrPtr = 0x01253d0c;

inline void* SoundMgr() { return *reinterpret_cast<void**>(kSoundMgrPtr); }

// `Sound_Play3D` : joue un effet positionnel. C'est le chemin du client, donc il
// honore tout seul le réglage « effets sonores » du joueur — ne pas le
// court-circuiter pour le rendre « silencieux quand il faut ».
//
// 🔴 HUIT ARGUMENTS DE PILE, PAS SEPT. La fonction finit sur `RET 0x20` : au-delà
// de {nom, x, y, z, distMax, distMin, volume} elle en dépile un huitième, que le
// natif passe toujours à 0. N'en pousser que sept sous-dépile la pile de quatre
// octets et fait crasher L'APPELANT juste après le retour — un plantage qui ne
// ressemble en rien à un problème de son, et que le mini-jeu Peggle a payé une
// fois (« crash au contact d'une bille »).
//
// `this` = SoundMgr(), passé en ECX : on l'appelle donc en __fastcall avec un
// EDX factice, la façon habituelle d'émuler un __thiscall dans ce projet.
//
// ⚠ AU LOGIN, ELLE EST INUTILISABLE : elle est gatée par un état de jeu que
// l'écran de connexion n'a pas encore. La parade d'ouverture passe par un autre
// chemin — cf. features/overlays/login_parade.cc.
constexpr uintptr_t kPlay3DAddr = 0x00600770;

}  // namespace audio
