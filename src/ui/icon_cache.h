#pragma once

// ── Cache d'icônes d'item ────────────────────────────────────────────────────
// Charge l'icône d'un item par son nameid via le TexMgr natif, la convertit en
// texture ImGui (magenta -> transparent) et la garde en cache.
//
// Ce code existait à l'IDENTIQUE dans six plugins — storage, inventaire,
// cash shop, boutique NPC, échange, dialogue NPC — chacun avec sa copie des
// cinq adresses natives, de la fonction SEH qui lit les pixels, de la boucle de
// colorkey et du garde d'epoch de device. Six endroits à corriger pour un seul
// bug, et six caches distincts pour les mêmes icônes : ouvrir l'inventaire puis
// le storage chargeait deux fois les mêmes textures.
//
// ⚠ Toute texture rendue ici vit en D3DPOOL_DEFAULT : elle MEURT à un reset de
// device (ALT-TAB en plein écran). Le cache s'en occupe seul, via
// Overlay_DeviceEpoch() — un appelant n'a rien à vider. Ne jamais conserver un
// IconTex d'une frame à l'autre : le redemander est gratuit sur un hit.

#include <cstdint>

namespace ro {

// Texture ImGui d'une icône, avec ses dimensions natives (pour garder le ratio).
// `tex` nul = icône introuvable ; c'est un résultat mémorisé, pas une erreur à
// réessayer chaque frame.
struct IconTex {
  void* tex = nullptr;
  int   w   = 0;
  int   h   = 0;
};

// Icône de l'item `nameid`. `identified` = 0 donne l'icône « non identifié »,
// que le client construit sous un autre chemin — d'où sa présence dans la CLÉ du
// cache et pas seulement dans l'appel. Les copies précédentes ne mémorisaient
// que le nameid : le premier état vu figeait l'icône pour tous les autres, et un
// item non identifié gardait son icône terne une fois identifié.
IconTex ItemIcon(uint32_t nameid, int identified = 1);

}  // namespace ro
