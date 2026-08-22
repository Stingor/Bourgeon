#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "features/plugin.h"
#include "ragnarok/game_settings.h"

// ── GameSettings ─────────────────────────────────────────────────────────────
//
// La fenêtre « Game Settings » (`CUIGameSettingsUI`, id **0x271E** = 10014) en
// ImGui. RE complète : docs/game_option_re.md §3.
//
// ── CE QUI EST PORTÉ, ET CE QUI RESTE AU NATIF ──────────────────────────────
// Portés : **Effets**, **Contrôles**, **Divers** — les trois onglets que le
// client construit à partir d'une TABLE (62 lignes lues dans `GameSettings.lub`),
// donc les seuls qui se rejouent sans rien figer — **Basique** (groupe Audio,
// les deux bascules `TALKTYPE`, le skin et la priorité du processus, docs §3.9)
// et **Graphismes** (docs §3.10).
//
// ⛔ **LA FENÊTRE NATIVE N'EST PLUS JAMAIS OUVERTE.** Le panneau ne la fabrique
// nulle part ; elle n'apparaît que si le joueur a remis le menu Échap du client,
// et elle est alors masquée puis détruite.
//
// ⚠ L'onglet Graphismes sépare ce qui s'applique AU CLIC (finesse des sprites et
// des textures, filtrage) de ce que le client ne lit **qu'à son démarrage** (API,
// adaptateur, résolution, plein écran). Ce n'est pas notre choix : le [Apply] du
// client ne fait aucun reset de device — et ne relance rien non plus, son drapeau
// « restart » se contentant d'ouvrir une page web. Les quatre réglages
// structurels restent donc un brouillon jusqu'à validation, puis sont écrits ET
// sauvés, pour le prochain lancement. Le §3.10 du doc raconte le piège en entier.
//
// ⛔ Le groupe **Courrier (RODEX)** du natif n'est PAS repris : il est mort sur
// Moonlight, le serveur ne mappant pas le paquet que le client émet. Détail et
// marche à suivre dans le .cc, à l'endroit où il aurait pris place.
//
// ── POURQUOI CE PANNEAU EST PLUS UTILE QUE LE NATIF ─────────────────────────
//   - une RECHERCHE, et une vue « Tout » qui fusionne les trois onglets : le
//     natif oblige à savoir d'avance dans quel onglet vit un réglage ;
//   - la DESCRIPTION de chaque option en infobulle — le natif la met dans une
//     colonne tronquée à droite, et sa fenêtre d'aide demande un second clic ;
//   - la COMMANDE SLASH équivalente affichée à côté du libellé : c'est ce que le
//     champ `Tooltip` du Lua contient réellement, et le natif ne le montre pas ;
//   - les options revenues à leur DÉFAUT sont distinguées de celles que le joueur
//     a changées, ce que le natif ne dit nulle part.
//
// ── CE QUI EST ROUTÉ VERS LE CLIENT, ET NE SERA JAMAIS RECOPIÉ ──────────────
// La liste des options, leurs libellés, leurs défauts et l'écriture des valeurs
// passent tous par `ragnarok/game_settings.h`, donc par le manager natif. Le
// panneau ne connaît aucun identifiant d'option — sauf les deux du groupe Audio,
// que le CLIENT lui-même code en dur dans son groupe (docs §3.3).
//
// ── INTERCEPTION ────────────────────────────────────────────────────────────
// Deux chemins, et le premier suffit en usage normal :
//   1. notre menu Échap ouvre CE panneau au lieu de fabriquer la native ;
//   2. filet de sécurité — si le joueur a remis le menu Échap natif, son bouton
//      « game settings » fabrique la 0x271E : le hook de `MakeWindow` la masque et
//      bascule ici, et `OnTick` la détruit.
// Vérifié par recherche d'octets : `MakeWindow(0x271E)` n'existe qu'à un seul
// endroit dans l'image, la commande 458 du menu Échap (docs §5.2).
//
// 🔴 DÉTRUIRE, PAS MASQUER : une native laissée vivante mais invisible avale un
// appui sur deux (le schéma du client est *ferme-si-existe / crée-sinon*) et garde
// le clavier. Cf. reference_native_window_toggle_router.
//
// Comme le menu Échap et la table des raccourcis, ce panneau est **hors du groupe
// « Interface moderne » et ON par défaut** : il ne remplace pas un morceau de HUD
// mais un écran de réglages, il n'a besoin de rien de moderne pour fonctionner —
// tout passe par le manager du client — et il est dessiné au-dessus de tout, là
// où le natif s'enterre sous les fenêtres du jeu.

// ── Les onglets vus par le système de liens ─────────────────────────────────
//
// Un onglet de ce panneau est une DESTINATION comme une autre : Maj + clic sur
// sa languette pose « [Réglage: Graphismes] » dans le chat, et un lecteur qui
// clique dessus arrive ici, sur le bon onglet.
//
// 🔴 LA CLÉ N'EST PAS LE NUMÉRO D'ONGLET, même règle que pour les sections de
// Moonlight Settings : un lien voyage vers d'autres clients, et la numérotation
// du Lua n'est ni contiguë ni stable d'une version du client à l'autre. Une clé
// posée ne se renomme donc **jamais** — les lignes de chat déjà envoyées la
// portent encore. Le libellé, lui, est libre : il est traduit à l'affichage.
//
// ⚠ `graphics` est délibérément l'ANCIENNE clé de la section « Graphismes » de
// Moonlight Settings, qui a déménagé dans l'onglet du même nom : les liens déjà
// posés dans le chat continuent d'ouvrir la bonne chose.
namespace gslink {

// Aucun onglet — valeur hors de toute numérotation, la nôtre comprise.
constexpr int kNoTab = INT32_MIN;

// L'onglet que désigne cette clé, `kNoTab` si elle n'est pas des nôtres.
int TabByKey(const char* key);

// Le libellé NON TRADUIT d'une clé, `nullptr` si inconnue. C'est ce que
// `iface::DestLabel` consulte pour composer « [Réglage: …] ».
const char* LabelByKey(const char* key);

// La clé d'un onglet, `nullptr` s'il n'est pas liable.
const char* KeyByTab(int tab);

}  // namespace gslink

class GameSettings : public Plugin {
 public:
  const char* name() const override { return "GameSettings"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Hook de `MakeWindow` pour l'id 0x271E : masque la native (détruite au tick) et
  // bascule notre panneau. No-op quand c'est NOUS qui l'ouvrons (bouton
  // « réglages graphiques »).
  void HandleNativeCreation(void* win);

  // Ouvre le panneau depuis le menu Échap (bouton « Réglages du jeu »).
  void OpenFromMenu();

  // Ouvre le panneau SUR UN ONGLET donné. C'est ce qu'honore un lien de réglage
  // reçu dans le chat — même chemin que le saut vers une section de Moonlight
  // Settings, à ceci près que la destination est ici un onglet.
  void OpenTab(int tab);

  bool IsOpen() const { return open_; }

  // Onglet supplémentaire, que le client n'a pas : les trois listes réunies.
  // C'est lui qui donne son sens à la recherche — on cherche un réglage sans
  // savoir dans quel onglet le client l'a rangé.
  // 🔴 NÉGATIFS, et c'est une correction de bug. Ces deux-là sont NOS onglets, pas
  // ceux du client : ils doivent être hors de la numérotation du Lua, qui n'est
  // ni contiguë ni bornée (EFFECT=1, CONTROL=2, GRAPHIC=3, ETC=4 — un client plus
  // récent peut en ajouter). `kTabBasic` valait 4 et est entré en collision avec
  // `ETC` le jour où celui-ci a été lu correctement.
  static constexpr int kTabAll      = -1;
  static constexpr int kTabBasic    = -2;
  static constexpr int kTabGraphics = -3;

  // ── Settings PERSISTANTS (bourgeon_settings.yaml, via MoonlightUi) ──────────
  bool imgui_enabled_ = true;

  // ── Bordure d'emblème ──────────────────────────────────────────────────────
  //
  // 🔴 PERSISTÉE CHEZ NOUS, PARCE QUE LE CLIENT NE LA PERSISTE PAS DU TOUT. La
  // chaîne « Emblem Frame » n'existe **nulle part dans l'exécutable** : la ligne
  // qu'on trouve dans `SaveData\OptionInfo.lua` est un fossile qu'aucun code ne
  // relit ni ne réécrit. Et son drapeau (`TT_EMBLEM_FRAME_ON_OFF` 0xF3) n'est ni
  // dans `OptionTbl` ni dans `CmdOnOffList` : la clé n'existe donc même pas dans
  // la table des drapeaux au démarrage, et `GetFlag` rend 0. D'où le « revient
  // sur OFF à chaque relance » constaté en jeu.
  //
  // On garde donc la valeur de notre côté et on la réinjecte à l'entrée en jeu.
  bool emblem_frame_ = false;
  bool& emblem_frame() { return emblem_frame_; }

 private:
  void Close();
  // Réinjecte `emblem_frame_` dans la table des drapeaux du client. Une fois par
  // entrée en jeu : la table est reconstruite à chaque session.
  void ApplyEmblemFrame();
  bool emblem_frame_applied_ = false;
  void RefreshRows();

  void DrawBasicTab();
  void DrawGraphicsTab();
  // Le bas de l'onglet Graphismes : les effets d'écran et les sprites d'armes
  // doubles, qui n'appartiennent pas au client. Venus de Moonlight Settings.
  void DrawBourgeonGraphics();
  void DrawListTab(int tab);

  // Relit les listes d'adaptateurs et de modes auprès du client. Coûteux —
  // l'énumération DX9 crée un `IDirect3D9Ex` — donc AU TICK et sur évènement
  // (ouverture de l'onglet, changement d'API ou d'adaptateur), jamais par frame.
  void RefreshGraphicsLists();

  bool open_ = false;
  // Dernière transition de carte vue par `OnTick` (cf. Bourgeon::MapLoadEpoch).
  // Un écart signifie qu'un warp / @load a eu lieu : l'écran se referme avec le
  // HUD natif, comme le fait la fenêtre du client.
  uint32_t map_epoch_ = 0;
  bool need_pos_ = false;
  bool show_panel_ = true;

  // Grâce sur la pile Échap : la frappe qui ouvre ne doit pas refermer aussitôt.
  // Même piège que le menu Échap, docs/game_option_re.md §5.6 point 10.
  int esc_grace_frames_ = 0;

  // ⛔ Plus aucun drapeau de routage vers la native : nous ne la fabriquons plus
  // JAMAIS. Elle n'apparaît que si le joueur a remis le menu Échap du client, et
  // le hook la masque puis `OnTick` la détruit, sans exception.

  // Écritures différées pour la même raison : `SetOption` traverse le handler
  // natif de l'option, qui peut recréer des fenêtres et repeindre le monde.
  struct PendingWrite {
    bool valid = false;
    int  id = 0;
    bool on = false;
    bool exec = false;  // ligne de type EXE : exécuter au lieu d'écrire
    // Option que le client ne sait PAS écrire par son id, faute de clé dans sa
    // table de drapeaux : on passe alors par le nom de sa commande de chat, seul
    // chemin qui insère (cf. gamesettings::SetOnByCommand). Littéral statique,
    // jamais alloué — sa durée de vie dépasse celle de la structure.
    const char* slash = nullptr;
  };
  // 🔴 UNE FILE, et un affichage OPTIMISTE par-dessus.
  //
  // Les écritures sont différées au tick (elles traversent un handler natif), et
  // l'affichage relit l'état à chaque frame : entre le clic et le tick, la case
  // se redessinait donc dans son ANCIEN état puis basculait — un clignotement,
  // exactement celui qu'avait le Battle Mode de la table des raccourcis.
  // `PendingValue` fait afficher ce que le joueur vient de demander tant que
  // l'écriture n'a pas eu lieu.
  //
  // Une FILE et non une seule case : deux clics rapprochés tiennent dans le même
  // tick, et la seconde demande écrasait la première sans que rien ne le dise.
  std::vector<PendingWrite> pending_writes_;

  // La valeur à AFFICHER pour cette option : celle en attente s'il y en a une,
  // sinon celle du client. La dernière demande gagne — c'est le dernier clic.
  bool PendingValue(int id, bool actual) const;

  bool pending_reset_ = false;
  bool confirm_reset_ = false;

  // Changement de skin demandé, appliqué au TICK. Il ne rejoint pas la file
  // ci-dessus : ce n'est pas une bascule d'option, et surtout il fait bien plus
  // qu'écrire un drapeau — le client PURGE toutes ses textures .bmp, ce qui tue
  // aussi celles que nos propres caches gardent.
  //
  // 🔴 Différé pour DEUX raisons cumulées, dont chacune suffirait : c'est une
  // commande native (freeze muet si elle tombe dans une frame ImGui), et elle
  // relâche des textures que la frame en cours est en train de dessiner
  // (feedback_texture_release_defer_frame).
  static constexpr int kNoPendingSkin = -2;  // ≠ kSkinDefault, qui vaut -1
  int pending_skin_ = kNoPendingSkin;

  // ── Onglet Graphismes ──────────────────────────────────────────────────────
  //
  // 🔴 SES QUATRE RÉGLAGES STRUCTURELS SONT UN BROUILLON. Système de rendu,
  // adaptateur, résolution et plein écran ne s'appliquent PAS à chaud : le client
  // les écrit puis SE RELANCE (docs §3.10). Les garder localement jusqu'à ce que
  // le joueur accepte la relance est la seule façon honnête de les présenter —
  // écrire au clic donnerait une fenêtre dont les valeurs mentent jusqu'au
  // prochain démarrage.
  //
  // Les trois autres — détail des sprites, détail des textures, filtrage — ne
  // sont PAS ici : ils s'appliquent au clic et se relisent chez le client, comme
  // le reste du panneau.
  struct GraphicsDraft {
    int  system = 0;
    int  adapter = 0;
    int  mode = -1;  // index dans `modes_`, -1 tant que rien ne correspond
    bool fullscreen = false;
  };
  GraphicsDraft draft_;

  // Listes rendues par le CLIENT, jamais reconstruites de notre côté : une
  // énumération Direct3D à nous pourrait proposer un mode qu'il refusera au
  // démarrage, et l'échec serait alors hors de portée du joueur.
  // Bornes de sécurité, pas des vérités du client : elles ne servent qu'à ne pas
  // laisser une énumération inattendue dicter la taille d'un tampon.
  static constexpr int kMaxAdapters = 16;
  static constexpr int kMaxModes    = 256;

  std::vector<gamesettings::graphics::Adapter> adapters_;
  std::vector<gamesettings::graphics::Mode>    modes_;

  // Le libellé prêt à afficher de chaque adaptateur, DANS LE MÊME ORDRE que
  // `adapters_`. Composé à l'énumération : chacun coûte deux appels Windows sur
  // la sortie d'affichage, et le combo se redessine à chaque frame.
  std::vector<std::string> adapter_labels_;

  // 🔴 Les trois réglages « effet immédiat » sont eux aussi DIFFÉRÉS AU TICK, et
  // pour la même raison que le skin : les appliquer recharge des textures du
  // client, ce qui n'a rien à faire au milieu d'une frame ImGui
  // (feedback_no_native_cmd_during_imgui_frame). L'affichage montre la demande en
  // attendant, sinon la case se redessinerait dans son ancien état jusqu'au tick.
  // −1 = rien en attente.
  struct PendingHotGraphics {
    int sprite    = -1;
    int texture   = -1;
    int trilinear = -1;  // 0 ou 1
  };
  PendingHotGraphics pending_hot_;

  bool graphics_ready_   = false;  // les listes ont été chargées au moins une fois
  bool pending_graphics_refresh_ = false;
  bool confirm_restart_  = false;
  bool pending_restart_  = false;
  bool pending_shutdown_ = false;  // « Enregistrer et quitter » : l'arrêt suit l'écriture

  // Un réglage structurel a été enregistré pendant cette ouverture du panneau.
  // Sert à afficher l'accusé de réception : sans lui, le bouton se grise et rien
  // ne dit au joueur que quelque chose a été retenu — puisque, justement, rien
  // ne change à l'écran avant le prochain démarrage.
  bool saved_structural_ = false;

  // L'adaptateur que le client CHOISIRA au prochain démarrage. −1 tant que les
  // listes n'ont pas été chargées.
  //
  // 🔴 MIS EN CACHE À DESSEIN. `gamesettings::graphics::CurrentAdapterIndex()`
  // énumère les adaptateurs pour comparer comme le client — un device Direct3D
  // par appel. Le relire par frame, dans le test « quelque chose a-t-il
  // changé ? », coûterait cela soixante fois par seconde.
  int current_adapter_ = -1;

  // Avertissement « DirectX 7 » à l'instant du CHOIX, pas après la relance : en
  // DX7 le moteur n'a ni shaders ni cible de rendu, et une bonne part de Bourgeon
  // s'éteint. Le texte est celui de `Dx7Warning`, partagé — pas recopié.
  bool confirm_dx7_ = false;
  int  system_before_dx7_ = 0;  // pour revenir en arrière si le joueur refuse

  // Une ligne affichée. On RECOPIE la description du client au lieu de pointer
  // dedans : le vecteur source peut être réalloué, et une infobulle qui survit à
  // la frame lirait alors de la mémoire libérée.
  struct Row {
    gamesettings::Option option;
    bool value = false;  // relu à chaque frame, il est le sujet de l'écran
  };

  // Lignes de la table, relues à l'ouverture et après chaque écriture — jamais
  // par frame : chaque ligne coûte trois conversions de chaîne.
  //
  // 🔴 Relues à l'OUVERTURE et pas seulement au tick : la première frame doit
  // montrer l'état vrai, sinon le panneau s'affiche avec les valeurs de la fois
  // précédente et se corrige sous les yeux du joueur (docs §5.6 point 11).
  std::vector<Row> rows_;
  bool rows_dirty_ = true;

  int  tab_ = kTabAll;
  char filter_[64] = {0};

  // Cache l'option dont l'infobulle est ouverte, pour ne pas rechercher sa
  // description à chaque frame de survol.
  int hovered_id_ = 0;
};
