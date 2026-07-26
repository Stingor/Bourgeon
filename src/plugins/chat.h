#pragma once

#include "plugins/plugin.h"

// Retouches client de la fenêtre de chat (client 20250716) : icônes d'objets
// natives sur les liens <ITEML>, horodatage des lignes, et une largeur de chat
// personnalisée (le chat d'origine ne se redimensionne qu'en hauteur).
//
// Le plugin porte les hooks moteur, le mécanisme d'application ET ses réglages.
// MoonlightUi n'en garde que la PERSISTANCE (il possède le fichier yaml) et
// l'endroit où le panneau s'affiche : les quatre réglages ci-dessous vivaient
// chez lui en double, un miroir à retenir synchronisé à la main.
class ChatTweaks : public Plugin {
 public:
  ChatTweaks();

  const char* name() const override { return "Chat"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // ── Réglages (publics : lus/écrits par la table de persistance de MoonlightUi) ──
  bool& custom_width()    { return custom_width_; }
  int&  custom_width_px() { return custom_width_px_; }
  bool& timestamps()      { return timestamps_; }
  bool& item_icons()      { return item_icons_; }

  // Borne la largeur puis pousse les quatre réglages vers le moteur. À appeler
  // après un chargement de configuration, et à chaque changement venu de l'UI.
  void ApplySettings();

  // Section « Chat » du panneau Moonlight (hors couleurs de fond, qui relèvent
  // du patch mémoire de MoonlightUi). Renvoie true si un réglage a changé.
  bool DrawSettings();

 private:
  bool custom_width_    = false;
  int  custom_width_px_ = 800;   // borné 320..1200 par ApplySettings
  bool timestamps_      = false; // préfixe [HH:MM:SS] sur les nouvelles lignes
  bool item_icons_      = true;  // icônes natives sur les liens <ITEML>
};

namespace chat {
// Set/clear a custom width for the main chat window (px clamped 320..1200).
// Applies live if the chat exists; otherwise the WndProc hook enforces it on the
// next in-game relayout. Passer par ChatTweaks::ApplySettings de préférence.
void SetCustomWidth(bool enabled, int px);

// Enable/disable a [HH:MM:SS] timestamp prefix on every new chat line.  Stored
// into the chat's raw history, so it persists across re-wraps/resizes.
void SetTimestamps(bool enabled);

// Enable/disable the native item icons on <ITEML> chat links.
void SetItemIcons(bool enabled);

// Clear the main chat window's history: empties every channel tab's raw-history
// vectors and re-wraps to a blank display.  Safe no-op if the chat isn't live.
void ClearHistory();
}  // namespace chat
