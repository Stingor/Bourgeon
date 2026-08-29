#include "features/fx/wand_bolt.h"

#include <Windows.h>
#include <mmsystem.h>  // timeGetTime (winmm) — la MÊME horloge que CArrowEffect

#include <cstdint>
#include <cstring>

#include "utils/log_console.h"
#include "ragnarok/item_db.h"  // itemdb::kItemIdToWeaponClassAddr
#include "utils/memory_patch.h"  // mem::WriteCode

namespace {

// ── Adresses (client 20250716, no-ASLR : addr Ghidra == live) ────────────────
constexpr uintptr_t kArrowSpawn     = 0x00c6d9d0;  // Arrow_SpawnProjectileToTarget

// 🔴 Arrow_SpawnProjectileToTarget a QUATRE appelants, pas un. Une recherche
// textuelle sur le listing n'en avait montré qu'un : ce sont les XREFS qui font
// foi. Patcher le seul 0x00d425ce donnait un détour posé mais JAMAIS appelé —
// et des flèches, évidemment.
//
// Les trois sites de TIR, tous de disposition identique (seul le registre qui
// porte l'acteur change, edi ou esi, ce qui nous est indifférent puisqu'on lit
// la pile) :
//
//     0x00d4245d  CActorSprite_SetMotion, juste après Lua IsItemUsingArrow  <- le tir d'arme
//     0x00d425ce  CActorSprite_SetMotion, chemin Job_NeedsLuaItemPosOffset / case 9
//     0x00d41c5f  sub_D41790 (variante de SetMotion)
//
// Le quatrième (0x00c45046, CActorSprite_SpawnJobEffect) n'est pas un tir
// d'arme : on n'y touche pas.
//
// Les trois sont patchés parce que le filtre d'arme les rend inoffensifs : un
// site qui n'est pas emprunté par une baguette rend kKeepNativeJob et le natif
// garde son projectile.
constexpr uintptr_t kArrowSpawnCalls[] = {
    0x00d4245d,
    0x00d425ce,
    0x00d41c5f,
};

// ── La classe du projectile ──────────────────────────────────────────────────
// `Arrow_SpawnProjectileToTarget` alloue 0x170 octets et y construit un
// CArrowEffect. Ses deux méthodes utiles sont des entrées de VTABLE, donc
// crochetables par une écriture de QUATRE OCTETS — ni relogement de code, ni
// prologue à recopier, ni assembleur :
//
//     slot 1 (+0x04)  CArrowEffect_Update  0x00dabb10   (une seule xref : ce slot)
//     slot 2 (+0x08)  CArrowEffect_OnMsg   0x00db01b0
//
// `Update` n'ayant QUE cette xref, aucun appel ne peut contourner le détour.
constexpr uintptr_t kArrowEffectVtable = 0x0109c3e8;
constexpr uintptr_t kArrowEffectUpdate = 0x00dabb10;
constexpr uintptr_t kArrowEffectOnMsg  = 0x00db01b0;
constexpr int kSlotUpdate = 1;
constexpr int kSlotOnMsg  = 2;

// ── Champs d'un CArrowEffect ─────────────────────────────────────────────────
constexpr int kEffet_RotationDeg  = 0x07c;  // int   : rotation ÉCRAN, en degrés
constexpr int kEffet_Depart       = 0x08c;  // uint  : timeGetTime du tir
constexpr int kEffet_RetardDepart = 0x160;  // float : attente avant départ, en PAS
constexpr int kEffet_Marque       = 0x16c;  // libre : notre marque

// « WAND ». Le constructeur (0x00da9e50) initialise jusqu'à +0x168 et laisse
// +0x16C tel que `operator new` l'a rendu — donc quelconque. C'est pourquoi
// OnMsg écrit la marque À COUP SÛR, y compris à zéro : sans cela, du garbage
// pourrait se relire comme une marque.
constexpr uint32_t kMarqueBaguette = 0x444e4157u;

// Le site d'appel passe `acteur + 0x10` comme PREMIER ARGUMENT PILE
// (`lea ecx,[edi+10h] / push ecx`), tandis que `ecx` porte la scène. On remonte
// donc de cette position à l'acteur, puis à son arme (`[edi+440h]`, vérifié aux
// quatre sites 0x00d42385 / 0x00d42502 / 0x00d42559 / 0x00d4257f).
constexpr int kPosToActor       = -0x10;
constexpr int kActorWeaponId    = 0x440;

// Au-delà de ce seuil le champ d'arme est un ITEM ID à convertir ; en deçà,
// c'est déjà une classe d'arme. Même seuil que le client (0x00d42394).
constexpr int kWeaponClassMax   = 105;

// Le job dont on emprunte le projectile. 1495 = STONE_SHOOTER ; les autres
// valeurs utilisables sont listées dans l'en-tête.
constexpr int kProjectileJob    = 1495;

// Sentinelle « ne touche à rien » : -1 n'est pas un job valide, et le client
// traite de toute façon toute valeur hors [1410, 22176] comme « flèche ».
constexpr int kKeepNativeJob    = -1;

// ── Notre propre sprite de projectile ───────────────────────────────────────
// Emprunter le job d'un tireur du jeu donne SON projectile ; on remplace ensuite
// le sprite par le nôtre, pour n'avoir plus à dépendre d'un fichier de Gravity —
// et pour laisser au vrai STONE_SHOOTER le sien, ce qu'une réécriture de la
// chaîne en .rdata (0x0109CC30) n'aurait pas permis.
//
// 🔴 Le chemin est en **CP949**, exactement dans la forme que le client garde en
// .rdata : il part du dossier monstre (`몬스터` = B8 F3 BD BA C5 CD), sans
// préfixe `data\sprite\`, et il porte son extension. Les octets sont écrits en
// échappement hexadécimal : le fichier source est en UTF-8, y taper le mot
// coréen produirait trois octets par caractère au lieu de deux.
constexpr char kSprPropre[] = "\xB8\xF3\xBD\xBA\xC5\xCD\\baguette_shot.spr";
constexpr char kActPropre[] = "\xB8\xF3\xBD\xBA\xC5\xCD\\baguette_shot.act";

// `__thiscall(this, chemin)`, `retn 4` toutes les deux. Elles relâchent la
// texture précédente avant de charger : les rappeler après le natif est un
// échange propre, pas une fuite.
constexpr uintptr_t kSetBodySpr = 0x00c58e90;  // CActorSprite_SetBodySprFromPath
constexpr uintptr_t kSetBodyAct = 0x00c55bf0;  // CActorSprite_SetBodyActFromPath

// ── Réglages du vol ──────────────────────────────────────────────────────────
// Diviseur appliqué à l'horloge du projectile : 1 = vitesse native, 2 = deux
// fois plus lent. Ralentir et durer plus longtemps sont ici la MÊME chose (cf.
// UpdateDetour), il n'y a donc qu'un nombre à régler.
constexpr float kFacteurLenteur = 3.0f;
static_assert(kFacteurLenteur > 0.0f, "un facteur nul figerait le projectile");

// ⛔ L'attente qui précède le départ, elle, NE suit PAS la lenteur — et c'est
// mesuré, pas choisi :
//
//   1. À faible ASPD elle est déjà longue (elle vaut `[acteur+0x58] × [acteur
//      +0x378]`, donc l'animation elle-même) ; la multiplier faisait partir la
//      boulette APRÈS la fin du geste.
//   2. Surtout, le client date les dégâts à `(attente + 8 pas) × 24 ms`
//      (`CActorSprite_ProcessDamageAction`, 0x00C5E1A0). Cette formule suppose
//      l'attente native : la changer désynchronise le coup sans rien pour le
//      rattraper.
//
// Le recul du départ rallongerait l'attente d'office, puisqu'elle se compte dans
// la même unité que le vol. On l'en défait donc explicitement ci-dessous, et
// SEULE la course ralentit.

// ── Les dégâts attendent l'arrivée de la boulette ────────────────────────────
// Le client ne montre PAS les dégâts à la réception du paquet : il empile un
// message DATÉ dans la file de la cible (`acteur+0x2A8`) et le rejoue à
// l'échéance. Et cette date tient déjà compte du projectile — c'est mesuré :
//
//     SetMotion         attente = [acteur+0x58] × [acteur+0x378]   (en pas)
//                       puis [+0x378] += 8 / [+0x58]     <- SEULEMENT si l'arme tire
//     ProcessDamage     base_ms = [+0x378] × [+0x58] × 24.0
//                               = (attente + 8 pas) × 24 = attente_ms + 192 ms
//
// Autrement dit le client diffère déjà les dégâts du temps de vol NATIF, pour
// tout le monde, joueurs compris. Les suppléments par job qu'on voit juste après
// (+192 pour l'Archer Squelette 1016, +408, +912) sont des rattrapages pour les
// mobs dont le projectile est plus lent encore.
//
// Notre boulette est dans ce cas. Le supplément à ajouter n'est donc PAS le vol
// entier mais le SURPLUS qu'on a introduit — sans quoi on décalerait de 192 ms
// de trop :
constexpr int kVolNatifMs = 192;  // 8 pas de 24 ms, cf. CArrowEffect_Update
constexpr int kSupplementDegatsMs =
    static_cast<int>(kVolNatifMs * (kFacteurLenteur - 1.0f));

// Le dernier calcul avant que l'échéance ne soit posée. Six octets, donc de la
// place pour un `E8 rel32` suivi d'un `nop` :
//
//     0x00C5E372  call dword ptr [__imp_timeGetTime]   FF 15 B0 17 FC 00
//     0x00C5E378  add  eax, [ebp+delai]
//
// Rendre l'heure PLUS le supplément revient exactement à l'ajouter au délai.
constexpr uintptr_t kDamageDueTimeCall = 0x00c5e372;
constexpr uint8_t kDamageDueTimeOctets[] = {0xFF, 0x15, 0xB0, 0x17, 0xFC, 0x00};

// Vrai entre la naissance d'une boulette et la pose de l'échéance des dégâts.
// L'ordre est GARANTI, et c'est ce qui dispense de retrouver l'attaquant :
// `ProcessDamageAction` appelle SetMotion(2) en 0x00C5E17E — donc notre détour
// de tir — puis date le message en 0x00C5E372. Même appel, même fil.
bool g_tir_baguette = false;

// Correction d'orientation, en degrés, ajoutée à la rotation d'écran du seul
// projectile de baguette.
//
// Ce n'est pas un défaut du code : le rendu additionne la rotation de l'acteur
// et celle de la couche du .act (`Actor_ComputeLayerQuad` : `[+0x7C] + couche[7]`),
// et applique le tout à UNE image — un projectile RO n'a pas huit directions
// dessinées, il en a une seule que le moteur fait pivoter. L'écart vient donc de
// l'ART : `stone_shooter_bullet` (une action, une frame, `miroir = 1`) n'est pas
// dessiné selon le même axe que la flèche générique.
//
// 180 = tête-bêche. Si le projectile part de travers plutôt qu'à l'envers,
// essayer 90 ou 270 : ce sont les seules valeurs plausibles, l'art étant
// axé sur les diagonales de l'écran.
constexpr int kRotationDeg = 270;

// Les douze classes d'arme de type bâton, telles que le client les numérote
// (`Weapon_IDs` dans weapontable.lub) : les deux types de base plus les dix
// sous-types qui se replient sur eux. C'est EXACTEMENT la liste ajoutée à
// `BowTypeList` côté GRF — les deux doivent rester d'accord.
constexpr int kWandClasses[] = {
    10,   // WEAPONTYPE_ROD
    23,   // WPCLASS_TWOHANDROD
    69,   // Arc_Wand
    70,   // Mighty_Staff
    71,   // Blessed_Wand
    72,   // Bone_Wand
    96,   // Staff_Of_Soul
    97,   // Wizardy_Staff
    99,   // FOXTAIL_BROWN
    100,  // FOXTAIL_GREEN
    101,  // CandyCaneRod
    102,  // FOXTAIL_METAL
};

using ItemIdToClassFn = int(__stdcall*)(int);

// Cible du saut final, en mémoire : un `jmp dword ptr [...]` ne consomme aucun
// registre, là où passer par eax obligerait à le libérer.
void* g_real_arrow_spawn = reinterpret_cast<void*>(kArrowSpawn);

// Lecture protégée du champ d'arme. Déportée pour que le `__try` ne cohabite
// pas avec un objet à destructeur (MSVC C2712).
bool ReadWeaponField(const void* pos, int* out) {
  __try {
    const uint8_t* actor = reinterpret_cast<const uint8_t*>(pos) + kPosToActor;
    *out = *reinterpret_cast<const int*>(actor + kActorWeaponId);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool ItemIdToWeaponClass(int item_id, int* out) {
  __try {
    *out = reinterpret_cast<ItemIdToClassFn>(itemdb::kItemIdToWeaponClassAddr)(item_id);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

}  // namespace

// ── Le corps du détour, en C ─────────────────────────────────────────────────
// Appelé une fois par tir, uniquement sur le chemin « cette arme tire quelque
// chose » — donc jamais pour une attaque de mêlée.
//
// `extern "C"` pour que le nom soit résoluble depuis l'assembleur en ligne sans
// dépendre de la décoration C++.
extern "C" int Bourgeon_WandProjectileJob(void* pos) {
  int weapon = 0;
  if (!ReadWeaponField(pos, &weapon))
    return kKeepNativeJob;

  // Pour un joueur, le champ porte l'item id (LOOK_WEAPON) : c'est le cas
  // NORMAL, et il faut la même conversion que le client.
  if (weapon > kWeaponClassMax && !ItemIdToWeaponClass(weapon, &weapon))
    return kKeepNativeJob;

  for (int wand_class : kWandClasses) {
    if (weapon == wand_class) {
      // Le jeton que Bourgeon_HeureEcheanceDegats consommera quelques
      // instructions plus loin, dans le même appel — et que OnMsgDetour lit au
      // passage pour ne toucher que NOTRE projectile.
      g_tir_baguette = true;
      return kProjectileJob;
    }
  }
  return kKeepNativeJob;
}

// ── L'échéance des dégâts ────────────────────────────────────────────────────
// Remplace le `call timeGetTime` de CActorSprite_ProcessDamageAction. Rend
// l'heure telle quelle pour tout le monde ; pour notre boulette, elle rend
// l'heure PLUS le temps de vol supplémentaire, ce qui repousse d'autant
// l'affichage des dégâts, l'animation de recul de la cible et le nombre.
//
// Le jeton est à USAGE UNIQUE. S'il restait posé — le seul cas connu est une
// sortie anticipée de ProcessDamageAction quand la cible n'existe plus — il ne
// coûterait qu'un unique coup affiché trop tard.
extern "C" uint32_t Bourgeon_HeureEcheanceDegats() {
  const uint32_t maintenant = ::timeGetTime();
  if (!g_tir_baguette)
    return maintenant;
  g_tir_baguette = false;
  return maintenant + kSupplementDegatsMs;
}

namespace {

// Rend la pile EXACTEMENT comme le `call` d'origine l'avait laissée — même
// adresse de retour, mêmes arguments, même `ecx` — après avoir éventuellement
// réécrit le dernier argument (le job). La vraie fonction fera son `retn 10h`.
//
// eax/ecx/edx sont volatils pour la convention C, donc sauvés autour de
// l'appel ; ebx/esi/edi/ebp sont préservés par la fonction C elle-même.
// Disposition de la pile une fois nos trois registres sauvés :
//
//     +0 edx   +4 ecx   +8 eax   +12 retour
//     +16 position   +20 targetGid   +24 duréeVol   +28 job
//
// 🔴 La position source est le PREMIER ARGUMENT PILE, pas `ecx` : le site
// d'appel fait `lea ecx,[edi+10h] / push ecx / mov ecx,eax`, donc `this` est la
// scène et la position passe par la pile. Avoir lu `ecx` ici a coûté un premier
// essai entier — on lisait l'arme d'un objet qui n'en a pas, l'`__except`
// attrapait, et le tir restait une flèche.
//
// Le corps reste en ASCII pur : un caractère multi-octets dans un bloc `__asm`
// n'a rien à y gagner et tout à y perdre.
__declspec(naked) void ArrowSpawnDetour() {
  __asm {
    push eax
    push ecx
    push edx
    mov  eax, [esp + 16]             // arg0 = position source (acteur+0x10)
    push eax
    call Bourgeon_WandProjectileJob
    add  esp, 4
    cmp  eax, -1
    je   garder_le_job_natif
    mov  [esp + 28], eax
  garder_le_job_natif:
    pop  edx
    pop  ecx
    pop  eax
    jmp  dword ptr [g_real_arrow_spawn]
  }
}

// ── Les deux méthodes crochetées ─────────────────────────────────────────────
// `Update` ne prend que `this` et rend un booléen « encore en vie » : `retn`
// sans immédiat. `OnMsg` finit sur `retn 34h`, soit TREIZE arguments pile
// nettoyés par l'appelé — c'est le désassemblage qui le dit, pas Hex-Rays, qui
// apparie mal les arguments 64 bits poussés en `cdq; push edx; push eax`.
using UpdateFn = int(__fastcall*)(void* self, void* edx);
using OnMsgFn  = int(__fastcall*)(void* self, void* edx, int a0, int msg,
                                  int sous, int a3, int a4, int a5, int a6,
                                  int a7, int a8, int job, int job_haut,
                                  int a11, int a12);

UpdateFn g_update_natif = reinterpret_cast<UpdateFn>(kArrowEffectUpdate);
OnMsgFn  g_onmsg_natif  = reinterpret_cast<OnMsgFn>(kArrowEffectOnMsg);

uint8_t* Champs(void* self) { return static_cast<uint8_t*>(self); }

using SetPathFn = int(__fastcall*)(void* self, void* edx, const char* chemin);

// Déportée pour que le `__try` n'ait aucun objet à dérouler (MSVC C2712).
void RemplacerSprite(void* self) {
  __try {
    reinterpret_cast<SetPathFn>(kSetBodySpr)(self, nullptr, kSprPropre);
    reinterpret_cast<SetPathFn>(kSetBodyAct)(self, nullptr, kActPropre);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

bool EstUneBaguette(void* self) {
  return *reinterpret_cast<const uint32_t*>(Champs(self) + kEffet_Marque) ==
         kMarqueBaguette;
}

// 14 et 64 sont les DEUX messages qui initialisent un projectile ; tout
// CArrowEffect qui atteindra `Update` est passé par l'un ou l'autre. On y écrit
// donc la marque sans condition — la mettre à zéro pour les autres est ce qui
// empêche un +0x16C jamais initialisé de se faire passer pour le nôtre.
int __fastcall OnMsgDetour(void* self, void* edx, int a0, int msg, int sous,
                           int a3, int a4, int a5, int a6, int a7, int a8,
                           int job, int job_haut, int a11, int a12) {
  const int resultat = g_onmsg_natif(self, edx, a0, msg, sous, a3, a4, a5, a6,
                                     a7, a8, job, job_haut, a11, a12);

  if (sous == 0 && (msg == 14 || msg == 64)) {
    // `job_haut` est l'extension de signe du job (le client la teste lui-même
    // avant la plage [1410, 22176]) : un job négatif n'est pas le nôtre.
    //
    // 🔴 Le job ne suffit PAS à nous reconnaître : un vrai STONE_SHOOTER tire
    // sous le même. D'où le jeton, posé au tir par Bourgeon_WandProjectileJob —
    // qu'on se contente de LIRE ici, sans le consommer : son consommateur
    // (Bourgeon_HeureEcheanceDegats) tourne plus loin dans le MÊME appel de
    // CActorSprite_ProcessDamageAction, après ce message 14.
    //
    // Le mob emprunté garde donc son projectile, sa vitesse et son orientation.
    const bool notre = (msg == 14 && job_haut == 0 && job == kProjectileJob
                        && g_tir_baguette);
    *reinterpret_cast<uint32_t*>(Champs(self) + kEffet_Marque) =
        notre ? kMarqueBaguette : 0u;

    // Le natif vient de poser le sprite du tireur emprunté ; on met le nôtre à
    // la place.
    if (notre)
      RemplacerSprite(self);

    // L'attente avant départ se compte dans la MÊME unité que le temps de vol :
    // le recul du départ la rallongerait donc d'autant. On l'annule ici pour que
    // la boulette parte à l'instant natif — au point du geste que le client a
    // choisi, et celui que sa formule de dégâts suppose (cf. kFacteurLenteur).
    if (notre)
      *reinterpret_cast<float*>(Champs(self) + kEffet_RetardDepart) /=
          kFacteurLenteur;
  }
  return resultat;
}

int __fastcall UpdateDetour(void* self, void* edx) {
  if (!EstUneBaguette(self))
    return g_update_natif(self, edx);

  uint8_t* champs = Champs(self);
  uint32_t* depart = reinterpret_cast<uint32_t*>(champs + kEffet_Depart);

  // Ralentir SANS toucher au client. `Update` ne lit le temps que par
  // `(timeGetTime() - [+0x8C]) / 24`, et tout en dépend : la position vaut
  // `origine + t x vitesse`, la fin de vie tombe à `t > 8 / echelle`. Reculer
  // l'instant de départ divise donc ce `t` unique — le vol devient exactement N
  // fois plus lent ET N fois plus long, sans qu'une moitié puisse se
  // désynchroniser de l'autre. Toucher la constante 24.0 du client (0x0100D328)
  // aurait été le même calcul, mais partagé avec tout le reste du moteur.
  const uint32_t vrai_depart = *depart;
  const uint32_t maintenant = ::timeGetTime();
  const uint32_t ecoule = maintenant - vrai_depart;  // sûr au repliement
  *depart = maintenant - static_cast<uint32_t>(ecoule / kFacteurLenteur);

  const int resultat = g_update_natif(self, edx);

  *depart = vrai_depart;  // on ne fait que PRÊTER l'objet : il repart intact

  // L'orientation, une fois le natif passé. `+0x7C` est une rotation d'ÉCRAN en
  // degrés, recalculée à chaque image et additionnée par le rendu à celle de la
  // couche du .act : la corriger ici ne touche que notre projectile.
  if (kRotationDeg != 0) {
    int* rotation = reinterpret_cast<int*>(champs + kEffet_RotationDeg);
    const int corrigee = (*rotation + kRotationDeg) % 360;
    *rotation = corrigee < 0 ? corrigee + 360 : corrigee;
  }
  return resultat;
}

// Le geste lui-même — vérifier ce que contient le slot, puis l'écrire — vit
// dans `mem::SwapVtableSlot` (utils/memory_patch.h) depuis qu'ItemDropArc en a
// eu besoin à son tour. Ne reste ici que le journal, qui porte le nom du module.
uintptr_t PoserSlot(uintptr_t slot, uintptr_t attendu, uintptr_t detour) {
  uintptr_t trouve = 0;
  const uintptr_t ancien = mem::SwapVtableSlot(slot, attendu, detour, &trouve);
  if (!ancien) {
    if (trouve != attendu)
      LogDiag("[WandBolt] slot 0x{:08X} : contient 0x{:08X} au lieu de 0x{:08X}, NON crochete",
              slot, trouve, attendu);
    else
      LogDiag("[WandBolt] slot 0x{:08X} : page non ouvrable, NON crochete", slot);
  }
  return ancien;
}

}  // namespace

WandBolt::WandBolt() {
  for (uintptr_t call_site : kArrowSpawnCalls) {
    // Chaque site doit être CELUI qu'on a lu : un `E8` dont la cible est bien
    // Arrow_SpawnProjectileToTarget. Sur toute autre disposition, on ne patche
    // rien — écrire cinq octets au hasard dans le chemin d'animation tuerait le
    // client.
    const uint8_t* site = reinterpret_cast<const uint8_t*>(call_site);
    const int32_t rel = *reinterpret_cast<const int32_t*>(call_site + 1);
    if (site[0] != 0xE8 || call_site + 5 + rel != kArrowSpawn) {
      // Le cas le plus probable d'un site inattendu : un patch WARP a réécrit
      // ces octets. On donne ce qu'on a lu, sinon le diagnostic tourne en rond.
      LogDiag("[WandBolt] site 0x{:08X} inattendu : octets {:02X} {:02X} {:02X} {:02X} {:02X}, "
              "cible calculee 0x{:08X} (attendu E8 -> 0x{:08X}) : site IGNORE",
              call_site, site[0], site[1], site[2], site[3], site[4],
              static_cast<uintptr_t>(call_site + 5 + rel), kArrowSpawn);
      continue;
    }

    uint8_t patch[5] = {0xE8, 0, 0, 0, 0};
    const int32_t new_rel = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&ArrowSpawnDetour) - (call_site + 5));
    std::memcpy(patch + 1, &new_rel, sizeof(new_rel));
    if (!mem::WriteCode(call_site, patch, sizeof(patch)))
      LogDiag("[WandBolt] page non ouvrable en 0x{:08X} : site IGNORE", call_site);
  }

  // Vitesse et orientation : les deux slots de vtable, dans cet ORDRE. `Update`
  // n'est crocheté que si `OnMsg` l'a été, car c'est OnMsg qui écrit la marque ;
  // sans elle, Update lirait un +0x16C non initialisé.
  const uintptr_t ancien_onmsg =
      PoserSlot(kArrowEffectVtable + kSlotOnMsg * 4, kArrowEffectOnMsg,
                reinterpret_cast<uintptr_t>(&OnMsgDetour));
  if (ancien_onmsg) {
    g_onmsg_natif = reinterpret_cast<OnMsgFn>(ancien_onmsg);
    const uintptr_t ancien_update =
        PoserSlot(kArrowEffectVtable + kSlotUpdate * 4, kArrowEffectUpdate,
                  reinterpret_cast<uintptr_t>(&UpdateDetour));
    if (ancien_update)
      g_update_natif = reinterpret_cast<UpdateFn>(ancien_update);
  }

  // Les dégâts, enfin, attendent la boulette. Rien à faire si on ne ralentit
  // pas : le client diffère déjà du vol natif.
  if (kSupplementDegatsMs <= 0)
    return;

  const uint8_t* site_degats = reinterpret_cast<const uint8_t*>(kDamageDueTimeCall);
  if (std::memcmp(site_degats, kDamageDueTimeOctets, sizeof(kDamageDueTimeOctets)) != 0) {
    LogDiag("[WandBolt] site des degats 0x{:08X} inattendu : "
            "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} : NON patche",
            kDamageDueTimeCall, site_degats[0], site_degats[1], site_degats[2],
            site_degats[3], site_degats[4], site_degats[5]);
    return;
  }

  // `E8 rel32` puis `nop` : le `call [__imp_...]` d'origine faisait six octets.
  uint8_t patch_degats[6] = {0xE8, 0, 0, 0, 0, 0x90};
  const int32_t rel_degats = static_cast<int32_t>(
      reinterpret_cast<uintptr_t>(&Bourgeon_HeureEcheanceDegats) -
      (kDamageDueTimeCall + 5));
  std::memcpy(patch_degats + 1, &rel_degats, sizeof(rel_degats));
  if (!mem::WriteCode(kDamageDueTimeCall, patch_degats, sizeof(patch_degats))) {
    LogDiag("[WandBolt] site des degats 0x{:08X} : page non ouvrable", kDamageDueTimeCall);
    return;
  }
}
