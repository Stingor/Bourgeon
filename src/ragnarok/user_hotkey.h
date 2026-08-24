#pragma once

#include <cstdint>

// ── user_hotkey : les raccourcis clavier du CLIENT, en accès typé ────────────
// (client 20250716, base 0x400000 — RE : docs/game_option_re.md §4)
//
// Le client range ses raccourcis côté LUA (`SaveData\UserKeys.lua`, tables
// `USERKEY_<catégorie+1>`) et n'expose que des ponts C minces. Ce module les
// enveloppe une fois pour toutes : SEH, destruction des `std::string` que les
// ponts allouent, et une struct de ligne prête à dessiner.
//
// 🔴 POURQUOI PASSER PAR LE NATIF ET NE RIEN RECOPIER. Le nom de touche rendu
// ici est **layout-aware** : il vient du jeu, donc « A » sur AZERTY et « Q » sur
// QWERTY, rebinds du joueur compris. Une table de touches écrite à la main a
// déjà été proposée puis refusée dans ce projet pour cette raison exacte
// (cf. project_shortcut_bar_re).
//
// ⚠ ÉTAT : LECTURE SEULE. L'écriture (`ChangeUserHotKey`, `SaveUserHotKeys2`)
// et le contrôle de collision (MsgStringTable 1489) ne sont pas encore RE'd —
// c'est le chantier suivant, décrit dans docs/game_option_re.md §5.8. Tant qu'il
// n'est pas fait, le remappage passe par la fenêtre native.

namespace userhotkey {

// Les quatre catégories, telles que le Lua les numérote (0-based côté C, +1 côté
// Lua). ⚠ L'ordre des ONGLETS de la fenêtre native n'est PAS celui-ci — voir
// `CategoryForTab`.
enum Category {
  kSkillBar1 = 0,   // USERKEY_1 — barre de raccourcis, onglet 1
  kInterface = 1,   // USERKEY_2 — fenêtres et commandes d'interface
  kMacros    = 2,   // USERKEY_3 — l'onglet que la fenêtre native nomme « Macros »
  kSkillBar2 = 3,   // USERKEY_4 — barre de raccourcis, onglet 2
  kCategoryCount = 4,
};

// Onglet affiché -> catégorie Lua. 0->0, 1->3, 2->1, 3->2.
//
// 🔴 L'ordre visuel n'est PAS l'ordre des catégories, et s'y tromper affiche
// silencieusement les raccourcis d'un autre onglet. On appelle le natif
// (`UserHotkey_TabIndexToCategory` 0x005D4C50) plutôt que de recopier la table.
int CategoryForTab(int tab_index);

// Nombre de lignes AFFICHABLES de la catégorie, trous compris… c'est-à-dire
// déduits : pour la catégorie Interface le natif retranche lui-même les douze
// commandes non remappables (index 30, 33, 34, 36, 43, 45, 53, 58, 62, 64, 65,
// 66). 0 si le Lua n'est pas prêt.
int RowCount(int category);

// Index de commande de la n-ième ligne affichable, ou -1 s'il n'y en a pas.
// C'est lui — et pas le numéro de ligne — qu'attendent les autres ponts.
int CommandIndexAt(int category, int row);

// Une ligne de la table des raccourcis.
struct Binding {
  int  command_index = -1;  // ce que `CommandIndexAt` a rendu
  int  key_code1 = 0;       // code de la touche principale (0 = aucune)
  int  key_code2 = 0;       // modificateur / seconde touche
  char key_name[64] = {0};  // « F1 », « A », « Shift+F1 » — layout-aware, UTF-8
  char label[128] = {0};    // « Hotkey 2-1 » — le champ EXE de UserKeys.lua, UTF-8
  bool assigned = false;    // false = aucune touche affectée à cette commande
};

// Remplit `out` pour la ligne demandée. Renvoie false si la ligne n'existe pas
// (label vide côté client = commande inexistante, c'est le test de validité du
// natif). Les deux `std::string` allouées par le pont sont détruites ici.
bool ReadBinding(int category, int row, Binding* out);

// Raccourci PAR DÉFAUT d'une commande — ce que le client lui donnerait sur une
// installation neuve. Renvoie false si la commande n'existe pas.
//
// C'est la brique du [Reset] natif, mais prise UNITAIREMENT : son bouton passe
// les quatre catégories entières (`UIHotKeyWnd_StageDefaultBindings` 0x008E2930),
// là où une seule ligne suffit souvent. Le natif n'expose aucun pont C pour ce
// global : on appelle donc `GetOriginalHotKeyInfo(cat+1, cmdIdx)` par l'API C de
// Lua (`ragnarok/lua.h`), exactement ce que le natif fait en ligne.
bool ReadDefaultBinding(int category, int command_index, Binding* out);

// ── Écriture ────────────────────────────────────────────────────────────────
// RE : docs/game_option_re.md §4.9.

// Affecte une touche à une commande — ou l'EFFACE si `key1` et `key2` valent 0.
//
// `label_utf8` est le libellé rendu par `ReadBinding` : côté Lua c'est LUI qui
// identifie l'entrée (champ EXE), il doit donc être repassé tel quel. Il est
// reconverti ici vers la code-page du client, et emballé dans un `std::string`
// construit avec l'allocateur du jeu — le pont natif attend un vrai `std::string`,
// pas un `char*`.
//
// ⚠ Les codes sont des VK Windows (65 = « A », 16/17/18 = Maj/Ctrl/Alt), donc
// exactement ce que rend `hotkeys::CaptureMainVk`. `key2` est le modificateur.
//
// N'écrit RIEN sur le disque : appeler `Save()` après la rafale.
bool WriteBinding(int category, int command_index, int key1, int key2,
                  const char* label_utf8);

// Grave `SaveData\UserKeys.lua`. Une fois, après les écritures.
bool Save();

// ── Le pont C brut, pour qui ne peut pas passer par l'API ci-dessus ─────────
// `UserHotkey_Lua_GetHotKey(out, catégorie, index)` — le Lua `GetHotKey(cat+1,
// idx)`, qui remplit une struct de 0x38 octets au format « dd>ddss ».
//
// La barre de raccourcis l'appelle directement plutôt que par `ReadBinding` :
// elle est dessinée sous SEH, dans une portée qui s'interdit tout objet C++ à
// dérouler, et la struct de ligne de ce module en contient.
//
// 🔴 Elle construit DEUX `std::string` qu'il faut DÉTRUIRE (`rag::kStdStringDtorAddr`) :
// au-delà de quinze caractères, le nom part sur le tas et fuit sinon.
constexpr uintptr_t kGetHotKeyAddr = 0x00d80950;

}  // namespace userhotkey
