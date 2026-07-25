#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "plugins/plugin.h"

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

  // Vrai si CETTE session de login a suivi la voie Moonlight (web login + choix
  // de compte), par opposition au repli « Login classique » (champs natifs). Sert
  // à réserver le char-select ImGui au parcours Moonlight : login natif => UI
  // native de bout en bout. Réarmé à chaque retour sur l'écran de login.
  bool DroveMoonlightLogin() const { return drove_moonlight_login_; }

  // Résultat d'une requête HTTP, publié par le thread worker sous verrou. Public
  // pour que le helper DoPost (moonlight_auth.cc) puisse le nommer.
  struct HttpResult {
    long status = 0;      // code HTTP (0 = échec transport)
    std::string body;     // corps de la réponse
    std::string error;    // message d'erreur transport (vide si OK)
  };

 private:
  enum class State {
    kDisabled,     // pas de base_url / désactivé — login natif inchangé
    kWebLogin,     // formulaire identifiants Moonlight
    kAuthing,      // attente de la réponse HTTP /auth
    kPickAccount,  // sélecteur de compte RO
    kSelecting,    // attente de la réponse HTTP /select
    kDriveLogin,   // AutoLogin pilote le login natif (pas d'UI focusable ici)
    kError,        // message d'erreur + réessayer
  };

  struct Account {
    long account_id = 0;
    std::string userid;
    std::string label;
    int char_count = 0;
    std::string last_login;
    bool banned = false;
  };

  void LoadConfig();
  void SavePref();  // persiste web_user/remember dans le yaml (jamais l'OTP)

  // Nom lisible d'un état (instrumentation LogDiag des transitions).
  static const char* StateName(State s);

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
  void DrawPickAccount();
  void DrawError();

  // Traitement des réponses HTTP.
  void HandleAuthResponse(const HttpResult& r);
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
  // Auto-confirmation du char-server (entre login réussi et char-select) : on
  // envoie Entrée par intervalles tant que la liste de persos n'est pas chargée.
  unsigned long charsrv_tick_ = 0;
  int charsrv_tries_ = 0;
  // Latch « char-select ATTEINT » : posé dès que la liste de persos est chargée. Une
  // fois vrai, le DRIVE de login est TERMINÉ -> on n'auto-confirme plus (sinon l'Entrée
  // fuit au char-select et déclenche une entrée en jeu parasite) et on désarme la
  // détection d'échec (la socket char-server tombe NORMALEMENT en entrant en jeu, ce que
  // la détection prenait pour un login raté -> faux kError -> boucle de re-login). Reset
  // uniquement sur un vrai (re)login. Cf. moonlight_auth.cc.
  bool charsel_reached_ = false;

  // Service-select (liste <connection> clientinfo) : auto-passé par nav clavier
  // AVANT d'afficher le formulaire (n'apparaît que si >1 connexion — dev).
  int server_index_ = 0;              // connexion cible (défaut 0 = Moonlight-Destiny)
  int server_count_ = 0;              // nb de <connection> dans clientinfo.xml
  std::string server_name_;           // valeur --server (envoyée au site -> DB cible)
  bool server_select_done_ = false;   // sélection native (0x2723) déjà envoyée
  bool svc_kbd_fallback_ = false;     // repli clavier déjà tenté cette session
  unsigned long login_enter_tick_ = 0;  // GetTickCount() à l'entrée en mode login
  unsigned long diag_conn_tick_ = 0;    // throttle du dump diagnostic de la table
  // Dernier état loggué (détecteur de transition -> LogDiag). Sentinelle invalide
  // pour que la 1ʳᵉ frame loggue toujours l'état courant.
  State last_logged_state_ = static_cast<State>(-1);
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
