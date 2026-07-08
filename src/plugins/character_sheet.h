#pragma once

#include "plugins/plugin.h"

// Feuille de personnage facon WoW : un avatar central entoure des slots
// d'equipement (+ costume), avec un volet stats a droite. AGREGE les fenetres
// natives Status (id 0xb) + Equipement (id 0xa), qui restent CONSERVEES : ceci est
// un COMPLEMENT opt-in (setting charsheet_imgui, defaut OFF), ouvert via Alt+F.
//
// Interactif : survol slot = tooltip (nom+refine), clic-gauche slot = fenetre de
// description native, clic-droit slot = DESEQUIPER (CZ 0x00AB), boutons +STR/+AGI/...
// (CZ 0x00BB, actifs seulement si le cout de montee <= points de statut restants).
// Onglet Equipement / Costume pour les deux jeux de slots.
//
// Donnees lues LIVE des globals session (cf. project_character_sheet). L'avatar
// central reutilise le moteur de capture-sprite de basic_info (Phase 2 ; placeholder
// pour l'instant). Skin RO via ro_imgui.
class CharacterSheet : public Plugin {
 public:
  CharacterSheet();

  const char* name() const override { return "CharacterSheet"; }

  void OnRenderUI() override;

  // Setting PERSISTANT (bourgeon_settings.yaml "charsheet_imgui", gere par
  // MoonlightUi). Defaut OFF : opt-in. Quand ON, Alt+F bascule la fenetre.
  bool imgui_enabled_ = false;

 private:
  bool show_    = true;   // fenetre visible (bascule Alt+F)
  bool costume_ = false;  // onglet costume actif (vs equipement)
  bool need_pos_ = true;  // 1er placement de la fenetre

  void DrawStatsPanel();
  void DrawDoll(float avail_w);
  // Dessine un slot d'equipement a (x,y) taille sz dans le draw courant.
  void DrawSlot(int slot, bool costume, float x, float y, float sz);
};
