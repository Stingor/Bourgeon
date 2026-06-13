#include "plugins/moonlight_ui.h"

#include <Windows.h>
#include <cstring>

#include "bourgeon.h"
#include "imgui.h"
#include "plugins/discord_relay.h"
#include "spdlog/fmt/fmt.h"
#include "utils/byte_pattern.h"
#include "utils/log_console.h"

MoonlightUi::MoonlightUi() {
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodeFromServer);
  FindChatBgInstruction();
}

// ── Chat background color ─────────────────────────────────────────────────

void MoonlightUi::FindChatBgInstruction() {
  // Locate the .text section via the PE header of the main module.
  const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandle(nullptr));
  const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const auto* sec  = IMAGE_FIRST_SECTION(nt);

  uint8_t* text_start = nullptr;
  size_t   text_size  = 0;
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    if (std::memcmp(sec->Name, ".text", 5) == 0) {
      text_start = const_cast<uint8_t*>(base) + sec->VirtualAddress;
      text_size  = sec->Misc.VirtualSize;
      break;
    }
  }

  if (!text_start) {
    LogError("[MoonlightUi] chat_bg: .text section not found");
    return;
  }

  // Pattern: C7 ?? D8 00 00 00 ?? ?? ?? ??
  //          ^^^^^^^^^^^^^^^^^^^^  ─ MOV [reg+0xD8], imm32
  //                                ^─────────^ ─ the ARGB value we patch (+6)
  // Wildcards on the ModRM byte (any register) and the 4-byte ARGB immediate
  // so the pattern works whether or not a WARP binary patch was already applied.
  BytePattern pat(
      {0xC7, 0x00, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      "x?xxxx????"
  );

  auto* found = static_cast<uint8_t*>(pat.Search(text_start, text_size));
  if (!found) {
    LogError("[MoonlightUi] chat_bg: init instruction not found in .text");
    return;
  }

  // The 4-byte ARGB immediate starts 6 bytes into the instruction
  // (1 opcode + 1 ModRM + 4 displacement bytes).
  chat_bg_instr_ = reinterpret_cast<uint32_t*>(found + 6);

  // Make the immediate field writable (one VirtualProtect, never restored).
  DWORD old_protect;
  VirtualProtect(chat_bg_instr_, sizeof(uint32_t),
                 PAGE_EXECUTE_READWRITE, &old_protect);

  // Seed the ImGui picker from whatever color is currently in the instruction
  // (works with the original 0x66000000 or a WARP-patched value).
  const uint32_t argb = *chat_bg_instr_;
  chat_bg_color_[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f; // R
  chat_bg_color_[1] = static_cast<float>((argb >>  8) & 0xFF) / 255.0f; // G
  chat_bg_color_[2] = static_cast<float>( argb        & 0xFF) / 255.0f; // B
  chat_bg_color_[3] = static_cast<float>((argb >> 24) & 0xFF) / 255.0f; // A

  LogInfo("[MoonlightUi] chat_bg: instruction at VA 0x{:08X}, initial color 0x{:08X}",
          reinterpret_cast<uint32_t>(found), argb);
}

void MoonlightUi::PatchInstruction(uint32_t argb) {
  if (!chat_bg_instr_) return;
  *chat_bg_instr_ = argb;
  FlushInstructionCache(GetCurrentProcess(), chat_bg_instr_, sizeof(uint32_t));
}

void MoonlightUi::PatchExistingObjects(uint32_t argb) {
  HANDLE heap = GetProcessHeap();
  if (!heap) return;
  if (!HeapLock(heap)) return;

  PROCESS_HEAP_ENTRY entry = {};
  int count = 0;
  while (HeapWalk(heap, &entry)) {
    if (!(entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
    if (entry.cbData < kChatBgColorOff + sizeof(uint32_t)) continue;

    // Chat window objects start with their vtable pointer.
    const auto* vtable_ptr = static_cast<const uint32_t*>(entry.lpData);
    if (*vtable_ptr != kChatWinVtable) continue;

    auto* color_field = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(entry.lpData) + kChatBgColorOff);
    *color_field = argb;
    ++count;
  }

  HeapUnlock(heap);
  LogInfo("[MoonlightUi] chat_bg: patched {} existing window object(s)", count);
}

uint32_t MoonlightUi::PickerToArgb() const {
  const uint32_t r = static_cast<uint32_t>(chat_bg_color_[0] * 255.0f + 0.5f) & 0xFF;
  const uint32_t g = static_cast<uint32_t>(chat_bg_color_[1] * 255.0f + 0.5f) & 0xFF;
  const uint32_t b = static_cast<uint32_t>(chat_bg_color_[2] * 255.0f + 0.5f) & 0xFF;
  const uint32_t a = static_cast<uint32_t>(chat_bg_color_[3] * 255.0f + 0.5f) & 0xFF;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

// ── Server settings sync ──────────────────────────────────────────────────

void MoonlightUi::UpdateRelay() {
  if (auto* relay = Bourgeon::Instance().discord_relay()) {
    relay->set_chat_active(discord_chat_ && in_gonryun_);
  }
}

void MoonlightUi::OnModeSwitch(ModeMgr::ModeType mode_type,
                               const char* map_name) {
  in_game_    = (mode_type == ModeMgr::ModeType::kGame);
  in_gonryun_ = in_game_ && (strncmp(map_name, kDiscordMap, sizeof(kDiscordMap) - 1) == 0);
  UpdateRelay();
}

// ZC packet layout (data points past [opcode:2][total_len:2]):
//   [count:2][{id:2, value:2} * count]
void MoonlightUi::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                               uint16_t len) {
  if (opcode != kOpcodeFromServer) return;
  if (len < 2) return;

  const uint16_t count = *reinterpret_cast<const uint16_t*>(data);
  const uint16_t expected_len = static_cast<uint16_t>(2 + count * 4);
  if (len < expected_len) {
    LogError("[MoonlightUi] ZC_BOURGEON_SETTINGS truncated: len={} count={}", len, count);
    return;
  }

  for (uint16_t i = 0; i < count; ++i) {
    const uint16_t id    = *reinterpret_cast<const uint16_t*>(data + 2 + i * 4);
    const uint16_t value = *reinterpret_cast<const uint16_t*>(data + 2 + i * 4 + 2);
    switch (id) {
      case kSettingDiscord:
        discord_chat_ = (value != 0);
        UpdateRelay();
        LogInfo("[MoonlightUi] discord={}", discord_chat_);
        break;
      default:
        LogInfo("[MoonlightUi] unknown setting id={} value={}", id, value);
        break;
    }
  }
}

void MoonlightUi::SendSetting(uint16_t id, uint16_t value) {
  uint8_t buf[8];
  *reinterpret_cast<uint16_t*>(buf)     = kOpcodeToServer;
  *reinterpret_cast<uint16_t*>(buf + 2) = 8;
  *reinterpret_cast<uint16_t*>(buf + 4) = id;
  *reinterpret_cast<uint16_t*>(buf + 6) = value;
  Bourgeon::Instance().SendPacket(buf, sizeof(buf));
}

// ── ImGui panel ───────────────────────────────────────────────────────────

void MoonlightUi::OnRenderUI() {
  if (!in_game_) return;

  if (ImGui::Begin("Moonlight-Destiny")) {

    // ── Discord relay ───────────────────────────────────────────────────
    ImGui::TextUnformatted("Discord");
    ImGui::Separator();
    if (ImGui::Checkbox("Chat relay (Gonryun only)", &discord_chat_)) {
      UpdateRelay();
      SendSetting(kSettingDiscord, discord_chat_ ? 1 : 0);
    }

    ImGui::Spacing();

    // ── Chat window appearance ──────────────────────────────────────────
    ImGui::TextUnformatted("Chat window");
    ImGui::Separator();

    if (chat_bg_instr_) {
      // Live patch: update the instruction immediate on every drag tick
      // (cheap 4-byte write, imperceptible cost).
      if (ImGui::ColorEdit4("Background##chatbg", chat_bg_color_,
                            ImGuiColorEditFlags_AlphaBar)) {
        PatchInstruction(PickerToArgb());
      }
      // Expensive heap walk: only when the user releases the control.
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        PatchExistingObjects(PickerToArgb());
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Color and opacity of the chat window background.\n"
            "Changes take effect immediately on new windows;\n"
            "existing windows are updated on release.");
      }
    } else {
      ImGui::TextDisabled("(chat background patch unavailable)");
    }
  }
  ImGui::End();
}
