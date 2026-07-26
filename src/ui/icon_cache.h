#pragma once

// ── Cache d'icônes d'item ────────────────────────────────────────────────────
// Mémorise l'icône d'un item par nameid. Le CHARGEMENT lui-même appartient à
// ui/game_texture.h — ici il n'y a que la construction du chemin depuis le
// nameid, et le cache.
//
// Ce code existait à l'identique dans sept plugins — storage, inventaire,
// cash shop, boutique NPC, échange, dialogue NPC, feuille de perso — chacun avec
// sa copie des adresses natives, du chargeur et du garde d'epoch de device. Sept
// caches distincts pour les mêmes icônes : ouvrir l'inventaire puis le storage
// chargeait deux fois les mêmes textures.
//
// ⚠ Ne jamais conserver un IconTex d'une frame à l'autre : les textures meurent
// à un reset de device, et le cache s'en occupe seul (Overlay_DeviceEpoch). Le
// redemander à chaque frame est gratuit sur un hit.

#include <cstdint>

#include "ui/game_texture.h"

namespace ro {

// Une icône est une texture du jeu comme une autre. Le nom reste : les appelants
// manipulent des icônes, et `IconTex ic = ItemIcon(...)` se lit mieux que
// `GameTexture`.
using IconTex = GameTexture;

// Icône de l'item `nameid`. `identified` = 0 donne l'icône « non identifié », que
// le client construit sous un autre chemin — d'où sa présence dans la CLÉ du
// cache et pas seulement dans l'appel. Les copies précédentes ne mémorisaient que
// le nameid : le premier état rencontré figeait l'icône pour tous les autres, et
// un item non identifié gardait son icône terne une fois identifié.
//
// `tex` nul = icône introuvable ; c'est un résultat MÉMORISÉ, pas une erreur à
// réessayer chaque frame.
IconTex ItemIcon(uint32_t nameid, int identified = 1);

// Image « COLLECTION » de l'item : l'art de preview, bien plus grand que l'icône
// d'inventaire — c'est ce que le cash shop affiche. Le client la range sous un
// autre dossier (« …\collection\<resname>.bmp ») et la nomme par son RESNAME, pas
// par son id, d'où le détour par la DB d'items.
//
// Repli sur ItemIcon(nameid) quand l'item n'a pas d'art de collection : mieux
// vaut la petite icône que rien. Le repli est mémorisé ici aussi, sinon un item
// sans collection referait la résolution du resname à chaque frame.
IconTex ItemCollectionIcon(uint32_t nameid);

}  // namespace ro
