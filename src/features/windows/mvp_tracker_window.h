#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "features/link_gesture.h"  // links::MenuAnchor (l'ancre appartient à l'appelant)
#include "features/plugin.h"
#include "ui/mob_sprite.h"          // ro::MobSpriteRes

namespace mvp {
struct Slot;
struct Obs;
}  // namespace mvp

// ── MvpTrackerWindow ─────────────────────────────────────────────────────────
//
// Le carnet de chasse MVP : une table triable des créneaux, le groupe qui les
// partage, et une alerte sur les favoris. L'état vient de MvpTracker (systems/),
// qui parle au serveur ; cette classe ne fait que dessiner et commander.
//
// ── Ce qu'elle affiche, et ce qu'elle refuse d'afficher ─────────────────────
// Une observation MÉRITÉE (Convex Mirror) donne un instant : un compte à rebours.
// Tout le reste donne une FENÊTRE, déduite des délais publics du script de spawn.
// Un créneau que personne du groupe n'a observé n'affiche RIEN — alors même que
// le serveur en connaît le tirage à la milliseconde. C'est la règle de
// non-triche, et elle se voit à l'écran.
//
// L'ÂGE de l'information est une COLONNE, pas une infobulle : « tué il y a 3 h »
// et « signalé il y a 3 h » ne veulent pas dire la même chose, et c'est
// précisément ce qu'il faut pour juger une ligne d'un coup d'œil.
//
// Le compte à rebours est DÉRIVÉ à chaque frame de l'heure serveur (décalage
// relevé sur les paquets), jamais un compteur entretenu.
//
// ⚠ Cette fenêtre ne lit RIEN de la mémoire du client : pas de garde de
// timestamp de build à poser. Seule la couche de minimap en lit.

// ── « Inviter dans mon carnet MVP », partout où l'on clique un joueur ────────
//
// UNE seule implémentation, appelée par les TROIS menus joueur du projet : les
// pseudos du chat (`links::DrawMenu`), le clic droit sur une entité du monde
// (`EntityContextMenu`) et les listes de la feuille de personnage. Ils n'ont ni
// la même architecture ni le même code d'entrée, mais ils doivent proposer la
// même chose, grisée pour les mêmes raisons et avec les mêmes mots.
//
// `name_utf8` est le pseudo tel qu'on l'affiche ; la conversion vers l'encodage
// du fil appartient au producteur du paquet (MvpTracker::Send).
//
// Dessine l'entrée, grisée avec sa raison quand l'invitation est impossible.
void DrawMvpInviteMenuItem(const char* name_utf8);

class MvpTrackerWindow : public Plugin {
 public:
  MvpTrackerWindow();

  const char* name() const override { return "MvpTrackerWindow"; }

  void OnRenderUI() override;
  void OnTick() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  bool IsOpen() const { return open_; }
  void Open();
  void Toggle();

  // ── Une LIGNE DÉTACHÉE ─────────────────────────────────────────────────────
  //
  // Un créneau sorti du carnet par glisser-déposer : il vit seul à l'écran et
  // survit à la fermeture du carnet, ce qui est tout son propos.
  //
  // 🔴 La clé PERSISTÉE est `(mob_id, map)`, jamais `slot_id`. Celui-ci vaut le
  // rang dans un registre reconstruit à chaque démarrage du serveur : ajoutez un
  // `boss_monster` et toutes les lignes épinglées désigneraient le voisin. Même
  // raison, et même clé, que les favoris en base.
  struct PinnedLine {
    uint16_t mob_id = 0;
    char     map[16] = {};
    int      x = 0, y = 0;
    // Résolu à l'exécution depuis le catalogue. JAMAIS écrit dans le yaml.
    uint16_t slot_id = 0xFFFF;
    // Taille MESURÉE au dernier dessin, en pixels. Runtime, jamais persistée.
    // 🔴 Sans elle, l'aimantage collait une ligne à `voisine.y + MA hauteur` au
    // lieu de `voisine.y + SA hauteur` — d'où un trou (ou un chevauchement) dès
    // que deux lignes n'avaient pas la même taille, ce qui est la règle : le
    // texte varie, et le sprite peut être là ou pas.
    float measured_w = 0.0f;
    float measured_h = 0.0f;
  };

  // Public : c'est settings_containers.cc qui la lit et l'écrit, comme les
  // mémos de la minimap et la disposition de l'inventaire.
  std::vector<PinnedLine> lines_;

  // Section « Carnet de chasse MVP » du panneau Moonlight. Rend true quand un
  // réglage a changé, comme les autres DrawSettings du projet.
  bool DrawSettings();

 private:
  // L'invitation reçue, en MODALE — et hors du carnet, qui peut être fermé.
  void DrawInvitePopup();
  void DrawGroupPanel();
  void DrawTable();
  void DrawAlerts();
  void DrawManualPopup();
  // Le sprite du monstre dans son rang, à hauteur de ligne.
  void DrawRowSprite(uint16_t mob_id, int size_px, bool animate);
  // Le bouton favori : art si les bitmaps sont là, petit bouton texte sinon.
  // Rend true au clic.
  bool DrawFavoriteButton(bool favorite);
  // Le créneau passe-t-il le filtre ? Vrai quand le champ est éteint ou vide.
  bool MatchesFilter(const mvp::Slot& slot) const;
  // La carte en infobulle, avec la tombe pointée dessus.
  void DrawMapPreview(const mvp::Slot& slot, const mvp::Obs* obs);
  // Le titre de la fenêtre REPLIÉE : la prochaine échéance connue. Garde
  // l'identifiant après « ### » intact — c'est lui qui porte position, taille
  // et épingle.
  void BuildCollapsedTitle(char* out, size_t cap);
  // Les lignes détachées, chacune dans sa propre petite fenêtre.
  void DrawPinnedLines();
  // Termine un glissement parti d'une poignée : dessine le fantôme, et détache
  // au lâcher si la souris est SORTIE du carnet.
  void FinishRowDrag();
  // Aimantage d'une ligne sur les bords des AUTRES lignes, un axe à la fois.
  // Même patron que MenuIcons::SnapIcon : quatre candidats par voisine — aligner
  // les débuts, aligner les fins, se poser juste après, se poser juste avant.
  float SnapLine(float v, float ext, int self, bool y_axis) const;
  // Détache un créneau à l'endroit du lâcher. Sans effet s'il est déjà détaché :
  // deux lignes pour un même créneau diraient deux fois la même chose.
  void PinSlot(const mvp::Slot& slot, ImVec2 at);
  // Retrouve le `slot_id` de chaque ligne depuis sa clé stable. À rejouer quand
  // le catalogue change — un @reloadscript le renumérote.
  void ResolvePinnedSlots();
  // Le texte d'une ligne : nom et échéance, la même règle que la table.
  void FormatLineText(const mvp::Slot& slot, char* out, size_t cap);

  // Analyse souple d'une heure saisie : « 1430 », « 14h30 », « 14:30 », et un
  // « - » de tête pour la veille (« -2350 »). Rend 0 si illisible.
  int64_t ParseKillTime(const char* text) const;

  bool open_ = false;

  // Saisies. Buffers UTF-8 : la conversion vers l'encodage du fil appartient au
  // producteur, donc à MvpTracker::Send.
  char group_name_buf_[64] = {};
  char invite_buf_[32] = {};
  char manual_time_buf_[16] = {};
  // Filtre de la table. NON persisté, et volontairement : on filtre pour trouver
  // quelque chose maintenant, pas pour rouvrir demain sur une table amputée sans
  // se rappeler pourquoi.
  char filter_buf_[32] = {};

  uint16_t manual_slot_ = 0xFFFF;   // créneau visé par la saisie manuelle
  bool     open_manual_popup_ = false;
  // L'invitation pour laquelle la modale a DÉJÀ été ouverte : sans ça, réémettre
  // OpenPopup à chaque frame empêcherait de la fermer.
  uint32_t invite_shown_ = 0;
  bool     confirm_leave_ = false;

  // Alertes déjà déclenchées, pour ne sonner qu'une fois par observation.
  // Clé = slot_id, valeur = `reported_at` de l'observation qui a sonné.
  static constexpr int kMaxAlerts = 32;
  struct FiredAlert { uint16_t slot_id; int64_t reported_at; };
  FiredAlert fired_[kMaxAlerts] = {};
  int fired_count_ = 0;

  // Bandeau d'alerte courant.
  uint16_t alert_slot_ = 0xFFFF;
  unsigned alert_ms_   = 0;

  bool snapshot_requested_ = false;
  // Le pli de la frame PRÉCÉDENTE : ImGui ne le dit qu'après Begin, alors que le
  // titre se donne avant.
  bool was_collapsed_ = false;

  // ── Le glisser-déposer qui DÉTACHE ────────────────────────────────────────
  // Le créneau saisi par sa poignée, et le rectangle du carnet au moment du
  // lâcher : on ne détache que si la souris en est SORTIE, sinon un glissement
  // maladroit à l'intérieur de la table poserait une ligne sous la fenêtre.
  uint16_t drag_slot_   = 0xFFFF;
  ImVec2   win_pos_     = ImVec2(0.0f, 0.0f);
  ImVec2   win_size_    = ImVec2(0.0f, 0.0f);
  // La ligne détachée en cours de déplacement, et la prise de la souris.
  int      line_drag_   = -1;
  ImVec2   line_drag_off_ = ImVec2(0.0f, 0.0f);
  // Le catalogue vu la dernière fois qu'on a résolu les `slot_id`.
  size_t   resolved_for_ = 0;

  // 🔴 L'ancre du menu de liens appartient à CETTE fenêtre, jamais au module :
  // deux surfaces visibles en même temps arment chacune la sienne, et une ancre
  // partagée ferait ouvrir par l'une le menu armé par l'autre.
  links::MenuAnchor menu_;
  // 🔴🔴 UNE POIGNÉE PAR CLASSE, jamais une seule partagée.
  //
  // `LoadMobSprite` ne tient PAS un cache global : son cache est la poignée
  // qu'on lui passe, et il ne retient qu'UN id (`if (res->class_id == class_id)
  // return;`). Avec une poignée unique réutilisée d'un rang à l'autre, chaque
  // ligne d'un mob différent relit et reparse son `.spr` — quatre-vingts fois
  // par frame. C'est ce qui gelait le jeu, la rotation des sprites du Bio Lab
  // n'ayant fait que le rendre visible.
  std::unordered_map<uint16_t, ro::MobSpriteRes> sprites_;
  // Chargements de sprites autorisés dans la frame — cf. le pavé de
  // DrawRowSprite. Remis à zéro en tête d'OnRenderUI.
  static constexpr int kMaxSpriteLoadsPerFrame = 2;
  int sprite_loads_this_frame_ = 0;
  // 🔬 Horodatage du dernier relevé d'occupation du cache (diagnostic).
};
