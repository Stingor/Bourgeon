#pragma once
// Dimensions des pieces de skin RO (w x h). Les PIXELS sont charges au runtime
// depuis les fichiers du client (GRF/data) via LoadClientBmp -- pas de donnees
// embarquees (les joueurs ont toujours ces textures dans le jeu).
namespace ro { namespace skin {
struct Blob { int w; int h; };
static const Blob kTitlebarLeft = { 12, 17 };
static const Blob kTitlebarMid = { 12, 17 };
static const Blob kTitlebarRight = { 12, 17 };
static const Blob kSysCloseOff = { 11, 11 };
static const Blob kSysCloseOn = { 11, 11 };
static const Blob kSysMiniOff = { 11, 11 };
static const Blob kSysMiniOn = { 11, 11 };
static const Blob kSysBaseOff = { 11, 11 };
static const Blob kSysBaseOn = { 11, 11 };
static const Blob kBtnOutLeft = { 6, 20 };
static const Blob kBtnOutMid = { 6, 20 };
static const Blob kBtnOutRight = { 6, 20 };
static const Blob kBtnOverLeft = { 6, 20 };
static const Blob kBtnOverMid = { 6, 20 };
static const Blob kBtnOverRight = { 6, 20 };
static const Blob kBtnPressLeft = { 6, 20 };
static const Blob kBtnPressMid = { 6, 20 };
static const Blob kBtnPressRight = { 6, 20 };
static const Blob ksBtnOutLeft = { 6, 14 };
static const Blob ksBtnOutMid = { 6, 2 };
static const Blob ksBtnOutRight = { 6, 14 };
static const Blob ksBtnOverLeft = { 6, 14 };
static const Blob ksBtnOverMid = { 6, 2 };
static const Blob ksBtnOverRight = { 6, 14 };
static const Blob ksBtnPressLeft = { 6, 14 };
static const Blob ksBtnPressMid = { 6, 2 };
static const Blob ksBtnPressRight = { 6, 14 };
static const Blob kBtnResize = { 13, 13 };
static const Blob kCheckbox0 = { 10, 10 };
static const Blob kCheckbox1 = { 10, 10 };
static const Blob kScroll0Up = { 13, 13 };
static const Blob kScroll0Down = { 13, 13 };
static const Blob kScroll0Mid = { 13, 13 };
static const Blob kScroll0BarUp = { 13, 4 };
static const Blob kScroll0BarMid = { 13, 4 };
static const Blob kScroll0BarDown = { 13, 4 };
static const Blob kBtnbarLeft = { 21, 21 };
static const Blob kBtnbarMid = { 21, 21 };
static const Blob kBtnbarRight = { 21, 21 };
static const Blob kIconNum = { 9, 14 };
static const Blob kUpbarLeft = { 12, 20 };
static const Blob kUpbarMid = { 5, 20 };
static const Blob kUpbarRight = { 12, 20 };
static const Blob kSysboxLm = { 14, 14 };
static const Blob kSysboxRm = { 14, 14 };
static const Blob kSysboxLd = { 14, 14 };
static const Blob kSysboxMd = { 14, 14 };
static const Blob kSysboxRd = { 14, 14 };
static const Blob kSysboxLu = { 14, 14 };
static const Blob kSysboxMu = { 14, 14 };
static const Blob kSysboxRu = { 14, 14 };
} }  // namespace ro::skin
