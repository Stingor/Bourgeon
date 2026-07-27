#pragma once

// ── Textures du client ───────────────────────────────────────────────────────
// Charger un .bmp du jeu par son chemin et en faire une texture ImGui : la
// primitive dont ro::ItemIcon n'est qu'un cas particulier (chemin construit
// depuis un nameid).
//
// Elle était recopiée sous trois noms — LoadTexByPath (inventaire, storage),
// LoadSkillIcon (feuille de perso), LoadItemIcon (les six plugins d'items) — et
// chaque copie traînait avec elle la fonction SEH qui lit les pixels bruts, la
// boucle de colorkey magenta, et sa propre déclaration des adresses natives.
//
// L'adresse du TexMgr (0x00a90350) apparaît encore dans une dizaine de fichiers
// pour d'AUTRES usages que le rendu ImGui (préchargement, handle natif passé au
// moteur). Elle est donc exposée ici sous `texmgr::` : ces appelants peuvent s'y
// référer sans la redéclarer, même quand TextureFromGameFile ne leur convient pas.

#include <cstdint>
#include <vector>

namespace ro {

// Une texture ImGui et ses dimensions natives (pour préserver le ratio).
// `tex` nul = chargement impossible.
struct GameTexture {
  void* tex = nullptr;
  int   w   = 0;
  int   h   = 0;
};

// Adresses du gestionnaire de textures natif (client 20250716).
namespace texmgr {
constexpr uintptr_t kGet     = 0x00a90350;  // __cdecl() -> mgr
constexpr uintptr_t kMakeKey = 0x00a9f030;  // __cdecl(path) -> key
constexpr uintptr_t kLoad    = 0x00a8d4a0;  // __fastcall(mgr, _, key) -> tex
// Champs de l'objet texture renvoyé par kLoad.
constexpr int kWidth  = 0x114;
constexpr int kHeight = 0x118;
constexpr int kPixels = 0x11c;  // BGRA brut
}  // namespace texmgr

// Charge `path` (encodé en CP949, ex. « 유저인터페이스\item\501.bmp ») via le
// TexMgr natif et le convertit en texture ImGui.
//
// Le magenta pur (0xFF00FF) devient TRANSPARENT : c'est le color-key historique
// de RO, les .bmp du client ne portent pas de canal alpha.
//
// ⚠ La texture rendue vit en D3DPOOL_DEFAULT : elle MEURT à un reset de device
// (ALT-TAB en plein écran). L'appelant qui la conserve doit la recharger quand
// Overlay_DeviceEpoch() change — ro::ItemIcon le fait déjà pour ses icônes.
//
// Sûr à appeler même si le device ou la session ne sont pas prêts : rend une
// texture nulle plutôt que de lever (le chemin natif est gardé par un SEH).
GameTexture TextureFromGameFile(const char* path);

// Mêmes pixels, mais rendus à l'APPELANT au lieu d'être téléversés au GPU : pour
// les traitements côté CPU (l'éditeur d'emblème importe ainsi une icône d'item,
// qui fait justement 24x24 comme un emblème).
//
// `argb` reçoit w*h pixels en B, G, R, A — l'alpha vaut 0 sur le magenta pur
// (color-key RO), 255 ailleurs, exactement comme la texture rendue. false si le
// fichier est absent ou hors format.
bool GameFilePixels(const char* path, std::vector<uint8_t>* argb, int* w, int* h);

}  // namespace ro
