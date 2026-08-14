#include "features/fx/palette_inject.h"

#include <Windows.h>  // SEH autour des déréférencements d'objets natifs

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "ragnarok/globals.h"  // kModeMgrAddr / kModeMgrGetActiveAddr
#include "ragnarok/own_actor.h"  // ActorSlotSpritePath (le .spr d'un emplacement)
#include "ui/sprite_path.h"      // HairPaletteRelForSprite
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

namespace fx {
namespace palette_inject {
namespace {

// ── Adresses natives (client 20250716, base 0x400000) ────────────────────────
//
// Toutes VÉRIFIÉES EN MÉMOIRE LIVE le 2026-08-11, jeu attaché — pas seulement
// lues dans l'IDB, qui est vanilla alors que l'exe déployé porte ~110 patchs
// WARP.

// `UITextureMgr_FindCachedRes` — le POINT DE PASSAGE OBLIGÉ des deux chargeurs.
// 🔴 C'est ce qui en fait le bon endroit : `UITextureMgr_Load` (0x00a8d4a0) et
// `UITextureMgr_LoadResourceByName` (0x00a8d9f0) sont du code dupliqué qui
// converge ici. Hooker l'une des deux seulement laisserait la moitié des
// fenêtres au comportement natif.
// Convention établie par le désassemblage, pas par le décompilé : la fonction
// finit par `ret 8` (0x00a8d491) ⇒ DEUX arguments pile.
constexpr uintptr_t kFindCachedRes = 0x00A8D3E0;
constexpr uint8_t kFindCachedResPrologue[5] = {0x55, 0x8B, 0xEC, 0x81, 0xEC};

// `CActorSprite_RebuildBodyPalettePath` — le seul endroit du chemin « corps »
// qui connaisse À LA FOIS l'acteur et sa palette. Le producteur du chemin,
// `Job_BuildBodyPalettePath_impl` (0x00b42580), ne reçoit que (job, sexe,
// couleur) : il ne peut pas savoir DE QUI il s'agit.
// Finit par `ret` nu (0x00d3dd54) ⇒ AUCUN argument pile.
constexpr uintptr_t kRebuildBodyPalettePath = 0x00D3DC90;
constexpr uint8_t kRebuildPrologue[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};

// `CActorSprite_RebuildHeadPalette` — l'exact pendant du précédent, pour la
// TÊTE. Elle recalcule le chemin de palette de cheveux et l'écrit dans
// l'acteur :
//
//     if (this[271])                                  // acteur+1084, la garde
//       std_string_assign(this + 284, Job_GetHeadPalettePath(...));  // +0x470
//
// 🔴 Elle ÉCRASE donc exactement ce que `ApplyHairToActor` vient d'y poser. Sans
// détour ici, toute reconstruction de tête rend au joueur sa couleur native :
// symptôme observé le 2026-08-12 — après un changement de coiffure, le joueur
// était SEUL à ne pas voir sa propre couleur, les autres la conservant parce
// qu'ils réappliquent la recette reçue au tick suivant.
//
// Même prologue que le corps (`55 8B EC 6A FF`), et même convention : `retn` nu,
// donc aucun argument pile.
constexpr uintptr_t kRebuildHeadPalette = 0x00D3DED0;

// `Palette_ConvertRgbaToArgb1555(dst, srcRGBA, 256)` — la primitive du CLIENT.
// On ne la réimplémente pas : c'est elle qui fixe le format que le blit lit, y
// compris son cas particulier (magenta pur -> 0x421).
constexpr uintptr_t kConvertRgbaToArgb1555 = 0x00566770;

// `std::string::assign(this, src, len)` du CLIENT. 🔴 Indispensable : la chaîne
// de l'acteur a été allouée par le CRT statique du jeu ; la remplir avec notre
// runtime ferait libérer son tampon par l'`operator delete` du jeu — un free
// inter-tas.
constexpr uintptr_t kStdStringAssign = 0x004F1940;

// Vtable de `CPaletteRes`. Les consommateurs de palette n'appellent AUCUNE
// méthode virtuelle sur l'objet (vérifié sur les 4 sites : ils ne font qu'un
// `add eax, 0x510`), donc un bloc à nous n'en a pas besoin — mais la poser ne
// coûte rien et rend l'objet reconnaissable à un débogueur.
constexpr uintptr_t kPaletteResVtable = 0x01011DBC;

// ── Disposition d'un CRes, LUE sur une CPaletteRes vivante ───────────────────
constexpr int kOffResPin = 0x04;    // 0 = purgeable par sub_A8F810
constexpr int kOffResStamp = 0x08;  // timeGetTime, réécrit par le cache hit
constexpr int kOffResType = 0x0C;   // index de type (16 pour `pal`)
constexpr int kOffResHash = 0x10;   // empreinte du chemin
constexpr int kOffResPath = 0x14;   // le chemin résolu, en clair
constexpr int kOffResRgba = 0x110;  // 1024 o RGBA, tels que lus du fichier
constexpr int kOffResArgb = 0x510;  // 256 dwords ARGB1555 — CE QUE LE RENDU LIT
constexpr size_t kPaletteResSize = 0x910;

// 🔴 Index de type de l'extension `pal` = 16. Il dépend de l'ORDRE D'EXÉCUTION
// des initialiseurs statiques et n'est pas déductible du binaire : mesuré en RAM
// le 2026-08-11, confirmé trois fois (rang dans le vecteur des prototypes de
// 0x0159D450+0x14, préfixe "palette\" correspondant, et champ +0x0c d'un objet
// réel). Si un jour le client change, c'est la PREMIÈRE chose à re-mesurer.
constexpr int kPaletteTypeIndex = 16;

// Offsets sur le CActorSprite (vtable 0x01094810).
constexpr int kOffActorPalettePath = 0x1C;    // std::string, relative à `palette\`
constexpr int kOffActorClothesColor = 0x48;   // 🔴 == 0 ⇒ AUCUNE palette externe
constexpr int kOffActorGid = 0x110;
// Tête : chemin de palette de CHEVEUX et « couleur de cheveux » qui l'active.
// 🔴 Issus de la reconnaissance de `CActorSprite_BuildPartQuads` (0x605c30) :
// « partie 1 (TÊTE) et acteur+1084 > 0 → charge acteur+0x470 ». Même schéma que
// le corps, garde à zéro comprise.
constexpr int kOffActorHairPalettePath = 0x470;
constexpr int kOffActorHairColor = 1084;  // 0x43C

// (La coupe de cheveux vivait ici : `CActorSprite_BuildHead_Slot1` 0x00D3F4F0,
// qui range la coupe en acteur+1080 puis rebâtit l'emplacement 1. Retirée le
// 2026-08-12 — voir le .h. L'adresse reste notée dans docs/ au cas où.)

// Le marqueur de nos chemins. ASCII pur et tout en minuscules : le natif passe
// le chemin au `_strlwr` de `Grf_NormalizePath`, et un dossier coréen nous
// vaudrait le piège d'encodage déjà payé ailleurs.
constexpr char kMarker[] = "palette\\bourgeon\\";
constexpr size_t kMarkerLen = sizeof(kMarker) - 1;
// Le même marqueur SANS le préfixe de type : c'est sous cette forme que le
// chemin vit dans l'acteur (relatif à `palette\`, comme `몸\body_56.pal`).
constexpr char kMarkerRel[] = "bourgeon\\";
constexpr size_t kMarkerRelLen = sizeof(kMarkerRel) - 1;

using ConvertFn = void(__cdecl*)(void* dst, const void* src, int count);
using StringAssignFn = void*(__thiscall*)(void* self, const void* src, size_t len);
using FindCachedResFn = void*(__fastcall*)(void* mgr, void* edx, int type,
                                           const char* path);
using RebuildFn = void(__fastcall*)(void* actor, void* edx);
// La tête RETOURNE une valeur (le décompilé la propage) : on la relaie plutôt
// que de la perdre, même si les appelants connus n'en font rien.
using RebuildHeadFn = int(__fastcall*)(void* actor, void* edx);

FindCachedResFn g_orig_find = nullptr;
RebuildFn g_orig_rebuild = nullptr;
RebuildHeadFn g_orig_rebuild_head = nullptr;

// ── Le registre des recettes ─────────────────────────────────────────────────
//
// Une entrée par acteur. Le bloc est REMPLACÉ (jamais muté) à chaque version :
// la texture d'atlas est cuite pour la clé {frame, palette}, donc seul un
// POINTEUR NEUF la fait refaire. Muter les octets en place ne se verrait qu'à
// l'éviction, plusieurs secondes plus tard.
struct Entry {
  std::vector<uint8_t> block;  // kPaletteResSize octets
  // La même palette en RGBA, AVANT conversion. Un kilo-octet de plus par acteur,
  // et il évite à tout consommateur hors rendu — les pantins de l'interface —
  // de refaire la fusion, la détection de rampes et l'application de recette
  // pour aboutir aux octets qu'on a déjà.
  std::vector<uint8_t> rgba;
};

std::mutex g_mutex;
std::map<uint32_t, Entry> g_recipes;

// Les GID pour lesquels on a déjà signalé « chemin demandé, aucune recette ».
// Le rendu repasse par le détour à chaque frame : sans ce garde, un corps noir
// écrirait des milliers de lignes par seconde. Remis à zéro par `SetRecipe`,
// pour qu'une PERTE ULTÉRIEURE soit de nouveau signalée.
std::map<uint32_t, bool> g_miss_signale;

// Ce que le NATIF avait choisi, par acteur : son chemin de palette et sa
// couleur de vêtement. Séparés des recettes, car ils doivent survivre à un
// ClearRecipe — c'est tout ce qui permet de rendre au joueur son apparence
// EXACTE, y compris quand on a dû forcer sa couleur pour débloquer le rendu.
struct NativeLook {
  std::string path;
  int clothes_color = -1;
  // 🔴 L'acteur, MÉMORISÉ DEPUIS LE DÉTOUR. C'est la seule source fiable : le
  // détour le reçoit en argument, avec le GID qui va avec. Le retrouver nous-
  // mêmes par la chaîne GameMode -> gestionnaire -> +0x2c suppose que l'acteur
  // du joueur y est ET que son GID vaut l'AID, deux hypothèses de trop.
  // Revalidé (GID relu, sous SEH) avant chaque usage, donc un pointeur devenu
  // caduc est simplement ignoré.
  void* actor = nullptr;

  // 🔴🔴 A-t-on forcé la couleur de vêtement de cet acteur ? Si oui, le chemin
  // de palette que le natif écrit désormais DÉRIVE DE NOTRE VALEUR, plus de
  // celle du serveur — et le re-capturer empoisonnerait la base.
  //
  // Le cycle, mesuré (2026-08-11) : couleur d'origine 0 ⇒ le natif n'écrit
  // AUCUNE palette externe, donc la base est le sprite nu. On force 1 pour
  // débloquer le rendu. À la reconstruction suivante de l'acteur, le natif lit
  // ce 1, produit `몸\body_1.pal`, et sans ce drapeau on le prendrait pour la
  // couleur du joueur : la base deviendrait `sprite ⊕ body_1.pal` sur ce
  // client-là, et le sprite nu sur les autres.
  //
  // Symptôme observé : le MÊME personnage plus sombre chez les autres joueurs
  // que chez lui — le client qui avait subi une reconstruction voyait une base
  // différente. Une recette ne porte aucune couleur : si la base diverge d'un
  // client à l'autre, les couleurs divergent avec elle.
  bool color_forced = false;
};
std::map<uint32_t, NativeLook> g_native_paths;

// Couleur de cheveux imposée, par acteur. Séparée des recettes : elle ne
// fabrique aucun bloc, elle ne fait que désigner un fichier existant.
std::map<uint32_t, int> g_hair;


// 🔴🔴 LES BLOCS RETIRÉS NE SONT JAMAIS LIBÉRÉS — et c'est la seule façon
// correcte de faire.
//
// Le cache d'atlas du client indexe ses textures par la PAIRE {image, palette},
// donc par l'ADRESSE de notre bloc. Rendre un bloc au tas laisse cette entrée
// vivante ; l'allocateur rend alors très volontiers la même adresse au bloc
// suivant, et l'atlas croit posséder déjà la texture de cette clé : il ressort
// l'ANCIENNE image.
//
// Symptôme observé (2026-08-11) avant ce correctif : la couleur se figeait au
// noir, revenait parfois au clair sans raison, et basculait sur des seuils
// arbitraires — un réglage de luminosité à -95 donnait des bras rouges, -96 du
// noir. Rien à voir avec le calcul de couleur : c'était une texture périmée
// ressortie du cache.
//
// Une poubelle circulaire ne suffit PAS : elle ne fait que retarder la
// réutilisation de l'adresse. Il faut qu'aucune adresse ne resserve JAMAIS
// tant que le client tourne.
//
// Le coût est borné par le débounce de l'appelant : un bloc fait 2320 octets,
// et une longue séance de réglage en produit quelques centaines — de l'ordre du
// mégaoctet, contre une corruption visuelle inexplicable.
std::vector<std::vector<uint8_t>> g_retired;

void Retire(std::vector<uint8_t>&& block) {
  g_retired.push_back(std::move(block));
}

// Le chemin ne porte QUE le GID, jamais la version. C'est délibéré : ainsi le
// chemin écrit dans l'acteur ne se périme jamais, et changer de recette ne
// demande pas de le réécrire — notre détour rend simplement le bloc COURANT.
void MakePath(uint32_t gid, char* out, size_t out_size) {
  std::snprintf(out, out_size, "bourgeon\\%u.pal", gid);
}

// Le GID encodé dans un chemin déjà préfixé, ou 0 si ce n'est pas l'un des
// nôtres. Comparaison sur les octets bruts : le natif ne normalise qu'APRÈS.
uint32_t GidFromPath(const char* path) {
  if (!path || _strnicmp(path, kMarker, static_cast<int>(kMarkerLen)) != 0)
    return 0;
  const char* p = path + kMarkerLen;
  uint32_t gid = 0;
  if (*p < '0' || *p > '9') return 0;
  for (; *p >= '0' && *p <= '9'; ++p) gid = gid * 10 + static_cast<uint32_t>(*p - '0');
  return (*p == '.') ? gid : 0;
}

// Remplit un bloc neuf à la disposition d'un CPaletteRes.
void BuildBlock(std::vector<uint8_t>* block, const uint8_t* rgba, uint32_t gid) {
  block->assign(kPaletteResSize, 0);
  uint8_t* b = block->data();
  *reinterpret_cast<uintptr_t*>(b) = kPaletteResVtable;
  // Le « pin » anti-purge à 1 : `sub_A8F810` détruit toute ressource dont ce
  // champ vaut 0, avec l'`operator delete` DU JEU — sur notre bloc, ce serait
  // une corruption de tas. Notre objet n'est de toute façon jamais inscrit dans
  // le cache natif, donc la purge ne peut pas le voir ; la ceinture ne coûte
  // rien et protège si un jour on l'y inscrivait.
  *reinterpret_cast<int*>(b + kOffResPin) = 1;
  *reinterpret_cast<int*>(b + kOffResStamp) = 0;
  *reinterpret_cast<int*>(b + kOffResType) = kPaletteTypeIndex;
  *reinterpret_cast<uint32_t*>(b + kOffResHash) = 0;
  char relatif[64];
  MakePath(gid, relatif, sizeof(relatif));
  std::snprintf(reinterpret_cast<char*>(b + kOffResPath),
                kOffResRgba - kOffResPath, "palette\\%s", relatif);
  std::memcpy(b + kOffResRgba, rgba, 1024);
  // La conversion est faite par la primitive du CLIENT, pas par nous : c'est
  // elle qui définit le format que le blit lira.
  reinterpret_cast<ConvertFn>(kConvertRgbaToArgb1555)(b + kOffResArgb,
                                                      b + kOffResRgba, 256);
}

// ── Les détours ──────────────────────────────────────────────────────────────

// Lecture d'un acteur, isolée : MSVC refuse `__try` dans une fonction qui doit
// dérouler des objets C++ (C2712), et le détour en manipule.
bool ReadActorGid(void* actor, uint32_t* gid) {
  __try {
    *gid = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(actor) +
                                        kOffActorGid);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Lit la std::string de chemin de palette portée par l'acteur. Isolée pour le
// SEH, comme les autres lectures d'objets natifs.
bool ReadActorPalettePath(void* actor, char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  __try {
    // std::string MSVC : buffer de 16 octets, puis size, puis capacity. Au-delà
    // de 15 caractères le buffer devient un pointeur — le chemin d'un `.pal`
    // dépasse souvent cette limite, donc les deux cas comptent.
    const char* base = reinterpret_cast<const char*>(actor) + kOffActorPalettePath;
    const size_t size = *reinterpret_cast<const size_t*>(base + 16);
    const size_t cap = *reinterpret_cast<const size_t*>(base + 20);
    const char* p = (cap >= 16) ? *reinterpret_cast<const char* const*>(base)
                                : base;
    if (!p || size == 0 || size >= out_size) return false;
    std::memcpy(out, p, size);
    out[size] = '\0';
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; return false; }
}

// Pose le chemin, et débloque au besoin la garde de couleur. Sous SEH pour la
// même raison que les autres accès à l'acteur.
//
// `unlock_color` : 🔴 sans lui, RIEN ne s'affiche pour un personnage jamais
// teint. Les sites de rendu ne chargent une palette externe que si la couleur de
// vêtement est NON NULLE — à 0, le client garde la palette interne du .spr et ne
// demande jamais de fichier. On ne force que pour les porteurs de recette, et
// seulement dans le cache d'apparence du client : le serveur n'en sait rien.
// En restauration, on ne force PAS — on rend au contraire sa valeur d'origine.
//
// Rend true si la couleur a effectivement été FORCÉE — l'appelant doit alors le
// mémoriser dans `NativeLook::color_forced`, faute de quoi le chemin que le
// natif écrira ensuite (dérivé de notre valeur) serait pris pour celui du
// serveur.
bool ApplyPathToActor(void* actor, const char* relative_path,
                      bool unlock_color, int restore_color = -1) {
  bool forced = false;
  __try {
    char* a = reinterpret_cast<char*>(actor);
    if (unlock_color) {
      if (*reinterpret_cast<int*>(a + kOffActorClothesColor) == 0) {
        *reinterpret_cast<int*>(a + kOffActorClothesColor) = 1;
        forced = true;
      }
    } else if (restore_color >= 0) {
      *reinterpret_cast<int*>(a + kOffActorClothesColor) = restore_color;
    }
    reinterpret_cast<StringAssignFn>(kStdStringAssign)(
        a + kOffActorPalettePath, relative_path, std::strlen(relative_path));
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return forced;
}

// ── Couleur de CHEVEUX ──────────────────────────────────────────────────────
// Rien à fabriquer ici : une palette de cheveux officielle est un vrai fichier,
// donc il suffit d'en écrire le chemin dans l'acteur. Toute la machinerie de
// blocs ne sert qu'aux couleurs COMPOSÉES du corps, qui n'existent nulle part.
void ApplyHairToActor(void* actor, const char* relative_path) {
  __try {
    char* a = reinterpret_cast<char*>(actor);
    // Même garde que pour le corps : à zéro, le natif ne charge AUCUNE palette
    // externe et garde les couleurs du sprite.
    if (*reinterpret_cast<int*>(a + kOffActorHairColor) == 0)
      *reinterpret_cast<int*>(a + kOffActorHairColor) = 1;
    reinterpret_cast<StringAssignFn>(kStdStringAssign)(
        a + kOffActorHairPalettePath, relative_path,
        std::strlen(relative_path));
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Pose la couleur de cheveux retenue pour ce GID. false = rien à poser, ou
// chemin non résolu.
bool PushHairPalette(uint32_t gid, void* actor) {
  int couleur = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_hair.find(gid);
    if (it == g_hair.end()) return false;
    couleur = it->second;
  }
  if (couleur <= 0 || !actor) return false;
  // Emplacement 1 = la TÊTE. Son chemin porte la RACE, qui décide du dossier de
  // palettes : mesuré le 2026-08-04, 90 % des pixels d'une tête Doram tombent
  // sur des index que la palette humaine laisse noirs.
  char head_spr[300];
  if (!rag::ActorSlotSpritePath(actor, 1, head_spr, sizeof(head_spr)))
    return false;
  char rel[160];
  if (!ro::HairPaletteRelForSprite(head_spr, couleur, rel, sizeof(rel)))
    return false;
  ApplyHairToActor(actor, rel);
  return true;
}

// La couleur de vêtement portée par l'acteur, ou -1 si illisible.
int ReadActorClothesColor(void* actor) {
  __try {
    return *reinterpret_cast<int*>(reinterpret_cast<char*>(actor) +
                                   kOffActorClothesColor);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

void* __fastcall Hooked_FindCachedRes(void* mgr, void* edx, int type,
                                      const char* path) {
  // Sortie à la première ligne pour tout ce qui n'est pas une palette : cette
  // fonction sert VINGT familles de ressources (sprites, textures, cartes,
  // sons...). Une comparaison d'entier, et le natif reprend la main intact.
  if (type == kPaletteTypeIndex) {
    const uint32_t gid = GidFromPath(path);
    if (gid != 0) {
      bool manque = false;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_recipes.find(gid);
        if (it != g_recipes.end() && it->second.block.size() == kPaletteResSize)
          return it->second.block.data();
        manque = true;
      }
      // Recette disparue entre l'écriture du chemin et le rendu : surtout PAS
      // nullptr — les appelants font `add eax, 0x510` sans tester, et 0x510
      // serait déréférencé au blit. On laisse le natif répondre : il ne
      // trouvera pas le fichier, mais c'est SON chemin d'erreur, pas un crash
      // que nous aurions fabriqué.
      //
      // 🔴 Et on le DIT. C'est ici, et nulle part ailleurs, que se voit le
      // corps noir : notre chemin est demandé, personne ne répond, le natif
      // cherche un fichier qui n'existe pas et rend une palette vide. Une
      // palette vide EST un corps noir. Sans cette ligne, le defaut ne laisse
      // aucune trace — l'acteur porte le bon chemin, les détours sont en place,
      // et tout paraît normal (mesuré au débogueur le 2026-08-13).
      if (manque) {
        // Le rendu repasse ici à chaque frame : une ligne par PERTE, remise à
        // zéro dès qu'une recette réapparaît pour ce GID (cf. SetRecipe).
        bool dire = false;
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          if (!g_miss_signale[gid]) { g_miss_signale[gid] = true; dire = true; }
        }
        if (dire)
          LogDiag("[palette] gid={} : chemin demandé mais AUCUNE recette — le "
                  "natif va rendre une palette vide (corps noir)", gid);
      }
    }
  }
  if (!g_orig_find) return nullptr;  // ne peut pas arriver : voir EnsureInstalled
  return g_orig_find(mgr, edx, type, path);
}

// Le natif vient de réécrire le chemin de palette de cheveux : on repose le
// nôtre par-dessus. Rien d'autre — la couleur est le seul choix qui vive ici.
int __fastcall Hooked_RebuildHeadPalette(void* actor, void* edx) {
  int r = 0;
  if (g_orig_rebuild_head) r = g_orig_rebuild_head(actor, edx);
  uint32_t gid = 0;
  if (actor && ReadActorGid(actor, &gid) && gid != 0) PushHairPalette(gid, actor);
  return r;
}

void __fastcall Hooked_RebuildBodyPalettePath(void* actor, void* edx) {
  // Le natif d'abord, TOUJOURS : il porte des effets de bord (résolution de la
  // classe affichée, cas du bébé, madogear) qu'on ne saurait pas refaire, et
  // c'est ce qui garantit qu'un acteur sans recette est stricto sensu intact.
  if (g_orig_rebuild) g_orig_rebuild(actor, edx);

  uint32_t gid = 0;
  if (!actor || !ReadActorGid(actor, &gid) || gid == 0) return;

  // 🔴 Capturer AVANT d'écrire le nôtre. C'est le seul instant où le chemin du
  // serveur est visible : une fois notre chemin posé, il a disparu, et avec lui
  // la couleur de vêtement que le joueur a choisie au styliste.
  char natif[128];
  // (le test écarte NOTRE propre chemin : le mémoriser effacerait le vrai)
  const bool chemin_natif =
      ReadActorPalettePath(actor, natif, sizeof(natif)) &&
      _strnicmp(natif, kMarkerRel, static_cast<int>(kMarkerRelLen)) != 0;
  const int couleur = chemin_natif ? ReadActorClothesColor(actor) : -1;

  // 🔴 La couleur de cheveux est reposée ICI, sur le même détour que le corps.
  // Le client rebâtit les deux chemins ensemble à chaque changement d'apparence,
  // et nous n'avons pas de détour propre à la tête : se greffer sur celui-ci est
  // ce qui fait survivre le choix à un changement de carte ou de tenue.
  PushHairPalette(gid, actor);

  bool a_une_recette = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    NativeLook& n = g_native_paths[gid];
    // L'acteur est rafraîchi DANS TOUS LES CAS : c'est lui qui permettra de
    // reposer un chemin sans attendre que le client repasse par ici.
    n.actor = actor;
    // 🔴 Mais le chemin, LUI, ne se capture qu'une fois la couleur intacte.
    // Après un forçage, ce que le natif écrit vient de nous — le relire ferait
    // dériver la base de ce client, et deux clients ne montreraient plus les
    // mêmes couleurs du même personnage.
    if (chemin_natif && !n.color_forced) {
      n.path = natif;
      n.clothes_color = couleur;
    }
    a_une_recette = g_recipes.find(gid) != g_recipes.end();
  }
  if (!a_une_recette) return;

  char relatif[64];
  MakePath(gid, relatif, sizeof(relatif));
  if (ApplyPathToActor(actor, relatif, /*unlock_color=*/true)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_native_paths[gid].color_forced = true;
  }
}

// ── Retrouver l'acteur d'un GID ──────────────────────────────────────────────
//
// ⚠ On NE remonte PAS la chaîne GameMode -> gestionnaire d'acteurs -> +0x2c,
// pourtant utilisée ailleurs dans le projet. Elle suppose deux choses de trop :
// que l'acteur cherché est celui du joueur, et que son GID vaut l'AID. Le détour
// nous donne les deux gratuitement et sans hypothèse — et il devra de toute
// façon servir aux AUTRES joueurs quand la recette circulera sur le réseau.

// Le GID porté par cet acteur correspond-il ? Sert à revalider un pointeur
// mémorisé : un acteur détruit ne rendra pas ce GID (ou fera lever le SEH).
bool ActorHasGid(void* actor, uint32_t gid) {
  uint32_t g = 0;
  return actor && ReadActorGid(actor, &g) && g == gid;
}

// L'acteur de ce GID, tel que le détour l'a vu. Rend nullptr tant qu'il n'est
// pas passé une fois — au spawn, en pratique.
void* KnownActor(uint32_t gid) {
  void* actor = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_native_paths.find(gid);
    if (it == g_native_paths.end()) return nullptr;
    actor = it->second.actor;
  }
  return ActorHasGid(actor, gid) ? actor : nullptr;
}

bool PrologueMatches(uintptr_t addr, const uint8_t* expected, size_t n) {
  __try {
    return std::memcmp(reinterpret_cast<const void*>(addr), expected, n) == 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

}  // namespace

void EnsureInstalled() {
  static bool done = false;
  if (done) return;
  done = true;

  // Garde de version : si les prologues ne sont pas ceux qu'on a relevés sur ce
  // client, on ne pose RIEN. Un détour posé de travers sur un autre exe ne
  // décolorerait pas un personnage, il corromprait la pile de l'appelant.
  if (!PrologueMatches(kFindCachedRes, kFindCachedResPrologue,
                       sizeof(kFindCachedResPrologue)) ||
      !PrologueMatches(kRebuildBodyPalettePath, kRebuildPrologue,
                       sizeof(kRebuildPrologue)) ||
      !PrologueMatches(kRebuildHeadPalette, kRebuildPrologue,
                       sizeof(kRebuildPrologue))) {
    LogDiag("palette_inject: prologues inattendus, detours NON poses");
    return;
  }

  using namespace hooking;
  g_orig_find = reinterpret_cast<FindCachedResFn>(
      HookManager::Instance().SetHook(
          HookType::kJmpHook, reinterpret_cast<uint8_t*>(kFindCachedRes),
          reinterpret_cast<uint8_t*>(&Hooked_FindCachedRes)));
  g_orig_rebuild = reinterpret_cast<RebuildFn>(
      HookManager::Instance().SetHook(
          HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kRebuildBodyPalettePath),
          reinterpret_cast<uint8_t*>(&Hooked_RebuildBodyPalettePath)));
  g_orig_rebuild_head = reinterpret_cast<RebuildHeadFn>(
      HookManager::Instance().SetHook(
          HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kRebuildHeadPalette),
          reinterpret_cast<uint8_t*>(&Hooked_RebuildHeadPalette)));

  // Un trampoline nul = SetHook a échoué (prologue non relocalisable). Le détour
  // ne doit alors JAMAIS avaler l'appel : sans palette, le corps du joueur ne
  // serait plus dessiné du tout au lieu d'être recoloré.
  LogDebug("palette_inject: find={} rebuild={} tete={}",
          static_cast<const void*>(g_orig_find),
          static_cast<const void*>(g_orig_rebuild),
          static_cast<const void*>(g_orig_rebuild_head));
}

bool SetRecipe(uint32_t gid, const uint8_t* base, const ro::PaletteRamp* ramps,
               int ramp_count, const ro::PaletteRecipe& recipe) {
  if (gid == 0 || !base) return false;

  uint8_t teinte[1024];
  if (!ro::ApplyRecipe(base, 1024, ramps, ramp_count, recipe, teinte,
                       sizeof(teinte)))
    return false;

  std::vector<uint8_t> block;
  BuildBlock(&block, teinte, gid);

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Entry& e = g_recipes[gid];
    // L'ancien bloc part à la poubelle plutôt qu'au ramasse-miettes : le rendu
    // de la frame en cours peut encore tenir son `+0x510`.
    if (!e.block.empty()) Retire(std::move(e.block));
    e.block = std::move(block);
    e.rgba.assign(teinte, teinte + 1024);
    // Cet acteur a de nouveau une recette : une perte future doit être signalée.
    g_miss_signale.erase(gid);
  }

  // 🔴 Poser le chemin TOUT DE SUITE, sans quoi rien ne changerait à l'écran.
  // `CActorSprite_RebuildBodyPalettePath` n'est appelée qu'au spawn et aux
  // changements d'apparence, PAS à chaque frame : attendre qu'elle repasse
  // voudrait dire attendre indéfiniment. L'acteur garderait son chemin natif, et
  // notre palette ne serait jamais demandée.
  void* actor = KnownActor(gid);
  if (actor) {
    char relatif[64];
    MakePath(gid, relatif, sizeof(relatif));
    if (ApplyPathToActor(actor, relatif, /*unlock_color=*/true)) {
      // Forçage mémorisé : à partir d'ici, le chemin que le natif écrira vient
      // de NOUS et ne doit plus jamais être relu comme celui du serveur.
      std::lock_guard<std::mutex> lock(g_mutex);
      NativeLook& n = g_native_paths[gid];
      n.color_forced = true;
      // 🔴 Le forçage ne se déclenche QUE sur une couleur nulle : on connaît donc
      // la valeur d'origine, et il faut la retenir. Sans elle, `ClearRecipe`
      // laisserait l'acteur sur notre 1 avec un chemin vide — le natif
      // chercherait une palette externe inexistante.
      if (n.clothes_color < 0) n.clothes_color = 0;
    }
  } else {
    // Sans acteur connu, la palette est prête mais personne ne la demandera
    // avant que le client ne repasse par le détour — d'où un changement de
    // couleur qui n'arrive « que quand on bouge ». Le dire, plutôt que de
    // laisser chercher.
    LogDiag("palette_inject: gid {} sans acteur connu, la couleur ne "
            "changera qu'au prochain rebuild du client", gid);
  }
  return true;
}

void ClearRecipe(uint32_t gid) {
  NativeLook natif;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_recipes.find(gid);
    if (it == g_recipes.end()) return;
    Retire(std::move(it->second.block));
    g_recipes.erase(it);
    auto np = g_native_paths.find(gid);
    if (np != g_native_paths.end()) natif = np->second;
  }

  // 🔴 RENDRE son chemin d'origine à l'acteur. Le laisser sur
  // `bourgeon\<gid>.pal` alors que le bloc n'existe plus le priverait de
  // palette : notre détour passerait la main au natif, qui chercherait un
  // fichier inexistant. C'est justement pour pouvoir défaire cela qu'on a
  // mémorisé le chemin du serveur.
  void* actor = KnownActor(gid);
  if (actor) {
    // On rend AUSSI la couleur de vêtement d'origine : l'avoir forcée pour
    // débloquer le rendu ne doit pas laisser de trace une fois la recette
    // retirée.
    //
    // 🔴🔴 Le cas SANS chemin natif n'est pas un « rien à faire », c'est le plus
    // dangereux. Un personnage dont la couleur de vêtement vaut 0 n'a JAMAIS de
    // palette externe : le natif ne lui écrit aucun chemin, donc on n'en a
    // mémorisé aucun. Sortir ici laisserait l'acteur sur notre chemin
    // synthétique alors que le bloc vient de disparaître — le natif chercherait
    // `palette\bourgeon\<gid>.pal`, ne le trouverait pas, et le rendu
    // appliquerait une table vide : personnage ENTIÈREMENT NOIR (observé le
    // 2026-08-11, sur l'écran des autres joueurs).
    //
    // Le bon état de repli est le chemin VIDE avec la couleur d'origine : c'est
    // exactement ce que le natif produit tout seul, et il reprend alors la
    // palette interne du `.spr`.
    const int couleur = natif.clothes_color >= 0 ? natif.clothes_color : 0;
    ApplyPathToActor(actor, natif.path.empty() ? "" : natif.path.c_str(),
                     /*unlock_color=*/false, couleur);
  }
  // La couleur d'origine est revenue : le natif redevient la source de vérité,
  // donc on réautorise la capture de son chemin.
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto np = g_native_paths.find(gid);
    if (np != g_native_paths.end()) np->second.color_forced = false;
  }
}

void ForgetActor(uint32_t gid) {
  if (gid == 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_recipes.find(gid);
  if (it != g_recipes.end()) {
    // Même traitement qu'ailleurs : le bloc est mis à la retraite, jamais
    // libéré — le cache d'atlas du client garde son adresse en clé.
    Retire(std::move(it->second.block));
    g_recipes.erase(it);
  }
  // 🔴 L'entrée natif part ENTIÈREMENT, `color_forced` compris. C'est tout
  // l'objet de cette fonction : ce drapeau interdit de recapturer le chemin
  // natif, et le laisser levé ferait refuser au personnage SUIVANT la capture du
  // sien — il hériterait de la base de son prédécesseur, sur le même AID.
  g_native_paths.erase(gid);
  g_hair.erase(gid);
  // ⚠ Aucun acteur n'est touché, et c'est la condition d'emploi : celui qu'on
  // oublie n'existe plus, son pointeur mémorisé ne vaut plus rien.
}

bool NativePalettePath(uint32_t gid, char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_native_paths.find(gid);
  if (it == g_native_paths.end() || it->second.path.empty() ||
      it->second.path.size() >= out_size)
    return false;
  std::memcpy(out, it->second.path.c_str(), it->second.path.size() + 1);
  return true;
}

void SetHairPalette(uint32_t gid, int palette_id) {
  if (gid == 0) return;
  if (palette_id < 1 || palette_id > kHairPaletteMax) {
    ClearHairPalette(gid);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_hair[gid] = palette_id;
  }
  // Tout de suite, sans attendre le prochain rebuild : sinon le changement ne
  // se verrait qu'au prochain changement d'apparence.
  if (void* actor = KnownActor(gid)) PushHairPalette(gid, actor);
}

void ClearHairPalette(uint32_t gid) {
  bool avait = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    avait = g_hair.erase(gid) != 0;
  }
  if (!avait) return;
  // 🔴 RENDRE le chemin natif, et le faire REBÂTIR plutôt que le rejouer de
  // mémoire.
  //
  // Effacer notre entrée ne suffit pas : l'acteur porte encore, dans sa chaîne
  // `+0x470`, le dernier chemin qu'on y a écrit. Rien ne l'en délogerait avant
  // le prochain rafraîchissement d'apparence — d'où une couleur de cheveux qui
  // survivait à sa propre suppression (constaté le 2026-08-12 sur « tout
  // réinitialiser »).
  //
  // On appelle donc le natif, qui RECALCULE le chemin depuis l'état courant de
  // l'acteur (coiffure, classe, sexe). Mémoriser l'ancien chemin aurait été plus
  // simple et FAUX : il périme dès que le joueur change de coiffure ou de
  // classe, et on lui rendrait une palette qui ne correspond plus à sa tête.
  //
  // Le trampoline, pas la fonction détournée : inutile de repasser par notre
  // hook, qui ne trouverait de toute façon plus d'entrée à reposer.
  if (g_orig_rebuild_head) {
    if (void* actor = KnownActor(gid)) g_orig_rebuild_head(actor, nullptr);
  }
  LogDebug("[palette] couleur de cheveux retirée pour gid={}", gid);
}

bool ActorBodySpritePath(uint32_t gid, char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  // 🔴 `KnownActor` revalide le pointeur (GID relu sous SEH) : un acteur détruit
  // depuis le dernier passage du détour rend nullptr au lieu d'être déréférencé.
  void* actor = KnownActor(gid);
  if (!actor) return false;
  // Emplacement 0 = le CORPS, cf. les trois branches de
  // `CActorSprite_BuildPartQuads` (partie 0 = corps, partie 1 = tête).
  return rag::ActorSlotSpritePath(actor, 0, out, out_size);
}

int ReassertPaths() {
  // Les GID d'abord, sous verrou, et le verrou RELÂCHÉ avant de toucher au
  // moindre acteur : `ApplyPathToActor` appelle du code du jeu, et le tenir
  // sous notre mutex nous exposerait à un ordre de prise inverse du détour.
  std::vector<uint32_t> gids;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    gids.reserve(g_recipes.size());
    for (const auto& kv : g_recipes) gids.push_back(kv.first);
  }

  // Ce qu'on a déjà signalé, pour ne pas inonder le journal si un jour quelque
  // chose réécrit le chemin à chaque frame : une ligne par PERTE, pas par
  // passage. Fil de rendu uniquement, donc pas de verrou.
  static std::map<uint32_t, bool> signale;

  int reposes = 0;
  for (uint32_t gid : gids) {
    void* actor = KnownActor(gid);
    if (!actor) continue;  // acteur mort ou jamais vu : rien à réparer
    char porte[128];
    if (!ReadActorPalettePath(actor, porte, sizeof(porte))) continue;
    // Déjà le nôtre : le cas de très loin le plus fréquent, et il ne coûte
    // qu'une comparaison de neuf octets.
    if (_strnicmp(porte, kMarkerRel, static_cast<int>(kMarkerRelLen)) == 0) {
      signale.erase(gid);  // une perte ultérieure sera de nouveau signalée
      continue;
    }

    char relatif[64];
    MakePath(gid, relatif, sizeof(relatif));
    if (ApplyPathToActor(actor, relatif, /*unlock_color=*/true)) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_native_paths[gid].color_forced = true;
    }
    ++reposes;
    if (!signale[gid]) {
      signale[gid] = true;
      LogDiag("[palette] gid={} avait repris le chemin natif ({}) : reposé",
              gid, porte);
    }
  }
  return reposes;
}

bool ActorAlive(uint32_t gid) {
  return gid != 0 && KnownActor(gid) != nullptr;
}

bool HasRecipe(uint32_t gid) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_recipes.find(gid) != g_recipes.end();
}

bool InjectedPalette(uint32_t gid, uint8_t* out, size_t out_size) {
  if (!out || out_size < 1024) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_recipes.find(gid);
  if (it == g_recipes.end() || it->second.rgba.size() < 1024) return false;
  std::memcpy(out, it->second.rgba.data(), 1024);
  return true;
}

int RecipeCount() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return static_cast<int>(g_recipes.size());
}

int KnownGids(uint32_t* out, int max) {
  if (!out || max <= 0) return 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  int n = 0;
  for (const auto& kv : g_native_paths) {
    if (n >= max) break;
    out[n++] = kv.first;
  }
  return n;
}

}  // namespace palette_inject
}  // namespace fx
