#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "imgui.h"
#include "plugins/bourgeon_opcodes.h"
#include "plugins/plugin.h"

// Le toolkit UI générique et le gate staff vivaient ici ; ils sont désormais dans
// ui/ro_widgets.h, ui/align_grid.h et plugins/staff_gate.h. Ils restent inclus
// depuis ce header pour que les fichiers qui n'en utilisaient qu'une pièce
// continuent de compiler — mais un NOUVEAU code doit inclure directement l'en-tête
// dont il a besoin, et non ce header de plugin.
#include "plugins/staff_gate.h"
#include "ui/align_grid.h"
#include "ui/ro_widgets.h"

// « Tout-ImGui ou tout-natif » : l'inventaire (InventoryViewer::imgui_enabled_),
// le storage (StorageTweaks::imgui_enabled_) et les barres d'action
// (SkillBarTweaks::enabled_) modernes s'activent ENSEMBLE — plus de mixe possible.
// Force les 3 flags à `on`. Appelée par chacune des 3 cases synchronisées et par
// LoadSettings (réconciliation d'un yaml hérité mixé). Encapsule l'accès aux
// plugins pour que SkillBarTweaks::DrawSettings bascule l'ensemble sans inclure
// les headers des deux autres. Sûre si un plugin manque.
void SetModernInterface(bool on);

// Moonlight-Destiny settings panel — manages client/server settings sync.
class MoonlightUi : public Plugin {
 public:
  MoonlightUi();

  const char* name() const override { return "Moonlight-Destiny UI"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void OnRenderUI() override;
  void OnTick() override;

  // Demande l'écriture de bourgeon_settings.yaml. Public so sibling plugins (e.g. the
  // status-icon panel) can persist their own config through the shared file.
  //
  // ⚠ L'écriture est DIFFÉRÉE (anti-rebond) : l'appel ne fait que marquer les réglages
  // sales, et le fichier n'est réellement écrit qu'après kSettingsFlushDelayMs sans
  // nouvelle demande (flush depuis OnTick), ou immédiatement sur changement de mode.
  // C'est indispensable parce que les sites d'appel sont des widgets ImGui évalués à
  // chaque frame : un slider ou un color picker renvoie true à CHAQUE frame de
  // glissement, ce qui déclenchait autant de sérialisations YAML complètes + autant
  // d'écritures disque sur le thread de rendu (~120 pour un drag de 2 s).
  // Appeler FlushSettings() pour forcer une écriture immédiate.
  void SaveSettings();

  // Écrit tout de suite si des réglages sont en attente. À utiliser avant une
  // opération qui pourrait faire perdre la fenêtre d'anti-rebond.
  void FlushSettings();

  // Shared HUD alignment grid. Public so sibling plugins (BasicInfoTweaks bars,
  // MenuIconTweaks) can read it for snapping while dragging/resizing.
  AlignGrid grid_;

  // ── Liste autolootid (partagée) : le panneau de description enrichi
  // (ItemDescTweaks) réintègre le bouton +/- alootid. ─────────────────────────
  bool IsAlootId(uint32_t id) const;   // l'item est-il dans la liste ?
  bool AddAlootId(uint32_t id);        // ajoute + notifie serveur (false si plein/déjà)
  bool RemoveAlootId(uint32_t id);     // retire + notifie serveur (false si absent)
  const char* ItemName(uint32_t id) const;  // nom via itemInfoMerged.lua (ou nullptr)

  // ── Accès direct à une section de config ────────────────────────────────────
  // Sections de l'en-tête « Interface de jeu ». Les LIBELLÉS vivent dans la table
  // kIfaceSections du .cc, où chaque ligne porte explicitement son identifiant :
  // il n'y a donc plus deux listes à garder alignées, et insérer une section ne
  // peut plus décaler silencieusement les suivantes. kIfaceCount ferme l'enum et
  // sert de borne (OpenInterfaceSection) et de contrôle de taille à la compilation.
  enum IfaceSection {
    kIfaceSkillBar = 0, kIfaceBasicInfo, kIfaceChat, kIfaceMenuIcons,
    kIfaceStatusIcons, kIfaceQuest, kIfaceDesc, kIfaceSkin, kIfaceNpc,
    kIfaceStorage, kIfaceInventory,
    kIfaceCount,
  };
  // Ouvre le panneau Moonlight directement sur `section` : déplie la fenêtre,
  // ouvre l'en-tête « Interface de jeu », sélectionne l'entrée de nav et scrolle
  // dessus. Appelé par le bullet de barre de titre des fenêtres Bourgeon
  // (cf. ro::SetNextWindowTitleBullet) pour joindre LEUR config en un clic.
  // Prend effet au rendu suivant (sûr à appeler depuis n'importe quel OnRenderUI).
  void OpenInterfaceSection(int section);

 private:
  // Sends a single setting change to the server.
  // CZ: [opcode:2][total_len:2][id:2][value:2]
  void SendSetting(uint16_t id, uint32_t value);

  static constexpr uint16_t kOpcodeFromServer    = bopcodes::kSettings;    // ZC_BOURGEON_SETTINGS
  static constexpr uint16_t kOpcodeToServer      = bopcodes::kSetting;     // CZ_BOURGEON_SETTING
  static constexpr uint16_t kOpcodePresetList    = bopcodes::kPresetList;  // ZC_BOURGEON_PRESET_LIST
  static constexpr uint16_t kOpcodePresetCmd     = bopcodes::kPresetCmd;   // CZ_BOURGEON_PRESET_CMD
  // We read the current map name from the STANDARD client packet 0x0091
  // (ZC_NPCACK_MAPMOVE), which arrives on login and every warp/map change and
  // carries mapname[16] right after the opcode.  This needs no custom packet
  // and no server changes, so there is no opcode to collide with the client's
  // packet-length table (0x0BFC and 0x0BFF both turned out to be reserved).
  static constexpr uint16_t kOpcodeMapMove    = 0x0091;  // [opcode:2][mapname:16][x:2][y:2]
  static constexpr uint16_t kMapNameLen       = 16;      // mapname field width in 0x0091

  // Setting IDs sent via CZ 0x0F04 (must match server-side clif_parse_bourgeon_setting).
  static constexpr uint16_t kSettingShowExp     = 0;
  static constexpr uint16_t kSettingShowZeny    = 1;
  static constexpr uint16_t kSettingShowMobInfo = 2;
  static constexpr uint16_t kSettingSeparate    = 3;
  static constexpr uint16_t kSettingBlockExp    = 4;
  static constexpr uint16_t kSettingAlootRare   = 5;
  static constexpr uint16_t kSettingAlootRate   = 6;  // autoloot rate 0-100
  static constexpr uint16_t kSettingAlootPognon = 7;  // zeny threshold / 100 on wire
  static constexpr uint16_t kSettingAlootType   = 8;  // autoloottype toggle
  static constexpr uint16_t kSettingDiscordChat  = 9;  // discord relay opt-in
  static constexpr uint16_t kSettingShowDelay   = 10;
  static constexpr uint16_t kSettingShowSpeed   = 11;
  static constexpr uint16_t kSettingSellStuff   = 12;
  static constexpr uint16_t kSettingSellItem    = 13;
  static constexpr uint16_t kSettingNoAsk       = 14;
  static constexpr uint16_t kSettingNoks        = 15;  // 0=off 1=self 2=party 3=guild
  static constexpr uint16_t kSettingWings       = 16;
  static constexpr uint16_t kSettingAlootMvp      = 17;
  static constexpr uint16_t kSettingAlootMvpRwd   = 18;
  static constexpr uint16_t kSettingTriInv        = 19;
  static constexpr uint16_t kSettingTriCart       = 20;
  static constexpr uint16_t kSettingTriStorage    = 21;
  static constexpr uint16_t kSettingTriGstorage   = 22;
  static constexpr uint16_t kSettingAlootId       = 23;  // add item ID to autolootid list (0=clear)
  static constexpr uint16_t kSettingAlootIdRemove = 24;  // remove item ID from list
  static constexpr uint16_t kSettingStaff         = 26;  // niveau de groupe serveur (pc_get_group_level), lecture seule

  // Updates both directions of the relay based on current state.
  void UpdateRelay();

  static constexpr char kDiscordMap[] = "gonryun";  // sizeof - 1 = 7, correct prefix length

  bool in_game_    = false;
  bool in_gonryun_ = false;

  // Server-synced (ID 9); restored from globalreg on login.
  bool discord_chat_ = false;

  // Server-synced toggles (IDs above).
  bool show_exp_      = false;
  bool show_zeny_     = false;
  bool show_mob_info_ = false;
  bool separate_      = false;
  bool block_exp_     = false;
  bool aloot_rare_    = false;
  int  aloot_rate_    = 0;    // 0-100 (%)
  int  aloot_pognon_  = 0;    // 0-1,000,000 (z), stored locally; wire = /100
  int  aloot_type_mask_ = 0;  // bitmask uint16 : bit i = (1 << item_type i)

  bool show_delay_  = false;
  bool show_speed_  = false;
  bool sell_stuff_  = false;
  bool sell_item_   = false;
  bool no_ask_      = false;
  int  noks_        = 0;  // 0=off 1=self 2=party 3=guild
  bool wings_           = false;
  bool aloot_mvp_       = false;
  bool aloot_mvp_rwd_   = false;
  int  tri_inv_         = 0;  // e_sort_mode 0-6
  int  tri_cart_        = 0;
  int  tri_storage_     = 0;
  int  tri_gstorage_    = 0;

  std::vector<uint32_t> aloot_ids_;        // client-tracked autolootid list (max 50)
  std::vector<uint32_t> alootid_saved_ids_; // snapshot at last preset load/save
  int                   aloot_id_input_ = 0;

  struct AlootPreset { uint8_t no; std::string name; bool autoload; };
  std::vector<AlootPreset> alootid_presets_;   // received from server (ZC 0x0F07)
  uint8_t alootid_active_preset_   = 0;        // currently loaded preset no (0=none)
  uint8_t alootid_selected_preset_ = 0;        // selected in combo (may differ from active)
  char    alootid_preset_input_[64] = {};       // name field for save
  char    alootid_rename_input_[64] = {};       // name field for rename (cmd 6)
  bool    alootid_rename_open_      = false;    // checkbox: show rename field

  // Sends a preset management command to the server (CZ 0x0F06).
  void SendPresetCmd(uint8_t cmd, uint8_t no = 0, const char* name = nullptr);
  bool                  show_alootid_overlay_ = false;  // floating Add/Remove overlay on right-click

  // Address of the item description window message handler (FUN_008c18b0,
  // 20250716 client).  We hook it to capture the nameid of right-clicked items.
  static constexpr uintptr_t kItemDescWndAddr = 0x008c18b0;
  static constexpr uintptr_t kSkillDescWndAddr = 0x008ca900;  // UIItemTooltipWnd_OnMsg (skill 0x2e)
  // [edi+0x218] in the game's UI manager object (edi=0x0131F4E8): pointer to
  // the active item description window, 0 when no tooltip is open.  Written by
  // the game independently of our hook, so polling it catches silent closes
  // (e.g. comparison→non-comparison transition).
  static constexpr uintptr_t kItemDescWndGlobalPtr = 0x0131F700;

  std::unordered_map<uint32_t, std::string> item_names_;  // ID → Name from itemInfoMerged.lua
  void LoadItemNames();

  // ── Chat window background colours ───────────────────────────────────────
  // Three independent, in-game-customisable chat backgrounds. Each is backed by
  // one or more ARGB immediates in .text (default 0x66000000 = 40% alpha black),
  // patched live so future windows and the per-frame draws use the new colour.
  // The ctor-based sites also store the colour into the object, so a heap walk
  // recolours already-open windows.
  //
  //   Main     : UINewChatWnd ctor (obj+0xD8) + selected-tab colour (Draw).
  //   Detached : detached outer border (Draw) + UISubChatHisWnd ctor (obj+0xD4).
  //   Whisper  : 1:1 whisper window background (Draw).

  // Vtable addresses (20250716 client) used to recognise live window objects.
  static constexpr uint32_t kVtUINewChatWnd    = 0x01037F80;  // main chat window
  static constexpr uint32_t kVtUISubChatHisWnd = 0x01037EA8;  // detached chat history

  enum ChatBgGroupId { kChatBgMain = 0, kChatBgDetached, kChatBgWhisper, kChatBgCount };

  // A heap-stored colour field to recolour on existing objects.
  struct ChatBgHeapTarget { uint32_t vtable; uint32_t field_off; };

  // One user-facing chat-background setting.
  struct ChatBgGroup {
    const char* label    = "";                  // UI label
    const char* yaml_key = "";                  // persistence key
    std::vector<uint32_t*>        instrs;       // resolved writable .text ARGB immediates
    std::vector<ChatBgHeapTarget> heap;         // existing-object recolour targets
    float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // ImGui RGBA picker state
    bool  editing  = false;                     // picker drag in progress
  };

  ChatBgGroup chat_bg_[kChatBgCount];
  bool        chat_bg_found_ = false;           // at least one site resolved

  // Custom main-chat width — the stock chat resizes height only.  The apply
  // mechanism (engine hooks) lives in the ChatTweaks plugin; this owns the
  // setting + UI + persistence and drives it via chat::SetCustomWidth().
  bool chat_width_enabled_ = false;
  int  chat_width_px_      = 800;
  bool chat_timestamps_    = false;  // [HH:MM:SS] prefix on chat lines
  bool chat_item_icons_    = true;   // native item icons on <ITEML> chat links

  // Scans .text once (constructor) and resolves every group's immediates + heap
  // targets, seeding each picker from the colour currently in the binary.
  void FindChatBgSites();

  // Writes argb to all of a group's .text immediates (+ icache flush). When
  // walk_heap is set and the group has heap targets, also recolours live objects.
  void ApplyChatBg(ChatBgGroup& g, uint32_t argb, bool walk_heap);

  // Single heap walk: recolours every live object matching one of g's heap targets.
  void PatchChatBgObjects(const ChatBgGroup& g, uint32_t argb);

  static uint32_t ArgbFromPicker(const float c[4]);
  static void     PickerFromArgb(float c[4], uint32_t argb);

  // Restores all chat-bg groups from bourgeon_settings.yaml in the game
  // directory.  Called after FindChatBgSites so it can immediately apply the
  // saved colours.  (SaveSettings is declared public above.)
  void LoadSettings();

  // Shared colour presets, applicable to any group's picker.
  struct ChatBgPreset { std::string name; uint32_t argb; };
  std::vector<ChatBgPreset> chat_bg_presets_;
  char preset_name_buf_[64] = {};

  // When true, shows a small draggable window listing the colour presets, so the
  // user can quickly switch the MAIN chat colour without opening the picker.
  bool mainchat_preset_bar_ = false;

  // Log verbosity threshold (rAthena-style), persisted so it's discoverable in
  // bourgeon_settings.yaml. Applied via LogConsole::SetLevel on load; also read
  // directly by LogConsole at startup so it takes effect before login.
  // One of: trace, debug, info, warn, error, off.
  std::string log_level_ = "info";

  // Persisted collapse state of the Moonlight-Destiny window.
  // Restored once per login via SetNextWindowCollapsed; saved on every change.
  bool ui_collapsed_     = false;
  bool apply_collapse_   = false;  // set by LoadSettings, consumed on first render
  // Repli demandé par Échap (dernière fenêtre avant le jeu) ; posé par
  // ProcessEscapeStack via RegisterEscapeMinimizeWindow, consommé au rendu suivant.
  bool collapse_requested_ = false;

  // Section sélectionnée dans « Interface de jeu » (membre, et non statique local,
  // pour qu'OpenInterfaceSection puisse la piloter depuis une autre fenêtre).
  int  iface_nav_ = 0;
  // Saut demandé par OpenInterfaceSection : force l'ouverture de l'en-tête et le
  // scroll au prochain rendu, puis se consomme.
  bool iface_jump_ = false;

  // ── Anti-rebond de la sauvegarde (voir SaveSettings) ────────────────────────
  // Délai d'inactivité avant écriture réelle. Assez long pour absorber un drag de
  // slider (60 Hz), assez court pour qu'un réglage suivi d'un alt-F4 immédiat reste
  // l'exception. Un changement de mode force de toute façon un flush.
  static constexpr unsigned kSettingsFlushDelayMs = 400;
  bool     settings_dirty_    = false;  // une écriture est en attente
  unsigned settings_dirty_ms_ = 0;      // GetTickCount() de la DERNIÈRE demande

  // Écrit réellement le fichier. Tout le corps historique de SaveSettings ; n'est
  // plus appelé que par FlushSettings, jamais depuis un widget.
  void WriteSettingsFile();

  // ── Panneaux extraits d'OnRenderUI (dossier plugins/moonlight_ui/) ──────────
  // Déclarés MEMBRES et non fonctions libres : ils manipulent l'état privé de la
  // classe (miroirs de réglages serveur, presets alootid…). Les en faire des
  // fonctions libres aurait imposé de rendre ces membres publics — soit exactement
  // le défaut que le chantier 8 cherche à supprimer. Leur définition vit dans le
  // .cc du dossier, pas ici. Voir plugins/moonlight_ui/internal.h.
  void DrawCommandsPanel();
  void DrawFunPanels();
  void DrawAlootOverlay();
  void InstallItemDescProbe();
};
