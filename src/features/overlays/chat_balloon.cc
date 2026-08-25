#include "ragnarok/globals.h"
#include "features/overlays/chat_balloon.h"

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstring>
#include <iterator>

#include "imgui.h"

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/systems/image_preview.h"  // imgprev : emotes Discord deja en cache
#include "features/windows/chat_window.h"
#include "ragnarok/game_scene.h"
#include "ragnarok/uiwnd.h"
#include "ui/game_emotes.h"   // ro::emote : les emotes du JEU (emotion.act)
#include "ui/icon_cache.h"    // ro::ItemIcon : le cache d'icones partage avec le chat
#include "ui/ro_imgui.h"
#include "utils/hooking/hook_manager.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Adresses natives (client 20250716, no-ASLR : IDB == live) ────────────────
// Tout est documenté dans docs/entity_chat_balloon_re.md.
namespace {

// UIBalloonText::SetText(texte, 35 caractères) __thiscall, `retn 8`. Le point de
// passage COMMUN aux deux créateurs de bulle (acteur et unité de sol).
constexpr uintptr_t kSetTextWrapped = 0x00830240;
// UITransBalloonText::Paint __thiscall sans argument, `retn`.
constexpr uintptr_t kBalloonPaint = 0x008263a0;
// CTagMgr::Transform(this, &sortie, entrée PAR VALEUR) __thiscall — le
// transformateur de balises GLOBAL du client (singleton `0x0131ed60`).
//
// 🔴 C'est le SECOND transformateur du chemin de la bulle, et le seul qui tourne
// encore chez nous : le msg 7 n'appelle `ChatText_TransformTagLinks` que sous
// `if (g_pNewChatWnd)` — mort — mais appelle celui-ci SANS condition
// (`0x00c4db6c`). Sa table de balises (littéraux à `0x00fd5a00`) porte `<MSG>`,
// `MSI_`, `<NR>`, `<NAMELESS>`, `<TIP…>` … et `<NAVIL>`, qu'il remplace par sa
// forme d'AFFICHAGE `<carte x,y>` — nom INTERNE de carte, plus aucune balise.
// D'où le défaut corrigé ici : un lien de lieu arrivait à notre parseur déjà
// aplati, donc affiché littéralement au-dessus de la tête alors que la même
// ligne est un lien dans la chatbox, laquelle ingère AVANT (`ChatAction`).
constexpr uintptr_t kTagMgrTransform = 0x007fbfc0;
// L'adresse de RETOUR du site d'appel de la bulle — le seul qui nous concerne.
// `CTagMgr::Transform` sert tout le client ; sans ce filtre on capterait le
// texte de n'importe quelle infobulle.
//
// ⚠ Variable, et pas `constexpr` : le stub la compare en MÉMOIRE (`cmp eax,
// symbole` lit le CONTENU, à l'inverse d'un `push` — cf. le piège documenté sur
// PaintStub). C'est justement ce qu'on veut ici, et ça garde l'adresse écrite en
// UN seul endroit ; une `constexpr` pourrait n'avoir aucun emplacement à lire.
uintptr_t g_tagmgr_ret_from_balloon = 0x00c4db71;
// UIWindow::ClearSurface(couleur) __thiscall — 0xFFFF00FF est la couleur-clé du
// client : une surface remplie de ce magenta est intégralement transparente.
constexpr uintptr_t kClearSurface = 0x00a1cb30;
constexpr uint32_t  kColorKey     = 0xFFFF00FF;

// UIWindowMgr::QueueDestroyWindow(fenêtre) __thiscall — destruction DIFFÉRÉE,
// exactement celle que le natif s'applique à lui-même à l'expiration des 5 s
// (CActorSprite_UpdateOverheadWidgets) et au despawn
// (ActorAiClass_DestroyAttachedUI). En la réutilisant on ne saute aucune de ses
// étapes de démontage.
constexpr uintptr_t kQueueDestroyWindow = 0x00a447d0;
// ⚠ Le gestionnaire de fenêtres EST l'objet global, pas un pointeur vers lui :
// le natif fait `mov ecx, OFFSET dword_131F4E8`. Le déréférencer donnerait un
// `this` aberrant.

// GameMode_GetActive(mgr) __fastcall : le CGameMode actif, 0 hors jeu.
using QueueDestroyFn = void(__thiscall*)(void*, void*);

// Offsets (cf. docs/entity_nameplate_re.md et entity_chat_balloon_re.md).
constexpr int kNode_Actor   = 0x08;   //  node+8          = pointeur acteur
constexpr int kAct_ScreenX  = 0xac;   //  int  : X écran projeté (pieds)
constexpr int kAct_ScreenY  = 0xb0;   //  int  : Y écran projeté (pieds)
constexpr int kAct_Aid      = 0x110;  //  uint : AID
constexpr int kAct_Balloon  = 0x264;  //  UITransBalloonText* : LA bulle
constexpr int kAct_Height   = 0x5c;   //  float : facteur de hauteur du sprite

// UITransBalloonText : couleur par défaut du texte, posée par ZC_NPC_CHAT (et le
// rose 0xFF8080 codé en dur de la Talkie Box).
constexpr int kBal_Color = 0x90;

// Les acteurs sans AID exploitable (unités de sol) sont indexés par pointeur.
inline uint32_t KeyForActor(void* actor, uint32_t aid) {
  return (aid != 0) ? aid : static_cast<uint32_t>(reinterpret_cast<uintptr_t>(actor));
}

// Un champ à un offset : la lecture est celle de tout le monde (globals.h),
// et le `using` garde les points d'appel de ce fichier tels quels.
using rag::Read;

void* g_tramp_settext = nullptr;
void* g_tramp_paint   = nullptr;
void* g_tramp_tagxform = nullptr;

// ── Handlers appelés depuis les stubs ────────────────────────────────────────
void __cdecl BalloonCapture(void* window, const char* text) {
  ChatBalloon::OnNativeSetText(window, text);
}

void __cdecl TagTransformCapture(const void* in_string) {
  ChatBalloon::OnNativeTagTransform(in_string);
}

// Renvoyé dans AL : 1 => le natif doit se taire pour cette fenêtre.
int __cdecl BalloonSilence(void* window) {
  return ChatBalloon::IsActorBalloon(window) ? 1 : 0;
}

// OBSERVATEUR : on copie le texte encore brut (avant la coupure à 35) puis on
// relaie tel quel. `pushad` empile 0x20 octets ; +0x20 = adresse de retour,
// +0x24 = premier argument pile (le texte).
__declspec(naked) void SetTextStub() {
  __asm {
    pushad
    mov  edx, [esp + 0x24]   // char* texte
    push edx
    push ecx                 // this = la fenêtre bulle
    call BalloonCapture
    add  esp, 8
    popad
    jmp  [g_tramp_settext]
  }
}

// OBSERVATEUR, lui aussi : on copie le texte AVANT que le transformateur de
// balises global ne l'aplatisse, et on relaie tel quel.
//
// ⚠ Filtré par l'ADRESSE DE RETOUR, pas par un état : `CTagMgr::Transform` sert
// tout le client, et seul le site de la bulle nous intéresse. Après `pushad`
// (0x20 octets), `[esp+0x20]` est l'adresse de retour de l'appelant, `+0x24` la
// std::string de SORTIE, `+0x28` l'entrée — passée PAR VALEUR, donc 0x18 octets
// posés à même la pile de l'appelant.
__declspec(naked) void TagTransformStub() {
  __asm {
    pushad
    mov  eax, [esp + 0x20]   // adresse de retour de l'appelant
    // Dans l'asm inline MSVC un identifiant C++ désigne un EMPLACEMENT mémoire :
    // on compare donc bien au CONTENU de la variable (le msg 7 de l'acteur).
    cmp  eax, [g_tagmgr_ret_from_balloon]
    jne  passthrough
    lea  eax, [esp + 0x28]   // &std::string d'ENTRÉE (argument par valeur)
    push eax
    call TagTransformCapture
    add  esp, 4
  passthrough:
    popad
    jmp  [g_tramp_tagxform]
  }
}

// SILENCE : si un acteur revendique cette fenêtre, on remplit sa surface de la
// couleur-clé et on rend la main — le natif ne dessine rien, mais garde son
// cycle de vie. `POPAD` ne touche pas aux drapeaux, le `test` peut donc le
// précéder.
__declspec(naked) void PaintStub() {
  __asm {
    pushad
    push ecx
    call BalloonSilence
    add  esp, 4
    test al, al
    popad
    jnz  silence
    jmp  [g_tramp_paint]
  silence:
    // ⚠ Valeurs en DUR, et pas les constantes nommées juste au-dessus : dans
    // l'asm inline MSVC, un identifiant C++ désigne un EMPLACEMENT mémoire, pas
    // un immédiat — `push kColorKey` empilerait le contenu de la variable.
    push 0FFFF00FFh          // = kColorKey, la couleur-clé du client
    mov  eax, 0A1CB30h       // = kClearSurface (UIWindow::ClearSurface)
    call eax                 // __thiscall : this déjà en ecx (restauré par popad)
    retn                     // UITransBalloonText::Paint = `retn` (aucun argument pile)
  }
}

}  // namespace

std::mutex                       ChatBalloon::s_mutex;
std::vector<ChatBalloon::Pending> ChatBalloon::s_pending;
std::unordered_set<void*>        ChatBalloon::s_claimed;
std::string                      ChatBalloon::s_pending_raw;

ChatBalloon::ChatBalloon() {
  using namespace hooking;
  g_tramp_settext = HookManager::Instance().SetHook(
      HookType::kJmpHook, reinterpret_cast<uint8_t*>(kSetTextWrapped),
      reinterpret_cast<uint8_t*>(&SetTextStub));
  g_tramp_paint = HookManager::Instance().SetHook(
      HookType::kJmpHook, reinterpret_cast<uint8_t*>(kBalloonPaint),
      reinterpret_cast<uint8_t*>(&PaintStub));
  g_tramp_tagxform = HookManager::Instance().SetHook(
      HookType::kJmpHook, reinterpret_cast<uint8_t*>(kTagMgrTransform),
      reinterpret_cast<uint8_t*>(&TagTransformStub));
}

ChatBalloon::~ChatBalloon() = default;

// ── Détours ──────────────────────────────────────────────────────────────────
void ChatBalloon::OnNativeSetText(void* window, const char* wire) {
  if (window == nullptr || wire == nullptr || *wire == '\0') return;
  // Même garde que `IsActorBalloon` : overlay éteint, on ne capte rien. Sans
  // cela la file se remplirait de messages que personne ne draine — OnRenderUI
  // rend la main avant.
  ChatBalloon* self = Bourgeon::Instance().chat_balloon();
  if (self == nullptr || !self->Active()) return;
  // Rien de coûteux ici : ni parcours d'acteurs, ni conversion, ni parseur. On
  // ne sait même pas encore si cette fenêtre est une bulle ou une infobulle.
  std::lock_guard<std::mutex> lock(s_mutex);
  // 🔴 CONSOMMATION EN UN COUP, quoi qu'il arrive ensuite : la capture d'avant
  // transformation n'est valable que pour l'appel qui vient, et la laisser armée
  // la ferait ressortir sur la prochaine infobulle venue (le détour de
  // `SetTextWrapped` est partagé avec elles).
  std::string raw;
  raw.swap(s_pending_raw);
  if (s_pending.size() >= 64) return;  // garde-fou : on ne bufferise pas l'infini
  Pending p;
  p.window = window;
  p.wire.assign(wire);
  p.raw.swap(raw);
  s_pending.push_back(std::move(p));
}

// Le texte du msg 7 AVANT `CTagMgr::Transform`. Cf. le commentaire de
// `kTagMgrTransform` : le transformateur remplace `<NAVIL>` par une forme
// d'affichage qui n'est plus une balise pour personne, et notre parseur ne voit
// alors qu'un `<carte x,y>` en toutes lettres.
void ChatBalloon::OnNativeTagTransform(const void* native_string) {
  if (native_string == nullptr) return;
  ChatBalloon* self = Bourgeon::Instance().chat_balloon();
  if (self == nullptr || !self->Active()) return;
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;

  // std::string MSVC : données en place tant que la capacité tient dans l'objet
  // (+0x10 = taille, +0x14 = capacité), pointeur au-delà.
  const uint8_t* base = static_cast<const uint8_t*>(native_string);
  const uint32_t size = *reinterpret_cast<const uint32_t*>(base + 0x10);
  const uint32_t cap  = *reinterpret_cast<const uint32_t*>(base + 0x14);
  const char* text = (cap >= 0x10) ? *reinterpret_cast<const char* const*>(base)
                                   : reinterpret_cast<const char*>(base);
  if (text == nullptr || size == 0 || size > 4096) return;
  std::string raw(text, size);

  // 🔴 ON N'ARME QUE POUR `<NAVIL>`, et c'est délibéré :
  //  · c'est la SEULE balise que le transformateur mange et que notre parseur
  //    sache rendre — `<ITEML>` et nos balises maison lui sont inconnues et
  //    traversent intactes (mesuré en jeu) ;
  //  · tout ce qu'il résout par ailleurs (`<MSG>MSI_…`, `<NR>`, `<NAMELESS>`)
  //    doit continuer de nous arriver RÉSOLU : le texte d'avant transformation
  //    ne sert donc que là où il apporte quelque chose ;
  //  · armer rarement rend le couplage avec `SetTextWrapped` sûr : les deux
  //    appels se suivent dans le même handler, sur le même fil.
  // ⚠ Majuscules seulement, comme le parseur : le client accepte aussi
  // `<navil>`, mais le rendre ici afficherait une balise brute là où le
  // transformateur, lui, donnait au moins un lieu lisible.
  // (Le `<` n'est jamais un octet de queue CP949 : la recherche ne peut pas
  // tomber au milieu d'un caractère double-octet.)
  if (raw.find("<NAVIL>") == std::string::npos) return;

  std::lock_guard<std::mutex> lock(s_mutex);
  s_pending_raw.swap(raw);
}

bool ChatBalloon::IsActorBalloon(void* window) {
  if (window == nullptr) return false;
  // 🔴 Le détour est posé une fois pour toutes, mais il ne doit RIEN faire tant
  // que l'overlay n'affiche pas : décrocher la case dans le panneau doit rendre
  // sa bulle au client, pas supprimer toute bulle du jeu. Même raisonnement pour
  // un client d'une autre version — les offsets d'acteur seraient faux, donc on
  // ne revendique rien et le natif reprend la main intégralement.
  ChatBalloon* self = Bourgeon::Instance().chat_balloon();
  if (self == nullptr || !self->Active()) return false;
  if (Bourgeon::Instance().client().timestamp() != 20250716) return false;
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_claimed.count(window) != 0) return true;
  }
  // Raté de cache : une bulle qui vient de naître, avant notre prochaine frame.
  // On tranche tout de suite en parcourant la liste d'acteurs, sinon le natif la
  // dessinerait pendant une frame — un clignotement de la balise brute.
  void* gm = rag::ActiveModeIfReady();
  if (!gm) return false;
  void* actor_mgr = Read<void*>(gm, gamescene::kGmActorMgr);
  if (!actor_mgr) return false;
  void* sentinel = Read<void*>(actor_mgr, gamescene::kAmListHead);
  if (!sentinel) return false;

  auto owns = [&](void* actor) {
    return actor != nullptr && Read<void*>(actor, kAct_Balloon) == window;
  };
  bool found = owns(Read<void*>(actor_mgr, gamescene::kAmOwnPlayer));  // hors liste !
  int guard = 0;
  for (void* node = Read<void*>(sentinel, 0);
       !found && node && node != sentinel && guard < 4096;
       node = Read<void*>(node, 0), ++guard) {
    found = owns(Read<void*>(node, kNode_Actor));
  }
  if (!found) return false;
  std::lock_guard<std::mutex> lock(s_mutex);
  s_claimed.insert(window);
  return true;
}

// ── Frame ────────────────────────────────────────────────────────────────────
// On vient peut-être d'être décochés : ne pas laisser derrière soi des bulles
// figées à l'écran ni des captures que plus personne ne draine.
//
// ⚠ Fonction séparée, et pas un bloc dans OnRenderUI : le `lock_guard` y serait
// un objet à dérouler, ce que MSVC refuse dans une fonction contenant `__try`
// (C2712).
void ChatBalloon::ResetWhenDisabled() {
  balloons_.clear();
  // ⚠ Vider aussi la file de destruction : éteints, on ne la draine plus, et ses
  // pointeurs d'acteurs deviendraient périmés. Les rejouer plus tard, à la
  // réactivation, reviendrait à déréférencer des acteurs libérés depuis
  // longtemps. Les fenêtres concernées expirent toutes seules côté natif.
  pending_destroy_.clear();
  std::lock_guard<std::mutex> lock(s_mutex);
  s_pending.clear();
  s_claimed.clear();
  s_pending_raw.clear();
}

// Suit la chatbox moderne, sans réglage propre : voir le commentaire du header.
bool ChatBalloon::Active() const {
  const ChatWindow* chat = Bourgeon::Instance().chat_window();
  return chat != nullptr && chat->imgui_enabled_;
}

bool ChatBalloon::HasBalloonFor(void* actor) const {
  if (actor == nullptr || balloons_.empty() || !Active()) return false;
  const uint32_t aid = Read<uint32_t>(actor, kAct_Aid);
  return balloons_.count(KeyForActor(actor, aid)) != 0;
}

void ChatBalloon::OnRenderUI() {
  if (!Active()) {
    if (!balloons_.empty()) ResetWhenDisabled();
    return;
  }
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;
  // Dessin SEUL : l'adoption et la destruction ont eu lieu à OnGameFramePulse,
  // avant que le jeu ne dessine.
  __try {
    DrawBalloons();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Une entité à moitié construite/libérée a fait faute : on saute la frame.
    // Rien de natif n'a été modifié — le détour de Paint reste en place.
  }
}

void ChatBalloon::OnGameFramePulse() {
  if (!Active()) return;
  if (Bourgeon::Instance().client().timestamp() != 20250716) {
    pending_destroy_.clear();
    return;
  }
  // Adoption d'abord : elle remplit `pending_destroy_` pour les fenêtres nées
  // depuis le dernier battement…
  SyncGuarded();
  // …puis on les tue TOUT DE SUITE, dans la même frame et avant le dessin.
  if (pending_destroy_.empty()) return;
  std::vector<Doomed> doomed;
  doomed.swap(pending_destroy_);
  DestroyAdopted(doomed);
}

// ⚠ Le `__try` vit SEUL dans sa fonction, sans aucun objet local : MSVC refuse
// de mêler gestion structurée d'exceptions et déroulement d'objets (C2712).
void ChatBalloon::SyncGuarded() {
  __try {
    SyncWithActors();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Acteur à moitié construit/libéré : on saute ce battement. Rien de natif
    // n'a été modifié à ce stade — la destruction, elle, a sa propre garde.
  }
}

// Hors de toute frame ImGui, et sous __try : entre l'adoption et ici, l'acteur a
// pu mourir ou changer de map.
void ChatBalloon::DestroyAdopted(const std::vector<Doomed>& doomed) {
  auto queue_destroy = reinterpret_cast<QueueDestroyFn>(kQueueDestroyWindow);
  void* mgr = reinterpret_cast<void*>(uiwnd::kUIWindowMgrAddr);
  // ⚠ Boucle indexée, pas un range-for : en debug MSVC les itérateurs de vecteur
  // sont des objets à dérouler, ce que `__try` interdit dans la même fonction
  // (C2712) — le même piège que dans OnRenderUI.
  for (size_t i = 0; i < doomed.size(); ++i) {
    const Doomed& d = doomed[i];
    __try {
      // Ne toucher à rien si l'acteur porte désormais une AUTRE fenêtre : le
      // natif en a recréé une entre-temps (nouvelle réplique), et elle sera
      // adoptée à son tour.
      if (Read<void*>(d.actor, kAct_Balloon) != d.window) continue;
      *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(d.actor) + kAct_Balloon) = nullptr;
      queue_destroy(mgr, d.window);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      // Acteur déjà libéré : le natif a détruit la fenêtre avec lui
      // (ActorAiClass_DestroyAttachedUI). Rien à faire.
    }
  }
}

void ChatBalloon::SyncWithActors() {
  void* gm = rag::ActiveModeIfReady();
  if (!gm) { balloons_.clear(); return; }
  void* actor_mgr = Read<void*>(gm, gamescene::kGmActorMgr);
  if (!actor_mgr) { balloons_.clear(); return; }
  void* sentinel = Read<void*>(actor_mgr, gamescene::kAmListHead);
  if (!sentinel) return;
  void* own_actor = Read<void*>(actor_mgr, gamescene::kAmOwnPlayer);

  // Les textes captés depuis la dernière frame.
  std::vector<Pending> pending;
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    pending.swap(s_pending);
  }
  // Reconstruit à neuf CHAQUE frame, dans un jeu local publié d'un coup à la
  // fin : le tas recycle les blocs, et une entrée périmée ferait taire une
  // infobulle qui aurait hérité de l'adresse d'une ancienne bulle.
  std::unordered_set<void*> claimed;

  const uint32_t now = GetTickCount();
  ChatWindow* chat = Bourgeon::Instance().chat_window();

  // Un seul passage par acteur : on revendique la fenêtre, on rapproche les
  // textes en attente, on relit la couleur, puis on marque la fenêtre native
  // pour destruction.
  std::unordered_set<uint32_t> alive;
  auto visit = [&](void* actor) {
    if (actor == nullptr) return;
    const uint32_t aid = Read<uint32_t>(actor, kAct_Aid);
    const uint32_t key = KeyForActor(actor, aid);
    // 🔴 `alive` se remplit pour TOUT acteur présent, pas seulement pour ceux
    // qui portent une fenêtre : depuis qu'on détruit la native dès adoption,
    // « plus de fenêtre » ne veut plus dire « plus personne ». Le lier à la
    // fenêtre effaçait notre propre bulle la frame suivante.
    alive.insert(key);

    void* window = Read<void*>(actor, kAct_Balloon);
    if (window == nullptr) return;
    claimed.insert(window);

    // Un texte en attente pour CETTE fenêtre ?
    for (Pending& p : pending) {
      if (p.window != window || p.wire.empty()) continue;
      // 🔴 LE raccordement à l'interface moderne : c'est le parseur de la
      // chatbox ImGui qui résout les balises, pas une copie locale. Sans lui,
      // `<MOBL>` / `<CRAF>` / `<ITMR>` — et même `<ITEML>` — s'affichent bruts,
      // la résolution native étant morte (g_pNewChatWnd == 0).
      //
      // ⚠ On prend les FRAGMENTS, pas `plain` : ce dernier n'est que la
      // concaténation des textes, donc une emote du jeu y reste écrite
      // « :nom: » et une icône d'objet disparaît.
      if (chat == nullptr) { p.wire.clear(); continue; }
      // 🔴 Le texte d'AVANT `CTagMgr::Transform` gagne quand il existe — il n'est
      // capté que s'il porte un `<NAVIL>`, que le transformateur aplatit en
      // `<carte x,y>` (nom interne, plus de balise, donc plus de lien). Partout
      // ailleurs c'est bien le texte TRANSFORMÉ qu'il faut : lui seul a ses
      // `<MSG>MSI_…` résolus.
      ChatWindow::Line parsed;
      chat->ParseWireLine(p.raw.empty() ? p.wire.c_str() : p.raw.c_str(), &parsed);
      p.wire.clear();  // consommé
      p.raw.clear();
      if (parsed.runs.empty()) continue;

      Balloon& b = balloons_[key];
      b.line = std::move(parsed);
      b.born_ms = now;
      // Le natif fige 5 000 ms pour tout le monde : un pavé de trois lignes
      // disparaît aussi vite qu'un « ok ». On tient compte de la longueur.
      const int len = static_cast<int>(b.line.plain.size());
      // « Garder la durée du client » doit rendre EXACTEMENT les 5 000 ms du
      // natif — le plafond ne s'applique qu'à notre durée proportionnelle, sans
      // quoi un plafond bas trahirait l'option.
      b.life_ms = follow_native_life_
                      ? 5000u
                      : static_cast<uint32_t>(
                            std::min(base_life_ms_ + per_char_ms_ * len, max_life_ms_));
    }

    auto it = balloons_.find(key);
    if (it != balloons_.end()) {
      // Couleur portée par la fenêtre native : ZC_NPC_CHAT y écrit la sienne, et
      // la Talkie Box son rose 0xFF8080. On la relit AVANT de détruire — après,
      // il n'y aurait plus rien à lire.
      const uint32_t rgb = Read<uint32_t>(window, kBal_Color);
      if (rgb != 0 && rgb <= 0xFFFFFF) it->second.rgb = rgb;
      if (actor == own_actor && !show_self_) it->second.life_ms = 0;
    }

    // 🔴 REMPLACEMENT, pas masquage : la fenêtre native est mise à la casse dès
    // que son texte est à nous. Effacer sa surface à la couleur-clé ne suffisait
    // pas — `UIWindow_Render` blitte la surface quoi qu'il arrive, et il restait
    // un rectangle à l'écran. Plus d'objet, plus de rectangle.
    //
    // La destruction est faite par OnGameFramePulse, dans la MEME frame et
    // AVANT que le jeu ne dessine. Passer par OnRenderUI laissait la fenetre
    // native visible une frame entiere (rectangle clignotant), et OnTick,
    // bride a ~100 ms, en laissait plusieurs.
    pending_destroy_.push_back({actor, window});
  };

  int guard = 0;
  for (void* node = Read<void*>(sentinel, 0);
       node && node != sentinel && guard < 4096;
       node = Read<void*>(node, 0), ++guard) {
    visit(Read<void*>(node, kNode_Actor));
  }
  // ⚠ Le joueur local n'apparaît PAS dans cette liste — c'est pour ça que sa
  // bulle restait native alors que celles des autres étaient bien reprises. Il
  // vit à part, à `actorMgr+0x2C`, et c'est justement lui que vise
  // `clif_displaymessage` (ZC 0x008E appelle `*(actorMgr+0x2C)->OnMsg`).
  visit(own_actor);

  {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_claimed.swap(claimed);
  }

  // Purge : expirés, ou acteur disparu (mort, hors de vue, changement de map).
  for (auto it = balloons_.begin(); it != balloons_.end();) {
    const bool gone = (alive.count(it->first) == 0);
    const bool expired = (now - it->second.born_ms) >= it->second.life_ms;
    it = (gone || expired) ? balloons_.erase(it) : std::next(it);
  }
}

// Une passe unique, mesure ou dessin selon `dl`. Calquée sur la boucle de
// ChatWindow::DrawLines : mêmes primitives, mêmes hauteurs, même avance de 2 px
// après une image — c'est ce qui fait qu'une phrase rend pareil ici et là.
void ChatBalloon::LayoutRuns(const ChatWindow::Line& line, float wrap,
                             float font_px, uint32_t fallback_rgb, ImDrawList* dl,
                             ImVec2 origin, int alpha255, ImVec2* out_size) {
  ImFont* font = ImGui::GetFont();
  const float line_h = font_px;
  const float img_h = font_px + 2.0f;   // emotes et icônes : hauteur de ligne
  const ImU32 link_col = IM_COL32(0xF4, 0x93, 0x4A, alpha255);  // = kLinkCol du chat
  const ImU32 def_col = IM_COL32((fallback_rgb >> 16) & 0xff, (fallback_rgb >> 8) & 0xff,
                                 fallback_rgb & 0xff, alpha255);

  float x = 0.0f, y = 0.0f, row_h = line_h, maxw = 0.0f;
  auto newline = [&]() {
    if (x > maxw) maxw = x;
    x = 0.0f;
    y += row_h;
    row_h = line_h;
  };

  for (size_t ri = 0; ri < line.runs.size(); ++ri) {
    const ChatWindow::Run& run = line.runs[ri];

    // Emote du JEU. Elle sort du GRF : elle est là ou elle ne sera jamais, donc
    // aucune place à réserver. 🔴 Tester l'existence AVANT de toucher à la
    // géométrie — sinon on replie la ligne pour une image qui n'arrive pas.
    if (run.game_emote >= 0 && ro::emote::Exists(run.game_emote)) {
      if (x > 0.0f && x + img_h > wrap) newline();
      if (img_h > row_h) row_h = img_h;
      if (dl != nullptr) {
        const ImVec2 p(origin.x + x, origin.y + y);
        ro::emote::Draw(dl, run.game_emote, p, ImVec2(p.x + img_h, p.y + img_h),
                        static_cast<float>(ImGui::GetTime()));
      }
      x += img_h + 2.0f;
      continue;  // l'image REMPLACE le repli « :nom: »
    }

    // Emote Discord (relais). 🔴 On appelle `Get` mais JAMAIS `Request` : une
    // bulle vit cinq secondes, elle n'a pas le temps d'attendre un
    // téléchargement, et un overlay n'a rien à faire sur le réseau. Déjà en
    // cache — parce que le chat l'a demandée en affichant la même ligne — on la
    // dessine ; sinon le fragment retombe sur son « :nom: », qui est déjà dans
    // `run.text` et sera rendu par le chemin normal plus bas.
    //
    // ⚠ Pas de place réservée non plus, contrairement au chat : rien n'arrivera
    // en cours de route, donc il n'y a pas de réorganisation à éviter.
    if (!run.emote_url.empty()) {
      const imgprev::Preview em = imgprev::Get(run.emote_url.c_str());
      if (em.state == imgprev::Preview::kReady && em.tex != nullptr && em.h > 0) {
        const float iw = img_h * static_cast<float>(em.w) / static_cast<float>(em.h);
        if (x > 0.0f && x + iw > wrap) newline();
        if (img_h > row_h) row_h = img_h;
        if (dl != nullptr) {
          const ImVec2 p(origin.x + x, origin.y + y);
          dl->AddImage(reinterpret_cast<ImTextureID>(em.tex), p,
                       ImVec2(p.x + iw, p.y + img_h), ImVec2(0, 0), ImVec2(1, 1),
                       IM_COL32(255, 255, 255, alpha255));
        }
        x += iw + 2.0f;
        continue;
      }
      // Pas encore là (ou jamais) : on laisse le « :nom: » se dessiner.
    }

    // Icône d'objet : un fragment porteur d'un id mais SANS texte.
    if (run.item_id != 0 && run.text.empty()) {
      ro::IconTex icon = ro::ItemIcon(run.item_id);
      if (icon.tex != nullptr && icon.h > 0) {
        const float iw = img_h * static_cast<float>(icon.w) / static_cast<float>(icon.h);
        if (x > 0.0f && x + iw > wrap) newline();
        if (img_h > row_h) row_h = img_h;
        if (dl != nullptr) {
          const ImVec2 p(origin.x + x, origin.y + y);
          dl->AddImage(reinterpret_cast<ImTextureID>(icon.tex), p,
                       ImVec2(p.x + iw, p.y + img_h), ImVec2(0, 0), ImVec2(1, 1),
                       IM_COL32(255, 255, 255, alpha255));
        }
        x += iw + 2.0f;
      }
      continue;
    }

    if (run.text.empty()) continue;
    const ImU32 col = run.is_link() ? link_col
                                    : (run.color != 0
                                           ? IM_COL32((run.color >> 16) & 0xff,
                                                      (run.color >> 8) & 0xff,
                                                      run.color & 0xff, alpha255)
                                           : def_col);

    // Repli MOT À MOT à l'intérieur du fragment : un lien long ne doit pas
    // déborder de la bulle, et couper au fragment ne suffirait pas.
    const char* s = run.text.c_str();
    const char* end = s + run.text.size();
    while (s < end) {
      const float avail = wrap - x;
      // ⚠ `CalcWordWrapPosition` prend une TAILLE. `CalcWordWrapPositionA`, qui
      // prenait un facteur d'échelle, n'est plus qu'un alias obsolète depuis
      // ImGui 1.92 — et `ImFont::FontSize`, dont il fallait le dériver, n'existe
      // plus du tout.
      const char* stop = font->CalcWordWrapPosition(font_px, s, end, avail);
      if (stop == s) {
        // Rien ne tient sur ce qu'il reste de la rangée. Si elle est déjà
        // entamée on la termine ; sinon la bulle est plus étroite qu'un mot et
        // il faut poser au moins un caractère, sans quoi on boucle sans fin.
        if (x > 0.0f) { newline(); continue; }
        stop = s + 1;
      }
      if (dl != nullptr)
        dl->AddText(font, font_px, ImVec2(origin.x + x, origin.y + y), col, s, stop);
      x += font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, s, stop).x;
      s = stop;
      while (s < end && *s == ' ') ++s;  // l'espace de coupure ne se reporte pas
      if (s < end) newline();
    }
  }

  if (out_size != nullptr) {
    if (x > maxw) maxw = x;
    *out_size = ImVec2(maxw, y + row_h);
  }
}

void ChatBalloon::DrawBalloons() {
  if (balloons_.empty()) return;

  void* gm = rag::ActiveModeIfReady();
  if (!gm) return;
  void* actor_mgr = Read<void*>(gm, gamescene::kGmActorMgr);
  if (!actor_mgr) return;
  void* sentinel = Read<void*>(actor_mgr, gamescene::kAmListHead);
  if (!sentinel) return;

  const ImGuiIO& io = ImGui::GetIO();
  const float disp_w = io.DisplaySize.x;
  const float disp_h = io.DisplaySize.y;
  // 🔴 Fond de z-index : derrière TOUTES nos fenêtres ImGui, jamais de
  // SetCursorPos (convention maison, cf. feedback_imgui_overlay_drawlist).
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const float font_px =
      ImGui::GetFontSize() *
      (font_scale_ < 0.5f ? 0.5f : (font_scale_ > 2.0f ? 2.0f : font_scale_));
  const float max_px = disp_w * max_width_ratio_;

  constexpr float kPadX = 6.0f;
  constexpr float kPadY = 3.0f;
  constexpr float kRound = 4.0f;
  const uint32_t now = GetTickCount();

  // Rectangles déjà posés cette frame : le natif ne fait AUCUN anti-chevauchement
  // (il se contente d'un clamp écran), deux joueurs côte à côte se recouvrent.
  std::vector<ImVec4> placed;
  placed.reserve(balloons_.size());

  auto draw_one = [&](void* actor) {
    if (actor == nullptr) return;
    const uint32_t aid = Read<uint32_t>(actor, kAct_Aid);
    auto it = balloons_.find(KeyForActor(actor, aid));
    if (it == balloons_.end()) return;
    Balloon& b = it->second;

    const int sx = Read<int32_t>(actor, kAct_ScreenX);
    const int sy = Read<int32_t>(actor, kAct_ScreenY);
    if (sx <= 0 || sy <= 0 || sx >= static_cast<int>(disp_w) ||
        sy >= static_cast<int>(disp_h))
      return;  // hors champ : pas de bulle collée au bord, contrairement au natif

    ImVec2 sz(0.0f, 0.0f);
    LayoutRuns(b.line, max_px, font_px, b.rgb, nullptr, ImVec2(0, 0), 255, &sz);
    if (sz.x <= 0.0f) return;
    const float bw = sz.x + kPadX * 2.0f;
    const float bh = sz.y + kPadY * 2.0f;

    // Hauteur du sprite, comme le natif — mais recalculée CHAQUE frame, là où
    // g_OverheadWidgetYScale est un magic static figé à la première frame (donc
    // faux après un changement de résolution en cours de session).
    const float y_scale = 81.0f * disp_h / 480.0f;
    float px = static_cast<float>(sx) - bw * 0.5f;
    float py = static_cast<float>(sy) - y_scale * Read<float>(actor, kAct_Height) -
               bh - static_cast<float>(y_offset_);

    // Remontée tant que ça chevauche une bulle déjà posée.
    for (int pass = 0; pass < 8; ++pass) {
      bool moved = false;
      for (const ImVec4& r : placed) {
        if (px < r.z && px + bw > r.x && py < r.w && py + bh > r.y) {
          py = r.y - bh - 2.0f;
          moved = true;
        }
      }
      if (!moved) break;
    }

    px = std::max(2.0f, std::min(px, disp_w - bw - 2.0f));
    py = std::max(2.0f, std::min(py, disp_h - bh - 2.0f));
    placed.push_back(ImVec4(px, py, px + bw, py + bh));

    // Fondu sur la fin — le natif disparaît d'un coup.
    float alpha = 1.0f;
    if (fade_) {
      const uint32_t age = now - b.born_ms;
      if (b.life_ms > 600 && age > b.life_ms - 600)
        alpha = static_cast<float>(b.life_ms - age) / 600.0f;
      alpha = std::max(0.0f, std::min(1.0f, alpha));
    }
    // Opacité réglable, appliquée EN FACTEUR sur le fondu et non à sa place :
    // les deux doivent se composer, sinon baisser l'opacité ferait réapparaître
    // la bulle en pleine extinction.
    alpha *= opacity_;
    const int a255 = static_cast<int>(alpha * 255.0f);

    // Le natif ne peint QUE un liseré 0x5C5C5C sur du transparent : illisible
    // dès que le décor est clair. On garde son liseré, on ajoute un fond.
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + bw, py + bh),
                      IM_COL32(16, 16, 16, static_cast<int>(alpha * 165.0f)), kRound);
    dl->AddRect(ImVec2(px, py), ImVec2(px + bw, py + bh),
                IM_COL32(0x5C, 0x5C, 0x5C, a255), kRound);

    // Deuxième passe, à l'identique : couleurs par fragment, emotes du jeu,
    // icônes d'objet. La couleur de la fenêtre native ne sert plus que de repli
    // pour les fragments sans couleur propre (ZC_NPC_CHAT, Talkie Box).
    LayoutRuns(b.line, max_px, font_px, b.rgb, dl,
               ImVec2(px + kPadX, py + kPadY), a255, nullptr);
  };

  int guard = 0;
  for (void* node = Read<void*>(sentinel, 0);
       node && node != sentinel && guard < 4096;
       node = Read<void*>(node, 0), ++guard) {
    draw_one(Read<void*>(node, kNode_Actor));
  }
  // Le joueur local, qui n'est PAS dans la liste (vérifié en live : sentinelle
  // pointant sur elle-même, acteur propre à `actorMgr+0x2C`). Sans cette ligne,
  // sa bulle existerait dans notre modèle mais ne serait jamais dessinée.
  draw_one(Read<void*>(actor_mgr, gamescene::kAmOwnPlayer));
}

// ── Réglages ─────────────────────────────────────────────────────────────────
void ChatBalloon::DrawSettings() {
  bool save = false;
  // Pas de case d'activation : la bulle suit la chatbox ImGui. Le dire, plutôt
  // que de laisser une section muette quand celle-ci est éteinte.
  if (!Active()) {
    ImGui::TextDisabled(i18n::Tr(
        "Suit la chatbox ImGui : allume-la pour remplacer les bulles. Avec la "
        "chatbox native, le client résout ses propres liens et sa bulle est "
        "correcte — la remplacer n'apporterait rien."));
  } else {
    ImGui::TextDisabled(i18n::Tr(
        "Les bulles reprennent le rendu de la chatbox : mêmes libellés de liens, "
        "mêmes couleurs. Sans elles le client afficherait les balises brutes."));
    Spacing();
    if (ro::RoCheckbox(i18n::Tr("Afficher aussi les tiennes"), &show_self_)) save = true;
    SameLine();
    if (ro::RoCheckbox(i18n::Tr("Fondu en fin d'affichage"), &fade_)) save = true;
    if (ro::RoCheckbox(i18n::Tr("Garder la durée du client (5 s fixes)"),
                       &follow_native_life_))
      save = true;

    if (!follow_native_life_) {
      ImGui::SetNextItemWidth(ro::Px(160.0f));
      if (WheelSliderInt(i18n::Tr("Durée de base (ms)"), &base_life_ms_, 1500, 10000)) save = true;
      if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
      ImGui::SetNextItemWidth(ro::Px(160.0f));
      if (WheelSliderInt(i18n::Tr("Rallonge par caractère (ms)"), &per_char_ms_, 0, 120)) save = true;
      if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
      ImGui::TextDisabled(i18n::Tr(
          "Le client donne 5 s à tout le monde : un pavé de trois lignes "
          "disparaît aussi vite qu'un « ok »."));
    }

    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (WheelSliderInt(i18n::Tr("Décalage vertical"), &y_offset_, -40, 40)) save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (WheelSliderFloat(i18n::Tr("Taille du texte"), &font_scale_, 0.7f, 1.6f)) save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (WheelSliderFloat(i18n::Tr("Largeur maximale (fraction d'écran)"),
                         &max_width_ratio_, 0.15f, 0.60f))
      save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;

    // Plancher à 0.25 : en dessous la bulle est illisible sans être invisible,
    // donc le réglage ne servirait qu'à croire à un bug d'affichage. Qui veut
    // s'en passer éteint la chatbox ImGui.
    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (WheelSliderFloat(i18n::Tr("Opacité"), &opacity_, 0.25f, 1.0f)) save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
  }

  if (save) {
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}
