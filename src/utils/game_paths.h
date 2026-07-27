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

// `relative` résolu depuis le dossier du jeu. Le séparateur est déjà fourni par
// GameDir(), donc passer "data\\clientinfo.xml" et non "\\data\\clientinfo.xml".
std::string InGameDir(const std::string& relative);

}  // namespace paths
