#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "features/plugin.h"

// Retouches client de la fenêtre de chat (client 20250716) : icônes d'objets
// natives sur les liens <ITEML>, horodatage des lignes, et une largeur de chat
// personnalisée (le chat d'origine ne se redimensionne qu'en hauteur).
//
// Le plugin porte les hooks moteur, le mécanisme d'application ET ses réglages.
// MoonlightUi n'en garde que la PERSISTANCE (il possède le fichier yaml) et
// l'endroit où le panneau s'affiche : les quatre réglages ci-dessous vivaient
// chez lui en double, un miroir à retenir synchronisé à la main.
class ChatTweaks : public Plugin {
 public:
  ChatTweaks();

  const char* name() const override { return "Chat"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // ── Réglages (publics : lus/écrits par la table de persistance de MoonlightUi) ──
  bool& custom_width()    { return custom_width_; }
  int&  custom_width_px() { return custom_width_px_; }
  bool& timestamps()      { return timestamps_; }
  bool& item_icons()      { return item_icons_; }

  // Borne la largeur puis pousse les quatre réglages vers le moteur. À appeler
  // après un chargement de configuration, et à chaque changement venu de l'UI.
  void ApplySettings();

  // Section « Chat » du panneau Moonlight, hors couleurs de fond. Renvoie true
  // si un réglage a changé.
  bool DrawSettings();

  // ── Couleurs de fond des fenêtres de chat ────────────────────────────────
  // Trois fonds indépendants, réglables en jeu. Chacun est porté par un ou
  // plusieurs immédiats ARGB dans le .text du client (0x66000000 = noir 40 %
  // par défaut), réécrits à chaud pour que les fenêtres à venir et les dessins
  // par frame prennent la nouvelle couleur. Les sites situés dans un
  // CONSTRUCTEUR rangent aussi la couleur dans l'objet : un parcours du tas
  // recolore alors les fenêtres déjà ouvertes.
  //
  //   Principal  : ctor de UINewChatWnd (obj+0xD8) + couleur d'onglet actif (Draw)
  //   Détachées  : bordure extérieure (Draw) + ctor de UISubChatHisWnd (obj+0xD4)
  //   Chuchotement : fond de la fenêtre 1:1 (Draw)
  enum BgGroupId { kBgMain = 0, kBgDetached, kBgWhisper, kBgCount };

  // Un préréglage de couleur nommé, partagé par les trois groupes.
  struct BgPreset {
    std::string name;
    uint32_t argb;
  };

  // Accès pour la PERSISTANCE (moonlight_ui/settings_containers.cc) et pour le
  // panneau. La clé yaml appartient au groupe, pas à l'appelant.
  const char* bg_yaml_key(int group) const;
  float*      bg_color(int group);          // état du color picker, RGBA 0..1
  bool        bg_available() const { return bg_found_; }
  std::vector<BgPreset>& bg_presets() { return bg_presets_; }

  // Écrit `argb` dans tous les immédiats du groupe (+ vidage du cache
  // d'instructions). `walk_heap` recolore en plus les objets déjà construits.
  void ApplyBackground(int group, uint32_t argb, bool walk_heap);
  // Pousse les trois couleurs courantes, objets vivants compris. Appelé après un
  // chargement de configuration.
  void ApplyAllBackgrounds();

  // Un groupe de la section « Couleurs du chat » : pastille, sélecteur,
  // préréglages. Renvoie true si la couleur ou la liste de préréglages a changé.
  // Les trois groupes sont dessinés séparément parce que MoonlightUi intercale
  // sa case « Barre de préréglages » à droite du premier.
  bool DrawBackgroundGroup(int group);
  // Barre flottante de préréglages : bascule la couleur du chat PRINCIPAL en un
  // clic, sans ouvrir le sélecteur. Rendue par MoonlightUi tant que son réglage
  // « mainchat_preset_bar » est actif. Renvoie true si la couleur a changé.
  bool DrawPresetBar();

 private:
  bool custom_width_    = false;
  int  custom_width_px_ = 800;   // borné 320..1200 par ApplySettings
  bool timestamps_      = false; // préfixe [HH:MM:SS] sur les nouvelles lignes
  bool item_icons_      = true;  // icônes natives sur les liens <ITEML>

  // Comment reconnaître, dans le tas, un objet déjà construit dont il faut
  // recolorer le champ de fond : sa vtable (adresse virtuelle) l'identifie, et
  // l'offset dit où est la couleur dans l'objet.
  struct BgHeapTarget {
    uint32_t vtable_va;
    uint32_t color_field_off;
  };

  // Un réglage de fond de chat, tel que l'utilisateur le voit.
  struct BgGroup {
    const char* label    = "";  // libellé du panneau
    const char* yaml_key = "";  // clé de persistance
    // Pointeurs ÉCRITURE vers les immédiats ARGB en .text : on patche du code
    // machine en place (VirtualProtect posé par FindBackgroundSites).
    std::vector<uint32_t*>    argb_imm_ptrs;
    std::vector<BgHeapTarget> heap_targets;  // objets déjà construits, à recolorer
    float picker_rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // état du color picker
    bool  picker_drag_in_progress = false;  // glissement du sélecteur en cours
  };

  BgGroup bg_[kBgCount];
  bool    bg_found_ = false;                    // au moins un site résolu
  std::vector<BgPreset> bg_presets_;
  char    preset_name_buf_[64] = {};

  // Scanne le .text UNE fois (constructeur) et résout, pour chaque groupe, ses
  // immédiats et ses cibles de tas, en amorçant le picker avec la couleur
  // actuellement dans le binaire.
  void FindBackgroundSites();
  // Un seul parcours du tas : recolore tous les objets vivants du groupe.
  void PatchBackgroundObjects(const BgGroup& group, uint32_t argb);
};

namespace chat {
// Set/clear a custom width for the main chat window (px clamped 320..1200).
// Applies live if the chat exists; otherwise the WndProc hook enforces it on the
// next in-game relayout. Passer par ChatTweaks::ApplySettings de préférence.
void SetCustomWidth(bool enabled, int px);

// Enable/disable a [HH:MM:SS] timestamp prefix on every new chat line.  Stored
// into the chat's raw history, so it persists across re-wraps/resizes.
void SetTimestamps(bool enabled);

// Enable/disable the native item icons on <ITEML> chat links.
void SetItemIcons(bool enabled);

// Clear the main chat window's history: empties every channel tab's raw-history
// vectors and re-wraps to a blank display.  Safe no-op if the chat isn't live.
void ClearHistory();
}  // namespace chat
