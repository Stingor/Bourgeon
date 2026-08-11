#include "features/fx/ground_paint.h"

#include <Windows.h>

#include <cstdint>

#include "imgui.h"
#include "bourgeon.h"                            // Bourgeon::Instance().moonlight_ui()->SaveSettings()
#include "features/moonlight_ui/moonlight_ui.h"  // ColorEdit4WithAlphaBar() (helper standardisé)
#include "ui/ro_imgui.h"                         // ro::RoCheckbox (skin RO du panneau)
#include "utils/hooking/hook_manager.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// Backend actif (DX9 vs DX7) — le « Sol uni » n'existe que sur le chemin de rendu DX9.
// Défini ailleurs.
extern bool g_imgui_dx7_active;

namespace ground_paint {
namespace {

// ── Adresses natives (client 20250716, base 0x400000) ─────────────────────────
//
// ⚠⚠ PIÈGE MAJEUR (constaté en live 2026-07-18) : le client a DEUX chemins de rendu
// complets et parallèles. La famille 0x00552xxx (World_RenderScene 0x00552fa0,
// World_DrawGroundTiles 0x00552710, World_DrawWaterSurface 0x00552b70) est le chemin
// DX7 et n'est JAMAIS exécutée en DX9 : un JMP hook posé sur 0x00552710 s'installe
// correctement (trampoline non nul) mais ne se déclenche pas une seule fois.
//
// Le chemin DX9 réellement emprunté :
//   RendererDX9_RenderScene 0x0055ca60  (BeginScene / EndScene)
//     -> RendererDX9_FlushWorldScene 0x0055e5b0   (jumeau de World_RenderScene)
//          -> RendererDX9_DrawGroundTiles 0x0055d680   <-- LE SOL, notre cible
//          -> FUN_0055d850                              (l'eau)
//     -> FUN_005511d0  (RESET des buckets, PAS un flush — ne pas s'y tromper)
//
// MÊME objet et MÊMES listes que la version DX7 (param_1 est un int*, donc [n] =
// octet n*4) : [0x57]/[0x58] = +0x15c/+0x160 (sol), [0x6c]/[0x6d] = +0x1b0/+0x1b4
// (lightmap). Deux boucles, SetTexture puis DrawPrimRecord — identique.
//
// La fonction du sol ne touche NI COLOROP NI COLORARG1 : elle ne pose que l'adressage
// (FUN_005486e0(w, 0, 1, v) / (w, 0, 2, v) = ADDRESSU/ADDRESSV, v=3 CLAMP pour le sol,
// v=1 WRAP pour la lightmap — l'équivalent DX9 du SetTextureStageState(0, 0xc, v) =
// D3DTSS_ADDRESS du chemin DX7). On peut donc simplement ENCADRER l'appel original :
// stage 0 en COLOROP = SELECTARG1 / COLORARG1 = TFACTOR + TEXTUREFACTOR = notre
// couleur. Les deux couches sortent unies, la géométrie et le z-buffer restent
// intacts (l'occlusion par le terrain reste correcte), et on restaure MODULATE/
// TEXTURE en sortie car le stage 0 sert à TOUTE la suite de la scène.
constexpr uintptr_t kDX9DrawGround = 0x0055d680;  // RendererDX9_DrawGroundTiles(this = renderer DX9)

// On tape DIRECTEMENT sur le vrai IDirect3DDevice9, pas sur le wrapper d'état en
// +0x25c : les slots de vtable du wrapper (+0x50/+0x8c/+0x94) venaient de commentaires
// Ghidra déjà pris en défaut, et une sonde a montré que +0x50 tombe sur une fonction
// qui charge scr_logo.bmp.
//
// PREUVE que +0x260 est le device COM brut (RendererDX9_DrawPrimRecord 0x0055c830,
// = vtable renderer 0x00fd6d64 slot +0x38, appelée ici même via (**(*param_1+0x38))) :
//   piVar2 = *(this + 0x260);  (**(piVar2 + 0x14c))(...)  = DrawPrimitiveUP
// 0x14c / 4 = 83 = l'index EXACT de DrawPrimitiveUP dans IDirect3DDevice9 -> c'est
// bien la vtable D3D9 standard, donc les index 57 et 67 ci-dessous sont sûrs.
constexpr int kOffD3D9Device = 0x260;   // renderer DX9 -> IDirect3DDevice9*
constexpr int kVtSetRenderState      = 57 * 4;  // 0x0e4
constexpr int kVtSetTextureStageState = 67 * 4;  // 0x10c

constexpr unsigned kD3DRS_TEXTUREFACTOR = 60;
constexpr unsigned kD3DTSS_COLOROP      = 1;
constexpr unsigned kD3DTSS_COLORARG1    = 2;
constexpr unsigned kD3DTOP_SELECTARG1   = 2;
constexpr unsigned kD3DTOP_MODULATE     = 4;
constexpr unsigned kD3DTA_TEXTURE       = 2;
constexpr unsigned kD3DTA_TFACTOR       = 3;

constexpr uintptr_t kDX9DrawPrimRec  = 0x0055c830;  // vtbl +0x38 : draw mono-texture
constexpr uintptr_t kDX9DrawPrimDual = 0x0055c8c0;  // vtbl +0x34 : draw BI-texture (aniso)
constexpr uintptr_t kDX9DrawTerrain  = 0x0055d850;  // RendererDX9_DrawTerrainSurfaces

using DrawGroundFn = void(__fastcall*)(void*);
DrawGroundFn g_orig_draw_ground = nullptr;

// > 0 pendant l'exécution de RendererDX9_DrawGroundTiles : c'est ce qui distingue les
// primitives du SOL de toutes les autres dans Hooked_DrawPrimRecord (mono-thread rendu).
int g_in_ground_pass = 0;

bool  g_enabled  = false;
float g_col[4]   = {0.0f, 0.0f, 0.0f, 1.0f};  // noir opaque par défaut

// NOTE : RendererDX9_DrawGroundTiles 0x0055d680 s'exécute avec ses deux listes VIDES
// (mesuré en live). On la garde hookée par sécurité — si une carte l'utilisait, la
// couleur s'appliquerait aussi — mais le terrain réel passe par
// RendererDX9_DrawTerrainSurfaces (cf. Hooked_DrawTerrain).
void __fastcall Hooked_DrawGround(void* self) {
  ++g_in_ground_pass;
  if (g_orig_draw_ground) g_orig_draw_ground(self);
  --g_in_ground_pass;
}

// ── Le VRAI point d'injection : juste avant le DrawPrimitiveUP ────────────────
// Poser COLOROP/COLORARG1 autour de l'appel à RendererDX9_DrawGroundTiles NE MARCHE
// PAS, alors que les appels renvoient pourtant S_OK. Raison : l'objet en renderer+0x25c
// est un CACHE D'ÉTATS (FUN_00547990 recopie 0xd2 dwords depuis wrapper+8 = render
// states, 0x42 depuis wrapper+0x350 = texture stage states, 0x1c depuis wrapper+0x458).
// Nos écritures vont directement sur le device COM et court-circuitent ce cache : tout
// re-push des valeurs cachées entre notre écriture et le draw réel écrase notre COLOROP,
// silencieusement.
//
// RendererDX9_DrawPrimRecord 0x0055c830 est le dernier maillon : il fait
// Device_SetFVFCached puis appelle DrawPrimitiveUP (vtbl +0x14c) SUR LE DEVICE BRUT
// (this+0x260), sans aucun flush d'état entre les deux. Écrire juste avant l'original
// est donc inattaquable — plus rien ne peut s'intercaler.
//
// __thiscall(this = renderer, rec) -> émulé en __fastcall (edx ignoré).
using DrawPrimRecFn = void(__fastcall*)(void*, void*, void*);
DrawPrimRecFn g_orig_draw_prim = nullptr;

// Pose la couleur unie sur le stage 0, appelle le draw natif, puis restaure. Le stage 0
// sert à TOUTE la suite de la scène : on remet exactement les valeurs que le cache du
// wrapper croit actives (MODULATE / TEXTURE), donc cache et device restent cohérents.
void PaintAroundDraw(void* self, void* edx, void* rec, DrawPrimRecFn orig) {
  __try {
    void* dev = *reinterpret_cast<void**>(reinterpret_cast<char*>(self) + kOffD3D9Device);
    if (!dev) {
      if (orig) orig(self, edx, rec);
      return;
    }
    void** vt = *reinterpret_cast<void***>(dev);
    // Méthodes COM : __stdcall, `this` en 1er argument sur la pile.
    auto SetRS = reinterpret_cast<long(__stdcall*)(void*, unsigned, unsigned)>(
        vt[kVtSetRenderState / sizeof(void*)]);
    auto SetTSS = reinterpret_cast<long(__stdcall*)(void*, unsigned, unsigned, unsigned)>(
        vt[kVtSetTextureStageState / sizeof(void*)]);

    auto ch = [](float v) -> unsigned {
      int i = static_cast<int>(v * 255.0f + 0.5f);
      return static_cast<unsigned>(i < 0 ? 0 : (i > 255 ? 255 : i));
    };
    const unsigned argb = (ch(g_col[3]) << 24) | (ch(g_col[0]) << 16) |
                          (ch(g_col[1]) << 8) | ch(g_col[2]);

    SetRS(dev, kD3DRS_TEXTUREFACTOR, argb);
    SetTSS(dev, 0, kD3DTSS_COLORARG1, kD3DTA_TFACTOR);
    SetTSS(dev, 0, kD3DTSS_COLOROP, kD3DTOP_SELECTARG1);
    if (orig) orig(self, edx, rec);
    SetTSS(dev, 0, kD3DTSS_COLOROP, kD3DTOP_MODULATE);
    SetTSS(dev, 0, kD3DTSS_COLORARG1, kD3DTA_TEXTURE);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void __fastcall Hooked_DrawPrimRecord(void* self, void* edx, void* rec) {
  if (g_in_ground_pass <= 0 || !g_enabled || g_imgui_dx7_active || !self) {
    if (g_orig_draw_prim) g_orig_draw_prim(self, edx, rec);
    return;
  }
  PaintAroundDraw(self, edx, rec, g_orig_draw_prim);
}

// Jumeau BI-TEXTURE : vtbl +0x34 = RendererDX9_DrawPrimRecordDualTex 0x0055c8c0.
// La branche aniso de RendererDX9_DrawTerrainSurfaces dessine le terrain PAR CE
// SLOT-LÀ (SetTexture sur les stages 0 ET 1), pas par +0x38 — c'est la raison pour
// laquelle le compteur restait à 0 avec le seul hook sur +0x38.
DrawPrimRecFn g_orig_draw_prim_dual = nullptr;

void __fastcall Hooked_DrawPrimRecordDual(void* self, void* edx, void* rec) {
  if (g_in_ground_pass <= 0 || !g_enabled || g_imgui_dx7_active || !self) {
    if (g_orig_draw_prim_dual) g_orig_draw_prim_dual(self, edx, rec);
    return;
  }
  PaintAroundDraw(self, edx, rec, g_orig_draw_prim_dual);
}

// Marqueur de passe pour RendererDX9_DrawTerrainSurfaces 0x0055d850 — LA fonction qui
// dessine réellement le terrain (3 branches selon OptionInfo 0x77 et le support aniso :
// listes +0x168, +0x1f8 via vtbl +0x34, ou +0x174 via vtbl +0x38). Prouvé en live :
// RendererDX9_DrawGroundTiles 0x0055d680 s'exécute avec ses DEUX listes VIDES.
DrawGroundFn g_orig_draw_terrain = nullptr;

void __fastcall Hooked_DrawTerrain(void* self) {
  ++g_in_ground_pass;
  if (g_orig_draw_terrain) g_orig_draw_terrain(self);
  --g_in_ground_pass;
}

}  // namespace

// ── API publique ──────────────────────────────────────────────────────────────
bool&  enabled() { return g_enabled; }
float* color()   { return g_col; }

void EnsureInstalled() {
  static bool done = false;
  if (done) return;
  done = true;
  using namespace hooking;
  // Un trampoline nul = SetHook a échoué (prologue non relocalisable) : le hook ne
  // doit alors JAMAIS avaler l'appel, sinon le sol disparaît au lieu de changer de
  // couleur. C'est pourquoi chaque hook teste son original avant de le chaîner.
  g_orig_draw_ground = reinterpret_cast<DrawGroundFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kDX9DrawGround),
          reinterpret_cast<uint8_t*>(&Hooked_DrawGround)));
  g_orig_draw_prim = reinterpret_cast<DrawPrimRecFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kDX9DrawPrimRec),
          reinterpret_cast<uint8_t*>(&Hooked_DrawPrimRecord)));
  g_orig_draw_prim_dual = reinterpret_cast<DrawPrimRecFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kDX9DrawPrimDual),
          reinterpret_cast<uint8_t*>(&Hooked_DrawPrimRecordDual)));
  g_orig_draw_terrain = reinterpret_cast<DrawGroundFn>(
      HookManager::Instance().SetHook(HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kDX9DrawTerrain),
          reinterpret_cast<uint8_t*>(&Hooked_DrawTerrain)));
}

void DrawSettings() {
  bool changed = ro::RoCheckbox(i18n::Tr("Sol uni (fond de capture)"), &g_enabled);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(i18n::Tr("Repeint tout le terrain .gnd d'une couleur unie, sans toucher au reste\n"
                      "de la scène : la géométrie et le z-buffer du sol restent intacts, donc\n"
                      "l'occlusion par le terrain reste correcte.\n"
                      "L'eau, le ciel et le brouillard ne sont PAS affectés.\n"
                      "DX9 uniquement (le chemin de rendu DX7 est une autre famille de "
                      "fonctions)."));
  if (g_imgui_dx7_active) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), i18n::Tr("(DX7 : non supporté)"));
  }
  if (g_enabled) {
    // Poser les hooks dès l'activation : ils ne servent à rien tant que la case est
    // décochée, et EnsureInstalled est idempotent.
    EnsureInstalled();
    ImGui::SetNextItemWidth(ro::Px(200.0f));
    ColorEdit4WithAlphaBar(i18n::Tr("Couleur du sol"), g_col);
    // Le picker renvoie true à CHAQUE frame de drag : on ne persiste qu'au relâchement,
    // sinon on réécrit tout le YAML des dizaines de fois par seconde.
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    ImGui::TextDisabled(i18n::Tr("L'alpha est ignoré (la passe du sol est opaque)."));
  }
  if (changed) {
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}

}  // namespace ground_paint
