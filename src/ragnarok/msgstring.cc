#include "ragnarok/msgstring.h"

#include <windows.h>

#include <cctype>
#include <cstring>

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
  // ⚠ LocalToUtf8, pas Cp949ToUtf8 : la MsgStringTable est du texte du CLIENT,
  // donc dans SA code-page — 949 en Corée, 1252 en Europe. La décoder autrement
  // que lui ferait dire à nos fenêtres autre chose qu'aux siennes.
  // LocalToUtf8 accepte la chaîne vide et rend « » : pas de cas particulier.
  return ro::LocalToUtf8(Cp949(id));
}

const char* Utf8Or(int id, const char* fallback) {
  const char* raw = Cp949(id);
  // Les deux sentinelles du client, plus la table pas encore chargée. Le test
  // porte sur le PRÉFIXE : la seconde forme finit par l'id (« NO MSG : 4240 »).
  if (!raw || !*raw || std::strncmp(raw, "NO MSG", 6) == 0)
    return fallback ? fallback : "";
  return ro::LocalToUtf8(raw);
}

const char* Flatten(const char* src) {
  static thread_local char buf[128];
  int n = 0;
  for (const char* p = src; p && *p && n < static_cast<int>(sizeof(buf)) - 1; ++p)
    buf[n++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
  while (n > 0 && buf[n - 1] == ' ') --n;  // pas d'espace traînant avant le « ## »
  buf[n] = '\0';
  return buf;
}

const char* StripColors(const char* src) {
  static thread_local char bufs[4][512];
  static thread_local int slot = 0;
  slot = (slot + 1) & 3;
  char* out = bufs[slot];
  const int cap = static_cast<int>(sizeof(bufs[0])) - 1;
  int o = 0;
  for (const char* p = src; p && *p && o < cap;) {
    if (*p == '^') {
      bool hex6 = true;
      for (int k = 1; k <= 6; ++k) {
        const char c = p[k];
        if (!c || !std::isxdigit(static_cast<unsigned char>(c))) { hex6 = false; break; }
      }
      if (hex6) { p += 7; continue; }
    }
    out[o++] = *p++;
  }
  out[o] = '\0';
  return out;
}

}  // namespace msgstr
