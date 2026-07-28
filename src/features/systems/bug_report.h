#pragma once

#include <cstdint>
#include <string>

#include "features/plugin.h"

// ── BugReport ─────────────────────────────────────────────────────────
//
// Système de rapport de bug CONTEXTUEL et partagé. N'importe quel plugin peut
// poser un petit bouton « 🐛 Signaler » dans sa fenêtre ImGui via
// BugReport::Button(ctx). Au clic, une fenêtre modale unique s'ouvre, pré-remplie
// avec le contexte capturé (id d'item/skill/NPC + extras) ; le joueur tape un
// message court et l'envoie.
//
// Le paquet CZ_BOURGEON_BUG_REPORT (0x0F13) ne transporte QUE {catégorie, JSON de
// contexte, message}. L'IDENTITÉ (compte/perso), la MAP et la POSITION sont
// ajoutées côté serveur depuis la session — jamais envoyées ni approuvées depuis
// le client. Le serveur insère en base `bug_reports`, relaie sur Discord, puis
// renvoie ZC_BOURGEON_BUG_REPORT_ACK (0x0F14) → toast in-game.
//
// Points d'entrée v1 : description item, description skill, dialogue NPC, et un
// raccourci global (Ctrl+Alt+B) qui ouvre un rapport générique.

class BugReport : public Plugin {
 public:
  // Catégorie de rapport (mirrorée côté serveur : e_bug_report_category).
  enum Category : uint8_t {
    kGeneric = 0,
    kItem    = 1,
    kSkill   = 2,
    kNpc     = 3,
    kQuest   = 4,
  };

  // Contexte capturé au moment du clic. `label` est montré au joueur (lisible),
  // `json` est stocké tel quel en base pour le site (clé/valeur machine).
  struct Context {
    uint8_t     category = kGeneric;
    std::string label;   // ex. « Objet : Pomme (#501) »
    std::string json;    // ex. {"item_id":501,"refine":7}
  };

  BugReport();

  const char* name() const override { return "BugReport"; }

  void OnRenderUI() override;   // dessine la modale + le toast d'accusé
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // --- API partagée (appelée par les autres plugins) ---
  // Dessine un petit bouton 🐛 inline (à appeler DANS une fenêtre ImGui).
  // `imgui_id` désambiguïse le bouton sur la pile d'ID ImGui (unique par site).
  // Au clic : mémorise `ctx` et demande l'ouverture de la modale au frame suivant.
  void Button(const Context& ctx, const char* imgui_id);
  // Ouvre directement la modale avec ce contexte (raccourci / menu).
  void Open(const Context& ctx);

  // --- Constructeurs de contexte (helpers pratiques) ---
  static Context ItemContext(uint32_t item_id, const std::string& name,
                             int refine = -1);
  static Context SkillContext(uint32_t skill_id, const std::string& name,
                              int level = -1);
  static Context NpcContext(uint32_t gid, const std::string& name);
  static Context GenericContext();

  // Opt-out global (persisté par MoonlightUi) : désactive les boutons contextuels
  // « Signaler un bug » ET le raccourci Ctrl+Alt+B.
  bool& enabled() { return enabled_; }

 private:
  void SendReport(const Context& ctx, const std::string& message);
  void RenderModal();
  void RenderAckToast();

  bool        enabled_ = true;         // opt-out (boutons + raccourci)
  bool        want_open_ = false;      // demande d'ouverture différée (pile ID)
  bool        modal_open_ = false;     // la modale est actuellement ouverte
  Context     ctx_;                    // contexte de la modale courante
  char        msg_buf_[512] = {0};     // saisie joueur (UTF-8)
  uint32_t    last_send_tick_ = 0;     // anti-spam client (throttle bouton Envoyer)

  uint8_t     ack_status_ = 0xFF;      // dernier statut ACK (0xFF = aucun)
  uint32_t    ack_tick_ = 0;           // GetTickCount de réception (pour le fade)
};
