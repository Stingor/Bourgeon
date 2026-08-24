#include "ragnarok/own_actor.h"

#include <Windows.h>  // SEH autour des déréférencements d'objets natifs

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ragnarok/game_scene.h"
#include "ragnarok/globals.h"

namespace rag {
namespace {

// ── Chaîne jusqu'à l'acteur du joueur ────────────────────────────────────────
// Validée au débogueur : `GameMode_GetActive` -> CMode, gestionnaire d'acteurs
// à +0xcc, acteur du joueur à +0x2c (vtable 0x01094810, gid @+0x110 = AID).
// Les deux offsets sont `gamescene::kGmActorMgr` et `gamescene::kAmOwnPlayer` —
// ce fichier en portait sa propre copie du second, ce qui faisait de lui le
// SEPTIÈME déclarant d'une constante qu'il était censé rassembler.

// Les deux vecteurs d'emplacements. `CActorSprite_SetSlotAct` (0x00d3fd90)
// écrit dans le premier, `CActorSprite_SetSlotSprite` (0x00d3d7c0) dans le
// second — malgré leurs noms, qui sont INVERSÉS dans l'IDB. Ce qui compte est
// vérifié en jeu : +0x4ac porte les CELLULES (le .spr), +0x4b8 les IMAGES (le
// .act).
constexpr int kOffCellArr  = 0x4ac;  // begin ; end à +4
constexpr int kOffFrameArr = 0x4b8;

// Emplacements, confirmés en jeu en changeant d'équipement : SEUL le 7 bouge
// quand on change de bouclier, le 6 restant vide la plupart du temps.
constexpr int kSlotTrail = 6, kSlotWeapon = 5, kSlotShield = 7;

// ── Le chemin d'une ressource chargée ────────────────────────────────────────
// `UITextureMgr_Load` (0x00a8d4a0) garde le chemin RÉSOLU dans la ressource,
// sous la forme `{ uint32 empreinte; char chemin[]; }` posée à +0x10 — d'où les
// caractères à +0x14. Le chemin est complet (`data\...`, préfixe de type déjà
// appliqué) et passé au `_strlwr` du client, qui ne touche qu'à l'ASCII : les
// octets CP949 des dossiers coréens y survivent.
constexpr int kOffResPath = 0x14;

// ── Sous-acteurs : le chariot ────────────────────────────────────────────────
// Le chariot n'est pas un emplacement du composite mais un ACTEUR ENFANT.
// Deux sources, parce qu'aucune n'est fiable seule : le pointeur persistant
// +0x380, stable toute la frame, et la liste de rendu +0x3a8, souvent VIDE au
// moment où l'interface dessine.
constexpr int kOffChildPrimary = 0x380;
constexpr int kOffChildHead    = 0x3a8;
constexpr int kOffChildCount   = 0x3ac;
constexpr int kOffChildActive  = 0xa0;
constexpr int kOffChildSpr     = 0x104;
constexpr int kOffChildAct     = 0x108;
constexpr int kOffChildVisible = 0x158;
constexpr int kOffChildParent  = 0x15c;

// 손수레, en hexadécimal ÉCHAPPÉ : nos sources sont en UTF-8 et un littéral
// coréen n'y désignerait aucun dossier du GRF. C'est le nom de dossier du
// chariot, cherché dans le chemin du sprite de l'enfant — son « genre » (+0x1a8)
// est générique et ne distingue rien.
constexpr char kCartMark[] = "\xBC\xD5\xBC\xF6\xB7\xB9";

// Copie brute du chemin stocké dans la ressource. Séparée de la mise en forme
// pour que le `__try` n'enferme que le déréférencement.
bool ResRawPath(void* res, char* raw, size_t raw_size) {
  if (!res || !raw || raw_size == 0) return false;
  raw[0] = '\0';
  __try {
    const char* p = reinterpret_cast<const char*>(res) + kOffResPath;
    size_t i = 0;
    for (; i + 1 < raw_size && p[i]; ++i) raw[i] = p[i];
    raw[i] = '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    raw[0] = '\0';
  }
  return raw[0] != '\0';
}

// Chemin de la ressource, prêt pour `ro::LoadSpritePair` : préfixé `data\` et
// privé de son extension. Rend false si la ressource est absente ou muette.
bool ResBasePath(void* res, char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  char raw[300];
  if (!ResRawPath(res, raw, sizeof(raw))) return false;

  // `ro::LoadSpritePair` veut une BASE : c'est lui qui recolle `.spr` et
  // `.act`. On ne coupe que sur une extension de 3 lettres du dernier segment,
  // pour ne pas amputer un dossier qui contiendrait un point.
  const size_t n = std::strlen(raw);
  if (n > 4 && raw[n - 4] == '.') raw[n - 4] = '\0';
  if (!raw[0]) return false;

  // 🔴 Le chemin stocké commence à `sprite\`, PAS à `data\`.
  //
  // `UITextureMgr_Load` (0x00a8d4a0) ne recolle que le préfixe de TYPE, celui
  // qui dépend de l'extension ; le `data\` vient d'une couche plus basse, que
  // nous court-circuitons. C'est exactement le piège déjà payé sur le corps et
  // les coiffes : le gabarit du client est relatif à la racine des sprites, et
  // les DEUX préfixes sont à poser soi-même.
  //
  // Symptôme sans ça : « fichier introuvable : sprite\... » et une arme
  // silencieusement absente, un calque manquant n'étant pas une erreur pour le
  // composeur.
  const bool has_data = std::strlen(raw) > 5 &&
                        (raw[0] == 'd' || raw[0] == 'D') &&
                        (raw[1] == 'a' || raw[1] == 'A') &&
                        (raw[2] == 't' || raw[2] == 'T') &&
                        (raw[3] == 'a' || raw[3] == 'A') && raw[4] == '\\';
  if (has_data)
    lstrcpynA(out, raw, static_cast<int>(out_size));
  else
    std::snprintf(out, out_size, "data\\%s", raw);
  return out[0] != '\0';
}

// Le chemin du sprite de cet enfant contient-il `needle` (octets CP949) ?
bool ChildSprPathHas(void* spr, const char* needle, int nlen) {
  __try {
    const char* p = reinterpret_cast<const char*>(spr) + kOffResPath;
    for (int i = 0; i < 256 && p[i]; ++i) {
      int j = 0;
      for (; j < nlen && p[i + j]; ++j)
        if (p[i + j] != needle[j]) break;
      if (j == nlen) return true;
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Les deux ressources d'un emplacement, ou {nullptr, nullptr} s'il est
// incomplet — un emplacement d'arme périmé n'en garde souvent qu'une.
void SlotPair(void* actor, int slot, void** cells, void** frames) {
  *cells = nullptr;
  *frames = nullptr;
  __try {
    char* a = reinterpret_cast<char*>(actor);
    void** cbeg = *reinterpret_cast<void***>(a + kOffCellArr);
    void** cend = *reinterpret_cast<void***>(a + kOffCellArr + 4);
    void** fbeg = *reinterpret_cast<void***>(a + kOffFrameArr);
    if (!cbeg || !cend || !fbeg) return;
    if (slot < 0 || slot >= static_cast<int>(cend - cbeg)) return;
    void* c = cbeg[slot];
    void* f = fbeg[slot];
    if (c && f) { *cells = c; *frames = f; }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ReadSlot(void* actor, int slot, char* spr, size_t spr_size, char* act,
              size_t act_size) {
  void* cells = nullptr;
  void* frames = nullptr;
  SlotPair(actor, slot, &cells, &frames);
  if (!ResBasePath(cells, spr, spr_size) ||
      !ResBasePath(frames, act, act_size)) {
    spr[0] = '\0';
    act[0] = '\0';
  }
}

}  // namespace

bool ActorSlotSpritePath(void* actor, int slot, char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  if (!actor) return false;
  void* cells = nullptr;
  void* frames = nullptr;
  // 🔴 `SlotPair` exige les DEUX ressources. Ce n'est pas une lourdeur ici : un
  // emplacement qui n'a gardé que ses cellules est un emplacement PÉRIMÉ — une
  // arme déséquipée dont la ressource traîne — et lire son chemin ferait
  // ressusciter un sprite que le jeu ne dessine plus.
  SlotPair(actor, slot, &cells, &frames);
  return ResBasePath(cells, out, out_size);
}

bool ReadOwnActorSprites(OwnActorSprites* out) {
  if (!out) return false;
  std::memset(out, 0, sizeof(*out));

  void* actor = OwnActor();
  if (!actor) return false;

  ReadSlot(actor, kSlotWeapon, out->weapon_spr, sizeof(out->weapon_spr),
           out->weapon_act, sizeof(out->weapon_act));
  ReadSlot(actor, kSlotTrail, out->trail_spr, sizeof(out->trail_spr),
           out->trail_act, sizeof(out->trail_act));
  ReadSlot(actor, kSlotShield, out->shield_spr, sizeof(out->shield_spr),
           out->shield_act, sizeof(out->shield_act));

  // ── Le chariot ─────────────────────────────────────────────────────────────
  __try {
    void* kids[8];
    int nk = 0;
    char* a = reinterpret_cast<char*>(actor);
    void* prim = *reinterpret_cast<void**>(a + kOffChildPrimary);
    if (prim) kids[nk++] = prim;
    void* head = *reinterpret_cast<void**>(a + kOffChildHead);
    const int count = *reinterpret_cast<int*>(a + kOffChildCount);
    if (head && count > 0 && count <= 64) {
      void* node = *reinterpret_cast<void**>(head);  // head->next = 1er nœud
      for (int i = 0; i < count && node && node != head && nk < 8; ++i) {
        void* c = *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 8);
        node = *reinterpret_cast<void**>(node);
        bool dup = (c == nullptr);
        for (int k = 0; k < nk && !dup; ++k)
          if (kids[k] == c) dup = true;
        if (!dup) kids[nk++] = c;
      }
    }
    for (int i = 0; i < nk; ++i) {
      char* c = reinterpret_cast<char*>(kids[i]);
      if (*reinterpret_cast<void**>(c + kOffChildParent) != actor) continue;
      if (*reinterpret_cast<uint8_t*>(c + kOffChildActive) == 0) continue;
      if (*reinterpret_cast<uint8_t*>(c + kOffChildVisible) == 0) continue;
      void* spr = *reinterpret_cast<void**>(c + kOffChildSpr);
      void* act = *reinterpret_cast<void**>(c + kOffChildAct);
      if (!spr || !act) continue;
      if (!ChildSprPathHas(spr, kCartMark, 6)) continue;
      if (!ResBasePath(spr, out->cart_spr, sizeof(out->cart_spr)) ||
          !ResBasePath(act, out->cart_act, sizeof(out->cart_act))) {
        out->cart_spr[0] = '\0';
        out->cart_act[0] = '\0';
      }
      break;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}

  return true;
}

void* OwnActorOf(void* game_mode) {
  void* actor = nullptr;
  __try {
    if (game_mode) {
      void* mgr = *reinterpret_cast<void**>(
          reinterpret_cast<char*>(game_mode) + gamescene::kGmActorMgr);
      if (mgr)
        actor = *reinterpret_cast<void**>(
            reinterpret_cast<char*>(mgr) + gamescene::kAmOwnPlayer);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { actor = nullptr; }
  return actor;
}

void* OwnActor() { return OwnActorOf(rag::ActiveModeIfReady()); }

}  // namespace rag
