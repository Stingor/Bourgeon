#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "imgui.h"

#include "features/plugin.h"
#include "features/windows/chat_window.h"  // ChatWindow::Line / Run : LE modèle partagé

// ── Bulle de chat au-dessus des entités, en ImGui ────────────────────────────
//
// Remplacement COMPLET du `UITransBalloonText` natif. La rétro-ingénierie du
// chemin natif est dans **docs/entity_chat_balloon_re.md** ; l'essentiel :
//
//   · trois paquets la déclenchent, tous via le message 7 de l'acteur —
//     ZC 0x008E (`clif_displaymessage`, sur SON PROPRE acteur), ZC 0x008D
//     (par GID) et ZC 0x02C1 (`ZC_NPC_CHAT`, le seul qui porte une couleur) ;
//   · une QUATRIÈME source existe et se rate facilement : `CSkill::OnMsg`
//     (vtable 0x0109c1e8) crée la même bulle pour l'unité de sol — la Talkie
//     Box — avec un rose `0xFF8080` codé en dur ;
//   · la fenêtre native vit à `acteur+0x264`, l'horodatage à `acteur+0x248`.
//
// 🔴 POURQUOI ON REMPLACE, et pas seulement pour faire joli : chez nous la
// résolution de balises native est MORTE. Le chemin de la bulle n'appelle
// `ChatText_TransformTagLinks` que sous `if (g_pNewChatWnd)`, or ce pointeur
// vaut 0 dès que la chatbox native est détruite — ce qui est notre cas. Résultat
// mesuré en jeu : `<ITEML>` lui-même ressort littéralement, en plus de nos
// `<MOBL>` / `<ITMR>` / `<CRAF>`. Aucun réglage natif ne rattrape ça.
//
// ⚠ Mais la résolution native n'est pas ENTIÈREMENT morte, et c'est un piège :
// le msg 7 appelle un SECOND transformateur, `CTagMgr::Transform` (0x007fbfc0),
// celui-là SANS condition. Ce qu'il résout (`<MSG>MSI_…`, `<NR>`, `<NAMELESS>`)
// nous arrive donc tout fait — mais il mange aussi `<NAVIL>`, qu'il aplatit en
// `<carte x,y>` : nom INTERNE de carte, plus de balise, donc plus de lien pour
// personne. La même ligne restait pourtant cliquable dans la chatbox, qui ingère
// en amont (`ChatAction`). D'où le détour 3.
//
// ── Comment on s'y prend ────────────────────────────────────────────────────
// Trois détours de fonction ENTIÈRE, aucun patch en milieu de fonction :
//
//   1. `UIBalloonText_SetTextWrapped` (0x00830240) — OBSERVATEUR. C'est le seul
//      point par lequel passent les DEUX créateurs (l'acteur et l'unité de sol),
//      avec le texte encore brut, avant la coupure. On copie, on relaie.
//   2. `UITransBalloonText_Paint` (0x008263a0) — bretelle. Quand la fenêtre
//      peinte appartient à un acteur, on remplit sa surface de la couleur-clé et
//      on rend la main.
//   3. `CTagMgr::Transform` (0x007fbfc0) — OBSERVATEUR lui aussi, filtré sur
//      l'ADRESSE DE RETOUR du site de la bulle (0x00c4db71), puisque la fonction
//      sert tout le client. Il capte le texte d'AVANT transformation, et
//      seulement s'il porte un `<NAVIL>` : partout ailleurs c'est le texte
//      transformé qu'il faut garder.
//
// 🔴 Ce n'est PAS le détour 2 qui fait disparaître la bulle native : la fenêtre
// est DÉTRUITE, à `OnGameFramePulse`, dès que son texte est adopté — via
// `UIWindowMgr::QueueDestroyWindow`, c'est-à-dire la fonction que le natif
// s'applique déjà à lui-même à l'expiration et au despawn. Aucun de ses devoirs
// cachés n'est donc sauté.
//
// ⚠ Effacer la surface ne SUFFIT PAS, contrairement à ce qu'on pouvait croire :
// `UIWindow_Render 0x00a1ce10` blitte la surface quoi qu'il arrive, et un
// rectangle restait visible. Le détour 2 ne sert plus qu'à couvrir l'instant
// entre la création de la fenêtre et le battement qui la tue.
//
// ⚠ Le détour 1 ne peut pas filtrer : `SetTextWrapped` sert aussi aux infobulles
// (13 appelants, dont `UIWindow_ShowHoverTooltip`). C'est le détour 2 qui
// discrimine, en testant si un acteur revendique la fenêtre à son `+0x264`.
//
// Le rendu passe par `ImGui::GetBackgroundDrawList()` : la bulle reste DERRIÈRE
// toutes nos fenêtres, jamais de `SetCursorPos` (convention maison).
class ChatBalloon : public Plugin {
 public:
  ChatBalloon();
  ~ChatBalloon() override;

  const char* name() const override { return "ChatBalloon"; }

  // Ne fait plus que DESSINER : l'adoption et la destruction ont lieu avant, à
  // `OnGameFramePulse`.
  void OnRenderUI() override;

  // 🔴 Battement par frame, AVANT que le jeu ne dessine, hors de toute frame
  // ImGui (`Bourgeon::OnGameFrame`). C'est ici qu'on adopte le texte des bulles
  // natives et qu'on les DÉTRUIT.
  //
  // ⚠ Le faire depuis OnRenderUI ne marchait pas, et pas seulement à cause de
  // l'interdiction d'appeler du natif pendant une frame ImGui : OnRenderUI
  // arrive APRÈS le dessin du jeu, donc la fenêtre native restait visible une
  // frame entière — un rectangle qui clignotait derrière la bulle. Depuis
  // OnTick (~100 ms) c'était plusieurs frames.
  void OnGameFramePulse();

  // Section du panneau de réglages (« Interface »).
  void DrawSettings();

  // 🔴 PAS d'interrupteur propre. La bulle est une conséquence de la chatbox
  // moderne, pas une option à côté : elle n'existe que parce que celle-ci
  // détruit la fenêtre native et emporte avec elle la résolution de balises du
  // client (`g_pNewChatWnd` passe à 0, cf. docs/entity_chat_balloon_re.md).
  // Chatbox native rallumée, le client sait de nouveau résoudre ses liens et sa
  // propre bulle redevient correcte — la nôtre n'aurait plus lieu d'être.
  bool Active() const;

  // Une bulle est-elle affichée EN CE MOMENT au-dessus de cet acteur ?
  //
  // Posée pour la barre d'incantation (features/overlays/cast_bar.cc) : quand le
  // client annonce le sort dans une bulle — ce qu'il fait pour les JOUEURS, pas
  // pour les monstres — réécrire son nom sur la barre fait doublon, et l'étiquette
  // tombe en plus dans le cadre de la bulle. Répondre à la question plutôt que de
  // deviner « c'est un joueur » couvre aussi le cas inverse, et le cas d'un mob
  // qui parle.
  //
  // ⚠ Déréférence l'acteur : à appeler depuis un chemin déjà sous garde
  // d'exception. Faux si l'overlay est éteint — la fenêtre native est alors
  // vivante, et c'est à `acteur+0x264` qu'il faut regarder.
  bool HasBalloonFor(void* actor) const;

  // Accesseurs pour la persistance (bourgeon_settings.yaml via MoonlightUi).
  bool&  show_self()          { return show_self_; }
  bool&  fade()               { return fade_; }
  bool&  follow_native_life() { return follow_native_life_; }
  int&   base_life_ms()       { return base_life_ms_; }
  int&   per_char_ms()        { return per_char_ms_; }
  int&   max_life_ms()        { return max_life_ms_; }
  int&   y_offset()           { return y_offset_; }
  float& font_scale()         { return font_scale_; }
  float& max_width_ratio()    { return max_width_ratio_; }
  float& opacity()            { return opacity_; }

  // ── Appelés depuis les stubs de détour ─────────────────────────────────────
  // Copie le texte brut annoncé pour cette fenêtre native. Pas de décodage ici :
  // on ne sait pas encore à quel acteur elle appartient, et on ne veut pas
  // parcourir la liste d'acteurs sous le détour.
  // `ret_addr` = l'adresse de retour de l'appelant, transmise telle quelle par le
  // stub : c'est elle, et rien d'autre, qui dit si la bulle vient d'un ACTEUR.
  static void OnNativeSetText(void* window, const char* wire, uintptr_t ret_addr);
  // Copie le texte du msg 7 AVANT le transformateur de balises global
  // (`CTagMgr::Transform`), et SEULEMENT s'il porte un `<NAVIL>` : c'est la
  // seule balise que ce transformateur aplatit alors que notre parseur, lui,
  // sait en faire un lien. `native_string` est une std::string MSVC native.
  static void OnNativeTagTransform(const void* native_string);
  // Vrai si un acteur revendique cette fenêtre à son `+0x264` : le natif doit
  // alors se taire.
  static bool IsActorBalloon(void* window);

 private:
  // Une bulle vivante, indexée par AID (ou par pointeur d'acteur pour les
  // entités sans AID exploitable, cf. `KeyForActor`).
  //
  // ⚠ Elle ne garde AUCUN pointeur vers la fenêtre native : celle-ci est
  // détruite dès que son texte est adopté. Tout ce qu'elle portait d'utile (le
  // texte, la couleur) est recopié ici au moment de l'adoption.
  struct Balloon {
    // 🔴 Les FRAGMENTS du chat, pas une chaîne : c'est ce qui porte les couleurs
    // par morceau, l'index d'emote du jeu et les icônes d'objet. Une chaîne
    // plate laissait « :question: » écrit en toutes lettres.
    ChatWindow::Line line;
    uint32_t    rgb = 0xFFFFFF;  // couleur de repli (ZC_NPC_CHAT, Talkie Box)
    uint32_t    born_ms = 0;   // GetTickCount() de la dernière réplique
    uint32_t    life_ms = 0;   // durée retenue pour CE message
  };
  // La taille n'est pas mémorisée : elle se recalcule à chaque frame par la
  // passe de mesure de `LayoutRuns`. C'est quelques dizaines de mots pour une
  // poignée de bulles — et un cache aurait à s'invalider sur la police, la
  // largeur d'écran ET le contenu, pour rien.

  // Fenêtre native adoptée, en attente de destruction au battement de frame. On
  // garde l'acteur avec, pour ne remettre son `+0x264` à zéro que si c'est bien
  // encore CETTE fenêtre qu'il porte.
  struct Doomed {
    void* actor;
    void* window;
  };

  void ResetWhenDisabled();
  void SyncGuarded();  // le __try, seul dans sa fonction (C2712)
  void DrawBalloons();
  void DestroyAdopted(const std::vector<Doomed>& doomed);
  // Rapproche les textes captés des acteurs qui les portent, et purge ce qui a
  // expiré. Un seul parcours de la liste d'acteurs par frame.
  void SyncWithActors();

  // Une SEULE passe de mise en page pour les deux usages : `dl == nullptr` se
  // contente de mesurer (on a besoin de la taille avant de savoir où poser la
  // bulle), sinon elle dessine. Deux implémentations divergeraient au premier
  // ajustement d'espacement, et le cadre ne collerait plus au texte.
  void LayoutRuns(const ChatWindow::Line& line, float wrap, float font_px,
                  uint32_t fallback_rgb, ImDrawList* dl, ImVec2 origin,
                  int alpha255, ImVec2* out_size);

  bool show_self_ = true;
  bool fade_ = true;
  bool follow_native_life_ = false;  // false = notre durée proportionnelle
  int  base_life_ms_ = 5000;         // le natif : 5000 ms fixes, quelle que soit la longueur
  int  per_char_ms_ = 45;            // rallonge par caractère (0 = durée fixe)
  int  max_life_ms_ = 12000;
  int  y_offset_ = 0;
  float font_scale_ = 1.0f;
  float max_width_ratio_ = 0.28f;    // largeur max de bulle, en fraction d'écran
  // Opacité d'ensemble (fond, liseré, texte). Facteur MULTIPLICATIF du fondu de
  // fin, pas un remplacement : les deux se composent.
  float opacity_ = 1.0f;

  std::unordered_map<uint32_t, Balloon> balloons_;
  std::vector<Doomed> pending_destroy_;

  // Boîte aux lettres du détour. Le dispatch natif tourne sur le fil du jeu,
  // mais on ne PARIE pas là-dessus pour une structure partagée : verrou court,
  // copie, et tout le décodage se fait dans la frame.
  struct Pending {
    void*       window;
    std::string wire;
    // Le MÊME message, tel qu'il était AVANT `CTagMgr::Transform`. Vide sauf
    // s'il portait un `<NAVIL>` — cf. `OnNativeTagTransform`.
    std::string raw;
  };
  static std::mutex          s_mutex;
  static std::vector<Pending> s_pending;
  // Armé par le détour de `CTagMgr::Transform`, consommé par le tout prochain
  // `OnNativeSetText` : les deux appels se suivent dans le même handler natif.
  static std::string s_pending_raw;
  // Fenêtres natives dont on sait déjà qu'elles appartiennent à un acteur :
  // évite de reparcourir la liste d'acteurs à chaque repeinte.
  static std::unordered_set<void*> s_claimed;
  // 🔴 Les fenêtres nées du site d'appel de la bulle d'ACTEUR, avec l'instant de
  // leur naissance. Le filet qui couvre l'intervalle « la fenêtre existe, mais on
  // ne peut pas encore dire à qui elle est » — celui d'un CHANGEMENT DE CARTE,
  // pendant lequel `ActiveModeIfReady()` rend 0 et où le natif peignait donc sa
  // bulle une frame, au milieu de l'écran. Vidé dès que `SyncWithActors` a pu
  // regarder (il ne garde que ce qu'un acteur porte encore), et de toute façon
  // périmé au bout de la vie native : au-delà, l'adresse peut être celle d'une
  // infobulle, qui doit rester visible.
  static std::unordered_map<void*, uint32_t> s_born_from_actor;
};
