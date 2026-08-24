#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ragnarok/globals.h"

// ── Les listes SOCIALES du client : groupe et amis ───────────────────────────
//
// Elles vivent dans le manager de session (`rag::kSessionAddr`), alimentées par
// les handlers de paquets. La fenêtre native Amis/Groupe les relit à CHAQUE frame
// plutôt que de tenir un état : on fait pareil, au même endroit, avec les mêmes
// accesseurs. Détruire une fenêtre n'assèche donc rien.
//
// RE complète : docs/party_friend_re.md.
//
// Ce header est né le jour où un second consommateur est arrivé (le HUD de
// groupe, en plus de la fenêtre) — même raisonnement que `uiwnd.h` pour les
// fenêtres : une adresse recopiée dans deux fichiers finit par diverger.
//
// ⚠ DETTE ASSUMÉE, À SOLDER : `features/windows/party_friend_window.cc` porte
// ENCORE sa propre copie de ces offsets et de ces lecteurs, antérieure à ce
// fichier. Elle doit migrer ici — tant que ce n'est pas fait, toute correction
// d'offset est à appliquer AUX DEUX endroits.
namespace rag::social {

// ── L'entrée sociale (0x50 octets) ──────────────────────────────────────────
// MÊME type des deux côtés (amis et groupe) ; copy-ctor natif @0x00701df0. Les
// listes sont circulaires et la donnée commence à `nœud+8`.
struct Entry {
  uint32_t    gid    = 0;   // +0x04 — GID/AID, la clé des acteurs
  uint32_t    id2    = 0;   // +0x08 — second id (char id), clé du getter 0xd5d740
  std::string name;         // +0x0C
  std::string map;          // +0x24 — nom brut, tel que le serveur l'envoie
  bool        is_leader = false;  // +0x3C — 🔴 le natif code 0 = CHEF
  bool        offline   = false;  // +0x40
  uint32_t    color  = 0;   // +0x44 — encodage RGB/BGR NON tranché : ne pas rendre
  uint16_t    job    = 0;   // +0x48 — alimente icon_jobs_<job>.bmp
  uint16_t    level  = 0;   // +0x4A — le « Lv.%d » du natif

  // PV. 🔴 `has_hp` FAUX veut dire « hors de portée », pas « mort » : les PV
  // viennent du `CPc` de l'ACTEUR (cf. UpdateMemberHpGauges), donc un membre trop
  // loin n'en a aucun — le client officiel n'en affiche pas davantage.
  bool has_hp = false;
  int  hp     = 0;
  int  max_hp = 0;
};

// Relit la liste des membres du groupe (avec leurs PV) ou celle des amis.
// Ne lève jamais : une liste en cours de remaniement rend simplement moins
// d'entrées.
void ReadParty(std::vector<Entry>& out);
void ReadFriends(std::vector<Entry>& out);

// Mon AID (`g_Account_Aid`), celui que le natif compare pour reconnaître ma ligne.
uint32_t OwnAid();
// Nombre de membres, directement au manager (`Social_GetPartyMemberCount`).
int PartyMemberCount();

// Nom de classe affichable d'un job id. Passe par le résolveur natif
// (0x00d5bb40) et met en cache : il traverse la table Lua des classes.
const char* JobName(int job_id);

// Chemin de l'ICÔNE de classe, prête pour `ro::CachedTextureFromGameFile` :
// « <racine UI CP949>\renewalparty\icon_jobs_<job>.bmp ». C'est le gabarit exact
// du client (@0x0070622a, où le job est lu en `[esi+50h]` = data+0x48).
// ⚠ La variante `_die` existe mais son drapeau (distinct de « hors ligne ») n'est
// pas identifié : on ne la produit pas.
void JobIconPath(int job_id, char* out, size_t cap);

// MAX_PARTY / MAX_FRIENDS tels que MOONLIGHT les définit
// (`src/common/mmo.hpp`) — ⚠ PAS ceux d'un autre émulateur : Hercules dit 12 là où
// Moonlight dit 24. Le client, lui, écrit 12 en dur dans son propre compteur.
constexpr int kMaxPartyMembers = 24;
constexpr int kMaxFriends      = 40;

// `FriendList_AddByName` : demander en ami par le NOM, comme le fait `/friend`.
// Deux surfaces l'émettent — la commande du chat et le hub Groupe/Amis — et
// chacune la déclarait. C'est la même requête, donc les mêmes refus serveur.
constexpr uintptr_t kFriendListAddByNameAddr = 0x00a2c600;

}  // namespace rag::social
