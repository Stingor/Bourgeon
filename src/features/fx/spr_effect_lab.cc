#include "ragnarok/lua.h"
#include "ragnarok/globals.h"
#include "features/fx/spr_effect_lab.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "imgui.h"
#include "bourgeon.h"        // Bourgeon::Instance().IsMapLoading() / IsGameActive() (gate anti-crash warp)
#include "features/fx/ez_effect_capture.h"  // capture EZ PARTAGÉE (hooks, blend par primitive, rendu ré-ancré)
#include "features/moonlight_ui/moonlight_ui.h"  // ColorEdit4WithAlphaBar() (helper standardisé)
#include "utils/hooking/hook_manager.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// Backend actif (DX9 vs DX7) — le « Sol uni » n'existe que sur le chemin de rendu DX9.
// Défini ailleurs.
extern bool g_imgui_dx7_active;

namespace spr_lab {
namespace {

// ── Adresses / offsets natifs (client 20250716, base 0x400000) ────────────────
// Réutilisés à l'identique de la RE existante (cf. basic_info.cc, docs/hat_effect_re.md).
constexpr int       kOffActorMgr      = 0xcc;        // CMode -> actorMgr
constexpr int       kOffOwnActor      = 0x2c;        // actorMgr -> acteur joueur

constexpr uintptr_t kToggleEffectId   = 0x00c44940;  // Actor_ToggleEffectId(actor, unifiedId, add) __thiscall
constexpr int       kHatOrdinalBase   = 0x98a;       // id unifié d'un hat effect = ordinal + 0x98a

// Bridge Lua brut (Lua 5.1) — pour appeler GetHatEffectID(ordinal) = getter NATIF de l'id concret
// (ordinal -> id d'effet interne). Mêmes adresses/mécanique que basic_info.cc (HatLuaNum) :
// double-deref de g_pLuaStateMgr, lua_checkstack AVANT push (le natif le fait), résultat via
// lua_tonumber. Évite tout hardcode de l'id concret (cf. règle « jamais hardcoder, appeler le natif »).

// ── Cas de test par défaut : Digital_Space ────────────────────────────────────
// Ordinal 87 (HatEffectIDs.lub) -> on SPAWN via Actor_ToggleEffectId(actor, 87+0x98a, 1).
// L'id CONCRET (Digital_Space = 1240) n'est PAS hardcodé : on le RÉSOUT via le getter NATIF
// GetHatEffectID(ordinal) (comme basic_info). On CAPTURE en matchant le nœud primitif sur cet id.
int  g_ui_ordinal        = 87;    // ordinal à spawner (SEUL champ saisi)
int  g_resolved_concrete = 0;     // aperçu : GetHatEffectID(g_ui_ordinal) (affichage UI)
bool g_suppress          = true;  // true = ne PAS dessiner l'effet en jeu (overlay seul)
// Mode de BLEND. Un mode global ne peut PAS marcher : un même effet mélange des primitives additives
// et des primitives en alpha dans la même frame. En alpha, les primitives que le natif dessine en
// additif apparaissent en CARRÉS NOIRS (du noir additif = invisible, du noir alpha = du noir) ; en
// additif global, à l'inverse, les primitives alpha CRAMENT en blanc. La bonne réponse est le blend
// RÉEL de chaque primitive, lu dans l'enregistrement (+0x18/+0x1c) et rejoué via
// D3D9_ExplicitBlendCallback (UserCallbackData : octet bas = SRCBLEND, octet suivant = DESTBLEND).
// 0 = natif par primitive (défaut, correct) | 1 = alpha normal | 2 = additif global (comparaison)
int g_blend_mode         = 0;
bool g_debug             = false; // overlay diagnostic : capture BRUTE + ancre + rayon (in situ)
bool  g_dbg_proj_ok      = false; // dernière ancre : true = projection native disponible
float g_dbg_ax = 0.0f, g_dbg_ay = 0.0f;  // dernière ancre écran (diagnostic UI)
// Garde-fou VOLONTAIREMENT LARGE : les « traînées » sont de la géométrie LÉGITIME (le natif les rend
// en additif + dégradé d'alpha, donc discrètes) — c'est notre RENDU qu'on a corrigé, pas la géométrie.
// Ce filtre ne sert plus qu'à écarter les cas francs (NaN/inf). À baisser seulement si un effet
// précis déborde encore.
float g_max_r            = 2000.0f; // rayon (px) max d'un sommet vs l'ancre (attrape aussi NaN/inf)

// ── État de spawn (reconcile persistant : SURVIT au changement de map/@refresh) ──
int   g_wanted_ordinal    = 0;        // ordinal voulu (0 = éteint)
int   g_applied_ordinal   = 0;        // ordinal appliqué sur l'acteur COURANT
int   g_applied_concrete  = 0;        // id concret de l'effet spawné (= GetHatEffectID(applied)) — match capture
void* g_applied_actor     = nullptr;  // acteur sur lequel on a appliqué. ⚠⚠ COMPARAISON SEULE : au warp cet
                                      // acteur est LIBÉRÉ (operator_delete) -> le déréférencer = USE-AFTER-FREE.
// ⚠ PAS de détection de perte par « N frames sans capture » : une telle branche re-togglait l'effet
// sur le MÊME acteur, or le remove natif ne nettoie pas les nœuds orphelins -> un nœud tické de plus
// à chaque fois, donc une animation qui accélère (2×, 3×…). La perte se détecte UNIQUEMENT par le
// changement de pointeur d'acteur (au warp, il change toujours).

// ── Helpers ───────────────────────────────────────────────────────────────────
// La capture contient désormais TOUS les effets de l'acteur (chaque primitive porte son
// `effect_id`) : tout ce que le lab COMPTE ou DESSINE doit donc être filtré sur NOTRE id, sinon
// les chiffres et les tracés incluent les autres effets du joueur.
bool IsOurs(const ez_capture::Prim& p) {
  return g_applied_concrete > 0 && p.effect_id == g_applied_concrete;
}

void* GetOwnActor() {
  void* actor = nullptr;
  __try {
    void* gm = reinterpret_cast<void*(__fastcall*)(int)>(rag::kModeMgrGetActiveAddr)(static_cast<int>(rag::kModeMgrAddr));
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
    void* L = lua::State();
    if (L) {
      lua::CheckStack(L, 3);
      lua::GetField(
          L, lua::kGlobalsIndex, "GetHatEffectID");
      lua::PushNumber(L, static_cast<double>(ordinal));
      if (lua::PCall(L, 1, 1, 0) == 0)
        r = static_cast<int>(lua::ToNumber(L, -1));
      lua::SetTop(L, -2);
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
    void* L = lua::State();
    if (L) {
      lua::CheckStack(L, 3);
      lua::GetField(
          L, lua::kGlobalsIndex, "GetHatEfResName");
      lua::PushNumber(L, static_cast<double>(ordinal));
      if (lua::PCall(L, 1, 1, 0) == 0) {
        const char* s = lua::ToLString(
            L, -1, nullptr);
        if (s && s[0]) { std::strncpy(out, s, cap - 1); out[cap - 1] = '\0'; }
      }
      lua::SetTop(L, -2);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

// ── Catalogue des effets .spr/EZ (scan NATIF, pas de liste hardcodée) ──────────
// On balaie les ordinaux et on interroge le client lui-même : GetHatEffectID(ord) > 0 => c'est un
// effet EZ/hatEffectID (ce que CE lab sait rendre) ; on note aussi GetHatEfResName (indicatif).
// Construit à la demande (bouton), une fois, en jeu (Lua prêt). ~500 pcalls -> quelques ms.
// kind : 0 = INERTE (ne rend rien, même nativement) | 1 = EZ procédural (rendu ET capturé par ce lab)
//        2 = .str (rend NATIVEMENT via le pipeline billboard STR — autre chemin que EzEffect_Draw,
//            donc visible en jeu mais a priori PAS capturé par ce lab).
struct HatEntry { int ord; int cid; char res[48]; int kind; };
std::vector<HatEntry> g_catalog;
bool g_catalog_built = false;
int  g_catalog_impl = 0;   // nombre d'entrées réellement implémentées
constexpr int kMaxOrdinalScan = 512;

void BuildCatalog() {
  g_catalog.clear();
  g_catalog_impl = 0;
  for (int ord = 1; ord < kMaxOrdinalScan; ++ord) {
    const int cid = ResolveConcreteId(ord);
    char res[48];
    ResolveResName(ord, res, sizeof(res));
    if (cid > 0) {                      // effet EZ/hatEffectID = rendable par ce lab
      HatEntry e;
      e.ord = ord; e.cid = cid;
      // ⚠ Un ordinal rend s'il a une entrée PROCÉDURALE **OU** un resourceFileName (.str) — ne tester
      // que la table procédurale marquait « inerte » des effets qui fonctionnent parfaitement.
      // Le nom de ressource vient du getter NATIF GetHatEfResName (pas d'heuristique de notre cru).
      if (ez_capture::EffectIdIsImplemented(cid)) e.kind = 1;
      else if (res[0]) e.kind = 2;
      else e.kind = 0;
      if (e.kind != 0) g_catalog_impl++;
      std::strncpy(e.res, res, sizeof(e.res) - 1); e.res[sizeof(e.res) - 1] = '\0';
      g_catalog.push_back(e);
    }
  }
  g_catalog_built = true;
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
    g_applied_ordinal = g_wanted_ordinal;
    g_applied_actor   = actor;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_applied_ordinal = g_wanted_ordinal;
    g_applied_actor   = actor;
  }

  // Ciblage du module de capture. ⚠ L'acteur n'est transmis que pour être COMPARÉ (le module ne
  // Rien à déclarer au module : il résout lui-même l'acteur joueur et capture tout ce qui lui
  // appartient, en étiquetant chaque primitive (Prim::effect_id). C'est NOUS qui filtrons, au dessin
  // (DrawOpts::ids) et à la suppression (SetSuppressedIds).
  // ⚠ NE PAS réintroduire un réglage de capture partagé : le doll l'armait et le lab l'effaçait à
  // chaque frame, d'où une capture qui ne tenait qu'une ou deux frames sur deux.
}

// ── Overlay : dessine les primitives capturées AU CENTRE de l'écran ───────────
// Tout le travail (capture, ancre, blend par primitive) vit dans ez_capture : ici on ne fait que
// choisir la destination (centre écran) et l'échelle (1 = taille native).
void DrawCenteredOverlay() {
  const int count = ez_capture::Count();
  if (count <= 0) return;
  // ⚠⚠ RIEN DE SPAWNÉ = RIEN À DESSINER. Sans ce garde, `opts.id_count` tombe à 0 et la règle de
  // filtrage du module (« pas de filtre -> tout le capturé ») fait dessiner TOUT le tampon : sur
  // gonryun on redessinait ainsi les nuages d'ambiance de la map par-dessus l'écran, et seul le
  // garde-fou de proximité `max_r` les retenait — d'où leur apparition quand on montait le Rayon.
  // Le lab montre UN effet, désigné par son id : « aucun id » signifie « rien », jamais « tout ».
  if (g_applied_concrete <= 0) return;
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  if (disp.x <= 0 || disp.y <= 0) return;

  // Options de dessin : elles portent AUSSI le filtre, et l'ancre doit être celle de NOTRE effet —
  // demander l'ancre sans filtre projetait la position d'un autre effet du joueur (ancre figée hors
  // écran, plus rien de dessiné).
  ez_capture::DrawOpts opts;
  opts.ox         = disp.x * 0.5f;
  opts.oy         = disp.y * 0.5f;
  opts.scale      = 1.0f;          // 1 = taille native, juste recentré
  opts.blend_mode = g_blend_mode;
  opts.use_zorder = false;         // overlay : une seule passe, pas de tri autour d'un sprite
  opts.max_r      = g_max_r;
  opts.ids        = &g_applied_concrete;
  opts.id_count   = (g_applied_concrete > 0) ? 1 : 0;
  opts.include_effmgr = false;     // le lab ne vise qu'un effet précis, par id

  // Ancre ÉCRAN (projection de la position monde du nœud) : sert au diagnostic affiché dans l'UI,
  // et au tracé de l'overlay debug. ez_capture::Draw la recalcule de la même façon.
  float ax = 0.0f, ay = 0.0f;
  const bool have_anchor = ez_capture::ProjectAnchor(opts, &ax, &ay, nullptr);
  g_dbg_proj_ok = have_anchor; g_dbg_ax = ax; g_dbg_ay = ay;

  // MODE DEBUG : dessine la capture BRUTE (positions écran NATIVES, sans reproject) + l'ancre et le
  // cercle du rayon. À utiliser avec « Cacher en jeu » DÉCOCHÉ : nos contours de primitives se
  // superposent alors à l'effet natif in-world -> on voit si l'ancre tombe au bon endroit.
  if (g_debug) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();  // au-dessus de tout
    if (have_anchor) {
      dl->AddCircleFilled(ImVec2(ax, ay), 5.0f, IM_COL32(0, 255, 255, 255));         // ancre
      dl->AddCircle(ImVec2(ax, ay), g_max_r, IM_COL32(0, 255, 255, 120), 64, 1.5f);  // rayon du filtre
    }
    const ez_capture::Prim* prims = ez_capture::Prims();
    const ImU32 col = IM_COL32(60, 255, 60, 220);
    for (int i = 0; i < count; ++i) {
      const ez_capture::Prim& q = prims[i];
      if (!IsOurs(q)) continue;  // ne pas tracer les contours des autres effets du joueur
      if (q.n == 3)
        dl->AddTriangle(ImVec2(q.x[0], q.y[0]), ImVec2(q.x[1], q.y[1]),
                        ImVec2(q.x[2], q.y[2]), col, 1.5f);
      else
        dl->AddQuad(ImVec2(q.x[0], q.y[0]), ImVec2(q.x[1], q.y[1]),
                    ImVec2(q.x[3], q.y[3]), ImVec2(q.x[2], q.y[2]), col, 1.5f);
    }
    return;  // en debug on ne dessine PAS l'overlay reprojeté
  }

  // `opts` a été rempli plus haut (l'ancre en dépend). ⚠ `&g_applied_concrete` doit rester valide
  // pendant Draw : c'est une globale, donc c'est bon.
  ez_capture::Draw(ImGui::GetForegroundDrawList(), opts);
}

// ── Sol uni (fond de capture) ─────────────────────────────────────────────────
// Repeint TOUT le terrain .gnd d'une couleur unie réglable, pour isoler un effet
// sur un fond neutre lors d'une capture d'écran.
//
// ⚠⚠ PIÈGE MAJEUR (constaté en live 2026-07-18) : le client a DEUX chemins de rendu
// complets et parallèles. La famille 0x00552xxx (World_RenderScene 0x00552fa0,
// World_DrawGroundTiles 0x00552710, World_DrawWaterSurface 0x00552b70) est le chemin
// DX7 et n'est JAMAIS exécutée en DX9 : un JMP hook posé sur 0x00552710 s'installe
// correctement (trampoline non nul) mais ne se déclenche pas une seule fois.
//
// Le chemin DX9 réellement emprunté :
//   RendererDX9_RenderScene 0x0055ca60  (BeginScene / EndScene)
//     -> RendererDX9_FlushWorldScene 0x0055e5b0   (jumeau de World_RenderScene)
//          -> RendererDX9_DrawGroundTiles 0x0055d680   <-- LE SOL, notre cible
//          -> FUN_0055d850                              (l'eau)
//     -> FUN_005511d0  (RESET des buckets, PAS un flush — ne pas s'y tromper)
//
// MÊME objet et MÊMES listes que la version DX7 (param_1 est un int*, donc [n] =
// octet n*4) : [0x57]/[0x58] = +0x15c/+0x160 (sol), [0x6c]/[0x6d] = +0x1b0/+0x1b4
// (lightmap). Deux boucles, SetTexture puis DrawPrimRecord — identique.
//
// La fonction du sol ne touche NI COLOROP NI COLORARG1 : elle ne pose que l'adressage
// (FUN_005486e0(w, 0, 1, v) / (w, 0, 2, v) = ADDRESSU/ADDRESSV, v=3 CLAMP pour le sol,
// v=1 WRAP pour la lightmap — l'équivalent DX9 du SetTextureStageState(0, 0xc, v) =
// D3DTSS_ADDRESS du chemin DX7). On peut donc simplement ENCADRER l'appel original :
// stage 0 en COLOROP = SELECTARG1 / COLORARG1 = TFACTOR + TEXTUREFACTOR = notre
// couleur. Les deux couches sortent unies, la géométrie et le z-buffer restent
// intacts (l'occlusion par le terrain reste correcte), et on restaure MODULATE/
// TEXTURE en sortie car le stage 0 sert à TOUTE la suite de la scène.
constexpr uintptr_t kDX9DrawGround = 0x0055d680;  // RendererDX9_DrawGroundTiles(this = renderer DX9)

// On tape DIRECTEMENT sur le vrai IDirect3DDevice9, pas sur le wrapper d'état en
// +0x25c : les slots de vtable du wrapper (+0x50/+0x8c/+0x94) venaient de commentaires
// Ghidra déjà pris en défaut, et une sonde a montré que +0x50 tombe sur une fonction
// qui charge scr_logo.bmp.
//
// PREUVE que +0x260 est le device COM brut (RendererDX9_DrawPrimRecord 0x0055c830,
// = vtable renderer 0x00fd6d64 slot +0x38, appelée ici même via (**(*param_1+0x38))) :
//   piVar2 = *(this + 0x260);  (**(piVar2 + 0x14c))(...)  = DrawPrimitiveUP
// 0x14c / 4 = 83 = l'index EXACT de DrawPrimitiveUP dans IDirect3DDevice9 -> c'est
// bien la vtable D3D9 standard, donc les index 57 et 67 ci-dessous sont sûrs.
constexpr int kOffD3D9Device = 0x260;   // renderer DX9 -> IDirect3DDevice9*
constexpr int kVtSetRenderState      = 57 * 4;  // 0x0e4
constexpr int kVtSetTextureStageState = 67 * 4;  // 0x10c

constexpr unsigned kD3DRS_TEXTUREFACTOR = 60;
constexpr unsigned kD3DTSS_COLOROP      = 1;
constexpr unsigned kD3DTSS_COLORARG1    = 2;
constexpr unsigned kD3DTOP_SELECTARG1   = 2;
constexpr unsigned kD3DTOP_MODULATE     = 4;
constexpr unsigned kD3DTA_TEXTURE       = 2;
constexpr unsigned kD3DTA_TFACTOR       = 3;

constexpr uintptr_t kDX9DrawPrimRec  = 0x0055c830;  // vtbl +0x38 : draw mono-texture
constexpr uintptr_t kDX9DrawPrimDual = 0x0055c8c0;  // vtbl +0x34 : draw BI-texture (aniso)
constexpr uintptr_t kDX9DrawTerrain  = 0x0055d850;  // RendererDX9_DrawTerrainSurfaces

using DrawGroundFn = void(__fastcall*)(void*);
DrawGroundFn g_orig_draw_ground = nullptr;

// > 0 pendant l'exécution de RendererDX9_DrawGroundTiles : c'est ce qui distingue les
// primitives du SOL de toutes les autres dans Hooked_DrawPrimRecord (mono-thread rendu).
int g_in_ground_pass = 0;

bool  g_ground_paint    = false;
float g_ground_col[4]   = {0.0f, 0.0f, 0.0f, 1.0f};  // noir opaque par défaut

// NOTE : RendererDX9_DrawGroundTiles 0x0055d680 s'exécute avec ses deux listes VIDES
// (mesuré en live). On la garde hookée par sécurité — si une carte l'utilisait, la
// couleur s'appliquerait aussi — mais le terrain réel passe par
// RendererDX9_DrawTerrainSurfaces (cf. Hooked_DrawTerrain).
void __fastcall Hooked_DrawGround(void* self) {
  ++g_in_ground_pass;
  if (g_orig_draw_ground) g_orig_draw_ground(self);
  --g_in_ground_pass;
}

// ── Le VRAI point d'injection : juste avant le DrawPrimitiveUP ────────────────
// Poser COLOROP/COLORARG1 autour de l'appel à RendererDX9_DrawGroundTiles NE MARCHE
// PAS, alors que les appels renvoient pourtant S_OK. Raison : l'objet en renderer+0x25c
// est un CACHE D'ÉTATS (FUN_00547990 recopie 0xd2 dwords depuis wrapper+8 = render
// states, 0x42 depuis wrapper+0x350 = texture stage states, 0x1c depuis wrapper+0x458).
// Nos écritures vont directement sur le device COM et court-circuitent ce cache : tout
// re-push des valeurs cachées entre notre écriture et le draw réel écrase notre COLOROP,
// silencieusement.
//
// RendererDX9_DrawPrimRecord 0x0055c830 est le dernier maillon : il fait
// Device_SetFVFCached puis appelle DrawPrimitiveUP (vtbl +0x14c) SUR LE DEVICE BRUT
// (this+0x260), sans aucun flush d'état entre les deux. Écrire juste avant l'original
// est donc inattaquable — plus rien ne peut s'intercaler.
//
// __thiscall(this = renderer, rec) -> émulé en __fastcall (edx ignoré).
using DrawPrimRecFn = void(__fastcall*)(void*, void*, void*);
DrawPrimRecFn g_orig_draw_prim = nullptr;

// Pose la couleur unie sur le stage 0, appelle le draw natif, puis restaure. Le stage 0
// sert à TOUTE la suite de la scène : on remet exactement les valeurs que le cache du
// wrapper croit actives (MODULATE / TEXTURE), donc cache et device restent cohérents.
void PaintAroundDraw(void* self, void* edx, void* rec, DrawPrimRecFn orig) {
  __try {
    void* dev = *reinterpret_cast<void**>(reinterpret_cast<char*>(self) + kOffD3D9Device);
    if (!dev) {
      if (orig) orig(self, edx, rec);
      return;
    }
    void** vt = *reinterpret_cast<void***>(dev);
    // Méthodes COM : __stdcall, `this` en 1er argument sur la pile.
    auto SetRS = reinterpret_cast<long(__stdcall*)(void*, unsigned, unsigned)>(
        vt[kVtSetRenderState / sizeof(void*)]);
    auto SetTSS = reinterpret_cast<long(__stdcall*)(void*, unsigned, unsigned, unsigned)>(
        vt[kVtSetTextureStageState / sizeof(void*)]);

    auto ch = [](float v) -> unsigned {
      int i = static_cast<int>(v * 255.0f + 0.5f);
      return static_cast<unsigned>(i < 0 ? 0 : (i > 255 ? 255 : i));
    };
    const unsigned argb = (ch(g_ground_col[3]) << 24) | (ch(g_ground_col[0]) << 16) |
                          (ch(g_ground_col[1]) << 8) | ch(g_ground_col[2]);

    SetRS(dev, kD3DRS_TEXTUREFACTOR, argb);
    SetTSS(dev, 0, kD3DTSS_COLORARG1, kD3DTA_TFACTOR);
    SetTSS(dev, 0, kD3DTSS_COLOROP, kD3DTOP_SELECTARG1);
    if (orig) orig(self, edx, rec);
    SetTSS(dev, 0, kD3DTSS_COLOROP, kD3DTOP_MODULATE);
    SetTSS(dev, 0, kD3DTSS_COLORARG1, kD3DTA_TEXTURE);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void __fastcall Hooked_DrawPrimRecord(void* self, void* edx, void* rec) {
  if (g_in_ground_pass <= 0 || !g_ground_paint || g_imgui_dx7_active || !self) {
    if (g_orig_draw_prim) g_orig_draw_prim(self, edx, rec);
    return;
  }
  PaintAroundDraw(self, edx, rec, g_orig_draw_prim);
}

// Jumeau BI-TEXTURE : vtbl +0x34 = RendererDX9_DrawPrimRecordDualTex 0x0055c8c0.
// La branche aniso de RendererDX9_DrawTerrainSurfaces dessine le terrain PAR CE
// SLOT-LÀ (SetTexture sur les stages 0 ET 1), pas par +0x38 — c'est la raison pour
// laquelle le compteur restait à 0 avec le seul hook sur +0x38.
DrawPrimRecFn g_orig_draw_prim_dual = nullptr;

void __fastcall Hooked_DrawPrimRecordDual(void* self, void* edx, void* rec) {
  if (g_in_ground_pass <= 0 || !g_ground_paint || g_imgui_dx7_active || !self) {
    if (g_orig_draw_prim_dual) g_orig_draw_prim_dual(self, edx, rec);
    return;
  }
  PaintAroundDraw(self, edx, rec, g_orig_draw_prim_dual);
}

// Marqueur de passe pour RendererDX9_DrawTerrainSurfaces 0x0055d850 — LA fonction qui
// dessine réellement le terrain (3 branches selon OptionInfo 0x77 et le support aniso :
// listes +0x168, +0x1f8 via vtbl +0x34, ou +0x174 via vtbl +0x38). Prouvé en live :
// RendererDX9_DrawGroundTiles 0x0055d680 s'exécute avec ses DEUX listes VIDES.
DrawGroundFn g_orig_draw_terrain = nullptr;

void __fastcall Hooked_DrawTerrain(void* self) {
  ++g_in_ground_pass;
  if (g_orig_draw_terrain) g_orig_draw_terrain(self);
  --g_in_ground_pass;
}

}  // namespace

// ── API publique ──────────────────────────────────────────────────────────────
bool&  ground_paint_enabled() { return g_ground_paint; }
float* ground_color()         { return g_ground_col; }

void EnsureInstalled() {
  static bool done = false;
  if (done) return;
  done = true;
  using namespace hooking;
  ez_capture::EnsureInstalled();  // hooks de capture EZ (module partagé, idempotent)
  g_orig_draw_ground = reinterpret_cast<DrawGroundFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kDX9DrawGround),
          reinterpret_cast<uint8_t*>(&Hooked_DrawGround)));
  // Un trampoline nul = SetHook a échoué (prologue non relocalisable) : le hook ne
  // doit alors JAMAIS avaler l'appel, sinon le sol disparaît au lieu de changer de
  // couleur. Hooked_DrawGround teste g_orig_draw_ground, mais on le trace ici.
  g_orig_draw_prim = reinterpret_cast<DrawPrimRecFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kDX9DrawPrimRec),
          reinterpret_cast<uint8_t*>(&Hooked_DrawPrimRecord)));
  g_orig_draw_prim_dual = reinterpret_cast<DrawPrimRecFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kDX9DrawPrimDual),
          reinterpret_cast<uint8_t*>(&Hooked_DrawPrimRecordDual)));
  g_orig_draw_terrain = reinterpret_cast<DrawGroundFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kDX9DrawTerrain),
          reinterpret_cast<uint8_t*>(&Hooked_DrawTerrain)));
}

void RenderFrame() {
  // Les hooks du « Sol uni » doivent exister dès que le réglage est actif, même si l'onglet
  // SPR Lab n'a jamais été ouvert de la session (réglage restauré depuis le YAML au login).
  // EnsureInstalled est idempotent.
  if (g_ground_paint) EnsureInstalled();

  // Suppression in-world NOMINATIVE (notre seul id) : supprimer « tout le capturé » ferait
  // disparaître les AUTRES effets du joueur, dont la barre d'icônes de statut (bug réel constaté).
  if (g_suppress && g_applied_concrete > 0) {  // piloté par la case « Cacher en jeu »
    const int sup[1] = { g_applied_concrete };
    ez_capture::SetSuppressedIds(ez_capture::kSlotLab, sup, 1);
  } else {
    ez_capture::SetSuppressedIds(ez_capture::kSlotLab, nullptr, 0);
  }
  Reconcile();
  DrawCenteredOverlay();
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
      g_wanted_ordinal = g_ui_ordinal;    }
  } else {
    if (ImGui::Button("Arrêter")) {
      g_wanted_ordinal = 0;    }
  }
  ImGui::SameLine();
  // DIAGNOSTIC (bissection d'une régression du rendu natif) : neutralise le hook du chemin
  // CEffectMgr. Si le z-order des chapeaux/costumes natifs redevient correct en cochant, c'est ce
  // hook qui perturbe le rendu ; sinon il est hors de cause. ⚠ Seul écrivain de ce réglage.
  static bool s_no_effmgr = false;
  if (ImGui::Checkbox("Couper le hook CEffectMgr (diag)", &s_no_effmgr))
    ez_capture::SetEffMgrCaptureEnabled(!s_no_effmgr);
  ImGui::SameLine();
  // DIAGNOSTIC : lever l'exclusion de la famille .str. Si un effet rend en jeu mais n'est JAMAIS
  // capturé (0 dessin le concernant), c'est peut-être NOUS qui l'écartons par sa vtable.
  static bool s_capture_str = false;
  if (ImGui::Checkbox("Capturer aussi les .str (diag)", &s_capture_str))
    ez_capture::SetCaptureStrEffects(s_capture_str);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Les instances CEZ2STREffect sont normalement écartées (autre pipeline).\n"
                      "Coche pour vérifier si un effet non capturé appartient à cette famille :\n"
                      "s'il apparaît alors dans « effect_id capturés », c'est le cas.\n"
                      "⚠ Peut provoquer un double dessin ailleurs : diagnostic uniquement.");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Sert à isoler une régression : coche, puis regarde si le rendu NATIF\n"
                      "(z-order des chapeaux/costumes) redevient correct.\n"
                      "Coché = la famille aura/statut n'est plus ni capturée ni composée\n"
                      "sur le doll (Perm_Frost & co disparaîtront de l'aperçu).");
  ImGui::SameLine();
  // Répartition tri/quad : diagnostic direct du layout (EZ = triangles 3 sommets, STR = quads 4).
  // ⚠ Count()/Prims() ramènent TOUS les effets du joueur : on ne compte que les NÔTRES (IsOurs),
  // sinon les chiffres gonflent avec les auras, statuts et autres effets équipés.
  const int cap_count = ez_capture::Count();
  const ez_capture::Prim* cap_prims = ez_capture::Prims();
  int n_tri = 0, n_quad = 0, n_ours = 0;
  for (int i = 0; i < cap_count; ++i) {
    if (!IsOurs(cap_prims[i])) continue;
    n_ours++;
    (cap_prims[i].n == 3 ? n_tri : n_quad)++;
  }
  ImGui::Text("état: %s (%d prim : %d tri, %d quad)", on ? "actif" : "éteint", n_ours, n_tri, n_quad);

  // ── Sonde « rien ne s'affiche » : localise la cause au lieu de la deviner ──
  // Beaucoup d'ordinaux ne rendent RIEN, même nativement (ex. 57, et toute la plage 60-78 =
  // auras LEVEL99/LEVEL160). Ces trois compteurs disent OÙ ça s'arrête.
  if (on) {
    const ez_capture::Stats st = ez_capture::LastFrameStats();
    // dessins/inserts viennent du module et restent tels quels ; « capturés » est en revanche
    // ramené à NOTRE effet (n_ours), sinon un autre effet du joueur masquerait notre 0.
    ImGui::TextDisabled("sonde: %d/%d dessins / %d inserts / %d capturés",
                        st.draws, st.all_draws, st.inserts, n_ours);
    ImGui::SameLine();
    if (st.draws == 0)
      ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f), "-> nœud jamais dessiné (non créé ?)");
    else if (st.inserts == 0)
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "-> dessiné mais n'émet RIEN (ressource ?)");
    else if (n_ours == 0)
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "-> émis mais REJETÉ par nous (bug)");
    else
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "-> OK");
    // Ce que le filtre de FORME a refusé cette frame. Quand « émis mais rejeté » s'affiche, c'est
    // ici que se trouve la raison : type de primitive, nombre de sommets, primitive indexée.
    // Formes acceptées : type 4 (TRIANGLELIST) à 3 sommets, ou type 5 (TRIANGLESTRIP) à 4, non indexé.
    if (st.rej_count > 0)
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                         "rejets: %d  (type=%d, sommets=%d, indices=%d)",
                         st.rej_count, st.rej_type, st.rej_vtx, st.rej_idx);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("dessins  = appels d'EzEffect_Draw sur NOTRE nœud\n"
                        "inserts  = primitives soumises pendant ce dessin (avant nos filtres)\n"
                        "capturés = ce qu'on garde après filtres de forme\n\n"
                        "0 dessin        -> le nœud n'existe pas : problème AMONT du rendu.\n"
                        "dessins, 0 ins. -> le sous-rendu n'émet rien : ressource absente\n"
                        "                   (le chargeur échoue en silence) ou condition non remplie.\n"
                        "ins. mais 0 cap.-> le jeu émet et NOUS rejetons : là c'est notre bug.");
  }

  // Facteurs de blend RÉELLEMENT capturés cette frame (D3DBLEND bruts : 2=ONE, 5=SRCALPHA,
  // 6=INVSRCALPHA…). Diagnostic direct : s'il y a plusieurs couples, aucun blend GLOBAL ne
  // peut convenir — c'est la démonstration que le mode « natif par primitive » est nécessaire.
  // ⚠ Là encore, uniquement NOS primitives : les couples de blend des autres effets du joueur
  // n'apprendraient rien sur celui qu'on étudie.
  int  bl_src[4] = {0, 0, 0, 0}, bl_dst[4] = {0, 0, 0, 0}, bl_n = 0;
  for (int i = 0; i < cap_count; ++i) {
    if (!IsOurs(cap_prims[i])) continue;
    bool seen = false;
    for (int k = 0; k < bl_n; ++k)
      if (bl_src[k] == cap_prims[i].src_blend && bl_dst[k] == cap_prims[i].dst_blend) { seen = true; break; }
    if (!seen && bl_n < 4) {
      bl_src[bl_n] = cap_prims[i].src_blend; bl_dst[bl_n] = cap_prims[i].dst_blend; bl_n++;
    }
  }

  // ── MESURE : ids RÉELS de la famille CEffectMgr (auras, statuts, certains costumes) ─────────
  // Ces primitives n'ont pas d'id concret ; leur instance porte SON id à +0x04. DEUX hypothèses
  // successives sur son encodage ont été RÉFUTÉES en jeu (« ces costumes ne passent pas par ce
  // chemin », puis « id == ordinal + 0x98a ») : on ne suppose plus, on AFFICHE.
  // Placé HAUT dans le panneau, au-dessus du catalogue, pour rester visible sans défiler.
  {
    const ez_capture::Prim* p = ez_capture::Prims();
    const int total = ez_capture::Count();
    int vals[8], nv = 0, n_em = 0;
    for (int i = 0; i < total; ++i) {
      if (p[i].effect_id >= 0) continue;          // chemin EZ : id concret, pas concerné
      n_em++;
      bool seen = false;
      for (int k = 0; k < nv; ++k) if (vals[k] == p[i].effmgr_id) { seen = true; break; }
      if (!seen && nv < 8) vals[nv++] = p[i].effmgr_id;
    }
    if (nv > 0) {
      char b[128]; int o = 0;
      for (int k = 0; k < nv && o < 110; ++k)
        o += std::snprintf(b + o, sizeof(b) - o, k ? ", %d" : "%d", vals[k]);
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "ids CEffectMgr (%d prim) : %s", n_em, b);
    } else {
      ImGui::TextDisabled("ids CEffectMgr : aucun");
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Valeurs BRUTES lues à instance+0x04, pour établir leur encodage :\n"
                        "équipe un costume dont tu connais l'ordinal, puis déséquipe-le et\n"
                        "regarde quelle valeur disparaît.");

    // ── TOUS les effect_id capturés, sans filtrage ────────────────────────────
    // Angle mort corrigé : la ligne ci-dessus ne montre que la famille CEffectMgr (id < 0). Si des
    // primitives sont capturées avec un id POSITIF différent de celui qu'on attend, rien ne le
    // révélait — on voyait seulement « 0 capturés » sans comprendre pourquoi.
    int all[10], na = 0, cnt[10] = {0};
    for (int i = 0; i < total; ++i) {
      int k = 0;
      for (; k < na; ++k) if (all[k] == p[i].effect_id) break;
      if (k == na && na < 10) { all[na] = p[i].effect_id; cnt[na] = 0; na++; }
      if (k < 10) cnt[k]++;
    }
    if (na > 0) {
      char b[160]; int o = 0;
      for (int k = 0; k < na && o < 140; ++k)
        o += std::snprintf(b + o, sizeof(b) - o, k ? ", %d×%d" : "%d×%d", all[k], cnt[k]);
      ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "effect_id capturés : %s", b);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Tous les ids présents dans la capture, au format id×nombre.\n"
                          "Attendu : l'id concret de l'effet spawné. Une valeur DIFFÉRENTE\n"
                          "signifie que le nœud porte un autre id que celui résolu par Lua.\n"
                          "-1 = famille CEffectMgr.");
    } else {
      ImGui::TextDisabled("effect_id capturés : aucun (rien n'est capturé)");
    }
  }

  // ── QUI soumet les primitives ? ──────────────────────────────────────────────
  // Pour les effets qui ne sont dessinés par AUCUN de nos hooks d'appartenance (ils délèguent à un
  // objet hôte piloté par un autre sous-système), c'est le seul moyen de trouver quoi hooker :
  // on relève les appelants du puits de primitives et on compare effet ACTIF vs ÉTEINT.
  // L'adresse qui n'apparaît QUE lorsque l'effet tourne est la fonction cherchée.
  {
    static bool show_callers = false;
    ImGui::Checkbox("Appelants du puits (diag)", &show_callers);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Adresses de retour des fonctions qui soumettent des primitives, avec leur\n"
                        "nombre d'appels sur la dernière frame.\n"
                        "Méthode : relève la liste effet ÉTEINT, puis effet ACTIF —\n"
                        "l'adresse qui APPARAÎT est celle qui dessine l'effet.");
    if (show_callers) {
      const ez_capture::Caller* c = ez_capture::Callers();
      const int n = ez_capture::CallerCount();
      if (ImGui::BeginChild("##ez_callers", ImVec2(0, 0), true)) {
        for (int i = 0; i < n; ++i)
          ImGui::TextDisabled("0x%08X  ×%d", static_cast<unsigned>(c[i].addr), c[i].count);
        if (n == 0) ImGui::TextDisabled("(aucun appel cette frame)");
      }
      ImGui::EndChild();
    }
  }

  ImGui::Checkbox("Cacher en jeu (overlay seul)", &g_suppress);
  ImGui::SetNextItemWidth(180.0f);
  ImGui::Combo("Blend", &g_blend_mode,
               "Natif par primitive\0Alpha normal\0Additif global\0");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Natif par primitive : rejoue SRCBLEND/DESTBLEND que le jeu a posés pour\n"
                      "chaque primitive (record +0x18/+0x1c) — le seul mode correct, car un\n"
                      "effet mélange additif et alpha dans la même frame.\n"
                      "Alpha normal : les primitives additives sortent en CARRÉS NOIRS.\n"
                      "Additif global : les primitives alpha CRAMENT en blanc.\n"
                      "⚠ Le mode natif agit sur le device D3D9 : inopérant sous DX7.");
  ImGui::SameLine();
  if (bl_n > 0) {
    char b[96]; int o = 0;
    for (int k = 0; k < bl_n && o < 80; ++k)
      o += std::snprintf(b + o, sizeof(b) - o, k ? " + %d/%d" : "%d/%d", bl_src[k], bl_dst[k]);
    ImGui::TextDisabled("blends: %s", b);
  } else {
    ImGui::TextDisabled("blends: -");
  }

  ImGui::Checkbox("Debug capture (brut + ancre)", &g_debug);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Dessine la capture BRUTE (positions écran natives) + l'ancre (point cyan) et\n"
                      "le cercle du rayon, SANS reprojection. Décoche « Cacher en jeu » pour\n"
                      "comparer les contours (vert) à l'effet natif in-world.");
  ImGui::SameLine();
  ImGui::TextDisabled("ancre (%.0f,%.0f)%s", g_dbg_ax, g_dbg_ay, g_dbg_proj_ok ? "" : " [non projetée]");

  ImGui::SetNextItemWidth(180.0f);
  ImGui::SliderFloat("Rayon (px)", &g_max_r, 100.0f, 3000.0f, "%.0f");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Garde-fou large par défaut (la géométrie « traînée » est LÉGITIME :\n"
                      "le natif la rend en additif + dégradé d'alpha). À baisser seulement si\n"
                      "un effet déborde encore : Rayon = distance sommet/ancre.");

  // ── Catalogue : liste NATIVE des ordinaux d'effets .spr/EZ (aucune liste hardcodée) ──
  ImGui::Separator();
  if (ImGui::Button(g_catalog_built ? "Rescanner" : "Scanner le catalogue")) BuildCatalog();
  ImGui::SameLine();
  ImGui::TextDisabled("%d effets EZ, dont %d implémentés (clic = spawn)",
                      static_cast<int>(g_catalog.size()), g_catalog_impl);
  if (g_catalog_built) {
    static char filter[32] = "";
    static bool only_impl = true;   // par défaut on masque les inertes : ils ne rendront JAMAIS rien
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("filtre", filter, sizeof(filter));
    ImGui::SameLine();
    ImGui::Checkbox("implémentés seulement", &only_impl);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Un effect id sans entrée dans la table de saut du client (0x00bc2e04)\n"
                        "tombe sur un DEFAULT `mov al,1 ; ret` : le nœud est créé et tické, mais\n"
                        "ne dessine JAMAIS rien — nativement compris, et sans aucune erreur.\n"
                        "Ce n'est pas une ressource absente : c'est du code qui n'existe pas.\n"
                        "L'état est LU dans la table du client, pas recopié d'une liste.");
    if (ImGui::BeginChild("##spr_catalog", ImVec2(0, 190), true)) {
      for (const HatEntry& e : g_catalog) {
        if (only_impl && e.kind == 0) continue;
        const char* tag = (e.kind == 0) ? "   [inerte]" : (e.kind == 2 ? "   [.str]" : "");
        char label[128];
        if (e.res[0])
          std::snprintf(label, sizeof(label), "ord %-3d  ->  id %-5d   %s%s", e.ord, e.cid, e.res, tag);
        else
          std::snprintf(label, sizeof(label), "ord %-3d  ->  id %-5d%s", e.ord, e.cid, tag);
        if (filter[0] && !std::strstr(label, filter)) continue;
        // Inerte = grisé (cliquable quand même, pour le vérifier soi-même via la sonde).
        // .str = teinté : rend NATIVEMENT, mais par un autre pipeline -> ce lab ne le capture pas.
        if (e.kind == 0) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        else if (e.kind == 2) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
        if (ImGui::Selectable(label, e.ord == g_wanted_ordinal)) {
          g_ui_ordinal        = e.ord;
          g_resolved_concrete = e.cid;
          g_wanted_ordinal    = e.ord;   // spawn immédiat au clic
        }
        if (e.kind != 1) ImGui::PopStyleColor();
      }
    }
    ImGui::EndChild();
    ImGui::TextDisabled("« inerte » = ni entrée procédurale ni resourceFileName -> ne rend rien, même");
    ImGui::TextDisabled("nativement.  « .str » = rend en jeu, mais via le pipeline billboard STR :");
    ImGui::TextDisabled("un autre chemin que EzEffect_Draw, donc a priori NON capturé par ce lab.");
  }

  // ── Fond de capture : sol uni ───────────────────────────────────────────────
  ImGui::Separator();
  bool ground_changed = ImGui::Checkbox("Sol uni (fond de capture)", &g_ground_paint);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Repeint tout le terrain .gnd d'une couleur unie, sans toucher au reste\n"
                      "de la scène : la géométrie et le z-buffer du sol restent intacts, donc\n"
                      "l'occlusion par le terrain reste correcte.\n"
                      "L'eau, le ciel et le brouillard ne sont PAS affectés.\n"
                      "DX9 uniquement (le chemin de rendu DX7 est une autre famille de "
                      "fonctions).");
  if (g_imgui_dx7_active) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "(DX7 : non supporté)");
  }
  if (g_ground_paint) {
    ImGui::SetNextItemWidth(200.0f);
    ColorEdit4WithAlphaBar("Couleur du sol", g_ground_col);
    // Le picker renvoie true à CHAQUE frame de drag : on ne persiste qu'au relâchement,
    // sinon on réécrit tout le YAML des dizaines de fois par seconde.
    if (ImGui::IsItemDeactivatedAfterEdit()) ground_changed = true;
    ImGui::TextDisabled("L'alpha est ignoré (la passe du sol est opaque).");
  }
  if (ground_changed) {
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}

}  // namespace spr_lab
