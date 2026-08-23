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
// CZ_BOURGEON_STYLE (0x0F26), client -> serveur, 56 octets fixes :
//     [opcode:2][longueur:2]        = 4
//     [version:1]                   = kWireVersion
//     [drapeaux:1]                  bit 0 = effacer CE corps, bit 1 = effacer TOUT
//     [corps:uint32 LE]             `ro::BodySpriteKey` du `.spr` de corps
//     [palette:int16 LE]            palette de corps officielle 1..553, -1 = celle du serveur
//     [cheveux:int16 LE]            palette de cheveux 1..251, -1 = celle du personnage
//     [coiffure:int16 LE]           coupe 1..80, -1 = celle du personnage
//     [réglages: 8 × 5]             = 40
//
// ZC_BOURGEON_STYLES (0x0F27), serveur -> client, longueur variable :
//     [opcode:2][longueur:2][nombre:2]              = 6
//     puis `nombre` fois, 56 octets chacun :
//     [gid:4][version:1][drapeaux:1][corps:uint32][palette:int16][cheveux:int16]
//     [coiffure:int16][réglages: 8 × 5]
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
// ── 🔴🔴 UNE RECETTE PAR CORPS (v7, 2026-08-15) ─────────────────────────────
//
// Une recette ne désigne ses pièces que par des INDEX de palette, et les index
// n'ont aucune signification commune d'un corps à l'autre (mesuré : un Dragon
// Knight utilise 7-73, un Creator 112-223). Jusqu'à la v6, la même recette était
// donc ré-appliquée par index sur le corps du moment, quel qu'il fût : monter
// sur une monture rendait au joueur des couleurs plausibles mais AUTRES que les
// siennes. Le compromis a tenu tant qu'on croyait la monture un cas rare ; elle
// ne l'est pas, et les costumes de corps comme les changements de classe posent
// exactement le même problème.
//
// La v7 range donc une recette PAR CORPS, chacune désignée par
// `ro::BodySpriteKey` — le condensé du chemin de son `.spr`.
//
// 🔴 Le serveur ne calcule JAMAIS cette clé et n'a pas à savoir ce qu'elle
// désigne. Il ne connaît pas le sprite d'un joueur : cette résolution est faite
// par le client, avec une demi-douzaine de cas particuliers dont une copie
// serveur divergerait au premier costume. Il range N recettes sous leurs clés et
// les rediffuse TOUTES ; c'est le client destinataire — le seul à voir le corps
// réellement monté — qui choisit laquelle appliquer.
//
// ⚠ Un lot ZC REDÉFINIT INTÉGRALEMENT les variantes des GID qu'il mentionne. Le
// serveur envoie donc toujours l'ensemble complet d'un joueur, y compris quand
// une seule vient de changer. C'est ce qui rend la règle du client trivialement
// juste : « ce que je reçois est tout ce qu'il a ».
//
// ── Le REPLI, et pourquoi il existe ─────────────────────────────────────────
// Un corps sans variante propre reprend celle marquée `kFlagDefault`. Sans ce
// repli, enfourcher une monture jamais personnalisée ferait perdre au joueur
// TOUTES ses couleurs d'un coup, ce qui serait une régression franche sur la v6.
// Avec, il garde un rendu proche, et n'a qu'à valider une fois monté pour donner
// à ce corps-là ses propres réglages.

namespace fx {
namespace style_sync {

// Tailles de trame — partagées avec le serveur, qui doit les recopier.
constexpr int kRampBytes    = 5;
constexpr int kAdjustBytes  = ro::kMaxRamps * kRampBytes;                // 40
constexpr int kCzBytes      = 4 + 1 + 1 + 4 + 2 + 2 + 2 + kAdjustBytes;  // 56
constexpr int kZcHeadBytes  = 4 + 2;                                     // 6
constexpr int kZcEntryBytes = 4 + 1 + 1 + 4 + 2 + 2 + 2 + kAdjustBytes;  // 56

// Nombre de corps qu'un personnage peut habiller séparément.
//
// 🔴 Ce plafond dimensionne le STOCKAGE SERVEUR : une variable de personnage par
// variante, chacune tenant très largement sous les 254 caractères d'une
// `char_reg_str`. Le monter demande d'ajouter des variables côté serveur, pas
// seulement de changer ce nombre.
//
// Quatre couvre ce qu'un joueur porte réellement : son corps, sa monture, un
// costume, et un de rab. Au-delà, la variante la plus anciennement validée cède
// sa place — le serveur range un numéro de séquence pour savoir laquelle.
constexpr int kMaxVariants = 4;

// 🔴 À incrémenter dès que la disposition OU le sens des rampes change — et
// `ro::kMaxRamps` en fait partie, puisqu'il dimensionne le bloc de réglages.
//
// ── Ce que cet octet sert VRAIMENT ───────────────────────────────────────────
// Pas à migrer : une seule version est acceptée, l'autre est jetée, et le joueur
// retrouve son apparence native. Il sert à survivre au fait que le client et le
// serveur ne sont JAMAIS déployés à la même seconde. Pendant un patch, des
// clients d'hier et d'aujourd'hui se croisent sur la même carte ; sans cet
// octet, celui qui reçoit une recette d'une autre époque la lit quand même et
// peint les mauvaises pièces du costume, avec l'aplomb d'un format valide. Un
// joueur en couleurs natives se remarque à peine ; un joueur aux bottes couleur
// de cape se signale comme un bug.
//
// ⚠ C'est aussi pourquoi les recettes ne se « rattrapent » pas : rien ne
// distingue, dans le bloc de réglages, une recette d'avant d'une recette
// d'après. Seul cet octet le dit.
//
// v6 (2026-08-12) : classement des rampes pondéré par la saturation. Pas un
// octet ne bouge dans la trame — c'est le SENS des rangs qui change, une recette
// ne désignant ses pièces que par un rang. Les cinq versions précédentes
// (2026-08-11/12) ajoutaient des champs et se migraient ; celle-ci se jette,
// comme la v2 qui avait déjà déplacé les frontières de rampes.
//
// v7 (2026-08-15) : une recette PAR CORPS. La trame gagne quatre octets de clé,
// et un joueur peut désormais avoir plusieurs entrées dans un même lot. Les
// recettes v6 sont jetées comme les précédentes : elles ne portent aucune clé,
// donc rien ne dirait à quel corps les rattacher — et les rattacher au corps
// courant serait un pari, puisque le joueur peut être monté au moment où on lit.
//
// 🔴 Elle est la seule entrée que le serveur INTERPRÈTE. Tout le reste, il le
// range sans le comprendre ; celle-ci, il l'applique par `pc_changelook`, ce qui
// l'écrit dans `sd->status.hair`, la sauvegarde avec le personnage et l'annonce
// à la zone par le ZC_SPRITE_CHANGE natif — clients vanilla compris. Elle est
// ici parce que POUR LE JOUEUR la coiffure fait partie du style, et que c'est
// son point de vue qui commande le format.
constexpr uint8_t kWireVersion = 7;

// CZ : efface la variante de CE corps. ZC : ce joueur n'a plus aucune recette.
constexpr uint8_t kFlagClear = 0x01;
// CZ seulement : efface TOUTES les variantes du personnage. Le bouton « Supprimer
// mon style » enlève tout — un joueur qui veut redevenir lui-même ne s'attend
// pas à devoir le demander corps par corps.
constexpr uint8_t kFlagClearAll = 0x02;
// ZC seulement : cette variante est celle du REPLI, appliquée aux corps qui n'en
// ont pas. Le serveur la pose sur la plus ancienne qu'il détient — le client ne
// la devine pas, sans quoi deux clients pourraient choisir différemment.
constexpr uint8_t kFlagDefault = 0x04;

// 🔴 Le garde-fou du protocole. Ces tailles sont recopiées à la main côté
// serveur (moonlight : packets_struct.hpp, PACKET_CZ_BOURGEON_STYLE et
// PACKET_BOURGEON_STYLE_ENTRY). Faire varier `ro::kMaxRamps` les changerait
// SANS ERREUR — le client enverrait 50 octets là où le serveur en attend 46, et
// rAthena rejetterait la trame en silence. Ces assertions transforment cet
// accident muet en échec de compilation, à charge de bumper `kWireVersion` et de
// mettre le serveur à jour.
static_assert(kCzBytes == 56, "trame CZ modifiée : bumper kWireVersion et mettre à jour moonlight");
static_assert(kZcEntryBytes == 56, "entrée ZC modifiée : bumper kWireVersion et mettre à jour moonlight");

// Envoie la recette du joueur POUR UN CORPS DONNÉ. À appeler quand il VALIDE,
// pas à chaque mouvement de curseur : l'aperçu est déjà local et immédiat, et le
// serveur n'a que faire des soixante états intermédiaires d'un glissement.
//
// `body_key` = `ro::BodySpriteKey` du corps qu'il porte à cet instant. Une clé
// nulle est refusée : ranger une recette sous « corps inconnu » la rendrait
// inapplicable, et elle prendrait la place d'une vraie.
void SendRecipe(const ro::PaletteRecipe& recipe, uint32_t body_key);

// Dit au serveur d'oublier la variante de CE corps. Les autres survivent, et le
// corps concerné retombe sur la variante de repli — ou sur son apparence native
// si c'était la dernière.
void SendClear(uint32_t body_key);

// Dit au serveur d'oublier TOUT : le joueur revient à son apparence native, pour
// lui comme pour les autres, sur tous ses corps.
void SendClearAll();

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

// La recette d'un joueur EN VUE, telle que `ZC_BOURGEON_STYLES` (0x0F27) nous
// l'a livrée, pour le corps `body_key` — ou sa variante de repli si ce corps-là
// n'a pas la sienne (même règle que ce qu'on pose sur son acteur, donc le même
// résultat à l'écran). Rend false si ce joueur n'a rien partagé.
//
// 🔴 Elle existe pour les vues qui dessinent un AUTRE joueur ailleurs qu'à sa
// place dans le monde — la fiche « Voir l'équipement », d'abord. Sans elle, ces
// vues montreraient l'apparence NATIVE d'un joueur qui s'est recoloré : le
// pantin et le personnage à l'écran ne seraient pas la même personne.
//
// ⚠ Le registre n'existe QUE pour les joueurs dont le serveur nous a envoyé le
// style — donc ceux qui sont (ou ont été) en vue pendant cette session.
// `body_key` à 0 = « je ne sais pas quel corps » : la variante de repli est
// rendue.
bool RemoteRecipe(uint32_t gid, uint32_t body_key, ro::PaletteRecipe* out);

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

// La recette que NOTRE acteur porte sur le corps `body_key`, s'il y en a une.
// L'éditeur s'en sert pour démarrer sur les couleurs que le joueur a déjà
// partagées, au lieu d'une recette vide qui les effacerait à la première
// application.
//
// `out_exact` (facultatif) dit si elle a été faite POUR ce corps ou si c'est le
// repli. La fenêtre le montre au joueur : sans ça, il croirait modifier le style
// de sa monture alors qu'il s'apprête à réécrire celui de son corps à pied.
//
// `body_key` nul = « je ne sais pas quel corps » : rend alors la variante de
// repli, qui est le meilleur résumé de l'allure du personnage.
bool LocalRecipe(uint32_t body_key, ro::PaletteRecipe* out,
                 bool* out_exact = nullptr);

// Le joueur a-t-il une variante FAITE POUR ce corps ? (Le repli ne compte pas.)
bool LocalHasVariant(uint32_t body_key);

// Combien de corps le joueur a-t-il habillés ? Pour la fenêtre, qui prévient
// quand le plafond est atteint et qu'une validation de plus en évincera une.
int LocalVariantCount();

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

  // Remet en file d'application les acteurs qui ont CHANGÉ DE CORPS — monture
  // enfourchée ou quittée, costume de corps, changement de classe. Rend le
  // nombre d'acteurs concernés.
  //
  // 🔴 Sans elle, la palette posée reste celle de l'ancien corps : ses index ne
  // désignent plus les mêmes pièces, et le personnage part en couleurs délavées
  // ou fausses jusqu'à ce que son propriétaire revalide son style à la main.
  // C'est exactement la règle annoncée en tête de ce fichier — « on applique par
  // index de rampe, sans condition » — qui n'était appliquée que par l'éditeur
  // du joueur, pas par la boucle qui sert tout le monde.
  int RefreshChangedBodies();

  // Impose la palette du SPRITE aux corps que les palettes de vêtement du client
  // abîment — et à eux seuls. Voir le .cc : on ne dévie du natif que si la
  // fusion récupère une part mesurable des pixels, et jamais pour un joueur qui
  // a choisi ses propres couleurs.
  int AutoRepair(int budget);

  // Notre acteur vient de cesser d'exister (retour au char-select) : on oublie
  // tout ce qui le concernait.
  //
  // 🔴 Le bon moment, et le seul, pour purger l'injection : l'acteur n'existe
  // pas, donc il n'y a rien à ménager. En jeu, `ClearRecipe` lui rendrait le
  // chemin de palette mémorisé — celui du personnage précédent.
  void ForgetLocalActor();

  // Oublie le style du personnage PRÉCÉDENT quand on en change sans quitter le
  // client.
  //
  // 🔴 Notre registre de recettes est indexé par GID, c'est-à-dire par l'AID —
  // qui ne change PAS d'un personnage à l'autre du même compte. Sans cette
  // purge, le suivant hérite de la palette du précédent, et le serveur ne peut
  // pas nous rattraper : à un personnage sans style, il n'envoie rien du tout.
  void ForgetPreviousCharacter();

  // Pose NOS couleurs depuis le cache local dès que l'acteur existe, sans
  // attendre que le serveur nous les renvoie. Sans elle, la connexion affiche
  // une autre apparence pendant une bonne seconde. Idempotente.
  void RestoreLocalFromCache();
};

#endif  // BOURGEON_FEATURES_FX_STYLE_SYNC_H_
