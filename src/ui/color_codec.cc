#include "ui/color_codec.h"

#include <cctype>

namespace ro {

uint32_t ArgbFromPicker(const float picker_rgba[4]) {
  const uint32_t r = static_cast<uint32_t>(picker_rgba[0] * 255.0f + 0.5f) & 0xFF;
  const uint32_t g = static_cast<uint32_t>(picker_rgba[1] * 255.0f + 0.5f) & 0xFF;
  const uint32_t b = static_cast<uint32_t>(picker_rgba[2] * 255.0f + 0.5f) & 0xFF;
  const uint32_t a = static_cast<uint32_t>(picker_rgba[3] * 255.0f + 0.5f) & 0xFF;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

void PickerFromArgb(float picker_rgba[4], uint32_t argb) {
  picker_rgba[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;  // R
  picker_rgba[1] = static_cast<float>((argb >>  8) & 0xFF) / 255.0f;  // G
  picker_rgba[2] = static_cast<float>( argb        & 0xFF) / 255.0f;  // B
  picker_rgba[3] = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;  // A
}

ImVec4 ImVec4FromArgb(uint32_t argb) {
  float c[4];
  PickerFromArgb(c, argb);
  return ImVec4(c[0], c[1], c[2], c[3]);
}

// Délègue à ImGui plutôt que de refaire les décalages : c'est ce qui garantit que
// la valeur relue est bit-à-bit celle qu'un ImDrawList aurait produite, et que le
// yaml déjà sur disque des joueurs continue de se relire à l'identique.
uint32_t ImU32FromPicker(const float picker_rgba[4]) {
  return static_cast<uint32_t>(ImGui::ColorConvertFloat4ToU32(
      ImVec4(picker_rgba[0], picker_rgba[1], picker_rgba[2], picker_rgba[3])));
}

void PickerFromImU32(uint32_t imu32, float picker_rgba[4]) {
  const ImVec4 f = ImGui::ColorConvertU32ToFloat4(imu32);
  picker_rgba[0] = f.x;
  picker_rgba[1] = f.y;
  picker_rgba[2] = f.z;
  picker_rgba[3] = f.w;
}

bool ParseHex8(const std::string& text, uint32_t* out_argb) {
  if (text.size() != 8) return false;
  uint32_t value = 0;
  for (const char ch : text) {
    // isxdigit() attend une valeur représentable en unsigned char : un octet
    // >= 0x80 passé tel quel sur un char signé serait un comportement indéfini.
    if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    const uint32_t digit =
        (ch >= '0' && ch <= '9') ? static_cast<uint32_t>(ch - '0')
      : (ch >= 'a' && ch <= 'f') ? static_cast<uint32_t>(ch - 'a' + 10)
                                 : static_cast<uint32_t>(ch - 'A' + 10);
    value = (value << 4) | digit;
  }
  *out_argb = value;
  return true;
}

}  // namespace ro
