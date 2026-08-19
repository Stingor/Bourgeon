#pragma once

#include <cstddef>  // size_t (RouteCellPath)
#include <cstdint>
#include <string>
#include <vector>

#include "features/link_gesture.h"  // links::Target (gestes communs d'un lien)
#include "features/plugin.h"
#include "ui/mob_sprite.h"  // ro::MobSpriteRes (aperçu du monstre sélectionné)

// ── NavigationWindow ─────────────────────────────────────────────────────────
//
// Remplacement ImGui de la navigation du client (chercher une carte / un NPC /
// un monstre, puis se faire guider). RE complète : docs/navigation_re.md
// (mémoire project_navigation_re).
//
// ── CE QU'ON REMPLACE, ET CE QU'ON NE REMPLACE PAS ───────────────────────────
// On ne réimplémente RIEN du moteur. `CNavigation` (l'objet statique
// `0x015C3090`) porte le graphe des 1301 cartes, le pathfinder, le suivi et le
// surlignage de l'itinéraire sur la carte du monde. On se contente de le
// PILOTER et de redessiner sa surface — même partage que StorageWindow ou
// RodexWindow.
//
// 🔴 Le moteur est UNIQUE et son état est PARTAGÉ : le terme de recherche, le
// filtre, les résultats et l'itinéraire sont des champs de `CNavigation`, pas
// des copies. C'est ce qui permet de remplacer l'interface sans réécrire une
// ligne de pathfinder — et ce qui explique qu'un itinéraire lancé d'ailleurs
// (lien de chat, `navigateto` scripté) s'affiche ici sans qu'on ait rien fait.
//
// ── LES QUATRE NATIVES SONT ROUTÉES ─────────────────────────────────────────
// La principale (203) et ses satellites — itinéraire (314), sélecteur de trace
// (306), aide (229) — sont masquées à la naissance par le hook `MakeWindow`
// (features/patches/window_pos_tweaks.cc → `HandleNativeCreation`) puis
// DÉTRUITES au tick. Détruire et non masquer : les deux chemins d'ouverture
// font « ferme-si-existe / crée-sinon », donc une native masquée avalerait un
// appui sur deux.
//
// Aucun devoir de naissance ni de mort à rejouer : la fenêtre n'émet AUCUN
// paquet — vérifié, pas un appel à `CRagConnection_SendPacket` dans toute la
// plage de sa classe — contrairement à la rédaction de courrier 0x108.
//
// ⚠ Le remplacement est gouverné par `imgui_enabled_`, membre du groupe
// « Interface moderne ». Éteint, la native revit telle quelle.
//
// ── LES TROIS DÉFAUTS QU'ON CORRIGE ──────────────────────────────────────────
//  1. Le natif n'expose qu'UNE case à cocher pour TROIS options d'itinéraire :
//     son `cmd 213` écrit « service » et « scroll » ensemble en forçant
//     « avion » à 0, si bien que le joueur ne peut pas allumer ce dont il a
//     besoin. On a d'abord exposé les trois séparément — mais c'était encore
//     poser la question au joueur. Sur Moonlight elle n'a qu'une réponse : le
//     Warp Agent (« services Kafra ») est gratuit et partout, et rien d'autre
//     n'atteint ce qui n'est pas relié à pied. Les trois bits sont donc FORCÉS
//     et l'interface n'en parle plus (cf. `kAllRouteOptions`).
//  2. Le natif éclate la tâche sur quatre fenêtres (recherche, itinéraire,
//     choix d'icône, aide) dont deux ne suivent même pas la principale quand on
//     la déplace. Ici : un seul panneau. Son aide, un pavé qui explique la
//     mécanique plutôt que l'usage, devient une infobulle de trois phrases.
//  3. Le natif affiche `"[%d]%s"` — l'identifiant brut — et n'offre ni
//     recherche incrémentale, ni tri, ni historique. Ici : une table, un filtre
//     en pastilles, et la recherche se relance à la frappe (le moteur est
//     LOCAL, aucun aller-retour serveur).
//  4. Le natif ne PERSISTE pas le choix de trace au sol : sa fenêtre 306 écrit
//     dans le moteur et nulle part ailleurs, donc le joueur le repose à chaque
//     session. Ici c'est un réglage (`route_icon_`).
//
// Les résultats sont en outre des LIENS (features/link_gesture.h) : clic droit
// pour le menu, Maj+clic pour partager dans le chat — un monstre y ouvre sa
// fiche, une carte s'y envoie en `<NAVIL>`, un PNJ en `[PNJ: …]`.
//
// 🔴 AUCUN APPEL NATIF DEPUIS LA FRAME IMGUI. Tout ce qui touche `CNavigation`
// (chercher, calculer un itinéraire, l'arrêter) est empilé dans une intention et
// consommé au tick suivant — la règle habituelle de Bourgeon.

class NavigationWindow : public Plugin {
 public:
  NavigationWindow();

  const char* name() const override { return "NavigationWindow"; }

  void OnTick() override;      // consomme les intentions, relit l'état du moteur
  void OnRenderUI() override;  // dessine le panneau

  // Ouvre / referme le panneau. Appelé par l'action `win_navigation` du
  // catalogue de raccourcis (hotkey_actions) et par le panneau Bourgeon.
  void Toggle();
  bool IsOpen() const { return open_; }

  // Ouvrir le panneau SUR une recherche déjà faite. Le sens « fiche de monstre →
  // où le trouver » : la fiche sait QUOI, la navigation sait OÙ.
  // `monsters_only` allume la pastille Monstres, sinon un nom courant comme
  // « Poring » noierait le résultat sous les cartes et les PNJ homonymes.
  // ⚠ La recherche part au tick suivant, jamais ici : le moteur est natif et
  // l'appelant est presque toujours en pleine frame ImGui.
  //
  // 🔴 `map_filter` BORNE le résultat à une carte. Un nom de PNJ n'est pas une
  // identité : c'est un RÔLE, et le serveur en pose trente-huit sous le nom
  // « Warp Agent ». Chercher ce nom sans contexte rend donc trente-huit lignes
  // indiscernables, dont aucune ne répond à la question posée — « celui-là,
  // celui que je viens de voir ». Le lien de chat transporte donc la carte de
  // son auteur, et elle arrive ici.
  //
  // Nul ou vide = pas de bornage. C'est le cas normal d'un monstre (on veut
  // justement tous ses lieux d'apparition) et d'une carte.
  void OpenSearch(const char* term_utf8, bool monsters_only,
                  const char* map_filter = nullptr);

  // ── La native vient de naître : on prend sa place ──────────────────────────
  // Appelée par le hook MakeWindow (features/patches/window_pos_tweaks.cc) pour
  // la fenêtre principale 203 et ses trois satellites (306 icône, 314
  // itinéraire, 229 aide). On masque sur-le-champ — sinon une frame native passe
  // à l'écran — et `OnTick` DÉTRUIT, le natif manipulant encore la fenêtre ici.
  //
  // 🔴 Détruire et non masquer : les deux chemins d'ouverture (bouton du menu
  // d'icônes, raccourci) font « ferme-si-existe / crée-sinon ». Une native
  // masquée existe donc toujours et avalerait un appui sur deux ; détruite, elle
  // n'existe jamais et toute demande repasse par la fabrique.
  //
  // Seule la 203 bascule notre panneau : sa création EST la demande du joueur.
  // Les satellites ne naissent que sur ordre de la 203 — qui n'existe plus —
  // donc leur cas n'est qu'un filet de sécurité.
  void HandleNativeCreation(void* win, int window_id);

  // ── Ce qu'un lien `<NAVIL>` du chat a besoin d'emprunter ───────────────────
  // Un lieu posté dans le chat est un LIEN comme un autre (links::kNavi) : il
  // s'ouvre, se survole et se repartage. Les trois gestes ont besoin d'ici, et
  // seulement d'ici — c'est cette fenêtre qui sait piloter le moteur.

  // Lancer le guidage vers une carte. `(0, 0)` = la carte entière, ce qui est le
  // cas ordinaire d'un lien partagé.
  // 🔴 Des coordonnées ne suffisent pas : le moteur les traduit en cellule et
  // REFUSE tout ce qui n'est pas praticable. On vise donc la carte dès qu'elles
  // ne sont pas strictement positives — même test que `clif_navigateTo` côté
  // serveur. ⚠ La route part au tick suivant, jamais ici.
  void GoTo(const char* map_name, int x, int y);

  // Le plan de la carte, inscrit dans un carré de `side` sans déformation. C'est
  // le bitmap du radar, servi par le cache de textures du jeu — donc rien à
  // libérer, et une carte sans bitmap écrit simplement « (pas de miniature) ».
  // Statique : l'aperçu d'un lien n'a pas besoin de la fenêtre, seulement du nom.
  static void DrawMapThumbnail(const char* map_name, float side);

  // Nom AFFICHÉ d'une carte (« Prontera »), ou son nom interne si le client ne
  // le connaît pas — jamais une chaîne vide, pour qu'un libellé de lien reste
  // toujours lisible.
  static std::string MapLabel(const char* map_name);

  // ── Ce que la MINIMAP a besoin de savoir de l'itinéraire ──────────────────
  //
  // Un itinéraire est une suite de CARTES : sur celle où l'on se trouve, la
  // seule chose utile est « par où sortir ». C'est ce que rend cette méthode —
  // la cellule du point de passage de l'étape courante.
  //
  // ⚠ Elle ne dit rien du chemin À L'INTÉRIEUR de la carte : c'est l'affaire de
  // `RouteCellPath` ci-dessous. Elle reste utile comme REPLI — entre le clic et
  // le premier pas, le moteur n'a pas encore calculé son chemin local, et une
  // direction vaut mieux que rien.
  //
  // Rend false s'il n'y a pas d'itinéraire, si la carte courante n'y figure
  // pas, ou si l'étape n'a pas de point publié (le cas de la destination
  // finale : on y est arrivé, il n'y a plus de sortie à prendre).
  bool RouteExitOnMap(const char* map_name, int* out_x, int* out_y) const;

  // ── LE TRACÉ EXACT, CELLULE PAR CELLULE ───────────────────────────────────
  //
  // 🔴 Correction d'une affirmation antérieure : le chemin à l'intérieur de la
  // carte EXISTE bel et bien sous forme de liste, et le moteur le tient tout
  // prêt. Mesuré dans `CNavigation_BuildCellPath` (0x00B2FC30, docs §10) :
  // l'A★ de déplacement du client tourne sur la .gat de la carte courante et
  // écrit sa sortie dans un `std::vector` de `CNavigation`, chaque élément
  // valant `{ int x; int y; int dir; int t_ms; }` et la suite étant ordonnée
  // DÉPART → ARRIVÉE. C'est CETTE liste que le natif sème au sol en
  // `navi_grid<jeu>_<dir>.tga`, une cellule sur deux.
  //
  // Le natif la projette même déjà en espace minimap (un second vecteur, en
  // pixels d'un carré 128 × 128) — mais quantifiée à ce carré, donc inutilisable
  // au zoom. On lit les CELLULES et on applique notre propre transformation.
  //
  // Rend le nombre de points écrits. La suite est DÉCIMÉE : on ne garde que le
  // premier point, le dernier, et ceux où la direction change — la ligne brisée
  // obtenue passe exactement par les mêmes cellules, en dix à quarante points au
  // lieu de plusieurs centaines. Rend 0 s'il n'y a pas de guidage actif.
  //
  // ⚠ Le moteur ne recalcule ce chemin qu'à l'entrée de carte et quand le joueur
  // s'en écarte assez pour qu'aucune trace ne soit plus visible (± 10 cellules).
  // La ligne peut donc partir d'un point qu'on a quitté : c'est le comportement
  // du natif, pas un défaut de lecture.
  struct PathPoint {
    int x   = 0;
    int y   = 0;
    int dir = 0;  // 0..7, PAIR = orthogonal · IMPAIR = diagonale
  };
  size_t RouteCellPath(PathPoint* out, size_t max) const;

  // Y a-t-il quelque chose à arrêter ? Vrai dès qu'un guidage est en cours,
  // qu'il traverse plusieurs cartes ou qu'il se termine sur celle-ci — c'est ce
  // second cas que le bouton d'arrêt manquait, `route_` étant alors vide.
  bool IsGuidanceActive() const;

  // ── Le CONTENU d'une carte, sans passer par une recherche ─────────────────
  // « Qu'est-ce qu'il y a ici ? » — tous les PNJ et tous les spawns déclarés
  // pour cette carte, groupés par nature. Le moteur ne sait pas répondre à ça :
  // sa recherche compare un TERME à des noms, jamais une carte à son contenu.
  // On lit donc directement les deux vecteurs du nœud (cf. le .cc).
  //
  // ⚠ Remplit nos groupes SANS toucher au vecteur de résultats du moteur : la
  // fenêtre native le partage, et l'écraser lui ferait afficher notre liste.
  // Conséquence assumée : la recherche suivante remplacera cet affichage, comme
  // n'importe quel autre résultat.
  void ShowMapContents(const char* map_name);

  // Reste-t-il des étapes après celle de cette carte ? La minimap le dit au
  // joueur : un marqueur « sortie » et un marqueur « vous êtes arrivé » ne
  // veulent pas dire la même chose.
  bool IsFollowingRoute() const { return following_ || !route_.empty(); }

  // ── Settings PERSISTANTS (bourgeon_settings.yaml) ──────────────────────────
  // « navigation_imgui » : basculé en GROUPE par SetModernInterface, donc défaut
  // OFF comme tous les membres du groupe.
  //
  // 🔴 Il ne dit plus « le panneau est disponible » mais « le panneau REMPLACE
  // la native » : depuis le routage, l'allumer détruit les quatre fenêtres du
  // client. C'est pour ça qu'il a rejoint le groupe — la navigation renvoie vers
  // la fiche de monstre, qui en fait partie, et un lien moderne menant à une
  // fenêtre native serait exactement le mixe qu'on supprime.
  bool imgui_enabled_ = false;

  // « navigation_route_icon » : le jeu de traces semées au sol par le guidage,
  // de 1 à 8. Le client en offre huit et n'en garde AUCUN d'une session à
  // l'autre — sa fenêtre 306 écrit dans le moteur et rien d'autre. C'est donc un
  // réglage chez nous, reposé dans le moteur au tick quand les deux divergent.
  int route_icon_ = 1;

  // ── Les trois options d'itinéraire : TOUJOURS ACTIVES, jamais affichées ───
  // 🔴 Ce ne sont pas des réglages sur Moonlight, et les exposer était une
  // erreur de lecture du serveur :
  //  · « services Kafra » y désigne le **Warp Agent**, gratuit et présent
  //    partout. Le laisser éteint revient à cacher au joueur le seul moyen
  //    d'atteindre tout ce qui n'est pas relié à pied — il ne verrait qu'un
  //    « aucun chemin » incompréhensible ;
  //  · l'avion et les scrolls sont désuets ici (et `navi_scroll_krpri.lub` est
  //    de toute façon VIDE : `{"NULL"}`).
  // Une case qui ne doit jamais être décochée n'est pas une case, c'est un
  // piège. On force donc les trois bits et on n'en parle plus.
  //
  // ⚠ Il faut les REPOSER régulièrement, pas seulement au démarrage : le moteur
  // les remet à zéro et `SearchRoute` les réécrit depuis SON masque à chaque
  // appel, y compris quand l'itinéraire vient d'ailleurs (fenêtre native, lien
  // de chat, `navigateto` scripté).

 private:
  // ── Miroir des résultats du moteur ────────────────────────────────────────
  // Recopié au tick depuis `CNavigation`, jamais lu pendant la frame : les
  // résultats natifs vivent dans des `std::string` MSVC qu'une recherche
  // concurrente peut réallouer sous nos pieds.
  struct Entry {
    int         type = 0;   // 0 et 1 = CARTE · 2 = NPC · 3 = monstre
    int         x    = 0;   // (-1000, -1000) = « la carte entière »
    int         y    = 0;
    // Renseignés seulement pour un NPC ou un monstre, en lisant SON nœud
    // (`CNavi_Object`). Le natif n'en montre presque rien : il se contente d'une
    // tranche de densité (« nombreux », « peu ») pour les monstres.
    int  subtype = 0;      // 101/102 pour un NPC, 300/301 pour un spawn
    int  level   = 0;      // monstre : son niveau
    int  amount  = 0;      // monstre : le nombre d'exemplaires du spawn
    int  stats   = 0;      // monstre : ((ele_lv*20+def_ele)<<16)|(size<<8)|race
    int  sprite_class = 0; // id de CLASSE, ce qu'attend ro::LoadMobSprite
    bool is_mvp  = false;  // subtype == 301 (`mexp != 0` côté serveur)
    bool is_shop = false;  // subtype == 102
    std::string name;       // nom affiché (UTF-8)
    // 🔴 Nom INTERNE de la carte — le seul que le moteur accepte quand on lui
    // demande un itinéraire. Il ne se déduit PAS de `name` : pour un NPC ou un
    // monstre, `name` est celui de la créature, et la carte s'obtient par le
    // slot virtuel +0x20 de l'objet que le moteur range dans le résultat.
    std::string map;
  };
  struct Group {
    std::string name;          // libellé du premier membre
    int         type  = 0;
    std::vector<Entry> entries;
  };

  // Intentions, consommées au tick (jamais exécutées en frame).
  struct GoIntent {
    bool        armed = false;
    std::string map;
    int         type = 0;
    int         x = 0, y = 0;
    int         mob_id = 0;
  };

  void PumpIntents();      // exécute les intentions accumulées
  void RefreshResults();   // recopie le vecteur de groupes du moteur
  void RefreshRoute();     // recopie les étapes de l'itinéraire
  void RunSearch();        // écrit terme + filtre, appelle CNavigation::Search

  // Repose les trois options dans le moteur et relit l'état du guidage.
  void ReadOptions();

  bool open_     = false;
  bool need_pos_ = true;

  // Saisie. `dirty_` arme une recherche, consommée au tick suivant : c'est ce
  // décalage d'une frappe qui suffit à ne pas relancer le moteur au milieu de la
  // frame de rendu.
  char        input_[64] = {0};
  std::string pending_term_;
  bool        dirty_ = false;

  // Filtre d'AFFICHAGE (pas celui du moteur, qui reste en mode « tout ») :
  // -1 = tout · 0 = maps · 2 = NPC · 3 = monstres. ⚠ « tout » ne peut pas valoir
  // 0, sinon il partage sa valeur avec les cartes et les deux pastilles
  // s'allument ensemble.
  int filter_ = -1;

  std::vector<Group> groups_;

  // La carte à laquelle les résultats sont BORNÉS, nom interne, vide si aucun
  // bornage. Appliquée dans `RefreshResults`, donc à chaque relecture du moteur :
  // le bornage survit à un changement de pastille, qui ne relance pas la
  // recherche.
  //
  // ⚠ Elle vit CHEZ NOUS et pas dans le moteur : le natif partage son vecteur de
  // résultats avec nous, et y toucher lui ferait afficher notre liste tronquée.
  std::string search_map_;

  // ── Sélection : la liste à gauche, le détail à droite ─────────────────────
  // Le natif éclate la tâche sur quatre fenêtres et n'offre AUCUN détail : sa
  // liste dit « [12]Yoyo » et rien d'autre. On garde donc la liste sobre — un
  // clic sélectionne, sans bouton par ligne — et tout ce que le `.lub` sait
  // (niveau, densité du spawn, élément, taille, race, MVP, boutique, plan de la
  // carte) va dans le volet de détail, avec les actions.
  //
  // Des INDEX, pas une copie : le détail doit suivre le miroir des résultats
  // plutôt que figer un instantané. Ils sont donc revalidés à chaque frame, et
  // remis à -1 dès qu'une recherche renouvelle les groupes.
  int  sel_group_ = -1;
  int  sel_entry_ = -1;
  // Sprite du monstre sélectionné. La poignée porte son propre `class_id`, donc
  // la recharger à chaque frame avec le même id ne coûte qu'une comparaison —
  // pas besoin de guetter le changement de sélection.
  ro::MobSpriteRes mob_sprite_;

  const Entry* Selection() const;  // nullptr si la sélection n'est plus valide

  // Ce qu'une ligne de résultat DÉSIGNE, dans le vocabulaire commun des liens.
  // La liste devient ainsi une surface de liens comme les autres : clic droit =
  // le menu, Maj+clic = le lien dans le chat. Le clic GAUCHE reste la sélection
  // — c'est le métier du widget, et le volet de détail en dépend.
  links::Target TargetOf(const Entry& entry) const;
  // Cible du clic droit, mise de côté : le popup s'ouvre à la frame suivante et
  // hors de l'arbre (l'identifiant d'un popup se hache avec la pile d'ids).
  links::Target row_menu_;
  bool          row_menu_open_ = false;

  void DrawResultsPane();
  void DrawDetailPane();
  void DrawRoute();
  void DrawRouteIconPicker();
  bool RouteIconButton(int icon, float side, const char* id);
  // Posé par un clic dans le sélecteur : l'écriture est un appel NATIF, donc
  // jamais depuis la frame.
  bool route_icon_armed_ = false;

  // Une étape de l'itinéraire : la carte traversée, et le POINT DE SORTIE à
  // rejoindre dessus. Les coordonnées ne sont pas toujours connues (le moteur
  // n'en publie pas pour la destination finale, qui n'est la source d'aucun
  // lien) — d'où `has_pos`, qui distingue « pas de point » de « (0, 0) ».
  struct RouteStep {
    std::string map;
    int  x = 0;
    int  y = 0;
    bool has_pos = false;
  };
  std::vector<RouteStep> route_;
  bool                   following_ = false;


  GoIntent go_;
  bool     stop_armed_ = false;

  // ── « Aucun chemin » ──────────────────────────────────────────────────────
  // Le natif se contente d'un message système dans le chat, que le joueur ne
  // relie jamais à son clic — et il ne dit surtout pas POURQUOI. Or la cause est
  // presque toujours la même : la carte visée n'est reliée au reste du monde par
  // aucun warp, seulement par un PNJ de transport. Le graphe ne contient ces
  // liaisons-là que sous le type 204, que le pathfinder REFUSE tant que l'option
  // « services Kafra » est éteinte. On garde donc la demande pour pouvoir la
  // rejouer telle quelle avec l'option, au lieu de laisser le joueur conclure
  // que la destination n'existe pas.
  GoIntent last_go_;
  bool     no_route_ = false;
  // « Partager » : même intention différée que le reste, puisqu'elle appelle le
  // natif — lequel déplie la barre de chat et écrit dedans.
  GoIntent share_;

  // Le graphe est-il chargé ? (vecteur de cartes non vide). Sans lui, chercher
  // ne rendrait rien et l'écran serait inexplicablement vide.
  bool  graph_ready_ = false;
  int   map_count_   = 0;
};
