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
#include <excpt.h>  // __try / __except — jamais <Windows.h> dans un en-tête

#include "ragnarok/uiwnd.h"  // MakeWindow / CloseWindow / OnMsg / SetPos

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

// `ClientDB_FillInfoById` : __stdcall(ItemSkillInfo* out, int id) -> out.
// Construit l'info dans `out` en allant la chercher dans CELLE des bases du
// client qui couvre cet id.
//
// 🔴 CONTRADICTION TRANCHÉE AU DÉCOMPILATEUR (2026-08-23). Deux fichiers la
// déclaraient, sous DEUX noms qui se contredisaient — `kGetInvItemAddr` (barre
// de raccourcis) et `kSkillEntryFill` (feuille de personnage) — et le premier
// est franchement faux : elle ne touche JAMAIS à l'inventaire. C'est un
// AIGUILLEUR par plage d'identifiant :
//
//     id ∈ [8000,  8060]                  -> base « 8000 »
//     id ∈ [8200,  8241] ∪ [8400, 8457]   -> base « 2008 »
//     id ∈ [10000, 10019]                 -> base « 10000 »
//     sinon                               -> la liste des compétences APPRISES
//
// ⚠ C'est cette dernière branche qui explique le défaut historique de la barre
// de raccourcis : pour la compétence d'une AUTRE classe (personnage GM
// multi-classe), la liste apprise ne rend rien et le nom de ressource sort vide.
// Quand on veut le nom d'une compétence INDÉPENDAMMENT de l'appris, il faut
// passer par `lua::kGetSkillIdNameAddr`, pas par ici.
//
// ⚠ `out` est construit par la fonction : l'appelant DOIT le détruire avec
// `kFilledInfoDtorAddr` ci-dessous.
constexpr uintptr_t kFillInfoByIdAddr = 0x00d7fa90;

// Le destructeur de la structure. 🔴 IL EST OBLIGATOIRE quand on a fait
// remplir un ItemSkillInfo par le client : elle porte des std::string, dont la
// capacité peut déborder le SSO — les abandonner fuit dans le tas DU CLIENT.
// Trois fichiers l'appelaient, sous les noms kSkillEntryDtor, kSkillInfoDtor et
// un littéral au milieu d'un `reinterpret_cast`.
constexpr uintptr_t kFilledInfoDtorAddr = 0x00739cd0;

// ── Noms d'affichage ─────────────────────────────────────────────────────────
// kBuildDisplayNameAddr compose le nom complet (préfixes de cartes, refine,
// grade…) ; kBaseNameFallbackAddr est le repli quand il échoue. Les appels sont
// à protéger par SEH — cf. les copies de `SafeBuildName`.
constexpr uintptr_t kBuildDisplayNameAddr  = 0x008a0570;
constexpr uintptr_t kBaseNameFallbackAddr  = 0x006a2b50;

// Nombre TOTAL d'emplacements de carte : __fastcall(ItemSkillInfo*) -> lit
// descRecord+0x30 ; 0 pour l'enregistrement nul. C'est la source UNIVERSELLE du
// « [N] » — les viewers qui lisent leurs emplacements ailleurs (storage : champ
// d'un paquet serveur ; vending : nœud+kNodeSlots) n'ont ces données que dans
// LEUR contexte, alors que cet appel marche partout où l'on tient un info.
constexpr uintptr_t kSlotCountAddr = 0x006a4c10;

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
constexpr int kItemDescMsgSet  = 0x18;
constexpr int kSkillDescMsgSet = 0x3d;
constexpr int kSkillDescShownId = 0x104;

// Ouvre — ou REFERME — la fiche de compétence à la position donnée.
//
// La bascule n'est pas un raffinement : c'est ce que fait le natif, et l'oublier
// donne une fenêtre qui ne se ferme plus quand on reclique la compétence qui l'a
// ouverte. Elle se joue sur `+0x104`, l'id que la fenêtre affiche déjà.
//
// TROIS fichiers portaient cette séquence — la feuille de personnage, la fiche
// de monstre, et la barre de compétences (celle-là en ligne, au milieu d'une
// fonction plus grande, donc invisible à toute comparaison de fonctions).
inline void OpenSkillDesc(int skill_id, int mx, int my) {
  if (skill_id <= 0) return;
  __try {
    void* wnd = uiwnd::MakeWindow(uiwnd::kSkillDescWndId);
    if (!wnd) return;
    if (*reinterpret_cast<int*>(reinterpret_cast<char*>(wnd) +
                                kSkillDescShownId) == skill_id) {
      uiwnd::CloseWindow(uiwnd::kSkillDescWndId);
      return;
    }
    uiwnd::OnMsg(wnd, kSkillDescMsgSet, skill_id, 0, 0, 0);
    uiwnd::SetPos(wnd, mx, my);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Nom de ressource d'un objet, et classe d'arme ───────────────────────────
// `ItemSkillDB_GetResName(info)` -> le nom de ressource CP949 (celui du .bmp
// d'icône et du .spr), ou vide. ⚠ Il lit `rec+8` ou `rec+0x1C` selon l'octet
// « identifié » à `ItemSkillInfo+0x5c` — poser cet octet à 1 avant l'appel est
// donc ce que fait tout appelant qui monte une fiche autonome.
constexpr uintptr_t kGetResNameAddr = 0x006a4bc0;

// `Weapon_ItemIdToWeaponClass(id)` __stdcall, `retn 4` : l'id d'objet -> la
// classe d'arme au sens du RENDU (celle qui choisit le .spr tenu en main et
// l'animation). Deux effets s'en servent — le tir de baguette pour savoir si
// l'arme est à distance, les sprites duals pour choisir la couche.
constexpr uintptr_t kItemIdToWeaponClassAddr = 0x00d8a1d0;

}  // namespace itemdb
