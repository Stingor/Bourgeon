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
  for (const char* p = haystack; *p; ++p) {
    size_t i = 0;
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
