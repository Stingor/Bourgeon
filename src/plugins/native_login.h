#pragma once

#include <string>
#include <vector>

// native_login — pilotage du login RO natif SANS frappes clavier synthétiques.
//
// Au lieu de simuler des touches (fragile : focus, Save ID, timing), on écrit
// directement le texte dans les deux CUIEdit de UILoginWnd puis on déclenche le
// handler du bouton Start (OnMsg cmd 0xBA), exactement comme un clic humain. Le
// mode natif enchaîne ensuite connect -> CA_LOGIN -> char-server -> char-select
// tout seul. RE + vérif adversariale : voir docs/login_connect_re.md.
//
// Toutes les fonctions sont à appeler depuis le thread jeu (hook rendu login).
namespace native_login {

// True si le mode courant est CLoginMode (on est à l'écran login/char-select).
bool AtLoginScreen();

// True si une fenêtre UILoginWnd VIVANTE est présente (vtable validée). Sert à
// détecter un ÉCHEC de login : après le tir, si l'écran de login réapparaît
// (fenêtre recréée), c'est que l'auth a échoué (sur succès on passe au
// char-select où cette fenêtre est détruite).
bool LoginWindowPresent();

// Noms (<display>) des <connection> de clientinfo.xml, DANS L'ORDRE du
// service-select natif — lus dans l'arbre XML que le client garde en mémoire
// (parsé au boot par LoadClientInfoXml 0x0171d320, qui ouvre le fichier via
// ResFileStream = le VFS, donc GRF COMPRIS).
//
// ⚠ C'est la SEULE source correcte. Lire `data\clientinfo.xml` sur le disque ne
// marche que sur un client de dev : chez les joueurs le fichier n'existe QUE dans
// moonlight.grf, la lecture disque rend une liste vide -> « 0 connexion » -> le
// service-select n'était jamais franchi automatiquement.
//
// Liste vide si l'arbre n'est pas encore parsé (appelé trop tôt au boot) : dans ce
// cas réessayer plus tard. Bornée à 8 entrées, comme LoadClientInfoXml.
std::vector<std::string> ClientInfoConnectionNames();

// Commit NATIF d'une connexion clientinfo `index` = franchissement du SERVICE-SELECT
// PRÉ-LOGIN (liste <connection>). CLoginMode_SendMsg cmd 0x2723 : applique
// address/domain/version/servicetype de la connexion #index (Apply_ClientInfoConnection
// FUN_00a72da0) puis pose l'état 3 = écran de login. C'est exactement ce que
// déclenche l'Entrée clavier sur la fenêtre, mais instantané et avec l'index exact.
// Renvoie true si l'appel est parti (pas de garantie d'effet -> prévoir un repli).
bool SelectClientInfoConnection(int index);

// Sélectionne NATIVEMENT le CHAR-SERVER `index` — écran POST-LOGIN (pas le
// service-select !). CLoginMode_SendMsg cmd 0x2713, table à mode+0x1e8 stride 0xa0,
// peuplée UNIQUEMENT par Net_OnAcceptLogin_ParseAccount (AC_ACCEPT_LOGIN 0x0ac4).
// N'appeler qu'APRÈS login (avant, la table contient tout autre chose). Renvoie
// false tant que l'entrée n'est pas chargée (IP nulle) -> réessayer.
// NB : avec un seul char-server (cas moonlight) cet écran est sauté nativement.
bool SelectConnection(int index);

// Écrit userid + password/OTP dans les champs natifs et déclenche le bouton Start
// (aucune frappe). Renvoie true si effectivement déclenché (fenêtre de login
// présente), false si l'écran n'est pas prêt (réessayer au frame suivant).
// À appeler UNE SEULE FOIS (edge-trigger) : re-déclencher relance une connexion.
bool DriveLogin(const char* userid, const char* password);

// Masque (hide=true) / réaffiche la fenêtre de login native ET son fond
// (bg_login.tga, fenêtre séparée). No-op hors CLoginMode (garde anti-UAF).
// N'appeler EN MASQUAGE que pendant que le formulaire ImGui est affiché (la
// fenêtre existe alors à coup sûr) ; une fois le login déclenché, ne plus toucher
// (le flag +0x28 persiste, la fenêtre reste cachée jusqu'à sa destruction native).
void MaskLoginWindow(bool hide);

// fd de la socket login (-1 = pas connecté). Sonde de progression du login.
int SocketFd();

// True si la liste de personnages est chargée (le dispatcher cmd 8 renvoie un
// CHARACTER_INFO pour au moins un slot).
// ⚠ NE PAS s'en servir pour « on est arrivé au char-select » : ces structures
// SURVIVENT à un retour à l'écran de connexion (bouton « Revenir au login » du
// char-select), donc la sonde reste vraie pendant tout le login SUIVANT. Utiliser
// CharSelectWindowPresent().
bool CharListLoaded();

// True si la fenêtre NATIVE « Select Service » (choix du char-server, id 2) est
// vivante. Elle n'est construite QUE par l'état 6 du mode login, atteint sur
// AC_ACCEPT_LOGIN — donc sa présence PROUVE que l'auth serveur a réussi. C'est la
// seule sonde fiable pour n'auto-confirmer (Entrée) qu'après un login accepté :
// « la fenêtre de login n'est pas là » ne prouve rien, elle est aussi absente
// pendant les boîtes d'erreur natives d'un login REFUSÉ, où les Entrées postées
// enchaînent les popups et relancent des logins parasites.
bool CharServerWindowPresent();

// True si la fenêtre NATIVE du char-select (UINewSelectCharWnd, id 0x115) est
// vivante. Sonde fiable de « on est arrivé au char-select » : les fenêtres sont
// toutes purgées à chaque changement d'état du mode (aucun résidu, contrairement aux
// CHARACTER_INFO). Sert à savoir quand ARRÊTER l'auto-confirmation du char-server
// (ni avant — la fenêtre id 2 attend une validation, ni après — l'Entrée fuirait
// dans le char-select).
bool CharSelectWindowPresent();

}  // namespace native_login
