#pragma once

// ── imgprev:: — L'APERÇU D'UNE IMAGE POSTÉE DANS LE CHAT ─────────────────────
//
// Survoler un lien d'image dans le chat en montre le contenu, sans ouvrir le
// navigateur. Utile, et pas anodin : aller chercher une image, c'est CONTACTER UN
// SERVEUR. D'où la règle qui structure tout ce module.
//
// 🔴 LISTE BLANCHE D'HÔTES, ET ELLE N'EST PAS NÉGOCIABLE.
//
// Une adresse postée dans le chat est choisie par un TIERS. Aller la chercher au
// survol révélerait l'adresse IP du joueur à qui a posté le lien — c'est
// exactement le fonctionnement d'un « IP logger », et c'est pire qu'un clic :
// personne ne DÉCIDE de survoler, et dans un chat qui défile ce sont les lignes
// qui glissent sous un curseur immobile.
//
// Ce qui rend l'aperçu sûr : ne charger que depuis des hébergeurs connus (CDN
// Discord, imgur, le site Moonlight). Sur ces hôtes-là, le posteur ne contrôle
// PAS le serveur contacté — il n'apprend donc rien de qui a regardé. Le vecteur
// de désanonymisation disparaît, et la liste couvre la quasi-totalité des images
// réellement postées.
//
// Hors liste blanche : rien n'est téléchargé, le lien reste un lien ordinaire
// avec sa confirmation (cf. links::DrawUrlConfirm).
//
// ⚠ On décode une donnée HOSTILE. D'où les bornes : taille de téléchargement
// plafonnée, délai d'expiration, `Content-Type` vérifié, dimensions bornées AVANT
// toute allocation, et l'image réduite à la taille d'affichage plutôt que gardée
// en pleine résolution (la VRAM d'un client RO n'est pas extensible).

#include <cstdint>
#include <string>
#include <vector>

namespace imgprev {

// Un aperçu et son état. `tex` n'est valide que dans l'état kReady.
struct Preview {
  enum State : uint8_t {
    kNone = 0,   // jamais demandé
    kPending,    // téléchargement / décodage en cours
    kReady,      // texture prête
    kFailed,     // hors liste blanche, réseau KO, format refusé, trop gros…
  };
  State state = kNone;
  void* tex   = nullptr;
  int   w     = 0;
  int   h     = 0;
};

// L'adresse désigne-t-elle une image qu'on accepte d'aller chercher ? Vrai
// seulement si l'HÔTE est dans la liste blanche ET que l'adresse ressemble à une
// image (extension, éventuellement suivie d'une query string).
bool IsPreviewable(const char* url);

// Demande l'aperçu. Idempotent : appeler à chaque frame de survol ne relance
// rien. Sans effet si l'adresse n'est pas prévisualisable.
void Request(const char* url);

// L'état courant d'une adresse. Ne déclenche AUCUN téléchargement — c'est
// `Request` qui le fait, et lui seul.
Preview Get(const char* url);

// Remplace la liste blanche. Prévu pour une liste envoyée par le SERVEUR : la
// liste par défaut est compilée dans le client pour que la fonctionnalité marche
// sans rien changer côté serveur, mais elle doit pouvoir s'élargir sans repatcher.
//
// Format d'une entrée : un hôte en minuscules, sans schéma. Un point initial vaut
// SUFFIXE (« .discordapp.net » couvre « cdn.discordapp.net » et ses voisins) ;
// sans point initial, l'égalité stricte est exigée.
void SetHostWhitelist(const std::vector<std::string>& hosts);

// L'hôte d'une adresse (« https://klipy.com/gifs/x » -> « klipy.com »). Vide si
// l'adresse n'a pas de forme exploitable. Public pour que le menu contextuel
// puisse NOMMER ce qu'il propose d'autoriser — on n'accorde pas à l'aveugle.
std::string HostOfUrl(const char* url);

// ── Les deux autorisations du joueur ─────────────────────────────────────────
//
// 🔴 ELLES NE SE VALENT PAS, et c'est contre-intuitif : autoriser un HÔTE est
// bien plus engageant qu'ouvrir un lien. Un clic révèle l'adresse IP une fois,
// délibérément ; un hôte autorisé la révèle AUTOMATIQUEMENT, au survol, et pour
// toujours. La confirmation doit donc être plus insistante pour le second.

// Cette adresse-ci, une fois. Rien n'est retenu. Le risque est exactement celui
// d'un clic sur le lien — que le joueur peut déjà faire — d'où l'absence de
// cérémonie. Les redirections de CETTE adresse sont suivies sans revérifier la
// liste : le joueur a consenti à contacter cette ressource, pas un hôte.
void AllowOnce(const char* url);

// L'adresse a-t-elle été autorisée EXPLICITEMENT par le joueur (à l'unité, ou par
// son hôte) ? Distinct de `IsPreviewable`, qui couvre aussi la liste intégrée.
//
// 🔴 Sert à borner correctement l'opt-in : le réglage « aperçu au survol »
// gouverne ce qui se charge TOUT SEUL, pas ce que le joueur a demandé. Une image
// réclamée d'un clic s'affiche même réglage éteint — sinon le geste ne ferait
// rien, sans que rien ne le dise.
bool IsExplicitlyAllowed(const char* url);

// Cet hôte, durablement. Persisté par la chatbox (clé « chatwnd_url_hosts »).
void AllowHost(const char* host);
void ForgetHost(const char* host);

// La liste du JOUEUR, séparée de l'intégrée et de celle du serveur : elle seule
// est révocable depuis l'interface, et une liste qu'on ne peut pas inspecter ni
// défaire n'aurait pas dû exister.
std::vector<std::string> UserHosts();
// Sérialisation pour la persistance (hôtes séparés par « ; »).
std::string UserHostsCsv();
void        SetUserHostsCsv(const std::string& csv);

// Entretien, à appeler une fois par frame et HORS rendu : c'est ici que les
// téléchargements terminés deviennent des textures (la création D3D n'a rien à
// faire sur un thread worker), que le cache est purgé après un reset de device,
// et qu'il est borné.
void Tick();

}  // namespace imgprev
