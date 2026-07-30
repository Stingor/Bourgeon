#include "features/craft_data.h"

#include <cstdlib>
#include <string>
#include <unordered_map>

#include "utils/game_paths.h"
#include "utils/log_console.h"
#include "yaml-cpp/yaml.h"

namespace craftdata {
namespace {

constexpr int kMaxWeaponLevel = 4;
constexpr int kMaxRefine      = 10;
// Amplitude maximale d'une plage compressée. Purement défensif : une clé mal
// formée (« 1101-999999 ») ferait sinon boucler des centaines de milliers de fois
// et gonfler la table pour rien. Les vraies familles d'armes font quelques
// dizaines d'ids.
constexpr uint32_t kMaxRangeSpan = 4096;

struct Tables {
  bool ok = false;
  std::unordered_map<uint32_t, int> weapon_lv;
  // produit -> `req_skill` de sa recette. Un produit peut apparaître dans plusieurs
  // entrées de produce_db (le serveur en a de vrais doublons, cf. Steel) ; on garde la
  // PREMIÈRE, comme le fait `skill_can_produce_mix` qui re-cherche par nameid.
  std::unordered_map<uint32_t, int> recipe_skill;
  // produit -> `itemlv` de sa recette (1-3 arme, 11-20 nourriture, sinon exact).
  // Même règle que `recipe_skill` : la PREMIÈRE entrée gagne.
  std::unordered_map<uint32_t, int> recipe_lv;
  // Kit de cuisine -> niveau (`cooking N;`). 🔴 Ce niveau n'est dans AUCUN paquet :
  // le serveur le garde dans `menuskill_val` et n'envoie qu'un `mk_type = 1` identique
  // pour les cinq kits. C'est pourtant le terme dominant de la réussite d'un plat.
  std::unordered_map<uint32_t, int> cooking_kit;
  // -1 = absent du fichier. À NE PAS remplacer par 100 : cf. l'en-tête.
  int  weapon_produce_rate = -1;
  int  potion_produce_rate = -1;
  bool oridecon_research_fix = false;
  // [niveau d'arme][refine visé] ; -1 = absent du fichier. Indices 1-based, d'où
  // les +1 : on lit ces bornes telles qu'elles s'écrivent côté serveur plutôt que
  // de décaler à la main à chaque accès.
  int refine_weapon[kMaxWeaponLevel + 1][kMaxRefine + 1];
};

// Chargement PARESSEUX et unique. Une statique locale : le compilateur garantit
// l'initialisation thread-safe et une seule exécution, donc pas de drapeau à
// tenir ni de relecture par appel.
const Tables& Get() {
  static const Tables tables = [] {
    Tables t;
    for (auto& row : t.refine_weapon)
      for (int& cell : row) cell = -1;

    const std::string path = paths::RecipesPath();
    try {
      const YAML::Node root = YAML::LoadFile(path);

      if (const YAML::Node wl = root["weapon_lv"]) {
        // ⚠ Clés lues en CHAÎNE, pas en entier : elles sont compressées en plages
        // « 1101-1109: 1 ». Les armes se suivent par familles, si bien que 707
        // entrées tiennent en quelques dizaines de lignes. Un seul id reste écrit
        // seul, d'où les deux formes à accepter.
        for (auto it = wl.begin(); it != wl.end(); ++it) {
          const std::string key = it->first.as<std::string>();
          const int level = it->second.as<int>();
          const size_t dash = key.find('-');
          const uint32_t first =
              static_cast<uint32_t>(std::strtoul(key.c_str(), nullptr, 10));
          const uint32_t last =
              (dash == std::string::npos)
                  ? first
                  : static_cast<uint32_t>(
                        std::strtoul(key.c_str() + dash + 1, nullptr, 10));
          if (last < first || last - first > kMaxRangeSpan) continue;  // garde-fou
          for (uint32_t id = first; id <= last; ++id) t.weapon_lv[id] = level;
        }
      }
      if (const YAML::Node rates = root["rates"]) {
        t.weapon_produce_rate = rates["weapon_produce"].as<int>(-1);
        t.potion_produce_rate = rates["potion_produce"].as<int>(-1);
        t.oridecon_research_fix =
            rates["oridecon_research_fix"].as<int>(0) != 0;
      }
      if (const YAML::Node recipes = root["recipes"]) {
        for (const YAML::Node& entry : recipes) {
          const uint32_t id = entry["id"].as<uint32_t>(0);
          const int skill   = entry["skill"].as<int>(0);
          // `skill: 0` = fabrication par script d'objet sans compétence requise :
          // rien à retenir, et le noter effacerait la distinction avec « absent ».
          const int item_lv = entry["lv"].as<int>(0);
          if (id != 0 && item_lv > 0)
            t.recipe_lv.emplace(id, item_lv);  // emplace : la première gagne
          if (id == 0 || skill == 0) continue;
          t.recipe_skill.emplace(id, skill);  // emplace : la première gagne
        }
      }
      if (const YAML::Node kits = root["cooking_kits"]) {
        for (auto it = kits.begin(); it != kits.end(); ++it) {
          const uint32_t item_id = it->first.as<uint32_t>(0);
          const int level        = it->second.as<int>(0);
          if (item_id == 0 || level <= 0) continue;
          t.cooking_kit[item_id] = level;
        }
      }
      if (const YAML::Node rw = root["refine_weapon"]) {
        for (auto lvl = rw.begin(); lvl != rw.end(); ++lvl) {
          const int weapon_level = lvl->first.as<int>();
          if (weapon_level < 1 || weapon_level > kMaxWeaponLevel) continue;
          for (auto r = lvl->second.begin(); r != lvl->second.end(); ++r) {
            const int target = r->first.as<int>();
            if (target < 1 || target > kMaxRefine) continue;
            t.refine_weapon[weapon_level][target] = r->second.as<int>();
          }
        }
      }
      t.ok = true;
      LogInfo("[craftdata] {} : {} armes, {} recettes, table de refine chargee",
              path, t.weapon_lv.size(), t.recipe_skill.size());
    } catch (const std::exception& error) {
      // ⚠ Absence NON fatale, et surtout NON silencieuse : le fichier est livré
      // avec le patch, un joueur peut très bien ne pas l'avoir encore. Les
      // fonctions rendront alors « inconnu », et l'interface se taira au lieu
      // d'afficher un zéro qui passerait pour une vraie valeur.
      LogError("[craftdata] {} illisible ({}) — chances de refine et recettes "
               "etendues indisponibles", path, error.what());
    }
    return t;
  }();
  return tables;
}

}  // namespace

bool Available() { return Get().ok; }

int  WeaponProduceRate()    { return Get().weapon_produce_rate; }
int  PotionProduceRate()    { return Get().potion_produce_rate; }
bool OrideconResearchFix()  { return Get().oridecon_research_fix; }

int RecipeSkill(uint32_t item_id) {
  const Tables& t = Get();
  const auto it = t.recipe_skill.find(item_id);
  return (it == t.recipe_skill.end()) ? 0 : it->second;
}

int RecipeItemLevel(uint32_t item_id) {
  const Tables& t = Get();
  const auto it = t.recipe_lv.find(item_id);
  return (it == t.recipe_lv.end()) ? 0 : it->second;
}

int CookingKitLevel(uint32_t item_id) {
  const Tables& t = Get();
  const auto it = t.cooking_kit.find(item_id);
  return (it == t.cooking_kit.end()) ? 0 : it->second;
}

int WeaponLevel(uint32_t item_id) {
  const Tables& t = Get();
  const auto it = t.weapon_lv.find(item_id);
  return (it == t.weapon_lv.end()) ? 0 : it->second;
}

int RefineRateWeapon(int weapon_level, int target_refine) {
  if (weapon_level < 1 || weapon_level > kMaxWeaponLevel) return -1;
  if (target_refine < 1 || target_refine > kMaxRefine) return -1;
  return Get().refine_weapon[weapon_level][target_refine];
}

int RefineChancePercent(uint32_t item_id, int current_refine, int job_level,
                        bool third_class) {
  const int weapon_level = WeaponLevel(item_id);
  if (weapon_level <= 0) return -1;
  const int rate = RefineRateWeapon(weapon_level, current_refine + 1);
  if (rate < 0) return -1;

  // Transcription littérale de skill.cpp:11167-11171. La division entière est
  // VOULUE, y compris son résultat négatif sous job 50 : c'est ce que fait le
  // serveur, et arrondir « gentiment » ferait annoncer une chance qu'il
  // n'appliquera pas.
  int per = rate / 100;
  if (third_class) per += 10;
  else             per += (job_level - 50) / 2;

  // Le tirage est `per > rnd() % 100`, avec rnd()%100 dans 0..99 : au-delà de 100
  // c'est certain, en dessous de 1 c'est impossible. On borne donc l'AFFICHAGE
  // sans toucher au calcul.
  if (per < 0)   per = 0;
  if (per > 100) per = 100;
  return per;
}

}  // namespace craftdata
