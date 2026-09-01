#include "features/moonlight_ui/internal.h"
#include "features/systems/moonlight_auth.h"  // SiteBaseUrl

#include <windows.h>
#include <shellapi.h>  // ShellExecuteA (lien « avatar Discord » vers l'UCP)

#include "bourgeon.h"
#include "imgui.h"
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
#include "features/windows/make_item_window.h"
#include "features/windows/entity_context_menu.h"
#include "features/windows/monster_info_window.h"
#include "features/windows/mvp_tracker_window.h"
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

// ── LES SECTIONS D'« INTERFACE DE JEU » ──────────────────────────────────────
// La table de la nav latérale de cet en-tête. Sa mécanique — la liste, le geste
// de lien, les infobulles — vit dans panel_nav.cc, partagée avec la nav de
// « Gameplay » ; ici il n'y a que le CONTENU.
//
// Source UNIQUE : chaque ligne porte son identifiant d'enum, sa clé et son
// libellé. Insérer/déplacer une entrée ne peut donc pas désaligner silencieusement
// le libellé et le contenu — la panne muette que produisait la paire « enum +
// tableau de chaînes » maintenue à la main.
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

using iface::NavEntry;

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
constexpr NavEntry kIfaceSections[] = {
    {MoonlightUi::kIfaceSkillBar,    "skill_bar",    "Barre d'action"},
    {MoonlightUi::kIfaceCastBar,     "cast_bar",     "Barre de Cast"},
    {MoonlightUi::kIfaceBasicInfo,   "basic_info",   "Basic Info"},
    {MoonlightUi::kIfaceCart,        "cart",         "Cart"},
    {MoonlightUi::kIfaceMvpTracker,  "mvp_tracker",  "Carnet de chasse MVP"},
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

}  // namespace

namespace iface {

const NavGroup& InterfaceGroup() {
  static constexpr NavGroup kGroup{kNavInterface, "interface", kIfaceSections,
                                   IM_ARRAYSIZE(kIfaceSections)};
  return kGroup;
}

}  // namespace iface

// ── En-tête « Interface de jeu » ─────────────────────────────────────────────
// Navigation latérale + les 13 sections de configuration. C'était le bloc dominant
// d'OnRenderUI (742 lignes sur 1702) : une nav, puis une cascade de 11 tests sur
// l'entrée courante, chacun avec sa propre variable changed homonyme.
// La table kIfaceSections (source unique libellé + identifiant, cf. chantier 5)
// vit ici, au plus près de son usage ; la MÉCANIQUE de la liste, elle, est partie
// dans panel_nav.cc le jour où « Gameplay » a eu la même.
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
    // réglage dont dépendent treize des vingt-cinq sections.
    bool modern = ModernInterfaceEnabled();
    changed |= DrawModernInterfaceCheckbox(
        &modern,
        i18n::Tr("Les réglages propres à chaque fenêtre restent dans leur section "
        "ci-dessous, et y sont grisés tant que cette case est décochée : sans "
        "elle, ces fenêtres n'existent pas."));

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
          ImGui::BeginDisabled(!language.available);
          if (ImGui::Selectable(language.label, selected) && !selected) {
            changed |= i18n::SetLanguage(language.code);
            // La table de messages du CLIENT suit la même langue que notre
            // interface. Sûr en pleine frame : le rechargement n'efface AUCUNE
            // chaîne déjà rendue (elles vivent dans un stockage qu'on ne libère
            // jamais), donc un libellé « en vol » reste valide.
            msgoverride::Reload();
          }
          ImGui::EndDisabled();
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
    // un MEMBRE (nav_[kNavInterface]) : OpenInterfaceSection la pilote depuis le
    // bullet de barre de titre d'une autre fenêtre Bourgeon. La table des sections
    // (kIfaceSections) vit en tête de ce fichier — elle sert aussi aux liens, et
    // la mécanique de la liste est partagée avec la nav de « Gameplay »
    // (panel_nav.cc).
    // Référence, et non copie : les tests ci-dessous lisent la section que
    // BeginNavPanel vient d'écrire, dans la MÊME frame que le clic.
    int& iface_nav = nav_[iface::kNavInterface];
    iface::BeginNavPanel(iface::InterfaceGroup(), &iface_nav);
    {
      PushItemWidth(160.0f);

      // ── Sections qui n'existent QUE si le groupe « Interface moderne » l'est ─
      //
      // Elles ne règlent que des fenêtres ImGui : groupe coupé, elles n'existent
      // pas et leurs options ne changent rien. Un réglage sans effet est un piège —
      // on le grise, plutôt que de laisser croire qu'il agit.
      //
      // 🔴 LA LISTE N'EST PAS ICI, et c'est tout l'intérêt : `SectionNeedsModern`
      // la lit dans la table qui définit le groupe (moonlight_ui.cc), celle-là même
      // qui bascule les plugins et qui écrit l'infobulle de la case. Elle a été
      // recopiée à la main pendant longtemps, et l'infobulle taisait déjà cinq
      // membres. Ajouter un plugin au groupe grise désormais sa section du même
      // geste.
      //
      // Le TEST, lui, reste au site d'appel unique : un BeginDisabled à apparier
      // dans treize plugins finit par se dépareiller sur un chemin de sortie.
      //
      // UNE section du groupe répond faux ici (`kSelfGated` dans la table) : le
      // Chat, dont le relais Discord et les retouches du chat NATIF n'ont rien à
      // voir avec le groupe — seule la chatbox ImGui en est, et elle porte son
      // propre grisage, partiel.
      const bool needs_modern = SectionNeedsModern(iface_nav);
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
        ImGui::TextColored(ImVec4(166 / 255.0f, 102 / 255.0f, 0.0f, 1.0f), "%s",
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
      if (iface_nav == kIfaceSkillBar)
      {
        if (auto* sb = Bourgeon::Instance().skill_bar()) {
          if (sb->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Barres d'info (HUD bars + alignment grid) ────────────────────────
      // ── Status Portrait (head + pseudo + classe + niveau, indépendants) ──
      if (iface_nav == kIfaceBasicInfo) {
        if (auto* basic_info = Bourgeon::Instance().basic_info()) {
          if (basic_info->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Barre d'incantation (CastBar) ────────────────────────────────────
      // Sa jumelle HUD (« Cast ») se règle avec les autres barres, dans la
      // section Basic Info : c'est la même famille de widgets, même verrou,
      // même aimantation. Ici on ne traite que celles des entités.
      if (iface_nav == kIfaceCastBar) {
        if (auto* cast = Bourgeon::Instance().cast_bar()) {
          if (cast->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Fenêtre de cible (TargetFrame) ───────────────────────────────────
      // Elle suit la sélection native — la même que la petite flèche blanche du
      // jeu — et complète ce que le client ignore par une requête serveur.
      if (iface_nav == kIfaceTargetFrame) {
        if (auto* tf = Bourgeon::Instance().target_frame()) {
          if (tf->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Groupe / Amis : la FENÊTRE (PartyFriendWindow) ───────────────────
      // À distinguer de la grille juste en dessous : celle-ci REMPLACE la fenêtre
      // native 0x45, l'autre est un HUD. Elles lisent la même source mais ne se
      // règlent pas ensemble — on ne consulte pas une liste comme on surveille
      // des barres de vie.
      if (iface_nav == kIfacePartyFriend) {
        if (auto* pfw = Bourgeon::Instance().party_friend_window()) {
          if (pfw->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Groupe : la grille (PartyFrames) ─────────────────────────────────
      // Un HUD de raid frames — une tuile par membre, dont la couleur DIT l'état.
      // HORS du groupe « Interface moderne » : il ne remplace aucune fenêtre, il
      // ajoute ce que le client n'a pas sous cette forme, et il a du sens même en
      // interface native.
      if (iface_nav == kIfacePartyFrames) {
        if (auto* pf = Bourgeon::Instance().party_frames()) {
          if (pf->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Carnet de chasse MVP (MvpTrackerWindow) ──────────────────────────
      // Ce que le groupe a OBSERVÉ, jamais ce que le serveur sait : le tirage du
      // respawn ne sort que par un Convex Mirror.
      if (iface_nav == kIfaceMvpTracker) {
        if (auto* mvp = Bourgeon::Instance().mvp_tracker_window()) {
          if (mvp->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Chat Settings ────────────────────────────────────────────────────
      if (iface_nav == kIfaceChat) {
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
              ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
            }
          }

          // Chatbox ImGui — son remplacement. Les réglages du natif ci-dessus ne
          // s'affichent que si elle est ÉTEINTE : allumée, elle détruit la fenêtre
          // qu'ils habillent (cf. features/windows/chat_window.h).
          SeparatorText(i18n::Tr("Chatbox ImGui"));
          if (auto* chat_window = Bourgeon::Instance().chat_window()) {
            changed |= chat_window->DrawSettings();
          } else {
            ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
          }

          // La bulle au-dessus des têtes vit ICI, avec la chatbox, et pas dans
          // une section « overlays » : elle affiche exactement les mêmes lignes,
          // résolues par le MÊME parseur (ChatWindow::PlainTextFromWire). Les
          // séparer inviterait à les faire diverger.
          SeparatorText(i18n::Tr("Bulles au-dessus des têtes"));
          if (auto* balloon = Bourgeon::Instance().chat_balloon()) {
            changed |= balloon->DrawSettings();
          } else {
            ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
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
              ImGui::TextDisabled("%s",
                                i18n::Tr("(patch du fond de chat indisponible)"));
            }
          }

          PopStyleCompact();
        }
        if (changed) SaveSettings();
      }

      // ── Menu icons (ImGui replacement) ───────────────────────────────────
      if (iface_nav == kIfaceMenuIcons) {
        if (auto* mi = Bourgeon::Instance().menu_icons()) {
          if (mi->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Status icons (StatusIconBar) ──────────────────────────────────
      if (iface_nav == kIfaceStatusIcons) {
        if (auto* si = Bourgeon::Instance().status_icons()) {
          if (si->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Suivi de quête (QuestTracker) ──────────────────────────────
      if (iface_nav == kIfaceQuest) {
        if (auto* qt = Bourgeon::Instance().quest_tracker()) {
          if (qt->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Minimap (carte du lieu + position du personnage) ────────────
      if (iface_nav == kIfaceMinimap) {
        if (auto* mm = Bourgeon::Instance().minimap()) {
          if (mm->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Bandeau « objet obtenu » (ItemObtainToast) ─────────────────
      if (iface_nav == kIfaceItemToast) {
        if (auto* iot = Bourgeon::Instance().item_obtain_toast()) {
          if (iot->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Descriptions (ItemDescWindow : panneaux techniques item/skill) ───
      if (iface_nav == kIfaceDesc) {
        if (auto* idt = Bourgeon::Instance().item_desc()) {
          if (idt->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Skin RO (police + habillage des fenêtres ImGui) ──────────────────
      // Seule section qui ne pilote PAS un plugin : elle règle le toolkit, donc
      // elle vit dans ui/skin_panel.cc. Ici on ne fait que l'afficher.
      if (iface_nav == kIfaceSkin) {
        if (ro::DrawSkinPanel()) SaveSettings();
      }

      // ── Fenêtre NPC (dialogue / menu / prompt ImGui) ─────────────────────
      if (iface_nav == kIfaceNpc) {
        if (auto* nd = Bourgeon::Instance().npc_dialog_window()) {
          if (nd->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Storage (StorageWindow : viewer ImGui + colonnes/filtre/survol) ───
      if (iface_nav == kIfaceStorage) {
        if (auto* stg = Bourgeon::Instance().storage_window()) {
          if (stg->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Inventaire (InventoryViewer : viewer ImGui + filtre/onglets) ──────
      if (iface_nav == kIfaceInventory) {
        if (auto* iv = Bourgeon::Instance().inventory_viewer()) {
          if (iv->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Cart (CartViewer : viewer ImGui + filtre/onglets) ──────────────
      if (iface_nav == kIfaceCart) {
        if (auto* cv = Bourgeon::Instance().cart_viewer()) {
          if (cv->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // (Pas de section « Banque » : elle n'avait plus aucun réglage à offrir. Son
      // contenu est imposé par un fond bitmap à hauteur fixe — rien n'y est
      // masquable — et elle suit le groupe « Interface moderne » comme les autres.
      // Il ne restait qu'un paragraphe descriptif, qui n'a pas sa place dans un
      // panneau de réglages. `bank_imgui` continue d'être persisté et basculé par
      // SetModernInterface : c'est la SECTION qui disparaît, pas le réglage.)

      // ── Refine d'arme (WeaponRefineWindow : compétence Upgrade Weapon) ─
      if (iface_nav == kIfaceRefine) {
        if (auto* wr = Bourgeon::Instance().weapon_refine_window()) {
          if (wr->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Fabrication (MakeItemWindow : « LIST » 94 + « Manufacturing List » 79) ─
      if (iface_nav == kIfaceMakeItem) {
        if (auto* mk = Bourgeon::Instance().make_item_window()) {
          if (mk->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Fiche de monstre (MonsterInfoWindow : « Monster Info » 0x4D, Sense) ──
      if (iface_nav == kIfaceMonsterInfo) {
        if (auto* mi = Bourgeon::Instance().monster_info()) {
          ImGui::TextWrapped("%s",
              i18n::Tr("Remplace la fenêtre « Monster Info » qu'ouvre la compétence Sense. "
              "Elle ajoute ce que le paquet du skill ne transporte pas : nom "
              "fiable, EXP, ATK/MATK, stats de base, modes, drops, lieux "
              "d'apparition et compétences. Le nom d'un monstre dans la table des "
              "sources d'une fiche d'objet l'ouvre d'un clic."));
          ImGui::Separator();
          if (mi->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Fiche de pet (PetWindow : 88 + menu 260 + évolution 261 + liste 90) ─
      if (iface_nav == kIfacePet) {
        if (auto* pw = Bourgeon::Instance().pet_window()) {
          ImGui::TextWrapped("%s",
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
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Menu contextuel du clic droit sur une entité ────────────────────────
      if (iface_nav == kIfaceContextMenu) {
        if (auto* ecm = Bourgeon::Instance().entity_context_menu()) {
          ImGui::TextWrapped("%s",
              i18n::Tr("Remplace le menu du clic droit sur une entité. Les actions ne "
              "sont pas réécrites : elles repassent par le dispatcher du client, "
              "donc ses vérifications et ses confirmations restent jouées. Le "
              "menu du client n'existait que sur un joueur, son pet, son "
              "homoncule et son mercenaire — ici il peut aussi s'ouvrir sur les "
              "monstres et les NPC."));
          ImGui::Separator();
          if (ecm->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      // ── Atlas des recettes (CraftAtlas) ────────────────────────────────────
      // 🔴 PAS dans `needs_modern` : l'Atlas n'AGIT sur rien — il n'envoie aucun
      // paquet et ne remplace aucune fenêtre native, il lit un fichier. Le gate
      // « interface moderne » porte sur ce qui agit, pas sur ce qui affiche.
      if (iface_nav == kIfaceCraftAtlas) {
        if (auto* atlas = Bourgeon::Instance().craft_atlas()) {
          if (atlas->DrawSettings()) SaveSettings();
        } else {
          ImGui::TextDisabled("%s", i18n::Tr(kPluginUnavailable));
        }
      }

      ImGui::EndDisabled();  // apparié au BeginDisabled(locked) au-dessus
      PopItemWidth();
    }
    iface::EndNavPanel();
    PopStyleCompact();
  }
}
