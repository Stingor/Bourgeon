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
#include <deque>
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

  // « Déplie la barre de saisie », SANS lui donner le clavier : la traduction de
  // l'action 3 de ChatAction (msg 0x10 au natif = dérouler la barre repliée).
  // 🔴 Le focus n'a rien à faire ici. Le client émet cette action à tout bout de
  // champ — le handler d'annonce `sub_00CB7510` l'appelle (0x00cb7957) juste avant
  // ses deux add-lines, comme des dizaines de handlers de paquets : donner le
  // clavier à chacun, c'est voler ses touches de déplacement au joueur à chaque
  // annonce du serveur. `/bm`, lui, garde la main tout seul (l'envoi du texte
  // repose déjà le focus sur la saisie).
  void RequestInputBarDeploy() { deploy_pending_ = true; }

  // Interrupteur de la fenêtre (persisté par MoonlightUi, clé « chatwnd_imgui »).
  // Public comme chez les autres fenêtres modernes : c'est la table de réglages
  // qui l'écrit (MLUI_FIELD).
  bool imgui_enabled_ = false;

  // ── Réglages (lus/écrits par la table de persistance de MoonlightUi) ──
  bool& timestamps()   { return timestamps_; }
  bool& item_icons()   { return item_icons_; }
  bool& input_bar()    { return input_bar_; }
  bool& diagnostic()   { return diagnostic_; }
  int&  history_cap()  { return history_cap_; }
  bool& keep_history() { return keep_history_; }
  int&  keep_lines()   { return keep_lines_; }
  bool& locked()          { return locked_; }          // géométrie figée
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
  // ajoutent une ligne (1 et 0x13). `text`/`sender` sont les chaînes du client,
  // déjà recopiées dans des tampons à nous : rien n'est retenu d'elles.
  //
  // `source` marque QUI a fourni la ligne : 'A' = le détour de `ChatAction`,
  // 'W' = le `case 0x25` du WndProc natif. Il n'est affiché qu'en mode
  // diagnostic, et il est là pour une raison précise : « la ligne arrive en
  // double » a exactement deux explications — nos deux sources qui ingèrent la
  // même ligne, ou le serveur qui l'envoie deux fois — et elles se corrigent à
  // des endroits opposés. Deux `A` (ou deux `W`) accusent le serveur ; un `A` et
  // un `W` nous accusent.
  void Ingest(const char* text, uint32_t rgb, const char* sender, int type,
              char source);

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
  bool IngestWhisper(const char* with_wire, const char* text_wire, uint32_t rgb,
                     uint32_t aid);

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

  // Vide l'historique (bouton du panneau, changement de personnage).
  void ClearHistory();

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
  bool AppendItemLinkFromLink(const itemcell::ChatLink& link);

  // Arme une commande (`@iteminfo`, `@mobinfo`…) pour le prochain FlushPending.
  // Même canal que l'envoi d'une ligne tapée — donc le pipeline COMPLET du
  // client. 🔴 Publique parce que le menu des liens (features/link_gesture.cc)
  // s'en sert : une commande ne doit JAMAIS partir pendant une frame ImGui.
  void QueueCommand(const char* utf8);

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

 private:
  // ── Le modèle ──────────────────────────────────────────────────────────────
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
    enum LinkKind : uint8_t { kNone = 0, kItem, kMob, kUrl, kPlayer };
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

  // Un canal, tel que le registre natif le décrit (nom + 25 octets de filtre).
  // `node` est l'adresse du nœud : les 25 cases d'options écrivent DEDANS, comme
  // le fait la fenêtre native 0x84 — le registre reste la source de vérité.
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
    // 🔴 `detached` vient du registre natif au départ, mais dès que le joueur
    // l'a changé chez nous, c'est NOTRE valeur qui fait foi. Sans ce drapeau, la
    // fusion périodique remettrait le canal là où le client le croit — deux
    // secondes après le geste, sans rien pour l'expliquer. À retirer le jour où
    // l'on déplacera vraiment l'entrée entre les deux registres natifs.
    bool        detach_owned = false;
    uintptr_t   node = 0;
    uint8_t     filter[25] = {};
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

  // ── Une fenêtre par canal détaché, plus la dockée qui porte les onglets ──────
  ro::RoChatSkin MakeSkin(const Channel* channel) const;
  void ApplySizeConstraints(const ro::RoChatSkin& skin);
  void DrawDockedWindow();
  void DrawDetachedWindow(int index);
  void DrawDetachedHeader(int index);
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
  void DrawWhisperWindow(int index);
  void DrawWhisperHeader(int index);
  void DrawWhisperInput(int index);
  void QueueWhisperSend(Channel& channel);
  // Conversation dont la croix a été cliquée pendant la frame. 🔴 On ne peut pas
  // la retirer sur place : le rendu parcourt `channels_` par indice, et supprimer
  // en cours de route décalerait tout ce qui suit. Purgée en FIN de frame.
  uint32_t whisper_close_id_ = 0;
  // Dernière interrogation du dictionnaire de noms, pour ne pas la refaire à
  // chaque tour de boucle d'entrées.
  uint32_t whisper_guild_stamp_ = 0;
  // Complète `whisper_guild` depuis le dictionnaire de noms du client. 🔴 Appelée
  // depuis OnProcessInput, JAMAIS pendant une frame : une entrée inconnue fait
  // ÉMETTRE au client une requête de nom.
  void ResolveWhisperGuilds();

  // Bande d'onglets + boutons du client. Renvoie sa hauteur.
  float DrawTabStrip();
  void  DrawChannel(const Channel& channel, float height);
  void  DrawLines(const Channel& channel);
  void  DrawInputRow();
  // Grille des emotes du jeu, et l'écriture de son choix dans la saisie.
  void  DrawEmotePicker();
  void  InsertIntoInput(const char* text);
  void  DrawLogOptionsPopup();
  void  CreateChannel();
  void  CloseChannel(int index);

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
  bool layout_dirty_ = false;  // une écriture est due (structure ou filtre modifié)
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
  // Taille de police de référence, relevée HORS de toute fenêtre au début de la
  // frame : c'est la seule qui ne porte l'échelle de personne. Tout le log se
  // dessine à partir d'elle, avec des tailles explicites — donc sans dépendre de
  // ce qu'une fenêtre enfant hérite ou non de sa parente.
  float base_font_size_ = 0.0f;
  // Verrouillage de la géométrie : plus de déplacement ni de redimensionnement,
  // ni pour la fenêtre dockée ni pour les flottantes. Les onglets, les menus et
  // l'arrachage continuent de fonctionner — c'est la GÉOMÉTRIE qui est figée, pas
  // la fenêtre.
  bool locked_         = false;
  bool url_confirm_    = true;   // opt-OUT : le garde-fou est là par défaut
  bool url_preview_    = false;  // opt-IN : rien n'est téléchargé sans accord
  int  font_family_    = 0;      // 0 = Système (cf. ro::ChatFamilyFont)
  bool thumbs_         = false;  // afficher les vignettes ? (opt-in)
  int  thumb_px_       = 48;     // hauteur, bornée 24..128
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
  std::string ResolveItemLinks(const char* utf8) const;
  // Oublie les liens dont le nom n'est plus dans la saisie : le joueur qui efface
  // un lien doit pouvoir en reposer un autre sans buter sur le plafond de trois.
  void PruneItemLinks();
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
  // Le lien visé par le menu contextuel ouvert. Il est mis de côté au clic droit
  // et survit jusqu'à la frame suivante — c'est là que le popup s'ouvre. Le menu
  // lui-même est celui de TOUT LE CLIENT (features/link_gesture.h) : la chatbox
  // n'a pas son menu à elle.
  links::Target link_menu_;

  char search_[64] = {};
  char input_[256] = {};        // ligne de saisie (UTF-8)
  char whisper_[32] = {};       // destinataire du chuchotement (UTF-8)
  bool focus_input_next_ = false;
  // TAB fait la navette entre la saisie et le champ « Pseudo ». ImGui ne le fait
  // pas tout seul : sa navigation au clavier est désactivée, et un InputText
  // n'insère pas la tabulation sans `AllowTabInput`. C'est donc à nous.
  bool focus_whisper_next_ = false;
  // 🔴 La touche est RELEVÉE au clavier, la décision se prend au RENDU. C'est là
  // seulement que l'état d'ImGui est valide : savoir si une autre zone de texte a
  // déjà le focus (recherche, renommage, panneau de réglages) évite de la lui
  // voler à chaque Entrée.
  bool enter_pending_ = false;
  // Idem pour l'action 3 de ChatAction : déplier la barre, sans toucher au focus.
  bool deploy_pending_ = false;
  // ── Battle mode (`/bm`) ─────────────────────────────────────────────────────
  // Barre masquée ; Entrée l'ouvre et lui donne le focus ; Échap, un clic ailleurs
  // ou un envoi À VIDE la referment. Ce dernier est le vrai confort : on sort du
  // chat sans lâcher le clavier, en deux Entrée.
  bool battle_mode_ = false;  // reflet de g_BattleModeOn, relu à chaque frame
  bool input_open_  = false;  // en battle mode uniquement : barre dépliée ?
  // Lit le drapeau du CLIENT (`g_BattleModeOn 0x0131F50E`, persisté dans son
  // OptionInfo). On le lit plutôt que de suivre la commande : ainsi la valeur
  // restaurée à la connexion et toute bascule venue d'ailleurs nous parviennent.
  bool ReadNativeBattleMode() const;
  // La barre est-elle à dessiner cette frame ?
  bool InputRowVisible() const {
    return input_bar_ && (!battle_mode_ || input_open_);
  }
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

  // ── Geste d'arrachage / de recollage ────────────────────────────────────────
  // Un onglet tiré hors de la bande devient une fenêtre ; une fenêtre lâchée sur
  // la bande y revient. La bande est mémorisée à chaque frame parce que c'est la
  // FENÊTRE DÉTACHÉE qui doit savoir où elle est — et elle est dessinée après.
  int    drag_tab_       = -1;  // indice de l'onglet en cours d'arrachage
  int    drag_detached_  = -1;  // indice de la flottante en cours de déplacement
  ImVec2 strip_min_{};
  ImVec2 strip_max_{};
  bool   strip_valid_    = false;
  // Position demandée pour la flottante qui vient d'être arrachée : elle doit
  // apparaître SOUS le curseur, là où le joueur l'a lâchée, et pas à l'endroit par
  // défaut d'ImGui. Un seul geste à la fois, donc un seul en attente.
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

// Libellé d'un des 25 types de message (enum §3.1.1 de la doc). Volontairement
// en anglais : ce sont les libellés du client (msgstringtable), ceux que les
// joueurs lisent dans la fenêtre native d'options de log.
const char* TypeLabel(int type);
}  // namespace chatwnd
