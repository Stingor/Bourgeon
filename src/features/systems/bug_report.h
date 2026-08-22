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
// raccourci global qui ouvre un rapport générique.
//
// 🔴 CE RACCOURCI N'EST PAS CÂBLÉ ICI. Il vit dans le catalogue des actions
// (`hotkeys::tool_bug_report`, features/hotkey_actions.h), qui garde Ctrl+Alt+B
// comme défaut mais le rend visible et remappable dans l'écran des raccourcis —
// ce qu'un `IsKeyPressed` posé dans notre frame ne pouvait pas offrir.

class BugReport : public Plugin {
 public:
  // Catégorie de rapport (mirrorée côté serveur : e_bug_report_category).
  enum Category : uint8_t {
    kGeneric = 0,
    kItem    = 1,
    kSkill   = 2,
    kNpc     = 3,
    kQuest   = 4,
    kStyle   = 5,
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
  // Variante DISCRÈTE : un petit bouton posé dans la BARRE DE TITRE de la fenêtre
  // RO courante, calé entre le titre et la croix de fermeture.
  //
  // 🔴 À appeler EN DERNIER dans la fenêtre (juste avant End), et jamais dans un
  // BeginChild — le placement se calcule depuis GetWindowPos, et le curseur de
  // layout n'est PAS restauré après coup. Le restaurer demanderait un SetCursorPos
  // final, qui arme `DC.IsSetPos` et fait lever à End() « Code uses SetCursorPos()
  // to extend window/parent boundaries » faute d'item derrière lui.
  //
  // 🔴 Préférer cette forme à Button() dans toute fenêtre dont le bas est occupé
  // par des boutons qui vont et viennent : posé en pied de page, le rapport de bug
  // glisse pour occuper la place libérée, atterrit sous le curseur et ouvre son
  // infobulle tout seul. Dans la barre de titre, sa position ne dépend de rien.
  // Pas d'`imgui_id` : la barre de titre n'en contient qu'un.
  void TitleBarButton(const Context& ctx);
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
  // « Signaler un bug » ET l'action clavier (relu par `hotkeys::Invoke`, qui
  // refuse alors d'ouvrir la modale).
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
