#pragma once

// ── Lister ce qu'il y a DANS les GRF montés ──────────────────────────────────
//
// Charger un fichier packé n'a jamais posé de problème : `FileMgr_LoadToMemory`
// (cf. ui/spr_act.cc) résout indifféremment le disque et les GRF. Mais il faut
// lui donner un NOM — et un GRF n'est pas un dossier : `FindFirstFile` n'y voit
// rien, ce qui interdit toute galerie « prends celui que tu veux » sur du
// contenu livré avec le serveur.
//
// Ce module ouvre ce chemin-là : il parcourt la table de fichiers que le client
// tient déjà en mémoire pour chaque archive montée. Le GRF de Moonlight est
// CHIFFRÉ (« Event Horizon »), mais cela ne gêne en rien — cette table est le
// résultat DÉCHIFFRÉ du montage, pas le fichier sur disque.
//
// Structures du client 20250716 (base 0x400000), lues, jamais écrites :
//   g_FileMgr+0x00        -> tête de la liste chaînée (circulaire) des archives
//   nœud+0x00             -> suivant ; nœud+0x0C -> l'objet CGrf
//   CGrf+0x24 / +0x28     -> début / fin du vecteur d'entrées (comme un
//                            std::vector : le client y fait sa dichotomie)
//   entrée : 0x120 octets, +0x08 taille réelle, +0x18 drapeaux,
//            +0x1C empreinte, +0x20 nom (char[256], minuscules, antislashs)
// Repères pris dans `Grf_FindEntry 0x00a8a8f0` (`lea ecx, [ebx+1Ch]` : la
// comparaison porte sur {empreinte, nom} à +0x1C) et `Grf_ReadAndDecodeEntry
// 0x00a8a5d0` (`strlen(entrée+32)`). 0x20 + 256 = 0x120 : la structure est
// complète, il n'y a rien après le nom.
//
// ⚠ Le tri du vecteur est celui de l'EMPREINTE (djb2 du nom en minuscules)
// avant celui du nom : deux fichiers d'un même dossier n'y sont pas voisins.
// Un filtre par préfixe se paie donc en balayage complet — c'est fait pour être
// appelé à un moment calme (un scan de galerie), pas à chaque frame.

#include <cstdint>
#include <string>
#include <vector>

namespace rag {

struct GrfFile {
  std::string path;      // nom normalisé du GRF : minuscules, antislashs
  uint32_t    size = 0;  // taille décompressée, en octets
};

// Liste les fichiers des archives montées dont le nom commence par `prefix` et
// se termine par `suffix` (les deux insensibles à la casse ; « / » et « \ » y
// sont équivalents). Un `suffix` vide accepte tout.
//
// Les résultats suivent l'ORDRE DE PRIORITÉ des archives : la liste est
// parcourue de la première à la dernière, et `FileMgr_LoadFromGrf` s'arrête lui
// aussi au premier GRF qui contient l'entrée. Un même nom présent dans deux
// archives sort donc deux fois, et garder la PREMIÈRE occurrence désigne bien
// le fichier que le loader chargera.
//
// Rend false si la table n'a pas pu être lue — offsets devenus faux après une
// mise à jour du client, ou GRF pas encore montés. `out` est alors vide :
// l'appelant dégrade vers ce qu'il sait faire sans (typiquement, le disque
// seul) au lieu d'afficher une liste amputée sans le dire.
bool ListGrfFiles(const char* prefix, const char* suffix,
                  std::vector<GrfFile>* out, int limit = 256);

}  // namespace rag
