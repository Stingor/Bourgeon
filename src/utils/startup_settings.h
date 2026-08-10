#pragma once

#include "yaml-cpp/yaml.h"

// ── Les réglages lus AVANT l'entrée en jeu ───────────────────────────────────
// Auto-login, char-select, login Moonlight. Ils vivaient dans
// `bourgeon_settings.yaml`, aux côtés des réglages de jeu ; ils sont maintenant
// dans `paths::StartupSettingsPath()`.
//
// 🔴 La frontière est TEMPORELLE, pas thématique : bourgeon_settings.yaml n'est
// relu qu'à la transition vers le mode jeu. Un réglage qui doit agir plus tôt n'y
// a rien à faire — la langue de l'interface y est restée trop longtemps, et
// l'écran de login sortait en français quel qu'ait été le choix du joueur.
//
// ⚠ Ces trois sections ne sont écrites par PERSONNE : ce sont des surcharges de
// configuration, posées à la main. C'est aussi pourquoi la reprise ci-dessous ne
// recopie rien — elle se contente de lire l'ancien fichier tant que le nouveau ne
// porte pas la section. Recopier obligerait à réécrire un document qu'aucun code
// ne possède, pour n'y gagner qu'un aller-retour disque au lancement.
namespace startup {

// La section racine `name`, cherchée dans le fichier de démarrage puis, à
// défaut, dans l'ancien `bourgeon_settings.yaml`.
//
// Rend un nœud INVALIDE (testable avec `if (node)`) quand ni l'un ni l'autre ne
// la porte, ou quand les deux fichiers sont absents — le premier lancement passe
// donc par ce chemin sans que ce soit une anomalie.
//
// ⚠ Le nœud rendu partage la propriété de son document : il reste valide après le
// retour, contrairement à ce que suggérerait une lecture locale.
YAML::Node Section(const char* name);

// ── La police de TOUTE l'interface ───────────────────────────────────────────
// Index de famille `ro::` : -1 = police intégrée d'ImGui, 0 = Malgun Gothic,
// >0 = une famille du système (cf. ro::ChatFamilyLabel). `fallback` est rendu
// quand aucun fichier ne porte le réglage — premier lancement compris.
//
// 🔴 ICI et plus dans `bourgeon_settings.yaml`, pour la même raison que la
// langue : ce fichier-là n'est relu qu'à l'entrée en jeu. Le choix n'agissait
// donc ni sur l'écran de login ni sur le char-select, et pire, sa relecture
// ÉCRASAIT la police qu'on venait de choisir au login.
//
// L'ancienne place (`moonlight_ui.ui_font_family`, et le booléen
// `moonlight_ui.malgun_font` d'avant lui) reste lue en repli tant que la clé
// neuve n'existe pas — comme Section() ci-dessus, sans rien recopier : la
// première écriture d'ici suffit à faire déménager le réglage.
int UiFontFamily(int fallback);

// Enregistre la police dans le fichier de démarrage.
//
// 🔴 On RELIT puis on remplace la seule clé : ce document porte aussi la langue
// et les sections `auto_login`, `char_select` et `moonlight_auth`. L'écrire à
// plat le tronquerait, et changer de police effacerait les identifiants
// d'auto-login.
void SaveUiFontFamily(int family);

}  // namespace startup
