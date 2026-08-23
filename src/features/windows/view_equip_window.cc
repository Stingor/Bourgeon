#include "features/windows/view_equip_window.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "bourgeon.h"
#include "features/fx/palette_base.h"    // fusion .spr + .pal, détection des rampes
#include "features/fx/palette_cache.h"   // DollKey
#include "features/fx/palette_inject.h"  // ActorBodySpritePath
#include "features/fx/style_sync.h"      // la recette d'un joueur en vue
#include "features/item_cell.h"
#include "features/link_gesture.h"       // gestes et menu standard d'un objet
#include "features/staff_gate.h"         // IsStaff : le seuil 80, celui du serveur
#include "features/windows/chat_window.h"  // QueueCommand (@cloneequip / @clonestat)
#include "imgui.h"
#include "ragnarok/equip_slots.h"        // MES pièces portées, par emplacement
#include "ui/doll.h"
#include "ui/icon_cache.h"
#include "ui/palette_ramps.h"
#include "ui/ro_imgui.h"
#include "ui/sprite_path.h"
#include "utils/i18n.h"
#include "utils/log_console.h"

namespace {

// ── Le paquet ────────────────────────────────────────────────────────────────
// `ZC_EQUIPWIN_MICROSCOPE` : SIX opcodes selon le PACKETVER, un seul atteint
// (docs/view_equip_re.md §10). On ne revendique que celui-là — revendiquer les
// autres masquerait un jour un vrai changement de version derrière un silence.
constexpr uint16_t kOpViewEquipAck = 0x0B37;  // ZC, la réponse
constexpr uint16_t kOpViewEquipReq = 0x02D6;  // CZ, la demande [op:2][aid:4]

// 🔴 Offsets comptés depuis `data`, qui commence APRÈS l'opcode (régime
// RegisterReplaceOpcode, cf. plugin.h) : chacun vaut donc l'offset du paquet
// MOINS 2. La table de la doc est en offsets de paquet — ne pas les mélanger.
constexpr int kOffLength       = 0;   // paquet +2
constexpr int kOffName         = 2;   // paquet +4,  24 octets
constexpr int kOffJob          = 26;
constexpr int kOffHair         = 28;
constexpr int kOffHeadLow      = 30;
constexpr int kOffHeadMid      = 32;
constexpr int kOffHeadTop      = 34;
constexpr int kOffRobe         = 36;
constexpr int kOffHairColor    = 38;
constexpr int kOffClothesColor = 40;
constexpr int kOffBody2        = 42;
constexpr int kOffSex          = 44;
constexpr int kHeaderSize      = 45;  // paquet 47
constexpr int kEntrySize       = 68;  // 0x44

// Champs d'une pièce, comptés depuis le début de son entrée.
constexpr int kItemId        = 2;
constexpr int kItemLocation  = 7;
constexpr int kItemWearState = 11;
constexpr int kItemCards     = 15;  // 4 x uint32
constexpr int kItemLook      = 37;
constexpr int kItemOptCount  = 39;
constexpr int kItemOptions   = 40;  // 5 x {id:2, valeur:2, param:1}
constexpr int kItemRefine    = 65;
constexpr int kItemGrade     = 66;
constexpr int kItemFlags     = 67;

constexpr uint8_t kFlagIdentified = 0x01;
constexpr uint8_t kFlagDamaged    = 0x02;

// ── Emplacements ─────────────────────────────────────────────────────────────
// L'index de la grille est `log2` du bit `EQP_*` (docs §6.1). Les COSTUMES
// portent leurs propres bits et sont remappés sur les mêmes index — c'est ce que
// fait `EquipLocation_DecodeToSlots` (0x00D55850), et c'est pour ça qu'ils vivent
// dans un tableau séparé côté natif : sans quoi ils écraseraient l'équipement.
constexpr uint32_t kEqpHeadLow  = 0x0001;
constexpr uint32_t kEqpWeapon   = 0x0002;
constexpr uint32_t kEqpGarment  = 0x0004;
constexpr uint32_t kEqpAccL     = 0x0008;
constexpr uint32_t kEqpArmor    = 0x0010;
constexpr uint32_t kEqpShield   = 0x0020;
constexpr uint32_t kEqpShoes    = 0x0040;
constexpr uint32_t kEqpAccR     = 0x0080;
constexpr uint32_t kEqpHeadTop  = 0x0100;
constexpr uint32_t kEqpHeadMid  = 0x0200;

constexpr uint32_t kEqpCostumeHeadTop = 0x0400;
constexpr uint32_t kEqpCostumeHeadMid = 0x0800;
constexpr uint32_t kEqpCostumeHeadLow = 0x1000;
constexpr uint32_t kEqpCostumeGarment = 0x2000;
constexpr uint32_t kEqpAmmo           = 0x8000;

// Les six emplacements d'OMBRE. Ils n'existent pas sur moonlight (aucun objet de
// `db/import/` n'en porte un), mais ils sont dans le paquet dès que le serveur en
// distribue — et 🔴 le client les fait tomber sur les index des COSTUMES, où ils
// écraseraient un vrai costume en silence (docs §6.2). On les range donc à part.
constexpr uint32_t kEqpShadowMask = 0x3F4000;  // 0x4000 | 0x10000..0x200000

// Index de grille, ou -1. `costume`/`shadow`/`ammo` disent dans quelle SECTION
// la pièce va — deux pièces peuvent partager un index sans se marcher dessus.
int SlotFromWearState(uint32_t wear, bool* costume, bool* shadow, bool* ammo,
                      bool* two_handed) {
  *costume    = false;
  *shadow     = false;
  *ammo       = false;
  *two_handed = false;

  if (wear & kEqpAmmo) { *ammo = true; return -1; }

  if (wear & kEqpShadowMask) {
    *shadow = true;
    if (wear & 0x010000) return 4;  // armure d'ombre
    if (wear & 0x024000) return 1;  // arme d'ombre
    if (wear & 0x040000) return 5;  // bouclier d'ombre
    if (wear & 0x080000) return 6;  // chaussures d'ombre
    if (wear & 0x100000) return 3;  // accessoire d'ombre gauche
    if (wear & 0x200000) return 7;  // accessoire d'ombre droit
    return -1;
  }

  if (wear & (kEqpCostumeHeadTop | kEqpCostumeHeadMid | kEqpCostumeHeadLow |
              kEqpCostumeGarment)) {
    *costume = true;
    if (wear & kEqpCostumeHeadTop) return 8;
    if (wear & kEqpCostumeHeadMid) return 9;
    if (wear & kEqpCostumeHeadLow) return 0;
    return 2;  // cape de costume
  }

  // Une arme à deux mains porte arme ET bouclier : elle s'affiche sur la ligne
  // « arme », et la ligne « bouclier » dit pourquoi elle est vide.
  *two_handed = (wear & kEqpWeapon) && (wear & kEqpShield);

  if (wear & kEqpHeadTop)  return 8;
  if (wear & kEqpHeadMid)  return 9;
  if (wear & kEqpHeadLow)  return 0;
  if (wear & kEqpArmor)    return 4;
  if (wear & kEqpWeapon)   return 1;
  if (wear & kEqpShield)   return 5;
  if (wear & kEqpGarment)  return 2;
  if (wear & kEqpShoes)    return 6;
  if (wear & kEqpAccL)     return 3;
  if (wear & kEqpAccR)     return 7;
  return -1;
}

const char* SlotLabel(int slot) {
  switch (slot) {
    case 0: return i18n::Tr("Tête (bas)");
    case 1: return i18n::Tr("Arme");
    case 2: return i18n::Tr("Cape");
    case 3: return i18n::Tr("Accessoire gauche");
    case 4: return i18n::Tr("Armure");
    case 5: return i18n::Tr("Bouclier");
    case 6: return i18n::Tr("Chaussures");
    case 7: return i18n::Tr("Accessoire droit");
    case 8: return i18n::Tr("Tête (haut)");
    case 9: return i18n::Tr("Tête (milieu)");
    default: return "?";
  }
}

// Taille FIXE de la fenêtre : celle qu'elle avait par défaut, où les dix
// emplacements d'équipement tiennent sans défilement (vérifié à l'écran). Les
// costumes, plus rares, font défiler la liste dans son cadre.
const ImVec2 kWindowSize(620.0f, 380.0f);
// Avec la colonne « la mienne ». DEUX tailles fixes plutôt qu'un
// redimensionnement : la fenêtre reste juste assez grande pour ce qu'elle
// montre, et le joueur n'a rien à ajuster à la main en cochant la case.
// L'identifiant du menu contextuel d'objet. UN seul pour toute la fenêtre : le
// menu ne montre que la cible mise de côté au clic droit, il n'a donc aucune
// raison d'exister en autant d'exemplaires qu'il y a de cellules.
const char* const kItemMenuId = "##viewequip_item_menu";

const ImVec2 kWindowSizeCompare(900.0f, 380.0f);

// L'ordre de lecture : de la tête aux pieds, comme on regarde quelqu'un.
constexpr int kEquipOrder[]   = {8, 9, 0, 4, 1, 5, 2, 6, 3, 7};
constexpr int kCostumeOrder[] = {8, 9, 0, 2};

uint16_t ReadU16(const uint8_t* p) {
  uint16_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
uint32_t ReadU32(const uint8_t* p) {
  uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

// ── Les couleurs composées par le joueur inspecté ────────────────────────────
//
// Un joueur qui s'est recoloré (éditeur de style, CZ 0x0F26) est vu par tous
// dans SES couleurs : le serveur diffuse sa RECETTE, et chaque client recalcule
// la palette. Un pantin qui l'ignorerait montrerait l'apparence native d'un
// personnage que le joueur a justement sous les yeux, autrement coloré — le
// pantin et la personne ne seraient pas la même.
//
// 🔴 Le CHEMIN DU SPRITE vient de l'ACTEUR quand il est là, pas d'une déduction.
// La cible est forcément sur notre carte (garde serveur), donc son acteur porte
// le corps que le client a RÉELLEMENT résolu — ce que la déduction rate sur les
// 3e et 4e classes, les montures et les costumes de corps. Et depuis la v7, ce
// chemin désigne AUSSI la variante de recette : se tromper de corps, c'est
// appliquer les couleurs d'un autre.
//
// 🔴 Le résultat est CACHÉ. Le construire coûte l'analyse complète d'un `.spr` ;
// ce panneau, lui, se redessine à chaque frame.
struct DollPalette {
  const uint8_t* rgba = nullptr;  // 1024 o RGBA, nullptr = pas de recette
  const char*    key  = nullptr;  // clé de cache de teinte du composeur
  const char*    spr  = nullptr;  // corps résolu, nullptr = laisser déduire
  int            hair = -1;       // couleur de cheveux imposée, -1 = celle du perso
};

uint64_t HashBytes(const void* data, size_t size, uint64_t seed) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint64_t h = seed;
  for (size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;  // FNV-1a
  }
  return h;
}

DollPalette ResolveDollPalette(uint32_t gid, int job, int body, int sex,
                               int clothes_color) {
  DollPalette out;
  if (gid == 0) return out;  // fiche non demandée par nous : pas d'acteur à lire

  // Le corps RÉEL de l'acteur, sinon la déduction (l'acteur peut être hors de
  // portée d'affichage : même carte ne veut pas dire à l'écran).
  char spr[512] = {0};
  if (!fx::palette_inject::ActorBodySpritePath(gid, spr, sizeof(spr)) &&
      !ro::BodySpritePath(job, body, sex, spr, sizeof(spr)))
    return out;

  const uint32_t body_key = ro::BodySpriteKey(spr);
  ro::PaletteRecipe recipe;
  const bool has_recipe = fx::style_sync::RemoteRecipe(gid, body_key, &recipe);

  struct Cached {
    uint64_t sig = ~0ull;
    std::vector<uint8_t> rgba;
    std::string key;
    std::string spr;
    int hair = -1;
  };
  static std::map<uint32_t, Cached> cache;
  // Une session peut inspecter beaucoup de monde ; chaque entrée retient 1 Ko de
  // palette. On repart de zéro plutôt que d'accumuler — la fiche affichée sera
  // recalculée à la frame suivante, et c'est tout ce qu'on perd.
  if (cache.size() > 64) cache.clear();

  // La RECETTE elle-même entre dans la signature : un joueur qui retouche ses
  // couleurs pendant qu'on le regarde doit changer sous nos yeux, comme son
  // personnage à l'écran.
  //
  // 🔴 CHAMP PAR CHAMP, jamais `sizeof(recipe)` d'un bloc : la structure porte du
  // BOURRAGE (`RampAdjust` = int16+int8+int8+uint8 = 5 utiles, 6 alloués) dont le
  // contenu est indéterminé. Hasher les octets bruts ferait varier la signature
  // sans que rien n'ait changé — donc rebâtir la palette À CHAQUE FRAME, c'est-à-
  // dire ré-analyser un `.spr` entier soixante fois par seconde. C'est le même
  // piège que celui qui interdit d'envoyer la struct telle quelle sur le réseau
  // (cf. features/fx/style_sync.h).
  uint64_t sig = HashBytes(spr, std::strlen(spr), 1469598103934665603ull);
  sig = HashBytes(&job, sizeof(job), sig);
  sig = HashBytes(&sex, sizeof(sex), sig);
  sig = HashBytes(&clothes_color, sizeof(clothes_color), sig);
  sig = HashBytes(&has_recipe, sizeof(has_recipe), sig);
  if (has_recipe) {
    sig = HashBytes(&recipe.palette_id, sizeof(recipe.palette_id), sig);
    sig = HashBytes(&recipe.hair_palette_id, sizeof(recipe.hair_palette_id), sig);
    sig = HashBytes(&recipe.hair_style, sizeof(recipe.hair_style), sig);
    for (const ro::RampAdjust& r : recipe.ramps) {
      sig = HashBytes(&r.hue, sizeof(r.hue), sig);
      sig = HashBytes(&r.sat, sizeof(r.sat), sig);
      sig = HashBytes(&r.val, sizeof(r.val), sig);
      sig = HashBytes(&r.absolute, sizeof(r.absolute), sig);
    }
  }

  Cached& c = cache[gid];
  if (c.sig != sig) {
    c.sig = sig;
    c.rgba.clear();
    c.key.clear();
    c.hair = -1;
    c.spr  = spr;

    if (has_recipe) {
      // La couleur de CHEVEUX ne demande aucun calcul : le composeur sait teindre
      // une tête à partir d'un numéro de palette officielle.
      c.hair = recipe.hair_palette_id > 0 ? recipe.hair_palette_id : -1;

      // 🔴 La teinte de BASE de la recette prime sur la couleur de vêtement du
      // personnage : c'est elle qui a servi de base au joueur, et fusionner une
      // autre palette ferait détecter d'autres rampes — donc des réglages posés
      // à côté.
      char pal[160] = {0};
      const int couleur =
          recipe.palette_id > 0 ? recipe.palette_id : clothes_color;
      if (couleur >= 0) ro::BodyPalettePath(couleur, job, pal, sizeof(pal));

      fx::palette_base::Body base;
      if (fx::palette_base::BuildFromPaths(spr, pal[0] ? pal : nullptr, &base) ==
          fx::palette_base::kOk) {
        c.rgba.assign(1024, 0);
        if (ro::ApplyRecipe(base.base.data(), base.base.size(), base.ramps,
                            base.ramp_count, recipe, c.rgba.data(),
                            c.rgba.size()))
          c.key = fx::palette_cache::DollKey(gid, c.rgba.data());
        else
          c.rgba.clear();
      }
    }
  }

  // Le chemin de corps est rendu MÊME sans recette : c'est lui qui corrige la
  // déduction sur les 3e et 4e classes, recolorées ou non.
  out.spr  = c.spr.empty() ? nullptr : c.spr.c_str();
  out.hair = c.hair;
  if (!c.rgba.empty() && !c.key.empty()) {
    out.rgba = c.rgba.data();
    out.key  = c.key.c_str();
  }
  return out;
}

}  // namespace

ViewEquipWindow::ViewEquipWindow() {
  // 🔴 Le prédicat DÉCIDE, à chaque paquet, sur le fil réseau. Faux -> le
  // handler natif se déroule et la fenêtre 139 s'ouvre comme avant ; vrai -> il
  // est sauté et nous recevons les octets, donc la native ne naît JAMAIS. C'est
  // ce qui évite d'avoir à la détruire au tick : elle n'a pas d'autre créateur.
  //
  // Rien à rejouer de ce que le handler faisait : ses écritures ne servent qu'à
  // la fenêtre qu'on remplace (docs/view_equip_re.md §9.1).
  Bourgeon::Instance().RegisterReplaceOpcode(
      kOpViewEquipAck, [this]() { return imgui_enabled_; });
}

void ViewEquipWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                   uint16_t len) {
  // Fil RÉSEAU : on copie, rien de plus (cf. features/net_inbox.h).
  //
  // 🔴 `PushAnnounced`, PAS `Push` : ce paquet est à longueur VARIABLE. Le
  // dispatcher ne transmet que ses premiers octets — le corps, lui, vit complet
  // dans le tampon de réception du client. Copier `len` octets rendrait un
  // en-tête sans AUCUNE pièce, et rien ne le dirait : la fenêtre s'ouvrirait sur
  // un joueur en sous-vêtements.
  net_inbox_.PushAnnounced(opcode, data, len);
}

void ViewEquipWindow::OnModeSwitch(ModeMgr::ModeType, const char*) {
  // Changement de map ou retour au char-select : la cible n'est plus forcément
  // là, et le serveur exige la même map. Garder l'affichage donnerait une fiche
  // périmée sans rien qui le dise.
  open_ = false;
  target_ = Target{};
  pending_aid_ = 0;
  refresh_requested_ = false;
}

void ViewEquipWindow::NotePendingTarget(uint32_t aid) { pending_aid_ = aid; }

void ViewEquipWindow::OnTick() {
  // Le groupe « Interface moderne » vient d'être coupé alors que la fiche était
  // ouverte : on FERME, on ne rouvre pas l'équivalent natif. C'est la règle du
  // projet sur toute bascule — rouvrir signifierait fabriquer une session qu'on
  // n'a jamais observée. Et on jette les données : au rallumage, une fiche
  // rescapée d'un autre moment mentirait sans que rien ne le dise.
  if (!imgui_enabled_) {
    if (open_) {
      open_ = false;
      target_ = Target{};
    }
    refresh_requested_ = false;
    return;
  }

  if (!refresh_requested_) return;
  refresh_requested_ = false;
  if (target_.aid == 0) return;

  uint8_t packet[6];
  std::memcpy(packet + 0, &kOpViewEquipReq, sizeof(uint16_t));
  std::memcpy(packet + 2, &target_.aid, sizeof(uint32_t));
  // La demande repart à l'identique du bouton natif. Le serveur revérifie tout
  // (même map, autorisation) : si la cible est partie ou a fermé son équipement,
  // la réponse n'arrive pas — et c'est bien ainsi qu'il faut le dire au joueur,
  // pas en inventant un état local.
  pending_aid_ = target_.aid;
  Bourgeon::Instance().SendPacket(packet, sizeof(packet));
}

void ViewEquipWindow::HandlePacket(uint16_t opcode, const uint8_t* data,
                                   uint16_t len) {
  if (opcode != kOpViewEquipAck || data == nullptr) return;

  // 🔴 La longueur est ANNONCÉE par le serveur : elle borne la boucle, donc on
  // ne la croit pas sur parole. Un paquet tronqué ou une longueur absurde ne
  // doit pas faire lire au-delà du tampon.
  const int announced = static_cast<int>(ReadU16(data + kOffLength));
  const int usable    = (announced > 2 && announced - 2 <= static_cast<int>(len))
                            ? announced - 2
                            : static_cast<int>(len);
  if (usable < kHeaderSize) {
    LogDiag("[ViewEquip] paquet trop court ({} octets)", usable);
    return;
  }

  Target next;
  next.aid = pending_aid_;  // 0 si la réponse n'a pas été demandée par nous

  // Le nom vient du FIL : `WireToUtf8`, jamais `LocalToUtf8` (qui décrit ce que
  // le CLIENT a chargé) — cf. ui/ro_imgui.h. Le champ n'est pas garanti terminé.
  char raw_name[25] = {0};
  std::memcpy(raw_name, data + kOffName, 24);
  next.name = ro::WireToUtf8(raw_name);

  next.job           = ReadU16(data + kOffJob);
  next.hair          = ReadU16(data + kOffHair);
  next.head_low      = ReadU16(data + kOffHeadLow);
  next.head_mid      = ReadU16(data + kOffHeadMid);
  next.head_top      = ReadU16(data + kOffHeadTop);
  next.robe          = ReadU16(data + kOffRobe);
  next.hair_color    = ReadU16(data + kOffHairColor);
  next.clothes_color = ReadU16(data + kOffClothesColor);
  next.body2         = ReadU16(data + kOffBody2);
  next.sex           = data[kOffSex];

  const int count = (usable - kHeaderSize) / kEntrySize;
  next.pieces.reserve(static_cast<size_t>(count));

  for (int i = 0; i < count; ++i) {
    const uint8_t* e = data + kHeaderSize + i * kEntrySize;

    Piece piece;
    piece.wear_state = ReadU32(e + kItemWearState);
    // `WearState == 0` = pièce non portée : le serveur ne devrait pas l'envoyer
    // (sa boucle ne prend que `it.equip`), et le natif la saute aussi.
    if (piece.wear_state == 0) continue;

    const uint8_t flags = e[kItemFlags];

    // Tout tient dans un ChatLink : c'est la structure que le projet emploie
    // déjà pour un objet qui n'est pas à nous, et c'est elle qui donne accès au
    // nom composé, à l'infobulle, à la description et au lien de chat.
    piece.link.id     = ReadU32(e + kItemId);
    piece.link.equip  = ReadU32(e + kItemLocation);
    piece.link.refine = e[kItemRefine];
    piece.link.view   = ReadU16(e + kItemLook);
    piece.link.grade  = e[kItemGrade];
    piece.link.broken = (flags & kFlagDamaged) != 0;
    // Le type décoré du lien : ce paquet ne transporte QUE de l'équipement (le
    // serveur filtre par `itemdb_isequip2`), donc la réponse est toujours oui.
    piece.link.equipable = true;
    for (int c = 0; c < 4; ++c)
      piece.link.cards[c] = ReadU32(e + kItemCards + 4 * c);

    const int opt_count = e[kItemOptCount] > 5 ? 5 : e[kItemOptCount];
    piece.link.opt_count = opt_count;
    for (int o = 0; o < opt_count; ++o) {
      const uint8_t* opt = e + kItemOptions + 5 * o;
      piece.link.opt_id[o]    = ReadU16(opt + 0);
      piece.link.opt_value[o] = ReadU16(opt + 2);
      piece.link.opt_param[o] = opt[4];
    }

    piece.slot = SlotFromWearState(piece.wear_state, &piece.costume,
                                   &piece.shadow, &piece.ammo,
                                   &piece.two_handed);

    // Le libellé complet — refine, préfixes de cartes, « <forgeron>'s », suffixe
    // « [N] » — est composé par le name-builder NATIF depuis un ItemSkillInfo
    // fabriqué à partir du lien. Rien à réimplémenter, et le rendu est
    // exactement celui du chat.
    char label[128] = {0};
    itemcell::BuildChatLinkName(piece.link, label, sizeof(label));
    piece.label = (label[0] != '\0') ? ro::WireToUtf8(label)
                                     : itemcell::NameById(piece.link.id);
    // Un objet NON identifié n'a pas à révéler son nom composé : le serveur
    // n'envoie de toute façon que de l'équipement porté (donc identifié), mais
    // le drapeau existe et le dire coûte une ligne.
    if ((flags & kFlagIdentified) == 0) piece.label = i18n::Tr("(non identifié)");

    next.pieces.push_back(std::move(piece));
  }

  // ⚠ On décode sur le fil principal mais HORS frame ImGui (le drain se fait
  // depuis le hook OnUpdate du mode) : l'horloge n'est lisible que si le contexte
  // existe déjà. Sans lui, l'âge affiché partira simplement de zéro.
  next.received_at =
      (ImGui::GetCurrentContext() != nullptr) ? ImGui::GetTime() : 0.0;
  target_ = std::move(next);
  pending_aid_ = 0;

  // Le natif OUVRE ici, et il a raison : trois des quatre refus du serveur sont
  // SILENCIEUX (cible absente, autre map). Une fenêtre ouverte « en attente »
  // resterait vide sans que rien vienne jamais la remplir (docs §2).
  open_ = true;
  need_focus_ = true;
}

const ViewEquipWindow::Piece* ViewEquipWindow::Find(int slot,
                                                    bool costume) const {
  for (const Piece& p : target_.pieces) {
    if (p.shadow || p.ammo) continue;
    if (p.costume == costume && p.slot == slot) return &p;
  }
  return nullptr;
}

void ViewEquipWindow::RequestRefresh() { refresh_requested_ = true; }

// ── Le pantin ────────────────────────────────────────────────────────────────
// Le composeur maison (`ro::DrawDoll`) prend exactement ce que l'en-tête du
// paquet porte. 🔴 On lui donne les coiffes de l'EN-TÊTE et non celles des objets
// listés : `vd.look[...]` est ce que la cible montre VRAIMENT à l'écran, costume
// compris. Le natif, lui, va les chercher dans les objets de l'onglet affiché —
// ce qui lui permet de montrer « sans les costumes » dans l'onglet Équipement,
// mais donne un pantin qui n'est pas la personne qu'on a en face.
//
// ⚠ Ni l'arme ni le bouclier n'y sont : le paquet ne porte pas leur look. On les
// laisse de côté DÉLIBÉRÉMENT (décision de l'utilisateur, 2026-08-22) — la
// fiche liste déjà les deux pièces, avec leurs cartes et leur raffinement, et
// c'est ce qu'on vient y lire. Les tirer de l'acteur serait faisable (la cible
// est sur notre carte) mais n'ajouterait rien qui ne soit déjà écrit à côté.
void ViewEquipWindow::DrawDollPanel(float width, float height) {
  ro::DollLook look;
  look.sex           = target_.sex;
  look.job           = target_.job;
  // `body` est la CLASSE qui nomme le sprite de corps, pas un style : laissée à
  // 0, tout le monde s'afficherait en Novice (cf. ui/doll.h). Un style de corps
  // équipé arrive sous son propre identifiant dans `body2`.
  look.body          = (target_.body2 > 0) ? target_.body2 : target_.job;
  look.hair          = target_.hair;
  look.hair_color    = target_.hair_color;
  look.clothes_color = target_.clothes_color;
  look.head_low      = target_.head_low;
  look.head_mid      = target_.head_mid;
  look.head_top      = target_.head_top;
  look.garment       = target_.robe;

  // Couleurs composées par le joueur inspecté : elles priment sur la palette de
  // vêtement officielle. Sans ça, un personnage recoloré s'afficherait ici dans
  // son apparence native alors qu'il est, à trois mètres, de la couleur qu'il a
  // choisie.
  const DollPalette pal = ResolveDollPalette(target_.aid, target_.job,
                                             look.body, target_.sex,
                                             target_.clothes_color);
  look.body_palette     = pal.rgba;
  look.body_palette_key = pal.key;
  look.body_spr_override = pal.spr;
  if (pal.hair > 0) look.hair_color = pal.hair;

  // Une fenêtre réduite au minimum peut ne rien laisser au pantin : sans ce
  // plancher, la hauteur passerait négative et le cadrage n'aurait plus de sens.
  const float doll_h = (height - 26.0f > 40.0f) ? height - 26.0f : 40.0f;

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ro::DollDrawOpts opts;
  opts.dir            = doll_dir_;
  opts.anim_seconds   = static_cast<float>(ImGui::GetTime());
  opts.center_on_body = true;
  // L'échelle est mesurée sur TOUTES les images et orientations : sans ça, le
  // pantin grandit et rétrécit quand on le fait tourner (une arme s'écarte de
  // face et se replie de dos). Coût assumé — c'est une vue unique.
  opts.fit_span = true;
  const bool drawn = ro::DrawDoll(ImGui::GetWindowDrawList(), look, origin.x,
                                  origin.y, width, doll_h, opts);
  ImGui::Dummy(ImVec2(width, doll_h));

  // Molette sur le pantin = rotation, le même geste que sur l'avatar de la fiche
  // de personnage. Les boutons restent pour qui n'a pas de molette.
  if (ImGui::IsItemHovered()) {
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) doll_dir_ = (doll_dir_ + (wheel > 0.0f ? 1 : 7)) & 7;
  }

  if (!drawn) {
    ImGui::TextDisabled("%s", i18n::Tr("(silhouette indisponible)"));
    return;
  }

  // Deux boutons plutôt qu'un glisser : le pantin partage sa zone avec la liste,
  // et un glisser y serait attrapé par la fenêtre.
  const float bw = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
  if (ro::RoButton("<###viewequip_left", bw, 0.0f))
    doll_dir_ = (doll_dir_ + 7) % 8;
  ImGui::SameLine();
  if (ro::RoButton(">###viewequip_right", bw, 0.0f))
    doll_dir_ = (doll_dir_ + 1) % 8;
}

// MA pièce à cet emplacement, ramenée à la MÊME forme que celle de la cible.
//
// 🔴 Le même `ChatLink` et le même compositeur de nom des deux côtés : deux
// colonnes composées autrement — l'une par le name-builder, l'autre à la main —
// se compareraient mal, et l'œil prendrait la différence de mise en forme pour
// une différence d'objet.
bool ViewEquipWindow::MyPiece(int slot, bool costume, itemcell::ChatLink* out,
                              std::string* label) const {
  rag::equip::WornPiece worn;
  const int base = costume ? rag::equip::kOwnCostumeBase
                           : rag::equip::kOwnEquipBase;
  if (!rag::equip::ReadWorn(slot, base, &worn)) return false;

  *out = itemcell::ChatLink{};
  out->id        = worn.nameid;
  out->equip     = worn.location;
  out->refine    = static_cast<uint32_t>(worn.refine);
  out->grade     = static_cast<uint32_t>(worn.grade);
  out->view      = worn.view;
  out->broken    = worn.damaged;
  out->equipable = true;
  for (int c = 0; c < 4; ++c) out->cards[c] = worn.cards[c];
  out->opt_count = worn.opt_count;
  for (int o = 0; o < worn.opt_count; ++o) {
    out->opt_id[o]    = static_cast<uint16_t>(worn.opt_index[o]);
    out->opt_value[o] = static_cast<uint16_t>(worn.opt_value[o]);
    out->opt_param[o] = worn.opt_param[o];
  }

  char buf[128] = {0};
  itemcell::BuildChatLinkName(*out, buf, sizeof(buf));
  *label = (buf[0] != '\0') ? ro::WireToUtf8(buf) : itemcell::NameById(out->id);
  return true;
}

// Une cellule « icône + nom ». Survol et menu contextuel valent pour les DEUX
// colonnes ; seul le CLIC (ouverture de la description) est réservé à la pièce
// de la cible — la mienne, je peux déjà l'ouvrir depuis mon inventaire.
void ViewEquipWindow::DrawItemCell(const itemcell::ChatLink& link,
                                   const std::string& label, int id) {
  ImGui::PushID(id);

  // 🔴 Un GROUPE, pour que l'icône ET le nom forment une seule zone sensible :
  // `IsItemHovered` ne parle que du DERNIER widget, et sans ça survoler l'icône
  // n'aurait rien fait — le joueur vise pourtant l'image en premier.
  ImGui::BeginGroup();
  ro::IconTex ic = ro::ItemIcon(link.id);
  if (ic.tex) {
    const ImVec4 tint =
        link.broken ? ImGui::ColorConvertU32ToFloat4(itemcell::kDamagedShadow)
                    : ImVec4(1, 1, 1, 1);
    ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(20, 20),
                       ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint);
    ImGui::SameLine();
  }
  itemcell::NameText(label.c_str(), link.broken);
  ImGui::EndGroup();

  // Les gestes du projet, pas les nôtres : clic = description, Maj+clic = lien
  // de chat, clic droit = menu. `links::` les factorise pour que toutes les
  // surfaces d'objet du client répondent pareil — une fenêtre qui inventerait
  // les siens obligerait le joueur à réapprendre.
  const links::Target target = links::FromItem(link, label.c_str());
  const bool hovered = ImGui::IsItemHovered();
  switch (links::Hit(target, hovered)) {
    case links::Gesture::kDescription: {
      // ARMÉE ici, jouée hors frame par itemcell::FlushDeferredDesc : ouvrir une
      // fenêtre native au milieu du rendu ImGui est proscrit.
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      itemcell::DeferDescFromChatLink(link, static_cast<int>(mouse.x),
                                      static_cast<int>(mouse.y));
      break;
    }
    case links::Gesture::kChatLink:
      links::PostToChat(target);
      break;
    case links::Gesture::kMenu:
      menu_target_ = target;
      menu_open_   = true;  // ouvert plus bas, hors de cette pile d'ids
      break;
    case links::Gesture::kNone:
      break;
  }

  if (hovered) {
    hover_target_ = target;
    hover_valid_  = true;
  }

  ImGui::PopID();
}

void ViewEquipWindow::DrawRow(const Piece& piece, int slot,
                              bool costume_section) {
  ImGui::TableNextRow();

  ImGui::TableNextColumn();
  ImGui::TextDisabled("%s", SlotLabel(slot));

  ImGui::TableNextColumn();
  ImGui::PushID(slot * 2 + (costume_section ? 1 : 0));
  DrawItemCell(piece.link, piece.label, 0);

  if (compare_) {
    ImGui::TableNextColumn();
    itemcell::ChatLink mine;
    std::string mine_label;
    if (MyPiece(slot, costume_section, &mine, &mine_label)) {
      // Même objet de BASE des deux côtés : on le teinte, plutôt que de laisser
      // l'oeil comparer deux lignes qui se ressemblent. Ce n'est PAS « la même
      // pièce » — raffinement, cartes et options peuvent differer, et le survol
      // les montre.
      const bool meme_objet = (mine.id == piece.link.id);
      if (meme_objet) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(70, 120, 70, 255));
      DrawItemCell(mine, mine_label, 1);
      if (meme_objet) ImGui::PopStyleColor();
    } else {
      ImGui::TextDisabled("%s", i18n::Tr("—"));
    }
  }

  ImGui::PopID();
}

void ViewEquipWindow::DrawPieceList() {
  constexpr ImGuiTableFlags kFlags =
      ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;

  const int columns = compare_ ? 3 : 2;

  auto section = [&](const char* title, const int* order, int order_count,
                     bool costume) {
    if (!ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::PushID(costume ? "costume" : "equip");
    if (ImGui::BeginTable("###viewequip_tbl", columns, kFlags)) {
      ImGui::TableSetupColumn("###slot", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::CalcTextSize(i18n::Tr("Accessoire gauche")).x);
      ImGui::TableSetupColumn("###item", ImGuiTableColumnFlags_WidthStretch);
      // ⛔ PAS d'en-tête « la sienne / la mienne » (retiré sur demande,
      // 2026-08-23) : c'était une LIGNE de table de plus, qui décalait tout le
      // tableau sans rien apprendre — la colonne de gauche porte déjà le nom de
      // l'emplacement, et laquelle des deux colonnes est la sienne se voit.
      if (compare_)
        ImGui::TableSetupColumn("###mine", ImGuiTableColumnFlags_WidthStretch);
      for (int i = 0; i < order_count; ++i) {
        const int slot = order[i];
        if (const Piece* piece = Find(slot, costume)) {
          DrawRow(*piece, slot, costume);
          continue;
        }
        if (!show_empty_) continue;

        // Emplacement VIDE. La native n'en disait rien ; savoir que la cible n'a
        // ni cape ni accessoire est pourtant la moitié de ce qu'on vient
        // regarder. Le bouclier occupé par une arme à deux mains le dit.
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", SlotLabel(slot));
        ImGui::TableNextColumn();
        const Piece* weapon = costume ? nullptr : Find(1, false);
        if (slot == 5 && weapon != nullptr && weapon->two_handed)
          ImGui::TextDisabled("%s", i18n::Tr("(arme à deux mains)"));
        else
          ImGui::TextDisabled("%s", i18n::Tr("—"));

        // Une ligne vide chez la cible ne l'est pas forcément chez moi : c'est
        // même le cas le plus parlant de la comparaison.
        if (compare_) {
          ImGui::TableNextColumn();
          ImGui::PushID(2000 + slot * 2 + (costume ? 1 : 0));
          itemcell::ChatLink mine;
          std::string mine_label;
          if (MyPiece(slot, costume, &mine, &mine_label))
            DrawItemCell(mine, mine_label, 1);
          else
            ImGui::TextDisabled("%s", i18n::Tr("—"));
          ImGui::PopID();
        }
      }
      ImGui::EndTable();
    }
    ImGui::PopID();
  };

  // 🔴 L'OUVERTURE se fait ICI, pas au clic : l'identifiant d'un popup se hache
  // avec la pile d'ids courante, et un `OpenPopup` appelé sous le `PushID` d'une
  // cellule donnerait un id que ce `BeginPopup`-ci ne retrouverait jamais.
  // ⚠ Dans le MÊME `BeginChild` que les cellules — un popup appartient à sa
  // fenêtre, et l'enfant en est une.
  if (menu_open_) {
    menu_open_ = false;
    ImGui::OpenPopup(kItemMenuId);
  }
  links::DrawMenu(kItemMenuId, menu_target_);

  section(i18n::Tr("Équipement###viewequip_sec_equip"), kEquipOrder,
          IM_ARRAYSIZE(kEquipOrder), false);

  // La section Costume n'existe que si la cible en porte : une section vide de
  // dix lignes « — » ne dit rien de plus que son absence.
  bool has_costume = false;
  for (const Piece& p : target_.pieces)
    if (p.costume) { has_costume = true; break; }
  if (has_costume)
    section(i18n::Tr("Costume###viewequip_sec_costume"), kCostumeOrder,
            IM_ARRAYSIZE(kCostumeOrder), true);

  // Les pièces hors grille : munition, et l'équipement d'OMBRE — que le client
  // natif ferait tomber sur les index des costumes, où il en écraserait un
  // (docs §6.2). Ici, chacun garde sa ligne.
  bool has_extra = false;
  for (const Piece& p : target_.pieces)
    if (p.ammo || p.shadow) { has_extra = true; break; }
  if (has_extra &&
      ImGui::CollapsingHeader(i18n::Tr("Autres###viewequip_sec_other"),
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::PushID("other");
    if (ImGui::BeginTable("###viewequip_tbl_other", 2, kFlags)) {
      ImGui::TableSetupColumn("###slot", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::CalcTextSize(i18n::Tr("Accessoire gauche")).x);
      ImGui::TableSetupColumn("###item", ImGuiTableColumnFlags_WidthStretch);
      int index = 0;
      for (const Piece& p : target_.pieces) {
        if (!p.ammo && !p.shadow) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", p.ammo ? i18n::Tr("Munition")
                                         : i18n::Tr("Ombre"));
        ImGui::TableNextColumn();
        ImGui::PushID(1000 + index++);
        DrawItemCell(p.link, p.label, 0);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    ImGui::PopID();
  }
}

// L'aperçu de l'objet survolé. 🔴 Appelé APRÈS `EndRoWindow` : il crée son
// propre popup, et un popup ouvert DANS une fenêtre en hérite le clip — le même
// patron que les grilles d'inventaire et d'échoppe.
//
// C'est `links::HoverPreview` et non un tooltip à nous : la description simple
// d'un objet est la même partout dans le client, cartes et options d'INSTANCE
// comprises (la `Target` porte la balise relue, pas seulement l'id).
void ViewEquipWindow::DrawHoverPreview() {
  if (!hover_valid_) return;
  links::HoverPreview(hover_target_);
}

void ViewEquipWindow::OnRenderUI() {
  if (!imgui_enabled_ || !open_) return;

  hover_valid_ = false;  // reposé à chaque frame par la cellule survolée

  if (need_focus_) {
    ImGui::SetNextWindowFocus();
    need_focus_ = false;
  }
  // Taille FIXE, ni repli ni poignée de redimensionnement (`ImGuiCond_Always` :
  // sans lui, une taille rangée dans l'`imgui.ini` d'une version précédente
  // survivrait au flag `NoResize`, et la fenêtre rouvrirait à l'ancienne taille
  // sans qu'on puisse plus la corriger). La liste défile dans son cadre quand la
  // cible porte des costumes en plus de son équipement.
  ImGui::SetNextWindowSize(compare_ ? kWindowSizeCompare : kWindowSize,
                           ImGuiCond_Always);

  // ⚠ L'id `###viewequip` fige l'identité ImGui : inspecter quelqu'un d'autre
  // change le titre, pas la position ni la taille que le joueur a choisies.
  char title[192];
  if (target_.name.empty()) {
    _snprintf_s(title, sizeof(title), _TRUNCATE, "%s###viewequip",
                i18n::Tr("Équipement"));
  } else {
    char named[160];
    _snprintf_s(named, sizeof(named), _TRUNCATE, i18n::Tr("Équipement de %s"),
                target_.name.c_str());
    _snprintf_s(title, sizeof(title), _TRUNCATE, "%s###viewequip", named);
  }

  const bool begun = ro::BeginRoWindow(
      title, &open_, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
  if (begun) {
    // En-tête : ce que le paquet dit de la personne, et rien de plus. Ni niveau
    // ni guilde — ils n'y sont pas, et les deviner serait mentir.
    ImGui::TextDisabled("%s", target_.sex == 1 ? i18n::Tr("Homme")
                                               : i18n::Tr("Femme"));
    ImGui::SameLine();
    const double age = ImGui::GetTime() - target_.received_at;
    ImGui::TextDisabled(i18n::Tr("· relevé il y a %d s"),
                        static_cast<int>(age));
    ImGui::SameLine();

    // ── Outils du STAFF ──────────────────────────────────────────────────────
    // Trois commandes de moonlight, rejouées par le canal du chat : le SERVEUR
    // reste la seule autorité (`conf/import/groups.yml` accorde `cloneequip`,
    // `clonestat` et `jobchange` au groupe 80, exactement le seuil d'`IsStaff()`)
    // et son verdict revient dans le chat. Rien n'est réimplémenté ici — c'est
    // la règle du projet sur tout ce qui a déjà une atcommand.
    //
    // ⚠ « Cloner : » et trois libellés courts, parce que trois libellés complets
    // ne tenaient pas sur la ligne : ils auraient poussé « Actualiser » hors de
    // la fenêtre, dont la largeur est FIXE.
    // ⚠ Les deux premières prennent un NOM de personnage — la seule identité que
    // la réponse du serveur nous donne (elle ne porte pas l'AID) — et
    // `QueueCommand` convertit vers l'encodage du fil ET diffère l'envoi hors
    // frame ImGui.
    if (IsStaff() && !target_.name.empty()) {
      char cmd[64];
      ImGui::TextDisabled("%s", i18n::Tr("Cloner :"));
      ImGui::SameLine();

      if (ro::RoButton(i18n::Tr("équipement###viewequip_cloneequip"))) {
        std::snprintf(cmd, sizeof(cmd), "@cloneequip %s", target_.name.c_str());
        if (auto* chat = Bourgeon::Instance().chat_window()) chat->QueueCommand(cmd);
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
                          i18n::Tr("Copie SUR MOI l'équipement de ce joueur "
                                   "(@cloneequip). Le serveur refuse sur "
                                   "soi-même."));
      ImGui::SameLine();

      if (ro::RoButton(i18n::Tr("stats###viewequip_clonestat"))) {
        std::snprintf(cmd, sizeof(cmd), "@clonestat %s", target_.name.c_str());
        if (auto* chat = Bourgeon::Instance().chat_window()) chat->QueueCommand(cmd);
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
                          i18n::Tr("Remplace MES statistiques par les siennes "
                                   "(@clonestat)."));
      ImGui::SameLine();

      // 🔴 `@job` prend un ID DE CLASSE, et ce que le paquet nous donne est la
      // classe AFFICHÉE (`vd.look[LOOK_BASE]`) : sur un joueur monté ou déguisé,
      // c'est une classe « factice » (JOB_KNIGHT2 & co) que le serveur REFUSE
      // explicitement (« You can not change to this job by command. »). On
      // envoie quand même : le refus arrive dans le chat, ce qui vaut mieux que
      // de deviner une classe de base à partir d'une table à maintenir.
      if (ro::RoButton(i18n::Tr("job###viewequip_clonejob"))) {
        std::snprintf(cmd, sizeof(cmd), "@job %d", target_.job);
        if (auto* chat = Bourgeon::Instance().chat_window()) chat->QueueCommand(cmd);
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
                          i18n::Tr("Me donne la MÊME classe (@job).\n"
                                   "⚠ Sur moonlight cette commande enchaîne aussi "
                                   "@blvl 999, @jlvl 100, @allskills et @allstats.\n"
                                   "Un joueur monté ou déguisé sera refusé : sa "
                                   "classe affichée est une classe factice."));
      ImGui::SameLine();
    }

    // Le bouton d'actualisation n'a de sens que si l'on sait À QUI redemander —
    // ce que seul le menu contextuel nous a dit (le paquet, lui, ne porte pas
    // l'AID). Grisé plutôt que masqué : un bouton qui disparaît n'explique rien.
    const float bw = ImGui::CalcTextSize(i18n::Tr("Actualiser")).x + 24.0f;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw - 16.0f);
    ImGui::BeginDisabled(target_.aid == 0);
    if (ro::RoButton(i18n::Tr("Actualiser###viewequip_refresh"), bw, 0.0f))
      RequestRefresh();
    ImGui::EndDisabled();
    if (target_.aid == 0 && ImGui::IsItemHovered())
      ImGui::SetTooltip("%s",
                        i18n::Tr("Cette fiche n'a pas été demandée depuis le menu "
                                 "contextuel : le paquet ne porte pas l'identifiant "
                                 "du joueur."));

    ImGui::Separator();

    const float doll_w = 150.0f;
    const float avail_h = ImGui::GetContentRegionAvail().y - 28.0f;
    ImGui::BeginChild("###viewequip_doll", ImVec2(doll_w, avail_h), false,
                      ImGuiWindowFlags_NoScrollbar);
    DrawDollPanel(doll_w - 8.0f, avail_h);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("###viewequip_list", ImVec2(0.0f, avail_h), false);
    DrawPieceList();
    ImGui::EndChild();

    ro::RoCheckbox(i18n::Tr("Emplacements vides###viewequip_empty"), &show_empty_);
    ImGui::SameLine();
    ro::RoCheckbox(i18n::Tr("Comparer###viewequip_compare"), &compare_);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s",
                        i18n::Tr("Affiche en regard ce que JE porte au même "
                                 "emplacement. La fenêtre s'élargit."));
    ImGui::SameLine();
    ImGui::TextDisabled(i18n::Tr("· %d pièce(s)"),
                        static_cast<int>(target_.pieces.size()));

    // Un vrai bouton « Fermer », en plus de la croix de la barre de titre : la
    // croix du skin RO est petite, et cette fenêtre se ferme souvent (on inspecte
    // quelqu'un, on regarde, on referme).
    //
    // ⚠ On écrit `open_` et rien d'autre : c'est le MÊME chemin que la croix
    // (`ro::BeginRoWindow(title, &open_)`), donc aucun second état de fermeture à
    // tenir à jour. La fiche, elle, reste en mémoire — rouvrir sur la même cible
    // ne redemande rien au serveur.
    const float cw = ImGui::CalcTextSize(i18n::Tr("Fermer")).x + 28.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - cw - 16.0f);
    if (ro::RoButton(i18n::Tr("Fermer"), cw, 0.0f)) open_ = false;
  }
  ro::EndRoWindow();

  DrawHoverPreview();
}
