#include "features/patches/berserk_chat_unlock.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include "utils/memory_patch.h"  // mem::WriteCode

// ── Allow chatting while Berserk (client-side gate removal) ──────────────────
// See the header and project_berserk_chat_gate_re for the full RE. In short:
// Status_IsBerserkActive returns 1 when EFST_BERSERK (0x6b) is up, and every
// chat-channel handler in CMode::SendMsg aborts the send on that 1. Patching the
// function to always return 0 restores chat on every channel while Berserk.

namespace {

// Status_IsBerserkActive — the gate. Stock prologue is `MOV EAX,[0x0136E6C8]`
// (loads g_StatusEffectList_begin) == bytes A1 C8 E6 36 01. We refuse to patch
// anything that does not start with that exact load.
constexpr uintptr_t kIsBerserkActive = 0x00D8E6C0;

const uint8_t kStockPrologue[3] = {0xA1, 0xC8, 0xE6};  // mov eax, ds:[0136E6C8]
const uint8_t kReturnZero[3]    = {0x31, 0xC0, 0xC3};  // xor eax,eax ; ret

}  // namespace

BerserkChatUnlock::BerserkChatUnlock() {
  // Guard the exact build: only patch when the gate starts with the known
  // g_StatusEffectList_begin load. Bail silently on any other layout.
  if (std::memcmp(reinterpret_cast<void*>(kIsBerserkActive), kStockPrologue,
                  sizeof(kStockPrologue)) != 0)
    return;

  mem::WriteCode(kIsBerserkActive, kReturnZero, sizeof(kReturnZero));
}
