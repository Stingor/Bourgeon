#include "features/windows/palette_editor.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_internal.h"  // GetInputTextState (resync du champ de préréglage)

#include <string>

#include "features/fx/palette_base.h"
#include "features/fx/palette_cache.h"
#include "features/fx/palette_inject.h"
#include "features/fx/style_sync.h"
#include "bourgeon.h"  // chat_window() : poser le lien de style
#include "features/hotkey_util.h"
#include "features/windows/chat_window.h"
#include "ragnarok/globals.h"
#include "ui/doll.h"       // pantin d'aperçu (tête + corps)
#include "ui/head_icon.h"    // vignettes des grilles de coiffure
#include "ui/spr_act.h"      // lecture des .pal pour les vignettes de palette
#include "ui/sprite_view.h"  // corps peint d'une palette en mémoire
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "ui/sprite_path.h"   // BodySpriteKey (le style se range par corps)
#include "utils/log_console.h"
#include "ui/sprite_path.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace {

// ── Identité du joueur (client 20250716, base 0x400000) ──────────────────────
// `GetSex(session)` — le sexe décide du dossier de sprite, pas seulement du nom.
// `Job_ResolveMountedClassFromOption(session)` — la classe AFFICHÉE, montures
// comprises.

// Délai d'inactivité avant d'écrire le brouillon sur disque. Assez long pour
// qu'un glissement de curseur ne produise qu'une écriture, assez court pour que
// ce qui est perdu tienne dans une seconde de réglages.
constexpr double kDraftDelay = 1.5;

using GetSexFn = int(__fastcall*)(void*, void*);

int OwnSex() {
  return reinterpret_cast<GetSexFn>(rag::kOwnSexAddr)(
      reinterpret_cast<void*>(rag::kSessionAddr), nullptr);
}
int OwnJob() { return rag::OwnDisplayedJobId(); }

// Apparence courante, telle que le client la tient à jour sur ZC_SPRITE_CHANGE.
// Mêmes globales que `BuildOwnDollLook` (features/overlays/basic_info.cc).

// ── Grille de vignettes, commune aux trois sélecteurs ───────────────────────
//
// `dessine(valeur, coin, cote)` peint UNE case ; le reste — disposition, fond,
// numéro, survol, sélection, clic — est identique partout et n'a pas à être
// recopié trois fois. Rend true si le joueur vient de choisir.
//
// `double_clic`, s'il est fourni, passe à true quand le joueur a DOUBLE-cliqué :
// il a arrêté son choix et veut refermer. Le simple clic, lui, laisse la grille
// ouverte — c'est ce qui permet de comparer trois coiffures en les essayant sur
// le pantin, qui est justement la raison d'être de ces grilles.
//
// ⚠ `ImGui::IsRectVisible` avant de dessiner : une grille de 553 cases n'en
// montre qu'une centaine, et charger les autres coûterait des textures que
// personne ne regarde. Même garde que la grille de création de personnage.
template <typename DessineCase>
bool GrillePicker(int premier, int dernier, int cols, float cote, int* valeur,
                  DessineCase dessine, bool* double_clic = nullptr) {
  bool choisi = false;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (int v = premier; v <= dernier; ++v) {
    if ((v - premier) % cols != 0) ImGui::SameLine();
    ImGui::PushID(v);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p.x + cote, p.y + cote);
    const bool sel = (*valeur == v);
    dl->AddRectFilled(p, p1,
                      sel ? IM_COL32(255, 220, 140, 55) : IM_COL32(0, 0, 0, 40),
                      3.0f);
    if (ImGui::IsRectVisible(ImVec2(cote, cote))) dessine(v, p, cote);
    // Le numéro reste lisible sur un sprite clair grâce à son ombre portée.
    char num[8];
    std::snprintf(num, sizeof(num), "%d", v);
    const ImVec2 ts = ImGui::CalcTextSize(num);
    const ImVec2 tp(p.x + 2.0f, p.y + cote - ts.y - 1.0f);
    ro::AddTextRelief(dl, tp, IM_COL32(255, 255, 255, 235), num,
                      IM_COL32(0, 0, 0, 200), ImVec2(1.0f, 1.0f));
    ImGui::InvisibleButton("c", ImVec2(cote, cote));
    if (ImGui::IsItemClicked()) {
      *valeur = v;
      choisi = true;
    }
    // Le premier clic du double a déjà posé la valeur juste au-dessus ; celui-ci
    // n'ajoute que le congé. On repose quand même `*valeur`, parce que rien ne
    // garantit que les deux clics ont visé la même case.
    if (double_clic != nullptr && ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      *valeur = v;
      choisi = true;
      *double_clic = true;
    }
    if (sel)
      dl->AddRect(p, p1, IM_COL32(255, 210, 110, 255), 3.0f, 0, 2.0f);
    else if (ImGui::IsItemHovered())
      dl->AddRect(p, p1, IM_COL32(200, 210, 235, 160), 3.0f, 0, 1.5f);
    ImGui::PopID();
  }
  return choisi;
}

// Hauteur d'une grille en cases, bornée pour rester dans l'écran.
float HauteurGrille(int total, int cols, float cote, int lignes_max) {
  const float pad = ImGui::GetStyle().ItemSpacing.y;
  int lignes = (total + cols - 1) / cols;
  if (lignes > lignes_max) lignes = lignes_max;
  return lignes * cote + (lignes - 1) * pad +
         2.0f * ImGui::GetStyle().WindowPadding.y;
}

// 🔴 La largeur DOIT compter l'ascenseur quand la grille défile, sinon il mord
// sur la dernière colonne et rogne ses vignettes. Rien ne le signale : le
// contenu est simplement coupé à droite. Même piège que la grille de couleurs de
// la création de personnage, où il avait déjà été payé.
bool GrilleDefile(int total, int cols, int lignes_max) {
  return (total + cols - 1) / cols > lignes_max;
}

float LargeurGrille(int cols, float cote, bool avec_ascenseur) {
  return cols * cote + (cols - 1) * ImGui::GetStyle().ItemSpacing.x +
         2.0f * ImGui::GetStyle().WindowPadding.x +
         (avec_ascenseur ? ImGui::GetStyle().ScrollbarSize : 0.0f);
}

// Recopie en ASCII STRICT, tout octet hors 0x20-0x7E devenant `%NN`.
//
// 🔴 Les chemins de sprite RO traversent des dossiers CORÉENS encodés en CP949
// (`인간족\몸통\남`), et le suffixe de sexe fait partie du nom de fichier. Les
// laisser bruts donnerait du charabia sur Discord et dans la base ; les remplacer
// par '?' perdrait justement ce suffixe, qui désigne la moitié du corps édité. Le
// pourcentage garde l'information ET reste du JSON valide.
//
// Les guillemets et l'antislash sont échappés au passage : un chemin RO en est
// plein, et un JSON cassé n'est plus lisible par personne.
void AppendAscii(std::string* out, const char* src, size_t max) {
  if (!src) return;
  for (size_t i = 0; src[i] != '\0' && i < max; ++i) {
    const unsigned char c = static_cast<unsigned char>(src[i]);
    if (c == '"' || c == '\\') {
      out->push_back('\\');
      out->push_back(static_cast<char>(c));
    } else if (c >= 0x20 && c <= 0x7E) {
      out->push_back(static_cast<char>(c));
    } else {
      char hex[8];
      std::snprintf(hex, sizeof(hex), "%%%02X", c);
      *out += hex;
    }
  }
}

}  // namespace

// Ce qu'il faut à quelqu'un d'autre pour REPRODUIRE le défaut, et rien de plus.
//
// 🔴 Un défaut de style est presque impossible à reproduire sur description ou
// capture d'écran, et c'est ce qui justifie ce bouton plus qu'ailleurs : une
// recette ne porte AUCUNE couleur, elle ne veut rien dire hors du sprite sur
// lequel elle a été composée. Mais toute la reproduction tient en quelques
// centaines d'octets — le code de partage et le sprite porté suffisent à remonter
// la scène exacte.
//
// Le choix des champs vient des défauts RÉELLEMENT rencontrés pendant le
// développement, pas d'une idée de ce qui pourrait servir :
//   * `spr`    — le sprite réellement porté. Décisif : chaque défaut de cette
//                fenêtre y a ramené, et une base fausse fausse tout le reste.
//   * `style`  — le code de partage : la recette entière, rejouable telle quelle.
//   * `ramps`  — les rampes détectées. « Ce curseur ne change pas cette zone »
//                se lit là, et nulle part ailleurs.
//   * `cover`  — la part du corps qu'AUCUN curseur n'atteint. Répond d'un coup
//                à « il manque une couleur ».
//   * `actor`  — la base vient-elle de l'acteur ou du repli par déduction ? Les
//                deux ne désignent pas forcément le même sprite.
//   * `inject` — une recette est-elle réellement posée sur le personnage ? C'est
//                ce qui sépare « mal calculé » de « pas appliqué ».
//
// ⚠ Ni identité, ni carte, ni position : le serveur les ajoute depuis la session,
// et le client n'a pas à les affirmer.
BugReport::Context PaletteEditor::BugContext() const {
  BugReport::Context c;
  c.category = BugReport::kStyle;

  std::string js = "{\"ver\":";
  js += std::to_string(static_cast<int>(fx::style_sync::kWireVersion));
  js += ",\"spr\":\"";
  AppendAscii(&js, body_path_.c_str(), 200);
  js += "\",\"style\":\"";
  AppendAscii(&js, fx::palette_cache::EncodeShare(recipe_).c_str(), 160);
  js += "\",\"cover\":[";
  js += std::to_string(pixels_covered_);
  js += ',';
  js += std::to_string(pixels_total_);
  js += "],\"actor\":";
  js += resolved_from_actor_ ? "true" : "false";
  js += ",\"inject\":";
  js += fx::palette_inject::HasRecipe(rag::OwnAccountId()) ? "true" : "false";
  js += ",\"seed\":";
  js += seeded_ ? "true" : "false";
  // Un brouillon en cours signale que le joueur n'a pas encore validé : le
  // personnage ne porte donc PAS ce qu'il décrit.
  js += ",\"draft\":";
  js += (fx::palette_cache::EncodeShare(recipe_) !=
         fx::palette_cache::EncodeShare(applied_))
            ? "true"
            : "false";
  js += ",\"ramps\":[";
  for (int i = 0; i < ramp_count_; ++i) {
    if (i) js += ',';
    js += '[';
    js += std::to_string(static_cast<int>(ramps_[i].start));
    js += ',';
    js += std::to_string(static_cast<int>(ramps_[i].length));
    js += ',';
    js += std::to_string(ramps_[i].pixels);
    js += ',';
    js += std::to_string(ramps_[i].hue);
    js += ']';
  }
  js += ']';
  if (!error_.empty()) {
    js += ",\"err\":\"";
    AppendAscii(&js, error_.c_str(), 120);
    js += '"';
  }
  js += '}';
  // Le serveur borne à 1024 : on coupe ici plutôt que de lui laisser produire un
  // JSON tronqué au milieu d'une chaîne, qu'aucun outil ne relirait.
  c.json = js.size() <= 1024 ? js : "{\"ver\":0,\"err\":\"contexte trop long\"}";

  // Le libellé est ce que le JOUEUR voit : il doit reconnaître de quoi on parle,
  // pas lire le rapport.
  c.label = i18n::Tr("Style du personnage");
  return c;
}

const uint8_t* PaletteEditor::PalettePreview(int palette_id, int* budget) {
  auto it = palette_preview_.find(palette_id);
  if (it != palette_preview_.end())
    return it->second.empty() ? nullptr : it->second.data();
  if (spr_palette_.size() < 1024) return nullptr;
  if (budget && *budget <= 0) return nullptr;  // retenté à la frame suivante
  if (budget) --*budget;

  // On ne lit QUE le `.pal` (1 Kio) et on refait la fusion nous-mêmes. Passer
  // par `palette_base::BuildForGid` relirait le `.spr` — une centaine d'images —
  // pour chacune des 553 palettes.
  //
  // 🔴 Et c'est bien la FUSION qu'on montre, pas le `.pal` nu : c'est elle que
  // l'éditeur prendra pour base si le joueur choisit cette teinte. Afficher le
  // fichier brut donnerait des vignettes à moitié noires pour les 3e/4e classes,
  // c'est-à-dire l'inverse de ce que le joueur obtiendra.
  std::vector<uint8_t>& fusion = palette_preview_[palette_id];
  char pal[160];
  std::vector<uint8_t> brut;
  if (body_path_.empty() ||
      !ro::BodyPalettePathForSprite(body_path_.c_str(), palette_id, pal,
                                    sizeof(pal)) ||
      !ro::spract::ReadFile(pal, &brut) || brut.size() < 1024) {
    return nullptr;  // mémorisé VIDE : une palette absente n'est pas relue
  }
  fusion.assign(1024, 0);
  ro::MergeServerPalette(spr_palette_.data(), spr_palette_.size(), brut.data(),
                         1024, fusion.data(), fusion.size());
  return fusion.data();
}

void PaletteEditor::SeedWornHead() {
  // 🔴 La coiffure part de celle que le personnage PORTE, et écrase donc celle
  // qu'un amorçage vient éventuellement de poser. La recette peut être en
  // retard — un passage chez un styliste NPC change `sd->status.hair` sans rien
  // nous dire — et c'est la coupe réellement portée qui fait autorité.
  recipe_.hair_style = static_cast<int16_t>(*reinterpret_cast<int*>(rag::kOwnHairStyleAddr));

  // ⛔ La COULEUR de cheveux ne se comble PAS ici, et ce fut une erreur de
  // l'essayer (2026-08-12, corrigé le jour même).
  //
  // Le besoin était réel — un code partagé doit porter une couleur, sans quoi il
  // arrive dans les cheveux du destinataire — mais l'endroit était faux. Combler
  // ici transforme « rien d'imposé » en CHOIX EXPLICITE, et ça se voit : la
  // fenêtre annonçait « Couleur de cheveux (22) » là où le joueur n'avait rien
  // demandé, sans que rien ne change ni sur lui ni sur le pantin. Une interface
  // qui revendique un choix qu'on n'a pas fait est pire qu'incomplète.
  //
  // Le comblement appartient à `ShareableRecipe`, au moment de FABRIQUER un code
  // — le seul contexte où -1 change de sens.
}

ro::PaletteRecipe PaletteEditor::ShareableRecipe() const {
  ro::PaletteRecipe out = recipe_;
  // 🔴 « Rien d'imposé » (-1) ne veut pas dire la même chose ici et là, et c'est
  // toute la subtilité de ce champ.
  //
  // Sur un joueur EN VUE, -1 est exactement juste : son acteur porte déjà la
  // couleur que le serveur lui a donnée, et ne rien imposer la laisse paraître.
  // Mieux : un passage chez le styliste se voit alors tout seul, sans que notre
  // recette ait à l'apprendre.
  //
  // Dans un CODE, le même -1 devient « la tienne » chez celui qui le colle : le
  // style arrive dans les cheveux du destinataire, qui se croit copié à tort. Un
  // code voyage sans son porteur, il doit donc être COMPLET. On fige ici ce que
  // la recette laissait ouvert — et seulement ici.
  if (out.hair_palette_id <= 0) {
    out.hair_palette_id =
        static_cast<int16_t>(*reinterpret_cast<int*>(rag::kOwnHairColorAddr));
  }
  if (out.hair_style <= 0) {
    out.hair_style = static_cast<int16_t>(*reinterpret_cast<int*>(rag::kOwnHairStyleAddr));
  }
  return out;
}

bool PaletteEditor::SeedFromShared() {
  if (seeded_) return false;
  // 🔴 Ne JAMAIS écraser ce que le joueur a déjà posé. Le rattrapage tourne à
  // chaque frame tant qu'il n'a pas abouti ; sans cette garde, il annulerait
  // sous les doigts un préréglage chargé ou un code collé.
  //
  // ⚠ Une seconde garde a existé ici — « et la recette est encore neutre » —
  // en doublure de celle-ci. Elle rendait le rattrapage MORT : l'ouverture de la
  // fenêtre pose la coupe portée, ce qui suffit à rendre la recette non neutre,
  // donc plus aucune tentative n'aboutissait après la première. Un éditeur
  // ouvert avant l'arrivée de nos couleurs restait vide pour de bon — et valider
  // depuis cet état AURAIT EFFACÉ le style du joueur. Tous les gestes qu'elle
  // prétendait protéger posent déjà `touched_`.
  if (touched_) return false;

  // 🔴🔴 Le corps se résout ICI, et surtout pas en se fiant à `body_key_`.
  //
  // Celui-ci n'est renseigné que par `Reload()`, qui tourne APRÈS nous — ses
  // deux appelants amorcent d'abord, justement parce que la recette porte la
  // teinte de base dont dépendent les rampes. À la première ouverture il vaut
  // donc ZÉRO, et `LocalRecipe(0, ...)` rend le REPLI : un joueur en selle
  // voyait sa fenêtre s'amorcer sur le style de son corps à pied, puis
  // l'afficher sur la base de sa monture. Les pastilles annonçaient des couleurs
  // qu'il n'avait jamais choisies (un noir devenu blanc), le pantin montrait des
  // zones vives — et le personnage à l'écran, lui, était parfaitement juste,
  // puisque la boucle de propagation, elle, connaît le corps.
  //
  // ⚠ Signature de cette panne, et elle vaut au-delà d'ici : quand la FENÊTRE
  // et le PERSONNAGE divergent, ce n'est pas l'injection qu'il faut suspecter,
  // c'est l'ordre dans lequel la fenêtre apprend ce qu'elle affiche.
  uint32_t cle = body_key_;
  if (cle == 0) {
    char spr[352];
    if (fx::palette_inject::ActorBodySpritePath(rag::OwnAccountId(), spr, sizeof(spr)) &&
        spr[0] != '\0')
      cle = ro::BodySpriteKey(spr);
  }
  // La variante du corps PORTÉ, pas « la » recette du joueur : depuis la v7 il
  // en a une par corps, et amorcer sur celle d'un autre lui ferait réécrire le
  // style de son personnage à pied en croyant régler sa monture.
  if (!fx::style_sync::LocalRecipe(cle, &recipe_)) return false;
  // 🔴 Verrouillé sur le SUCCÈS seulement. Le verrouiller sur un échec — ce qui
  // arrivait quand l'éditeur s'ouvrait avant que les couleurs ne soient connues
  // — figeait une recette vide pour toute la session, et le joueur ne pouvait
  // plus retoucher ses couleurs qu'après une reconnexion.
  seeded_ = true;
  // L'amorçage rend la recette du SERVEUR : ce que le personnage porte
  // réellement sur la tête reprend donc la main par-dessus.
  SeedWornHead();
  // Ce qui vient du serveur EST ce qui est posé sur le personnage : c'est le
  // point de référence à partir duquel un brouillon se mesure.
  applied_ = recipe_;
  return true;
}

bool PaletteEditor::Reload() {
  error_.clear();
  ramp_count_ = 0;
  base_.clear();
  pixels_total_ = 0;
  pixels_covered_ = 0;

  const uint32_t gid = rag::OwnAccountId();
  const int sex = OwnSex();
  const int body = rag::OwnJobId();

  // 🔴 `recipe_.palette_id` DOIT être connu avant d'arriver ici : c'est lui qui
  // choisit le fichier de palette sur lequel la base se construit, donc les
  // rampes qu'on va détecter dessus. Amorcer la recette APRÈS cet appel — ce que
  // faisait cette fonction — donnait une première ouverture où les curseurs
  // affichaient bien « palette 386 » pendant que le corps portait encore les
  // couleurs nues du sprite. La tête, elle, était juste : sa palette n'est qu'un
  // chemin de fichier, elle ne dépend pas de la base. Cheveux teints et corps
  // d'origine, c'est la signature de cette inversion.
  //
  // ── D'abord le sprite que le CLIENT a chargé ──────────────────────────────
  // 🔴 Et non celui qu'on déduirait de (classe, sexe, monture). La déduction
  // rejoue `Job_ResolveBodyClass` et sa demi-douzaine de cas particuliers —
  // styles de corps alternatifs des 3e/4e classes, montures, costumes — et dès
  // qu'elle diverge d'un cheveu, on édite la palette d'un AUTRE sprite : les
  // rampes restent parfaitement valides, mais leurs index ne désignent plus rien
  // de ce qui est à l'écran. Symptôme exact : quelques curseurs agissent sur des
  // zones au hasard, et celles que le joueur visait ne bougent jamais.
  fx::palette_base::Body b;
  fx::palette_base::Status st =
      fx::palette_base::BuildForGid(gid, recipe_.palette_id, &b);
  resolved_from_actor_ = (st != fx::palette_base::kNoSprite);

  if (!resolved_from_actor_) {
    // Repli : tant que le détour n'a pas vu l'acteur (avant le premier spawn),
    // il n'y a que la déduction. Elle vaut mieux qu'un panneau vide.
    char path[512];
    if (!ro::BodySpritePath(OwnJob(), body, sex, path, sizeof(path))) {
      error_ = i18n::Tr("Corps introuvable.");
      return false;
    }
    st = fx::palette_base::BuildFromSpritePath(path, gid, &b);
  }
  if (st != fx::palette_base::kOk) {
    error_ = fx::palette_base::StatusText(st);
    return false;
  }

  // Le corps a changé : les vignettes de teinte ne valent plus rien, elles
  // étaient fusionnées sur l'ANCIEN sprite.
  const uint32_t nouvelle_cle = ro::BodySpriteKey(b.spr_path.c_str());
  if (body_path_ != b.spr_path) {
    palette_preview_.clear();
    palette_page_ = 0;
    // Le sprite décodé de la pipette ne vaut plus rien : il décrivait l'ancien
    // corps, et ses index désigneraient d'autres pièces.
    body_res_ = ro::spract::Resource();
    body_res_tried_ = false;

    // ── Le style suit le CORPS ────────────────────────────────────────────
    //
    // 🔴 Depuis la v7, chaque corps a le sien. Enfourcher une monture déjà
    // habillée doit donc ramener SES réglages, sinon le joueur regarderait les
    // couleurs de sa monture en manipulant celles de son corps à pied — et la
    // première validation écraserait ces dernières.
    //
    // ⚠ Sauf s'il a du travail EN COURS : un réglage non validé ne se jette pas
    // sous prétexte qu'on a changé de monture. Dans ce cas on garde ce qu'il
    // compose, qui est aussi ce que l'acteur affiche (le repli).
    const bool travail_en_cours =
        fx::palette_cache::EncodeShare(recipe_) !=
        fx::palette_cache::EncodeShare(applied_);
    ro::PaletteRecipe sienne;
    bool exacte = false;
    // ⚠ `body_key_` nul compte comme un changement, et c'est une ceinture : à la
    // toute première résolution du corps, il vaut zéro. Si l'amorçage n'a pas
    // encore eu lieu — ou s'il a rendu le repli — c'est ici qu'on rattrape la
    // variante propre à ce corps, tant que rien n'est en cours de réglage.
    if (nouvelle_cle != body_key_ && !travail_en_cours &&
        fx::style_sync::LocalRecipe(nouvelle_cle, &sienne, &exacte) && exacte) {
      recipe_ = sienne;
      applied_ = sienne;
      SeedWornHead();  // la tête reste celle que le personnage porte
    }
  }
  body_path_ = b.spr_path;
  body_key_ = nouvelle_cle;
  loaded_body_ = body;
  loaded_sex_ = sex;
  base_ = b.base;
  spr_palette_ = b.spr_palette;
  std::memcpy(ramps_, b.ramps, sizeof(ramps_));
  ramp_count_ = b.ramp_count;
  pixels_total_ = b.pixels_total;
  pixels_covered_ = b.pixels_covered;

  survolee_ = -1;  // le corps a changé : plus rien n'est survolé
  // 🔴 Aucune injection ici. Ouvrir l'éditeur, changer de teinte de base ou
  // charger un préréglage ne DOIT pas modifier le personnage : tout cela ne
  // nourrit que le pantin. Le sprite en scène n'apprend rien avant la
  // validation.
  return true;
}

void PaletteEditor::ResetForNewCharacter() {
  recipe_ = ro::PaletteRecipe();
  applied_ = ro::PaletteRecipe();
  // 🔴 Le verrou d'amorçage est le plus traître des trois : il dit « déjà
  // amorcé », donc rouvrir la fenêtre après un changement de personnage
  // ressortait la recette du PRÉCÉDENT et la proposait à valider.
  seeded_ = false;
  touched_ = false;
  draft_seen_.clear();
  draft_tick_ = 0.0;
  // Vider le chemin suffit à faire jeter par `Reload` tout ce qui en dérive :
  // vignettes de teinte, sprite décodé de la pipette, rampes, base fusionnée.
  body_path_.clear();
  // L'aperçu d'un style reçu porte le corps de l'ancien personnage.
  preview_body_path_.clear();
  preview_base_.clear();
  preview_ramp_count_ = 0;

  // 🔴 On ne RÉAMORCE pas ici, et c'est délibéré : la recette locale appartient
  // à `StyleSync`, qui purge la sienne dans SON tour de frame. Rien ne dit que
  // le nôtre passe après — amorcer maintenant figerait peut-être la recette de
  // l'ancien personnage, définitivement, puisque l'amorçage ne se fait qu'une
  // fois. Le rattrapage par frame s'en charge dès que la valeur est saine.
  //
  // La tête, elle, se lit sur les globales du client : aucun ordre à supposer.
  if (open_) {
    SeedWornHead();
    applied_ = recipe_;
  }
}

void PaletteEditor::TickDraft() {
  const uint32_t cid = rag::OwnCharId();
  if (cid == 0) return;

  const std::string courant = fx::palette_cache::EncodeShare(recipe_);
  if (courant != draft_seen_) {
    // Le joueur vient de bouger quelque chose : on note l'instant et on attend
    // qu'il s'arrête. Un glissement de curseur repasse ici à chaque frame et
    // repousse donc l'écriture d'autant.
    draft_seen_ = courant;
    draft_tick_ = ImGui::GetTime();
    return;
  }
  if (draft_tick_ == 0.0) return;                          // rien en attente
  if (ImGui::GetTime() - draft_tick_ < kDraftDelay) return;
  draft_tick_ = 0.0;

  // 🔴 On n'enregistre que l'ÉCART avec ce qui est posé : un état identique à ce
  // que le personnage porte n'est pas un brouillon, et le proposer ferait un
  // bouton dont le clic ne change rien — ce qui se lit comme une panne.
  //
  // ⛔ Mais revenir à cet état n'EFFACE PAS le brouillon, et cette asymétrie est
  // le cœur du filet. Elle l'a d'ailleurs été à l'envers : le brouillon était
  // effacé dès que l'écart se refermait, si bien que « Revenir à mon style »
  // aurait jeté ce qu'il est censé rendre récupérable. Le brouillon garde donc
  // le dernier état NON VALIDÉ — c'est ce que son nom promet — et ne disparaît
  // qu'à la validation ou à la suppression du style.
  if (courant != fx::palette_cache::EncodeShare(applied_))
    fx::palette_cache::DraftSave(cid, &recipe_);
}

void PaletteEditor::Apply() {
  if (base_.size() < 1024 || ramp_count_ == 0) return;
  const uint32_t gid = rag::OwnAccountId();
  if (gid == 0) return;
  touched_ = true;  // geste explicite : plus de rattrapage d'amorçage
  // Ce qui est validé n'est plus un brouillon : la réserve se vide, et l'écart
  // qui la nourrit retombe à zéro.
  applied_ = recipe_;
  fx::palette_cache::DraftSave(rag::OwnCharId(), nullptr);
  draft_tick_ = 0.0;
  // 🔴 On injecte MÊME quand tous les réglages sont à zéro, et c'est délibéré.
  //
  // La base de l'édition est la palette INTERNE du .spr, pas la palette externe
  // que le joueur porte (body_<n>.pal). Il le faut : dans celle du jeu, 126
  // index sur 255 sont noirs (mesuré en mémoire vive), et un réglage HSV est
  // MULTIPLICATIF — sur du noir, v vaut 0, donc aucun curseur ne peut jamais le
  // rallumer. Éditer par-dessus la palette externe serait impuissant là où
  // l'éditeur sert le plus : les 4e classes et les montures, dont 40 à 62 % des
  // pixels sont noircis.
  //
  // Valider des réglages nuls a donc un sens : c'est demander les couleurs
  // d'origine du sprite, ce que le chemin natif ne sait pas produire. Pour
  // n'avoir plus AUCUNE palette imposée, il faut « Supprimer mon style ».
  // Niveau info, pas diagnostic : ce n'est pas une anomalie, c'est un geste
  // explicite du joueur — donc rare, et sans risque d'inonder le journal. La
  // boucle de propagation, elle, ne dit plus rien quand tout va bien (cf.
  // style_sync) : une pose par joueur visible, c'était du bruit permanent.
  LogInfo("[palette] éditeur corps={:08x} rampes={} pal={} couverture={}/{}",
          body_key_, ramp_count_, static_cast<int>(recipe_.palette_id),
          pixels_covered_, pixels_total_);
  fx::palette_inject::SetRecipe(gid, base_.data(), ramps_, ramp_count_,
                                recipe_);
  // La tête suit le même geste, mais par un tout autre chemin : sa palette est
  // un vrai fichier, on se contente de la désigner.
  fx::palette_inject::SetHairPalette(gid, recipe_.hair_palette_id);
}

void PaletteEditor::RestoreServerColors() {
  const uint32_t gid = rag::OwnAccountId();
  if (gid == 0) return;
  // 🔴 Constructeur par défaut, JAMAIS `memset` : les sentinelles valent -1, et
  // un memset demanderait la palette 0 et la couleur de cheveux 0.
  recipe_ = ro::PaletteRecipe();
  // Même état canonique que « tout réinitialiser » : la fenêtre repart de la
  // première coiffure, et non de celle que le personnage porte.
  //
  // ⚠ Le personnage, LUI, garde sa coupe actuelle. « Supprimer mon style »
  // retire nos palettes — ici et sur le serveur — mais ne peut pas défaire un
  // changement de coiffure : la coupe d'AVANT n'est mémorisée nulle part, et
  // imposer la n°1 à la place perdrait celle du joueur sans retour possible.
  // Pour en changer, il en choisit une et valide.
  recipe_.hair_style = 1;
  // Retirer la recette rend le joueur au chemin natif STRICT : sa couleur de
  // vêtement, son body_<n>.pal, exactement comme si ce module n'existait pas.
  fx::palette_inject::ClearRecipe(gid);
  fx::palette_inject::ClearHairPalette(gid);
  // ⚠ La COIFFURE, elle, n'est pas défaite : elle appartient au serveur et a
  // été appliquée pour de bon. « Supprimer mon style » retire nos palettes, pas
  // un passage chez le styliste — pour en changer, il faut en choisir une autre
  // et valider.
  // Geste explicite : plus de rattrapage d'amorçage par-dessus (cf. touched_).
  touched_ = true;
  // L'apparence native devient ce qui est posé, et la réserve se vide : après
  // avoir demandé le vide, on ne se voit pas proposer de recharger l'avant.
  applied_ = recipe_;
  fx::palette_cache::DraftSave(rag::OwnCharId(), nullptr);
  draft_tick_ = 0.0;
  // 🔴 Purger AUSSI le registre de propagation. Notre propre recette y dort
  // depuis le login (le serveur nous la repousse), et la boucle d'application la
  // REPOSERAIT au tick suivant — `HasRecipe` vient justement de retomber, donc
  // sa garde ne retient plus rien. Le personnage reprenait alors ses anciennes
  // couleurs pendant que les pièces affichaient des réglages à zéro.
  fx::style_sync::ForgetLocal();
  // Et les AUTRES doivent l'oublier aussi, sinon ils continueraient d'afficher
  // des couleurs que le joueur vient d'abandonner.
  // TOUT, et pas seulement le corps porté : quelqu'un qui demande à redevenir
  // lui-même ne s'attend pas à devoir le redemander une fois en selle.
  fx::style_sync::SendClearAll();
  shared_tick_ = 0.0;
  // Rebâtir la base : elle avait été fusionnée sur l'ancienne teinte, et la
  // fenêtre reste ouverte. Rien n'est injecté — `Reload` ne touche jamais au
  // personnage.
  Reload();
}

void PaletteEditor::ForgetCurrentBodyStyle() {
  const uint32_t gid = rag::OwnAccountId();
  if (gid == 0 || body_key_ == 0) return;
  // Le serveur d'abord : c'est lui qui tient la liste des corps habillés, et
  // c'est de lui que les autres joueurs tiendront la nouvelle.
  fx::style_sync::SendClear(body_key_);
  touched_ = true;
  fx::palette_cache::DraftSave(rag::OwnCharId(), nullptr);
  draft_tick_ = 0.0;

  // 🔴 Poser le repli NOUS-MÊMES. L'écho du serveur arrivera bien, mais la
  // boucle de propagation refuse de poser notre propre style tant que cette
  // fenêtre est ouverte — pour ne pas faire sauter en arrière un réglage en
  // cours. Sans ce qui suit, le corps garderait donc à l'écran le style qu'on
  // vient justement de supprimer.
  ro::PaletteRecipe repli;
  if (fx::style_sync::LocalRecipe(body_key_, &repli)) {
    recipe_ = repli;
    SeedWornHead();  // la tête reste celle que le personnage porte
    Apply();         // pose sur le personnage, sans rien renvoyer au serveur
  } else {
    // C'était la dernière : il n'y a plus rien à porter.
    fx::palette_inject::ClearRecipe(gid);
    fx::palette_inject::ClearHairPalette(gid);
    recipe_ = ro::PaletteRecipe();
    recipe_.hair_style = 1;
    applied_ = recipe_;
  }
  // La base ne change pas (même corps), mais les vignettes et l'état affiché en
  // dépendent — et `Reload` est le seul endroit qui les remette d'aplomb.
  Reload();
}

int PaletteEditor::PickPart(float sx, float sy) const {
  if (doll_scale_ <= 0.0f || ramp_count_ <= 0 || !body_res_.ok) return -1;

  // Écran -> unités `.act`. `DollPlacement` donne l'origine du repère du sprite
  // (les PIEDS de l'acteur) et l'échelle : l'inverse est une soustraction et une
  // division, sans rien à redevinner du cadrage.
  const float ax = (sx - doll_origin_x_) / doll_scale_;
  const float ay = (sy - doll_origin_y_) / doll_scale_;

  // Pose = anim*8 + dir, image 0 : le pantin est figé (`anim_seconds` négatif).
  const size_t pose = static_cast<size_t>(doll_dir_ & 7);
  if (pose >= body_res_.actions.size()) return -1;
  const ro::spract::Action& action = body_res_.actions[pose];
  if (action.frames.empty()) return -1;
  const ro::spract::Frame& frame = action.frames[0];

  // Du DESSUS vers le dessous : c'est le premier calque opaque qui répond, comme
  // à l'écran. Un calque transparent en ce point laisse passer la question au
  // suivant — sans quoi une manche translucide masquerait le torse.
  for (int i = static_cast<int>(frame.layers.size()) - 1; i >= 0; --i) {
    const ro::spract::Layer& l = frame.layers[i];
    // 🔴 Section Indexed8 uniquement : une image Bgra32 porte ses couleurs par
    // pixel et n'a AUCUN index — il n'y a rien à désigner dessus.
    if (l.index < 0 || l.type != 0) continue;
    if (l.index >= static_cast<int>(body_res_.indexed.size())) continue;
    const ro::spract::Image& img = body_res_.indexed[l.index];
    if (img.w <= 0 || img.h <= 0 || img.index.empty()) continue;

    // Le calque est centré sur son offset — même convention que
    // `SpriteResolveFrame`, dont ce test est l'inverse.
    const float w = img.w * l.scale_x;
    const float h = img.h * l.scale_y;
    if (w <= 0.0f || h <= 0.0f) continue;
    const float x0 = l.off_x - w * 0.5f;
    const float y0 = l.off_y - h * 0.5f;
    if (ax < x0 || ay < y0 || ax >= x0 + w || ay >= y0 + h) continue;

    float u = (ax - x0) / w;
    if (l.mirror) u = 1.0f - u;
    const float v = (ay - y0) / h;
    int px = static_cast<int>(u * img.w);
    int py = static_cast<int>(v * img.h);
    if (px < 0) px = 0;
    if (px >= img.w) px = img.w - 1;
    if (py < 0) py = 0;
    if (py >= img.h) py = img.h - 1;

    const int idx = img.index[static_cast<size_t>(py) * img.w + px];
    if (idx == 0) continue;  // transparent : le calque du dessous répondra

    for (int r = 0; r < ramp_count_; ++r) {
      if (idx >= ramps_[r].start && idx < ramps_[r].start + ramps_[r].length)
        return r;
    }
    // Index réel, mais hors de toute rampe : c'est justement la part du corps
    // qu'aucun curseur n'atteint (cf. « Réglable : N% »). Répondre -1 est exact.
    return -1;
  }
  return -1;
}

void PaletteEditor::DrawPreviewDoll(float size, int highlight) {
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##pantin", ImVec2(size, size));
  const bool survole = ImGui::IsItemHovered();

  // 🔴 La molette passe par le VERROU anti-défilement du toolkit. Sans lui,
  // parcourir la fenêtre à la molette ferait pivoter le pantin au passage — et
  // inversement, le faire pivoter ferait défiler la fenêtre.
  const float roue = mui::LastItemWheel();
  if (roue != 0.0f) {
    doll_dir_ = (doll_dir_ + (roue > 0.0f ? 1 : 7)) % 8;
  }
  if (survole) {
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(i18n::Tr("Molette : tourner"));
    ImGui::EndTooltip();
  }

  ro::DollLook look;
  look.sex = OwnSex();
  look.job = OwnJob();
  look.body = rag::OwnJobId();
  look.hair = *reinterpret_cast<int*>(rag::kOwnHairStyleAddr);
  look.hair_color = *reinterpret_cast<int*>(rag::kOwnHairColorAddr);
  look.clothes_color = *reinterpret_cast<int*>(rag::kOwnClothesColorAddr);
  // 🔴 Rien d'autre : ni coiffe, ni cape, ni arme. On règle la palette du CORPS,
  // et un chapeau volumineux rétrécirait le personnage tout en masquant les
  // pièces qu'on cherche justement à voir.
  if (recipe_.hair_palette_id > 0) look.hair_color = recipe_.hair_palette_id;
  if (recipe_.hair_style > 0) look.hair = recipe_.hair_style;
  // 🔴 Le sprite LU sur l'acteur, pas celui qu'on déduirait de (classe, sexe).
  // La déduction rate les 3e et 4e classes — le pantin les montrait en tenue de
  // base pendant que le personnage portait sa vraie robe. C'est exactement la
  // raison pour laquelle `Reload` lit déjà le sprite au lieu de le calculer.
  if (!body_path_.empty()) look.body_spr_override = body_path_.c_str();

  // 🔴 La palette est RECALCULÉE ici, et surtout pas relue de l'injection.
  //
  // Ce pantin EST l'aperçu : le personnage en scène, lui, ne bouge plus tant que
  // le joueur n'a pas validé. Lire l'injection montrerait donc son apparence
  // ACTUELLE — c'est-à-dire tout sauf ce qu'il est en train de régler.
  static uint8_t rgba[1024];
  static std::string key;
  const uint32_t gid = rag::OwnAccountId();
  if (gid != 0 && base_.size() >= 1024 &&
      ro::ApplyRecipe(base_.data(), base_.size(), ramps_, ramp_count_, recipe_,
                      rgba, sizeof(rgba))) {
    key = fx::palette_cache::DollKey(gid, rgba);

    // ── Pulsation de la pièce survolée ────────────────────────────────────
    //
    // 🔴 On pulse la LUMINOSITÉ, pas l'alpha. `spract::DecodePalette` force
    // l'opacité sur tous les index sauf le 0 — c'est la convention RO
    // (`NoTransparencyExceptFirstPixel`), et s'en écarter rendrait la plupart
    // des sprites invisibles. Un éclaircissement se lit de toute façon mieux
    // qu'une transparence sur un fond chargé.
    //
    // 🔴🔴 Le niveau est QUANTIFIÉ, et c'est la contrainte qui commande tout le
    // reste. Le composeur met en cache une texture par CLÉ de teinte : une
    // pulsation continue produirait une clé neuve à chaque frame, donc une
    // texture neuve à chaque frame, indéfiniment. Quatre paliers suffisent à
    // l'œil et bornent le coût à quatre palettes par pièce.
    if (highlight >= 0 && highlight < ramp_count_) {
      constexpr int kPaliers = 4;
      const double t = ImGui::GetTime() * 2.0;  // ~2 aller-retours par seconde
      const double onde = 0.5 - 0.5 * std::cos(t * 3.14159265);  // 0..1 doux
      const int palier = static_cast<int>(onde * (kPaliers - 1) + 0.5);
      // Modifié EN PLACE : `ApplyRecipe` réécrit `rgba` à chaque frame, il n'y a
      // donc rien à préserver d'une frame à l'autre.
      const int debut = ramps_[highlight].start;
      const int fin = debut + ramps_[highlight].length;
      const int force = 60 + palier * 55;  // 60..225, vers le blanc
      for (int i = debut; i < fin && i < 256; ++i) {
        for (int c = 0; c < 3; ++c) {
          const int v = rgba[4 * i + c] + force;
          rgba[4 * i + c] = static_cast<uint8_t>(v > 255 ? 255 : v);
        }
      }
      // La clé porte la pièce ET le palier : sans eux, le composeur ressortirait
      // la texture de la palette non pulsée.
      char suffixe[48];
      std::snprintf(suffixe, sizeof(suffixe), ":hl%d:%d", highlight, palier);
      key += suffixe;
    }

    look.body_palette = rgba;
    look.body_palette_key = key.c_str();
  }

  ro::DollDrawOpts opts;
  opts.dir = doll_dir_;
  opts.anim = 0;              // pose debout
  opts.anim_seconds = -1.0f;  // figée : un aperçu de couleur n'a pas à s'agiter
  // 🔴 PAS de `fit_body_only` ici, contrairement à l'aperçu d'équipement. Ce
  // drapeau calcule l'échelle sur le CORPS SEUL et laisse délibérément déborder
  // tout le reste — la tête comprise, qui sortait alors par le haut du
  // rectangle et recouvrait le texte au-dessus.
  //
  // Il existe pour qu'un chapeau volumineux ne rétrécisse pas le personnage
  // d'un article survolé au suivant. Ce pantin-ci ne porte NI coiffe NI cape :
  // sa silhouette EST la tête plus le corps, donc le cadrage par défaut — qui
  // fait tout rentrer — est à la fois juste et stable.

  // Et un rognage, par sécurité. Le cadrage ci-dessus suffit pour un pantin nu,
  // mais `DrawDoll` ne rogne rien : le jour où l'on ajoutera une pièce à cet
  // aperçu, le débordement reviendrait recouvrir le texte au lieu d'être coupé
  // net dans son cadre.
  // Le cadrage résolu : c'est lui qui permettra à la pipette de convertir un
  // point écran en unités `.act`.
  ro::DollPlacement place;
  opts.out_placement = &place;

  ImGui::PushClipRect(p0, ImVec2(p0.x + size, p0.y + size), true);
  const bool dessine =
      ro::DrawDoll(ImGui::GetWindowDrawList(), look, p0.x, p0.y, size, size,
                   opts);
  ImGui::PopClipRect();
  if (dessine) {
    doll_origin_x_ = place.origin_x;
    doll_origin_y_ = place.origin_y;
    doll_scale_ = place.scale;
  }

  // ── La pipette ────────────────────────────────────────────────────────────
  // Le `.spr` n'est décodé qu'au PREMIER survol : la plupart des ouvertures de
  // la fenêtre n'y touchent jamais, et l'analyse coûte une centaine d'images.
  if (survole && !body_res_tried_ && !body_path_.empty()) {
    body_res_tried_ = true;
    const std::string spr = body_path_ + ".spr";
    const std::string act = body_path_ + ".act";
    ro::spract::Load(spr.c_str(), act.c_str(), &body_res_);
  }
  pipette_ = survole ? PickPart(ImGui::GetIO().MousePos.x,
                                ImGui::GetIO().MousePos.y)
                     : -1;
}

void PaletteEditor::DrawHairStylePicker() {
  if (!ImGui::BeginPopup("##coiffures")) return;
  constexpr int kCols = 8;
  constexpr float kCote = 34.0f;
  const int maxi = fx::style_sync::kHairStyleMax;
  ImGui::TextUnformatted(i18n::Tr("Coiffure"));
  // Dit sur place pourquoi des cases sont vides : sans ça, le joueur cherche un
  // bug là où il n'y en a pas.
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 20.0f);
  ImGui::TextUnformatted(
      i18n::Tr("Certains numéros n'ont pas de sprite : leur case reste vide."));
  ImGui::PopTextWrapPos();
  ImGui::BeginChild(
      "##g",
      ImVec2(LargeurGrille(kCols, kCote, GrilleDefile(maxi, kCols, 10)),
             HauteurGrille(maxi, kCols, kCote, 10)),
      true);
  const int sex = OwnSex();
  const int job = OwnJob();
  // La vignette porte la couleur CHOISIE : on compare des coupes telles qu'on
  // les portera, pas dans une teinte qu'on vient d'abandonner.
  const int couleur = recipe_.hair_palette_id > 0 ? recipe_.hair_palette_id
                                                  : *reinterpret_cast<int*>(rag::kOwnHairColorAddr);
  int choix = recipe_.hair_style > 0 ? recipe_.hair_style
                                     : *reinterpret_cast<int*>(rag::kOwnHairStyleAddr);
  bool ferme = false;
  if (GrillePicker(1, maxi, kCols, kCote, &choix,
                   [&](int v, ImVec2 p, float c) {
                     ro::DrawHeadIcon(ImGui::GetWindowDrawList(), p.x, p.y, c, v,
                                      sex, couleur, /*allow_upscale=*/true,
                                      job);
                   },
                   &ferme)) {
    recipe_.hair_style = static_cast<int16_t>(choix);
    touched_ = true;
  }
  ImGui::EndChild();
  // 🔴 Après `EndChild`, jamais dedans : la fermeture vise le popup, et la
  // demander depuis un enfant reviendrait à parier sur la pile de fenêtres
  // d'ImGui plutôt que sur ce qu'on veut dire.
  if (ferme) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void PaletteEditor::DrawHairColorPicker() {
  if (!ImGui::BeginPopup("##couleurs_cheveux")) return;
  constexpr int kCols = 16;
  constexpr float kCote = 24.0f;
  const int maxi = fx::palette_inject::kHairPaletteMax;
  ImGui::TextUnformatted(i18n::Tr("Couleur de cheveux"));
  ImGui::BeginChild(
      "##g",
      ImVec2(LargeurGrille(kCols, kCote, GrilleDefile(maxi, kCols, 10)),
             HauteurGrille(maxi, kCols, kCote, 10)),
      true);
  const int sex = OwnSex();
  const int job = OwnJob();
  const int coupe = recipe_.hair_style > 0 ? recipe_.hair_style
                                           : *reinterpret_cast<int*>(rag::kOwnHairStyleAddr);
  // Chaque case = SA PROPRE coupe dans la teinte N. C'est ce que fait l'écran de
  // création, et c'est la seule façon honnête de montrer une couleur de cheveux :
  // une pastille unie ne dit rien du dégradé qu'une palette applique.
  int choix = recipe_.hair_palette_id > 0 ? recipe_.hair_palette_id : 0;
  bool ferme = false;
  if (GrillePicker(1, maxi, kCols, kCote, &choix,
                   [&](int v, ImVec2 p, float c) {
                     ro::DrawHeadIcon(ImGui::GetWindowDrawList(), p.x, p.y, c,
                                      coupe, sex, v, /*allow_upscale=*/true,
                                      job);
                   },
                   &ferme)) {
    recipe_.hair_palette_id = static_cast<int16_t>(choix);
    touched_ = true;
  }
  ImGui::EndChild();
  if (ferme) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void PaletteEditor::DrawBodyPalettePicker() {
  if (!ImGui::BeginPopup("##palettes_corps")) return;
  // 🔴 Le VRAI CORPS peint, pas une pastille de couleur moyenne. L'essai avec
  // une moyenne a été rejeté à l'usage : la palette 484 rendait un beige qui ne
  // laissait rien présager du vert qu'elle contient. Une palette de vêtement
  // n'est pas une teinte, c'est une distribution — et seul le sprite la montre.
  //
  // ⚠ Le prix, c'est UNE TEXTURE PAR PALETTE (même sprite, autre palette). D'où
  // la pagination : 553 vignettes vivantes satureraient le cache de
  // `ui/sprite_view`, qui évincerait alors en pleine frame des textures déjà
  // dessinées. Une page en tient 48.
  constexpr int kCols = 8;
  constexpr int kRows = 6;
  constexpr int kParPage = kCols * kRows;
  constexpr float kCote = 46.0f;
  const int maxi = fx::palette_base::kOfficialPaletteMax;
  const int pages = (maxi + kParPage - 1) / kParPage;
  if (palette_page_ >= pages) palette_page_ = pages - 1;
  if (palette_page_ < 0) palette_page_ = 0;

  ImGui::TextUnformatted(i18n::Tr("Teinte de base"));
  // Pagination. « < » et « > » sont dans la plage 0x20-0xFF que couvre la police
  // de l'interface — une flèche d'icône n'y serait qu'un rectangle vide.
  if (ro::RoButton("<", ImGui::GetFontSize() * 1.6f) && palette_page_ > 0)
    --palette_page_;
  ImGui::SameLine();
  char page[64];
  std::snprintf(page, sizeof(page), i18n::Tr("Page %d/%d"), palette_page_ + 1,
                pages);
  ImGui::TextUnformatted(page);
  ImGui::SameLine();
  if (ro::RoButton(">", ImGui::GetFontSize() * 1.6f) &&
      palette_page_ + 1 < pages)
    ++palette_page_;

  const int premier = palette_page_ * kParPage + 1;
  int dernier = premier + kParPage - 1;
  if (dernier > maxi) dernier = maxi;

  // Paginée : jamais d'ascenseur, une page tient exactement dans la grille.
  ImGui::BeginChild(
      "##g",
      ImVec2(LargeurGrille(kCols, kCote, /*avec_ascenseur=*/false),
             HauteurGrille(kParPage, kCols, kCote, kRows)),
      true);
  // Budget de lecture par frame : la page se remplit en deux ou trois images au
  // lieu d'ouvrir 48 fichiers dans la même.
  int budget = 12;
  int choix = recipe_.palette_id > 0 ? recipe_.palette_id : 0;
  bool ferme = false;
  if (GrillePicker(premier, dernier, kCols, kCote, &choix,
                   [&](int v, ImVec2 p, float c) {
                     const uint8_t* rgba = PalettePreview(v, &budget);
                     if (!rgba) return;  // pas encore lue, ou introuvable
                     // 🔴 La clé identifie le CONTENU : le sprite ET le numéro.
                     // Le cache de teintes partage ses textures entre appels de
                     // même clé, et deux corps différents ne doivent pas se
                     // repasser les leurs.
                     char cle[320];
                     std::snprintf(cle, sizeof(cle), "%s#bp%d",
                                   body_path_.c_str(), v);
                     ro::SpriteRes res;
                     if (!ro::LoadSpritePairRawPalette(body_path_.c_str(),
                                                       body_path_.c_str(), cle,
                                                       rgba, &res))
                       return;
                     ro::DrawSprite(ImGui::GetWindowDrawList(), res, p,
                                    ImVec2(p.x + c, p.y + c),
                                    /*anim_seconds=*/0.0f, /*action=*/0,
                                    /*ms_per_frame=*/0.0f,
                                    /*allow_upscale=*/true);
                   },
                   &ferme)) {
    recipe_.palette_id = static_cast<int16_t>(choix);
    touched_ = true;
    // 🔴 Rechargement COMPLET : la teinte de base change la fusion, donc le
    // découpage des rampes. Les réglages en place se ré-appliquent par index.
    Reload();
  }
  ImGui::EndChild();
  if (ferme) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void PaletteEditor::OpenStylePreview(const char* code, const char* owner_utf8) {
  if (code == nullptr || *code == '\0') return;
  ro::PaletteRecipe recu;
  if (!fx::palette_cache::DecodeShare(code, &recu)) return;

  preview_recipe_ = recu;
  preview_code_ = code;
  preview_owner_ = owner_utf8 != nullptr ? owner_utf8 : "";
  preview_dir_ = 0;

  // 🔴 La base n'est PAS construite ici, mais à la première frame — et le chemin
  // vidé est ce qui la déclenche. Elle l'était ici, et l'aperçu montrait alors
  // un corps qui n'était pas celui du joueur : cette fenêtre s'ouvre depuis un
  // LIEN DE CHAT, sans que l'éditeur ait forcément servi, donc à un moment où
  // rien n'a encore résolu le sprite porté. Différer d'une frame fait tomber ce
  // cas et celui du joueur qui change de tenue pendant l'aperçu, par le même
  // chemin.
  preview_body_path_.clear();
  preview_base_.clear();
  preview_ramp_count_ = 0;
  preview_open_ = true;
}

void PaletteEditor::RebuildStylePreview() {
  preview_base_.clear();
  preview_ramp_count_ = 0;
  // 🔴 La base est construite avec la teinte de base DU STYLE REÇU, pas la
  // nôtre : c'est elle qui décide de la fusion, donc du découpage des rampes.
  // Réutiliser notre `base_` appliquerait les réglages reçus à côté.
  fx::palette_base::Body b;
  if (fx::palette_base::BuildForGid(rag::OwnAccountId(), preview_recipe_.palette_id, &b) !=
      fx::palette_base::kOk)
    return;
  preview_base_ = b.base;
  std::memcpy(preview_ramps_, b.ramps, sizeof(preview_ramps_));
  preview_ramp_count_ = b.ramp_count;
}

void PaletteEditor::TryStyleCode(const char* code) {
  if (code == nullptr || *code == '\0') return;
  ro::PaletteRecipe recu;
  if (!fx::palette_cache::DecodeShare(code, &recu)) return;
  recipe_ = recu;
  touched_ = true;
  SetOpen(true);
  // 🔴 `SetOpen` réamorce la coiffure sur celle que le personnage PORTE : ici
  // c'est celle du style reçu qu'on veut essayer, donc on la repose APRÈS.
  if (recu.hair_style > 0) recipe_.hair_style = recu.hair_style;
  Reload();
}

void PaletteEditor::DrawStylePreview() {
  if (!preview_open_) return;

  // ── Le corps sur lequel l'aperçu se pose ──────────────────────────────────
  //
  // 🔴 Relu sur l'ACTEUR à chaque frame, et non hérité de l'éditeur. Deux cas,
  // tous deux constatés :
  //   * cette fenêtre s'ouvre depuis un lien de chat, sans que l'éditeur ait
  //     jamais servi. `body_path_` était alors vide, le pantin retombait sur le
  //     sprite déduit de (classe, sexe), et les tenues de 3e et 4e classe
  //     disparaissaient : le style s'affichait sur un corps qui n'était pas le
  //     nôtre — donc sur d'autres pièces que celles qu'on croyait voir ;
  //   * le joueur équipe ou retire sa tenue pendant que l'aperçu est ouvert, et
  //     il attend de le voir suivre.
  // Même leçon qu'ailleurs dans ce module : le sprite RÉELLEMENT porté fait
  // autorité, jamais ce qu'on en déduirait.
  //
  // Le chemin sert aussi de témoin : tant qu'il ne bouge pas, on ne reconstruit
  // rien — une reconstruction coûte l'analyse complète d'un `.spr`.
  {
    char spr[352];
    const bool lu =
        fx::palette_inject::ActorBodySpritePath(rag::OwnAccountId(), spr, sizeof(spr));
    const char* porte = (lu && spr[0] != '\0') ? spr : "";
    if (preview_body_path_ != porte) {
      preview_body_path_ = porte;
      RebuildStylePreview();
    }
  }

  char titre[128];
  std::snprintf(titre, sizeof(titre), "%s###style_preview",
                i18n::Tr("Aperçu d'un style"));
  if (ro::BeginRoWindow(titre, &preview_open_,
                        ImGuiWindowFlags_AlwaysAutoResize |
                            ImGuiWindowFlags_NoCollapse)) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 16.0f);
    char ligne[160];
    std::snprintf(ligne, sizeof(ligne), i18n::Tr("Le style de %s, sur toi."),
                  preview_owner_.empty() ? "?" : preview_owner_.c_str());
    ImGui::TextUnformatted(ligne);
    ImGui::PopTextWrapPos();

    const float taille = ImGui::GetFontSize() * 9.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##apercu", ImVec2(taille, taille));
    const float roue = mui::LastItemWheel();
    if (roue != 0.0f) preview_dir_ = (preview_dir_ + (roue > 0.0f ? 1 : 7)) % 8;
    if (ImGui::IsItemHovered()) {
      ImGui::BeginTooltip();
      ImGui::TextUnformatted(i18n::Tr("Molette : tourner"));
      ImGui::EndTooltip();
    }

    ro::DollLook look;
    look.sex = OwnSex();
    look.job = OwnJob();
    look.body = rag::OwnJobId();
    look.hair = *reinterpret_cast<int*>(rag::kOwnHairStyleAddr);
    look.hair_color = *reinterpret_cast<int*>(rag::kOwnHairColorAddr);
    look.clothes_color = *reinterpret_cast<int*>(rag::kOwnClothesColorAddr);
    if (preview_recipe_.hair_palette_id > 0)
      look.hair_color = preview_recipe_.hair_palette_id;
    if (preview_recipe_.hair_style > 0) look.hair = preview_recipe_.hair_style;
    // Le sprite de l'aperçu, pas celui de l'éditeur : l'éditeur peut n'avoir
    // jamais été ouvert.
    if (!preview_body_path_.empty())
      look.body_spr_override = preview_body_path_.c_str();

    static uint8_t rgba[1024];
    static std::string cle;
    if (preview_base_.size() >= 1024 &&
        ro::ApplyRecipe(preview_base_.data(), preview_base_.size(),
                        preview_ramps_, preview_ramp_count_, preview_recipe_,
                        rgba, sizeof(rgba))) {
      cle = fx::palette_cache::DollKey(rag::OwnAccountId(), rgba);
      cle += ":apercu";  // jamais la même clé que le pantin de l'éditeur
      look.body_palette = rgba;
      look.body_palette_key = cle.c_str();
    }

    ro::DollDrawOpts opts;
    opts.dir = preview_dir_;
    opts.anim = 0;
    opts.anim_seconds = -1.0f;
    ImGui::PushClipRect(p0, ImVec2(p0.x + taille, p0.y + taille), true);
    ro::DrawDoll(ImGui::GetWindowDrawList(), look, p0.x, p0.y, taille, taille,
                 opts);
    ImGui::PopClipRect();

    if (ro::RoButton(i18n::Tr("Essayer dans l'éditeur"))) {
      TryStyleCode(preview_code_.c_str());
      preview_open_ = false;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::BeginTooltip();
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 20.0f);
      ImGui::TextUnformatted(
          i18n::Tr("Charge ce style dans l'éditeur. Ton personnage ne change "
                   "qu'à la validation."));
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
    }
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Copier le code")))
      ImGui::SetClipboardText(preview_code_.c_str());
  }
  ro::EndRoWindow();
}

void PaletteEditor::Toggle() { SetOpen(!open_); }

void PaletteEditor::SetOpen(bool open) {
  // Ré-ouvrir une fenêtre déjà ouverte ne doit RIEN faire : un NPC qui répète sa
  // commande relancerait sinon un rechargement complet — donc une réinjection —
  // au milieu des réglages du joueur.
  if (open == open_) return;
  open_ = open;
  if (open_) {
    // 🔴 Tout ceci ne vaut QUE tant que le joueur n'a rien posé.
    //
    // Rouvrir la fenêtre ne doit rien changer à ce qu'il avait composé : il l'a
    // FERMÉE, pas annulée. Sans cette garde, un « tout réinitialiser » suivi
    // d'une fermeture revenait défait à la réouverture — la coupe repassait à
    // celle qu'il porte, et le brouillon mis de côté était effacé par la remise
    // à niveau de `applied_` juste en dessous. Le joueur voyait donc son travail
    // disparaître pour avoir fermé une fenêtre.
    if (!touched_) {
      // 🔴 Amorcer AVANT de charger : la recette porte la teinte de base, et
      // c'est elle qui décide sur quel fichier de palette les rampes sont
      // détectées.
      SeedFromShared();
      // Même si l'amorçage a échoué : la tête part de ce qui est PORTÉ, faute de
      // quoi la fenêtre s'ouvrirait sur une coupe que le joueur n'a pas.
      SeedWornHead();
      // La référence du brouillon se cale sur ce qu'on vient d'afficher. Sans
      // ça, l'écart de départ serait celui d'une recette vierge contre une tête
      // portée — donc non nul — et la fenêtre proposerait dès l'ouverture
      // suivante de « récupérer » un brouillon jamais composé. Un filet qui se
      // déclenche tout seul cesse d'être lu.
      applied_ = recipe_;
    }
    Reload();
  }
  // Rien à défaire à la fermeture : l'éditeur n'a jamais touché au personnage.
}

void PaletteEditor::OnRenderUI() {
  // Filet de sécurité, idempotent. La pose des détours appartient désormais à
  // `StyleSync`, qui la fait dès les écrans de LOGIN — les poser seulement ici
  // les mettait en place après l'apparition de l'acteur, et les couleurs
  // arrivaient avec une seconde de retard. Depuis CE fil dans les deux cas : les
  // fonctions détournées tournent sur le fil de rendu, elles ne peuvent donc pas
  // être en cours d'exécution pendant qu'on réécrit leurs cinq premiers octets.
  fx::palette_inject::EnsureInstalled();

  // ── Changement de personnage sans quitter le client ─────────────────────
  //
  // 🔴 AVANT le raccourci : sinon un Alt+P frappé la même frame ouvrirait la
  // fenêtre sur l'état de l'ancien personnage, et l'amorçage — qui ne se fait
  // qu'une fois — figerait sa recette pour toute la session.
  {
    const uint32_t cid = rag::OwnCharId();
    if (cid != 0 && cid != session_cid_) {
      if (session_cid_ != 0) ResetForNewCharacter();
      session_cid_ = cid;
    }
  }

  // 🔴 Le raccourci est lu AVANT tout retour anticipé — sinon il n'ouvrirait
  // jamais la fenêtre depuis l'état fermé. Et il ne se déclenche pas pendant
  // qu'on écrit : `NativeTextInputHasFocus` réplique la garde de
  // `UIWindowMgr_OnKeyDown`, sans quoi taper « palette » dans le chat ouvrirait
  // des fenêtres.
  const ImGuiIO& io = ImGui::GetIO();
  if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_P, false) &&
      !io.WantTextInput && !hotkeys::NativeTextInputHasFocus()) {
    Toggle();
  }

  // 🔴 Plus de débounce, parce qu'il n'y a plus rien à amortir : bouger un
  // curseur ne produit plus de bloc de palette, il ne change qu'un aperçu
  // dessiné à la frame suivante. Chaque application produisait un bloc DÉFINITIF
  // (son adresse sert de clé au cache d'atlas du client, donc jamais réutilisable)
  // et un glissement en fabriquait des dizaines. Une seule est posée maintenant :
  // celle de la validation.

  // Tenu à jour CHAQUE frame, et non dans `Toggle()` : la fenêtre se ferme aussi
  // par sa croix, qui ne passe pas par là. Tant qu'elle est ouverte, l'écho de
  // notre propre recette ne doit pas être reposé sur l'acteur — il écraserait le
  // réglage en cours.
  fx::style_sync::SetLocalEditing(open_);

  // L'aperçu d'un style reçu vit à part : il s'ouvre depuis le chat, et fermer
  // l'éditeur ne doit pas le fermer.
  DrawStylePreview();

  if (!open_) return;

  // ── Le corps a changé sous nos pieds ──────────────────────────────────────
  //
  // 🔴 On surveille le SPRITE, pas la classe. Un style de corps qu'on équipe ou
  // qu'on retire change le sprite SANS changer ni la classe ni le sexe : la
  // fenêtre restait alors sur l'ancien corps, et il fallait la fermer et la
  // rouvrir pour qu'elle suive. Même leçon qu'au chargement — c'est le sprite
  // RÉELLEMENT porté qui fait autorité, jamais ce qu'on en déduirait.
  //
  // Le test coûte la lecture d'une chaîne sur l'acteur, une fois par frame.
  {
    char spr[352];
    const bool lu = fx::palette_inject::ActorBodySpritePath(rag::OwnAccountId(), spr,
                                                            sizeof(spr));
    if ((lu && spr[0] != '\0' && body_path_ != spr) ||
        (!lu && (loaded_body_ != rag::OwnJobId() || loaded_sex_ != OwnSex()))) {
      // 🔴 La recette est CONSERVÉE, pas remise à zéro. Les rampes du nouveau
      // sprite se redécoupent, et les réglages s'y ré-appliquent PAR INDEX —
      // exactement ce que font les autres clients quand ce joueur change de
      // corps (cf. l'en-tête de style_sync : la règle doit être la même partout,
      // sinon il se verrait autrement qu'on le voit).
      Reload();
    }
  }

  // Rattrapage d'amorçage : l'éditeur a pu s'ouvrir avant que nos couleurs ne
  // soient connues — au login, l'acteur n'est pas monté et le serveur n'a rien
  // renvoyé. On réessaie donc, et le succès impose un rechargement COMPLET : la
  // recette amorcée porte une teinte de base, donc une autre palette sous les
  // rampes.
  if (!seeded_ && SeedFromShared()) Reload();

  // Le brouillon est mis à l'abri PENDANT l'édition, pas à la fermeture : ce
  // dont il protège — plantage, coupure, client tué — ne passe pas par une
  // fermeture propre.
  TickDraft();

  if (need_pos_) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f,
                                   io.DisplaySize.y * 0.35f),
                            ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }

  // 🔴 Le `###` vit dans le GABARIT, hors de la traduction : c'est lui qui donne
  // à la fenêtre son identité stable dans imgui.ini. Un catalogue qui le
  // perdrait en traduisant ferait renaître la fenêtre ailleurs.
  char title[128];
  std::snprintf(title, sizeof(title), "%s###palette_editor",
                i18n::Tr("Style du personnage"));

  // 🔴 Taille FIXE — mais MESURÉE, jamais codée en dur : `AlwaysAutoResize` fait
  // suivre la fenêtre à son contenu. Une largeur en dur serait fausse dès qu'on
  // change de langue ou de taille de police, qui sont l'une et l'autre des
  // réglages du joueur ; ici, plus rien ne se rogne et plus rien ne se
  // redimensionne à la main.
  //
  // `NoCollapse` retire le bouton « minimiser » (le skin le masque sur ce flag,
  // cf. `show_mini` dans ro_imgui.cc) : replier une fenêtre qu'on ne peut plus
  // redimensionner n'apporte rien, et la croix suffit à s'en débarrasser.
  //
  // EndRoWindow doit TOUJOURS être appelé, même quand Begin rend false.
  if (ro::BeginRoWindow(title, &open_,
                        ImGuiWindowFlags_AlwaysAutoResize |
                            ImGuiWindowFlags_NoCollapse)) {
    // 🔴 Repli de texte à une largeur DÉRIVÉE DE LA POLICE, jamais `TextWrapped`.
    //
    // `TextWrapped` calcule sa position de repli depuis la largeur de la
    // FENÊTRE. Dans une fenêtre auto-dimensionnée, cette largeur n'existe pas
    // encore à la passe de mesure : le paragraphe est donc mesuré d'un seul
    // tenant, la fenêtre s'ajuste à la ligne entière, puis se replie à la frame
    // suivante — une ouverture pleine largeur avant correction, à chaque fois.
    //
    // Une largeur en pixels serait fausse dès qu'on change de taille de police,
    // qui est un réglage du joueur. `GetFontSize()` suit ce réglage tout seul —
    // même idiome que `mui::HelpMarker`.
    const float wrap = ImGui::GetCursorPosX() + ImGui::GetFontSize() * 24.0f;
    ImGui::PushTextWrapPos(wrap);
    if (!error_.empty()) {
      ImGui::TextUnformatted(error_.c_str());
      ImGui::PopTextWrapPos();
      if (ro::RoButton(i18n::Tr("Réessayer"))) Reload();
    } else {
      ImGui::TextUnformatted(i18n::Tr("Choisissez une pièce, puis réglez sa couleur."));
      ImGui::PopTextWrapPos();

      // ── Ce que les curseurs atteignent VRAIMENT ───────────────────────────
      // Les huit pièces sont les huit plus grands dégradés du corps ; tout le
      // reste — petits détails, contours noirs — n'a pas de curseur et n'en
      // aura pas. Le taire laisserait le joueur chercher indéfiniment la pièce
      // qui commande une zone que rien ne commande. C'est exactement la
      // question qu'on nous a posée.
      if (pixels_total_ > 0) {
        char diag[128];
        std::snprintf(diag, sizeof(diag), i18n::Tr("Réglable : %d%% du corps"),
                      static_cast<int>(100.0 * pixels_covered_ / pixels_total_ +
                                       0.5));
        ImGui::TextUnformatted(diag);
        if (ImGui::IsItemHovered()) {
          ImGui::BeginTooltip();
          // 🔴 Le CHEMIN du sprite n'est pas affiché, et ne peut pas l'être :
          // les dossiers de sprites sont en coréen CP949, alors que la police
          // de l'interface ne porte que 0x20-0xFF. Il ne s'afficherait qu'en
          // rectangles vides. Ce qui est réellement utile — savoir si le sprite
          // a été LU ou DEVINÉ — tient de toute façon en une ligne lisible.
          ImGui::TextUnformatted(
              resolved_from_actor_
                  ? i18n::Tr("Sprite lu sur le personnage à l'écran.")
                  : i18n::Tr("Sprite déduit de la classe (moins fiable)."));
          ImGui::EndTooltip();
        }
      }

      // ── À QUEL CORPS ce réglage appartient ────────────────────────────────
      //
      // 🔴 Depuis la v7, un joueur habille chaque corps séparément : le sien, sa
      // monture, ses costumes. Sans cette ligne, rien à l'écran ne distingue
      // « je règle ma monture » de « je m'apprête à réécrire le style de mon
      // personnage à pied » — deux gestes identiques aux conséquences opposées.
      //
      // ⚠ Le corps ne peut pas être NOMMÉ : son nom est le chemin de son sprite,
      // en coréen CP949, que la police de l'interface (0x20-0xFF) rendrait en
      // rectangles vides. On dit donc ce qui se décide, pas ce qui se porte.
      if (body_key_ != 0) {
        const bool sien = fx::style_sync::LocalHasVariant(body_key_);
        const int total = fx::style_sync::LocalVariantCount();
        if (sien) {
          char etat[160];
          std::snprintf(etat, sizeof(etat),
                        i18n::Tr("Ce corps a son propre style (%d au total)."),
                        total);
          ImGui::TextUnformatted(etat);
        } else if (total > 0) {
          ImGui::TextUnformatted(
              i18n::Tr("Ce corps reprend ton style principal."));
        }
        if (ImGui::IsItemHovered() && (sien || total > 0)) {
          ImGui::BeginTooltip();
          ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
          ImGui::TextUnformatted(
              sien ? i18n::Tr("Les couleurs d'un corps ne veulent rien dire sur "
                              "un autre : chaque monture, chaque costume a donc "
                              "son propre style. Valider ne change que celui-ci.")
                   : i18n::Tr("Tu n'as pas encore habillé ce corps : il reprend "
                              "ton style principal, au plus proche. Valide pour "
                              "lui donner le sien, sans toucher aux autres."));
          ImGui::PopTextWrapPos();
          ImGui::EndTooltip();
        }
      }
      ImGui::Separator();

      // La palette telle qu'elle sera rendue — elle sert aux pastilles, donc
      // ce que le joueur voit dans la fenêtre est ce qu'il voit sur lui.
      uint8_t tinted[1024];
      if (!ro::ApplyRecipe(base_.data(), base_.size(), ramps_, ramp_count_,
                           recipe_, tinted, sizeof(tinted)))
        std::memcpy(tinted, base_.data(), 1024);

      // ── Les pièces : chacune porte SA pastille, cliquable ────────────────
      // Un clic sur la pastille ouvre directement le sélecteur de cette pièce,
      // sans passer par une seconde pastille commune. Le nom, à côté, choisit
      // la pièce que les curseurs ajustent.
      // Un nuancier ouvert RECOUVRE tout ce qui suit dans la fenêtre. On le note
      // pour neutraliser ces réglages-là plus bas : sans quoi viser le nuancier
      // revient à attraper la poignée d'un curseur resté dessous.
      bool picker_open = false;

      // 🔴 Le pantin à GAUCHE, les pièces à DROITE — et pas l'inverse, qui était
      // la première disposition. Un nuancier s'ouvre SOUS sa pastille : avec les
      // pièces à gauche, il retombait pile sur le pantin et masquait ce qu'on
      // cherchait justement à regarder en choisissant une couleur. À droite, il
      // déborde hors de la fenêtre, où il ne cache rien.
      //
      // La hauteur se CALCULE au lieu de se mesurer après coup : chaque ligne
      // vaut une hauteur de cadre (la pastille), et il faut la connaître AVANT de
      // dessiner le pantin puisqu'il vient en premier. Tout est dérivé des
      // métriques de la police — rien en pixels.
      int survolee = -1;  // pièce sous le curseur : ses pixels pulseront
      const float ligne_h = ImGui::GetFrameHeight();
      const float liste_h =
          ramp_count_ > 0
              ? ramp_count_ * ligne_h +
                    (ramp_count_ - 1) * ImGui::GetStyle().ItemSpacing.y
              : 0.0f;
      // Le pantin est dessiné d'abord mais la pièce survolée n'est connue qu'à la
      // fin de la liste : il utilise donc celle de la frame PRÉCÉDENTE. Un
      // décalage d'une image sur une pulsation qui bat deux fois par seconde ne
      // se voit pas.
      if (liste_h > 1.0f) {
        DrawPreviewDoll(liste_h, survolee_);
        ImGui::SameLine();
      }

      ImGui::BeginGroup();
      for (int i = 0; i < ramp_count_; ++i) {
        ImGui::PushID(i);
        const uint32_t now = ro::RampColor(tinted, sizeof(tinted), ramps_[i]);
        float rgb[4] = {((now >> 16) & 0xFF) / 255.0f,
                        ((now >> 8) & 0xFF) / 255.0f,
                        (now & 0xFF) / 255.0f, 1.0f};
        bool this_open = false;
        // `##` : la pastille sans libellé. Pas d'alpha : l'octet de
        // transparence d'une entrée de palette ne veut rien dire — `ApplyRecipe`
        // le laisse INTACT, et seul l'index 0 est transparent. Un curseur qui ne
        // commande rien inviterait à le bouger pour rien.
        //
        // 🔴 Et RIEN QUE le nuancier : ni R/G/B, ni T/S/V, ni hexadécimal. Ces
        // chiffres décrivent l'index REPRÉSENTATIF de la pièce, pas la pièce —
        // le reste du dégradé garde son modelé et ne vaut pas cette valeur. Les
        // afficher laisse croire qu'on impose une couleur exacte, alors que la
        // luminosité reste relative et qu'elle plafonne à ce que la rampe
        // permet. « #DC9084 » sur un pantalon ne se compare à rien et ne se
        // retient pas : trois rangées de chiffres pour une commande qui n'existe
        // pas.
        if (RoColorSwatch("##sw", rgb, &this_open, /*with_alpha=*/false,
                          /*numeric_inputs=*/true)) {
          const uint32_t target =
              (static_cast<uint32_t>(rgb[0] * 255.0f + 0.5f) << 16) |
              (static_cast<uint32_t>(rgb[1] * 255.0f + 0.5f) << 8) |
              static_cast<uint32_t>(rgb[2] * 255.0f + 0.5f);
          // 🔴 Depuis la palette de BASE, jamais depuis la teintée : partir du
          // résultat courant ferait s'additionner les réglages à chaque clic,
          // et la couleur dériverait.
          recipe_.ramps[i] = ro::AdjustToReach(base_.data(), base_.size(),
                                               ramps_[i], target);
          touched_ = true;
        }
        picker_open = picker_open || this_open;
        if (ImGui::IsItemHovered()) survolee = i;
        ImGui::SameLine();

        // Le nom, aligné sur le milieu de la pastille. Plus de `Selectable` :
        // sélectionner une pièce ne servait qu'au curseur de teinte, qui n'existe
        // plus — le nuancier fait tout, et une sélection sans effet n'invite qu'à
        // chercher ce qu'elle commande.
        // ── Une pièce peinte SOMBRE ne pourra pas être éclaircie ────────────
        //
        // 🔴 Et il faut le dire, sinon la commande a l'air cassée : désigner un
        // rouge vif sur une zone d'ombre rend un rouge sombre, la valeur
        // « remonte puis redescend », et rien n'explique pourquoi. C'est
        // pourtant le sprite qui parle — l'entrejambe, le dessous des bras, la
        // nuque sont peints sombres par l'artiste, et les réglages conservent ce
        // modelé au lieu de l'aplatir.
        //
        // Seuil à 224 sur 255 (≈ 88 %) : au-dessus, le plafond ne se remarque
        // pas. Mesuré sur les 421 corps, il désigne environ une pièce sur six —
        // assez rare pour que la mention veuille dire quelque chose, assez
        // fréquent pour couvrir les cas qu'on nous a signalés.
        const int plafond =
            ro::RampValueCeiling(base_.data(), base_.size(), ramps_[i]);
        const bool a_lombre = plafond < 224;

        char label[64];
        if (a_lombre)
          std::snprintf(label, sizeof(label), "%s %d %s", i18n::Tr("Pièce"),
                        i + 1, i18n::Tr("(ombre)"));
        else
          std::snprintf(label, sizeof(label), "%s %d", i18n::Tr("Pièce"), i + 1);
        // 🔴 `AlignTextToFramePadding`, et surtout PAS un `SetCursorPosY` à la
        // main. Repositionner le curseur en Y après le texte laisse le X là où il
        // est et annule l'avance de ligne : les huit pièces se dessinaient alors
        // TOUTES au même endroit, empilées, et la fenêtre semblait n'en avoir
        // qu'une. C'est l'idiome ImGui prévu pour aligner un libellé sur la
        // hauteur d'un cadre.
        ImGui::AlignTextToFramePadding();
        // La pièce que la PIPETTE désigne s'allume : c'est la réponse à
        // « quelle ligne commande cette manche ? », et elle doit sauter aux yeux
        // sans qu'on quitte le pantin du regard.
        if (i == pipette_)
          ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.43f, 1.0f), "%s", label);
        else
          ImGui::TextUnformatted(label);
        // Survoler le NOM comme la pastille désigne la pièce : les deux sont sur
        // la même ligne, et viser l'un ou l'autre veut dire la même chose.
        if (ImGui::IsItemHovered()) {
          survolee = i;
          // L'explication ne s'affiche QUE pour les pièces concernées. Une
          // infobulle qui dirait « pas de limite » sur les cinq autres noierait
          // celle qui compte.
          if (a_lombre) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
            char t[224];
            std::snprintf(
                t, sizeof(t),
                i18n::Tr("Zone d'ombre du sprite : la clarté de cette pièce "
                         "plafonne à %d %%. Les réglages gardent le modelé du "
                         "dégradé, donc une pièce peinte sombre le reste — tu "
                         "peux changer sa teinte, pas l'éclairer."),
                (plafond * 100) / 255);
            ImGui::TextUnformatted(t);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
          }
        }

        // ── Remise à zéro, PAR PIÈCE ──────────────────────────────────────
        //
        // 🔴 Le bouton n'apparaît que si la pièce a été RETOUCHÉE. C'est ce qui
        // le rend lisible sans icône dédiée : une croix posée sur toutes les
        // lignes se lirait « supprimer la pièce », alors qu'à côté d'une pièce
        // visiblement modifiée elle ne peut vouloir dire qu'« annuler ça ».
        // Elle montre du même coup, d'un coup d'œil, ce qu'on a touché.
        //
        // « x » et non une flèche de retour : la police de l'interface ne couvre
        // que 0x20-0xFF, une icône Unicode n'y serait qu'un rectangle vide.
        if (!recipe_.ramps[i].IsNeutral()) {
          ImGui::SameLine();
          if (ro::RoSmallButton("x")) {
            recipe_.ramps[i] = ro::RampAdjust();
            touched_ = true;
          }
          if (ImGui::IsItemHovered()) {
            survolee = i;
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(i18n::Tr("Réinitialiser cette pièce"));
            ImGui::EndTooltip();
          }
        }
        ImGui::PopID();
      }
      ImGui::EndGroup();
      // Pour la pulsation du pantin, dessiné AVANT la liste : la ligne survolée
      // si le curseur est dans la liste, sinon la pièce que la pipette désigne
      // sur le pantin. Les deux gestes posent la même question par les deux
      // bouts, et allument donc la même chose.
      survolee_ = survolee >= 0 ? survolee : pipette_;

      ImGui::Separator();

      // 🔴 Tout ce qui suit passe SOUS le nuancier quand il est ouvert. On le
      // neutralise plutôt que de le déplacer hors de la fenêtre : un sélecteur
      // qui surgit ailleurs qu'à côté de sa pastille est déroutant, alors qu'un
      // réglage grisé pendant qu'on choisit une couleur se comprend tout seul.
      // (Le bouton « Fermer » du nuancier, lui, est dans le popup : on ne peut
      // pas se retrouver coincé.)
      if (picker_open) ImGui::BeginDisabled();

      // ── La palette de vêtement de départ ─────────────────────────────────
      // 🔴 Elle change la BASE, donc les rampes détectées : les pièces peuvent
      // se redécouper et se reclasser. Les réglages en place se ré-appliquent
      // alors par index, comme lors d'un changement de sprite. C'est pour ça que
      // le numéro voyage DANS la recette — sinon les autres joueurs
      // recalculeraient une base différente et ne verraient pas les mêmes
      // couleurs.
      // ── Les trois sélecteurs, en boutons ─────────────────────────────────
      //
      // 🔴 Des grilles de VIGNETTES, plus des curseurs. « Coiffure 47 » ne veut
      // rien dire : on choisit une coupe en la voyant, comme à la création du
      // personnage. Un curseur obligeait à parcourir 80 valeurs une par une
      // pour découvrir ce qu'elles étaient.
      //
      // Et en POPUP, pas dans la fenêtre : trois grilles de 80, 251 et 553 cases
      // y tiendraient mal, et l'éditeur doit rester lisible quand on ne choisit
      // rien. Le libellé du bouton porte le choix courant — c'est ce qui remplace
      // la valeur qu'affichait le curseur.
      {
        char b[96];
        std::snprintf(b, sizeof(b), "%s (%d)", i18n::Tr("Coiffure"),
                      recipe_.hair_style > 0 ? recipe_.hair_style
                                             : *reinterpret_cast<int*>(rag::kOwnHairStyleAddr));
        if (ro::RoButton(b, ro::ButtonWidth(b)))
          ImGui::OpenPopup("##coiffures");
        DrawHairStylePicker();

        const int hc = recipe_.hair_palette_id > 0 ? recipe_.hair_palette_id : 0;
        if (hc == 0)
          std::snprintf(b, sizeof(b), "%s (%s)", i18n::Tr("Couleur de cheveux"),
                        i18n::Tr("d'origine"));
        else
          std::snprintf(b, sizeof(b), "%s (%d)", i18n::Tr("Couleur de cheveux"),
                        hc);
        if (ro::RoButton(b, ro::ButtonWidth(b)))
          ImGui::OpenPopup("##couleurs_cheveux");
        DrawHairColorPicker();

        // 🔴 Le libellé porte les NUMÉROS DU SERVEUR, 1 à 553. Le joueur et
        // l'administrateur doivent parler de la même palette : « palette 42 »
        // ici doit être `body_42.pal` là-bas. Et « d'origine » n'est pas la
        // palette 0 — celle-ci n'existe pas pour le jeu, une couleur de vêtement
        // nulle signifiant « aucune palette externe » ; la sentinelle reste -1.
        const int pid = recipe_.palette_id > 0 ? recipe_.palette_id : 0;
        if (pid == 0)
          std::snprintf(b, sizeof(b), "%s (%s)", i18n::Tr("Teinte de base"),
                        i18n::Tr("d'origine"));
        else
          std::snprintf(b, sizeof(b), "%s (%d)", i18n::Tr("Teinte de base"),
                        pid);
        if (ro::RoButton(b, ro::ButtonWidth(b)))
          ImGui::OpenPopup("##palettes_corps");
        if (ImGui::IsItemHovered()) {
          ImGui::BeginTooltip();
          ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
          ImGui::TextUnformatted(
              i18n::Tr("Les couleurs officielles du styliste servent de point "
                       "de départ ; tes réglages s'appliquent par-dessus. "
                       "Changer de teinte de base peut redécouper les pièces."));
          ImGui::PopTextWrapPos();
          ImGui::EndTooltip();
        }
        DrawBodyPalettePicker();
      }
      ImGui::Separator();
      // ⛔ Plus de curseur de teinte par pièce. Il ne restait que pour ce que le
      // nuancier ne sait pas exprimer — faire pivoter une teinte en gardant la
      // MATIÈRE du dégradé — mais il doublait une commande déjà présente et
      // demandait de « sélectionner » une pièce avant d'agir. Le nuancier suffit,
      // et tout réglage est désormais une couleur imposée.
      // ── TROIS retours en arrière, et ils ne se recouvrent pas ────────────
      //
      // 🔴 La confusion entre eux a été signalée deux fois, et la deuxième a
      // montré qu'il en MANQUAIT un. Ils répondent à trois questions distinctes
      // que le joueur se pose vraiment :
      //   « annule mes retouches »  -> Revenir à mon style   (ce qu'il PORTE)
      //   « donne-moi une page blanche » -> Repartir de zéro (état CANONIQUE)
      //   « enlève tout ça »        -> Supprimer mon style   (serveur COMPRIS)
      // Le premier manquait, et « Tout réinitialiser » en tenait lieu de
      // travers : il rendait une page blanche là où on demandait un retour en
      // arrière. D'où son nouveau nom, qui dit ce qu'il fait au lieu de le
      // suggérer.
      const bool a_des_retouches =
          fx::palette_cache::EncodeShare(recipe_) !=
          fx::palette_cache::EncodeShare(applied_);
      if (!a_des_retouches) ImGui::BeginDisabled();
      if (ro::RoButton(i18n::Tr("Revenir à mon style"))) {
        // 🔴 Le brouillon est mis à l'abri AVANT, et sans attendre le délai
        // d'inactivité : ce bouton jette exactement ce qu'il protège. Sans ça,
        // un clic malheureux perdrait une demi-heure de réglages sans retour
        // possible — alors que « Dernier style non validé » est juste à côté.
        if (rag::OwnCharId() != 0)
          fx::palette_cache::DraftSave(rag::OwnCharId(), &recipe_);
        draft_tick_ = 0.0;
        recipe_ = applied_;
        touched_ = true;  // geste explicite (cf. SeedFromShared)
        // 🔴 RECHARGER : la teinte de base peut différer, donc la fusion et le
        // découpage des rampes aussi.
        Reload();
      }
      if (!a_des_retouches) ImGui::EndDisabled();
      if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(
            a_des_retouches
                ? i18n::Tr("Annule tes retouches en cours et revient à ce que "
                           "ton personnage porte. Ton travail reste "
                           "récupérable par « Dernier style non validé ».")
                : i18n::Tr("Rien à annuler : l'aperçu montre déjà ce que ton "
                           "personnage porte."));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }

      // ⚠ UNE SEULE chaîne pour l'ouverture ET pour le Begin : ImGui apparie les
      // popups par l'identifiant qui suit `###`, et l'échec est SILENCIEUX. C'est
      // aussi ce `###` qui rend l'identité INSENSIBLE à la langue — sans lui,
      // changer de langue entre l'ouverture et le rendu perdrait le popup.
      const char* kResetPopup  = i18n::Tr("Repartir de zéro###bourgeon_style_reset");
      const char* kDeletePopup = i18n::Tr("Supprimer mon style###bourgeon_style_delete");
      const char* kForgetPopup =
          i18n::Tr("Oublier ce corps###bourgeon_style_forget_body");

      if (ro::RoButton(i18n::Tr("Repartir de zéro")))
        ImGui::OpenPopup(kResetPopup);
      if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(
            i18n::Tr("Page blanche : les huit pièces, la teinte de base, la "
                     "couleur de cheveux et la première coiffure. Ce n'est PAS "
                     "un retour à ce que tu portes. Ton personnage ne change "
                     "qu'à la validation."));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }

      // Le troisième EFFACE, ici et sur le serveur — d'où un libellé qui le dit.
      if (ro::RoButton(i18n::Tr("Supprimer mon style")))
        ImGui::OpenPopup(kDeletePopup);
      if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(
            i18n::Tr("Efface ta palette, chez toi ET sur le serveur. Ton "
                     "personnage retrouve l'apparence qu'il avait avant, pour "
                     "toi comme pour les autres. Ce n'est PAS un retour à ta "
                     "dernière couleur partagée : celle-ci est perdue."));
        ImGui::EndTooltip();
      }

      // Et le geste FIN : n'oublier que le corps porté. Proposé seulement quand
      // il en reste un autre — sinon il ferait doublon avec le bouton ci-dessus,
      // avec un libellé plus obscur.
      if (fx::style_sync::LocalHasVariant(body_key_) &&
          fx::style_sync::LocalVariantCount() > 1) {
        if (ro::RoButton(i18n::Tr("Oublier le style de ce corps")))
          ImGui::OpenPopup(kForgetPopup);
        if (ImGui::IsItemHovered()) {
          ImGui::BeginTooltip();
          ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
          ImGui::TextUnformatted(
              i18n::Tr("Efface le style de CE corps seulement. Tes autres corps "
                       "gardent le leur, et celui-ci reprend ton style "
                       "principal."));
          ImGui::PopTextWrapPos();
          ImGui::EndTooltip();
        }
      }

      // ── Les confirmations ────────────────────────────────────────────────
      //
      // 🔴 Des popups ImGui, JAMAIS la modale native (`UIWndMgr_ShowMessageBox`)
      // : celle-ci ne rend pas la main, elle boucle en relançant le tick et le
      // rendu du mode courant — appelée d'ici, c'est-à-dire entre `NewFrame` et
      // `Render`, elle fige le client sans crash ni message.
      //
      // Elles gardent les deux gestes DESTRUCTIFS et eux seuls : « tout
      // réinitialiser » jette un réglage qui a pu demander du temps, et
      // « supprimer » efface jusque sur le serveur, sans retour possible.
      // 🔴 Cadre RO, pas un `BeginPopupModal` nu : skinner les boutons ne
      // suffisait pas, il manquait le contenant — un corps sombre ImGui au milieu
      // d'une fenêtre RO claire. ImGui garde la modalité et le voile ; le cadre
      // centre lui-même et pousse les couleurs RO pour le contenu.
      // ⚠ `EndRoPopupModal` UNIQUEMENT si le Begin a rendu true (règle EndPopup).
      if (ro::BeginRoPopupModal(kResetPopup)) {
        // Sinon un Échap fermerait À LA FOIS la confirmation et l'éditeur derrière.
        ro::SuppressEscapeStack();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 20.0f);
        ImGui::TextUnformatted(
            i18n::Tr("Remettre tout l'aperçu à zéro ? Tes réglages en cours "
                     "seront perdus."));
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        if (ro::RoButton(i18n::Tr("Réinitialiser"))) {
          // 🔴 Le constructeur par défaut, JAMAIS `memset` : les sentinelles de
          // cette structure valent -1 (« rien d'imposé »), pas 0.
          recipe_ = ro::PaletteRecipe();
          // 🔴 La coiffure retombe sur la PREMIÈRE, pas sur celle que le
          // personnage porte. « Tout réinitialiser » doit rendre un état
          // canonique — le même quel que soit le point de départ — sinon le
          // bouton ne fait visiblement rien sur cette ligne-là, et le joueur
          // croit qu'elle lui échappe. Les palettes, elles, retombent bien sur
          // « d'origine » (-1) par le constructeur ci-dessus.
          //
          // ⚠ Sans conséquence tant qu'on ne valide pas : le personnage ne
          // change qu'à la validation, et fermer la fenêtre n'engage rien.
          recipe_.hair_style = 1;
          // 🔴 Le drapeau de geste EXPLICITE, et il manquait depuis le début.
          //
          // Il dit « le joueur a demandé le vide, c'est un choix » — sans quoi
          // rien ne distingue cet état de « la fenêtre n'a pas encore été
          // amorcée », et tout ce qui comble un vide se croit autorisé à le
          // défaire : l'amorçage depuis le serveur, et la remise à niveau de la
          // tête portée à la réouverture. Le commentaire de `SeedFromShared` a
          // toujours décrit ce drapeau comme posé ici ; il ne l'était pas.
          touched_ = true;
          // 🔴 RECHARGER : la teinte de base vient de changer, donc la base
          // fusionnée et le découpage des rampes aussi.
          Reload();
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ro::RoButton(i18n::Tr("Annuler"))) ImGui::CloseCurrentPopup();
        ro::EndRoPopupModal();
      }

      if (ro::BeginRoPopupModal(kDeletePopup)) {
        ro::SuppressEscapeStack();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 20.0f);
        ImGui::TextUnformatted(
            i18n::Tr("Supprimer ton style, chez toi ET sur le serveur ? Ton "
                     "personnage retrouve son apparence d'avant, pour toi comme "
                     "pour les autres. C'est définitif."));
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        if (ro::RoButton(i18n::Tr("Supprimer"))) {
          RestoreServerColors();
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ro::RoButton(i18n::Tr("Annuler"))) ImGui::CloseCurrentPopup();
        ro::EndRoPopupModal();
      }

      if (ro::BeginRoPopupModal(kForgetPopup)) {
        ro::SuppressEscapeStack();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 20.0f);
        ImGui::TextUnformatted(
            i18n::Tr("Oublier le style de ce corps ? Il reprendra ton style "
                     "principal, chez toi comme chez les autres. Tes autres "
                     "corps ne changent pas."));
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        if (ro::RoButton(i18n::Tr("Oublier"))) {
          ForgetCurrentBodyStyle();
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ro::RoButton(i18n::Tr("Annuler"))) ImGui::CloseCurrentPopup();
        ro::EndRoPopupModal();
      }

      // ── LA VALIDATION ────────────────────────────────────────────────────
      //
      // 🔴 Le seul geste de cette fenêtre qui touche au personnage. Tout ce qui
      // précède ne vit que dans le pantin : on peut tout dérégler, essayer
      // vingt coiffures et fermer sans que rien n'ait bougé.
      //
      // C'est aussi ce qui protège le serveur et les autres joueurs. Envoyer à
      // chaque mouvement de curseur produirait des dizaines d'états par seconde
      // dont personne ne veut, et ferait clignoter le personnage chez les autres
      // pendant qu'on cherche sa couleur.
      ImGui::Separator();
      if (ro::RoButton(i18n::Tr("Valider et partager mon style"))) {
        // 1. Chez soi, tout de suite : sans ça, le joueur validerait et ne
        //    verrait rien changer jusqu'à l'écho du serveur.
        Apply();
        // 2. Au serveur, en UN seul envoi : la recette porte tout le style,
        //    coiffure comprise. ⚠ Le serveur ne se contente pas de la ranger —
        //    il APPLIQUE la coiffure (`pc_changelook`), qui devient un vrai
        //    changement de personnage, sauvegardé et vu de tous.
        fx::style_sync::SendRecipe(recipe_, body_key_);
        shared_tick_ = ImGui::GetTime();
      }
      if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(
            i18n::Tr("Applique ce style à ton personnage — jusqu'ici, rien n'a "
                     "changé hors de cet aperçu. Les autres joueurs te verront "
                     "ainsi, et ta coiffure change pour de bon."));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
      // Accusé purement local : le serveur ne répond rien, et lui inventer une
      // confirmation qu'on n'a pas serait mentir. On dit ce qu'on sait — que le
      // paquet est parti.
      if (shared_tick_ > 0.0 && ImGui::GetTime() - shared_tick_ < 4.0) {
        ImGui::SameLine();
        ImGui::TextUnformatted(i18n::Tr("Envoyé."));
      }

      // ── Préréglages ──────────────────────────────────────────────────────
      // Purement LOCAUX : le serveur n'en sait rien et n'a pas à en savoir. Un
      // préréglage n'est pas une apparence, c'est un brouillon qu'on reprend.
      ImGui::Separator();
      // 🔴 La section s'ouvre d'elle-même quand il y a un brouillon à récupérer,
      // et une seule fois. C'est le seul moment où quelqu'un a besoin de la
      // trouver sans savoir qu'elle existe : il vient de perdre une session, et
      // un bouton de sauvetage replié dans un menu ne sauve personne. Le reste du
      // temps la section reste comme le joueur l'a laissée.
      const bool a_brouillon = fx::palette_cache::HasDraft(rag::OwnCharId());
      if (a_brouillon) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
      if (ImGui::TreeNode(i18n::Tr("Préréglages"))) {
        // ── Le dernier style non validé ────────────────────────────────────
        if (a_brouillon) {
          if (ro::RoButton(i18n::Tr("Dernier style non validé"))) {
            ro::PaletteRecipe brouillon;
            if (fx::palette_cache::DraftLoad(rag::OwnCharId(), &brouillon)) {
              recipe_ = brouillon;
              touched_ = true;  // même raison que pour un préréglage
              // 🔴 Rechargement COMPLET : le brouillon porte sa propre teinte de
              // base, donc ni la fusion ni les rampes ne sont les mêmes.
              Reload();
            }
          }
          if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
            ImGui::TextUnformatted(
                i18n::Tr("Ce que tu composais la dernière fois sans le valider. "
                         "Gardé sur cet ordinateur, pour ce personnage."));
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
          }
          ImGui::Separator();
        }
        std::string noms[fx::palette_cache::kMaxPresets];
        const int n = fx::palette_cache::PresetNames(
            noms, fx::palette_cache::kMaxPresets);
        for (int i = 0; i < n; ++i) {
          ImGui::PushID(i);
          // 🔴 La croix est soumise AVANT le nom, et ce n'est pas un détail de
          // mise en page : un `Selectable` occupe toute la largeur restante et
          // avalait le clic sur toute la ligne, croix comprise. Soumettre le
          // bouton d'abord lui réserve son rectangle ; le `Selectable` prend ce
          // qui reste.
          //
          // Centrage vertical : un bouton a la hauteur d'un cadre, un
          // `Selectable` celle d'une ligne de texte. On descend le plus court de
          // la moitié de l'écart, sinon la croix flotte au-dessus du nom.
          const float ligne = ImGui::GetTextLineHeight();
          const float cadre = ImGui::GetFrameHeight();
          const float y = ImGui::GetCursorPosY();
          if (cadre < ligne) ImGui::SetCursorPosY(y + (ligne - cadre) * 0.5f);
          // « × » (U+00D7) : la police de l'interface couvre 0x20-0xFF, ce
          // caractère en fait donc partie — contrairement à un vrai signe
          // multiplication typographique ou à une croix d'icône.
          const bool supprime = ro::RoSmallButton("x");
          ImGui::SetCursorPosY(y);
          if (supprime) {
            fx::palette_cache::PresetDelete(noms[i]);
            ImGui::PopID();
            break;  // la liste vient de bouger sous nos pieds
          }
          ImGui::SameLine();
          if (cadre > ligne) ImGui::SetCursorPosY(y + (cadre - ligne) * 0.5f);
          // Largeur explicite : même piège d'auto-dimensionnement que plus haut.
          if (ImGui::Selectable(noms[i].c_str(), false, 0,
                                ImVec2(ImGui::GetFontSize() * 9.0f, 0.0f))) {
            // 🔴 Maj+clic = poser le lien dans le chat. C'est la convention de
            // gestes du projet, la même que sur un objet ou une compétence, et
            // c'est justement ce qui la rend utile : le joueur n'a rien de neuf
            // à apprendre. Le geste s'arrête au dépôt dans la barre de saisie —
            // c'est lui qui envoie.
            if (ImGui::GetIO().KeyShift) {
              ro::PaletteRecipe partage;
              if (fx::palette_cache::PresetLoad(noms[i], &partage)) {
                if (auto* chat = Bourgeon::Instance().chat_window()) {
                  const std::string code =
                      fx::palette_cache::EncodeShare(partage);
                  // 🔴 Le NOM DU PRÉRÉGLAGE comme étiquette, pas notre pseudo.
                  // Celui-ci est déjà en tête de la ligne de chat : le répéter
                  // dans le lien n'apprend rien, et empêche surtout de
                  // distinguer trois préréglages postés à la suite. L'étiquette
                  // doit porter ce que le lecteur ne sait pas encore.
                  chat->AppendStyleLink(code.c_str(), noms[i].c_str());
                }
              }
            } else if (fx::palette_cache::PresetLoad(noms[i], &recipe_)) {
              // Geste explicite : plus de rattrapage d'amorçage par-dessus, même
              // si le préréglage se trouve être neutre (cf. `SeedFromShared`).
              touched_ = true;
              // Le nom revient dans le champ de saisie : charger puis retoucher
              // puis réenregistrer sous le même nom est le geste le plus courant,
              // et le retaper à chaque fois n'apporte rien.
              lstrcpynA(preset_name_, noms[i].c_str(), sizeof(preset_name_));
              // 🔴 Et on RESYNCHRONISE le widget. Écrire dans le tampon ne suffit
              // pas si le champ a le focus : ImGui garde une copie interne et la
              // réécrit par-dessus la nôtre à la frame suivante. Le joueur
              // verrait alors l'ancien texte, et « Enregistrer » créerait un
              // doublon au lieu de mettre à jour.
              if (preset_field_id_ != 0) {
                if (ImGuiInputTextState* st =
                        ImGui::GetInputTextState(preset_field_id_))
                  st->ReloadUserBufAndMoveToEnd();
              }
              // 🔴 Rechargement COMPLET : un préréglage porte sa propre teinte
              // de base, donc la fusion et les rampes ne sont plus les mêmes.
              Reload();
            }
          }
          if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(
                i18n::Tr("Clic : charger. Maj+clic : lien dans le chat."));
            ImGui::EndTooltip();
          }
          ImGui::PopID();
        }

        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        ImGui::InputText("##nom", preset_name_, sizeof(preset_name_));
        // Relevé APRÈS soumission : c'est le seul moment où l'identifiant du
        // widget est connu. Il servira à la frame suivante.
        preset_field_id_ = ImGui::GetItemID();
        ImGui::SameLine();
        if (ro::RoButton(i18n::Tr("Enregistrer"))) {
          // Le nom est CONSERVÉ après l'enregistrement : on vient peut-être de
          // mettre à jour un préréglage existant, et on peut vouloir enchaîner.
          // 🔴 La recette PARTAGEABLE : un préréglage se recharge des semaines
          // plus tard, éventuellement sur un autre personnage. Y laisser
          // « couleur d'origine » lui ferait prendre celle du moment, et le
          // joueur ne retrouverait pas l'allure qu'il croyait avoir rangée.
          fx::palette_cache::PresetSave(preset_name_, ShareableRecipe());
        }

        // ── Code partageable ────────────────────────────────────────────
        // La même chaîne que celle rangée en base : un joueur la copie et la
        // donne à un autre. Le presse-papiers plutôt qu'un champ de texte, parce
        // que 90 caractères d'hexadécimal ne se lisent ni ne se retapent.
        ImGui::Separator();
        if (ro::RoButton(i18n::Tr("Copier le code"))) {
          // 🔴 `ShareableRecipe` et non `recipe_` : un code quitte son porteur.
          // « Couleur d'origine » y deviendrait « la tienne » chez qui le colle.
          const std::string code =
              fx::palette_cache::EncodeShare(ShareableRecipe());
          ImGui::SetClipboardText(code.c_str());
          shared_tick_ = ImGui::GetTime();
        }
        ImGui::SameLine();
        // Poser le lien dans la barre de chat. Le geste s'arrête là : c'est le
        // joueur qui envoie, comme pour tout autre lien.
        if (ro::RoButton(i18n::Tr("Partager dans le chat"))) {
          if (auto* chat = Bourgeon::Instance().chat_window()) {
            // Même raison que pour « Copier le code » : le lien voyage seul.
            const std::string code =
                fx::palette_cache::EncodeShare(ShareableRecipe());
            // Pseudo nul = le nôtre, résolu par la chatbox : le getter natif et
            // la conversion de code-page sont chez elle.
            chat->AppendStyleLink(code.c_str(), nullptr);
          }
        }
        ImGui::SameLine();
        if (ro::RoButton(i18n::Tr("Coller un code"))) {
          const char* colle = ImGui::GetClipboardText();
          ro::PaletteRecipe recu;
          if (colle && fx::palette_cache::DecodeShare(colle, &recu)) {
            recipe_ = recu;
            touched_ = true;  // même raison que pour un préréglage
            // 🔴 Rechargement COMPLET : un code porte sa propre teinte de base,
            // donc la fusion et les rampes ne sont plus les mêmes.
            Reload();
          } else {
            error_code_tick_ = ImGui::GetTime();
          }
        }
        if (error_code_tick_ > 0.0 &&
            ImGui::GetTime() - error_code_tick_ < 4.0) {
          ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
          ImGui::TextUnformatted(
              i18n::Tr("Code illisible ou d'une version trop ancienne."));
          ImGui::PopTextWrapPos();
        }
        ImGui::TreePop();
      }

      if (picker_open) ImGui::EndDisabled();
    }

    // 🔴 EN DERNIER dans la fenêtre, et hors de tout BeginChild : le placement
    // se calcule depuis `GetWindowPos` et le curseur de layout n'est pas
    // restauré derrière (cf. BugReport::TitleBarButton).
    //
    // Dans la BARRE DE TITRE plutôt qu'en pied de page, et ici la raison est
    // criante : le bas de cette fenêtre bouge sans arrêt — « Envoyé. » qui
    // apparaît, le bouton de brouillon qui va et vient, l'arbre des préréglages
    // qu'on déplie. Un bouton de signalement qui glisse sous le curseur et ouvre
    // son infobulle tout seul se ferait cliquer par accident.
    if (auto* br = Bourgeon::Instance().bug_report()) br->TitleBarButton(BugContext());
  }
  ro::EndRoWindow();

  // La fenêtre vient d'être fermée : la recette reste EN JEU (c'est le but —
  // le joueur garde ses couleurs). Seul « Tout réinitialiser » la retire.
}
