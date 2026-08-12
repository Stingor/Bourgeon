#include "features/fx/style_sync.h"

#include <Windows.h>  // SEH autour de la lecture des globales du client

#include <cstring>
#include <iterator>  // std::next
#include <map>

#include "bourgeon.h"  // SendPacket / RegisterRecvOpcode
#include "features/fx/palette_base.h"
#include "features/fx/palette_cache.h"
#include "features/fx/palette_inject.h"
#include "features/systems/bourgeon_opcodes.h"
#include "features/windows/palette_editor.h"  // ouverture pilotée par un NPC
#include "utils/log_console.h"

namespace {

// AID de notre compte — et GID de notre acteur. Même adresse que dans
// quick_cast / make_item_window / char_select : la constante est recopiée là où
// elle sert, comme partout dans ce projet.
constexpr uintptr_t kOwnAidAddr = 0x015FB9A4;
// 🔴 `g_Own_CharId`, à ne PAS confondre avec l'AID ci-dessus. C'est lui qui
// identifie un PERSONNAGE — et c'est la même valeur que `CHARACTER_INFO+0x00` au
// char-select, donc la seule clé de cache que les deux écrans partagent.
constexpr uintptr_t kOwnCharIdAddr = 0x015FB9A8;

uint32_t ReadGlobalU32(uintptr_t addr) {
  __try {
    return *reinterpret_cast<const uint32_t*>(addr);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

uint32_t OwnGid() { return ReadGlobalU32(kOwnAidAddr); }
uint32_t OwnCharId() { return ReadGlobalU32(kOwnCharIdAddr); }

// La recette d'un AUTRE joueur, en attente d'être posée sur son acteur.
struct Remote {
  ro::PaletteRecipe recipe;
  bool clear = false;
  // 🔴 Cette VERSION-CI a-t-elle été posée sur l'acteur ? Remis à false à chaque
  // paquet reçu, et c'est essentiel : sans ce drapeau, on ne se fiait qu'à
  // « l'acteur a-t-il DÉJÀ une recette », ce qui est vrai dès la première. Un
  // joueur qui retouchait ses couleurs et les re-partageait restait donc figé,
  // chez les autres, sur sa toute première version.
  bool applied = false;
  // Tentatives d'application déjà faites. Un acteur qu'on n'arrive pas à monter
  // (sprite absent, corps sans palette) ne doit pas être réessayé indéfiniment :
  // chaque essai relit et reparse un `.spr`, ce qui n'est pas gratuit.
  int attempts = 0;
};

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

// Notre propre recette, telle que le serveur nous la renvoie, et l'état de
// l'éditeur qui décide si on a le droit de la reposer.
ro::PaletteRecipe g_local_recipe;
bool g_has_local = false;
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
  LogDiag("[palette] registre élagué : {} -> {}", avant, g_remote.size());
}

void SendOne(const ro::PaletteRecipe& recipe, bool clear) {
  uint8_t pkt[fx::style_sync::kCzBytes];
  std::memset(pkt, 0, sizeof(pkt));
  *reinterpret_cast<uint16_t*>(&pkt[0]) = bopcodes::kStyle;
  *reinterpret_cast<uint16_t*>(&pkt[2]) =
      static_cast<uint16_t>(fx::style_sync::kCzBytes);
  pkt[4] = fx::style_sync::kWireVersion;
  pkt[5] = clear ? fx::style_sync::kFlagClear : 0;
  const int16_t pal = clear ? static_cast<int16_t>(-1) : recipe.palette_id;
  const int16_t hair = clear ? static_cast<int16_t>(-1) : recipe.hair_palette_id;
  const int16_t coupe = clear ? static_cast<int16_t>(-1) : recipe.hair_style;
  pkt[6] = static_cast<uint8_t>(pal & 0xFF);
  pkt[7] = static_cast<uint8_t>((pal >> 8) & 0xFF);
  pkt[8] = static_cast<uint8_t>(hair & 0xFF);
  pkt[9] = static_cast<uint8_t>((hair >> 8) & 0xFF);
  pkt[10] = static_cast<uint8_t>(coupe & 0xFF);
  pkt[11] = static_cast<uint8_t>((coupe >> 8) & 0xFF);
  if (!clear) WriteAdjusts(&pkt[12], recipe);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

}  // namespace

namespace fx {
namespace style_sync {

bool Available() { return g_instance != nullptr; }

void SetLocalEditing(bool editing) { g_local_editing = editing; }

bool LocalRecipe(ro::PaletteRecipe* out) {
  if (!out || !g_has_local) return false;
  *out = g_local_recipe;
  return true;
}

void ForgetLocal() {
  g_has_local = false;
  g_local_recipe = ro::PaletteRecipe();
  const uint32_t self = OwnGid();
  if (self != 0) g_remote.erase(self);
}

void SendRecipe(const ro::PaletteRecipe& recipe) {
  if (!g_instance) return;
  SendOne(recipe, /*clear=*/false);
  // Ce que le joueur vient de partager EST désormais sa recette de référence,
  // sans attendre que le serveur nous la renvoie.
  g_local_recipe = recipe;
  g_has_local = true;
  // Sans attendre l'écho du serveur : si la session se termine avant qu'il ne
  // revienne, le char-select afficherait encore l'ancienne apparence.
  fx::palette_cache::Save(OwnCharId(), &recipe);
}

void SendClear() {
  if (!g_instance) return;
  const ro::PaletteRecipe vide;
  SendOne(vide, /*clear=*/true);
  fx::palette_cache::Save(OwnCharId(), nullptr);
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

    Remote r;
    r.clear = (e[5] & fx::style_sync::kFlagClear) != 0;
    if (!r.clear) {
      r.recipe.palette_id = static_cast<int16_t>(
          static_cast<uint16_t>(e[6]) | (static_cast<uint16_t>(e[7]) << 8));
      r.recipe.hair_palette_id = static_cast<int16_t>(
          static_cast<uint16_t>(e[8]) | (static_cast<uint16_t>(e[9]) << 8));
      r.recipe.hair_style = static_cast<int16_t>(
          static_cast<uint16_t>(e[10]) | (static_cast<uint16_t>(e[11]) << 8));
      ReadAdjusts(e + 12, &r.recipe);
    }

    // ── Notre propre recette ─────────────────────────────────────────────
    // Elle nous revient au login (le serveur nous la repousse) et à chaque
    // partage (la diffusion de zone nous inclut). On la MÉMORISE toujours —
    // l'éditeur démarrera dessus au lieu d'une recette vide qui l'effacerait —
    // mais on ne la POSE que si l'éditeur est fermé : sinon on écraserait un
    // réglage en cours par la dernière version validée, et le joueur verrait
    // ses curseurs sauter en arrière.
    if (gid == self) {
      g_has_local = !r.clear;
      g_local_recipe = r.recipe;
      // Le cache local suit le serveur, qui fait autorité. C'est ce qui permet
      // au char-select — hors de portée du map-server — de montrer les bonnes
      // couleurs dès la prochaine session.
      fx::palette_cache::Save(OwnCharId(), r.clear ? nullptr : &r.recipe);
      // 🔴 Un EFFACEMENT est honoré même l'éditeur ouvert. Il n'apporte aucune
      // couleur, donc il ne risque pas d'écraser un réglage en cours — alors
      // que le laisser en attente ferait REPOSER l'ancienne recette au tick
      // suivant, dès que `HasRecipe` retombe. C'est exactement ce qui rendait
      // le bouton « Supprimer mes couleurs » sans effet durable.
      if (r.clear) {
        g_remote.erase(gid);
        // L'éditeur a déjà retiré l'injection de son côté ; ne pas le refaire
        // pendant qu'il est ouvert évite un aller-retour visible.
        if (!g_local_editing) fx::palette_inject::ClearRecipe(gid);
        continue;
      }
      if (g_local_editing) continue;
      g_remote[gid] = r;
      continue;
    }

    if (r.clear) {
      // Effacement : immédiat, il ne demande rien de l'acteur.
      fx::palette_inject::ClearRecipe(gid);
      fx::palette_inject::ClearHairPalette(gid);
      g_remote.erase(gid);
      continue;
    }
    g_remote[gid] = r;  // remplace toute version précédente, compteur remis à 0
  }
  PruneIfCrowded();
}

int StyleSync::ApplyPending(int budget) {
  int done = 0;
  for (auto it = g_remote.begin(); it != g_remote.end() && done < budget;) {
    const uint32_t gid = it->first;
    Remote& r = it->second;

    // CETTE version est déjà posée : le détour de `palette_inject` la
    // ré-applique tout seul quand l'acteur est reconstruit (changement de carte,
    // de tenue). Rien à faire — et surtout pas un SetRecipe de plus, qui
    // allouerait un bloc de palette supplémentaire à chaque passage,
    // définitivement (cf. palette_inject.cc).
    //
    // 🔴 Le test porte sur `r.applied`, PAS seulement sur `HasRecipe` : ce
    // dernier reste vrai d'une version à l'autre, et s'y fier seul gèlerait
    // l'acteur sur la première recette reçue.
    if (r.applied && fx::palette_inject::HasRecipe(gid)) {
      ++it;
      continue;
    }

    fx::palette_base::Body body;
    const fx::palette_base::Status st =
        fx::palette_base::BuildForGid(gid, r.recipe.palette_id, &body);
    if (st == fx::palette_base::kNoSprite) {
      // L'acteur n'est pas encore monté côté client. C'est le cas NORMAL juste
      // après l'annonce : on réessaiera au prochain tick, sans compter d'échec.
      ++it;
      continue;
    }
    ++done;
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

    fx::palette_inject::SetRecipe(gid, body.base.data(), body.ramps,
                                  body.ramp_count, r.recipe);
    fx::palette_inject::SetHairPalette(gid, r.recipe.hair_palette_id);
    // 🔴 La COIFFURE de la recette n'est pas posée, et ce n'est pas un oubli :
    // le serveur l'a déjà appliquée par `pc_changelook`, et son ZC_SPRITE_CHANGE
    // natif est arrivé avant nous. L'acteur la porte donc déjà — la reposer
    // referait le travail du jeu, avec le rechargement de texture que cela
    // suppose.
    r.applied = true;
    ++it;
  }
  return done;
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
    LogDiag("[palette] gid={} réparé : {} px noirs sur {} récupérés", gid,
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

  ro::PaletteRecipe recipe;
  if (!fx::palette_cache::Load(cid, &recipe)) {
    g_restored_cid = cid;  // ce personnage n'a pas de couleurs : rien à faire
    return;
  }
  fx::palette_base::Body body;
  // Acteur pas encore monté : on réessaiera au prochain tick.
  if (fx::palette_base::BuildForGid(gid, recipe.palette_id, &body) !=
      fx::palette_base::kOk)
    return;

  fx::palette_inject::SetRecipe(gid, body.base.data(), body.ramps,
                                body.ramp_count, recipe);
  fx::palette_inject::SetHairPalette(gid, recipe.hair_palette_id);
  // 🔴 Renseigner AUSSI la recette locale, celle qui amorce l'éditeur.
  //
  // Sans ça, elle n'était posée que par l'écho du SERVEUR — plus lent que le
  // cache. Un joueur qui ouvrait l'éditeur dans cet intervalle l'amorçait à
  // vide, et l'amorçage ne se faisant qu'une fois, ses couleurs restaient
  // ineditables pour toute la session. Le cache sait déjà tout : il doit donc
  // servir les deux usages, pas seulement l'affichage.
  g_local_recipe = recipe;
  g_has_local = true;
  g_restored_cid = cid;
  LogDiag("[palette] couleurs restaurées du cache local (cid={})", cid);
}

void StyleSync::OnRenderUI() {
  // Idempotent, et sur le FIL DE RENDU comme l'exige la pose des détours.
  fx::palette_inject::EnsureInstalled();
  RestoreLocalFromCache();
}

void StyleSync::OnRenderLoginUI() {
  // 🔴 Poser les détours AVANT que le monde n'existe. Ils ne servent à rien sur
  // ces écrans, mais ils doivent être en place à l'instant où le premier acteur
  // est monté — sinon le détour manque sa reconstruction et les couleurs
  // arrivent avec une seconde de retard.
  fx::palette_inject::EnsureInstalled();
}

void StyleSync::OnTick() {
  // (La restauration locale, elle, tente sa chance à CHAQUE frame — cf.
  // OnRenderUI. La refaire ici ne coûterait qu'une comparaison, mais ne
  // gagnerait rien.)
  if (!g_remote.empty() && ApplyPending(kApplyBudget) > 0) return;
  // Les recettes ensuite : elles portent un choix explicite du joueur, la
  // réparation n'est qu'un défaut.
  AutoRepair(kRepairBudget);
}
