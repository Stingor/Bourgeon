#pragma once

#include <cstddef>
#include <string>

// ── msgstring_override : TOUTE la table de messages du client, traduisible ───
// (client 20250716, base 0x400000 — RE : docs/game_option_re.md §3.8)
//
// Le client range ses 4360 textes d'interface dans `data\msgstringtable.csv` et
// n'en parle qu'une langue à la fois — l'anglais chez Moonlight. Ce module
// DÉTOURNE `MsgStringTable_GetById` (0x00A9ED30) et répond depuis un catalogue
// Bourgeon quand il en a un.
//
// 🔴 LA PORTÉE EST LE JEU ENTIER, pas seulement nos fenêtres. Le détour est posé
// sur le lecteur que TOUT le client appelle : bulles d'aide, messages d'erreur,
// boutons des fenêtres natives, confirmations. Bourgeon n'affiche que 48 de ces
// ids ; les 4300 autres n'étaient jusqu'ici traduisibles d'aucune façon.
//
// ── Trois garde-fous, tous nécessaires ──────────────────────────────────────
//
// 1. **Les spécificateurs `printf` sont un contrat.** 515 entrées en portent
//    (« Error Code - %08d », « You got %s (%d). »). Le client passe ses arguments
//    à l'aveugle : une traduction qui en perd un, en ajoute un ou en change
//    l'ordre ne donne pas un texte fautif, elle donne un CRASH. Chaque traduction
//    est donc comparée à l'original AU CHARGEMENT, et REJETÉE si la séquence
//    diffère — l'original reste, et le refus part au journal.
//
// 2. **L'encodage n'est pas l'UTF-8.** Nos catalogues sont en UTF-8 ; le natif
//    dessine dans la code-page du CLIENT, lue à l'exécution (`0x0159B818` :
//    1252 en Europe, 949 en Corée). Les chaînes sont donc converties UNE FOIS au
//    chargement par `ro::Utf8ToLocal`. ⚠ En servicetype coréen, les accents
//    français n'existent pas dans la code-page et deviendraient des « ? » : c'est
//    une limite du client, pas un bug d'ici.
//
// 3. **Le pointeur rendu doit SURVIVRE.** Le client garde ce `const char*` et
//    peut le relire plus tard. Les chaînes converties vivent donc dans un stockage
//    persistant, jamais dans un tampon rotatif — et un rechargement de langue ne
//    libère rien tant que le processus vit.
//
// ── Pourquoi la clé du catalogue est `MSI_*` et non l'id ─────────────────────
// Les ids se relèvent mal et se décalent d'une version à l'autre : `MSI_OPTION_ESC`
// était noté 4240 dans notre doc alors qu'il vaut 4241 — et une erreur d'un cran
// n'a pas l'air d'une erreur, elle affiche un autre texte plausible. La table
// `clé -> id` est donc refaite AU DÉMARRAGE depuis le csv du client lui-même :
// elle suit ses mises à jour toute seule.
//
// ⚠ Le csv est OBLIGATOIRE côté client (vérifié en jeu : sans lui il ne démarre
// pas), donc il est toujours là quand on le lit.

namespace msgoverride {

// Charge le catalogue de la langue courante et pose le détour au premier appel.
// Rejouable : appeler après un changement de langue recharge tout.
//
// Sans catalogue pour la langue active — français inclus, puisque le client parle
// déjà anglais et qu'un `fr.yaml` peut manquer — le détour reste posé mais ne
// répond rien, et chaque appel repart au natif. Aucun coût mesurable.
void Reload();

// Le texte traduit d'un id, ou nullptr s'il n'y en a pas. Pointeur STABLE.
// Appelée par le détour ; exposée pour que `msgstr::` puisse la court-circuiter
// sans repasser par le natif.
const char* Lookup(int id);

// ── État, pour le panneau de réglages et le journal ──────────────────────────
struct Stats {
  std::size_t entries = 0;    // traductions actives
  std::size_t rejected = 0;   // refusées : séquence de formats différente
  std::size_t table = 0;      // entrées lues dans le csv du client
  bool hooked = false;        // détour posé
  std::string language;       // code de la langue chargée
};
Stats Current();

}  // namespace msgoverride
