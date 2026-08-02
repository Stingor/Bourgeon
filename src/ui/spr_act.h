#pragma once

// ── Lecture et interprétation des .spr / .act, chez nous ─────────────────────
//
// Port direct de l'implémentation de référence de GRF Editor
// (`GRF/FileFormats/SprFormat/` et `GRF/FileFormats/ActFormat/`, dépôt local —
// cf. [[reference_grf_act_spr_reference_impl]]). Ce module ne connaît QUE les
// formats de fichier : aucune structure interne du client n'y apparaît.
//
// ── Pourquoi ce module existe ────────────────────────────────────────────────
// La fiche de monstre passait par les objets du client (CSprite/CAction, atlas
// de sprites) en devinant leurs dispositions mémoire. Cinq défauts d'affilée en
// sont sortis — ancrage, mise à l'échelle de l'offset, boîte englobante, atlas
// indexé-8 uniquement, disposition des cellules RGBA — et AUCUN ne venait du
// format, tous de l'interface. Le format, lui, est sans ambiguïté.
//
// Ce que ça supprime : l'atlas et sa limite indexé-8, la structure de cellule
// devinée, le pas de calque 0x24, les offsets spr+0x510/+0x51C, Act_GetFrame*
// — tout ce qui casserait au prochain Ragexe.
//
// ── Le seul contact avec le natif ────────────────────────────────────────────
// `ReadFile` appelle `FileMgr_LoadToMemory`, le VFS du client. Il est
// INDISPENSABLE : `data.grf` de Moonlight est un GRF CHIFFRÉ (signature
// « Event Horizon »), qu'aucun lecteur zlib maison n'ouvre. Il donne en prime
// gratuitement la règle de priorité du client (dossier `data\` avant les GRF),
// donc un override posé par un patcheur est vu comme le client le voit.
// L'inflate zlib pour les .spr v3.2 passe aussi par le natif, faute de zlib
// dans le projet — c'est une fonction standard, pas une structure interne.

#include <cstdint>
#include <vector>

namespace ro {
namespace spract {

// Une image du .spr, déjà convertie en ARGB prête à téléverser
// (`Overlay_CreateTextureARGB`). Ligne 0 = HAUT de l'image.
struct Image {
  int w = 0;
  int h = 0;
  std::vector<uint32_t> argb;  // 0xAARRGGBB, w*h entrées
};

// Un calque d'une image d'animation. Champs bruts du .act, sans interprétation
// géométrique : c'est l'appelant qui compose (cf. ui/mob_sprite.cc).
struct Layer {
  float    off_x = 0.0f;
  float    off_y = 0.0f;
  int      index = -1;   // n° d'image DANS SA SECTION (relatif), -1 = calque vide
  int      type  = 0;    // 0 = section Indexed8, 1 = section Bgra32
  bool     mirror = false;
  uint32_t tint = 0xFFFFFFFFu;  // 0xAARRGGBB
  float    scale_x = 1.0f;
  float    scale_y = 1.0f;
  int      rotation = 0;  // degrés, sens horaire
  int      w = 0;         // dimensions de l'image résolue (0 si non résolue)
  int      h = 0;
};

struct Frame {
  std::vector<Layer> layers;
};

struct Action {
  std::vector<Frame> frames;
  // Cadence DÉCLARÉE de l'action, en « ticks » RO. L'intervalle par image vaut
  // `speed * 25` millisecondes — même conversion que `Action.AnimationSpeed`
  // de GRF Editor. Défaut 6 (= 150 ms), la valeur par défaut de la référence.
  float speed = 6.0f;
};

// Une paire .spr/.act chargée.
struct Resource {
  bool ok = false;
  int  spr_version = 0;  // major*10 + minor (21 = 2.1, 32 = 3.2)
  int  act_version = 0;
  // Les deux SECTIONS du .spr, dans l'ordre du fichier. Un calque indexe la
  // sienne, pas la concaténation : `type` choisit la section, `index` la place.
  std::vector<Image>  indexed;
  std::vector<Image>  bgra;
  std::vector<Action> actions;

  // L'image d'un calque, ou nullptr si l'index sort de sa section.
  const Image* Get(int index, int type) const;
};

// Octets bruts d'un fichier du VFS du client. `data_relative_path` est le
// chemin TEL QU'IL EST DANS LE GRF, préfixe `data\` compris, en CP949
// (ex. "data\\\xb8\xf3\xbd\xba\xc5\xcd\\Chocho.spr"). Rend false si absent.
bool ReadFile(const char* data_relative_path, std::vector<uint8_t>* out);

// Charge une paire. Les deux chemins sont ceux du VFS (préfixe `data\` compris,
// CP949). Rend false si l'un des deux manque ou n'est pas lisible ; `out` est
// alors laissé dans un état inutilisable (`ok == false`).
//
// 🔴 Les deux chemins plutôt qu'une base commune : c'est l'appelant qui les
// forme, et il les forme à partir des gabarits CP949 du client — nos sources
// sont en UTF-8 et ne peuvent pas porter de littéral coréen.
bool Load(const char* spr_path, const char* act_path, Resource* out);

// ── Parseurs, exposés pour les tests et pour les appelants qui ont déjà les
// octets en main (extraction, prévisualisation d'un fichier posé à la main).
bool ParseSpr(const uint8_t* data, size_t size, Resource* out);
bool ParseAct(const uint8_t* data, size_t size, Resource* out);

}  // namespace spract
}  // namespace ro
