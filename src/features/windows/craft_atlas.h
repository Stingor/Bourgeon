#pragma once

#include <climits>  // INT_MIN : sentinelle « position jamais mémorisée »
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "features/plugin.h"

// ── CraftAtlas ───────────────────────────────────────────────────────────────
//
// L'ATLAS DES RECETTES : ce que le jeu sait fabriquer, PARCOURABLE.
//
// ── Le manque ────────────────────────────────────────────────────────────────
// Aucune des deux sources du client ne se parcourt :
//  · `MetalProcessRecipe_GetLines` (docs/make_item_list_re.md §6.1) est un lookup
//    PAR CLÉ sur une std::map dont rien n'expose l'énumération — on ne peut lui
//    demander une recette que si l'on connaît déjà le produit ;
//  · le serveur n'envoie une liste (ZC 0x018D / 0x01AD / 0x025A) qu'APRÈS un
//    lancement de compétence, et déjà filtrée sur ce que le personnage sait faire.
// Un joueur ne peut donc pas savoir ce qu'exige une recette avant d'avoir le
// métier, ni ce que deviendront les matériaux qu'il ramasse. Les deux questions
// se posent pourtant AVANT de se spécialiser, pas après.
//
// C'est la section `by_skill` de `SystemEN\bourgeon_recipes.yaml` qui rend le
// parcours possible, et l'index INVERSE (matériau -> recettes) est construit par
// `craftdata` au chargement : personne, ni client ni serveur, ne tient cette
// table-là.
//
// ── Une seule fiche, quatre chemins pour y arriver ───────────────────────────
// La fiche est celle d'un OBJET, et elle dit tout ce que l'Atlas sait de lui :
// sa recette, ce qu'il permet de fabriquer, ce qu'il donne en flèches, et d'où
// ces flèches viennent. Les onglets ne sont donc pas quatre vues différentes —
// ce sont quatre façons de DÉSIGNER l'objet dont on veut la fiche :
//   · Métiers   : par compétence (l'index `by_skill`), « que fait ce métier ? »
//   · Produits  : tout ce qui se fabrique, à plat, pour la recherche par nom
//   · Matériaux : l'index inverse, « mes 200 Jellopy servent à quoi ? »
//   · Flèches   : la fabrication de flèches, dont la clé est le MATÉRIAU et non
//                 le produit (cf. craftdata::ArrowRecipe) — d'où son onglet.
//
// ── Les gestes : CELLULES, pas liens ─────────────────────────────────────────
// 🔴 Ici le clic GAUCHE navigue, et c'est le clic DROIT qui ouvre la description.
// C'est la convention des CELLULES (feedback_right_click_opens_description), pas
// celle des LIENS de chat (`links::`, où le gauche ouvre la description) — et le
// choix est délibéré : dans un atlas, chaque ligne est une entrée d'index dont
// l'intérêt premier est d'être SUIVIE. Faire ouvrir une fenêtre de description
// par le geste le plus courant obligerait à la refermer à chaque saut. Maj+clic
// pose le lien dans le chat, comme partout ailleurs.
//
// ── Ce que l'Atlas ne dit PAS, et pourquoi ───────────────────────────────────
// Aucune CHANCE DE RÉUSSITE. Elle dépend de ce que le joueur tient au moment de
// fabriquer — kit de cuisine employé, maîtrise culinaire, compétence réellement
// lancée, stats — dont l'Atlas, consulté hors contexte, ne sait rien. Ce calcul
// appartient à la fenêtre de fabrication, qui a ces termes sous la main
// (MakeItemWindow::DrawSuccessChance). Annoncer ici un chiffre « moyen » serait
// un nombre faux affiché avec l'autorité d'un nombre juste.
//
// Pas d'opt-in « interface moderne » non plus : l'Atlas n'AGIT sur rien — il
// n'envoie aucun paquet, ne remplace aucune fenêtre native et ne touche pas
// l'inventaire. Il se contente de lire (feedback_optin_scope_actions_not_display).

class CraftAtlas : public Plugin {
 public:
  const char* name() const override { return "CraftAtlas"; }

  void OnRenderUI() override;
  void OnTick() override;

  // Contenu de la section « Atlas des recettes » du panneau Moonlight. Renvoie
  // true si un réglage a changé (l'appelant persiste alors le fichier).
  bool DrawSettings();

  // Ouvre l'Atlas SUR un objet — le point d'entrée des autres fenêtres (« et ça,
  // ça sert à quoi ? »). Ouvre la fenêtre si elle était fermée et remet
  // l'historique à plat : on arrive par une porte, pas au milieu d'un parcours.
  void OpenOnItem(uint32_t item_id);

  void Toggle() { open_ = !open_; }

  // Le libellé d'une compétence. PUBLIC et statique : la description d'objet en a
  // besoin pour son étiquette « Craft », et une quatrième copie de l'adresse du
  // résolveur natif (déjà dans skill_bar et character_sheet) n'aurait rien
  // apporté qu'un endroit de plus à corriger.
  //
  // `0` n'est pas une compétence manquante mais la fabrication SANS compétence
  // requise, celle qu'ouvre un objet. 🔴 Encore faut-il dire LAQUELLE : les
  // soixante plats de cuisine sont dans ce cas (`req_skill = 0`, on les ouvre avec
  // un kit), et les ranger sous « sans compétence » à côté d'un « Mix Cooking »
  // bien nommé donne l'impression d'une donnée manquante. D'où `recipe_lv` : un
  // itemlv de 11 à 20 est la marque d'un plat — c'est ce même test qui sert au
  // serveur pour appliquer la formule de cuisine.
  static const char* SkillLabel(int skill, int recipe_lv = 0);

  // Réglages persistés (moonlight_ui SettingsTable).
  bool& open()              { return open_; }
  bool& only_craftable()    { return only_craftable_; }
  bool& show_unavailable()  { return show_unavailable_; }
  bool& desc_tooltip()      { return desc_tooltip_; }
  int&  pos_x()             { return pos_x_; }
  int&  pos_y()             { return pos_y_; }

 private:
  // ── Les listes d'index, construites UNE fois ──────────────────────────────
  // Elles ne dépendent que du YAML, qui ne se relit jamais en cours de partie.
  // Le tri est fait par NOM d'objet, donc il faut que la DB d'items du client
  // soit chargée : d'où la construction au premier RENDU (on est alors en jeu)
  // et non au chargement du plugin.
  void EnsureIndex();

  // Stock de l'inventaire, en UN parcours par frame. Interroger la liste
  // chaînée par ligne affichée la reparcourrait des dizaines de fois pour la
  // même image — et l'Atlas montre le stock sur presque chaque ligne.
  void RebuildOwned();
  int  Owned(uint32_t item_id) const;

  // Combien de fois la recette est réalisable avec l'inventaire courant.
  // -1 = indéterminable (aucun matériau consommable : recette faite de guides
  // seulement). Les matériaux NON CONSOMMÉS ne bornent rien — les compter
  // afficherait « 0 » sur une recette parfaitement réalisable.
  int  CraftableCount(uint32_t product) const;

  // Le filtre courant retient-il cet objet ? Compare au nom ET à l'id : on
  // cherche « Jellopy » comme « 909 », et les deux se croisent dans les mêmes
  // conversations.
  bool Matches(uint32_t item_id) const;

  // La recette est-elle jouable sur ce serveur ? Une compétence qu'aucune classe
  // ne peut apprendre ici rend ses recettes injouables — elles restent pourtant
  // dans produce_db. ⚠ Le critère est l'ARBRE DE COMPÉTENCES, jamais le mode
  // renewal : Moonlight est pre-renewal et ouvre quand même les classes de 3e,
  // dont les recettes font près de la moitié du fichier (cf. craft_data.h).
  bool RecipeIsPlayable(uint32_t product) const;

  // Combien de recettes JOUABLES consomment cet objet. C'est ce nombre que la
  // liste des matériaux affiche et que la fiche détaille — les deux doivent dire
  // le MÊME chiffre. Annoncer « sert dans 12 recettes » puis n'en montrer que
  // quatre ferait chercher les huit autres.
  int PlayableUses(uint32_t material) const;

  // ── Navigation ────────────────────────────────────────────────────────────
  // Aller à la fiche d'un objet, en empilant la précédente. Le retour est ce qui
  // rend l'index inverse utilisable : on descend dans un matériau pour voir où il
  // sert, puis on revient à la recette d'où l'on venait.
  void GoTo(uint32_t item_id);
  void GoBack();

  void DrawToolbar();
  void DrawSkillTree();
  void DrawProductList();
  void DrawMaterialList();
  void DrawArrowList();
  void DrawSheet();
  // Une ligne d'objet cliquable : icône + libellé, avec les gestes de cellule
  // (gauche = naviguer, droit = description, Maj = lien de chat) et le survol
  // mémorisé pour l'aperçu. `selected` grise la ligne comme la sélection
  // courante. Renvoie true si la ligne vient d'être suivie.
  bool DrawItemRow(uint32_t item_id, const char* suffix, bool selected,
                   unsigned int text_color);

  // ── État d'affichage ──────────────────────────────────────────────────────
  bool     open_ = false;
  uint32_t sel_id_ = 0;          // l'objet dont la fiche est affichée, 0 = aucun
  std::vector<uint32_t> back_;   // pile de navigation (le bouton « Retour »)
  int      tab_ = 0;             // onglet courant, pour le rendu de la liste
  char     filter_[64] = {0};

  // Survol : on MÉMORISE, on peint APRÈS. `itemcell::DrawTooltip` crée son propre
  // popup et doit donc être appelé HORS de toute fenêtre ImGui — appelé depuis la
  // boucle d'une liste, il ne s'affiche pas du tout.
  bool     hover_valid_ = false;
  uint32_t hover_id_    = 0;

  // ── Index ─────────────────────────────────────────────────────────────────
  bool index_ready_ = false;
  std::vector<uint32_t> products_;       // tous les produits, triés par nom
  std::vector<uint32_t> materials_;      // tous les matériaux, triés par nom
  std::vector<uint32_t> arrow_sources_;  // toutes les sources de flèches, idem

  // id -> quantité en inventaire, refaite au début de chaque frame de rendu.
  std::unordered_map<uint32_t, int> owned_;

  // ── Réglages ──────────────────────────────────────────────────────────────
  // Ne montrer que les recettes réalisables MAINTENANT. Défaut OFF : l'Atlas sert
  // d'abord à préparer ce qu'on n'a pas encore, et un filtre par défaut cacherait
  // justement ce qu'on venait chercher.
  bool only_craftable_ = false;
  // Montrer aussi les recettes dont aucune classe ne peut apprendre la compétence
  // ici. Défaut OFF : ce sont des lignes que le serveur refusera toujours. Le
  // réglage existe quand même — le fichier de recettes sert aussi sur un serveur
  // au contenu différent, et un joueur curieux a le droit de voir la table
  // entière. ⚠ Sur Moonlight aujourd'hui il ne change RIEN : les 21 compétences
  // des recettes y sont apprenables, 3e classes comprises.
  bool show_unavailable_ = false;
  bool desc_tooltip_   = true;
  int  pos_x_ = INT_MIN;  // INT_MIN = « jamais posée » — PAS -1 : une fenêtre à
  int  pos_y_ = INT_MIN;  // cheval sur le bord gauche a un x négatif légitime.
  bool pos_dirty_ = false;
};
