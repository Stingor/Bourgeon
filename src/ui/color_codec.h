#pragma once

// ── Codec couleur ────────────────────────────────────────────────────────────
// DEUX encodages entiers cohabitent dans Bourgeon, et c'est irréductible :
//
//   ARGB  0xAARRGGBB — l'ordre du CLIENT NATIF. C'est ce que les instructions
//                      patchées du jeu attendent (fond de chat), donc ce que le
//                      yaml persiste, en chaîne « %08X ».
//   ImU32 0xAABBGGRR — l'ordre d'ImGui (R en poids faible). C'est ce que
//                      ColorConvertFloat4ToU32 rend et ce que les ImDrawList
//                      consomment. Persisté en ENTIER DÉCIMAL par les clés
//                      statusicon_* et ro_skin_*.
//
// Les deux prennent le même `float[4]` du color picker et rendent un `uint32_t` :
// RIEN dans les types n'avertit que les sorties diffèrent. Copier une ligne de
// sauvegarde d'un bloc à l'autre inverse R et B, silencieusement. C'est déjà
// arrivé — d'où quatre lambdas locales quasi identiques créées pour compenser.
//
// D'où la règle : ces six fonctions sont les SEULES conversions autorisées, et
// chaque nom dit son encodage. Ne jamais réinliner un décalage de bits.
//
// Le picker est toujours du float[4] dans l'ordre {R, G, B, A}, chacun dans 0..1
// — la convention d'ImGui::ColorEdit4.

#include <cstdint>
#include <string>

#include "imgui.h"

namespace ro {

// ── Encodage NATIF : 0xAARRGGBB ──────────────────────────────────────────────
uint32_t ArgbFromPicker(const float picker_rgba[4]);
void     PickerFromArgb(float picker_rgba[4], uint32_t argb);
ImVec4   ImVec4FromArgb(uint32_t argb);

// ── Encodage IMGUI : 0xAABBGGRR ──────────────────────────────────────────────
uint32_t ImU32FromPicker(const float picker_rgba[4]);
void     PickerFromImU32(uint32_t imu32, float picker_rgba[4]);

// Lit exactement 8 chiffres hexadécimaux. NE LANCE JAMAIS, contrairement à
// std::stoul : une clé corrompue dans le yaml rendait `false` impossible à
// distinguer d'une exception, et l'exception remontait jusqu'au catch qui
// enveloppe TOUT LoadSettings — une seule couleur illisible faisait perdre au
// joueur la totalité de sa configuration.
//
// Refuse tout ce qui n'est pas 8 caractères strictement hexadécimaux : pas de
// signe, pas d'espace de tête, pas de préfixe 0x (std::strtoul les accepte tous).
// `out_argb` n'est écrit que si la fonction rend true.
bool ParseHex8(const std::string& text, uint32_t* out_argb);

}  // namespace ro
