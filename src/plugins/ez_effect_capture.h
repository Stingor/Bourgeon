#pragma once

// ── Capture PARTAGÉE des effets EZ (hat effects procéduraux) ──────────────────
//
// Socle commun à tous les consommateurs qui veulent RE-DESSINER un effet EZ ailleurs que là où le
// jeu le pose (overlay au centre de l'écran, doll de la fiche perso, aperçu cash-shop, tooltip
// d'item…). Le principe est de laisser le JEU faire tout le travail — on ne réimplémente aucun des
// ~170 sous-rendus — et de simplement intercepter les primitives qu'il émet pour NOTRE effet.
//
// Pourquoi ce module existe : cette capture est subtile à écrire correctement, et trois erreurs
// coûteuses ont déjà été payées (cf. project_spr_effect_lab). Les voici, pour que personne ne les
// réintroduise en réécrivant sa propre version :
//
//  1. LE NOMBRE DE SOMMETS N'EST PAS TOUJOURS 4. C'est un CHAMP de l'enregistrement (+0x04). La
//     majorité des effets EZ émettent des TRIANGLES de 3 sommets (TRIANGLELIST) ; seule la famille
//     STR/sprite émet des quads de 4 (TRIANGLESTRIP). Lire un 4ᵉ sommet inexistant lit le pointeur
//     de texture et les entiers de blend réinterprétés en float -> sommets fantômes à ~1000 px.
//  2. LES COULEURS SONT PAR SOMMET. Appliquer la couleur du sommet 0 à toute la primitive rend
//     OPAQUES des traînées qui doivent s'estomper (dégradé d'alpha).
//  3. LE BLEND EST PAR PRIMITIVE, et il est dans l'enregistrement (+0x18/+0x1c) — PAS dans les
//     flags. Un blend global ne peut pas marcher : un même effet mélange additif et alpha dans la
//     même frame (en alpha, les primitives additives sortent en carrés noirs ; en additif global,
//     les primitives alpha crament en blanc).
//
// ⚠ Les FLAGS (`Prim::flags`) sont une TOUTE AUTRE chose que le blend : ils ne codent que le bucket
// de tri. Le bit 0x8 = « rendu AVANT le personnage » (donc derrière lui) — c'est ce qui permet au
// doll de dessiner l'effet en deux passes autour du sprite.

#include "imgui.h"

namespace ez_capture {

// Une primitive capturée, en coordonnées ÉCRAN absolues (XYZRHW : le natif a déjà projeté).
struct Prim {
  void*    tex;          // handle GPU natif (== ImTextureID)
  int      n;            // NOMBRE DE SOMMETS RÉEL : 3 (TRIANGLELIST) ou 4 (TRIANGLESTRIP)
  float    x[4], y[4];
  float    u[4], v[4];
  unsigned argb[4];      // D3DCOLOR natif (0xAARRGGBB) — une couleur PAR SOMMET
  unsigned flags;        // bucket de tri (bit 0x8 = derrière le perso). PAS le blend.
  int      src_blend;    // D3DRS_SRCBLEND  du record (D3DBLEND brut)
  int      dst_blend;    // D3DRS_DESTBLEND du record
  // Id d'effet CONCRET de la primitive. Deux valeurs particulières :
  //  -1 = famille CEffectMgr (cf. effmgr_id) ;
  //  -2 = famille « hôte particule » (kStrParticleId) : un CEZ2STREffect qui délègue son rendu à un
  //       CEZ2STRParticle. Aucun de nos hooks d'appartenance n'est sur ce chemin, donc l'id n'y est
  //       pas accessible — ces primitives sont reconnues à la PROVENANCE de l'appel et incluses
  //       explicitement via DrawOpts::include_str_particle.
  int      effect_id;
  // Id de l'INSTANCE CEffectMgr qui a émis la primitive (-1 sur le chemin EZ).
  // ⚠ ESPACE D'IDS DIFFÉRENT de `effect_id`, et ils se CHEVAUCHENT — ne jamais les comparer entre
  // eux. Ici : < 0x98a = effet générique (skill/statut) ; >= 0x98a = hat effect, valeur =
  // `ordinal + 0x98a`. C'est ce qui permet de reconnaître un costume précis sur ce chemin, alors
  // que son id concret n'y est jamais stocké (il ne se résout que via Lua).
  int      effmgr_id;
  // Position MONDE du nœud qui a émis cette primitive = son ancre.
  // ⚠ Elle est portée PAR PRIMITIVE et non globalement : la capture contient plusieurs effets, et
  // une ancre unique « celle du premier capturé de la frame » désignait un effet arbitraire — donc
  // une ancre fausse pour tous les autres, et un rendu projeté hors écran.
  float    world[3];
};

// Installe les hooks (idempotent). Les hooks JMP se CHAÎNENT : ce module peut coexister avec
// d'autres hooks déjà posés sur les mêmes adresses.
void EnsureInstalled();

// ── Ciblage : quelles primitives nous appartiennent ? ────────────────────────
// ⚠⚠ IL N'Y A VOLONTAIREMENT AUCUN RÉGLAGE PARTAGÉ DE CAPTURE — ni acteur, ni ciblage, ni opt-in.
// Le module résout LUI-MÊME l'acteur joueur et capture tout ce qui lui appartient (chemins EZ et
// CEffectMgr). C'est délibéré : chaque réglage partagé est un état à plusieurs écrivains, et on en a
// payé deux — un ciblage global qui avalait le rendu de tous les effets du joueur (icônes de statut
// disparues), puis un « acteur courant » que le doll armait et que le lab effaçait à chaque frame,
// d'où une capture qui ne tenait qu'une ou deux frames sur deux.
// Le module capture TOUT ce qui appartient à l'acteur ciblé, en étiquetant chaque primitive avec son
// `effect_id`, et chaque consommateur FILTRE au moment de dessiner (DrawOpts::ids). C'est ce qui
// permet au lab, au doll et à la fiche perso de coexister : un ciblage global serait un état partagé
// que le dernier appelant écraserait — et, combiné à la suppression in-world, ça avalait le rendu de
// tous les effets du joueur (la barre d'icônes de statut disparaissait).

// La famille CEffectMgr (auras, statuts — ex. Perm_Frost) est capturée elle aussi, systématiquement.
// Ces effets ne passent pas par EzEffect_Draw mais par un dessin par-effet en phase render, et leur
// appartenance se juge sur le HANDLE du joueur. Leur id n'est pas résolu sur ce chemin : leurs
// primitives portent effect_id = -1, et un consommateur les inclut via DrawOpts::include_effmgr.
// (Les `.str` name-based en sont exclus : ils relèvent du pipeline billboard STR.)

// ── Suppression in-world (nominative, par id concret) ────────────────────────
// ── Interrupteur de DIAGNOSTIC ───────────────────────────────────────────────
// Neutralise le hook du chemin CEffectMgr (il n'arme plus l'appartenance, donc plus aucune capture
// ni suppression par cette voie). Sert UNIQUEMENT à bissecter une régression du rendu natif en un
// seul build, sans avoir à recompiler deux fois.
// ⚠ C'est délibérément le SEUL réglage partagé restant en dehors de la suppression : il doit n'avoir
// QU'UN écrivain (la case à cocher de diagnostic du lab). Ne l'utilise pas comme réglage fonctionnel.
void SetEffMgrCaptureEnabled(bool enable);   // défaut : true

// Second interrupteur de DIAGNOSTIC : lever l'exclusion des instances `CEZ2STREffect` (famille des
// effets `.str` name-based), normalement écartées parce qu'elles relèvent d'un autre pipeline.
// Sert à vérifier si un effet qui rend en jeu SANS être capturé passe en réalité par cette famille.
// ⚠ Activé, on risque un double dessin avec le pipeline `.str` du consommateur : diagnostic seulement.
void SetCaptureStrEffects(bool enable);      // défaut : false

// Empêche certains effets de se dessiner sur le personnage en jeu (le hook ne chaîne pas pour leurs
// primitives) : c'est ce qui permet d'afficher un effet UNIQUEMENT dans un overlay ou sur un doll.
// count == 0 ne supprime RIEN (et surtout pas « tout »).
// ⚠ CHAQUE CONSOMMATEUR A SON PROPRE EMPLACEMENT et la suppression est l'UNION des demandes : sans
// ça, deux consommateurs s'écraseraient mutuellement — le défaut qui a déjà coûté deux bugs ici.
// ⚠ Les primitives de la famille CEffectMgr portent effect_id = -1 : elles ne sont pas supprimables
// par id (un effet d'aperçu de cette famille resterait visible sur le personnage).
enum SuppressSlot { kSlotLab = 0, kSlotDoll = 1, kSlotSheet = 2, kSuppressSlotCount = 4 };
void SetSuppressedIds(int slot, const int* ids, int count);

// ── Résultat de la frame courante ────────────────────────────────────────────
const Prim* Prims();
int         Count();

// Ancre ÉCRAN + échelle de profondeur de L'EFFET QUE TU DESSINES, par projection de la position
// monde de son nœud. On passe les MÊMES options qu'à Draw : l'ancre est celle de la première
// primitive qui passe ton filtre — sinon on projetterait la position d'un autre effet du joueur.
// `screen_scale` (peut être nullptr) sert aux consommateurs qui redimensionnent (doll) : le ratio
// R = taille_voulue / screen_scale convertit 1 unité monde en pixels de destination.
// Renvoie false si rien ne correspond au filtre ou si la projection native a échoué.
struct DrawOpts;
bool ProjectAnchor(const DrawOpts& opts, float* ax, float* ay, float* screen_scale);

// ── Dessin ré-ancré ──────────────────────────────────────────────────────────
// out = (ox, oy) + (v - ancre) * scale. scale = 1 conserve la taille native.
struct DrawOpts {
  float ox = 0.0f, oy = 0.0f;   // point de destination (centre écran, ou origine du doll)
  float scale = 1.0f;           // 1 = taille native ; doll = (s / screen_scale) * calibrage
  int   blend_mode = 0;         // 0 = natif par primitive (correct) | 1 = alpha | 2 = additif global
  bool  use_zorder = false;     // true = ne dessiner qu'une phase (cf. draw_behind)
  bool  draw_behind = false;    // phase à dessiner : true = primitives à bit 0x8 (derrière le perso)
  float max_r = 0.0f;           // garde-fou : rejette un sommet à > max_r px de l'ancre (0 = désactivé)
  // FILTRE PAR EFFET : n'utiliser que les primitives dont l'effect_id est dans cette liste.
  // ⚠ nullptr / count == 0 => RIEN (et surtout pas « tout »). La capture couvre bien plus que ce que
  // tu veux montrer — jusqu'aux effets d'ambiance de la map : un consommateur sans id à demander doit
  // ne rien dessiner. Pour une famille entière, dis-le explicitement (include_effmgr /
  // include_str_particle). C'est ICI que chaque consommateur choisit ce qui le concerne, plutôt que
  // dans un état global partagé.
  const int* ids = nullptr;
  int        id_count = 0;
  // Primitives de la famille CEffectMgr (effect_id == -1). Deux façons de les traiter :
  //  - `effmgr_ids` non vide  -> on ne garde que celles dont `effmgr_id` y figure (filtrage PRÉCIS,
  //    c'est ce qui permet de distinguer un costume ÉQUIPÉ d'un costume seulement SURVOLÉ) ;
  //  - sinon `include_effmgr` -> tout ou rien (repli, quand on n'a pas d'id à fournir).
  // ⚠ `effmgr_ids` est dans l'espace des ids d'INSTANCE : pour un hat effect, `ordinal + 0x98a`.
  const int* effmgr_ids = nullptr;
  int        effmgr_id_count = 0;
  bool       include_effmgr = false;
  // Inclure la famille « hôte particule » (effect_id == kStrParticleId). Elle est ANONYME : on ne
  // peut pas la filtrer par effet. À n'activer que sur une surface qui montre UN effet à la fois —
  // typiquement l'aperçu — jamais sur le doll, qui ne saurait pas distinguer le porté du survolé.
  bool       include_str_particle = false;
};
// Dessine dans `dl`. Ne fait rien si la capture est vide ou si la projection échoue.
void Draw(ImDrawList* dl, const DrawOpts& opts);

// Cette primitive passe-t-elle le filtre de ces options ? (Même règle que Draw et ProjectAnchor.)
// À utiliser par tout consommateur qui MESURE la capture — bbox, cadrage, mise à l'échelle : mesurer
// sans filtrer ferait entrer dans le calcul des effets qu'on ne dessine pas.
bool Matches(const Prim& prim, const DrawOpts& opts);

// ── Sonde « pourquoi rien ne s'affiche ? » ───────────────────────────────────
// Sépare trois causes qu'on confond facilement :
//   draws == 0             -> notre nœud n'est jamais dessiné : il n'existe pas, ou il est INERTE
//                             (id sans entrée dans le dispatcher) -> le problème est en AMONT.
//   draws > 0, inserts == 0 -> le nœud vit et est dessiné, mais son sous-rendu n'émet aucune
//                             primitive (ressource, condition interne).
//   inserts > 0, captured == 0 -> le jeu émet et c'est NOUS qui rejetons -> bug de capture.
// `all_draws` (tous les nœuds EZ de la scène) sert de référence.
struct Stats {
  int draws, all_draws, inserts, captured;
  // Dernière primitive REJETÉE par le filtre de forme (0/0/0 si aucun rejet). Indispensable : quand
  // « inserts > 0, captured == 0 », c'est NOUS qui jetons, et ces trois valeurs disent pourquoi.
  int rej_type;   // D3DPRIMITIVETYPE (4 = TRIANGLELIST, 5 = TRIANGLESTRIP)
  int rej_vtx;    // nombre de sommets annoncé
  int rej_idx;    // nombre d'indices (non nul = primitive indexée, non gérée)
  int rej_count;  // combien de rejets cette frame
};
// Bilan de la dernière frame complète. ⚠ Le basculement se fait sur la frontière de frame de la file
// de rendu : un léger décalage entre compteurs est possible, c'est un diagnostic, pas une métrique.
Stats LastFrameStats();

// ── Qui soumet les primitives ? ──────────────────────────────────────────────
// Certaines familles d'effets ne dessinent pas elles-mêmes : elles délèguent à un objet hôte, dessiné
// par un sous-système que nos hooks d'appartenance ne couvrent pas. On ne peut alors PAS savoir à qui
// appartiennent leurs quads. Ce relevé donne les adresses de retour des appelants du puits de
// primitives, avec leur nombre d'appels : en comparant effet actif / éteint, on identifie la fonction
// à hooker. Diagnostic — à lire dans le lab, pas à utiliser en logique.
// Marqueur d'effect_id pour la famille « hôte particule » (cf. Prim::effect_id).
constexpr int kStrParticleId = -2;

struct Caller { uintptr_t addr; int count; };
int  CallerCount();                 // nombre d'appelants distincts vus la frame précédente
const Caller* Callers();            // tableau, trié par nombre d'appels décroissant

// ── Utilitaire : cet effet est-il seulement IMPLÉMENTÉ dans ce client ? ──────
// Beaucoup d'ids ne dessinent RIEN, même nativement et SANS erreur : le dispatcher les route vers
// un DEFAULT `mov al,1 ; ret`. Le nœud est bien créé et tické — il est juste muet. On lit la table
// de saut DU CLIENT (vérité terrain ; aucune liste recopiée, donc valable après un update).
// ⚠ Un ordinal peut aussi rendre via un `.str` (resourceFileName) : ce test ne couvre QUE la voie
// procédurale. Un false ne signifie donc pas « ne rend rien » pour un ordinal porteur d'un .str.
bool EffectIdIsImplemented(int concrete_id);

}  // namespace ez_capture
