#pragma once

// ── La `std::string` du CLIENT ───────────────────────────────────────────────
//
// Le client est bâti avec la STL de MSVC : un champ `std::string` occupe 0x18
// octets — seize qui portent SOIT le texte court, SOIT un pointeur vers le tas,
// puis la taille (+0x10) et la capacité (+0x14). C'est la CAPACITÉ qui tranche :
// au-delà de quinze, le texte est ailleurs.
//
// ⚠ 0x18 est la taille du CHAMP, pas toujours son pas dans une structure : un
// `double` qui le suit s'aligne sur huit et laisse quatre octets de bourrage.
//
// Ces quatre lignes de décodage étaient recopiées ONZE fois, sous six noms —
// CopyClientString(), ReadStdStringSEH(), ReadClientString(), StdStringData(),
// et deux ternaires anonymes au milieu d'une fonction plus grande. Trois
// écritures concurrentes de l'offset de capacité cohabitaient : `0x14` en dur,
// `+ 20` en décimal, et quatre constantes locales de noms différents.
//
// Rien de tout cela ne divergeait encore — mais c'est le genre de tas qui ne
// diverge que le jour où l'on corrige l'un des onze.
//
// ⚠ SEH ⇒ AUCUN objet à destructeur dans les fonctions ci-dessous (C2712 :
// « __try dans une fonction qui exige un déroulement d'objet »). C'est la raison
// de ces tampons bruts, pas un goût pour le C.

// Même parti pris qu'uiwnd.h et game_scene.h : `<excpt.h>` et non `<Windows.h>`,
// pour ne pas imposer Windows à qui ne veut que lire une chaîne.
#include <cstddef>  // size_t
#include <cstdint>
#include <cstring>
#include <excpt.h>  // __try/__except

namespace rag::clientstr {

constexpr int      kSizeOff   = 0x10;  // _Mysize, relatif au CHAMP
constexpr int      kCapOff    = 0x14;  // _Myres
constexpr uint32_t kSsoMax    = 16;    // capacité >= 16 ⇒ le texte est sur le tas
constexpr int      kFieldSize = 0x18;  // le champ ENTIER : pas d'un vector, offset
                                       // du membre suivant dans une structure

// Longueur annoncée. À n'appeler que sous la protection de l'appelant.
inline uint32_t Size(const void* field) {
  return *reinterpret_cast<const uint32_t*>(
      reinterpret_cast<const uint8_t*>(field) + kSizeOff);
}

// Le corps du texte, SSO comprise. NI copie NI protection : réservé aux
// appelants qui sont DÉJÀ dans leur propre `__try` et qui veulent lire le texte
// sur place (un `atoi`, une comparaison) sans le recopier.
inline const char* Data(const void* field) {
  const uint8_t* s = reinterpret_cast<const uint8_t*>(field);
  const uint32_t cap = *reinterpret_cast<const uint32_t*>(s + kCapOff);
  return (cap >= kSsoMax) ? *reinterpret_cast<const char* const*>(s)
                          : reinterpret_cast<const char*>(s);
}

// Recopie le texte dans `out`, en REFUSANT la troncature : rend false si le
// champ est vide, aberrant, ou trop long pour `out_size`. C'est le contrat de
// l'appelant qui n'a rien à faire d'un nom coupé en deux.
inline bool Copy(const void* field, char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  __try {
    if (!field) return false;
    const uint32_t size = Size(field);
    if (size == 0 || size >= out_size) return false;
    const char* src = Data(field);
    if (!src) return false;
    std::memcpy(out, src, size);
    out[size] = '\0';
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; return false; }
}

// Recopie le texte dans `out`, en le TRONQUANT à `cap - 1` caractères. `out` est
// toujours terminé, et vide si la lecture échoue. C'est le contrat de l'appelant
// qui préfère un nom coupé à pas de nom du tout.
//
// La troncature se fait sur la LONGUEUR annoncée et non sur un parcours jusqu'au
// zéro : un champ à moitié réécrit pendant qu'on le lit n'entraîne pas la lecture
// au-delà de son tampon.
inline void CopyTruncating(const void* field, char* out, int cap) {
  if (!out || cap <= 0) return;
  out[0] = '\0';
  __try {
    if (!field) return;
    const char* src = Data(field);
    if (!src) return;
    uint32_t n = Size(field);
    if (n > static_cast<uint32_t>(cap - 1)) n = static_cast<uint32_t>(cap - 1);
    std::memcpy(out, src, n);
    out[n] = '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

}  // namespace rag::clientstr
