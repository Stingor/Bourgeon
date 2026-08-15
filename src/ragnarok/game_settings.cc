#include "ragnarok/game_settings.h"

#include <windows.h>

#include <cstring>

#include "ragnarok/globals.h"
#include "ragnarok/talktype.h"
#include "ragnarok/uiwnd.h"
#include "ui/ro_imgui.h"  // ro::LocalToUtf8

namespace gamesettings {
namespace {

// ── Le singleton et ses primitives (docs/game_option_re.md §3) ───────────────

// `CGameSettingsMgr*`, créé et rempli par `CSession_ctor`. Nul hors du jeu.
constexpr uintptr_t kMgrPtrAddr = 0x0131ee7c;

// Drapeau INTERNE d'une bascule, lu dans l'unordered_map 0x012515FC.
using GetFlag_t = char(__cdecl*)(unsigned int);
constexpr uintptr_t kGetFlagAddr = 0x0068ea70;

// 1 pour les cinq options rangées à l'envers. On INTERROGE le client au lieu de
// recopier sa liste : elle est en dur dans un `switch`, et un client plus récent
// pourrait en ajouter une sixième.
using IsInverted_t = char(__cdecl*)(int);
constexpr uintptr_t kIsInvertedAddr = 0x0068eaf0;

// Écriture BRUTE du drapeau, sans handler ni message. Réservée aux deux cases du
// groupe Audio, qui sont les seules que le natif écrive ainsi.
using SetFlagRaw_t = char(__cdecl*)(unsigned int, char);
constexpr uintptr_t kSetFlagRawAddr = 0x0068fd50;

// `CGameSettingsMgr::SetOption(id, valeur, annonce)` — le chemin complet :
// trouve le handler de l'option et le laisse appliquer l'effet.
using MgrSetOption_t = void(__thiscall*)(void*, unsigned int, char, char);
constexpr uintptr_t kMgrSetOptionAddr = 0x0068dfd0;

// Résout un nom de commande slash en identifiant d'option, puis écrit sans
// exiger que la clé préexiste. Rend 3 quand la commande est inconnue — ce qui
// arrive plus souvent qu'on ne croit, cf. `SetRawFlag`.
using SetFlagByCommand_t = int(__cdecl*)(const char*, char);
constexpr uintptr_t kSetFlagByCommandAddr = 0x0068fc70;
constexpr int kCommandUnknown = 3;

// ── La table des drapeaux, et sa porte d'insertion ──────────────────────────
//
// `GameSettings_SetFlagRaw` ne met à jour que l'existant ; la seule façon de
// CRÉER une clé sans passer par un nom de commande est cette table de hachage,
// que le client interroge lui-même une fois le nom résolu.
//
// `out` reçoit 5 octets : le pointeur sur le nœud, puis 1 si la clé vient d'être
// créée. Le nœud porte la clé en +8 et **la valeur en +12**, sur un octet.
constexpr uintptr_t kFlagMapAddr = 0x012515fc;
using MapGetOrInsert_t = void*(__thiscall*)(void*, void*, const uint32_t*);
constexpr uintptr_t kMapGetOrInsertAddr = 0x0068cdf0;
constexpr int kMapNodeValue = 12;
struct MapInsertResult {
  uintptr_t node = 0;
  uint8_t   created = 0;
};

// `CGameSettingsMgr::ExecOption(id)` — les lignes de type EXE.
using MgrExecOption_t = char(__thiscall*)(void*, int);
constexpr uintptr_t kMgrExecOptionAddr = 0x0068e160;

// `CGameSettingsMgr::ResetAllToDefault()`.
using MgrResetAll_t = void(__thiscall*)(void*);
constexpr uintptr_t kMgrResetAllAddr = 0x0068f8e0;

// Le vecteur de descriptions, et la taille d'un enregistrement.
constexpr int kVecBeginOffset = 0x0c;
constexpr int kVecEndOffset   = 0x10;
constexpr int kRecordSize     = 100;  // 0x64 — vérifié champ par champ

constexpr int kRecIdOffset          = 0x00;
constexpr int kRecTabOffset         = 0x04;
constexpr int kRecTypeOffset        = 0x08;
constexpr int kRecTitleOffset       = 0x0c;
constexpr int kRecTooltipOffset     = 0x24;
constexpr int kRecTipBoxOffset      = 0x3c;
constexpr int kRecDescriptionOffset = 0x40;
constexpr int kRecDefaultOffset     = 0x58;

// ── Son ─────────────────────────────────────────────────────────────────────
constexpr uintptr_t kSoundMgrAddr = 0x01253d0c;
constexpr int kSndEffectVolumeOffset = 0xe0;  // écrit par SetEffectVolume
constexpr int kSndBgmVolumeOffset    = 0xe4;
constexpr int kSndMaster2DOffset     = 0xe8;  // CE curseur-ci, côté fenêtre

using SoundSetVolume_t = void(__thiscall*)(void*, int);
constexpr uintptr_t kSoundSetBgmAddr       = 0x00600be0;
constexpr uintptr_t kSoundSetEffectAddr    = 0x00600ae0;
constexpr uintptr_t kSoundSetMaster2DAddr  = 0x00600a80;
constexpr uintptr_t kSoundSet3DAddr        = 0x00600ab0;

// `CMode::SendMsg`, vtable+0x18 du mode actif — le message 90 (re)démarre ou
// coupe la musique.
using DispCmd_t = void(__thiscall*)(void*, int, int, int, int, int);
constexpr int kVfDispCmd = 0x18;
constexpr int kCmdBgmToggled = 90;

// ── Les trois groupes câblés en dur de la page Basique (docs §3.9) ───────────

// La classe de priorité que le client a posée. `CUIGroupProcessPriority` coche
// ses boutons dessus (0x009F0910) et son reset y remet NORMAL (0x009ED9C0).
constexpr uintptr_t kPriorityClassAddr = 0x0160232c;

// ── Réglages graphiques (docs §3.10) ────────────────────────────────────────

// Le bloc de configuration. Relevé EN DIRECT sur le client, pas seulement lu au
// désassemblage.
constexpr uintptr_t kCfgFullscreenAddr   = 0x01602610;
constexpr uintptr_t kCfgWidthAddr        = 0x01602614;
constexpr uintptr_t kCfgHeightAddr       = 0x01602618;
constexpr uintptr_t kCfgBppAddr          = 0x0160261c;
constexpr uintptr_t kCfgSpriteDetailAddr = 0x01602630;
constexpr uintptr_t kCfgTextureDetailAddr= 0x01602634;
constexpr uintptr_t kCfgTrilinearAddr    = 0x01602638;
constexpr uintptr_t kCfgRenderSystemAddr = 0x01602640;
constexpr uintptr_t kCfgDx9AdapterGuid   = 0x01602644;  // 16 o.
constexpr uintptr_t kCfgDx9DeviceName    = 0x01602654;  // 32 o.
constexpr uintptr_t kCfgDx7DeviceGuid    = 0x016025c8;  // 16 o.
constexpr uintptr_t kCfgDx7DriverGuid    = 0x016025d8;  // 16 o.
constexpr uintptr_t kCfgDx7AdapterName   = 0x016025e8;  // 40 o.

// Deux compagnons du niveau de détail des sprites, écrits par le natif en même
// temps que lui. Leur rôle exact n'est pas établi ; ils sont reproduits parce
// que le natif les écrit, pas parce qu'on saurait s'en passer.
constexpr uintptr_t kSpriteDetailFlagA = 0x01602b60;
constexpr uintptr_t kSpriteDetailFlagB = 0x01602b64;

// Diviseur de textures dérivé du niveau : 0 -> 4, 1 -> 2, 2 -> 1.
constexpr uintptr_t kTextureDownscaleAddr = 0x0122b3d8;

// ⛔ `g_RestartRequested` (0x01602a8c) — DÉLIBÉRÉMENT INUTILISÉ, gardé pour la
// mémoire. Le nom ment : à la sortie de sa boucle, `WinMainCRTStartup_Run`
// (0x00dba10d) ne ré-exécute rien. Il fait UN SEUL geste,
// `ShellExecuteA("OPEN", MsgString(0xD75), …)` — et 0xD75 est
// `MSI_WEB_ADDRESS_FOR_RESTART` : le client ouvre la PAGE WEB du lanceur
// d'origine dans le navigateur, puis s'arrête, en laissant l'humain relancer.
// (Si la ligne de commande contient « Dev », c'est le remplaçant « http:// ».)
// Le poser depuis notre panneau faisait donc surgir un navigateur au lieu d'un
// client — constaté en jeu le 2026-08-15. On ne le pose plus.
//
// Sans conséquence pour la configuration : `OptionInfo_SaveToFile` passe AVANT
// ce test, à chaque arrêt propre. Ce que nous écrivons est sauvé de toute façon.

// La fabrique de sprites et son cache — à notifier quand le filtrage ou la
// finesse des textures changent, sinon les textures déjà chargées gardent
// l'ancien réglage.
using SpriteTexFactoryGet_t = void*(__cdecl*)();
constexpr uintptr_t kSpriteTexFactoryGetAddr = 0x00554070;
using SpriteTexFactoryApply_t = void(__thiscall*)(void*);
constexpr uintptr_t kSpriteTexFactoryApplyAddr = 0x00560f80;

// 🔴 __THISCALL, PAS __CDECL. Le natif fait `mov ecx, offset g_SpriteTexFactoryCache`
// puis `jmp sub_568B30` : le cache passe par ECX. Déclarée `__cdecl`, la fonction
// lisait le contenu d'un ECX de passage — et sortait AUSSITÔT par son test de
// liste vide, sans rien recharger et sans se plaindre. Symptôme exact vécu en
// jeu : les réglages « effet immédiat » n'agissaient qu'au redémarrage suivant,
// puisque les globales, elles, étaient bien écrites.
using CacheFlush_t = void(__thiscall*)(void*);
constexpr uintptr_t kSpriteTexCacheFlushAddr = 0x00568b30;
constexpr uintptr_t kSpriteTexCacheAddr      = 0x0125161c;

// Un `std::vector` du client : trois pointeurs, rien de plus.
struct ClientVector {
  uint8_t* begin = nullptr;
  uint8_t* end   = nullptr;
  uint8_t* cap   = nullptr;
};

using EnumAdapters_t = void(__cdecl*)(ClientVector*, int);
constexpr uintptr_t kEnumAdaptersAddr = 0x00560fb0;
constexpr int kAdapterRecordSize = 104;
constexpr int kAdapterRecIndex   = 0x04;
constexpr int kAdapterRecDesc    = 0x08;  // std::string
constexpr int kAdapterRecDx7Drv  = 0x20;  // 16 o.
constexpr int kAdapterRecDx7Dev  = 0x30;  // 16 o.
constexpr int kAdapterRecDx9Guid = 0x40;  // 16 o.
constexpr int kAdapterRecDx9Name = 0x50;  // std::string

using EnumModes_t = void(__cdecl*)(ClientVector*, int, int);
constexpr uintptr_t kEnumModesAddr = 0x00561550;
constexpr int kModeRecordSize = 36;
constexpr int kModeRecLabel   = 0x0c;  // std::string

// Rebâtit l'enregistrement de l'adaptateur COURANT depuis la configuration.
// 🔴 Ne remplit que ce que la comparaison ci-dessous consulte : le champ
// `kAdapterRecIndex` reste à ZÉRO. Voir `CurrentAdapterIndex`.
using GetCurrentAdapter_t = void*(__cdecl*)(void*);
constexpr uintptr_t kGetCurrentAdapterAddr = 0x005610b0;

// Le comparateur du client lui-même : `this` = un enregistrement énuméré, le
// paramètre = celui de la configuration. L'index n'entre PAS dans la
// comparaison — en DirectX 9, ce sont le GUID (+0x40) ET le nom de sortie
// (+0x50) qui décident, et il faut bien les deux : deux écrans branchés sur la
// même carte partagent le GUID, seul `\\.\DISPLAYn` les sépare.
using AdapterEquals_t = bool(__thiscall*)(const void*, const void*);
constexpr uintptr_t kAdapterEqualsAddr = 0x00560d60;

// La sauvegarde du fichier d'options, `this` = la session. Le client ne l'appelle
// qu'à l'arrêt ; on la déclenche nous-mêmes pour qu'un réglage structurel
// survive à une fin de partie brutale.
using OptionSave_t = void(__thiscall*)(void*);
constexpr uintptr_t kOptionSaveAddr    = 0x00d78970;
constexpr uintptr_t kOptionContextAddr = 0x015fa3c0;  // la session

// La déconnexion propre puis l'arrêt du mode courant — les deux gestes du
// [Apply] natif qui, eux, marchent parfaitement. Sans le drapeau de « relance »
// qui les accompagnait, l'arrêt est un arrêt : plus de page web.
using ConnGetInstance_t = void*(__cdecl*)();
constexpr uintptr_t kConnGetInstanceAddr = 0x00c14d60;
using ConnDisconnect_t = void(__thiscall*)(void*);
constexpr uintptr_t kConnDisconnectAddr = 0x00c14320;
constexpr int kCmdShutdown = 2;

// ⚠ Le mode que le natif interroge dans son [Apply] est ce GLOBAL, pas celui que
// rend le gestionnaire de modes. Les deux peuvent différer, et on suit le natif.
constexpr uintptr_t kCurrentModePtrAddr = 0x0121333c;

// `this` en ecx ; le client appelle indifféremment en __thiscall ou __fastcall.
using StrDtor_t = void(__thiscall*)(void*);
using OperatorDelete_t = void(__cdecl*)(void*);

// Gestionnaire de skins. `this[7]` = index courant, `this[11]/this[12]` = le
// vecteur de noms (des `std::string`, 24 octets pièce).
constexpr uintptr_t kSkinMgrAddr       = 0x011fe3a8;
constexpr uintptr_t kSkinCurrentAddr   = 0x011fe3c4;  // mgr+0x1C
constexpr uintptr_t kSkinVecBeginAddr  = 0x011fe3d4;  // mgr+0x2C
constexpr uintptr_t kSkinVecEndAddr    = 0x011fe3d8;  // mgr+0x30
constexpr int       kSkinRecordSize    = 24;          // sizeof(std::string)

// `GetSkinName(index)` — rend un `const char*` du client, déjà résolu (court ou
// alloué). `-1` donne le nom de l'entrée par défaut.
using SkinGetName_t = const char*(__thiscall*)(void*, int);
constexpr uintptr_t kSkinGetNameAddr = 0x007a6f10;

// `SetSkin(index)` — retient l'index PUIS purge toutes les textures .bmp du
// gestionnaire, pour qu'elles se rechargent depuis le nouveau dossier.
using SkinSet_t = void(__thiscall*)(void*, int);
constexpr uintptr_t kSkinSetAddr = 0x007a7f70;

void* Mgr() {
  __try {
    return *reinterpret_cast<void**>(kMgrPtrAddr);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void* SoundMgr() {
  __try {
    return *reinterpret_cast<void**>(kSoundMgrAddr);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void* ActiveGameMode() {
  __try {
    using GetActive_t = void*(__thiscall*)(void*);
    return reinterpret_cast<GetActive_t>(rag::kModeMgrGetActiveAddr)(
        reinterpret_cast<void*>(rag::kModeMgrAddr));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Un `std::string` de MSVC, recopié en UTF-8 dans `out`.
//
// Représentation : seize octets qui portent SOIT le texte court, SOIT un
// pointeur, puis la taille et la capacité. C'est la capacité qui tranche —
// au-delà de 15, le texte est ailleurs.
void CopyClientString(const uint8_t* field, char* out, size_t out_size) {
  out[0] = '\0';
  __try {
    const uint32_t capacity = *reinterpret_cast<const uint32_t*>(field + 0x14);
    const uint32_t size     = *reinterpret_cast<const uint32_t*>(field + 0x10);
    if (size == 0 || size > 0x4000) return;  // vide, ou champ manifestement faux
    const char* data = (capacity >= 16)
                           ? *reinterpret_cast<const char* const*>(field)
                           : reinterpret_cast<const char*>(field);
    if (!data) return;
    // ⚠ La conversion se fait dans la code-page du CLIENT, pas en CP949 fixe :
    // ces textes viennent de son Lua, donc de son encodage (même règle que la
    // MsgStringTable, cf. ragnarok/msgstring.cc).
    const char* utf8 = ro::LocalToUtf8(data);
    if (!utf8) return;
    std::strncpy(out, utf8, out_size - 1);
    out[out_size - 1] = '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

// La même chaîne, mais SANS conversion : telle que le client l'a écrite.
//
// 🔴 À utiliser quand la destination est un tampon DU CLIENT. Repasser par l'UTF-8
// y écrirait un texte que lui ne sait pas relire — et il s'agit ici du nom
// d'adaptateur qu'il comparera au démarrage.
void CopyClientStringRaw(const uint8_t* field, char* out, size_t out_size) {
  out[0] = '\0';
  __try {
    const uint32_t capacity = *reinterpret_cast<const uint32_t*>(field + 0x14);
    const uint32_t size     = *reinterpret_cast<const uint32_t*>(field + 0x10);
    if (size == 0 || size > 0x4000) return;
    const char* data = (capacity >= 16)
                           ? *reinterpret_cast<const char* const*>(field)
                           : reinterpret_cast<const char*>(field);
    if (!data) return;
    const size_t take = (size < out_size - 1) ? size : out_size - 1;
    std::memcpy(out, data, take);
    out[take] = '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

// Adresse de l'enregistrement `index`, ou nul.
const uint8_t* RecordAt(int index) {
  __try {
    void* mgr = Mgr();
    if (!mgr || index < 0) return nullptr;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(mgr);
    const uint8_t* begin = *reinterpret_cast<const uint8_t* const*>(base + kVecBeginOffset);
    const uint8_t* end   = *reinterpret_cast<const uint8_t* const*>(base + kVecEndOffset);
    if (!begin || !end || end < begin) return nullptr;
    const int count = static_cast<int>((end - begin) / kRecordSize);
    if (index >= count) return nullptr;
    return begin + static_cast<ptrdiff_t>(index) * kRecordSize;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool IsInverted(int id) {
  __try {
    return reinterpret_cast<IsInverted_t>(kIsInvertedAddr)(id) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool RawFlag(int id) {
  __try {
    return reinterpret_cast<GetFlag_t>(kGetFlagAddr)(
               static_cast<unsigned int>(id)) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void SetRawFlag(int id, bool value) {
  __try {
    reinterpret_cast<SetFlagRaw_t>(kSetFlagRawAddr)(static_cast<unsigned int>(id),
                                                    value ? 1 : 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}

  // 🔴 VÉRIFIER, PARCE QUE L'ÉCHEC EST MUET. `GameSettings_SetFlagRaw` ne MET À
  // JOUR que des clés existantes : sur une option absente de la table des
  // drapeaux, elle sort sans rien écrire et sans se plaindre. C'est le cas de la
  // bordure d'emblème (0xF3) — inerte jusque dans la fenêtre native du client.
  //
  // Le chemin par nom de commande (`GameSettings_SetFlagByCommandName`) sait
  // insérer, mais il exige un nom résoluble ; or « /frame » n'existe NULLE PART
  // dans l'image, ni dans le `CmdOnOffList` du Lua. Il rendait donc 3, et la
  // case restait morte.
  //
  // On insère donc nous-mêmes, par la porte que le client emprunte lui-même une
  // fois le nom résolu : sa table de hachage. Relire d'abord évite d'y toucher
  // quand l'écriture ordinaire a suffi — le cas de toutes les autres options.
  if (RawFlag(id) == value) return;
  __try {
    const uint32_t key = static_cast<uint32_t>(id);
    MapInsertResult out = {};
    reinterpret_cast<MapGetOrInsert_t>(kMapGetOrInsertAddr)(
        reinterpret_cast<void*>(kFlagMapAddr), &out, &key);
    if (out.node) *reinterpret_cast<uint8_t*>(out.node + kMapNodeValue) = value ? 1 : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int ClampVolume(int volume) {
  if (volume < 0) return 0;
  if (volume > kVolumeMax) return kVolumeMax;
  return volume;
}

int SoundField(int offset) {
  __try {
    void* mgr = SoundMgr();
    if (!mgr) return 0;
    return *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(mgr) + offset);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

}  // namespace

bool Available() { return Mgr() != nullptr && Count() > 0; }

int Count() {
  __try {
    void* mgr = Mgr();
    if (!mgr) return 0;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(mgr);
    const uint8_t* begin = *reinterpret_cast<const uint8_t* const*>(base + kVecBeginOffset);
    const uint8_t* end   = *reinterpret_cast<const uint8_t* const*>(base + kVecEndOffset);
    if (!begin || !end || end < begin) return 0;
    return static_cast<int>((end - begin) / kRecordSize);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool At(int index, Option* out) {
  if (!out) return false;
  const uint8_t* rec = RecordAt(index);
  if (!rec) return false;

  __try {
    out->id        = *reinterpret_cast<const int*>(rec + kRecIdOffset);
    out->tab       = *reinterpret_cast<const int*>(rec + kRecTabOffset);
    out->type      = *reinterpret_cast<const int*>(rec + kRecTypeOffset);
    out->tipbox_id = *reinterpret_cast<const int*>(rec + kRecTipBoxOffset);
    // Le défaut est rangé SOUS SA FORME INTERNE : on le rend à l'endroit, comme
    // la valeur courante, sans quoi les cinq options inversées afficheraient un
    // défaut contraire à leur libellé.
    const bool raw_default = rec[kRecDefaultOffset] != 0;
    out->default_on = raw_default ^ IsInverted(out->id);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

  CopyClientString(rec + kRecTitleOffset, out->title, sizeof(out->title));
  CopyClientString(rec + kRecTooltipOffset, out->tooltip, sizeof(out->tooltip));
  CopyClientString(rec + kRecDescriptionOffset, out->description,
                   sizeof(out->description));
  return true;
}

bool IsOn(int id) { return RawFlag(id) ^ IsInverted(id); }

bool InTable(int id) {
  const int count = Count();
  for (int i = 0; i < count; ++i) {
    const uint8_t* rec = RecordAt(i);
    if (!rec) continue;
    __try {
      if (*reinterpret_cast<const int*>(rec + kRecIdOffset) == id) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  }
  return false;
}

void SetOn(int id, bool on) {
  void* mgr = Mgr();
  if (!mgr) return;
  const char internal = static_cast<char>((on ^ IsInverted(id)) ? 1 : 0);

  // 🔴 DEUX CHEMINS, ET LE MAUVAIS NE DIT RIEN. `CGameSettingsMgr::SetOption`
  // commence par CHERCHER l'id dans le vecteur `OptionTbl` ; s'il ne l'y trouve
  // pas, elle sort **sans rien écrire et sans se plaindre**. Or toutes les
  // options du client n'y sont pas : la page Basique en câble plusieurs en dur
  // (bordure d'emblème 0xF3, notification de connexion 0xA5), et celles-là
  // n'existent que dans la table des drapeaux.
  //
  // Bug vécu en jeu le 2026-08-14 : les deux cases de l'onglet Basique restaient
  // figées. La LECTURE marchait — elle passe par la table des drapeaux, qui les
  // contient — donc la case affichait fidèlement une valeur que l'écriture ne
  // changeait jamais. Un test « ça lit bien » n'aurait rien vu.
  //
  // On aiguille donc sur la présence RÉELLE dans la table, plutôt que d'entretenir
  // une liste d'exceptions qui se périmerait au premier `GameSettings.lub` modifié
  // par le serveur.
  if (!InTable(id)) {
    // Écriture directe, exactement ce que font les groupes de la page Basique
    // (`0x009EEF50`, `0x009EF000`). Pas de handler à déclencher : ces options
    // sont relues par leur consommateur, pas appliquées à l'écriture.
    SetRawFlag(id, on ^ IsInverted(id));
    return;
  }

  __try {
    // annonce = 0 : la case à cocher native n'écrit rien au chat (c'est la
    // commande slash qui le fait). Cf. GameSettingsUI_OptionItem_OnCheck.
    reinterpret_cast<MgrSetOption_t>(kMgrSetOptionAddr)(
        mgr, static_cast<unsigned int>(id), internal, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool SetOnByCommand(const char* slash_command, bool on) {
  if (!slash_command || !*slash_command) return false;
  // ⚠ Pas de XOR d'inversion ici, et ce n'est pas un oubli : on ne connaît l'id
  // qu'APRÈS l'appel, trop tard pour corriger la valeur. Le cas ne se pose pas —
  // les cinq options rangées à l'envers sont toutes dans `OptionTbl`, donc
  // accessibles par `SetOn`, alors que ce chemin-ci ne sert qu'aux options qui
  // n'y sont pas.
  __try {
    const int id = reinterpret_cast<SetFlagByCommand_t>(kSetFlagByCommandAddr)(
        slash_command, on ? 1 : 0);
    return id != kCommandUnknown;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void Exec(int id) {
  void* mgr = Mgr();
  if (!mgr) return;
  __try {
    reinterpret_cast<MgrExecOption_t>(kMgrExecOptionAddr)(mgr, id);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ResetAllToDefault() {
  void* mgr = Mgr();
  if (!mgr) return;
  __try {
    reinterpret_cast<MgrResetAll_t>(kMgrResetAllAddr)(mgr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Son ─────────────────────────────────────────────────────────────────────

int BgmVolume() { return SoundField(kSndBgmVolumeOffset); }

void SetBgmVolume(int volume) {
  void* mgr = SoundMgr();
  if (!mgr) return;
  const int value = ClampVolume(volume);
  __try {
    reinterpret_cast<SoundSetVolume_t>(kSoundSetBgmAddr)(mgr, value);
    // ⚠ Le natif aligne ENSUITE `+0xE0` sur le volume BGM qu'il vient de poser.
    // Le geste surprend — c'est le champ nommé « effect volume » — mais il est
    // reproduit tel quel : s'en écarter ferait diverger notre curseur du sien
    // sur un champ dont le rôle exact n'est pas établi.
    reinterpret_cast<SoundSetVolume_t>(kSoundSetEffectAddr)(
        mgr, SoundField(kSndBgmVolumeOffset));
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int EffectVolume() { return SoundField(kSndMaster2DOffset); }

void SetEffectVolume(int volume) {
  void* mgr = SoundMgr();
  if (!mgr) return;
  const int value = ClampVolume(volume);
  __try {
    reinterpret_cast<SoundSetVolume_t>(kSoundSetMaster2DAddr)(mgr, value);
    reinterpret_cast<SoundSetVolume_t>(kSoundSet3DAddr)(mgr, value);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool BgmEnabled() { return RawFlag(TT_MUSIC_ON_OFF); }

void SetBgmEnabled(bool on) {
  const bool was_on = RawFlag(TT_MUSIC_ON_OFF);
  SetRawFlag(TT_MUSIC_ON_OFF, on);
  if (was_on == on) return;
  // 🔴 Le devoir caché : sans ce message, couper la musique laisse le morceau en
  // cours jouer jusqu'au bout, et la rallumer ne relance rien.
  __try {
    void* mode = ActiveGameMode();
    if (mode) uiwnd::Vf<DispCmd_t>(mode, kVfDispCmd)(mode, kCmdBgmToggled, 0, 0, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool EffectSoundEnabled() { return RawFlag(TT_EFFECT_SOUND_ON_OFF); }

void SetEffectSoundEnabled(bool on) { SetRawFlag(TT_EFFECT_SOUND_ON_OFF, on); }

// ── Priorité du processus ───────────────────────────────────────────────────

int ProcessPriority() {
  __try {
    return static_cast<int>(*reinterpret_cast<const uint32_t*>(kPriorityClassAddr));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return kPriorityNormal; }
}

void SetProcessPriority(int priority_class) {
  if (priority_class != kPriorityHigh && priority_class != kPriorityNormal &&
      priority_class != kPriorityLow)
    return;
  // ⚠ On refait le geste du client au lieu d'appeler sa fonction : la sienne
  // (0x009EF070) est une méthode du groupe natif et choisit la valeur en
  // COMPARANT le bouton cliqué à trois pointeurs de widgets — sans la fenêtre,
  // elle n'a rien à comparer. Les deux lignes qu'elle exécute ensuite sont
  // celles-ci, dans cet ordre.
  ::SetPriorityClass(::GetCurrentProcess(), static_cast<DWORD>(priority_class));
  __try {
    *reinterpret_cast<uint32_t*>(kPriorityClassAddr) =
        static_cast<uint32_t>(priority_class);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Skin ────────────────────────────────────────────────────────────────────

int SkinCount() {
  __try {
    const uint8_t* begin = *reinterpret_cast<const uint8_t* const*>(kSkinVecBeginAddr);
    const uint8_t* end   = *reinterpret_cast<const uint8_t* const*>(kSkinVecEndAddr);
    if (!begin || !end || end < begin) return 0;
    return static_cast<int>((end - begin) / kSkinRecordSize);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool SkinName(int index, char* out, int out_size) {
  if (!out || out_size <= 0) return false;
  out[0] = '\0';
  if (index != kSkinDefault && (index < 0 || index >= SkinCount())) return false;
  __try {
    const char* name = reinterpret_cast<SkinGetName_t>(kSkinGetNameAddr)(
        reinterpret_cast<void*>(kSkinMgrAddr), index);
    if (!name || !*name) return false;
    const char* utf8 = ro::LocalToUtf8(name);
    if (!utf8) return false;
    std::strncpy(out, utf8, static_cast<size_t>(out_size) - 1);
    out[out_size - 1] = '\0';
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; return false; }
}

int CurrentSkin() {
  __try {
    return static_cast<int>(*reinterpret_cast<const int32_t*>(kSkinCurrentAddr));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return kSkinDefault; }
}

void SetSkin(int index) {
  if (index != kSkinDefault && (index < 0 || index >= SkinCount())) return;
  if (index == CurrentSkin()) return;  // le natif teste aussi : sinon purge à vide
  __try {
    reinterpret_cast<SkinSet_t>(kSkinSetAddr)(
        reinterpret_cast<void*>(kSkinMgrAddr), index);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Réglages graphiques ─────────────────────────────────────────────────────

namespace graphics {
namespace {

int ReadInt(uintptr_t address, int fallback) {
  __try {
    return *reinterpret_cast<const int32_t*>(address);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
}

void WriteInt(uintptr_t address, int value) {
  __try {
    *reinterpret_cast<int32_t*>(address) = value;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int ClampDetail(int level) {
  if (level < 0) return 0;
  if (level > kDetailMax) return kDetailMax;
  return level;
}

// Le cache de la fabrique de sprites, vidé quand la finesse ou le filtrage
// changent. Sans lui, les textures déjà chargées gardent l'ancien réglage.
void FlushSpriteTextures() {
  __try {
    reinterpret_cast<CacheFlush_t>(kSpriteTexCacheFlushAddr)(
        reinterpret_cast<void*>(kSpriteTexCacheAddr));
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Libère un vecteur rendu par un énumérateur du client, exactement comme il le
// fait lui-même.
//
// 🔴 Deux devoirs, et en oublier un ne se voit pas tout de suite. D'abord chaque
// `std::string` des enregistrements est détruite une par une — sinon chaque
// ouverture du panneau fuit un texte par adaptateur et par mode. Ensuite, un bloc
// de 4096 octets ou plus est SUR-ALLOUÉ par MSVC et son vrai pointeur est rangé
// juste avant : libérer l'adresse apparente corromprait le tas. Le natif fait ce
// test, et 4096 octets ne font que 114 modes d'affichage — c'est atteignable.
void FreeVector(ClientVector* vec, int record_size, const int* string_offsets,
                int string_count) {
  if (!vec || !vec->begin) return;
  __try {
    for (uint8_t* rec = vec->begin; rec != vec->end; rec += record_size) {
      for (int i = 0; i < string_count; ++i) {
        reinterpret_cast<StrDtor_t>(rag::kStdStringDtorAddr)(rec + string_offsets[i]);
      }
    }
    void* block = vec->begin;
    const size_t bytes = static_cast<size_t>(vec->end - vec->begin);
    if (bytes >= 0x1000) {
      block = *(reinterpret_cast<void* const*>(vec->begin) - 1);
    }
    reinterpret_cast<OperatorDelete_t>(rag::kGameOperatorDeleteAddr)(block);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  vec->begin = vec->end = vec->cap = nullptr;
}

}  // namespace

// ── ⚡ À chaud ──────────────────────────────────────────────────────────────

int SpriteDetail() { return ClampDetail(ReadInt(kCfgSpriteDetailAddr, kDetailMax)); }

void SetSpriteDetail(int level) {
  const int value = ClampDetail(level);
  if (value == SpriteDetail()) return;
  WriteInt(kCfgSpriteDetailAddr, value);
  // Les deux compagnons, écrits comme le natif les écrit : (1,1) au niveau 0,
  // (0,1) au niveau 1, (0,0) au niveau 2.
  WriteInt(kSpriteDetailFlagA, value == 0 ? 1 : 0);
  WriteInt(kSpriteDetailFlagB, value <= 1 ? 1 : 0);
}

int TextureDetail() { return ClampDetail(ReadInt(kCfgTextureDetailAddr, kDetailMax)); }

void SetTextureDetail(int level) {
  const int value = ClampDetail(level);
  if (value == TextureDetail()) return;
  WriteInt(kCfgTextureDetailAddr, value);
  // 0 -> 4, 1 -> 2, 2 -> 1 : le diviseur appliqué aux textures.
  const int factor = (value == 0) ? 4 : (value == 1) ? 2 : 1;
  if (factor == ReadInt(kTextureDownscaleAddr, factor)) return;
  WriteInt(kTextureDownscaleAddr, factor);
  FlushSpriteTextures();
}

bool Trilinear() { return ReadInt(kCfgTrilinearAddr, 0) != 0; }

void SetTrilinear(bool on) {
  if (on == Trilinear()) return;
  WriteInt(kCfgTrilinearAddr, on ? 1 : 0);
  __try {
    void* factory = reinterpret_cast<SpriteTexFactoryGet_t>(kSpriteTexFactoryGetAddr)();
    if (factory)
      reinterpret_cast<SpriteTexFactoryApply_t>(kSpriteTexFactoryApplyAddr)(factory);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  FlushSpriteTextures();
}

// ── 🔁 Structurels ─────────────────────────────────────────────────────────

int  System()       { return ReadInt(kCfgRenderSystemAddr, kRenderDx9); }
bool Fullscreen()   { return ReadInt(kCfgFullscreenAddr, 0) != 0; }
int  Width()        { return ReadInt(kCfgWidthAddr, 0); }
int  Height()       { return ReadInt(kCfgHeightAddr, 0); }
int  BitsPerPixel() { return ReadInt(kCfgBppAddr, 32); }

bool EnumerateAdapters(int system, Adapter* out, int max_count, int* out_count) {
  if (out_count) *out_count = 0;
  if (!out || max_count <= 0) return false;
  if (system != kRenderDx7 && system != kRenderDx9) return false;

  ClientVector vec;
  __try {
    reinterpret_cast<EnumAdapters_t>(kEnumAdaptersAddr)(&vec, system);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

  int count = 0;
  if (vec.begin && vec.end > vec.begin) {
    const int total = static_cast<int>((vec.end - vec.begin) / kAdapterRecordSize);
    for (int i = 0; i < total && count < max_count; ++i) {
      const uint8_t* rec = vec.begin + static_cast<ptrdiff_t>(i) * kAdapterRecordSize;
      Adapter& adapter = out[count];
      adapter.index = *reinterpret_cast<const int32_t*>(rec + kAdapterRecIndex);
      CopyClientString(rec + kAdapterRecDesc, adapter.name, sizeof(adapter.name));
      // La sortie d'affichage : sans elle, deux écrans branchés sur la même carte
      // donnent deux lignes rigoureusement identiques. Vide en DirectX 7, dont
      // l'énumération ne remplit pas ce champ.
      CopyClientString(rec + kAdapterRecDx9Name, adapter.device, sizeof(adapter.device));
      ++count;
    }
  }
  const int kStrings[] = {kAdapterRecDesc, kAdapterRecDx9Name};
  FreeVector(&vec, kAdapterRecordSize, kStrings, 2);
  if (out_count) *out_count = count;
  return true;
}

bool EnumerateModes(int system, int adapter_index, Mode* out, int max_count,
                    int* out_count) {
  if (out_count) *out_count = 0;
  if (!out || max_count <= 0) return false;
  if (system != kRenderDx7 && system != kRenderDx9) return false;

  ClientVector vec;
  __try {
    reinterpret_cast<EnumModes_t>(kEnumModesAddr)(&vec, system, adapter_index);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

  int count = 0;
  if (vec.begin && vec.end > vec.begin) {
    const int total = static_cast<int>((vec.end - vec.begin) / kModeRecordSize);
    for (int i = 0; i < total && count < max_count; ++i) {
      const uint8_t* rec = vec.begin + static_cast<ptrdiff_t>(i) * kModeRecordSize;
      Mode& mode = out[count];
      mode.width  = *reinterpret_cast<const int32_t*>(rec + 0);
      mode.height = *reinterpret_cast<const int32_t*>(rec + 4);
      mode.bpp    = *reinterpret_cast<const int32_t*>(rec + 8);
      CopyClientString(rec + kModeRecLabel, mode.label, sizeof(mode.label));
      ++count;
    }
  }
  const int kStrings[] = {kModeRecLabel};
  FreeVector(&vec, kModeRecordSize, kStrings, 1);
  if (out_count) *out_count = count;
  return true;
}

int CurrentAdapterIndex() {
  // L'enregistrement rebâti par le client depuis sa configuration : 104 octets,
  // dont deux `std::string` qu'il construit — donc à détruire.
  //
  // 🔴 SON CHAMP `index` NE VEUT RIEN DIRE : le client le met à zéro et ne le
  // remplit jamais. Le lire, c'est répondre « le premier de la liste » à toutes
  // les questions — et c'est ce que faisait ce code, d'où un panneau qui
  // reproposait éternellement le même écran quel que soit le choix enregistré.
  //
  // On refait donc ce que fait `D3D9_ResolveAdapterOrdinalAndCreateDevice`
  // (0x00565230) juste avant de créer son device : énumérer, comparer avec le
  // comparateur du client, et rendre l'index de celui qui correspond. Passer par
  // sa comparaison plutôt que la nôtre garantit que nous montrons exactement
  // l'adaptateur qu'il choisira.
  const int system = System();
  if (system != kRenderDx7 && system != kRenderDx9) return -1;

  alignas(4) uint8_t current[kAdapterRecordSize] = {0};
  bool current_built = false;
  __try {
    reinterpret_cast<GetCurrentAdapter_t>(kGetCurrentAdapterAddr)(current);
    current_built = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }

  ClientVector vec;
  __try {
    reinterpret_cast<EnumAdapters_t>(kEnumAdaptersAddr)(&vec, system);
  } __except (EXCEPTION_EXECUTE_HANDLER) { vec = ClientVector(); }

  int index = -1;
  if (vec.begin && vec.end > vec.begin) {
    const int total = static_cast<int>((vec.end - vec.begin) / kAdapterRecordSize);
    __try {
      for (int i = 0; i < total; ++i) {
        const uint8_t* rec = vec.begin + static_cast<ptrdiff_t>(i) * kAdapterRecordSize;
        if (!reinterpret_cast<AdapterEquals_t>(kAdapterEqualsAddr)(rec, current))
          continue;
        index = *reinterpret_cast<const int32_t*>(rec + kAdapterRecIndex);
        break;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { index = -1; }
  }
  const int kStrings[] = {kAdapterRecDesc, kAdapterRecDx9Name};
  FreeVector(&vec, kAdapterRecordSize, kStrings, 2);

  if (current_built) {
    __try {
      reinterpret_cast<StrDtor_t>(rag::kStdStringDtorAddr)(current + kAdapterRecDesc);
      reinterpret_cast<StrDtor_t>(rag::kStdStringDtorAddr)(current + kAdapterRecDx9Name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
  }

  // Aucune correspondance : le client fera de même et retombera sur l'adaptateur
  // 0. Autant l'annoncer plutôt que de rendre -1, qui ferait choisir au panneau
  // une ligne au hasard.
  return (index >= 0) ? index : 0;
}

bool AdapterChoiceMovesWindow() { return Fullscreen(); }

bool ApplyStructural(int system, int adapter_index, int width, int height,
                     int bpp, bool fullscreen) {
  if (system != kRenderDx7 && system != kRenderDx9) return false;
  if (width <= 0 || height <= 0 || bpp <= 0) return false;

  // 🔴 Les GUID de l'adaptateur ne se devinent pas : ils ne vivent que dans
  // l'énumération. On la refait donc ICI, et on écrit la configuration TANT QUE
  // l'enregistrement est vivant — recopier ses chaînes pour s'en servir plus tard
  // laisserait des pointeurs morts.
  ClientVector vec;
  __try {
    reinterpret_cast<EnumAdapters_t>(kEnumAdaptersAddr)(&vec, system);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

  bool written = false;
  if (vec.begin && vec.end > vec.begin) {
    const int total = static_cast<int>((vec.end - vec.begin) / kAdapterRecordSize);
    __try {
      for (int i = 0; i < total; ++i) {
        const uint8_t* rec = vec.begin + static_cast<ptrdiff_t>(i) * kAdapterRecordSize;
        if (*reinterpret_cast<const int32_t*>(rec + kAdapterRecIndex) != adapter_index)
          continue;
        if (system == kRenderDx9) {
          std::memcpy(reinterpret_cast<void*>(kCfgDx9AdapterGuid),
                      rec + kAdapterRecDx9Guid, 16);
          char device[64] = {0};
          CopyClientStringRaw(rec + kAdapterRecDx9Name, device, sizeof(device));
          std::memset(reinterpret_cast<void*>(kCfgDx9DeviceName), 0, 32);
          std::strncpy(reinterpret_cast<char*>(kCfgDx9DeviceName), device, 31);
        } else {
          std::memcpy(reinterpret_cast<void*>(kCfgDx7DriverGuid),
                      rec + kAdapterRecDx7Drv, 16);
          std::memcpy(reinterpret_cast<void*>(kCfgDx7DeviceGuid),
                      rec + kAdapterRecDx7Dev, 16);
          char name[64] = {0};
          CopyClientStringRaw(rec + kAdapterRecDesc, name, sizeof(name));
          std::memset(reinterpret_cast<void*>(kCfgDx7AdapterName), 0, 40);
          std::strncpy(reinterpret_cast<char*>(kCfgDx7AdapterName), name, 39);
        }
        written = true;
        break;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { written = false; }
  }
  const int kStrings[] = {kAdapterRecDesc, kAdapterRecDx9Name};
  FreeVector(&vec, kAdapterRecordSize, kStrings, 2);

  // L'adaptateur demandé n'existe pas : ne RIEN écrire. Une configuration à
  // moitié valide est le seul échec dont on ne se relève pas depuis le jeu — le
  // client ne redémarrerait plus.
  if (!written) return false;

  WriteInt(kCfgRenderSystemAddr, system);
  WriteInt(kCfgWidthAddr, width);
  WriteInt(kCfgHeightAddr, height);
  WriteInt(kCfgBppAddr, bpp);
  WriteInt(kCfgFullscreenAddr, fullscreen ? 1 : 0);

  // Sauver TOUT DE SUITE. Le client, lui, n'écrit son fichier d'options qu'à
  // l'arrêt propre : une déconnexion brutale ou un plantage entre ce clic et la
  // fermeture emporterait le réglage, et le joueur croirait le panneau menteur.
  __try {
    reinterpret_cast<OptionSave_t>(kOptionSaveAddr)(
        reinterpret_cast<void*>(kOptionContextAddr));
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return true;
}

void ShutdownClient() {
  __try {
    void* connection = reinterpret_cast<ConnGetInstance_t>(kConnGetInstanceAddr)();
    if (connection)
      reinterpret_cast<ConnDisconnect_t>(kConnDisconnectAddr)(connection);
    void* mode = *reinterpret_cast<void**>(kCurrentModePtrAddr);
    if (mode) uiwnd::Vf<DispCmd_t>(mode, kVfDispCmd)(mode, kCmdShutdown, 0, 0, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

}  // namespace graphics

}  // namespace gamesettings
