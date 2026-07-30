#include "bourgeon.h"

#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include "imgui.h"
#include "ragnarok/skill_cooldowns.h"
#include "ui/ro_imgui.h"
#include "ui/window_clamp.h"
#include "ui/window_zorder.h"
#include "features/systems/auto_login.h"
#include "features/systems/moonlight_auth.h"
#include "features/windows/char_select.h"
#include "features/patches/chat.h"
#include "features/systems/cheat_detector.h"
#include "features/systems/discord_relay.h"
#include "features/overlays/basic_info.h"
#include "features/overlays/dps_meter.h"
#include "features/overlays/menu_icons.h"
#include "features/systems/integrity_check.h"
#include "features/systems/dx7_warning.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/patches/status_tweaks.h"
#include "features/patches/berserk_chat_unlock.h"
#include "features/patches/inventory_tweaks.h"
#include "features/windows/inventory_viewer.h"
#include "features/windows/bank_window.h"
#include "features/windows/cart_viewer.h"
#include "features/patches/equip_tweaks.h"
#include "features/patches/window_pos_tweaks.h"
#include "features/overlays/status_icon_bar.h"
#include "features/overlays/quest_tracker.h"
#include "features/fx/screen_fx.h"
#include "features/fx/weapon_layer.h"
#include "features/fx/weapon_dual_sprites.h"
#include "features/fx/hat_effect_depth.h"
#include "features/patches/skill_tree_tweaks.h"
#include "features/gameplay/fps_view.h"
#include "features/gameplay/keyboard_move.h"
#include "features/gameplay/player_jump.h"
#include "features/minigames/doom.h"
#include "features/minigames/roggle.h"
#include "features/minigames/rojeweled.h"
#include "features/overlays/skill_bar.h"
#include "features/item_cell.h"  // itemcell::FlushDeferredDesc (desc au relâchement)
#include "features/windows/item_desc_window.h"
#include "features/windows/storage_window.h"
#include "features/windows/cashshop_window.h"
#include "features/windows/npc_shop_window.h"
#include "features/windows/vending_window.h"
#include "features/windows/make_item_window.h"
#include "features/windows/weapon_refine_window.h"
#include "features/windows/trade_window.h"
#include "features/windows/rodex_window.h"
#include "features/windows/npc_dialog_window.h"
#include "features/systems/bug_report.h"
#include "features/windows/character_sheet.h"
#include "features/overlays/login_parade.h"
#include "features/overlays/entity_names.h"
#include "utils/hooking/hook_manager.h"
#include "features/staff_gate.h"  // IsStaff() — gate de la fenêtre de logs
#include "utils/log_console.h"

Bourgeon::Bourgeon()
    : plugins_(), last_tick_count_(), log_lines_(), client_() {}

RagnarokClient& Bourgeon::client() { return client_; }
DiscordRelay* Bourgeon::discord_relay() { return discord_relay_; }
DpsMeter*     Bourgeon::dps_meter()     { return dps_meter_; }
BasicInfo* Bourgeon::basic_info() { return basic_info_; }
MenuIcons* Bourgeon::menu_icons()  { return menu_icons_; }
StatusIconBar* Bourgeon::status_icons() { return status_icons_; }
QuestTracker* Bourgeon::quest_tracker() { return quest_tracker_; }
ScreenFx* Bourgeon::screen_fx() { return screen_fx_; }
FpsView* Bourgeon::fps_view() { return fps_view_; }
PlayerJump* Bourgeon::player_jump() { return player_jump_; }
KeyboardMove* Bourgeon::keyboard_move() { return keyboard_move_; }
Doom* Bourgeon::doom() { return doom_; }
Roggle* Bourgeon::roggle() { return roggle_; }
Rojeweled* Bourgeon::rojeweled() { return rojeweled_; }
WeaponDualSprites* Bourgeon::weapon_dual_sprites() { return weapon_dual_sprites_; }
MoonlightUi* Bourgeon::moonlight_ui() { return moonlight_ui_; }
SkillBar* Bourgeon::skill_bar() { return skill_bar_; }
ChatTweaks* Bourgeon::chat_tweaks() { return chat_tweaks_; }
StorageWindow* Bourgeon::storage_window() { return storage_window_; }
InventoryViewer* Bourgeon::inventory_viewer() { return inventory_viewer_; }
CartViewer* Bourgeon::cart_viewer() { return cart_viewer_; }
BankWindow* Bourgeon::bank_window() { return bank_window_; }
CashShopWindow* Bourgeon::cashshop_window() { return cashshop_window_; }
NpcShopWindow* Bourgeon::npc_shop_window() { return npc_shop_window_; }
VendingWindow* Bourgeon::vending_window() { return vending_window_; }
WeaponRefineWindow* Bourgeon::weapon_refine_window() { return weapon_refine_window_; }
MakeItemWindow* Bourgeon::make_item_window() { return make_item_window_; }
TradeWindow* Bourgeon::trade_window() { return trade_window_; }
RodexWindow* Bourgeon::rodex_window() { return rodex_window_; }
NpcDialogWindow* Bourgeon::npc_dialog_window() { return npc_dialog_window_; }
BugReport* Bourgeon::bug_report() { return bug_report_; }
CharacterSheet* Bourgeon::character_sheet() { return character_sheet_; }
LoginParade* Bourgeon::login_parade() { return login_parade_; }
ItemDescWindow* Bourgeon::item_desc() { return item_desc_; }
EntityNames* Bourgeon::entity_names() { return entity_names_; }

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
  if (auto* vt = vending_window()) vt->FlushPending();
  // Refine Whitesmith : MÊME raison. Ses actions rejouent des chemins
  // natifs (CMode::SendMsg cmd 182, destruction de la fenêtre 111, relance du
  // skill) qui peuvent déclencher une modale BLOQUANTE — laquelle relance le
  // tick/rendu du mode et gèlerait le client si on l'atteignait depuis une
  // frame ImGui (cf. docs/weapon_refine_re.md §6).
  if (auto* wr = weapon_refine_window()) wr->FlushPending();
  // Fabrication : MÊME raison. Ses actions rejouent CMode::SendMsg (cmd 130/153/207)
  // et peuvent OUVRIR une fenêtre native (la 80, choix des matériaux) — deux
  // chemins qui n'ont rien à faire entre NewFrame() et Render().
  if (auto* mi = make_item_window()) mi->FlushPending();
  // Déplacement clavier : ici AUSSI (pas seulement dans OnRenderUI) pour qu'il
  // survive au « cacher l'interface » natif (F11), qui coupe la passe UI des
  // plugins. Auto-limité dans le temps -> aucun doublon de demande.
  if (auto* km = keyboard_move()) km->Update();
  // Description d'item : les viewers ARMENT au clic (itemcell::DeferDesc*), on
  // OUVRE ici — hors frame ImGui ET bouton relâché. Ouverte au clic, un appui
  // PROLONGÉ faisait passer la description DERRIÈRE la fenêtre cliquée (course
  // de focus détaillée dans item_cell.h). Avant HideNativeDescWindows, pour que
  // la fenêtre native qui vient de naître soit cachée dans la même passe.
  itemcell::FlushDeferredDesc();
  // Cache les fenêtres de description natives DANS LA PHASE INPUT (par frame, non
  // throttlé) -> flicker ~nul à l'ouverture d'un skill (dont l'OnMsg n'est PAS
  // hookée, contrairement à l'item). Idempotent/sûr (re-cache chaque frame).
  if (auto* idt = item_desc()) {
    idt->HideNativeDescWindows();  // item 0xc + comparaison 0xea
    idt->HideNativeSkillWindow();  // skill 0x2e
  }
}

void Bourgeon::NotifySkillCast(int skill_id, int skill_lv) {
  // Observation pure, sur un chemin TRÈS chaud et ré-entrant : on se contente de
  // relayer, sans allocation ni travail.
  if (auto* mk = make_item_window()) mk->NotifySkillCast(skill_id, skill_lv);
}

void Bourgeon::NotifyItemUse(unsigned item_index) {
  // Simple relais, comme NotifySkillCast : ce hook est sur le chemin d'envoi de
  // TOUS les paquets, il ne doit rien faire de plus qu'un test de pointeur quand
  // le plugin est absent. La résolution index -> identifiant appartient au
  // plugin, qui possède le parcours d'inventaire vérifié.
  if (auto* mk = make_item_window()) mk->NotifyItemUse(item_index);
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
  // Le HUD (barre de skills, barres HP/SP/XP/zeny/poids, portrait) reste sous
  // toutes les vraies fenêtres. Même endroit et mêmes raisons que le clamp
  // ci-dessus : après NewFrame, avant le premier Begin. Voir ui/window_zorder.h.
  ro::SendBackgroundWindowsToBack();
  // Stand down while a map is loading: hide all plugin UI. This also stops
  // SkillBar::EnsureCreated() from MakeWindow'ing the native shortcut bar
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
  // MenuIcons::FlushPending — il ne filtre pas un rendu mais un CLIC, et
  // s'exécute hors de cette boucle.
  if (uiwnd::IsHudReplaced()) return;
  // Fenêtre de logs en jeu, à la place de la console Windows. Double condition,
  // et les deux comptent : le gate STAFF (le journal expose tout ce que le client
  // trace) et un interrupteur explicite dans « Staff Tools ». Le test de staff
  // est refait ICI, au rendu, et pas seulement là où la case se coche : un niveau
  // de groupe peut changer en cours de session (reconnexion, changement de
  // personnage), et la fenêtre doit disparaître avec le droit.
  if (show_log_window_ && IsStaff()) ShowBourgeonWindow();
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

void Bourgeon::RegisterReplaceOpcode(uint16_t opcode,
                                     std::function<bool()> claim) {
  client_.rag_connection().RegisterReplaceOpcode(opcode, std::move(claim));
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
    auto bug_report = std::make_unique<BugReport>();
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
    auto storage_window = std::make_unique<StorageWindow>();
    storage_window_ = storage_window.get();
    plugins_.emplace_back(std::move(storage_window));
  }
  {
    // Banque de zeny (Ctrl+B) : remplace la fenêtre native (masquée) quand le
    // groupe « Interface moderne » est actif.
    auto bank_window = std::make_unique<BankWindow>();
    bank_window_ = bank_window.get();
    plugins_.emplace_back(std::move(bank_window));
  }
  {
    auto cashshop_window = std::make_unique<CashShopWindow>();
    cashshop_window_ = cashshop_window.get();
    plugins_.emplace_back(std::move(cashshop_window));
  }
  {
    auto npc_shop_window = std::make_unique<NpcShopWindow>();
    npc_shop_window_ = npc_shop_window.get();
    plugins_.emplace_back(std::move(npc_shop_window));
  }
  {
    // Échoppe joueur (vente ET achat : même classe native, cf. vending_window.h).
    auto vending_window = std::make_unique<VendingWindow>();
    vending_window_ = vending_window.get();
    plugins_.emplace_back(std::move(vending_window));
    // Refine d'arme Whitesmith : remplace « Upgradeable weapons » (id 111).
    // Doit exister AVANT tout paquet 0x0221 — son constructeur enregistre
    // l'observation de l'opcode et pose le détour de la modale « liste vide ».
    auto weapon_refine_window = std::make_unique<WeaponRefineWindow>();
    weapon_refine_window_ = weapon_refine_window.get();
    plugins_.emplace_back(std::move(weapon_refine_window));

    // Fabrication : remplace les DEUX fenêtres de liste natives (94 « LIST » et
    // 79 « Manufacturing List ») — cf. docs/make_item_list_re.md.
    auto make_item_window = std::make_unique<MakeItemWindow>();
    make_item_window_ = make_item_window.get();
    plugins_.emplace_back(std::move(make_item_window));
  }
  {
    auto trade_window = std::make_unique<TradeWindow>();
    trade_window_ = trade_window.get();
    plugins_.emplace_back(std::move(trade_window));

    auto rodex_window = std::make_unique<RodexWindow>();
    rodex_window_ = rodex_window.get();
    plugins_.emplace_back(std::move(rodex_window));

    auto npc_dialog_window = std::make_unique<NpcDialogWindow>();
    npc_dialog_window_ = npc_dialog_window.get();
    plugins_.emplace_back(std::move(npc_dialog_window));
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
  plugins_.emplace_back(std::make_unique<WeaponLayer>());
  {
    auto weapon_dual = std::make_unique<WeaponDualSprites>();
    weapon_dual_sprites_ = weapon_dual.get();
    plugins_.emplace_back(std::move(weapon_dual));
  }
  plugins_.emplace_back(std::make_unique<HatEffectDepth>());
  plugins_.emplace_back(std::make_unique<SkillTreeTweaks>());
  {
    auto fps_view = std::make_unique<FpsView>();
    fps_view_ = fps_view.get();
    plugins_.emplace_back(std::move(fps_view));
  }
  {
    auto player_jump = std::make_unique<PlayerJump>();
    player_jump_ = player_jump.get();
    plugins_.emplace_back(std::move(player_jump));
  }
  {
    auto keyboard_move = std::make_unique<KeyboardMove>();
    keyboard_move_ = keyboard_move.get();
    plugins_.emplace_back(std::move(keyboard_move));
  }
  {
    auto doom = std::make_unique<Doom>();
    doom_ = doom.get();
    plugins_.emplace_back(std::move(doom));
  }
  {
    auto roggle = std::make_unique<Roggle>();
    roggle_ = roggle.get();
    plugins_.emplace_back(std::move(roggle));
  }
  {
    auto rojeweled = std::make_unique<Rojeweled>();
    rojeweled_ = rojeweled.get();
    plugins_.emplace_back(std::move(rojeweled));
  }
  {
    auto item_desc = std::make_unique<ItemDescWindow>();
    item_desc_ = item_desc.get();
    plugins_.emplace_back(std::move(item_desc));
  }
  {
    auto skill_bar = std::make_unique<SkillBar>();
    skill_bar_ = skill_bar.get();
    plugins_.emplace_back(std::move(skill_bar));
  }
  {
    auto status_icons = std::make_unique<StatusIconBar>();
    status_icons_ = status_icons.get();
    plugins_.emplace_back(std::move(status_icons));
  }
  {
    auto quest_tracker = std::make_unique<QuestTracker>();
    quest_tracker_ = quest_tracker.get();
    plugins_.emplace_back(std::move(quest_tracker));
  }
  {
    auto screen_fx = std::make_unique<ScreenFx>();
    screen_fx_ = screen_fx.get();
    plugins_.emplace_back(std::move(screen_fx));
  }
  {
    auto dps = std::make_unique<DpsMeter>();
    dps_meter_ = dps.get();
    plugins_.emplace_back(std::move(dps));
  }
  {
    auto basic_info = std::make_unique<BasicInfo>();
    basic_info_ = basic_info.get();
    plugins_.emplace_back(std::move(basic_info));
  }
  {
    auto menu_icons = std::make_unique<MenuIcons>();
    menu_icons_ = menu_icons.get();
    plugins_.emplace_back(std::move(menu_icons));
  }
  {
    auto relay = std::make_unique<DiscordRelay>();
    discord_relay_ = relay.get();
    plugins_.emplace_back(std::move(relay));
  }
  {
    auto entity_names = std::make_unique<EntityNames>();
    entity_names_ = entity_names.get();
    plugins_.emplace_back(std::move(entity_names));
  }

  for (const auto& plugin : plugins_) {
    // LogInfo("Loaded plugin: {}", plugin->name());
  }
}

void Bourgeon::ShowBourgeonWindow() {
  ImGui::SetNextWindowSize(ImVec2(760.0f, 430.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Bourgeon", &show_log_window_)) {
    ImGui::End();
    return;
  }

  // List of loaded plugins
  if (ImGui::CollapsingHeader("Loaded plugins")) {
    for (const auto& plugin : plugins_) {
      ImGui::BulletText("%s", plugin->name());
    }
  }

  // Logs: a live mirror of every LogInfo/LogDiag/LogError (fed by the in-memory
  // spdlog sink), not just the plugin lines pushed via AddLogLine.  Snapshotted
  // each frame so it stays thread-safe against sinks running on other threads.
  //
  // ── Pourquoi un InputTextMultiline en lecture seule, et plus un TextUnformatted
  // par ligne ── Le rendu précédent était INCOPIABLE : ImGui ne sait pas
  // sélectionner du texte statique. Or un journal qu'on ne peut pas coller dans un
  // rapport ne sert qu'à être relu à voix haute. Le champ de saisie, lui, apporte
  // gratuitement la sélection à la souris, Ctrl+A et Ctrl+C — le comportement
  // attendu de n'importe quelle console.
  //
  // Ce qu'on perd : le clipper (les lignes sont toutes mises en forme d'un bloc).
  // D'où la borne kMaxShown : au-delà, on ne garde que la QUEUE, la seule qui
  // compte pour un diagnostic en cours.
  if (ImGui::CollapsingHeader("Logs", ImGuiTreeNodeFlags_DefaultOpen)) {
    constexpr size_t kMaxShown = 2000;

    static std::vector<std::string> log_lines;
    static std::vector<char> flat;      // tampon NUL-terminé pour ImGui
    static size_t last_count = static_cast<size_t>(-1);
    static char   filter[64] = {0};
    static char   last_filter[64] = {0};
    static bool   follow = true;
    static bool   scroll_to_end = false;

    LogLineBuffer::instance().Snapshot(&log_lines);

    // Le tampon n'est reconstruit que si quelque chose a changé : le remettre à
    // plat à chaque frame recopierait des centaines de kilo-octets pour rien, et
    // ferait sauter la sélection en cours sous la souris du joueur.
    const bool filter_changed = std::strcmp(filter, last_filter) != 0;
    if (log_lines.size() != last_count || filter_changed) {
      last_count = log_lines.size();
      std::strncpy(last_filter, filter, sizeof(last_filter) - 1);
      last_filter[sizeof(last_filter) - 1] = '\0';

      std::string joined;
      size_t kept = 0;
      // Deux passes : on compte d'abord les lignes retenues pour ne garder que
      // les kMaxShown DERNIÈRES (la queue), pas les premières.
      for (const auto& line : log_lines)
        if (!filter[0] || line.find(filter) != std::string::npos) ++kept;
      const size_t skip = kept > kMaxShown ? kept - kMaxShown : 0;
      size_t seen = 0;
      for (const auto& line : log_lines) {
        if (filter[0] && line.find(filter) == std::string::npos) continue;
        if (seen++ < skip) continue;
        joined += line;
        if (joined.empty() || joined.back() != '\n') joined += '\n';
      }
      flat.assign(joined.begin(), joined.end());
      flat.push_back('\0');
      scroll_to_end = follow;
    }
    if (flat.empty()) flat.push_back('\0');

    if (ImGui::Button("Copier tout")) ImGui::SetClipboardText(flat.data());
    ImGui::SameLine();
    if (ImGui::Button("Vider")) {
      LogLineBuffer::instance().Clear();
      last_count = static_cast<size_t>(-1);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Suivre", &follow);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##logfilter", "filtre (sous-chaîne)", filter,
                             sizeof(filter));
    ImGui::SameLine();
    ImGui::TextDisabled("(sélection souris · Ctrl+A · Ctrl+C)");

    // ⚠ ReadOnly : ImGui n'écrit pas dans le tampon, mais il gère la sélection et
    // la copie exactement comme un champ éditable. `AllowTabInput` reste OFF pour
    // que Tab continue de naviguer.
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_ReadOnly;
    if (scroll_to_end) flags |= ImGuiInputTextFlags_CallbackAlways;
    ImGui::InputTextMultiline(
        "##logs", flat.data(), flat.size(), ImVec2(-FLT_MIN, -FLT_MIN), flags,
        [](ImGuiInputTextCallbackData* data) -> int {
          // Suivi du bas : on pose le CURSEUR en fin de tampon et ImGui fait
          // défiler pour le garder visible. C'est le seul levier de défilement
          // exposé pour un champ multiligne — son enfant scrollable n'est pas
          // adressable de l'extérieur.
          data->CursorPos = data->BufTextLen;
          data->SelectionStart = data->SelectionEnd = data->CursorPos;
          return 0;
        });
    scroll_to_end = false;
  }

  ImGui::End();
}
