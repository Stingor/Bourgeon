#pragma once

#include "yaml-cpp/yaml.h"

// ── settings_containers : les réglages qui ne sont pas des valeurs ───────────
// En-tête PRIVÉ au dossier src/features/moonlight_ui/ (cf. internal.h).
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
// MenuIcons::BuildIconList les applique plus tard.
void ReadMenuIcons(const YAML::Node& ui);
void WriteMenuIcons(YAML::Emitter& out);

// Placement libre de l'inventaire : nameid -> index de case (client seul).
void ReadInventoryLayout(const YAML::Node& ui);
void WriteInventoryLayout(YAML::Emitter& out);

// Marqueurs de la minimap, sous « minimap_memos: { <carte>: [ {x,y,name}, … ] } ».
// La clé est le nom de carte SANS extension ; une carte sans marqueur n'a pas
// d'entrée. Les cartes sont écrites dans l'ordre de la std::map (donc trié), et
// les marqueurs dans leur ordre de création — le fichier reste stable d'une
// sauvegarde à l'autre.
void ReadMinimapMemos(const YAML::Node& ui);
void WriteMinimapMemos(YAML::Emitter& out);

// NPC rendus sourds au clic gauche : séquence d'objets { id, name }. L'id est un
// GID de la plage réservée aux NPC à identifiant fixe (moon/npc_fixed_id.yml) —
// c'est LUI la clé ; le nom n'est là que pour nommer la ligne du panneau quand
// le NPC est sur une autre carte. Possédés par EntityContextMenu.
void ReadBlockedNpcs(const YAML::Node& ui);
void WriteBlockedNpcs(YAML::Emitter& out);

// Raccourcis des actions BOURGEON : séquence d'objets { id, vk, ctrl, alt, shift }.
// La clé est l'identifiant STABLE de l'action (features/hotkey_actions.h), jamais
// son index — réordonner le catalogue ne doit déplacer aucun raccourci. Seules
// les actions RÉELLEMENT liées sont écrites, et une entrée dont l'identifiant est
// inconnu est ignorée (action retirée depuis).
//
// ⚠ Ne couvre PAS les raccourcis du JEU : ceux-là vivent dans `UserKeys.lua`, que
// le client téléverse lui-même par compte (cf. project_external_settings_re). Les
// deux mondes se rejoignent à l'ÉCRAN, pas dans le stockage.
void ReadBourgeonHotkeys(const YAML::Node& ui);
void WriteBourgeonHotkeys(YAML::Emitter& out);

// Favoris d'entrepôt : une séquence d'ids d'items (client seul).
void ReadStorageFavorites(const YAML::Node& ui);
void WriteStorageFavorites(YAML::Emitter& out);

// Onglets d'entrepôt personnalisés : id de storage -> nom libre + icône d'item
// (client seul ; le serveur, lui, n'envoie que ses propres noms).
void ReadStorageTabCustom(const YAML::Node& ui);
void WriteStorageTabCustom(YAML::Emitter& out);

// Skin RO : la configuration vivante (clés « ro_skin_* ») ET les presets nommés.
// Les deux partagent le même format, d'où la même paire de fonctions.
// ⚠ `rounding` n'est PAS persisté dans les presets, et l'y ajouter changerait le
// comportement : appliquer un thème écraserait l'arrondi choisi par le joueur.
void ReadSkinAndPresets(const YAML::Node& ui);
void WriteSkinAndPresets(YAML::Emitter& out);

// Presets d'équipement (loadouts nommés, par CID) — possédés par CharacterSheet.
void ReadEquipPresets(const YAML::Node& ui);
void WriteEquipPresets(YAML::Emitter& out);

// Couleurs de fond des trois fenêtres de chat — possédées par ChatTweaks. Ce
// n'est pas une table de descripteurs parce que les CLÉS appartiennent aux
// groupes eux-mêmes (chat_bg, chat_bg_detached, chat_bg_whisper) et non à des
// littéraux : elles restent au même endroit que les sites qu'elles patchent.
// L'application est différée à ChatTweaks::ApplyAllBackgrounds (PostLoadApply).
void ReadChatBackgrounds(const YAML::Node& ui);
void WriteChatBackgrounds(YAML::Emitter& out);

// Préréglages de couleur de chat (séquence nom + hex ARGB), partagés par les
// trois groupes. Possédés par ChatTweaks.
void ReadChatBgPresets(const YAML::Node& ui);
void WriteChatBgPresets(YAML::Emitter& out);

// ── Réglages INDEXÉS ─────────────────────────────────────────────────────────
// Même raison que les collections : leurs clés n'existent pas comme littéraux,
// elles se construisent à l'exécution (« expbar_ » + kBarKeys[i] + « _x »…), ce
// qu'une table de descripteurs ne peut pas décrire.

// Disposition des barres EXP/HP/SP : expbar_<barre>_{show,x,y,w,h,color}.
void ReadBarLayout(const YAML::Node& ui);
void WriteBarLayout(YAML::Emitter& out);

// Disposition du portrait par élément : portrait_<élément>_{show,x,y,w,h,rounding,bg,fg}.
void ReadPortraitLayout(const YAML::Node& ui);
void WritePortraitLayout(YAML::Emitter& out);
// Les cinq cadres du HUD de cible (position, taille, couleurs, on/off).
// Même forme que les barres et le portrait : un tableau d'éléments ne se
// décrit pas par l'adresse d'un champ, donc il ne passe pas par la table.
void ReadTargetLayout(const YAML::Node& ui);
void WriteTargetLayout(YAML::Emitter& out);

// Barres de raccourcis : skillbarN_* (3 barres fixes) + skillbar_itemN (contenu
// persisté de la barre d'items).
void ReadSkillBarLayout(const YAML::Node& ui);
void WriteSkillBarLayout(YAML::Emitter& out);

// Positions de fenêtres natives mémorisées : status_pos_*, equip_pos_*, puis la
// table de WindowPosTweaks (achievement, banque, courrier…). INT_MIN = « aucune
// position mémorisée », la fenêtre garde son placement natif.
void ReadWindowPositions(const YAML::Node& ui);
void WriteWindowPositions(YAML::Emitter& out);

// ── Clés renommées ───────────────────────────────────────────────────────────
// Recopie les anciennes clés sur les nouvelles quand celles-ci manquent, DANS
// L'ARBRE EN MÉMOIRE (le fichier n'est pas touché ; il se réécrit sous les
// nouveaux noms à la sauvegarde suivante). Tout le reste de la lecture ignore
// donc l'existence des anciens noms.
//
// À appeler en PREMIER, avant toute lecture. Datée : supprimable quand plus
// aucun yaml de joueur ne portera les anciennes clés.
void MigrateLegacyKeys(YAML::Node ui);

}  // namespace moonlight_ui
