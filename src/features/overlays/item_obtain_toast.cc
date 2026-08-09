#include "features/overlays/item_obtain_toast.h"

#include <Windows.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "imgui.h"

#include "bourgeon.h"
#include "features/item_cell.h"          // itemcell::BuildDisplayName
#include "features/moonlight_ui/moonlight_ui.h"
#include "ragnarok/msgstring.h"          // msgstr::Utf8 (libellés natifs)
#include "ragnarok/uiwnd.h"              // uiwnd::SafeSetVisible
#include "ui/icon_cache.h"               // ro::ItemIcon
#include "ui/ro_imgui.h"                 // ro::LocalToUtf8, ro::DrawDescPanelFrame
#include "ui/ro_widgets.h"               // mui::
#include "utils/i18n.h"
#include "utils/log_console.h"

using namespace mui;

// ===========================================================================
// Bandeau d'obtention d'objet — masquage du natif + réimplémentation ImGui.
// Conception et RE : item_obtain_toast.h / docs/item_obtain_notify_re.md.
// ===========================================================================

namespace {

// ── Adresses du client 20250716 ─────────────────────────────────────────────
constexpr uintptr_t kVTable  = 0x0103274c;         // UINotifyItemObtainWnd
constexpr uintptr_t kMsgSlot = kVTable + 0x94;     // vtable+0x94 = OnMsg
constexpr uintptr_t kMsgOrig = 0x008c5860;         // UINotifyItemObtainWnd_OnMsg

// Le SEUL message qui nous intéresse. Il arrive en DERNIER des trois (76 puis 36
// puis 34) et porte un `ItemSkillInfo*` déjà rempli — tout est dedans, y compris
// la quantité. Les deux autres ne transportent rien qu'il n'ait déjà.
constexpr int kMsgSetInfo = 34;  // 0x22

// `MSI_EA_OBTAIN` — « ` - %d obtained.` ». 🔴 L'espace de tête appartient au
// libellé : on concatène tel quel, sans en ajouter une.
constexpr int kMsiObtain = 696;

// ── Champs d'un ItemSkillInfo ───────────────────────────────────────────────
// (mêmes offsets et mêmes noms que features/item_cell.cc et inventory_viewer.cc)
constexpr int kInfoNum     = 0x10;  // int    : quantité
constexpr int kInfoIdStr   = 0x2c;  // std::string : l'itemId EN TEXTE
constexpr int kInfoIdCap   = 0x40;  // capacité SSO (= 0x2c+0x14) ; >15 => heap
constexpr int kInfoIdent   = 0x5c;  // octet  : identifié
constexpr int kInfoDamaged = 0x5d;  // octet  : cassé
constexpr int kInfoRefine  = 0x60;  // int    : niveau d'affinage

// ── Géométrie, reprise du natif ─────────────────────────────────────────────
// Le natif : cadre haut de 32, icône 24x24 blittée en (13,5), texte en x=41,
// y=10. On garde ces valeurs — c'est ce qui fait que le remplacement se pose au
// même endroit et de la même taille que ce qu'il remplace.
constexpr float kRowH      = 32.0f;
constexpr float kIconX     = 13.0f;
constexpr float kIconSize  = 24.0f;  // (le natif la pose à y+5 dans ses 32 px ;
                                     //  la nôtre est centrée, cf. le rendu)
constexpr float kTextX     = 41.0f;
constexpr float kTextXNoIcon = 13.0f;  // le natif recule le texte quand il n'y a pas d'icône
constexpr float kPadRight  = 13.0f;
constexpr float kMinWidth  = 34.0f;    // plancher natif

// Plafond dur de la pile. Le réglage joue en dessous ; ceci n'est là que pour
// qu'un `max_lines` aberrant relu d'un yaml trafiqué ne fasse pas grossir la
// file sans fin.
constexpr int kMaxLines = 20;

// ── État ────────────────────────────────────────────────────────────────────

// Une ligne affichée. POD pur : elle est remplie sous SEH (cf. ReadInfo), et le
// SEH de MSVC est interdit dans une fonction qui déroule des objets C++.
struct Toast {
  uint32_t id         = 0;
  int      amount     = 0;
  int      refine     = 0;
  bool     identified = true;
  bool     damaged    = false;
  uint32_t born_ms    = 0;   // timeGetTime du dernier apport (regroupement compris)
  char     name[160]  = {0}; // nom composé par le name-builder natif, en UTF-8
};

using OnMsg_t = int(__fastcall*)(void* self, void* edx, int a0, int msg,
                                 int p2, int p3, int p4, int p5);

OnMsg_t g_msg_orig = nullptr;

ItemObtainToastConfig g_cfg;
bool g_in_game    = false;
bool g_needs_save = false;

// État de l'ancre déplaçable. `g_anchor_w` est sa largeur relue d'une frame sur
// l'autre (il faut la connaître pour la centrer, et elle dépend de la langue) ;
// `g_anchor_force` repositionne la fenêtre pour UNE frame, quand un réglage
// change la position sous elle — sans ça, ImGui garderait la position qu'elle
// avait à sa création et le retour au centrage ne se verrait pas.
float g_anchor_w     = 200.0f;
bool  g_anchor_force = false;

// 🔴 Le hook est appelé depuis le dispatch de paquets du client, pas depuis notre
// boucle de rendu. Ce dispatch touche l'UI native (il ouvre des fenêtres, écrit
// dans le chat), donc il tourne selon toute vraisemblance sur le fil principal —
// mais « selon toute vraisemblance » ne suffit pas quand le prix de la certitude
// est un verrou non contesté sur une liste de cinq éléments. On ne parie pas.
std::mutex g_mu;
std::vector<Toast> g_pending;  // écrit par le hook, vidé par le rendu
std::vector<Toast> g_live;     // la pile affichée, propriété du fil de rendu

// ── Lecture de l'ItemSkillInfo ──────────────────────────────────────────────
// POD uniquement, sous SEH : un `info` à moitié construit ne doit pas tuer le
// client. Le nom composé se prend APRÈS, hors du __try (BuildDisplayName pose
// déjà son propre garde).
bool ReadInfo(const void* info, Toast* out) {
  __try {
    const auto* p = static_cast<const uint8_t*>(info);
    const uint32_t cap = *reinterpret_cast<const uint32_t*>(p + kInfoIdCap);
    const char* ids = (cap > 0xf)
                          ? *reinterpret_cast<const char* const*>(p + kInfoIdStr)
                          : reinterpret_cast<const char*>(p + kInfoIdStr);
    if (!ids) return false;
    out->id         = static_cast<uint32_t>(std::atoi(ids));
    out->amount     = *reinterpret_cast<const int*>(p + kInfoNum);
    out->refine     = *reinterpret_cast<const int*>(p + kInfoRefine);
    out->identified = *(p + kInfoIdent) != 0;
    out->damaged    = *(p + kInfoDamaged) != 0;
    return out->id != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void Capture(const void* info) {
  if (!info) return;

  Toast t;
  if (!ReadInfo(info, &t)) return;
  // Le natif affiche `num_` tel quel, y compris le cas `result == 3` où il n'a
  // PAS corrigé la quantité du delta déjà en sac. On fait pareil : ce module
  // reproduit le bandeau, il ne réinterprète pas le paquet.
  if (t.amount <= 0) t.amount = 1;

  // Le nom exactement comme le client le compose : refine, préfixes de cartes,
  // « <forgeron>'s ». Rendu dans la code-page du client, donc à convertir.
  char raw[160] = {0};
  itemcell::BuildDisplayName(const_cast<void*>(info), raw, sizeof(raw));
  // Repli sur le nom de base de la DB du client si la composition n'a rien
  // rendu : une ligne « - 1 obtained. » sans objet ne dit rien à personne, et
  // `NameById` rend au pire « #<id> », qui se diagnostique.
  //
  // ⚠ Les deux sources n'ont PAS le même encodage : `BuildDisplayName` rend la
  // code-page du client (à convertir), `NameById` rend déjà de l'UTF-8 (à ne
  // surtout pas reconvertir — ce serait du mojibake sur le premier accent).
  const char* utf8;
  if (raw[0]) {
    utf8 = ro::LocalToUtf8(raw);
  } else {
    utf8 = itemcell::NameById(t.id);
  }
  std::snprintf(t.name, sizeof(t.name), "%s", utf8 ? utf8 : "");

  t.born_ms = timeGetTime();

  std::lock_guard<std::mutex> lock(g_mu);
  // La file d'attente ne peut pas gonfler : au-delà du plafond on jette la plus
  // ancienne, qui serait de toute façon poussée hors de la pile à la frame
  // suivante.
  if (g_pending.size() >= static_cast<size_t>(kMaxLines)) g_pending.erase(g_pending.begin());
  g_pending.push_back(t);
}

// ── Le détour ───────────────────────────────────────────────────────────────
// `__fastcall` + deux premiers paramètres (ecx, edx) est la façon d'écrire un
// `__thiscall` à six arguments pile — la même que uiwnd::OnMsg.
int __fastcall OnMsgHook(void* self, void* edx, int a0, int msg,
                         int p2, int p3, int p4, int p5) {
  if (g_cfg.enabled && msg == kMsgSetInfo && p2)
    Capture(reinterpret_cast<const void*>(p2));

  const int r = g_msg_orig(self, edx, a0, msg, p2, p3, p4, p5);

  // On laisse le natif faire tout son travail (il tient son propre modèle et son
  // minuteur), puis on l'efface. Pas de destruction ici : `self` est le `this` de
  // l'appel en cours. Sa fenêtre se supprimera seule au bout de ses 5 s.
  if (g_cfg.enabled) uiwnd::SafeSetVisible(self, false);
  return r;
}

template <typename T>
void PatchValue(uintptr_t addr, T value) {
  DWORD old_protect;
  if (VirtualProtect(reinterpret_cast<void*>(addr), sizeof(T),
                     PAGE_EXECUTE_READWRITE, &old_protect)) {
    *reinterpret_cast<T*>(addr) = value;
    VirtualProtect(reinterpret_cast<void*>(addr), sizeof(T), old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), sizeof(T));
  }
}

// ── Helpers de rendu ────────────────────────────────────────────────────────

inline ImU32 RgbToImU32(int rgb, int alpha = 255) {
  return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, alpha);
}
// ⚠ QUATRE composantes, pas trois. `mui::ColorEdit4WithAlphaBar` appelle
// `ImGui::ColorEdit4`, qui LIT ET ÉCRIT rgba[3] : lui passer un `float[3]`
// déborde du tampon dans les deux sens. L'alpha ne sert à rien ici (les couleurs
// sont opaques), il est là parce que le widget y touche.
inline void RgbToF4(int rgb, float* f) {
  f[0] = ((rgb >> 16) & 0xff) / 255.0f;
  f[1] = ((rgb >> 8) & 0xff) / 255.0f;
  f[2] = (rgb & 0xff) / 255.0f;
  f[3] = 1.0f;
}
inline int F3ToRgb(const float* f) {
  return (static_cast<int>(f[0] * 255.0f + 0.5f) << 16) |
         (static_cast<int>(f[1] * 255.0f + 0.5f) << 8) |
         static_cast<int>(f[2] * 255.0f + 0.5f);
}

// Le format ne porte-t-il QU'UNE conversion, et est-elle entière ? « %% » ne
// compte pas (c'est un pour-cent littéral). Tout le reste — %s, %n, deux
// conversions, un drapeau de largeur exotique — est refusé.
bool FormatTakesOneInt(const char* fmt) {
  int conversions = 0;
  for (const char* p = fmt; *p; ++p) {
    if (*p != '%') continue;
    ++p;
    if (*p == '%') continue;      // « %% » : littéral
    if (*p != 'd' && *p != 'i') return false;
    if (++conversions > 1) return false;
  }
  return conversions == 1;
}

// ── Tout tombe sur un pixel ENTIER ──────────────────────────────────────────
// 🔴 Le cadre 9-slice blitte ses tuiles avec un filtre POINT (ro_imgui pose
// ImCb_PointFilter). Une borne à x.5 échantillonne alors entre deux texels : au
// lieu d'adoucir, le point DUPLIQUE ou MANGE une colonne — d'où des coutures
// visibles aux angles et un bord qui « bave » d'un pixel selon la position.
//
// Les sources de demi-pixel n'étaient pas évidentes : la largeur vient d'une
// MESURE DE TEXTE (fractionnaire quasi toujours), le centrage divise par deux,
// et la hauteur est mise à l'échelle de la police. Poser une seule de ces
// valeurs sur un entier ne suffit pas — il faut la largeur ET l'origine, sinon
// le bord droit retombe entre deux pixels quelle que soit l'origine.
inline float Snap(float v) { return std::floor(v + 0.5f); }

// Un pourcentage de réglage vers l'alpha 0..255 d'un ImU32.
inline int PctToA8(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return (pct * 255) / 100;
}

// Fond LIBRE, dessiné à la place du cadre RO quand celui-ci est masqué. Sans
// lui, le texte se retrouve à nu sur la scène : lisible sur un sol sombre,
// illisible dès qu'un mur clair ou un effet passe dessous.
//
// Contrairement au cadre sysbox, tout est peint en primitives ImGui — donc
// couleur, opacité, arrondi et bordure sont libres, ce que l'art 9-slice ne
// permettait pas.
void DrawFreeBackground(ImDrawList* dl, float x0, float y0, float x1, float y1) {
  const float r = static_cast<float>(g_cfg.bg_rounding);
  if (g_cfg.bg_enabled) {
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                      RgbToImU32(g_cfg.bg_rgb, PctToA8(g_cfg.bg_alpha)), r);
  }
  if (g_cfg.border_enabled && g_cfg.border_thickness > 0) {
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1),
                RgbToImU32(g_cfg.border_rgb, PctToA8(g_cfg.border_alpha)), r, 0,
                static_cast<float>(g_cfg.border_thickness));
  }
}

int LineCap() {
  int cap = g_cfg.max_lines;
  if (cap < 1) cap = 1;
  if (cap > kMaxLines) cap = kMaxLines;
  return cap;
}

// Deux apports vont-ils sur la même ligne ? Même objet, même état : sinon un
// « +7 » fraîchement forgé se ferait avaler par la pile de non-affinés.
bool SameLine(const Toast& a, const Toast& b) {
  return a.id == b.id && a.refine == b.refine &&
         a.identified == b.identified && a.damaged == b.damaged;
}

// Verse la file d'attente dans la pile affichée, en regroupant si demandé, puis
// purge les lignes expirées et écrête au nombre voulu.
void PumpQueue(uint32_t now) {
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (const Toast& in : g_pending) {
      bool merged = false;
      if (g_cfg.merge_same) {
        for (Toast& cur : g_live) {
          if (!SameLine(cur, in)) continue;
          cur.amount += in.amount;
          cur.born_ms = in.born_ms;  // le regroupement fait repartir le minuteur
          merged = true;
          break;
        }
      }
      if (!merged) g_live.push_back(in);
    }
    g_pending.clear();
  }

  const uint32_t life = static_cast<uint32_t>(g_cfg.duration_ms < 250 ? 250
                                                                     : g_cfg.duration_ms);
  for (size_t i = g_live.size(); i-- > 0;) {
    // Soustraction non signée : elle reste juste au débordement de timeGetTime
    // (~49,7 jours d'uptime), là où `now > born + life` se tromperait.
    if (now - g_live[i].born_ms >= life)
      g_live.erase(g_live.begin() + static_cast<ptrdiff_t>(i));
  }
  const size_t cap = static_cast<size_t>(LineCap());
  if (g_live.size() > cap)
    g_live.erase(g_live.begin(), g_live.end() - static_cast<ptrdiff_t>(cap));
}

}  // namespace

// ===========================================================================

ItemObtainToast::ItemObtainToast() {
  // Détour de l'OnMsg dans la vtable STATIQUE : toutes les instances passent par
  // là, il n'y a donc rien à réarmer à chaque bandeau. On ne pose le détour que
  // si le slot porte bien la fonction attendue — un client d'une autre version
  // laisserait le natif intact plutôt que de sauter dans le vide.
  const uintptr_t cur = *reinterpret_cast<uintptr_t*>(kMsgSlot);
  if (cur == kMsgOrig) {
    g_msg_orig = reinterpret_cast<OnMsg_t>(kMsgOrig);
    PatchValue<uintptr_t>(kMsgSlot, reinterpret_cast<uintptr_t>(&OnMsgHook));
  } else {
    LogDiag("[ItemObtainToast] slot OnMsg inattendu ({:#x}), detour non pose", cur);
  }
}

ItemObtainToastConfig& ItemObtainToast::config() { return g_cfg; }

void ItemObtainToast::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  g_in_game = (mode_type == ModeMgr::ModeType::kGame);
  // Un changement de carte ou un retour au login ne doit rien laisser traîner :
  // les lignes d'avant ne veulent plus rien dire.
  std::lock_guard<std::mutex> lock(g_mu);
  g_pending.clear();
  g_live.clear();
}

void ItemObtainToast::OnRenderUI() {
  if (!g_in_game) return;
  if (!g_cfg.enabled) {
    // Désactivé en cours de partie : on ne garde pas une pile qui ne se videra
    // plus (le hook ne l'alimente plus).
    if (!g_live.empty()) g_live.clear();
    return;
  }

  const uint32_t now = timeGetTime();
  PumpQueue(now);

  // Déverrouillé, la pile vide : on montre quand même une ligne d'exemple. Sans
  // ça, placer l'ancre supposerait d'aller ramasser un objet toutes les cinq
  // secondes pour voir ce qu'on règle.
  const bool preview = !g_cfg.locked && g_live.empty();
  if (g_live.empty() && !preview) return;

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  if (!dl || !font) return;

  const float scale = (g_cfg.font_scale < 50 ? 50 : g_cfg.font_scale) / 100.0f;
  // Taille de police ENTIÈRE : un corps fractionnaire fait rééchantillonner les
  // glyphes de l'atlas et rend tout le texte pâteux, réglage de 110 % compris.
  const float fsize = Snap(ImGui::GetFontSize() * scale);

  // Hauteur d'une ligne. Toujours ENTIÈRE : elle sert d'incrément vertical d'une
  // ligne à la suivante, donc une fraction décalerait toute la pile de plus en
  // plus. Le contenu, c'est l'icône (24) ou le texte, selon le plus haut.
  const float pad_v = static_cast<float>(g_cfg.pad_v < 0 ? 0 : g_cfg.pad_v);
  const float content = (g_cfg.show_icon && kIconSize > fsize) ? kIconSize : fsize;
  float row_h = Snap(content + 2.0f * pad_v);

  // AVEC le cadre RO, la hauteur n'est pas libre : elle se cale sur la grille de
  // tuiles, sinon le 9-slice échantillonne entre ses texels.
  //
  // ⚠ On s'écarte alors des 32 px du natif, en connaissance de cause. 32, c'est
  // 28 de bordure (deux tuiles) + 4 d'intérieur : le toolkit ÉCRASE une tuile de
  // 14 dans 4 px sur les deux montants, un rapport 3,5:1 — la couture la plus
  // visible du cadre. À 28 il n'y a plus d'intérieur du tout, rien que les
  // quatre coins blittés 1:1. Les 24 px de l'icône y tiennent avec 2 px de
  // marge, et c'est aussi pourquoi `pad_v` n'a d'effet visible qu'une fois assez
  // grand pour faire passer la ligne au palier suivant (42).
  const float tile = ro::DescPanelEdge();  // 14
  if (g_cfg.show_frame && tile >= 1.0f) {
    const float need = row_h;
    row_h = 2.0f * tile;
    if (need > row_h) row_h += std::ceil((need - row_h) / tile) * tile;
  }

  const float gap = static_cast<float>(g_cfg.row_gap < 0 ? 0 : g_cfg.row_gap);

  const ImU32 col_text = RgbToImU32(g_cfg.text_rgb);
  const ImU32 col_qty  = RgbToImU32(g_cfg.qty_rgb);
  // L'ombre rouge du natif sous le nom d'un équipement CASSÉ (cf. item_cell.h) :
  // le texte ne change pas de couleur, c'est l'ombre décalée qui le fait paraître
  // rouge. On reprend le même geste pour rester cohérent avec les viewers.
  const ImU32 col_dmg = itemcell::kDamagedShadow;

  // Le suffixe vient de la MsgStringTable, donc d'un fichier du serveur : c'est
  // un format non littéral, et le passer tel quel à snprintf ferait confiance à
  // ce fichier pour ne contenir qu'un « %d ». Un « %s » qui s'y glisserait
  // lirait un pointeur pris sur la pile. On n'accepte donc que les formats à
  // UNE conversion entière, et on retombe sur le libellé d'origine sinon.
  // (Le natif, lui, ne vérifie rien — c'est un manque qu'on comble.)
  const char* fmt = msgstr::Utf8(kMsiObtain);
  if (!fmt || !*fmt || !FormatTakesOneInt(fmt)) fmt = " - %d obtained.";

  const float screen_w = ImGui::GetIO().DisplaySize.x;

  // ── L'ancre ───────────────────────────────────────────────────────────────
  // Le bandeau se dessine sur le foreground draw list : il passe au-dessus de
  // tout, mais ne peut rien capter à la souris. La poignée est donc une vraie
  // fenêtre ImGui, affichée SEULEMENT quand le verrou est ouvert — verrouillé,
  // le bandeau redevient parfaitement transparent aux clics, ce qu'on attend
  // d'une notification.
  //
  // C'est le geste direct, à la place des deux curseurs X/Y qu'il fallait
  // régler à l'aveugle : on attrape le bandeau là où il est.
  if (!g_cfg.locked) {
    const bool centered = g_cfg.pos_x < 0;
    const float ax = centered ? Snap((screen_w - g_anchor_w) * 0.5f)
                              : static_cast<float>(g_cfg.pos_x);
    ImGui::SetNextWindowPos(ImVec2(ax, static_cast<float>(g_cfg.pos_y)),
                            g_anchor_force ? ImGuiCond_Always : ImGuiCond_Once);
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags af =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("###itemtoast_anchor", nullptr, af)) {
      ImGui::TextUnformatted(i18n::Tr("Objet obtenu — glisser pour placer"));
      // Largeur relue pour la frame SUIVANTE : on ne peut pas centrer une
      // fenêtre dont la taille n'est pas encore connue, et elle dépend de la
      // langue. Le centrage converge donc en une frame, ce qui ne se voit pas.
      g_anchor_w = ImGui::GetWindowWidth();
      const ImVec2 wp = ImGui::GetWindowPos();
      const int nx = static_cast<int>(Snap(wp.x));
      const int ny = static_cast<int>(Snap(wp.y));
      // Un déplacement HORIZONTAL sort du centrage — on ne peut pas à la fois
      // centrer et placer. Un déplacement purement vertical, lui, le préserve :
      // descendre le bandeau ne doit pas le décentrer au passage.
      if (nx != static_cast<int>(ax)) {
        g_cfg.pos_x = nx;
        g_needs_save = true;
      }
      if (ny != g_cfg.pos_y) {
        g_cfg.pos_y = ny;
        g_needs_save = true;
      }
    }
    ImGui::End();
    g_anchor_force = false;
  }

  float y = static_cast<float>(g_cfg.pos_y);

  // La ligne d'exemple de l'aperçu. Un objet réel (501) plutôt qu'un texte
  // inventé : on voit l'icône, la vraie police et la vraie largeur.
  Toast demo;
  if (preview) {
    demo.id     = 501;
    demo.amount = 1;
    const char* dn = itemcell::NameById(demo.id);
    std::snprintf(demo.name, sizeof(demo.name), "%s", dn ? dn : "");
  }

  // `newest_on_top` : la pile pousse vers le bas, la plus récente en tête. Sinon
  // les nouvelles s'ajoutent sous les anciennes, comme un journal.
  const int n = preview ? 1 : static_cast<int>(g_live.size());
  for (int k = 0; k < n; ++k) {
    const Toast& t =
        preview ? demo : (g_cfg.newest_on_top ? g_live[n - 1 - k] : g_live[k]);

    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), fmt, t.amount);

    const ro::IconTex icon = g_cfg.show_icon ? ro::ItemIcon(t.id, t.identified ? 1 : 0)
                                             : ro::IconTex{};
    const bool has_icon = g_cfg.show_icon && icon.tex != nullptr;
    const float text_x = has_icon ? kTextX : kTextXNoIcon;

    const ImVec2 name_sz = font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, t.name);
    const ImVec2 suf_sz  = font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, suffix);

    // `ceil` et pas `Snap` : arrondir la largeur VERS LE BAS rognerait le
    // dernier glyphe d'un demi-pixel. Les autres termes sont déjà entiers, donc
    // la largeur l'est aussi une fois la mesure de texte arrondie.
    float w = std::ceil(text_x + name_sz.x + suf_sz.x + kPadRight);
    if (w < kMinWidth) w = kMinWidth;

    // 🔴 Deuxième moitié du problème de texels, et elle ne se règle PAS par un
    // arrondi de coordonnées. `ro::DrawDescPanelFrame` ÉTIRE sa bande centrale
    // (BlitStretch) là où le client, lui, la RÉPÈTE par pas de 14. Une largeur
    // intérieure qui n'est pas un multiple de la tuile fait donc échantillonner
    // la texture entre ses texels sur toute la longueur du bandeau — visible
    // d'autant plus que le filtre est POINT.
    //
    // On cale l'intérieur sur la grille de tuiles. C'est exactement l'intention
    // de l'arrondi natif à 28 (`if (w % 28) w += 28`), en réparant au passage
    // son erreur : lui AJOUTE 28 au lieu d'arrondir, donc sa largeur n'est
    // presque jamais un multiple de quoi que ce soit.
    if (g_cfg.show_frame) {
      const float e = ro::DescPanelEdge();  // 14 px, le côté d'une tuile sysbox
      if (e >= 1.0f) {
        const float inner = w - 2.0f * e;
        if (inner > 0.0f) w = 2.0f * e + std::ceil(inner / e) * e;
      }
    }

    // `pos_x` négatif = centré, ce que fait le natif à sa façon (un x pensé pour
    // 640, recentré à la résolution courante). Le centrage divise par deux : une
    // largeur impaire donne un x en .5, d'où le Snap.
    const float x = (g_cfg.pos_x < 0) ? Snap((screen_w - w) * 0.5f)
                                      : static_cast<float>(g_cfg.pos_x);

    if (g_cfg.show_frame)
      ro::DrawDescPanelFrame(dl, x, y, x + w, y + row_h);
    else
      DrawFreeBackground(dl, x, y, x + w, y + row_h);

    if (has_icon) {
      // Blit 1:1, comme le client : l'icône fait déjà 24x24, la redimensionner
      // ne ferait que la rendre floue.
      const float iw = icon.w > 0 ? static_cast<float>(icon.w) : kIconSize;
      const float ih = icon.h > 0 ? static_cast<float>(icon.h) : kIconSize;
      // Centrée dans la LIGNE, pas posée à un décalage fixe : le natif écrit
      // y+5 parce que sa ligne fait 32 ; la nôtre en fait 28, et le même
      // décalage ferait déborder l'icône du cadre par le bas.
      const ImVec2 ip(x + kIconX, Snap(y + (row_h - ih) * 0.5f));
      dl->AddImage(reinterpret_cast<ImTextureID>(icon.tex), ip,
                   ImVec2(ip.x + iw, ip.y + ih));
    }

    // Le texte aussi : ImGui échantillonne l'atlas de police en POINT, donc un
    // y fractionnaire suffit à rendre tous les glyphes flous.
    const float ty = Snap(y + (row_h - fsize) * 0.5f);
    const ImVec2 name_pos(x + text_x, ty);
    if (t.damaged)
      dl->AddText(font, fsize, ImVec2(name_pos.x + 1.0f, name_pos.y + 1.0f),
                  col_dmg, t.name);
    dl->AddText(font, fsize, name_pos, col_text, t.name);
    dl->AddText(font, fsize, ImVec2(Snap(name_pos.x + name_sz.x), ty), col_qty,
                suffix);

    y += row_h + gap;
  }

  // Une position posée à la souris se sauvegarde toute seule — mais seulement
  // une fois le glissement TERMINÉ, sinon on réécrirait le yaml à chaque frame
  // du déplacement.
  if (g_needs_save && !ImGui::IsAnyItemActive() &&
      !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
    g_needs_save = false;
  }
}

void ItemObtainToast::DrawSettings() {
  g_needs_save |= ro::RoCheckbox(i18n::Tr("Bandeau d'objet obtenu personnalisé"),
                                 &g_cfg.enabled);
  SameLine();
  HelpMarker(i18n::Tr("Activé = bandeau empilable et réglable\n"
                      "Désactivé = bandeau d'origine du client."));

  ImGui::BeginDisabled(!g_cfg.enabled);

  g_needs_save |= WheelSliderInt(i18n::Tr("Lignes max"), &g_cfg.max_lines, 1, kMaxLines);
  SameLine();
  HelpMarker(i18n::Tr("Le bandeau d'origine est bloqué à UNE ligne : un second "
                      "ramassage écrase le premier."));

  g_needs_save |= ro::RoCheckbox(i18n::Tr("La plus récente en haut"),
                                 &g_cfg.newest_on_top);
  g_needs_save |= ro::RoCheckbox(i18n::Tr("Regrouper le même objet"),
                                 &g_cfg.merge_same);
  SameLine();
  HelpMarker(i18n::Tr("Additionne les quantités sur une seule ligne tant qu'elle "
                      "est affichée, au lieu d'en ouvrir une par ramassage."));

  // Clé « Durée (ms) » : elle existe déjà au catalogue, autant la réutiliser que
  // d'en créer une quasi identique. Le format ne répète donc pas l'unité.
  g_needs_save |= WheelSliderInt(i18n::Tr("Durée (ms)"), &g_cfg.duration_ms,
                                 500, 20000, "%d");
  SameLine();
  HelpMarker(i18n::Tr("Le client d'origine tient 5000 ms, sans réglage possible."));

  SeparatorText(i18n::Tr("Placement"));

  // Pas de curseur X/Y : le placement se fait à la souris. Deux mécanismes pour
  // le même geste seraient une hésitation gratuite pour le joueur.
  bool unlocked = !g_cfg.locked;
  if (ro::RoCheckbox(i18n::Tr("Déverrouiller (glisser pour déplacer)"), &unlocked)) {
    g_cfg.locked = !unlocked;
    g_anchor_force = true;
    g_needs_save = true;
  }
  SameLine();
  HelpMarker(i18n::Tr("Déverrouillé, une poignée apparaît à l'emplacement du "
                      "bandeau : glisse-la où tu veux. Un exemple s'affiche "
                      "tant que rien n'a été ramassé.\n"
                      "Verrouillé, le bandeau ne capte plus aucun clic."));

  bool centered = (g_cfg.pos_x < 0);
  if (ro::RoCheckbox(i18n::Tr("Centrer horizontalement"), &centered)) {
    // 220 = l'abscisse du natif sur sa largeur de référence de 640 : décocher
    // repose le bandeau là où le client l'aurait mis.
    g_cfg.pos_x = centered ? -1 : 220;
    g_anchor_force = true;
    g_needs_save = true;
  }
  SameLine();
  HelpMarker(i18n::Tr("Le centrage survit à un déplacement VERTICAL de la "
                      "poignée ; un déplacement horizontal en sort."));

  SeparatorText(i18n::Tr("Compacité"));
  g_needs_save |= WheelSliderInt(i18n::Tr("Interligne"), &g_cfg.row_gap, 0, 16, "%d px");
  SameLine();
  HelpMarker(i18n::Tr("L'espace entre deux lignes. À 0 elles se touchent."));
  g_needs_save |= WheelSliderInt(i18n::Tr("Marge verticale"), &g_cfg.pad_v, 0, 12, "%d px");
  SameLine();
  HelpMarker(i18n::Tr("La marge au-dessus et au-dessous du contenu d'une ligne.\n"
                      "Sans effet tant que le cadre RO est affiché : sa hauteur "
                      "est imposée par la taille de ses tuiles."));
  g_needs_save |= WheelSliderInt(i18n::Tr("Taille police"), &g_cfg.font_scale,
                                 60, 200, "%d%%");

  SeparatorText(i18n::Tr("Apparence"));
  g_needs_save |= ro::RoCheckbox(i18n::Tr("Afficher l'icône"), &g_cfg.show_icon);
  g_needs_save |= ro::RoCheckbox(i18n::Tr("Cadre RO"), &g_cfg.show_frame);
  SameLine();
  HelpMarker(i18n::Tr("Le cadre clair du client. Décoché, le fond libre "
                      "ci-dessous prend sa place."));

  // Le fond libre ne sert QUE sans le cadre RO : le griser dit pourquoi il ne
  // fait rien, au lieu de laisser croire à un réglage cassé.
  ImGui::BeginDisabled(g_cfg.show_frame);
  g_needs_save |= ro::RoCheckbox(i18n::Tr("Fond libre"), &g_cfg.bg_enabled);
  float bgc[4];
  RgbToF4(g_cfg.bg_rgb, bgc);
  if (ColorEdit4WithAlphaBar(i18n::Tr("Couleur du fond"), bgc)) {
    g_cfg.bg_rgb = F3ToRgb(bgc);
    g_needs_save = true;
  }
  g_needs_save |= WheelSliderInt(i18n::Tr("Opacité du fond"), &g_cfg.bg_alpha,
                                 0, 100, "%d%%");
  g_needs_save |= WheelSliderInt(i18n::Tr("Arrondi"), &g_cfg.bg_rounding,
                                 0, 16, "%d px");

  g_needs_save |= ro::RoCheckbox(i18n::Tr("Bordure"), &g_cfg.border_enabled);
  ImGui::BeginDisabled(!g_cfg.border_enabled);
  float bdc[4];
  RgbToF4(g_cfg.border_rgb, bdc);
  if (ColorEdit4WithAlphaBar(i18n::Tr("Couleur de la bordure"), bdc)) {
    g_cfg.border_rgb = F3ToRgb(bdc);
    g_needs_save = true;
  }
  g_needs_save |= WheelSliderInt(i18n::Tr("Opacité de la bordure"),
                                 &g_cfg.border_alpha, 0, 100, "%d%%");
  g_needs_save |= WheelSliderInt(i18n::Tr("Épaisseur de la bordure"),
                                 &g_cfg.border_thickness, 1, 4, "%d px");
  ImGui::EndDisabled();
  ImGui::EndDisabled();

  SeparatorText(i18n::Tr("Couleurs"));
  float tc[4], qc[4];
  RgbToF4(g_cfg.text_rgb, tc);
  RgbToF4(g_cfg.qty_rgb, qc);
  if (ColorEdit4WithAlphaBar(i18n::Tr("Couleur du nom"), tc)) {
    g_cfg.text_rgb = F3ToRgb(tc);
    g_needs_save = true;
  }
  if (ColorEdit4WithAlphaBar(i18n::Tr("Couleur de la quantité"), qc)) {
    g_cfg.qty_rgb = F3ToRgb(qc);
    g_needs_save = true;
  }

  ImGui::EndDisabled();

  // On ne sauvegarde qu'une édition POSÉE : sans ça, un glissement de slider
  // écrirait le yaml à chaque frame.
  if (g_needs_save && !ImGui::IsAnyItemActive()) {
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
    g_needs_save = false;
  }
}
