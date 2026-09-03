#include "features/systems/login_spectator.h"

#include <Windows.h>  // GetTickCount, mutex de session (jeton du décor)

#include <cstring>

#include "bourgeon.h"
#include "features/gameplay/afk_screen.h"
#include "features/systems/moonlight_auth.h"
#include "features/systems/native_login.h"
#include "features/windows/char_select.h"
#include "ragnarok/actor.h"
#include "ragnarok/globals.h"
#include "ragnarok/own_actor.h"
#include "ui/ro_imgui.h"
#include "utils/i18n.h"
#include "utils/log_console.h"
#include "utils/startup_settings.h"

namespace {

// ── Ce que le serveur attend ─────────────────────────────────────────────────
// L'identifiant est comparé tel quel par le login-server (SPECTATOR_USERID) ; le
// mot de passe est ignoré côté serveur, MAIS pas côté client : le handler du
// bouton « Start » (UILoginWnd_OnMsg cmd 186) refuse d'envoyer quoi que ce soit
// sous 4 caractères d'identifiant et 6 de mot de passe, sans un mot d'erreur.
// Celui-ci en fait neuf — il ne garde rien, il franchit une longueur.
constexpr char kSpectatorUser[] = "moonlight_spectator";
constexpr char kSpectatorPass[] = "spectator";

// La plage d'identifiants que le login-server réserve aux sessions de décor
// (SPECTATOR_ACCOUNT_ID_BASE et SPECTATOR_ID_COUNT dans mmo.hpp, dépôt serveur).
// Elle sert ici à RECONNAÎTRE une session de décor, jamais à en demander une :
// c'est le serveur qui distribue, parmi les libres.
constexpr uint32_t kSpectatorAidBase  = 2900000;
constexpr uint32_t kSpectatorAidCount = 90000;

bool IsSpectatorAccountId(uint32_t account_id) {
  return account_id >= kSpectatorAidBase &&
         account_id < kSpectatorAidBase + kSpectatorAidCount;
}

// Le personnage du spectateur est fabriqué à la volée par le char-server, et il
// occupe toujours le premier emplacement (char_spectator_load : `p->slot = 0`).
constexpr int kSpectatorSlot = 0;

// Contrôle « entrer en jeu » de UINewSelectCharWnd (cf. charsel::DriveNativeCtrl).
constexpr int kCtrlEnterGame = 0xB8;

// ── Les délais ───────────────────────────────────────────────────────────────
// Généreux : ils ne servent qu'à ABANDONNER, jamais à cadencer. Une étape qui
// aboutit le fait à la frame près, puisqu'on interroge des sondes d'état et non
// des minuteries.
constexpr uint32_t kServiceTimeoutMs = 8000;   // franchir le choix de connexion
constexpr uint32_t kLoginTimeoutMs   = 8000;   // trouver la fenêtre de login
constexpr uint32_t kAuthTimeoutMs    = 25000;  // login -> char-select
constexpr uint32_t kEnterTimeoutMs   = 25000;  // char-select -> monde
constexpr uint32_t kLeaveTimeoutMs   = 15000;  // décor -> écran de connexion
// 🔴 Avant ce délai, la réapparition de la fenêtre de login ne prouve RIEN :
// elle est encore là au moment où l'on vient d'appuyer sur « Start », et elle
// n'est détruite qu'au passage à l'écran suivant. Sans ce sursis, on conclurait
// « identifiants refusés » à chaque tentative, y compris celles qui marchent.
constexpr uint32_t kAuthGraceMs = 2500;
// Au-delà, une socket de login toujours fermée vaut « on n'ira pas plus loin ».
// ⚠ Le motif reste AMBIGU et le message le dit : un refus ferme la socket lui
// aussi, et la fenêtre de login n'est reconstruite qu'après le clic sur la boîte
// d'erreur — qu'on supprime. « Injoignable » et « refusé » sont donc
// indiscernables d'ici, et prétendre trancher serait mentir au journal.
constexpr uint32_t kUnreachableMs = 4000;

// Le temps qu'on laisse à la fenêtre de login pour finir de s'initialiser avant
// d'écrire dedans (cf. kLogin).
constexpr uint32_t kLoginSettleMs = 400;

// Cadence des comptes rendus d'une sortie qui s'éternise.
constexpr uint32_t kLeaveReportMs = 1000;

// Combien de temps le décor reste VIVANT avant de se couper tout seul.
//
// 🔴 Cette valeur doit rester STRICTEMENT INFÉRIEURE au `spectator_session_ttl`
// du serveur (battle_config, 600 s par défaut), et c'est tout l'intérêt d'avoir
// les deux : le serveur borne ce qui n'est pas un client Bourgeon, nous nous
// coupons AVANT lui. Une déconnexion subie passe par le chemin de détection de
// perte de lien, qui affiche une boîte — par-dessus l'écran de connexion, au
// milieu de la saisie du joueur. Volontaire, elle ne montre rien : la scène se
// fige, et un joueur qui a laissé son client ouvert ne regarde plus le décor.
constexpr uint32_t kDecorLifetimeMs = 300000;  // 5 min

// Combien de temps le voile TIENT après l'arrivée dans le monde (cf. le rendu).
// Assez pour couvrir la mise en place — le pantin, le HUD, la caméra — sans
// retarder l'apparition du décor au point que le joueur s'en aperçoive.
constexpr uint32_t kArrivalCoverMs = 500;

enum class Step {
  kOff,       // inerte
  kService,   // franchir le choix de connexion (<connection> du clientinfo)
  kLogin,     // écrire les identifiants et déclencher « Start »
  kWaitChar,  // attendre le char-select (en confirmant le char-server au passage)
  kEnter,     // tirer le contrôle « entrer en jeu » sur le premier emplacement
  kEntering,  // attendre la bascule en mode jeu
  kInWorld,   // le décor est là ; il se coupe du serveur peu après
  // 🔴 Sortie DEMANDÉE, pas encore tirée. L'étape existe pour une seule raison :
  // le bouton qui l'arme est cliqué en pleine frame ImGui, et la bascule de mode
  // est native. Un cran d'attente jusqu'au battement suivant, et elle part hors
  // frame comme tout le reste de la séquence.
  kLeaveWanted,
  kLeaving,   // bascule demandée : on attend l'écran de connexion
  kFailed,    // abandon ; le motif est dans g_status
};

Step g_step = Step::kOff;

// Début de l'ÉTAPE en cours (et non de la séquence) : chaque délai se compte
// depuis l'entrée dans son étape, sinon les retards s'additionnent et la
// dernière étape hérite d'un budget déjà consommé.
uint32_t g_step_ms = 0;

// La demande de sélection de connexion n'est tirée qu'une fois : c'est un
// message natif, pas un état qu'on maintiendrait.
bool g_service_sent = false;

// Le décor a-t-il été coupé du serveur ?
bool g_frozen = false;

// L'opt-out, lu paresseusement depuis le fichier de démarrage.
bool g_want_backdrop = true;
bool g_want_loaded = false;

// Début de l'attente d'armement (première frame hors-jeu). 0 = pas encore vue.
uint32_t g_pending_since = 0;
// Au-delà, on cesse de couvrir : l'écran de connexion n'arrive pas, et un voile
// noir sans fin serait pire que le défilement qu'on cherchait à éviter. Large,
// parce qu'il ne borne qu'un habillage — l'armement, lui, a ses propres délais.
constexpr uint32_t kPendingMaxMs = 8000;

// L'armement automatique n'a lieu qu'UNE fois par lancement. Après une sortie —
// ou un échec — l'écran de connexion appartient au joueur : le relancer
// derrière son dos lui reprendrait le formulaire qu'il est en train de remplir.
bool g_auto_tried = false;

// Comptes rendus déjà écrits pour la sortie en cours.
int g_leave_reports = 0;

// Une nouvelle session est demandée dès que celle-ci sera fermée (cf. Reroll).
bool g_reroll = false;

// Sorties rejouées pour la session en cours, et leur plafond. Borner évite de
// rejouer sans fin une manœuvre que le client refuse ; au-delà, on relâche
// l'état, faute de mieux.
int g_leave_retries = 0;
constexpr int kMaxLeaveRetries = 3;

char g_status[160] = {0};

void SetStatus(const char* text) {
  strncpy_s(g_status, sizeof(g_status), text, _TRUNCATE);
}

// ── Un seul décor VIVANT par machine ─────────────────────────────────────────
// Le serveur plafonne déjà les sessions par ADRESSE (`spectator_max_per_ip`), et
// c'est lui l'autorité — il est le seul à voir un inconnu. Mais son refus se
// PAIE : il arrive par le chemin d'erreur du login natif, donc par une boîte que
// le joueur n'a pas demandée, sur un écran où il n'a même pas encore tapé son
// nom, et il faut plusieurs secondes de voile avant qu'on renonce. Un jeton
// local répond tout de suite, et sans rien montrer.
//
// Nommé sans préfixe, donc LOCAL à la session Windows : deux clients lancés par
// le même joueur se voient — c'est le cas courant, celui du multi-client. Deux
// machines derrière une même adresse restent l'affaire du serveur, qui les
// départage (le second y perd le décor, pas sa connexion).
//
// 🔴 Windows rend le mutex quand le processus meurt : un client qui plante ne
// laisse pas le jeton derrière lui, et il n'y a donc rien à réparer.
constexpr char kLiveSlotName[] = "Bourgeon.LoginBackdrop.Live";
HANDLE g_live_slot = nullptr;

// Sans effet de bord : appelable à chaque frame, contrairement à la prise.
bool LiveSlotTaken() {
  HANDLE h = OpenMutexA(SYNCHRONIZE, FALSE, kLiveSlotName);
  if (h == nullptr) return false;
  CloseHandle(h);
  return true;
}

// Idempotent. Rend faux si un autre client tient déjà le décor.
bool ClaimLiveSlot() {
  if (g_live_slot != nullptr) return true;
  HANDLE h = CreateMutexA(nullptr, TRUE, kLiveSlotName);
  // ⚠ Pas de jeton = pas de garde, et surtout pas de refus : on tente, et c'est
  // le serveur qui tranchera. Une panne de mutex ne doit pas coûter le décor.
  if (h == nullptr) return true;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(h);
    return false;
  }
  g_live_slot = h;
  return true;
}

// Rendu dès que la session est rendue, et pas avant : tant que le décor vit, il
// occupe la seule place que le serveur accorde à cette adresse.
void ReleaseLiveSlot() {
  if (g_live_slot == nullptr) return;
  ReleaseMutex(g_live_slot);
  CloseHandle(g_live_slot);
  g_live_slot = nullptr;
}

// ── Rendre la connexion ──────────────────────────────────────────────────────
// Le geste central de ce module, et l'idée qui a débloqué tout le reste. Il se
// joue au moment de PARTIR, juste avant de reprendre le mode de connexion.
//
// Ce qui est à l'écran y reste : le `CGameMode` continue de tourner sans serveur
// — c'est ce que le décor hors ligne démontre depuis le premier jour — et
// `CRagConnection::SendPacket` sort sans rien écrire dès que la socket vaut -1.
// La scène se fige simplement sur son dernier instant, le temps du basculement.
//
// Ce qu'on y gagne : aucune session spectateur ne survit à l'instant où la vraie
// connexion du joueur part. Plus rien ne peut la parasiter — ni char-select du
// spectateur à traverser, ni identifiant à libérer, ni fantôme à laisser
// expirer. Et comme c'est une déconnexion VOLONTAIRE, le client n'affiche
// aucune boîte : celle-là vient du chemin de DÉTECTION d'une perte de lien.
//
// Idempotent : appelable sans se demander si c'est déjà fait.
void FreezeDecor() {
  if (g_frozen) return;
  g_frozen = true;
  const bool cut = rag::DisconnectFromServer();
  // La session rendue, la place est libre : un autre client de cette machine
  // peut prendre le décor à son tour (cf. ClaimLiveSlot).
  ReleaseLiveSlot();
  LogDiag("[LoginSpectator] décor coupé du serveur ({}) — la scène est figée",
          cut ? "connexion rendue" : "aucune connexion à rendre");
}

// ── Le décor peut-il seulement s'armer ? ─────────────────────────────────────
// Partagé par l'armement ET par l'attente qui le précède, et c'est le point :
// couvrir l'écran pour un décor qui ne viendra jamais serait pire que ne rien
// couvrir — un voile noir de plusieurs secondes au lancement, sans rien derrière.
//
// Chaque condition couvre un moyen d'enfermer le joueur devant une ville sans
// champ de saisie (cf. l'en-tête du module).
bool BackdropPossible() {
  if (!spectator::BackdropWanted()) return false;
  auto* auth = Bourgeon::Instance().moonlight_auth();
  // Sans le formulaire Moonlight, PAS de décor : il est le seul écran de
  // connexion capable de se dessiner par-dessus le monde. Et c'est aussi lui qui
  // confisque le clavier au natif — sans lui, le voile cacherait des écrans qui
  // continueraient de recevoir les touches.
  if (auth == nullptr || !auth->enabled() || auth->NativeFallback()) return false;
  // 🔴 Et pas pendant qu'il pilote un login — y compris son PASSTHROUGH, celui
  // d'un retour au char-select depuis le jeu, où l'état est repris sans qu'aucun
  // login ne reparte. Dans cet état il demande `spectator::Leave()` à chaque
  // frame : le décor se réarmait, l'autre le refermait, et ainsi de suite —
  // une boucle entrée/sortie mesurée toutes les trois secondes au journal, qui
  // a fini par faire planter le client en reconstruisant le char-select.
  if (auth->IsDrivingLogin()) return false;
  // L'auto-login de la ligne de commande pilote déjà les mêmes écrans natifs.
  // ⚠ Relu ici plutôt que demandé à `AutoLogin` : l'exposer aurait demandé un
  // accesseur de plus dans Bourgeon pour un test défensif d'une ligne. La source
  // de vérité reste son `ParseCommandLine`.
  if (std::strstr(GetCommandLineA(), "--login:") != nullptr) return false;
  // 🔴 Un autre client de cette machine tient déjà le décor. Testé SANS prendre
  // le jeton (la prise, elle, attend `Begin`) : ce prédicat tourne à chaque
  // frame, et un test qui réserve laisserait une place occupée par un décor qui
  // n'est jamais parti. Le `g_live_slot` de tête est ce qui nous distingue de
  // l'autre client : une fois le jeton à nous, il est bien pris — par nous.
  if (g_live_slot == nullptr && LiveSlotTaken()) return false;
  return true;
}

// Nom d'étape pour le journal. Une séquence qui traverse des écrans natifs ne se
// débogue pas autrement : ce qu'on voit à l'écran ne dit pas QUI l'a mis là.
const char* StepName(Step step) {
  switch (step) {
    case Step::kOff:         return "off";
    case Step::kService:     return "service-select";
    case Step::kLogin:       return "login";
    case Step::kWaitChar:    return "attente char-select";
    case Step::kEnter:       return "entrée en jeu";
    case Step::kEntering:    return "chargement";
    case Step::kInWorld:     return "en session";
    case Step::kLeaveWanted: return "sortie demandée";
    case Step::kLeaving:     return "sortie en cours";
    case Step::kFailed:      return "échec";
  }
  return "?";
}

void GoTo(Step step, const char* status) {
  const Step from = g_step;
  g_step = step;
  g_step_ms = GetTickCount();
  if (status != nullptr) SetStatus(status);
  LogDiag("[LoginSpectator] {} -> {} (login_wnd={} charsel_wnd={})",
          StepName(from), StepName(step), native_login::LoginWindowPresent(),
          native_login::CharSelectWindowPresent());
}

// Force la sortie : connexion rendue, puis reprise du mode de connexion. Rend
// faux si le client a refusé la bascule (aucun mode actif).
bool ForceLeaveWorld() {
  FreezeDecor();
  if (auto* afk = Bourgeon::Instance().afk_screen()) afk->EndNow();
  return rag::RequestModeSwitch(0 /* login */, "login");
}

void Fail(const char* reason) {
  // 🔴🔴 JAMAIS d'abandon qui laisse le client DANS LE MONDE. Relâcher l'état
  // pendant qu'une session spectateur tourne, c'est en faire la partie du
  // joueur : `InWorld()` devient faux, le HUD complet s'affiche, et il se
  // retrouve à jouer un personnage qui n'est à personne — sur une session que
  // rien ne fermera. C'est le seul défaut de ce module qui abîme autre chose que
  // l'esthétique.
  //
  // Un abandon avant l'entrée en jeu (choix de connexion, login, char-select)
  // passe par ici sans rien déclencher : `IsGameActive()` y est faux.
  if (Bourgeon::Instance().IsGameActive()) {
    LogError("[LoginSpectator] abandon EN JEU ({}) — sortie forcée", reason);
    ForceLeaveWorld();
    g_leave_reports = 0;
    g_leave_retries = 0;
    GoTo(Step::kLeaving, i18n::Tr("Fermeture de la session…"));
    return;
  }
  // Rien n'a été ouvert, ou plus rien ne l'est : la place ne nous sert plus.
  // (L'abandon EN JEU est passé par ForceLeaveWorld, donc par FreezeDecor.)
  ReleaseLiveSlot();
  g_step = Step::kFailed;
  g_step_ms = GetTickCount();
  SetStatus(reason);
  LogError("[LoginSpectator] abandon : {}", reason);
}

uint32_t StepAgeMs() { return GetTickCount() - g_step_ms; }

// ── Le déroulé, une étape par battement ──────────────────────────────────────
// Chaque branche pose la MÊME question : la sonde d'état dit-elle que l'écran
// attendu est là ? Sinon, a-t-on assez attendu ? On ne compte jamais de frames,
// et 🔴 on ne conclut JAMAIS sur l'envoi d'un message — seulement sur son effet.
// Cette règle a été enfreinte une fois dans la branche de sortie, et elle a
// coûté trois allers-retours.
void StepSequence() {
  switch (g_step) {
    case Step::kService:
      // Déjà devant la fenêtre de login : le choix de connexion a été franchi —
      // donc l'adresse du serveur est POSÉE. C'est le cas nominal, puisque
      // l'armement n'a lieu qu'une fois cette fenêtre là (cf. MaybeAutoStart).
      if (native_login::LoginWindowPresent()) {
        GoTo(Step::kLogin, i18n::Tr("Connexion en cours…"));
        return;
      }
      // Sinon on le franchit nous-mêmes. ⚠ Filet, pas chemin nominal : on vise
      // la connexion 0, là où le formulaire Moonlight sait viser CELLE que le
      // joueur a demandée (`--server:`). Avec une seule connexion au clientinfo
      // — notre cas — les deux se valent ; avec plusieurs, mieux vaut que ce
      // soit lui qui l'ait fait avant nous.
      if (!g_service_sent) {
        // 🔴 Attendre que le client ait PARSÉ son clientinfo. L'arbre XML n'est
        // pas encore là au tout début du lancement (LoadClientInfoXml tourne au
        // boot) et la liste des connexions est alors VIDE — appliquer « la
        // connexion 0 » revient à appliquer une entrée qui n'existe pas, donc
        // une adresse de serveur nulle. Le client part se connecter dans le
        // vide, revient à l'écran de login, et la séquence conclut à un refus.
        //
        // C'est exactement pourquoi ça marchait en armant à la main — le temps
        // d'ouvrir un panneau, l'arbre était lu depuis longtemps — et pourquoi
        // l'armement automatique, lui, tombait au mauvais moment. Le formulaire
        // Moonlight porte déjà la même garde de son côté (`server_count_ > 0`).
        if (native_login::ClientInfoConnectionNames().empty()) return;
        g_service_sent = true;
        // La première connexion du clientinfo, celle que le client propose par
        // défaut : le spectateur ne regarde qu'un serveur, le nôtre.
        native_login::SelectClientInfoConnection(0);
        return;
      }
      if (StepAgeMs() > kServiceTimeoutMs) {
        Fail(i18n::Tr("Le choix de connexion n'a pas été franchi."));
      }
      return;

    case Step::kLogin:
      // 🔴 On LAISSE LA FENÊTRE SE POSER avant d'écrire dedans. Mesuré : la
      // séquence tirait 112 ms après son apparition, et le serveur répondait
      // « Unregistered ID » — le client finit d'initialiser ses champs APRÈS les
      // avoir construits (il y repose l'identifiant mémorisé par « Save ID »),
      // écrasant le nôtre. Le bouton partait alors avec le contenu du client.
      //
      // Intermittent, donc : selon la frame où l'on tombait, ça passait ou non.
      // C'est la même précaution que l'auto-login de la ligne de commande, qui
      // attend lui aussi avant de saisir (cf. auto_login.h, kWaitLoginTicks).
      if (StepAgeMs() < kLoginSettleMs) return;
      // Un seul tir : `DriveLogin` déclenche une CONNEXION, la rejouer en
      // ouvrirait une seconde. Elle ne rend `true` que si la fenêtre était bien
      // là, ce qui fait de son retour la garde d'edge.
      if (native_login::DriveLogin(kSpectatorUser, kSpectatorPass)) {
        GoTo(Step::kWaitChar, i18n::Tr("Authentification…"));
        return;
      }
      if (StepAgeMs() > kLoginTimeoutMs) {
        Fail(i18n::Tr("La fenêtre de connexion n'est jamais apparue."));
      }
      return;

    case Step::kWaitChar:
      if (native_login::CharSelectWindowPresent()) {
        GoTo(Step::kEnter, i18n::Tr("Entrée en jeu…"));
        return;
      }
      // Un seul char-server chez nous, donc cet écran est sauté nativement ;
      // s'il apparaît quand même, on le confirme au lieu de laisser le joueur
      // devant un écran qu'il n'a pas demandé.
      if (native_login::CharServerWindowPresent()) {
        native_login::SelectConnection(0);
        return;
      }
      // La fenêtre de login RECONSTRUITE après le sursis : l'authentification a
      // été refusée (le serveur renvoie au formulaire, souvent avec sa propre
      // boîte d'erreur par-dessus).
      if (StepAgeMs() > kAuthGraceMs && native_login::LoginWindowPresent()) {
        Fail(i18n::Tr("Session spectateur refusée par le serveur."));
        return;
      }
      // 🔴 Serveur injoignable : on renonce VITE. Le décor s'arme tout seul au
      // lancement, et le voile couvre pendant ce temps — attendre le délai plein
      // laisserait chaque joueur devant un écran noir de vingt secondes chaque
      // fois que le serveur tousse, avant de lui rendre un formulaire qui, lui,
      // aurait pu s'afficher tout de suite. La socket de login qui ne monte pas
      // le dit bien avant l'absence de char-select.
      if (StepAgeMs() > kUnreachableMs && native_login::SocketFd() < 0) {
        Fail(i18n::Tr("Décor de connexion abandonné : pas de session "
                      "(serveur injoignable, ou session refusée)."));
        return;
      }
      if (StepAgeMs() > kAuthTimeoutMs) {
        Fail(i18n::Tr("Pas de réponse du serveur de personnages."));
      }
      return;

    case Step::kEnter:
      // Le natif construit lui-même le paquet et enchaîne sur la connexion
      // zone ; on ne fait que déclencher son handler de bouton.
      charsel::DriveNativeCtrl(kCtrlEnterGame, kSpectatorSlot);
      GoTo(Step::kEntering, i18n::Tr("Chargement du monde…"));
      return;

    case Step::kEntering:
      // L'arrivée se constate dans `NotifyModeSwitch`, pas ici : c'est le client
      // qui annonce le changement de mode, et l'attendre autrement reviendrait à
      // deviner. Il ne reste qu'à renoncer si elle ne vient jamais.
      if (StepAgeMs() > kEnterTimeoutMs) {
        Fail(i18n::Tr("Le monde ne s'est pas chargé."));
      }
      return;

    case Step::kInWorld:
      // 🔴 Rien à faire, et c'est le choix : la session reste VIVANTE tant que le
      // joueur n'a pas validé sa connexion. La ville bouge — joueurs qui
      // passent, monstres, NPC — au lieu d'être une photographie.
      //
      // Couper plus tôt (deux secondes après l'entrée, ce qu'on a d'abord fait)
      // ne protégeait de rien qu'on n'ait déjà réglé ailleurs : la seule chose
      // qui doit être vraie, c'est qu'AUCUNE session spectateur ne survive au
      // moment où la vraie connexion part — et ça, c'est `kLeaveWanted` qui le
      // garantit, en rendant la connexion AVANT de basculer. Les identifiants,
      // eux, ne se marchent plus dessus depuis qu'ils sont pris parmi les libres
      // (login_spectator_pick_id, côté serveur).
      //
      // 🔴 Une seule borne : au bout de kDecorLifetimeMs, on rend la connexion
      // NOUS-MÊMES. Le décor n'y perd que son mouvement — la scène reste à
      // l'écran, figée — et la session cesse d'occuper le serveur pour un joueur
      // qui n'est plus devant. Ne PAS attendre que le serveur coupe : sa coupure
      // à lui se voit (boîte de perte de lien), la nôtre non.
      if (StepAgeMs() > kDecorLifetimeMs) FreezeDecor();
      return;

    case Step::kLeaveWanted:
      // Le décor est déjà coupé du serveur : il ne reste qu'à reprendre le mode
      // de connexion. Une bascule LOCALE, sans rien demander à personne — c'est
      // le geste du décor hors ligne, et le seul qui ne dépende d'aucune réponse.
      //
      // 🔴 Ce n'est plus CZ_RESTART puis retour au char-select. Ce chemin-là
      // traversait le char-select DU SPECTATEUR — celui qui proposait
      // « Spectator » au joueur — et il s'est révélé sans issue : mesuré, la
      // commande de retour n'atteignait même pas le mode (elle passait par le
      // getter GATÉ, qui rend zéro hors état 1 ; cf. rag::ActiveModeIfReady).
      FreezeDecor();  // filet : le délai n'avait peut-être pas couru
      // 🔴 Rendre la POSE DE CAMÉRA pendant qu'il y a encore une caméra. Après
      // la bascule, l'écriture ne va plus nulle part : la plongée du décor et
      // son orbite restaient en place jusque dans la partie du joueur, qui
      // héritait d'un angle qu'il n'avait pas choisi.
      if (auto* afk = Bourgeon::Instance().afk_screen()) afk->EndNow();
      if (!rag::RequestModeSwitch(0 /* login */, "login")) {
        Fail(i18n::Tr("Sortie refusée : aucun mode actif."));
        return;
      }
      GoTo(Step::kLeaving, i18n::Tr("Retour à l'écran de connexion…"));
      return;

    case Step::kLeaving:
      // 🔴 LA SEULE fin de la sortie : la fenêtre de connexion est là.
      if (native_login::LoginWindowPresent()) {
        GoTo(Step::kOff, i18n::Tr("Session spectateur fermée."));
        // 🔴 Le rebond se fait ICI et nulle part ailleurs : c'est le seul point
        // où l'on SAIT que la session précédente est close et que l'écran de
        // connexion est là. Enchaîner plus tôt rouvrirait une session pendant
        // qu'une autre se ferme — exactement la boucle qui a fait planter le
        // client lors du réarmement automatique.
        if (g_reroll) {
          g_reroll = false;
          spectator::Begin();
        }
        return;
      }
      // Une sortie qui dure se raconte : sans cela, « bloqué » est tout ce qu'on
      // sait, et les sondes qui répondent « non » sont précisément celles qu'il
      // faut voir.
      if (StepAgeMs() > kLeaveReportMs * (g_leave_reports + 1)) {
        ++g_leave_reports;
        LogDiag("[LoginSpectator] sortie en attente depuis {} ms "
                "(login_mode={} login_wnd={} charsel_wnd={} charsrv_wnd={} "
                "socket={})",
                StepAgeMs(), native_login::AtLoginScreen(),
                native_login::LoginWindowPresent(),
                native_login::CharSelectWindowPresent(),
                native_login::CharServerWindowPresent(),
                native_login::SocketFd());
      }
      // 🔴 Garde-fou, et non paresse : rester coincé ici tiendrait
      // `Connecting()` vrai pour toujours, c'est-à-dire garderait le formulaire
      // de connexion muet — un client inutilisable jusqu'au redémarrage.
      if (StepAgeMs() > kLeaveTimeoutMs) {
        // 🔴 Même règle qu'au-dessus : tant que le client est DANS LE MONDE, on
        // ne relâche pas — on rejoue la sortie. Relâcher ici ferait du décor la
        // partie du joueur, avec un HUD complet sur un personnage qui n'est à
        // personne. Borné, pour ne pas rejouer une manœuvre qui ne prend pas.
        if (Bourgeon::Instance().IsGameActive() &&
            g_leave_retries < kMaxLeaveRetries) {
          ++g_leave_retries;
          LogError("[LoginSpectator] toujours en jeu après {} ms — sortie "
                   "rejouée ({}/{})",
                   StepAgeMs(), g_leave_retries, kMaxLeaveRetries);
          ForceLeaveWorld();
          g_step_ms = GetTickCount();
          g_leave_reports = 0;
          return;
        }
        GoTo(Step::kOff, i18n::Tr("Sortie du décor : le client n'a pas suivi."));
        LogError("[LoginSpectator] sortie sans écran d'arrivée — état relâché");
      }
      return;

    case Step::kOff:
    case Step::kFailed:
      return;
  }
}

// ── Le voile de séquence ─────────────────────────────────────────────────────
// 🔴 Il COUVRE et il CAPTE, et les deux comptent. La séquence traverse les
// écrans natifs — dont le char-select de la session spectateur, qui propose
// « Spectator » en toutes lettres au joueur venant de saisir ses identifiants.
//
// La capture n'est pas décorative non plus : les écrans RO réagissent au clavier
// sans jamais consulter leur visibilité, et une Entrée résiduelle sur ce
// char-select clique son bouton par défaut — c'est-à-dire entre en jeu sur le
// personnage du spectateur.
//
// ⚠ Ici on CAPTE, contrairement au voile du pilotage de login (moonlight_auth) :
// notre séquence n'attend aucune frappe, elle écrit dans les champs et déclenche
// les handlers directement.
void DrawSequenceCover() {
  ro::DrawFullscreenCover(
      g_status[0] != '\0' ? g_status : i18n::Tr("Connexion…"),
      /*capture_keyboard=*/true);
}

}  // namespace

namespace spectator {

bool Connecting() {
  return g_step != Step::kOff && !InWorld() && g_step != Step::kFailed;
}

// ⚠ `kLeaveWanted` en fait partie : la bascule n'est pas encore demandée, on est
// donc toujours dans le monde. L'en exclure ferait réapparaître le HUD — et
// réveillerait l'écran de veille — pendant le battement qui sépare le clic de
// son effet.
bool InWorld() {
  return g_step == Step::kInWorld || g_step == Step::kLeaveWanted;
}

bool Active() { return Connecting() || InWorld(); }

bool Pending() {
  if (g_auto_tried || g_step != Step::kOff) return false;
  if (!BackdropPossible()) return false;
  // 🔴 Jamais par-dessus le CHAR-SELECT. Le réarmement au retour d'une partie
  // (cf. NotifyModeSwitch) rouvre cette attente, et le chemin du retour passe
  // par le char-select : sans cette garde, le voile viendrait couvrir l'écran où
  // le joueur choisit son personnage — un écran qui est bien le sien, lui.
  // L'armement, de son côté, attend déjà la fenêtre de LOGIN.
  if (native_login::CharSelectWindowPresent() ||
      native_login::MakeCharWindowPresent()) {
    return false;
  }
  // Le compte à rebours part au PREMIER appel, c'est-à-dire à la première frame
  // hors-jeu : on ne dispose de rien de plus fiable pour dater le lancement, et
  // c'est de toute façon l'instant qui nous intéresse.
  if (g_pending_since == 0) g_pending_since = GetTickCount();
  return (GetTickCount() - g_pending_since) < kPendingMaxMs;
}

void Begin() {
  if (Active()) return;
  // 🔴 Dernière porte, et la seule qui RÉSERVE. Un client qui n'obtient pas le
  // jeton ne tente rien : pas de voile, pas de séquence, pas de refus serveur à
  // essuyer — l'écran de connexion s'affiche comme si le décor n'existait pas.
  if (!ClaimLiveSlot()) {
    LogDiag("[LoginSpectator] décor cédé : un autre client de cette machine le tient");
    return;
  }
  g_service_sent = false;
  g_frozen = false;
  g_leave_reports = 0;
  GoTo(Step::kService, i18n::Tr("Ouverture de la session spectateur…"));
  LogDiag("[LoginSpectator] séquence armée");
}

void ScrubNativePrefill() {
  // 🔴 Jamais pendant la séquence : c'est ELLE qui vient d'écrire dans ce champ,
  // et l'effacer entre l'écriture et le tir du bouton « Start » enverrait un
  // identifiant vide — le handler natif refuse alors sans un mot, et le décor
  // ne partirait plus jamais.
  if (Active()) return;
  if (native_login::ClearLoginIdIf(kSpectatorUser)) {
    LogDiag("[LoginSpectator] identifiant du décor retiré du champ de login");
  }
}

void Rearm() {
  // Seulement au repos : une séquence en cours n'a pas besoin qu'on lui rende un
  // droit qu'elle exerce déjà, et la couper là serait pire.
  if (Active()) return;
  g_step = Step::kOff;  // sortir d'un éventuel kFailed
  g_auto_tried = false;
  g_pending_since = 0;
}

void Reroll() {
  if (g_step != Step::kInWorld) return;
  g_reroll = true;
  Leave();
}

void Leave() {
  if (g_step != Step::kInWorld) return;
  // Demandée seulement : c'est le battement qui la tire (cf. kLeaveWanted).
  g_leave_reports = 0;
  g_leave_retries = 0;
  GoTo(Step::kLeaveWanted, i18n::Tr("Fermeture de la session…"));
}

bool BackdropWanted() {
  // Lu une fois : ce fichier est sur le disque, et la question se pose à chaque
  // battement de l'écran de connexion.
  if (!g_want_loaded) {
    g_want_loaded = true;
    g_want_backdrop = startup::LoginBackdropEnabled(/*fallback=*/true);
  }
  return g_want_backdrop;
}

void SetBackdropWanted(bool wanted) {
  if (BackdropWanted() == wanted) return;  // rien à écrire, rien à fermer
  g_want_backdrop = wanted;
  startup::SaveLoginBackdropEnabled(wanted);
  // Décocher pendant que le décor tourne le referme tout de suite. Une case qui
  // n'agirait qu'au prochain lancement passerait pour cassée — et le joueur qui
  // la décoche veut précisément retrouver son écran de connexion ordinaire.
  if (!wanted) {
    Leave();
    return;
  }
  // Et RECOCHER le relance, ici et maintenant. C'est ce que le geste veut dire —
  // et c'est aussi la seule seconde chance offerte après un échec : l'armement
  // automatique, lui, ne se rejoue pas de la session (il reprendrait au joueur
  // le formulaire qu'il est peut-être en train de remplir).
  if (g_step == Step::kOff || g_step == Step::kFailed) {
    g_step = Step::kOff;  // sortir de kFailed, sinon Begin() ne prend pas
    Begin();
  }
}

void HideOwnActor() {
  if (!InWorld()) return;
  void* actor = rag::OwnActor();
  if (actor == nullptr) return;
  // 🔴 `kDrawEnabled`, et SURTOUT PAS le bit « invisible » du masque d'options :
  // celui-là n'est lu que par le dispatch de rendu d'une AUTRE classe, pas par
  // celui d'un joueur — la démonstration est dans ragnarok/actor.h, et elle a
  // coûté un aller-retour où l'option ne faisait rien du tout.
  *(reinterpret_cast<uint8_t*>(actor) + rag::actor::kDrawEnabled) = 0;
}

CameraPose Camera() {
  // Les valeurs de la mise en scène, en dur (cf. l'en-tête). 35° au-dessus de
  // l'horizon : assez haut pour découvrir la ville, assez bas pour qu'elle garde
  // sa perspective. Un recul modeste, une orbite lente — le décor doit vivre
  // sans attirer l'œil, il y a un formulaire par-dessus.
  return CameraPose{/*tilt_deg=*/35.0f, /*zoom_factor=*/1.40f,
                    /*spin_deg_s=*/6.0f};
}

const char* Status() { return g_status; }

bool NotifyModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) {
  if (mode_type == ModeMgr::ModeType::kGame) {
    // ⚠ On ne prend cette entrée en jeu pour la nôtre QUE si on l'attendait : le
    // joueur peut très bien se connecter normalement pendant que le module dort,
    // et une session mal reconnue lui volerait son HUD.
    if (g_step != Step::kEntering) return false;
    GoTo(Step::kInWorld, i18n::Tr("Décor en place."));
    LogDiag("[LoginSpectator] session spectateur en place sur « {} »",
            map_name != nullptr ? map_name : "?");
    return true;  // ne pas propager
  }

  // Retour hors du monde depuis une session : la fin, demandée (Leave) ou subie.
  // La bascule vient d'avoir lieu, il ne reste qu'à en attendre l'écran.
  if (InWorld()) {
    g_leave_reports = 0;
    g_leave_retries = 0;
    GoTo(Step::kLeaving, i18n::Tr("Retour à l'écran de connexion…"));
    return false;
  }

  // ── Retour d'une VRAIE partie : le décor a droit à une seconde vie ─────────
  // L'armement ne se rejoue pas de lui-même — il reprendrait au joueur le
  // formulaire qu'il est peut-être en train de remplir — mais ce cas-ci est
  // l'inverse : le joueur QUITTE sa session et revient à l'accueil, où il
  // s'attend à retrouver ce qu'il a vu au lancement.
  //
  // ⚠ Le retour au char-select passe aussi par ici (c'est le même changement de
  // mode). On ne fait donc que RÉARMER : c'est `MaybeAutoStart` qui décide,
  // et il exige la fenêtre de LOGIN — absente au char-select. Le tri se fait
  // tout seul, sans qu'on ait à distinguer les deux écrans.
  if (g_step == Step::kOff || g_step == Step::kFailed) {
    g_step = Step::kOff;
    g_auto_tried = false;
    g_pending_since = 0;
  }
  return false;
}

}  // namespace spectator

namespace {

// ── L'armement automatique ───────────────────────────────────────────────────
// Le décor est la voie par défaut : au premier écran de connexion, il part seul.
// Les gardes ci-dessous ne sont pas des précautions de style — chacune couvre un
// moyen d'enfermer le joueur devant une ville sans champ de saisie (cf. l'en-tête).
void MaybeAutoStart() {
  if (g_auto_tried || g_step != Step::kOff) return;
  // 🔴 Un autre client de cette machine tient le décor : on renonce POUR DE BON,
  // et pas seulement pour cette frame. Sans ce marquage, le jour où il rend sa
  // session — le joueur s'y connecte —, notre séquence partirait ICI, longtemps
  // après la fenêtre où le voile pouvait la couvrir : le joueur verrait son
  // propre écran de connexion se remplir tout seul de « moonlight_spectator ».
  // Un retour au login le rendra à nouveau candidat (Rearm remet le drapeau).
  if (g_live_slot == nullptr && LiveSlotTaken()) {
    g_auto_tried = true;
    return;
  }
  if (!BackdropPossible()) return;
  // 🔴 On attend la FENÊTRE DE LOGIN, pas seulement « on est dans le mode ».
  // Ce n'est pas une nuance : tant qu'elle n'est pas là, le client est encore au
  // service-select, et le franchir POSE L'ADRESSE DU SERVEUR
  // (Apply_ClientInfoConnection). Ce travail appartient au formulaire Moonlight,
  // qui sait quelle connexion viser (`--server:`, index résolu dans le
  // clientinfo) — or notre séquence le fait TAIRE dès qu'elle démarre.
  //
  // Démarrer trop tôt le privait donc de son seul moment utile, et le client
  // restait sans adresse : `socket_fd=-1`, aucune connexion ouverte — ni pour le
  // décor, ni pour le login du joueur ENSUITE, puisque l'adresse vaut pour toute
  // la session. Mesuré au journal, et c'est ce qui rendait le décochage/recochage
  // « réparateur » : le formulaire reprenait la main et appliquait la connexion.
  if (!native_login::LoginWindowPresent()) return;

  g_auto_tried = true;
  spectator::Begin();
}

// ── Une arrivée À LA MAIN sur le compte du décor ─────────────────────────────
// L'identifiant réservé voyage en clair dans la DLL et son mot de passe est
// ignoré : rien n'empêche un joueur d'entrer avec — il n'a même pas eu à le
// chercher, on le lui laissait pré-rempli (cf. ScrubNativePrefill). Le serveur
// tient sa part (invisible, muet, immobile, rien n'est écrit), mais il ne peut
// pas distinguer notre séquence d'un humain : ce qu'il rend est une partie
// complète à l'écran, sur un personnage qui ne peut ni marcher ni parler, et
// que rien ne referme avant la coupure du serveur — laquelle arrive, elle, par
// le chemin de la perte de lien, avec sa boîte.
//
// Le client, lui, SAIT. Il connaît la plage d'identifiants du décor et il sait
// s'il a piloté cette session. Une session de décor qu'il n'a pas ouverte est
// donc une erreur, et la refermer tout de suite est la seule réponse honnête :
// laisser le joueur dans un monde où il ne peut rien faire est pire que de le
// ramener à son écran de connexion.
//
// ⚠ En jeu SEULEMENT. L'identifiant de compte est un global que le client pose
// à l'acceptation du login et n'efface jamais : au retour à l'écran de
// connexion, il porte encore celui de la session précédente — donc celui du
// décor qui vient de se fermer. S'y fier là reviendrait à éjecter le joueur de
// son propre login. En jeu, il est celui de la session en cours, sans ambiguïté.
void EjectManualSpectatorSession() {
  if (spectator::Active()) return;  // notre décor : c'est son travail
  if (!Bourgeon::Instance().IsGameActive()) return;
  if (!IsSpectatorAccountId(rag::OwnAccountIdSafe())) return;

  LogError("[LoginSpectator] session spectateur ouverte À LA MAIN (AID {}) — "
           "fermeture", rag::OwnAccountIdSafe());
  ForceLeaveWorld();
  g_leave_reports = 0;
  g_leave_retries = 0;
  // Le même retour que celui d'un décor : voile posé le temps de la bascule,
  // puis l'écran de connexion. Le motif s'affiche dessus.
  GoTo(Step::kLeaving,
       i18n::Tr("Ce compte est réservé au décor de l'écran de connexion."));
}

}  // namespace

void LoginSpectator::OnTick() {
  EjectManualSpectatorSession();
  spectator::ScrubNativePrefill();
  MaybeAutoStart();
  StepSequence();
}

void LoginSpectator::OnRenderLoginUI() {
  // On couvre pendant la séquence — aller comme retour, les écrans natifs
  // qu'elle traverse n'appartiennent pas au joueur — ET pendant l'attente qui la
  // précède (cf. spectator::Pending) : sans ça, le décor s'annonce par un défilé
  // d'écrans qui ne le concernent pas.
  //
  // 🔴 Et on TIENT le voile un instant après l'arrivée. La demi-seconde qui suit
  // la fin du chargement est celle où tout se met en place à l'écran : le pantin
  // que le serveur n'a pas encore dit de cacher (son paquet coûte un aller-retour),
  // ce que le HUD natif dessine avant d'être vetoé, la caméra qui n'a pas encore
  // pris sa pose. Couvrir cet instant règle la famille entière, là où traiter
  // chaque symptôme séparément revient à courir après des frames.
  const bool settling =
      spectator::InWorld() && StepAgeMs() < kArrivalCoverMs;
  if (spectator::Connecting() || spectator::Pending() || settling) {
    DrawSequenceCover();
    return;
  }

  // ⚠ Et RIEN d'autre. Une fois en session, le décor n'a pas d'interface : c'est
  // un fond, et le seul écran qui compte par-dessus est le formulaire de
  // connexion (dispatché ici même, comme tous les hooks d'écran de connexion).
  //
  // Le panneau « Session spectateur » qui vivait ici a disparu avec la raison
  // qui le justifiait : quand le décor se déclenchait à la main, il fallait bien
  // une porte de sortie. Maintenant qu'il est la voie par défaut, la sortie est
  // celle du joueur — il se connecte — ou la case « Ville en fond » du
  // formulaire, qui le ferme sur-le-champ.
}
