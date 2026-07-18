#include "plugins/spr_effect_lab.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "imgui.h"
#include "bourgeon.h"        // Bourgeon::Instance().IsMapLoading() / IsGameActive() (gate anti-crash warp)
#include "d3d9/d3d9_hook.h"  // D3D9_AdditiveBlendCallback (blend additif optionnel)
#include "utils/hooking/hook_manager.h"

// Backend actif (DX9 vs DX7) — choisit l'offset du handle GPU natif dans CTexture,
// exactement comme la capture du curseur RO / basic_info. Défini ailleurs.
extern bool g_imgui_dx7_active;

namespace spr_lab {
namespace {

// ── Adresses / offsets natifs (client 20250716, base 0x400000) ────────────────
// Réutilisés à l'identique de la RE existante (cf. basic_info.cc, docs/hat_effect_re.md).
constexpr uintptr_t kGameModeGet      = 0x00a75340;  // GameMode_GetActive(mgr)
constexpr uintptr_t kModeMgr          = 0x1213338;   // CModeMgr (arg)
constexpr int       kOffActorMgr      = 0xcc;        // CMode -> actorMgr
constexpr int       kOffOwnActor      = 0x2c;        // actorMgr -> acteur joueur
constexpr int       kOffCamera        = 0xd0;        // CMode -> caméra
constexpr int       kOffViewMtx       = 0x98;        // caméra -> matrice de vue (arg projection)

constexpr uintptr_t kSceneProject     = 0x005541b0;  // Scene_ProjectWorldToScreen(ctx,_,world,view,&sx,&sy,&invW)

constexpr uintptr_t kEzEffectDraw     = 0x00b666d0;  // EzEffect_Draw(this=nœud EZ, param_1) — hooké (appartenance)
constexpr uintptr_t kRenderQueueInsert= 0x00550b10;  // RenderQueue_InsertPrimitive(queue, prim, flags) — hooké (capture)

constexpr uintptr_t kEzChildVtbl      = 0x01088c48;  // nœud EZ enfant (celui que EzEffect_Draw dessine)
constexpr int       kEzParentOff      = 0x140;       // enfant EZ -> nœud PRIMITIF parent
constexpr int       kEzEffectId       = 0x168;       // nœud primitif -> id d'effet CONCRET (Digital_Space = 1240)
constexpr int       kEzSrcActor       = 0x138;       // nœud primitif -> acteur source

constexpr int       kPrimVertsOff     = 0x0;         // prim[0] -> tableau de sommets (XYZRHW écran)
constexpr int       kPrimTexOff       = 0x8;         // prim[2] -> CTexture*
constexpr int       kPrimVertStride   = 0x20;        // 4 sommets, stride 0x20 (x@0 y@4 z@8 w@c argb@10 u@18 v@1c)
constexpr int       kCTexOffDX9       = 0x12c;       // CTexture -> IDirect3DTexture9*
constexpr int       kCTexOffDX7       = 0x128;       // CTexture -> handle DX7

constexpr uintptr_t kToggleEffectId   = 0x00c44940;  // Actor_ToggleEffectId(actor, unifiedId, add) __thiscall
constexpr int       kHatOrdinalBase   = 0x98a;       // id unifié d'un hat effect = ordinal + 0x98a

constexpr int       kQueueFrameOff    = 0x38;        // scene-queue -> compteur de frame (frontière = vidage buffer)

// Bridge Lua brut (Lua 5.1) — pour appeler GetHatEffectID(ordinal) = getter NATIF de l'id concret
// (ordinal -> id d'effet interne). Mêmes adresses/mécanique que basic_info.cc (HatLuaNum) :
// double-deref de g_pLuaStateMgr, lua_checkstack AVANT push (le natif le fait), résultat via
// lua_tonumber. Évite tout hardcode de l'id concret (cf. règle « jamais hardcoder, appeler le natif »).
constexpr uintptr_t kLuaStateB    = 0x015ffd78;  // g_pLuaStateMgr : *=mgr ; **=lua_State
constexpr uintptr_t kLuaGetFieldB = 0x00519df0;  // lua_getfield(L,idx,k)
constexpr uintptr_t kLuaPushNumB  = 0x0051a4b0;  // lua_pushnumber(L,double)
constexpr uintptr_t kLuaPCallB    = 0x0051a290;  // lua_pcall(L,nargs,nres,errf)->int
constexpr uintptr_t kLuaToNumB    = 0x0051ad20;  // lua_tonumber(L,idx)->double
constexpr uintptr_t kLuaToLStrB   = 0x0051aca0;  // lua_tolstring(L,idx,&len)->const char*
constexpr uintptr_t kLuaCheckStk  = 0x0051b570;  // lua_checkstack(L,n)
constexpr uintptr_t kLuaSetTopB   = 0x0051aab0;  // lua_settop(L,idx)
constexpr int       kLuaGlobalsB  = -10002;      // LUA_GLOBALSINDEX (5.1)

// ── Cas de test par défaut : Digital_Space ────────────────────────────────────
// Ordinal 87 (HatEffectIDs.lub) -> on SPAWN via Actor_ToggleEffectId(actor, 87+0x98a, 1).
// L'id CONCRET (Digital_Space = 1240) n'est PAS hardcodé : on le RÉSOUT via le getter NATIF
// GetHatEffectID(ordinal) (comme basic_info). On CAPTURE en matchant le nœud primitif sur cet id.
int  g_ui_ordinal        = 87;    // ordinal à spawner (SEUL champ saisi)
int  g_resolved_concrete = 0;     // aperçu : GetHatEffectID(g_ui_ordinal) (affichage UI)
bool g_suppress          = true;  // true = ne PAS dessiner l'effet en jeu (overlay seul)
bool g_additive          = false; // blend additif global (glows) ; défaut alpha normal
bool g_debug             = false; // overlay diagnostic : capture BRUTE + ancre + rayon (in situ)
bool  g_dbg_proj_ok      = false; // dernière ancre : true = projection native, false = repli bbox
float g_dbg_ax = 0.0f, g_dbg_ay = 0.0f;  // dernière ancre écran (diagnostic UI)
// Mode de calcul de l'ANCRE (le point de la capture qu'on ramène au centre de l'écran).
// 0 = MÉDIANE des sommets (défaut) : l'ancre est déduite des quads EUX-MÊMES, donc l'amas atterrit au
//     centre PAR CONSTRUCTION. Robuste : contrairement au centre de bbox, un seul quad géant (ou un
//     sommet NaN/derrière-caméra) ne la déplace pas — la médiane ignore les extrêmes.
// 1 = projection monde (ancienne valeur par défaut) : correcte pour beaucoup d'effets, mais tombe à
//     ~1000 px de la géométrie pour certains (constaté id 829) -> tout l'effet part hors écran.
// 2 = centre de bbox : sensible aux outliers, gardé pour comparaison/diagnostic.
int   g_anchor_mode      = 0;
// Garde-fous VOLONTAIREMENT LARGES : les « traînées » sont de la géométrie LÉGITIME (le natif les rend
// en additif + dégradé d'alpha, donc discrètes) — c'est notre RENDU qu'on a corrigé, pas la géométrie.
// Ces deux filtres ne servent plus qu'à écarter les cas francs (NaN/inf, quad absurde). À baisser
// seulement si un effet précis déborde encore.
float g_max_r            = 2000.0f; // rayon (px) max d'un sommet vs l'ancre (attrape aussi NaN/inf)
float g_max_quad         = 4000.0f; // taille (px) max d'un quad (anti-quad absurde)

// ── État de spawn (reconcile persistant : SURVIT au changement de map/@refresh) ──
int   g_wanted_ordinal    = 0;        // ordinal voulu (0 = éteint)
int   g_applied_ordinal   = 0;        // ordinal appliqué sur l'acteur COURANT
int   g_applied_concrete  = 0;        // id concret de l'effet spawné (= GetHatEffectID(applied)) — match capture
void* g_applied_actor     = nullptr;  // acteur sur lequel on a appliqué. ⚠⚠ COMPARAISON SEULE : au warp cet
                                      // acteur est LIBÉRÉ (operator_delete) -> le déréférencer = USE-AFTER-FREE.
int   g_no_capture_frames = 0;        // frames rendus consécutifs SANS capture pendant qu'on veut l'effet
constexpr int kReviveFrames = 60;     // seuil de « perte » (~1 s) : re-spawn si acteur changé OU 0 capture > K

// ── Buffer de capture ─────────────────────────────────────────────────────────
struct Quad {
  float x[4], y[4], u[4], v[4];
  unsigned argb[4];
  void*    tex;    // handle GPU natif (== ImTextureID)
  unsigned blend;  // param_2 de RenderQueue_InsertPrimitive (bits bucket/blend)
};
constexpr int kCapMax = 2048;   // Digital_Space émet des centaines de quads
Quad     g_caps[kCapMax];
int      g_count = 0;
uint32_t g_last_frame = 0;
void*    g_queue = nullptr;      // scene-queue vue au dernier insert (pour la projection)
float    g_world[3] = {0, 0, 0}; // position monde du nœud (ancre) ce frame
bool     g_world_set = false;

// État « en cours de dessin » posé par le hook EzEffect_Draw, lu par le hook insert.
bool     g_cur_owned = false;
void*    g_cur_node  = nullptr;

// ── Helpers ───────────────────────────────────────────────────────────────────
void* GetOwnActor() {
  void* actor = nullptr;
  __try {
    void* gm = reinterpret_cast<void*(__fastcall*)(int)>(kGameModeGet)(static_cast<int>(kModeMgr));
    if (gm) {
      void* mgr = *reinterpret_cast<void**>(reinterpret_cast<char*>(gm) + kOffActorMgr);
      if (mgr) actor = *reinterpret_cast<void**>(reinterpret_cast<char*>(mgr) + kOffOwnActor);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { actor = nullptr; }
  return actor;
}

// GetHatEffectID(ordinal) NATIF : ordinal (87) -> id concret (1240). AUCUN hardcode.
// Bridge Lua brut (Lua 5.1) avec lua_checkstack AVANT push (le natif le fait ; sinon pile pleine
// -> pcall échoue -> -1). POD/SEH. Renvoie <=0 si non résolu (Lua pas prêt). Cf. basic_info HatLuaNum.
int ResolveConcreteId(int ordinal) {
  int r = 0;
  __try {
    void* M = *reinterpret_cast<void**>(kLuaStateB);
    void* L = M ? *reinterpret_cast<void**>(M) : nullptr;
    if (L) {
      reinterpret_cast<int(__cdecl*)(void*, int)>(kLuaCheckStk)(L, 3);
      reinterpret_cast<void(__cdecl*)(void*, int, const char*)>(kLuaGetFieldB)(
          L, kLuaGlobalsB, "GetHatEffectID");
      reinterpret_cast<void(__cdecl*)(void*, double)>(kLuaPushNumB)(L, static_cast<double>(ordinal));
      if (reinterpret_cast<int(__cdecl*)(void*, int, int, int)>(kLuaPCallB)(L, 1, 1, 0) == 0)
        r = static_cast<int>(reinterpret_cast<double(__cdecl*)(void*, int)>(kLuaToNumB)(L, -1));
      reinterpret_cast<void(__cdecl*)(void*, int)>(kLuaSetTopB)(L, -2);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
  return r;
}

// GetHatEfResName(ordinal) NATIF : chemin du .str d'un hat effect « name-based » (ex. gold_shower),
// VIDE pour les effets EZ/hatEffectID. Sert de libellé indicatif dans le catalogue. POD/SEH.
void ResolveResName(int ordinal, char* out, int cap) {
  if (cap <= 0) return;
  out[0] = '\0';
  __try {
    void* M = *reinterpret_cast<void**>(kLuaStateB);
    void* L = M ? *reinterpret_cast<void**>(M) : nullptr;
    if (L) {
      reinterpret_cast<int(__cdecl*)(void*, int)>(kLuaCheckStk)(L, 3);
      reinterpret_cast<void(__cdecl*)(void*, int, const char*)>(kLuaGetFieldB)(
          L, kLuaGlobalsB, "GetHatEfResName");
      reinterpret_cast<void(__cdecl*)(void*, double)>(kLuaPushNumB)(L, static_cast<double>(ordinal));
      if (reinterpret_cast<int(__cdecl*)(void*, int, int, int)>(kLuaPCallB)(L, 1, 1, 0) == 0) {
        const char* s = reinterpret_cast<const char*(__cdecl*)(void*, int, size_t*)>(kLuaToLStrB)(
            L, -1, nullptr);
        if (s && s[0]) { std::strncpy(out, s, cap - 1); out[cap - 1] = '\0'; }
      }
      reinterpret_cast<void(__cdecl*)(void*, int)>(kLuaSetTopB)(L, -2);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

// ── Catalogue des effets .spr/EZ (scan NATIF, pas de liste hardcodée) ──────────
// On balaie les ordinaux et on interroge le client lui-même : GetHatEffectID(ord) > 0 => c'est un
// effet EZ/hatEffectID (ce que CE lab sait rendre) ; on note aussi GetHatEfResName (indicatif).
// Construit à la demande (bouton), une fois, en jeu (Lua prêt). ~500 pcalls -> quelques ms.
struct HatEntry { int ord; int cid; char res[48]; };
std::vector<HatEntry> g_catalog;
bool g_catalog_built = false;
constexpr int kMaxOrdinalScan = 512;

void BuildCatalog() {
  g_catalog.clear();
  for (int ord = 1; ord < kMaxOrdinalScan; ++ord) {
    const int cid = ResolveConcreteId(ord);
    char res[48];
    ResolveResName(ord, res, sizeof(res));
    if (cid > 0) {                      // effet EZ/hatEffectID = rendable par ce lab
      HatEntry e;
      e.ord = ord; e.cid = cid;
      std::strncpy(e.res, res, sizeof(e.res) - 1); e.res[sizeof(e.res) - 1] = '\0';
      g_catalog.push_back(e);
    }
  }
  g_catalog_built = true;
}

// Le nœud EZ en cours de dessin est-il NOTRE effet ? vtbl enfant EZ, id concret du primitif parent
// (+0x168) == g_applied_concrete (résolu au spawn), ET acteur source (+0x138) == g_applied_actor.
// ⚠ g_applied_actor est COMPARÉ, jamais déréférencé (UAF-safe). Le double critère isole notre effet
// des autres effets EZ (auras/statuts) présents sur le même acteur. Inerte si aucun effet actif.
bool IsOurNode(void* ez) {
  if (g_applied_ordinal == 0 || g_applied_concrete <= 0 || !ez) return false;
  if (*reinterpret_cast<uintptr_t*>(ez) != kEzChildVtbl) return false;
  void* prim = *reinterpret_cast<void**>(reinterpret_cast<char*>(ez) + kEzParentOff);
  if (!prim) return false;
  if (*reinterpret_cast<int*>(reinterpret_cast<char*>(prim) + kEzEffectId) != g_applied_concrete)
    return false;
  void* src = *reinterpret_cast<void**>(reinterpret_cast<char*>(prim) + kEzSrcActor);
  return src == g_applied_actor;  // comparaison seule
}

// ── Hooks (chaînés au-dessus de ceux de basic_info : PrepareHook relocalise le JMP) ──
using EzDrawFn = void(__fastcall*)(void* self, void* edx, float* p);
using RenderInsertFn = void(__fastcall*)(void* queue, void* edx, int* prim, unsigned flags);
EzDrawFn       g_orig_ez_draw = nullptr;
RenderInsertFn g_orig_insert  = nullptr;

// Établit l'appartenance au niveau NŒUD : `self` = le nœud EZ dessiné. Save/restore = robuste
// au nesting (un effet dessine plusieurs sous-nœuds/couches).
void __fastcall Hooked_EzDraw(void* self, void* edx, float* p) {
  const bool  prevOwned = g_cur_owned;
  void* const prevNode  = g_cur_node;
  bool owned = false;
  __try { owned = IsOurNode(self); } __except (EXCEPTION_EXECUTE_HANDLER) { owned = false; }
  g_cur_owned = owned;
  g_cur_node  = owned ? self : nullptr;
  g_orig_ez_draw(self, edx, p);
  g_cur_owned = prevOwned;
  g_cur_node  = prevNode;
}

// Puits commun. Frontière de frame -> vide le buffer. Si le nœud en cours est le nôtre : on
// snapshot le quad (4 sommets XYZRHW écran + UV + couleur + texture). Si g_suppress, on NE
// chaîne PAS (l'effet ne rend pas en jeu) ; sinon passthrough.
void __fastcall Hooked_Insert(void* queue, void* edx, int* prim, unsigned flags) {
  __try {
    const uint32_t frame = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(queue) + kQueueFrameOff);
    if (frame != g_last_frame) { g_last_frame = frame; g_count = 0; g_world_set = false; }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}

  bool captured = false;
  if (g_cur_owned && prim) {
    __try {
      g_queue = queue;
      if (!g_world_set && g_cur_node) {  // ancre = pos monde du nœud EZ (+0x10/14/18)
        const float* w = reinterpret_cast<const float*>(reinterpret_cast<char*>(g_cur_node) + 0x10);
        g_world[0] = w[0]; g_world[1] = w[1]; g_world[2] = w[2];
        g_world_set = true;
      }
      const char* verts = *reinterpret_cast<char* const*>(reinterpret_cast<char*>(prim) + kPrimVertsOff);
      void* ctex = *reinterpret_cast<void* const*>(reinterpret_cast<char*>(prim) + kPrimTexOff);
      const int off = g_imgui_dx7_active ? kCTexOffDX7 : kCTexOffDX9;
      void* native = ctex ? *reinterpret_cast<void**>(reinterpret_cast<char*>(ctex) + off) : nullptr;
      if (verts && native && g_count < kCapMax) {
        Quad& q = g_caps[g_count++];
        q.tex = native;
        q.blend = flags;
        for (int k = 0; k < 4; ++k) {
          const float* vtx = reinterpret_cast<const float*>(verts + k * kPrimVertStride);
          q.x[k] = vtx[0]; q.y[k] = vtx[1];
          q.u[k] = vtx[6]; q.v[k] = vtx[7];
          q.argb[k] = *reinterpret_cast<const unsigned*>(verts + k * kPrimVertStride + 0x10);
        }
        captured = true;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
  }

  // Suppression in-world : pour NOS quads capturés, on ne chaîne pas -> ni le natif ni
  // basic_info ne les voient -> invisibles en jeu, visibles seulement dans l'overlay.
  if (captured && g_suppress) return;
  g_orig_insert(queue, edx, prim, flags);
}

// ── Reconcile spawn / despawn (persistant, survit au changement de map) ────────
// RE (workflow 2026-07-18) : au warp/@refresh l'acteur joueur est DÉTRUIT+RECRÉÉ (nouveau
// pointeur, operator_new(0x540)) et sa liste d'effets repart vide ; le jeu ne rejoue QUE les
// effets équipés (via g_StatusEffectList côté serveur), PAS un effet purement client -> le nôtre
// est perdu. On le détecte et on le re-spawn. GATE obligatoire : ne rien faire pendant le
// chargement de map (agir = UAF connu) ni hors du monde actif.
void Reconcile() {
  if (Bourgeon::Instance().IsMapLoading() || !Bourgeon::Instance().IsGameActive())
    return;  // défense en profondeur (RenderFrame est déjà gardé en amont)

  void* actor = GetOwnActor();

  // DÉTECTION DE PERTE : UNIQUEMENT le changement de pointeur d'acteur (warp/@refresh : l'acteur est
  // détruit+recréé -> nouveau pointeur, RE confirmée). ⚠ NE PAS re-toggler sur le MÊME acteur :
  // Actor_ToggleEffectId fait un remove-all/add-all dont la passe *remove* ne nettoie PAS les nœuds
  // orphelins (case 3 ne suit qu'UN nœud en +0x14c) -> chaque re-toggle LAISSE un nœud primitif de
  // plus dans la scene-list, TICKÉ en plus -> l'animation accélère (2×,3×… = bug « plus rapide »,
  // workflow 2026-07-18). Un NOUVEL acteur repart propre (ancien + orphelins libérés ensemble) ->
  // re-spawn au warp = 1 seule instance = vitesse native. ⚠ actor != g_applied_actor = COMPARAISON.
  if (g_applied_ordinal != 0 && actor != g_applied_actor) {
    g_applied_ordinal = 0;
  }

  if (g_wanted_ordinal == g_applied_ordinal && actor == g_applied_actor) return;  // rien à faire
  if (!actor) return;                                                             // hors-jeu : on retentera

  using ToggleFn = void(__thiscall*)(void*, int, char);
  const ToggleFn toggle = reinterpret_cast<ToggleFn>(kToggleEffectId);
  __try {
    // Despawn propre SEULEMENT si l'acteur est encore le nôtre (sinon l'ancien est déjà libéré).
    if (g_applied_ordinal != 0 && actor == g_applied_actor)
      toggle(actor, g_applied_ordinal + kHatOrdinalBase, 0);
    if (g_wanted_ordinal != 0) {
      g_applied_concrete = ResolveConcreteId(g_wanted_ordinal);  // id concret NATIF (pas de hardcode)
      toggle(actor, g_wanted_ordinal + kHatOrdinalBase, 1);
    } else {
      g_applied_concrete = 0;
    }
    g_applied_ordinal   = g_wanted_ordinal;
    g_applied_actor     = actor;
    g_no_capture_frames = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_applied_ordinal   = g_wanted_ordinal;
    g_applied_actor     = actor;
    g_no_capture_frames = 0;
  }
}

// ── Overlay : dessine les quads capturés AU CENTRE de l'écran ─────────────────
// Ancre écran = projection de la position monde du nœud (stable, suit l'effet en jeu) ->
// out = centre + (v - ancre) : conserve forme + TAILLE natives, juste recentré. Repli =
// centre de la bbox des quads si la projection échoue.
// Un quad doit-il être rejeté ? (a) sommet trop loin de l'ancre — attrape aussi NaN/inf (projection
// derrière la caméra) ; (b) quad DÉMESURÉ (traînée dégénérée). Les deux sont des garde-fous : par
// défaut ils sont LARGES, car les « traînées » sont en fait de la géométrie LÉGITIME que le natif
// rend en additif + dégradé d'alpha — c'est notre rendu qu'il faut corriger, pas la géométrie.
// Utilisé par le rendu ET l'overlay debug (mêmes couleurs = pas de divergence).
bool QuadDropped(const Quad& q, float ax, float ay, float r2) {
  for (int k = 0; k < 4; ++k) {
    const float dxp = q.x[k] - ax, dyp = q.y[k] - ay;
    if (!(dxp * dxp + dyp * dyp < r2)) return true;
  }
  float x0 = q.x[0], x1 = q.x[0], y0 = q.y[0], y1 = q.y[0];
  for (int k = 1; k < 4; ++k) {
    if (q.x[k] < x0) x0 = q.x[k];
    if (q.x[k] > x1) x1 = q.x[k];
    if (q.y[k] < y0) y0 = q.y[k];
    if (q.y[k] > y1) y1 = q.y[k];
  }
  return !((x1 - x0) < g_max_quad && (y1 - y0) < g_max_quad);
}

void DrawCenteredOverlay() {
  if (g_count <= 0) return;
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  if (disp.x <= 0 || disp.y <= 0) return;
  const float cx = disp.x * 0.5f, cy = disp.y * 0.5f;
  const int count = g_count < kCapMax ? g_count : kCapMax;

  // Ancre : projection du monde (comme le natif Effect_SubmitStrQuad). SEH autour du natif.
  float ax = 0.0f, ay = 0.0f, invW = 0.0f;
  bool have_anchor = false;
  if (g_queue && g_world_set) {
    __try {
      void* gm  = reinterpret_cast<void*(__fastcall*)(int)>(kGameModeGet)(static_cast<int>(kModeMgr));
      void* cam = gm ? *reinterpret_cast<void**>(reinterpret_cast<char*>(gm) + kOffCamera) : nullptr;
      float* view = cam ? reinterpret_cast<float*>(reinterpret_cast<char*>(cam) + kOffViewMtx) : nullptr;
      if (view) {
        reinterpret_cast<void(__fastcall*)(void*, void*, float*, float*, float*, float*, float*)>(
            kSceneProject)(g_queue, nullptr, g_world, view, &ax, &ay, &invW);
        have_anchor = true;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { have_anchor = false; }
  }
  if (!have_anchor) {  // repli : centre de la bbox capturée
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < count; ++i)
      for (int k = 0; k < 4; ++k) {
        const float vx = g_caps[i].x[k], vy = g_caps[i].y[k];
        if (vx < x0) x0 = vx; if (vx > x1) x1 = vx;
        if (vy < y0) y0 = vy; if (vy > y1) y1 = vy;
      }
    if (x1 < x0) return;
    ax = (x0 + x1) * 0.5f; ay = (y0 + y1) * 0.5f;
  }
  g_dbg_proj_ok = have_anchor; g_dbg_ax = ax; g_dbg_ay = ay;  // diagnostic (affiché dans l'UI)

  // D3DCOLOR (0xAARRGGBB) -> IM_COL32 (0xAABBGGRR) : échange R<->B.
  auto conv = [](unsigned c) -> unsigned {
    return (c & 0xff00ff00u) | ((c & 0x00ff0000u) >> 16) | ((c & 0x000000ffu) << 16);
  };

  ImDrawList* dl = ImGui::GetForegroundDrawList();  // au-dessus de tout
  const float r2 = g_max_r * g_max_r;

  // MODE DEBUG : dessine la capture BRUTE (positions écran NATIVES, sans reproject) + l'ancre et le
  // cercle du rayon. À utiliser avec « Cacher en jeu » DÉCOCHÉ : nos contours de quads se superposent
  // alors à l'effet natif in-world -> on voit si l'ancre tombe au bon endroit et quels quads sont des
  // outliers (ROUGE = rejeté par le filtre, VERT = gardé). Diagnostic quand le reproject déraille.
  if (g_debug) {
    dl->AddCircleFilled(ImVec2(ax, ay), 5.0f, IM_COL32(0, 255, 255, 255));        // ancre
    dl->AddCircle(ImVec2(ax, ay), g_max_r, IM_COL32(0, 255, 255, 120), 64, 1.5f);  // rayon du filtre
    for (int i = 0; i < count; ++i) {
      const Quad& q = g_caps[i];
      const bool drop = QuadDropped(q, ax, ay, r2);
      dl->AddQuad(ImVec2(q.x[0], q.y[0]), ImVec2(q.x[1], q.y[1]),
                  ImVec2(q.x[3], q.y[3]), ImVec2(q.x[2], q.y[2]),
                  drop ? IM_COL32(255, 60, 60, 220) : IM_COL32(60, 255, 60, 220), 1.5f);
    }
    return;  // en debug on ne dessine PAS l'overlay reprojeté
  }

  if (g_additive)
    dl->AddCallback(reinterpret_cast<ImDrawCallback>(D3D9_AdditiveBlendCallback()), nullptr);
  void* cur_tex = nullptr;      // texture actuellement poussée (batch : 1 draw call par texture)
  bool  tex_pushed = false;
  for (int i = 0; i < count; ++i) {
    const Quad& q = g_caps[i];
    if (!q.tex) continue;
    if (QuadDropped(q, ax, ay, r2)) continue;  // garde-fous larges (outliers francs / NaN)
    // Sommets natifs v0=HG v1=HD v2=BG v3=BD -> ordre ImGui HG,HD,BD,BG (v0,v1,v3,v2).
    const ImVec2 p0(cx + (q.x[0] - ax), cy + (q.y[0] - ay));
    const ImVec2 p1(cx + (q.x[1] - ax), cy + (q.y[1] - ay));
    const ImVec2 p2(cx + (q.x[3] - ax), cy + (q.y[3] - ay));
    const ImVec2 p3(cx + (q.x[2] - ax), cy + (q.y[2] - ay));
    // ⚠ COULEURS PAR SOMMET (gouraud). AddImageQuad n'accepte qu'UNE couleur : appliquer celle du
    // sommet 0 à tout le quad rendait OPAQUES les traînées à DÉGRADÉ D'ALPHA (nativement elles
    // s'estompent jusqu'à transparent) -> d'où le « mess » de longues traînées solides. On écrit
    // donc les 4 sommets à la main avec leur propre couleur/alpha, comme le fait le natif.
    // Texture poussée SEULEMENT quand elle change (sinon 1 draw call par quad = des centaines).
    if (q.tex != cur_tex) {
      if (tex_pushed) dl->PopTexture();
      dl->PushTexture((ImTextureID)(uintptr_t)q.tex);
      cur_tex = q.tex;
      tex_pushed = true;
    }
    dl->PrimReserve(6, 4);
    const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    dl->PrimWriteIdx(base);     dl->PrimWriteIdx(base + 1); dl->PrimWriteIdx(base + 2);
    dl->PrimWriteIdx(base);     dl->PrimWriteIdx(base + 2); dl->PrimWriteIdx(base + 3);
    dl->PrimWriteVtx(p0, ImVec2(q.u[0], q.v[0]), conv(q.argb[0]));
    dl->PrimWriteVtx(p1, ImVec2(q.u[1], q.v[1]), conv(q.argb[1]));
    dl->PrimWriteVtx(p2, ImVec2(q.u[3], q.v[3]), conv(q.argb[3]));
    dl->PrimWriteVtx(p3, ImVec2(q.u[2], q.v[2]), conv(q.argb[2]));
  }
  if (tex_pushed) dl->PopTexture();
  if (g_additive)
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

}  // namespace

// ── API publique ──────────────────────────────────────────────────────────────
void EnsureInstalled() {
  static bool done = false;
  if (done) return;
  done = true;
  using namespace hooking;
  g_orig_insert = reinterpret_cast<RenderInsertFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kRenderQueueInsert),
          reinterpret_cast<uint8_t*>(&Hooked_Insert)));
  g_orig_ez_draw = reinterpret_cast<EzDrawFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kEzEffectDraw),
          reinterpret_cast<uint8_t*>(&Hooked_EzDraw)));
}

void RenderFrame() {
  Reconcile();
  DrawCenteredOverlay();
  // LIVENESS (hors Reconcile : doit compter CHAQUE frame, même quand Reconcile early-return —
  // sinon la détection de perte par « 0 capture » se fige). g_count = quads capturés ce frame.
  if (g_wanted_ordinal != 0) {
    if (g_count > 0) g_no_capture_frames = 0;
    else if (g_applied_ordinal != 0) g_no_capture_frames++;
  } else {
    g_no_capture_frames = 0;
  }
}

void DrawDebugControls() {
  EnsureInstalled();  // installe les hooks au 1er affichage du panneau (lazy)

  ImGui::TextUnformatted("SPR Effect Lab — rend un hat effect .spr/EZ au centre de l'écran.");
  ImGui::Separator();

  // Seul l'ordinal est saisi ; l'id concret est résolu par le getter NATIF GetHatEffectID.
  ImGui::SetNextItemWidth(120.0f);
  if (ImGui::InputInt("Ordinal", &g_ui_ordinal) || g_resolved_concrete <= 0)
    g_resolved_concrete = ResolveConcreteId(g_ui_ordinal);
  ImGui::SameLine();
  if (g_resolved_concrete > 0)
    ImGui::Text("-> id concret %d (natif)", g_resolved_concrete);
  else
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "-> non résolu (en jeu ? Lua prêt ?)");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("L'id concret vient de GetHatEffectID(ordinal), le getter du jeu.\n"
                      "Ordinal 87 = Digital_Space -> 1240. Aucun hardcode.");

  const bool on = (g_wanted_ordinal != 0);
  if (!on) {
    if (ImGui::Button("Spawn + afficher au centre")) {
      g_wanted_ordinal = g_ui_ordinal;
      g_no_capture_frames = 0;
    }
  } else {
    if (ImGui::Button("Arrêter")) {
      g_wanted_ordinal = 0;
      g_no_capture_frames = 0;
    }
  }
  ImGui::SameLine();
  ImGui::Text("état: %s (%d quads)", on ? "actif" : "éteint", g_count);

  ImGui::Checkbox("Cacher en jeu (overlay seul)", &g_suppress);
  ImGui::SameLine();
  ImGui::Checkbox("Blend additif", &g_additive);
  ImGui::Checkbox("Debug capture (brut + ancre)", &g_debug);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Dessine la capture BRUTE (positions écran natives) + l'ancre (point cyan) et\n"
                      "le cercle du rayon, SANS reprojection. Décoche « Cacher en jeu » pour\n"
                      "comparer à l'effet natif : rouge = quad rejeté, vert = gardé.");
  ImGui::SameLine();
  ImGui::TextDisabled("ancre %s (%.0f,%.0f)", g_dbg_proj_ok ? "proj" : "bbox", g_dbg_ax, g_dbg_ay);

  ImGui::SetNextItemWidth(180.0f);
  ImGui::SliderFloat("Rayon (px)", &g_max_r, 100.0f, 3000.0f, "%.0f");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(180.0f);
  ImGui::SliderFloat("Quad max (px)", &g_max_quad, 100.0f, 4000.0f, "%.0f");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Garde-fous larges par défaut (la géométrie « traînée » est LÉGITIME :\n"
                      "le natif la rend en additif + dégradé d'alpha). À baisser seulement si\n"
                      "un effet déborde encore : Rayon = distance sommet/ancre, Quad max =\n"
                      "taille d'un quad.");

  // ── Catalogue : liste NATIVE des ordinaux d'effets .spr/EZ (aucune liste hardcodée) ──
  ImGui::Separator();
  if (ImGui::Button(g_catalog_built ? "Rescanner" : "Scanner le catalogue")) BuildCatalog();
  ImGui::SameLine();
  ImGui::TextDisabled("%d effets EZ trouvés (clic = spawn)", static_cast<int>(g_catalog.size()));
  if (g_catalog_built) {
    static char filter[32] = "";
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("filtre", filter, sizeof(filter));
    if (ImGui::BeginChild("##spr_catalog", ImVec2(0, 190), true)) {
      for (const HatEntry& e : g_catalog) {
        char label[128];
        if (e.res[0])
          std::snprintf(label, sizeof(label), "ord %-3d  ->  id %-5d   %s", e.ord, e.cid, e.res);
        else
          std::snprintf(label, sizeof(label), "ord %-3d  ->  id %-5d", e.ord, e.cid);
        if (filter[0] && !std::strstr(label, filter)) continue;
        if (ImGui::Selectable(label, e.ord == g_wanted_ordinal)) {
          g_ui_ordinal        = e.ord;
          g_resolved_concrete = e.cid;
          g_wanted_ordinal    = e.ord;   // spawn immédiat au clic
          g_no_capture_frames = 0;
        }
      }
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Certains (famille aura/statut) peuvent ne pas se rendre ici.");
  }
}

}  // namespace spr_lab
