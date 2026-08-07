#include "features/patches/chat.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "imgui.h"
#include "ui/color_codec.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/byte_pattern.h"
#include "utils/log_console.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Fonds des fenêtres de chat ───────────────────────────────────────────────
// Recherche par motif des sites natifs qui peignent le fond des fenêtres de chat,
// puis réécriture de la couleur — dans le code (immédiats) et dans les objets déjà
// construits (parcours du tas). C'est le code le PLUS couplé au client de tout le
// projet : recherche de motifs, VirtualProtect, HeapWalk.
//
// Il a longtemps vécu chez MoonlightUi, qui n'a pourtant rien à voir avec le
// chat : c'est le panneau de réglages qui l'avait attiré là. Le patch, son état
// et son UI appartiennent au plugin Chat ; MoonlightUi n'en garde que la
// persistance et l'endroit où le panneau s'affiche.

namespace {

// Description statique de chaque site de fond que l'on patche. Les jokers
// couvrent l'immédiat ARGB de 4 octets, pour que la recherche fonctionne qu'un
// patch binaire WARP ait déjà été appliqué à l'exe ou non.
struct ChatBgSiteDesc {
  int                  group;        // ChatTweaks::BgGroupId
  std::vector<uint8_t> bytes;
  const char*          mask;
  size_t               imm_off;      // offset de l'ARGB (4 o) dans la correspondance
  uint32_t             heap_vtable;  // 0 = pas de recoloration d'objets pour ce site
  uint32_t             heap_field;
};

const ChatBgSiteDesc kChatBgSites[] = {
  // ── Chat principal ───────────────────────────────────────────────────────
  // Site 1 : ctor UINewChatWnd  MOV [reg+0xD8], imm32  (couleur rangée dans obj+0xD8)
  {0, {0xC7, 0x00, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
      "x?xxxx????", 6, 0x01037F80, 0xD8},
  // Site 2 : couleur d'onglet actif  CMP / MOV ECX,imm32 / MOV EAX,0x99000000 / CMOVZ
  {0, {0x3B, 0x9E, 0x14, 0x01, 0x00, 0x00, 0xB9, 0x00, 0x00, 0x00, 0x00,
       0xB8, 0x00, 0x00, 0x00, 0x99, 0x0F, 0x44, 0xC1},
      "xxxxxxx????xxxxxxxx", 7, 0, 0},
  // ── Fenêtres de chat détachées ───────────────────────────────────────────
  // Site 3 : bordure extérieure  MOV EAX,[ESI+0xEC] / PUSH imm32 / PUSH [ESI+0xE8]
  {1, {0x8B, 0x86, 0xEC, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x00, 0x00,
       0xFF, 0xB6, 0xE8, 0x00, 0x00, 0x00},
      "xxxxxxx????xxxxxx", 7, 0, 0},
  // Site 8 : ctor UISubChatHisWnd  MOV [ESI+0xD4], imm32 / MOV [ESI+0xC4], EAX
  {1, {0xC7, 0x86, 0xD4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x89, 0x86, 0xC4, 0x00, 0x00, 0x00},
      "xxxxxx????xxxxxx", 6, 0x01037EA8, 0xD4},
  // ── Fenêtre de chuchotement (1:1) ────────────────────────────────────────
  // Site 6 : MOV EAX,[ESI+0x14] / PUSH imm32 / PUSH [ESI+0x18] / SUB EAX,2
  {2, {0x8B, 0x46, 0x14, 0x68, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x76, 0x18,
       0x83, 0xE8, 0x02},
      "xxxx????xxxxxx", 4, 0, 0},
};

// Le parcours lui-même. Fonction SÉPARÉE parce qu'elle porte un __try : MSVC
// refuse (C2712) qu'une même fonction mêle SEH et objets à destructeur — or
// l'appelant en a un, le garde de verrou. D'où aussi les paramètres POD : un
// tableau brut plutôt que le std::vector de l'appelant.
//
// Le __try n'est pas décoratif. Sans lui, une faute pendant le parcours laissait
// le tas du process VERROUILLÉ à vie : le client gelait entièrement au premier
// malloc d'un autre thread, sans rien dans le log.
// Copie POD des cibles : BgHeapTarget est un type PRIVÉ de ChatTweaks, hors de
// portée d'une fonction libre. Le tableau est rempli par l'appelant AVANT de
// verrouiller le tas — allouer sous HeapLock s'interbloquerait avec soi-même.
struct HeapRecolourTarget {
  uint32_t vtable_va;
  uint32_t color_field_off;
};
constexpr size_t kMaxHeapTargets = 8;

int WalkAndRecolour(HANDLE heap, const HeapRecolourTarget* targets, size_t target_count,
                    uint32_t argb) {
  int recoloured = 0;
  __try {
    PROCESS_HEAP_ENTRY entry = {};
    while (HeapWalk(heap, &entry)) {
      if (!(entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
      // Début de l'objet — et NON sa vtable : c'est son déréférencement, plus
      // bas, qui donne la vtable. Sur du parcours de tas brut, cet écart d'un
      // niveau d'indirection est exactement ce qui produit un crash.
      const auto* obj_first_word = static_cast<const uint32_t*>(entry.lpData);
      for (size_t i = 0; i < target_count; ++i) {
        if (entry.cbData < targets[i].color_field_off + sizeof(uint32_t)) continue;
        if (*obj_first_word != targets[i].vtable_va) continue;
        *reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(entry.lpData) + targets[i].color_field_off) = argb;
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

void ChatTweaks::FindBackgroundSites() {
  bg_[kBgMain].label        = i18n::Tr("Chat principal");
  bg_[kBgMain].yaml_key     = "chat_bg";          // conservée pour la compatibilité
  bg_[kBgDetached].label    = i18n::Tr("Fenêtres détachées");
  bg_[kBgDetached].yaml_key = "chat_bg_detached";
  bg_[kBgWhisper].label     = "Chuchotement (1:1)";
  bg_[kBgWhisper].yaml_key  = "chat_bg_whisper";

  // Localise la section .text via l'en-tête PE du module principal.
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
    LogError("[Chat] fond : section .text introuvable");
    return;
  }

  int site_idx = 0;
  for (const ChatBgSiteDesc& desc : kChatBgSites) {
    const char* group_name = bg_[desc.group].label;
    BytePattern pattern(desc.bytes, desc.mask);
    auto* found = static_cast<uint8_t*>(pattern.Search(text_start, text_size));
    if (!found) {
      LogError("[Chat] fond : site #{} (groupe {} '{}') INTROUVABLE",
               site_idx, desc.group, group_name);
      ++site_idx;
      continue;
    }
    auto* immediate = reinterpret_cast<uint32_t*>(found + desc.imm_off);

    // Rend l'immédiat inscriptible (un seul VirtualProtect, jamais restauré).
    //
    // Le retour est VÉRIFIÉ, et le site ignoré s'il échoue : sinon `immediate`
    // partait dans instrs quoi qu'il arrive, et le premier changement de couleur
    // écrivait dans une page restée en lecture seule — violation d'accès dans
    // .text. Un protecteur (« Lotus »), CFG ou ACG suffisent à provoquer ça, et
    // le reste du code sait déjà vivre sans un site (instrs vide, bg_found_).
    DWORD old_protect = 0;
    if (!VirtualProtect(immediate, sizeof(uint32_t), PAGE_EXECUTE_READWRITE,
                        &old_protect)) {
      LogError("[Chat] fond : site #{} (groupe {} '{}') non inscriptible "
               "(VirtualProtect erreur {}) — site ignoré",
               site_idx, desc.group, group_name, GetLastError());
      ++site_idx;
      continue;
    }

    BgGroup& group = bg_[desc.group];
    group.argb_imm_ptrs.push_back(immediate);
    if (desc.heap_vtable) group.heap_targets.push_back({desc.heap_vtable, desc.heap_field});
    bg_found_ = true;
    ++site_idx;
  }

  // Amorce chaque picker avec la couleur actuellement dans son premier immédiat.
  for (int i = 0; i < kBgCount; ++i) {
    BgGroup& group = bg_[i];
    if (!group.argb_imm_ptrs.empty()) ro::PickerFromArgb(group.picker_rgba, *group.argb_imm_ptrs.front());
  }
}

const char* ChatTweaks::bg_yaml_key(int group) const {
  if (group < 0 || group >= kBgCount) return "";
  return bg_[group].yaml_key;
}

float* ChatTweaks::bg_color(int group) {
  if (group < 0 || group >= kBgCount) return nullptr;
  return bg_[group].picker_rgba;
}

void ChatTweaks::ApplyBackground(int group, uint32_t argb, bool walk_heap) {
  if (group < 0 || group >= kBgCount) return;
  BgGroup& target = bg_[group];
  for (uint32_t* immediate : target.argb_imm_ptrs) {
    *immediate = argb;
    FlushInstructionCache(GetCurrentProcess(), immediate, sizeof(uint32_t));
  }
  if (walk_heap && !target.heap_targets.empty()) PatchBackgroundObjects(target, argb);
}

void ChatTweaks::ApplyAllBackgrounds() {
  for (int i = 0; i < kBgCount; ++i) {
    if (bg_[i].argb_imm_ptrs.empty()) continue;
    // walk_heap = TRUE, et il le faut. Les réglages ne sont relus qu'à l'ENTRÉE
    // EN JEU, donc une fois le HUD construit : la fenêtre de chat principale est
    // déjà là. Le patch des immédiats .text ne vaut que pour les fenêtres créées
    // APRÈS — sans le parcours, le chat principal gardait sa couleur par défaut
    // à chaque login, en donnant l'impression que le réglage n'était pas
    // sauvegardé alors qu'il était correctement écrit et relu.
    ApplyBackground(i, ro::ArgbFromPicker(bg_[i].picker_rgba), /*walk_heap=*/true);
  }
}

void ChatTweaks::PatchBackgroundObjects(const BgGroup& group, uint32_t argb) {
  if (group.heap_targets.empty()) return;

  // Tout ce qui alloue doit être fait AVANT HeapLock.
  HeapRecolourTarget targets[kMaxHeapTargets];
  size_t target_count = 0;
  for (const BgHeapTarget& target : group.heap_targets) {
    if (target_count == kMaxHeapTargets) {
      LogError("[Chat] fond[{}] : {} cibles de tas pour {} places — "
               "les suivantes ne seront pas recolorées",
               group.yaml_key, group.heap_targets.size(), kMaxHeapTargets);
      break;
    }
    targets[target_count++] = {target.vtable_va, target.color_field_off};
  }

  HANDLE heap = GetProcessHeap();
  HeapLockGuard lock(heap);
  if (!lock.locked()) return;

  const int recoloured = WalkAndRecolour(heap, targets, target_count, argb);
  if (recoloured < 0)
    LogError("[Chat] fond[{}] : faute pendant le parcours du tas — "
             "recoloration des objets vivants abandonnée", group.yaml_key);
}

// ── Panneau ──────────────────────────────────────────────────────────────────

bool ChatTweaks::DrawBackgroundGroup(int group_id) {
  if (group_id < 0 || group_id >= kBgCount) return false;
  BgGroup& group = bg_[group_id];
  if (group.argb_imm_ptrs.empty()) return false;
  bool changed = false;

  ImGui::PushID(group.yaml_key);
  const ImVec4 swatch(group.picker_rgba[0], group.picker_rgba[1], group.picker_rgba[2], group.picker_rgba[3]);
  if (ImGui::ColorButton("##btn", swatch, ImGuiColorEditFlags_AlphaPreview,
                         ImVec2(20, 20)))
    ImGui::OpenPopup("picker");
  SameLine();
  ImGui::TextUnformatted(group.label);

  if (ImGui::BeginPopup("picker")) {
    // ── Préréglages partagés ────────────────────────────────────────────────
    if (!bg_presets_.empty()) {
      ImGui::TextUnformatted(i18n::Tr("Préréglages :"));
      int delete_idx = -1;
      for (int i = 0; i < static_cast<int>(bg_presets_.size()); ++i) {
        const BgPreset& preset = bg_presets_[i];
        ImGui::PushID(i);
        // Un clic sur la pastille amène le picker sur la couleur du préréglage,
        // l'applique au fond et déclenche la sauvegarde.
        if (ImGui::ColorButton("##swatch", ro::ImVec4FromArgb(preset.argb),
                               ImGuiColorEditFlags_AlphaPreview |
                                   ImGuiColorEditFlags_NoTooltip,
                               ImVec2(18, 18))) {
          ro::PickerFromArgb(group.picker_rgba, preset.argb);
          ApplyBackground(group_id, preset.argb, true);
          changed = true;
        }
        SameLine();
        ImGui::TextUnformatted(preset.name.c_str());
        SameLine();
        if (ro::RoSmallButton("x")) delete_idx = i;
        ImGui::PopID();
      }
      if (delete_idx >= 0) {
        bg_presets_.erase(bg_presets_.begin() + delete_idx);
        changed = true;
      }
      SeparatorText(i18n::Tr("Sauvegarder une couleur comme preset"));
    }
    // ── Enregistrer la couleur courante ─────────────────────────────────────
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputTextWithHint("##preset_name", i18n::Tr("Nom du préréglage"), preset_name_buf_,
                             sizeof(preset_name_buf_));
    SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 3.0f);  // aligne le bouton sur le champ
    if (ro::RoButton("Enregistrer") && preset_name_buf_[0] != '\0') {
      bg_presets_.push_back({preset_name_buf_, ro::ArgbFromPicker(group.picker_rgba)});
      preset_name_buf_[0] = '\0';
      changed = true;
    }
    SeparatorText(i18n::Tr("Choisir une couleur"));
    if (ImGui::ColorPicker4("##pick", group.picker_rgba,
                            ImGuiColorEditFlags_AlphaBar |
                                ImGuiColorEditFlags_NoSidePreview)) {
      ApplyBackground(group_id, ro::ArgbFromPicker(group.picker_rgba), false);
      group.picker_drag_in_progress = true;
    }
    if (ro::RoButton("Fermer")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // Finalisation HORS du popup, à dessein. Elle était dedans : Échap, un clic
  // hors du popup ou le bouton Fermer pendant un glissement le fermaient avant
  // qu'elle ne soit atteinte, et `editing` restait à true. La couleur était alors
  // visible tout de suite — le .text est déjà patché — mais jamais propagée aux
  // fenêtres DÉJÀ OUVERTES, ni persistée : elle disparaissait au login suivant.
  //
  // !IsMouseDown plutôt que IsMouseReleased : le relâchement est un événement
  // d'UNE frame, qu'un popup fermé entre-temps fait manquer.
  if (group.picker_drag_in_progress && !ImGui::IsMouseDown(0)) {
    ApplyBackground(group_id, ro::ArgbFromPicker(group.picker_rgba), true);
    changed = true;
    group.picker_drag_in_progress = false;
  }
  ImGui::PopID();
  return changed;
}

bool ChatTweaks::DrawPresetBar() {
  if (!bg_found_) return false;
  bool changed = false;

  ImGui::SetNextWindowBgAlpha(0.85f);
  ImGui::SetNextWindowSize(ImVec2(80.0f, 10.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(40.0f, 1.0f), ImVec2(8000.0f, 8000.0f));
  // Reprend l'arrondi de cadre/poignée/fenêtre de la fenêtre Moonlight-Destiny.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
  // Abaisse la taille minimale par fenêtre (32x32 par défaut) pour que la barre
  // puisse être réduite à une seule ligne de préréglages.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(40.0f, 1.0f));
  // Sans barre de titre (minimaliste). Reste déplaçable par le corps, et
  // redimensionnable.
  if (ImGui::Begin("Chat presets", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoNav)) {
    PushStyleCompact();
    BgGroup& main_chat = bg_[kBgMain];
    if (bg_presets_.empty()) {
      ImGui::TextDisabled(i18n::Tr("Aucun préréglage."));
      ImGui::TextDisabled(i18n::Tr("Ajoute-en depuis le"));
      ImGui::TextDisabled(i18n::Tr("sélecteur du chat."));
    } else {
      for (int i = 0; i < static_cast<int>(bg_presets_.size()); ++i) {
        const BgPreset& preset = bg_presets_[i];
        ImGui::PushID(i);
        if (ImGui::ColorButton("##sw", ro::ImVec4FromArgb(preset.argb),
                               ImGuiColorEditFlags_AlphaPreview |
                                   ImGuiColorEditFlags_NoTooltip,
                               ImVec2(14, 14))) {
          ro::PickerFromArgb(main_chat.picker_rgba, preset.argb);
          ApplyBackground(kBgMain, preset.argb, true);
          changed = true;
        }
        SameLine();
        ImGui::TextUnformatted(preset.name.c_str());
        ImGui::PopID();
        SameLine();
      }
    }
    PopStyleCompact();
  }
  ImGui::End();
  ImGui::PopStyleVar(6);
  // La fermeture se fait en décochant « Barre de préréglages » (pas de [x]).
  return changed;
}
