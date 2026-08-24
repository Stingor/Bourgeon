#pragma once

// ── ChatWindow — la chatbox en ImGui ─────────────────────────────────────────
//
// Remplacement de la chatbox native (client 20250716). RE complète :
// docs/chatbox_re.md, mémoire project_chatbox_imgui_conversion.
//
// ÉTAT : phase 1 (ingestion + rendu) livrée, puis phase 2 PARTIELLE — la fenêtre
// ENVOIE désormais, mais la native est encore créée à côté ; ce qui reste pour la
// bascule complète, c'est de l'empêcher de naître (MakeWindow(1) et 0x84) et de
// reprendre le focus ENTER.
//
// INGESTION. On écoute LE chokepoint `UIWindowMgr_ChatAction 0x00a4ad20` — la
// fonction par laquelle passe TOUTE ligne de chat, quel que soit son émetteur
// (paquet réseau, message système, Lua) — et on tient notre propre modèle : un
// anneau de lignes {texte, couleur, type, sender, heure}.
//
// Pourquoi ce point d'accroche et pas le `case 0x25` du WndProc : à la bascule la
// fenêtre native ne NAÎTRA plus, et `ChatAction` empile alors chaque ligne dans la
// file `mgr+0x4C4` SANS LIMITE (elle n'est drainée qu'à la création de la
// fenêtre). Supprimer la fenêtre sans intercepter ici = fuite mémoire illimitée.
//
// 🔴 Ce détour porte AUSSI le filtre de messages système historique (la liste
// `kBlockedMsgs`, autrefois `Bourgeon::InstallChatMessageFilter` dans
// bourgeon.cc). Il n'y a qu'un seul jeu d'octets à l'entrée de la fonction : deux
// détours concurrents sur la même adresse, c'est le second qui gagne et le
// premier qui meurt en silence. Les deux besoins vivent donc dans le même stub.
//
// ENVOI. Copie fidèle de `ChatMacro_SendEmotionHotkeySlot 0x00a47400`, qui envoie
// un texte par le pipeline COMPLET du client sans qu'aucune fenêtre existe :
// filtre de mots interdits, `/commandes` (table + handlers désactivables), et
// `CMode::SendMsg` selon le mode d'envoi. On ne réimplémente aucune règle de jeu.
// 🔴 Ces appels natifs ne sont JAMAIS joués pendant une frame ImGui (modales
// bloquantes qui relancent le rendu) : le rendu ARME, `FlushPending` joue, depuis
// `Bourgeon::OnProcessInput`.
//
// SKIN. La chatbox du client n'a pas de barre de titre : c'est un rectangle
// translucide sombre, des onglets gris en haut, une ligne de saisie en bas. Le
// cadre est `ro::BeginRoChatWindow` (ui/ro_imgui.h) ; les onglets et les boutons
// sont peints ici, les boutons avec les VRAIS bitmaps du client (les noms sont
// ceux de `UINewChatWnd_Create` : battle_option, dialog_btn0, sys_base).

#include <cstdint>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "features/item_cell.h"     // itemcell::ChatLink (un lien <ITEML> relu)
#include "features/link_gesture.h"  // links::Target (gestes communs d'un lien)
#include "features/plugin.h"
#include "imgui.h"
#include "ui/ro_imgui.h"  // ro::RoChatSkin (rendu par valeur par MakeSkin)

class ChatWindow : public Plugin {
 public:
  ChatWindow();
  ~ChatWindow() override;

  const char* name() const override { return "ChatWindow"; }

  void OnRenderUI() override;
  // Écrit la disposition dès qu'elle a cessé de bouger : c'est le SEUL moment où
  // elle est enregistrée pour un joueur qui ferme le client depuis le monde (cf.
  // OnTick).
  void OnTick() override;
  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  // Fil RÉSEAU : on COPIE, rien de plus (cf. features/net_inbox.h).
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;
  // ENTRÉE donne le focus à notre saisie, comme la touche le faisait pour la
  // native. On ne BLOQUE pas la touche : une fois la native détruite, plus
  // personne d'autre ne l'attend côté chat.
  void OnKeyDown(unsigned long vkey, int new_key, int accurate_key) override;

  // Joue les commandes natives armées pendant le rendu (envoi de texte). Appelée
  // par Bourgeon::OnProcessInput, hors de toute frame ImGui — cf. l'en-tête.
  void FlushPending();

  // « Ouvre le chat et donne-lui le focus » — l'intention derrière la touche
  // ENTRÉE. Traitée au prochain rendu, où l'état d'ImGui est valide.
  void RequestInputFocus() { enter_pending_ = true; }

  // 🔴 LES TOUCHES QUE LE WndProc NOUS CONFISQUE, REMISES DIRECTEMENT. Sans ça
  // elles se perdent, et c'est le piège le moins visible de tout ce fichier :
  // `OnKeyDown` n'est PAS branché sur le WndProc — il vient de
  // `Bourgeon::FireKeyDown`, appelé depuis `UIWindowMgr::ProcessPushButtonHook`,
  // c'est-à-dire depuis LE JEU. Dès que la barre est ouverte sans clavier, le
  // WndProc retire la touche au jeu (sinon la lettre déplacerait le personnage) —
  // et du même coup il affame notre propre handler. Entrée et Échap ne
  // refermaient alors plus rien.
  void OnRawKey(unsigned long vkey);

  // 🔴 « TAPER ÉCRIT DANS LA BARRE », SANS TENIR L'`ActiveId`. Vrai quand la barre
  // est dépliée en battle mode et que personne n'écrit ailleurs : les caractères
  // sont alors CONFISQUÉS au jeu (sinon ils déplaceraient le personnage) et remis
  // à la saisie, qui prend le clavier au passage.
  //
  // C'est ce qui reste de l'invariant du chat natif, une fois admis qu'ImGui
  // interdit de garder l'`ActiveId` en permanence (cf. le bloc du battle mode plus
  // bas) : le joueur retrouve « j'ouvre, je clique ailleurs, je tape, ça s'écrit
  // dans le chat » sans qu'aucun clic ni double-clic ne soit sacrifié.
  //
  // Lu aussi par le WndProc, qui décide de rendre ou non la touche au client.
  bool WantsTypedKeys() const;

  // 🔴 ÉCHAP NOUS APPARTIENT tant que la barre est dépliée en battle mode, même
  // sans le clavier. Lu par le WndProc pour la confisquer au jeu : sans ça, la
  // frappe qui referme la barre ouvrirait AUSSI le menu du client — deux effets
  // pour un geste. Le pendant de `ro::AnyEscapeWindowOpen`, pour une barre qui
  // n'est pas une fenêtre RO.
  bool WantsEscapeKey() const;

  // 🔴 LA BARRE EST-ELLE ARMÉE, c'est-à-dire en droit de recevoir ENTRÉE avant
  // qui que ce soit d'autre ? Lu par les fenêtres qui se servent de la touche
  // comme raccourci de validation — le dialogue NPC en tête, dont un lien
  // `<ITEML>`/`<MOBL>` se relaie dans le chat d'un Maj+clic. Sans ce prédicat, le
  // lien atterrissait dans la saisie et n'en repartait plus : le dialogue prenait
  // la touche pour son bouton « Suivant », et il fallait fermer le script pour
  // pouvoir envoyer.
  //
  // Armée = la ligne de saisie est à l'écran ET l'un de ces trois signes : la
  // barre est DÉPLIÉE en battle mode (le joueur l'a ouverte exprès), elle porte du
  // TEXTE, ou elle a le CLAVIER — ou est sur le point de l'avoir. Hors battle mode
  // et vide et sans clavier, elle ne réclame rien : « Entrée = Suivant » continue
  // de marcher, ce qui est tout l'intérêt de ne pas confisquer la touche pour la
  // seule durée d'un script.
  //
  // Lu depuis le WndProc, donc ENTRE deux frames : d'où la tolérance d'une frame
  // sur le focus (même mécanique que `PickerOpen`).
  bool OwnsEnterKey() const;

  // Interrupteur de la fenêtre (persisté par MoonlightUi, clé « chatwnd_imgui »).
  // Public comme chez les autres fenêtres modernes : c'est la table de réglages
  // qui l'écrit (MLUI_FIELD).
  bool imgui_enabled_ = false;

  // ── Réglages (lus/écrits par la table de persistance de MoonlightUi) ──
  bool& magnet()       { return magnet_; }
  bool& timestamps()   { return timestamps_; }
  bool& item_icons()   { return item_icons_; }
  bool& input_bar()    { return input_bar_; }
  bool& diagnostic()   { return diagnostic_; }
  int&  history_cap()  { return history_cap_; }
  bool& keep_history() { return keep_history_; }
  int&  keep_lines()   { return keep_lines_; }
  // Avertir avant d'ouvrir une adresse dans le navigateur. Opt-OUT : par défaut
  // on avertit, parce qu'un lien de chat vient d'un tiers et que le texte
  // affiché n'a aucun rapport obligé avec la destination. Le joueur qui sait ce
  // qu'il fait peut le retirer ; c'est son choix, et il est explicite.
  bool& url_confirm()     { return url_confirm_; }
  bool  url_confirm() const { return url_confirm_; }
  // Aperçu d'une image au survol d'un lien. Opt-IN, décoché par défaut : c'est du
  // trafic réseau déclenché par le lien d'AUTRUI, et ça mérite un accord explicite
  // même si la liste blanche d'hôtes rend l'opération sans danger pour l'anonymat
  // (cf. features/systems/image_preview.h).
  bool& url_preview()     { return url_preview_; }
  bool  url_preview() const { return url_preview_; }
  // Famille de police du LOG (index dans ro::ChatFamily*). 0 = « Système », qui
  // ne force rien. Les familles sont bakées au démarrage, donc changer de valeur
  // s'applique à chaud — contrairement aux glyphes coréens, qui touchent l'atlas.
  int&  font_family()     { return font_family_; }
  // Afficher les vignettes d'image et les emotes ? Éteint, le lien reste du
  // texte cliquable et l'emote son « :nom: » — et rien n'est téléchargé.
  //
  // 🔴 SÉPARÉ de la taille, à dessein. Un « 0 = aucune » sur le curseur
  // mélangeait deux questions dans une seule commande : le curseur ne dit plus
  // que la taille, la case dit s'il y en a.
  bool& thumbs()          { return thumbs_; }
  // Hauteur des vignettes, en pixels. Bornée à 24..128 : en dessous ce n'est
  // plus lisible, au-dessus ça cesse d'être une vignette.
  //
  // ⚠ Au-delà de la hauteur d'une ligne, la RANGÉE grandit pour accueillir
  // l'image — le repli et le cache de hauteur suivent (cf. `row_h` au dessin).
  int&  thumb_px()        { return thumb_px_; }
  int   thumb_px() const  { return thumb_px_; }
  // Hôtes autorisés PAR LE JOUEUR, sérialisés (« a.com;b.net »). La liste vivante
  // est celle d'imgprev ; ce champ n'est que sa forme persistable — c'est lui que
  // les réglages écrivent et relisent, et OnTick les resynchronise.
  std::string& url_hosts() { return url_hosts_; }
  int& font_scale_pct()   { return font_scale_pct_; }  // texte du LOG, 70..160 %
  int& ui_scale_pct()     { return ui_scale_pct_; }    // habillage, 70..160 %
  int& padding_px()       { return padding_px_; }      // marge du cadre
  int& line_gap_px()      { return line_gap_px_; }     // interligne

  // Couleurs du skin, au format du color picker (float RGBA 0..1) : c'est ce
  // qu'attend la persistance kColorHex, qui les écrit en « AARRGGBB ». Publiques
  // et nues, comme les couleurs de dps_meter — la table de réglages prend leur
  // adresse (MLUI_FIELD).
  float body_rgba_[4]   = {0.0f, 0.0f, 0.0f, 0.59f};   // ≈ le 0x96000000 du fond natif
  float border_rgba_[4] = {0.77f, 0.77f, 0.77f, 1.0f};
  float tab_rgba_[4]    = {0.56f, 0.58f, 0.56f, 1.0f}; // gris de l'UITabStrip (0x8E938E)

  // Section « Chatbox ImGui » du panneau Moonlight. Renvoie true si un réglage a
  // changé (déclenche la sauvegarde côté MoonlightUi).
  bool DrawSettings();

  // Appelée par le détour de ChatAction, sur le fil du jeu, pour les actions qui
  // ajoutent une ligne (1 et 0x13). `text` est la chaîne du client, déjà
  // recopiée dans un tampon à nous : rien n'est retenu d'elle.
  //
  // 🔴 AUCUN `sender` NE TRAVERSE ICI, et c'est délibéré : celui que le client
  // tend à ses points d'entrée est un résidu de pile (démonstration dans le
  // corps d'`Ingest`). Le locuteur se lit dans le TEXTE, comme le fait le natif.
  //
  // `source` marque QUI a fourni la ligne : 'A' = le détour de `ChatAction`,
  // 'W' = le `case 0x25` du WndProc natif. Il n'est affiché qu'en mode
  // diagnostic, et il est là pour une raison précise : « la ligne arrive en
  // double » a exactement deux explications — nos deux sources qui ingèrent la
  // même ligne, ou le serveur qui l'envoie deux fois — et elles se corrigent à
  // des endroits opposés. Deux `A` (ou deux `W`) accusent le serveur ; un `A` et
  // un `W` nous accusent.
  void Ingest(const char* text, uint32_t rgb, int type, char source);

  // ── Chuchotement 1:1 ────────────────────────────────────────────────────────
  // Range une ligne de conversation privée dans la fenêtre du correspondant, en
  // l'ouvrant si besoin. `with_wire` est le nom du CORRESPONDANT — celui d'en
  // face dans les DEUX sens — et `text_wire` la ligne déjà mise en forme par le
  // client, tous deux dans la code-page du fil.
  //
  // Appelée par le détour du pivot natif (`UIWindowMgr_OnWhisperReceived`), donc
  // depuis un handler de paquet : hors frame ImGui, sur le fil principal.
  // Renvoie true si la ligne a été prise en charge — l'appelant dit alors au
  // client qu'une fenêtre l'a consommée, exactement comme sa popup native.
  //
  // 🔴 `outgoing` est le SENS, et il ne se devine nulle part ailleurs : le texte
  // du pivot est composé pour la fenêtre 1:1 (« <qui parle> : … ») et ne porte
  // aucun « To »/« From ». Seule la couleur-marqueur du client le dit. Il sert à
  // l'en-tête de la copie que cette ligne dépose dans le JOURNAL — sans quoi un
  // chuchotement s'y lit comme une parole publique.
  bool IngestChatRoomLine(const char* local_text, uint32_t rgb);
  void DrawChatRoomLog(std::string* link_insert_target);
  bool AppendToActiveInput(const std::string& insert);
  // Publique pour la même raison que les deux au-dessus : le SALON dessine cette
  // ligne dans sa propre fenêtre (`chatwnd::DrawChatInputRow`). Elle reste écrite
  // pour être appelée UNE fois par frame, tout en bas d'un conteneur ImGui.
  void DrawInputRow();
  // Public parce que le pont `chatwnd::ResolveOutgoingLinks` en a besoin : une
  // saisie qui n'est pas la barre principale (celle du salon) doit pouvoir
  // retraduire ses libellés de liens avant d'envoyer. Const, sans effet de bord.
  std::string ResolveItemLinks(const char* utf8) const;
  void ClearChatRoomLog();
  bool IngestWhisper(const char* with_wire, const char* text_wire, uint32_t rgb,
                     uint32_t aid, bool outgoing);

  // Ouvre (ou ramène) la conversation avec ce joueur, et lui donne le clavier —
  // c'est un geste EXPLICITE, contrairement à la réception d'un message. `name`
  // est dans la code-page du fil.
  //
  // 🔴 DEUX chemins d'ouverture chez le client, et le second ne passe pas par le
  // pivot : le « Chuchoter » du menu contextuel d'entité appelle `ChatAction`
  // case 14 en direct. C'est par ici qu'il arrive, et c'est aussi le point
  // d'entrée pour NOS propres surfaces (liste de guilde, etc.).
  //
  // `aid_display` est facultatif : l'AID **obfusqué** tel que le client l'écrit
  // entre crochets, seule forme dont dispose le menu contextuel. Il est décodé
  // pour retrouver l'AID réel, sans lequel la guilde resterait introuvable.
  // Passer nullptr quand on a mieux à offrir — ou rien.
  bool OpenWhisperWindow(const char* name_wire, const char* aid_display = nullptr);
  // Variante pour les appelants qui connaissent déjà l'AID réel.
  bool OpenWhisperWindowByAid(const char* name_wire, uint32_t aid);

  // Le geste « chuchoter » ORDINAIRE : le nom va dans la box « Pseudo » de la
  // barre principale, la barre s'ouvre si le battle mode l'avait repliée, et le
  // clavier part à la saisie. Aucune fenêtre ne s'ouvre, rien ne part sur le fil.
  //
  // 🔴 C'est le geste par DÉFAUT de toutes nos entrées « Chuchoter » (menu
  // contextuel d'entité, lien de joueur, liste de guilde). Ouvrir une fenêtre
  // séparée est l'entrée d'à côté : un joueur qui a décoché les fenêtres
  // individuelles du « Friend Setup » ne doit pas en voir surgir une parce qu'il
  // a cliqué « Chuchoter » — c'est exactement ce qu'il a refusé.
  //
  // Rend false si l'interface moderne est éteinte (la barre n'est pas dessinée) :
  // l'appelant retombe alors sur le chemin natif.
  bool TargetWhisper(const char* name_wire);

  // Vide l'historique (bouton du panneau, changement de personnage).
  void ClearHistory();

  // ── `/savechat` ────────────────────────────────────────────────────────────
  //
  // Le dump ponctuel du journal, un fichier par onglet. Le client en a un
  // (`ChatLog_SaveAllToFiles 0x00907030`, docs/chatbox_re.md §6.3) mais il
  // parcourt SES fenêtres — que l'interface moderne détruit — et sort donc sans
  // rien écrire ni rien dire. La commande était devenue muette.
  //
  // ⚠ ARMER, jamais écrire tout de suite : `ChatActionFilter` peut tourner sur le
  // fil réseau, et `channels_` n'appartient qu'au rendu. L'écriture a lieu à la
  // frame suivante, dans `OnRenderUI` (cf. features/net_inbox.h, même règle).
  void RequestSaveLog();

  // ── Le parseur du chat, ouvert aux autres surfaces d'affichage ──────────────
  // Rend le texte AFFICHABLE d'une ligne brute (code-page client) : `<ITEML>`,
  // `<MOBL>`, `<ITMR>` et `<CRAF>` résolus en leur libellé, `^RRGGBB` et `^i[]`
  // retirés. Sortie UTF-8, prête pour ImGui.
  //
  // 🔴 C'est le RACCORDEMENT de la bulle de tête à l'interface moderne, et il
  // passe par le MÊME `ParseUtf8` que la chatbox — pas par un résolveur bis. Une
  // seconde implémentation divergerait au premier libellé qu'on retoucherait
  // d'un seul côté, et le joueur verrait deux textes différents pour la même
  // phrase, à l'écran en même temps.
  //
  // ⚠ Ce n'est pas une commodité mais une nécessité : chez nous la résolution
  // native est MORTE. Le chemin natif de la bulle appelle
  // `ChatText_TransformTagLinks` sous `if (g_pNewChatWnd)`, or ce pointeur vaut 0
  // dès que la chatbox native est détruite — donc même `<ITEML>`, pourtant une
  // balise du client, ressort littéralement (cf. docs/entity_chat_balloon_re.md).
  std::string PlainTextFromWire(const char* wire) const;

  // ── Maj + clic gauche sur un objet = son LIEN dans la saisie ────────────────
  // `info` = un ItemSkillInfo vivant (nœud de liste inventaire/chariot, slot
  // d'équipement). On insère le NOM lisible dans la barre de saisie et on garde
  // de côté le `<ITEML>…</ITEML>` correspondant, que `QueueSend` substitue au
  // moment de l'envoi : c'est exactement la mécanique du natif (l'accessoire
  // `UIItemTagOnChat` à `edit+0x144` tient le texte affiché ET le texte résolu).
  //
  // Renvoie false si la chatbox ImGui n'est pas active, si la barre de saisie est
  // masquée par le réglage, ou si les TROIS liens du natif sont déjà posés — même
  // plafond, pour la même raison : un message de chat a une longueur bornée.
  bool AppendItemLink(void* info);

  // Poser un lien de NAVIGATION : « va à cet endroit ». Balise `<NAVIL>`, et
  // c'est la seule de la famille qui soit NATIVE — le client la reconnaît dans
  // `UIRichTextBox_OnMsg` et rappelle son pathfinder au clic. Aucun détour
  // maison n'est donc nécessaire, et un joueur sans Bourgeon suit le lien.
  //
  // 🔴 Elle est insérée TELLE QUELLE, visible, sans mécanique de substitution à
  // l'envoi : c'est exactement ce que fait le bouton « Share » du natif, qui
  // pose la balite brute dans la barre et laisse le joueur valider. Y ajouter un
  // libellé lisible la rendrait plus jolie chez nous et ILLISIBLE chez les
  // autres, puisque c'est le texte envoyé qui voyage.
  //
  // Les coordonnées sont encodées en base 62 (`0123456789a-zA-Z`), poids faible
  // d'abord, sur deux caractères — la forme qu'attend le client.
  // `x`/`y` à 0 = « la carte entière ».
  bool AppendNaviLink(const char* map_name, int x, int y);

  // Poser un lien de RECHERCHE de navigation : « [Carte: Prontera] »,
  // « [PNJ: Kari] », « [Monstre: Poring] ». Balise `<NAVS>famille:terme</NAVS>`,
  // à NOUS — le client n'a rien de tel, et il ne pouvait rien avoir : un PNJ et
  // un monstre n'ont pas de position unique, donc pas de `<NAVIL>` possible.
  //
  // `kind` est un `links::NaviKind`. Le terme est le nom INTERNE pour une carte
  // (seule identité indépendante de la langue) et le nom affiché sinon.
  //
  // ⚠ Le libellé lisible est composé chez le LECTEUR, comme pour un lien de
  // réglage : c'est pourquoi cette balise passe par la mécanique de
  // substitution à l'envoi plutôt que d'être insérée nue comme `<NAVIL>`.
  //
  // `map_utf8` est le CONTEXTE : la carte où l'auteur a vu ce qu'il partage.
  // Nul ou vide = sans contexte. Il occupe un champ du MILIEU de la balise
  // (`<NAVS>famille:carte:terme</NAVS>`), le terme restant le dernier — seule
  // façon de laisser celui-ci contenir espaces et ponctuation.
  bool AppendNaviSearchLink(uint8_t kind, const char* term_utf8,
                            const char* map_utf8 = nullptr);

  // Idem pour un MONSTRE. Le client ne sait pas nommer un monstre (le nom ne vient
  // ni de mob_db, qu'il n'a pas, ni du paquet de la fiche) : c'est l'appelant qui
  // le fournit — la fiche de monstre et la table des drops l'ont tous deux — et la
  // balise le transporte, sinon le lecteur ne verrait qu'un numéro.
  // `rank` : 0 = normal, 1 = boss, 2 = MVP (même convention que la table des drops).
  bool AppendMobLink(uint32_t mob_id, int rank, const char* name_utf8);

  // RELAYER un lien d'objet : reposer dans la saisie celui qu'un autre joueur
  // vient d'afficher. On n'a pas l'objet — on ne l'aura jamais — mais la balise
  // suffit : elle est ré-encodée telle quelle (itemcell::BuildChatLinkFromLink),
  // donc le refine, les cartes et le forgeron survivent au relais.
  // ⚠ Bascule AUTOMATIQUEMENT sur `AppendItemRefLink` quand l'objet n'est pas
  // dans le sac : le client refuse d'envoyer un `<ITEML>` qu'on ne possède pas.
  bool AppendItemLinkFromLink(const itemcell::ChatLink& link);

  // Poser une RÉFÉRENCE d'objet : l'objet de BASE, sans refine ni cartes, dans
  // notre propre balise `<ITMR>`. C'est le seul moyen de parler d'un objet qu'on
  // n'a pas — le client bloque le `<ITEML>` correspondant À L'ENVOI, la ligne
  // entière avec (« Item tags can only tag items you own. »). Même détour que
  // `<MOBL>` pour les monstres : une balise que le client ne connaît pas ne peut
  // pas la filtrer.
  //
  // ⚠ Un joueur SANS Bourgeon voit la balise brute — d'où le nom en clair dedans,
  // qui garde la ligne lisible. `AppendItemLinkFromLink` reste donc préférable
  // dès qu'on possède l'objet : `<ITEML>` est rendu par tous les clients.
  // `name_utf8` nul ou vide -> le nom de la DB client.
  bool AppendItemRefLink(uint32_t item_id, const char* name_utf8);

  // Poser le lien d'une RECETTE : « [Recette: Acid Bottle] ». Désigne la façon de
  // FAIRE l'objet, pas l'objet — le survol montre le métier et les composants, le
  // clic ouvre l'Atlas. Balise `<CRAF>`, même détour maison que `<ITMR>`.
  // 🔴 Refusé (false) si l'objet n'a pas de recette : un lien vers une fiche vide
  // vaut moins que pas de lien.
  bool AppendRecipeLink(uint32_t item_id, const char* name_utf8);

  // Poser le lien d'une DESTINATION DE RÉGLAGES : « [Réglage: Objet obtenu] ».
  // `key` désigne un en-tête du panneau ou une section de sa nav (cf.
  // iface::DestLabel) ; false si cette version ne la connaît pas. Balise
  // `<SETL>clé:libellé</SETL>`, même détour maison que `<MOBL>` et `<ITMR>` — le
  // client ne connaît pas la balise, donc il ne la filtre pas.
  //
  // 🔴 Ce qui voyage est la CLÉ (« item_toast »), jamais un numéro : un numéro
  // décrit l'ordre d'une version de Bourgeon, et une insertion enverrait le
  // lecteur sur la section d'à côté sans que rien ne le dise. Le libellé voyage
  // aussi, mais seulement pour rester lisible chez qui n'a pas Bourgeon : à
  // l'affichage, c'est le libellé LOCAL (donc traduit) qui gagne.
  bool AppendSettingLink(const char* key);

  // Poser le lien d'un STYLE : « [Style: Étiquette] ». Balise
  // `<STYL>étiquette:code`.
  //
  // 🔴 Le CODE voyage EN ENTIER, contrairement à tous les autres liens qui ne
  // transportent qu'une désignation. Un objet ou un monstre existent chez le
  // lecteur ; un style n'est connu que tant que son porteur est en vue, et un
  // lien qui meurt quand la personne change de carte vaut moins que pas de lien.
  //
  // `owner_utf8` est une ÉTIQUETTE, pas une identité : le pseudo du porteur
  // quand on partage son propre style (nul = le nôtre, résolu ici), mais le nom
  // du préréglage quand on partage celui-ci. C'est ce que le lecteur ne sait pas
  // encore qui doit s'y trouver — le pseudo de l'expéditeur, lui, est déjà en
  // tête de sa ligne de chat.
  //
  // ⚠ La balise fait donc une centaine de caractères, visibles bruts chez un
  // client sans Bourgeon — l'étiquette est en tête pour garder le début lisible.
  bool AppendStyleLink(const char* code, const char* owner_utf8);

  // Arme une commande (`@iteminfo`, `@mobinfo`…) pour le prochain FlushPending.
  // Même canal que l'envoi d'une ligne tapée — donc le pipeline COMPLET du
  // client. 🔴 Publique parce que le menu des liens (features/link_gesture.cc)
  // s'en sert : une commande ne doit JAMAIS partir pendant une frame ImGui.
  void QueueCommand(const char* utf8);

  // ── Les bascules de canal d'une ligne TAPÉE ────────────────────────────────
  // Un « % / $ / # » en tête de phrase, ou la touche équivalente maintenue au
  // moment où l'on valide. Ce sont les trois du client, relevées dans le
  // `case 0` de `Chat_HandleChatMessage` (docs/chatbox_re.md §3.3) : elles
  // envoient la phrase à un autre canal que celui de la combo, sans y toucher.
  //
  // 🔴 Publique parce que le chemin d'envoi vit dans le namespace anonyme du
  // .cc — elle n'a aucun autre usage à l'extérieur.
  struct SendToggles {
    bool party = false;  // « % » ou Ctrl
    bool guild = false;  // « $ » ou Alt
    bool ally  = false;  // « # » ou Verr.Maj
  };

  // ── Actions sur un JOUEUR désigné par son NOM ───────────────────────────────
  // Armées pendant la frame, jouées par `FlushPending` — deux d'entre elles
  // passent par le natif, proscrit entre NewFrame et Render. `name` est dans la
  // code-page du fil.
  //
  // 🔴 Toutes prennent un NOM et pas un AID, parce que c'est tout ce qu'une ligne
  // de chat porte. Cela EXCLUT le chemin du menu contextuel d'entité, qui résout
  // sa cible par `GameMode_CopyEntityName(gm, out, aid)` et ne connaît donc que
  // les acteurs présents à l'écran.
  enum class NameAction : uint8_t { kNone = 0, kPartyInvite, kFriendAdd, kGuildInvite };
  void QueueNameAction(NameAction action, const char* name_wire);
  // Suis-je dans un groupe / une guilde ? Le menu des liens s'en sert pour griser
  // ce qui n'a aucune chance d'aboutir, avec sa raison — d'où leur place ici et
  // non dans la partie privée. `InGuild` sert aussi en interne (#ally hors
  // guilde) : c'est la globale `g_OwnGuildId`, celle que le chemin d'envoi natif
  // consulte lui-même pour autoriser le mode Guilde.
  bool InParty() const;
  bool InGuild() const;
  // Ce pseudo (UTF-8) est-il le nôtre ? Le menu s'en sert pour ne pas proposer
  // de s'inviter soi-même — le serveur refuserait, et l'entrée n'aurait servi
  // qu'à faire cliquer dans le vide.
  bool IsOwnName(const char* utf8) const;

  // Compteurs de DIAGNOSTIC, posés par le détour. `seen` = appels d'ajout de
  // ligne vus (action 1 ou 0x13), `kept` = lignes réellement entrées dans le
  // modèle. Deux nombres qui divergent disent où chercher : seen == kept mais peu
  // de lignes à l'écran ⇒ c'est le filtre de canal ; seen bien plus petit que ce
  // que le chat natif affiche ⇒ les lignes passent ailleurs que par ChatAction.
  unsigned ingest_seen_ = 0;
  unsigned ingest_kept_ = 0;

  // ── Le modèle ──────────────────────────────────────────────────────────────
  // 🔴 `Run` et `Line` sont PUBLICS, et pas par commodité : ce sont eux que les
  // autres surfaces d'affichage doivent consommer. La bulle au-dessus des têtes
  // en dessine exactement les mêmes fragments (couleurs, emotes du jeu, icônes
  // d'objet, liens), avec les mêmes primitives — c'est ce qui garantit qu'elle
  // ne montrera jamais autre chose que le chat pour la même phrase.
  //
  // ⚠ Rendre depuis `Line::plain` ne suffit PAS : ce n'est que la concaténation
  // des textes, donc un fragment d'emote y reste écrit « :nom: » et une icône
  // d'objet y disparaît. Il faut la liste des runs.
 public:
  // Un fragment de ligne déjà analysé : le parse (^RRGGBB, ^i[], <ITEML>) est
  // fait UNE fois à l'ingestion, pas à chaque frame. Le chat est précisément
  // l'endroit où le coût par frame a déjà coûté des freezes (chat_trim_freeze).
  struct Run {
    // Genre du fragment CLIQUABLE. Un lien de chat n'est plus forcément un objet :
    // le monstre et l'URL empruntent exactement le même chemin (zone de clic
    // continue, couleur, curseur main, menu contextuel) — seule change l'action.
    // `kPlayer` = le pseudo en tête de ligne, posé après le parse à partir du
    // `sender` déjà extrait — il ne s'analyse pas depuis le texte, rien ne
    // distingue un pseudo d'un mot ordinaire.
    // `kRecipe` réutilise `item_id` : il désigne un objet, mais l'intention est
    // sa RECETTE — le survol montre métier et matériaux, le clic ouvre l'Atlas.
    // `kSetting` ne désigne rien du jeu : c'est une SECTION du panneau de
    // réglages Bourgeon, et le clic l'ouvre. C'est le premier lien qui parle du
    // CLIENT plutôt que du monde — « va voir ce réglage » est ce qu'on répond
    // vingt fois par jour dans un chat d'entraide.
    // `kStyle` est le second lien qui ne parle pas du monde : il porte le STYLE
    // d'un joueur — couleurs de corps, palette de cheveux, coiffure. 🔴 Il
    // transporte le CODE en entier, pas un pseudo à résoudre : un style ne se
    // retrouve nulle part une fois son porteur hors de vue, et un lien qui meurt
    // quand la personne change de carte vaut moins que pas de lien (même règle
    // que `kRecipe` sur une recette absente).
    // `kNavi` est le LIEU d'un `<NAVIL>` — la balise du bouton « Share » de la
    // navigation, que le client natif rend cliquable de son côté. Le clic lance
    // le guidage ; c'est le seul lien dont l'action MET LE JEU EN MOUVEMENT.
    // `kNaviSearch` est une RECHERCHE de navigation (« [Carte: Prontera] »,
    // « [PNJ: Kari] ») : balise à nous, libellé composé chez le lecteur, et le
    // clic ouvre le panneau dessus. C'est le seul lien capable de désigner un
    // PNJ ou un monstre par son LIEU — ni l'un ni l'autre n'a de position unique.
    enum LinkKind : uint8_t {
      kNone = 0, kItem, kMob, kUrl, kPlayer, kRecipe, kSetting, kStyle, kNavi,
      kNaviSearch
    };
    std::string text;      // UTF-8, prêt pour ImGui
    uint32_t    color = 0; // 0 = couleur par défaut de la ligne
    // Balisage **gras** / *italique*, la syntaxe de Discord — donc un message
    // relayé se met en forme tout seul, et un joueur peut l'écrire aussi.
    //
    // 🔴 Ce sont de VRAIES polices, pas un effet : ImGui ne synthétise ni l'un ni
    // l'autre. Et une variante étant plus large, celle du fragment doit servir à
    // sa MESURE autant qu'à son dessin — sinon le repli tombe à côté.
    bool        bold = false;
    bool        italic = false;
    uint32_t    item_id = 0;  // lien <ITEML> / icône ^i[] : id de l'objet
    uint8_t     kind = kNone;
    bool is_link() const { return kind != kNone; }
    // kMob : le monstre désigné. Le NOM n'est pas dans le client (cf.
    // project_monster_info_window) — c'est la balise qui le porte, et le rang sert
    // au badge « [MVP] » comme dans la table des drops.
    uint32_t    mob_id = 0;
    uint8_t     mob_rank = 0;  // 0 = normal, 1 = boss, 2 = MVP
    // Le nom NU, sans son badge — celui que le texte affiche décoré (« <[MVP]
    // Baphomet> ») et que la balise, elle, transporte tel quel. Le garder évite
    // de le ré-extraire d'un libellé pour relayer le lien.
    std::string mob_name;
    // kSetting : la CLÉ de la destination, telle qu'elle a voyagé. Elle n'est
    // posée QUE si cette version la reconnaît — sinon le fragment n'est pas un
    // lien du tout (le texte reste lisible, il ne mène simplement nulle part).
    std::string setting_key;
    // kNavi : le nom INTERNE de la carte et la position, telles que la balise les
    // porte. `(0, 0)` = la carte entière — c'est ce que le partage écrit quand la
    // destination est un lieu et non un point.
    std::string navi_map;
    int         navi_x = 0;
    int         navi_y = 0;
    // kNaviSearch : ce qu'on cherche et dans quelle famille (`links::NaviKind`).
    // Pour une carte, c'est le nom INTERNE — la seule identité qui ne dépende
    // pas de la langue de l'expéditeur.
    std::string navi_term;
    uint8_t     navi_kind = 0;
    // Le CONTEXTE de la recherche, s'il y en a un : le nom interne de la carte
    // où l'auteur a vu ce qu'il partage. Il réutilise `navi_map` ci-dessus —
    // même nature, même règle. Sans lui, « [PNJ: Warp Agent] » désigne les
    // trente-huit du serveur à la fois.
    // kStyle : le code de style, tel qu'il a voyagé, et le pseudo de son auteur.
    // Le code est ce que `palette_cache::EncodeShare` produit — la même chaîne
    // que le presse-papiers.
    std::string style_code;
    std::string style_owner;
    // kUrl : l'adresse NUE. Séparée du texte parce que le texte affiché peut
    // emporter la ponctuation qui suit (« regarde https://… ! ») alors que
    // l'adresse ouverte, elle, doit s'arrêter avant.
    std::string url;
    // 🔴 L'objet d'un AUTRE joueur n'existe nulle part ailleurs : ni dans notre
    // sac, ni dans une liste de session. Tout ce qu'on saura jamais de lui (son
    // refine, ses cartes, son forgeron, ses options) est dans la balise, et c'est
    // donc ici qu'il faut le garder — sans quoi le nom perd ses préfixes et la
    // description ouvre l'item de BASE.
    itemcell::ChatLink item;

    // ── Emote Discord ─────────────────────────────────────────────────────────
    // Le relais transmet les emotes personnalisées telles que Discord les écrit :
    // `<:nom:id>`, ou `<a:nom:id>` pour une animée. Illisible en jeu — un message
    // qui n'est QUE ça ne veut rien dire du tout.
    //
    // 🔴 C'est le cas d'image le plus SÛR de tous : l'adresse est construite PAR
    // NOUS à partir du seul identifiant, vers le CDN de Discord. Le posteur ne
    // choisit ni l'hôte ni le chemin — rien de ce qui rend l'aperçu d'un lien
    // délicat ne s'applique ici.
    //
    // `text` porte le repli « :nom: », affiché tant que l'image n'est pas arrivée
    // (ou si le joueur a coupé les images) : lisible, et sans une seule requête.
    std::string emote_url;

    // ── Emote du JEU ──────────────────────────────────────────────────────────
    // L'index d'une emote de `emotion.act` — les bulles au-dessus des têtes,
    // réemployées ici DANS la ligne. -1 = ce fragment n'en est pas une.
    //
    // Rien à voir avec `emote_url` malgré la parenté d'usage : l'image sort du
    // GRF du joueur, donc aucune requête, aucune attente, et elle s'affiche même
    // hors ligne. `text` porte ici aussi le « :nom: » de repli — c'est d'ailleurs
    // exactement ce que lisent les joueurs sans Bourgeon, puisque c'est cette
    // forme-là qui part au serveur.
    int16_t game_emote = -1;
  };

  struct Line {
    std::vector<Run> runs;
    // 🔴 Le texte UTF-8 avec son BALISAGE intact (^RRGGBB, ^i[], <ITEML>). C'est
    // lui qu'on écrit dans l'historique persistant : le texte rendu (`plain`)
    // perdrait les couleurs et les liens d'objets, et `runs` serait un format de
    // fichier illisible et fragile.
    std::string      raw;
    std::string      plain;   // texte nu UTF-8 (recherche + copie)
    std::string      sender;  // UTF-8, extrait comme le natif pour {1,3,4,0x15}
    uint32_t         rgb = 0xFFFFFF;
    uint8_t          type = 0;
    char             source = 'A';  // 'A' = ChatAction, 'W' = WndProc natif
    // Ligne À NOUS, qu'aucun filtre de type ne concerne. Le repère de session
    // posé par `LoadHistory` en est le seul cas : il porte le type broadcast
    // pour paraître dans tous les onglets, et depuis que le broadcast a sa
    // propre case (t25), ce type ne suffit plus à le garantir — décocher les
    // annonces serveur effacerait le trait qui sépare hier d'aujourd'hui.
    //
    // Volontairement NON sérialisé : relu depuis l'historique, un repère n'est
    // plus un repère, juste une vieille ligne parmi les autres.
    bool             pinned = false;
    // Rang d'arrivée, strictement croissant, posé par `TrimLines` — le seul
    // endroit qui voie chaque ligne neuve une fois et une seule. C'est ce que
    // compare le « vider cet onglet » : un onglet retient le rang qu'il avait
    // atteint, et masque tout ce qui lui est antérieur. Le tampon, lui, n'est pas
    // touché — les autres onglets gardent ces lignes, et le geste s'annule.
    //
    // 🔴 Un numéro plutôt qu'une position : `TrimLines` évince des lignes AU
    // MILIEU du tampon, et `LoadHistory` en insère EN TÊTE. Un index deviendrait
    // faux au premier débordement, en désignant une ligne qui n'a rien à voir.
    uint64_t         seq = 0;
    // Conversation 1:1 à laquelle cette ligne appartient : le nom du CORRESPONDANT
    // (jamais le nôtre), en UTF-8, dans les deux sens de la conversation. Vide pour
    // tout le reste.
    //
    // 🔴 Il ne se déduit PAS de `sender` : à l'aller, le client construit
    // « ( To cible (aid) ) : texte » et `sender` porte donc cette parenthèse
    // entière, pas la cible. C'est le détour du pivot natif qui le pose, là où les
    // deux sens arrivent encore séparés et proprement typés.
    std::string      whisper_with;
    uint8_t          hour = 0, minute = 0, second = 0;
    // Hauteur du repli, mémorisée pour la largeur et les options d'affichage qui
    // l'ont produite. Elle permet de SAUTER une ligne hors écran sans remesurer
    // un seul mot : sans ça, un historique long se paie à chaque frame, y compris
    // pour ce que personne ne voit.
    float            cached_wrap = -1.0f;
    float            cached_height = 0.0f;
    uint8_t          cached_flags = 0xFF;  // horodatage + icônes (cf. DrawLines)
  };

  // Analyse une ligne brute (code-page client) en fragments prêts à dessiner.
  // C'est LE point d'entrée des autres surfaces : même parseur, donc mêmes
  // libellés de liens, mêmes couleurs, mêmes emotes que la chatbox.
  void ParseWireLine(const char* wire, Line* out) const;

 private:
  // Un canal, tel que le registre natif le décrit (nom + 25 octets de filtre).
  // `node` est l'adresse du nœud : les 25 PREMIÈRES cases d'options écrivent
  // DEDANS, comme le fait la fenêtre native 0x84 — le registre reste la source de
  // vérité. La 26e, elle, n'a pas d'octet là-bas (cf. `filter` plus bas).
  struct Channel {
    // 🔴 Identifiant STABLE, ENGENDRÉ PAR NOUS, jamais réutilisé. C'est lui qui
    // porte tout ce que nous attachons à un canal (réglages, état détaché,
    // historique). Ni l'index natif — dense, renuméroté à chaque suppression ou
    // détachement — ni le nom — libre, renommable, duplicable par le joueur — ne
    // peuvent tenir ce rôle : les deux échouent EN SILENCE, en transférant les
    // réglages d'un canal à un autre longtemps après le geste qui l'a causé.
    uint32_t    id = 0;
    int         index = 0;     // index natif COURANT, simple lien vers le registre
    std::string name;          // UTF-8
    bool        detached = false;
    // ── À QUELLE FENÊTRE ce canal appartient ────────────────────────────────
    // 0 = la fenêtre principale, celle qui porte la saisie. Toute autre valeur
    // désigne une flottante — et PLUSIEURS canaux peuvent partager la même :
    // c'est ce qui permet de regrouper des onglets détachés dans une seule
    // fenêtre, au lieu d'une fenêtre par canal.
    //
    // 🔴 L'identifiant est STABLE et jamais réutilisé, comme celui d'un canal :
    // la fenêtre ImGui est nommée avec lui, et le voir changer lui ferait perdre
    // sa position et sa taille. Il ne descend donc PAS de l'identifiant du canal
    // fondateur, qui peut partir ou se fermer alors que la fenêtre reste.
    //
    // 🔴 `detached` en est le simple REFLET (`group != 0`). Il reste parce que
    // vingt-cinq endroits le lisent, mais les deux champs se posent ENSEMBLE, par
    // `SetChannelGroup` — et par personne d'autre. Les laisser diverger, c'est un
    // canal dessiné dans une fenêtre et compté dans une autre.
    uint32_t    group = 0;
    // 🔴 `detached` vient du registre natif au départ, mais dès que le joueur
    // l'a changé chez nous, c'est NOTRE valeur qui fait foi. Sans ce drapeau, la
    // fusion périodique remettrait le canal là où le client le croit — deux
    // secondes après le geste, sans rien pour l'expliquer. À retirer le jour où
    // l'on déplacera vraiment l'entrée entre les deux registres natifs.
    bool        detach_owned = false;
    // Géométrie figée de SA fenêtre — n'a de sens que détaché (une conversation
    // 1:1 l'est aussi). 🔴 Le verrou est une propriété de la FENÊTRE, pas du
    // joueur : une fenêtre qu'on vient d'arracher n'hérite donc PAS de celui de
    // la fenêtre principale, sinon elle naîtrait immobile là où le joueur l'a
    // lâchée, sans rien pour le lui dire. Il se pose depuis le menu contextuel de
    // son en-tête.
    bool        locked = false;
    uintptr_t   node = 0;
    // 🔴 VINGT-SIX cases, alors que le nœud natif n'en porte que 25 : la dernière
    // (index 25 = le type broadcast, celui des annonces serveur) est À NOUS. Le
    // client ne filtre JAMAIS le broadcast — il n'a donc aucun octet où l'écrire,
    // et `WriteChannelFilter` s'arrête à 25 exprès. Le joueur, lui, veut pouvoir
    // taire les annonces dans l'onglet où il suit sa guilde, et c'est justement le
    // seul type qu'il ne pouvait pas décocher.
    //
    // ⚠ Le défaut d'un `Channel` neuf est ZÉRO, donc « tout coupé » : chaque site
    // de création remplit le tableau à 1 (`memset`), et la relecture d'une
    // disposition écrite AVANT cette case doit faire de même pour ce qu'elle n'y
    // trouve pas — sinon les annonces disparaîtraient d'un coup, sans un mot.
    uint8_t     filter[26] = {};
    // ── « Vider cet onglet » (clic droit sur l'onglet) ──────────────────────
    // Rang de la dernière ligne au moment du geste : cet onglet n'affiche plus
    // que ce qui est arrivé APRÈS. Zéro = rien n'a été vidé, et la règle ne
    // s'applique alors pas du tout — une ligne dont le rang n'aurait pas encore
    // été posé ne doit pas disparaître par accident.
    //
    // 🔴 Un MASQUE, pas une suppression : le tampon `lines_` est partagé par
    // tous les onglets, et en retirer les lignes d'un onglet les arracherait à
    // tous ceux qui les acceptent aussi. Vider « Guilde » ne doit rien coûter à
    // l'onglet qui montre tout. C'est aussi ce qui rend le geste réversible.
    //
    // ⚠ NON persisté, et il ne peut pas l'être : les rangs sont réattribués à
    // chaque session (cf. `Line::seq`), donc un rang relu d'un fichier ne
    // désignerait plus rien. Le vidage vaut pour la session en cours.
    uint64_t    clear_seq = 0;
    // Métriques de mise en page de SA fenêtre, mesurées à la frame précédente
    // (arrondi de la hauteur par rangées entières). Elles vivent sur le canal et
    // pas sur la classe : deux fenêtres n'ont ni la même hauteur de ligne ni le
    // même chrome.
    float       line_h = 0.0f;
    float       chrome_h = 0.0f;

    // ── Apparence PROPRE à ce canal ─────────────────────────────────────────
    // `style_own` faux = le canal suit les réglages généraux. Un drapeau plutôt
    // qu'une valeur sentinelle par champ : le joueur veut « comme les autres » ou
    // « à moi », et dans le premier cas ses onglets doivent SUIVRE un changement
    // du réglage général, pas en garder une copie figée.
    bool        style_own = false;
    int         font_pct  = 100;   // taille du texte du log
    int         padding   = 3;     // marge du cadre
    int         line_gap  = 2;     // interligne
    float       body[4]   = {0.0f, 0.0f, 0.0f, 0.588f};  // fond, format picker

    // ── Conversation 1:1 ────────────────────────────────────────────────────
    // Non vide = ce canal n'est pas un onglet mais une fenêtre de chuchotement
    // dédiée à ce correspondant. Il ne paraît JAMAIS dans la bande d'onglets, ne
    // vient d'aucun registre natif, n'accepte que les lignes qui le nomment, et
    // sa cible d'envoi est figée — le joueur ne peut pas s'y tromper de
    // destinataire, ce qui est tout l'intérêt d'une fenêtre séparée.
    std::string whisper_with;   // UTF-8, nom du correspondant
    std::string whisper_input;  // sa saisie à lui : chaque conversation garde la sienne
    // Guilde du correspondant, telle que le dictionnaire de noms du client la
    // connaît. ⚠ Peut rester VIDE pour toujours : le serveur ne répond aux
    // requêtes de nom que pour les unités de la même carte (rAthena,
    // clif_parse_GetCharNameRequest -> map_id2bl). Le titre s'en passe alors.
    std::string whisper_guild;
    uint32_t    whisper_aid = 0;
    bool        whisper_focus = false;  // rendre le focus à sa saisie
    // 🔴 AJOUT DIFFÉRÉ D'UNE FRAME, et il n'y a pas moyen de faire autrement.
    // `DrawWhisperInput` recopie `whisper_input` dans un tampon local AVANT de
    // dessiner le sélecteur, puis réécrit `whisper_input` depuis ce tampon
    // APRÈS : un emoji posé directement dans `whisper_input` depuis la grille
    // serait donc écrasé dans la même frame, sans laisser de trace. On le met
    // en attente ici, et la saisie le consomme au début de la frame suivante —
    // seize millisecondes, invisibles.
    std::string whisper_pending_insert;
    // La saisie transite par un tampon de cette taille dans `DrawWhisperInput`.
    // Tout ce qui l'écrit doit s'y tenir : au-delà, la copie tronque — et une
    // troncature tombant au milieu d'un caractère UTF-8 (un emoji en fait
    // quatre octets) laisserait une séquence invalide, donc un losange.
    static constexpr size_t kInputBufSize = 256;
    // Dernière activité (GetTickCount). Sert uniquement à choisir laquelle céder
    // sa place quand le plafond de fenêtres est atteint.
    uint32_t    whisper_stamp = 0;
  };

  // ── Canaux du SERVEUR (rAthena), poussés par ZC 0x0F21 ─────────────────────
  // 🔴 À ne pas confondre avec `Channel` juste au-dessus, qui est un ONGLET de la
  // chatbox. Ceux-ci sont les canaux de chat du serveur (`#global`, `#trade`, le
  // canal de carte…) : le client n'en connaît AUCUN par lui-même — ils vivent
  // dans conf/channels.conf, filtrés par groupe — et la seule autre façon de les
  // lire serait d'analyser le texte de « @channel list », qui est localisé.
  //
  // Parler dans un canal = chuchoter à « #nom » : c'est le routage de rAthena
  // (clif_parse_WisMessage, branche `target[0] == '#'`), qui rejoint même le
  // canal au passage s'il est sans mot de passe. La combo n'a donc qu'à écrire le
  // nom dans la box destinataire ; tout le reste est le chemin natif habituel.
  struct ServerChannel {
    std::string name;   // AVEC le '#' (le paquet l'omet, comme le serveur)
    std::string alias;  // libellé de channels.conf (« [Global] »), affiché en aide
    uint32_t    color = IM_COL32_WHITE;  // ImU32, déjà remis à l'endroit (le serveur stocke en BGR)
    bool        require_guild = false;  // #ally : rien à afficher hors guilde
    bool        can_chat = true;
  };
  std::vector<ServerChannel> server_channels_;

  // Valeurs EFFECTIVES : celles du canal s'il a les siennes, sinon les générales.
  // `channel` peut être nul (aucun canal lisible) — on rend alors les générales.
  int   EffFontPct(const Channel* channel) const;
  int   EffPadding(const Channel* channel) const;
  int   EffLineGap(const Channel* channel) const;
  const float* EffBody(const Channel* channel) const;

  // Ligne acceptée par le canal ? Rejoue exactement la règle du natif :
  // `filtre[type] != 0`, plus le broadcast qui passe partout (§3.1).
  bool ChannelAccepts(const Channel& channel, const Line& line) const;

  // 🔴 SOURCE UNIQUE du pas vertical entre deux lignes de log. Le dessin, le
  // calage du défilement et la contrainte de taille DOIVENT lire la même valeur :
  // ils en avaient chacun une, et les 6 px d'écart coupaient la dernière ligne
  // quelle que soit l'interligne choisie.
  // Taille de police du LOG, en pixels. Explicite partout où le log se dessine.
  // Le canal est passé partout : c'est LUI qui porte la taille depuis que chaque
  // onglet peut avoir la sienne.
  float LogFontSize(const Channel* channel) const;
  float LineHeight(const Channel* channel) const;
  // Débord du dernier glyphe sous son pas : le pas peut être PLUS COURT que la
  // hauteur du texte (interligne serré), auquel cas la dernière ligne dépasse la
  // hauteur réservée. C'est ce débord qu'il faut ajouter au contenu.
  float LineOverhang(const Channel* channel) const;

  // ── Une fenêtre par GROUPE, plus la principale qui porte la saisie ──────────
  ro::RoChatSkin MakeSkin(const Channel* channel) const;
  void ApplySizeConstraints(const ro::RoChatSkin& skin);
  void DrawDockedWindow();
  // ── Conversations 1:1 ──────────────────────────────────────────────────────
  // Index du canal 1:1 déjà ouvert pour ce correspondant, ou -1.
  // Rend cliquable le pseudo en tête de ligne, à partir du `sender` déjà
  // extrait. Après le parse : il découpe un fragment déjà analysé.
  void MarkSenderAsPlayerLink(Line* line) const;
  // Un chuchotement REÇU entre dans les destinataires récents : c'est le geste
  // qui suit, et la liste ne se remplissait qu'à l'envoi.
  void RememberWhisperPeer(const Line& line);

  int  FindWhisperChannel(const std::string& with_utf8) const;
  // Idem, en le créant au besoin. Rend -1 si la limite est atteinte.
  int  FindOrCreateWhisper(const std::string& with_utf8, uint32_t aid);
  // 🔴 Une conversation n'a plus de fenêtre à elle : elle vit dans un GROUPE, comme
  // un onglet, et peut donc partager sa fenêtre avec d'autres. Seule sa SAISIE lui
  // reste propre — destinataire figé, tampon à elle — et `DrawGroupWindow` ne la
  // dessine que quand l'onglet actif en est une.
  void DrawWhisperInput(int index);
  void QueueWhisperSend(Channel& channel);
  // Canal dont la fermeture a été demandée pendant la frame — la croix d'une
  // conversation, autrefois ; le clic molette sur un onglet, désormais. 🔴 On ne
  // peut pas le retirer sur place : le rendu parcourt `channels_` par indice, et
  // supprimer en cours de route décalerait tout ce qui suit. Purgé en FIN de frame.
  uint32_t close_channel_id_ = 0;
  // ── Confirmation avant fermeture ────────────────────────────────────────────
  // Canal dont la fermeture attend un OUI (0 = aucune). Les TROIS gestes qui
  // ferment — la croix d'une conversation, le clic molette sur un onglet,
  // l'entrée du menu contextuel — arment celui-ci au lieu de fermer ; seule la
  // modale pose ensuite `close_channel_id_`. Le clic molette surtout : c'est le
  // plus facile à faire sans le vouloir, en visant l'onglet d'à côté.
  uint32_t confirm_close_id_ = 0;
  // 🔴 L'OUVERTURE se demande, elle ne se déduit pas de `confirm_close_id_`.
  // `OpenPopup` prend l'identifiant de la fenêtre COURANTE : appelé depuis la
  // bande d'onglets, il ouvrirait un popup que le `BeginPopupModal` de la racine
  // ne retrouverait jamais. Le geste lève ce drapeau, la racine ouvre.
  //
  // Et le déduire de `confirm_close_id_ != 0` rouvrirait la modale à la frame
  // suivant un abandon par Échap — une modale qu'on ne peut plus refuser.
  bool     confirm_close_open_ = false;
  void     DrawCloseConfirmPopup();
  // Dernière interrogation du dictionnaire de noms, pour ne pas la refaire à
  // chaque tour de boucle d'entrées.
  uint32_t whisper_guild_stamp_ = 0;
  // Complète `whisper_guild` depuis le dictionnaire de noms du client. 🔴 Appelée
  // depuis OnProcessInput, JAMAIS pendant une frame : une entrée inconnue fait
  // ÉMETTRE au client une requête de nom.
  void ResolveWhisperGuilds();

  // ── Une fenêtre par GROUPE ──────────────────────────────────────────────────
  // Les flottantes, conversations 1:1 comprises. Le groupe 0 (la principale) garde
  // sa propre fonction : lui seul porte la ligne de saisie générale, les boutons
  // du client et la cible de recollage historique.
  //
  // 🔴 Une fenêtre est un GROUPE, pas un canal. C'est ce qui permet à plusieurs
  // onglets d'y cohabiter — et c'est pour ça que son identifiant ImGui descend du
  // groupe et non du canal, qui peut la quitter alors qu'elle reste.
  void DrawGroupWindow(uint32_t group);
  // La bande d'onglets d'un groupe : sélection, réordonnancement, arrachage, et
  // l'enregistrement du rectangle comme CIBLE DE DÉPÔT pour la frame.
  void DrawGroupStrip(uint32_t group);
  // Déplace un canal vers `group`, au rang `dest_slot` compté parmi les canaux DE
  // CE GROUPE tels qu'ils sont dessinés. `group` 0 = la fenêtre principale.
  void MoveChannelToGroup(int from, uint32_t group, int dest_slot);
  // Le canal actif d'un groupe (indice dans `channels_`), ou -1 s'il est vide.
  int  GroupActiveIndex(uint32_t group) const;

  // Bande d'onglets + boutons du client. Renvoie sa hauteur.
  float DrawTabStrip();
  // Ctrl + molette au-dessus d'une fenêtre de chat = zoom du texte du log. Cible
  // exactement ce que lit `EffFontPct` : l'onglet s'il a ses réglages propres,
  // les réglages généraux sinon. Sans effet si la fenêtre n'est pas survolée.
  void  HandleFontZoom(Channel& channel);
  // Jette les hauteurs de repli mémorisées par les lignes : elles valaient pour
  // l'ANCIENNE mise en page. À appeler après tout changement de `LineHeight`
  // (taille du texte, interligne) — voir le commentaire de la définition.
  void  InvalidateLineLayout();
  void  DrawChannel(const Channel& channel, float height);
  void  DrawLines(const Channel& channel);
  // Le sélecteur, DEUX onglets — et la séparation n'est pas cosmétique : ce qui
  // arrive au bout du fil n'est pas de même nature.
  //   · « Emotes » = les images du GRF. Elles voyagent en `:nom:` et le clic
  //     ENVOIE, seul (voir DrawGameEmoteGrid pour pourquoi).
  //   · « Emoji » = du TEXTE Unicode. Le clic INSÈRE dans la saisie, parce
  //     qu'un emoji a vocation à se mettre AU MILIEU d'une phrase.
  //
  // `whisper_index` désigne la conversation 1:1 depuis laquelle la grille est
  // ouverte, ou -1 pour la barre principale. Il ne sert pas qu'au destinataire :
  // c'est aussi lui qui dit à QUELLE saisie rendre le clavier après l'envoi.
  // 🔴 Le popup vit dans la fenêtre COURANTE (ImGui hache son identifiant avec la
  // pile de celle-ci), donc `DrawEmotePicker` doit être appelé dans la même
  // fenêtre que le bouton qui l'ouvre — sans quoi deux conversations ouvertes se
  // partageraient une grille et la mauvaise recevrait l'emote.
  //
  // `btn_min`/`btn_max` sont les coins du BOUTON qui ouvre la grille, en
  // coordonnées écran : c'est au-dessus de lui que le popup se pose, pour ne pas
  // recouvrir la saisie ni les dernières lignes du log.
  void  DrawEmotePicker(const ImVec2& btn_min, const ImVec2& btn_max,
                        int whisper_index = -1);
  void  DrawGameEmoteGrid(int whisper_index);
  void  DrawEmojiGrid(int whisper_index);
  // Ajoute du texte à la FIN de la saisie visée (barre principale ou
  // conversation), sans rien envoyer. Ne fait rien si le tampon est plein.
  void  AppendToInput(const char* utf8, int whisper_index);
  // `whisper_utf8` nul = le destinataire courant de la barre principale.
  bool  SendTextNow(const char* text, const char* whisper_utf8 = nullptr);
  void  DrawLogOptionsPopup();
  void  CreateChannel();
  void  CloseChannel(int index);
  // 🔴 LE SEUL ÉCRIVAIN de `Channel::group` et de son reflet `Channel::detached`.
  // `group` 0 = la fenêtre principale. Tout le reste du code lit l'un ou l'autre
  // sans avoir à se demander lequel fait foi.
  static void SetChannelGroup(Channel& channel, uint32_t group);
  // Un identifiant de fenêtre flottante, neuf et jamais revu. Voir `Channel::group`
  // pour pourquoi il ne peut pas être celui du canal qui la fonde.
  uint32_t NewGroupId() { return next_group_id_++; }
  // Combien de canaux vivent dans cette fenêtre ? Zéro = elle n'existe plus.
  int GroupSize(uint32_t group) const;

  // ── Persistance de la disposition (SaveData\bourgeon_chat.yaml) ─────────────
  // Ce que le joueur a construit — ses onglets, leurs noms, leur ordre, ce qui est
  // détaché, et les 25 filtres de chacun. Sans ce fichier, tout cela ne survit pas
  // à la reconnexion : le client relit SON `ChatWndInfo_U.lua`, qui décrit encore
  // l'ancienne disposition, et le travail du joueur disparaît sans un mot.
  void LoadLayout();
  void SaveLayout() const;
  // Historique conservé d'une session à l'autre (option, désactivée par défaut).
  void LoadHistory();
  void SaveHistory() const;

  // Écrit un fichier par onglet dans `<racine>\Chat\`. Rend le nombre de
  // fichiers écrits, 0 si rien n'était à écrire ou si le dossier a résisté.
  int SaveLogToFiles();
  // Armé par `RequestSaveLog`, consommé au rendu. `std::atomic` parce que les
  // deux bouts ne sont pas sur le même fil.
  std::atomic<bool> pending_save_log_{false};
  // ── Éviction : un quota PAR TYPE, pas un plafond unique ─────────────────────
  // 🔴 LE DÉFAUT QUE ÇA CORRIGE, rapporté par des joueurs : on entre en donjon, le
  // log de combat crache des centaines de lignes de dégâts, et les conversations
  // disparaissent de la fenêtre. Elles n'étaient pas filtrées — elles avaient été
  // ÉVINCÉES, poussées hors d'un tampon que tous les onglets partageaient.
  //
  // Le stockage reste commun, et il doit le rester : une même ligne s'affiche
  // légitimement dans plusieurs onglets à la fois (un chuchotement dans « Regular
  // Chat » ET dans tout onglet qui coche « Whisper »), un tampon par onglet la
  // dupliquerait. C'est l'ÉVICTION qui devient sélective : on garde les `cap`
  // dernières lignes de CHAQUE type. Une rafale de combat n'évince alors que du
  // combat, et la parole d'il y a dix minutes reste là.
  //
  // Un onglet montre l'UNION de plusieurs types ; lui garantir `cap` lignes par
  // type lui en garantit donc au moins autant qu'avant, jamais moins.
  void TrimLines();
  // Rang de la DERNIÈRE ligne entrée, ou zéro si le tampon est vide. C'est la
  // borne que pose « vider cet onglet » : tout ce qui existe à cet instant passe
  // derrière elle, et rien de ce qui arrivera ensuite n'est concerné. Prend le
  // verrou du tampon — à n'appeler que depuis un geste, jamais par frame.
  uint64_t LastLineSeq() const;
  // Combien de lignes de chaque type vivent dans `lines_`. Tenu à jour à chaque
  // entrée et à chaque éviction — recompter à la volée coûterait un parcours du
  // tampon par ligne reçue.
  // 🔴 Indexé par TYPE et non par canal, et c'est ce qui rend l'éviction sûre
  // depuis n'importe quel fil : elle ne lit jamais `channels_`, que le rendu
  // remanie (onglets déplacés, fermés, regroupés).
  // 25 types filtrables, plus UN panier pour tout le reste — le broadcast (0x19)
  // et les types qu'un serveur ajouterait sans nous prévenir.
  static constexpr int kTypeBuckets = 26;
  static int TypeBucket(uint8_t type) {
    return (type < kTypeBuckets - 1) ? static_cast<int>(type) : kTypeBuckets - 1;
  }
  size_t type_count_[kTypeBuckets] = {};
  // Combien de lignes du tampon sont déjà comptées ci-dessus. Le compte se fait
  // dans `TrimLines`, à partir de ce repère : les sites d'ingestion n'ont donc
  // rien à tenir, et rien ne peut diverger si l'un d'eux oublie un jour d'appeler.
  size_t counted_lines_ = 0;
  // Prochain rang à distribuer (cf. `Line::seq`). Il ne recule JAMAIS, pas même
  // après un vidage général : deux lignes de la session ne doivent pas pouvoir
  // porter le même rang, sinon un onglet vidé ferait réapparaître la seconde.
  uint64_t next_line_seq_ = 1;
  bool layout_dirty_ = false;  // une écriture est due (structure ou filtre modifié)
  // Quand le drapeau a été posé (GetTickCount), pour l'anti-rebond d'`OnTick`.
  uint32_t layout_dirty_ms_ = 0;
  // 🔴 TOUT MARQUAGE PASSE PAR ICI, jamais par `layout_dirty_` en direct : c'est
  // l'horodatage qui décide de la date d'écriture, et un site qui l'oublierait
  // ferait écrire la disposition au tout début d'un geste continu (le zoom à la
  // molette repose le drapeau à chaque cran).
  void MarkLayoutDirty();
  void  ParseText(const char* local_text, Line* out) const;
  void  ParseUtf8(const std::string& text, Line* out) const;
  // Remplit `select_buf_` avec le canal tel qu'il est à l'écran (filtre du canal +
  // recherche), en texte nu, pour le mode sélection. Ne reconstruit que si le
  // modèle ou le filtrage ont bougé.
  void RefreshSelectBuffer(const Channel& channel);
  // Arme l'envoi du contenu de la barre de saisie (joué par FlushPending).
  void QueueSend();

  // Relit `g_ChatChannelRegistry` + `g_ChatDetachedChannelRegistry`. Sans
  // registre lisible (avant l'entrée en jeu, par exemple) on garde un canal
  // « Public » qui accepte tout : la fenêtre n'est jamais vide de sa faute.
  void RefreshChannels();

  std::deque<Line> lines_;
  // `mutable` : un verrou ne fait pas partie de l'état LOGIQUE de l'objet. Sans
  // ça, `SaveHistory() const` ne pourrait pas le prendre — alors qu'elle ne fait
  // que LIRE les lignes, ce qui est précisément la raison d'être de son `const`.
  mutable std::mutex lines_mutex_;  // ingestion = fil du jeu, rendu = la frame
  std::vector<Channel> channels_;
  uint32_t         channels_stamp_ = 0;  // dernier rafraîchissement (GetTickCount)
  uint32_t         next_channel_id_ = 1; // 0 = « aucun canal », jamais attribué
  // Idem pour les FENÊTRES flottantes. 0 est réservé à la principale, donc le
  // compteur part de 1. Relevé au chargement pour ne jamais réattribuer l'id
  // d'une fenêtre qui vit encore.
  uint32_t         next_group_id_   = 1;
  // 🔴 Bascule d'AUTORITÉ. Tant qu'elle est fausse, le registre natif décrit la
  // liste des canaux et la fusion l'importe. Dès que le joueur y touche chez nous
  // — créer, fermer, renommer, détacher — c'est NOTRE liste qui fait foi, et la
  // fusion s'arrête net : sinon le registre ressusciterait deux secondes plus tard
  // l'onglet qu'il vient de fermer, et effacerait le nom qu'il vient de choisir.
  // Elle disparaîtra quand on écrira vraiment dans les deux registres natifs.
  bool             structure_owned_ = false;
  char             rename_buf_[32] = {};  // saisie du renommage, dans le menu
  uint32_t         rename_id_ = 0;        // canal dont `rename_buf_` porte le nom

  // Réglages.
  // Aimantation des chatbox entre elles et sur les bords de l'écran (l'aimant
  // lui-même vit dans ui/window_clamp.h — il ne concerne que les fenêtres qui se
  // déclarent, et à ce jour ce sont les nôtres). Coché par défaut : c'est ce que
  // font les fenêtres NATIVES du client, dont le gestionnaire d'adjacence range
  // déjà les siennes bord à bord.
  bool magnet_      = true;
  bool timestamps_  = false;
  bool item_icons_  = true;
  bool input_bar_   = true;
  // Mode DIAGNOSTIC : affiche tout ce qui est ingéré, sans filtre de canal, en
  // préfixant chaque ligne de son type. C'est la mesure qui tranche « la ligne
  // n'est pas arrivée » contre « la ligne est arrivée et le filtre l'a écartée » —
  // les deux se ressemblent à l'écran, et se corrigent à des endroits opposés.
  bool diagnostic_  = false;
  int  history_cap_ = 500;      // borné 100..5000
  // ⚠ Conserver l'historique entre deux sessions écrit les CHUCHOTEMENTS EN CLAIR
  // dans un fichier à côté du jeu. Désactivé par défaut, et l'infobulle le dit :
  // sur une machine partagée, ça se lit sans rien savoir faire.
  bool keep_history_  = false;
  int  keep_lines_    = 100;    // borné 20..1000
  // Deux échelles SÉPARÉES : celle du texte du log — le seul qu'on lise vraiment —
  // et celle de l'habillage (onglets, boutons, ligne de saisie). Les lier obligeait
  // à choisir entre un chat lisible et une bande d'onglets qui mange la fenêtre.
  int  font_scale_pct_ = 100;  // log
  int  ui_scale_pct_   = 100;  // habillage
  // Bornes de l'échelle du LOG, partagées par les curseurs (général et par
  // onglet) et par le zoom à la molette : trois sites qui doivent s'accorder,
  // sans quoi un geste pourrait poser une valeur que le curseur d'à côté refuse
  // d'afficher. Le pas est celui d'un cran de molette.
  static constexpr int kFontPctMin  = 70;
  static constexpr int kFontPctMax  = 160;
  static constexpr int kFontZoomStep = 5;
  // Jusqu'à quand montrer le pourcentage atteint par le zoom (`ImGui::GetTime`,
  // en secondes). Sans lui le geste est muet aux butées : le joueur continue de
  // tourner la molette sur un texte qui ne bouge plus, sans rien pour le dire.
  double zoom_hint_until_ = 0.0;
  // Taille de police de référence, relevée HORS de toute fenêtre au début de la
  // frame : c'est la seule qui ne porte l'échelle de personne. Tout le log se
  // dessine à partir d'elle, avec des tailles explicites — donc sans dépendre de
  // ce qu'une fenêtre enfant hérite ou non de sa parente.
  float base_font_size_ = 0.0f;
  // Verrouillage de la géométrie de la fenêtre PRINCIPALE : plus de déplacement
  // ni de redimensionnement. Les onglets, les menus et l'arrachage continuent de
  // fonctionner — c'est la GÉOMÉTRIE qui est figée, pas la fenêtre.
  //
  // 🔴 Il ne vaut QUE pour elle. Chaque fenêtre détachée porte le sien
  // (`Channel::locked`) : verrouiller le chat principal parce qu'on l'a bien placé
  // ne doit pas clouer au sol une flottante qu'on vient d'arracher. Les deux se
  // posent au même endroit — le menu contextuel de l'onglet ou de l'en-tête — et
  // se rangent dans le fichier de DISPOSITION, avec le reste de la géométrie,
  // plutôt que dans les réglages généraux.
  bool locked_         = false;
  bool url_confirm_    = true;   // opt-OUT : le garde-fou est là par défaut
  bool url_preview_    = false;  // opt-IN : rien n'est téléchargé sans accord
  int  font_family_    = 0;      // 0 = Système (cf. ro::ChatFamilyFont)
  bool thumbs_         = false;  // afficher les vignettes ? (opt-in)
  int  thumb_px_       = 48;     // hauteur, bornée 24..128
  // Arme le bouton d'export des emotes, au bas de la grille. Staff seulement, et
  // volontairement NON persisté : c'est une manipulation ponctuelle, pas un
  // réglage — chaque session repart désarmée.
  bool emote_export_   = false;
  std::string url_hosts_;        // hôtes autorisés par le joueur, « a.com;b.net »
  std::string url_hosts_seen_;   // dernière valeur poussée vers imgprev
  int  padding_px_     = 3;
  int  line_gap_px_    = 2;

  // Historique de saisie, rappelé aux flèches ↑/↓. Le natif fait exactement ça
  // dans `UIChatEditCtrl::OnMsg 0x0082b960` (msg 18 = haut, 19 = bas) : un
  // std::vector<std::string> à `edit+0x11C`, un index à `+0x140` qui va de 0 à
  // count INCLUS, et à `+0x128` le BROUILLON — le texte en cours de frappe, mis
  // de côté en entrant dans l'historique et rendu en ressortant par le bas.
  // Sa réserve d'origine est de 30 entrées ; on en garde 50, plus généreux.
  static constexpr int kInputHistoryMax = 50;
  std::vector<std::string> input_history_;   // du plus ancien au plus récent
  std::string input_draft_;                  // brouillon mis de côté (UTF-8)
  int  history_index_ = 0;                   // = size() ⇒ on est sur le brouillon
  void PushInputHistory(const char* utf8);
  // Rappel ↑/↓ : écrit dans le tampon d'ImGui via le callback d'historique.
  void RecallHistory(int direction, ImGuiInputTextCallbackData* data);

  // Destinataires de chuchotement récents — l'équivalent du bouton natif « Select
  // Receiver » (msg 0xE1), qui liste l'historique tenu par la box destinataire
  // (`UIWhisperNameEditCtrl`, vecteur `+0x11C`). Plus récent EN TÊTE : une liste
  // de rappel se lit du haut, et c'est le dernier interlocuteur qu'on redemande.
  static constexpr int kWhisperHistoryMax = 20;
  std::vector<std::string> whisper_history_;
  void PushWhisperHistory(const char* utf8);
  void DrawWhisperHistoryPopup();

  // ── Liens d'objets posés dans la saisie, en attente de résolution ───────────
  // `display` est ce que le joueur LIT dans la barre (UTF-8, nom décoré) ;
  // `wire` est le `<ITEML>…</ITEML>` qui partira à sa place. Un couple plutôt
  // qu'un id : le joueur peut effacer, déplacer ou dupliquer du texte entre la
  // pose et l'envoi, et seule la chaîne affichée permet de retrouver OÙ chaque
  // lien doit reprendre sa forme longue.
  //
  // Le plafond de trois est celui du natif (`0x00841b50` : `if (count >= 3)
  // return 0`), et ce n'est pas arbitraire — trois liens complets font déjà
  // ~180 octets sur les 255 d'un message.
  struct PendingLink {
    std::string display;
    std::string wire;
    // Même genre que dans `Run` : la pastille posée dans la saisie se clique comme
    // celle du log, donc elle doit savoir ce qu'elle désigne.
    uint8_t     kind = Run::kItem;
    uint32_t    mob_id = 0;
    uint8_t     mob_rank = 0;
    std::string mob_name;
    std::string setting_key;  // kSetting
    std::string style_code;   // kStyle : le code, tel qu'il partira
    std::string style_owner;  // kStyle : le pseudo affiché
    std::string navi_term;    // kNaviSearch : le terme, tel qu'il partira
    uint8_t     navi_kind = 0;
    std::string navi_map;     // … et sa carte de contexte, si elle existe
    // La balise RELUE, pour que le lien posé dans la saisie soit déjà un objet
    // cliquable — c'est ce que fait le natif, qui accroche un vrai bouton sur sa
    // ligne de saisie (`UIItemTagButton`) plutôt que d'y écrire du texte mort.
    itemcell::ChatLink item;
  };
  static constexpr int kMaxItemLinks = 3;
  std::vector<PendingLink> item_links_;
  // Rend le texte à ENVOYER : `input_` où chaque nom posé reprend sa forme
  // `<ITEML>`. Balayage de GAUCHE à DROITE en consommant les liens dans l'ordre
  // de pose — deux exemplaires du même objet donnent deux liens distincts, et
  // celui que le joueur a effacé est simplement sauté.
  // Non nul pendant le dessin d'un log DÉTOURNÉ : les `Append*Link` y écrivent au
  // lieu de `input_`. Remis à nul dès le retour — ce n'est PAS un état persistant.
  std::string* input_redirect_ = nullptr;
  // Oublie les liens dont le nom n'est plus dans la saisie : le joueur qui efface
  // un lien doit pouvoir en reposer un autre sans buter sur le plafond de trois.
  void PruneItemLinks();

  // ── La mécanique commune des NEUF `Append*Link` ─────────────────────────────
  // Chaque sorte de lien a sa propre matière — un objet, une recette, un monstre,
  // une destination de réglages… — mais toutes finissent par le MÊME geste :
  // élaguer, vérifier le quota, poser le libellé dans la saisie, mettre la balise
  // de côté pour la substitution à l'envoi, et rendre la main au clavier.
  //
  // 🔴 Ce geste était recopié dans HUIT d'entre elles. Le relevé de doublons n'en
  // appariait que DEUX — les six autres composent leur libellé et leur balise
  // différemment, ce qui les faisait passer sous le seuil de similarité.

  // Élague puis dit s'il reste de la place. `false` = la ligne porte déjà le
  // maximum de liens.
  bool LinkSlotAvailable();

  // Pose `pending.display` dans la saisie active et retient `pending` pour la
  // substitution à l'envoi. `false` si la barre refuse le texte — l'appelant n'a
  // alors rien laissé derrière lui.
  //
  // ⚠ Le focus est donné ICI, et ce n'est pas cosmétique : le geste vient souvent
  // d'une AUTRE fenêtre (inventaire, fiche de personnage), et sans ça le joueur
  // devrait encore cliquer dans la barre avant de pouvoir taper.
  bool PostPendingLink(PendingLink pending);

  // ── 🔴 PRÉVENIR IMGUI QU'ON A ÉCRIT DANS `input_` DANS SON DOS ──────────────
  // TANT QUE LE CHAMP EST ACTIF, IL ÉDITE SA PROPRE COPIE et la réécrit dans notre
  // buffer à chaque frame. Un lien posé pendant que la saisie a le focus est donc
  // effacé avant même d'être affiché : le premier lien passait (barre pas encore
  // focalisée), et plus aucun ensuite — puisque poser un lien DONNE le focus.
  //
  // C'est écrit tel quel dans imgui_internal.h : « If you modify underlying
  // user-passed buffer while active you need to call this ». Le projet le savait
  // déjà pour l'historique (flèche haut/bas), qui passe par un CALLBACK pour cette
  // raison exacte — les Append*Link, eux, écrivaient directement.
  //
  // À appeler après TOUTE écriture directe dans `input_`. Sans effet si le champ
  // n'est pas actif (GetInputTextState rend nullptr).
  void NotifyInputEdited();
  // L'id ImGui du champ de saisie, relevé à sa soumission. Mémorisé parce que les
  // Append*Link sont appelés depuis d'AUTRES fenêtres (l'inventaire, le panneau de
  // réglages) : on ne peut pas le recalculer là-bas, l'id se hache avec la pile de
  // la fenêtre courante.
  ImGuiID input_field_id_ = 0;
  // Le MÊME piège, pour la box « Pseudo » : `TargetWhisper` y écrit depuis un menu
  // contextuel, et le joueur peut très bien avoir laissé son curseur dedans — le
  // nom qu'on vient d'y poser serait alors écrasé par la copie interne du widget.
  void    NotifyWhisperEdited();
  ImGuiID whisper_field_id_ = 0;
  // ── Le clavier, rendu quand la palette d'emoji se REFERME ──────────────────
  // L'onglet « Emoji » ne se ferme pas au clic (on en pioche souvent plusieurs
  // d'affilée, contrairement à une emote qui part seule), donc on ne peut pas
  // rendre le focus au moment du clic : ce serait fermer le popup à la place du
  // joueur. On note qu'une pioche a eu lieu, ET DEPUIS QUELLE SAISIE, puis on
  // rend le clavier à celle-là quand le popup a disparu. Sans la cible, le
  // premier sélecteur venu — celui de la barre principale, dessiné à chaque
  // frame — consommerait la demande et volerait le focus à la conversation.
  bool picker_picked_        = false;
  int  picker_picked_target_ = -1;  // index de conversation, -1 = barre principale
  // ── Ce qu'il faut savoir de la grille HORS frame ────────────────────────────
  // `WantsEscapeKey`/`OnRawKey` sont appelés depuis le WndProc, donc ENTRE deux
  // frames : d'où un numéro de frame plutôt qu'un booléen à remettre à zéro, qui
  // demanderait un point de reset unique dans le rendu — et il n'y en a pas, la
  // grille étant dessinée par la barre principale ET par chaque conversation.
  int    picker_open_frame_ = -1;    // dernière frame où la grille était à l'écran
  bool   picker_close_      = false; // Échap reçu : elle se ferme au rendu suivant
  // Sa taille, mesurée à la frame précédente. Un popup s'auto-dimensionne, donc il
  // ne connaît la sienne qu'après avoir été dessiné une fois — et il en faut une
  // estimation AVANT, pour le poser au-dessus du bouton plutôt qu'en travers de la
  // barre de saisie.
  ImVec2 picker_size_{0.0f, 0.0f};
  bool   PickerOpen() const;
  // Repeint les liens POSÉS dans la ligne de saisie — crochets et couleur, comme
  // dans le log — et les rend cliquables, à l'image des boutons que le natif
  // accroche à la sienne. À appeler JUSTE APRÈS avoir soumis le champ, avec sa
  // géométrie et son état : un `InputText` n'a pas de texte riche, la couleur ne
  // s'obtient qu'en RECOUVRANT le fragment (fond du champ) pour le réécrire.
  void DrawInputLinkChips(const ImVec2& field_pos, float field_w, float field_h,
                          bool field_active, bool field_hovered);
  // Ce qu'un fragment DÉSIGNE, dans le vocabulaire commun des liens.
  links::Target TargetOf(const Run& run) const;
  links::Target TargetOf(const PendingLink& link) const;
  // Le lien visé par le menu contextuel ouvert : mis de côté au clic droit, le
  // popup s'ouvrant hors du child qui l'a demandé. Le menu lui-même est celui de
  // TOUT LE CLIENT (features/link_gesture.h) : la chatbox n'a pas le sien.
  //
  // 🔴 DEUX ancres et pas une : le log et la barre de saisie ont chacun leur
  // popup, et une ancre partagée ferait consommer le drapeau de l'un par le
  // `Draw` de l'autre — celui qui dessine en premier gagnerait.
  links::MenuAnchor link_menu_;   // le log
  links::MenuAnchor input_menu_;  // les pastilles de la barre de saisie

  char search_[64] = {};
  char input_[256] = {};        // ligne de saisie (UTF-8)
  char whisper_[32] = {};       // destinataire du chuchotement (UTF-8)
  bool focus_input_next_ = false;
  // TAB fait la navette entre la saisie et le champ « Pseudo ». ImGui ne le fait
  // pas tout seul : sa navigation au clavier est désactivée, et un InputText
  // n'insère pas la tabulation sans `AllowTabInput`. C'est donc à nous.
  bool focus_whisper_next_ = false;
  // Dernière frame où une des deux boxes de la ligne (saisie ou « Pseudo »)
  // portait le clavier. Un numéro de frame et pas un booléen, pour la même raison
  // que la grille d'emotes : `OwnsEnterKey` est lu depuis le WndProc, entre deux
  // frames, et il n'existe aucun point de reset unique dans le rendu.
  int  input_focus_frame_ = -1;
  // 🔴 La touche est RELEVÉE au clavier, la décision se prend au RENDU. C'est là
  // seulement que l'état d'ImGui est valide : savoir si une autre zone de texte a
  // déjà le focus (recherche, renommage, panneau de réglages) évite de la lui
  // voler à chaque Entrée.
  bool enter_pending_ = false;
  // Idem pour ÉCHAP, et pour la même raison : la barre peut être ouverte SANS
  // avoir le clavier, et il n'y a alors aucun widget pour voir la touche.
  bool escape_pending_ = false;
  // 🔴 Les caractères capturés à la frame N, rendus à ImGui à la frame N+1 — pas
  // écrits dans `input_`. La nuance est vitale : `SetKeyboardFocusHere` active le
  // champ par le chemin du TABULATEUR, qui SÉLECTIONNE TOUT dès que le tampon a
  // changé depuis la dernière fois (imgui_widgets.cpp 4894, `recycle_state`).
  // Écrire le caractère dans le tampon puis demander le focus, c'est donc le voir
  // sélectionné — et effacé par la frappe suivante. Rendu à `AddInputCharacter`
  // une frame plus tard, il s'insère au curseur comme n'importe quelle frappe.
  std::vector<ImWchar> typed_pending_;
  // ── Battle mode (`/bm`) ─────────────────────────────────────────────────────
  // Barre masquée. Les règles, et elles valent que la barre ait le clavier ou
  // non — c'est tout l'enjeu :
  //   • ENTRÉE, barre fermée      → ouvre, avec le clavier ;
  //   • ENTRÉE, ouverte et vide   → SORT, en une frappe ;
  //   • ENTRÉE, ouverte et pleine → ENVOIE, et garde la main ;
  //   • ÉCHAP, ouverte            → SORT, en une frappe ;
  //   • une LETTRE, ouverte       → s'écrit dans la barre, qui prend le clavier.
  // 🔴 La dernière ligne est ce qui rend les autres possibles : tant qu'Entrée
  // devait servir à reprendre le clavier, elle ne pouvait pas refermer, et une
  // barre ouverte sans focus devenait un piège — ni sortie ni écriture sans
  // aller cliquer dedans à la souris.
  //
  // ⛔ IL N'Y A PAS D'ÉTAT « TOUJOURS FOCALISÉE », ET IL NE PEUT PAS Y EN AVOIR.
  // Le chat natif ne rend jamais le clavier tant que sa barre est ouverte ; on a
  // essayé de tenir la même règle en reprenant le focus dès qu'il partait, et ça
  // a cassé TOUTES les interactions à la souris du client. La cause est le modèle
  // d'ImGui, pas notre code : tant qu'un widget détient l'`ActiveId`, plus rien
  // n'est survolable ailleurs (`ItemHoverable`, imgui.cpp 4982), la fenêtre visée
  // n'est même pas focalisée par le clic (8507), et le repli au double-clic d'une
  // barre de titre exige `g.ActiveId == 0` (7715). Résultat : premier clic mangé
  // partout, double-clic impossible. `ActiveIdAllowOverlap`, qui lèverait tout
  // ça, n'est réglable que depuis le glisser-déposer.
  //
  // Donc : la saisie prend le clavier sur un GESTE (Entrée, un clic dedans, un
  // envoi, un lien posé), et le rend comme n'importe quel champ ImGui. Ce que
  // l'invariant protégeait vraiment — sortir en une frappe — est tenu par Échap,
  // traité hors du champ.
  //
  // Conséquences ailleurs dans le code, à ne pas défaire :
  //   • tout ce qui pose `input_open_` pose AUSSI le focus (liens d'objet…) ;
  //   • l'action 3 de ChatAction n'ouvre RIEN (cf. ChatActionFilter) : le client
  //     l'émet à chaque annonce, et ouvrir en volant le clavier prendrait les
  //     touches de déplacement du joueur ;
  //   • les DEUX champs referment : Entrée depuis « Pseudo » vaut Entrée depuis
  //     la saisie, comme au natif.
  bool battle_mode_ = false;  // reflet de g_BattleModeOn, relu à chaque frame
  bool input_open_  = false;  // en battle mode uniquement : barre dépliée ?
  // Laquelle des deux boxes portait le clavier en dernier : c'est à celle-là
  // qu'on le rend après un envoi, une emote cliquée ou un lien posé. Le natif ne
  // déplace pas le curseur du joueur sous prétexte qu'on a cliqué un bouton.
  bool focus_on_whisper_ = false;
  // Lit le drapeau du CLIENT (`g_BattleModeOn 0x0131F50E`, persisté dans son
  // OptionInfo). On le lit plutôt que de suivre la commande : ainsi la valeur
  // restaurée à la connexion et toute bascule venue d'ailleurs nous parviennent.
  bool ReadNativeBattleMode() const;
  // La barre EXISTE-t-elle cette frame ? Pas forcément ici : quand un salon de
  // chat est ouvert, c'est lui qui la porte (cf. `chatwnd::DrawChatInputRow`).
  // Tout ce qui raisonne sur le CLAVIER — Entrée volée au jeu, frappes mises en
  // file, Échap — doit répondre « oui » dans ce cas-là aussi : la barre est bien
  // là, sous les doigts du joueur, simplement dans une autre fenêtre.
  //
  // 🔴 Et le salon l'affiche même repliée par le battle mode : dans un salon,
  // écrire EST l'activité. Une barre pliée y serait une impasse.
  bool InputRowVisible() const {
    return row_in_room_ || (input_bar_ && (!battle_mode_ || input_open_));
  }
  // La barre est-elle à dessiner ICI, dans la chatbox ? Le pendant du précédent,
  // pour les seuls sites de RENDU.
  bool DrawsInputRowHere() const {
    return !row_in_room_ && input_bar_ && (!battle_mode_ || input_open_);
  }
  // Un salon de chat porte-t-il la barre cette frame ? Relu UNE fois par frame,
  // en tête d'`OnRenderUI`, pour que la réponse ne dépende pas de l'ordre dans
  // lequel les deux fenêtres se dessinent.
  bool row_in_room_ = false;
  // Met notre état à jour quand le joueur tape `/bm` ou `/battlemode`. La commande
  // part AUSSI au client, qui garde son propre comportement clavier.
  void TrackBattleModeCommand(const char* utf8);
  // Destruction de la chatbox NATIVE quand la nôtre est active. Jouée hors frame
  // ImGui (depuis OnProcessInput), comme toute commande native.
  void SuppressNativeChat();
  int  active_channel_  = 0;
  // Canal que le popup d'options configure. Les 25 cases sont une propriété DU
  // CANAL, pas de la fenêtre : le clic droit sur un onglet règle CET onglet, même
  // s'il n'est pas celui qu'on lit. Sans cet index, le popup aurait configuré le
  // canal actif — donc pas celui que le joueur vient de désigner.
  int  logopt_channel_  = -1;

  // ── Geste d'arrachage, de regroupement et de recollage ──────────────────────
  // 🔴 UN SEUL GESTE, celui de l'ONGLET : tiré dans une autre bande il change de
  // fenêtre, tiré dans le vide il fonde la sienne, tiré dans la principale il y
  // revient. La fenêtre entière, elle, ne se glisse plus — c'est ImGui qui la
  // déplace par le vide de sa bande, et il le fait mieux que nous.
  int    drag_tab_       = -1;  // indice de l'onglet en cours de glissement
  ImVec2 strip_min_{};          // la bande de la PRINCIPALE, pour son propre test
  ImVec2 strip_max_{};
  // 🔴 TOUTES les bandes de la frame, une par fenêtre ouverte. Il en fallait une
  // seule tant qu'un onglet ne pouvait que revenir à la principale ; depuis qu'il
  // peut tomber sur N'IMPORTE QUELLE fenêtre, le lâcher doit savoir laquelle est
  // sous le curseur. Reconstruite à chaque frame — une fenêtre repliée ou fermée
  // ne doit pas rester une cible.
  struct StripRect {
    uint32_t group = 0;
    ImVec2   min{};
    ImVec2   max{};
  };
  std::vector<StripRect> strips_;
  // La cible désignée pour le lâcher en cours, remplie par la bande survolée au
  // moment où elle se dessine (c'est elle qui peint le trait d'insertion), et lue
  // APRÈS toutes les fenêtres. Le rang est compté parmi les onglets du groupe.
  bool     drop_valid_ = false;
  uint32_t drop_group_ = 0;
  int      drop_slot_  = -1;
  // Le canal actif de chaque FENÊTRE, par identifiant de groupe -> identifiant de
  // canal. Une entrée périmée est sans danger : elle ne correspond plus à rien et
  // le premier onglet du groupe reprend la main. Le groupe 0 n'est pas ici — c'est
  // `active_channel_` qui le porte, avec tout ce qui en dépend déjà.
  std::map<uint32_t, uint32_t> group_active_;
  // Position demandée pour la flottante qui vient d'être arrachée : elle doit
  // apparaître SOUS le curseur, là où le joueur l'a lâchée, et pas à l'endroit par
  // défaut d'ImGui. Un seul geste à la fois, donc un seul en attente.
  // 🔴 Indexée sur le GROUPE : c'est la fenêtre qu'on place, pas le canal.
  uint32_t pending_pos_id_ = 0;
  ImVec2   pending_pos_{};
  bool show_search_ = false;    // barre de recherche dépliée (bouton)
  // Mode sélection : le log passe en zone de texte lecture seule, sélectionnable
  // à la souris et copiable au Ctrl+C. Volontairement NON persisté : c'est un
  // geste ponctuel (« je veux coller ces trois lignes »), pas une préférence.
  bool        select_mode_ = false;
  std::string select_buf_;              // texte nu servi à InputTextMultiline
  uint32_t    select_key_  = 0xFFFFFFFFu;  // signature du dernier remplissage

  // Envoi armé pendant le rendu, joué par FlushPending (hors frame ImGui). Les
  // deux chaînes sont déjà dans la code-page du FIL, prêtes pour le natif.
  std::string pending_text_;
  std::string pending_whisper_;
  bool        has_pending_ = false;
  // 🔴 Une ligne TAPÉE et une macro ne routent pas pareil : seule la première
  // lit les préfixes et les touches (cf. SendToggles). Les touches sont relevées
  // à la VALIDATION et voyagent ici, parce que FlushPending tourne une frame
  // plus tard — le temps de relâcher Ctrl et de changer de canal sans le vouloir.
  bool        pending_typed_ = false;
  SendToggles pending_toggles_;
  // Action « par nom » en attente (cf. QueueNameAction). Une seule à la fois : ce
  // sont des gestes de menu, et il n'en part qu'un par clic.
  std::string pending_name_;
  NameAction  pending_name_action_ = NameAction::kNone;
  void FlushNameAction();
};

namespace chatwnd {
// Ingestion depuis le `case 0x25` du WndProc natif, appelée par le hook que
// ChatTweaks pose déjà sur `UINewChatWnd_WndProc` (features/patches/chat.cc).
//
// 🔴 POURQUOI DEUX SOURCES. `ChatAction` est le point d'ENTRÉE public, mais il
// n'est pas le seul chemin qui aboutit au chat : des lignes que le natif affiche
// n'y passent pas (constaté en jeu — la fenêtre ImGui en recevait une partie
// seulement). Le `case 0x25` du WndProc, lui, est le point où le natif dépose
// RÉELLEMENT ses lignes : `UISubChatWnd_AddLine` n'a que trois appelants, et
// celui-ci est le sien. S'y brancher donne la parité par construction.
//
// Les deux sources ne peuvent pas faire doublon : tant que la fenêtre native
// existe, c'est ELLE qui alimente (le WndProc tourne) ; quand elle n'existera
// plus — la bascule de la phase 2 — le WndProc ne tournera plus du tout et
// `ChatAction` reprend seul, ce qui est précisément son rôle documenté.
void IngestNativeLine(const char* text, uint32_t rgb, int type,
                      const char* sender);

// ── TROISIÈME source : nos propres lignes ────────────────────────────────────
// `UIM_PUSHINTOCHATHISTORY` — la voie par laquelle Bourgeon écrit dans le chat
// (relais Discord, DPS meter) — n'atteint NI `ChatAction` NI, une fois la native
// détruite, quoi que ce soit d'affiché. C'était l'angle mort de la chatbox ImGui,
// et il ne contenait que nos propres sorties, d'où le temps qu'il a mis à se voir.
//
// Renvoie true si la ligne a été prise : l'appelant ne doit alors PAS la passer au
// natif (sans fenêtre pour la consommer, elle s'empile dans `mgr+0x4C4`, jamais
// drainée). Renvoie false tant que la native vit — c'est son WndProc qui alimente.
bool IngestPluginLine(const char* text, uint32_t rgb);

// ── QUATRIÈME source : le SALON DE CHAT ──────────────────────────────────────
// `ChatRoomWindow` remplace la fenêtre native du salon (id 28), mais son log doit
// dire exactement ce que dit le nôtre : balises `<ITEML>`, liens d'objets, de
// monstres, de cartes, icônes `^i[]`, emotes du jeu et de Discord, gras/italique,
// couleurs `^RRGGBB`. Recopier ce rendu en aurait fait une SIXIÈME copie — le
// travers que `project_link_label_widget_todo` demande justement d'arrêter.
//
// 🔴 Ces lignes empruntent donc la machinerie des CONVERSATIONS 1:1, qui existe
// déjà et qui fait exactement ce qu'il faut : une ligne portant un
// `whisper_with` n'entre dans AUCUN onglet du journal, et ne s'affiche que dans
// la fenêtre qui porte le même. Le salon en devient une, sous un tag réservé
// qu'aucun nom de personnage ne peut porter.
//
// Ce qu'on gagne au passage, sans une ligne de plus : le repli, le cache de
// hauteurs, la recherche, les clics sur les liens, l'historique persistant.
bool IngestChatRoomLine(const char* local_text, uint32_t rgb);

// Dessine le log du salon DANS le conteneur ImGui courant (à appeler entre un
// BeginChild et son End). Le canal est synthétique : il ne figure pas dans la
// liste d'onglets et n'a donc ni case, ni réglage propre — il reprend ceux du
// canal principal, que le joueur a déjà réglés une fois.
// `link_insert_target` reçoit le texte des liens qu'on Maj+clique pendant ce
// dessin — la saisie du SALON, donc, et pas la barre du chat général. Peut être
// nul : les liens repartent alors vers la barre principale.
void DrawChatRoomLog(std::string* link_insert_target);

// Retraduit les libellés de liens (`<Nom d'objet>`, `[Style: X]`…) en BALISES du
// fil. À appeler juste avant d'envoyer une ligne qui vient d'une saisie AUTRE que
// la barre principale : la table des liens en attente est globale à la chatbox,
// c'est elle qui sait ce que chaque libellé désigne.
std::string ResolveOutgoingLinks(const char* utf8);

// ── La BARRE DE SAISIE, prêtée au salon ──────────────────────────────────────
// Dessine la ligne de saisie de la chatbox — box destinataire, mode d'envoi,
// sélecteur d'emotes, champ, chips de liens, historique — DANS la fenêtre ImGui
// courante, à appeler tout en bas de celle-ci.
//
// 🔴 Ce n'est pas une copie : c'est LA barre, déplacée le temps qu'un salon soit
// ouvert. Tant qu'on est dans un salon, c'est le seul endroit où l'on peut
// parler — le serveur route un message ordinaire vers le salon dès que
// `sd->chatID` est posé (clif.cpp, `clif_parse_GlobalMessage` :
// `sd->chatID ? CHAT_WOS : AREA_CHAT_WOC`). Une saisie propre au salon aurait
// donc été une DEUXIÈME boîte faisant exactement la même chose, avec ses liens,
// son historique et ses préfixes à re-câbler un par un.
//
// La chatbox, elle, cesse de la dessiner pendant ce temps : un `InputText` peint
// deux fois sous le même identifiant se disputerait le clavier avec lui-même.
//
// Renvoie false — et ne dessine rien — si la chatbox moderne est éteinte :
// l'appelant doit alors le dire, sinon la fenêtre n'a plus de saisie du tout.
bool DrawChatInputRow();

// Écrit un nom dans la box destinataire de la barre : la suite part en
// chuchotement. C'est ce que fait le bouton « Select Receiver » du chat natif, et
// c'est par là que la liste des membres d'un salon chuchote à l'un d'eux.
bool TargetWhisper(const char* name_wire);

// Oublie les lignes du salon. Appelé à l'entrée dans un salon et à la sortie :
// un salon n'est pas une conversation qui se poursuit, et retrouver les lignes du
// précédent en ouvrant le suivant n'aurait aucun sens.
void ClearChatRoomLog();

// Libellé d'un des 25 types de message, plus le broadcast 0x19 (enum §3.1.1 de
// la doc). Volontairement en anglais : ce sont les libellés du client
// (msgstringtable), ceux que les joueurs lisent dans la fenêtre native d'options
// de log. Le broadcast n'y figure pas — il n'y était pas filtrable — et reprend
// le mot du jeu, « Broadcast ».
const char* TypeLabel(int type);
}  // namespace chatwnd
