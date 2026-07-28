#include "bourgeon.h"

#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include "imgui.h"
#include "ragnarok/skill_cooldowns.h"
#include "ui/ro_imgui.h"
#include "ui/window_clamp.h"
#include "plugins/auto_login.h"
#include "plugins/moonlight_auth.h"
#include "plugins/char_select.h"
#include "plugins/chat.h"
#include "plugins/cheat_detector.h"
#include "plugins/discord_relay.h"
#include "plugins/basic_info.h"
#include "plugins/dps_meter.h"
#include "plugins/menu_icons.h"
#include "plugins/integrity_check.h"
#include "plugins/dx7_warning.h"
#include "plugins/moonlight_ui.h"
#include "plugins/status_tweaks.h"
#include "plugins/berserk_chat_unlock.h"
#include "plugins/inventory_tweaks.h"
#include "plugins/inventory_viewer.h"
#include "plugins/bank_tweaks.h"
#include "plugins/cart_viewer.h"
#include "plugins/equip_tweaks.h"
#include "plugins/window_pos_tweaks.h"
#include "plugins/status_icon_tweaks.h"
#include "plugins/quest_tracker_tweaks.h"
#include "plugins/settings_tweaks.h"
#include "plugins/weapon_layer.h"
#include "plugins/weapon_dual_sprites.h"
#include "plugins/hat_effect_depth.h"
#include "plugins/skill_tree_tweaks.h"
#include "plugins/fps_view.h"
#include "plugins/keyboard_move.h"
#include "plugins/player_jump.h"
#include "plugins/doom_tweaks.h"
#include "plugins/roggle_tweaks.h"
#include "plugins/rojeweled_tweaks.h"
#include "plugins/skill_bar_tweaks.h"
#include "plugins/item_desc_tweaks.h"
#include "plugins/storage_tweaks.h"
#include "plugins/cashshop_tweaks.h"
#include "plugins/shop_tweaks.h"
#include "plugins/vending_tweaks.h"
#include "plugins/trade_tweaks.h"
#include "plugins/rodex_tweaks.h"
#include "plugins/npc_dialog_tweaks.h"
#include "plugins/bug_report.h"
#include "plugins/character_sheet.h"
#include "plugins/login_parade.h"
#include "plugins/entity_names.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

Bourgeon::Bourgeon()
    : plugins_(), last_tick_count_(), log_lines_(), client_() {}

RagnarokClient& Bourgeon::client() { return client_; }
DiscordRelay* Bourgeon::discord_relay() { return discord_relay_; }
DpsMeter*     Bourgeon::dps_meter()     { return dps_meter_; }
BasicInfoTweaks* Bourgeon::basic_info() { return basic_info_; }
MenuIconTweaks* Bourgeon::menu_icons()  { return menu_icons_; }
StatusIconTweaks* Bourgeon::status_icons() { return status_icons_; }
QuestTrackerTweaks* Bourgeon::quest_tracker() { return quest_tracker_; }
SettingsTweaks* Bourgeon::settings_tweaks() { return settings_tweaks_; }
FpsViewTweaks* Bourgeon::fps_view() { return fps_view_; }
PlayerJumpTweaks* Bourgeon::player_jump() { return player_jump_; }
KeyboardMoveTweaks* Bourgeon::keyboard_move() { return keyboard_move_; }
DoomTweaks* Bourgeon::doom() { return doom_; }
RoggleTweaks* Bourgeon::roggle() { return roggle_; }
RojeweledTweaks* Bourgeon::rojeweled() { return rojeweled_; }
WeaponDualSprites* Bourgeon::weapon_dual_sprites() { return weapon_dual_sprites_; }
MoonlightUi* Bourgeon::moonlight_ui() { return moonlight_ui_; }
SkillBarTweaks* Bourgeon::skill_bar() { return skill_bar_; }
ChatTweaks* Bourgeon::chat_tweaks() { return chat_tweaks_; }
StorageTweaks* Bourgeon::storage_tweaks() { return storage_tweaks_; }
InventoryViewer* Bourgeon::inventory_viewer() { return inventory_viewer_; }
CartViewer* Bourgeon::cart_viewer() { return cart_viewer_; }
BankTweaks* Bourgeon::bank_tweaks() { return bank_tweaks_; }
CashShopTweaks* Bourgeon::cashshop_tweaks() { return cashshop_tweaks_; }
ShopTweaks* Bourgeon::shop_tweaks() { return shop_tweaks_; }
VendingTweaks* Bourgeon::vending_tweaks() { return vending_tweaks_; }
TradeTweaks* Bourgeon::trade_tweaks() { return trade_tweaks_; }
RodexTweaks* Bourgeon::rodex_tweaks() { return rodex_tweaks_; }
NpcDialogTweaks* Bourgeon::npc_dialog_tweaks() { return npc_dialog_tweaks_; }
BugReportTweaks* Bourgeon::bug_report() { return bug_report_; }
CharacterSheet* Bourgeon::character_sheet() { return character_sheet_; }
LoginParade* Bourgeon::login_parade() { return login_parade_; }
ItemDescTweaks* Bourgeon::item_desc() { return item_desc_; }
EntityNamesTweaks* Bourgeon::entity_names() { return entity_names_; }

namespace {
// Silence le message chat "Successfully purchased emotion." (EMSG_EMOTION_
// EXPANTION_BUY_SUCCESS) affiché à CHAQUE ACK d'octroi d'emote (ZC 0x0BED). Le
// serveur octroie tous les packs au login -> spam d'une ligne par pack. La string
// (@0x01091ae8) est référencée par DEUX émetteurs identiques :
//   - 0x00cb13c6 : dans NET_CashEmotion_RecvBuySuccess (0x00cb13a0), PAS le chemin
//     du grant login ;
//   - 0x00ca0c5d : dans le dispatcher cash-emotion inliné (le VRAI handler 0x0BED
//     au login, juste après CALL MarkPackPurchased @0x00ca0c58).
// Les deux ont le même bloc "message" de 38o : MOV ECX,[strMgr] + 4 push + CALL
// lookup EMSG (FUN_00771110) + push EAX/1 + MOV ECX,imm + CALL ajout chat
// (FUN_00a4ad20), les 2 CALL étant __thiscall (nettoient leur pile -> pas de
// déséquilibre). On NOP les 2 blocs, en conservant MarkPackPurchased (ownership)
// avant et le refresh UI après. Sanity-check (0x8B au début, 0xE8 à +0x21) pour ne
// rien patcher si le binaire diffère.
void PatchSilenceEmotePurchaseMsg() {
  constexpr uintptr_t kBlocks[] = {0x00cb13c6, 0x00ca0c5d};
  constexpr size_t    kLen = 0x26;  // 38o (jusqu'au MOV ECX,reg suivant)
  for (uintptr_t addr : kBlocks) {
    auto* p = reinterpret_cast<uint8_t*>(addr);
    __try {
      if (p[0] != 0x8B || p[0x21] != 0xE8) {
        LogError("[emote-msg] 0x{:08x} motif inattendu (0x{:02x}/0x{:02x}) — ignoré",
                 addr, p[0], p[0x21]);
        continue;
      }
      DWORD old;
      if (VirtualProtect(p, kLen, PAGE_EXECUTE_READWRITE, &old)) {
        memset(p, 0x90, kLen);  // NOP le bloc message
        VirtualProtect(p, kLen, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, kLen);
        // LogInfo("[emote-msg] 0x{:08x} silencie", addr);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      LogError("[emote-msg] 0x{:08x} exception pendant le patch", addr);
    }
  }
}

// ── Filtre de messages système du chat ──────────────────────────────────────
// FUN_00a4ad20 (__thiscall this=chatMgr, param_1=case, param_2=texte, ...) ajoute
// une ligne au chat quand param_1 == 1 ou 0x13. Son switch(param_1) tombe dans un
// DEFAULT no-op pour toute valeur non gérée. On détourne l'entrée : si le texte
// matche la blocklist, on réécrit param_1 sur la pile en 0x7fffffff -> la fonction
// s'exécute mais n'ajoute RIEN (et fait son propre épilogue/RET N -> zéro risque
// ABI). Réutilisable : ajouter une sous-chaîne à kBlockedMsgs pour masquer un
// autre message système (match par sous-chaîne, insensible aux codes couleur ^).
constexpr uintptr_t kChatAddFn = 0x00a4ad20;
void* g_tramp_chat = nullptr;
const char* const kBlockedMsgs[] = {
    "Command List: /h | /help",
    "error when loading the data account settings",
    "current shop display function is in",
};
int __fastcall ChatShouldBlock(int param_1, const char* text) {
  if ((param_1 != 1 && param_1 != 0x13) || text == nullptr) return 0;
  __try {
    for (const char* pat : kBlockedMsgs)
      if (std::strstr(text, pat) != nullptr) return 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return 0;
}
// Naked : sauve eax/ecx/edx, teste (param_1, texte) via ChatShouldBlock (__fastcall),
// et si bloqué neutralise param_1 sur la pile, puis continue dans l'original.
__declspec(naked) void ChatAddStub() {
  __asm {
    push eax
    push ecx
    push edx
    mov  ecx, [esp+0x10]   // param_1  ([esp]=edx,+4=ecx,+8=eax,+0xc=ret,+0x10=p1)
    mov  edx, [esp+0x14]   // param_2 (texte)
    call ChatShouldBlock   // __fastcall(ecx=p1, edx=texte) -> eax
    test eax, eax
    jz   chat_pass
    mov  dword ptr [esp+0x10], 0x7fffffff  // -> switch default (aucune ligne)
  chat_pass:
    pop  edx
    pop  ecx
    pop  eax
    jmp  [g_tramp_chat]
  }
}
void InstallChatMessageFilter() {
  g_tramp_chat = hooking::HookManager::Instance().SetHook(
      hooking::HookType::kJmpHook, reinterpret_cast<uint8_t*>(kChatAddFn),
      reinterpret_cast<uint8_t*>(&ChatAddStub));
  // LogInfo("[chat-filter] hook {}", g_tramp_chat != nullptr ? "OK" : "FAIL");
}
}  // namespace

bool Bourgeon::Initialize() {
  // LogInfo("Bourgeon {}\n", BOURGEON_VERSION);

  if (!client_.Initialize()) {
    LogError("Bourgeon failed to initialize");
    return false;
  }

  PatchSilenceEmotePurchaseMsg();  // supprime le spam "purchased emotion" au login
  InstallChatMessageFilter();      // masque quelques messages systeme au login

  // LogInfo("Bourgeon initialized successfully!");
  LoadPlugins();

  return true;
}

void Bourgeon::OnTick() {
  // Only run once every 100ms (6 frames at 60fps)
  const auto current_tick_count = GetTickCount();
  if (current_tick_count >= last_tick_count_ &&
      current_tick_count <= last_tick_count_ + 100) {
    return;
  }
  last_tick_count_ = current_tick_count;

  for (auto& plugin : plugins_) {
    try {
      plugin->OnTick();
    } catch (const std::exception& error) {
      LogError("[{}] OnTick: {}", plugin->name(), error.what());
    }
  }
}

void Bourgeon::OnProcessInput() {
  // Runs every frame in the game's input phase (NOT throttled like OnTick) so a
  // menu-icon click dispatches with the same timing/context as a native click —
  // OnTick's ~100ms throttle delayed it to a random frame, which made heavy
  // windows (world map) crash intermittently.
  if (auto* mi = menu_icons()) mi->FlushPending();
  // ⚠ Échoppe joueur : MÊME raison, mais pour un danger plus sévère que du
  // flicker. Ses boutons pilotent des commandes natives dont certaines ouvrent
  // une modale BLOQUANTE (UIWndMgr_ShowMessageBoxModal 0x00A31A30), qui ne rend
  // pas la main : elle boucle en RELANÇANT le tick/rendu du mode courant. La
  // déclencher depuis OnRenderUI — donc entre ImGui::NewFrame() et Render() —
  // relance le rendu en pleine frame ImGui. Les commandes sont donc empilées
  // pendant le rendu et rejouées ICI, hors de toute frame ImGui.
  if (auto* vt = vending_tweaks()) vt->FlushPending();
  // Déplacement clavier : ici AUSSI (pas seulement dans OnRenderUI) pour qu'il
  // survive au « cacher l'interface » natif (F11), qui coupe la passe UI des
  // plugins. Auto-limité dans le temps -> aucun doublon de demande.
  if (auto* km = keyboard_move()) km->Update();
  // Cache les fenêtres de description natives DANS LA PHASE INPUT (par frame, non
  // throttlé) -> flicker ~nul à l'ouverture d'un skill (dont l'OnMsg n'est PAS
  // hookée, contrairement à l'item). Idempotent/sûr (re-cache chaque frame).
  if (auto* idt = item_desc()) {
    idt->HideNativeDescWindows();  // item 0xc + comparaison 0xea
    idt->HideNativeSkillWindow();  // skill 0x2e
  }
}

void Bourgeon::AddLogLine(std::string log_line) {
  // LogInfo("[plugin] {}", log_line);
  log_lines_.emplace_back(std::move(log_line));
}

void Bourgeon::SetMapLoading(bool loading) {
  if (loading) map_loading_since_ms_.store(GetTickCount());
  map_loading_.store(loading);
}

bool Bourgeon::IsMapLoading() const {
  if (!map_loading_.load()) return false;
  // Safety cap: never report "loading" for more than 20 s, so a missed end
  // signal (CZ_NOTIFY_ACTORINIT 0x007d) can't permanently lock UI/input.
  return (GetTickCount() - map_loading_since_ms_.load()) <= 20000u;
}

void Bourgeon::NotifyGameUpdate() {
  // Per-frame heartbeat from GameMode::OnUpdateHook — CGameMode::OnUpdate only
  // runs while the game world is the active mode, so a fresh value means "in
  // game", and login/char-select (where it never runs) leaves it stale.
  last_game_update_ms_.store(GetTickCount());
}

bool Bourgeon::IsGameActive() const {
  const uint32_t last = last_game_update_ms_.load();
  if (last == 0) return false;  // CGameMode never updated (login / char-select)
  // Fresh within the last second = CGameMode is the actively-updating mode. A
  // cleared heartbeat (mode switch to login) or a stale one (char-change, where
  // the client does not reliably re-fire the mode switch) both read as "not in
  // game", so no plugin window survives the login / character-select screens.
  return (GetTickCount() - last) <= 1000u;
}

namespace {

// « Cacher toute l'interface » natif (F11 par défaut) : le hotkey behavior 0x74
// appelle UIWindowMgr::ToggleHideAllWindows (0x00a47720), une machine à états
// dont le compteur vit dans g_UIWindowMgr(0x0131f4e8)+0x508 :
//   0 = jamais utilisé, 1 = interface visible, 2 = interface cachée.
// L'état 2 est posé après que la fonction a parcouru la liste des fenêtres
// (+0x17c) et appelé SetVisible(0) sur chacune, en mémorisant les fenêtres
// masquées dans +0x50c pour les restaurer au basculement suivant. On lit donc
// l'état natif au lieu d'entretenir notre propre toggle : peu importe la touche
// réellement liée au behavior 0x74 (remappable via les hotkeys Lua), l'overlay
// suit toujours l'interface du jeu.
bool IsNativeUiHidden() {
  constexpr uintptr_t kHideAllStateOffset = 0x508;
  return *reinterpret_cast<const int*>(uiwnd::kUIWindowMgrAddr + kHideAllStateOffset) == 2;
}

}  // namespace

void Bourgeon::RenderUI() {
  // Aucune fenêtre ImGui ne sort de l'écran de jeu. En PREMIÈRE ligne, avant tout
  // return anticipé et avant le moindre Begin de plugin : on est juste après
  // ImGui::NewFrame() (les deux chemins de rendu, DX7 et DX9, appellent RenderUI
  // là), donc le déplacement souris de la frame est déjà appliqué et la position
  // corrigée est celle qui sera dessinée. Écrans de login inclus (le bloc
  // OnRenderLoginUI ci-dessous dessine aussi des fenêtres). Voir ui/window_clamp.h.
  ro::KeepWindowsOnScreen();
  // Stand down while a map is loading: hide all plugin UI. This also stops
  // SkillBarTweaks::EnsureCreated() from MakeWindow'ing the native shortcut bar
  // every frame while the HUD is being torn down/rebuilt — the race that freed a
  // UIShortCutWnd while it was still in the native window-snap manager and caused
  // the use-after-free crash (WinSnap edge-adjacency deref at 0x007a85c4).
  if (IsMapLoading()) return;
  // Stand down outside the game world: at the login and character-select screens
  // CGameMode::OnUpdate does not run, so the game-update heartbeat is stale/cleared
  // and we draw no in-game plugin UI. This is the single choke point that guarantees
  // no plugin ImGui window can linger on those screens, regardless of a plugin's own
  // in_game_ tracking (which the char-change path can leave stale).
  // EXCEPTION: plugins that opt into login/char-select cosmetics get a dedicated
  // hook here (OnRenderLoginUI) — e.g. the Poring parade — then we still return so
  // the normal in-game path never runs off-world.
  if (!IsGameActive()) {
    // Règle des écrans hors-jeu : on ne minimise RIEN. Les fenêtres RO du login
    // (formulaire de connexion Moonlight…) perdent leur bouton sys_mini —
    // replier un formulaire de connexion en simple barre de titre n'a aucun
    // sens et le joueur n'a pas de barre des tâches pour le retrouver.
    ro::SetWindowCollapseAllowed(false);
    for (auto& plugin : plugins_) {
      try {
        plugin->OnRenderLoginUI();
      } catch (const std::exception& error) {
        LogError("[{}] OnRenderLoginUI: {}", plugin->name(), error.what());
      }
    }
    return;
  }
  // Suivre le « cacher l'interface » natif (F11) : quand le joueur masque l'UI du
  // jeu — capture d'écran, vue dégagée — l'overlay ImGui disparaît avec elle.
  if (IsNativeUiHidden()) return;
  // Même règle quand une UI plein écran remplace le HUD (carte du monde) : le jeu
  // cesse de dessiner son interface, l'overlay doit suivre.
  //
  // Ce test existait déjà, mais SEULEMENT dans le OnRenderUI de trois plugins qui
  // le refaisaient chacun chez eux (grille d'alignement, barres d'exp/portrait,
  // icônes de menu). Tous les overlays ajoutés depuis — panneau Moonlight, barres
  // de skill, DPS, FPS, inventaire, storage… — restaient affichés par-dessus la
  // carte du monde. La duplication masquait le manque : trois copies du test
  // donnaient l'impression que le cas était traité, alors qu'aucune ne pouvait
  // couvrir le rendu d'un autre plugin. C'est ici, et nulle part ailleurs, que la
  // question « le HUD est-il remplacé ? » doit se poser pour le rendu.
  //
  // Les trois tests locaux sont donc retirés : gardés, ils auraient de nouveau
  // laissé croire que chaque plugin gère le cas lui-même. Un seul survit,
  // MenuIconTweaks::FlushPending — il ne filtre pas un rendu mais un CLIC, et
  // s'exécute hors de cette boucle.
  if (uiwnd::IsHudReplaced()) return;
  // if (strstr(GetCommandLineA(), "--console") != nullptr) { // Render Bourgeon's main window
  // ShowBourgeonWindow();
  // }
  if (strstr(GetCommandLineA(), "--demo") != nullptr) {
    ImGui::ShowDemoWindow();
  }
  // En jeu : le repli des fenêtres RO redevient disponible (pendant du blocage
  // posé sur la branche login ci-dessus).
  ro::SetWindowCollapseAllowed(true);
  // Render windows drawn by plugins
  for (auto& plugin : plugins_) {
    try {
      plugin->OnRenderUI();
    } catch (const std::exception& error) {
      LogError("[{}] OnRenderUI: {}", plugin->name(), error.what());
    }
  }
  // Échap centralisé : ferme la fenêtre RO la plus au-dessus (après que toutes se
  // soient enregistrées ce frame). Voir ui/ro_imgui.h.
  ro::ProcessEscapeStack();
}

void Bourgeon::FireModeSwitch(ModeMgr::ModeType mode_type,
                              const char* map_name) {
  // Leaving the game world (login / character-select): clear the game-update
  // heartbeat so RenderUI hides every plugin window this very frame, instead of
  // waiting up to a second for it to go stale. Entering the game re-arms it via
  // NotifyGameUpdate() from GameMode::OnUpdateHook.
  if (mode_type != ModeMgr::ModeType::kGame) {
    last_game_update_ms_.store(0);
    // Les cooldowns appartiennent au personnage quitté ; ceux qui courent encore
    // sont réémis par le serveur à l'entrée en jeu (skill_blockpc_start).
    ro::ClearSkillCooldowns();
  }
  for (auto& plugin : plugins_) {
    try {
      plugin->OnModeSwitch(mode_type, map_name);
    } catch (const std::exception& error) {
      LogError("[{}] OnModeSwitch: {}", plugin->name(), error.what());
    }
  }
}

void Bourgeon::FireTalkType(const char* chat_buffer) {
  for (auto& plugin : plugins_) {
    try {
      plugin->OnTalkType(chat_buffer);
    } catch (const std::exception& error) {
      LogError("[{}] OnTalkType: {}", plugin->name(), error.what());
    }
  }
}

void Bourgeon::FireChatMessage(const char* chat_buffer) {
  for (auto& plugin : plugins_) {
    try {
      plugin->OnChatMessage(chat_buffer);
    } catch (const std::exception& error) {
      LogError("[{}] OnChatMessage: {}", plugin->name(), error.what());
    }
  }
}

void Bourgeon::FireKeyDown(unsigned long vkey, int new_key, int accurate_key) {
  for (auto& plugin : plugins_) {
    try {
      plugin->OnKeyDown(vkey, new_key, accurate_key);
    } catch (const std::exception& error) {
      LogError("[{}] OnKeyDown: {}", plugin->name(), error.what());
    }
  }
}

void Bourgeon::FireRecvPacket(uint16_t opcode, const uint8_t* data,
                              uint16_t len) {
  // Table de cooldowns partagée : alimentée ici, en un seul point, avant les
  // plugins. Ceux qui l'affichent (barre d'action, feuille de perso) ne
  // dépendent donc ni de leur ordre de chargement ni de leur présence.
  ro::FeedSkillCooldownPacket(opcode, data, len);
  for (auto& plugin : plugins_) {
    try {
      plugin->OnRecvPacket(opcode, data, len);
    } catch (const std::exception& error) {
      LogError("[{}] OnRecvPacket: {}", plugin->name(), error.what());
    }
  }
}

bool Bourgeon::SendPacket(const uint8_t* buf, size_t len) {
  return client_.rag_connection().SendPacket(
      static_cast<int>(len), reinterpret_cast<char*>(const_cast<uint8_t*>(buf)));
}

void Bourgeon::RegisterRecvOpcode(uint16_t opcode) {
  client_.rag_connection().RegisterRecvOpcode(opcode);
}

void Bourgeon::RegisterObserveOpcode(uint16_t opcode, uint16_t forward_len) {
  client_.rag_connection().RegisterObserveOpcode(opcode, forward_len);
}

void Bourgeon::LoadPlugins() {
  // Services partagés, avant les plugins : ils observent des paquets pour le
  // compte de plusieurs d'entre eux (voir ragnarok/skill_cooldowns.h).
  ro::InstallSkillCooldowns();

  AutoLogin* auto_login = nullptr;
  {
    auto al = std::make_unique<AutoLogin>();
    auto_login = al.get();
    plugins_.emplace_back(std::move(al));
  }
  // Front d'authentification ImGui « compte Moonlight ». Délègue le login natif
  // final à AutoLogin une fois le compte RO choisi (voir moonlight_auth.h).
  MoonlightAuth* moonlight_auth = nullptr;
  {
    auto ma = std::make_unique<MoonlightAuth>(auto_login);
    moonlight_auth = ma.get();
    plugins_.emplace_back(std::move(ma));
  }
  // Remplacement ImGui du char-select (activé par défaut ; opt-out yaml
  // char_select.imgui:false). Réservé au parcours de login Moonlight (gate via
  // moonlight_auth). Socle du lobby unifié ; voir char_select.h / charselect_re.md.
  plugins_.emplace_back(std::make_unique<CharSelect>(moonlight_auth));
  plugins_.emplace_back(std::make_unique<IntegrityCheck>());
  plugins_.emplace_back(std::make_unique<CheatDetector>());
  // Avertit dès l'écran de login quand le client rend en DirectX 7 (voir
  // dx7_warning.h) : la moitié des modules graphiques de Bourgeon y est inerte.
  plugins_.emplace_back(std::make_unique<Dx7Warning>());
  {
    auto moonlight_ui = std::make_unique<MoonlightUi>();
    moonlight_ui_ = moonlight_ui.get();
    plugins_.emplace_back(std::move(moonlight_ui));
  }
  {
    auto bug_report = std::make_unique<BugReportTweaks>();
    bug_report_ = bug_report.get();
    plugins_.emplace_back(std::move(bug_report));
  }
  {
    auto chat_tweaks = std::make_unique<ChatTweaks>();
    chat_tweaks_ = chat_tweaks.get();
    plugins_.emplace_back(std::move(chat_tweaks));
  }
  plugins_.emplace_back(std::make_unique<StatusTweaks>());
  plugins_.emplace_back(std::make_unique<BerserkChatUnlock>());
  plugins_.emplace_back(std::make_unique<InventoryTweaks>());
  {
    auto inventory_viewer = std::make_unique<InventoryViewer>();
    inventory_viewer_ = inventory_viewer.get();
    plugins_.emplace_back(std::move(inventory_viewer));
  }
  {
    // Cart : même famille que l'inventaire (fenêtre sœur côté client), et même
    // interrupteur de groupe « Interface moderne ».
    auto cart_viewer = std::make_unique<CartViewer>();
    cart_viewer_ = cart_viewer.get();
    plugins_.emplace_back(std::move(cart_viewer));
  }
  {
    auto storage_tweaks = std::make_unique<StorageTweaks>();
    storage_tweaks_ = storage_tweaks.get();
    plugins_.emplace_back(std::move(storage_tweaks));
  }
  {
    // Banque de zeny (Ctrl+B) : remplace la fenêtre native (masquée) quand le
    // groupe « Interface moderne » est actif.
    auto bank_tweaks = std::make_unique<BankTweaks>();
    bank_tweaks_ = bank_tweaks.get();
    plugins_.emplace_back(std::move(bank_tweaks));
  }
  {
    auto cashshop_tweaks = std::make_unique<CashShopTweaks>();
    cashshop_tweaks_ = cashshop_tweaks.get();
    plugins_.emplace_back(std::move(cashshop_tweaks));
  }
  {
    auto shop_tweaks = std::make_unique<ShopTweaks>();
    shop_tweaks_ = shop_tweaks.get();
    plugins_.emplace_back(std::move(shop_tweaks));
  }
  {
    // Échoppe joueur (vente ET achat : même classe native, cf. vending_tweaks.h).
    auto vending_tweaks = std::make_unique<VendingTweaks>();
    vending_tweaks_ = vending_tweaks.get();
    plugins_.emplace_back(std::move(vending_tweaks));
  }
  {
    auto trade_tweaks = std::make_unique<TradeTweaks>();
    trade_tweaks_ = trade_tweaks.get();
    plugins_.emplace_back(std::move(trade_tweaks));

    auto rodex_tweaks = std::make_unique<RodexTweaks>();
    rodex_tweaks_ = rodex_tweaks.get();
    plugins_.emplace_back(std::move(rodex_tweaks));

    auto npc_dialog_tweaks = std::make_unique<NpcDialogTweaks>();
    npc_dialog_tweaks_ = npc_dialog_tweaks.get();
    plugins_.emplace_back(std::move(npc_dialog_tweaks));
  }
  {
    auto character_sheet = std::make_unique<CharacterSheet>();
    character_sheet_ = character_sheet.get();
    plugins_.emplace_back(std::move(character_sheet));
  }
  {
    auto login_parade = std::make_unique<LoginParade>();
    login_parade_ = login_parade.get();
    plugins_.emplace_back(std::move(login_parade));
  }
  plugins_.emplace_back(std::make_unique<EquipTweaks>());
  plugins_.emplace_back(std::make_unique<WindowPosTweaks>());
  plugins_.emplace_back(std::make_unique<WeaponLayerTweaks>());
  {
    auto weapon_dual = std::make_unique<WeaponDualSprites>();
    weapon_dual_sprites_ = weapon_dual.get();
    plugins_.emplace_back(std::move(weapon_dual));
  }
  plugins_.emplace_back(std::make_unique<HatEffectDepthTweaks>());
  plugins_.emplace_back(std::make_unique<SkillTreeTweaks>());
  {
    auto fps_view = std::make_unique<FpsViewTweaks>();
    fps_view_ = fps_view.get();
    plugins_.emplace_back(std::move(fps_view));
  }
  {
    auto player_jump = std::make_unique<PlayerJumpTweaks>();
    player_jump_ = player_jump.get();
    plugins_.emplace_back(std::move(player_jump));
  }
  {
    auto keyboard_move = std::make_unique<KeyboardMoveTweaks>();
    keyboard_move_ = keyboard_move.get();
    plugins_.emplace_back(std::move(keyboard_move));
  }
  {
    auto doom = std::make_unique<DoomTweaks>();
    doom_ = doom.get();
    plugins_.emplace_back(std::move(doom));
  }
  {
    auto roggle = std::make_unique<RoggleTweaks>();
    roggle_ = roggle.get();
    plugins_.emplace_back(std::move(roggle));
  }
  {
    auto rojeweled = std::make_unique<RojeweledTweaks>();
    rojeweled_ = rojeweled.get();
    plugins_.emplace_back(std::move(rojeweled));
  }
  {
    auto item_desc = std::make_unique<ItemDescTweaks>();
    item_desc_ = item_desc.get();
    plugins_.emplace_back(std::move(item_desc));
  }
  {
    auto skill_bar = std::make_unique<SkillBarTweaks>();
    skill_bar_ = skill_bar.get();
    plugins_.emplace_back(std::move(skill_bar));
  }
  {
    auto status_icons = std::make_unique<StatusIconTweaks>();
    status_icons_ = status_icons.get();
    plugins_.emplace_back(std::move(status_icons));
  }
  {
    auto quest_tracker = std::make_unique<QuestTrackerTweaks>();
    quest_tracker_ = quest_tracker.get();
    plugins_.emplace_back(std::move(quest_tracker));
  }
  {
    auto settings_tweaks = std::make_unique<SettingsTweaks>();
    settings_tweaks_ = settings_tweaks.get();
    plugins_.emplace_back(std::move(settings_tweaks));
  }
  {
    auto dps = std::make_unique<DpsMeter>();
    dps_meter_ = dps.get();
    plugins_.emplace_back(std::move(dps));
  }
  {
    auto basic_info = std::make_unique<BasicInfoTweaks>();
    basic_info_ = basic_info.get();
    plugins_.emplace_back(std::move(basic_info));
  }
  {
    auto menu_icons = std::make_unique<MenuIconTweaks>();
    menu_icons_ = menu_icons.get();
    plugins_.emplace_back(std::move(menu_icons));
  }
  {
    auto relay = std::make_unique<DiscordRelay>();
    discord_relay_ = relay.get();
    plugins_.emplace_back(std::move(relay));
  }
  {
    auto entity_names = std::make_unique<EntityNamesTweaks>();
    entity_names_ = entity_names.get();
    plugins_.emplace_back(std::move(entity_names));
  }

  for (const auto& plugin : plugins_) {
    // LogInfo("Loaded plugin: {}", plugin->name());
  }
}

void Bourgeon::ShowBourgeonWindow() const {
  ImGui::Begin("Bourgeon");

  // List of loaded plugins
  if (ImGui::CollapsingHeader("Loaded plugins")) {
    for (const auto& plugin : plugins_) {
      ImGui::BulletText("%s", plugin->name());
    }
  }

  // Logs: a live mirror of every LogInfo/LogDiag/LogError (fed by the in-memory
  // spdlog sink), not just the plugin lines pushed via AddLogLine.  Snapshotted
  // each frame so it stays thread-safe against sinks running on other threads.
  if (ImGui::CollapsingHeader("Logs")) {
    std::vector<std::string> log_lines;
    LogLineBuffer::instance().Snapshot(&log_lines);
    ImGui::BeginChild("scrolling", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    // Clipped lines
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(log_lines.size()));
    while (clipper.Step()) {
      for (int line_no = clipper.DisplayStart; line_no < clipper.DisplayEnd;
           line_no++) {
        ImGui::TextUnformatted(log_lines[line_no].c_str());
      }
    }
    clipper.End();
    ImGui::PopStyleVar();

    // Auto-scroll when at the bottom
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
      ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
  }

  ImGui::End();
}
