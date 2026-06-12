#pragma once

#include "plugins/plugin.h"

// Moonlight-Destiny settings panel (C++ port of the former ui_demo.py).
class MoonlightUi : public Plugin {
 public:
  const char* name() const override { return "Moonlight-Destiny UI"; }

  void OnRenderUI() override;

 private:
  bool window_open_ = true;
  int aloot_type_ = 0;
  bool discord_chat_ = false;
  char text_input_[256] = "Hello";
  int list_box_selection_ = -1;
};
