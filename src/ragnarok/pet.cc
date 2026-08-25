#include "ragnarok/pet.h"

#include <Windows.h>

#include <cstring>

#include "bourgeon.h"
#include "ragnarok/game_scene.h"
#include "ragnarok/globals.h"
#include "ragnarok/uiwnd.h"
#include "ui/ro_imgui.h"
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "ragnarok/stl_node.h"  // rag::treenode

namespace rag {
namespace pet {
namespace {

// ── Le bloc de globals du pet (client 20250716, base 0x400000) ──────────────
// docs/pet_re.md §1. Écrit par `ZC_PROPERTY_PET` (0x01A2, case 418 du dispatch)
// et par `ZC_CHANGESTATE_PET` (0x01A4). Toutes ces valeurs ont été relevées live.
// ⚠ La TÊTE du bloc est publiée dans pet.h : le menu contextuel d'entité en a
// besoin pour reconnaître SON familier parmi les acteurs cliqués. Elle est donc
// reprise d'ici plutôt que réécrite — le reste du bloc, lui, ne sort pas.
constexpr uintptr_t kAid         = kOwnPetAidAddr;
constexpr uintptr_t kRenameFlag  = 0x015fb3b4;  // 0 = renommable une fois
constexpr uintptr_t kName        = 0x015fb3b8;  // char[32] (le paquet n'en donne que 24)
constexpr uintptr_t kAccessory   = 0x015fb3d8;  // ITID ; 0 = aucun
constexpr uintptr_t kClass       = 0x015fb3dc;  // = id de mob
constexpr uintptr_t kLevel       = 0x015fb3e0;
constexpr uintptr_t kHunger      = 0x015fb3e4;
constexpr uintptr_t kIntimacy    = 0x015fb3ec;
constexpr uintptr_t kEggInvIndex = 0x015fb3f0;  // -1 = aucun
constexpr uintptr_t kPrevHunger  = 0x015fb3f4;  // la faim sauvée avant écrasement
constexpr uintptr_t kAutoFeed    = 0x015fb99c;  // écrit par le handler de ZC 0x02D9

// ⚠ `0x015FB3DC` est typé `char[]` dans l'IDB : le pseudo-code l'affiche comme un
// pointeur déréférencé. C'est faux — l'assembleur dit `mov dword_15FB3DC, eax`
// (0x00CBAB6D, 0x00CBACAA). Ne pas propager (docs §1).

// La rédaction de courrier RODEX, testée telle quelle par le msg 39 / id 467.
// La fenêtre que le callback de confirmation (msg 6 / id 488) refuse de doubler,
// c'est l'ÉCHANGE : `uiwnd::kCUIExchangeUI`.
//
// 🔴 Ce commentaire disait « le client la cherche par id, sans jamais nommer la
// classe ». C'était faux, et seul l'annuaire pouvait le montrer : `trade_window`
// avait RE'é 0x271b en live des mois plus tôt. Deux fichiers savaient chacun une
// moitié ; le tri par VALEUR les a mis à la même ligne.

// ── Le manager d'évolution — `CPetEvolutionMgr` ─────────────────────────────
// Instance globale, relevée dans `UIPetInfoWnd_OnCreate` @0x00879b00
// (`mov ecx, dword_1254D8C`). Elle porte DEUX conteneurs distincts, peuplés par
// le même `system\PetEvolutionCln_true.lub` — les confondre est l'erreur que
// docs §8.1 corrige :
//   · +0x08 : la MAP des recettes d'évolution (clé = ITID d'œuf source) ;
//   · +0x18..+0x1C : le VECTEUR plat `AutoFeedingPetList`.
constexpr uintptr_t kEvolutionMgrPtr = 0x01254d8c;
constexpr int kMgr_RecipeMap     = 0x08;  // std::map ; +0 = _Myhead, +4 = _Mysize
constexpr int kMgr_AutoFeedBegin = 0x18;
constexpr int kMgr_AutoFeedEnd   = 0x1c;

// La valeur d'une map est une `pair` : la clé est en tête, et le vecteur suit —
// d'où les deux décalages ci-dessous plutôt que deux valeurs absolues.
constexpr int kNode_VecBegin = rag::treenode::kValue + 0x4;
constexpr int kNode_VecEnd   = rag::treenode::kValue + 0x8;
// Une recette occupe 16 octets et son PREMIER dword est l'ITID de l'œuf CIBLE —
// c'est le seul champ que le menu natif en tire (`*i` de la boucle @0x0088678e,
// avec un `v6 += 4` sur un `_DWORD*`, donc un pas de 16 octets).
constexpr int kRecipeStride = 16;
// ...et les douze octets qui suivent cet ITID sont un `std::vector<pair<int,int>>`
// INLINE : les matériaux de CETTE recette. Relevé dans
// `PetEvolution_LookupRequirements` @0x00631E10, qui les recopie par
// `v9 = rec[1] ; v8 = rec[2] ; ... v9 += 2` — donc begin/end en +4/+8 et des
// paires de huit octets `{itid, quantité}`.
constexpr int kRecipe_MatBegin = 0x04;
constexpr int kRecipe_MatEnd   = 0x08;
constexpr int kMaterialStride  = 8;
// Garde-fou de parcours : un arbre sain ne descend jamais si profond, et une
// map corrompue ne doit pas figer la frame.
constexpr int kTreeWalkGuard = 64;

// ── Fonctions natives ───────────────────────────────────────────────────────
// (`CMode::SendMsg` n'est plus redescendu ici : c'est `rag::RawModeSendMsgSafe`,
// qui fait la même chose — même entrée 6 de la vtable, même lecture BRUTE du
// pointeur de mode, même SEH. Les trois envois de ce fichier la réécrivaient.)
// docs §11 — ITID d'œuf -> classe de mob. 🔴 `__thiscall`, `this` = la SESSION :
// la fonction va chercher l'état Lua en `[ecx+0x59B8]` et appelle le global
// `GetPetJTID_by_PetEggITID`. La déclarer `__cdecl` faisait deux dégâts d'un
// coup — `ecx` non initialisé (donc échec systématique, d'où une classe 0 lue
// « Novice »), ET 4 octets de pile corrompus à chaque appel, la fonction
// finissant par `retn 4`.
// Rend **-1** en cas d'échec, pas 0 (`or ecx,-1` / `cmovnz ecx, out`).
using EggToMobFn = int (__thiscall*)(void*, int);
// `Job_GetDisplayNameOrResName(ctx, class, -1)`, le seul chemin qui respecte les
// overrides `jobName.lub` du client.
using JobNameFn  = const char* (__thiscall*)(void*, int, int);
// `sub_A85BE0` : `retn 4`, aucun usage d'ecx -> __stdcall(const char*). Rend 1
// quand le scan de mots bannis REJETTE le nom (il `setz` le résultat du scan).
using BannedFn   = char (__stdcall*)(const char*);
// `Actor_FindByGid(gid) -> CActor*`, le raccourci global du client : il résout
// lui-même le mode actif (`GameMode_GetActive` puis `ActorList_FindByGID`) et rend
// donc nullptr proprement hors jeu. Le même que celui de l'inspecteur d'entité.
using FindActorFn = void* (__stdcall*)(uint32_t);

constexpr uintptr_t kEggToMobAddr = 0x00d823f0;
// Le `this` de `Job_GetDisplayNameOrResName`, c'est la session — le même objet
// que `g_UIWindowContextKey`. On prend celui de globals.h, pas une redéclaration.

// Les deux chaînes interdites du renommage sont des `std::string` du client
// (msg 2812). SSO MSVC : le tampon est en place tant que la capacité tient sous
// 16, sinon `+0x00` pointe le heap. Capacité en `+0x14`.
constexpr uintptr_t kForbidden1 = 0x0120451c;
constexpr uintptr_t kForbidden2 = 0x01204534;

constexpr int kModeMsgCommandPet = 150;
constexpr int kModeMsgRenamePet  = 145;
constexpr int kModeMsgSelectEgg  = 146;

// `CZ_CONFIG` 0x02D8, 10 octets : [op:2][type:4][value:4]. `type = 2` est
// `CONFIG_PET_AUTOFEED` de `e_config_type` côté serveur (docs §8.2).
constexpr uint32_t kConfigPetAutoFeed = 2;

// `CZ_PET_EVOLUTION` 0x09FB, à longueur variable : [op:2][len:2][eggId:4][items].
// ⚠ `EvolvedPetEggID` fait QUATRE octets sur ce client (variante moderne du
// `#if PACKETVER_RE_NUM >= 20180704` de packets_struct.hpp).
constexpr uint16_t kOpPetEvolution = 0x09fb;

inline int  ReadInt(uintptr_t a) { return *reinterpret_cast<int*>(a); }
inline void* ReadPtr(uintptr_t a) { return *reinterpret_cast<void**>(a); }

// Adaptateur d'ADRESSE : ce fichier désigne les champs par des `uintptr_t`.
const char* StdStringData(uintptr_t s) {
  return rag::clientstr::Data(reinterpret_cast<const void*>(s));
}

// La map de recettes, ou nullptr. Descente d'arbre binaire ordinaire : la clé
// est l'ITID de l'œuf SOURCE.
void* FindRecipeNode(int egg_itid) {
  void* mgr = ReadPtr(kEvolutionMgrPtr);
  if (!mgr) return nullptr;
  auto* map = reinterpret_cast<uint8_t*>(mgr) + kMgr_RecipeMap;
  auto* head = *reinterpret_cast<uint8_t**>(map);
  if (!head) return nullptr;
  // `_Myhead->_Parent` est la RACINE ; l'en-tête lui-même est toujours _Isnil.
  auto* node = *reinterpret_cast<uint8_t**>(head + 0x04);
  for (int guard = 0; node && guard < kTreeWalkGuard; ++guard) {
    if (*reinterpret_cast<uint8_t*>(node + rag::treenode::kIsNil)) return nullptr;
    const int key = *reinterpret_cast<int*>(node + rag::treenode::kValue);
    if (egg_itid == key) return node;
    node = *reinterpret_cast<uint8_t**>(node +
                                        (egg_itid < key ? rag::treenode::kLeft : rag::treenode::kRight));
  }
  return nullptr;
}

}  // namespace

bool DecodeEggCards(const uint32_t* cards, int count, EggCards* out) {
  if (!cards || count < 4 || !out) return false;
  *out = EggCards{};
  // 🔴 Format du FIL, pas celui de la base : `clif_addcards` a déjà décalé le
  // rang et isolé le bit de renommage (cf. le tableau de pet.h). Les slots 0 et 1
  // sont mis à zéro par le serveur — les tester ne dirait rien.
  const int rank = static_cast<int>(cards[2]);
  out->intimacy_rank = (rank >= 1 && rank <= 5) ? rank : 0;
  out->renamed = (cards[3] & 1u) != 0;
  // Un œuf vierge arrive entièrement à zéro ; un œuf habité porte au minimum son
  // rang, que le serveur pose dès la création (`pet_create_egg`) et rafraîchit à
  // chaque retour à l'œuf. Zéro partout = personne dedans.
  out->has_pet = (out->intimacy_rank != 0) || out->renamed;
  return true;
}

int IntimacyRankMsgId(int rank) {
  // Les cinq paliers de `pet_get_card3_intimacy`, dans l'ordre, mappés sur les
  // mêmes msg que le libellé de la fiche (docs §5.3).
  switch (rank) {
    case 1: return 672;  // Awkward
    case 2: return 673;  // Shy
    case 3: return 669;  // Neutral
    case 4: return 674;  // Cordial
    case 5: return 675;  // Loyal
    default: return 0;
  }
}

bool Present() {
  __try {
    // 1. l'AID. 🔴 Il NE REDESCEND JAMAIS À ZÉRO : c'est le premier piège.
    const int aid = ReadInt(kAid);
    if (aid == 0) return false;
    // 2. la classe, le test du client lui-même (`g_Own_PetClass != -1`). Elle ne
    //    retombe que par `Pet_ResetOwnGlobals`, donc seulement quand le sous-type
    //    6 arrive avec data == 0 — c'est-à-dire, côté serveur, quand l'intimité
    //    vaut exactement 1. Sur un retour à l'œuf ordinaire, elle reste.
    if (ReadInt(kClass) <= 0) return false;
    // 3. l'ACTEUR. 🔴 C'est le SEUL signal du chemin ordinaire : `pet_return_egg`
    //    fait `unit_free(pd, CLR_OUTSIGHT)`, et l'entité disparaît de la liste.
    //    `Actor_FindByGid` est le raccourci du client (GameMode actif puis
    //    ActorList_FindByGID) : il rend nullptr proprement hors jeu, là où lire
    //    le mode à la main tomberait sur celui du login.
    return reinterpret_cast<FindActorFn>(gamescene::kFindActorByGidAddr)(
               static_cast<uint32_t>(aid)) != nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadState(State* out) {
  if (!out) return false;
  __try {
    const int aid = ReadInt(kAid);
    const int cls = ReadInt(kClass);
    // Même test que `Present()`, et pour la même raison : l'AID survit au départ
    // du pet, la classe non. Sans lui, la fiche continuerait de dessiner le
    // sprite d'une classe -1 avec les dernières valeurs reçues.
    if (aid == 0 || cls <= 0) return false;
    out->aid         = static_cast<uint32_t>(aid);
    out->cls         = cls;
    out->level       = ReadInt(kLevel);
    out->hunger      = ReadInt(kHunger);
    out->prev_hunger = ReadInt(kPrevHunger);
    out->intimacy    = ReadInt(kIntimacy);
    out->accessory   = ReadInt(kAccessory);
    out->egg_index   = ReadInt(kEggInvIndex);
    out->renameable  = (ReadInt(kRenameFlag) == 0);
    out->auto_feed   = (ReadInt(kAutoFeed) != 0);
    std::memcpy(out->name, reinterpret_cast<const void*>(kName),
                sizeof(out->name));
    out->name[sizeof(out->name) - 1] = '\0';
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// docs §2.5 — `Pet_GetHungerRank` 0x00D82190.
int HungerRank(int hunger) {
  if (hunger >= 91) return 4;
  if (hunger >= 76) return 3;
  if (hunger >= 26) return 2;
  if (hunger >= 11) return 1;
  return 0;
}

// docs §2.5 — `Pet_GetIntimacyRank` 0x00D82050.
int IntimacyRank(int intimacy) {
  if (intimacy > 900) return 4;
  if (intimacy > 750) return 3;
  if (intimacy > 250) return 2;
  if (intimacy > 100) return 1;
  return 0;
}

int IntimacyMsgId(int intimacy, int cls) {
  // ⚠ Cas particulier de la MONTURE Doram (classes 4050..4052) sous 100
  // d'intimité : le client change de barème (docs §5.3).
  if (cls >= 4050 && cls <= 4052 && intimacy <= 100) {
    if (intimacy <= 20) return 1022;
    if (intimacy <= 40) return 1021;
    return 672;
  }
  if (intimacy <= 100)  return 672;   // 0x2A0
  if (intimacy <= 250)  return 673;   // 0x2A1
  if (intimacy <= 750)  return 669;   // 0x29D — « Neutral », relevé live à 400
  if (intimacy <= 900)  return 674;   // 0x2A2
  if (intimacy <= 1000) return 675;   // 0x2A3
  return 676;                         // 0x2A4
}

bool EggAutoFeedListed(int egg_itid) {
  if (egg_itid == 0) return false;
  __try {
    void* mgr = ReadPtr(kEvolutionMgrPtr);
    if (!mgr) return false;
    auto* base = reinterpret_cast<uint8_t*>(mgr);
    auto* it  = *reinterpret_cast<int**>(base + kMgr_AutoFeedBegin);
    auto* end = *reinterpret_cast<int**>(base + kMgr_AutoFeedEnd);
    for (int guard = 0; it && it != end && guard < 4096; ++it, ++guard)
      if (*it == egg_itid) return true;
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int EvolutionTargets(int egg_itid, int* out, int max_out) {
  if (!out || max_out <= 0 || egg_itid == 0) return 0;
  __try {
    auto* node = reinterpret_cast<uint8_t*>(FindRecipeNode(egg_itid));
    if (!node) return 0;
    auto* it  = *reinterpret_cast<uint8_t**>(node + kNode_VecBegin);
    auto* end = *reinterpret_cast<uint8_t**>(node + kNode_VecEnd);
    int count = 0;
    while (it && it < end && count < max_out) {
      out[count++] = *reinterpret_cast<int*>(it);
      it += kRecipeStride;
    }
    return count;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

int EvolutionMaterials(int src_egg_itid, int dst_egg_itid, EvolutionMaterial* out,
                       int max_out) {
  if (!out || max_out <= 0 || src_egg_itid == 0 || dst_egg_itid == 0) return 0;
  __try {
    auto* node = reinterpret_cast<uint8_t*>(FindRecipeNode(src_egg_itid));
    if (!node) return 0;
    auto* rec = *reinterpret_cast<uint8_t**>(node + kNode_VecBegin);
    auto* rec_end = *reinterpret_cast<uint8_t**>(node + kNode_VecEnd);
    while (rec && rec < rec_end && *reinterpret_cast<int*>(rec) != dst_egg_itid)
      rec += kRecipeStride;
    if (!rec || rec >= rec_end) return 0;
    // L'enregistrement porte le vecteur de matériaux INLINE, juste après l'ITID
    // cible : begin en +4, end en +8 (cap en +12). Paires {itid, quantité} de
    // huit octets — c'est exactement la boucle `v9 += 2` de la fonction native.
    auto* it  = *reinterpret_cast<uint8_t**>(rec + kRecipe_MatBegin);
    auto* end = *reinterpret_cast<uint8_t**>(rec + kRecipe_MatEnd);
    int count = 0;
    while (it && it < end && count < max_out) {
      out[count].itid   = *reinterpret_cast<int*>(it);
      out[count].amount = *reinterpret_cast<int*>(it + 4);
      ++count;
      it += kMaterialStride;
    }
    return count;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool SendEvolution(int dst_egg_itid) {
  if (dst_egg_itid == 0) return false;
  uint8_t pkt[8];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpPetEvolution;
  *reinterpret_cast<uint16_t*>(pkt + 2) = sizeof(pkt);  // paquet à longueur VARIABLE
  *reinterpret_cast<uint32_t*>(pkt + 4) = static_cast<uint32_t>(dst_egg_itid);
  return Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

int EggItidToMobClass(int egg_itid) {
  if (egg_itid == 0) return 0;
  __try {
    const int cls = reinterpret_cast<EggToMobFn>(kEggToMobAddr)(rag::Session(),
                                                                egg_itid);
    // -1 = le Lua n'a rien rendu. On le remonte comme 0 (« inconnu ») plutôt que
    // de le laisser filer vers `Job_GetDisplayNameOrResName`, dont la table
    // rendrait un nom pour n'importe quel index — c'est comme ça qu'un échec
    // s'affichait « Novice ».
    return cls > 0 ? cls : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

const char* MobDisplayNameUtf8(int mob_class) {
  const char* raw = nullptr;
  __try {
    // Un MONSTRE n'a pas de variante de sexe : l'argument est inerte ici.
    // On garde donc `kJobSexSelf` (le -1 historique) plutot que de basculer
    // sur le nom de base sans preuve que la table reponde pareil.
    raw = rag::JobNameForSex(mob_class, rag::kJobSexSelf);
  } __except (EXCEPTION_EXECUTE_HANDLER) { raw = nullptr; }
  return (raw && raw[0]) ? ro::LocalToUtf8(raw) : "";
}

bool MailWriteOpen() {
  __try {
    return ReadPtr(uiwnd::kMailWriteWndSlot) != nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool FeedBlockerWindowOpen() {
  __try {
    return uiwnd::FindWindow(uiwnd::kCUIExchangeUI) != nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool NameHasForbiddenWord(const char* wire_name) {
  if (!wire_name || !wire_name[0]) return false;
  __try {
    const char* a = StdStringData(kForbidden1);
    const char* b = StdStringData(kForbidden2);
    if (a && a[0] && std::strstr(wire_name, a)) return true;
    if (b && b[0] && std::strstr(wire_name, b)) return true;
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool NameHasBannedWord(const char* wire_name) {
  if (!wire_name || !wire_name[0]) return false;
  __try {
    return reinterpret_cast<BannedFn>(rag::kBannedWordContainsAddr)(wire_name) == 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SendCommand(int cmd) {
  return rag::RawModeSendMsgSafe(kModeMsgCommandPet, cmd);
}

bool SendRename(const char* wire_name) {
  if (!wire_name || !wire_name[0]) return false;
  if (!rag::RawModeSendMsgSafe(
          kModeMsgRenamePet,
          static_cast<int>(reinterpret_cast<intptr_t>(wire_name))))
    return false;
  // Le natif recopie ensuite le nom dans le global. Sans ça la fiche
  // afficherait l'ancien jusqu'au prochain `ZC_PROPERTY_PET`.
  __try {
    auto* dst = reinterpret_cast<char*>(kName);
    std::strncpy(dst, wire_name, kNameMaxBytes - 1);
    dst[kNameMaxBytes - 1] = '\0';
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SendSelectEgg(int inv_index) {
  if (inv_index < 0) return false;
  return rag::RawModeSendMsgSafe(kModeMsgSelectEgg, inv_index);
}

void SendAutoFeed(bool on) {
  uint8_t pkt[10];
  *reinterpret_cast<uint16_t*>(pkt + 0) = Bourgeon::kOpConfig;
  *reinterpret_cast<uint32_t*>(pkt + 2) = kConfigPetAutoFeed;
  *reinterpret_cast<uint32_t*>(pkt + 6) = on ? 1u : 0u;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}


}  // namespace pet
}  // namespace rag
