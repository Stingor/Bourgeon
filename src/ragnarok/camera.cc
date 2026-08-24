#include "ragnarok/camera.h"

#include <Windows.h>

#include "utils/hooking/hook_manager.h"

// ── Adresses (client 20250716, sans ASLR : adresse Ghidra == adresse live) ───
namespace {
constexpr int       kCamOffInMode = 0xd0;        // CGameMode+0xd0 = pCam
constexpr uintptr_t kCamClamp     = 0x00c82340;  // Camera_ApplyViewDistanceClamp
}  // namespace

// Portée fichier et NON anonyme : l'asm inline du stub nu les résout par nom.
static void* g_pcam  = nullptr;  // dernière caméra vue
static void* g_tramp = nullptr;  // -> prologue relocalisé + corps original

// Cueille la caméra depuis le mode de jeu (ECX à l'entrée de la fonction de
// clamp). Sous `__try` : la chaîne de pointeurs n'a de sens qu'une fois en jeu,
// et cette fonction tourne aussi pendant les transitions de carte.
//
// La vtable est VÉRIFIÉE avant de retenir le pointeur. Ce n'est pas de la
// prudence décorative : `CGameMode+0xd0` n'est renseigné qu'après
// `CGameMode_EnterWorld`, et lire ce champ trop tôt rendrait un pointeur mort
// que tout le monde écrirait ensuite.
void __fastcall RoCameraCapture(void* gamemode) {
  __try {
    if (!gamemode) return;
    void* pcam = *reinterpret_cast<void**>(
        reinterpret_cast<char*>(gamemode) + kCamOffInMode);
    if (pcam && *reinterpret_cast<uintptr_t*>(pcam) == ro::camera::kCameraVTable)
      g_pcam = pcam;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Entrée nue : ECX = param_1 (CGameMode). On le passe à la capture (les `push`
// ne touchent pas ECX), puis on entre dans l'original par le trampoline.
__declspec(naked) static void RoCameraCaptureStub() {
  __asm {
    push eax
    push ecx                   // param_1 préservé au travers de l'appel
    push edx
    call RoCameraCapture       // __fastcall(ecx = gamemode)
    pop  edx
    pop  ecx
    pop  eax
    jmp  [g_tramp]
  }
}

namespace ro::camera {

void Install() {
  if (g_tramp) return;  // idempotent : le premier appelant pose, les autres non
  g_tramp = hooking::HookManager::Instance().SetHook(
      hooking::HookType::kJmpHook, reinterpret_cast<uint8_t*>(kCamClamp),
      reinterpret_cast<uint8_t*>(&RoCameraCaptureStub));
}

// 🔴 VALIDÉ À CHAQUE APPEL, et ce n'est pas de la superstition : `g_pcam` est un
// pointeur MIS EN CACHE, alors que l'objet, lui, meurt avec son CGameMode — un
// changement de carte, un retour au choix de personnage, et il ne reste qu'une
// adresse qui a l'air bonne. Or ce qui lit la caméra le fait souvent SANS que le
// joueur touche à rien (un écran de veille, par définition), donc sans que le
// hook de capture ait eu l'occasion de rafraîchir quoi que ce soit. Relire la
// vtable coûte une comparaison et transforme une écriture dans de la mémoire
// libérée en un simple « pas de caméra pour l'instant ».
void* Get() {
  if (!g_pcam) return nullptr;
  __try {
    if (*reinterpret_cast<uintptr_t*>(g_pcam) != ro::camera::kCameraVTable) g_pcam = nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_pcam = nullptr;
  }
  return g_pcam;
}

float Read(int offset, float fallback) {
  void* pcam = Get();
  if (!pcam) return fallback;
  return *reinterpret_cast<float*>(reinterpret_cast<char*>(pcam) + offset);
}

void Write(int offset, float value) {
  void* pcam = Get();
  if (!pcam) return;
  *reinterpret_cast<float*>(reinterpret_cast<char*>(pcam) + offset) = value;
}

}  // namespace ro::camera
