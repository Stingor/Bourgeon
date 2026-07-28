#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "imgui.h"
#include "features/systems/bourgeon_opcodes.h"
#include "features/plugin.h"

// Le toolkit UI générique et le gate staff vivaient ici ; ils sont désormais dans
// ui/ro_widgets.h, ui/align_grid.h et features/staff_gate.h. Ils restent inclus
// depuis ce header pour que les fichiers qui n'en utilisaient qu'une pièce
// continuent de compiler — mais un NOUVEAU code doit inclure directement l'en-tête
// dont il a besoin, et non ce header de plugin.
#include "features/staff_gate.h"
#include "ui/align_grid.h"
#include "ui/ro_widgets.h"

// « Tout-ImGui ou tout-natif » — SOURCE UNIQUE du groupe « Interface moderne ».
// Les fenêtres qui s'activent ENSEMBLE (plus de mixe possible) :
//   inventaire (InventoryViewer::imgui_enabled_, sertissage de cartes inclus),
//   cart (CartViewer::imgui_enabled_), storage (StorageWindow::imgui_enabled_),
//   barres d'action (SkillBarTweaks::enabled_), échange (TradeWindow::imgui_enabled_),
//   courrier RODEX (RodexWindow::imgui_enabled_), échoppe joueur — vente ET
//   échoppe d'achat (VendingWindow::imgui_enabled_), feuille de personnage
//   (CharacterSheet::imgui_enabled_), cash shop (CashShopWindow::imgui_enabled_) et
//   boutique PNJ (NpcShopWindow::imgui_enabled_).
// C'est la définition du corps de la fonction qui fait foi : les panneaux qui
// portent la case s'y RÉFÈRENT au lieu de recopier la liste (elle a déjà rouillé
// une fois — le libellé annonçait trois fenêtres sur six).
// Appelée par chacune des cases synchronisées et par LoadSettings (réconciliation
// d'un yaml hérité mixé). Encapsule l'accès aux plugins pour qu'un panneau bascule
// l'ensemble sans inclure les headers des autres. Sûre si un plugin manque.
void SetModernInterface(bool on);

// Dessine la case « Interface moderne » (skin RO) suivie de son point d'aide, et
// applique déjà SetModernInterface() au clic. Renvoie true si l'état a changé —
// au panneau appelant de persister.
// `window_help` décrit ce que la bascule change POUR CETTE fenêtre-là ; la LISTE du
// groupe, elle, est écrite une seule fois dans le corps de cette fonction. Les
// quatre panneaux porteurs la recopiaient chacun dans leur infobulle, et les trois
// derniers membres (échoppe, feuille de perso, boutiques) n'y figuraient nulle part.
bool DrawModernInterfaceCheckbox(bool* enabled, const char* window_help);

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
  // (ItemDescWindow) réintègre le bouton +/- alootid. ─────────────────────────
  bool IsAlootId(uint32_t id) const;   // l'item est-il dans la liste ?
  bool AddAlootId(uint32_t id);        // ajoute + notifie serveur (false si plein/déjà)
  bool RemoveAlootId(uint32_t id);     // retire + notifie serveur (false si absent)
  const char* ItemName(uint32_t id) const;  // nom via itemInfoMerged.lua (ou nullptr)

  // ── Tri serveur (e_sort_mode 0-6) ───────────────────────────────────────────
  // Les combos « Tri … » de « Commands Settings » pilotent un réglage SERVEUR
  // (settings 19-22 -> sort_inventory_items / sort_storage_items côté moonlight),
  // pas un tri de vue. Ils sont exposés ici pour que les panneaux des fenêtres
  // concernées (inventaire/cart, storage/guilde) affichent LE MÊME combo :
  // même état, même envoi, aucune copie locale à resynchroniser.
  enum SortTarget { kSortInventory = 0, kSortCart, kSortStorage, kSortGuildStorage };
  // Dessine le combo (skin RO) + son point d'aide, et envoie le réglage au serveur
  // si la sélection change. Renvoie true dans ce cas.
  bool DrawSortModeCombo(SortTarget target);

  // ── Accès direct à une section de config ────────────────────────────────────
  // Sections de l'en-tête « Interface de jeu ». Les LIBELLÉS vivent dans la table
  // kIfaceSections du .cc, où chaque ligne porte explicitement son identifiant :
  // il n'y a donc plus deux listes à garder alignées, et insérer une section ne
  // peut plus décaler silencieusement les suivantes. kIfaceCount ferme l'enum et
  // sert de borne (OpenInterfaceSection) et de contrôle de taille à la compilation.
  enum IfaceSection {
    kIfaceSkillBar = 0, kIfaceBasicInfo, kIfaceChat, kIfaceMenuIcons,
    kIfaceStatusIcons, kIfaceQuest, kIfaceDesc, kIfaceSkin, kIfaceNpc,
    kIfaceStorage, kIfaceInventory, kIfaceCart, kIfaceBank,
    kIfaceCount,
  };
  // Ouvre le panneau Moonlight directement sur `section` : déplie la fenêtre,
  // ouvre l'en-tête « Interface de jeu », sélectionne l'entrée de nav et scrolle
  // dessus. Appelé par le bullet de barre de titre des fenêtres Bourgeon
  // (cf. ro::SetNextWindowTitleBullet) pour joindre LEUR config en un clic.
  // Prend effet au rendu suivant (sûr à appeler depuis n'importe quel OnRenderUI).
  void OpenInterfaceSection(int section);

 private:
  // Les réglages qui appartiennent à MoonlightUi elle-même (et non à un plugin)
  // sont décrits, comme tous les autres, par des tables de descripteurs — mais
  // un descripteur pointe l'ADRESSE du champ, or ceux-ci sont privés. Cette
  // struct-amie ne porte QUE ces tables (définies dans moonlight_ui.cc) : c'est
  // le strict minimum d'ouverture, à comparer aux membres qu'il aurait fallu
  // rendre publics. Elle disparaîtra à l'étape C, quand chaque plugin portera
  // son propre LoadConfig/SaveConfig.
  friend struct MoonlightUiOwnSettings;

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
  static constexpr uint16_t kSettingShowExp              = 0;
  static constexpr uint16_t kSettingShowZeny             = 1;
  static constexpr uint16_t kSettingShowMobInfo          = 2;
  static constexpr uint16_t kSettingSeparateKills        = 3;
  static constexpr uint16_t kSettingBlockExp             = 4;
  static constexpr uint16_t kSettingAlootRare            = 5;
  static constexpr uint16_t kSettingAlootRate            = 6;   // autoloot rate 0-100
  static constexpr uint16_t kSettingAlootMinZenyDiv100   = 7;   // seuil zeny, /100 sur le fil
  static constexpr uint16_t kSettingAlootType            = 8;   // autoloottype toggle
  static constexpr uint16_t kSettingDiscordChat          = 9;   // discord relay opt-in
  static constexpr uint16_t kSettingShowAttackDelay      = 10;
  static constexpr uint16_t kSettingShowMoveSpeed        = 11;
  static constexpr uint16_t kSettingSellStuff            = 12;
  static constexpr uint16_t kSettingSellItem             = 13;
  static constexpr uint16_t kSettingNoAsk                = 14;
  static constexpr uint16_t kSettingNoksMode             = 15;  // 0=off 1=self 2=party 3=guild
  static constexpr uint16_t kSettingWings                = 16;
  static constexpr uint16_t kSettingAlootMvp             = 17;
  static constexpr uint16_t kSettingAlootMvpRwd          = 18;
  static constexpr uint16_t kSettingSortModeInventory    = 19;  // e_sort_mode 0-6
  static constexpr uint16_t kSettingSortModeCart         = 20;
  static constexpr uint16_t kSettingSortModeStorage      = 21;
  static constexpr uint16_t kSettingSortModeGuildStorage = 22;
  static constexpr uint16_t kSettingAlootId       = 23;  // add item ID to autolootid list (0=clear)
  static constexpr uint16_t kSettingAlootIdRemove = 24;  // remove item ID from list
  // ⚠ Ce n'est PAS un booléen « est staff » : la valeur est le NIVEAU de groupe
  // serveur (pc_get_group_level, 0 pour un joueur). Le seuil staff est appliqué
  // ailleurs, par IsStaff() (staff_gate.h). Un `if (kSettingGroupLevel)` écrit de
  // bonne foi serait donc toujours vrai — d'où le nom, qui dit la valeur.
  static constexpr uint16_t kSettingGroupLevel    = 26;  // lecture seule

  // Updates both directions of the relay based on current state.
  void UpdateRelay();

  // PRÉFIXE de nom de carte, comparé au strncmp : « gonryun_in » matche aussi.
  static constexpr char kDiscordRelayMapPrefix[] = "gonryun";  // sizeof - 1 = 7

  bool in_game_ = false;
  // « Je suis sur la carte qui active le relais Discord » — et non « je suis à
  // Gonryun » : la carte vient de kDiscordRelayMapPrefix, plus du dur.
  bool on_discord_relay_map_ = false;

  // Server-synced (ID 9); restored from globalreg on login.
  bool discord_chat_ = false;

  // Miroirs locaux des bascules SERVEUR (ids ci-dessus). Ce sont des commandes
  // rAthena au nom indevinable depuis le client, d'où les noms développés — et
  // deux d'entre elles ne sont PAS des booléens malgré leur ancien nom.
  bool show_exp_      = false;
  bool show_zeny_     = false;
  bool show_mob_info_ = false;
  bool separate_kills_enabled_ = false;
  bool block_exp_     = false;
  bool aloot_rare_    = false;
  int  aloot_rate_    = 0;    // 0-100 (%)
  int  aloot_min_zeny_  = 0;  // 0-1 000 000 (z) en local ; /100 sur le fil
  int  aloot_type_mask_ = 0;  // bitmask uint16 : bit i = (1 << item_type i)

  bool show_attack_delay_enabled_ = false;
  bool show_move_speed_enabled_   = false;
  bool sell_stuff_enabled_        = false;
  bool sell_item_enabled_         = false;
  bool no_ask_enabled_            = false;
  int  noks_mode_                 = 0;  // ⚠ énuméré : 0=off 1=self 2=party 3=guild
  bool wings_enabled_             = false;
  bool aloot_mvp_       = false;
  bool aloot_mvp_rwd_   = false;
  int  sort_mode_inventory_    = 0;  // ⚠ énumérés : e_sort_mode 0-6, pas des booléens
  int  sort_mode_cart_         = 0;
  int  sort_mode_storage_      = 0;
  int  sort_mode_guild_storage_ = 0;

  std::vector<uint32_t> aloot_ids_;        // client-tracked autolootid list (max 50)
  std::vector<uint32_t> alootid_saved_ids_; // snapshot at last preset load/save
  int                   aloot_id_input_ = 0;

  // `slot_no` (et non `no`, qui se lit comme une négation) : NUMÉRO d'emplacement
  // de preset côté serveur, 0 = aucun.
  struct AlootPreset { uint8_t slot_no; std::string name; bool autoload; };
  std::vector<AlootPreset> alootid_presets_;   // received from server (ZC 0x0F07)
  uint8_t alootid_active_preset_   = 0;        // currently loaded preset no (0=none)
  uint8_t alootid_selected_preset_ = 0;        // selected in combo (may differ from active)
  char    alootid_preset_input_[64] = {};       // name field for save
  char    alootid_rename_input_[64] = {};       // name field for rename (cmd 6)
  bool    alootid_rename_open_      = false;    // checkbox: show rename field

  // Opération demandée au serveur sur un preset autolootid (CZ 0x0F06, octet de
  // commande). Les valeurs sont celles qu'attend clif_parse_bourgeon_preset_cmd :
  // ne pas les renuméroter. Un enum plutôt que des littéraux nus parce que
  // `SendPresetCmd(4, …)` supprimait un preset sans que rien ne le dise.
  enum AlootPresetCmd : uint8_t {
    kAlootPresetSave     = 2,
    kAlootPresetLoad     = 3,
    kAlootPresetDelete   = 4,
    kAlootPresetAutoload = 5,  // preset_no 0 = désactiver le chargement auto
    kAlootPresetRename   = 6,
  };
  // Sends a preset management command to the server (CZ 0x0F06).
  void SendPresetCmd(AlootPresetCmd command, uint8_t preset_no = 0,
                     const char* preset_name = nullptr);
  bool                  show_alootid_overlay_ = false;  // floating Add/Remove overlay on right-click

  // Address of the item description window message handler (FUN_008c18b0,
  // 20250716 client).  We hook it to capture the nameid of right-clicked items.
  static constexpr uintptr_t kItemDescWndAddr = 0x008c18b0;
  // (Le pendant SKILL, UIItemTooltipWnd_OnMsg 0x008ca900 pour le message 0x2e, a
  //  été retiré avec son hook — cf. le chemin enrichi d'ItemDescWindow. L'adresse
  //  reste notée ici, en commentaire, pour qui voudrait le refaire.)
  // [edi+0x218] in the game's UI manager object (edi=0x0131F4E8): pointer to
  // the active item description window, 0 when no tooltip is open.  Written by
  // the game independently of our hook, so polling it catches silent closes
  // (e.g. comparison→non-comparison transition).
  static constexpr uintptr_t kItemDescWndGlobalPtr = 0x0131F700;

  std::unordered_map<uint32_t, std::string> item_names_;  // ID → Name from itemInfoMerged.lua
  void LoadItemNames();

  // ── Chat ─────────────────────────────────────────────────────────────────
  // Tout ce qui touche à la fenêtre de chat — largeur, horodatage, icônes
  // d'objets, et les COULEURS DE FOND (patch d'immédiats .text + parcours du tas)
  // — appartient au plugin ChatTweaks (features/patches/chat.h, features/patches/chat_bg.cc).
  // MoonlightUi n'en garde que la persistance et l'endroit où le panneau
  // s'affiche. Le patch mémoire a longtemps vécu ici : c'est le panneau de
  // réglages qui l'y avait attiré, pas une raison technique.
  //
  // (Les conversions de couleur vivent dans ui/color_codec.h : ro::ArgbFromPicker,
  //  ro::PickerFromArgb, ro::ImVec4FromArgb. Elles étaient membres statiques ici
  //  alors qu'elles ne touchent aucun état — et ce voisinage rendait invisible le
  //  second encodage, ImU32, qui coexiste dans le même yaml.)

  // Relit bourgeon_settings.yaml (section moonlight_ui) et distribue les réglages
  // à leurs propriétaires. (SaveSettings est déclarée publique plus haut.)
  void LoadSettings();

  // Effets de bord d'un chargement : bornage des valeurs, et poussée vers les
  // couches qui en gardent une copie (chat natif, journal, d3d9, icônes de
  // statut). Appelée en dernier par LoadSettings, une fois TOUT lu.
  void PostLoadApply();

  // Affiche une petite fenêtre déplaçable listant les préréglages de couleur,
  // pour changer le fond du chat PRINCIPAL sans ouvrir le sélecteur. Les
  // préréglages appartiennent à ChatTweaks ; ce drapeau-ci décide seulement si la
  // barre est à l'écran, et c'est bien un réglage du panneau Moonlight.
  bool mainchat_preset_bar_ = false;

  // Log verbosity threshold (rAthena-style), persisted so it's discoverable in
  // bourgeon_settings.yaml. Applied via LogConsole::SetLevel on load; also read
  // directly by LogConsole at startup so it takes effect before login.
  // One of: trace, debug, info, warn, error, off.
  std::string log_level_ = "info";

  // Persisted collapse state of the Moonlight-Destiny window.
  // Restored once per login via SetNextWindowCollapsed; saved on every change.
  bool ui_collapsed_     = false;
  bool pending_collapse_restore_   = false;  // set by LoadSettings, consumed on first render
  // Repli demandé par Échap (dernière fenêtre avant le jeu) ; posé par
  // ProcessEscapeStack via RegisterEscapeMinimizeWindow, consommé au rendu suivant.
  bool pending_collapse_request_ = false;

  // Section sélectionnée dans « Interface de jeu » (membre, et non statique local,
  // pour qu'OpenInterfaceSection puisse la piloter depuis une autre fenêtre).
  int  iface_nav_ = 0;
  // Saut demandé par OpenInterfaceSection : force l'ouverture de l'en-tête et le
  // scroll au prochain rendu, puis se consomme.
  bool pending_iface_jump_ = false;

  // ── Anti-rebond de la sauvegarde (voir SaveSettings) ────────────────────────
  // Délai d'inactivité avant écriture réelle. Assez long pour absorber un drag de
  // slider (60 Hz), assez court pour qu'un réglage suivi d'un alt-F4 immédiat reste
  // l'exception. Un changement de mode force de toute façon un flush.
  static constexpr unsigned kSettingsFlushDelayMs = 400;
  bool     settings_dirty_    = false;  // une écriture est en attente
  unsigned settings_dirty_ms_ = 0;      // GetTickCount() de la DERNIÈRE demande

  // Vrai quand la dernière lecture du yaml s'est interrompue en cours de route.
  // L'état en mémoire ne reflète alors qu'une PARTIE du fichier — le reste est
  // aux défauts — et WriteSettingsFile refuse d'écrire tant qu'il est posé :
  // réémettre depuis cet état rendrait la perte définitive. Remis à false par une
  // lecture menée à son terme (LoadSettings, à chaque changement de carte).
  bool settings_load_failed_ = false;

  // Écrit réellement le fichier. Tout le corps historique de SaveSettings ; n'est
  // plus appelé que par FlushSettings, jamais depuis un widget.
  void WriteSettingsFile();

  // ── Panneaux extraits d'OnRenderUI (dossier features/moonlight_ui/) ──────────
  // Déclarés MEMBRES et non fonctions libres : ils manipulent l'état privé de la
  // classe (miroirs de réglages serveur, presets alootid…). Les en faire des
  // fonctions libres aurait imposé de rendre ces membres publics — soit exactement
  // le défaut que le chantier 8 cherche à supprimer. Leur définition vit dans le
  // .cc du dossier, pas ici. Voir features/moonlight_ui/internal.h.
  void DrawCommandsPanel();
  void DrawFunPanels();
  void DrawAlootOverlay();
  void DrawInterfacePanel();
  void InstallItemDescProbe();
};
