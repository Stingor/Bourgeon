#pragma once

// ── L'EMPLACEMENT PRINCIPAL d'un masque `equip` ──────────────────────────────
//
// Le serveur décrit où une pièce (ou la carte qui s'y sertit) se pose par un
// masque de bits EQP_* ; l'interface, elle, veut UN mot : « Armor », « Head
// top ». La traduction masque → mot était écrite QUATRE fois — l'entrepôt
// (`PrimaryEquipSlot`), le cash shop (`SlotOf`), la feuille de perso
// (`kRefinePos`), l'album de cartes — avec les mêmes bits et le même ordre de
// priorité, chacune sous la forme que son appelant préférait. Quatre tables à
// garder alignées à la main : un emplacement ajouté (costume, munition) qu'une
// seule oublie y apparaît dans « Tous » et dans aucun onglet.
//
// Ici, la vérité sur les BITS et sur la PRIORITÉ. Les libellés sont les termes
// du jeu, en anglais, comme partout — seuls les trois « Head … » se traduisent,
// et c'est l'appelant qui décide s'il les passe à i18n::Tr.
//
// ⚠ L'entrepôt, le cash shop et la feuille de perso portent encore leur copie :
// leurs formes de retour (un index d'onglet, une clé de filtre, une paire
// masque/libellé) ne se réduisent pas en un geste sans réordonner leurs
// combos. Ils sont à migrer ici un par un, en vérifiant chaque fois que l'ordre
// des entrées d'un combo ne change pas sous les doigts du joueur.

#include <cstdint>

namespace equipslot {

// Les bits EQP_* du serveur (rAthena, src/map/pc.hpp), tels qu'ils voyagent dans
// les paquets d'objets.
constexpr uint32_t kHeadLow    = 0x000001;
constexpr uint32_t kHandR      = 0x000002;  // arme
constexpr uint32_t kGarment    = 0x000004;
constexpr uint32_t kAccL       = 0x000008;
constexpr uint32_t kArmor      = 0x000010;
constexpr uint32_t kHandL      = 0x000020;  // bouclier
constexpr uint32_t kShoes      = 0x000040;
constexpr uint32_t kAccR       = 0x000080;
constexpr uint32_t kHeadTop    = 0x000100;
constexpr uint32_t kHeadMid    = 0x000200;
constexpr uint32_t kCostumeAny = 0x003C00;  // COSTUME_HEAD_TOP/MID/LOW + GARMENT
constexpr uint32_t kAmmo       = 0x008000;
constexpr uint32_t kShadowAny  = 0x3F0000;

constexpr uint32_t kAccessory  = kAccL | kAccR;
constexpr uint32_t kHeadAny    = kHeadTop | kHeadMid | kHeadLow;

// Le libellé de l'emplacement PRINCIPAL. Une pièce multi-emplacements est
// nommée par le plus significatif — la tête avant le corps, le corps avant les
// mains — et non par le premier bit rencontré. Chaîne vide si aucun bit connu.
inline const char* PrimaryLabel(uint32_t equip) {
  if (equip & kHeadTop)    return "Head top";
  if (equip & kHeadMid)    return "Head mid";
  if (equip & kHeadLow)    return "Head bot";
  if (equip & kArmor)      return "Armor";
  if (equip & kGarment)    return "Garment";
  if (equip & kShoes)      return "Shoes";
  if (equip & kAccessory)  return "Accessory";
  if (equip & kHandL)      return "Shield";
  if (equip & kHandR)      return "Weapon";
  if (equip & kAmmo)       return "Ammunition";
  if (equip & kCostumeAny) return "Costume";
  if (equip & kShadowAny)  return "Shadow";
  return "";
}

}  // namespace equipslot
