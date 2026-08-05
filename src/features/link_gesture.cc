#include "features/link_gesture.h"

#include <Windows.h>
#include <shellapi.h>  // ShellExecuteA (site, bestiaire, adresses du chat)

#include <cstdio>
#include <cstring>  // _strnicmp (adresse « www.… » sans schéma)

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"     // liste alootid
#include "features/windows/cashshop_window.h"       // disponibilité au vote shop
#include "features/windows/chat_window.h"           // poser un lien, armer une commande
#include "features/windows/item_desc_window.h"      // itemdesc::OpenItemDbPage
#include "features/windows/monster_info_window.h"   // fiche d'un monstre
#include "imgui.h"
#include "ui/ro_imgui.h"                            // ro::SetHoverCursor

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
    // 🔴 L'adresse ENTIÈRE avant de cliquer. Une ligne de chat tronque, et c'est
    // précisément sur ce qu'on ne voit pas qu'un lien trompe.
    //
    // ⚠ Texte SOMBRE, même raison que le menu ci-dessous : l'infobulle hérite du
    // texte CLAIR que la chatbox pousse pour son fond sombre, alors que le fond
    // d'une infobulle, lui, est clair. L'adresse s'affichait en blanc sur blanc —
    // donc invisible, exactement là où elle est le plus utile.
    ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(target.url.c_str());
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
        if (ImGui::MenuItem("Base de données du site")) itemdesc::OpenItemDbPage(id);
        if (ImGui::MenuItem("Lien dans le chat")) PostToChat(target);
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
                          "Vote shop : ajouter au panier (%d pts)", price);
            if (ImGui::MenuItem(label)) shop->OpenWithItem(id);
          }
        }
        ImGui::Separator();
        // Ramassage automatique : la MÊME liste que l'overlay de la description
        // (elle vit dans MoonlightUi, qui la tient à jour avec le serveur) —
        // surtout pas une seconde copie qui divergerait.
        if (auto* mu = Bourgeon::Instance().moonlight_ui()) {
          const bool looted = mu->IsAlootId(id);
          if (ImGui::MenuItem(looted ? "Retirer de l'alootid" : "Ajouter à l'alootid")) {
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
        if (ImGui::MenuItem("Fiche du monstre")) OpenMobSheet(target.mob_id);
        if (ImGui::MenuItem("Bestiaire du site")) OpenMobDbPage(target.mob_id);
        if (ImGui::MenuItem("Lien dans le chat")) PostToChat(target);
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
      case Target::kUrl: {
        if (ImGui::MenuItem("Ouvrir dans le navigateur")) OpenUrl(target.url.c_str());
        // 🔴 L'adresse est écrite par un TIERS. La copier plutôt que l'ouvrir est
        // le geste prudent, et le menu doit l'offrir : personne ne peut juger un
        // lien sur les quelques caractères qui tiennent dans une ligne de chat.
        if (ImGui::MenuItem("Copier l'adresse"))
          ImGui::SetClipboardText(target.url.c_str());
        break;
      }
      default: break;
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor();
}

void DrawUrlConfirm() {
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

  ImGui::TextUnformatted("Ce lien vient d'un autre joueur.");
  ImGui::TextUnformatted("Il ouvrira votre navigateur sur :");
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

}  // namespace links
