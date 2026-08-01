#pragma once

#include <cstdint>

#include "features/plugin.h"

// ── BankWindow ───────────────────────────────────────────────────────────────
//
// Banque de zeny (Ctrl+B) en ImGui, skin RO, en REMPLACEMENT de la fenêtre native :
// celle-ci ne NAÎT PLUS. On a pris la place du handler de ZC_BANKING_CHECK 0x09A6
// (RegisterReplaceOpcode, révocable par l'interrupteur), qui était le seul chemin
// d'ouverture de la fenêtre 275 — le client ne l'ouvre jamais de lui-même.
//
// Auparavant elle naissait puis on la masquait. Une native masquée garde le
// CLAVIER : Entrée y déclenchait le bouton par défaut, c'est-à-dire un transfert
// de zeny décidé par personne. Le même piège que le refine (docs/weapon_refine_re.md
// §10), sur une fenêtre qui manipule de l'argent.
//
// 🔴 Ce que le handler remplacé faisait et que NOUS faisons donc maintenant :
// écrire `g_BankVault`. Cette globale n'est écrite que par 0x09A6/0x09A8/0x09AA, et
// elle est lue AILLEURS dans le client (le shop de styling propose d'aller à la
// banque quand `zeny < prix <= zeny + banque`, 0x007F0380). Ne plus l'écrire aurait
// laissé ce chemin sur un solde périmé. Les deux ACK 0x09A8/0x09AA, eux, restent au
// natif : ils tiennent les mêmes globales à jour et affichent leurs erreurs en chat,
// gratuitement, et leur rafraîchissement d'UI est un no-op sans fenêtre native.
//
// Les gardes d'OUVERTURE du `case 275` de MakeWindow (refine, grade-enchant,
// enchant, barter étendu, runes) sont rejouées ici, message en chat compris — ce
// n'est plus MakeWindow qui les porte. ⚠ Leur liste d'ids DIFFÈRE de celle qui
// bloque les transferts (Bank_IsBlockedByOpenWindow), les deux coexistent.
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

class BankWindow : public Plugin {
 public:
  BankWindow();

  const char* name() const override { return "BankWindow"; }

  void OnTick() override;      // relit les soldes + purge une native résiduelle
  void OnRenderUI() override;  // dessine la fenêtre ImGui si la banque est ouverte
  // ZC_BANKING_CHECK 0x09A6 : c'est LUI qui ouvre (et referme) la banque. Reprend
  // aussi l'écriture de g_BankVault que faisait le handler natif remplacé.
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Ouvre — ou referme — la banque, exactement comme Ctrl+B. C'est le chemin du
  // behavior 146 natif : si la banque est ouverte on la ferme côté client, sinon on
  // DEMANDE au serveur de l'ouvrir (CZ_REQ_BANKING_CHECK 0x09AB) — le client n'ouvre
  // jamais la banque lui-même, et l'ouvrir sans aller-retour afficherait un solde
  // périmé.
  // Public : appelé par le bouton « sac de zeny » du footer de l'InventoryViewer.
  //
  // Le raccourci NATIF continue de marcher : il lit `g_pUIBankWnd`, qui reste nul
  // puisque la fenêtre 275 ne naît plus, donc il envoie toujours 0x09AB — et c'est
  // notre handler qui fait la bascule à la réception. Un aller-retour serveur de
  // plus pour fermer, invisible à l'usage.
  void ToggleFromUi();

  // Appelé par le hook MakeWindow de WindowPosTweaks à la création de la fenêtre
  // 275. Ne fait plus rien, et c'est voulu — mêmes raisons que NpcDialogWindow :
  // la fenêtre ne naît plus, la masquer serait nuisible (une native invisible garde
  // le clavier), et la détruire ici est exclu puisqu'on est à l'intérieur de
  // MakeWindow, dont l'appelant va déréférencer le retour. C'est OnTick qui purge.
  void HideNativeAtCreation(void* win);

  // ── Settings PERSISTANTS (bourgeon_settings.yaml, chargés/sauvés par MoonlightUi)
  // « bank_imgui » : fenêtre ImGui, handler de 0x09A6 remplacé. Basculé en GROUPE
  // par SetModernInterface, jamais isolément. Défaut OFF, comme tout le groupe.
  bool imgui_enabled_ = false;

 private:
  // Ferme la SESSION banque côté client : notre fenêtre, plus une native résiduelle
  // s'il en traîne une. Aucun paquet — le serveur ne tient aucun état de banque
  // ouverte (ce client n'envoie jamais CZ_REQ_CLOSE_BANKING 0x09B8), exactement
  // comme le X natif qui se contente d'un SaveRectAndCloseWindow.
  void CloseSession();
  // Envoie 0x09A7 / 0x09A9 après avoir revalidé côté client (mêmes règles que le
  // natif). Pose le message d'état et vide le champ en cas de succès d'envoi.
  void SendTransfer(bool deposit);
  // Pose le message d'état depuis un id MsgStringTable natif (converti CP949→UTF-8),
  // pour afficher EXACTEMENT le libellé que le client afficherait.
  void SetStatusFromMsgString(int msg_id, bool is_error);
  void SetStatus(const char* utf8, bool is_error);

  // (Plus de `quick_amounts_` ni de `show_total_` : le fond de cette fenêtre est un
  // bitmap du client à hauteur FIXE, donc rien de son contenu ne peut être masqué
  // sans faire glisser le reste hors du fond peint. Les deux blocs sont désormais
  // permanents, et la fenêtre n'a plus de section de réglages du tout.)

  // Session banque ouverte. C'est le PAQUET 0x09A6 qui la bascule (fil réseau), et
  // notre fermeture qui l'éteint — plus la présence d'une fenêtre native, qui
  // n'existe plus. Même choix que NpcDialogWindow : un état à nous, dont la source
  // est le serveur, plutôt qu'une observation indirecte du client.
  bool open_     = false;
  bool was_open_ = false;  // front montant (réinitialisation à la 1re ouverture)
  bool need_pos_ = false;  // (re)placer la fenêtre à l'ouverture
  bool show_panel_ = true; // transitoire : détection du clic sur le X

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
