#include "features/windows/tutorial_window.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "bourgeon.h"
#include "features/hotkey_actions.h"
#include "features/hotkey_util.h"
#include "features/moonlight_ui/moonlight_ui.h"  // SaveSettings (le yaml partagé)
#include "features/staff_gate.h"
#include "imgui.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "ui/spr_act.h"  // ReadFile : le VFS du client (disque PUIS les GRF)
#include "utils/i18n.h"
#include "utils/log_console.h"
#include "yaml-cpp/yaml.h"

namespace {

// ── Ce que le décodeur a le droit de garder pour UNE page ────────────────────
//
// 🔴 CES TROIS NOMBRES SONT DÉDUITS DE L'ENREGISTREUR DE ZONE, pas choisis au
// hasard : ses défauts sont 6 s à 10 images/s, sur 640 px de large — soit 60
// images. Le plafond d'images est donc posé juste au-dessus (64) pour qu'un clip
// enregistré aux réglages d'usine passe ENTIER : tronquer, c'est couper la fin du
// geste qu'on voulait montrer, et personne ne verrait pourquoi.
//
// La largeur, elle, est ramenée à 360 px : 64 images de 640 px pèseraient 88 Mio
// de pixels, quand 64 images de 360 px en pèsent 19. C'est aussi la largeur à
// laquelle la fenêtre les affiche — agrandir ne montrerait rien de plus.
const imgdec::Limits kGifLimits = {
    4096,                    // garde-fou d'en-tête
    360,                     // largeur (ou hauteur) d'affichage
    64,                      // 6,4 s à 10 images/s
    24u * 1024u * 1024u,     // ~19 Mio attendus, la marge absorbe un format 16:10
};

// Mise en page. Aucune de ces valeurs n'est un nombre nu dans le code : elles
// passent toutes par `ro::Px`, qui suit l'échelle d'interface du joueur.
constexpr float kWindowW    = 720.0f;
constexpr float kWindowH    = 540.0f;
constexpr float kSummaryW   = 205.0f;  // colonne du sommaire
constexpr float kFooterH    = 34.0f;   // barre de navigation du bas
constexpr float kGifPadY    = 6.0f;
constexpr float kChipPadX   = 5.0f;    // pastille d'un raccourci : marge interne
constexpr float kChipPadY   = 1.0f;
constexpr float kChipRound  = 3.0f;

// Couleurs. Le crème est celui que le reste de l'interface utilise pour un texte
// mis en avant sur fond sombre.
constexpr ImU32 kChipBg     = IM_COL32(48, 42, 32, 255);
constexpr ImU32 kChipBorder = IM_COL32(150, 132, 96, 255);
constexpr ImU32 kChipText   = IM_COL32(245, 237, 209, 255);
constexpr ImU32 kTipBg      = IM_COL32(32, 40, 32, 160);
constexpr ImU32 kTipBorder  = IM_COL32(110, 150, 110, 200);

// Le dossier du tutoriel : données de CLIENT, livrées par le patcher auprès des
// autres données de jeu — pas un réglage utilisateur, donc pas dans SaveData.
//
// 🔴 C'EST UN CHEMIN DU VFS, PAS UN CHEMIN DE DISQUE, et il fut l'inverse. Le
// patcher range ce dossier DANS le GRF de Moonlight comme le reste des données
// de client : chez l'auteur, qui a `data\bourgeon\tutorial\` en fichiers
// libres, un `fopen` trouvait tout ; chez le JOUEUR il ne trouvait rien, et le
// tutoriel s'ouvrait sur « Fichier introuvable ». La panne ne pouvait donc se
// voir qu'après livraison.
//
// `ro::spract::ReadFile` est le lecteur du client lui-même, celui qui résout le
// disque D'ABORD et les GRF ensuite (cf. reference_grf_loading_patcher) : un
// fichier déposé à la main dans `data\` prime toujours sur l'archive, donc
// corriger une page sans repacker marche exactement comme avant.
//
// ⚠ Les entrées d'un GRF sont en MINUSCULES et en antislashs (le VFS normalise
// ce qu'on lui donne, mais l'entrée, elle, est ce que le packeur y a mis). Les
// noms de gifs écrits dans le yaml suivent donc la même règle, et restent en
// ASCII — le reste du VFS est en CP949, nos sources en UTF-8, et un nom accentué
// ne désignerait la même entrée dans aucun des deux.
const char* const kTutorialDir = "data\\bourgeon\\tutorial\\";

std::string TutorialPath(const std::string& name) {
  return std::string(kTutorialDir) + name;
}

// Octets d'un fichier du tutoriel. `out` vide si le nom n'existe ni sur le
// disque ni dans une archive montée.
bool ReadTutorialFile(const std::string& name, std::vector<uint8_t>* out) {
  return ro::spract::ReadFile(TutorialPath(name).c_str(), out);
}

// Le fichier de contenu de la langue courante, avec repli sur le français. Rend
// le NOM retenu (pour le message d'erreur) et ses octets.
//
// Le catalogue i18n ne convient pas pour ce texte-là : il traduit des LITTÉRAUX
// du code, et une page de tutoriel est une donnée. Traduire, ici, c'est déposer
// `tutorial.en.yaml` à côté de `tutorial.yaml`.
bool ReadContent(std::string* out_name, std::vector<uint8_t>* out) {
  const std::string& lang = i18n::LanguageCode();
  if (lang != "fr") {
    *out_name = "tutorial." + lang + ".yaml";
    if (ReadTutorialFile(*out_name, out)) return true;
  }
  *out_name = "tutorial.yaml";
  return ReadTutorialFile(*out_name, out);
}

// Le libellé du raccourci que CE joueur a posé sur une action de Bourgeon.
// « (aucune touche) » si l'action existe mais n'est liée à rien — c'est une
// information, pas un échec : elle dit au joueur qu'il peut en poser une.
std::string HotkeyLabelOf(const std::string& action_id) {
  const int index = hotkeys::IndexOf(action_id.c_str());
  if (index < 0) return std::string(i18n::Tr("(action inconnue)"));
  const hotkeys::Binding& b = hotkeys::BindingAt(index);
  if (b.vk == 0) return std::string(i18n::Tr("(aucune touche)"));
  char label[64] = {};
  hotkeys::Label(b.vk, b.ctrl, b.alt, b.shift, label, sizeof(label));
  return std::string(label);
}

// ── Couper un titre trop long, SANS casser un caractère ─────────────────
//
// Le sommaire est une colonne étroite et les titres sont libres : « Inventaire,
// entrepôt et charrette » déborde, et ImGui le laisse alors filer SOUS le bord de
// la colonne — le texte disparaît sans que rien ne signale qu'il en manque (vu en
// jeu le 2026-09-05). On coupe donc nous-mêmes, en posant les points de
// suspension qui DISENT qu'il y a une suite ; le titre entier reste accessible en
// infobulle.
//
// ⚠ La coupe se fait sur une frontière UTF-8. Trancher au milieu d'un « é »
// (deux octets) laisserait un octet orphelin, qu'ImGui rend en carré vide — et
// nos titres sont français, donc le cas est la règle et non l'exception.
bool Ellipsize(const std::string& text, float max_width, std::string* out) {
  if (ImGui::CalcTextSize(text.c_str()).x <= max_width) {
    *out = text;
    return false;
  }
  static const char* const kEllipsis = "...";
  const float dots = ImGui::CalcTextSize(kEllipsis).x;
  size_t cut = text.size();
  while (cut > 0) {
    --cut;
    // Reculer jusqu'au début d'un caractère : les octets 0x80-0xBF sont des
    // CONTINUATIONS, jamais un début.
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    if (ImGui::CalcTextSize(text.substr(0, cut).c_str()).x + dots <= max_width) break;
  }
  *out = text.substr(0, cut) + kEllipsis;
  return true;
}

// ── Lire un texte du yaml, ou DIRE pourquoi il manque ───────────────────
//
// 🔴 UNE VALEUR NON QUOTÉE EST UN PIÈGE, ET IL EST MUET. YAML lit « a : b » comme
// une PAIRE clé/valeur, pas comme une phrase : la puce
//     - Tout est **traduit** : le français est la langue par défaut.
// n'est donc pas un texte mais une map, `as<std::string>` rend son repli, et la
// page affichait une puce VIDE sans que rien ne l'explique (constaté en jeu le
// 2026-09-05, quatre puces perdues sur dix pages). Même famille que le « * » en
// tête, qui lui au moins fait échouer le fichier entier, donc se voit.
//
// D'où cette lecture : ce qui n'est pas un scalaire devient un texte qui DIT
// quoi corriger, à l'endroit exact où la phrase manquait.
std::string TextOf(const YAML::Node& node) {
  if (!node) return std::string();
  if (node.IsScalar()) return node.as<std::string>("");
  return std::string(i18n::Tr(
      "(!) ligne à mettre entre guillemets dans tutorial.yaml"));
}

// ── Le balisage, et pourquoi il est si pauvre ────────────────────────────────
// Trois formes, pas une de plus : `{touche:id}`, `{perso}` et `**gras**`. Un
// tutoriel se lit, il ne se met pas en page — et chaque forme ajoutée serait à
// re-rendre à l'identique le jour où le site publiera le même yaml.
enum class SegStyle { kPlain, kBold, kChip };

struct Segment {
  std::string text;
  SegStyle    style = SegStyle::kPlain;
};

// Découpe un paragraphe en segments, en RÉSOLVANT les substitutions au passage.
// Résolues ici et non au chargement : le raccourci peut changer pendant que la
// fenêtre est ouverte, et la phrase doit changer avec lui.
std::vector<Segment> SplitMarkup(const std::string& text) {
  std::vector<Segment> out;
  std::string plain;
  bool bold = false;

  auto flush = [&](SegStyle style) {
    if (!plain.empty()) {
      out.push_back({plain, style});
      plain.clear();
    }
  };

  for (size_t i = 0; i < text.size();) {
    if (text.compare(i, 2, "**") == 0) {
      flush(bold ? SegStyle::kBold : SegStyle::kPlain);
      bold = !bold;
      i += 2;
      continue;
    }
    if (text[i] == '{') {
      const size_t end = text.find('}', i);
      if (end != std::string::npos) {
        const std::string tag = text.substr(i + 1, end - i - 1);
        if (tag.compare(0, 7, "touche:") == 0) {
          flush(bold ? SegStyle::kBold : SegStyle::kPlain);
          out.push_back({HotkeyLabelOf(tag.substr(7)), SegStyle::kChip});
          i = end + 1;
          continue;
        }
        if (tag == "perso") {
          plain += Bourgeon::Instance().client().session().GetCharName();
          i = end + 1;
          continue;
        }
      }
    }
    plain += text[i];
    ++i;
  }
  flush(bold ? SegStyle::kBold : SegStyle::kPlain);
  return out;
}

// Un mot (ou une pastille) prêt à poser sur la ligne.
struct Word {
  std::string text;
  SegStyle    style;
  float       width;
};

float MeasureWord(const std::string& text, SegStyle style) {
  if (style == SegStyle::kBold) {
    ImFont* bold = ro::FontBold();
    if (bold != nullptr) {
      return bold->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f,
                                 text.c_str(), text.c_str() + text.size()).x;
    }
  }
  const float w = ImGui::CalcTextSize(text.c_str()).x;
  if (style == SegStyle::kChip) return w + 2.0f * ro::Px(kChipPadX);
  return w;
}

}  // namespace

// ── Le contenu ───────────────────────────────────────────────────────────────

bool TutorialWindow::LoadContent() {
  pages_.clear();
  content_version_ = 0;
  load_error_.clear();
  content_lang_ = i18n::LanguageCode();

  std::string          name;
  std::vector<uint8_t> bytes;
  if (!ReadContent(&name, &bytes) || bytes.empty()) {
    char msg[512];
    std::snprintf(msg, sizeof(msg), i18n::Tr("Fichier introuvable : %s"),
                  TutorialPath(name).c_str());
    load_error_ = msg;
    return false;
  }

  try {
    // ⚠ `YAML::Load` sur une chaîne, et non `LoadFile` : les octets viennent du
    // VFS, pas d'un descripteur de fichier. Un éventuel BOM UTF-8 en tête est
    // reconnu par yaml-cpp, qui détecte l'encodage de son flux.
    const YAML::Node root = YAML::Load(
        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    content_version_ = root["version"].as<int>(1);

    const YAML::Node pages = root["pages"];
    if (pages && pages.IsSequence()) {
      for (const YAML::Node& node : pages) {
        Page page;
        page.id    = TextOf(node["id"]);
        page.title = TextOf(node["title"]);
        page.gif   = TextOf(node["gif"]);
        page.body  = TextOf(node["body"]);
        page.tip   = TextOf(node["tip"]);
        if (const YAML::Node bullets = node["bullets"]) {
          if (bullets.IsSequence())
            for (const YAML::Node& b : bullets) page.bullets.push_back(TextOf(b));
        }
        // Une page sans identifiant ne pourrait pas être reprise : on lui en
        // donne un, dérivé de son rang, plutôt que de la refuser.
        if (page.id.empty()) {
          char fallback[32];
          std::snprintf(fallback, sizeof(fallback), "page%d",
                        static_cast<int>(pages_.size() + 1));
          page.id = fallback;
        }
        if (!page.title.empty() || !page.body.empty()) pages_.push_back(page);
      }
    }
  } catch (const std::exception& e) {
    // yaml-cpp lance sur un document mal formé. On NOMME l'erreur au lieu de
    // rendre une fenêtre vide : ces fichiers sont les nôtres, un auteur les lit.
    char msg[512];
    std::snprintf(msg, sizeof(msg), i18n::Tr("Lecture impossible : %s"), e.what());
    load_error_ = msg;
    LogDiag("[tutorial] {}", load_error_);
    return false;
  }

  if (pages_.empty() && load_error_.empty())
    load_error_ = i18n::Tr("Le fichier ne contient aucune page.");
  return load_error_.empty();
}

// ── Ouverture / navigation ───────────────────────────────────────────────────

void TutorialWindow::Open() {
  LoadContent();

  // Reprise : on rouvre sur la page mémorisée, si elle existe toujours. Un
  // identifiant disparu (page retirée du yaml) renvoie au début, sans bruit.
  int start = 0;
  for (size_t i = 0; i < pages_.size(); ++i) {
    if (pages_[i].id == last_page_) {
      start = static_cast<int>(i);
      break;
    }
  }
  open_ = true;
  anim_page_ = -1;  // force le (re)chargement du gif de la page ouverte
  ShowPage(start);
}

void TutorialWindow::Close() {
  open_ = false;
  anim_.Unload();
  anim_page_ = -1;

  // Fermer, c'est avoir vu. Sans ça, le tutoriel se rouvrirait à chaque
  // connexion chez qui l'a refermé — ce qui ne se lit plus comme une aide.
  seen_version_ = content_version_ > 0 ? content_version_ : seen_version_;
  if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
}

void TutorialWindow::ShowPage(int index) {
  if (pages_.empty()) return;
  page_ = std::max(0, std::min(index, static_cast<int>(pages_.size()) - 1));
  last_page_ = pages_[page_].id;

  // 🔴 Une animation à la fois : celle qu'on quitte est rendue avant que celle
  // qu'on ouvre ne soit demandée. Deux gifs de 19 Mio en vol, dans un processus
  // 32 bits, c'est un plafond de VRAM qu'on n'a pas les moyens de tester.
  if (anim_page_ != page_) {
    anim_.Unload();
    anim_page_ = page_;
    const std::string& gif = pages_[page_].gif;
    if (!gif.empty()) {
      // 🔴 La lecture se fait ICI, sur le thread du jeu, et pas dans le thread
      // de décodage de GifAnim. Le VFS est du code natif du client — liste
      // chaînée des archives, allocateur maison — dont rien ne dit qu'il
      // supporte deux appelants ; tout le reste du projet ne l'appelle que
      // depuis ce thread-ci. Ce sont donc les OCTETS qu'on confie au thread. Le
      // coût payé ici est d'une lecture par CHANGEMENT DE PAGE, pas par frame.
      std::vector<uint8_t> bytes;
      ReadTutorialFile(gif, &bytes);  // vide : GifAnim le dira à l'écran
      anim_.Load(TutorialPath(gif), std::move(bytes), kGifLimits);
    }
  }
}

// ── Événements ───────────────────────────────────────────────────────────────

void TutorialWindow::OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) {
  const bool now_in_game = mode_type == ModeMgr::ModeType::kGame;
  if (now_in_game && !in_game_) check_auto_ = true;
  if (!now_in_game && in_game_) {
    // Retour au char-select : on ferme et on rend la mémoire. Rouvrir en jeu
    // reprendra la page mémorisée.
    if (open_) Close();
  }
  in_game_ = now_in_game;
}

void TutorialWindow::OnTick() {
  // Pump même fenêtre fermée : c'est lui qui vide la file de libération
  // différée. Sans textures ni pixels, il ne coûte rien.
  anim_.Pump();

  if (!check_auto_) return;
  check_auto_ = false;
  if (!auto_open_ || open_) return;

  // ⚠ Le contenu est lu ICI et non au démarrage : c'est son `version:` qui décide
  // de l'ouverture, et le relire à chaque entrée en jeu coûte une lecture de
  // fichier par session.
  if (!LoadContent()) return;
  if (seen_version_ >= content_version_) return;
  Open();
}

// ── Rendu ────────────────────────────────────────────────────────────────────

void TutorialWindow::DrawRichText(const std::string& text, float wrap_width) {
  // 🔴 UN PARAGRAPHE EST UN BLOC DE LIGNES, PAS UNE LIGNE. Un bloc « body: | »
  // du yaml est écrit à la largeur du FICHIER, avec des retours tous les 80
  // caractères qui ne veulent rien dire — c'est la ligne VIDE qui sépare deux
  // paragraphes. Couper à chaque retour donnait le texte aéré et haché de la
  // première version (vu en jeu le 2026-09-05) : les retours simples sont donc
  // recollés en espaces, et c'est le retour à la ligne D'ICI — calculé sur la
  // largeur de la fenêtre — qui décide où le texte se coupe.
  size_t start = 0;
  bool   first_para = true;
  while (start <= text.size()) {
    size_t end = text.find("\n\n", start);
    const bool last = end == std::string::npos;
    if (last) end = text.size();
    std::string para = text.substr(start, end - start);
    start = last ? text.size() + 1 : end + 2;

    for (char& c : para)
      if (c == '\n') c = ' ';
    while (!para.empty() && para.back() == ' ') para.pop_back();
    if (para.empty()) continue;

    if (!first_para) ImGui::Spacing();  // respiration ENTRE paragraphes
    first_para = false;

    // Chaque segment est découpé en MOTS : c'est l'unité de retour à la ligne.
    std::vector<Word> words;
    for (const Segment& seg : SplitMarkup(para)) {
      if (seg.style == SegStyle::kChip) {
        words.push_back({seg.text, seg.style, MeasureWord(seg.text, seg.style)});
        continue;
      }
      size_t i = 0;
      while (i < seg.text.size()) {
        const size_t space = seg.text.find(' ', i);
        const std::string word =
            seg.text.substr(i, space == std::string::npos ? std::string::npos : space - i);
        if (!word.empty())
          words.push_back({word, seg.style, MeasureWord(word, seg.style)});
        if (space == std::string::npos) break;
        i = space + 1;
      }
    }

    const float space_w = ImGui::CalcTextSize(" ").x;
    const float line_h  = ImGui::GetTextLineHeight();
    float used = 0.0f;
    bool  first_on_line = true;

    for (const Word& w : words) {
      if (!first_on_line && used + space_w + w.width > wrap_width) {
        ImGui::NewLine();
        used = 0.0f;
        first_on_line = true;
      }
      if (!first_on_line) {
        ImGui::SameLine(0.0f, space_w);
        used += space_w;
      }

      switch (w.style) {
        case SegStyle::kBold: {
          ImFont* bold = ro::FontBold();
          if (bold != nullptr) ImGui::PushFont(bold);
          ImGui::TextUnformatted(w.text.c_str());
          if (bold != nullptr) ImGui::PopFont();
          break;
        }
        case SegStyle::kChip: {
          // La pastille : on RÉSERVE la place avec un Dummy, puis on peint dans
          // le rectangle que l'item vient d'occuper. Passer par la draw-list
          // évite d'inventer un widget pour trois traits.
          const float pad_x = ro::Px(kChipPadX);
          const float pad_y = ro::Px(kChipPadY);
          ImGui::Dummy(ImVec2(w.width, line_h));
          const ImVec2 p0 = ImGui::GetItemRectMin();
          const ImVec2 p1 = ImGui::GetItemRectMax();
          ImDrawList* dl = ImGui::GetWindowDrawList();
          dl->AddRectFilled(ImVec2(p0.x, p0.y - pad_y), ImVec2(p1.x, p1.y + pad_y),
                            kChipBg, ro::Px(kChipRound));
          dl->AddRect(ImVec2(p0.x, p0.y - pad_y), ImVec2(p1.x, p1.y + pad_y),
                      kChipBorder, ro::Px(kChipRound));
          dl->AddText(ImVec2(p0.x + pad_x, p0.y), kChipText, w.text.c_str());
          break;
        }
        default:
          ImGui::TextUnformatted(w.text.c_str());
          break;
      }

      used += w.width;
      first_on_line = false;
    }
    if (!first_on_line) ImGui::NewLine();
  }
}

void TutorialWindow::DrawSummary(float width) {
  ImGui::BeginChild("tutorial_summary", ImVec2(width, -ro::Px(kFooterH)), true);
  for (size_t i = 0; i < pages_.size(); ++i) {
    const bool current = static_cast<int>(i) == page_;

    // Le numéro est mesuré avec le titre : c'est la ligne ENTIÈRE qui doit tenir.
    char numbered[192];
    std::snprintf(numbered, sizeof(numbered), "%d. %s", static_cast<int>(i) + 1,
                  pages_[i].title.c_str());
    std::string shown;
    const bool cut = Ellipsize(numbered, ImGui::GetContentRegionAvail().x, &shown);

    // ⚠ L'identité du widget tient à son rang, pas à son libellé : le texte
    // affiché change avec la largeur de la colonne et avec la langue.
    char label[224];
    std::snprintf(label, sizeof(label), "%s###tutopage%d", shown.c_str(),
                  static_cast<int>(i));

    if (current) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kChipText));
    if (ImGui::Selectable(label, current)) ShowPage(static_cast<int>(i));
    if (current) ImGui::PopStyleColor();
    if (cut && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", numbered);
  }
  ImGui::EndChild();
}

void TutorialWindow::DrawPage(const Page& page, float content_width) {
  ImGui::BeginChild("tutorial_page", ImVec2(0.0f, -ro::Px(kFooterH)), false);

  ImFont* bold = ro::FontBold();
  if (bold != nullptr) ImGui::PushFont(bold);
  ImGui::TextUnformatted(page.title.c_str());
  if (bold != nullptr) ImGui::PopFont();
  ImGui::Separator();
  ImGui::Spacing();

  // L'image. Centrée, à sa taille décodée : la réduction a déjà eu lieu, et
  // l'étirer rendrait floue une capture faite au pixel près.
  if (!page.gif.empty()) {
    anim_.Pump();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (void* tex = anim_.Frame()) {
      const float w = static_cast<float>(anim_.Width());
      const float h = static_cast<float>(anim_.Height());
      const float shown_w = std::min(w, avail);
      const float shown_h = (w > 0.0f) ? h * (shown_w / w) : h;
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - shown_w) * 0.5f));
      ImGui::Image(TexId(tex), ImVec2(shown_w, shown_h));
      // Une réserve du décodeur (« animé illisible : affiché en image fixe ») se
      // dit SOUS l'image : sans ça, un gif immobile passe pour une panne
      // d'affichage, et c'est par cette question-là qu'elle est remontée.
      if (!anim_.Error().empty())
        ImGui::TextDisabled("%s : %s", page.gif.c_str(), anim_.Error().c_str());
      ImGui::Dummy(ImVec2(0.0f, ro::Px(kGifPadY)));
    } else if (anim_.Loading()) {
      ImGui::TextDisabled("%s", i18n::Tr("Chargement de l'animation..."));
    } else if (!anim_.Error().empty()) {
      // Ces gifs-là sont les nôtres : un échec est un bug d'auteur, il se dit.
      ImGui::TextDisabled("%s : %s", page.gif.c_str(), anim_.Error().c_str());
    }
  }

  if (!page.body.empty()) {
    DrawRichText(page.body, content_width);
    ImGui::Spacing();
  }

  for (const std::string& bullet : page.bullets) {
    ImGui::Bullet();
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    DrawRichText(bullet, content_width - ImGui::GetCursorPosX());
  }

  if (!page.tip.empty()) {
    ImGui::Spacing();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::BeginGroup();
    ImGui::Indent(ro::Px(6.0f));
    ImGui::TextDisabled("%s", i18n::Tr("Astuce"));
    DrawRichText(page.tip, content_width - ro::Px(12.0f));
    ImGui::Unindent(ro::Px(6.0f));
    ImGui::EndGroup();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p0.x - ro::Px(4.0f), p0.y - ro::Px(2.0f)),
                      ImVec2(p1.x + ro::Px(4.0f), p1.y + ro::Px(2.0f)), kTipBg,
                      ro::Px(kChipRound));
    dl->AddRect(ImVec2(p0.x - ro::Px(4.0f), p0.y - ro::Px(2.0f)),
                ImVec2(p1.x + ro::Px(4.0f), p1.y + ro::Px(2.0f)), kTipBorder,
                ro::Px(kChipRound));
  }

  ImGui::EndChild();
}

void TutorialWindow::OnRenderUI() {
  if (!open_) return;

  // La langue a changé pendant que la fenêtre était ouverte : le contenu est un
  // FICHIER par langue, il faut donc le relire (le catalogue i18n, lui, ne sait
  // rien de ces pages).
  if (content_lang_ != i18n::LanguageCode()) {
    const std::string keep = last_page_;
    LoadContent();
    last_page_ = keep;
    anim_page_ = -1;
    int index = 0;
    for (size_t i = 0; i < pages_.size(); ++i)
      if (pages_[i].id == last_page_) index = static_cast<int>(i);
    ShowPage(index);
  }

  ImGui::SetNextWindowSize(ImVec2(ro::Px(kWindowW), ro::Px(kWindowH)),
                           ImGuiCond_FirstUseEver);
  bool keep_open = true;
  const bool begun =
      ro::BeginRoWindow(i18n::TrId("Découvrir Bourgeon", "bourgeon_tutorial"), &keep_open);
  if (!begun) {
    ro::EndRoWindow();
    if (!keep_open) Close();
    return;
  }

  if (pages_.empty()) {
    ImGui::TextWrapped("%s", load_error_.empty()
                                 ? i18n::Tr("Aucune page de tutoriel n'est installée.")
                                 : load_error_.c_str());
    ImGui::Spacing();
    if (ro::RoButton(i18n::Tr("Réessayer"))) LoadContent();
    ro::EndRoWindow();
    if (!keep_open) Close();
    return;
  }

  DrawSummary(ro::Px(kSummaryW));
  ImGui::SameLine();
  const float content_width = ImGui::GetContentRegionAvail().x - ro::Px(8.0f);
  DrawPage(pages_[page_], content_width);

  // ── Pied : où on en est, et par où continuer ──────────────────────────────
  ImGui::Separator();
  char counter[64];
  std::snprintf(counter, sizeof(counter), i18n::Tr("Page %d sur %d"), page_ + 1,
                static_cast<int>(pages_.size()));
  ImGui::TextDisabled("%s", counter);
  ImGui::SameLine();

  const bool last = page_ + 1 >= static_cast<int>(pages_.size());
  const char* next_label = last ? i18n::Tr("Terminer") : i18n::Tr("Suivant >");
  const float buttons_w = ro::ButtonWidth(i18n::Tr("< Précédent")) +
                          ro::ButtonWidth(next_label) + ImGui::GetStyle().ItemSpacing.x;
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - buttons_w - ro::Px(12.0f));

  ImGui::BeginDisabled(page_ == 0);
  if (ro::RoButton(i18n::Tr("< Précédent"))) ShowPage(page_ - 1);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ro::RoButton(next_label)) {
    if (last) {
      keep_open = false;
    } else {
      ShowPage(page_ + 1);
    }
  }

  ro::EndRoWindow();
  if (!keep_open) Close();
}

// ── Réglages ─────────────────────────────────────────────────────────────────

bool TutorialWindow::DrawSettings() {
  bool changed = false;

  if (ro::RoButton(i18n::Tr("Revoir le tutoriel"))) Open();
  ImGui::SameLine();
  mui::HelpMarker(i18n::Tr(
      "Une visite guidée des nouveautés de Bourgeon, avec une animation par "
      "fonctionnalité. Reprend à la page où vous vous étiez arrêté."));

  if (ro::RoCheckbox(i18n::Tr("Le proposer à la première connexion"), &auto_open_))
    changed = true;

  // Le tutoriel se rouvre quand son contenu gagne des pages. Remettre ce
  // compteur à zéro est la façon de le revoir « comme la première fois ».
  if (seen_version_ > 0) {
    ImGui::SameLine();
    if (ro::RoSmallButton(i18n::Tr("Oublier"))) {
      seen_version_ = 0;
      changed = true;
    }
  }

  if (IsStaff() && !load_error_.empty()) {
    ImGui::TextDisabled("%s", load_error_.c_str());
  }
  return changed;
}
