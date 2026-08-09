#pragma once

// ── Homoncule : état et compétences, lus dans le client ──────────────────────
//
// Le client ne range RIEN dans un objet : tout l'état de l'homoncule vit dans un
// bloc de globals plats (0x015FF918…), écrit par le décodeur de ZC_PROPERTY_HOMUN
// 0x0BA4. Ses compétences, elles, sont dans une `std::list` SÉPARÉE (session+0x64) —
// pas dans le bundle du personnage : leurs ids commencent à HM_SKILLBASE = 8001 et
// n'apparaissent jamais au Grimoire. RE complet : docs/homunculus_re.md.
//
// POURQUOI UN MODULE. Deux consommateurs, et pas les mêmes besoins : l'onglet
// Homoncule de la feuille de personnage (qui remplace les fenêtres natives 113 et
// 114) et la barre de raccourcis (qui doit reconnaître un skill d'homoncule posé
// dans une case, et le lancer par le bon chemin). Recopier le parcours de liste
// dans les deux est exactement la dispersion que l'annuaire d'adresses a coûté à
// nettoyer — cf. le même raisonnement dans `player_skills.h`.
//
// ⚠ En-tête volontairement sans <Windows.h> : les lectures sont protégées par SEH,
// et un `__try` est interdit dans toute fonction abritant un objet à destructeur non
// trivial (C2712). Les corps vivent donc dans le .cc, jamais en inline.

#include <cstdint>

namespace rag {
namespace homun {

// Bornes serveur (skill.hpp) : HM_SKILLBASE = 8001 (HLIF_HEAL), MAX_HOMUNSKILL = 43.
constexpr int kSkillBase  = 8001;
constexpr int kSkillCount = 43;
// Un id de compétence appartient-il à l'homoncule ? C'est le test SKILL_CHK_HOMUN du
// serveur, et c'est lui qui décide du chemin de lancement : le remplisseur natif
// (0x00D7FA90) ne connaît que le bundle du PERSONNAGE, un id d'homoncule y est
// « introuvable » et le repli lancerait la compétence SUR LE JOUEUR.
constexpr bool IsSkillId(int id) { return id >= kSkillBase && id < kSkillBase + kSkillCount; }

// Marge de parcours (le serveur en envoie au plus 43).
constexpr int kMaxSkills = 64;

// Drapeaux du champ `flags` du paquet (bit field côté serveur).
constexpr int kFlagRenamed = 0x1;  // le renommage a déjà été utilisé (une seule fois)
constexpr int kFlagResting = 0x2;  // homoncule au repos (vaporized)
constexpr int kFlagAlive   = 0x4;  // PV > 0

// Une compétence de l'homoncule. `pos` = rang dans la liste BRUTE, entrées invalides
// comprises : c'est lui qu'attend l'accesseur natif par index, alors qu'un indice de
// tableau filtré désignerait la mauvaise compétence.
struct Skill {
  int id = 0;
  int inf = 0;          // masque de ciblage ; 0 = passive
  int level = 0;        // niveau APPRIS
  int sp = 0;           // coût en SP au niveau appris
  int range = 0;        // portée au niveau appris
  int upgradable = 0;   // 1 = peut encore monter (verdict SERVEUR)
  int pos = 0;
};

// Fiche d'état complète, POD pur.
struct State {
  int aid = 0;
  int cls = -1;         // classe / job id ; -1 = le client ne connaît aucun homoncule
  int level = 0;
  int hp = 0, max_hp = 0, sp = 0, max_sp = 0;
  int atk = 0, matk = 0, hit = 0, crit = 0, def = 0, mdef = 0, flee = 0;
  int amotion = 0;      // ASPD = (2000 - amotion) / 10 — le paquet ne porte pas l'ASPD
  int intimacy = 0;     // 0..1000 (le serveur envoie son intimité / 100)
  int hunger = 0;       // 0..100
  int flags = 0;
  int range = 0;
  int skill_points = 0;
  bool auto_feed = false;
  long long exp = 0;
  long long exp_next = 0;  // 0 = niveau maximum atteint
  char name[32] = {};
  char job[32] = {};    // nom de ressource de la classe (jobname.lub), ex. « AMISTR »
};

// Un homoncule est-il invoqué ? MÊME garde que le raccourci natif Alt+R (le drapeau
// posé par ZC_CHANGESTATE_MER state 0), doublée du test de classe que fait
// `MakeWindow` avant de créer la fenêtre 113.
bool Present();

// Remplit `out`. Renvoie false (et laisse `out` par défaut) si le client ne connaît
// aucun homoncule ou si la lecture échoue.
bool ReadState(State* out);

// Copie jusqu'à `cap` compétences ; renvoie le nombre écrit.
int ReadSkills(Skill* out, int cap);

// Niveau appris de `skill_id`, 0 si l'homoncule ne l'a pas (ou n'existe pas).
int SkillLevel(int skill_id);

// Lance la compétence par le chemin NATIF : l'ItemSkillInfo pris dans la liste de
// l'homoncule, puis la cmd 0x71 du dispatcher, qui route sur l'INF — pour INF 4
// (« sur soi ») elle vise bien l'homoncule et non le joueur. Renvoie false si la
// compétence est introuvable. `level` est borné au niveau appris, comme le natif.
bool LaunchSkill(int skill_id, int level);

// 🔴 LE MÉNAGE QUE FAISAIT LA FENÊTRE NATIVE 113. Sa commande « supprimer » ne se
// contentait pas d'envoyer le paquet : elle vidait le bloc d'ordre à l'homoncule
// (six dwords, ctx+0x55DC) et remettait le drapeau « j'ai un homoncule » à zéro. Le
// serveur n'envoie AUCUN signal dont le client tirerait ce nettoyage. Qui remplace
// cette fenêtre hérite du geste — sans lui, le client garde un ordre en attente vers
// un acteur qui n'existe plus, et l'interface croit l'homoncule toujours là.
void NotifyDeleted();

}  // namespace homun
}  // namespace rag
