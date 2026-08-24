#include "ragnarok/globals.h"
#include "features/fx/ez_effect_capture.h"

#include <Windows.h>
#include <intrin.h>   // _ReturnAddress() : identifier QUI soumet une primitive

#include <cstdint>

#include "d3d9/d3d9_hook.h"  // D3D9_ExplicitBlendCallback / D3D9_AdditiveBlendCallback
#include "ragnarok/game_scene.h"
#include "utils/hooking/hook_manager.h"

// Backend actif (DX9 vs DX7) : choisit l'offset du handle GPU natif dans CTexture.
extern bool g_imgui_dx7_active;

namespace ez_capture {
namespace {

// ── Adresses natives (client 20250716, base 0x400000) ─────────────────────────
constexpr int       kOffOwnActor       = 0x2c;        // actorMgr -> acteur joueur
constexpr int       kOffCamera         = 0xd0;        // CMode -> caméra
constexpr int       kOffViewMtx        = 0x98;        // caméra -> matrice de vue
constexpr uintptr_t kSceneProject      = 0x005541b0;  // Scene_ProjectWorldToScreen(ctx,_,world,view,&sx,&sy,&invW)
constexpr uintptr_t kDepthScale        = 0x00553e80;  // Effect_DepthToScreenScale(ctx,_,invW) -> float

constexpr uintptr_t kEzEffectDraw      = 0x00b666d0;  // EzEffect_Draw(nœud EZ) — hooké (appartenance)
constexpr uintptr_t kRenderQueueInsert = 0x00550b10;  // RenderQueue_InsertPrimitive — hooké (capture)

constexpr uintptr_t kEzChildVtbl       = 0x01088c48;  // vtable du nœud EZ enfant (celui que Draw dessine)
constexpr int       kEzParentOff       = 0x140;       // enfant EZ -> nœud PRIMITIF parent
constexpr int       kEzEffectId        = 0x168;       // nœud primitif -> id d'effet CONCRET
constexpr int       kEzSrcActor        = 0x138;       // nœud primitif -> acteur source
constexpr int       kEzNodeWorldPos    = 0x10;        // nœud EZ -> position monde (x,y,z)

// ── Second chemin : la famille CEffectMgr (auras, statuts — ex. Perm_Frost) ───
// Ces effets ne passent PAS par EzEffect_Draw : ils sont dessinés par effet, en phase render, via
// EffectInstance_RenderDraw. Sans ce hook, toute cette famille est invisible pour la capture.
// Deux différences importantes avec le chemin EZ :
//   - l'appartenance se détermine par HANDLE de propriétaire (effet+0x20 == handle du joueur), pas
//     par pointeur d'acteur ;
//   - la position monde de l'instance est à +0x8 (et non +0x10 comme sur un nœud EZ).
constexpr uintptr_t kEffRenderFn      = 0x00ae8480;  // EffectInstance_RenderDraw (__fastcall, ECX = effet)
constexpr int       kEffOwnerHnd      = 0x20;        // effet -> handle de l'acteur propriétaire
constexpr uintptr_t kCEZ2STRVtbl      = 0x010758d8;  // CEZ2STREffect (.str name-based) -> à EXCLURE
constexpr int       kEffWorldPos      = 0x08;        // instance CEffectMgr -> position monde (x,y,z)
// Id de l'instance CEffectMgr (RE 2026-07-19) : posé par Effect_SetEffectId 0x00ae84c0 (vtable +0x6c)
// dans la classe RACINE (ctor 0x00ae7cd0) -> valable pour les ~100 sous-classes. Vaut -1 avant le
// setter. ⚠ ESPACE D'IDS DIFFÉRENT de l'id concret des hat effects : ici, < 0x98a = effet générique
// (skill/statut) et >= 0x98a = hat effect dont la valeur est `ordinal + 0x98a`. Les deux espaces se
// CHEVAUCHENT : ne jamais comparer un id d'instance à un id concret.
// ── Troisième famille : effets délégués à un hôte particule (CEZ2STRParticle) ─
// Un `CEZ2STREffect` ne dessine pas lui-même : il crée un hôte particule (effet+0xcc, via
// `CEZ2STREffect_CreateHostSprite` 0x00b1b3b0) et c'est le système de particules qui soumet les
// quads. AUCUN de nos hooks d'appartenance n'est sur ce chemin — ces effets étaient donc totalement
// invisibles (constaté sur l'ordinal 277 / id 2443).
// L'appelant a été identifié EMPIRIQUEMENT en relevant les adresses de retour du puits, effet actif
// vs éteint : seule `0x00ADA5A4` apparaît avec l'effet (retour de l'appel situé en 0x00ADA59F, dans
// la fonction 0x00ADA590). On reconnaît donc ces primitives à leur PROVENANCE, sans hook de plus.
// ⚠ Leur id d'effet n'est pas accessible sur ce chemin : elles sont marquées `kStrParticleId` et un
// consommateur les inclut explicitement (DrawOpts::include_str_particle).
constexpr uintptr_t kStrParticleRet   = 0x00ADA5A4;

constexpr int       kEffEffectId      = 0x04;
constexpr int       kEffHatIdBase     = 0x98a;       // id d'instance d'un hat effect = ordinal + 0x98a

// Layout de l'enregistrement de primitive — PROUVÉ par le flush RendererDX9_DrawPrimRecord
// 0x0055c830 (DrawPrimitiveUP/DrawIndexedPrimitiveUP, stride 0x20, FVF 0x1C4).
constexpr int kPrimVertsOff    = 0x00;  // -> tableau de sommets (XYZRHW écran)
constexpr int kPrimVtxCountOff = 0x04;  // ⚠ nombre de sommets RÉEL (3 ou 4) — jamais supposer 4
constexpr int kPrimTexOff      = 0x08;  // -> CTexture*
constexpr int kPrimIdxCountOff = 0x10;  // nb d'indices (0 = non indexé)
constexpr int kPrimSrcBlendOff = 0x18;  // D3DRS_SRCBLEND  (blend PAR RECORD)
constexpr int kPrimDstBlendOff = 0x1c;  // D3DRS_DESTBLEND
constexpr int kPrimTypeOff     = 0x24;  // D3DPRIMITIVETYPE : 4 = TRIANGLELIST, 5 = TRIANGLESTRIP
constexpr int kPrimVertStride  = 0x20;  // x@0 y@4 z@8 rhw@0xc argb@0x10 specular@0x14 u@0x18 v@0x1c

constexpr int kCTexOffDX9      = 0x12c;  // CTexture -> IDirect3DTexture9*
constexpr int kCTexOffDX7      = 0x128;  // CTexture -> handle DX7

constexpr int kQueueFrameOff   = 0x38;   // scene-queue -> compteur de frame (frontière = vidage)

// Table de saut du dispatcher d'effect id (0x00bb9260, slot vtbl+0x3c de la vtable primitive) :
//   eax = effectId - 491 ; if (eax > 0x79a) -> DEFAULT ; else jmp [eax*4 + 0x00bc2e04]
// DEFAULT = `mov al,1 ; ret` -> nœud créé et tické, mais MUET.
constexpr uintptr_t kEffectJumpTable   = 0x00bc2e04;
constexpr int       kEffectJumpBase    = 491;
constexpr int       kEffectJumpCount   = 1947;
constexpr uintptr_t kEffectJumpDefault = 0x00bc2de1;

// ── État ──────────────────────────────────────────────────────────────────────
// ⚠ Depuis que le ciblage a quitté la capture, ce buffer accueille TOUS les effets du joueur (et
// plus un seul effet filtré) : à taille égale il serait devenu relativement plus étroit, alors qu'un
// effet dense émet déjà des centaines de primitives. Doublé en conséquence. Dépassement = troncature
// silencieuse (l'effet s'affiche incomplet), d'où la marge.
constexpr int kCapMax = 4096;
Prim     g_caps[kCapMax];
int      g_count = 0;
uint32_t g_last_frame = 0xFFFFFFFFu;

void*    g_queue = nullptr;        // scene-queue vue au dernier insert (pour la projection)
// Dernière échelle de profondeur VALIDE (px écran par unité monde à la profondeur de l'acteur).
// Les familles sans position monde n'en produisent aucune ; réutiliser la dernière connue vaut
// infiniment mieux que 1.0, qui ferait dessiner l'effet à plusieurs fois sa taille sur un doll.
float    g_last_screen_scale = 0.0f;
// ⚠ PAS d'ancre GLOBALE : la position monde est stockée PAR PRIMITIVE (Prim::world). Une ancre
// unique, renseignée par la première primitive capturée de la frame, désignait un effet ARBITRAIRE
// dès lors que la capture couvre tous les effets du joueur — et l'ancre restait alors figée sur un
// autre effet pendant que le nôtre bougeait (constaté : ancre immobile à (4800,2354) alors que le
// personnage traversait l'écran) -> tout le rendu reprojeté partait hors écran.

// Acteur joueur, RÉSOLU PAR LE MODULE (rafraîchi à chaque frontière de frame).
// ⚠⚠ Il n'y a VOLONTAIREMENT aucun setter : quand c'était un réglage partagé, deux consommateurs
// l'écrivaient en alternance (le doll l'armait, le lab l'effaçait à chaque frame) et la capture ne
// tenait qu'une frame ou deux sur deux. Les consommateurs veulent tous le MÊME acteur — le joueur —
// donc le module le résout lui-même et le conflit n'existe plus.
// ⚠ COMPARÉ, JAMAIS déréférencé (au changement de map il est libéré).
void*    g_owner_actor = nullptr;
// Suppression : un emplacement PAR CONSOMMATEUR, la suppression effective étant leur UNION. Un
// tableau unique ferait du dernier appelant l'écraseur des autres (cf. les deux bugs d'état partagé).
constexpr int kMaxSuppressedPerSlot = 8;
int      g_suppressed_ids[kSuppressSlotCount][kMaxSuppressedPerSlot] = {};
int      g_suppressed_count[kSuppressSlotCount] = {};

// L'id concret de l'effet en cours de dessin (-1 = famille CEffectMgr : id non résolu sur ce chemin).
int      g_cur_effect_id = -1;
// L'id d'INSTANCE CEffectMgr en cours de dessin (-1 sur le chemin EZ). Espace distinct, cf. en-tête.
int      g_cur_effmgr_id = -1;

// Posé par les hooks de dessin, lu par le hook insert.
bool     g_cur_owned = false;
void*    g_cur_node  = nullptr;
bool     g_cur_via_effmgr = false;  // le nœud courant vient de CEffectMgr -> position monde à +0x8
// Contexte du rendu d'effet en cours, INDÉPENDANT de l'appartenance (g_cur_owned) : il faut pouvoir
// distinguer « effet du JOUEUR mais écarté par sa vtable » (les CEZ2STREffect, qui alimentent
// justement la famille « hôte particule ») de « effet ÉTRANGER » (ambiance de la map). Les deux sont
// non possédés ; seul le second doit voir ses particules refusées.
bool     g_cur_in_eff_render = false;  // on est dans EffectInstance_RenderDraw (owner lisible)
bool     g_cur_eff_is_player = false;  // ...et cet effet a pour propriétaire le JOUEUR (owner == AID)
bool     g_effmgr_enabled = true;   // interrupteur de DIAGNOSTIC (cf. en-tête), pas un réglage
bool     g_capture_str    = false;  // idem : lever l'exclusion des CEZ2STREffect (famille .str)

// Sonde (cf. Stats dans l'en-tête). Les *_cur s'accumulent, puis basculent sur frontière de frame.
Stats g_stats{}, g_stats_cur{};

// Relevé des APPELANTS du puits de primitives (cf. Caller dans l'en-tête). Bascule à la frontière de
// frame comme le reste. Volontairement borné : on ne cherche pas l'exhaustivité, seulement à repérer
// la fonction qui apparaît quand l'effet est actif et disparaît quand il ne l'est pas.
constexpr int kMaxCallers = 24;
Caller g_callers[kMaxCallers]{}, g_callers_cur[kMaxCallers]{};
int    g_caller_count = 0, g_caller_count_cur = 0;

void NoteCaller(uintptr_t ra) {
  for (int i = 0; i < g_caller_count_cur; ++i)
    if (g_callers_cur[i].addr == ra) { g_callers_cur[i].count++; return; }
  if (g_caller_count_cur < kMaxCallers)
    g_callers_cur[g_caller_count_cur++] = Caller{ra, 1};
}

using EzDrawFn       = void(__fastcall*)(void* self, void* edx, float* p);
using RenderInsertFn = void(__fastcall*)(void* queue, void* edx, int* prim, unsigned flags);
using EffRenderFn    = void(__fastcall*)(void* self, void* edx);
EzDrawFn       g_orig_ez_draw   = nullptr;
RenderInsertFn g_orig_insert    = nullptr;
EffRenderFn    g_orig_eff_render = nullptr;

// Le nœud EZ en cours de dessin appartient-il à l'acteur ciblé ? Critère : vtable enfant EZ + acteur
// source du nœud primitif parent. On ne filtre PAS par id ici : on relève l'id (`out_id`) et on le
// laisse au consommateur, qui filtre au dessin.
bool IsOurNode(void* ez, int* out_id) {
  if (!ez || !g_owner_actor) return false;
  if (*reinterpret_cast<uintptr_t*>(ez) != kEzChildVtbl) return false;
  void* prim = *reinterpret_cast<void**>(reinterpret_cast<char*>(ez) + kEzParentOff);
  if (!prim) return false;
  void* src = *reinterpret_cast<void**>(reinterpret_cast<char*>(prim) + kEzSrcActor);
  if (src != g_owner_actor) return false;  // comparaison seule
  if (out_id) *out_id = *reinterpret_cast<int*>(reinterpret_cast<char*>(prim) + kEzEffectId);
  return true;
}

// Résout l'acteur joueur. Appelé à chaque frontière de frame : si un dessin d'effet précède le
// premier insert de la frame, on compare avec la valeur de la frame précédente — sans conséquence,
// puisqu'on ne fait que COMPARER le pointeur (jamais le déréférencer).
void RefreshOwnerActor() {
  void* actor = nullptr;
  __try {
    void* gm = reinterpret_cast<void*(__fastcall*)(int)>(rag::kModeMgrGetActiveAddr)(static_cast<int>(rag::kModeMgrAddr));
    if (gm) {
      void* mgr = *reinterpret_cast<void**>(reinterpret_cast<char*>(gm) + gamescene::kGmActorMgr);
      if (mgr) actor = *reinterpret_cast<void**>(reinterpret_cast<char*>(mgr) + kOffOwnActor);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { actor = nullptr; }
  g_owner_actor = actor;
}

bool IsSuppressed(int effect_id) {
  // CEffectMgr (-1) : id non résolu -> non supprimable. En revanche kStrParticleId (-2) EST
  // supprimable : c'est un marqueur de FAMILLE, pas un id manquant — on masque alors toute la
  // famille, ce qui est acceptable le temps d'un survol.
  if (effect_id < 0 && effect_id != kStrParticleId) return false;
  for (int s = 0; s < kSuppressSlotCount; ++s)
    for (int i = 0; i < g_suppressed_count[s]; ++i)
      if (g_suppressed_ids[s][i] == effect_id) return true;
  return false;
}

// Appartenance au niveau NŒUD. Save/restore : robuste au nesting (un effet dessine plusieurs
// sous-nœuds/couches).
void __fastcall Hooked_EzDraw(void* self, void* edx, float* p) {
  const bool  prev_owned = g_cur_owned;
  void* const prev_node  = g_cur_node;
  const bool prev_via = g_cur_via_effmgr;
  const int  prev_id  = g_cur_effect_id;
  bool owned = false;
  int  id = -1;
  __try { owned = IsOurNode(self, &id); } __except (EXCEPTION_EXECUTE_HANDLER) { owned = false; }
  g_stats_cur.all_draws++;
  if (owned) {
    g_stats_cur.draws++;
    g_cur_via_effmgr = false;   // chemin EZ : position monde à +0x10
    g_cur_effect_id  = id;
    g_cur_effmgr_id  = -1;      // pas d'instance CEffectMgr sur ce chemin
  }
  g_cur_owned = owned;
  g_cur_node  = owned ? self : nullptr;
  g_orig_ez_draw(self, edx, p);
  g_cur_owned = prev_owned;
  g_cur_node  = prev_node;
  g_cur_via_effmgr = prev_via;
  g_cur_effect_id  = prev_id;
}

// Chemin CEffectMgr : dessin par effet, en phase render. Arme — ou DÉSARME — l'appartenance pour la
// durée du draw -> les primitives soumises pendant celui-ci sont capturées par Hooked_Insert.
// Save/restore = robuste au nesting. ⚠ Les deux branches doivent écrire l'état : cf. le désarmement
// ci-dessous, dont l'absence faisait hériter les effets d'ambiance de la map de l'id du joueur.
void __fastcall Hooked_EffRender(void* self, void* edx) {
  const bool  prev_owned = g_cur_owned;
  void* const prev_node  = g_cur_node;
  const bool  prev_via   = g_cur_via_effmgr;
  const int   prev_id    = g_cur_effect_id;
  const int   prev_em_id = g_cur_effmgr_id;
  const bool  prev_in_eff = g_cur_in_eff_render;
  const bool  prev_is_pl  = g_cur_eff_is_player;
  // Capturé par défaut (pas d'opt-in fonctionnel : ce serait un état à deux écrivains de plus).
  // g_effmgr_enabled est un interrupteur de DIAGNOSTIC (bissection d'une régression), pas un réglage.
  bool owned = false;
  int  em_id = -1;
  bool in_eff = false, is_player = false;
  if (g_effmgr_enabled && self) {
    __try {
      const int owner = *reinterpret_cast<int*>(reinterpret_cast<char*>(self) + kEffOwnerHnd);
      in_eff    = true;                                        // owner lu : le contexte est fiable
      is_player = (owner == *reinterpret_cast<int*>(rag::kOwnAccountIdAddr));
      if (is_player) {                                         // effet du JOUEUR ?
        // Exclut les .str name-based : ils relèvent d'un autre pipeline (billboard STR).
        // ⚠ Exclusion levable en DIAGNOSTIC : un effet qui rend en jeu sans être capturé peut très
        // bien être de cette famille, et c'est alors NOUS qui l'écartons avant de le voir.
        owned = g_capture_str || (*reinterpret_cast<uintptr_t*>(self) != kCEZ2STRVtbl);
        if (owned) em_id = *reinterpret_cast<int*>(reinterpret_cast<char*>(self) + kEffEffectId);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { owned = false; in_eff = false; is_player = false; }
  }
  g_cur_in_eff_render = in_eff;
  g_cur_eff_is_player = is_player;
  if (owned) {
    g_stats_cur.draws++;
    g_cur_owned = true;
    g_cur_node = self;
    g_cur_via_effmgr = true;             // position monde à +0x8 sur ce chemin
    g_cur_effmgr_id  = em_id;            // id brut de l'instance (+0x04), conservé pour diagnostic
    // ⚠ MESURÉ EN JEU (2026-07-19) : pour un costume, cette valeur EST l'id CONCRET (Permafrost
    // Oblivion, ordinal 248 -> 2429, identique à GetHatEffectID(248)). L'espace « < 0x98a » décrit
    // par la RE comme « effet générique » est donc exactement celui des ids concrets (491..2437).
    // On l'expose comme un effect_id normal : la famille CEffectMgr devient filtrable et
    // supprimable comme le chemin EZ, au lieu d'être un bloc anonyme.
    // ⚠ AUCUN plafond à 0x98a. J'avais posé ce garde-fou en pensant qu'au-delà on basculait dans
    // l'autre espace (`ordinal + 0x98a`) : FAUX en pratique. Les ids concrets dépassent 0x98a pour
    // les effets récents (mesuré : HAT_EF_C_2025RosFesta, ordinal 277 -> 2443 = 0x98b), et ils
    // étaient alors rejetés — d'où un effet absent de l'aperçu ET impossible à masquer sur le
    // personnage. L'espace `ordinal + 0x98a` ne nous parvient JAMAIS : il correspond aux hat effects
    // à `resourceFileName`, dont les instances sont des CEZ2STREffect, exclues plus haut par vtable.
    g_cur_effect_id  = (em_id > 0) ? em_id : -1;
  } else {
    // ⚠⚠ DÉSARMEMENT INCONDITIONNEL. Ce hook armait l'appartenance mais ne l'ÉTEIGNAIT jamais : une
    // instance NON possédée héritait de la fenêtre de l'appelant, et TOUTES ses primitives étaient
    // capturées avec l'effect_id d'un effet du JOUEUR. C'est ainsi que les nuages de map (CCloud,
    // ctor 0x006c9430, vtable 0x0100bf60, owner handle -1 -> jamais « à nous ») entraient dans la
    // capture sur gonryun : ils ne sortaient ensuite QUE par le garde-fou de proximité `max_r`, d'où
    // leur réapparition dès qu'on relâchait le rayon (un cache-misère, pas un filtre).
    // Symétrique de Hooked_EzDraw, qui écrit g_cur_owned dans les DEUX cas.
    g_cur_owned      = false;
    g_cur_node       = nullptr;
    g_cur_via_effmgr = false;
    g_cur_effect_id  = -1;
    g_cur_effmgr_id  = -1;
  }
  g_orig_eff_render(self, edx);
  g_cur_owned = prev_owned;
  g_cur_node  = prev_node;
  g_cur_via_effmgr = prev_via;
  g_cur_effect_id  = prev_id;
  g_cur_effmgr_id  = prev_em_id;
  g_cur_in_eff_render = prev_in_eff;
  g_cur_eff_is_player = prev_is_pl;
}

// Puits commun de toutes les primitives 2D. Frontière de frame -> vide le buffer.
void __fastcall Hooked_Insert(void* queue, void* edx, int* prim, unsigned flags) {
  // Appelant réel : notre hook est atteint par un JMP depuis le puits, donc l'adresse de retour est
  // bien celle du code du jeu qui soumet cette primitive.
  const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
  NoteCaller(ra);
  // Famille « hôte particule » : reconnue à la PROVENANCE, puisque aucun hook d'appartenance ne
  // couvre ce chemin. On la capture même si g_cur_owned est faux.
  // ⚠⚠ LA PROVENANCE SEULE NE SUFFIT PAS : ce call-site est PARTAGÉ avec les effets d'ambiance de la
  // map (nuages CCloud sur gonryun, ctor 0x006c9430, vtable 0x0100bf60, owner handle -1), qui
  // réutilisent tel quel le système de particules des .str. Capturés, ils étaient redessinés par le
  // doll, le portrait et l'aperçu — les surfaces qui incluent cette famille anonyme.
  // On exige donc que l'effet en cours de rendu soit celui du JOUEUR. Ce test porte sur l'OWNER, pas
  // sur l'appartenance : la famille « hôte particule » est faite de CEZ2STREffect, précisément exclus
  // de g_cur_owned par leur vtable — refuser sur `!g_cur_owned` supprimerait les vrais effets portés.
  // Hors de tout rendu d'effet (owner illisible), on conserve le comportement historique plutôt que
  // de perdre silencieusement une famille.
  const bool from_str_particle =
      (ra == kStrParticleRet) && (!g_cur_in_eff_render || g_cur_eff_is_player);
  __try {
    const uint32_t frame =
        *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(queue) + kQueueFrameOff);
    if (frame != g_last_frame) {
      g_last_frame = frame;
      g_stats_cur.captured = g_count;   // bilan de la frame qui vient de s'achever
      g_stats = g_stats_cur;
      g_stats_cur = Stats{};
      // Bascule des appelants, triés par nombre d'appels décroissant (tri par insertion, ≤24 entrées).
      for (int i = 1; i < g_caller_count_cur; ++i) {
        const Caller c = g_callers_cur[i];
        int j = i - 1;
        while (j >= 0 && g_callers_cur[j].count < c.count) { g_callers_cur[j + 1] = g_callers_cur[j]; --j; }
        g_callers_cur[j + 1] = c;
      }
      for (int i = 0; i < g_caller_count_cur; ++i) g_callers[i] = g_callers_cur[i];
      g_caller_count = g_caller_count_cur;
      g_caller_count_cur = 0;
      g_count = 0;
      RefreshOwnerActor();
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}

  bool captured = false;
  if ((g_cur_owned || from_str_particle) && prim) {
    g_stats_cur.inserts++;              // AVANT nos filtres de forme
    __try {
      g_queue = queue;
      // Ancre de CETTE primitive = position monde du nœud qui l'émet, relue à chaque insertion.
      // ⚠ L'offset DÉPEND du chemin : nœud EZ -> +0x10 ; instance CEffectMgr -> +0x8.
      // ⚠ Sur le chemin « hôte particule » il n'y a pas de nœud : pas d'ancre monde. Elle restera à
      // zéro et ProjectAnchor bascule alors sur le centre de la géométrie capturée.
      float node_world[3] = {0.0f, 0.0f, 0.0f};
      if (g_cur_node && !from_str_particle) {
        const int pos_off = g_cur_via_effmgr ? kEffWorldPos : kEzNodeWorldPos;
        const float* w =
            reinterpret_cast<const float*>(reinterpret_cast<char*>(g_cur_node) + pos_off);
        node_world[0] = w[0]; node_world[1] = w[1]; node_world[2] = w[2];
      }
      const char* p = reinterpret_cast<char*>(prim);
      const char* verts = *reinterpret_cast<char* const*>(p + kPrimVertsOff);
      const int vtx_count = *reinterpret_cast<const int*>(p + kPrimVtxCountOff);
      const int idx_count = *reinterpret_cast<const int*>(p + kPrimIdxCountOff);
      const int prim_type = *reinterpret_cast<const int*>(p + kPrimTypeOff);
      void* ctex = *reinterpret_cast<void* const*>(p + kPrimTexOff);
      const int off = g_imgui_dx7_active ? kCTexOffDX7 : kCTexOffDX9;
      void* native = ctex ? *reinterpret_cast<void**>(reinterpret_cast<char*>(ctex) + off) : nullptr;
      // On ne lit QUE les sommets réellement présents, et on ignore ce qu'on ne sait pas dessiner.
      const bool shape_ok =
          (prim_type == 4 && vtx_count == 3) || (prim_type == 5 && vtx_count == 4);
      if (!verts || !native || idx_count != 0 || !shape_ok) {
        // On garde la trace de CE qu'on refuse : sans ça, « le jeu émet mais rien ne s'affiche »
        // reste indiscernable d'un défaut de capture.
        g_stats_cur.rej_count++;
        g_stats_cur.rej_type = prim_type;
        g_stats_cur.rej_vtx  = vtx_count;
        g_stats_cur.rej_idx  = idx_count;
      }
      if (verts && native && idx_count == 0 && shape_ok && g_count < kCapMax) {
        Prim& q = g_caps[g_count++];
        q.tex = native;
        q.n = vtx_count;
        q.flags = flags;
        q.effect_id = from_str_particle ? kStrParticleId : g_cur_effect_id;
        q.effmgr_id = from_str_particle ? -1 : g_cur_effmgr_id;
        q.world[0] = node_world[0]; q.world[1] = node_world[1]; q.world[2] = node_world[2];
        q.src_blend = *reinterpret_cast<const int*>(p + kPrimSrcBlendOff);
        q.dst_blend = *reinterpret_cast<const int*>(p + kPrimDstBlendOff);
        for (int k = 0; k < vtx_count; ++k) {
          const float* vtx = reinterpret_cast<const float*>(verts + k * kPrimVertStride);
          q.x[k] = vtx[0]; q.y[k] = vtx[1];
          q.u[k] = vtx[6]; q.v[k] = vtx[7];
          q.argb[k] = *reinterpret_cast<const unsigned*>(verts + k * kPrimVertStride + 0x10);
        }
        captured = true;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
  }

  // Suppression in-world NOMINATIVE : on ne cesse de chaîner que pour les ids explicitement demandés.
  // ⚠ Ne jamais généraliser en « supprimer tout ce qui est capturé » : la capture couvre TOUS les
  // effets du joueur, et on ferait alors disparaître la barre d'icônes de statut avec.
  // ⚠ Utiliser l'id EFFECTIF : sur le chemin « hôte particule », g_cur_effect_id n'est pas armé,
  // c'est la provenance qui identifie la primitive.
  if (captured && IsSuppressed(from_str_particle ? kStrParticleId : g_cur_effect_id)) return;
  g_orig_insert(queue, edx, prim, flags);
}

// D3DCOLOR (0xAARRGGBB) -> IM_COL32 (0xAABBGGRR) : échange R<->B (A et G en place).
inline unsigned ToImCol(unsigned c) {
  return (c & 0xff00ff00u) | ((c & 0x00ff0000u) >> 16) | ((c & 0x000000ffu) << 16);
}

}  // namespace

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
  g_orig_eff_render = reinterpret_cast<EffRenderFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kEffRenderFn),
          reinterpret_cast<uint8_t*>(&Hooked_EffRender)));
  RefreshOwnerActor();
}

void SetEffMgrCaptureEnabled(bool enable) { g_effmgr_enabled = enable; }

void SetCaptureStrEffects(bool enable) { g_capture_str = enable; }

void SetSuppressedIds(int slot, const int* ids, int count) {
  if (slot < 0 || slot >= kSuppressSlotCount) return;
  if (count < 0) count = 0;
  if (count > kMaxSuppressedPerSlot) count = kMaxSuppressedPerSlot;
  if (!ids) count = 0;
  for (int i = 0; i < count; ++i) g_suppressed_ids[slot][i] = ids[i];
  g_suppressed_count[slot] = count;
}

const Prim* Prims() { return g_caps; }
int         Count() { return g_count < kCapMax ? g_count : kCapMax; }

// Vrai si cette primitive passe le filtre du consommateur. Règle UNIQUE, partagée par le dessin,
// la projection de l'ancre et la mesure (cf. Matches) : les trois doivent voir le même sous-ensemble.
bool Wanted(const Prim& q, const DrawOpts& o) {
  if (q.effect_id == kStrParticleId) return o.include_str_particle;  // famille anonyme
  if (q.effect_id < 0) {                          // famille CEffectMgr : filtrer sur l'id d'INSTANCE
    if (o.effmgr_id_count > 0 && o.effmgr_ids) {
      for (int k = 0; k < o.effmgr_id_count; ++k)
        if (o.effmgr_ids[k] == q.effmgr_id) return true;
      return false;
    }
    return o.include_effmgr;                      // repli : tout ou rien
  }
  // ⚠⚠ AUCUN id demandé = RIEN, jamais « tout ». L'ancienne règle (« pas de filtre -> tout le
  // capturé ») transformait un consommateur qui n'a rien à montrer en consommateur qui montre TOUT :
  // sans hat effect équipé, le doll et le portrait redessinaient l'intégralité du tampon — sur
  // gonryun, 480 primitives de nuages d'ambiance (effect_id 230) par-dessus l'écran. Un consommateur
  // qui veut une famille entière le dit explicitement (include_effmgr / include_str_particle).
  if (o.id_count <= 0 || !o.ids) return false;
  for (int k = 0; k < o.id_count; ++k)
    if (o.ids[k] == q.effect_id) return true;
  return false;
}

bool ProjectAnchor(const DrawOpts& o, float* ax, float* ay, float* screen_scale) {
  const int count = g_count < kCapMax ? g_count : kCapMax;
  if (count <= 0 || !g_queue) return false;
  // Ancre = celle de la PREMIÈRE primitive qui passe le filtre : projeter la position d'un autre
  // effet du joueur donnerait un point sans rapport, et tout le rendu partirait hors écran.
  const Prim* anchor_prim = nullptr;
  for (int i = 0; i < count; ++i)
    if (Wanted(g_caps[i], o)) { anchor_prim = &g_caps[i]; break; }
  if (!anchor_prim) return false;

  // Certaines familles n'ont PAS de position monde (rendu délégué à un hôte particule : aucun nœud
  // à interroger). On prend alors celle de l'ACTEUR : un acteur dérive du même type de nœud qu'un
  // effet, sa position est donc au même offset. C'est une ancre STABLE, et elle fournit une vraie
  // échelle de profondeur.
  // ⚠ Ne PAS dériver l'ancre de la géométrie capturée : elle est ANIMÉE, la bbox bouge à chaque
  // frame, et l'effet se recale en permanence -> rendu saccadé et échelle fausse (constaté).
  bool use_actor_world = (anchor_prim->world[0] == 0.0f && anchor_prim->world[1] == 0.0f &&
                          anchor_prim->world[2] == 0.0f);
  float actor_world[3] = {0.0f, 0.0f, 0.0f};
  if (use_actor_world && g_owner_actor) {
    __try {
      const float* w =
          reinterpret_cast<const float*>(reinterpret_cast<char*>(g_owner_actor) + kEzNodeWorldPos);
      actor_world[0] = w[0]; actor_world[1] = w[1]; actor_world[2] = w[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) { actor_world[0] = actor_world[1] = actor_world[2] = 0.0f; }
  }
  const bool actor_world_ok =
      (actor_world[0] != 0.0f || actor_world[1] != 0.0f || actor_world[2] != 0.0f);

  // Dernier recours seulement : aucune position monde nulle part -> bas-centre de la géométrie.
  if (use_actor_world && !actor_world_ok) {
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < count; ++i) {
      if (!Wanted(g_caps[i], o)) continue;
      for (int k = 0; k < g_caps[i].n; ++k) {
        const float vx = g_caps[i].x[k], vy = g_caps[i].y[k];
        if (vx < x0) x0 = vx; if (vx > x1) x1 = vx;
        if (vy < y0) y0 = vy; if (vy > y1) y1 = vy;
      }
    }
    if (x1 < x0) return false;
    // ⚠ ANCRE = BAS-CENTRE, pas centre : le natif ancre un effet sur l'ORIGINE de l'acteur (ses
    // pieds). Prendre le centre de la géométrie décalait tout verticalement de la moitié de la
    // hauteur de l'effet.
    if (ax) *ax = (x0 + x1) * 0.5f;
    if (ay) *ay = y1;
    // Échelle : aucune profondeur disponible ici. On réutilise la dernière échelle valide connue
    // (celle d'un effet du joueur, donc à la bonne profondeur) plutôt que 1.0, qui donnerait un
    // facteur de redimensionnement absurde côté doll.
    if (screen_scale) *screen_scale = (g_last_screen_scale > 1e-4f) ? g_last_screen_scale : 1.0f;
    return true;
  }

  float x = 0.0f, y = 0.0f, inv_w = 0.0f, s = 0.0f;
  bool ok = false;
  __try {
    void* gm = reinterpret_cast<void*(__fastcall*)(int)>(rag::kModeMgrGetActiveAddr)(static_cast<int>(rag::kModeMgrAddr));
    void* cam = gm ? *reinterpret_cast<void**>(reinterpret_cast<char*>(gm) + kOffCamera) : nullptr;
    float* view =
        cam ? reinterpret_cast<float*>(reinterpret_cast<char*>(cam) + kOffViewMtx) : nullptr;
    if (view) {
      // ⚠⚠ COPIE OBLIGATOIRE : le natif ÉCRIT dans le vecteur monde qu'on lui passe (transformation
      // en place). Lui donner directement la position stockée la corromprait, et cette fonction est
      // appelée plusieurs fois par frame depuis la mutualisation (lab + doll × 2 passes de z-order).
      float world[3] = { use_actor_world ? actor_world[0] : anchor_prim->world[0],
                         use_actor_world ? actor_world[1] : anchor_prim->world[1],
                         use_actor_world ? actor_world[2] : anchor_prim->world[2] };
      reinterpret_cast<void(__fastcall*)(void*, void*, float*, float*, float*, float*, float*)>(
          kSceneProject)(g_queue, nullptr, world, view, &x, &y, &inv_w);
      s = reinterpret_cast<float(__fastcall*)(void*, void*, float)>(kDepthScale)(
          g_queue, nullptr, inv_w);
      ok = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  if (!ok) return false;
  if (s > 1e-4f) g_last_screen_scale = s;   // mémorisée pour les familles sans position monde
  if (ax) *ax = x;
  if (ay) *ay = y;
  if (screen_scale) *screen_scale = s;
  return true;
}

void Draw(ImDrawList* dl, const DrawOpts& o) {
  if (!dl) return;
  const int count = Count();
  if (count <= 0) return;
  float ax = 0.0f, ay = 0.0f;
  if (!ProjectAnchor(o, &ax, &ay, nullptr)) return;   // ancre de L'EFFET FILTRÉ, pas d'un autre

  const float r2 = o.max_r > 0.0f ? o.max_r * o.max_r : 0.0f;

  if (o.blend_mode == 2)  // additif GLOBAL : comparaison seulement
    dl->AddCallback(reinterpret_cast<ImDrawCallback>(D3D9_AdditiveBlendCallback()), nullptr);

  void* cur_tex = nullptr;
  bool  tex_pushed = false;
  int   cur_src = -1, cur_dst = -1;
  bool  blend_cb = false;

  for (int i = 0; i < count; ++i) {
    const Prim& q = g_caps[i];
    if (!q.tex) continue;
    if (!Wanted(q, o)) continue;   // le consommateur filtre ICI ce qui le concerne
    // Z-order par primitive : le bit 0x8 des flags = « rendu avant le personnage » (donc derrière).
    if (o.use_zorder && (((q.flags & 0x8u) != 0) != o.draw_behind)) continue;
    if (r2 > 0.0f) {  // garde-fou : `!(d2 < r2)` attrape aussi NaN/inf
      bool far_out = false;
      for (int k = 0; k < q.n; ++k) {
        const float dx = q.x[k] - ax, dy = q.y[k] - ay;
        if (!(dx * dx + dy * dy < r2)) { far_out = true; break; }
      }
      if (far_out) continue;
    }
    // BLEND NATIF PAR PRIMITIVE : on rejoue les facteurs que le jeu a posés pour CETTE primitive.
    // Callback émis seulement quand le couple CHANGE, sinon on casse le batch à chaque primitive.
    // ⚠ Agit sur le device D3D9 : inopérant sous DX7 (repli = alpha normal d'ImGui).
    if (o.blend_mode == 0 && (q.src_blend != cur_src || q.dst_blend != cur_dst)) {
      cur_src = q.src_blend;
      cur_dst = q.dst_blend;
      if (cur_src > 0 && cur_src < 256 && cur_dst > 0 && cur_dst < 256) {
        const uintptr_t data = static_cast<uintptr_t>((cur_dst & 0xff) << 8) |
                               static_cast<uintptr_t>(cur_src & 0xff);
        dl->AddCallback(reinterpret_cast<ImDrawCallback>(D3D9_ExplicitBlendCallback()),
                        reinterpret_cast<void*>(data));
        blend_cb = true;
      }
    }
    if (q.tex != cur_tex) {  // 1 draw call par texture, pas par primitive
      if (tex_pushed) dl->PopTexture();
      dl->PushTexture((ImTextureID)(uintptr_t)q.tex);
      cur_tex = q.tex;
      tex_pushed = true;
    }
    auto at = [&](int k) {
      return ImVec2(o.ox + (q.x[k] - ax) * o.scale, o.oy + (q.y[k] - ay) * o.scale);
    };
    // ⚠ COULEURS PAR SOMMET (gouraud) : une couleur unique rendrait opaques les traînées à
    // dégradé d'alpha, qui doivent s'estomper.
    if (q.n == 3) {
      dl->PrimReserve(3, 3);
      const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
      dl->PrimWriteIdx(base); dl->PrimWriteIdx(base + 1); dl->PrimWriteIdx(base + 2);
      for (int k = 0; k < 3; ++k)
        dl->PrimWriteVtx(at(k), ImVec2(q.u[k], q.v[k]), ToImCol(q.argb[k]));
    } else {
      // TRIANGLESTRIP 4 sommets : v0=HG v1=HD v2=BG v3=BD -> ordre horaire v0,v1,v3,v2.
      dl->PrimReserve(6, 4);
      const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
      dl->PrimWriteIdx(base); dl->PrimWriteIdx(base + 1); dl->PrimWriteIdx(base + 2);
      dl->PrimWriteIdx(base); dl->PrimWriteIdx(base + 2); dl->PrimWriteIdx(base + 3);
      const int order[4] = {0, 1, 3, 2};
      for (int k = 0; k < 4; ++k) {
        const int s = order[k];
        dl->PrimWriteVtx(at(s), ImVec2(q.u[s], q.v[s]), ToImCol(q.argb[s]));
      }
    }
  }
  if (tex_pushed) dl->PopTexture();
  // Restaure l'état ImGui dès qu'on y a touché, sinon toute l'UI dessinée ensuite hérite du blend.
  if (o.blend_mode == 2 || blend_cb)
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

bool Matches(const Prim& prim, const DrawOpts& opts) { return Wanted(prim, opts); }

Stats LastFrameStats() { return g_stats; }

int           CallerCount() { return g_caller_count; }
const Caller* Callers()     { return g_callers; }

bool EffectIdIsImplemented(int concrete_id) {
  if (concrete_id < kEffectJumpBase || concrete_id >= kEffectJumpBase + kEffectJumpCount)
    return false;
  bool impl = false;
  __try {
    const uintptr_t* tbl = reinterpret_cast<const uintptr_t*>(kEffectJumpTable);
    impl = tbl[concrete_id - kEffectJumpBase] != kEffectJumpDefault;
  } __except (EXCEPTION_EXECUTE_HANDLER) { impl = false; }
  return impl;
}

}  // namespace ez_capture
