#pragma once

#include <string>

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

// ── L'échelle de toute l'interface ───────────────────────────────────────────
// Un pourcentage (100 = taille d'origine), pour les écrans très définis où une
// interface calibrée en pixels devient minuscule. `fallback` est rendu quand
// aucun fichier ne porte le réglage — premier lancement compris.
//
// 🔴 ICI, et pour la MÊME raison que la police juste au-dessus : ce réglage doit
// agir dès l'écran de login. `bourgeon_settings.yaml` n'est relu qu'à l'entrée
// en jeu — l'échelle n'aurait pris qu'une fois connecté, et la relecture aurait
// écrasé un changement fait entre-temps.
//
// Contrairement à la police, aucune ancienne place à reprendre : le réglage
// naît ici.
int UiScalePercent(int fallback);
void SaveUiScalePercent(int percent);

// ── Le décor de connexion (features/systems/login_spectator) ─────────────────
// La ville vivante derrière le formulaire, ACTIVÉE par défaut. Opt-out : c'est
// une mise en scène, et elle coûte — un monde chargé, une session ouverte sur le
// serveur. Qui joue sur une machine juste, une connexion étroite, ou qui veut
// simplement se connecter vite, doit pouvoir s'en passer.
//
// 🔴 ICI et pas dans `bourgeon_settings.yaml`, pour la même raison que la langue
// et la police : ce réglage décide de ce qui se passe AVANT toute entrée en jeu,
// et l'autre fichier n'est relu qu'à ce moment-là. Il n'aurait donc jamais eu
// d'effet sur l'écran qu'il gouverne.
bool LoginBackdropEnabled(bool fallback);
void SaveLoginBackdropEnabled(bool enabled);

// ── Un booléen quelconque, à la racine ───────────────────────────────────────
// 🔴 Pour TOUT réglage qui agit avant l'entrée en jeu. La frontière est
// TEMPORELLE (cf. le ⚠ en tête) et elle se rappelle à nous à chaque fois :
// `bourgeon_settings.yaml` n'est relu qu'en entrant en jeu, donc un réglage qui
// gouverne l'écran de connexion y est LU TROP TARD — il vaut son défaut pendant
// tout le temps où il compte. La parade de Porings s'affichait ainsi chez des
// joueurs qui l'avaient décochée, et l'écran de veille pilotait la caméra du
// décor avec des valeurs que personne n'avait choisies.
//
// Écrit en 0/1 : ce document est tapé à la main, et un entier se relit sans se
// demander si `no` compte pour faux.
bool BoolKey(const char* key, bool fallback);
void SaveBoolKey(const char* key, bool value, const char* what);

// ── Écrire UNE clé à la racine, sans toucher au reste du document ────────────
//
// Relit le fichier, pose `key: value`, réécrit tout. Le document est relu à
// chaque fois PARCE QUE personne ne le possède : trois sections y sont posées à
// la main par le joueur (cf. le ⚠ en tête), et les écraser serait le pire des
// bugs — silencieux, et sur une configuration que l'utilisateur a tapée.
//
// `what` nomme le réglage dans le journal si l'écriture échoue. C'est le seul
// endroit qui parle à l'utilisateur en cas de disque plein ou de fichier
// verrouillé, d'où un nom en clair plutôt qu'une clé YAML.
//
// ⚠ La vérification d'erreur se fait APRÈS le flush : une ouverture réussie ne
// dit rien de l'écriture.
//
// i18n.cc portait sa propre copie de ces vingt lignes pour la langue, à la
// ligne près.
void SaveRootKey(const char* key, int value, const char* what);
void SaveRootKey(const char* key, const std::string& value, const char* what);

}  // namespace startup
