#include "plugins/moonlight_ui/internal.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "plugins/moonlight_ui.h"
#include "ui/color_codec.h"
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

    // Rend l'immédiat inscriptible (un seul VirtualProtect, jamais restauré).
    //
    // Le retour est VÉRIFIÉ, et le site ignoré s'il échoue : sinon `imm` partait
    // dans g.instrs quoi qu'il arrive, et le premier changement de couleur
    // écrivait dans une page restée en lecture seule — violation d'accès dans
    // .text. Un protecteur (« Lotus »), CFG ou ACG suffisent à provoquer ça, et
    // le reste du code sait déjà vivre sans un site (g.instrs.empty(),
    // chat_bg_found_).
    DWORD old_protect = 0;
    if (!VirtualProtect(imm, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &old_protect)) {
      LogError("[MoonlightUi] chat_bg: site #{} (groupe {} '{}') non inscriptible "
               "(VirtualProtect erreur {}) — site ignoré",
               site_idx, d.group, gname, GetLastError());
      ++site_idx;
      continue;
    }

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
    if (!g.instrs.empty()) ro::PickerFromArgb(g.color, *g.instrs.front());
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

namespace {

// Le parcours lui-même. Fonction SÉPARÉE parce qu'elle porte un __try : MSVC
// refuse (C2712) qu'une même fonction mêle SEH et objets à destructeur — or
// l'appelant en a un, le garde de verrou. D'où aussi les paramètres POD : un
// tableau brut plutôt que le std::vector de l'appelant.
//
// Le __try n'est pas décoratif. Sans lui, une faute pendant le walk laissait le
// tas du process VERROUILLÉ à vie : le client gelait entièrement au premier
// malloc d'un autre thread, sans rien dans le log.
// Copie POD des cibles : ChatBgHeapTarget est un type PRIVÉ de MoonlightUi, hors
// de portée d'une fonction libre. Le tableau est rempli par l'appelant AVANT de
// verrouiller le tas — allouer sous HeapLock s'interbloquerait avec soi-même.
struct HeapRecolourTarget { uint32_t vtable; uint32_t field_off; };
constexpr size_t kMaxHeapTargets = 8;

int WalkAndRecolour(HANDLE heap, const HeapRecolourTarget* targets, size_t target_count,
                    uint32_t argb) {
  int recoloured = 0;
  __try {
    PROCESS_HEAP_ENTRY entry = {};
    while (HeapWalk(heap, &entry)) {
      if (!(entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
      // Un objet fenêtre commence par son pointeur de vtable.
      const auto* vtable_ptr = static_cast<const uint32_t*>(entry.lpData);
      for (size_t i = 0; i < target_count; ++i) {
        if (entry.cbData < targets[i].field_off + sizeof(uint32_t)) continue;
        if (*vtable_ptr != targets[i].vtable) continue;
        *reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(entry.lpData) + targets[i].field_off) = argb;
        ++recoloured;
      }
    }
    return recoloured;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;  // le verrou est relâché par le garde de l'appelant
  }
}

// Relâche le verrou du tas QUOI QU'IL ARRIVE — sortie anticipée comme exception.
// Un tas laissé verrouillé ne se manifeste pas par un crash mais par un gel
// total et silencieux, le pire symptôme à diagnostiquer.
class HeapLockGuard {
 public:
  explicit HeapLockGuard(HANDLE heap)
      : heap_(heap && HeapLock(heap) ? heap : nullptr) {}
  ~HeapLockGuard() { if (heap_) HeapUnlock(heap_); }
  HeapLockGuard(const HeapLockGuard&) = delete;
  HeapLockGuard& operator=(const HeapLockGuard&) = delete;
  bool locked() const { return heap_ != nullptr; }

 private:
  HANDLE heap_;
};

}  // namespace

void MoonlightUi::PatchChatBgObjects(const ChatBgGroup& g, uint32_t argb) {
  if (g.heap.empty()) return;

  // Tout ce qui alloue doit être fait AVANT HeapLock.
  HeapRecolourTarget targets[kMaxHeapTargets];
  size_t target_count = 0;
  for (const ChatBgHeapTarget& t : g.heap) {
    if (target_count == kMaxHeapTargets) {
      LogError("[MoonlightUi] chat_bg[{}]: {} cibles de tas pour {} places — "
               "les suivantes ne seront pas recolorées",
               g.yaml_key, g.heap.size(), kMaxHeapTargets);
      break;
    }
    targets[target_count++] = {t.vtable, t.field_off};
  }

  HANDLE heap = GetProcessHeap();
  HeapLockGuard lock(heap);
  if (!lock.locked()) return;

  const int recoloured = WalkAndRecolour(heap, targets, target_count, argb);
  if (recoloured < 0)
    LogError("[MoonlightUi] chat_bg[{}]: faute pendant le parcours du tas — "
             "recoloration des objets vivants abandonnée", g.yaml_key);
}

// (ArgbFromPicker / PickerFromArgb ont migré vers ui/color_codec.h, namespace ro.)
