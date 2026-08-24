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
#include "features/moonlight_ui/moonlight_ui.h"  // helpers UI du toolkit (namespace mui)
#include "ragnarok/game_scene.h"
#include "ragnarok/own_actor.h"  // rag::kActorToggleEffectIdAddr / kHatEffectIdBase
#include "ui/ro_imgui.h"  // ro::Px (échelle de l'interface, largeurs de contrôles)
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace spr_lab {
namespace {

// ── Adresses / offsets natifs (client 20250716, base 0x400000) ────────────────
// Réutilisés à l'identique de la RE existante (cf. basic_info.cc, docs/hat_effect_re.md).

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

  void* actor = rag::OwnActor();

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
  const ToggleFn toggle = reinterpret_cast<ToggleFn>(rag::kActorToggleEffectIdAddr);
  __try {
    // Despawn propre SEULEMENT si l'acteur est encore le nôtre (sinon l'ancien est déjà libéré).
    if (g_applied_ordinal != 0 && actor == g_applied_actor)
      toggle(actor, g_applied_ordinal + rag::kHatEffectIdBase, 0);
    if (g_wanted_ordinal != 0) {
      g_applied_concrete = ResolveConcreteId(g_wanted_ordinal);  // id concret NATIF (pas de hardcode)
      toggle(actor, g_wanted_ordinal + rag::kHatEffectIdBase, 1);
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

}  // namespace

// ── API publique ──────────────────────────────────────────────────────────────
void EnsureInstalled() {
  // Le lab n'a plus de hook à lui : tout ce qu'il capture passe par le module EZ
  // partagé, lui-même idempotent (le garde local n'aurait plus rien à garder).
  ez_capture::EnsureInstalled();
}

void RenderFrame() {
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

  ImGui::TextUnformatted(i18n::Tr("SPR Effect Lab — rend un hat effect .spr/EZ au centre de l'écran."));
  ImGui::Separator();

  // Seul l'ordinal est saisi ; l'id concret est résolu par le getter NATIF GetHatEffectID.
  ImGui::SetNextItemWidth(ro::Px(120.0f));
  if (ImGui::InputInt(i18n::Tr("Ordinal"), &g_ui_ordinal) || g_resolved_concrete <= 0)
    g_resolved_concrete = ResolveConcreteId(g_ui_ordinal);
  ImGui::SameLine();
  if (g_resolved_concrete > 0)
    ImGui::Text(i18n::Tr("-> id concret %d (natif)"), g_resolved_concrete);
  else
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), i18n::Tr("-> non résolu (en jeu ? Lua prêt ?)"));
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(i18n::Tr("L'id concret vient de GetHatEffectID(ordinal), le getter du jeu.\n"
                      "Ordinal 87 = Digital_Space -> 1240. Aucun hardcode."));

  const bool on = (g_wanted_ordinal != 0);
  if (!on) {
    if (ro::RoButton(i18n::Tr("Spawn + afficher au centre"))) {
      g_wanted_ordinal = g_ui_ordinal;    }
  } else {
    if (ro::RoButton(i18n::Tr("Arrêter"))) {
      g_wanted_ordinal = 0;    }
  }
  ImGui::SameLine();
  // DIAGNOSTIC (bissection d'une régression du rendu natif) : neutralise le hook du chemin
  // CEffectMgr. Si le z-order des chapeaux/costumes natifs redevient correct en cochant, c'est ce
  // hook qui perturbe le rendu ; sinon il est hors de cause. ⚠ Seul écrivain de ce réglage.
  static bool s_no_effmgr = false;
  if (ro::RoCheckbox(i18n::Tr("Couper le hook CEffectMgr (diag)"), &s_no_effmgr))
    ez_capture::SetEffMgrCaptureEnabled(!s_no_effmgr);
  ImGui::SameLine();
  // DIAGNOSTIC : lever l'exclusion de la famille .str. Si un effet rend en jeu mais n'est JAMAIS
  // capturé (0 dessin le concernant), c'est peut-être NOUS qui l'écartons par sa vtable.
  static bool s_capture_str = false;
  if (ro::RoCheckbox(i18n::Tr("Capturer aussi les .str (diag)"), &s_capture_str))
    ez_capture::SetCaptureStrEffects(s_capture_str);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(i18n::Tr("Les instances CEZ2STREffect sont normalement écartées (autre pipeline).\n"
                      "Coche pour vérifier si un effet non capturé appartient à cette famille :\n"
                      "s'il apparaît alors dans « effect_id capturés », c'est le cas.\n"
                      "/!\\ Peut provoquer un double dessin ailleurs : diagnostic uniquement."));
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(i18n::Tr("Sert à isoler une régression : coche, puis regarde si le rendu NATIF\n"
                      "(z-order des chapeaux/costumes) redevient correct.\n"
                      "Coché = la famille aura/statut n'est plus ni capturée ni composée\n"
                      "sur le doll (Perm_Frost & co disparaîtront de l'aperçu)."));
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
  ImGui::Text(i18n::Tr("état: %s (%d prim : %d tri, %d quad)"), on ? "actif" : i18n::Tr("éteint"), n_ours, n_tri, n_quad);

  // ── Sonde « rien ne s'affiche » : localise la cause au lieu de la deviner ──
  // Beaucoup d'ordinaux ne rendent RIEN, même nativement (ex. 57, et toute la plage 60-78 =
  // auras LEVEL99/LEVEL160). Ces trois compteurs disent OÙ ça s'arrête.
  if (on) {
    const ez_capture::Stats st = ez_capture::LastFrameStats();
    // dessins/inserts viennent du module et restent tels quels ; « capturés » est en revanche
    // ramené à NOTRE effet (n_ours), sinon un autre effet du joueur masquerait notre 0.
    ImGui::TextDisabled(i18n::Tr("sonde: %d/%d dessins / %d inserts / %d capturés"),
                        st.draws, st.all_draws, st.inserts, n_ours);
    ImGui::SameLine();
    if (st.draws == 0)
      ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f), i18n::Tr("-> nœud jamais dessiné (non créé ?)"));
    else if (st.inserts == 0)
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), i18n::Tr("-> dessiné mais n'émet RIEN (ressource ?)"));
    else if (n_ours == 0)
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), i18n::Tr("-> émis mais REJETÉ par nous (bug)"));
    else
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), i18n::Tr("-> OK"));
    // Ce que le filtre de FORME a refusé cette frame. Quand « émis mais rejeté » s'affiche, c'est
    // ici que se trouve la raison : type de primitive, nombre de sommets, primitive indexée.
    // Formes acceptées : type 4 (TRIANGLELIST) à 3 sommets, ou type 5 (TRIANGLESTRIP) à 4, non indexé.
    if (st.rej_count > 0)
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                         "rejets: %d  (type=%d, sommets=%d, indices=%d)",
                         st.rej_count, st.rej_type, st.rej_vtx, st.rej_idx);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(i18n::Tr("dessins  = appels d'EzEffect_Draw sur NOTRE nœud\n"
                        "inserts  = primitives soumises pendant ce dessin (avant nos filtres)\n"
                        "capturés = ce qu'on garde après filtres de forme\n\n"
                        "0 dessin        -> le nœud n'existe pas : problème AMONT du rendu.\n"
                        "dessins, 0 ins. -> le sous-rendu n'émet rien : ressource absente\n"
                        "                   (le chargeur échoue en silence) ou condition non remplie.\n"
                        "ins. mais 0 cap.-> le jeu émet et NOUS rejetons : là c'est notre bug."));
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
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), i18n::Tr("ids CEffectMgr (%d prim) : %s"), n_em, b);
    } else {
      ImGui::TextDisabled(i18n::Tr("ids CEffectMgr : aucun"));
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(i18n::Tr("Valeurs BRUTES lues à instance+0x04, pour établir leur encodage :\n"
                        "équipe un costume dont tu connais l'ordinal, puis déséquipe-le et\n"
                        "regarde quelle valeur disparaît."));

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
        o += std::snprintf(b + o, sizeof(b) - o, k ? i18n::Tr(", %d×%d") : i18n::Tr("%d×%d"), all[k], cnt[k]);
      ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), i18n::Tr("effect_id capturés : %s"), b);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(i18n::Tr("Tous les ids présents dans la capture, au format id×nombre.\n"
                          "Attendu : l'id concret de l'effet spawné. Une valeur DIFFÉRENTE\n"
                          "signifie que le nœud porte un autre id que celui résolu par Lua.\n"
                          "-1 = famille CEffectMgr."));
    } else {
      ImGui::TextDisabled(i18n::Tr("effect_id capturés : aucun (rien n'est capturé)"));
    }
  }

  // ── QUI soumet les primitives ? ──────────────────────────────────────────────
  // Pour les effets qui ne sont dessinés par AUCUN de nos hooks d'appartenance (ils délèguent à un
  // objet hôte piloté par un autre sous-système), c'est le seul moyen de trouver quoi hooker :
  // on relève les appelants du puits de primitives et on compare effet ACTIF vs ÉTEINT.
  // L'adresse qui n'apparaît QUE lorsque l'effet tourne est la fonction cherchée.
  {
    static bool show_callers = false;
    ro::RoCheckbox(i18n::Tr("Appelants du puits (diag)"), &show_callers);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(i18n::Tr("Adresses de retour des fonctions qui soumettent des primitives, avec leur\n"
                        "nombre d'appels sur la dernière frame.\n"
                        "Méthode : relève la liste effet ÉTEINT, puis effet ACTIF —\n"
                        "l'adresse qui APPARAÎT est celle qui dessine l'effet."));
    if (show_callers) {
      const ez_capture::Caller* c = ez_capture::Callers();
      const int n = ez_capture::CallerCount();
      if (ImGui::BeginChild("##ez_callers", ImVec2(0, 0), true)) {
        for (int i = 0; i < n; ++i)
          ImGui::TextDisabled(i18n::Tr("0x%08X  ×%d"), static_cast<unsigned>(c[i].addr), c[i].count);
        if (n == 0) ImGui::TextDisabled(i18n::Tr("(aucun appel cette frame)"));
      }
      ImGui::EndChild();
    }
  }

  ro::RoCheckbox(i18n::Tr("Cacher en jeu (overlay seul)"), &g_suppress);
  ImGui::SetNextItemWidth(ro::Px(180.0f));
  ImGui::Combo("Blend", &g_blend_mode,
               i18n::Tr("Natif par primitive\0Alpha normal\0Additif global\0"));
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(i18n::Tr("Natif par primitive : rejoue SRCBLEND/DESTBLEND que le jeu a posés pour\n"
                      "chaque primitive (record +0x18/+0x1c) — le seul mode correct, car un\n"
                      "effet mélange additif et alpha dans la même frame.\n"
                      "Alpha normal : les primitives additives sortent en CARRÉS NOIRS.\n"
                      "Additif global : les primitives alpha CRAMENT en blanc.\n"
                      "/!\\ Le mode natif agit sur le device D3D9 : inopérant sous DX7."));
  ImGui::SameLine();
  if (bl_n > 0) {
    char b[96]; int o = 0;
    for (int k = 0; k < bl_n && o < 80; ++k)
      o += std::snprintf(b + o, sizeof(b) - o, k ? " + %d/%d" : "%d/%d", bl_src[k], bl_dst[k]);
    ImGui::TextDisabled(i18n::Tr("blends: %s"), b);
  } else {
    ImGui::TextDisabled(i18n::Tr("blends: -"));
  }

  ro::RoCheckbox(i18n::Tr("Debug capture (brut + ancre)"), &g_debug);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(i18n::Tr("Dessine la capture BRUTE (positions écran natives) + l'ancre (point cyan) et\n"
                      "le cercle du rayon, SANS reprojection. Décoche « Cacher en jeu » pour\n"
                      "comparer les contours (vert) à l'effet natif in-world."));
  ImGui::SameLine();
  ImGui::TextDisabled(i18n::Tr("ancre (%.0f,%.0f)%s"), g_dbg_ax, g_dbg_ay, g_dbg_proj_ok ? "" : i18n::Tr(" [non projetée]"));

  ImGui::SetNextItemWidth(ro::Px(180.0f));
  ro::RoSliderFloat("Rayon (px)", &g_max_r, 100.0f, 3000.0f, "%.0f");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(i18n::Tr("Garde-fou large par défaut (la géométrie « traînée » est LÉGITIME :\n"
                      "le natif la rend en additif + dégradé d'alpha). À baisser seulement si\n"
                      "un effet déborde encore : Rayon = distance sommet/ancre."));

  // ── Catalogue : liste NATIVE des ordinaux d'effets .spr/EZ (aucune liste hardcodée) ──
  ImGui::Separator();
  if (ro::RoButton(g_catalog_built ? "Rescanner" : i18n::Tr("Scanner le catalogue"))) BuildCatalog();
  ImGui::SameLine();
  ImGui::TextDisabled(i18n::Tr("%d effets EZ, dont %d implémentés (clic = spawn)"),
                      static_cast<int>(g_catalog.size()), g_catalog_impl);
  if (g_catalog_built) {
    static char filter[32] = "";
    static bool only_impl = true;   // par défaut on masque les inertes : ils ne rendront JAMAIS rien
    ImGui::SetNextItemWidth(ro::Px(180.0f));
    ImGui::InputText("filtre", filter, sizeof(filter));
    ImGui::SameLine();
    ro::RoCheckbox(i18n::Tr("implémentés seulement"), &only_impl);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(i18n::Tr("Un effect id sans entrée dans la table de saut du client (0x00bc2e04)\n"
                        "tombe sur un DEFAULT `mov al,1 ; ret` : le nœud est créé et tické, mais\n"
                        "ne dessine JAMAIS rien — nativement compris, et sans aucune erreur.\n"
                        "Ce n'est pas une ressource absente : c'est du code qui n'existe pas.\n"
                        "L'état est LU dans la table du client, pas recopié d'une liste."));
    if (ImGui::BeginChild("##spr_catalog", ImVec2(0, 190), true)) {
      for (const HatEntry& e : g_catalog) {
        if (only_impl && e.kind == 0) continue;
        const char* tag = (e.kind == 0) ? "   [inerte]" : (e.kind == 2 ? "   [.str]" : "");
        char label[128];
        if (e.res[0])
          std::snprintf(label, sizeof(label), i18n::Tr("ord %-3d  ->  id %-5d   %s%s"), e.ord, e.cid, e.res, tag);
        else
          std::snprintf(label, sizeof(label), i18n::Tr("ord %-3d  ->  id %-5d%s"), e.ord, e.cid, tag);
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
    ImGui::TextDisabled(i18n::Tr("« inerte » = ni entrée procédurale ni resourceFileName -> ne rend rien, même"));
    ImGui::TextDisabled(i18n::Tr("nativement.  « .str » = rend en jeu, mais via le pipeline billboard STR :"));
    ImGui::TextDisabled(i18n::Tr("un autre chemin que EzEffect_Draw, donc a priori NON capturé par ce lab."));
  }

  // Le « Sol uni » (fond de capture) vivait ici ; il n'a jamais rien partagé avec la
  // capture EZ et est désormais un module à part, dans « Staff Tools » :
  // features/fx/ground_paint.h.
}

}  // namespace spr_lab
