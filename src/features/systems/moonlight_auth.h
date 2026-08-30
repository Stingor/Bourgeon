#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "features/plugin.h"

class AutoLogin;

// MoonlightAuth — écran d'authentification ImGui moderne « compte Moonlight ».
//
// Objectif : au lieu de saisir les identifiants d'un compte Ragnarok, le joueur
// se connecte avec son compte web Moonlight (forum phpBB) et CHOISIT lequel de
// ses comptes RO liés utiliser. Le regroupement « 1 compte web -> N comptes RO »
// existe déjà en base (rathena.login.user_id = phpbb_users.user_id) ; ce plugin
// en est le front client.
//
// Flux (voir docs/login_auth_imgui_design.md) :
//   1. Formulaire ImGui : identifiant + mot de passe Moonlight.
//   2. POST HTTPS action=auth -> le site valide et renvoie la liste des comptes
//      RO liés + un ticket court.
//   3. Sélecteur ImGui : le joueur choisit un compte.
//   4. POST HTTPS action=select -> le site pose un mot de passe éphémère (OTP)
//      MD5 sur ce compte et renvoie l'OTP en clair.
//   5. On délègue à AutoLogin::DriveWithCredentials(userid, otp) : la séquence
//      native éprouvée (CA_LOGIN classique -> char-server -> char-select).
//
// Le rendu se fait dans OnRenderLoginUI (uniquement sur l'écran login/char-select).
// L'HTTP tourne sur un thread worker (jamais bloquant sur le thread de rendu) ;
// l'UI poll le résultat. Pendant l'étape de pilotage (kDriveLogin) on ne dessine
// AUCUNE fenêtre ImGui focusable, sinon le hook WndProc avalerait les WM_CHAR
// synthétiques avant les champs natifs.
//
// Configuration (bourgeon_settings.yaml, section `moonlight_auth:`) :
//   base_url:   https://…            (REQUIS ; vide => plugin inerte, login natif)
//   endpoint:   /api/game_login.php  (défaut)
//   enabled:    true                 (défaut si base_url présent)
//   save_id:    false                (état de la case « Save ID » native)
//   remember:   true                 (mémoriser l'identifiant web, jamais l'OTP)
//   web_user:   <dernier identifiant> (rempli automatiquement si remember)
class MoonlightAuth : public Plugin {
 public:
  explicit MoonlightAuth(AutoLogin* auto_login);
  ~MoonlightAuth() override;

  const char* name() const override { return "Moonlight Auth"; }

  void OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) override;
  void OnRenderLoginUI() override;

  bool enabled() const { return enabled_; }

  // Le joueur a choisi « Login classique » pour cette session : les écrans
  // NATIFS redeviennent les siens, et notre formulaire se tait. Lu par le décor
  // de connexion, qui ne doit surtout pas s'armer dans ce cas — il remplacerait
  // ces écrans natifs par une ville sans champ de saisie.
  bool NativeFallback() const { return native_fallback_; }

  // Un login est en cours de pilotage — y compris le PASSTHROUGH d'un retour au
  // char-select depuis le jeu, où cet état est repris tel quel sans qu'aucun
  // login ne reparte (cf. OnModeSwitch).
  //
  // 🔴 Le décor de connexion doit s'y refuser : dans cet état, ce module demande
  // `spectator::Leave()` à chaque frame, et un décor qui se réarme en face
  // rouvre une session que l'autre referme aussitôt. Mesuré : une boucle
  // entrée/sortie complète toutes les trois secondes, qui a fini par faire
  // planter le client dans la construction du char-select.
  bool IsDrivingLogin() const { return state_ == State::kDriveLogin; }

  // Un login est EN COURS de pilotage — la séquence tourne vraiment, le
  // char-select n'est pas encore atteint, et un voile couvre l'écran.
  //
  // 🔴 À distinguer d'`IsDrivingLogin()`, qui est vrai aussi pour le PASSTHROUGH
  // d'un retour au char-select depuis le jeu. Là, le joueur est bel et bien
  // devant son char-select et doit pouvoir y taper — confisquer le clavier
  // l'empêcherait de nommer un personnage.
  bool IsDrivingLoginActive() const {
    return state_ == State::kDriveLogin && !charsel_reached_;
  }

  // L'adresse du SITE, sans '/' final. Vide si la config l'a effacée.
  const std::string& base_url() const { return base_url_; }

  // Vrai si CETTE session de login a suivi la voie Moonlight (web login + choix
  // de compte), par opposition au repli « Login classique » (champs natifs). Sert
  // à réserver le char-select ImGui au parcours Moonlight : login natif => UI
  // native de bout en bout. Réarmé à chaque retour sur l'écran de login.
  bool DroveMoonlightLogin() const { return drove_moonlight_login_; }

  // Le hook de WndProc consulte ceci pour confisquer LE CLAVIER au client NATIF
  // pendant tout le parcours de login moderne (formulaire web, choix du compte,
  // pilotage du login, char-select). Il ne le rend qu'en SORTANT du parcours
  // (repli « Login classique », écran natif assumé — cf.
  // CharSelect::NativeScreenHasKeyboard) ou une fois en jeu.
  //
  // ⚠ C'est un PRÉDICAT et non `io.WantCaptureKeyboard` : la capture ImGui est un
  // état de rendu, or pendant le pilotage du login on ne dessine RIEN, et une
  // modale native lance sa propre pompe de messages — le rendu s'arrête
  // exactement quand la touche est le plus dangereuse, et la capture resterait
  // figée sur sa dernière valeur.
  //
  // 🔴 Sans cette confiscation, un joueur qui martèle Entrée agit sur des écrans
  // natifs qu'il ne voit même pas : les deux touches déclenchent le bouton par
  // défaut de la fenêtre RO prioritaire, et la visibilité n'entre JAMAIS en ligne
  // de compte (UIWindowMgr_OnKeyDown 0x00A471E0 -> OnMsg 6/+0x8C). Constaté :
  // entrée en jeu sur le personnage du premier slot, ou ouverture de la fenêtre
  // native de création, pendant le court instant où le char-select natif est
  // découvert ; et, sur l'écran de login masqué, une msgbox modale bloquante
  // (UILoginWnd_OnMsg case 186 exige id>=4 et mot de passe>=6 caractères).
  //
  // Nos PROPRES frappes (auto-confirm du char-server) ne sont pas concernées :
  // elles passent par RagnarokClient::PostGameKey, que le hook reconnaît.
  bool WantsKeyboard() const;

  // Ramène le flux au formulaire de login web (état kWebLogin) et oublie la session
  // authentifiée. À appeler quand on RETOURNE à l'écran de connexion sans changer de
  // MODE — c'est le cas du bouton « Revenir au login » du char-select (commande de
  // mode 10011 : le client reste en CLoginMode, donc OnModeSwitch ne repasse PAS et
  // le plugin resterait bloqué en kDriveLogin, laissant l'écran de login NATIF).
  // No-op si le plugin est désactivé.
  // `service_select_pending` : true = le natif va reconstruire l'écran de choix de
  // connexion (<connection> du clientinfo), il faudra donc le repasser ; false = on
  // arrive directement sur la fenêtre de login (cas du retour depuis le char-select,
  // état 3), aucune sélection à tirer.
  void RearmWebLogin(bool service_select_pending = true);

  // Résultat d'une requête HTTP, publié par le thread worker sous verrou. Public
  // pour que le helper DoPost (moonlight_auth.cc) puisse le nommer.
  struct HttpResult {
    long status = 0;      // code HTTP (0 = échec transport)
    std::string body;     // corps de la réponse
    std::string error;    // message d'erreur transport (vide si OK)
  };

 private:
  enum class State {
    kDisabled,      // pas de base_url / désactivé — login natif inchangé
    kWebLogin,      // formulaire identifiants Moonlight
    kAuthing,       // attente de la réponse HTTP /auth
    kDiscordStart,  // attente de la réponse HTTP discord_start (ouvre le navigateur)
    kDiscordWait,   // navigateur ouvert : polling discord_poll jusqu'à résolution
    kPickAccount,   // sélecteur de compte RO
    kSelecting,     // attente de la réponse HTTP /select
    kKickWait,      // compte encore en ligne : on laisse le serveur fermer la
                    // session (kick demandé par le login-server) avant de
                    // redemander un OTP — le premier essai est toujours refusé
    kDriveLogin,    // AutoLogin pilote le login natif (pas d'UI focusable ici)
    kError,         // message d'erreur + réessayer
  };

  struct Account {
    long account_id = 0;
    std::string userid;
    std::string label;
    int char_count = 0;
    std::string last_login;
    bool banned = false;
    // Un perso du compte est actuellement en jeu (char.online côté serveur, vu
    // au moment de l'appel /auth). Sert à ne PAS proposer par défaut un compte
    // déjà connecté : le rejouer déconnecterait la session en cours.
    bool online = false;
    // La session en ligne est un marchand automatique (@autotrade : ligne
    // `vendings`/`buyingstores` avec autotrade=1). Implique `online` — c'est une
    // précision sur la nature de la session, pas un état distinct : reprendre le
    // compte ferme la boutique.
    bool autotrade = false;
  };

  void LoadConfig();
  // Persiste l'identifiant web (fichier dédié) et, si « Se souvenir du mot de passe »
  // est coché, le mot de passe chiffré DPAPI. Jamais l'OTP de jeu.
  // `password` : mot de passe à mémoriser. DOIT être fourni par l'appelant quand
  // pass_buf_ a déjà été effacé — c'est le cas depuis HandleAuthResponse, car
  // ApplyAccountList vide le champ avant de rendre la main. nullptr = utiliser
  // pass_buf_ tel quel.
  void SavePref(const char* password = nullptr);

  // Résout la connexion cible (service-select clientinfo) : défaut = 1ʳᵉ entrée
  // (Moonlight-Destiny), override `--server=<nom>` au lancement.
  void ResolveServer();
  // Auto-sélectionne la connexion sur l'écran de service-select natif par
  // navigation clavier (Haut ×count puis Bas ×index puis Entrée), comme AutoLogin.
  void DriveServerSelect();

  // Lance un POST vers l'endpoint avec un corps x-www-form-urlencoded déjà encodé.
  // Non bloquant : rend la main aussitôt ; le résultat arrive via TakeResult().
  void StartPost(const std::string& form_body);
  // Récupère le résultat si le worker a terminé ; renvoie false sinon.
  bool TakeResult(HttpResult* out);
  void JoinWorker();

  // Handlers d'états (dessin ImGui).
  void DrawWebLogin();
  void DrawSpinner(const char* label);
  void DrawDiscordWait();
  void DrawPickAccount();
  void DrawError();

  // Login via compte Discord (OAuth2 dans le navigateur, cf. site oauth_discord.php).
  void StartDiscordLogin();  // POST discord_start -> ouvre le navigateur

  // POST action=select du compte `selected_` : demande un OTP frais au site et
  // passe en kSelecting. Appelé au clic « Jouer » ET à la re-tentative après le
  // refus « compte déjà connecté » (l'OTP précédent est brûlé, cf. kKickWait).
  void StartAccountSelect();

  // Renseigne accounts_/web_ticket_ à partir d'un JSON {web_ticket, accounts} et
  // passe à kPickAccount ; renvoie false (+ error_msg_/kError) si invalide/vide.
  // Partagé par la réponse /auth et la réponse discord_poll résolue.
  bool ApplyAccountList(const HttpResult& r);

  // Traitement des réponses HTTP.
  void HandleAuthResponse(const HttpResult& r);
  void HandleDiscordStartResponse(const HttpResult& r);
  void HandleDiscordPollResponse(const HttpResult& r);
  void HandleSelectResponse(const HttpResult& r);

  AutoLogin* auto_login_ = nullptr;  // non-owning ; pilote le login natif

  State state_ = State::kDisabled;

  // Config.
  bool enabled_ = false;
  bool save_id_ = false;
  bool remember_ = true;      // mémoriser l'identifiant web
  bool remember_pw_ = false;  // mémoriser le mot de passe web (DPAPI, opt-in)
  bool verify_tls_ = true;  // dev : `verify_tls: false` ignore la validation cert
  std::string base_url_;
  std::string endpoint_ = "/api/game_login.php";

  // Saisie du formulaire (buffers ImGui).
  char user_buf_[64] = {0};
  char pass_buf_[64] = {0};
  std::string error_msg_;

  // Login Discord (OAuth navigateur + polling). game_session_ = id opaque renvoyé
  // par discord_start ; le navigateur est ouvert sur discord_authorize_url_ ; on
  // interroge discord_poll toutes les poll_interval jusqu'à discord_deadline_tick_.
  std::string game_session_;
  std::string discord_authorize_url_;
  unsigned long discord_deadline_tick_ = 0;  // GetTickCount() limite (TTL)
  unsigned long discord_poll_tick_ = 0;       // dernier POST discord_poll
  unsigned long discord_poll_interval_ms_ = 2000;

  // Session web + comptes.
  std::string web_ticket_;
  std::vector<Account> accounts_;
  int selected_ = -1;
  // Demande de recentrer la liste sur le compte sélectionné (posé par la
  // navigation aux flèches, consommé au dessin de la ligne).
  bool pick_scroll_to_sel_ = false;

  // Pilotage du login natif (voie seamless SANS frappe, cf. native_login.h) :
  // credentials résolus, déclenchés une seule fois (edge-trigger) via SetText +
  // OnMsg dans OnRenderLoginUI.
  std::string drive_user_;
  std::string drive_pw_;
  bool fired_ = false;
  bool socket_seen_ = false;      // diag : la socket login s'est-elle ouverte ?
  unsigned long fire_tick_ = 0;   // GetTickCount() au tir (détection d'échec/timeout)
  // Première frame où la fenêtre de login a été vue, pour lui laisser le temps de
  // finir de s'initialiser avant d'écrire dedans. 0 = pas vue (ou disparue).
  //
  // 🔴 Le client repose le contenu de ses champs APRÈS les avoir construits (il y
  // remet l'identifiant de « Save ID ») : écrire trop tôt fait partir le bouton
  // Start avec le contenu du CLIENT, pas le nôtre. Sur l'identifiant ça donne un
  // « Unregistered ID », sur le mot de passe un OTP qui n'est pas le bon — et
  // comme le login-server brûle l'OTP en le validant, le réessai échoue à son
  // tour. Le défaut est resté invisible tant que l'écran de login restait posé
  // le temps que le joueur saisisse ; il est devenu visible quand la fenêtre a
  // commencé à être reconstruite juste avant le tir.
  unsigned long login_wnd_tick_ = 0;
  // Auto-confirmation du char-server (entre login réussi et char-select) : on
  // envoie Entrée par intervalles tant que la liste de persos n'est pas chargée.
  unsigned long charsrv_tick_ = 0;
  int charsrv_tries_ = 0;
  // Compte choisi alors qu'il était déjà en jeu (joueur actif ou autotrade). Le
  // login-server REFUSE forcément ce premier essai (code 8 « Server still
  // recognizes your last login ») après avoir demandé aux char-servers de kicker
  // la session — et il a déjà régénéré le web_auth_token au passage, donc l'OTP
  // est brûlé. On enchaîne alors kKickWait -> nouvel OTP -> nouvel essai, au lieu
  // d'afficher « OTP invalide » à un joueur qui n'a rien fait de mal.
  bool selected_online_ = false;
  int relogin_tries_ = 0;             // essais consommés par ce rattrapage
  unsigned long kick_wait_tick_ = 0;  // début de l'attente de fermeture
  // Latch « char-select ATTEINT » : posé dès que la liste de persos est chargée. Une
  // fois vrai, le DRIVE de login est TERMINÉ -> on n'auto-confirme plus (sinon l'Entrée
  // fuit au char-select et déclenche une entrée en jeu parasite) et on désarme la
  // détection d'échec (la socket char-server tombe NORMALEMENT en entrant en jeu, ce que
  // la détection prenait pour un login raté -> faux kError -> boucle de re-login). Reset
  // uniquement sur un vrai (re)login. Cf. moonlight_auth.cc.
  bool charsel_reached_ = false;

  // Service-select (liste <connection> clientinfo) : franchi nativement AVANT
  // d'afficher le formulaire (n'apparaît que si >1 connexion).
  int server_index_ = 0;              // connexion cible (défaut 0 = Moonlight-Destiny)
  int server_count_ = 0;              // nb de <connection> vues (0 = pas encore résolu)
  std::string server_name_;           // valeur --server (envoyée au site -> DB cible)
  bool server_select_done_ = false;   // sélection native (0x2723) déjà envoyée
  bool svc_kbd_fallback_ = false;     // repli clavier déjà tenté cette session
  unsigned long login_enter_tick_ = 0;  // GetTickCount() à l'entrée en mode login
  unsigned long svc_select_tick_ = 0;   // GetTickCount() au tir du 0x2723 (0 = jamais
                                        // tiré -> pas de repli clavier à armer)
  // Vrai dès qu'un login Moonlight a été déclenché et tant que la session
  // char-server est vivante. Survit à un re-OnModeSwitch(kLogin) (retour au
  // char-select depuis le jeu) pour NE PAS reforcer une ré-authentification web.
  bool authenticated_ = false;

  // Filet de sécurité : si le site/endpoint est injoignable, le joueur peut
  // basculer sur le login natif pour la session (le formulaire ImGui se masque
  // -> plus de capture clavier -> les champs natifs redeviennent utilisables).
  // Réarmé à chaque retour sur l'écran de login.
  bool native_fallback_ = false;

  // Posé quand on pilote effectivement le login via la voie Moonlight (après le
  // choix de compte). Lu par CharSelect pour n'activer l'UI ImGui que sur ce
  // parcours. Réarmé (false) à chaque retour sur l'écran de login.
  bool drove_moonlight_login_ = false;

  // Thread worker HTTP.
  std::thread worker_;
  std::mutex mtx_;
  HttpResult result_;
  std::atomic<bool> busy_{false};   // requête en cours
  std::atomic<bool> ready_{false};  // résultat disponible à récupérer
};

// ── L'adresse du SITE, pour composer un lien de page ─────────────────────────
//
// 🔴 LE DOMAINE ÉTAIT ÉCRIT EN DUR DANS TROIS AUTRES FICHIERS — la page
// bestiaire, la DB d'objets, l'aide « avatar Discord » — alors que
// `moonlight_auth:` accepte un `base_url` justement pour pointer une instance de
// DEV ou LOCALE. Sur une telle instance, le login partait au serveur configuré
// pendant que ces trois liens continuaient d'ouvrir la production. Invisible
// pour un joueur (le défaut d'usine EST la production), mais faux dès qu'on
// développe contre autre chose.
//
// ⚠ JAMAIS VIDE. Effacer `base_url` est la façon documentée de DÉSACTIVER le
// login web ; ça ne veut pas dire « plus de site ». On retombe alors sur
// l'adresse d'usine, sans quoi les trois liens deviendraient des URL tronquées.
//
// ⚠ NE COUVRE PAS deux autres porteurs du domaine, et c'est délibéré :
//   · la liste blanche d'hôtes de l'aperçu d'images est une décision de
//     SÉCURITÉ — l'élargir depuis un fichier de config est un autre sujet ;
//   · le préfixe des images relayées est fabriqué côté SERVEUR par
//     `groq_service.py` : il ne suit pas notre config, et le commentaire du chat
//     le dit déjà.
const char* SiteBaseUrl();
