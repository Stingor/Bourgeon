#pragma once

#include <map>
#include <string>
#include <vector>

#include "plugins/plugin.h"

// Recreates the Basic-Info menu icons as ImGui widgets (Approach B). The native
// grid (UIMenuIconWnd) is hidden (its DrawContent vtable slot is swapped to a
// no-op) and each FUNCTIONAL icon (shown in the native grid per the WARP
// visibility table @0x814064) is redrawn in ImGui from the game's own icon
// bitmap, clickable (routes the command to the game's UI dispatcher) and with
// the native tooltip text. Phase 2a: load + draw + click + tooltip + hide grid.
// Phase 2b will add per-icon move/snap/show-hide/persistence (edit mode).
class MenuIconTweaks : public Plugin {
 public:

  // Contenu de sa section dans le panneau Moonlight. Rend true si un réglage
  // a changé — l'appelant décide de sauvegarder, une seule fois.
  bool DrawSettings();
  const char* name() const override { return "Menu Icons"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRenderUI() override;
  void OnTick() override;
  void FlushPending();  // dispatch the queued click (input phase + OnTick fallback)

  // Replace native menu icons with ImGui ones (driven from MoonlightUi).
  bool enabled_   = false;
  bool edit_mode_ = false;  // drag icons to reposition (driven from MoonlightUi)
  // Set after a drag-end or show/hide change; MoonlightUi drains it -> saves YAML.
  bool geometry_dirty_ = false;

  struct Icon {
    const char* name;   // bitmap base name -> \menu_icon\bt_<name>.bmp
    int         cmd_id; // UI command id (routed on click)
    int         msg_id; // tooltip message id
    void*       tex = nullptr;  // IDirect3DTexture9* (lazily loaded)
    int         w = 0, h = 0;
    int         x = 0, y = 0;   // screen position
    bool        hidden = false; // user-hidden via the MoonlightUi list
  };

  // Persisted per-icon position/visibility, keyed by icon name. MoonlightUi loads
  // it before the icons exist; BuildIconList applies it; every edit updates it
  // (the canonical source MoonlightUi writes back to the YAML).
  struct IconSave { int x = -1, y = -1; bool hidden = false; bool valid = false; };
  std::map<std::string, IconSave> saved_;

  // Live icon list (built in-game) — MoonlightUi iterates it for the show/hide UI.
  std::vector<Icon>& icons() { return icons_; }

 private:
  bool in_game_     = false;
  bool icons_built_ = false;
  bool grid_hidden_ = false;
  bool pending_refresh_ = false;  // request a server clif_refresh (drains in OnTick)
  int  pending_cmd_ = 0;   // click queued in OnRenderUI, dispatched from OnTick
  int  dragging_    = -1;  // index of the icon being dragged in edit mode, else -1
  float drag_off_x_ = 0.0f, drag_off_y_ = 0.0f;  // mouse-to-origin offset at grab
  std::vector<Icon> icons_;

  void BuildIconList();         // populate icons_ with the functional icons
  void HideNativeGrid(bool hide);
  void DispatchCommand(int cmd_id);
  // Magnetic snap of value v (extent ext) on one axis to other icons' edges.
  float SnapIcon(float v, float ext, int self, bool y_axis) const;
};
