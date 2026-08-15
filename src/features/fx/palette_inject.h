#ifndef BOURGEON_FEATURES_FX_PALETTE_INJECT_H_
#define BOURGEON_FEATURES_FX_PALETTE_INJECT_H_

#include <cstdint>

#include "ui/palette_ramps.h"

// Injection d'une palette de corps qui vit EN MÉMOIRE, sans aucun fichier.
//
// ── Le mécanisme, en trois phrases ───────────────────────────────────────────
// Le rendu ne connaît d'une palette qu'un POINTEUR vers une table ARGB1555, et
// ce pointeur est aussi la moitié de la clé du cache d'atlas. Il suffit donc de
// fabriquer un bloc à la disposition d'un `CPaletteRes` et de le faire rendre à
// la place du fichier. Deux accroches y suffisent : l'une donne à l'acteur un
// chemin de palette qui n'appartient qu'à lui, l'autre rend NOTRE bloc quand ce
// chemin est demandé.
//
// ── 🔴 Pourquoi un chemin unique par acteur est OBLIGATOIRE ──────────────────
// Le cache de ressources du client est indexé par CHEMIN, et le chemin natif ne
// dépend que de (job, sexe, couleur de vêtement, bodyStyle) : deux joueurs de
// même classe et même couleur partagent PHYSIQUEMENT la même `CPaletteRes`.
// Écrire dedans les repeindrait tous — ce que la contrainte du projet interdit.
//
// ── 🔴 Pourquoi on ne mute JAMAIS une palette déjà affichée ──────────────────
// La texture d'atlas est cuite UNE fois pour la clé {frame, palette} et n'est
// refaite qu'à l'éviction, purement temporelle. Changer les octets sans changer
// le pointeur ne se verrait pas — puis la couleur basculerait toute seule
// quelques secondes plus tard. Chaque version d'une recette reçoit donc son
// PROPRE bloc, à une adresse neuve.
//
// ── La contrainte qui prime sur tout ─────────────────────────────────────────
// « Le système de palettes natif doit fonctionner quoi qu'il arrive, sans
// modifier les palettes existantes. » Nos détours sortent donc vers le
// trampoline à la première ligne dès que l'acteur n'a pas de recette, aucun
// `.pal` n'est touché, et rien n'est inscrit dans le cache du client.

namespace fx {
namespace palette_inject {

// Pose les détours, une seule fois. 🔴 À appeler DEPUIS LE FIL DE RENDU : les
// fonctions visées tournent sur ce fil, donc elles ne peuvent pas être en cours
// d'exécution pendant qu'on réécrit leurs cinq premiers octets. Sans effet si
// les détours sont déjà posés ou si le binaire ne correspond pas.
void EnsureInstalled();

// Donne (ou remplace) la recette d'un acteur, désigné par son GID.
// `ramps` / `ramp_count` décrivent les rampes du sprite de CE corps, telles que
// `ro::DetectRamps` les a trouvées ; `base` est la palette RGBA d'origine du
// `.spr` (1024 octets), sur laquelle la recette s'applique.
// Rend false si les arguments ne tiennent pas debout — et ne touche alors à
// rien, l'acteur gardant son apparence native.
bool SetRecipe(uint32_t gid, const uint8_t* base, const ro::PaletteRamp* ramps,
               int ramp_count, const ro::PaletteRecipe& recipe);

// Retire la recette d'un acteur : il retrouve exactement son rendu natif.
//
// ⚠ Suppose que l'acteur EXISTE : c'est en le remettant sur son chemin d'origine
// que la fonction fait son travail. Quand il n'existe plus — retour au
// char-select, changement de personnage — c'est `ForgetActor` qu'il faut.
void ClearRecipe(uint32_t gid);

// Oublie TOUT ce qu'on sait de cet acteur, sans toucher à aucun acteur.
//
// 🔴 À appeler quand l'acteur a cessé d'exister, typiquement au retour vers le
// char-select. `ClearRecipe` n'y convient pas et serait même NUISIBLE : il rend
// à l'acteur le chemin de palette MÉMORISÉ, qui est alors celui du personnage
// précédent — on repeindrait le suivant avec la couleur de vêtement de son
// prédécesseur.
//
// 🔴 Le drapeau `color_forced` est la vraie raison d'être de cette fonction. Il
// interdit de recapturer le chemin natif tant qu'il est levé — protection juste
// pour un acteur donné, poison d'un personnage à l'autre : le nouveau venu se
// voyait refuser la capture de SON chemin, et gardait donc la base de l'ancien.
// Comme le GID vaut l'AID, qui ne change pas entre deux personnages du même
// compte, rien ne distinguait les deux sans cet appel.
void ForgetActor(uint32_t gid);

// ── Couleur de CHEVEUX ──────────────────────────────────────────────────────
//
// 🔴 Bien plus simple que le corps, et pour une raison de fond : une palette de
// cheveux officielle est un VRAI FICHIER. Il n'y a donc rien à fabriquer en
// mémoire — on écrit le chemin dans l'acteur et le client charge le fichier
// lui-même. Toute la machinerie de blocs ARGB1555 ne sert qu'aux couleurs
// COMPOSÉES, qui n'existent dans aucun fichier.
//
// `palette_id` valide = 1..251 ; négatif ou nul = retour à la couleur native.
void SetHairPalette(uint32_t gid, int palette_id);
void ClearHairPalette(uint32_t gid);

// Numéro le plus élevé d'une palette de cheveux officielle. Les fichiers vont de
// `head_0.pal` à `head_251.pal`, mais 0 signifie « aucune palette externe » —
// même convention que pour le corps.
constexpr int kHairPaletteMax = 251;

// ⛔ La COIFFURE ne passe PAS par ici, et n'y reviendra pas.
//
// Une machinerie d'injection a existé (appel de `CActorSprite_BuildHead_Slot1`
// depuis `OnTick`, coupe d'origine mémorisée, réparation à chaque battement) ;
// elle a été RETIRÉE le 2026-08-12, pour deux raisons qui se cumulent :
//   * c'est le SERVEUR qui l'applique (`pc_changelook` sur CZ_BOURGEON_STYLE),
//     et son ZC_SPRITE_CHANGE natif l'annonce à tout le monde — nous n'avons
//     donc rien à poser, ni sur soi ni sur les autres ;
//   * l'aperçu vit dans le pantin de l'éditeur, qui compose ses propres sprites
//     de tête et n'a rien à demander à l'acteur.
// Il ne restait qu'un détour à entretenir pour un besoin qui n'existait plus.

// Le chemin de palette que le NATIF avait choisi pour cet acteur, relatif à
// `palette\` (ex. `몸\body_56.pal`). Rend false si on ne l'a pas encore vu.
//
// 🔴 Il est capturé dans le détour, AVANT qu'on n'écrive le nôtre : une fois la
// recette posée, l'acteur ne porte plus que notre chemin synthétique, et la
// couleur choisie par le joueur au styliste serait définitivement perdue.
bool NativePalettePath(uint32_t gid, char* out, size_t out_size);

// Le chemin de base du `.spr` de CORPS que le client a résolu pour cet acteur
// (sans extension, préfixé `data\`). Rend false tant que le détour n'a pas vu
// l'acteur — au spawn, en pratique.
//
// 🔴 On le LIT sur l'acteur au lieu de le recalculer depuis (classe, sexe,
// monture) : la résolution native enchaîne assez de cas particuliers pour qu'une
// copie diverge au premier costume ajouté — et d'un AUTRE joueur nous n'avons de
// toute façon pas les variables d'entrée, seulement le composite déjà monté.
bool ActorBodySpritePath(uint32_t gid, char* out, size_t out_size);

// Repose notre chemin sur tout acteur qui porte une recette mais qui a repris
// son chemin de palette NATIF. Rend le nombre d'acteurs réparés.
//
// 🔴 Pourquoi ce chien de garde existe : le détour de reconstruction est notre
// seule occasion d'écrire, or `acteur+0x1C` peut être réécrit ailleurs. Symptôme
// rapporté le 2026-08-13 — après une longue mise en veille de la fenêtre, le
// CORPS redevenait noir (la palette native d'une 4e classe laisse des index
// noirs, cf. la réparation automatique) pendant que la TÊTE restait juste, et
// seul un changement de carte le corrigeait. Un changement de carte, c'est
// exactement une reconstruction : la recette était donc intacte, seul le chemin
// avait été perdu.
//
// ⚠ À appeler depuis le FIL DE RENDU : on écrit dans un acteur.
int ReassertPaths();

// Les acteurs dont le CORPS a changé depuis le calcul de leur palette : monture
// enfourchée ou quittée, costume de corps équipé, changement de classe. Rend le
// nombre de GID écrits dans `out` (au plus `max`).
//
// 🔴 Pourquoi cette fonction existe. Une recette ne désigne ses couleurs que par
// des INDEX de palette, et les index n'ont aucune signification commune d'un
// sprite à l'autre. Le bloc de 1024 octets est donc valable pour UN corps
// précis ; le jour où le client en monte un autre, il continue pourtant d'être
// rendu — le détour repose fidèlement notre chemin, et personne ne recalcule
// rien. Symptôme exact (2026-08-15, monture) : le personnage passe en couleurs
// délavées ou fausses, alors que le pantin de l'éditeur — qui, lui, suit le
// sprite — montre déjà la bonne interprétation. Il fallait revalider son style
// à la main pour que l'écran se remette d'accord avec lui-même.
//
// ⚠ Le chemin de référence est ADOPTÉ au passage : un GID n'est signalé qu'une
// fois par changement. C'est à l'appelant de réessayer si son recalcul échoue —
// signaler en boucle ferait fabriquer un bloc de palette par tick, et les blocs
// ne sont jamais libérés.
int PollStaleSprites(uint32_t* out, int max);

// L'acteur de ce GID existe-t-il encore ? Le pointeur mémorisé par le détour est
// revalidé (GID relu, sous SEH), donc un acteur détruit rend false.
//
// 🔴 C'est la question à poser avant toute purge : « le monde a-t-il disparu ? »
// ne se déduit PAS d'un compteur de temps. Voir `ForgetActor`.
bool ActorAlive(uint32_t gid);

// Cet acteur a-t-il une recette active ?
bool HasRecipe(uint32_t gid);

// La palette RGBA (1024 o) actuellement INJECTÉE pour cet acteur, recopiée dans
// `out`. false s'il n'en a pas.
//
// 🔴 C'est ce qu'il faut aux pantins de l'interface — feuille de perso, portrait,
// aperçu d'équipement, enregistreur GIF. Sans elle, ils recomposeraient à partir
// des fichiers et afficheraient donc les couleurs d'AVANT, pendant que le
// personnage en scène porte les nouvelles. Recopier plutôt que rendre un
// pointeur : l'appelant dessine sur plusieurs frames, et le bloc peut être
// remplacé entre-temps par un réglage de curseur.
bool InjectedPalette(uint32_t gid, uint8_t* out, size_t out_size);

// Nombre de recettes vivantes — pour le panneau de diagnostic.
int RecipeCount();

// Les GID de tous les acteurs que le détour a vus passer, recette ou non.
// Rend le nombre écrit dans `out` (au plus `max`).
//
// 🔴 C'est la SEULE énumération d'acteurs dont nous disposions, et elle est
// gratuite : le détour de reconstruction voit tout ce qui apparaît à l'écran.
// Remonter la liste d'acteurs du client demanderait de traverser ses conteneurs
// natifs à chaque passage, pour la même information.
//
// ⚠ Elle n'est pas purgée : un GID reste connu après la disparition de son
// acteur. Les appelants revalident via les fonctions qui prennent un GID —
// toutes rendent false sur un acteur mort.
int KnownGids(uint32_t* out, int max);

}  // namespace palette_inject
}  // namespace fx

#endif  // BOURGEON_FEATURES_FX_PALETTE_INJECT_H_
