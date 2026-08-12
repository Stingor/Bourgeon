#ifndef BOURGEON_UI_PALETTE_RAMPS_H_
#define BOURGEON_UI_PALETTE_RAMPS_H_

#include <cstdint>
#include <vector>

#include "ui/spr_act.h"

// Rampes d'une palette de sprite, et recoloration par décalage HSV.
//
// ── Pourquoi des « rampes » ──────────────────────────────────────────────────
// Une palette RO n'est pas 256 couleurs indépendantes : c'est une suite de
// DÉGRADÉS, un par pièce du costume (la tunique, la cape, les bottes…), chacun
// allant du clair au foncé dans une teinte stable. Recolorier une pièce, c'est
// faire pivoter la teinte de SA rampe — le contraste et les reflets, eux, sont
// préservés. C'est ce qui permet à une recette de tenir en quelques octets tout
// en restant belle sur n'importe quel corps.
//
// ── Pourquoi la détection doit être DÉTERMINISTE ─────────────────────────────
// 🔴 Ce qui circule sur le réseau ne porte AUCUNE couleur : seulement des plages
// d'index et des décalages. Chaque client ré-applique la recette sur la palette
// interne du `.spr` qu'il possède déjà. Deux clients doivent donc trouver
// EXACTEMENT les mêmes rampes à partir des mêmes octets — d'où l'absence totale
// de flottant dans les seuils de coupure et l'ordre de tri explicite.
//
// La référence de cet algorithme est `tools/palette_ramps.py`, validé sur les
// 421 corps du client (0 sans rampe, 86 % des pixels couverts en moyenne au
// plafond de 8 rampes). Toute modification doit être faite DES DEUX CÔTÉS,
// sinon les clients divergent et les joueurs se voient dans des couleurs
// différentes. Le script couvre la chaîne ENTIÈRE — fusion, détection, recette
// dans les deux modes — et sa sortie a été comparée octet par octet à celle de
// ce fichier sur 8 corps contrastés.
//
// 🔴 Piège d'arrondi, déjà rencontré : `round()` de Python fait de l'arrondi
// BANCAIRE (`round(0.5) == 0`) là où `static_cast<int>(x + 0.5)` arrondit
// toujours au-dessus. Les deux côtés utilisent la seconde règle.

namespace ro {

// Le plafond de rampes exposées. 8 est le coude de la courbe de rendement,
// mesuré sur les 421 corps : 4 → 71 % des pixels couverts, 6 → 81 %, 8 → 86 %,
// 10 → 88 %, 12 → 89 %. Au-delà on paie des curseurs pour des broutilles.
// 🔴 Cette valeur dimensionne la recette réseau : la changer casse le protocole.
constexpr int kMaxRamps = 8;

// Une plage contiguë d'index formant un dégradé.
struct PaletteRamp {
  uint8_t start = 0;    // premier index de palette
  uint8_t length = 0;   // nombre d'index
  int pixels = 0;       // surface occupée, toutes images confondues
  int hue = -1;         // teinte dominante en degrés (0-359), -1 si neutre/gris

  // L'index qui REPRÉSENTE la rampe : sa pastille de couleur dans l'éditeur, et
  // le point d'appui quand le joueur désigne une couleur.
  //
  // 🔴 Le milieu de la plage, mais parmi les index PEINTS ET NON NOIRS. La
  // règle naïve — le milieu tout court — tombait sur du noir pur pour 84 des
  // 3302 pièces mesurées : leur pastille s'affichait noire alors que la pièce
  // monte jusqu'au blanc (mesuré sur `biolo_남` pièce 5 : milieu à 0, index le
  // plus clair peint à 255). Le joueur voyait un carré noir qui refusait de
  // changer, ce qui ressemble à une panne au point d'appeler un rapport de bug.
  // Pire, `AdjustToReach` y calculait un réglage de luminosité NUL, sa garde
  // `from.v > 0` échouant sur un noir.
  //
  // ⚠ Cet index est purement LOCAL. Il ne voyage pas, et rien de ce qui voyage
  // n'en dépend : `ApplyRecipe` traite tous les index de la rampe et ne le
  // consulte jamais. Il échappe donc à la règle « toute modification des DEUX
  // côtés » — la référence Python n'a pas à le connaître, et le changer ne
  // périme aucune recette.
  uint8_t ref = 0;
};

// Le réglage d'UNE rampe, dans l'un de deux modes.
//
// ── RELATIF (défaut) ─────────────────────────────────────────────────────────
// `hue` pivote, `sat` et `val` MULTIPLIENT. La structure du dégradé est
// intégralement préservée — ombres, reflets, écarts de saturation entre index.
// 🔴 Sa limite : la saturation étant un facteur, une rampe terne le reste. Sur
// une pièce beige, +100 % ne fait que doubler une saturation déjà faible, là où
// une pièce vive sature à fond. D'où le second mode.
//
// ── ABSOLU ───────────────────────────────────────────────────────────────────
// `hue` et `sat` sont IMPOSÉS à toute la rampe ; seule la luminosité reste celle
// du dégradé (ajustée par `val`). C'est exactement ce que fait une palette de
// vêtement officielle : une teinte unie, modulée par la lumière du sprite.
// Toute pièce peut alors atteindre n'importe quelle couleur, quelle que soit la
// sienne au départ.
struct RampAdjust {
  int16_t hue = 0;   // relatif : -359..359 ; absolu : 0..359
  int8_t sat = 0;    // relatif : -100..100 ; absolu : 0..100
  int8_t val = 0;    // TOUJOURS relatif : -100..100
  uint8_t absolute = 0;  // 0 = relatif, 1 = absolu

  bool IsNeutral() const {
    return absolute == 0 && hue == 0 && sat == 0 && val == 0;
  }
};

// La recette complète d'un corps : un réglage par rampe détectée.
// 🔴 Elle ne vaut QUE pour le sprite dont elle a été tirée — les index n'ont
// aucune signification commune d'un corps à l'autre (mesuré : un Dragon Knight
// utilise 7-73, un Creator 112-223). Le sprite est donc à retenir avec.
struct PaletteRecipe {
  RampAdjust ramps[kMaxRamps];

  // Palette de vêtement OFFICIELLE sur laquelle la recette s'applique, 0..552.
  // -1 = celle que le serveur a assignée au personnage (le styliste).
  //
  // 🔴 Elle fait partie de la RECETTE, et non d'un réglage local, parce qu'elle
  // change la BASE. Or la base n'est pas transmise — chaque client la recalcule
  // à partir du `.spr` qu'il possède et de la palette de vêtement. Un joueur qui
  // choisirait ici sans que le numéro voyage serait vu par les autres dans de
  // tout autres couleurs.
  //
  // ⚠ Changer ce numéro change aussi les RAMPES détectées : la fusion n'est plus
  // la même, donc les frontières et le classement des pièces bougent. Les
  // réglages en place se ré-appliquent alors par INDEX, comme lors d'un
  // changement de sprite.
  int16_t palette_id = -1;

  // Palette de CHEVEUX officielle, 1..251. -1 = celle du personnage.
  //
  // 🔴 Elle voyage aussi, pour la même raison : sans elle, le joueur serait seul
  // à voir sa couleur. Mais contrairement au corps, elle ne passe par AUCUN
  // calcul — une palette de cheveux officielle est un vrai fichier, on se
  // contente d'en désigner un. Il n'y a donc ni rampes ni réglages HSV pour la
  // tête : la question ne s'est pas posée, les palettes de cheveux du client
  // fonctionnent.
  int16_t hair_palette_id = -1;

  // COIFFURE, 1..80. -1 = celle du personnage.
  //
  // 🔴 Elle est ici parce que POUR LE JOUEUR elle fait partie du style, au même
  // titre qu'une couleur — et c'est son point de vue qui commande la structure.
  // Elle n'est pourtant pas de même nature que le reste : les autres champs sont
  // une fiction de rendu que chaque client retraduit, alors que celui-ci
  // désigne un vrai état de personnage. Le serveur ne se contente donc pas de le
  // ranger : il l'APPLIQUE (`pc_changelook(LOOK_HAIR)`), et c'est le
  // ZC_SPRITE_CHANGE natif — pas notre recette — qui l'annonce aux autres,
  // clients vanilla compris.
  //
  // ⚠ Conséquence : sur un acteur AUTRE que soi, ce champ ne se pose pas. Il ne
  // sert qu'au transport de ce que le joueur demande, et à ce qui garde une
  // allure complète : préréglages et codes partageables.
  int16_t hair_style = -1;

  bool IsNeutral() const {
    if (palette_id >= 0 || hair_palette_id >= 0 || hair_style >= 0) return false;
    for (int i = 0; i < kMaxRamps; ++i)
      if (!ramps[i].IsNeutral()) return false;
    return true;
  }
};

// Compte les pixels par index sur toutes les images palettisées. L'index 0 est
// le transparent : il est forcé à 0 et ne peut jamais entrer dans une rampe.
void CountIndexUsage(const spract::Resource& res, int usage[256]);

// Fusionne la palette du SERVEUR par-dessus celle du SPRITE, en ne gardant du
// serveur que ce qu'il définit vraiment. Écrit 1024 octets RGBA dans `out`.
//
// ── Pourquoi cette fusion existe ─────────────────────────────────────────────
// Les deux palettes seules sont insuffisantes. Celle du serveur porte la couleur
// de vêtement que le joueur (et les autres) voient déjà, mais elle ne définit
// qu'une PARTIE des index : mesuré en mémoire vive sur body_56, 126 de ses 255
// entrées sont noires, et les corps de 4e classe utilisent massivement celles-là
// — d'où les silhouettes noires. Celle du sprite est complète mais ignore la
// teinte choisie. La fusion prend le meilleur des deux.
//
// ── Ce qui compte comme un « trou » ──────────────────────────────────────────
// 🔴 Pas « l'index est noir » : un noir peut être VOULU, et c'est même le cas le
// plus courant — les sprites RO ont des contours noirs. Le critère est la
// PLAGE : un dégradé qui part du clair et finit dans le noir est une ombre
// légitime, tandis qu'une plage noire d'un bout à l'autre n'est que du
// remplissage. On ne remplace donc un index que si TOUTE sa plage contiguë est
// noire.
//
// Mesuré sur les 421 corps du client : sans fusion 20,04 % des pixels tombent
// sur un index noir, 2,24 % avec le critère naïf (quatre octets nuls), et
// 1,26 % avec celui-ci — sans jamais toucher un contour.
bool MergeServerPalette(const uint8_t* sprite, size_t sprite_size,
                        const uint8_t* server, size_t server_size,
                        uint8_t* out, size_t out_size);

// Découpe `palette` (1024 octets RGBA) en dégradés, les plus VISIBLES d'abord.
// `usage` vient de `CountIndexUsage` : un index jamais peint n'appartient à
// aucune rampe, sinon on exposerait des curseurs qui ne changent rien.
// Rend le nombre de rampes écrites dans `out` (au plus `kMaxRamps`).
//
// 🔴 « Visible » n'est PAS « grand » : le rang vaut `pixels × (256 + saturation
// moyenne)`, pas la seule surface. Une pièce petite mais FRANCHE — une rune
// rouge, un œil qui brille — passe donc devant un grand aplat terne. Mesuré sur
// les 421 corps : 24 d'entre eux récupèrent ainsi une couleur vive qui tombait
// hors des huit retenues, dont la monture du Dragon Knight qui avait motivé le
// changement. 🔴 Ce classement fait PARTIE du protocole — une recette ne
// désigne ses pièces que par un rang, donc le modifier oblige à incrémenter
// `fx::style_sync::kWireVersion` et à jeter les recettes antérieures.
int DetectRamps(const uint8_t* palette, size_t palette_size,
                const int usage[256], PaletteRamp* out, int max_out);

// Applique la recette et écrit 1024 octets RGBA dans `out`.
// Les index hors rampes sont recopiés tels quels — y compris l'index 0.
bool ApplyRecipe(const uint8_t* palette, size_t palette_size,
                 const PaletteRamp* ramps, int ramp_count,
                 const PaletteRecipe& recipe, uint8_t* out, size_t out_size);

// Chemin court : détecte les rampes d'une ressource déjà chargée.
int DetectRamps(const spract::Resource& res, PaletteRamp* out, int max_out);

// La couleur REPRÉSENTATIVE d'une rampe (0xRRGGBB) : l'index du milieu, celui
// qui porte la couleur que le joueur croit voir. Le premier est souvent un
// reflet presque blanc, le dernier une ombre presque noire.
uint32_t RampColor(const uint8_t* palette, size_t palette_size,
                   const PaletteRamp& ramp);

// Le réglage qui amène la rampe de sa couleur d'origine à `target` (0xRRGGBB).
//
// C'est ce qui permet d'offrir un choix de couleur DIRECT sans renoncer au
// modèle par décalages : on convertit ce que le joueur montre en un décalage
// relatif, donc le dégradé garde ses ombres et ses reflets, et la recette reste
// transmissible en quelques octets.
RampAdjust AdjustToReach(const uint8_t* palette, size_t palette_size,
                         const PaletteRamp& ramp, uint32_t target);

// La clarté MAXIMALE (0..255) que `AdjustToReach` peut donner à cette rampe.
//
// 🔴 Une pièce peinte sombre le RESTE, et ce n'est pas un défaut : la luminosité
// s'applique en facteur pour préserver le modelé du dégradé — ombres, reflets,
// volume — et ce facteur est borné à +100 %. Le plafond vaut donc deux fois la
// clarté d'origine, jamais plus.
//
// Il faut pouvoir le DIRE au joueur. Sans lui, désigner un rouge vif sur une
// zone d'ombre rend un rouge sombre sans explication : la valeur « remonte puis
// redescend », ce qui se lit comme une commande cassée alors que c'est le sprite
// qui parle. Mesuré sur les 421 corps : 19 % des pièces n'atteignent pas le
// blanc, et 73 % des corps en ont au moins une nettement bridée — presque
// toujours des zones d'ombre voulues (entrejambe, dessous de bras, nuque).
//
// ⚠ À garder en phase avec la borne de `AdjustToReach` : ces deux-là décrivent
// la même limite, l'une en l'appliquant, l'autre en l'annonçant.
int RampValueCeiling(const uint8_t* palette, size_t palette_size,
                     const PaletteRamp& ramp);

}  // namespace ro

#endif  // BOURGEON_UI_PALETTE_RAMPS_H_
