#pragma once

#include "imgui.h"

// ── hotkey_util : les raccourcis clavier de Bourgeon, décrits au même endroit ─
// Plusieurs fonctionnalités laissent le joueur choisir sa touche : les presets
// d'équipement de la fiche de personnage, le saut, l'enregistreur de zone et les
// actions du catalogue (hotkey_actions). Elles partagent ici la capture du combo,
// son libellé et — le point qui compte — le CONTRÔLE DE CONFLIT : un combo déjà
// pris est refusé en nommant son propriétaire. Sans ce contrôle la même touche
// déclencherait deux actions à la fois, sans que rien ne l'explique au joueur.
//
// 🔴 LE CONTRÔLE TRAVERSE LES DEUX MONDES. Il inspecte aussi les raccourcis du
// CLIENT, et les QUATRE catégories de `UserKeys.lua` — pas seulement les deux
// barres de raccourcis. Les commandes d'interface du jeu (Alt+E, Alt+Q…) partent
// par le même chemin clavier que les nôtres : les ignorer laissait poser une
// touche déjà prise, donc deux actions sur une frappe.
//
// Touches capturables : lettres, chiffres, F1..F12 et Espace. Rien d'autre,
// volontairement : ce sont celles dont on SAIT que le natif les route vers
// ProcessPushButton (d'où viennent OnKeyDown et les hotkeys de skill). Une
// touche non routée donnerait un raccourci muet, indiagnosticable côté joueur.
namespace hotkeys {

// Qui demande le combo, pour s'exclure du contrôle : on ne se déclare pas en
// conflit avec soi-même en redéfinissant son propre raccourci.
enum class Owner {
  kNone,          // rien à exclure
  kEquipPreset,   // preset d'équipement ; self_index = son index dans equip_presets()
  kJump,          // touche de saut (PlayerJump)
  kZoneRecorder,  // touches de l'enregistreur de zone ; self_index = kZoneRecKey*
  kAction,        // action Bourgeon ; self_index = son index dans hotkey_actions
  kClientCommand, // commande du CLIENT ; self_index = ClientSelf(cat, cmdIdx)
  kKeyboardMove,  // déplacement clavier ; self_index = son slot (KeyboardMove::k*)
};

// Encodage du `self_index` d'une commande du client : elle a besoin de DEUX
// nombres (catégorie et index de commande) là où la signature n'en passe qu'un.
inline constexpr int kClientSelfScale = 1000;  // > tout index de commande (max ~70)
inline constexpr int ClientSelf(int category, int command_index) {
  return category * kClientSelfScale + command_index;
}

// L'enregistreur de zone porte TROIS touches distinctes, qui se contrôlent l'une
// contre l'autre comme n'importe quelle autre paire : elles se passent en
// `self_index` pour ne pas se déclarer en conflit avec elles-mêmes.
inline constexpr int kZoneRecKeyRecord = 0;  // lance / arrête l'enregistrement
inline constexpr int kZoneRecKeySelect = 1;  // retrace la zone
inline constexpr int kZoneRecKeyShot   = 2;  // une image fixe de la même zone

int      ImGuiKeyToVk(ImGuiKey key);
ImGuiKey VkToImGuiKey(int vkey);

// Première touche PRINCIPALE pressée cette frame (modificateurs exclus), en VK
// Windows ; 0 si aucune. À appeler pendant le rendu ImGui.
//
// Jeu RESTREINT — lettres, chiffres, F1..F12, Espace — et c'est délibéré pour les
// actions de BOURGEON : ce sont les touches dont on SAIT que le natif les route
// vers `ProcessPushButton`, d'où viennent `OnKeyDown` et notre dispatch. Une
// touche non routée donnerait un raccourci muet, indiagnosticable côté joueur.
int CaptureMainVk();

// Le jeu LARGE : tout ce qui précède, plus F13..F24, la ponctuation, le pavé
// numérique, les touches d'édition et de navigation, Impr. écran et Pause.
//
// 🔴 RÉSERVÉ AUX COMMANDES DU CLIENT. Elles ne passent pas par notre dispatch
// mais par le sien, qui accepte bien plus que `ProcessPushButton` : s'en tenir au
// jeu restreint rendait IRRÉCUPÉRABLE toute commande dont la touche par défaut
// n'y figure pas — « Screenshot » sur Impr. écran, par exemple, était effaçable
// et plus jamais réattribuable (constaté en jeu le 2026-08-14).
//
// ⚠ Échap n'y est pas, ni les modificateurs seuls : la première annule la
// capture, les seconds ne sont pas des touches principales.
int CaptureAnyVk();

// Libellé lisible du combo : « Ctrl+Maj+F1 », « Espace », « (aucun) ».
void Label(int vkey, bool ctrl, bool alt, bool shift, char* out, int cap);

// Le combo est-il déjà attribué ? Renvoie true et décrit son propriétaire dans
// `what` (au plus `cap` octets, toujours terminé).
bool Conflict(int vkey, bool ctrl, bool alt, bool shift, Owner self, int self_index,
              char* what, int cap);

// Une zone de saisie NATIVE a le focus (chat, message privé, montant de vente…) :
// la frappe est un caractère, pas un raccourci. Réplique la garde de
// UIWindowMgr_OnKeyDown (0x00a471e0). Vit ici parce que TOUT raccourci global doit
// la consulter — sans elle, taper « zoulou » dans le chat fait courir, sauter, et
// maintenant filmer. En cas de lecture impossible, renvoie true : dans le doute on
// n'agit pas.
bool NativeTextInputHasFocus();

// Une capture de combo est en cours quelque part dans l'UI : les raccourcis ne
// doivent pas se déclencher pendant ce temps, la touche pressée sert à remapper.
// L'écran qui capture appelle PingCapture() À CHAQUE FRAME ; la capture expire
// ainsi d'elle-même dès qu'il cesse d'être dessiné (onglet quitté, fenêtre
// repliée) au lieu de laisser les raccourcis morts jusqu'au redémarrage.
void PingCapture();
bool CaptureInProgress();

}  // namespace hotkeys
