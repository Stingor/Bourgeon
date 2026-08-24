#pragma once

// ── De quelle NATURE est une entité ? ────────────────────────────────────────
//
// Le client range joueurs, monstres, PNJ, portails et unités spéciales dans un
// même champ « job id », et sépare les familles par de simples plages. Ses
// fonctions feuilles (`Job_IsPlayerJobId`, `Job_IsMonsterId`, …) ne font rien
// d'autre que ces comparaisons — on les réimplémente donc plutôt que de les
// appeler : aucun appel natif, donc utilisables même à moitié chargé, et depuis
// un `__try` sans rien risquer.
//
// Ces prédicats étaient recopiés dans CINQ fichiers — cast_bar, entity_names,
// target_frame, entity_context_menu et quick_cast — trois d'entre eux portant
// la paire complète mot pour mot, avec un paramètre tantôt `unsigned` tantôt
// `uint32_t`. Une plage corrigée à un seul endroit aurait fait dire à la barre
// d'incantation et à l'étiquette de nom deux choses différentes du même acteur.
//
// RE : docs/entity_nameplate_re.md §3.

#include <cstdint>

namespace rag {

// Un JOUEUR : les classes de base et leurs avancées, puis la plage moderne.
inline bool IsPlayerJob(uint32_t id) {
  return (id <= 0x1e) || (id - 0xfa1u <= 0x7ceu);
}

// Un MONSTRE. La seconde plage est celle des invocations et des mercenaires.
inline bool IsMonsterJob(uint32_t id) {
  return (static_cast<int>(id) >= 0x3e9 && static_cast<int>(id) <= 0xf9e) ||
         (id - 0x4e35u <= 0x2ecau);
}

// Un PNJ, ou un portail : le client ne les distingue pas par le job.
inline bool IsNpcOrPortalJob(uint32_t id) {
  return (id >= 45 && id < 1000) || (id - 10001u <= 0x270du);
}

// Les unités posées au sol (pièges, zones de compétence).
inline bool IsSpecialUnitJob(uint32_t id) { return id - 6001u <= 0x33u; }

// Le warp : le natif n'y attache aucune action de clic.
constexpr uint32_t kJobPortal = 45;

}  // namespace rag
