#pragma once

#include <set>
#include <string>

#include "features/plugin.h"

// ── Les sons de sprite : ceux que le client n'a jamais joués, et ceux dont on
//    ne veut plus ─────────────────────────────────────────────────────────────
//
// Deux réglages indépendants, réunis ici parce qu'ils traitent du même sujet :
// ce qu'un sprite fait entendre.
//
// ── 1. « Rétablir les sons manquants » (first_frame_enabled_) ────────────────
//
// Chaque image d'un `.act` peut porter un événement sonore, et le client joue
// ceux des images qu'il vient de franchir. Au CHANGEMENT d'action il remet son
// curseur « dernière image sonorisée » à 0, puis joue les images curseur+1 …
// courante — l'image 0 est donc sautée :
//
//     Actor_PlayFrameSounds 0x00C47200
//       0x00C47254  MOV DWORD PTR [ESI+44],0   ; curseur <- 0
//                   XOR EDI,EDI
//                   …
//                   CMP EDI,EBX                ; EBX = image courante
//                   JGE (rien à jouer)
//                   INC EDI                    ; la boucle part de 1
//
// L'image 0 n'est atteinte que par l'AUTRE branche, celle où le curseur est
// PLUS LOIN que l'image courante. Deux situations l'y amènent :
//   • une animation qui BOUCLE — repos, marche ;
//   • une action REPOSÉE À L'IDENTIQUE. `ChildSprite_SetAction` (0x00C55DA0)
//     réécrit l'action (+0x34, +0x38) et remet l'image à 0 (+0x3C) sans toucher
//     au curseur (+0x44) : un monstre frappé alors qu'il joue déjà son animation
//     de coup reçu repasse donc par l'image 0, et la sonorise.
//
// 🔴 CE QUI RESTE MUET N'EST DONC PAS « toute action qui ne boucle pas », mais
// la première image d'une action qui vient d'en REMPLACER une autre. En jeu :
//   • les sons de coup reçu et d'attaque s'entendent DÉJÀ dès le deuxième coup
//     d'affilée — seul le tout premier manque, ce qui ne s'entend pas ;
//   • les sons de MORT ne s'entendent JAMAIS : l'action de mort succède toujours
//     à une autre, et n'est jamais reposée.
//
// Relevé sur les 2103 sprites de monstres du data.grf du client : **47 sprites
// portent leur son de mort sur cette première image**, et ne le font donc jamais
// entendre. Le cas le plus connu est le Bio Lab — Seyren, Eremes, Katrinn et les
// autres ont une animation de mort d'UNE SEULE image portant
// `doppleganger_die.wav` — et le Doppelganger lui-même, qui partage ce son.
//
// (95 autres sprites posent de même un son sur la première image de leur
// animation de coup reçu, et 11 sur celle de leur attaque. Ceux-là s'entendent
// déjà : le correctif ne leur ajoute que le premier coup.)
//
// Le correctif tient en neuf octets : le curseur repart de -1, donc la boucle
// de l'image 0. Posé et retiré à chaud, sans relancer le client. Rien d'autre ne
// change : la branche du rebouclage n'est pas touchée, et aucun son ne peut être
// entendu deux fois puisque le curseur est réécrit à la fin de chaque appel.
//
// ── 2. « Sons à taire » (muted_wavs_) ────────────────────────────────────────
//
// Le réglage ci-dessus n'est pas la seule raison d'avoir cette liste : les sons
// de coup reçu, eux, s'entendent déjà et peuvent lasser sans qu'on ait rien
// activé. Plutôt que de tout subir ou de couper le son du jeu, un détour de
// `Sound_Play3D` (0x00600770) refuse les `.wav` dont le nom est dans la liste.
//
// ⚠ CE FILTRE VAUT POUR TOUT LE SON DU CLIENT, pas seulement pour les sprites :
// c'est le seul chemin par lequel un `.wav` positionné est joué. C'est délibéré
// — un son d'interface pénible s'y tait aussi bien — mais il faut le savoir
// avant d'y mettre un nom générique.
//
// Les deux réglages sont indépendants : taire un son n'exige pas d'avoir rétabli
// quoi que ce soit, et le détour reste posé même quand la liste est vide (il ne
// coûte alors qu'une comparaison de chaîne par son joué).
class SpriteSoundTweaks : public Plugin {
 public:
  SpriteSoundTweaks();

  // Le detour de Sound_Play3D interroge ce module par le pointeur que tient
  // Bourgeon ; laisse en place, il lirait un objet detruit au premier son joue
  // apres le dechargement. On le retire ici, et on rend au client ses neuf
  // octets d'origine — dans cet ordre.
  ~SpriteSoundTweaks() override;

  const char* name() const override { return "SpriteSoundTweaks"; }

  // Le patch se pose ICI et non à la construction : le yaml n'est lu qu'APRÈS
  // l'enregistrement des modules, et le tick est le premier endroit qui voie
  // l'état voulu une fois les réglages chargés. Ne fait rien tant que l'état
  // posé est déjà le bon — donc pas de VirtualProtect toutes les 100 ms.
  void OnTick() override;

  // Panneau « Gameplay > Sons des monstres ». true quand un réglage a changé et
  // qu'il faut réécrire le yaml.
  bool DrawSettings();

  // ── Réglages persistés par MoonlightUi (clés spritesound_*) ────────────────
  // Le défaut ne rétablit RIEN. Ces sons manquent depuis toujours : ils font
  // partie du jeu tel que les joueurs le connaissent, et les réveiller sans
  // qu'on l'ait demandé changerait l'ambiance sonore de tout le monde d'un
  // lancement à l'autre. Celui que ça intéresse va le chercher.
  bool first_frame_enabled_ = false;
  // Noms de `.wav` en minuscules ASCII, tels qu'ils figurent dans le `.act`.
  std::set<std::string> muted_wavs_;

  // Interrogé par le détour de Sound_Play3D, pour CHAQUE son joué.
  bool IsMuted(const char* wav_name) const;

 private:
  void SetFirstFrameSounds(bool on);

  // Les neuf octets d'origine étaient bien là au démarrage. Faux = autre build,
  // ou quelqu'un d'autre a déjà patché ce site : on ne touche à rien.
  bool site_verified_ = false;
  bool patch_applied_ = false;
};
