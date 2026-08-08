#pragma once

// ── Chemins du client ────────────────────────────────────────────────────────
// Le bloc « dossier de l'exe » (GetModuleFileNameA + find_last_of + resize) était
// recopié SEPT fois, dont deux à neuf lignes d'intervalle dans le même fichier, et
// sous trois noms concurrents — GameDir(), ModuleDir(), SettingsPath() — définis
// chacun dans le namespace anonyme de son .cc. Deux d'entre eux ne se comportaient
// même pas pareil quand le chemin n'a pas de séparateur : l'un rendait le chemin
// entier, l'autre une chaîne vide.
//
// Le littéral "bourgeon_settings.yaml" était lui répété dans cinq fichiers — cinq
// endroits à corriger le jour où le fichier change de nom, alors que trois d'entre
// eux écrivent dans ce même document (cf. la fusion du root, chantier 2).

#include <string>

namespace paths {

// Dossier de l'exécutable du jeu, séparateur final INCLUS ("D:\\RO\\").
// Calculé une seule fois : le chemin de l'exe ne change pas d'une exécution.
const std::string& GameDir();

// Les fichiers que Bourgeon écrit à côté de l'exe. Un seul littéral pour chacun.
//
// ⚠ bourgeon_settings.yaml est PARTAGÉ : moonlight_ui, auto_login, char_select et
// moonlight_auth y lisent chacun leur propre section racine. Ne jamais le
// réécrire sans fusionner le document existant (cf. MoonlightUi::WriteSettingsFile).
std::string SettingsPath();
std::string MoonlightUserPath();  // identifiant web mémorisé (texte clair)
std::string MoonlightPwPath();    // mot de passe web chiffré DPAPI
std::string LastCharsPath();      // CID des derniers persos joués (récence)
// Mise en page personnalisée du char-select (sièges, ancres, fond choisi).
// Fichier DÉDIÉ et non une section de bourgeon_settings.yaml : le document est
// volumineux (25 sièges + ancres), il est réécrit à chaque sortie du mode
// « Personnaliser », et il n'a aucun lecteur en dehors de char_select_layout —
// le tenir à l'écart du yaml partagé évite d'exposer les réglages des autres
// modules à ces réécritures fréquentes.
std::string CharSelectLayoutPath();
// Disposition de la chatbox ImGui : liste des canaux (identifiant stable, nom,
// ordre, détaché ou non) et leurs 25 octets de filtre. Fichier DÉDIÉ, dans
// `SaveData\` auprès du `ChatWndInfo_U.lua` du client — même nature de donnée,
// même dossier — et surtout PAS une section de bourgeon_settings.yaml : il est
// réécrit à chaque changement de carte, et rien d'autre ne le lit. Le dossier est
// créé au besoin.
std::string ChatLayoutPath();
// Historique du chat conservé d'une session à l'autre (option). Fichier ENCORE
// distinct de la disposition : il est volumineux et réécrit à chaque déconnexion,
// alors que la disposition ne bouge presque jamais.
// ⚠ Il contient les CHUCHOTEMENTS EN CLAIR. L'option est désactivée par défaut.
std::string ChatHistoryPath();
// ── Ce que Bourgeon doit connaître AVANT d'entrer en jeu ─────────────────────
// Langue de l'interface, auto-login, char-select, login Moonlight.
//
// 🔴 La frontière n'est pas thématique, elle est TEMPORELLE, et c'est ce qui la
// rend tenable : `bourgeon_settings.yaml` n'est relu qu'à la transition vers le
// mode jeu (MoonlightUi::LoadSettings, appelée sur `in_game_ && !was_in_game`).
// Tout réglage qui doit agir sur l'écran de login ou de char-select y arriverait
// donc trop tard — la langue s'appliquait après le login, une fois l'écran qu'on
// voulait traduire déjà passé.
//
// Ce fichier-ci est lu au CHARGEMENT DE LA DLL. Y mettre un réglage, c'est dire
// « il agit avant le jeu » ; le laisser dans bourgeon_settings.yaml, c'est dire
// « il n'agit qu'en jeu ». Premier pas de l'éclatement prévu du yaml partagé.
//
// ⚠ PARTAGÉ lui aussi (langue + trois sections de login) : ne jamais le réécrire
// sans fusionner le document existant.
std::string StartupSettingsPath();
// Recettes de fabrication, générées depuis les DB serveur par
// moonlight/tools/gen_metalprocess.py.
//
// ⚠ À NE PAS confondre avec data\MetalProcessItemList.txt : celui-là est le
// fichier que le CLIENT NATIF lit (fenêtre 80), au format contraint de Gravity.
// Celui-ci est le nôtre, et il porte ce que l'autre ne peut pas exprimer —
// itemlv, compétence requise, rendements des flèches, index par compétence.
//
// Il vit dans SystemEN\ (ou System\) auprès d'itemInfoMerged.lua, et non à côté
// de l'exe : c'est de la DONNÉE DE PATCH, versionnée avec le client et livrée aux
// joueurs, pas un réglage utilisateur comme bourgeon_settings.yaml. Les deux
// dispositions de client sont essayées, comme dans MoonlightUi::LoadItemNames.
std::string RecipesPath();

// Catalogue de traduction de l'interface Bourgeon, un fichier par langue :
// `SaveData\lang\en.yaml`. Le dossier est créé au besoin.
//
// Dans SaveData\ et non auprès d'itemInfo : c'est du texte de NOTRE interface,
// pas de la donnée de patch. Un joueur peut y déposer une langue que le client
// n'embarque pas, ou corriger une tournure sans attendre un patch — c'est
// précisément pour ça que le catalogue est un fichier et pas une table compilée.
// `code` est un code ISO 639-1 minuscule ("en", "es").
std::string LangPath(const std::string& code);
// Gabarit des textes que le catalogue ci-dessus ne couvre pas encore, écrit à
// côté de lui : `SaveData\lang\en.missing.yaml`. Cf. i18n::ExportMissing.
std::string LangMissingPath(const std::string& code);

// `relative` résolu depuis le dossier du jeu. Le séparateur est déjà fourni par
// GameDir(), donc passer "data\\clientinfo.xml" et non "\\data\\clientinfo.xml".
std::string InGameDir(const std::string& relative);

}  // namespace paths
