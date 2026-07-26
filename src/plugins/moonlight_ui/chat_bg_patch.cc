#include "plugins/moonlight_ui/internal.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "plugins/moonlight_ui.h"
#include "utils/byte_pattern.h"
#include "utils/log_console.h"

// ── Patch des fonds de chat ──────────────────────────────────────────────────
// Recherche par motif des sites natifs qui peignent le fond des fenêtres de chat,
// puis réécriture de la couleur — dans le code (immédiats) et dans les objets déjà
// construits (parcours du tas). C'est le code le PLUS couplé au client de tout le
// plugin : recherche de motifs, VirtualProtect, HeapWalk.
//
// Il vivait au milieu de moonlight_ui.cc, entre du chargement de réglages et du
// rendu ImGui. L'isoler ici met une frontière claire autour du seul endroit qui
// écrit dans la mémoire du process, et rend kChatBgSites privé à son fichier.

namespace {
// Static description of every chat-background site we patch.  Wildcards cover
// the 4-byte ARGB immediate so the search works whether or not a WARP binary
// patch was already applied to the exe.
struct ChatBgSiteDesc {
  int                  group;        // ChatBgGroupId
  std::vector<uint8_t> bytes;
  const char*          mask;
  size_t               imm_off;      // offset of the 4-byte ARGB inside the match
  uint32_t             heap_vtable;  // 0 = no heap recolour for this site
  uint32_t             heap_field;
};

const ChatBgSiteDesc kChatBgSites[] = {
  // ── Main chat panel ──────────────────────────────────────────────────────
  // Site 1: UINewChatWnd ctor  MOV [reg+0xD8], imm32   (colour stored in obj+0xD8)
  {0, {0xC7, 0x00, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      "x?xxxx????", 6, 0x01037F80, 0xD8},
  // Site 2: selected-tab colour  CMP / MOV ECX,imm32 / MOV EAX,0x99000000 / CMOVZ
  {0, {0x3B, 0x9E, 0x14, 0x01, 0x00, 0x00, 0xB9, 0x00, 0x00, 0x00, 0x00,
       0xB8, 0x00, 0x00, 0x00, 0x99, 0x0F, 0x44, 0xC1},
      "xxxxxxx????xxxxxxxx", 7, 0, 0},
  // ── Detached chat windows ────────────────────────────────────────────────
  // Site 3: outer border  MOV EAX,[ESI+0xEC] / PUSH imm32 / PUSH [ESI+0xE8]
  {1, {0x8B, 0x86, 0xEC, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xB6, 0xE8, 0x00, 0x00, 0x00},
      "xxxxxxx????xxxxxx", 7, 0, 0},
  // Site 8: UISubChatHisWnd ctor  MOV [ESI+0xD4], imm32 / MOV [ESI+0xC4], EAX
  {1, {0xC7, 0x86, 0xD4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x89, 0x86, 0xC4, 0x00, 0x00, 0x00},
      "xxxxxx????xxxxxx", 6, 0x01037EA8, 0xD4},
  // ── Whisper (1:1) window ──────────────────────────────────────────────────
  // Site 6: MOV EAX,[ESI+0x14] / PUSH imm32 / PUSH [ESI+0x18] / SUB EAX,2
  {2, {0x8B, 0x46, 0x14, 0x68, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x76, 0x18,
       0x83, 0xE8, 0x02},
      "xxxx????xxxxxx", 4, 0, 0},
};
}  // namespace

void MoonlightUi::FindChatBgSites() {
  chat_bg_[kChatBgMain].label        = "Main chat";
  chat_bg_[kChatBgMain].yaml_key     = "chat_bg";          // kept for back-compat
  chat_bg_[kChatBgDetached].label    = "Detached windows";
  chat_bg_[kChatBgDetached].yaml_key = "chat_bg_detached";
  chat_bg_[kChatBgWhisper].label     = "Whisper (1:1)";
  chat_bg_[kChatBgWhisper].yaml_key  = "chat_bg_whisper";

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

  // LogInfo("[MoonlightUi] chat_bg: scanning .text [0x{:08X} .. +0x{:X}] for {} site(s)",
          // reinterpret_cast<uint32_t>(text_start), text_size,
          // static_cast<int>(sizeof(kChatBgSites) / sizeof(kChatBgSites[0])));

  int site_idx = 0;
  for (const ChatBgSiteDesc& d : kChatBgSites) {
    const char* gname = chat_bg_[d.group].label;
    BytePattern pat(d.bytes, d.mask);
    auto* found = static_cast<uint8_t*>(pat.Search(text_start, text_size));
    if (!found) {
      LogError("[MoonlightUi] chat_bg: site #{} (group {} '{}') NOT FOUND",
               site_idx, d.group, gname);
      ++site_idx;
      continue;
    }
    auto* imm = reinterpret_cast<uint32_t*>(found + d.imm_off);

    // Make the immediate field writable (one VirtualProtect, never restored).
    DWORD old_protect;
    VirtualProtect(imm, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &old_protect);

    ChatBgGroup& g = chat_bg_[d.group];
    g.instrs.push_back(imm);
    if (d.heap_vtable) g.heap.push_back({d.heap_vtable, d.heap_field});
    chat_bg_found_ = true;

    // LogInfo("[MoonlightUi] chat_bg: site #{} (group {} '{}') found @ VA 0x{:08X}, "
            // "imm @ +{} = 0x{:08X}{}",
            // site_idx, d.group, gname, reinterpret_cast<uint32_t>(found),
            // d.imm_off, *imm,
            // d.heap_vtable ? " [+heap recolour]" : "");
    ++site_idx;
  }

  // Seed each picker from the colour currently in its first immediate, and log a
  // per-group summary so missing sites are obvious in the log.
  for (int i = 0; i < kChatBgCount; ++i) {
    ChatBgGroup& g = chat_bg_[i];
    if (!g.instrs.empty()) PickerFromArgb(g.color, *g.instrs.front());
    // LogInfo("[MoonlightUi] chat_bg group {} '{}': {} instr site(s), {} heap target(s)",
            // i, g.label, static_cast<int>(g.instrs.size()),
            // static_cast<int>(g.heap.size()));
  }
}

void MoonlightUi::ApplyChatBg(ChatBgGroup& g, uint32_t argb, bool walk_heap) {
  for (uint32_t* p : g.instrs) {
    *p = argb;
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(uint32_t));
  }
  if (walk_heap && !g.heap.empty()) PatchChatBgObjects(g, argb);
}

void MoonlightUi::PatchChatBgObjects(const ChatBgGroup& g, uint32_t argb) {
  HANDLE heap = GetProcessHeap();
  if (!heap || !HeapLock(heap)) return;

  PROCESS_HEAP_ENTRY entry = {};
  int count = 0;
  while (HeapWalk(heap, &entry)) {
    if (!(entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
    // Window objects start with their vtable pointer.
    const auto* vtable_ptr = static_cast<const uint32_t*>(entry.lpData);
    for (const ChatBgHeapTarget& t : g.heap) {
      if (entry.cbData < t.field_off + sizeof(uint32_t)) continue;
      if (*vtable_ptr != t.vtable) continue;
      *reinterpret_cast<uint32_t*>(
          static_cast<uint8_t*>(entry.lpData) + t.field_off) = argb;
      ++count;
    }
  }

  HeapUnlock(heap);
  // LogInfo("[MoonlightUi] chat_bg[{}]: recoloured {} live object(s)", g.yaml_key, count);
}

uint32_t MoonlightUi::ArgbFromPicker(const float c[4]) {
  const uint32_t r = static_cast<uint32_t>(c[0] * 255.0f + 0.5f) & 0xFF;
  const uint32_t g = static_cast<uint32_t>(c[1] * 255.0f + 0.5f) & 0xFF;
  const uint32_t b = static_cast<uint32_t>(c[2] * 255.0f + 0.5f) & 0xFF;
  const uint32_t a = static_cast<uint32_t>(c[3] * 255.0f + 0.5f) & 0xFF;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

void MoonlightUi::PickerFromArgb(float c[4], uint32_t argb) {
  c[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f; // R
  c[1] = static_cast<float>((argb >>  8) & 0xFF) / 255.0f; // G
  c[2] = static_cast<float>( argb        & 0xFF) / 255.0f; // B
  c[3] = static_cast<float>((argb >> 24) & 0xFF) / 255.0f; // A
}
