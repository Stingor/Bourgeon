#pragma once

#include <cstdint>

#include "features/plugin.h"

// QuickCast — lancer une compétence en UNE action (opt-in, réservé au STAFF).
//
// Nativement, une compétence de zone ou ciblée demande DEUX actions : la touche
// arme le « mode ciblage » (curseur de visée), puis le CLIC résout la souris et
// lance vraiment. Ce plugin supprime la seconde.
//
// ── Comment, et pourquoi PAS autrement ──────────────────────────────────────
// Trois façons de se passer du clic, deux mauvaises :
//
//  1. Simuler le clic (poser l'octet brut de g_Mouse). PREMIÈRE VERSION, ABANDONNÉE :
//     dépend de l'état de la souris, expose à une course de timing avec la boucle
//     de rendu (l'état de frame met 3 frames à retomber), risque de cliquer une
//     fenêtre native, et surtout NE RÉPÈTE PAS au sol — le cast au sol exige un
//     appui *frais* à chaque fois, que l'auto-répétition clavier ne fournit pas
//     de façon fiable.
//  2. Fabriquer le paquet CZ_USE_SKILL / CZ_USE_SKILL_TOGROUND. REFUSÉ : on saute
//     la barre de cast, l'animation, les cooldowns clients et le rangement du
//     niveau que le serveur attend (menuskill_val, dont dépendent les listes de
//     fabrication) ; et il faudrait maintenir une table de paquets par famille de
//     compétence en parallèle de celle que le client possède déjà.
//  3. ✅ Émettre le MESSAGE que le clic aurait produit. C'est ce que fait ce
//     plugin, et c'est déjà le pattern de KeyboardMove (qui appelle
//     Actor_OnMsg(0x11) au lieu de simuler un clic-sol ou de forger CZ:WalkToXY).
//
// Le clic natif n'envoie pas un paquet : il aboutit à un message posté à l'acteur
// joueur (scène GameMode+0xCC, acteur +0x2C), via sa vtable+8 :
//   • sol    : Actor_OnMsg(0x41, skillId, x, y)  puis  Actor_OnMsg(0x5A, niveau)
//              — cellule résolue par GameMode_PickGroundCellUnderMouse 0x00C69A40
//   • cible  : Actor_OnMsg(0x29, GID, skillId)   puis  Actor_OnMsg(0x5A, niveau)
//              — GID pris dans le quadtree de picking des nameplates
// Puis on désarme le ciblage avec CMode::SendMsg(0x47), exactement comme le
// pipeline souris natif le fait après un cast.
//
// Tout ce qui compte reste donc NATIF : animation, barre de cast, cooldowns,
// construction et envoi du paquet — et le serveur reste seul juge de la légalité
// (SP, portée, cooldown, cible autorisée).
//
// 🔑 CES MESSAGES N'ENVOIENT RIEN : ils REMPLISSENT UNE FILE dans l'acteur —
// `+0x500` nature (3 = sur cible, 4 = au sol), `+0x514` GID, `+0x524` id,
// `+0x528` niveau (posé par le 0x5A : case 90 de Actor_OnMsg, écriture vérifiée
// en 0x00D47B67). C'est `Actor_ProcessPendingAction_Tick` 0x00D43400 qui la
// consomme à la frame suivante et émet le paquet. Deux conséquences heureuses :
// l'ordre « lancement puis niveau » est correct (les deux remplissent la file
// avant qu'elle ne soit lue), et la file n'ayant QU'UN slot, répéter n'inonde pas
// le réseau — on écrase une requête en attente, on n'en empile pas trente.
// Les gardes natives du message survivent aussi (le 0x41 refuse le motion state
// 6 = FREEZE). Cf. la RE de la file dans project_skill_input_latency.
//
// ⚠ CE QU'ON PERD, ET QU'ON RÉIMPLÉMENTE ICI : les validations qui vivaient dans
// le chemin du CLIC (CursorMgr_UpdateHover / GameMode_HandleActorClick) — refus de
// se cibler soi-même en offensif, règles Maj/PVP/GVG. Elles ne sont pas dans
// Actor_OnMsg. Les pré-checks de ce plugin ne sont donc plus un simple filtre de
// confort : ils sont le contrat. Cf. docs/target_system_re.md §5.
//
// Modes de ciblage (CGameMode+0x408) : 0 aucun · 1 sol · 2 cible offensive · 3 ·
// 4 soutien · 5 piège. Les modes 3 et 5 gardent le ciblage natif.
//
// ── RÉPÉTITION : pourquoi une boucle À NOUS est indispensable ───────────────
// 🔴 Le client IGNORE l'auto-répétition clavier. Game_MainWndProc (0x00DB8100)
// appelle UIWindowMgr_OnKeyDown avec un quatrième argument valant
// `(lParam & 0x40000000) == 0` — le bit 30 de lParam est l'état PRÉCÉDENT de la
// touche, donc l'argument n'est vrai que sur un appui FRAIS. Or tout le bloc
// hotkey de UIWindowMgr_OnKeyDown (0x00A471E0) est sous `if (cet argument)`.
// Conséquence : maintenir une touche n'émet AUCUN 0x71/0x48 supplémentaire, et
// se contenter de réagir au 0x48 ne peut donner qu'un lancement par pression.
// (Ce que le natif répète — GameMode_RepeatActorAction 0x00C77120 — dépend de la
// SOURIS tenue et d'une cible mémorisée en CGameMode+0xF0 ; il n'a jamais eu
// d'équivalent au sol, d'où l'asymétrie observée.)
// On mémorise donc le lancement (mode, id, niveau) et la touche qui l'a déclenché
// — capturée dans OnKeyDown, qui passe juste AVANT le dispatch natif du hotkey —
// puis Update() rejoue le lancement par frame tant que GetAsyncKeyState confirme
// la touche enfoncée. La visée est ré-évaluée à chaque fois : le sort suit le
// curseur. `repeat_ms_` est donc la VRAIE période de répétition.
//
// Adresses spécifiques au client 20250716 (no-ASLR).
class QuickCast : public Plugin {
 public:
  const char* name() const override { return "QuickCast"; }

  // Appelé par le hook CMode::SendMsg (game_mode.cc) juste APRÈS que le natif a
  // traité une commande 0x48 « entrer en mode ciblage ». `cmode` = l'objet
  // receveur (CGameMode en jeu), porteur de l'état de ciblage à +0x408.
  void OnEnterTargeting(void* cmode);

  // Capture la touche qui va (peut-être) déclencher un 0x48 : le hook de
  // UIWindowMgr::ProcessPushButton prévient AVANT de laisser tourner le dispatch
  // natif du hotkey. `accurate_key` non nul = appui frais (cf. l'en-tête).
  void OnKeyDown(unsigned long vkey, int new_key, int accurate_key) override;

  // Rejoue le lancement mémorisé tant que la touche reste enfoncée. Appelée par
  // OnRenderUI ET par Bourgeon::OnProcessInput — la seconde garde la répétition
  // vivante quand l'interface est masquée (F11 coupe la passe UI des plugins).
  // Auto-limitée dans le temps : deux appels dans la même frame sont sans effet.
  void Update();

  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Panneau de configuration (section « Staff Tools » de MoonlightUi).
  void DrawSettings();

  // Accesseurs pour la persistance (bourgeon_settings.yaml via MoonlightUi).
  bool& ground_enabled() { return ground_enabled_; }
  bool& target_enabled() { return target_enabled_; }
  int&  repeat_ms()      { return repeat_ms_; }

 private:
  // Gardes communes au premier lancement et à chaque répétition (opt-in, staff,
  // client attendu, en jeu, hors chargement, curseur sur le monde).
  bool CanCastNow() const;

  bool ground_enabled_ = false;  // sorts de zone : cast direct sous la souris
  bool target_enabled_ = false;  // sorts ciblés : cast direct sur le survolé

  // Période de répétition tant que la touche est maintenue, et intervalle minimal
  // entre deux lancements en général.
  //
  // ⚠ Ce n'est PAS la première ligne de défense : le cooldown RÉEL de la
  // compétence est consulté avant (ro::SkillCooldownRemainingMs, alimentée par
  // ZC_SKILL_POSTDELAY 0x043D). Mais cette table ne couvre que les compétences
  // QUI ONT un cooldown — la plupart des sorts n'en ont pas, ils sont bornés par
  // le délai d'après-incantation (`canact_tick` côté serveur), lequel n'est jamais
  // transmis au client. Cette période est donc ce qui règle le rythme pour eux.
  int repeat_ms_ = 200;

  uint32_t last_cast_ms_ = 0;

  // Dernière touche vue en appui frais, en attente d'un 0x48 (0 = aucune).
  unsigned long pending_vk_ = 0;

  // Lancement mémorisé, rejoué par Update() tant que `repeat_vk_` est enfoncée.
  // repeat_vk_ == 0 = pas de répétition en cours (déclenchement à la souris, par
  // exemple un clic sur une case de la barre de raccourcis).
  unsigned long repeat_vk_ = 0;
  int repeat_mode_  = 0;
  int repeat_skill_ = 0;
  int repeat_level_ = 0;
};
