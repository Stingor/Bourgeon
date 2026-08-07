#pragma once

#include <cstdint>

#include "features/plugin.h"

// Barre d'action ImGui qui REMPLACE visuellement la barre de raccourcis native
// (UIShortCutWnd, window id 0x24) — façon Bartender/Dominos/ElvUI.
//
// Principe (validé par RE adverse, voir memory project_shortcut_bar_re.md) :
//   - La fenêtre native reste VIVANTE mais CACHÉE (vtable+0x38 -> this+0x28=0).
//     On ne la détruit JAMAIS (le destructeur nulle g_UIWindowMgr+0x1e8 et tue
//     le clavier F1-F9, qui passe par ce cache et non par la visibilité).
//   - On dessine nos propres slots en ImGui en PUR DESSIN (rectangles bordés +
//     couleurs de fond + texte), SANS texture du jeu : color-codé skill/item,
//     id + niveau, overlay de cooldown. (Icônes = polish v2 éventuel.)
//   - SOURCE D'INDEX UNIQUE : on lit chaque slot via this+0xc4[i] (pointeur vers
//     le record 7 octets dans les globals -> données live) ; un clic gauche
//     déclenche l'usage par OnMsg(0,0x29,col,row) — exactement la voie des
//     F-keys -> dessin et activation toujours cohérents. OnMsg(0x17) (rebuild de
//     this+0xc4) est renvoyé seulement au hide et au changement d'onglet.
//   - Vider une case : clic molette dessus, ou la glisser hors des barres et
//     relâcher (le geste de la barre native, dont OnDrop efface le slot quand le
//     dépôt tombe à côté). Clic droit = description.
//
// v1 : une barre configurable (colonnes, taille, espacement, position, nb slots).
// NON câblé dans MoonlightUi (persistance yaml + multi-barres = étape suivante).
class SkillBar : public Plugin {
 public:
  SkillBar();

  const char* name() const override { return "Skill Bar"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnKeyDown(unsigned long vkey, int new_key, int accurate_key) override;
  void OnRenderUI() override;

  // (Plus de HandleNativeDrop : ses deux sources — l'inventaire et le grimoire
  // natifs — appartiennent au même groupe « Interface moderne » que cette barre,
  // donc quand elle est active leurs fenêtres ne naissent plus. Remplir une case
  // passe par le glisser ImGui.)

  // ── Config GLOBALE (publique : câblage MoonlightUi / persistance yaml) ──────
  bool  enabled_       = false;  // remplacement ImGui actif (cache la barre native)
  bool  locked_        = true;   // barres verrouillées (fixes) ; décoché = déplaçables. Slots
                                 // toujours utilisables/réarrangeables, lock ou pas.
  bool  bilinear_      = false;  // filtre texture icônes : false=POINT (net), true=LINEAR (flou ImGui)
  bool  clickthrough_  = false;  // clics traversent les barres (vont au jeu) sauf si Shift maintenu
  bool  show_keys_     = true;   // affiche l'étiquette de touche (F1-F9..) en haut-gauche des slots
  bool  bold_text_     = false;  // faux-gras des textes (touches + nombres) : re-dessin décalé
  float key_scale_     = 1.0f;   // multiplicateur de taille du texte des touches (F1..), 0.5..2.0
  float count_scale_   = 1.0f;   // multiplicateur de taille du texte nombre (count objet / niveau), 0.5..2.0
  bool  dirty_         = false;  // config modifiée -> MoonlightUi draine et persiste (yaml)
  // (Taille/espacement sont PAR BARRE, dans BarCfg. L'aimantation utilise la grille d'alignement
  //  PARTAGÉE de MoonlightUi, pas de réglage propre ici.)

  // ── 3 barres FIXES : l'index == la région native (cf. kRegions dans le .cc) ──
  //   bar 0 = Onglet 1 (skills, g_ShortCutSlots_Tab0, 36 slots)
  //   bar 1 = Onglet 2 (skills, g_ShortCutSlots_Tab1, 36 slots)
  //   bar 2 = Items    (g_ShortCutItemSlotExt, 9 slots)
  // Position/visibilité/grille réglables par barre ; pas d'ajout/suppression (jeu fixe).
  struct BarCfg {
    bool  visible;     // barre affichée
    int   x, y;        // position écran
    int   columns;     // colonnes de la grille (1..12)
    int   first_slot;  // premier slot logique de la région
    int   slot_count;  // nombre de slots affichés
    float icon_size;   // px par icône (PAR barre)
    float spacing;     // px entre icônes (PAR barre)
  };
  static constexpr int kBarCount = 3;
  // Barre d'items : capacité PLUGIN (le natif n'a que 9 slots ; nos slots d'items n'ont pas de
  // raccourci clavier et sont stockés/persistés côté client -> on peut en offrir plus).
  static constexpr int kItemSlotMax = 36;
  BarCfg bars_[kBarCount] = {
      {true,  300, 500, 9, 0, 9, 32.0f, 2.0f},   // Onglet 1
      {false, 300, 540, 9, 0, 9, 32.0f, 2.0f},   // Onglet 2
      {false, 300, 580, 9, 0, 9, 32.0f, 2.0f},   // Items
  };

  // Contenu PERSISTÉ de la barre d'items (nameids ; 0 = vide). Le natif ne persiste PAS les slots
  // d'items (OnDrop appelle juste SetShortCutItemSlot, aucun paquet) -> on les sauve en yaml et on les
  // restaure à l'entrée en jeu. SnapshotItemSlots() rafraîchit ce tableau depuis les globals live.
  uint32_t item_slots_[kItemSlotMax] = {};
  void SnapshotItemSlots();     // lit g_ShortCutItemSlotExt -> item_slots_ (appelé avant la sauvegarde)
  void DrawSettings();   // contenu des réglages (réutilisé dans l'onglet MoonlightUi)

  // Rejoue l'utilisation d'une case d'OBJET — la voie exacte de sa touche. Pour
  // QuickCast, qui répète tant que la touche est maintenue (le client ignorant
  // l'auto-répétition clavier, cf. quick_cast.h). Renvoie false dès qu'il n'y a
  // plus rien à répéter : case vidée, réarrangée, autre objet, ou sac vide — le
  // signal d'arrêt de la boucle.
  bool RepeatItemSlot(int region, int slot, uint32_t nameid);

  // Couleurs (RGBA 0..1) — fond du cadre, fond par type, fond vide, bordures.
  float col_frame_[4]    = {0.050f, 0.050f, 0.070f, 0.600f};  // fond du cadre (derrière les boutons)
  float col_skill_[4]    = {0.157f, 0.510f, 0.275f, 0.863f};  // vert
  float col_item_[4]     = {0.157f, 0.314f, 0.588f, 0.863f};  // bleu
  float col_empty_[4]    = {0.118f, 0.118f, 0.141f, 0.784f};  // vide
  float col_border_[4]   = {0.000f, 0.000f, 0.000f, 0.784f};
  float col_borderhi_[4] = {1.000f, 0.863f, 0.471f, 0.902f};  // survol / édition
  float col_keytext_[4]  = {0.000f, 0.000f, 0.000f, 1.000f};  // texte des touches (F1..) — noir par défaut
  float col_count_[4]    = {0.000f, 0.000f, 0.000f, 1.000f};  // texte nombre (count objet / niveau skill) — noir
  float col_textout_[4]  = {1.000f, 1.000f, 1.000f, 1.000f};  // contour/ombre des textes (blanc, 8 directions)

 private:
  void DrawBar(int bar);     // dessine la barre d'index `bar` (== région native)
  // Retrait d'une case en la glissant HORS des barres — le geste de la barre
  // native, dont OnDrop efface le slot quand le dépôt tombe à côté. Sans lui, un
  // glisser lâché dans le décor se contentait de ne rien faire : pour vider une
  // case il fallait deviner le clic molette, que rien n'annonce.
  void UpdateDragRemoval();
  // Le point est-il sur une barre visible (avec une marge de pardon) ? Sert à la
  // fois à la décision de retrait et à la croix rouge de l'aperçu : les deux
  // répondent ainsi toujours pareil, donc ce que le joueur voit est ce qui arrive.
  bool PointOverAnyBar(float x, float y) const;
  // Oublie le glisser en cours (barres éteintes, sortie du jeu) : son état ne doit
  // pas servir à décider du sort d'un glisser suivant.
  void ForgetDrag() {
    drag_src_region_ = drag_src_slot_ = -1;
    drag_delivered_ = drag_released_ = false;
    drag_over_bars_ = true;
  }
  // Recense les cases réellement DESSINÉES cette frame (barre visible + slot dans
  // la plage affichée). Un raccourci dont la case n'y figure pas est refusé : les
  // slots vivent dans les globals du client, donc masquer une barre ou réduire son
  // nombre de cases ne les désarmait pas, et une barre rangée après avoir été
  // remplie continuait de lancer ses compétences en aveugle.
  void RefreshDrawnSlots();

  bool in_game_        = false;
  bool native_hidden_  = false;  // état courant de la barre native
  bool items_restored_ = false;  // slots d'items restaurés (WriteSlotRecord) pour cette session ?
  float bar_drag_off_x_ = 0.0f;  // décalage curseur -> coin lors du glisser d'une barre (pour le snap)
  float bar_drag_off_y_ = 0.0f;
  int  last_tab_      = -1;      // pour détecter un changement d'onglet (re-hide natif)

  // ── Glisser d'une case en cours (payload "SBSLOT") — suivi pour le retrait ──
  int  drag_src_region_ = -1;    // région/slot d'origine du glisser (-1 = aucun glisser)
  int  drag_src_slot_   = -1;
  bool drag_delivered_  = false; // une case a accepté le payload -> déplacement, pas retrait
  bool drag_released_   = false; // bouton relâché : la décision ci-dessous est figée
  bool drag_over_bars_  = true;  // curseur au-dessus d'une barre au relâchement. Vrai au repos
                                 // ET entre deux glissers : l'état neutre ne retire rien, et la
                                 // croix rouge de l'aperçu n'apparaît pas sur sa première frame.
};
