#pragma once

#include "yaml-cpp/yaml.h"

// ── settings_containers : les réglages qui ne sont pas des valeurs ───────────
// En-tête PRIVÉ au dossier src/plugins/moonlight_ui/ (cf. internal.h).
//
// La table de descripteurs (settings_table.h) décrit un réglage par l'adresse
// d'un champ : un bool, un int, une couleur. Six réglages n'entrent pas dans ce
// moule — ce sont des COLLECTIONS, avec leur propre forme dans le yaml (map de
// maps, séquence d'objets, séquence d'entiers) et souvent un tri à l'écriture
// pour que le fichier reste stable d'une sauvegarde à l'autre.
//
// Chacun garde EXACTEMENT sa place dans l'ordre des clés : ces fonctions sont
// appelées là où le bloc était écrit, pour que le yaml d'un joueur ne bouge pas.
//
// Chaque paire lit et écrit la même chose ; c'est le seul endroit à regarder
// pour savoir comment une collection est persistée.

namespace moonlight_ui {

// Positions et visibilité par icône de menu, sous « menu_icons: { <nom>: {…} } ».
// Rangées à la lecture parce que la liste d'icônes vivante n'existe qu'en jeu :
// MenuIconTweaks::BuildIconList les applique plus tard.
void ReadMenuIcons(const YAML::Node& ui);
void WriteMenuIcons(YAML::Emitter& out);

// Placement libre de l'inventaire : nameid -> index de case (client seul).
void ReadInventoryLayout(const YAML::Node& ui);
void WriteInventoryLayout(YAML::Emitter& out);

// Favoris d'entrepôt : une séquence d'ids d'items (client seul).
void ReadStorageFavorites(const YAML::Node& ui);
void WriteStorageFavorites(YAML::Emitter& out);

// Skin RO : la configuration vivante (clés « ro_skin_* ») ET les presets nommés.
// Les deux partagent le même format, d'où la même paire de fonctions.
// ⚠ `rounding` n'est PAS persisté dans les presets, et l'y ajouter changerait le
// comportement : appliquer un thème écraserait l'arrondi choisi par le joueur.
void ReadSkinAndPresets(const YAML::Node& ui);
void WriteSkinAndPresets(YAML::Emitter& out);

// Presets d'équipement (loadouts nommés, par CID) — possédés par CharacterSheet.
void ReadEquipPresets(const YAML::Node& ui);
void WriteEquipPresets(YAML::Emitter& out);

}  // namespace moonlight_ui
