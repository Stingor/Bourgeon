#pragma once

#include <cstdint>
#include <vector>

// ── Les BONS DE TRAVAIL « albums de cartes » (STAFF) ─────────────────────────
//
// 🔴🔴 CE FICHIER NE DIT PAS CE QU'UN ALBUM CONTIENT. Il dit ce qu'un membre du
// staff veut y ajouter ou en retirer, et rien d'autre.
//
// La vérité d'un album d'objets est SERVEUR : `db/import/item_group_db.yml`
// (groupes CARDALBUM et MAGICCARDALBUM), lu par `getrandgroupitem(IG_CardAlbum)`.
// Le `packageitem.lub` que le client charge au démarrage — et dont l'album tire
// ses macarons, cf. features/windows/item_probability.h — en est ENGENDRÉ par
// `gen_packageitem.py`. C'est un fichier d'AFFICHAGE.
//
// D'où la règle qui gouverne tout ce module : **on n'édite jamais le lub**. Une
// carte ajoutée au lub seul produirait un client qui promet une carte que
// l'album ne donnera jamais — et l'édition serait écrasée sans un mot à la
// première régénération. Le menu staff pose donc une INTENTION ici ; le script
// `apply_card_album_delta.py` (dépôt client, auprès de gen_packageitem.py) la
// porte dans le YAML serveur, puis régénère le lub depuis lui. Le sens de la
// synchronisation reste serveur -> client ; ce qui remonte fait trois champs.
//
// Corollaire pour l'interface : une intention n'est PAS un fait. Une pochette
// dont la carte est « à ajouter » ne se dessine pas comme une carte qui tombe
// déjà de l'album ; elle porte une marque d'attente, jusqu'à ce que le script
// ait tourné et que le lub régénéré la donne pour de bon.
//
// Le fichier vit dans `SaveData\bourgeon_card_album_delta.yaml`
// (paths::CardAlbumDeltaPath) et n'existe que sur les postes où quelqu'un a
// édité quelque chose. Absent = cas nominal.
namespace albumdelta {

// L'album visé. Ce sont des ids d'OBJET (le conteneur), pas des groupes serveur :
// le script fait la conversion, lui seul connaît `IG_CardAlbum`.
constexpr uint32_t kOldAlbum    = 616;
constexpr uint32_t kMysticAlbum = 12246;

// Bornes du poids de tirage, telles que `Rate` les accepte côté rAthena. Le
// plancher est 1 : un poids nul ne « désactive » pas une entrée, il la rend
// intirable tout en la laissant s'afficher — exactement le mensonge qu'on évite.
constexpr int kRateMin = 1;
constexpr int kRateMax = 10000;

struct Entry {
  uint32_t album  = 0;
  uint32_t card   = 0;
  int      rate   = 0;      // poids demandé ; ignoré quand `remove`
  bool     remove = false;  // false = ajouter, true = retirer
};

// Le contenu du fichier, lu au premier appel. Vide s'il n'existe pas.
const std::vector<Entry>& All();

// L'intention posée sur ce couple, ou nullptr. C'est ce que l'album interroge
// pour marquer une pochette « en attente ».
const Entry* Find(uint32_t album, uint32_t card);

// Pose une intention (remplace celle du même couple) et réécrit le fichier.
// Rend false si l'écriture a échoué — l'appelant doit le DIRE : une intention
// qui n'atteint pas le disque ne sera jamais appliquée, et le macaron d'attente
// affirmerait le contraire.
bool Set(uint32_t album, uint32_t card, int rate, bool remove);

// Retire l'intention : la pochette revient à ce que dit le serveur. Rend false
// si l'écriture a échoué. Retirer une intention absente est un succès.
bool Clear(uint32_t album, uint32_t card);

}  // namespace albumdelta
