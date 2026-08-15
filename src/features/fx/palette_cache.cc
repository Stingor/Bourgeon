#include "features/fx/palette_cache.h"

#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <cstdlib>  // atoi
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "features/fx/style_sync.h"  // kWireVersion / kRampBytes
#include "utils/game_paths.h"
#include "utils/log_console.h"

namespace fx {
namespace palette_cache {
namespace {

// Même forme que côté serveur : "<version>:<hex>". Partager la convention n'est
// pas de la coquetterie — c'est ce qui garantit qu'un changement de format
// invalide les DEUX stockages de la même façon, au lieu d'en laisser un servir
// des recettes mal alignées.
constexpr int kHexChars = ro::kMaxRamps * fx::style_sync::kRampBytes * 2;

// char_id -> ses variantes encodées. 🔴 La PREMIÈRE est celle du repli : le
// fichier garde ainsi l'information sans avoir à la répéter, et un YAML relu à
// la main reste lisible.
std::map<uint32_t, std::vector<std::string>> g_entries;
bool g_loaded = false;
uint32_t g_generation = 0;

char ToHex(int v) { return static_cast<char>(v < 10 ? '0' + v : 'a' + v - 10); }

int FromHex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string Encode(const ro::PaletteRecipe& recipe, uint32_t body_key) {
  std::string out;
  out.reserve(24 + kHexChars);
  char head[64];
  // "<version>:<corps>:<palette>:<cheveux>:<coiffure>:" — en clair, pour rester
  // lisible en base de données comme dans le presse-papiers. MÊME forme que la
  // chaîne du serveur : les deux stockages s'invalident donc ensemble.
  //
  // Le corps est en HEXADÉCIMAL sur 8 chiffres, à la différence des autres
  // champs : c'est un condensé, pas un nombre qu'on lit ou qu'on compare à
  // l'œil, et la largeur fixe le rend repérable d'un coup dans une valeur SQL.
  std::snprintf(head, sizeof(head), "%d:%08x:%d:%d:%d:",
                fx::style_sync::kWireVersion, body_key,
                static_cast<int>(recipe.palette_id),
                static_cast<int>(recipe.hair_palette_id),
                static_cast<int>(recipe.hair_style));
  out = head;
  for (int i = 0; i < ro::kMaxRamps; ++i) {
    const ro::RampAdjust& a = recipe.ramps[i];
    // Champ par champ, comme sur le réseau : la struct porte du bourrage.
    const uint8_t bytes[fx::style_sync::kRampBytes] = {
        static_cast<uint8_t>(a.hue & 0xFF),
        static_cast<uint8_t>((a.hue >> 8) & 0xFF),
        static_cast<uint8_t>(a.sat),
        static_cast<uint8_t>(a.val),
        a.absolute,
    };
    for (uint8_t b : bytes) {
      out.push_back(ToHex((b >> 4) & 0xF));
      out.push_back(ToHex(b & 0xF));
    }
  }
  return out;
}

bool Decode(const std::string& s, ro::PaletteRecipe* out,
            uint32_t* out_body_key = nullptr) {
  if (out_body_key) *out_body_key = 0;
  // 🔴 La VERSION d'abord, et une SEULE est acceptée : la courante.
  //
  // Des branches de migration ont existé (v2→v3→v4→v5) tant que les versions
  // n'ajoutaient que des CHAMPS — les réglages de rampes, eux, gardaient leur
  // sens. La v6 est d'une autre nature : elle change le CLASSEMENT des rampes,
  // donc le rang 3 d'hier ne désigne plus la même pièce du costume. Migrer
  // reviendrait à repeindre les bottes en couleur de cape, avec l'aplomb du
  // format valide. Une entrée d'une autre version est donc IGNORÉE, et le joueur
  // retrouve son apparence native — un cache se refait, une confiance non.
  char head[8];
  const int n =
      std::snprintf(head, sizeof(head), "%d:", fx::style_sync::kWireVersion);
  if (s.size() < static_cast<size_t>(n) || s.compare(0, n, head) != 0)
    return false;
  // Le champ de CORPS, ajouté en v7 : huit chiffres hexadécimaux entre la
  // version et la palette.
  const size_t sep0 = s.find(':', n);
  if (sep0 == std::string::npos || sep0 - n != 8) return false;
  const size_t sep1 = s.find(':', sep0 + 1);
  if (sep1 == std::string::npos) return false;
  const size_t sep2 = s.find(':', sep1 + 1);
  if (sep2 == std::string::npos) return false;
  const size_t sep3 = s.find(':', sep2 + 1);
  if (sep3 == std::string::npos) return false;
  const size_t plen = sep3 + 1;
  if (s.size() != plen + kHexChars) return false;

  if (out_body_key) {
    uint32_t key = 0;
    for (size_t i = n; i < sep0; ++i) {
      const int v = FromHex(s[i]);
      if (v < 0) return false;
      key = (key << 4) | static_cast<uint32_t>(v);
    }
    *out_body_key = key;
  }
  out->palette_id = static_cast<int16_t>(std::atoi(s.c_str() + sep0 + 1));
  out->hair_palette_id = static_cast<int16_t>(std::atoi(s.c_str() + sep1 + 1));
  out->hair_style = static_cast<int16_t>(std::atoi(s.c_str() + sep2 + 1));

  uint8_t bytes[ro::kMaxRamps * fx::style_sync::kRampBytes];
  for (int i = 0; i < kHexChars; i += 2) {
    const int hi = FromHex(s[plen + i]);
    const int lo = FromHex(s[plen + i + 1]);
    if (hi < 0 || lo < 0) return false;
    bytes[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
  }
  for (int i = 0; i < ro::kMaxRamps; ++i) {
    const uint8_t* p = bytes + i * fx::style_sync::kRampBytes;
    ro::RampAdjust& a = out->ramps[i];
    a.hue = static_cast<int16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
    a.sat = static_cast<int8_t>(p[2]);
    a.val = static_cast<int8_t>(p[3]);
    a.absolute = p[4] ? 1 : 0;
  }
  return true;
}

void EnsureLoaded() {
  if (g_loaded) return;
  g_loaded = true;  // même en cas d'échec : ne pas relire le disque à chaque appel
  const std::string path = paths::PaletteCachePath();
  try {
    const YAML::Node root = YAML::LoadFile(path);
    if (!root || !root.IsMap()) return;
    for (const auto& kv : root) {
      const uint32_t id = kv.first.as<uint32_t>(0u);
      if (id == 0) continue;
      std::vector<std::string> vals;
      if (kv.second.IsSequence()) {
        for (const YAML::Node& n : kv.second) {
          const std::string v = n.as<std::string>("");
          if (!v.empty()) vals.push_back(v);
        }
      } else {
        // Forme d'avant la v7 : une seule recette par personnage, en scalaire.
        // On la lit sans effort particulier — `Decode` la refusera sur sa
        // version, ce qui est exactement le traitement voulu.
        const std::string v = kv.second.as<std::string>("");
        if (!v.empty()) vals.push_back(v);
      }
      if (!vals.empty()) g_entries[id] = std::move(vals);
    }
  } catch (const std::exception&) {
    // Fichier absent au premier lancement : c'est le cas NOMINAL, pas une
    // erreur. Un fichier corrompu se traite pareil — un cache se reconstruit.
  }
}

void Flush() {
  const std::string path = paths::PaletteCachePath();
  YAML::Node root(YAML::NodeType::Map);
  for (const auto& kv : g_entries) {
    YAML::Node seq(YAML::NodeType::Sequence);
    for (const std::string& v : kv.second) seq.push_back(v);
    root[kv.first] = seq;
  }
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    LogError("[palette] cache non écrit : {}", path);
    return;
  }
  f << root;
  f << "\n";
}

}  // namespace

void SaveAll(uint32_t char_id,
             const std::map<uint32_t, ro::PaletteRecipe>& variants,
             uint32_t default_key) {
  if (char_id == 0) return;
  EnsureLoaded();
  if (variants.empty()) {
    if (g_entries.erase(char_id) == 0) return;  // rien à effacer
    ++g_generation;
    Flush();
    return;
  }

  std::vector<std::string> encoded;
  encoded.reserve(variants.size());
  // Le repli EN TÊTE : c'est la convention du fichier, et celle que `LoadAll`
  // relit. La ranger ailleurs demanderait un second champ pour la désigner.
  auto repli = variants.find(default_key);
  if (repli != variants.end()) encoded.push_back(Encode(repli->second, repli->first));
  for (const auto& kv : variants) {
    if (repli != variants.end() && kv.first == repli->first) continue;
    encoded.push_back(Encode(kv.second, kv.first));
  }

  auto it = g_entries.find(char_id);
  if (it != g_entries.end() && it->second == encoded) return;  // rien de neuf
  g_entries[char_id] = std::move(encoded);
  ++g_generation;
  Flush();
}

uint32_t Generation() { return g_generation; }

bool LoadAll(uint32_t char_id, std::map<uint32_t, ro::PaletteRecipe>* out,
             uint32_t* out_default_key) {
  if (char_id == 0 || !out) return false;
  out->clear();
  if (out_default_key) *out_default_key = 0;
  EnsureLoaded();
  auto it = g_entries.find(char_id);
  if (it == g_entries.end()) return false;
  for (const std::string& s : it->second) {
    ro::PaletteRecipe recipe;
    uint32_t key = 0;
    // Version périmée ou entrée abîmée : ignorée en silence, comme toujours ici.
    // Un cache se refait ; une recette mal alignée, elle, se voit.
    if (!Decode(s, &recipe, &key) || key == 0) continue;
    if (out->empty() && out_default_key) *out_default_key = key;  // la 1ʳᵉ = le repli
    (*out)[key] = recipe;
  }
  return !out->empty();
}

bool Load(uint32_t char_id, uint32_t body_key, ro::PaletteRecipe* out) {
  if (!out) return false;
  std::map<uint32_t, ro::PaletteRecipe> variants;
  uint32_t defaut = 0;
  if (!LoadAll(char_id, &variants, &defaut)) return false;
  if (body_key != 0) {
    auto exact = variants.find(body_key);
    if (exact != variants.end()) {
      *out = exact->second;
      return true;
    }
  }
  auto repli = variants.find(defaut);
  if (repli == variants.end()) repli = variants.begin();
  *out = repli->second;
  return true;
}

// ── Préréglages ─────────────────────────────────────────────────────────────
// Fichier SÉPARÉ du cache : ce sont deux natures de donnée. Le cache reflète le
// serveur et se reconstruit tout seul ; un préréglage est un choix du joueur que
// rien ne recréerait s'il disparaissait.
namespace {

std::vector<std::pair<std::string, std::string>> g_presets;  // nom -> encodé
bool g_presets_loaded = false;

std::string PresetsPath() {
  std::string p = paths::PaletteCachePath();
  const size_t dot = p.rfind(".yaml");
  if (dot != std::string::npos) p.replace(dot, 5, "_presets.yaml");
  return p;
}

void EnsurePresets() {
  if (g_presets_loaded) return;
  g_presets_loaded = true;
  try {
    const YAML::Node root = YAML::LoadFile(PresetsPath());
    if (!root || !root.IsSequence()) return;
    for (const YAML::Node& n : root) {
      const std::string nom = n["nom"].as<std::string>("");
      const std::string val = n["recette"].as<std::string>("");
      if (!nom.empty() && !val.empty() &&
          static_cast<int>(g_presets.size()) < kMaxPresets)
        g_presets.emplace_back(nom, val);
    }
  } catch (const std::exception&) {
    // Absent au premier lancement : cas nominal.
  }
}

void FlushPresets() {
  // 🔴 Une SÉQUENCE, pas une map : l'ordre d'ajout est ce que le joueur voit
  // dans la liste, et une map le trierait alphabétiquement dans son dos.
  YAML::Node root(YAML::NodeType::Sequence);
  for (const auto& kv : g_presets) {
    YAML::Node n(YAML::NodeType::Map);
    n["nom"] = kv.first;
    n["recette"] = kv.second;
    root.push_back(n);
  }
  std::ofstream f(PresetsPath(), std::ios::binary | std::ios::trunc);
  if (!f) {
    LogError("[palette] préréglages non écrits : {}", PresetsPath());
    return;
  }
  f << root << "\n";
}

}  // namespace

int PresetNames(std::string* out, int max) {
  if (!out || max <= 0) return 0;
  EnsurePresets();
  int n = 0;
  for (const auto& kv : g_presets) {
    if (n >= max) break;
    out[n++] = kv.first;
  }
  return n;
}

// Espaces de bord retirés. 🔴 Sans ça, « Rouge » et « Rouge » (avec l'espace
// qu'on laisse en tapant) sont DEUX préréglages : l'enregistrement croit créer,
// le joueur croit mettre à jour, et la liste se peuple de jumeaux.
std::string NomPropre(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return std::string();
  const size_t b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

bool PresetSave(const std::string& raw_name, const ro::PaletteRecipe& recipe) {
  const std::string name = NomPropre(raw_name);
  if (name.empty()) return false;
  EnsurePresets();
  // La coiffure est DANS la recette : un préréglage garde donc l'allure
  // complète, sans traitement particulier.
  // 🔴 Clé de corps NULLE, et ce n'est pas un oubli : un préréglage est fait pour
  // être reposé sur n'importe quel corps — c'est même son seul intérêt. Y graver
  // celui de sa naissance laisserait croire qu'il lui est réservé.
  const std::string encoded = Encode(recipe, /*body_key=*/0);
  for (auto& kv : g_presets) {
    if (kv.first == name) {  // homonyme : on remplace, sans doublon dans la liste
      kv.second = encoded;
      FlushPresets();
      LogDebug("[palette] préréglage '{}' mis à jour", name);
      return true;
    }
  }
  if (static_cast<int>(g_presets.size()) >= kMaxPresets) {
    LogDiag("[palette] préréglage '{}' refusé : plafond de {} atteint", name,
            kMaxPresets);
    return false;
  }
  g_presets.emplace_back(name, encoded);
  FlushPresets();
  LogDebug("[palette] préréglage '{}' créé ({} au total)", name,
          g_presets.size());
  return true;
}

bool PresetLoad(const std::string& raw_name, ro::PaletteRecipe* out) {
  if (!out) return false;
  const std::string name = NomPropre(raw_name);
  EnsurePresets();
  for (const auto& kv : g_presets)
    if (kv.first == name) return Decode(kv.second, out);
  return false;
}

void PresetDelete(const std::string& raw_name) {
  const std::string name = NomPropre(raw_name);
  EnsurePresets();
  for (size_t i = 0; i < g_presets.size(); ++i) {
    if (g_presets[i].first == name) {
      g_presets.erase(g_presets.begin() + i);
      FlushPresets();
      return;
    }
  }
}

// ── Brouillon ───────────────────────────────────────────────────────────────
// Un troisième fichier, et pour la même raison qui en avait fait un deuxième :
// trois natures de donnée. Le cache reflète le serveur et se reconstruit tout
// seul ; un préréglage est un choix NOMMÉ que le joueur a posé ; un brouillon
// est un accident qu'on rattrape — il s'écrase sans prévenir et disparaît dès
// qu'il est validé. Les mélanger obligerait à distinguer à la lecture ce qu'on
// distingue ici gratuitement, et un effacement de brouillon toucherait un
// fichier où dorment des préréglages.
namespace {

std::map<uint32_t, std::string> g_drafts;  // char_id -> "<version>:<hex>"
bool g_drafts_loaded = false;

std::string DraftsPath() {
  std::string p = paths::PaletteCachePath();
  const size_t dot = p.rfind(".yaml");
  if (dot != std::string::npos) p.replace(dot, 5, "_draft.yaml");
  return p;
}

void EnsureDrafts() {
  if (g_drafts_loaded) return;
  g_drafts_loaded = true;
  try {
    const YAML::Node root = YAML::LoadFile(DraftsPath());
    if (!root || !root.IsMap()) return;
    for (const auto& kv : root) {
      const uint32_t id = kv.first.as<uint32_t>(0u);
      const std::string val = kv.second.as<std::string>("");
      if (id == 0 || val.empty()) continue;
      // 🔴 Filtré DÈS LA LECTURE, contrairement au cache et aux préréglages. La
      // fenêtre ne propose son bouton que si un brouillon existe : garder en
      // mémoire une entrée d'une version périmée — qui ne se décodera jamais —
      // afficherait un bouton qui ne fait rien, ce qui se lit comme une panne.
      ro::PaletteRecipe jetable;
      if (Decode(val, &jetable)) g_drafts[id] = val;
    }
  } catch (const std::exception&) {
    // Absent au premier lancement : cas nominal.
  }
}

void FlushDrafts() {
  YAML::Node root(YAML::NodeType::Map);
  for (const auto& kv : g_drafts) root[kv.first] = kv.second;
  std::ofstream f(DraftsPath(), std::ios::binary | std::ios::trunc);
  if (!f) {
    LogError("[palette] brouillon non écrit : {}", DraftsPath());
    return;
  }
  f << root << "\n";
}

}  // namespace

void DraftSave(uint32_t char_id, const ro::PaletteRecipe* recipe) {
  if (char_id == 0) return;
  EnsureDrafts();
  if (recipe) {
    // Clé nulle, comme les préréglages : un brouillon suit le joueur, pas un
    // corps. S'il en change avant de reprendre son travail, ses réglages se
    // ré-appliqueront par index — le même compromis que partout ailleurs.
    const std::string encoded = Encode(*recipe, /*body_key=*/0);
    auto it = g_drafts.find(char_id);
    if (it != g_drafts.end() && it->second == encoded) return;  // rien de neuf
    g_drafts[char_id] = encoded;
  } else if (g_drafts.erase(char_id) == 0) {
    return;  // rien à effacer : pas de réécriture inutile
  }
  FlushDrafts();
}

bool DraftLoad(uint32_t char_id, ro::PaletteRecipe* out) {
  if (char_id == 0 || !out) return false;
  EnsureDrafts();
  auto it = g_drafts.find(char_id);
  if (it == g_drafts.end()) return false;
  return Decode(it->second, out);
}

bool HasDraft(uint32_t char_id) {
  if (char_id == 0) return false;
  EnsureDrafts();
  return g_drafts.count(char_id) != 0;
}

std::string EncodeShare(const ro::PaletteRecipe& recipe) {
  // Sans clé de corps : un code se colle sur le personnage qu'on veut. C'est
  // aussi ce qui en fait un comparateur sûr pour « ce réglage a-t-il changé
  // depuis la validation ? » — deux recettes identiques donnent la même chaîne,
  // quel que soit le corps porté au moment du calcul.
  return Encode(recipe, /*body_key=*/0);
}

bool DecodeShare(const std::string& code, ro::PaletteRecipe* out) {
  if (!out) return false;
  // Tolérer les espaces autour : un code collé depuis un chat ou un forum en
  // traîne presque toujours, et refuser pour ça serait gratuitement pénible.
  const size_t a = code.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return false;
  const size_t b = code.find_last_not_of(" \t\r\n");
  return Decode(code.substr(a, b - a + 1), out);
}

const char* DollKey(uint32_t char_id, const uint8_t* rgba) {
  static char key[64];
  if (!rgba) {
    key[0] = '\0';
    return key;
  }
  // Empreinte FNV-1a des 1024 octets : c'est le CONTENU qui doit distinguer
  // deux clés, sinon le composeur ressortirait les textures d'une palette
  // précédente pour le même personnage.
  uint32_t h = 2166136261u;
  for (int i = 0; i < 1024; ++i) {
    h ^= rgba[i];
    h *= 16777619u;
  }
  std::snprintf(key, sizeof(key), "bourgeon:%u:%08x", char_id, h);
  return key;
}

}  // namespace palette_cache
}  // namespace fx
