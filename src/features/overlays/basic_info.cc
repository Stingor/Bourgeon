#include "ragnarok/lua.h"
#include "ragnarok/render.h"
#include "ragnarok/globals.h"
#include "ragnarok/own_actor.h"  // arme / bouclier / chariot déjà résolus par le client
#include "ragnarok/game_scene.h"
#include "ui/doll.h"  // aperçu d'article : pantin COMPOSÉ (remplace la capture)
#include "ui/sprite_view.h"  // chariot : sprite indépendant, posé sur le pantin
#include "ui/sprite_path.h"  // BodySpriteKey (le style se range par corps)
#include "ui/game_texture.h"
#include "features/overlays/basic_info.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"

#include "ragnarok/uiwnd.h"

#include <Windows.h>

#include <algorithm>
#include <cfloat>  // FLT_MAX (mesure de texte à taille imposée)
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "bourgeon.h"
#include "d3d9/d3d9_hook.h"  // D3D9_CompositeQuadsRGBA (export GIF avatar)
#include "features/overlays/cast_bar.h"  // barre « Cast » : contenu et présence
#include "features/systems/bourgeon_opcodes.h"  // kHatEffectMap (ZC 0x0F17)
#include "features/fx/ez_effect_capture.h"  // capture PARTAGÉE des effets EZ (doll + aperçu)
#include "features/fx/palette_cache.h"   // DollKey (clé de teinte du composeur)
#include "features/fx/palette_inject.h"  // InjectedPalette (couleurs composées)
#include "features/fx/style_sync.h"    // LocalRecipe (couleur de cheveux choisie)
#include "features/moonlight_ui/moonlight_ui.h"  // shared AlignGrid (snap + draw)
#include "utils/gif_writer.h"       // GifWrite
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"
#include "utils/i18n.h"
#include "utils/memory_patch.h"  // mem::PatchValue

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// DX9 vs DX7 active backend (defined in ragnarok_client.cc) — picks the
// CTexture native-handle offset, like the RO cursor capture.
extern bool g_imgui_dx7_active;

// ── Live value sources (20250716 client) ─────────────────────────────────────
// EXP confirmed by RE of UIBasicInfoWnd::OnCreate (UIINT64BarGraph_SetCurMax) —
// INT64.  HP/SP confirmed by UIBasicInfoWnd::DrawContent, which reads them as
// (int) — INT32.  `max` may sit below `cur` (job exp); that's fine.
namespace {

struct Src {
  uintptr_t   cur, max;
  long long   max_const;  // if != 0, literal max overriding *max (zeny = INT32 cap)
  bool        wide;       // true = INT64, false = INT32
  bool        grouped;    // thousands-group the amount in the label (zeny)
  // 🔴 PAS de `label` ici. Il y en avait un, copie exacte de
  // BasicInfo::kBarLabels — deux sources de vérité pour les mêmes sept libellés,
  // l'une peinte sur la barre, l'autre dans les réglages. Elles ont dérivé : en
  // traduisant celle-ci, la case à cocher « Poids » restait française en EN/ES.
  // Le libellé se lit maintenant à un seul endroit, kBarLabels, indexé par BarId
  // comme cette table.
  const char* win_id;     // ImGui window id (### keeps it stable)
};
// Zeny (g_PlayerZeny @0x015fba90) and Weight (cur @0x015fbaa0 / max @0x015fba9c)
// confirmed by RE of UIBasicInfoWnd::DrawContent @0x0095e620 — both INT32. Zeny
// has no in-memory max, but the client stores it signed 32-bit, so its bar fills
// relative to the INT32 hard cap (kZenyMax) and shows the amount thousands-grouped.
constexpr long long kZenyMax = 2147483647LL;  // INT32_MAX = client hard zeny cap
const Src kSrc[BasicInfo::kBarCount] = {
  {rag::kOwnBaseExpAddr, rag::kOwnBaseExpNextAddr, 0, true,  false, "###BIBaseExp"},
  {rag::kOwnJobExpAddr,  rag::kOwnJobExpNextAddr,  0, true,  false, "###BIJobExp"},
  {rag::kOwnHpAddr,      rag::kOwnMaxHpAddr,       0, false, false, "###BIHp"},
  {rag::kOwnSpAddr,      rag::kOwnMaxSpAddr,       0, false, false, "###BISp"},
  {rag::kZenyAddr,       rag::kZenyAddr,    kZenyMax, false, true,  "###BIZeny"},
  {rag::kWeightCurAddr,  rag::kWeightMaxAddr,      0, false, false, "###BIWeight"},
  // Incantation : AUCUNE globale à lire. Ses valeurs sont l'écoulé et la durée
  // totale du cast en cours, relevés sur l'acteur par CastBar et poussés à
  // DrawBar depuis OnRenderUI. Les deux adresses nulles sont là pour garder la
  // table alignée sur BarId — elles ne sont jamais déréférencées.
  {0,          0,          0,        false, false, "###BICast"},
};

// Formats a signed integer with thousands separators (1234567 -> "1,234,567")
// into `out` — used for the grouped zeny bar. Bounded; always NUL-terminated.
void GroupInt(long long v, char* out, size_t n) {
  if (n == 0) return;
  char tmp[24];
  std::snprintf(tmp, sizeof(tmp), "%lld", v < 0 ? -v : v);
  const int len = static_cast<int>(std::strlen(tmp));
  const int cap = static_cast<int>(n) - 1;  // room for the NUL
  int oi = 0;
  if (v < 0 && oi < cap) out[oi++] = '-';
  for (int i = 0; i < len; ++i) {
    if (i > 0 && (len - i) % 3 == 0 && oi < cap) out[oi++] = ',';
    if (oi < cap) out[oi++] = tmp[i];
  }
  out[oi] = '\0';
}

inline long long RDval(uintptr_t addr, bool wide) {
  return wide ? *reinterpret_cast<const volatile int64_t*>(addr)
              : static_cast<long long>(
                    *reinterpret_cast<const volatile int32_t*>(addr));
}

inline float ExpFrac(long long cur, long long max) {
  if (max <= 0) return 0.0f;
  const double f = static_cast<double>(cur) / static_cast<double>(max);
  if (f < 0.0) return 0.0f;
  if (f > 1.0) return 1.0f;
  return static_cast<float>(f);
}

// ── Status-portrait value sources (20250716 client) ──────────────────────────
// Les niveaux de base et de job viennent de `rag::kBaseLevelAddr` /
// `rag::kJobLevelAddr` : les mêmes globales que celles dont le UIBasicInfoWnd
// natif tire ses « Base Lv. » et « Job Lv. ».

inline int RDi(uintptr_t a) { return *reinterpret_cast<volatile int*>(a); }

// Class-name lookup, exactly as UIBasicInfoWnd::DrawContent does it:
//   jobid = FUN_00d5b580(session);  name = FUN_00d5bb40(session, jobid, -1)
// Both are __thiscall (this=session); called via __fastcall with a dummy edx so
// the real args land on the stack (standard thiscall->fastcall shim).

// ── Apparence du joueur : les globales que le client tient à jour ───────────
//
// Ce sont celles-là mêmes que lit l'équipement natif (FUN_008cf970), donc elles
// suivent un changement de coiffure ou de couleur sans qu'on ait à s'abonner à
// quoi que ce soit. Cf. [[project_own_look_globals]].
constexpr uintptr_t kGarmentView = 0x015fb2a0;  // g_OwnLook_GarmentRobeViewId
// Les VUES de coiffe, telles que le SERVEUR les a décidées : le client les tient
// à jour sur ZC_SPRITE_CHANGE et c'est d'elles qu'il habille l'acteur en scène.
// Cf. `BuildOwnDollLook` pour ce qu'elles apportent que l'équipement ne sait pas.
constexpr uintptr_t kHeadLowView = 0x015fb294;  // g_OwnLook_HeadBottomViewId
constexpr uintptr_t kHeadTopView = 0x015fb298;  // g_OwnLook_HeadTopViewId
constexpr uintptr_t kHeadMidView = 0x015fb29c;  // g_OwnLook_HeadMidViewId
// Deux globales de plus, publiées par `ragnarok/globals.h` :
//   * `rag::kOwnJobIdAddr` — 🔴 la CLASSE qui nomme le sprite de corps, second
//     argument de `Job_ResolveBodyClass`. À ne pas confondre avec le job
//     ajusté par la monture.
//   * `rag::kOwnAccountIdAddr` — l'AID du joueur, et le GID de son acteur, donc
//     la clé de sa palette composée.

// CTexture -> handle GPU natif. L'offset dépend du back-end.
constexpr int kCTexOffDX9 = 0x12c, kCTexOffDX7 = 0x128;

// Chaîne CMode -> gestionnaire d'acteurs -> acteur du joueur.

// ── Le chariot, posé à plat ──────────────────────────────────────────────────
//
// 100 % statique, zéro lecture live : la position du chariot dans le monde ne
// converge pas (elle FIGE sous un seuil de distance), et la lire faisait dériver
// le pantin. On le place donc par trigonométrie sur l'orientation affichée.
constexpr float kCartTilePx = 32.0f;   // 1 tuile en px natifs — monter = éloigner
constexpr float kCamPitch   = 0.766f;  // écrasement vertical de l'axe Z (mesuré)
constexpr float kCartNudgeX = 0.0f, kCartNudgeY = 0.0f;
constexpr int   kCartDirOffset = 0;
// En pose de combat le corps pivote d'un cran ; le chariot suit.
constexpr int   kCartCombatShift = 1;

// ── Aperçu d'équipement (mouseover) ──────────────────────────────────────────
//
// L'aperçu compose le personnage portant SEULEMENT l'article survolé, comme la
// fenêtre native : il faut donc savoir dans quel slot de tête ranger son viewID.
// Le mapping vient de `UICostumePreviewWnd` OnMsg 0x17.
//
// Le slot ARME n'y figure pas — le natif non plus ne prévisualise que la tête et
// la cape.
// Masques d'emplacement, tels que le serveur les envoie dans la définition d'un
// article — un bit par slot, et les articles multi-slots en portent plusieurs.
enum Emplacement {
  kEmpHeadLow        = 0x1,
  kEmpGarment        = 0x4,
  kEmpHeadTop        = 0x100,
  kEmpHeadMid        = 0x200,
  kEmpCostumeTop     = 0x400,
  kEmpCostumeMid     = 0x800,
  kEmpCostumeLow     = 0x1000,
  kEmpCostumeGarment = 0x2000,
  // Combinaisons courantes : un casque intégral occupe les trois slots de tête.
  kEmpTopMid            = kEmpHeadTop | kEmpHeadMid,                 // 0x300
  kEmpTopMidLow         = kEmpHeadTop | kEmpHeadMid | kEmpHeadLow,   // 0x301
  kEmpMidLow            = kEmpHeadMid | kEmpHeadLow,                 // 0x201
  kEmpCostumeTopMid     = kEmpCostumeTop | kEmpCostumeMid,           // 0xc00
  kEmpCostumeTopLow     = kEmpCostumeTop | kEmpCostumeLow,           // 0x1400
  kEmpCostumeMidLow     = kEmpCostumeMid | kEmpCostumeLow,           // 0x1800
  kEmpCostumeTopMidLow  = kEmpCostumeTop | kEmpCostumeMid | kEmpCostumeLow,
};

enum PvSlot { PV_NONE, PV_TOP, PV_MID, PV_LOW, PV_GARMENT };

// Range l'article dans UN slot pour l'aperçu : le plus haut qu'il occupe.
//
// ⚠ Une énumération de cas, et non un test de bits : c'est ce que fait
// `UICostumePreviewWnd` (OnMsg 0x17), et un test de bits donnerait un autre
// résultat sur les combinaisons — `kEmpTopMidLow` irait en tête basse alors que
// le natif le prévisualise en tête haute.
PvSlot MapEmplacementToSlot(int emp) {
  switch (emp) {
    case kEmpHeadTop:
    case kEmpCostumeTop:
    case kEmpCostumeTopLow:
    case kEmpTopMid:
    case kEmpCostumeTopMid:       return PV_TOP;
    case kEmpHeadMid:
    case kEmpCostumeMid:
    case kEmpMidLow:
    case kEmpCostumeMidLow:       return PV_MID;
    case kEmpHeadLow:
    case kEmpCostumeLow:
    case kEmpTopMidLow:
    case kEmpCostumeTopMidLow:    return PV_LOW;
    case kEmpGarment:
    case kEmpCostumeGarment:      return PV_GARMENT;
    default:                      return PV_NONE;
  }
}

// ── Câblage slot -> COUCHE ───────────────────────────────────────────────────
//
// `hg_top`/`hg_mid`/`hg_low` ne sont PAS des slots d'équipement : ce sont les
// noms de couche de l'actor ctor, c'est-à-dire l'ORDRE D'EMPILEMENT que le
// composeur respectera (`low` dessous, `top` dessus). Le câblage ci-dessous —
// slot 8 -> hg_mid, slot 9 -> hg_low, slot 0 -> hg_top — est celui du rendu
// natif capturé, et il ne se déduit d'aucune règle : c'est une mesure.
//
// 🔴 Il vit ICI et nulle part ailleurs. Ses deux appelants (l'équipement porté
// et les vues du serveur) alimentent les mêmes slots ; le recopier chez l'un
// des deux, c'est se garantir un empilement qui diverge de l'autre au premier
// ajustement.
void AssignHeadgearLayers(int view_slot0, int view_slot8, int view_slot9,
                          int* hg_top, int* hg_mid, int* hg_low) {
  *hg_mid = view_slot8;
  *hg_low = view_slot9;
  *hg_top = view_slot0;
}

// ── Coiffes PORTÉES ──────────────────────────────────────────────────────────
//
// Les 3 view ids des coiffes RÉELLEMENT équipées, lues dans la table
// d'équipement du client (session+0x17d0). C'est ce que demande la vue
// « costumes masqués » — les costumes n'y entrent donc pas, par définition, et
// la table costume (+0x2b30) n'est plus consultée ici.
//
// De-dup par tag : une coiffe multi-slots occupe plusieurs entrées avec le même
// invIndex (+4), et rendre sa vue dans chacune la dessinerait plusieurs fois sur
// elle-même. Priorité de couche du rendu natif capturé : slot 8, puis 9, puis 0.
//
// Slots equip (vérifié en jeu) : 0 = head-bot/low, 8 = head-top, 9 = head-mid ;
// le câblage slot -> couche est celui d'`AssignHeadgearLayers`.
//
// 🔴 Elle décrit L'ÉQUIPEMENT PORTÉ, jamais l'apparence : le champ de vue qu'elle
// lit (+0x70) vient du catalogue d'items, donc elle IGNORE la transmogrification.
// C'est voulu ici. Pour ce que le monde voit, cf. `BuildOwnDollLook`.
void ResolveHeadgearViews(int* hg_top, int* hg_mid, int* hg_low) {
  const int slots[3] = {0, 8, 9};  // ordre de lecture ; [k] -> couche câblée plus bas
  int tag[3], view[3];
  for (int k = 0; k < 3; ++k) {
    const uintptr_t gen = rag::kSessionAddr + 0x17d0 + static_cast<uintptr_t>(slots[k]) * 0xf8;
    tag[k]  = *reinterpret_cast<int*>(gen + 4);
    view[k] = *reinterpret_cast<int*>(gen + 0x70);
  }
  const int v8 = tag[1] ? view[1] : 0;
  const int v9 = (tag[2] && tag[2] != tag[1]) ? view[2] : 0;
  const int v0 = (tag[0] && tag[0] != tag[1] && tag[0] != tag[2]) ? view[0] : 0;
  AssignHeadgearLayers(v0, v8, v9, hg_top, hg_mid, hg_low);
}


// ── Capture d'effet STR (hat effect .str) ─────────────────────────────────────
// Miroir du moteur de capture SPRITE ci-dessus, mais pour les effets .str : les hat
// effects (costumes SANS viewid, cf. docs/hat_effect_re.md). Un effet .str est un
// billboard 2D animé (glow additif) rendu par un pipeline SÉPARÉ des sprites :
// Effect_DrawStrFrameQuads (vtable+0xc) boucle les couches actives et appelle
// Effect_SubmitStrQuad (0x00bcfb10) qui projette monde->écran + insère en file
// différée. On hooke Effect_SubmitStrQuad : hors capture -> natif (les effets du
// monde rendent normalement) ; en capture (g_str_cap_active) -> on ASSEMBLE les 4
// sommets 2D EXACTEMENT comme le natif (mêmes branches min/max = winding + UV
// corrects), SANS la projection monde/écran ni l'échelle caméra, puis on SUPPRIME
// l'insert natif. La rotation + l'ancrage tête + l'échelle se font au composite.
constexpr uintptr_t kStrSubmitQuad = 0x00bcfb10;  // Effect_SubmitStrQuad (hooké)
constexpr uintptr_t kStrLayerTex   = 0x00715be0;  // Str_GetLayerTexture(layer, idx)

// Une couche capturée d'un effet .str : 4 sommets locaux (pré-rotation, relatifs à
// (cx,cy)) + UV normalisés + couleur + blend. Le composite applique rotation(angle)
// puis (cx,cy), puis son propre ancrage/échelle, et dessine via AddImageQuad (tint).
struct StrCapLayer {
  void*    tex;                       // IDirect3DTexture9* (page de la couche)
  float    cx, cy;                    // centre 2D de la couche (unités canvas .str)
  float    vx[4], vy[4];              // 4 coins locaux (pré-rotation)
  float    vu[4], vv[4];              // 4 UV (déjà normalisés [0..1])
  float    angle;                     // angle brut workBuf[0x15] (deg ; conv. au composite)
  unsigned rgba;                      // IM_COL32(r,g,b,a) (tint de la couche)
  int      sblend, dblend;            // facteurs de blend .str (additif quasi toujours)
};
StrCapLayer g_str_caps[64];
int         g_str_count = 0;
bool        g_str_cap_active = false;  // vrai seulement pendant NOTRE rendu d'effet

using StrLayerTexFn = void* (__fastcall*)(void* layer, void* edx, int idx);
using StrSubmitFn = void (__fastcall*)(void* self, void* edx, void* layer,
                                       float* workBuf, int depthIdx, float* camera);
StrSubmitFn g_orig_str_quad = nullptr;

// Résolution texture des .str en SOUS-DOSSIER = mécanisme NATIF (RE 2026-07-12). Le nœud
// in-world (CActorSprite host, dir posé par _splitpath dans CActorSprite_LoadStrEffect) charge ses
// textures via Str_GetLayerTextureWithDir 0x00715d30 : construit "effect\<dir><basename>" à partir
// d'un std::string (dir) et met en cache au MÊME slot que Str_GetLayerTexture (layer+idx*4+0x1c8).
// On appelle CETTE fonction (au lieu d'un loader maison) -> même chemin, même cache, MÊME type
// d'objet écrit dans +0x1c8 que le rendu natif -> cohérent (pas de confusion de type = plus de
// crash), pas de cache maison (FPS natif). dir = partie dossier de GetHatEfResName, CASSE
// D'ORIGINE (le natif ne minusculise pas ; l'effet rend in-world donc la casse résout bien).
char g_str_cap_subdir[80] = "";                    // dir du .str (ex. "efst_Gold_Shower\") ou ""
constexpr uintptr_t kStrLayerTexDir = 0x00715d30;  // Str_GetLayerTextureWithDir(layer, idx, &dir)
using StrLayerTexDirFn = void* (__fastcall*)(void* layer, void* edx, int idx, const void* dir);

// std::string MSVC (release, 32-bit) reconstruite en POD : [union buf16/ptr][_Mysize][_Myres].
// Le natif lit _Mysize @+0x10, et si _Myres @+0x14 > 15 déréférence *ptr (mode tas), sinon lit les
// caractères en place (SSO). Remplie depuis le dir à chaque capture ; passée par & au natif. POD
// -> utilisable même si le hook est sous SEH (pas d'unwind C++).
struct MsvcStdString { char bx[16]; uint32_t mysize; uint32_t myres; };
MsvcStdString g_str_dir_str = { {0}, 0, 15 };
char          g_str_dir_chars[80] = "";
void SetStrCapDir(const char* dir) {
  size_t n = dir ? std::strlen(dir) : 0;
  if (n >= sizeof(g_str_dir_chars)) n = sizeof(g_str_dir_chars) - 1;
  std::memcpy(g_str_cap_subdir, dir ? dir : "", n); g_str_cap_subdir[n] = '\0';
  std::memcpy(g_str_dir_chars,  dir ? dir : "", n); g_str_dir_chars[n]  = '\0';
  if (n < 16) {                                          // SSO : caractères en place
    std::memcpy(g_str_dir_str.bx, g_str_dir_chars, n + 1);
    g_str_dir_str.mysize = static_cast<uint32_t>(n);
    g_str_dir_str.myres  = 15;                           // <=15 -> natif lit bx en place
  } else {                                               // tas : ptr dans les 4 premiers octets
    *reinterpret_cast<char**>(g_str_dir_str.bx) = g_str_dir_chars;
    g_str_dir_str.mysize = static_cast<uint32_t>(n);
    g_str_dir_str.myres  = static_cast<uint32_t>(n);     // >15 -> natif déréférence *ptr
  }
}

// Hook sur Effect_SubmitStrQuad. Assemble les 4 sommets comme le natif (0x00bcfb10) :
// bloc X/U gardé par workBuf[0xb]<=workBuf[0xa], bloc Y/V par workBuf[0x10]<=workBuf[0xf].
// Coins X = workBuf[0xa..0xd], Y = workBuf[0xe..0x11] ; src rect (px) = workBuf[2..5] ;
// UV normalisés par tex+0x14/tex+0xc (U) & tex+0x18/tex+0x10 (V). SEH-gardé.
void __fastcall Hooked_StrQuad(void* self, void* edx, void* layer, float* workBuf,
                               int depthIdx, float* camera) {
  if (!g_str_cap_active) {
    g_orig_str_quad(self, edx, layer, workBuf, depthIdx, camera);
    return;
  }
  __try {
    if (layer && workBuf && g_str_count < 64) {
      const int off = g_imgui_dx7_active ? kCTexOffDX7 : kCTexOffDX9;
      int texIdx = static_cast<int>(workBuf[0x12]);
      if (texIdx < 0) texIdx = 0;
      // Résolution texture = résolveurs NATIFS (mêmes chemin/cache/type d'objet que le rendu
      // in-world -> cohérent, pas de crash de confusion de type). Sous-dossier ->
      // Str_GetLayerTextureWithDir(layer, idx, &dir) : "effect\<dir><basename>". Racine ->
      // Str_GetLayerTexture(layer, idx) : "effect\<basename>". Les deux mettent en cache au slot
      // natif layer+idx*4+0x1c8 (pas de cache maison = FPS natif).
      void* ctex = g_str_cap_subdir[0]
          ? reinterpret_cast<StrLayerTexDirFn>(kStrLayerTexDir)(layer, nullptr, texIdx, &g_str_dir_str)
          : reinterpret_cast<StrLayerTexFn>(kStrLayerTex)(layer, nullptr, texIdx);
      void* native = ctex ? *reinterpret_cast<void**>(reinterpret_cast<char*>(ctex) + off)
                          : nullptr;
      if (native) {
        StrCapLayer& L = g_str_caps[g_str_count++];
        L.tex = native;
        L.cx = workBuf[0]; L.cy = workBuf[1];
        const float u0 = workBuf[2], v0 = workBuf[3];       // srcU0, srcV0 (px)
        const float u1 = workBuf[4] + workBuf[2];           // srcU0+srcW
        const float v1 = workBuf[5] + workBuf[3];           // srcV0+srcH
        // Bloc X/U (vx/vu des 4 sommets) — réplique EXACTE de Effect_SubmitStrQuad.
        if (workBuf[0xb] <= workBuf[0xa]) {
          L.vx[0] = workBuf[0xc]; L.vu[0] = u1;
          L.vx[1] = workBuf[0xb]; L.vu[1] = u1;
          L.vx[2] = workBuf[0xd]; L.vu[2] = u0;
          L.vx[3] = workBuf[0xa]; L.vu[3] = u0;
        } else {
          L.vx[0] = workBuf[0xd]; L.vu[0] = u0;
          L.vx[1] = workBuf[0xa]; L.vu[1] = u0;
          L.vx[2] = workBuf[0xc]; L.vu[2] = u1;
          L.vx[3] = workBuf[0xb]; L.vu[3] = u1;
        }
        // Bloc Y/V (vy/vv des 4 sommets) — réplique EXACTE.
        if (workBuf[0x10] <= workBuf[0xf]) {
          L.vy[0] = workBuf[0xe];  L.vv[0] = v0;
          L.vy[1] = workBuf[0x11]; L.vv[1] = v1;
          L.vy[2] = workBuf[0xf];  L.vv[2] = v0;
          L.vy[3] = workBuf[0x10]; L.vv[3] = v1;
        } else {
          L.vy[0] = workBuf[0x11]; L.vv[0] = v1;
          L.vy[1] = workBuf[0xe];  L.vv[1] = v0;
          L.vy[2] = workBuf[0x10]; L.vv[2] = v1;
          L.vy[3] = workBuf[0xf];  L.vv[3] = v0;
        }
        // Normalisation UV : *= tex[0x14]/tex[0xc] (U), tex[0x18]/tex[0x10] (V).
        const int tW  = *reinterpret_cast<int*>(reinterpret_cast<char*>(ctex) + 0xc);
        const int tH  = *reinterpret_cast<int*>(reinterpret_cast<char*>(ctex) + 0x10);
        const int t14 = *reinterpret_cast<int*>(reinterpret_cast<char*>(ctex) + 0x14);
        const int t18 = *reinterpret_cast<int*>(reinterpret_cast<char*>(ctex) + 0x18);
        const float us = (tW != 0) ? static_cast<float>(t14) / static_cast<float>(tW) : 0.0f;
        const float vs = (tH != 0) ? static_cast<float>(t18) / static_cast<float>(tH) : 0.0f;
        for (int k = 0; k < 4; ++k) { L.vu[k] *= us; L.vv[k] *= vs; }
        L.angle = workBuf[0x15];
        const unsigned r = static_cast<unsigned>(static_cast<int>(workBuf[0x16])) & 0xffu;
        const unsigned g = static_cast<unsigned>(static_cast<int>(workBuf[0x17])) & 0xffu;
        const unsigned b = static_cast<unsigned>(static_cast<int>(workBuf[0x18])) & 0xffu;
        const unsigned a = static_cast<unsigned>(static_cast<int>(workBuf[0x19])) & 0xffu;
        // Couleur de la couche (tint) = workBuf[0x16..0x19] (r,g,b,a) = IM_COL32(r,g,b,a).
        // Pour gold_shower : blanc × alpha ANIMÉ (le fondu du glow vient de a, pas de la texture).
        L.rgba = (a << 24) | (b << 16) | (g << 8) | r;
        // Blend PAR COUCHE = facteurs D3DBLEND ENTIERS (RE : workBuf[0x1a]/[0x1b] copiés en MOV
        // brut depuis le keyframe +0x70/+0x74, PAS des floats). Les lire en INT brut, sinon le
        // cast float->int donne 0 (les bits entiers 5/7 = floats dénormaux ≈ 0).
        L.sblend = reinterpret_cast<const int*>(workBuf)[0x1a];  // gold_shower : 5 = D3DBLEND_SRCALPHA
        L.dblend = reinterpret_cast<const int*>(workBuf)[0x1b];  // gold_shower : 7 = D3DBLEND_DESTALPHA
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  // insert natif SUPPRIMÉ pendant la capture (rien ne rend dans la scène)
}

void InstallStrCapture() {
  static bool done = false;
  if (done) return;
  done = true;
  using namespace hooking;
  g_orig_str_quad = reinterpret_cast<StrSubmitFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kStrSubmitQuad),
          reinterpret_cast<uint8_t*>(&Hooked_StrQuad)));
}

// Origine du canvas .str : soustraite du centre de couche par Effect_SubmitStrQuad
// (recentre l'effet sur le point d'ancrage = tête de l'acteur). LUES à l'exécution
// (pas de valeur figée). L'échelle caméra + la position écran du nœud sont ignorées :
// on ancre nous-mêmes (ox,oy) à l'échelle `scale` de notre choix.
constexpr uintptr_t kStrCanvasCx = 0x01022f5c;  // DAT_01022f5c (soustrait de workBuf[0])
constexpr uintptr_t kStrCanvasCy = 0x01013e88;  // DAT_01013e88 (soustrait de workBuf[1])

// Composite les couches STR capturées (g_str_caps) par-dessus le dessin courant, EXACTEMENT
// comme le natif (RE Effect_SubmitStrQuad) : (ox,oy) = point d'ancrage écran (canvas 320/240),
// `scale` = échelle canvas.str -> px écran. Par couche : rotation (unité native 1024/tour) autour
// du centre, translation (centre - centreCanvas), ancrage + échelle, blend NATIF par couche,
// AddImageQuad (tint = couleur). No-op si vide. Aucun paramètre de calibrage manuel : tout est
// dérivé du natif (cf. constantes d'ancre/échelle dans RenderPlayerAvatar).
void DrawStrCapLayers(ImDrawList* dl, float ox, float oy, float scale) {
  if (!dl || g_str_count <= 0) return;
  float canvas_cx = 0.0f, canvas_cy = 0.0f;
  __try {
    canvas_cx = *reinterpret_cast<const float*>(kStrCanvasCx);
    canvas_cy = *reinterpret_cast<const float*>(kStrCanvasCy);
  } __except (EXCEPTION_EXECUTE_HANDLER) { canvas_cx = canvas_cy = 0.0f; }

  // Blend PAR COUCHE = facteurs NATIFS capturés (plus de mode alpha forcé qui peignait le fond
  // noir des textures -> bord noir). Callback DX9 explicite par couche (DX7 : repli alpha).
  for (int i = 0; i < g_str_count; ++i) {
    const StrCapLayer& L = g_str_caps[i];
    if (!L.tex) continue;
    // Rotation : unité native = 1024 par tour (RE Effect_SubmitStrQuad), rad = angle * 2π/1024.
    const float rad = L.angle * 0.006135923f;  // 2π/1024 (et NON deg->rad)
    const float ca = std::cos(rad), sa = std::sin(rad);
    ImVec2 p[4];
    for (int k = 0; k < 4; ++k) {
      const float rx = L.vx[k] * ca - L.vy[k] * sa;
      const float ry = L.vx[k] * sa + L.vy[k] * ca;
      p[k].x = (rx + (L.cx - canvas_cx)) * scale + ox;
      p[k].y = (ry + (L.cy - canvas_cy)) * scale + oy;
    }
    // SRCBLEND/DESTBLEND natifs. DESTALPHA(7)->ONE(2), INVDESTALPHA(8)->ZERO(1) car le backbuffer
    // RO n'a pas d'alpha destination (le HW traite DESTALPHA comme 1.0) ; autres facteurs tels
    // quels. gold_shower = SRCALPHA(5)/ONE(2) = additif modulé par alpha -> fond noir invisible.
    int dst = L.dblend;
    if (dst == 7) dst = 2; else if (dst == 8) dst = 1;
    if (L.sblend > 0 && dst > 0) {
      void* cb = reinterpret_cast<void*>(
          static_cast<uintptr_t>((L.sblend & 0xff) | ((dst & 0xff) << 8)));
      dl->AddCallback(reinterpret_cast<ImDrawCallback>(D3D9_ExplicitBlendCallback()), cb);
    } else {
      dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);  // repli alpha (blend inconnu)
    }
    // Winding : strip natif (0,1,2,3) -> périmètre AddImageQuad (0,2,3,1).
    const ImTextureID tex = (ImTextureID)(uintptr_t)L.tex;
    dl->AddImageQuad(tex, p[0], p[2], p[3], p[1],
                     ImVec2(L.vu[0], L.vv[0]), ImVec2(L.vu[2], L.vv[2]),
                     ImVec2(L.vu[3], L.vv[3]), ImVec2(L.vu[1], L.vv[1]), L.rgba);
  }
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);  // restaure l'alpha ImGui
}

// ── Capture d'effet EZ-PARTICULES (hat effects `hatEffectID`) ─────────────────
// 2E FAMILLE de hat effects (docs/hat_effect_re.md, RE 2026-07-13). Les entrées
// HatEffectInfo avec `hatEffectID` (ex. HAT_EF_Digital_Space ordinal 87 -> id 1240)
// NE sont PAS des .str : GetHatEfResName renvoie vide (fiche perso NUE, validé). Elles
// rendent via le système PARTICULES procédural, plus la famille CEffectMgr (auras/statuts
// type Perm_Frost) qui passe par un draw par-effet en phase render.
// ⚠ TOUTE cette capture (hooks, appartenance, projection d'ancre, blend et couleurs par
// sommet) vit désormais dans le module PARTAGÉ `ez_capture` (features/fx/ez_effect_capture.h).
// On ne garde ici que la CONSOMMATION : ciblage de l'acteur, filtre de dessin par id, FIT
// et composite sur le doll. Ne pas ré-implémenter de capture locale : les trois pièges
// (nombre de sommets variable, couleur par sommet, blend par primitive) sont documentés
// dans l'en-tête du module.
// Aperçu cashshop « try before you buy » : spawn temporaire d'un effet EZ/CEffectMgr NON équipé sur le
// JOUEUR (il rend en jeu -> la capture live le récupère). Actor_ToggleEffectId(this=acteur, id, addFlag) :
// addFlag=1 insère l'id dans l'ENSEMBLE actor+0x3fc et RÉ-APPLIQUE TOUT l'ensemble (donc les effets
// ÉQUIPÉS du joueur restent/sont restaurés) ; addFlag=0 retire l'id de l'ensemble et n'enlève QUE cet
// effet. -> les effets équipés sont préservés par l'ensemble (⚠ NE PAS utiliser Effect_ApplyEffectIdToActor
// direct 0xc41ba0 : il casse l'état équipé -> @refresh nécessaire). id unifié = ordinal + 0x98a.

// Ids concrets CIBLES = GetHatEffectID(ordinal) des hat effects équipés (rempli chaque OnTick). Ils ne
// filtrent PLUS la capture (le module capture tout ce qui appartient à l'acteur ciblé) mais le DESSIN :
// passés en `DrawOpts::ids`, ils isolent le(s) hat effect(s) des autres effets du joueur (auras
// skill/statut ids 164/230, footprint). 0 cible = pas de filtre -> tout ce qui a été capturé.
int   g_ez_target_ids[8] = {0};
int   g_ez_target_count  = 0;                        // 0 = pas de filtre id au dessin

void*    g_ez_owner_actor= nullptr;  // acteur dont on capture l'effet (nullptr = capture off)
void*    g_ez_last_actor = nullptr;  // dernier ownActor NON-null connu (repli si mode transitoirement inactif)
int      g_ez_preview_ord    = 0;    // ordinal EZ/CEffectMgr demandé pour l'aperçu cashshop (0 = aucun)
DWORD    g_ez_preview_tick   = 0;    // dernier RequestEzPreview (keep-alive : despawn si non rafraîchi)
int      g_ez_preview_active = 0;    // ordinal réellement spawné sur le joueur (0 = aucun)
// Id CONCRET de l'effet d'aperçu (résolu par GetHatEffectID). Indispensable DEUX fois : pour le
// supprimer du rendu du monde, et pour l'AUTORISER au dessin — g_ez_target_ids ne liste que les
// effets ÉQUIPÉS, donc sans ça un aperçu d'effet non équipé est filtré et n'apparaît nulle part.
int      g_ez_preview_cid    = 0;


// Installe la capture PARTAGÉE (module ez_capture) : hooks chaînés, idempotents. On active en plus la
// famille CEffectMgr (auras/statuts type Perm_Frost), que le doll doit composer comme les effets EZ.
// ⚠ La suppression in-world ne vise QUE l'effet d'APERÇU (cf. ReconcileEzPreview) : les hat effects
// ÉQUIPÉS restent visibles sur le personnage, ils sont légitimement portés.
void InstallEzCapture() {
  static bool done = false;
  if (done) return;
  done = true;
  // La famille CEffectMgr (auras/statuts, ex. Perm_Frost) est capturée d'office par le module ; le
  // doll l'inclut au dessin via DrawOpts::include_effmgr.
  ez_capture::EnsureInstalled();
}

// ── Aperçu cashshop d'un effet EZ/CEffectMgr NON équipé (« try before you buy ») ──────────────
// Appelé CHAQUE frame de survol d'un item-effet EZ (keep-alive). ReconcileEzPreview (OnTick) applique la
// différence : spawn/despawn temporaire de l'effet SUR LE JOUEUR via Actor_ToggleEffectId -> il rend en
// jeu -> la capture live du module ez_capture le récupère -> composité sur l'aperçu. L'effet
// apparaît sur le perso en jeu pendant le survol (voulu). Robuste au survol qui change / s'arrête (keep-alive 300 ms).
void RequestEzPreview(int ordinal) {
  g_ez_preview_ord = ordinal;
  g_ez_preview_tick = GetTickCount();
}
// Défini plus bas : appel brut d'un global Lua getter(ord). Sert ici à résoudre l'id CONCRET de
// l'effet d'aperçu (GetHatEffectID), seul moyen de le supprimer nominativement du rendu du monde.
float HatLuaNum(int ordinal, const char* fn, float def);

void ReconcileEzPreview() {
  int wanted = g_ez_preview_ord;
  if (wanted != 0 && GetTickCount() - g_ez_preview_tick > 300) wanted = 0;  // survol terminé -> despawn
  if (wanted == g_ez_preview_active) return;                                 // rien à faire
  void* actor = rag::OwnActor();
  if (!actor) return;                          // hors-jeu : on retentera (l'état actif reste, resync au retour)
  using ToggleFn = void(__thiscall*)(void*, int, char);
  const ToggleFn toggle = reinterpret_cast<ToggleFn>(rag::kActorToggleEffectIdAddr);
  __try {
    if (g_ez_preview_active != 0)
      toggle(actor, g_ez_preview_active + rag::kHatEffectIdBase, 0);   // despawn l'ancien aperçu (retire de l'ensemble)
    if (wanted != 0)
      toggle(actor, wanted + rag::kHatEffectIdBase, 1);                // spawn le nouvel aperçu (insère + ré-applique tout)
    g_ez_preview_active = wanted;
  } __except (EXCEPTION_EXECUTE_HANDLER) { g_ez_preview_active = wanted; }

  // L'aperçu doit se voir SUR LE DOLL UNIQUEMENT : on demande au module de ne pas laisser passer ses
  // primitives vers le rendu du monde. La suppression est NOMINATIVE et ne vise QUE l'effet d'aperçu,
  // surtout pas les hat effects ÉQUIPÉS — ceux-là sont légitimement portés par le personnage et
  // doivent rester visibles en jeu.
  // ⚠ Un aperçu de la famille CEffectMgr n'a pas d'id résolu (effect_id = -1) : il n'est pas
  // supprimable par id et restera visible sur le personnage.
  g_ez_preview_cid =
      (g_ez_preview_active != 0)
          ? static_cast<int>(HatLuaNum(g_ez_preview_active, "GetHatEffectID", 0.0f))
          : 0;
  // On masque sur le personnage : l'effet d'aperçu par son id, PLUS la famille « hôte particule »
  // tant qu'un aperçu est en cours — celle-ci est anonyme, on ne peut la viser que globalement.
  // ⚠ Conséquence assumée : pendant un survol, un costume PORTÉ de cette même famille disparaît lui
  // aussi du personnage. C'est bref, et ça évite l'effet parasite qui restait collé au sprite.
  int sup[2];
  int nsup = 0;
  if (g_ez_preview_cid > 0) sup[nsup++] = g_ez_preview_cid;
  if (g_ez_preview_active != 0) sup[nsup++] = ez_capture::kStrParticleId;
  ez_capture::SetSuppressedIds(ez_capture::kSlotDoll, nsup ? sup : nullptr, nsup);
}

// Dessin du composite EZ sur le doll. Les sommets capturés sont en écran (XYZRHW, même FVF/chemin de
// draw que les STR -> RenderPrim_DrawRecord partagé) ; l'ancre écran et l'échelle de profondeur sont
// fournies par le module (ez_capture::ProjectAnchor). Repasser à false si régression.
bool g_ez_enabled = true;  // verts = écran (XYZRHW confirmé) ; outliers filtrés par proximité (g_ez_max_r)
// Calibrage manuel de l'échelle EZ sur le doll (défaut 1.0 = ratio physique s/S_live).
float g_ez_cal = 1.0f;
// Plancher du FIT : le doll ne rétrécit pas en-dessous de ce ratio de son échelle de base, même si
// l'effet est plus grand que le cadre (au-delà, l'effet déborde et est rogné par le clip du rect).
// Évite le « doll minuscule » quand un effet (aura) s'étale large. Réglable (0.5-0.8 raisonnable).
float g_ez_fit_floor = 0.6f;
// Rayon écran (px) autour de l'ancre au-delà duquel un triangle est rejeté (outlier hors-effet).
float g_ez_max_r = 700.0f;
// Blend : chaque primitive porte SON PROPRE SRCBLEND/DESTBLEND, lu dans l'enregistrement par le module
// et rejoué tel quel (DrawOpts::blend_mode = 0). C'est le seul mode correct : un même effet mélange
// additif et alpha dans la même frame. g_ez_additive=true force l'additif GLOBAL (outil de comparaison
// des glows uniquement — il crame les primitives alpha).
bool g_ez_additive = false;

// FIT : les quads sont dessinés LIVE (animés) chaque frame ; seule la BBOX de l'effet est figée une
// fois (stabilité du scale doll), invalidée au changement de hat effect (0x0A3B) et nettoyée après
// quelques frames sans capture (effet retiré).
bool     g_ez_frozen_valid = false;  // bbox FIT calculée pour l'effet courant
int      g_ez_empty_frames = 0;      // frames consécutives SANS effet capturé (Count()==0) -> nettoie la bbox
// BBox de l'effet figé, en unités « doll par unité d'échelle s » (= (v-ancre)/S). Sert au FIT :
// RenderPlayerAvatar réduit s (perso ET effet ensemble) pour que l'effet tienne dans le cadre fixe.
float    g_ez_fz_dx0 = 0.0f, g_ez_fz_dx1 = 0.0f, g_ez_fz_dy0 = 0.0f, g_ez_fz_dy1 = 0.0f;

// Composite les primitives EZ capturées par le module sur le doll. Les sommets sont en écran ABSOLU
// (projetés monde par le natif ce frame) ; ez_capture::ProjectAnchor donne l'ancre écran (ax,ay) de
// l'effet et S = px écran par unité canvas à cette profondeur. Doll = (ox + (vx-ax)·R, oy + (vy-ay)·R)
// avec R = (s/S)·cal (1 unité canvas monde -> s px doll, comme le sprite). No-op si rien n'est capturé
// ou si la projection échoue.
// `before` : ne dessine que la phase demandée. Z-ORDER par primitive = bit 0x8 des FLAGS natifs (RE
// render-order, workflow 2026-07-16) : le jeu range chaque primitive dans un BUCKET, et le bit 0x8 =
// "render before character" (bucket dessiné AVANT le perso, donc DERRIÈRE lui). Le module l'applique
// via DrawOpts::use_zorder/draw_behind. Un effet est TOUJOURS tout-devant OU tout-derrière (le moteur
// ne l'intercale jamais entre les couches perso).
// PAS de reset ici (appelé 2×/frame) : la capture est vidée sur frontière de frame par le module.
// Construit le filtre d'effets d'une surface : les hat effects ÉQUIPÉS, plus l'effet d'APERÇU si
// cette surface le montre. Factorisé pour que le DESSIN, la MESURE (bbox du FIT) et la DÉCISION
// d'appliquer le FIT voient rigoureusement le même sous-ensemble.
// `ids` doit pouvoir accueillir 9 entrées. Renvoie le nombre d'ids écrits.
int BuildEzFilter(int* ids, bool with_preview) {
  int n = 0;
  for (int i = 0; i < g_ez_target_count && n < 8; ++i) ids[n++] = g_ez_target_ids[i];
  if (with_preview && g_ez_preview_cid > 0) {
    bool dup = false;
    for (int i = 0; i < n; ++i) if (ids[i] == g_ez_preview_cid) { dup = true; break; }
    if (!dup) ids[n++] = g_ez_preview_cid;
  }
  return n;
}

// Nombre de primitives capturées qui concernent LE DOLL (équipés seulement). ⚠ Ne pas utiliser
// ez_capture::Count() pour cela : il compte TOUS les effets du joueur, donc l'aperçu survolé
// ailleurs — le doll croirait alors avoir quelque chose à cadrer.
int EzPrimCountForDoll() {
  int ids[9];
  ez_capture::DrawOpts o;
  o.id_count = BuildEzFilter(ids, /*with_preview=*/false);
  o.ids = ids;
  o.include_effmgr = false;  // les CEffectMgr portent l'id concret : filtrés par `ids` (cf. DrawEzCapTris)
  o.include_str_particle = (g_ez_preview_active == 0);  // même règle que DrawEzCapTris
  const ez_capture::Prim* p = ez_capture::Prims();
  const int total = ez_capture::Count();
  int n = 0;
  for (int i = 0; i < total; ++i)
    if (ez_capture::Matches(p[i], o)) ++n;
  return n;
}

// `with_preview` : inclure l'effet d'APERÇU en cours (survol cash-shop / description d'item).
// ⚠ Réservé aux surfaces d'aperçu. Le doll de la fiche perso doit montrer ce que le personnage PORTE,
// pas ce qu'on est en train de survoler ailleurs — sinon l'effet survolé apparaît aussi sur le doll.
void DrawEzCapTris(ImDrawList* dl, float ox, float oy, float s, bool before, bool with_preview) {
  if (!dl) return;
  // ANCRE ÉCRAN LIVE : reprojetée CHAQUE frame. Comme elle suit l'acteur, (vx-ax) annule la
  // translation quand le perso marche, tout en gardant les primitives LIVE -> l'ANIMATION joue
  // (l'ancien freeze figeait UN instantané -> position stable mais anim gelée).
  // Filtre = effets ÉQUIPÉS + l'effet d'APERÇU en cours. Construit AVANT l'ancre : celle-ci doit être
  // celle d'un effet QU'ON DESSINE — la demander sans filtre projetait la position du premier effet
  // capturé, souvent un autre (aura, statut), d'où une ancre sans rapport et un doll vide.
  int ids[9];
  const int id_count = BuildEzFilter(ids, with_preview);
  ez_capture::DrawOpts o;
  o.ox = ox; o.oy = oy;
  o.blend_mode = g_ez_additive ? 2 : 0;   // 0 = blend natif par primitive (correct)
  o.use_zorder = true; o.draw_behind = before;
  o.max_r = g_ez_max_r;                   // filtre de proximité : rejette les outliers « nuages »
  o.ids = ids; o.id_count = id_count;
  // Famille CEffectMgr : filtrage PRÉCIS par id d'instance (`ordinal + 0x98a`), et non plus « tout
  // ou rien ». C'est ce qui permet enfin de ne montrer sur le doll que les costumes PORTÉS, et de
  // réserver le costume SURVOLÉ à la surface d'aperçu.
  // ⚠ L'essai en jeu a montré que ces effets passent bel et bien par ce chemin (les exclure vidait
  // le doll ET l'aperçu), contrairement à ce qu'une analyse statique avait conclu.
  // ⚠ Plus besoin d'inclusion en bloc : MESURÉ en jeu, les instances CEffectMgr portent l'id CONCRET
  // (ordinal 248 -> 2429), donc le module les expose comme des effect_id normaux et elles passent par
  // le filtre `ids` ci-dessus, exactement comme le chemin EZ. C'est ce qui permet enfin de réserver
  // le costume SURVOLÉ à la surface d'aperçu, et de le MASQUER sur le personnage.
  o.include_effmgr = false;
  // Famille « hôte particule » (CEZ2STREffect → CEZ2STRParticle) : ANONYME, impossible à rattacher à
  // un effet précis. On tranche donc par le CONTEXTE plutôt que par l'id :
  //  - surface d'aperçu -> toujours (elle ne montre qu'un effet à la fois) ;
  //  - doll -> seulement si AUCUN aperçu n'est en cours, car alors ces primitives ne peuvent venir
  //    que d'un effet PORTÉ. Pendant un survol l'origine est ambiguë, on s'abstient.
  // ⚠ Conséquence assumée : pendant un survol, un effet PORTÉ de cette famille disparaît du doll.
  o.include_str_particle = with_preview || (g_ez_preview_active == 0);
  // ⚠ AUCUN id à montrer = ne rien dessiner. La règle du module est « pas de filtre -> tout le
  // capturé » : sans ce garde, un personnage sans hat effect équipé faisait dessiner TOUT le tampon
  // sur le doll (constaté côté lab : les nuages d'ambiance de gonryun redessinés par-dessus l'écran).
  // On ne sort PAS si la famille « hôte particule » est demandée : elle est anonyme, donc légitimement
  // sans id — c'est le cas de la surface d'aperçu.
  if (id_count == 0 && !o.include_str_particle) return;

  float ax, ay, S;
  if (!ez_capture::ProjectAnchor(o, &ax, &ay, &S) || S <= 1e-4f) return;
  const int count = ez_capture::Count();
  const float R = (s / S) * g_ez_cal;
  o.scale = R;
  // FIT : bbox figée UNE fois (l'effet garde ~la même étendue -> évite que le scale du doll jitter à
  // chaque frame d'anim). RenderPlayerAvatar lit g_ez_fz_dx0.. AVANT ce dessin. Invalidée au changement
  // de hat effect (handler 0x0A3B). ax/ay/S restent LIVE (l'ancre s'annule dans (vx-ax), cf. supra).
  // ⚠ On itère sur p.n sommets RÉELS (3 ou 4) : lire un 4e sommet inexistant ramasserait le pointeur
  // de texture et les blends réinterprétés en float -> bbox aberrante.
  // ⚠ SEUL LE DOLL MESURE. La bbox figée ne sert qu'au cadrage du doll (lue par RenderPlayerAvatar),
  // mais cette fonction est appelée AUSSI par le tooltip d'aperçu : le laisser écrire figeait une
  // bbox incluant l'effet SURVOLÉ, et le cadre du doll se redimensionnait pour un effet qu'il ne
  // dessine même pas (constaté 2026-07-19). Un état partagé de plus, avec deux écrivains.
  if (!with_preview && !g_ez_frozen_valid && count > 0) {
    const ez_capture::Prim* caps = ez_capture::Prims();  // LIVE (animé)
    float dx0 = 1e9f, dx1 = -1e9f, dy0 = 1e9f, dy1 = -1e9f;
    const float mr2 = g_ez_max_r * g_ez_max_r, invS = 1.0f / S;
    for (int i = 0; i < count; ++i) {
      const ez_capture::Prim& p = caps[i];
      // ⚠ MÊME filtre que le dessin : sans ça, un effet capturé mais NON dessiné (typiquement
      // l'aperçu survolé ailleurs) élargit la bbox et fait rétrécir le cadre du doll pour « faire
      // tenir » quelque chose qui n'y est pas affiché.
      if (!ez_capture::Matches(p, o)) continue;
      for (int k = 0; k < p.n; ++k) {
        const float ddx = p.x[k] - ax, ddy = p.y[k] - ay;
        if (ddx * ddx + ddy * ddy > mr2) continue;
        if (ddx < dx0) dx0 = ddx; if (ddx > dx1) dx1 = ddx;
        if (ddy < dy0) dy0 = ddy; if (ddy > dy1) dy1 = ddy;
      }
    }
    if (dx1 >= dx0) {
      g_ez_fz_dx0 = dx0 * invS; g_ez_fz_dx1 = dx1 * invS;
      g_ez_fz_dy0 = dy0 * invS; g_ez_fz_dy1 = dy1 * invS;
      g_ez_frozen_valid = true;
    }
  }
  if (!g_ez_enabled) return;  // dessin désactivé (calibrage) — doll propre
  // Dessin délégué : le module gère le z-order (bit 0x8), le blend PAR PRIMITIVE (celui de
  // l'enregistrement) et les couleurs PAR SOMMET (dégradés d'alpha des traînées préservés).
  // `o` est rempli plus haut (l'ancre et l'échelle en dépendent).
  ez_capture::Draw(dl, o);
}

// ── Spawn / pilotage d'un nœud d'effet STR autonome (hat effect) ──────────────
// Recette RE (docs/hat_effect_re.md, agent 2026-07-12) : alloc(0x11ca8)+ctor,
// Effect_LoadStrByEffectId(id CONCRET), horloge d'anim = node+0x178 (ms relatif),
// Effect_UpdateStrKeyframes remplit les workBuf, puis on ITÈRE les couches nous-mêmes
// (SANS les gardes visibilité de Effect_DrawStrFrameQuads) en appelant le SubmitStrQuad
// HOOKÉ (capture). Nœuds MIS EN CACHE par id concret (persistants : pas d'alloc/dtor
// par frame, pas de gestion de durée de vie fragile ; ~72 Ko × nb d'effets distincts).
constexpr uintptr_t kEffCtor     = 0x00b90780;  // EffectInst_Ctor_StrNode(node)
constexpr uintptr_t kEffLoadStr  = 0x00bb4170;  // Effect_LoadStrByEffectId(node,src,id,x,y,z)
constexpr uintptr_t kEffUpdateKF = 0x00bced10;  // Effect_UpdateStrKeyframes(node,offXYZ,f,f)
constexpr int       kEffNodeSize = 0x11ca8;     // taille du nœud (alloc)
constexpr int kEffCStr       = 0x7ec;    // CStr* chargé (0 si échec de chargement)
constexpr int kEffLayerBase  = 0x7f0;    // valeur = CStr+0x110 (base couches/keyframes)
constexpr int kEffClockMs    = 0x178;    // horloge d'anim (ms) — pilotée par nous
constexpr int kEffWorkBuf0   = 0x7f4;    // workBuf couche L = node+0x7f4+L*0x74
constexpr int kEffActiveFlag = 0x119fc;  // flag actif couche L = node+0x119fc+L (octet)
constexpr int kEffLoopMax    = 0x11c80;  // boucles max (compteur node+0x11c7c ; Effect_ResetStrLoop rembobine +0x178=0)
// Cadence native EXACTE (RE : GameLogic_ComputeTimestepCount 0x00c16a90) : le pas fixe est
// DAT_015e5a9c = 0x10 = 16 ms/frame (flag DAT_0122b3dc=1) -> 62.5 Hz, INDÉPENDANT du FPS de rendu.
// node+0x178 (index de frame ENTIER) avance de +1 toutes les 16 ms. f(T) = floor(T_ms / 16).
constexpr unsigned kStrFrameMs = 16;  // ms par frame .str (pas fixe natif, non deviné)

// Bridge Lua brut (Lua 5.1) — même mécanique que StatusName (character_sheet) : évite
// le wrapper varargs (string BYVAL). Adresses partagées avec les autres plugins.

// Un nœud d'effet en cache : le nœud lui-même + son tick de départ (horloge relative :
// node+0x178 = GetTickCount()-start, sinon un tick absolu dépasserait toutes les
// keyframes -> effet « fini » = rien). failed = chargement échoué (ne pas re-tenter).
struct StrEffNode {
  void*    node    = nullptr;
  DWORD    last    = 0;      // GetTickCount du dernier tick appliqué (cadence delta temps réel)
  unsigned acc     = 0;      // reste de ms (< 16) pas encore converti en frame native
  bool     primed  = false;  // frame 0 rendue au moins une fois
  bool     failed  = false;
};
std::unordered_map<int, StrEffNode> g_str_nodes;  // id concret -> nœud
float g_str_srcpos[0x20] = {0};                   // struct srcPos (XYZ @+0x10), zéro

// Table itemId(client) -> ordinal de hat effect (e_hat_effects), poussée par le serveur
// au login (ZC 0x0F17, cf. clif_bourgeon_hateffect_map). AUTORITATIVE (scan des scripts
// item_db) : le client ne mappe pas item->ordinal nativement (effectHatItemTable = simple
// appartenance). Sert à ItemToHatOrdinal pour prévisualiser les costumes sans viewid.
std::unordered_map<uint32_t, uint16_t> g_hat_item_ord;

// ── Diagnostic (POD) du dernier pilotage d'effet — rempli par les fonctions SEH, loggé
// throttlé (fmt) par l'appelant hors __try (RenderPlayerAvatar). Sert à localiser où la
// chaîne casse : résolution ordinal->concret, charge du .str, comptage de couches, capture.
int g_hatfx_dg_concrete = 0;   // id concret pilotté (0 = résolution ratée)
int g_hatfx_dg_nodeok   = -1;  // -1 non tenté, 0 échec charge/.str absent, 1 nœud chargé
int g_hatfx_dg_layers   = -1;  // layerCount du .str (nb de couches)
int g_hatfx_dg_captured = -1;  // g_str_count après capture (nb de couches capturées)

// Résolution NATIVE ordinal hat -> nom/chemin du .str, via le global Lua GetHatEfResName
// (cf. Effect_ResolveResourceName 0x00af0900). C'est LE résolveur des hat effects par NOM
// (GetHatEffectID renvoie -1 pour eux). Ex : ordinal 48 -> "efst_Gold_Shower\coin2.str".
// POD ONLY (SEH). out vidé si échec.

// Wrapper caché (map = op C++, hors __try). Renvoie le nom .str de l'ordinal, "" si échec
// (non figé : Lua peut ne pas être prêt au 1er appel).
const char* HatOrdinalToResName(int ordinal) {
  static std::unordered_map<int, std::string> cache;
  auto it = cache.find(ordinal);
  if (it != cache.end()) return it->second.c_str();
  char buf[96];
  lua::HatEffectResName(ordinal, buf, sizeof(buf));
  if (buf[0]) return (cache[ordinal] = buf).c_str();
  return "";
}

// Paramètres d'un hat effect (Lua, AUCUN hardcode) :
//   pos_x  = GetHatEfPosX(ord)           : décalage horizontal (HatEffectInfo.lub)
//   before= IsRenderBeforeCharacter(ord): effet DERRIÈRE le perso (true) ou devant (false)
// (Le décalage VERTICAL des effets "tête/scène" vient d'un nudge ÉCRAN -80 natif
//  (CActorSprite_ComputeStrScreenAnchor), NON reproductible à plat sans l'échelle de rendu en jeu S.
//  On ancre donc tous les effets sur l'ORIGINE de l'acteur (pieds) et le .str se place lui-même :
//  cercle magique au sol, pièces au-dessus, etc. -> correct pour les effets sol, un peu bas pour
//  les effets tête/scène qui voudraient le -80.)
struct HatEffectParams { float pos_x = 0.0f; bool before = false; };

// Appel d'un global Lua getter(ord). Le pont lui-même — checkstack, appel
// protégé, dépilage — vit dans `lua::CallGlobal*`, avec les deux autres lectures
// du même genre (le SPR Lab et le nom de ressource d'effet).
//
// ⚠ Le résultat NUMÉRIQUE se lit via lua_tonumber (comme le natif
// Lua_CallGlobal_va pour 'd'/'l'). lua_tolstring convertirait aussi, mais on
// évite l'aller-retour chaîne + atof, qui est fragile.
//
// ⚠ `HatLuaBool` gagne au passage le `CheckStack` que sa version d'origine
// n'avait pas — les deux autres copies l'avaient.
float HatLuaNum(int ordinal, const char* fn, float def) {
  return static_cast<float>(lua::CallGlobalNum(fn, ordinal, def));
}
bool HatLuaBool(int ordinal, const char* fn, bool def) {
  return lua::CallGlobalBool(fn, ordinal, def);
}
// Caché par ordinal. NE met en cache QUE si Lua est prêt (sinon repli non figé -> réessai).
const HatEffectParams& HatOrdinalParams(int ordinal) {
  static std::unordered_map<int, HatEffectParams> cache;
  static const HatEffectParams s_fallback;
  auto it = cache.find(ordinal);
  if (it != cache.end()) return it->second;
  if (!lua::State()) return s_fallback;  // Lua pas prêt -> pas de cache, on réessaiera
  HatEffectParams p;
  p.pos_x   = HatLuaNum(ordinal, "GetHatEfPosX", 0.0f);
  p.before = HatLuaBool(ordinal, "IsRenderBeforeCharacter", false);
  return cache[ordinal] = p;
}

// Adresses de chargement de ressource (comme cashshop_window) + AddRef (0x00a8e800).
constexpr int       kEffDummyId = 0x59;       // StormGust : id concret bidon (setup natif complet)

// Crée un nœud d'effet STR chargé du .str `strName` (par NOM). POD ONLY (SEH -> pas de
// C2712). Stratégie : ctor + Effect_LoadStrByEffectId avec un id concret BIDON (StormGust)
// pour le setup COMPLET du nœud (préambule + init vtable), puis on REMPLACE la ressource
// .str par la nôtre (queue de Effect_LoadStrByEffectId : UITextureMgr_Load + AddRef + champs
// +0x7ec/+0x7f0/+0x119f8). +0x11c80=9999 -> boucle (aperçu permanent). nullptr si échec.
void* StrNode_CreateByName(const char* strName) {
  if (!strName || !strName[0]) return nullptr;
  void* node = std::calloc(1, static_cast<size_t>(kEffNodeSize));
  if (!node) return nullptr;
  bool ok = false;
  __try {
    reinterpret_cast<void(__fastcall*)(void*)>(kEffCtor)(node);
    reinterpret_cast<void(__fastcall*)(void*, void*, void*, int, float, float, float)>(
        kEffLoadStr)(node, nullptr, g_str_srcpos, kEffDummyId, 0.0f, 0.0f, 0.0f);
    // Un .str passe par le MÊME gestionnaire qu'un .bmp : il aiguille par
    // extension. Ce n'est pas un détournement, c'est l'usage prévu.
    void* strObj = ro::texmgr::LoadResource(strName);
    if (strObj) {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + kEffCStr) = strObj;
      reinterpret_cast<void(__fastcall*)(void*)>(ro::texmgr::kAddRefAddr)(strObj);  // garde le .str vivant
      *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + kEffLayerBase) =
          reinterpret_cast<char*>(strObj) + 0x110;
      const int fc = *reinterpret_cast<int*>(reinterpret_cast<char*>(strObj) + 0x118);
      *reinterpret_cast<int*>(reinterpret_cast<char*>(node) + 0x119f8) =
          (*reinterpret_cast<int*>(reinterpret_cast<char*>(strObj) + 0x128) == 0) ? fc - 1 : fc;
      // Boucle "à l'infini" = VALEUR NATIVE EXACTE : les auras qui bouclent (gc_darkcrow, chill…)
      // mettent 9999 dans node+0x11c80 (RE : Effect_LoadStrByEffectId). On la réplique telle quelle.
      *reinterpret_cast<int*>(reinterpret_cast<char*>(node) + kEffLoopMax) = 9999;
      // NB : on NE modifie NI les noms NI le cache du CStr (partagé avec le rendu in-world natif
      // -> corruption/crash). La résolution du BON chemin texture (avec sous-dossier) se fait
      // dans le hook Hooked_StrQuad via le factory (cache par chemin), cf. g_str_cap_subdir.
      ok = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  if (!ok) { std::free(node); return nullptr; }
  return node;
}

// Pilote le nœud À LA MANIÈRE DU NATIF puis ITÈRE les couches -> capture via le hook.
// RE (Effect_UpdateStrKeyframes 0x00bced10 + Effect_ResetStrLoop 0x00bb5b10) : node+0x178 est un
// INDEX DE FRAME ENTIER que le driver natif incrémente de +1 par tick. L'avance d'un keyframe
// n'a lieu QUE si node+0x178 == kf.endFrame EXACTEMENT (égalité entière) -> il FAUT visiter chaque
// entier (donc +1 à la fois, jamais un saut = elapsed_ms). Quand toutes les couches finissent,
// UpdateKF appelle Effect_ResetStrLoop qui rembobine node+0x178=0 -> la BOUCLE est INTERNE au
// nœud (plafond node+0x11c80). Donc : on n'écrit JAMAIS une horloge absolue ; on incrémente de
// `steps` (relatif), 1 UpdateKF par incrément. `prime` = 1er tick -> rend la frame 0 sans avancer.
// POD ONLY (SEH, aucun objet C++ -> évite C2712).
void StrNode_DriveCapture(void* node, int steps, bool prime) {
  __try {
    int* clock = reinterpret_cast<int*>(reinterpret_cast<char*>(node) + kEffClockMs);
    float off3[3] = {0.0f, 0.0f, 0.0f};
    const auto updkf =
        reinterpret_cast<unsigned(__fastcall*)(void*, void*, void*, float, float)>(kEffUpdateKF);
    if (prime) { *clock = 0; updkf(node, nullptr, off3, 0.0f, 0.0f); }  // frame 0
    for (int i = 0; i < steps; ++i) {
      *clock += 1;                              // +1 frame native (relatif ; boucle interne rembobine)
      updkf(node, nullptr, off3, 0.0f, 0.0f);   // avance keyframes (match exact) + remplit workBuf
    }
    void* layerBase =
        *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + kEffLayerBase);
    if (layerBase) {
      int layerCount = *reinterpret_cast<int*>(reinterpret_cast<char*>(layerBase) + 8);
      if (layerCount < 1) layerCount = 1;
      if (layerCount > 48) layerCount = 48;  // borne sûre (évite d'itérer des couches fantômes)
      g_hatfx_dg_layers = layerCount;  // diag
      g_str_cap_active = true;
      int depthIdx = 0;
      // Réplique la boucle de Effect_DrawStrFrameQuads mais SANS les gardes (FUN_00d9d020
      // effets-activés / FUN_00c0afa0 visible) qui recaleraient un nœud autonome.
      for (int Lyr = 1; Lyr < layerCount && g_str_count < 64; ++Lyr) {
        if (*(reinterpret_cast<char*>(node) + kEffActiveFlag + Lyr) != 0) {
          void* layerStruct = reinterpret_cast<char*>(layerBase) + 0x10 + Lyr * 0x380;
          float* workBuf = reinterpret_cast<float*>(
              reinterpret_cast<char*>(node) + kEffWorkBuf0 + Lyr * 0x74);
          // Appel du SubmitStrQuad HOOKÉ -> Hooked_StrQuad capture (camera ignoré).
          reinterpret_cast<StrSubmitFn>(kStrSubmitQuad)(
              node, nullptr, layerStruct, workBuf, depthIdx, nullptr);
          depthIdx += 2;
        }
      }
      g_str_cap_active = false;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { g_str_cap_active = false; }
}

// Pilote le nœud du .str de `concreteId` (crée/charge au 1er appel, cache ensuite) et
// CAPTURE ses couches 2D dans g_str_caps via le hook. Réinitialise g_str_count. No-op si
// l'effet ne charge pas. La MAP (C++) et le SEH sont dans des fonctions SÉPARÉES (C2712 :
// MSVC interdit __try dans une fonction qui déroule aussi des objets C++).
void CaptureHatEffectOrdinal(int ordinal) {
  InstallStrCapture();
  g_str_count = 0;
  g_hatfx_dg_concrete = ordinal;
  g_hatfx_dg_nodeok = -1; g_hatfx_dg_layers = -1; g_hatfx_dg_captured = -1;  // reset diag
  if (!g_orig_str_quad || ordinal <= 0) return;
  StrEffNode& slot = g_str_nodes[ordinal];  // clé = ordinal (crée l'entrée si absente)
  if (slot.failed) { g_hatfx_dg_nodeok = 0; return; }
  if (!slot.node) {
    const char* name = HatOrdinalToResName(ordinal);  // GetHatEfResName -> .str
    if (!name || !name[0]) { g_hatfx_dg_nodeok = 0; return; }  // Lua pas prêt : retenter (pas failed)
    void* node = StrNode_CreateByName(name);
    if (!node) { slot.failed = true; g_hatfx_dg_nodeok = 0; return; }
    slot.node = node;
    slot.last = GetTickCount();
    slot.acc = 0;
  }
  g_hatfx_dg_nodeok = 1;
  // dir du .str (partie dossier de GetHatEfResName, ex. "efst_Gold_Shower\") pour le résolveur
  // NATIF subdir-aware du hook — CASSE D'ORIGINE (comme _splitpath natif ; rend in-world donc OK).
  // "" -> effet racine. Remplit aussi la std::string POD passée à Str_GetLayerTextureWithDir.
  {
    const char* name = HatOrdinalToResName(ordinal);  // caché
    const char* bs = name ? std::strrchr(name, '\\') : nullptr;
    if (bs) {
      char dir[80];
      int n = static_cast<int>(bs - name) + 1;         // inclut le '\' final (comme _splitpath)
      if (n > 0 && n < static_cast<int>(sizeof(dir))) {
        std::memcpy(dir, name, static_cast<size_t>(n));
        dir[n] = '\0';
        SetStrCapDir(dir);
      } else {
        SetStrCapDir("");
      }
    } else {
      SetStrCapDir("");
    }
  }
  // Cadence temps réel EXACTE (16 ms/frame natif) par ACCUMULATEUR DE DELTA : on ne compte que le
  // temps écoulé entre deux captures RÉELLES (la fiche ne rend/tick que visible), donc pas de
  // fast-forward à la réouverture. steps = frames natives dues ce tick ; le reste <16 ms est gardé
  // dans slot.acc. La BOUCLE est interne au nœud (Effect_ResetStrLoop rembobine node+0x178) ->
  // aucun reset d'horloge ici. Après un gros trou (fiche masquée), on borne + on vide le backlog.
  const DWORD now = GetTickCount();
  slot.acc += now - slot.last;   // ms écoulées depuis le dernier tick (delta, non absolu)
  slot.last = now;
  int steps = static_cast<int>(slot.acc / kStrFrameMs);
  slot.acc -= static_cast<unsigned>(steps) * kStrFrameMs;
  if (steps > 8) { steps = 8; slot.acc = 0; }  // trou (fiche masquée) : reprendre, pas rattraper
  StrNode_DriveCapture(slot.node, steps, !slot.primed);
  slot.primed = true;
  g_hatfx_dg_captured = g_str_count;
}

// ── L'apparence du joueur, telle que le composeur la veut ────────────────────
//
// Partagée par l'avatar plein-corps, le portrait de tête et l'export GIF : trois
// vues du MÊME personnage, qui ne doivent jamais diverger sur ce qu'il porte.
//
// `eq` doit survivre à l'usage de `look` : celui-ci ne garde que des POINTEURS
// vers ses chaînes de chemins.
// ── Les couleurs composées par le joueur, pour un pantin ────────────────────
//
// 🔴 Un pantin recompose ses couleurs À PARTIR DES FICHIERS. Sans ce raccord, il
// afficherait donc la palette d'AVANT pendant que le personnage en scène porte
// les nouvelles — le joueur se verrait de deux couleurs différentes dans la même
// fenêtre.
//
// La palette vient de l'INJECTION, pas d'un recalcul : ce sont exactement les
// octets qui sont rendus à l'écran, et les reproduire ici ferait deux chemins à
// garder d'accord. Le tampon est statique et vit jusqu'au `DrawDoll` de la même
// frame, qui n'en garde rien (il ne fait qu'un slot de teinte).
bool FillOwnDollPalette(ro::DollLook* look) {
  static uint8_t rgba[1024];
  static std::string key;
  const uint32_t gid = rag::OwnAccountId();
  if (gid == 0) return false;

  // 🔴 Le sprite de corps LU sur l'acteur, pas celui qu'on déduirait de
  // (classe, sexe, monture). La déduction rejoue `Job_ResolveBodyClass` et ses
  // cas particuliers, et là où elle diverge le pantin montre un AUTRE corps que
  // celui à l'écran — les 3e et 4e classes s'affichaient en tenue de base
  // (constaté le 2026-08-12). Statique : `DollLook` ne garde qu'un pointeur, et
  // le dessin a lieu plus tard dans la frame.
  static char spr[352];
  if (fx::palette_inject::ActorBodySpritePath(gid, spr, sizeof(spr)) &&
      spr[0] != '\0')
    look->body_spr_override = spr;

  // ── Les CHEVEUX d'abord, car ils sont indépendants du corps ───────────────
  // Le pantin compose sa tête depuis un NUMÉRO de couleur, exactement comme le
  // fait le styliste — il n'y a donc rien à injecter, juste à lui donner le bon.
  // 🔴 Sans ça, il lirait la couleur du serveur : la tête du pantin resterait à
  // sa teinte d'origine pendant que celle du personnage en scène a changé.
  //
  // La variante du corps PORTÉ : le style se range par corps, et la couleur de
  // cheveux qu'il faut au pantin est celle de la recette réellement appliquée —
  // le joueur peut très bien avoir choisi une teinte de cheveux différente pour
  // sa monture. Le sprite vient d'être lu juste au-dessus, donc la clé est
  // gratuite ; un corps illisible retombe sur le style principal.
  ro::PaletteRecipe recette;
  if (fx::style_sync::LocalRecipe(ro::BodySpriteKey(spr), &recette) &&
      recette.hair_palette_id > 0)
    look->hair_color = recette.hair_palette_id;
  // 🔴 Rien à faire pour la COUPE : elle vit dans la globale d'apparence que le
  // client tient à jour sur ZC_SPRITE_CHANGE, et `BuildOwnDollLook` l'a déjà
  // lue. C'est le serveur qui la possède, donc elle est juste par construction.

  if (!fx::palette_inject::InjectedPalette(gid, rgba, sizeof(rgba)))
    return false;
  // 🔴 La clé identifie le CONTENU : le cache de teintes du composeur partage
  // ses textures entre appels de même clé, donc une clé figée ferait ressortir
  // les couleurs précédentes à chaque mouvement de curseur.
  key = fx::palette_cache::DollKey(gid, rgba);
  look->body_palette = rgba;
  look->body_palette_key = key.c_str();
  return true;
}

void BuildOwnDollLook(bool show_costume, ro::DollLook* look,
                      rag::OwnActorSprites* eq) {
  using GetSexFn = int(__fastcall*)(void*, void*);
  look->sex = reinterpret_cast<GetSexFn>(rag::kOwnSexAddr)(
      reinterpret_cast<void*>(rag::kSessionAddr), nullptr);
  look->job = rag::OwnDisplayedJobId();
  look->hair          = *reinterpret_cast<int*>(rag::kOwnHairStyleAddr);
  look->hair_color    = *reinterpret_cast<int*>(rag::kOwnHairColorAddr);
  look->clothes_color = *reinterpret_cast<int*>(rag::kOwnClothesColorAddr);
  // 🔴 `body` = la CLASSE, pas un style. C'est le second argument de
  // `Job_ResolveBodyClass`, et c'est lui qui nomme le sprite de corps — le
  // laisser à 0 demandait la classe 0, donc Novice pour tout le monde.
  //
  // ⚠ La classe BRUTE, jamais celle ajustée par la monture : `look->job` porte
  // déjà l'ajustement, et `Job_ResolveBodyClass` refait lui-même le remap
  // (4008 -> 4014) à partir des deux.
  look->body = rag::OwnJobId();
  // Couleurs composées par le joueur, si elles sont posées sur son acteur. Sans
  // ça, ce pantin montrerait les couleurs d'avant pendant que le personnage en
  // scène porte les nouvelles.
  FillOwnDollPalette(look);
  // ── Coiffes : les VUES du serveur, pas les items portés ───────────────────
  //
  // 🔴 Le viewID ne se déduit PAS de l'équipement. Moonlight a un système de
  // TRANSMOGRIFICATION (`moon/stylist.npc`) : un `setlook LOOK_HEAD_TOP, <vue>`
  // donne à une coiffe équipée le sprite d'un AUTRE chapeau, sans rien changer à
  // l'item porté. Le serveur mémorise la substitution (`sd->setlook_head_*`) et
  // la ré-applique après chaque recalcul d'apparence (`pc_set_costume_view`).
  //
  // Le tableau d'équipement du client ne peut donc pas répondre : son champ de
  // vue (+0x70) vient du `wItemSpriteNumber` du paquet, que `clif_item_equip`
  // remplit avec le `look` de l'item_db — la valeur de CATALOGUE, jamais la
  // substituée. En dériver l'apparence, c'est dessiner l'ITEM au lieu de ce que
  // le monde voit.
  //
  // Ce que le monde voit arrive par ZC_SPRITE_CHANGE, et le client le range dans
  // ses globales d'apparence — celles-là mêmes dont il habille l'acteur en
  // scène. On les lit telles quelles : le serveur y a DÉJÀ tranché la précédence
  // des costumes, la déduplication d'une coiffe multi-slots (il met 0 dans les
  // slots couverts) et la transmogrification. Rien à recalculer.
  //
  // ⚠ Elles ne savent RIEN de la distinction costume / équipement réel : elles
  // ne portent que le résultat. « Voir les costumes » décoché reste donc servi
  // par `ResolveHeadgearViews`, qui remonte aux items — et qui, à ce titre,
  // ignore la transmogrification. C'est cohérent : cette vue-là montre ce que le
  // personnage PORTE, pas ce à quoi il ressemble.
  if (show_costume) {
    AssignHeadgearLayers(*reinterpret_cast<int*>(kHeadLowView),   // slot equip 0
                         *reinterpret_cast<int*>(kHeadTopView),   // slot equip 8
                         *reinterpret_cast<int*>(kHeadMidView),   // slot equip 9
                         &look->head_top, &look->head_mid, &look->head_low);
  } else {
    ResolveHeadgearViews(&look->head_top, &look->head_mid, &look->head_low);
  }
  // La CAPE suit exactement la même règle, et pour la même raison : `setlook
  // LOOK_ROBE` existe aussi (`sd->setlook_robe`). La vue du serveur d'abord ; la
  // table d'équipement seulement pour montrer la cape PORTÉE.
  //
  // ⚠ La branche « costumes masqués » lisait elle aussi la globale, donc une
  // cape de costume restait affichée alors qu'on demandait l'équipement réel.
  // Elle remonte désormais au slot 2 de la table normale : les deux tables
  // partagent l'indexage des slots — la cape de costume se lisait au même index
  // 2 de la table costume, comme les trois têtes vivent en 0/8/9 dans les deux.
  if (show_costume) {
    look->garment = *reinterpret_cast<int*>(kGarmentView);
  } else {
    const uintptr_t genG = rag::kSessionAddr + 0x17d0 + 2 * 0xf8;  // cape portée
    look->garment = (*reinterpret_cast<int*>(genG + 4) != 0)
                        ? *reinterpret_cast<int*>(genG + 0x70)
                        : 0;
  }

  // Arme, bouclier, traînée, chariot : les chemins que le CLIENT a résolus.
  // Hors jeu la lecture échoue et tout reste vide — mains nues, le bon repli.
  rag::ReadOwnActorSprites(eq);
  look->weapon.spr_base       = eq->weapon_spr;
  look->weapon.act_base       = eq->weapon_act;
  look->weapon_trail.spr_base = eq->trail_spr;
  look->weapon_trail.act_base = eq->trail_act;
  look->shield.spr_base       = eq->shield_spr;
  look->shield.act_base       = eq->shield_act;
}

}  // namespace

void BasicInfo::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  in_game_ = (mode_type == ModeMgr::ModeType::kGame);
  // Repart d'une liste vide à l'entrée en jeu : le serveur re-pousse la liste
  // complète des hat effects au spawn (0x0A3B status=1). Évite qu'un effet d'un
  // perso précédent persiste (aucun status=0 n'est émis à la déconnexion).
  if (in_game_) { own_hat_effects_.clear(); g_ez_frozen_valid = false; }
}

// Tooltip d'aperçu d'un équipement porté par le perso (appelé par item_desc au
// survol de « ViewID : N »). Capture le perso + l'item (viewID dans le slot) via
// le moteur du portrait, puis composite les sprites dans un tooltip ImGui. Ne
// fait rien si l'item n'est pas un headgear/garment (slot PV_NONE).
bool BasicInfo::CanPreview(int emplacement) const {
  return MapEmplacementToSlot(emplacement) != PV_NONE;
}

void BasicInfo::RenderItemPreviewTooltip(int view_id, int emplacement,
                                               int hat_ordinal) {
  // Rien à montrer si ni sprite (viewid) ni effet (hat effect) : sortie.
  if (view_id == 0 && hat_ordinal == 0) return;
  const PvSlot slot = MapEmplacementToSlot(emplacement);
  // Un sprite non-prévisualisable (slot inconnu) SANS effet -> rien. Avec un hat effect,
  // on rend quand même le perso de BASE (view 0) + l'effet.
  if (slot == PV_NONE && hat_ordinal == 0) return;
  // Molette (pendant le survol) = rotation du perso (dir 0..7), filtrée par le verrou
  // anti-défilement (ui/ro_widgets.h) : il pose la possession de la molette, donc ImGui
  // ne scrolle plus aucune fenêtre pendant l'aperçu (ex. la scrollbar de la description
  // qui se trouve dessous) — mais seulement une fois le curseur posé et hors d'un
  // défilement en cours, sinon parcourir la liste ferait tourner le perso au passage.
  //
  // 🔴 `RegionWheel` et pas `LastItemWheel` : le survol est celui de l'APPELANT, et il
  // n'est pas toujours le dernier item soumis — le lien « ViewID : N » de la description
  // est suivi d'un « (molette : tourner) » avant qu'on n'arrive ici. Les trois appelants
  // (description ×2, cash shop) n'entrent que sous leur propre test de survol.
  static int s_dir = 0;
  const float wheel = mui::RegionWheel("bgn_item_preview_spin", /*hovered=*/true);
  if (wheel != 0.0f) s_dir = (s_dir + (wheel > 0.0f ? 1 : 7)) & 7;  // +1 / -1 avec wrap
  // Carrousel d'animations, ~2,5 s chacune : marche -> repos -> assis.
  static const int kPvAnims[3] = {1, 0, 2};
  const int pv_anim = kPvAnims[(GetTickCount() / 2500u) % 3u];

  // Hauteur du CORPS à l'écran. C'est une taille ABSOLUE et non un ajustement au
  // cadre : le personnage doit garder exactement la même stature d'un article
  // survolé au suivant, sinon la comparaison ne veut plus rien dire. D'où
  // `fit_body_only` côté composeur.
  constexpr float kPvBodyPx = 120.0f;

  // Boîte large + haute : costumes larges (cat à côté) / hauts (hats) ne sont pas
  // cropés. Fond transparent -> l'espace vide est invisible.
  const float box_w = 260.0f, box_h = 240.0f;
  // Fond + bordure transparents : seul le sprite du perso s'affiche (pas de boîte).
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32_BLACK_TRANS);
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32_BLACK_TRANS);
  // Ancre le preview PRÈS du curseur (sinon l'offset de tooltip par défaut + la
  // grande boîte l'éloignent). Le sprite est ancré en bas de la boîte (pieds à
  // top+box_h-14) -> on place la boîte au-dessus du curseur pour que le perso
  // apparaisse juste à côté. Régler kPreviewDX (horizontal) / kPreviewDY (vertical,
  // + = plus bas) pour la distance.
  constexpr float kPreviewDX = -16.0f, kPreviewDY = -10.0f;
  const ImVec2 mouse = ImGui::GetMousePos();
  ImGui::SetNextWindowPos(
      ImVec2(mouse.x + kPreviewDX, mouse.y + kPreviewDY - box_h),
      ImGuiCond_Always);
  ImGui::BeginTooltip();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(box_w, box_h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Hat effect (.str) superposé : MÊME logique que l'avatar (ancre = ORIGINE + hatEffectPos(X) Lua ;
  // ordre derrière/devant = isRenderBeforeCharacter ; cf. RenderPlayerAvatar). Zéro hardcode.
  static const HatEffectParams s_hp_none_pv;
  const HatEffectParams& hp = hat_ordinal ? HatOrdinalParams(hat_ordinal) : s_hp_none_pv;
  // Deux familles : .str name-based (resname NON vide) = chemin piloté off-screen (DrawStrCapLayers) ;
  // sinon (resname vide) = effet EZ/CEffectMgr (hatEffectID, ex. Digital_Space/Perm_Frost) qu'on ne peut
  // PAS piloter hors-scène -> on le SPAWNE temporairement sur le joueur (RequestEzPreview) et on capture
  // le rendu live (ez_capture), composité comme le doll (z-order par primitive via DrawEzCapTris).
  const char* rn_pv = hat_ordinal ? HatOrdinalToResName(hat_ordinal) : "";
  const bool isStr = (rn_pv && rn_pv[0]);
  // Ne PAS spawn l'aperçu si l'effet est DÉJÀ équipé : il rend déjà sur le joueur (capturé/affiché tel
  // quel). Sinon le toggle-remove de l'aperçu retirerait l'instance ÉQUIPÉE (même id) -> script hateffect
  // « overruled » jusqu'à @refresh. On ne spawne QUE les effets NON équipés (le vrai « try before buy »).
  const bool alreadyEquipped =
      std::find(own_hat_effects_.begin(), own_hat_effects_.end(),
                static_cast<uint16_t>(hat_ordinal)) != own_hat_effects_.end();
  if (hat_ordinal && !isStr && !alreadyEquipped) RequestEzPreview(hat_ordinal);  // « try before buy »
  // Les deux passes d'effets, regroupées : elles se posent toutes sur l'ORIGINE
  // de l'acteur et à son échelle — exactement ce que rend `ro::DollPlacement`.
  //
  // ⚠ Structure et non lambdas : la passe ARRIÈRE doit être appelée depuis
  // `ro::DrawDoll` par un pointeur de fonction NU (`doll.h` ne dépend d'aucun
  // en-tête standard), donc rien ne peut être capturé — tout transite par ici.
  struct PvHatPass {
    BasicInfo*  self;
    ImDrawList* dl;
    int         ordinal;
    bool        is_str;
    bool        before;  // le .str se place-t-il DERRIÈRE le personnage ?
    float       pos_x;   // décalage horizontal Lua, en unités sprite

    void Str(float ox, float oy, float s) {
      if (ordinal == 0 || !is_str) return;
      CaptureHatEffectOrdinal(ordinal);
      self->hat_diag_concrete_ = ordinal;
      self->hat_diag_layers_   = g_str_count;
      // Ancre = ORIGINE de l'acteur (oy) comme l'avatar : le .str se place
      // lui-même (au sol, sur la tête, ou centré).
      DrawStrCapLayers(dl, ox + pos_x * s, oy, s);
    }
    void Behind(float ox, float oy, float s) {
      if (before) Str(ox, oy, s);
      if (ordinal && !is_str)
        DrawEzCapTris(dl, ox, oy, s, /*before=*/true, /*with_preview=*/true);
    }
    void Front(float ox, float oy, float s) {
      if (!before) Str(ox, oy, s);
      if (ordinal && !is_str)
        DrawEzCapTris(dl, ox, oy, s, /*before=*/false, /*with_preview=*/true);
    }
  };
  PvHatPass hats{this, dl, hat_ordinal, isStr, hp.before, hp.pos_x};

  // Le personnage de BASE plus le SEUL article survolé : les autres
  // emplacements restent vides.
  //
  // ⚠ Pas de garde SEH ici, volontairement : `__try` interdirait tout objet à
  // destructeur dans cette fonction (C2712), et ces mêmes lectures se font déjà
  // sans garde ailleurs dans ce fichier (cf. ClassName). La fonction n'est de
  // toute façon atteinte qu'avec une session ouverte.
  using GetSexFn = int(__fastcall*)(void*, void*);
  ro::DollLook look;
  look.sex = reinterpret_cast<GetSexFn>(rag::kOwnSexAddr)(
      reinterpret_cast<void*>(rag::kSessionAddr), nullptr);
  look.job = rag::OwnDisplayedJobId();
  look.hair          = *reinterpret_cast<int*>(rag::kOwnHairStyleAddr);
  look.hair_color    = *reinterpret_cast<int*>(rag::kOwnHairColorAddr);
  look.clothes_color = *reinterpret_cast<int*>(rag::kOwnClothesColorAddr);
  // 🔴 `body` = la CLASSE (cf. RenderPlayerAvatar) : c'est elle qui nomme le
  // sprite de corps. À 0, tout le monde s'affichait en Novice.
  look.body = rag::OwnJobId();
  // Idem pour l'aperçu d'équipement (description d'objet, échoppe) : il montre
  // TON personnage portant l'objet, donc tes couleurs.
  FillOwnDollPalette(&look);
  look.head_top = (slot == PV_TOP)     ? view_id : 0;
  look.head_mid = (slot == PV_MID)     ? view_id : 0;
  look.head_low = (slot == PV_LOW)     ? view_id : 0;
  look.garment  = (slot == PV_GARMENT) ? view_id : 0;

  ro::DollPlacement pl;
  ro::DollDrawOpts o;
  o.dir           = s_dir;
  o.anim          = pv_anim;
  o.anim_seconds  = static_cast<float>(ImGui::GetTime());
  o.fit_body_only = true;  // stature CONSTANTE d'un article au suivant
  o.out_placement = &pl;
  o.underlay_ctx  = &hats;
  o.underlay = [](void* c, const ro::DollPlacement& p) {
    static_cast<PvHatPass*>(c)->Behind(p.origin_x, p.origin_y, p.scale);
  };
  // Rectangle calé pour que le CORPS fasse kPvBodyPx de haut, pieds à 14 px du
  // bas de la boîte : `fit_body_only` rend l'échelle égale à hauteur/corps.
  const bool drawn = ro::DrawDoll(dl, look, p0.x,
                                  p0.y + box_h - 14.0f - kPvBodyPx, box_w,
                                  kPvBodyPx, o);
  const float ox = drawn ? pl.origin_x : 0.0f;
  const float oy = drawn ? pl.origin_y : 0.0f;
  const float s  = drawn ? pl.scale : 1.0f;
  // DEVANT le personnage. 🔴 Conditionné au pantin : sans lui il n'y a pas
  // d'ancre, et un effet ancré sur (0,0) atterrirait dans le coin de l'écran.
  if (drawn) hats.Front(ox, oy, s);
  ImGui::EndTooltip();
  ImGui::PopStyleColor(2);
}

// Résout item id -> ordinal de hat effect via la table poussée par le serveur au login
// (g_hat_item_ord, ZC 0x0F17). Autoritative (scan des scripts item_db). 0 si l'item n'a
// pas de hat effect (ou table pas encore reçue). Le natif ne mappe PAS item->ordinal :
// effectHatItemTable côté client n'est qu'une appartenance, et GetHatEffectID prend
// l'ordinal (pas l'itemId) -> d'où le push serveur.
int BasicInfo::ItemToHatOrdinal(int item_id) {
  if (item_id <= 0) return 0;
  auto it = g_hat_item_ord.find(static_cast<uint32_t>(item_id));
  return (it != g_hat_item_ord.end()) ? static_cast<int>(it->second) : 0;
}

// ── Avatar plein-corps de la fiche de personnage ─────────────────────────────
//
// Le personnage entier — corps, tête, coiffes, cape, arme, bouclier — plus ses
// effets de costume et son chariot, composité dans la fenêtre ImGui courante.
//
// 🔴 Plus aucune CAPTURE ici. Le pantin est assemblé par `ro::DrawDoll` à partir
// des .spr/.act, comme le char-select et l'aperçu d'article. Ce qui restait
// propre à cette vue et l'y retenait :
//
//   - l'ARME et le BOUCLIER, que le composeur ne savait pas assembler. Il le
//     sait désormais, à condition qu'on lui donne leurs fichiers — ce que
//     `rag::ReadOwnActorSprites` lit sur l'acteur du joueur ;
//   - le CADRAGE, centré sur le corps et non sur la silhouette, pour qu'une arme
//     large ne pousse pas le personnage de côté (`center_on_body`) ;
//   - le PLAFOND d'échelle, qui fait rétrécir le personnage AVEC un effet de
//     costume trop grand pour le cadre (`scale_limit`).
//
// Et ce qui n'a rien à faire dans un assemblage : les effets (.str et EZ) et le
// CHARIOT sont des sprites indépendants, posés ici même sur le repère que le
// composeur rend — derrière via son rappel, devant après son retour.
//
// ⚠ Le cadrage n'est plus FIGÉ par une clé (rect, pose, apparence) comme
// autrefois : il n'a plus besoin de l'être. Le composeur mesure sur l'image 0 de
// chaque pièce, jamais sur l'image dessinée — l'échelle ne peut donc pas
// « respirer » au rythme de l'animation, qui était toute la raison du gel.
//
// À appeler entre Begin/End. anim = type d'action, dir = 0..7 (0 = face).
void BasicInfo::RenderPlayerAvatar(float x, float y, float w, float h,
                                         int anim, int dir, bool animate, bool show_costume) {
  if (Bourgeon::Instance().client().session().aid() == 0) return;
  const float pad = 4.0f;
  if (w <= 2.0f * pad || h <= 2.0f * pad) return;

  // ── Apparence LIVE ────────────────────────────────────────────────────────
  //
  // ⚠ Pas de garde SEH dans cette fonction, volontairement : `__try` y
  // interdirait tout objet à destructeur (C2712), et il y en a. Les lectures de
  // globales ci-dessous se font déjà sans garde ailleurs dans ce fichier, et la
  // fonction n'est atteinte qu'avec une session ouverte.
  ro::DollLook look;
  rag::OwnActorSprites eq;
  BuildOwnDollLook(show_costume, &look, &eq);
  // ── Le chariot ────────────────────────────────────────────────────────────
  //
  // Sprite INDÉPENDANT, pas une pièce du personnage : il traîne ~1 tuile
  // derrière lui dans le monde. On le place à plat, sans lire ni sa position ni
  // la caméra — la RE montre que sa position live ne converge pas (elle FIGE
  // sous un seuil de distance), et la lire faisait dériver le pantin dès que le
  // joueur marchait.
  //
  // Carte monde -> écran mesurée sur la matrice de vue (caméra standard, termes
  // croisés nuls) : X monde -> X écran, Z monde -> Y écran écrasé de kCamPitch,
  // Y écran vers le bas. « Derrière » = (-sin, cos) du cap.
  const int   d      = dir & 7;
  const int   d_eff  = (anim == 4) ? ((d + kCartCombatShift) & 7) : d;
  const float rad    = static_cast<float>(-d_eff) * 45.0f * 0.01745329252f;
  ro::SpriteRes cart_res;
  bool cart_ok = false;
  if (eq.cart_spr[0])
    cart_ok = ro::LoadSpritePair(eq.cart_spr,
                                 eq.cart_act[0] ? eq.cart_act : nullptr, nullptr,
                                 &cart_res);
  const float now = static_cast<float>(ImGui::GetTime());
  // Le chariot ne défile QU'EN MARCHE : la RE prouve que son image n'avance que
  // si le joueur se DÉPLACE — en combat sur place il reste figé. Ce défilement
  // est justement ce qui simule le déplacement (le châssis reste fixe, la caisse
  // rebondit d'une image à l'autre).
  const bool  cart_anim = cart_ok && animate && anim == 1;
  const unsigned cart_action = static_cast<unsigned>((d_eff + kCartDirOffset) & 7);

  // ── Les effets de costume et le chariot, en deux passes ───────────────────
  //
  // ⚠ Structure et non lambdas : la passe ARRIÈRE est appelée depuis
  // `ro::DrawDoll` par un pointeur de fonction NU (`doll.h` ne dépend d'aucun
  // en-tête standard), donc rien ne peut être capturé — tout transite par ici.
  struct AvPass {
    BasicInfo*           self;
    ImDrawList*          dl;
    const ro::SpriteRes* cart;
    unsigned             cart_action;
    unsigned             cart_frame;
    float                cart_dx, cart_dy;  // en unités .act
    bool                 cart_behind;

    void Cart(const ro::DollPlacement& p) {
      if (!cart || !cart->res) return;
      ro::SpriteQuad q[24];
      const int m = ro::SpriteResolveFrame(*cart, cart_action, cart_frame, q, 24,
                                           /*apply_rotation=*/false);
      for (int i = 0; i < m; ++i) {
        if (!q[i].tex) continue;
        ImVec2 c[4];
        for (int k = 0; k < 4; ++k)
          c[k] = ImVec2(p.origin_x + (q[i].corner[k].x + cart_dx) * p.scale,
                        p.origin_y + (q[i].corner[k].y + cart_dy) * p.scale);
        dl->AddImageQuad(reinterpret_cast<ImTextureID>(q[i].tex), c[0], c[1],
                         c[2], c[3], ImVec2(q[i].uv0.x, q[i].uv0.y),
                         ImVec2(q[i].uv1.x, q[i].uv0.y),
                         ImVec2(q[i].uv1.x, q[i].uv1.y),
                         ImVec2(q[i].uv0.x, q[i].uv1.y), q[i].tint);
      }
    }

    // Effets .str (costumes SANS viewid), pilotés depuis un nœud autonome puis
    // compositéss. AUCUN réglage en dur : l'ancre ET l'ordre derrière/devant
    // viennent de HatEffectInfo.lub par Lua.
    //
    // Ancre = ORIGINE de l'acteur, c'est-à-dire ses pieds. Le .str place SON
    // contenu par rapport à ça : cercle magique au SOL, pluie de pièces
    // AU-DESSUS, scène CENTRÉE — une seule ancre les sert tous.
    void Str(const ro::DollPlacement& p, bool before_phase) {
      self->hat_diag_active_ = static_cast<int>(self->own_hat_effects_.size());
      for (uint16_t ordinal : self->own_hat_effects_) {
        const HatEffectParams& hp = HatOrdinalParams(static_cast<int>(ordinal));
        if (hp.before != before_phase) continue;
        CaptureHatEffectOrdinal(static_cast<int>(ordinal));
        self->hat_diag_concrete_ = static_cast<int>(ordinal);
        self->hat_diag_layers_   = g_str_count;  // 0 = résolution/charge ratée
        DrawStrCapLayers(dl, p.origin_x + hp.pos_x * p.scale, p.origin_y,
                         p.scale);
      }
    }

    void Behind(const ro::DollPlacement& p) {
      // Effets EZ/CEffectMgr (hatEffectID) capturés dans la passe monde et
      // ré-ancrés ici. Le z-order est PAR QUAD, via le bit natif que la capture
      // conserve : phase derrière avant le personnage, phase devant après.
      // Doll de la fiche : effets ÉQUIPÉS seulement, jamais l'article survolé
      // ailleurs.
      DrawEzCapTris(dl, p.origin_x, p.origin_y, p.scale, /*before=*/true,
                    /*with_preview=*/false);
      Str(p, true);
      if (cart_behind) Cart(p);
    }

    void Front(const ro::DollPlacement& p) {
      if (!cart_behind) Cart(p);
      Str(p, false);
      DrawEzCapTris(dl, p.origin_x, p.origin_y, p.scale, /*before=*/false,
                    /*with_preview=*/false);
    }
  };

  AvPass pass{};
  pass.self        = this;
  pass.dl          = ImGui::GetWindowDrawList();
  pass.cart        = cart_ok ? &cart_res : nullptr;
  pass.cart_action = cart_action;
  pass.cart_frame =
      cart_anim ? ro::SpriteFrameIndex(cart_res, cart_action, now) : 0u;
  pass.cart_dx = -std::sin(rad) * kCartTilePx + kCartNudgeX;
  pass.cart_dy = -std::cos(rad) * kCamPitch * kCartTilePx + kCartNudgeY;
  // Z-order : le chariot passe DEVANT le corps pour les trois orientations « de
  // dos » {3,4,5} — le personnage regarde à l'opposé, le chariot est donc PLUS
  // PRÈS de la caméra. Il est derrière pour les cinq autres.
  pass.cart_behind = (d_eff < 3 || d_eff > 5);

  // ── Faire tenir un effet de costume plus grand que le cadre ───────────────
  //
  // Le CADRE ne bouge pas ; c'est l'effet qui rétrécit pour y tenir, et le
  // personnage rétrécit AVEC lui (l'effet suit son échelle). On ne fait que
  // RÉDUIRE, jamais agrandir, et jamais sous un plancher — passé lui, l'effet
  // déborde et se fait rogner plutôt que d'écraser le personnage.
  //
  // ⚠ Le plancher se mesure sur l'échelle de BASE, pas sur celle de la frame
  // précédente : sinon chaque frame plafonnée rabaisserait la suivante, et le
  // pantin s'effondrerait sur lui-même. D'où la mémoire ci-dessous, qui n'est
  // rafraîchie que sur les frames NON plafonnées.
  //
  // ⚠ Le FIT ne s'applique que si un effet est réellement capturé cette frame.
  // Sinon, un costume-effet retiré laisserait le pantin petit jusqu'au prochain
  // paquet. Après quelques frames vides on jette la boîte figée.
  static float s_base_scale = 0.0f;
  float scale_limit = 0.0f;
  if (g_ez_frozen_valid && EzPrimCountForDoll() <= 0) {
    if (++g_ez_empty_frames > 4) g_ez_frozen_valid = false;
  } else if (g_ez_frozen_valid) {
    g_ez_empty_frames = 0;
    const float ew = g_ez_fz_dx1 - g_ez_fz_dx0;
    const float eh = g_ez_fz_dy1 - g_ez_fz_dy0;
    if (ew > 1.0f && eh > 1.0f && s_base_scale > 0.0f) {
      const float m   = 0.94f;  // petite marge intérieure
      const float sfx = (w - 2.0f * pad) * m / ew;
      const float sfy = (h - 2.0f * pad) * m / eh;
      float sfit = (sfx < sfy) ? sfx : sfy;
      const float floor_s = s_base_scale * g_ez_fit_floor;
      if (sfit < floor_s) sfit = floor_s;
      if (sfit < s_base_scale) scale_limit = sfit;
    }
  }

  // ── Le pantin ─────────────────────────────────────────────────────────────
  ro::DollPlacement pl;
  ro::DollDrawOpts o;
  o.dir            = d;
  o.anim           = anim;
  // 🔴 L'horloge tourne TOUJOURS. Le combo de poses distingue « Marche » de
  // « Marche (animé) » : ça ne concerne que le CORPS. Les coiffes et la cape
  // vivent dans les deux cas, comme en jeu sur un personnage à l'arrêt — une
  // horloge négative les figerait avec lui.
  o.anim_seconds   = now;
  o.freeze_body    = !animate;
  o.center_on_body = true;
  // 🔴 L'échelle se calcule sur l'enveloppe MAXIMALE : toutes les images, les
  // huit orientations. C'est le seul réglage qui tienne les deux bouts.
  //
  // Deux essais l'ont montré. Mesurer la pose affichée fait sauter la taille à
  // la molette : l'arme et le bouclier s'écartent de face et se replient de dos.
  // Mesurer le corps seul règle ça mais rogne tout le reste — et beaucoup de
  // coiffes de costume posent leur sprite À CÔTÉ du personnage (un compagnon,
  // une monture), donc elles débordent bien plus que lui.
  //
  // Sur l'enveloppe de tout, ce qui rentre rentre dans toutes les positions, et
  // l'échelle ne dépend plus de celle qu'on regarde.
  o.fit_span    = true;
  o.scale_limit = scale_limit;
  // Plancher de stature : le personnage ne descend pas sous la moitié de la
  // hauteur du cadre, même s'il faut laisser déborder ce qu'il porte à côté de
  // lui. Un costume-compagnon élargit l'enveloppe au point de le réduire de
  // moitié, sous un grand vide — la fiche montre AVANT TOUT un personnage.
  //
  // C'est le seul bouton : le monter rogne davantage les sprites latéraux, le
  // baisser rend le personnage plus petit quand il en porte un.
  o.min_body_height = (h - 2.0f * pad) * 0.50f;
  o.out_placement  = &pl;
  o.underlay_ctx   = &pass;
  o.underlay = [](void* c, const ro::DollPlacement& p) {
    static_cast<AvPass*>(c)->Behind(p);
  };

  ImDrawList* dl = pass.dl;
  dl->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);
  const bool drawn = ro::DrawDoll(dl, look, x + pad, y + pad, w - 2.0f * pad,
                                  h - 2.0f * pad, o);
  // 🔴 Conditionné au pantin : sans lui il n'y a pas d'ancre, et un effet ancré
  // sur (0,0) atterrirait dans le coin de l'écran.
  if (drawn) {
    pass.Front(pl);
    if (scale_limit <= 0.0f) s_base_scale = pl.scale;  // frame non plafonnée
  }
  dl->PopClipRect();
}

// Exporte le pantin (pose `anim` + direction `dir` courantes) en GIF animé à fond
// TRANSPARENT vers `filepath`.
//
// Tout vient du compositeur maison : `force_frame` joue chaque image nommément,
// `quad_sink` livre les calques au lieu de les peindre, et
// `D3D9_CompositeQuadsRGBA` les rastérise hors écran. Le moteur de capture natif
// n'intervient plus.
//
// Deux passes, et la première n'est pas facultative : l'enveloppe doit couvrir
// TOUTES les images, sinon le personnage se recadrerait d'une image à l'autre et
// le GIF tremblerait.
bool BasicInfo::ExportAvatarGif(int anim, int dir, const char* filepath,
                                     bool show_costume) {
  if (!filepath || Bourgeon::Instance().client().session().aid() == 0) return false;
  const int CW = 256, CH = 340;  // canvas GIF (agrandi pour la qualité ; LZW compressé)
  const float PAD = 14.0f;

  ro::DollLook look;
  rag::OwnActorSprites eq;
  BuildOwnDollLook(show_costume, &look, &eq);

  // Le puits : il accumule les calques d'UNE image, dans l'ordre de dessin.
  struct QuadSink {
    std::vector<D3D9TexQuad> quads;
    static void Add(void* ctx, const ro::DollQuad& q) {
      auto* self = static_cast<QuadSink*>(ctx);
      if (!q.tex) return;
      D3D9TexQuad d;
      d.tex = q.tex;
      for (int c = 0; c < 4; ++c) {
        d.cx[c] = q.corner[c].x;
        d.cy[c] = q.corner[c].y;
      }
      d.u0 = q.uv0.x;  d.v0 = q.uv0.y;
      d.u1 = q.uv1.x;  d.v1 = q.uv1.y;
      self->quads.push_back(d);
    }
  };

  // Échelle 1 et origine (0,0) : les calques sortent en unités .act, le cadrage
  // vient après — une fois l'enveloppe de toutes les images connue.
  ro::DollPlacement unit;
  unit.origin_x = 0.0f;
  unit.origin_y = 0.0f;
  unit.scale    = 1.0f;

  // `DrawDoll` réclame une liste de dessin même quand il n'y peint rien (mesure
  // seule, ou puits de quads). Celle-ci ne reçoit donc jamais rien.
  ImDrawList* dl = ImGui::GetForegroundDrawList();

  ro::DollPlacement pl;
  ro::DollDrawOpts probe;
  probe.dir           = dir & 7;
  probe.anim          = anim;
  probe.anim_seconds  = 0.0f;
  probe.measure_only  = true;
  probe.out_placement = &pl;
  if (!ro::DrawDoll(dl, look, 0.0f, 0.0f, static_cast<float>(CW),
                    static_cast<float>(CH), probe))
    return false;

  int nframes = pl.frame_count;
  if (nframes < 1) nframes = 1;
  if (nframes > 40) nframes = 40;  // garde-fou

  // Passe 1 : les calques de chaque image, et l'enveloppe de leur union.
  std::vector<QuadSink> frames(static_cast<size_t>(nframes));
  float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
  for (int f = 0; f < nframes; ++f) {
    QuadSink& sink = frames[static_cast<size_t>(f)];
    ro::DollDrawOpts o;
    o.dir            = dir & 7;
    o.anim           = anim;
    o.anim_seconds   = 0.0f;
    o.force_frame    = f;
    o.place_override = &unit;
    o.quad_sink      = &QuadSink::Add;
    o.quad_sink_ctx  = &sink;
    ro::DrawDoll(dl, look, 0.0f, 0.0f, static_cast<float>(CW),
                 static_cast<float>(CH), o);
    for (const D3D9TexQuad& q : sink.quads) {
      for (int c = 0; c < 4; ++c) {
        minx = std::min(minx, q.cx[c]);  maxx = std::max(maxx, q.cx[c]);
        miny = std::min(miny, q.cy[c]);  maxy = std::max(maxy, q.cy[c]);
      }
    }
  }
  const float bw = maxx - minx, bh = maxy - miny;
  if (bw <= 1.0f || bh <= 1.0f) return false;
  const float s  = std::min((CW - 2 * PAD) / bw, (CH - 2 * PAD) / bh);
  const float ox = CW * 0.5f - (minx + maxx) * 0.5f * s;  // centré en X
  const float oy = CH - PAD - maxy * s;                   // pieds en bas

  // Passe 2 : appliquer ce cadrage aux calques déjà résolus, puis rastériser.
  //
  // Les coins sont en unités .act (échelle 1, origine 0), donc le cadrage n'est
  // qu'une affinité — inutile de recomposer le pantin une seconde fois.
  std::vector<uint32_t> canvas(static_cast<size_t>(CW) * CH);
  std::vector<std::vector<uint32_t>> gif_frames(static_cast<size_t>(nframes));
  std::vector<const uint32_t*> ptrs(static_cast<size_t>(nframes));
  std::vector<D3D9TexQuad> placed;
  for (int f = 0; f < nframes; ++f) {
    placed = frames[static_cast<size_t>(f)].quads;
    for (D3D9TexQuad& q : placed) {
      for (int c = 0; c < 4; ++c) {
        q.cx[c] = ox + q.cx[c] * s;
        q.cy[c] = oy + q.cy[c] * s;
      }
    }
    if (!D3D9_CompositeQuadsRGBA(placed.data(), static_cast<int>(placed.size()),
                                 CW, CH, canvas.data()))
      return false;
    gif_frames[static_cast<size_t>(f)] = canvas;  // copie de l'image
    ptrs[static_cast<size_t>(f)] = gif_frames[static_cast<size_t>(f)].data();
  }

  // Le .act compte en unités de 25 ms ; le GIF en centisecondes.
  int delay_cs = static_cast<int>(pl.frame_delay * 2.5f + 0.5f);
  if (delay_cs < 2) delay_cs = 2;
  const bool ok = GifWrite(filepath, ptrs.data(), CW, CH, nframes, delay_cs);
  if (!ok) LogError("Avatar GIF échec : {}", filepath);
  return ok;
}

bool BasicInfo::DrawBar(BarId id, long long cur, long long max,
                        const char* label_override) {
  Bar& bar = bars_[id];

  // La grille d'alignement appartient à MoonlightUi ; `ui/` ne doit rien savoir
  // de `features/`, d'où son passage en paramètre plutôt qu'une lecture là-bas.
  const AlignGrid* grid = nullptr;
  if (auto* mui = Bourgeon::Instance().moonlight_ui()) grid = &mui->grid_;

  // Les barres s'aimantent entre elles et partent en bloc sous CTRL. `shown`
  // suit les interrupteurs : une barre éteinte ne sert ni d'aimant ni de
  // compagnon de bloc.
  bool shown[kBarCount];
  for (int j = 0; j < kBarCount; ++j) shown[j] = bars_[j].show;
  ro::HudFrameSiblings siblings;
  siblings.first  = &bars_[0].rect;
  siblings.shown  = shown;
  siblings.count  = kBarCount;
  siblings.stride = static_cast<int>(sizeof(Bar));

  ro::HudFrameOpts opts;
  opts.locked   = locked_;
  opts.rounding = rounding_;
  opts.bg       = bg_color_;
  opts.grid     = grid;
  opts.siblings = siblings;
  opts.index    = id;
  opts.sticky   = sticky_;
  // 5 px et non les 8 par défaut du module : une barre d'info peut être très
  // fine, et c'était déjà la borne d'ici avant la mise en commun.
  opts.min_w = opts.min_h = 5.0f;
  // 🔴 PAS `opts.border` : le module peint son liseré AVANT le contenu, or une
  // jauge pleine et arrondie le recouvrirait. Il est donc tracé plus bas, après
  // le remplissage — c'est l'ordre qu'avait cette barre, et il compte.

  bool changed = false;
  if (ro::BeginHudFrame(kSrc[id].win_id, &bar.rect, opts, &changed)) {
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 sz = ImGui::GetWindowSize();
    const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = rounding_;  // AddRect* borne à la demi-dimension

    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(
        ImVec4(bar.fill[0], bar.fill[1], bar.fill[2], bar.fill[3]));
    const float f = ExpFrac(cur, max);

    // Le FOND est déjà peint par le cadre (`opts.bg`). Reste le remplissage :
    // sauté quand il est quasi vide (sinon un croissant arrondi subsiste à 0 %),
    // et arrondi seulement du côté ARRIÈRE pour que le front de progression
    // reste une arête franche.
    if (vertical_) {
      const float fillpx = (p1.y - p0.y) * f;  // remplit vers le haut
      if (fillpx >= 1.0f) {
        const ImDrawFlags fl = (f >= 0.999f) ? ImDrawFlags_RoundCornersAll
                                             : ImDrawFlags_RoundCornersBottom;
        dl->AddRectFilled(ImVec2(p0.x, p1.y - fillpx), p1, fill, rounding, fl);
      }
    } else {
      const float fillpx = (p1.x - p0.x) * f;  // remplit vers la droite
      if (fillpx >= 1.0f) {
        const ImDrawFlags fl = (f >= 0.999f) ? ImDrawFlags_RoundCornersAll
                                             : ImDrawFlags_RoundCornersLeft;
        dl->AddRectFilled(p0, ImVec2(p0.x + fillpx, p1.y), fill, rounding, fl);
      }
    }
    if (border_) dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 160), rounding);

    if (text_mode_ != 0) {
      // Le libellé vient d'une TABLE : Tr prend donc une variable, et le
      // catalogue porte ces clés sans que l'extracteur statique puisse les voir
      // (même cas que les items de RoCombo). Sans ça la barre de poids restait
      // « Poids » en anglais comme en espagnol. Les termes du jeu — HP, SP,
      // Zeny, Base, Job, Cast — y figurent à l'identique : ils ne changent pas
      // de langue, mais leur entrée les garde hors de la liste des manquants.
      const char* label = i18n::Tr(BasicInfo::kBarLabels[id]);
      char buf[96];
      if (label_override != nullptr)  // barre d'incantation : texte déjà composé
        std::snprintf(buf, sizeof(buf), "%s", label_override);
      else if (kSrc[id].grouped) {  // zeny : libellé + montant en milliers
        char num[24];
        GroupInt(cur, num, sizeof(num));
        std::snprintf(buf, sizeof(buf), "%s %s", label, num);
      } else if (text_mode_ == 1)
        std::snprintf(buf, sizeof(buf), "%s %.2f%%", label, f * 100.0f);
      else if (text_mode_ == 2)
        std::snprintf(buf, sizeof(buf), "%s %lld / %lld", label, cur, max);
      else
        std::snprintf(buf, sizeof(buf), "%s %lld / %lld (%.2f%%)", label, cur,
                      max, f * 100.0f);
      ro::HudBarText(dl, p0, p1, buf);
    }
  }
  // La poignée et les bords allumés sont peints par `EndHudFrame`, PAR-DESSUS
  // le contenu — sans quoi la jauge les recouvrirait.
  ro::EndHudFrame();
  return changed;
}

namespace {
// Bornes de la taille de texte des étiquettes du portrait, appliquées AU DESSIN
// (et pas seulement au slider) : une valeur aberrante venue d'un yaml édité à la
// main ne doit pas produire une police de 300 px ni de taille nulle.
constexpr float kPortTextScaleMin = 0.50f;
constexpr float kPortTextScaleMax = 4.00f;
float ClampTextScale(float s) {
  return s < kPortTextScaleMin ? kPortTextScaleMin
                               : (s > kPortTextScaleMax ? kPortTextScaleMax : s);
}

// Returns the current text content for a portrait element (empty string for the
// head element, which draws a sprite placeholder instead of text).
void PortraitText(int id, char* out, size_t n) {
  switch (id) {
    case BasicInfo::kPortName: {
      const std::string nm = Bourgeon::Instance().client().session().GetCharName();
      std::snprintf(out, n, "%s", nm.empty() ? "?" : nm.c_str());
      break;
    }
    case BasicInfo::kPortClass: {
      const char* cls = rag::OwnClassName();
      std::snprintf(out, n, "%s", (cls && cls[0]) ? cls : "");
      break;
    }
    case BasicInfo::kPortLevel:
      // base/job merged, simply "%d/%d" as requested.
      std::snprintf(out, n, "%d/%d", RDi(rag::kBaseLevelAddr), RDi(rag::kJobLevelAddr));
      break;
    default:
      out[0] = '\0';
      break;
  }
}
}  // namespace

// Connected group of SHOWN portrait elements whose frames touch/overlap `seed`
// (transitive closure, 2px tolerance so edge-adjacent frames count). Returns a
// PortId bitmask. Used by CTRL block-move to drag a cluster of frames as one.
// Une étiquette du portrait, dans son propre cadre libre : fond et opacité à
// elle, arrondi à elle, couleur de texte à elle. Le déplacement, le
// redimensionnement par les bords, l'aimantation sur la grille, le bloc CTRL et
// le clamp écran sont ceux de `ro::BeginHudFrame` — les mêmes gestes que les
// barres et que le HUD de cible, parce qu'un HUD qui se manipule autrement
// qu'un autre HUD du même jeu est un HUD raté.
// Rend true si la géométrie a changé cette frame.
bool BasicInfo::DrawPortraitElem(PortId id) {
  PortraitElem& e = ports_[id];

  const AlignGrid* grid = nullptr;
  if (auto* mui = Bourgeon::Instance().moonlight_ui()) grid = &mui->grid_;

  // Les étiquettes du portrait partent en bloc sous CTRL quand leurs cadres se
  // touchent. ⚠ `sticky` ÉTEINT : elles n'ont jamais eu l'aimantation entre
  // voisines, et le portrait n'a pas d'interrupteur pour la régler — les barres,
  // elles, ont le leur. La migration ne change rien à ce que le joueur ressent.
  bool shown[kPortCount];
  for (int j = 0; j < kPortCount; ++j) shown[j] = ports_[j].show;
  ro::HudFrameSiblings siblings;
  siblings.first  = &ports_[0].rect;
  siblings.shown  = shown;
  siblings.count  = kPortCount;
  siblings.stride = static_cast<int>(sizeof(PortraitElem));

  ro::HudFrameOpts opts;
  opts.locked   = portrait_locked_;
  opts.border   = portrait_border_;
  opts.rounding = e.rounding;
  opts.bg       = e.bg;
  opts.grid     = grid;
  opts.siblings = siblings;
  opts.index    = static_cast<int>(id);
  opts.sticky   = false;

  char id_buf[24];
  std::snprintf(id_buf, sizeof(id_buf), "###Port%d", static_cast<int>(id));

  bool changed = false;
  if (ro::BeginHudFrame(id_buf, &e.rect, opts, &changed)) {
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 sz = ImGui::GetWindowSize();
    const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fg = ImGui::ColorConvertFloat4ToU32(
        ImVec4(e.fg[0], e.fg[1], e.fg[2], e.fg[3]));
    // Le fond et le liseré sont peints par le cadre (`opts.bg` / `opts.border`),
    // sous ce qui suit.

    if (id == kPortHead) {
      // Regenerated head sprite: composite the captured actor layers, fitted to
      // the frame width and top-anchored (so the head fills the top) + clipped.
      if (portrait_head_sprite_) {
        // ── Le portrait passe par le COMPOSEUR ──────────────────────────────
        //
        // Deux temps, imposés par ImGui : on mesure d'abord (rien n'est dessiné,
        // `measure_only`), on décide du cadrage, puis on dessine avec ce
        // placement imposé. Impossible de faire autrement — une fois les quads
        // émis dans la liste de dessin, on ne peut plus revenir les recadrer.
        //
        // Le pantin est toujours composé ENTIER : la tête s'ancre au corps, et
        // les coiffes à la tête. « Tête seule » est un choix de CADRAGE, pas de
        // composition — on zoome sur la boîte de la tête, le reste sort du clip.
        ro::DollLook look;
        rag::OwnActorSprites eq;
        BuildOwnDollLook(/*show_costume=*/true, &look, &eq);
        // La cape encombre un portrait serré, et « tête seule » la rend absurde.
        if (!portrait_show_garment_ || portrait_head_only_) look.garment = 0;

        const float pnow = portrait_animate_
                               ? static_cast<float>(ImGui::GetTime())
                               : -1.0f;
        ro::DollPlacement pm;
        ro::DollDrawOpts mo;
        mo.dir           = portrait_dir_;
        mo.anim          = portrait_anim_;
        mo.anim_seconds  = pnow;
        mo.measure_only  = true;
        mo.out_placement = &pm;
        const bool measured =
            ro::DrawDoll(dl, look, p0.x, p0.y, sz.x, sz.y, mo);

        // Boîtes rendues par la mesure, en unités .act.
        const float hx0 = pm.head_x0, hy0 = pm.head_y0;
        const float hx1 = pm.head_x1, hy1 = pm.head_y1;
        const float bx0 = pm.body_x0, by0 = pm.body_y0;
        const float bx1 = pm.body_x1, by1 = pm.body_y1;
        const bool has_head = pm.head_valid, has_body = pm.body_valid;
        const float minx = has_head ? std::min(hx0, bx0) : bx0;
        const float maxx = has_head ? std::max(hx1, bx1) : bx1;
        const float miny = has_head ? std::min(hy0, by0) : by0;
        const float maxy = has_head ? std::max(hy1, by1) : by1;
        // « Tête seule » cadre sur la tête ; sinon sur l'ensemble.
        const float bw = (portrait_head_only_ && has_head) ? (hx1 - hx0)
                                                           : (maxx - minx);
        const float bh = (portrait_head_only_ && has_head) ? (hy1 - hy0)
                                                           : (maxy - miny);
        if (measured && bw > 1.0f && bh > 1.0f) {
          // Anchor the zoom on the bbox CENTRE so zooming in/out stays centred in
          // the frame (offx/offy = where that centre lands, 0.5/0.5 = middle) and
          // clip. Zoom + focus offsets are live-tunable sliders.
          const float availW = sz.x - 4.0f;
          const float s = (availW / bw) * portrait_head_zoom_;
          // Focus point: head-only -> the head centre; head+body -> the MEDIAN of
          // the head centre and the body centre (a flattering upper-body framing).
          float focusX = (minx + maxx) * 0.5f;
          float focusY = (miny + maxy) * 0.5f;
          if (!portrait_head_only_ && has_head && has_body) {
            focusX = (hx0 + hx1 + bx0 + bx1) * 0.25f;  // (headCx + bodyCx)/2
            focusY = (hy0 + hy1 + by0 + by1) * 0.25f;  // (headCy + bodyCy)/2
          } else if (has_head) {
            focusX = (hx0 + hx1) * 0.5f;               // head centre
            focusY = (hy0 + hy1) * 0.5f;
          }
          // Map the focus to the FRAME centre so the zoom always pivots centred;
          // offx/offy are a delta FROM centre (0 = centred, ±0.5 = ±half a frame).
          // This keeps zoom in/out stable regardless of the offsets.
          const float ox = p0.x + sz.x * (0.5f + portrait_head_offx_) - focusX * s;
          const float oy = p0.y + sz.y * (0.5f + portrait_head_offy_) - focusY * s;
          dl->PushClipRect(ImVec2(p0.x + 1.0f, p0.y + 1.0f),
                           ImVec2(p1.x - 1.0f, p1.y - 1.0f), true);
          // Hat effects : MÊME traitement que le doll. `ox`/`oy` sont ici aussi l'ORIGINE de
          // l'acteur en coordonnées écran et `s` son échelle, donc l'appel est identique — d'où le
          // z-order en deux passes autour des couches du sprite (bit 0x8 des flags natifs).
          // `with_preview` = false : le portrait montre ce que le personnage PORTE, jamais l'effet
          // survolé ailleurs (même règle que le doll).
          DrawEzCapTris(dl, ox, oy, s, /*before=*/true, /*with_preview=*/false);
          // Le même pantin, redessiné au placement qu'on vient de calculer. Le
          // rectangle n'est plus qu'un cadre de dessin : c'est le clip ci-dessus
          // qui coupe ce qui dépasse de la vignette.
          ro::DollPlacement forced;
          forced.origin_x = ox;
          forced.origin_y = oy;
          forced.scale    = s;
          ro::DollDrawOpts po;
          po.dir            = portrait_dir_;
          po.anim           = portrait_anim_;
          po.anim_seconds   = pnow;
          po.place_override = &forced;
          ro::DrawDoll(dl, look, p0.x, p0.y, sz.x, sz.y, po);
          DrawEzCapTris(dl, ox, oy, s, /*before=*/false, /*with_preview=*/false);
          dl->PopClipRect();
        }
      } else {
        const char* ph = " ";
        const ImVec2 ts = ImGui::CalcTextSize(ph);
        dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f),
                    IM_COL32(255, 255, 255, 70), ph);
      }
    } else {
      char buf[96];
      PortraitText(id, buf, sizeof(buf));
      // Taille réglable par étiquette. `HudCenteredText` mesure À LA TAILLE DE
      // DESSIN — mesurer à une taille et peindre à une autre décale le centrage —
      // et fait suivre l'ombre, qui disparaîtrait sous un gros texte.
      ro::HudCenteredText(dl, p0, p1, buf, fg,
                          ImGui::GetFontSize() * ClampTextScale(e.text_scale));
    }
  }
  // La poignée et les bords allumés sont peints par `EndHudFrame`, PAR-DESSUS le
  // contenu : le portrait les recouvrirait sinon.
  ro::EndHudFrame();
  return changed;
}

// Draws every shown portrait element (each its own movable frame), persisting
// the layout once on drag-end.
void BasicInfo::DrawPortrait() {
  if (!portrait_visible_) return;
  // Session globals are only populated once a character is in the world.
  if (Bourgeon::Instance().client().session().aid() == 0) return;

  bool changed = false;
  for (int i = 0; i < kPortCount; ++i) {
    if (!ports_[i].show) continue;
    changed |= DrawPortraitElem(static_cast<PortId>(i));
  }
  if (changed) portrait_drag_pending_ = true;
  if (portrait_drag_pending_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    portrait_drag_pending_ = false;
    geometry_dirty_ = true;  // drained by MoonlightUi (saves the yaml)
  }
}

// ── Section « BasicInfo » du panneau Moonlight ──────────────────────────
// Déplacée depuis moonlight_ui/panel_interface.cc : ces widgets ne pilotent
// que l'état de CE plugin. MoonlightUi ne garde que l'appel et la décision
// de sauvegarder. Rend true si un réglage a changé.
bool BasicInfo::DrawSettings() {
  bool changed = false;
  PushStyleCompact();

  changed |= ro::RoCheckbox(i18n::Tr("Masquer la fenêtre Basic Info d'origine"), &portrait_hide_basic_info_);
  SameLine(); HelpMarker(i18n::Tr("Masque la fenêtre native \"Basic Info\"."));

  SeparatorText(i18n::Tr("Barres d'info"));
  changed |= ro::RoCheckbox(i18n::Tr("Afficher les barres"), &bars_visible_);

  // Hors du BeginDisabled : masquer les jauges du client ne dépend pas de nos
  // barres à nous. Ne rien afficher du tout est un choix qui se défend.
  changed |= ro::RoCheckbox(i18n::Tr("Masquer les barres HP/SP sous mon personnage"),
                            &hide_own_pc_gage_);
  SameLine(); HelpMarker(
      i18n::Tr("Masque les deux petites jauges HP/SP que le client dessine sous "
      "les pieds de TON personnage dans le monde.\n"
      "Celles des autres joueurs et des monstres ne bougent pas."));

  ImGui::BeginDisabled(!bars_visible_);
  Indent();
    for (int i = 0; i < BasicInfo::kBarCount; ++i) {
      if (i) SameLine();
      // Tr, et sur la VARIABLE : ces libellés viennent d'une table, invisible à
      // l'extracteur statique. C'est ce qui manquait ici — « Poids » restait
      // français en EN/ES alors que sa traduction était au catalogue.
      changed |= ro::RoCheckbox(i18n::Tr(BasicInfo::kBarLabels[i]), &bars_[i].show);
    }
    SameLine(); HelpMarker(i18n::Tr("Affiche/cache chaque barre indépendamment."));
  Unindent();

  changed |= ro::RoCheckbox(i18n::Tr("Verrouiller les barres"), &locked_);
  SameLine(); HelpMarker(
      i18n::Tr("Verrouillée : les barres ne bougent plus et laissent passer les clics au jeu.\n"
      "Déverrouillée : glissez-les pour les déplacer, tirez le coin pour redimensionner."));

  changed |= ro::RoCheckbox(i18n::Tr("Aimanter les barres (snap)"), &sticky_);
  SameLine(); HelpMarker(
      i18n::Tr("Quand tu glisses une barre près d'une autre, ses bords s'alignent "
      "et se collent automatiquement (~10px).\nÉloigne-la pour la "
      "détacher. Les barres restent indépendantes."));

  changed |= ro::RoCheckbox(i18n::Tr("Vertical"), &vertical_);
  SameLine(); HelpMarker(
      i18n::Tr("Remplissage vertical des barres. \n"
      "Décoche pour les barres horizontales."));

  changed |= ro::RoCheckbox(i18n::Tr("Bordure des barres"), &border_);
  SameLine(); HelpMarker(
      i18n::Tr("Trait sombre 1px autour de chaque barre (HP/SP/EXP...). \n"
      "Décoche pour des barres sans contour."));

  const char* modes[] = {"Aucun", "Pourcentage", "Valeurs", "Les deux"};
  changed |= ro::RoCombo(i18n::Tr("Texte des barres"), &text_mode_, modes, IM_ARRAYSIZE(modes));
  SameLine(); HelpMarker(
      i18n::Tr("Ce qui est écrit sur les barres : rien, le pourcentage, les "
      "valeurs brutes (courant / max) ou les deux."));
  changed |= WheelSliderFloat(i18n::Tr("Arrondi"), &rounding_, 0.0f, 16.0f);
  SameLine(); HelpMarker(i18n::Tr("Arrondi des coins des barres."));

  for (int i = 0; i < BasicInfo::kBarCount; ++i) {
    char lbl[32];
    // Le gabarit ET son trou : « Color %s » avec « Poids » dedans ne serait
    // traduit qu'à moitié.
    std::snprintf(lbl, sizeof(lbl), i18n::Tr("Couleur %s"),
                  i18n::Tr(BasicInfo::kBarLabels[i]));
    changed |= ColorEdit4WithAlphaBar(lbl, bars_[i].fill);
  }
  changed |= ColorEdit4WithAlphaBar(i18n::Tr("Fond / Opacité"), bg_color_);

  TextUnformatted(i18n::Tr("Tailles rapides de barres (toutes) :"));
  // Ce n'est PAS un préréglage (le mot désigne déjà trois autres familles dans
  // ce projet) : c'est le bouton qui applique une taille à TOUTES les barres.
  auto bar_size_button = [&](const char* label, int width_px, int height_px) {
    SameLine();
    if (ro::RoButton(label)) {
      for (int j = 0; j < BasicInfo::kBarCount; ++j) {
        bars_[j].rect.w = width_px;
        bars_[j].rect.h = height_px;
      }
      changed = true;
    }
  };
  bar_size_button("XS", 200, 9);
  bar_size_button("S", 400, 16);
  bar_size_button("M", 600, 22);
  bar_size_button("L", 800, 30);
  ImGui::EndDisabled();

  SeparatorText(i18n::Tr("Portrait personnage"));
  changed |= ro::RoCheckbox(i18n::Tr("Afficher le portrait et les étiquettes"), &portrait_visible_);
  SameLine(); HelpMarker(
      i18n::Tr("Portrait de statut : la tête du personnage, le pseudo, la classe "
      "et le niveau sont des éléments INDÉPENDANTS — chacun déplaçable, "
      "redimensionnable, avec sa couleur/opacité de fond, son arrondi et sa "
      "taille de texte."));

  ImGui::BeginDisabled(!portrait_visible_);

  changed |= ro::RoCheckbox(i18n::Tr("Verrouiller le portrait"), &portrait_locked_);
  Tooltip(i18n::Tr("Si les éléments sont déverrouillés et en contact les uns avec les autres, ils sont déplaçables en maintenant Ctrl."));
  SameLine(); HelpMarker(
      i18n::Tr("Verrouillé : les éléments ne bougent plus et laissent passer les clics au jeu.\n"
      "Déverrouillé : glisse pour déplacer, tire un bord/coin pour redimensionner (aimantage à la grille d'alignement)."));

  changed |= ro::RoCheckbox(i18n::Tr("Tête seule (sans le corps)"), &portrait_head_only_);
  SameLine(); HelpMarker(
      i18n::Tr("Ne génère que la tête (visage/cheveux/coiffes) et retire le corps.\n"
      "Décoche pour le personnage entier."));

  changed |= ro::RoCheckbox(i18n::Tr("Cape / garment"), &portrait_show_garment_);
  SameLine(); HelpMarker(
      i18n::Tr("Affiche la cape/garment équipée (seulement en mode corps "
      "entier — décoche \"Tête seule\" pour la voir)."));

  changed |= WheelSliderFloat(i18n::Tr("Zoom"), &portrait_head_zoom_, 0.10f, 2.0f);
  SameLine(); HelpMarker(i18n::Tr("Ajuster avec le zoom."));

  changed |= WheelSliderFloat(i18n::Tr("Décalage horiz."), &portrait_head_offx_, -1.5f, 1.5f);
  SameLine(); HelpMarker(
      i18n::Tr("Décale le portrait horizontalement (0 = centré).\n"
      "Sert à cadrer la tête/le corps ; le zoom reste centré."));

  changed |= WheelSliderFloat(i18n::Tr("Décalage vert."), &portrait_head_offy_, -1.5f, 1.5f);
  SameLine(); HelpMarker(
      i18n::Tr("Décale le portrait verticalement (0 = centré).\n"
      "Optionnel — le zoom reste centré ; laisse à 0 si tu n'en as pas besoin."));

  static const char* kLabelsAnim[] = { "Repos", "Marche", "Assis", "Ramasser", "Combat", "Attaque", "Touché", "Gelé", "Mort" };
  changed |= ro::RoCombo(i18n::Tr("Animation"), &portrait_anim_, kLabelsAnim, IM_ARRAYSIZE(kLabelsAnim));
  SameLine(); HelpMarker(
      i18n::Tr("Pose animée du portrait (Combat = posture prête au combat).\n"
      "Le nombre d'images de l'animation s'ajuste automatiquement."));

  static const char* kLabelsDir[] = { "Face", "Profil-Gauche", "Gauche", "Arrière-Gauche", "Dos", "Arrière-Droite", "Droite", "Profil-Droite" };
  changed |= ro::RoCombo(i18n::Tr("Direction"), &portrait_dir_, kLabelsDir, IM_ARRAYSIZE(kLabelsDir));
  SameLine(); HelpMarker(
      i18n::Tr("Oriente le portrait. 0 = face. Essaie les valeurs pour trouver "
      "l'angle voulu (le rendu se met à jour en direct)."));

  changed |= ro::RoCheckbox(i18n::Tr("Animer"), &portrait_animate_);
  SameLine(); HelpMarker(
      i18n::Tr("Joue les images de l'animation (ex. le balayage de la posture "
      "Combat). Décoche pour figer une pose calme (image 0)."));

  SeparatorText(i18n::Tr("Couleurs, arrondis et taille de texte du portrait et des étiquettes"));
  changed |= ro::RoCheckbox(i18n::Tr("Bordure"), &portrait_border_);
  SameLine(); HelpMarker(i18n::Tr("Trait 1px autour du cadre et des étiquettes."));

  // Per-element config: show / background colour+opacity / rounding /
  // text colour / text size.  Each element is independent.
  for (int i = 0; i < BasicInfo::kPortCount; ++i) {
    auto& e = ports_[i];
    ImGui::PushID(i);
    changed |= ro::RoCheckbox(i18n::Tr(BasicInfo::kPortLabels[i]), &e.show);
    Indent();
    changed |= ColorEdit4WithAlphaBar(i18n::Tr("Fond / Opacité"), e.bg);
    if (i != BasicInfo::kPortHead) {
      SameLine();
      changed |= ColorEdit4WithAlphaBar(i18n::Tr("Texte"), e.fg);
    }
    changed |= WheelSliderFloat(i18n::Tr("Arrondi"), &e.rounding, 0.0f, 16.0f, "%.0f", 1.0f);
    if (i != BasicInfo::kPortHead) {
      changed |= WheelSliderFloat(i18n::Tr("Taille du texte"), &e.text_scale,
                                  kPortTextScaleMin, kPortTextScaleMax);
      SameLine(); HelpMarker(
          i18n::Tr("Taille du texte de cette étiquette (1.00 = taille de l'interface).\n"
          "Le texte reste centré et coupé au cadre : agrandis le cadre s'il "
          "déborde."));
    }
    Unindent();
    ImGui::PopID();
  }
  PopStyleCompact();

  ImGui::EndDisabled(); // portrait_visible_
  return changed;
}

void BasicInfo::OnRenderUI() {
  if (!in_game_) return;
  // The alignment grid is drawn by MoonlightUi (shared overlay), not here.

  DrawPortrait();  // independent of the EXP-bar master toggle below

  if (!bars_visible_) return;
  // Globals are only populated once a character is in the world.
  if (Bourgeon::Instance().client().session().aid() == 0) return;

  bool changed = false;
  for (int i = 0; i < kBarCount; ++i) {
    if (!bars_[i].show) continue;
    if (i == kCast) {
      // Barre d'incantation : pas de globale à lire, et surtout elle n'existe
      // que le temps du sort. Hors incantation on ne dessine RIEN — une jauge
      // vide en permanence prendrait la place sans rien dire.
      const CastBar* cb = Bourgeon::Instance().cast_bar();
      if (cb == nullptr || !cb->own_cast().active) continue;
      char label[96];
      cb->OwnCastLabel(label, sizeof(label));
      changed |= DrawBar(kCast, cb->own_cast().elapsed_ms,
                         cb->own_cast().total_ms, label);
      continue;
    }
    const long long cur = RDval(kSrc[i].cur, kSrc[i].wide);
    const long long max = kSrc[i].max_const ? kSrc[i].max_const
                                            : RDval(kSrc[i].max, kSrc[i].wide);
    changed |= DrawBar(static_cast<BarId>(i), cur, max);
  }


  if (changed) drag_pending_ = true;
  // Persist exactly once, when the user releases the mouse after moving/resizing.
  if (drag_pending_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    drag_pending_   = false;
    geometry_dirty_ = true;  // drained by MoonlightUi (saves the YAML)
  }
}

namespace {
// ── 🔴🔴 La barre HP/SP sous le personnage est `UIPlayerGage` ────────────
//
// ⏱ TROIS correctifs faux avant celui-ci, et chacun reposait sur une déduction
// qui se tenait. Ce qui a fini par trancher, c'est la MESURE : lecture du
// processus vivant, parcours de la liste de fenêtres du gestionnaire, relevé des
// positions. Ce que la mesure dit, et que la lecture statique ne disait pas :
//
//   · l'acteur du joueur (`actorMgr+0x2C`) ne porte AUCUNE jauge — `+0x300`,
//     `+0x424`, `+0x428`, `+0x488` sont tous nuls. Toutes les sondes de slots
//     visaient donc du vide ;
//   · la barre est une FENÊTRE AUTONOME de la liste du gestionnaire, mesurée à
//     60×9 (deux barres empilées) et posée exactement sous les pieds
//     (`x_perso - 30`) ;
//   · sa classe n'est ni `UIPcGage` (`0x0102BCA0`) ni `UIMonsterGage`
//     (`0x0102BE50`) mais **`UIPlayerGage`** — vtable **`0x0102BD78`**, ctor
//     `0x00836530`. D'où l'échec du détour précédent : `UIPcGage_Paint` n'est
//     JAMAIS appelée pour elle (point d'arrêt posé en jeu, jamais atteint).
//
// 🔴 On la trouve donc par sa VTABLE, pas par un offset d'acteur : c'est le
// seul critère que la mesure valide, et il ne dépend d'aucune classe d'acteur.
// La vtable n'a que deux xrefs dans tout le binaire (ctor + dtor) : cette classe
// ne sert qu'à ça.
//
// ⚠ Et l'écriture de `+0x28` TIENT ici — vérifié en direct : le drapeau est resté
// à 0 sur plusieurs lectures. Rien ne le réaffirme, contrairement aux jauges
// d'ACTEUR (`+0x300`), que `CActorSprite_UpdateAttachedSprite` repose à chaque
// frame. C'est pourquoi un simple battement suffit ici, sans détour — et c'est
// exactement le genre de détail qu'on ne peut pas deviner.
//
// 🔴 On la MASQUE (drapeau `+0x28`), on ne la détruit pas : elle appartient au
// client, qui la crée et la nourrit.
constexpr uintptr_t kUIPlayerGageVtable = 0x0102bd78;

// Pose la visibilité de toutes les fenêtres `UIPlayerGage`. Rend true si au
// moins une a été touchée — hors jeu, ou avant sa création, il n'y a rien.
//
// ⚠ SEH ⇒ aucun objet C++ ici.
bool SetPlayerGageVisible(bool visible) {
  bool touched = false;
  __try {
    void* sentinel = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(uiwnd::Mgr()) + uiwnd::kOffWindowList);
    if (!sentinel) return false;
    void* node = *reinterpret_cast<void**>(sentinel);
    // Garde-fou de parcours : une liste corrompue ne doit pas faire tourner le
    // client en rond. Elle en compte une douzaine en pratique.
    for (int guard = 0; node && node != sentinel && guard < 256; ++guard) {
      void* wnd = *(reinterpret_cast<void**>(node) + 2);
      if (wnd && *reinterpret_cast<uintptr_t*>(wnd) == kUIPlayerGageVtable) {
        uiwnd::SetVisible(wnd, visible);
        touched = true;
      }
      node = *reinterpret_cast<void**>(node);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return touched;
  }
  return touched;
}

// ── Hide native Basic Info PRE-RENDER via its msg-0x22 handler ───────────────
// UIBasicInfoWnd (id 0) vtable 0x0103e35c. Its OnMsg is vtable+0x94; MakeWindow's
// id-0 case calls it with msg 0x22 (layout-restore) DURING creation, before the
// first frame — so hiding there avoids the login flicker of the OnTick approach.
//
// We hide by moving it OFF-SCREEN via a raw +0x1c/+0x20 write. This is what kills
// the native dock/snap "ghost" (a hidden-in-place window is still a snap target;
// an off-screen one is not — live-verified). Crucially it does NOT corrupt the
// saved position: BASICINFOWNDINFO.X/Y persist from a SEPARATE store in the window
// manager (mgr+0x514), which a raw field write never touches — LIVE-VERIFIED via
// x32dbg: live pos -> -10000 while mgr+0x514 stayed (0,0). The real pos is captured
// before the override so it can be restored if the option is turned off.
// Same handler ABI as Equip/Status (ret 0x18, this in ECX). Vtable-slot patch = safe.
constexpr uintptr_t kBIMsgSlot    = 0x0103e35c + 0x94;  // vtable+0x94 OnMsg slot
constexpr int       kBIWinX       = 0x1c;
constexpr int       kBIWinY       = 0x20;
constexpr int       kBIOffScreen  = -10000;
constexpr int       kBIMsgRestore = 0x22;
constexpr int       kBIUnset      = static_cast<int>(0x80000000);  // "no saved pos yet"
using BIMsg_t = int (__fastcall*)(void*, void*, int, int, int, int, int, int);  // ret 0x18
BIMsg_t g_bi_orig_msg = nullptr;
bool    g_bi_hide     = false;               // synced from portrait_hide_basic_info_
int     g_bi_saved_x  = kBIUnset, g_bi_saved_y = kBIUnset;  // real pos, captured once

// Move Basic Info off-screen via a raw field write (NOT SetPos — a raw write does
// not sync back to the persisted mgr+0x514, so the save stays intact). Captures the
// real on-screen position the first time so it can be restored.
inline void BIPinOffscreen(void* w) {
  int* px = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(w) + kBIWinX);
  int* py = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(w) + kBIWinY);
  if (*px > kBIOffScreen + 5000) { g_bi_saved_x = *px; g_bi_saved_y = *py; }
  *px = kBIOffScreen;
  *py = kBIOffScreen;
}

// Noms des paramètres alignés sur uiwnd::OnMsg : le natif prend SIX entiers dont
// `msg` est le DEUXIÈME ; le premier (`arg0`) vaut 0 sur tous les sites d'appel
// connus et son rôle n'est pas établi. On relaie tout tel quel.
int __fastcall BIMsgHook(void* self, void* edx, int arg0, int msg, int p2, int p3,
                         int p4, int p5) {
  __try {
    const int r = g_bi_orig_msg(self, edx, arg0, msg, p2, p3, p4, p5);
    if (msg == kBIMsgRestore && g_bi_hide) BIPinOffscreen(self);  // pre-render off-screen
    return r;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// ── Barres d'EXP de la Basic Info NATIVE : plafonds de niveau client vs serveur ──
//
// `UIBasicInfoWnd_LayoutChildren` (0x0095dfb0) ne « cache » pas les jauges d'exp : il les
// POUSSE hors du cadre (x = -200) dès qu'il croit le personnage au niveau max —
//   base : `Job_GetMaxBaseLevel(job) == g_Own_BaseLevel`   (égalité STRICTE)
//   job  : `g_Own_JobLevel >= Job_GetMaxJobLevel(job)`
// Les deux plafonds sortent de `MaxLevelTable` d'ExternalSettings_kr.lub, lue une fois au
// boot (globals g_ES_Max* 0x01602288+). Sur ce client ils valent 99 (base) et 70 (2e classe
// transcendante) alors que Moonlight monte à 999 / 80 (db/import/job_exp.yml). D'où le
// symptôme : la barre de base disparaît PILE au niveau 99 et revient au 100 (l'égalité
// stricte redevient fausse), et la barre de job disparaît dès le job 70 — définitivement,
// celle-là, puisque son test est un `>=`.
//
// 🔴 Le lub ne PEUT PAS porter ce correctif : ses plafonds sont par CATÉGORIE de job (les
// ~9 branches codées en dur des deux getters : novice / 2e / 3e / 4e / transcendant /
// Doram…), alors que Moonlight les définit par GROUPE DE JOBS dans db/import/job_exp.yml,
// bien plus fin — `MaxJobLevel` y vaut 10, 50, 52, 60, 70, 80, 99 ou 111 selon la classe.
// Aucune valeur unique par catégorie ne serait juste. On détourne donc les deux GETTERS,
// appelés QUE par la Basic Info (`Job_GetMaxBaseLevel` : LayoutChildren +
// DrawCollapsed_4thJob ; `Job_GetMaxJobLevel` : LayoutChildren).
// Effet de bord évité au passage : `g_ES_MaxBaseLevel` sert AUSSI, en égalité stricte, à
// `CActorSprite_ApplyLevelJobAura` (0x00c41950) pour l'aura de niveau 99 — le relever
// l'aurait déplacée au niveau 999. Sans conséquence sur Moonlight (les auras de niveau n'y
// sont pas affichées) ; le détour laisse ce chemin intact de toute façon.
//
// Le vrai plafond, seul le SERVEUR le connaît, et il le dit déjà : `pc_nextbaseexp` /
// `pc_nextjobexp` (moonlight src/map/pc.cpp) renvoient une SENTINELLE au niveau max —
// MAX_LEVEL_BASE_EXP = 99 999 999 / MAX_LEVEL_JOB_EXP = 999 999 999 — au lieu du coût du
// palier suivant. Aucune des deux n'est un palier réel de la table Moonlight (vérifié dans
// db/import/job_exp.yml : les seules occurrences de 999999999 sont le dernier palier des
// groupes Novice), donc le test est sans faux positif. On garde ainsi l'INTENTION du natif
// — barre masquée au VRAI niveau max — au lieu de forcer bêtement les barres visibles.
constexpr uintptr_t kJobGetMaxBaseLevel = 0x00d99ca0;  // __stdcall(jobId)
constexpr uintptr_t kJobGetMaxJobLevel  = 0x00d99d30;  // __stdcall(jobId)
constexpr long long kSrvMaxBaseExp  = 99999999LL;   // MAX_LEVEL_BASE_EXP (moonlight const.hpp)
constexpr long long kSrvMaxJobExp   = 999999999LL;  // MAX_LEVEL_JOB_EXP  (idem)
constexpr int       kBILevelUnreached = 0x7fffffff;  // plafond qu'aucun niveau n'atteint
using JobMaxLevelFn_t = int(__stdcall*)(int);
JobMaxLevelFn_t g_bi_orig_max_base_lv = nullptr;
JobMaxLevelFn_t g_bi_orig_max_job_lv  = nullptr;

// Le personnage est-il au niveau max du SERVEUR ? (sentinelle d'exp, cf. bloc ci-dessus)
inline bool BIAtServerMaxBase() {
  return *reinterpret_cast<const long long*>(rag::kOwnBaseExpNextAddr) == kSrvMaxBaseExp;
}
inline bool BIAtServerMaxJob() {
  return *reinterpret_cast<const long long*>(rag::kOwnJobExpNextAddr) == kSrvMaxJobExp;
}

// Au vrai max on rend le niveau COURANT : ça rejoue exactement la branche « masquer » du
// natif (base `==`, job `>=`). Sinon un plafond hors d'atteinte => la barre reste en place.
// Les trois sites d'appel passent toujours la classe du propre joueur, d'où la lecture des
// globals de session ; hors jeu (niveau nul) on rend la main au natif, faute de savoir.
int __stdcall BIMaxBaseLevelHook(int job_id) {
  const int lv = *reinterpret_cast<const int*>(rag::kBaseLevelAddr);
  if (lv <= 0 && g_bi_orig_max_base_lv) return g_bi_orig_max_base_lv(job_id);
  return BIAtServerMaxBase() ? lv : kBILevelUnreached;
}
int __stdcall BIMaxJobLevelHook(int job_id) {
  const int lv = *reinterpret_cast<const int*>(rag::kJobLevelAddr);
  if (lv <= 0 && g_bi_orig_max_job_lv) return g_bi_orig_max_job_lv(job_id);
  return BIAtServerMaxJob() ? lv : kBILevelUnreached;
}

// Filet de relayout. `LayoutChildren` n'est rejoué qu'à la création, au repli/dépli et au
// changement de classe (sub_D70D60) : atteindre le niveau max EN COURS de session ne
// repositionne rien. On le rejoue donc nous-mêmes quand l'état « au max » bascule, avec le
// même appel que le natif : LayoutChildren(fenêtre, hauteur courante).
constexpr uintptr_t kBILayoutChildren = 0x0095dfb0;  // __thiscall(wnd, height)
constexpr int       kBIWinHeight      = 0x18;
using BILayoutFn_t = void(__thiscall*)(void*, int);
bool g_bi_cap_known = false, g_bi_base_capped = false, g_bi_job_capped = false;
}  // namespace

BasicInfo::BasicInfo() {
  // Install the msg-0x22 hide hook at DLL load (before any Basic Info is created),
  // so the very first HUD creation at login is caught pre-render (no flicker).
  void* cur = *reinterpret_cast<void**>(kBIMsgSlot);
  if (cur && cur != reinterpret_cast<void*>(&BIMsgHook)) {
    g_bi_orig_msg = reinterpret_cast<BIMsg_t>(cur);
    mem::PatchValue<void*>(kBIMsgSlot, reinterpret_cast<void*>(&BIMsgHook));
  }
  // Plafonds de niveau des jauges d'exp natives : le client les tient d'ExternalSettings
  // (99 / 70) alors que Moonlight monte plus haut, ce qui escamote les barres au niveau 99
  // et dès le job 70. Détour des deux getters — voir le bloc de commentaire au-dessus de
  // BIMaxBaseLevelHook. Posé au chargement de la DLL : les adresses sont statiques et le
  // HUD n'existe pas encore, donc le tout premier layout est déjà correct.
  {
    auto& hm = hooking::HookManager::Instance();
    g_bi_orig_max_base_lv = reinterpret_cast<JobMaxLevelFn_t>(
        hm.SetHook(hooking::HookType::kJmpHook, reinterpret_cast<uint8_t*>(kJobGetMaxBaseLevel),
                   reinterpret_cast<uint8_t*>(&BIMaxBaseLevelHook)));
    g_bi_orig_max_job_lv = reinterpret_cast<JobMaxLevelFn_t>(
        hm.SetHook(hooking::HookType::kJmpHook, reinterpret_cast<uint8_t*>(kJobGetMaxJobLevel),
                   reinterpret_cast<uint8_t*>(&BIMaxJobLevelHook)));
  }
  // ZC_EQUIPMENT_EFFECT 0x0A3B (VAR) : [len:2][aid:4][status:1]{effectId:2}. On
  // OBSERVE (paquet natif intact) pour suivre les hat effects actifs du joueur ;
  // `data` pointe dans le buffer recv live -> on lit toute la liste via `len`
  // (champ de longueur du paquet). 7 = len(2)+aid(4)+status(1) garantis forwardés.
  Bourgeon::Instance().RegisterObserveOpcode(0x0A3B, 7);
  // ZC_BOURGEON_HATEFFECT_MAP 0x0F17 (opcode CUSTOM > 0x0C35 -> livré par le reader-hook,
  // data = octets APRÈS [type:2][len:2]) : table itemId->ordinal poussée au login. Cf.
  // bourgeon_opcodes.h / clif_bourgeon_hateffect_map (moonlight).
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kHatEffectMap);
}

// Suit les hat effects (.str) actifs sur le JOUEUR à partir de ZC_EQUIPMENT_EFFECT
// (0x0A3B). Le serveur envoie la liste COMPLÈTE (status=1) au spawn/refresh et des
// bascules unitaires (status=1 équip / status=0 déséquip). Modèle incrémental sur un
// ensemble : status=1 => ajoute chaque id, status=0 => retire. On ne garde que le
// propre joueur (aid == propre aid). `data` = buffer juste après l'opcode :
// data[0..1]=packetLength (inclut l'opcode), data[2..5]=aid, data[6]=status,
// data[7..]=liste d'effectId (2 o. chacun).
// Fil RÉSEAU : on copie, rien de plus (cf. features/net_inbox.h). 0x0A3B est un
// paquet à longueur ANNONCÉE dont le handler lit au-delà de `len` : PushAnnounced.
void BasicInfo::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                             uint16_t len) {
  if (opcode == 0x0A3B) net_inbox_.PushAnnounced(opcode, data, len);
  else                  net_inbox_.Push(opcode, data, len);
}

void BasicInfo::HandlePacket(uint16_t opcode, const uint8_t* data,
                             uint16_t len) {
  // ZC_BOURGEON_HATEFFECT_MAP (custom recv) : data = APRÈS le header [type:2][len:2]
  // -> [count:2] puis count × {itemId:4, ordinal:2}. Remplace la table en cache.
  if (opcode == bopcodes::kHatEffectMap) {
    if (!data || len < 2) return;
    const uint16_t count = *reinterpret_cast<const uint16_t*>(data);
    const uint8_t* p = data + 2;
    g_hat_item_ord.clear();
    g_hat_item_ord.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
      if (2 + (i + 1) * 6 > len) break;  // garde-fou (paquet tronqué)
      const uint32_t id  = *reinterpret_cast<const uint32_t*>(p + i * 6);
      const uint16_t ord = *reinterpret_cast<const uint16_t*>(p + i * 6 + 4);
      if (id != 0 && ord != 0) g_hat_item_ord[id] = ord;
    }
    return;
  }
  if (opcode != 0x0A3B || len < 7 || data == nullptr) return;
  const uint16_t plen   = *reinterpret_cast<const uint16_t*>(data);
  const uint32_t aid    = *reinterpret_cast<const uint32_t*>(data + 2);
  const uint8_t  status = data[6];
  const uint32_t own    = Bourgeon::Instance().client().session().aid();
  // Corps = plen - en-tête(opcode2 + len2 + aid4 + status1 = 9). Borné défensivement.
  int body = static_cast<int>(plen) - 9;
  if (body < 0) body = 0;
  const int nfx = body / 2;
  if (aid != own) return;  // joueur only
  const uint8_t* p = data + 7;
  for (int i = 0; i < nfx; ++i) {
    const uint16_t fx = *reinterpret_cast<const uint16_t*>(p + i * 2);
    auto it = std::find(own_hat_effects_.begin(), own_hat_effects_.end(), fx);
    if (status) {
      if (it == own_hat_effects_.end()) { own_hat_effects_.push_back(fx); g_ez_frozen_valid = false; }
    } else if (it != own_hat_effects_.end()) {
      own_hat_effects_.erase(it);
      g_ez_frozen_valid = false;  // bbox FIT à recalculer pour ce qui reste
    }
  }
}

// Barre HP/SP native sous le personnage (`UIPlayerGage`) — battement par FRAME,
// en tête de frame, avant que le jeu ne dessine.
//
// 🔴 REJOUÉ à chaque frame plutôt que tenu par une comptabilité : le client
// détruit et recrée sa fenêtre sans prévenir (changement de carte, mort,
// re-spawn), et une comptabilité oublierait toujours un cas. Réécrire un drapeau
// déjà à la bonne valeur ne coûte qu'un stockage sur une liste d'une douzaine
// d'entrées.
//
// ⚠ On n'écrit `visible` QUE dans le sens du masquage. À la décoche,
// `own_gage_hidden_` la rend visible UNE fois : la forcer à visible chaque frame
// reprendrait au client ses propres raisons de la cacher.
void BasicInfo::OnGameFramePulse() {
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;
  if (hide_own_pc_gage_) {
    if (SetPlayerGageVisible(false)) own_gage_hidden_ = true;
  } else if (own_gage_hidden_) {
    SetPlayerGageVisible(true);
    own_gage_hidden_ = false;
  }
}

// Enforces the "Masquer la fenêtre Basic Info d'origine" option using the native
// VISIBILITY (SetVisible), NOT an off-screen move — so nothing corrupts a saved
// position. The msg-0x22 hook above hides it pre-render at creation; here we
// re-hide if the game re-shows it, and restore visibility when the option is off.
//   BasicInfo singleton ptr = *(g_UIWindowMgr 0x0131f4e8 + 0x1dc) (vtable
//   0x0103e35c); null until the HUD is created.
void BasicInfo::OnTick() {
  // 🔴 Plus rien à préparer ici pour le portrait : il se compose lui-même au
  // rendu, par `ro::DrawDoll`. L'ancien chemin faisait rendre le personnage par
  // le moteur natif pendant la phase de mise à jour pour en capturer les
  // calques — le composeur n'a besoin ni de ce détour, ni de son instant précis.

  // ── Capture EZ/CEffectMgr (hat effects `hatEffectID`) : composite doll + aperçu cashshop ──
  // Le module ez_capture remplit sa capture pendant la passe de rendu MONDE ; consommée par
  // RenderPlayerAvatar / RenderItemPreviewTooltip. Reset = frontière de frame (dans le module).
  // Ici on ré-arme le propriétaire pour la passe suivante.
  InstallEzCapture();
  // Aperçu cashshop : spawn/despawn l'effet EZ survolé sur le joueur (keep-alive expiré -> despawn).
  ReconcileEzPreview();
  // Arme le propriétaire. GameMode_GetActive renvoie 0 si le mode est transitoirement « inactif »
  // (*(mgr+0x58)!=1) ; on garde alors le DERNIER acteur connu (l'effet est continu, l'acteur stable).
  // Armé aussi si un aperçu cashshop est actif (effet spawné sur le joueur, à capturer).
  // ⚠ Cet acteur est seulement COMPARÉ, JAMAIS déréférencé (au changement de map il est libéré -> UAF).
  if (own_hat_effects_.empty() && g_ez_preview_active == 0) {
    g_ez_owner_actor = nullptr;
  } else {
    void* live = rag::OwnActor();
    if (live) g_ez_last_actor = live;
    g_ez_owner_actor = live ? live : g_ez_last_actor;
  }
  // (Le module résout lui-même l'acteur joueur : rien à lui transmettre ici.)
  // Ids concrets CIBLES = GetHatEffectID(ordinal) des hat effects équipés -> filtre le DESSIN
  // (DrawOpts::ids) pour isoler le(s) hat effect(s) du footprint (230) / aura (164). Les CEffectMgr
  // (Perm_Frost...) n'ont pas d'id résolu sur leur chemin : ils passent par `include_effmgr`.
  g_ez_target_count = 0;
  for (uint16_t ord : own_hat_effects_) {
    if (g_ez_target_count >= 8) break;
    const int cid = static_cast<int>(HatLuaNum(static_cast<int>(ord), "GetHatEffectID", -1.0f));
    if (cid > 0) g_ez_target_ids[g_ez_target_count++] = cid;
  }

  constexpr uintptr_t kBasicInfoPtr = 0x0131f6c4;  // 0x0131f4e8 + 0x1dc

  g_bi_hide = portrait_hide_basic_info_;  // seen by the pre-render msg-0x22 hook
  void* bi = *reinterpret_cast<void**>(kBasicInfoPtr);
  if (!bi) return;  // HUD not created yet

  // Jauges d'exp : rejoue le layout natif quand l'état « au niveau max serveur » bascule
  // (atteindre le max en jeu, ou le quitter). Inutile si la native est cachée, et surtout
  // on ne veut pas rejouer son dock de la grille d'icônes pendant qu'elle est hors-écran.
  if (!portrait_hide_basic_info_) {
    const bool base_capped = BIAtServerMaxBase();
    const bool job_capped  = BIAtServerMaxJob();
    if (!g_bi_cap_known || base_capped != g_bi_base_capped || job_capped != g_bi_job_capped) {
      g_bi_cap_known   = true;
      g_bi_base_capped = base_capped;
      g_bi_job_capped  = job_capped;
      reinterpret_cast<BILayoutFn_t>(kBILayoutChildren)(
          bi, *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(bi) + kBIWinHeight));
    }
  }

  if (portrait_hide_basic_info_) {
    BIPinOffscreen(bi);       // keep off-screen (raw write; persist mgr+0x514 untouched)
    bi_pinned_off_ = true;
  } else if (bi_pinned_off_) {
    if (g_bi_saved_x != kBIUnset) {  // restore real pos when the option is turned off
      *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(bi) + kBIWinX) = g_bi_saved_x;
      *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(bi) + kBIWinY) = g_bi_saved_y;
    }
    bi_pinned_off_ = false;
  }
}
