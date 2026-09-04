#pragma once

// ── Table CHAÎNE -> valeur, interrogeable par `const char*` SANS allouer ─────
//
// LE PROBLÈME. Une `std::unordered_map<std::string, T>` interrogée avec un
// `const char*` construit une `std::string` TEMPORAIRE à chaque appel — y
// compris sur un SUCCÈS, et y compris pour `operator[]`, qui prend sa clé par
// valeur. Nos clés sont des chemins d'interface du client
// (« 유저인터페이스\item\… ») : bien au-delà des quinze caractères que la SSO
// loge sur la pile, donc une allocation et une libération par interrogation.
// Or ces caches sont interrogés à chaque frame, pour chaque icône et chaque
// bouton dessinés — c'est-à-dire des centaines de fois par image.
//
// 🔴 ET C++17 N'A AUCUNE PARADE DIRECTE. La recherche hétérogène — chercher par
// `string_view` sans construire de `string` — n'est arrivée aux tables de
// hachage qu'en C++20. En C++17, seul le conteneur ORDONNÉ la connaît, par le
// comparateur transparent `std::less<>` : c'est le choix fait dans
// `utils/i18n.cc`, et il y est justifié pour SES clés. Il ne convient pas
// ici — nos chemins partagent un LONG préfixe commun, qui est le pire cas d'un
// arbre : chaque comparaison le reparcourt avant de départager.
//
// LA PARADE. Indexer par `std::string_view` et garder les chaînes elles-mêmes
// dans un conteneur à adresses stables. La recherche redevient un hachage sans
// allocation ; l'insertion, elle, en fait une — une seule fois par clé.
//
// ⚠⚠ `std::deque` et NON `std::vector` pour le stockage. Une réallocation de
// vector déplacerait les `std::string`, et toutes les vues de la table
// pointeraient alors dans de la mémoire libérée — sans le moindre signe avant
// le crash. C'est exactement la raison qui l'impose déjà à `g_storage` dans
// `ragnarok/msgstring_override.cc`, où elle est commentée en ces termes.
//
// ⚠ Ce que cette table ne fait PAS : effacer une entrée. Aucun appelant n'en a
// besoin (ce sont des caches qu'on vide en bloc), et l'ajouter demanderait de
// savoir aussi retirer la chaîne du stockage — donc de renoncer aux adresses
// stables, ou de laisser fuir. `Clear` vide les deux ensemble.
//
// ⚠ Seul `Find` tolère une clé NULLE — c'est le seul appel qu'un chemin de
// rendu fait sur une donnée venue du client. `operator[]` et `Emplace` écrivent,
// et un appelant qui écrit sait toujours quelle clé il pose : leur passer
// nullptr construirait un `string_view` sur un pointeur nul, ce qui est un
// comportement indéfini.

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace util {

template <typename T>
class StrKeyMap {
 public:
  using Map = std::unordered_map<std::string_view, T>;

  // La valeur associée à `key`, ou nullptr si absente. AUCUNE allocation.
  T* Find(const char* key) {
    if (key == nullptr) return nullptr;  // string_view(nullptr) est un UB
    const auto it = map_.find(std::string_view(key));
    return it == map_.end() ? nullptr : &it->second;
  }
  const T* Find(const char* key) const {
    if (key == nullptr) return nullptr;
    const auto it = map_.find(std::string_view(key));
    return it == map_.end() ? nullptr : &it->second;
  }

  // La valeur de `key`, construite par défaut si elle manque. N'alloue qu'à la
  // création — c'est le remplaçant direct de `map[key]`, qui allouait toujours.
  T& operator[](const char* key) {
    const std::string_view view(key);
    const auto it = map_.find(view);
    if (it != map_.end()) return it->second;
    return map_[Own(key)];
  }

  // Insère si `key` est absente ; ne REMPLACE jamais une entrée existante, et
  // rend celle qui est en place dans ce cas. Même sémantique que
  // `std::unordered_map::emplace`, dont un appelant au moins dépend (deux clés
  // de la MsgStringTable peuvent porter le même texte anglais : la première
  // traduction gagne).
  T& Emplace(const char* key, T value) {
    const std::string_view view(key);
    const auto it = map_.find(view);
    if (it != map_.end()) return it->second;
    return map_.emplace(Own(key), std::move(value)).first->second;
  }

  // 🔴 Les deux ENSEMBLE : une chaîne conservée sans son entrée ne serait plus
  // jamais retrouvée, et une entrée conservée sans sa chaîne serait pendante.
  void Clear() {
    map_.clear();
    storage_.clear();
  }

  bool        empty() const { return map_.empty(); }
  std::size_t size()  const { return map_.size(); }

  // Parcours des entrées (relâchement de handles, statistiques). Les clés en
  // sortent en `std::string_view`, valides tant que la table n'est pas vidée.
  typename Map::iterator begin() { return map_.begin(); }
  typename Map::iterator end()   { return map_.end(); }
  typename Map::const_iterator begin() const { return map_.begin(); }
  typename Map::const_iterator end()   const { return map_.end(); }

 private:
  // Recopie `key` dans le stockage et rend une vue dessus. À n'appeler QUE
  // lorsque la clé est absente : elle y ajouterait sinon un doublon que rien ne
  // viendrait relire.
  std::string_view Own(const char* key) {
    storage_.emplace_back(key);
    return std::string_view(storage_.back());
  }

  std::deque<std::string> storage_;  // propriétaire des clés, adresses STABLES
  Map                     map_;      // les vues, plus la valeur
};

}  // namespace util
