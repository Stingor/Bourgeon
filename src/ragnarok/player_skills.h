#pragma once

// ── Compétences APPRISES du personnage ───────────────────────────────────────
//
// Le niveau appris d'une compétence, lu dans le bundle client
// (`CPlayerSkillBundle`, session+0x0C). C'est la même source que le Grimoire de la
// feuille de personnage — cf. docs/skill_tree_re.md partie II.
//
// POURQUOI UN MODULE. Le parcours du bundle existait déjà en DEUX copies (feuille de
// personnage, refine), chacune avec ses sept constantes redéclarées, et un troisième
// consommateur se présentait (les chances de fabrication). C'est exactement la
// dispersion que l'annuaire d'adresses a coûté à nettoyer.
//
// ⚠ En-tête volontairement sans <Windows.h> : la lecture est protégée par SEH, et un
// `__try` est interdit dans toute fonction abritant un objet à destructeur non trivial
// (C2712). Le corps vit donc dans le .cc, jamais en inline.
//
// ✅ DETTE SOLDÉE (2026-08-24). L'en-tête annonçait « les deux copies gardent
// leur parcours, reprendre le reste plus tard ». Repris : le refine appelle
// désormais cette fonction, et ses huit constantes redéclarées ont disparu.
//
// La feuille de personnage, elle, GARDE son parcours — et ce n'est pas un
// abandon : elle ne cherche pas UN niveau, elle relève l'arbre ENTIER (position
// dans la grille, prérequis, dédoublonnage inter-onglets). Deux travaux
// différents qui partagent un squelette, pas une copie.
//
// 🔴 Et la fusion a rendu un défaut : voir le commentaire sur l'absence de
// `break` dans le .cc — les deux lectures « un seul niveau » s'arrêtaient au
// premier match, alors que le Grimoire avait DÉJÀ documenté qu'un onglet peut
// porter deux fiches de la même compétence.

#include <cstdint>

namespace rag {

// ── Le bundle, et par où on y entre ─────────────────────────────────────────
// Les trois adresses du parcours, jusqu'ici recopiées à l'identique dans les
// trois consommateurs (ce module, la feuille de personnage, le refine) — la
// dette que l'entête ci-dessus annonçait, soldée pour la partie ADRESSES.
//
// ⚠ Les deux copies gardent leur PARCOURS : elles fonctionnent et sont sous
// test. Ce qui est mutualisé ici, ce sont les points d'entrée, c'est-à-dire
// exactement ce qui bougera au prochain portage d'exe.
constexpr uintptr_t kSkillBundleAddr   = 0x015fa3cc;  // CPlayerSkillBundle (session+0x0C)
constexpr uintptr_t kSkillFlatListAddr = 0x015fa3e0;  // bundle+0x14 : l'onglet « divers »
constexpr uintptr_t kSkillGetTabListAddr = 0x00738370;  // __thiscall(bundle, tab) -> std::list*

// Niveau APPRIS de la compétence, 0 si elle n'est pas connue (ou illisible).
//
// ⚠ Deux champs portent un niveau dans un nœud du bundle : `+0x30` (int16) est la
// VÉRITÉ SERVEUR — jamais modifiée en local — et `+0x10` ne sert que de repli. Les
// confondre donnerait le niveau d'affichage d'un onglet en cours d'édition.
//
// `sp_cost` (optionnel) reçoit le coût SP AU NIVEAU COURANT, lu sur la MÊME
// fiche. Le demander à part rejouerait tout le parcours pour retomber sur le
// même nœud. 0 = inconnu.
int LearnedSkillLevel(int skill_id, int* sp_cost = nullptr);

}  // namespace rag
