#pragma once

#include "features/plugin.h"

// ── La zone d'un sort, dessinée au sol ──────────────────────────────────────
//
// DEUX moments, et ils n'ont rien en commun sous le capot :
//
//   1. LE SORT EST ARMÉ et attend son clic au sol. Aucun paquet n'existe à cet
//      instant : c'est entièrement local — personne d'autre ne voit ce que tu
//      vises — et c'est ce module qui le dessine.
//   2. L'INCANTATION a commencé, le serveur a répondu. Là, le client sait déjà
//      tout faire ; ce module ne fait que réparer et filtrer.
//
// ── 2. L'incantation : le mécanisme « ShowScale » de kRO, et sa panne ────────
//
// Le serveur, quand un sort porte `INF2_SHOWSCALE`, émet `ZC_SKILL_SCALE`
// (opcode 0x0A41 : AID, skillId, skillLv, x, y, durée) ; le client y répond dans
// `EffectApply_SkillScaleEffect` 0x00cf8080, qui interroge la globale Lua
// `GetSkillScale(id, lv)` pour la taille en cellules, puis pose l'effet 1114
// (`CSquareRangeEffect`, texture `effect\SquareRange.tga`) sur chaque case, à la
// hauteur du terrain. Le rendu épouse donc le relief et vaut en DX7 comme DX9.
//
// 🔴🔴 MAIS IL NE MARCHE JAMAIS POUR SES PROPRES SORTS. La native résout
// l'acteur source avec `Actor_FindByGid` (0x00d806a0), qui ne parcourt que la
// LISTE des acteurs — or le client range le pantin du joueur À PART, en
// `actorMgr+0x2C`. Elle rend nullptr pour nous, et la fonction sort à sa
// première ligne, en silence. Personne ne l'avait vu parce que kRO ne pose
// `ShowScale` que sur des sorts de MVP : le cas « moi » n'existait pas.
//
// ⭐ Le client possède déjà la bonne porte : `ActorMgr_FindByGidOrSelf`
// (0x00a69e70) commence par `if (gid == g_Account_Aid) return *(this+0x2C)`. Le
// correctif tient donc en CINQ OCTETS : on réécrit le `call` de la native.
//
// 🔴 On patche LE SITE D'APPEL (0x00cf80b2), PAS `Actor_FindByGid` elle-même :
// cinq fichiers du projet et tout le natif l'appellent, et plusieurs comptent
// justement sur le fait qu'elle EXCLUE le joueur (« mon pet est-il sorti ? »,
// « ce membre de groupe est-il en vue ? »). Le site, lui, n'a qu'un client.
//
// ⭐ ET C'EST CE QUI DONNE LE FILTRE GRATUITEMENT. Notre remplaçant étant le
// passage obligé de la native, y rendre nullptr la fait sortir exactement comme
// avant : « ne pas montrer les sorts des joueurs » n'est pas un masquage, c'est
// le comportement d'origine rendu à l'identique.
//
// ── 1. Le ciblage : ce que le natif ne sait pas faire ───────────────────────
//
// Rien n'arrive du serveur tant qu'on n'a pas cliqué. Tout se lit donc dans le
// `CGameMode` : `+0x408` le mode de ciblage (1 = sol), `+0x40c` le sort armé,
// `+0x414` son niveau — les mêmes que QuickCast — et la case sous le curseur
// par `GameMode_PickGroundCellUnderMouse` 0x00c69a40, celle-là même que le clic
// natif résoudrait.
//
// ⭐ La TAILLE vient du même `GetSkillScale` que le natif interroge, pas d'une
// table à nous : la prévisualisation et le rendu d'incantation ne peuvent donc
// pas se contredire.
//
// Le dessin passe par `grey_world::AddScenePainter` : peindre une case n'est
// possible que dans la passe de scène du client, et c'est GreyWorld qui la
// détourne — inutile de reproduire ses cinquante lignes calquées sur un
// désassemblage. ⚠ S'inscrire POSE ses détours, mais ils restent inertes tant
// que GreyWorld lui-même est éteint.
//
// ── Ce que ce module ne fait pas ────────────────────────────────────────────
//
// Il ne décide pas QUELS sorts ont une zone — c'est le drapeau `ShowScale` du
// `skill_db` serveur — ni de QUELLE taille : ça vient de
// `SKILL_INFO_LIST[id].SkillScale[lv]`, que `tools/gen_skill_scale.py` dérive de
// `db/pre-re/skill_db.yml` et écrit dans les deux à la fois. Sans le drapeau
// aucun paquet ne part ; sans la table le client dessine du 0×0. Dans les deux
// cas rien ne se voit, et rien ne le signale.
//
// Cf. project_skill_aoe_preview_showscale.

namespace skill_range {

// ── État persistable ─────────────────────────────────────────────────────────
// Sérialisé champ par champ par MoonlightUi sous les clés « skillrange_* ».
// Les couleurs sont au format du picker ImGui — 4 floats RGBA.
struct Config {
  // ── Le sort ARMÉ, sous le curseur ─────────────────────────────────────────
  // 🔴 ÉTEINT PAR DÉFAUT. C'est une aide au visage découvert, pas le jeu tel
  // qu'il est : celui qui la veut la demande.
  bool preview = false;

  // Le dessin d'une case, les mêmes trois que GreyWorld — deux textures du
  // client, une à nous. Les valeurs sont celles de `Pattern`.
  enum Pattern { kPatternRing = 0, kPatternTile, kPatternSolid, kPatternCount };
  int pattern = kPatternTile;

  // Joint, en pourcentage du côté de la case. N'a de sens que sur le carreau
  // plein : creuser un cadre ou un anneau ne ferait que les rapetisser.
  int gap = 12;

  // Couleur ET opacité de la zone armée (RGBA du picker).
  float color[4] = {1.0f, 1.0f, 1.0f, 0.50f};

  // ── L'INCANTATION des sorts de JOUEURS ────────────────────────────────────
  // 🔴 ÉTEINT PAR DÉFAUT AUSSI, et ce n'est pas la même chose que ci-dessus :
  // cette zone-là est celle que le SERVEUR annonce, en `AREA`. L'allumer montre
  // les sorts de tout le monde autour de soi, le sien compris.
  //
  // ⚠ LES MONSTRES NE SONT PAS CONCERNÉS. Leur zone marche nativement depuis
  // toujours (leur acteur est dans la liste que la native parcourt) : on n'y
  // touche pas, ni pour l'allumer, ni pour l'éteindre.
  bool players = false;
};

Config& cfg();

// Section « Zone des sorts » du panneau Gameplay. Rend true si un réglage a
// changé (l'appelant persiste).
bool DrawSettings();

}  // namespace skill_range

// Posé une fois à la construction, sous garde du motif d'octets d'origine.
class SkillRangePatch : public Plugin {
 public:
  SkillRangePatch();

  const char* name() const override { return "SkillRangePatch"; }
};
