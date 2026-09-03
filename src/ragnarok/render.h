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

// Le CENTRE de l'écran, en pixels et en ENTIERS — relevé dans le désassemblage de
// `World_ProjectPointToScreen` 0x00554380, qui finit ses deux coordonnées par
// « … * échelle + *(int*)(ctx + 0x30) » (et 0x34 pour Y). C'est donc la seule
// origine que le client emploie réellement pour poser un point à l'écran.
//
// ⭐ Il vaut la moitié de la taille ci-dessus, et cette redondance sert : deux
// lectures qui doivent s'accorder font un témoin qu'un offset devenu faux ne
// passe pas en silence (cf. grey_world, qui s'en sert avant de jeter des cases).
constexpr int kOffScreenCenterX = 0x30;
constexpr int kOffScreenCenterY = 0x34;

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

// ── Cache de références de sprites ──────────────────────────────────────────
// L'OBJET (pas un pointeur vers lui), voisin immédiat du contexte de rendu
// ci-dessus — 0x24 octets plus loin, ce qui n'est pas un hasard : les deux sont
// des membres du même bloc global de rendu.
//
// Deux fichiers le déclaraient chacun de leur côté, pour deux usages opposés
// qui ne se savaient pas voisins : la barre d'icônes d'état y prend une
// référence de sprite (`SpriteRef` __thiscall(cache, chemin, 0, 0, 1, 0) —
// SIX arguments), et le changement de skin le VIDE, parce que les mêmes chemins
// doivent alors rendre d'autres images.
constexpr uintptr_t kSpriteRefCacheAddr = 0x0125161c;

// ── L'INSERTION d'une primitive dans la file de rendu ────────────────────────
// `RenderQueue_InsertPrimitive` — la file en ECX, deux arguments pile, `retn 8`.
// C'est par elle que passe tout ce que le client soumet au device, et deux
// modules s'y intéressent pour deux raisons opposées : GreyWorld y POSE ses
// quads de cellule, EzEffectCapture la DÉTOURNE pour capturer ce qui y entre.
//
// 🔴 Ils la déclaraient chacun de leur côté, sous deux noms (`kInsertPrimitive`
// et `kRenderQueueInsert`) — donc invisibles l'un à l'autre au relevé, alors
// qu'ils se disputent la même fonction.
constexpr uintptr_t kRenderQueueInsertAddr = 0x00550b10;

}  // namespace render
