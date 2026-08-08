#include "features/fx/zone_recorder.h"

#include <windows.h>

#include <shellapi.h>     // ShellExecuteA (« Ouvrir le dossier »)
#include <shlobj_core.h>  // DROPFILES (CF_HDROP) ; tire ole2.h -> DROPEFFECT_COPY

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "imgui.h"

#include "bourgeon.h"
#include "d3d9/d3d9_hook.h"
#include "features/hotkey_util.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/staff_gate.h"
#include "ui/ro_imgui.h"
#include "utils/game_paths.h"
#include "utils/gif_writer.h"
#include "utils/log_console.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// Le client rend-il en DirectX 7 ? (défini dans le proxy DirectDraw ; d3d9_hook.cc
// le déclare de la même façon.) Toute la capture passe par le backbuffer D3D9.
extern bool g_imgui_dx7_active;

namespace {

// Le module vivant, pour le pont entre le hook Present (fonction libre) et
// l'instance. Un seul enregistreur : la fenêtre de capture de Present est unique.
ZoneRecorder* g_recorder = nullptr;

void PostFrameThunk() {
  if (g_recorder) g_recorder->OnPostFrame();
}

// Plafond des images gardées en mémoire le temps de l'encodage. Le client est un
// processus 32 bits qui a déjà ses GRF, ses textures et ses sons en mémoire :
// au-delà, ce n'est pas l'enregistrement qui échoue proprement, c'est le jeu qui
// tombe. 128 Mio laissent ~140 images en 640x360, soit 14 s à 10 images/s.
constexpr size_t kMaxCaptureBytes = 128u * 1024u * 1024u;

constexpr int kMinZonePx = 32;  // en deçà, un tracé est un clic qui a glissé

// Durée d'affichage d'un message flottant (copie faite, recadrage refusé). Assez
// long pour être lu sans quitter le jeu des yeux, assez court pour ne pas traîner
// sur l'écran pendant qu'on cadre la prise suivante.
constexpr unsigned long kToastMs = 3000;

// <jeu>\screenshot\bourgeon_zone_AAAAMMJJ_HHMMSS.gif (dossier créé au besoin).
std::string TimestampedGifPath() {
  const std::string dir = paths::InGameDir("screenshot");
  CreateDirectoryA(dir.c_str(), nullptr);
  SYSTEMTIME t;
  GetLocalTime(&t);
  char name[64];
  std::snprintf(name, sizeof(name), "\\bourgeon_zone_%04d%02d%02d_%02d%02d%02d.gif",
                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  return dir + name;
}

// 🔴 On copie le GIF EN TANT QUE FICHIER (CF_HDROP), pas en tant qu'image. Un
// CF_DIB/CF_BITMAP n'emporterait que la première image et l'animation — tout
// l'intérêt du GIF — serait perdue. Avec CF_HDROP, un Ctrl+V dans Discord (ou
// dans un dossier de l'explorateur) colle le .gif complet, exactement comme un
// copier-coller de fichier depuis l'explorateur.
bool CopyGifToClipboard(const std::string& path) {
  // CF_HDROP = un DROPFILES suivi de la liste des chemins, terminée par un NUL
  // supplémentaire (d'où les +2 : celui du chemin, puis celui de la liste).
  const size_t bytes = sizeof(DROPFILES) + path.size() + 2;
  HGLOBAL files = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!files) return false;
  auto* drop = static_cast<DROPFILES*>(GlobalLock(files));
  if (!drop) {
    GlobalFree(files);
    return false;
  }
  ZeroMemory(drop, bytes);
  drop->pFiles = sizeof(DROPFILES);  // décalage du premier chemin
  drop->fWide  = FALSE;              // chemins ANSI (comme tout paths::)
  std::memcpy(reinterpret_cast<char*>(drop) + sizeof(DROPFILES), path.c_str(),
              path.size());
  GlobalUnlock(files);

  // Sans « Preferred DropEffect », un collage dans l'explorateur peut être traité
  // comme un DÉPLACEMENT : le GIF disparaîtrait alors de <jeu>\screenshot.
  HGLOBAL effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
  if (effect) {
    if (auto* value = static_cast<DWORD*>(GlobalLock(effect))) {
      *value = DROPEFFECT_COPY;
      GlobalUnlock(effect);
    } else {
      GlobalFree(effect);
      effect = nullptr;
    }
  }

  // OpenClipboard(nullptr) : le presse-papier s'attache au thread appelant, comme
  // le fait ImGui. Aucune pompe de messages n'est ouverte ici — rien qui puisse
  // figer la frame en cours.
  if (!OpenClipboard(nullptr)) {
    // Échec normal et transitoire : une autre application tient le presse-papier
    // à cet instant. Rien à réparer, mais on trace le code pour le diagnostic.
    LogError("ZoneRecorder: OpenClipboard refusé (code {})", GetLastError());
    GlobalFree(files);
    if (effect) GlobalFree(effect);
    return false;
  }
  EmptyClipboard();
  // SetClipboardData réussi = le presse-papier devient PROPRIÉTAIRE du bloc ;
  // le libérer nous-mêmes serait un double free. En échec, il reste à nous.
  const bool ok = SetClipboardData(CF_HDROP, files) != nullptr;
  if (!ok) GlobalFree(files);
  if (effect) {
    const UINT format = RegisterClipboardFormatA(i18n::Tr("Preferred DropEffect"));
    if (format == 0 || SetClipboardData(format, effect) == nullptr)
      GlobalFree(effect);
  }
  CloseClipboard();
  return ok;
}

// Ouvre l'explorateur sur le dossier ET met le GIF en surbrillance, plutôt que de
// lâcher le joueur dans un screenshot\ qui contient déjà des dizaines de fichiers.
void RevealInExplorer(const std::string& path) {
  // /select veut le chemin entre guillemets : le dossier du jeu peut contenir des
  // espaces, et l'argument serait sinon coupé au premier.
  const std::string arg = "/select,\"" + path + "\"";
  // ShellExecute ne fait que poster la demande au shell (cf. charsel::
  // OpenBackgroundFolder) : pas de dialogue bloquant sur le fil principal.
  ShellExecuteA(nullptr, "open", "explorer.exe", arg.c_str(), nullptr,
                SW_SHOWNORMAL);
}

// Cartouche sombre + texte, la seule forme d'affichage flottant du module.
void DrawBadge(ImDrawList* dl, const ImVec2& pos, const char* text, ImU32 color) {
  const ImVec2 ts = ImGui::CalcTextSize(text);
  dl->AddRectFilled(pos, ImVec2(pos.x + ts.x + 16.0f, pos.y + ts.y + 8.0f),
                    IM_COL32(0, 0, 0, 205), 4.0f);
  dl->AddText(ImVec2(pos.x + 8.0f, pos.y + 4.0f), color, text);
}

// Une place pour le témoin, EN DEHORS du rectangle enregistré — il serait sinon
// filmé avec le reste (la capture est prise après notre overlay). On essaie
// au-dessus, en dessous, à gauche, à droite ; sans marge suffisante on renonce à
// l'afficher plutôt que d'abîmer la capture.
bool PlaceBadge(const ImVec2& disp, float zx, float zy, float zw, float zh,
                float bw, float bh, ImVec2* out) {
  const float gap = 6.0f;
  // Au-dessus / en dessous : l'abscisse peut être ramenée dans l'écran sans
  // risque, la bande reste entièrement hors de la zone. Idem pour l'ordonnée
  // quand le témoin va sur un côté.
  const float x = (std::min)((std::max)(0.0f, zx), (std::max)(0.0f, disp.x - bw));
  const float y = (std::min)((std::max)(0.0f, zy), (std::max)(0.0f, disp.y - bh));
  if (zy - bh - gap >= 0.0f)        { *out = ImVec2(x, zy - bh - gap); return true; }
  if (zy + zh + gap + bh <= disp.y) { *out = ImVec2(x, zy + zh + gap); return true; }
  if (zx - bw - gap >= 0.0f)        { *out = ImVec2(zx - bw - gap, y); return true; }
  if (zx + zw + gap + bw <= disp.x) { *out = ImVec2(zx + zw + gap, y); return true; }
  return false;
}

}  // namespace

ZoneRecorder::ZoneRecorder() { g_recorder = this; }

ZoneRecorder::~ZoneRecorder() {
  // Dans cet ordre : le fil de rendu cesse d'abord de nous appeler, ensuite
  // seulement l'objet peut disparaître.
  D3D9_SetPostFrameCallback(nullptr);
  g_recorder = nullptr;
  if (encoder_thread_.joinable()) encoder_thread_.join();
}

// ── Capture, sur le fil de rendu ─────────────────────────────────────────────
void ZoneRecorder::OnPostFrame() {
  if (state_ != State::kRecording) return;

  const unsigned long now = GetTickCount();
  // Comparaison en signé APRÈS soustraction : GetTickCount repasse par zéro tous
  // les 49 jours, et un `now < next` naïf gèlerait l'enregistrement à ce moment-là.
  if (static_cast<long>(now - next_capture_tick_) < 0) return;

  if (frame_count_ < static_cast<int>(frames_.size())) {
    if (D3D9_GrabBackbufferRegion(zone_x_, zone_y_, zone_w_, zone_h_,
                                  capture_w_, capture_h_,
                                  frames_[static_cast<size_t>(frame_count_)].data()))
      ++frame_count_;
  }

  next_capture_tick_ += interval_ms_;
  // Retard accumulé (chargement, gros combat) : on repart de l'instant présent
  // au lieu de rattraper en rafale — ce qui donnerait plusieurs images identiques
  // collées, et une animation qui saute.
  if (static_cast<long>(now - next_capture_tick_) > 0)
    next_capture_tick_ = now + interval_ms_;

  const bool full = frame_count_ >= static_cast<int>(frames_.size());
  const bool elapsed =
      (now - record_start_tick_) >= static_cast<unsigned long>(duration_s_) * 1000u;
  // On ne fait que changer d'état : le thread d'encodage est lancé par PumpState,
  // hors du hook de rendu.
  if (full || elapsed) state_ = State::kEncoding;
}

// ── Machine à états ──────────────────────────────────────────────────────────
void ZoneRecorder::PumpState() {
  if (state_ == State::kCountdown) {
    if (static_cast<long>(GetTickCount() - countdown_end_tick_) >= 0) BeginRecording();
    return;
  }
  // Filet de sécurité : c'est OnPostFrame qui clôt normalement l'enregistrement,
  // or il ne tourne que si Present tourne. Une fenêtre réduite en plein écran (ou
  // un device perdu) le suspend, et l'enregistrement resterait armé indéfiniment,
  // callback compris. Passé la durée demandée plus cinq secondes, on encode ce
  // qu'on a — quitte à ce que ce soit peu.
  if (state_ == State::kRecording) {
    const unsigned long limit = static_cast<unsigned long>(duration_s_) * 1000u + 5000u;
    if (GetTickCount() - record_start_tick_ >= limit) state_ = State::kEncoding;
    return;
  }
  if (state_ != State::kEncoding) return;

  if (!encoder_thread_.joinable()) {  // captures finies, encodage pas encore parti
    FinishRecording();
    return;
  }
  if (!encode_finished_.load()) return;

  encoder_thread_.join();
  if (encode_ok_.load()) {
    last_saved_path_ = encode_path_;
    char msg[64];
    std::snprintf(msg, sizeof(msg), i18n::Tr("GIF écrit (%d images)"), encode_frames_);
    last_status_ = msg;
    LogInfo("ZoneRecorder: {} ({} images, {}x{})", encode_path_, encode_frames_,
            capture_w_, capture_h_);
    // 🔴 La copie se fait ICI, sur le fil principal, et pas au bout du thread
    // d'encodage : OpenClipboard s'attache au THREAD appelant, et le faire depuis
    // un thread qui se termine dans la foulée est un mauvais pari. Le join vient
    // d'avoir lieu, le fichier est donc complet et fermé.
    if (auto_copy_) {
      clip_msg_ = CopyGifToClipboard(last_saved_path_)
                      ? i18n::Tr("GIF copié — Ctrl+V dans Discord pour le poster.")
                      : i18n::Tr("Copie automatique impossible : une autre application "
                                 "retient le presse-papier. Le bouton ci-dessous "
                                 "réessaie.");
      // Le panneau est souvent fermé au moment où l'encodage s'achève : sans ce
      // témoin à l'écran, rien ne dirait que le GIF est prêt À COLLER.
      toast_msg_  = i18n::Tr("GIF copié dans le presse-papier");
      toast_tick_ = GetTickCount();
    }
  } else {
    last_saved_path_.clear();
    last_status_ = i18n::Tr("échec de l'écriture du GIF");
    LogError("ZoneRecorder: écriture impossible : {}", encode_path_);
  }
  last_status_tick_ = GetTickCount();
  frames_.clear();
  frames_.shrink_to_fit();
  frame_count_ = 0;
  state_ = State::kIdle;
}

bool ZoneRecorder::ArmRecording() {
  if (state_ != State::kIdle || !IsStaff()) return false;
  last_status_.clear();
  last_status_tick_ = GetTickCount();
  clip_msg_ = nullptr;
  // Un « GIF copié » de la prise précédente n'a rien à faire par-dessus le
  // décompte de celle qui commence.
  toast_msg_ = nullptr;

  if (g_imgui_dx7_active) {
    last_status_ = i18n::Tr("indisponible en DirectX 7");
    return false;
  }
  if (!IsZoneValid()) {
    last_status_ = i18n::Tr("aucune zone définie");
    return false;
  }

  ComputeOutputSize(&capture_w_, &capture_h_);
  const int wanted = (std::max)(1, fps_ * duration_s_);
  if (EstimatedBytes() > kMaxCaptureBytes) {
    last_status_ = i18n::Tr("trop d'images : réduis la durée ou la largeur");
    return false;
  }

  // 🔴 Tout réserver MAINTENANT. Une allocation ratée en pleine capture ne
  // laisserait qu'un enregistrement tronqué, et pas de bonne façon de le dire.
  frames_.clear();
  frames_.shrink_to_fit();
  try {
    frames_.resize(static_cast<size_t>(wanted));
    for (auto& frame : frames_)
      frame.resize(static_cast<size_t>(capture_w_) * capture_h_);
  } catch (const std::bad_alloc&) {
    frames_.clear();
    frames_.shrink_to_fit();
    last_status_ = i18n::Tr("mémoire insuffisante pour cet enregistrement");
    return false;
  }

  frame_count_ = 0;
  interval_ms_ = static_cast<unsigned>((std::max)(1, 1000 / (std::max)(1, fps_)));
  countdown_end_tick_ =
      GetTickCount() + static_cast<unsigned long>((std::max)(0, start_delay_s_)) * 1000u;
  state_ = State::kCountdown;  // PumpState démarre, même avec un délai nul
  return true;
}

// Le tracé part TOUJOURS d'une ardoise vierge : sans ça, le rectangle de la
// session précédente resterait dessiné sous le voile avant le premier clic.
bool ZoneRecorder::BeginZoneSelection() {
  if (g_imgui_dx7_active) {
    // Même refus que ArmRecording, posé ICI plutôt que chez chaque appelant : sans
    // backbuffer D3D9 à relire, cadrer une zone ne mènerait nulle part.
    last_status_      = i18n::Tr("indisponible en DirectX 7");
    last_status_tick_ = GetTickCount();
    return false;
  }
  select_hint_.clear();
  dragging_ = false;
  drag_ax_ = drag_bx_ = drag_ay_ = drag_by_ = 0.0f;
  state_ = State::kSelecting;
  return true;
}

// Le raccourci de tracé. Il vaut pendant qu'on JOUE, panneau fermé : chaque refus
// se dit donc à l'écran (toast_msg_) et pas seulement dans le panneau.
void ZoneRecorder::ToggleZoneSelection() {
  toast_msg_  = nullptr;
  toast_tick_ = GetTickCount();
  switch (state_) {
    case State::kIdle:
      // Le refus DX7 de BeginZoneSelection ne va que dans last_status_, donc dans
      // le panneau — or on est ici justement parce qu'il est fermé.
      if (!BeginZoneSelection())
        toast_msg_ = i18n::Tr("Enregistrement indisponible en DirectX 7");
      break;
    case State::kSelecting:
      CancelAll();  // 2e appui = on renonce, comme Échap
      break;
    case State::kCountdown:
      // Le décompte sert justement à se placer : si on appuie sur « retracer »
      // à ce moment-là, c'est que le cadrage ne va pas. On abandonne le départ
      // et on rouvre le tracé, plutôt que d'obliger à annuler d'abord.
      CancelAll();
      BeginZoneSelection();
      break;
    case State::kRecording:
      // Les tampons sont dimensionnés pour la zone courante et le GIF veut des
      // images de taille constante : recadrer ici donnerait un fichier incohérent.
      toast_msg_ = i18n::Tr("Recadrage impossible pendant l'enregistrement");
      break;
    case State::kEncoding:
      toast_msg_ = i18n::Tr("Recadrage impossible pendant l'encodage");
      break;
  }
}

void ZoneRecorder::BeginRecording() {
  record_start_tick_ = GetTickCount();
  next_capture_tick_ = record_start_tick_;
  state_ = State::kRecording;
  // Le callback n'est PRIS que le temps de l'enregistrement : le reste du temps,
  // Present ne paie même pas l'appel indirect.
  D3D9_SetPostFrameCallback(&PostFrameThunk);
}

void ZoneRecorder::FinishRecording() {
  D3D9_SetPostFrameCallback(nullptr);
  frames_.resize(static_cast<size_t>((std::max)(0, frame_count_)));

  if (frame_count_ < 2) {
    frames_.clear();
    frames_.shrink_to_fit();
    frame_count_ = 0;
    last_status_ = i18n::Tr("aucune image capturée");
    last_status_tick_ = GetTickCount();
    state_ = State::kIdle;
    return;
  }

  encode_path_   = TimestampedGifPath();
  encode_frames_ = frame_count_;
  // Le GIF compte en centièmes de seconde ; 2 cs est le plancher que respectent
  // les décodeurs (en dessous, la plupart substituent 10 cs et l'animation traîne).
  const int delay_cs = (std::max)(2, static_cast<int>(100.0 / (std::max)(1, fps_) + 0.5));
  encode_finished_.store(false);
  encode_ok_.store(false);

  // Le thread ne touche QUE `frames_`, `capture_*` et `encode_path_`, tous figés
  // ici et relus par personne tant que l'état vaut kEncoding.
  encoder_thread_ = std::thread([this, delay_cs]() {
    std::vector<const uint32_t*> ptrs(frames_.size());
    for (size_t i = 0; i < frames_.size(); ++i) ptrs[i] = frames_[i].data();
    const bool ok = GifWriteScreen(encode_path_.c_str(), ptrs.data(), capture_w_,
                                   capture_h_, static_cast<int>(frames_.size()), delay_cs);
    encode_ok_.store(ok);
    encode_finished_.store(true);  // en DERNIER : c'est ce que le fil principal guette
  });
}

void ZoneRecorder::CancelAll() {
  if (state_ == State::kEncoding) return;  // un GIF en cours d'écriture va au bout
  D3D9_SetPostFrameCallback(nullptr);
  frames_.clear();
  frames_.shrink_to_fit();
  frame_count_ = 0;
  dragging_ = false;
  select_hint_.clear();
  toast_msg_ = nullptr;
  state_ = State::kIdle;
}

// ── Événements ───────────────────────────────────────────────────────────────
void ZoneRecorder::OnTick() { PumpState(); }

void ZoneRecorder::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  // Retour au login / char-select : la zone n'a plus de sens et OnRenderUI ne
  // sera plus appelé pour dessiner quoi que ce soit.
  if (mode_type != ModeMgr::ModeType::kGame) CancelAll();
}

void ZoneRecorder::OnKeyDown(unsigned long vkey, int, int) {
  if (!IsStaff()) return;
  // ProcessPushButton ne transmet que la touche principale : les modificateurs se
  // lisent sur l'état clavier du message en cours (cf. player_jump).
  const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
  const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  auto matches = [&](int bound_vk, bool bound_ctrl, bool bound_alt, bool bound_shift) {
    return bound_vk != 0 && static_cast<int>(vkey) == bound_vk && ctrl == bound_ctrl &&
           alt == bound_alt && shift == bound_shift;
  };
  const bool record = matches(key_vk_, key_ctrl_, key_alt_, key_shift_);
  const bool select = !record && matches(sel_key_vk_, sel_key_ctrl_, sel_key_alt_,
                                         sel_key_shift_);
  if (!record && !select) return;

  if (hotkeys::CaptureInProgress()) return;  // un remappage est en cours
  if (!Bourgeon::Instance().IsGameActive()) return;
  if (Bourgeon::Instance().IsMapLoading()) return;
  if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard) return;
  if (hotkeys::NativeTextInputHasFocus()) return;

  if (select) {
    ToggleZoneSelection();
    return;
  }

  switch (state_) {
    case State::kIdle:
      // Pas encore de zone : la touche ouvre le tracé, comme l'outil de Windows.
      if (IsZoneValid()) ArmRecording();
      else               { select_hint_.clear(); state_ = State::kSelecting; }
      break;
    case State::kCountdown:
      CancelAll();  // 2e appui pendant le décompte = on renonce
      break;
    case State::kRecording:
      state_ = State::kEncoding;  // arrêt anticipé ; PumpState encode ce qu'on a
      break;
    default:
      break;  // kSelecting (la souris décide) / kEncoding (rien à interrompre)
  }
}

void ZoneRecorder::OnRenderUI() {
  PumpState();
  if (!IsStaff()) {
    if (state_ == State::kSelecting) CancelAll();  // droit perdu en cours de session
    return;
  }
  switch (state_) {
    case State::kSelecting: DrawSelectionOverlay(); break;
    case State::kCountdown:
    case State::kRecording:
    case State::kEncoding:  DrawRecordingOverlay(); break;
    // À l'arrêt, seul un message récent a quelque chose à dire (« GIF copié »).
    case State::kIdle:      if (HasToast()) DrawToast(); break;
  }
}

// ── Tracé de la zone ─────────────────────────────────────────────────────────
void ZoneRecorder::DrawSelectionOverlay() {
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 disp = io.DisplaySize;
  if (disp.x <= 0.0f || disp.y <= 0.0f) return;  // fenêtre minimisée

  // Fenêtre plein écran invisible : elle n'existe que pour PRENDRE la souris.
  // Sans elle, le clic de tracé traverserait jusqu'au jeu et enverrait le
  // personnage marcher au milieu de la capture qu'on prépare.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(disp);
  ImGui::SetNextWindowBgAlpha(0.0f);
  ImGui::SetNextWindowFocus();
  ImGui::Begin("##zone_recorder_select", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavFocus |
                   ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::InvisibleButton("##zone_recorder_canvas", disp);
  ImGui::End();
  ImGui::PopStyleVar();

  const ImVec2 mouse = io.MousePos;
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    dragging_ = true;
    select_hint_.clear();
    drag_ax_ = drag_bx_ = mouse.x;
    drag_ay_ = drag_by_ = mouse.y;
  } else if (dragging_) {
    drag_bx_ = mouse.x;
    drag_by_ = mouse.y;
  }

  // Rectangle courant, normalisé et borné à l'écran.
  float x0 = (std::min)(drag_ax_, drag_bx_), x1 = (std::max)(drag_ax_, drag_bx_);
  float y0 = (std::min)(drag_ay_, drag_by_), y1 = (std::max)(drag_ay_, drag_by_);
  x0 = (std::max)(0.0f, x0); y0 = (std::max)(0.0f, y0);
  x1 = (std::min)(disp.x, x1); y1 = (std::min)(disp.y, y1);

  // Draw-list de PREMIER PLAN : le voile doit couvrir aussi les fenêtres ImGui
  // ouvertes (le panneau depuis lequel on vient de cliquer, notamment).
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImU32 veil  = IM_COL32(0, 0, 0, 120);
  const ImU32 frame = IM_COL32(255, 205, 70, 255);

  if (!dragging_ && x1 - x0 < 1.0f) {
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), disp, veil);
  } else {
    // Quatre bandes autour de la sélection : la zone choisie reste en clair.
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(disp.x, y0), veil);
    dl->AddRectFilled(ImVec2(0.0f, y1), ImVec2(disp.x, disp.y), veil);
    dl->AddRectFilled(ImVec2(0.0f, y0), ImVec2(x0, y1), veil);
    dl->AddRectFilled(ImVec2(x1, y0), ImVec2(disp.x, y1), veil);
    dl->AddRect(ImVec2(x0 - 1.0f, y0 - 1.0f), ImVec2(x1 + 1.0f, y1 + 1.0f), frame,
                0.0f, 0, 2.0f);
    char size[48];
    std::snprintf(size, sizeof(size), "%d x %d", static_cast<int>(x1 - x0),
                  static_cast<int>(y1 - y0));
    const ImVec2 ts = ImGui::CalcTextSize(size);
    const float ly = (y0 - ts.y - 4.0f >= 0.0f) ? y0 - ts.y - 4.0f : y1 + 4.0f;
    dl->AddRectFilled(ImVec2(x0, ly), ImVec2(x0 + ts.x + 8.0f, ly + ts.y + 2.0f),
                      IM_COL32(0, 0, 0, 180));
    dl->AddText(ImVec2(x0 + 4.0f, ly + 1.0f), IM_COL32(255, 255, 255, 255), size);
  }

  // Consigne, en haut au centre.
  const char* hint = select_hint_.empty()
                         ? i18n::Tr("Trace la zone à enregistrer  —  Échap pour annuler")
                         : select_hint_.c_str();
  const ImVec2 hs = ImGui::CalcTextSize(hint);
  const ImVec2 hp((disp.x - hs.x) * 0.5f, 24.0f);
  dl->AddRectFilled(ImVec2(hp.x - 10.0f, hp.y - 6.0f),
                    ImVec2(hp.x + hs.x + 10.0f, hp.y + hs.y + 6.0f),
                    IM_COL32(0, 0, 0, 200), 4.0f);
  dl->AddText(hp, select_hint_.empty() ? IM_COL32(255, 255, 255, 255)
                                       : IM_COL32(255, 120, 120, 255), hint);

  if (dragging_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    dragging_ = false;
    const int nw = static_cast<int>(x1 - x0), nh = static_cast<int>(y1 - y0);
    if (nw < kMinZonePx || nh < kMinZonePx) {
      select_hint_ = i18n::Tr("Zone trop petite (32 px minimum) — recommence");
      drag_ax_ = drag_bx_ = drag_ay_ = drag_by_ = 0.0f;
    } else {
      zone_x_ = static_cast<int>(x0);
      zone_y_ = static_cast<int>(y0);
      zone_w_ = nw;
      zone_h_ = nh;
      state_ = State::kIdle;
      select_hint_.clear();
      if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
    }
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) CancelAll();
}

// ── Aperçu de la zone mémorisée (survol dans le panneau) ─────────────────────
// 🔴 Appelé UNIQUEMENT à l'arrêt (state_ == kIdle) : la capture est prise après
// notre overlay, ce cadre serait donc filmé s'il s'affichait pendant un
// enregistrement. Le garde-fou est chez l'appelant, où l'état est connu.
void ZoneRecorder::DrawZonePreview() const {
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  if (disp.x <= 0.0f || disp.y <= 0.0f) return;  // fenêtre minimisée

  const float x0 = static_cast<float>(zone_x_);
  const float y0 = static_cast<float>(zone_y_);
  const float x1 = x0 + static_cast<float>(zone_w_);
  const float y1 = y0 + static_cast<float>(zone_h_);

  // Draw-list de PREMIER PLAN : le panneau « Staff Tools » recouvre souvent la
  // zone, et c'est justement par-dessus lui qu'il faut voir le cadre.
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  // Pas de voile plein écran, contrairement au tracé : un simple survol ne doit
  // pas noircir l'écran. Un remplissage très discret suffit à situer la zone.
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 205, 70, 28));
  dl->AddRect(ImVec2(x0 - 1.0f, y0 - 1.0f), ImVec2(x1 + 1.0f, y1 + 1.0f),
              IM_COL32(255, 205, 70, 255), 0.0f, 0, 2.0f);

  // Mêmes dimensions que pendant le tracé, au même endroit — au-dessus du cadre,
  // ou en dessous s'il touche le haut de l'écran. L'abscisse est ramenée dans
  // l'écran : une zone mémorisée sous une AUTRE résolution peut déborder, et
  // c'est précisément le cas où l'aperçu sert à quelque chose.
  char size[48];
  std::snprintf(size, sizeof(size), "%d x %d", zone_w_, zone_h_);
  const ImVec2 ts = ImGui::CalcTextSize(size);
  const float lw = ts.x + 8.0f;
  const float lx = (std::min)((std::max)(0.0f, x0), (std::max)(0.0f, disp.x - lw));
  const float ly = (y0 - ts.y - 4.0f >= 0.0f) ? y0 - ts.y - 4.0f : y1 + 4.0f;
  dl->AddRectFilled(ImVec2(lx, ly), ImVec2(lx + lw, ly + ts.y + 2.0f),
                    IM_COL32(0, 0, 0, 180));
  dl->AddText(ImVec2(lx + 4.0f, ly + 1.0f), IM_COL32(255, 255, 255, 255), size);
}

// ── Décompte, témoin d'enregistrement ────────────────────────────────────────
void ZoneRecorder::DrawRecordingOverlay() {
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  if (disp.x <= 0.0f || disp.y <= 0.0f) return;

  const float zx = static_cast<float>(zone_x_), zy = static_cast<float>(zone_y_);
  const float zw = static_cast<float>(zone_w_), zh = static_cast<float>(zone_h_);
  ImDrawList* dl = ImGui::GetForegroundDrawList();

  // 🔴 Le cadre borde la zone par l'EXTÉRIEUR (de -3 à -1 px). La capture prend
  // exactement [zone, zone+taille[ : un cadre posé sur le bord se retrouverait
  // dans le GIF, sur toutes les images.
  const ImU32 color = (state_ == State::kRecording) ? IM_COL32(255, 70, 70, 235)
                                                    : IM_COL32(255, 205, 70, 235);
  if (state_ != State::kEncoding)
    dl->AddRect(ImVec2(zx - 2.0f, zy - 2.0f), ImVec2(zx + zw + 2.0f, zy + zh + 2.0f),
                color, 0.0f, 0, 2.0f);

  // Un refus de recadrage prend la place du témoin le temps d'être lu : il arrive
  // panneau fermé, et le compteur d'images qu'il masque revient dans 3 secondes.
  // Le message est pris TEL QUEL et non recopié : une traduction plus longue que
  // le tampon local sortirait tronquée.
  const bool toast = HasToast();
  char buf[64];
  const char* label = toast_msg_;
  if (!toast) {
    if (state_ == State::kCountdown) {
      const long left = static_cast<long>(countdown_end_tick_ - GetTickCount());
      std::snprintf(buf, sizeof(buf), i18n::Tr("Départ dans %ld…"), (left / 1000) + 1);
    } else if (state_ == State::kRecording) {
      const unsigned long ms = GetTickCount() - record_start_tick_;
      // C'est le GABARIT qu'on traduit, avant la composition : « img » est un mot,
      // et une fois le compteur inséré la chaîne n'est plus une clé de catalogue.
      std::snprintf(buf, sizeof(buf), i18n::Tr("● REC  %.1f / %d s  (%d img)"),
                    ms / 1000.0f, duration_s_, frame_count_);
    } else {
      std::snprintf(buf, sizeof(buf), "%s", i18n::Tr("Encodage du GIF…"));
    }
    label = buf;
  }

  const ImVec2 ts = ImGui::CalcTextSize(label);
  const float bw = ts.x + 16.0f, bh = ts.y + 8.0f;
  ImVec2 pos;
  // L'encodage vient APRÈS la dernière capture : rien de ce qu'on dessine à ce
  // moment-là ne peut plus finir dans le GIF, donc on s'autorise le centre.
  if (state_ == State::kEncoding)
    pos = ImVec2((disp.x - bw) * 0.5f, disp.y - bh - 40.0f);
  else if (!PlaceBadge(disp, zx, zy, zw, zh, bw, bh, &pos))
    return;  // aucune marge libre : mieux vaut pas de témoin qu'un témoin filmé

  DrawBadge(dl, pos, label, toast ? IM_COL32(255, 235, 150, 245) : color);
}

// ── Message flottant ─────────────────────────────────────────────────────────
bool ZoneRecorder::HasToast() const {
  return toast_msg_ != nullptr && GetTickCount() - toast_tick_ < kToastMs;
}

// Rien n'est capturé en kIdle : le bas de l'écran est donc sans danger, et c'est
// là que le joueur regarde déjà (chat, barre d'action). Sert surtout à annoncer la
// copie automatique, qui aboutit une fois le panneau refermé.
void ZoneRecorder::DrawToast() const {
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  if (disp.x <= 0.0f || disp.y <= 0.0f) return;
  const ImVec2 ts = ImGui::CalcTextSize(toast_msg_);
  const ImVec2 pos((disp.x - (ts.x + 16.0f)) * 0.5f, disp.y - (ts.y + 8.0f) - 40.0f);
  DrawBadge(ImGui::GetForegroundDrawList(), pos, toast_msg_,
            IM_COL32(255, 235, 150, 245));
}

// ── Dimensions de sortie et budget ───────────────────────────────────────────
void ZoneRecorder::ComputeOutputSize(int* out_w, int* out_h) const {
  int w = zone_w_, h = zone_h_;
  if (max_width_ > 0 && w > max_width_) {
    h = static_cast<int>(static_cast<double>(h) * max_width_ / w + 0.5);
    w = max_width_;
  }
  *out_w = (std::max)(2, w);
  *out_h = (std::max)(2, h);
}

size_t ZoneRecorder::EstimatedBytes() const {
  int w = 0, h = 0;
  ComputeOutputSize(&w, &h);
  const size_t frames = static_cast<size_t>((std::max)(1, fps_ * duration_s_));
  return frames * static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
}

// ── Panneau « Staff Tools » ──────────────────────────────────────────────────
void ZoneRecorder::DrawSettings() {
  bool save = false;

  if (g_imgui_dx7_active) {
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                       i18n::Tr("Indisponible : le client rend en DirectX 7."));
    ImGui::TextDisabled(i18n::Tr("Bascule en DirectX 9 dans Setup.exe pour enregistrer."));
    return;
  }

  // ── Zone ──
  // « 977 x 689 en 3, 301 » ne dit rien de concret : on montre le rectangle
  // lui-même, à sa place à l'écran, dès que la souris passe sur la ligne qui le
  // décrit ou sur le bouton qui le refait.
  bool preview_zone = false;
  if (IsZoneValid()) {
    ImGui::Text(i18n::Tr("Zone : %d x %d  (en %d, %d)"), zone_w_, zone_h_, zone_x_, zone_y_);
    preview_zone = ImGui::IsItemHovered();
  } else {
    ImGui::TextDisabled(i18n::Tr("Aucune zone définie."));
  }
  const bool busy = (state_ != State::kIdle);
  ImGui::BeginDisabled(busy);
  if (ro::RoButton(IsZoneValid() ? i18n::Tr("Redéfinir la zone") : i18n::Tr("Définir la zone")))
    BeginZoneSelection();
  // Grisé pendant un enregistrement, le bouton n'est PAS survolable — et c'est
  // exactement ce qu'on veut ici (cf. DrawZonePreview).
  preview_zone = preview_zone || ImGui::IsItemHovered();
  // 🔴 JAMAIS pendant un enregistrement : la capture est prise APRÈS notre
  // overlay, ce cadre atterrirait dans le GIF. La ligne « Zone : … » ci-dessus
  // n'est pas grisée, elle, donc le garde-fou se met ICI et pas au survol.
  if (preview_zone && !busy && IsZoneValid()) DrawZonePreview();
  SameLine();
  ImGui::BeginDisabled(!IsZoneValid());
  // « Filmer » et pas « Enregistrer » : ce dernier est LA clé de traduction des
  // trois boutons de sauvegarde du projet, et sortait donc « Save » en anglais sur
  // un bouton qui lance une capture.
  if (ro::RoButton(i18n::Tr("Filmer"))) ArmRecording();
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  SameLine();
  HelpMarker(
      i18n::Tr("Trace un rectangle à la souris, comme l'outil de capture de Windows. La "
      "zone est mémorisée : ensuite, la touche ci-dessous suffit. Une seconde "
      "touche rouvre le tracé en jeu, pour recadrer sans rouvrir ce panneau.\n\n"
      "L'image enregistrée est celle que tu vois — monde, interface native ET "
      "fenêtres Bourgeon. Le cadre et le témoin, eux, sont dessinés en dehors de "
      "la zone : ils n'apparaissent jamais dans le GIF.\n\n"
      "Un second appui sur la touche arrête avant la fin."));

  if (state_ == State::kSelecting)
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), i18n::Tr("Tracé en cours à l'écran…"));

  // ── Réglages ──
  SeparatorText(i18n::Tr("Réglages"));
  // Persisté au RELÂCHEMENT et non à chaque frame de glissement : réécrire le
  // yaml soixante fois par seconde pour un curseur qu'on promène est absurde.
  // ⚠ La molette est traitée HORS du slider et ne désactive donc jamais l'item :
  // sans le second terme, un réglage fait à la molette ne serait JAMAIS enregistré
  // (cf. l'avertissement de WheelSliderInt dans ui/ro_widgets.h).
  auto slider = [&](const char* label, int* value, int lo, int hi) {
    ImGui::SetNextItemWidth(160.0f);
    const bool changed = WheelSliderInt(label, value, lo, hi);
    if (ImGui::IsItemDeactivatedAfterEdit() || (changed && !ImGui::IsItemActive()))
      save = true;
  };
  slider(i18n::Tr("Images / s"), &fps_, 5, 20);
  slider(i18n::Tr("Durée (s)"), &duration_s_, 1, 15);
  slider(i18n::Tr("Largeur max"), &max_width_, 160, 1280);
  SameLine();
  HelpMarker(
      i18n::Tr("La zone est réduite à cette largeur (proportions gardées) avant d'entrer "
      "dans le GIF. C'est le réglage qui pèse le plus lourd : la mémoire et la "
      "taille du fichier montent avec le CARRÉ de la largeur."));
  slider(i18n::Tr("Délai avant départ (s)"), &start_delay_s_, 0, 5);
  SameLine();
  HelpMarker(i18n::Tr("Le temps de refermer ce panneau et de te placer avant que ça tourne."));

  // Le trajet complet d'un tutoriel, c'est « je filme, je colle dans Discord » :
  // sans ça il faut rouvrir ce panneau après chaque prise pour cliquer sur un
  // bouton. Éteint par défaut — écraser le presse-papier est une ACTION, et le
  // joueur y a peut-être mis autre chose.
  if (ro::RoCheckbox(i18n::Tr("Copier dans le presse-papier à la fin"), &auto_copy_))
    save = true;
  SameLine();
  HelpMarker(
      i18n::Tr("Dès que le GIF est écrit, il part dans le presse-papier comme fichier : "
      "un Ctrl+V dans Discord le poste, animation comprise.\n\n"
      "Un message le confirme à l'écran, panneau fermé compris. Ce que tu avais "
      "copié auparavant est perdu."));

  int ow = 0, oh = 0;
  ComputeOutputSize(&ow, &oh);
  const size_t bytes = EstimatedBytes();
  const bool over_budget = bytes > kMaxCaptureBytes;
  ImGui::TextColored(
      over_budget ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
      i18n::Tr("%d images en %d x %d  —  %.0f Mo en mémoire"), fps_ * duration_s_, ow, oh,
      bytes / (1024.0 * 1024.0));
  if (over_budget)
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                       i18n::Tr("Au-delà du budget : réduis la durée ou la largeur."));

  // ── Raccourcis ──
  // Deux touches indépendantes, toutes deux facultatives. Une ligne par touche,
  // sous un ImGui::PushID : les boutons « Redéfinir » et « Effacer » portent le
  // même libellé sur les deux lignes, donc sans cet ID ils partageraient le leur
  // et un clic irait à la mauvaise ligne.
  SeparatorText(i18n::Tr("Raccourcis"));
  auto hotkey_row = [&](const char* title, const char* id, int slot, int* vkey_out,
                        bool* ctrl_out, bool* alt_out, bool* shift_out) {
    ImGui::PushID(id);
    ImGui::TextDisabled("%s", title);
    SameLine();
    if (capturing_key_ == slot) {
      // Gèle les autres raccourcis le temps du choix : la touche pressée doit
      // remapper, pas déclencher l'action qu'elle porte encore.
      hotkeys::PingCapture();
      ImGui::Text(i18n::Tr("appuie sur une touche…  (Échap : annuler)"));
      ImGuiIO& io = ImGui::GetIO();
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        capturing_key_ = -1;
        key_conflict_msg_.clear();
      } else if (const int vkey = hotkeys::CaptureMainVk()) {
        const bool ctrl = io.KeyCtrl, alt = io.KeyAlt, shift = io.KeyShift;
        char what[64];
        // `slot` s'exclut lui-même du contrôle, mais PAS l'autre touche du module :
        // les deux se déclencheraient ensemble sur le même combo.
        if (hotkeys::Conflict(vkey, ctrl, alt, shift, hotkeys::Owner::kZoneRecorder,
                              slot, what, sizeof(what))) {
          key_conflict_msg_ =
              std::string(i18n::Tr("Déjà utilisé par ")) + what + i18n::Tr(" — choisis une autre touche");
        } else {
          *vkey_out  = vkey;
          *ctrl_out  = ctrl;
          *alt_out   = alt;
          *shift_out = shift;
          capturing_key_ = -1;
          key_conflict_msg_.clear();
          save = true;
        }
      }
    } else {
      char label[48];
      hotkeys::Label(*vkey_out, *ctrl_out, *alt_out, *shift_out, label, sizeof(label));
      ImGui::Text("%s", label);
      SameLine(0.0f, 6.0f);
      if (ro::RoButton(i18n::Tr("Redéfinir"))) {
        capturing_key_ = slot;
        key_conflict_msg_.clear();
      }
      if (*vkey_out != 0) {
        SameLine(0.0f, 6.0f);
        if (ro::RoButton(i18n::Tr("Effacer"))) {
          *vkey_out = 0;
          *ctrl_out = *alt_out = *shift_out = false;
          save = true;
        }
      }
    }
    ImGui::PopID();
  };

  hotkey_row(i18n::Tr("Filmer :"), "zonerec_key", hotkeys::kZoneRecKeyRecord,
             &key_vk_, &key_ctrl_, &key_alt_, &key_shift_);
  hotkey_row(i18n::Tr("Retracer la zone :"), "zonesel_key", hotkeys::kZoneRecKeySelect,
             &sel_key_vk_, &sel_key_ctrl_, &sel_key_alt_, &sel_key_shift_);
  SameLine();
  HelpMarker(
      i18n::Tr("Rouvre le tracé à la souris sans passer par ce panneau — qui recouvre "
      "justement ce que tu cherches à cadrer.\n\n"
      "Un second appui annule le tracé, comme Échap. Pendant un décompte, la "
      "touche renonce au départ et rouvre le tracé ; pendant un enregistrement "
      "ou un encodage, elle refuse et le dit à l'écran (les images d'un GIF "
      "doivent toutes avoir la même taille)."));
  if (!key_conflict_msg_.empty())
    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", key_conflict_msg_.c_str());

  // ── Dernier résultat ──
  // Le MESSAGE s'efface au bout d'une minute — un « aucune zone définie » n'a
  // rien à faire à l'écran dix minutes plus tard. Le dernier GIF écrit, lui,
  // reste offert tant que la session dure : on revient le copier bien après
  // l'enregistrement, une fois le tutoriel rédigé, et le faire disparaître
  // obligerait à réenregistrer pour retrouver les boutons.
  const bool show_status =
      !last_status_.empty() && GetTickCount() - last_status_tick_ < 60000u;
  if (show_status || !last_saved_path_.empty()) {
    ImGui::Separator();
    if (show_status) ImGui::TextWrapped("%s", last_status_.c_str());
    if (!last_saved_path_.empty()) {
      ImGui::TextDisabled("%s", last_saved_path_.c_str());
      if (ro::RoButton(i18n::Tr("Copier dans le presse-papier"))) {
        clip_msg_ = CopyGifToClipboard(last_saved_path_)
                        ? i18n::Tr("GIF copié — Ctrl+V dans Discord pour le poster.") : i18n::Tr("Copie impossible : une autre application retient le "
                          "presse-papier, réessaie.");
      }
      Tooltip(i18n::Tr("Colle le fichier GIF lui-même, animation comprise."));
      SameLine(0.0f, 6.0f);
      if (ro::RoButton(i18n::Tr("Ouvrir le dossier"))) RevealInExplorer(last_saved_path_);
      if (clip_msg_) ImGui::TextWrapped("%s", clip_msg_);
    }
  }

  if (save) {
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}
