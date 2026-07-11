#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "plugins/plugin.h"

// Un item d'un preset d'equipement : identite suffisante pour le RETROUVER en
// inventaire lors de l'application (nameid + refine + cartes + grade). leftHand =
// arme equipee en main gauche (dual-wield, slot bouclier).
struct EquipPresetItem {
  uint32_t nameid = 0;
  int      refine = 0;
  uint32_t cards[4] = {0, 0, 0, 0};
  int      grade = 0;
  bool     leftHand = false;
};
// Un preset = un jeu d'equipement nomme, propre a UN personnage (cid = g_Own_CharId).
struct EquipPreset {
  uint32_t                     cid = 0;
  std::string                  name;
  std::vector<EquipPresetItem> items;
  // Raccourci clavier optionnel (VK Windows + modificateurs). hotkeyVk==0 => aucun.
  int  hotkeyVk = 0;
  bool hkCtrl = false, hkAlt = false, hkShift = false;
};

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

  // Presets d'equipement (loadouts nommes), persistes par MoonlightUi dans le yaml
  // (noeud "equip_presets"). Tous personnages confondus ; filtres par cid a l'affichage.
  std::vector<EquipPreset>& equip_presets() { return equip_presets_; }

 private:
  std::vector<EquipPreset> equip_presets_;
  char preset_name_buf_[24] = {};  // saisie du nom (sauvegarde / renommage)
  std::string preset_status_;      // retour UI (ex. « Applique », « 2 item(s) manquant(s) »)
  int  hk_capturing_ = -1;         // index (dans equip_presets_) du preset en capture de touche
  std::string hk_conflict_msg_;    // message de conflit affiché pendant la capture
  bool show_    = true;   // fenetre visible (bascule Alt+F)
  int  tab_     = 0;      // onglet actif : 0=Equipement, 1=Costume, 2=Presets
  bool costume_ = false;  // == (tab_==1) ; garde pour DrawDoll/DrawSlot
  bool need_pos_ = true;  // 1er placement de la fenetre
  float chrome_w_ = 20.0f;  // largeur du chrome (fenetre - contenu), mesuree/frame
  // Pose de l'avatar (selecteur sous l'avatar).
  int  avatar_anim_ = 4;      // animType : 0=Repos..4=Combat..8=Mort (def Combat)
  int  avatar_dir_ = 0;       // direction 0..7 (0=face)
  bool avatar_animate_ = true;
  bool avatar_show_costume_ = true;  // costumes affichés (Costume tab, ou Équip + config on)
  std::string gif_status_;    // retour UI du dernier export GIF (nom / erreur)

  // Dialogue « Enregistrer sous » du GIF : ouvert sur un THREAD séparé (ne pas
  // bloquer le rendu/réseau du jeu), résultat consommé par le thread principal.
  std::atomic<bool> gif_dialog_busy_{false};   // un dialogue est en cours
  std::atomic<bool> gif_dialog_ready_{false};  // le dialogue a rendu un résultat
  std::string gif_dialog_path_;                // chemin choisi (vide = annulé)
  int gif_export_anim_ = 4, gif_export_dir_ = 0;  // pose/dir figées au clic
  bool gif_export_show_costume_ = true;           // état costume figé au clic (GIF)

  void DrawStatsPanel();
  void DrawDoll(float avail_w);
  // Onglet Presets : liste des presets du perso (icones des items) + sauvegarde.
  void DrawPresetsTab();
  // Sauve l'equipement porte actuellement comme preset nomme (perso courant).
  void SaveCurrentEquipAsPreset(const char* name);
  // Applique un preset : desequipe les slots hors preset, equipe les items manquants.
  void ApplyPreset(const EquipPreset& p);
  // Declenche l'application d'un preset dont le raccourci clavier est pressé (en jeu, hors
  // saisie texte). Actif même fenêtre fermée.
  void ProcessPresetHotkeys();
  // Conflit du combo (vk+mods) avec un AUTRE preset (self exclu) ou un raccourci natif de la
  // barre de skills/items. Renvoie true + décrit le conflit dans `what`.
  bool HotkeyConflict(int vk, bool ctrl, bool alt, bool shift, int selfIdx, char* what, int cap);
  // Dessine un slot d'equipement a (x,y) taille sz dans le draw courant.
  void DrawSlot(int slot, bool costume, float x, float y, float sz);
  // Ouvre le dialogue "Enregistrer sous" du GIF (thread séparé, non bloquant).
  void RequestGifSave();
};
