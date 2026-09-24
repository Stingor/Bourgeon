#include "features/windows/card_album_delta.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <string>

#include "utils/game_paths.h"   // paths::CardAlbumDeltaPath
#include "utils/log_console.h"  // LogError

namespace albumdelta {
namespace {

// La clé du document. Nommée, et pas une séquence à la racine : le fichier est lu
// par un script Python autant que par nous, et un document qui s'annonce se
// diagnostique tout seul quand on l'ouvre par erreur.
constexpr char kRootKey[] = "card_album_delta";

std::vector<Entry> g_entries;
bool g_loaded = false;

// L'en-tête est écrit à la main : l'émetteur yaml-cpp ne porte pas de
// commentaires, et un bon de travail qu'on ouvre sans savoir ce qu'il commande
// est un bon de travail qu'on applique de travers.
constexpr char kHeader[] =
    "# Bons de travail « albums de cartes », posés EN JEU par le staff.\n"
    "#\n"
    "# Ce fichier ne dit PAS ce qu'un album contient : il dit ce qu'on veut y\n"
    "# ajouter (action: add, avec son poids de tirage) ou en retirer (remove).\n"
    "#\n"
    "# La vérité est serveur : moonlight/db/import/item_group_db.yml, groupes\n"
    "# CARDALBUM (album 616) et MAGICCARDALBUM (album 12246). Le fichier client\n"
    "# packageitem.lub en est ENGENDRÉ — ne jamais l'éditer à la main.\n"
    "#\n"
    "# À appliquer par :  python apply_card_album_delta.py\n"
    "#   (dépôt moonlight-client, auprès de gen_packageitem.py)\n"
    "# Il patche le YAML serveur, régénère le lub, puis vide ce fichier.\n";

void Load() {
  if (g_loaded) return;
  g_loaded = true;  // même en cas d'échec : ne pas retourner au disque à chaque frame
  try {
    const YAML::Node root = YAML::LoadFile(paths::CardAlbumDeltaPath());
    const YAML::Node list = root[kRootKey];
    if (!list || !list.IsSequence()) return;
    for (const auto& n : list) {
      Entry e;
      e.album = n["album"].as<uint32_t>(0u);
      e.card = n["card"].as<uint32_t>(0u);
      e.rate = n["rate"].as<int>(0);
      e.remove = (n["action"].as<std::string>("add") == "remove");
      if (e.album == 0 || e.card == 0) continue;
      g_entries.push_back(e);
    }
  } catch (const std::exception&) {
    // Fichier absent : c'est le cas NOMINAL, personne n'a encore rien édité. Un
    // document abîmé se traite pareil — repartir de rien vaut mieux qu'appliquer
    // la moitié d'une intention.
  }
}

bool Flush() {
  const std::string path = paths::CardAlbumDeltaPath();
  YAML::Node list(YAML::NodeType::Sequence);
  for (const Entry& e : g_entries) {
    YAML::Node n(YAML::NodeType::Map);
    n["album"] = e.album;
    n["card"] = e.card;
    n["action"] = e.remove ? "remove" : "add";
    // Le poids n'est écrit que pour un ajout : un `rate` sur un retrait
    // laisserait croire au script qu'il a un poids à reporter quelque part.
    if (!e.remove) n["rate"] = e.rate;
    list.push_back(n);
  }
  YAML::Node root(YAML::NodeType::Map);
  root[kRootKey] = list;

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    LogError("[album] bon de travail non écrit : {}", path);
    return false;
  }
  f << kHeader << root << "\n";
  f.flush();
  if (!f) {
    LogError("[album] bon de travail écrit à moitié : {}", path);
    return false;
  }
  return true;
}

std::vector<Entry>::iterator FindMut(uint32_t album, uint32_t card) {
  return std::find_if(g_entries.begin(), g_entries.end(), [&](const Entry& e) {
    return e.album == album && e.card == card;
  });
}

}  // namespace

const std::vector<Entry>& All() {
  Load();
  return g_entries;
}

const Entry* Find(uint32_t album, uint32_t card) {
  Load();
  const auto it = FindMut(album, card);
  return it == g_entries.end() ? nullptr : &*it;
}

bool Set(uint32_t album, uint32_t card, int rate, bool remove) {
  Load();
  if (album == 0 || card == 0) return false;
  rate = std::clamp(rate, kRateMin, kRateMax);

  Entry e;
  e.album = album;
  e.card = card;
  e.rate = rate;
  e.remove = remove;

  const auto it = FindMut(album, card);
  if (it == g_entries.end())
    g_entries.push_back(e);
  else
    *it = e;

  // 🔴 L'écriture d'abord, l'état ensuite : si le disque refuse, on RETIRE ce
  // qu'on vient de poser. Garder une intention que le fichier ne porte pas
  // afficherait un macaron d'attente pour un travail que le script ne verra
  // jamais.
  if (Flush()) return true;
  const auto undo = FindMut(album, card);
  if (undo != g_entries.end()) g_entries.erase(undo);
  return false;
}

bool Clear(uint32_t album, uint32_t card) {
  Load();
  const auto it = FindMut(album, card);
  if (it == g_entries.end()) return true;  // rien à retirer : déjà à l'état voulu
  const Entry saved = *it;
  g_entries.erase(it);
  if (Flush()) return true;
  g_entries.push_back(saved);
  return false;
}

}  // namespace albumdelta
