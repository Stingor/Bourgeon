#include "features/patches/window_pos_tweaks.h"  // WindowPos_PersistOnMsg
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "ui/game_texture.h"
#include "features/patches/status_tweaks.h"

#include "ragnarok/globals.h"
#include "ragnarok/msgstring.h"  // msgstr:: (libellés natifs du client)
#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include <climits>
#include <cstdint>
#include <cstdio>

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "utils/log_console.h"
#include "utils/memory_patch.h"  // mem::PatchValue

// ===========================================================================
// UIStatusWnd relayout (20250716 client / Moonlight-Destiny.exe, base 0x400000)
//
// Background bitmap (the game always reads this, skins aside):
//   data\texture\<유저인터페이스>\statuswnd\w_statwin_bg.bmp  = 302 x 115, labels
//   baked into the art.  Code draws only the dynamic numbers.
//
// Bitmap row centers (bitmap-Y of each label band), shared by the left stat
// rows and the right derived-stat rows:
//   row 0..6 = { 12, 27, 43, 59, 76, 91, 106 }
// The bg is blitted at node-Y = *(this+0xd4) (= 17, the title-bar height), so a
// value's node-Y = blitY + rowCenter - 6  (text top vs label center).
//
// Layout produced (see overlay verification):
//   LEFT box [34..84 | 84..98 | 98..123]:
//     base value   left  x=36
//     +bonus       left  x=49 (base<100) / x=54,y+2,sz12 (base>=100)
//     + arrow btn  reposi  x=86 (box arrow-cell)
//     raise-cost   right x=121
//   LEFT row 6: Status Point value right x=121
//   RIGHT (right-aligned at x):
//     row0 Atk        "%d + %d"  x=294
//     row1 Aspd       "%d"       x=208     Flee  "%d + %d"  x=294 (sz12)
//     row2 Def        "%d + %d"  x=208(sz12)  Mdef "%d + %d" x=294 (sz12)
//     row3 Matk       "%d ~ %d"  x=294
//     row4 Hit        "%d"       x=294
//     row5 Critical   "%d"       x=294
//     row6 Guild name (if present) right x=294, sz12, fontFace 0
// ===========================================================================

namespace {

// ---- patch sites -----------------------------------------------------------
constexpr uintptr_t kHeightImm = 0x00a3a48b;  // push 0x8d (h=141) imm in MakeWindow
constexpr uintptr_t kWidthImm  = 0x00a3a490;  // push 0x118 (w=280) imm in MakeWindow
constexpr uintptr_t kDrawSlot  = 0x01032a24;  // UIStatusWnd vtable +0x50 (DrawContent)
constexpr uintptr_t kDrawOrig  = 0x008b66a0;  // original UIStatusWnd::DrawContent
// Position persistence (the engine never saves window id 0xb — see workflow RE).
constexpr uintptr_t kMsgSlot   = 0x01032a68;  // UIStatusWnd vtable +0x94 (message handler slot)
constexpr uintptr_t kMsgOrig   = 0x008cb7c0;  // FUN_008cb7c0 status msg handler (ret 0x18 = SIX stack args!)

constexpr uint32_t  kNewWidth  = 302;
constexpr uint32_t  kNewHeight = 132;         // 17 title bar + 115 bitmap

// ---- engine functions ------------------------------------------------------
constexpr uintptr_t kDrawTitleBar = 0x00898bc0;  // __thiscall(this, char hasClose, char* title, int width)
constexpr uintptr_t kBgNormalPath = 0x010361b4;  // "...\statuswnd\w_statwin_bg.bmp"

// ---- GLOBAL title-bar text offset (shared UIWindow_DrawTitleBar 0x00898bc0) -
// Moves the title text in EVERY window (the title position is hardcoded in this
// shared function).  +dx = right, +dy = down.  0/0 leaves it unchanged.
constexpr int kTitleDx = 0;
constexpr int kTitleDy = -2;
constexpr uintptr_t kTitleWhiteX = 0x00898cd5;  // push 0x13  (white title x)
constexpr uintptr_t kTitleWhiteY = 0x00898cd2;  // lea eax,[edi-0xd] disp8 (white y-off)
constexpr uintptr_t kTitleBlackX = 0x00898cef;  // push 0x12  (black edge x)
constexpr uintptr_t kTitleBlackY = 0x00898cea;  // lea eax,[edi-0xe] disp8 (black y-off)

using DrawOrig_t = void (__fastcall*)(void*, void*);
using TitleBar_t = void (__fastcall*)(void*, void*, char, const char*, int);
using DrawText_t = void (__fastcall*)(void*, void*, int, int, const char*, unsigned,
                                      int, int, unsigned, unsigned char, unsigned char);
using DrawTextR_t = void (__fastcall*)(void*, void*, int, int, const char*, unsigned,
                                       int, int, unsigned, unsigned char);
using Blit_t   = void (__fastcall*)(void*, void*, int, int, void*, int);
using StatusMsg_t = int (__fastcall*)(void*, void*, int, int, int, int, int, int);  // FUN_008cb7c0 — SIX stack args (ret 0x18)

const auto g_status_msg_orig = reinterpret_cast<StatusMsg_t>(kMsgOrig);
// Position enregistrée de la fenêtre de statut (INT_MIN = jamais écrite).
int g_saved_x = INT_MIN, g_saved_y = INT_MIN;
// Drapeau one-shot : une position vient d'être lue du yaml et attend d'être
// réimposée à la fenêtre vivante. Consommé par WindowPos_TrackLive.
bool g_restore_pending = false;
// L'état de suivi au tick (ex-`static` de OnTick, cf. window_pos_tweaks.h).
WindowPosTracker g_tracker;

// ---- session fields ---------------------------------------------------------
// Les stats, les sous-stats de combat et l'objet guilde viennent de
// `ragnarok/globals.h`. Les trois blocs STR..LUK étaient ici ÉNUMÉRÉS adresse par
// adresse ; ils sont contigus, de pas 4, et `rag::` les indexe déjà — dix-huit
// littéraux de moins à retrouver au portage.
//
// Le nom de guilde est une std::string MSVC posée à `rag::kGuildObjAddr` : la
// longueur est à +0x10 et la capacité à +0x14, et c'est la CAPACITÉ qui dit où
// vivent les octets (SSO en place sous 0x10, tas au-delà).
// ⚠ Plus de decalage de capacite ici : `rag::clientstr` porte la regle SSO.
// Celui de la TAILLE reste, parce qu'il sert d'un test metier — « taille 0 » veut
// dire « pas de guilde », ce qui n'est pas une question de disposition.
constexpr uintptr_t kGuildLen = rag::kGuildObjAddr + 0x10;  // _Mysize (0 == no guild)

const int kRowCenter[7] = {12, 28, 44, 60, 76, 92, 106}; // bitmap-Y of each label row, shared left/right

// Mouseover tooltip hit-rects (OnMouseMove FUN_008ba610, vtable+0x70). 16 rects,
// each {x,y} stored as two int32 in .rdata; the handler tests
// (x <= cx <= x+0x50) && (y <= cy <= y+0xc) and shows GetMsg(helpid). The stock
// rects sit at the OLD layout; we relocate each to its stat's new position.
// Rect is 80px wide: left rects cover label+box; right rects are anchored on the
// label (x=130 colA / x=216 colB) so hovering the stat NAME shows its tooltip.
// help-id -> stat resolved from data\msgstringtable.csv (MSI_DESC_*).
struct HoverRect { uintptr_t xa, ya; int nx, ny; };  // ny = blitY(17)+rowCenter-6
const HoverRect kHoverRects[16] = {
    {0x010371e0, 0x010371e4,   6,  23},  // STR  0xc28  left r0
    {0x010371e8, 0x010371ec,   6,  38},  // AGI  0xc29  left r1
    {0x01037240, 0x01037244,   6,  54},  // VIT  0xc2a  left r2
    {0x01037248, 0x0103724c,   6,  70},  // INT  0xc2b  left r3
    {0x010372b0, 0x010372b4,   6,  87},  // DEX  0xc2c  left r4
    {0x010372b8, 0x010372bc,   6, 102},  // LUK  0xc2d  left r5
    {0x010371c0, 0x010371c4, 130,  23},  // ATK  0xc2e  right r0 (full)
    {0x010371c8, 0x010371cc, 130,  70},  // MATK 0xc34  right r3 (full)
    {0x01037220, 0x01037224, 130,  87},  // HIT  0xc30  right r4 (full)
    {0x01037228, 0x0103722c, 130, 102},  // CRI  0xc31  right r5 (full)
    {0x010372a0, 0x010372a4,   6, 117},  // POINT0xc33  left  r6 (Status Point)
    {0x010372a8, 0x010372ac, 130, 117},  // GUILD0xc32  right r6
    {0x010371d0, 0x010371d4, 130,  54},  // DEF  0xc2f  right r2 colA
    {0x010371d8, 0x010371dc, 216,  54},  // MDEF 0xc35  right r2 colB
    {0x01037230, 0x01037234, 216,  38},  // FLEE 0xc36  right r1 colB
    {0x01037238, 0x0103723c, 130,  38},  // ASPD 0xc37  right r1 colA
};
constexpr uintptr_t kRectGuardAddr = 0x010371c0;  // ATK rect x; stock value 108

inline int RD(uintptr_t a) { return *reinterpret_cast<int*>(a); }
inline int RDo(void* base, int off) {
  return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(base) + off);
}

void DTextL(void* w, int x, int y, const char* s, int size, int face = 1) {
  reinterpret_cast<DrawText_t>(uiwnd::kDrawTextAddr)(w, nullptr, x, y, s, 0, face, size, 0, 0, 0);
}
void DTextR(void* w, int x, int y, const char* s, int size, int face = 1) {
  reinterpret_cast<DrawTextR_t>(uiwnd::kDrawTextRightAddr)(w, nullptr, x, y, s, 0, face, size, 0, 0);
}

// Draw all NORMAL-view content at the new 302x115 coordinates.  POD-only
// locals so the caller can wrap us in SEH.
void DrawNormal(void* wnd, int blitY) {
  char buf[40];

  // ---- left column: base / +bonus / raise-cost --------------------------
  for (int i = 0; i < 6; ++i) {
    const int y = blitY + kRowCenter[i] - 6;
    const int base = RD(rag::kStatBaseAddr + i * 4);
    int txtsize = 13;
    if( base > 999) txtsize = 11;
    snprintf(buf, sizeof(buf), "%d", base);
    DTextL(wnd, 37, y, buf, txtsize);
    txtsize = 13;
    const int bonus = RD(rag::kStatBonusAddr + i * 4);
    if (bonus != 0) {
      if( bonus > 999) txtsize = 11;
      snprintf(buf, sizeof(buf), (bonus > 0) ? "+" : "-");
      DTextL(wnd, 55, y, buf, txtsize);
      snprintf(buf, sizeof(buf), "%d", (bonus > 0) ? bonus : bonus * -1);
      DTextL(wnd, 61, y, buf, txtsize);
    }

    snprintf(buf, sizeof(buf), "%d", RD(rag::kStatRaiseCostAddr + i * 4));
    DTextR(wnd, 121, y, buf, 13);
  }

  const int y6 = blitY + kRowCenter[6] - 6;  // row 7 (Status Point / Guild)

  // ---- left row 7: Status Point -----------------------------------------
  snprintf(buf, sizeof(buf), "%d", RD(rag::kOwnStatusPointsAddr));
  DTextR(wnd, 121, y6, buf, 13);

  // ---- right side --------------------------------------------------------
  const int y0 = blitY + kRowCenter[0] - 6;
  snprintf(buf, sizeof(buf), "%d + %d", RD(rag::kOwnAtk1Addr), RD(rag::kOwnAtk2Addr));
  DTextR(wnd, 294, y0, buf, 13);

  const int y1 = blitY + kRowCenter[1] - 6;
  snprintf(buf, sizeof(buf), "%d", rag::AspdFromAmotion(RD(rag::kOwnAttackDelayAddr)));  // displayed ASPD
  DTextR(wnd, 208, y1, buf, 13);
  snprintf(buf, sizeof(buf), "%d", RD(rag::kOwnFleeAddr));
  DTextR(wnd, 294, y1, buf, 13);

  const int y2 = blitY + kRowCenter[2] - 6;
  snprintf(buf, sizeof(buf), "%d + %d", RD(rag::kOwnDefSoftAddr), RD(rag::kOwnDefHardAddr));
  DTextR(wnd, 208, y2, buf, 13);
  snprintf(buf, sizeof(buf), "%d + %d", RD(rag::kOwnMdefSoftAddr), RD(rag::kOwnMdefHardAddr));
  DTextR(wnd, 294, y2, buf, 13);

  const int y3 = blitY + kRowCenter[3] - 6;
  snprintf(buf, sizeof(buf), "%d ~ %d", RD(rag::kOwnMatkMinAddr), RD(rag::kOwnMatkMaxAddr));
  DTextR(wnd, 294, y3, buf, 13);

  const int y4 = blitY + kRowCenter[4] - 6;
  snprintf(buf, sizeof(buf), "%d", RD(rag::kOwnHitAddr));
  DTextR(wnd, 208, y4, buf, 13);  // col A (aligns under Aspd/Def)

  const int y5 = blitY + kRowCenter[5] - 6;
  snprintf(buf, sizeof(buf), "%d", RD(rag::kOwnCritAddr));
  DTextR(wnd, 208, y5, buf, 13);  // col A (aligns under Aspd/Def)
  snprintf(buf, sizeof(buf), "%d", RD(rag::kOwnPerfectDodgeAddr));
  DTextR(wnd, 294, y5, buf, 13);

  // ---- right row 7: guild name (if any) ---------------------------------
  if (RD(kGuildLen) != 0) {
    const char* gname =
        rag::clientstr::Data(reinterpret_cast<const void*>(rag::kGuildObjAddr));
    DTextL(wnd, 163, y6 - 1, gname, 12, 0);  // left-aligned right after the Guild label
  }

  // ---- reposition the 6 stat-up arrow buttons into the box arrow-cell -----
  // Replicate the native show/hide rule (FUN_008cb7c0 case 0x23, RE-verified): show
  // the arrow only when the stat can still be raised (raise-cost != 0 — the server
  // sends 0 at max level) AND the player has enough points (rag::kOwnStatusPointsAddr >= cost);
  // otherwise hide it off-screen exactly like the native (-100,-100). We rewrite the
  // button x/y every frame, so without this our relayout re-showed arrows the native
  // had hidden. Mirror the native arithmetic literally — no extra max-level logic.
  const int points = RD(rag::kOwnStatusPointsAddr);
  for (int i = 0; i < 6; ++i) {
    void* btn = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(wnd) + 0xb4 + i * 4);
    if (!btn) continue;
    const int cost = RD(rag::kStatRaiseCostAddr + i * 4);
    const bool show = (cost != 0) && (points >= cost);
    // ⚠ ÉCRITURE directe des champs, pas `uiwnd::SetPos` : le natif fait de
    // même ici, et passer par la vtable déclencherait sa logique de
    // repositionnement. Les offsets, eux, sont ceux du foyer.
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(btn) + uiwnd::kOffPosX) =
        show ? 88 : -100;
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(btn) + uiwnd::kOffPosY) =
        show ? (blitY + kRowCenter[i] - 5) : -100;
  }
}

// Replacement for UIStatusWnd::DrawContent (__thiscall -> __fastcall, edx unused).
void __fastcall DrawContentHook(void* wnd, void* /*edx*/) {
  __try {
    // 4th-job expanded trait view: leave it to the original for now.
    if (*(reinterpret_cast<char*>(wnd) + 0xfc) == 1) {
      reinterpret_cast<DrawOrig_t>(kDrawOrig)(wnd, nullptr);
      return;
    }

    // Cp949 et non Utf8 : ce titre repart au dessinateur de texte NATIF, qui
    // attend justement du CP949.
    const char* title = msgstr::Cp949(0x69);
    reinterpret_cast<TitleBar_t>(kDrawTitleBar)(wnd, nullptr, 1, title, 0);

    const int blitY = RDo(wnd, 0xd4);
    if (RDo(wnd, 0x18) == blitY || RDo(wnd, 0x30) != 0) return;  // minimized/guard

    void* tex = ro::texmgr::LoadResource(
        reinterpret_cast<const char*>(kBgNormalPath));
    if (tex) reinterpret_cast<Blit_t>(uiwnd::kBlitImageToNodeAddr)(wnd, nullptr, 0, blitY, tex, 1);

    DrawNormal(wnd, blitY);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Replacement for the STATUS message handler FUN_008cb7c0 (vtable +0x94). The engine
// never persists window id 0xb's position, so we do: snapshot live x/y on close (msg 6
// sub 0xc9) + trigger a settings save, and on layout-restore (msg 0x22) override SetPos
// with the saved x/y AFTER the native restore (we override, so the native validator/
// default are irrelevant). SIX stack args / ret 0x18 — a 5-arg hook corrupts the stack.
// Noms des paramètres alignés sur uiwnd::OnMsg : `msg` est le DEUXIÈME des six
// entiers natifs, `p2` le suivant (ici la sous-commande du msg 6), et `arg0` le
// premier, dont le rôle n'est pas établi (0 partout).
int __fastcall StatusMsgHook(void* self, void* edx, int arg0, int msg, int p2,
                             int p3, int p4, int p5) {
  return WindowPos_PersistOnMsg(self, edx, arg0, msg, p2, p3, p4, p5,
                                g_status_msg_orig, &g_saved_x, &g_saved_y);
}

}  // namespace

StatusTweaks::StatusTweaks() {
  // 1) Resize the window at creation: 280x141 -> 302x132 to match the bitmap.
  const uint32_t cur_h = *reinterpret_cast<uint32_t*>(kHeightImm);
  const uint32_t cur_w = *reinterpret_cast<uint32_t*>(kWidthImm);
  if (cur_h == 141 && cur_w == 280) {
    mem::PatchValue<uint32_t>(kHeightImm, kNewHeight);
    mem::PatchValue<uint32_t>(kWidthImm, kNewWidth);
    // LogInfo("[Status] window size patched to {}x{}", kNewWidth, kNewHeight);
  } else {
    LogError("[Status] SetSize immediates unexpected (w={} h={}); size patch skipped",
             cur_w, cur_h);
  }

  // 2) Swap the DrawContent vtable slot to our relayout (slot is in .data, RW).
  const uintptr_t cur_slot = *reinterpret_cast<uintptr_t*>(kDrawSlot);
  if (cur_slot == kDrawOrig) {
    mem::PatchValue<void*>(kDrawSlot, reinterpret_cast<void*>(&DrawContentHook));
    // LogInfo("[Status] DrawContent vtable hook installed");
  } else {
    LogError("[Status] vtable slot 0x01032a24 = 0x{:x}, expected 0x008b66a0; hook skipped",
             cur_slot);
  }

  // 3) Relocate the 16 mouseover tooltip hit-rects to the new layout.
  const int guard = *reinterpret_cast<int*>(kRectGuardAddr);
  if (guard == 108 || guard == 130) {  // stock (108) or already-patched (130)
    for (const HoverRect& r : kHoverRects) {
      mem::PatchValue<int32_t>(r.xa, r.nx);
      mem::PatchValue<int32_t>(r.ya, r.ny);
    }
    // LogInfo("[Status] 16 tooltip hit-rects relocated to new layout");
  } else {
    LogError("[Status] tooltip rect guard 0x010371c0 = {}, expected 108; rect patch skipped",
             guard);
  }

  // 4) GLOBAL title-bar text offset — moves EVERY window's title text (shared
  //    DrawTitleBar). y-disp8 = -(base - dy): base 13/14, +dy moves down.
  if (*reinterpret_cast<uint8_t*>(kTitleWhiteX) == 0x13) {
    mem::PatchValue<uint8_t>(kTitleWhiteX, static_cast<uint8_t>(19 + kTitleDx));
    mem::PatchValue<uint8_t>(kTitleBlackX, static_cast<uint8_t>(18 + kTitleDx));
    mem::PatchValue<uint8_t>(kTitleWhiteY, static_cast<uint8_t>(kTitleDy - 13));
    mem::PatchValue<uint8_t>(kTitleBlackY, static_cast<uint8_t>(kTitleDy - 14));
    // LogInfo("[Status] title-bar text offset patched dx={} dy={} (all windows)",
            // kTitleDx, kTitleDy);
  } else {
    LogError("[Status] DrawTitleBar title-x = 0x{:x}, expected 0x13; title offset skipped",
             *reinterpret_cast<uint8_t*>(kTitleWhiteX));
  }

  // 5) Hook the message handler (vtable +0x94) to persist the window position the
  //    engine never saves for id 0xb: snapshot x/y on close, re-apply on open.
  const uintptr_t cur_msg = *reinterpret_cast<uintptr_t*>(kMsgSlot);
  if (cur_msg == kMsgOrig) {
    mem::PatchValue<void*>(kMsgSlot, reinterpret_cast<void*>(&StatusMsgHook));
    // LogInfo("[Status] message-handler hook installed (position persistence)");
  } else {
    LogError("[Status] msg vtable slot 0x{:x} = 0x{:x}, expected 0x{:x}; pos-persist skipped",
             kMsgSlot, cur_msg, kMsgOrig);
  }
}

// Accès à la position enregistrée pour le yaml de MoonlightUi (les deux entiers
// vivent dans le namespace anonyme ci-dessus ; ces trois fonctions font le pont
// vers la couche de persistance). Clés : « status_pos_x » / « status_pos_y ».
int  StatusTweaks_SavedX() { return g_saved_x; }
int  StatusTweaks_SavedY() { return g_saved_y; }
void StatusTweaks_SetSavedPos(int x, int y) {
  g_saved_x = x;
  g_saved_y = y;
  // Une position À L'ÉCRAN qu'on vient de lire doit être réimposée à la fenêtre
  // vivante au tick suivant : le client rouvre ses fenêtres restées ouvertes à
  // leur emplacement natif en dur, et l'ordre entre cette réouverture et notre
  // écrasement sur msg 0x22 n'est pas garanti d'un redémarrage à l'autre. C'est
  // ce forçage, depuis OnTick, qui survit au redémarrage. Les coordonnées
  // négatives sont refusées ici : elles viennent d'un yaml jamais écrit, pas
  // d'un joueur.
  g_restore_pending = (x != INT_MIN && x >= 0 && y >= 0);
}

// Persiste la position de la fenêtre quand elle change. On lit la position VIVANTE
// directement dans la fenêtre à chaque tick (FindWindow) : la fenêtre de statut ne
// se redessine PAS à chaque frame (seulement sur changement de stat ou rafraîchi
// périodique), et une capture depuis DrawContent retardait jusqu'à ~1 s. Couvre la
// fin de glisser et TOUS les chemins de fermeture (croix, Échap, bascule Alt+A,
// sortie du jeu).
//
// 🔴 Le corps est chez WindowPos_TrackLive : il était identique, mot pour mot, à
// celui d'EquipTweaks::OnTick. Ne pas le recopier ici pour « simplifier ».
void StatusTweaks::OnTick() {
  WindowPos_TrackLive(uiwnd::kUIStatusWnd, &g_tracker, &g_saved_x, &g_saved_y,
                      &g_restore_pending);
}
