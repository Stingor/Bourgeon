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
// ✅ Dette soldée le 2026-08-24 : `features/windows/party_friend_window.cc`
// portait sa propre copie de ces offsets et de ces lecteurs, antérieure à ce
// fichier — sept fonctions et vingt-deux constantes en double. Elle a migré ici,
// et la fenêtre comme le HUD de groupe lisent maintenant les mêmes octets.
namespace rag::social {

// ── L'entrée sociale (0x50 octets) ──────────────────────────────────────────
// MÊME type des deux côtés (amis et groupe) ; copy-ctor natif @0x00701df0. Les
// listes sont circulaires et la donnée commence à `nœud+8`.
struct Entry {
  uint32_t    gid    = 0;   // +0x04 — GID/AID, la clé des acteurs
  uint32_t    id2    = 0;   // +0x08 — second id (char id), clé du getter 0xd5d740
  std::string name;         // +0x0C
  std::string map;          // +0x24 — nom BRUT : à passer par MapDisplayName
  bool        is_leader = false;  // +0x3C — 🔴 le natif code 0 = CHEF
  bool        offline   = false;  // +0x40
  uint32_t    color  = 0;   // +0x44 — encodage RGB/BGR NON tranché : ne pas rendre
  uint16_t    job    = 0;   // +0x48 — alimente icon_jobs_<job>.bmp
  // +0x4A — le « Lv.%d » du natif. Prouvé au DÉSASSEMBLAGE et non au
  // décompilateur : le site du « Lv.%d » (0x0070433d) pousse
  // `movzx eax, word ptr [esi+52h]`, soit nœud+0x52 = data+0x4A. L'argument
  // passant par un sprintf variadique, le décompilateur ne le montrait pas.
  uint16_t    level  = 0;
  // ⚠ Reste non identifié dans la structure native : +0x4C (u32).

  // PV. 🔴 `has_hp` FAUX veut dire « hors de portée », pas « mort » : les PV
  // viennent du `CPc` de l'ACTEUR (cf. UpdateMemberHpGauges), donc un membre trop
  // loin n'en a aucun — le client officiel n'en affiche pas davantage.
  bool has_hp = false;
  int  hp     = 0;
  int  max_hp = 0;

  // ── L'APPARENCE, pour une vignette de tête ────────────────────────────────
  //
  // 🔴 `has_look` FAUX veut dire « hors de portée », comme `has_hp` : ces trois
  // valeurs sont lues sur l'ACTEUR, et un membre sur une autre carte n'en a pas.
  //
  // 🔴🔴 C'ÉTAIT LA DIFFÉRENCE AVEC LA GUILDE. Sa fenêtre montre la tête de
  // tous ses membres en ligne parce que ZC_MEMBERMGR_INFO PORTE la coiffure, sa
  // couleur et le sexe ; les paquets de groupe et d'amis ne portent rien de tel.
  //
  // ⚠ Le repli existe maintenant — `EntityLooks` (CZ 0x0F2E) demande au serveur
  // l'apparence des membres et des amis EN LIGNE — mais il vit ailleurs, et
  // c'est voulu : ce champ-ci ne raconte QUE ce que l'acteur sait, et il le sait
  // tout de suite. Une surface lit l'acteur d'abord, le registre ensuite.
  bool has_look    = false;
  int  hair        = 0;   // id de coiffure BRUT (le remap .spr est dans head_icon)
  int  hair_color  = 0;
  int  sex         = 1;   // 0 = femme
};

// Relit la liste des membres du groupe (avec leurs PV) ou celle des amis.
// Ne lève jamais : une liste en cours de remaniement rend simplement moins
// d'entrées.
void ReadParty(std::vector<Entry>& out);
void ReadFriends(std::vector<Entry>& out);

// Mon AID (`g_Account_Aid`), celui que le natif compare pour reconnaître ma ligne.
uint32_t OwnAid();

// MON entrée, fabriquée depuis mes globales.
//
// 🔴 Hors groupe, la liste du client est VIDE — je n'y figure pas, et il n'y a
// donc rien à lire pour moi. Un affichage qui veut montrer ma tuile en solo ne
// peut que la construire, et il vaut mieux le faire ici qu'à côté de chaque
// rendu : ce sont les mêmes globales, avec les mêmes pièges (le nom est un
// `char[]` NU, et le job est le job RÉEL, pas l'apparence).
//
// ⚠ Ce n'est PAS un membre de groupe : `is_leader` reste faux et `map` porte la
// carte courante. Rend faux tant qu'aucun personnage n'est en jeu.
bool ReadSelfEntry(Entry* out);

// Suis-je le CHEF du groupe ?
//
// 🔴 Relit la liste à chaque appel, et c'est voulu : l'information ne vit nulle
// part ailleurs qu'en `+0x3C` de mon entrée (« 0 = chef »). La tenir dans un
// membre d'une fenêtre revient à la lier au RENDU de cette fenêtre — et une
// surface qui n'est pas affichée cesse alors de la mettre à jour, ce qui a fait
// disparaître « Expulser » du menu de la grille tant que la fenêtre Amis/Groupe
// restait fermée.
bool AmIPartyLeader();

// Ce nom est-il DÉJÀ dans ma liste d'amis ?
//
// Le client a sa propre fonction pour ça (`FriendList_HasNameByCtx`), mais la
// liste est déjà lisible d'ici : on la parcourt, ce qui évite un appel natif et
// reste vrai pour un nom qui n'est pas celui d'un membre présent.
// ⚠ Comparaison EXACTE, dans la code-page du client — c'est ainsi que le
// serveur identifie un personnage.
bool IsFriendByName(const char* name);

// Ce nom est-il DÉJÀ dans mon groupe ? Le pendant de `IsFriendByName`, pour les
// surfaces qui proposent d'inviter : la demande partirait, le serveur la
// refuserait, et le joueur croirait avoir invité quelqu'un.
// ⚠ Comparaison par NOM et non par GID : c'est par le nom que l'invitation
// voyage (CZ 0x02C4), et une entrée d'AMI ne porte pas les mêmes identifiants
// qu'une entrée de groupe.
bool IsPartyMemberByName(const char* name);

// Ce nom est-il dans MA guilde ? Le troisième de la famille, à côté de
// `IsFriendByName` et `IsPartyMemberByName`.
//
// Le roster vit dans l'objet CGuild du client sous forme de liste chaînée
// (`CGuild + 0xdc`) — la feuille de personnage le lisait déjà en entier pour son
// onglet « Guilde », mais en local. Faux d'office quand on n'est dans aucune
// guilde : il n'y a alors pas de roster à parcourir.
bool IsGuildMemberByName(const char* name);

// L'entrée d'un membre du groupe, par GID. Rend false s'il n'y est plus.
// Sert aux surfaces qui n'ont qu'un GID et ont besoin du NOM (les actions de
// groupe voyagent par nom).
bool FindPartyMember(uint32_t gid, Entry* out);
// Nombre de membres, directement au manager (`Social_GetPartyMemberCount`).
int PartyMemberCount();

// Nom de classe affichable d'un job id, MIS EN CACHE (le résolveur traverse la
// table Lua des classes), avec repli « Classe %d » si la table ne connaît pas
// l'id.
//
// 🔴 Vaut pour N'IMPORTE QUEL ROSTER, pas seulement groupe et amis : la feuille
// de personnage l'emploie pour sa liste de GUILDE. C'est la seule chose de cet
// en-tête qui déborde du modèle party/friends, et c'est voulu — l'alternative
// était un troisième cache identique.
//
// 🔴 Nomme un TIERS : passe `rag::kJobSexBase` (99) au résolveur, pas -1. Le
// pourquoi est dans globals.h ; pour sa PROPRE classe, c'est `rag::OwnClassName`.
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
