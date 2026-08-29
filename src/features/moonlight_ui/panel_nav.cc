// ── La mécanique commune des en-têtes et des navs latérales ──────────────────
// Ce fichier ne dessine AUCUN réglage : il porte ce qui est partagé entre les
// en-têtes du panneau Moonlight et les deux navigations latérales qu'ils
// hébergent (« Interface de jeu » et « Gameplay »).
//
//   • la table des EN-TÊTES et l'en-tête qui sait se lier (LinkableHeader) ;
//   • la résolution d'une CLÉ de lien vers sa destination (DestLabel,
//     DestParentLabel, GroupByKey) ;
//   • la NAV elle-même (BeginNavPanel/EndNavPanel) : liste à gauche, contenu à
//     droite, cache de largeur, geste de lien, infobulles.
//
// Tout cela vivait dans panel_interface.cc, où c'était la mécanique d'une seule
// nav. Le jour où « Gameplay » a eu besoin de la même — même geste, même cache de
// largeur, mêmes deux infobulles concurrentes — la recopier aurait fait diverger
// la lecture la plus délicate du système de liens (le clic modifié sur un
// CollapsingHeader, et le Selectable muet quand la barre de chat détient
// l'ActiveId). Les TABLES, elles, restent chacune dans son panel_*.cc : c'est le
// contenu qui a une place naturelle, pas le mécanisme.

#include "features/moonlight_ui/internal.h"

#include <algorithm>
#include <cstring>  // std::strcmp (résolution d'une clé)

#include "bourgeon.h"
#include "imgui.h"
#include "imgui_internal.h"         // TreeNodeSetOpen (annuler le pli d'un Maj + clic)
#include "features/link_gesture.h"  // Maj + clic sur une entrée de nav = un lien
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/staff_gate.h"    // IsStaff (en-tête réservé au staff)
#include "features/windows/game_settings.h"  // gslink — les onglets sont des destinations
#include "ui/ro_widgets.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace {

// Un EN-TÊTE du panneau. `staff_only` n'est pas de la décoration : l'en-tête
// n'EXISTE pas chez un non-staff, donc son lien ne doit pas s'y former — sans
// quoi le lecteur cliquerait sur un lien qui ne peut rien ouvrir.
struct PanelHeader {
  const char* key;
  const char* label;  // non traduit
  bool        staff_only;
};

// ⚠ « staff_tools » a QUITTÉ cette table : les outils du staff ne sont plus un
// en-tête de ce panneau mais leur propre fenêtre (features/windows/staff_tools.h).
// L'y laisser aurait formé des liens vers une destination qui n'existe plus —
// `DestLabel` rendant un libellé pour une clé que `LinkableHeader` ne déplie
// jamais, le lien aurait été cliquable et muet.
//
// ⚠ « graphics » l'a quittée à son tour : la section Graphismes est devenue
// l'onglet du même nom de **Game Settings**, et c'est `gslink` qui porte
// désormais sa clé — la MÊME, pour que les liens déjà posés dans le chat
// continuent d'ouvrir la bonne chose. `DestLabel` la lui demande plus bas.
constexpr PanelHeader kPanelHeaders[] = {
    {"rules",       "Règles du serveur", false},
    {"dps",         "DPS Meter",         false},
    {"minigames",   "Mini-jeux",         false},
    {"interface",   "Interface de jeu",  false},
    {"gameplay",    "Gameplay",          false},
    {"commands",    "Commands Settings", false},
};

// ⚠ LE GESTE DE LIEN A DÉMÉNAGÉ dans `links::ShiftClickedLastItem` /
// `links::HoveredForLinkTooltip` (features/link_gesture.h), avec le récit de ses
// deux échecs. Il est parti d'ici le jour où les ONGLETS de Game Settings ont eu
// besoin du même geste : le recopier là-bas aurait fait diverger la seule lecture
// délicate du système de liens.
bool ShiftClickedLastItem() { return links::ShiftClickedLastItem(); }

// L'en-tête que désigne cette clé, s'il est DISPONIBLE pour ce joueur.
const PanelHeader* HeaderByKey(const char* key) {
  if (key == nullptr || key[0] == '\0') return nullptr;
  for (const PanelHeader& header : kPanelHeaders) {
    if (std::strcmp(header.key, key) != 0) continue;
    return (header.staff_only && !IsStaff()) ? nullptr : &header;
  }
  return nullptr;
}

}  // namespace

namespace iface {

const NavGroup& Group(NavGroupId group) {
  return (group == kNavGameplay) ? GameplayGroup() : InterfaceGroup();
}

namespace {

// La section que porte cette clé, cherchée dans les DEUX navs — un seul espace de
// clés, donc une seule recherche. `*out_group` reçoit la nav qui l'héberge.
const NavEntry* EntryByKey(const char* key, const NavGroup** out_group) {
  if (key == nullptr || key[0] == '\0') return nullptr;
  for (int g = 0; g < kNavGroupCount; ++g) {
    const NavGroup& group = Group(static_cast<NavGroupId>(g));
    for (int i = 0; i < group.count; ++i) {
      if (std::strcmp(group.entries[i].key, key) != 0) continue;
      if (out_group != nullptr) *out_group = &group;
      return &group.entries[i];
    }
  }
  return nullptr;
}

}  // namespace

const NavGroup* GroupByKey(const char* key, int* id) {
  const NavGroup* group = nullptr;
  const NavEntry* entry = EntryByKey(key, &group);
  if (entry == nullptr) return nullptr;
  if (id != nullptr) *id = entry->id;
  return group;
}

const char* DestLabel(const char* key) {
  if (const NavEntry* entry = EntryByKey(key, nullptr)) return entry->label;
  if (const PanelHeader* header = HeaderByKey(key)) return header->label;
  // Troisième étage : les onglets de Game Settings. Ils vivent dans leur propre
  // fenêtre, mais partagent l'espace de clés — un lien de réglage ne dit pas
  // QUELLE fenêtre il ouvre, seulement CE qu'il désigne.
  return gslink::LabelByKey(key);
}

const char* DestParentLabel(const char* key) {
  const NavGroup* group = nullptr;
  if (EntryByKey(key, &group) == nullptr) return nullptr;  // un en-tête n'a pas de parent
  // Le libellé de l'en-tête d'accueil, et non une chaîne écrite ici : « Interface
  // de jeu » était en dur dans l'infobulle des liens, et la première section de
  // Gameplay s'y serait annoncée sous le mauvais en-tête.
  const PanelHeader* header = HeaderByKey(group->header_key);
  return (header != nullptr) ? header->label : nullptr;
}

// ── L'en-tête qui sait se lier ───────────────────────────────────────────────
// Remplace `CollapsingHeader(i18n::Tr("…"))` aux en-têtes du panneau. Il porte les
// deux bouts du lien : le geste qui le POSE (Maj + clic) et le saut qui l'HONORE
// (un lien reçu déplie l'en-tête et scrolle dessus).
//
// 🔴 UN EN-TÊTE IMGUI REFUSE LE CLIC MODIFIÉ, et c'est ce qui a fait échouer la
// première version de ce code. `TreeNodeBehavior` pose `NoKeyModsAllowed` dès que
// la souris n'est pas sur la FLÈCHE (imgui_widgets.cpp, « We allow clicking on the
// arrow section with keyboard modifiers held ») : Maj enfoncé, le corps de
// l'en-tête ne reçoit aucun clic, donc aucune bascule à observer — guetter
// `IsItemToggledOpen()` ne déclenchait jamais rien.
// On lit donc le geste NOUS-MÊMES, sur le survol : `mods_ok` ne gouverne que
// l'activation, `hovered` reste calculé normalement. Et c'est aussi le geste que
// `links::Hit` reconnaît partout ailleurs — clic ENFONCÉ, pas relâché.
//
// ⚠ Sur la flèche, en revanche, les modificateurs SONT acceptés et l'en-tête a
// basculé : on annule dans ce cas précis, pour que le geste soit le même sur toute
// la largeur.
bool LinkableHeader(const char* key) {
  const PanelHeader* header = HeaderByKey(key);
  // Clé inconnue (ou en-tête réservé au staff) : on dessine quand même l'en-tête,
  // sinon une faute de frappe ferait disparaître toute une partie du panneau.
  // Seul le LIEN se retire, ce qui est la seule chose qui n'a pas de sens ici.
  const char* label = (header != nullptr) ? header->label : key;

  MoonlightUi* mu = Bourgeon::Instance().moonlight_ui();
  const bool jump = (mu != nullptr) && mu->ConsumeHeaderJump(key);
  if (jump) ImGui::SetNextItemOpen(true, ImGuiCond_Always);

  bool open = CollapsingHeader(i18n::Tr(label));
  if (jump) ImGui::SetScrollHereY(0.0f);

  if (header != nullptr && links::CanPostToChat() && ShiftClickedLastItem()) {
    links::PostToChat(links::FromSetting(key));
    // Clic sur la flèche : lui seul a été accepté, donc lui seul a basculé.
    if (ImGui::IsItemToggledOpen()) {
      ImGui::TreeNodeSetOpen(ImGui::GetItemID(), !open);
      open = !open;
    }
  }
  // Le geste n'a aucune trace visible : sans cette ligne, il n'existe que pour
  // qui l'a lu dans un changelog. Mêmes drapeaux de survol : l'aide doit tenir
  // exactement là où le geste tient, sinon elle disparaît précisément au moment
  // où l'on s'en sert — la barre de chat ouverte.
  if (header != nullptr && links::CanPostToChat() &&
      links::HoveredForLinkTooltip())
    ImGui::SetTooltip(i18n::Tr("Maj + clic : poser le lien de cette "
                               "section dans le chat"));
  return open;
}

// ── La nav latérale ──────────────────────────────────────────────────────────
// Liste des sections à gauche, contenu de la section choisie à droite. L'entrée
// active est un état de l'APPELANT (un membre de MoonlightUi) et non une statique
// locale : un lien reçu doit pouvoir la piloter depuis une autre fenêtre.
void BeginNavPanel(const NavGroup& group, int* current) {
  // Dimensions dérivées du texte/style (pas de pixels fixes) : la liste garde
  // la largeur de sa plus longue entrée, bornée à 40 % de la place dispo pour
  // rester lisible sur fenêtre étroite.
  // La mesure des libellés ne dépend que de la POLICE et de la LANGUE : on la
  // garde en cache au lieu de refaire un CalcTextSize par entrée à chaque frame,
  // et on la réinvalide dès que l'une des deux bouge.
  //
  // 🔴 LA LANGUE FAIT PARTIE DE LA CLÉ, au même titre que la police. Sans elle,
  // la largeur restait celle de la langue affichée en premier : en passant de
  // l'anglais à l'espagnol, la liste gardait la mesure des libellés anglais —
  // plus courts — et « Seguimiento de misiones » sortait coupé net à
  // « Seguimiento de m ». Rien ne le signalait, et changer de police « réparait »
  // le symptôme le temps d'une remesure, ce qui égarait le diagnostic.
  //
  // ⚠ Le cache est indexé PAR GROUPE : les deux navs se dessinent dans la même
  // frame, et une seule statique aurait donné à « Gameplay » la largeur mesurée
  // sur « Interface de jeu » (bien plus large) — puis l'inverse à la frame
  // suivante, la liste changeant de largeur à chaque battement.
  const ImGuiStyle& st = ImGui::GetStyle();
  struct LabelsWidth {
    float    w    = 0.0f;   // largeur du plus long libellé, en px
    ImFont*  font = nullptr;
    float    size = 0.0f;
    unsigned lang = 0u;     // i18n::CatalogEpoch() de la mesure
  };
  static LabelsWidth s_cache[kNavGroupCount];
  LabelsWidth& cache = s_cache[group.group];
  if (cache.font != ImGui::GetFont() || cache.size != ImGui::GetFontSize() ||
      cache.lang != i18n::CatalogEpoch()) {
    cache.w = 0.0f;
    for (int i = 0; i < group.count; ++i)
      cache.w = (std::max)(cache.w,
                           ImGui::CalcTextSize(i18n::Tr(group.entries[i].label)).x);
    cache.font = ImGui::GetFont();
    cache.size = ImGui::GetFontSize();
    cache.lang = i18n::CatalogEpoch();
  }
  const float nav_w_wanted =
      cache.w + st.WindowPadding.x * 2.0f + st.FramePadding.x * 2.0f;
  const float nav_w_min = ImGui::GetFontSize() * 5.0f;
  const float nav_w_max =
      (std::max)(nav_w_min, ImGui::GetContentRegionAvail().x * 0.4f);
  const float nav_w = (std::min)((std::max)(nav_w_wanted, nav_w_min), nav_w_max);
  // La borne a mordu : les libellés ne tiennent pas et ImGui les coupe NET, sans
  // même une ellipse pour le dire. C'est ce qui arrive à une fenêtre étroite dans
  // une langue verbeuse — « Suivi de quête » fait 14 signes, « Seguimiento de
  // misiones » en fait 23. On ne triche pas sur la largeur (le contenu à droite
  // en a besoin), on rend le libellé lisible autrement : en infobulle.
  const bool nav_clipped = nav_w < nav_w_wanted;
  const float nav_h = ImGui::GetTextLineHeightWithSpacing() * group.count
                    + st.WindowPadding.y * 2.0f;

  // Identifiant du child dérivé de la CLÉ D'EN-TÊTE : deux navs dans la même
  // fenêtre, deux états de scroll distincts. Un « nav » partagé les aurait fait
  // se marcher dessus.
  ImGui::PushID(group.header_key);
  ImGui::BeginChild("nav", ImVec2(nav_w, nav_h), ImGuiChildFlags_Borders);
  // 🔴 Maj + clic POSE LE LIEN DE LA SECTION dans la barre de chat, et ne
  // navigue pas. C'est la convention des liens du client (features/link_gesture.h)
  // appliquée à une entrée de nav : « regarde ce réglage » est exactement ce
  // qu'on veut dire à quelqu'un qu'on aide, et le décrire à la voix (« le panneau
  // Moonlight, en-tête Interface de jeu, huitième entrée ») ne marche jamais.
  //
  // Maj DÉSARME donc la sélection : sans ça le geste poserait le lien ET
  // changerait de section sous les yeux de celui qui explique.
  const bool link_posts = links::CanPostToChat();
  const bool shift_held = ImGui::GetIO().KeyShift;
  for (int i = 0; i < group.count; ++i) {
    const NavEntry& entry = group.entries[i];
    const bool selected = ImGui::Selectable(i18n::Tr(entry.label),
                                            *current == entry.id);
    // 🔴 Le geste de lien est lu SUR LA GÉOMÉTRIE, et pas par le retour du
    // Selectable : celui-ci est muet dès que la barre de saisie du chat détient
    // l'ActiveId — c'est-à-dire juste après qu'on y a posé un lien (cf.
    // ShiftClickedLastItem). Le deuxième lien d'affilée ne partait jamais.
    if (link_posts && ShiftClickedLastItem()) {
      links::PostToChat(links::FromSetting(entry.key));
    } else if (selected && !(link_posts && shift_held)) {
      // ⚠ Sans barre de chat pour l'accueillir, Maj + clic redevient un clic
      // ORDINAIRE. Le geste n'est alors annoncé nulle part : le laisser avaler
      // le clic rendrait l'entrée muette pour qui a une main sur Maj sans y
      // penser — et il n'aurait aucun moyen de comprendre pourquoi.
      *current = entry.id;
    }
    // Deux choses à dire au survol, et aucune n'est toujours vraie :
    //   • le libellé ENTIER, quand la liste est trop étroite pour lui — sinon
    //     rien ne permet de lire une entrée coupée ;
    //   • le geste de lien, qui n'a aucune trace visible et n'existerait que
    //     pour qui l'a lu dans un changelog — seulement s'il y a une barre de
    //     chat pour l'accueillir.
    // Une seule infobulle porte les deux : deux `SetTooltip` d'affilée sur le
    // même item, c'est le second qui écrase le premier. Le délai vient du style
    // (`ForTooltip`) : la liste se survole en permanence, elle ne doit pas
    // clignoter au passage.
    if ((link_posts || nav_clipped) && links::HoveredForLinkTooltip()) {
      const char* const gesture =
          link_posts ? i18n::Tr("Maj + clic : poser le lien de cette "
                                "section dans le chat")
                     : nullptr;
      // Format « %s » plutôt que la chaîne nue : une traduction est une donnée
      // de fichier, et un « % » qui s'y glisserait serait lu comme une
      // conversion — avec un argument qui n'existe pas.
      if (nav_clipped && gesture != nullptr)
        ImGui::SetTooltip("%s\n%s", i18n::Tr(entry.label), gesture);
      else
        ImGui::SetTooltip("%s", nav_clipped ? i18n::Tr(entry.label) : gesture);
    }
  }
  ImGui::EndChild();

  SameLine();
  // Hauteur libre (AutoResizeY) : le panneau prend exactement la place de son
  // contenu et c'est la fenêtre parente qui scrolle — plus de dépendance à la
  // taille de la fenêtre ni de scrollbar imbriquée.
  ImGui::BeginChild("content", ImVec2(0.0f, 0.0f),
                    ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
  ImGui::PushTextWrapPos(0.0f);  // wrap le texte à la largeur du child
}

void EndNavPanel() {
  ImGui::PopTextWrapPos();
  ImGui::EndChild();
  ImGui::PopID();  // apparié au PushID(header_key) de BeginNavPanel
}

}  // namespace iface
