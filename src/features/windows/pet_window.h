#pragma once

#include <cstdint>

#include "features/plugin.h"
#include "ragnarok/pet.h"
#include "ui/mob_sprite.h"

// ── PetWindow ────────────────────────────────────────────────────────────────
//
// La fiche du pet, en ImGui. Elle remplace QUATRE fenêtres natives d'un coup :
// `UIPetInfoWnd` (id **88**), le menu de commandes qu'elle ouvrait (`UIMenuWnd`
// id **260**), la fenêtre d'évolution (`UIPetEvolutionWnd` id **261**, fondue
// dans un panneau de la fiche) et la liste d'éclosion (`UIPetEggListWnd`
// id **90**, fenêtre à part — voir plus bas). RE complète dans docs/pet_re.md.
//
// Fenêtre FLOTTANTE et autonome, comme la native : le pet n'est pas un onglet de
// la feuille de personnage. On le regarde en jouant, souvent pour une seule
// chose (la faim), et il faut pouvoir le laisser ouvert dans un coin.
//
// ── 🔴 DÉTRUIRE, jamais masquer ──────────────────────────────────────────────
// `GameMode_PetIntimacyWarnAndOpenInfo` (0x00C65A60) **rouvre la 88 de force**
// dès que l'intimité passe sous 100 — un remplaçant qui se contenterait de la
// masquer la verrait revenir toute seule, invisible mais vivante, et une native
// invisible garde le CLAVIER (cf. [[feedback_hidden_native_window_keyboard]]).
// Elle est donc masquée à la naissance puis détruite au tick suivant, et sa
// demande d'ouverture est routée ici — exactement le protocole de
// [[reference_native_window_toggle_router]].
//
// ── Rejouer, pas réécrire ────────────────────────────────────────────────────
// Aucun paquet n'est refabriqué : les cinq commandes passent par
// `CMode::SendMsg(150, cmd)`, le point d'entrée du client, qui porte lui-même le
// gate « pas de nourriture en sac » (msg 591). 🔴 Cet appel peut ouvrir une
// boîte native : il est empilé pendant le rendu et rejoué depuis
// `Bourgeon::OnProcessInput`, comme EntityContextMenu et WeaponRefineWindow
// (cf. [[feedback_no_native_cmd_during_imgui_frame]]).
//
// ── 🔴 Les gardes que la fenêtre native portait, et elle seule ───────────────
// Trois refus vivaient dans `UIPetInfoWnd::OnMsg`, pas dans `SendMsg` ni côté
// serveur. Les perdre ferait partir des demandes que le client d'origine
// n'émettait jamais (cf. [[feedback_replaced_handler_hidden_duties]]) :
//   · rédaction de courrier ouverte -> « nourrir » refusé (msg 2985) ;
//   · fenêtre 10011 présente -> « nourrir » refusé à la CONFIRMATION (msg 3550) ;
//   · évolution refusée sous 900 d'intimité (msg 2576).
// Elles sont reproduites telles quelles, y compris le fait qu'il y en ait DEUX
// pour nourrir, à deux instants différents du même geste.
//
// ── 🔴 Ce que le natif ne disait pas ─────────────────────────────────────────
// « Unequip Accessory » était proposé même sans accessoire (relevé live :
// accessoire = 0, entrée bien présente) : ici elle est grisée, avec la raison.
// L'évolution disparaissait sans un mot quand l'intimité était trop basse ou que
// le client ignorait l'œuf : ici la ligne reste, grisée, et dit pourquoi.
//
// OPT-IN : « pet_imgui », membre du groupe « Interface moderne » (défaut OFF).
// Coupé, les natives reprennent leur service à la prochaine demande — elles ne
// sont plus détruites, donc le client les recrée.

// ── La liste d'éclosion ─────────────────────────────────────────────────────
// Troisième native reprise ici : `UIPetEggListWnd` (id **90**), la petite liste
// que `@hatch` fait apparaître. Elle ne NAÎT PLUS — son unique créateur est le
// handler de `ZC_PETEGG_LIST` (0x01A6), dont on prend la place
// (`RegisterReplaceOpcode`, révocable par l'interrupteur). Rien d'autre ne vit
// dans ce handler : il crée la fenêtre et lui passe les index, un par un.
//
// 🔴 Ce que la native ne disait pas, et qui coûtait le geste entier : un œuf
// **vierge** (obtenu par `@item`) figure dans la liste — `clif_sendegg` ne
// filtre que sur le TYPE — mais `pet_select_egg` refuse de l'éclore, sans un
// mot. Le joueur cliquait, et il ne se passait rien. On sait maintenant les
// reconnaître (leurs quatre slots arrivent à zéro, cf. ragnarok/pet.h) : ils
// sont marqués, et le bouton refuse avec la raison.

class PetWindow : public Plugin {
 public:
  PetWindow();

  const char* name() const override { return "PetWindow"; }

  void OnTick() override;
  void OnRenderUI() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  // 🔴 Fil RÉSEAU : on COPIE, on ne décode pas (cf. features/net_inbox.h).
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Rejoue HORS frame ImGui la commande choisie (cf. l'en-tête). Appelée par
  // Bourgeon::OnProcessInput.
  void FlushPending();

  // Appelée par le hook de `MakeWindow` pour les ids 88 et 260 : masque la
  // native avant sa première frame et bascule notre fenêtre.
  void HandleNativeCreation(void* win, int window_id);

  void Open();
  bool IsOpen() const { return open_; }

  // Contenu de la section « Fiche de pet » du panneau Moonlight.
  // Rend true si un réglage a changé.
  bool DrawSettings();

  // « pet_imgui » : basculé en GROUPE par SetModernInterface. Défaut OFF.
  bool imgui_enabled_ = false;
  // « pet_confirm_egg » : demander confirmation avant « Remettre dans l'œuf ».
  // Le natif n'en demandait PAS — mais l'entrée est voisine de « Spectacle »
  // dans le même menu, et se tromper range le pet pour de bon. Défaut ON.
  bool& confirm_egg() { return confirm_egg_; }
  // « pet_animate » : animer le sprite du pet. Défaut ON.
  bool& animate() { return animate_; }

 private:
  // L'ITID de l'œuf porté, via l'index d'inventaire de `State::egg_index`.
  // 0 = inconnu — ce qui est le cas ORDINAIRE pet sorti (cf. ragnarok/pet.h).
  static int EggItid(const rag::pet::State& pet);

  void DrawHeader(const rag::pet::State& pet);
  void DrawGauges(const rag::pet::State& pet);
  void DrawActions(const rag::pet::State& pet);
  void DrawRenameRow(const rag::pet::State& pet);
  // Le panneau d'évolution, DANS la fenêtre : il remplace `UIPetEvolutionWnd`
  // (id 261), qui était une fenêtre à part alors qu'elle ne parlait que du pet
  // qu'on a sous les yeux. Sprites de mob de part et d'autre d'une flèche,
  // matériaux avec ce qu'on possède, et le refus DIT plutôt que subi.
  void DrawEvolutionPanel(const rag::pet::State& pet, int src_egg);
  // La liste d'éclosion, fenêtre à part : elle n'a rien à voir avec la fiche
  // (elle apparaît justement quand on n'a PAS de pet) et les deux ne sont jamais
  // à l'écran en même temps dans le cours normal des choses.
  void DrawHatchWindow();

  // Combien d'exemplaires de cet objet dans le sac. Le serveur additionne TOUTES
  // les piles (`pet_evolution_requirements_check`), on fait pareil.
  static int InventoryCount(uint32_t itid);

  // Ce qui est en attente de rejeu hors frame.
  enum class Pending {
    kNone,
    kCommand,     // pending_arg_ = la commande CZ_COMMAND_PET
    kRename,      // pending_name_
    kEvolution,   // pending_arg_ = œuf CIBLE (la source est relue au moment du rejeu)
    kSelectEgg,   // pending_arg_ = index d'INVENTAIRE de l'œuf à faire éclore
  };

  bool open_       = false;
  bool need_pos_   = true;
  bool need_focus_ = false;

  // Confirmations, en ImGui plutôt qu'en boîte native : la native est BLOQUANTE
  // et relance le tick du mode sous notre frame.
  bool confirm_feed_ = false;
  bool confirm_egg_ask_ = false;

  // Renommage. 🔴 Le tampon est LARGE exprès, et la borne du client n'est pas
  // imposée par la saisie : elle porte sur la forme RÉSEAU (le msg 6/184 teste
  // `strlen(texte) >= 24` sur ce que le champ natif contient, donc du CP949), et
  // un accent y pèse un octet là où il en pèse deux en UTF-8. Tronquer la saisie
  // à 24 octets d'UTF-8 refuserait des noms que le client accepte — et pourrait
  // couper une séquence multi-octets en deux. On laisse taper, et on refuse avec
  // le message EXACT du client (msg 682) après conversion.
  bool rename_edit_ = false;
  char rename_buf_[64] = {};
  // Retour de la dernière action, affiché sous les boutons. `status_error_`
  // décide de la couleur : un refus du client en gris se lit comme une simple
  // information, alors que c'est la raison pour laquelle rien n'est parti.
  char status_[192] = {};
  bool status_error_ = false;

  // Ticks consécutifs SANS acteur de pet, hors chargement de map. Le client
  // n'émet aucun « le pet est parti » exploitable (cf. OnTick) : la disparition
  // de l'acteur est le seul signal, et il faut la confirmer dans le temps.
  int pet_gone_ticks_ = 0;

  // Le pet pour lequel l'ouverture FORCÉE de basse intimité a déjà eu lieu.
  // Réplique de la table des avertis du client (cf. HandleNativeCreation) ;
  // remise à 0 dès que l'intimité repasse au-dessus de 99, comme lui.
  uint32_t low_intimacy_warned_aid_ = 0;

  Pending  pending_      = Pending::kNone;
  int      pending_arg_  = 0;
  char     pending_name_[rag::pet::kNameMaxBytes] = {};

  // Sprite du pet, rechargé quand la classe change.
  ro::MobSpriteRes sprite_;
  // Les deux sprites du panneau d'évolution. Le natif y mettait deux
  // ILLUSTRATIONS `.bmp` (`illust\`), une par espèce : elles manquent pour la
  // plupart des pets et retombaient sur `pet_noimage.bmp`. Le sprite existe
  // toujours, lui, et c'est la créature telle qu'on la voit dans le monde.
  ro::MobSpriteRes evo_src_sprite_;
  ro::MobSpriteRes evo_dst_sprite_;

  // Évolution dépliée : l'ITID de l'œuf CIBLE choisi, 0 = panneau replié.
  int evo_target_ = 0;

  // ── La liste d'éclosion (remplace UIPetEggListWnd, id 90) ─────────────────
  // Plafond = `MAX_INVENTORY` côté serveur, la borne de la boucle de
  // `clif_sendegg` : le paquet ne peut pas en annoncer davantage.
  static constexpr int kMaxEggs = 100;
  bool hatch_open_     = false;
  bool hatch_need_pos_ = true;
  // L'index d'inventaire choisi, PAS la ligne : c'est lui qui part sur le fil,
  // et une liste réémise pendant que la fenêtre est ouverte ne doit pas déplacer
  // silencieusement la sélection. -1 = aucune, comme le `+0xBC` du natif.
  int  hatch_sel_   = -1;
  int  hatch_count_ = 0;
  int  hatch_index_[kMaxEggs] = {};
  bool hatch_focus_ = false;

  bool confirm_egg_ = true;
  bool animate_     = true;
};
