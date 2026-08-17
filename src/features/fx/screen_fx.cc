#include "features/fx/screen_fx.h"

#include "ragnarok/uiwnd.h"
#include <windows.h>

#include <cstdint>
#include <cstdio>

#include "imgui.h"

#include "bourgeon.h"
#include "d3d9/d3d9_hook.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/systems/net_ping.h"     // le ping affiché par l'overlay
#include "features/windows/chat_window.h"  // imgui_enabled_ : plafond du FXAA
#include "ui/ro_imgui.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace {

// ── Native UIWindow flicker-free position restore ────────────────────────────
// For both the ESC "Game Options" window (UIEscOptionWnd, vtable 0x010384a0) and
// the "Game Settings" window (CUIGameSettingsUI, vtable 0x01047d7c). The engine
// centres each on open through its SetPos vtable slot (vtable+0x10 = the shared
// UIWindow_SetPos 0x00874af0), BEFORE the first draw. Restoring from OnTick was one
// frame (~100 ms) late, so the window could be seen at the default centre before
// jumping to the saved spot. We swap each class's SetPos slot so the engine's
// initial open-centring is rewritten to the saved position in place (no jump). The
// ctor/OnCreate never move the window, so the centring is the FIRST SetPos on a
// fresh instance — detected by instance-pointer newness. A second swap on the dtor
// slot (vtable[0]) clears that token the instant the window is destroyed (race-free
// re-arm, even on a fast same-address reopen). User drags pass straight through and
// are persisted by OnTick's polling below.
using SetPosFn = void  (__fastcall*)(void*, void*, int, int);
using DtorFn   = void* (__fastcall*)(void*, void*, char);

ScreenFx* g_owner          = nullptr;  // to read the saved positions
void*           g_orig_setpos    = nullptr;  // shared stock SetPos (0x00874af0)
void*           g_esc_positioned = nullptr;  // armed token: ESC window
void*           g_gs_positioned  = nullptr;  // armed token: Game Settings window
void*           g_esc_orig_dtor  = nullptr;  // stock 0x008db420
void*           g_gs_orig_dtor   = nullptr;  // stock 0x009eb410

// Substitutes the saved (sx,sy) for the engine's open-centring on the FIRST SetPos
// of a fresh instance; otherwise forwards (x,y) unchanged.
void SetPosRestore(void*& token, void* self, int sx, int sy,
                   void* edx, int x, int y) {
  SetPosFn orig = reinterpret_cast<SetPosFn>(g_orig_setpos);
  if (self != token) {
    token = self;
    if (sx != INT_MIN && sx >= 0 && sy >= 0) {
      orig(self, nullptr, sx, sy);
      return;
    }
  }
  orig(self, edx, x, y);
}

void __fastcall Hooked_EscSetPos(void* self, void* edx, int x, int y) {
  SetPosRestore(g_esc_positioned, self,
                g_owner ? g_owner->esc_x() : INT_MIN,
                g_owner ? g_owner->esc_y() : INT_MIN, edx, x, y);
}
void __fastcall Hooked_GsSetPos(void* self, void* edx, int x, int y) {
  SetPosRestore(g_gs_positioned, self,
                g_owner ? g_owner->gopt_x() : INT_MIN,
                g_owner ? g_owner->gopt_y() : INT_MIN, edx, x, y);
}

void* __fastcall Hooked_EscDtor(void* self, void* edx, char flag) {
  if (self == g_esc_positioned) g_esc_positioned = nullptr;
  return reinterpret_cast<DtorFn>(g_esc_orig_dtor)(self, edx, flag);
}
void* __fastcall Hooked_GsDtor(void* self, void* edx, char flag) {
  if (self == g_gs_positioned) g_gs_positioned = nullptr;
  return reinterpret_cast<DtorFn>(g_gs_orig_dtor)(self, edx, flag);
}

// Swaps one vtable slot for `hook` iff it still holds `expected` (defensive), via
// VirtualProtect; saves the original into *save_orig.
void PatchVtableSlot(uintptr_t vtable, int off, void* expected, void* hook, void** save_orig) {
  void** slot = reinterpret_cast<void**>(vtable + off);
  if (*slot != expected) return;
  DWORD old;
  if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
    *save_orig = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
  }
}

}  // namespace

ScreenFx::ScreenFx() { g_owner = this; }

void ScreenFx::Apply() {
  D3D9_SetPostFx(fx_);
  D3D9_SetTextureFilter(tex_filter_);
}

// Documented in the header. Null chatbox (very early, or the plugin is absent)
// answers the STOCK ceiling: the safe one is the one that assumes native text.
float ScreenFx::FxaaMaxStrength() const {
  const ChatWindow* chat = Bourgeon::Instance().chat_window();
  return (chat != nullptr && chat->imgui_enabled_) ? 1.0f : 0.5f;
}

namespace {

// Writes a float into a read-only (.rdata) location via VirtualProtect; no-op if
// already equal. Used to scale the engine's wheel-zoom step constants.
void PatchFloatRO(float* addr, float value) {
  if (*addr == value) return;
  DWORD old;
  if (VirtualProtect(addr, sizeof(float), PAGE_EXECUTE_READWRITE, &old)) {
    *addr = value;
    VirtualProtect(addr, sizeof(float), old, &old);
  }
}

}  // namespace

void ScreenFx::DrawSettings() {
  bool apply = false;  // push to the renderer this frame (live preview)
  bool save  = false;  // persist to disk (on release, not every drag frame)
  auto slider = [&](const char* label, float* v, float lo, float hi) {
    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (WheelSliderFloat(label, v, lo, hi)) apply = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
  };

  // ── Overlay FPS ────────────────────────────────────────────────────────────
  if (ro::RoCheckbox(i18n::Tr("Overlay FPS"), &fps_overlay_)) save = true;
  if (fps_overlay_) {
    ImGui::Indent();
    if (ro::RoCheckbox(i18n::Tr("Courbe des temps d'image"), &fps_graph_)) save = true;
    ImGui::SameLine();
    HelpMarker(i18n::Tr("Les 120 dernières images, de 0 à 33 ms. Un pic visible "
                        "est un à-coup — c'est ce qu'un compteur moyenné cache."));

    if (ro::RoCheckbox(i18n::Tr("Ping serveur"), &fps_show_ping_)) save = true;
    ImGui::SameLine();
    HelpMarker(i18n::Tr("Mesuré sur l'échange d'heure que le client entretient "
                        "déjà avec le serveur, toutes les quelques secondes : "
                        "rien n'est envoyé en plus.\n"
                        "« -- » tant qu'aucun échange n'a eu lieu."));

    if (ro::RoCheckbox(i18n::Tr("Ombrage du texte"), &fps_shadow_)) save = true;
    ImGui::SameLine();
    HelpMarker(i18n::Tr("Une ombre d'un pixel sous le texte. C'est elle qui le "
                        "garde lisible si vous rendez le fond transparent."));

    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (WheelSliderFloat(i18n::Tr("Taille du texte"), &fps_scale_, 0.6f, 3.0f, "%.1fx"))
      save = true;

    // ⚠ `ColorEdit4` avec l'alpha : « couleur ET opacité » est UNE décision pour
    // le joueur, et deux widgets l'obligeraient à faire l'aller-retour entre un
    // curseur et une pastille pour juger du résultat.
    ImVec4 bg = ImGui::ColorConvertU32ToFloat4(fps_bg_col_);
    if (RoColorSwatch(i18n::Tr("Fond"), &bg.x, nullptr, /*with_alpha=*/true,
                          /*numeric_inputs=*/false)) {
      fps_bg_col_ = ImGui::ColorConvertFloat4ToU32(bg);
      save = true;
    }
    ImVec4 text = ImGui::ColorConvertU32ToFloat4(fps_text_col_);
    if (RoColorSwatch(i18n::Tr("Texte"), &text.x, nullptr, /*with_alpha=*/true,
                          /*numeric_inputs=*/false)) {
      fps_text_col_ = ImGui::ColorConvertFloat4ToU32(text);
      save = true;
    }
    ImGui::Unindent();
  }
  ImGui::Spacing();

  if (ro::RoCheckbox(i18n::Tr("Post-processing (effets d'écran)"), &fx_.enabled)) {
    apply = true;
    save  = true;
  }
  ImGui::TextDisabled(i18n::Tr("Affecte le rendu du moteur (monde + UI native), pas l'overlay."));

  if (fx_.enabled) {
    // ── Presets ──
    auto preset = [&](const char* name, D3D9PostFx p) {
      if (ro::RoButton(name)) {
        p.enabled = true;
        fx_ = p;
        apply = true;
        save  = true;
      }
    };
    D3D9PostFx neutral;  // all-default
    preset(i18n::Tr("Neutre"), neutral);
    SameLine();
    { D3D9PostFx p; p.brightness=-0.06f; p.contrast=1.08f; p.saturation=0.85f;
      p.temperature=-0.25f; p.vignette=0.35f; preset(i18n::Tr("Nuit"), p); }
    SameLine();
    { D3D9PostFx p; p.contrast=1.15f; p.saturation=1.12f; p.vignette=0.30f;
      p.sharpen=0.30f; p.aberration=0.20f; p.fxaa=true; preset(i18n::Tr("Cinéma"), p); }
    SameLine();
    { D3D9PostFx p; p.contrast=1.10f; p.saturation=1.20f; p.grain=0.35f;
      p.vignette=0.45f; preset(i18n::Tr("Rétro"), p); }
    SameLine();
    { D3D9PostFx p; p.filter=1; preset(i18n::Tr("N&B"), p); }

    SeparatorText(i18n::Tr("Couleur"));
    slider(i18n::Tr("Luminosité"),  &fx_.brightness, -0.5f, 0.5f);
    slider(i18n::Tr("Contraste"),   &fx_.contrast,    0.5f, 2.0f);
    slider(i18n::Tr("Gamma"),       &fx_.gamma,       0.5f, 2.0f);
    slider(i18n::Tr("Saturation"),  &fx_.saturation,  0.0f, 2.0f);
    slider(i18n::Tr("Température"),  &fx_.temperature,-1.0f, 1.0f);
    // 🔴 Libellés NUS : `ro::RoCombo` traduit ses items lui-même, à la lecture.
    // Les envelopper ici les traduirait DEUX fois — l'anglais rendu par le
    // premier `Tr` repartirait dans le second, qui ne le trouverait pas au
    // catalogue et l'inscrirait comme « à traduire ». Rien ne se voit à l'écran,
    // mais le gabarit d'export se remplit de textes déjà traduits.
    const char* filters[] = {"Aucun", "Noir & blanc", "Sépia", "Négatif", "Daltonien"};
    ImGui::SetNextItemWidth(ro::Px(180.0f));
    if (ro::RoCombo(i18n::Tr("Filtre"), &fx_.filter, filters, IM_ARRAYSIZE(filters))) {
      apply = true;
      save  = true;
    }

    SeparatorText(i18n::Tr("Effets"));
    slider(i18n::Tr("Vignette"),      &fx_.vignette,   0.0f, 1.0f);
    slider(i18n::Tr("Grain"),         &fx_.grain,      0.0f, 1.0f);
    slider(i18n::Tr("Aberration"),    &fx_.aberration, 0.0f, 1.0f);
    slider(i18n::Tr("Netteté"),       &fx_.sharpen,    0.0f, 1.0f);
    if (ro::RoCheckbox(i18n::Tr("FXAA (anti-crénelage)"), &fx_.fxaa)) { apply = true; save = true; }
    if (fx_.fxaa) {
      // Ceiling follows the chatbox mode — see FxaaMaxStrength.
      const float fxaa_max = FxaaMaxStrength();
      slider(i18n::Tr("  Force FXAA"), &fx_.fxaa_strength, 0.0f, fxaa_max);
      SameLine();
      HelpMarker(fxaa_max > 0.5f
                     ? i18n::Tr("Le FXAA plein écran adoucit aussi le texte de l'UI.\n"
                                "Chatbox ImGui allumée : le chat est dessiné dans l'overlay,\n"
                                "que le post-traitement ne touche pas — le plafond monte donc\n"
                                "à 1.0. Il reste du texte natif (noms au-dessus des personnages,\n"
                                "fenêtres natives) : au-delà de 0.5, lui aussi s'adoucit.")
                     : i18n::Tr("Le FXAA plein écran adoucit aussi le texte de l'UI.\n"
                                "Plafonné à 0.5 — au-delà le texte devient illisible.\n"
                                "Avec la chatbox ImGui, le plafond monte à 1.0."));
    }

    if (ro::RoButton(i18n::Tr("Réinitialiser"))) {
      fx_ = D3D9PostFx{};
      fx_.enabled = true;
      apply = true;
      save  = true;
    }
  }

  // NB: global texture-filter tweak (tex_filter_ + SetSamplerState hook) is kept
  // in the code but NOT exposed — RO renders sprites ~1:1 so POINT/LINEAR look
  // identical (no visual payoff), same as the camera zoom below.

  // NB: camera zoom-out tweak (zoom_enabled_/factor/speed + OnTick) is kept in the
  // code but intentionally NOT exposed in the UI (disabled by default).

  if (apply) Apply();
  if (save) {
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}

// Le texte de l'overlay, avec son ombre portée.
//
// 🔴 UNE OMBRE, PAS UN CONTOUR. Un contour demande quatre passes et se voit
// comme un halo ; une ombre d'un pixel en bas à droite suffit à décoller le
// texte de n'importe quel fond, et c'est justement ce qui rend l'overlay
// lisible quand le joueur met l'opacité du fond à zéro — ce qu'il fera, puisque
// le réglage existe. L'ombre est noire à l'alpha du texte : elle disparaît donc
// avec lui si l'on rend le texte translucide.
void ScreenFx::FpsText(const char* text) const {
  if (fps_shadow_) {
    const ImVec2 pos = ImGui::GetCursorPos();
    const ImU32 alpha = (fps_text_col_ >> IM_COL32_A_SHIFT) & 0xFF;
    ImGui::SetCursorPos(ImVec2(pos.x + 1.0f, pos.y + 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, alpha));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    // Revenir EXACTEMENT sur la position d'origine : `SameLine` ne suffirait
    // pas, il ne rattrape que l'axe X et laisserait la ligne décalée d'un pixel
    // vers le bas pour tout ce qui suit.
    ImGui::SetCursorPos(pos);
  }
  ImGui::PushStyleColor(ImGuiCol_Text, fps_text_col_);
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
}

void ScreenFx::OnRenderUI() {
  if (!fps_overlay_) return;
  const ImGuiIO& io = ImGui::GetIO();
  fps_hist_[fps_head_] = io.DeltaTime * 1000.0f;  // ms
  fps_head_ = (fps_head_ + 1) % IM_ARRAYSIZE(fps_hist_);

  // Fond posé par le STYLE et non par `SetNextWindowBgAlpha` : celui-ci ne règle
  // que l'alpha, et le joueur choisit ici la couleur entière.
  ImGui::PushStyleColor(ImGuiCol_WindowBg, fps_bg_col_);
  // ⚠ `NoDecoration` ne retire PAS la bordure : il enlève titre, redimensionnement
  // et barre de défilement, mais le liseré vient de `WindowBorderSize`, un style.
  // Sans cette ligne l'overlay porte un cadre que le joueur n'a pas demandé — et
  // qui reste visible même avec le fond rendu transparent.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                           ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("##fps_overlay", nullptr, flags)) {
    // ⚠ `SetWindowFontScale` et pas une police de plus : l'overlay n'affiche que
    // des chiffres, et charger un second atlas pour ça coûterait de la mémoire
    // vidéo à chaque changement de taille. Le grossissement est un peu mou au
    // -delà de ×2, ce qui est acceptable ici et ne l'aurait pas été pour du texte.
    ImGui::SetWindowFontScale(fps_scale_);

    // 🔴 LA LIGNE NE PORTE QUE LES IMAGES PAR SECONDE. Elle affichait aussi
    // « (16,7 ms) » — le temps d'une image, l'inverse du chiffre d'à côté. Deux
    // reproches, tous deux fondés : ça ne dit rien de plus au joueur, et depuis
    // que le ping existe, deux nombres en « ms » se suivent sans que rien ne
    // distingue une durée d'affichage d'une latence réseau.
    //
    // Le temps d'image n'a pas disparu : il est passé SUR LA COURBE, qui est
    // précisément ce qu'elle trace, avec son unité écrite en toutes lettres.
    char line[96];
    std::snprintf(line, sizeof(line), i18n::Tr("%.0f FPS"), io.Framerate);
    FpsText(line);

    if (fps_show_ping_) {
      const int ping_us = NetPing::LastUs();
      if (ping_us >= 0) {
        // Une décimale, parce que la mesure la mérite maintenant : sur un
        // serveur local l'aller-retour tient sous la milliseconde, et l'arrondir
        // à l'entier redonnerait le « 0 ms » que la basse résolution affichait.
        std::snprintf(line, sizeof(line), i18n::Tr("%.1f ms au serveur"),
                      ping_us / 1000.0f);
      } else {
        // 🔴 DIRE L'ABSENCE. Le client ne demande l'heure du serveur que toutes
        // les quelques secondes : avant le premier battement — et hors du jeu —
        // il n'y a rien à afficher. Un « 0 ms » se lirait comme une mesure.
        std::snprintf(line, sizeof(line), "%s", i18n::Tr("-- ms au serveur"));
      }
      FpsText(line);
    }

    if (fps_graph_) {
      // Le temps d'une image, ÉCRIT SUR SA COURBE. `ms/image` et pas `ms` tout
      // court : c'est la seule chose qui empêche de le lire comme un ping.
      std::snprintf(line, sizeof(line), i18n::Tr("%.1f ms/image"),
                    1000.0f / io.Framerate);
      // `PlotLines` dessine son texte avec `ImGuiCol_Text` : sans ce push, la
      // légende ignorerait la couleur choisie pour le reste de l'overlay.
      ImGui::PushStyleColor(ImGuiCol_Text, fps_text_col_);
      ImGui::PlotLines("##ft", fps_hist_, IM_ARRAYSIZE(fps_hist_), fps_head_, line,
                       0.0f, 33.3f, ImVec2(140.0f * fps_scale_, 32.0f * fps_scale_));
      ImGui::PopStyleColor();
    }
    ImGui::SetWindowFontScale(1.0f);
  }
  ImGui::End();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

void ScreenFx::OnTick() {
  // ── FXAA ceiling follows the chatbox mode ────────────────────────────────────
  // Turning the ImGui chatbox back OFF puts the native chat log — the densest,
  // smallest text of the whole frame — back under the post-fx passes. A strength
  // the player was only granted for the ImGui mode has to come back down with it,
  // and be persisted: what the slider can show is then also what is applied, with
  // no hidden value sitting above its own maximum. Skipped while the chatbox
  // plugin is not there yet, so a saved high value survives startup ordering.
  if (Bourgeon::Instance().chat_window() != nullptr) {
    const float fxaa_max = FxaaMaxStrength();
    if (fx_.fxaa_strength > fxaa_max) {
      fx_.fxaa_strength = fxaa_max;
      Apply();
      if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
    }
  }

  // Extended camera zoom-out: keep the engine's max view-distance clamp globals
  // (read every camera update by Camera_ApplyViewDistanceClamp @ 0x00c82340)
  // raised to default * zoom_factor_. 20250716-specific addresses.
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;

  // Install the flicker-free position hooks once: swap the SetPos slot (vtable+0x10
  // = stock UIWindow_SetPos 0x00874af0) and the dtor slot (vtable[0]) of both the
  // ESC "Game Options" (UIEscOptionWnd, vtbl 0x010384a0) and "Game Settings"
  // (CUIGameSettingsUI, vtbl 0x01047d7c) window classes. Runs well before the user
  // can open either window (OnTick fires every ~100 ms).
  static bool hooks_installed = false;
  if (!hooks_installed) {
    hooks_installed = true;
    void* setpos = reinterpret_cast<void*>(0x00874af0);
    PatchVtableSlot(0x010384a0, 0x10, setpos, reinterpret_cast<void*>(&Hooked_EscSetPos), &g_orig_setpos);
    PatchVtableSlot(0x010384a0, 0x00, reinterpret_cast<void*>(0x008db420), reinterpret_cast<void*>(&Hooked_EscDtor), &g_esc_orig_dtor);
    PatchVtableSlot(0x01047d7c, 0x10, setpos, reinterpret_cast<void*>(&Hooked_GsSetPos), &g_orig_setpos);
    PatchVtableSlot(0x01047d7c, 0x00, reinterpret_cast<void*>(0x009eb410), reinterpret_cast<void*>(&Hooked_GsDtor), &g_gs_orig_dtor);
  }

  // 🔴 Ces deux globaux ne nous appartiennent PAS : le moteur les écrit lui
  // aussi, et pas seulement au démarrage.
  //   - OptionInfo_LoadAndApplyAll (0x00d759f0) pose l'outdoor à 480 ou 400
  //     selon le drapeau /zoom (GameSettings_GetFlag(0xf1)) au chargement.
  //   - GameSettingsCmd_ZoomOut_OnOff (0x006918c0) le RÉÉCRIT à chaque bascule
  //     de la commande **/zoom** en jeu — c'est là que vit le patch WARP
  //     `ZoomMax`, qui gonfle ces deux immédiats.
  // Réécrire à chaque tick une base capturée une fois pour toutes annulait donc
  // /zoom (et ZoomMax avec) 100 ms après la frappe, y compris option DÉCOCHÉE,
  // puisque la branche « désactivé » restaurait elle aussi sa base. On n'écrit
  // plus que si l'option est active, et on RE-BASE dès que la valeur lue n'est
  // plus celle qu'on avait posée : c'est le moteur qui vient de parler, sa
  // valeur devient la nouvelle référence à multiplier.
  auto* max_out = reinterpret_cast<float*>(0x012291c0);  // g_cam_zoomMaxOutdoor
  auto* max_in  = reinterpret_cast<float*>(0x012291c4);  // g_cam_zoomMaxIndoor
  if (zoom_enabled_) {
    if (!zoom_applied_ || *max_out != zoom_written_out_) zoom_base_out_ = *max_out;
    if (!zoom_applied_ || *max_in  != zoom_written_in_)  zoom_base_in_  = *max_in;
    zoom_written_out_ = zoom_base_out_ * zoom_factor_;
    zoom_written_in_  = zoom_base_in_  * zoom_factor_;
    *max_out = zoom_written_out_;
    *max_in  = zoom_written_in_;
    zoom_applied_ = true;
  } else if (zoom_applied_) {
    // On vient d'être décoché : rendre la main UNE fois — et seulement sur ce
    // qui porte encore notre valeur, pour ne pas piétiner un /zoom entre-temps.
    if (*max_out == zoom_written_out_) *max_out = zoom_base_out_;
    if (*max_in  == zoom_written_in_)  *max_in  = zoom_base_in_;
    zoom_applied_ = false;
  }

  // Wheel zoom step (responsiveness): scale the .rdata step constants read by the
  // wheel-zoom handler FUN_00c7d4f0 so each notch moves further.
  auto* step1 = reinterpret_cast<float*>(0x01091520);
  auto* step2 = reinterpret_cast<float*>(0x01091528);
  if (!zoom_step_ok_) {
    zoom_step_base1_ = *step1;
    zoom_step_base2_ = *step2;
    zoom_step_ok_ = true;
  }
  // .rdata : personne d'autre que nous n'y écrit — mais n'y toucher que quand la
  // valeur voulue diffère évite deux VirtualProtect toutes les 100 ms pour rien.
  const float want1 = zoom_enabled_ ? zoom_step_base1_ * zoom_speed_ : zoom_step_base1_;
  const float want2 = zoom_enabled_ ? zoom_step_base2_ * zoom_speed_ : zoom_step_base2_;
  if (*step1 != want1) PatchFloatRO(step1, want1);
  if (*step2 != want2) PatchFloatRO(step2, want2);

  // ── Game Settings window (CUIGameSettingsUI, id 0x271e): persist position ─────
  // Hooked_GsSetPos (the SetPos vtable hook above) makes the restore flicker-free
  // WHEN it fires. But this window is created by a generic registry (DAT_0131ef08),
  // NOT UIWindowMgr, and UIWindow_SetPos is a shared base function — so we cannot
  // rule out the registry centring it via a DIRECT (non-vtable) call that bypasses
  // the hook. We therefore KEEP the one-frame OnTick restore-on-open as a safety
  // net: it restores BEFORE the save branch can run, so a bypassed hook can never
  // let the engine's centre overwrite the saved position. When the hook did fire,
  // the window is already in place and this restore is a harmless no-op. (ESC above
  // is save-only because its centring-through-vtable is proven by disassembly.)
  // Found via FindWindow(0x271e); pos = win+0x1c/0x20; SetPos via vtable slot.
  void* gw = uiwnd::FindWindow(0x271e);
  if (gw) {
    int* px = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(gw) + 0x1c);
    int* py = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(gw) + 0x20);
    if (!gopt_was_open_ && gopt_x_ != INT_MIN && gopt_x_ >= 0 && gopt_y_ >= 0) {
      // Just opened with a saved position -> ensure it (no-op if the hook placed it).
      reinterpret_cast<SetPosFn>(*reinterpret_cast<uintptr_t*>(
          *reinterpret_cast<uintptr_t*>(gw) + 0x10))(gw, nullptr, gopt_x_, gopt_y_);
      gopt_x_ = *px; gopt_y_ = *py;  // sync to the applied (possibly clamped) value
    } else if (*px != gopt_x_ || *py != gopt_y_) {
      gopt_x_ = *px; gopt_y_ = *py;
      const unsigned long now = GetTickCount();
      if (now - gopt_last_save_ >= 200) {  // throttle disk writes during a drag
        gopt_last_save_ = now;
        if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
      }
    }
    gopt_was_open_ = true;
  } else {
    if (gopt_was_open_) {  // just closed -> re-arm restore + flush final position
      g_gs_positioned = nullptr;
      if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
    }
    gopt_was_open_ = false;
  }

  // ── ESC "Game Options" menu (UIEscOptionWnd): persist position on move ────────
  // The ESC pop-up (Character Select / game settings / Shortcut Config / Exit /
  // Return). Restore-on-open is handled flicker-free by Hooked_EscSetPos (the
  // SetPos vtable hook installed above), so here we only persist the user's drags
  // (poll win+0x1c/0x20, throttled) and re-arm the restore hook on close. The live
  // pointer is g_UIWindowMgr+0x408 (null while closed), vtable-guarded against any
  // stale slot value.
  void* ew = *reinterpret_cast<void**>(uiwnd::kUIWindowMgrAddr + 0x408);
  if (ew && *reinterpret_cast<uintptr_t*>(ew) == 0x010384a0) {
    const int ex = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ew) + 0x1c);
    const int ey = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ew) + 0x20);
    if (ex != esc_x_ || ey != esc_y_) {
      esc_x_ = ex; esc_y_ = ey;
      const unsigned long now = GetTickCount();
      if (now - esc_last_save_ >= 200) {  // throttle disk writes during a drag
        esc_last_save_ = now;
        if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
      }
    }
    esc_was_open_ = true;
  } else {
    if (esc_was_open_) {  // just closed -> re-arm restore + flush final position
      g_esc_positioned = nullptr;
      if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
    }
    esc_was_open_ = false;
  }
}
