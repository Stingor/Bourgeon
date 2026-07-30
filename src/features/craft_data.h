#pragma once

#include <cstdint>

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

}  // namespace craftdata
