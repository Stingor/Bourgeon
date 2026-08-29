#pragma once

#include <string>

// ── chat_commands : les commandes que le joueur CROIT que le serveur connaît ──
//
// Un nouveau venu tape ce qu'il tapait ailleurs : `/atlas`, `/settings`,
// `/warp`, `@showloot`. Aucune n'existe — ni dans la table slash du client, ni
// dans les atcommands de Moonlight — et le client ne le dit PAS : une commande
// inconnue retombe sur le `case 0` de `Chat_HandleChatMessage` (« ce n'est pas
// une commande »), c'est-à-dire sur du TEXTE ORDINAIRE. La tentative part donc
// en chat public, sous les yeux de tout le monde, et le joueur n'apprend rien
// sinon qu'il s'est trompé devant témoins.
//
// Ce module est le filtre qui manque : une table de noms, consultée AVANT que
// la ligne n'atteigne le fil (`ChatWindow::QueueSend`). Trois réponses
// possibles, combinables :
//   • ouvrir ce que la commande DÉSIGNE chez nous (l'Atlas, les réglages, la
//     Navigation) — la commande devient un raccourci qui marche ;
//   • répondre une ligne d'orientation dans le chat, pour les gestes qui n'ont
//     pas d'équivalent ici (`/autoattack`, `/warp`) ;
//   • RÉÉCRIRE vers la commande serveur qui fait le travail (`/cmd` →
//     `@commands`), envoyée par le pipeline complet du client.
//
// ── CE QUE CE MODULE NE FAIT PAS ─────────────────────────────────────────────
// Il n'intercepte RIEN d'autre que les noms de sa table. Une commande native du
// client (`/who`, `/navi`, `/help`…) et une atcommand du serveur (`@autoloot`,
// `@mobinfo`…) traversent sans être vues : les manger serait remplacer un chemin
// qui marche par une copie qui dérive.
//
// 🔴 AUCUN NOM DE LA TABLE N'EXISTE DANS LE CLIENT, et ce n'est pas une
// impression : les noms ont été cherchés dans les chaînes de
// `Moonlight-Destiny.exe` (la table de `ChatCmd_LookupSlashCommandTable` s'y lit
// en clair, « /set1 /set2 /set3 /camera /tp /fog … »), avec un témoin négatif
// pour prouver que la recherche voit quelque chose. Résultat : zéro occurrence
// pour tous, SAUF `/tp` — qui EST une commande native, à côté de `/camera` et
// `/fog`, et qu'on prend donc sciemment au client. C'est le seul cas, et il est
// délibéré : sur Moonlight, `/tp` ne fait rien d'utile, et c'est ce que les
// joueurs tapent pour se déplacer.
//
// ⚠ AVANT D'AJOUTER UN NOM : refaire cette vérification. Un nom déjà connu du
// client serait CONFISQUÉ en silence, et la panne se lirait « telle commande ne
// marche plus depuis la mise à jour » — sans rien pour la relier à ce fichier.
//
// ⚠ Seule la barre de saisie MODERNE passe par ici. Interface native (chatbox du
// client), la ligne suit le chemin d'origine et repart en chat public comme
// avant : c'est le repli assumé du projet, pas un oubli.

namespace chatcmd {

// Ce qu'une ligne interceptée déclenche. Les champs sont indépendants : une même
// entrée peut à la fois DIRE quelque chose et OUVRIR une fenêtre.
struct Outcome {
  // Faux = ce n'est pas une de nos commandes ; l'appelant envoie la ligne comme
  // il l'aurait fait sans nous. Les autres champs ne veulent alors rien dire.
  bool handled = false;

  // Ligne à afficher dans le chat du joueur, en UTF-8 déjà traduit, ou nullptr.
  // 🔴 Le pointeur appartient au catalogue i18n et meurt au prochain changement
  // de langue : à consommer dans la frame, jamais à ranger.
  const char* message = nullptr;

  // Identifiant d'action locale (ouvrir une fenêtre), ou nullptr. 🔴 À JOUER
  // HORS FRAME IMGUI, par `RunAction` : une action peut demander une fenêtre
  // native, ce qui gèle le client depuis un rendu.
  const char* action = nullptr;

  // Ligne à envoyer À LA PLACE, ou nullptr. C'est du texte de chat ordinaire
  // (« @commands ») : l'appelant l'arme comme n'importe quelle commande.
  const char* rewrite = nullptr;

  // Ce que le joueur a écrit APRÈS le nom de la commande, blancs de tête ôtés.
  // Vide s'il n'a rien mis. Transmis tel quel à `RunAction`.
  std::string argument;
};

// Examine une ligne TAPÉE, en UTF-8. Rend `handled == false` pour tout ce qui
// n'est pas une de nos commandes — y compris une phrase ordinaire.
//
// ⚠ Appelle `i18n::Tr` : à n'appeler que depuis le fil de rendu, comme tout ce
// qui traduit.
Outcome Try(const char* utf8_line);

// Joue l'action désignée par `Try`. `argument` est le reste de la ligne (jamais
// nul ; vide s'il n'y en avait pas).
//
// ⚠ HORS FRAME IMGUI (`ChatWindow::FlushPending`, donc depuis OnProcessInput).
void RunAction(const char* action, const char* argument);

}  // namespace chatcmd
