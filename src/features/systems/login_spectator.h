#pragma once

#include "features/plugin.h"

// LoginSpectator — la capitale VIVANTE derrière l'écran de connexion.
//
// Le client ouvre lui-même une session « spectateur » sur le serveur avant que
// le joueur ne se connecte : il entre réellement en jeu, sur le point
// d'observation choisi côté serveur (`spectator_point`), et ce qu'on voit est le
// monde tel qu'il tourne — NPC, monstres, joueurs qui passent. L'écran de veille
// prend ensuite la caméra en main et efface les deux interfaces ; il ne reste
// que le décor, sur lequel le formulaire de connexion vient se poser.
//
// C'est la suite — et le remplacement — du décor HORS LIGNE
// (features/overlays/login_backdrop). Celui-ci basculait le client en
// `CGameMode` sans serveur : la ville y est DÉSERTE, puisque NPC et monstres
// n'existent que côté serveur, et le personnage n'a aucune donnée. Il reste
// comme banc d'essai, mais ce n'est plus la voie.
//
// ── Ce qui rend la chose possible, côté serveur ──────────────────────────────
// L'identifiant réservé `moonlight_spectator` est authentifié SANS toucher la
// base : ni compte ni personnage n'est créé, l'`account_id` se dérive du numéro
// de session (2900000 + slot), et le personnage est rendu invisible, muet et
// immobile. Rien n'est écrit, donc rien n'est à nettoyer. Tout le détail est
// dans le dépôt serveur (branche `map-login`) ; ici on ne fait que dérouler la
// séquence de connexion à sa place.
//
// 🔴 La séquence est celle du CLIENT, et non des paquets fabriqués : on écrit
// dans les champs natifs et on déclenche les mêmes handlers qu'un clic humain
// (features/systems/native_login, puis le contrôle 0xB8 du char-select). Un
// CA_LOGIN forgé sauterait tout ce que le mode natif fait autour — et c'est
// justement ce « autour » qui monte la connexion zone.
//
// Le décor s'arme TOUT SEUL au premier écran de connexion, et c'est la voie par
// défaut. Le joueur peut s'en passer (case « Ville en fond » du formulaire,
// persistée dans le fichier de démarrage — cf. startup::LoginBackdropEnabled).
//
// 🔴 Trois gardes avant de partir, et chacune couvre un moyen d'ENFERMER le
// joueur dans un décor sans porte de sortie :
//   · le formulaire Moonlight doit être actif. C'est le seul écran de connexion
//     capable de se dessiner PAR-DESSUS le monde ; sans lui, le décor remplace
//     l'écran de login natif par une ville sans champ de saisie ;
//   · pas de repli « Login classique » en cours, pour la même raison — ce repli
//     rend la main aux champs NATIFS, qui n'existent pas pendant le décor ;
//   · pas d'auto-login en ligne de commande, qui pilote déjà la même séquence et
//     se battrait avec la nôtre.
namespace spectator {

// La séquence est demandée ou en cours (avant l'entrée en jeu). Les écrans de
// connexion de Bourgeon — formulaire Moonlight, char-select ImGui — se taisent
// tant que c'est vrai : ils dessineraient par-dessus des écrans natifs qu'on est
// en train de piloter, et un clic du joueur tomberait au milieu d'une séquence
// automatique.
bool Connecting();

// La session en cours EST une session spectateur : le monde tourne, mais ce
// n'est pas la partie du joueur. Le `CGameMode`, lui, est bien réel — d'où la
// double lecture de ce prédicat :
//   · `Bourgeon::RenderUI` dispatche les hooks d'ÉCRAN DE CONNEXION par-dessus
//     le jeu, et non le HUD (il n'y a pas de personnage à afficher) ;
//   · `AfkScreen` tient sa veille ouverte sans rien avaler (cf. son OnTick).
bool InWorld();

// L'un ou l'autre.
bool Active();

// Le décor est ATTENDU mais pas encore armé : le client finit de se mettre en
// route (il lui faut ~1,5 s pour franchir le service-select et construire son
// écran de connexion). Sans cela, le joueur voit défiler trois écrans avant le
// décor — le fond de login statique, la parade de Porings, puis le noir du
// chargement. On couvre donc dès la première frame, et il n'en reste qu'un.
//
// ⚠ Couvrir n'est PAS museler : le formulaire Moonlight continue de tourner
// dessous, parce que c'est lui qui franchit le service-select — et le faire
// taire à ce moment-là est exactement ce qui a coûté le plus cher dans ce
// chantier (aucune adresse de serveur, aucune connexion, pour personne).
//
// Plafonné dans le temps : si l'écran de connexion n'arrive jamais, mieux vaut
// rendre la main que tenir un voile noir devant un client qui n'ira nulle part.
bool Pending();

// ── Effacer le pantin, sans attendre le serveur ──────────────────────────────
// À appeler une fois par frame de jeu, AVANT le dessin (cf. Bourgeon::OnGameFrame).
//
// 🔴 Le serveur cache bien le personnage du spectateur — il le fait même deux
// fois, pour les autres joueurs et pour lui-même (le paquet SELF de
// `clif_parse_LoadEndAck`) — mais cette seconde annonce arrive APRÈS un
// aller-retour réseau : le pantin est visible le temps d'une frame ou deux au
// sortir du chargement. Le client, lui, sait dès maintenant qu'il regarde un
// décor ; il n'a aucune raison d'attendre qu'on le lui dise.
//
// ⚠ Chaque frame, et pas une fois pour toutes : le moteur écrit lui-même cet
// octet (culling), et le paquet du serveur repassera par-dessus.
void HideOwnActor();

// ── La mise en scène du décor ────────────────────────────────────────────────
// La pose que la caméra prend derrière l'écran de connexion. C'est l'écran de
// veille qui l'applique (il sait déjà lisser une orbite et une plongée), mais
// les VALEURS sont ici, et elles sont EN DUR.
//
// 🔴 Volontairement pas un réglage : c'est la même image pour tout le monde. Un
// écran de connexion est une mise en scène, pas un poste de travail — et ces
// chiffres n'ont rien à voir avec ceux de la veille du joueur EN JEU, qui
// répondent à une tout autre question (s'éloigner de son clavier sans perdre de
// vue sa partie). Confondre les deux, c'est laisser le décor changer d'allure
// d'un joueur à l'autre selon un réglage qu'aucun d'eux n'a fait pour ça.
struct CameraPose {
  float tilt_deg;     // hauteur au-dessus de l'horizon, en degrés POSITIFS
  float zoom_factor;  // recul, en multiples de la distance de repos
  float spin_deg_s;   // vitesse d'orbite, degrés par seconde
};
CameraPose Camera();

// Arme la séquence depuis l'écran de connexion. Sans effet si elle tourne déjà
// ou si l'on est déjà en jeu.
void Begin();

// Quitte la session : retour au char-select puis à l'écran de connexion. Sans
// effet hors session spectateur.
void Leave();

// Referme la session et en rouvre une aussitôt, ce qui retire un nouveau lieu
// (le tirage est fait par le serveur, à chaque ouverture de session — cf.
// db/spectator_points.yml).
//
// ⚠ Il n'y a pas de raccourci : rien, côté client, ne permet de se téléporter
// ailleurs. Le point d'observation est décidé par le char-server au moment où il
// fabrique le personnage, donc en changer VEUT DIRE refaire une session. D'où le
// détour complet — sortie, bascule, reconnexion — qui prend une seconde ou deux
// et reste caché par le voile.
void Reroll();

// Dernier verdict lisible (progression, ou motif d'abandon). Jamais nul.
const char* Status();

// ── L'opt-out ────────────────────────────────────────────────────────────────
// Le joueur veut-il du décor ? Lu depuis le fichier de démarrage au premier
// appel (défaut : oui), puis gardé en mémoire.
bool BackdropWanted();

// Pose le choix ET l'écrit. Décocher en pleine session la ferme sur-le-champ :
// une case qui ne prend effet qu'au prochain lancement passerait pour cassée.
void SetBackdropWanted(bool wanted);

// Rend son droit à l'armement automatique : le prochain écran de connexion aura
// son décor. N'arme RIEN par lui-même — c'est `MaybeAutoStart` qui décidera, et
// il attend la fenêtre de login.
//
// 🔴 À appeler depuis les retours à l'écran de connexion qui ne changent PAS de
// mode. Le bouton « Revenir au login » du char-select en est un : le client
// reste en mode connexion, seul son état bouge (9/6 -> 3), donc aucune annonce
// de changement de mode n'est émise et le réarmement branché dessus ne voit
// rien passer. `MoonlightAuth::RearmWebLogin` existe pour exactement la même
// raison, au même endroit.
void Rearm();

// Appelé par `Bourgeon::FireModeSwitch` AVANT la diffusion aux modules. Renvoie
// vrai quand il ne faut PAS propager.
//
// 🔴 L'entrée en jeu d'une session spectateur n'en est pas une : le monde est
// réel, mais le personnage n'appartient à personne — niveau 1, sacs vides, aucun
// skill. Les modules qui s'arment sur cette annonce y liraient un état qui n'est
// celui de personne, et certains ÉMETTENT (fabrication d'une fenêtre native,
// demande au serveur) : autant de gestes faits au nom d'un joueur qui n'est même
// pas encore connecté. Le retour hors du monde, lui, se propage normalement —
// les modules doivent pouvoir se défaire de ce qu'ils avaient posé.
bool NotifyModeSwitch(ModeMgr::ModeType mode_type, const char* map_name);

}  // namespace spectator

class LoginSpectator : public Plugin {
 public:
  const char* name() const override { return "Login Spectator"; }

  // La séquence avance ICI, et surtout pas au rendu. Deux raisons, chacune
  // suffisante :
  //   · 🔴 elle déclenche des commandes NATIVES (choix de connexion, bouton
  //     « Start », contrôle du char-select). En pleine frame ImGui, le client se
  //     fige sans un mot — c'est une règle du projet, pas une précaution ;
  //   · ce battement-ci est le seul qui ne s'arrête jamais. Le rendu, lui, sort
  //     pendant les chargements de carte : la séquence resterait suspendue en
  //     plein « entrée en jeu », sans même pouvoir renoncer, et l'écran de
  //     connexion ne reviendrait jamais.
  void OnTick() override;

  // Le panneau de session, dessiné PAR-DESSUS le monde : ce hook est dispatché
  // sur les écrans hors du monde et, pendant une session spectateur, sur le jeu
  // lui-même (cf. spectator::InWorld).
  void OnRenderLoginUI() override;

  // ⚠ PAS de `OnModeSwitch` ici : le module n'apprend pas les changements de mode
  // par la diffusion, il la DÉCIDE (spectator::NotifyModeSwitch, appelée en amont
  // par Bourgeon). Le redéclarer en plus l'aurait fait courir deux fois sur le
  // retour au login — la seule annonce qui, elle, se propage.
};
