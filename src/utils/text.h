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

#include <string>

namespace text {

// Minuscules ASCII SEULEMENT ('A'..'Z'). Les octets ≥ 0x80 passent intacts.
std::string ToLowerAscii(std::string s);

// `needle` apparaît-il dans `haystack`, la casse ASCII ignorée ?
// Une aiguille vide répond OUI (aucun filtre = tout passe) ; une botte de foin
// vide répond NON. C'est le contrat des deux champs de recherche qui s'en
// servent — les réglages du jeu et ceux des raccourcis.
bool ContainsNoCase(const char* haystack, const char* needle);

// Un chiffre BASE 62 ('0'-'9', 'a'-'z', 'A'-'Z' = 0..61), ou -1. C'est l'encodage
// que le client emploie dans ses balises de lien d'objet du chat. Deux fichiers
// le décodaient, l'un en `char` et l'autre en `unsigned char` — même table.
int Base62Digit(char c);

}  // namespace text
