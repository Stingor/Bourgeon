#ifndef BOURGEON_FEATURES_WINDOWS_PALETTE_EDITOR_H_
#define BOURGEON_FEATURES_WINDOWS_PALETTE_EDITOR_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "features/plugin.h"
#include "ui/palette_ramps.h"
#include "ui/spr_act.h"  // Resource : les index de palette, pour la pipette

// Éditeur de STYLE — le joueur compose son apparence, puis la valide.
//
// ── 🔴 Rien ne touche au personnage avant la validation ──────────────────────
// L'aperçu vit dans le PANTIN de la fenêtre, et là seulement. Curseurs,
// préréglages, codes collés, coiffures essayées : tout cela ne fait que
// recalculer 1024 octets de palette pour le pantin. Le sprite en scène, la
// feuille de perso et les autres joueurs ne voient que ce qui a été validé.
//
// Ce n'était pas le premier choix. L'éditeur poussait d'abord chaque mouvement
// de curseur dans le rendu (features/fx/palette_inject), en pariant que l'aperçu
// le plus fidèle était le personnage lui-même. Le prix s'est révélé trop lourd :
// ouvrir la fenêtre changeait déjà l'apparence, chaque réglage fabriquait un
// bloc de palette définitif, un « annuler » laissait des restes à moitié posés
// sur l'acteur, et il fallait défaire à la fermeture ce qu'on avait posé à
// l'ouverture. Le pantin rend tout cela inutile — et l'essai gratuit.
//
// ── Ce que le joueur manipule ────────────────────────────────────────────────
// Pas 256 couleurs : les RAMPES du sprite (les dégradés d'une même pièce du
// costume), détectées par `ro::DetectRamps`, et pour chacune trois décalages
// HSV relatifs. Le dégradé garde donc ses ombres et ses reflets, seule la
// teinte bascule — et la recette tient en quelques octets, ce qui la rend
// transmissible aux autres joueurs.

class PaletteEditor : public Plugin {
 public:
  const char* name() const override { return "PaletteEditor"; }

  // Ouvre/ferme la fenêtre. À l'ouverture, le corps du joueur est (re)chargé et
  // ses rampes redétectées : elles ne valent QUE pour ce sprite-là.
  void Toggle();
  // Ouvre ou ferme sans basculer — ce qu'il faut à un NPC, qui doit pouvoir dire
  // « ouvre » sans refermer une fenêtre déjà ouverte.
  void SetOpen(bool open);
  bool IsOpen() const { return open_; }

  // ── Le style d'un AUTRE joueur, reçu par un lien de chat ────────────────
  //
  // Ouvre une fenêtre d'aperçu : un pantin — le NÔTRE — habillé de ce style.
  // 🔴 C'est le seul aperçu honnête possible : une recette ne porte aucune
  // couleur, elle ne veut rien dire hors d'un sprite. Montrer « ce que ça
  // donnerait sur moi » est exactement la question que se pose celui qui clique.
  // Sans effet si le code est illisible.
  void OpenStylePreview(const char* code, const char* owner_utf8);

  // Charge ce code dans l'éditeur, comme un « coller ». ⚠ N'applique RIEN au
  // personnage : il faudra valider. Un lien de chat vient d'un autre joueur, il
  // ne doit pas pouvoir changer une apparence d'un simple clic.
  void TryStyleCode(const char* code);

  // Chaque frame, entre NewFrame et Render. Porte aussi le raccourci
  // d'ouverture et la pose paresseuse des détours d'injection : cette fonction
  // est le seul endroit du module dont on SAIT qu'il tourne sur le fil de rendu.
  void OnRenderUI() override;

 private:
  // Amorce la recette depuis les couleurs déjà partagées (serveur ou cache
  // local). Rend true si elle vient de le faire — l'appelant doit alors
  // RECHARGER, car la teinte de base ainsi obtenue change la palette sur
  // laquelle les rampes se détectent.
  //
  // 🔴 Doit précéder `Reload()`, jamais le suivre. L'inverse donnait une
  // première ouverture où les curseurs annonçaient la bonne palette pendant que
  // le corps portait encore les couleurs nues du sprite.
  bool SeedFromShared();

  // Jette tout ce qui appartenait au personnage précédent.
  //
  // 🔴 Rien dans cette fenêtre ne vaut pour deux personnages : ni la recette, ni
  // le sprite chargé, ni le verrou d'amorçage. Ce dernier est le plus traître —
  // il dit « déjà amorcé », donc sans cette remise à zéro, rouvrir la fenêtre
  // après un changement de personnage ressortait la recette du PRÉCÉDENT et la
  // proposait à valider.
  void ResetForNewCharacter();

  // Personnage pour lequel l'état courant a été bâti, 0 = aucun.
  uint32_t session_cid_ = 0;

  // Pose sur la recette ce que le personnage porte RÉELLEMENT sur la tête.
  //
  // 🔴 Deux règles opposées, parce que les deux valeurs n'ont pas le même
  // maître. La COUPE écrase : elle appartient au serveur, qui l'applique pour de
  // bon, et sa globale client suit. La COULEUR ne fait que combler un trou :
  // c'est nous qui l'injectons, la globale du client l'ignore, et l'imposer
  // effacerait à chaque ouverture la couleur choisie par le joueur.
  void SeedWornHead();

  // Recharge le sprite du corps du joueur et redétecte les rampes.
  // Rend false (et renseigne `error_`) si le corps n'est pas recolorable.
  //
  // 🔴 À appeler dès que `recipe_.palette_id` CHANGE : c'est lui qui désigne le
  // fichier de palette sur lequel la base est fusionnée, donc les rampes. Sans
  // rechargement, les réglages continuent de s'appliquer sur l'ANCIENNE base et
  // les curseurs affichent une teinte de base que le personnage ne porte pas.
  //
  // ⚠ Ne touche PAS au personnage : cette fonction ne fait que (re)construire de
  // quoi dessiner l'aperçu.
  bool Reload();

  // VALIDE : pousse la recette sur le personnage lui-même.
  //
  // 🔴 Appelée par le SEUL bouton de validation, et de nulle part ailleurs. Tout
  // le reste de cette fenêtre est un aperçu qui vit dans le pantin ; le sprite en
  // scène, la feuille de perso et les autres joueurs ne voient que ce qui a été
  // validé. C'est ce qui rend l'essai gratuit — on peut tout dérégler et fermer
  // sans conséquence — et ce qui a supprimé toute une famille de bugs, où un
  // aperçu à moitié posé sur l'acteur survivait à un « annuler ».
  void Apply();

  // Retire la recette : le joueur retrouve EXACTEMENT son apparence native.
  void RestoreServerColors();

  // Dessine le pantin d'aperçu à droite de la liste des pièces.
  //
  // 🔴 Tête et corps SEULEMENT : ni coiffe, ni cape, ni arme. Ce qu'on règle ici
  // est la palette du CORPS ; un chapeau volumineux rétrécirait le personnage et
  // masquerait justement les pièces à voir.
  // `highlight` = la pièce survolée dans la liste, ou -1. Ses pixels PULSENT sur
  // le pantin, ce qui répond d'un coup d'œil à « quelle zone commande cette
  // ligne ? ».
  void DrawPreviewDoll(float size, int highlight);

  // ── Les trois sélecteurs ────────────────────────────────────────────────
  //
  // 🔴 Des GRILLES DE VIGNETTES, pas des curseurs, et c'est un choix guidé par
  // l'écran de création de personnage : « coiffure 47 » ne veut rien dire, on
  // choisit une coiffure en la VOYANT. Un curseur obligeait à parcourir 80
  // valeurs une par une pour découvrir ce qu'elles sont.
  //
  // Elles vivent dans des popups plutôt que dans la fenêtre : trois grilles de
  // 80, 251 et 553 cases y tiendraient mal, et l'éditeur doit rester lisible
  // quand on ne choisit rien.
  // La fenêtre d'aperçu d'un style reçu. Indépendante de l'éditeur : elle
  // s'ouvre depuis le chat, et fermer l'un ne doit pas fermer l'autre.
  void DrawStylePreview();

  void DrawHairStylePicker();
  void DrawHairColorPicker();
  void DrawBodyPalettePicker();

  // Les 1024 octets qu'une palette de corps officielle donnerait SUR CE SPRITE,
  // fusion comprise — de quoi peindre une vignette qui montre le vrai corps.
  //
  // 🔴 Une couleur moyenne ne suffit PAS, et l'essai l'a montré : la palette 484
  // rendait un beige qui ne laissait rien présager du vert qu'elle contient. Une
  // palette de vêtement n'est pas une teinte, c'est une distribution.
  //
  // `budget` borne le nombre de `.pal` lus dans la frame : la grille se remplit
  // en quelques images au lieu d'ouvrir 553 fichiers d'un coup. Rend nullptr
  // tant que la palette n'est pas connue.
  const uint8_t* PalettePreview(int palette_id, int* budget);

  bool open_ = false;
  bool need_pos_ = true;

  // Le corps sur lequel les rampes ont été détectées. 🔴 Les index de palette
  // n'ont AUCUNE signification commune d'un corps à l'autre : changer de classe
  // ou de style de corps invalide toute la recette.
  std::string body_path_;
  int loaded_body_ = -1;
  int loaded_sex_ = -1;

  // Le sprite a-t-il été LU sur l'acteur, ou seulement déduit de la classe ?
  // 🔴 La déduction rejoue une résolution native pleine de cas particuliers
  // (styles de corps alternatifs, montures) ; quand elle diverge, les rampes
  // restent valides mais désignent les MAUVAIS index — des curseurs qui agissent
  // au hasard et laissent intactes les zones visées. Savoir laquelle des deux a
  // servi est donc la première question à poser devant un réglage qui « ne fait
  // rien ».
  bool resolved_from_actor_ = false;

  // Pixels peints au total, et ceux qu'une rampe couvre. L'écart est la part du
  // corps qu'AUCUN curseur n'atteint : au-delà des huit plus grands dégradés, le
  // reste n'a pas de réglage. C'est affiché, parce que c'est exactement ce que
  // le joueur constate quand « cette zone-là ne change pas ».
  int pixels_total_ = 0;
  int pixels_covered_ = 0;

  std::vector<uint8_t> base_;  // palette RGBA d'origine du .spr (1024 o)
  ro::PaletteRamp ramps_[ro::kMaxRamps];
  int ramp_count_ = 0;
  ro::PaletteRecipe recipe_;

  // Pièce survolée à la frame PRÉCÉDENTE, pour la pulsation du pantin.
  //
  // 🔴 Décalage d'une image assumé : le pantin est dessiné AVANT la liste (il
  // est à gauche), donc au moment où on le peint, on ne sait pas encore ce que
  // la souris survolera. Sur une pulsation qui bat deux fois par seconde, une
  // image de retard ne se voit pas.
  int survolee_ = -1;

  // Instant du dernier envoi au serveur, pour un accusé local de quelques
  // secondes. 🔴 Il ne dit PAS que le serveur a accepté — il n'accuse rien — mais
  // seulement que le paquet est parti.
  double shared_tick_ = 0.0;

  // La recette a-t-elle déjà été amorcée depuis celle que le serveur nous a
  // renvoyée ? Une seule fois par session : ensuite, ce que le joueur a réglé
  // ici fait autorité, et ré-amorcer à chaque ouverture annulerait des retouches
  // qu'il n'a pas encore partagées.
  bool seeded_ = false;

  // Le joueur a-t-il EXPLICITEMENT demandé le vide ?
  //
  // 🔴 Verrou d'amorçage. Tant que l'amorçage n'a pas abouti, il est retenté à
  // chaque frame — sinon un éditeur ouvert avant que les couleurs ne soient
  // connues resterait vide pour toute la session. Une recette non neutre suffit
  // à l'arrêter dans presque tous les cas ; les deux EFFACEMENTS, eux, rendent
  // la recette neutre et se verraient donc défaire par le rattrapage suivant.
  bool touched_ = false;

  // ── Le brouillon : ce que le joueur composait et n'a pas validé ─────────
  //
  // 🔴 Écrit sur DISQUE en cours de route, pas à la fermeture de la fenêtre.
  // L'accident visé — plantage, coupure, client tué — ne passe justement pas par
  // une fermeture propre, et n'attraper que celle-ci ne protégerait que du cas
  // où le joueur n'a rien perdu.
  //
  // Le prix est une écriture de fichier, donc pas à chaque frame : on attend que
  // le joueur ait fini de bouger. Un glissement de curseur produit des dizaines
  // d'états par seconde dont aucun ne mérite le disque.
  void TickDraft();

  // Le style tel qu'il est POSÉ sur le personnage. Le brouillon n'existe que par
  // son écart avec `recipe_` — tant que les deux coïncident, il n'y a rien à
  // récupérer, et proposer de recharger l'identique serait un bouton qui ment.
  ro::PaletteRecipe applied_;

  // Encodage de `recipe_` vu à la frame précédente, et l'instant où il a changé.
  // `draft_tick_` à 0 = rien en attente d'écriture.
  std::string draft_seen_;
  double draft_tick_ = 0.0;

  // Nom en cours de saisie pour un nouveau préréglage.
  char preset_name_[32] = {0};

  // Identifiant ImGui du champ ci-dessus, relevé à la frame précédente.
  //
  // 🔴 Écrire dans `preset_name_` ne suffit PAS quand le champ a le focus :
  // ImGui travaille alors sur sa copie interne et la réécrit par-dessus la nôtre
  // à la frame suivante. Notre valeur n'aura jamais été affichée — et surtout,
  // « Enregistrer » repartirait de l'ancien texte. Il faut resynchroniser le
  // widget par son état (`ReloadUserBufAndMoveToEnd`), d'où cet identifiant.
  // Même idiome que la chatbox (`ChatWindow::NotifyInputEdited`).
  unsigned int preset_field_id_ = 0;

  // Instant du dernier code collé illisible, pour un message éphémère. Un
  // collage raté qui ne dit rien laisse croire à un bug de l'éditeur.
  double error_code_tick_ = 0.0;

  // La COIFFURE que le joueur essaie, 1..80.
  //
  // 🔴 HORS de la recette, et c'est une frontière de nature. Une recette est une
  // fiction de rendu : le serveur la range sans la comprendre, chaque client la
  // retraduit. La coiffure, elle, est un VRAI état de personnage —
  // `sd->status.hair`, ce que change le styliste — sauvegardé avec lui et
  // annoncé à tout le monde par le protocole natif.
  //
  // Ce champ n'est donc qu'un APERÇU : il vaut la coupe portée tant que le
  // joueur n'a rien changé, et « Partager mon style » le transforme en vraie
  // demande (CZ 0x0F29). Fermer sans partager le rend à sa valeur d'origine.
  // (La coiffure vit dans `recipe_.hair_style` : elle fait partie du style, donc
  // elle voyage, se range dans un préréglage et s'écrit dans un code comme le
  // reste. Elle est amorcée à l'ouverture depuis la coupe RÉELLEMENT portée —
  // celle-ci fait autorité, un passage chez un styliste NPC ayant pu la changer
  // sans que notre recette le sache.)

  // ── La PIPETTE ──────────────────────────────────────────────────────────
  //
  // Désigne la pièce sous le curseur quand on survole le pantin. C'est la
  // réciproque de la pulsation : celle-ci répond à « où agit cette ligne ? », la
  // pipette à « quelle ligne commande cette manche ? » — et c'est la seconde
  // question que le joueur se pose vraiment.
  //
  // 🔴 Elle ne compare AUCUNE couleur. Deux pièces peuvent porter la même teinte
  // après réglage, et une comparaison les confondrait. On lit l'INDEX de palette
  // du pixel source — que le parseur conserve précisément pour ça — et on cherche
  // la rampe qui le contient : la réponse est exacte par construction.
  //
  // Rend le numéro de pièce, ou -1 (hors du corps, ou pixel transparent).
  int PickPart(float screen_x, float screen_y) const;

  // Le `.spr` du corps, gardé DÉCODÉ : la pipette a besoin des index par pixel,
  // que la texture ne porte plus. Chargé au premier survol, jeté quand le corps
  // change.
  ro::spract::Resource body_res_;
  bool body_res_tried_ = false;

  // Où le pantin a atterri à la frame précédente — origine et échelle, de quoi
  // convertir un point écran en unités `.act`.
  float doll_origin_x_ = 0.0f;
  float doll_origin_y_ = 0.0f;
  float doll_scale_ = 0.0f;  // 0 = pas encore dessiné

  // Pièce désignée par la pipette CETTE frame, ou -1. Le pantin étant dessiné
  // avant la liste, elle est connue à temps pour souligner la bonne ligne.
  int pipette_ = -1;

  // ── L'aperçu d'un style reçu ────────────────────────────────────────────
  // Sa propre base et ses propres rampes : la teinte de base du style reçu n'est
  // pas la nôtre, donc ni la fusion ni le découpage ne le sont.
  bool preview_open_ = false;
  std::string preview_owner_;
  std::string preview_code_;
  ro::PaletteRecipe preview_recipe_;
  std::vector<uint8_t> preview_base_;
  ro::PaletteRamp preview_ramps_[ro::kMaxRamps];
  int preview_ramp_count_ = 0;
  int preview_dir_ = 0;

  // Le sprite de corps sur lequel l'aperçu se pose, relu sur l'ACTEUR.
  //
  // 🔴 Distinct de `body_path_`, qui n'existe que si l'éditeur a été ouvert —
  // or cette fenêtre s'ouvre depuis un lien de chat. En le partageant, l'aperçu
  // retombait sur le sprite déduit de (classe, sexe) et perdait les tenues de 3e
  // et 4e classe : le style s'affichait alors sur un corps qui n'était pas celui
  // du joueur, donc sur d'autres pièces que celles qu'il croyait voir.
  //
  // Il sert aussi de témoin de changement : tant qu'il ne bouge pas, rien n'est
  // reconstruit — une reconstruction coûte l'analyse complète d'un `.spr`.
  std::string preview_body_path_;

  // (Re)construit la base et les rampes de l'aperçu sur le corps courant.
  void RebuildStylePreview();

  // La palette du `.spr` seule, brute : l'ingrédient qu'on garde pour fusionner
  // les 553 palettes de vêtement sans relire le sprite à chaque vignette.
  std::vector<uint8_t> spr_palette_;

  // Le résultat de cette fusion, par numéro de palette. 🔴 Indispensable : sans
  // mémo, la grille relirait et refusionnerait 553 fichiers PAR FRAME.
  std::map<int, std::vector<uint8_t>> palette_preview_;

  // Page courante de la grille des teintes de base.
  //
  // 🔴 La pagination n'est pas cosmétique : chaque vignette est une TEXTURE
  // distincte (même sprite, autre palette). Les 553 d'un coup satureraient le
  // cache de `ui/sprite_view`, qui se mettrait à évincer en pleine frame des
  // vignettes déjà dessinées — cf. [[feedback_texture_release_defer_frame]].
  int palette_page_ = 0;

  // Orientation du pantin d'aperçu, 0..7. La molette la fait tourner.
  //
  // 🔴 Un aperçu de FACE ne suffit pas : beaucoup de pièces — la cape d'un
  // manteau, l'arrière d'une jupe, la nuque — n'existent que sur d'autres
  // orientations. Sans rotation, le joueur règle des pièces qu'il ne voit pas.
  int doll_dir_ = 0;

  std::string error_;
};

#endif  // BOURGEON_FEATURES_WINDOWS_PALETTE_EDITOR_H_
