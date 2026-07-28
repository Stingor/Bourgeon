#pragma once

// ── Base de descriptions item/skill, et fenêtre qui l'affiche ────────────────
// (client 20250716, base 0x400000 ; cf. docs/npc_dialog_re.md et
//  project_item_skill_desc_window_re)
//
// Neuf adresses et quatre identifiants qui allaient TOUJOURS ensemble — ouvrir
// la description d'un objet, c'est charger le DB, construire un ItemSkillInfo,
// puis le passer à la fenêtre — mais qui étaient redéclarés dans sept à huit
// fichiers, sous jusqu'à trois orthographes chacun (kInfoCtor, kItemInfoCtor,
// kItemSkillInfoCtor…). C'est aussi le socle du chantier « généraliser
// DrawItemCell » : la cellule d'item de vending_window sert de modèle, et ses
// briques (SafeBuildName, OpenItemDesc) sont recopiées dans six fichiers.
//
// En-tête MINUSCULE (<cstdint> seul), comme uiwnd.h et globals.h.

#include <cstdint>

namespace itemdb {

// ── La base elle-même ────────────────────────────────────────────────────────
// Un std::map (arbre rouge-noir MSVC) id -> enregistrement de description.
// kNilAddr est le nœud sentinelle de l'arbre : une recherche qui y aboutit
// signifie « absent », elle ne doit PAS être déréférencée.
constexpr uintptr_t kTableAddr  = 0x01255130;  // la map
constexpr uintptr_t kNilAddr    = 0x01255138;  // sentinelle de l'arbre
constexpr uintptr_t kLookupAddr = 0x006a0d40;  // __cdecl(id, &table) -> record

// Chargement paresseux : le DB n'est peuplé qu'à la première demande.
// kEnsureCachePtr est l'emplacement d'un POINTEUR (à déréférencer), qui sert de
// `this` à kEnsureLoadedAddr.
constexpr uintptr_t kEnsureLoadedAddr = 0x006a06b0;
constexpr uintptr_t kEnsureCachePtr   = 0x0125510c;

// ── ItemSkillInfo : ce que la fenêtre de description attend ──────────────────
// La structure est COMMUNE aux objets et aux skills — d'où son nom. Un info
// construit puis SetId() porte l'id mais PAS le texte de description : pour une
// description complète il faut passer l'info que le SERVEUR a remplie (le nœud
// de la liste d'inventaire/storage), pas un info reconstruit.
constexpr uintptr_t kInfoCtorAddr  = 0x006a1b20;
constexpr uintptr_t kInfoSetIdAddr = 0x006a6570;

// ── Noms d'affichage ─────────────────────────────────────────────────────────
// kBuildDisplayNameAddr compose le nom complet (préfixes de cartes, raffinage,
// grade…) ; kBaseNameFallbackAddr est le repli quand il échoue. Les appels sont
// à protéger par SEH — cf. les copies de `SafeBuildName`.
constexpr uintptr_t kBuildDisplayNameAddr  = 0x008a0570;
constexpr uintptr_t kBaseNameFallbackAddr  = 0x006a2b50;

// ── Les deux fenêtres de description ─────────────────────────────────────────
// ⚠ Ce sont DEUX fenêtres distinctes, et leur appariement id/message a été
// établi à la dure — ne pas les intervertir (skill_bar l'avait fait, cf. le
// correctif f6a58b3) :
//
//   OBJET : fenêtre 0x0c, message 0x18, paramètre = &ItemSkillInfo (STRUCT)
//   SKILL : fenêtre 0x2e, message 0x3d, paramètre = id BRUT
//
// Sur la fenêtre 0x2e, +0x104 porte l'id actuellement affiché : le natif s'en
// sert pour basculer (re-cliquer le même skill referme la fenêtre).
constexpr int kItemDescWndId   = 0x0c;
constexpr int kItemDescMsgSet  = 0x18;
constexpr int kSkillDescWndId  = 0x2e;
constexpr int kSkillDescMsgSet = 0x3d;
constexpr int kSkillDescShownId = 0x104;

}  // namespace itemdb
