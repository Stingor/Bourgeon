#pragma once

#include "imgui.h"

// ── hotkey_util : les raccourcis clavier de Bourgeon, décrits au même endroit ─
// Plusieurs fonctionnalités laissent le joueur choisir sa touche : les presets
// d'équipement de la fiche de personnage, le saut, l'enregistreur de zone et les
// actions du catalogue (hotkey_actions). Elles partagent ici la capture du combo,
// son libellé et — le point qui compte — le CONTRÔLE DE CONFLIT : sans lui, la
// même touche déclencherait deux actions à la fois, sans que rien ne l'explique
// au joueur.
//
// Il se lit sous DEUX formes, et le choix n'est pas cosmétique :
//   · `Conflict` ne rend qu'une PHRASE — de quoi refuser en nommant le
//     propriétaire. C'est ce que font les écrans qui n'ont pas la table des
//     raccourcis sous les yeux (le saut, les presets, l'enregistreur de zone) :
//     ils ne pourraient pas montrer ce qu'ils viennent d'effacer.
//   · `FindConflicts` rend l'IDENTITÉ de chaque détenteur — de quoi aller lui
//     RETIRER la touche. C'est ce que fait la table des raccourcis, comme le
//     natif : refuser y laissait la touche à l'ancienne fonction alors que le
//     joueur venait de la donner à une autre.
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

// Le jeu de touches des actions de BOURGEON : `CaptureMainVk` plus **Tab**.
//
// 🔴 Tab n'y était pas, et son absence n'était pas un choix : c'était le filet de
// sécurité « ne pas offrir une touche que le jeu ne route pas ». Vérification
// faite dans le binaire, il la route :
//   · `Game_MainWndProc` (0x00DB8100) ne teste JAMAIS VK 9 — aucune comparaison à
//     9 dans toute la fonction — donc Tab descend jusqu'à
//     `UIWindowMgr_OnKeyDown` (0x00A471E0), c'est-à-dire jusqu'à notre hook ;
//   · là, le case 9 tombe dans le dispatch des raccourcis du CLIENT
//     (`UIWindowMgr_DispatchHotkeyBehavior`), où RIEN n'est lié à Tab — le
//     ciblage clavier n'existe pas nativement (docs/target_system_re.md).
//
// Et quand la barre de chat a le focus, `HotkeyDispatch::OnKeyDown` s'efface de
// lui-même (`NativeTextInputHasFocus`) : Tab y retrouve son rôle de saisie. Il
// n'y a donc rien à confisquer, et la touche reste au client quand il en a
// l'usage.
//
// ⚠ Les autres touches de `kExtendedKeys` restent hors du jeu des actions : les
// flèches appartiennent au déplacement, Entrée et Espace au « bouton par défaut »
// des fenêtres (0x00A47317), Échap au menu et à l'annulation.
int CaptureActionVk();

// Libellé lisible du combo : « Ctrl+Maj+F1 », « Espace », « (aucun) ».
void Label(int vkey, bool ctrl, bool alt, bool shift, char* out, int cap);

// Le combo est-il déjà attribué ? Renvoie true et décrit son propriétaire dans
// `what` (au plus `cap` octets, toujours terminé).
bool Conflict(int vkey, bool ctrl, bool alt, bool shift, Owner self, int self_index,
              char* what, int cap);

// ── Le propriétaire d'un combo, DÉSIGNÉ et pas seulement nommé ───────────────
//
// `Conflict` ne rend qu'une phrase : de quoi refuser, pas de quoi agir. La table
// des raccourcis, elle, VOLE la touche à son détenteur (c'est ce que fait le
// natif) et a donc besoin de savoir à QUI la retirer. D'où cette forme longue,
// qui porte l'identité — assez pour aller délier la ligne concernée.
struct ConflictOwner {
  Owner owner = Owner::kNone;
  // Le `self_index` de ce propriétaire, dans le sens qu'impose son `owner` :
  // index de preset, slot de déplacement, index d'action, kZoneRecKey*… Pour une
  // commande du client c'est `ClientSelf(category, command_index)`, mais les deux
  // champs suivants la donnent déjà décomposée.
  int  index = -1;
  int  category = -1;       // kClientCommand
  int  command_index = -1;  // kClientCommand
  // Le champ `EXE` de la commande du client : c'est LUI qui identifie l'entrée
  // côté Lua, il faut le repasser tel quel pour la délier.
  char label[128] = {0};
  // Le libellé humain, déjà traduit — « le saut », « l'action « X » »…
  char what[128] = {0};
  // 🔴 Faux = combo RÉSERVÉ, qui n'appartient à aucune ligne remappable et ne se
  // vole donc pas (Alt+F, qui ouvre la fiche). L'appelant doit refuser.
  bool releasable = true;
};

// Tous les propriétaires du combo (le demandeur exclu), dans la limite de
// `max_out`. Renvoie leur nombre — 0 quand la touche est libre.
//
// ⚠ Il peut y en avoir PLUSIEURS : les catégories 0 et 3 sont deux pages de la
// même barre et ont le droit de partager une touche, et un état hérité peut très
// bien porter deux détenteurs. Voler à un seul en laisserait un debout.
int FindConflicts(int vkey, bool ctrl, bool alt, bool shift, Owner self, int self_index,
                  ConflictOwner* out, int max_out);

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

// ── La frappe est PRISE par une action de Bourgeon ───────────────────────────
//
// 🔴 CONFISQUER LA FRAPPE EST POSSIBLE, contrairement à ce qui a longtemps été
// écrit ici. `OnKeyDown` vient bien du jeu (notre hook de `ProcessPushButton`),
// mais ce hook DÉCIDE d'appeler ou non le handler natif : lui rendre `true`
// avale la touche. Le chemin manquant n'était que celui-ci — un drapeau posé par
// le dispatch, relevé par le hook juste après.
//
// À quoi ça sert : le contrôle de collision refuse à une action toute touche
// qu'une commande du CLIENT utilise, mais il ne voit que les commandes
// REMAPPABLES. Le client en a d'autres (les douze index que
// `UserHotkey_RowToCommandIndex` saute pour la catégorie Interface — la tipbox
// d'Alt+D en est), et rien n'empêchait alors une frappe de nourrir les deux
// mondes : l'action ET la fenêtre du jeu, ensemble, à chaque appui.
//
// `ClaimKey` se pose pendant `OnKeyDown` ; `TakeKeyClaim` le relève ET le remet à
// zéro — un seul lecteur, juste après la diffusion de la frappe.
void ClaimKey();
bool TakeKeyClaim();

}  // namespace hotkeys
