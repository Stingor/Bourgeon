#pragma once

#include <cstdint>

#include "features/plugin.h"

// Déplacement du personnage au clavier : ZQSD (AZERTY) + flèches directionnelles.
//
// RO n'a pas de déplacement clavier : on marche en cliquant une cellule au sol.
// Ce plugin réutilise EXACTEMENT le chemin natif du clic-au-sol
// (GameMode_HandleGroundClick_RequestMove 0x00c75aa0) :
//
//   1. cellule courante  = MapCoord_WorldToTileAndSub(gameMode, actor+0x10,
//                          actor+0x18, &x, &y, &subX, &subY)   [0x00c6aa80]
//   2. cible             = cellule courante + direction * « anticipation »
//   3. validation        = Cell_IsMoveTargetValid(gameMode, x, y)  [0x00c6cf80]
//      sinon repli       = Move_ClampToReachableCell(gameMode, &x, &y)
//                          [0x00c69160] (= « clic derrière un mur : marche au
//                          plus loin »)
//   4. envoi             = acteur joueur -> vtable+8 (Actor_OnMsg 0x00d473b0),
//                          message 0x11 (x, y) = « marche vers cette cellule ».
//                          Ce message construit la demande de marche via
//                          CMode::SendMsg(0x8a), avec le throttle natif de 50 ms.
//                          ⚠ Dans le TAMPON du client l'opcode est **0x035F**
//                          (`mov eax, 35Fh` en 0x00C8F865, 5 octets : opcode +
//                          x/y/dir empaquetes) ; c'est le SERVEUR qui lit
//                          0x0881, apres le brouillage natif du premier mot.
//                          Cette ligne annoncait 0x0881 : la valeur etait celle
//                          du packet_db, pas celle qu'on voit dans SendPacketHook.
//
// Aucun paquet n'est fabriqué à la main et aucune position n'est écrite : le
// serveur reste maître du déplacement, exactement comme un clic souris. Tenir
// une touche ré-émet une demande toutes les `refresh_ms_` ms — c'est le même
// comportement que garder le bouton de la souris enfoncé.
//
// Direction écran -> direction grille : la caméra RO est libre en rotation, donc
// « haut de l'écran » n'est pas toujours +Y en cellules. On lit la matrice de vue
// de la caméra (pCam = *(CGameMode+0xd0), matrice 4x3 row-major en +0x98,
// construite par FUN_0054bf80 : lignes = (axeX, axeY, axeZ) puis translation) et
// on projette les 8 directions de cellules possibles sur l'écran pour retenir
// celle qui colle le mieux à la direction demandée. Le déplacement suit donc la
// rotation de la caméra sans aucune constante devinée.
//
// Garde-fous : rien ne part si une zone de saisie native a le focus (même test
// que UIWindowMgr_OnKeyDown 0x00a471e0 pour les hotkeys « lettre »), si ImGui
// capture le clavier, hors jeu ou pendant un chargement de carte.
// Adresses spécifiques au client 20250716.
class KeyboardMove : public Plugin {
 public:
  const char* name() const override { return "KeyboardMove"; }

  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Lit les touches et émet la demande de marche. Appelée par OnRenderUI ET par
  // Bourgeon::OnProcessInput : la première donne la cadence par frame, la
  // seconde garde le déplacement vivant quand l'interface est masquée (F11 coupe
  // le dispatch de OnRenderUI). Auto-limitée dans le temps, donc appeler deux
  // fois dans la même frame ne change rien.
  void Update();

  // Toggle + réglages exposés au menu moonlight_ui.
  bool enabled() const { return enabled_; }
  // Surcharge non-const : la table de persistance décrit un réglage par
  // l'ADRESSE de sa valeur, il lui faut donc une lvalue (cf. kbmove_enabled).
  bool& enabled() { return enabled_; }
  void SetEnabled(bool on);
  int*  p_look_ahead()      { return &look_ahead_; }
  int*  p_refresh_ms()      { return &refresh_ms_; }
  bool* p_stop_on_release() { return &stop_on_release_; }
  bool* p_camera_relative() { return &camera_relative_; }
  // Mêmes deux réglages, en lvalue, pour la table de persistance.
  bool& stop_on_release() { return stop_on_release_; }
  bool& camera_relative() { return camera_relative_; }

  // ── Les touches, CONFIGURABLES (VK Windows) ────────────────────────────────
  // Quatre directions × DEUX jeux : le principal (ZQSD sur un clavier français)
  // et un second (les flèches). Les deux marchent en même temps, comme avant —
  // ce qui change, c'est qu'aucun des huit n'est plus figé dans le code.
  //
  // 🔴 Sans modificateur, et ce n'est pas un oubli : `Update` refuse de marcher
  // dès que Ctrl, Alt ou Maj est enfoncé (Alt+lettre et Ctrl+lettre sont des
  // raccourcis du jeu, Maj sert au clic forcé). Une direction sur « Ctrl+Z » ne
  // se déclencherait donc JAMAIS. L'écran de raccourcis refuse d'en poser.
  //
  // ⚠ 0 = touche retirée. Une direction peut n'avoir aucune touche ; elle est
  // alors simplement injouable, ce qui est un choix valide (jouer aux seules
  // flèches, par exemple).
  enum MoveKeySlot {
    kFwd = 0, kBack, kLeft, kRight,          // jeu principal
    kAltFwd, kAltBack, kAltLeft, kAltRight,  // second jeu
    kMoveKeyCount
  };
  // Publiques : la table de persistance décrit un réglage par l'ADRESSE de sa
  // valeur, et l'écran des raccourcis les lit et les écrit.
  // VK : 0x26 haut, 0x28 bas, 0x25 gauche, 0x27 droite.
  int keys_[kMoveKeyCount] = {'Z', 'S', 'Q', 'D', 0x26, 0x28, 0x25, 0x27};

 private:
  void Reset();  // oublie la direction en cours (changement de map, hors jeu…)

  // OFF par défaut : ZQSD sont des touches que beaucoup ont déjà liées à une
  // compétence, et un pas déclencherait alors le sort. On ne prend pas quatre
  // touches au joueur sans qu'il le demande.
  bool enabled_         = false;
  bool camera_relative_ = true;  // suivre la rotation de la caméra
  bool stop_on_release_ = true;  // au relâchement : demander la cellule courante
  int  look_ahead_      = 2;     // cellules visées devant le personnage
  int  refresh_ms_      = 150;   // période de ré-émission touche maintenue

  int      last_dx_     = 0;  // dernière direction demandée (cellules)
  int      last_dy_     = 0;
  uint32_t last_req_ms_ = 0;
  bool     was_moving_  = false;
};
