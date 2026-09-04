#include "utils/text.h"

#include <cstdio>
#include <cstring>

namespace text {
namespace {

inline char LowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

std::string ToLowerAscii(std::string s) {
  for (char& c : s) c = LowerAscii(c);
  return s;
}

bool ContainsNoCase(const char* haystack, const char* needle) {
  if (!needle || !*needle) return true;
  if (!haystack || !*haystack) return false;
  const size_t n = std::strlen(needle);
  // 🔴 Le premier caractère de l'aiguille, minusculé UNE SEULE FOIS, sert de
  // crible. Sans lui, CHAQUE position du texte entrait dans la boucle interne,
  // qui reminusculait l'aiguille caractère par caractère — deux appels par
  // comparaison, refaits autant de fois qu'il y a de positions.
  //
  // Ce n'est pas une coquetterie : la recherche du chat appelle cette fonction
  // sur le texte ET l'expéditeur de chaque ligne du tampon (cf.
  // ChatWindow::LineMatchesSearch, qui en mémorise désormais le résultat).
  const char head = LowerAscii(needle[0]);
  for (const char* p = haystack; *p; ++p) {
    if (LowerAscii(*p) != head) continue;
    // Le premier caractère est acquis : la comparaison reprend au SECOND. Une
    // aiguille d'un seul caractère sort donc d'ici avec `i == n`, soit trouvée.
    size_t i = 1;
    while (i < n && p[i] && LowerAscii(p[i]) == LowerAscii(needle[i])) ++i;
    if (i == n) return true;
  }
  return false;
}

int Base62Digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 36;
  return -1;
}

uint32_t Base62Decode(const char* s, size_t len) {
  uint32_t v = 0;
  for (size_t i = 0; i < len; ++i) {
    const int d = Base62Digit(s[i]);
    if (d < 0) break;
    v = v * 62u + static_cast<uint32_t>(d);
  }
  return v;
}

bool IsHex6(const char* s) {
  for (int i = 0; i < 6; ++i) {
    const char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F')))
      return false;
  }
  return true;
}

const char* SearchSub(const char* begin, const char* end, const char* needle) {
  const size_t n = std::strlen(needle);
  if (n == 0 || static_cast<size_t>(end - begin) < n) return nullptr;
  for (const char* p = begin; p + n <= end; ++p)
    if (std::memcmp(p, needle, n) == 0) return p;
  return nullptr;
}

void GroupThousands(long long value, char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  char raw[32];
  std::snprintf(raw, sizeof(raw), "%lld", value);
  const int len = static_cast<int>(std::strlen(raw));
  // Le premier groupe est le RESTE de la division par trois — « 1,234,567 »
  // commence par un chiffre seul, pas par trois.
  int lead = (len % 3) ? (len % 3) : 3;
  size_t o = 0;
  for (int i = 0; i < len && o + 2 < cap; ++i) {
    if (i == lead && i != 0) { out[o++] = ','; lead += 3; }
    out[o++] = raw[i];
  }
  out[o] = '\0';
}

}  // namespace text
