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
// 🔜 Les deux copies existantes n'ont PAS été migrées : elles fonctionnent et sont
// sous test. À reprendre quand le chantier fabrication sera soldé.

namespace rag {

// Niveau APPRIS de la compétence, 0 si elle n'est pas connue (ou illisible).
//
// ⚠ Deux champs portent un niveau dans un nœud du bundle : `+0x30` (int16) est la
// VÉRITÉ SERVEUR — jamais modifiée en local — et `+0x10` ne sert que de repli. Les
// confondre donnerait le niveau d'affichage d'un onglet en cours d'édition.
int LearnedSkillLevel(int skill_id);

}  // namespace rag
