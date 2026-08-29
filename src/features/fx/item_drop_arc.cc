#include "features/fx/item_drop_arc.h"

#include <Windows.h>
#include <mmsystem.h>  // timeGetTime (winmm) — la MÊME horloge que le CItem

#include <cmath>
#include <cstdint>
#include <unordered_map>

#include "ragnarok/actor.h"      // rag::actor : position monde et instant de naissance
#include "ragnarok/game_scene.h"  // gamescene : mode -> gestionnaire -> acteurs, terrain
#include "ragnarok/globals.h"     // rag::ActiveModeIfReady, rag::Read
#include "ragnarok/stl_node.h"    // rag::listnode : le nœud de std::list du client
#include "utils/log_console.h"
#include "utils/memory_patch.h"  // mem::WriteCode

namespace {

// ── Adresses (client 20250716, no-ASLR : addr Ghidra == live) ────────────────
constexpr uintptr_t kCItemVtable = 0x010932ac;  // vtable de CItem
constexpr uintptr_t kCItemOnTick = 0x00d1d240;  // slot 1 : l'update par frame
constexpr int kSlotOnTick = 1;

// ── Champs propres au CItem ─────────────────────────────────────────────────
// Ceux de la position et de l'instant de naissance sont ceux de tout acteur et
// vivent dans `ragnarok/actor.h` : +0x10 X, +0x14 hauteur, +0x18 Z, +0x8C
// l'horodatage. Les trois ci-dessous n'appartiennent qu'à la classe des objets
// au sol — le `CItem` ne fait que 0x190 octets, il n'a pas les champs d'un
// CActorSprite complet.
constexpr int kItemAid     = 0x17c;  // AID du flooritem (alloué sous 2 000 000)
constexpr int kItemChute   = 0x180;  // int : 1 = chute native en cours
constexpr int kItemVitesse = 0x184;  // float : vitesse verticale initiale

// 🔴 La SIGNATURE d'une naissance, et pas seulement le drapeau. `CItem_Ctor`
// (0x00d1cfb0) n'initialise NI +0x180 NI +0x184 — il s'arrête à +0x16C — et il a
// huit appelants, dont trois hors du chemin de spawn. Un `CItem` né autrement
// présenterait donc à +0x180 ce qu'`operator new` a laissé là ; le prendre pour
// une chute reposerait l'objet quinze unités trop bas.
//
// L'init écrit les deux champs coup sur coup (0x00d1d4bb, 0x00d1d4d3) : le
// drapeau à 1 ET la vitesse à -0.6, cette dernière en immédiat dans le code
// (`mov dword ptr [edi+184h], 0BF19999Ah`). On compare donc les OCTETS de la
// constante — pas un flottant calculé, qui inviterait à discuter d'epsilon.
constexpr uint32_t kVitesseNaissance = 0xBF19999Au;  // -0.6f

// Hauteur de naissance posée par `CItem_InitFromItemSpawn` : le sol MOINS ceci.
// C'est ce qui nous donne l'altitude du terrain sans rien appeler (cf. l'en-tête).
constexpr float kNaissanceAuDessusDuSol = 15.0f;

// ── Les deux rebonds ────────────────────────────────────────────────────────
// Fractions de la durée du vol et de la crête de l'arc. Un objet qui retombe et
// s'arrête net a l'air de coller au sol ; deux rebonds courts suffisent à le
// poser. Ils se font sur place : la case d'arrivée est déjà atteinte.
constexpr float kRebond1Duree   = 0.32f;
constexpr float kRebond1Hauteur = 0.22f;
constexpr float kRebond2Duree   = 0.18f;
constexpr float kRebond2Hauteur = 0.05f;

// Durée de vol minimale. En deçà l'arc n'est plus qu'un saut d'une frame, et la
// division par la durée n'a plus de sens.
constexpr int kDureeMiniMs = 60;

// Jusqu'où chercher l'entité qui a lâché l'objet, en CASES. Le serveur pose le
// drop sur la case du monstre ou l'une des trois voisines ; deux cases et demie
// laissent la marge du sous-positionnement et d'un monstre qui bougeait encore,
// sans aller ramasser une entité d'un autre combat.
constexpr float kRayonEnCases = 2.5f;

// Au-delà de ce nombre d'arcs vivants, on fait le ménage des entrées qu'aucune
// fin d'arc n'a effacées (un objet ramassé en plein vol est détruit sans nous
// prévenir). Le seuil est haut exprès : la purge est un filet, pas le régime
// normal — un arc s'efface tout seul en moins d'une seconde.
constexpr size_t kSeuilPurge   = 64;
constexpr uint32_t kAgeMaxMs   = 5000;

using OnTickFn = char(__fastcall*)(void* self, void* edx);
OnTickFn g_ontick_natif = reinterpret_cast<OnTickFn>(kCItemOnTick);

// Le module, pour que le détour lise ses réglages. Posé par le constructeur,
// remis à zéro par le destructeur : un détour qui survivrait au module rend la
// main au natif au lieu de déréférencer un objet mort.
ItemDropArc* g_module = nullptr;

// Un objet en vol. `dst` est la position que le serveur a choisie — celle que le
// natif avait écrite à la naissance, et celle où l'on repose l'objet à la fin.
// `src` est l'entité dont il jaillit, quand on a su la trouver.
struct Arc {
  float    src_x, src_y, src_z;  // d'où l'objet part (src_y = le sol, là-bas)
  float    dst_x, dst_z;
  float    sol;     // altitude du terrain sous la case d'arrivée
  uint32_t depart;  // timeGetTime de la naissance (CItem+0x8C)
  uint32_t aid;     // 🔴 le contrôle d'identité : cf. le détour
};

// Indexée par l'adresse du CItem — le détour n'a que ça sous la main, et il faut
// pouvoir retrouver l'entrée sans parcourir quoi que ce soit.
std::unordered_map<void*, Arc> g_arcs;

// Direction du jaillissement, tirée de l'AID : stable d'une frame à l'autre
// (l'entrée n'est calculée qu'une fois, mais la stabilité vaut relecture) et
// différente d'un objet au suivant, pour qu'une pluie de butin parte en gerbe.
// Multiplication de Knuth puis étalement sur un tour complet.
float AngleDepuisAid(uint32_t aid) {
  const uint32_t melange = aid * 2654435761u;
  return static_cast<float>(melange >> 8) * (6.2831853f / 16777216.0f);
}

// Parabole normalisée : 0 au départ, 1 à mi-course, 0 à l'arrivée.
float Cloche(float p) { return 4.0f * p * (1.0f - p); }

void PurgerLesOublies(uint32_t maintenant) {
  if (g_arcs.size() < kSeuilPurge) return;
  for (auto it = g_arcs.begin(); it != g_arcs.end();) {
    if (maintenant - it->second.depart > kAgeMaxMs)
      it = g_arcs.erase(it);
    else
      ++it;
  }
}

// ── D'où l'objet jaillit : l'entité qui vient de le lâcher ──────────────────
//
// Le paquet de drop ne nomme personne — ni monstre, ni joueur. Il n'a pas à le
// faire : côté serveur, `mob_process_drop_list` pose le PREMIER objet sur la
// case même du monstre (`DIR_CENTER`) et les suivants sur les cases voisines, et
// le monstre est encore là quand le paquet arrive (les drops partent sans délai,
// l'unité n'est retirée qu'à +250 ms). L'entité la plus proche du point de chute
// EST donc celle qui a lâché l'objet, dans tous les cas ordinaires — y compris
// pour un objet qu'un joueur jette à ses pieds.
//
// ⚠ ON NE FILTRE PAS SUR LE TYPE (`acteur+0x314`). La correspondance de ce champ
// avec `clif_bl_type` est DÉDUITE et non vérifiée, et `CActorSprite_InitDefaults`
// (0x00c45f47) y écrit 4 par défaut — un filtre « monstre » y aurait rejeté au
// hasard. La proximité, elle, se mesure.
//
// Rendu : true si une source a été trouvée, avec sa position monde.
// 🔴 SEH et scalaires uniquement : la chaîne mode → gestionnaire → liste se rompt
// à chaque changement de carte, et `__try` interdit de toute façon le moindre
// objet à destructeur dans cette fonction.
bool TrouverEntiteSource(float dst_x, float dst_z,
                         float* out_x, float* out_y, float* out_z) {
  __try {
    void* gm = rag::ActiveModeIfReady();
    if (!gm) return false;
    void* mgr = rag::Read<void*>(gm, gamescene::kGmActorMgr);
    if (!mgr) return false;

    // Le rayon, en CASES : le serveur ne pose jamais un drop à plus d'une case
    // de son monstre, et convertir par le terrain évite de deviner combien vaut
    // une case en unités de monde. Terrain absent (chargement) : on renonce
    // plutôt que d'inventer une échelle.
    void* terrain = rag::Read<void*>(mgr, gamescene::kAmTerrain);
    if (!terrain) return false;
    const float cote =
        static_cast<float>(rag::Read<int>(terrain, gamescene::kTerrainCellSize));
    if (cote <= 0.0f) return false;
    const float rayon = cote * kRayonEnCases;

    float meilleure = rayon * rayon;  // distance² : pas de racine à tirer
    void* source = nullptr;

    auto examiner = [&](void* acteur) {
      if (acteur == nullptr) return;
      // Un objet au sol n'est pas une source — et rien ne garantit qu'il ne se
      // trouve pas dans cette liste-ci en plus de celle des nœuds de scène.
      if (rag::Read<uintptr_t>(acteur, 0) == kCItemVtable) return;
      const float dx = rag::Read<float>(acteur, rag::actor::kPosX) - dst_x;
      const float dz = rag::Read<float>(acteur, rag::actor::kPosZ) - dst_z;
      const float d2 = dx * dx + dz * dz;
      if (d2 >= meilleure) return;
      meilleure = d2;
      source = acteur;
    };

    // La std::list<Actor*> : nœud {next@0, prev@4, value@8}, CIRCULAIRE — on
    // s'arrête sur la sentinelle, jamais sur un nul. Garde-fou d'itérations au
    // cas où la liste serait corrompue.
    void* sentinelle = rag::Read<void*>(mgr, gamescene::kAmListHead);
    if (sentinelle) {
      int garde = 0;
      for (void* node = rag::Read<void*>(sentinelle, rag::listnode::kNext);
           node && node != sentinelle && garde < 4096;
           node = rag::Read<void*>(node, rag::listnode::kNext), ++garde) {
        examiner(rag::Read<void*>(node, rag::listnode::kValue));
      }
    }
    // 🔴 Le joueur local N'EST PAS dans la liste : il a son propre emplacement.
    // Sans cette ligne, un objet qu'on jette à ses pieds ne jaillirait de rien.
    examiner(rag::Read<void*>(mgr, gamescene::kAmOwnPlayer));

    if (source == nullptr) return false;
    *out_x = rag::Read<float>(source, rag::actor::kPosX);
    *out_y = rag::Read<float>(source, rag::actor::kPosY);
    *out_z = rag::Read<float>(source, rag::actor::kPosZ);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Écrit la position monde de l'objet. Les trois champs sont ceux de tout acteur.
void PoserPosition(uint8_t* champs, float x, float hauteur, float z) {
  *reinterpret_cast<float*>(champs + rag::actor::kPosX) = x;
  *reinterpret_cast<float*>(champs + rag::actor::kPosY) = hauteur;
  *reinterpret_cast<float*>(champs + rag::actor::kPosZ) = z;
}

char __fastcall OnTickDetour(void* self, void* edx) {
  uint8_t* champs = static_cast<uint8_t*>(self);
  int32_t* chute  = reinterpret_cast<int32_t*>(champs + kItemChute);

  // Le cas de loin le plus fréquent : un objet posé depuis longtemps, alors
  // qu'aucun arc ne court. Tous les objets d'une carte encombrée passent par
  // cette ligne, et par elle seule.
  if (g_arcs.empty() && *chute == 0) return g_ontick_natif(self, edx);

  const uint32_t aid = *reinterpret_cast<const uint32_t*>(champs + kItemAid);
  const bool naissance =
      *chute == 1 &&
      *reinterpret_cast<const uint32_t*>(champs + kItemVitesse) == kVitesseNaissance;

  auto it = g_arcs.find(self);
  // 🔴 Une entrée peut parler d'un AUTRE objet : un `CItem` ramassé en plein vol
  // est détruit sans nous prévenir, et `operator new` peut rendre la même adresse
  // au suivant. L'AID tranche — et une naissance sur une adresse déjà connue est
  // le même cas, vu de l'autre bout.
  if (it != g_arcs.end() && (it->second.aid != aid || naissance)) {
    g_arcs.erase(it);
    it = g_arcs.end();
  }

  const bool actif = g_module != nullptr && g_module->enabled();

  if (it == g_arcs.end()) {
    // Rien à reprendre, et rien à intercepter : soit le module est éteint (le
    // natif garde alors SA chute, intacte), soit l'objet était déjà posé.
    if (!actif || !naissance) return g_ontick_natif(self, edx);

    const uint32_t maintenant = ::timeGetTime();
    PurgerLesOublies(maintenant);  // AVANT l'insertion : elle invaliderait tout

    Arc arc;
    arc.dst_x  = *reinterpret_cast<const float*>(champs + rag::actor::kPosX);
    arc.dst_z  = *reinterpret_cast<const float*>(champs + rag::actor::kPosZ);
    // Le natif vient d'écrire « sol - 15 » : l'altitude du terrain sous la case
    // d'arrivée s'en déduit exactement, sans appeler Terrain_GetHeightAt.
    arc.sol    = *reinterpret_cast<const float*>(champs + rag::actor::kPosY) +
              kNaissanceAuDessusDuSol;
    arc.depart = *reinterpret_cast<const uint32_t*>(champs + rag::actor::kAnimStart);
    arc.aid    = aid;

    // L'objet jaillit de l'ENTITÉ qui l'a lâché quand on la trouve — c'est tout
    // l'effet recherché. À défaut (aucune entité dans le rayon, terrain pas
    // encore là), il part d'à côté de sa case, dans une direction tirée de son
    // AID : moins juste, mais toujours mieux qu'une chute verticale, et deux
    // objets de la même salve ne se superposent pas.
    if (!TrouverEntiteSource(arc.dst_x, arc.dst_z, &arc.src_x, &arc.src_y,
                             &arc.src_z)) {
      const float angle = AngleDepuisAid(aid);
      const float ecart = *g_module->p_kick();
      arc.src_x = arc.dst_x + std::cos(angle) * ecart;
      arc.src_z = arc.dst_z + std::sin(angle) * ecart;
      arc.src_y = arc.sol;
    }

    // Le natif lâche prise : son intégrateur sort désormais dès son premier test,
    // après avoir fait le seul travail qui nous reste utile (le scintillement).
    *chute = 0;
    it = g_arcs.emplace(self, arc).first;
  }

  const Arc arc = it->second;  // copie : l'appel natif ci-dessous peut tout bouger
  const char resultat = g_ontick_natif(self, edx);

  // Le module vient d'être décoché alors que l'objet était en l'air : on le
  // repose au lieu de le laisser flotter jusqu'au ramassage.
  if (!actif) {
    PoserPosition(champs, arc.dst_x, arc.sol, arc.dst_z);
    g_arcs.erase(self);
    return resultat;
  }

  const int duree = *g_module->p_duration_ms() > kDureeMiniMs
                        ? *g_module->p_duration_ms()
                        : kDureeMiniMs;
  const float hauteur = *g_module->p_height();
  const uint32_t vol      = static_cast<uint32_t>(duree);
  const uint32_t rebond1  = static_cast<uint32_t>(duree * kRebond1Duree);
  const uint32_t rebond2  = static_cast<uint32_t>(duree * kRebond2Duree);
  const uint32_t ecoule   = ::timeGetTime() - arc.depart;

  float x = arc.dst_x, z = arc.dst_z, y = arc.sol;
  if (ecoule < vol) {
    // Le vol. L'horizontale suit une décélération (l'objet est éjecté vite puis
    // se pose), la verticale une cloche. 🔴 La hauteur MONTE quand la coordonnée
    // DIMINUE — même convention que le saut du personnage.
    const float p    = static_cast<float>(ecoule) / static_cast<float>(vol);
    const float reste = 1.0f - p;
    const float avance = 1.0f - reste * reste;
    x = arc.src_x + (arc.dst_x - arc.src_x) * avance;
    z = arc.src_z + (arc.dst_z - arc.src_z) * avance;
    // Le SOL suit le même chemin que l'objet : entre les pieds de l'entité et la
    // case d'arrivée, le terrain peut monter ou descendre, et une cloche posée
    // sur la seule altitude d'arrivée ferait rentrer l'objet dans une pente.
    const float sol_courant = arc.src_y + (arc.sol - arc.src_y) * avance;
    y = sol_courant - hauteur * Cloche(p);
  } else if (ecoule < vol + rebond1) {
    const float p = static_cast<float>(ecoule - vol) / static_cast<float>(rebond1);
    y = arc.sol - hauteur * kRebond1Hauteur * Cloche(p);
  } else if (ecoule < vol + rebond1 + rebond2) {
    const float p =
        static_cast<float>(ecoule - vol - rebond1) / static_cast<float>(rebond2);
    y = arc.sol - hauteur * kRebond2Hauteur * Cloche(p);
  } else {
    // Posé. On rend la main : l'objet garde la position que le serveur lui a
    // donnée, au sol, exactement comme si le natif l'y avait fait tomber.
    g_arcs.erase(self);
  }

  PoserPosition(champs, x, y, z);
  return resultat;
}

}  // namespace

ItemDropArc::ItemDropArc() {
  const uintptr_t slot = kCItemVtable + kSlotOnTick * 4;
  uintptr_t trouve = 0;
  const uintptr_t ancien = mem::SwapVtableSlot(
      slot, kCItemOnTick, reinterpret_cast<uintptr_t>(&OnTickDetour), &trouve);
  // Rien posé : le natif garde ses chutes verticales, et le butin tombe comme
  // avant. Dire LEQUEL des deux refus, sinon le diagnostic tourne en rond.
  if (!ancien) {
    if (trouve != kCItemOnTick)
      LogDiag("[ItemDropArc] slot 0x{:08X} : contient 0x{:08X} au lieu de 0x{:08X}, "
              "NON crochete",
              slot, trouve, kCItemOnTick);
    else
      LogDiag("[ItemDropArc] slot 0x{:08X} : page non ouvrable, NON crochete", slot);
    return;
  }
  g_ontick_natif = reinterpret_cast<OnTickFn>(ancien);
  g_module = this;
}

ItemDropArc::~ItemDropArc() {
  // Le détour reste en place (le module vit autant que le processus), mais il
  // doit repartir en pass-through plutôt que lire un objet détruit.
  g_module = nullptr;
  g_arcs.clear();
}

void ItemDropArc::OnModeSwitch(ModeMgr::ModeType, const char*) { g_arcs.clear(); }
