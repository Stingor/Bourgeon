#pragma once

#include <cstdint>
#include <vector>

// ── craftdata : les données SERVEUR que le client n'a pas ─────────────────────
//
// Lit `SystemEN\bourgeon_recipes.yaml` (cf. paths::RecipesPath()), généré depuis
// les DB du serveur par moonlight/tools/gen_metalprocess.py.
//
// ⚠ À NE PAS confondre avec `data\MetalProcessItemList.txt` : celui-là est le
// fichier que le client NATIF lit dans sa fenêtre 80, au format contraint de
// Gravity (des lignes de texte indexées par produit, rien d'autre). Il reste
// livré — le plugin est opt-in, les joueurs en interface native en profitent.
// Celui-ci est le nôtre, et il porte ce que l'autre ne peut PAS exprimer.
//
// Chargement PARESSEUX au premier appel, une seule fois : le fichier fait
// quelques dizaines de kilo-octets et rien ne le relit en cours de partie.
namespace craftdata {

// Vrai si le fichier a été lu avec succès. Utile pour ne pas afficher une donnée
// absente comme si elle valait zéro.
bool Available();

// ── Réglages serveur qui MULTIPLIENT les chances de fabrication ──────────────
//
// 🔴 `make_per = make_per * rate / 100` côté serveur (skill.cpp). Sur Moonlight le
// taux vaut **500**, donc les chances de forge sont QUINTUPLÉES : un calcul qui
// l'ignore annonce un chiffre cinq fois trop bas, sans que rien ne le signale.
//
// Rendent **-1 quand le YAML ne les porte pas** — et c'est volontairement distinct de
// « 100 » : une valeur par défaut plausible ferait afficher un nombre faux sur un
// serveur qui a changé le réglage. -1 veut dire « je ne sais pas », et l'appelant doit
// alors se TAIRE.
int WeaponProduceRate();
int PotionProduceRate();
// Le bonus BS_ORIDEOCON (armes de niveau >= 3) s'applique-t-il ? `no` sur Moonlight.
bool OrideconResearchFix();

// Compétence REQUISE par la recette d'un produit (`req_skill` de produce_db), 0 si
// inconnue.
//
// 🔴 C'est la clef du calcul des chances de fabrication, et pour une raison qui ne se
// devine pas : `clif_parse_ProduceMix` appelle `skill_produce_mix(sd, 0, …)`, mais la
// fonction commence par `if (!skill_id) skill_id = skill_produce_db[idx].req_skill;`.
// La compétence qui décide du bonus n'est donc PAS celle que le joueur a lancée (il a
// pu n'utiliser qu'un Mini Furnace) mais celle que la RECETTE exige. C'est aussi ce
// qui explique qu'un non-forgeron voie une liste vide : `skill_can_produce_mix` filtre
// sur ce même champ.
int RecipeSkill(uint32_t item_id);

// `itemlv` de la recette d'un produit (1-3 arme, 11-20 nourriture, sinon valeur exacte),
// 0 si inconnu. Pour la cuisine c'est le terme de DIFFICULTÉ : `-400 * (itemlv - 10)`,
// soit -4 % par palier au-dessus de 11 — un plat d'itemlv 20 perd 40 points, ce qui
// explique qu'un kit modeste propose quand même tous les plats sans permettre de les
// réussir.
int RecipeItemLevel(uint32_t item_id);

// Niveau d'un kit de cuisine (`cooking N;` de son script), 0 si l'objet n'en est pas un.
//
// 🔴 Ce niveau n'est dans AUCUN paquet : le serveur le range dans `menuskill_val` et
// n'envoie qu'un `mk_type = 1` identique pour les cinq kits. C'est pourtant le terme
// DOMINANT de la réussite — `1200 * (niveau - 10)`, soit +12 % par palier — et à partir
// de **15** la réussite est GARANTIE (`make_per = 10000`, sans aucun tirage). Le client
// retrouve donc le kit par l'objet consommé, observé sur CZ_USE_ITEM.
//
// ⚠ Un niveau hors [11,20] n'est PAS de la cuisine : le serveur teste
// `menuskill_val > 10 && <= 20` avant d'appliquer la formule et retombe sinon sur un
// `make_per = 5000` plat. Cas réel : 12849 Combination Kit rend **30**, et produce_db
// n'a aucune recette d'itemlv 30 — sa liste est toujours vide.
int CookingKitLevel(uint32_t item_id);

// Niveau d'arme (1..4) d'un objet, 0 si inconnu.
// 🔴 C'est LA donnée qui manque au client pour calculer une chance de refine :
// elle n'est ni dans le paquet ZC 0x0221 (qui ne porte qu'index, nameid, refine
// et cartes) ni dans l'itemInfo.
int WeaponLevel(uint32_t item_id);

// Taux de base d'un refine, sur 10000 (donc /100 = pourcentage), pour le refine
// VISÉ (passer de +5 à +6 demande `target_refine = 6`). -1 si inconnu.
// Type « Normal » uniquement : c'est celui que `WS_WEAPONREFINE` emploie
// (`REFINE_COST_NORMAL` dans skill.cpp).
int RefineRateWeapon(int weapon_level, int target_refine);

// Chance RÉELLE en pourcentage d'un refine, ou -1 si indéterminable.
//
// Le calcul est DIRECT : le serveur fait
//     per = Rate/100 + (classe 3 ? +10 : (job_level - 50) / 2)
//     succès si per > rnd() % 100
// et `rnd()%100` étant uniforme sur 0..99, la probabilité de succès EST `per` %.
//
// ⚠ Une première rédaction ajoutait ici que le `rnd_value(1, 100) * 10` de la
// FABRICATION « interdit d'y annoncer un chiffre ferme ». C'est FAUX : ce tirage est
// uniforme sur 100 valeurs, donc la probabilité s'obtient en moyennant les 100
// issues de `rnd() % 10000 < make_per`. Un terme aléatoire À L'INTÉRIEUR d'un seuil
// ne rend pas la probabilité indéterminable — il demande juste une somme.
// Cf. le pavé de MakeItemWindow::DrawSuccessChance.
//
// ⚠ `job_level` inférieur à 50 donne un bonus NÉGATIF ((30-50)/2 = -10) : ce
// n'est pas un bug, le serveur fait bien cela.
int RefineChancePercent(uint32_t item_id, int current_refine, int job_level,
                        bool third_class);

// ── L'ATLAS : PARCOURIR la table, et plus seulement l'interroger ─────────────
//
// Tout ce qui précède répond à « que sais-tu de CET objet ? ». L'Atlas pose la
// question inverse — « que peut-on fabriquer, et avec quoi ? » — et AUCUNE des
// deux sources du client n'y répond :
//   · `MetalProcessRecipe_GetLines` est un lookup PAR CLÉ sur une std::map dont
//     rien n'expose l'énumération (docs/make_item_list_re.md §6.1) ;
//   · le serveur n'envoie une liste qu'APRÈS un lancement de compétence, filtrée
//     sur ce que le personnage sait déjà faire.
// Un joueur ne peut donc pas savoir ce qu'exige une recette avant d'avoir le
// métier, ni ce que devient un matériau qu'il ramasse. C'est la section
// `by_skill` du YAML qui rend le parcours possible, et elle n'existe que pour ça.

// Un matériau d'une recette.
struct Ingredient {
  uint32_t id  = 0;
  int      qty = 0;
  // Quantité 0 dans produce_db : l'objet doit être POSSÉDÉ, mais n'est PAS
  // consommé (les guides de fabrication). Conséquence à ne pas rater : il ne
  // borne pas le nombre de fabrications possibles — d'où ce drapeau plutôt
  // qu'un `qty` ramené à 1, qui aurait effacé la distinction.
  bool     not_consumed = false;
};

// Ce qu'une fabrication REND. Distinct d'`Ingredient` parce que le sens du
// nombre s'inverse : ici `qty` est un gain, pas une exigence.
struct Yield {
  uint32_t id  = 0;
  int      qty = 0;
};

struct Recipe {
  uint32_t product  = 0;
  int      lv       = 0;  // `itemlv` : 1-3 arme, 11-20 nourriture, sinon exact
  int      skill    = 0;  // 0 = fabrication par script d'objet
  int      skill_lv = 0;
  std::vector<Ingredient> mats;
};

// ── Combien d'exemplaires une fabrication RÉUSSIE rend ──────────────────────
// 🔴 Un, presque partout — mais pas partout, et rien dans produce_db ne le dit :
// `skill_produce_mix` recalcule lui-même `qty` pour cinq compétences (runes,
// poisons, cuisine et pharmacie de Genetic, transmutation). Afficher une recette
// de rune sans le dire annonce « 1 » là où le serveur en rend deux à six.
//
// Les bornes sont celles du SERVEUR, tous niveaux et toutes stats confondus : la
// valeur exacte dépend du personnage, que le fichier ne connaît pas. On donne
// donc une fourchette honnête plutôt qu'un chiffre inventé.
struct ProduceQty {
  enum Mode {
    kFixed,       // un seul exemplaire — le cas ordinaire
    kPerUnit,     // chaque exemplaire est tiré séparément : on peut en avoir moins
    kAllOrNone,   // la quantité part d'un coup si la fabrication réussit
    // 🔴 CUSTOM MOONLIGHT — n'existe dans aucun rAthena. Une fabrication DÉJÀ
    // réussie reçoit un bonus de +1 à +4, tiré une seule fois. Le joueur n'a
    // aucun moyen de le découvrir en jeu : rien ne l'annonce, et il faudrait
    // fabriquer des centaines de fois pour le soupçonner. C'est exactement ce
    // qu'un atlas doit dire.
    kBonusRoll,
    kTable,       // le rendement est décrit par une AUTRE table (changematerial)
  };
  int  min  = 1;
  int  max  = 1;
  Mode mode = kFixed;
  // Probabilité du bonus, en POUR MILLE (162 = 16,2 %) — `kBonusRoll` seulement.
  // En pour mille et non en pourcentage entier parce que la valeur réelle a une
  // décimale qui compte : arrondie à 16 %, elle serait fausse de 1,2 point.
  int  bonus_chance_permille = 0;

  bool IsFixedOne() const { return mode == kFixed && min == 1 && max == 1; }
};

// Le rendement des recettes d'une compétence. Rend `{1, 1, kFixed}` pour tout ce
// qui n'a pas d'entrée — de très loin le cas majoritaire, et la bonne réponse.
// ⚠ La clé est le `skill` d'une Recipe (le `req_skill` de produce_db), même quand
// le serveur normalise cette compétence en une autre avant de choisir la règle
// (2024 GC_RESEARCHNEWPOISON -> GC_CREATENEWPOISON) : c'est ce qui permet de
// retrouver la règle depuis une recette, sans rejouer la normalisation.
ProduceQty QtyForSkill(int skill);

// ── La production en MASSE (Twilight Alchemy) ───────────────────────────────
// Trois compétences rendent une fournée entière d'un seul lancement, sans liste
// et sans passer par le `req_skill` d'une recette : elles appellent
// `skill_produce_mix` avec une quantité en dur. produce_db ne les mentionne NULLE
// PART — les objets rendus y figurent bien, mais sous AM_PHARMACY et à l'unité.
//
// Sans ceci, la fiche d'une White Potion dirait « 1 par fabrication » alors qu'une
// Twilight Alchemy en sort deux cents d'un coup. C'est le genre d'écart qui décide
// d'une soirée de farm.
struct BulkSource {
  int skill = 0;
  int min   = 0;  // 🔴 « jusqu'à », pas « exactement » : chaque exemplaire de la
  int max   = 0;  //    fournée est tiré à make_per dans la boucle du serveur.
};

// Les compétences qui produisent cet objet en masse. Vide pour presque tout.
const std::vector<BulkSource>& BulkSourcesOf(uint32_t item_id);

// Fabrication de flèches — ⚠ sa clé est le matériau CONSOMMÉ, pas le produit :
// une fabrication consomme exactement un exemplaire de `source` et rend tout ce
// que porte `yields`. C'est l'inverse de toutes les autres recettes, et ce n'est
// pas une bizarrerie de notre YAML : `create_arrow_db` du serveur est bâti ainsi.
struct ArrowRecipe {
  uint32_t source = 0;
  std::vector<Yield> yields;
};

// ── Ce que CE serveur rend accessible ───────────────────────────────────────
// Le mode des FORMULES de combat (src/config/renewal.hpp du serveur).
// ⚠ Il ne dit RIEN de ce qui se fabrique : Moonlight est pre-renewal et ouvre
// pourtant les classes de 3e, dont les recettes (runes, poisons de Guillotine
// Cross, cuisine et pharmacie de Genetic) font près de la moitié de produce_db.
// Filtrer l'Atlas là-dessus amputerait des recettes parfaitement jouables — c'est
// `SkillIsLearnable` qu'il faut, pas ceci.
bool ServerIsRenewal();

// Une classe peut-elle APPRENDRE cette compétence sur ce serveur ? Calculé par le
// générateur sur l'arbre EFFECTIF (base + import), et c'est le seul critère juste
// pour masquer une recette injouable.
//
// Rend true quand l'information manque (fichier ancien, arbre illisible) : dans le
// doute on MONTRE. Cacher une recette réelle est une perte silencieuse ; en
// montrer une injouable ne coûte qu'une ligne de trop.
bool SkillIsLearnable(int skill);

// Toutes les recettes, dans l'ordre du fichier. Vide si le YAML manque.
// Les références et pointeurs rendus par cette famille restent valides jusqu'à
// la fin du processus : les tables sont construites une fois et jamais relues.
const std::vector<Recipe>& AllRecipes();

// La recette d'un produit, nullptr si aucune. ⚠ Un produit peut apparaître
// plusieurs fois dans produce_db (le serveur a de vrais doublons, cf. Steel) : on
// garde la PREMIÈRE, exactement comme `skill_can_produce_mix` qui re-cherche par
// nameid. Le générateur les a déjà réduits, mais la règle vaut pour la suite.
const Recipe* RecipeOf(uint32_t product);

// Les compétences qui fabriquent quelque chose, TRIÉES.
// ⚠ `0` en fait partie et n'est pas une valeur manquante : c'est la fabrication
// par SCRIPT D'OBJET (Mini Furnace, marteaux, kits) — celle qu'aucun métier
// n'ouvre et que le natif ne rattache à rien.
const std::vector<int>& SkillsWithRecipes();

// Les produits d'une compétence, dans l'ordre du serveur. Vide si la compétence
// ne fabrique rien.
const std::vector<uint32_t>& ProductsOfSkill(int skill);

// ── L'index INVERSE, celui que ni le client ni le serveur ne tiennent ────────
// « Mes 200 Jellopy servent à quoi ? » — la question qu'un joueur se pose devant
// son sac, et à laquelle aucune table du jeu ne répond : produce_db se lit
// produit -> matériaux, jamais l'inverse. Construit ICI au chargement, en un
// passage sur les recettes.
//
// Rendent des INDICES dans `AllRecipes()` / `AllArrows()` plutôt que des
// pointeurs : c'est ce qui permet à l'appelant de les garder, de les trier ou de
// les filtrer sans rien copier.
const std::vector<int>& RecipesUsing(uint32_t material);

const std::vector<ArrowRecipe>& AllArrows();
// La recette de flèches qui consomme `source`, nullptr si cet objet n'en fait
// pas. Une seule par source (clé unique du fichier).
const ArrowRecipe* ArrowFrom(uint32_t source);
// Indices des recettes de flèches qui PRODUISENT `arrow_id` — l'inverse du
// précédent, et la seule façon de répondre à « où trouve-t-on ces flèches ? ».
const std::vector<int>& ArrowsYielding(uint32_t arrow_id);

}  // namespace craftdata
