#include "ui/mob_sprite.h"

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB / ReleaseTexture / DeviceEpoch
#include "ui/spr_act.h"
#include "utils/log_console.h"  // LogDiag (dump de calques)

namespace ro {
namespace {

// ── Le seul appel natif restant : id de classe -> nom de dossier ─────────────
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
// Gabarits de chemin du client, en CP949 : "몬스터\%s.spr" / "몬스터\%s.act".
// 🔴 On les lit DANS le binaire plutôt que de les écrire ici : nos sources sont
// en UTF-8, un littéral coréen y serait encodé en UTF-8 et ne désignerait aucun
// dossier du GRF.
constexpr uintptr_t kFmtSpr = 0x0103181c;
constexpr uintptr_t kFmtAct = 0x0103182c;

using JobResNameFn = const char* (__fastcall*)(void*, void*, unsigned, int);

// Intervalle d'une image, en millisecondes, pour une cadence déclarée en ticks.
// L'unité est le « tick » RO de 25 ms — même conversion que GRF Editor
// (`Action.AnimationSpeed * 25`).
constexpr float kActDelayTickMs = 25.0f;

// ── Cache des ressources ─────────────────────────────────────────────────────
//
// Une entrée par id de classe. Elle porte les fichiers parsés, les textures
// téléversées PARESSEUSEMENT (une fiche n'affiche qu'une action : inutile de
// monter en VRAM les 8 directions × 5 actions du .spr) et la boîte englobante
// mémoïsée.
struct MobRes {
  spract::Resource res;
  std::vector<void*> tex_indexed;
  std::vector<void*> tex_bgra;
  unsigned epoch = 0;

  // Boîte englobante de TOUTE l'action, en unités .act. Calculée une fois par
  // action — cf. le pavé au-dessus de son calcul.
  int   box_action = -1;
  float box_min_x = 0, box_min_y = 0, box_max_x = 0, box_max_y = 0;
  bool  box_valid = false;
};

std::unordered_map<int, std::unique_ptr<MobRes>> g_cache;
std::deque<int> g_cache_order;  // ordre d'arrivée, pour l'éviction
// Un monstre coûte quelques centaines de kilo-octets de VRAM (2 à 6 images
// téléversées). Seize entrées bornent la facture tout en couvrant largement le
// va-et-vient d'un joueur entre plusieurs fiches.
constexpr size_t kMaxCached = 16;

void ReleaseTextures(MobRes* m) {
  for (void* t : m->tex_indexed) Overlay_ReleaseTexture(t);
  for (void* t : m->tex_bgra) Overlay_ReleaseTexture(t);
  m->tex_indexed.clear();
  m->tex_bgra.clear();
}

// Texture d'une image, créée au premier besoin.
void* TextureFor(MobRes* m, int index, int type) {
  const unsigned epoch = Overlay_DeviceEpoch();
  if (epoch != m->epoch) {
    // ⚠ On LÂCHE sans Release : ces handles appartiennent à un device qui
    // n'existe plus (cf. [[feedback_texture_cache_device_epoch]]).
    m->tex_indexed.clear();
    m->tex_bgra.clear();
    m->epoch = epoch;
  }
  const std::vector<spract::Image>& imgs =
      (type == 0) ? m->res.indexed : m->res.bgra;
  if (index < 0 || static_cast<size_t>(index) >= imgs.size()) return nullptr;
  std::vector<void*>& tex = (type == 0) ? m->tex_indexed : m->tex_bgra;
  if (tex.size() != imgs.size()) tex.assign(imgs.size(), nullptr);
  if (!tex[index]) {
    const spract::Image& img = imgs[index];
    if (img.w > 0 && img.h > 0 && !img.argb.empty())
      tex[index] = Overlay_CreateTextureARGB(img.argb.data(), img.w, img.h);
  }
  return tex[index];
}

// Nom de dossier du monstre, en CP949. Chaîne vide si l'id est hors table —
// et on n'invente AUCUN repli : un monstre sans entrée jobName n'a pas de
// sprite, la fenêtre affiche son placeholder. Retomber sur « poring » serait
// précisément le défaut qu'on corrige ici.
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

MobRes* AcquireResource(int class_id) {
  auto it = g_cache.find(class_id);
  if (it != g_cache.end()) return it->second.get();

  char name[128] = {0};
  if (!ResNameFor(class_id, name, sizeof(name))) return nullptr;

  // ── Le chemin complet, TEL QU'IL EST dans le GRF ──────────────────────────
  //
  // 🔴 `data\sprite\`, PAS `data\`. Le gabarit du client ("몬스터\%s.spr") est un
  // chemin relatif à la racine des SPRITES, et deux couches natives le
  // complètent successivement :
  //   1. `UITextureMgr_Load` (0x00a8d4a0) appelle `Path_ConcatPrefixIfMissing`
  //      avec un préfixe choisi selon l'EXTENSION — « sprite\ » pour .spr/.act,
  //      « texture\ » pour les .bmp/.tga de l'interface ;
  //   2. `Res_MakeDataRootRelativePath` (0x00573340) ajoute ensuite « data\ ».
  //
  // On court-circuite les deux, donc on doit poser les deux. Ne mettre que
  // « data\ » donnait « fichier introuvable » sur TOUS les monstres : la clé
  // GRF est bien `data\sprite\몬스터\<nom>.spr`.
  char spr_path[320] = "data\\sprite\\";
  char act_path[320] = "data\\sprite\\";
  // ⚠ `std::snprintf` et non `_snprintf_s` : le gabarit n'est pas un littéral
  // (il est lu dans le binaire du client), et la famille sécurisée déclenche
  // alors C4774.
  const size_t head = 12;  // strlen("data\\sprite\\")
  std::snprintf(spr_path + head, sizeof(spr_path) - head,
                reinterpret_cast<const char*>(kFmtSpr), name);
  std::snprintf(act_path + head, sizeof(act_path) - head,
                reinterpret_cast<const char*>(kFmtAct), name);

  auto entry = std::make_unique<MobRes>();
  if (!spract::Load(spr_path, act_path, &entry->res)) return nullptr;
  entry->epoch = Overlay_DeviceEpoch();

  if (g_cache_order.size() >= kMaxCached) {
    const int victim = g_cache_order.front();
    g_cache_order.pop_front();
    auto vit = g_cache.find(victim);
    if (vit != g_cache.end()) {
      // Le device est vivant ici (on vient d'en créer une entrée) : les textures
      // de la victime se libèrent pour de bon, sinon parcourir le bestiaire
      // laisserait une texture en VRAM par image jamais revue.
      ReleaseTextures(vit->second.get());
      g_cache.erase(vit);
    }
  }
  MobRes* raw = entry.get();
  g_cache[class_id] = std::move(entry);
  g_cache_order.push_back(class_id);
  return raw;
}

// ── Un calque résolu, prêt à dessiner ────────────────────────────────────────
//
// On garde les QUATRE COINS, pas un rectangle : la rotation du calque fait
// partie de la géométrie et un rectangle aligné aux axes la perdrait — à la fois
// pour le dessin et pour la boîte englobante qui sert au cadrage.
struct ResolvedLayer {
  void*  tex = nullptr;
  ImVec2 uv0{0.0f, 0.0f}, uv1{1.0f, 1.0f};
  ImVec2 corner[4];  // TL, TR, BR, BL en unités .act, rotation appliquée
  ImU32  tint = IM_COL32_WHITE;
  // Champs bruts, uniquement pour le dump de diagnostic.
  int   dbg_x = 0, dbg_y = 0, dbg_index = 0, dbg_type = 0, dbg_mirror = 0,
        dbg_rot = 0, dbg_w = 0, dbg_h = 0;
  float dbg_sx = 1.0f, dbg_sy = 1.0f;
};

constexpr int kMaxDrawLayers = 64;

const spract::Frame* FrameAt(const MobRes* m, unsigned action, unsigned frame) {
  if (action >= m->res.actions.size()) return nullptr;
  const spract::Action& a = m->res.actions[action];
  if (frame >= a.frames.size()) return nullptr;
  return &a.frames[frame];
}

// Résout tous les calques de (action, frame) en textures + géométrie.
// Rend le nombre de calques écrits dans `out`.
//
// `upload` : à false, on calcule la seule géométrie sans créer de texture —
// c'est ce dont le balayage de la boîte englobante a besoin, et ça évite de
// monter en VRAM des images qui ne seront jamais dessinées.
int ResolveFrameLayers(MobRes* m, unsigned action, unsigned frame_index,
                       ResolvedLayer* out, int max_out, bool upload) {
  const spract::Frame* frame = FrameAt(m, action, frame_index);
  if (!frame) return 0;

  int count = 0;
  for (const spract::Layer& L : frame->layers) {
    if (count >= max_out) break;
    const spract::Image* img = m->res.Get(L.index, L.type);
    if (!img || img->w <= 0 || img->h <= 0) continue;

    ResolvedLayer& r = out[count];
    r.tex = upload ? TextureFor(m, L.index, L.type) : nullptr;
    if (upload && !r.tex) continue;

    const float sx = L.scale_x;
    const float sy = L.scale_y;
    const float cw = static_cast<float>(img->w);
    const float ch = static_cast<float>(img->h);

    // ── Géométrie d'un calque : `Plane.FromLayer` de GRF Editor, à la lettre ──
    //
    // Dans CET ordre, et l'ordre compte :
    //   1. image CENTRÉE sur l'origine, translation de (-(w+1)/2, -(h+1)/2)
    //      — division entière, comme la référence ;
    //   2. échelle du calque (autour de l'origine, donc le centrage est lui
    //      aussi mis à l'échelle) ;
    //   3. rotation de -rotation, centre décalé d'un demi-pixel sur les
    //      dimensions IMPAIRES (sinon un calque de largeur impaire dérive d'un
    //      demi-pixel à chaque image) ;
    //   4. translation par (OffsetX, OffsetY) — 🔴 offset NON mis à l'échelle.
    //
    // 🔴 Le point 4 est celui qui a coûté le plus cher. J'avais multiplié
    // l'offset par l'échelle du calque, sur la foi de chiffres relevés à
    // l'époque où l'on lisait les calques DANS la mémoire du client, avec une
    // disposition devinée — c'est-à-dire précisément la couche qu'on a jetée en
    // écrivant ce parseur. Ces chiffres ne valaient rien.
    //
    // Deux preuves indépendantes le confirment :
    //   · le binaire : `Actor_DrawSprites` (0x007AC820) pose la position d'un
    //     calque à `acteur.x + layer.OffsetX` / `acteur.y + layer.OffsetY`. La
    //     seule échelle qu'il applique à un offset est celle du JOB
    //     (`Actor_GetJobSpriteScale`), jamais celle du calque ;
    //   · le symptôme : Sarah flotte de haut en bas, et ses flammes (échelle
    //     0,3) restaient immobiles. Un offset mis à l'échelle ne leur faisait
    //     suivre que 30 % du mouvement du corps — elles se décrochaient.
    //     Offsets et corps vivent dans le MÊME repère, il ne faut rien
    //     multiplier.
    const int   iw = img->w, ih = img->h;
    const float cx0 = static_cast<float>(-((iw + 1) / 2));
    const float cy0 = static_cast<float>(-((ih + 1) / 2));
    float px[4] = {cx0, cx0 + cw, cx0 + cw, cx0};        // TL, TR, BR, BL
    float py[4] = {cy0, cy0,      cy0 + ch, cy0 + ch};
    for (int k = 0; k < 4; ++k) { px[k] *= sx; py[k] *= sy; }
    if (L.rotation != 0) {
      // `RotateZ(-rotation)` de la référence :
      //   x' = x*cos + y*sin ;  y' = -x*sin + y*cos
      const float rad = static_cast<float>(-L.rotation) * 3.14159265358979f / 180.0f;
      const float cs = std::cos(rad), sn = std::sin(rad);
      const float rcx = (iw % 2) ? -0.5f * sx : 0.0f;
      const float rcy = (ih % 2) ? -0.5f * sy : 0.0f;
      for (int k = 0; k < 4; ++k) {
        const float dx = px[k] - rcx, dy = py[k] - rcy;
        px[k] = rcx + dx * cs + dy * sn;
        py[k] = rcy - dx * sn + dy * cs;
      }
    }
    for (int k = 0; k < 4; ++k)
      r.corner[k] = ImVec2(L.off_x + px[k], L.off_y + py[k]);

    // Texture dédiée par image : UV triviales. Le miroir est un simple échange
    // des u — un retournement SUR PLACE, dans le même quad, ce qui est bien la
    // sémantique du champ (ScaleX = -1 autour du centre de l'image).
    r.uv0 = ImVec2(0.0f, 0.0f);
    r.uv1 = ImVec2(1.0f, 1.0f);
    if (L.mirror) { r.uv0.x = 1.0f; r.uv1.x = 0.0f; }

    // Un calque à teinte entièrement nulle serait invisible : la plupart des
    // .act laissent ce champ à 0xFFFFFFFF, quelques-uns le laissent à zéro.
    r.tint = (L.tint == 0) ? IM_COL32_WHITE
                           : IM_COL32((L.tint >> 16) & 0xFF, (L.tint >> 8) & 0xFF,
                                      L.tint & 0xFF, (L.tint >> 24) & 0xFF);

    r.dbg_x = static_cast<int>(L.off_x);
    r.dbg_y = static_cast<int>(L.off_y);
    r.dbg_index = L.index;
    r.dbg_type = L.type;
    r.dbg_mirror = L.mirror ? 1 : 0;
    r.dbg_rot = L.rotation;
    r.dbg_sx = sx;
    r.dbg_sy = sy;
    r.dbg_w = img->w;
    r.dbg_h = img->h;
    ++count;
  }
  return count;
}

}  // namespace

bool LoadMobSprite(int class_id, MobSpriteRes* res) {
  if (!res || class_id <= 0) return false;
  if (res->class_id == class_id) return res->res != nullptr;
  res->class_id = class_id;
  res->res = AcquireResource(class_id);
  res->failed = (res->res == nullptr);
  return !res->failed;
}

int MobActionFrameCount(const MobSpriteRes& res, unsigned action) {
  const MobRes* m = static_cast<const MobRes*>(res.res);
  if (!m || action >= m->res.actions.size()) return 0;
  return static_cast<int>(m->res.actions[action].frames.size());
}

bool DrawMobSprite(ImDrawList* draw_list, const MobSpriteRes& res,
                   ImVec2 rect_min, ImVec2 rect_max, float anim_seconds,
                   unsigned action, float ms_per_frame, bool allow_upscale,
                   float alpha) {
  MobRes* m = static_cast<MobRes*>(res.res);
  if (!draw_list || !m || action >= m->res.actions.size()) return false;
  const float box_w = rect_max.x - rect_min.x;
  const float box_h = rect_max.y - rect_min.y;
  if (box_w <= 1.0f || box_h <= 1.0f) return false;

  const spract::Action& act = m->res.actions[action];
  const int frames = static_cast<int>(act.frames.size());
  if (frames <= 0) return false;

  // Image courante. Une cadence <= 0 fige l'animation sur la première image,
  // exactement comme le natif (periodMs = 0).
  unsigned frame_index = 0;
  if (frames > 1 && ms_per_frame > 0.0f) {
    // La cadence DÉCLARÉE par le .act prime sur celle demandée par l'appelant :
    // elle est propre à l'action et varie beaucoup d'un monstre à l'autre. Le
    // paramètre ne sert plus que d'interrupteur (0 = figé) et de repli.
    const float declared = act.speed * kActDelayTickMs;
    if (declared > 1.0f && declared < 10000.0f) ms_per_frame = declared;
    const float cycle = frames * (ms_per_frame / 1000.0f);
    float t = std::fmod(anim_seconds, cycle);
    if (t < 0.0f) t += cycle;
    int idx = static_cast<int>(t / cycle * frames);
    if (idx < 0) idx = 0;
    if (idx >= frames) idx = frames - 1;
    frame_index = static_cast<unsigned>(idx);
  }

  ResolvedLayer layers[kMaxDrawLayers];
  const int n = ResolveFrameLayers(m, action, frame_index, layers,
                                   kMaxDrawLayers, /*upload=*/true);
  if (n <= 0) return false;

  // ── Boîte englobante de TOUTE l'action ─────────────────────────────────────
  // 🔴 Calculée UNE fois, sur toutes les images — comme `GenerateBoundingBox(act,
  // actionIndex)` de GRF Editor, dont `GenerateImages` se sert ensuite pour
  // rendre chaque image du GIF.
  //
  // La recalculer par image faisait varier l'échelle de cadrage à chaque image :
  // le monstre se dilatait et se contractait au rythme de son animation. C'est
  // le défaut « le sprite se déforme selon les mouvements ».
  if (!m->box_valid || m->box_action != static_cast<int>(action)) {
    ResolvedLayer scratch[kMaxDrawLayers];
    bool any = false;
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    for (int f = 0; f < frames; ++f) {
      const int c = ResolveFrameLayers(m, action, static_cast<unsigned>(f),
                                       scratch, kMaxDrawLayers, /*upload=*/false);
      for (int i = 0; i < c; ++i) {
        for (int k = 0; k < 4; ++k) {
          const ImVec2& p = scratch[i].corner[k];
          if (!any) { bx0 = bx1 = p.x; by0 = by1 = p.y; any = true; continue; }
          if (p.x < bx0) bx0 = p.x;
          if (p.x > bx1) bx1 = p.x;
          if (p.y < by0) by0 = p.y;
          if (p.y > by1) by1 = p.y;
        }
      }
    }
    if (any) {
      m->box_min_x = bx0; m->box_max_x = bx1;
      m->box_min_y = by0; m->box_max_y = by1;
      m->box_action = static_cast<int>(action);
      m->box_valid = true;
    }
  }

  // ── Trace, une ligne par monstre ───────────────────────────────────────────
  // Le dump calque par calque a servi à trancher l'ancrage et la mise à
  // l'échelle de l'offset ; ces questions sont closes. Il ne reste qu'un
  // résumé : de quoi voir d'un coup d'œil, sur un monstre qui s'afficherait
  // mal, si le fichier a été lu (versions, nombre d'images) — les champs
  // `dbg_*` de ResolvedLayer restent là pour rouvrir le dump détaillé au besoin.
  {
    static int s_dumped_class = -1;
    if (res.class_id != s_dumped_class) {
      s_dumped_class = res.class_id;
      LogDiag("[MobSprite] classe {} : spr v{} ({} img + {} bgra), act v{}, "
              "action {} = {} images à {:.0f} ms, {} calques",
              res.class_id, m->res.spr_version,
              static_cast<int>(m->res.indexed.size()),
              static_cast<int>(m->res.bgra.size()), m->res.act_version, action,
              frames, act.speed * kActDelayTickMs, n);
    }
  }

  // La boîte de l'ACTION (ci-dessus), jamais celle de l'image courante : c'est
  // elle qui fixe l'échelle ET le centrage, donc les deux restent stables d'une
  // image à l'autre. Repli sur l'image courante si le balayage a échoué.
  float min_x, max_x, min_y, max_y;
  if (m->box_valid && m->box_action == static_cast<int>(action)) {
    min_x = m->box_min_x; max_x = m->box_max_x;
    min_y = m->box_min_y; max_y = m->box_max_y;
  } else {
    min_x = max_x = layers[0].corner[0].x;
    min_y = max_y = layers[0].corner[0].y;
    for (int i = 0; i < n; ++i) {
      for (int k = 0; k < 4; ++k) {
        const ImVec2& p = layers[i].corner[k];
        if (p.x < min_x) min_x = p.x;
        if (p.x > max_x) max_x = p.x;
        if (p.y < min_y) min_y = p.y;
        if (p.y > max_y) max_y = p.y;
      }
    }
  }
  const float span_x = max_x - min_x;
  const float span_y = max_y - min_y;
  if (span_x <= 0.0f || span_y <= 0.0f) return false;

  // Échelle « pour tenir dans la boîte », puis bornée à 1 : on RÉDUIT ce qui
  // dépasse, on n'agrandit jamais un petit monstre. Les tailles du .act sont
  // celles du jeu — un Poring doit rester petit à côté d'un Baphomet.
  float scale = box_w / span_x;
  const float fit_y = box_h / span_y;
  if (fit_y < scale) scale = fit_y;
  if (!allow_upscale && scale > 1.0f) scale = 1.0f;
  if (scale <= 0.0f) return false;

  const float center_x = (min_x + max_x) * 0.5f;
  const float center_y = (min_y + max_y) * 0.5f;
  const float origin_x = (rect_min.x + rect_max.x) * 0.5f - center_x * scale;
  const float origin_y = (rect_min.y + rect_max.y) * 0.5f - center_y * scale;

  int a = static_cast<int>(alpha * 255.0f + 0.5f);
  if (a < 0) a = 0;
  if (a > 255) a = 255;

  bool drawn = false;
  for (int i = 0; i < n; ++i) {
    const ResolvedLayer& l = layers[i];
    if (!l.tex) continue;
    // Alpha global appliqué PAR-DESSUS la teinte du calque (multiplicatif sur
    // le canal A, comme le fait le moteur).
    const ImU32 tint =
        (l.tint & 0x00FFFFFFu) |
        (static_cast<ImU32>(((l.tint >> IM_COL32_A_SHIFT) & 0xFF) * a / 255)
         << IM_COL32_A_SHIFT);
    ImVec2 p[4];
    for (int k = 0; k < 4; ++k)
      p[k] = ImVec2(origin_x + l.corner[k].x * scale,
                    origin_y + l.corner[k].y * scale);
    // AddImageQuad (et non AddImage) : le quad peut être TOURNÉ, un rectangle
    // aligné aux axes ne saurait pas le représenter.
    draw_list->AddImageQuad(reinterpret_cast<ImTextureID>(l.tex), p[0], p[1],
                            p[2], p[3],
                            ImVec2(l.uv0.x, l.uv0.y), ImVec2(l.uv1.x, l.uv0.y),
                            ImVec2(l.uv1.x, l.uv1.y), ImVec2(l.uv0.x, l.uv1.y),
                            tint);
    drawn = true;
  }
  return drawn;
}

}  // namespace ro
