#ifndef BOURGEON_FEATURES_FX_STYLE_SYNC_H_
#define BOURGEON_FEATURES_FX_STYLE_SYNC_H_

#include <cstdint>

#include "features/plugin.h"
#include "ui/palette_ramps.h"

// Propagation des couleurs de corps : la recette du joueur part au serveur, et
// celles des joueurs en vue reviennent pour être appliquées à leurs acteurs.
//
// ── Ce qui circule, et ce qui ne circule PAS ─────────────────────────────────
// 🔴 Aucune couleur ne passe sur le réseau. Une recette ne porte que des
// DÉCALAGES par rampe ; chaque client recalcule la palette lui-même, à partir du
// `.spr` qu'il possède déjà et de la palette de vêtement que le serveur a
// choisie pour cet acteur. C'est ce qui la fait tenir en 50 octets au lieu du
// kilo-octet d'une palette — et c'est aussi ce qui la rend fragile : les deux
// clients DOIVENT détecter exactement les mêmes rampes sur les mêmes octets,
// d'où l'algorithme déterministe de `ui/palette_ramps` et sa référence Python.
//
// ── Le format de trame ───────────────────────────────────────────────────────
// 🔴 La struct `ro::RampAdjust` n'est JAMAIS recopiée telle quelle : elle porte
// un octet de bourrage (int16 + int8 + int8 + uint8 = 5 utiles, 6 alloués) dont
// la position dépend du compilateur. Tout passe champ par champ.
//
// Un RÉGLAGE de rampe, 5 octets :
//     [teinte:int16 LE][saturation:int8][luminosité:int8][absolu:uint8]
//
// CZ_BOURGEON_STYLE (0x0F26), client -> serveur, 52 octets fixes :
//     [opcode:2][longueur:2]        = 4
//     [version:1]                   = kWireVersion
//     [drapeaux:1]                  bit 0 = EFFACER (le reste est ignoré)
//     [palette:int16 LE]            palette de corps officielle 1..553, -1 = celle du serveur
//     [cheveux:int16 LE]            palette de cheveux 1..251, -1 = celle du personnage
//     [coiffure:int16 LE]           coupe 1..80, -1 = celle du personnage
//     [réglages: 8 × 5]             = 40
//
// ZC_BOURGEON_STYLES (0x0F27), serveur -> client, longueur variable :
//     [opcode:2][longueur:2][nombre:2]              = 6
//     puis `nombre` fois, 52 octets chacun :
//     [gid:4][version:1][drapeaux:1][palette:int16][cheveux:int16][coiffure:int16]
//     [réglages: 8 × 5]
//
// ⚠ La coiffure d'une entrée ZC n'est PAS posée sur l'acteur : le serveur l'a
// déjà appliquée par `pc_changelook`, et son ZC_SPRITE_CHANGE natif est arrivé
// avant nous. Elle n'y figure que pour que NOTRE propre écho porte une allure
// complète — ce qui amorce l'éditeur et remplit le cache local.
//
// L'envoi par LOT n'est pas un luxe : entrer sur une carte peuplée fait arriver
// des dizaines de joueurs d'un coup, et un paquet par joueur multiplierait le
// nombre de trames pour rien.
//
// ── Pourquoi la trame ne porte NI la classe NI le sprite ─────────────────────
// La tentation est grande : les index de palette n'ont aucune signification
// commune d'un corps à l'autre (mesuré : un Dragon Knight utilise 7-73, un
// Creator 112-223), donc on voudrait vérifier que la recette correspond bien au
// corps affiché, et la refuser sinon.
//
// 🔴 Ce serait une erreur, et la raison est une contrainte de COHÉRENCE. Quand
// le joueur monte à cheval, son sprite de corps change (`knight` ->
// `knight_riding`) et SON PROPRE éditeur ré-applique la même recette, par index,
// sur le nouveau sprite. Si les autres clients refusaient de le faire, il se
// verrait autrement que les autres le voient. La règle doit donc être la même
// partout : on applique PAR INDEX DE RAMPE, sans condition.
//
// Ce n'est pas arbitraire non plus : les rampes sont triées par surface, donc la
// rampe N est « la N-ième pièce la plus visible » — une correspondance qui garde
// du sens d'un corps à l'autre. La version, elle, sert à refuser proprement une
// trame d'un futur format (un `kMaxRamps` différent, par exemple).

namespace fx {
namespace style_sync {

// Tailles de trame — partagées avec le serveur, qui doit les recopier.
constexpr int kRampBytes    = 5;
constexpr int kAdjustBytes  = ro::kMaxRamps * kRampBytes;            // 40
constexpr int kCzBytes      = 4 + 1 + 1 + 2 + 2 + 2 + kAdjustBytes;  // 52
constexpr int kZcHeadBytes  = 4 + 2;                                 // 6
constexpr int kZcEntryBytes = 4 + 1 + 1 + 2 + 2 + 2 + kAdjustBytes;  // 52

// 🔴 À incrémenter dès que la disposition change — et `ro::kMaxRamps` en fait
// partie, puisqu'il dimensionne le bloc de réglages. Un client qui reçoit une
// version inconnue IGNORE l'entrée : mieux vaut des couleurs natives qu'une
// palette lue de travers.
// v2 (2026-08-11) : `kMinLength` passé de 3 à 1 dans la détection de rampes. Les
// frontières et le CLASSEMENT des rampes changent, donc une recette v1 désigne
// d'autres pièces — il faut la jeter, pas la réinterpréter.
// v3 (2026-08-11) : ajout de `palette_id`, la palette de vêtement officielle sur
// laquelle la recette s'applique. Une v2 se MIGRE (palette_id = -1, la palette du
// serveur) au lieu d'être jetée : ses réglages de rampes restent valides.
// v4 (2026-08-11) : ajout de `hair_palette_id`. Une v3 se MIGRE (cheveux = ceux
// du personnage) ; ses réglages de corps restent valides.
// v5 (2026-08-12) : ajout de `hair_style`, la COIFFURE. Une v4 se MIGRE (coupe =
// celle du personnage).
//
// 🔴 Elle est la seule entrée que le serveur INTERPRÈTE. Tout le reste, il le
// range sans le comprendre ; celle-ci, il l'applique par `pc_changelook`, ce qui
// l'écrit dans `sd->status.hair`, la sauvegarde avec le personnage et l'annonce
// à la zone par le ZC_SPRITE_CHANGE natif — clients vanilla compris. Elle est
// ici parce que POUR LE JOUEUR la coiffure fait partie du style, et que c'est
// son point de vue qui commande le format.
constexpr uint8_t kWireVersion = 5;

constexpr uint8_t kFlagClear = 0x01;  // « ce joueur n'a plus de recette »

// 🔴 Le garde-fou du protocole. Ces tailles sont recopiées à la main côté
// serveur (moonlight : packets_struct.hpp, PACKET_CZ_BOURGEON_STYLE et
// PACKET_BOURGEON_PALETTE_ENTRY). Faire varier `ro::kMaxRamps` les changerait
// SANS ERREUR — le client enverrait 50 octets là où le serveur en attend 46, et
// rAthena rejetterait la trame en silence. Ces assertions transforment cet
// accident muet en échec de compilation, à charge de bumper `kWireVersion` et de
// mettre le serveur à jour.
static_assert(kCzBytes == 52, "trame CZ modifiée : bumper kWireVersion et mettre à jour moonlight");
static_assert(kZcEntryBytes == 52, "entrée ZC modifiée : bumper kWireVersion et mettre à jour moonlight");

// Envoie la recette courante du joueur. À appeler quand il VALIDE, pas à chaque
// mouvement de curseur : l'aperçu est déjà local et immédiat, et le serveur n'a
// que faire des soixante états intermédiaires d'un glissement.
void SendRecipe(const ro::PaletteRecipe& recipe);

// Dit au serveur d'oublier la recette : le joueur revient à son apparence
// native, pour lui comme pour les autres.
void SendClear();

// Numéro le plus élevé d'une coiffure proposée. Même convention et même valeur
// que la grille de création de personnage (`kMaxHairStyle`, features/windows/
// char_select.cc).
//
// ⚠ C'est une borne d'INTERFACE, pas la règle : la vraie borne est
// `max_hair_style` côté serveur (80 dans `conf/import/`, et non les 42 que
// laisse croire `conf/battle/client.conf`). `pc_changelook` BORNE au lieu de
// refuser, donc un écart ici ne casse rien — il donnerait juste au joueur une
// coiffure clampée sans le lui dire.
constexpr int kHairStyleMax = 80;

// Le module est-il en place ? (false = paquet non envoyé, p. ex. hors session.)
bool Available();

// L'éditeur est-il ouvert ?
//
// 🔴 Sert à trancher le sort de NOTRE PROPRE recette quand elle nous revient du
// serveur. Éditeur ouvert : on l'ignore — le joueur est en train de régler, et
// lui reposer la dernière version validée ferait sauter ses curseurs en arrière.
// Éditeur fermé : on l'APPLIQUE, et c'est ainsi que ses couleurs reviennent à
// chaque connexion sans qu'il ait rien à rouvrir.
void SetLocalEditing(bool editing);

// La dernière recette reçue pour NOTRE acteur, s'il y en a une. L'éditeur s'en
// sert pour démarrer sur les couleurs que le joueur a déjà partagées, au lieu
// d'une recette vide qui les effacerait à la première application.
bool LocalRecipe(ro::PaletteRecipe* out);

// Oublie NOTRE recette : celle du registre d'application ET celle qui amorce
// l'éditeur.
//
// 🔴 À appeler par qui retire l'injection de sa propre initiative. Sans ça, la
// recette reste en attente dans le registre et la boucle d'application la
// REPOSE au tick suivant — le personnage reprend ses anciennes couleurs alors
// que l'éditeur affiche des réglages à zéro. C'est le bug qu'a produit le bouton
// « Supprimer mes couleurs » (2026-08-11).
void ForgetLocal();

}  // namespace style_sync
}  // namespace fx

// Le module lui-même. Enregistré dans Bourgeon::LoadPlugins.
class StyleSync : public Plugin {
 public:
  StyleSync();
  ~StyleSync() override;

  const char* name() const override { return "StyleSync"; }

  // 🔴 FIL RÉSEAU : on COPIE, on ne décode rien (cf. features/net_inbox.h).
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Fil principal, une fois par frame : le décodage.
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Fil principal, ~10 fois par seconde : l'application aux acteurs.
  //
  // 🔴 Pourquoi PAS dans HandlePacket, qui est pourtant déjà sur le bon fil : au
  // moment où la recette arrive, l'acteur n'existe souvent pas encore côté
  // client — le serveur annonce volontiers les joueurs d'une carte avant que
  // leurs sprites ne soient montés. Il faut donc RÉESSAYER, et un tick est le
  // bon rythme pour ça.
  void OnTick() override;

  // Chaque frame du monde de jeu : tentative de restauration depuis le cache.
  //
  // 🔴 Pas seulement au tick : celui-ci est bridé à ~100 ms, et la restauration
  // ne peut aboutir qu'une fois l'acteur monté. Essayer à chaque frame fait
  // arriver les couleurs à la PREMIÈRE image où c'est possible, au lieu d'un
  // dixième de seconde plus tard. Une fois faite, elle ne coûte qu'une
  // comparaison d'entiers.
  void OnRenderUI() override;

  // Écrans de login et de sélection de personnage.
  //
  // 🔴 C'est ici que les détours sont POSÉS, et c'est tout l'objet de cette
  // surcharge. Les poser depuis le monde de jeu — le seul endroit où ils
  // l'étaient — les mettait en place APRÈS l'apparition de l'acteur : le détour
  // ne l'avait donc pas vu, `KnownActor` rendait nul, et les couleurs
  // attendaient le prochain rafraîchissement d'apparence. D'où une seconde de
  // palette d'origine à chaque connexion.
  void OnRenderLoginUI() override;

 private:
  // Applique ce qui peut l'être, au plus `budget` acteurs. Rend le nombre traité.
  int ApplyPending(int budget);

  // Impose la palette du SPRITE aux corps que les palettes de vêtement du client
  // abîment — et à eux seuls. Voir le .cc : on ne dévie du natif que si la
  // fusion récupère une part mesurable des pixels, et jamais pour un joueur qui
  // a choisi ses propres couleurs.
  int AutoRepair(int budget);

  // Pose NOS couleurs depuis le cache local dès que l'acteur existe, sans
  // attendre que le serveur nous les renvoie. Sans elle, la connexion affiche
  // une autre apparence pendant une bonne seconde. Idempotente.
  void RestoreLocalFromCache();
};

#endif  // BOURGEON_FEATURES_FX_STYLE_SYNC_H_
