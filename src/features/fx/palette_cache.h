#ifndef BOURGEON_FEATURES_FX_PALETTE_CACHE_H_
#define BOURGEON_FEATURES_FX_PALETTE_CACHE_H_

#include <cstdint>
#include <string>

#include "ui/palette_ramps.h"

// Cache local des couleurs composées par le joueur, une entrée par PERSONNAGE.
//
// ── Pourquoi un cache alors que le serveur stocke déjà ───────────────────────
// 🔴 Le serveur reste la SOURCE DE VÉRITÉ : il range la recette dans une
// variable de personnage et la renvoie à chaque connexion. Ce cache ne le
// double pas — il sert le seul écran que le map-server n'atteint pas : le
// CHAR-SELECT. Là, le client parle au char-server et ne sait donc rien des
// couleurs ; sans cache, un personnage s'y afficherait dans son apparence
// native puis changerait en entrant en jeu.
//
// Conséquence assumée : sur une machine neuve, le char-select montre l'apparence
// native jusqu'à la première connexion de ce personnage. C'est un cache, pas une
// sauvegarde — le perdre ne perd rien.

namespace fx {
namespace palette_cache {

// Range la recette de ce personnage. `recipe` nul = efface l'entrée.
// Écrit le fichier immédiatement : ces appels sont rares (un partage, un login),
// et différer l'écriture perdrait la recette à la fermeture du client.
void Save(uint32_t char_id, const ro::PaletteRecipe* recipe);

// La recette en cache pour ce personnage. false si aucune, ou si l'entrée a été
// écrite par une version antérieure du format de recette — auquel cas elle est
// ININTERPRÉTABLE et doit être ignorée, pas relue de travers.
bool Load(uint32_t char_id, ro::PaletteRecipe* out);

// Compteur incrémenté à CHAQUE modification du cache.
//
// 🔴 Sert aux appelants qui mémoisent un résultat dérivé — le char-select
// recompose une palette entière par personnage, ce qui coûte l'analyse d'un
// `.spr`, et ne peut pas le refaire à chaque frame. Sans ce compteur dans leur
// signature de cache, un « ce personnage n'a pas de couleurs » calculé au
// premier passage ne serait plus jamais revérifié : les couleurs n'apparaissaient
// qu'après avoir quitté et relancé le jeu.
uint32_t Generation();

// ── Préréglages nommés ──────────────────────────────────────────────────────
//
// Une recette mise de côté sous un nom, réutilisable sur n'importe quel
// personnage. Purement LOCAL : le serveur n'en sait rien et n'a pas à en savoir
// — un préréglage n'est pas une apparence, c'est un brouillon.
//
// ⚠ Une recette ne vaut vraiment que pour le corps sur lequel elle a été faite :
// appliquée ailleurs, elle se ré-applique PAR INDEX de rampe. Le résultat reste
// plausible (les rampes sont triées par surface) mais pas identique.
constexpr int kMaxPresets = 32;

// Les noms rangés, dans l'ordre du fichier. Rend le nombre écrit.
int PresetNames(std::string* out, int max);

// Range la recette sous ce nom, en remplaçant un homonyme. false si le nom est
// vide ou si le plafond est atteint.
bool PresetSave(const std::string& name, const ro::PaletteRecipe& recipe);

bool PresetLoad(const std::string& name, ro::PaletteRecipe* out);
void PresetDelete(const std::string& name);

// ── Brouillon : le dernier style COMPOSÉ mais jamais validé ─────────────────
//
// 🔴 Il ne répond qu'à un accident : le joueur cherche sa couleur, ne valide
// pas, et perd la session — plantage, coupure, déconnexion, une envie de partir
// se coucher. Rien d'autre ne garde cet état. Le serveur ne connaît que ce qui a
// été validé ; le cache ci-dessus ne fait que refléter le serveur ; un
// préréglage demande un geste explicite, précisément celui que n'a pas fait
// quelqu'un qui n'avait pas fini. Sans cette réserve, une demi-heure de réglages
// s'évapore sans que le joueur ait rien fait de mal.
//
// PAR PERSONNAGE, comme le cache : une recette ne vaut que pour le corps sur
// lequel elle a été composée, et rendre à un personnage le brouillon d'un autre
// lui poserait des couleurs qui ne veulent rien dire.
//
// ⚠ Un seul brouillon par personnage, écrasé à chaque fois. Ce n'est pas un
// historique : c'est un filet, et un filet à plusieurs mailles demanderait au
// joueur de choisir entre des états qu'il ne sait plus distinguer.
void DraftSave(uint32_t char_id, const ro::PaletteRecipe* recipe);
bool DraftLoad(uint32_t char_id, ro::PaletteRecipe* out);

// Y a-t-il quelque chose à récupérer ? Interrogé à chaque frame par la fenêtre,
// donc volontairement plus léger qu'un `DraftLoad` — pas de décodage.
bool HasDraft(uint32_t char_id);

// ── Code partageable ────────────────────────────────────────────────────────
//
// La MÊME chaîne que celle rangée en base et dans le cache : `<version>:<palette
// de corps>:<palette de cheveux>:<80 hexadécimaux>`. Un joueur peut la copier et
// la donner à un autre.
//
// 🔴 Elle porte une VERSION, et `DecodeShare` refuse celles qu'il ne sait pas
// relire. C'est ce qui évite qu'un vieux code, échangé sur un forum longtemps
// après un changement de format, pose des couleurs sur les mauvaises pièces.
//
// ⚠ Une recette ne vaut vraiment que pour le corps sur lequel elle a été faite :
// collée sur une autre classe, elle se ré-applique par INDEX de rampe. Le
// résultat reste plausible — les pièces sont triées par surface — sans être
// identique.
std::string EncodeShare(const ro::PaletteRecipe& recipe);
bool DecodeShare(const std::string& code, ro::PaletteRecipe* out);

// Clé pour `DollLook::body_palette_key`.
//
// 🔴 Elle identifie le CONTENU de la palette, pas seulement son propriétaire :
// le cache de teintes du composeur partage ses textures entre deux appels de
// même clé, donc une clé qui ne bougerait pas avec les octets ferait ressortir
// les textures d'une palette précédente. Le pointeur rendu vise un tampon
// statique, valable jusqu'au prochain appel.
const char* DollKey(uint32_t char_id, const uint8_t* rgba);

}  // namespace palette_cache
}  // namespace fx

#endif  // BOURGEON_FEATURES_FX_PALETTE_CACHE_H_
