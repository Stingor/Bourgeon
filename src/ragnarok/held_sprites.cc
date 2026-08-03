#include "ragnarok/held_sprites.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ui/spr_act.h"
#include "utils/log_console.h"

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
// Bouclier (emplacement 7). Signature (out, job, sexe, vue_bouclier, style).
// ⚠ Ce ne sont PAS les `Job_GetWeaponShield*Path` : ceux-là servent la couche
// voisine de l'ARME (emplacement 6). Vérifié dans
// `CActorSprite_BuildShield_Slot7` (0x00d401d0), qui appelle bien ces deux-ci.
constexpr uintptr_t kShieldAct = 0x00d5e1d0;
constexpr uintptr_t kShieldSpr = 0x00d5e240;
// Conversions identifiant d'objet -> classe d'arme / vue de bouclier.
constexpr uintptr_t kItemToWeaponClass = 0x00d8a1d0;  // -1 = pas une arme
constexpr uintptr_t kItemToShieldView  = 0x00d84850;
// Destructeur de std::string du client (__fastcall, ecx = la chaîne).
constexpr uintptr_t kStrDtor = 0x004f08f0;

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
using IdFn_t    = int(__stdcall*)(int);
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
  reinterpret_cast<DtorFn_t>(kStrDtor)(s);
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

// Appelle un constructeur à 6 arguments sous SEH (aucun objet C++ dans le __try).
bool BuildPath6(uintptr_t fn, int job, int sex, int cls, int view, char* dst,
                size_t dst_size) {
  __try {
    NativeStr s;
    reinterpret_cast<PathFn6_t>(fn)(&s, job, sex, cls, view, /*style=*/0);
    TakeAndStrip(&s, dst, dst_size);
    return dst[0] != '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    dst[0] = '\0';
    return false;
  }
}

// Idem à 5 arguments (les constructeurs de bouclier).
bool BuildPath5(uintptr_t fn, int job, int sex, int view, char* dst,
                size_t dst_size) {
  __try {
    NativeStr s;
    reinterpret_cast<PathFn5_t>(fn)(&s, job, sex, view, /*style=*/0);
    TakeAndStrip(&s, dst, dst_size);
    return dst[0] != '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    dst[0] = '\0';
    return false;
  }
}

int CallIdFn(uintptr_t fn, int item_id) {
  __try {
    return reinterpret_cast<IdFn_t>(fn)(item_id);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

}  // namespace

bool ResolveHeldSprites(int job, int sex, int weapon_id, int shield_id,
                        HeldSpritePaths* out) {
  if (!out) return false;
  *out = HeldSpritePaths{};
  if (sex != 0 && sex != 1) return false;

  bool any = false;

  // Trace de mise au point, émise UNE fois par combinaison : la chaîne compte
  // quatre étapes (identifiant -> classe -> chemin -> fichier présent) et seul le
  // journal dit laquelle a cédé. Sans ce garde-fou, ce serait 25 places × 60 Hz.
  static int s_last_job = -1, s_last_sex = -1, s_last_wp = -1, s_last_sh = -1;
  const bool trace = (job != s_last_job || sex != s_last_sex ||
                      weapon_id != s_last_wp || shield_id != s_last_sh);
  if (trace) {
    s_last_job = job;
    s_last_sex = sex;
    s_last_wp = weapon_id;
    s_last_sh = shield_id;
  }

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
    if (trace)
      LogDiag("[Held] job={} sexe={} vue d'arme={} -> {} (présent={})", job, sex,
              weapon_id, spr, ok);
    if (!ok) {
      view = -1;
      ok = BuildPath6(kWeaponSpr, job, sex, wclass, view, spr, sizeof(spr)) &&
           ActExists(spr);
      if (trace)
        LogDiag("[Held]   repli par classe: {} (présent={})", spr, ok);
    }
    // Une arme équipée qu'on n'arrive pas à dessiner est une anomalie, pas un
    // cas normal : on la signale au niveau ERREUR pour qu'elle sorte même quand
    // le client tourne en `--loglevel=warn`. Le dernier chemin essayé suffit
    // d'ordinaire à dire ce qui manque.
    if (!ok && trace)
      LogError("[Held] arme (vue {}) non résolue — dernier chemin: {}",
               weapon_id, spr);
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
  // Même règle que l'arme : le champ est déjà une VUE, celle que le natif pose
  // dans `this+277` avant d'appeler ses constructeurs (cf.
  // `CActorSprite_BuildShield_Slot7`). Repli par identifiant d'objet ensuite.
  if (shield_id > 0) {
    char spr[260], act[260];
    int  view = shield_id;
    bool ok = BuildPath5(kShieldSpr, job, sex, view, spr, sizeof(spr)) &&
              ActExists(spr);
    if (trace)
      LogDiag("[Held] vue de bouclier={} -> {} (présent={})", shield_id, spr, ok);
    if (!ok) {
      const int as_item = CallIdFn(kItemToShieldView, shield_id);
      if (as_item > 0 && as_item != shield_id) {
        view = as_item;
        ok = BuildPath5(kShieldSpr, job, sex, view, spr, sizeof(spr)) &&
             ActExists(spr);
        if (trace)
          LogDiag("[Held]   repli objet: vue={} -> {} (présent={})", view, spr, ok);
      }
    }
    if (!ok && trace)
      LogError("[Held] bouclier (vue {}) non résolu — dernier chemin: {}",
               shield_id, spr);
    if (ok) {
      if (!BuildPath5(kShieldAct, job, sex, view, act, sizeof(act)))
        act[0] = '\0';
      std::memcpy(out->shield_spr, spr, sizeof(spr));
      std::memcpy(out->shield_act, act, sizeof(act));
      any = true;
    }
  }

  return any;
}

}  // namespace rag
