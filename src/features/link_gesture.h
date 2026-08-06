#pragma once

// ── links:: — LES GESTES D'UN LIEN, UNE SEULE FOIS ───────────────────────────
//
// Un « lien » est une RÉFÉRENCE : le nom d'un objet dans une ligne de chat, un
// nom de monstre dans une table de drops, une adresse web. Ce n'est PAS une
// cellule d'inventaire — là, le clic gauche a déjà un métier (sélectionner,
// utiliser, glisser) et rien de ce qui suit ne s'y applique.
//
// 🔴 LA CONVENTION, valable partout où l'on affiche un lien :
//
//   clic GAUCHE  → la description en jeu (objet : la fenêtre de description ;
//                  monstre : sa fiche ; adresse : le navigateur)
//   clic DROIT   → le menu contextuel, le même partout
//   MAJ + clic   → le lien dans la barre de chat
//
// Elle vivait en trois exemplaires — la chatbox (log), la chatbox (barre de
// saisie), la table « qui le drop » — et ils avaient déjà divergé : le clic droit
// ouvrait la fiche ici et un menu là, le Maj+clic n'existait qu'à un endroit. Un
// quatrième appelant aurait recopié l'un des trois. D'où ce module : les surfaces
// décrivent CE QU'ELLES MONTRENT (un `Target`), pas ce que ça fait.
//
// ⚠ CE QUI DOIT RESTER DIFFÉRÉ. Ouvrir une description passe par le natif, ce qui
// est proscrit pendant une frame ImGui (modale bloquante qui relance le rendu) :
// `OpenDescription` ARME (itemcell::DeferDesc*), et Bourgeon::OnProcessInput joue.
// Idem pour les commandes du menu, armées via ChatWindow::QueueCommand.

#include <cstdint>
#include <string>

#include "features/item_cell.h"  // itemcell::ChatLink

namespace links {

// Ce qu'un lien DÉSIGNE. Copiable et conservable : le menu contextuel s'ouvre à
// la frame suivante, l'appelant doit donc garder sa cible sous la main.
struct Target {
  enum Kind : uint8_t { kNone = 0, kItem, kMob, kUrl, kPlayer };
  uint8_t kind = kNone;

  // kItem — la balise RELUE, pas seulement l'id : elle porte le refine, les
  // cartes, le grade, le forgeron. Ouvrir par l'id ne montrerait que l'item de
  // base, c'est-à-dire justement pas celui qu'on a cliqué.
  itemcell::ChatLink item;

  // kMob — le nom voyage avec, parce que le client ne sait pas nommer un
  // monstre (ni mob_db ni le paquet de sa fiche ne le lui donnent).
  uint32_t mob_id = 0;
  uint8_t  mob_rank = 0;  // 0 = normal, 1 = boss, 2 = MVP
  std::string mob_name;   // UTF-8

  std::string url;    // kUrl

  // kPlayer — le pseudo d'un joueur, tel qu'une ligne de chat le porte. C'est
  // TOUT ce dont on dispose : pas d'AID, donc aucune des actions du menu
  // contextuel d'entité (qui résout sa cible par l'acteur à l'écran) n'est
  // utilisable ici. Celles du menu ci-dessous prennent toutes un nom.
  std::string player_name;  // UTF-8

  std::string label;  // ce que le menu affiche en tête (UTF-8)

  bool valid() const { return kind != kNone; }
};

Target FromItem(const itemcell::ChatLink& link, const char* label_utf8);
// Pour les listes qui n'ont qu'un id (membres d'un combo, cartes serties…) :
// l'objet de BASE, sans refine ni cartes — il n'y a rien d'autre à en dire.
Target FromItemId(uint32_t item_id, const char* label_utf8);
Target FromMob(uint32_t mob_id, int rank, const char* name_utf8);
Target FromUrl(const char* url);
// Le pseudo d'un joueur (UTF-8). ⚠ Le clic GAUCHE n'a rien à ouvrir ici — un
// joueur n'a pas de « description » — donc seuls le menu et le Maj+clic jouent.
Target FromPlayer(const char* name_utf8);

// Le geste RECONNU, sans rien jouer. Pour les surfaces qui ont leur propre façon
// d'ouvrir une description : les cartes et les membres de combo d'une fenêtre de
// description REMPLACENT l'objet affiché (comme un lien de carte natif) au lieu
// d'ouvrir une seconde fenêtre — c'est leur description à elles, et la convention
// porte sur le GESTE, pas sur la façon de l'honorer.
enum class Gesture { kNone, kDescription, kMenu, kChatLink };
Gesture Hit(const Target& target, bool hovered);

// Les gestes, sur une zone SURVOLÉE. `hovered` est fourni par l'appelant : toutes
// les surfaces ne sont pas des items ImGui (le log du chat teste ses rectangles à
// la main). Pose le curseur « main », joue le clic gauche et le Maj+clic, et
// renvoie **true quand le menu doit s'ouvrir** — l'appelant fait l'OpenPopup
// lui-même, l'identifiant d'un popup se hachant avec la pile d'ids de SA fenêtre.
bool Gestures(const Target& target, bool hovered);

// L'APERÇU au survol, à appeler quand la zone est survolée : pour un objet, la
// description simple (le même tooltip RO que les viewers) ; pour un monstre, une
// fiche compacte — sprite, niveau, race, élément, PV. Il crée son propre popup,
// donc rien à ouvrir ni à fermer autour.
//
// 🔴 Un lien de MONSTRE déclenche une demande au serveur la première fois (la
// fiche n'est pas dans le client). C'est borné : une seule demande par monstre,
// et survoler n'ouvre ni ne change la fiche affichée.
void HoverPreview(const Target& target);

// Le menu contextuel. À appeler dans la MÊME fenêtre ImGui que l'OpenPopup, avec
// la cible que l'appelant a mise de côté au clic droit.
void DrawMenu(const char* popup_id, const Target& target);

// Les actions, utilisables seules (un bouton, une entrée de menu à soi).
void OpenDescription(const Target& target);
bool PostToChat(const Target& target);

// ── L'avertissement avant d'ouvrir une adresse ───────────────────────────────
// Une adresse postée dans le chat vient d'un TIERS, et le texte affiché n'a
// aucun rapport obligé avec la destination. Ouvrir un lien passe donc par une
// confirmation qui montre l'adresse COMPLÈTE — sauf si le joueur a retiré le
// garde-fou dans les réglages de la chatbox, ce qui est son droit.
//
// 🔴 À APPELER UNE FOIS PAR FRAME, hors de toute fenêtre, par la surface qui
// offre des liens d'adresse — aujourd'hui la seule est la chatbox (`Run::kUrl`).
// L'ouverture ImGui est DIFFÉRÉE parce que `OpenUrl` part d'un menu contextuel,
// donc d'une autre pile d'ID : un `OpenPopup` posé là ne trouverait pas la
// modale. Même piège et même remède que `ro::OpenQuantityPrompt`.
void DrawUrlConfirm();

}  // namespace links
