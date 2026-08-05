#include "features/windows/entity_context_menu.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "imgui.h"

#include "bourgeon.h"
#include "features/staff_gate.h"
#include "features/windows/monster_info_window.h"
#include "ragnarok/globals.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

using namespace mui;

// ── Adresses natives (client 20250716, no-ASLR) ──────────────────────────────
// Tout est établi dans docs/entity_context_menu_re.md ; les numéros de section
// ci-dessous y renvoient.
namespace {

// §4 — le constructeur du menu natif du monde. Notre unique point d'interception.
constexpr uintptr_t kShowEntityContextMenu = 0x00c6e990;

// §6.2 — CMode::SendMsg, message 24 = « la ligne N du menu a été cliquée ».
constexpr int kMsgMenuItemChosen = 24;

// §2 — état des boutons de la frame. 3 = relâché CETTE frame.
constexpr uintptr_t kMouseLButtonState = 0x011e40e4;
constexpr uintptr_t kMouseRButtonState = 0x011e40e8;
constexpr int       kBtnPressed        = 1;  // appui NEUF
constexpr int       kBtnHeld           = 2;  // maintenu (drag)
constexpr int       kBtnReleased       = 3;  // relâché CETTE frame
constexpr int       kBtnRepeat         = 4;  // appui trop rapproché du précédent

// §4.1 — les gardes que le natif applique avant d'ouvrir : on les reprend telles
// quelles, sinon notre menu apparaîtrait là où le client refusait le sien.
constexpr uintptr_t kReplayActive  = 0x015beecc;  // lecture d'un replay
constexpr uintptr_t kStorageWndPtr = 0x0131f770;  // storage NATIF ouvert

// §4.3 — les trois champs du CGameMode que le menu écrit et que le dispatch relit.
constexpr int kGm_MenuCodes  = 0x1cc;  // std::vector<int> : begin/end/cap
constexpr int kGm_MenuTarget = 0x2e0;  // uint32 : AID de la cible
constexpr int kGm_ActorMgr   = 0x0cc;

// §8 — helpers natifs réutilisés.
constexpr uintptr_t kStdVectorIntPushBack = 0x007a7fa0;  // __thiscall(vec*, int*)
constexpr uintptr_t kActiveIdSetContains  = 0x00a727f0;  // __cdecl(aid) -> bool
constexpr uintptr_t kNameDictGetEntry     = 0x005a1460;  // __thiscall(dict, gid)
constexpr uintptr_t kPostActorClickAction = 0x00c753a0;  // __thiscall(gm, aid, flag)
constexpr uintptr_t kActorListFindByGid   = 0x00a69eb0;  // __thiscall(actorMgr, gid)
// Le prédicat d'adoption du client (`sub_D99860`) : niveau >= 70, non monté, en
// couple, cible éligible… C'est LUI qui décide de l'entrée « Adopter » dans le
// menu natif, et le dispatcher ne le rejoue pas — sans ce test, l'entrée partirait
// au serveur pour se faire refuser.
constexpr uintptr_t kAdoptionEligible     = 0x00d99860;  // __stdcall(aid) -> bool
// « Ceci n'est pas un vrai joueur » : type d'acteur ∈ {1, 6, 12} ou job de
// NPC/portail. Le natif s'en sert pour REFUSER tout menu (docs §4.1) — sans ce
// test, un PNJ scripté sous forme de joueur se verrait proposer « échanger ».
constexpr uintptr_t kIsHostileOrSpecial   = 0x00d9d220;  // __stdcall(aid, job) -> bool
constexpr int       kGm_NameDict          = 0x160;
constexpr int       kName_Str             = 0x04;   // std::string dans CNameInfo
constexpr int       kName_Size            = 0x14;
constexpr int       kName_Cap             = 0x18;

// Globaux de session lus pour décider quelles entrées ont un sens (§5.4).
constexpr uintptr_t kOwnAccountAid = 0x015fb9a4;
constexpr uintptr_t kOwnGuildId    = 0x0159c230;
constexpr uintptr_t kGuildIsMaster = 0x0159c23c;
constexpr uintptr_t kInPartyFlag   = 0x015ff804;
constexpr uintptr_t kOwnPetAid     = 0x015fb3b0;
constexpr uintptr_t kSessionAddr   = 0x015fa3c0;
// ⚠ Ces deux offsets sont ceux des comparaisons `*((int*)session + N)` du client,
// pas les noms qu'IDA leur a donnés : 5462*4 = 0x5558 et 5506*4 = 0x5608.
constexpr int kSess_HomunAid = 0x5558;  // GameMode_IsCurrentId5558
constexpr int kSess_MercAid  = 0x5608;  // GameMode_IsCurrentId5608

// ── Le groupe : `std::list<PartyMember>` à session+0x17B8 ────────────────────
// +0x17BC = le nœud sentinelle (`_Myhead`), +0x17C0 = la taille — c'est ce
// dernier que `Social_GetPartyMemberCount` (0x00d5cf50) rend tel quel, et dont
// ChatWindow se sert déjà pour autoriser le canal groupe.
// Nœud : next@0, prev@4, valeur@8 ; le NOM du membre est la `std::string` de
// valeur+0x0C, soit nœud+0x14 (taille +0x24, capacité +0x28).
//
// 🔴 On compare par NOM, pas par identifiant. La valeur porte deux champs
// numériques — `Social_FindPartyMemberByAid` (0x00d5d650) cherche sur
// valeur+0x04, `sub_D5D740` sur valeur+0x08 — et l'un des deux est un char_id :
// comparer valeur+0x04 à l'AID du menu ne reconnaît AUCUN membre (mesuré en
// jeu). Le natif tranche pareil : il a l'AID sous la main (`gm+0x2E0`) et va
// pourtant chercher le membre par son nom (`sub_D5D960`, comparaison
// insensible à la casse). Une clé numérique mal choisie ferait bien pire que ne
// rien griser : un char_id peut valoir l'AID de QUELQU'UN D'AUTRE.
constexpr int kSess_PartyListHead = 0x17bc;
constexpr int kPartyNode_Name     = 0x14;  // std::string : nœud+0x14
constexpr int kPartyNode_NameSize = 0x24;
constexpr int kPartyNode_NameCap  = 0x28;
constexpr int kPartyWalkGuard     = 64;    // un groupe plafonne à 12 : garde-fou

// Acteur : `vtable+0xC4` rend l'**id de guilde**. C'est l'appel exact que
// `GameMode_ShowEntityContextMenu` fait (`call [edx+0C4h]` @0x00c6f4e2) pour
// décider de proposer, ou non, l'invitation en guilde.
constexpr int kActorVt_GetGuildId = 0xc4;

// Type d'acteur (`acteur+0x314`) : 7 = objet au sol, 1/6/12 = hostile/spécial.
constexpr int kActor_Type = 0x314;

// §3 — catégories du quad de picking. La catégorie 0 (acteur ordinaire : joueur,
// monstre, PNJ scripté) n'a pas de constante : c'est le cas par défaut, tranché
// sur le job faute de mieux.
constexpr int kPickNpc        = 1;
constexpr int kPickSkillUnit  = 2;
constexpr int kPickGroundItem = 3;
constexpr int kPickSpecial    = 4;  // pet / homoncule / mercenaire / élémentaire

// §6.3 — codes d'action natifs (ceux qu'on rejoue).
constexpr int kCodeDeal        = 4;
constexpr int kCodePartyInvite = 5;
constexpr int kCodeAddFriend   = 10;
constexpr int kCodeBlockChat   = 12;
constexpr int kCodeUnblockChat = 13;
constexpr int kCodeWhisper     = 20;
constexpr int kCodeGuildInvite = 22;
constexpr int kCodeMannerPlus  = 23;
constexpr int kCodeMannerMinus = 24;
constexpr int kCodeGuildAlly   = 25;
constexpr int kCodeGuildFoe    = 26;
constexpr int kCodeKick        = 28;
constexpr int kCodePetFeed     = 29;
constexpr int kCodePetPerform  = 30;
constexpr int kCodePetToEgg    = 31;
constexpr int kCodePetUnequip  = 32;
constexpr int kCodePetStatus   = 33;
constexpr int kCodeShowGid     = 34;
constexpr int kCodeChatBanLog  = 35;
constexpr int kCodeAdopt       = 36;
constexpr int kCodeHomunStatus = 37;
constexpr int kCodeHomunFeed   = 38;
constexpr int kCodeHomunStandBy = 39;
constexpr int kCodeMercStatus  = 40;
constexpr int kCodeMercStandBy = 41;
constexpr int kCodeViewEquip   = 42;
constexpr int kCodeFullStrip   = 44;
constexpr int kCodeCopyCCode   = 57;
constexpr int kCodeSendMail    = 58;
constexpr int kCodeReportUser  = 63;
// ⛔ Code 65 (« Close stall », CZ 0x0AF9) VOLONTAIREMENT absent : moonlight ne
// déclare pas ce paquet, et clif_parse DÉCONNECTE sur un opcode inconnu — le GM
// qui cliquerait cette entrée se ferait kicker lui-même (docs §6.3).

// CZ_CONTACTNPC : parler à un NPC. 7 octets, comme le natif l'émet lui-même dans
// la branche « entité hostile/spéciale » de CursorMgr_UpdateHover (docs §7).
constexpr uint16_t kCzContactNpc = 0x0090;

using PushBackFn   = void   (__thiscall*)(void*, const int*);
using GetGuildIdFn = uint32_t (__thiscall*)(void*);
// ⚠ Deux conventions différentes, et confondre les deux décale la pile de 4
// octets à chaque appel : ActiveIdSet_Contains est __cdecl (l'appelant dépile),
// le prédicat d'adoption est __stdcall (l'appelée dépile).
using ContainsFn   = bool   (__cdecl*)(uint32_t);
using PredicateFn  = bool   (__stdcall*)(uint32_t);
using Predicate2Fn = int    (__stdcall*)(uint32_t, uint32_t);
using GetEntryFn   = void*  (__thiscall*)(void*, uint32_t);
using DispatchFn   = int    (__thiscall*)(void*, int, int, int, int, int);
using ClickFn      = int    (__thiscall*)(void*, uint32_t, int);
using FindActorFn  = void*  (__thiscall*)(void*, uint32_t);
using ShowMenuFn   = int    (__fastcall*)(void*, void*, int, int);

template <typename T>
inline T Read(const void* base, int off) {
  return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(base) + off);
}
inline int  ReadGlobalInt(uintptr_t a) { return *reinterpret_cast<int*>(a); }
inline void WriteGlobalInt(uintptr_t a, int v) { *reinterpret_cast<int*>(a) = v; }
inline void* ReadGlobalPtr(uintptr_t a) { return *reinterpret_cast<void**>(a); }

// L'AID est-il dans la liste `<admin>` du clientinfo.xml (docs §4.2) ? C'est ce
// que le DISPATCHER natif exige pour les codes 23/24/44 : sans cela, l'action
// est rejetée en silence, et proposer l'entrée serait mentir au joueur.
bool ClientAdminListContains(uint32_t aid) {
  __try {
    return reinterpret_cast<ContainsFn>(kActiveIdSetContains)(aid);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool AdoptionEligible(uint32_t aid) {
  __try {
    return reinterpret_cast<PredicateFn>(kAdoptionEligible)(aid);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool IsHostileOrSpecialUnit(uint32_t aid, uint32_t job) {
  __try {
    return reinterpret_cast<Predicate2Fn>(kIsHostileOrSpecial)(aid, job) == 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Nom brut d'une entité, tel que le dictionnaire du client le porte. Comme le
// natif, un GID inconnu déclenche la demande au serveur (le nom arrivera plus
// tard, et le menu affichera l'AID d'ici là).
//
// ⚠ SEH ⇒ AUCUN objet C++ dans cette fonction (C2712 : « __try dans une fonction
// qui exige un déroulement d'objet »). La std::string se construit chez
// l'appelant. C'est la raison de ce découpage en deux, pas un goût pour les
// buffers bruts.
bool ReadEntityNameRaw(void* game_mode, uint32_t aid, char* out, size_t out_size) {
  __try {
    void* dict = reinterpret_cast<uint8_t*>(game_mode) + kGm_NameDict;
    void* entry = reinterpret_cast<GetEntryFn>(kNameDictGetEntry)(dict, aid);
    if (!entry) return false;
    const void* str = reinterpret_cast<const uint8_t*>(entry) + kName_Str;
    const unsigned size = Read<unsigned>(entry, kName_Size);
    const unsigned cap  = Read<unsigned>(entry, kName_Cap);
    if (size == 0 || size >= out_size) return false;
    const char* src = (cap >= 16) ? Read<const char*>(str, 0)
                                  : reinterpret_cast<const char*>(str);
    if (!src) return false;
    memcpy(out, src, size);
    out[size] = '\0';
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::string EntityName(void* game_mode, uint32_t aid) {
  char buffer[64] = {};
  if (!ReadEntityNameRaw(game_mode, aid, buffer, sizeof(buffer)))
    return std::string();
  // Les noms voyagent dans l'encodage du client : on affiche en UTF-8.
  const char* utf8 = ro::WireToUtf8(buffer);
  return utf8 ? std::string(utf8) : std::string();
}

void* FindActor(void* game_mode, uint32_t aid) {
  __try {
    void* actor_mgr = Read<void*>(game_mode, kGm_ActorMgr);
    if (!actor_mgr) return nullptr;
    return reinterpret_cast<FindActorFn>(kActorListFindByGid)(actor_mgr, aid);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// La cible est-elle DÉJÀ dans notre groupe ? Le natif se posait la question
// (docs §5.4a) et retirait l'entrée ; on préfère la griser, mais la source est
// la même liste et la clé est la même : le NOM, comparé sans tenir compte de la
// casse. `wire_name` est le nom BRUT du dictionnaire — pas sa conversion UTF-8,
// qui ne correspondrait plus aux octets de la liste.
// Lecture seule, sans appel natif : parcourir la std::list circulaire coûte
// moins que d'appeler une fonction à retour de structure.
// ⚠ SEH ⇒ aucun objet C++ ici.
bool OwnPartyContains(const char* wire_name) {
  if (!wire_name || !*wire_name) return false;
  __try {
    void* session = reinterpret_cast<void*>(kSessionAddr);
    void** head = Read<void**>(session, kSess_PartyListHead);
    if (!head) return false;
    void** node = reinterpret_cast<void**>(*head);
    for (int guard = 0; node && node != head && guard < kPartyWalkGuard; ++guard) {
      const unsigned size = Read<unsigned>(node, kPartyNode_NameSize);
      const unsigned cap  = Read<unsigned>(node, kPartyNode_NameCap);
      const char* name =
          (cap >= 16) ? Read<const char*>(node, kPartyNode_Name)
                      : reinterpret_cast<const char*>(
                            reinterpret_cast<uint8_t*>(node) + kPartyNode_Name);
      if (name && size != 0 && _stricmp(name, wire_name) == 0) return true;
      node = reinterpret_cast<void**>(*node);
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// L'id de guilde que porte l'acteur visé, 0 s'il n'en a pas (ou s'il n'est pas
// dans la liste d'acteurs). Même chemin que le natif : acteur puis vtable+0xC4.
uint32_t ActorGuildId(void* actor) {
  __try {
    if (!actor) return 0;
    void** vtable = *reinterpret_cast<void***>(actor);
    return reinterpret_cast<GetGuildIdFn>(
        vtable[kActorVt_GetGuildId / sizeof(void*)])(actor);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Prédicats de classe, réimplémentés d'après les fonctions feuilles du client
// (docs §3) — aucun appel natif, donc utilisables même à moitié chargé.
inline bool IsPlayerJob(uint32_t id) {
  return (id <= 0x1e) || (id - 0xfa1u <= 0x7ceu);
}
inline bool IsMonsterJob(uint32_t id) {
  return (static_cast<int>(id) >= 0x3e9 && static_cast<int>(id) <= 0xf9e) ||
         (id - 0x4e35u <= 0x2ecau);
}
inline bool IsNpcOrPortalJob(uint32_t id) {
  return (id >= 45 && id < 1000) || (id - 10001u <= 0x270du);
}
inline bool IsSpecialUnitJob(uint32_t id) { return id - 6001u <= 0x33u; }
constexpr uint32_t kJobPortal = 45;  // warp : le natif n'y attache aucune action

// ── Rejouer une entrée de menu par le dispatcher du client (docs §9.2) ───────
// Les trois gestes du natif, puis « la ligne 0 a été cliquée ». Rien n'est
// réécrit : c'est le client qui décide encore de tout.
// ⚠ SEH ⇒ aucun objet C++ ici non plus.
bool RunNativeMenuCode(int code, uint32_t aid) {
  __try {
    void* game_mode = ReadGlobalPtr(rag::kActiveModePtr);
    if (!game_mode) return false;
    auto* gm = reinterpret_cast<uint8_t*>(game_mode);

    // 1. la cible — le dispatcher la relit dans CE champ, pas dans nos membres
    *reinterpret_cast<uint32_t*>(gm + kGm_MenuTarget) = aid;
    // 2. vider le vecteur de codes : end = begin, exactement comme le natif
    //    (`mov [edi+1D0h], eax` de son case 101). Aucune libération, donc rien
    //    à réallouer et aucun pointeur à invalider.
    void** begin = reinterpret_cast<void**>(gm + kGm_MenuCodes);
    void** end   = reinterpret_cast<void**>(gm + kGm_MenuCodes + 4);
    *end = *begin;
    // 3. y pousser NOTRE code, et un seul
    reinterpret_cast<PushBackFn>(kStdVectorIntPushBack)(gm + kGm_MenuCodes, &code);
    // 4. « la ligne 0 du menu a été cliquée »
    void** vtable = *reinterpret_cast<void***>(game_mode);
    reinterpret_cast<DispatchFn>(vtable[6])(game_mode, kMsgMenuItemChosen, 0, 0,
                                            0, 0);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Le clic gauche du client sur un acteur, à l'identique : c'est LUI qui tranche
// entre attaquer, suivre et refuser (surcharge, consentement PvP).
bool RunNativeActorClick(uint32_t aid) {
  __try {
    void* game_mode = ReadGlobalPtr(rag::kActiveModePtr);
    if (!game_mode) return false;
    reinterpret_cast<ClickFn>(kPostActorClickAction)(game_mode, aid, 1);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Le module, pour le détour (fonction libre : il tourne avant tout objet).
EntityContextMenu* g_owner = nullptr;
void* g_trampoline = nullptr;

// Corps du détour, isolé dans sa propre fonction : __try/__except est interdit
// dans une fonction qui doit dérouler des objets C++, et OnNativeContextMenu en
// manipule (std::string, std::vector).
bool SafeOnNativeContextMenu(void* game_mode, const int* quad, int blocked) {
  __try {
    return g_owner->OnNativeContextMenu(game_mode, quad, blocked);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Une entité à moitié construite a fait faute : on laisse tomber CE clic.
    // Rien de natif n'a été modifié, et le natif ne construira rien non plus —
    // ce qui vaut mieux que de le laisser lire le même quad douteux.
    return true;
  }
}

// ⚠ ABI : le natif est `__thiscall(this, quad, blocked)` avec `ret 8` ; un
// `__fastcall(this, edx, quad, blocked)` a exactement la même convention côté
// appelant ET côté pile. Le trampoline se rappelle donc sous la même signature.
int __fastcall ShowEntityContextMenuDetour(void* game_mode, void* edx, int quad,
                                           int blocked) {
  if (g_owner && g_owner->imgui_enabled_) {
    if (SafeOnNativeContextMenu(game_mode, reinterpret_cast<const int*>(quad),
                                blocked)) {
      return 0;  // le natif ne construit rien : sa fenêtre 0x12 ne naît jamais
    }
  }
  // Sans trampoline, il n'y a rien vers quoi revenir — et le détour ne devrait
  // même pas tourner. On rend la main sans rien faire plutôt que de sauter dans
  // le vide (le menu natif manquerait, le client survivrait).
  if (!g_trampoline) return 0;
  return reinterpret_cast<ShowMenuFn>(g_trampoline)(game_mode, edx, quad, blocked);
}

}  // namespace

EntityContextMenu::EntityContextMenu() {
  g_owner = this;
  // Posé inconditionnellement, comme FpsView : le timestamp du client n'est pas
  // encore connu au moment de LoadPlugins(). Le détour, lui, ne fait rien tant
  // que `imgui_enabled_` est faux.
  using namespace hooking;
  g_trampoline = HookManager::Instance().SetHook(
      HookType::kJmpHook, reinterpret_cast<uint8_t*>(kShowEntityContextMenu),
      reinterpret_cast<uint8_t*>(&ShowEntityContextMenuDetour));
  if (!g_trampoline) {
    LogDiag("[EntityContextMenu] detour NON pose (prologue non relocalisable)");
  }
}

void EntityContextMenu::OnModeSwitch(ModeMgr::ModeType, const char*) {
  // Un changement de map/mode invalide la cible : le menu ne doit pas survivre
  // à l'entité qu'il désigne.
  open_ = request_open_ = false;
  items_.clear();
  target_aid_ = 0;
  pending_code_ = 0;
  pending_local_ = Local::kNone;
}

// ── Interception ─────────────────────────────────────────────────────────────

bool EntityContextMenu::OnNativeContextMenu(void* game_mode, const int* quad,
                                            int blocked) {
  if (!game_mode) return true;

  // Les gardes de CONTEXTE du natif (docs §4.1), moins celle sur l'état du
  // bouton : celle-là vient plus bas, parce qu'on a désormais deux choses à
  // faire à deux instants différents du même clic. Elles ne sont pas
  // cosmétiques : sans elles, le menu s'ouvrirait pendant un drag, par-dessus
  // une fenêtre native, ou pendant un replay.
  if (ReadGlobalInt(kReplayActive)) return true;
  if (ReadGlobalInt(kMouseLButtonState) == kBtnHeld) return true;
  if ((GetAsyncKeyState(VK_SHIFT) >> 8) != 0) return true;  // Maj = attaque forcée
  if (!quad) return true;
  if (blocked) return true;
  if (ReadGlobalPtr(kStorageWndPtr)) return true;

  // Ce détour tourne à CHAQUE frame, y compris quand la souris ne fait que
  // survoler. Trois états du bouton droit nous concernent : l'appui (qu'il faut
  // désarmer), sa répétition, et le relâchement (qui ouvre). Pour tous les
  // autres, on sort avant de classifier — ce qui évite d'appeler des prédicats
  // natifs soixante fois par seconde pour rien.
  const int rbutton = ReadGlobalInt(kMouseRButtonState);
  if (rbutton != kBtnPressed && rbutton != kBtnRepeat && rbutton != kBtnReleased)
    return true;

  const uint32_t aid = static_cast<uint32_t>(quad[6]);
  const uint32_t job = static_cast<uint32_t>(quad[7]);
  const int      cat = quad[8];

  const Kind kind = ClassifyTarget(game_mode, aid, job, cat);
  if (kind == Kind::kNone) return true;

  // Les entités que le client n'a JAMAIS servies restent derrière leur opt-in.
  const bool native_served = (kind == Kind::kPlayer || kind == Kind::kPet ||
                              kind == Kind::kHomunculus || kind == Kind::kMercenary);
  if (!native_served && !all_entities_) return true;

  // 🔴 Un objet au sol, une unité de compétence ou une entité non classée n'ont
  // AUCUNE action de jeu : il n'en reste que de l'identité brute (nom, GID, quad
  // de pick), c'est-à-dire un outil de débogage. Pour un joueur, ce menu ne
  // ferait qu'avaler son clic droit sans rien lui offrir — il est donc réservé
  // au réglage staff, et le clic repart au natif pour tout le monde d'autre.
  const bool diagnostic_only = (kind == Kind::kSkillUnit ||
                                kind == Kind::kGroundItem || kind == Kind::kOther);
  if (diagnostic_only && !(staff_extras_ && IsStaff())) return true;

  // 🔴 Alt + clic droit sur un MONSTRE = ordre à l'homoncule (Alt+Maj = au
  // mercenaire) : `CursorMgr_UpdateHover` lit l'appui droit pour ça
  // (docs §7). On ne s'en mêle pas — ni menu, ni neutralisation.
  if ((GetAsyncKeyState(VK_MENU) >> 8) != 0 && kind == Kind::kMonster) return true;

  // ── 🔴 Le clic droit ne peut pas faire DEUX choses à la fois ────────────────
  // Le menu s'ouvre au RELÂCHEMENT (état 3) ; l'interaction du monde, elle, part
  // à l'APPUI (état 1) — une frame plus tôt, dans `CursorMgr_UpdateHover`, qui
  // tourne APRÈS nous dans la même passe souris. Sans ce qui suit, un clic droit
  // sur un PNJ ouvrait le dialogue ET notre menu (bug remonté en jeu).
  //
  // On efface donc l'état du bouton droit pour le RESTE de la frame, et
  // seulement quand ce clic nous appartient. Portée exacte, vérifiée sur les
  // seize lecteurs de ce global :
  //   · `UIWindowMgr_DispatchMouseInput` et `Camera_DragControl` l'ont déjà lu
  //     (ils passent AVANT nous) -> l'interface native et la rotation de caméra
  //     ne changent pas d'un pouce ;
  //   · `CursorMgr_UpdateHover`, `GroundClick_RequestMove`, `RepeatActorAction`
  //     passent APRÈS -> c'est eux, et eux seuls, qu'on désarme ;
  //   · on n'efface QUE l'appui (1 et 4). L'état « maintenu » (2) reste intact,
  //     donc rien de ce qui dépend d'un droit tenu ne bouge.
  // Le prochain `Mouse_UpdateFrameState` recalcule tout depuis les octets BRUTS
  // du WndProc : notre écriture ne survit pas à la frame, et le relâchement
  // produira bien son état 3.
  if (rbutton == kBtnPressed || rbutton == kBtnRepeat) {
    WriteGlobalInt(kMouseRButtonState, 0);
    return true;
  }
  // Reste le seul état encore possible : le relâchement, qui ouvre le menu.

  target_aid_  = aid;
  target_job_  = job;
  target_cat_  = cat;
  kind_        = kind;
  target_name_ = EntityName(game_mode, aid);

  // Ce que la cible est DÉJÀ : le menu grise ce qui n'aurait aucun sens plutôt
  // que de le retirer (le natif, lui, retirait l'entrée — le joueur ne pouvait
  // pas distinguer « pas invitable » de « pas encore chargé »). Lu ici, une
  // seule fois par ouverture, et seulement quand la question se pose.
  target_in_party_ = false;
  target_guild_id_ = 0;
  if (kind == Kind::kPlayer) {
    // Le nom BRUT, pas `target_name_` : la liste du groupe porte les octets du
    // client, et la conversion UTF-8 ne s'y comparerait plus.
    char wire_name[64] = {};
    ReadEntityNameRaw(game_mode, aid, wire_name, sizeof(wire_name));
    target_in_party_ = OwnPartyContains(wire_name);
    target_guild_id_ = ActorGuildId(FindActor(game_mode, aid));
  }

  BuildItems();
  if (items_.empty()) return true;

  request_open_ = true;
  request_tick_ = GetTickCount();
  return true;
}

EntityContextMenu::Kind EntityContextMenu::ClassifyTarget(void* game_mode,
                                                          uint32_t aid,
                                                          uint32_t job,
                                                          int category) const {
  if (aid == 0) return Kind::kNone;

  // Le pick tranche en premier : il sait des choses que le job ignore (un NPC
  // scripté sous forme de joueur porte un job de joueur — le cas qui a servi de
  // référence pendant la RE).
  if (category == kPickSkillUnit) return Kind::kSkillUnit;
  if (category == kPickGroundItem) return Kind::kGroundItem;
  if (category == kPickNpc) return Kind::kNpc;

  // Ses propres compagnons, avant tout test de job : ce sont les seuls cas où le
  // natif ouvrait autre chose qu'un menu de joueur.
  {
    void* session = reinterpret_cast<void*>(kSessionAddr);
    if (aid == static_cast<uint32_t>(ReadGlobalInt(kOwnPetAid))) {
      void* actor = FindActor(game_mode, aid);
      if (actor && Read<uint8_t>(actor, kActor_Type) == 7) return Kind::kPet;
    }
    if (aid == Read<uint32_t>(session, kSess_HomunAid)) return Kind::kHomunculus;
    if (aid == Read<uint32_t>(session, kSess_MercAid)) return Kind::kMercenary;
  }

  if (aid == static_cast<uint32_t>(ReadGlobalInt(kOwnAccountAid))) return Kind::kSelf;
  if (category == kPickSpecial || IsSpecialUnitJob(job)) return Kind::kOther;
  if (IsMonsterJob(job)) return Kind::kMonster;
  // 🔴 APRÈS le monstre, AVANT le joueur. Un PNJ scripté porte souvent une
  // classe de JOUEUR (kafra, marchand d'événement, PNJ de quête) : c'est ce
  // prédicat que le natif consulte pour REFUSER son menu joueur (docs §4.1).
  // Le placer après `IsMonsterJob` garantit qu'un monstre reste un monstre —
  // ce prédicat est vrai aussi pour des types d'acteur particuliers.
  if (IsHostileOrSpecialUnit(aid, job)) return Kind::kNpc;
  if (IsNpcOrPortalJob(job)) return Kind::kNpc;
  if (IsPlayerJob(job)) return Kind::kPlayer;
  return Kind::kOther;
}

// ── Construction des entrées ─────────────────────────────────────────────────

void EntityContextMenu::BuildItems() {
  items_.clear();
  const bool staff = staff_extras_ && IsStaff();
  const uint32_t own_aid = static_cast<uint32_t>(ReadGlobalInt(kOwnAccountAid));
  const bool client_admin = ClientAdminListContains(own_aid);

  auto add = [&](const char* label, int code, Local local = Local::kNone,
                 bool separator = false, bool is_staff = false,
                 const char* tip = nullptr) {
    Item item;
    item.label = label;
    item.code = code;
    item.local = local;
    item.separator = separator;
    item.staff = is_staff;
    if (tip) item.tip = tip;
    items_.push_back(std::move(item));
  };
  // Grise la dernière entrée ajoutée, avec la raison en infobulle.
  auto disable_last = [&](const char* why) {
    items_.back().disabled = true;
    items_.back().tip = why;
  };

  switch (kind_) {
    case Kind::kPlayer: {
      const bool in_guild  = ReadGlobalInt(kOwnGuildId) != 0;
      const bool is_master = in_guild && ReadGlobalInt(kGuildIsMaster) != 0;
      const bool in_party  = ReadGlobalInt(kInPartyFlag) != 0;

      add("Voir l'équipement", kCodeViewEquip);
      add("Proposer un échange", kCodeDeal);
      // Les deux invitations restent VISIBLES et grisées quand elles n'ont pas
      // de sens : le natif les faisait disparaître, ce qui laissait croire que
      // le client n'en était pas capable. La raison part en infobulle.
      if (in_party) {
        add("Inviter dans le groupe", kCodePartyInvite);
        if (target_in_party_) disable_last("Déjà membre de votre groupe.");
      }
      if (in_guild) {
        add("Inviter dans la guilde", kCodeGuildInvite);
        // Condition du natif à l'identique : il n'offrait l'invitation que sur
        // une cible SANS guilde. (Il exigeait en plus le droit d'invitation,
        // `dword_159C234` — non repris : ce flag n'est pas tranché en RE, et
        // s'en servir risquerait de masquer l'entrée à qui y a droit.)
        if (target_guild_id_ != 0) disable_last("Ce joueur a déjà une guilde.");
      }
      if (is_master) {
        add("Proposer une alliance", kCodeGuildAlly, Local::kNone, true);
        add("Déclarer la guilde ennemie", kCodeGuildFoe);
      }
      add("Chuchoter", kCodeWhisper, Local::kNone, true);
      add("Ajouter en ami", kCodeAddFriend);
      add("Envoyer un courrier", kCodeSendMail);
      // Même condition que le natif : le dispatcher, lui, ne la rejoue pas, et
      // la demande partirait au serveur pour se faire refuser.
      if (AdoptionEligible(target_aid_)) add("Adopter", kCodeAdopt);
      // 🔴 Deux entrées, pas une bascule : les codes 12 et 13 sont deux
      // commandes DISTINCTES côté client (docs §6.3). Le natif choisissait pour
      // le joueur en interrogeant sa liste de blocage ; on préfère lui laisser
      // dire ce qu'il veut plutôt que de deviner à sa place.
      add("Bloquer le chat", kCodeBlockChat, Local::kNone, true);
      add("Débloquer le chat", kCodeUnblockChat);
      add("Signaler ce joueur", kCodeReportUser);
      add("Copier le nom", 0, Local::kCopyName, true);
      break;
    }
    case Kind::kSelf:
      add("Copier mon nom", 0, Local::kCopyName);
      break;
    case Kind::kMonster:
      add("Attaquer", 0, Local::kAttack);
      add("Fiche du monstre", 0, Local::kMonsterInfo, true);
      add("Copier le nom", 0, Local::kCopyName);
      break;
    case Kind::kNpc:
      // Pas d'« Interagir » sur un PORTAIL : le natif sort avant toute action
      // dès que le job vaut 45 (docs §7), et un warp n'a pas de dialogue.
      if (target_job_ != kJobPortal) add("Interagir", 0, Local::kTalkToNpc);
      add("Copier le nom", 0, Local::kCopyName, true);
      break;
    case Kind::kPet:
      add("Statut du pet", kCodePetStatus);
      add("Nourrir", kCodePetFeed);
      add("Spectacle", kCodePetPerform);
      add("Retirer l'accessoire", kCodePetUnequip);
      add("Remettre dans l'œuf", kCodePetToEgg, Local::kNone, true);
      break;
    case Kind::kHomunculus:
      add("Statut de l'homoncule", kCodeHomunStatus);
      add("Nourrir", kCodeHomunFeed);
      add("En attente", kCodeHomunStandBy);
      break;
    case Kind::kMercenary:
      add("Statut du mercenaire", kCodeMercStatus);
      add("En attente", kCodeMercStandBy);
      break;
    case Kind::kSkillUnit:
    case Kind::kGroundItem:
    case Kind::kOther:
      // Ces trois-là ne s'ouvrent que pour le staff (cf. `diagnostic_only` dans
      // OnNativeContextMenu) : le reste du menu est la section staff ci-dessous.
      add("Copier le nom", 0, Local::kCopyName);
      break;
    default:
      break;
  }

  if (!staff) return;

  // ── Section staff ──────────────────────────────────────────────────────────
  // Gate SERVEUR (niveau de groupe >= 80). Les trois entrées marquées « client
  // admin » ont EN PLUS une garde dans le dispatcher natif : sans notre AID dans
  // le clientinfo.xml, le client les rejette en silence — on ne les propose donc
  // pas plutôt que d'offrir un bouton mort (docs §4.2).
  bool first = true;
  auto add_staff = [&](const char* label, int code, Local local = Local::kNone,
                       const char* tip = nullptr) {
    add(label, code, local, first, true, tip);
    first = false;
  };

  // L'identifiant seul, puis l'identité brute de ce que le pick a désigné : AID,
  // job et catégorie. C'est exactement ce qu'on va chercher au débogueur quand
  // une entité se comporte mal — et ça vaut pour TOUTES les entités, y compris
  // celles que le client n'ouvrait pas. Un identifiant nu ne dit rien à un
  // joueur : ces deux lignes vivent ici, et nulle part ailleurs.
  const char* copy_id_label = (kind_ == Kind::kSelf)   ? "Copier mon AID"
                              : (kind_ == Kind::kPlayer) ? "Copier l'AID"
                                                         : "Copier le GID";
  add_staff(copy_id_label, 0, Local::kCopyId);
  add_staff("Copier l'identité de pick", 0, Local::kCopyPickInfo);

  if (kind_ == Kind::kPlayer) {
    // Le « C-Code » est un identifiant de diagnostic (le natif le pose dans le
    // presse-papier) : même famille que l'AID, même place.
    add_staff("Copier le C-Code", kCodeCopyCCode);
    add_staff("Afficher le GID dans le chat", kCodeShowGid);
    add_staff("Expulser (kick)", kCodeKick, Local::kNone,
              "Le serveur revalide la permission avant d'agir.");
    add_staff("Journal des blocages de chat", kCodeChatBanLog);
    if (client_admin) {
      add_staff("Retirer tout l'équipement", kCodeFullStrip);
      add_staff("Point de manière +", kCodeMannerPlus);
      add_staff("Point de manière −", kCodeMannerMinus);
    }
  }
}

// ── Rendu ────────────────────────────────────────────────────────────────────

void EntityContextMenu::OnRenderUI() {
  if (!imgui_enabled_) return;

  if (request_open_) {
    const bool stale = (GetTickCount() - request_tick_) > 500u;
    request_open_ = false;
    if (stale) return;  // interface masquée au moment du clic : la demande a péri
    open_ = true;
    ImGui::OpenPopup("##bourgeon_entity_ctx");
    // 🔴 La position vient d'ImGui, PAS des globaux souris du client
    // (`g_MouseScreenX/Y`, docs §2) : ceux-là sont en pixels CLIENT, et rien ne
    // garantit qu'ils partagent l'échelle du DisplaySize d'ImGui. Le menu
    // s'ouvre au relâchement du bouton, donc le curseur n'a pas bougé entre le
    // tick qui l'a décidé et cette frame.
    ImGui::SetNextWindowPos(ImGui::GetIO().MousePos, ImGuiCond_Always);
  }
  if (!open_) return;

  // Serré : un menu contextuel se lit d'un coup d'œil, il ne se contemple pas.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 3.0f));
  const bool visible = ImGui::BeginPopup(
      "##bourgeon_entity_ctx",
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
          ImGuiWindowFlags_NoSavedSettings);
  if (visible) {
    // En-tête : QUI est visé. Le natif ne le disait pas — il fallait se souvenir
    // de ce qu'on avait cliqué.
    const char* kind_label = "Entité";
    switch (kind_) {
      case Kind::kSelf:       kind_label = "Moi"; break;
      case Kind::kPlayer:     kind_label = "Joueur"; break;
      case Kind::kMonster:    kind_label = "Monstre"; break;
      case Kind::kNpc:        kind_label = "NPC"; break;
      case Kind::kPet:        kind_label = "Pet"; break;
      case Kind::kHomunculus: kind_label = "Homoncule"; break;
      case Kind::kMercenary:  kind_label = "Mercenaire"; break;
      case Kind::kSkillUnit:  kind_label = "Unité"; break;
      case Kind::kGroundItem: kind_label = "Objet au sol"; break;
      default: break;
    }
    if (target_name_.empty()) {
      ImGui::Text("%s (%u)", kind_label, target_aid_);
    } else {
      ImGui::Text("%s", target_name_.c_str());
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.35f, 1.0f), "· %s", kind_label);
    }
    ImGui::Separator();

    for (size_t i = 0; i < items_.size(); ++i) {
      const Item& item = items_[i];
      if (item.separator && i != 0) ImGui::Separator();
      if (item.staff) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.28f, 0.10f, 1.0f));
      }
      if (item.disabled) ImGui::BeginDisabled();
      const bool clicked = ImGui::Selectable(
          item.label.c_str(), false, ImGuiSelectableFlags_NoAutoClosePopups);
      if (item.disabled) ImGui::EndDisabled();
      if (item.staff) ImGui::PopStyleColor();
      // 🔴 `AllowWhenDisabled` : sans ce drapeau, une entrée grisée ne compte pas
      // comme survolée — et c'est justement celle dont il faut dire POURQUOI.
      if (!item.tip.empty() &&
          ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", item.tip.c_str());
      }
      if (clicked) {
        Choose(item);
        ImGui::CloseCurrentPopup();
        open_ = false;
        break;
      }
    }
    ImGui::EndPopup();
  } else {
    // ImGui a fermé le popup (clic hors, Échap) : on suit.
    open_ = false;
  }
  ImGui::PopStyleVar(2);
}

// ── Exécution ────────────────────────────────────────────────────────────────

void EntityContextMenu::Choose(const Item& item) {
  // Rien n'est joué ICI : on est entre NewFrame() et Render(), et plusieurs de
  // ces actions ouvrent une modale native BLOQUANTE qui relance le tick du mode.
  pending_aid_   = target_aid_;
  pending_code_  = item.code;
  pending_local_ = item.local;
  pending_arg_   = target_job_;
}

void EntityContextMenu::FlushPending() {
  const int   code  = pending_code_;
  const Local local = pending_local_;
  if (code == 0 && local == Local::kNone) return;
  pending_code_  = 0;
  pending_local_ = Local::kNone;

  const uint32_t aid = pending_aid_;
  const uint32_t arg = pending_arg_;

  switch (local) {
    case Local::kCopyName:
      ImGui::SetClipboardText(target_name_.empty() ? "" : target_name_.c_str());
      return;
    case Local::kCopyId: {
      char buffer[24];
      snprintf(buffer, sizeof(buffer), "%u", aid);
      ImGui::SetClipboardText(buffer);
      return;
    }
    case Local::kCopyPickInfo: {
      char buffer[192];
      snprintf(buffer, sizeof(buffer),
               "%s | AID %u (0x%08X) | job %u | categorie de pick %d",
               target_name_.empty() ? "(nom inconnu)" : target_name_.c_str(),
               aid, aid, arg, target_cat_);
      ImGui::SetClipboardText(buffer);
      return;
    }
    case Local::kMonsterInfo:
      // `by_view` : le quad porte une classe de SPRITE, pas un id de mob_db —
      // exactement le cas du skill Sense, que la fiche sait déjà résoudre.
      if (auto* mi = Bourgeon::Instance().monster_info()) mi->Open(arg, true);
      return;
    case Local::kTalkToNpc: {
      uint8_t packet[7] = {};
      packet[0] = static_cast<uint8_t>(kCzContactNpc & 0xff);
      packet[1] = static_cast<uint8_t>(kCzContactNpc >> 8);
      memcpy(packet + 2, &aid, 4);
      packet[6] = 0;
      Bourgeon::Instance().SendPacket(packet, sizeof(packet));
      return;
    }
    case Local::kAttack:
      if (!RunNativeActorClick(aid))
        LogDiag("[EntityContextMenu] clic acteur refuse (cible {})", aid);
      return;
    case Local::kNone:
      break;
  }

  if (!RunNativeMenuCode(code, aid))
    LogDiag("[EntityContextMenu] action {} injouable (cible {})", code, aid);
}

// ── Réglages ─────────────────────────────────────────────────────────────────

bool EntityContextMenu::DrawSettings() {
  // (Aucune case « Menu contextuel ImGui » ici : `imgui_enabled_` appartient au
  // groupe « Interface moderne », dont l'interrupteur unique vit en tête du
  // panneau. Une case locale rouvrirait l'état mixte que ce groupe existe pour
  // interdire — c'est la règle suivie par toutes les fenêtres du groupe.)
  bool changed = false;
  changed |= ro::RoCheckbox("Sur toutes les entités###ctxmenu_all", &all_entities_);
  ImGui::SameLine();
  HelpMarker(
      "Le client n'ouvrait de menu que sur un joueur, son pet, son homoncule ou "
      "son mercenaire. Coché, le menu s'ouvre aussi sur les monstres et les "
      "NPC.");

  if (IsStaff()) {
    changed |= ro::RoCheckbox("Outils du staff###ctxmenu_staff", &staff_extras_);
    ImGui::SameLine();
    HelpMarker(
        "Ajoute au menu les actions et les identifiants réservés au staff, et "
        "ouvre le menu sur les entités de diagnostic (unités de compétence, "
        "objets au sol). Certaines entrées exigent en plus que l'AID du compte "
        "figure dans le clientinfo.xml du client : elles n'apparaissent que "
        "dans ce cas.");
  }
  return changed;
}
