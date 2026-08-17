#include "bourgeon.h"

#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include <cstdio>  // std::snprintf (compteur de lignes de la fenêtre de logs)

#include "imgui.h"
#include "ragnarok/skill_cooldowns.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"  // mui::WheelGateNewFrame (verrou molette anti-défilement)
#include "ui/window_clamp.h"
#include "ui/window_zorder.h"
#include "features/systems/auto_login.h"
#include "features/systems/moonlight_auth.h"
#include "features/windows/char_select.h"
#include "features/patches/chat.h"
#include "features/systems/cheat_detector.h"
#include "features/systems/discord_relay.h"
#include "features/systems/image_preview.h"
#include "features/overlays/basic_info.h"
#include "features/overlays/dps_meter.h"
#include "features/overlays/menu_icons.h"
#include "features/systems/integrity_check.h"
#include "features/systems/ui_caps.h"
#include "features/systems/dx7_warning.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/patches/status_tweaks.h"
#include "features/patches/berserk_chat_unlock.h"
#include "features/patches/inventory_tweaks.h"
#include "features/windows/inventory_viewer.h"
#include "features/windows/bank_window.h"
#include "features/windows/game_menu.h"
#include "features/windows/staff_tools.h"
#include "features/windows/game_settings.h"
#include "features/windows/hotkey_settings.h"
#include "features/windows/macro_window.h"
#include "features/windows/cart_viewer.h"
#include "features/patches/equip_tweaks.h"
#include "features/patches/window_pos_tweaks.h"
#include "features/overlays/status_icon_bar.h"
#include "features/overlays/minimap.h"
#include "features/overlays/quest_tracker.h"
#include "features/overlays/item_obtain_toast.h"
#include "features/fx/screen_fx.h"
#include "features/fx/zone_recorder.h"
#include "features/fx/weapon_layer.h"
#include "features/fx/weapon_dual_sprites.h"
#include "features/fx/hat_effect_depth.h"
#include "features/patches/skill_tree_tweaks.h"
#include "features/gameplay/fps_view.h"
#include "features/gameplay/keyboard_move.h"
#include "features/gameplay/quick_cast.h"
#include "features/gameplay/player_jump.h"
#include "features/hotkey_dispatch.h"
#include "features/minigames/doom.h"
#include "features/minigames/roggle.h"
#include "features/minigames/rojeweled.h"
#include "features/overlays/skill_bar.h"
#include "features/item_cell.h"  // itemcell::FlushDeferredDesc (desc au relâchement)
#include "features/windows/item_desc_window.h"
#include "features/windows/entity_context_menu.h"
#include "features/windows/entity_inspector.h"
#include "features/windows/monster_info_window.h"
#include "features/windows/navigation_window.h"
#include "features/fx/style_sync.h"
#include "features/windows/palette_editor.h"
#include "features/windows/pet_window.h"
#include "features/windows/storage_window.h"
#include "features/windows/cashshop_window.h"
#include "features/windows/npc_shop_window.h"
#include "features/windows/vending_window.h"
#include "features/windows/craft_atlas.h"
#include "features/windows/make_item_window.h"
#include "features/windows/weapon_refine_window.h"
#include "features/windows/trade_window.h"
#include "features/windows/chat_window.h"
#include "features/windows/rodex_window.h"
#include "features/windows/npc_dialog_window.h"
#include "features/systems/bug_report.h"
#include "features/systems/net_ping.h"
#include "features/windows/character_sheet.h"
#include "features/overlays/login_parade.h"
#include "features/overlays/cast_bar.h"
#include "features/overlays/chat_balloon.h"
#include "features/overlays/entity_names.h"
#include "utils/hooking/hook_manager.h"
#include "features/staff_gate.h"  // IsStaff() — gate de la fenêtre de logs
#include "utils/log_console.h"
#include "ragnarok/msgstring_override.h"
#include "utils/i18n.h"
#include "utils/startup_settings.h"  // police de l'interface, lue avant le login

Bourgeon::Bourgeon()
    : plugins_(), last_tick_count_(), log_lines_(), client_() {}

RagnarokClient& Bourgeon::client() { return client_; }
DiscordRelay* Bourgeon::discord_relay() { return discord_relay_; }
DpsMeter*     Bourgeon::dps_meter()     { return dps_meter_; }
BasicInfo* Bourgeon::basic_info() { return basic_info_; }
MenuIcons* Bourgeon::menu_icons()  { return menu_icons_; }
StatusIconBar* Bourgeon::status_icons() { return status_icons_; }
QuestTracker* Bourgeon::quest_tracker() { return quest_tracker_; }
Minimap* Bourgeon::minimap() { return minimap_; }
ItemObtainToast* Bourgeon::item_obtain_toast() { return item_obtain_toast_; }
ScreenFx* Bourgeon::screen_fx() { return screen_fx_; }
ZoneRecorder* Bourgeon::zone_recorder() { return zone_recorder_; }
FpsView* Bourgeon::fps_view() { return fps_view_; }
PlayerJump* Bourgeon::player_jump() { return player_jump_; }
KeyboardMove* Bourgeon::keyboard_move() { return keyboard_move_; }
QuickCast* Bourgeon::quick_cast() { return quick_cast_; }
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
GameMenu* Bourgeon::game_menu() { return game_menu_; }
StaffTools* Bourgeon::staff_tools() { return staff_tools_; }
HotkeySettings* Bourgeon::hotkey_settings() { return hotkey_settings_; }
MacroWindow* Bourgeon::macro_window() { return macro_window_; }
GameSettings* Bourgeon::game_settings() { return game_settings_; }
CashShopWindow* Bourgeon::cashshop_window() { return cashshop_window_; }
NpcShopWindow* Bourgeon::npc_shop_window() { return npc_shop_window_; }
VendingWindow* Bourgeon::vending_window() { return vending_window_; }
WeaponRefineWindow* Bourgeon::weapon_refine_window() { return weapon_refine_window_; }
MakeItemWindow* Bourgeon::make_item_window() { return make_item_window_; }
CraftAtlas* Bourgeon::craft_atlas() { return craft_atlas_; }
TradeWindow* Bourgeon::trade_window() { return trade_window_; }
ChatWindow* Bourgeon::chat_window() { return chat_window_; }
RodexWindow* Bourgeon::rodex_window() { return rodex_window_; }
NpcDialogWindow* Bourgeon::npc_dialog_window() { return npc_dialog_window_; }
MoonlightAuth* Bourgeon::moonlight_auth() { return moonlight_auth_; }
CharSelect* Bourgeon::char_select() { return char_select_; }
BugReport* Bourgeon::bug_report() { return bug_report_; }
CharacterSheet* Bourgeon::character_sheet() { return character_sheet_; }
LoginParade* Bourgeon::login_parade() { return login_parade_; }
ItemDescWindow* Bourgeon::item_desc() { return item_desc_; }
MonsterInfoWindow* Bourgeon::monster_info() { return monster_info_; }
NavigationWindow* Bourgeon::navigation_window() { return navigation_window_; }
PetWindow* Bourgeon::pet_window() { return pet_window_; }
PaletteEditor* Bourgeon::palette_editor() { return palette_editor_; }
EntityContextMenu* Bourgeon::entity_context_menu() {
  return entity_context_menu_;
}
EntityInspector* Bourgeon::entity_inspector() { return entity_inspector_; }
EntityNames* Bourgeon::entity_names() { return entity_names_; }
ChatBalloon* Bourgeon::chat_balloon() { return chat_balloon_; }
CastBar* Bourgeon::cast_bar() { return cast_bar_; }

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

// ── Filtre de messages système du chat — DÉMÉNAGÉ ───────────────────────────
// Il vivait ici : un détour de `UIWindowMgr_ChatAction` (0x00a4ad20) qui masquait
// trois messages système en neutralisant l'argument `action` sur la pile. Cette
// fonction est aussi LE chokepoint par lequel passe toute ligne de chat, et le
// remplacement ImGui de la chatbox doit l'intercepter (features/windows/
// chat_window.cc). Or il n'y a qu'un seul jeu d'octets à détourner à cette
// adresse : deux détours concurrents, c'est le second posé qui gagne, et le
// premier redevient silencieusement inopérant. Les deux besoins vivent donc dans
// le MÊME stub, chez ChatWindow, avec la liste `kBlockedMsgs`.
}  // namespace

bool Bourgeon::Initialize() {
  LogInfo("Bourgeon {}\n", BOURGEON_VERSION);

  // 🔴 LA LANGUE EN PREMIER, avant les plugins et donc avant le moindre dessin.
  // Elle vivait dans bourgeon_settings.yaml, que MoonlightUi ne relit qu'à la
  // transition vers le mode jeu : l'écran de login et le char-select sortaient
  // toujours en français, quel que soit le réglage — les deux écrans que voit
  // d'abord le joueur à qui la traduction s'adresse.
  i18n::LoadLanguageSetting();

  // Et dans la foulée, la table de messages du CLIENT — ses 4360 textes, dont
  // les 4300 que Bourgeon n'affiche pas lui-même mais que le joueur lit tout le
  // temps. Le détour est posé ici, une fois, avant que le client ait la moindre
  // occasion de demander un message : `msgoverride::Reload` est ensuite rejoué à
  // chaque changement de langue. Cf. ragnarok/msgstring_override.h.
  msgoverride::Reload();

  // La police de l'interface voyage avec elle, et pour la même raison : l'écran
  // de login la propose juste à côté du choix de la langue, et
  // `bourgeon_settings.yaml` n'aurait rendu son verdict qu'une fois cet écran
  // passé. Rien n'est dessiné à cet instant — `SetUiFontFamily` se contente de
  // mémoriser, et `ro::LoadKoreanFont` appliquera la sélection quand il bâtira
  // l'atlas.
  ro::SetUiFontFamily(startup::UiFontFamily(ro::UiFontFamily()));

  // L'échelle de l'interface suit le même chemin, et pour les mêmes raisons :
  // elle doit valoir dès la première fenêtre dessinée, donc dès le login. Ici
  // encore il n'y a pas de contexte ImGui — `SetUiScalePercent` mémorise, et
  // c'est `ro::ApplyUiScale()`, juste après `ImGui::CreateContext()`
  // (ragnarok_client.cc), qui la posera sur le style.
  ro::SetUiScalePercent(startup::UiScalePercent(ro::UiScalePercent()));

  if (!client_.Initialize()) {
    LogError("Bourgeon failed to initialize");
    return false;
  }

  PatchSilenceEmotePurchaseMsg();  // supprime le spam "purchased emotion" au login
  // (le filtre de messages système du chat est posé par ChatWindow — cf. ci-dessus)

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

  // Aperçus d'images : téléversement des téléchargements finis, purge au reset de
  // device, bornage du cache. 🔴 ICI et pas au rendu : Tick relâche des textures,
  // et relâcher pendant une frame ImGui casse le flush (une draw-list les
  // référence encore). OnTick tourne hors frame — c'est la seule place sûre.
  imgprev::Tick();

  for (auto& plugin : plugins_) {
    try {
      plugin->OnTick();
    } catch (const std::exception& error) {
      LogError("[{}] OnTick: {}", plugin->name(), error.what());
    }
  }
}

// Battement par FRAME, hors frame ImGui — la contrepartie fine d'OnTick, pour ce
// qui ne supporte ni son bridage à ~100 ms ni le caractère événementiel
// d'OnProcessInput. Le dispatch reste NOMINATIF (pas de boucle sur les plugins) :
// c'est un chemin chaud, et seul ce qui le demande explicitement doit le payer.
void Bourgeon::OnGameFrame() {
  // QuickCast : la répétition d'un OBJET tant que la touche est maintenue. Sa
  // cadence descend à ~20 ms pour le staff (le serveur ramène `canuseitem_tick`
  // à 20 ms au-dessus du niveau de groupe 40), donc un battement de 100 ms
  // aurait plafonné le réglage cinq fois trop haut sans rien en dire.
  if (auto* qc = quick_cast()) qc->UpdateItemRepeat();

  // 🔴 Bulles de chat : adoption du texte natif ET destruction de la fenêtre,
  // ici et PAS dans OnRenderUI. Deux raisons qui se cumulent :
  //   · `QueueDestroyWindow` est un appel natif, interdit pendant une frame
  //     ImGui (freeze muet) ;
  //   · surtout, OnRenderUI arrive APRÈS que le jeu a dessiné. La fenêtre
  //     native restait donc visible une frame entière — un rectangle qui
  //     clignotait derrière la bulle. Ce battement-ci passe AVANT le dessin.
  // OnTick (~100 ms) était encore pire : plusieurs frames de clignotement.
  if (auto* cb = chat_balloon()) cb->OnGameFramePulse();

  // Barres d'incantation : même raison d'être ici. Le masquage de la fenêtre
  // native (drapeau +0x28) doit précéder le dessin du jeu, sinon elle reste
  // visible une frame. Le relevé des horodatages d'incantation se fait dans la
  // foulée, sous la même garde.
  if (auto* cast = cast_bar()) cast->OnGameFramePulse();

  // Minimap : même famille de raison. Le client RECRÉE son radar (fenêtre 14) à
  // chaque entrée de carte ; le refermer au rythme d'OnTick le laissait vivre
  // un bon dixième de seconde à chaque téléport. ⚠ Ce battement ne peut pas
  // pour autant tout couvrir : il passe en TÊTE de frame, la recréation a lieu
  // au milieu de la précédente. La frame résiduelle est refusée ailleurs, par
  // un détour sur le site d'appel du dessin natif (features/overlays/minimap.cc,
  // `kDrawMiniMapCall`). Ici on ne fait que détruire, et vider la bascule de
  // fenêtre du menu (carte du monde, navigation) : une fenêtre lourde ouverte
  // depuis OnTick tombe sur une frame quelconque, et plante par intermittence.
  if (auto* mm = minimap()) mm->OnGameFramePulse();
}

// 🔴 Le décodage des paquets, rejoué sur le fil PRINCIPAL pour tous les modules.
//
// ⚠ Appelé à CHAQUE frame depuis les hooks OnUpdate des modes, PAS seulement
// depuis OnProcessInput. La raison est un piège de nommage : sur le client
// 20250716, la configuration donne `ProcessInput: 0x00c86740`, mais cette adresse
// est CMode::SendMsg — le dispatcher de messages GÉNÉRAL du jeu, appelé sur
// ÉVÉNEMENT (un clic sol, un lancement de compétence, une requête interne), et non
// une phase de frame comme sur les clients plus anciens. Les modules dont l'UI est
// entièrement en ImGui n'en émettent aucun : nos propres CZ partent par
// SendPacketRef, et les clics sont mangés par ImGui avant le WndProc du jeu.
//
// ⏱ Symptôme observé en jeu (dialogue NPC ImGui) : le script n'avançait plus tant
// qu'on ne cliquait pas HORS de la fenêtre — ce clic-là traversait jusqu'au jeu,
// déclenchait un SendMsg, et débloquait d'un coup toute la file. « Pas toujours »,
// parce que n'importe quel autre SendMsg (déplacement, attaque) faisait l'appoint.
void Bourgeon::DrainNetInboxes() {
  if (draining_inboxes_) return;  // ré-entrance : cf. bourgeon.h
  draining_inboxes_ = true;
  for (auto& plugin : plugins_) {
    try {
      plugin->DrainNetInbox();
    } catch (const std::exception& error) {
      LogError("[{}] DrainNetInbox: {}", plugin->name(), error.what());
    } catch (...) {
      LogError("[{}] DrainNetInbox: exception non standard", plugin->name());
    }
  }
  draining_inboxes_ = false;
}

void Bourgeon::OnProcessInput() {
  // ⚠ Le nom ment sur ce client : ce n'est pas une phase de frame mais
  // CMode::SendMsg, donc un point d'entrée ÉVÉNEMENTIEL (cf. DrainNetInboxes
  // ci-dessus). Ce qui suit y reste quand même : une commande native rejouée ici
  // part avec le timing et le contexte d'un clic natif — OnTick, bridé à ~100 ms,
  // la décalait sur une frame au hasard, ce qui faisait crasher par intermittence
  // les fenêtres lourdes (carte du monde).

  // Les paquets d'abord : quand le joueur AGIT, autant ne pas attendre la frame
  // suivante. La frame, elle, draine de son côté — c'est elle le filet.
  DrainNetInboxes();

  if (auto* mi = menu_icons()) mi->FlushPending();
  // Chatbox ImGui : MÊME raison. Envoyer un message rejoue le pipeline natif
  // complet (table de commandes, CMode::SendMsg), qui peut ouvrir une modale
  // BLOQUANTE — à ne jamais déclencher entre NewFrame() et Render().
  if (auto* cw = chat_window()) cw->FlushPending();
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
  // Fiche de monstre : le clic sur une compétence ouvre la fenêtre de description
  // NATIVE (0x2E) — MakeWindow + OnMsg, à ne pas jouer depuis une frame ImGui.
  if (auto* mif = monster_info()) mif->FlushPending();
  // Menu contextuel d'entité : MÊME raison, et la plus tranchante de toutes —
  // « nourrir le pet » et « nourrir l'homoncule » ouvrent une boîte de dialogue
  // native BLOQUANTE (docs/entity_context_menu_re.md §6.3, codes 29 et 38).
  if (auto* ecm = entity_context_menu()) ecm->FlushPending();
  // Fiche de pet : MÊME raison, et la même boîte — `SendMsg(150, 1)` est le
  // chemin par lequel « nourrir » ouvre sa confirmation native (docs/pet_re.md
  // §12.2). S'y ajoute l'ouverture de la fenêtre d'évolution (MakeWindow 261).
  if (auto* pw = pet_window()) pw->FlushPending();
  // Déplacement clavier : ici AUSSI (pas seulement dans OnRenderUI) pour qu'il
  // survive au « cacher l'interface » natif (F11), qui coupe la passe UI des
  // plugins. Auto-limité dans le temps -> aucun doublon de demande.
  if (auto* km = keyboard_move()) km->Update();
  // QuickCast : MÊME raison. Le client ignore l'auto-répétition clavier (le
  // dispatch hotkey ne tourne que sur un appui frais), donc la répétition d'un
  // sort touche maintenue est NOTRE boucle — elle doit survivre à F11.
  if (auto* qc = quick_cast()) qc->Update();
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

void Bourgeon::NotifySkillTargeting(void* cmode) {
  // Simple relais depuis le hook de CMode::SendMsg (cmd 0x48). Chemin chaud et
  // ré-entrant : QuickCast lit l'état de ciblage, émet le lancement par les
  // messages d'acteur du clic natif, puis rappelle SendMsg(0x47) pour désarmer.
  if (auto* qc = quick_cast()) qc->OnEnterTargeting(cmode);
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
  // Un changement d'échelle de l'interface demandé à la frame précédente se pose
  // ICI, et nulle part ailleurs : c'est le seul instant de la frame où le style
  // ImGui n'est sous aucun PushStyleVar. Avant le clamp, qui doit mesurer les
  // fenêtres à leur taille neuve. Voir ui/ro_imgui.h.
  ro::ApplyPendingUiScale();

  // Aucune fenêtre ImGui ne sort de l'écran de jeu. En PREMIÈRE ligne, avant tout
  // return anticipé et avant le moindre Begin de plugin : on est juste après
  // ImGui::NewFrame() (les deux chemins de rendu, DX7 et DX9, appellent RenderUI
  // là), donc le déplacement souris de la frame est déjà appliqué et la position
  // corrigée est celle qui sera dessinée. Écrans de login inclus (le bloc
  // OnRenderLoginUI ci-dessous dessine aussi des fenêtres). Voir ui/window_clamp.h.
  //
  // L'aimant passe AVANT le clamp, et pas l'inverse : il peut pousser une fenêtre
  // d'un ou deux pixels hors de l'écran en la collant à une voisine, et c'est au
  // clamp de dire le dernier mot. Il tourne à chaque frame, y compris éteint : il
  // y recycle la liste des fenêtres aimantables.
  ro::SnapMovingWindowToPeers();
  ro::KeepWindowsOnScreen();
  // Le HUD (barre de skills, barres HP/SP/XP/zeny/poids, portrait) reste sous
  // toutes les vraies fenêtres. Même endroit et mêmes raisons que le clamp
  // ci-dessus : après NewFrame, avant le premier Begin. Voir ui/window_zorder.h.
  ro::SendBackgroundWindowsToBack();
  // Qui a droit à la molette cette frame ? Même fenêtre de tir que les deux appels
  // ci-dessus, et pour une raison voisine : ImGui vient d'appliquer le défilement
  // dans NewFrame, et c'est la seule occasion de le constater avant que le premier
  // widget ne réclame la molette. Avant le `return` de chargement de carte et celui
  // du hors-jeu : le char-select a lui aussi des zones qui tournent à la molette.
  // Voir ui/ro_widgets.h.
  mui::WheelGateNewFrame();
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
  if (show_log_window_ && IsStaff()) ShowLogWindow();

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

// Aiguillage d'une ligne écrite par NOUS (UIM_PUSHINTOCHATHISTORY). Il vit ici et
// non dans le hook : `ragnarok/` ne connaît pas les features, et n'a pas à les
// connaître. Bourgeon, lui, tient déjà les deux bouts.
bool Bourgeon::RouteChatLine(const char* text, uint32_t rgb) {
  return chatwnd::IngestPluginLine(text, rgb);
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

void Bourgeon::RegisterReplaceOpcode(
    uint16_t opcode, std::function<bool(const uint8_t*, uint16_t)> claim) {
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
    moonlight_auth_ = moonlight_auth;  // le hook de WndProc l'interroge (WantsKeyboard)
    plugins_.emplace_back(std::move(ma));
  }
  // Remplacement ImGui du char-select (activé par défaut ; opt-out yaml
  // char_select.imgui:false). Réservé au parcours de login Moonlight (gate via
  // moonlight_auth). Socle du lobby unifié ; voir char_select.h / charselect_re.md.
  {
    auto char_select = std::make_unique<CharSelect>(moonlight_auth);
    char_select_ = char_select.get();  // le hook de WndProc l'interroge aussi
    plugins_.emplace_back(std::move(char_select));
  }
  plugins_.emplace_back(std::make_unique<IntegrityCheck>());
  // Annonce au serveur les interfaces modernes ALLUMÉES, pour que les scripts NPC
  // n'envoient du markup Bourgeon qu'à un client qui le rendra (cf. ui_caps.h).
  // Après IntegrityCheck : il n'envoie rien tant que le serveur ne l'a pas reconnu.
  plugins_.emplace_back(std::make_unique<UiCaps>());
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
    // Mesure de latence : aucun accesseur, tout son état est statique — le seul
    // lecteur est l'overlay FPS, qui appelle `NetPing::LastMs()`.
    plugins_.emplace_back(std::make_unique<NetPing>());
  }
  {
    auto chat_tweaks = std::make_unique<ChatTweaks>();
    chat_tweaks_ = chat_tweaks.get();
    plugins_.emplace_back(std::move(chat_tweaks));

    // La chatbox ImGui. Son constructeur pose LE détour de ChatAction, qui porte
    // aussi le filtre de messages système autrefois installé par Initialize() :
    // il doit donc exister avant le premier message du login (LoadPlugins tourne
    // au chargement de la DLL, bien avant l'entrée en jeu).
    auto chat_window = std::make_unique<ChatWindow>();
    chat_window_ = chat_window.get();
    plugins_.emplace_back(std::move(chat_window));
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
    // Menu Échap (« Game Options », id 155) : remplace la native, qui est masquée à
    // la naissance puis DÉTRUITE au tick. C'est lui qui commande l'accès à Game
    // Settings et à Shortcut Settings — leur seul chemin d'ouverture.
    auto game_menu = std::make_unique<GameMenu>();
    game_menu_ = game_menu.get();
    plugins_.emplace_back(std::move(game_menu));
  }
  {
    // Outils du staff, sortis du panneau de réglages : ce sont des outils de
    // travail, pas des préférences (cf. features/windows/staff_tools.h).
    auto staff_tools = std::make_unique<StaffTools>();
    staff_tools_ = staff_tools.get();
    plugins_.emplace_back(std::move(staff_tools));
  }
  {
    // Table des raccourcis (« Shortcut Settings », id 156). Écriture comprise :
    // elle passe par les ponts Lua du client (userhotkey::WriteBinding + Save).
    auto hotkey_settings = std::make_unique<HotkeySettings>();
    hotkey_settings_ = hotkey_settings.get();
    plugins_.emplace_back(std::move(hotkey_settings));
  }
  {
    // Les dix macros de chat (« Shortcut List », UIEmotionWnd id 86, Alt+M).
    // Enregistrée APRÈS l'écran des raccourcis : elle lui délègue la réaffectation
    // de ses touches, et l'ordre d'enregistrement est celui du rendu.
    auto macro_window = std::make_unique<MacroWindow>();
    macro_window_ = macro_window.get();
    plugins_.emplace_back(std::move(macro_window));
  }
  {
    // Réglages du jeu (« Game Settings », id 0x271E). Les trois onglets pilotés
    // par la table d'options du client, plus le son ; graphismes et skin restent
    // au natif, derrière un bouton.
    auto game_settings = std::make_unique<GameSettings>();
    game_settings_ = game_settings.get();
    plugins_.emplace_back(std::move(game_settings));
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

    // Atlas des recettes : la vue d'ensemble que la fenêtre ci-dessus ne peut pas
    // donner — elle ne montre que ce que le serveur vient de proposer, après un
    // lancement de compétence. L'Atlas, lui, se lit sans rien lancer.
    auto craft_atlas = std::make_unique<CraftAtlas>();
    craft_atlas_ = craft_atlas.get();
    plugins_.emplace_back(std::move(craft_atlas));
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
  // Aucun accesseur : personne ne l'interroge. Il écoute le clavier et joue les
  // actions du catalogue, dont l'état vit dans hotkey_actions.
  plugins_.emplace_back(std::make_unique<HotkeyDispatch>());
  {
    auto quick_cast = std::make_unique<QuickCast>();
    quick_cast_ = quick_cast.get();
    plugins_.emplace_back(std::move(quick_cast));
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

    // Fiche de monstre : remplace « Monster Info » (id 0x4D, skill Sense) et
    // sert de cible aux liens de monstre posés ailleurs (table des drops d'une
    // fiche d'item). Son constructeur revendique le paquet 0x018C — il doit donc
    // exister AVANT le premier Sense de la session.
    auto monster_info = std::make_unique<MonsterInfoWindow>();
    monster_info_ = monster_info.get();
    plugins_.emplace_back(std::move(monster_info));

    // Navigation ImGui (chercher une carte / un NPC / un monstre, puis se faire
    // guider). Elle REMPLACE les quatre fenêtres natives — la 203 et ses
    // satellites 306 / 314 / 229 — qui sont détruites au tick, et pilote le
    // moteur `CNavigation` (0x015C3090) sans rien en réimplémenter.
    // Cf. docs/navigation_re.md.
    auto navigation_window = std::make_unique<NavigationWindow>();
    navigation_window_ = navigation_window.get();
    plugins_.emplace_back(std::move(navigation_window));

    // Fiche de pet : remplace UIPetInfoWnd (88) et son menu de commandes (260).
    // Avant le menu contextuel, dont l'entrée « Statut du pet » aboutit ici par
    // le routage de MakeWindow. Cf. docs/pet_re.md.
    auto pet_window = std::make_unique<PetWindow>();
    pet_window_ = pet_window.get();
    plugins_.emplace_back(std::move(pet_window));

    // Éditeur de couleurs du personnage (Alt+P). Son OnRenderUI pose aussi,
    // paresseusement, les détours d'injection de palette — depuis le fil de
    // rendu, seul moment où les fonctions visées ne sont pas en cours
    // d'exécution.
    auto palette_editor = std::make_unique<PaletteEditor>();
    palette_editor_ = palette_editor.get();
    plugins_.emplace_back(std::move(palette_editor));

    // Propagation des couleurs : envoie la recette du joueur et applique celles
    // des autres. Séparé de l'éditeur exprès — il doit tourner même si le joueur
    // n'ouvre jamais la fenêtre, sans quoi personne ne verrait personne.
    plugins_.emplace_back(std::make_unique<StyleSync>());

    // Menu du clic droit sur une entité. Son constructeur pose LE détour de
    // GameMode_ShowEntityContextMenu — le seul chemin qui produise le menu du
    // monde (docs/entity_context_menu_re.md §9). Après la fiche de monstre :
    // une de ses entrées l'ouvre.
    auto entity_context_menu = std::make_unique<EntityContextMenu>();
    entity_context_menu_ = entity_context_menu.get();
    plugins_.emplace_back(std::move(entity_context_menu));

    // Inspecteur de propriétés (staff). Aucun détour, aucun paquet revendiqué :
    // il ne fait que lire, et n'existe que parce qu'une entrée du menu
    // ci-dessus l'ouvre — d'où sa place juste après.
    auto entity_inspector = std::make_unique<EntityInspector>();
    entity_inspector_ = entity_inspector.get();
    plugins_.emplace_back(std::move(entity_inspector));
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
    auto minimap = std::make_unique<Minimap>();
    minimap_ = minimap.get();
    plugins_.emplace_back(std::move(minimap));
  }
  {
    auto item_obtain_toast = std::make_unique<ItemObtainToast>();
    item_obtain_toast_ = item_obtain_toast.get();
    plugins_.emplace_back(std::move(item_obtain_toast));
  }
  {
    auto screen_fx = std::make_unique<ScreenFx>();
    screen_fx_ = screen_fx.get();
    plugins_.emplace_back(std::move(screen_fx));
  }
  {
    // Enregistreur de zone -> GIF (staff). Capture le backbuffer APRÈS l'overlay,
    // via le callback de fin de frame du hook Present.
    auto zone_recorder = std::make_unique<ZoneRecorder>();
    zone_recorder_ = zone_recorder.get();
    plugins_.emplace_back(std::move(zone_recorder));
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
  {
    // Bulle de chat au-dessus des têtes. Son constructeur POSE DEUX DÉTOURS
    // (capture du texte + mise en silence du rendu natif) : il doit donc être
    // construit après la chatbox, dont il consomme le parseur de balises.
    auto chat_balloon = std::make_unique<ChatBalloon>();
    chat_balloon_ = chat_balloon.get();
    plugins_.emplace_back(std::move(chat_balloon));
  }
  {
    // Barre d'incantation. Aucun détour : son constructeur ne fait qu'observer
    // trois opcodes, pour récupérer le skillId que le client jette avant de
    // créer sa barre (cf. features/overlays/cast_bar.h).
    auto cast_bar = std::make_unique<CastBar>();
    cast_bar_ = cast_bar.get();
    plugins_.emplace_back(std::move(cast_bar));
  }

  for (const auto& plugin : plugins_) {
    // LogInfo("Loaded plugin: {}", plugin->name());
  }
}

void Bourgeon::ShowLogWindow() {
  ImGui::SetNextWindowSize(ImVec2(760.0f, 430.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(100.0f, 100.0f),
      ImVec2(ImGui::GetIO().DisplaySize.x* 0.8f, ImGui::GetIO().DisplaySize.y * 0.8f));
  if (!ImGui::Begin("Logs", &show_log_window_)) {
    ImGui::End();
    return;
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
  constexpr size_t kMaxShown = 2000;

  static std::vector<LogLine> log_lines;
  static std::vector<char> flat;      // tampon NUL-terminé pour ImGui
  static size_t last_count = static_cast<size_t>(-1);
  static char   filter[64] = {0};
  static char   last_filter[64] = {0};
  static bool   follow = true;
  static bool   scroll_to_end = false;

  // ── Filtre par NIVEAU ────────────────────────────────────────────────────────
  // Les macros du projet ne couvrent que trois niveaux spdlog effectifs, et le
  // joueur qui cherche « logwarn » doit les retrouver sous leurs noms de MACRO,
  // pas sous ceux de spdlog :
  //   LogInfo  → info      LogDiag → warn      LogError → error
  // 🔴 Il n'existe PAS de LogWarn distinct : LogDiag EST le niveau warn (cf.
  // utils/log_console.h). Une case « Diag » et une case « Warn » séparées
  // mentiraient — c'est un seul et même bouton, étiqueté pour les deux noms.
  // LogDebug est compilé hors des builds release : sa case n'existe que là où
  // des lignes de debug peuvent réellement arriver, plutôt que de proposer un
  // filtre qui ne peut jamais rien montrer.
  static bool lvl_debug = true;
  static bool lvl_info = true;
  static bool lvl_warn = true;
  static bool lvl_error = true;
  static int  last_mask = -1;
  static size_t kept_count = 0;
  const int mask = (lvl_debug ? 1 : 0) | (lvl_info ? 2 : 0) |
                   (lvl_warn ? 4 : 0) | (lvl_error ? 8 : 0);

  // trace et critical n'ont aucune macro dédiée, mais rien n'interdit à spdlog
  // d'en produire : on les rattache au voisin le plus proche pour qu'une ligne
  // ne puisse jamais devenir INVISIBLE quelles que soient les cases cochées.
  auto level_shown = [&](spdlog::level::level_enum lvl) {
    switch (lvl) {
      case spdlog::level::trace:
      case spdlog::level::debug: return lvl_debug;
      case spdlog::level::info:  return lvl_info;
      case spdlog::level::warn:  return lvl_warn;
      default:                   return lvl_error;  // err + critical
    }
  };

  LogLineBuffer::instance().Snapshot(&log_lines);

  // Le tampon n'est reconstruit que si quelque chose a changé : le remettre à
  // plat à chaque frame recopierait des centaines de kilo-octets pour rien, et
  // ferait sauter la sélection en cours sous la souris du joueur.
  const bool filter_changed = std::strcmp(filter, last_filter) != 0;
  if (log_lines.size() != last_count || filter_changed || mask != last_mask) {
    last_count = log_lines.size();
    last_mask = mask;
    std::strncpy(last_filter, filter, sizeof(last_filter) - 1);
    last_filter[sizeof(last_filter) - 1] = '\0';

    auto passes = [&](const LogLine& line) {
      return level_shown(line.level) &&
             (!filter[0] || line.text.find(filter) != std::string::npos);
    };

    std::string joined;
    size_t kept = 0;
    // Deux passes : on compte d'abord les lignes retenues pour ne garder que
    // les kMaxShown DERNIÈRES (la queue), pas les premières.
    for (const auto& line : log_lines)
      if (passes(line)) ++kept;
    kept_count = kept;
    const size_t skip = kept > kMaxShown ? kept - kMaxShown : 0;
    size_t seen = 0;
    for (const auto& line : log_lines) {
      if (!passes(line)) continue;
      if (seen++ < skip) continue;
      joined += line.text;
      if (joined.empty() || joined.back() != '\n') joined += '\n';
    }
    flat.assign(joined.begin(), joined.end());
    flat.push_back('\0');
    scroll_to_end = follow;
  }
  if (flat.empty()) flat.push_back('\0');

  if (ImGui::Button(i18n::Tr("Copier tout"))) ImGui::SetClipboardText(flat.data());
  ImGui::SameLine();
  if (ImGui::Button(i18n::Tr("Vider"))) {
    LogLineBuffer::instance().Clear();
    last_count = static_cast<size_t>(-1);
  }
  ImGui::SameLine();
  ImGui::Checkbox(i18n::Tr("Suivre"), &follow);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ro::Px(220.0f));
  ImGui::InputTextWithHint("##logfilter", i18n::Tr("filtre (sous-chaîne)"), filter,
                            sizeof(filter));
  ImGui::SameLine();
  ImGui::TextDisabled(i18n::Tr("(sélection souris · Ctrl+A · Ctrl+C)"));

  // Deuxième rangée : les niveaux. Chaque case porte la couleur que la ligne
  // aurait en console, pour qu'on la reconnaisse sans lire l'étiquette.
  auto level_box = [](const char* label, bool* on, ImVec4 color,
                      const char* tip) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::Checkbox(label, on);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine();
  };

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(i18n::Tr("Niveaux :"));
  ImGui::SameLine();
#ifdef BOURGEON_DEBUG
  level_box(i18n::Tr("Debug"), &lvl_debug, ImVec4(0.60f, 0.60f, 0.60f, 1.00f),
            i18n::Tr("LogDebug — compilé uniquement dans les builds de debug."));
#endif
  level_box(i18n::Tr("Info"), &lvl_info, ImVec4(0.80f, 0.88f, 0.80f, 1.00f),
            i18n::Tr("LogInfo — le déroulé normal du client."));
  level_box(i18n::Tr("Diag / Warn"), &lvl_warn, ImVec4(1.00f, 0.78f, 0.35f, 1.00f),
            i18n::Tr("LogDiag — même niveau que warn ; il n'y a pas de LogWarn "
                     "séparé."));
  level_box(i18n::Tr("Erreur"), &lvl_error, ImVec4(1.00f, 0.45f, 0.45f, 1.00f),
            i18n::Tr("LogError — ce qui a échoué."));

  // Le compte RETENU face au total : sans lui, un niveau décoché ressemble à un
  // journal vide, et on cherche la panne du mauvais côté.
  char counts[64];
  std::snprintf(counts, sizeof(counts), i18n::Tr("%zu / %zu lignes"), kept_count,
                log_lines.size());
  ImGui::TextDisabled("%s", counts);

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

  ImGui::End();
}
