#pragma once

// ── Mise en page personnalisable de l'écran de sélection de personnage ────────
// Les positions des sièges et des blocs d'interface étaient des CONSTANTES du
// namespace anonyme de char_select.cc : une table `g_seats[25]` calée à l'œil sur
// le décor livré, et un registre d'ancres alimenté par un éditeur au clavier dont
// le seul aboutissement était un `LogDiag` à recopier dans le code. Le joueur ne
// pouvait donc ni choisir son décor, ni replacer quoi que ce soit.
//
// Ce module sort ces données du code et leur donne un fichier
// (paths::CharSelectLayoutPath()) : le mode « Personnaliser » les modifie, elles
// sont relues au lancement suivant. char_select.cc n'y voit qu'un état partagé —
// il lit State() là où il lisait ses constantes.
//
// Portée : la MACHINE, pas le compte. Un seul layout vaut pour toutes les
// connexions depuis ce client, à l'image des autres réglages Bourgeon.
//
// Les coordonnées restent NORMALISÉES [0..1] (fraction de l'écran) : le décor est
// dessiné étiré plein cadre, un layout calé en 1920x1080 vaut donc tel quel en
// 1280x720. Rien ici ne dépend de la résolution.

#include <string>
#include <vector>

namespace charsel {

// Nombre de sièges de la scène. FIXE : la pagination des bancs (kHeadSeats /
// CharForSeat dans char_select.cc) repose sur ce découpage, et le décor livré est
// numéroté 1..25. Le joueur replace les sièges, il n'en ajoute pas.
constexpr int kSeatCount = 25;

// Types d'action des sprites de personnage (index dans le .act ; la pose vaut
// `anim*8 + dir`). Le client, lui, manipule des « motions » internes qu'il
// traduit ensuite — ce sont bien les index de FICHIER qui comptent ici.
//
// 0, 1, 2 et 4 sont établis : ce sont ceux qu'utilisent déjà l'avatar de la
// fiche de personnage (kPoses) et la scène du banquet. 6 et 8 suivent la
// convention des outils de la communauté (ActEditor, roBrowser) ; le composeur
// replie de toute façon une action absente sur le nombre réel du fichier
// (`PieceAction`), donc une valeur inexacte donne une pose inattendue, jamais un
// plantage.
constexpr int kAnimStand = 0;  // debout / repos
constexpr int kAnimWalk  = 1;
constexpr int kAnimSit   = 2;  // assis
constexpr int kAnimPick  = 3;  // ramassage (poses penchées, souvent cocasses)
constexpr int kAnimFight = 4;  // garde de combat
constexpr int kAnimAtk   = 5;  // attaque
constexpr int kAnimHurt  = 6;  // touché
constexpr int kAnimFroze = 7;  // gelé / pétrifié
constexpr int kAnimDie   = 8;  // mort
constexpr int kAnimAtk2  = 9;  // seconde attaque
constexpr int kAnimAtk3  = 10; // troisième attaque
// 11 et 12 existent dans les fichiers (13 types en tout) mais ne portent pas de
// nom sûr — proposés tels quels, l'aperçu à l'écran tranche mieux qu'un libellé
// inventé.
constexpr int kAnimExtra1 = 11;
constexpr int kAnimExtra2 = 12;

// Un siège : point où poser les PIEDS du pantin (nx, ny), hauteur d'un
// personnage STANDARD en fraction de la hauteur d'écran (`scale` — la
// perspective du décor veut des personnages plus petits au fond), et la POSE
// qu'il y prend.
//
// ⚠ « Standard » et non « du pantin » : les sprites gardent leurs proportions de
// fichier, ils ne sont plus ajustés à ce cadre. Un personnage naturellement plus
// grand qu'un Novice — monture, grand chapeau — déborde donc vers le haut, et
// c'est voulu : c'est ce qui donne à la scène une échelle commune. Cf.
// `RefDollSpanUnits` dans char_select.cc.
//
// La pose est par SIÈGE et non globale : sur un décor de taverne, les convives
// sont assis et le videur reste debout près de la porte.
struct Seat {
  float nx = 0.5f, ny = 0.5f, scale = 0.115f;
  // Orientation du corps, 0..7 (0 = de face). Molette en mode « Personnaliser ».
  int dir = 0;
  // Type d'action du .act (pose = anim*8 + dir). 2 = assis, cf. kPoses côté UI.
  int anim = kAnimSit;
  // Orientation de la TÊTE : -1/0 = dans l'axe du corps, 1 et 2 = tournée d'un
  // cran d'un côté ou de l'autre. Cf. ro::DollDrawOpts::head_dir.
  int head_dir = -1;
  // Image imposée dans l'action (-1 = laisser le composeur choisir). Une action
  // en compte plusieurs, et beaucoup valent le coup d'œil isolément — c'est le
  // sous-menu « Image » du mode « Personnaliser ».
  int frame = -1;
  // Jouer l'animation du CORPS au lieu de le figer sur sa première image.
  //
  // N'a d'effet qu'en Marche et en Combat : le composeur ne fait défiler le corps
  // que pour ces deux actions — ailleurs, les images sont des poses et des
  // expressions, pas une décoration (les accessoires, eux, vivent toujours).
  bool animate = false;
};

// Point d'ancrage nommé d'un bloc d'interface (titre, barre d'action, pagination,
// barre de sortie). Le nom est STOCKÉ (buffer) et non un pointeur vers un
// littéral : il est relu du fichier, donc il doit survivre à sa ligne de code.
struct AnchorPt {
  char  name[24] = {0};
  float nx = 0.0f, ny = 0.0f;
};

constexpr int kMaxAnchors = 16;

struct Layout {
  Seat        seats[kSeatCount];
  AnchorPt    anchors[kMaxAnchors];
  int         anchor_count = 0;
  std::string background;  // chemin VFS du .bmp de décor (vide = fond d'usine)
  // Places LIBRES (marqueur « + » de création). Les masquer donne une scène
  // qui ne montre que les personnages ; l'opacité permet de les garder
  // discrètes sans les perdre de vue. Défauts = comportement d'origine.
  //
  // ⚠ Le masquage ne vaut que pour l'AFFICHAGE : en mode « Personnaliser » les
  // places restent visibles, sans quoi on ne pourrait plus les placer, et la
  // création reste accessible par le menu contextuel.
  bool  hide_empty_seats = false;
  float empty_seat_alpha = 1.0f;  // 0..1
};

// État courant. Le premier appel lit le fichier ; un fichier absent, illisible ou
// hors bornes retombe sur Factory() (jamais d'écran cassé faute de layout).
Layout& State();

// Le layout d'usine, tel qu'il était figé dans char_select.cc.
const Layout& Factory();

// Ancre `name`, enregistrée au défaut donné si elle n'existe pas encore (c'est le
// site d'appel qui déclare sa position d'usine, comme avant). nullptr si le
// registre est plein.
AnchorPt* AnchorRef(const char* name, float def_nx, float def_ny);

// L'éditeur signale une modification ; l'écriture n'a lieu qu'à SaveIfDirty()
// (sortie du mode « Personnaliser », entrée en jeu, retour au login) — pas à
// chaque pixel de glissement de souris.
void MarkDirty();
bool Dirty();
void SaveIfDirty();
// Écrit MAINTENANT, que l'état soit marqué modifié ou non. Pour les gestes dont
// le joueur attend un effet immédiat sur le disque (enregistrer/supprimer une
// mise en page), et non pour le glissement de souris.
void Save();

// Remise à l'état d'usine. `ResetPlacement` ne touche pas au décor choisi : on
// veut pouvoir repartir d'un placement propre SUR son propre fond ; `ResetAll`
// restaure aussi le décor d'origine.
void ResetPlacement();
void ResetAll();

// ── Mises en page enregistrées ───────────────────────────────────────────────
// Un preset est un layout COMPLET (sièges + ancres + décor) rangé sous un nom,
// dans le même fichier que l'état courant. Sert à garder plusieurs compositions —
// une par décor, typiquement — et à revenir à l'une d'elles sans tout replacer.
//
// Le layout d'ORIGINE n'en fait pas partie : il est dans le code (Factory()) et
// se restaure par ResetAll(). Il ne peut donc être ni écrasé ni supprimé.
struct Preset {
  std::string name;
  Layout      data;
};

constexpr int kMaxPresets = 20;

const std::vector<Preset>& Presets();
// Range l'état courant sous `name` (écrase le preset de même nom) et écrit le
// fichier. false si le nom est vide ou si la limite est atteinte.
bool SavePreset(const char* name);
// Recopie le preset dans l'état courant (décor compris). false si hors bornes.
bool ApplyPreset(int index);
bool DeletePreset(int index);

// ── Galerie de décors ────────────────────────────────────────────────────────
// Deux origines : les fonds LIVRÉS avec le client (dans le GRF, connus par leur
// nom) et ceux que le joueur dépose dans BackgroundDir(). Les deux sont résolus
// par le loader natif via le VFS, qui cherche le disque AVANT les GRF.
struct Background {
  std::string path;   // chemin VFS passé au loader (relatif à data\)
  std::string label;  // libellé affiché
  bool        user = false;  // déposé par le joueur (vs livré avec le client)
};

// Liste courante ; scannée au premier appel.
const std::vector<Background>& Backgrounds();
// Re-scanne le dossier joueur (bouton « Rafraîchir » : le joueur vient d'y
// déposer un fichier sans quitter le jeu).
void RescanBackgrounds();

// Dossier disque où déposer ses décors : <jeu>\data\texture\lobby\. Créé au
// besoin. ⚠ C'est bien data\TEXTURE\lobby\ : le TexMgr natif résout les .bmp
// relativement à data\texture\ (cf. le .cc), pas à data\.
const std::string& BackgroundDir();
// Ouvre ce dossier dans l'explorateur Windows.
void OpenBackgroundFolder();

// Nombre de .bmp posés dans l'emplacement « naturel » mais FAUX (<jeu>\data\
// lobby\), et ce chemin. Sert uniquement à guider le joueur qui s'y est trompé —
// ces fichiers-là sont introuvables pour le client.
int MisplacedBackgroundCount();
const std::string& MisplacedBackgroundDir();

}  // namespace charsel
