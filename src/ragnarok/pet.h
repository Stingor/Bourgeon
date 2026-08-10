#pragma once

// ── Le pet : état, gardes du client et actions ───────────────────────────────
//
// Comme l'homoncule, le pet n'est rangé dans AUCUN objet : tout son état vit
// dans un bloc de globals plats à partir de `0x015FB3B0`, écrit par les
// décodeurs de `ZC_PROPERTY_PET` (0x01A2) et `ZC_CHANGESTATE_PET` (0x01A4).
// RE complet : docs/pet_re.md.
//
// POURQUOI UN MODULE. Deux consommateurs déjà : la fiche ImGui (PetWindow, qui
// remplace `UIPetInfoWnd` id 88 et son menu de commandes id 260) et le menu
// contextuel d'entité, qui doit savoir reconnaître SON pet. Le même raisonnement
// que `homunculus.h` — recopier les offsets des deux côtés est exactement ce que
// l'annuaire d'adresses a coûté à nettoyer.
//
// 🔴 REJOUER, PAS RÉÉCRIRE. Les actions ne fabriquent pas de paquets : elles
// passent par `CMode::SendMsg(150, cmd)`, le point d'entrée du client. C'est lui
// qui porte le gate « pas de nourriture en sac » (docs §4.4) — un remplaçant qui
// émettrait `CZ 0x01A1` directement changerait le comportement observable.
// Corollaire : `SendCommand` peut ouvrir une boîte native, donc JAMAIS pendant
// une frame ImGui (cf. [[feedback_no_native_cmd_during_imgui_frame]]).
//
// ⚠ En-tête volontairement sans <Windows.h> : les lectures sont protégées par
// SEH, et un `__try` est interdit dans toute fonction abritant un objet à
// destructeur non trivial (C2712). Les corps vivent donc dans le .cc.

#include <cstdint>

namespace rag {
namespace pet {

// Commandes de `CZ_COMMAND_PET` (0x01A1), telles que le case 150 de
// `CMode::SendMsg` les attend (docs §4.4).
constexpr int kCmdInfo         = 0;
constexpr int kCmdFeed         = 1;
constexpr int kCmdPerform      = 2;
constexpr int kCmdToEgg        = 3;
constexpr int kCmdAccessoryOff = 4;

// Le client refuse l'évolution en dessous de ce seuil, AVANT d'ouvrir sa fenêtre
// (`UIPetInfoWnd::OnMsg` msg 39 / id 471). Le serveur refait le test à sa façon
// (`intimate < PET_INTIMATE_LOYAL`).
constexpr int kEvolutionMinIntimacy = 900;

// Borne de nom du client : le champ d'édition natif accepte 50 caractères, mais
// le msg 6/184 REFUSE au-delà de 23 (`strlen >= 0x18`). C'est cette borne-là qui
// compte, et le paquet ne transporte de toute façon que 24 octets.
constexpr int kNameMaxBytes = 24;   // 23 caractères + terminateur

// Plafond de lecture de `EvolutionTargets`. Le `.lub` livré n'en déclare qu'une
// poignée par œuf ; c'est un garde-fou de tampon, pas une règle de jeu.
constexpr int kMaxEvolutions = 16;

// Fiche d'état, POD pur.
struct State {
  uint32_t aid = 0;         // AID/GID de l'entité pet
  int  cls = 0;             // classe = id de mob
  int  level = 0;
  int  hunger = 0;          // 0..100
  int  prev_hunger = 0;     // la faim AVANT le dernier ZC_CHANGESTATE_PET
  int  intimacy = 0;        // 0..1000
  int  accessory = 0;       // ITID de l'accessoire porté ; 0 = aucun
  int  egg_index = -1;      // index d'INVENTAIRE de l'œuf ; -1 = aucun
  bool renameable = false;  // `rename_flag == 0` : le renommage n'a pas servi
  bool auto_feed = false;
  char name[32] = {};       // encodage RÉSEAU (CP949), pas UTF-8
};

// ── L'ŒUF, vu comme un OBJET ────────────────────────────────────────────────
// Cet ITID est-il un œuf de familier ? Le client tranche sur une simple PLAGE
// codée en dur — `IsPetEggItem` (0x00D8EBE0) tient en cinq instructions et ne
// fait que `9001 <= id <= 9499`. On la reproduit plutôt que d'appeler : pas de
// convention à se tromper, et utilisable dans un `constexpr`.
constexpr bool IsEggItem(uint32_t itid) { return itid >= 9001 && itid <= 9499; }

// Les quatre « cartes » d'un œuf ne sont PAS des cartes : c'est la fiche du pet
// qu'il contient. 🔴 Mais ce que le CLIENT en reçoit n'est pas ce que la base du
// serveur en garde — `clif_addcards` (moonlight clif.cpp:2864) RÉÉCRIT les
// quatre slots avant de les mettre sur le fil :
//
//   base serveur                          -> ce qui arrive au client
//   card[0] = CARD0_PET (0x0100)             card[0] = 0
//   card[1] = mot bas du pet_id              card[1] = 0
//   card[2] = mot haut du pet_id             card[2] = card[3] >> 1  (rang 1..5)
//   card[3] = renommé | rang << 1            card[3] = card[3] & 1   (renommé)
//
// Conséquences, toutes vérifiées côté serveur :
//   · `CARD0_PET` n'atteint JAMAIS le client — le chercher pour reconnaître un
//     œuf habité rend systématiquement « œuf vierge » ;
//   · le `pet_id` non plus : aucun paquet ne le porte, il n'y a rien à afficher ;
//   · les deux slots restants sont des NOMBRES, pas des ITID : les passer au
//     rendu de cartes donne « #5 » et le nom de l'objet d'id 1.
// Un œuf VIERGE (obtenu par `@item`) a ses quatre slots à zéro côté serveur et
// prend le chemin ordinaire de `clif_addcards` : il arrive donc à zéro lui aussi.
// C'est ce qui distingue les deux.
struct EggCards {
  bool has_pet = false;       // un pet est bien dedans (rang ou renommage non nuls)
  bool renamed = false;
  int  intimacy_rank = 0;     // 1..5 ; 0 = inconnu
};
// `cards` = les quatre slots tels que l'inventaire les porte. Rend false si on
// ne lui a pas donné de quoi décider.
bool DecodeEggCards(const uint32_t* cards, int count, EggCards* out);

// Le libellé msgstringtable du rang rendu par `DecodeEggCards` — le MÊME que
// celui de la fiche, pour que l'œuf et le pet ne disent pas deux choses
// différentes de la même intimité. 0 si le rang est inconnu.
int IntimacyRankMsgId(int rank);

// Un pet est-il sorti ? Trois conditions, et il en faut TROIS.
//
// 🔴 PAS `g_Own_PetAid != 0` : cet AID ne redescend jamais à zéro. Le seul
// endroit qui nettoie quoi que ce soit est `Pet_ResetOwnGlobals` (0x00D8A830),
// qui ne remet que `g_Own_PetEggInvIndex` et `g_Own_PetClass` à -1 — et c'est
// bien la CLASSE que le client interroge (`if (g_Own_PetClass != -1)`,
// 0x00CBB416).
//
// 🔴 …ET PAS LA CLASSE NON PLUS, sur le chemin ordinaire. `Pet_ResetOwnGlobals`
// n'est appelé que par le sous-type 6 de `ZC_CHANGESTATE_PET` avec **data == 0**,
// or `clif_send_petdata` calcule `data = (intimate == 1) ? 0 : 1` : un pet rangé
// dans son œuf avec une intimité normale envoie donc **data = 1**, dont la
// branche cliente ne fait quelque chose QUE si la fenêtre 90 est ouverte. Sur un
// retour à l'œuf ordinaire, le client ne réinitialise RIEN — et sa propre fiche
// 88 reste elle aussi à l'écran sur un pet absent. Ce n'est pas une régression
// qu'on introduit, c'est un manque qu'on comble.
//
// Le seul observable qui reste est donc **l'ACTEUR** : `unit_free(pd,
// CLR_OUTSIGHT)` le fait disparaître, et `Actor_FindByGid` rend nullptr.
// ⚠ Un changement de map vide la liste d'acteurs pendant un aller-retour réseau
// avant que le pet ne réapparaisse : un appelant qui FERME quelque chose là-dessus
// doit exiger une absence CONTINUE (cf. PetWindow::OnTick).
bool Present();
bool ReadState(State* out);

// Les deux paliers du client (docs §2.5), tels que le bavardage spontané et la
// fiche les emploient.
int HungerRank(int hunger);      // 0..4
int IntimacyRank(int intimacy);  // 0..4

// L'id msgstringtable du libellé d'intimité, cas particulier des montures Doram
// compris (`UIPetInfoWnd_RefreshIntimacyLabel` 0x00888F00, docs §5.3).
int IntimacyMsgId(int intimacy, int cls);

// ── L'œuf porté ─────────────────────────────────────────────────────────────
// ⚠ L'ITID de l'œuf ne se lit PAS ici : il vient de l'index d'inventaire
// `State::egg_index`, donc de la liste d'objets — couche `features/item_cell`,
// pas `ragnarok/`. Voir `EggItid()` dans features/windows/pet_window.cc.
// 🔴 Et cet index vaut **-1 tant que le pet est sorti** (relevé live). Le natif
// interroge quand même, obtient une fiche vide, et c'est POUR ÇA que son bouton
// d'auto-feeding et sa liste d'évolutions sont si souvent absents. On reproduit
// sans corriger — le droit à l'auto-feeding est une donnée du client, la deviner
// serait inventer.

// Cet œuf figure-t-il dans `AutoFeedingPetList` ? C'est ce que teste
// `PetAutoFeeding_IsEggListed` (0x00632610), et c'est CETTE liste — pas les
// recettes d'évolution — qui décide de l'existence du bouton (docs §8.1).
// Réimplémenté plutôt qu'appelé : la fonction n'est qu'un balayage du vecteur
// plat `CPetEvolutionMgr+0x18..+0x1C`, et le lire nous-mêmes évite un appel.
bool EggAutoFeedListed(int egg_itid);

// Les œufs vers lesquels `egg_itid` peut évoluer, lus dans la map de recettes
// `CPetEvolutionMgr+0x08`. Rend le nombre écrit dans `out` (0 = aucune recette).
int EvolutionTargets(int egg_itid, int* out, int max_out);

// Un matériau d'une recette : l'objet et la quantité exigée.
struct EvolutionMaterial {
  int itid = 0;
  int amount = 0;
};
constexpr int kMaxEvolutionMaterials = 16;

// Les matériaux du couple (œuf source -> œuf cible). Lus DIRECTEMENT dans la
// recette : chaque enregistrement de 16 octets porte `{ITID cible, puis un
// std::vector<pair<int,int>> begin/end/cap}`, et c'est ce vecteur que
// `PetEvolution_LookupRequirements` (0x00631E10) se contente de recopier. On lit
// la source plutôt que d'appeler la copie — cette fonction rend un vecteur PAR
// VALEUR, qu'il faudrait ensuite libérer sur le tas du client.
// ⚠ C'est la table du CLIENT (`PetEvolutionCln_true.lub`). Le serveur revalide
// avec la sienne (`pet_db.yml`) : une divergence donne une recette affichée qui
// échoue en `FAIL_RECIPE`.
int EvolutionMaterials(int src_egg_itid, int dst_egg_itid, EvolutionMaterial* out,
                       int max_out);

// 🔴 Seuil d'intimité du SERVEUR (`PET_INTIMATE_LOYAL`, pet.hpp). Il ne coïncide
// PAS avec celui du client (`kEvolutionMinIntimacy`, > 900) : entre 901 et 909,
// le client proposait l'évolution et le serveur la refusait — avec un message
// qui parle d'intimité, au moins, mais sans dire qu'il manquait neuf points.
constexpr int kEvolutionServerMinIntimacy = 910;

// `CZ_PET_EVOLUTION` 0x09FB. ✅ Le serveur n'utilise QUE l'ITID de l'œuf cible :
// `clif_parse_pet_evolution` (moonlight clif.cpp:25338) lit `EvolvedPetEggID` et
// appelle `pet_evolution`, qui relit les matériaux dans SA base. La liste que le
// natif recopiait derrière l'en-tête n'est jamais parcourue — on envoie donc le
// paquet minimal, long de 8, plutôt que de reproduire une conversion que
// personne ne lit (docs/pet_re.md §7.2 la donnait d'ailleurs pour non tracée).
bool SendEvolution(int dst_egg_itid);

// ITID d'œuf -> classe de mob (`PetEgg_ItidToMobClass` 0x00D823F0), et le nom
// affichable d'une classe. Le second rend un tampon ROTATIF (cf. ro::LocalToUtf8) :
// à recopier s'il doit survivre à la frame.
int EggItidToMobClass(int egg_itid);
const char* MobDisplayNameUtf8(int mob_class);

// ── Les gardes purement CLIENT ──────────────────────────────────────────────
// 🔴 Elles ne sont revalidées NI par `SendMsg(150, …)`, NI par le serveur : elles
// vivaient dans `UIPetInfoWnd::OnMsg`, la fenêtre qu'on remplace. Les perdre,
// c'est laisser partir des demandes que le client d'origine n'émettait jamais
// (cf. [[feedback_replaced_handler_hidden_duties]]).

// La rédaction de courrier est ouverte -> « nourrir » est refusé (msg 2985).
bool MailWriteOpen();
// La fenêtre 10011 existe -> « nourrir » est refusé au moment de CONFIRMER
// (msg 3550). Deux gardes distinctes à deux instants distincts, comme le natif.
bool FeedBlockerWindowOpen();
// Le nom contient l'une des deux chaînes interdites du client (msg 2812).
bool NameHasForbiddenWord(const char* wire_name);
// Le scan de mots bannis du client rejette ce nom (msg 2932).
bool NameHasBannedWord(const char* wire_name);

// ── Actions ─────────────────────────────────────────────────────────────────
// 🔴 Toutes HORS frame ImGui : `SendCommand` peut ouvrir une boîte native.
bool SendCommand(int cmd);
// `CMode::SendMsg(145, texte)` -> `CZ_RENAME_PET` 0x01A5. `wire_name` est en
// encodage RÉSEAU, pas en UTF-8. Recopie aussi le nom dans le global, comme le
// natif — sinon la fiche continuerait d'afficher l'ancien jusqu'au prochain
// `ZC_PROPERTY_PET`.
bool SendRename(const char* wire_name);
// `CZ_CONFIG` 0x02D8 type 2. On n'écrit RIEN localement : le serveur répond par
// `ZC 0x02D9`, que le client applique lui-même sur son global — sinon la case
// mentirait sur un refus. Même raisonnement que l'auto-alimentation de l'homoncule.
void SendAutoFeed(bool on);

// ── L'éclosion ──────────────────────────────────────────────────────────────
// `CMode::SendMsg(146, index)` → `CZ_SELECT_PETEGG` 0x01A7 `[op:2][index:2]`.
// `inv_index` est l'index d'inventaire tel que `ZC_PETEGG_LIST` (0x01A6) l'a
// donné : `clif_sendegg` envoie `client_index(i)` = i+2, le client le range tel
// quel dans `ItemInfo+0x04`, et `clif_parse_SelectEgg` retranche les 2. On
// renvoie donc EXACTEMENT ce qu'on a reçu — c'est ce que fait
// `UIPetEggListWnd::OnMsg` (msg 6 / id 184), qui passe le `+0x04` de l'entrée.
// ⚠ Le serveur exige `menuskill_id == SA_TAMINGMONSTER` : hors du contexte
// ouvert par `@hatch`, le paquet est ignoré en silence.
bool SendSelectEgg(int inv_index);

}  // namespace pet
}  // namespace rag
