#pragma once

#include <cstddef>
#include <string>

#include "yaml-cpp/yaml.h"

// ── settings_table : décrire un réglage UNE fois, le lire et l'écrire ────────
// En-tête PRIVÉ au dossier src/plugins/moonlight_ui/ (cf. internal.h).
//
// LoadSettings et WriteSettingsFile étaient deux miroirs manuels de ~190 clés,
// maintenus à la main dans le même ordre, sans rien pour garantir qu'ils restent
// d'accord. C'est ce mécanisme qui a produit skillbar_locked (lu, jamais écrit),
// la divergence itemdesc_anchor (défaut 3 d'un côté, 0 de l'autre) et les 66
// replis ternaires littéraux. Un réglage décrit ici l'est UNE fois : clé, type,
// où vit la valeur, où vit son défaut.
//
// Le champ vivant appartient souvent à un plugin qui peut ne pas être
// enregistré. D'où DEUX résolveurs plutôt qu'un pointeur :
//   • `live` rend nullptr si le plugin est absent — la lecture ne fait rien ;
//   • `def` rend l'adresse du MÊME champ dans une instance par défaut, donc
//     jamais nullptr — l'écriture a toujours quelque chose à émettre.
// C'est ce qui répare la perte silencieuse : les anciens blocs `if (eb)`,
// `if (mi)`, `if (sb)` faisaient DISPARAÎTRE 151 clés du fichier quand le plugin
// manquait, alors que le bloc voisin, lui, émettait un repli.
//
// Les résolveurs sont des lambdas SANS capture, donc convertibles en pointeurs
// de fonction : les tables restent des agrégats constants, en .rdata.

namespace moonlight_ui {

enum class SettingType {
  kBool,
  kInt,
  kUInt,
  kFloat,
  kString,
  kColorHex,   // float[4] (picker) <-> chaîne hex ARGB « AARRGGBB »
  kColorU32,   // float[4] (picker) <-> entier décimal ImU32 (héritage, cf. plus bas)
};

using FieldResolver = void* (*)();          // champ vivant, nullptr si plugin absent
using DefaultResolver = const void* (*)();  // même champ dans une instance par défaut

struct SettingDesc {
  const char* key;
  SettingType type;
  FieldResolver live;
  DefaultResolver def;
};

// `ui` = nœud « moonlight_ui » du document. Une clé absente pose le défaut, une
// valeur illisible aussi (yaml-cpp le fait déjà via as<T>(fallback)) — jamais
// d'exception qui ferait perdre le reste du fichier.
void ReadSettings(const YAML::Node& ui, const SettingDesc* table, size_t count);
void WriteSettings(YAML::Emitter& out, const SettingDesc* table, size_t count);

template <size_t N>
inline void ReadSettings(const YAML::Node& ui, const SettingDesc (&table)[N]) {
  ReadSettings(ui, table, N);
}
template <size_t N>
inline void WriteSettings(YAML::Emitter& out, const SettingDesc (&table)[N]) {
  WriteSettings(out, table, N);
}

// ── Couleurs persistées ──────────────────────────────────────────────────────
// Deux formats coexistent sur disque, hérités et désormais FIGÉS — les unifier
// invaliderait les bourgeon_settings.yaml déjà chez les joueurs :
//   • chaîne hex ARGB « AARRGGBB » — chat_bg, dps, expbar, portrait, grid,
//     skillbar, ground_paint, presets de chat ;
//   • entier décimal ImU32        — statusicon_*, ro_skin_* et presets de skin.
// Ce fichier est le seul endroit du projet qui connaît cette dualité ; ailleurs
// on nomme l'encodage (cf. ui/color_codec.h).

// Laisse `picker_rgba` INTACT si la clé est absente, vide ou corrompue : la
// valeur par défaut du plugin survit. Avant, un std::stoul lançait sur une clé
// corrompue et l'exception remontait jusqu'au catch qui enveloppe TOUT
// LoadSettings — une seule couleur illisible faisait perdre le reste de la
// configuration. Rend true si la couleur a été appliquée.
bool ReadArgbKey(const YAML::Node& ui, const std::string& key, float picker_rgba[4]);
std::string HexArgb(const float picker_rgba[4]);
void WriteArgbKey(YAML::Emitter& out, const std::string& key, const float picker_rgba[4]);

}  // namespace moonlight_ui

// Résolveurs de champ, à utiliser dans les tables. `getter` est un accesseur de
// Bourgeon (dps_meter, basic_info…), `expr` le chemin du champ depuis le plugin
// (« locked_ », « config().corner », « fx().gamma »).
#define MLUI_FIELD(getter, expr)                                  \
  []() -> void* {                                                 \
    auto* owner = Bourgeon::Instance().getter();                  \
    return owner ? &(owner->expr) : nullptr;                      \
  }

// Défaut d'un champ de structure de configuration : l'adresse du même champ dans
// une instance construite par défaut. Le défaut reste donc écrit UNE fois, dans
// le header du plugin, jamais recopié en littéral ici.
#define MLUI_DEFAULT(Type, member)             \
  []() -> const void* {                        \
    static const Type defaults{};              \
    return &defaults.member;                   \
  }

// Défaut littéral, pour les plugins dont le champ n'est atteignable que par un
// accesseur (membre privé) : on ne peut pas y lire l'initialisateur du header.
// Ce littéral devient alors LA source unique du défaut — lecture ET repli
// d'écriture le prennent ici, au lieu des deux littéraux jumeaux d'avant.
#define MLUI_LITERAL(Type, value)              \
  []() -> const void* {                        \
    static const Type fallback = value;        \
    return &fallback;                          \
  }
