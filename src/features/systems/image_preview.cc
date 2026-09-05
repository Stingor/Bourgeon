#include "features/systems/image_preview.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB / _DeviceEpoch
#include "utils/img_decode.h"  // imgdec:: — WIC, composition des gifs, réduction
#include "utils/log_console.h"
#include "utils/text.h"  // text::ToLowerAscii / ContainsNoCase

#pragma comment(lib, "winhttp.lib")

namespace imgprev {
namespace {

// ── Les bornes. On décode une donnée hostile : chacune évite un dégât précis ──
constexpr size_t kMaxDownload   = 8u * 1024u * 1024u;  // 8 Mio : au-delà, on renonce
constexpr int    kMaxSourceDim  = 16384;  // garde-fou d'en-tête (image « zip bomb »)
// Taille d'AFFICHAGE. On réduit au décodage plutôt que de garder la pleine
// résolution : c'est tout ce qu'on montrera, et la VRAM d'un client RO n'est pas
// extensible. Une capture 4K devient 512 px sans qu'on ait jamais alloué 4K.
constexpr int    kMaxPreviewDim = 512;
constexpr size_t kMaxCached     = 24;  // aperçus gardés en mémoire
constexpr int    kMaxInFlight   = 3;   // téléchargements simultanés
// (Les plafonds passés au décodeur sont posés plus bas, avec imgdec::Limits.)

// ── La liste blanche ─────────────────────────────────────────────────────────
// Par défaut, les hébergeurs sur lesquels les joueurs postent réellement. Un
// point initial = suffixe de domaine.
//
// 🔴 Ce n'est PAS une liste de « sites de confiance » au sens moral : c'est la
// liste des serveurs que le POSTEUR NE CONTRÔLE PAS. C'est ça qui protège — il
// ne peut rien apprendre de qui a regardé son image.
const char* const kDefaultHosts[] = {
    ".discordapp.com",  // cdn.discordapp.com, media.discordapp.com
    ".discordapp.net",  // media.discordapp.net (proxy d'images Discord)
    ".discord.com",
    ".imgur.com",
    "imgur.com",
    ".moonlight-destiny.fr",
    "moonlight-destiny.fr",
};

std::mutex               g_mutex;       // protège tout ce qui suit
std::vector<std::string> g_hosts;       // liste intégrée (ou celle du serveur)
std::vector<std::string> g_user_hosts;  // celle du JOUEUR, révocable
// Adresses autorisées à l'unité par un geste explicite. Pas d'hôte, pas de
// persistance : une seule adresse, une seule fois.
std::vector<std::string> g_allow_once;
int                      g_in_flight = 0;

// Un pixel décodé, en attente de devenir une texture (la création D3D ne se fait
// que sur le thread principal).
struct Decoded {
  std::string url;
  // Une image fixe = UNE entrée ; un GIF animé, N canevas déjà composés.
  std::vector<std::vector<uint8_t>> frames;
  std::vector<int>                  delays;  // ms, même taille que `frames`
  int                               w = 0;
  int                               h = 0;
  bool                              ok = false;
};
std::vector<Decoded> g_finished;

struct Entry {
  Preview::State     state = Preview::kNone;
  std::vector<void*> texs;    // 1 = fixe, N = animé
  std::vector<int>   delays;  // ms
  int      total_ms = 0;      // durée d'un tour (0 = fixe)
  DWORD    t0 = 0;            // origine de l'horloge d'animation
  int      w = 0, h = 0;
  uint64_t used = 0;  // compteur d'usage, pour évincer le plus ancien
};

// Libère TOUTES les textures d'une entrée. Centralisé : il y a quatre endroits
// où une entrée meurt (éviction, révocation d'hôte, remplacement, échec), et
// n'en libérer qu'une sur N fuirait en silence.
void ReleaseEntry(Entry* e) {
  for (void* t : e->texs)
    if (t) Overlay_ReleaseTexture(t);
  e->texs.clear();
  e->delays.clear();
  e->total_ms = 0;
}
std::unordered_map<std::string, Entry> g_cache;
uint64_t g_use_counter = 0;

// ── Conversions large <-> étroit, explicites ─────────────────────────────────
// `std::string(w.begin(), w.end())` tronque chaque wchar_t en char : juste sur de
// l'ASCII, faux au premier caractère au-delà, et C4244 à la compilation. Une
// adresse vient ici de texte arbitraire (og:image, en-tête Location) : on passe
// donc par un vrai transcodage UTF-8.
std::string Narrow(const wchar_t* w) {
  if (w == nullptr || w[0] == L'\0') return std::string();
  const int need =
      WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (need <= 1) return std::string();
  std::string out(static_cast<size_t>(need - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], need, nullptr, nullptr);
  return out;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                       static_cast<int>(s.size()), nullptr, 0);
  if (need <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(need), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                      &out[0], need);
  return out;
}

// L'hôte d'une adresse (« https://cdn.discordapp.com/x.png » -> « cdn.discordapp.com »).
// Vide si l'adresse n'a pas de forme exploitable.
std::string HostOf(const std::string& url) {
  size_t start = url.find("://");
  start = (start == std::string::npos) ? 0 : start + 3;
  size_t end = url.size();
  for (size_t i = start; i < url.size(); ++i) {
    const char c = url[i];
    if (c == '/' || c == '?' || c == '#' || c == ':') { end = i; break; }
  }
  if (end <= start) return std::string();
  // Un « @ » avant l'hôte (userinfo) permettrait « https://cdn.discordapp.com@evil.tld/ » :
  // l'hôte réel serait evil.tld alors que l'œil lit Discord. On REFUSE la forme.
  const std::string authority = url.substr(start, end - start);
  if (authority.find('@') != std::string::npos) return std::string();
  return text::ToLowerAscii(authority);
}

bool MatchesList(const std::string& host,
                 const std::vector<std::string>& list) {
  for (const std::string& rule : list) {
    if (rule.empty()) continue;
    if (rule[0] == '.') {
      if (host.size() > rule.size() &&
          host.compare(host.size() - rule.size(), rule.size(), rule) == 0)
        return true;
    } else if (host == rule) {
      return true;
    }
  }
  return false;
}

bool HostAllowed(const std::string& host) {
  if (host.empty()) return false;
  return MatchesList(host, g_hosts) || MatchesList(host, g_user_hosts);
}

void EnsureHosts() {
  if (!g_hosts.empty()) return;
  for (const char* h : kDefaultHosts) g_hosts.push_back(h);
}

// Version verrouillée, pour les threads worker (qui ne tiennent pas g_mutex).
bool HostAllowedSafe(const std::string& host) {
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureHosts();
  return HostAllowed(host);
}

// ── og:image : l'image d'une PAGE ────────────────────────────────────────────
//
// Beaucoup de liens d'images n'en sont pas : « klipy.com/gifs/disco-100 » est une
// PAGE qui contient un gif, sans extension et servie en text/html. C'est la forme
// que prennent la plupart des hébergeurs de gifs, et c'est ainsi que Discord
// lui-même fabrique ses aperçus : en lisant la balise OpenGraph de la page.
//
// Cherche <meta property="og:image" content="..."> en tolérant l'ordre des
// attributs. Volontairement rustique : on ne parse pas du HTML, on repère une
// balise et on en extrait un champ, sur un extrait borné à quelques dizaines de
// kio (les balises og vivent dans le <head>).
bool ExtractOgImage(const std::vector<uint8_t>& html, std::string* out) {
  const std::string s = text::ToLowerAscii(
      std::string(reinterpret_cast<const char*>(html.data()),
                  std::min<size_t>(html.size(), 64u * 1024u)));
  const size_t marker = s.find("og:image");
  if (marker == std::string::npos) return false;
  // Bornes de la balise qui contient le marqueur : l'attribut `content` peut
  // précéder comme suivre `property`, mais il est forcément dans le même <meta>.
  const size_t open  = s.rfind('<', marker);
  size_t close = s.find('>', marker);
  if (open == std::string::npos || close == std::string::npos) return false;
  const size_t ctag = s.find("content", open);
  if (ctag == std::string::npos || ctag > close) return false;
  size_t q = s.find_first_of("\"'", ctag);
  if (q == std::string::npos || q > close) return false;
  const char quote = s[q];
  const size_t end = s.find(quote, q + 1);
  if (end == std::string::npos || end > close) return false;
  // Recopié depuis l'ORIGINAL, pas depuis la version minusculée : une adresse
  // peut porter une casse significative (jetons de signature de CDN).
  *out = std::string(reinterpret_cast<const char*>(html.data()) + q + 1,
                     end - q - 1);
  // Seule entité vraiment courante dans une URL d'og:image.
  for (size_t p = out->find("&amp;"); p != std::string::npos;
       p = out->find("&amp;", p + 1))
    out->replace(p, 5, "&");
  return !out->empty();
}

// ── Téléchargement (thread worker) ───────────────────────────────────────────
// Un seul saut. Rend false dès que quelque chose sort des clous : schéma, taille,
// transport. `out_status` et `out_ctype` sont renseignés même sur un 3xx, pour que
// l'appelant décide de suivre — ou non — la redirection.
bool DownloadOnce(const std::string& url, size_t max_bytes,
                  std::vector<uint8_t>* out, DWORD* out_status,
                  std::string* out_ctype, std::string* out_location) {
  const std::wstring wurl = Widen(url);
  URL_COMPONENTS uc;
  ZeroMemory(&uc, sizeof(uc));
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {0};
  wchar_t path[2048] = {0};
  uc.lpszHostName = host;      uc.dwHostNameLength = 255;
  uc.lpszUrlPath  = path;      uc.dwUrlPathLength  = 2047;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
  // http:// en clair révélerait l'adresse au réseau traversé ; les hébergeurs de
  // la liste sont tous en HTTPS de toute façon.
  if (uc.nScheme != INTERNET_SCHEME_HTTPS) return false;

  // ── L'agent utilisateur ──────────────────────────────────────────────────────
  // « Bourgeon/ImagePreview » se faisait refuser en 403 par les sites protégés
  // (klipy, mesuré) : beaucoup rejettent ce qui ne ressemble pas à un navigateur.
  //
  // On reste IDENTIFIABLE plutôt que de se déguiser : le jeton `Mozilla/5.0
  // (compatible; ...)` est la forme conventionnelle d'un client non-navigateur
  // honnête, et l'URL permet à un administrateur de savoir à qui il a affaire s'il
  // veut nous bloquer ou nous laisser passer. Un site qui refuse quand même a pris
  // une décision qu'on respecte : l'aperçu échoue proprement, et c'est tout.
  static const wchar_t kUserAgent[] =
      L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
      L"(compatible; Bourgeon/1.0; +https://moonlight-destiny.fr)";
  HINTERNET hs = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hs) return false;
  WinHttpSetTimeouts(hs, 5000, 5000, 8000, 8000);

  bool ok = false;
  HINTERNET hc = WinHttpConnect(hs, host, uc.nPort, 0);
  HINTERNET hr = nullptr;
  if (hc) {
    hr = WinHttpOpenRequest(hc, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  }
  // 🔴🔴 REDIRECTIONS DÉSARMÉES. WinHttp les suit TOUT SEUL par défaut — et cela
  // suffisait à annuler toute la liste blanche : une adresse sur un hôte autorisé
  // répondant « 302 vers evil.tld » aurait été suivie en silence, livrant
  // exactement l'IP que ce module existe pour protéger. On les traite donc à la
  // main, en revérifiant l'hôte à CHAQUE saut (cf. FetchImage).
  if (hr) {
    DWORD disable = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(hr, WINHTTP_OPTION_DISABLE_FEATURE, &disable,
                     sizeof(disable));
  }
  // Sans `Accept`, WinHttp n'en envoie aucun — et un serveur qui négocie le
  // contenu peut alors répondre autre chose qu'une image, voire refuser.
  static const wchar_t kHeaders[] =
      L"Accept: image/avif,image/webp,image/*,text/html;q=0.8,*/*;q=0.5\r\n"
      L"Accept-Language: fr,en;q=0.8";
  if (hr && WinHttpSendRequest(hr, kHeaders, static_cast<DWORD>(-1),
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(hr, nullptr)) {
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    *out_status = status;

    // Le serveur DIT-IL que c'est une image ? L'extension de l'adresse, elle, ne
    // prouve rien — et bien des liens d'images n'en ont aucune.
    wchar_t ctype[256] = {0};
    DWORD   ctype_len = sizeof(ctype);
    if (WinHttpQueryHeaders(hr, WINHTTP_QUERY_CONTENT_TYPE,
                            WINHTTP_HEADER_NAME_BY_INDEX, ctype, &ctype_len,
                            WINHTTP_NO_HEADER_INDEX)) {
      *out_ctype = Narrow(ctype);
    }
    // Cible d'une redirection : rendue à l'appelant SANS être suivie.
    wchar_t loc[2048] = {0};
    DWORD   loc_len = sizeof(loc);
    if (status >= 300 && status < 400 &&
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_LOCATION,
                            WINHTTP_HEADER_NAME_BY_INDEX, loc, &loc_len,
                            WINHTTP_NO_HEADER_INDEX)) {
      *out_location = Narrow(loc);
    }

    if (status == 200) {
      ok = true;
      DWORD avail = 0;
      while (WinHttpQueryDataAvailable(hr, &avail) && avail > 0) {
        if (out->size() + avail > max_bytes) { ok = false; break; }
        const size_t base = out->size();
        out->resize(base + avail);
        DWORD read = 0;
        if (!WinHttpReadData(hr, out->data() + base, avail, &read)) {
          ok = false;
          break;
        }
        out->resize(base + read);
        if (read == 0) break;
      }
    }
  }
  if (hr) WinHttpCloseHandle(hr);
  if (hc) WinHttpCloseHandle(hc);
  WinHttpCloseHandle(hs);
  if (!ok) out->clear();
  return ok;
}

// L'octet-à-octet de l'image, en suivant ce qu'il faut suivre — et rien d'autre.
//
// Deux résolutions possibles, chacune bornée et chacune re-soumise à la liste
// blanche :
//   • REDIRECTION (3xx) : on lit `Location` nous-mêmes et on revérifie l'hôte.
//   • PAGE HTML : on y cherche `og:image` (c'est la forme des hébergeurs de gifs
//     comme klipy/tenor, qui servent une page et non un fichier), et l'adresse
//     trouvée doit ELLE AUSSI être sur un hôte autorisé — sans quoi une page
//     dont le contenu est fourni par un utilisateur pourrait pointer l'aperçu
//     vers le traceur de son choix.
bool FetchImage(const std::string& start_url, std::vector<uint8_t>* out) {
  constexpr int    kMaxHops = 3;   // redirections + saut og:image
  constexpr size_t kMaxHtml = 256u * 1024u;

  // Autorisation à l'unité : le joueur a explicitement demandé CETTE image. On ne
  // resoumet donc pas ses sauts à la liste — il a consenti à contacter cette
  // ressource, exactement comme s'il avait cliqué. C'est borné à cette adresse et
  // à cette fois-ci ; rien n'en est retenu.
  bool one_shot = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const std::string& u : g_allow_once)
      if (u == start_url) { one_shot = true; break; }
  }

  std::string url = start_url;
  bool html_hop_used = false;

  for (int hop = 0; hop < kMaxHops; ++hop) {
    if (!one_shot && !HostAllowedSafe(HostOf(url))) {  // à CHAQUE saut
      // 🔴 Le cas le plus probable après un accord d'hôte : la PAGE est autorisée
      // mais son og:image vit sur un CDN différent, qui ne l'est pas. Sans cette
      // trace, l'échec est un « Aperçu indisponible » muet, indiscernable d'un
      // site en panne.
      LogDiag("[imgprev] saut {} refuse : hote '{}' hors liste ({})", hop,
              HostOf(url), url);
      return false;
    }

    DWORD       status = 0;
    std::string ctype, location;
    out->clear();
    const bool got = DownloadOnce(url, kMaxDownload, out, &status, &ctype,
                                  &location);

    if (status >= 300 && status < 400 && !location.empty()) {
      url = location;          // l'hôte sera revérifié en tête de boucle
      continue;
    }
    if (!got) {
      LogDiag("[imgprev] echec HTTP {} sur {}", status, url);
      return false;
    }

    const std::string lower = text::ToLowerAscii(ctype);
    if (lower.compare(0, 6, "image/") == 0) return true;

    // Une PAGE : une seule tentative og:image, jamais en chaîne.
    if (!html_hop_used && lower.compare(0, 9, "text/html") == 0 &&
        out->size() <= kMaxHtml) {
      std::string img;
      if (!ExtractOgImage(*out, &img)) {
        // Page rendue par du JavaScript, ou balise absente : il n'y a rien à
        // trouver dans le HTML initial, et on ne va pas exécuter de script.
        LogDiag("[imgprev] pas d'og:image dans la page ({} o) : {}",
                out->size(), url);
        return false;
      }
      LogDiag("[imgprev] og:image -> {}", img);
      url = img;
      html_hop_used = true;
      continue;
    }
    LogDiag("[imgprev] type refuse '{}' ({} o) : {}", ctype, out->size(), url);
    return false;
  }
  LogDiag("[imgprev] trop de sauts : {}", start_url);
  return false;
}

// ── Décodage (thread worker) ────────────────────────────────────
// Le décodage lui-même vit dans `utils/img_decode` : WIC, la composition des
// images d'un gif animé et la réduction y sont écrits UNE fois, pour ici et pour
// le tutoriel. Ne reste ici que ce qui est propre à l'aperçu — ses BORNES, plus
// serrées que celles d'un gif qu'on livre soi-même, parce que ce qu'on décode
// vient d'un tiers.

// Image fixe : réduite à la taille d'affichage. Une capture 4K devient 512 px
// sans qu'on ait jamais alloué 4K.
// (Les deux derniers champs de `Limits` ne concernent que l'animation.)
const imgdec::Limits kStillLimits = {kMaxSourceDim, kMaxPreviewDim, 0, 0};

// Animation : 256 px et 12 Mio de pixels gardés. L'aperçu s'affiche dans une
// infobulle — la vignette fait la hauteur d'une ligne de chat, et payer plus
// large en VRAM n'achèterait rien de visible.
const imgdec::Limits kAnimLimits = {kMaxSourceDim, 256, 60, 12u * 1024u * 1024u};

// Le thread de travail d'UNE adresse. Rien de D3D ici : il ne produit que des
// pixels, que `Tick` transformera en texture sur le thread principal.
void FetchWorker(std::string url) {
  // WIC est du COM : chaque thread doit l'initialiser pour lui-même.
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  Decoded done;
  done.url = url;
  std::vector<uint8_t> bytes;
  if (FetchImage(url, &bytes)) {
    // Animé d'abord ; sinon l'image fixe, qui elle sait aussi RÉDUIRE les grandes
    // images — d'où l'ordre : un GIF trop grand pour être animé y retombe et
    // s'affiche quand même, figé, plutôt que d'échouer.
    imgdec::Animation anim;
    if (imgdec::DecodeAnimation(bytes.data(), bytes.size(), kAnimLimits, &anim)) {
      done.frames = std::move(anim.frames);
      done.delays = std::move(anim.delays_ms);
      done.w      = anim.w;
      done.h      = anim.h;
      done.ok     = true;
    }
    if (!done.ok) {
      std::vector<uint8_t> single;
      if (imgdec::DecodeStill(bytes.data(), bytes.size(), kStillLimits, &single,
                              &done.w, &done.h)) {
        done.frames.push_back(std::move(single));
        done.delays.push_back(0);
        done.ok = true;
      }
    }
  }

  if (SUCCEEDED(co)) CoUninitialize();

  std::lock_guard<std::mutex> lock(g_mutex);
  g_finished.push_back(std::move(done));
  --g_in_flight;
}

// Purge après un reset de device : les textures D3DPOOL_DEFAULT sont DÉJÀ mortes,
// on jette les handles sans rien libérer (les libérer planterait). Même règle que
// ro::ItemIcon.
void DropOnDeviceReset() {
  static Overlay_DeviceEpochWatch s_watch;
  if (!s_watch.Changed()) return;
  g_cache.clear();
}

}  // namespace

bool IsPreviewable(const char* url) {
  if (url == nullptr || url[0] == '\0') return false;
  // 🔴 Plus d'exigence d'EXTENSION. Elle paraissait prudente et elle était juste
  // fausse : « klipy.com/gifs/disco-100 » est une page qui contient un gif, et
  // c'est la forme de la plupart des hébergeurs. Le seul verrou de sécurité est
  // l'HÔTE ; ce qu'on a réellement reçu, c'est le `Content-Type` qui le dit, et
  // un échec est mis en cache — on ne redemande pas.
  const std::string s(url);
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureHosts();
  for (const std::string& u : g_allow_once)
    if (u == s) return true;  // demandée à l'unité par le joueur
  return HostAllowed(HostOf(s));
}

void Request(const char* url) {
  if (!IsPreviewable(url)) return;
  const std::string key(url);

  std::lock_guard<std::mutex> lock(g_mutex);
  auto found = g_cache.find(key);
  if (found != g_cache.end()) {
    found->second.used = ++g_use_counter;
    return;  // déjà connu (prêt, en cours, ou échoué : on ne réessaie pas)
  }
  if (g_in_flight >= kMaxInFlight) return;  // on retentera au prochain survol

  Entry e;
  e.state = Preview::kPending;
  e.used  = ++g_use_counter;
  g_cache.emplace(key, e);
  ++g_in_flight;
  std::thread(FetchWorker, key).detach();
}

Preview Get(const char* url) {
  Preview p;
  if (url == nullptr || url[0] == '\0') return p;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto found = g_cache.find(std::string(url));
  if (found == g_cache.end()) return p;
  Entry& e = found->second;
  e.used = ++g_use_counter;
  p.state = e.state;
  p.w     = e.w;
  p.h     = e.h;
  if (e.texs.empty()) return p;
  // Image FIXE : la première, toujours. ANIMÉE : celle où en est l'horloge.
  // Le temps est lu ICI plutôt que confié à l'appelant — le gif doit tourner à sa
  // vitesse propre, pas à celle du rendu ni du nombre de survols.
  if (e.total_ms <= 0 || e.delays.size() != e.texs.size()) {
    p.tex = e.texs[0];
    return p;
  }
  DWORD elapsed = (GetTickCount() - e.t0) % static_cast<DWORD>(e.total_ms);
  size_t idx = 0;
  for (size_t i = 0; i < e.delays.size(); ++i) {
    if (elapsed < static_cast<DWORD>(e.delays[i])) { idx = i; break; }
    elapsed -= static_cast<DWORD>(e.delays[i]);
    idx = i;
  }
  p.tex = e.texs[idx];
  return p;
}

bool IsExplicitlyAllowed(const char* url) {
  if (url == nullptr || url[0] == '\0') return false;
  const std::string s(url);
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const std::string& u : g_allow_once)
    if (u == s) return true;
  return MatchesList(HostOf(s), g_user_hosts);
}

std::string HostOfUrl(const char* url) {
  if (url == nullptr || url[0] == '\0') return std::string();
  return HostOf(std::string(url));
}

void AllowOnce(const char* url) {
  if (url == nullptr || url[0] == '\0') return;
  const std::string s(url);
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const std::string& u : g_allow_once)
    if (u == s) return;
  // Une adresse déjà tentée et refusée reste en cache à l'état kFailed : sans ce
  // retrait, autoriser après coup ne rejouerait rien et le joueur croirait que
  // son geste n'a servi à rien.
  g_cache.erase(s);
  g_allow_once.push_back(s);
  if (g_allow_once.size() > 64) g_allow_once.erase(g_allow_once.begin());
}

void AllowHost(const char* host) {
  if (host == nullptr || host[0] == '\0') return;
  const std::string h = text::ToLowerAscii(std::string(host));
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const std::string& x : g_user_hosts)
    if (x == h) return;
  g_user_hosts.push_back(h);
}

void ForgetHost(const char* host) {
  if (host == nullptr || host[0] == '\0') return;
  const std::string h = text::ToLowerAscii(std::string(host));
  std::lock_guard<std::mutex> lock(g_mutex);
  for (size_t i = 0; i < g_user_hosts.size(); ++i) {
    if (g_user_hosts[i] != h) continue;
    g_user_hosts.erase(g_user_hosts.begin() + i);
    // Retirer un hôte doit AGIR tout de suite : on jette ses aperçus déjà en
    // cache, sinon ils continueraient de s'afficher après révocation.
    for (auto it = g_cache.begin(); it != g_cache.end();) {
      if (HostOf(it->first) == h) {
        ReleaseEntry(&it->second);
        it = g_cache.erase(it);
      } else {
        ++it;
      }
    }
    return;
  }
}

std::vector<std::string> UserHosts() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_user_hosts;
}

std::string UserHostsCsv() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::string out;
  for (const std::string& h : g_user_hosts) {
    if (!out.empty()) out += ';';
    out += h;
  }
  return out;
}

void SetUserHostsCsv(const std::string& csv) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_user_hosts.clear();
  size_t start = 0;
  while (start <= csv.size()) {
    size_t sep = csv.find(';', start);
    if (sep == std::string::npos) sep = csv.size();
    std::string h = text::ToLowerAscii(csv.substr(start, sep - start));
    while (!h.empty() && (h.front() == ' ')) h.erase(h.begin());
    while (!h.empty() && (h.back() == ' ')) h.pop_back();
    if (!h.empty()) g_user_hosts.push_back(h);
    start = sep + 1;
  }
}

void SetHostWhitelist(const std::vector<std::string>& hosts) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_hosts.clear();
  for (const std::string& h : hosts)
    if (!h.empty()) g_hosts.push_back(text::ToLowerAscii(h));
  if (g_hosts.empty()) EnsureHosts();  // liste vide = on retombe sur le défaut
}

void Tick() {
  std::vector<Decoded> ready;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    DropOnDeviceReset();
    ready.swap(g_finished);
  }

  // Création des textures HORS verrou : Overlay_CreateTextureARGB touche le
  // device, et on ne tient pas un mutex partagé avec les threads réseau pendant
  // un appel D3D.
  for (Decoded& d : ready) {
    std::vector<void*> texs;
    if (d.ok && d.w > 0 && d.h > 0) {
      for (const std::vector<uint8_t>& px : d.frames) {
        void* t = Overlay_CreateTextureARGB(px.data(), d.w, d.h);
        if (t == nullptr) break;  // VRAM épuisée : on garde ce qui a marché
        texs.push_back(t);
      }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    auto found = g_cache.find(d.url);
    if (found == g_cache.end()) {
      // Évincée entre-temps (ou reset de device) : ces textures n'ont plus de
      // place où vivre. On les relâche tout de suite — jamais dessinées, donc
      // aucune draw-list ne les référence.
      for (void* t : texs) Overlay_ReleaseTexture(t);
      continue;
    }
    if (!texs.empty()) {
      Entry& e = found->second;
      ReleaseEntry(&e);  // remplacement : ne pas fuir l'ancienne
      e.state = Preview::kReady;
      e.texs  = std::move(texs);
      e.w     = d.w;
      e.h     = d.h;
      e.t0    = GetTickCount();
      // Une seule image reste FIXE : total_ms nul, et Get s'en tient à la
      // première sans jamais consulter l'horloge.
      if (e.texs.size() > 1) {
        e.delays.assign(d.delays.begin(),
                        d.delays.begin() + static_cast<long>(e.texs.size()));
        for (int ms : e.delays) e.total_ms += ms;
      }
      if (e.total_ms <= 0) { e.total_ms = 0; e.delays.clear(); }
    } else {
      found->second.state = Preview::kFailed;
    }
  }

  // Bornage du cache. 🔴 On n'évince QUE depuis Tick, jamais pendant le rendu :
  // relâcher une texture qu'une draw-list référence encore fait planter au flush
  // (cf. feedback_texture_release_defer_frame). Tick est appelé hors frame.
  std::lock_guard<std::mutex> lock(g_mutex);
  while (g_cache.size() > kMaxCached) {
    auto oldest = g_cache.end();
    for (auto it = g_cache.begin(); it != g_cache.end(); ++it) {
      if (it->second.state == Preview::kPending) continue;  // pas de course
      if (oldest == g_cache.end() || it->second.used < oldest->second.used)
        oldest = it;
    }
    if (oldest == g_cache.end()) break;  // que des téléchargements en cours
    ReleaseEntry(&oldest->second);
    g_cache.erase(oldest);
  }
}

}  // namespace imgprev
