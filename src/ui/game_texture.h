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

// `BuildItemIconGrfPath` : le chemin GRF de l'icône d'un objet, construit par le
// client lui-même — « 유저인터페이스\item\<resname>.bmp ». C'est LA façon de
// nommer un .bmp d'icône : le GRF les range par nom de ressource CP949, jamais
// par id, et `ResolveItemResNameById` fait la traduction depuis la DB d'objets
// (donc même pour un objet qu'on n'a pas en inventaire).
//
// 🔴 SIGNATURE TRANCHÉE AU DÉSASSEMBLAGE (2026-08-23), parce que le projet en
// portait QUATRE, toutes différentes, pour cette seule adresse :
//
//     void __stdcall(const char* id_str, char* out /*>=260*/, int identified)
//
// La fonction finit sur `retn 0Ch` — donc TROIS dwords, qu'elle dépile
// elle-même — et son troisième argument est bel et bien LU (`cmp [ebp+arg_8], 0`
// puis `[eax+8]` si non nul, `[eax+0x1C]` sinon) : c'est lui qui choisit entre
// le nom de ressource IDENTIFIÉ et l'autre.
//
// ⚠ `identified` = 1 pour l'icône normale, 0 pour celle d'un objet non
// identifié. Le déclarer à DEUX arguments — ce que faisait la barre de
// raccourcis — pousse 8 octets quand la fonction en dépile 12 : elle lit alors
// son drapeau dans de la pile NON INITIALISÉE et peut choisir l'autre nom de
// ressource, ou un pointeur invalide. Le SEH de l'appelant masquait la seconde
// moitié du symptôme en perdant simplement l'icône.
//
// ⚠ L'id part en CHAÎNE DÉCIMALE (la fonction fait `atoi` dessus), pas en entier.
constexpr uintptr_t kBuildItemIconPath = 0x00d5a720;
}  // namespace texmgr

// ── Gabarits de chemin de l'interface, lus DANS le binaire du client ─────────
// Ce ne sont pas des adresses de code : ce sont les littéraux CP949 que le
// client garde dans ses données, et qu'on lui reprend tels quels.
//
// 🔴 POURQUOI NE PAS LES ÉCRIRE. Nos sources sont en UTF-8 : un
// « 유저인터페이스\… » écrit ici serait encodé en UTF-8 et ne désignerait AUCUNE
// entrée du GRF, qui est en CP949. Les lire dans le binaire est la seule façon
// d'obtenir les bons octets sans coller d'échappement hexadécimal illisible.
//
// `kUiRootSample` est un chemin COMPLET dont la plupart des appelants ne
// gardent que le PRÉFIXE — le dossier racine de l'interface. Cinq fichiers le
// déclaraient : quatre le nommaient d'après le fichier qu'il désigne
// (`kBtnbarPath`), un d'après l'usage qu'il en fait (`kUiPrefixPath`). Deux noms
// pour deux façons de s'en servir, une seule chaîne.
namespace uipath {
constexpr uintptr_t kUiRootSample = 0x010357b8;  // « …\basic_interface\btnbar_left.bmp »
constexpr uintptr_t kIconWeight   = 0x0103db00;  // « …\inventory\icon_weight.bmp »
}  // namespace uipath

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

// La même texture, MÉMORISÉE par chemin — y compris l'échec, qui est un résultat
// et non une raison de retenter à chaque frame. C'est ce que ro::ItemIcon fait
// depuis un nameid ; ici la clé est le chemin lui-même, pour les surfaces où il
// vient d'ailleurs que de la DB d'items (une balise `<IMG>` d'un script NPC, par
// exemple).
//
// Le cache se vide seul au reset de device (Overlay_DeviceEpoch) : l'appelant
// n'a rien à surveiller, il redemande simplement à chaque frame — un hit ne coûte
// qu'une recherche de table.
GameTexture CachedTextureFromGameFile(const char* path);

// Vide ce cache, textures relâchées.
//
// 🔴 À appeler quand le CONTENU d'un chemin change sans que le device bouge : le
// seul cas connu est le changement de skin, qui purge le gestionnaire de textures
// du client pour que les mêmes chemins servent d'autres images. Sans cela, nos
// copies mémorisées afficheraient l'ancien skin jusqu'au prochain reset de device.
//
// ⚠ JAMAIS pendant une frame ImGui : relâcher une texture que la frame en cours
// référence encore corrompt le tas (feedback_texture_release_defer_frame).
// L'appelant diffère au tick — c'est de toute façon obligatoire pour le
// changement de skin lui-même, qui est une commande native.
void InvalidateGameTextures();

// Mêmes pixels, mais rendus à l'APPELANT au lieu d'être téléversés au GPU : pour
// les traitements côté CPU (l'éditeur d'emblème importe ainsi une icône d'item,
// qui fait justement 24x24 comme un emblème).
//
// `argb` reçoit w*h pixels en B, G, R, A — l'alpha vaut 0 sur le magenta pur
// (color-key RO), 255 ailleurs, exactement comme la texture rendue. false si le
// fichier est absent ou hors format.
bool GameFilePixels(const char* path, std::vector<uint8_t>* argb, int* w, int* h);

}  // namespace ro
