#pragma once

#include <cstddef>
#include <initializer_list>

// ── ro_imgui : socle du toolkit ImGui « façon RO » ────────────────────────────
// Briques bloquantes du remplacement complet de l'UI native par de l'ImGui :
//   1. Conversion d'encodage CP949 <-> UTF-8. Le client RO parle CP949 sur le
//      fil et dans ses structures ; ImGui veut de l'UTF-8. Toute chaîne qui
//      traverse la frontière (affichage OU saisie) DOIT passer par ici.
//   2. Widgets texte qui font la conversion tout seuls (TextCp949, InputTextCp949)
//      pour qu'aucun site d'appel n'ait à y penser.
//   3. Chargement de la police coréenne (glyphes pré-bakés — compatible DX7+DX9).
//
// Tout est à appeler depuis le thread principal du jeu (comme le reste d'ImGui).

struct ImFont;
struct ImDrawList;

namespace ro {

// Convertit une chaîne CP949 null-terminée en UTF-8. Le résultat pointe vers un
// buffer thread-local rotatif (8 emplacements) : valide jusqu'au 8e appel
// suivant sur le même thread. Suffisant pour l'usage typique
// `TextCp949(a); TextCp949(b);` dans une même frame ; ne pas stocker le pointeur.
const char* Cp949ToUtf8(const char* cp949);

// Convertit de l'UTF-8 vers CP949 dans `out` (taille `out_size`, terminaison
// incluse). Les caractères non-mappables deviennent '?'. Renvoie le nombre
// d'octets écrits (hors '\0'), ou -1 si `out` est trop petit / conversion ratée.
int Utf8ToCp949(const char* utf8, char* out, size_t out_size);

// ── Texte venu du CLIENT, dans SA code-page ─────────────────────────────────
// À préférer aux deux fonctions ci-dessus pour tout ce qui vient du jeu — noms
// d'objets, descriptions, noms de personnages, libellés de la MsgStringTable.
//
// La différence n'est pas cosmétique : le client ne parle pas toujours CP949. Il
// pose sa code-page effective au démarrage d'après son servicetype
// (rag::kClientCodePageAddr : 949 en Corée, 1252 en Europe) et c'est ELLE qu'il
// passe à MultiByteToWideChar avant de dessiner. Coder 949 en dur donne du
// mojibake sur tout ce qui porte un accent en servicetype européen. On lit donc
// la même valeur que lui, et nos fenêtres disent exactement ce que disent les
// siennes.
//
// Cp949ToUtf8 reste juste là où l'encodage est connu par CONSTRUCTION : un
// littéral CP949 écrit dans nos sources (les chemins « 유저인터페이스\… »), un
// fichier dont on sait qu'il est coréen.
//
// Buffer thread-local rotatif, même contrat que Cp949ToUtf8 : à consommer tout
// de suite, jamais à stocker. Jamais nul.
const char* LocalToUtf8(const char* local);

// Le retour : une saisie ImGui (UTF-8) vers la code-page du client, pour ce qui
// PART au natif ou sur le fil (un nom de personnage à la création, par exemple).
// Sans ça, un nom accentué partirait en octets UTF-8 et s'afficherait en
// mojibake partout ailleurs. Mêmes garanties de tampon.
const char* Utf8ToLocal(const char* utf8);

// ── Texte venu DU FIL (serveur), et son retour ───────────────────────────────
// ⚠ Ce n'est PAS la même code-page que LocalToUtf8, et la nuance a déjà coûté
// des accents mangés : `LocalToUtf8` vaut pour ce que le CLIENT a chargé (DB
// d'objets, msgstringtable, noms d'onglets relus de son .lua — la code-page de
// son servicetype, souvent 949), tandis que ce qui arrive PAR LE RÉSEAU — lignes
// de chat, courrier, textes de scripts — est du **1252**, en dur, parce que
// l'encodage du fil est une propriété du SERVEUR et non de la machine qui lit.
// `CP_ACP` décrirait la locale du joueur, qui vaut 949 chez ceux qui l'ont réglée
// en coréen pour leur client RO. Le raisonnement complet est au-dessus de
// kWireCodePage, dans ro_imgui.cc.
//
// ⚠⚠ MISE À JOUR : le fil porte MAINTENANT LES DEUX ENCODAGES. 1252 ne sait pas
// écrire un emoji, donc ce qui vient du relais Discord — ou d'un joueur qui en
// tape un — voyage en UTF-8, pendant que les scripts NPC, les msg_conf et tout
// l'historique déjà écrit restent en 1252.
//
// `WireToUtf8` tranche donc à la LECTURE, sur la validité stricte de l'UTF-8 :
// « é » en 1252 (l'octet 0xE9 seul) n'est pas de l'UTF-8 valide et se décode en
// 1252 ; « é » en UTF-8 (C3 A9) est pris tel quel. Les accents des textes
// existants sont conservés à l'octet près, sans rien changer côté serveur.
//
// Elle retire au passage les sélecteurs de variante (U+FE0F & co) : ImGui ne
// fait pas de shaping et dessinerait leur glyphe, VIDE mais large d'un emoji.
//
// Mêmes garanties de tampon que les fonctions ci-dessus (rotatif, à consommer
// tout de suite). Sens retour pour une saisie ImGui qui repart au serveur.
const char* WireToUtf8(const char* ansi);
const char* Utf8ToWire(const char* utf8);

// La chaîne est-elle de l'UTF-8 valide portant au moins un octet non-ASCII ?
// C'est le prédicat exact sur lequel `WireToUtf8` se décide, exposé parce que
// certains traitements doivent connaître l'encodage AVANT la conversion — la
// neutralisation de l'octet 0xA0 dans le parseur du chat, notamment : c'est un
// NBSP en 1252, mais l'octet de continuation du « à » en UTF-8.
bool IsUtf8(const char* s);

// ── Le retour pour du TEXTE, à ne pas confondre avec `Utf8ToWire` ────────────
// 🔴 Réservé aux PHRASES que le serveur ne fait que relayer (ligne de chat,
// courrier). Un nom de personnage, une cible de chuchotement ou un nom de canal
// sont des IDENTIFIANTS que le serveur compare octet par octet à sa base en
// 1252 : ceux-là passent par `Utf8ToWire`, sans exception, sinon la recherche
// échoue au premier accent.
//
// Règle : 1252 tant que la phrase y rentre ENTIÈREMENT (elle part alors
// exactement comme avant — mêmes octets en base, dans les logs, pour les
// commandes @), UTF-8 seulement si elle contient un caractère hors 1252, donc
// en pratique un emoji. Le lecteur accepte les deux.
const char* Utf8ToWireText(const char* utf8);

// Charge DEUX polices dans l'atlas : la police intégrée d'ImGui (repli) et Malgun
// Gothic (glyphes hangul + latin, présente sur tout Windows 10/11). Puis
// sélectionne l'active selon l'état du toggle (voir SetFontEnabled), sans jamais
// avoir à reconstruire l'atlas ensuite. À appeler UNE fois, après
// ImGui::CreateContext() et avant la première frame. Repli silencieux sur la
// police intégrée si Malgun est absente. Renvoie la police active.
ImFont* LoadKoreanFont(float size_px = 15.0f);

// Active/désactive la police Malgun À CHAUD (bascule io.FontDefault, aucun rebuild
// d'atlas — les deux polices sont déjà bakées). Sûr à appeler même avant
// LoadKoreanFont (l'état est mémorisé et appliqué au chargement). Désactivé =
// retour à la police intégrée d'ImGui.
//
// C'est le cas particulier de SetUiFontFamily(-1) ci-dessous : réactiver ne
// défait PAS un choix de famille. Il portait la vieille clé yaml « malgun_font »,
// qui n'est plus écrite — startup::UiFontFamily() la lit encore en repli, mais
// résout elle-même le booléen en index et n'appelle plus ceci.
void SetFontEnabled(bool enabled);

// ── La police de TOUTE l'interface ───────────────────────────────────────────
// Index dans la table des familles (cf. ChatFamilyLabel) : 0 = Malgun Gothic, le
// défaut ; -1 = police intégrée d'ImGui ; >0 = une famille du système. Bascule à
// chaud, sans rebuild d'atlas — tout est baké au démarrage.
//
// Les variantes FontBold/FontItalic suivent la famille choisie, et la chatbox
// réglée sur « Système » suit elle aussi.
//
// ⚠ Une famille absente du système, ou écartée par le mode « glyphes coréens »
// (cf. KoreanGlyphsWanted), n'est pas bakée : la sélection retombe alors
// silencieusement sur Malgun. Un menu doit donc masquer les familles dont
// ChatFamilyFont rend nullptr, sinon il propose des entrées sans effet.
void SetUiFontFamily(int index);
int  UiFontFamily();

// ── Variantes grasse et italique ─────────────────────────────────────────────
// Bakées dans le MÊME atlas et à la MÊME taille que la police normale, par
// LoadKoreanFont. Rendent nullptr quand le fichier système est absent : dans ce
// cas l'appelant garde sa police courante — un texte non gras vaut mieux qu'un
// texte manquant.
//
// 🔴 Une variante est PLUS LARGE que la normale. La police d'un fragment doit
// donc servir à sa MESURE (`CalcTextSizeA`) autant qu'à son dessin, sinon les
// retours à la ligne tombent à côté.
//
// ⚠ L'italique ne couvre que le LATIN : aucune police coréenne courante n'en
// fournit, celle-ci vient d'Arial. Du texte coréen en italique restera droit.
ImFont* FontBold();
ImFont* FontItalic();

// ── Familles au choix (chatbox ET interface) ─────────────────────────────────
// Toutes bakées au démarrage : basculer ensuite ne coûte rien. L'index 0 est
// « Système », qui ne force rien et laisse la police de base — c'est la seule à
// couvrir le coréen quand le réglage de débogage est actif.
//
// ⚠ Le nom dit « Chat » pour raison historique : la même table sert désormais à
// la police de l'interface (SetUiFontFamily). L'ORDRE EN EST FIGÉ, l'index étant
// persisté des deux côtés — une famille s'ajoute en fin de table.
//
// `ChatFamilyFont` rend nullptr pour l'index 0 sans style : l'appelant garde
// alors sa police courante. Repli en cascade sinon (style absent -> normal de la
// même famille), parce que rester dans la famille en perdant le gras vaut mieux
// que changer de dessin pour le garder.
int         ChatFamilyCount();
const char* ChatFamilyLabel(int index);
ImFont*     ChatFamilyFont(int index, bool bold, bool italic);

// ── Glyphes coréens : réservés au DÉBOGAGE ───────────────────────────────────
// 🔴 Le hangul pesait 97 % de l'atlas (11 172 glyphes sur ~11 500) pour des
// caractères qu'aucun joueur ne voit : le jeu est en français/anglais, et les
// conversions CP949 du code traduisent l'ENCODAGE DU FIL, pas un contenu coréen.
// Il n'est donc plus baké par défaut.
//
// Il le redevient quand le staff active le réglage « korean_glyphs » : les
// chemins des fichiers du jeu (« 유저인터페이스\… ») redeviennent lisibles dans
// la console de logs.
//
// ⚠ EXIGE UN REDÉMARRAGE : l'atlas est construit une seule fois à l'init, et le
// backend DX7 n'a aucun chemin de texture dynamique pour le refaire.
bool KoreanGlyphsWanted();
bool IsFontEnabled();

// ── L'ÉCHELLE de toute l'interface ───────────────────────────────────────────
// Un pourcentage : 100 = la taille d'origine, 200 = le double. Sert aux écrans
// très définis (un 21:9 en 3440×1440, un 4K), où une interface calibrée en
// pixels sur du 1024×768 devient illisible sans que rien ne soit « cassé ».
//
// 🔴 100 % EST LE CHEMIN D'ORIGINE, À L'IDENTIQUE. `Px` rend alors son argument
// (v * 1.0f est exact en IEEE-754, pas d'arrondi qui traîne), `ApplyUiScale`
// n'appelle PAS `ScaleAllSizes` — qui n'est pas neutre à 1.0, il tronque chaque
// champ du style et cumule `_MainScale` — et la police garde son
// `FontScaleMain` d'usine. Un joueur qui ne touche à rien voit exactement ce
// qu'il voyait avant.
//
// ⚠ DX9 SEULEMENT, en pratique. Le backend officiel re-rastérise les glyphes à
// la taille demandée (`ImGuiBackendFlags_RendererHasTextures`) ; notre backend
// DX7 maison ne le déclare pas, donc l'atlas y serait ÉTIRÉ — du texte flou.
// L'échelle rejoint la liste des modules inertes sous le proxy DirectDraw (cf.
// features/systems/dx7_warning.h), et son réglage s'y grise.
//
// Les paliers proposés au joueur sont ENTIERS ou demi-entiers (100/125/150/…) :
// l'art du skin est échantillonné en POINT (pixel-art net), et un facteur
// quelconque y donnerait des bordures d'épaisseur inégale.
// Le CHOIX du joueur — celui qu'on persiste et qu'on affiche au réglage. Il
// reste ce qu'il est même là où l'échelle ne s'applique pas (DX7) : ce que le
// joueur a demandé ne doit pas se perdre parce qu'il a changé de mode de rendu.
void SetUiScalePercent(int percent);
int  UiScalePercent();

// Le facteur EFFECTIVEMENT appliqué (1.0f à 100 %). C'est lui qu'on multiplie —
// et il vaut 1.0 en DirectX 7 quel que soit le choix ci-dessus. Les deux ne
// s'accordent donc pas toujours, et c'est voulu.
float UiScale();

// Une longueur d'ART ou de MARGE, en pixels d'origine, mise à l'échelle. Tout
// ce qui est mesuré sur une pièce du skin (hauteur de barre de titre, cap d'un
// 3-slice, côté d'une case à cocher) passe par ici :
//   const float cap = ro::Px((float)skin::kTitlebarLeft.w);
// 🔴 PAS pour ce qui dérive déjà de la police (`GetFontSize`, `CalcTextSize`) :
// celle-ci suit `FontScaleMain` toute seule, la remultiplier la ferait grossir
// au carré.
float Px(float pixels);

// Applique l'échelle courante au style ImGui (police + toutes les longueurs).
// À appeler UNE fois après `ImGui::CreateContext()` + le thème de couleurs — le
// premier appel photographie le style à 100 %, qui sert ensuite de référence à
// tous les changements. `SetUiScalePercent` s'en charge ensuite tout seul.
//
// 🔴 On repart TOUJOURS de cette photo, jamais du style courant : `ScaleAllSizes`
// tronque à l'entier, si bien qu'enchaîner ×1.5 puis ×(1/1.5) ne rendrait pas
// les valeurs de départ. Sans référence, un aller-retour 100 → 150 → 100
// laisserait l'interface durablement de travers.
void ApplyUiScale();

// Pose l'échelle demandée depuis la dernière frame, s'il y en a une. À appeler
// UNE fois par frame, entre `ImGui::NewFrame()` et le premier `Begin` — la même
// fenêtre de tir que le clamp des fenêtres (Bourgeon::RenderUI). Ne coûte rien
// tant que personne n'a touché au réglage.
//
// 🔴 C'EST LE SEUL MOMENT OÙ LE STYLE PEUT CHANGER. `SetUiScalePercent` se
// contente donc de marquer : appelé depuis un panneau, il tomberait en pleine
// frame, sous les `PushStyleVar` de la fenêtre qui le dessine — dont les `Pop`
// de fin restaureraient ensuite les valeurs de l'ANCIENNE échelle par-dessus la
// nouvelle, laissant un style à moitié converti.
void ApplyPendingUiScale();

// Affiche une chaîne CP949 comme texte ImGui (convertit en UTF-8 d'abord).
void TextCp949(const char* cp949);

// ── Skin « façon RO » ─────────────────────────────────────────────────────────
// Fenêtre ImGui habillée avec les VRAIES pièces du client (barre de titre 3-slice
// titlebar_*, boutons sys_close/sys_mini, corps plein). S'utilise comme Begin/End :
//   if (ro::BeginRoWindow("Titre", &open)) { ... }
//   ro::EndRoWindow();
// EndRoWindow doit TOUJOURS être appelé après BeginRoWindow (même si false).
// Les mécaniques (drag/resize/collapse) restent natives ImGui ; seules la barre de
// titre et la peinture sont à nous. Renvoie ImGui::Begin (false si repliée/masquée).
bool BeginRoWindow(const char* title, bool* p_open = nullptr,
                   int imgui_window_flags = 0);
void EndRoWindow();

// Repli (« minimiser ») autorisé, GLOBALEMENT, pour toutes les fenêtres RO.
// Posé une fois par frame par Bourgeon::RenderUI : vrai dans le monde de jeu,
// FAUX à l'écran de login / char-select. Hors du jeu, aucune fenêtre ne doit
// pouvoir être repliée — la barre de titre y perd son bouton sys_mini (et le
// double-clic ne replie plus), sinon le joueur peut laisser un formulaire de
// connexion réduit à une barre de titre sans moyen évident de le rouvrir.
// Interne : équivaut à forcer ImGuiWindowFlags_NoCollapse sur chaque
// BeginRoWindow, ce qui redéplie aussi une fenêtre restée repliée en jeu.
void SetWindowCollapseAllowed(bool allowed);

// ── Boîte de dialogue MODALE façon RO ──────────────────────────────────────────
// Même habillage que BeginRoWindow (barre de titre 3-slice titlebar_* + corps clair)
// mais rendue via ImGui::BeginPopupModal : ImGui BLOQUE et ASSOMBRIT l'arrière-plan
// tout seul (aucune gestion de modalité côté appelant). Centrée à l'apparition. Pas
// de boutons système (mini/close/resize) : un dialogue se ferme par ses propres
// boutons (ImGui::CloseCurrentPopup). S'utilise comme BeginPopupModal :
//   if (want) ImGui::OpenPopup("Titre");
//   if (ro::BeginRoPopupModal("Titre")) {
//     ... ImGui::Text / InputText / ro::RoButton ...
//     ro::EndRoPopupModal();
//   }
// ⚠ EndRoPopupModal NE DOIT être appelé QUE si BeginRoPopupModal renvoie true (règle
// EndPopup d'ImGui). Le contenu profite auto des couleurs RO (corps clair, texte
// sombre, champ blanc) — utiliser ro::RoButton (art clair).
// (ro_imgui.h n'inclut pas imgui.h -> défaut en littéral : 64 =
// ImGuiWindowFlags_AlwaysAutoResize = 1<<6. Statique dans imgui.h.)
bool BeginRoPopupModal(const char* title, int imgui_window_flags = 64);
void EndRoPopupModal();

// Placement et voile de la PROCHAINE modale RO, à appeler JUSTE AVANT
// BeginRoPopupModal. Sans appel : centrée sur l'écran, avec le voile ImGui qui
// assombrit l'arrière-plan. Avec : coin haut-gauche à (x, y) — à l'apparition
// seulement, la modale reste déplaçable — et voile optionnel (`dim_background` à
// false pour un petit dialogue contextuel, où assombrir tout l'écran pour saisir
// un nombre serait disproportionné). Comme le bullet de titre, la demande est
// CONSOMMÉE par BeginRoPopupModal (pas de fuite sur la modale suivante).
void SetNextRoModalPos(float x, float y, bool dim_background = true);

// ── 3e style de cadre : la CHATBOX ───────────────────────────────────────────
// La chatbox du client n'a ni barre de titre, ni corps clair : c'est un rectangle
// translucide SOMBRE, bordé d'un filet clair, qu'on déplace en le glissant et
// qu'on redimensionne par une poignée en bas à droite. Aucun des deux styles
// ci-dessus ne convient — sur un corps clair, tout ce que le serveur envoie en
// blanc devient invisible, et une barre de titre RO au-dessus d'un chat n'existe
// nulle part dans le client.
//
// Ce que ce style pose, et que l'appelant n'a donc pas à refaire : fond et filet
// aux couleurs demandées, texte CLAIR par défaut, champs de saisie restés CLAIRS
// (les UIEditWnd natifs le sont — penser à pousser un texte sombre autour d'un
// InputText), scrollbars RO, poignée de resize `btn_resize`, échelle de police et
// marges propres à la fenêtre.
//
// ⚠ Les deux couleurs sont des **ImU32** (0xAABBGGRR, ce que rend IM_COL32 et
// ImU32FromPicker) — pas de l'ARGB natif. Cf. ui/color_codec.h : les deux
// encodages prennent le même `uint32_t` et rien dans le type ne les distingue.
struct RoChatSkin {
  unsigned int body_col   = 0x96000000;  // fond du log (défaut ≈ le 0x66000000 natif)
  unsigned int border_col = 0xFFC5C5C5;  // filet du cadre
  float        font_scale = 1.0f;        // taille de police propre au chat
  float        padding    = 3.0f;        // marge intérieure du cadre
  float        line_gap   = 2.0f;        // interligne (densité de l'historique)
  // Arrondi des coins du cadre. Le chat natif est carré ; on adoucit par défaut.
  // ⚠ Ne concerne QUE le cadre : le fond du log est une child window laissée
  // carrée, sinon ses coins découvriraient le fond de la fenêtre sur 6 px.
  float        rounding   = 6.0f;
  // Bornes du redimensionnement par les bords. `max_*` à 0 = « 80 % de l'écran ».
  float        min_w = 400.0f, min_h = 200.0f;
  float        max_w = 0.0f,   max_h = 0.0f;
  // Quantification de la HAUTEUR : `snap_base + N × snap_step`. 🔴 Si l'appelant
  // impose la même règle par SetNextWindowSizeConstraints, il DOIT la donner ici
  // aussi : sinon le redimensionnement pose une hauteur qu'ImGui corrige à la
  // frame suivante, et le bord opposé — celui qu'on ne touche pas — se met à
  // dériver. `snap_step` à 0 = aucune quantification.
  float        snap_step = 0.0f;
  float        snap_base = 0.0f;
  // Verrouillage. `movable` pose `NoMove` ; `resizable` retire les quatre bandes
  // de redimensionnement. Les deux sont séparés parce qu'ils se verrouillent
  // parfois séparément — une fenêtre figée en taille peut rester déplaçable.
  bool         movable   = true;
  bool         resizable = true;
};
// Pas de `p_open` : la chatbox du client n'a pas de bouton de fermeture (elle se
// replie, ou on la coupe dans les options). Le module qui l'appelle garde son
// propre interrupteur.
bool BeginRoChatWindow(const char* id, const RoChatSkin& skin,
                       int imgui_window_flags = 0);
void EndRoChatWindow();
// Un bord est-il en train d'être tiré ? 🔴 L'appelant s'en sert pour n'imposer sa
// quantification de hauteur QUE pendant le geste. Appliquée en permanence, elle
// redimensionnerait la fenêtre à chaque changement d'onglet dès que la taille de
// police devient propre à chaque onglet — et c'est elle qui fait vibrer le bas de
// la fenêtre, les métriques étant mesurées avec une frame de retard.
bool RoChatWindowIsResizing();

// ── Bullet de la barre de titre, cliquable ────────────────────────────────────
// Le petit blit sys_base à gauche du titre est décoratif par défaut (comme dans le
// client). Appeler ceci JUSTE AVANT BeginRoWindow le rend interactif pour cette
// fenêtre-là : survol = art « on » + curseur main + `tooltip`, et le clic est
// récupéré par TitleBulletClicked() juste APRÈS BeginRoWindow (à appeler que Begin
// ait renvoyé true ou non, la barre de titre existe même repliée) :
//   ro::SetNextWindowTitleBullet("Options du storage");
//   const bool begun = ro::BeginRoWindow(...);
//   if (ro::TitleBulletClicked()) /* ouvrir la config de CETTE fenêtre */;
// TitleBulletClicked() ne vaut que pour la fenêtre qui vient d'être ouverte : la
// demande est consommée par BeginRoWindow, elle ne fuit pas sur la suivante.
void SetNextWindowTitleBullet(const char* tooltip = nullptr);
bool TitleBulletClicked();

// ── Épingle de la barre de titre (« Échap ne me ferme pas ») ─────────────────
// Ajoute un 3e bouton système, JUSTE À GAUCHE de la croix : [mini][pin][close].
// Épinglée, la fenêtre sort de la pile Échap — elle ne se ferme plus sur Échap,
// et n'avale plus la touche non plus (le jeu la reçoit, cf. SkipNextEscapeWindow).
// La croix, elle, ferme toujours : l'épingle protège du geste RÉFLEXE, pas de la
// fermeture voulue.
//
// Opt-in, à appeler JUSTE AVANT BeginRoWindow ; la demande est CONSOMMÉE par lui
// (pas de fuite sur la fenêtre suivante), comme le bullet ci-dessus. Sans effet
// sur une fenêtre non fermable (`p_open` nul) : Échap ne la fermait déjà pas.
//
// L'état est PAR FENÊTRE et persisté dans imgui.ini, à côté de sa position et de
// sa taille, sous la même identité (le hachage repart après le dernier « ### »,
// donc un titre traduit garde son épingle d'une langue à l'autre).
void SetNextWindowPinnable();

// Branche la lecture/écriture des épingles sur imgui.ini. 🔴 À appeler entre
// ImGui::CreateContext() et la PREMIÈRE frame : ImGui charge son ini au premier
// NewFrame et IGNORE toute entrée dont le handler n'est pas encore enregistré —
// posé plus tard, on ne lirait rien et l'état déjà écrit serait perdu.
void RegisterPinSettingsHandler();

// Applique le skin RO à un popup ouvert HORS de toute fenêtre skinnée (menu
// contextuel du monde, par exemple). Un menu ouvert DANS une BeginRoWindow
// hérite déjà de ces couleurs — c'est pourquoi le menu de l'inventaire est
// clair sans rien demander ; celui du monde, lui, retombait sur le thème ImGui
// par défaut, donc sombre. À encadrer autour du BeginPopup/EndPopup :
//   const int n = ro::PushPopupSkin();
//   if (ImGui::BeginPopup("x")) { ... ImGui::EndPopup(); }
//   ro::PopPopupSkin(n);
// Le dépilage doit avoir lieu même quand BeginPopup renvoie faux : les couleurs
// sont poussées avant lui, pas par lui.
int PushPopupSkin();
void PopPopupSkin(int count);

// Couleur de CORPS (ImGuiCol_WindowBg) de la PROCHAINE fenêtre RO, en ARGB ImU32,
// à la place de la couleur configurée du skin. Pour une fenêtre qui doit rester
// blanche quels que soient les réglages (liste d'items du storage, p. ex.) :
//   ro::SetNextWindowBodyColor(IM_COL32(255, 255, 255, 255));
//   ro::BeginRoWindow(...);
// Comme le bullet, la demande est CONSOMMÉE par BeginRoWindow (pas de fuite sur la
// fenêtre suivante). Le reste du skin (titre, bordure, onglets…) est inchangé.
void SetNextWindowBodyColor(unsigned int argb);

// Bouton habillé avec les pièces btn_* du client (3-slice, états normal/survol/
// pressé). w/h à 0 = taille auto (texte + marges / hauteur native). Renvoie true
// au clic. À utiliser dans une fenêtre RO (fond clair). Ignore le skin toggle :
// dessine toujours le bouton RO (c'est un widget, pas un chrome de fenêtre).
//
// Une largeur IMPOSÉE trop courte pour le libellé ne le laisse pas déborder de
// l'art : le texte est rétréci (par crans, plancher à 78 %) puis, si ça ne suffit
// pas, coupé avec « ... » et le libellé entier passe en infobulle. C'est un
// garde-fou, pas une solution : le client parle FR/EN/ES et une mise en page
// calée sur le français y perd en lisibilité. Là où le conteneur peut respirer,
// mesurer les libellés traduits avec ButtonWidth/MaxButtonWidth ci-dessous.
bool RoButton(const char* label, float w = 0.0f, float h = 0.0f);
bool RoSmallButton(const char* label, float w = 0.0f, float h = 0.0f);

// Largeur qu'occuperait le bouton en taille automatique, DANS LA LANGUE COURANTE —
// de quoi dimensionner un conteneur sur ses libellés traduits :
//   const float col_w = ro::MaxButtonWidth({i18n::Tr("Panier"),
//                                           i18n::Tr("Achat 1-Click")});
// À appeler pendant une frame ImGui (la mesure dépend de la police courante).
float ButtonWidth(const char* label);
float SmallButtonWidth(const char* label);
float MaxButtonWidth(std::initializer_list<const char*> labels);

// Bouton d'état (outil courant, option retenue…) : quand `active` est vrai, il se
// dessine ENFONCÉ et son libellé passe en gras — l'art « press » seul se confond
// avec le survol. Renvoie true au clic, comme RoButton ; c'est à l'appelant de
// tenir l'état. Le gras est un faux-gras (re-dessin décalé d'un pixel) : ImGui n'a
// qu'une seule graisse chargée.
bool RoToggleButton(const char* label, bool active, float w = 0.0f, float h = 0.0f);
// Le même bouton d'état, en PETIT (art sbtn_*). Pour une barre d'onglets qui
// doit partager sa ligne avec autre chose : deux boutons pleine taille mangent
// une largeur que la fenêtre n'a pas toujours.
bool RoSmallToggleButton(const char* label, bool active, float w = 0.0f,
                         float h = 0.0f);

// Slider habillé en SCROLLBAR HORIZONTALE RO (pièces natives scroll1left|mid|right
// pour la piste/les flèches, scroll1bar_left|mid|right pour le curseur) : c'est le
// vocabulaire visuel du client, qui n'a pas de « slider » à proprement parler.
// Comportement ImGui conservé : drag du curseur, Ctrl+clic = saisie directe.
// Les FLÈCHES ajustent à l'unité : `arrow_step` (0 = 1 pour un entier, 0.01 pour
// un flottant), Shift = pas ×10, maintien = répétition. La valeur est affichée à
// droite de la barre, le label après (comme un slider ImGui). Retombe sur
// ImGui::Slider* quand le skin est désactivé.
bool RoSliderFloat(const char* label, float* v, float lo, float hi,
                   const char* format = "%.2f", float arrow_step = 0.0f,
                   int imgui_slider_flags = 0);
bool RoSliderInt(const char* label, int* v, int lo, int hi,
                 const char* format = "%d", int arrow_step = 0,
                 int imgui_slider_flags = 0);

// Case à cocher habillée avec les pièces checkbox_0/1 du client. Comme
// ImGui::Checkbox : renvoie true si l'état a changé.
bool RoCheckbox(const char* label, bool* v);

// Largeur qu'occupe cette case DANS LA LANGUE COURANTE — le pendant de
// ButtonWidth, et pour la même raison : une contrainte de taille de fenêtre
// calée sur le français laisse sortir un libellé anglais ou espagnol du cadre.
float CheckboxWidth(const char* label);

// Bouton radio habillé avec le skin natif du client (radiobtn_on/off.bmp sous
// 유저인터페이스\). Dessine l'image on/off + label cliquable ; renvoie true si cliqué
// (à l'appelant de poser la valeur). Repli automatique sur ImGui::RadioButton si les
// bmp sont absents du GRF/data. Un joueur qui remplace les bmp voit son skin.
bool RadioImage(const char* label, bool selected);

// Force le rechargement de TOUTES les pièces de skin chargées depuis le client.
//
// 🔴 Un seul appelant légitime : le changement de skin du jeu. `SkinMgr_SetSkin`
// purge le gestionnaire de textures natif, et les mêmes chemins servent ensuite
// d'autres images — sans cet appel, nos fenêtres garderaient l'ancienne apparence
// jusqu'au prochain reset de device. Les resets de device, eux, sont déjà couverts
// tout seuls.
//
// ⚠ À appeler AU TICK, jamais en frame : les pièces sont dessinées par la frame en
// cours. Cf. docs/game_option_re.md §3.9.
void InvalidateSkinTextures();

// Combo box (menu déroulant) habillé RO : champ (fond input + bordure) + bouton
// flèche txtbox_btn_a/b/c (états normal/survol/pressé, texture native du client),
// liste ouverte en popup au fond « corps » RO. S'utilise EXACTEMENT comme
// ImGui::BeginCombo — l'appelant ajoute des ImGui::Selectable entre les deux :
//   if (ro::RoBeginCombo("##id", preview)) { ...Selectable...; ro::RoEndCombo(); }
// RoEndCombo NE DOIT être appelé QUE si RoBeginCombo renvoie true. Largeur =
// ImGui::CalcItemWidth() (respecte SetNextItemWidth).
bool RoBeginCombo(const char* label, const char* preview_value);
void RoEndCombo();

// Combo « tout fait » : liste de libellés + index sélectionné, sans Begin/End à écrire.
// ⚠ CONTRAT DU RETOUR : true UNIQUEMENT sur un changement effectif de sélection (une
// seule frame), PAS tant que le popup est ouvert, et PAS si l'on reclique l'entrée déjà
// active. C'est ce qui permet aux appelants d'écrire sans risque le motif habituel
//   changed |= ro::RoCombo(...);  ...  if (changed) SaveSettings();
// sans déclencher une sérialisation par frame. Toute évolution doit préserver ce contrat.
//
// 🔴 `items` SE PASSE EN FRANÇAIS NU. RoCombo traduit chaque libellé À SA LECTURE,
// ce qui couvre d'un coup tous les combos du projet sans toucher un seul appel.
// Envelopper les items chez l'appelant les traduirait donc DEUX fois : le premier
// `Tr` rend l'anglais, le second ne le trouve pas au catalogue et l'inscrit comme
// « à traduire ». Rien ne se voit à l'écran — c'est le gabarit d'export qui se
// remplit de textes déjà traduits, et on cherche longtemps d'où ils viennent.
// (Le `label`, lui, s'enveloppe normalement : c'est un argument, pas un item.)
//
// ⚠ Corollaire : un tableau d'items ne doit JAMAIS être `static` s'il est
// construit avec `Tr` — mais comme il se passe nu, la question ne se pose pas.
bool RoCombo(const char* label, int* current_item, const char* const items[], int items_count);

// Barre d'onglets habillée RO (pièces tabh_{l,m,r}_{on,off} du client : contour
// gris à coins arrondis, l'onglet ACTIF ouvert sur le contenu). S'utilise
// EXACTEMENT comme ImGui::BeginTabBar — et les onglets eux-mêmes restent des
// ImGui::BeginTabItem/EndTabItem ordinaires, rien à convertir de ce côté :
//   if (ro::RoBeginTabBar("mes_onglets")) {
//     if (ImGui::BeginTabItem("Un")) { ...; ImGui::EndTabItem(); }
//     ro::RoEndTabBar();
//   }
// RoEndTabBar NE DOIT être appelé QUE si RoBeginTabBar renvoie true (il dépile
// les couleurs poussées par le Begin).
//
// 🔴 Une barre imbriquée dans une autre doit elle aussi passer par RoBeginTabBar :
// le Begin rend les fonds d'onglet ImGui TRANSPARENTS pour peindre l'art à leur
// place, et ce style reste actif pendant le contenu de l'onglet ouvert — un
// ImGui::BeginTabBar qui y resterait nu se retrouverait donc invisible.
bool RoBeginTabBar(const char* str_id, int tab_bar_flags = 0);
void RoEndTabBar();

// Barre horizontale 3-slice (btnbar_*) dessinée dans le rect donné, sur le draw
// list de la fenêtre courante. Pour un footer/bandeau dans une fenêtre RO.
void DrawBar(float x0, float y0, float x1, float y1);

// Dessine l'icône compteur (icon_num) à (x, y). Renvoie sa largeur (pour placer
// le texte du compteur juste après).
float DrawIconNum(float x, float y);

// Variante « fenêtre de description » : design distinct (barre de titre claire
// skill_upbar + cadre boîte sysbox) mais même config/couleurs/scrollbar/toggle.
// Pour les panneaux de description (item/skill). Même usage que Begin/EndRoWindow.
// `title_shadow` != 0 => ombre (ARGB ImU32) décalée +1,+1 sous le texte du titre
// (ex. 0x5050fa rouge pour un item cassé). 0 = pas d'ombre.
bool BeginRoDescWindow(const char* title, bool* p_open = nullptr,
                       int imgui_window_flags = 0, unsigned int title_shadow = 0);
void EndRoDescWindow();

// Panneau de description SANS barre de titre (cadre boîte sysbox complet). Pour
// les sous-panneaux attachés à une fenêtre de description (cartes, options).
bool BeginRoDescPanel(const char* id, int imgui_window_flags = 0);
void EndRoDescPanel();

// Épaisseur du bord du cadre sysbox (px). Sert à dimensionner un panneau dessiné
// à la main via DrawDescPanelFrame.
float DescPanelEdge();

// `fill_bg` = false : blitte UNIQUEMENT l'art sysbox, sans peindre le fond clair.
// Indispensable quand l'appelant a déjà peint un fond ARRONDI dessous (le fond de
// cette fonction est un rect à angles droits, il écraserait les coins ronds).
// Dessine le cadre « panneau de description » (fond clair + cadre sysbox 9-slice,
// même look que BeginRoDescPanel) dans le rect [x0,y0]-[x1,y1] sur un ImDrawList
// ARBITRAIRE. Permet de dessiner des sous-panneaux (cartes, options) directement
// sur la draw list de la fenêtre PARENTE — ainsi ils suivent son z-order au lieu
// d'être des fenêtres ImGui séparées qui passent derrière les autres. L'appelant
// gère le clip (les coords peuvent sortir du rect de la fenêtre courante ->
// PushClipRectFullScreen).
void DrawDescPanelFrame(ImDrawList* dl, float x0, float y0, float x1, float y1,
                        bool fill_bg = true);

// ── Échap centralisé ──────────────────────────────────────────────────────────
// Ferme la fenêtre RO la plus au-dessus (z-order) à chaque Échap, une par une,
// jusqu'à ce qu'il n'en reste aucune ; tant qu'une fenêtre RO fermable est
// ouverte, Échap est avalé (le jeu ne reçoit rien → pas de menu natif intempestif).
// BeginRo*Window enregistre automatiquement leur fenêtre (si p_open non null).
void RegisterEscapeWindow(bool* p_open);   // interne aux BeginRo*Window
void ProcessEscapeStack();                 // à appeler 1×/frame après tous les OnRenderUI
bool AnyEscapeWindowOpen();                // lu par le WndProc pour avaler Échap

// Sort la PROCHAINE fenêtre RO de la pile Échap : elle ne se fermera pas sur
// Échap, et n'avalera pas la touche non plus. À appeler juste AVANT son
// BeginRoWindow, qui consomme le drapeau.
//
// 🔴 À RÉSERVER aux fenêtres qu'on garde ouvertes EN TRAVAILLANT — l'établi du
// staff, par exemple. Pour tout le reste, Échap doit fermer : c'est le geste que
// le joueur essaie en premier, et une fenêtre qui lui résiste passe pour bloquée.
// Ici, l'inverse est vrai : l'établi resterait ouvert pendant qu'on joue, et
// chaque Échap destiné au menu du jeu le refermerait par surprise.
void SkipNextEscapeWindow();

// Neutralise la pile Échap pour CE frame : à appeler tant qu'un popup modal ImGui
// (ex. la modale « Signaler un bug ») est ouvert, AVANT ProcessEscapeStack. Sans
// ça, Échap fermerait à la fois la modale ET la fenêtre RO derrière. Race-free :
// on lève le flag pendant que la modale est encore ouverte, même si elle se ferme
// juste après (CloseCurrentPopup la retire aussitôt de la pile ImGui).
void SuppressEscapeStack();

// Fenêtre principale (Moonlight-Destiny) : Échap la MINIMISE (repli) au lieu de la
// fermer, et seulement EN DERNIER — quand plus aucune fenêtre fermable n'est ouverte
// (la seule restante avant que le jeu ne reçoive Échap pour ses natives). À appeler
// chaque frame où la fenêtre est DÉPLIÉE, en passant un pointeur vers un flag membre
// « repli demandé » que ProcessEscapeStack met à true et que la fenêtre consomme au
// rendu suivant. Compte comme « ouverte » pour l'avalage d'Échap.
void RegisterEscapeMinimizeWindow(bool* p_request_collapse);

// (Le skin RO n'est plus optionnel : c'est l'habillage STANDARD des fenêtres ImGui
// Bourgeon. SetSkinEnabled/IsSkinEnabled ont donc disparu — les textures sont
// toujours créées paresseusement à la 1ère utilisation.)

// ── Curseur RO au survol ──────────────────────────────────────────────────────
// Le toolkit DEMANDE un type de curseur RO (valeur de *(CursorMgr+0x50)) pour la
// frame courante ; le hook curseur (ragnarok_client) l'applique quand la souris
// est sur une fenêtre ImGui. 0 = flèche. Appelé par les widgets au survol.
// Les types de curseur que le toolkit sait demander. 🔴 CINQ fichiers
// redeclaraient cette valeur, parce que l'API demandait un `int` sans nommer
// ce qu'elle accepte — le défaut classique d'une signature trop large.
//
// ⚠ ET IL FAUT PASSER PAR `SetHoverCursor`. `ImGui::SetMouseCursor` est un NO-OP
// dans ce client : `io.ConfigFlags` porte `NoMouseCursorChange`. Quatre fichiers
// répétaient cette phrase à côté de leur copie de la constante.
//
// 🔴 VÉRIFIÉE EN JEU (cf. project_ro_cursor) — et c'est une leçon sur les copies :
// `ro_imgui.cc` portait encore « à confirmer », `weapon_refine_window` disait
// « vérifié ». Sur cinq copies, la plus ancienne n'est pas la plus juste.
constexpr int kCursorHand = 2;

void SetHoverCursor(int ro_cursor_type);
// Lu+remis à 0 par le hook curseur, une fois par frame (évite un état figé).
int TakeHoverCursor();

// « Curseur plein écran » : quand une UI Bourgeon occupe TOUT l'écran (login
// Moonlight, char-select), on veut n'avoir QUE le curseur redessiné par ImGui,
// partout — pas seulement au-dessus des fenêtres ImGui. Le plugin concerné appelle
// SetFullscreenCursorActive() CHAQUE frame tant qu'il occupe l'écran ; le latch
// expire de lui-même (≤1 frame) dès que plus personne ne l'asserte (retour au jeu,
// login classique, minimize) -> aucun « retrait » explicite à gérer.
// Effet (ragnarok_client) : le curseur natif est rendu HORS ÉCRAN (sa capture
// continue), et la garde « souris au-dessus d'une fenêtre ImGui » du redraw saute.
void SetFullscreenCursorActive();
bool FullscreenCursorActive();

// Leviers de customisation du skin (ce que RO ne propose pas). Modifiable à chaud ;
// lu par BeginRoWindow. Persisté par l'appelant via SkinConfig() (moonlight_ui).
struct RoSkinConfig {
  float title_brightness = 1.0f;  // 0.5..1.5 — multiplie l'art titre + boutons sys
  float rounding = 3.0f;          // 0..8    — arrondi bas de la fenêtre
  float alpha = 1.0f;             // 0.3..1  — opacité globale de la fenêtre
  float body_col[4]   = {243.f / 255, 245.f / 255, 250.f / 255, 1.f};  // corps
  float border_col[4] = {197.f / 255, 197.f / 255, 197.f / 255, 1.f};  // bordure
  float title_text[4] = {0.f, 0.f, 0.f, 1.f};                          // texte de la barre de titre
  float body_text[4]  = {28.f / 255, 30.f / 255, 38.f / 255, 1.f};     // texte du corps (labels, boutons, footer)
  float tab_col[4]    = {130.f / 255, 148.f / 255, 200.f / 255, 1.f};  // onglet actif
  float tab_inact[4]  = {210.f / 255, 214.f / 255, 222.f / 255, 1.f};  // onglet inactif
  float input_col[4]  = {206.f / 255, 206.f / 255, 206.f / 255, 1.f};  // champ de saisie
  float header_col[4] = {206.f / 255, 206.f / 255, 206.f / 255, 1.f};  // en-tête tableau
  float slot_col[4]   = {206.f / 255, 206.f / 255, 206.f / 255, 1.f};  // fond des cases (feuille perso)
  float doll_col[4]   = {228.f / 255, 230.f / 255, 236.f / 255, 1.f};  // fond du doll/avatar (feuille perso)
  float card_col[4]       = {245.f / 255, 243.f / 255, 232.f / 255, 1.f};  // fond carte item (crème)
  float card_head_col[4]  = {58.f / 255, 55.f / 255, 48.f / 255, 1.f};     // bandeau titre de carte
  float card_head_text[4] = {240.f / 255, 238.f / 255, 228.f / 255, 1.f};  // texte du bandeau de carte
  // Corps des fenêtres de LISTE (storage…) : elles veulent un fond propre, plus
  // clair que le corps général, pour que la liste d'items se lise bien. Blanc pur
  // par défaut. Utilisé via SetNextWindowBodyColor(ListBodyColorU32()).
  float list_col[4] = {1.f, 1.f, 1.f, 1.f};
};
RoSkinConfig& SkinConfig();

// `list_col` du skin en ARGB ImU32, prêt pour SetNextWindowBodyColor.
unsigned int ListBodyColorU32();

// Widgets de réglage du skin (sliders + color pickers) à placer dans un panneau
// ImGui existant. Renvoie true si une valeur a changé (pour déclencher une sauvegarde).
bool ShowRoSkinSettings();

// Luminosité (title_brightness, clampée 0..2) à appliquer comme teinte aux IMAGES de
// jeu (icônes d'item, aperçus) dessinées dans une fenêtre RO, pour qu'elles suivent
// le skin. L'opacité est déjà gérée par ImGui via style.Alpha → NON incluse ici.
// Usage : const float b = ro::SkinImageBrightness();
//         ImGui::Image(tex, size, ImVec2(0,0), ImVec2(1,1), ImVec4(b, b, b, 1.0f));
float SkinImageBrightness();

// La même luminosité, déjà prête à servir de teinte à ImDrawList::AddImage — qui
// prend un ImU32 et non un ImVec4, et n'applique PAS style.Alpha de lui-même
// (contrairement à ImGui::Image). L'alpha courant est donc intégré ici.
//
// Ce petit calcul était recopié à l'identique dans cart_viewer, inventory_viewer
// et storage_window, chacun sous le nom `SkinImgTint` dans son namespace anonyme.
ImU32 SkinImageTint();

// ── La couleur « 0xRRGGBB » du client <-> trois flottants d'ImGui ────────────
// Trois fichiers portaient ce couple, avec deux écritures de l'aller-retour.
// L'arrondi à +0.5f n'est pas décoratif : sans lui, un réglage relu puis
// réécrit DÉRIVE d'une unité à chaque tour.
inline void RgbToF3(int rgb, float* f) {
  f[0] = ((rgb >> 16) & 0xff) / 255.0f;
  f[1] = ((rgb >> 8) & 0xff) / 255.0f;
  f[2] = (rgb & 0xff) / 255.0f;
}
// La même couleur, mais prête pour un ImDrawList — l'alpha en plus.
//
// 🔴 Recopiée dans les TROIS fichiers qui utilisent déjà le couple ci-dessus,
// et à l'identique. `ui/color_codec.h` interdit pourtant en toutes lettres de
// « réinliner un décalage de bits » : la moitié de la famille était au foyer,
// le dernier membre était resté derrière.
inline ImU32 RgbToImU32(int rgb, int alpha = 255) {
  return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, alpha);
}
inline int F3ToRgb(const float* f) {
  const int r = static_cast<int>(f[0] * 255.0f + 0.5f);
  const int g = static_cast<int>(f[1] * 255.0f + 0.5f);
  const int b = static_cast<int>(f[2] * 255.0f + 0.5f);
  return (r << 16) | (g << 8) | b;
}


// InputText dont le buffer est du CP949 en entrée ET en sortie : la saisie
// (coréen via IME, latin) est éditée en UTF-8 en interne puis re-convertie en
// CP949 dans `cp949_buf`. `buf_size` = taille en octets du buffer CP949.
// `imgui_input_flags` = ImGuiInputTextFlags optionnels.
//
// Renvoie ce que renverrait `ImGui::InputText` avec ces mêmes drapeaux — donc
// « édité » dans le cas nu, mais « Entrée pressée » avec
// `ImGuiInputTextFlags_EnterReturnsTrue`. ⚠ `cp949_buf`, LUI, est tenu à jour à
// CHAQUE frappe dans les deux cas : ne pas déduire de la valeur de retour que le
// tampon n'a pas bougé.
bool InputTextCp949(const char* label, char* cp949_buf, size_t buf_size,
                    int imgui_input_flags = 0);

// ── Un texte, sa passe de RELIEF, et son faux gras ───────────────────────────
//
// Le geste « écrire deux fois le même texte, la première décalée » était posé à
// la main dans une vingtaine d'endroits, sous trois formes qui ne différaient
// que par leurs options :
//
//   if (ox || oy) dl->AddText({p.x + ox, p.y + oy}, contour, t);
//   dl->AddText(p, couleur, t);
//   if (bold) dl->AddText({p.x + 1.0f, p.y}, couleur, t);   // faux gras
//
// Ces trois lignes-là étaient recopiées MOT POUR MOT dans `skill_bar` (deux
// fois) et `status_icon_bar`, et sans le gras dans `item_cell` et la feuille de
// personnage.
//
// ⚠ « RELIEF » et non « ombre » : la passe décalée n'est pas toujours sombre.
// `storage_window` pose du BLANC sous un texte foncé — c'est un REHAUT, le même
// geste à l'envers. Le nom ne préjuge donc pas de la teinte.
//
// 🔴 `relief_off` NUL = aucune passe de relief. C'est voulu : les appelants
// portaient tous un `if (ox || oy)` qui disparaît ici. Un décalage qui vient
// d'un réglage du joueur peut valoir zéro, et c'est un cas NORMAL, pas un oubli.
//
// ⚠ Le décalage est en PIXELS D'ÉCRAN, pas à l'échelle de l'interface : les
// appelants qui veulent qu'il suive `ro::Px` le passent déjà converti (c'est le
// cas de la barre de titre RO). Convertir ici doublerait la mise à l'échelle de
// ceux qui le font déjà.
//
// `bold` repasse le texte décalé d'un pixel en X, DANS SA PROPRE COULEUR : le
// faux gras du client, celui qui épaissit sans changer de police.
void AddTextRelief(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text,
                   ImU32 relief_col, ImVec2 relief_off, bool bold = false);

// ── Un texte CERNÉ : ses huit voisins, puis lui ─────────────────────────────
//
// Le cerne qui rend un chiffre lisible PAR-DESSUS LA SCÈNE, quelle que soit la
// couleur du décor dessous — un badge de raffinement, un compteur de
// compétence, le temps restant d'un statut.
//
// Ces huit passes étaient écrites SIX fois, à la boucle près :
//
//   for (int oy = -1; oy <= 1; ++oy)
//     for (int ox = -1; ox <= 1; ++ox)
//       if (ox || oy) dl->AddText({p.x + ox, p.y + oy}, cerne, t);
//   dl->AddText(p, couleur, t);
//
// ⚠ Rayon d'UN pixel d'écran, jamais mis à l'échelle : c'est un cerne, pas une
// bordure. À l'échelle, il deviendrait un liseré épais qui mangerait le glyphe.
//
// ⛔ NE PAS y ramener les cernes à QUATRE passes (la feuille de personnage en a
// deux, en croix ; `entity_names` un, en diagonale). Ils sont volontairement
// plus légers — huit passes les épaissiraient.
void AddTextHalo(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text,
                 ImU32 halo_col, bool bold = false);
void AddTextHalo(ImDrawList* dl, ImFont* font, float font_px, ImVec2 pos,
                 ImU32 col, const char* text, ImU32 halo_col,
                 bool bold = false);

// La même, avec police et corps explicites — la forme qu'exigent les overlays
// qui dessinent hors du contexte de police courant. `text_end` sert aux
// appelants qui écrivent une TRANCHE de chaîne (une ligne de chat mesurée par
// pointeurs, jamais terminée par un zéro à l'endroit voulu).
void AddTextRelief(ImDrawList* dl, ImFont* font, float font_px, ImVec2 pos,
                   ImU32 col, const char* text, const char* text_end,
                   ImU32 relief_col, ImVec2 relief_off, bool bold = false);

// Idem, avec un indice affiché tant que le champ est VIDE. `hint` est de l'UTF-8
// (du texte à nous, jamais envoyé au client) : il ne passe PAS par le CP949.
bool InputTextCp949WithHint(const char* label, const char* hint, char* cp949_buf,
                            size_t buf_size, int imgui_input_flags = 0);

}  // namespace ro
