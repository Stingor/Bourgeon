#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "plugins/plugin.h"

// ── ItemDescTweaks (SQUELETTE / placeholder, non-intrusif) ──────────────────
//
// But final : enrichir DRASTIQUEMENT la fenêtre de description item/skill avec
// des données techniques récupérées du serveur moonlight (stats réelles, bonus
// script, sources de drop pour les items ; formule de dégâts, coût SP/niveau,
// cast/cooldown réels pour les skills), rendues PAR-DESSUS l'overlay ImGui (la
// fenêtre native se dessine sous ImGui).
//
// Architecture cible (cf. mémoire project_item_skill_desc_window_re) :
//   - la fenêtre native (id 0xc, vtable 0x01032aac) reste la SOURCE de données
//     côté client (nom/desc/cartes/options/niveaux) + le DÉCLENCHEUR ;
//   - le RENDU bascule en panneau ImGui propriétaire ;
//   - une couche requête/cache async récupère le bloc technique du serveur.
//
// ÉTAT ACTUEL = SQUELETTE NON-INTRUSIF :
//   - AUCUN hook installé (le OnMsg 0xc 0x008c18b0 est déjà hooké par MoonlightUi
//     — on ne le re-hooke pas). On lit l'état en POLLING read-only du pointeur
//     live de la fenêtre desc (kDescWndLivePtr), sous garde SEH.
//   - AUCUN paquet envoyé / opcode enregistré (les constantes/handlers serveur
//     sont présents mais désactivés derrière kEnableServerFetch=false).
//   - AUCUN rendu natif neutralisé (on ne fait que SUPERPOSER un panneau
//     placeholder ; la fenêtre native reste intacte).
// => Quand on activera la vraie feature : flip kEnableServerFetch, enregistrer
//    les opcodes, et (option) neutraliser UIItemSkillDescWnd_DrawContent.

class ItemDescTweaks : public Plugin {
 public:
  ItemDescTweaks();

  const char* name() const override { return "ItemDescTweaks"; }

  void OnTick() override;       // capture l'item/skill courant (polling read-only)
  void OnRenderUI() override;   // dessine le panneau placeholder ImGui
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Toggle runtime de l'overlay placeholder (TODO : exposer dans moonlight_ui).
  bool& enabled() { return enabled_; }

 private:
  // État d'une entrée de cache de données techniques (par id).
  enum class FetchState : uint8_t { kNone, kPending, kReady, kFailed };

  struct TechData {
    FetchState     state = FetchState::kNone;
    uint32_t       requested_tick = 0;   // GetTickCount au moment de la requête
    bool           is_skill = false;
    // Placeholder : champs techniques à remplir depuis la réponse serveur.
    std::string    raw;                  // payload brut (debug, pour l'instant)
  };

  // Lance (à terme) une requête serveur pour cet id ; STUB pour l'instant.
  void RequestTechData(uint32_t id, bool is_skill);

  bool      enabled_ = true;             // overlay placeholder visible
  uint32_t  current_id_ = 0;             // id desc courant (0 = aucun)
  bool      current_is_skill_ = false;
  bool      desc_open_ = false;          // une fenêtre desc native est ouverte ?
  int       desc_x_ = 0, desc_y_ = 0;    // position écran de la fenêtre native
  int       desc_w_ = 0, desc_h_ = 0;    // taille de la fenêtre native

  std::unordered_map<uint32_t, TechData> cache_;
};
