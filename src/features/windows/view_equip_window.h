#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "features/item_cell.h"     // itemcell::ChatLink
#include "features/link_gesture.h"   // links::Target / Hit / DrawMenu
#include "features/plugin.h"

// ── ViewEquipWindow ──────────────────────────────────────────────────────────
//
// « Voir l'équipement » d'un autre joueur. Remplace la fenêtre native id **139**
// (`0x8B`) — RE complète dans `docs/view_equip_re.md`.
//
// ── Ce qu'elle remplace, et par quel bout ────────────────────────────────────
// La native n'est pas une fenêtre à elle : c'est la classe `UIEquipWnd` (celle
// de NOTRE équipement) instanciée avec le drapeau `+0xB4 = 1`. Elle n'a qu'UN
// seul créateur — la dernière ligne du handler de `ZC_EQUIPWIN_MICROSCOPE
// 0x0B37` — donc on n'a pas à la détruire au tick comme les autres : on
// REVENDIQUE le paquet, et elle ne naît jamais. C'est le même schéma que
// MonsterInfoWindow avec `ZC_MONSTER_INFO`.
//
// 🔴 Aucun devoir natif à rejouer, et ce n'est pas une supposition (docs §9.1) :
// le handler ne fait que remplir deux tableaux de session (`+0x2180`, `+0x34E0`)
// que SEULE la fenêtre native lit, écrire des globales d'apparence que seul son
// pantin lit, et appeler un rafraîchissement de fenêtres d'objets — or ce paquet
// ne change RIEN de ce que le joueur possède. Sa branche « œuf de pet » est
// morte (double filtre serveur : `equip_index[]` + `itemdb_isequip2`).
//
// ── Pourquoi décoder le paquet plutôt que lire le natif ──────────────────────
// Les tableaux natifs perdent le grade, le détail des options aléatoires et les
// drapeaux, et ne gardent qu'UNE cible. Le fil, lui, porte tout. Chaque pièce
// devient donc un `itemcell::ChatLink` — la structure que le projet emploie déjà
// pour « un objet qui n'est pas à nous » — et tout le reste (nom composé avec
// refine/cartes/« [N] », infobulle, description complète, lien de chat) est
// alors du code DÉJÀ écrit et déjà éprouvé.
//
// ── Ce qu'elle apporte que la native ne pouvait pas ──────────────────────────
//  · elle RESTE ouverte et se rafraîchit quand on inspecte quelqu'un d'autre
//    (la native, elle, se ferme : son couple Close/MakeWindow est un basculeur) ;
//  · cartes, options, grade, raffinement, objet cassé, lisibles d'un coup —
//    la native tronque le nom à 70 px ;
//  · lien de chat et description, que la native INTERDIT sur l'équipement
//    d'autrui (`if (!this[45])` dans son gestionnaire de glisser) ;
//  · les emplacements VIDES sont montrés : « il n'a pas de cape » est une
//    information, et la native n'en disait rien.
//
// OPT-IN : membre du groupe « Interface moderne ». Coupée, le paquet repart au
// handler natif et la fenêtre d'origine réapparaît, intacte.
class ViewEquipWindow : public Plugin {
 public:
  ViewEquipWindow();

  const char* name() const override { return "ViewEquipWindow"; }

  void OnRenderUI() override;
  void OnTick() override;
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;

  // L'AID de la cible que le joueur vient de demander.
  //
  // 🔴 Il n'est PAS dans la réponse : `ZC_EQUIPWIN_MICROSCOPE` ne porte que le
  // NOM. Sans cet appel, le bouton « Actualiser » n'aurait personne à qui
  // redemander, et deux joueurs homonymes seraient indiscernables. C'est
  // `EntityContextMenu` qui le sait — il l'appelle juste avant de rejouer le
  // code natif 42.
  //
  // ⚠ Une demande peut rester sans réponse (cible partie, autre map, refus) :
  // l'AID n'est promu « cible affichée » qu'à la RÉCEPTION.
  void NotePendingTarget(uint32_t aid);

  bool IsOpen() const { return open_; }

  // « viewequip_imgui » : basculé en GROUPE par SetModernInterface. Défaut OFF.
  bool imgui_enabled_ = false;

  // « viewequip_show_empty » : montrer les emplacements que la cible ne porte
  // pas. Défaut ON — c'est un apport sur la native, qui n'en disait rien.
  bool& show_empty() { return show_empty_; }

  // « viewequip_compare » : afficher en regard MA pièce du même emplacement.
  // Défaut OFF — la fenêtre s'élargit quand c'est actif, et on ne compare pas à
  // chaque fois qu'on regarde quelqu'un.
  bool& compare() { return compare_; }

 private:
  // Une pièce portée, telle que le fil la décrit.
  struct Piece {
    itemcell::ChatLink link;      // id, refine, grade, cartes, options, cassé
    uint32_t wear_state = 0;      // masque EQP_* où elle est PORTÉE
    int      slot       = -1;     // 0..9 (cf. docs §6.1), -1 = hors grille
    bool     costume    = false;
    bool     shadow     = false;  // emplacement d'ombre (inutilisé sur moonlight)
    bool     ammo       = false;
    bool     two_handed = false;  // occupe arme ET bouclier
    std::string label;            // nom composé, UTF-8, « [N] » compris
  };

  // Le joueur inspecté. Tout est COPIÉ : les globales du client n'ont de place
  // que pour une cible et sont écrasées à l'inspection suivante.
  struct Target {
    uint32_t aid = 0;             // 0 = inconnu (réponse non sollicitée par nous)
    std::string name;             // UTF-8
    int job = 0, sex = 0, hair = 0;
    int head_low = 0, head_mid = 0, head_top = 0, robe = 0;
    int hair_color = 0, clothes_color = 0, body2 = 0;
    std::vector<Piece> pieces;
    double received_at = 0.0;     // ImGui::GetTime() à la réception
  };

  void DrawDollPanel(float width, float height);
  void DrawPieceList();
  void DrawRow(const Piece& piece, int slot, bool costume_section);
  // Une cellule « icône + nom », gestes compris. Le même rendu pour la pièce de
  // la cible et pour la mienne : deux colonnes composées autrement se
  // compareraient mal.
  //
  // 🔴 `id` n'est pas décoratif : les deux colonnes dessinent la MÊME structure
  // au même endroit de la pile d'ids, et sans un identifiant distinct ImGui
  // signale « 2 visible items with conflicting ID » — puis les deux cellules se
  // partagent le même popup.
  void DrawItemCell(const itemcell::ChatLink& link, const std::string& label,
                    int id);
  // MA pièce à cet emplacement, prête à être rendue comme celle de la cible.
  bool MyPiece(int slot, bool costume, itemcell::ChatLink* out,
               std::string* label) const;
  // Redemande l'équipement de la cible affichée (CZ 0x02D6).
  void RequestRefresh();

  // La pièce occupant `slot` dans la section demandée, ou nullptr.
  const Piece* Find(int slot, bool costume) const;

  Target   target_;
  bool     open_        = false;
  bool     need_focus_  = false;
  uint32_t pending_aid_ = 0;   // demande en vol, promue à la réception

  // Vue
  // 🔴 0 = DE FACE (`ro::DollDrawOpts::dir`, et `avatar_dir_` de la fiche de
  // personnage vaut 0 pour la même raison). 4, c'est le DOS — la fiche s'ouvrait
  // sur un personnage vu de derrière.
  int  doll_dir_   = 0;
  bool show_empty_ = true;  // montrer les emplacements vides

  bool compare_ = false;  // colonne « la mienne » (élargit la fenêtre)

  // Ce qui est survolé cette frame, et ce sur quoi le menu vient d'être demandé.
  //
  // 🔴 L'aperçu CRÉE SON PROPRE POPUP : il doit être dessiné HORS de toute
  // fenêtre ImGui, donc après EndRoWindow — on ne peut pas le dessiner là où on
  // le déclenche, d'où ce relais.
  //
  // 🔴 Le menu, lui, s'ouvre HORS de la pile d'ids où le clic a eu lieu :
  // l'identifiant d'un popup se hache avec cette pile, et un `OpenPopup` appelé
  // sous le `PushID` d'une cellule donnerait un id que le `BeginPopup` d'après
  // ne retrouverait jamais. Le clic se contente donc de LEVER LE DRAPEAU.
  links::Target     hover_target_;
  bool              hover_valid_ = false;
  links::MenuAnchor menu_;

  // L'aperçu d'objet, dessiné hors fenêtre depuis OnRenderUI.
  void DrawHoverPreview();

  // Demande d'actualisation armée par le bouton, jouée par OnTick.
  //
  // 🔴 Pas depuis la frame ImGui : un envoi déclenché en plein rendu part sur le
  // fil principal au milieu d'un état que le client n'attend pas là, et c'est la
  // règle du projet pour tout ce qui sort vers le natif ou le réseau depuis une
  // interface (cf. itemcell::FlushDeferredDesc, EntityContextMenu::FlushPending).
  bool refresh_requested_ = false;
};
