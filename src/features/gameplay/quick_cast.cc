#include "features/gameplay/quick_cast.h"
#include "ragnarok/actor.h"  // rag::actor : les offsets de CActorSprite

#include <Windows.h>

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/overlays/skill_bar.h"  // RepeatItemSlot (rejeu d'une case d'objet)
#include "features/overlays/target_frame.h"  // la cible du HUD, comme source de visée
#include "features/staff_gate.h"
#include "imgui.h"
#include "ragnarok/game_scene.h"
#include "ragnarok/own_actor.h"  // rag::OwnActor / rag::OwnActorOf
#include "ragnarok/globals.h"
#include "ragnarok/skill_cooldowns.h"
#include "ragnarok/uiwnd.h"
#include "ui/ro_imgui.h"    // ro::RoCheckbox
#include "ui/ro_widgets.h"  // mui::HelpMarker, mui::WheelSliderInt
#include "utils/i18n.h"
#include "utils/game_focus.h"  // win::GameHasFocus
#include "ragnarok/job_ids.h"  // rag::IsPlayerJob / IsMonsterJob

// ── Adresses (client 20250716, no-ASLR : addr Ghidra == live) ────────────────
namespace {
// Raycast souris -> cellule sol la plus proche. bool __thiscall(gameMode,
// int* outX, int* outY) — la fonction même qu'emprunte le clic-sol natif.
constexpr uintptr_t kPickGroundCellAddr = 0x00c69a40;

// Quadtree de picking des acteurs, reconstruit à chaque frame par le rendu des
// sprites. QueryPoint : float* __thiscall(tree, float x, float y) -> quad
// (10 floats) ou nullptr. C'est la source du survol natif.
constexpr int kQuadAid = 6;  // dword : AID de l'acteur
constexpr int kQuadJob = 7;  // dword : job/classe (discrimine joueur/monstre)
constexpr int kQuadCat = 8;  // dword : catégorie de pick (0 = acteur)

// Fenêtre native sous un point écran. void* __thiscall(g_UIWindowMgr, x, y) :
// hit-test PUR (itère la liste de fenêtres, vtbl+0xC8), sans effet de bord.
constexpr uintptr_t kWndAtPointAddr = 0x00a336d0;

// Position écran de la souris, tenue par le WndProc du jeu.

// vtable de CGameMode. Sert de GARDE : CMode::SendMsg est aussi le dispatch des
// autres modes (login, char-select), et +0x408 n'y voudrait rien dire. Relevée
// en live (`[[*(0x0121333c)] + 0x18] == 0x00c86740` = CMode::SendMsg).
constexpr uintptr_t kGameModeVtable = 0x010904b8;

// État de ciblage dans CGameMode, posé par le case 0x48.
constexpr int kOffTargetingMode  = 0x408;  // 1 sol, 2 cible, 4 soutien
constexpr int kOffTargetingSkill = 0x40c;
constexpr int kOffTargetingLevel = 0x414;

// Acteur -> état de mouvement/action. C'est la SEULE donnée « suis-je prêt ? »
// que le client possède vraiment, et le natif s'en sert exactement ainsi : dans
// Actor_ProcessPendingAction_Tick (0x00D43400), les cas 3 (skill sur cible) et 4
// (skill au sol) n'exécutent la requête en attente que si
// `état < 2 || état == 4 || état >= 16` — sinon la file patiente. On réplique ce
// prédicat pour ne pas ré-émettre pendant une incantation ou une animation.
inline bool ActorStateAllowsCast(int state) {
  return static_cast<unsigned>(state) < 2u || state == 4 || state >= 16;
}

// Messages d'acteur (vtable+8 = Actor_OnMsg), tels que les émet le clic natif.
constexpr int kActorMsgCastOnGround = 0x41;  // (skillId, x, y)
constexpr int kActorMsgCastOnTarget = 0x29;  // (GID, skillId)
constexpr int kActorMsgSkillLevel   = 0x5a;  // (niveau)

// CMode::SendMsg : sortie du mode ciblage (le pendant du 0x48).
constexpr int kSendMsgLeaveTargeting = 0x47;

constexpr unsigned kJobWarpPortal = 45;  // catégorie 0 mais non ciblable

// Durée de validité d'une frappe en attente. Une touche n'annonce que l'action
// qu'elle déclenche dans la foulée — tout le chemin frappe -> dispatch hotkey ->
// activation est synchrone, donc quelques dizaines de millisecondes suffisent
// très largement. Au-delà, la touche est simplement TENUE pour autre chose
// (marcher, par exemple) et ne doit pas se retrouver associée à un déclenchement
// à la souris survenu entre-temps.
constexpr uint32_t kPendingKeyLifetimeMs = 250;

// ── Appel de Actor_OnMsg via la vtable (+8) ─────────────────────────────────
// Le natif empile TOUJOURS 13 dwords : un mot de tête à 0, le message en 64 bits,
// puis CINQ paramètres 64 bits (les inutilisés restent à 0). Vérifié sur les
// quatre messages employés ici et sur le 0x11 de KeyboardMove.
// La convention de nettoyage exacte est inconnue (Ghidra ne récupère pas les 13
// paramètres) : on restaure ESP soi-même, ce qui reste correct que la fonction
// nettoie ou non. Pas de SEH ici — un __try ne cohabite pas avec un bloc __asm,
// et l'appelant encadre déjà.
__declspec(noinline) void ActorSendMsg(void* actor, int msg,
                                       int p1lo, int p1hi,
                                       int p2lo, int p2hi,
                                       int p3lo, int p3hi) {
  void** vtbl = *reinterpret_cast<void***>(actor);
  void* fn = vtbl[2];  // vtable+8 = Actor_OnMsg
  __asm {
    push esi
    mov  esi, esp
    push 0            // params 4 et 5, inutilisés
    push 0
    push 0
    push 0
    mov  eax, p3hi
    push eax
    mov  eax, p3lo
    push eax
    mov  eax, p2hi
    push eax
    mov  eax, p2lo
    push eax
    mov  eax, p1hi
    push eax
    mov  eax, p1lo
    push eax
    push 0            // message, dword de poids fort
    mov  eax, msg
    push eax          // message, dword de poids faible
    push 0            // mot de tête (toujours 0 chez le natif)
    mov  ecx, actor
    mov  eax, fn
    call eax
    mov  esp, esi
    pop  esi
  }
}

// Acteur joueur, ou nullptr. La descente elle-même est `rag::OwnActorOf` ; ce
// qui reste ici, et qui est la RAISON D'ÊTRE de cette enveloppe, c'est la
// VALIDATION : `SendMsg` sert aussi aux modes login et char-select, où +0xCC
// n'a aucun sens. Trois points d'appel s'y fient — deux le disent en commentaire.
void* GetOwnActor(void* cmode) {
  __try {
    if (!cmode || *reinterpret_cast<uintptr_t*>(cmode) != kGameModeVtable)
      return nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
  return rag::OwnActorOf(cmode);
}

// ── La cible du HUD comme SOURCE DE VISÉE ───────────────────────────────────
// Le HUD de cible sait qui le joueur a désigné en dernier ; quand il accepte de
// le dire (réglage « Les sorts ciblés partent sur la cible »), une compétence
// n'a plus besoin de la souris pour savoir sur qui partir. Toute la validation
// est CHEZ LUI — c'est lui qui possède la cible, et les règles d'une cible
// légale sont les mêmes que celles de PickTargetGid.
bool HudCastArmed() {
  auto* tf = Bourgeon::Instance().target_frame();
  return tf != nullptr && tf->CastsOnHudTarget();
}
unsigned HudTargetGid(int mode) {
  auto* tf = Bourgeon::Instance().target_frame();
  return tf ? tf->SkillTargetGid(mode) : 0u;
}

// L'utilisateur vise-t-il le monde, ou une fenêtre native ? (Pas d'effet de bord :
// simple hit-test.) Viser une fenêtre = il ne désigne pas le sol derrière.
bool CursorOverNativeWindow() {
  __try {
    using WndAtPoint_t = void*(__thiscall*)(void*, int, int);
    return reinterpret_cast<WndAtPoint_t>(kWndAtPointAddr)(
               reinterpret_cast<void*>(uiwnd::kUIWindowMgrAddr),
               *reinterpret_cast<int*>(rag::kMouseScreenXAddr),
               *reinterpret_cast<int*>(rag::kMouseScreenYAddr)) != nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return true;  // dans le doute, on laisse le ciblage natif
  }
}

// La souris désigne-t-elle le MONDE ?
//
// 🔴 Séparé des gardes générales depuis que la visée peut venir d'ailleurs : ce
// test ne concerne QUE les visées à la souris (la cellule d'un sort au sol, et
// l'entité sous le curseur). La cible du HUD, elle, ne demande rien à la souris
// — refuser de lancer parce que le curseur passe au-dessus d'une fenêtre serait
// exactement le contraire de ce que ce réglage promet.
bool MouseAimsAtWorld() {
  // Notre interface : l'utilisateur ne désigne pas le monde derrière.
  if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse)
    return false;
  if (CursorOverNativeWindow()) return false;
  return true;
}

// Résout la cible sous le curseur pour les modes 2 (offensif) et 4 (soutien).
// Renvoie 0 si rien de compatible : le ciblage natif reste alors armé, ce qui est
// le repli voulu (surtout ne rien lancer « à vide »).
unsigned PickTargetGid(int mode) {
  __try {
    using QueryPoint_t = float*(__thiscall*)(void*, float, float);
    const float* quad = reinterpret_cast<QueryPoint_t>(gamescene::kQuadTreeQueryPointAddr)(
        reinterpret_cast<void*>(gamescene::kPickQuadTreeAddr),
        static_cast<float>(*reinterpret_cast<int*>(rag::kMouseScreenXAddr)),
        static_cast<float>(*reinterpret_cast<int*>(rag::kMouseScreenYAddr)));
    if (!quad) return 0;
    const int* q = reinterpret_cast<const int*>(quad);
    if (q[kQuadCat] != 0) return 0;  // 1 NPC, 2 unité de skill, 3/4 spéciaux
    const unsigned aid = static_cast<unsigned>(q[kQuadAid]);
    const unsigned job = static_cast<unsigned>(q[kQuadJob]);
    if (aid == 0 || job == kJobWarpPortal) return 0;
    if (mode == 2) {
      // Offensif : monstre uniquement, et jamais soi-même. C'est ICI que vit
      // désormais la règle — le chemin natif qui la portait
      // (CursorMgr_UpdateHover) n'est plus emprunté.
      if (aid == rag::OwnAccountId()) return 0;
      if (!rag::IsMonsterJob(job)) return 0;  // joueur : PVP/GVG au clic manuel
    }
    return aid;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// Le cast est parti : on quitte le mode ciblage comme le fait le pipeline souris
// natif après un lancement, sinon le curseur de visée resterait armé et le
// prochain clic relancerait la compétence.
void LeaveTargeting(void* cmode) {
  __try {
    rag::ModeSendMsg(cmode, kSendMsgLeaveTargeting);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// État de mouvement/action de l'acteur. Illisible -> on renvoie 0 (« au repos »),
// donc laisser passer : dans le doute, ne pas ajouter de blocage.
int ReadActorState(void* actor) {
  __try {
    return *reinterpret_cast<int*>(reinterpret_cast<char*>(actor) +
                                   rag::actor::kMotionState);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// Lit l'état de ciblage posé par le 0x48. false si rien d'exploitable.
// POD uniquement -> le __try peut englober tout le natif (C2712 sinon).
bool ReadTargetingState(void* cmode, int* mode, int* skill, int* level) {
  __try {
    char* m = reinterpret_cast<char*>(cmode);
    *mode  = *reinterpret_cast<int*>(m + kOffTargetingMode);
    *skill = *reinterpret_cast<int*>(m + kOffTargetingSkill);
    *level = *reinterpret_cast<int*>(m + kOffTargetingLevel);
    return *mode != 0 && *skill != 0;  // 0x48 natif parti en garde
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Corps réel : résout la visée et émet les messages d'acteur. Renvoie true si une
// compétence est effectivement partie. Prend mode/id/niveau en paramètres plutôt
// que de les relire dans `cmode` — la répétition les rejoue alors que le ciblage
// natif a justement été désarmé par le premier lancement.
bool EmitCast(void* cmode, void* actor, int mode, int skill, int level,
              bool ground_on, bool target_on, bool mouse_on_world) {
  __try {
    // Compétence encore en cooldown : le serveur refuserait, inutile d'émettre.
    // C'est la VRAIE donnée, préférable à un délai deviné — mais elle ne couvre
    // que les compétences qui ont un cooldown (ZC_SKILL_POSTDELAY 0x043D). La
    // plupart des sorts n'en ont pas : ils sont bornés par l'after-cast delay,
    // que le serveur ne communique jamais au client. D'où la période de cadence
    // en complément, cf. QuickCast::Update.
    // L'identifiant convient : `+0x40C` est celui qui finit dans CZ_USE_SKILL
    // (via la file d'acteur `+0x524`), donc l'identifiant SERVEUR, le même que
    // celui porté par 0x043D.
    if (ro::SkillCooldownRemainingMs(static_cast<uint16_t>(skill)) > 0)
      return false;

    if (mode == 1) {  // sol
      // Un sort de zone vise une CELLULE, et il n'y en a qu'une source : le
      // curseur. Si celui-ci ne désigne pas le monde, il n'y a rien à viser —
      // et rien que la cible du HUD puisse remplacer.
      if (!ground_on || !mouse_on_world) return false;
      int x = -1, y = -1;
      using PickGround_t = uint8_t(__thiscall*)(void*, int*, int*);
      if ((reinterpret_cast<PickGround_t>(kPickGroundCellAddr)(cmode, &x, &y) &
           0xff) == 0)
        return false;  // rien de visé (ciel, hors carte) -> ciblage laissé armé
      ActorSendMsg(actor, kActorMsgCastOnGround, skill, skill >> 31,
                   x, x >> 31, y, y >> 31);
      ActorSendMsg(actor, kActorMsgSkillLevel, level, level >> 31, 0, 0, 0, 0);
      return true;
    }

    if (mode == 2 || mode == 4) {  // cible offensive / soutien
      // 🔴 La SOURIS d'abord, le HUD ensuite. Viser quelqu'un du curseur est un
      // geste explicite : il ne doit jamais se faire voler par la cible du HUD.
      // Le second n'intervient que là où le premier ne dit rien — curseur dans
      // le vide, sur une fenêtre, ou visée à la souris tout simplement éteinte.
      unsigned gid = (target_on && mouse_on_world) ? PickTargetGid(mode) : 0u;
      if (gid == 0) gid = HudTargetGid(mode);
      if (gid == 0) return false;
      // ⚠ Le GID part en 64 bits avec un mot haut à ZÉRO, pas une extension de
      // signe : c'est ce que fait le natif, et un AID au bit 31 armé deviendrait
      // sinon négatif.
      ActorSendMsg(actor, kActorMsgCastOnTarget, static_cast<int>(gid), 0,
                   skill, skill >> 31, 0, 0);
      ActorSendMsg(actor, kActorMsgSkillLevel, level, level >> 31, 0, 0, 0, 0);
      return true;
    }

    return false;  // modes 3/5 : ciblage natif inchangé
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
}  // namespace

// Conditions communes au premier lancement et à chaque répétition.
bool QuickCast::CanCastNow() const {
  // Trois opt-in mènent ici, dont un qui n'est pas à nous : le HUD de cible peut
  // fournir la visée à lui seul, QuickCast entièrement éteint par ailleurs.
  if (!ground_enabled_ && !target_enabled_ && !HudCastArmed()) return false;
  if (Bourgeon::Instance().client().timestamp() != 20250716) return false;
  if (!Bourgeon::Instance().IsGameActive() ||
      Bourgeon::Instance().IsMapLoading())
    return false;
  // Jeu au second plan : la touche est peut-être toujours enfoncée
  // physiquement, mais elle ne s'adresse plus à lui (cf. GameHasFocus). Sans
  // effet sur le premier lancement — il vient d'une frappe reçue par le jeu.
  if (!win::GameHasFocus()) return false;
  // ⚠ Le curseur n'est PLUS testé ici. Il ne conditionne que les visées à la
  // souris, et c'est EmitCast qui les distingue désormais (MouseAimsAtWorld) :
  // une visée par le HUD n'a que faire de l'endroit où le curseur se trouve.
  return true;
}

void QuickCast::OnKeyDown(unsigned long vkey, int new_key, int accurate_key) {
  // Seul un appui FRAIS peut mener à un 0x48 ou à l'usage d'un objet (le natif
  // ignore l'auto-répétition, cf. l'en-tête). On retient la touche pour
  // l'associer à l'action qui suit immédiatement, dans la même passe de message.
  if (accurate_key) {
    pending_vk_    = vkey;
    pending_vk_ms_ = GetTickCount();
  }
}

// Touche en attente, consommée : renvoie 0 si elle a expiré ou n'est plus tenue.
// Consommer dans tous les cas est délibéré — une frappe ne vaut que pour l'action
// qu'elle amène, sinon un déclenchement ULTÉRIEUR à la souris en hériterait et se
// mettrait à se répéter tout seul.
unsigned long QuickCast::TakePendingKey() {
  const unsigned long vk = pending_vk_;
  pending_vk_ = 0;
  if (vk == 0) return 0;
  if (GetTickCount() - pending_vk_ms_ > kPendingKeyLifetimeMs) return 0;
  if ((GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) == 0) return 0;
  return vk;
}

// Ce mode de visée est-il le NÔTRE ? C'est ce qui décide si une touche a le
// droit de laisser un cercle derrière elle (cf. UpdateDisarm) : là où QuickCast
// ne prend pas la main, le déroulé natif « la touche arme, le clic lance » doit
// rester intact.
bool QuickCast::ClaimsMode(int mode) const {
  if (mode == 1) return ground_enabled_;
  if (mode == 2 || mode == 4) return target_enabled_ || HudCastArmed();
  return false;  // 3 et 5 : ciblage natif, on n'y touche pas
}

void QuickCast::OnEnterTargeting(void* cmode) {
  // Un 0x48 consomme la touche en attente, qu'il aboutisse ou non (cf.
  // TakePendingKey).
  const unsigned long vk = TakePendingKey();
  repeat_vk_ = 0;  // un nouvel armement annule la répétition précédente

  if (!CanCastNow()) return;

  void* actor = GetOwnActor(cmode);
  if (!actor) return;

  // 🔴 L'état de ciblage est lu AVANT la cadence, et ce n'est pas cosmétique :
  // c'est lui qui dit si le cercle qui vient d'apparaître nous appartient. Le
  // refus par la cadence sortait autrefois d'ici sans rien noter — et laissait
  // donc exactement le cercle orphelin que UpdateDisarm existe pour retirer.
  int mode = 0, skill = 0, level = 0;
  if (!ReadTargetingState(cmode, &mode, &skill, &level)) return;
  const bool claimed = ClaimsMode(mode);

  // Une touche à NOUS vient d'armer une visée : on en devient responsable
  // jusqu'à son relâchement.
  //
  // 🔴 « À nous » ne veut PAS dire « une frappe fraîche à cet instant ».
  // ⏱ Mesuré au journal : un appui maintenu produit un SECOND `0x48` ~500 ms
  // plus tard (l'auto-répétition du système), et `TakePendingKey` ne rend la
  // touche qu'au PREMIER. Le second arrivait donc avec `vk == 0` et n'était plus
  // surveillé par personne — c'est exactement le cercle qui restait armé quand
  // on relâchait. Tant qu'une touche à nous est encore tenue (`arm_vk_`), la
  // visée qu'elle vient de réarmer reste la nôtre.
  // ⚠ « Encore tenue », pas « déjà vue » : une touche relâchée dont
  // `UpdateDisarm` n'a pas encore consommé le relâchement ne doit pas s'attribuer
  // le cercle d'un lancement fait à la souris entre-temps.
  const bool still_held =
      arm_vk_ != 0 && (GetAsyncKeyState(static_cast<int>(arm_vk_)) & 0x8000) != 0;

  if (claimed && (vk != 0 || still_held)) {
    if (vk != 0) arm_vk_ = vk;
    arm_mode_ = mode;  // le cercle courant est celui de CE mode
  } else if (!claimed) {
    // Le cercle qui vient d'apparaître n'est pas le nôtre : on lâche toute
    // surveillance en cours.
    arm_vk_ = 0;
  }
  // ⚠ Reste un cas SANS surveillance, et c'est voulu : mode à nous, aucune
  // touche — le lancement vient d'un CLIC sur une case de la barre. Le déroulé
  // natif « ça arme, je clique ma cible » y reste entier, et c'est la seule voie
  // qui subsiste pour viser à la main ce que QuickCast refuse de viser tout seul
  // (un JOUEUR en mode offensif). Son cercle attend donc un clic, comme chez le
  // client.

  const uint32_t now = GetTickCount();
  const uint32_t period = repeat_ms_ > 0 ? static_cast<uint32_t>(repeat_ms_) : 0;
  const bool too_soon = last_cast_ms_ != 0 && now - last_cast_ms_ < period;

  if (too_soon || !EmitCast(cmode, actor, mode, skill, level, ground_enabled_,
                            target_enabled_, MouseAimsAtWorld())) {
    // Rien n'est parti : cadence, cooldown, ou simplement rien de visé. On ARME
    // QUAND MÊME la répétition — c'est ce qui permet à un lancement refusé sur
    // l'instant d'aboutir sans relâcher : la cadence s'écoule, le cooldown
    // tombe, ou le curseur arrive sur une cible. Sans cela, marteler deux
    // touches de suite en perdait une, silencieusement.
    // Même raison qu'au-dessus : si ce 0x48 n'est que le second d'un même
    // geste (auto-répétition), la touche est celle qu'on surveille déjà.
    // ⏱ C'est aussi ce qui récupérait un lancement PERDU : au journal, un second
    // 0x48 refusé par la cadence n'armait aucune répétition, alors que la cible
    // du HUD était bien là — le sort ne partait jamais.
    const unsigned long key = vk != 0 ? vk : (still_held ? arm_vk_ : 0);
    if (key != 0 && claimed) {
      repeat_vk_    = key;
      repeat_mode_  = mode;
      repeat_skill_ = skill;
      repeat_level_ = level;
    }
    return;
  }

  last_cast_ms_ = now;
  LeaveTargeting(cmode);
  // 🔴 ON GARDE `arm_vk_`. Le réflexe — « le cercle est parti, plus rien à
  // surveiller » — était le bug : la touche est toujours tenue, elle réarmera
  // une visée dans un instant, et il faut encore quelqu'un pour la retirer.
  // `UpdateDisarm` ne fait rien quand il n'y a rien d'armé, donc le garder ne
  // coûte rien ; le lâcher coûtait un cercle orphelin.

  // Armer la répétition — seulement si une touche encore enfoncée a amené ce
  // lancement : cela distingue le clavier d'un clic sur une case de la barre de
  // raccourcis, qui ne doit rien répéter. Même repli que plus haut : sur un
  // `0x48` d'auto-répétition, la touche est celle qu'on surveille déjà.
  const unsigned long held = vk != 0 ? vk : (still_held ? arm_vk_ : 0);
  if (held != 0) {
    repeat_vk_    = held;
    repeat_mode_  = mode;
    repeat_skill_ = skill;
    repeat_level_ = level;
  }
}

// ── Une touche QuickCast ne laisse jamais de cercle derrière elle ────────────
//
// ⏱ Symptôme vu en jeu : on relâche la touche et le cercle de visée reste armé.
// Il n'est pas seulement inesthétique — c'est un PIÈGE : le prochain clic, qui
// visait autre chose, lance la compétence oubliée.
//
// Il apparaît chaque fois que le natif a armé la visée (case 0x48) sans que
// QuickCast ait pu lancer : cadence pas écoulée, compétence en cooldown, ou
// curseur qui ne désigne rien d'exploitable. La répétition armée ci-dessus
// rattrape les deux premiers dès qu'ils se lèvent ; le dernier, non — et de
// toute façon il faut bien un état de sortie propre.
//
// 🔴 Ne vaut QUE pour une visée armée par une TOUCHE, et dans un mode que
// QuickCast prend en charge. Deux exclusions délibérées :
//   · lancer une compétence en CLIQUANT sa case de barre de raccourcis ne pose
//     aucune touche (TakePendingKey rend 0) : le déroulé natif « ça arme, je
//     clique ma cible » reste entier. C'est la voie qui reste pour viser à la
//     main ce que QuickCast refuse de viser tout seul — un JOUEUR en mode
//     offensif, notamment (cf. PickTargetGid) ;
//   · les modes 3 et 5, que QuickCast ne touche pas.
//
// 🔴 Appelée depuis Bourgeon::OnGameFrame, donc HORS frame ImGui : elle émet
// CMode::SendMsg, le dispatcher que notre propre hook intercepte — le jouer
// entre NewFrame() et Render() ferait tourner OnProcessInput au milieu d'une
// frame. C'est aussi pourquoi elle n'est pas dans Update().
void QuickCast::UpdateDisarm() {
  if (arm_vk_ == 0) return;
  // Toujours enfoncée : le joueur n'a pas fini son geste.
  if ((GetAsyncKeyState(static_cast<int>(arm_vk_)) & 0x8000) != 0) return;
  const int armed_mode = arm_mode_;
  arm_vk_ = 0;

  void* cmode = rag::ActiveModeSafe();
  if (!cmode || !GetOwnActor(cmode)) return;  // valide aussi la vtable du mode

  int mode = 0, skill = 0, level = 0;
  // Plus rien d'armé : le lancement a fini par partir, ou le natif a désarmé
  // tout seul. Rien à faire.
  if (!ReadTargetingState(cmode, &mode, &skill, &level)) return;
  // Autre chose est armé depuis : ce cercle-là n'est pas celui qu'on surveille,
  // et l'effacer volerait au joueur une visée qu'il vient de demander.
  if (mode != armed_mode) return;

  LeaveTargeting(cmode);
}

void QuickCast::Update() {
  if (repeat_vk_ == 0) return;

  // Touche relâchée : fin de la répétition. Testé AVANT tout le reste pour que
  // l'état s'oublie même si une autre garde bloque le lancement.
  if ((GetAsyncKeyState(static_cast<int>(repeat_vk_)) & 0x8000) == 0) {
    repeat_vk_ = 0;
    return;
  }

  const uint32_t now = GetTickCount();
  const uint32_t period = repeat_ms_ > 0 ? static_cast<uint32_t>(repeat_ms_) : 0;
  if (now - last_cast_ms_ < period) return;

  if (!CanCastNow()) return;

  void* cmode = rag::ActiveModeSafe();
  if (!cmode) return;
  void* actor = GetOwnActor(cmode);  // valide aussi la vtable du mode
  if (!actor) return;

  // Occupé (incantation, animation d'attaque…) : le natif ferait patienter la
  // requête, autant ne rien émettre. Vraie donnée du client, contrairement à la
  // période — mais elle ne connaît QUE l'état d'animation, pas le délai
  // d'après-incantation du serveur. Uniquement sur la répétition : un appui
  // volontaire n'est jamais bloqué par cette lecture.
  if (!ActorStateAllowsCast(ReadActorState(actor))) return;

  // La visée est ré-évaluée : au sol la cellule suit le curseur, sur cible le GID
  // est re-résolu (donc suivre un monstre du curseur enchaîne dessus). Si plus
  // rien n'est visé, on n'émet pas — mais on reste armé, le curseur peut revenir.
  if (EmitCast(cmode, actor, repeat_mode_, repeat_skill_, repeat_level_,
               ground_enabled_, target_enabled_, MouseAimsAtWorld()))
    last_cast_ms_ = now;
}

void QuickCast::OnUseItemSlot(int region, int slot, uint32_t nameid) {
  // La touche est consommée même si la répétition est éteinte : elle a bien
  // servi à cette activation-là et ne doit pas resservir plus tard.
  const unsigned long vk = TakePendingKey();

  if (!item_enabled_) return;
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;
  if (vk == 0) return;  // clic sur la case : une seule utilisation, rien à répéter

  item_vk_      = vk;
  item_region_  = region;
  item_slot_    = slot;
  item_id_      = nameid;
  last_item_ms_ = GetTickCount();  // le premier usage vient d'avoir lieu
}

void QuickCast::UpdateItemRepeat() {
  if (item_vk_ == 0) return;

  // Touche relâchée : fin de la répétition. Testé AVANT tout le reste pour que
  // l'état s'oublie même si une autre garde bloque l'utilisation.
  if ((GetAsyncKeyState(static_cast<int>(item_vk_)) & 0x8000) == 0) {
    item_vk_ = 0;
    return;
  }
  if (!item_enabled_) { item_vk_ = 0; return; }
  // Jeu passé au second plan (alt-tab la touche enfoncée) : elle ne s'adresse
  // plus à lui. On arrête net plutôt que de vider le sac en arrière-plan.
  if (!win::GameHasFocus()) { item_vk_ = 0; return; }
  // Saisie en cours dans notre interface : la frappe appartient au champ de
  // texte, pas à la barre de raccourcis. (Ouvrir le chat sans lâcher la touche.)
  if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantTextInput) {
    item_vk_ = 0;
    return;
  }
  if (!Bourgeon::Instance().IsGameActive() || Bourgeon::Instance().IsMapLoading())
    return;  // chargement de carte : on patiente, la touche est toujours tenue

  const uint32_t now = GetTickCount();
  // Étage de DROIT, pas d'interface : le plancher joueur s'applique ici, sur la
  // période effective, pour qu'un yaml édité à la main ne le contourne pas. Le
  // staff garde les 20 ms que son serveur honore ; pour tout le monde, en
  // dessous de 100 ms le serveur répond de toute façon « veuillez patienter ».
  const int floor_ms = IsStaff() ? 20 : 100;
  const int wanted_ms = (item_repeat_ms_ > floor_ms) ? item_repeat_ms_ : floor_ms;
  const uint32_t period = static_cast<uint32_t>(wanted_ms);
  if (now - last_item_ms_ < period) return;

  // Le rejeu appartient à la barre : elle seule sait ce que la case porte
  // encore, et ce qu'il en reste en sac. Un `false` = plus rien à répéter (case
  // vidée ou réarrangée, dernier exemplaire consommé).
  auto* bar = Bourgeon::Instance().skill_bar();
  if (!bar || !bar->RepeatItemSlot(item_region_, item_slot_, item_id_)) {
    item_vk_ = 0;
    return;
  }
  last_item_ms_ = now;
}

void QuickCast::OnRenderUI() { Update(); }

void QuickCast::OnModeSwitch(ModeMgr::ModeType mode_type,
                             const char* map_name) {
  repeat_vk_  = 0;
  pending_vk_ = 0;
  item_vk_    = 0;
  // Le mode qui portait la visée n'existe plus : il n'y a plus de cercle à
  // retirer, et le `cmode` qu'on irait interroger ne serait pas le même.
  arm_vk_     = 0;
}

void QuickCast::DrawSettings() {
  bool save = false;
  if (ro::RoCheckbox(i18n::Tr("Sort de zone : cast direct sous la souris"),
                     &ground_enabled_))
    save = true;
  ImGui::SameLine();
  mui::HelpMarker(
      i18n::Tr("Une compétence de ZONE (Storm Gust, pièges…) part immédiatement sur la "
      "cellule sous le curseur : plus besoin du clic de confirmation.\n\n"
      "Si aucune cellule valide n'est visée (ciel, interface), le mode ciblage "
      "classique reste armé. L'animation, la barre de cast et l'envoi restent "
      "ceux du jeu : le serveur reste maître du lancement."));
  if (ro::RoCheckbox(i18n::Tr("Sort ciblé : cast direct sur la cible survolée"),
                     &target_enabled_))
    save = true;
  ImGui::SameLine();
  mui::HelpMarker(
      i18n::Tr("Une compétence CIBLÉE part immédiatement si une cible compatible est "
      "sous le curseur : monstre pour un sort offensif ; joueur, monstre ou "
      "soi-même pour un soutien.\n\n"
      "Sans cible compatible sous le curseur (ou pour viser un joueur en "
      "PVP/GVG), le mode ciblage classique reste armé — rien n'est perdu."));

  if (ground_enabled_ || target_enabled_) {
    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (mui::WheelSliderInt(i18n::Tr("Cadence (ms)"), &repeat_ms_, 50, 1000)) save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
    ImGui::SameLine();
    mui::HelpMarker(
        i18n::Tr("Période de répétition quand tu MAINTIENS la touche : la compétence "
        "s'enchaîne à ce rythme, en suivant le curseur.\n\n"
        "Le jeu, lui, ignore l'auto-répétition du clavier — un appui maintenu "
        "ne lance qu'une fois. Cette répétition est donc ajoutée par le "
        "plugin. Valeur haute = un lancement par pression.\n\n"
        "Le COOLDOWN réel de la compétence est de toute façon respecté : tant "
        "qu'elle n'est pas prête, rien n'est envoyé. Ce réglage ne sert qu'aux "
        "compétences SANS cooldown, dont le rythme est fixé par le délai "
        "d'après-incantation — que le serveur ne communique pas au client."));
  }

  ImGui::Spacing();
  if (ro::RoCheckbox(i18n::Tr("Objet : répéter tant que la touche est maintenue"),
                     &item_enabled_))
    save = true;
  ImGui::SameLine();
  mui::HelpMarker(
      i18n::Tr("Une case de la barre d'action qui porte un OBJET (Old Blue Box, Dead "
      "Branch, potions…) s'utilise en boucle tant que tu gardes sa touche "
      "enfoncée, au lieu d'une fois par pression.\n\n"
      "Ça s'arrête tout seul quand tu relâches, quand la case change, ou quand "
      "il n'en reste plus en sac. Cliquer la case, en revanche, ne répète "
      "rien : une seule utilisation, comme avant.\n\n"
      "Attention : chaque répétition CONSOMME un objet — dont le dernier."));
  if (item_enabled_) {
    // Le plancher dépend du DROIT, pas de la fenêtre qui affiche : 20 ms pour
    // le staff (son serveur descend jusque-là), 100 ms pour tout le monde.
    // L'application réelle est dans UpdateItemRepeat — ce clamp-ci ne fait que
    // remettre l'AFFICHAGE dans la plage, un yaml édité à la main compris.
    const int item_floor = IsStaff() ? 20 : 100;
    if (item_repeat_ms_ < item_floor) item_repeat_ms_ = item_floor;
    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (mui::WheelSliderInt(i18n::Tr("Cadence objet (ms)"), &item_repeat_ms_,
                            item_floor, 1000))
      save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
    ImGui::SameLine();
    if (IsStaff()) {
      mui::HelpMarker(
          i18n::Tr("Période de répétition des objets. Elle est SÉPARÉE de celle des "
          "compétences parce que ce n'est pas le même frein : ici c'est le "
          "serveur qui fixe le minimum entre deux objets.\n\n"
          "Pour le staff, ce minimum est de 20 ms — le serveur descend "
          "l'intervalle à cette valeur au-dessus du niveau de groupe 40, au lieu "
          "des 325 ms habituels. Le curseur va donc jusque-là.\n\n"
          "En dessous, rien ne va plus vite : le serveur refuse et répond "
          "« veuillez patienter » dans le chat. En pratique, la vraie limite est "
          "la fréquence d'images du client."));
    } else {
      mui::HelpMarker(
          i18n::Tr("Période de répétition des objets, séparée de celle des "
          "compétences parce que le frein n'est pas le même : c'est le SERVEUR "
          "qui fixe l'intervalle minimal entre deux objets — 325 ms en temps "
          "normal, davantage en PvP sur les soins.\n\n"
          "Régler plus vite que lui ne consomme rien de plus : il refuse et "
          "répond « veuillez patienter » dans le chat. Ce curseur ne descend "
          "pas sous 100 ms."));
    }
  }

  if (save) {
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}
