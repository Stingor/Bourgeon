#pragma once

// ── Contexte de rendu, atlas de sprites, animations ──────────────────────────
// (client 20250716, base 0x400000 ; cf. docs/sprite_rendering_re.md)
//
// kContextPtr portait QUATRE noms dans le projet — kSceneCtxPtr,
// kRendererObjPtr, kViewportPtr et kRenderContextPtrVa — parce que chaque module
// l'a baptisé d'après le seul champ qu'il en lisait : l'atlas de sprites pour
// les uns (+0xc0), la taille du viewport pour un autre (+0x28/+0x2c), la vtable
// SetView pour la caméra FPS, une simple adresse pour le patch de profondeur des
// hat effects. C'est un seul et même objet ; les quatre descriptions en sont des
// facettes. D'où le nom neutre, et la liste des champs connus ci-dessous.
//
// En-tête MINUSCULE (<cstdint> seul), comme uiwnd.h, globals.h et item_db.h.

#include <cstdint>

namespace render {

// ── Le contexte de rendu ─────────────────────────────────────────────────────
// ⚠ EMPLACEMENT d'un pointeur : à DÉRÉFÉRENCER (contrairement à
// rag::kSessionAddr ou uiwnd::kUIWindowMgrAddr, qui sont les objets eux-mêmes).
constexpr uintptr_t kContextPtr = 0x012515f8;

inline void* Context() { return *reinterpret_cast<void**>(kContextPtr); }

// Champs connus du contexte.
constexpr int kOffViewportW  = 0x28;
constexpr int kOffViewportH  = 0x2c;
constexpr int kOffSpriteAtlas = 0xc0;

inline int ViewportWidth() {
  void* ctx = Context();
  return ctx ? *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ctx) + kOffViewportW) : 0;
}
inline int ViewportHeight() {
  void* ctx = Context();
  return ctx ? *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ctx) + kOffViewportH) : 0;
}

// L'atlas de sprites vit DANS le contexte (objet embarqué, pas un pointeur) :
// son adresse est ctx + 0xc0, d'où l'absence de déréférencement final.
inline void* SpriteAtlas() {
  void* ctx = Context();
  return ctx ? reinterpret_cast<uint8_t*>(ctx) + kOffSpriteAtlas : nullptr;
}

// ── Atlas de sprites ─────────────────────────────────────────────────────────
// GetCached rend la texture déjà construite pour (cellule, palette, géométrie),
// ou 0 ; Build la construit. Les deux sont __thiscall sur l'atlas — le projet
// les écrit indifféremment `__thiscall(atlas, …)` ou `__fastcall(atlas, edx, …)`,
// ce qui produit le même appel.
constexpr uintptr_t kAtlasGetCachedAddr = 0x00566b70;
constexpr uintptr_t kAtlasBuildAddr     = 0x005663d0;

// ── Animations (.act) ────────────────────────────────────────────────────────
// Act_GetFrame(act, action, frame) -> frame*, __thiscall.
constexpr uintptr_t kActionGetFrameAddr = 0x0070f4b0;

}  // namespace render
