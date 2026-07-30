#include "ragnarok/msgstring.h"

#include <windows.h>

#include "ui/ro_imgui.h"  // ro::Cp949ToUtf8

namespace msgstr {
namespace {

using MsgStringGet_t = const char*(__cdecl*)(int);

}  // namespace

const char* Cp949(int id) {
  __try {
    const char* s = reinterpret_cast<MsgStringGet_t>(kGetAddr)(id);
    return s ? s : "";
  } __except (EXCEPTION_EXECUTE_HANDLER) { return ""; }
}

const char* Utf8(int id) {
  // Cp949ToUtf8 accepte la chaîne vide et rend « » : pas de cas particulier.
  return ro::Cp949ToUtf8(Cp949(id));
}

}  // namespace msgstr
