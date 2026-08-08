#include "features/windows/craft_atlas.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "bourgeon.h"
#include "features/craft_data.h"
#include "features/item_cell.h"
#include "features/link_gesture.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/windows/chat_window.h"  // poser un lien de recette
#include "imgui.h"
#include "ui/icon_cache.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace {

// ── Modèle SESSION de l'inventaire ───────────────────────────────────────────
// La std::list que le client tient à jour quel que soit l'état de ses fenêtres —
// même source qu'InventoryViewer et MakeItemWindow. 🔴 `kInvListHead` est
// l'adresse du GLOBAL ; la SENTINELLE est ce qu'il contient, et c'est elle qui
// termine le parcours (la liste est CIRCULAIRE : comparer au global ne rencontre
// jamais la condition d'arrêt et resomme l'inventaire jusqu'au garde-fou).
constexpr uintptr_t kInvListHead  = 0x015fbab0;
constexpr int       kNodeNext     = 0x00;
constexpr int       kNodeInfo     = 0x08;
constexpr int       kNodeAmt      = 0x18;
constexpr int       kInfoIdStr    = 0x2c;  // std::string : l'id EN TEXTE (le jeu fait atoi)
constexpr int       kInfoIdCap    = 0x40;  // capacité SSO (+0x2c + 0x14) ; > 15 => heap
constexpr int       kMaxInvNodes  = 4096;  // garde-fou de parcours

// Résolveur de nom de compétence LOCALISÉ (wrapper Lua natif, cf. skill_bar et
// character_sheet) : char* GetSkillName(int id), « Unknown-Skill » si inconnu.
// C'est la seule source qui couvre TOUTES les compétences, custom comprises.
constexpr uintptr_t kGetSkillNameLua = 0x0073a1f0;
using GetSkillNameLua_t = char* (__cdecl*)(int);

constexpr int kRoCursorHand = 2;  // *(CursorMgr+0x50) : la main du client

// Palette du projet, celle de la fabrication et du refine — sur le corps CLAIR
// du skin RO (feedback_imgui_ro_light_body_colors) : `TextDisabled` y est
// illisible, et un vert/rouge vif y bave.
constexpr ImU32 kColOk   = IM_COL32( 13, 107,  31, 255);
constexpr ImU32 kColBad  = IM_COL32(166,  38,  38, 255);
constexpr ImU32 kColDim  = IM_COL32(110, 110, 110, 255);
constexpr ImU32 kColWarn = IM_COL32(166, 102,   0, 255);
constexpr ImU32 kColText = IM_COL32( 20,  20,  20, 255);

inline ImVec4      V4(ImU32 c)   { return ImGui::ColorConvertU32ToFloat4(c); }
inline ImTextureID TexId(void* t) { return reinterpret_cast<ImTextureID>(t); }

// L'itemId d'un ItemSkillInfo : une std::string sur laquelle le jeu fait atoi
// (§4.4 de docs/make_item_list_re.md). Petite chaîne = tampon INTERNE, grande =
// pointeur ; la capacité tranche.
uint32_t InfoItemId(const uint8_t* info) {
  const uint32_t cap = *reinterpret_cast<const uint32_t*>(info + kInfoIdCap);
  const char* ids = (cap > 0xf)
                        ? *reinterpret_cast<char* const*>(info + kInfoIdStr)
                        : reinterpret_cast<const char*>(info + kInfoIdStr);
  return ids ? static_cast<uint32_t>(std::atoi(ids)) : 0u;
}

// (Pas de `IsLastItemRightClicked` local : `mui::` en fournit un, et le
//  redéfinir ici rendait l'appel ambigu sous `using namespace mui`. La version
//  partagée est `ImGui::IsItemClicked(Right)`, ce qu'il faut pour un vrai widget
//  — nos lignes en sont, elles se terminent par un EndGroup.)

// Recherche insensible à la casse, sur de l'ASCII. Les noms d'objets du client
// arrivent en UTF-8 : un octet de continuation (>= 0x80) n'est jamais confondu
// avec une lettre ASCII, donc la comparaison octet à octet reste correcte pour
// ce à quoi elle sert — trouver « jellopy » en tapant « JELL ».
bool IContains(const char* hay, const char* needle) {
  if (!needle || !needle[0]) return true;
  if (!hay) return false;
  for (const char* h = hay; *h; ++h) {
    const char* a = h;
    const char* b = needle;
    while (*a && *b &&
           std::tolower(static_cast<unsigned char>(*a)) ==
               std::tolower(static_cast<unsigned char>(*b))) { ++a; ++b; }
    if (!*b) return true;
  }
  return false;
}

const char* ItemName(uint32_t id) {
  const char* n = itemcell::NameById(id);  // jamais nul : « #<id> » au pire
  return (n && n[0]) ? n : "?";
}

}  // namespace

// ── Index ────────────────────────────────────────────────────────────────────

void CraftAtlas::EnsureIndex() {
  if (index_ready_) return;
  index_ready_ = true;  // posé D'ABORD : une DB d'items muette ne doit pas faire
                        // retenter la construction à chaque frame.

  for (const craftdata::Recipe& r : craftdata::AllRecipes())
    products_.push_back(r.product);

  // Les matériaux viennent de l'index inverse plutôt que d'un second passage sur
  // les recettes : c'est exactement l'ensemble des clés qui ont au moins une
  // recette, sans dédoublonnage à écrire.
  for (const craftdata::Recipe& r : craftdata::AllRecipes())
    for (const craftdata::Ingredient& ing : r.mats)
      if (std::find(materials_.begin(), materials_.end(), ing.id) == materials_.end())
        materials_.push_back(ing.id);

  for (const craftdata::ArrowRecipe& a : craftdata::AllArrows())
    arrow_sources_.push_back(a.source);

  // Tri par NOM, pas par id : le joueur cherche « Emperium », pas 714. Les ids
  // consécutifs regroupent des familles, mais aucune liste ne se lit de tête.
  auto by_name = [](uint32_t a, uint32_t b) {
    const int cmp = _stricmp(ItemName(a), ItemName(b));
    return (cmp != 0) ? (cmp < 0) : (a < b);  // départage stable sur l'id
  };
  std::sort(products_.begin(), products_.end(), by_name);
  std::sort(materials_.begin(), materials_.end(), by_name);
  std::sort(arrow_sources_.begin(), arrow_sources_.end(), by_name);
}

void CraftAtlas::RebuildOwned() {
  owned_.clear();
  // Un SEUL parcours par frame. Interroger la liste chaînée par ligne affichée la
  // reparcourrait des dizaines de fois pour la même image, et l'Atlas montre le
  // stock sur presque chaque ligne.
  __try {
    uint8_t* head = *reinterpret_cast<uint8_t**>(kInvListHead);
    if (!head) return;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
    int guard = 0;
    while (node && node != head && guard++ < kMaxInvNodes) {
      const uint8_t* info = node + kNodeInfo;
      const int amount = *reinterpret_cast<const int*>(node + kNodeAmt);
      const uint32_t id = InfoItemId(info);
      node = *reinterpret_cast<uint8_t**>(node + kNodeNext);
      if (amount > 0 && id != 0) owned_[id] += amount;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Inventaire illisible (chargement de carte, session en cours de bascule) :
    // ce qu'on a déjà compté reste valable, le reste s'affichera à 0. Rien à
    // signaler — la frame suivante refera le parcours.
  }
}

int CraftAtlas::Owned(uint32_t item_id) const {
  const auto it = owned_.find(item_id);
  return (it == owned_.end()) ? 0 : it->second;
}

int CraftAtlas::CraftableCount(uint32_t product) const {
  const craftdata::Recipe* r = craftdata::RecipeOf(product);
  if (!r) return -1;
  int best = -1;
  for (const craftdata::Ingredient& ing : r->mats) {
    if (ing.not_consumed) {
      // Un guide n'est pas consommé : il ne borne pas le nombre de fabrications,
      // mais son absence les interdit TOUTES. Deux règles opposées pour la même
      // ligne, d'où le traitement séparé.
      if (Owned(ing.id) < 1) return 0;
      continue;
    }
    if (ing.qty <= 0) continue;  // ne devrait pas arriver : qty 0 == non consommé
    const int possible = Owned(ing.id) / ing.qty;
    best = (best < 0) ? possible : (std::min)(best, possible);
  }
  return best;  // -1 = recette faite de guides seulement : on ne conclut pas
}

const char* CraftAtlas::SkillLabel(int skill, int recipe_lv) {
  if (skill == 0) {
    // 🔴 `itemlv` entre 11 et 20 = un PLAT. C'est le test du serveur lui-même
    // (`menuskill_val > 10 && <= 20` avant la formule de cuisine), pas une
    // convention inventée ici. Ces recettes n'exigent aucune compétence — elles
    // s'ouvrent avec un kit — mais les afficher comme « sans compétence » à côté
    // d'un « Mix Cooking » correctement nommé se lit comme une donnée manquante.
    if (recipe_lv >= 11 && recipe_lv <= 20) return i18n::Tr("Cuisine (kit)");
    return i18n::Tr("Sans compétence (objet ou script)");
  }
  const char* n = reinterpret_cast<GetSkillNameLua_t>(kGetSkillNameLua)(skill);
  return (n && n[0]) ? n : "?";
}

bool CraftAtlas::RecipeIsPlayable(uint32_t product) const {
  if (show_unavailable_) return true;
  const craftdata::Recipe* r = craftdata::RecipeOf(product);
  if (!r) return true;  // pas de recette : rien à juger, on ne masque pas
  return craftdata::SkillIsLearnable(r->skill);
}

int CraftAtlas::PlayableUses(uint32_t material) const {
  const std::vector<craftdata::Recipe>& all = craftdata::AllRecipes();
  int count = 0;
  for (int index : craftdata::RecipesUsing(material))
    if (RecipeIsPlayable(all[index].product)) ++count;
  return count;
}

bool CraftAtlas::Matches(uint32_t item_id) const {
  if (!filter_[0]) return true;
  if (IContains(ItemName(item_id), filter_)) return true;
  char id_text[16];
  std::snprintf(id_text, sizeof(id_text), "%u", item_id);
  return IContains(id_text, filter_);
}

// ── Navigation ───────────────────────────────────────────────────────────────

void CraftAtlas::GoTo(uint32_t item_id) {
  if (item_id == 0 || item_id == sel_id_) return;
  if (sel_id_ != 0) back_.push_back(sel_id_);
  // Borne de la pile : un aller-retour prolongé entre deux matériaux ne doit pas
  // faire enfler indéfiniment un historique dont personne ne remonte le début.
  if (back_.size() > 64) back_.erase(back_.begin());
  sel_id_ = item_id;
}

void CraftAtlas::GoBack() {
  if (back_.empty()) return;
  sel_id_ = back_.back();
  back_.pop_back();
}

void CraftAtlas::OpenOnItem(uint32_t item_id) {
  open_ = true;
  back_.clear();  // on arrive par une porte, pas au milieu d'un parcours
  sel_id_ = item_id;
}

// ── Rendu ────────────────────────────────────────────────────────────────────

void CraftAtlas::OnTick() {
  // La position n'est écrite qu'à la FERMETURE, pas à chaque frame de glissement :
  // MoonlightUi possède le fichier de réglages, on ne fait que demander l'écriture.
  if (!open_ && pos_dirty_) {
    pos_dirty_ = false;
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
  }
}

void CraftAtlas::OnRenderUI() {
  if (!open_) return;

  EnsureIndex();
  RebuildOwned();
  hover_valid_ = false;  // relevé pendant le rendu, consommé juste après

  if (pos_x_ != INT_MIN && pos_y_ != INT_MIN) {
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(pos_x_),
                                   static_cast<float>(pos_y_)),
                            ImGuiCond_FirstUseEver);
  }
  const float em = ImGui::GetFontSize();
  ImGui::SetNextWindowSize(ImVec2(em * 54.0f, em * 28.0f), ImGuiCond_FirstUseEver);
  // Bornes en `em` et non en pixels : la fenêtre suit la taille de police choisie
  // dans les réglages, sinon elle rétrécit visuellement quand la police grossit.
  // ⚠ La largeur MINIMALE doit laisser tenir les quatre onglets ET la colonne de
  // gauche : en dessous, la barre d'onglets se met à défiler et les deux derniers
  // deviennent invisibles — on ne découvre pas un onglet qu'on ne voit pas.
  ImGui::SetNextWindowSizeConstraints(ImVec2(em * 38.0f, em * 15.0f),
                                      ImVec2(FLT_MAX, FLT_MAX));

  char title[128];
  std::snprintf(title, sizeof(title), "%s###craftatlas",
                i18n::Tr("Atlas des recettes"));

  // Bullet de la barre de titre : raccourci vers la section « Atlas » du panneau
  // Moonlight, comme la fabrication, le refine, l'inventaire et le storage.
  ro::SetNextWindowTitleBullet(i18n::Tr("Réglages de l'Atlas"));
  bool open = true;
  const bool begun = ro::BeginRoWindow(title, &open);
  // ⚠ À lire JUSTE APRÈS BeginRoWindow et hors du `if (begun)` : le drapeau est
  // posé par Begin lui-même et vaut aussi pour une fenêtre repliée.
  if (ro::TitleBulletClicked())
    if (auto* mu = Bourgeon::Instance().moonlight_ui())
      mu->OpenInterfaceSection(MoonlightUi::kIfaceCraftAtlas);

  if (begun) {
    const ImVec2 p = ImGui::GetWindowPos();
    const int nx = static_cast<int>(p.x), ny = static_cast<int>(p.y);
    if (nx != pos_x_ || ny != pos_y_) { pos_x_ = nx; pos_y_ = ny; pos_dirty_ = true; }

    if (!craftdata::Available()) {
      // Dire l'ABSENCE, et laquelle : le fichier est livré avec le patch, un
      // joueur peut ne pas l'avoir encore. Une fenêtre vide se lirait comme
      // « rien ne se fabrique sur ce serveur », ce qui est faux.
      ImGui::PushStyleColor(ImGuiCol_Text, kColWarn);
      TextWrapped(i18n::Tr(
          "Le fichier de recettes est introuvable ou illisible "
          "(SystemEN\\bourgeon_recipes.yaml). L'Atlas ne peut rien montrer tant "
          "qu'il manque — le reste du client fonctionne normalement."));
      ImGui::PopStyleColor();
    } else {
      DrawToolbar();
      ImGui::Separator();

      // Deux colonnes : la liste (choisir un objet) et la fiche (le lire). La
      // largeur de la liste suit la police plutôt qu'un pourcentage : sur une
      // fenêtre élargie, c'est la FICHE qui doit gagner la place — elle porte des
      // lignes longues, la liste n'a que des noms.
      //
      // ⚠ `em * 16` ne suffisait PAS, et l'erreur venait de n'avoir compté que les
      // noms : la colonne porte aussi les quatre onglets, la flèche d'un arbre,
      // l'icône, et des libellés composés comme « Sans compétence (objet ou
      // script) (60) ». Tout était rogné, onglets compris.
      const float list_w = (std::min)(em * 24.0f,
                                      ImGui::GetContentRegionAvail().x * 0.55f);
      if (ImGui::BeginChild("##atlas_list", ImVec2(list_w, 0.0f),
                            ImGuiChildFlags_Borders)) {
        if (ImGui::BeginTabBar("##atlas_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
          if (ImGui::BeginTabItem(i18n::Tr("Métiers"))) {
            tab_ = 0; DrawSkillTree(); ImGui::EndTabItem();
          }
          if (ImGui::BeginTabItem(i18n::Tr("Produits"))) {
            tab_ = 1; DrawProductList(); ImGui::EndTabItem();
          }
          if (ImGui::BeginTabItem(i18n::Tr("Matériaux"))) {
            tab_ = 2; DrawMaterialList(); ImGui::EndTabItem();
          }
          if (ImGui::BeginTabItem(i18n::Tr("Flèches"))) {
            tab_ = 3; DrawArrowList(); ImGui::EndTabItem();
          }
          // EndTabBar DOIT rester dans le if (BeginTabBar) : ImGui l'exige.
          ImGui::EndTabBar();
        }
      }
      ImGui::EndChild();

      SameLine();
      if (ImGui::BeginChild("##atlas_sheet", ImVec2(0.0f, 0.0f),
                            ImGuiChildFlags_Borders))
        DrawSheet();
      ImGui::EndChild();
    }
  }
  ro::EndRoWindow();

  // Hors de toute fenêtre ImGui : c'est la condition d'emploi de DrawTooltip.
  if (hover_valid_ && desc_tooltip_)
    itemcell::DrawTooltip(hover_id_, nullptr, 0, nullptr, 0, 0, nullptr);

  if (!open) open_ = false;
}

void CraftAtlas::DrawToolbar() {
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
  ImGui::InputTextWithHint("##atlasfilter", i18n::Tr("Filtrer par nom ou par id…"),
                           filter_, sizeof(filter_));
  SameLine();
  if (ro::RoButton(i18n::Tr("Effacer"))) filter_[0] = '\0';

  SameLine();
  ro::RoCheckbox(i18n::Tr("Réalisable maintenant"), &only_craftable_);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(i18n::Tr(
        "Ne garder que les recettes dont TOUS les matériaux sont dans le sac. "
        "Le filtre ne regarde pas les compétences apprises : le serveur, lui, "
        "refusera une recette dont le métier manque."));

  SameLine();
  ImGui::BeginDisabled(back_.empty());
  if (ro::RoButton(i18n::Tr("Retour"))) GoBack();
  ImGui::EndDisabled();
}

bool CraftAtlas::DrawItemRow(uint32_t item_id, const char* suffix, bool selected,
                             unsigned int text_color) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float icon = ImGui::GetTextLineHeight();
  bool followed = false;

  ImGui::PushID(static_cast<int>(item_id));
  ImGui::BeginGroup();
  const ro::IconTex ic = ro::ItemIcon(item_id, 1);
  if (ic.tex) ImGui::Image(TexId(ic.tex), ImVec2(icon, icon));
  else        ImGui::Dummy(ImVec2(icon, icon));
  SameLine(0.0f, style.ItemInnerSpacing.x);

  char label[256];
  if (suffix && suffix[0])
    std::snprintf(label, sizeof(label), "%s %s", ItemName(item_id), suffix);
  else
    std::snprintf(label, sizeof(label), "%s", ItemName(item_id));
  ImGui::TextColored(V4(text_color), "%s", label);
  ImGui::EndGroup();

  // Après EndGroup, « le dernier item » EST le groupe : survol et clic portent
  // sur l'icône ET le texte. Ni Image ni Text ne consomment d'entrée, donc rien
  // à l'intérieur ne brouille le test.
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) {
    ro::SetHoverCursor(kRoCursorHand);
    if (desc_tooltip_) { hover_valid_ = true; hover_id_ = item_id; }
  }
  if (selected) {
    // La sélection est SOULIGNÉE et non surlignée : un fond plein sur le corps
    // clair du skin RO mangerait le texte, et la ligne porte déjà sa couleur de
    // sens (rouge = matériau manquant) qu'un fond rendrait illisible.
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y), ImVec2(mx.x, mx.y),
                                        text_color);
  }

  // 🔴 Gestes de CELLULE, pas de lien : gauche = suivre, droit = description.
  // Cf. l'en-tête du .h — dans un index, le geste le plus courant est de SUIVRE
  // l'entrée, pas d'ouvrir une fenêtre qu'il faudra refermer à chaque saut.
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (ImGui::GetIO().KeyShift) {
      // ⚠ Inconditionnel, et c'est possible depuis que la chatbox bascule seule :
      // le client refuse d'envoyer un `<ITEML>` portant un objet absent du sac
      // (« Item tags can only tag items you own. »), donc `AppendItemLinkFromLink`
      // passe alors par notre balise `<ITMR>`. C'est le cas NORMAL ici — un atlas
      // sert surtout à parler de ce qu'on n'a pas encore.
      links::PostToChat(links::FromItemId(item_id, ItemName(item_id)));
    } else {
      GoTo(item_id);
      followed = true;
    }
  }
  if (IsLastItemRightClicked()) {
    const ImVec2 mouse = ImGui::GetMousePos();
    // ⚠ Par ID : l'objet peut très bien ne pas être en inventaire — c'est même
    // le cas intéressant — donc il n'y a pas toujours d'ItemSkillInfo vivant.
    // Et DIFFÉRÉ : ouvrir une fenêtre native pendant une frame ImGui est proscrit
    // (feedback_no_native_cmd_during_imgui_frame).
    itemcell::DeferDescById(item_id, 0, 0, static_cast<int>(mouse.x),
                            static_cast<int>(mouse.y));
  }
  ImGui::PopID();
  return followed;
}

void CraftAtlas::DrawSkillTree() {
  const bool filtering = filter_[0] != '\0';
  int shown = 0;

  // Indentation resserrée : celle d'ImGui par défaut vaut une largeur de police
  // entière, prise sur CHAQUE ligne de produit alors que l'arbre n'a qu'un seul
  // niveau. Dans une colonne étroite, c'est autant de nom en moins.
  ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, ImGui::GetFontSize() * 0.75f);

  for (int skill : craftdata::SkillsWithRecipes()) {
    // Une compétence qu'aucune classe ne peut apprendre ici n'a que des recettes
    // injouables : le métier entier disparaît, pas seulement ses lignes.
    if (!show_unavailable_ && !craftdata::SkillIsLearnable(skill)) continue;
    const std::vector<uint32_t>& products = craftdata::ProductsOfSkill(skill);

    // Compter d'abord ce qui passe les filtres : un métier dont plus rien ne
    // ressort ne doit pas s'afficher comme un dossier vide à ouvrir pour rien.
    int matching = 0;
    for (uint32_t id : products) {
      if (!Matches(id)) continue;
      if (only_craftable_ && CraftableCount(id) <= 0) continue;
      ++matching;
    }
    if (matching == 0) continue;
    ++shown;

    // Un filtre actif OUVRE les métiers qui ont un résultat : sinon la recherche
    // ne montrerait que des dossiers fermés, et il faudrait tous les déplier pour
    // trouver la ligne qu'on vient de demander.
    if (filtering || only_craftable_)
      ImGui::SetNextItemOpen(true, ImGuiCond_Always);

    // Libellé du GROUPE. Pour `skill == 0`, il dépend de ce qu'il contient : on ne
    // dit « Cuisine » que si TOUTES ses recettes en sont (cf. SkillLabel). Une
    // seule recette étrangère et l'on retombe sur le libellé générique — mieux
    // vaut un titre vague qu'un titre faux pour la ligne qui s'y cacherait.
    int group_lv = 0;
    if (skill == 0) {
      bool all_dishes = true;
      for (uint32_t id : products) {
        const int lv = craftdata::RecipeItemLevel(id);
        if (lv < 11 || lv > 20) { all_dishes = false; break; }
      }
      if (all_dishes && !products.empty()) group_lv = 11;
    }

    char header[160];
    std::snprintf(header, sizeof(header), "%s (%d)###skill%d",
                  SkillLabel(skill, group_lv), matching, skill);
    if (!ImGui::TreeNode(header)) continue;

    for (uint32_t id : products) {
      if (!Matches(id)) continue;
      const int craftable = CraftableCount(id);
      if (only_craftable_ && craftable <= 0) continue;
      char suffix[32] = {0};
      if (craftable > 0) std::snprintf(suffix, sizeof(suffix), "x%d", craftable);
      DrawItemRow(id, suffix, id == sel_id_,
                  (craftable > 0) ? kColOk : kColText);
    }
    ImGui::TreePop();
  }

  ImGui::PopStyleVar();  // IndentSpacing

  if (shown == 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
    TextWrapped(i18n::Tr("Aucun métier ne correspond."));
    ImGui::PopStyleColor();
  }
}

void CraftAtlas::DrawProductList() {
  int shown = 0;
  for (uint32_t id : products_) {
    if (!Matches(id)) continue;
    if (!RecipeIsPlayable(id)) continue;
    const int craftable = CraftableCount(id);
    if (only_craftable_ && craftable <= 0) continue;
    char suffix[32] = {0};
    if (craftable > 0) std::snprintf(suffix, sizeof(suffix), "x%d", craftable);
    DrawItemRow(id, suffix, id == sel_id_, (craftable > 0) ? kColOk : kColText);
    ++shown;
  }
  if (shown == 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
    TextWrapped(i18n::Tr("Aucun produit ne correspond."));
    ImGui::PopStyleColor();
  }
}

void CraftAtlas::DrawMaterialList() {
  int shown = 0;
  for (uint32_t id : materials_) {
    if (!Matches(id)) continue;
    const int have = Owned(id);
    // Le filtre « réalisable » n'a pas de sens sur un matériau (il n'a pas de
    // recette à réaliser) : on le lit ici comme « ceux que j'ai en sac », qui est
    // la question équivalente de ce côté-ci de l'index.
    if (only_craftable_ && have <= 0) continue;
    // Un matériau qui ne sert QUE dans des recettes injouables n'a rien à faire
    // dans la liste : sa fiche serait vide, et il ferait nombre pour rien.
    const int uses = PlayableUses(id);
    if (uses == 0) continue;
    char suffix[48];
    if (have > 0) std::snprintf(suffix, sizeof(suffix), "(%d)  [%d]", have, uses);
    else          std::snprintf(suffix, sizeof(suffix), "[%d]", uses);
    DrawItemRow(id, suffix, id == sel_id_, (have > 0) ? kColOk : kColText);
    ++shown;
  }
  if (shown == 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
    TextWrapped(i18n::Tr("Aucun matériau ne correspond."));
    ImGui::PopStyleColor();
  }
}

void CraftAtlas::DrawArrowList() {
  int shown = 0;
  for (uint32_t id : arrow_sources_) {
    if (!Matches(id)) continue;
    const int have = Owned(id);
    if (only_craftable_ && have <= 0) continue;
    char suffix[32] = {0};
    if (have > 0) std::snprintf(suffix, sizeof(suffix), "(%d)", have);
    DrawItemRow(id, suffix, id == sel_id_, (have > 0) ? kColOk : kColText);
    ++shown;
  }
  if (shown == 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
    TextWrapped(i18n::Tr("Aucune source de flèches ne correspond."));
    ImGui::PopStyleColor();
  }
}

void CraftAtlas::DrawSheet() {
  if (sel_id_ == 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
    TextWrapped(i18n::Tr(
        "Choisissez un objet à gauche.\n\n"
        "Clic gauche : suivre l'objet dans l'Atlas.\n"
        "Clic droit : ouvrir sa description.\n"
        "Maj + clic : poser son lien dans le chat."));
    ImGui::PopStyleColor();
    return;
  }

  const uint32_t id = sel_id_;

  // ── En-tête : l'objet lui-même ────────────────────────────────────────────
  const float icon = ImGui::GetTextLineHeight() * 2.0f;
  const ro::IconTex ic = ro::ItemIcon(id, 1);
  ImGui::BeginGroup();
  if (ic.tex) ImGui::Image(TexId(ic.tex), ImVec2(icon, icon));
  else        ImGui::Dummy(ImVec2(icon, icon));
  ImGui::EndGroup();
  SameLine();
  ImGui::BeginGroup();
  ImGui::TextColored(V4(kColText), "%s", ItemName(id));
  const int have = Owned(id);
  ImGui::TextColored(V4(have > 0 ? kColOk : kColDim),
                     i18n::Tr("id %u  ·  en sac : %d"), id, have);
  ImGui::EndGroup();
  ImGui::Separator();

  bool said_something = false;

  // ── 1. Sa recette ─────────────────────────────────────────────────────────
  if (const craftdata::Recipe* r = craftdata::RecipeOf(id)) {
    said_something = true;
    SeparatorText(i18n::Tr("Comment le fabriquer"));

    if (r->skill == 0) {
      // 🔴 Pas une donnée manquante : ces recettes s'ouvrent par le SCRIPT d'un
      // objet (`produce N;` d'une Mini Furnace, d'un marteau, d'un kit), sans
      // qu'aucune compétence n'y donne accès. Le dire évite de chercher un métier
      // qui n'existe pas.
      ImGui::TextColored(V4(kColWarn), i18n::Tr(
          "Aucune compétence : cette recette s'ouvre en UTILISANT un objet "
          "(fourneau, marteau, kit)."));
    } else {
      ImGui::TextColored(V4(kColText), i18n::Tr("Compétence : %s (id %d), niveau %d"),
                         SkillLabel(r->skill, r->lv), r->skill, r->skill_lv);
    }
    if (r->lv > 0)
      ImGui::TextColored(V4(kColDim), i18n::Tr("Niveau de recette (itemlv) : %d"),
                         r->lv);

    if (!craftdata::SkillIsLearnable(r->skill)) {
      // Affiché SEULEMENT quand le joueur a demandé à voir ces recettes : sinon
      // elles ne sont pas là. Le dire évite qu'il aille chercher un métier
      // qu'aucune classe n'ouvre sur ce serveur.
      ImGui::TextColored(V4(kColWarn), i18n::Tr(
          "Injouable ici : aucune classe de ce serveur n'apprend cette "
          "compétence."));
    }

    // ── Le rendement, quand il n'est pas de UN ──────────────────────────────
    // 🔴 Le taire reviendrait à annoncer « 1 » : c'est ce que tout le monde
    // suppose. Or `skill_produce_mix` recalcule la quantité pour cinq
    // compétences, et une rune en rend deux à six.
    const craftdata::ProduceQty qty = craftdata::QtyForSkill(r->skill);
    if (!qty.IsFixedOne()) {
      if (qty.mode == craftdata::ProduceQty::kTable) {
        ImGui::TextColored(V4(kColWarn), i18n::Tr(
            "Le résultat n'est pas ce produit : le serveur tire dans sa propre "
            "table de transmutation, avec ses taux et ses quantités."));
      } else if (qty.mode == craftdata::ProduceQty::kBonusRoll) {
        // 🔴 Le bonus le plus intéressant à afficher de tous : il est PROPRE à ce
        // serveur, et rien en jeu ne l'annonce. Un joueur ne peut pas le déduire
        // — il faudrait fabriquer des centaines de fois pour soupçonner que la
        // pile monte plus vite que le compte des fabrications.
        // La probabilité est écrite avec sa décimale : arrondie à 16 %, elle
        // serait fausse de plus d'un point.
        ImGui::TextColored(V4(kColOk), i18n::Tr(
            "Rendement : 1, et %d,%d %% de chances d'en obtenir %d à %d de plus "
            "(bonus propre à ce serveur)."),
            qty.bonus_chance_permille / 10, qty.bonus_chance_permille % 10,
            1, qty.max - 1);
      } else if (qty.mode == craftdata::ProduceQty::kPerUnit) {
        // « Jusqu'à » et non « de tant à tant » : chaque exemplaire est tiré
        // séparément, donc le bas de la fourchette n'est pas un plancher.
        ImGui::TextColored(V4(kColOk), i18n::Tr(
            "Rendement : %d à %d exemplaires, chacun tiré séparément (on peut "
            "en obtenir moins)."), qty.min, qty.max);
      } else {
        ImGui::TextColored(V4(kColOk), i18n::Tr(
            "Rendement : %d à %d exemplaires d'un coup, selon le niveau et la "
            "difficulté."), qty.min, qty.max);
      }
    }

    const int craftable = CraftableCount(id);
    if (craftable > 0)
      ImGui::TextColored(V4(kColOk), i18n::Tr("Réalisable %d fois avec le sac actuel."),
                         craftable);
    else if (craftable == 0)
      ImGui::TextColored(V4(kColBad), i18n::Tr("Matériaux insuffisants."));

    // Partager la RECETTE, pas l'objet : le lien posé s'affiche « [Recette: … ] »,
    // montre métier et composants au survol et ouvre l'Atlas au clic. C'est ce
    // qu'on veut donner à quelqu'un à qui l'on explique une fabrication — un
    // simple lien d'objet ne dirait rien de tout cela.
    if (ro::RoSmallButton(i18n::Tr("Partager la recette"))) {
      if (auto* chat = Bourgeon::Instance().chat_window())
        chat->AppendRecipeLink(id, ItemName(id));
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(i18n::Tr(
          "Pose « [Recette: %s] » dans la barre de chat. Au survol, le lien "
          "montrera le métier et les composants ; au clic, il ouvrira l'Atlas."),
          ItemName(id));

    ImGui::Spacing();
    for (const craftdata::Ingredient& ing : r->mats) {
      const int stock = Owned(ing.id);
      // Un matériau non consommé est satisfait dès qu'on en a UN : exiger `qty`
      // (qui vaut 0) l'afficherait en rouge alors que la recette est réalisable.
      const bool enough = ing.not_consumed ? (stock >= 1) : (stock >= ing.qty);
      char suffix[128];
      if (ing.not_consumed)
        std::snprintf(suffix, sizeof(suffix), i18n::Tr("(requis, non consommé — en sac : %d)"),
                      stock);
      else if (!enough)
        // Le MANQUE plutôt que le seul stock : « (3) » oblige à faire la
        // soustraction de tête pour chaque ligne rouge, et c'est justement le
        // chiffre qu'on va chercher — combien aller ramasser.
        std::snprintf(suffix, sizeof(suffix), i18n::Tr("x%d  (%d — il en manque %d)"),
                      ing.qty, stock, ing.qty - stock);
      else
        std::snprintf(suffix, sizeof(suffix), "x%d  (%d)", ing.qty, stock);
      DrawItemRow(ing.id, suffix, false, enough ? kColOk : kColBad);
    }
  }

  // ── 1 bis. La production en MASSE ─────────────────────────────────────────
  // Deux cents potions d'un seul lancement ne se devinent nulle part : produce_db
  // ne connaît que la recette à l'unité, et rien en jeu ne dit qu'une compétence
  // en sort une fournée. C'est l'information qui change une soirée de farm.
  const std::vector<craftdata::BulkSource>& bulk = craftdata::BulkSourcesOf(id);
  if (!bulk.empty()) {
    said_something = true;
    SeparatorText(i18n::Tr("Production en masse"));
    for (const craftdata::BulkSource& source : bulk) {
      // « jusqu'à » : la fournée passe par la même boucle que le reste, où chaque
      // exemplaire est tiré au taux de réussite. Annoncer un chiffre ferme serait
      // promettre ce que le serveur ne garantit pas.
      if (source.min == source.max)
        ImGui::TextColored(V4(kColOk), i18n::Tr("%s : jusqu'à %d par lancement"),
                           SkillLabel(source.skill), source.max);
      else
        ImGui::TextColored(V4(kColOk), i18n::Tr("%s : de %d à %d par lancement"),
                           SkillLabel(source.skill), source.min, source.max);
    }
  }

  // ── 2. Les flèches qu'il donne ────────────────────────────────────────────
  if (const craftdata::ArrowRecipe* a = craftdata::ArrowFrom(id)) {
    said_something = true;
    SeparatorText(i18n::Tr("Transformable en flèches"));
    // ⚠ UNE fabrication consomme UN exemplaire et rend TOUT ce qui suit — ce
    // n'est pas un tirage entre les lignes. Le dire, parce que la présentation en
    // liste suggère naturellement le contraire.
    ImGui::TextColored(V4(kColDim), i18n::Tr(
        "Un exemplaire consommé donne tout ce qui suit, en une fois :"));
    for (const craftdata::Yield& y : a->yields) {
      char suffix[64];
      std::snprintf(suffix, sizeof(suffix), i18n::Tr("x%d obtenues"), y.qty);
      DrawItemRow(y.id, suffix, false, kColText);
    }
  }

  // ── 3. L'index INVERSE : où cet objet sert ────────────────────────────────
  const std::vector<int>& uses = craftdata::RecipesUsing(id);
  const std::vector<craftdata::Recipe>& all_recipes = craftdata::AllRecipes();
  // Le COMPTE vient de la MÊME fonction que celui de la liste des matériaux :
  // deux chiffres différents pour la même question enverraient chercher des
  // recettes qui n'existent pas.
  const int playable_uses = PlayableUses(id);

  if (playable_uses > 0) {
    said_something = true;
    char header[96];
    std::snprintf(header, sizeof(header), i18n::Tr("Sert dans %d recettes"),
                  playable_uses);
    SeparatorText(header);
    const std::vector<craftdata::Recipe>& all = all_recipes;
    for (int index : uses) {
      const craftdata::Recipe& r = all[index];
      if (!RecipeIsPlayable(r.product)) continue;
      int qty = 0;
      bool guide = false;
      for (const craftdata::Ingredient& ing : r.mats)
        if (ing.id == id) { qty = ing.qty; guide = ing.not_consumed; break; }
      char suffix[128];
      if (guide)
        std::snprintf(suffix, sizeof(suffix), i18n::Tr("(à posséder — %s)"),
                      SkillLabel(r.skill, r.lv));
      else
        std::snprintf(suffix, sizeof(suffix), i18n::Tr("x%d — %s"), qty,
                      SkillLabel(r.skill, r.lv));
      DrawItemRow(r.product, suffix, false, kColText);
    }
  }

  // ── 4. D'où viennent ces flèches ──────────────────────────────────────────
  const std::vector<int>& from = craftdata::ArrowsYielding(id);
  if (!from.empty()) {
    said_something = true;
    SeparatorText(i18n::Tr("S'obtient en transformant"));
    const std::vector<craftdata::ArrowRecipe>& all = craftdata::AllArrows();
    for (int index : from) {
      const craftdata::ArrowRecipe& a = all[index];
      int qty = 0;
      for (const craftdata::Yield& y : a.yields)
        if (y.id == id) { qty = y.qty; break; }
      char suffix[64];
      std::snprintf(suffix, sizeof(suffix), i18n::Tr("-> x%d  (en sac : %d)"), qty,
                    Owned(a.source));
      DrawItemRow(a.source, suffix, false, kColText);
    }
  }

  if (!said_something) {
    // Cas réel : on arrive ici par le clic sur un objet qui n'est ni fabricable
    // ni matériau (le produit d'une transformation de flèches, par exemple, quand
    // rien d'autre ne le concerne). Dire QUOI manque vaut mieux qu'une page vide.
    ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
    TextWrapped(i18n::Tr(
        "Cet objet n'a pas de recette, n'entre dans aucune et ne se transforme "
        "pas en flèches. Le fichier de recettes ne couvre que la fabrication : "
        "quêtes, échanges et butin de monstres n'y sont pas."));
    ImGui::PopStyleColor();
  }
}

bool CraftAtlas::DrawSettings() {
  bool changed = false;
  // Le panneau peut être ouvert sans que l'Atlas l'ait jamais été : sans ceci, le
  // décompte du bas annoncerait « 0 matériaux » sur un fichier parfaitement
  // chargé — un chiffre faux, et exactement celui qui ferait croire à un fichier
  // vide.
  EnsureIndex();

  ImGui::TextWrapped(i18n::Tr(
      "L'Atlas répertorie tout ce que le serveur sait fabriquer : par métier, par "
      "produit, par matériau, et les flèches. Le client ne peut pas le faire seul "
      "— sa table de recettes ne s'interroge que par produit connu, et le serveur "
      "n'envoie une liste qu'après un lancement de compétence."));
  ImGui::Separator();

  if (ro::RoCheckbox(i18n::Tr("Ouvrir l'Atlas"), &open_)) changed = true;
  if (ro::RoCheckbox(i18n::Tr("Aperçu de l'objet au survol"), &desc_tooltip_))
    changed = true;
  if (ro::RoCheckbox(i18n::Tr("Ne montrer que ce qui est réalisable"), &only_craftable_))
    changed = true;
  if (ro::RoCheckbox(i18n::Tr("Montrer aussi les recettes injouables sur ce serveur"),
                     &show_unavailable_))
    changed = true;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(i18n::Tr(
        "produce_db contient des recettes dont aucune classe de ce serveur "
        "n'apprend la compétence. Elles sont masquées par défaut : le serveur "
        "les refuserait. Le critère est l'arbre des compétences, pas le mode "
        "renewal — ce serveur est pre-renewal et ouvre pourtant les classes de "
        "3e, dont les recettes sont bien jouables."));

  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
  TextWrapped(i18n::Tr(
      "Dans les listes : clic gauche pour suivre un objet, clic droit pour sa "
      "description, Maj + clic pour poser son lien dans le chat."));
  ImGui::PopStyleColor();

  if (craftdata::Available()) {
    char line[192];
    std::snprintf(line, sizeof(line),
                  i18n::Tr("%d recettes, %d matériaux, %d transformations de flèches."),
                  static_cast<int>(craftdata::AllRecipes().size()),
                  static_cast<int>(materials_.size()),
                  static_cast<int>(craftdata::AllArrows().size()));
    ImGui::TextColored(V4(kColDim), "%s", line);
  } else {
    ImGui::TextColored(V4(kColWarn), i18n::Tr(
        "Fichier de recettes absent : SystemEN\\bourgeon_recipes.yaml"));
  }
  return changed;
}
