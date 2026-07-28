#pragma once

#include "features/plugin.h"

// Removes the CLIENT-side gate that silently blocks chat while Berserk
// (EFST_BERSERK 0x6b / 107) is active.
//
// RE (20250716 client, no-ASLR addr == live) — see project_berserk_chat_gate_re:
//  - Status_IsBerserkActive @ 0x00D8E6C0 scans g_StatusEffectList (the local
//    player's status-icon list, 0x0136E6C8) and returns 1 when EFST 0x6b is
//    present.
//  - The 7 chat-channel handlers inside CMode::SendMsg (0x00C86740) — public
//    (cmd 0x06 @0x00C8A63A / 0x00C8A81B), party, guild, whisper
//    (@0x00C8B1C8/B608/B88F/BB0F/0x00C91E68) — each do `CMP EAX,1 / JZ exit`,
//    so a `1` return drops the outgoing CZ_REQUEST_CHAT (opcode 0xF3) before
//    it is ever sent: the input box clears but nothing reaches the chat.
//  - Emotes (/shy, ...) are a different command and are NOT gated.
//
// Fix: force Status_IsBerserkActive to always return 0 (xor eax,eax ; ret).
// All 7 callers are chat channels, so this unblocks chat only and leaves the
// item/skill Berserk restrictions untouched. Equivalent to the server-side fix
// of not sending EFST_BERSERK (which likewise leaves 0x6b out of the list).
//
// Applied once at construction; no toggle, no UI.
class BerserkChatUnlock : public Plugin {
 public:
  BerserkChatUnlock();

  const char* name() const override { return "BerserkChatUnlock"; }
};
