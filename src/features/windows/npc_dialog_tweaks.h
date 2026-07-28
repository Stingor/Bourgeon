#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "features/plugin.h"

// ── NpcDialogTweaks ──────────────────────────────────────────────────────────
//
// Ré-implémentation ImGui des INTERACTIONS NPC (dialogue / menu / prompts),
// calquée sur ShopTweaks (project_npc_dialog_re.md, doc docs/npc_dialog_re.md).
//
// Modèle « capture recv → état → overlay → send » :
//  - CAPTURE (OBSERVE, le handler natif tourne toujours) : ZC_SAY_DIALOG 0xB4 /
//    WAIT 0xB5 / CLOSE 0xB6 / MENU_LIST 0xB7 / OPEN_EDITDLG 0x142 / EDITDLGSTR
//    0x1D4 / CLEAR 0x8D6 / SAY2 0x972 / WAIT2 0x973. On ne fait que bâtir le
//    modèle dans OnRecvPacket (jamais de send/opération fenêtre depuis le recv).
//  - RENDU : overlay ImGui (texte riche ^RRGGBB + liens ; menu avec recherche +
//    touches 1-9 ; input nombre/texte). Meilleur que le natif sur typo/UX.
//  - SEND (thread principal) : requêtes brutes CZ_REQ_NEXT_SCRIPT 0xB9 /
//    CZ_CHOOSE_MENU 0xB8 / CZ_INPUT_EDITDLG 0x143 / CZ_INPUT_EDITDLGSTR 0x1D5.
//    FERMETURE via CMode::SendMsg cmd 0x28 (débloque l'état dialogue CLIENT ;
//    CZ_CLOSE_DIALOG seul laisse le perso bloqué — cf. ShopTweaks::CloseNativeShop).
//
// OPT-IN : imgui_enabled_ = false par défaut ; quand ON, on cache les fenêtres
// natives (0x10/0x11/0x38/0x64/0xE2, flag wnd+0x28) et on rend notre overlay.
//
// P2 (non couvert ici) : icônes ^i / émotes ^e inline (texture atlas), gras/
// italique rendus (font atlas), wiring clic liens item/navi/quest, dialogue
// secondaire 0xE2 (SAY2/WAIT2 capturés mais non distingués), quest/monolog
// dialog, honorer ALIGN/SIZE/POS. Validation mots-interdits de l'input texte
// (le serveur re-valide de toute façon).

class NpcDialogTweaks : public Plugin {
 public:
  NpcDialogTweaks();

  const char* name() const override { return "NpcDialogTweaks"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Settings persistants (la SÉRIALISATION reste chez MoonlightUi).
  bool imgui_enabled_ = false;  // OPT-IN : dialogue natif par défaut.
  bool menu_search_ = true;     // barre de recherche au-dessus des longs menus (>8 choix)

  // Contenu de la section « Fenêtre NPC » du panneau Moonlight. Rend true si un
  // réglage a changé — c'est l'appelant qui décide de sauvegarder, une seule fois.
  //
  // Ces widgets vivaient dans panel_interface.cc, à sept cents lignes du code
  // qu'ils pilotent : quelqu'un qui travaille sur NpcDialogTweaks ne les y
  // trouvait pas. Même patron que SkillBarTweaks::DrawSettings et les quatre
  // autres plugins qui délèguent déjà.
  bool DrawSettings();

  bool& show_panel() { return show_panel_; }

  // Appelé par le hook MakeWindow (WindowPosTweaks) à la création d'une fenêtre
  // de dialogue NPC : si imgui_enabled_, cache la fenêtre AVANT le 1er rendu
  // (évite un flash natif que le seul OnTick laisserait passer).
  void HideNativeAtCreation(void* win, int window_id);

  // Appelé par SendPacketHook : vrai si l'overlay ImGui est actif ET l'opcode est
  // un CZ de dialogue NPC (next/menu/nombre/texte/close). On JETTE alors l'envoi,
  // car il vient forcément de la fenêtre NATIVE résiduelle (nos propres CZ passent
  // par SendPacketRef et contournent ce hook). Neutralise « got 1, valid [1..0] ».
  bool ShouldSuppressNativeDialogSend(uint16_t opcode) const;

 private:
  enum InputMode { kInputNone, kInputNumber, kInputString };

  // Segment de texte riche (résultat du parseur d'une ligne).
  struct Run {
    std::string text;
    uint32_t    color;      // IM_COL32 (0 = couleur de texte par défaut)
    bool        bold = false;
    bool        italic = false;
    int         link = 0;    // 0=aucun ; sinon cmd de lien (URL/ITEM/NAVI/QUEST)
    std::string link_arg;    // URL brute, ou id d'item (<INFO>) pour ouvrir la desc
    int         icon_id = 0; // ^i[id] : icône d'item inline (0 = pas une icône)
  };

  void Reset();                       // vide le modèle (fermeture/warp)
  void PushText(const char* s);       // ajoute une ligne de texte NPC
  static void ParseLine(const std::string& raw, std::vector<Run>* out);
  void DrawRichLines();               // rendu word-wrap multi-couleur (ImDrawList)
  void DrawMenu(float group_h);       // liste de choix (hauteur fixe) : recherche + touches 1-9
  void DrawInput();                   // prompt nombre / texte

  // Envois (thread principal uniquement).
  void SendNext();
  void SendMenuChoice(int one_based);
  void SendMenuCancel();
  void SendNumber(int value);
  void SendString(const char* text);
  void CloseDialog();                 // cmd 0x28 (débloque client) + détruit natif
  void OpenItemDescById(uint32_t id); // clic sur un lien <ITEM> -> fenêtre desc 0xc

  bool DialogActiveNative() const;    // lit CGameMode+0x24C (flag dialogue actif)
  void HideNativeWindows();           // cache 0x10/0x11/0x38/0x64/0xE2 chaque tick
  void ShowNativeWindows();           // dé-cache (toggle OFF / sécurité) -> +0x28=1

  // ── Modèle (bâti par OnRecvPacket, lu par OnRenderUI) ──
  std::vector<std::string> lines_;    // texte NPC accumulé (parsé au rendu)
  std::vector<std::string> choices_;  // menu courant (vide = pas de menu)
  bool       has_next_  = false;      // bouton [Next] demandé (WAIT)
  bool       has_close_ = false;      // bouton [Close] demandé (CLOSE)
  bool       start_fresh_ = true;     // prochain SAY = nouvelle conversation (vide le texte)
  InputMode  input_mode_ = kInputNone;
  bool       input_need_focus_ = false;  // focus clavier du champ à sa 1re frame
  uint32_t   gid_ = 0;                 // GID du NPC en cours

  std::unordered_map<uint32_t, std::string> npc_names_;  // GID -> nom (titre)

  // UI transitoire.
  char  num_buf_[16]  = {0};
  char  str_buf_[128] = {0};
  char  menu_filter_[64] = {0};
  int         pending_link_cmd_ = 0;  // type de lien cliqué (0x1D0 item / 0x1B5 url ; 0=aucun)
  std::string pending_link_arg_;      // argument du lien (id d'item ou url), traité au prochain OnTick
  int   menu_hot_ = -1;               // choix focus clavier (index VISIBLE ; -1 = auto-focus #1)
  int   menu_vis_count_ = 0;          // nb d'items visibles (frame préc., pour le wrap flèches)
  unsigned menu_gen_ = 0;             // génération du menu (incr. à chaque ZC_MENU_LIST)
  unsigned menu_answered_gen_ = 0xFFFFFFFFu;  // génération déjà répondue (anti double-envoi)

  bool  open_ = false;                // interaction NPC active ?
  bool  was_open_ = false;
  bool  need_pos_ = false;            // (re)placer la fenêtre à l'ouverture
  bool  show_panel_ = true;
  bool  map_changed_ = false;         // warp reçu (fermer au tick)
  bool  pending_reset_ = false;       // reset modèle au prochain tick (thread sûr)
  bool  prev_imgui_enabled_ = false;  // détection changement du toggle -> close propre
};
