#pragma once

#include <cstdint>
#include <vector>

#include "features/plugin.h"

// Client-side tweaks for the "Basic Info" character window (UIBasicInfoWnd) and
// its surrounding HUD.  Renders standalone, freely movable & resizable bars for
// Base EXP, Job EXP, HP, SP, Zeny and Weight as an ImGui overlay (the client
// already feeds mouse input to ImGui, so the windows are draggable/resizable out
// of the box); more Basic-Info customizations will be added here over time.
//
// Zeny has no in-memory max, but the client stores it signed 32-bit, so its bar
// fills relative to the INT32 hard cap (2,147,483,647) and shows the amount
// thousands-grouped. Weight is a real fraction (cur / max).
//
// Each bar is an INDEPENDENT widget with its own position/size/colour/show flag.
// A global "lock" flips the windows to NoMove|NoResize|NoInputs, which freezes
// them AND makes them click-through (locked bars never set io.WantCaptureMouse,
// so clicks fall through to the game).  "Sticky" is a magnetic snap: while you
// drag a bar, its edges snap-align to any other shown bar (pull away to detach).
//
// Values are read live from the session globals the native UIBasicInfoWnd gauges
// use: Base EXP cur/max @0x015fb9d0/0x015fb9d8 and Job EXP cur/max
// @0x015fb9e8/0x015fb9e0 (INT64); HP cur/max @0x015ff908/0x015ff90c and SP
// cur/max @0x015ff910/0x015ff914 (INT32, like the client's own BasicInfo draw);
// Zeny @0x015fba90 (INT32, g_PlayerZeny) and Weight cur/max @0x015fbaa0/0x015fba9c
// (INT32) — all confirmed by RE of UIBasicInfoWnd::DrawContent @0x0095e620.
//
// This plugin owns all the settings state; the settings UI + persistence live
// in MoonlightUi's "EXP Bar" section, which reaches in via Bourgeon::basic_info().
class BasicInfo : public Plugin {
 public:

  // Contenu de sa section dans le panneau Moonlight. Rend true si un réglage
  // a changé — l'appelant décide de sauvegarder, une seule fois.
  bool DrawSettings();
  BasicInfo();  // installs the msg-0x22 hook that hides native Basic Info pre-render

  const char* name() const override { return "Basic Info"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRenderUI() override;
  void OnTick() override;  // enforces the "hide native Basic Info" option
  // Observe ZC 0x0A3B (ZC_EQUIPMENT_EFFECT / hat effects) pour SUIVRE les effets de
  // chapeau (.str) actifs sur le JOUEUR (aid == propre aid) : status=1 ajoute, 0 retire.
  // 🔴 Ne fait que COPIER : `own_hat_effects_` et la table d'ordinaux sont écrites
  // par HandlePacket, sur le fil principal (cf. features/net_inbox.h).
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Effets de chapeau (hat effect ids = enum e_hat_effects serveur) actifs sur le
  // joueur, dérivés de 0x0A3B. Consommés par RenderPlayerAvatar pour superposer les
  // .str des costumes sans viewid sur l'avatar. Vidés au changement de perso/carte.
  const std::vector<uint16_t>& own_hat_effects() const { return own_hat_effects_; }

  // Tooltip d'aperçu (perso portant l'item) au survol de « ViewID : N » dans
  // item_desc_window / cashshop. Réutilise le moteur de capture sprite du portrait.
  // hat_ordinal != 0 : superpose EN PLUS l'effet .str du hat effect (ordinal
  // e_hat_effects) sur la tête — permet de prévisualiser les costumes SANS viewid
  // (view_id==0 + hat_ordinal!=0 = perso de base + effet). No-op si rien à montrer.
  // À appeler entre Begin/End d'une frame UI.
  void RenderItemPreviewTooltip(int view_id, int emplacement, int hat_ordinal = 0);
  // true si l'item (par son emplacement) est prévisualisable (headgear/garment).
  bool CanPreview(int emplacement) const;
  // Résout un item id (client) -> ordinal de hat effect (e_hat_effects) via la table
  // poussée par le serveur au login (ZC 0x0F17, cf. clif_bourgeon_hateffect_map). 0 si
  // l'item n'a pas de hat effect (ou table pas encore reçue). Le client ne mappe PAS
  // item->ordinal nativement. Pour gater/piloter l'aperçu des costumes sans viewid.
  int ItemToHatOrdinal(int item_id);

  // Avatar plein-corps (perso complet : corps + coiffes + garment, apparence
  // live) composité dans la fenêtre ImGui courante, fit aspect DANS [x,y,w,h] et
  // ancré pieds-en-bas (façon WoW). anim = animType (0=Repos,4=Combat…), dir =
  // 0..7 (0=face), animate = joue les images de l'action. Capture live à chaque
  // appel (réutilise le moteur de capture sprite du portrait, buffer dédié).
  // Utilisé par character_sheet. À appeler entre Begin/End d'une frame UI.
  // show_costume : false = rend les sprites d'équipement RÉELS (masque les costumes),
  // true = affiche les costumes (précédence costume sur coiffes/garment).
  void RenderPlayerAvatar(float x, float y, float w, float h, int anim, int dir,
                          bool animate, bool show_costume = true);

  // Exporte le pantin (pose `anim` + direction `dir`) en GIF animé fond transparent
  // vers `filepath` (chemin complet ; le dossier doit exister). Toutes les images de
  // la pose sont capturées et compositées hors-écran. Renvoie true si écrit. À appeler
  // pendant le rendu de l'overlay (device D3D9). Utilisé par character_sheet.
  bool ExportAvatarGif(int anim, int dir, const char* filepath, bool show_costume = true);

  // ── Bars (read/written by MoonlightUi) ────────────────────────────────────
  struct Bar {
    bool  show;
    int   x, y, w, h;
    float fill[4];  // ImGui RGBA picker state
  };

  enum BarId { kBaseExp = 0, kJobExp, kHp, kSp, kZeny, kWeight, kBarCount };

  // Persistence key suffix + UI label, indexed by BarId.
  static constexpr const char* kBarKeys[kBarCount] = {"base", "job",  "hp",
                                                      "sp",   "zeny", "weight"};
  static constexpr const char* kBarLabels[kBarCount] = {"Base", "Job",  "HP",
                                                        "SP",   "Zeny", "Poids"};

  // Global style shared by every bar.
  bool  visible_   = false;  // master toggle for the whole feature (opt-in)
  bool  locked_    = false;  // freeze + click-through (all bars)
  bool  sticky_    = false;  // magnetic edge-snap between bars on drag
  int   text_mode_ = 1;      // 0=none 1=percent 2=values 3=both
  bool  vertical_  = false;  // false = horizontal fill, true = vertical fill
  bool  border_    = true;   // draw the 1px dark outline around each bar
  float rounding_  = 4.0f;   // corner rounding of the drawn bars (0..16)
  float bg_color_[4] = {0.05f, 0.05f, 0.07f, 0.70f};  // shared background + alpha

  // The alignment grid moved to a shared AlignGrid owned by MoonlightUi
  // (Bourgeon::Instance().moonlight_ui()->grid_); bars read it for snapping.

  Bar bars_[kBarCount] = {
    /* Base EXP */ {true, 100,  76, 220, 16, {0.36f, 0.78f, 1.00f, 1.00f}},
    /* Job EXP  */ {true, 100,  94, 220, 16, {1.00f, 0.82f, 0.30f, 1.00f}},
    /* HP       */ {true, 100,  40, 220, 16, {0.85f, 0.27f, 0.27f, 1.00f}},
    /* SP       */ {true, 100,  58, 220, 16, {0.30f, 0.62f, 0.95f, 1.00f}},
    /* Zeny     */ {true, 100, 112, 220, 16, {0.98f, 0.73f, 0.20f, 1.00f}},
    /* Poids    */ {true, 100, 130, 220, 16, {0.65f, 0.55f, 0.80f, 1.00f}},
  };

  // ── Status portrait: independent, movable HUD elements ────────────────────
  // The head sprite, pseudo, class and level are each a STANDALONE draggable +
  // resizable frame with its own position/size, background colour+opacity,
  // corner rounding and text size, so players can build a custom status UI to
  // their taste.
  // (Continuation of the Basic Info HUD; the head sprite capture lands in a
  // follow-up — placeholder for now. Eventual goal: hide the native Basic Info
  // window once this replaces it.) Persisted by MoonlightUi under "portrait_".
  struct PortraitElem {
    bool  show;
    int   x, y, w, h;
    float bg[4];      // background colour + opacity
    float fg[4];      // text colour (ignored for the head element)
    float rounding;   // background corner rounding (0..16)
    float text_scale; // taille du texte, ×  la police UI (ignoré pour la tête)
  };
  enum PortId { kPortHead = 0, kPortName, kPortClass, kPortLevel, kPortCount };
  static constexpr const char* kPortKeys[kPortCount] = {"head", "name", "class",
                                                        "level"};
  static constexpr const char* kPortLabels[kPortCount] = {"Portrait", "Pseudo",
                                                          "Classe", "Niveau"};

  bool portrait_visible_         = false;  // master toggle (opt-in)
  bool portrait_locked_          = false;  // freeze + click-through (all elems)
  bool portrait_hide_basic_info_ = false;  // hide native UIBasicInfoWnd (id 0)
  bool portrait_border_          = false;  // draw a 1px border around each frame
  bool portrait_head_sprite_     = true;   // render the real head sprite (vs box)
  bool portrait_head_only_       = true;   // keep only head-region layers (no body)
  float portrait_head_zoom_      = 1.0f;   // zoom into the head (0.10 .. 2.00)
  float portrait_head_offx_      = 0.0f;   // pan X from frame centre (0 = centred)
  float portrait_head_offy_      = 0.0f;   // pan Y from frame centre (0 = centred)
  int  portrait_anim_            = 4;      // animType (0=idle..4=standby..8=dead)
  int  portrait_dir_             = 0;      // facing direction 0..7 (0 = front)
  bool portrait_animate_         = true;   // play the action frames (vs freeze)
  bool portrait_show_garment_    = true;   // feed the equipped garment/cape (full body)

  // ── Hat effects (.str) sur l'avatar ───────────────────────────────────────
  // Calibrage 100% AUTOMATIQUE (RE : échelle = échelle sprite car R=1/jobScale ; ancre =
  // pieds - 11 px sprite ; cf. RenderPlayerAvatar). AUCUN réglage manuel (pas d'offset/échelle
  // à la main : ils ne transféraient pas d'un effet/pose à l'autre). Seul un on/off subsiste.
  int   hat_diag_concrete_ = 0;   // dernier id d'effet CONCRET rendu (0=aucun)
  int   hat_diag_layers_   = 0;   // nb de couches .str capturées au dernier rendu (0=échec spawn/capture)
  int   hat_diag_active_   = 0;   // nb de hat effects actifs (own_hat_effects_.size())

  PortraitElem ports_[kPortCount] = {
    /* head  */ {true,  60,  60, 100, 100, {0.05f, 0.05f, 0.07f, 0.78f},
                 {1.00f, 1.00f, 1.00f, 1.00f}, 4.0f, 1.0f},
    /* name  */ {true,  60, 164, 110,  20, {0.05f, 0.05f, 0.07f, 0.78f},
                 {1.00f, 0.82f, 0.30f, 1.00f}, 4.0f, 1.0f},
    /* class */ {true,  60, 186, 110,  20, {0.05f, 0.05f, 0.07f, 0.78f},
                 {1.00f, 1.00f, 1.00f, 1.00f}, 4.0f, 1.0f},
    /* level */ {true,  60, 208, 110,  20, {0.05f, 0.05f, 0.07f, 0.78f},
                 {0.36f, 0.78f, 1.00f, 1.00f}, 4.0f, 1.0f},
  };

  // Set by MoonlightUi when a size preset is applied so DrawBar force-applies
  // the new geometry for one frame even while unlocked.
  bool force_apply_ = false;

  // Set after the user finishes a drag/resize; MoonlightUi drains it (saves the
  // YAML once) so we never write the file every frame.
  bool geometry_dirty_ = false;

 private:
  // Fil réseau -> fil principal : on copie les octets, HandlePacket décode en phase
  // d'entrée (cf. features/net_inbox.h).
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  bool in_game_      = false;
  bool drag_pending_ = false;  // geometry changed mid-drag, awaiting mouse-up

  // Hat effects (.str) actifs sur le joueur, suivis via 0x0A3B (cf. OnRecvPacket).
  // Petit vecteur (0-3 en pratique) ; dédup manuel à l'insertion.
  std::vector<uint16_t> own_hat_effects_;

  // Custom drag/resize state (only one bar is ever dragged at a time).
  enum DragEdge { kEdgeL = 1, kEdgeR = 2, kEdgeT = 4, kEdgeB = 8 };
  int   drag_mode_  = 0;     // 0=none, 1=move, 2=resize
  int   drag_edges_ = 0;     // DragEdge bitmask of the edge(s) grabbed (resize)
  float drag_off_x_ = 0.0f;  // mouse-to-anchor offset captured at drag start
  float drag_off_y_ = 0.0f;
  int   drag_group_mask_ = 0;  // CTRL-move: PortId bitmask dragged as one block

  // Draws bar `id` with the given current/max value. Returns true if its stored
  // geometry changed this frame (user drag/resize).
  bool DrawBar(BarId id, long long cur, long long max);

  // Draws all shown portrait elements (each its own movable/resizable frame).
  // Sets geometry_dirty_ on drag-end so MoonlightUi persists the new layout.
  void DrawPortrait();
  // Draws one portrait element `id`; returns true if its geometry changed.
  bool DrawPortraitElem(PortId id);
  // Connected group of SHOWN portrait elements whose frames touch/overlap `seed`
  // (transitive). Returns a PortId bitmask. Used for CTRL block-move.
  int  PortraitTouchGroup(int seed) const;
  bool portrait_drag_pending_ = false;

  // "Hide native Basic Info" state: while on, OnTick pins the native
  // UIBasicInfoWnd (id 0) off-screen and remembers its real position so it can
  // be restored when the option is turned off.
  bool bi_pinned_off_ = false;
  int  bi_saved_x_ = 0, bi_saved_y_ = 0;

  // Nearest magnetic alignment of value `v` (extent `ext`) on one axis to any
  // other shown bar's edges, within the snap threshold; else returns `v`.
  float SnapValue(float v, float ext, int self_id, bool y_axis) const;
};
