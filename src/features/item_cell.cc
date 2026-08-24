#include "features/item_cell.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "imgui.h"
#include "ragnarok/globals.h"  // rag::kGameOperatorDeleteAddr
#include "ragnarok/item_info.h"  // rag::itemlist : le layout du noeud
#include "ragnarok/item_db.h"
#include "ragnarok/uiwnd.h"
#include "ui/ro_imgui.h"
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "utils/text.h"  // text::Base62Digit

namespace itemcell {
namespace {

// BuildDisplayName alloue son vecteur de décalages avec l'allocateur du client :
// à rendre au même (rag::kGameOperatorDeleteAddr).

// std::vector MSVC tel que le jeu le passe (3 pointeurs).
struct GVec { int* first; int* last; int* end; };

using BuildName_t   = int   (__thiscall*)(void*, void*, int*, GVec*, char**,
                                          size_t*, char**, char, char);
using GetBaseName_t = size_t(__thiscall*)(void*, char*, size_t*, char);
using GameFree_t    = void  (__cdecl*)(void*);

using InfoCtor_t     = void(__fastcall*)(void*);
using InfoSetId_t    = void(__thiscall*)(void*, int);
using EnsureLoaded_t = char(__thiscall*)(void*, int);
using DescLookup_t   = void*(__cdecl*)(int, void*);

// Décalage du nom de base dans l'enregistrement de la DB de descriptions.
constexpr int kDescRecName = 0x04;

// Cache de noms, un seul pour tout le client. Il remplace six caches privés qui
// résolvaient et stockaient chacun les mêmes chaînes.
std::unordered_map<uint32_t, std::string> g_name_cache;

// SEH ISOLÉ et POD SEULEMENT : aucune std::string dans cette portée, sinon MSVC
// refuse le __try (C2712, objet à destructeur déroulable). D'où le passage par
// `out` plutôt qu'un retour de chaîne.
void ResolveNameSEH(uint32_t id, char* out, size_t cap) {
  out[0] = '\0';
  __try {
    // La DB n'est peuplée qu'à la demande : sans ce coup de pouce, le premier
    // affichage d'un item jamais consulté rendrait « #<id> ».
    void* cache = *reinterpret_cast<void**>(itemdb::kEnsureCachePtr);
    if (cache)
      reinterpret_cast<EnsureLoaded_t>(itemdb::kEnsureLoadedAddr)(
          cache, static_cast<int>(id));
    void* rec = reinterpret_cast<DescLookup_t>(itemdb::kLookupAddr)(
        static_cast<int>(id), reinterpret_cast<void*>(itemdb::kTableAddr));
    // kNilAddr est la sentinelle de l'arbre : y aboutir signifie « absent », la
    // déréférencer lirait le nœud bidon.
    if (rec && rec != reinterpret_cast<void*>(itemdb::kNilAddr)) {
      const char* nm =
          *reinterpret_cast<char**>(reinterpret_cast<char*>(rec) + kDescRecName);
      if (nm) { std::strncpy(out, nm, cap - 1); out[cap - 1] = '\0'; }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

// ── std::list<ItemSkillInfo> de session : nœud et champs lus ──────────────────
// Nœud MSVC : next@+0, prev@+4, value@+8 — `value` EST l'ItemSkillInfo.
using rag::itemlist::kNodeNext;
using rag::itemlist::kNodeInfo;
using rag::itemlist::kInfoIndex;
using rag::itemlist::kInfoIdStr;
using rag::itemlist::kInfoAmount;
using rag::itemlist::kWalkGuard;
// ItemSkillInfo tel que le natif le passe à OnMsg 0x18. 0x100 octets : la
// structure en fait 0xf8, on arrondit (cf. le memcpy 0x5c..0xf8 du pont `src`).
constexpr size_t kInfoSize = 0x100;

// La demande d'ouverture en vol (cf. l'en-tête pour le pourquoi du différé).
// `list_head` != 0 => par index ; sinon `id` != 0 => par id ; les deux nuls =>
// rien en attente. Écrasée par toute nouvelle demande : une souris, un geste.
struct DeferredDesc {
  uintptr_t   list_head;
  int         index;
  uint32_t    id;
  uint16_t    view;
  uint32_t    location;
  const void* src;
  int         mx, my;
  // Lien de chat : COPIÉ, pas pointé. L'objet n'appartient à personne ici (c'est
  // celui d'un autre joueur), il n'y a donc rien à garder en vie — et rien qui
  // puisse mourir entre le clic et le relâchement.
  ChatLink    link;
  bool        from_link;
};
DeferredDesc g_deferred = {};

// ── Le nom du client -> UTF-8, sans couper un caractère en deux ─────────────
//
// 🔴🔴 LE DÉFAUT QUE CECI CORRIGE ÉTAIT LÀ DEPUIS TOUJOURS, INVISIBLE.
// `BuildDisplayName` écrit dans la CODE-PAGE DU CLIENT ; `itemcell::NameText`
// et `DrawTooltip` attendent de l'UTF-8 — c'est écrit dans leur signature. Les
// trois viewers passaient l'un à l'autre sans conversion. Tant que tous les noms
// d'objets étaient de l'ASCII anglais, les deux encodages coïncidaient et rien
// ne se voyait.
//
// La traduction de la MsgStringTable a mis le premier octet accentué dans un nom
// d'objet : `MSI_NAMED_PET` = « Bien-aimé », que le client PRÉFIXE au nom d'un
// œuf de familier. Résultat à l'écran : « Bien-aim◆ring Egg ». ImGui lit l'octet
// `0xE9` comme la tête d'une séquence UTF-8 de trois octets, avale « é », « P »
// et « o » d'un coup, et rend un carré. La traduction n'a pas créé le bug, elle
// l'a RÉVÉLÉ.
//
// ⚠ La conversion FAIT GROSSIR : un accent latin passe de 1 à 2 octets, un
// caractère coréen de 2 à 3. D'où un tampon d'arrivée plus large que celui du
// client — et, si ça déborde quand même, une troncature sur une FRONTIÈRE de
// caractère. Couper au milieu d'une séquence rendrait exactement le carré qu'on
// vient de faire disparaître.
// Coupe la DERNIÈRE séquence UTF-8 si elle est incomplète — et seulement dans ce
// cas. ⚠ Le piège de cette fonction est d'être trop zélée : reculer dès qu'on
// voit un octet ≥ 0x80 amputerait un accent PARFAITEMENT VALIDE en fin de
// chaîne. On remonte donc jusqu'à l'octet de TÊTE, on lit la longueur qu'il
// annonce, et on ne tranche que si elle dépasse ce qui reste.
void TrimIncompleteUtf8(char* s) {
  if (!s) return;
  size_t n = std::strlen(s);
  size_t start = n;
  while (start > 0 &&
         (static_cast<unsigned char>(s[start - 1]) & 0xC0) == 0x80) --start;
  if (start == 0) return;
  --start;  // l'octet de tête du dernier caractère
  const unsigned char lead = static_cast<unsigned char>(s[start]);
  size_t need = 1;
  if      ((lead & 0xE0) == 0xC0) need = 2;
  else if ((lead & 0xF0) == 0xE0) need = 3;
  else if ((lead & 0xF8) == 0xF0) need = 4;
  if (start + need > n) s[start] = '\0';
}

void CopyNameAsUtf8(const char* local, char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  out[0] = '\0';
  if (!local || !local[0]) return;

  const char* utf8 = ro::LocalToUtf8(local);
  if (!utf8) return;

  size_t n = std::strlen(utf8);
  if (n >= out_size) {
    n = out_size - 1;
    // Reculer tant qu'on est sur un octet de CONTINUATION (10xxxxxx) : la
    // frontière est le premier octet qui n'en est pas un.
    while (n > 0 && (static_cast<unsigned char>(utf8[n]) & 0xC0) == 0x80) --n;
  }
  std::memcpy(out, utf8, n);
  out[n] = '\0';
}

}  // namespace

void BuildDisplayName(void* info, char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  out[0] = '\0';
  // Le nom tel que le CLIENT le compose, dans SA code-page. Il ne sortira pas
  // d'ici sous cette forme — voir la conversion en fin de fonction.
  char local[128];
  local[0] = '\0';
  // SEH ISOLÉ, et c'est le point important : un item dont le name-builder plante
  // ne doit pas avorter TOUTE l'énumération. Leçon de l'inventaire, où un seul
  // item fautif faisait disparaître la moitié de la liste.
  __try {
    char nbuf[128]; nbuf[0] = '\0';
    char* bufptr = nbuf; size_t ncap = sizeof(nbuf);
    int color_out = 0; char* hl_ptr = nullptr;
    GVec offsets = {nullptr, nullptr, nullptr};
    // `this` = le GESTIONNAIRE, jamais une fenêtre : c'est lui qui porte la liste
    // de requêtes de noms lue à +0x18C (raisonnement complet dans item_cell.h).
    reinterpret_cast<BuildName_t>(itemdb::kBuildDisplayNameAddr)(
        uiwnd::Mgr(), info, &color_out, &offsets, &bufptr, &ncap, &hl_ptr, 0, 0);
    size_t k = 0;
    while (k + 1 < sizeof(local) && nbuf[k]) { local[k] = nbuf[k]; ++k; }
    local[k] = '\0';
    if (offsets.first) reinterpret_cast<GameFree_t>(rag::kGameOperatorDeleteAddr)(offsets.first);
    if (local[0] == '\0') {
      size_t cap = sizeof(local);
      reinterpret_cast<GetBaseName_t>(itemdb::kBaseNameFallbackAddr)(info, local,
                                                                    &cap, 0);
      local[sizeof(local) - 1] = '\0';
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { local[0] = '\0'; }

  // 🔴 LA CONVERSION EST ICI, PAS CHEZ L'APPELANT — et c'est le fond du sujet.
  //
  // Elle a longtemps été « la responsabilité de l'appelant », documentée en
  // toutes lettres dans item_cell.h. Résultat mesuré : sur ONZE sites, six ne
  // convertissaient pas du tout et cinq employaient `WireToUtf8`, qui décrit
  // l'encodage du FIL et non celui du client. Un seul site le faisait comme il
  // faut. Une règle que la quasi-totalité des appelants enfreint n'est pas une
  // règle, c'est un piège — d'autant qu'elle reste INVISIBLE tant que les noms
  // sont en ASCII, ce qu'ils étaient jusqu'à ce que la traduction de la
  // MsgStringTable glisse « Bien-aimé » devant un nom d'œuf de familier.
  //
  // En la posant ici, aucun appelant ne peut plus l'oublier, ni se tromper de
  // convertisseur. Le contrat devient : `out` est de l'UTF-8, prêt pour ImGui.
  //
  // ⚠ La conversion FAIT GROSSIR le texte (un accent latin passe de 1 à 2
  // octets) : `CopyNameAsUtf8` tronque donc sur une FRONTIÈRE de caractère,
  // jamais au milieu d'une séquence.
  CopyNameAsUtf8(local, out, out_size);
}

int SlotCount(void* info) {
  if (!info) return 0;
  __try {
    return reinterpret_cast<int(__fastcall*)(void*)>(itemdb::kSlotCountAddr)(info);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ── Lien d'objet pour le chat (`<ITEML>…</ITEML>`) ───────────────────────────
// Le client a UN endroit où il forge ce texte : `UIWnd_AppendItemLinkButton
// 0x00865230`, appelée par `UIChatWnd_InsertItemLink 0x008217f0` (le Maj+clic de
// l'inventaire, cf. `UIInventoryWnd_OnLButtonDown 0x0094afb0`). On le rejoue
// champ par champ plutôt que de l'appeler : sa signature réelle est
// `(UIItemTagOnChat*, info, police)` — elle CRÉE un `UIItemTagButton` de 0x200
// octets et l'accroche à la fenêtre qui a le focus. Sans chatbox native, il n'y a
// plus ni accessoire ni fenêtre à qui l'accrocher.
//
// La table des séparateurs du client (`"!#$%&'()*+,-/"`, indices 3/4/5/7/9/10/11)
// et celle de rAthena (`ItemDatabase::create_item_link`, src/map/itemdb.cpp)
// coïncident exactement, PACKETVER ≥ 20200724 :
//   %=refine  &=viewID  '=grade  )=carte  +=id d'option  ,=paramètre  -=valeur
//
// Deux écarts ASSUMÉS entre le client et le serveur, tranchés en faveur du
// CLIENT — c'est lui la référence pour ce geste, c'est son texte que reçoivent
// les autres joueurs :
//   · le champ `&` (viewID) est écrit MÊME sur un objet non équipable ;
//   · les cartes sont émises `SlotCount` fois (et non 4 d'office), sauf si l'une
//     des quatre est non nulle.
namespace {

constexpr char kB62[] =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

// base62 du client (`0x00842b30`), poids fort en tête. Sa boucle
// `do { … } while (v >= 62)` émet toujours un chiffre AVANT le dernier : le
// résultat ne fait donc JAMAIS moins de deux caractères, ce qui est le
// `string_left_pad(base62_encode(v), '0', 2)` de rAthena. `min_width` sert au
// seul champ qui demande plus : l'emplacement d'équipement, cadré sur 5.
void AppendB62(char* out, size_t cap, size_t* len, uint32_t value, int min_width) {
  char digits[16];
  int n = 0;
  do {
    digits[n++] = kB62[value % 62u];
    value /= 62u;
  } while (value != 0 && n < 12);
  while (n < min_width && n < 12) digits[n++] = '0';
  while (n > 0 && *len + 1 < cap) out[(*len)++] = digits[--n];
}

void AppendChar(char* out, size_t cap, size_t* len, char c) {
  if (*len + 1 < cap) out[(*len)++] = c;
}

void AppendStr(char* out, size_t cap, size_t* len, const char* s) {
  while (*s && *len + 1 < cap) out[(*len)++] = *s++;
}

// Les types d'objet qu'`ItemTitle_IsDecoratedType 0x006a5d70` déclare équipables
// (= `itemdb_isequip2` côté rAthena). Lu dans info+0x00.
bool IsEquipType(int type) {
  switch (type) {
    case 4: case 5: case 8: case 9: case 11:
    case 12: case 13: case 14: case 15: case 21:
      return true;
    default:
      return false;
  }
}

}  // namespace

bool BuildChatLink(void* info, char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  if (!info) return false;
  // Appel natif SORTI du __try : il porte déjà son propre SEH, et MSVC refuse un
  // __try dans une portée qui déroulerait des objets.
  int slots = SlotCount(info);

  size_t len = 0;
  bool ok = false;
  __try {
    const uint8_t* p = static_cast<const uint8_t*>(info);
    const int      type  = *reinterpret_cast<const int*>(p + 0x00);
    const uint32_t equip = *reinterpret_cast<const uint32_t*>(p + 0x08);
    const char*    ids   = rag::clientstr::Data(p + kInfoIdStr);
    const uint32_t id     = ids ? static_cast<uint32_t>(std::atoi(ids)) : 0u;
    const uint32_t refine = *reinterpret_cast<const uint32_t*>(p + 0x60);
    const uint32_t look   = *reinterpret_cast<const uint32_t*>(p + 0x70);
    const uint32_t grade  =
        static_cast<uint32_t>(*reinterpret_cast<const int16_t*>(p + 0x88));
    uint32_t cards[4];
    for (int i = 0; i < 4; ++i)
      cards[i] = *reinterpret_cast<const uint32_t*>(p + 0x1c + i * 4);
    int nopt = *reinterpret_cast<const int*>(p + 0x98);
    if (nopt < 0) nopt = 0;
    if (nopt > 5) nopt = 5;

    if (id != 0) {  // sans nameid il n'y a pas de lien à faire
      AppendStr(out, out_size, &len, "<ITEML>");
      AppendB62(out, out_size, &len, equip, 5);
      AppendChar(out, out_size, &len, IsEquipType(type) ? '1' : '0');
      AppendB62(out, out_size, &len, id, 2);
      if (refine != 0) {
        AppendChar(out, out_size, &len, '%');
        AppendB62(out, out_size, &len, refine, 2);
      }
      AppendChar(out, out_size, &len, '&');
      AppendB62(out, out_size, &len, look, 2);
      AppendChar(out, out_size, &len, '\'');
      AppendB62(out, out_size, &len, grade, 2);

      // Bornage des emplacements, à l'identique du natif : 0 emplacement connu
      // vaut 4, et une carte présente dans N'IMPORTE lequel des quatre mots force
      // 4 — sinon un item forgé (dont ces mots portent les données du forgeron)
      // verrait sa charge tronquée.
      if (slots <= 0) slots = 4;
      if (cards[0] || cards[1] || cards[2] || cards[3]) slots = 4;
      if (slots > 4) slots = 4;
      for (int i = 0; i < slots; ++i) {
        AppendChar(out, out_size, &len, ')');
        AppendB62(out, out_size, &len, cards[i], 2);
      }

      for (int k = 0; k < nopt; ++k) {
        const uint8_t* e = p + 0x9c + k * 5;
        const uint32_t opt_id    = static_cast<uint32_t>(*reinterpret_cast<const int16_t*>(e));
        const uint32_t opt_value = static_cast<uint32_t>(*reinterpret_cast<const int16_t*>(e + 2));
        const uint32_t opt_param = e[4];
        if (opt_id == 0) break;  // le client n'affiche rien au-delà d'un id nul
        AppendChar(out, out_size, &len, '+');
        AppendB62(out, out_size, &len, opt_id, 2);
        AppendChar(out, out_size, &len, ',');
        AppendB62(out, out_size, &len, opt_param, 2);
        AppendChar(out, out_size, &len, '-');
        AppendB62(out, out_size, &len, opt_value, 2);
      }
      // ── Champ PRIVÉ Moonlight : l'équipement cassé (cf. ChatLink::broken) ──
      // Le format officiel ne porte pas `+0x5d`. On l'ajoute en DERNIER, avec un
      // séparateur que le client n'écrit jamais (`!`, rang 0 de sa table) : son
      // décodeur le range dans une case qu'il ne lit pas, et tout le reste de la
      // balise est intact pour qui ne connaît pas ce champ.
      if (*reinterpret_cast<const uint8_t*>(p + 0x5d) != 0) {
        AppendChar(out, out_size, &len, '!');
        AppendB62(out, out_size, &len, 1, 2);
      }
      AppendStr(out, out_size, &len, "</ITEML>");
      ok = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }

  out[len < out_size ? len : out_size - 1] = '\0';
  // Une balise ouvrante sans sa fermante ferait une ligne de chat illisible chez
  // TOUT LE MONDE : plutôt rien qu'un lien tronqué.
  if (!ok || std::strstr(out, "</ITEML>") == nullptr) {
    out[0] = '\0';
    return false;
  }
  return true;
}

// ── Le lien de chat RELU ─────────────────────────────────────────────────────
namespace {

uint32_t B62Decode(const char* s, size_t len) {
  uint32_t v = 0;
  for (size_t i = 0; i < len; ++i) {
    const int d = text::Base62Digit(s[i]);
    if (d < 0) break;
    v = v * 62u + static_cast<uint32_t>(d);
  }
  return v;
}

// L'ItemSkillInfo que le lien décrit, monté sur le tampon de l'appelant.
// ⚠ À N'APPELER QUE depuis une portée __try : elle exécute du code du jeu.
void FabricateFromLink(uint8_t* info, const ChatLink& l) {
  std::memset(info, 0, kInfoSize);
  reinterpret_cast<InfoCtor_t>(itemdb::kInfoCtorAddr)(info);
  reinterpret_cast<InfoSetId_t>(itemdb::kInfoSetIdAddr)(info, static_cast<int>(l.id));
  // 🔴 LE TYPE @0x00 N'EST PAS DANS LE LIEN — et il faut pourtant l'écrire, sans
  // quoi le nom sort NU. Le name-builder ne consulte ce champ qu'à travers un
  // prédicat, `ItemTitle_IsDecoratedType 0x006a5d70`, qui teste l'appartenance à
  // {4,5,8,9,11,12,13,14,15,21} — et c'est EXACTEMENT ce booléen que le 6e
  // caractère de la balise transporte (cf. UIWnd_AppendItemLinkButton). Une
  // valeur dedans (4 = arme) ou dehors (3 = consommable) reproduit donc
  // fidèlement la décision du client, sans avoir à deviner le vrai type.
  *reinterpret_cast<int*>(info + 0x00) = l.equipable ? 4 : 3;
  *reinterpret_cast<uint32_t*>(info + 0x08) = l.equip;
  for (int i = 0; i < 4; ++i)
    *reinterpret_cast<uint32_t*>(info + 0x1c + i * 4) = l.cards[i];
  info[0x5c] = 1;  // identifié : sans ce drapeau le builder rend le nom de base
  info[0x5d] = l.broken ? 1 : 0;  // cassé (champ privé de la balise)
  *reinterpret_cast<uint32_t*>(info + 0x60) = l.refine;
  *reinterpret_cast<uint32_t*>(info + 0x70) = l.view;
  *reinterpret_cast<int16_t*>(info + 0x88)  = static_cast<int16_t>(l.grade);
  int nopt = l.opt_count;
  if (nopt < 0) nopt = 0;
  if (nopt > 5) nopt = 5;
  *reinterpret_cast<int*>(info + 0x98) = nopt;
  for (int k = 0; k < nopt; ++k) {
    uint8_t* e = info + 0x9c + k * 5;
    *reinterpret_cast<int16_t*>(e)     = static_cast<int16_t>(l.opt_id[k]);
    *reinterpret_cast<int16_t*>(e + 2) = static_cast<int16_t>(l.opt_value[k]);
    e[4] = l.opt_param[k];
  }
  // Même coup de pouce que partout ailleurs : la DB de descriptions n'est peuplée
  // qu'à la demande, et un objet jamais consulté rendrait un nom vide.
  void* cache = *reinterpret_cast<void**>(itemdb::kEnsureCachePtr);
  if (cache)
    reinterpret_cast<EnsureLoaded_t>(itemdb::kEnsureLoadedAddr)(
        cache, static_cast<int>(l.id));
}

}  // namespace

bool ParseChatLink(const char* tag, const char* end, ChatLink* out,
                   const char** tag_end) {
  if (tag_end) *tag_end = end;
  if (!tag || !end || !out || end - tag < 7) return false;
  if (std::strncmp(tag, "<ITEML>", 7) != 0) return false;
  *out = ChatLink{};

  // La fermante est cherchée DANS les bornes : une balise tronquée (ligne coupée
  // par le serveur) ne doit pas faire lire au-delà du texte.
  const char* close = nullptr;
  for (const char* q = tag + 7; q + 8 <= end; ++q)
    if (std::strncmp(q, "</ITEML>", 8) == 0) { close = q; break; }
  const char* stop = (close != nullptr) ? close : end;
  if (tag_end) *tag_end = (close != nullptr) ? close + 8 : end;

  const char* p = tag + 7;
  if (stop - p < 6) return false;
  out->equip = B62Decode(p, 5);
  p += 5;
  out->equipable = (*p++ == '1');

  size_t n = 0;
  while (p + n < stop && text::Base62Digit(p[n]) >= 0) ++n;
  if (n == 0) return false;
  out->id = B62Decode(p, n);
  p += n;

  // Champs facultatifs : un séparateur, sa charge en base62. La table est celle
  // du client ET de rAthena (cf. BuildChatLink) ; un séparateur qu'on ne connaît
  // pas voit simplement sa charge sautée, plutôt que de désynchroniser le reste.
  int card = 0;
  while (p < stop) {
    const char sep = *p++;
    size_t k = 0;
    while (p + k < stop && text::Base62Digit(p[k]) >= 0) ++k;
    const uint32_t v = (k > 0) ? B62Decode(p, k) : 0;
    p += k;
    const int slot = out->opt_count;
    switch (sep) {
      case '%':  out->refine = v; break;
      case '&':  out->view   = v; break;
      case '\'': out->grade  = v; break;
      case ')':  if (card < 4) out->cards[card++] = static_cast<uint32_t>(v); break;
      // Une option arrive en TROIS morceaux (`+id`, `,paramètre`, `-valeur`) et
      // c'est la valeur qui clôt le triplet : compter là, c'est ne jamais
      // enregistrer une option à moitié lue.
      case '+':  if (slot < 5) out->opt_id[slot]    = static_cast<uint16_t>(v); break;
      case ',':  if (slot < 5) out->opt_param[slot] = static_cast<uint8_t>(v); break;
      case '-':  if (slot < 5) { out->opt_value[slot] = static_cast<uint16_t>(v);
                                 ++out->opt_count; } break;
      case '!':  out->broken = (v != 0); break;  // champ privé Moonlight
      default: break;
    }
  }
  return out->id != 0;
}

void BuildChatLinkName(const ChatLink& link, char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  out[0] = '\0';
  if (link.id == 0) return;
  uint8_t info[kInfoSize];
  bool built = false;
  __try {
    FabricateFromLink(info, link);
    built = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { built = false; }
  if (!built) return;
  // BuildDisplayName porte son propre SEH et son propre repli sur le nom de base.
  BuildDisplayName(info, out, out_size);
  // ⚠ Le suffixe « [N] » n'est PAS composé par le name-builder (il faut lui
  // passer son dernier argument à 1, ce qu'aucun de nos appels ne fait) : c'est
  // à l'appelant de l'ajouter, comme partout ailleurs dans le projet. Le compte
  // vient de la DB du client, pas de la balise — elle ne le transporte pas, et
  // l'info fabriquée suffit à l'y lire.
  const int slots = SlotCount(info);
  if (slots > 0) {
    const size_t len = std::strlen(out);
    std::snprintf(out + len, (len < out_size) ? out_size - len : 0, " [%d]", slots);
  }
}

// Ré-encoder une balise depuis un lien DÉJÀ relu. Il n'y a pas de second
// encodeur : on refabrique l'ItemSkillInfo que la balise décrit et on repasse par
// `BuildChatLink`, le seul endroit qui connaisse le format. C'est ce qui permet de
// RELAYER le lien d'un objet qu'on ne possède pas — celui qu'un autre joueur vient
// de poster — sans jamais avoir eu l'objet sous la main.
bool BuildChatLinkFromLink(const ChatLink& link, char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  if (link.id == 0) return false;
  uint8_t info[kInfoSize];
  bool built = false;
  __try {
    FabricateFromLink(info, link);
    built = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { built = false; }
  if (!built) return false;
  return BuildChatLink(info, out, out_size);
}

const char* NameById(uint32_t id) {
  auto it = g_name_cache.find(id);
  if (it != g_name_cache.end()) return it->second.c_str();
  // 128 et non 64 : la conversion se fait sur des octets CP949, et couper au
  // milieu d'une paire d'octets donnerait une séquence UTF-8 invalide.
  char buf[128];
  ResolveNameSEH(id, buf, sizeof(buf));
  if (buf[0] == '\0') std::snprintf(buf, sizeof(buf), "#%u", id);
  // Les noms de la DB sont dans la code-page du CLIENT et ImGui attend de
  // l'UTF-8. C'est identique pour l'immense majorité d'entre eux — sur les
  // 27 219 noms d'itemInfoMerged.lua, 27 199 sont en ASCII, où la conversion ne
  // change pas un octet — mais pas pour les 20 restants (reliquats coréens :
  // « 수라 Shadow Cube »…), que rendre bruts donnerait de l'UTF-8 invalide.
  //
  // ⚠ ro::LocalToUtf8 et NON Cp949ToUtf8 : le client pose sa code-page d'après
  // son servicetype et ce n'est pas toujours 949 (cf. rag::kClientCodePageAddr).
  // On décode comme LUI décode, sans quoi nos fenêtres et les siennes
  // n'afficheraient pas la même chose.
  //
  // Converti ICI, une fois à l'insertion : le tampon rendu est thread-local et
  // ROTATIF, la copie dans la std::string le met hors de portée. On mémorise
  // même l'échec — un id absent de la DB le restera, et réessayer à chaque frame
  // relancerait le chargement paresseux pour rien.
  return (g_name_cache[id] = ro::LocalToUtf8(buf)).c_str();
}

const char* Label(char* out, size_t out_size, const char* name, int slots) {
  if (!out || out_size == 0) return "";
  const char* base = (name && name[0]) ? name : "(?)";
  if (slots > 0) std::snprintf(out, out_size, "%s [%d]", base, slots);
  else           std::snprintf(out, out_size, "%s", base);
  // ⚠ `snprintf` tronque à l'OCTET. Le nom est en UTF-8 (cf. ItemRow::name) : une
  // coupure au milieu d'une séquence rendrait un carré — exactement le défaut que
  // la conversion à la source vient de faire disparaître.
  TrimIncompleteUtf8(out);
  return out;
}

void DrawTooltip(uint32_t id, const uint32_t* cards, int card_count,
                 const itemdesc::SimpleOpt* opts, int opt_count, int refine,
                 const char* name, bool damaged) {
  if (id == 0) return;
  // Largeur max (wrap du texte), à l'échelle : c'est un cadre autour de TEXTE,
  // et un plafond resté à 330 px sur une police doublée transformait chaque
  // description en colonne de deux mots. (`DescPanelEdge` suit déjà l'échelle.)
  const float kWidth = ro::Px(330.0f);
  const float edge = ro::DescPanelEdge();
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(255, 255, 255, 255));
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));  // sur fond clair
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ro::Px(4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(edge, edge));
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(0.0f, 0.0f), ImVec2(kWidth, ImGui::GetIO().DisplaySize.y * 0.8f));
  ImGui::BeginTooltip();
  // Le cadre sysbox se peint DERRIÈRE le texte : on scinde le draw list en deux
  // canaux, le contenu dans le 1, le cadre dans le 0, puis on fusionne.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  itemdesc::RenderSimpleDesc(id, kWidth - 2.0f * edge, cards, card_count, opts,
                             opt_count, refine, name, damaged);
  dl->ChannelsSetCurrent(0);
  const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
  ro::DrawDescPanelFrame(dl, pos.x, pos.y, pos.x + size.x, pos.y + size.y, false);
  dl->ChannelsMerge();
  ImGui::EndTooltip();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
}

// ── Ouverture de la fenêtre de description (0x0c) ────────────────────────────

void OpenDescFromInfo(const void* info, int mx, int my) {
  if (!info) return;
  __try {
    void* dwnd = uiwnd::MakeWindow(itemdb::kItemDescWndId);
    if (!dwnd) return;
    // OnMsg 0x18 COPIE l'info dans wnd+0xb8 : on ne cède rien, et l'objet du jeu
    // n'est pas modifié. C'est ce qui rend l'appel sûr sur un nœud vivant.
    uiwnd::OnMsg(dwnd, itemdb::kItemDescMsgSet,
                 static_cast<int>(reinterpret_cast<uintptr_t>(info)), 0, 0, 0);
    uiwnd::SetPos(dwnd, mx, my);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void OpenDescById(uint32_t id, uint16_t view, uint32_t location, int mx, int my,
                  const void* src) {
  if (id == 0) return;
  __try {
    uint8_t info[kInfoSize];
    std::memset(info, 0, sizeof(info));
    reinterpret_cast<InfoCtor_t>(itemdb::kInfoCtorAddr)(info);
    reinterpret_cast<InfoSetId_t>(itemdb::kInfoSetIdAddr)(info, static_cast<int>(id));
    if (src) {
      // Le name-builder natif (0x008a0570) décore le nom à partir du TYPE @0,
      // des cartes @0x1c-0x28, du refine @0x60, du grade @0x88 : sans ces
      // champs il rend le nom NU. On saute les deux std::string (@0x2c id,
      // @0x44 resname) que SetId vient de construire — les recopier ferait
      // partager un tampon heap entre deux ItemSkillInfo.
      const uint8_t* s = reinterpret_cast<const uint8_t*>(src);
      std::memcpy(info + 0x00, s + 0x00, 0x2c);         // type .. cartes
      std::memcpy(info + 0x5c, s + 0x5c, 0xf8 - 0x5c);  // identifié/refine/view/grade/options
    } else {
      *reinterpret_cast<uint32_t*>(info + 0x08) = location;  // equip point : gate « aperçu »
      *reinterpret_cast<uint32_t*>(info + 0x70) = view;      // viewID      : gate « aperçu »
    }
    info[0x5c] = 1;  // identifié : resname/desc lus dans l'enregistrement DB
    // Chargement paresseux du DB : sans ça la description serait vide au premier
    // affichage d'un item jamais consulté.
    void* cache = *reinterpret_cast<void**>(itemdb::kEnsureCachePtr);
    if (cache)
      reinterpret_cast<EnsureLoaded_t>(itemdb::kEnsureLoadedAddr)(cache, static_cast<int>(id));
    void* dwnd = uiwnd::MakeWindow(itemdb::kItemDescWndId);
    if (dwnd) {
      uiwnd::OnMsg(dwnd, itemdb::kItemDescMsgSet,
                   static_cast<int>(reinterpret_cast<uintptr_t>(info)), 0, 0, 0);
      uiwnd::SetPos(dwnd, mx, my);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Description d'un objet qu'on ne possède PAS : tout vient de la balise. Même
// message que les deux autres portes (0x18, qui COPIE l'info), sur un
// ItemSkillInfo fabriqué — donc cartes, refine, grade et options compris.
void OpenDescFromChatLink(const ChatLink& link, int mx, int my) {
  if (link.id == 0) return;
  __try {
    uint8_t info[kInfoSize];
    FabricateFromLink(info, link);
    void* dwnd = uiwnd::MakeWindow(itemdb::kItemDescWndId);
    if (dwnd) {
      uiwnd::OnMsg(dwnd, itemdb::kItemDescMsgSet,
                   static_cast<int>(reinterpret_cast<uintptr_t>(info)), 0, 0, 0);
      uiwnd::SetPos(dwnd, mx, my);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Ouverture différée au relâchement (cf. l'en-tête pour le pourquoi) ───────

void DeferDescFromIndex(uintptr_t list_head, int index, int mx, int my) {
  g_deferred = DeferredDesc{};
  g_deferred.list_head = list_head;
  g_deferred.index     = index;
  g_deferred.mx        = mx;
  g_deferred.my        = my;
}

void DeferDescById(uint32_t id, uint16_t view, uint32_t location, int mx, int my,
                   const void* src) {
  g_deferred = DeferredDesc{};
  g_deferred.id       = id;
  g_deferred.view     = view;
  g_deferred.location = location;
  g_deferred.src      = src;
  g_deferred.mx       = mx;
  g_deferred.my       = my;
}

void DeferDescFromChatLink(const ChatLink& link, int mx, int my) {
  g_deferred = DeferredDesc{};
  g_deferred.link      = link;
  g_deferred.from_link = true;
  g_deferred.mx        = mx;
  g_deferred.my        = my;
}

void FlushDeferredDesc() {
  if (g_deferred.list_head == 0 && g_deferred.id == 0 && !g_deferred.from_link)
    return;
  // Le « bouton relâché » est LA condition — pas seulement « hors frame ImGui ».
  // Tant qu'un bouton est enfoncé, la fenêtre cliquée garde le focus et
  // repasserait devant la description à la frame suivante (cf. l'en-tête).
  if (ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
      ImGui::IsMouseDown(ImGuiMouseButton_Right))
    return;
  const DeferredDesc d = g_deferred;
  g_deferred = DeferredDesc{};
  if (d.from_link)
    OpenDescFromChatLink(d.link, d.mx, d.my);
  else if (d.list_head != 0)
    OpenDescFromInfo(FindInfoByIndex(d.list_head, d.index), d.mx, d.my);
  else
    OpenDescById(d.id, d.view, d.location, d.mx, d.my, d.src);
}

// ── Parcours des listes de session ───────────────────────────────────────────

namespace {

// ── Le parcours d'une liste d'objets, UNE fois ───────────────────────────────
// Quatre fonctions de ce fichier le récitaient. Trois d'entre elles le faisaient
// sous un `__try` GLOBAL — et c'est précisément la structure que l'extracteur
// (plus bas) a dû abandonner après le bug « 16 objets au lieu de 22 » : un seul
// nœud abîmé y avorte TOUT le parcours.
//
// 🔴 Pour une RECHERCHE, cet abandon rend « pas trouvé » ; pour un COMPTE, il
// rend un total PARTIEL, sans rien signaler. `CountById` est le compteur de
// stock du projet — la fenêtre de fabrication en tire ses « N possédés ». Un
// sous-compte silencieux y est pire qu'une absence de réponse.
//
// La garde est donc PAR NŒUD, comme dans l'extracteur : un nœud fautif est sauté
// (ou arrête le parcours s'il casse le chaînage), sans perdre les précédents.
//
// `visit(info)` renvoie true pour arrêter. Tout est POD ici, et les lambdas des
// appelants ne capturent que par référence : rien à dérouler, donc pas de C2712.
template <typename Visit>
void WalkItemList(uintptr_t list_head, Visit visit) {
  uint8_t* head = nullptr;
  uint8_t* node = nullptr;
  __try {
    head = *reinterpret_cast<uint8_t**>(list_head);
    if (head) node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
  // 🔴 `list_head` est l'adresse du GLOBAL ; le nœud sentinelle est ce qu'il
  // CONTIENT, et c'est lui — pas le global — qui ferme la liste circulaire.
  // Comparer le nœud courant à l'adresse du global fait boucler jusqu'au
  // garde-fou et repasser des centaines de fois sur la même liste. La fenêtre de
  // fabrication a payé ce défaut en affichant « 28300 possédés » pour 200 objets.
  if (!head) return;

  int guard = 0;
  while (node && node != head && guard++ < kWalkGuard) {
    // Le SUIVANT d'abord, sous sa propre garde : un chaînage corrompu arrête net,
    // sans boucle infinie, et sans perdre ce qui a déjà été lu.
    uint8_t* next = nullptr;
    __try { next = *reinterpret_cast<uint8_t**>(node + kNodeNext); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    bool stop = false;
    __try { stop = visit(node + kNodeInfo); }
    __except (EXCEPTION_EXECUTE_HANDLER) { stop = false; }  // ce nœud-là, pas les autres
    if (stop) return;

    node = next;
  }
}

}  // namespace

void* FindInfoById(uintptr_t list_head, uint32_t id) {
  if (id == 0) return nullptr;
  void* found = nullptr;
  WalkItemList(list_head, [&](uint8_t* info) {
    if (rag::itemlist::ItemId(info) != id) return false;
    found = info;
    return true;
  });
  return found;
}

// 🔴 LE COMPTEUR DE STOCK DU PROJET. Quatre fichiers en portaient leur propre
// parcours — `OwnedCount` (fabrication), `OreCount` (raffinage),
// `CountCardStock` (inventaire) et une variante dans l'Atlas — et deux d'entre
// eux avaient dû apprendre SÉPARÉMENT le piège de la sentinelle ci-dessous.
//
// ⚠ Le `amount > 0` vient de ces copies-là ; celle-ci ne l'avait pas. Sur un
// inventaire sain la différence est nulle (aucun nœud ne porte une quantité
// négative), mais garder le test rend les quatre PROUVABLEMENT équivalentes
// plutôt que « équivalentes en pratique ».
int CountById(uintptr_t list_head, uint32_t id) {
  if (id == 0) return 0;
  int total = 0;
  WalkItemList(list_head, [&](uint8_t* info) {
    const int amount = *reinterpret_cast<int*>(info + kInfoAmount);
    if (amount > 0 && rag::itemlist::ItemId(info) == id) total += amount;
    return false;  // toute la liste : un même objet peut occuper plusieurs nœuds
  });
  return total;
}

void* FindInfoByIndex(uintptr_t list_head, int index) {
  void* found = nullptr;
  WalkItemList(list_head, [&](uint8_t* info) {
    if (*reinterpret_cast<int*>(info + kInfoIndex) != index) return false;
    found = info;
    return true;
  });
  return found;
}

// Les champs d'`ItemSkillInfo` que le viewer montre, en plus de ceux que
// `rag::itemlist` publie (index, quantité, id). Tous relus en jeu.
namespace {
constexpr int kInfoType     = 0x00;  // int   : type d'item (onglets)
constexpr int kInfoLoc      = 0x08;  // u32   : masque d'emplacement d'équipement
constexpr int kInfoCards    = 0x1c;  // 4 x u32
constexpr int kInfoIdent    = 0x5c;  // octet : identifié ?
constexpr int kInfoDamaged  = 0x5d;  // octet : équipement CASSÉ
constexpr int kInfoRefine   = 0x60;  // int
constexpr int kInfoFav      = 0x74;  // octet : favori
constexpr int kInfoOptCount = 0x98;  // int
constexpr int kInfoOpts     = 0x9c;  // entrées de 5 octets
constexpr int kMaxOpts      = 5;     // ce que porte `ItemRow::opts`
}  // namespace

int ExtractList(uintptr_t list_head, ItemRow* out, int max) {
  if (!out || max <= 0) return 0;

  int count = 0;
  // 🔴 La garde PAR NŒUD est celle de `WalkItemList` — c'est cette fonction-ci
  // qui l'a inventée, après le bug « 16 objets au lieu de 22 » : sous un `__try`
  // global, un seul item fautif avorte toute l'énumération. `count` n'est
  // incrémenté qu'en FIN de visite, donc un item illisible ne consomme pas de
  // place et les suivants sont lus normalement.
  WalkItemList(list_head, [&](uint8_t* info) {
    ItemRow& it = out[count];
    it.id         = rag::itemlist::ItemId(info);
    it.amount     = *reinterpret_cast<int*>(info + kInfoAmount);
    it.index      = *reinterpret_cast<int*>(info + kInfoIndex);
    it.loc        = *reinterpret_cast<uint32_t*>(info + kInfoLoc);
    it.refine     = *reinterpret_cast<int*>(info + kInfoRefine);
    it.type       = *reinterpret_cast<int*>(info + kInfoType);
    it.identified = *reinterpret_cast<uint8_t*>(info + kInfoIdent);
    it.damaged    = *reinterpret_cast<uint8_t*>(info + kInfoDamaged);
    it.favorite   = *reinterpret_cast<uint8_t*>(info + kInfoFav);
    for (int k = 0; k < 4; ++k)
      it.cards[k] = *reinterpret_cast<uint32_t*>(info + kInfoCards + k * 4);
    int nopt = *reinterpret_cast<int*>(info + kInfoOptCount);
    if (nopt < 0) nopt = 0;
    if (nopt > kMaxOpts) nopt = kMaxOpts;
    it.opt_count = nopt;
    for (int k = 0; k < nopt; ++k) {
      const uint8_t* e = info + kInfoOpts + k * 5;
      it.opts[k].index = *reinterpret_cast<const int16_t*>(e);
      it.opts[k].value = *reinterpret_cast<const int16_t*>(e + 2);
      it.opts[k].param = e[4];
    }
    // Les deux briques partagées portent leur PROPRE SEH, par item — et
    // `BuildDisplayName` rend désormais de l'UTF-8, plus rien à convertir ici.
    BuildDisplayName(info, it.name, sizeof(it.name));
    it.total_slots = SlotCount(info);
    ++count;
    return count >= max;  // tampon plein : on s'arrête là
  });
  return count;
}

void NameText(const char* utf8, bool damaged) {
  if (!utf8) utf8 = "";
  // L'ombre se soumet AVANT le texte au même draw list : elle reste dessous,
  // comme dans DrawName natif (+1,+1, texte inchangé par-dessus).
  if (damaged && utf8[0]) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f),
                                        kDamagedShadow, utf8);
  }
  ImGui::TextUnformatted(utf8);
}

void DrawTile(ImDrawList* draw_list, const ImVec2& p0, const ImVec2& p1,
              float cell, const ro::IconTex& icon, int refine, int amount,
              bool damaged) {
  if (!draw_list) return;

  if (icon.tex && icon.w > 0 && icon.h > 0) {
    // 🔴 L'icône part de sa taille d'art MISE À L'ÉCHELLE, pas de sa taille en
    // pixels : la case grandit avec le réglage d'interface, et une icône restée
    // à 24 px y flotterait au milieu. Le rapport icône/case est ainsi le même à
    // toutes les échelles — et à 100 % la valeur ne change pas d'un pixel.
    float dw = ro::Px(static_cast<float>(icon.w));
    float dh = ro::Px(static_cast<float>(icon.h));
    if (dw > cell || dh > cell) {  // réduire seulement, jamais agrandir
      const float s = cell / (dw > dh ? dw : dh);
      dw *= s; dh *= s;
    }
    const ImVec2 ip(p0.x + (cell - dw) * 0.5f, p0.y + (cell - dh) * 0.5f);
    // Cassé = icône TEINTÉE du rouge de l'ombre native (la tinte multiplie la
    // texture : l'icône reste reconnaissable, mais rougie).
    draw_list->AddImage(reinterpret_cast<ImTextureID>(icon.tex), ip,
                        ImVec2(ip.x + dw, ip.y + dh), ImVec2(0, 0), ImVec2(1, 1),
                        damaged ? kDamagedShadow : ro::SkinImageTint());
  } else {
    draw_list->AddText(ImVec2(p0.x + cell * 0.5f - 4, p0.y + cell * 0.5f - 7),
                       ImGui::GetColorU32(ImGuiCol_TextDisabled), "?");
  }

  char badge[16] = {0};
  if (refine > 0)      std::snprintf(badge, sizeof(badge), "+%d", refine);
  else if (amount > 1) std::snprintf(badge, sizeof(badge), "%d", amount);
  if (badge[0]) {
    const ImVec2 ts = ImGui::CalcTextSize(badge);
    const ImVec2 bp(p1.x - ts.x - ro::Px(2.0f), p1.y - ts.y - ro::Px(1.0f));
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    for (int oy = -1; oy <= 1; ++oy)      // cerne : les 8 voisins en blanc…
      for (int ox = -1; ox <= 1; ++ox)
        if (ox || oy) draw_list->AddText(ImVec2(bp.x + ox, bp.y + oy), white, badge);
    draw_list->AddText(bp, IM_COL32(0, 0, 0, 255), badge);  // …puis le noir dessus
  }
}

}  // namespace itemcell
