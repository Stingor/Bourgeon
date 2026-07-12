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
bool IsFontEnabled();

// Affiche une chaîne CP949 comme texte ImGui (convertit en UTF-8 d'abord).
void TextCp949(const char* cp949);

// ── Skin « façon RO » ─────────────────────────────────────────────────────────
// Fenêtre ImGui habillée avec les VRAIES pièces du client (barre de titre 3-slice
// titlebar_*, boutons sys_close/sys_mini, corps plein). S'utilise comme Begin/End :
//   if (ro::BeginRoWindow("Titre", &open)) { ... }
//   ro::EndRoWindow();
// EndRoWindow doit TOUJOURS être appelé après BeginRoWindow (même si false).
// Quand le skin est désactivé (SetSkinEnabled(false)), retombe sur un ImGui::Begin
// standard — les mécaniques (drag/resize/collapse) restent natives ImGui dans les
// deux cas. Renvoie ImGui::Begin (false si repliée/masquée).
bool BeginRoWindow(const char* title, bool* p_open = nullptr,
                   int imgui_window_flags = 0);
void EndRoWindow();

// Bouton habillé avec les pièces btn_* du client (3-slice, états normal/survol/
// pressé). w/h à 0 = taille auto (texte + marges / hauteur native). Renvoie true
// au clic. À utiliser dans une fenêtre RO (fond clair). Ignore le skin toggle :
// dessine toujours le bouton RO (c'est un widget, pas un chrome de fenêtre).
bool RoButton(const char* label, float w = 0.0f, float h = 0.0f);

// Case à cocher habillée avec les pièces checkbox_0/1 du client. Comme
// ImGui::Checkbox : renvoie true si l'état a changé.
bool RoCheckbox(const char* label, bool* v);

// Combo box (menu déroulant) habillé RO : champ (fond input + bordure) + bouton
// flèche txtbox_btn_a/b/c (états normal/survol/pressé, texture native du client),
// liste ouverte en popup au fond « corps » RO. S'utilise EXACTEMENT comme
// ImGui::BeginCombo — l'appelant ajoute des ImGui::Selectable entre les deux :
//   if (ro::RoBeginCombo("##id", preview)) { ...Selectable...; ro::RoEndCombo(); }
// RoEndCombo NE DOIT être appelé QUE si RoBeginCombo renvoie true. Largeur =
// ImGui::CalcItemWidth() (respecte SetNextItemWidth).
bool RoBeginCombo(const char* label, const char* preview_value);
void RoEndCombo();

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

// Dessine le cadre « panneau de description » (fond clair + cadre sysbox 9-slice,
// même look que BeginRoDescPanel) dans le rect [x0,y0]-[x1,y1] sur un ImDrawList
// ARBITRAIRE. Permet de dessiner des sous-panneaux (cartes, options) directement
// sur la draw list de la fenêtre PARENTE — ainsi ils suivent son z-order au lieu
// d'être des fenêtres ImGui séparées qui passent derrière les autres. Repli
// rounded-rect si le skin est désactivé. L'appelant gère le clip (les coords
// peuvent sortir du rect de la fenêtre courante -> PushClipRectFullScreen).
void DrawDescPanelFrame(ImDrawList* dl, float x0, float y0, float x1, float y1);

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

// Active/désactive le skin RO à chaud (les textures sont créées à la 1ère utilisation).
void SetSkinEnabled(bool enabled);
bool IsSkinEnabled();

// ── Curseur RO au survol ──────────────────────────────────────────────────────
// Le toolkit DEMANDE un type de curseur RO (valeur de *(CursorMgr+0x50)) pour la
// frame courante ; le hook curseur (ragnarok_client) l'applique quand la souris
// est sur une fenêtre ImGui. 0 = flèche. Appelé par les widgets au survol.
void SetHoverCursor(int ro_cursor_type);
// Lu+remis à 0 par le hook curseur, une fois par frame (évite un état figé).
int TakeHoverCursor();

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
  float card_col[4]       = {245.f / 255, 243.f / 255, 232.f / 255, 1.f};  // fond carte item (crème)
  float card_head_col[4]  = {58.f / 255, 55.f / 255, 48.f / 255, 1.f};     // bandeau titre de carte
  float card_head_text[4] = {240.f / 255, 238.f / 255, 228.f / 255, 1.f};  // texte du bandeau de carte
};
RoSkinConfig& SkinConfig();

// Widgets de réglage du skin (sliders + color pickers) à placer dans un panneau
// ImGui existant. Renvoie true si une valeur a changé (pour déclencher une sauvegarde).
bool ShowRoSkinSettings();

// Luminosité (title_brightness, clampée 0..2) à appliquer comme teinte aux IMAGES de
// jeu (icônes d'item, aperçus) dessinées dans une fenêtre RO, pour qu'elles suivent
// le skin. L'opacité est déjà gérée par ImGui via style.Alpha → NON incluse ici.
// Usage : const float b = ro::SkinImageBrightness();
//         ImGui::Image(tex, size, ImVec2(0,0), ImVec2(1,1), ImVec4(b, b, b, 1.0f));
float SkinImageBrightness();


// InputText dont le buffer est du CP949 en entrée ET en sortie : la saisie
// (coréen via IME, latin) est éditée en UTF-8 en interne puis re-convertie en
// CP949 dans `cp949_buf`. `buf_size` = taille en octets du buffer CP949.
// `imgui_input_flags` = ImGuiInputTextFlags optionnels. Renvoie true quand édité.
bool InputTextCp949(const char* label, char* cp949_buf, size_t buf_size,
                    int imgui_input_flags = 0);

}  // namespace ro
