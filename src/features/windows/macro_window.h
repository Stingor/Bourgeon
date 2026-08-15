#pragma once

#include <cstdint>

#include "features/plugin.h"
#include "ragnarok/emotion_hotkey.h"

// ── MacroWindow ──────────────────────────────────────────────────────────────
//
// La fenêtre « Shortcut List » (`UIEmotionWnd`, id **86**, celle d'Alt+M) en
// ImGui : les dix macros de chat `Alt+1 … Alt+0`. RE complète :
// docs/shortcut_list_re.md.
//
// ⚠ Le nom de classe du client ment sur toute la ligne — `UIEmotionWnd` n'affiche
// aucune émote, ses bitmaps sont dans `\ShortCutList\`, son titre vient de
// `MSI_SHORTCUT_LIST` et sa sauvegarde s'appelle `EmotionHotkey`. Cinq
// vocabulaires pour un seul objet ; c'est écrit ici une fois pour que personne
// n'aille chercher du côté de `UIMacroRegisterWnd`, qui est l'anti-bot.
//
// ── CE QUE CETTE VUE CORRIGE ────────────────────────────────────────────────
//
// 1. 🔴 **FERMER NE PERD PLUS RIEN.** Chez le natif, le report des champs vers le
//    vecteur est le slot vtable +0x2C, et le cas 86 de
//    `UIWindowMgr_SaveRectAndCloseWindow` (0x00A2FC17) ne l'appelle pas : croix,
//    commande 201 ou second Alt+M détruisent la fenêtre sans rien enregistrer.
//    Seule la destruction générale (retour à l'écran de personnages, fermeture du
//    client) sauvait — et seulement si la fenêtre était restée ouverte. Ici chaque
//    frappe part dans le vecteur du client tout de suite, et le fichier suit peu
//    après (écriture différée, cf. `kSaveIdleTicks`).
// 2. **On voit enfin où part la macro.** L'envoi suit `g_ChatInputTargetMode` :
//    la même ligne part en public, en groupe, en guilde ou en clan selon l'onglet
//    de chat courant, ce que le natif n'affiche nulle part. La cible effective —
//    gardes d'appartenance comprises — est écrite en tête de la fenêtre.
// 3. **On peut essayer une macro sans fermer** : un bouton d'envoi par ligne, qui
//    passe par le chemin natif complet (`ChatMacro_SendEmotionHotkeySlot`).
// 4. **La touche affichée est la vraie.** Le natif ne remplace « Alt + n » par le
//    nom de touche réel que si la commande a une entrée dans `UserKeys.lua` ; on
//    retombe sinon sur le raccourci D'ORIGINE du client plutôt que sur un libellé
//    en dur. Un clic sur la touche ouvre l'écran des raccourcis à l'onglet Macros.
// 5. **La limite de 50 octets se voit** : le champ natif tronquait en silence.
// 6. **Réordonner** : glisser une ligne sur une autre échange les deux textes. Les
//    touches, elles, restent attachées au rang (Alt+3 est toujours la 3ᵉ ligne).
// 7. **Revenir aux valeurs d'usine**, que le natif ne sait pas faire — et avec les
//    bons ids : le client saute le `0x224` (`/lv2`) dans sa liste de défauts.
//
// ⚠ CE QUE LE NATIF SAIT ENCORE ET PAS NOUS : son bouton « view » ouvrait la
// liste d'émotes (`UICashEmotionListWnd`, id 87) et un clic y insérait le jeton
// dans la ligne QUI A LE FOCUS (`EmotionToken_InsertIntoMacroRowOrChat`, dont le
// test est `g_UIEmotionWnd != 0`). Notre fenêtre détruisant la native, ce test
// échoue et le jeton partirait dans la barre de chat : le bouton serait un piège.
// Il est donc absent, en attendant un sélecteur d'émotes à nous.
//
// INTERCEPTION — recette du menu Échap et de l'écran des raccourcis : masquer à la
// naissance dans le hook de `MakeWindow`, DÉTRUIRE au tick. La fenêtre n'existant
// jamais, toute demande d'ouverture (comportement de raccourci 114 = Alt+M)
// repasse forcément par la fabrique, qui est le seul point à couvrir.

class MacroWindow : public Plugin {
 public:
  const char* name() const override { return "MacroWindow"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // Hook de `MakeWindow` pour l'id 86 : masque la native (détruite au tick) et
  // bascule notre panneau.
  void HandleNativeCreation(void* win);

  bool IsOpen() const { return open_; }

  // ── Setting PERSISTANT (bourgeon_settings.yaml, via MoonlightUi) ────────────
  // « macrolist_imgui » : ON PAR DÉFAUT et HORS du groupe « Interface moderne »,
  // comme l'écran des raccourcis et le menu Échap. Cette fenêtre ne lit et n'écrit
  // que par les structures et les fonctions du CLIENT : elle rend le même service
  // dans les deux modes.
  bool imgui_enabled_ = true;

 private:
  void Open();
  void Close();
  // Relit les dix macros depuis le vecteur du client vers nos tampons d'édition.
  void ReloadFromClient();
  // Recopie le tampon `slot` dans le vecteur du client et arme l'écriture disque.
  void CommitRow(int slot);
  // Le libellé de touche de la ligne : surcharge du joueur, sinon raccourci
  // d'origine du client, sinon « Alt + n ». Écrit dans `out` (UTF-8).
  void KeyLabel(int slot, char* out, size_t out_size) const;
  void DrawRow(int slot);

  // ── Menu contextuel : préremplir avec une commande du serveur ──────────────
  // Le clic droit sur une ligne propose les commandes `@…` les plus utiles,
  // classées, avec leur effet en clair. C'est ce qui rend la fenêtre utilisable
  // sans connaître la liste par cœur — le natif n'offrait qu'un champ vide.
  //
  // 🔴 La liste et les descriptions sont RECOPIÉES DU SERVEUR (conf/atcommands.yml,
  // conf/import/atcommands.yml et src/custom/atcommand.inc de `moonlight`), pas
  // devinées : `@storeall` y prend un numéro de storage, `@autolootpognon` un prix
  // plancher, et `@storage1..5` sont des alias de `storagealt1..5`. Une description
  // inventée serait un contresens sur la commande d'un autre.
  //
  // Renvoie true si la ligne a été modifiée (préremplie ou vidée).
  bool DrawPrefillMenu(int slot);
  // Écrit le texte dans la ligne, le pousse au client et arme la sauvegarde.
  void SetRowText(int slot, const char* local);

  bool open_ = false;
  bool need_pos_ = false;
  bool show_panel_ = true;

  // Grâce sur la pile Échap : la frappe qui ouvre ne doit pas refermer aussitôt.
  int esc_grace_frames_ = 0;

  // 🔴 Les tampons d'édition sont en CODE-PAGE DU CLIENT, pas en UTF-8 :
  // `ro::InputTextCp949` édite en UTF-8 en interne et re-convertit, et c'est cette
  // forme-là qui part telle quelle dans le `std::string` du client. Un aller-retour
  // de plus mangerait les accents (feedback_french_ui_accents).
  // +1 pour le NUL ; la troncature à `kMaxBytes` est faite par le pont.
  char rows_[emohotkey::kSlotCount][emohotkey::kMaxBytes + 1] = {};
  bool loaded_ = false;

  // ── Écriture disque DIFFÉRÉE ───────────────────────────────────────────────
  // Le vecteur du client, lui, est à jour dès la frappe — c'est ce qui corrige le
  // défaut du natif, et ça suffit à ce que les points de sauvegarde du CLIENT
  // enregistrent la bonne valeur. `Save()` écrit deux fichiers et rebâtit la charge
  // de synchro web : on ne le joue donc pas à chaque touche, mais après une pause.
  // Et jamais depuis une frame ImGui — d'où le passage par `OnTick`.
  bool dirty_ = false;
  int  idle_ticks_ = 0;
  static constexpr int kSaveIdleTicks = 5;  // ~500 ms (OnTick ≈ 100 ms)

  // Envoi armé au rendu, joué au tick : `ChatMacro_SendEmotionHotkeySlot` peut
  // ouvrir une modale (refus de balise d'objet), donc jamais en pleine frame.
  int pending_send_ = -1;

  // Demande d'ouverture de l'écran des raccourcis, posée par le clic sur une
  // touche. Différée pour la même raison.
  bool pending_open_hotkeys_ = false;

  bool confirm_reset_ = false;
  bool pending_reset_ = false;
};
