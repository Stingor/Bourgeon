#include "features/link_gesture.h"

#include <Windows.h>
#include <shellapi.h>  // ShellExecuteA (site, bestiaire, adresses du chat)

#include <cstdio>
#include <cstring>  // _strnicmp (adresse « www.… » sans schéma)

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"     // liste alootid
#include "features/systems/image_preview.h"         // aperçu d'une image de chat
#include "features/windows/cashshop_window.h"       // disponibilité au vote shop
#include "features/windows/chat_window.h"           // poser un lien, armer une commande
#include "features/windows/item_desc_window.h"      // itemdesc::OpenItemDbPage
#include "features/windows/monster_info_window.h"   // fiche d'un monstre
#include "imgui.h"
#include "ui/ro_imgui.h"                            // ro::SetHoverCursor
#include "utils/i18n.h"

namespace links {
namespace {

// 🔴 Les cadres RO poussent un texte CLAIR (leur fond est sombre) ; une infobulle
// et un popup, eux, ont un fond CLAIR. Sans cette couleur, tout ce qu'on y écrit
// est blanc sur blanc. Une seule définition pour les deux surfaces — l'infobulle
// d'adresse l'avait justement oubliée.
const ImU32 kDarkText = IM_COL32(24, 22, 20, 255);

// Page « bestiaire » du site. Même rôle que `itemdesc::OpenItemDbPage` pour un
// objet — écrite ICI, une fois, pour que les appelants n'aient pas chacun leur
// copie de l'URL à corriger.
void OpenMobDbPage(uint32_t mob_id) {
  if (mob_id == 0) return;
  char url[192];
  std::snprintf(url, sizeof(url),
                "https://moonlight-destiny.fr/index.php?page=bestiary&mobid=%u",
                mob_id);
  ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

// L'adresse telle qu'elle partira au navigateur. « www.… » sans schéma n'est pas
// une URL pour le shell : il l'ouvrirait comme un chemin de fichier.
std::string FullUrl(const char* url) {
  std::string full;
  if (_strnicmp(url, "www.", 4) == 0) full = "https://";
  full += url;
  return full;
}

void LaunchUrl(const char* url) {
  if (url == nullptr || url[0] == '\0') return;
  const std::string full = FullUrl(url);
  ShellExecuteA(nullptr, "open", full.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ── L'avertissement avant d'ouvrir le navigateur ─────────────────────────────
// Une adresse postée dans le chat vient d'un autre joueur, et le texte affiché
// n'a rien à voir avec la destination : c'est le vecteur d'hameçonnage le plus
// banal qui soit. On demande donc confirmation, en montrant l'adresse COMPLÈTE —
// celle qui partira, schéma compris, pas celle qui est écrite dans la ligne.
//
// Le joueur peut retirer le garde-fou (réglage de la chatbox). C'est son choix,
// il est explicite, et le refuser reviendrait à faire cliquer trois fois par
// lien quelqu'un qui sait ce qu'il fait.
//
// 🔴 L'ouverture ImGui est DIFFÉRÉE. `OpenUrl` est appelée depuis un menu
// contextuel (donc une autre pile d'ID) ou depuis le dessin d'une ligne de log :
// un `ImGui::OpenPopup` posé là ne trouverait pas la modale. Même piège, même
// remède que `ro::OpenQuantityPrompt`.
std::string g_pending_url;
bool        g_confirm_requested = false;
// Même mécanique différée pour l'autorisation d'un HÔTE : elle part aussi d'un
// menu contextuel, donc d'une autre pile d'ID.
std::string g_pending_host;
bool        g_host_confirm_requested = false;

// Le réglage vit dans la chatbox — c'est là que les adresses arrivent, et c'est
// là que le joueur ira le chercher. Chatbox absente : on avertit, parce que le
// défaut d'un garde-fou est d'être là.
bool UrlConfirmEnabled() {
  const ChatWindow* chat = Bourgeon::Instance().chat_window();
  return chat == nullptr || chat->url_confirm();
}

void OpenUrl(const char* url) {
  if (url == nullptr || url[0] == '\0') return;
  if (!UrlConfirmEnabled()) {
    LaunchUrl(url);
    return;
  }
  g_pending_url       = url;
  g_confirm_requested = true;
}

// ── L'aperçu d'une image, sous l'adresse ─────────────────────────────────────
//
// 🔴 UN DÉLAI AVANT DE TÉLÉCHARGER. Sans lui, traverser le chat déclencherait une
// requête par lien croisé — personne ne DÉCIDE de survoler, et dans un chat qui
// défile ce sont les lignes qui glissent sous un curseur immobile. Le délai fait
// du survol un geste : il faut s'arrêter là pour que quoi que ce soit parte.
//
// Le reste des garde-fous (liste blanche d'hôtes, bornes, décodage) vit dans
// imgprev — voir l'en-tête de features/systems/image_preview.h pour le pourquoi.
constexpr float kPreviewHoverDelay = 0.4f;  // secondes

// L'aperçu courant, et l'armement du téléchargement si le survol dure. Séparé du
// DESSIN parce que la réponse décide de la forme de l'infobulle : une image prête
// s'affiche SEULE, sans cadre ni fond ni adresse, et cela se décide avant
// `BeginTooltip`.
imgprev::Preview UrlPreviewState(const std::string& url) {
  imgprev::Preview none;
  const ChatWindow* chat = Bourgeon::Instance().chat_window();
  if (chat == nullptr) return none;
  const bool explicit_ok = imgprev::IsExplicitlyAllowed(url.c_str());
  if (!chat->url_preview() && !explicit_ok) return none;
  if (!imgprev::IsPreviewable(url.c_str())) return none;

  // 🔴 UN DÉLAI AVANT DE TÉLÉCHARGER : sans lui, traverser le chat déclencherait
  // une requête par lien croisé. Le compteur se mesure sur l'ADRESSE, pour que
  // glisser d'un lien à l'autre reparte de zéro.
  static std::string s_hovered;
  static float       s_elapsed = 0.0f;
  if (s_hovered != url) {
    s_hovered = url;
    s_elapsed = 0.0f;
  } else {
    s_elapsed += ImGui::GetIO().DeltaTime;
  }
  if (s_elapsed < kPreviewHoverDelay) return none;

  imgprev::Request(url.c_str());
  return imgprev::Get(url.c_str());
}

// Le STATUT, quand aucune image n'est prête à montrer. Vit dans l'infobulle
// ordinaire, sous l'adresse.
void DrawUrlPreviewStatus(const std::string& url,
                          const imgprev::Preview& p) {
  const ChatWindow* chat = Bourgeon::Instance().chat_window();
  if (chat == nullptr) return;
  // 🔴 L'OPT-IN BORNE CE QUI SE CHARGE TOUT SEUL, pas ce que le joueur a demandé.
  // Réglage éteint et rien de demandé = on se tait complètement, y compris le
  // message d'explication — du bruit pour qui a dit ne pas vouloir d'images.
  const bool explicit_ok = imgprev::IsExplicitlyAllowed(url.c_str());
  if (!chat->url_preview() && !explicit_ok) return;

  if (!imgprev::IsPreviewable(url.c_str())) {
    // 🔴 NE PAS SE TAIRE. Ne rien afficher parce que l'hôte est inconnu est le
    // comportement voulu, mais il est indiscernable d'une panne — et la
    // fonctionnalité devient invisible pour qui ne connaît pas la règle.
    const std::string host = imgprev::HostOfUrl(url.c_str());
    ImGui::Separator();
    ImGui::TextDisabled(i18n::Tr("Aperçu non chargé : %s n'est pas dans vos sites"),
                        host.empty() ? "ce site" : host.c_str());
    ImGui::TextDisabled(i18n::Tr("autorisés. Clic droit pour l'afficher."));
    return;
  }
  if (p.state == imgprev::Preview::kPending) {
    ImGui::Separator();
    ImGui::TextDisabled(i18n::Tr("Chargement de l'aperçu..."));
  } else if (p.state == imgprev::Preview::kFailed) {
    // Motif volontairement vague : le détail (404, type refusé, trop gros)
    // n'aide en rien celui qui regarde. Le journal, lui, porte la raison exacte.
    ImGui::Separator();
    ImGui::TextDisabled(i18n::Tr("Aperçu indisponible."));
  }
}

// La fiche en jeu, avec repli sur le site quand elle est désactivée : un lien qui
// ne fait RIEN est pire qu'un lien qui fait autre chose de sensé.
void OpenMobSheet(uint32_t mob_id) {
  if (mob_id == 0) return;
  MonsterInfoWindow* sheet = Bourgeon::Instance().monster_info();
  if (sheet == nullptr || !sheet->imgui_enabled_) {
    OpenMobDbPage(mob_id);
    return;
  }
  // by_view = false : l'id d'un lien vient de mob_db, ce n'est pas une classe de
  // sprite (un monstre déguisé porterait celle d'un autre).
  sheet->Open(mob_id, /*by_view=*/false);
}

void QueueCommand(const char* utf8) {
  if (ChatWindow* chat = Bourgeon::Instance().chat_window()) chat->QueueCommand(utf8);
}

}  // namespace

Target FromItem(const itemcell::ChatLink& link, const char* label_utf8) {
  Target t;
  if (link.id == 0) return t;
  t.kind = Target::kItem;
  t.item = link;
  if (label_utf8 != nullptr) t.label = label_utf8;
  return t;
}

Target FromItemId(uint32_t item_id, const char* label_utf8) {
  itemcell::ChatLink link;
  link.id = item_id;
  return FromItem(link, label_utf8);
}

Target FromMob(uint32_t mob_id, int rank, const char* name_utf8) {
  Target t;
  if (mob_id == 0) return t;
  t.kind     = Target::kMob;
  t.mob_id   = mob_id;
  t.mob_rank = static_cast<uint8_t>((rank < 0 || rank > 2) ? 0 : rank);
  if (name_utf8 != nullptr) t.mob_name = name_utf8;
  t.label = t.mob_name;
  return t;
}

Target FromUrl(const char* url) {
  Target t;
  if (url == nullptr || url[0] == '\0') return t;
  t.kind  = Target::kUrl;
  t.url   = url;
  t.label = url;
  return t;
}

Target FromPlayer(const char* name_utf8) {
  Target t;
  if (name_utf8 == nullptr || name_utf8[0] == '\0') return t;
  t.kind        = Target::kPlayer;
  t.player_name = name_utf8;
  t.label       = name_utf8;
  return t;
}

void OpenDescription(const Target& target) {
  switch (target.kind) {
    case Target::kItem: {
      // 🔴 ARMÉE, pas ouverte : la fenêtre de description est native, et un appel
      // natif pendant une frame ImGui gèle le client en silence. Par le LIEN et
      // non par l'id — la balise porte le refine, les cartes et le forgeron.
      const ImVec2 mouse = ImGui::GetMousePos();
      itemcell::DeferDescFromChatLink(target.item, static_cast<int>(mouse.x),
                                      static_cast<int>(mouse.y));
      break;
    }
    case Target::kMob: OpenMobSheet(target.mob_id); break;
    case Target::kUrl: OpenUrl(target.url.c_str()); break;
    case Target::kPlayer: {
      // Un joueur n'a pas de « description » à ouvrir. Le geste GAUCHE lui rend
      // donc ce qui en est l'équivalent le plus proche : la conversation privée.
      // La convention porte sur l'intention — consulter — pas sur la fenêtre.
      ChatWindow* chat = Bourgeon::Instance().chat_window();
      if (chat != nullptr && !chat->IsOwnName(target.player_name.c_str()))
        chat->OpenWhisperWindowByAid(ro::Utf8ToWire(target.player_name.c_str()), 0);
      break;
    }
    default: break;
  }
}

bool PostToChat(const Target& target) {
  ChatWindow* chat = Bourgeon::Instance().chat_window();
  if (chat == nullptr || !chat->imgui_enabled_) return false;
  if (target.kind == Target::kMob)
    return chat->AppendMobLink(target.mob_id, target.mob_rank, target.mob_name.c_str());
  if (target.kind == Target::kItem) return chat->AppendItemLinkFromLink(target.item);
  return false;
}

Gesture Hit(const Target& target, bool hovered) {
  if (!target.valid() || !hovered) return Gesture::kNone;
  ro::SetHoverCursor(2);  // curseur « main » RO
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    // 🔴 Maj testé D'ABORD, et il DÉSARME le clic simple : sans ça le geste
    // poserait le lien ET ouvrirait la description par-dessus.
    return ImGui::GetIO().KeyShift ? Gesture::kChatLink : Gesture::kDescription;
  }
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) return Gesture::kMenu;
  return Gesture::kNone;
}

bool Gestures(const Target& target, bool hovered) {
  switch (Hit(target, hovered)) {
    case Gesture::kDescription: OpenDescription(target); return false;
    case Gesture::kChatLink:    PostToChat(target);      return false;
    case Gesture::kMenu:        return true;
    default:                    return false;
  }
}

void HoverPreview(const Target& target) {
  if (target.kind == Target::kItem) {
    // Le nom DÉCORÉ, composé par le name-builder natif depuis la balise (refine,
    // affixes de cartes, forgeron). Rendu dans la code-page du client, c'est ce
    // qu'attend le tooltip — comme tous ses autres appelants.
    char name[192];
    itemcell::BuildChatLinkName(target.item, name, sizeof(name));
    // Les options d'INSTANCE ne sont pas dans la DB du client : elles viennent de
    // la balise, seule source d'information sur l'objet d'un autre joueur.
    itemdesc::SimpleOpt opts[5];
    int opt_count = 0;
    for (int i = 0; i < target.item.opt_count && i < 5; ++i) {
      opts[opt_count].index = static_cast<int16_t>(target.item.opt_id[i]);
      opts[opt_count].value = static_cast<int16_t>(target.item.opt_value[i]);
      opts[opt_count].param = target.item.opt_param[i];
      ++opt_count;
    }
    // Les cartes partent telles quelles : sur un objet FORGÉ ce sont les données
    // du forgeron, et c'est le rendu de la description qui fait la différence
    // (même critère que la fenêtre complète) — pas à nous de la refaire ici.
    itemcell::DrawTooltip(target.item.id, target.item.cards, 4,
                          opt_count ? opts : nullptr, opt_count,
                          static_cast<int>(target.item.refine),
                          name[0] ? name : nullptr, target.item.broken);
    return;
  }
  if (target.kind == Target::kMob) {
    if (MonsterInfoWindow* sheet = Bourgeon::Instance().monster_info())
      sheet->DrawHoverPreview(target.mob_id);
    return;
  }
  if (target.kind == Target::kUrl) {
    // La forme de l'infobulle se décide AVANT de l'ouvrir, d'où l'état relevé ici.
    const imgprev::Preview p = UrlPreviewState(target.url);

    if (p.state == imgprev::Preview::kReady && p.tex != nullptr) {
      // 🔴 L'IMAGE SEULE : ni fond, ni cadre, ni adresse. Une image se suffit —
      // le chrome de l'infobulle et l'adresse répétée en dessous ne font que
      // l'encombrer, et l'adresse est déjà lisible dans la ligne de chat.
      // Même recette que l'illustration d'une carte (item_desc_window).
      ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
      ImGui::BeginTooltip();
      ImGui::Image(reinterpret_cast<ImTextureID>(p.tex),
                   ImVec2(static_cast<float>(p.w), static_cast<float>(p.h)));
      ImGui::EndTooltip();
      ImGui::PopStyleColor(2);
      return;
    }

    // Pas d'image : l'infobulle ordinaire, avec l'adresse ENTIÈRE — une ligne de
    // chat tronque, et c'est précisément sur ce qu'on ne voit pas qu'un lien
    // trompe. Texte SOMBRE : le fond d'une infobulle est clair, alors que la
    // chatbox pousse un texte clair pour son propre fond sombre.
    ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(target.url.c_str());
    DrawUrlPreviewStatus(target.url, p);
    ImGui::EndTooltip();
    ImGui::PopStyleColor();
  }
}

void DrawMenu(const char* popup_id, const Target& target) {
  ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
  if (ImGui::BeginPopup(popup_id)) {
    if (!target.label.empty()) ImGui::TextDisabled("%s", target.label.c_str());
    ImGui::Separator();
    char cmd[64];
    switch (target.kind) {
      case Target::kItem: {
        const uint32_t id = target.item.id;
        if (ImGui::MenuItem("Description")) OpenDescription(target);
        if (ImGui::MenuItem(i18n::Tr("Base de données du site"))) itemdesc::OpenItemDbPage(id);
        if (ImGui::MenuItem(i18n::Tr("Lien dans le chat"))) PostToChat(target);
        ImGui::Separator();
        // ── Disponibilité au vote shop ─────────────────────────────────────
        //
        // L'entrée n'existe QUE si l'objet y est vendu : sa présence EST
        // l'information. Le prix y figure parce que la question d'après est
        // toujours « combien ? », et qu'y répondre ici évite d'ouvrir la
        // boutique pour rien.
        //
        // 🔴 On n'affiche jamais l'inverse (« pas au vote shop »). Le catalogue
        // vient du serveur et peut ne pas être arrivé : une absence ne prouve
        // rien, et l'annoncer serait affirmer plus qu'on ne sait.
        if (auto* shop = Bourgeon::Instance().cashshop_window()) {
          int32_t price = 0;
          if (shop->imgui_enabled_ && shop->FindItem(id, nullptr, &price)) {
            char label[80];
            std::snprintf(label, sizeof(label),
                          i18n::Tr("Vote shop : ajouter au panier (%d pts)"), price);
            if (ImGui::MenuItem(label)) shop->OpenWithItem(id);
          }
        }
        ImGui::Separator();
        // Ramassage automatique : la MÊME liste que l'overlay de la description
        // (elle vit dans MoonlightUi, qui la tient à jour avec le serveur) —
        // surtout pas une seconde copie qui divergerait.
        if (auto* mu = Bourgeon::Instance().moonlight_ui()) {
          const bool looted = mu->IsAlootId(id);
          if (ImGui::MenuItem(looted ? i18n::Tr("Retirer de l'alootid") : i18n::Tr("Ajouter à l'alootid"))) {
            if (looted) mu->RemoveAlootId(id);
            else        mu->AddAlootId(id);
          }
        }
        ImGui::Separator();
        // Commandes serveur, envoyées par le pipeline COMPLET du client (mêmes
        // règles qu'une ligne tapée) : leur réponse revient dans le chat.
        if (ImGui::MenuItem("@iteminfo")) {
          std::snprintf(cmd, sizeof(cmd), "@iteminfo %u", id);
          QueueCommand(cmd);
        }
        if (ImGui::MenuItem("@whodrops")) {
          std::snprintf(cmd, sizeof(cmd), "@whodrops %u", id);
          QueueCommand(cmd);
        }
        break;
      }
      case Target::kMob: {
        if (ImGui::MenuItem(i18n::Tr("Fiche du monstre"))) OpenMobSheet(target.mob_id);
        if (ImGui::MenuItem(i18n::Tr("Bestiaire du site"))) OpenMobDbPage(target.mob_id);
        if (ImGui::MenuItem(i18n::Tr("Lien dans le chat"))) PostToChat(target);
        ImGui::Separator();
        if (ImGui::MenuItem("@mobinfo")) {
          std::snprintf(cmd, sizeof(cmd), "@mobinfo %u", target.mob_id);
          QueueCommand(cmd);
        }
        if (ImGui::MenuItem("@whereis")) {
          std::snprintf(cmd, sizeof(cmd), "@whereis %u", target.mob_id);
          QueueCommand(cmd);
        }
        break;
      }
      case Target::kPlayer: {
        // ⚠ Le NOM repart dans la code-page du FIL : il est arrivé de là, il y
        // retourne. Les paquets et les fonctions natives visées ne connaissent
        // pas l'UTF-8 — un pseudo accentué ne trouverait personne.
        const char* wire = ro::Utf8ToWire(target.player_name.c_str());
        ChatWindow* chat = Bourgeon::Instance().chat_window();
        const bool self  = chat != nullptr && chat->IsOwnName(target.player_name.c_str());

        // Chuchoter : rien de natif, rien sur le fil — juste notre fenêtre 1:1.
        if (!self && ImGui::MenuItem("Chuchoter…")) {
          if (chat != nullptr) chat->OpenWhisperWindowByAid(wire, 0);
        }
        if (!self && chat != nullptr) {
          // Grisées PLUTÔT que cachées quand elles n'ont aucune chance : le
          // joueur voit ce qui existe, et pourquoi ça ne s'offre pas à lui.
          const bool in_party = chat->InParty();
          if (!in_party) ImGui::BeginDisabled();
          if (ImGui::MenuItem(i18n::Tr("Inviter dans le groupe")))
            chat->QueueNameAction(ChatWindow::NameAction::kPartyInvite, wire);
          if (!in_party) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
              ImGui::SetTooltip(
                  i18n::Tr("Il faut être dans un groupe — et en être le chef — pour "
                  "inviter quelqu'un."));
          }
          const bool in_guild = chat->InGuild();
          if (!in_guild) ImGui::BeginDisabled();
          if (ImGui::MenuItem(i18n::Tr("Inviter dans la guilde")))
            chat->QueueNameAction(ChatWindow::NameAction::kGuildInvite, wire);
          if (!in_guild) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
              ImGui::SetTooltip(i18n::Tr("Il faut appartenir à une guilde pour y inviter."));
          }
          if (ImGui::MenuItem(i18n::Tr("Ajouter en ami")))
            chat->QueueNameAction(ChatWindow::NameAction::kFriendAdd, wire);
          ImGui::Separator();
        }
        if (ImGui::MenuItem(i18n::Tr("Copier le nom")))
          ImGui::SetClipboardText(target.player_name.c_str());
        break;
      }
      case Target::kUrl: {
        // ── Les deux autorisations d'image, et elles ne se valent PAS ────────
        //
        // « Cette image » ne retient rien : le risque est celui d'un clic sur le
        // lien, que le joueur peut déjà faire. « Toujours » accorde en revanche
        // un accès AUTOMATIQUE et PERMANENT au survol — beaucoup plus engageant,
        // d'où les points de suspension et la confirmation qui suit.
        // ⚠ PAS derrière le réglage « aperçu au survol ». Ces deux entrées sont
        // des gestes EXPLICITES, et les cacher tant que le réglage est éteint
        // rendait la fonctionnalité introuvable : rien dans le menu, rien dans
        // l'infobulle, aucun moyen de savoir qu'elle existe.
        {
          const std::string host = imgprev::HostOfUrl(target.url.c_str());
          if (!imgprev::IsPreviewable(target.url.c_str())) {
            if (ImGui::MenuItem(i18n::Tr("Afficher cette image")))
              imgprev::AllowOnce(target.url.c_str());
            if (!host.empty()) {
              std::snprintf(cmd, sizeof(cmd), i18n::Tr("Toujours afficher %s..."),
                            host.c_str());
              if (ImGui::MenuItem(cmd)) {
                g_pending_host = host;
                g_host_confirm_requested = true;
              }
            }
            ImGui::Separator();
          }
        }
        if (ImGui::MenuItem(i18n::Tr("Ouvrir dans le navigateur"))) OpenUrl(target.url.c_str());
        // 🔴 L'adresse est écrite par un TIERS. La copier plutôt que l'ouvrir est
        // le geste prudent, et le menu doit l'offrir : personne ne peut juger un
        // lien sur les quelques caractères qui tiennent dans une ligne de chat.
        if (ImGui::MenuItem(i18n::Tr("Copier l'adresse")))
          ImGui::SetClipboardText(target.url.c_str());
        break;
      }
      default: break;
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor();
}

void DrawHostConfirm();  // définie juste en dessous

void DrawUrlConfirm() {
  // Les deux modales partagent le même appel unique par frame : celle de l'hôte
  // doit être dessinée AVANT le `return` anticipé de celle-ci, sinon elle ne
  // s'ouvrirait jamais quand aucune adresse n'attend confirmation.
  DrawHostConfirm();

  static const char* const kPopupId = "Ouvrir cette adresse ?";

  if (g_confirm_requested) {
    g_confirm_requested = false;
    ImGui::OpenPopup(kPopupId);
  }

  // Sous le curseur, là où le joueur vient de cliquer — et SANS voile : assombrir
  // tout l'écran pour une confirmation d'un clic serait disproportionné, et le
  // chat doit rester lisible pendant qu'on décide. Pas de bullet de titre non
  // plus : il n'y a rien à replier ici.
  const ImVec2 mouse = ImGui::GetMousePos();
  ro::SetNextRoModalPos(mouse.x, mouse.y, false);
  // Fermeture qui ne vient pas de nos boutons (Échap, clic ailleurs) : on
  // n'ouvre rien, et c'est exactement le bon défaut pour un garde-fou.
  if (!ro::BeginRoPopupModal(kPopupId)) return;
  // Échap doit fermer CETTE modale, pas la chatbox derrière elle.
  ro::SuppressEscapeStack();

  ImGui::TextUnformatted(i18n::Tr("Ce lien vient d'un autre joueur."));
  ImGui::TextUnformatted(i18n::Tr("Il ouvrira votre navigateur sur :"));
  ImGui::Spacing();
  // 🔴 L'adresse COMPLÈTE, schéma compris — c'est-à-dire celle qui partira
  // vraiment, pas celle qui est écrite dans la ligne de chat. Tout l'intérêt de
  // l'avertissement est là : le texte affiché et la destination peuvent n'avoir
  // aucun rapport. Colorée pour qu'on la distingue de la phrase qui l'annonce.
  const std::string full = FullUrl(g_pending_url.c_str());
  ImGui::TextColored(ImVec4(0.10f, 0.20f, 0.55f, 1.0f), "%s", full.c_str());
  ImGui::Spacing();

  bool close = false;
  if (ro::RoButton("Ouvrir")) {
    LaunchUrl(g_pending_url.c_str());
    close = true;
  }
  ImGui::SameLine();
  if (ro::RoButton("Annuler")) close = true;
  ImGui::SameLine();
  // Copier plutôt qu'ouvrir : le geste prudent doit être à portée ICI aussi, pas
  // seulement dans le menu contextuel qu'on vient de quitter.
  if (ro::RoButton("Copier")) {
    ImGui::SetClipboardText(full.c_str());
    close = true;
  }

  if (close) {
    g_pending_url.clear();
    ImGui::CloseCurrentPopup();
  }
  ro::EndRoPopupModal();
}

// ── Autoriser un HÔTE durablement ────────────────────────────────────────────
//
// 🔴 CETTE CONFIRMATION-CI EST LA PLUS INSISTANTE DES DEUX, et c'est
// contre-intuitif. Ouvrir un lien révèle l'adresse IP une fois, sur un geste
// délibéré. Autoriser un hôte la révèle AUTOMATIQUEMENT, à chaque survol, et
// jusqu'à révocation — le joueur ne fera plus jamais le geste, et c'est
// justement ce qui rend l'accord lourd de conséquences.
//
// Le texte dit donc ce qui est accordé, pas ce qu'on gagne.
void DrawHostConfirm() {
  static const char* const kPopupId = "Autoriser ce site ?";

  if (g_host_confirm_requested) {
    g_host_confirm_requested = false;
    ImGui::OpenPopup(kPopupId);
  }
  const ImVec2 mouse = ImGui::GetMousePos();
  ro::SetNextRoModalPos(mouse.x, mouse.y, false);
  if (!ro::BeginRoPopupModal(kPopupId)) return;
  ro::SuppressEscapeStack();

  ImGui::TextUnformatted(i18n::Tr("Les images de ce site se chargeront"));
  ImGui::TextUnformatted(i18n::Tr("automatiquement, au simple survol :"));
  ImGui::Spacing();
  ImGui::TextColored(ImVec4(0.10f, 0.20f, 0.55f, 1.0f), "%s",
                     g_pending_host.c_str());
  ImGui::Spacing();
  ImGui::TextUnformatted(i18n::Tr("Ce serveur pourra alors voir que vous etes"));
  ImGui::TextUnformatted(i18n::Tr("en ligne, et depuis quelle adresse."));
  ImGui::Spacing();
  ImGui::TextDisabled(i18n::Tr("Reglages du chat pour revenir dessus."));
  ImGui::Spacing();

  bool close = false;
  if (ro::RoButton("Autoriser")) {
    imgprev::AllowHost(g_pending_host.c_str());
    // Persistance : la liste vit dans imgprev, la chatbox n'en garde que la
    // forme sérialisée que les réglages savent écrire.
    if (ChatWindow* chat = Bourgeon::Instance().chat_window())
      chat->url_hosts() = imgprev::UserHostsCsv();
    close = true;
  }
  ImGui::SameLine();
  if (ro::RoButton("Annuler")) close = true;

  if (close) {
    g_pending_host.clear();
    ImGui::CloseCurrentPopup();
  }
  ro::EndRoPopupModal();
}

}  // namespace links
