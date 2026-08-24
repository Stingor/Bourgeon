#include "ragnarok/emotion_hotkey.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>

#include "ragnarok/globals.h"  // rag::kSessionAddr, kOwnGuildIdAddr, kStdStringAssignAddr
#include "ragnarok/msgstring.h"
#include "ui/ro_imgui.h"  // ro::Utf8ToLocal (les défauts viennent en UTF-8)

namespace emohotkey {
namespace {

// ── Adresses (client 20250716, no-ASLR : addr IDA == live) ───────────────────
// RE : docs/shortcut_list_re.md §3, §5 et §6.

// 🔴 CES TROIS ADRESSES SONT LES CHAMPS D'UN std::vector, PAS UN TABLEAU.
// `0x015FB398` contient le POINTEUR vers le premier `std::string` ; il faut le
// déréférencer avant d'indexer. Le décompilé d'IDA mélange les deux notations
// d'une ligne à l'autre, et le lire au premier degré donne un tableau fantôme au
// milieu des globales voisines.
constexpr uintptr_t kMacrosFirst = 0x015fb398;  // _Myfirst
constexpr uintptr_t kMacrosLast  = 0x015fb39c;  // _Mylast

// std::string MSVC : buffer SSO de 16 octets, puis taille, puis capacité.
constexpr size_t kStringStride = 0x18;
constexpr size_t kStringSizeOff = 0x10;
constexpr size_t kStringCapOff  = 0x14;
constexpr size_t kSsoCapacity   = 15;

// std::string::assign — __thiscall(this, src, len). L'annuaire (`ragnarok/globals.h`)
// ne porte que le destructeur ; l'assign est redéclaré ici comme il l'est dans
// `user_hotkey.cc` et `chat_window.cc`.

// UserSettings_SaveJson — __thiscall, `this` = la VALEUR de 0x01251668 (vérifié au
// désassemblage : `mov ecx, dword_1251668`, pas `mov ecx, offset`).
constexpr uintptr_t kSaveJsonAddr = 0x0059e950;
constexpr uintptr_t kSaveJsonSelf = 0x01251668;

// ChatMacro_SendEmotionHotkeySlot(slot) — __stdcall, un argument pile (`retn 4`).
// L'`ecx` posé juste avant l'appel dans le natif est un résidu : la fonction ne
// lit aucun `this`.
constexpr uintptr_t kSendMacroAddr = 0x00a47400;

// La cible d'envoi et ses gardes d'appartenance (répliques du natif).

using StrAssign_t = void*(__thiscall*)(void*, const char*, size_t);
using SaveJson_t  = int(__fastcall*)(void*, void*);
using SendMacro_t = void(__stdcall*)(int);
using PartyCount_t = int(__fastcall*)(void*, void*);

// Les dix valeurs d'usine. 🔴 Le 0x224 (`/lv2`) est SAUTÉ par le chargeur du
// client : la liste n'est pas dix ids consécutifs.
const int kDefaultMsgIds[kSlotCount] = {
    0x220, 0x221, 0x222, 0x223, 0x225, 0x226, 0x227, 0x228, 0x229, 0x22A};

// Adresse du n-ième `std::string` du vecteur, ou nullptr si le vecteur n'a pas
// exactement dix entrées. POD only : appelée depuis des blocs SEH.
uint8_t* SlotAddr(int slot) {
  if (slot < 0 || slot >= kSlotCount) return nullptr;
  uint8_t* first = nullptr;
  uint8_t* last  = nullptr;
  __try {
    first = *reinterpret_cast<uint8_t**>(kMacrosFirst);
    last  = *reinterpret_cast<uint8_t**>(kMacrosLast);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
  if (!first || !last) return nullptr;
  if (static_cast<size_t>(last - first) != kStringStride * kSlotCount)
    return nullptr;
  return first + static_cast<size_t>(slot) * kStringStride;
}

}  // namespace

bool Ready() { return SlotAddr(0) != nullptr; }

bool ReadLocal(int slot, char* out_local, size_t out_size) {
  if (!out_local || out_size == 0) return false;
  out_local[0] = '\0';
  uint8_t* s = SlotAddr(slot);
  if (!s) return false;

  __try {
    const uint32_t size = *reinterpret_cast<const uint32_t*>(s + kStringSizeOff);
    const uint32_t cap  = *reinterpret_cast<const uint32_t*>(s + kStringCapOff);
    const char* data = (cap > kSsoCapacity)
                           ? *reinterpret_cast<const char* const*>(s)
                           : reinterpret_cast<const char*>(s);
    if (!data) return false;
    // Une taille aberrante veut dire qu'on ne lit pas un std::string : on rend
    // vide plutôt que de recopier n'importe quoi.
    if (size > kMaxBytes * 8) return false;
    size_t n = size;
    if (n > out_size - 1) n = out_size - 1;
    std::memcpy(out_local, data, n);
    out_local[n] = '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out_local[0] = '\0';
    return false;
  }
  return true;
}

bool WriteLocal(int slot, const char* local) {
  uint8_t* s = SlotAddr(slot);
  if (!s) return false;
  if (!local) local = "";

  size_t len = std::strlen(local);
  if (len > kMaxBytes) len = kMaxBytes;

  __try {
    // Le `assign` du client, sur SON std::string : c'est exactement ce que fait
    // `EmotionHotkey_SaveFromEditBoxes`, donc la croissance hors-SSO passe par
    // l'allocateur du jeu et le vecteur reste cohérent pour ses propres
    // sauvegardes.
    reinterpret_cast<StrAssign_t>(rag::kStdStringAssignAddr)(s, local, len);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool Save() {
  __try {
    void* self = *reinterpret_cast<void**>(kSaveJsonSelf);
    if (!self) return false;
    reinterpret_cast<SaveJson_t>(kSaveJsonAddr)(self, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool Send(int slot) {
  if (slot < 0 || slot >= kSlotCount) return false;
  if (!Ready()) return false;
  __try {
    reinterpret_cast<SendMacro_t>(kSendMacroAddr)(slot);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

const char* DefaultLocal(int slot) {
  if (slot < 0 || slot >= kSlotCount) return nullptr;
  const char* utf8 = msgstr::Utf8(kDefaultMsgIds[slot]);
  if (!utf8 || !*utf8) return nullptr;
  return ro::Utf8ToLocal(utf8);
}

Target CurrentTarget() {
  int mode = 0;
  __try {
    mode = *reinterpret_cast<const int*>(rag::kInputTargetModeAddr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return Target::kPublic;
  }

  // Les gardes du natif, dans son ordre : sans guilde / sans groupe / sans clan,
  // l'envoi retombe en public. Les montrer ici évite de promettre une cible que
  // le client n'utilisera pas.
  if (mode == 2) {
    __try {
      if (rag::OwnGuildId() != 0)
        return Target::kGuild;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return Target::kPublic;
  }
  if (mode == 1) {
    int count = 0;
    __try {
      count = reinterpret_cast<PartyCount_t>(rag::kPartyMemberCountAddr)(
          reinterpret_cast<void*>(rag::kSessionAddr), nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      count = 0;
    }
    return count ? Target::kParty : Target::kPublic;
  }
  if (mode == 3) {
    __try {
      const uint8_t* clan = *reinterpret_cast<const uint8_t* const*>(rag::kClanStatePtrAddr);
      if (clan && clan[0x5C]) return Target::kClan;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return Target::kPublic;
  }
  return Target::kPublic;
}

}  // namespace emohotkey
