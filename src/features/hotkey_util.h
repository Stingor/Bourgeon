#pragma once

#include "imgui.h"

// ── hotkey_util : les raccourcis clavier de Bourgeon, décrits au même endroit ─
// Deux fonctionnalités laissent le joueur choisir sa touche : les presets
// d'équipement de la fiche de personnage et le saut. Elles partagent ici la
// capture du combo, son libellé et — le point qui compte — le CONTRÔLE DE
// CONFLIT : un combo déjà pris par un autre preset, par le saut ou par un
// raccourci natif de la barre de skills est refusé en nommant son propriétaire.
// Sans ce contrôle la même touche déclencherait deux actions à la fois, sans
// que rien ne l'explique au joueur.
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
  kZoneRecorder,  // touche d'enregistrement de zone (ZoneRecorder, staff)
};

int      ImGuiKeyToVk(ImGuiKey key);
ImGuiKey VkToImGuiKey(int vkey);

// Première touche PRINCIPALE pressée cette frame (modificateurs exclus), en VK
// Windows ; 0 si aucune. À appeler pendant le rendu ImGui.
int CaptureMainVk();

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
