#pragma once

// ── Briques de chemin d'un sprite de PERSONNAGE ──────────────────────────────
//
// Trois jetons reviennent dans tous les gabarits du client — le dossier de
// SEXE, le dossier de RACE, et le prédicat qui choisit le second. Ils sont ici
// parce que le pantin (ui/doll.cc) et la vignette de tête (ui/head_icon.cc)
// composent chacun leurs chemins et se retrouvaient à en garder deux copies.
//
// 🔴 La RACE n'est pas une décoration : un Doram (Summoner) range TOUT sous un
// autre dossier racine. Un résolveur qui écrit `인간족` en dur ne rend pas un
// sprite approximatif — il ne rend RIEN, et le corps manquant emporte le pantin
// entier. C'est exactement le défaut qu'avait le Summoner.
namespace ro {

// Dossier de sexe, en CP949 : 남 (homme) / 여 (femme). `sex` : 0 = femme.
const char* SexFolder(int sex);

// La classe est-elle de la race DORAM (Summoner) ?
//
// Réponse du client — `Job_NeedsLuaItemPosOffset` (0x00d9cf80) — et pas d'une
// liste recopiée : c'est ce même prédicat qui gouverne, chez lui, le dossier de
// race, la table de coiffures et la variante de couvre-chef.
bool IsDoramJob(int job);

// Dossier de race, en CP949 : 도람족 (Doram) ou 인간족 (humain). Jamais nul.
const char* RaceFolder(int job);

}  // namespace ro
