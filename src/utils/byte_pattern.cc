#include "utils/byte_pattern.h"

BytePattern::BytePattern(const std::vector<uint8_t> &pattern,
                         const std::string &mask) {
  pattern_ = pattern;
  mask_ = mask;
}

void *BytePattern::Search(void *address, size_t len) {
  uint8_t *haystack = reinterpret_cast<uint8_t *>(address);
  const size_t nlen = mask_.length();
  if (nlen == 0 || len < nlen) return nullptr;
  const size_t last = nlen - 1;

  // Wildcard-aware Boyer-Moore-Horspool bad-character table.
  //
  // The skip on a mismatch is driven by the haystack byte aligned with the last
  // pattern position. Only the LITERAL bytes that come AFTER the last wildcard
  // ('?') in the mask can produce a safe skip: any position at or before a
  // wildcard could match an arbitrary byte, so we must never skip past it (doing
  // so would jump over a valid match — the original bug here keyed the table on
  // the raw pattern bytes, treating wildcard 0x00 placeholders as literals).
  size_t last_wild = nlen;  // nlen == "no wildcard"
  for (size_t i = 0; i < nlen; ++i)
    if (mask_[i] == '?') last_wild = i;

  size_t default_skip = (last_wild == nlen) ? nlen : (last - last_wild);
  if (default_skip == 0) default_skip = 1;  // last position itself is a wildcard

  size_t bad_char_skip[256];
  for (size_t i = 0; i < 256; ++i) bad_char_skip[i] = default_skip;

  const size_t start = (last_wild == nlen) ? 0 : (last_wild + 1);
  for (size_t scan = start; scan < last; ++scan)
    bad_char_skip[pattern_[scan]] = last - scan;

  while (len >= nlen) {
    size_t scan = last;
    while (mask_[scan] == '?' || haystack[scan] == pattern_[scan]) {
      if (scan == 0) return haystack;
      --scan;
    }
    const size_t skip = bad_char_skip[haystack[last]];
    len -= skip;
    haystack += skip;
  }

  return nullptr;
}

bool BytePattern::Match(void *address, size_t offset) { return false; }

size_t BytePattern::GetSize() { return pattern_.size(); }
