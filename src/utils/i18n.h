#pragma once

// ── i18n : la langue de l'interface Bourgeon ─────────────────────────────────
// Bourgeon est écrit en français alors que le CLIENT, lui, est déjà anglophone :
// le dossier de données s'appelle `SystemEN`, et `itemInfomoon.lua` — les items
// custom de Moonlight — est en anglais. Notre interface est donc l'îlot français
// d'un client qui ne l'est pas. Ce module sert à réconcilier les deux.
//
// ── LE FRANÇAIS EST LA CLÉ ───────────────────────────────────────────────────
// Pas de clés symboliques (`kBtnClose`) : le libellé français DU CODE est la clé
// du catalogue. Ce choix se paie et se gagne :
//   • gagné — la migration des ~2 450 sites d'appel est mécanique (envelopper le
//     littéral), et le code reste lisible en français, comme ses commentaires ;
//   • payé — retoucher un texte français perd SILENCIEUSEMENT sa traduction.
//     C'est ce que `MissingCount()`/`ExportMissing()` existent pour rattraper :
//     toute chaîne demandée mais absente du catalogue est retenue, et on peut la
//     réexporter en gabarit à traduire.
//
// ── LE FRANÇAIS NE COÛTE RIEN ────────────────────────────────────────────────
// En français le catalogue est VIDE : `Tr` rend son argument après un seul test.
// Aucune allocation, aucune recherche, rien à charger. La langue par défaut est
// donc exactement aussi rapide qu'avant l'existence de ce fichier.
//
// ── ⚠ CE QUE `Tr` ATTEND : UN LITTÉRAL ───────────────────────────────────────
// `Tr("Fermer")`, jamais `Tr(buffer)`. Une chaîne construite à l'exécution (nom
// de joueur, texte formaté, message serveur) n'a rien à faire ici : elle ne sera
// dans aucun catalogue, et elle polluerait la liste des textes manquants avec
// autant d'entrées qu'il y a de valeurs possibles. Pour un texte à trous, c'est
// le GABARIT qu'on traduit, et lui seul :
//     std::snprintf(buf, n, Tr("%d objets vendus"), count);
// Les messages venant du SERVEUR se relaient tels quels — cf. la règle « message
// serveur exact » : les traduire les rendrait faux.

#include <cstddef>
#include <string>
#include <vector>

namespace i18n {

// ── Traduire ─────────────────────────────────────────────────────────────────
// Rend la traduction de `fr` dans la langue courante, ou `fr` lui-même si la
// langue est le français, si la clé manque au catalogue, ou si sa traduction est
// vide. Le résultat n'est donc JAMAIS nul : un catalogue incomplet dégrade en
// français, il ne blanchit pas l'interface.
//
// 🔴 Le pointeur rendu appartient au catalogue et meurt au prochain
// `SetLanguage()`. Le consommer dans la frame (ce que fait tout appel ImGui, qui
// copie) est sûr ; le stocker dans un `static const char*` ne l'est pas.
//
// ⚠ À n'appeler que depuis le fil de RENDU. Le catalogue n'est pas gardé par un
// verrou : le protéger coûterait à chaque libellé de chaque frame pour un accès
// concurrent qui n'existe pas — le fil recv ne dessine rien (il copie et rend la
// main), et c'est OnRenderUI qui bascule la langue.
const char* Tr(const char* fr);

// ── Traduire SANS changer l'identité du widget ───────────────────────────────
// Rend « <traduction>###<stable_id> », soit le libellé traduit suivi d'un
// identifiant ImGui figé.
//
// 🔴 À UTILISER PARTOUT OÙ IMGUI DÉRIVE UN ÉTAT DU LIBELLÉ. ImGui hache le texte
// d'un widget pour l'identifier ; traduire ce texte crée donc un NOUVEAU widget
// aux yeux d'ImGui, et l'état attaché à l'ancien est perdu. Ce qui se perd n'est
// pas cosmétique :
//   • ImGui::Begin        — position et taille de la fenêtre dans imgui.ini ;
//   • TableSetupColumn    — largeur des colonnes, colonne et sens du tri ;
//   • OpenPopup/BeginPopup— le popup ne s'ouvre plus, les deux noms divergeant ;
//   • BeginTabItem        — l'onglet sélectionné.
// La partie après « ### » ne s'affiche pas et seule elle compte pour l'identité,
// d'où un widget qui garde son état d'une langue à l'autre.
//
// Un simple ImGui::Text, Button ou Selectable n'a pas besoin de ça : rien n'y
// survit d'une frame à l'autre. `Tr` suffit.
//
// `stable_id` se passe NU (« bourgeon_language »), sans « ### » : la fonction
// l'ajoute. Il doit être unique dans sa fenêtre et ne JAMAIS changer ensuite —
// le renommer revient exactement à traduire le libellé.
//
// ⚠ Le pointeur rendu vient d'un petit anneau de tampons : il reste valide le
// temps de l'appel ImGui qui suit, pas au-delà. Huit appels imbriqués dans la
// même expression sont sûrs ; au neuvième, le premier est recyclé.
const char* TrId(const char* fr, const char* stable_id);

// ── La langue courante ───────────────────────────────────────────────────────
// Code ISO 639-1 minuscule : "fr", "en", "es". "fr" = la langue SOURCE.
const std::string& LanguageCode();

// Pose la langue, recharge le catalogue ET l'écrit sur le disque. Sans effet si
// le code est déjà celui en cours — recharger jetterait les pointeurs rendus par
// `Tr` pour rien. Rend true si la langue a changé.
//
// La persistance est FAITE ICI, et pas laissée à l'appelant : le combo du login
// s'affiche avant que `MoonlightUi` n'ait de cycle d'écriture, et un réglage qui
// ne tient que jusqu'à la fermeture du jeu passerait pour un bug.
bool SetLanguage(const std::string& code);

// Recharge le catalogue de la langue courante depuis le disque, sans rien
// changer d'autre : pour reprendre à chaud un fichier de langue qu'on vient de
// corriger.
void ReloadCatalog();

// ── La GÉNÉRATION du catalogue ───────────────────────────────────────────────
// Un compteur qui avance à CHAQUE (re)chargement du catalogue, donc à chaque
// changement de langue. Il ne veut rien dire seul : il se compare à la valeur
// qu'on avait mémorisée.
//
// 🔴 À METTRE DANS LA CLÉ DE TOUT CACHE QUI MESURE DU TEXTE TRADUIT. Une largeur
// calculée avec `CalcTextSize(Tr(...))` puis conservée est calibrée pour UNE
// langue et fausse pour les autres — « Suivi de quête » devient « Seguimiento de
// misiones ». La panne est MUETTE : le texte est simplement coupé, sans erreur ni
// avertissement, et elle ne se voit qu'en changeant de langue EN COURS DE PARTIE.
// Mesurer les libellés traduits ne suffit donc pas : encore faut-il les remesurer
// quand la traduction change — un cache qui ne s'invalide que sur la police
// laisse le bug entier derrière lui.
unsigned CatalogEpoch();

// ── La langue, au CHARGEMENT DE LA DLL ───────────────────────────────────────
// À appeler une fois, tôt, avant que quoi que ce soit ne dessine.
//
// 🔴 La langue vivait dans `bourgeon_settings.yaml`, lu par `MoonlightUi` à la
// seule transition vers le mode jeu. L'écran de login et le char-select se
// dessinaient donc TOUJOURS en français, quel que soit le réglage — l'anglophone
// à qui la traduction s'adresse ne la voyait qu'une fois connecté. Elle est
// maintenant dans `paths::StartupSettingsPath()`, lu ici.
//
// Reprend l'ancienne clé `moonlight_ui.language` si le nouveau fichier n'existe
// pas encore, et l'écrit à son nouvel emplacement : le choix déjà fait par un
// joueur ne doit pas disparaître dans le déménagement.
void LoadLanguageSetting();

// ── Les langues proposées ────────────────────────────────────────────────────
struct Language {
  const char* code;      // "fr", "en", "es"
  const char* label;     // ce que le joueur lit dans le combo : « Français »
  bool available;        // le catalogue existe sur le disque (toujours vrai en fr)
};

// Les langues CONNUES du client, dans l'ordre d'affichage, avec pour chacune la
// présence de son fichier. La liste est en dur, mais la disponibilité est lue sur
// le disque : déposer `SaveData\lang\es.yaml` suffit à activer l'espagnol, sans
// recompiler. Relue à chaque appel — le combo ne s'ouvre pas assez souvent pour
// que ça compte, et une langue déposée en cours de partie apparaît aussitôt.
std::vector<Language> AvailableLanguages();

// Le libellé d'un code, pour l'afficher hors du combo.
//
// ⚠ Pour un code CONNU, rend un littéral immortel. Pour un code inconnu, rend
// `code.c_str()` — le pointeur ne vit alors pas plus longtemps que l'argument.
// Ne pas l'appeler sur un temporaire : `LabelOf(LanguageCode())` est sûr,
// `LabelOf(std::string(node["language"]))` ne l'est pas.
const char* LabelOf(const std::string& code);

// ── Les textes non traduits ──────────────────────────────────────────────────
// Toute chaîne demandée à `Tr` et absente du catalogue est retenue ici (langue
// non française uniquement — en français il n'y a par définition rien à
// traduire). C'est le filet de sécurité du choix « le français est la clé » :
// un libellé retouché dans le code réapparaît immédiatement dans cette liste.
std::size_t MissingCount();

// Écrit les textes manquants en gabarit YAML prêt à traduire, à côté du
// catalogue : `SaveData\lang\<code>.missing.yaml`. Chaque entrée y a sa clé
// française et une valeur VIDE — `Tr` traitant une traduction vide comme
// absente, un gabarit rempli à moitié se comporte correctement.
// Rend false si l'écriture échoue ; `out_path` reçoit le chemin visé dans tous
// les cas, pour que l'appelant puisse le montrer.
bool ExportMissing(std::string* out_path);

}  // namespace i18n
