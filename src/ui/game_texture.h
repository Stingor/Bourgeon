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
#include <cstdio>
#include <vector>

namespace ro {

// Une texture ImGui et ses dimensions natives (pour préserver le ratio).
// `tex` nul = chargement impossible.
struct GameTexture {
  void* tex = nullptr;
  int   w   = 0;
  int   h   = 0;
};

// ── Gestionnaire de RESSOURCES natif (client 20250716) ──────────────────────
// 🔴 SON NOM MENT, ET LE PROJET S'EST FAIT PRENDRE. Le client l'appelle
// « UITextureMgr », mais `kLoad` aiguille **par EXTENSION de fichier** vers un
// chargeur par type (`ResolveTypeIndexFromExt`, un objet-fabrique par index) :
// c'est un gestionnaire de RESSOURCES. D'où le fait, longtemps inexpliqué dans
// nos sources, que la parade de connexion lui demande un `.wav` et l'aperçu
// d'effet de couvre-chef un `.str` — ce ne sont pas des détournements, c'est
// l'usage prévu. Une extension inconnue rend nullptr sans rien charger.
namespace texmgr {
constexpr uintptr_t kGet = 0x00a90350;  // __cdecl() -> mgr

// 🔴 `UIPath_ResolveSkin` — s'appelait ici `kMakeKey`, ce qui décrivait un objet
// que cette fonction NE CONSTRUIT PAS. Désassemblage (2026-08-24) : elle rend un
// `const char*`, et son travail est la résolution de SKIN. Si le chemin commence
// par la racine d'interface — les 14 octets CP949 `유저인터페이스`, vérifiés à
// 0x010253b4 — elle tente le même chemin sous le dossier du skin actif, puis un
// repli « UI\ », en ne gardant la réécriture que si la ressource EXISTE vraiment
// (`UITextureMgr_ResourceExists`, 0x00a8e500). Dans tous les autres cas elle
// rend le pointeur d'entrée TEL QUEL — c'est pourquoi un nom de .wav peut
// sauter l'appel sans la moindre conséquence.
//
// ⚠ NON RÉENTRANTE : le chemin réécrit pointe dans l'un de DEUX std::string
// GLOBAUX (0x0159d654 / 0x0159d670). Le résultat doit être consommé avant le
// prochain appel ; ne jamais le conserver.
constexpr uintptr_t kResolveSkinPath = 0x00a9f030;  // __cdecl(path) -> path

// `UIResourceMgr_LoadByPath` — __thiscall(mgr, path). Le troisième argument de
// la forme __fastcall ci-dessous n'est PAS une clé : c'est le chemin lui-même
// (l'EDX est le fourre-tout habituel de l'émulation __thiscall).
//
// Ce qu'il fait du chemin, et qui se voit sur les appelants : il le copie dans
// un tampon de 260 (donc TRONQUE au-delà), le passe dans une table de
// translation (0x012154c8 — un tolower ASCII pur : `/` n'est PAS converti en
// `\`, à l'appelant de mettre des antislashs), puis MÉMORISE le résultat. Deux
// chargements du même chemin rendent le MÊME objet, sans incrémenter le
// compteur de références — c'est à l'appelant qui veut le garder de faire
// l'AddRef (cf. l'aperçu d'effet de couvre-chef). Échec = nullptr, journalisé
// une seule fois par chemin.
constexpr uintptr_t kLoad = 0x00a8d4a0;  // __thiscall(mgr, path) -> res

// Comptage de références de la ressource rendue par kLoad — __fastcall, l'objet
// en ECX. `kLoad` ne l'incrémente PAS : qui veut GARDER une ressource au-delà de
// l'appel doit prendre sa référence, et la rendre. Deux fichiers les
// déclaraient, sous `kResAddRef` et `kTexAddRef` — même fonction, et le second
// nom laissait croire qu'elle ne valait que pour les textures alors qu'elle vaut
// pour tout ce que ce gestionnaire charge.
constexpr uintptr_t kAddRefAddr  = 0x00a8e800;
constexpr uintptr_t kReleaseAddr = 0x00a8f910;

// Champs de l'objet texture rendu par kLoad quand la ressource est un .bmp.
constexpr int kWidth  = 0x114;
constexpr int kHeight = 0x118;
constexpr int kPixels = 0x11c;  // BGRA brut

// ── Le pas de trois, en un appel ────────────────────────────────────────────
// Treize fichiers récitaient la même séquence — obtenir le gestionnaire,
// résoudre le skin, charger — chacun avec ses trois `using …_t` recopiés. Une
// convention d'appel fausse dans une seule de ces copies ne se serait vue qu'à
// l'exécution, et sur un seul écran.
//
// Ces enveloppes ne contiennent que des pointeurs bruts : un `__try` ENGLOBANT
// les couvre, et elles n'introduisent aucun objet à dérouler (pas de C2712).
// Elles ne gardent RIEN — pas de cache, pas d'AddRef : c'est `ro::ItemIcon` et
// `CachedTextureFromGameFile` qui mémorisent, un cran plus haut.
inline void* Mgr() {
  return reinterpret_cast<void* (__cdecl*)()>(kGet)();
}

inline const char* ResolveSkinPath(const char* path) {
  return reinterpret_cast<const char* (__cdecl*)(const char*)>(kResolveSkinPath)(path);
}

// Le chemin est en CP949 et attend des ANTISLASHS (ex. « 유저인터페이스\item\501.bmp »).
// nullptr si le gestionnaire n'est pas prêt, l'extension inconnue, ou le fichier absent.
inline void* LoadResource(const char* path) {
  if (!path || !path[0]) return nullptr;
  void* mgr = Mgr();
  if (!mgr) return nullptr;
  const char* resolved = ResolveSkinPath(path);
  if (!resolved) return nullptr;
  return reinterpret_cast<void* (__fastcall*)(void*, void*, const char*)>(kLoad)(
      mgr, nullptr, resolved);
}

// Même chose SANS résolution de skin, pour les ressources qui ne vivent pas sous
// la racine d'interface (un .wav de monstre, p. ex.) : la résolution y est un
// aller-retour sans effet, et l'appelant qui la saute le dit ainsi plutôt qu'en
// omettant silencieusement une étape.
inline void* LoadResourceRaw(const char* path) {
  if (!path || !path[0]) return nullptr;
  void* mgr = Mgr();
  if (!mgr) return nullptr;
  return reinterpret_cast<void* (__fastcall*)(void*, void*, const char*)>(kLoad)(
      mgr, nullptr, path);
}

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

// L'appel typé, pour que la signature ci-dessus n'ait plus à être recopiée : la
// conversion de l'id en chaîne est faite ici, puisque l'oublier est justement
// l'erreur que la signature nue invite à commettre.
//
// `identified` : 1 sauf si l'on tient l'objet et qu'on le SAIT non identifié.
// L'état n'est PAS une propriété de l'id — il vit dans l'instance
// (`ItemSkillInfo+0x5c`) — donc tout appelant qui n'a qu'un nameid passe 1.
// `out` doit faire au moins 260 octets.
inline void BuildItemIconPath(unsigned nameid, char* out, int identified = 1) {
  char idstr[16];
  std::snprintf(idstr, sizeof(idstr), "%u", nameid);
  out[0] = '\0';
  reinterpret_cast<void (__stdcall*)(const char*, char*, int)>(kBuildItemIconPath)(
      idstr, out, identified);
}
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

// Le dossier racine lui-même, en ÉCHAPPEMENT HEXADÉCIMAL — la seule façon
// d'écrire ces 14 octets CP949 dans une source UTF-8. C'est bien la même chaîne
// que le client garde à 0x010253b4, vérifiée octet à octet (2026-08-24), et
// c'est le préfixe que `kResolveSkinPath` reconnaît pour tenter le skin actif.
//
// Onze fichiers la recopiaient sous SIX noms (kUIDir, kUiRoot, kUiDirCp949 et
// trois copies anonymes en pleine expression). Une seule d'entre elles suffisait
// à se tromper d'un octet pour donner un chemin qui n'existe dans aucun GRF —
// et l'erreur ne se serait vue que sur l'écran concerné, sous forme d'une image
// manquante, jamais d'un message.
inline constexpr char kUiRoot[] = "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA";

// Deux autres littéraux du binaire, chacun déclaré dans deux fichiers.
constexpr uintptr_t kIconNum       = 0x0103dad4;  // « …\basic_interface\num_%d.bmp » (compteurs)
constexpr uintptr_t kFmtMonsterSpr = 0x0103181c;  // « 몬스터\%s.spr » — le dossier des MONSTRES

// Remplace le NOM DE FICHIER d'un chemin de l'exe par `file`, en gardant son
// dossier. C'est ce qui permet de ne JAMAIS réécrire un préfixe CP949 à la
// main : on part d'une chaîne du binaire et on n'en change que la dernière
// composante.
//
// Quatre fichiers en portaient une variante — trois sous le nom
// `BasicInterfacePath`, figé sur `kUiRootSample`, et un sous cette forme
// générale dont il tirait aussi `inventory\` et `styleshop\`.
void WithFileName(uintptr_t exe_path, const char* file, char* out, size_t out_size);

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
