#include "ragnarok/globals.h"
#include "features/gameplay/keyboard_move.h"

#include "ragnarok/game_scene.h"
#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include <cmath>
#include <cstdint>

#include "bourgeon.h"
#include "features/hotkey_util.h"  // NativeTextInputHasFocus (garde partagée)
#include "features/windows/item_desc_window.h"  // WantsSideArrows (livre ouvert)
#include "imgui.h"
#include "ragnarok/camera.h"  // ro::camera::kCameraVTable (garde de validite du pointeur)

// ── Adresses (client 20250716, no-ASLR : addr Ghidra == live) ────────────────
namespace {
constexpr uintptr_t kCellValid    = 0x00c6cf80;  // Cell_IsMoveTargetValid
constexpr uintptr_t kClampReach   = 0x00c69160;  // Move_ClampToReachableCell
constexpr uintptr_t kNoPathFlag   = 0x0131f764;  // != 0 -> le natif passe en msg 0x10

constexpr int kOffOwnActor = 0x2c;   // scène -> acteur joueur
constexpr int kOffCamera   = 0xd0;   // CGameMode -> pCam
constexpr int kOffViewMat  = 0x98;   // pCam -> matrice de vue (12 floats)

constexpr int kOffActorX     = 0x10;   // acteur -> position monde X (float)
constexpr int kOffActorZ     = 0x18;   // acteur -> position monde Z (float)
constexpr int kOffActorState = 0x70;   // acteur -> état (6 = pas de pathfinding)

constexpr int kMsgWalkTo    = 0x11;  // Actor_OnMsg : « marche vers (x,y) »
constexpr int kMsgWalkToRaw = 0x10;  // idem, variante sans validation client

void* GetOwnActor(void* gm) {
  void* actor = nullptr;
  __try {
    if (gm) {
      void* mgr =
          *reinterpret_cast<void**>(reinterpret_cast<char*>(gm) + gamescene::kGmActorMgr);
      if (mgr)
        actor = *reinterpret_cast<void**>(reinterpret_cast<char*>(mgr) +
                                          kOffOwnActor);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    actor = nullptr;
  }
  return actor;
}

// La garde « une saisie native a le focus » (réplique de UIWindowMgr_OnKeyDown,
// 0x00a471e0) vit dans hotkey_util : elle vaut pour TOUT raccourci global, et une
// copie par module se serait corrigée une par une le jour où un offset bouge.

// ── Appel de Actor_OnMsg via la vtable (+8) ─────────────────────────────────
// Signature exacte du natif inconnue côté nettoyage de pile (Ghidra ne récupère
// que 11 des 13 dwords que les appelants empilent), donc on reproduit l'appel
// tel quel en asm et on RESTAURE ESP nous-mêmes : correct que la fonction soit
// __thiscall (ret 0x34) ou __cdecl. Ordre des arguments repris du clic-au-sol :
//   (0, msg, 0, x, x>>31, y, y>>31, 0, 0, 0, 0, 0, 0)   this = acteur (ECX)
// Les messages voyagent en 64 bits, d'où les dwords de poids fort.
// (pas de SEH ici : un __try ne fait pas bon ménage avec un bloc __asm — les
// appelants encadrent déjà l'appel.)
__declspec(noinline) void ActorSendWalkMsg(void* actor, int msg, int x, int y) {
  void** vtbl = *reinterpret_cast<void***>(actor);
  void* fn = vtbl[2];  // vtable+8 = OnMsg
  const int xh = x >> 31;
  const int yh = y >> 31;
  __asm {
    push esi
    mov  esi, esp
    push 0
    push 0
    push 0
    push 0
    push 0
    push 0
    mov  eax, yh
    push eax
    mov  eax, y
    push eax
    mov  eax, xh
    push eax
    mov  eax, x
    push eax
    push 0            // msg (dword de poids fort)
    mov  eax, msg
    push eax          // msg (dword de poids faible)
    push 0            // dword de tête (toujours 0 chez le natif)
    mov  ecx, actor
    mov  eax, fn
    call eax
    mov  esp, esi
    pop  esi
  }
}

// Cellule (x,y) occupée par l'acteur.
bool ActorCell(void* gm, void* actor, int* out_x, int* out_y) {
  bool ok = false;
  __try {
    using WorldToTile_t =
        void(__thiscall*)(void*, float, float, int*, int*, unsigned*, unsigned*);
    char* a = reinterpret_cast<char*>(actor);
    unsigned sub_x = 0, sub_y = 0;
    reinterpret_cast<WorldToTile_t>(gamescene::kWorldToTileAddr)(
        gm, *reinterpret_cast<float*>(a + kOffActorX),
        *reinterpret_cast<float*>(a + kOffActorZ), out_x, out_y, &sub_x, &sub_y);
    ok = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  return ok;
}

// ── Direction écran -> direction en cellules ────────────────────────────────
// Projette les 8 voisins sur les axes écran de la caméra et retient le plus
// aligné avec la direction demandée. `screen_x` = +1 vers la droite de l'écran,
// `screen_y` = +1 vers le haut de l'écran.
bool ScreenDirToCellDelta(void* gm, bool camera_relative, int screen_x,
                          int screen_y, int* out_dx, int* out_dy) {
  // Repli sans caméra : caméra par défaut = +X en cellules vers la droite de
  // l'écran, +Y vers le haut.
  float right_x = 1.0f, right_y = 0.0f;  // contribution écran-droite de (dx, dy)
  float up_x = 0.0f, up_y = 1.0f;        // contribution écran-haut de (dx, dy)
  if (camera_relative) {
    __try {
      void* cam =
          *reinterpret_cast<void**>(reinterpret_cast<char*>(gm) + kOffCamera);
      if (cam && *reinterpret_cast<uintptr_t*>(cam) == ro::camera::kCameraVTable) {
        const float* m = reinterpret_cast<const float*>(
            reinterpret_cast<char*>(cam) + kOffViewMat);
        // Lignes = (axeX, axeY, axeZ) : m[0..2] = X du repère caméra, etc.
        // Une direction de cellule (dx, dy) devient le vecteur monde
        // (dx, 0, dy) -> écran (dot avec axeX, dot avec axeY).
        right_x = m[0]; right_y = m[6];   // droite écran pour +X / +Y en cellules
        // L'axe Y du repère caméra pointe visuellement vers le BAS : le monde RO
        // a son Y vers le bas (cf. FpsView, « monter = soustraire ») et le
        // vecteur « up » passé au builder suit cette convention. D'où le signe -,
        // sinon Z/S et flèches haut/bas sont inversés (constaté en jeu).
        up_x    = -m[1]; up_y   = -m[7];  // haut écran pour +X / +Y en cellules
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }

  const float want_len =
      std::sqrt(static_cast<float>(screen_x * screen_x + screen_y * screen_y));
  if (want_len <= 0.0f) return false;
  const float wx = static_cast<float>(screen_x) / want_len;
  const float wy = static_cast<float>(screen_y) / want_len;

  float best = -2.0f;
  int best_dx = 0, best_dy = 0;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) continue;
      const float fx = static_cast<float>(dx), fy = static_cast<float>(dy);
      const float sx = fx * right_x + fy * right_y;
      const float sy = fx * up_x + fy * up_y;
      const float len = std::sqrt(sx * sx + sy * sy);
      if (len < 1e-4f) continue;  // direction écrasée par la projection
      const float score = (sx * wx + sy * wy) / len;
      if (score > best) { best = score; best_dx = dx; best_dy = dy; }
    }
  }
  if (best <= -2.0f) return false;
  *out_dx = best_dx;
  *out_dy = best_dy;
  return true;
}

// Demande de marche vers (x, y), avec la validation client du clic-au-sol.
void RequestWalk(void* gm, void* actor, int x, int y, bool validate) {
  __try {
    // Mêmes cas que le natif : mode « sans pathfinding » ou état 6 -> msg 0x10,
    // qui court-circuite la validation client.
    const bool raw = *reinterpret_cast<int*>(kNoPathFlag) != 0 ||
                     *reinterpret_cast<int*>(reinterpret_cast<char*>(actor) +
                                             kOffActorState) == 6;
    if (!raw && validate) {
      using CellValid_t = int(__thiscall*)(void*, int, int);
      using Clamp_t     = int(__thiscall*)(void*, int*, int*);
      if ((reinterpret_cast<CellValid_t>(kCellValid)(gm, x, y) & 0xff) == 0) {
        // Cible inatteignable telle quelle : on rabote à la dernière cellule
        // atteignable en ligne droite (comportement « clic derrière un mur »).
        if ((reinterpret_cast<Clamp_t>(kClampReach)(gm, &x, &y) & 0xff) == 0)
          return;
      }
    }
    ActorSendWalkMsg(actor, raw ? kMsgWalkToRaw : kMsgWalkTo, x, y);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}
}  // namespace

void KeyboardMove::SetEnabled(bool on) {
  if (on == enabled_) return;
  enabled_ = on;
  Reset();
}

void KeyboardMove::Reset() {
  last_dx_ = last_dy_ = 0;
  was_moving_ = false;
}

void KeyboardMove::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  if (mode_type != ModeMgr::ModeType::kGame) Reset();
}

void KeyboardMove::OnRenderUI() { Update(); }

void KeyboardMove::Update() {
  if (!enabled_) return;
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;
  // Appelable hors passe de rendu (OnProcessInput) : contexte ImGui obligatoire,
  // et on se met en retrait hors monde / pendant un chargement de carte.
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (!Bourgeon::Instance().IsGameActive() ||
      Bourgeon::Instance().IsMapLoading()) {
    Reset();
    return;
  }

  // Le clavier appartient à ImGui (fenêtre Bourgeon en saisie) ou à une textbox
  // native (chat, message privé, montant de vente…) -> on ne bouge pas. Un
  // modificateur enfoncé non plus : Alt+lettre / Ctrl+lettre sont des raccourcis
  // du jeu (et Maj sert au clic forcé), ils ne doivent pas faire marcher.
  const ImGuiIO& io = ImGui::GetIO();
  const bool keys_ours = !io.WantCaptureKeyboard && !io.KeyCtrl && !io.KeyAlt &&
                         !io.KeyShift && !hotkeys::NativeTextInputHasFocus();

  // 🔴 Les flèches ← → peuvent appartenir à une AUTRE fenêtre moderne — le panneau
  // livre s'en sert pour tourner ses pages, et le hook WndProc les confisque alors
  // au client. Cette confiscation ne nous protège pas : on ne lit pas les messages
  // Windows mais l'état ImGui, que le backend a enregistré AVANT qu'on retire la
  // touche au jeu. Sans ce test, tourner une page faisait marcher le personnage —
  // le geste avait donc encore deux effets, l'un d'eux resté invisible au premier
  // correctif.
  //
  // Seules les DEUX flèches latérales se taisent : ↑ et ↓ ne portent pas la
  // pagination du client, elles ne sont pas confisquées, et ZQSD reste actif de
  // bout en bout — lire un livre n'a jamais empêché d'avancer.
  const bool side_arrows_ours = !ItemDescWindow::WantsSideArrows();

  // Une touche d'un des deux jeux est-elle enfoncée ? Les touches sont des VK
  // configurables : on repasse par la table de `hotkey_util`, qui est l'inverse
  // exacte de celle du backend ImGui.
  //
  // ⚠ La règle des flèches latérales suit la TOUCHE, pas l'emplacement : le
  // joueur peut avoir mis ← ailleurs que dans le second jeu, et c'est bien ←
  // qu'il faut taire quand le panneau livre s'en sert.
  auto pressed = [&](int slot) {
    const int vkey = keys_[slot];
    if (vkey == 0) return false;
    if (!side_arrows_ours && (vkey == VK_LEFT || vkey == VK_RIGHT)) return false;
    const ImGuiKey key = hotkeys::VkToImGuiKey(vkey);
    return key != ImGuiKey_None && ImGui::IsKeyDown(key);
  };

  int screen_x = 0, screen_y = 0;
  if (keys_ours) {
    if (pressed(kFwd)   || pressed(kAltFwd))   ++screen_y;
    if (pressed(kBack)  || pressed(kAltBack))  --screen_y;
    if (pressed(kRight) || pressed(kAltRight)) ++screen_x;
    if (pressed(kLeft)  || pressed(kAltLeft))  --screen_x;
  }

  void* gm = rag::ActiveModeSafe();
  void* actor = GetOwnActor(gm);
  if (!gm || !actor) { Reset(); return; }

  const uint32_t now = GetTickCount();

  // Plus aucune touche : on demande la cellule courante pour s'arrêter net
  // (sinon le personnage finit le trajet demandé, comme après un clic).
  if (screen_x == 0 && screen_y == 0) {
    if (was_moving_) {
      was_moving_ = false;
      last_dx_ = last_dy_ = 0;
      if (stop_on_release_) {
        int cx = 0, cy = 0;
        if (ActorCell(gm, actor, &cx, &cy)) {
          RequestWalk(gm, actor, cx, cy, /*validate=*/false);
          last_req_ms_ = now;
        }
      }
    }
    return;
  }

  int dx = 0, dy = 0;
  if (!ScreenDirToCellDelta(gm, camera_relative_, screen_x, screen_y, &dx, &dy))
    return;

  // Cadence : ré-émission périodique tant que la touche est tenue (le natif fait
  // pareil quand on garde le bouton de la souris enfoncé), avec un plancher de
  // 60 ms sur un changement de direction — le natif s'auto-limite à 50 ms.
  const bool dir_changed = (dx != last_dx_ || dy != last_dy_);
  const uint32_t since = now - last_req_ms_;
  const uint32_t period = static_cast<uint32_t>(refresh_ms_ > 60 ? refresh_ms_ : 60);
  if (since < (dir_changed ? 60u : period)) return;

  int cx = 0, cy = 0;
  if (!ActorCell(gm, actor, &cx, &cy)) return;

  const int ahead = look_ahead_ > 0 ? look_ahead_ : 1;
  RequestWalk(gm, actor, cx + dx * ahead, cy + dy * ahead, /*validate=*/true);

  last_dx_ = dx;
  last_dy_ = dy;
  last_req_ms_ = now;
  was_moving_ = true;
}
