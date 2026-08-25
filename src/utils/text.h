#pragma once

// ── Comparaisons de texte INSENSIBLES À LA CASSE ─────────────────────────────
//
// Quatre fichiers portaient leur propre `ToLower` et deux leur propre
// `Contains`. Ce n'était pas la même fonction quatre fois : TROIS passaient par
// `std::tolower`, dont le résultat DÉPEND DE LA LOCALE du processus, et une
// seule se limitait à l'ASCII.
//
// 🔴 La différence n'est pas théorique ici. Ces fonctions servent à comparer des
// noms de fichiers, des URL et des noms de processus — des chaînes qui arrivent
// en CP949 ou en UTF-8, jamais en Latin-1. Sous une locale non-C, `std::tolower`
// touche des octets ≥ 0x80 qui sont la MOITIÉ d'un caractère multi-octet : la
// comparaison devient dépendante de la machine, et l'écart ne se voit que chez
// le joueur dont le système n'est pas configuré comme le nôtre.
//
// Le nom le dit donc : ces fonctions sont ASCII, et c'est un choix.

#include <cstddef>  // size_t
#include <cstdint>  // uint32_t
#include <string>

namespace text {

// Minuscules ASCII SEULEMENT ('A'..'Z'). Les octets ≥ 0x80 passent intacts.
std::string ToLowerAscii(std::string s);

// `needle` apparaît-il dans `haystack`, la casse ASCII ignorée ?
// Une aiguille vide répond OUI (aucun filtre = tout passe) ; une botte de foin
// vide répond NON. C'est le contrat des champs de recherche qui s'en servent —
// les réglages du jeu, ceux des raccourcis, et le filtre du courrier.
//
// ⚠ Sur un sujet accentué, les octets non-ASCII sont comparés tels quels : « é »
// trouve « é », mais « E » ne trouve pas « É ». Les noms de personnage n'en
// sortent pas, et c'est le seul repli honnête sans table Unicode.
//
// (ImGuiTextFilter aurait été plus court, mais il est SENSIBLE à la casse :
// taper « gettar » n'y trouverait pas « Gettar ». Constat de rodex_window, qui
// portait la seconde écriture de cette fonction.)
bool ContainsNoCase(const char* haystack, const char* needle);

// Un chiffre BASE 62 ('0'-'9', 'a'-'z', 'A'-'Z' = 0..61), ou -1. C'est l'encodage
// que le client emploie dans ses balises de lien d'objet du chat. Deux fichiers
// le décodaient, l'un en `char` et l'autre en `unsigned char` — même table.
int Base62Digit(char c);

// La VALEUR d'une suite de chiffres base 62, lue de gauche à droite.
//
// 🔴 S'ARRÊTE au premier caractère qui n'en est pas un, au lieu de propager le
// -1 de `Base62Digit`. Les deux copies de ce décodeur ne se protégeaient pas
// pareil, et chacune avait raison chez elle : celle du chat borne son entrée
// AVANT d'appeler (elle compte les chiffres jusqu'au premier caractère
// étranger), celle du lien d'objet appelle avec une longueur FIXE de 5 sur un
// champ qui peut être plus court. Sans le garde, un -1 converti en `uint32_t`
// fait exploser le résultat en silence. La version gardée est donc la seule des
// deux qui convienne aux deux appels.
uint32_t Base62Decode(const char* s, size_t len);

// Les six premiers caractères sont-ils des chiffres HEXADÉCIMAUX ? C'est le test
// d'une couleur `^RRGGBB` du client.
//
// 🔴 Des deux copies, l'une passait par `std::isxdigit` — donc par la LOCALE,
// que l'en-tête de ce fichier proscrit pour cette raison même. Celle-ci est
// ASCII, comme tout ce qui vit ici.
//
// ⚠ Lit SIX octets : c'est à l'appelant de garantir qu'ils existent.
bool IsHex6(const char* s);

// Cherche `needle` dans [begin, end), un intervalle qui n'est PAS terminé par
// un zéro — le corps d'un paquet, une ligne de chat parcourue par pointeurs.
// `strstr` lirait au-delà de `end`.
//
// 🔴 Rend `nullptr` sur une aiguille VIDE. Des deux copies, une seule testait
// `n == 0` : l'autre rendait `begin`, c'est-à-dire « trouvé tout de suite ».
// Aucun appelant actuel ne passe une aiguille vide (ce sont des balises
// littérales), mais deux réponses opposées au même appel ne pouvaient pas
// rester.
const char* SearchSub(const char* begin, const char* end, const char* needle);

// Un nombre avec des VIRGULES tous les trois chiffres — « 1,234,567 ».
//
// Écrit deux fois : `FormatZeny` (échoppe joueur) et `Grouped` (fiche de
// monstre), à la ligne près, jusqu'au calcul du premier groupe. Le séparateur
// est la virgule parce que c'est celui du client, pas celui de la locale : ces
// chaînes voisinent des libellés que le jeu produit lui-même.
//
// ⚠ Tronque proprement si `cap` est trop petit, et termine toujours par un NUL.
void GroupThousands(long long value, char* out, size_t cap);

}  // namespace text
