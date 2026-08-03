#pragma once

// ── Pantin d'un personnage, composé chez nous ────────────────────────────────
//
// Un personnage RO n'est pas UN sprite : c'est un ASSEMBLAGE de .spr/.act
// indépendants — corps, tête, coiffes — recalés les uns sur les autres par les
// points d'ancrage de leurs images.
//
// Ce module compose cet assemblage à partir d'une simple APPARENCE (job, sexe,
// coiffure, couleurs…), sans acteur en scène, sans session en jeu et sans
// fenêtre native. Il marche donc au login comme au char-select.
//
// ── Ce qu'il a remplacé ──────────────────────────────────────────────────────
// Un moteur de CAPTURE : le client rendait l'acteur hors écran et on interceptait
// les quads qu'il dessinait (hook sur Actor_SubmitSpriteQuad). C'était juste et
// complet, mais ça exigeait un acteur vivant, un hook global, un cache de pages
// d'atlas et une dizaine d'offsets de structure devinés. Ici, rien de tout ça :
// on lit les fichiers. Le char-select, l'aperçu d'article et l'avatar de la
// fiche de personnage y sont passés ; il ne subsiste de capture que pour
// l'export GIF, qui a besoin d'un rendu hors écran de toute façon.
//
// ── Ce qu'il ne fait pas ─────────────────────────────────────────────────────
// Les compagnons (cart, faucon) et les effets .str n'en sont pas. Ils n'ont
// rien à y faire : ce sont des sprites INDÉPENDANTS, posés par rapport au
// personnage et non assemblés avec lui. L'appelant les dessine lui-même, avec
// le repère que `DollPlacement` lui rend — derrière via `DollDrawOpts::underlay`,
// devant en dessinant après.
//
// La CAPE (`DollLook::garment`), l'ARME et le BOUCLIER (`DollLook::weapon`,
// `shield`), eux, sont bien de l'assemblage : ils entrent dans le cadrage et
// s'intercalent dans l'ordre de dessin.

#include <cstdint>

#include "imgui.h"

namespace ro {

// Une pièce TENUE EN MAIN — arme, traînée d'arme, bouclier — désignée par ses
// FICHIERS et non par un id d'objet.
//
// 🔴 Pourquoi des chemins, alors que tout le reste du pantin part d'un id : la
// résolution native (`CActorSprite_BuildWeaponLayers` 0x00d403a0) ne se
// reproduit pas honnêtement. Elle enchaîne une classe d'arme, un « seau » de
// bouclier, une demi-douzaine de cas particuliers (montures, madogear), et
// surtout deux SONDAGES d'existence de fichier qui décident du repli. La
// recopier au jugé, c'est se tromper sur les cas rares sans jamais le savoir.
//
// Le client, lui, a déjà fait ce travail : les chemins résolus sont posés sur
// l'acteur du joueur, et `ragnarok/own_actor.h` les y lit. Le composeur reste
// donc ce qu'il est — un lecteur de fichiers — et n'hérite d'aucune de ces
// règles.
//
// ⚠ Conséquence assumée : arme et bouclier n'apparaissent que sur un pantin
// dont l'acteur existe en jeu. Au char-select ils resteront absents, ce qui est
// aussi le comportement du char-select natif.
struct DollHeldPiece {
  const char* spr_base = nullptr;  // chemin VFS SANS extension ; nullptr = rien
  const char* act_base = nullptr;  // nullptr = même base que le .spr
};

// Apparence d'un personnage. Mêmes champs que `BasicInfo::DollLook`, dont les
// offsets viennent de CHARACTER_INFO (char-select natif) — on garde une
// structure distincte pour que ce module ne dépende d'aucun plugin.
struct DollLook {
  int sex = 0;            // 0 = femme, 1 = homme (DÉJÀ résolu, jamais 99)
  int job = 0;            // id de classe
  // 🔴 La CLASSE qui nomme le sprite de corps — et non un « style ».
  //
  // `Job_ResolveBodyClass(job, body, 1)` (0x00d99150) ne lit `job` que pour
  // reconnaître un BÉBÉ ou une MONTURE ; c'est `body` qu'il remappe et qu'il
  // rend. Le laisser à 0 demande donc la classe 0 : tout le monde s'affiche en
  // Novice, quelle que soit la valeur de `job`.
  //
  // En jeu, c'est la classe BRUTE du joueur (g_Own_JobId) ; au char-select,
  // c'est le champ de CHARACTER_INFO +0x58, que la liste appelle « style de
  // corps » mais qui porte bien la classe. Un style de corps équipé y arrive
  // sous son propre identifiant (4332..4344), que le natif remappe.
  int body = 0;
  int hair = 0;           // id de coiffure BRUT (le remap est fait ici)
  int hair_color = -1;    // -1 = palette d'origine du sprite
  int clothes_color = -1; // -1 = palette d'origine du sprite
  int head_low = 0;       // accessoire du bas   (0 = aucun)
  int head_top = 0;       // accessoire du haut
  int head_mid = 0;       // accessoire du milieu
  // Cape (garment), par son id de VUE. 0 = aucune.
  //
  // ⚠ Elle passe DEVANT ou DERRIÈRE le personnage selon l'orientation, et ce
  // n'est pas nous qui en décidons : le client interroge le global Lua
  // `DrawOnTop`. Voir le .cc.
  int garment = 0;

  // Arme, traînée d'arme et bouclier. Vides = mains nues.
  //
  // ⚠ Les trois se dessinent d'un bloc, dans cet ordre — traînée, arme,
  // bouclier — et ce bloc passe DERRIÈRE tout le personnage dans les
  // orientations de dos. C'est le composeur qui s'en charge.
  DollHeldPiece weapon;
  DollHeldPiece weapon_trail;
  DollHeldPiece shield;
};

// Où le pantin a atterri, une fois le cadrage résolu.
//
// `origin_x`/`origin_y` = le point où tombe l'origine (0,0) du repère sprite,
// c'est-à-dire l'ORIGINE DE L'ACTEUR — ses pieds, pas le centre de sa boîte.
// C'est exactement la convention `(ox, oy, s)` qu'attendent les couches d'effets
// de costume (`DrawStrCapLayers`, `DrawEzCapTris`) : elles peuvent donc se poser
// sur un pantin composé sans rien recalculer.
struct DollPlacement {
  float origin_x = 0.0f;
  float origin_y = 0.0f;
  float scale    = 1.0f;  // une unité .act -> pixels écran

  // Boîtes du pantin en unités .act, dans le repère de l'acteur — donc AVANT
  // `origin` et `scale`. Elles servent à cadrer sur une partie plutôt que sur
  // l'ensemble : un portrait veut la tête, pas les pieds.
  //
  // `head` couvre la tête ET ses coiffes ; `body` le corps seul. `*_valid` dit
  // si la partie a été rencontrée — une race sans sprite de tête existe.
  float head_x0 = 0.0f, head_y0 = 0.0f, head_x1 = 0.0f, head_y1 = 0.0f;
  float body_x0 = 0.0f, body_y0 = 0.0f, body_x1 = 0.0f, body_y1 = 0.0f;
  bool  head_valid = false;
  bool  body_valid = false;

  // Nombre d'images de l'action du CORPS pour la pose demandée, et durée d'une
  // image en unités .act (25 ms chacune). De quoi enregistrer une animation
  // complète sans redemander le `.act`.
  int frame_count = 1;
  int frame_delay = 4;
};

// Un calque prêt à peindre, en coordonnées ÉCRAN — ce que `DrawDoll` enverrait
// à ImGui. Les quatre coins sont donnés séparément : ceux de l'arme et du
// bouclier sont TOURNÉS par le `.act`, un rectangle ne suffirait pas.
struct DollQuad {
  void*  tex;
  ImVec2 corner[4];
  ImVec2 uv0, uv1;
  ImU32  tint;
};

// Reçoit les calques dans l'ordre de dessin, du fond vers l'avant.
using DollQuadSinkFn = void (*)(void* ctx, const DollQuad& quad);

// Rappel invoqué UNE fois, le cadrage résolu et AVANT le premier calque.
//
// 🔴 C'est le seul moment où l'on peut glisser quelque chose DERRIÈRE le pantin.
// Il ne s'agit pas d'une commodité : un effet de costume marqué « avant le
// personnage » doit passer sous TOUS ses calques, cape comprise, et le pantin se
// dessine en un seul appel.
//
// ⚠ Pourquoi un rappel plutôt qu'une fonction de mesure séparée : mesurer
// demanderait de rejouer tout le chargement, l'ancrage et le cadrage une
// deuxième fois — donc de payer deux fois, et surtout de risquer que la mesure
// et le dessin divergent. Une seule traversée, une seule vérité.
using DollUnderlayFn = void (*)(void* ctx, const DollPlacement& placement);

// Réglages optionnels. Ils ont leur structure parce qu'ils sont nombreux, rares,
// et qu'une liste d'arguments à treize positions ne se relit pas.
struct DollDrawOpts {
  int      dir          = 0;       // orientation 0..7 (0 = de face)
  int      anim         = 0;       // type d'action ; pose = anim*8 + dir
  float    anim_seconds = -1.0f;   // horloge ; NÉGATIF = figé sur la 1ʳᵉ image
  uint32_t tint         = 0xFFFFFFFFu;

  // Cadrer sur le CORPS SEUL au lieu de la silhouette entière.
  //
  // 🔴 Par défaut, l'échelle est calculée pour que TOUT rentre — coiffes et cape
  // comprises. C'est ce qu'on veut d'une vignette, où rien ne doit déborder.
  // C'est faux dans un APERÇU de costume : un chapeau volumineux rétrécirait le
  // personnage, et sa taille changerait d'un article survolé au suivant alors
  // que c'est justement la référence de comparaison. Avec ce drapeau, l'échelle
  // ET le centrage ne dépendent que du corps ; les pièces rapportées peuvent
  // dépasser du rectangle — `DrawDoll` ne rogne rien, c'est à l'appelant de
  // prévoir la marge (ou de poser son propre clip s'il veut couper).
  bool fit_body_only = false;

  // Figer le CORPS sur son image 0 — sans figer les accessoires.
  //
  // 🔴 Ce n'est pas la même chose qu'une horloge négative, qui fige TOUT. Les
  // deux horloges du pantin sont indépendantes : le corps ne défile qu'en Marche
  // et en Combat, les coiffes et la cape vivent en permanence sur la leur. Une
  // vue qui propose « Marche » et « Marche (animé) » ne parle que de la
  // PREMIÈRE ; les costumes, eux, doivent continuer à bouger dans les deux cas,
  // comme ils le font en jeu sur un personnage à l'arrêt.
  bool freeze_body = false;

  // Centrer et poser les PIEDS d'après le corps seul, mais garder l'échelle
  // calculée sur la silhouette entière.
  //
  // 🔴 Ce n'est PAS `fit_body_only` : là, tout rentre encore — c'est le repère
  // qui change. Sans ça, une arme à deux mains élargit la boîte d'un seul côté
  // et pousse le personnage à l'opposé ; une cape longue lui enfonce les pieds
  // sous le bas du rectangle. Le corps est le seul point fixe honnête.
  //
  // ⚠ La demi-largeur retenue est la plus grande des deux, mesurée DEPUIS le
  // centre du corps : aucun côté ne peut alors sortir du rectangle.
  bool center_on_body = false;

  // Mesurer l'enveloppe MAXIMALE : toutes les images, toutes les orientations.
  //
  // 🔴 Sans ça, l'échelle est calculée sur l'image 0 de l'orientation affichée —
  // et elle change donc quand le personnage tourne ou quand son animation
  // l'étire. C'est visible : à la molette, le pantin grandit et rétrécit, parce
  // qu'une arme s'écarte de face et se replie de dos.
  //
  // Les coiffes en sont la meilleure raison : beaucoup de costumes posent leur
  // sprite À CÔTÉ du personnage (un compagnon, une monture) et débordent bien
  // plus que lui. Les exclure de la mesure les fait rogner ; les mesurer sur la
  // seule image affichée fait sauter l'échelle. Il faut donc l'enveloppe de
  // TOUT, prise une bonne fois.
  //
  // ⚠ Le coût est réel : chaque pièce est résolue pour les 8 orientations et
  // toutes leurs images, à chaque frame de rendu. C'est du calcul pur, sans
  // rechargement (les ressources sont déjà en cache), et l'ordre de grandeur
  // reste celui d'une centaine de résolutions de frame. À réserver à une vue
  // unique, pas à une grille de vignettes.
  bool fit_span = false;

  // Hauteur MINIMALE du corps à l'écran, en pixels. <= 0 = aucune.
  //
  // 🔴 C'est un plancher, et il existe parce que la largeur peut écraser le
  // personnage. Beaucoup de coiffes de costume posent leur sprite À CÔTÉ de lui
  // — un compagnon, une monture : l'enveloppe devient deux fois plus large que
  // haute, et dans un cadre étroit, tout faire rentrer réduit le personnage de
  // moitié alors qu'il reste du vide au-dessus.
  //
  // Passé ce plancher, on cesse de tout faire rentrer : le personnage garde sa
  // stature et ce qui dépasse sur les côtés déborde du rectangle. `DrawDoll` ne
  // rogne rien — c'est à l'appelant de poser un clip s'il le souhaite.
  //
  // La hauteur du CORPS, et non celle de la silhouette : c'est la seule mesure
  // qui ne dépende ni de l'équipement ni de l'orientation, donc la seule qui
  // donne une stature comparable d'un personnage à l'autre.
  float min_body_height = 0.0f;

  // Plafond d'échelle, en pixels par unité .act. <= 0 = aucun.
  //
  // Sert à faire tenir dans le cadre autre chose que le pantin — un effet de
  // costume plus grand que lui. L'appelant mesure l'effet, en déduit l'échelle
  // qui le ferait rentrer, et la pose ici : le personnage rétrécit AVEC son
  // effet, ce qui est le seul moyen de garder les deux cohérents.
  float scale_limit = 0.0f;

  // Tout calculer sans rien dessiner : `out_placement` est renseigné, aucun
  // quad n'est émis, `underlay` n'est pas appelé.
  //
  // 🔴 Sert à cadrer sur une PARTIE du pantin. Les boîtes de `DollPlacement`
  // n'existent qu'une fois la composition faite, et ImGui dessine dans l'ordre
  // des appels : impossible de « revenir en arrière » pour recadrer. On mesure
  // donc d'abord, on décide, puis on dessine — c'est ce que fait le portrait.
  bool measure_only = false;

  // Imposer origine et échelle au lieu de les calculer d'après [x,y,w,h].
  //
  // Seuls `origin_x`, `origin_y` et `scale` sont lus. Le rectangle passé à
  // `DrawDoll` n'est alors plus qu'un cadre de dessin — c'est à l'appelant de
  // le clipper s'il veut couper.
  const DollPlacement* place_override = nullptr;

  // Jouer CETTE image du corps plutôt que celle de l'horloge. < 0 = horloge.
  //
  // Sert à enregistrer une animation image par image : le temps qui passe ne
  // permet pas de viser une image précise, et échantillonner l'horloge en
  // sauterait ou en doublerait.
  //
  // ⚠ L'image des accessoires en DÉCOULE (elle se calcule à partir de celle du
  // corps), donc forcer le corps suffit à figer tout le pantin.
  int force_frame = -1;

  // Recevoir les calques au lieu de les peindre. Non nul = rien n'est envoyé à
  // `draw_list`, tout passe par le puits — dans le même ordre.
  DollQuadSinkFn quad_sink     = nullptr;
  void*          quad_sink_ctx = nullptr;

  DollUnderlayFn underlay      = nullptr;
  void*          underlay_ctx  = nullptr;
  DollPlacement* out_placement = nullptr;
};

// Dessine le pantin dans [x, y, w, h] de la fenêtre ImGui courante : ratio
// conservé, corps centré en X, pieds ancrés en BAS du rectangle.
//
//   dir           orientation 0..7 (0 = de face)
//   anim          type d'action (0 = debout, 2 = assis…) ; pose = anim*8 + dir
//   anim_seconds  horloge de l'appelant (ex. ImGui::GetTime()). NÉGATIF = figé
//                 sur la première image, ce que veut une vignette.
//   tint          multiplication de couleur (IM_COL32) ; 0xFFFFFFFF = aucune
//
// ⚠ DEUX horloges, et c'est là qu'est tout le piège.
//
// Le CORPS et la TÊTE ne défilent qu'en MARCHE (anim 1) et en COMBAT (anim 4) —
// vérifié au débogueur (sur un personnage assis, `acteur+0x3c` reste à 0) et
// confirmé en dur dans le client, qui force l'image à 0 pour les autres types
// d'action. Partout ailleurs ils restent sur leur image 0 : leurs images sont
// des poses et des expressions de visage, pas une décoration.
//
// Les ACCESSOIRES s'animent quand même, sur leur propre horloge : c'est
// `Act_ResolveAltAnimFrame`, dont le portage vit dans le .cc. Une coiffe porte
// un MULTIPLE exact des images de la tête et parcourt son SOUS-GROUPE au fil du
// temps réel. D'où des costumes qui vivent sur un personnage immobile.
//
// Rend false si rien n'a pu être dessiné — à l'appelant de poser son
// placeholder. Un membre manquant (coiffe absente du GRF) n'est PAS un échec :
// il est simplement omis.
//
// ⚠ `underlay` et `out_placement` n'interviennent qu'une fois le cadrage
// résolu — donc jamais sur les échecs précoces (corps introuvable, rectangle
// dégénéré), mais bien dans le cas de bord où le cadrage aboutit et où aucun
// calque ne se révèle dessinable. Un appelant qui superpose des effets doit
// donc quand même tester le retour avant sa passe de devant : sans pantin, une
// ancre seule ne veut rien dire.
bool DrawDoll(ImDrawList* draw_list, const DollLook& look, float x, float y,
              float w, float h, const DollDrawOpts& opts);

// Forme courte, pour les appelants qui n'ont besoin que de la pose — la grande
// majorité. Strictement équivalente à la précédente avec des options par défaut.
bool DrawDoll(ImDrawList* draw_list, const DollLook& look, float x, float y,
              float w, float h, int dir = 0, int anim = 0,
              float anim_seconds = -1.0f, uint32_t tint = 0xFFFFFFFFu);

}  // namespace ro
