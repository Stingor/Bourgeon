#include "ui/game_texture.h"
#include "features/systems/moonlight_auth.h"

#include <Windows.h>
#include <dpapi.h>  // CryptProtectData/CryptUnprotectData (sauvegarde mot de passe)
#include <winhttp.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include <shellapi.h>  // CommandLineToArgvW (parse --server)

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <vector>

#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB / Overlay_DeviceEpoch
#include "imgui.h"
#include "nlohmann/json.hpp"
#include "features/systems/auto_login.h"
#include "features/systems/login_spectator.h"
#include "features/systems/native_login.h"
#include "features/windows/game_settings.h"  // le panneau de réglages, ouvert d'ici
#include "bourgeon.h"                        // Bourgeon::game_settings()
#include "ragnarok/ragnarok_client.h"  // PostGameKey (frappes destinées au natif)
#include "ui/ro_imgui.h"
#include "ui/skin_panel.h"  // ro::DrawUiFontCombo (le combo de police, partagé)
#include "utils/log_console.h"
#include "utils/game_paths.h"
#include "utils/startup_settings.h"
#include "yaml-cpp/yaml.h"
#include "ragnarok/msgstring_override.h"
#include "utils/i18n.h"
#include "utils/text.h"  // text::ToLowerAscii / ContainsNoCase

namespace {

// Largeur PLANCHER des contrôles du formulaire. La fenêtre s'auto-dimensionne
// autour d'eux, et ce minimum évite que champs et boutons ne s'effondrent quand
// il n'y a rien de plus large à afficher.
constexpr float kFormMinW = 340.0f;

// Largeur commune des contrôles du formulaire : tout ce que la fenêtre offre.
//
// 🔴 DYNAMIQUE et non figée, parce que la fenêtre est en `AlwaysAutoResize` :
// elle se règle sur son élément le plus large, et un formulaire calibré en dur
// pour le français laissait une bande vide à droite dès qu'un libellé traduit
// dépassait — l'anglais et l'espagnol allongent la ligne « Langue / Police ».
// Ici chaque contrôle occupe la largeur réelle, quelle qu'elle soit.
//
// ⚠ À appeler EN DÉBUT DE LIGNE : `GetContentRegionAvail` part du curseur, donc
// après un `SameLine` il ne rend plus que ce qui reste.
//
// ⚠ Rien ne doit demander PLUS que ce que cette fonction rend, sans quoi la
// fenêtre grandirait d'un cran à chaque frame. Un contrôle exactement large de
// `FormWidth()` contribue exactement la largeur courante : le calcul a un point
// fixe, et la taille se stabilise en une frame ou deux. (Le plancher, lui, ne
// pousse qu'une fois vers le haut.)
float FormWidth() {
  return (std::max)(ImGui::GetContentRegionAvail().x, kFormMinW);
}

// URL « usine » du serveur Moonlight — identique pour tous les joueurs, intégrée
// au build : aucun utilisateur n'a à toucher à la config. La section yaml
// `moonlight_auth:` ne sert qu'à SURCHARGER (dev/local, ou désactiver).
constexpr const char* kDefaultBaseUrl = "https://moonlight-destiny.fr";

// L'adresse RETENUE, publiée par LoadConfig pour `SiteBaseUrl` (cf. l'en-tête).
// Vide tant que la config n'a pas été lue : l'accesseur retombe alors sur
// l'adresse d'usine ci-dessus.
std::string g_site_base;

// Délai au-delà duquel « mode login SANS fenêtre de login » vaut service-select,
// quand le nombre de connexions n'a pas pu être déterminé. Assez long pour couvrir
// la construction de UILoginWnd (chargement des textures au 1er lancement) : le
// prix d'un faux positif est une transition d'état rejouée, pas un blocage.
constexpr unsigned long kSvcSelectProbeMs = 1500;

// Auto-confirmation du char-server : délai APRÈS LE TIR du login au-delà duquel
// on poste l'Entrée même sans avoir vu la fenêtre « Select Service » (id 2).
// Repli de sûreté si un parcours ne la construit pas ; la détection d'échec
// s'arme à 1,2 s, donc ce repli ne peut plus tirer sur un login refusé.
constexpr unsigned long kCharSrvProbeMs = 1500;

// Rattrapage « compte encore en ligne » (joueur actif ou autotrade) : le premier
// essai est TOUJOURS refusé, le login-server ayant seulement DEMANDÉ le kick de
// la session en cours. Temps laissé à la chaîne login->char->map pour fermer la
// session (map_quit + sauvegarde), puis nombre d'essais de rattrapage.
constexpr unsigned long kKickWaitMs = 3000;
constexpr int kMaxRelogins = 2;

// (Le dossier du jeu vit dans utils/game_paths.h : paths::GameDir().)

// Identifiant web mémorisé — fichier dédié pour NE PAS réécrire (et abîmer) le
// yaml de config, qui est souvent édité à la main et lu seul par AutoLogin.
std::string RememberPath() { return paths::MoonlightUserPath(); }
// Mot de passe web mémorisé — chiffré DPAPI (lié au compte Windows courant :
// illisible par un autre utilisateur/machine, aucune clé à gérer).
std::string PwPath() { return paths::MoonlightPwPath(); }
bool DpapiEncryptToFile(const std::string& path, const std::string& plain) {
  DATA_BLOB in{static_cast<DWORD>(plain.size()),
               reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
  DATA_BLOB out{};
  if (!CryptProtectData(&in, L"MoonlightAuth", nullptr, nullptr, nullptr, 0,
                        &out))
    return false;
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  bool ok = false;
  if (f) {
    f.write(reinterpret_cast<const char*>(out.pbData), out.cbData);
    ok = static_cast<bool>(f);
  }
  LocalFree(out.pbData);
  return ok;
}

std::string DpapiDecryptFromFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "";
  std::stringstream ss;
  ss << f.rdbuf();
  std::string enc = ss.str();
  if (enc.empty()) return "";
  DATA_BLOB in{static_cast<DWORD>(enc.size()),
               reinterpret_cast<BYTE*>(const_cast<char*>(enc.data()))};
  DATA_BLOB out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
    return "";
  std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
  LocalFree(out.pbData);
  return plain;
}

// Liste ordonnée des <display>…</display> de clientinfo.xml (mêmes noms/ordre que
// le service-select natif). Scanner volontairement minimal (ASCII).
std::vector<std::string> ReadConnectionNames(const std::string& path) {
  std::vector<std::string> names;
  std::ifstream f(path, std::ios::binary);
  if (!f) return names;
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string xml = ss.str();
  const std::string open = "<display>", close = "</display>";
  size_t pos = 0;
  while ((pos = xml.find(open, pos)) != std::string::npos) {
    const size_t start = pos + open.size();
    const size_t end = xml.find(close, start);
    if (end == std::string::npos) break;
    std::string n = xml.substr(start, end - start);
    const auto b = n.find_first_not_of(" \t\r\n");
    const auto e = n.find_last_not_of(" \t\r\n");
    names.push_back(b == std::string::npos ? "" : n.substr(b, e - b + 1));
    pos = end + close.size();
  }
  return names;
}

// Valeur de `--server:<x>` / `--server=<x>` sur la ligne de commande (vide sinon).
std::string ParseServerArg() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return "";
  std::string out;
  for (int i = 1; i < argc; ++i) {
    std::string a;
    for (const wchar_t* w = argv[i]; *w; ++w) a.push_back(static_cast<char>(*w));
    const std::string pfx = "--server";
    if (a.size() > pfx.size() && a.compare(0, pfx.size(), pfx) == 0 &&
        (a[pfx.size()] == ':' || a[pfx.size()] == '=')) {
      out = a.substr(pfx.size() + 1);
      break;
    }
  }
  LocalFree(argv);
  return out;
}

// Encodage x-www-form-urlencoded d'une valeur.
std::string UrlEncode(const std::string& s) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xf]);
    }
  }
  return out;
}

// Traduit un code d'erreur WinHTTP en message lisible (les plus courants).
std::string WinHttpErr(unsigned long e) {
  const char* m;
  switch (e) {
    case 12002: m = i18n::Tr("délai dépassé (timeout)"); break;
    case 12007: m = i18n::Tr("nom de domaine introuvable (DNS)"); break;
    case 12029: m = i18n::Tr("connexion refusée/impossible"); break;
    case 12030: m = i18n::Tr("connexion coupée"); break;
    case 12045: m = i18n::Tr("certificat TLS : autorité non reconnue"); break;
    case 12057: m = i18n::Tr("certificat TLS : révocation non vérifiable"); break;
    case 12175: m = i18n::Tr("échec TLS (certificat invalide/auto-signé ?)"); break;
    default:    m = "erreur"; break;
  }
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%s [%lu]", m, e);
  return buf;
}

// POST synchrone (exécuté sur le thread worker). full_url = ASCII.
MoonlightAuth::HttpResult DoPost(const std::string& full_url,
                                 const std::string& body, bool verify_tls) {
  MoonlightAuth::HttpResult res;

  std::wstring wurl(full_url.begin(), full_url.end());
  URL_COMPONENTS uc;
  ZeroMemory(&uc, sizeof(uc));
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {0};
  wchar_t path[2048] = {0};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 255;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2047;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    res.error = i18n::Tr("URL invalide");
    return res;
  }
  const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

  HINTERNET hs = WinHttpOpen(L"Bourgeon/MoonlightAuth",
                             WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hs) {
    res.error = i18n::Tr("WinHttpOpen a échoué");
    return res;
  }
  WinHttpSetTimeouts(hs, 10000, 10000, 15000, 15000);

  HINTERNET hc = WinHttpConnect(hs, host, uc.nPort, 0);
  HINTERNET hr = nullptr;
  if (hc) {
    hr = WinHttpOpenRequest(hc, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                            secure ? WINHTTP_FLAG_SECURE : 0);
  }
  if (hr && secure && !verify_tls) {
    // Dev uniquement : ignore la validation du certificat (cert auto-signé /
    // reverse-proxy LAN sans le vrai cert). NE JAMAIS activer en prod.
    DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hr, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));
  }
  if (hr) {
    static const wchar_t kHdr[] =
        L"Content-Type: application/x-www-form-urlencoded";
    BOOL ok = WinHttpSendRequest(
        hr, kHdr, static_cast<DWORD>(-1), const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (ok) ok = WinHttpReceiveResponse(hr, nullptr);
    if (!ok) res.error = WinHttpErr(GetLastError());
    if (ok) {
      DWORD code = 0, len = sizeof(code);
      WinHttpQueryHeaders(
          hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
      res.status = static_cast<long>(code);

      std::string body_out;
      DWORD avail = 0;
      do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(hr, &avail)) break;
        if (avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(hr, &chunk[0], avail, &read)) break;
        chunk.resize(read);
        body_out += chunk;
      } while (avail > 0);
      res.body = std::move(body_out);
    }
    // (res.error déjà renseigné via WinHttpErr si !ok)
  } else if (res.error.empty()) {
    // Échec de WinHttpConnect / WinHttpOpenRequest.
    res.error = WinHttpErr(GetLastError());
  }

  if (hr) WinHttpCloseHandle(hr);
  if (hc) WinHttpCloseHandle(hc);
  WinHttpCloseHandle(hs);
  if (!res.error.empty())
    LogDiag("[MoonlightAuth] POST {} -> {}", full_url, res.error);
  return res;
}

// ── Texture du bouton Discord (data\texture\Discord.bmp) ─────────────────────
// Chargée via le TexMgr natif (chemin RELATIF à data\texture\, comme les icônes
// d'item) puis convertie en texture overlay ImGui. Cache 1 slot, recréé après un
// reset device (epoch) — sinon handle mort -> crash au dessin (cf. d3d9_hook.h).

struct ButtonTex { void* tex = nullptr; int w = 0; int h = 0; };

// Charge une BMP de data\texture\ en pixels BGRA (pointeur natif). SEH (POD only).
bool LoadRawBgra(const char* path, const uint8_t** bgra, int* w, int* h) {
  // 1024 et non 4096 : ce sont des VIGNETTES d'ecran de login, et un plafond
  // serre est un garde-fou de vraisemblance de plus sur une structure lue en
  // memoire (cf. ui/game_texture.h).
  ro::texmgr::RawImage img;
  if (!ro::texmgr::LoadRaw(path, &img, /*max_dim=*/1024)) return false;
  *bgra = img.bgra; *w = img.w; *h = img.h;
  return true;
}

ButtonTex LoadDiscordBmp() {
  const uint8_t* bgra = nullptr;
  int w = 0, h = 0;
  if (!LoadRawBgra("Discord.bmp", &bgra, &w, &h)) return {};
  std::vector<uint8_t> argb(static_cast<size_t>(w) * h * 4);
  for (int i = 0; i < w * h; ++i) {
    const uint8_t b = bgra[i * 4], g = bgra[i * 4 + 1], r = bgra[i * 4 + 2];
    const bool ck = (r == 0xFF && g == 0 && b == 0xFF);  // magenta -> transparent
    argb[i * 4] = b; argb[i * 4 + 1] = g; argb[i * 4 + 2] = r;
    argb[i * 4 + 3] = ck ? 0 : 0xFF;
  }
  // Alpha-bleeding : les pixels transparents gardent alpha=0 mais reçoivent la
  // couleur MOYENNE d'un voisin opaque. Sans ça, le filtrage bilinéaire de l'GPU
  // mélange le magenta (RGB conservé) dans les bords -> halo rose autour des
  // lettres. Une seule passe suffit (le bilinéaire ne déborde que d'un texel).
  std::vector<uint8_t> out = argb;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int idx = (y * w + x) * 4;
      if (argb[idx + 3] != 0) continue;  // opaque -> inchangé
      int bb = 0, gg = 0, rr = 0, n = 0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          const int nidx = (ny * w + nx) * 4;
          if (argb[nidx + 3] == 0) continue;  // voisin transparent -> ignoré
          bb += argb[nidx]; gg += argb[nidx + 1]; rr += argb[nidx + 2]; ++n;
        }
      }
      if (n) {
        out[idx] = static_cast<uint8_t>(bb / n);
        out[idx + 1] = static_cast<uint8_t>(gg / n);
        out[idx + 2] = static_cast<uint8_t>(rr / n);
        // alpha reste 0 : le pixel est toujours transparent, seule sa couleur
        // « fantôme » change pour que l'interpolation ne tire plus vers le rose.
      }
    }
  }
  return {Overlay_CreateTextureARGB(out.data(), w, h), w, h};
}

// Texture du bouton (chargement paresseux, retry tant que non chargée -> couvre
// le device pas encore capturé ; recréée après reset device). Renvoie {} si
// Discord.bmp est absent (le bouton bascule alors sur un repli texte).
const ButtonTex& DiscordButtonTex() {
  static ButtonTex s_tex;
  static unsigned s_epoch = 0;
  const unsigned e = Overlay_DeviceEpoch();
  if (e != s_epoch) { s_tex = {}; s_epoch = e; }
  if (!s_tex.tex) s_tex = LoadDiscordBmp();
  return s_tex;
}

// Petit lien hypertexte ImGui : texte bleu, souligné au survol (+ curseur main),
// ouvre `url` dans le navigateur au clic.
void HyperlinkOpen(const char* label, const std::string& url) {
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 160, 235, 255));
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  if (ImGui::IsItemHovered()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y), ImVec2(mx.x, mx.y),
                                        IM_COL32(120, 160, 235, 255));
  }
  if (ImGui::IsItemClicked())
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Le temps qu'on laisse à la fenêtre de login pour finir de s'initialiser avant
// d'écrire dans ses champs (cf. MoonlightAuth::login_wnd_tick_). Le décor de
// connexion attend la même chose, pour la même raison.
constexpr unsigned long kLoginSettleMs = 400;

// Combien de temps on attend la fenêtre de login, pendant le PILOTAGE, avant de
// se dire qu'un choix de connexion nous barre la route et de le franchir. Large :
// c'est un filet, et le cas nominal est que la fenêtre arrive d'elle-même.
constexpr unsigned long kSvcSelectAfterDriveMs = 2000;

// ── Le choix de la langue et de la police, sur l'écran de login ──────────────
// Les deux réglages existent déjà dans le panneau « Interface de jeu », mais
// celui-là n'est atteignable qu'une fois EN JEU. Un joueur anglophone qui lance
// le client pour la première fois devrait donc traverser un formulaire français,
// un char-select français et une entrée en jeu avant de pouvoir demander
// l'anglais. C'est ici qu'il faut le lui proposer, sur le premier écran qu'il
// voit — et la police va avec : c'est aussi le premier écran où on la lit.
void DrawLanguageAndFontPickers() {
  ImGui::Spacing();
  ImGui::Separator();

  // Les deux combos se PARTAGENT la largeur du formulaire, libellés déduits :
  // cette ligne est la plus large de la fenêtre, donc la seule qui puisse en
  // fixer la taille. Lui laisser des largeurs en dur revenait à l'élargir pour
  // elle seule pendant que le reste du formulaire gardait la sienne — d'où la
  // bande vide à droite, d'autant plus visible que les libellés s'allongent en
  // anglais et en espagnol.
  //
  // Les libellés se mesurent donc TRADUITS (« Idioma » n'a pas la largeur de
  // « Langue »), et `hide_text_after_double_hash` écarte l'identifiant stable que
  // `TrId` colle derrière.
  const char* lang_label = i18n::TrId("Langue", "bourgeon_login_language");
  const char* font_label = i18n::TrId("Police", "bourgeon_login_ui_font");
  const ImGuiStyle& style = ImGui::GetStyle();
  const float labels_w = ImGui::CalcTextSize(lang_label, nullptr, true).x +
                         ImGui::CalcTextSize(font_label, nullptr, true).x;
  // Un combo suivi de son libellé, deux fois : deux espacements internes et
  // celui qui sépare les deux groupes.
  const float spacing_w = style.ItemInnerSpacing.x * 2.0f + style.ItemSpacing.x;
  // Plancher par combo : en dessous, l'aperçu (« Malgun Gothic ») ne dirait plus
  // rien. La fenêtre s'élargit alors une fois pour l'accueillir, et les autres
  // contrôles suivent puisqu'ils lisent la même largeur.
  constexpr float kComboMinW = 110.0f;
  const float combos_w =
      (std::max)(FormWidth() - labels_w - spacing_w, kComboMinW * 2.0f);
  // 40/60 : les noms de langue sont courts (« Français », « Español »), les noms
  // de police beaucoup moins (« ImGui (ProggyClean) »).
  const float lang_w = (std::max)(combos_w * 0.4f, kComboMinW);
  const float font_w = combos_w - lang_w;

  // 🔴 COPIE et non référence : `SetLanguage` réécrit la chaîne globale au milieu
  // de la boucle. Une référence changerait donc de valeur en cours de route, et
  // les entrées suivantes se compareraient au code qu'on vient de poser.
  const std::string current = i18n::LanguageCode();
  ImGui::SetNextItemWidth(lang_w);
  // `TrId` et non `Tr` : RoBeginCombo fait `PushID(label)`, donc un libellé
  // traduit donnerait un widget différent d'une langue à l'autre.
  if (ro::RoBeginCombo(lang_label, i18n::LabelOf(current))) {
    for (const i18n::Language& language : i18n::AvailableLanguages()) {
      const bool selected = (current == language.code);
      // Une langue sans catalogue reste VISIBLE mais inerte : la masquer
      // laisserait croire que Bourgeon ne la connaît pas.
      if (!language.available) ImGui::BeginDisabled();
      // `language.label` est un littéral immortel de la table des langues — et
      // surtout PAS une chaîne rendue par `Tr` : `SetLanguage` vide le catalogue,
      // ce qui invaliderait un pointeur obtenu avant le clic. C'est aussi
      // pourquoi ces combos sont dessinés EN DERNIER, après tout le reste de la
      // fenêtre : plus aucun libellé traduit n'est en vol quand il bascule.
      if (ImGui::Selectable(language.label, selected) && !selected) {
        i18n::SetLanguage(language.code);
        msgoverride::Reload();  // la table du client suit, cf. panel_interface.cc
      }
      if (!language.available) ImGui::EndDisabled();
      if (selected) ImGui::SetItemDefaultFocus();
    }
    // 🔴 `ro::RoEndCombo`, PAS `ImGui::EndCombo` : RoBeginCombo n'appelle pas
    // BeginCombo, il dessine le champ à la main et ouvre un `ImGui::BeginPopup`.
    ro::RoEndCombo();
  }

  // 🔴 La police APRÈS la langue, et ce n'est pas qu'une question de mise en
  // page : ses libellés viennent eux aussi du catalogue, et les lire une fois la
  // bascule ci-dessus passée garantit qu'aucun pointeur jeté par `SetLanguage`
  // n'est encore en vol. (`font_label`, lui, est une COPIE — `TrId` recopie la
  // traduction dans son anneau, d'où sa lecture possible plus haut.)
  ImGui::SameLine();
  ro::DrawUiFontCombo(font_label, font_w);

  // ── Le décor de connexion ─────────────────────────────────────────────────
  // Sa place est ici, avec la langue et la police : les trois réglages du
  // PREMIER écran, ceux qu'on ne peut pas aller chercher dans le panneau du jeu
  // sans s'être connecté d'abord. Et celui-ci gouverne précisément ce qu'on a
  // sous les yeux.
  //
  // Décocher ferme le décor sur-le-champ et l'écrit (spectator::SetBackdropWanted).
  bool backdrop = spectator::BackdropWanted();
  if (ro::RoCheckbox(i18n::TrId("Ville en fond", "bourgeon_login_backdrop"),
                     &backdrop)) {
    spectator::SetBackdropWanted(backdrop);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s",
                      i18n::Tr("Affiche une ville du serveur en direct derrière "
                               "cet écran, tirée au hasard à chaque connexion. "
                               "Décoche si ta machine ou ta connexion préfèrent "
                               "s'en passer."));
  }

  // ⚠ Seulement quand un décor est EN PLACE : sans session à refermer, ce bouton
  // n'aurait rien à relancer. Il disparaît donc de lui-même pendant la séquence
  // et une fois le décor décoché.
  if (backdrop && spectator::InWorld()) {
    ImGui::SameLine();
    if (ro::RoSmallButton(
            i18n::TrId("Changer de vue", "bourgeon_login_backdrop_reroll"))) {
      spectator::Reroll();
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s",
                        i18n::Tr("Tire une autre ville. Le lieu est choisi par "
                                 "le serveur à l'ouverture d'une session : en "
                                 "changer demande d'en rouvrir une, ce qui prend "
                                 "un instant."));
    }
  }
}

}  // namespace

MoonlightAuth::MoonlightAuth(AutoLogin* auto_login) : auto_login_(auto_login) {
  LoadConfig();
  ResolveServer();
  state_ = enabled_ ? State::kWebLogin : State::kDisabled;
  // Un plugin inerte = login web indisponible : c'est une anomalie de config
  // (base_url manquant dans le fichier de démarrage), pas un fonctionnement normal.
  if (!enabled_)
    LogDiag("[MoonlightAuth] inerte (base_url manquant/enabled=false) — login web "
            "indisponible");
}

MoonlightAuth::~MoonlightAuth() { JoinWorker(); }

void MoonlightAuth::LoadConfig() {
  // Valeurs usine : activé pour tous, URL du serveur intégrée au build. Le yaml
  // est OPTIONNEL et ne fait que surcharger (dev/local, ou enabled:false).
  base_url_ = kDefaultBaseUrl;
  enabled_ = true;
  try {
    // Réglage d'AVANT le jeu : fichier de démarrage, ancien yaml en secours.
    const YAML::Node ma = startup::Section("moonlight_auth");
    if (ma) {
      base_url_ = ma["base_url"].as<std::string>(base_url_);
      endpoint_ = ma["endpoint"].as<std::string>(endpoint_);
      save_id_ = ma["save_id"].as<bool>(save_id_);
      remember_ = ma["remember"].as<bool>(remember_);
      verify_tls_ = ma["verify_tls"].as<bool>(verify_tls_);
      enabled_ = ma["enabled"].as<bool>(enabled_);
    }
  } catch (const std::exception& e) {
    LogError("[MoonlightAuth] config illisible: {}", e.what());
  }
  if (base_url_.empty()) enabled_ = false;
  // Retire un éventuel '/' final du base_url pour concaténer proprement.
  while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();

  // 🔴 Publiée pour les liens de PAGE (bestiaire, DB d'objets, avatar), qui
  // vivent hors de ce module. Posée même quand `enabled_` est faux : effacer
  // `base_url` désactive le login WEB, ça ne veut pas dire « plus de site ».
  g_site_base = base_url_;

  // Identifiant mémorisé (fichier dédié, non le yaml).
  if (remember_) {
    std::ifstream rf(RememberPath());
    if (rf) {
      std::string u;
      std::getline(rf, u);
      const auto e = u.find_last_not_of(" \t\r\n");
      if (e != std::string::npos) u.resize(e + 1);
      std::snprintf(user_buf_, sizeof(user_buf_), "%s", u.c_str());
    }
  }

  // Mot de passe mémorisé (DPAPI) : la présence d'un fichier déchiffrable = opt-in
  // actif. Pré-remplit le champ pour accélérer le login.
  const std::string pw = DpapiDecryptFromFile(PwPath());
  if (!pw.empty()) {
    remember_pw_ = true;
    remember_ = true;  // le pw mémorisé implique l'identifiant
    std::snprintf(pass_buf_, sizeof(pass_buf_), "%s", pw.c_str());
  }
}

void MoonlightAuth::SavePref(const char* password) {
  // Identifiant web.
  if (remember_) {
    std::ofstream o(RememberPath(), std::ios::trunc);
    if (o) o << user_buf_;
  } else {
    DeleteFileA(RememberPath().c_str());
  }
  // Mot de passe web (DPAPI, opt-in). Jamais l'OTP de jeu — seulement le mot de
  // passe du compte Moonlight, chiffré, lié au compte Windows.
  // ⚠ On lit `password` et NON pass_buf_ : l'unique appelant (HandleAuthResponse)
  // passe par ApplyAccountList, qui efface pass_buf_ avant de rendre la main. Lire
  // le membre ici revenait donc toujours à voir un champ vide, à prendre la branche
  // else et à SUPPRIMER le fichier — « Se souvenir du mot de passe » ne fonctionnait
  // jamais, et la case revenait décochée au lancement suivant (remember_pw_ est
  // reconstruit depuis la présence de ce fichier).
  const char* pw_to_store = password ? password : pass_buf_;
  if (remember_pw_ && pw_to_store[0] != '\0') {
    DpapiEncryptToFile(PwPath(), pw_to_store);
  } else {
    DeleteFileA(PwPath().c_str());
  }
}

void MoonlightAuth::ResolveServer() {
  // Source PRIMAIRE = l'arbre XML natif (clientinfo.xml lu par le client via son
  // VFS, GRF compris). La lecture DISQUE ne sert que de repli si l'arbre n'est pas
  // encore parsé : chez les joueurs clientinfo.xml n'existe QUE dans moonlight.grf,
  // donc `data\clientinfo.xml` est absent -> 0 connexion -> le service-select
  // n'était jamais franchi (le formulaire Moonlight restait derrière lui).
  std::vector<std::string> names = native_login::ClientInfoConnectionNames();
  if (names.empty()) names = ReadConnectionNames(paths::ClientInfoPath());
  server_count_ = static_cast<int>(names.size());
  server_index_ = 0;  // défaut = 1ʳᵉ connexion (Moonlight-Destiny, base clientinfo)
  server_name_ = ParseServerArg();  // envoyé au site pour cibler la bonne DB
  const std::string want = text::ToLowerAscii(server_name_);
  if (!want.empty()) {
    for (int i = 0; i < server_count_; ++i) {
      if (text::ToLowerAscii(names[i]) == want) {
        server_index_ = i;
        break;
      }
    }
  }
}

void MoonlightAuth::DriveServerSelect() {
  // Frappes DESTINÉES AU NATIF : postées par le canal marqué, seul chemin qui
  // traverse la capture ImGui et notre propre confiscation d'Entrée
  // (cf. WantsEnterKey).
  auto press = [](int vk) { RagnarokClient::PostGameKey(vk); };
  // Le client mémorise la dernière connexion : on force le haut de liste (Haut
  // borne en tête), puis on descend à l'index cible, puis on valide.
  for (int i = 0; i < server_count_; ++i) press(VK_UP);
  for (int i = 0; i < server_index_; ++i) press(VK_DOWN);
  press(VK_RETURN);
}

void MoonlightAuth::JoinWorker() {
  if (worker_.joinable()) worker_.join();
}

void MoonlightAuth::StartPost(const std::string& form_body) {
  JoinWorker();
  ready_.store(false);
  busy_.store(true);
  const std::string url = base_url_ + endpoint_;
  const bool verify_tls = verify_tls_;
  worker_ = std::thread([this, url, form_body, verify_tls]() {
    HttpResult r = DoPost(url, form_body, verify_tls);
    {
      std::lock_guard<std::mutex> lock(mtx_);
      result_ = std::move(r);
    }
    ready_.store(true);
    busy_.store(false);
  });
}

bool MoonlightAuth::TakeResult(HttpResult* out) {
  if (!ready_.load()) return false;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    *out = result_;
  }
  ready_.store(false);
  JoinWorker();  // le thread a fini (ready_ posé après result_) : join immédiat
  return true;
}

void MoonlightAuth::OnModeSwitch(ModeMgr::ModeType mode_type,
                                 const char* /*map*/) {
  if (!enabled_) return;
  if (mode_type == ModeMgr::ModeType::kLogin) {
    // Nouvelle entrée à l'écran de login : réarme l'auto-passage du service-select
    // (fenêtre <connection>, présente seulement si >1 connexion).
    login_enter_tick_ = GetTickCount();
    server_select_done_ = false;
    svc_kbd_fallback_ = false;
    svc_select_tick_ = 0;
    // Le constructeur du plugin tourne AVANT que le client ne parse clientinfo.xml
    // (LoadClientInfoXml) : la liste des connexions y était alors vide. On la
    // re-résout ici — en mode login, l'arbre XML natif est forcément prêt.
    if (server_count_ == 0) ResolveServer();
    // 🔴 Sortie d'une session spectateur : ce retour au mode login est PROVOQUÉ
    // par nous — le décor se démonte justement pour rendre l'écran de connexion
    // natif dans lequel il y a un login à piloter. Le prendre pour une
    // déconnexion ordinaire réarmerait le formulaire, effaçant l'OTP qu'on vient
    // d'obtenir et renvoyant le joueur à la case départ après qu'il a tout saisi.
    if (spectator::Active()) return;
    // Retour au char-select DEPUIS LE JEU (changement de perso) : c'est un
    // kGame->kLogin, mais la session char-server est toujours vivante et on est
    // déjà authentifié. Il ne faut PAS reforcer le formulaire web (sinon le
    // joueur ressaisirait ses identifiants Moonlight par-dessus le char-select).
    // On reste en passthrough (kDriveLogin + fired_), char-select ImGui conservé.
    if (authenticated_ && native_login::SocketFd() != -1) {
      state_ = State::kDriveLogin;
      fired_ = true;                   // ne pas re-déclencher de login
      socket_seen_ = true;             // déjà connecté (pas de log "socket ouverte")
      fire_tick_ = GetTickCount();     // évite un faux "échec" (tick périmé)
      charsel_reached_ = true;         // on REVIENT au char-select : drive déjà terminé
      // drove_moonlight_login_ reste vrai -> CharSelect ImGui reste actif.
      return;
    }
    // Sinon (vraie (re)connexion / déconnexion) : (ré)arme le formulaire. On
    // n'écrase pas une saisie en cours si on n'est pas déjà en fin de flux.
    if (state_ == State::kDriveLogin || state_ == State::kDisabled)
      RearmWebLogin();  // mode LOGIN sans session vivante
  }
}

bool MoonlightAuth::WantsKeyboard() const {
  // Repli « Login classique » : les écrans natifs redeviennent ceux du joueur, on
  // leur rend le clavier (taper ses identifiants et valider à l'Entrée, c'est
  // tout l'intérêt du repli).
  if (!enabled_ || native_fallback_ || state_ == State::kDisabled) return false;
  // Et seulement sur les écrans de connexion : une fois en jeu, le clavier est
  // celui du jeu. `AtLoginScreen()` couvre login ET char-select (même mode).
  //
  // 🔴 Le décor de connexion en fait partie, tout « en jeu » qu'il soit : le
  // client y est bel et bien en mode de jeu, donc sans cette seconde branche
  // chaque touche du formulaire part au JEU — raccourcis, chatbox, et le
  // personnage du spectateur qui reçoit des ordres pendant qu'on tape son
  // identifiant.
  // ⚠ `Pending` est là pour couvrir l'instant le plus tôt de tous : le voile du
  // décor est déjà posé alors que le client n'est parfois même pas encore entré
  // dans son mode de connexion. `AtLoginScreen()` répond alors « non », et sans
  // ce troisième terme les touches partiraient à des écrans natifs en train de
  // se construire, invisibles sous le voile.
  return native_login::AtLoginScreen() || spectator::InWorld() ||
         spectator::Pending();
}

void MoonlightAuth::RearmWebLogin(bool service_select_pending) {
  if (!enabled_) return;
  accounts_.clear();
  selected_ = -1;
  web_ticket_.clear();
  game_session_.clear();
  discord_authorize_url_.clear();
  error_msg_.clear();
  pass_buf_[0] = '\0';
  // « Se souvenir du mot de passe » coché : on repré-remplit depuis le coffre DPAPI,
  // comme au lancement (LoadConfig). Sans ça, un retour à l'écran de connexion
  // obligerait à retaper le mot de passe alors que l'option est active.
  if (remember_pw_) {
    const std::string pw = DpapiDecryptFromFile(PwPath());
    if (!pw.empty()) std::snprintf(pass_buf_, sizeof(pass_buf_), "%s", pw.c_str());
  }
  native_fallback_ = false;        // réarme le login moderne à chaque retour
  drove_moonlight_login_ = false;  // reset le gate du char-select ImGui
  fired_ = false;                  // réarme le déclencheur de login natif
  socket_seen_ = false;
  charsrv_tries_ = 0;
  selected_online_ = false;        // rattrapage « compte en ligne » désarmé
  relogin_tries_ = 0;
  charsel_reached_ = false;        // vrai (re)login -> le drive reprend à zéro
  authenticated_ = false;
  // Service-select (fenêtre de choix de <connection>) : à repasser seulement si le
  // natif va le reconstruire (nouvelle entrée dans le mode login).
  // ⚠ Sur un retour depuis le char-select (cmd 10011 -> état 3), il N'apparaît PAS :
  // l'état 3 recrée directement UILoginWnd. Tirer quand même la sélection
  // (cmd 0x2723 = CLoginMode_SendMsg 10019, Apply_ClientInfoConnection) rejouerait une
  // transition d'état et pourrait ramener un écran de choix — d'où le drapeau.
  login_enter_tick_ = GetTickCount();
  server_select_done_ = !service_select_pending;
  svc_kbd_fallback_ = false;
  svc_select_tick_ = 0;
  state_ = State::kWebLogin;
}

void MoonlightAuth::OnRenderLoginUI() {
  if (!enabled_ || state_ == State::kDisabled) return;
  // Séquence spectateur EN COURS : elle pilote les écrans natifs, et notre
  // formulaire viendrait s'y superposer — le joueur cliquerait au milieu d'un
  // automate. Une fois la session EN PLACE, en revanche, ce formulaire est
  // exactement ce qu'il faut dessiner : c'est lui l'écran de connexion, et la
  // capitale est son décor.
  if (spectator::Connecting()) return;
  // Le décor tient-il ? Trois branches en dépendent plus bas — il n'y a pas
  // d'écran de connexion NATIF derrière, donc rien à franchir, rien à masquer, et
  // le pilotage du login doit d'abord faire tomber la session.
  const bool backdrop = spectator::InWorld();
  // Repli login natif choisi pour la session : la fenêtre native a déjà été
  // réaffichée (edge, au clic du bouton) — on rend juste la main.
  if (native_fallback_) return;
  // À partir d'ici, le parcours Moonlight occupe l'écran : on n'affiche QUE le
  // curseur redessiné par ImGui, partout (le natif est poussé hors écran). Posé
  // chaque frame et couvrant toutes les branches à `return` précoce ci-dessous
  // (kDriveLogin, service-select, formulaire). NON posé en repli natif -> curseur
  // classique intact pour le « Login classique ».
  ro::SetFullscreenCursorActive();
  // Pilotage natif du login (aucune UI focusable ici). Edge-trigger : on écrit
  // userid+OTP dans les champs natifs et on déclenche le bouton Start UNE fois
  // (SetText + OnMsg). On NE masque plus par frame (OnMsg 0xBA détruit la fenêtre
  // -> plus rien à masquer, et la garde vtable rendrait l'écriture no-op).
  if (state_ == State::kDriveLogin) {
    // 🔴 La session spectateur doit tomber AVANT quoi que ce soit d'autre : le
    // pilotage écrit dans les champs de l'écran de connexion NATIF, qui n'existe
    // pas tant que le décor tient. Et rien d'autre ne doit tourner en attendant
    // — le chemin du retour repasse par le char-select natif, que les sondes
    // ci-dessous prendraient pour la fin réussie d'un login pas même commencé
    // (`charsel_reached_`), laissant le joueur sur un écran qui n'attend plus
    // rien. `Leave` est sans effet une fois la sortie lancée : la redemander à
    // chaque frame ne relance pas la manœuvre.
    if (spectator::Active()) {
      spectator::Leave();
      return;
    }
    // ── Couvrir le pilotage ───────────────────────────────────────────────────
    // De la validation du compte jusqu'à l'arrivée du char-select, le client
    // traverse des écrans NATIFS qu'on est en train de piloter : la fenêtre de
    // login, le choix du char-server, le char-select natif avant que le nôtre ne
    // le recouvre. Une petite seconde, largement de quoi les voir défiler.
    //
    // 🔴 SANS capturer le clavier, et c'est toute la difficulté de cet endroit :
    // ce bloc s'interdisait jusqu'ici la moindre fenêtre ImGui, parce qu'une
    // fenêtre FOCUSABLE ferait avaler par le hook les frappes qu'on destine aux
    // écrans natifs (l'Entrée d'auto-confirmation du char-server). Un voile sans
    // widget ni navigation ne prend pas le focus : il couvre sans rien
    // intercepter.
    //
    // On s'arrête dès `charsel_reached_` : à partir de là, c'est au char-select
    // ImGui de tenir l'écran, avec son propre décor.
    if (!charsel_reached_) {
      // 🔴 ET IL CAPTE LE CLAVIER. La règle vaut pour tout voile : tant qu'un
      // écran d'attente couvre l'image, aucune touche du joueur ne doit atteindre
      // ce qu'il y a dessous — ici des écrans natifs qu'on est en train de
      // piloter, et qui réagissent aux touches sans consulter leur visibilité.
      //
      // ⚠ La réserve d'origine (« aucune fenêtre focusable ici ») ne tient plus :
      // elle datait du pilotage à coups de frappes synthétiques. Aujourd'hui les
      // champs sont écrits directement, et la seule frappe qu'on poste encore
      // porte une marque dans son `lParam` que le hook reconnaît AVANT ImGui
      // (cf. kSyntheticKeyLParam) — capturer ici ne peut donc pas l'avaler.
      ro::DrawFullscreenCover(i18n::Tr("Connexion…"),
                              /*capture_keyboard=*/true);
    }
    if (!fired_) {
      // 🔴 Laisser la fenêtre de login SE POSER avant d'écrire dedans (cf.
      // login_wnd_tick_). Sans ce délai, le contenu que le client repose après
      // construction écrase le nôtre, et c'est le sien qui part avec le bouton
      // Start — « Unregistered ID » sur l'identifiant, « mot de passe
      // incorrect » sur l'OTP.
      if (!native_login::LoginWindowPresent()) {
        login_wnd_tick_ = 0;  // pas encore là (ou reconstruite) : on recommence
        // 🔴 Elle peut ne JAMAIS venir, et c'est arrivé : au retour du décor de
        // connexion, le client reconstruit son mode et se retrouve devant le
        // CHOIX DE CONNEXION. Ce bloc-ci attend la fenêtre de login ; la branche
        // qui sait franchir le service-select est plus bas et n'est jamais
        // atteinte, puisqu'on sort d'ici. Résultat mesuré : le voile
        // « Connexion… » pour toujours, sans échec ni tir — le client semble
        // pendu alors qu'il attend simplement qu'on lui réponde.
        //
        // On franchit donc nous-mêmes, une seule fois (`server_select_done_`,
        // remis à zéro à chaque entrée dans le mode), et seulement après avoir
        // laissé au mode le temps de se construire : tirer sur un mode à peine
        // né sélectionnerait dans le vide.
        if (!server_select_done_ && login_enter_tick_ != 0 &&
            (GetTickCount() - login_enter_tick_) > kSvcSelectAfterDriveMs) {
          server_select_done_ = true;
          LogDiag("[MoonlightAuth] pilotage en attente : choix de connexion "
                  "franchi (index {})",
                  server_index_);
          native_login::SelectClientInfoConnection(server_index_);
        }
        return;
      }
      const unsigned long now = GetTickCount();
      if (login_wnd_tick_ == 0) {
        login_wnd_tick_ = now;
        return;
      }
      if (now - login_wnd_tick_ < kLoginSettleMs) return;
      if (native_login::DriveLogin(drive_user_.c_str(), drive_pw_.c_str())) {
        fired_ = true;
        fire_tick_ = GetTickCount();
        LogDiag("[MoonlightAuth] login piloté pour « {} » ({} ms après "
                "l'apparition de la fenêtre)",
                drive_user_, now - login_wnd_tick_);
      }
      return;
    }
    // Socket login ouverte = le login a atteint le serveur (un refus est alors un
    // problème de credentials, pas de transport).
    if (!socket_seen_ && native_login::SocketFd() >= 0) {
      socket_seen_ = true;
      charsrv_tick_ = GetTickCount();  // départ de l'auto-confirm char-server
    }

    // Char-select ATTEINT (liste de persos chargée) => le DRIVE de login est TERMINÉ.
    // On latche pour ne PLUS JAMAIS (de cette session) : (a) auto-confirmer — sinon
    // l'Entrée fuit dans notre char-select ImGui et déclenche une entrée en jeu parasite
    // (log « entrée en jeu slot=… » non voulu) ; (b) surveiller un échec — la socket
    // char-server tombe NORMALEMENT quand on entre en jeu, ce que la détection prenait
    // pour un login raté (dt>15s, socket=-1) -> faux kError -> reset authenticated_ ->
    // re-drive complet en boucle. On reste en passthrough idle ; CharSelect gère l'UI.
    // ⚠ Sonde = la FENÊTRE native du char-select, pas CharListLoaded() : les
    // CHARACTER_INFO survivent à un retour à l'écran de connexion (bouton « Revenir
    // au login »), donc la liste paraissait déjà chargée au login SUIVANT -> ce latch
    // tombait immédiatement, l'auto-confirmation ne tirait plus et le joueur restait
    // bloqué sur la fenêtre « Select Service » (choix du char-server, id 2). Les
    // fenêtres, elles, sont purgées à chaque changement d'état du mode.
    // 🔴 …ET l'écran natif de CRÉATION (0x116) : un compte SANS personnage
    // n'arrive jamais sur le char-select, le client va droit à l'état 8. Sans ce
    // second terme le latch ne tombait pas, l'auto-confirmation continuait de
    // poster des Entrées — dans un écran de saisie de nom — et la détection
    // d'échec restait armée sur une session pourtant établie.
    if (!charsel_reached_ && (native_login::CharSelectWindowPresent() ||
                              native_login::MakeCharWindowPresent()))
      charsel_reached_ = true;
    if (charsel_reached_) return;

    // ÉCHEC — testé AVANT l'auto-confirmation, pour que la moindre Entrée cesse
    // d'être postée dès qu'un refus est avéré (sinon elles tombent sur les boîtes
    // d'erreur natives et relancent des logins parasites).
    // Succès -> char-select (fenêtre login détruite). Échec (OTP refusé/timeout)
    // -> le mode revient à l'écran de login, la fenêtre UILoginWnd est RECRÉÉE.
    // Grâce ~1,2 s (destruction async) puis timeout dur si la socket ne monte pas.
    const unsigned long dt = GetTickCount() - fire_tick_;
    if (((dt > 1200) && native_login::LoginWindowPresent()) ||
        ((dt > 15000) && (native_login::SocketFd() == -1))) {
      LogDiag("[MoonlightAuth] ÉCHEC login détecté (dt={}ms, login_wnd={}, "
              "socket_fd={}, was_online={}, relogin_tries={})",
              dt, native_login::LoginWindowPresent(), native_login::SocketFd(),
              selected_online_, relogin_tries_);
      drove_moonlight_login_ = false;
      authenticated_ = false;
      fired_ = false;
      // Compte qui était encore en jeu (joueur ou autotrade) : ce refus était
      // ATTENDU. Le login-server valide l'OTP (et régénère donc le token, ce qui
      // brûle notre OTP) PUIS refuse avec le code 8 parce que le compte est dans
      // online_db, après avoir demandé aux char-servers de kicker la session. Le
      // kick est en cours : on attend, on redemande un OTP frais, on rejoue.
      if (selected_online_ && relogin_tries_ < kMaxRelogins &&
          !web_ticket_.empty()) {
        kick_wait_tick_ = GetTickCount();
        state_ = State::kKickWait;
        return;
      }
      error_msg_ =
          selected_online_
              ? i18n::Tr("La session précédente de ce compte ne s'est pas fermée à temps. "
                "Réessaie dans quelques secondes.") : i18n::Tr("Connexion refusée : compte encore connecté, OTP expiré ou "
                "serveur injoignable. Réessaie.");
      state_ = State::kError;  // re-masque le natif + réaffiche le formulaire
      return;
    }

    // Auto-confirmation du char-server. AC_ACCEPT_LOGIN 0x0ac4 amène le client à
    // l'état 6, qui CONSTRUIT la fenêtre id 2 « Select Service » (liste des
    // char-servers, boutons OK/cancel) : elle attend une validation, d'où cette
    // Entrée. On s'ARRÊTE dès que la fenêtre du char-select (0x115) est là ->
    // jamais d'Entrée au char-select.
    //
    // ⚠ Gate = la fenêtre id 2 elle-même, PAS « la fenêtre de login est absente ».
    // Ce dernier test ne prouvait rien : après un login REFUSÉ, la fenêtre de
    // login est détruite (OnMsg 0xBA l'a fermée au tir) et pas encore recréée
    // pendant que le client affiche sa boîte d'erreur — la rafale partait donc en
    // plein échec, chaque Entrée validant une popup puis relançant un login avec
    // un OTP déjà brûlé : cascade de « Incorrect User ID or Password ». Repli
    // temporel conservé (kCharSrvProbeMs) au cas où l'écran id 2 ne serait pas
    // construit sur certains parcours : la détection d'échec ci-dessus s'arme
    // avant (1,2 s), donc ce repli ne peut plus tirer sur un refus.
    //
    // ⚠ Rythme VOLONTAIREMENT lent (200 ms, 10 essais = 2 s de couverture, contre
    // 50 ms/20 essais avant) : chaque essai POSTE une Entrée dans la file Win32, que
    // le client consommera plus tard, sur l'écran qu'il aura atteint entre-temps. À
    // 50 ms on chargeait la file d'une vingtaine de munitions qui retombaient sur le
    // char-select. Le char-select ImGui s'en protège aussi de son côté (fenêtre
    // d'insensibilité à l'arrivée), mais mieux vaut ne pas les tirer du tout.
    // ⚠ Repli mesuré depuis le TIR (fire_tick_), pas depuis charsrv_tick_ : ce
    // dernier est réécrit à chaque Entrée postée, la condition serait donc
    // rearmée/désarmée par ses propres tirs.
    const bool login_accepted = native_login::CharServerWindowPresent() ||
                                (dt > kCharSrvProbeMs);
    if (socket_seen_ && login_accepted && !native_login::LoginWindowPresent() &&
        !native_login::CharSelectWindowPresent() && charsrv_tries_ < 10 &&
        (charsrv_tries_ == 0 || (GetTickCount() - charsrv_tick_) > 200)) {
      // ⚠ Canal MARQUÉ (PostGameKey) : cette Entrée-là vise la fenêtre native
      // « Select Service ». Postée nue, elle serait soit avalée par la capture
      // clavier, soit interprétée par notre char-select ImGui comme une frappe
      // du joueur — c'est ainsi qu'une entrée en jeu partait toute seule.
      RagnarokClient::PostGameKey(VK_RETURN);
      charsrv_tick_ = GetTickCount();
      ++charsrv_tries_;
    }
    return;
  }

  // Attente de la fermeture de la session précédente (kick demandé par le
  // login-server au refus), puis nouvel OTP et nouvel essai. Aucune UI focusable
  // n'est nécessaire ici : le spinner est dessiné par le chemin normal plus bas.
  if (state_ == State::kKickWait &&
      (GetTickCount() - kick_wait_tick_) > kKickWaitMs) {
    ++relogin_tries_;
    LogDiag("[MoonlightAuth] compte encore en ligne : nouvel OTP et re-login "
            "(essai {}/{})",
            relogin_tries_, kMaxRelogins);
    StartAccountSelect();  // -> kSelecting -> HandleSelectResponse -> kDriveLogin
  }

  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  if (disp.x <= 0.0f || disp.y <= 0.0f) return;  // garde minimize

  // Service-select (liste <connection>) : tant que la fenêtre de LOGIN n'est pas
  // là, on ne dessine PAS le formulaire (sinon il apparaît par-dessus le
  // service-select, qui capte alors le clavier). Avec une seule connexion il n'y a
  // pas de service-select : la fenêtre de login arrive directement.
  //
  // Franchissement : commit NATIF instantané de la connexion cible via
  // CLoginMode_SendMsg cmd 0x2723 (Apply_ClientInfoConnection -> état login). C'est
  // ce que déclenche l'Entrée clavier, mais direct et sans délai.
  // ⚠ Ne PAS confondre avec cmd 0x2713 : celui-là sélectionne le CHAR-SERVER
  // (post-login, table mode+0x1e8 peuplée par AC_ACCEPT_LOGIN) — c'était mon erreur
  // initiale, d'où les IP garbage et le « Failed to Connect ».
  // Repli clavier (éprouvé) si le natif n'a pas fait apparaître l'écran de login.
  // ⚠ `backdrop` en tête : par-dessus la capitale, la fenêtre de login native est
  // absente parce qu'il n'y a pas de mode login du tout, et non parce qu'un
  // service-select attendrait d'être franchi. Sans cette garde, on tirerait des
  // sélections de connexion dans le vide, en boucle, au lieu de dessiner le
  // formulaire.
  if (!backdrop && !native_login::LoginWindowPresent()) {
    // Init paresseuse au cas où OnModeSwitch n'aurait pas été émis à la 1ʳᵉ entrée.
    if (login_enter_tick_ == 0) login_enter_tick_ = GetTickCount();
    // Re-résoudre TANT QUE la liste est vide : la 1ʳᵉ résolution peut tomber avant
    // LoadClientInfoXml (arbre natif pas encore parsé, et sans data\clientinfo.xml
    // sur disque le repli ifstream échoue aussi). Dès que l'arbre devient lisible,
    // server_count_ est connu et on franchit sans attendre kSvcSelectProbeMs.
    if (server_count_ == 0 && !server_select_done_) ResolveServer();
    const unsigned long now = GetTickCount();
    // Écran de login absent alors qu'on est dans le mode login = service-select.
    // On tire dès que la liste est connue NON VIDE : observé live (2026-07-31),
    // même avec UNE seule connexion le client reste ~1,5 s sans UILoginWnd — le
    // 0x2723 mène à l'état login dans tous les cas, et re-poser l'état 3 est sans
    // effet si le natif y allait déjà. Si clientinfo reste illisible même
    // nativement, on tranche à l'usure : au-delà de kSvcSelectProbeMs sans
    // fenêtre de login, on franchit sur la connexion 0.
    const bool at_service_select =
        server_count_ > 0 || (now - login_enter_tick_) > kSvcSelectProbeMs;
    if (at_service_select && !server_select_done_) {
      if (native_login::SelectClientInfoConnection(server_index_)) {
        server_select_done_ = true;
        svc_select_tick_ = now;
        if (server_count_ == 0) {
          LogDiag("[MoonlightAuth] service-select tranché à l'usure après {} ms "
                  "(clientinfo illisible même nativement) -> franchi sur "
                  "l'index {}",
                  now - login_enter_tick_, server_index_);
        }
      }
    } else if (server_select_done_ && svc_select_tick_ != 0 && !svc_kbd_fallback_ &&
               (now - svc_select_tick_) > 1500) {
      // Toujours pas d'écran de login 1,5 s après le 0x2723 -> il n'a pas pris.
      LogDiag("[MoonlightAuth] service-select : 0x2723 sans effet -> REPLI clavier");
      DriveServerSelect();
      svc_kbd_fallback_ = true;
    }
    return;  // pas de formulaire tant qu'on n'est pas sur la fenêtre de login
  }

  // Formulaire moderne affiché : masquer la fenêtre de login NATIVE derrière lui
  // (réappliqué chaque frame : le mode peut la reconstruire). Réversible via
  // « Login classique » (MaskLoginWindow(false) ci-dessus).
  native_login::MaskLoginWindow(true);

  // Récupère un éventuel résultat HTTP avant de dessiner l'état correspondant.
  if (state_ == State::kAuthing || state_ == State::kSelecting ||
      state_ == State::kDiscordStart || state_ == State::kDiscordWait) {
    HttpResult r;
    if (TakeResult(&r)) {
      switch (state_) {
        case State::kAuthing:      HandleAuthResponse(r); break;
        case State::kSelecting:    HandleSelectResponse(r); break;
        case State::kDiscordStart: HandleDiscordStartResponse(r); break;
        case State::kDiscordWait:  HandleDiscordPollResponse(r); break;
        default: break;
      }
    }
  }

  // Polling Discord : navigateur ouvert, on interroge discord_poll par intervalles
  // jusqu'à résolution (kPickAccount), erreur, ou expiration (deadline TTL).
  if (state_ == State::kDiscordWait && !busy_.load()) {
    const unsigned long now = GetTickCount();
    if (discord_deadline_tick_ != 0 && now > discord_deadline_tick_) {
      error_msg_ =
          i18n::Tr("Connexion Discord expirée. Réessaie et valide dans le navigateur.");
      state_ = State::kError;
    } else if (now - discord_poll_tick_ >= discord_poll_interval_ms_) {
      discord_poll_tick_ = now;
      StartPost("action=discord_poll&game_session=" + UrlEncode(game_session_) +
                "&server=" + UrlEncode(server_name_));
    }
  }

  // La réponse /select a pu déclencher le pilotage natif : ne rien dessiner.
  if (state_ == State::kDriveLogin) return;

  // 🔴 Le décor va s'armer : on ne DESSINE pas, mais on a fait tout le reste.
  // Le formulaire apparaissait une poignée de frames avant que le voile ne se
  // pose, puis disparaissait dessous — un clignotement à l'ouverture du client.
  //
  // ⚠ Et c'est un `return` ICI, pas en tête de fonction. Tout ce qui précède
  // compte et doit continuer de tourner : le franchissement du service-select
  // (qui POSE L'ADRESSE DU SERVEUR, sans quoi plus personne ne se connecte — le
  // bug le plus coûteux de ce chantier), le masquage de la fenêtre native, les
  // réponses HTTP en vol. Se taire n'est pas s'arrêter.
  if (spectator::Pending()) return;

  // Force la capture CLAVIER tant que le formulaire est affiché : sinon les
  // touches (Entrée, flèches) fuient vers l'UI de login NATIVE en arrière-plan
  // (elle n'a pas d'InputText focus -> WantCaptureKeyboard resterait faux). Le
  // hook WndProc avale alors les entrées pour le jeu.
  //
  // ⚠ On NE force PAS la souris : `io.WantCaptureMouse` deviendrait vrai sur TOUT
  // l'écran, et login_parade ignore les clics quand il est vrai
  // (`IsMouseClicked && !WantCaptureMouse`) -> plus aucune interaction avec les
  // Porings. Inutile de toute façon : la fenêtre de login native est masquée
  // (+0x28 = 0 coupe le rendu ET le hit-test), donc un clic hors formulaire ne
  // peut rien atteindre ; et au survol du formulaire ImGui capture tout seul.
  ImGui::SetNextFrameWantCaptureKeyboard(true);

  // 🔴 `FirstUseEver` et non `Always` : reposée à CHAQUE frame, la position
  // annulait tout déplacement — la fenêtre revenait au centre sous le curseur du
  // joueur qui essayait de la bouger, et rien n'indiquait pourquoi. Elle
  // s'ouvre donc centrée, puis appartient au joueur. Le décor lui donne enfin une
  // raison de la déplacer : regarder la ville derrière.
  //
  // ⚠ Le pivot ne vaut plus que pour ce premier placement : `AlwaysAutoResize`
  // fait ensuite grandir la fenêtre vers le bas et la droite quand on passe du
  // formulaire au choix du compte. C'est le prix d'une fenêtre déplaçable, et il
  // est mince.
  ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  // ⚠ Plus de `NoSavedSettings` : la position déplacée par le joueur part dans
  // `imgui.ini` et lui revient au lancement suivant. `FirstUseEver` s'efface
  // devant elle — le centrage ne vaut donc que pour un premier lancement, ou
  // pour un `imgui.ini` neuf. Seule la position est rangée : `AlwaysAutoResize`
  // garde la main sur la taille.
  const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize;
  // 🔴 `TrId` et non `Tr`, et ce n'est pas cosmétique : ImGui range l'état d'une
  // fenêtre sous son TITRE. Traduit, le titre change — et avec lui l'identité de
  // la fenêtre, donc la position rangée. Le joueur qui place son formulaire puis
  // passe le client en anglais le retrouverait au centre, sans rien pour
  // l'expliquer. L'identifiant stable rend la position indépendante de la langue.
  if (ro::BeginRoWindow(
          i18n::TrId("Connexion Moonlight", "moonlight_auth_login"), nullptr,
          flags)) {
    switch (state_) {
      case State::kWebLogin:     DrawWebLogin(); break;
      case State::kAuthing:      DrawSpinner(i18n::Tr("Authentification…")); break;
      case State::kDiscordStart: DrawSpinner(i18n::Tr("Ouverture de Discord…")); break;
      case State::kDiscordWait:  DrawDiscordWait(); break;
      case State::kPickAccount:  DrawPickAccount(); break;
      case State::kSelecting:    DrawSpinner(i18n::Tr("Préparation du compte…")); break;
      case State::kKickWait:
        DrawSpinner(i18n::Tr("Fermeture de la session en cours sur ce compte…"));
        break;
      case State::kError:        DrawError(); break;
      default: break;
    }
    // HORS du switch, et volontairement en DERNIER : les combos restent
    // atteignables dans tous les états — y compris l'écran d'erreur, où un joueur
    // qui ne comprend pas le message a justement besoin d'en changer la langue.
    DrawLanguageAndFontPickers();

    // ── Réglages : son, skin, graphismes, échelle ────────────────────────────
    // Dans la BARRE DE TITRE, comme le bouton de rapport de bug : le corps de
    // cette fenêtre change de taille à chaque étape du login (formulaire, choix
    // du compte, erreur), et un bouton posé dedans se déplacerait sous le
    // curseur.
    //
    // 🔴 APRÈS TOUT LE RESTE, et cet ordre est une contrainte : `TitleBarButton`
    // ne restaure pas le curseur de layout, il doit donc être le dernier item
    // soumis à la fenêtre.
    //
    // ⚠ Pas pendant le PILOTAGE : à cet instant l'écran est couvert d'un voile,
    // et le joueur n'a rien à régler d'une séquence qui se déroule sans lui.
    if (auto* gs = Bourgeon::Instance().game_settings()) {
      if (!IsDrivingLoginActive() &&
          ro::TitleBarButton(
              i18n::Tr("Réglages"),
              i18n::Tr("Son, skin, réglages graphiques et taille de "
                       "l'interface"))) {
        gs->OpenFromLogin();
      }
    }
  }
  ro::EndRoWindow();
}

void MoonlightAuth::DrawWebLogin() {
  ImGui::TextUnformatted(i18n::Tr("Connecte-toi avec ton compte Moonlight"));
  ImGui::Spacing();

  ImGui::TextUnformatted(i18n::Tr("Identifiant"));
  ImGui::SetNextItemWidth(FormWidth());
  ImGui::InputText("##user", user_buf_, sizeof(user_buf_));

  ImGui::TextUnformatted(i18n::Tr("Mot de passe"));
  ImGui::SetNextItemWidth(FormWidth());
  const bool submit_pw = ImGui::InputText(
      "##pass", pass_buf_, sizeof(pass_buf_),
      ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);

  bool remember = remember_;
  if (ro::RoCheckbox(i18n::Tr("Se souvenir de moi"), &remember)) remember_ = remember;
  bool remember_pw = remember_pw_;
  if (ro::RoCheckbox(i18n::Tr("Se souvenir du mot de passe"), &remember_pw)) {
    remember_pw_ = remember_pw;
    if (remember_pw_)
      remember_ = true;  // le mot de passe mémorisé implique l'identifiant
    else
      DeleteFileA(PwPath().c_str());  // décoché -> on supprime tout de suite
  }

  ImGui::Spacing();
  const bool has_input = user_buf_[0] != '\0' && pass_buf_[0] != '\0';
  const bool click = ro::RoButton(i18n::Tr("Se connecter"), FormWidth(), 0.0f);
  // Entrée valide le formulaire quel que soit le champ focus (ou sans focus) :
  // `submit_pw` ne couvre que le champ mot de passe.
  const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                     ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
  if ((click || submit_pw || enter) && has_input) {
    const std::string form = "action=auth&username=" +
                             UrlEncode(user_buf_) + "&password=" +
                             UrlEncode(pass_buf_) + "&server=" +
                             UrlEncode(server_name_);
    StartPost(form);
    state_ = State::kAuthing;
  }

  // Connexion via Discord : ouvre le navigateur (OAuth2, cf. site). Même résultat
  // que le login web (liste de comptes), mais sans mot de passe à saisir.
  ImGui::Spacing();
  {
    const ButtonTex& d = DiscordButtonTex();
    bool discord = false;
    if (d.tex && d.w > 0) {
      // Cadre = bouton RO (skin 3-slice btn_*, états survol/clic), image Discord
      // dessinée CENTRÉE dedans à sa taille native (bornée à la largeur utile).
      // Le colorkey magenta de l'image laisse voir le skin RO autour du logo.
      const float pad = 6.0f;
      float iw = static_cast<float>(d.w);
      float ih = static_cast<float>(d.h);
      const float maxw = FormWidth() - pad * 2.0f;
      if (iw > maxw) { ih *= maxw / iw; iw = maxw; }
      const float btnh = ih + pad * 2.0f;
      discord = ro::RoButton("##discord_login", FormWidth(), btnh);
      const ImVec2 a = ImGui::GetItemRectMin();
      const ImVec2 b = ImGui::GetItemRectMax();
      const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
      // Coin haut-gauche arrondi au pixel entier + taille native exacte : chaque
      // texel tombe sur un pixel écran (échantillonnage 1:1) -> logo net, sans
      // flou ni bavure d'interpolation.
      const ImVec2 p0(static_cast<float>(static_cast<int>(c.x - iw * 0.5f)),
                      static_cast<float>(static_cast<int>(c.y - ih * 0.5f)));
      const ImVec2 p1(p0.x + iw, p0.y + ih);
      const float bri = ro::SkinImageBrightness();  // suit la luminosité du skin
      int cb = static_cast<int>(255.0f * bri);
      if (cb > 255) cb = 255;  // brightness peut aller à 2.0 -> borne IM_COL32
      ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)d.tex, p0, p1,
                                           ImVec2(0, 0), ImVec2(1, 1),
                                           IM_COL32(cb, cb, cb, 255));
    } else {
      // Repli texte si Discord.bmp est absent : teinte « blurple » Discord.
      ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(88, 101, 242, 255));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(105, 117, 245, 255));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(71, 82, 196, 255));
      discord = ro::RoButton(i18n::Tr("Se connecter avec Discord"), FormWidth(), 0.0f);
      ImGui::PopStyleColor(3);
    }
    if (discord) StartDiscordLogin();
  }

  // Plusieurs comptes Moonlight à regrouper ? Lien vers le service de fusion de
  // comptes du site (ouvre le navigateur). URL bâtie sur base_url_ (même origine
  // que les appels API) — pas d'hôte hardcodé.
  ImGui::Spacing();
  ImGui::TextDisabled("%s", i18n::Tr("Plusieurs comptes à regrouper ?"));
  ImGui::SameLine();
  HyperlinkOpen(i18n::Tr("Fusionner mes comptes"),
                base_url_ + "/ucp.php?i=moonlight&mode=merge");

  // Option secondaire : se connecter directement à un compte RO via le login
  // natif (login site oublié, préférence, etc.). Discrète, sous le bouton
  // principal — masque le formulaire moderne pour la session.
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextDisabled("%s", i18n::Tr("Tu préfères te connecter à un compte Ragnarok ?"));
  if (ro::RoSmallButton(i18n::Tr("Utiliser le login classique"))) {
    native_fallback_ = true;
    // 🔴 Et on ferme le décor. Ce repli rend la main aux champs NATIFS — qui
    // n'existent pas tant qu'une session spectateur tient l'écran : le joueur se
    // retrouverait devant une ville, sans formulaire d'aucune sorte. Sans effet
    // hors décor.
    spectator::Leave();
    // 🔴 Et on retire l'identifiant du décor du champ ID. C'est la séquence qui
    // l'y a écrit, et le client le REPOSE à chaque reconstruction de sa fenêtre
    // (case « Save ID ») : sans ça, le repli s'ouvre sur un formulaire déjà
    // rempli de `moonlight_spectator`, dont le mot de passe est ignoré — un
    // joueur qui valide entre en jeu sur le compte du décor.
    // ⚠ Le battement repasse derrière (spectator::ScrubNativePrefill) : la
    // fenêtre visée ici est celle d'AVANT la bascule de `Leave`, et celle qui la
    // remplacera portera la même valeur mémorisée.
    spectator::ScrubNativePrefill();
    native_login::MaskLoginWindow(false);  // réaffiche le natif (one-shot)
  }
}

void MoonlightAuth::DrawSpinner(const char* label) {
  // Petit indicateur animé simple (points).
  const int dots = static_cast<int>(ImGui::GetTime() * 3.0f) % 4;
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%s%.*s", label, dots, "...");
  ImGui::TextUnformatted(buf);
}

void MoonlightAuth::DrawDiscordWait() {
  DrawSpinner(i18n::Tr("En attente de Discord"));
  ImGui::Spacing();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + FormWidth());
  ImGui::TextWrapped(
      "%s", i18n::Tr("Termine la connexion dans ton navigateur, puis reviens ici : ta liste "
            "de comptes s'affichera automatiquement."));
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  // Ré-ouvrir la page si le joueur a fermé l'onglet par erreur.
  if (!discord_authorize_url_.empty() &&
      ro::RoSmallButton(i18n::Tr("Rouvrir la page Discord"))) {
    ShellExecuteA(nullptr, "open", discord_authorize_url_.c_str(), nullptr,
                  nullptr, SW_SHOWNORMAL);
  }
  ImGui::Spacing();
  if (ro::RoButton(i18n::Tr("Annuler"), FormWidth(), 0.0f)) {
    game_session_.clear();
    discord_authorize_url_.clear();
    state_ = State::kWebLogin;
  }
}

void MoonlightAuth::DrawPickAccount() {
  ImGui::TextUnformatted(i18n::Tr("Choisis un compte Ragnarok"));
  ImGui::TextDisabled("%s", i18n::Tr("Flèches pour naviguer, Entrée (ou double-clic) pour jouer."));
  ImGui::Separator();

  const int n = static_cast<int>(accounts_.size());
  bool confirm = false;

  // Navigation au clavier (Haut/Bas, avec répétition si la touche est maintenue).
  // Le formulaire force déjà la capture clavier, donc ces touches ne fuient pas
  // vers l'UI native. La liste boucle aux extrémités. On parcourt AUSSI les comptes
  // bannis : les sauter silencieusement rendrait la navigation imprévisible ; le
  // bouton « Jouer » se grise de lui-même, ce qui est plus explicite.
  if (n > 0) {
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
      selected_ = (selected_ < 0) ? 0 : (selected_ + 1) % n;
      pick_scroll_to_sel_ = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
      selected_ = (selected_ <= 0) ? n - 1 : selected_ - 1;
      pick_scroll_to_sel_ = true;
    }
  }

  // Liste à hauteur bornée (scrolle en interne au-delà de ~8) ; les boutons
  // restent toujours visibles sous la liste (plus besoin de scroller).
  const int vis = n < 8 ? n : 8;
  const float rowh = ImGui::GetTextLineHeightWithSpacing();
  const float listh = (vis > 0 ? vis : 1) * rowh + 8.0f;
  ImGui::BeginChild("##accts", ImVec2(FormWidth(), listh), true);
  for (int i = 0; i < n; ++i) {
    const Account& a = accounts_[i];
    ImGui::PushID(i);
    if (a.banned) ImGui::BeginDisabled();
    char line[192];
    // Un compte en autotrade est aussi « en ligne » : on n'affiche que le badge
    // le plus précis des deux.
    std::snprintf(line, sizeof(line), i18n::Tr("%s  (%d perso%s)%s%s%s%s"), a.label.c_str(),
                  a.char_count, a.char_count > 1 ? "s" : "",
                  a.banned ? "  [banni]" : "",
                  a.autotrade ? "  [autotrade]" : (a.online ? i18n::Tr("  [en ligne]") : ""),
                  a.last_login.empty() ? "" : "   ",
                  a.last_login.empty() ? "" : a.last_login.c_str());
    // Compte déjà connecté : sélectionnable (le joueur peut vouloir reprendre la
    // main) mais visuellement estompé, pour qu'on ne le choisisse pas par réflexe.
    // Teinte distincte pour l'autotrade (session marchande, pas un joueur actif).
    const bool busy_session = (a.online || a.autotrade) && !a.banned;
    if (busy_session)
      ImGui::PushStyleColor(ImGuiCol_Text, a.autotrade
                                               ? IM_COL32(120, 195, 165, 255)
                                               : IM_COL32(200, 170, 110, 255));
    if (ImGui::Selectable(line, selected_ == i,
                          ImGuiSelectableFlags_AllowDoubleClick)) {
      selected_ = i;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) confirm = true;
    }
    if (busy_session) ImGui::PopStyleColor();
    // Amène le compte sélectionné dans la vue : au 1er affichage, et à chaque
    // déplacement aux flèches (sinon la sélection sortirait de la zone visible).
    if (selected_ == i && (ImGui::IsWindowAppearing() || pick_scroll_to_sel_))
      ImGui::SetScrollHereY();
    if (a.banned) ImGui::EndDisabled();
    ImGui::PopID();
  }
  ImGui::EndChild();
  pick_scroll_to_sel_ = false;  // consommé

  ImGui::Spacing();
  const bool can_play =
      selected_ >= 0 && selected_ < n && !accounts_[selected_].banned;

  // Avertissement explicite : jouer un compte déjà connecté déconnecte l'autre
  // session (le serveur ne tolère qu'une connexion par compte). Cas autotrade :
  // ce qu'on perd est une boutique en cours de vente, on le dit tel quel.
  if (can_play && (accounts_[selected_].online || accounts_[selected_].autotrade)) {
    const bool is_autotrade = accounts_[selected_].autotrade;
    ImGui::PushStyleColor(ImGuiCol_Text, is_autotrade
                                             ? IM_COL32(140, 215, 185, 255)
                                             : IM_COL32(230, 190, 110, 255));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + FormWidth());
    ImGui::TextWrapped(
        "%s",
        is_autotrade
            ? i18n::Tr("Ce compte tient une boutique en autotrade : le choisir fermera "
              "la boutique et déconnectera le marchand.") : i18n::Tr("Ce compte est déjà connecté : le choisir déconnectera la session "
              "en cours."));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }

  // Entrée (ou pavé num.) = jouer le compte focus.
  if (can_play && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                   ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
    confirm = true;

  if (!can_play) ImGui::BeginDisabled();
  if (ro::RoButton(i18n::Tr("Jouer"), FormWidth(), 0.0f)) confirm = true;
  if (!can_play) ImGui::EndDisabled();

  if (ro::RoButton(i18n::Tr("Retour"), FormWidth(), 0.0f)) {
    accounts_.clear();
    selected_ = -1;
    web_ticket_.clear();
    state_ = State::kWebLogin;
    return;
  }

  if (confirm && can_play) {
    const Account& a = accounts_[selected_];
    // Mémorisé AVANT le départ : conditionne le rattrapage « session encore
    // ouverte » quand le premier essai se fera refuser (cf. kKickWait).
    selected_online_ = a.online || a.autotrade;
    relogin_tries_ = 0;
    StartAccountSelect();
  }
}

void MoonlightAuth::StartAccountSelect() {
  if (selected_ < 0 || selected_ >= static_cast<int>(accounts_.size())) return;
  char aid[32];
  std::snprintf(aid, sizeof(aid), "%ld", accounts_[selected_].account_id);
  const std::string form = "action=select&web_ticket=" + UrlEncode(web_ticket_) +
                           "&account_id=" + aid + "&server=" +
                           UrlEncode(server_name_);
  StartPost(form);
  state_ = State::kSelecting;
}

void MoonlightAuth::DrawError() {
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 110, 255));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + FormWidth());
  ImGui::TextWrapped("%s", error_msg_.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::Spacing();
  // Retour direct au choix du compte quand la session web tient encore : après un
  // refus « compte déjà connecté », rien ne justifie de retaper son mot de passe
  // Moonlight — le ticket signé suffit à redemander un OTP. (Ticket périmé =
  // /select répondra une erreur, on retombera ici.)
  if (!accounts_.empty() && !web_ticket_.empty() &&
      ro::RoButton(i18n::Tr("Choisir un compte"), FormWidth(), 0.0f)) {
    error_msg_.clear();
    state_ = State::kPickAccount;
  }
  if (ro::RoButton(i18n::Tr("Réessayer"), FormWidth(), 0.0f)) {
    error_msg_.clear();
    state_ = State::kWebLogin;
  }
  // Filet de sécurité (échec uniquement) : basculer sur le login natif pour la
  // session — utile si le site est en panne partielle alors que le jeu répond.
  if (ro::RoButton(i18n::Tr("Login classique"), FormWidth(), 0.0f)) {
    error_msg_.clear();
    native_fallback_ = true;
    // 🔴 Et on ferme le décor. Ce repli rend la main aux champs NATIFS — qui
    // n'existent pas tant qu'une session spectateur tient l'écran : le joueur se
    // retrouverait devant une ville, sans formulaire d'aucune sorte. Sans effet
    // hors décor.
    spectator::Leave();
    // 🔴 Et on retire l'identifiant du décor du champ ID. C'est la séquence qui
    // l'y a écrit, et le client le REPOSE à chaque reconstruction de sa fenêtre
    // (case « Save ID ») : sans ça, le repli s'ouvre sur un formulaire déjà
    // rempli de `moonlight_spectator`, dont le mot de passe est ignoré — un
    // joueur qui valide entre en jeu sur le compte du décor.
    // ⚠ Le battement repasse derrière (spectator::ScrubNativePrefill) : la
    // fenêtre visée ici est celle d'AVANT la bascule de `Leave`, et celle qui la
    // remplacera portera la même valeur mémorisée.
    spectator::ScrubNativePrefill();
    native_login::MaskLoginWindow(false);  // réaffiche le natif (one-shot)
  }
}

bool MoonlightAuth::ApplyAccountList(const HttpResult& r) {
  if (!r.error.empty()) {
    error_msg_ = i18n::Tr("Connexion au serveur impossible : ") + r.error;
    state_ = State::kError;
    return false;
  }
  try {
    const auto j = nlohmann::json::parse(r.body);
    if (r.status != 200 || !j.value("ok", false)) {
      error_msg_ = j.value("error", std::string(i18n::Tr("Authentification refusée.")));
      state_ = State::kError;
      return false;
    }
    web_ticket_ = j.value("web_ticket", std::string());
    accounts_.clear();
    for (const auto& e : j.value("accounts", nlohmann::json::array())) {
      Account a;
      a.account_id = e.value("account_id", 0L);
      a.userid = e.value("userid", std::string());
      a.label = e.value("label", a.userid);
      a.char_count = e.value("char_count", 0);
      a.last_login = e.value("last_login", std::string());
      a.banned = e.value("banned", false);
      a.online = e.value("online", false);
      a.autotrade = e.value("autotrade", false);
      if (a.autotrade) a.online = true;  // l'autotrade est une session en jeu
      accounts_.push_back(std::move(a));
    }
    if (accounts_.empty()) {
      error_msg_ = i18n::Tr("Aucun compte Ragnarok lié à ce compte Moonlight.");
      state_ = State::kError;
      return false;
    }
    // Range les comptes indisponibles EN BAS de la liste (tri stable, l'ordre
    // serveur est conservé à l'intérieur de chaque groupe) : d'abord ceux qu'on
    // peut jouer, puis ceux en autotrade (on ne perd qu'une boutique), puis ceux
    // dont un joueur tient la session, puis les bannis. Le multi-client reste
    // possible : ils sont relégués, pas interdits.
    std::stable_sort(accounts_.begin(), accounts_.end(),
                     [](const Account& lhs, const Account& rhs) {
                       auto rank = [](const Account& a) {
                         if (a.banned) return 3;
                         if (a.autotrade) return 1;
                         return a.online ? 2 : 0;
                       };
                       return rank(lhs) < rank(rhs);
                     });
    // Pré-sélectionne le compte joué le plus récemment (last_login "YYYY-MM-DD"
    // → max lexicographique) PARMI ceux réellement jouables, sinon le premier.
    // Ainsi « Entrée » suffit, et jamais sur un compte déjà connecté.
    selected_ = 0;
    for (int i = 0; i < static_cast<int>(accounts_.size()); ++i) {
      const Account& candidate = accounts_[i];
      if (candidate.banned || candidate.online) continue;
      if (accounts_[selected_].banned || accounts_[selected_].online ||
          candidate.last_login > accounts_[selected_].last_login)
        selected_ = i;
    }
    pass_buf_[0] = '\0';
    state_ = State::kPickAccount;
    return true;
  } catch (const std::exception&) {
    error_msg_ = i18n::Tr("Réponse du serveur illisible.");
    state_ = State::kError;
    return false;
  }
}

void MoonlightAuth::HandleAuthResponse(const HttpResult& r) {
  // Seul l'échec est loggué : sur succès le body porte le web_ticket et la liste
  // des comptes, qui n'ont rien à faire dans bourgeon.log.
  if (!r.error.empty() || r.status != 200)
    LogDiag("[MoonlightAuth] /auth -> status={} err='{}' body='{}'", r.status,
            r.error, r.body);
  // Mémorise l'identifiant web et, si l'utilisateur l'a demandé, le mot de passe du
  // compte Moonlight (chiffré DPAPI) — jamais l'OTP de jeu.
  // ApplyAccountList efface pass_buf_ dès que l'authentification a réussi (le mot de
  // passe ne reste pas en mémoire) : on en prend donc une copie AVANT l'appel, sinon
  // SavePref ne verrait qu'un champ vide. La copie est effacée aussitôt après.
  char password_entered[sizeof(pass_buf_)];
  std::snprintf(password_entered, sizeof(password_entered), "%s", pass_buf_);
  if (ApplyAccountList(r)) SavePref(password_entered);
  SecureZeroMemory(password_entered, sizeof(password_entered));
}

void MoonlightAuth::StartDiscordLogin() {
  // Ouvre une session de login Discord côté site : il renvoie l'URL OAuth à
  // ouvrir dans le navigateur + un id de session à interroger (poll). Le client
  // ne fait JAMAIS l'OAuth lui-même (navigateur + client_secret serveur requis).
  game_session_.clear();
  discord_authorize_url_.clear();
  error_msg_.clear();
  StartPost("action=discord_start&server=" + UrlEncode(server_name_));
  state_ = State::kDiscordStart;
}

void MoonlightAuth::HandleDiscordStartResponse(const HttpResult& r) {
  if (!r.error.empty() || r.status != 200)
    LogDiag("[MoonlightAuth] discord_start -> status={} err='{}' body='{}'",
            r.status, r.error, r.body);
  if (!r.error.empty()) {
    error_msg_ = i18n::Tr("Connexion au serveur impossible : ") + r.error;
    state_ = State::kError;
    return;
  }
  try {
    const auto j = nlohmann::json::parse(r.body);
    if (r.status != 200 || !j.value("ok", false)) {
      error_msg_ =
          j.value("error", std::string(i18n::Tr("Impossible de démarrer la connexion Discord.")));
      state_ = State::kError;
      return;
    }
    game_session_ = j.value("game_session", std::string());
    // Le serveur renvoie un chemin RELATIF (oauth_discord.php est à la racine du
    // domaine, PAS sous le board phpBB /forum/). On préfixe avec notre base_url_
    // (l'origine exacte de nos appels API) = source de vérité unique de l'hôte.
    const std::string path = j.value("authorize_path", std::string());
    if (game_session_.empty() || path.empty()) {
      error_msg_ = i18n::Tr("Réponse incomplète du serveur.");
      state_ = State::kError;
      return;
    }
    discord_authorize_url_ =
        base_url_ + (path[0] == '/' ? path : ("/" + path));
    const int interval = j.value("poll_interval", 2);
    const int ttl = j.value("ttl", 180);
    discord_poll_interval_ms_ = static_cast<unsigned long>((interval > 0 ? interval : 2) * 1000);
    discord_deadline_tick_ =
        GetTickCount() + static_cast<unsigned long>((ttl > 0 ? ttl : 180) * 1000);
    discord_poll_tick_ = GetTickCount();  // 1er poll après un intervalle
    // Ouvre le navigateur système sur la page d'autorisation Discord. ⚠ peut
    // faire perdre le focus au jeu plein écran (Alt-Tab) — comportement attendu.
    ShellExecuteA(nullptr, "open", discord_authorize_url_.c_str(), nullptr,
                  nullptr, SW_SHOWNORMAL);
    state_ = State::kDiscordWait;
  } catch (const std::exception&) {
    error_msg_ = i18n::Tr("Réponse du serveur illisible.");
    state_ = State::kError;
  }
}

void MoonlightAuth::HandleDiscordPollResponse(const HttpResult& r) {
  // Erreur transport transitoire (perte réseau ponctuelle) : NE bascule PAS en
  // erreur — le prochain intervalle réessaiera tant que la deadline n'est pas
  // atteinte (gérée dans OnRenderLoginUI).
  if (!r.error.empty()) {
    LogDiag("[MoonlightAuth] discord_poll transport err='{}' (on réessaie)", r.error);
    return;
  }
  try {
    const auto j = nlohmann::json::parse(r.body);
    if (r.status == 410 || r.status != 200) {
      error_msg_ = j.value("error", std::string(i18n::Tr("Session Discord expirée. Réessaie.")));
      state_ = State::kError;
      return;
    }
    if (!j.value("ok", false)) {
      error_msg_ = j.value("error", std::string(i18n::Tr("Connexion Discord refusée.")));
      state_ = State::kError;
      return;
    }
    if (j.value("pending", false)) return;  // pas encore validé -> continue le poll
    // Résolu : la réponse porte web_ticket + accounts (comme /auth).
    ApplyAccountList(r);
  } catch (const std::exception&) {
    // JSON illisible ponctuel : on réessaie plutôt que d'échouer sec.
    LogDiag("[MoonlightAuth] discord_poll JSON illisible (on réessaie)");
  }
}

void MoonlightAuth::HandleSelectResponse(const HttpResult& r) {
  // Idem /auth : sur succès le body porte l'OTP de jeu — jamais dans le log.
  if (!r.error.empty() || r.status != 200)
    LogDiag("[MoonlightAuth] /select -> status={} err='{}' body='{}'", r.status,
            r.error, r.body);
  if (!r.error.empty()) {
    error_msg_ = i18n::Tr("Connexion au serveur impossible : ") + r.error;
    state_ = State::kError;
    return;
  }
  try {
    const auto j = nlohmann::json::parse(r.body);
    if (r.status != 200 || !j.value("ok", false)) {
      error_msg_ = j.value("error", std::string(i18n::Tr("Sélection du compte refusée.")));
      state_ = State::kError;
      return;
    }
    const std::string userid = j.value("userid", std::string());
    const std::string otp = j.value("otp", std::string());
    if (userid.empty() || otp.empty()) {
      error_msg_ = i18n::Tr("Réponse incomplète du serveur.");
      state_ = State::kError;
      return;
    }
    // Login natif SANS frappe : on mémorise les credentials ; le déclenchement
    // (SetText des 2 champs + OnMsg du bouton Start) se fait edge-triggered dans
    // OnRenderLoginUI (voie seamless, cf. native_login.h). Le mode natif enchaîne
    // ensuite connect -> CA_LOGIN -> char-server -> char-select tout seul.
    drive_user_ = userid;
    drive_pw_ = otp;
    fired_ = false;
    socket_seen_ = false;
    charsrv_tries_ = 0;
    drove_moonlight_login_ = true;  // -> autorise le char-select ImGui
    authenticated_ = true;          // survit au retour char-select (pas de re-auth)
    state_ = State::kDriveLogin;
  } catch (const std::exception&) {
    error_msg_ = i18n::Tr("Réponse du serveur illisible.");
    state_ = State::kError;
  }
}

// Cf. l'en-tête pour le pourquoi.
//
// ⚠ Lit un état de FICHIER et non le plugin, délibérément : ce module ne connaît
// pas `Bourgeon`, et les trois appelants sont des liens d'interface qui doivent
// répondre même si le plugin n'a pas encore été construit. `LoadConfig` y publie
// l'adresse retenue ; avant elle, c'est l'adresse d'usine.
const char* SiteBaseUrl() {
  return g_site_base.empty() ? kDefaultBaseUrl : g_site_base.c_str();
}
