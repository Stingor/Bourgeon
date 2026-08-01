#include "ragnarok/game_mode.h"

#include "bourgeon.h"
#include "imgui.h"
#include "ragnarok/mode_mgr.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// On some clients (20250716+) the "ProcessInput" address is actually
// CMode::SendMsg(int msg, int p1, int p2, int p3, int p4) — a __thiscall
// taking 5 stack args with callee cleanup (RET 0x14). Hooking it with the
// no-arg member hook below corrupts the caller's stack on early return (the
// caller's pushed args are left behind and its epilog pops them into
// EBX/ESI/EDI), crashing the game. For those clients the configuration sets
// ProcessInputArgs: 5 and we install this signature-correct hook instead.
// __fastcall with 5 stack params also emits RET 0x14, keeping the stack
// balanced on both the blocking and forwarding paths.
using ProcessInputMsg_t = int(__fastcall*)(void* ecx, void* edx, int msg,
                                           int p1, int p2, int p3, int p4);
static ProcessInputMsg_t g_orig_process_input_msg = nullptr;

// CMode::SendMsg (the ProcessInput target @0x00c86740) is the game's GENERAL,
// RE-ENTRANT message dispatch — used for real input AND internal data queries
// (e.g. SendMsg(9) = current map name, during world-map init). We do NOT suppress
// input here: ImGui input blocking is handled at the WndProc level (WindowProcHook
// in ragnarok_client.cc drops WM_* mouse/keyboard when the cursor is over ImGui).
// Suppressing SendMsg here returned NULL to those internal queries and crashed
// the world-map open on strlen(NULL). The depth counter only lets the queued-icon
// dispatch run once, at the outermost (top-level) call.
static int g_send_msg_depth = 0;
struct SendMsgDepthGuard {
  SendMsgDepthGuard() { ++g_send_msg_depth; }
  ~SendMsgDepthGuard() { --g_send_msg_depth; }
};

// Commandes de LANCEMENT de compétence. 0x45 impose la cible, 0x71 route par
// l'INF de la compétence ; dans les deux cas p1 est l'identifiant de compétence.
// (cf. la mémoire reference_cmode_sendmsg_use_skill)
static constexpr int kSendMsgUseSkillTargeted = 0x45;
static constexpr int kSendMsgUseSkillByInf    = 0x71;
// Entrée en MODE CIBLAGE (curseur de visée) : le case 0x48 pose l'état dans le
// CGameMode receveur (+0x408 mode, +0x40C id, +0x414 niveau) et attend le clic.
// ⚠ Dispatché DEPUIS le case 0x71 (routage par INF), donc à profondeur >= 2 —
// ne PAS le gater sur g_send_msg_depth == 1. Le pendant 0x47 annule le ciblage.
static constexpr int kSendMsgEnterSkillTargeting = 0x48;

// 🔴 Les deux commandes ne passent PAS la même chose en p1, et les confondre
// donnait un identifiant absurde (⏱ vu en jeu : « skill_id=1760536 » pour
// SA_CREATECON, qui vaut 1007 — 1760536 = 0x001AD858, une adresse de PILE).
//   0x45 : p1 = skillId, p2 = cible GID, p3 = niveau ;
//   0x71 : p1 = POINTEUR sur une CSkillInfo, p2 = niveau. L'id est à +8, l'INF à
//          +0xC, le niveau appris à +0x10 (cf. reference_cmode_sendmsg_use_skill).
// La barre de raccourcis emprunte 0x71 : c'était donc le cas le plus courant qui
// tombait à côté.
//
// ⚠ Lecture déportée dans sa propre fonction : le hook contient un
// SendMsgDepthGuard, un objet à destructeur — MSVC refuse tout __try dans une
// fonction qui exige un déroulement (C2712).
static int ReadSkillIdFromInfo(int info_ptr) {
  if (!info_ptr) return 0;
  __try {
    return *reinterpret_cast<const int*>(
        static_cast<uintptr_t>(static_cast<unsigned>(info_ptr)) + 8);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int __fastcall Hooked_ProcessInputMsg(void* ecx, void* edx, int msg,
                                             int p1, int p2, int p3, int p4) {
  SendMsgDepthGuard depth;
  if (g_send_msg_depth == 1) {  // top-level call only
    Bourgeon::Instance().OnProcessInput();  // dispatch queued icon clicks
    // Observation PURE des lancements de compétence — on ne modifie rien, on
    // note. C'est la seule source possible pour savoir quelle compétence a
    // ouvert une liste de fabrication : aucun des paquets de liste ne la porte,
    // et un même ZC 0x01AD sert quatre métiers différents
    // (cf. docs/make_item_list_re.md §2.2).
    // Le NIVEAU compte autant que l'id : c'est lui que le serveur range dans
    // `menuskill_val` et qui décide du contenu de la liste (une pharmacie de
    // niveau 5 ne propose pas la même chose qu'au niveau 1). Relancer à 1 une
    // compétence connue à 5 appauvrirait la liste sans rien dire.
    if (msg == kSendMsgUseSkillTargeted)
      Bourgeon::Instance().NotifySkillCast(p1, p3);
    else if (msg == kSendMsgUseSkillByInf)
      Bourgeon::Instance().NotifySkillCast(ReadSkillIdFromInfo(p1), p2);
  }
  // QuickCast : réagir APRÈS que le natif a armé le mode ciblage (0x48), l'état
  // (+0x408…) étant posé par le case lui-même. Quelle que soit la profondeur :
  // la barre de raccourcis y arrive via 0x71 -> 0x48 (depth 2).
  // ⚠ Le plugin RE-RENTRE ici : après avoir émis le lancement il appelle
  // SendMsg(0x47) pour quitter le ciblage, comme le fait le pipeline souris
  // natif. C'est sans danger — 0x47 ne redéclenche pas de 0x48, donc pas de
  // récursion — mais le compteur de profondeur doit rester en place.
  if (msg == kSendMsgEnterSkillTargeting) {
    const int ret = g_orig_process_input_msg(ecx, edx, msg, p1, p2, p3, p4);
    Bourgeon::Instance().NotifySkillTargeting(ecx);
    return ret;
  }
  return g_orig_process_input_msg(ecx, edx, msg, p1, p2, p3, p4);
}

GameMode::GameMode(const YAML::Node& game_mode_configuration) {
  using namespace hooking;

  // Hooks
  const auto onupdate_addr = game_mode_configuration["OnUpdate"];
  if (!onupdate_addr.IsDefined()) {
    throw std::exception("Missing required field 'OnUpdate' for CGameMode");
  }
  GameMode::OnUpdateRef = HookManager::Instance().SetHook(
      HookType::kJmpHook,
      reinterpret_cast<uint8_t*>(onupdate_addr.as<uint32_t>()),
      reinterpret_cast<uint8_t*>(void_cast(&GameMode::OnUpdateHook)));

  const auto processinput_addr = game_mode_configuration["ProcessInput"];
  if (!processinput_addr.IsDefined()) {
    throw std::exception("Missing required field 'ProcessInput' for CGameMode");
  }
  const auto processinput_args = game_mode_configuration["ProcessInputArgs"];
  if (processinput_args.IsDefined() && processinput_args.as<int>() == 5) {
    g_orig_process_input_msg = reinterpret_cast<ProcessInputMsg_t>(
        HookManager::Instance().SetHook(
            HookType::kJmpHook,
            reinterpret_cast<uint8_t*>(processinput_addr.as<uint32_t>()),
            reinterpret_cast<uint8_t*>(Hooked_ProcessInputMsg)));
  } else {
    GameMode::ProcessInputRef = HookManager::Instance().SetHook(
        HookType::kJmpHook,
        reinterpret_cast<uint8_t*>(processinput_addr.as<uint32_t>()),
        reinterpret_cast<uint8_t*>(void_cast(&GameMode::ProcessInputHook)));
  }
}

void GameMode::OnUpdateHook() {
  ModeMgr::FireModeSwitch(ModeMgr::ModeType::kGame);
  // Heartbeat: this hook runs only while CGameMode is the actively-updating mode
  // (the in-world game loop), so it is Bourgeon's authoritative "in game" signal
  // — more reliable than the mode-switch event, which the char-change path does
  // not always re-fire. RenderUI() gates plugin UI on its freshness.
  Bourgeon::Instance().NotifyGameUpdate();
  Bourgeon::Instance().OnTick();
  return OnUpdateRef(this);
}

void GameMode::ProcessInputHook() {
  SendMsgDepthGuard depth;
  if (g_send_msg_depth == 1)  // top-level call only
    Bourgeon::Instance().OnProcessInput();  // dispatch queued icon clicks
  return ProcessInputRef(this);
}

// References
MethodRef<GameMode, void (GameMode::*)()> GameMode::OnUpdateRef;
MethodRef<GameMode, void (GameMode::*)()> GameMode::ProcessInputRef;
