#pragma once

#include <cstdint>

#include "features/plugin.h"

// QuickCast — lancer une compétence en UNE action, et enchaîner tant que la
// touche reste enfoncée (opt-in, réservé au STAFF). La répétition vaut aussi
// pour les cases d'OBJET de la barre de raccourcis (voir le bloc dédié plus bas).
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
// ── OBJETS : la même répétition, pour les cases qui portent un objet ─────────
// Un objet ne demande pas de visée : sa touche l'utilise en une action, la
// question du « clic de confirmation » ne se pose donc pas. Reste l'autre
// moitié du plugin, qui vaut telle quelle : maintenir la touche n'ouvre qu'UNE
// Old Blue Box, parce que le client ignore l'auto-répétition (même bloc que
// ci-dessus). On rejoue donc l'activation de la case tant que la touche est
// tenue.
//
// Le rejeu, c'est SkillBar::RepeatItemSlot — autrement dit exactement la voie
// de la touche (UIShortCutWnd::OnMsg 0x29), pas un CZ_USE_ITEM fabriqué : rien
// à maintenir en parallèle du client, et ses gardes survivent. C'est aussi
// SkillBar qui prévient (son hook de cet OnMsg voit toute activation, d'où
// qu'elle vienne), parce que ces cases sont à lui.
//
// ⚠ Deux limites, PROPRES aux objets :
//  • chaque répétition CONSOMME. La boucle s'arrête d'elle-même dès que la case
//    change ou que le sac est vide, mais elle ne demande rien avant d'ouvrir la
//    dernière boîte.
//  • le SERVEUR fixe l'intervalle minimal entre deux objets, et il est BAS ICI.
//    `item_use_interval` vaut 325 ms sur Moonlight (500 de plus en PvP sur les
//    soins), MAIS `pc_useitem` le ramène à **20 ms au-dessus du niveau de groupe
//    40** (patch maison « opti spam de branch en gm », src/map/pc.cpp). Or cette
//    option est réservée au staff, donc niveau >= 80 : ses utilisateurs ont
//    TOUJOURS le plancher à 20 ms. D'où une cadence séparée de celle des sorts —
//    ce n'est pas le même frein — et une plage qui descend jusque-là.
//    En pratique la limite devient la FRAME du client (~16 ms à 60 fps), d'où le
//    battement par frame plutôt qu'un OnTick bridé.
//
// Adresses spécifiques au client 20250716 (no-ASLR).
class QuickCast : public Plugin {
 public:
  const char* name() const override { return "QuickCast"; }

  // Appelé par le hook CMode::SendMsg (game_mode.cc) juste APRÈS que le natif a
  // traité une commande 0x48 « entrer en mode ciblage ». `cmode` = l'objet
  // receveur (CGameMode en jeu), porteur de l'état de ciblage à +0x408.
  void OnEnterTargeting(void* cmode);

  // Appelé par SkillBar quand une case portant un OBJET vient d'être activée,
  // quelle qu'en soit la voie (touche de l'onglet affiché, routage de l'autre
  // onglet, barre d'objets, clic sur la case). Arme la répétition si la frappe
  // qui l'a déclenchée est toujours enfoncée — un clic de souris, lui, ne
  // répète rien.
  void OnUseItemSlot(int region, int slot, uint32_t nameid);

  // Capture la touche qui va (peut-être) armer un ciblage ou utiliser un objet :
  // le hook de UIWindowMgr::ProcessPushButton prévient AVANT de laisser tourner
  // le dispatch natif du hotkey. `accurate_key` non nul = appui frais (cf.
  // l'en-tête). C'est ce qui distingue ensuite le clavier de la souris.
  void OnKeyDown(unsigned long vkey, int new_key, int accurate_key) override;

  // Rejoue le lancement mémorisé tant que la touche reste enfoncée. Appelée par
  // OnRenderUI ET par Bourgeon::OnProcessInput — la seconde garde la répétition
  // vivante quand l'interface est masquée (F11 coupe la passe UI des plugins).
  // Auto-limitée dans le temps : deux appels dans la même frame sont sans effet.
  void Update();

  // Répétition des OBJETS, appelée par Bourgeon::OnGameFrame — donc à CHAQUE
  // frame, et hors de toute frame ImGui. Les deux comptent :
  //  • hors frame ImGui, parce que le rejeu passe par une commande native
  //    (UIShortCutWnd::OnMsg), à ne jamais jouer entre NewFrame() et Render() ;
  //    c'est ce qui exclut Update(), appelée depuis la passe UI.
  //  • par frame, parce que la cadence utile descend à ~20 ms pour le staff (cf.
  //    le bloc OBJETS ci-dessus) : OnTick, bridé à ~100 ms, aurait plafonné le
  //    réglage cinq fois trop haut en donnant l'illusion de l'honorer.
  void UpdateItemRepeat();

  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Panneau de configuration (section « Staff Tools » de MoonlightUi).
  void DrawSettings();

  // Accesseurs pour la persistance (bourgeon_settings.yaml via MoonlightUi).
  bool& ground_enabled() { return ground_enabled_; }
  bool& target_enabled() { return target_enabled_; }
  int&  repeat_ms()      { return repeat_ms_; }
  bool& item_enabled()   { return item_enabled_; }
  int&  item_repeat_ms() { return item_repeat_ms_; }

 private:
  // Gardes communes au premier lancement et à chaque répétition (opt-in, staff,
  // client attendu, en jeu, hors chargement, jeu au premier plan, curseur sur le
  // monde).
  bool CanCastNow() const;

  // Prend (et efface) la touche fraîche en attente, ou 0 si elle a expiré ou
  // n'est plus enfoncée. Une action déclenchée AUTREMENT qu'au clavier n'a donc
  // rien à répéter.
  unsigned long TakePendingKey();

  bool ground_enabled_ = false;  // sorts de zone : cast direct sous la souris
  bool target_enabled_ = false;  // sorts ciblés : cast direct sur le survolé
  bool item_enabled_   = false;  // objets : répétition tant que la touche est tenue

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

  // Période de répétition des OBJETS, distincte de celle des sorts : ici le
  // plancher vient du SERVEUR, et il est à 20 ms pour le staff (cf. le bloc
  // OBJETS de l'en-tête). Le défaut reste un cran au-dessus — de quoi vider une
  // pile de boîtes en quelques secondes sans coller au tick serveur, où la
  // moindre latence ferait refuser une utilisation sur deux.
  int item_repeat_ms_ = 50;

  uint32_t last_cast_ms_ = 0;

  // Dernière touche vue en appui frais, en attente de l'action qu'elle déclenche
  // (0 = aucune), et l'instant où elle a été vue. L'horodatage compte : sans lui,
  // une touche pressée puis gardée pour tout autre chose — une direction tenue,
  // par exemple — se retrouvait associée à un déclenchement ULTÉRIEUR à la
  // souris, qui se mettait alors à se répéter tout seul. Une frappe ne vaut que
  // pour l'action qu'elle amène, c'est-à-dire dans la passe de message qui suit.
  unsigned long pending_vk_    = 0;
  uint32_t      pending_vk_ms_ = 0;

  // Lancement mémorisé, rejoué par Update() tant que `repeat_vk_` est enfoncée.
  // repeat_vk_ == 0 = pas de répétition en cours (déclenchement à la souris, par
  // exemple un clic sur une case de la barre de raccourcis).
  unsigned long repeat_vk_ = 0;
  int repeat_mode_  = 0;
  int repeat_skill_ = 0;
  int repeat_level_ = 0;

  // Répétition d'OBJET en cours : la touche tenue, et la case exacte à rejouer.
  // On mémorise aussi le nameid pour que la boucle s'arrête si la case change
  // sous nos pieds (vidée, réarrangée) plutôt que d'utiliser « ce qu'il y a
  // maintenant » à cet endroit. item_vk_ == 0 = pas de répétition en cours.
  unsigned long item_vk_     = 0;
  int           item_region_ = -1;
  int           item_slot_   = -1;
  uint32_t      item_id_     = 0;
  uint32_t      last_item_ms_ = 0;
};
