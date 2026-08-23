#pragma once

#include <cstdint>

#include "features/plugin.h"
#include "imgui.h"  // IM_COL32 (couleur par défaut de l'horloge)

// ── Écran de veille : le jeu se regarde tourner ──────────────────────────────
// Passé un délai sans la moindre entrée, l'interface s'efface et la caméra prend
// la main : elle recule, se relève et tourne lentement autour du personnage. La
// première touche ou le premier clic rend tout comme c'était.
//
// Trois leviers, tous déjà en place ailleurs dans Bourgeon — rien n'est
// reproduit ici :
//
//   1. LA CAMÉRA — `ragnarok/camera.h`. Le rig lerpe vers une pose CIBLE ; on
//      n'écrit donc que les cibles (+0x44 tilt, +0x48 yaw, +0x4c distance) et le
//      moteur fournit gratuitement l'accélération, la décélération et le retour
//      en douceur. 🔴 Et comme `Camera_ApplyViewDistanceClamp` ne tourne que
//      lorsque le joueur agit sur la caméra, en veille PERSONNE ne réécrit ces
//      cibles : nos valeurs tiennent sans avoir à lutter.
//
//   2. L'INTERFACE — deux moitiés, coupées chacune à son unique point de
//      passage : la nôtre au dispatch de `Bourgeon::RenderUI`, celle du client
//      en vetoant l'appel à `UIWindowMgr_RenderWindows` (0x00c74fd6), le même
//      motif que le radar natif (cf. features/overlays/minimap.cc).
//
//      🔴 Et NON le « cacher l'interface » natif (F11,
//      `UIWindowMgr_ToggleHideAllWindows` 0x00a47720), pourtant tentant puisque
//      `IsNativeUiHidden()` est déjà branché sur notre overlay. Cette
//      fonction-là FERME (`UIWindowMgr_SaveRectAndCloseWindow`) toutes les
//      fenêtres qui ne sont pas sur sa liste blanche, détruit le radar pour le
//      recréer ensuite, et joue un effet sur l'acteur ; elle demande même deux
//      appels quand une fenêtre non masquable traîne. Acceptable pour un geste
//      VOLONTAIRE du joueur, inacceptable pour un basculement AUTOMATIQUE :
//      partir se faire un café ne doit pas fermer ce qu'on avait ouvert. Le veto
//      de rendu, lui, ne touche à aucun état — il saute un dessin, et la frame
//      suivante peut le redessiner.
//
//   3. LA TEINTE — `D3D9PostFx`. Vignette, désaturation et grain existent déjà
//      dans la passe de post-traitement ; la veille se contente de les COMPOSER
//      par-dessus le réglage du joueur (jamais de l'écraser), et rend la main
//      exactement telle qu'elle l'a prise. DX9 uniquement : sous le proxy DX7,
//      la caméra et le masquage fonctionnent, la teinte est simplement absente.
//
// L'inactivité se mesure au seul endroit où toutes les entrées passent : le hook
// de WndProc (ragnarok/ragnarok_client.cc), qui appelle `afk::NoteInput()`.
class AfkScreen : public Plugin {
 public:
  // Réglages persistés (bourgeon_settings.yaml, préfixe `afk_`). Les défauts
  // s'écrivent ICI et nulle part ailleurs : la table de moonlight_ui les relit
  // depuis une instance par défaut (MLUI_DEFAULT).
  struct Config {
    bool  enabled       = false;  // opt-in : la veille ne surprend personne
    int   delay_s       = 90;     // silence avant bascule (10..900 s)
    // ── Caméra ───────────────────────────────────────────────────────────────
    float spin_deg_s    = 6.0f;   // vitesse d'orbite, degrés/s (négatif = sens inverse)
    // 🔴 Degrés AU-DESSUS DE L'HORIZON, en positif : c'est ce qu'un joueur règle.
    // Le moteur, lui, veut l'opposé (son pitch de repos est -45) — la conversion
    // se fait dans StepCamera, qui explique pourquoi se tromper de signe ne
    // donne pas une caméra basse mais une caméra sous le terrain.
    float tilt_deg      = 62.0f;  // 45 = comme en jeu, 85 = presque à la verticale
    float zoom_factor   = 1.6f;   // recul, en multiples de la distance de repos
    // Durée de la rampe d'ENTRÉE. Le retour se joue trois fois plus vite (cf.
    // kWakeSpeedup) : il répond à un geste du joueur, pas à une mise en scène.
    float ease_s        = 2.5f;
    // ── Habillage ────────────────────────────────────────────────────────────
    // Ce qui a le droit de RÉVEILLER (afk::kWake*). Ne change rien à ce qui
    // repousse l'endormissement : là, toute activité compte, toujours.
    int   wake_on       = 0;      // kWakeAny
    bool  hide_ui       = true;   // effacer les deux interfaces
    bool  hide_cursor   = true;   // et la flèche de la souris avec
    float vignette      = 0.45f;  // 0 = aucune (composée en MAX avec celle du joueur)
    float desaturate    = 0.0f;   // 0 = couleurs intactes, 1 = noir et blanc
    float grain         = 0.0f;   // grain argentique, 0..1
    // ── Horloge ──────────────────────────────────────────────────────────────
    bool  show_clock    = true;   // heure locale
    bool  show_away     = true;   // + la ligne « Absent depuis … »
    // Ancrage sur une grille 3×3, en ordre de lecture (0 = haut-gauche,
    // 4 = centre, 8 = bas-droite). Les autres overlays du projet se contentent
    // des quatre coins, ce qui suffit à un HUD ; ici l'écran est vide et les
    // positions qui comptent — centre et bas-centre — sont justement celles
    // qu'un choix en quatre coins ne sait pas exprimer.
    int   clock_anchor  = 7;      // bas-centre
    int   clock_margin  = 64;     // px depuis le ou les bords ancrés
    float clock_scale   = 2.2f;   // taille de l'heure (la 2ᵉ ligne en découle)
    bool  clock_shadow  = true;   // ombre portée d'un pixel
    // ARGB empaqueté à la façon d'IM_COL32 (l'ordre d'ImGui), comme l'overlay
    // FPS : la table de réglages ne connaît pas `ImVec4`, et un entier se relit
    // à l'identique d'une version à l'autre.
    uint32_t clock_col  = IM_COL32(255, 255, 255, 255);
  };

  AfkScreen();

  const char* name() const override { return "AfkScreen"; }

  Config& config() { return cfg_; }

  // Vrai dès que la veille a commencé — rampe d'entrée comprise. C'est ce que
  // `Bourgeon::RenderUI` interroge pour savoir s'il doit taire les autres
  // modules, et le stub du veto pour savoir s'il doit sauter le dessin natif.
  bool active() const { return phase_ != Phase::kAwake; }

  // L'interface doit-elle disparaître de CETTE frame ? Distinct de `active()` :
  // pendant la rampe de sortie la caméra revient encore, mais l'interface, elle,
  // est déjà rendue au joueur — sinon le réveil se paierait de deux secondes
  // d'écran nu, exactement quand il veut agir.
  bool hiding_ui() const;

  // Le curseur doit-il disparaître de cette frame ? Même règle que l'interface :
  // il revient dès le réveil, sans attendre que la caméra se soit reposée — on
  // vise avec, il ne peut pas manquer à l'appel au moment où la main revient sur
  // la souris.
  //
  // 🔴 CE MODULE N'ÉTEINT PAS LE CURSEUR LUI-MÊME, il répond seulement à la
  // question. Les DEUX curseurs vivent dans ragnarok_client.cc et l'interrogent :
  // `Hooked_CursorRender` pousse le quad natif hors écran, `DrawROCursorImGui`
  // renonce à en redessiner notre copie par-dessus nos fenêtres. Le drapeau natif
  // `g_cursor_hidden` NE CONVIENT PAS — il ne garde pas le chemin de rendu utilisé
  // en jeu ; le détail est au-dessus de `Hooked_CursorRender`.
  bool hiding_cursor() const;

  // Réveil immédiat, quelle qu'en soit la raison (entrée, sortie du monde,
  // option décochée). Sans effet si la veille n'est pas en cours.
  void Wake();

  // Entrer en veille SUR-LE-CHAMP, sans attendre le délai — le bouton d'essai du
  // panneau et le raccourci clavier « Écran de veille ». Le prochain geste
  // réveille comme d'habitude.
  //
  // ⚠ Marche même quand la veille AUTOMATIQUE est décochée : c'est un geste
  // explicite, et le joueur qui se lève de sa chaise n'a pas à activer d'abord un
  // délai dont il ne veut pas.
  void StartNow();

  // Décide d'entrer en veille ou d'en sortir (~100 ms suffisent pour un délai
  // qui se compte en dizaines de secondes).
  void OnTick() override;

  // ⚠ La caméra n'avance PAS ici. `OnRenderUI` n'est dispatché qu'après deux
  // gardes — interface native masquée (F11), HUD remplacé par une UI plein écran
  // — sous lesquelles la veille doit continuer de tourner. Il ne reste ici que
  // l'horloge, qui est du dessin.
  void OnRenderUI() override;

  // Le mouvement de caméra et la teinte, une fois par frame de JEU. Appelé
  // explicitement par Bourgeon (comme ChatBalloon, CastBar, Minimap…), donc
  // AVANT que la scène ne soit rendue : la pose écrite ici vaut pour l'image en
  // cours, pas pour la suivante.
  void OnGameFramePulse();

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

 private:
  enum class Phase { kAwake, kEntering, kHeld, kLeaving };

  void  BeginSleep();
  // Fin sans rampe : on repose la caméra et on éteint la teinte séance tenante.
  // Pour ce qui n'est pas un réveil du joueur — sortie du monde, changement de
  // carte —, là où dérouler deux secondes d'animation n'aurait aucun sens.
  void  AbortSleep();
  void  StepCamera(float dt);
  void  ApplyPostFx(float blend);
  void  RestorePostFx();
  float EaseBlend() const;   // 0..1, l'avancement de la rampe en cours
  // Une ligne de l'horloge, centrée, avec son ombre portée.
  void  DrawClockLine(const char* text, float scale, uint32_t color);

  Config cfg_;

  Phase    phase_        = Phase::kAwake;
  uint32_t phase_start_  = 0;  // GetTickCount du début de la rampe en cours
  uint32_t sleep_start_  = 0;  // GetTickCount de l'entrée en veille (pour l'horloge)

  // ── Pose de repos, reprise telle quelle au réveil ─────────────────────────
  bool  base_ok_    = false;
  float base_pitch_ = 0.0f;
  float base_yaw_   = 0.0f;
  float base_dist_  = 0.0f;
  // Angle d'orbite accumulé depuis l'entrée en veille, en degrés. Il s'ajoute au
  // yaw de repos ; c'est lui qu'on ramène à zéro pendant la rampe de sortie,
  // pour que le joueur retrouve SON orientation et pas une autre.
  float spin_deg_   = 0.0f;
  // L'orbite figée au moment du réveil, ramenée dans [-180, 180] : la caméra
  // rentre par le chemin le plus court, au lieu de dérouler à l'envers les cinq
  // tours accumulés pendant une longue absence.
  float spin_at_leave_ = 0.0f;
  uint32_t last_step_ms_ = 0;  // frame précédente, pour le dt du mouvement

  // ── Post-traitement ──────────────────────────────────────────────────────
  // Rien à sauvegarder : le réglage du joueur (`ScreenFx::fx()`) n'est JAMAIS
  // modifié. La veille en compose une copie locale qu'elle pousse vers la couche
  // d3d9, et le réveil se contente de redemander `ScreenFx::Apply()` — la
  // configuration d'origine est toujours restée la source de vérité. Une veille
  // qui s'interromprait mal ne peut donc pas laisser le jeu désaturé.
  bool fx_pushed_ = false;  // avons-nous une teinte à défaire ?

};

// ── Le compteur d'inactivité ─────────────────────────────────────────────────
// Il vit hors de la classe parce que son seul alimentateur, le hook de WndProc,
// tourne bien avant que les modules ne soient chargés — et qu'il doit rester
// utilisable même si AfkScreen n'est pas enregistré.
namespace afk {

// Ce qui a le droit de mettre fin à la veille.
//
// ⚠ Ce réglage ne dit PAS ce qui compte comme activité — ça, c'est toujours
// tout. Il dit seulement ce qui ROUVRE les yeux une fois endormi. « Clavier
// seulement » existe pour la raison la plus banale qui soit : une souris posée
// sur un bureau qu'on bouscule, un capteur trop sensible, et la veille ne tient
// jamais.
enum WakeMode : int {
  kWakeAny      = 0,  // clavier et souris
  kWakeKeyboard = 1,  // le clavier seul
  kWakeMouse    = 2,  // la souris seule
};

// Renseigné par AfkScreen à chaque battement, comme `SetSleeping`.
void SetWakeMode(int mode);


// Passe TOUS les messages de la fenêtre. Rend `true` quand le message doit être
// AVALÉ, c'est-à-dire ne parvenir ni au jeu ni à ImGui.
//
// Deux rôles en une fonction, et ce n'est pas de l'économie : il faut décider
// d'avaler AU MOMENT MÊME où l'on constate l'entrée, sur le fil de la fenêtre.
// Un simple drapeau « on s'est réveillé » consommé plus tard laisserait passer
// le clic de la frame en cours — celui-là même qui déplacerait le personnage
// dans une direction que le joueur n'a pas choisie, puisqu'il visait un décor
// vu sous un autre angle.
//
// N'avale QUE la souris (boutons et molette), jamais le clavier : une touche
// porte une intention explicite, alors qu'un clic ne vaut que par l'endroit où
// il tombe, et cet endroit vient de changer.
bool FilterMessage(unsigned int msg, intptr_t lparam);

// Date (GetTickCount) de la dernière entrée réelle, TOUTES sources confondues.
// C'est le compteur d'inactivité, celui qui décide de l'endormissement. 0 =
// aucune depuis le lancement — traité comme « à l'instant » par l'appelant,
// faute de quoi le client s'endormirait avant le premier geste du joueur.
uint32_t LastInputMs();

// Date de la dernière entrée AUTORISÉE À RÉVEILLER (cf. WakeMode). Identique à
// la précédente en mode « clavier et souris » ; c'est celle-ci que la veille
// interroge une fois endormie.
uint32_t LastWakeInputMs();

// Renseigné par AfkScreen à chaque frame : `FilterMessage` a besoin de savoir
// s'il faut avaler, et n'a pas d'autre moyen d'atteindre le module.
void SetSleeping(bool sleeping);

// Marque comme « déjà avalés » les boutons actuellement enfoncés, pour que leur
// relâchement ne mette pas fin à une veille qui vient tout juste de commencer.
void SwallowHeldButtons();

}  // namespace afk
