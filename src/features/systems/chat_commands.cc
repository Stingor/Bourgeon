#include "features/systems/chat_commands.h"

#include <cstring>

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"    // le panneau de réglages
#include "features/windows/craft_atlas.h"          // l'Atlas des recettes
#include "features/windows/navigation_window.h"    // la recherche de lieux
#include "utils/i18n.h"

namespace chatcmd {
namespace {

// ── Les actions ──────────────────────────────────────────────────────────────
// Des identifiants et non des pointeurs de fonction : ils traversent une frame
// (armés pendant le rendu, joués au tick suivant) et un identifiant se compare,
// se journalise et se relit — ce qu'un pointeur ne fait pas. Même choix que le
// catalogue de raccourcis (`hotkeys::Invoke`), pour la même raison.
constexpr char kActionAtlas[]      = "atlas";
constexpr char kActionSettings[]   = "settings";
constexpr char kActionNavigation[] = "navigation";

// Une commande : ses noms, et ce qu'elle déclenche.
struct Entry {
  // Le premier est le nom canonique, les suivants ses synonymes. nullptr = fin
  // de liste — la table en aligne au plus quatre, ce qui a toujours suffi.
  const char* names[4];
  const char* action;      // action locale, ou nullptr
  const char* message_fr;  // ligne d'orientation (clé i18n), ou nullptr
  const char* rewrite;     // ligne envoyée à la place, ou nullptr
};

// 🔴 CE QUI JUSTIFIE CHAQUE ENTRÉE : le joueur a un BUT, et ce but existe ici
// sous un autre nom. La table le traduit — elle n'invente pas une fonction que
// le serveur n'a pas.
const Entry kCommands[] = {
    // Les trois qui OUVRENT. Rien à dire dans le chat : la fenêtre qui s'ouvre
    // est la réponse, et une ligne de plus ne ferait que la commenter.
    {{"/atlas", "/recipes", "/recettes"}, kActionAtlas, nullptr, nullptr},
    {{"/settings", "/config", "/configs", "/moon"}, kActionSettings, nullptr,
     nullptr},

    // Les deux qui EXPLIQUENT. Elles désignent des gestes que Moonlight refuse
    // (l'auto-attaque) ou rend inutiles (le déplacement gratuit par la Warp
    // Agent) : il n'y a rien à ouvrir, seulement à répondre.
    {{"/autoattack", "@autoattack"},
     nullptr,
     "Pas d'auto-attaque sur ce serveur, ici il faut jouer soi-même.",
     nullptr},
    {{"/warp", "/tp", "/teleport"},
     nullptr,
     "La Warp Agent est là pour vous servir.",
     nullptr},

    // Celle qui RÉÉCRIT. La liste des commandes existe déjà, côté serveur, et
    // elle est à jour par construction — la recopier ici, c'est promettre une
    // seconde liste qui divergera dès la prochaine atcommand ouverte aux
    // joueurs.
    {{"/cmd", "/command", "/commands"}, nullptr, nullptr, "@commands"},

    // Les deux `@` que le serveur ne connaît pas. rAthena leur répond bien
    // « commande inconnue », mais cette réponse n'apprend rien : ici on nomme
    // ce qui, chez nous, fait le travail.
    {{"@npclocation"},
     kActionNavigation,
     "Recherche de PNJ, de monstre ou de carte : c'est la fenêtre Navigation.",
     nullptr},
    {{"@showloot"},
     nullptr,
     "Ramassage automatique : @autoloot, @autolootitem <objet>, @autoloottype "
     "<type>. Pour savoir qui lâche quoi : @whodrops <objet>, ou la fiche du "
     "monstre.",
     nullptr},
};

bool IsBlank(char c) { return c == ' ' || c == '\t'; }

// Comparaison INSENSIBLE À LA CASSE : « /Atlas » est la même intention que
// « /atlas », et refuser la majuscule renverrait le joueur exactement là d'où on
// essaie de le sortir. Les noms de la table sont de l'ASCII pur, donc `_stricmp`
// suffit — aucun risque de casse dépendante de la locale.
const Entry* Find(const char* name) {
  for (const Entry& entry : kCommands) {
    for (const char* candidate : entry.names) {
      if (candidate == nullptr) break;
      if (_stricmp(candidate, name) == 0) return &entry;
    }
  }
  return nullptr;
}

}  // namespace

Outcome Try(const char* utf8_line) {
  Outcome out;
  if (utf8_line == nullptr) return out;
  // Une commande commence par « / » (client) ou « @ » (serveur). Tout le reste
  // est une phrase, et une phrase ne se fait pas fouiller.
  if (utf8_line[0] != '/' && utf8_line[0] != '@') return out;

  // Le nom s'arrête au premier blanc ; ce qui suit est l'argument.
  size_t length = 0;
  while (utf8_line[length] != '\0' && !IsBlank(utf8_line[length])) ++length;
  const std::string name(utf8_line, length);

  const Entry* entry = Find(name.c_str());
  if (entry == nullptr) return out;

  const char* rest = utf8_line + length;
  while (IsBlank(*rest)) ++rest;

  out.handled  = true;
  out.action   = entry->action;
  out.rewrite  = entry->rewrite;
  out.message  = (entry->message_fr != nullptr) ? i18n::Tr(entry->message_fr) : nullptr;
  out.argument = rest;
  return out;
}

void RunAction(const char* action, const char* argument) {
  if (action == nullptr) return;
  if (argument == nullptr) argument = "";
  Bourgeon& app = Bourgeon::Instance();

  // 🔴 OUVRIR, jamais basculer. `Toggle` est le geste d'un RACCOURCI, où la même
  // touche fait l'aller et le retour ; une commande TAPÉE, elle, est une demande
  // dans un seul sens — la fermer parce qu'elle était déjà ouverte se lirait
  // comme une commande qui n'a pas marché.
  if (std::strcmp(action, kActionAtlas) == 0) {
    if (auto* atlas = app.craft_atlas())
      if (!atlas->IsOpen()) atlas->Toggle();
    return;
  }
  if (std::strcmp(action, kActionSettings) == 0) {
    // `ShowWindow` déplie AUSSI la fenêtre repliée et lui rend le focus : c'est
    // le même point d'entrée que le menu Échap, et il n'y en a pas d'autre.
    if (auto* ui = app.moonlight_ui()) ui->ShowWindow();
    return;
  }
  if (std::strcmp(action, kActionNavigation) == 0) {
    auto* navigation = app.navigation_window();
    if (navigation == nullptr) return;
    // Un argument, c'est déjà la question posée : « @npclocation Kafra » veut
    // une recherche, pas une fenêtre vide à remplir soi-même. `monsters_only`
    // reste faux — le joueur qui écrit ce nom-là cherche un PNJ, mais rien ne
    // dit qu'il ne cherche pas une carte, et la pastille l'exclurait.
    if (argument[0] != '\0') {
      navigation->OpenSearch(argument, false);
      return;
    }
    if (!navigation->IsOpen()) navigation->Toggle();
    return;
  }
}

}  // namespace chatcmd
