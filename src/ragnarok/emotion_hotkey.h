#pragma once

#include <cstddef>

// ── emotion_hotkey : les dix macros de chat du client, en accès typé ─────────
// (client 20250716, base 0x400000 — RE : docs/shortcut_list_re.md)
//
// Ce sont les lignes « Alt + 1 … Alt + 0 » de la fenêtre « Shortcut List »
// (`UIEmotionWnd`, id 86). Elles vivent dans un `std::vector<std::string>` de dix
// entrées à `g_UIWindowContextKey+0xFD8`, et un raccourci les envoie par le
// pipeline de chat complet du client.
//
// 🔴 CE MODULE EXISTE POUR CORRIGER LE DÉFAUT CENTRAL DU NATIF. Chez le client,
// le report des champs de saisie vers ce vecteur est `EmotionHotkey_SaveFromEditBoxes`
// (slot vtable +0x2C), et le cas 86 de la fermeture NE L'APPELLE PAS : fermer la
// fenêtre perd ce qu'on vient de taper. Ici, `WriteLocal` écrit dans le vecteur
// tout de suite — donc même sans `Save()`, les points de sauvegarde du CLIENT
// (retour à l'écran de personnages, fermeture) enregistrent la bonne valeur.
//
// ⚠ TOUT EST PAR COMPTE, jamais par personnage : le fichier local porte l'AID
// obfusqué et le nœud JSON part dans la synchro `/userconfig`.

namespace emohotkey {

// Dix, et c'est une contrainte du client : le vecteur est dimensionné par son
// chargeur, les comportements de raccourci 200..209 indexent dedans, et
// `EmotionHotkey_IsDirty` compare exactement dix paires.
// ⚠ Dix emplacements de MACRO. Rien à voir avec les dix de `rag::equip`, qui
// sont ceux de l'ÉQUIPEMENT : même nom, même valeur, aucun rapport. Ne pas les
// fusionner sur la foi d'un relevé par valeur.
constexpr int kSlotCount = 10;

// Longueur maximale d'une macro, en octets de la code-page du client.
//
// 🔴 C'est celle du champ de saisie NATIF (`edit+0x88` = 50), et on la garde
// délibérément : le joueur peut revenir à la fenêtre native (réglage
// `macrolist_imgui`), et son champ tronquerait alors en silence tout ce qui
// dépasse. Le client sait techniquement en envoyer plus (son tampon d'envoi fait
// 256 octets), mais accepter 90 caractères ici pour les perdre au premier
// aller-retour serait un cadeau empoisonné.
constexpr size_t kMaxBytes = 50;

// Vrai quand le vecteur du client est en place (dix entrées). Faux avant l'entrée
// en jeu, et c'est la garde à poser avant tout le reste.
bool Ready();

// Lit la macro `slot` telle quelle, dans la code-page du client — la forme
// qu'attend `ro::InputTextCp949`. Renvoie false si le vecteur n'est pas prêt.
bool ReadLocal(int slot, char* out_local, size_t out_size);

// Écrit la macro `slot` depuis un tampon en code-page du client, DANS LE VECTEUR
// DU CLIENT (via son propre `std::string::assign`, donc son allocateur).
// Tronque à `kMaxBytes`. N'écrit rien sur le disque : appeler `Save()`.
bool WriteLocal(int slot, const char* local);

// Grave les macros : `savedata\shortcutlist\<AID obfusqué>.lua` ET le nœud JSON
// `EmotionHotkey` (donc la synchro web par compte). C'est `UserSettings_SaveJson`,
// exactement ce qu'appelle le natif après son report.
//
// 🔴 APPEL NATIF : jamais depuis une frame ImGui
// (feedback_no_native_cmd_during_imgui_frame). Il écrit deux fichiers — à ne pas
// jouer à chaque frappe non plus.
bool Save();

// Envoie la macro `slot` par `ChatMacro_SendEmotionHotkeySlot 0x00A47400` : le
// seul chemin d'envoi du client qui ne dépende d'aucune fenêtre (filtre de
// balises, `/commandes`, puis `CMode::SendMsg` selon la cible courante).
//
// 🔴 APPEL NATIF, et il peut ouvrir une modale : jamais depuis une frame ImGui.
bool Send(int slot);

// Les dix valeurs d'usine, telles que `EmotionHotkey_LoadListFromLua` les pose :
// msgstring 0x220..0x223, **0x225**, 0x226..0x22A — le 0x224 (`/lv2`) est sauté
// par le client, et reproduire dix ids consécutifs décalerait sept lignes.
// `slot` hors bornes ou table de messages absente -> nullptr.
const char* DefaultLocal(int slot);

// ── Où part une macro ────────────────────────────────────────────────────────
// L'envoi suit `g_ChatInputTargetMode`, c'est-à-dire l'onglet de chat courant :
// la MÊME macro part en public, en groupe, en guilde ou en clan selon un état que
// la fenêtre native n'affiche nulle part. Les gardes d'appartenance du natif sont
// rejouées ici (pas de guilde -> retombe en public), pour que ce qu'on montre
// soit ce qui partira vraiment.
enum class Target { kPublic, kParty, kGuild, kClan };
Target CurrentTarget();

}  // namespace emohotkey
