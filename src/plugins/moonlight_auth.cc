#include "plugins/moonlight_auth.h"

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
#include "plugins/auto_login.h"
#include "plugins/native_login.h"
#include "ragnarok/ragnarok_client.h"  // GameWindow (nav clavier service-select)
#include "ui/ro_imgui.h"
#include "utils/log_console.h"
#include "utils/game_paths.h"
#include "yaml-cpp/yaml.h"

namespace {

// Largeur commune des contrôles du formulaire (la fenêtre s'auto-dimensionne
// autour ; largeur fixe pour éviter que les champs/boutons ne s'effondrent).
constexpr float kFormW = 340.0f;

// URL « usine » du serveur Moonlight — identique pour tous les joueurs, intégrée
// au build : aucun utilisateur n'a à toucher à la config. La section yaml
// `moonlight_auth:` ne sert qu'à SURCHARGER (dev/local, ou désactiver).
constexpr const char* kDefaultBaseUrl = "https://moonlight-destiny.fr";

// (Le dossier du jeu vit dans utils/game_paths.h : paths::GameDir().)

// Identifiant web mémorisé — fichier dédié pour NE PAS réécrire (et abîmer) le
// yaml de config, qui est souvent édité à la main et lu seul par AutoLogin.
std::string RememberPath() { return paths::MoonlightUserPath(); }
// Mot de passe web mémorisé — chiffré DPAPI (lié au compte Windows courant :
// illisible par un autre utilisateur/machine, aucune clé à gérer).
std::string PwPath() { return paths::MoonlightPwPath(); }
std::string ClientInfoPath() { return paths::InGameDir("data\\clientinfo.xml"); }

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

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
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
    case 12002: m = "délai dépassé (timeout)"; break;
    case 12007: m = "nom de domaine introuvable (DNS)"; break;
    case 12029: m = "connexion refusée/impossible"; break;
    case 12030: m = "connexion coupée"; break;
    case 12045: m = "certificat TLS : autorité non reconnue"; break;
    case 12057: m = "certificat TLS : révocation non vérifiable"; break;
    case 12175: m = "échec TLS (certificat invalide/auto-signé ?)"; break;
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
    res.error = "URL invalide";
    return res;
  }
  const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

  HINTERNET hs = WinHttpOpen(L"Bourgeon/MoonlightAuth",
                             WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hs) {
    res.error = "WinHttpOpen a échoué";
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
constexpr uintptr_t kTexMgrGet  = 0x00a90350;  // UITextureMgr_Get() __cdecl -> mgr
constexpr uintptr_t kTexMakeKey = 0x00a9f030;  // __cdecl(path) -> key
constexpr uintptr_t kTexLoad    = 0x00a8d4a0;  // __fastcall(mgr, 0, key) -> tex
constexpr int kTexOffW = 0x114, kTexOffH = 0x118, kTexOffPix = 0x11c;
using TexMgrGet_t  = void*(__cdecl*)();
using TexMakeKey_t = void*(__cdecl*)(const char*);
using TexLoad_t    = void*(__fastcall*)(void*, void*, void*);

struct ButtonTex { void* tex = nullptr; int w = 0; int h = 0; };

// Charge une BMP de data\texture\ en pixels BGRA (pointeur natif). SEH (POD only).
bool LoadRawBgra(const char* path, const uint8_t** bgra, int* w, int* h) {
  __try {
    void* mgr = reinterpret_cast<TexMgrGet_t>(kTexMgrGet)();
    if (!mgr) return false;
    void* key = reinterpret_cast<TexMakeKey_t>(kTexMakeKey)(path);
    if (!key) return false;
    void* t = reinterpret_cast<TexLoad_t>(kTexLoad)(mgr, nullptr, key);
    if (!t) return false;
    const int tw = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexOffW);
    const int th = *reinterpret_cast<int*>(static_cast<char*>(t) + kTexOffH);
    const uint8_t* px =
        *reinterpret_cast<const uint8_t**>(static_cast<char*>(t) + kTexOffPix);
    if (tw <= 0 || th <= 0 || tw > 1024 || th > 1024 || !px) return false;
    *bgra = px; *w = tw; *h = th;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
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

}  // namespace

MoonlightAuth::MoonlightAuth(AutoLogin* auto_login) : auto_login_(auto_login) {
  LoadConfig();
  ResolveServer();
  state_ = enabled_ ? State::kWebLogin : State::kDisabled;
  // Diagnostic (niveau warn -> toujours dans bourgeon.log) : dit si le plugin est
  // armé et pourquoi. « inerte » = pas de base_url dans bourgeon_settings.yaml.
  LogDiag("[MoonlightAuth] {} (base_url='{}', endpoint='{}')",
          enabled_ ? "ARMÉ" : "inerte (base_url manquant/enabled=false)",
          base_url_, endpoint_);
}

MoonlightAuth::~MoonlightAuth() { JoinWorker(); }

void MoonlightAuth::LoadConfig() {
  // Valeurs usine : activé pour tous, URL du serveur intégrée au build. Le yaml
  // est OPTIONNEL et ne fait que surcharger (dev/local, ou enabled:false).
  base_url_ = kDefaultBaseUrl;
  enabled_ = true;
  std::ifstream f(paths::SettingsPath());
  if (f) {
    try {
      const YAML::Node root = YAML::Load(f);
      const YAML::Node ma = root["moonlight_auth"];
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
  }
  if (base_url_.empty()) enabled_ = false;
  // Retire un éventuel '/' final du base_url pour concaténer proprement.
  while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();

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
  const std::vector<std::string> names = ReadConnectionNames(ClientInfoPath());
  server_count_ = static_cast<int>(names.size());
  server_index_ = 0;  // défaut = 1ʳᵉ connexion (Moonlight-Destiny, base clientinfo)
  server_name_ = ParseServerArg();  // envoyé au site pour cibler la bonne DB
  const std::string want = ToLower(server_name_);
  if (!want.empty()) {
    for (int i = 0; i < server_count_; ++i) {
      if (ToLower(names[i]) == want) {
        server_index_ = i;
        break;
      }
    }
  }
  LogDiag("[MoonlightAuth] service-select : {} connexion(s), cible index {}{}",
          server_count_, server_index_,
          want.empty() ? "" : (" (--server=" + want + ")"));
}

void MoonlightAuth::DriveServerSelect() {
  HWND hwnd = static_cast<HWND>(RagnarokClient::GameWindow());
  if (!hwnd) return;
  auto press = [hwnd](int vk) {
    PostMessageW(hwnd, WM_KEYDOWN, static_cast<WPARAM>(vk), 0);
    PostMessageW(hwnd, WM_KEYUP, static_cast<WPARAM>(vk), 0);
  };
  // Le client mémorise la dernière connexion : on force le haut de liste (Haut
  // borne en tête), puis on descend à l'index cible, puis on valide.
  for (int i = 0; i < server_count_; ++i) press(VK_UP);
  for (int i = 0; i < server_index_; ++i) press(VK_DOWN);
  press(VK_RETURN);
  LogDiag("[MoonlightAuth] service-select auto -> connexion index {}", server_index_);
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
    LogDiag("[MoonlightAuth] -> mode LOGIN (authenticated={}, drove_ml={}, "
            "socket_fd={}, {} connexion(s), cible idx={})",
            authenticated_, drove_moonlight_login_, native_login::SocketFd(),
            server_count_, server_index_);
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
      RearmWebLogin("mode LOGIN sans session vivante");
  }
}

void MoonlightAuth::RearmWebLogin(const char* reason, bool service_select_pending) {
  if (!enabled_) return;
  LogDiag("[MoonlightAuth] réarmement du login web ({}) : état {} -> kWebLogin",
          reason ? reason : "?", StateName(state_));
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
  state_ = State::kWebLogin;
}

const char* MoonlightAuth::StateName(State s) {
  switch (s) {
    case State::kDisabled: return "kDisabled";
    case State::kWebLogin: return "kWebLogin";
    case State::kAuthing: return "kAuthing";
    case State::kDiscordStart: return "kDiscordStart";
    case State::kDiscordWait: return "kDiscordWait";
    case State::kPickAccount: return "kPickAccount";
    case State::kSelecting: return "kSelecting";
    case State::kDriveLogin: return "kDriveLogin";
    case State::kError: return "kError";
  }
  return "(init)";
}

void MoonlightAuth::OnRenderLoginUI() {
  if (!enabled_ || state_ == State::kDisabled) return;
  // Instrumentation : loggue chaque transition d'état (un seul point couvre les
  // ~15 sites de `state_ = …`). Verbeux mais inestimable pour diagnostiquer.
  if (state_ != last_logged_state_) {
    LogDiag("[MoonlightAuth] état -> {}", StateName(state_));
    last_logged_state_ = state_;
  }
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
    if (!fired_) {
      char rb_id[64] = {0}, rb_pw[64] = {0};
      if (native_login::DriveLogin(drive_user_.c_str(), drive_pw_.c_str(), rb_id,
                                   rb_pw, sizeof(rb_id))) {
        fired_ = true;
        fire_tick_ = GetTickCount();
        LogDiag("[MoonlightAuth] login tiré : userid='{}' otp='{}' | champs "
                "NATIFS relus -> id='{}' pw='{}'",
                drive_user_, drive_pw_, rb_id, rb_pw);
      }
      return;
    }
    // Diag : la socket login s'est-elle ouverte ? (le login a-t-il atteint le
    // serveur, ou OnMsg n'a-t-il pas déclenché la connexion ?)
    if (!socket_seen_ && native_login::SocketFd() >= 0) {
      socket_seen_ = true;
      charsrv_tick_ = GetTickCount();  // départ de l'auto-confirm char-server
      LogDiag("[MoonlightAuth] socket login OUVERTE (fd={}) -> le login a atteint "
              "le serveur (refus = credentials)",
              native_login::SocketFd());
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
    if (!charsel_reached_ && native_login::CharSelectWindowPresent())
      charsel_reached_ = true;
    if (charsel_reached_) return;

    // Auto-confirmation du char-server. AC_ACCEPT_LOGIN 0x0ac4 amène le client à
    // l'état 6, qui CONSTRUIT la fenêtre id 2 « Select Service » (liste des
    // char-servers, boutons OK/cancel) : elle attend une validation, d'où cette
    // Entrée. On tire IMMÉDIATEMENT dès le login réussi (fenêtre de login détruite),
    // et on s'ARRÊTE dès que la fenêtre du char-select (0x115) est
    // là -> jamais d'Entrée au char-select. Gaté sur
    // "login réussi" (pas de fenêtre de login présente) pour ne pas taper sur un
    // écran d'échec. La latence restante login->char-select = RÉSEAU (2 RTT).
    //
    // ⚠ Rythme VOLONTAIREMENT lent (200 ms, 10 essais = 2 s de couverture, contre
    // 50 ms/20 essais avant) : chaque essai POSTE une Entrée dans la file Win32, que
    // le client consommera plus tard, sur l'écran qu'il aura atteint entre-temps. À
    // 50 ms on chargeait la file d'une vingtaine de munitions qui retombaient sur le
    // char-select. Le char-select ImGui s'en protège aussi de son côté (fenêtre
    // d'insensibilité à l'arrivée), mais mieux vaut ne pas les tirer du tout.
    if (socket_seen_ && !native_login::LoginWindowPresent() &&
        !native_login::CharSelectWindowPresent() && charsrv_tries_ < 10 &&
        (charsrv_tries_ == 0 || (GetTickCount() - charsrv_tick_) > 200)) {
      HWND hwnd = static_cast<HWND>(RagnarokClient::GameWindow());
      if (hwnd) {
        PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0);
        PostMessageW(hwnd, WM_KEYUP, VK_RETURN, 0);
      }
      charsrv_tick_ = GetTickCount();
      ++charsrv_tries_;
      LogDiag("[MoonlightAuth] char-server auto-confirm (Entrée, essai {})",
              charsrv_tries_);
    }

    // Déjà tiré : détecter un ÉCHEC pour ne pas rester bloqué sur un écran mort.
    // Succès -> char-select (fenêtre login détruite). Échec (OTP refusé/timeout)
    // -> le mode revient à l'écran de login, la fenêtre UILoginWnd est RECRÉÉE.
    // Grâce ~1,2 s (destruction async) puis timeout dur si la socket ne monte pas.
    const unsigned long dt = GetTickCount() - fire_tick_;
    if (((dt > 1200) && native_login::LoginWindowPresent()) ||
        ((dt > 15000) && (native_login::SocketFd() == -1))) {
      LogDiag("[MoonlightAuth] ÉCHEC login détecté (dt={}ms, login_wnd={}, "
              "socket_fd={}) -> kError",
              dt, native_login::LoginWindowPresent(), native_login::SocketFd());
      error_msg_ =
          "Connexion refusée (identifiants/OTP invalides ou serveur "
          "injoignable). Réessaie.";
      drove_moonlight_login_ = false;
      authenticated_ = false;
      fired_ = false;
      state_ = State::kError;  // re-masque le natif + réaffiche le formulaire
    }
    return;
  }

  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  if (disp.x <= 0.0f || disp.y <= 0.0f) return;  // garde minimize

  // Service-select (liste <connection>) : tant que la fenêtre de LOGIN n'est pas
  // là, on ne dessine PAS le formulaire (sinon il apparaît par-dessus le
  // service-select, qui capte alors le clavier). Avec 0/1 connexion il n'y a pas de
  // service-select : la fenêtre de login arrive directement.
  //
  // Franchissement : commit NATIF instantané de la connexion cible via
  // CLoginMode_SendMsg cmd 0x2723 (Apply_ClientInfoConnection -> état login). C'est
  // ce que déclenche l'Entrée clavier, mais direct et sans délai.
  // ⚠ Ne PAS confondre avec cmd 0x2713 : celui-là sélectionne le CHAR-SERVER
  // (post-login, table mode+0x1e8 peuplée par AC_ACCEPT_LOGIN) — c'était mon erreur
  // initiale, d'où les IP garbage et le « Failed to Connect ».
  // Repli clavier (éprouvé) si le natif n'a pas fait apparaître l'écran de login.
  if (!native_login::LoginWindowPresent()) {
    // Init paresseuse au cas où OnModeSwitch n'aurait pas été émis à la 1ʳᵉ entrée.
    if (login_enter_tick_ == 0) login_enter_tick_ = GetTickCount();
    const unsigned long now = GetTickCount();
    if (server_count_ > 1 && !server_select_done_) {
      if (native_login::SelectClientInfoConnection(server_index_)) {
        server_select_done_ = true;
        LogDiag("[MoonlightAuth] service-select NATIF (0x2723) -> connexion idx={}",
                server_index_);
      }
    } else if (server_count_ > 1 && server_select_done_ && !svc_kbd_fallback_ &&
               (now - login_enter_tick_) > 1500) {
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
          "Connexion Discord expirée. Réessaie et valide dans le navigateur.";
      state_ = State::kError;
    } else if (now - discord_poll_tick_ >= discord_poll_interval_ms_) {
      discord_poll_tick_ = now;
      StartPost("action=discord_poll&game_session=" + UrlEncode(game_session_) +
                "&server=" + UrlEncode(server_name_));
    }
  }

  // La réponse /select a pu déclencher le pilotage natif : ne rien dessiner.
  if (state_ == State::kDriveLogin) return;

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

  ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_AlwaysAutoResize;
  if (ro::BeginRoWindow("Connexion Moonlight", nullptr, flags)) {
    switch (state_) {
      case State::kWebLogin:     DrawWebLogin(); break;
      case State::kAuthing:      DrawSpinner("Authentification…"); break;
      case State::kDiscordStart: DrawSpinner("Ouverture de Discord…"); break;
      case State::kDiscordWait:  DrawDiscordWait(); break;
      case State::kPickAccount:  DrawPickAccount(); break;
      case State::kSelecting:    DrawSpinner("Préparation du compte…"); break;
      case State::kError:        DrawError(); break;
      default: break;
    }
  }
  ro::EndRoWindow();
}

void MoonlightAuth::DrawWebLogin() {
  ImGui::TextUnformatted("Connecte-toi avec ton compte Moonlight");
  ImGui::Spacing();

  ImGui::TextUnformatted("Identifiant");
  ImGui::SetNextItemWidth(kFormW);
  ImGui::InputText("##user", user_buf_, sizeof(user_buf_));

  ImGui::TextUnformatted("Mot de passe");
  ImGui::SetNextItemWidth(kFormW);
  const bool submit_pw = ImGui::InputText(
      "##pass", pass_buf_, sizeof(pass_buf_),
      ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);

  bool remember = remember_;
  if (ro::RoCheckbox("Se souvenir de moi", &remember)) remember_ = remember;
  bool remember_pw = remember_pw_;
  if (ro::RoCheckbox("Se souvenir du mot de passe", &remember_pw)) {
    remember_pw_ = remember_pw;
    if (remember_pw_)
      remember_ = true;  // le mot de passe mémorisé implique l'identifiant
    else
      DeleteFileA(PwPath().c_str());  // décoché -> on supprime tout de suite
  }

  ImGui::Spacing();
  const bool has_input = user_buf_[0] != '\0' && pass_buf_[0] != '\0';
  const bool click = ro::RoButton("Se connecter", kFormW, 0.0f);
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
      const float maxw = kFormW - pad * 2.0f;
      if (iw > maxw) { ih *= maxw / iw; iw = maxw; }
      const float btnh = ih + pad * 2.0f;
      discord = ro::RoButton("##discord_login", kFormW, btnh);
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
      discord = ro::RoButton("Se connecter avec Discord", kFormW, 0.0f);
      ImGui::PopStyleColor(3);
    }
    if (discord) StartDiscordLogin();
  }

  // Plusieurs comptes Moonlight à regrouper ? Lien vers le service de fusion de
  // comptes du site (ouvre le navigateur). URL bâtie sur base_url_ (même origine
  // que les appels API) — pas d'hôte hardcodé.
  ImGui::Spacing();
  ImGui::TextDisabled("Plusieurs comptes à regrouper ?");
  ImGui::SameLine();
  HyperlinkOpen("Fusionner mes comptes",
                base_url_ + "/ucp.php?i=moonlight&mode=merge");

  // Option secondaire : se connecter directement à un compte RO via le login
  // natif (login site oublié, préférence, etc.). Discrète, sous le bouton
  // principal — masque le formulaire moderne pour la session.
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextDisabled("Tu préfères te connecter à un compte Ragnarok ?");
  if (ro::RoSmallButton("Utiliser le login classique")) {
    native_fallback_ = true;
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
  DrawSpinner("En attente de Discord");
  ImGui::Spacing();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kFormW);
  ImGui::TextWrapped(
      "Termine la connexion dans ton navigateur, puis reviens ici : ta liste "
      "de comptes s'affichera automatiquement.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  // Ré-ouvrir la page si le joueur a fermé l'onglet par erreur.
  if (!discord_authorize_url_.empty() &&
      ro::RoSmallButton("Rouvrir la page Discord")) {
    ShellExecuteA(nullptr, "open", discord_authorize_url_.c_str(), nullptr,
                  nullptr, SW_SHOWNORMAL);
  }
  ImGui::Spacing();
  if (ro::RoButton("Annuler", kFormW, 0.0f)) {
    game_session_.clear();
    discord_authorize_url_.clear();
    state_ = State::kWebLogin;
  }
}

void MoonlightAuth::DrawPickAccount() {
  ImGui::TextUnformatted("Choisis un compte Ragnarok");
  ImGui::TextDisabled("Flèches pour naviguer, Entrée (ou double-clic) pour jouer.");
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
  ImGui::BeginChild("##accts", ImVec2(kFormW, listh), true);
  for (int i = 0; i < n; ++i) {
    const Account& a = accounts_[i];
    ImGui::PushID(i);
    if (a.banned) ImGui::BeginDisabled();
    char line[192];
    std::snprintf(line, sizeof(line), "%s  (%d perso%s)%s%s%s", a.label.c_str(),
                  a.char_count, a.char_count > 1 ? "s" : "",
                  a.banned ? "  [banni]" : "",
                  a.last_login.empty() ? "" : "   ",
                  a.last_login.empty() ? "" : a.last_login.c_str());
    if (ImGui::Selectable(line, selected_ == i,
                          ImGuiSelectableFlags_AllowDoubleClick)) {
      selected_ = i;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) confirm = true;
    }
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

  // Entrée (ou pavé num.) = jouer le compte focus.
  if (can_play && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                   ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
    confirm = true;

  if (!can_play) ImGui::BeginDisabled();
  if (ro::RoButton("Jouer", kFormW, 0.0f)) confirm = true;
  if (!can_play) ImGui::EndDisabled();

  if (ro::RoButton("Retour", kFormW, 0.0f)) {
    accounts_.clear();
    selected_ = -1;
    web_ticket_.clear();
    state_ = State::kWebLogin;
    return;
  }

  if (confirm && can_play) {
    const Account& a = accounts_[selected_];
    char aid[32];
    std::snprintf(aid, sizeof(aid), "%ld", a.account_id);
    const std::string form = "action=select&web_ticket=" +
                             UrlEncode(web_ticket_) + "&account_id=" + aid +
                             "&server=" + UrlEncode(server_name_);
    StartPost(form);
    state_ = State::kSelecting;
  }
}

void MoonlightAuth::DrawError() {
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 110, 255));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kFormW);
  ImGui::TextWrapped("%s", error_msg_.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::Spacing();
  if (ro::RoButton("Réessayer", kFormW, 0.0f)) {
    error_msg_.clear();
    state_ = State::kWebLogin;
  }
  // Filet de sécurité (échec uniquement) : basculer sur le login natif pour la
  // session — utile si le site est en panne partielle alors que le jeu répond.
  if (ro::RoButton("Login classique", kFormW, 0.0f)) {
    error_msg_.clear();
    native_fallback_ = true;
    native_login::MaskLoginWindow(false);  // réaffiche le natif (one-shot)
  }
}

bool MoonlightAuth::ApplyAccountList(const HttpResult& r) {
  if (!r.error.empty()) {
    error_msg_ = "Connexion au serveur impossible : " + r.error;
    state_ = State::kError;
    return false;
  }
  try {
    const auto j = nlohmann::json::parse(r.body);
    if (r.status != 200 || !j.value("ok", false)) {
      error_msg_ = j.value("error", std::string("Authentification refusée."));
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
      accounts_.push_back(std::move(a));
    }
    if (accounts_.empty()) {
      error_msg_ = "Aucun compte Ragnarok lié à ce compte Moonlight.";
      state_ = State::kError;
      return false;
    }
    // Pré-sélectionne le compte joué le plus récemment (last_login "YYYY-MM-DD"
    // → max lexicographique), sinon le premier. Ainsi « Entrée » suffit.
    selected_ = 0;
    for (int i = 1; i < static_cast<int>(accounts_.size()); ++i)
      if (accounts_[i].last_login > accounts_[selected_].last_login)
        selected_ = i;
    pass_buf_[0] = '\0';
    state_ = State::kPickAccount;
    return true;
  } catch (const std::exception&) {
    error_msg_ = "Réponse du serveur illisible.";
    state_ = State::kError;
    return false;
  }
}

void MoonlightAuth::HandleAuthResponse(const HttpResult& r) {
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
  LogDiag("[MoonlightAuth] discord_start -> status={} err='{}' body='{}'",
          r.status, r.error, r.body);
  if (!r.error.empty()) {
    error_msg_ = "Connexion au serveur impossible : " + r.error;
    state_ = State::kError;
    return;
  }
  try {
    const auto j = nlohmann::json::parse(r.body);
    if (r.status != 200 || !j.value("ok", false)) {
      error_msg_ =
          j.value("error", std::string("Impossible de démarrer la connexion Discord."));
      state_ = State::kError;
      return;
    }
    game_session_ = j.value("game_session", std::string());
    // Le serveur renvoie un chemin RELATIF (oauth_discord.php est à la racine du
    // domaine, PAS sous le board phpBB /forum/). On préfixe avec notre base_url_
    // (l'origine exacte de nos appels API) = source de vérité unique de l'hôte.
    const std::string path = j.value("authorize_path", std::string());
    if (game_session_.empty() || path.empty()) {
      error_msg_ = "Réponse incomplète du serveur.";
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
    LogDiag("[MoonlightAuth] navigateur ouvert (session={}) -> {}", game_session_,
            discord_authorize_url_);
    state_ = State::kDiscordWait;
  } catch (const std::exception&) {
    error_msg_ = "Réponse du serveur illisible.";
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
      error_msg_ = j.value("error", std::string("Session Discord expirée. Réessaie."));
      state_ = State::kError;
      return;
    }
    if (!j.value("ok", false)) {
      error_msg_ = j.value("error", std::string("Connexion Discord refusée."));
      state_ = State::kError;
      return;
    }
    if (j.value("pending", false)) return;  // pas encore validé -> continue le poll
    // Résolu : la réponse porte web_ticket + accounts (comme /auth).
    LogDiag("[MoonlightAuth] discord_poll résolu -> liste de comptes");
    ApplyAccountList(r);
  } catch (const std::exception&) {
    // JSON illisible ponctuel : on réessaie plutôt que d'échouer sec.
    LogDiag("[MoonlightAuth] discord_poll JSON illisible (on réessaie)");
  }
}

void MoonlightAuth::HandleSelectResponse(const HttpResult& r) {
  LogDiag("[MoonlightAuth] /select -> status={} err='{}' body='{}'", r.status,
          r.error, r.body);
  if (!r.error.empty()) {
    error_msg_ = "Connexion au serveur impossible : " + r.error;
    state_ = State::kError;
    return;
  }
  try {
    const auto j = nlohmann::json::parse(r.body);
    if (r.status != 200 || !j.value("ok", false)) {
      error_msg_ = j.value("error", std::string("Sélection du compte refusée."));
      state_ = State::kError;
      return;
    }
    const std::string userid = j.value("userid", std::string());
    const std::string otp = j.value("otp", std::string());
    if (userid.empty() || otp.empty()) {
      error_msg_ = "Réponse incomplète du serveur.";
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
    error_msg_ = "Réponse du serveur illisible.";
    state_ = State::kError;
  }
}
