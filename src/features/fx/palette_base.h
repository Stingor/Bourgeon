#ifndef BOURGEON_FEATURES_FX_PALETTE_BASE_H_
#define BOURGEON_FEATURES_FX_PALETTE_BASE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "ui/palette_ramps.h"

// La base d'édition d'un corps : la palette sur laquelle une recette s'applique,
// et les rampes qu'on y a trouvées.
//
// ── Pourquoi ce module existe séparément ─────────────────────────────────────
// La même construction sert DEUX appelants qui n'ont rien d'autre en commun :
// l'éditeur, pour le joueur local, et la propagation réseau, pour chaque autre
// joueur en vue. 🔴 Elle doit donner exactement le même résultat des deux côtés —
// sinon deux clients numérotent les rampes différemment et une recette désigne
// la mauvaise pièce du costume. La partager est donc une exigence, pas un
// confort.

namespace fx {
namespace palette_base {

struct Body {
  // Le sprite réellement édité, base sans extension (`data\sprite\...`).
  std::string spr_path;
  // 1024 octets RGBA : palette du sprite fusionnée avec celle du serveur.
  std::vector<uint8_t> base;

  // La palette INTERNE du `.spr`, brute, avant toute fusion.
  //
  // 🔴 Exposée parce que c'est le seul ingrédient qu'on ne puisse pas se
  // reprocurer à bon compte : la relire coûte l'analyse complète du sprite (une
  // centaine d'images). Qui veut fusionner d'AUTRES palettes de vêtement sur ce
  // corps — la grille de sélection de teinte, qui en montre 553 — la garde donc
  // une fois et ne relit ensuite qu'un `.pal` de 1 Kio par vignette.
  std::vector<uint8_t> spr_palette;

  ro::PaletteRamp ramps[ro::kMaxRamps];
  int ramp_count = 0;
  // Diagnostic : pixels peints au total, et ceux qu'une rampe couvre donc rend
  // réglables. L'écart est la part du corps qu'AUCUN curseur n'atteint — la
  // première chose à regarder quand un joueur dit « ça ne change pas cette
  // zone-là ».
  int pixels_total = 0;
  int pixels_covered = 0;

  // Pixels peints tombant sur un index quasi NOIR — avant fusion (sous la seule
  // palette du serveur) puis après. Leur écart est ce que la fusion RÉPARE.
  //
  // 🔴 C'est le critère de la réparation automatique : on n'impose la palette du
  // sprite à un corps que si l'on peut MONTRER qu'elle le répare. Un seuil sur
  // la seule noirceur absolue serait arbitraire — beaucoup de sprites ont des
  // contours noirs parfaitement voulus.
  int pixels_black_native = 0;
  int pixels_black_merged = 0;

  // Ce résultat venait-il du cache, donc sans rien coûter ?
  //
  // 🔴 Exposé pour que la boucle d'application des recettes puisse budgéter ce
  // qui COÛTE au lieu de compter des appels. Elle s'accorde deux constructions
  // par battement pour ne pas figer l'image sur une carte peuplée ; sans ce
  // drapeau, elle compterait aussi les vingt reprises gratuites du même corps et
  // ferait attendre les joueurs pour rien — c'était exactement le symptôme :
  // une seconde de couleurs d'origine à la connexion.
  bool cached = false;
};

enum Status {
  kOk = 0,
  kNoSprite,     // aucun chemin de sprite exploitable
  kUnreadable,   // le .spr n'a pas pu être lu
  kNoPalette,    // .spr sans palette (Bgra32 : couleurs par pixel)
  kNoRamp,       // aucun dégradé détecté
};

// Message traduit correspondant, prêt à afficher.
const char* StatusText(Status s);

// Construit la base à partir des DEUX chemins, sans rien demander au jeu.
//
// C'est la forme de fond, et la seule utilisable hors session : au char-select
// il n'y a pas d'acteur, donc rien à qui demander la palette de vêtement — mais
// `CHARACTER_INFO` porte la couleur, dont le chemin se déduit.
// `pal_path` nul ou vide = pas de fusion, la palette du sprite seule.
Status BuildFromPaths(const char* spr_base, const char* pal_path, Body* out);

// Construit la base à partir d'un chemin de sprite donné. `gid` ne sert qu'à
// retrouver la palette de serveur que le natif avait choisie pour cet acteur.
Status BuildFromSpritePath(const char* spr_base, uint32_t gid, Body* out);

// Numéro le plus élevé d'une palette de vêtement officielle : elles vont de
// **1 à 553**.
//
// 🔴 Et non de 0 à 552. `body_0.pal` existe bien sur le disque (554 fichiers en
// tout), mais le jeu ne l'emploie JAMAIS : une couleur de vêtement nulle veut
// dire « aucune palette externe », et le client garde alors la palette interne
// du sprite. Proposer 0 offrirait donc une teinte que personne ne voit, et
// décalerait tous les numéros par rapport à ce que le serveur manipule.
constexpr int kOfficialPaletteMax = 553;

// Le chemin d'une palette officielle POUR CET ACTEUR, obtenu en remplaçant le
// numéro dans celui que le natif lui avait choisi.
//
// 🔴 On réécrit le chemin natif au lieu de le recomposer : le dossier dépend de
// la RACE (un Doram a le sien, et les deux indexations n'ont rien de commun), et
// le chemin natif la porte déjà. Le recomposer demanderait la classe de
// l'acteur, qu'on n'a pas pour un autre joueur.
bool OfficialPalettePath(uint32_t gid, int palette_id, char* out,
                         size_t out_size);

// Construit la base d'un acteur en LISANT le sprite que le client a chargé pour
// lui (emplacement 0 du composite).
//
// 🔴 C'est la voie à préférer, et de loin. Re-dériver le chemin depuis (classe,
// sexe, monture) rejoue une résolution native pleine de cas particuliers —
// styles de corps alternatifs des 3e/4e classes, montures, costumes — et une
// copie qui diverge donne des rampes VALIDES mais qui désignent les mauvais
// index : les curseurs agissent alors sur des zones au hasard et laissent
// intactes celles que le joueur visait. Le client, lui, a déjà tranché ; on lit
// son verdict.
Status BuildForGid(uint32_t gid, Body* out);

// Même chose, mais sur une palette de vêtement IMPOSÉE (0..552). `palette_id`
// négatif retombe sur celle du serveur, donc sur la surcharge ci-dessus.
Status BuildForGid(uint32_t gid, int palette_id, Body* out);

}  // namespace palette_base
}  // namespace fx

#endif  // BOURGEON_FEATURES_FX_PALETTE_BASE_H_
