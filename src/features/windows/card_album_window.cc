#include "features/windows/card_album_window.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "bourgeon.h"                           // Bourgeon::Instance()
#include "d3d9/d3d9_hook.h"                     // Overlay_SetTextureFilter (POINT sur les illustrations)
#include "features/equip_slot.h"                // equipslot : bits et libellé d'emplacement
#include "features/hotkey_util.h"               // hotkeys::OpenButton (bouton + touche liée)
#include "features/item_cell.h"                 // itemcell::ExtractList, ItemRow
#include "features/windows/item_desc_window.h"  // itemdesc::CardName / CardIllustPath / RenderSimpleDesc
#include "features/windows/item_probability.h"  // itemprob : d'où tombe une carte (packageitem.lub)
#include "features/moonlight_ui/moonlight_ui.h"  // OpenInterfaceSection / SaveSettings (menu de la puce)
#include "features/windows/inventory_viewer.h"  // DraggedItemNameId (la pochette visée par un glisser)
#include "features/windows/viewer_probes.h"     // viewers::MouseOverInventory (retrait par glisser)
#include "imgui.h"
#include "ragnarok/globals.h"  // rag::kInventoryListAddr
#include "ui/card_thumb.h"     // ro::cardthumb — les vignettes des pochettes
#include "ui/icon_cache.h"     // ro::ItemIcon
#include "ui/qty_prompt.h"     // ro::OpenQuantityPrompt / QuantityPrompt
#include "ui/ro_imgui.h"       // ro::BeginRoWindow, ro::RoButton, ro::Px
#include "ui/ro_widgets.h"     // mui::IsLastItemRightClicked
#include "ui/ui_palette.h"     // ro::pal
#include "utils/i18n.h"        // i18n::Tr
#include "utils/text.h"        // text::ContainsNoCase (la recherche par nom ou par id)

namespace {

// Miroir EXACT de e_card_album_cmd côté moonlight (src/map/clif.hpp). Les deux
// listes doivent bouger ensemble : un cmd ajouté ici sans l'être là-bas est
// silencieusement ignoré par le serveur.
constexpr uint8_t kCmdRefresh = 0;
constexpr uint8_t kCmdUnlock  = 1;  // arg = index inventaire ; sacrifie 1 copie
constexpr uint8_t kCmdPut     = 2;  // arg = index inventaire
constexpr uint8_t kCmdGet     = 3;  // arg = nameid SERVEUR
constexpr uint8_t kCmdClose   = 4;  // la fenêtre se ferme : rend le verrou, pas de réponse

// Miroir de e_card_album_result (src/map/card_album.hpp).
constexpr uint8_t kResOk              = 0;
constexpr uint8_t kResFail            = 1;
constexpr uint8_t kResNotACard        = 2;
constexpr uint8_t kResLocked          = 3;
constexpr uint8_t kResAlreadyUnlocked = 4;
constexpr uint8_t kResNotEnough       = 5;
constexpr uint8_t kResInventoryFull   = 6;
constexpr uint8_t kResBusy            = 7;
constexpr uint8_t kResBound           = 8;
constexpr uint8_t kResStackFull       = 9;
constexpr uint8_t kResNoAccount       = 10;
constexpr uint8_t kResInUse           = 11;  // tenu par un autre compte de jeu du même compte Moonlight
constexpr uint8_t kResNotOpen         = 12;

// Taille d'une CARD_ALBUM_ENTRY sur le fil : [id:4][amount:2][flags:1][equip:4].
constexpr int kEntrySize = 11;

// Le type d'objet « carte » tel que le client le range dans ItemRow::type.
constexpr int kItemTypeCard = 6;

// L'inventaire ne bouge pas soixante fois par seconde ; le relire à chaque frame
// pour 300 entrées serait du gaspillage pur.
constexpr uint32_t kInvScanIntervalMs = 400;

// Combien de temps le compte rendu de la dernière commande reste affiché.
constexpr uint32_t kResultShowMs = 6000;

// Assez pour l'inventaire étendu du client (MAX_INVENTORY = 300 côté serveur).
constexpr int kInvScanMax = 500;

// Le payload d'une POCHETTE qu'on glisse hors du classeur. Le contenu importe
// peu (l'id, pour le fantôme) : c'est `drag_id_` qui compte au relâché, comme
// pour les viewers.
constexpr char kDragPayload[] = "ALBUM_CARD";

// ── Le classeur ─────────────────────────────────────────────────────────────
// Toutes en pixels LOGIQUES (ro::Px les met à l'échelle de l'interface).

// La taille d'une illustration dans sa pochette : 300×400 réduit de 2,5, la
// vignette est générée EXACTEMENT à cette taille (à l'échelle de l'interface
// près) et dessinée 1:1 — c'est la condition du pixel-perfect.
constexpr float kArtW = 120.0f;
constexpr float kArtH = 160.0f;
constexpr float kSpineW = 18.0f;       // la reliure entre les deux pages
constexpr float kPagePad = 12.0f;      // marge intérieure d'une page
constexpr float kCellGap = 10.0f;      // entre deux pochettes
constexpr float kPocketMargin = 3.0f;  // la pochette dépasse l'art de ce pas
constexpr float kPocketRounding = 5.0f;
constexpr float kArtRounding = 4.0f;
constexpr float kBarH = 9.0f;          // barre de complétion
constexpr float kNameMinScale = 0.70f; // la police d'un nom ne descend pas plus bas
constexpr float kNameIconGap  = 3.0f;  // entre l'icône de la carte et son nom
constexpr float kNameIconSize = 24.0f; // sa taille NATIVE : les icônes d'item font 24×24
constexpr uint32_t kFlipFadeMs = 140;  // fondu d'une page tournée
constexpr float kDefaultW = 960.0f;
constexpr float kDefaultH = 700.0f;
constexpr float kMinW = 720.0f;
constexpr float kMinH = 480.0f;

// Les teintes propres au classeur. Le papier et les pastilles du cash shop
// viennent du skin (ro::SkinConfig, réglables par le joueur) ; ce qui est ici
// n'a pas d'équivalent dans ui/ui_palette.h — une pochette sombre sur un corps
// clair est le contraire de ce que la palette adresse.
constexpr ImU32 kPocketBg     = IM_COL32(58, 50, 44, 255);    // le plastique de la pochette
constexpr ImU32 kPocketEdge   = IM_COL32(255, 255, 255, 28);  // son reflet
constexpr ImU32 kPocketShadow = IM_COL32(0, 0, 0, 55);
constexpr ImU32 kArtMissing   = IM_COL32(96, 88, 80, 255);    // fond quand l'illustration manque
constexpr ImU32 kPageEdge     = IM_COL32(0, 0, 0, 45);
constexpr ImU32 kSpineDark    = IM_COL32(0, 0, 0, 80);
constexpr ImU32 kBadgeStock   = IM_COL32(222, 178, 54, 255);  // réserve pleine : doré
constexpr ImU32 kBadgeEmpty   = IM_COL32(150, 146, 138, 255); // réserve vide
constexpr ImU32 kSealFill     = IM_COL32(146, 44, 44, 230);   // le cachet « scellée »
constexpr ImU32 kSealText     = IM_COL32(250, 240, 235, 255);
constexpr ImU32 kHighlight    = IM_COL32(40, 150, 60, 255);
constexpr ImU32 kDropOk       = IM_COL32(40, 150, 60, 255);
constexpr ImU32 kBarTrack     = IM_COL32(0, 0, 0, 30);
constexpr ImU32 kBarFill      = IM_COL32(222, 178, 54, 255);
constexpr ImU32 kBarEdge      = IM_COL32(0, 0, 0, 60);

// Les macarons de PROVENANCE, au coin de la pochette. Deux teintes franchement
// distinctes : à quinze pixels c'est la COULEUR qui se lit, la lettre ne fait
// que confirmer.
constexpr ImU32 kSrcOldFill    = IM_COL32(196, 138, 58, 255);   // Old Card Album
constexpr ImU32 kSrcMysticFill = IM_COL32(124, 100, 196, 255);  // Mystical Card Album
constexpr ImU32 kSrcText       = IM_COL32(255, 248, 238, 255);
constexpr ImU32 kSrcEdge       = IM_COL32(0, 0, 0, 100);
constexpr float kSrcChip       = 15.0f;  // son diamètre, en pixels logiques
constexpr float kSrcChipGap    = 2.0f;

// ── Emplacement CIBLE d'une carte : les INTERCALAIRES ───────────────────────
//
// Une carte se pose sur un type d'équipement, et c'est ainsi que les joueurs les
// rangent mentalement : « mes cartes d'armure », « mes cartes d'arme ». Le masque
// vient du serveur (le client n'a pas d'item_db) ; les bits et le libellé
// principal viennent de features/equip_slot.h, la source unique.
//
// Les noms d'emplacements restent en anglais — ce sont des termes du jeu, comme
// partout ailleurs ; seul « Tous » se traduit, à l'affichage.
// Les trois emplacements de tête tiennent dans UN intercalaire : trois onglets
// pour une famille que les joueurs pensent comme une seule (« mes cartes de
// chapeau ») encombraient la barre sans rien classer de plus — le libellé de
// chaque pochette dit toujours lequel des trois.
struct SlotFilter { const char* label; uint32_t mask; };
const SlotFilter kSlotFilters[] = {
    {"Tous",      0},
    {"Armor",     equipslot::kArmor},
    {"Weapon",    equipslot::kHandR},
    {"Shield",    equipslot::kHandL},
    {"Garment",   equipslot::kGarment},
    {"Shoes",     equipslot::kShoes},
    {"Accessory", equipslot::kAccessory},
    {"Head",      equipslot::kHeadAny},
};
constexpr int kSlotFilterCount =
    static_cast<int>(sizeof(kSlotFilters) / sizeof(kSlotFilters[0]));

// 🔴 Libellés en FRANÇAIS NU : ro::RoCombo traduit chaque entrée à sa lecture,
// donc passer du i18n::Tr ici traduirait deux fois.
const char* const kSortLabels[] = {"Catalogue", "Nom", "Réserve", "Emplacement"};
constexpr int kSortCount = static_cast<int>(sizeof(kSortLabels) / sizeof(kSortLabels[0]));

// Ce que les pages montrent : tout, les pochettes ouvertes, ou les scellées —
// celles qu'il reste à conquérir.
const char* const kShowLabels[] = {"Toutes", "Débloquées", "Scellées"};
constexpr int kShowCount = static_cast<int>(sizeof(kShowLabels) / sizeof(kShowLabels[0]));
constexpr int kShowAll = 0, kShowUnlocked = 1, kShowSealed = 2;

// Le nom d'une carte réduit à ce qui la DISTINGUE : le client nomme
// « Poring Card [Armor] », et sous une pochette de 120 pixels ni le crochet ni
// le mot « Card » ne laissent de place au nom. Deux coupes, dans cet ordre :
//
//   1. l'emplacement, au premier « [ » (les espaces qui le précèdent avec) —
//      il est déjà dit par l'intercalaire et par le tooltip ;
//   2. le suffixe « Card », que TOUTES les entrées portent : dans un album de
//      cartes il ne distingue rien, et il vole cinq caractères aux noms longs.
//
// Les deux coupes se refusent à rendre une chaîne vide : mieux vaut un nom
// bizarre qu'une pochette anonyme.
const char* DisplayName(const char* nm, char* buf, size_t cap) {
  if (nm == nullptr || nm[0] == '\0') return "(?)";
  const char* br = std::strchr(nm, '[');
  size_t n = br != nullptr ? static_cast<size_t>(br - nm) : std::strlen(nm);
  while (n > 0 && nm[n - 1] == ' ') --n;
  if (n == 0) n = std::strlen(nm);  // « [Armor] » seul : mieux vaut tout que rien

  static const char kSuffix[] = " Card";
  const size_t suflen = sizeof(kSuffix) - 1;
  if (n > suflen && _strnicmp(nm + n - suflen, kSuffix, static_cast<int>(suflen)) == 0) {
    n -= suflen;
  }

  if (n >= cap) n = cap - 1;
  std::memcpy(buf, nm, n);
  buf[n] = '\0';
  return buf;
}

ImU32 WithAlpha(ImU32 c, float a) {
  const unsigned ca = (c >> IM_COL32_A_SHIFT) & 0xFF;
  const unsigned na = static_cast<unsigned>(ca * std::clamp(a, 0.0f, 1.0f) + 0.5f);
  return (c & ~IM_COL32_A_MASK) | (na << IM_COL32_A_SHIFT);
}

// La taille d'une illustration en pixels ÉCRAN, entière : c'est aussi la taille
// de sa vignette en texels, d'où l'arrondi — une vignette de 120,6 texels
// n'existe pas.
void ArtSizePx(float* w, float* h) {
  *w = std::floor(ro::Px(kArtW) + 0.5f);
  *h = std::floor(ro::Px(kArtH) + 0.5f);
}

// Le filtre POINT, posé par callback en tête d'un lot de dessins. Le filtre
// ambiant d'ImGui est LINEAR (le backend le repose à chaque SetupRenderState) ;
// ne pas le restaurer ensuite est voulu, le skin RO dessine lui aussi en POINT.
void ImCb_PointFilter(const ImDrawList*, const ImDrawCmd*) {
  Overlay_SetTextureFilter(false);
}

// ── Le snap de la fenêtre par pochette entière ──────────────────────────────
// Même mécanique que la grille du cash shop : le rappel de contrainte ramène la
// taille demandée sur un nombre entier de pochettes, en largeur (les DEUX pages
// gagnent une colonne ensemble) comme en hauteur. `chromew/chromeh` = ce que la
// fenêtre porte autour de la grille, mesuré à la frame précédente. UN SEUL état :
// une seule fenêtre est redimensionnée à la fois.
struct SnapState {
  float cellw = 0, cellh = 0, gap = 0;
  float chromew = 0, chromeh = 0;
  bool  valid = false;
};
SnapState g_snap;
constexpr int kMinCols = 2;
constexpr int kMinRows = 1;

void SnapWindowSize(ImGuiSizeCallbackData* d) {
  const SnapState& s = g_snap;
  const float sx = s.cellw + s.gap, sy = s.cellh + s.gap;
  // Largeur : deux pages, donc la grille d'UNE page est la moitié du reste.
  const float gw = (d->DesiredSize.x - s.chromew) * 0.5f;
  const float gh = d->DesiredSize.y - s.chromeh;
  int cols = static_cast<int>((gw + s.gap) / sx + 0.5f);
  int rows = static_cast<int>((gh + s.gap) / sy + 0.5f);
  if (cols < kMinCols) cols = kMinCols;
  if (rows < kMinRows) rows = kMinRows;
  d->DesiredSize.x = s.chromew + 2.0f * (cols * sx - s.gap);
  d->DesiredSize.y = s.chromeh + rows * sy - s.gap;
}

ImU32 F4(const float* c, float alpha = 1.0f) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3] * alpha));
}

// Le côté, en PIXELS, de l'icône dessinée devant un nom de carte. Entier : une
// icône d'item est une texture de 24×24 et se pose texel sur pixel.
float NameIconPx() { return std::floor(ro::Px(kNameIconSize)); }

// Un texte sur UNE ligne, centré dans [x0..x1], en réduisant la police pour
// tenir — jusqu'à un plancher, au-delà duquel on coupe : un nom illisible ne
// vaut pas mieux qu'un nom tronqué. Repris du cash shop, qui fait de même dans
// le bandeau de ses cartes.
//
// `icon` (facultatif) se dessine DEVANT le texte, dans un carré de
// `NameIconPx()` — 24 points, la taille NATIVE de l'icône d'inventaire, qui
// passe donc 1:1. L'ensemble icône + écart + nom est centré d'un bloc, et c'est
// la place restante qui borne la police : sans cela une icône posée après coup
// décentrerait le nom ou mordrait dessus. L'icône étant plus haute qu'une
// ligne de texte, la bande vaut la plus grande des deux — c'est la même
// hauteur que `BookLayout::name_h` réserve sous la pochette.
void DrawFittedText(ImDrawList* dl, float x0, float x1, float y, ImU32 col,
                    const char* text, void* icon = nullptr, ImU32 icon_tint = 0) {
  ImFont* font = ImGui::GetFont();
  const float base = ImGui::GetFontSize();
  const float avail = x1 - x0;
  if (avail <= 1.0f) return;
  const float isz = icon != nullptr ? NameIconPx() : 0.0f;
  const float gap = icon != nullptr ? ro::Px(kNameIconGap) : 0.0f;
  const float band = std::max(base, isz);
  const float room = std::max(1.0f, avail - isz - gap);
  float tw = font->CalcTextSizeA(base, FLT_MAX, 0.0f, text).x;
  float fsz = base;
  if (tw > room && tw > 0.0f) {
    fsz = std::max(base * kNameMinScale, base * room / tw);
    tw = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, text).x;
  }
  float x = x0 + std::max(0.0f, (avail - (isz + gap + tw)) * 0.5f);
  dl->PushClipRect(ImVec2(x0, y - 1.0f), ImVec2(x1, y + band + 2.0f), true);
  if (icon != nullptr) {
    // Sur des coordonnées entières : une icône d'inventaire est une petite
    // texture, la poser sur un demi-pixel la rend floue.
    const ImVec2 i0(std::floor(x), std::floor(y + (band - isz) * 0.5f));
    dl->AddImage(reinterpret_cast<ImTextureID>(icon), i0, ImVec2(i0.x + isz, i0.y + isz),
                 ImVec2(0, 0), ImVec2(1, 1), icon_tint);
    x += isz + gap;
  }
  dl->AddText(font, fsz, ImVec2(x, y + (band - fsz) * 0.5f), col, text);
  dl->PopClipRect();
}

// Une PASTILLE : fond soutenu + texte dessus. C'est ainsi qu'une mention saillit
// sur un fond clair — un TextColored assez vif pour attirer l'œil s'y noierait.
// `anchor` = quel point de la pastille se pose sur `pos` (0,0 = coin haut
// gauche ; 1,0 = haut droit ; 0.5,1 = bas centre).
ImVec2 DrawPill(ImDrawList* dl, const ImVec2& pos, const ImVec2& anchor,
                const char* text, ImU32 fill, ImU32 text_col, float alpha) {
  ImFont* font = ImGui::GetFont();
  const float fsz = ImGui::GetFontSize() * 0.9f;
  const ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, text);
  const float px = ro::Px(6.0f), py = ro::Px(1.5f);
  const ImVec2 size(ts.x + 2.0f * px, ts.y + 2.0f * py);
  const ImVec2 p0(std::floor(pos.x - size.x * anchor.x), std::floor(pos.y - size.y * anchor.y));
  const ImVec2 p1(p0.x + size.x, p0.y + size.y);
  dl->AddRectFilled(p0, p1, WithAlpha(fill, alpha), size.y * 0.5f);
  dl->AddText(font, fsz, ImVec2(p0.x + px, p0.y + py), WithAlpha(text_col, alpha), text);
  return size;
}

// ── D'OÙ TOMBE UNE CARTE : les deux albums d'objets ─────────────────────────
//
// 🔴 Aucune donnée n'est inventée ici, et le serveur n'est pas interrogé : le
// CLIENT porte déjà la table de tirage de l'Old Card Album (616) et du Mystical
// Card Album (12246). C'est `CNeoPackageItemMgr`, alimenté au démarrage par
// `data\luafiles514\lua files\probabilityinfo\packageitem.lub` — lui-même
// engendré depuis `db/import/item_group_db.yml` du serveur (gen_packageitem.py
// du dépôt client). La même table alimente l'onglet « Probabilités » d'une
// description ; on la lit par features/windows/item_probability.h.
//
// Elle est retournée UNE fois en index carte -> chance. Neuf cents pochettes ne
// peuvent pas parcourir chacune, à chaque frame, les ~690 entrées des deux
// albums — et `itemprob::Get` prévient lui-même qu'il n'est pas fait pour ça.
constexpr uint32_t kOldAlbumId    = 616;
constexpr uint32_t kMysticAlbumId = 12246;

// Ce que l'index retient d'une carte pour UN album. `total` vaut 0 quand le
// dénominateur n'est pas unique (plusieurs tirages) : la mention « N chances sur
// M » se tait alors, plutôt que de citer un M qui ne vaut que pour une partie
// des chances. Les deux albums livrés n'ont qu'un seul groupe, mais rien dans
// le format du lub ne l'impose.
struct SrcChance {
  double pct    = 0.0;
  int    weight = 0;
  int    total  = 0;
};

struct AlbumSource {
  uint32_t    item_id;
  const char* letter;    // la lettre du macaron
  const char* fallback;  // le nom, si itemInfoMerged.lua ne connaît pas l'objet
  ImU32       fill;
  bool        built = false;
  std::unordered_map<uint32_t, SrcChance> chance;
};

AlbumSource g_sources[] = {
    {kOldAlbumId,    "O", "Old Card Album",      kSrcOldFill},
    {kMysticAlbumId, "M", "Mystical Card Album", kSrcMysticFill},
};
constexpr int kSrcCount = static_cast<int>(sizeof(g_sources) / sizeof(g_sources[0]));

// Retourne la table une fois. ⚠ `built` ne se pose QUE sur un succès : la base
// du client est créée paresseusement, et `itemprob::Get` MET EN CACHE le vide
// qu'il trouverait avant le chargement du lub. `Has` (une recherche dans un
// arbre, sans allocation) est le garde-fou qui empêche ce cache empoisonné.
void BuildSources() {
  for (AlbumSource& s : g_sources) {
    if (s.built) continue;
    if (!itemprob::Has(s.item_id)) continue;
    const itemprob::Table* t = itemprob::Get(s.item_id);
    if (t == nullptr) continue;
    for (const itemprob::Group& g : t->groups) {
      for (const itemprob::Entry& e : g.entries) {
        // Un album rend des OBJETS ; une branche invoquerait des monstres, et
        // c'est l'URL du libellé qui tranche — pas la plage d'identifiants.
        if (e.is_mob || e.id == 0) continue;
        SrcChance& c = s.chance[e.id];
        const bool first = (c.weight == 0 && c.pct == 0.0);
        c.pct += e.pct;
        c.weight += e.weight;
        c.total = first ? g.total : (c.total == g.total ? c.total : 0);
      }
    }
    s.built = true;
  }
}

const SrcChance* SourceChance(const AlbumSource& s, uint32_t card_id) {
  const auto it = s.chance.find(card_id);
  return it == s.chance.end() ? nullptr : &it->second;
}

// Le nom de l'album, tel que le client le nomme (donc traduit comme le reste du
// jeu). Le repli n'est pas décoratif : `ItemName` rend nullptr tant que
// itemInfoMerged.lua n'est pas chargé, et un macaron sans nom ne dit rien.
const char* SourceName(const AlbumSource& s) {
  const MoonlightUi* ui = Bourgeon::Instance().moonlight_ui();
  const char* n = ui != nullptr ? ui->ItemName(s.item_id) : nullptr;
  return (n != nullptr && n[0] != '\0') ? n : s.fallback;
}

}  // namespace

CardAlbumWindow::CardAlbumWindow() {
  // ZC_BOURGEON_CARD_ALBUM : au-dessus de l'opcode max du client (0x0C35), donc
  // livré par le reader-hook et jamais confondu avec un paquet natif.
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kCardAlbum);
}

// ── Fil ─────────────────────────────────────────────────────────────────────

void CardAlbumWindow::Send(uint8_t cmd, uint32_t arg, uint16_t amount,
                           uint32_t card_id) {
  // Retenu AVANT l'envoi : la réponse ne redit pas ce qu'on a demandé, elle
  // porte l'état complet et un code. C'est ici qu'on sait de quelle carte il
  // s'agissait.
  last_cmd_ = cmd;
  last_card_id_ = card_id;
  last_amount_ = amount;
  cmd_in_flight_ = true;
  SendRaw(cmd, arg, amount);
}

void CardAlbumWindow::SendRaw(uint8_t cmd, uint32_t arg, uint16_t amount) {
  // [op:2][len:2][cmd:1][arg:4][amount:2] = 11 octets.
  uint8_t pkt[11];
  *reinterpret_cast<uint16_t*>(pkt + 0) = bopcodes::kCardAlbumCmd;
  *reinterpret_cast<uint16_t*>(pkt + 2) = sizeof(pkt);
  pkt[4] = cmd;
  *reinterpret_cast<uint32_t*>(pkt + 5) = arg;
  *reinterpret_cast<uint16_t*>(pkt + 9) = amount;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

void CardAlbumWindow::RequestRefresh() {
  Send(kCmdRefresh, 0, 0, 0);
  asked_ = true;
}

void CardAlbumWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                   uint16_t len) {
  // Fil réseau : copier, rien d'autre. `len` est la longueur du payload que le
  // dispatcher transmet (total − 4), et c'est tout ce qu'on lit.
  if (opcode == bopcodes::kCardAlbum) net_inbox_.Push(opcode, data, len);
}

// ZC_BOURGEON_CARD_ALBUM, sur le fil principal. `data` = payload APRÈS
// [op:2][len:2] : [result:1][count:2] puis count × [nameid:4][amount:2]
// [flags:1][equip:4].
void CardAlbumWindow::HandlePacket(uint16_t opcode, const uint8_t* data,
                                   uint16_t len) {
  if (opcode != bopcodes::kCardAlbum) return;
  if (data == nullptr || len < 3) return;

  last_result_ = data[0];
  result_tick_ = GetTickCount();

  // Un paquet que nous n'avons PAS demandé — @storealbum, par exemple, fait
  // répondre le serveur de la même façon. Le formuler avec `last_cmd_` dirait
  // la dernière carte rangée à la main : on le dit d'après ce qui a CHANGÉ.
  const bool unsolicited = !cmd_in_flight_;
  cmd_in_flight_ = false;
  const int64_t reserve_before = total_reserve_;
  const int unlocked_before = unlocked_count_;

  // Tenu ailleurs : rien à montrer. L'état qui accompagne ce code est le cache
  // d'une session qui n'a pas la main, il ne vaut rien.
  blocked_ = (last_result_ == kResInUse);
  if (blocked_) {
    rows_.clear();
    order_.clear();
    unlocked_count_ = 0;
    total_reserve_ = 0;
    asked_ = false;
    order_dirty_ = true;
    return;
  }

  const uint16_t count = *reinterpret_cast<const uint16_t*>(data + 1);

  // Le paquet est la SEULE source de la liste : on la remplace entièrement, on ne
  // la fusionne pas. Une fusion laisserait vivre un emplacement que le serveur
  // vient de retirer de son catalogue.
  rows_.clear();
  unlocked_count_ = 0;
  total_reserve_ = 0;

  for (uint16_t i = 0; i < count; ++i) {
    const size_t off = 3 + static_cast<size_t>(i) * kEntrySize;
    // Se fier au `count` annoncé et non à la taille reçue produirait une lecture
    // hors paquet si l'un des deux mentait.
    if (off + kEntrySize > len) break;

    Row r;
    r.id       = *reinterpret_cast<const uint32_t*>(data + off);
    r.amount   = *reinterpret_cast<const uint16_t*>(data + off + 4);
    r.unlocked = (data[off + 6] & 1) != 0;
    r.equip    = *reinterpret_cast<const uint32_t*>(data + off + 7);

    if (r.unlocked) {
      unlocked_count_++;
      total_reserve_ += r.amount;
    }
    rows_.push_back(r);
  }

  asked_ = false;
  order_dirty_ = true;

  if (unsolicited) {
    // Pas de carte à désigner : le compte rendu dit combien de copies sont
    // entrées, et combien de pochettes se sont ouvertes.
    last_cmd_ = kCmdRefresh;
    unsolicited_reserve_ = total_reserve_ - reserve_before;
    unsolicited_unlocked_ = unlocked_count_ - unlocked_before;
    return;
  }
  unsolicited_reserve_ = 0;
  unsolicited_unlocked_ = 0;

  // Le sacrifice a ouvert la pochette : le reste de la pile y entre sans que le
  // joueur ait à le redemander. Le message final dira les deux.
  if (last_cmd_ == kCmdUnlock && chain_id_ != 0) {
    const bool ok = last_result_ == kResOk && last_card_id_ == chain_id_;
    if (ok && chain_amount_ > 0) {
      chained_put_ = true;
      Send(kCmdPut, static_cast<uint32_t>(chain_index_), static_cast<uint16_t>(chain_amount_),
           chain_id_);
    }
    chain_id_ = 0;
    chain_index_ = -1;
    chain_amount_ = 0;
    if (chained_put_ && ok) return;  // le compte rendu viendra avec le dépôt
  }
  if (last_cmd_ == kCmdPut && chained_put_) {
    // Consommé au message : DrawHeader lit `last_chained_` — y compris quand le
    // dépôt est REFUSÉ, car le sacrifice qui le précédait, lui, a eu lieu, et le
    // joueur doit le savoir.
    last_chained_ = true;
    chained_put_ = false;
  } else {
    last_chained_ = false;
  }

  // Un ordre qui a ABOUTI sur une carte précise : on va la montrer. Un simple
  // rafraîchissement ne déplace rien — sinon les pages tourneraient sous les
  // doigts du joueur à chaque ouverture. Un dépôt enchaîné refusé montre quand
  // même sa pochette : elle vient de s'ouvrir.
  if ((last_result_ == kResOk || last_chained_) && last_cmd_ != kCmdRefresh &&
      last_card_id_ != 0) {
    highlight_id_ = last_card_id_;
    highlight_tick_ = result_tick_;
    scroll_to_highlight_ = true;
  }
}

void CardAlbumWindow::OnModeSwitch(ModeMgr::ModeType mode_type,
                                   const char* map_name) {
  (void)map_name;
  // Ce signal part à CHAQUE changement de map, pas seulement de session. Un
  // warp ne ferme pas l'album : il est au compte, pas à la map, et le verrou
  // serveur qu'on tient reste le nôtre. Fermer ici sans le rendre le laissait
  // tenu — l'autre compte de jeu se voyait refuser l'album jusqu'à ce qu'on
  // rouvre et referme la fenêtre.
  if (mode_type == ModeMgr::ModeType::kGame) return;

  // Retour au login ou au char-select : la session de zone est finie et
  // map_quit a rendu le verrou côté serveur. Rien à envoyer, tout à jeter — un
  // battement périmé n'est pas la même chose qu'une absence.
  rows_.clear();
  order_.clear();
  inv_cards_.clear();
  unlocked_count_ = 0;
  total_reserve_ = 0;
  cat_total_ = 0;
  cat_unlocked_ = 0;
  first_ = 0;
  asked_ = false;
  confirm_index_ = -1;
  open_confirm_ = false;
  cmd_in_flight_ = false;
  chain_id_ = 0;
  chain_index_ = -1;
  chain_amount_ = 0;
  chained_put_ = false;
  drag_active_ = false;
  blocked_ = false;
  pend_active_ = false;
  pend_open_prompt_ = false;
  highlight_id_ = 0;
  highlight_tick_ = 0;
  scroll_to_highlight_ = false;
  result_tick_ = 0;
  order_dirty_ = true;
  open_ = false;
}

// ── Inventaire (ce que le joueur PEUT offrir à l'album) ─────────────────────

void CardAlbumWindow::ScanInventory() {
  inv_cards_.clear();

  static itemcell::ItemRow items[kInvScanMax];
  const int n = itemcell::ExtractList(rag::kInventoryListAddr, items, kInvScanMax);

  for (int i = 0; i < n; ++i) {
    const itemcell::ItemRow& it = items[i];
    if (it.type != kItemTypeCard) continue;

    InvCard c;
    c.id     = it.id;
    c.index  = it.index;
    c.amount = it.amount;
    std::snprintf(c.name, sizeof(c.name), "%s", it.name);
    inv_cards_.push_back(c);
  }
}

const CardAlbumWindow::Row* CardAlbumWindow::Find(uint32_t id) const {
  for (const Row& r : rows_) {
    if (r.id == id) return &r;
  }
  return nullptr;
}

const CardAlbumWindow::InvCard* CardAlbumWindow::FindInHand(uint32_t id) const {
  for (const InvCard& c : inv_cards_) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

void CardAlbumWindow::OnTick() {
  // Hors frame : la seule place sûre pour rendre au device les vignettes
  // évincées. Avant le test d'ouverture — une fenêtre qu'on vient de fermer a
  // encore des libérations en attente.
  ro::cardthumb::FlushReleases();

  if (!open_) return;

  const uint32_t now = GetTickCount();
  if (now - inv_scan_tick_ >= kInvScanIntervalMs) {
    inv_scan_tick_ = now;
    ScanInventory();
  }
}

// ── Filtrage et tri ─────────────────────────────────────────────────────────

// La recherche porte sur le NOM ou sur l'ID, dans le même champ — celui qui
// arrive de la base d'items du site tape « 4001 », celui qui joue tape
// « Poring ». C'est déjà le geste de l'atlas de fabrication (CraftAtlas::Matches),
// et l'id y est comparé comme du TEXTE : « 40 » ramène donc 4001 comme 1940,
// ce qui se resserre en tapant l'id entier.
//
// 🔴 L'id sauve au passage les cartes dont le client n'a pas le nom : un nom
// vide n'a rien à comparer, et la pochette disparaissait en silence dès qu'un
// filtre était tapé. Elle reste maintenant trouvable par son id.
bool CardAlbumWindow::MatchesFilter(uint32_t card_id) const {
  if (filter_[0] == '\0') return true;

  const char* nm = itemdesc::CardName(card_id);
  if (nm[0] != '\0' && text::ContainsNoCase(nm, filter_)) return true;

  char id_text[16];
  std::snprintf(id_text, sizeof(id_text), "%u", card_id);
  return text::ContainsNoCase(id_text, filter_);
}

void CardAlbumWindow::RebuildOrder() {
  order_dirty_ = false;
  order_.clear();
  order_.reserve(rows_.size());

  const uint32_t mask =
      (slot_filter_ > 0 && slot_filter_ < kSlotFilterCount)
          ? kSlotFilters[slot_filter_].mask
          : 0;

  cat_total_ = 0;
  cat_unlocked_ = 0;

  for (size_t i = 0; i < rows_.size(); ++i) {
    const Row& r = rows_[i];
    if (mask != 0 && (r.equip & mask) == 0) continue;

    // La complétion de l'intercalaire se mesure AVANT la recherche et le
    // filtre « débloquées » : c'est la collection, pas la vue.
    cat_total_++;
    if (r.unlocked) cat_unlocked_++;

    if (show_filter_ == kShowUnlocked && !r.unlocked) continue;
    if (show_filter_ == kShowSealed && r.unlocked) continue;

    if (!MatchesFilter(r.id)) continue;
    order_.push_back(static_cast<int>(i));
  }

  const int mode = sort_mode_;
  const std::vector<Row>& rows = rows_;

  std::stable_sort(order_.begin(), order_.end(), [&](int a, int b) {
    const Row& ra = rows[a];
    const Row& rb = rows[b];
    int cmp = 0;
    switch (mode) {
      case 1:
        cmp = std::strcmp(itemdesc::CardName(ra.id), itemdesc::CardName(rb.id));
        break;
      case 2:
        // Les mieux fournies d'abord : c'est ce qu'on cherche quand on trie
        // par réserve. Une pochette scellée passe après toutes les ouvertes.
        cmp = (ra.unlocked != rb.unlocked) ? (ra.unlocked ? -1 : 1)
              : (ra.amount > rb.amount)    ? -1
              : (ra.amount < rb.amount)    ? 1
                                           : 0;
        break;
      case 3:
        cmp = std::strcmp(equipslot::PrimaryLabel(ra.equip), equipslot::PrimaryLabel(rb.equip));
        break;
      default:
        break;  // catalogue : l'ordre du serveur, par id
    }
    // Départage stable par id : sans cela deux cartes de même réserve
    // changeraient de place d'un rafraîchissement à l'autre.
    if (cmp == 0) cmp = (ra.id < rb.id) ? -1 : (ra.id > rb.id) ? 1 : 0;
    return cmp < 0;
  });

  // ⚠ Pas de retour à la première page ici : RebuildOrder tourne aussi à chaque
  // réponse du serveur, et un dépôt ne doit pas renvoyer le joueur au début du
  // classeur. C'est un changement de VUE (recherche, intercalaire, tri) qui
  // ramène en page 1, et ce sont ses widgets qui le font.
}

// ── Actions à quantité ──────────────────────────────────────────────────────

void CardAlbumWindow::ArmMove(uint8_t cmd, uint32_t arg, uint32_t card_id,
                              int max_amount, bool ask_quantity) {
  if (max_amount <= 0) return;

  pend_active_ = true;
  pend_cmd_ = cmd;
  pend_arg_ = arg;
  pend_card_id_ = card_id;
  pend_max_ = max_amount;
  // Une pile de 1 n'a pas de quantité à choisir : le dialogue serait un clic
  // supplémentaire pour une seule réponse possible.
  pend_open_prompt_ = ask_quantity && max_amount > 1;

  if (!pend_open_prompt_) {
    Send(pend_cmd_, pend_arg_, static_cast<uint16_t>(max_amount), pend_card_id_);
    pend_active_ = false;
  }
}

void CardAlbumWindow::PumpQuantityPrompt() {
  if (pend_active_ && pend_open_prompt_) {
    // 🔴 L'ouverture RÉELLE se fait ici, dans la pile d'ID de la fenêtre. Un
    // ImGui::OpenPopup lancé depuis le menu contextuel ne l'atteindrait pas.
    ro::OpenQuantityPrompt(this);
    pend_open_prompt_ = false;
  }

  // Destination NOMMÉE : « Retirer » seul ne dirait pas où part la carte.
  const char* verb = (pend_cmd_ == kCmdGet) ? i18n::Tr("Vers l'inventaire")
                                            : i18n::Tr("Vers l'album");
  bool cancelled = false;
  const int qty = ro::QuantityPrompt(this, verb, pend_max_, &cancelled);

  if (qty > 0) {
    Send(pend_cmd_, pend_arg_, static_cast<uint16_t>(qty), pend_card_id_);
    pend_active_ = false;
  } else if (cancelled) {
    pend_active_ = false;
  }
}

void CardAlbumWindow::OfferFromInventory(int client_index) {
  // Lecture FRAÎCHE : l'index vient de l'inventaire à l'instant, pas de notre
  // balayage d'il y a 400 ms.
  ScanInventory();
  inv_scan_tick_ = GetTickCount();
  for (const InvCard& c : inv_cards_) {
    if (c.index != client_index) continue;
    OfferCard({c.id, c.index, c.amount});
    return;
  }
  // Pas une carte de monstre : le même refus que le serveur donnerait, sans
  // faire le voyage.
  last_result_ = kResNotACard;
  last_cmd_ = kCmdPut;
  result_tick_ = GetTickCount();
}

void CardAlbumWindow::RouteDragRelease() {
  if (!drag_active_) return;
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  if (pl != nullptr && pl->IsDataType(kDragPayload)) {
    // Le glisser court encore : on suit la souris.
    const ImVec2 m = ImGui::GetMousePos();
    drag_mx_ = m.x;
    drag_my_ = m.y;
    return;
  }
  drag_active_ = false;  // relâché ce frame
  // Lâché sur l'inventaire = retrait. Une pile demande combien, comme le
  // retrait de l'entrepôt. Lâché ailleurs = rien.
  if (drag_id_ != 0 && viewers::MouseOverInventory(drag_mx_, drag_my_)) {
    ArmMove(kCmdGet, drag_id_, drag_id_, drag_amount_, true);
  }
  drag_id_ = 0;
}

void CardAlbumWindow::OfferCard(const Offer& c) {
  // Le catalogue porte TOUTES les cartes que l'album accepte : ce qui n'y est
  // pas n'est pas une carte de monstre (un enchant est de type 6 lui aussi) et
  // n'a pas de pochette à sceller ni à ouvrir. Le refuser ici évite une modale
  // de sacrifice pour un objet que le serveur refusera de toute façon.
  if (rows_.empty()) {
    RequestRefresh();
    return;
  }
  const Row* row = Find(c.id);
  if (row == nullptr) {
    last_result_ = kResNotACard;
    last_cmd_ = kCmdPut;
    result_tick_ = GetTickCount();
    return;
  }
  if (row->unlocked) {
    // La PILE ENTIÈRE, sans demander : offrir une carte à l'album, c'est la
    // ranger. La quantité partielle reste possible par le menu de la pochette
    // (« Depuis l'inventaire... »).
    ArmMove(kCmdPut, static_cast<uint32_t>(c.index), c.id, c.amount, false);
    return;
  }
  // Pochette scellée : le sacrifice, par son unique porte — et le RESTE de la
  // pile suit dès que la pochette est ouverte.
  RequestSacrifice(c.index, c.id, c.amount - 1);
}

void CardAlbumWindow::RequestSacrifice(int client_index, uint32_t card_id, int rest) {
  if (client_index < 0 || card_id == 0) return;
  // Ce qui suivra le sacrifice : les copies restantes de la pile, rangées dans
  // la pochette qu'il vient d'ouvrir. Consommé (ou abandonné) à la réponse.
  chain_index_ = client_index;
  chain_id_ = card_id;
  chain_amount_ = std::max(0, rest);
  if (auto_sacrifice_) {
    // Le joueur a choisi de ne plus être interrogé : la première copie part
    // tout de suite. Le compte rendu (pochette DÉBLOQUÉE, pochette surlignée)
    // reste, c'est lui qui dit ce qui vient de se passer.
    Send(kCmdUnlock, static_cast<uint32_t>(client_index), 1, card_id);
    return;
  }
  // 🔴 Sinon, jamais sans confirmation : la carte est consommée pour de bon.
  confirm_index_ = client_index;
  confirm_id_ = card_id;
  open_confirm_ = true;
}

void CardAlbumWindow::Flip(int delta) {
  if (per_spread_ <= 0 || delta == 0) return;
  const int total = static_cast<int>(order_.size());
  const int spreads = std::max(1, (total + per_spread_ - 1) / per_spread_);
  int page = first_ / per_spread_ + delta;
  page = std::clamp(page, 0, spreads - 1);
  const int nf = page * per_spread_;
  if (nf != first_) {
    first_ = nf;
    flip_tick_ = GetTickCount();
  }
}

// ── Ouverture ───────────────────────────────────────────────────────────────

void CardAlbumWindow::Open() {
  if (open_) return;
  // Éteinte dans les réglages, la fenêtre n'existe pas : s'ouvrir quand même
  // enverrait un refresh que le serveur jette (bit UiCaps absent) et laisserait
  // un clic mort. Le raccourci ne doit rien faire, comme pour le carnet MVP.
  if (!imgui_enabled_) return;
  open_ = true;
  blocked_ = false;
  // On ne DESSINE rien tant que le serveur n'a pas répondu : l'album est son
  // état, pas le nôtre.
  RequestRefresh();
  ScanInventory();
  inv_scan_tick_ = GetTickCount();
}

void CardAlbumWindow::Close() {
  // Rendre le verrou : tant qu'on le tient, aucun autre compte de jeu du même
  // compte Moonlight ne peut ouvrir son album. Le paquet n'a pas de réponse.
  if (open_) SendRaw(kCmdClose, 0, 0);
  open_ = false;
  confirm_index_ = -1;
  open_confirm_ = false;
}

void CardAlbumWindow::Toggle() {
  if (open_) Close();
  else Open();
}

// ── Rendu ───────────────────────────────────────────────────────────────────

const char* CardAlbumWindow::ResultText(uint8_t result) {
  switch (result) {
    case kResOk:              return i18n::Tr("C'est fait.");
    case kResNotACard:        return i18n::Tr("Seules les cartes de monstre entrent dans l'album.");
    case kResLocked:          return i18n::Tr("Cet emplacement n'est pas encore débloqué : sacrifiez d'abord une carte.");
    case kResAlreadyUnlocked: return i18n::Tr("Cet emplacement est déjà débloqué.");
    case kResNotEnough:       return i18n::Tr("Il n'y en a pas autant en réserve.");
    case kResInventoryFull:   return i18n::Tr("Votre inventaire est plein.");
    case kResBusy:            return i18n::Tr("Impossible pendant un échange ou avec un entrepôt ouvert.");
    case kResBound:           return i18n::Tr("Une carte liée ou en location ne peut pas être rangée.");
    case kResStackFull:       return i18n::Tr("La réserve de cette carte est pleine.");
    case kResNoAccount:       return i18n::Tr("Ce compte de jeu n'est rattaché à aucun compte Moonlight : l'album ne peut pas être enregistré.");
    case kResInUse:           return i18n::Tr("L'album est déjà ouvert sur un autre de vos comptes de jeu. Fermez-le là-bas d'abord.");
    case kResNotOpen:         return i18n::Tr("L'album n'est pas ouvert : rouvrez la fenêtre.");
    default:                  return i18n::Tr("Le serveur a refusé l'opération.");
  }
}

void CardAlbumWindow::OnRenderUI() {
  if (!open_ || !imgui_enabled_) return;

  // Ouvre le budget de vignettes de la frame (et jette le cache si le device a
  // été recréé). UNE fois, avant toute pochette.
  ro::cardthumb::BeginFrame();

  // L'index des provenances, retourné une fois pour toutes (deux drapeaux à
  // tester quand il est prêt). Ici et pas au constructeur : la base de tirage du
  // client est créée paresseusement, la construire trop tôt fixerait du vide.
  if (sources_) BuildSources();

  ImGui::SetNextWindowSize(ImVec2(ro::Px(kDefaultW), ro::Px(kDefaultH)),
                           ImGuiCond_FirstUseEver);
  // Le snap par pochette dès que le chrome est mesuré (frame précédente) ; avant
  // cela, un simple plancher.
  if (g_snap.valid) {
    const float sx = g_snap.cellw + g_snap.gap, sy = g_snap.cellh + g_snap.gap;
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(g_snap.chromew + 2.0f * (kMinCols * sx - g_snap.gap),
               g_snap.chromeh + kMinRows * sy - g_snap.gap),
        ImVec2(FLT_MAX, FLT_MAX), SnapWindowSize);
  } else {
    ImGui::SetNextWindowSizeConstraints(ImVec2(ro::Px(kMinW), ro::Px(kMinH)),
                                        ImVec2(FLT_MAX, FLT_MAX));
  }

  // Id stable (###) : le titre porte la complétion, qui change à chaque
  // sacrifice — sans lui, ImGui perdrait position et taille à chaque carte
  // débloquée.
  char title[128];
  std::snprintf(title, sizeof(title), "%s  %d / %d###bourgeon_card_album",
                i18n::Tr("Album de cartes"), unlocked_count_,
                static_cast<int>(rows_.size()));

  // La puce de la barre de titre ouvre les options de CETTE fenêtre.
  ro::SetNextWindowTitleBullet(i18n::Tr("Options de l'album"));

  bool open = open_;
  const bool begun = ro::BeginRoWindow(title, &open);
  if (begun) {
    if (ro::TitleBulletClicked()) ImGui::OpenPopup("album_opts");
    DrawBulletMenu();

    if (order_dirty_) RebuildOrder();

    // La taille de la fenêtre, pour que DrawBook déduise le chrome du snap.
    win_w_ = ImGui::GetWindowSize().x;
    win_h_ = ImGui::GetWindowSize().y;

    DrawHeader();
    DrawTabs();
    DrawCompletion();

    // Le classeur prend tout le reste.
    DrawBook(std::max(ro::Px(160.0f), ImGui::GetContentRegionAvail().y));

    // Le rect écran, pour que l'inventaire sache qu'un glisser s'est relâché
    // sur nous (viewers::MouseOverAlbum).
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    win_rect_.Capture(wp.x, wp.y, ws.x, ws.y);

    RouteDragRelease();

    // Déclarés au niveau de la FENÊTRE, pas dans une boucle de pochettes : c'est
    // ce qui empêche le modal de se repositionner sur le curseur à chaque frame,
    // et ce qui met le prompt de quantité dans la bonne pile d'ID.
    DrawConfirmModal();
    PumpQuantityPrompt();
  } else {
    win_rect_.Invalidate();
  }
  ro::EndRoWindow();

  if (!open) Close();
}

void CardAlbumWindow::DrawHeader() {
  // Assez large pour que le texte d'aide tienne en entier : c'est lui qui
  // apprend au joueur que l'id marche aussi.
  ImGui::SetNextItemWidth(ro::Px(215.0f));
  // Chaque changement de VUE ramène en page 1 : c'est le seul endroit qui ait
  // encore un sens quand le contenu des pages change.
  if (ImGui::InputTextWithHint("##album_filter",
                               i18n::Tr("Rechercher une carte : nom ou id"),
                               filter_, sizeof(filter_))) {
    order_dirty_ = true;
    first_ = 0;
  }

  ImGui::SameLine();
  ImGui::TextUnformatted(i18n::Tr("Ordre"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ro::Px(120.0f));
  if (ro::RoCombo("##album_sort", &sort_mode_, kSortLabels, kSortCount)) {
    order_dirty_ = true;
    first_ = 0;
  }

  ImGui::SameLine();
  ImGui::TextUnformatted(i18n::Tr("Afficher"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ro::Px(110.0f));
  if (ro::RoCombo("##album_show", &show_filter_, kShowLabels, kShowCount)) {
    order_dirty_ = true;
    first_ = 0;
  }

  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Rafraîchir"))) RequestRefresh();

  // Le compte rendu de la dernière commande, le temps d'être lu, à droite de la
  // rangée — là où l'œil va après avoir cliqué.
  //
  // 🔴 Le SUCCÈS s'affiche autant que l'échec. Le client natif ne dit que « 1
  // card est supprimée » : pour un sacrifice, c'est exactement la moitié
  // trompeuse de l'information — il annonce ce qu'on a perdu et tait ce qu'on a
  // gagné. Un refus muet, lui, ferait croire à un bug du client.
  if (result_tick_ == 0 || GetTickCount() - result_tick_ >= kResultShowMs) return;

  char msg[192];
  if (last_result_ != kResOk) {
    ImGui::SameLine();
    if (last_chained_ && last_cmd_ == kCmdPut) {
      // Le sacrifice a eu lieu, le dépôt qui suivait non : les deux se disent,
      // sinon le joueur croit avoir perdu sa carte pour rien.
      char nbuf[96];
      const char* nm = itemdesc::CardName(last_card_id_);
      nm = nm[0] != '\0' ? DisplayName(nm, nbuf, sizeof(nbuf)) : i18n::Tr("Cette carte");
      std::snprintf(msg, sizeof(msg),
                    i18n::Tr("%s : pochette DÉBLOQUÉE, mais les copies restantes n'ont pas "
                             "été rangées : %s"),
                    nm, ResultText(last_result_));
      ImGui::TextColored(ro::pal::kWarn, "%s", msg);
    } else {
      ImGui::TextColored(ro::pal::kRed, "%s", ResultText(last_result_));
    }
    return;
  }

  // Un rafraîchissement demandé n'est pas un événement. Un état reçu SANS
  // demande (@storealbum) l'est : on dit ce qui a changé, s'il a changé.
  if (last_cmd_ == kCmdRefresh) {
    if (unsolicited_reserve_ <= 0 && unsolicited_unlocked_ <= 0) return;
    std::snprintf(msg, sizeof(msg),
                  i18n::Tr("Album mis à jour : %lld copie(s) rangée(s), %d pochette(s) "
                           "ouverte(s)."),
                  static_cast<long long>(unsolicited_reserve_), unsolicited_unlocked_);
    ImGui::SameLine();
    ImGui::TextColored(ro::pal::kGreen, "%s", msg);
    return;
  }

  char nbuf[96];
  const char* nm = itemdesc::CardName(last_card_id_);
  nm = nm[0] != '\0' ? DisplayName(nm, nbuf, sizeof(nbuf)) : i18n::Tr("Cette carte");

  switch (last_cmd_) {
    case kCmdUnlock:
      std::snprintf(msg, sizeof(msg),
                    i18n::Tr("%s : pochette DÉBLOQUÉE. Vous pouvez désormais y "
                             "ranger vos copies."),
                    nm);
      break;
    case kCmdPut:
      if (last_chained_) {
        std::snprintf(msg, sizeof(msg),
                      i18n::Tr("%s : pochette DÉBLOQUÉE, et %d copie(s) rangée(s)."), nm,
                      static_cast<int>(last_amount_));
      } else {
        std::snprintf(msg, sizeof(msg), i18n::Tr("%s : %d rangée(s) dans l'album."),
                      nm, static_cast<int>(last_amount_));
      }
      break;
    case kCmdGet:
      std::snprintf(msg, sizeof(msg), i18n::Tr("%s : %d reprise(s) de l'album."),
                    nm, static_cast<int>(last_amount_));
      break;
    default:
      return;
  }
  ImGui::SameLine();
  ImGui::TextColored(ro::pal::kGreen, "%s", msg);
}

// Le menu de la puce : les réglages qui AGISSENT, à portée de main, et le
// chemin vers le panneau pour le reste. Un réglage changé ici est sauvé tout de
// suite, comme depuis le panneau.
void CardAlbumWindow::DrawBulletMenu() {
  if (!ImGui::BeginPopup("album_opts")) return;
  bool changed = false;
  if (ro::RoCheckbox(i18n::Tr("Sacrifier sans confirmation"), &auto_sacrifice_)) changed = true;
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::Tr("Rafraîchir"))) RequestRefresh();
  if (ImGui::MenuItem(i18n::Tr("Réglages de l'album..."))) {
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) {
      mu->OpenInterfaceSection(MoonlightUi::kIfaceCardAlbum);
    }
  }
  ImGui::EndPopup();
  if (changed) {
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
  }
}

// Les intercalaires : un onglet par emplacement d'équipement. Le survol d'un
// onglet dit combien de cartes de cette famille sont acquises.
void CardAlbumWindow::DrawTabs() {
  if (!ro::RoBeginTabBar("album_tabs")) return;
  for (int i = 0; i < kSlotFilterCount; ++i) {
    const char* label = (i == 0) ? i18n::Tr("Tous") : kSlotFilters[i].label;
    if (ImGui::BeginTabItem(label)) {
      if (slot_filter_ != i) {
        slot_filter_ = i;
        order_dirty_ = true;
        first_ = 0;
      }
      ImGui::EndTabItem();
    }
    if (ImGui::IsItemHovered() && !rows_.empty()) {
      int tot = 0, got = 0;
      const uint32_t mask = kSlotFilters[i].mask;
      for (const Row& r : rows_) {
        if (mask != 0 && (r.equip & mask) == 0) continue;
        tot++;
        if (r.unlocked) got++;
      }
      char tip[96];
      std::snprintf(tip, sizeof(tip), i18n::Tr("%d / %d cartes acquises"), got, tot);
      ImGui::SetTooltip("%s", tip);
    }
  }
  ro::RoEndTabBar();
}

// La barre de complétion de l'intercalaire actif, et le total en réserve.
void CardAlbumWindow::DrawCompletion() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  const float lh = ImGui::GetTextLineHeight();
  const float bar_h = ro::Px(kBarH);

  char left[128];
  const float pct = cat_total_ > 0 ? 100.0f * cat_unlocked_ / cat_total_ : 0.0f;
  if (slot_filter_ == 0) {
    std::snprintf(left, sizeof(left), i18n::Tr("%d / %d cartes  ·  %.1f %%"),
                  cat_unlocked_, cat_total_, pct);
  } else {
    std::snprintf(left, sizeof(left), i18n::Tr("%s : %d / %d  ·  %.1f %%"),
                  kSlotFilters[slot_filter_].label, cat_unlocked_, cat_total_, pct);
  }
  char right[96];
  std::snprintf(right, sizeof(right), i18n::Tr("En réserve : %lld"),
                static_cast<long long>(total_reserve_));

  const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
  const ImU32 label = ImGui::ColorConvertFloat4ToU32(ro::pal::kLabel);
  dl->AddText(p0, text, left);
  const float rw = ImGui::CalcTextSize(right).x;
  dl->AddText(ImVec2(p0.x + w - rw, p0.y), label, right);

  // La barre sous les chiffres : dorée comme les pastilles de réserve.
  const float by = p0.y + lh + ro::Px(3.0f);
  const float fill = cat_total_ > 0 ? w * cat_unlocked_ / cat_total_ : 0.0f;
  dl->AddRectFilled(ImVec2(p0.x, by), ImVec2(p0.x + w, by + bar_h), kBarTrack, bar_h * 0.5f);
  if (fill > 0.0f) {
    dl->AddRectFilled(ImVec2(p0.x, by), ImVec2(p0.x + std::max(fill, bar_h), by + bar_h),
                      kBarFill, bar_h * 0.5f);
  }
  dl->AddRect(ImVec2(p0.x, by), ImVec2(p0.x + w, by + bar_h), kBarEdge, bar_h * 0.5f);

  ImGui::Dummy(ImVec2(w, lh + ro::Px(3.0f) + bar_h + ro::Px(4.0f)));
}

// ── Le classeur ─────────────────────────────────────────────────────────────

void CardAlbumWindow::DrawBook(float height) {
  ImGui::BeginChild("album_book", ImVec2(0, height), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();

  // Les illustrations se dessinent en POINT, à l'échelle 1:1 de leur vignette :
  // le filtre ambiant d'ImGui est LINEAR et les rendait floues. Posé une fois
  // par lot, en tête de la liste du classeur.
  dl->AddCallback(ImCb_PointFilter, nullptr);

  // ── Géométrie : la pochette a UNE taille, la fenêtre s'y plie ────────────
  // Autant de colonnes et de rangées que la place en offre, jamais une pochette
  // coupée : c'est le snap par palier (SnapWindowSize) qui tient la fenêtre sur
  // des nombres entiers, comme la grille du cash shop.
  BookLayout lay;
  const float spine = ro::Px(kSpineW);
  const float pad = ro::Px(kPagePad);
  const float gap = ro::Px(kCellGap);
  const float margin = ro::Px(kPocketMargin);
  const float footer_h = ImGui::GetFrameHeight() + ro::Px(6.0f);
  lay.page_w = std::floor((avail.x - spine) * 0.5f);
  lay.page_h = avail.y;
  // L'icône devant le nom est plus haute qu'une ligne de texte : la bande la
  // loge, sinon elle déborderait sur la pochette d'en dessous.
  lay.name_h = std::max(ImGui::GetTextLineHeight(), name_icon_ ? NameIconPx() : 0.0f) +
               ro::Px(3.0f);
  const float inner_w = lay.page_w - 2.0f * pad;
  const float inner_h = lay.page_h - 2.0f * pad - footer_h;

  ArtSizePx(&lay.art_w, &lay.art_h);
  lay.cell_w = lay.art_w + 2.0f * margin;
  lay.cell_h = lay.art_h + 2.0f * margin + lay.name_h;
  lay.cols = std::max(1, static_cast<int>(std::floor((inner_w + gap) / (lay.cell_w + gap))));
  lay.rows = std::max(1, static_cast<int>(std::floor((inner_h + gap) / (lay.cell_h + gap))));
  lay.per_page = lay.cols * lay.rows;
  lay.per_spread = 2 * lay.per_page;
  per_spread_ = lay.per_spread;

  // Ce que la fenêtre porte AUTOUR de la grille, pour le snap de la frame
  // suivante : tout ce qui n'est pas pochette ni interstice.
  {
    SnapState& s = g_snap;
    s.cellw = lay.cell_w;
    s.cellh = lay.cell_h;
    s.gap = gap;
    s.chromew = win_w_ - avail.x + 4.0f * pad + spine;
    s.chromeh = win_h_ - avail.y + 2.0f * pad + footer_h;
    s.valid = true;
  }

  // ── Pagination ──────────────────────────────────────────────────────────
  const int total = static_cast<int>(order_.size());
  const int spreads = std::max(1, (total + lay.per_spread - 1) / lay.per_spread);

  if (scroll_to_highlight_) {
    for (int k = 0; k < total; ++k) {
      if (rows_[order_[k]].id == highlight_id_) {
        const int nf = (k / lay.per_spread) * lay.per_spread;
        if (nf != first_) flip_tick_ = GetTickCount();
        first_ = nf;
        break;
      }
    }
    scroll_to_highlight_ = false;
  }
  // Le surlignage a une fin. Sans ce désarmement, une demande de défilement
  // que personne n'a consommée resterait en attente et ferait sauter la vue
  // bien plus tard, sans raison visible pour le joueur.
  if (highlight_tick_ != 0 && GetTickCount() - highlight_tick_ >= kResultShowMs) {
    highlight_tick_ = 0;
    highlight_id_ = 0;
  }
  // Recalé sur une double page entière : un redimensionnement change le nombre
  // de pochettes par page, et `first_` doit rester un début de double page.
  first_ = std::clamp((first_ / lay.per_spread) * lay.per_spread, 0,
                      (spreads - 1) * lay.per_spread);

  // Molette sur le classeur = tourner ; flèches et Page↑/↓ quand la fenêtre a
  // le clavier et qu'aucun champ n'est en saisie.
  const ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.MouseWheel != 0.0f &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
    Flip(io.MouseWheel < 0.0f ? 1 : -1);
  }
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_PageUp)) Flip(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_PageDown)) Flip(1);
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) Flip(-spreads);
    if (ImGui::IsKeyPressed(ImGuiKey_End)) Flip(spreads);
  }

  // Fondu de la page tournée : les pochettes apparaissent en 140 ms. C'est peu,
  // et c'est ce qui fait « tourner » plutôt que « clignoter ».
  float alpha = 1.0f;
  if (flip_tick_ != 0) {
    const uint32_t dt = GetTickCount() - flip_tick_;
    alpha = dt >= kFlipFadeMs ? 1.0f : static_cast<float>(dt) / kFlipFadeMs;
    if (dt >= kFlipFadeMs) flip_tick_ = 0;
  }

  // ── Les deux pages et la reliure ────────────────────────────────────────
  const ImVec2 left_p0 = p0;
  const ImVec2 right_p0(p0.x + lay.page_w + spine, p0.y);
  DrawPage(dl, left_p0, lay, first_, true, alpha);
  DrawPage(dl, right_p0, lay, first_ + lay.per_page, false, alpha);

  // La reliure : une ombre qui descend dans le pli de chaque côté.
  const float sx0 = p0.x + lay.page_w, sx1 = sx0 + spine;
  const float smid = (sx0 + sx1) * 0.5f;
  dl->AddRectFilledMultiColor(ImVec2(sx0, p0.y), ImVec2(smid, p0.y + lay.page_h),
                              IM_COL32(0, 0, 0, 0), kSpineDark, kSpineDark, IM_COL32(0, 0, 0, 0));
  dl->AddRectFilledMultiColor(ImVec2(smid, p0.y), ImVec2(sx1, p0.y + lay.page_h),
                              kSpineDark, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), kSpineDark);

  // Le classeur est vide : dire pourquoi, au milieu.
  if (rows_.empty() || total == 0) {
    const char* msg = blocked_ ? ResultText(kResInUse)
                      : rows_.empty()
                          ? (asked_ ? i18n::Tr("Chargement…")
                                    : i18n::Tr("Aucune donnée. Cliquez sur Rafraîchir."))
                          : i18n::Tr("Aucune carte ne correspond.");
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(p0.x + (avail.x - ts.x) * 0.5f, p0.y + (avail.y - ts.y) * 0.5f),
                ImGui::ColorConvertFloat4ToU32(ro::pal::kLabel), msg);
  }

  ImGui::EndChild();
}

void CardAlbumWindow::DrawPage(ImDrawList* dl, const ImVec2& p0, const BookLayout& lay,
                               int first, bool left, float alpha) {
  const float pad = ro::Px(kPagePad);
  const float gap = ro::Px(kCellGap);
  const ImVec2 p1(p0.x + lay.page_w, p0.y + lay.page_h);
  const float rounding = ro::Px(3.0f);

  // Le papier : la couleur « carte » du skin, celle des cartes du cash shop.
  const ro::RoSkinConfig& sc = ro::SkinConfig();
  dl->AddRectFilled(p0, p1, F4(sc.card_col), rounding);
  // L'ombre du pli, côté reliure.
  const float sh = ro::Px(16.0f);
  if (left) {
    dl->AddRectFilledMultiColor(ImVec2(p1.x - sh, p0.y), p1, IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, 40), IM_COL32(0, 0, 0, 40), IM_COL32(0, 0, 0, 0));
  } else {
    dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + sh, p1.y), IM_COL32(0, 0, 0, 40),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 40));
  }
  dl->AddRect(p0, p1, kPageEdge, rounding);

  // Les pochettes, centrées dans la page.
  const float grid_w = lay.cols * lay.cell_w + (lay.cols - 1) * gap;
  const float x0 = p0.x + pad + std::max(0.0f, (lay.page_w - 2.0f * pad - grid_w) * 0.5f);
  const float y0 = p0.y + pad;
  const int total = static_cast<int>(order_.size());
  for (int i = 0; i < lay.per_page; ++i) {
    const int k = first + i;
    if (k >= total) break;
    const int col = i % lay.cols, row = i / lay.cols;
    const ImVec2 pos(std::floor(x0 + col * (lay.cell_w + gap)),
                     std::floor(y0 + row * (lay.cell_h + gap)));
    DrawPocket(dl, pos, lay, k, alpha);
  }

  // Le pied de page : la flèche du côté extérieur, le numéro au centre.
  const float fy = p1.y - ImGui::GetFrameHeight() - ro::Px(4.0f);
  const int page_no = (lay.per_page > 0) ? first / lay.per_page + 1 : 1;
  char num[32];
  std::snprintf(num, sizeof(num), "%d", page_no);
  const ImVec2 ns = ImGui::CalcTextSize(num);
  dl->AddText(ImVec2(p0.x + (lay.page_w - ns.x) * 0.5f,
                     fy + (ImGui::GetFrameHeight() - ns.y) * 0.5f),
              ImGui::ColorConvertFloat4ToU32(ro::pal::kLabel), num);

  // « < » et « > » en ASCII : les triangles U+25C0/25B6 ne sont pas dans la
  // police d'interface et ressortaient en emoji de secours.
  const float bw = std::max(ro::Px(26.0f), ro::SmallButtonWidth("<"));
  if (left) {
    if (first_ > 0) {
      ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, fy + ro::Px(2.0f)));
      if (ro::RoSmallButton("<", bw)) Flip(-1);
    }
  } else {
    if (first + lay.per_page < total) {
      ImGui::SetCursorScreenPos(ImVec2(p1.x - pad - bw, fy + ro::Px(2.0f)));
      if (ro::RoSmallButton(">", bw)) Flip(1);
    }
  }
}

void CardAlbumWindow::DrawPocket(ImDrawList* dl, const ImVec2& pos, const BookLayout& lay,
                                 int order_index, float alpha) {
  const int idx = order_[order_index];
  const Row& r = rows_[idx];
  char nbuf[96];
  const char* shown = DisplayName(itemdesc::CardName(r.id), nbuf, sizeof(nbuf));
  const float margin = ro::Px(kPocketMargin);

  ImGui::PushID(idx);

  // Un seul item ImGui pour toute la cellule (pochette + nom) : c'est lui qui
  // porte survol, clics, menu et dépôt. Tout le reste est dessiné.
  //
  // ⚠ SetNextItemAllowOverlap : sans lui, un item soumis PAR-DESSUS cette
  // cellule (les macarons de provenance, plus bas) ne recevrait jamais le
  // survol — ImGui donne la main au premier item soumis, pas au dernier.
  ImGui::SetCursorScreenPos(pos);
  if (sources_) ImGui::SetNextItemAllowOverlap();
  const bool pressed = ImGui::InvisibleButton("pk", ImVec2(lay.cell_w, lay.cell_h));
  const bool hovered = ImGui::IsItemHovered();
  // 🔴 Le clic DROIT se lit ICI, avec les deux autres, et pas au moment de s'en
  // servir : `IsLastItemRightClicked` passe par `IsItemClicked`, qui interroge
  // `g.LastItemData` — le DERNIER item soumis. Les macarons de provenance sont
  // soumis plus bas, et le test posé après eux interrogeait le macaron : le menu
  // contextuel d'une pochette portant un macaron ne s'ouvrait plus. C'est le
  // même mécanisme que le piège de `BeginDragDropSource`, plus bas.
  const bool rclicked = mui::IsLastItemRightClicked();
  const bool dragging = ImGui::GetDragDropPayload() != nullptr;

  const ImVec2 pk0 = pos;
  const ImVec2 pk1(pos.x + lay.cell_w, pos.y + lay.art_h + 2.0f * margin);
  const ImVec2 a0(pk0.x + margin, pk0.y + margin);
  const ImVec2 a1(a0.x + lay.art_w, a0.y + lay.art_h);
  const float pr = ro::Px(kPocketRounding), ar = ro::Px(kArtRounding);

  // La pochette : une ombre, le plastique sombre, son reflet.
  dl->AddRectFilled(ImVec2(pk0.x, pk0.y + ro::Px(2.0f)), ImVec2(pk1.x, pk1.y + ro::Px(2.0f)),
                    WithAlpha(kPocketShadow, alpha), pr);
  dl->AddRectFilled(pk0, pk1, WithAlpha(kPocketBg, alpha), pr);
  dl->AddRect(pk0, pk1, WithAlpha(kPocketEdge, alpha), pr);

  // L'illustration — en couleur si la pochette est ouverte, en silhouette
  // sinon. Le chemin n'est lu qu'à la première demande de cette clé.
  const ro::cardthumb::Thumb t = ro::cardthumb::Get(
      r.id, itemdesc::CardIllustPath(r.id), !r.unlocked,
      static_cast<int>(lay.art_w), static_cast<int>(lay.art_h));
  if (t.tex != nullptr && t.w > 0 && t.h > 0) {
    // 1:1 et sur des coordonnées ENTIÈRES : la vignette a été générée à la
    // taille de la pochette, chaque texel tombe sur un pixel. Les quatre
    // illustrations 300×240 sont plus courtes que la pochette et se centrent.
    const float w = static_cast<float>(t.w), h = static_cast<float>(t.h);
    const ImVec2 f0(std::floor(a0.x + (lay.art_w - w) * 0.5f), std::floor(a0.y + (lay.art_h - h) * 0.5f));
    const ImVec2 f1(f0.x + w, f0.y + h);
    // La luminosité du skin s'applique aux images d'une fenêtre RO, comme aux
    // icônes ; le fondu de page s'y ajoute.
    const ImU32 tint = WithAlpha(ro::SkinImageTint(), alpha);
    dl->AddImageRounded(reinterpret_cast<ImTextureID>(t.tex), f0, f1, ImVec2(0, 0),
                        ImVec2(1, 1), tint, ar);
  } else {
    // Pas d'illustration (ou pas encore) : un fond uni et l'icône d'inventaire
    // agrandie, le temps que la vignette arrive — ou pour de bon si elle manque.
    dl->AddRectFilled(a0, a1, WithAlpha(kArtMissing, alpha), ar);
    const ro::IconTex ic = ro::ItemIcon(r.id);
    if (ic.tex != nullptr) {
      const float sz = std::min(lay.art_w, lay.art_h) * 0.5f;
      const ImVec2 c((a0.x + a1.x) * 0.5f, (a0.y + a1.y) * 0.5f);
      const ImU32 tint = r.unlocked ? WithAlpha(ro::SkinImageTint(), alpha)
                                    : WithAlpha(IM_COL32(110, 110, 110, 255), alpha);
      dl->AddImage(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(c.x - sz * 0.5f, c.y - sz * 0.5f),
                   ImVec2(c.x + sz * 0.5f, c.y + sz * 0.5f), ImVec2(0, 0), ImVec2(1, 1), tint);
    }
  }

  // Survol : on ÉCLAIRCIT, on ne colore pas.
  if (hovered && !dragging) dl->AddRectFilled(a0, a1, ro::pal::kHoverTint, ar);

  // La pastille de réserve, ou le cachet.
  if (r.unlocked) {
    char qty[16];
    std::snprintf(qty, sizeof(qty), "\xC3\x97%u", r.amount);  // « × »
    DrawPill(dl, ImVec2(pk1.x - ro::Px(3.0f), pk0.y + ro::Px(3.0f)), ImVec2(1.0f, 0.0f), qty,
             r.amount > 0 ? kBadgeStock : kBadgeEmpty,
             ImGui::ColorConvertFloat4ToU32(ro::pal::kBlack), alpha);
  } else {
    DrawPill(dl, ImVec2((pk0.x + pk1.x) * 0.5f, pk1.y - ro::Px(6.0f)), ImVec2(0.5f, 1.0f),
             i18n::Tr("scellée"), kSealFill, kSealText, alpha);
  }

  // La carte que le dernier ordre a touchée : un liseré vert qui bat, le temps
  // du compte rendu. C'est ce qui rend un sacrifice visible.
  const bool lit = (r.id == highlight_id_ && highlight_tick_ != 0);
  if (lit) {
    const float pulse = 0.55f + 0.45f * std::sin(GetTickCount() * 0.008f);
    dl->AddRect(ImVec2(pk0.x - 1.0f, pk0.y - 1.0f), ImVec2(pk1.x + 1.0f, pk1.y + 1.0f),
                WithAlpha(kHighlight, pulse * alpha), pr, 0, ro::Px(2.5f));
  }

  // Le nom, sous la pochette — précédé de l'icône d'inventaire de la carte si
  // le joueur l'a demandée. Elle garde ses couleurs même sous une pochette
  // scellée : c'est la SILHOUETTE de la pochette qui dit ce qu'on n'a pas, et
  // l'icône ne sert qu'à reconnaître la carte d'un coup d'œil.
  const ImU32 name_col = r.unlocked ? ImGui::GetColorU32(ImGuiCol_Text)
                                    : ImGui::ColorConvertFloat4ToU32(ro::pal::kLabel);
  void* name_icon = nullptr;
  if (name_icon_) name_icon = ro::ItemIcon(r.id).tex;
  DrawFittedText(dl, pk0.x, pk1.x, pk1.y + ro::Px(2.0f), WithAlpha(name_col, alpha), shown,
                 name_icon, WithAlpha(ro::SkinImageTint(), alpha));

  // ── Une carte de l'inventaire passe au-dessus de SA pochette ─────────────
  // Le dépôt lui-même est routé par l'inventaire au relâché (viewer_probes) ;
  // ici on ne fait que le dire, en vert, pendant qu'il est encore temps.
  // ⚠ Par BeginDragDropTarget et non IsItemHovered : pendant un glisser, la
  // source est l'item ACTIF d'ImGui et tout autre item se déclare non survolé.
  // AcceptPeekOnly regarde sans accepter — le routage reste à l'inventaire.
  if (dragging && ImGui::BeginDragDropTarget()) {
    const ImGuiPayload* pl =
        ImGui::AcceptDragDropPayload("INV_ITEM", ImGuiDragDropFlags_AcceptPeekOnly);
    if (pl != nullptr) {
      if (auto* inv = Bourgeon::Instance().inventory_viewer()) {
        if (inv->DraggedItemNameId() == r.id) {
          dl->AddRect(ImVec2(pk0.x - 1.0f, pk0.y - 1.0f), ImVec2(pk1.x + 1.0f, pk1.y + 1.0f),
                      kDropOk, pr, 0, ro::Px(2.5f));
        }
      }
    }
    ImGui::EndDragDropTarget();
  }

  // ── Source de glisser : la pochette part vers l'inventaire = retrait ─────
  // Rien à emporter d'une pochette scellée ou vide. Le relâché est routé dans
  // RouteDragRelease, d'après la fenêtre sous la souris.
  if (r.unlocked && r.amount > 0 &&
      ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
    drag_active_ = true;
    drag_id_ = r.id;
    drag_amount_ = r.amount;
    ImGui::SetDragDropPayload(kDragPayload, &r.id, sizeof(r.id));
    if (t.tex != nullptr && t.w > 0 && t.h > 0) {
      const float gh = ro::Px(64.0f);
      ImGui::Image(reinterpret_cast<ImTextureID>(t.tex), ImVec2(gh * t.w / t.h, gh));
      ImGui::SameLine();
    }
    ImGui::TextUnformatted(shown);
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Lâcher sur l'inventaire pour reprendre"));
    ImGui::EndDragDropSource();
  }

  // ── Les MACARONS de provenance ───────────────────────────────────────────
  // « O » et « M » : la carte se trouve dans l'Old Card Album, dans le Mystical
  // Card Album, ou dans les deux. Cliquer ouvre la description de l'album —
  // laquelle porte l'onglet « Probabilités », c'est-à-dire la liste complète
  // dont ce macaron n'est qu'un extrait.
  //
  // 🔴 Soumis APRÈS le glisser, et ce n'est pas un détail de rangement :
  // `BeginDragDropSource()` s'attache au DERNIER item soumis. Un macaron posé
  // plus haut aurait volé à la pochette sa source de glisser, et c'est lui
  // qu'on aurait promené vers l'inventaire.
  bool on_chip = false;
  bool chip_rclick = false;
  if (sources_) {
    const float chip = ro::Px(kSrcChip);
    float cx = pk0.x + ro::Px(3.0f);
    const float cy = pk0.y + ro::Px(3.0f);
    for (int s = 0; s < kSrcCount; ++s) {
      const AlbumSource& src = g_sources[s];
      const SrcChance* ch = SourceChance(src, r.id);
      if (ch == nullptr) continue;

      ImGui::SetCursorScreenPos(ImVec2(cx, cy));
      ImGui::PushID(s);
      const bool hit = ImGui::InvisibleButton("src", ImVec2(chip, chip));
      const bool ho = ImGui::IsItemHovered();
      ImGui::PopID();
      on_chip = on_chip || ho;
      // Le macaron ne mange que le clic GAUCHE. Le clic DROIT sur ces quinze
      // pixels reste celui de la POCHETTE — c'est la carte qu'on visait, et un
      // menu contextuel qui s'ouvre partout sauf sur un coin est un défaut.
      if (ho && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) chip_rclick = true;

      const ImVec2 c(cx + chip * 0.5f, cy + chip * 0.5f);
      const float rad = chip * 0.5f;
      dl->AddCircleFilled(ImVec2(c.x, c.y + ro::Px(1.0f)), rad,
                          WithAlpha(kPocketShadow, alpha));
      dl->AddCircleFilled(c, rad, WithAlpha(src.fill, alpha));
      if (ho) dl->AddCircleFilled(c, rad, ro::pal::kHoverTint);
      dl->AddCircle(c, rad, WithAlpha(kSrcEdge, alpha));
      ImFont* font = ImGui::GetFont();
      const float fsz = ImGui::GetFontSize() * 0.85f;
      const ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, src.letter);
      dl->AddText(font, fsz, ImVec2(std::floor(c.x - ts.x * 0.5f), std::floor(c.y - ts.y * 0.5f)),
                  WithAlpha(kSrcText, alpha), src.letter);

      if (ho && !dragging) {
        ImGui::BeginTooltip();
        ImGui::Text("%s : %.4f%%", SourceName(src), ch->pct);
        if (ch->total > 0) {
          ImGui::TextColored(ro::pal::kLabel, i18n::Tr("%d chances sur %d"), ch->weight,
                             ch->total);
        }
        ImGui::TextColored(ro::pal::kLabel, "%s",
                           i18n::Tr("Clic : ouvrir sa description et sa table de tirage"));
        ImGui::EndTooltip();
      }
      if (hit) {
        // ⚠ DIFFÉRÉE comme partout ailleurs : OpenDesc* rejoue un OnMsg NATIF,
        // proscrit entre NewFrame() et Render().
        POINT pt;
        if (GetCursorPos(&pt)) itemcell::DeferDescById(src.item_id, 0, 0, pt.x, pt.y);
      }
      cx += chip + ro::Px(kSrcChipGap);
    }
  }

  // ── Survol : la description, avec la carte EN GRAND ──────────────────────
  // L'aperçu simple réduit l'illustration à une vignette ; ici on regarde des
  // cartes, la carte entière est ce qu'on veut voir.
  if (hovered && !dragging && !on_chip) itemdesc::RenderCardTooltipFull(r.id);

  // ── Gestes : ceux de l'entrepôt, à la lettre ─────────────────────────────
  // Clic GAUCHE : Maj -> tout retirer ; une seule copie -> retrait direct ;
  // sinon le menu. Une pochette scellée ou vide ouvre son menu.
  if (pressed) {
    if (r.unlocked && r.amount > 0 && (ImGui::GetIO().KeyShift || r.amount <= 1)) {
      ArmMove(kCmdGet, r.id, r.id, r.amount, false);
    } else {
      ImGui::OpenPopup("ctx");
    }
  }
  // Clic DROIT : Ctrl -> description directe ; Alt/Maj -> tout retirer ;
  // sinon le menu. Sur enfoncement et non relâchement — sortir de la zone doit
  // pouvoir annuler un menu, pas le déclencher ailleurs.
  if (rclicked || chip_rclick) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl) {
      POINT pt;
      if (GetCursorPos(&pt)) itemcell::DeferDescById(r.id, 0, 0, pt.x, pt.y);
    } else if ((io.KeyAlt || io.KeyShift) && r.unlocked && r.amount > 0) {
      ArmMove(kCmdGet, r.id, r.id, r.amount, false);
    } else {
      ImGui::OpenPopup("ctx");
    }
  }

  // BeginPopup et non BeginPopupContextItem : l'ouverture est décidée
  // au-dessus, sur la cellule, pas sur le dernier widget soumis.
  if (ImGui::BeginPopup("ctx")) {
    PocketMenu(r, shown);
    ImGui::EndPopup();
  }

  ImGui::PopID();
}

// Menu d'une pochette — mêmes entrées et même formulation que celui de
// l'entrepôt : (1), (tout : N), et « … » pour une quantité libre. S'y ajoute ce
// que le plateau permet pour CETTE carte : la déposer, ou la sacrifier.
void CardAlbumWindow::PocketMenu(const Row& r, const char* nm) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextUnformatted(nm);
  ImGui::PopStyleColor();
  ImGui::Separator();

  if (ImGui::MenuItem(i18n::Tr("Description"))) {
    // ⚠ DIFFÉRÉE : OpenDesc* rejoue un OnMsg NATIF, proscrit entre
    // NewFrame() et Render(). On arme, Bourgeon::OnProcessInput consomme.
    POINT pt;
    if (GetCursorPos(&pt)) itemcell::DeferDescById(r.id, 0, 0, pt.x, pt.y);
  }
  if (ImGui::MenuItem(i18n::Tr("Voir dans la base de données"))) {
    itemdesc::OpenItemDbPage(r.id);
  }

  if (r.unlocked && r.amount > 0) {
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::Tr("Vers l'inventaire (1)"))) {
      ArmMove(kCmdGet, r.id, r.id, 1, false);
    }
    if (r.amount > 1) {
      char lbl[64];
      std::snprintf(lbl, sizeof(lbl), i18n::Tr("Vers l'inventaire (tout : %d)"),
                    static_cast<int>(r.amount));
      if (ImGui::MenuItem(lbl)) ArmMove(kCmdGet, r.id, r.id, r.amount, false);
      if (ImGui::MenuItem(i18n::Tr("Vers l'inventaire..."))) {
        ArmMove(kCmdGet, r.id, r.id, r.amount, true);
      }
    }
  }

  // La carte est aussi en main : proposer le geste inverse depuis la pochette.
  if (const InvCard* c = FindInHand(r.id)) {
    ImGui::Separator();
    if (r.unlocked) {
      char lbl[64];
      std::snprintf(lbl, sizeof(lbl), i18n::Tr("Depuis l'inventaire (1 sur %d)"), c->amount);
      if (ImGui::MenuItem(lbl)) ArmMove(kCmdPut, c->index, c->id, 1, false);
      if (c->amount > 1) {
        std::snprintf(lbl, sizeof(lbl), i18n::Tr("Depuis l'inventaire (tout : %d)"), c->amount);
        if (ImGui::MenuItem(lbl)) ArmMove(kCmdPut, c->index, c->id, c->amount, false);
        if (ImGui::MenuItem(i18n::Tr("Depuis l'inventaire..."))) {
          ArmMove(kCmdPut, c->index, c->id, c->amount, true);
        }
      }
    } else if (ImGui::MenuItem(i18n::Tr("Sacrifier une copie..."))) {
      RequestSacrifice(c->index, c->id, c->amount - 1);
    }
  }
}

// ── La confirmation du sacrifice ────────────────────────────────────────────

void CardAlbumWindow::DrawConfirmModal() {
  // ⚠ EXACTEMENT la même chaîne que BeginPopupModal ci-dessous : ImGui dérive
  // l'id du libellé, et `##x` (id = « ##x ») ne donne pas le même que `###x`
  // (id = « x »). Les dépareiller ouvrirait un popup que personne ne dessine.
  // Le `###` garantit en prime que l'id survit à une traduction du titre.
  const char* const kConfirmTitle =
      i18n::Tr("Sacrifier cette carte ?###album_confirm");

  if (open_confirm_) {
    ImGui::OpenPopup(kConfirmTitle);
    open_confirm_ = false;
  }

  // Centré sur la fenêtre parente, une fois, à l'apparition : c'est ce qui
  // remplace le popup contextuel qui se replaçait au curseur d'une ligne. La
  // modale RO se place par son coin haut-gauche : on retire la moitié de sa
  // largeur attendue.
  const ImVec2 center = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  const float modal_w = ro::Px(420.0f);
  ro::SetNextRoModalPos(center.x + (size.x - modal_w) * 0.5f,
                        center.y + size.y * 0.5f - ro::Px(80.0f), true);
  ImGui::SetNextWindowSize(ImVec2(modal_w, 0.0f), ImGuiCond_Appearing);

  if (!ro::BeginRoPopupModal(kConfirmTitle)) {
    // Fermée autrement que par nos boutons (Échap) : c'est un abandon, reste de
    // pile compris.
    if (confirm_index_ >= 0) {
      chain_id_ = 0;
      chain_amount_ = 0;
    }
    confirm_index_ = -1;
    return;
  }
  // Échap doit fermer LA CONFIRMATION, pas le classeur derrière elle.
  ro::SuppressEscapeStack();

  // La carte qu'on s'apprête à donner, en silhouette : c'est la pochette qui
  // s'ouvrira.
  float aw = 0.0f, ah = 0.0f;
  ArtSizePx(&aw, &ah);  // la même clé que les pochettes : déjà en cache
  const ro::cardthumb::Thumb t =
      ro::cardthumb::Get(confirm_id_, itemdesc::CardIllustPath(confirm_id_), true,
                         static_cast<int>(aw), static_cast<int>(ah));
  const float th = ro::Px(96.0f);
  if (t.tex != nullptr && t.w > 0 && t.h > 0) {
    ImGui::Image(reinterpret_cast<ImTextureID>(t.tex), ImVec2(th * t.w / t.h, th));
    ImGui::SameLine();
  }
  ImGui::BeginGroup();
  const char* nm = itemdesc::CardName(confirm_id_);
  if (nm[0] != '\0') ImGui::TextUnformatted(nm);
  if (chain_amount_ > 0) {
    ImGui::TextColored(ro::pal::kLabel, i18n::Tr("Les %d autres copies seront rangées ensuite."),
                       chain_amount_);
  }
  ImGui::Separator();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ro::Px(300.0f));
  ImGui::TextWrapped("%s", i18n::Tr(
      "Elle sera CONSOMMÉE et débloquera définitivement sa pochette dans "
      "l'album. Les copies suivantes pourront alors y être rangées et reprises "
      "librement. Ce sacrifice ne peut pas être annulé."));
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
  ImGui::Spacing();

  // Entrée vaut « Sacrifier », comme OK dans le dialogue de quantité : la
  // question a déjà été posée, le geste irréversible est le clic qui l'a
  // ouverte. Échap est pris par la modale elle-même (abandon, ci-dessus).
  const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                     ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
  if (ro::RoButton(i18n::Tr("Sacrifier")) || enter) {
    if (confirm_index_ >= 0) {
      Send(kCmdUnlock, static_cast<uint32_t>(confirm_index_), 1, confirm_id_);
    }
    confirm_index_ = -1;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Annuler"))) {
    confirm_index_ = -1;
    chain_id_ = 0;
    chain_amount_ = 0;
    ImGui::CloseCurrentPopup();
  }

  ro::EndRoPopupModal();
}

bool CardAlbumWindow::DrawSettings() {
  bool changed = false;

  // 🔴 PLUS DE CASE « Album de cartes (interface moderne) » ICI. Elle disait deux
  // choses à la fois — le nom de la section qu'on lit déjà dans la nav, et
  // l'appartenance au groupe — sans dire la seule qui compte : que l'éteindre
  // coupe aussi le SERVEUR (le bit UiCaps tombe, les commandes d'album sont
  // refusées). L'album est désormais un membre ordinaire du groupe « Interface
  // moderne » (`kModernGroup`, moonlight_ui.cc) : sa section se grise et propose
  // le bouton commun quand le groupe est éteint, comme les treize autres.
  //
  // Rien ne l'ouvre depuis le jeu : ni fenêtre native, ni icône de menu. Le
  // bouton et le nom de l'action à lier sont donc les DEUX seules portes, et
  // elles doivent être ici.
  if (hotkeys::OpenButton(i18n::Tr("Ouvrir l'album"), "win_card_album")) Open();

  ImGui::Spacing();

  if (ro::RoCheckbox(i18n::Tr("Icône de la carte devant son nom"), &name_icon_)) {
    changed = true;
  }
  ImGui::SameLine();
  mui::HelpMarker(i18n::Tr(
      "Ajoute la petite icône d'inventaire de la carte devant son nom, sous "
      "chaque pochette. Le nom se réduit d'autant : sur des pochettes étroites, "
      "gagner l'icône coûte quelques lettres."));

  if (ro::RoCheckbox(i18n::Tr("Macarons « où la trouver »"), &sources_)) {
    changed = true;
  }
  ImGui::SameLine();
  mui::HelpMarker(i18n::Tr(
      "Pose « O » et « M » au coin d'une pochette quand la carte figure dans "
      "l'Old Card Album ou le Mystical Card Album. Le survol donne sa chance "
      "exacte, le clic ouvre la description de l'album — et sa table de tirage "
      "complète. La donnée est celle du client, la même que l'onglet "
      "« Probabilités » d'une description."));

  if (ro::RoCheckbox(i18n::Tr("Sacrifier sans confirmation"), &auto_sacrifice_)) {
    changed = true;
  }
  ImGui::SameLine();
  mui::HelpMarker(i18n::Tr(
      "La première copie d'une carte est sacrifiée dès qu'elle est offerte à "
      "l'album (glisser, menu « Vers l'album »), sans la question « Sacrifier "
      "cette carte ? ». Le sacrifice reste irréversible : la pochette débloquée "
      "et le message le disent après coup."));
  return changed;
}
