#pragma once

#include <cstddef>
#include <cstdint>  // uint32_t (BodySpriteKey)

// ── Briques de chemin d'un sprite de PERSONNAGE ──────────────────────────────
//
// Trois jetons reviennent dans tous les gabarits du client — le dossier de
// SEXE, le dossier de RACE, et le prédicat qui choisit le second. Ils sont ici
// parce que le pantin (ui/doll.cc) et la vignette de tête (ui/head_icon.cc)
// composent chacun leurs chemins et se retrouvaient à en garder deux copies.
//
// 🔴 La RACE n'est pas une décoration : un Doram (Summoner) range TOUT sous un
// autre dossier racine. Un résolveur qui écrit `인간족` en dur ne rend pas un
// sprite approximatif — il ne rend RIEN, et le corps manquant emporte le pantin
// entier. C'est exactement le défaut qu'avait le Summoner.
namespace ro {

// Dossier de sexe, en CP949 : 남 (homme) / 여 (femme). `sex` : 0 = femme.
const char* SexFolder(int sex);

// La classe est-elle de la race DORAM (Summoner) ?
//
// Réponse du client — `Job_NeedsLuaItemPosOffset` (0x00d9cf80) — et pas d'une
// liste recopiée : c'est ce même prédicat qui gouverne, chez lui, le dossier de
// race, la table de coiffures et la variante de couvre-chef.
bool IsDoramJob(int job);

// Dossier de race, en CP949 : 도람족 (Doram) ou 인간족 (humain). Jamais nul.
const char* RaceFolder(int job);

// Nom de ressource du CORPS (ex. `DRAGON_KNIGHT`, `기사`), tel que le client le
// rend — c'est-à-dire en MAJUSCULES pour les classes nommées en ASCII, alors que
// les fichiers du GRF sont en minuscules. Le client ne recolle les deux qu'au
// VFS, où `Grf_NormalizePath` applique `_strlwr` ; toute comparaison faite en
// amont doit donc ignorer la casse ASCII (et ELLE SEULE : abaisser un octet
// CP949 abîmerait le coréen).
//
// 🔴 `body` est la CLASSE qui nomme le sprite, pas un « style » :
// `Job_ResolveBodyClass(job, body, 1)` ne lit `job` que pour reconnaître un bébé
// ou une monture, et c'est `body` qu'il remappe. Le laisser à 0 demande la
// classe 0 — tout le monde en Novice.
//
// `out_job` (facultatif) rend la classe EFFECTIVE (cls + 3950), celle dont la
// tête et les coiffes doivent suivre la race.
bool BodyResName(int job, int body, char* out, size_t out_size,
                 int* out_job = nullptr);

// Chemin VFS du corps, SANS extension : `data\sprite\<race>\몸통\<sexe>\<nom>_<sexe>`.
//
// 🔴 `data\sprite\`, PAS `data\` : le gabarit du client est relatif à la racine
// des SPRITES, et les DEUX préfixes que les couches natives ajoutent l'un après
// l'autre sont à poser soi-même dès qu'on les court-circuite.
bool BodySpritePath(int job, int body, int sex, char* out, size_t out_size,
                    int* out_job = nullptr);

// Chemin VFS complet du `.pal` de CORPS pour cette couleur de vêtement, ex.
// `data\palette\몸\body_56.pal`.
//
// 🔴 Le dossier dépend de la RACE : un Doram a le sien, et les deux indexations
// n'ont rien de commun (mesuré : 46 % des pixels d'un corps Doram tombent sur
// des index que la palette humaine laisse noirs). Ne jamais recomposer ce chemin
// à la main — passer par ici.
void BodyPalettePath(int color, int job, char* out, size_t out_size);

// Même chose, mais la RACE est lue dans le chemin du sprite au lieu d'être
// déduite d'une classe.
//
// 🔴 C'est ce qui permet de servir un AUTRE joueur : on n'a pas sa classe, mais
// le client a déjà résolu son sprite, et le dossier de race y figure. Rend false
// si le chemin ne suit pas le gabarit `data\sprite\<race>\...`.
bool BodyPalettePathForSprite(const char* spr_base, int color, char* out,
                              size_t out_size);

// Palette de CHEVEUX, chemin RELATIF à `palette\` (`머리\head_3.pal`, ou
// `<race>\머리\head_3.pal` pour un Doram) — la forme sous laquelle l'acteur
// porte le sien. Race lue dans le chemin du sprite de TÊTE.
bool HairPaletteRelForSprite(const char* head_spr, int color, char* out,
                             size_t out_size);

// L'identité d'un CORPS, réduite à 32 bits : FNV-1a sur le chemin de son `.spr`.
//
// ── À quoi ça sert ───────────────────────────────────────────────────────────
// Une recette de couleurs ne désigne ses pièces que par des index de palette, et
// un index ne veut rien dire hors du sprite où il a été mesuré. Un joueur a donc
// une recette PAR CORPS — à pied, sur sa monture, en costume — et il faut
// pouvoir dire de laquelle on parle : sur le réseau, dans une variable de
// personnage, dans un fichier de cache. Le chemin lui-même ne convient pas
// (jusqu'à 80 octets, et du CP949 au milieu) ; ce condensé, si.
//
// 🔴 Tous les clients DOIVENT en trouver le même pour le même corps : c'est ce
// qui leur fait choisir la même variante pour le joueur qu'ils regardent. Ils
// lisent tous le chemin au même endroit — l'emplacement 0 du composite de
// l'acteur — donc la seule chose à garantir ici est la stabilité de la fonction.
// D'où la normalisation : séparateurs unifiés, extension `.spr` ignorée, et
// repli en minuscules des SEULS 'A'-'Z'.
//
// ⚠ Le repli ASCII n'est pas un détail : un `tolower()` de la CRT sur un octet
// CP949 dépend de la locale, et abîmerait les dossiers coréens du chemin — deux
// clients de locales différentes ne calculeraient plus la même clé.
//
// Rend 0 sur un chemin vide, ce qui vaut « corps inconnu » et ne désigne jamais
// une variante.
uint32_t BodySpriteKey(const char* spr_path);

}  // namespace ro
