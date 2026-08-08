#include "features/craft_data.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

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

// Rendu quand on interroge une clé absente. Une référence sur un vide STATIQUE
// plutôt qu'un pointeur nul : l'appelant écrit `for (int i : RecipesUsing(id))`
// sans garde, et une table non chargée se parcourt comme une table vide.
template <typename T>
const std::vector<T>& EmptyVector() {
  static const std::vector<T> empty;
  return empty;
}

struct Tables {
  bool ok = false;
  std::unordered_map<uint32_t, int> weapon_lv;
  // ── Les recettes, UNE fois ────────────────────────────────────────────────
  // Source unique : `recipe_at` indexe ce vecteur, et RecipeSkill/RecipeItemLevel
  // s'y ramènent au lieu de tenir chacune sa propre table. Un produit peut
  // apparaître dans plusieurs entrées de produce_db (le serveur en a de vrais
  // doublons, cf. Steel) ; on garde la PREMIÈRE, comme le fait
  // `skill_can_produce_mix` qui re-cherche par nameid.
  std::vector<Recipe> recipes;
  std::unordered_map<uint32_t, int> recipe_at;  // produit -> index dans `recipes`
  // ── L'index INVERSE, construit ici et nulle part ailleurs ─────────────────
  // matériau -> indices des recettes qui le consomment. Aucune table du jeu ne
  // se lit dans ce sens ; c'est un simple passage sur `recipes`, mais il faut
  // penser à le faire.
  std::unordered_map<uint32_t, std::vector<int>> used_by;
  // compétence -> ses produits, dans l'ordre du serveur (section `by_skill`),
  // et la liste TRIÉE de ces compétences pour l'affichage.
  std::unordered_map<int, std::vector<uint32_t>> by_skill;
  std::vector<int> skills;
  // compétence -> rendement, quand il n'est pas de UN. L'absence d'entrée est la
  // réponse « un exemplaire », pas une donnée manquante.
  std::unordered_map<int, ProduceQty> produce_qty;
  // objet -> les compétences qui le produisent en MASSE (Twilight Alchemy).
  // Indexé par objet et non par compétence : la question se pose toujours depuis
  // la fiche d'un objet (« d'où sortent ces deux cents potions ? »).
  std::unordered_map<uint32_t, std::vector<BulkSource>> bulk_by_item;
  bool server_renewal = false;
  // 🔴 DEUX états à ne pas confondre : « la liste est vide » (tout est apprenable)
  // et « la liste n'a pas été calculée » (le générateur n'a pas pu lire l'arbre).
  // Sans ce drapeau, un fichier ancien — qui n'a pas la clé du tout — se lirait
  // comme « rien n'est indisponible », ce qui est justement la bonne conclusion
  // par hasard, mais pour la mauvaise raison. Le jour où l'on inverserait le sens
  // par défaut, l'erreur serait silencieuse.
  bool has_availability = false;
  std::vector<int> unavailable_skills;
  // Flèches : la clé est le matériau CONSOMMÉ (cf. l'en-tête de ArrowRecipe).
  std::vector<ArrowRecipe> arrows;
  std::unordered_map<uint32_t, int> arrow_at;                 // source -> index
  std::unordered_map<uint32_t, std::vector<int>> arrow_from;  // flèche -> indices
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
          Recipe r;
          r.product  = entry["id"].as<uint32_t>(0);
          r.skill    = entry["skill"].as<int>(0);
          r.skill_lv = entry["skill_lv"].as<int>(0);
          r.lv       = entry["lv"].as<int>(0);
          if (r.product == 0) continue;
          if (const YAML::Node mats = entry["mats"]) {
            for (const YAML::Node& pair : mats) {
              if (pair.size() < 2) continue;  // ligne tronquée : on la saute
              Ingredient ing;
              ing.id  = pair[0].as<uint32_t>(0);
              ing.qty = pair[1].as<int>(0);
              if (ing.id == 0) continue;
              // 🔴 Quantité 0 = « à POSSÉDER, non consommé » (les guides), pas
              // « aucune » : c'est le marqueur que produce_db emploie, et le
              // perdre ferait disparaître le matériau de l'affichage alors qu'il
              // est bel et bien exigé.
              ing.not_consumed = (ing.qty == 0);
              r.mats.push_back(ing);
            }
          }
          // emplace : la PREMIÈRE entrée portant ce produit gagne (cf. Tables).
          // Le test se fait AVANT le push_back, sans quoi l'index pointerait sur
          // le doublon qu'on voulait ignorer.
          const int index = static_cast<int>(t.recipes.size());
          if (!t.recipe_at.emplace(r.product, index).second) continue;
          for (const Ingredient& ing : r.mats) t.used_by[ing.id].push_back(index);
          t.recipes.push_back(std::move(r));
        }
      }
      if (const YAML::Node by_skill = root["by_skill"]) {
        for (auto it = by_skill.begin(); it != by_skill.end(); ++it) {
          const int skill = it->first.as<int>(-1);
          // ⚠ `skill == 0` est LÉGITIME (fabrication par script d'objet) : le
          // rejeter comme « absent » retirerait de l'Atlas les soixante potions
          // de convertisseurs, qu'aucun métier ne fabrique.
          if (skill < 0) continue;
          // 🔴 COPIE du nœud, et ce n'est pas de la prudence gratuite. L'itérateur
          // de map de yaml-cpp rend son `pair<Node,Node>` à travers un PROXY
          // temporaire, détruit à la fin de l'expression complète. Un
          // `for (… : it->second)` lie donc sa référence à un nœud déjà mort : la
          // séquence se lit VIDE, sans le moindre plantage ni message. C'était le
          // bogue « onglet Métiers vide » — les autres onglets, qui viennent de
          // `recipes`, marchaient parfaitement.
          // ⚠ Le motif est sûr partout où `it->second` est consommé dans UNE
          // expression (`it->second.as<int>()`), et dangereux dès qu'on le garde
          // au-delà — ce qu'une boucle fait par construction.
          const YAML::Node product_ids = it->second;
          std::vector<uint32_t>& products = t.by_skill[skill];
          for (const YAML::Node& id : product_ids) {
            const uint32_t product = id.as<uint32_t>(0);
            if (product != 0) products.push_back(product);
          }
          if (products.empty()) t.by_skill.erase(skill);
          else                  t.skills.push_back(skill);
        }
        std::sort(t.skills.begin(), t.skills.end());
      }
      if (const YAML::Node server = root["server"]) {
        t.server_renewal = server["renewal"].as<bool>(false);
        if (const YAML::Node unavailable = server["unavailable_skills"]) {
          t.has_availability = true;
          for (const YAML::Node& skill : unavailable)
            t.unavailable_skills.push_back(skill.as<int>(-1));
        }
      }
      if (const YAML::Node qty = root["produce_qty"]) {
        for (auto it = qty.begin(); it != qty.end(); ++it) {
          const int skill = it->first.as<int>(-1);
          if (skill < 0) continue;
          ProduceQty pq;
          pq.min = it->second["min"].as<int>(1);
          pq.max = it->second["max"].as<int>(1);
          const std::string mode = it->second["mode"].as<std::string>("");
          if      (mode == "per_unit")    pq.mode = ProduceQty::kPerUnit;
          else if (mode == "all_or_none") pq.mode = ProduceQty::kAllOrNone;
          else if (mode == "bonus_roll")  pq.mode = ProduceQty::kBonusRoll;
          else if (mode == "table")       pq.mode = ProduceQty::kTable;
          else continue;  // mode inconnu : on se TAIT plutôt que de deviner
          pq.bonus_chance_permille = it->second["chance_permille"].as<int>(0);
          t.produce_qty[skill] = pq;
        }
      }
      if (const YAML::Node bulk = root["bulk_recipes"]) {
        for (auto it = bulk.begin(); it != bulk.end(); ++it) {
          const int skill = it->first.as<int>(0);
          if (skill == 0) continue;
          const YAML::Node entries = it->second;  // COPIE : cf. `by_skill` ci-dessus
          for (const YAML::Node& entry : entries) {
            if (entry.size() < 3) continue;  // [id, min, max]
            const uint32_t item_id = entry[0].as<uint32_t>(0);
            if (item_id == 0) continue;
            BulkSource source;
            source.skill = skill;
            source.min   = entry[1].as<int>(0);
            source.max   = entry[2].as<int>(0);
            t.bulk_by_item[item_id].push_back(source);
          }
        }
      }
      if (const YAML::Node arrows = root["arrows"]) {
        for (const YAML::Node& entry : arrows) {
          ArrowRecipe a;
          a.source = entry["src"].as<uint32_t>(0);
          if (a.source == 0) continue;
          if (const YAML::Node yields = entry["yields"]) {
            for (const YAML::Node& pair : yields) {
              if (pair.size() < 2) continue;
              Yield y;
              y.id  = pair[0].as<uint32_t>(0);
              y.qty = pair[1].as<int>(0);
              if (y.id != 0) a.yields.push_back(y);
            }
          }
          if (a.yields.empty()) continue;  // une source sans rendement ne dit rien
          const int index = static_cast<int>(t.arrows.size());
          if (!t.arrow_at.emplace(a.source, index).second) continue;
          for (const Yield& y : a.yields) t.arrow_from[y.id].push_back(index);
          t.arrows.push_back(std::move(a));
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
      LogInfo("[craftdata] {} : {} armes, {} recettes ({} matériaux indexés à "
              "l'envers), {} recettes de flèches, {} compétences, table de "
              "refine chargée",
              path, t.weapon_lv.size(), t.recipes.size(), t.used_by.size(),
              t.arrows.size(), t.skills.size());
    } catch (const std::exception& error) {
      // ⚠ Absence NON fatale, et surtout NON silencieuse : le fichier est livré
      // avec le patch, un joueur peut très bien ne pas l'avoir encore. Les
      // fonctions rendront alors « inconnu », et l'interface se taira au lieu
      // d'afficher un zéro qui passerait pour une vraie valeur.
      LogError("[craftdata] {} illisible ({}) — chances de refine et recettes "
               "étendues indisponibles", path, error.what());
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
  const Recipe* r = RecipeOf(item_id);
  // `skill: 0` = fabrication par script d'objet sans compétence requise. Rendre
  // 0 comme pour une recette absente est VOULU : le contrat de cette fonction est
  // « quelle compétence exiger », et la réponse est « aucune » dans les deux cas.
  // Qui a besoin de distinguer les deux interroge `RecipeOf`.
  return r ? r->skill : 0;
}

int RecipeItemLevel(uint32_t item_id) {
  const Recipe* r = RecipeOf(item_id);
  return r ? r->lv : 0;
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

// ── Atlas ───────────────────────────────────────────────────────────────────

bool ServerIsRenewal() { return Get().server_renewal; }

bool SkillIsLearnable(int skill) {
  const Tables& t = Get();
  // Information non calculée : on MONTRE. Cacher une recette réelle est une perte
  // silencieuse ; en montrer une injouable ne coûte qu'une ligne de trop.
  if (!t.has_availability) return true;
  // `0` n'est pas une compétence : c'est la fabrication par script d'objet, qui ne
  // s'apprend pas et reste toujours accessible.
  if (skill == 0) return true;
  return std::find(t.unavailable_skills.begin(), t.unavailable_skills.end(),
                   skill) == t.unavailable_skills.end();
}

ProduceQty QtyForSkill(int skill) {
  const Tables& t = Get();
  const auto it = t.produce_qty.find(skill);
  return (it == t.produce_qty.end()) ? ProduceQty{} : it->second;
}

const std::vector<BulkSource>& BulkSourcesOf(uint32_t item_id) {
  const Tables& t = Get();
  const auto it = t.bulk_by_item.find(item_id);
  return (it == t.bulk_by_item.end()) ? EmptyVector<BulkSource>() : it->second;
}

const std::vector<Recipe>& AllRecipes() { return Get().recipes; }

const Recipe* RecipeOf(uint32_t product) {
  const Tables& t = Get();
  const auto it = t.recipe_at.find(product);
  return (it == t.recipe_at.end()) ? nullptr : &t.recipes[it->second];
}

const std::vector<int>& SkillsWithRecipes() { return Get().skills; }

const std::vector<uint32_t>& ProductsOfSkill(int skill) {
  const Tables& t = Get();
  const auto it = t.by_skill.find(skill);
  return (it == t.by_skill.end()) ? EmptyVector<uint32_t>() : it->second;
}

const std::vector<int>& RecipesUsing(uint32_t material) {
  const Tables& t = Get();
  const auto it = t.used_by.find(material);
  return (it == t.used_by.end()) ? EmptyVector<int>() : it->second;
}

const std::vector<ArrowRecipe>& AllArrows() { return Get().arrows; }

const ArrowRecipe* ArrowFrom(uint32_t source) {
  const Tables& t = Get();
  const auto it = t.arrow_at.find(source);
  return (it == t.arrow_at.end()) ? nullptr : &t.arrows[it->second];
}

const std::vector<int>& ArrowsYielding(uint32_t arrow_id) {
  const Tables& t = Get();
  const auto it = t.arrow_from.find(arrow_id);
  return (it == t.arrow_from.end()) ? EmptyVector<int>() : it->second;
}

}  // namespace craftdata
