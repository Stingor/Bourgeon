#pragma once

#include <cstddef>

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
// Mêmes garanties de tampon que les fonctions ci-dessus (rotatif, à consommer
// tout de suite). Sens retour pour une saisie ImGui qui repart au serveur.
const char* WireToUtf8(const char* ansi);
const char* Utf8ToWire(const char* utf8);

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
void SetFontEnabled(bool enabled);

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

// ── Familles au choix pour la chatbox ────────────────────────────────────────
// Toutes bakées au démarrage : basculer ensuite ne coûte rien. L'index 0 est
// « Système », qui ne force rien et laisse la police de base — c'est la seule à
// couvrir le coréen quand le réglage de débogage est actif.
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
bool RoButton(const char* label, float w = 0.0f, float h = 0.0f);
bool RoSmallButton(const char* label, float w = 0.0f, float h = 0.0f);

// Bouton d'état (outil courant, option retenue…) : quand `active` est vrai, il se
// dessine ENFONCÉ et son libellé passe en gras — l'art « press » seul se confond
// avec le survol. Renvoie true au clic, comme RoButton ; c'est à l'appelant de
// tenir l'état. Le gras est un faux-gras (re-dessin décalé d'un pixel) : ImGui n'a
// qu'une seule graisse chargée.
bool RoToggleButton(const char* label, bool active, float w = 0.0f, float h = 0.0f);

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

// Bouton radio habillé avec le skin natif du client (radiobtn_on/off.bmp sous
// 유저인터페이스\). Dessine l'image on/off + label cliquable ; renvoie true si cliqué
// (à l'appelant de poser la valeur). Repli automatique sur ImGui::RadioButton si les
// bmp sont absents du GRF/data. Un joueur qui remplace les bmp voit son skin.
bool RadioImage(const char* label, bool selected);

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
bool RoCombo(const char* label, int* current_item, const char* const items[], int items_count);

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


// InputText dont le buffer est du CP949 en entrée ET en sortie : la saisie
// (coréen via IME, latin) est éditée en UTF-8 en interne puis re-convertie en
// CP949 dans `cp949_buf`. `buf_size` = taille en octets du buffer CP949.
// `imgui_input_flags` = ImGuiInputTextFlags optionnels. Renvoie true quand édité.
bool InputTextCp949(const char* label, char* cp949_buf, size_t buf_size,
                    int imgui_input_flags = 0);

// Idem, avec un indice affiché tant que le champ est VIDE. `hint` est de l'UTF-8
// (du texte à nous, jamais envoyé au client) : il ne passe PAS par le CP949.
bool InputTextCp949WithHint(const char* label, const char* hint, char* cp949_buf,
                            size_t buf_size, int imgui_input_flags = 0);

}  // namespace ro
