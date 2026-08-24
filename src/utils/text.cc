#include "utils/text.h"

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

}  // namespace text
