#include "features/windows/char_select_layout.h"

#include <Windows.h>
#include <shellapi.h>  // ShellExecuteA (« Ouvrir le dossier »)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "utils/game_paths.h"
#include "utils/log_console.h"
#include "yaml-cpp/yaml.h"
#include "utils/i18n.h"

namespace charsel {
namespace {

// ── Layout d'usine ───────────────────────────────────────────────────────────
// La table calée à l'œil sur le décor « banquet » livré, numéroté 1..25 sur
// l'image : 1 grand trône, 2 petit trône, 3-5 petites tables, 6-14 rangée haute,
// 15/16 bouts, 17-25 rangée basse. C'est ce que le joueur retrouve avec
// « Réinitialiser », et le repli de toute valeur illisible dans le fichier.
const Seat kFactorySeats[kSeatCount] = {
    {0.460f, 0.295f, 0.115f},  // 1  grand trône
    {0.545f, 0.304f, 0.115f},  // 2  petit trône
    {0.414f, 0.453f, 0.115f},  // 3  petite table gauche
    {0.497f, 0.453f, 0.115f},  // 4  petite table milieu
    {0.579f, 0.453f, 0.115f},  // 5  petite table droite
    {0.142f, 0.589f, 0.115f},  // 6  rangée haute
    {0.232f, 0.589f, 0.115f},  // 7
    {0.321f, 0.589f, 0.115f},  // 8
    {0.408f, 0.589f, 0.115f},  // 9
    {0.498f, 0.589f, 0.115f},  // 10
    {0.586f, 0.589f, 0.115f},  // 11
    {0.674f, 0.589f, 0.115f},  // 12
    {0.763f, 0.589f, 0.115f},  // 13
    {0.852f, 0.589f, 0.115f},  // 14
    {0.041f, 0.677f, 0.115f},  // 15 bout gauche
    {0.954f, 0.679f, 0.115f},  // 16 bout droit
    {0.111f, 0.837f, 0.115f},  // 17 rangée basse
    {0.207f, 0.837f, 0.115f},  // 18
    {0.305f, 0.837f, 0.115f},  // 19
    {0.401f, 0.837f, 0.115f},  // 20
    {0.497f, 0.837f, 0.115f},  // 21
    {0.594f, 0.837f, 0.115f},  // 22
    {0.689f, 0.837f, 0.115f},  // 23
    {0.782f, 0.837f, 0.115f},  // 24
    {0.880f, 0.837f, 0.115f},  // 25
};

// Décor livré avec le client (résolu dans le GRF). Le layout d'usine le
// désigne ; les fonds du joueur vivent, eux, sous lobby\.
const char kFactoryBackground[] = "lobby_hall.bmp";

// Sous-dossier scanné pour les décors du joueur. Le loader natif reçoit
// « lobby\<fichier>.bmp » : le VFS cherche le DISQUE avant les GRF, un fichier
// déposé là est donc trouvé sans repacker quoi que ce soit.
const char kUserBgSubdir[] = "lobby";

// 🔴 Le chemin donné au TexMgr n'est PAS relatif à data\, mais à data\texture\.
// `UITextureMgr_Load 0x00a8d4a0` fait `Path_ConcatPrefixIfMissing(chemin, préfixe
// du TYPE)` — pour un .bmp le préfixe est « texture\ » — et c'est seulement
// ensuite que `Res_MakeDataRootRelativePath 0x00573380` ajoute « data\ ». Même
// règle que les icônes d'items ou le bouton Discord (cf. moonlight_auth.cc).
// Le dossier à SCANNER sur le disque est donc data\texture\lobby\, pas
// data\lobby\ : un fichier posé là serait listé mais jamais trouvé au chargement.
const char kUserBgDiskDir[] = "data\\texture\\lobby\\";
// L'emplacement qu'on croit « naturel » (et que Bourgeon a indiqué à tort avant
// cette correction) : on le regarde pour pouvoir DIRE au joueur que ses images
// sont au mauvais endroit, plutôt que de le laisser devant une liste vide.
const char kUserBgWrongDir[] = "data\\lobby\\";

// Bornes de validation. Généreuses à dessein — un siège légèrement hors cadre est
// un choix de mise en page recevable (personnage qui entre par le bord), une
// coordonnée à 40 est un fichier corrompu.
bool ValidFrac(float v)  { return v > -0.5f && v < 1.5f; }
bool ValidScale(float v) { return v >= 0.02f && v <= 0.60f; }

// Un chemin VFS acceptable : ASCII imprimable, longueur raisonnable, pas de
// remontée d'arborescence ni de chemin absolu. Ce n'est pas une frontière de
// sécurité (le fichier est écrit par le joueur lui-même) mais un garde-fou contre
// une valeur bricolée à la main qui ferait diverger le loader natif.
bool ValidVfsPath(const std::string& p) {
  if (p.empty() || p.size() > 120) return false;
  if (p.find("..") != std::string::npos) return false;
  if (p.find(':') != std::string::npos) return false;
  if (p[0] == '\\' || p[0] == '/') return false;
  for (const char c : p)
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e)
      return false;
  return true;
}

Layout MakeFactory() {
  Layout l;
  for (int i = 0; i < kSeatCount; ++i) l.seats[i] = kFactorySeats[i];
  l.anchor_count = 0;  // les ancres s'enregistrent à leur premier AnchorRef()
  l.background = kFactoryBackground;
  return l;
}

bool g_dirty = false;
bool g_loaded = false;
std::vector<Preset> g_presets;

// Lit un layout (décor + sièges + ancres) depuis un nœud — le document racine ou
// l'entrée d'un preset, qui portent exactement les mêmes clés. Les champs absents
// ou hors bornes gardent la valeur déjà dans `out` : un fichier partiellement
// écrit (disque plein, crash) dégrade siège par siège au lieu de rendre l'écran
// inutilisable.
void ReadLayoutNode(const YAML::Node& node, Layout* out) {
  if (const YAML::Node bg = node["background"]) {
    const std::string p = bg.as<std::string>("");
    if (ValidVfsPath(p)) out->background = p;
    else if (!p.empty())
      LogError("[CharSelect] décor ignoré (chemin refusé): {}", p);
  }
  out->hide_empty_seats =
      node["hide_empty_seats"].as<bool>(out->hide_empty_seats);
  {
    const float a = node["empty_seat_alpha"].as<float>(out->empty_seat_alpha);
    if (a >= 0.0f && a <= 1.0f) out->empty_seat_alpha = a;
  }
  if (const YAML::Node seats = node["seats"]) {
    const int n = (std::min)(static_cast<int>(seats.size()), kSeatCount);
    for (int i = 0; i < n; ++i) {
      const YAML::Node s = seats[i];
      if (!s) continue;
      const float nx = s["nx"].as<float>(out->seats[i].nx);
      const float ny = s["ny"].as<float>(out->seats[i].ny);
      const float sc = s["scale"].as<float>(out->seats[i].scale);
      if (ValidFrac(nx) && ValidFrac(ny) && ValidScale(sc)) {
        out->seats[i].nx = nx;
        out->seats[i].ny = ny;
        out->seats[i].scale = sc;
      }
      // Pose : bornes larges mais fermées. `anim` est un index d'action de .act
      // — au-delà d'une vingtaine, plus aucun sprite n'en a autant et le repli
      // rendrait n'importe quoi ; `head_dir` vaut -1 (suit le corps) ou 0..2.
      const int dir = s["dir"].as<int>(out->seats[i].dir);
      const int anim = s["anim"].as<int>(out->seats[i].anim);
      const int head = s["head_dir"].as<int>(out->seats[i].head_dir);
      if (dir >= 0 && dir <= 7) out->seats[i].dir = dir;
      if (anim >= 0 && anim <= 20) out->seats[i].anim = anim;
      if (head >= -1 && head <= 2) out->seats[i].head_dir = head;
      out->seats[i].animate = s["animate"].as<bool>(out->seats[i].animate);
      const int fr = s["frame"].as<int>(out->seats[i].frame);
      if (fr >= -1 && fr <= 63) out->seats[i].frame = fr;
    }
  }
  if (const YAML::Node anchors = node["anchors"]) {
    out->anchor_count = 0;  // le nœud fait foi (un preset remplace le registre)
    for (const YAML::Node a : anchors) {
      if (out->anchor_count >= kMaxAnchors) break;
      const std::string name = a["name"].as<std::string>("");
      const float nx = a["nx"].as<float>(0.0f);
      const float ny = a["ny"].as<float>(0.0f);
      if (name.empty() || name.size() >= sizeof(AnchorPt::name)) continue;
      if (!ValidFrac(nx) || !ValidFrac(ny)) continue;
      AnchorPt& dst = out->anchors[out->anchor_count++];
      std::snprintf(dst.name, sizeof(dst.name), "%s", name.c_str());
      dst.nx = nx;
      dst.ny = ny;
    }
  }
}

// Recharge l'état courant ET les mises en page enregistrées depuis le fichier.
void LoadInto(Layout* out) {
  std::ifstream f(paths::CharSelectLayoutPath());
  if (!f) return;  // premier lancement : pas encore de fichier
  try {
    const YAML::Node root = YAML::Load(f);
    ReadLayoutNode(root, out);
    if (const YAML::Node presets = root["presets"]) {
      for (const YAML::Node p : presets) {
        if (static_cast<int>(g_presets.size()) >= kMaxPresets) break;
        const std::string name = p["name"].as<std::string>("");
        if (name.empty() || name.size() > 40) continue;
        Preset item;
        item.name = name;
        item.data = MakeFactory();  // socle : un preset amputé reste cohérent
        ReadLayoutNode(p, &item.data);
        g_presets.push_back(item);
      }
    }
  } catch (const std::exception& e) {
    LogError("[CharSelect] layout illisible ({}) -> mise en page d'origine",
             e.what());
    *out = MakeFactory();
    g_presets.clear();
  }
}

// ── Galerie ──────────────────────────────────────────────────────────────────
std::vector<Background> g_backgrounds;
bool g_scanned = false;
int  g_misplaced = 0;  // .bmp trouvés dans l'ancien emplacement, faux (cf. Scan)

// Un nom de fichier utilisable tel quel par le loader natif. Les noms non-ASCII
// sont ÉCARTÉS : le client attend ses chemins dans sa code-page (CP949), alors que
// FindFirstFileA les rend dans celle du système (CP1252 en fr-FR) — un décor nommé
// « décor.bmp » arriverait donc mojibaké au loader et ne serait jamais trouvé.
// Plutôt que d'échouer silencieusement à l'affichage, on ne le propose pas.
bool AsciiFileName(const char* n) {
  for (const char* p = n; *p; ++p)
    if (static_cast<unsigned char>(*p) < 0x20 || static_cast<unsigned char>(*p) > 0x7e)
      return false;
  return true;
}

void Scan() {
  g_scanned = true;
  g_backgrounds.clear();
  // Les décors livrés d'abord : ils existent toujours, ce sont eux qui font le
  // repli quand le fichier du joueur disparaît.
  g_backgrounds.push_back({kFactoryBackground, "Banquet (d'origine)", false});

  std::vector<Background> user;
  const std::string pattern = BackgroundDir() + "*.bmp";
  WIN32_FIND_DATAA fd = {};
  const HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    int skipped = 0;
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      if (!AsciiFileName(fd.cFileName)) { ++skipped; continue; }
      std::string label(fd.cFileName);
      const auto dot = label.find_last_of('.');
      if (dot != std::string::npos) label.resize(dot);
      user.push_back({std::string(kUserBgSubdir) + "\\" + fd.cFileName, label, true});
    } while (FindNextFileA(h, &fd) && user.size() < 200);
    FindClose(h);
    if (skipped > 0)
      LogDiag("[CharSelect] {} décor(s) ignoré(s) : nom non-ASCII (le loader du "
              "client attend sa propre code-page)", skipped);
  }
  std::sort(user.begin(), user.end(),
            [](const Background& a, const Background& b) { return a.label < b.label; });
  g_backgrounds.insert(g_backgrounds.end(), user.begin(), user.end());

  // Images laissées dans data\lobby\ : le client n'y regarde pas (il résout sous
  // data\texture\). Compté ICI, pas à l'affichage — le panneau le lit à chaque
  // frame, et un FindFirstFile par frame est de l'I/O disque pour rien.
  g_misplaced = 0;
  WIN32_FIND_DATAA wd = {};
  const std::string wrong = paths::InGameDir(kUserBgWrongDir) + "*.bmp";
  const HANDLE wh = FindFirstFileA(wrong.c_str(), &wd);
  if (wh != INVALID_HANDLE_VALUE) {
    do {
      if (!(wd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++g_misplaced;
    } while (FindNextFileA(wh, &wd));
    FindClose(wh);
    LogDiag("[CharSelect] {} .bmp dans {} : emplacement NON lu par le client "
            "(attendu : {})", g_misplaced, paths::InGameDir(kUserBgWrongDir),
            BackgroundDir());
  }
}

}  // namespace

const Layout& Factory() {
  static const Layout f = MakeFactory();
  return f;
}

Layout& State() {
  static Layout s = MakeFactory();
  if (!g_loaded) {
    g_loaded = true;  // AVANT LoadInto : une erreur ne doit pas relancer la lecture
    LoadInto(&s);
  }
  return s;
}

AnchorPt* AnchorRef(const char* name, float def_nx, float def_ny) {
  Layout& l = State();
  for (int i = 0; i < l.anchor_count; ++i)
    if (std::strcmp(l.anchors[i].name, name) == 0) return &l.anchors[i];
  if (l.anchor_count >= kMaxAnchors) return nullptr;
  AnchorPt& a = l.anchors[l.anchor_count++];
  std::snprintf(a.name, sizeof(a.name), "%s", name);
  a.nx = def_nx;
  a.ny = def_ny;
  return &a;
}

void MarkDirty() { g_dirty = true; }
bool Dirty() { return g_dirty; }

void SaveIfDirty() {
  if (!g_dirty) return;
  Save();
}

void Save() {
  g_dirty = false;  // même en cas d'échec d'écriture : inutile de réessayer en boucle
  const Layout& l = State();
  // Les clés d'un layout, écrites à l'identique pour l'état courant (à la racine)
  // et pour chaque preset — c'est ce qui permet à ReadLayoutNode de lire les deux.
  const auto emit_layout = [](YAML::Emitter& out, const Layout& lay) {
    out << YAML::Key << "background" << YAML::Value << lay.background;
    out << YAML::Key << "hide_empty_seats" << YAML::Value << lay.hide_empty_seats;
    out << YAML::Key << "empty_seat_alpha" << YAML::Value << lay.empty_seat_alpha;
    out << YAML::Key << "seats" << YAML::Value << YAML::BeginSeq;
    for (int i = 0; i < kSeatCount; ++i) {
      out << YAML::Flow << YAML::BeginMap;
      out << YAML::Key << "nx" << YAML::Value << lay.seats[i].nx;
      out << YAML::Key << "ny" << YAML::Value << lay.seats[i].ny;
      out << YAML::Key << "scale" << YAML::Value << lay.seats[i].scale;
      out << YAML::Key << "dir" << YAML::Value << lay.seats[i].dir;
      out << YAML::Key << "anim" << YAML::Value << lay.seats[i].anim;
      out << YAML::Key << "head_dir" << YAML::Value << lay.seats[i].head_dir;
      out << YAML::Key << "animate" << YAML::Value << lay.seats[i].animate;
      out << YAML::Key << "frame" << YAML::Value << lay.seats[i].frame;
      out << YAML::EndMap;
    }
    out << YAML::EndSeq;
    out << YAML::Key << "anchors" << YAML::Value << YAML::BeginSeq;
    for (int i = 0; i < lay.anchor_count; ++i) {
      out << YAML::Flow << YAML::BeginMap;
      out << YAML::Key << "name" << YAML::Value << lay.anchors[i].name;
      out << YAML::Key << "nx" << YAML::Value << lay.anchors[i].nx;
      out << YAML::Key << "ny" << YAML::Value << lay.anchors[i].ny;
      out << YAML::EndMap;
    }
    out << YAML::EndSeq;
  };

  YAML::Emitter out;
  out << YAML::BeginMap;
  out << YAML::Key << "version" << YAML::Value << 1;
  emit_layout(out, l);
  out << YAML::Key << "presets" << YAML::Value << YAML::BeginSeq;
  for (const Preset& p : g_presets) {
    out << YAML::BeginMap;
    out << YAML::Key << "name" << YAML::Value << p.name;
    emit_layout(out, p.data);
    out << YAML::EndMap;
  }
  out << YAML::EndSeq;
  out << YAML::EndMap;

  std::ofstream f(paths::CharSelectLayoutPath(), std::ios::trunc);
  if (!f) {
    LogError("[CharSelect] impossible d'écrire {}", paths::CharSelectLayoutPath());
    return;
  }
  f << i18n::Tr("# Bourgeon — mise en page de l'écran de sélection de personnage.\n")
    << i18n::Tr("# Écrit par le mode « Personnaliser » ; les coordonnées sont des\n")
    << i18n::Tr("# fractions de l'écran, donc valables à toute résolution.\n")
    << out.c_str() << "\n";
  LogDiag("[CharSelect] mise en page enregistrée ({} sièges, {} ancres, {} "
          "enregistrement(s), décor {})",
          kSeatCount, l.anchor_count, g_presets.size(), l.background);
}

const std::vector<Preset>& Presets() {
  State();  // garantit que le fichier (donc les presets) a été lu
  return g_presets;
}

bool SavePreset(const char* name) {
  if (!name || !*name) return false;
  std::string n(name);
  // Espaces de bordure retirés : « salon » et « salon  » ne doivent pas produire
  // deux entrées que le joueur ne saurait pas distinguer dans la liste.
  while (!n.empty() && (n.front() == ' ' || n.front() == '\t')) n.erase(n.begin());
  while (!n.empty() && (n.back() == ' ' || n.back() == '\t')) n.pop_back();
  if (n.empty() || n.size() > 40) return false;
  State();  // fichier lu avant de toucher à la liste
  for (Preset& p : g_presets) {
    if (p.name == n) {  // même nom -> mise à jour, pas de doublon
      p.data = State();
      Save();
      return true;
    }
  }
  if (static_cast<int>(g_presets.size()) >= kMaxPresets) {
    LogError("[CharSelect] {} mises en page enregistrées : limite atteinte",
             kMaxPresets);
    return false;
  }
  Preset item;
  item.name = n;
  item.data = State();
  g_presets.push_back(item);
  Save();
  return true;
}

bool ApplyPreset(int index) {
  if (index < 0 || index >= static_cast<int>(Presets().size())) return false;
  Layout& cur = State();
  const Layout src = g_presets[index].data;  // copie : State() est la destination
  for (int i = 0; i < kSeatCount; ++i) cur.seats[i] = src.seats[i];
  cur.anchor_count = src.anchor_count;
  for (int i = 0; i < src.anchor_count; ++i) cur.anchors[i] = src.anchors[i];
  cur.background = src.background;
  MarkDirty();
  return true;
}

bool DeletePreset(int index) {
  if (index < 0 || index >= static_cast<int>(Presets().size())) return false;
  g_presets.erase(g_presets.begin() + index);
  Save();
  return true;
}

void ResetPlacement() {
  Layout& l = State();
  for (int i = 0; i < kSeatCount; ++i) l.seats[i] = kFactorySeats[i];
  // Les ancres sont vidées et non remises à des valeurs figées ici : le site
  // d'appel de chaque bloc porte SON défaut, et AnchorRef les ré-enregistre à la
  // frame suivante. Un seul endroit connaît la position d'usine du titre.
  l.anchor_count = 0;
  MarkDirty();
}

void ResetAll() {
  State().background = kFactoryBackground;
  ResetPlacement();
}

const std::string& BackgroundDir() {
  static const std::string dir = paths::InGameDir(kUserBgDiskDir);
  return dir;
}

int MisplacedBackgroundCount() {
  if (!g_scanned) Scan();
  return g_misplaced;  // mesuré au SCAN : l'appelant l'affiche à chaque frame
}

const std::string& MisplacedBackgroundDir() {
  static const std::string dir = paths::InGameDir(kUserBgWrongDir);
  return dir;
}

const std::vector<Background>& Backgrounds() {
  if (!g_scanned) Scan();
  return g_backgrounds;
}

void RescanBackgrounds() { Scan(); }

void OpenBackgroundFolder() {
  // Le dossier n'existe pas au premier usage : le créer AVANT de l'ouvrir, sinon
  // l'explorateur affiche une erreur au joueur au lieu d'un dossier vide où
  // déposer ses images. Chaque niveau est créé (CreateDirectory ne crée pas les
  // parents) ; data\ et data\texture\ existent souvent déjà, on ne parie pas.
  CreateDirectoryA(paths::InGameDir("data").c_str(), nullptr);
  CreateDirectoryA(paths::InGameDir("data\\texture").c_str(), nullptr);
  CreateDirectoryA(BackgroundDir().c_str(), nullptr);
  // ShellExecute ne fait que poster la demande au shell : pas de pompe de
  // messages ouverte ici, donc rien qui puisse figer la frame en cours.
  ShellExecuteA(nullptr, "open", BackgroundDir().c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

}  // namespace charsel
