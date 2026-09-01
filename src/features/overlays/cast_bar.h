#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "features/plugin.h"

// ── Barre d'incantation, en ImGui ────────────────────────────────────────────
//
// Remplacement de la barre de cast native au-dessus des entités. La
// rétro-ingénierie est dans **docs/entity_chat_balloon_re.md** §8 et §12 ;
// l'essentiel tient en quelques lignes :
//
//   · le widget natif est un `UIRechargeGage` (vftable 0x0102bbc8, 0xA4 o) posé
//     à `acteur+0x270`, créé par le **msg 82** et retiré par le **msg 83** ;
//   · son Paint (0x00855910) n'est que TROIS RECTANGLES PLATS de 60×6 pixels :
//     fond, cuvette, remplissage. Aucun texte — ni nom de compétence, ni durée ;
//   · l'acteur porte lui-même les deux horodatages `timeGetTime` qui décrivent
//     l'incantation : `+0x284` DÉBUT et `+0x280` FIN. Progression, écoulé et
//     restant s'en déduisent pour n'importe quelle entité, SANS toucher au
//     réseau.
//
// 🔴 ON NE DÉTRUIT PAS LA FENÊTRE NATIVE, contrairement à la bulle de chat.
// `CActorSprite_UpdateOverheadWidgets` relit `acteur+0x270` chaque frame pour
// décider de l'expiration et envoyer le msg 83 — lequel fait autre chose que
// retirer la barre (branche `acteur+0x70 == 8`). La détruire reviendrait à
// reprendre ses devoirs sans les connaître. On la masque donc par son drapeau
// natif `+0x28` (`uiwnd::SetVisible`) et toute la machinerie de temps du client
// continue de tourner — c'est elle qui nous alimente.
//
// ⚠ Conséquence : le masquage doit être ANNULÉ quand on s'éteint, sinon une
// incantation en cours resterait invisible. `RestoreNatives` s'en charge.
//
// Le rendu passe par `ImGui::GetBackgroundDrawList()` : la barre reste derrière
// toutes nos fenêtres, jamais de `SetCursorPos` (convention maison).
//
// ── Le nom de la compétence ──────────────────────────────────────────────────
// 🔴 Le msg 82 ne transporte QUE la durée : `Effect_ApplySkillCastVisual`
// (0x00cee6e0) reçoit bien le skillId mais ne le passe pas au widget. Pour
// nommer l'incantation il faut donc capter le paquet nous-mêmes. Les trois
// opcodes partagent la même disposition (offsets comptés depuis l'opcode) :
//     +0x02 srcGID · +0x0E skillId · +0x14 durée en ms
// soit, après l'en-tête de 2 octets que `RegisterObserveOpcode` retire :
//     +0x00 srcGID · +0x0C skillId · +0x12 durée.
class CastBar : public Plugin {
 public:
  CastBar();

  const char* name() const override { return "CastBar"; }

  // Ne fait que DESSINER. Le relevé des acteurs et le masquage des fenêtres
  // natives ont lieu avant, au battement de frame.
  void OnRenderUI() override;

  // 🔴 Battement par frame, AVANT que le jeu ne dessine, hors de toute frame
  // ImGui (`Bourgeon::OnGameFrame`). Même raison que pour les bulles :
  // `OnRenderUI` arrive APRÈS le dessin du jeu, donc y masquer une fenêtre
  // native la laisserait visible une frame entière.
  void OnGameFramePulse();

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Section du panneau de réglages (« Interface »).
  bool DrawSettings();

  // ── Notre propre incantation, pour la barre HUD de BasicInfo ───────────────
  // Instantané relevé au battement de frame, PAS à la lecture : BasicInfo dessine
  // à l'intérieur d'une frame ImGui et n'a pas de garde d'exception, il ne doit
  // donc pas déréférencer l'acteur lui-même.
  struct OwnCast {
    bool  active       = false;
    float frac         = 0.0f;  // 0..1
    int   elapsed_ms   = 0;
    int   total_ms     = 0;
    int   remaining_ms = 0;
    char  name[32]     = {0};  // vide = compétence inconnue (paquet non capté)
  };
  const OwnCast& own_cast() const { return own_cast_; }
  // Libellé prêt à peindre : « Storm Gust 1,4 s », ou « 1,4 s » sans nom connu.
  void OwnCastLabel(char* out, size_t n) const;

  // ── Accesseurs pour la persistance (bourgeon_settings.yaml) ────────────────
  bool&  enabled()        { return enabled_; }
  bool&  show_players()   { return show_players_; }
  bool&  show_monsters()  { return show_monsters_; }
  bool&  show_npcs()      { return show_npcs_; }
  bool&  hide_own()       { return hide_own_; }
  int&   name_mode()      { return name_mode_; }
  bool&  show_time()      { return show_time_; }
  bool&  border()         { return border_; }
  int&   width()          { return width_; }
  int&   height()         { return height_; }
  int&   y_offset()       { return y_offset_; }
  float& rounding()       { return rounding_; }
  float& font_scale()     { return font_scale_; }
  float& opacity()        { return opacity_; }

  // ⚠ Les couleurs sont des MEMBRES publics, pas des accesseurs, parce que la
  // table de réglages prend l'adresse du champ (`&owner->expr`) : un accesseur
  // rendant `float*` lui donnerait l'adresse du pointeur temporaire.
  float bg_color_[4]       = {0.05f, 0.05f, 0.07f, 0.78f};
  float fill_color_[4]     = {0.42f, 0.68f, 1.00f, 1.00f};  // bleu incantation
  // Teinte distincte pour les MONSTRES : en PvM, le cast qu'on surveille n'est
  // pas le sien, et une couleur commune le noie au milieu d'un pack.
  float mob_fill_color_[4] = {1.00f, 0.55f, 0.25f, 1.00f};

 private:
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  void SyncGuarded();   // le __try, seul dans sa fonction (C2712)
  void SyncWithActors();
  void RestoreNatives();  // rend leur visibilité aux fenêtres qu'on masquait
  void DrawBars();

  // true si quelqu'un a besoin du relevé : nous, ou la barre HUD de BasicInfo.
  bool NeedsSync() const;
  // Nom de la compétence lancée par ce GID, ou nullptr si on ne l'a pas capté.
  //
  // ⚠ `cast_start` n'est pas décoratif : il APPARIE l'entrée à l'incantation en
  // cours. Sans lui, un paquet vieux de trente secondes nommerait la barre d'un
  // sort suivant dont on aurait raté le paquet — une erreur affichée avec
  // aplomb, pire qu'une barre anonyme.
  const char* SkillNameForGid(uint32_t gid, uint32_t cast_start) const;

  // Une bulle est-elle affichée au-dessus de cet acteur en ce moment ? Interroge
  // ChatBalloon quand il a pris la main, sinon lit `acteur+0x264`. Sert au mode
  // « nom du sort : si pas déjà annoncé ».
  bool EntityHasBalloon(void* actor) const;

  bool enabled_       = true;
  bool show_players_  = true;
  bool show_monsters_ = true;
  bool show_npcs_     = true;
  // Masque NOTRE barre au-dessus de la tête — pour qui la veut en barre
  // d'interface (BasicInfo) et pas sur le personnage. Agit aussi quand le
  // remplacement est éteint : dans ce cas on masque la fenêtre NATIVE, sinon
  // décocher le remplacement ferait réapparaître la barre qu'on voulait cacher.
  bool hide_own_      = false;
  // Nom du sort : 0 jamais · 1 **si pas déjà annoncé** (défaut) · 2 toujours.
  //
  // 🔴 Le défaut n'est pas « toujours » parce que le client ANNONCE DÉJÀ le sort
  // dans une bulle au-dessus de la tête — pour les JOUEURS. Le réécrire sur la
  // barre le donne deux fois, et l'étiquette tombe dans le cadre de la bulle.
  // Les monstres, eux, n'annoncent rien : c'est là que le nom a de la valeur.
  // Interroger la bulle plutôt que tester « est-ce un joueur » couvre aussi le
  // mob qui parle et le joueur dont la bulle a expiré au milieu d'un long cast.
  int  name_mode_     = 1;
  bool show_time_     = true;
  bool border_        = true;
  int   width_        = 76;   // le natif : 60 px, illisible
  int   height_       = 9;    // le natif : 6 px
  int   y_offset_     = 0;
  float rounding_     = 3.0f;
  float font_scale_   = 0.80f;
  float opacity_      = 1.0f;

  // Au moins une fenêtre native masquée par nous : sert à ne lancer la
  // restauration qu'à bon escient. `GameMode_GetActive` rend déjà nullptr hors
  // jeu, il n'y a donc pas de drapeau « en jeu » à tenir en plus.
  bool natives_hidden_ = false;

  OwnCast own_cast_;

  // GID -> compétence en cours d'incantation, alimenté par les ZC de cast. Le
  // client, lui, jette le skillId (cf. l'en-tête). Purgé sur l'âge : une entrée
  // ne sert qu'à nommer la barre qui vit en ce moment.
  struct WireCast {
    uint32_t stamp_ms = 0;    // timeGetTime de la réception
    char     name[32] = {0};  // résolu UNE fois, à la réception (cf. HandlePacket)
  };
  std::unordered_map<uint32_t, WireCast> wire_casts_;
};
