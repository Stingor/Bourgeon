#include "features/gameplay/afk_screen.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "bourgeon.h"
#include "d3d9/d3d9_hook.h"
#include "features/fx/screen_fx.h"
#include "imgui.h"
#include "ragnarok/camera.h"
#include "utils/hooking/hook_manager.h"
#include "utils/i18n.h"
#include "utils/log_console.h"

// ── Adresses (client 20250716, sans ASLR : adresse Ghidra == adresse live) ───
namespace {

// `GameMode_InGame_ProcessFrame` (0x00c74a80) dessine, dans cet ordre : la scène
// 3D, le radar natif, puis TOUTE l'interface du client en un seul appel :
//
//     0x00c74fc2  call GameMode_DrawMiniMap          ← déjà vetoé par Minimap
//     0x00c74fcc  call sub_A49CC0
//     0x00c74fd6  call UIWindowMgr_RenderWindows     ← ICI
//     0x00c74fdb  ...
//
// Sauter ce dernier appel efface d'un coup les fenêtres du client, les
// étiquettes de nom au-dessus des acteurs et les bulles de chat natives — tout
// ce que le gestionnaire de fenêtres rend. `UIWindowMgr_RenderWindows`
// (0x00a48fd0) ne fait QUE dessiner : elle parcourt sa liste, blitte chaque
// fenêtre et vide le lot de primitives. Aucun état de jeu n'en dépend, ce qui
// rend le veto réversible à la frame près — c'est très exactement pourquoi on ne
// passe pas par le « cacher l'interface » natif, qui, lui, ferme des fenêtres
// (le raisonnement complet est en tête d'afk_screen.h).
//
// ⚠ Le détour va sur le SITE D'APPEL, jamais sur la fonction : c'est la règle
// déjà apprise sur le radar, dont le prologue installe un cadre SEH qu'un
// JMP-hook ne sait pas relayer.
constexpr uintptr_t kRenderWindowsCall  = 0x00c74fd6;  // call UIWindowMgr_RenderWindows
constexpr uintptr_t kRenderWindowsAfter = 0x00c74fdb;  // l'instruction suivante
// L'appel attendu, vérifié avant de poser quoi que ce soit : sur un exe qui
// n'est pas celui qu'on croit, mieux vaut un journal qu'un détour au hasard au
// milieu d'une instruction.
constexpr uint8_t kRenderWindowsCallBytes[5] = {0xE8, 0xF5, 0x3F, 0xDD, 0xFF};

// Plafond de distance de vue du moteur, celui-là même que borne
// `Camera_ApplyViewDistanceClamp`. Lu, jamais écrit : ScreenFx est seul à le
// gonfler (option « dézoom étendu »), et la veille se contente de rester en
// dessous — au-delà, le far-clip et le brouillard découvrent les bords du monde.
// ⚠ Ce global ne nous appartient pas : le moteur le réécrit à chaque bascule de
// la commande /zoom. D'où une lecture À CHAQUE frame plutôt qu'une valeur mise
// de côté à l'entrée en veille.
constexpr uintptr_t kCamZoomMaxOutdoor = 0x012291c0;

// Bornes de la « hauteur au-dessus de l'horizon » proposée au joueur, en degrés
// positifs (le moteur, lui, veut l'opposé — cf. StepCamera). Le repos du client
// est 45 et sa propre plage ne va que de 25 à 65 ; on laisse monter plus haut,
// parce que c'est justement là que la vue devient cinématographique et que rien
// ne re-clampe pendant la veille, mais pas jusqu'à 90 : pile à la verticale, la
// direction du regard devient dégénérée.
constexpr float kWakeSpeedup = 3.0f;

constexpr float kTiltMinDeg = 15.0f;
constexpr float kTiltMaxDeg = 85.0f;

}  // namespace

// Portée fichier et NON anonyme : l'asm inline du stub nu les résout par nom.
static AfkScreen* g_afk_owner = nullptr;
static void* g_tramp_render_windows = nullptr;

// L'interface du client doit-elle disparaître de CETTE frame ? Appelée depuis le
// stub, donc en plein dessin du jeu : pas de frame ImGui autour, et surtout rien
// à dérouler ici — juste deux lectures.
bool NativeUiVetoed() {
  return g_afk_owner != nullptr && g_afk_owner->hiding_ui();
}

__declspec(naked) static void RenderWindowsStub() {
  __asm {
    pushad
    call NativeUiVetoed
    test al, al
    popad                 // POPAD ne touche pas aux drapeaux : le test tient
    jz   draw
    // ⚠ Adresse en DUR, et non la constante nommée plus haut : dans l'asm inline
    // MSVC, un identifiant C++ désigne un EMPLACEMENT mémoire — `jmp kXxx`
    // sauterait à son CONTENU.
    mov  eax, 0C74FDBh    // = kRenderWindowsAfter, l'instruction après l'appel
    jmp  eax              // l'appel n'a pas lieu ; eax est volatil pour l'appelant
  draw:
    jmp  [g_tramp_render_windows]
  }
}

// ── Le compteur d'inactivité ─────────────────────────────────────────────────
namespace afk {
namespace {

// Écrits par le fil de la fenêtre, lus par le fil de rendu. Ce sont les mêmes
// dans ce client, mais rien ne le garantit par écrit : des atomiques coûtent une
// instruction et ferment la question.
//
// 🔴 DEUX horodatages, et les confondre casse l'un ou l'autre :
//   • `g_last_input` — TOUTE activité, sans filtre. C'est lui qui repousse
//     l'endormissement. Le filtrer ferait s'endormir le client pendant que le
//     joueur promène sa souris, au seul motif qu'il a demandé un réveil au
//     clavier.
//   • `g_last_wake` — la seule activité qui a le DROIT de réveiller, selon le
//     réglage. C'est lui que la veille interroge une fois endormie.
// Hors du mode « clavier et souris », les deux divergent : c'est tout l'objet du
// réglage.
std::atomic<uint32_t> g_last_input{0};
std::atomic<uint32_t> g_last_wake{0};
std::atomic<bool>     g_sleeping{false};
std::atomic<int>      g_wake_mode{kWakeAny};
// Dernière position connue du curseur, pour distinguer un vrai mouvement du
// WM_MOUSEMOVE que Windows renvoie quand la fenêtre bouge sous une souris
// immobile. Sans ce test, une animation quelconque sous le curseur suffirait à
// empêcher la veille indéfiniment.
int  g_last_mx = INT_MIN;
int  g_last_my = INT_MIN;
// Boutons dont on a avalé l'appui : leur relâchement doit l'être aussi. Un
// bouton dont le jeu a vu le UP sans avoir vu le DOWN, c'est un clic fantôme —
// et selon la fenêtre survolée, un glisser-déposer qui se termine dans le vide.
unsigned g_swallowed_buttons = 0;

constexpr unsigned kBtnLeft   = 1u << 0;
constexpr unsigned kBtnRight  = 1u << 1;
constexpr unsigned kBtnMiddle = 1u << 2;

}  // namespace

uint32_t LastInputMs() { return g_last_input.load(); }
uint32_t LastWakeInputMs() { return g_last_wake.load(); }

void SetWakeMode(int mode) { g_wake_mode.store(mode); }

void SetSleeping(bool sleeping) {
  g_sleeping.store(sleeping);
  if (!sleeping) return;
  // En entrant en veille on repart d'un masque propre : un bouton resté marqué
  // d'une veille précédente ferait avaler le premier relâchement de celle-ci.
  g_swallowed_buttons = 0;
}

bool FilterMessage(unsigned int msg, intptr_t lparam) {
  unsigned button = 0;      // quel bouton ce message concerne-t-il ?
  bool is_press    = false; // appui (par opposition à relâchement)
  bool key_release = false; // relâchement de touche
  bool from_key    = false; // vient du clavier
  bool from_mouse  = false; // vient de la souris (molette et mouvement compris)

  switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: button = kBtnLeft;   is_press = true; from_mouse = true; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: button = kBtnRight;  is_press = true; from_mouse = true; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: button = kBtnMiddle; is_press = true; from_mouse = true; break;
    case WM_LBUTTONUP: button = kBtnLeft;   from_mouse = true; break;
    case WM_RBUTTONUP: button = kBtnRight;  from_mouse = true; break;
    case WM_MBUTTONUP: button = kBtnMiddle; from_mouse = true; break;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: from_mouse = true; break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_CHAR:
      from_key = true;
      break;
    case WM_KEYUP: case WM_SYSKEYUP:
      from_key = true;
      key_release = true;
      break;
    case WM_MOUSEMOVE: {
      const int mx = static_cast<short>(LOWORD(lparam));
      const int my = static_cast<short>(HIWORD(lparam));
      if (mx == g_last_mx && my == g_last_my) return false;  // pas un mouvement
      g_last_mx = mx;
      g_last_my = my;
      from_mouse = true;
      break;
    }
    default:
      // 🔴 Tout le reste ne compte PAS, et notamment WM_SETCURSOR, WM_PAINT,
      // WM_TIMER ou WM_NCHITTEST : le client en reçoit à la pelle sans que
      // personne n'ait touché à rien. Les compter, c'est une veille qui ne se
      // déclenche jamais.
      return false;
  }

  const bool was_sleeping = g_sleeping.load();
  const int  mode = g_wake_mode.load();
  // 🔴 Le droit de réveiller s'accorde à la SOURCE, pas au message : c'est ce qui
  // garde l'avalage symétrique. Avaler un appui sans avaler son relâchement
  // laisserait le client croire la touche encore enfoncée — un personnage qui
  // marche tout seul, et personne pour faire le rapprochement.
  const bool source_may_wake = (mode == kWakeAny) ||
                               (mode == kWakeKeyboard && from_key) ||
                               (mode == kWakeMouse && from_mouse);
  // Un RELÂCHEMENT ne réveille jamais : c'est la fin d'un geste commencé avant,
  // pas une intention neuve. Sans cette règle, le raccourci qui LANCE la veille
  // la terminerait de son propre relâchement, une frame plus tard.
  const bool is_release = key_release || (button != 0 && !is_press);
  const bool wakes_now = source_may_wake && !is_release;

  // Le relâchement suit le sort de son appui, veille ou pas : c'est ce qui rend
  // l'avalage symétrique même si le réveil a eu lieu entre les deux.
  //
  // 🔴 Et un relâchement avalé ne compte PAS comme activité. Il ne s'agit pas
  // d'un raffinement : c'est ce qui rend le bouton « Essayer maintenant » du
  // panneau utilisable. Son clic endort le client entre l'appui et le
  // relâchement, et ce relâchement-là — le second temps d'un geste déjà fini —
  // rouvrirait les yeux aussitôt fermés. La veille durerait une frame, et le
  // bouton passerait pour cassé.
  if (button != 0 && !is_press) {
    if ((g_swallowed_buttons & button) == 0) {
      g_last_input.store(GetTickCount());
      return false;
    }
    g_swallowed_buttons &= ~button;
    return true;
  }

  const uint32_t now = GetTickCount();
  g_last_input.store(now);               // repousse l'endormissement : TOUT compte
  if (wakes_now) g_last_wake.store(now); // réveille : seulement ce qui en a le droit

  if (!was_sleeping) return false;       // hors veille, rien n'est jamais avalé

  // ── En veille : ce qui n'a pas le droit de réveiller n'a pas non plus le
  // droit d'AGIR ────────────────────────────────────────────────────────────
  // Une touche qui traverserait sans réveiller lancerait un skill sur un monde
  // que le joueur ne voit pas, sous un angle qui n'est pas le sien. Demander
  // « seule la souris me réveille », c'est demander que le clavier soit sans
  // effet — pas qu'il agisse en aveugle.
  if (!source_may_wake) return true;

  // Le clic qui réveille est avalé, lui aussi, et pour une raison qui lui est
  // propre : le décor a tourné, il tomberait ailleurs que là où l'on croit
  // viser. Le clavier, lui, passe — une touche porte une intention explicite.
  if (button == 0) return false;
  g_swallowed_buttons |= button;
  return true;
}

void SwallowHeldButtons() {
  // Ce qui est DÉJÀ enfoncé au moment où l'on s'endort verra son relâchement
  // avalé, exactement comme si son appui l'avait été. Sans quoi le geste
  // commencé avant la veille viendrait la terminer.
  struct { int vk; unsigned mask; } kButtons[] = {
      {VK_LBUTTON, kBtnLeft}, {VK_RBUTTON, kBtnRight}, {VK_MBUTTON, kBtnMiddle}};
  for (const auto& b : kButtons)
    if (GetAsyncKeyState(b.vk) & 0x8000) g_swallowed_buttons |= b.mask;
}

}  // namespace afk

// ── Cycle de vie ─────────────────────────────────────────────────────────────

AfkScreen::AfkScreen() {
  g_afk_owner = this;
  // Capture de la caméra et détour du dessin natif posés INCONDITIONNELLEMENT :
  // au chargement des modules le timestamp du client n'est pas encore connu (il
  // l'est plus tard, dans RagnarokClient::Initialize), si bien qu'un install
  // gaté dessus ne se ferait jamais — la leçon du F9 muet de FpsView. Les
  // méthodes d'exécution, elles, gardent la garde.
  ro::camera::Install();

  const uint8_t* site = reinterpret_cast<const uint8_t*>(kRenderWindowsCall);
  if (memcmp(site, kRenderWindowsCallBytes, sizeof(kRenderWindowsCallBytes)) != 0) {
    LogError(
        "AfkScreen : 0x{:x} ne porte pas l'appel attendu au rendu de l'interface "
        "native — détour NON posé, l'écran de veille laissera l'interface du "
        "client affichée.",
        kRenderWindowsCall);
    return;
  }
  g_tramp_render_windows = hooking::HookManager::Instance().SetHook(
      hooking::HookType::kJmpHook, reinterpret_cast<uint8_t*>(kRenderWindowsCall),
      reinterpret_cast<uint8_t*>(&RenderWindowsStub));
}

// ── État ─────────────────────────────────────────────────────────────────────

bool AfkScreen::hiding_cursor() const {
  return cfg_.hide_cursor &&
         (phase_ == Phase::kEntering || phase_ == Phase::kHeld);
}

bool AfkScreen::hiding_ui() const {
  // Pendant la rampe de SORTIE l'interface est déjà rendue : le joueur vient
  // d'agir, il lui faut ses barres et son chat tout de suite, quand bien même la
  // caméra met encore deux secondes à revenir se poser.
  return cfg_.hide_ui &&
         (phase_ == Phase::kEntering || phase_ == Phase::kHeld);
}

float AfkScreen::EaseBlend() const {
  if (phase_ == Phase::kAwake) return 0.0f;
  if (phase_ == Phase::kHeld)  return 1.0f;
  // Le réglage du joueur décrit l'ENTRÉE ; la sortie en est une fraction.
  float ease_s = std::max(cfg_.ease_s, 0.05f);
  if (phase_ == Phase::kLeaving) ease_s /= kWakeSpeedup;
  float t = static_cast<float>(GetTickCount() - phase_start_) / (ease_s * 1000.0f);
  t = std::clamp(t, 0.0f, 1.0f);
  // Lissage en S : départ et arrivée sans à-coup. Le moteur lisse déjà sa propre
  // course vers la cible, mais il le fait à vitesse constante vers une cible qui,
  // elle, bondirait — c'est la cible qu'il faut adoucir.
  const float smooth = t * t * (3.0f - 2.0f * t);
  return (phase_ == Phase::kLeaving) ? (1.0f - smooth) : smooth;
}

void AfkScreen::BeginSleep() {
  if (!ro::camera::Get()) return;  // pas encore en jeu : on retentera

  base_pitch_ = ro::camera::Read(ro::camera::kTgtPitch);
  base_yaw_   = ro::camera::Read(ro::camera::kTgtYaw);
  base_dist_  = ro::camera::Read(ro::camera::kTgtDist);
  base_ok_    = true;

  spin_deg_      = 0.0f;
  phase_         = Phase::kEntering;
  phase_start_   = GetTickCount();
  sleep_start_   = phase_start_;
  last_step_ms_  = phase_start_;
  afk::SetSleeping(true);
}

void AfkScreen::Wake() {
  if (phase_ == Phase::kAwake || phase_ == Phase::kLeaving) return;
  // L'orbite accumulée est ramenée au plus court : après vingt minutes de veille
  // elle peut valoir plusieurs tours, et les dérouler à l'envers ferait tourner
  // le décor pendant un temps absurde alors que le joueur veut jouer.
  spin_at_leave_ = std::fmod(spin_deg_, 360.0f);
  if (spin_at_leave_ >  180.0f) spin_at_leave_ -= 360.0f;
  if (spin_at_leave_ < -180.0f) spin_at_leave_ += 360.0f;
  spin_deg_    = spin_at_leave_;
  phase_       = Phase::kLeaving;
  phase_start_ = GetTickCount();
  afk::SetSleeping(false);
}

void AfkScreen::AbortSleep() {
  if (phase_ == Phase::kAwake) return;
  // La pose de repos est remise si la caméra existe encore. Quand elle a disparu
  // avec son CGameMode — le cas d'un changement de carte —, `ro::camera::Write`
  // ne fait rien, et c'est très bien : le moteur reconstruit sa caméra de toute
  // façon, et il vaut mieux ne rien écrire du tout que d'écrire dans ce qui fut
  // un objet.
  if (base_ok_) {
    ro::camera::Write(ro::camera::kTgtPitch, base_pitch_);
    ro::camera::Write(ro::camera::kTgtYaw,   base_yaw_);
    ro::camera::Write(ro::camera::kTgtDist,  base_dist_);
  }
  base_ok_ = false;
  phase_   = Phase::kAwake;
  afk::SetSleeping(false);
  RestorePostFx();
}

void AfkScreen::StartNow() {
  if (phase_ != Phase::kAwake) return;
  BeginSleep();
  // Le clic qui vient d'appuyer sur « Essayer maintenant » est encore enfoncé :
  // son relâchement ne doit pas être lu comme le geste qui met fin à la veille.
  if (phase_ != Phase::kAwake) afk::SwallowHeldButtons();
}

// ── Décision ─────────────────────────────────────────────────────────────────

void AfkScreen::OnTick() {
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;

  // Le filtre d'entrées tourne sur le fil de la fenêtre et n'a pas d'autre moyen
  // d'atteindre la configuration : on la lui repose à chaque battement, comme
  // l'état de veille.
  afk::SetWakeMode(cfg_.wake_on);

  auto& app = Bourgeon::Instance();
  // Hors du monde, ou pendant un chargement de carte : ni veille ni maintien.
  // Sortir du monde en veille laisserait la teinte posée sur un écran de
  // sélection de personnage que personne ne saurait plus éteindre.
  if (!app.IsGameActive() || app.IsMapLoading()) {
    AbortSleep();
    return;
  }

  const uint32_t last = afk::LastInputMs();
  const uint32_t now  = GetTickCount();
  // Aucune entrée depuis le lancement : on prend « à l'instant » plutôt que
  // l'époque zéro, sinon le client s'endormirait avant le premier geste.
  const uint32_t idle_ms = (last == 0) ? 0u : (now - last);

  if (phase_ == Phase::kAwake) {
    if (!cfg_.enabled) return;
    const uint32_t delay_ms =
        static_cast<uint32_t>(std::clamp(cfg_.delay_s, 10, 900)) * 1000u;
    if (idle_ms >= delay_ms) BeginSleep();
    return;
  }

  // En veille : la moindre entrée réveille.
  //
  // ⚠ `cfg_.enabled` ne figure PAS dans ce test, et ce n'est pas un oubli : il
  // gouverne le DÉCLENCHEMENT automatique (plus haut), pas la veille en cours.
  // L'y remettre couperait net toute veille lancée à la main — au raccourci ou
  // au bouton d'essai — chez quiconque n'a pas coché la mise en veille
  // automatique, c'est-à-dire précisément ceux à qui ce geste sert.
  //
  // La question posée est « une entrée est-elle survenue APRÈS l'endormissement ? »,
  // et non « l'inactivité est-elle retombée sous un certain seuil ? » : un seuil
  // aurait été un chiffre arbitraire à choisir, alors que la comparaison de dates
  // répond exactement. La soustraction est SIGNÉE parce que GetTickCount repasse
  // par zéro tous les quarante-neuf jours — un serveur qui tourne, ça arrive.
  // 🔴 `LastWakeInputMs`, PAS `LastInputMs` : c'est ici, et ici seulement, que le
  // réglage « qui a le droit de me réveiller » se fait sentir. Le compteur
  // d'inactivité au-dessus, lui, prend tout — sans quoi demander un réveil au
  // clavier ferait s'endormir le client pendant qu'on promène la souris.
  const uint32_t woke = afk::LastWakeInputMs();
  const bool input_since_sleep =
      (woke != 0) && (static_cast<int32_t>(woke - sleep_start_) > 0);
  if (input_since_sleep) Wake();
}

// ── Mouvement ────────────────────────────────────────────────────────────────

void AfkScreen::OnGameFramePulse() {
  if (phase_ == Phase::kAwake) return;
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;

  const uint32_t now = GetTickCount();
  float dt = static_cast<float>(now - last_step_ms_) / 1000.0f;
  last_step_ms_ = now;
  // Une frame anormalement longue (chargement, fenêtre réduite, point d'arrêt)
  // ferait sauter l'orbite d'un quart de tour d'un coup : on la borne.
  dt = std::clamp(dt, 0.0f, 0.25f);

  const float blend = EaseBlend();
  StepCamera(dt);
  ApplyPostFx(blend);

  if (phase_ == Phase::kEntering && blend >= 1.0f) phase_ = Phase::kHeld;
  if (phase_ == Phase::kLeaving && blend <= 0.0f) {
    // Repose exactement la pose de départ : le lissage du moteur a beau avoir
    // convergé, il l'a fait vers une valeur interpolée, pas vers la mesure
    // d'origine. Sans cette dernière écriture, chaque veille décalerait la
    // caméra d'un cheveu, et vingt veilles d'un angle bien visible.
    if (base_ok_) {
      ro::camera::Write(ro::camera::kTgtPitch, base_pitch_);
      ro::camera::Write(ro::camera::kTgtYaw,   base_yaw_);
      ro::camera::Write(ro::camera::kTgtDist,  base_dist_);
    }
    base_ok_ = false;
    phase_   = Phase::kAwake;
    RestorePostFx();
  }
}

void AfkScreen::StepCamera(float dt) {
  if (!base_ok_ || !ro::camera::Get()) return;

  const float blend = EaseBlend();

  if (phase_ == Phase::kLeaving) {
    // L'orbite se résorbe avec la rampe, en même temps que le recul et la
    // plongée : tout revient d'un seul mouvement.
    spin_deg_ = spin_at_leave_ * blend;
  } else {
    spin_deg_ += cfg_.spin_deg_s * dt;
    // Replié pour lui seul, et SANS toucher à la pose courante : le lissage
    // (`Camera_LerpCurrentTowardTarget` 0x00a7ab90) ramène déjà la cible dans
    // [0, 360[ et rejoint le courant par le chemin le plus court (`j` borné à
    // ±180). Il n'y a donc aucun tour à dérouler à la main — seulement notre
    // accumulateur à empêcher de grandir sans fin, où le flottant finirait par
    // perdre ses décimales.
    spin_deg_ = std::fmod(spin_deg_, 360.0f);
  }

  // Distance de recul, bornée au plafond de vue du moteur : au-delà, le far-clip
  // et le brouillard découvrent les bords du monde (c'est la même limite que
  // celle qui plafonne le dézoom étendu de ScreenFx).
  const float target_dist =
      base_dist_ * std::clamp(cfg_.zoom_factor, 1.0f, 2.5f);
  const float max_dist = *reinterpret_cast<const float*>(kCamZoomMaxOutdoor);
  const float dist = std::min(target_dist, max_dist > 1.0f ? max_dist : target_dist);

  // 🔴 LE PITCH EST NÉGATIF. Le réglage se lit « hauteur au-dessus de
  // l'horizon » — 0 rasant, 90 à la verticale — parce que c'est ce qu'un joueur
  // règle ; le moteur, lui, veut l'opposé, et se tromper de signe ne donne pas
  // une caméra basse mais une caméra SOUS LE TERRAIN.
  //
  // La preuve est dans le builder (`FUN_00a7ae20`) : il pose
  // `eye = lookat + (-dist) * ligne2(RotX(pitch))`, soit `eye.Y = dist*sin(pitch)`,
  // sur un axe Y qui pointe vers le BAS. Un pitch positif enfonce donc l'œil, où
  // le clamp anti-sol de la même fonction (`if (eye.Y > Terrain_GetHeightAt(...))`)
  // le raccroche au sol — d'où une vue à ras de terre pour TOUTE valeur positive,
  // sans que rien n'ait l'air de répondre au réglage.
  // Le repos vaut -45, et `Camera_ApplyViewDistanceClamp` borne à
  // [-65, -25] dehors ([-55, -35] à l'intérieur) : au-delà, la vue reste juste
  // pendant la veille — cette fonction n'y tourne pas — et le moteur la ramènera
  // dans sa plage dès que le joueur reprendra la main sur sa caméra.
  const float want_pitch = -std::clamp(cfg_.tilt_deg, kTiltMinDeg, kTiltMaxDeg);
  const float pitch = base_pitch_ + (want_pitch - base_pitch_) * blend;

  ro::camera::Write(ro::camera::kTgtPitch, pitch);
  ro::camera::Write(ro::camera::kTgtYaw,   base_yaw_ + spin_deg_);
  ro::camera::Write(ro::camera::kTgtDist,  base_dist_ + (dist - base_dist_) * blend);
}

// ── Teinte ───────────────────────────────────────────────────────────────────

void AfkScreen::ApplyPostFx(float blend) {
  auto* fx_owner = Bourgeon::Instance().screen_fx();
  if (!fx_owner) return;
  const float v = std::clamp(cfg_.vignette,   0.0f, 1.0f) * blend;
  const float d = std::clamp(cfg_.desaturate, 0.0f, 1.0f) * blend;
  const float g = std::clamp(cfg_.grain,      0.0f, 1.0f) * blend;
  if (v <= 0.0f && d <= 0.0f && g <= 0.0f) {
    RestorePostFx();
    return;
  }

  // COMPOSER, jamais écraser : on part du réglage du joueur et on n'ajoute que
  // par-dessus. Sa vignette à lui ne doit pas s'effacer parce que la nôtre est
  // plus discrète, et sa désaturation doit se cumuler avec la nôtre plutôt que
  // de lui céder la place.
  D3D9PostFx fx = fx_owner->fx();
  fx.enabled    = true;
  fx.vignette   = std::max(fx.vignette, v);
  fx.grain      = std::max(fx.grain, g);
  fx.saturation = fx.saturation * (1.0f - d);
  // 🔴 La désaturation passe par `saturation` et NON par `filter = 1` (le
  // niveaux-de-gris tout fait) : un filtre est un interrupteur, il ne se fond
  // pas. Le noir et blanc doit s'installer avec la rampe, comme le reste.
  D3D9_SetPostFx(fx);
  fx_pushed_ = true;
}

void AfkScreen::RestorePostFx() {
  if (!fx_pushed_) return;
  fx_pushed_ = false;
  // Le réglage du joueur n'a jamais bougé : il suffit de le redemander.
  if (auto* fx_owner = Bourgeon::Instance().screen_fx()) fx_owner->Apply();
}

// ── Horloge ──────────────────────────────────────────────────────────────────

void AfkScreen::OnRenderUI() {
  if (phase_ == Phase::kAwake || !cfg_.show_clock) return;

  const float blend = EaseBlend();
  if (blend <= 0.01f) return;

  char hhmm[16] = {};
  std::time_t now_t = std::time(nullptr);
  std::tm local{};
  if (localtime_s(&local, &now_t) == 0)
    std::strftime(hhmm, sizeof(hhmm), "%H:%M", &local);

  const uint32_t away_min = (GetTickCount() - sleep_start_) / 60000u;
  char away[96] = {};
  if (away_min < 1)
    std::snprintf(away, sizeof(away), "%s", i18n::Tr("Absent à l'instant"));
  else if (away_min < 60)
    std::snprintf(away, sizeof(away), i18n::Tr("Absent depuis %u min"), away_min);
  else
    std::snprintf(away, sizeof(away), i18n::Tr("Absent depuis %uh%02u"),
                  away_min / 60u, away_min % 60u);

  // Ancrage sur la grille 3×3 : la colonne et la ligne donnent À LA FOIS la
  // position sur l'écran et le pivot de la fenêtre. C'est ce qui fait qu'un
  // ancrage à droite colle son BORD DROIT à la marge, sans cas particulier —
  // une fenêtre auto-dimensionnée dont on ne bougerait que la position
  // déborderait de l'écran dès que le texte s'allonge.
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const int anchor = std::clamp(cfg_.clock_anchor, 0, 8);
  const float col = static_cast<float>(anchor % 3) * 0.5f;  // 0 | 0.5 | 1
  const float row = static_cast<float>(anchor / 3) * 0.5f;
  const float margin = static_cast<float>(std::clamp(cfg_.clock_margin, 0, 400));
  ImGui::SetNextWindowPos(
      ImVec2(vp->WorkPos.x + margin + (vp->WorkSize.x - 2.0f * margin) * col,
             vp->WorkPos.y + margin + (vp->WorkSize.y - 2.0f * margin) * row),
      ImGuiCond_Always, ImVec2(col, row));
  ImGui::SetNextWindowBgAlpha(0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  // ⚠ `NoInputs` n'est pas décoratif : sans lui, cette fenêtre-ci intercepterait
  // le clic de réveil, et le hook de WndProc l'aurait avalé pour rien.
  if (ImGui::Begin("##afk_clock", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    // Le fondu passe par le style global : `GetColorU32` le multiplie dans la
    // couleur du texte, donc l'ombre s'estompe avec lui sans qu'on ait à la
    // teinter nous-mêmes.
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, blend);
    DrawClockLine(hhmm, std::clamp(cfg_.clock_scale, 0.5f, 6.0f), cfg_.clock_col);
    if (cfg_.show_away) {
      // La seconde ligne suit la première plutôt que d'avoir sa propre taille et
      // sa propre couleur : ce sont deux morceaux d'une même phrase, et deux
      // réglages de plus n'apporteraient qu'une façon supplémentaire de les
      // désaccorder. Elle hérite donc de l'échelle, en plus petit, et de la
      // couleur, en plus discret.
      const uint32_t faded =
          (cfg_.clock_col & ~IM_COL32_A_MASK) |
          (static_cast<uint32_t>(((cfg_.clock_col >> IM_COL32_A_SHIFT) & 0xFF) * 0.6f)
           << IM_COL32_A_SHIFT);
      DrawClockLine(away, std::clamp(cfg_.clock_scale * 0.45f, 0.5f, 3.0f), faded);
    }
    ImGui::PopStyleVar();
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

// Une ligne centrée dans la fenêtre, avec son ombre portée si elle est demandée.
//
// 🔴 UNE OMBRE, PAS UN CONTOUR — la règle est déjà celle de l'overlay FPS : un
// contour demande quatre passes et se voit comme un halo, là où une ombre d'un
// pixel suffit à décoller le texte de n'importe quel décor. Et ici le décor est
// le jeu lui-même, en mouvement : sans elle, une heure blanche disparaît dès que
// la caméra passe devant une falaise claire.
void AfkScreen::DrawClockLine(const char* text, float scale, uint32_t color) {
  ImGui::SetWindowFontScale(scale);
  const float x =
      (ImGui::GetWindowWidth() - ImGui::CalcTextSize(text).x) * 0.5f;
  const float y = ImGui::GetCursorPosY();
  if (cfg_.clock_shadow) {
    // L'ombre prend l'alpha du texte : à couleur transparente, elle s'efface
    // avec lui au lieu de rester seule en noir.
    ImGui::PushStyleColor(
        ImGuiCol_Text,
        IM_COL32(0, 0, 0, (color >> IM_COL32_A_SHIFT) & 0xFF));
    ImGui::SetCursorPos(ImVec2(x + 1.0f, y + 1.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::SetCursorPos(ImVec2(x, y));  // par-dessus, à la bonne place
  } else {
    ImGui::SetCursorPosX(x);
  }
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::SetWindowFontScale(1.0f);
}

void AfkScreen::OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) {
  if (mode_type == ModeMgr::ModeType::kGame) return;
  // Quitter le monde coupe net : pas de rampe de sortie à dérouler sur un écran
  // qui n'a plus de caméra, et surtout pas de teinte laissée derrière soi.
  AbortSleep();
}
