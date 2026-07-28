#pragma once

#include <cstdint>

#include "plugins/plugin.h"

// ── BankTweaks ───────────────────────────────────────────────────────────────
//
// Banque de zeny (Ctrl+B) en ImGui, skin RO, en REMPLACEMENT de la fenêtre native :
// celle-ci est masquée (hors rendu ET hors hit-test) tant que le viewer est actif.
// Elle continue de recevoir les paquets, donc rien n'est perdu — elle ne se voit
// simplement plus.
//
// Membre du groupe « Interface moderne » (SetModernInterface, moonlight_ui.h) :
// PLUS de case isolée dans le panneau. La banque échange des zeny avec la poche,
// dont le montant est affiché par l'inventaire moderne — et c'est le bouton « sac
// de zeny » du footer de cet inventaire qui l'ouvre. Une banque moderne au-dessus
// d'un inventaire natif (ou l'inverse) recréerait exactement le mixe qu'on a
// supprimé ailleurs.
//
// RE complète : docs/bank_zeny_re.md (mémoire project_bank_zeny_re).
// Rappels indispensables :
//   - UIBank_NewWnd : windowID 275 (0x113), vtable 0x01030fd4, 280×153.
//   - g_BankVault = 0x015fffc0 (s64) ; g_PlayerZeny = 0x015fba90 (s32).
//   - Le client N'OUVRE JAMAIS la banque lui-même : Ctrl+B envoie
//     CZ_REQ_BANKING_CHECK 0x09AB et c'est la réception de ZC_BANKING_CHECK
//     0x09A6 qui appelle MakeWindow(0x113). On se calque donc sur la présence de
//     la fenêtre native — pas de bouton « ouvrir » de notre côté.
//   - ⚠ 0x09A6 BASCULE la fenêtre : renvoyer 0x09AB alors que la banque est
//     ouverte la FERME. D'où l'absence de bouton « Rafraîchir » ici.
//
// ACTIONS : paquets bruts, identiques à ceux du bouton natif (RE UIBankWnd_OnMsg
// 0x00881b50) — le serveur (moonlight pc_bank_deposit / pc_bank_withdraw) valide
// tout, aucun exploit possible :
//   CZ_REQ_BANKING_DEPOSIT  0x09A7 { u16 op; u32 AID; u32 montant }  len 10
//   CZ_REQ_BANKING_WITHDRAW 0x09A9 { u16 op; u32 AID; u32 montant }  len 10

class BankTweaks : public Plugin {
 public:
  const char* name() const override { return "BankTweaks"; }

  void OnTick() override;      // suit la fenêtre native + relit les soldes
  void OnRenderUI() override;  // dessine la fenêtre ImGui si la banque est ouverte

  // Contenu de la section « Banque » du panneau Moonlight. Rend true si un
  // réglage a changé (l'appelant sauvegarde une seule fois).
  bool DrawSettings();

  // Ouvre — ou referme — la banque, exactement comme Ctrl+B. C'est le chemin du
  // behavior 146 natif, reproduit à l'identique : si la fenêtre 275 est ouverte on
  // la ferme côté client, sinon on DEMANDE au serveur de l'ouvrir
  // (CZ_REQ_BANKING_CHECK 0x09AB) — le client n'ouvre jamais la banque lui-même,
  // et un MakeWindow(275) direct afficherait un solde périmé.
  // Public : appelé par le bouton « sac de zeny » du footer de l'InventoryViewer.
  void ToggleFromUi();

  // ── Settings PERSISTANTS (bourgeon_settings.yaml, chargés/sauvés par MoonlightUi)
  // « bank_imgui » : fenêtre ImGui + native masquée. Basculé en GROUPE par
  // SetModernInterface, jamais isolément. Défaut OFF, comme tout le groupe.
  bool imgui_enabled_ = false;
  // « bank_quick_amounts » : rangée de boutons d'incrément rapide.
  bool& quick_amounts() { return quick_amounts_; }
  // « bank_show_total » : ligne « Total » (banque + poche) et jauge de plafond.
  bool& show_total()   { return show_total_; }

 private:
  // Envoie 0x09A7 / 0x09A9 après avoir revalidé côté client (mêmes règles que le
  // natif). Pose le message d'état et vide le champ en cas de succès d'envoi.
  void SendTransfer(bool deposit);
  // Pose le message d'état depuis un id MsgStringTable natif (converti CP949→UTF-8),
  // pour afficher EXACTEMENT le libellé que le client afficherait.
  void SetStatusFromMsgString(int msg_id, bool is_error);
  void SetStatus(const char* utf8, bool is_error);

  bool quick_amounts_  = true;   // setting : boutons +10k/+100k/…
  bool show_total_     = true;   // setting : ligne Total + jauge de plafond

  // C'est NOUS qui avons baissé le flag de visibilité de la native ? Sert à ne le
  // remettre à 1 qu'une seule fois, quand imgui_enabled_ repasse à false — forcer 1
  // à chaque tick se battrait avec le « tout masquer » natif (behavior 116).
  bool native_hidden_ = false;

  bool open_     = false;  // la banque native est ouverte ce frame ?
  bool was_open_ = false;  // front montant (placement à la 1re ouverture)
  bool need_pos_ = false;  // repositionner à côté de la native
  bool show_panel_ = true; // transitoire : détection du clic sur le X

  int  spawn_x_ = 0, spawn_y_ = 0;

  // Soldes lus dans les globales (rafraîchis chaque tick par les paquets ZC).
  long long vault_ = 0;  // g_BankVault  (s64)
  int       zeny_  = 0;  // g_PlayerZeny (s32)

  // Montant saisi. En s64 pour absorber une saisie hors bornes sans déborder ;
  // clampé à [0, INT32_MAX] à chaque édition, comme UIBankWnd_ApplyAmountDelta.
  long long amount_ = 0;

  // Message d'état (bas de fenêtre), équivalent de la std::string this+0xB8 du natif.
  char status_[192] = {0};
  bool status_error_ = false;

  // Anti-double-envoi : tick du dernier paquet parti (un double-clic sur
  // « Déposer » enverrait sinon deux fois le même transfert).
  unsigned long last_send_tick_ = 0;
  // Idem pour l'ouverture : ZC_BANKING_CHECK BASCULE la fenêtre, donc deux 0x09AB
  // partis coup sur coup l'ouvriraient puis la refermeraient. Compteur séparé de
  // last_send_tick_ pour qu'ouvrir la banque ne bloque pas le dépôt qui suit.
  unsigned long last_toggle_tick_ = 0;
};
