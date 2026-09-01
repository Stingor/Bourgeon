#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ── La table de PROBABILITÉ d'une boîte / d'une branche ──────────────────────
//
// Le client embarque, pour une poignée d'objets « conteneurs » (boîtes, albums,
// branches, sacs de pièces), la liste de ce qu'ils peuvent rendre et le poids de
// chaque tirage. C'est `NEO_PACKAGEITEM::CNeoPackageItemMgr`, alimenté au
// démarrage par `data\luafiles514\lua files\probabilityinfo\packageitem.lub`
// (dans `moonlight.grf`) via la fonction Lua `InsertPackage(itemId, groupe,
// total, libellé, poids)`.
//
// 🔴 Le libellé est du TEXTE RICHE, pas un nom nu — Moonlight y a mis des liens
// vers son site :
//     ^i[1146] <URL>Town Sword [1]<INFO>…?page=itemdb&itemid=1146</INFO></URL>
//     <URL>Corrupted Soul<INFO>…?page=bestiary&mobid=2475</INFO></URL>
// C'est donc l'URL qui dit si l'entrée désigne un OBJET ou un MONSTRE : une
// branche invoque des monstres, une boîte rend des objets. Ne pas le déduire de
// la plage d'identifiants — les deux se chevauchent.
//
// La fenêtre native équivalente (`CUIProbabilityTable`, id 0x271C = 10012) se
// compose avant ImGui : elle passerait SOUS l'overlay. On lit donc la donnée et
// on la redessine nous-mêmes.
namespace itemprob {

// Une ligne : ce que le conteneur peut rendre, et à quelle fréquence.
struct Entry {
  uint32_t    id     = 0;      // objet ou monstre, selon `is_mob`
  bool        is_mob = false;  // tranché par l'URL du libellé, pas par l'id
  int         weight = 0;      // numérateur du tirage
  double      pct    = 0.0;    // pourcentage, calculé comme le natif
  std::string name;            // libellé sans balisage, code-page du CLIENT
};

// Un tirage : les entrées se partagent `total`. Le groupe 0 est le lot GARANTI
// (le natif lui affiche 100 % sans diviser quoi que ce soit).
struct Group {
  int                id    = 0;
  int                total = 0;
  std::vector<Entry> entries;
};

struct Table {
  std::vector<Group> groups;
  size_t             entry_count = 0;  // somme des entrées, tous groupes
};

// L'objet a-t-il une table ? Interrogation DIRECTE de la base du client
// (O(log n), aucune allocation) : c'est ce qui garde l'onglet éligible frame
// après frame sans rien construire.
bool Has(uint32_t item_id);

// La table, lue puis mise en cache. nullptr si l'objet n'en a pas.
//
// 🔴 À n'appeler QUE quand l'onglet est ouvert : une boîte peut porter plus de
// mille entrées, et `Has` suffit à décider de l'affichage de l'onglet.
const Table* Get(uint32_t item_id);

}  // namespace itemprob
