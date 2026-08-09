#include "ui/emoji_set.h"

namespace ro {
namespace emoji {
namespace {

// ── 🔴 POURQUOI DES ÉCHAPPEMENTS `\U…` ET PAS LES EMOJI EN CLAIR ─────────────
//
// Les sources du projet sont en UTF-8 SANS BOM, et le build ne passe pas
// `/utf-8` à MSVC : le compilateur lit donc les fichiers dans la code-page du
// système (1252 ici). Les accents des commentaires et des chaînes y survivent
// par un heureux hasard — chaque octet UTF-8 d'un « é » (C3 A9) existe aussi en
// 1252, donc il traverse la conversion inchangé.
//
// Les emoji, eux, N'ONT PAS CETTE CHANCE. « 😍 » s'encode F0 9F 98 8D, et
// l'octet 0x8D n'est assigné à AUCUN caractère en 1252 : MSVC le remplace par
// « ? » (avertissement C4566, noyé dans le bruit du build). L'emoji écrit en
// clair arriverait donc corrompu dans le binaire, et le losange à l'écran
// enverrait chercher le bug du côté de la police ou de l'atlas — là où il n'est
// pas.
//
// `"\U0001F60D"` ne dépend d'aucun encodage de fichier : le compilateur produit
// la séquence UTF-8 exacte. Le nom en fin de ligne remplace la lisibilité perdue.
//
// ⚠ Deux règles pour ajouter une entrée :
//   1. PAS de sélecteur de variante U+FE0F, même si les tables Unicode
//      l'écrivent. Il est filtré à l'affichage (il occuperait la largeur d'un
//      emoji entier, cf. ro::WireToUtf8) et la couleur vient de la police, pas
//      de lui.
//   2. PAS de séquence à plusieurs points de code — drapeaux (deux indicateurs
//      régionaux), familles et métiers assemblés au ZWJ, teintes de peau. ImGui
//      ne fait pas de shaping : elles s'afficheraient en morceaux.

// Visages (80).
const char* const kFaces[] = {
    "\U0001F600", "\U0001F603", "\U0001F604", "\U0001F601", // grinning, open mouth, open mouth and eye, grinning eyes
    "\U0001F606", "\U0001F605", "\U0001F923", "\U0001F602", // open mouth and tig, open mouth and col, rolling on the flo, tears of joy
    "\U0001F642", "\U0001F643", "\U0001F609", "\U0001F60A", // slightly, upside-down, winking, eyes
    "\U0001F607", "\U0001F970", "\U0001F60D", "\U0001F929", // halo, eyes and three hea, heart-shaped eyes, grinning star eyes
    "\U0001F618", "\U0001F61A", "\U0001F60B", "\U0001F61B", // throwing a kiss, kissing closed eye, savouring deliciou, stuck-out tongue
    "\U0001F61C", "\U0001F92A", "\U0001F61D", "\U0001F911", // stuck-out tongue a, grinning one large, stuck-out tongue a, money-mouth
    "\U0001F917", "\U0001F92D", "\U0001F914", "\U0001F910", // hugging, eyes and hand cove, thinking, zipper-mouth
    "\U0001F928", "\U0001F610", "\U0001F636", "\U0001F60F", // one eyebrow raised, neutral, without mouth, smirking
    "\U0001F612", "\U0001F644", "\U0001F62C", "\U0001F634", // unamused, rolling eyes, grimacing, sleeping
    "\U0001F60C", "\U0001F614", "\U0001F62A", "\U0001F924", // relieved, pensive, sleepy, drooling
    "\U0001F637", "\U0001F912", "\U0001F922", "\U0001F92E", // medical mask, thermometer, nauseated, open mouth vomitin
    "\U0001F975", "\U0001F60E", "\U0001F913", "\U0001F9D0", // overheated, sunglasses, nerd, monocle
    "\U0001F615", "\U0001F61F", "\U0001F641", "\U0001F62E", // confused, worried, slightly frowning, open mouth
    "\U0001F632", "\U0001F633", "\U0001F97A", "\U0001F628", // astonished, flushed, pleading eyes, fearful
    "\U0001F630", "\U0001F625", "\U0001F622", "\U0001F62D", // open mouth and col, disappointed but r, crying, loudly crying
    "\U0001F631", "\U0001F616", "\U0001F61E", "\U0001F613", // screaming in fear, confounded, disappointed, cold sweat
    "\U0001F629", "\U0001F62B", "\U0001F971", "\U0001F624", // weary, tired, yawning, look of triumph
    "\U0001F621", "\U0001F620", "\U0001F92C", "\U0001F608", // pouting, angry, serious symbols co, horns
    "\U0001F47F", "\U0001F480", "\U00002620", "\U0001F4A9", // imp, skull, skull and crossbon, pile of poo
    "\U0001F921", "\U0001F47B", "\U0001F47D", "\U0001F916", // clown, ghost, extraterrestrial a, robot
};

// Mains (20).
const char* const kHands[] = {
    "\U0001F44D", "\U0001F44E", "\U0001F44C", "\U0000270C", // thumbs up sign, thumbs down sign, ok hand sign, victory hand
    "\U0001F91E", "\U0001F91D", "\U0001F44F", "\U0001F64C", // hand with index an, handshake, clapping hands sig, person raising bot
    "\U0001F64F", "\U0001F4AA", "\U0001F44B", "\U0000270B", // person with folded, flexed biceps, waving hand sign, raised hand
    "\U0001F91A", "\U0001F446", "\U0001F447", "\U0001F448", // raised back of han, white up pointing , white down pointin, white left pointin
    "\U0001F449", "\U0000261D", "\U0001F919", "\U0001F590", // white right pointi, white up pointing , call me hand, raised hand with f
};

// Symboles (24).
const char* const kSymbols[] = {
    "\U00002764", "\U0001F9E1", "\U0001F49B", "\U0001F49A", // heavy black heart, orange heart, yellow heart, green heart
    "\U0001F499", "\U0001F49C", "\U0001F5A4", "\U0001F90D", // blue heart, purple heart, black heart, white heart
    "\U0001F494", "\U0001F495", "\U0001F4AF", "\U00002728", // broken heart, two hearts, hundred points sym, sparkles
    "\U00002B50", "\U0001F31F", "\U0001F4AB", "\U000026A1", // white medium star, glowing star, dizzy symbol, high voltage sign
    "\U0001F525", "\U0001F4A5", "\U00002757", "\U00002753", // fire, collision symbol, heavy exclamation , black question mar
    "\U00002705", "\U0000274C", "\U000026A0", "\U0001F6AB", // white heavy check , cross mark, warning sign, no entry sign
};

// Aventure (24). La seule catégorie vraiment pensée pour le jeu : c'est là qu'on
// ajoute ce qui parle à un joueur de RO plutôt que ce qu'Unicode range ensemble.
const char* const kAdventure[] = {
    "\U00002694", "\U0001F6E1", "\U0001F3F9", "\U0001F5E1", // crossed swords, shield, bow and arrow, dagger knife
    "\U0001F4B0", "\U0001F48E", "\U0001F9EA", "\U0001F4DC", // money bag, gem stone, test tube, scroll
    "\U0001F5DD", "\U0001F451", "\U0001F3AF", "\U0001F3B2", // old key, crown, direct hit, game die
    "\U0001F3C6", "\U0001F947", "\U0001F381", "\U0001F514", // trophy, first place medal, wrapped present, bell
    "\U0000231B", "\U0001F9D9", "\U0001F409", "\U0001F9B4", // hourglass, mage, dragon, bone
    "\U00002697", "\U0001F52E", "\U0001FA84", "\U0001F9DA", // alembic, crystal ball, magic wand, fairy
};

// Nature (16).
const char* const kNature[] = {
    "\U0001F431", "\U0001F436", "\U0001F43A", "\U0001F98A", // cat, dog, wolf, fox
    "\U0001F43B", "\U0001F414", "\U0001F987", "\U0001F577", // bear, chicken, bat, spider
    "\U0001F338", "\U0001F339", "\U0001F33B", "\U0001F340", // cherry blossom, rose, sunflower, four leaf clover
    "\U0001F319", "\U00002600", "\U0001F308", "\U00002744", // crescent moon, black sun with ray, rainbow, snowflake
};

// Nourriture (12).
const char* const kFood[] = {
    "\U0001F34E", "\U0001F347", "\U0001F356", "\U0001F357", // red apple, grapes, meat on bone, poultry leg
    "\U0001F35E", "\U0001F9C0", "\U0001F37A", "\U0001F37B", // bread, cheese wedge, beer mug, clinking beer mugs
    "\U0001F377", "\U00002615", "\U0001F370", "\U0001F36C", // wine glass, hot beverage, shortcake, candy
};

// Objets (16).
const char* const kObjects[] = {
    "\U0001F389", "\U0001F38A", "\U0001F3B5", "\U0001F3B6", // party popper, confetti ball, musical note, multiple musical n
    "\U0001F4A4", "\U0001F4AC", "\U0001F440", "\U0001F9E0", // sleeping symbol, speech balloon, eyes, brain
    "\U0001F680", "\U0001F6E0", "\U0001F527", "\U0001F4E6", // rocket, hammer and wrench, wrench, package
    "\U0001F4BB", "\U0001F4F1", "\U0001F579", "\U0001F3AE", // personal computer, mobile phone, joystick, video game
};

#define RO_EMOJI_CATEGORY(label, table) \
  { label, table, static_cast<int>(sizeof(table) / sizeof((table)[0])) }

const Category kCategories[] = {
    RO_EMOJI_CATEGORY("Visages", kFaces),
    RO_EMOJI_CATEGORY("Mains", kHands),
    RO_EMOJI_CATEGORY("Symboles", kSymbols),
    RO_EMOJI_CATEGORY("Aventure", kAdventure),
    RO_EMOJI_CATEGORY("Nature", kNature),
    RO_EMOJI_CATEGORY("Nourriture", kFood),
    RO_EMOJI_CATEGORY("Objets", kObjects),
};

#undef RO_EMOJI_CATEGORY

constexpr int kCategoryCount =
    static_cast<int>(sizeof(kCategories) / sizeof(kCategories[0]));

}  // namespace

int CategoryCount() { return kCategoryCount; }

const Category& CategoryAt(int index) {
  if (index < 0 || index >= kCategoryCount) return kCategories[0];
  return kCategories[index];
}

int Count() {
  int total = 0;
  for (const Category& cat : kCategories) total += cat.count;
  return total;
}

}  // namespace emoji
}  // namespace ro
