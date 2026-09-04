#pragma once

// ── Vue de table mémorisée d'une frame à l'autre ─────────────────────────────
//
// LE PROBLÈME. Une table ImGui triable reconstruit sa vue — filtrage puis
// `std::sort` — à CHAQUE frame, y compris quand ni la source, ni le filtre, ni
// le tri n'ont bougé. Relevé sur neuf tables du projet : storage (jusqu'à 600
// lignes), fabrication, sources de drop, roster de guilde, MVP, fiche monstre.
//
// 🔴 ET TRIER LE MODÈLE EN PLACE N'EST PAS UNE OPTION. C'est pourtant le patron
// canonique d'ImGui (`SpecsDirty` -> on trie ses items). Ici l'ordre du serveur
// EST la référence : un paquet peut réécrire la liste à tout moment, et
// plusieurs fenêtres portent en commentaire que le modèle ne doit pas bouger.
// D'où une vue séparée — et d'où ce fichier, puisqu'une vue séparée doit être
// reconstruite pour rester juste.
//
// LE PRINCIPE. L'appelant refait chaque frame sa passe de filtrage, qui est
// O(n) et bon marché, mais il la pousse ICI plutôt que dans un vecteur local :
// un `Push` par ligne retenue, portant l'indice dans le modèle et une EMPREINTE
// des champs dont l'ordre dépend. `End` compare cette passe à celle de la frame
// précédente — une comparaison de POD, sans hachage donc sans collision. Si rien
// n'a changé et que le tri est le même, l'ordre trié de la frame précédente est
// conservé tel quel et le `std::sort` n'a pas lieu.
//
// 🔴 L'EMPREINTE DOIT PORTER TOUT CE DONT L'ORDRE DÉPEND. Le filtre, lui, y est
// déjà : filtrer autrement change la LISTE D'INDICES, qui est comparée
// intégralement. Ce qu'il faut y mettre à la main, ce sont les champs qui
// peuvent bouger SANS que la liste bouge — une quantité, un prix, un temps
// restant. Un champ oublié laisse la vue dans un ordre périmé jusqu'au prochain
// changement de liste ; il ne peut EN REVANCHE jamais produire un indice
// invalide, la liste d'indices étant, elle, toujours comparée en entier.
//
// ⚠ Des INDICES, jamais des pointeurs. Une vue survit d'une frame à l'autre ;
// un pointeur dans un `std::vector` que le modèle réalloue serait pendant. Les
// cinq sites qui portaient des `const T*` ont été convertis pour cette raison.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "imgui.h"  // ImGuiTableSortSpecs (TableSortKey, plus bas)

namespace ro {

// Compose une empreinte à partir des scalaires que le comparateur va lire.
// Mélange multiplicatif : deux champs échangés ne doivent pas donner la même
// valeur, sans quoi permuter une quantité et un prix passerait inaperçu.
inline uint64_t FingerprintMix(uint64_t seed, uint64_t value) {
  return (seed ^ value) * 1099511628211ull;  // premier FNV-1a 64 bits
}

// Raccourci pour le cas courant : « ces quelques champs, dans cet ordre ».
inline uint64_t Fingerprint() { return 1469598103934665603ull; }  // offset FNV
template <typename First, typename... Rest>
uint64_t Fingerprint(First first, Rest... rest) {
  return FingerprintMix(Fingerprint(rest...), static_cast<uint64_t>(first));
}

class SortedView {
 public:
  // Ouvre la passe de filtrage. `reserve` est la taille du MODÈLE, pas celle du
  // résultat : c'est le majorant, et il évite les réallocations en cours de
  // passe sans jamais sous-dimensionner.
  void Begin(std::size_t reserve) {
    pass_.clear();
    pass_.reserve(reserve);
  }

  // Une ligne retenue par le filtre. `fingerprint` résume les champs dont
  // l'ordre dépend et qui peuvent changer sans que la liste change (cf. l'avis
  // en tête) ; 0 quand il n'y en a aucun.
  void Push(int index, uint64_t fingerprint = 0) {
    pass_.push_back(Row{index, fingerprint});
  }

  // Ferme la passe. `sort_key` résume tout ce qui change l'ORDRE sans toucher à
  // la source : la colonne triée, le sens du tri, et au besoin l'onglet ou le
  // mode d'affichage courant.
  //
  // Rend true quand il faut (re)trier : `order()` est alors dans l'ordre du
  // MODÈLE, prêt pour un `std::sort`. Rend false quand la frame précédente
  // reste valable — `order()` garde alors son ordre trié, et l'appelant ne doit
  // pas y toucher.
  bool End(uint64_t sort_key) {
    if (sort_key == sort_key_ && pass_ == rows_) return false;
    sort_key_ = sort_key;
    rows_     = pass_;
    order_.clear();
    order_.reserve(rows_.size());
    for (const Row& row : rows_) order_.push_back(row.index);
    return true;
  }

  // Les indices à parcourir, dans l'ordre d'affichage.
  const std::vector<int>& order() const { return order_; }
  // Le même, à trier — À N'APPELER QUE SUR UN `End` VRAI.
  std::vector<int>& mutable_order() { return order_; }

  bool  empty() const { return order_.empty(); }
  std::size_t size() const { return order_.size(); }

  // Repart de zéro : la prochaine passe retriera forcément. À appeler quand la
  // vue change de SUJET (autre monstre, autre objet), sans quoi deux sujets aux
  // listes identiques se partageraient un ordre.
  // 🔴 La clé repart à `kNoSortKey`, et pas à zéro : une vue remise à zéro dont
  // la première passe serait VIDE doit quand même se reconstruire. Sans ça
  // `pass_ == rows_` est vrai (deux listes vides) et l'ordre resterait celui du
  // sujet précédent. Aucun appelant ne peut fabriquer cette clé-là.
  void Reset() {
    rows_.clear();
    order_.clear();
    pass_.clear();
    sort_key_ = kNoSortKey;
  }

 private:
  struct Row {
    int      index;
    uint64_t fingerprint;
    bool operator==(const Row& other) const {
      return index == other.index && fingerprint == other.fingerprint;
    }
  };

  // Valeur de tri impossible à produire par un appelant (les clés réelles sont
  // bâties sur une colonne et un sens), donc sûre comme « rien de mémorisé ».
  static constexpr uint64_t kNoSortKey = ~0ull;

  std::vector<Row> rows_;    // la passe mémorisée, dans l'ordre du modèle
  std::vector<Row> pass_;    // celle en cours de constitution
  std::vector<int> order_;   // les indices, dans l'ordre d'AFFICHAGE
  uint64_t sort_key_ = kNoSortKey;
};

// La clé de tri d'une table ImGui : colonne, sens, et présence d'un tri.
// Regroupée ici parce que les neuf sites la calculaient à l'identique.
//
// `specs` peut être nul, ou porter `SpecsCount == 0` (troisième clic : retour à
// l'ordre du modèle) — deux états distincts, et distincts d'un vrai tri, d'où le
// « +1 » sur la colonne : la colonne 0 triée ne doit pas ressembler à « aucun
// tri ». La valeur n'a pas d'autre sens que d'être stable et discriminante.
inline uint64_t TableSortKey(const ImGuiTableSortSpecs* specs) {
  if (specs == nullptr || specs->SpecsCount <= 0) return 0;
  const ImGuiTableColumnSortSpecs& first = specs->Specs[0];
  const uint64_t column = static_cast<uint64_t>(first.ColumnIndex) + 1;
  const uint64_t ascending =
      (first.SortDirection == ImGuiSortDirection_Ascending) ? 1 : 0;
  return column * 2 + ascending;
}

}  // namespace ro
