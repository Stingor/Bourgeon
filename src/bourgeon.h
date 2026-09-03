#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>

#include "features/plugin.h"
#include "ragnarok/packets.h"  // rag::zc : les opcodes vanilla partages
#include "ragnarok/ragnarok_client.h"

class DiscordRelay;
class DpsMeter;
class TargetFrame;
class BasicInfo;
class MenuIcons;
class StatusIconBar;
class StatusEffects;
class EntityLooks;
class QuestTracker;
class Minimap;
class ItemObtainToast;
class ScreenFx;
class ZoneRecorder;
class MoonlightUi;
class SkillBar;
class ChatTweaks;
class StorageWindow;
class InventoryViewer;
class CartViewer;
class BankWindow;
class ChatRoomWindow;
class GameMenu;
class StaffTools;
class CharDiagnostics;
class HotkeySettings;
class MacroWindow;
class GameSettings;
class CashShopWindow;
class NpcShopWindow;
class VendingWindow;
class WeaponRefineWindow;
class MakeItemWindow;
class CraftAtlas;
class TradeWindow;
class ChatWindow;
class RodexWindow;
class CharacterSheet;
class LoginParade;
class ItemDescWindow;
class MonsterInfoWindow;
class ViewEquipWindow;
class MvpTracker;
class MvpTrackerWindow;
class NavigationWindow;
class PetWindow;
class PartyFriendWindow;
class PartyFrames;
class PaletteEditor;
class EntityContextMenu;
class EntityInspector;
class FpsView;
class AfkScreen;
class ItemDropArc;
class PlayerJump;
class KeyboardMove;
class QuickCast;
class Doom;
class Roggle;
class Rojeweled;
class NpcDialogWindow;
class MoonlightAuth;
class CharSelect;
class BugReport;
class WeaponDualSprites;
class EntityNames;
class ChatBalloon;
class CastBar;

class Bourgeon {
 public:
  // Singleton stuff
  static Bourgeon& Instance() {
    static Bourgeon instance;
    return instance;
  }
  Bourgeon(Bourgeon const&) = delete;
  void operator=(Bourgeon const&) = delete;

  RagnarokClient& client();
  DiscordRelay* discord_relay();
  DpsMeter* dps_meter();
  TargetFrame* target_frame();
  BasicInfo* basic_info();
  MenuIcons* menu_icons();
  StatusIconBar* status_icons();
  StatusEffects* status_effects();
  EntityLooks* entity_looks();
  QuestTracker* quest_tracker();
  Minimap* minimap();
  ItemObtainToast* item_obtain_toast();
  ScreenFx* screen_fx();
  AfkScreen* afk_screen();
  ZoneRecorder* zone_recorder();
  MoonlightUi* moonlight_ui();
  SkillBar* skill_bar();
  ChatTweaks* chat_tweaks();
  StorageWindow* storage_window();
  InventoryViewer* inventory_viewer();
  CartViewer* cart_viewer();
  BankWindow* bank_window();
  ChatRoomWindow* chat_room_window();
  GameMenu* game_menu();
  StaffTools* staff_tools();
  // Fiche technique du personnage (staff). Exposée pour le bouton qui l'ouvre,
  // dans « Staff Tools ».
  CharDiagnostics* char_diagnostics();
  HotkeySettings* hotkey_settings();
  MacroWindow* macro_window();
  GameSettings* game_settings();
  CashShopWindow* cashshop_window();
  NpcShopWindow* npc_shop_window();
  VendingWindow* vending_window();
  WeaponRefineWindow* weapon_refine_window();
  MakeItemWindow* make_item_window();
  CraftAtlas* craft_atlas();
  TradeWindow* trade_window();
  ChatWindow* chat_window();
  RodexWindow* rodex_window();
  NpcDialogWindow* npc_dialog_window();
  // Front de login « compte Moonlight » et scène de sélection de personnage.
  // Consultés par le hook de WndProc : à eux deux ils disent si le clavier
  // appartient à notre UI ou au client natif pendant les écrans de connexion.
  MoonlightAuth* moonlight_auth();
  CharSelect* char_select();
  BugReport* bug_report();
  CharacterSheet* character_sheet();
  LoginParade* login_parade();
  ItemDescWindow* item_desc();
  MonsterInfoWindow* monster_info();
  // « Voir l'équipement » d'un autre joueur : remplace la fenêtre native 139.
  // EntityContextMenu lui donne l'AID de la cible avant de rejouer le code 42 —
  // la réponse du serveur, elle, ne le porte pas.
  ViewEquipWindow* view_equip_window();
  // Carnet de chasse MVP : l'état réseau et sa fenêtre. Le premier est aussi lu
  // par la couche de tombes de la minimap et par le masque UiCaps.
  MvpTracker* mvp_tracker();
  MvpTrackerWindow* mvp_tracker_window();
  // Navigation ImGui. Exposée pour l'action de raccourci `win_navigation`, pour
  // le hook qui route la native 203, et pour les liens de lieu du chat.
  NavigationWindow* navigation_window();
  PetWindow* pet_window();
  // Fenêtre Amis / Groupe ImGui (remplace UIMessengerGroupWnd 0x45). Exposée pour
  // le bouton « party » du menu d'icônes, qui l'ouvre et la referme.
  PartyFriendWindow* party_friend_window();
  // HUD de groupe en grille (raid frames). Exposé pour ses réglages.
  PartyFrames* party_frames();
  // Éditeur de couleurs du personnage (Alt+P). Exposé pour que la feuille de
  // perso puisse l'ouvrir : c'est là que le joueur regarde son apparence.
  PaletteEditor* palette_editor();
  EntityContextMenu* entity_context_menu();
  EntityInspector* entity_inspector();
  FpsView* fps_view();
  // Jaillissement des objets lâchés au sol. Exposé pour ses réglages.
  ItemDropArc* item_drop_arc();
  PlayerJump* player_jump();
  KeyboardMove* keyboard_move();
  QuickCast* quick_cast();
  Doom* doom();
  Roggle* roggle();
  Rojeweled* rojeweled();
  WeaponDualSprites* weapon_dual_sprites();
  EntityNames* entity_names();
  ChatBalloon* chat_balloon();
  CastBar* cast_bar();

  bool Initialize();

  // Vrai seulement sur le client dont Bourgeon connait les adresses en dur.
  // Le projet en appelle 395 hors configuration, toutes propres au 20250716 :
  // sur un autre client elles designent autre chose, et les poser revient a
  // ecrire au hasard dans son code. Interroge par les boucles de rendu et de
  // tick, qui sont appelees par des hooks installes des DllMain -- donc hors de
  // toute verification de version.
  bool native_features() const { return native_features_; }
  void OnTick();
  void OnProcessInput();  // dispatch de commandes natives, sur ÉVÉNEMENT (cf. .cc)

  // Battement par FRAME, hors de toute frame ImGui (hook OnUpdate du mode, avant
  // que le jeu ne dessine). C'est la seule horloge qui soit à la fois régulière
  // et sûre pour rejouer une commande native : OnTick est bridé à ~100 ms, et
  // OnProcessInput n'est qu'événementiel (CMode::SendMsg) sur ce client.
  // Réservé à ce qui a besoin d'une cadence FINE — le reste va dans OnTick.
  void OnGameFrame();

  // 🔴 Rejoue les paquets mis en file par le fil réseau, sur le fil PRINCIPAL, pour
  // TOUS les modules (cf. features/net_inbox.h). Appelé à CHAQUE frame depuis les
  // hooks OnUpdate des modes (jeu ET login/char-select), et non pas seulement
  // depuis OnProcessInput : sur le client 20250716, l'adresse « ProcessInput » est
  // en réalité CMode::SendMsg, un dispatcher ÉVÉNEMENTIEL, pas une phase de frame.
  // Un joueur qui n'agissait que dans une fenêtre ImGui n'en déclenchait aucun, et
  // ses paquets dormaient dans la file — le dialogue NPC n'avançait plus tant qu'on
  // ne cliquait pas HORS de la fenêtre.
  void DrainNetInboxes();

  // Relayé depuis le hook de CMode::SendMsg à chaque lancement de compétence
  // (commandes 0x45 / 0x71, p1 = identifiant). Observation pure : rien n'est
  // modifié. Les listes de fabrication en dépendent — la compétence qui les
  // ouvre n'est dans aucun paquet (docs/make_item_list_re.md §2.2).
  void NotifySkillCast(int skill_id, int skill_lv);
  // Relayé depuis RagConnection::SendPacketHook à chaque CZ_USE_ITEM (0x0439).
  // Reçoit l'INDEX d'inventaire — le seul champ du paquet — et le résout en
  // identifiant d'objet ici, tant que l'objet existe encore : il est sur le point
  // d'être consommé par le serveur. Sert aux listes de fabrication ouvertes par
  // un script d'objet (Mini Furnace, marteaux).
  void NotifyItemUse(unsigned item_index);
  // Relayé depuis le hook de CMode::SendMsg APRÈS chaque commande 0x48 « entrer
  // en mode ciblage » (curseur de visée). `cmode` = l'objet receveur (CGameMode
  // en jeu), porteur de l'état de ciblage à +0x408. Consommé par QuickCast.
  void NotifySkillTargeting(void* cmode);
  void AddLogLine(std::string log_line);
  // Fenêtre de logs en jeu — RÉSERVÉE AU STAFF (IsStaff(), group level >= 80).
  // Elle expose tout ce que le client journalise ; ce n'est pas une information
  // à mettre entre toutes les mains, et c'est aussi la surface qui remplace la
  // console Windows (que le joueur ne veut pas voir s'ouvrir).
  //
  // PERSISTÉ par MoonlightUi sous la clé « staff_log_window ». Le champ vit ici
  // et non dans un plugin : la table de réglages y accède par un résolveur écrit
  // à la main, puisque Bourgeon::Instance() ne peut pas être nul. Persister est
  // sans risque, l'affichage restant gaté par IsStaff() à chaque frame.
  bool& show_log_window() { return show_log_window_; }
  void RenderUI();

  // Plugin event dispatch, called from the game hooks.
  void FireModeSwitch(ModeMgr::ModeType mode_type, const char* map_name);
  void FireTalkType(const char* chat_buffer);
  void FireChatMessage(const char* chat_buffer);

  // Aiguille une ligne écrite par Bourgeon (UIM_PUSHINTOCHATHISTORY) vers la
  // chatbox ImGui quand elle a remplacé la native. true = prise en charge, et
  // l'appelant ne doit alors PAS la passer au natif (cf. chatwnd::IngestPluginLine).
  bool RouteChatLine(const char* text, uint32_t rgb);
  void FireKeyDown(unsigned long vkey, int new_key, int accurate_key);
  void FireRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len);

  // Packet helpers for plugins.
  // SendPacket: raw send — caller builds the full packet including any header.
  bool SendPacket(const uint8_t* buf, size_t len);
  // RegisterRecvOpcode: installs a dispatch-table hook so the given server
  // opcode is forwarded to OnRecvPacket instead of being dropped as unknown.
  void RegisterRecvOpcode(uint16_t opcode);

  // RegisterObserveOpcode: forwards a *standard* client packet to OnRecvPacket
  // without replacing its dispatch handler.  `forward_len` bytes after the
  // 2-byte opcode are passed as `data`.  Use for reading fields off packets the
  // client already handles (e.g. mapname from 0x0091 ZC_NPCACK_MAPMOVE).
  void RegisterObserveOpcode(uint16_t opcode, uint16_t forward_len);

  // ── « Le joueur change de carte » ──────────────────────────────────────
  // 🔴 Ce TEST était écrit QUATRE fois : ici (le hook de lecture, qui arme
  // `SetMapLoading`) et dans les trois fenêtres que le serveur ferme en silence
  // quand on quitte la carte — cash shop, boutique NPC, entrepôt. La copie
  // canonique, celle de `rag_connection`, l'écrivait en LITTÉRAUX NUS : aucun
  // relevé de `constexpr` ne la voyait.
  //
  // Les deux OPCODES, eux, vivent chez `rag::zc` (ragnarok/packets.h) : ils
  // étaient déclarés ici, dans MoonlightUi et dans npc_dialog_window, sous trois
  // noms pour la même valeur. C'est là aussi qu'on lira pourquoi il ne faut pas
  // les confondre avec `MapLoadEpoch()`.
  static bool IsWarpPacket(uint16_t opcode) {
    return opcode == rag::zc::kMapChange || opcode == rag::zc::kServerMove;
  }
  // Observe les deux, avec la longueur que les trois appelants demandaient déjà.
  void ObserveWarpPackets();

  // ── « La compétence a échoué » ─────────────────────────────────────────
  // ZC_ACK_TOUSESKILL, que les deux fenêtres de fabrication observent pour
  // savoir qu'une demande a été refusée. Même patron que ci-dessus : elles
  // écrivaient les deux mêmes lignes, avec la même longueur.
  static constexpr uint16_t kOpSkillFail = 0x0110;
  void ObserveSkillFail();

  // CZ_CONFIG, ÉMIS (pas observé) — la feuille de personnage et le familier
  // s'en servent tous deux pour pousser un réglage au serveur.
  static constexpr uint16_t kOpConfig = 0x02d8;

  // RegisterReplaceOpcode: prend la place du handler NATIF d'un paquet standard,
  // de façon révocable — `claim` est interrogé à chaque paquet et un « non » rend
  // la main au handler d'origine, à l'octet près. Détails et garde-fous dans
  // RagConnection::RegisterReplaceOpcode.
  //
  // Sert à empêcher une fenêtre native de NAÎTRE, au lieu de la masquer puis de la
  // détruire après coup : une native masquée reste vivante et garde le clavier
  // (Entrée/Espace valident son bouton par défaut, cf.
  // docs/make_item_list_re.md §12.5).
  //
  // La surcharge (data, len) est pour les opcodes MULTIPLEXÉS, dont un champ dit
  // à quoi le paquet sert — ZC_INVENTORY_START (0x0b08) ouvre l'inventaire, le
  // cart ou le storage selon son invType. Revendiquer l'opcode entier tuerait
  // deux fenêtres pour en remplacer une.
  void RegisterReplaceOpcode(uint16_t opcode, std::function<bool()> claim);
  void RegisterReplaceOpcode(
      uint16_t opcode, std::function<bool(const uint8_t* data, uint16_t len)> claim);

  // Map-loading gate. True from the ZC_NPCACK_MAPMOVE (0x0091) that begins a
  // warp/@load until the CZ_NOTIFY_ACTORINIT (0x007d) the client sends once the
  // new map is ready. During this window the native HUD is being torn down and
  // rebuilt (CGameMode::EnterWorld), so acting on it is unsafe — that race is
  // what freed a UIShortCutWnd while it was still in the native window-snap
  // manager and produced a use-after-free. While loading we stand down: hide the
  // plugin UI (which also stops SkillBar from MakeWindow'ing the shortcut
  // bar every frame) and swallow keyboard input.
  bool IsMapLoading() const;
  void SetMapLoading(bool loading);
  // Nombre de transitions de carte depuis le lancement (warp, @load, changement
  // de serveur). Il ne sert pas à savoir SI l'on charge — `IsMapLoading` le dit —
  // mais à constater qu'un chargement a EU LIEU depuis la dernière fois qu'on a
  // regardé : c'est ce que veulent les écrans qui doivent se refermer avec le HUD
  // natif (menu Échap, Game Settings, table des raccourcis), et que le seul état
  // booléen leur ferait manquer sur un chargement plus court que leur battement.
  uint32_t MapLoadEpoch() const { return map_load_epoch_.load(); }

  // 🔴 Une capture de touche est en cours dans l'interface : la frappe sert à
  // REMAPPER, elle ne doit RIEN déclencher — ni chez nous, ni chez le client.
  // C'est le devoir que la fenêtre native remplissait toute seule : tant qu'elle
  // vit, `UIWindowMgr_OnKeyDown` (0x00A47201) détourne et consomme tout le
  // clavier. Nous la détruisons, donc ce devoir nous revient, et le seul endroit
  // d'où on puisse encore couper le dispatch du JEU est le hook de
  // `ProcessPushButton` — après, la commande est déjà partie. Sans lui, choisir
  // Alt+D pour une action ouvrait AUSSI la tipbox du client au passage.
  //
  // Vit ici parce que `ragnarok/` ne connaît pas les features et n'a pas à les
  // connaître : Bourgeon tient les deux bouts, comme pour `RouteChatLine`.
  bool IsHotkeyCaptureActive() const;

  // Une action de Bourgeon vient de prendre la frappe diffusée par `FireKeyDown` :
  // le hook clavier doit la CONFISQUER au lieu de la passer au handler natif.
  // Relève le drapeau ET le remet à zéro — un seul lecteur, juste après la
  // diffusion. Voir `hotkeys::ClaimKey`.
  bool TakeHotkeyActionClaim();

  // Game-world gate. RenderUI() draws plugin ImGui windows only while the game
  // world is the actively-updating mode (CGameMode). At the login and
  // character-select screens CGameMode::OnUpdate does not run, so no plugin
  // window can linger there. NotifyGameUpdate() is the per-frame heartbeat fired
  // from GameMode::OnUpdateHook; IsGameActive() reports whether that heartbeat is
  // fresh. FireModeSwitch(non-kGame) also clears it for an instant hide — the
  // heartbeat staleness is the fallback for the char-change case, where the
  // client does not reliably re-fire a game->login mode switch (cf. the same
  // note in integrity_check.cc).
  void NotifyGameUpdate();
  bool IsGameActive() const;

 private:
  Bourgeon();

  void LoadPlugins();
  void ShowLogWindow();

  std::vector<std::unique_ptr<Plugin>> plugins_;
  DiscordRelay* discord_relay_ = nullptr;  // non-owning, lifetime tied to plugins_
  DpsMeter*     dps_meter_     = nullptr;  // non-owning, lifetime tied to plugins_
  TargetFrame*  target_frame_  = nullptr;  // non-owning, lifetime tied to plugins_
  BasicInfo* basic_info_ = nullptr;  // non-owning, lifetime tied to plugins_
  MenuIcons* menu_icons_  = nullptr;  // non-owning, lifetime tied to plugins_
  StatusIconBar* status_icons_ = nullptr;  // non-owning, lifetime tied to plugins_
  StatusEffects* status_effects_ = nullptr;  // idem
  EntityLooks*   entity_looks_   = nullptr;  // idem
  QuestTracker* quest_tracker_ = nullptr;  // non-owning, lifetime tied to plugins_
  Minimap* minimap_ = nullptr;  // non-owning, lifetime tied to plugins_
  ItemObtainToast* item_obtain_toast_ = nullptr;  // non-owning, lifetime tied to plugins_
  ScreenFx* screen_fx_ = nullptr; // non-owning, lifetime tied to plugins_
  ZoneRecorder* zone_recorder_ = nullptr;  // non-owning, lifetime tied to plugins_
  MoonlightUi* moonlight_ui_ = nullptr;       // non-owning, lifetime tied to plugins_
  SkillBar* skill_bar_ = nullptr;       // non-owning, lifetime tied to plugins_
  ChatTweaks* chat_tweaks_ = nullptr;         // non-owning, lifetime tied to plugins_
  StorageWindow* storage_window_ = nullptr;   // non-owning, lifetime tied to plugins_
  InventoryViewer* inventory_viewer_ = nullptr;  // non-owning, lifetime tied to plugins_
  CartViewer* cart_viewer_ = nullptr;            // non-owning, lifetime tied to plugins_
  BankWindow* bank_window_ = nullptr;          // non-owning, lifetime tied to plugins_
  ChatRoomWindow* chat_room_window_ = nullptr; // non-owning, lifetime tied to plugins_
  GameMenu* game_menu_ = nullptr;              // non-owning, lifetime tied to plugins_
  StaffTools* staff_tools_ = nullptr;          // non-owning, lifetime tied to plugins_
  CharDiagnostics* char_diagnostics_ = nullptr;  // idem
  HotkeySettings* hotkey_settings_ = nullptr;  // non-owning, lifetime tied to plugins_
  MacroWindow* macro_window_ = nullptr;        // non-owning, lifetime tied to plugins_
  GameSettings* game_settings_ = nullptr;      // non-owning, lifetime tied to plugins_
  CashShopWindow* cashshop_window_ = nullptr;  // non-owning, lifetime tied to plugins_
  NpcShopWindow* npc_shop_window_ = nullptr;          // non-owning, lifetime tied to plugins_
  VendingWindow* vending_window_ = nullptr;    // non-owning, lifetime tied to plugins_
  WeaponRefineWindow* weapon_refine_window_ = nullptr;  // non-owning, lifetime tied to plugins_
  MakeItemWindow* make_item_window_ = nullptr;  // non-owning, lifetime tied to plugins_
  CraftAtlas* craft_atlas_ = nullptr;            // non-owning, lifetime tied to plugins_
  TradeWindow* trade_window_ = nullptr;        // non-owning, lifetime tied to plugins_
  ChatWindow* chat_window_ = nullptr;          // non-owning, lifetime tied to plugins_
  RodexWindow* rodex_window_ = nullptr;        // non-owning, lifetime tied to plugins_
  NpcDialogWindow* npc_dialog_window_ = nullptr;  // non-owning, lifetime tied to plugins_
  MoonlightAuth* moonlight_auth_ = nullptr;    // non-owning, lifetime tied to plugins_
  CharSelect* char_select_ = nullptr;          // non-owning, lifetime tied to plugins_
  BugReport* bug_report_ = nullptr;  // non-owning, lifetime tied to plugins_
  CharacterSheet* character_sheet_ = nullptr;  // non-owning, lifetime tied to plugins_
  LoginParade* login_parade_ = nullptr;        // non-owning, lifetime tied to plugins_
  FpsView* fps_view_ = nullptr;         // non-owning, lifetime tied to plugins_
  AfkScreen* afk_screen_ = nullptr;     // non-owning, lifetime tied to plugins_
  ItemDropArc* item_drop_arc_ = nullptr;  // non-owning, lifetime tied to plugins_
  PlayerJump* player_jump_ = nullptr;   // non-owning, lifetime tied to plugins_
  KeyboardMove* keyboard_move_ = nullptr;  // non-owning, lifetime tied to plugins_
  QuickCast* quick_cast_ = nullptr;     // non-owning, lifetime tied to plugins_
  Doom* doom_ = nullptr;                // non-owning, lifetime tied to plugins_
  Roggle* roggle_ = nullptr;            // non-owning, lifetime tied to plugins_
  Rojeweled* rojeweled_ = nullptr;      // non-owning, lifetime tied to plugins_
  ItemDescWindow* item_desc_ = nullptr;       // non-owning, lifetime tied to plugins_
  MonsterInfoWindow* monster_info_ = nullptr;  // non-owning, lifetime tied to plugins_
  ViewEquipWindow* view_equip_window_ = nullptr;  // non-owning, lifetime tied to plugins_
  MvpTracker* mvp_tracker_ = nullptr;               // non-owning, idem
  MvpTrackerWindow* mvp_tracker_window_ = nullptr;  // non-owning, idem
  NavigationWindow* navigation_window_ = nullptr;  // non-owning, idem
  PetWindow* pet_window_ = nullptr;            // non-owning, lifetime tied to plugins_
  PartyFriendWindow* party_friend_window_ = nullptr;  // idem
  PartyFrames* party_frames_ = nullptr;               // idem
  PaletteEditor* palette_editor_ = nullptr;    // idem
  EntityContextMenu* entity_context_menu_ = nullptr;  // idem
  EntityInspector* entity_inspector_ = nullptr;       // idem
  WeaponDualSprites* weapon_dual_sprites_ = nullptr;  // non-owning, lifetime tied to plugins_
  EntityNames* entity_names_ = nullptr;  // non-owning, lifetime tied to plugins_
  ChatBalloon* chat_balloon_ = nullptr;  // non-owning, lifetime tied to plugins_
  CastBar* cast_bar_ = nullptr;          // non-owning, lifetime tied to plugins_
  uint32_t last_tick_count_;
  // Garde de ré-entrance de DrainNetInboxes : un HandlePacket peut émettre une
  // commande native, laquelle repasse par CMode::SendMsg -> OnProcessInput. Sans
  // cette garde, le Drain imbriqué viderait les tampons de sortie que la boucle
  // appelante est en train de parcourir (usage après libération).
  bool draining_inboxes_ = false;
  bool native_features_ = false;
  std::atomic<bool> map_loading_{false};
  std::atomic<uint32_t> map_loading_since_ms_{0};  // GetTickCount at load start
  std::atomic<uint32_t> map_load_epoch_{0};        // transitions de carte (fronts)
  std::atomic<uint32_t> last_game_update_ms_{0};   // GetTickCount of last CGameMode update (0 = never)
  std::vector<std::string> log_lines_;
  bool show_log_window_ = false;
  RagnarokClient client_;
};
