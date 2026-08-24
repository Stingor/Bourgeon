#pragma once

#include <cstddef>  // size_t (ActorSlotSpritePath)

// ── Ce que le client a DÉJÀ résolu sur l'acteur du joueur ────────────────────
//
// Un personnage à l'écran n'est pas fait que de son corps : il tient une arme,
// porte un bouclier, tire un chariot. Ces sprites-là ne se déduisent pas d'un
// id d'objet par une petite table — leur résolution native
// (`CActorSprite_BuildWeaponLayers` 0x00d403a0) enchaîne une classe d'arme, un
// « seau » de bouclier, une demi-douzaine de cas particuliers de montures, et
// deux SONDAGES d'existence de fichier dont dépend le repli.
//
// 🔴 On ne la recopie donc pas. Le client l'a déjà faite, et il garde le
// résultat : chaque emplacement de l'acteur pointe une ressource chargée, qui
// porte son propre chemin de fichier. On le LIT. C'est le même réflexe que pour
// la table des noms de coiffes — la vérité vit dans le client, pas dans une
// copie qui divergera au premier costume ajouté.
//
// ⚠ Ne marche qu'EN JEU : hors session, il n'y a pas d'acteur. Les appelants
// qui servent aussi le char-select doivent traiter l'échec comme « mains nues »,
// pas comme une erreur.

namespace rag {

// Chemins de fichiers du VFS, SANS extension — directement consommables par
// `ro::LoadSpritePair`. Vide = l'emplacement n'a rien.
//
// Les .spr et .act sont donnés séparément parce que rien ne garantit qu'ils
// partagent leur base : c'est déjà faux pour la cape, dont le .spr peut venir
// d'une disposition « plate » que le .act n'a pas.
struct OwnActorSprites {
  char weapon_spr[260];
  char weapon_act[260];
  // « Traînée » de l'arme : l'emplacement voisin, souvent vide, que le natif
  // remplit pour certaines classes d'armes (le deuxième fer d'une arme double,
  // la lame d'un fourreau…). Il se dessine SOUS l'arme.
  char trail_spr[260];
  char trail_act[260];
  char shield_spr[260];
  char shield_act[260];
  // Chariot. Ce n'est pas un emplacement de l'acteur mais un SOUS-ACTEUR
  // enfant ; il est ici parce qu'il se lit de la même façon et sert le même
  // appelant.
  char cart_spr[260];
  char cart_act[260];
};

// Remplit `out` depuis l'acteur du joueur. Rend false s'il n'y a pas d'acteur
// (hors jeu, changement de carte) — `out` est alors entièrement vidé, donc
// exploitable sans test supplémentaire.
//
// Un emplacement n'est retenu que si ses DEUX ressources sont là : un
// emplacement d'arme périmé n'en garde souvent qu'une, et le dessiner
// ressusciterait une arme déséquipée.
bool ReadOwnActorSprites(OwnActorSprites* out);

// Le chemin du `.spr` chargé dans un emplacement de N'IMPORTE QUEL acteur —
// base sans extension, préfixée `data\`, exactement comme les champs ci-dessus.
//
// C'est la même lecture que `ReadOwnActorSprites`, ouverte à un acteur qu'on
// nous désigne : le composite d'un AUTRE joueur porte ses ressources aux mêmes
// offsets, et c'est le seul moyen de savoir quel sprite de corps le client a
// résolu pour lui sans recopier toute la logique de classe et de monture.
//
// 🔴 L'emplacement **0 est le CORPS** (cf. les trois branches de
// `CActorSprite_BuildPartQuads` : partie 0 = corps, partie 1 = tête) ; 5 =
// l'arme, 6 = sa traînée, 7 = le bouclier.
//
// ⚠ `actor` n'est pas validé — l'appelant doit tenir d'une source sûre qu'il
// pointe un acteur vivant. Les déréférencements sont sous SEH, donc un pointeur
// mort rend false au lieu de tuer le client, mais ce n'est pas une licence.
bool ActorSlotSpritePath(void* actor, int slot, char* out, size_t out_size);

// ── Poser ou retirer un EFFET sur un acteur ─────────────────────────────────
// `Actor_ToggleEffectId(actor, unified_id, add)` __thiscall.
//
// 🔴 C'EST LA BONNE PORTE, ET IL Y EN A UNE MAUVAISE. `add = 1` insère l'id
// dans l'ENSEMBLE porté par l'acteur (`actor+0x3fc`) puis RÉ-APPLIQUE tout
// l'ensemble — les effets ÉQUIPÉS du joueur sont donc préservés, ou restaurés.
// `add = 0` retire l'id de l'ensemble et n'enlève que celui-là.
//
// ⚠ NE PAS employer `Effect_ApplyEffectIdToActor` (0x00c41ba0) directement :
// elle casse l'état équipé, et il faut alors un `@refresh` pour s'en remettre.
//
// L'id unifié d'un effet de couvre-chef vaut `ordinal + kHatEffectIdBase`.
constexpr uintptr_t kActorToggleEffectIdAddr = 0x00c44940;
constexpr int       kHatEffectIdBase         = 0x98a;

}  // namespace rag
