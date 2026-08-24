#include "ragnarok/user_hotkey.h"

#include <Windows.h>

#include <cstring>

#include "ragnarok/globals.h"
#include "ragnarok/lua.h"
#include "ui/ro_imgui.h"

namespace userhotkey {
namespace {

// ── Adresses (client 20250716, no-ASLR : addr IDA == live) ───────────────────
// RE : docs/game_option_re.md §4.4 et §4.5.

// UserHotkey_TabIndexToCategory(tab) -> catégorie.
constexpr uintptr_t kTabIndexToCategoryAddr = 0x005d4c50;

// UserHotkey_Lua_GetOriginalListSize(catégorie) -> nombre de lignes affichables.
// Lua `GetOriginalHotKeyListSize(catégorie+1)` (format « d>d ») ; le natif
// retranche lui-même les douze trous de la catégorie Interface.
constexpr uintptr_t kGetOriginalListSizeAddr = 0x00d81680;

// UserHotkey_RowToCommandIndex(catégorie, ligne) -> index de commande, ou -1.
constexpr uintptr_t kRowToCommandIndexAddr = 0x00d83b20;

// UserHotkey_Lua_GetHotKey(out, catégorie, cmdIdx) : Lua `GetHotKey(cat+1, idx)`
// (format « dd>ddss »). Remplit une struct de 0x38 octets.
constexpr uintptr_t kGetHotKeyAddr = 0x00d80950;

// La struct de sortie de GetHotKey.
//
// 🔴 `out+0x20` est le LIBELLÉ de la commande (le champ EXE de UserKeys.lua, ex.
// « Hotkey 2-1 »), et PAS une seconde touche — croyance antérieure corrigée le
// 2026-08-12. Trois preuves concordantes : UIHotKeyWnd_OnDraw le dessine dans la
// colonne des libellés, le commit du natif le repasse à ChangeUserHotKey, et
// UserKeys.lua stocke bien `{ EXE = "Hotkey 2-1", KEY1 = 65 }`.
constexpr int kOffKeyCode1 = 0x00;
constexpr int kOffKeyCode2 = 0x04;
constexpr int kOffKeyName  = 0x08;  // std::string
constexpr int kOffLabel    = 0x20;  // std::string
constexpr int kOutSize     = 0x38;

// ── Écriture (docs/game_option_re.md §4.9) ──────────────────────────────────
// UserHotkey_Lua_ChangeHotKey : Lua `ChangeUserHotKey(cat+1, cmdIdx, nom, k1, k2)`.
// ⚠ L'ordre C n'est PAS l'ordre Lua : le std::string est le DERNIER argument C.
constexpr uintptr_t kChangeHotKeyAddr = 0x005d56d0;
// UserHotkey_Lua_SaveUserHotKeys2 : AUCUN argument, fabrique son chemin lui-même.
constexpr uintptr_t kSaveUserKeysAddr = 0x005d54c0;
// std_string_assign __thiscall(str, src, len) : gère SSO et tas avec l'allocateur
// du jeu — indispensable, un libellé peut dépasser les 15 caractères du SSO.

using ChangeHotKey_t = int(__stdcall*)(int, int, int, int, const void*);
using SaveUserKeys_t = int(__stdcall*)();
using StrAssign_t    = void*(__thiscall*)(void*, const void*, size_t);

using TabToCat_t   = int(__stdcall*)(int);
using ListSize_t   = int(__stdcall*)(int);
using RowToCmd_t   = int(__stdcall*)(int, int);
using GetHotKey_t  = void*(__stdcall*)(void*, int, int);
using StrDtor_t    = void(__fastcall*)(void*);

// Copie une std::string MSVC (base = l'objet) vers `dst`, terminée NUL, tronquée.
// Layout : +0x00 données ou pointeur, +0x10 taille, +0x14 capacité ; les données
// sont EN PLACE tant que la capacité est < 0x10, sinon `+0x00` est un pointeur.
void CopyMsvcString(const void* str_obj, char* dst, int cap) {
  dst[0] = '\0';
  if (!str_obj || cap <= 1) return;
  const auto* base = static_cast<const uint8_t*>(str_obj);
  const unsigned size     = *reinterpret_cast<const unsigned*>(base + 0x10);
  const unsigned capacity = *reinterpret_cast<const unsigned*>(base + 0x14);
  const char* src = (capacity >= 0x10)
                        ? *reinterpret_cast<const char* const*>(base)
                        : reinterpret_cast<const char*>(base);
  if (!src || size == 0) return;
  int n = static_cast<int>(size);
  if (n > cap - 1) n = cap - 1;
  std::memcpy(dst, src, static_cast<size_t>(n));
  dst[n] = '\0';
}

}  // namespace

int CategoryForTab(int tab_index) {
  if (tab_index < 0 || tab_index >= kCategoryCount) return kSkillBar1;
  __try {
    return reinterpret_cast<TabToCat_t>(kTabIndexToCategoryAddr)(tab_index);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return kSkillBar1; }
}

int RowCount(int category) {
  if (category < 0 || category >= kCategoryCount) return 0;
  __try {
    const int n = reinterpret_cast<ListSize_t>(kGetOriginalListSizeAddr)(category);
    return (n > 0) ? n : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

int CommandIndexAt(int category, int row) {
  if (category < 0 || category >= kCategoryCount || row < 0) return -1;
  __try {
    return reinterpret_cast<RowToCmd_t>(kRowToCommandIndexAddr)(category, row);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

bool ReadBinding(int category, int row, Binding* out) {
  if (!out) return false;
  *out = Binding();

  const int cmd = CommandIndexAt(category, row);
  if (cmd < 0) return false;
  out->command_index = cmd;

  // Tampons LOCAUX et POD only : le SEH de MSVC interdit de dérouler des objets
  // C++ dans la même fonction.
  char key_name_local[64] = {0};
  char label_local[128]   = {0};
  int  kc1 = 0;
  int  kc2 = 0;

  __try {
    alignas(4) uint8_t buf[kOutSize];
    std::memset(buf, 0, sizeof(buf));
    reinterpret_cast<GetHotKey_t>(kGetHotKeyAddr)(buf, category, cmd);

    kc1 = *reinterpret_cast<int*>(buf + kOffKeyCode1);
    kc2 = *reinterpret_cast<int*>(buf + kOffKeyCode2);
    CopyMsvcString(buf + kOffKeyName, key_name_local, sizeof(key_name_local));
    CopyMsvcString(buf + kOffLabel, label_local, sizeof(label_local));

    // ⚠ Les DEUX std::string sont allouées par le pont : ne pas les détruire fuit
    // dès qu'un libellé dépasse 15 caractères (hors SSO).
    reinterpret_cast<StrDtor_t>(rag::kStdStringDtorAddr)(buf + kOffKeyName);
    reinterpret_cast<StrDtor_t>(rag::kStdStringDtorAddr)(buf + kOffLabel);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }

  // Libellé vide = la commande n'existe pas : c'est le test de validité du natif
  // (UIHotKeyWnd_OnDraw saute la ligne sans même avancer son compteur).
  if (label_local[0] == '\0') return false;

  out->key_code1 = kc1;
  out->key_code2 = kc2;
  out->assigned  = (key_name_local[0] != '\0');

  // Le client parle sa propre code-page (CP949 ou CP1252 selon l'install) ; ces
  // deux textes viennent de lui, donc ils passent par la conversion du toolkit.
  const char* key_utf8   = ro::LocalToUtf8(key_name_local);
  const char* label_utf8 = ro::LocalToUtf8(label_local);
  if (key_utf8) {
    std::strncpy(out->key_name, key_utf8, sizeof(out->key_name) - 1);
    out->key_name[sizeof(out->key_name) - 1] = '\0';
  }
  if (label_utf8) {
    std::strncpy(out->label, label_utf8, sizeof(out->label) - 1);
    out->label[sizeof(out->label) - 1] = '\0';
  }
  return true;
}

bool ReadDefaultBinding(int category, int command_index, Binding* out) {
  if (!out) return false;
  *out = Binding();
  if (category < 0 || category >= kCategoryCount || command_index < 0) return false;
  void* lua_state = lua::State();
  if (!lua_state) return false;
  out->command_index = command_index;

  // Tampons POD : le SEH de MSVC interdit de dérouler des objets C++ ici.
  char key_name_local[64] = {0};
  char label_local[128]   = {0};
  int  kc1 = 0;
  int  kc2 = 0;
  bool ok = false;

  __try {
    // 4 valeurs de retour + la fonction + 2 arguments : on demande la place.
    if (lua::CheckStack(lua_state, 8)) {
      lua::GetField(lua_state, lua::kGlobalsIndex, "GetOriginalHotKeyInfo");
      lua::PushNumber(lua_state, category + 1);  // le Lua est 1-based
      lua::PushNumber(lua_state, command_index);
      // Format natif « dd>ddss » : deux entrées, quatre sorties dans cet ordre —
      // code de touche, modificateur, NOM de touche, LIBELLÉ de la commande.
      if (lua::PCall(lua_state, 2, 4, 0) == 0) {
        kc1 = static_cast<int>(lua::ToNumber(lua_state, -4));
        kc2 = static_cast<int>(lua::ToNumber(lua_state, -3));
        const char* key_name = lua::ToLString(lua_state, -2, nullptr);
        const char* label    = lua::ToLString(lua_state, -1, nullptr);
        if (key_name) {
          std::strncpy(key_name_local, key_name, sizeof(key_name_local) - 1);
          key_name_local[sizeof(key_name_local) - 1] = '\0';
        }
        if (label) {
          std::strncpy(label_local, label, sizeof(label_local) - 1);
          label_local[sizeof(label_local) - 1] = '\0';
        }
        lua::Pop(lua_state, 4);
        ok = true;
      } else {
        // Échec : Lua a empilé un message d'erreur à la place des résultats.
        lua::Pop(lua_state, 1);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }

  // Même test de validité que le natif : libellé vide = la commande n'existe pas.
  if (!ok || label_local[0] == '\0') return false;

  out->key_code1 = kc1;
  out->key_code2 = kc2;
  out->assigned  = (key_name_local[0] != '\0');

  const char* key_utf8   = ro::LocalToUtf8(key_name_local);
  const char* label_utf8 = ro::LocalToUtf8(label_local);
  if (key_utf8) {
    std::strncpy(out->key_name, key_utf8, sizeof(out->key_name) - 1);
    out->key_name[sizeof(out->key_name) - 1] = '\0';
  }
  if (label_utf8) {
    std::strncpy(out->label, label_utf8, sizeof(out->label) - 1);
    out->label[sizeof(out->label) - 1] = '\0';
  }
  return true;
}

bool WriteBinding(int category, int command_index, int key1, int key2,
                  const char* label_utf8) {
  if (category < 0 || category >= kCategoryCount || command_index < 0)
    return false;

  // Retour vers la code-page du client : le libellé est reparti tel quel dans
  // UserKeys.lua, et un aller-retour UTF-8 non défait le corromprait.
  const char* local = ro::Utf8ToLocal(label_utf8 ? label_utf8 : "");
  if (!local) local = "";
  const size_t len = std::strlen(local);

  bool ok = false;
  __try {
    // std::string MSVC : {données ou pointeur, taille @+0x10, capacité @+0x14}.
    // On part d'un SSO vide et on laisse le natif grandir si besoin.
    alignas(4) uint8_t str[0x18];
    std::memset(str, 0, sizeof(str));
    *reinterpret_cast<unsigned*>(str + 0x14) = 15;
    reinterpret_cast<StrAssign_t>(rag::kStdStringAssignAddr)(str, local, len);

    reinterpret_cast<ChangeHotKey_t>(kChangeHotKeyAddr)(category, command_index,
                                                       key1, key2, str);

    reinterpret_cast<StrDtor_t>(rag::kStdStringDtorAddr)(str);
    ok = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  return ok;
}

bool Save() {
  __try {
    reinterpret_cast<SaveUserKeys_t>(kSaveUserKeysAddr)();
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

}  // namespace userhotkey
