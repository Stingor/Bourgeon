#include "features/systems/dx7_warning.h"

#include <Windows.h>
#include <shellapi.h>

#include <string>

#include "imgui.h"

#include "ui/ro_imgui.h"
#include "utils/log_console.h"
#include "utils/i18n.h"
#include "utils/game_paths.h"

// Armé par le proxy DirectDraw au premier EndScene du chemin DX7
// (ddraw/proxy_idirectdraw.cc). Faux => le client rend en Direct3D 9.
extern bool g_imgui_dx7_active;

namespace {

// Titre affiché + ID stable (tout ce qui suit ### est hors rendu).
//
// 🔴 Le libellé reste NU ici, et se traduit à ses points d'usage (`PopupTitle()`).
// Un `constexpr char[]` ne peut pas être initialisé par un appel de fonction —
// et même si le compilateur l'acceptait, la valeur serait figée au chargement,
// donc en français pour toujours.
constexpr char kPopupId[] = "Mode DirectX 7 détecté###dx7_warning";

// Le titre traduit, résolu à CHAQUE appel. L'identifiant stable voyage dans la
// traduction (« ###dx7_warning » en fait partie), donc OpenPopup et
// BeginPopupModal continuent de se répondre quelle que soit la langue.
inline const char* PopupTitle() { return i18n::Tr(kPopupId); }

// L'outil de configuration livré avec le client (ici OpenSetup, renommé
// Setup.exe). On ne propose le bouton que s'il est réellement présent.
bool FindSetupExe(std::wstring& out) {
  std::wstring dir;
  if (!paths::GameDirW(dir)) return false;
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
  paths::GameDirW(dir);

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

namespace dx7 {

void DrawWarningBody() {
  ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 460.0f);
  ImGui::TextUnformatted(
      i18n::Tr("Bourgeon est développé et testé pour DirectX 9. En DirectX 7, le moteur "
      "n'a ni shaders ni cible de rendu : plusieurs fonctionnalités sont "
      "automatiquement désactivées ou dégradées."));
  ImGui::Spacing();
  ImGui::BulletText("%s", i18n::Tr("Effets d'image : luminosité, contraste, filtres, vignettage, netteté, FXAA"));
  ImGui::BulletText("%s", i18n::Tr("Capture d'écran propre (sans interface) et filtrage des textures"));
  ImGui::BulletText("%s", i18n::Tr("Aperçus de sprites et d'effets (SPR Effect Lab, aperçus de costumes)"));
  ImGui::BulletText("%s", i18n::Tr("Mini-jeux : sprites réels indisponibles, DOOM inaccessible"));
  ImGui::Spacing();
  ImGui::TextUnformatted(
      i18n::Tr("Les performances et la compatibilité avec Windows 10/11 sont également "
      "bien meilleures en DirectX 9."));
  ImGui::PopTextWrapPos();
}

}  // namespace dx7

void Dx7Warning::Draw(bool at_login) {
  if (!g_imgui_dx7_active || dismissed_) return;

  // OpenPopup et BeginPopupModal au même niveau (hors de toute fenêtre) pour que
  // les ID concordent — même règle que les modales du char-select.
  if (!opened_) {
    ImGui::OpenPopup(PopupTitle());
    opened_ = true;
    if (!logged_) {
      logged_ = true;
      LogInfo("[DX7] client en DirectX 7 — avertissement affiché");
    }
  }

  if (!ro::BeginRoPopupModal(PopupTitle())) {
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

  ImGui::TextColored(ImVec4(0.75f, 0.15f, 0.15f, 1.0f),
                     "%s", i18n::Tr("Ton client tourne en DirectX 7."));
  ImGui::Spacing();
  dx7::DrawWarningBody();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 460.0f);
  // ⚠ Le menu Échap de Bourgeon sait désormais changer l'API lui-même (onglet
  // Graphismes). Le Setup reste proposé parce que cette modale s'affiche AUSSI à
  // l'écran de login, où le panneau de réglages n'existe pas.
  ImGui::TextUnformatted(
      i18n::Tr("Pour changer : Échap -> Réglages du jeu -> onglet « Graphismes », "
      "choisis « DirectX 9 » comme API graphique, puis applique — le client "
      "redémarrera. Le Setup du client fait la même chose."));
  ImGui::PopTextWrapPos();
  ImGui::Spacing();

  const bool has_setup = !SetupExe().empty();
  if (has_setup) {
    if (at_login) {
      // Hors du monde de jeu : on peut fermer le client sans couper la session
      // d'un personnage. Le Setup écrit son réglage dans le registre au moment
      // où le joueur enregistre — il n'a pas besoin que le jeu tourne.
      if (ro::RoButton(i18n::Tr("Quitter et ouvrir le Setup"))) {
        LaunchSetup();
        ExitProcess(0);
      }
    } else {
      // En jeu, on ne coupe rien : le Setup s'ouvre derrière la fenêtre du jeu et
      // le réglage ne sera pris en compte qu'au prochain lancement.
      if (ro::RoButton(i18n::Tr("Ouvrir le Setup"))) LaunchSetup();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", i18n::Tr("Le Setup s'ouvre derrière le jeu — le changement prend "
                                "effet au prochain lancement."));
    }
    ImGui::SameLine();
  }
  if (ro::RoButton(i18n::Tr("Continuer en DirectX 7"))) {
    ImGui::CloseCurrentPopup();
    dismissed_ = true;
  }

  ro::EndRoPopupModal();
}
