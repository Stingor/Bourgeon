#include "features/moonlight_ui/internal.h"
#include "features/systems/moonlight_auth.h"  // SiteBaseUrl

#include <windows.h>
#include <shellapi.h>  // ShellExecuteA (lien « avatar Discord » vers l'UCP)

#include <algorithm>
#include <cstring>  // std::strcmp (résolution d'une clé de section)

#include "bourgeon.h"
#include "imgui.h"
#include "imgui_internal.h"  // TreeNodeSetOpen (annuler le pli d'un Maj + clic)
#include "features/link_gesture.h"  // Maj + clic sur une entrée de nav = un lien
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/staff_gate.h"  // IsStaff (compteur de textes non traduits)
#include "ui/align_grid.h"
#include "ui/color_codec.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "ui/skin_panel.h"
#include "ragnarok/msgstring_override.h"
#include "utils/i18n.h"
#include "utils/log_console.h"  // LogDiag (export du gabarit de traduction)

// Types COMPLETS des plugins pilotés par les 13 sections (bourgeon.h n'en donne
// que des déclarations anticipées).
#include "features/overlays/basic_info.h"
#include "features/overlays/cast_bar.h"
#include "features/overlays/chat_balloon.h"
#include "features/windows/bank_window.h"
#include "features/windows/craft_atlas.h"
#include "features/windows/game_settings.h"  // gslink — les onglets sont des destinations
#include "features/windows/make_item_window.h"
#include "features/windows/entity_context_menu.h"
#include "features/windows/monster_info_window.h"
#include "features/windows/pet_window.h"
#include "features/windows/weapon_refine_window.h"
#include "features/systems/bug_report.h"
#include "features/patches/chat.h"
#include "features/windows/chat_window.h"
#include "features/windows/inventory_viewer.h"
#include "features/windows/cart_viewer.h"
#include "features/windows/item_desc_window.h"
#include "features/overlays/menu_icons.h"
#include "features/windows/npc_dialog_window.h"
#include "features/overlays/minimap.h"
#include "features/overlays/quest_tracker.h"
#include "features/overlays/item_obtain_toast.h"
#include "features/windows/rodex_window.h"
#include "features/overlays/skill_bar.h"
#include "features/overlays/status_icon_bar.h"
#include "features/overlays/party_frames.h"
#include "features/windows/party_friend_window.h"
#include "features/overlays/target_frame.h"
#include "features/windows/storage_window.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// Page du panneau utilisateur (UCP) du site qui génère l'avatar Discord du
// personnage au bon format. Mentionnée dans la section « Chat », à côté du
// réglage du relais Discord.
// ⚠ Composee a l'usage et non figee : le domaine suit `SiteBaseUrl()`, donc une
// instance de dev ouvre SON panneau utilisateur et non la production.
constexpr const char* kDiscordAvatarPath = "/ucp.php?i=profile&mode=avatar";

std::string DiscordAvatarUrl() {
  return std::string(SiteBaseUrl()) + kDiscordAvatarPath;
}

// ── LES DESTINATIONS D'UN LIEN DE RÉGLAGE ────────────────────────────────────
// Deux étages, deux tables : les EN-TÊTES du panneau (Staff Tools, Graphismes,
// Interface de jeu…) et les SECTIONS de la nav latérale d'« Interface de jeu ».
// Elles partagent un seul espace de CLÉS — une clé désigne une destination, sans
// qu'on ait à dire de quel étage elle vient.
//
// Source UNIQUE dans les deux cas : chaque ligne porte sa clé, son libellé et,
// pour une section, son identifiant d'enum. Insérer/déplacer une entrée ne peut
// donc pas désaligner silencieusement le libellé et le contenu — la panne muette
// que produisait la paire « enum + tableau de chaînes » maintenue à la main.
//
// 🔴 LA CLÉ N'EST PAS LE NUMÉRO D'ENUM, et c'est tout l'intérêt. Un lien de
// réglage posté dans le chat voyage vers d'AUTRES clients, qui peuvent avoir une
// autre version de Bourgeon : un numéro y désignerait la section voisine à la
// première insertion, et le lecteur atterrirait sans le savoir sur le mauvais
// réglage. La clé, elle, est un nom stable — inconnue chez le lecteur, elle ne
// résout rien du tout, ce qui est le bon échec.
// ⚠ Une clé posée ne se renomme donc JAMAIS : les lignes de chat déjà envoyées
// la portent encore. Le libellé, lui, est libre (il est traduit à l'affichage).
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

struct IfaceEntry {
  MoonlightUi::IfaceSection id;
  const char* key;
  const char* label;
};

// 🔴 ORDRE ALPHABÉTIQUE DU LIBELLÉ FRANÇAIS, et c'est un ordre FIGÉ, pas un tri.
// La liste s'était construite par ordre d'arrivée des chantiers : personne ne
// pouvait deviner où chercher « Fiche de pet », et chaque nouvelle section
// empirait le cas. Ranger la table une fois suffit — rien n'indexe ce tableau, ni
// la persistance (qui va par `key`) ni le rendu du contenu (qui va par `id`).
//
// ⚠ L'ordre suit le FRANÇAIS, y compris pour un joueur en anglais ou en espagnol,
// où la liste n'est donc alphabétique qu'à peu près (« Cast bar » se lit sous
// « Barre d'incantation »). C'est assumé : trier à l'affichage sur `i18n::Tr`
// demanderait une comparaison qui plie les accents et respecte la locale, pour un
// gain qui ne concerne pas la langue de référence.
constexpr IfaceEntry kIfaceSections[] = {
    {MoonlightUi::kIfaceSkillBar,    "skill_bar",    "Barre d'action"},
    {MoonlightUi::kIfaceCastBar,     "cast_bar",     "Barre de Cast"},
    {MoonlightUi::kIfaceBasicInfo,   "basic_info",   "Basic Info"},
    {MoonlightUi::kIfaceCart,        "cart",         "Cart"},
    {MoonlightUi::kIfaceChat,        "chat",         "Chat"},
    {MoonlightUi::kIfaceDesc,        "desc",         "Descriptions"},
    {MoonlightUi::kIfaceMakeItem,    "make_item",    "Fabrication"},
    {MoonlightUi::kIfaceTargetFrame, "target_frame", "Fenêtre de cible"},
    {MoonlightUi::kIfacePartyFrames, "party_frames", "Groupe (grille)"},
    {MoonlightUi::kIfacePartyFriend, "party_friend", "Groupe / Amis"},
    {MoonlightUi::kIfaceNpc,         "npc",          "Fenêtre NPC"},
    {MoonlightUi::kIfaceMonsterInfo, "monster_info", "Fiche de monstre"},
    {MoonlightUi::kIfacePet,         "pet",          "Fiche de pet"},
    {MoonlightUi::kIfaceStatusIcons, "status_icons", "Icônes de statut"},
    {MoonlightUi::kIfaceMenuIcons,   "menu_icons",   "Icônes du menu"},
    {MoonlightUi::kIfaceInventory,   "inventory",    "Inventaire"},
    {MoonlightUi::kIfaceContextMenu, "context_menu", "Menu contextuel"},
    {MoonlightUi::kIfaceMinimap,     "minimap",      "Minimap"},
    {MoonlightUi::kIfaceItemToast,   "item_toast",   "Objet obtenu"},
    {MoonlightUi::kIfaceCraftAtlas,  "craft_atlas",  "Recettes"},
    {MoonlightUi::kIfaceRefine,      "refine",       "Refine"},
    {MoonlightUi::kIfaceSkin,        "skin",         "Skin RO"},
    {MoonlightUi::kIfaceStorage,     "storage",      "Storage"},
    {MoonlightUi::kIfaceQuest,       "quest",        "Suivi de quête"},
};
// Message de static_assert : un LITTÉRAL. Il est lu à la compilation et
// s'adresse au développeur — jamais i18n::Tr.
static_assert(IM_ARRAYSIZE(kIfaceSections) == MoonlightUi::kIfaceCount,
              "kIfaceSections doit couvrir exactement l'enum IfaceSection");

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

const char* SectionLabel(int section) {
  for (const IfaceEntry& entry : kIfaceSections)
    if (entry.id == section) return entry.label;
  return nullptr;
}

int SectionByKey(const char* key) {
  if (key == nullptr || key[0] == '\0') return -1;
  for (const IfaceEntry& entry : kIfaceSections)
    if (std::strcmp(entry.key, key) == 0) return entry.id;
  return -1;
}

const char* DestLabel(const char* key) {
  const int section = SectionByKey(key);
  if (section >= 0) return SectionLabel(section);
  if (const PanelHeader* header = HeaderByKey(key)) return header->label;
  // Troisième étage : les onglets de Game Settings. Ils vivent dans leur propre
  // fenêtre, mais partagent l'espace de clés — un lien de réglage ne dit pas
  // QUELLE fenêtre il ouvre, seulement CE qu'il désigne.
  return gslink::LabelByKey(key);
}

// ── L'en-tête qui sait se lier ───────────────────────────────────────────────
// Remplace `CollapsingHeader(i18n::Tr("…"))` aux sept en-têtes du panneau. Il
// porte les deux bouts du lien : le geste qui le POSE (Maj + clic) et le saut qui
// l'HONORE (un lien reçu déplie l'en-tête et scrolle dessus).
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

}  // namespace iface

// ── En-tête « Interface de jeu » ─────────────────────────────────────────────
// Navigation latérale + les 13 sections de configuration. C'était le bloc dominant
// d'OnRenderUI (742 lignes sur 1702) : une nav, puis une cascade de 11 tests sur
// iface_nav_, chacun avec sa propre variable changed homonyme.
// La table kIfaceSections (source unique libellé + identifiant, cf. chantier 5)
// vit ici, au plus près de son usage.
void MoonlightUi::DrawInterfacePanel() {
  // Le dépliage sur saut (bullet de barre de titre d'une fenêtre Bourgeon, lien de
  // réglage reçu dans le chat) et le Maj + clic vivent dans LinkableHeader : cet
  // en-tête-ci est un en-tête du panneau comme les six autres.
  if (iface::LinkableHeader("interface")) {
    PushStyleCompact();
    bool changed = false;

    // ── L'interrupteur du groupe, ICI et nulle part ailleurs ─────────────────
    // Il vivait dans cinq sections à la fois (Barre d'action, Storage, Inventaire,
    // Cart, Banque) : cinq cases synchronisées pour un seul état, chacune donnant
    // l'impression de ne concerner que sa fenêtre alors qu'elle en basculait douze.
    // Sa place est en tête de l'en-tête, au-dessus de la navigation — c'est le
    // réglage dont dépendent sept des quinze sections.
    bool modern = ModernInterfaceEnabled();
    changed |= DrawModernInterfaceCheckbox(
        &modern,
        i18n::Tr("Les réglages propres à chaque fenêtre restent dans leur section "
        "ci-dessous (Inventaire, Cart, Storage, Banque, Refine, Fabrication, "
        "Barre d'action). Ils sont grisés tant que cette case est décochée : "
        "sans elle, ces fenêtres n'existent pas."));

    // ── Langue, police et échelle de l'interface ─────────────────────────────
    // Les trois réglages qui s'appliquent à TOUTE l'interface Bourgeon, groupés
    // ici. Les deux derniers vivaient dans la section « Skin RO » de la
    // navigation ci-dessous, où il fallait deviner qu'un skin change aussi la
    // police et la taille du texte — ils ne parlent pas d'habillage de fenêtre,
    // ils parlent de l'interface entière, comme la langue.
    //
    // Le libellé de la langue se traduit LUI-MÊME, et il dit « de l'interface » :
    // c'est le seul réglage qu'un joueur doit pouvoir retrouver quand l'interface
    // est déjà dans une langue qu'il ne lit pas, et il ne touche PAS à la langue
    // du serveur (noms d'objets, messages) — ce que le libellé nu laissait croire.
    {
      // COPIE et non référence : i18n::SetLanguage écrit dans la chaîne globale
      // au milieu de la boucle ci-dessous. Une référence changerait donc de
      // valeur en cours de route, et les entrées suivantes se compareraient au
      // code qu'on vient tout juste de poser.
      const std::string current = i18n::LanguageCode();
      ImGui::SetNextItemWidth(ro::Px(160.0f));
      // `TrId` et non `Tr` : RoBeginCombo fait `PushID(label)`, donc un libellé
      // traduit donnerait un widget différent à chaque langue. C'est le premier
      // cas du chantier, et il sera la règle pour tout ce qui porte un état.
      if (ro::RoBeginCombo(i18n::TrId("Langue de l'interface", "bourgeon_language"),
                           i18n::LabelOf(current))) {
        for (const i18n::Language& language : i18n::AvailableLanguages()) {
          const bool selected = (current == language.code);
          // Une langue sans catalogue reste VISIBLE, mais inerte. La masquer
          // laisserait croire que Bourgeon ne la connaît pas ; la griser dit ce
          // qui est vrai — elle est prévue, son fichier n'est pas là.
          if (!language.available) ImGui::BeginDisabled();
          if (ImGui::Selectable(language.label, selected) && !selected) {
            changed |= i18n::SetLanguage(language.code);
            // La table de messages du CLIENT suit la même langue que notre
            // interface. Sûr en pleine frame : le rechargement n'efface AUCUNE
            // chaîne déjà rendue (elles vivent dans un stockage qu'on ne libère
            // jamais), donc un libellé « en vol » reste valide.
            msgoverride::Reload();
          }
          if (!language.available) ImGui::EndDisabled();
          if (selected) ImGui::SetItemDefaultFocus();
        }
        // 🔴 `ro::RoEndCombo`, PAS `ImGui::EndCombo` : RoBeginCombo n'appelle pas
        // BeginCombo, il dessine le champ à la main et ouvre un `ImGui::BeginPopup`.
        // Le refermer avec EndCombo laisserait cinq PushStyleColor et un PushID
        // sur la pile.
        ro::RoEndCombo();
      }
      SameLine(); HelpMarker(
          i18n::Tr("Langue de l'interface Bourgeon. Le jeu lui-même (noms d'objets, "
          "descriptions, messages du serveur) n'est pas concerné.\n"
          "Une langue grisée est connue mais son fichier de traduction est "
          "absent de SaveData\\lang\\."));

      // Le compteur de textes non traduits, staff uniquement : c'est un outil de
      // TRADUCTION, pas un réglage. Un joueur n'a rien à faire d'un décompte
      // qu'il ne peut pas réduire, et le voir donnerait l'impression d'une
      // interface cassée là où elle se contente de retomber en français.
      if (IsStaff() && i18n::MissingCount() > 0) {
        ImGui::TextDisabled(i18n::Tr("%zu textes sans traduction"), i18n::MissingCount());
        SameLine();
        if (ro::RoSmallButton(i18n::Tr("Exporter"))) {
          std::string exported_path;
          // Le chemin est journalisé dans les DEUX cas : en échec, c'est lui qui
          // dit pourquoi (dossier absent, fichier verrouillé), ce qu'un simple
          // « erreur » ne dirait pas.
          //
          // Deux appels plutôt qu'un gabarit choisi par ternaire : le format de
          // spdlog est vérifié à la COMPILATION, et une expression ternaire n'en
          // est pas une constante — ça ne compile pas.
          if (i18n::ExportMissing(&exported_path)) {
            LogDiag("[i18n] gabarit écrit : {}", exported_path);
          } else {
            LogDiag("[i18n] gabarit NON écrit : {}", exported_path);
          }
        }
        SameLine(); HelpMarker(
            i18n::Tr("Écrit les textes rencontrés depuis le lancement et absents du "
            "catalogue dans SaveData\\lang\\<langue>.missing.yaml, prêts à "
            "traduire. N'y figure que ce qui a été AFFICHÉ : ouvre les fenêtres "
            "concernées avant d'exporter."));
      }

      // ── La police de toute l'interface ─────────────────────────────────────
      // Elle s'enregistre TOUTE SEULE (fichier de démarrage) : rien à remonter
      // dans `changed`, qui ne parle que de bourgeon_settings.yaml.
      ro::DrawUiFontCombo(i18n::TrId("Police de l'interface", "bourgeon_ui_font"),
                          ro::Px(160.0f));
      SameLine(); HelpMarker(
          i18n::Tr("Police de toute l'UI ImGui. Le gras et l'italique suivent la "
          "famille choisie, ainsi que la chatbox réglée sur « Système ».\n\n"
          "Malgun Gothic est la seule à couvrir le coréen : avec une autre, les "
          "chemins de fichiers du jeu (réglage de débogage) sortent en carrés.\n"
          "ProggyClean est la police intégrée d'ImGui, minuscule et sans accents "
          "élégants."));

      // ── L'échelle de toute l'interface ─────────────────────────────────────
      // Comme la police : réglage d'avant-jeu, qui s'enregistre lui-même.
      ro::DrawUiScaleCombo(i18n::TrId("Échelle de l'interface", "bourgeon_ui_scale"),
                           ro::Px(160.0f));
      SameLine(); HelpMarker(
          i18n::Tr("Agrandit TOUTE l'interface Bourgeon — texte, fenêtres, boutons, "
          "cadres — sans toucher au jeu lui-même. Pour les grands écrans très "
          "définis (ultrawide, 4K), où une interface calibrée en pixels devient "
          "minuscule.\n\n"
          "Les fenêtres déjà ouvertes gardent la taille qu'on leur avait donnée : "
          "les redimensionner une fois suffit.\n\n"
          "Sans effet en DirectX 7 (le réglage y est grisé) : ce mode ne sait pas "
          "redessiner les lettres à une autre taille, il ne ferait que les étirer."));
    }

    changed |= ro::RoCheckbox(i18n::Tr("Grille d'alignement"), &grid_.show);
    SameLine(); HelpMarker(
        i18n::Tr("Affiche une grille plein écran pour aligner ton interface "
        "(comme les add-ons d'interface de WoW)."));
    ImGui::SetNextItemWidth(ro::Px(160.0f));
    changed |= WheelSliderInt(i18n::Tr("Taille grille"), &grid_.cell_size_px, 4, 128);
    changed |= ro::RoCheckbox(i18n::Tr("Aimanter à la grille"), &grid_.snap);
    SameLine(); HelpMarker(
        i18n::Tr("Les barres et les icônes s'alignent sur les cellules de la grille "
        "pendant le déplacement et le redimensionnement."));
    changed |= ColorEdit4WithAlphaBar(i18n::Tr("Couleur grille"), grid_.color);

    // (Inventaire, Cart et Storage : tout est regroupé dans leurs sections
    // dédiées, sous la navigation latérale ci-dessous.)

    // (Cash shop, boutiques PNJ, échoppe joueur — vente ET échoppe d'achat —,
    // courrier RODEX et feuille de personnage (Alt+F) : AUCUNE case ici. Tous font
    // partie de l'interrupteur GLOBAL « Interface moderne » (liste dans
    // SetModernInterface, moonlight_ui.h), porté par les sections Inventaire,
    // Cart, Storage et Barre d'action.
    // Ils achètent, vendent, joignent ou équipent VERS et DEPUIS l'inventaire :
    // moderne d'un côté et natif de l'autre, les objets ne se glissent plus. Une
    // case isolée rouvrirait donc exactement le mixe qu'on a supprimé.)

    // Bouton « Signaler un bug » (desc item/skill + dialogue PNJ + raccourci).
    if (auto* br = Bourgeon::Instance().bug_report()) {
      changed |= ro::RoCheckbox(i18n::Tr("Afficher le bouton « Signaler un bug »"), &br->enabled());
      SameLine(); HelpMarker(
          i18n::Tr("Affiche le bouton de rapport de bug dans les fenêtres de "
          "description (item/skill) et le dialogue PNJ, et active l'action clavier "
          "« Signaler un bug » (Ctrl+Alt+B par défaut, réglable dans les "
          "raccourcis). Décoche pour tout désactiver."));
    }

    if (changed) SaveSettings();

    // Navigation latérale (liste à gauche, contenu à droite). L'entrée active est
    // un MEMBRE (iface_nav_) : OpenInterfaceSection la pilote depuis le bullet de
    // barre de titre d'une autre fenêtre Bourgeon. La table des sections
    // (kIfaceSections) vit en tête de ce fichier — elle sert aussi aux liens.

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
    const ImGuiStyle& st = ImGui::GetStyle();
    static float s_labels_w    = 0.0f;   // largeur du plus long libellé, en px
    static ImFont* s_labels_font = nullptr;
    static float s_labels_size = 0.0f;
    static unsigned s_labels_lang = 0u;  // i18n::CatalogEpoch() de la mesure
    if (s_labels_font != ImGui::GetFont() ||
        s_labels_size != ImGui::GetFontSize() ||
        s_labels_lang != i18n::CatalogEpoch()) {
      s_labels_w = 0.0f;
      for (const IfaceEntry& entry : kIfaceSections)
        s_labels_w = (std::max)(s_labels_w, ImGui::CalcTextSize(i18n::Tr(entry.label)).x);
      s_labels_font = ImGui::GetFont();
      s_labels_size = ImGui::GetFontSize();
      s_labels_lang = i18n::CatalogEpoch();
    }
    const float nav_w_wanted =
        s_labels_w + st.WindowPadding.x * 2.0f + st.FramePadding.x * 2.0f;
    const float nav_w_min = ImGui::GetFontSize() * 5.0f;
    const float nav_w_max =
        (std::max)(nav_w_min, ImGui::GetContentRegionAvail().x * 0.4f);
    const float nav_w =
        (std::min)((std::max)(nav_w_wanted, nav_w_min), nav_w_max);
    // La borne a mordu : les libellés ne tiennent pas et ImGui les coupe NET, sans
    // même une ellipse pour le dire. C'est ce qui arrive à une fenêtre étroite dans
    // une langue verbeuse — « Suivi de quête » fait 14 signes, « Seguimiento de
    // misiones » en fait 23. On ne triche pas sur la largeur (le contenu à droite
    // en a besoin), on rend le libellé lisible autrement : en infobulle.
    const bool nav_clipped = nav_w < nav_w_wanted;
    const float nav_h = ImGui::GetTextLineHeightWithSpacing() * kIfaceCount
                      + st.WindowPadding.y * 2.0f;

    ImGui::BeginChild("iface_nav", ImVec2(nav_w, nav_h), ImGuiChildFlags_Borders);
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
    for (const IfaceEntry& entry : kIfaceSections) {
      const bool selected = ImGui::Selectable(i18n::Tr(entry.label),
                                              iface_nav_ == entry.id);
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
        iface_nav_ = entry.id;
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
      if ((link_posts || nav_clipped) &&
          links::HoveredForLinkTooltip()) {
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
    ImGui::BeginChild("iface_content", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::PushTextWrapPos(0.0f);  // wrap le texte à la largeur du child
    {
      PushItemWidth(160.0f);

      // ── Sections qui n'existent QUE si le groupe « Interface moderne » l'est ─
      //
      // Ces sept-là ne règlent que des fenêtres ImGui : groupe coupé, elles
      // n'existent pas et leurs options ne changent rien. Un réglage sans effet est
      // un piège — on le grise, plutôt que de laisser croire qu'il agit.
      //
      // Le test est fait ICI, au site d'appel unique, et non dans chacun des sept
      // DrawSettings() : un BeginDisabled/EndDisabled à apparier dans sept plugins
      // finit toujours par se dépareiller sur un chemin de sortie.
      const bool needs_modern =
          iface_nav_ == kIfaceSkillBar  || iface_nav_ == kIfaceStorage ||
          iface_nav_ == kIfaceInventory || iface_nav_ == kIfaceCart    ||
          iface_nav_ == kIfaceRefine    || iface_nav_ == kIfaceMakeItem  ||
          iface_nav_ == kIfaceMonsterInfo || iface_nav_ == kIfacePet;
      const bool locked = needs_modern && !ModernInterfaceEnabled();
      if (locked) {
        // 🔴 Un APERÇU, pas un cimetière. Ces sections sont la meilleure vitrine de
        // l'interface moderne : le joueur qui les parcourt doit pouvoir LIRE ce
        // qu'elle apporte et avoir envie d'essayer. On bloque donc l'interaction —
        // un réglage sans effet est un piège — mais sans éteindre le texte.
        //
        // Volontairement AVANT le BeginDisabled, pour rester pleinement lisible, et
        // avec l'interrupteur À PORTÉE : renvoyer le joueur chercher une case en
        // haut de page, c'est le perdre.
        // Ocre d'avertissement du projet, celui de la fabrication et du refine
        // (`IM_COL32(166, 102, 0)`) : le jaune vif d'une première rédaction passait
        // mal sur le gris clair du skin RO — trop criard pour une invitation, et
        // moins lisible qu'un ton sourd sur fond pâle.
        ImGui::TextColored(ImVec4(166 / 255.0f, 102 / 255.0f, 0.0f, 1.0f),
                           i18n::Tr("Aperçu — ces réglages appartiennent à l'interface "
                           "moderne, qui est désactivée."));
        if (ro::RoButton(i18n::Tr("Activer l'interface moderne"))) {
          SetModernInterface(true);
          SaveSettings();
        }
        ImGui::Spacing();
      }
      // Seule l'INTERACTION est coupée : les libellés et leurs infobulles restent
      // lisibles, la section garde donc sa valeur de vitrine.
      ImGui::BeginDisabled(locked);

      // ── Barre d'action ───────────────────────────────────────────────────
      if (iface_nav_ == kIfaceSkillBar)
      {
        if (auto* sb = Bourgeon::Instance().skill_bar())
          sb->DrawSettings();
        else
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
      }

      // ── Barres d'info (HUD bars + alignment grid) ────────────────────────
      // ── Status Portrait (head + pseudo + classe + niveau, indépendants) ──
      if (iface_nav_ == kIfaceBasicInfo) {
        if (auto* basic_info = Bourgeon::Instance().basic_info()) {
          if (basic_info->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Barre d'incantation (CastBar) ────────────────────────────────────
      // Sa jumelle HUD (« Cast ») se règle avec les autres barres, dans la
      // section Basic Info : c'est la même famille de widgets, même verrou,
      // même aimantation. Ici on ne traite que celles des entités.
      if (iface_nav_ == kIfaceCastBar) {
        if (auto* cast = Bourgeon::Instance().cast_bar())
          cast->DrawSettings();
        else
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
      }

      // ── Fenêtre de cible (TargetFrame) ───────────────────────────────────
      // Elle suit la sélection native — la même que la petite flèche blanche du
      // jeu — et complète ce que le client ignore par une requête serveur.
      if (iface_nav_ == kIfaceTargetFrame) {
        if (auto* target_frame = Bourgeon::Instance().target_frame()) {
          if (target_frame->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }

        // ── Les ÉTATS de la cible ──────────────────────────────────────────
        //
        // 🔴 LE CADRE EST RANGÉ AVEC LES AUTRES. Sa case, sa position, son fond
        // et son liseré sont dans « Cadres » ci-dessus, et il obéit au verrou de
        // la fenêtre — comme le portrait, le nom ou les jauges. Il avait sa
        // propre géométrie et son propre verrou : deux cases « Verrouiller »
        // dans le même panneau, et un cadre qui ignorait l'aimantation de ses
        // frères alors qu'il décrit la MÊME cible.
        //
        // Ne reste donc ici que ce qui décrit son CONTENU, et n'a d'équivalent
        // dans aucun autre cadre.
        if (auto* tf = Bourgeon::Instance().target_frame()) {
          bool changed = false;
          SeparatorText(i18n::Tr("États de la cible"));

          // ⚠ PAS dans `changed` : l'aperçu ne se persiste pas.
          ro::RoCheckbox(i18n::Tr("Aperçu (faux statuts)"), &tf->st_preview_);
          SameLine(); HelpMarker(i18n::Tr(
              "Remplit l'affichage de faux états, aux durées étagées, le "
              "temps de le régler — sans attendre d'en avoir de "
              "vrais.\n\nNe se garde pas d'une session à l'autre."));

          const bool on = tf->elems_[TargetFrame::kElemStatus].show;
          if (!on) {
            ImGui::TextDisabled("%s", i18n::Tr(
                "Le cadre « États » est décoché dans Cadres, ci-dessus."));
          }
          ImGui::BeginDisabled(!on);

          // ── Icônes ────────────────────────────────────────────────────────
          SeparatorText(i18n::Tr("Icônes"));
          changed |= WheelSliderInt(i18n::Tr("Taille"), &tf->st_icon_px(),
                                    12, 48, "%d px");
          changed |= WheelSliderInt(i18n::Tr("Écart"), &tf->st_gap_px(),
                                    0, 12, "%d px");
          changed |= WheelSliderInt(i18n::Tr("Nombre au plus"),
                                    &tf->st_max_icons(), 1, 40, "%d");
          SameLine(); HelpMarker(i18n::Tr(
              "Au-delà, les états en trop sont écartés — ce sont les DERNIERS du "
              "rangement ci-dessous, qui décide donc de ce qu'on perd."));
          changed |= WheelSliderInt(i18n::Tr("Lignes d'icônes"),
                                    &tf->st_rows(), 1, 6, "%d");
          SameLine(); HelpMarker(i18n::Tr(
              "Le cadre se replie déjà sur sa largeur : ce réglage force en plus "
              "un nombre fixe par rangée, pour un cadre haut et étroit plutôt "
              "que long et plat."));
          {
            // ⚠ Items NUS : RoCombo traduit à la lecture.
            const char* kSorts[] = {"Ordre d'arrivée", "Bientôt fini d'abord",
                                    "Plus long d'abord"};
            changed |= ro::RoCombo(i18n::Tr("Rangement"), &tf->st_sort(),
                                   kSorts, IM_ARRAYSIZE(kSorts));
          }
          SameLine(); HelpMarker(i18n::Tr(
              "« Bientôt fini d'abord » met en tête ce qu'il faudra relancer, ou "
              "ce qu'il suffit d'attendre.\n\n"
              "Un état SANS durée (permanent) n'a pas sa place dans un tri par "
              "durée : il va en fin dans les deux sens, pour ne pas chasser de "
              "l'écran ce qui presse."));

          // ── Compte à rebours ──────────────────────────────────────────────
          SeparatorText(i18n::Tr("Compte à rebours"));
          changed |= ro::RoCheckbox(i18n::Tr("Afficher le temps restant"),
                                    &tf->st_show_time_);
          SameLine(); HelpMarker(i18n::Tr(
              "Sous chaque icône. Un état permanent n'en porte pas : un « 0 » "
              "sous un buff qui ne finit jamais le ferait croire terminé."));
          ImGui::BeginDisabled(!tf->st_show_time_);
          Indent();
            changed |= WheelSliderInt(i18n::Tr("Taille du texte"),
                                      &tf->st_time_px(), 7, 20, "%d px");
          Unindent();
          ImGui::EndDisabled();

          // ── Écoulement ────────────────────────────────────────────────────
          SeparatorText(i18n::Tr("Écoulement"));
          {
            const char* kSweeps[] = {"Aucun", "Balayage horaire",
                                     "Voile descendant"};
            changed |= ro::RoCombo(i18n::Tr("Grisage de la case"),
                                   &tf->st_sweep(), kSweeps,
                                   IM_ARRAYSIZE(kSweeps));
          }
          SameLine(); HelpMarker(i18n::Tr(
              "La case s'assombrit à mesure que l'état s'écoule : on voit "
              "d'un coup d'œil lesquels sont près de tomber.\n\n"
              "⚠ Un état sans échéance n'est jamais voilé : il n'a pas de part "
              "écoulée, et le griser à moitié le ferait croire à mi-course."));
          ImGui::BeginDisabled(tf->st_sweep() == statuscell::kSweepNone);
          Indent();
            changed |= RoColorSwatch(i18n::Tr("Voile"), tf->st_col_sweep_);
          Unindent();
          ImGui::EndDisabled();

          ImGui::EndDisabled();
          if (changed) SaveSettings();
        }
      }

      // ── Groupe / Amis : la FENÊTRE (PartyFriendWindow) ───────────────────
      // À distinguer de la grille juste en dessous : celle-ci REMPLACE la fenêtre
      // native 0x45, l'autre est un HUD. Elles lisent la même source mais ne se
      // règlent pas ensemble — on ne consulte pas une liste comme on surveille
      // des barres de vie.
      if (iface_nav_ == kIfacePartyFriend) {
        auto* pfw = Bourgeon::Instance().party_friend_window();
        if (pfw == nullptr) {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        } else {
          bool changed = false;
          SeparatorText(i18n::Tr("Fenêtre"));
          changed |= ro::RoCheckbox(i18n::Tr("Verrouiller la taille"),
                                    &pfw->lock_size());
          SameLine(); HelpMarker(i18n::Tr(
              "Empêche le redimensionnement, PAS le déplacement : la fenêtre "
              "se déplace toujours par sa barre de titre.\n\n"
              "Une fois la largeur réglée, viser le bord au lieu du titre la "
              "dérègle d'un pixel et recompose les lignes sous la souris."));
          SeparatorText(i18n::Tr("Contenu d'une ligne"));
          changed |= ro::RoCheckbox(i18n::Tr("Icône de classe"),
                                    &pfw->show_job_icon_);
          SameLine(); HelpMarker(i18n::Tr(
              "L'art du client, à gauche du nom. Éteinte, la ligne se resserre — "
              "utile sur une fenêtre étroite."));
          {
            // ⚠ Items NUS : RoCombo traduit à la lecture (cf. plus bas).
            const char* kHeads[] = {"Aucune", "Groupe", "Amis", "Les deux"};
            changed |= ro::RoCombo(i18n::Tr("Tête du personnage"),
                                   &pfw->head_mode_, kHeads,
                                   IM_ARRAYSIZE(kHeads));
          }
          SameLine(); HelpMarker(i18n::Tr(
              "La tête du personnage à la place de l'icône de classe, comme la "
              "fenêtre des membres de guilde.\n\n"
              "Pour qui est à l'écran, elle vient de son sprite et suit un "
              "changement de coiffure aussitôt. Pour les autres, le serveur la "
              "donne sur demande — un joueur HORS LIGNE n'en a pas, et dans "
              "l'onglet Amis la ligne reste alors sans vignette."));
          changed |= ro::RoCheckbox(i18n::Tr("Niveau devant le nom"),
                                    &pfw->show_level_);
          {
            const char* kMapModes[] = {"Nom complet",
                                       "Nom court",
                                       "Masquée"};
            changed |= ro::RoCombo(i18n::Tr("Carte"), &pfw->map_mode_,
                                    kMapModes, IM_ARRAYSIZE(kMapModes));
          }
          SameLine(); HelpMarker(i18n::Tr(
              "Le client écrit « Gonryun, the Hermit Land (Kunlun) » là où "
              "« Gonryun » suffit à se repérer — et le nom complet pousse le "
              "reste de la ligne hors d'une fenêtre étroite.\n\n"
              "Masquée, la ligne ne porte plus que le nom. L'infobulle au "
              "survol continue de donner la carte entière."));
          changed |= ro::RoCheckbox(i18n::Tr("Infobulle au survol"),
                                    &pfw->show_tooltip_);
          SameLine(); HelpMarker(i18n::Tr(
              "Au survol d'une ligne : la classe, la carte complète, la position "
              "et les PV/SP chiffrés — ce qui ne tient pas dans la ligne."));
          changed |= ro::RoCheckbox(i18n::Tr("Clic gauche : cibler le membre"),
                                    &pfw->click_targets_);
          SameLine(); HelpMarker(i18n::Tr(
              "Comme une tuile du HUD en grille : la ligne devient une cible "
              "cliquable, et un liseré blanc marque la cible courante.\n\n"
              "Sans effet sur un membre hors ligne ou hors de portée — ses PV et "
              "sa position viennent de son sprite, et il n'y en a pas.\n\n"
              "Demande le mode Ciblage, qui a son propre panneau : c'est lui qui "
              "décide qu'une cible existe."));
          changed |= ro::RoCheckbox(i18n::Tr("Buffs et debuffs"),
                                    &pfw->show_buffs_);
          SameLine(); HelpMarker(i18n::Tr(
              "Les icônes d'état du membre, à gauche de sa pastille.\n\n"
              "⚠ Une ligne SANS icône ne veut pas dire « aucun buff ». Le serveur "
              "ne diffuse ces états qu'aux joueurs qui VOIENT le personnage, "
              "et seulement au moment où ils COMMENCENT : un joueur déjà "
              "béni quand il entre à l'écran arrive sans rien.\n\n"
              "Ce qui s'affiche ici est donc ce qui est TOMBÉ sous vos yeux, "
              "pas l'état complet du personnage."));
          if (pfw->show_buffs_) {
            // ⚠ PAS dans `changed` : l'aperçu ne se persiste pas, et le
            // marquer réécrirait le fichier de réglages à chaque clic.
            ro::RoCheckbox(i18n::Tr("Aperçu (faux statuts)"), &pfw->buff_preview_);
            SameLine(); HelpMarker(i18n::Tr(
                "Remplit l'affichage de faux états, aux durées étagées, le "
                "temps de le régler — sans attendre d'en avoir de "
                "vrais.\n\nNe se garde pas d'une session à l'autre."));
            // 🔴 PushID : l'identifiant ImGui d'un widget est son LIBELLÉ,
            // donc sa TRADUCTION. « Taille des icônes » (états) et « Taille
            // de l'icône » (classe) sont distincts en français et deviennent
            // tous deux « Icon size » en anglais : deux widgets, un seul
            // identifiant, et ImGui lève une erreur en plein jeu.
            //
            // ⚠ Le code relu en français ne montre RIEN — c'est le catalogue
            // qui crée la collision, et il peut la recréer demain sur un
            // autre couple. D'où une isolation par BLOC, pas un libellé
            // rebaptisé qui ne protégerait que ce cas-ci.
            ImGui::PushID("status_icons");
            changed |= mui::WheelSliderInt(i18n::Tr("Taille des icônes"),
                                           &pfw->buff_px(), 8, 32, "%d px");
            changed |= mui::WheelSliderInt(i18n::Tr("Icônes au plus"),
                                           &pfw->buff_max(), 1, 24, "%d");
            changed |= mui::WheelSliderInt(i18n::Tr("Lignes d'icônes"),
                                           &pfw->buff_rows(), 1, 4, "%d");
            SameLine(); HelpMarker(i18n::Tr(
                "Une rangée unique s'allonge jusqu'à manger la place du nom ; "
                "en deux lignes, le même nombre d'états tient sur moitié moins "
                "de largeur.\n\n"
                "Le compte maximum se répartit entre les lignes — six icônes "
                "sur deux lignes font trois par ligne."));
            changed |= ro::RoCheckbox(i18n::Tr("Temps restant sous l'icône"),
                                      &pfw->buff_time_);
            {
              const char* kSweeps[] = {"Aucun",
                                       "Balayage horaire",
                                       "Voile descendant"};
              changed |= ro::RoCombo(i18n::Tr("Grisage de la case"),
                                     &pfw->buff_sweep(), kSweeps,
                                     IM_ARRAYSIZE(kSweeps));
            }
            SameLine(); HelpMarker(i18n::Tr(
                "La case s'assombrit à mesure que l'état s'écoule.\n\n"
                "⚠ La durée d'origine n'est portée par AUCUN paquet : elle est "
                "exacte quand on a vu l'état commencer, et repart de « plein » "
                "quand on le découvre en route."));
            ImGui::PopID();
          }

          // ── Densité ───────────────────────────────────────────────────────
          SeparatorText(i18n::Tr("Densité"));
          changed |= WheelSliderInt(i18n::Tr("Taille de l'icône"),
                                    &pfw->icon_px_, 16, 56, "%d px");
          changed |= WheelSliderInt(i18n::Tr("Espace entre les lignes"),
                                    &pfw->row_spacing_, 0, 12, "%d px");
          SameLine(); HelpMarker(i18n::Tr(
              "À zéro, les lignes se touchent : c'est le réglage qui gagne le "
              "plus de hauteur sur un groupe nombreux."));

          // ── Jauges ────────────────────────────────────────────────────────
          // PV et SP ensemble : ce sont deux jauges, elles partagent forme,
          // taille et placement du texte. Les séparer obligeait à faire des
          // allers-retours entre deux blocs pour un réglage commun.
          SeparatorText(i18n::Tr("Jauges"));
          changed |= ro::RoCheckbox(i18n::Tr("Barre de vie"),
                                    &pfw->show_hp_bar_);
          {
            const char* kHpModes[] = {
                "Rien", "Chiffres",
                "Pourcentage", "Chiffres et pourcentage"};
            changed |= ro::RoCombo(i18n::Tr("Texte des PV"),
                                   &pfw->hp_text_mode_, kHpModes,
                                   IM_ARRAYSIZE(kHpModes));
          }
          changed |= ro::RoCheckbox(i18n::Tr("Barre de SP"), &pfw->show_sp_);
          {
            const char* kSpModes[] = {
                "Rien", "Chiffres",
                "Pourcentage", "Chiffres et pourcentage"};
            changed |= ro::RoCombo(i18n::Tr("Texte du SP"),
                                    &pfw->sp_text_mode_, kSpModes,
                                    IM_ARRAYSIZE(kSpModes));
          }
          SameLine(); HelpMarker(i18n::Tr(
              "Le SP d'un autre joueur ne circule dans AUCUN paquet du jeu : il "
              "est demandé au serveur, membre par membre. Il n'apparaît donc que "
              "pour ceux qui sont à portée de vue, et coûte un peu de réseau — "
              "d'où le défaut éteint."));
          changed |= WheelSliderInt(i18n::Tr("Largeur des jauges"),
                                    &pfw->bar_w_, 20, 260, "%d px");
          changed |= WheelSliderInt(i18n::Tr("Hauteur d'une jauge"),
                                    &pfw->bar_h_, 3, 20, "%d px");
          changed |= ro::RoCheckbox(i18n::Tr("Jauges collées l'une sous l'autre"),
                                    &pfw->bars_stacked_);
          SameLine(); HelpMarker(i18n::Tr(
              "PV au-dessus, SP juste dessous, sans rien entre les deux.\n\n"
              "Le texte passe alors DANS les jauges : à côté, il pousserait la "
              "seconde d'une hauteur de ligne et elles ne seraient plus collées."));
          changed |= ro::RoCheckbox(i18n::Tr("Texte dans les jauges"),
                                    &pfw->text_in_bars_);
          SameLine(); HelpMarker(i18n::Tr(
              "Centré sur la jauge, avec une ombre pour rester lisible sur le "
              "vert comme sur le fond. La ligne ne s'allonge pas — mais il faut "
              "une jauge assez haute."));
          changed |= WheelSliderInt(i18n::Tr("Taille du texte des jauges"),
                                    &pfw->text_px_, 0, 20, "%d px");
          SameLine(); HelpMarker(i18n::Tr(
              "À zéro, celle de l'interface."));

          if (changed) SaveSettings();
        }
      }

      // ── Groupe : la grille (PartyFrames) ─────────────────────────────────
      // Un HUD de raid frames — une tuile par membre, dont la couleur DIT l'état.
      // HORS du groupe « Interface moderne » : il ne remplace aucune fenêtre, il
      // ajoute ce que le client n'a pas sous cette forme, et il a du sens même en
      // interface native.
      if (iface_nav_ == kIfacePartyFrames) {
        auto* pf = Bourgeon::Instance().party_frames();
        if (pf == nullptr) {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        } else {
          bool changed = false;
          changed |= ro::RoCheckbox(i18n::Tr("Afficher la grille de groupe"),
                                    &pf->enabled_);
          SameLine(); HelpMarker(i18n::Tr(
              "Remplace le HUD de groupe du client par une grille de tuiles : la "
              "barre de vie EST le fond de la tuile, et sa couleur dit l'état du "
              "membre. Le HUD d'origine est masqué tant que cette grille est "
              "active."));
          if (!pf->enabled_) ImGui::BeginDisabled();

          changed |= ro::RoCheckbox(i18n::Tr("Verrouiller la position"),
                                    &pf->locked_);
          SameLine(); HelpMarker(i18n::Tr(
              "Fige le cadre et laisse passer les clics vers le jeu.\n\n"
              "Maintenir MAJ, CURSEUR SUR LA GRILLE, la déverrouille le temps "
              "d'un déplacement : pas besoin de revenir décocher ici.\n\n"
              "Ailleurs à l'écran, MAJ garde son rôle habituel (attaque forcée) "
              "— la grille ne reprend la souris que sous le curseur."));

          // ── Disposition ───────────────────────────────────────────────────
          SeparatorText(i18n::Tr("Disposition"));
          changed |= WheelSliderInt(i18n::Tr("Colonnes"), &pf->columns_, 1, 6,
                                    "%d");
          SameLine(); HelpMarker(i18n::Tr(
              "1 colonne donne une liste, comme le HUD d'origine ; 2 ou 3 donnent "
              "la grille compacte des raid frames."));
          changed |= WheelSliderInt(i18n::Tr("Largeur des tuiles"),
                                    &pf->tile_w_, 60, 400, "%d px");
          changed |= WheelSliderInt(i18n::Tr("Hauteur des tuiles"),
                                    &pf->tile_h_, 18, 80, "%d px");
          changed |= WheelSliderInt(i18n::Tr("Espacement"), &pf->gap_, 0, 12,
                                    "%d px");
          SameLine(); HelpMarker(i18n::Tr(
              "La taille du cadre se DÉDUIT de ces valeurs : c'est la tuile qui "
              "commande.\n\n"
              "Tirer le cadre par sa poignée n'agit donc pas sur sa taille mais "
              "sur le nombre de COLONNES : l'élargir en ajoute, le rétrécir en "
              "retire. La hauteur, elle, suit le nombre de membres."));

          // ── Contenu d'une tuile ───────────────────────────────────────────
          SeparatorText(i18n::Tr("Contenu"));
          changed |= ro::RoCheckbox(i18n::Tr("Icône de classe"),
                                    &pf->show_job_icon_);
          SameLine(); HelpMarker(i18n::Tr(
              "L'art du client. C'est ce qui rend une grille lisible d'un coup "
              "d'œil : on reconnaît le soigneur à sa silhouette, pas à son nom."));
          changed |= ro::RoCheckbox(i18n::Tr("M'inclure dans la grille"),
                                    &pf->show_self_);
          changed |= ro::RoCheckbox(i18n::Tr("Garder les membres hors ligne"),
                                    &pf->show_offline_);
          changed |= ro::RoCheckbox(i18n::Tr("Afficher le niveau"),
                                    &pf->show_level_);
          {
            // Quatre façons d'écrire les PV. Le pourcentage seul est souvent le
            // plus lisible en combat : on compare des membres entre eux, on ne
            // lit pas des totaux.
            const char* kHpModes[] = {
                "Rien", "Chiffres",
                "Pourcentage", "Chiffres et pourcentage"};
            changed |= ro::RoCombo(i18n::Tr("Points de vie"),
                                    &pf->hp_text_mode_, kHpModes,
                                    IM_ARRAYSIZE(kHpModes));
          }
          changed |= WheelSliderInt(i18n::Tr("Taille du texte"), &pf->text_px_,
                                    8, 28, "%d px");
          SameLine(); HelpMarker(i18n::Tr(
              "Indépendante de la police des fenêtres : une grille se lit en "
              "périphérie de l'écran, pas de face."));

          changed |= ro::RoCheckbox(i18n::Tr("Infobulle au survol"),
                                    &pf->show_tooltip_);
          SameLine(); HelpMarker(i18n::Tr(
              "Le texte d'une tuile est DÉCOUPÉ à ses bords : sur une grille "
              "serrée, il ne reste parfois que les premières lettres d'un nom. "
              "L'infobulle le redonne en entier, avec la classe, la carte et les "
              "PV/SP chiffrés."));
          changed |= ro::RoCheckbox(i18n::Tr("Barre de SP"), &pf->show_sp_);
          SameLine(); HelpMarker(i18n::Tr(
              "Le SP d'un autre joueur ne circule dans AUCUN paquet du jeu : il "
              "est demandé au serveur membre par membre. Il n'apparaît donc que "
              "pour ceux qui sont à portée de vue."));
          if (!pf->show_sp_) ImGui::BeginDisabled();
          changed |= WheelSliderInt(i18n::Tr("Hauteur de la barre de SP"),
                                    &pf->sp_bar_h_, 2, 14, "%d px");
          if (!pf->show_sp_) ImGui::EndDisabled();

          // ── Interaction ───────────────────────────────────────────────────
          SeparatorText(i18n::Tr("Interaction"));
          changed |= ro::RoCheckbox(
              i18n::Tr("Lancer les sorts de soutien sur la tuile survolée"),
              &pf->cast_on_tile_);
          SameLine(); HelpMarker(i18n::Tr(
              "Une compétence de soutien (soin, buff) part sur le membre dont la "
              "tuile est sous le curseur : la touche arme, la tuile désigne. Le "
              "liseré blanc montre qui sera visé.\n\n"
              "La grille ne prend PAS le clic : marcher et frapper restent "
              "possibles curseur dessus.\n\n"
              "Les sorts d'ATTAQUE ne sont jamais concernés — viser un allié "
              "relève du PVP, que le jeu réserve au clic manuel."));
          if (!pf->cast_on_tile_) {
            SameLine();
            ImGui::TextDisabled("%s", i18n::Tr("(éteint)"));
          }

          changed |= ro::RoCheckbox(
              i18n::Tr("Cliquer les tuiles (cibler, menu du groupe)"),
              &pf->clickable_);
          SameLine(); HelpMarker(i18n::Tr(
              "Clic gauche : cibler le membre, comme un clic sur son "
              "personnage — sans effet si le mode Ciblage est éteint.\n"
              "Clic droit : le menu du personnage, celui de son sprite. Sur un "
              "membre qu'aucun sprite ne représente, un menu de repli propose "
              "ce qui voyage par nom : chuchoter, expulser.\n\n"
              "⚠ La grille PREND alors la souris sur toute sa surface : "
              "impossible de marcher ou de frapper en cliquant dessous. C'est "
              "le prix des gestes sur les tuiles — décoché, la grille se "
              "contente d'afficher et laisse tout passer."));

          changed |= ro::RoCheckbox(i18n::Tr("Buffs et debuffs"),
                                    &pf->show_buffs_);
          SameLine(); HelpMarker(i18n::Tr(
              "Les icônes d'état du membre, calées à droite de sa tuile. Le nom "
              "se découpe sur ce qu'elles laissent.\n\n"
              "⚠ Une tuile SANS icône ne veut pas dire « aucun buff ». Le serveur "
              "ne diffuse ces états qu'aux joueurs qui VOIENT le personnage, "
              "et seulement au moment où ils COMMENCENT : un joueur déjà "
              "béni quand il entre à l'écran arrive sans rien.\n\n"
              "Ce qui s'affiche ici est donc ce qui est TOMBÉ sous vos yeux, "
              "pas l'état complet du personnage."));
          if (pf->show_buffs_) {
            // ⚠ PAS dans `changed` : l'aperçu ne se persiste pas.
            ro::RoCheckbox(i18n::Tr("Aperçu (faux statuts)"), &pf->buff_preview_);
            SameLine(); HelpMarker(i18n::Tr(
                "Remplit l'affichage de faux états, aux durées étagées, le "
                "temps de le régler — sans attendre d'en avoir de "
                "vrais.\n\nNe se garde pas d'une session à l'autre."));

            // 🔴 PushID : l'identifiant ImGui d'un widget est son LIBELLÉ,
            // donc sa TRADUCTION. « Taille des icônes » (états) et « Taille
            // de l'icône » (classe) sont distincts en français et deviennent
            // tous deux « Icon size » en anglais : deux widgets, un seul
            // identifiant, et ImGui lève une erreur en plein jeu.
            //
            // ⚠ Le code relu en français ne montre RIEN — c'est le catalogue
            // qui crée la collision, et il peut la recréer demain sur un
            // autre couple. D'où une isolation par BLOC, pas un libellé
            // rebaptisé qui ne protégerait que ce cas-ci.
            ImGui::PushID("status_icons");
            changed |= mui::WheelSliderInt(i18n::Tr("Taille des icônes"),
                                           &pf->buff_px(), 6, 28, "%d px");
            changed |= mui::WheelSliderInt(i18n::Tr("Icônes au plus"),
                                           &pf->buff_max(), 1, 24, "%d");
            changed |= mui::WheelSliderInt(i18n::Tr("Lignes d'icônes"),
                                           &pf->buff_rows(), 1, 4, "%d");
            SameLine(); HelpMarker(i18n::Tr(
                "Une rangée unique s'allonge jusqu'à manger la place du nom ; "
                "en deux lignes, le même nombre d'états tient sur moitié moins "
                "de largeur.\n\n"
                "Le compte maximum se répartit entre les lignes — six icônes "
                "sur deux lignes font trois par ligne."));
            changed |= ro::RoCheckbox(i18n::Tr("Temps restant sous l'icône"),
                                      &pf->buff_time_);
            {
              const char* kSweeps[] = {"Aucun",
                                       "Balayage horaire",
                                       "Voile descendant"};
              changed |= ro::RoCombo(i18n::Tr("Grisage de la case"),
                                     &pf->buff_sweep(), kSweeps,
                                     IM_ARRAYSIZE(kSweeps));
            }
            SameLine(); HelpMarker(i18n::Tr(
                "La case s'assombrit à mesure que l'état s'écoule.\n\n"
                "⚠ La durée d'origine n'est portée par AUCUN paquet : elle est "
                "exacte quand on a vu l'état commencer, et repart de « plein » "
                "quand on le découvre en route."));
            ImGui::PopID();
          }

          // ── Couleurs ──────────────────────────────────────────────────────
          SeparatorText(i18n::Tr("Couleurs"));
          changed |= RoColorSwatch(i18n::Tr("Fond du cadre"), pf->col_frame_bg_);
          SameLine(); HelpMarker(i18n::Tr(
              "Le panneau qui porte les tuiles. Sans lui, une grille sombre sur "
              "une carte sombre devient impossible à distinguer du décor."));
          changed |= RoColorSwatch(i18n::Tr("Fond d'une tuile"), pf->col_tile_bg_);
          changed |= RoColorSwatch(i18n::Tr("Vie — haute"), pf->col_hp_high_);
          changed |= RoColorSwatch(i18n::Tr("Vie — moyenne"), pf->col_hp_mid_);
          changed |= RoColorSwatch(i18n::Tr("Vie — basse"), pf->col_hp_low_);
          changed |= RoColorSwatch(i18n::Tr("SP"), pf->col_sp_);
          changed |= RoColorSwatch(i18n::Tr("Texte"), pf->col_text_);
          changed |= RoColorSwatch(i18n::Tr("Texte — hors de portée"),
                                   pf->col_far_);
          SameLine(); HelpMarker(i18n::Tr(
              "Le membre est EN JEU, simplement trop loin pour que le client "
              "connaisse ses PV : il peut revenir à portée d'un instant à "
              "l'autre. À ne pas confondre avec un membre déconnecté."));
          changed |= RoColorSwatch(i18n::Tr("Texte — hors ligne"),
                                   pf->col_offline_);
          changed |= RoColorSwatch(i18n::Tr("Liseré de ma tuile"), pf->col_me_);

          changed |= WheelSliderInt(i18n::Tr("Seuil « vie moyenne »"),
                                    &pf->hp_mid_pct_, 20, 90, "%d %%");
          changed |= WheelSliderInt(i18n::Tr("Seuil « vie basse »"),
                                    &pf->hp_low_pct_, 5, 50, "%d %%");
          SameLine(); HelpMarker(i18n::Tr(
              "En dessous de ces pourcentages, la tuile change de couleur. C'est "
              "ce qui permet de repérer un blessé sans lire un seul chiffre."));

          if (!pf->enabled_) ImGui::EndDisabled();
          if (changed) SaveSettings();
        }
      }

      // ── Chat Settings ────────────────────────────────────────────────────
      if (iface_nav_ == kIfaceChat) {
        bool changed = false;
        // Deux propriétaires dans cette section, et c'est assumé : le relais
        // Discord est un réglage SERVEUR (MoonlightUi l'envoie), les retouches de
        // la fenêtre appartiennent à ChatTweaks, et les couleurs de fond au patch
        // mémoire de MoonlightUi. Les accolades restent pour ne pas réindenter.
        {
          PushStyleCompact();

          SeparatorText(i18n::Tr("Réglages généraux"));
          if (ro::RoCheckbox(i18n::Tr("Chat Discord (Gonryun only)"), &discord_chat_)) {
            UpdateRelay();
            SendSetting(kSettingDiscordChat, discord_chat_ ? 1 : 0);
          }
          SameLine(); HelpMarker(
              i18n::Tr("Relaie le canal Discord du serveur dans le chat du jeu, et tes "
              "messages vers Discord — uniquement sur la carte Gonryun."));

          // Avatar Discord : la page UCP du site génère l'image du personnage
          // déjà recadrée/dimensionnée pour Discord. C'est la MÊME identité que
          // le relais ci-dessus (le pseudo affiché côté Discord), d'où la place
          // de la mention ici plutôt que dans un panneau « compte ».
          ImGui::TextDisabled(
              i18n::Tr("Ton avatar Discord : le panneau utilisateur du site génère "
              "l'image de ton personnage à la bonne dimension pour Discord, en "
              "un clic."));
          const std::string avatar_url = DiscordAvatarUrl();
          if (ro::RoButton(i18n::Tr("Générer mon avatar Discord"))) {
            ShellExecuteA(nullptr, "open", avatar_url.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
          }
          SameLine(); HelpMarker(avatar_url.c_str());

          // 🔴 Les réglages du chat NATIF ne s'affichent que tant que ce chat
          // EXISTE. La chatbox ImGui le détruit : largeur, horodatage, icônes et
          // couleurs de fond n'agiraient alors plus sur rien. Laisser des réglages
          // inertes est le pire des retours — le joueur tourne une valeur, rien ne
          // bouge, et rien ne le lui dit.
          const bool native_chat_replaced =
              Bourgeon::Instance().chat_window() != nullptr &&
              Bourgeon::Instance().chat_window()->imgui_enabled_;

          if (!native_chat_replaced) {
            if (auto* chat_tweaks = Bourgeon::Instance().chat_tweaks()) {
              changed |= chat_tweaks->DrawSettings();
            } else {
              ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
            }
          }

          // Chatbox ImGui — son remplacement. Les réglages du natif ci-dessus ne
          // s'affichent que si elle est ÉTEINTE : allumée, elle détruit la fenêtre
          // qu'ils habillent (cf. features/windows/chat_window.h).
          SeparatorText(i18n::Tr("Chatbox ImGui"));
          if (auto* chat_window = Bourgeon::Instance().chat_window()) {
            changed |= chat_window->DrawSettings();
          } else {
            ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
          }

          // La bulle au-dessus des têtes vit ICI, avec la chatbox, et pas dans
          // une section « overlays » : elle affiche exactement les mêmes lignes,
          // résolues par le MÊME parseur (ChatWindow::PlainTextFromWire). Les
          // séparer inviterait à les faire diverger.
          SeparatorText(i18n::Tr("Bulles au-dessus des têtes"));
          if (auto* balloon = Bourgeon::Instance().chat_balloon()) {
            balloon->DrawSettings();
          } else {
            ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
          }

          if (!native_chat_replaced) SeparatorText(i18n::Tr("Couleurs du chat"));
          // Les trois fonds appartiennent à ChatTweaks (patch .text + parcours du
          // tas). Ils sont dessinés un par un et non en bloc pour une seule
          // raison : la case « Barre de préréglages » ci-dessous est un réglage de
          // MoonlightUi, et elle se pose à DROITE du premier sélecteur.
          if (auto* chat_tweaks =
                  native_chat_replaced ? nullptr : Bourgeon::Instance().chat_tweaks()) {
            if (chat_tweaks->bg_available()) {
              changed |= chat_tweaks->DrawBackgroundGroup(ChatTweaks::kBgMain);
              SameLine();
              changed |= ro::RoCheckbox(i18n::Tr("Barre de préréglages"), &mainchat_preset_bar_);
              changed |= chat_tweaks->DrawBackgroundGroup(ChatTweaks::kBgDetached);
              changed |= chat_tweaks->DrawBackgroundGroup(ChatTweaks::kBgWhisper);
            } else {
              ImGui::TextDisabled(i18n::Tr("(patch du fond de chat indisponible)"));
            }
          }

          PopStyleCompact();
        }
        if (changed) SaveSettings();
      }

      // ── Menu icons (ImGui replacement) ───────────────────────────────────
      if (iface_nav_ == kIfaceMenuIcons) {
        if (auto* mi = Bourgeon::Instance().menu_icons()) {
          if (mi->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Status icons (StatusIconBar) ──────────────────────────────────
      if (iface_nav_ == kIfaceStatusIcons) {
        if (auto* si = Bourgeon::Instance().status_icons())
          si->DrawSettings();
        else
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
      }

      // ── Suivi de quête (QuestTracker) ──────────────────────────────
      if (iface_nav_ == kIfaceQuest) {
        if (auto* qt = Bourgeon::Instance().quest_tracker())
          qt->DrawSettings();
        else
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
      }

      // ── Minimap (carte du lieu + position du personnage) ────────────
      if (iface_nav_ == kIfaceMinimap) {
        if (auto* mm = Bourgeon::Instance().minimap())
          mm->DrawSettings();
        else
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
      }

      // ── Bandeau « objet obtenu » (ItemObtainToast) ─────────────────
      if (iface_nav_ == kIfaceItemToast) {
        if (auto* iot = Bourgeon::Instance().item_obtain_toast())
          iot->DrawSettings();
        else
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
      }

      // ── Descriptions (ItemDescWindow : panneaux techniques item/skill) ───
      if (iface_nav_ == kIfaceDesc) {
        if (auto* idt = Bourgeon::Instance().item_desc()) {
          if (idt->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Skin RO (police + habillage des fenêtres ImGui) ──────────────────
      // Seule section qui ne pilote PAS un plugin : elle règle le toolkit, donc
      // elle vit dans ui/skin_panel.cc. Ici on ne fait que l'afficher.
      if (iface_nav_ == kIfaceSkin) {
        if (ro::DrawSkinPanel()) SaveSettings();
      }

      // ── Fenêtre NPC (dialogue / menu / prompt ImGui) ─────────────────────
      if (iface_nav_ == kIfaceNpc) {
        if (auto* nd = Bourgeon::Instance().npc_dialog_window()) {
          if (nd->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Storage (StorageWindow : viewer ImGui + colonnes/filtre/survol) ───
      if (iface_nav_ == kIfaceStorage) {
        if (auto* stg = Bourgeon::Instance().storage_window()) {
          if (stg->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Inventaire (InventoryViewer : viewer ImGui + filtre/onglets) ──────
      if (iface_nav_ == kIfaceInventory) {
        if (auto* iv = Bourgeon::Instance().inventory_viewer()) {
          if (iv->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Cart (CartViewer : viewer ImGui + filtre/onglets) ──────────────
      if (iface_nav_ == kIfaceCart) {
        if (auto* cv = Bourgeon::Instance().cart_viewer()) {
          if (cv->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // (Pas de section « Banque » : elle n'avait plus aucun réglage à offrir. Son
      // contenu est imposé par un fond bitmap à hauteur fixe — rien n'y est
      // masquable — et elle suit le groupe « Interface moderne » comme les autres.
      // Il ne restait qu'un paragraphe descriptif, qui n'a pas sa place dans un
      // panneau de réglages. `bank_imgui` continue d'être persisté et basculé par
      // SetModernInterface : c'est la SECTION qui disparaît, pas le réglage.)

      // ── Refine d'arme (WeaponRefineWindow : compétence Upgrade Weapon) ─
      if (iface_nav_ == kIfaceRefine) {
        if (auto* wr = Bourgeon::Instance().weapon_refine_window()) {
          if (wr->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Fabrication (MakeItemWindow : « LIST » 94 + « Manufacturing List » 79) ─
      if (iface_nav_ == kIfaceMakeItem) {
        if (auto* mk = Bourgeon::Instance().make_item_window()) {
          if (mk->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Fiche de monstre (MonsterInfoWindow : « Monster Info » 0x4D, Sense) ──
      if (iface_nav_ == kIfaceMonsterInfo) {
        if (auto* mi = Bourgeon::Instance().monster_info()) {
          ImGui::TextWrapped(
              i18n::Tr("Remplace la fenêtre « Monster Info » qu'ouvre la compétence Sense. "
              "Elle ajoute ce que le paquet du skill ne transporte pas : nom "
              "fiable, EXP, ATK/MATK, stats de base, modes, drops, lieux "
              "d'apparition et compétences. Le nom d'un monstre dans la table des "
              "sources d'une fiche d'objet l'ouvre d'un clic."));
          ImGui::Separator();
          if (mi->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Fiche de pet (PetWindow : 88 + menu 260 + évolution 261 + liste 90) ─
      if (iface_nav_ == kIfacePet) {
        if (auto* pw = Bourgeon::Instance().pet_window()) {
          ImGui::TextWrapped(
              i18n::Tr("Remplace la fiche du pet, le menu de commandes qu'elle "
              "ouvrait et la fenêtre d'évolution, en une seule fenêtre flottante — "
              "ainsi que la liste d'éclosion. Les commandes ne sont pas "
              "réécrites : elles repassent par le dispatcher du client, donc son "
              "refus « pas de nourriture en sac » reste joué. S'y ajoute ce que le "
              "client gardait pour lui : le sens de variation de la faim, le nom de "
              "l'accessoire porté, les matériaux d'évolution qu'on possède déjà, "
              "les œufs vides que le serveur refusera d'éclore, et la raison pour "
              "laquelle une action est indisponible plutôt que sa disparition."));
          ImGui::Separator();
          if (pw->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Menu contextuel du clic droit sur une entité ────────────────────────
      if (iface_nav_ == kIfaceContextMenu) {
        if (auto* ecm = Bourgeon::Instance().entity_context_menu()) {
          ImGui::TextWrapped(
              i18n::Tr("Remplace le menu du clic droit sur une entité. Les actions ne "
              "sont pas réécrites : elles repassent par le dispatcher du client, "
              "donc ses vérifications et ses confirmations restent jouées. Le "
              "menu du client n'existait que sur un joueur, son pet, son "
              "homoncule et son mercenaire — ici il peut aussi s'ouvrir sur les "
              "monstres et les NPC."));
          ImGui::Separator();
          if (ecm->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Atlas des recettes (CraftAtlas) ────────────────────────────────────
      // 🔴 PAS dans `needs_modern` : l'Atlas n'AGIT sur rien — il n'envoie aucun
      // paquet et ne remplace aucune fenêtre native, il lit un fichier. Le gate
      // « interface moderne » porte sur ce qui agit, pas sur ce qui affiche.
      if (iface_nav_ == kIfaceCraftAtlas) {
        if (auto* atlas = Bourgeon::Instance().craft_atlas()) {
          if (atlas->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled(i18n::Tr(kPluginUnavailable));
        }
      }

      ImGui::EndDisabled();  // apparié au BeginDisabled(locked) au-dessus
      PopItemWidth();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    PopStyleCompact();
  }
}
