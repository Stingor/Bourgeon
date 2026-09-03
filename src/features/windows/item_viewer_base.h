#pragma once

// ── Le socle commun des trois VIEWERS D'OBJETS ───────────────────────────────
//
// L'inventaire, le chariot et l'entrepôt sont trois vues d'une même chose : une
// liste d'`ItemSkillInfo` lue dans la session, rendue en ImGui à la place d'une
// fenêtre native qui ne naît plus, avec un glisser qui sort vers les deux
// autres. Le client lui-même les traite en sœurs — même framework de fenêtre,
// mêmes offsets de rect, même modèle d'item.
//
// Écrites l'une après l'autre en se copiant, elles avaient fini par porter
// VINGT ET UN MEMBRES IDENTIQUES chacune : sur les vingt-deux de `CartViewer`,
// vingt et un étaient ici. Ce n'était plus une ressemblance, c'était une classe
// de base qui n'avait jamais été écrite. Les défauts ont été comparés un à un
// avant la fusion — voir les deux ⚠ plus bas, seuls écarts trouvés.
//
// 🔴 Mais cette classe n'a longtemps porté que l'ÉTAT, alors que les en-têtes
// des deux fenêtres annonçaient la factorisation comme faite : le PIPELINE DE
// RENDU, lui, était resté recopié. 53 % des lignes normalisées du chariot
// étaient une copie de l'inventaire — le décor de la fenêtre, le placement par
// défaut, le snap par palier de case, le dialogue de quantité, le routage du
// glisser, l'aperçu au survol. Le squelette est descendu ici (§ « SQUELETTE de
// rendu »), sa moitié purement graphique dans `ro::grid` (ui/item_grid_chrome).
// Ce qui reste chez chacune est ce qui DIFFÈRE vraiment : sa boucle d'onglets,
// sa boucle de cases, son bandeau du bas et son menu contextuel.
//
// ── Ce qui reste chez chacune, et pourquoi ───────────────────────────────────
//   · `Item`, `kMaxItems`, `items_`, `Extract()` — le POD extrait diffère par
//     fenêtre (le chariot ignore `drag_type_`, l'entrepôt porte prix et méta).
//   · `enum PendAction` — les trois vocabulaires d'actions n'ont RIEN en commun
//     (utiliser/équiper/jeter d'un côté, deux sens de transfert de l'autre).
//     Seul le `pend_action_` qui les transporte est ici, en `int`, comme il
//     l'était déjà dans les trois.
//   · Tout le particulier : sertissage et placement libre pour l'inventaire,
//     colonnes/prix/onglets de coffre pour l'entrepôt.
//
// ⚠ `tabs_vertical_` vaut `true` ici parce que deux des trois le veulent ;
// `StorageWindow` le remet à `false` dans son constructeur, avec la raison.
// C'est le SEUL défaut qui divergeait vraiment.
//
// ⚠ `pend_action_ = 0` convient aux trois parce que leur premier énumérateur
// vaut 0 dans les trois (`kPendUse`, `kPendToBody`, `kPendStoToInv`). Ce n'est
// pas une coïncidence sur laquelle se reposer en silence : chaque classe pose un
// `static_assert` sous son enum pour que la fusion casse à la compilation si
// quelqu'un réordonne.

#include <cstdint>

#include "features/item_cell.h"  // itemcell::ItemRow : la ligne survolée
#include "features/plugin.h"
#include "ui/desc_pending_lock.h"  // ro::DescPendingLock : l'anti-flicker de l'aperçu
#include "ui/viewer_rect.h"        // ro::ViewerRect : le rect écran, capturé au rendu

class ItemViewerBase : public Plugin {
 public:
  // Setting PERSISTANT, jamais touché seul : les trois clés
  // (`inventory_imgui`, `cart_imgui`, `storage_imgui`) sont basculées en GROUPE
  // par SetModernInterface, avec les barres d'action, l'échange et le courrier.
  // Un viewer moderne qui devrait échanger des items par glisser avec une
  // fenêtre native n'aurait aucun sens. Public pour que MoonlightUi le
  // charge/sauve. OPT-IN : le natif reste le défaut.
  bool imgui_enabled_ = false;

  // La session est-elle ouverte ? À interroger par les AUTRES modules au lieu de
  // chercher la fenêtre native : elle ne naît plus en mode ImGui, et un
  // `FindWindow` nul y passerait pour « fermé ».
  bool IsOpen() const { return open_; }

  // Le point est-il au-dessus de CE viewer ? Sert aux deux autres à router un
  // dépôt par glisser. Les trois tests sont nécessaires — le pourquoi du
  // `open_` malgré `valid()` est expliqué dans ui/viewer_rect.h.
  bool PointOverViewer(int mx, int my) const {
    return open_ && imgui_enabled_ &&
           win_rect_.Contains(static_cast<float>(mx), static_cast<float>(my));
  }

  // ── Aller d'un viewer à l'autre, par la barre de titre ────────────────────
  //
  // Les trois fenêtres se répondent : on dépose depuis l'inventaire vers
  // l'entrepôt, on reprend du chariot, on range. Mais les OUVRIR demandait de
  // ressortir la souris vers la barre d'action ou une touche, alors que la
  // fenêtre voisine est ce qu'on regarde. Chacune porte donc un raccourci vers
  // les deux autres, dans sa barre de titre.
  //
  // OPT-IN, par viewer : trois boutons de plus dans un bandeau étroit ne sont pas
  // un cadeau pour qui joue en petite résolution ou n'ouvre jamais son chariot.
  enum class Peer { kInventory, kCart, kStorage };

  // Pose les deux boutons de la fenêtre COURANTE (`self` dit laquelle).
  //
  // 🔴 À appeler EN DERNIER dans la fenêtre, juste avant `EndRoWindow` : ils
  // passent par `ro::TitleBarButton`, qui ne restaure pas le curseur de layout.
  //
  // Ils s'empilent de DROITE à GAUCHE dans l'ordre où on les pose ; l'ordre est
  // choisi ici, pas par l'appelant.
  void DrawPeerButtons(Peer self);

  // ── Ce qu'un AUTRE viewer peut demander à celui-ci ────────────────────────
  //
  // Trois verbes, parce que les trois fenêtres ne s'ouvrent pas de la même
  // façon : l'inventaire et le chariot sont à NOUS — le natif est mort, c'est
  // notre `open_` qui fait foi — alors que l'entrepôt est une SESSION du
  // serveur, qu'il faut lui demander et qu'il peut refuser.
  virtual void PeerOpen()  { open_ = true; show_panel_ = true; need_pos_ = true; }
  virtual void PeerClose() { open_ = false; }
  // Pourquoi ce viewer refuse d'être ouvert ou fermé en ce moment — DÉJÀ
  // TRADUIT, et nul quand il accepte. Le bouton se grise alors et affiche ce
  // motif : un bouton inerte sans explication passe pour cassé.
  //
  // C'est l'entrepôt qui s'en sert, le temps qu'une bascule soit en vol.
  virtual const char* PeerBlockedReason() const { return nullptr; }

  // ── Settings PERSISTANTS communs (section du panneau Moonlight) ────────────
  bool& show_panel()     { return show_panel_; }
  bool& peer_buttons()   { return peer_buttons_; }

  bool& show_filter()    { return show_filter_; }
  bool& desc_tooltip()   { return show_desc_tooltip_; }
  bool& tabs_vertical()  { return tabs_vertical_; }
  int&  cur_tab()        { return cur_tab_; }

 protected:
  // ── La BASCULE, quand le hook MakeWindow voit naître la native ─────────────
  //
  // C'est la DEMANDE du joueur : il a appuyé sur la touche, ou cliqué l'icône.
  // On masque la native avant son premier rendu — pas de scintillement — et
  // c'est `OnTick` qui la détruit ensuite.
  //
  // 🔴 DÉTRUITE, pas masquée : toute bascule du client fait « ferme si elle
  // existe, sinon crée ». Une native vivante avalerait donc un appui sur deux.
  // Et comme elle n'existe jamais du point de vue du client, c'est NOUS qui
  // portons l'état ouvert/fermé — d'où l'inversion sur `open_` ici.
  //
  // L'inventaire et le chariot en portaient chacun leur copie, identiques à la
  // VTABLE PRÈS. C'est le seul paramètre : `uiwnd::kInventoryWndVTable` d'un
  // côté, `uiwnd::kCartWndVTable` de l'autre.
  void HandleNativeToggle(void* win, uintptr_t expected_vtable);

  // ── Le SQUELETTE de rendu, commun aux fenêtres à grille ───────────────────
  //
  // Dans l'ordre où on l'appelle. La moitié purement graphique (mesure du strip
  // d'onglets, champ de filtre, enfant défilant de la grille, pont de l'onglet
  // actif) est dans `ro::grid` : elle ne connaît aucun état de viewer.

  // 1. Ce viewer doit-il être dessiné ce frame ? Non => il n'a plus de rect :
  //    un dépôt lâché sur sa dernière position connue ne doit pas lui être
  //    routé.
  bool ShouldRender();

  // 2. Le décor de la fenêtre. Placement et taille par DÉFAUT de première
  //    ouverture — ils se lisaient sur la fenêtre native, qui ne naît plus —
  //    puis le redimensionnement par palier de case, la puce de barre de titre
  //    qui mène aux réglages de CETTE fenêtre, l'épingle, le X, et le bandeau
  //    rouge qui annonce qu'une composition d'échoppe gèle les transferts.
  struct WindowChrome {
    float spawn_x = 0, spawn_y = 0;    // défaut de PREMIÈRE ouverture seulement
    float spawn_w = 0, spawn_h = 0;
    const char* title = nullptr;       // DÉJÀ traduit, avec son `###id` ImGui
    const char* bullet_tip = nullptr;  // DÉJÀ traduit
    int  iface_section = 0;            // MoonlightUi::kIface* : où la puce mène
    bool lock_size = false;            // setting : plus de redimensionnement
  };
  // True => la fenêtre est ouverte ET dépliée : il reste à la remplir, puis à
  // appeler `EndViewerWindow`.
  // 🔴 False => `EndRoWindow` a DÉJÀ été appelé. Rendre la main IMMÉDIATEMENT.
  bool BeginViewerWindow(const WindowChrome& chrome);

  // 3. La fin de la fenêtre, dans l'ordre qui compte : les raccourcis vers les
  //    deux sœurs, la fermeture, puis l'aperçu de description. `hovered` = la
  //    ligne survolée ce frame, nulle si aucune.
  void EndViewerWindow(Peer self, const itemcell::ItemRow* hovered);

  // 4. L'action en attente, quand elle n'a pas besoin qu'on demande combien :
  //    arme le dialogue si une PILE est en jeu, sinon rend 1 et purge
  //    l'attente. À appeler AVANT `PumpQuantityPrompt`, et les deux peuvent
  //    rendre une quantité dans la même frame — c'était déjà le cas.
  int TakePendingAmount();
  // 5. Le dialogue « combien ? » partagé (ui/qty_prompt), habillé RO, identique
  //    dans les trois fenêtres. À appeler à CHAQUE frame : c'est lui qui décide
  //    de se dessiner. Rend la quantité validée, ou 0. `verb` = le libellé du
  //    bouton, DÉJÀ traduit.
  int PumpQuantityPrompt(const char* verb);

  // 6. Le glisser vient-il d'être RELÂCHÉ ? Tant qu'il court, la position de la
  //    souris est mémorisée dans `drag_mx_`/`drag_my_` — la frame du relâché,
  //    ImGui a déjà oublié le payload, et c'est cette dernière position connue
  //    qui désigne la cible. Rend true UNE fois, et clôt le glisser.
  bool DragReleased(const char* payload_type);
  // 7. Arme `action` sur l'objet qui vient d'être lâché ; le dialogue de
  //    quantité ne s'ouvre que si c'est une PILE.
  //    ⚠ NE PAS confondre avec `InventoryViewer::ArmDragQuantityPrompt`, qui
  //    ouvre le dialogue À TOUS LES COUPS. C'est son contrat — l'échange et le
  //    courrier demandent toujours la quantité, pile ou pas — et les deux
  //    doivent rester distincts.
  void ArmDraggedAction(int action);

  // ── Cycle de vie ──────────────────────────────────────────────────────────
  bool open_ = false;   // session ouverte ce frame ?
  // Valeur d'`imgui_enabled_` au tick précédent : détecte la BASCULE de mode,
  // qui doit ADOPTER une fenêtre déjà ouverte au lieu de la faire disparaître.
  bool prev_imgui_enabled_ = false;
  bool need_pos_ = false;    // poser la position par défaut à la 1re ouverture
  bool show_panel_ = true;   // transitoire : détecte le clic sur le X (ferme la session)

  // Rect écran, capturé au rendu pour que les AUTRES viewers puissent le tester
  // hors de leur propre rendu. Cf. ui/viewer_rect.h.
  ro::ViewerRect win_rect_;

  int item_count_ = 0;   // nb d'items valides dans le `items_` de la dérivée

  // ── Vue ───────────────────────────────────────────────────────────────────
  int  cur_tab_ = 0;                // onglet catégorie sélectionné (0 = Tout)
  bool show_filter_ = true;         // setting : champ de filtre par nom
  // setting (opt-in) : les raccourcis vers les deux autres viewers, en barre de
  // titre. Cf. `DrawPeerButtons`.
  bool peer_buttons_ = false;
  // Description au SURVOL : ouvre la VRAIE fenêtre de description (celle du clic
  // droit) tant que la souris reste sur la case, et la ferme en sortant. OFF =
  // simple tooltip texte (nom + quantité).
  bool show_desc_tooltip_ = false;
  bool tabs_vertical_ = true;       // setting : onglets verticaux (cf. le ⚠ en tête)

  // ── Survol et description ─────────────────────────────────────────────────
  // Case survolée ce frame (0 = aucune) : alimente l'aperçu, dessiné APRÈS la
  // fenêtre, en tooltip. L'INDEX sert à retrouver cartes et options (données
  // d'instance) ; il n'est valable que dans la frame courante, `items_` étant
  // reconstruit à chaque tick.
  uint32_t hover_desc_id_ = 0;
  int      hover_desc_idx_ = -1;
  // Verrou anti-flicker de l'aperçu, armé à la DEMANDE de description.
  ro::DescPendingLock desc_lock_;

  // ── Glisser d'un item hors de cette fenêtre ───────────────────────────────
  bool  drag_active_ = false;
  int   drag_index_ = 0, drag_amount_ = 0;
  float drag_mx_ = 0, drag_my_ = 0;  // dernière position souris pendant le glisser

  // ── Action en attente (posée par un glisser ou un menu, traitée au rendu) ──
  int  pend_id_ = 0;      // 0 = aucune action en attente
  int  pend_index_ = 0;   // index (inventaire / cart / storage) de la source
  int  pend_max_ = 0;     // quantité max (stack) pour le prompt
  bool pend_open_prompt_ = false;  // ouvrir le prompt quantité au prochain rendu
  int  pend_action_ = 0;  // valeur du `PendAction` de la dérivée (cf. le ⚠ en tête)
};
