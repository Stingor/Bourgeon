#pragma once

// ── links:: — LES GESTES D'UN LIEN, UNE SEULE FOIS ───────────────────────────
//
// Un « lien » est une RÉFÉRENCE : le nom d'un objet dans une ligne de chat, un
// nom de monstre dans une table de drops, une adresse web. Ce n'est PAS une
// cellule d'inventaire — là, le clic gauche a déjà un métier (sélectionner,
// utiliser, glisser) et rien de ce qui suit ne s'y applique.
//
// 🔴 LA CONVENTION, valable partout où l'on affiche un lien :
//
//   clic GAUCHE  → la description en jeu (objet : la fenêtre de description ;
//                  monstre : sa fiche ; adresse : le navigateur)
//   clic DROIT   → le menu contextuel, le même partout
//   MAJ + clic   → le lien dans la barre de chat
//
// Elle vivait en trois exemplaires — la chatbox (log), la chatbox (barre de
// saisie), la table « qui le drop » — et ils avaient déjà divergé : le clic droit
// ouvrait la fiche ici et un menu là, le Maj+clic n'existait qu'à un endroit. Un
// quatrième appelant aurait recopié l'un des trois. D'où ce module : les surfaces
// décrivent CE QU'ELLES MONTRENT (un `Target`), pas ce que ça fait.
//
// ⚠ CE QUI DOIT RESTER DIFFÉRÉ. Ouvrir une description passe par le natif, ce qui
// est proscrit pendant une frame ImGui (modale bloquante qui relance le rendu) :
// `OpenDescription` ARME (itemcell::DeferDesc*), et Bourgeon::OnProcessInput joue.
// Idem pour les commandes du menu, armées via ChatWindow::QueueCommand.

#include <cstdint>
#include <string>

#include "features/item_cell.h"  // itemcell::ChatLink
#include "imgui.h"               // ImVec4 (la couleur d'un lien, cf. `LabelOpts`)

namespace links {

// L'étiquette de rang d'un monstre, telle qu'elle ouvre un lien « <[MVP] Nom> ».
// Mêmes valeurs que la table des drops de la fenêtre de description (`boss` :
// 2 = MVP, 1 = mini-boss).
//
// 🔴 Son commentaire d'origine disait déjà « pour qu'un même monstre ne change
// pas d'étiquette selon l'endroit d'où le lien a été posé » — et il existait en
// DEUX exemplaires, un par poseur de lien. L'intention était écrite, la garantie
// non.
inline const char* MobRankTag(int rank) {
  if (rank == 2) return "[MVP]";
  if (rank == 1) return "[Boss]";
  return "[Mob]";
}

// Ce qu'un lien DÉSIGNE. Copiable et conservable : le menu contextuel s'ouvre à
// la frame suivante, l'appelant doit donc garder sa cible sous la main.
struct Target {
  // kRecipe — la RECETTE d'un objet, pas l'objet. Le geste gauche n'ouvre donc
  // pas sa description mais l'Atlas, et l'aperçu au survol montre le métier et
  // les matériaux plutôt que les stats. C'est un genre à part parce que c'est une
  // INTENTION différente : « comment on fabrique ça » n'est pas « c'est quoi ».
  // kSetting — une DESTINATION du panneau de réglages Moonlight : un en-tête
  // (« Graphismes ») ou une section de la nav (« Objet obtenu »). Le geste gauche
  // n'ouvre donc pas une description mais le panneau, déjà déplié au bon endroit :
  // c'est ce qu'on veut dire quand on aide quelqu'un, et « le panneau Moonlight,
  // Interface de jeu, huitième entrée » ne marche jamais à la voix.
  // kStyle — le STYLE d'un joueur : couleurs de corps, palette de cheveux,
  // coiffure. Le geste gauche en montre un APERÇU (un pantin le portant), le
  // menu propose de l'essayer ou d'en copier le code.
  // kNavi — un LIEU du monde, tel qu'un `<NAVIL>` de chat le transporte. Le
  // geste gauche n'ouvre aucune description : il LANCE LE GUIDAGE, ce qui est la
  // seule chose qu'on veuille faire d'un lieu que quelqu'un vient de partager.
  //
  // kNaviSearch — une RECHERCHE dans la navigation : « [Carte: Prontera] »,
  // « [PNJ: Kari] », « [Monstre: Poring] ». 🔴 Ce n'est PAS un doublon de kNavi,
  // et la différence est celle entre montrer un point et montrer une question :
  //  · kNavi désigne un endroit PRÉCIS et y envoie tout de suite. Sa balise est
  //    celle du client, donc tout le monde la lit — mais elle ne sait dire qu'un
  //    couple carte + coordonnées ;
  //  · kNaviSearch désigne ce qu'on CHERCHE, et ouvre le panneau dessus. C'est
  //    le seul moyen de partager un PNJ ou un monstre — ni l'un ni l'autre n'a
  //    de position unique, et aucune balise native ne les transporte.
  enum Kind : uint8_t {
    kNone = 0, kItem, kMob, kUrl, kPlayer, kRecipe, kSetting, kStyle, kNavi,
    kNaviSearch
  };
  uint8_t kind = kNone;

  // kItem — la balise RELUE, pas seulement l'id : elle porte le refine, les
  // cartes, le grade, le forgeron. Ouvrir par l'id ne montrerait que l'item de
  // base, c'est-à-dire justement pas celui qu'on a cliqué.
  itemcell::ChatLink item;

  // kMob — le nom voyage avec, parce que le client ne sait pas nommer un
  // monstre (ni mob_db ni le paquet de sa fiche ne le lui donnent).
  uint32_t mob_id = 0;
  uint8_t  mob_rank = 0;  // 0 = normal, 1 = boss, 2 = MVP
  std::string mob_name;   // UTF-8
  // 🔴 `mob_id` est-il une classe de SPRITE plutôt qu'un id de mob_db ? C'est le
  // cas de tout ce qui vient des données de navigation (`write_spawn` écrit
  // `vd.look[LOOK_BASE]`) et du skill Sense. Seul le SERVEUR sait faire la
  // correspondance inverse — le client n'a pas mob_db — d'où ce drapeau, que la
  // fiche et l'aperçu relaient dans leur demande.
  //
  // ⚠ Les deux coïncident pour l'écrasante majorité des monstres, et diffèrent
  // pour ceux qui empruntent l'apparence d'un autre. Les actions qui exigent un
  // VRAI id (la page du bestiaire, `@whodrops`, un lien `<MOBL>` posté) restent
  // donc offertes, mais elles désignent alors le monstre DE L'APPARENCE. Le
  // corriger demanderait un aller-retour serveur pour un cas marginal.
  bool mob_by_view = false;

  std::string url;    // kUrl

  // kPlayer — le pseudo d'un joueur, tel qu'une ligne de chat le porte. C'est
  // TOUT ce dont on dispose : pas d'AID, donc aucune des actions du menu
  // contextuel d'entité (qui résout sa cible par l'acteur à l'écran) n'est
  // utilisable ici. Celles du menu ci-dessous prennent toutes un nom.
  std::string player_name;  // UTF-8

  // kSetting — la CLÉ de la destination (« item_toast », « graphics »). C'est
  // aussi ce qui voyage sur le fil : un nom stable désigne la destination, là où
  // un numéro ne décrirait que l'ordre d'UNE version de Bourgeon. En-têtes et
  // sections partagent le même espace de clés (cf. iface::DestLabel).
  std::string setting_key;

  // kNavi — le nom INTERNE de la carte (« pay_fild10 »), le seul que le moteur
  // de navigation accepte ; le nom affiché est reconstruit LOCALEMENT pour le
  // libellé, chacun devant lire le lieu dans sa propre langue. `(0, 0)` = la
  // carte entière, le cas ordinaire d'un lieu partagé.
  std::string navi_map;
  int         navi_x = 0;
  int         navi_y = 0;

  // kNaviSearch — ce qu'on cherche, et dans quelle famille. Le TERME est le nom
  // AFFICHÉ (« Prontera », « Kari ») : c'est lui que le moteur compare, le nom
  // interne d'une carte n'y trouverait rien.
  std::string navi_term;
  uint8_t     navi_kind = 0;  // cf. links::NaviKind
  //
  // 🔴 Le CONTEXTE de la recherche : la carte où l'auteur du lien a vu ce qu'il
  // partage. Il réutilise `navi_map` ci-dessus — même nature, même règle : nom
  // INTERNE, jamais le nom affiché.
  //
  // Sans lui, « Linker ce NPC » sur un Warp Agent renvoyait les TRENTE-HUIT
  // Warp Agent du serveur, et le lecteur devait deviner lequel était le bon : un
  // nom de PNJ n'est pas une identité, c'est un RÔLE, dupliqué partout.
  //
  // Vide = sans contexte, la recherche porte sur tout le monde. C'est le cas
  // normal d'un MONSTRE (on veut justement tous ses lieux d'apparition) et d'une
  // CARTE (elle est déjà son propre lieu).

  // kStyle — le CODE en entier, et le pseudo de son auteur.
  //
  // 🔴 Le code voyage, il ne se résout pas. Un style n'existe nulle part une
  // fois son porteur hors de vue : le désigner par un pseudo ferait un lien qui
  // meurt quand la personne change de carte.
  std::string style_code;
  std::string style_owner;  // UTF-8

  std::string label;  // ce que le menu affiche en tête (UTF-8)

  bool valid() const { return kind != kNone; }
};

Target FromItem(const itemcell::ChatLink& link, const char* label_utf8);
// Pour les listes qui n'ont qu'un id (membres d'un combo, cartes serties…) :
// l'objet de BASE, sans refine ni cartes — il n'y a rien d'autre à en dire.
Target FromItemId(uint32_t item_id, const char* label_utf8);
// La RECETTE d'un objet. `label_utf8` est le nom NU du produit (« Acid Bottle ») :
// la décoration « [Recette: …] » appartient à la surface qui l'affiche, pas à la
// cible. Rend une cible vide si l'objet n'a pas de recette — un lien qui ne mène
// à rien vaut moins que pas de lien.
Target FromRecipe(uint32_t item_id, const char* label_utf8);
Target FromMob(uint32_t mob_id, int rank, const char* name_utf8);
// Le même monstre, désigné par sa classe de SPRITE. C'est la seule identité que
// portent les données de navigation et le skill Sense ; le serveur fait la
// correspondance inverse. Voir `Target::mob_by_view` pour ce que ça implique.
Target FromMobView(uint32_t view_class, int rank, const char* name_utf8);
Target FromUrl(const char* url);
// Le pseudo d'un joueur (UTF-8). ⚠ Le clic GAUCHE n'ouvre pas de description —
// un joueur n'en a pas — mais PRÉPARE le chuchotement : le pseudo dans la box
// destinataire de la barre de chat, le clavier dans la saisie. La fenêtre 1:1,
// elle, reste au menu.
Target FromPlayer(const char* name_utf8);

// Le STYLE d'un joueur, tel qu'un lien de chat le porte. Rend une cible vide si
// le code est illisible ou d'une version périmée — même règle qu'une recette
// absente : un lien qui n'ouvrirait rien vaut moins que pas de lien.
Target FromStyle(const char* code, const char* owner_utf8);

// Un LIEU, par le nom INTERNE de sa carte. `(0, 0)` = la carte entière. Le
// libellé est composé ici, dans la langue de CELUI QUI LIT : « <Prontera> », ou
// « <Prontera 150,150> » si la position est précise. Cible vide sans nom de
// carte — le reste est validé par le moteur au moment de partir.
Target FromNavi(const char* map_name, int x, int y);

// La famille d'une recherche de navigation. Elle décide du libellé, de la
// pastille de filtre allumée à l'ouverture, et de ce que montre le survol.
enum class NaviKind : uint8_t { kMap = 0, kNpc, kMob };

// Une RECHERCHE de navigation, par le nom AFFICHÉ de ce qu'on cherche. Le
// libellé est composé ici et dans la langue du LECTEUR — « [Carte: Prontera] »,
// « [PNJ: Kari] », « [Monstre: Poring] ». Cible vide sans terme.
//
// ⚠ Le terme voyage tel quel, y compris ses espaces : c'est un nom de créature
// ou de lieu, pas un identifiant. Seuls les chevrons sont refusés — ils
// couperaient la balise en deux à la relecture.
// `map_utf8` est le CONTEXTE (nom interne de carte), pour les PNJ dont le nom
// est porté par plusieurs exemplaires éparpillés. Nul ou vide = sans contexte.
Target FromNaviSearch(NaviKind kind, const char* term_utf8,
                      const char* map_utf8 = nullptr);

// Le libellé VISIBLE d'une recherche, composé localement (même règle que
// `SettingLabel` : un « [Carte: ] » anglophone n'impose rien à son lecteur).
// Avec un contexte de carte, il le NOMME : « [PNJ: Warp Agent (Gonryun)] ».
// C'est ce qui distingue deux liens homonymes dans le fil du chat.
std::string NaviSearchLabel(NaviKind kind, const char* term_utf8,
                            const char* map_utf8 = nullptr);

// Le libellé VISIBLE d'un lien de réglage : « [Réglage: Objet obtenu] ». Composé
// LOCALEMENT, jamais transmis tout fait — chacun le lit dans SA langue, et le
// « [Réglage: ] » d'un expéditeur anglophone n'impose rien à son lecteur. Chaîne
// vide si la destination est inconnue (ou indisponible) dans cette version.
std::string SettingLabel(const char* key);

// Une destination du panneau de réglages, par sa CLÉ — la seule forme qui voyage.
// Cible VIDE si cette version ne la connaît pas, ou si elle ne s'y affiche pas
// (un en-tête réservé au staff chez un joueur ordinaire) : c'est ce qui arrive à
// un lien reçu d'un client plus récent, et un texte inerte y vaut mieux qu'un lien
// qui ouvrirait le réglage d'à côté — ou rien du tout.
Target FromSetting(const char* key);

// ── Le Maj + clic, pour les surfaces qui ne passent pas par `Gestures` ───────
//
// Un en-tête repliable, un onglet, un bouton : ces widgets ont déjà un métier au
// clic gauche, et le lien ne s'ajoute que sur le geste MODIFIÉ. Ils ne peuvent
// donc pas appeler `Gestures`, qui gouverne les trois boutons — mais ils ne
// doivent pas non plus recopier la lecture du geste, qui est tout sauf évidente.
//
// 🔴 `ImGui::IsItemHovered()` EST INUTILISABLE ICI, et deux corrections
// successives par drapeaux n'y ont rien changé. Poser un lien donne le focus à la
// saisie du chat, et cet état la fait mentir de plusieurs façons à la fois : la
// saisie devient l'`ActiveId`, le focus venant de `SetKeyboardFocusHere` l'item
// actif est d'origine CLAVIER donc une branche PRIORITAIRE exige alors que l'item
// ait le focus nav, `g.HoveredWindow` est remis à NULL quand le clic initial
// n'appartient pas à une fenêtre… Chaque garde neutralisée en découvrait une
// autre, et le geste continuait de mourir dès la barre focalisée.
//
// On lit donc CE QU'ON VOIT : le rectangle du dernier item et le bouton BRUT de
// l'IO, plus la seule garde qui ne ment pas — « une AUTRE fenêtre n'est pas
// par-dessus », sans quoi on poserait un lien à travers la chatbox.
//
// ⚠ RÉSERVÉ À UN GESTE MODIFIÉ, qu'aucun widget ne réclame. Un geste ORDINAIRE
// doit rester au widget : les gardes d'ImGui sont ce qui empêche un slider d'en
// piloter un autre au passage.
bool ShiftClickedLastItem();

// Le SURVOL du dernier item, pour l'infobulle qui ANNONCE le geste. Elle, peut se
// contenter de l'`IsItemHovered` d'ImGui — c'est de la décoration, et si elle
// s'efface pendant que la saisie a le focus on ne perd qu'une aide, pas un geste.
bool HoveredForLinkTooltip();

// Le geste RECONNU, sans rien jouer. Pour les surfaces qui ont leur propre façon
// d'ouvrir une description : les cartes et les membres de combo d'une fenêtre de
// description REMPLACENT l'objet affiché (comme un lien de carte natif) au lieu
// d'ouvrir une seconde fenêtre — c'est leur description à elles, et la convention
// porte sur le GESTE, pas sur la façon de l'honorer.
enum class Gesture { kNone, kDescription, kMenu, kChatLink };
Gesture Hit(const Target& target, bool hovered);

// Les gestes, sur une zone SURVOLÉE. `hovered` est fourni par l'appelant : toutes
// les surfaces ne sont pas des items ImGui (le log du chat teste ses rectangles à
// la main). Pose le curseur « main », joue le clic gauche et le Maj+clic, et
// renvoie **true quand le menu doit s'ouvrir** — l'appelant fait l'OpenPopup
// lui-même, l'identifiant d'un popup se hachant avec la pile d'ids de SA fenêtre.
bool Gestures(const Target& target, bool hovered);

// L'APERÇU au survol, à appeler quand la zone est survolée : pour un objet, la
// description simple (le même tooltip RO que les viewers) ; pour un monstre, une
// fiche compacte — sprite, niveau, race, élément, PV. Il crée son propre popup,
// donc rien à ouvrir ni à fermer autour.
//
// 🔴 Un lien de MONSTRE déclenche une demande au serveur la première fois (la
// fiche n'est pas dans le client). C'est borné : une seule demande par monstre,
// et survoler n'ouvre ni ne change la fiche affichée.
void HoverPreview(const Target& target);

// ── DESSINER un lien ─────────────────────────────────────────────────────────
//
// Tout ce qui précède décrit le COMPORTEMENT d'un lien ; le PEINDRE restait à
// chaque surface, et les sept l'ont fait à l'identique : texte à la couleur des
// liens, soulignement tracé au DrawList sur le rectangle du dernier item,
// curseur « main », aperçu au survol, gestes, drapeau de menu. Une vingtaine de
// lignes recopiées par site, la couleur redéclarée CINQ fois — et le troisième
// exemplaire a été écrit le jour où l'onglet Spawns a eu besoin d'un lien.
//
// 🔴 CE QUI NE SE FACTORISE PAS, et c'est voulu : les surfaces qui composent un
// libellé riche — une icône d'équipement grisée quand la pièce est cassée, une
// ligne de liste qui est déjà un `Selectable`, un fragment peint à la main dans
// le log du chat. Elles gardent leur dessin et n'empruntent que `Decorate` et
// `Hit`, qui sont justement les deux briques qui ne supposent aucun libellé.

// La couleur d'un lien dans une fenêtre Bourgeon.
//
// ⚠ La chatbox garde la SIENNE (`kLinkCol`, l'orange du client) et ce n'est PAS
// un oubli à résorber : son fond est sombre et elle imite le chat natif, où les
// liens ont cette couleur-là. Ce bleu-ci est calibré pour le corps CLAIR d'une
// fenêtre RO. Deux fonds, deux couleurs — les unifier casserait l'un des deux.
ImVec4 LinkColor();

// La décoration d'un lien sur le DERNIER item dessiné : curseur « main » et
// soulignement. Un `BeginGroup`/`EndGroup` compte pour un seul item, ce qui rend
// une icône et son libellé sensibles ensemble — c'est ce qu'on veut, le joueur
// vise l'image en premier.
//
// `hovered` est fourni par l'appelant, comme pour `Gestures` : toutes les
// surfaces ne sont pas des items ImGui.
void Decorate(bool hovered);
void Decorate(bool hovered, const ImVec4& color);

// Ce qui change d'un lien à l'autre. Les valeurs par défaut sont celles des sept
// sites d'origine ; les setters se chaînent pour qu'un appel tienne sur sa ligne
// (`LabelOpts().Icon(id)`) — le C++17 du projet n'a pas les initialiseurs
// désignés qui rendraient l'agrégat aussi lisible.
struct LabelOpts {
  ImVec4   color        = LinkColor();
  uint32_t icon_item_id = 0;      // 0 = pas d'icône
  float    icon_size    = 0.0f;   // 0 = la hauteur d'une ligne de texte
  bool     preview      = true;   // l'aperçu au survol (`HoverPreview`)

  LabelOpts& Color(const ImVec4& c) { color = c; return *this; }
  LabelOpts& Icon(uint32_t item_id, float size = 0.0f) {
    icon_item_id = item_id;
    icon_size    = size;
    return *this;
  }
  LabelOpts& NoPreview() { preview = false; return *this; }
};

// ── Le menu contextuel DIFFÉRÉ ───────────────────────────────────────────────
//
// Un popup s'ouvre et se dessine dans la MÊME pile d'ids ; or on clique un lien
// depuis une table, un arbre ou un child, d'où l'`OpenPopup` reporté hors de
// cette pile. Chaque surface gardait donc sa paire `cible` + `drapeau` et son
// quatuor de lignes — le détour était à redécouvrir à chaque nouveau site.
//
// 🔴 L'ANCRE APPARTIENT À L'APPELANT (un membre de sa fenêtre, ou un statique de
// son fichier), elle n'est PAS un état global du module. Deux surfaces visibles
// en même temps arment chacune la sienne ; avec une ancre partagée, la première
// fenêtre dessinée ouvrirait le menu armé par la seconde — le popup n'appartient
// qu'à la pile d'ids qui l'a ouvert.
class MenuAnchor {
 public:
  void Arm(const Target& t) {
    target_ = t;
    armed_  = true;
  }
  // Renoncer à une ouverture armée. Pour les surfaces dont le CONTENU peut
  // disparaître entre le clic et le dessin — une page de dialogue qu'un paquet
  // remplace, par exemple : le menu s'ouvrirait sur un lien qui n'est plus là.
  void Disarm() { armed_ = false; }
  // À appeler une fois par frame, dans la fenêtre (ou le child) où le popup doit
  // vivre — hors de la table ou de l'arbre qui a armé.
  void Draw(const char* popup_id);

 private:
  Target target_;
  bool   armed_ = false;
};

// Le lien COMPLET : dessine, décore, montre l'aperçu, joue les gestes et arme le
// menu. C'est la forme à préférer — une ligne par lien.
void Label(const Target& target, const char* text_utf8, MenuAnchor& menu,
           const LabelOpts& opts = LabelOpts());

// Dessine et RECONNAÎT le geste sans le jouer : pour les surfaces qui honorent
// la description autrement — celles d'une fenêtre de description REMPLACENT
// l'objet affiché au lieu d'en ouvrir une seconde. Même distinction que
// `Hit` face à `Gestures`, appliquée au libellé.
Gesture LabelHit(const Target& target, const char* text_utf8,
                 const LabelOpts& opts = LabelOpts());

// Le survol du DERNIER lien dessiné, pour les surfaces qui veulent y ajouter
// quelque chose (une infobulle à elles).
//
// 🔴 À lire ici et NON par `ImGui::IsItemHovered()` : quand l'aperçu s'est
// affiché, le « dernier item » d'ImGui est celui de l'INFOBULLE — une fenêtre
// à part, dont rien ne restaure l'état au retour. Le survol lu là serait celui
// de la dernière ligne du tooltip, c'est-à-dire faux à chaque fois qu'il compte.
bool LabelHovered();

// Le menu contextuel. À appeler dans la MÊME fenêtre ImGui que l'OpenPopup, avec
// la cible que l'appelant a mise de côté au clic droit.
void DrawMenu(const char* popup_id, const Target& target);

// Les actions, utilisables seules (un bouton, une entrée de menu à soi).
void OpenDescription(const Target& target);
bool PostToChat(const Target& target);
// Y a-t-il seulement une barre de saisie pour accueillir un lien ? Pour les
// surfaces qui ANNONCENT le Maj+clic (une entrée de nav, un bouton) : promettre
// un geste qui ne peut rien faire est pire que de se taire.
bool CanPostToChat();

// ── L'avertissement avant d'ouvrir une adresse ───────────────────────────────
// Une adresse postée dans le chat vient d'un TIERS, et le texte affiché n'a
// aucun rapport obligé avec la destination. Ouvrir un lien passe donc par une
// confirmation qui montre l'adresse COMPLÈTE — sauf si le joueur a retiré le
// garde-fou dans les réglages de la chatbox, ce qui est son droit.
//
// 🔴 À APPELER UNE FOIS PAR FRAME, hors de toute fenêtre, par la surface qui
// offre des liens d'adresse — aujourd'hui la seule est la chatbox (`Run::kUrl`).
// L'ouverture ImGui est DIFFÉRÉE parce que `OpenUrl` part d'un menu contextuel,
// donc d'une autre pile d'ID : un `OpenPopup` posé là ne trouverait pas la
// modale. Même piège et même remède que `ro::OpenQuantityPrompt`.
void DrawUrlConfirm();

// Le LIBELLÉ d'un lien de recette, « [Recette: <produit>] ». Deux surfaces en
// posent — la chatbox et le dialogue NPC — et chacune le composait. Le format
// vient du catalogue de traduction : deux copies, c'était deux clés à tenir
// d'accord, sans que rien ne le signale si l'une dérivait.
std::string RecipeLinkLabel(const std::string& product_name);

}  // namespace links
