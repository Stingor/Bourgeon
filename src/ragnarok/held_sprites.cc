#include "ragnarok/held_sprites.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ragnarok/globals.h"  // rag::kStdStringDtorAddr
#include "ui/spr_act.h"

namespace rag {
namespace {

// ── Constructeurs de chemins du client (20250716, base 0x400000) ─────────────
// Tous __stdcall et tous de la même famille : ils REMPLISSENT un std::string
// fourni par l'appelant et le rendent.
//
//   Job_GetWeaponSpritePath(out, job, sexe, classe_arme, vue_ou_-1, style_corps)
//   Job_GetWeaponActPath   (idem)
// La « vue » est l'identifiant d'objet : passée, le nom vient de la table des
// vues (`Job_GetWeaponResNameByView`) ; à -1, du suffixe de classe. Le natif
// essaie la vue D'ABORD et retombe sur la classe si le fichier n'existe pas —
// c'est un des deux sondages annoncés dans own_actor.h, et on le refait ici.
constexpr uintptr_t kWeaponSpr = 0x00d8a010;
constexpr uintptr_t kWeaponAct = 0x00d8a160;
// Bouclier à variante DÉDIÉE (emplacement 7), celui qu'une classe dessine à sa
// façon. Signature (out, job, sexe, vue_bouclier, style) ; appelé par
// `CActorSprite_BuildShield_Slot7` (0x00d401d0).
//
// 🔴 Il rend une chaîne VIDE quand le couple (classe, vue) n'a pas de variante
// dédiée — et c'est le CAS NOMINAL, pas une anomalie : la plupart des boucliers
// sont génériques (`Shield_HasJobSpecificVariant` 0x00d72190 tranche).
constexpr uintptr_t kShieldAct = 0x00d5e1d0;
constexpr uintptr_t kShieldSpr = 0x00d5e240;
// Bouclier GÉNÉRIQUE (emplacement 6) : le repli du précédent, dessiné d'après le
// « seau » de bouclier. Signature (out, job, sexe, seau, vue, drapeau, style).
constexpr uintptr_t kShieldGenSpr = 0x00d8a080;
constexpr uintptr_t kShieldGenAct = 0x00d8a0f0;
// Destructeur de std::string du client (__fastcall, ecx = la chaîne).

// std::string de la CRT du client (msvcr110) : 16 octets de tampon interne
// réunis avec le pointeur, puis la taille et la capacité. 24 octets en tout.
// Au-delà de 15 caractères, le tampon cède la place à un pointeur — d'où le
// test sur la capacité à la lecture.
struct NativeStr {
  char     buf[16];
  uint32_t size;
  uint32_t capacity;
};

using PathFn5_t = void*(__stdcall*)(NativeStr*, int, int, int, int);
using PathFn6_t = void*(__stdcall*)(NativeStr*, int, int, int, int, int);
using PathFn7_t = void*(__stdcall*)(NativeStr*, int, int, int, int, int, int);
using DtorFn_t  = void(__fastcall*)(NativeStr*);

// Recopie la chaîne native dans `dst` en RETIRANT l'extension, puis la libère.
//
// Les constructeurs du client rendent un chemin complet (« …\x.spr ») alors que
// le composeur veut une base sans extension — il ajoute lui-même l'extension de
// chaque ressource.
//
// ⚠ Aucun objet C++ ici : la fonction est appelée depuis un __try (SEH), où un
// destructeur ferait échouer la compilation (C2712).
// 🔴 Le chemin rendu est relatif à la racine des SPRITES : les couches natives
// ajoutent « sprite\ » (selon l'extension) puis « data\ » plus tard, et on les
// court-circuite. On pose donc `data\sprite\` nous-mêmes, exactement comme le
// fait `BodyBasePath` dans le composeur.
void TakeAndStrip(NativeStr* s, char* dst, size_t dst_size) {
  const char* src = (s->capacity >= 16) ? *reinterpret_cast<char**>(s) : s->buf;
  dst[0] = '\0';
  if (src && *src) {
    std::snprintf(dst, dst_size, "data\\sprite\\%s", src);
    char* dot = std::strrchr(dst, '.');
    // Uniquement l'extension du NOM : un dossier peut contenir un point.
    if (dot && !std::strchr(dot, '\\') && !std::strchr(dot, '/')) *dot = '\0';
  }
  reinterpret_cast<DtorFn_t>(rag::kStdStringDtorAddr)(s);
}

// Le fichier existe-t-il dans le VFS ? Sert au repli « vue -> classe », comme le
// natif qui sonde avec `UITextureMgr_ResourceExists`. On sonde le `.act` : il est
// minuscule à côté du `.spr`, et le composeur a besoin des deux de toute façon.
bool ActExists(const char* base) {
  if (!base || !*base) return false;
  std::vector<uint8_t> bytes;
  char path[300];
  std::snprintf(path, sizeof(path), "%s.act", base);
  return ro::spract::ReadFile(path, &bytes);
}

// ── L'enveloppe commune des trois constructeurs ──────────────────────────────
// Les trois natifs ne different que par leur ARITE ; tout le reste — la
// `NativeStr` de sortie, le depouillement, le SEH, le repli sur chaine vide —
// etait recopie a l'identique.
//
// 🔴 L'appel LUI-MEME reste chez chacun, en toutes lettres. Le fondre derriere
// un paquet d'arguments variadique cacherait la signature du natif, qui est
// justement ce qu'on veut pouvoir relire — c'est le meme arbitrage que pour les
// paquets de fil.
//
// ⚠ `NativeStr` est du POD : sans quoi la declarer dans le `__try` serait
// refuse (C2712, « __try dans une fonction qui exige un deroulement d'objet »).
template <typename Call>
bool BuildPathWith(char* dst, size_t dst_size, Call call) {
  __try {
    NativeStr s;
    call(&s);
    TakeAndStrip(&s, dst, dst_size);
    return dst[0] != '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    dst[0] = '\0';
    return false;
  }
}

// Le constructeur à 6 arguments.
bool BuildPath6(uintptr_t fn, int job, int sex, int cls, int view, char* dst,
                size_t dst_size) {
  return BuildPathWith(dst, dst_size, [=](NativeStr* s) {
    reinterpret_cast<PathFn6_t>(fn)(s, job, sex, cls, view, /*style=*/0);
  });
}

// Idem à 5 arguments (les constructeurs de bouclier).
bool BuildPath5(uintptr_t fn, int job, int sex, int view, char* dst,
                size_t dst_size) {
  return BuildPathWith(dst, dst_size, [=](NativeStr* s) {
    reinterpret_cast<PathFn5_t>(fn)(s, job, sex, view, /*style=*/0);
  });
}

// Constructeur du bouclier GÉNÉRIQUE : un argument de plus (le drapeau, que le
// natif laisse à 0 pour un bouclier ordinaire).
bool BuildPath7(uintptr_t fn, int job, int sex, int bucket, int view, char* dst,
                size_t dst_size) {
  return BuildPathWith(dst, dst_size, [=](NativeStr* s) {
    reinterpret_cast<PathFn7_t>(fn)(s, job, sex, bucket, view, /*flag=*/0,
                                    /*style=*/0);
  });
}

}  // namespace

bool ResolveHeldSprites(int job, int sex, int weapon_id, int shield_id,
                        HeldSpritePaths* out) {
  if (!out) return false;
  *out = HeldSpritePaths{};
  if (sex != 0 && sex != 1) return false;

  bool any = false;

  // ── Arme ────────────────────────────────────────────────────────────────
  //
  // 🔴 `CHARACTER_INFO` porte une VUE d'arme (une classe), pas un identifiant
  // d'objet : c'est le champ que le serveur remplit avec `status.weapon`. Il se
  // passe donc TEL QUEL comme classe, avec une vue à -1 pour que le nom vienne
  // du suffixe de classe.
  //
  // Mesuré : traitée en identifiant d'objet, la valeur 1 donnait
  // `Weapon_ItemIdToWeaponClass = 0` (l'objet 1 n'existe pas), donc un suffixe
  // VIDE, donc le chemin du CORPS — `…\초보자\초보자_남`, un fichier qui existe
  // mais qui n'est pas une arme.
  //
  // Le second essai (identifiant d'objet) reste en repli : si un jour ce champ
  // portait un id, la chaîne continuerait de fonctionner.
  if (weapon_id > 0) {
    char spr[260], act[260];
    // Les fichiers du GRF donnent la règle :
    //   data\sprite\인간족\초보자\초보자_남_양손도끼.spr
    // — le suffixe est le NOM de l'arme, pas un numéro de classe. C'est donc la
    // voie « par vue » (`Job_GetWeaponResNameByView`) qu'il faut d'abord, la vue
    // étant précisément ce que porte `CHARACTER_INFO`.
    //
    // Le suffixe de CLASSE reste en repli : certaines armes n'ont pas de sprite
    // propre et se rendent avec celui de leur catégorie.
    int  wclass = weapon_id;
    int  view   = weapon_id;
    bool ok = BuildPath6(kWeaponSpr, job, sex, wclass, view, spr, sizeof(spr)) &&
              ActExists(spr);
    if (!ok) {
      view = -1;
      ok = BuildPath6(kWeaponSpr, job, sex, wclass, view, spr, sizeof(spr)) &&
           ActExists(spr);
    }
    if (ok) {
      // L'.act suit la MÊME clé que le .spr retenu — sans quoi on marierait les
      // images d'une arme aux mouvements d'une autre.
      if (!BuildPath6(kWeaponAct, job, sex, wclass, view, act, sizeof(act)))
        act[0] = '\0';
      std::memcpy(out->weapon_spr, spr, sizeof(spr));
      std::memcpy(out->weapon_act, act, sizeof(act));
      any = true;
    }
  }

  // ── Bouclier ────────────────────────────────────────────────────────────
  //
  // Deux emplacements, dans cet ordre :
  //   7 — la variante DÉDIÉE à la classe, quand elle existe ;
  //   6 — le bouclier GÉNÉRIQUE, dessiné d'après son « seau ».
  //
  // 🔴 L'emplacement 7 rend une chaîne VIDE quand le couple (classe, vue) n'a
  // pas de variante dédiée, et c'est le cas NOMINAL — la plupart des boucliers
  // sont génériques. Ce n'est donc pas une anomalie à signaler : c'est le signal
  // qu'il faut demander l'autre emplacement.
  if (shield_id > 0) {
    char spr[260], act[260];
    bool ok = BuildPath5(kShieldSpr, job, sex, shield_id, spr, sizeof(spr)) &&
              ActExists(spr);
    if (ok) {
      if (!BuildPath5(kShieldAct, job, sex, shield_id, act, sizeof(act)))
        act[0] = '\0';
    } else {
      // Générique. Le « seau » vient normalement de
      // `CActorSprite_ResolveShieldBucket`, qui interroge l'ACTEUR pour quelques
      // classes montées ; sans acteur, on prend sa branche par défaut — le seau
      // vaut la vue. Les cas particuliers retomberont sur « pas de bouclier ».
      ok = BuildPath7(kShieldGenSpr, job, sex, shield_id, -1, spr, sizeof(spr)) &&
           ActExists(spr);
      if (ok && !BuildPath7(kShieldGenAct, job, sex, shield_id, -1, act,
                            sizeof(act)))
        act[0] = '\0';
    }
    if (ok) {
      std::memcpy(out->shield_spr, spr, sizeof(spr));
      std::memcpy(out->shield_act, act, sizeof(act));
      any = true;
    }
  }

  return any;
}

}  // namespace rag
