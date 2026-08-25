#pragma once

// ── Les pièces PORTÉES, lues dans la session ─────────────────────────────────
//
// Le client range l'équipement du joueur dans deux tableaux de dix `ItemSkillInfo`
// (248 octets chacun) directement dans la session : l'équipement à
// `session+0x17D0`, le costume à `session+0x2B30`. Le tableau `session+0x2180`
// (et son costume `session+0x34E0`) est le MÊME layout, mais pour le joueur
// qu'on INSPECTE — cf. docs/view_equip_re.md §6.
//
// ── Pourquoi cet en-tête ─────────────────────────────────────────────────────
// Ces onze offsets vivaient dans un namespace anonyme de `character_sheet.cc`,
// donc invisibles au grep depuis ailleurs. La deuxième vue qui a eu besoin de
// « ce que je porte à cet emplacement » — la comparaison de la fiche
// « Voir l'équipement » — n'avait le choix qu'entre les recopier et les
// partager. C'est le raisonnement de `ragnarok/uiwnd.h`, appliqué une fois de
// plus : au prochain portage de client, il y a UN endroit à corriger.
//
// ✅ DETTE SOLDÉE (2026-08-25). Cet en-tête annonçait que `character_sheet.cc`
// gardait ses propres copies et que leur migration serait « une passe à part ».
// Elle est faite : seize constantes retirées, soixante usages redirigés, les
// valeurs relues une à une contre celles d'ici AVANT toute écriture.
//
// ⚠ UNE SEULE N'A PAS MIGRÉ, et c'est un choix : `kOffEquipAmount` porte le même
// offset que `kOffPresent` (0x10) mais un SENS différent — la quantité d'un item
// d'INVENTAIRE, qui coïncide avec le drapeau de présence pour une pièce portée.
// Le renommer effacerait la nuance que son commentaire porte.
//
// ⚠ Rien ici n'appelle le natif : ce sont des LECTURES de mémoire, sous SEH. Un
// tableau à moitié initialisé (entre deux paquets, pendant un changement de map)
// doit rendre « pas de pièce », jamais faire tomber le client.

#include <cstdint>
#include <cstdlib>  // atoi
#include <excpt.h>  // __try / __except

#include "ragnarok/globals.h"  // rag::kSessionAddr
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "ragnarok/item_info.h"  // rag::itemlist::kInfoSize

namespace rag {
namespace equip {

// Les deux tableaux du JOUEUR, dix emplacements chacun.
constexpr int kOwnEquipBase   = 0x17d0;
constexpr int kOwnCostumeBase = 0x2b30;
// Ceux du joueur INSPECTÉ (remplis par ZC_EQUIPWIN_MICROSCOPE ; le natif s'en
// sert pour sa fenêtre 139). Mêmes entrées, même layout.
constexpr int kOtherEquipBase   = 0x2180;
constexpr int kOtherCostumeBase = 0x34e0;

constexpr int kSlotStride = rag::itemlist::kInfoSize;  // une entrée = un ItemSkillInfo
constexpr int kSlotCount  = 10;

// Champs d'une entrée. L'index d'emplacement est `log2` du bit `EQP_*` :
// 0 tête (bas), 1 arme, 2 cape, 3 acc. gauche, 4 armure, 5 bouclier,
// 6 chaussures, 7 acc. droit, 8 tête (haut), 9 tête (milieu).
constexpr int kOffType     = 0x00;
constexpr int kOffInvIndex = 0x04;
constexpr int kOffLocation = 0x08;  // masque EQP_* où la pièce PEUT aller
constexpr int kOffWear     = 0x0c;  // != 0 = portée
constexpr int kOffPresent  = 0x10;  // == 1 = l'emplacement est occupé
constexpr int kOffCards    = 0x1c;  // 4 × uint32 (cartes, ou identité du forgeron)
constexpr int kOffResname  = 0x2c;  // std::string (SSO) : l'id d'objet EN TEXTE
constexpr int kOffDamaged  = 0x5d;  // octet : équipement CASSÉ
constexpr int kOffRefine   = 0x60;
constexpr int kOffView     = 0x70;  // look (sprite d'arme / de coiffe)
constexpr int kOffGrade    = 0x88;  // int16 : grade d'enchantement
constexpr int kOffOptCount = 0x98;  // int : nombre d'options aléatoires (0..5)
constexpr int kOffOpts     = 0x9c;  // entrées de 5 o : {int16 index, int16 value, uint8 param}

// Ce qu'une vue a besoin de savoir d'une pièce portée. POD volontairement : elle
// est remplie sous `__try`, où aucun objet à destructeur n'a le droit d'exister
// (erreur C2712).
struct WornPiece {
  uint32_t nameid = 0;
  int      inv_index = 0;
  uint32_t location = 0;
  int      refine = 0;
  int      grade = 0;
  uint16_t view = 0;
  bool     damaged = false;
  uint32_t cards[4] = {0, 0, 0, 0};
  int      opt_count = 0;
  int16_t  opt_index[5] = {0, 0, 0, 0, 0};
  int16_t  opt_value[5] = {0, 0, 0, 0, 0};
  uint8_t  opt_param[5] = {0, 0, 0, 0, 0};
};

// La pièce que JE porte à cet emplacement, ou false s'il est vide.
//
// ⚠ `base` désigne le tableau : `kOwnEquipBase` / `kOwnCostumeBase`. Les deux
// bases « autre joueur » existent aussi, mais une vue qui décode elle-même le
// paquet n'en a pas besoin — et elles ne portent ni le grade ni le détail des
// options (cf. docs/view_equip_re.md §9.2).
inline bool ReadWorn(int slot, int base, WornPiece* out) {
  if (!out || slot < 0 || slot >= kSlotCount) return false;
  __try {
    const uint8_t* e = reinterpret_cast<const uint8_t*>(
        kSessionAddr + base + slot * kSlotStride);
    // 🔴 LES DEUX tests : un emplacement libéré garde ses octets, seul
    // `kOffPresent` dit qu'il est occupé — et un index nul signale une entrée
    // jamais remplie. C'est la garde que `character_sheet` applique déjà.
    if (*reinterpret_cast<const int*>(e + kOffInvIndex) == 0 ||
        *reinterpret_cast<const int*>(e + kOffPresent) != 1)
      return false;

    out->inv_index = *reinterpret_cast<const int*>(e + kOffInvIndex);
    out->location  = *reinterpret_cast<const uint32_t*>(e + kOffLocation);
    out->refine    = *reinterpret_cast<const int*>(e + kOffRefine);
    out->grade     = *reinterpret_cast<const short*>(e + kOffGrade);
    out->view      = *reinterpret_cast<const uint16_t*>(e + kOffView);
    out->damaged   = *(e + kOffDamaged) != 0;
    for (int c = 0; c < 4; ++c)
      out->cards[c] = *reinterpret_cast<const uint32_t*>(e + kOffCards + c * 4);

    int n = *reinterpret_cast<const int*>(e + kOffOptCount);
    if (n < 0) n = 0;
    if (n > 5) n = 5;
    out->opt_count = n;
    for (int o = 0; o < n; ++o) {
      const uint8_t* opt = e + kOffOpts + o * 5;
      out->opt_index[o] = *reinterpret_cast<const int16_t*>(opt + 0);
      out->opt_value[o] = *reinterpret_cast<const int16_t*>(opt + 2);
      out->opt_param[o] = *(opt + 4);
    }

    // L'identifiant d'objet est rangé en TEXTE dans une std::string à SSO : au
    // delà de 15 caractères de capacité, le tampon est un pointeur.
    const char* rn = rag::clientstr::Data(e + kOffResname);
    out->nameid = (rn && rn[0]) ? static_cast<uint32_t>(std::atoi(rn)) : 0;
    return out->nameid != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ── Les masques EQP_* : OÙ une pièce peut aller ──────────────────────────────
//
// C'est le champ `kOffLocation` d'une entrée, et le `location` d'un objet
// d'inventaire. Un masque, pas un index : une arme à deux mains occupe les deux
// mains, un accessoire peut aller à gauche OU à droite.
//
// 🔴 Le jeu COMPLET vivait dans `view_equip_window.cc` et un sous-ensemble de
// quatre dans `character_sheet.cc` — dont DEUX sous d'autres noms : `kEqpHandR`
// pour l'arme et `kEqpHandL` pour le bouclier. Même valeur, deux vocabulaires :
// celui de rAthena (EQP_HAND_R / EQP_HAND_L) et celui de l'écran du client. Les
// deux sont justes, ce qui est précisément le problème — aucune recherche par
// nom ne trouvait les deux jeux.
//
// Nommage retenu : ce que la pièce EST. L'équivalent rAthena est en regard, pour
// que le rapprochement avec un script serveur reste immédiat.
constexpr uint32_t kEqpHeadLow  = 0x0001;  // EQP_HEAD_LOW
constexpr uint32_t kEqpWeapon   = 0x0002;  // EQP_HAND_R  — main droite
constexpr uint32_t kEqpGarment  = 0x0004;  // EQP_GARMENT
constexpr uint32_t kEqpAccL     = 0x0008;  // EQP_ACC_L
constexpr uint32_t kEqpArmor    = 0x0010;  // EQP_ARMOR
constexpr uint32_t kEqpShield   = 0x0020;  // EQP_HAND_L  — main gauche
constexpr uint32_t kEqpShoes    = 0x0040;  // EQP_SHOES
constexpr uint32_t kEqpAccR     = 0x0080;  // EQP_ACC_R
constexpr uint32_t kEqpHeadTop  = 0x0100;  // EQP_HEAD_TOP
constexpr uint32_t kEqpHeadMid  = 0x0200;  // EQP_HEAD_MID

// ⚠ L'INDEX D'UNE CASE DE GRILLE EST `log2` DU BIT. Les COSTUMES portent leurs
// propres bits et sont remappés sur les MÊMES index — c'est ce que fait
// `EquipLocation_DecodeToSlots` (0x00D55850), et c'est pour ça qu'ils vivent
// dans un tableau séparé côté natif : sans quoi ils écraseraient l'équipement.
constexpr uint32_t kEqpCostumeHeadTop = 0x0400;
constexpr uint32_t kEqpCostumeHeadMid = 0x0800;
constexpr uint32_t kEqpCostumeHeadLow = 0x1000;
constexpr uint32_t kEqpCostumeGarment = 0x2000;

// 🔴 Ce masque RANGE un objet hors de l'équipement ordinaire, et c'est ce qui
// permet de résoudre une pièce de preset dans la bonne famille : sans lui, un
// chapeau et son costume homonyme se confondent, et le preset équipe l'un pour
// l'autre.
constexpr uint32_t kEqpCostumeMask = 0x3C00;  // les quatre ci-dessus

constexpr uint32_t kEqpAmmo = 0x8000;  // EQP_AMMO — la munition

// Les six emplacements d'OMBRE : 0x4000, puis 0x10000 à 0x200000.
//
// Ils n'existent pas sur Moonlight (aucun objet de `db/import/` n'en porte un),
// mais ils sont dans le paquet dès que le serveur en distribue — et 🔴 LE CLIENT
// LES FAIT TOMBER SUR LES INDEX DES COSTUMES, où ils écraseraient un vrai
// costume EN SILENCE. D'où ce masque, qui sert à les ranger à part.
constexpr uint32_t kEqpShadowMask = 0x3F4000;

// ⚠ Le DUAL-WIELD se lit sur ces masques et non sur un type d'objet : une pièce
// qui porte `kEqpWeapon` ET `kEqpShield` est une arme à deux mains, deux pièces
// distinctes portant l'un et l'autre sont un vrai dual-wield.

}  // namespace equip
}  // namespace rag
