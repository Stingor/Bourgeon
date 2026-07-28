#include "features/systems/dx7_warning.h"

#include <Windows.h>
#include <shellapi.h>

#include <string>

#include "imgui.h"

#include "ui/ro_imgui.h"
#include "utils/log_console.h"

// Armé par le proxy DirectDraw au premier EndScene du chemin DX7
// (ddraw/proxy_idirectdraw.cc). Faux => le client rend en Direct3D 9.
extern bool g_imgui_dx7_active;

namespace {

// Titre affiché + ID stable (tout ce qui suit ### est hors rendu).
constexpr char kPopupId[] = "Mode DirectX 7 détecté###dx7_warning";

// Dossier de l'exécutable du jeu, backslash final inclus. Comme pour le patcher
// (integrity_check), on part du module et pas du CWD : un raccourci peut lancer
// le client depuis n'importe où, l'outil de config est toujours à côté de l'exe.
bool GameDir(std::wstring& out) {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  const std::wstring path(buf, n);
  const size_t slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos) return false;
  out = path.substr(0, slash + 1);
  return true;
}

// L'outil de configuration livré avec le client (ici OpenSetup, renommé
// Setup.exe). On ne propose le bouton que s'il est réellement présent.
bool FindSetupExe(std::wstring& out) {
  std::wstring dir;
  if (!GameDir(dir)) return false;
  for (const wchar_t* name : {L"Setup.exe", L"opensetup.exe", L"OpenSetup.exe"}) {
    std::wstring path = dir + name;
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
      out.swap(path);
      return true;
    }
  }
  return false;
}

// Chemin du Setup résolu une fois (l'appel disque ne se refait pas chaque frame).
const std::wstring& SetupExe() {
  static const std::wstring path = [] {
    std::wstring p;
    FindSetupExe(p);
    return p;
  }();
  return path;
}

void LaunchSetup() {
  const std::wstring& exe = SetupExe();
  if (exe.empty()) return;
  std::wstring dir;
  GameDir(dir);

  // ShellExecute plutôt que CreateProcess : le Setup peut demander une élévation
  // (écriture registre), et le shell gère l'invite UAC pour nous.
  SHELLEXECUTEINFOW sei = {};
  sei.cbSize      = sizeof(sei);
  sei.fMask       = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb      = L"open";
  sei.lpFile      = exe.c_str();
  sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
  sei.nShow       = SW_SHOWNORMAL;
  if (ShellExecuteExW(&sei)) {
    if (sei.hProcess) CloseHandle(sei.hProcess);
  } else {
    LogError("[DX7] impossible de lancer le Setup (erreur {})", GetLastError());
  }
}

}  // namespace

void Dx7Warning::Draw(bool at_login) {
  if (!g_imgui_dx7_active || dismissed_) return;

  // OpenPopup et BeginPopupModal au même niveau (hors de toute fenêtre) pour que
  // les ID concordent — même règle que les modales du char-select.
  if (!opened_) {
    ImGui::OpenPopup(kPopupId);
    opened_ = true;
    if (!logged_) {
      logged_ = true;
      LogInfo("[DX7] client en DirectX 7 — avertissement affiché");
    }
  }

  if (!ro::BeginRoPopupModal(kPopupId)) {
    // ImGui ferme un popup qui n'a pas été soumis pendant une frame — ce qui
    // arrive dès que RenderUI se met en retrait (chargement de carte, interface
    // native masquée en F11). Ce n'est PAS un acquittement : on réarme pour le
    // rouvrir. Seul le bouton « Continuer » (ou la fermeture du client) éteint
    // l'avertissement, via dismissed_.
    opened_ = false;
    return;
  }

  // Tant que la modale est là, Échap ne doit pas fermer les fenêtres RO derrière.
  ro::SuppressEscapeStack();

  ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 460.0f);

  ImGui::TextColored(ImVec4(0.75f, 0.15f, 0.15f, 1.0f),
                     "Ton client tourne en DirectX 7.");
  ImGui::Spacing();
  ImGui::TextUnformatted(
      "Bourgeon est développé et testé pour DirectX 9. En DirectX 7, le moteur "
      "n'a ni shaders ni cible de rendu : plusieurs fonctionnalités sont "
      "automatiquement désactivées ou dégradées.");
  ImGui::Spacing();
  ImGui::BulletText("Effets d'image : luminosité, contraste, filtres, vignettage, netteté, FXAA");
  ImGui::BulletText("Capture d'écran propre (sans interface) et filtrage des textures");
  ImGui::BulletText("Aperçus de sprites et d'effets (SPR Effect Lab, aperçus de costumes)");
  ImGui::BulletText("Mini-jeux : sprites réels indisponibles, DOOM inaccessible");
  ImGui::Spacing();
  ImGui::TextUnformatted(
      "Les performances et la compatibilité avec Windows 10/11 sont également "
      "bien meilleures en DirectX 9.");
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextUnformatted(
      "Pour changer : ferme le jeu, lance Setup.exe (dans le dossier du client), "
      "onglet « Graphics », choisis « DirectX 9 » dans « Graphics API », "
      "enregistre, puis relance le jeu.");

  ImGui::PopTextWrapPos();
  ImGui::Spacing();

  const bool has_setup = !SetupExe().empty();
  if (has_setup) {
    if (at_login) {
      // Hors du monde de jeu : on peut fermer le client sans couper la session
      // d'un personnage. Le Setup écrit son réglage dans le registre au moment
      // où le joueur enregistre — il n'a pas besoin que le jeu tourne.
      if (ro::RoButton("Quitter et ouvrir le Setup")) {
        LaunchSetup();
        ExitProcess(0);
      }
    } else {
      // En jeu, on ne coupe rien : le Setup s'ouvre derrière la fenêtre du jeu et
      // le réglage ne sera pris en compte qu'au prochain lancement.
      if (ro::RoButton("Ouvrir le Setup")) LaunchSetup();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Le Setup s'ouvre derrière le jeu — le changement prend "
                          "effet au prochain lancement.");
    }
    ImGui::SameLine();
  }
  if (ro::RoButton("Continuer en DirectX 7")) {
    ImGui::CloseCurrentPopup();
    dismissed_ = true;
  }

  ro::EndRoPopupModal();
}
