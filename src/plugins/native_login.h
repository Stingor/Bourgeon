#pragma once

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

// DIAGNOSTIC : dumpe dans bourgeon.log la table des connexions lue à mode+0x1e8
// (IP/port/état/nom, stride 0xa0) pour `count` entrées. Sert à comprendre pourquoi
// la sélection native échoue (« Failed to Connect ») : on voit si/quand la table
// est peuplée et avec quelles valeurs, à comparer avec ce que le clavier
// sélectionne. `tag` préfixe la ligne (ex. contexte d'appel).
void LogConnectionTable(int count, const char* tag);

// Écrit userid + password/OTP dans les champs natifs et déclenche le bouton Start
// (aucune frappe). Renvoie true si effectivement déclenché (fenêtre de login
// présente), false si l'écran n'est pas prêt (réessayer au frame suivant).
// À appeler UNE SEULE FOIS (edge-trigger) : re-déclencher relance une connexion.
// `readback_id`/`readback_pw` (si non nuls, taille `bufsz`) reçoivent le contenu
// RELU des champs natifs APRÈS SetText (diagnostic : confirme que SetText a bien
// peuplé les champs, avant l'envoi).
bool DriveLogin(const char* userid, const char* password,
                char* readback_id = nullptr, char* readback_pw = nullptr,
                unsigned bufsz = 0);

// Masque (hide=true) / réaffiche la fenêtre de login native ET son fond
// (bg_login.tga, fenêtre séparée). No-op hors CLoginMode (garde anti-UAF).
// N'appeler EN MASQUAGE que pendant que le formulaire ImGui est affiché (la
// fenêtre existe alors à coup sûr) ; une fois le login déclenché, ne plus toucher
// (le flag +0x28 persiste, la fenêtre reste cachée jusqu'à sa destruction native).
void MaskLoginWindow(bool hide);

// fd de la socket login (-1 = pas connecté). Sonde de progression du login.
int SocketFd();

// True si la liste de personnages est chargée (on est arrivé au char-select :
// le dispatcher cmd 8 renvoie un CHARACTER_INFO pour le slot 0). Sert à savoir
// quand ARRÊTER l'auto-confirmation du char-server (ni avant, ni après).
bool CharListLoaded();

}  // namespace native_login
