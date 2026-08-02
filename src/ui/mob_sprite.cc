#include "ui/mob_sprite.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "utils/log_console.h"

namespace ro {
namespace {

// ── Le seul appel natif : id de classe -> nom de fichier ─────────────────────
//
// `Job_GetDisplayNameOrResName` (0x00D5BB40) indexe le vecteur construit depuis
// `data\luafiles514\lua files\datainfo\jobName.lub` (exécuté au boot par
// Lua_LoadAllScriptFiles 0x00D646C0, juste après jobName_F.lub). C'est LA table
// qui donne le nom de fichier réel — et il ne coïncide pas avec l'AegisName du
// serveur : `[jobtbl.JT_CHONCHON] = "Chocho"`, le fichier étant `Chocho.spr`.
//
// Signature __thiscall(this, classId, sex). `this` est l'OBJET à
// g_UIWindowContextKey — son adresse, pas un pointeur à déréférencer. sex = -1
// (le client le résout lui-même ; sans objet pour un monstre, dont la branche
// sort avant). Rend une chaîne CP949, ou « » si classId est hors table.
constexpr uintptr_t kJobResName = 0x00d5bb40;
constexpr uintptr_t kJobNameCtx = 0x015fa3c0;
// Gabarit de chemin du client, en CP949 : "몬스터\%s.spr".
// 🔴 On le lit DANS le binaire plutôt que de l'écrire ici : nos sources sont en
// UTF-8, un littéral coréen y serait encodé en UTF-8 et ne désignerait aucun
// dossier du GRF. On lui retire son « .spr » : sprite_view veut une base.
constexpr uintptr_t kFmtSpr = 0x0103181c;

using JobResNameFn = const char* (__fastcall*)(void*, void*, unsigned, int);

// Nom de dossier du monstre, en CP949. Chaîne vide si l'id est hors table — et
// on n'invente AUCUN repli : un monstre sans entrée jobName n'a pas de sprite,
// l'appelant affiche son placeholder. Retomber sur « poring » serait justement
// le défaut qu'on corrige ici.
bool ResNameFor(int class_id, char* out, size_t out_size) {
  const char* name = nullptr;
  __try {
    name = reinterpret_cast<JobResNameFn>(kJobResName)(
        reinterpret_cast<void*>(kJobNameCtx), nullptr,
        static_cast<unsigned>(class_id), -1);
  } __except (EXCEPTION_EXECUTE_HANDLER) { name = nullptr; }
  if (!name) return false;
  bool ok = false;
  __try {
    if (*name) {
      lstrcpynA(out, name, static_cast<int>(out_size));
      ok = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  return ok;
}

// Chemin VFS complet SANS extension, ex. `data\sprite\몬스터\Chocho`.
//
// 🔴 `data\sprite\`, PAS `data\`. Le gabarit du client est relatif à la racine
// des SPRITES, et deux couches natives le complètent successivement :
// `UITextureMgr_Load` (0x00a8d4a0) ajoute un préfixe choisi selon l'EXTENSION
// (« sprite\ » pour .spr/.act), puis `Res_MakeDataRootRelativePath`
// (0x00573340) ajoute « data\ ». On court-circuite les deux, donc on pose les
// deux — ne mettre que « data\ » rendait « fichier introuvable » sur TOUS les
// monstres.
bool BasePathFor(int class_id, char* out, size_t out_size) {
  char name[128] = {0};
  if (!ResNameFor(class_id, name, sizeof(name))) return false;

  char tail[256];
  // ⚠ `std::snprintf` et non `_snprintf_s` : le gabarit n'est pas un littéral
  // (il est lu dans le binaire du client), et la famille sécurisée déclenche
  // alors C4774.
  std::snprintf(tail, sizeof(tail), reinterpret_cast<const char*>(kFmtSpr), name);
  // Retire le « .spr » final : sprite_view rajoute les deux extensions.
  const size_t n = std::strlen(tail);
  if (n > 4) tail[n - 4] = '\0';

  std::snprintf(out, out_size, "data\\sprite\\%s", tail);
  return true;
}

}  // namespace

bool LoadMobSprite(int class_id, MobSpriteRes* res) {
  if (!res || class_id <= 0) return false;
  if (res->class_id == class_id) return res->sprite.res != nullptr;
  res->class_id = class_id;
  res->sprite = SpriteRes{};

  char base[352];
  if (!BasePathFor(class_id, base, sizeof(base))) {
    res->failed = true;
    return false;
  }
  const bool ok = LoadSprite(base, &res->sprite);
  res->failed = !ok;
  if (ok) {
    // Une ligne par monstre : de quoi voir, sur un monstre qui s'afficherait
    // mal, si le fichier a bien été lu.
    LogDiag("[MobSprite] classe {} -> {}", class_id, base);
  }
  return ok;
}

int MobActionFrameCount(const MobSpriteRes& res, unsigned action) {
  return SpriteActionFrameCount(res.sprite, action);
}

bool DrawMobSprite(ImDrawList* draw_list, const MobSpriteRes& res,
                   ImVec2 rect_min, ImVec2 rect_max, float anim_seconds,
                   unsigned action, float ms_per_frame, bool allow_upscale,
                   float alpha) {
  return DrawSprite(draw_list, res.sprite, rect_min, rect_max, anim_seconds,
                    action, ms_per_frame, allow_upscale, alpha);
}

}  // namespace ro
