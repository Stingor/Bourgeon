#include "features/fx/style_sync.h"

#include <Windows.h>  // SEH autour de la lecture des globales du client

#include <cstring>
#include <iterator>  // std::next
#include <map>
#include <set>  // les corps dont on a déjà signalé la couverture

#include "bourgeon.h"  // SendPacket / RegisterRecvOpcode
#include "features/fx/palette_base.h"
#include "features/fx/palette_cache.h"
#include "features/fx/palette_inject.h"
#include "features/systems/bourgeon_opcodes.h"
#include "features/windows/palette_editor.h"  // ouverture pilotée par un NPC
#include "ragnarok/globals.h"                 // rag::kOwnAccountIdAddr / kOwnCharIdAddr
#include "ui/sprite_path.h"                   // ro::BodySpriteKey
#include "utils/log_console.h"

namespace {

// Les deux identités du joueur, lues sous SEH. Elles ne font qu'envelopper les
// accesseurs de `ragnarok/globals.h` : ce fichier tourne sur le fil réseau comme
// sur celui du rendu, et rien n'y garantit qu'une session existe.
//
// ⚠ Garder l'enveloppe plutôt qu'appeler `rag::` directement est un CHOIX :
// l'accesseur du catalogue est nu, et c'est la forme qu'ont prise tous les
// autres appelants du projet (target_frame, social, party_friend_window,
// weapon_refine_window) — le `__try` englobant couvre l'accesseur inliné.
uint32_t OwnGid() {
  __try {
    return rag::OwnAccountId();
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
uint32_t OwnCharId() {
  __try {
    return rag::OwnCharId();
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Le style d'un joueur : une recette par CORPS, plus celle qui sert de repli.
struct Remote {
  // Clé de corps (`ro::BodySpriteKey`) -> recette. Au plus `kMaxVariants`, le
  // serveur ne pouvant en ranger davantage.
  std::map<uint32_t, ro::PaletteRecipe> variants;
  // La variante à appliquer aux corps qui n'ont pas la leur. 🔴 DÉSIGNÉE par le
  // serveur (`kFlagDefault`), jamais devinée : deux clients qui la choisiraient
  // chacun de leur côté — le premier reçu, le plus petit numéro — finiraient par
  // diverger sur l'ordre d'arrivée d'un paquet.
  uint32_t default_key = 0;
  // 🔴 Cette VERSION-CI a-t-elle été posée sur l'acteur ? Remis à false à chaque
  // paquet reçu, et c'est essentiel : sans ce drapeau, on ne se fiait qu'à
  // « l'acteur a-t-il DÉJÀ une recette », ce qui est vrai dès la première. Un
  // joueur qui retouchait ses couleurs et les re-partageait restait donc figé,
  // chez les autres, sur sa toute première version.
  bool applied = false;
  // La variante RÉELLEMENT posée. Sans elle, un joueur qui enfourche sa monture
  // garderait la recette de son corps à pied : `applied` serait vrai, et rien ne
  // dirait qu'elle ne correspond plus au corps affiché.
  uint32_t applied_key = 0;
  // Tentatives d'application déjà faites. Un acteur qu'on n'arrive pas à monter
  // (sprite absent, corps sans palette) ne doit pas être réessayé indéfiniment :
  // chaque essai relit et reparse un `.spr`, ce qui n'est pas gratuit.
  int attempts = 0;
};

// La clé du corps que cet acteur porte EN CE MOMENT, ou 0 si on ne peut pas le
// lire (acteur mort, composite pas encore monté).
uint32_t CurrentBodyKey(uint32_t gid) {
  char spr[300];
  if (!fx::palette_inject::ActorBodySpritePath(gid, spr, sizeof(spr)))
    return 0;
  return ro::BodySpriteKey(spr);
}

// La recette à appliquer à ce corps : la sienne si elle existe, sinon le repli.
// Rend nullptr si ce joueur n'a aucune variante. `out_key` reçoit la clé
// RETENUE, qui n'est pas toujours celle demandée.
const ro::PaletteRecipe* PickVariant(const Remote& r, uint32_t body_key,
                                     uint32_t* out_key) {
  if (body_key != 0) {
    auto exact = r.variants.find(body_key);
    if (exact != r.variants.end()) {
      *out_key = body_key;
      return &exact->second;
    }
  }
  auto repli = r.variants.find(r.default_key);
  // Un serveur qui aurait omis le drapeau ne doit pas laisser le joueur sans
  // couleurs : la première variante fait alors office de repli. L'ordre d'une
  // `std::map` est celui des clés, donc identique chez tous les clients — ce qui
  // suffit à garder la règle déterministe.
  if (repli == r.variants.end()) repli = r.variants.begin();
  if (repli == r.variants.end()) return nullptr;
  *out_key = repli->first;
  return &repli->second;
}

// 🔴 Écrite et lue par le SEUL fil principal (HandlePacket et OnTick le sont
// tous les deux). Aucun verrou n'est donc nécessaire — et le fil réseau ne la
// voit jamais, puisqu'il se contente d'empiler des octets dans `net_inbox_`.
std::map<uint32_t, Remote> g_remote;

// ── Tables de la réparation automatique (cf. `AutoRepair`, plus bas) ─────────
// Déclarées ICI et non auprès d'elle : `PruneIfCrowded` les purge en même temps
// que `g_remote`, et il est défini plus haut dans ce fichier.
//
// GID déjà examinés : 1 = réparé, 0 = rien à faire. Un corps sain ne doit pas
// être ré-analysé à chaque tick pour aboutir au même « non ».
std::map<uint32_t, uint8_t> g_repair_seen;
// Premier instant où chaque GID nous est apparu, pour le sursis de réparation.
std::map<uint32_t, unsigned long> g_first_seen;
// Personnage dont les couleurs ont déjà été restaurées depuis le cache local.
// 🔴 Un identifiant, pas un booléen : changer de personnage sans quitter le jeu
// doit relancer la restauration, sinon le second reste sur son apparence native.
uint32_t g_restored_cid = 0;

// Personnage actuellement en jeu. 🔴 Sert à détecter un CHANGEMENT de
// personnage sans quitter le client — voir `ForgetPreviousCharacter`.
uint32_t g_session_cid = 0;

// NOS variantes, telles que le serveur nous les renvoie, et l'état de l'éditeur
// qui décide si on a le droit de les reposer.
std::map<uint32_t, ro::PaletteRecipe> g_local_variants;
uint32_t g_local_default = 0;
bool g_local_editing = false;

StyleSync* g_instance = nullptr;

// Au-delà, on renonce : ce corps ne se laisse pas monter.
constexpr int kMaxAttempts = 5;
// Acteurs traités par tick. Parser un `.spr` de corps coûte quelques
// millisecondes ; en traiter cinquante d'un coup à l'arrivée sur une carte
// peuplée se verrait comme un à-coup.
constexpr int kApplyBudget = 2;
// Plafond du registre. Rien ne nous dit jamais qu'un joueur a quitté la vue :
// sans borne, une longue session de ville accumulerait indéfiniment des recettes
// d'acteurs disparus. Au plafond, on jette d'abord celles dont l'acteur n'est
// même pas monté — ce sont exactement les joueurs partis.
constexpr size_t kMaxRemote = 512;

// Déclaré ici, défini plus bas : `Prune` a besoin de `palette_inject`.
void PruneIfCrowded();

// ── Sérialisation ────────────────────────────────────────────────────────────
// 🔴 Champ par champ, jamais un memcpy de la struct : `ro::RampAdjust` porte un
// octet de bourrage dont la position dépend du compilateur, et le serveur n'est
// pas compilé par le même.

void WriteAdjusts(uint8_t* out, const ro::PaletteRecipe& recipe) {
  for (int i = 0; i < ro::kMaxRamps; ++i) {
    const ro::RampAdjust& a = recipe.ramps[i];
    uint8_t* p = out + i * fx::style_sync::kRampBytes;
    p[0] = static_cast<uint8_t>(a.hue & 0xFF);
    p[1] = static_cast<uint8_t>((a.hue >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>(a.sat);
    p[3] = static_cast<uint8_t>(a.val);
    p[4] = a.absolute;
  }
}

void ReadAdjusts(const uint8_t* in, ro::PaletteRecipe* recipe) {
  for (int i = 0; i < ro::kMaxRamps; ++i) {
    const uint8_t* p = in + i * fx::style_sync::kRampBytes;
    ro::RampAdjust& a = recipe->ramps[i];
    a.hue = static_cast<int16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
    a.sat = static_cast<int8_t>(p[2]);
    a.val = static_cast<int8_t>(p[3]);
    // Tout non-zéro vaut « absolu » : un serveur ou un client plus récent
    // pourrait y mettre autre chose que 1, et une comparaison stricte ferait
    // silencieusement retomber la rampe en mode relatif.
    a.absolute = p[4] ? 1 : 0;
  }
}

void PruneIfCrowded() {
  if (g_remote.size() < kMaxRemote) return;
  const size_t avant = g_remote.size();
  for (auto it = g_remote.begin(); it != g_remote.end();) {
    // Un acteur non monté ET déjà posé nulle part : le joueur est hors de vue,
    // et sa recette ne sert plus à rien tant qu'il n'est pas re-annoncé — ce que
    // le serveur fera de toute façon à son retour.
    char spr[8];
    const bool monte =
        fx::palette_inject::ActorBodySpritePath(it->first, spr, sizeof(spr)) ||
        fx::palette_inject::HasRecipe(it->first);
    if (monte) {
      ++it;
    } else {
      // Les tables annexes suivent : sans ça, `g_first_seen` et `g_repair_seen`
      // grossiraient sans fin sur une ville très fréquentée.
      g_first_seen.erase(it->first);
      g_repair_seen.erase(it->first);
      it = g_remote.erase(it);
    }
  }
  LogDebug("[palette] registre élagué : {} -> {}", avant, g_remote.size());
}

void SendOne(const ro::PaletteRecipe& recipe, uint8_t flags, uint32_t body_key) {
  const bool clear = flags != 0;
  uint8_t pkt[fx::style_sync::kCzBytes];
  std::memset(pkt, 0, sizeof(pkt));
  *reinterpret_cast<uint16_t*>(&pkt[0]) = bopcodes::kStyle;
  *reinterpret_cast<uint16_t*>(&pkt[2]) =
      static_cast<uint16_t>(fx::style_sync::kCzBytes);
  pkt[4] = fx::style_sync::kWireVersion;
  pkt[5] = flags;
  pkt[6] = static_cast<uint8_t>(body_key & 0xFF);
  pkt[7] = static_cast<uint8_t>((body_key >> 8) & 0xFF);
  pkt[8] = static_cast<uint8_t>((body_key >> 16) & 0xFF);
  pkt[9] = static_cast<uint8_t>((body_key >> 24) & 0xFF);
  const int16_t pal = clear ? static_cast<int16_t>(-1) : recipe.palette_id;
  const int16_t hair = clear ? static_cast<int16_t>(-1) : recipe.hair_palette_id;
  const int16_t coupe = clear ? static_cast<int16_t>(-1) : recipe.hair_style;
  pkt[10] = static_cast<uint8_t>(pal & 0xFF);
  pkt[11] = static_cast<uint8_t>((pal >> 8) & 0xFF);
  pkt[12] = static_cast<uint8_t>(hair & 0xFF);
  pkt[13] = static_cast<uint8_t>((hair >> 8) & 0xFF);
  pkt[14] = static_cast<uint8_t>(coupe & 0xFF);
  pkt[15] = static_cast<uint8_t>((coupe >> 8) & 0xFF);
  if (!clear) WriteAdjusts(&pkt[16], recipe);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

}  // namespace

namespace fx {
namespace style_sync {

bool Available() { return g_instance != nullptr; }

// La recette d'un AUTRE joueur, pour les vues qui le dessinent hors du monde.
//
// 🔴 Même sélection de variante que ce qu'on pose sur son acteur (`PickVariant`)
// : une vue qui choisirait autrement afficherait d'autres couleurs que celles
// qu'on a sous les yeux, ce qui est pire que pas de couleurs du tout.
//
// ⚠ `g_remote` est écrite et lue par le SEUL fil principal (cf. sa déclaration).
// Cette fonction en fait partie : elle est appelée depuis le rendu.
bool RemoteRecipe(uint32_t gid, uint32_t body_key, ro::PaletteRecipe* out) {
  if (!out || gid == 0) return false;
  auto it = g_remote.find(gid);
  if (it == g_remote.end()) return false;
  uint32_t retenue = 0;
  const ro::PaletteRecipe* pick = PickVariant(it->second, body_key, &retenue);
  if (pick == nullptr) return false;
  *out = *pick;
  return true;
}

void SetLocalEditing(bool editing) { g_local_editing = editing; }

bool LocalRecipe(uint32_t body_key, ro::PaletteRecipe* out, bool* out_exact) {
  if (!out) return false;
  if (out_exact) *out_exact = false;
  if (g_local_variants.empty()) return false;
  if (body_key != 0) {
    auto exact = g_local_variants.find(body_key);
    if (exact != g_local_variants.end()) {
      *out = exact->second;
      if (out_exact) *out_exact = true;
      return true;
    }
  }
  // Même règle de repli que pour les autres joueurs — c'est bien le même objet
  // qu'on regarde des deux côtés du fil, et il ne doit pas se lire autrement
  // chez soi que chez les autres.
  auto repli = g_local_variants.find(g_local_default);
  if (repli == g_local_variants.end()) repli = g_local_variants.begin();
  *out = repli->second;
  return true;
}

bool LocalHasVariant(uint32_t body_key) {
  return body_key != 0 && g_local_variants.count(body_key) != 0;
}

int LocalVariantCount() { return static_cast<int>(g_local_variants.size()); }

void ForgetLocal() {
  g_local_variants.clear();
  g_local_default = 0;
  const uint32_t self = OwnGid();
  if (self != 0) g_remote.erase(self);
}

void SendRecipe(const ro::PaletteRecipe& recipe, uint32_t body_key) {
  if (!g_instance) return;
  // 🔴 Une clé nulle veut dire « je n'ai pas su lire le corps ». La ranger
  // quand même donnerait une variante qu'aucun client ne choisirait jamais —
  // elle ne correspondrait à aucun sprite — et elle prendrait la place d'une
  // vraie dans les quatre emplacements du personnage.
  if (body_key == 0) {
    LogDiag("[palette] style non partagé : corps inconnu (acteur pas monté ?)");
    return;
  }
  SendOne(recipe, /*flags=*/0, body_key);
  // Ce que le joueur vient de partager EST désormais sa référence pour ce corps,
  // sans attendre que le serveur nous le renvoie.
  g_local_variants[body_key] = recipe;
  if (g_local_default == 0) g_local_default = body_key;
  // ⚠ Le plafond est celui du serveur, et il évince la plus ANCIENNE : notre
  // copie locale doit suivre la même règle, sinon l'éditeur proposerait une
  // variante que le serveur a déjà oubliée. Faute de connaître son numéro de
  // séquence, on jette celle qui n'est pas le repli et dont la clé est la plus
  // petite — l'écart se corrige de toute façon au premier écho du serveur, qui
  // fait autorité.
  while (static_cast<int>(g_local_variants.size()) > kMaxVariants) {
    auto victime = g_local_variants.begin();
    if (victime->first == g_local_default && g_local_variants.size() > 1)
      ++victime;
    g_local_variants.erase(victime);
  }
  // Sans attendre l'écho du serveur : si la session se termine avant qu'il ne
  // revienne, le char-select afficherait encore l'ancienne apparence.
  fx::palette_cache::SaveAll(OwnCharId(), g_local_variants, g_local_default);
}

void SendClear(uint32_t body_key) {
  if (!g_instance || body_key == 0) return;
  const ro::PaletteRecipe vide;
  SendOne(vide, fx::style_sync::kFlagClear, body_key);
  g_local_variants.erase(body_key);
  if (g_local_default == body_key)
    g_local_default = g_local_variants.empty() ? 0
                                               : g_local_variants.begin()->first;
  fx::palette_cache::SaveAll(OwnCharId(), g_local_variants, g_local_default);
}

void SendClearAll() {
  if (!g_instance) return;
  const ro::PaletteRecipe vide;
  SendOne(vide, fx::style_sync::kFlagClearAll, /*body_key=*/0);
  g_local_variants.clear();
  g_local_default = 0;
  fx::palette_cache::SaveAll(OwnCharId(), g_local_variants, 0);
}

}  // namespace style_sync
}  // namespace fx

StyleSync::StyleSync() {
  g_instance = this;
  // Zone sûre (> 0x0C35) : livré par le reader-hook de RagConnection, pas par la
  // dispatch table. cf. features/systems/bourgeon_opcodes.h.
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kStyles);
  // Ouverture pilotée par un NPC : c'est ce module qui écoute, et non l'éditeur,
  // parce qu'il tourne même quand la fenêtre n'a jamais été ouverte.
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kStyleOpen);
}

StyleSync::~StyleSync() {
  if (g_instance == this) g_instance = nullptr;
}

void StyleSync::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                               uint16_t len) {
  net_inbox_.Push(opcode, data, len);  // fil RÉSEAU : rien d'autre
}

void StyleSync::HandlePacket(uint16_t opcode, const uint8_t* data,
                               uint16_t len) {
  if (opcode == bopcodes::kStyleOpen) {
    // [mode:1] — 0 ferme, 1 ouvre, 2 bascule. Un NPC de styliste veut « ouvre »
    // sans risquer de refermer ce que le joueur venait d'ouvrir lui-même.
    if (!data || len < 1) return;
    PaletteEditor* editeur = Bourgeon::Instance().palette_editor();
    if (!editeur) return;
    switch (data[0]) {
      case 0: editeur->SetOpen(false); break;
      case 1: editeur->SetOpen(true); break;
      default: editeur->Toggle(); break;
    }
    return;
  }
  if (opcode != bopcodes::kStyles) return;
  // `data` commence APRÈS [opcode:2][longueur:2] — régime RegisterRecvOpcode.
  if (!data || len < 2) return;
  const int count = static_cast<int>(data[0]) |
                    (static_cast<int>(data[1]) << 8);
  if (count <= 0) return;

  const uint32_t self = OwnGid();
  const uint8_t* p = data + 2;
  int available = (len - 2) / fx::style_sync::kZcEntryBytes;
  if (available > count) available = count;
  if (available < count) {
    // Le serveur annonce plus d'entrées qu'il n'en a envoyé : on prend ce qui
    // est là plutôt que de lire au-delà du tampon.
    LogDiag("[palette] lot tronqué : {} annoncées, {} lisibles", count,
            available);
  }

  // 🔴 Un lot REDÉFINIT INTÉGRALEMENT les variantes des joueurs qu'il mentionne :
  // le serveur envoie toujours l'ensemble complet d'un personnage, même quand une
  // seule vient de changer. On vide donc à la PREMIÈRE entrée rencontrée pour un
  // GID, et pas avant — sinon un lot tronqué ou une version refusée effacerait
  // des variantes valides sans rien mettre à la place.
  std::map<uint32_t, bool> vus;

  for (int i = 0; i < available; ++i) {
    const uint8_t* e = p + i * fx::style_sync::kZcEntryBytes;
    const uint32_t gid = static_cast<uint32_t>(e[0]) |
                         (static_cast<uint32_t>(e[1]) << 8) |
                         (static_cast<uint32_t>(e[2]) << 16) |
                         (static_cast<uint32_t>(e[3]) << 24);
    if (gid == 0) continue;

    // 🔴 Version inconnue = entrée IGNORÉE. La disposition des réglages peut
    // changer (un `kMaxRamps` différent, par exemple) ; la lire quand même
    // poserait des couleurs tirées d'octets mal découpés.
    if (e[4] != fx::style_sync::kWireVersion) continue;

    const uint8_t flags = e[5];
    const bool clear = (flags & fx::style_sync::kFlagClear) != 0;
    const uint32_t body_key = static_cast<uint32_t>(e[6]) |
                              (static_cast<uint32_t>(e[7]) << 8) |
                              (static_cast<uint32_t>(e[8]) << 16) |
                              (static_cast<uint32_t>(e[9]) << 24);
    ro::PaletteRecipe recipe;
    if (!clear) {
      recipe.palette_id = static_cast<int16_t>(
          static_cast<uint16_t>(e[10]) | (static_cast<uint16_t>(e[11]) << 8));
      recipe.hair_palette_id = static_cast<int16_t>(
          static_cast<uint16_t>(e[12]) | (static_cast<uint16_t>(e[13]) << 8));
      recipe.hair_style = static_cast<int16_t>(
          static_cast<uint16_t>(e[14]) | (static_cast<uint16_t>(e[15]) << 8));
      ReadAdjusts(e + 16, &recipe);
    }

    const bool premiere = !vus[gid];
    vus[gid] = true;

    // ── Nos propres variantes ────────────────────────────────────────────
    // Elles nous reviennent au login (le serveur nous les repousse) et à chaque
    // partage (la diffusion de zone nous inclut). On les MÉMORISE toujours —
    // l'éditeur démarrera dessus au lieu d'une recette vide qui les effacerait —
    // mais on ne les POSE que si l'éditeur est fermé : sinon on écraserait un
    // réglage en cours par la dernière version validée, et le joueur verrait
    // ses curseurs sauter en arrière.
    if (gid == self) {
      if (premiere) {
        g_local_variants.clear();
        g_local_default = 0;
      }
      if (!clear && body_key != 0) {
        g_local_variants[body_key] = recipe;
        if ((flags & fx::style_sync::kFlagDefault) || g_local_default == 0)
          g_local_default = body_key;
      }
      // Le cache local suit le serveur, qui fait autorité. C'est ce qui permet
      // au char-select — hors de portée du map-server — de montrer les bonnes
      // couleurs dès la prochaine session.
      fx::palette_cache::SaveAll(OwnCharId(), g_local_variants, g_local_default);
      // 🔴 Un EFFACEMENT est honoré même l'éditeur ouvert. Il n'apporte aucune
      // couleur, donc il ne risque pas d'écraser un réglage en cours — alors
      // que le laisser en attente ferait REPOSER l'ancienne recette au tick
      // suivant, dès que `HasRecipe` retombe. C'est exactement ce qui rendait
      // le bouton « Supprimer mes couleurs » sans effet durable.
      if (g_local_variants.empty()) {
        g_remote.erase(gid);
        // L'éditeur a déjà retiré l'injection de son côté ; ne pas le refaire
        // pendant qu'il est ouvert évite un aller-retour visible.
        if (!g_local_editing) fx::palette_inject::ClearRecipe(gid);
        continue;
      }
      if (g_local_editing) continue;
      // Notre entrée est une COPIE de nos variantes : à partir d'ici, elle suit
      // exactement le même chemin d'application que celle d'un autre joueur.
      Remote& r = g_remote[gid];
      r.variants = g_local_variants;
      r.default_key = g_local_default;
      r.applied = false;
      r.attempts = 0;
      continue;
    }

    Remote& r = g_remote[gid];
    if (premiere) {
      r.variants.clear();
      r.default_key = 0;
      r.applied = false;  // le lot peut changer la variante posée
      r.attempts = 0;
    }
    if (!clear && body_key != 0) {
      r.variants[body_key] = recipe;
      if ((flags & fx::style_sync::kFlagDefault) || r.default_key == 0)
        r.default_key = body_key;
    }
    if (r.variants.empty()) {
      // Effacement : immédiat, il ne demande rien de l'acteur.
      fx::palette_inject::ClearRecipe(gid);
      fx::palette_inject::ClearHairPalette(gid);
      g_remote.erase(gid);
    }
  }
  PruneIfCrowded();
}

int StyleSync::ApplyPending(int budget) {
  int done = 0;
  for (auto it = g_remote.begin(); it != g_remote.end() && done < budget;) {
    const uint32_t gid = it->first;
    Remote& r = it->second;

    // 🔴 La variante se choisit sur le corps que l'acteur porte MAINTENANT, à
    // chaque passage. C'est ici, et nulle part ailleurs, que se fait la
    // correspondance corps -> recette : le serveur nous a donné toutes les
    // variantes sans savoir laquelle s'applique, parce que lui ne voit pas le
    // sprite.
    const uint32_t body_key = CurrentBodyKey(gid);
    uint32_t choix = 0;
    const ro::PaletteRecipe* recette = PickVariant(r, body_key, &choix);
    if (recette == nullptr) {
      // Plus aucune variante : il n'y a rien à poser, et garder l'entrée ferait
      // rejouer ce calcul à chaque tick.
      it = g_remote.erase(it);
      continue;
    }

    // CETTE version est déjà posée : le détour de `palette_inject` la
    // ré-applique tout seul quand l'acteur est reconstruit (changement de carte,
    // de tenue). Rien à faire — et surtout pas un SetRecipe de plus, qui
    // allouerait un bloc de palette supplémentaire à chaque passage,
    // définitivement (cf. palette_inject.cc).
    //
    // 🔴 Le test porte sur `r.applied`, PAS seulement sur `HasRecipe` : ce
    // dernier reste vrai d'une version à l'autre, et s'y fier seul gèlerait
    // l'acteur sur la première recette reçue. Et sur `applied_key` : sans lui, un
    // joueur qui change de corps garderait la recette de l'ancien.
    if (r.applied && r.applied_key == choix &&
        fx::palette_inject::HasRecipe(gid)) {
      ++it;
      continue;
    }

    fx::palette_base::Body body;
    const fx::palette_base::Status st =
        fx::palette_base::BuildForGid(gid, recette->palette_id, &body);
    if (st == fx::palette_base::kNoSprite) {
      // L'acteur n'est pas encore monté côté client. C'est le cas NORMAL juste
      // après l'annonce : on réessaiera au prochain tick, sans compter d'échec.
      ++it;
      continue;
    }
    // 🔴 On ne décompte que ce qui a COÛTÉ. Une base ressortie du cache — le cas
    // de très loin le plus fréquent sur une carte peuplée, où vingt joueurs se
    // partagent une poignée d'apparences — ne prend aucun temps et ne doit donc
    // pas prendre de budget. Compter les appels au lieu du travail faisait
    // attendre dix-neuf joueurs pour la construction d'un seul : à la connexion,
    // ils apparaissaient une seconde dans leurs couleurs d'origine.
    if (!body.cached) ++done;
    if (st != fx::palette_base::kOk) {
      if (++r.attempts >= kMaxAttempts) {
        LogDiag("[palette] gid={} abandonné après {} essais (statut {})", gid,
                r.attempts, static_cast<int>(st));
        it = g_remote.erase(it);
        continue;
      }
      ++it;
      continue;
    }

    // Silence quand tout va bien : il y a une pose par joueur visible, et
    // annoncer chacune noierait le journal sur une carte peuplée. Ce qui mérite
    // une ligne, c'est une base que la recette ne peut presque pas TEINDRE :
    // trop peu de pixels appartiennent à une rampe, le joueur verra ses couleurs
    // d'origine et aucune erreur ne sera remontée nulle part.
    //
    // Le seuil est volontairement bas (un dixième) : les corps de 4e classe ont
    // légitimement 40 à 62 % de pixels noircis, donc une couverture partielle
    // est NORMALE et ne doit pas parler. Une fois par CORPS, pas par joueur —
    // vingt joueurs qui portent la même apparence, c'est le même défaut.
    if (body.pixels_total > 0 &&
        body.pixels_covered * 10 < body.pixels_total) {
      static std::set<uint32_t> corps_signales;
      if (corps_signales.insert(body_key).second) {
        LogDiag("[palette] corps={:08x} : couverture {}/{} ({} rampes) — la "
                "recette ne teindra presque rien (gid {})",
                body_key, body.pixels_covered, body.pixels_total,
                body.ramp_count, gid);
      }
    }
    fx::palette_inject::SetRecipe(gid, body.base.data(), body.ramps,
                                  body.ramp_count, *recette);
    fx::palette_inject::SetHairPalette(gid, recette->hair_palette_id);
    // 🔴 La COIFFURE de la recette n'est pas posée, et ce n'est pas un oubli :
    // le serveur l'a déjà appliquée par `pc_changelook`, et son ZC_SPRITE_CHANGE
    // natif est arrivé avant nous. L'acteur la porte donc déjà — la reposer
    // referait le travail du jeu, avec le rechargement de texture que cela
    // suppose.
    r.applied = true;
    r.applied_key = choix;
    ++it;
  }
  return done;
}

// ── Le corps a changé sous la palette ────────────────────────────────────────
//
// 🔴 Une recette ne désigne ses couleurs que par des INDEX, et un index ne veut
// rien dire hors du sprite où il a été mesuré. Le bloc de 1024 octets qu'on pose
// sur un acteur ne vaut donc que pour SON corps du moment. Quand le client en
// monte un autre — monture, costume de corps, classe qui change — rien ne le
// disait : le détour reposait fidèlement notre chemin de palette, et le rendu
// continuait d'appliquer la table de l'ancien corps par-dessus le nouveau
// sprite. D'où des couleurs délavées ou franchement fausses, jusqu'à ce que le
// joueur revalide son style à la main (observé sur une monture le 2026-08-15).
//
// ⚠ Le pantin de l'éditeur, lui, suivait déjà le sprite — c'est ce qui rendait
// le défaut lisible : l'aperçu montrait la bonne interprétation pendant que le
// personnage en portait une périmée. Deux chemins pour la même règle, un seul
// des deux la respectait.
int StyleSync::RefreshChangedBodies() {
  uint32_t gids[32];
  const int n = fx::palette_inject::PollStaleSprites(gids, 32);
  for (int i = 0; i < n; ++i) {
    const uint32_t gid = gids[i];

    // Une recette connue : il suffit de la redéclarer « à poser ». La boucle
    // d'application rebâtira la base sur le NOUVEAU sprite et refera le bloc.
    auto it = g_remote.find(gid);
    if (it != g_remote.end()) {
      it->second.applied = false;
      it->second.attempts = 0;  // le corps précédent n'a rien à léguer
      continue;
    }

    // Les nôtres, quand elles viennent du cache local : elles n'ont jamais
    // transité par le registre, et personne ne les reposerait.
    //
    // ⚠ On ne consulte PAS `g_local_editing` ici. Ce qu'on repose est la
    // dernière recette VALIDÉE, jamais les curseurs en cours — c'est donc bien
    // l'apparence que le joueur a partagée, celle que les autres voient déjà.
    // S'en abstenir pendant l'édition laisserait justement le personnage sur ses
    // couleurs périmées au moment où le joueur les regarde.
    if (gid == OwnGid() && !g_local_variants.empty()) {
      Remote r;
      r.variants = g_local_variants;
      r.default_key = g_local_default;
      g_remote[gid] = r;
      continue;
    }

    // Reste la réparation automatique : une recette NEUTRE, calculée sur le corps
    // d'avant. On la retire — l'acteur revient à son rendu natif — et on
    // réautorise l'examen, qui rejugera le nouveau corps au tick suivant. Sans
    // cette remise à zéro, la garde « il a déjà une recette » d'`AutoRepair` la
    // prendrait pour un travail fait et n'y reviendrait jamais.
    fx::palette_inject::ClearRecipe(gid);
    g_repair_seen.erase(gid);
    // 🔴 `g_first_seen`, en revanche, n'est PAS touché : son sursis sert à
    // laisser le réseau annoncer une recette au SPAWN. Ici l'acteur est là depuis
    // longtemps, et le remettre à zéro ne ferait que prolonger d'une seconde et
    // demie un corps noir.
  }
  return n;
}

// ── Réparation automatique des corps que les palettes du client abîment ──────
//
// 🔴 Le problème, et pourquoi il ne concerne PAS que les joueurs qui ouvrent
// l'éditeur. Une palette de vêtement (`몸\body_<n>.pal`) ne définit qu'une PARTIE
// des 256 index — mesuré en mémoire vive sur `body_56` : 126 entrées sur 255 y
// sont noires. Les corps de 3e/4e classe utilisent massivement les autres, et
// tombent donc en silhouette noire : jusqu'à 62 % des pixels d'un imperial_guard.
//
// La fusion `sprite ⊕ serveur` répare exactement ça, et elle est SÛRE par
// construction : elle ne rapatrie du sprite que les plages entièrement noires,
// donc jamais une ombre ni un contour voulus (20,04 % → 1,26 % sur les 421
// corps).
//
// ── Ce qui borne l'intervention ──────────────────────────────────────────────
// On ne dévie du natif que si l'on peut MONTRER que ça répare : la fusion doit
// récupérer au moins `kRepairMinPct` des pixels peints. Un corps que les
// palettes du client rendent correctement garde donc un rendu strictement natif
// — rien n'est injecté, aucun fichier n'est touché.
//
// Et jamais pour un porteur de recette : ses couleurs à lui priment.
constexpr int kRepairMinPct = 2;
// Acteurs EXAMINÉS par tick. Chaque examen coûte l'analyse d'un `.spr` ; c'est
// le même budget que l'application des recettes, et pour la même raison.
constexpr int kRepairBudget = 1;

// 🔴 Sursis avant de réparer un acteur qu'on vient de voir apparaître.
//
// La réparation pose une recette NEUTRE — les couleurs du sprite. Si elle
// devance la recette que le serveur envoie au spawn, le joueur voit ces
// couleurs-là pendant une seconde, puis les siennes : un clignotement à chaque
// connexion. On laisse donc au réseau le temps de parler. Un corps abîmé qui
// n'a pas de recette reste noir une seconde de plus, ce qui ne coûte rien ;
// l'inverse se voyait à chaque fois.
constexpr unsigned long kRepairGraceMs = 1500;

int StyleSync::AutoRepair(int budget) {
  uint32_t gids[256];
  const int n = fx::palette_inject::KnownGids(gids, 256);
  int done = 0;
  for (int i = 0; i < n && done < budget; ++i) {
    const uint32_t gid = gids[i];
    if (g_repair_seen.count(gid)) continue;

    // Sursis : laisser au serveur le temps d'annoncer une éventuelle recette,
    // sinon la réparation neutre s'affiche d'abord et le joueur voit ses
    // couleurs arriver en deux temps.
    const unsigned long now = GetTickCount();
    auto seen = g_first_seen.find(gid);
    if (seen == g_first_seen.end()) {
      g_first_seen[gid] = now;
      continue;
    }
    if (now - seen->second < kRepairGraceMs) continue;
    // Un porteur de recette est déjà servi — et le sera encore après un
    // changement de carte, le détour la reposant tout seul.
    if (fx::palette_inject::HasRecipe(gid)) continue;
    // Une recette en attente d'application : ne pas lui griller la place avec
    // une réparation neutre qu'elle remplacerait aussitôt.
    if (g_remote.count(gid)) continue;

    fx::palette_base::Body body;
    const fx::palette_base::Status st =
        fx::palette_base::BuildForGid(gid, &body);
    // Acteur pas encore monté : on réessaiera, sans compter d'échec ni marquer.
    if (st == fx::palette_base::kNoSprite) continue;
    ++done;
    if (st != fx::palette_base::kOk || body.pixels_total <= 0) {
      g_repair_seen[gid] = 0;
      continue;
    }

    const int repaired = body.pixels_black_native - body.pixels_black_merged;
    if (repaired * 100 < body.pixels_total * kRepairMinPct) {
      g_repair_seen[gid] = 0;  // ce corps se rend bien tout seul
      continue;
    }
    // Recette NEUTRE : on ne change aucune couleur choisie, on rend seulement
    // visibles les index que la palette du serveur laissait noirs.
    const ro::PaletteRecipe neutre;
    fx::palette_inject::SetRecipe(gid, body.base.data(), body.ramps,
                                  body.ramp_count, neutre);
    g_repair_seen[gid] = 1;
    LogDebug("[palette] gid={} réparé : {} px noirs sur {} récupérés", gid,
            repaired, body.pixels_total);
  }
  return done;
}

// Pose NOS couleurs depuis le cache local, sans attendre le serveur.
//
// 🔴 C'est ce qui supprime le clignotement de la connexion. Le serveur nous
// renvoie bien notre recette, mais après le handshake d'intégrité — soit une
// bonne seconde après l'apparition de l'acteur. D'ici là on affichait autre
// chose. Le cache, lui, est déjà sur le disque et n'attend personne.
//
// Le serveur reste la source de vérité : sa version écrase celle-ci dès
// qu'elle arrive. En cas de désaccord (couleurs changées depuis une autre
// machine), le joueur voit donc brièvement les anciennes — infiniment moins
// gênant que de voir un corps qui n'est pas le sien.
void StyleSync::RestoreLocalFromCache() {
  const uint32_t gid = OwnGid();
  const uint32_t cid = OwnCharId();
  if (gid == 0 || cid == 0) return;  // pas encore en jeu
  if (g_restored_cid == cid) return;
  if (fx::palette_inject::HasRecipe(gid)) { g_restored_cid = cid; return; }

  std::map<uint32_t, ro::PaletteRecipe> variants;
  uint32_t defaut = 0;
  if (!fx::palette_cache::LoadAll(cid, &variants, &defaut) || variants.empty()) {
    g_restored_cid = cid;  // ce personnage n'a pas de couleurs : rien à faire
    return;
  }

  // La variante du corps qu'il porte à cet instant — il peut très bien se
  // reconnecter en selle.
  Remote provisoire;
  provisoire.variants = variants;
  provisoire.default_key = defaut;
  uint32_t choix = 0;
  const ro::PaletteRecipe* recette =
      PickVariant(provisoire, CurrentBodyKey(gid), &choix);
  if (recette == nullptr) {
    g_restored_cid = cid;
    return;
  }

  fx::palette_base::Body body;
  // Acteur pas encore monté : on réessaiera au prochain tick.
  if (fx::palette_base::BuildForGid(gid, recette->palette_id, &body) !=
      fx::palette_base::kOk)
    return;

  fx::palette_inject::SetRecipe(gid, body.base.data(), body.ramps,
                                body.ramp_count, *recette);
  fx::palette_inject::SetHairPalette(gid, recette->hair_palette_id);
  // 🔴 Renseigner AUSSI les variantes locales, celles qui amorcent l'éditeur.
  //
  // Sans ça, elles n'étaient posées que par l'écho du SERVEUR — plus lent que le
  // cache. Un joueur qui ouvrait l'éditeur dans cet intervalle l'amorçait à
  // vide, et l'amorçage ne se faisant qu'une fois, ses couleurs restaient
  // ineditables pour toute la session. Le cache sait déjà tout : il doit donc
  // servir les deux usages, pas seulement l'affichage.
  g_local_variants = variants;
  g_local_default = defaut;
  g_restored_cid = cid;
  LogDebug("[palette] {} variante(s) restaurée(s) du cache local (cid={})",
          variants.size(), cid);
}

// Changement de PERSONNAGE sans quitter le client.
//
// 🔴 Notre registre de recettes est indexé par GID — c'est-à-dire par l'AID, qui
// ne change PAS d'un personnage à l'autre du même compte — et rien ne le vidait.
// Le personnage suivant héritait donc de la palette du précédent : un novice
// fraîchement créé arrivait dans les couleurs d'un 4e classe. Le symptôme est
// caractéristique et vaut diagnostic : chez les AUTRES, l'apparence était juste
// — le serveur, lui, range bien le style par personnage. Un décalage qui n'existe
// que sur son propre écran désigne toujours un état que nous seuls tenons.
//
// ⚠ Et le serveur ne pouvait pas nous rattraper : à un personnage SANS style il
// n'envoie rien du tout. Une absence de message ne corrige rien — c'est
// précisément pourquoi cette purge doit être faite ici, et non attendue de lui.
//
// Pire, la garde « il a déjà une recette » de `RestoreLocalFromCache` prenait cet
// héritage pour un travail déjà fait, et s'abstenait donc de charger la bonne.
//
// ⚠ Les recettes des AUTRES joueurs ne sont pas purgées, et c'est délibéré :
// elles sont indexées par leur propre AID, le serveur les renvoie à chaque entrée
// en zone, et de ce point de vue un changement de personnage n'est qu'un
// changement de carte de plus.
void StyleSync::ForgetPreviousCharacter() {
  const uint32_t cid = OwnCharId();
  if (cid == 0) return;              // char-select : rien à conclure
  if (cid == g_session_cid) return;  // le cas de très loin le plus fréquent
  const uint32_t precedent = g_session_cid;
  g_session_cid = cid;

  const uint32_t gid = OwnGid();

  // ── 🔴🔴 L'INJECTION HÉRITÉE, et pourquoi elle survivait ───────────────────
  //
  // Le GID vaut l'AID : d'un personnage à l'autre du même compte, c'est le MÊME
  // acteur du point de vue de tout ce que nous tenons. Une recette posée pour le
  // précédent reste donc appliquée au suivant — et le suivant n'a rien pour la
  // chasser s'il n'a pas de style à lui. Symptôme rapporté le 2026-08-16, et il
  // portait sa propre explication : le défaut ne se produisait QUE sur un
  // personnage sans couleurs. Avec des couleurs, `SetRecipe` remplaçait le bloc
  // et masquait tout.
  //
  // 🔴 Deux gardes se sont dérobées ensemble, et il fallait les deux pour voir
  // le bug :
  //   * la sortie « première entrée en jeu de la session » ci-dessus — or
  //     `ForgetLocalActor` remet `g_session_cid` à zéro au char-select, donc TOUT
  //     retour en jeu ressemble à une première entrée ;
  //   * la purge du char-select elle-même, gardée par `ActorAlive`, qui relit un
  //     GID dans une mémoire fraîchement libérée : elle peut parfaitement encore
  //     y trouver la bonne valeur et conclure que l'acteur vit toujours.
  //
  // Le changement de `char_id` est, lui, un signal FRANC : il ne dépend d'aucun
  // battement ni d'aucune mémoire libérée. C'est donc ici que la purge doit
  // vivre, et non plus seulement au char-select.
  //
  // ⚠ `ResetActor` et pas `ClearRecipe` : ce dernier rendrait à l'acteur le
  // chemin de palette MÉMORISÉ, qui est à cet instant celui du personnage
  // précédent — on repeindrait le nouveau avec la couleur de vêtement de son
  // prédécesseur. `ResetActor` fait RECALCULER les chemins par le natif, depuis
  // l'état courant de l'acteur.
  //
  // ⚠ Et seulement s'il y a quelque chose à défaire : sur une entrée en jeu
  // normale, purger le registre natif ferait perdre le chemin de palette capturé,
  // donc construire la prochaine base sur le sprite nu — c'est-à-dire diverger
  // des autres clients pour le même personnage.
  if (gid != 0 && fx::palette_inject::HasRecipe(gid))
    fx::palette_inject::ResetActor(gid);

  if (precedent == 0 && gid == 0) return;

  if (gid != 0) {
    // Ces états portent sur le corps du personnage PRÉCÉDENT : les laisser
    // ferait juger le nouveau sur l'analyse de l'ancien.
    g_remote.erase(gid);
    g_repair_seen.erase(gid);
    g_first_seen.erase(gid);
  }
  // Nos variantes locales servent à amorcer l'éditeur : les garder ferait
  // proposer au nouveau venu les couleurs de son prédécesseur.
  g_local_variants.clear();
  g_local_default = 0;
  // Et la restauration depuis le cache doit être REFAITE, avec la clé du
  // nouveau personnage.
  g_restored_cid = 0;
  LogDebug("[palette] personnage changé ({} -> {}) : style précédent oublié",
          precedent, cid);
}

// Le retour au char-select : notre acteur vient de cesser d'exister.
//
// 🔴 C'est LE bon moment, et le seul. Tout ce que nous savons de cet acteur est
// indexé par son GID — c'est-à-dire l'AID, qui ne change pas d'un personnage à
// l'autre du même compte — donc sans cette purge le suivant hérite de la palette
// du précédent. Le symptôme valait diagnostic : chez les autres joueurs
// l'apparence était juste, et le char-select aussi. Le serveur range bien par
// personnage, notre cache local aussi ; un décalage visible sur son seul écran
// désigne toujours un état que nous sommes seuls à tenir.
//
// ⚠ Et le serveur ne pouvait pas nous rattraper : à un personnage SANS style il
// n'envoie rien du tout, et une absence de message ne corrige rien.
//
// Ici l'acteur n'existe plus : `ForgetActor` est donc obligatoire, `ClearRecipe`
// écrirait dans un pointeur caduc et rendrait de surcroît un chemin périmé.
void StyleSync::ForgetLocalActor() {
  const uint32_t gid = OwnGid();
  if (gid == 0) return;
  // 🔴🔴 L'ACTEUR EXISTE-T-IL ENCORE ? Cette fonction n'est appelée que sur le
  // chemin « hors du monde », et ce chemin se décide sur `IsGameActive()`, qui
  // n'est qu'un battement de fraîcheur d'UNE SECONDE. Or un battement périmé ne
  // dit pas « le joueur a quitté le monde » : il dit « aucune frame n'a été
  // rendue depuis une seconde ». Une pause au débogueur, une longue mise en
  // veille de la fenêtre, un gel quelconque suffisent.
  //
  // Sans cette garde, la première frame après la reprise passait par ici et
  // oubliait la recette d'un acteur BIEN VIVANT. Comme `ForgetActor` ne touche
  // pas à l'acteur — c'est sa raison d'être — celui-ci gardait notre chemin de
  // palette, pour lequel plus personne ne répondait : le natif cherchait alors
  // `palette\bourgeon\<gid>.pal`, ne le trouvait pas, et rendait une table vide.
  // Une palette vide, c'est un CORPS ENTIÈREMENT NOIR — la tête, elle, ne bouge
  // pas, sa palette étant un vrai fichier. Mesuré au débogueur le 2026-08-13 :
  // chemin intact dans l'acteur, détours en place, et pourtant noir.
  //
  // ⚠ Et le vrai char-select reste couvert : l'acteur y est bel et bien
  // détruit, et cette fonction est rappelée à CHAQUE frame passée hors du
  // monde. Même si la mémoire libérée gardait un instant un GID plausible, les
  // frames suivantes verront l'acteur disparu et la purge se fera.
  if (fx::palette_inject::ActorAlive(gid)) return;
  if (!fx::palette_inject::HasRecipe(gid) && g_session_cid == 0) return;
  fx::palette_inject::ForgetActor(gid);
  g_remote.erase(gid);
  g_repair_seen.erase(gid);
  g_first_seen.erase(gid);
  g_local_variants.clear();
  g_local_default = 0;
  g_local_editing = false;
  g_restored_cid = 0;
  g_session_cid = 0;
}

void StyleSync::OnRenderUI() {
  // Idempotent, et sur le FIL DE RENDU comme l'exige la pose des détours.
  fx::palette_inject::EnsureInstalled();
  // 🔴 AVANT la restauration : celle-ci se croirait déjà faite en voyant la
  // recette héritée du personnage précédent.
  ForgetPreviousCharacter();
  RestoreLocalFromCache();

  // ── Le changement de corps se traite à la FRAME, pas au battement ─────────
  //
  // 🔴 `OnTick` est bridé à ~10 Hz : enfourcher une monture y laissait voir
  // jusqu'à un dixième de seconde de palette d'origine avant que la recette ne
  // reprenne la main. Ce n'est pas un délai de calcul — c'est un délai
  // d'APERÇU du problème, et il se supprime en regardant plus tôt.
  //
  // Le coût est une lecture de chaîne par porteur de recette et par frame, soit
  // exactement ce que fait déjà le chien de garde des chemins. Ce qui coûte —
  // l'analyse du `.spr` — n'a lieu que sur un vrai changement, et le cache de
  // bases le rend gratuit dès la deuxième fois qu'on enfourche.
  //
  // ⚠ `ApplyPending` est appelée ici EN PLUS du battement, jamais à sa place :
  // un acteur qui n'est pas encore monté doit continuer d'être réessayé, et
  // c'est le battement qui s'en charge à son rythme.
  if (RefreshChangedBodies() > 0) ApplyPending(kApplyBudget);
}

void StyleSync::OnRenderLoginUI() {
  // 🔴 Poser les détours AVANT que le monde n'existe. Ils ne servent à rien sur
  // ces écrans, mais ils doivent être en place à l'instant où le premier acteur
  // est monté — sinon le détour manque sa reconstruction et les couleurs
  // arrivent avec une seconde de retard.
  fx::palette_inject::EnsureInstalled();
  // Nous sommes entre deux personnages : rien de l'ancien ne doit franchir cet
  // écran. Idempotent — après le premier passage il n'y a plus rien à oublier.
  ForgetLocalActor();
}

void StyleSync::OnTick() {
  // (La restauration locale, elle, tente sa chance à CHAQUE frame — cf.
  // OnRenderUI. La refaire ici ne coûterait qu'une comparaison, mais ne
  // gagnerait rien.)
  //
  // 🔴 D'ABORD le chien de garde : une recette POSÉE qui ne s'affiche plus est
  // un défaut visible, là où les deux étapes suivantes ne font qu'accélérer un
  // affichage encore à venir. Ne rien reposer est le cas courant, et il ne
  // coûte qu'une comparaison de chemin par porteur de recette.
  fx::palette_inject::ReassertPaths();
  // 🔴 AVANT l'application : c'est elle qui remet en file les corps qui ont
  // changé, et `ApplyPending` saute toute entrée déjà posée. L'ordre inverse
  // ferait attendre un tick de plus à chaque enfourchement.
  RefreshChangedBodies();
  if (!g_remote.empty() && ApplyPending(kApplyBudget) > 0) return;
  // Les recettes ensuite : elles portent un choix explicite du joueur, la
  // réparation n'est qu'un défaut.
  AutoRepair(kRepairBudget);
}
