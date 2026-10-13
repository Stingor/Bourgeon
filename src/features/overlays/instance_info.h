#pragma once

#include <cstdint>
#include <string>

#include "features/plugin.h"
#include "features/systems/bourgeon_opcodes.h"

// ── LE DÉTAIL D'UNE INSTANCE (« MEMORIAL DUNGEON ») ─────────────────────────
//
// La fenêtre native (UIMemorialDunWnd, Alt+B) ne dit que le nom, la place en
// file et deux échéances. Le serveur Moonlight sait davantage, et l'envoie dans
// ZC_MOONLIGHT_INSTANCE_INFO (0x0F40) à qui annonce le bit kInstanceInfo de
// CZ_BOURGEON_UI_CAPS : la difficulté (HM) et le mode turbo choisis à la
// création, le propriétaire, les joueurs présents, les durées totales, et si
// CE joueur peut détruire l'instance.
//
// Ce panneau ImGui montre ce détail à côté de la fenêtre native, qui garde son
// bouton de destruction. Il s'ouvre de lui-même quand une instance paraît ou
// s'ouvre, pas à chaque rappel du serveur (il redit l'instance à chaque carte) ;
// la croix le ferme jusqu'à la prochaine instance.
//
// Les paquets standard 0x02CB / 0x02CC / 0x02CD / 0x02CE sont OBSERVÉS, jamais
// remplacés : le natif les traite comme avant.
class InstanceInfo : public Plugin {
 public:
  InstanceInfo();

  const char* name() const override { return "Instance info"; }

  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void OnRenderUI() override;

 private:
  enum class Phase : uint8_t { kNone, kWaiting, kOpen };

  void Arrive(const std::string& name, Phase phase);
  void Reset();

  static constexpr uint16_t kOpInstanceInfo = bopcodes::kInstanceInfo;

  Phase phase_ = Phase::kNone;
  std::string name_;  // UTF-8
  int wait_position_ = 0;
  uint32_t keep_limit_s_ = 0;  // secondes Unix du SERVEUR ; 0 = aucune
  uint32_t idle_limit_s_ = 0;
  bool open_ = false;

  // Le détail 0x0F40 ; `detailed_` faux tant qu'il n'est pas arrivé.
  bool detailed_ = false;
  int64_t server_ahead_s_ = 0;  // horloge du serveur moins celle de la machine
  uint32_t duration_s_ = 0;
  uint32_t idle_timeout_s_ = 0;
  int hard_mode_ = 1;
  bool turbo_ = false;
  uint8_t owner_kind_ = 0;  // e_instance_mode du serveur
  std::string owner_name_;  // UTF-8
  bool destroyable_ = false;
  bool may_destroy_ = false;
  bool on_instance_map_ = false;
  int players_inside_ = 0;
};
