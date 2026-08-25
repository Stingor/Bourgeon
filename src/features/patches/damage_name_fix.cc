#include "features/patches/damage_name_fix.h"
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "ragnarok/globals.h"  // rag::ActiveModeIfReady
#include "utils/log_console.h"
#include "utils/memory_patch.h"  // mem::WriteCode

namespace {

// ── Adresses (client 20250716, no-ASLR : addr Ghidra == live) ────────────────
constexpr uintptr_t kProcessDamageAction  = 0x00c5dfc0;  // CActorSprite_ProcessDamageAction
constexpr uintptr_t kProcessDamageCall    = 0x00c4d0a7;  // son appelant, UNIQUE
constexpr uintptr_t kCopyEntityName       = 0x00c68e50;  // GameMode_CopyEntityName

// Les deux appels du REJEU, et eux seuls. Relevés sur le désassemblage :
//
//     c480c9  mov edi, [esi+8]     ; noeud+0x08 = GID de la CIBLE
//     c480d1  mov eax, [esi+18h]   ; noeud+0x18 = GID de l'ATTAQUANT
//     c480f3  push edi             ; le gid
//     c480fa  push ecx             ; la sortie (std::string)
//     c480fd  call GameMode_CopyEntityName
constexpr uintptr_t kCopyEntityNameCalls[] = {
    0x00c480fd,  // le nom de la CIBLE — celui qui manque dans « [%s] subit … »
    0x00c48122,  // le nom de l'ATTAQUANT
};

// Champs de l'acteur, tous deux lus par `CActorSprite_ProcessDamageAction`.
constexpr int kActeur_GidPropre = 0x110;  // l'attaquant, c'est-à-dire `this`
constexpr int kActeur_GidCible  = 0x24c;  // écrit en tête, depuis le paquet

// ── Le `std::string` du client ───────────────────────────────────────────────
// MSVC, x86 : seize octets de petit tampon, puis la taille et la capacité. Une
// capacité >= 16 signifie que le texte est sur le TAS et que l'union porte alors
// un pointeur. Le client lui-même le lit ainsi (`if (v52 >= 0x10) v8 = Buf[0]`).
struct ChaineMsvc {
  union {
    char sso[16];
    char* tas;
  };
  uint32_t taille;
  uint32_t capacite;
};
static_assert(sizeof(ChaineMsvc) == 24, "disposition de std::string inattendue");

// La capacité d'une chaîne restée dans son petit tampon : quinze caractères
// plus le zéro final.
constexpr uint32_t kCapaciteSso = 15;

// On ne retient que ce qu'on saura RÉÉCRIRE sans allouer. Voir Restituer().
struct NomCourt {
  char texte[kCapaciteSso + 1];
};

// Borne de sécurité : au changement de carte, les GID d'hier ne reviendront
// jamais. Plutôt qu'un mécanisme d'expiration, on repart de zéro.
constexpr size_t kNomsMax = 4096;

std::unordered_map<uint32_t, NomCourt> g_noms;

using CopyNameFn      = void*(__fastcall*)(void* mode, void* edx, void* sortie, uint32_t gid);
// 🔴 QUATRE arguments pile, pas deux : la fonction finit sur `retn 10h` et le
// site d'appel pousse bien quatre valeurs (`push [ebx+24h] / push esi / push edi
// / push edx`). Hex-Rays n'en declare que deux, les seuls qu'elle reference — en
// recopier la signature aurait laisse 8 octets sur la pile a chaque coup porte.
using ProcessDamageFn = int(__fastcall*)(void* self, void* edx, int a1, int a2,
                                         int a3, int a4);
using OperatorDeleteFn = void(__cdecl*)(void*);

CopyNameFn      g_copy_natif    = reinterpret_cast<CopyNameFn>(kCopyEntityName);
ProcessDamageFn g_process_natif = reinterpret_cast<ProcessDamageFn>(kProcessDamageAction);

// ⚠ La struct RESTE — son union et son `static_assert` documentent la
// disposition mieux qu'un décalage nu — mais la RÈGLE SSO vient du foyer.
const char* Texte(const ChaineMsvc* chaine) {
  return rag::clientstr::Data(chaine);
}

void Memoriser(uint32_t gid, const ChaineMsvc* chaine) {
  if (gid == 0 || chaine->taille == 0 || chaine->taille > kCapaciteSso)
    return;
  if (g_noms.size() >= kNomsMax)
    g_noms.clear();

  NomCourt nom{};
  std::memcpy(nom.texte, Texte(chaine), chaine->taille);
  nom.texte[chaine->taille] = '\0';
  g_noms[gid] = nom;
}

// 🔴 On n'écrit QUE dans une chaîne vide restée en petit tampon. Le client
// vient de la copie-construire depuis une entrée de dictionnaire vide, donc
// c'est exactement l'état attendu — et cela garantit qu'il n'y a rien à
// libérer, ni rien à allouer avec un CRT qui n'est pas le nôtre.
bool Restituer(uint32_t gid, ChaineMsvc* chaine) {
  if (chaine->taille != 0 || chaine->capacite != kCapaciteSso)
    return false;

  const auto trouve = g_noms.find(gid);
  if (trouve == g_noms.end())
    return false;

  const size_t longueur = std::strlen(trouve->second.texte);
  if (longueur == 0 || longueur > kCapaciteSso)
    return false;

  std::memcpy(chaine->sso, trouve->second.texte, longueur + 1);
  chaine->taille = static_cast<uint32_t>(longueur);
  return true;
}

// Demande le nom au client pour l'apprendre, sans passer par nos propres
// détours : `g_copy_natif` pointe la fonction, pas un site d'appel patché.
void Apprendre(uint32_t gid) {
  if (gid == 0)
    return;

  void* mode = rag::ActiveModeIfReady();
  if (mode == nullptr)
    return;

  // `CNameDict_GetName` COPIE-CONSTRUIT dans la sortie : le tampon n'a pas
  // besoin d'être initialisé, mais il faut le détruire comme le client l'aurait
  // fait — avec SON `operator delete`, et seulement si le texte a débordé du
  // petit tampon (un nom de joueur long, jamais un nom de monstre).
  ChaineMsvc tampon{};
  g_copy_natif(mode, nullptr, &tampon, gid);
  Memoriser(gid, &tampon);
  if (tampon.capacite >= 16 && tampon.tas != nullptr)
    reinterpret_cast<OperatorDeleteFn>(rag::kGameOperatorDeleteAddr)(tampon.tas);
}

// Déportée pour que le `__try` ne cohabite pas avec un objet à destructeur
// (MSVC C2712) : la table vit dans les fonctions appelées, pas ici.
void ApprendreLesDeuxNoms(void* acteur) {
  __try {
    const uint8_t* champs = static_cast<const uint8_t*>(acteur);
    Apprendre(*reinterpret_cast<const uint32_t*>(champs + kActeur_GidPropre));
    Apprendre(*reinterpret_cast<const uint32_t*>(champs + kActeur_GidCible));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void ApprendreOuRestituer(void* sortie, uint32_t gid) {
  __try {
    ChaineMsvc* chaine = static_cast<ChaineMsvc*>(sortie);
    if (chaine->taille != 0)
      Memoriser(gid, chaine);  // le nom est bon : on l'apprend au passage
    else
      Restituer(gid, chaine);  // le client allait écrire « Quelqu'un »
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// ── Les deux détours ────────────────────────────────────────────────────────
// Au COUP : la cible est vivante, le dictionnaire la connaît encore. C'est le
// seul instant qui sauve le cas extrême — à très haute ASPD le monstre meurt
// avant que la moindre ligne différée ne sorte, donc il n'existe aucun
// affichage réussi dont on aurait pu apprendre son nom.
int __fastcall ProcessDamageDetour(void* self, void* edx, int a1, int a2, int a3,
                                   int a4) {
  const int resultat = g_process_natif(self, edx, a1, a2, a3, a4);
  ApprendreLesDeuxNoms(self);
  return resultat;
}

// Au REJEU, plusieurs centaines de millisecondes plus tard.
void* __fastcall CopyEntityNameDetour(void* mode, void* edx, void* sortie,
                                      uint32_t gid) {
  void* resultat = g_copy_natif(mode, edx, sortie, gid);
  ApprendreOuRestituer(sortie, gid);
  return resultat;
}

// Réécrit un `E8 rel32` après avoir vérifié qu'il vise bien ce qu'on croit. Sur
// toute autre disposition on s'abstient : écrire cinq octets au hasard dans le
// chemin des dégâts tuerait le client.
bool PatcherAppel(uintptr_t site, uintptr_t attendu, uintptr_t detour) {
  const uint8_t* octets = reinterpret_cast<const uint8_t*>(site);
  const int32_t relatif = *reinterpret_cast<const int32_t*>(site + 1);
  if (octets[0] != 0xE8 || site + 5 + relatif != attendu) {
    LogDiag("[DamageNameFix] site 0x{:08X} inattendu : {:02X} {:02X} {:02X} {:02X} {:02X}, "
            "cible calculee 0x{:08X} (attendu E8 -> 0x{:08X}) : site IGNORE",
            site, octets[0], octets[1], octets[2], octets[3], octets[4],
            static_cast<uintptr_t>(site + 5 + relatif), attendu);
    return false;
  }

  uint8_t patch[5] = {0xE8, 0, 0, 0, 0};
  const int32_t nouveau = static_cast<int32_t>(detour - (site + 5));
  std::memcpy(patch + 1, &nouveau, sizeof(nouveau));
  if (!mem::WriteCode(site, patch, sizeof(patch))) {
    LogDiag("[DamageNameFix] page non ouvrable en 0x{:08X} : site IGNORE", site);
    return false;
  }
  return true;
}

}  // namespace

DamageNameFix::DamageNameFix() {
  // L'ordre compte : sans l'apprentissage, la restitution n'aurait rien à
  // rendre. On ne pose donc les détours du rejeu QUE si celui du coup a pris.
  if (!PatcherAppel(kProcessDamageCall, kProcessDamageAction,
                    reinterpret_cast<uintptr_t>(&ProcessDamageDetour))) {
    LogDiag("[DamageNameFix] apprentissage NON pose : correctif inactif");
    return;
  }

  // Un site qui echoue se signale tout seul depuis PatcherAppel ; il n'y a
  // rien a compter ici.
  for (uintptr_t site : kCopyEntityNameCalls)
    PatcherAppel(site, kCopyEntityName,
                 reinterpret_cast<uintptr_t>(&CopyEntityNameDetour));
}
