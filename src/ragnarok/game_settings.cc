#include "ragnarok/game_settings.h"

#include <windows.h>

#include <cstring>

#include "bourgeon.h"  // Bourgeon::SendPacket (le CZ du réglage RODEX)
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

// La SEULE fonction qui insère une clé absente : elle résout un nom de commande
// slash en identifiant d'option, puis écrit sans exiger que la clé préexiste.
// Rend 3 quand la commande est inconnue. Cf. le commentaire de `SetOnByCommand`.
using SetFlagByCommand_t = int(__cdecl*)(const char*, char);
constexpr uintptr_t kSetFlagByCommandAddr = 0x0068fc70;
constexpr int kCommandUnknown = 3;

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

// Le drapeau RODEX, écrit UNIQUEMENT par les deux handlers de réception
// (0x00CF8290 pour la liste d'entrée, 0x00CF97A0 pour un changement).
constexpr uintptr_t kRodexAcceptAllAddr = 0x01602430;

// CZ_CONFIG : `{ u16 opcode ; u16 bourrage ; i32 type ; i32 valeur }`, 12 octets
// fixes — c'est la longueur que la table du client donne pour 0x0B93, et les
// champs sont bien alignés sur 4 (relevé au désassemblage de 0x009EF0F0, pas
// déduit d'un `struct` supposé packé).
constexpr uint16_t kCzConfigOpcode  = 0x0b93;
constexpr int32_t  kCzConfigRodex   = 1;  // le seul type que ce client émette

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

// ── RODEX ───────────────────────────────────────────────────────────────────

bool RodexAcceptsEveryone() {
  __try {
    return *reinterpret_cast<const uint8_t*>(kRodexAcceptAllAddr) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

void RequestRodexAcceptsEveryone(bool accept) {
#pragma pack(push, 1)
  struct CzConfig {
    uint16_t opcode;
    uint16_t padding;
    int32_t  type;
    int32_t  value;
  };
#pragma pack(pop)
  static_assert(sizeof(CzConfig) == 12, "CZ 0x0B93 fait 12 octets");

  CzConfig packet{};
  packet.opcode  = kCzConfigOpcode;
  packet.padding = 0;
  packet.type    = kCzConfigRodex;
  packet.value   = accept ? 1 : 0;
  Bourgeon::Instance().SendPacket(reinterpret_cast<const uint8_t*>(&packet),
                                  sizeof(packet));
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

}  // namespace gamesettings
