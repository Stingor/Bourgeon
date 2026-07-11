#pragma once
// tinf_inflate.h — DEFLATE (RFC 1951) + wrapper zlib (RFC 1950) compact, header-only.
// Adapté de « tinf » de Joergen Ibsen (zlib license / domaine public), réécrit en C++
// avec sortie std::vector. Suffisant pour décompresser les emblèmes de guilde (.ebm =
// BMP compressé zlib). Aucune dépendance externe.
#include <cstdint>
#include <vector>

namespace tinf {

struct Tree {
  uint16_t counts[16];   // nb de codes par longueur
  uint16_t symbols[288]; // symboles triés par longueur
};

struct Data {
  const uint8_t* src;
  const uint8_t* end;
  uint32_t tag;
  int bitcount;
  std::vector<uint8_t>* dest;
  Tree ltree, dtree;
};

// ── Tables DEFLATE (bases + bits supplémentaires) ────────────────────────────
static const uint16_t kLenBase[30] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,
                                      51,59,67,83,99,115,131,163,195,227,258,0};
static const uint8_t  kLenBits[30] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,
                                      5,5,5,5,0,0};
static const uint16_t kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,
                                       385,513,769,1025,1537,2049,3073,4097,6145,8193,
                                       12289,16385,24577};
static const uint8_t  kDistBits[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,
                                       11,11,12,12,13,13};
static const uint8_t  kClcIdx[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

inline int getbit(Data& d) {
  if (!(d.bitcount--)) {
    d.tag = (d.src < d.end) ? *d.src++ : 0;
    d.bitcount = 7;
  }
  const int bit = d.tag & 1;
  d.tag >>= 1;
  return bit;
}

inline uint32_t getbits(Data& d, int num, uint32_t base) {
  uint32_t val = 0;
  for (uint32_t mask = 1, i = 0; i < static_cast<uint32_t>(num); ++i, mask <<= 1)
    if (getbit(d)) val += mask;
  return val + base;
}

// Construit un arbre de Huffman canonique à partir des longueurs de code.
inline void build_tree(Tree& t, const uint8_t* lengths, uint32_t num) {
  uint16_t offs[16];
  for (int i = 0; i < 16; ++i) t.counts[i] = 0;
  for (uint32_t i = 0; i < num; ++i) t.counts[lengths[i]]++;
  t.counts[0] = 0;
  uint16_t sum = 0;
  for (int i = 0; i < 16; ++i) { offs[i] = sum; sum = static_cast<uint16_t>(sum + t.counts[i]); }
  for (uint32_t i = 0; i < num; ++i)
    if (lengths[i]) t.symbols[offs[lengths[i]]++] = static_cast<uint16_t>(i);
}

inline void build_fixed_trees(Tree& lt, Tree& dt) {
  for (int i = 0; i < 7; ++i) lt.counts[i] = 0;
  lt.counts[7] = 24; lt.counts[8] = 152; lt.counts[9] = 112;
  for (int i = 0; i < 24; ++i)  lt.symbols[i]             = static_cast<uint16_t>(256 + i);
  for (int i = 0; i < 144; ++i) lt.symbols[24 + i]        = static_cast<uint16_t>(i);
  for (int i = 0; i < 8; ++i)   lt.symbols[24 + 144 + i]  = static_cast<uint16_t>(280 + i);
  for (int i = 0; i < 112; ++i) lt.symbols[24 + 152 + i]  = static_cast<uint16_t>(144 + i);
  for (int i = 0; i < 16; ++i) dt.counts[i] = 0;
  dt.counts[5] = 32;
  for (int i = 0; i < 32; ++i) dt.symbols[i] = static_cast<uint16_t>(i);
}

inline int decode_symbol(Data& d, Tree& t) {
  int sum = 0, cur = 0, len = 0;
  do {
    cur = 2 * cur + getbit(d);
    if (++len >= 16) return -1;  // garde-fou données corrompues
    sum += t.counts[len];
    cur -= t.counts[len];
  } while (cur >= 0);
  const int idx = sum + cur;
  if (idx < 0 || idx >= 288) return -1;
  return t.symbols[idx];
}

// Décode les arbres dynamiques (bloc BTYPE=2).
inline bool decode_trees(Data& d, Tree& lt, Tree& dt) {
  Tree code_tree;
  uint8_t lengths[288 + 32] = {0};
  const uint32_t hlit  = getbits(d, 5, 257);
  const uint32_t hdist = getbits(d, 5, 1);
  const uint32_t hclen = getbits(d, 4, 4);
  if (hlit > 288 || hdist > 32 || hclen > 19) return false;
  for (uint32_t i = 0; i < 19; ++i) lengths[i] = 0;
  for (uint32_t i = 0; i < hclen; ++i) lengths[kClcIdx[i]] = static_cast<uint8_t>(getbits(d, 3, 0));
  build_tree(code_tree, lengths, 19);
  uint32_t num = 0;
  while (num < hlit + hdist) {
    const int sym = decode_symbol(d, code_tree);
    if (sym < 0) return false;
    if (sym == 16) {           // répéter la longueur précédente 3-6 fois
      const uint8_t prev = (num > 0) ? lengths[num - 1] : 0;
      for (uint32_t n = getbits(d, 2, 3); n && num < hlit + hdist; --n) lengths[num++] = prev;
    } else if (sym == 17) {    // 3-10 zéros
      for (uint32_t n = getbits(d, 3, 3); n && num < hlit + hdist; --n) lengths[num++] = 0;
    } else if (sym == 18) {    // 11-138 zéros
      for (uint32_t n = getbits(d, 7, 11); n && num < hlit + hdist; --n) lengths[num++] = 0;
    } else {
      lengths[num++] = static_cast<uint8_t>(sym);
    }
  }
  build_tree(lt, lengths, hlit);
  build_tree(dt, lengths + hlit, hdist);
  return true;
}

// Décompresse un bloc Huffman (BTYPE=1 ou 2).
inline bool inflate_block_data(Data& d, Tree& lt, Tree& dt) {
  for (;;) {
    int sym = decode_symbol(d, lt);
    if (sym < 0) return false;
    if (sym == 256) return true;  // fin de bloc
    if (sym < 256) {
      d.dest->push_back(static_cast<uint8_t>(sym));
    } else {
      sym -= 257;
      if (sym >= 29) return false;
      const uint32_t length = getbits(d, kLenBits[sym], kLenBase[sym]);
      const int dsym = decode_symbol(d, dt);
      if (dsym < 0 || dsym >= 30) return false;
      const uint32_t dist = getbits(d, kDistBits[dsym], kDistBase[dsym]);
      const size_t sz = d.dest->size();
      if (dist == 0 || dist > sz) return false;
      const size_t start = sz - dist;
      for (uint32_t i = 0; i < length; ++i)
        d.dest->push_back((*d.dest)[start + i]);  // ré-indexe (gère le realloc + l'overlap LZ77)
    }
  }
}

// Bloc non compressé (BTYPE=0) : aligné octet.
inline bool inflate_uncompressed_block(Data& d) {
  d.bitcount = 0;  // rejette les bits partiels -> repart sur une frontière d'octet
  if (d.src + 4 > d.end) return false;
  const uint32_t len = static_cast<uint32_t>(d.src[0]) | (static_cast<uint32_t>(d.src[1]) << 8);
  const uint32_t inv = static_cast<uint32_t>(d.src[2]) | (static_cast<uint32_t>(d.src[3]) << 8);
  if (len != ((~inv) & 0xffff)) return false;
  d.src += 4;
  if (d.src + len > d.end) return false;
  for (uint32_t i = 0; i < len; ++i) d.dest->push_back(*d.src++);
  return true;
}

// Inflate un flux DEFLATE brut vers `out`.
inline bool inflate_raw(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out) {
  Data d;
  d.src = src; d.end = src + srcLen; d.tag = 0; d.bitcount = 0; d.dest = &out;
  int bfinal;
  do {
    bfinal = getbit(d);
    const int btype = static_cast<int>(getbits(d, 2, 0));
    bool ok;
    if (btype == 0)      ok = inflate_uncompressed_block(d);
    else if (btype == 1) { build_fixed_trees(d.ltree, d.dtree); ok = inflate_block_data(d, d.ltree, d.dtree); }
    else if (btype == 2) ok = decode_trees(d, d.ltree, d.dtree) && inflate_block_data(d, d.ltree, d.dtree);
    else                 return false;
    if (!ok) return false;
    if (out.size() > (16u << 20)) return false;  // garde-fou anti-bombe
  } while (!bfinal);
  return true;
}

// Décompresse un flux ZLIB (RFC 1950 : en-tête 2 octets + DEFLATE + adler32 ignoré).
inline bool zlib_uncompress(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out) {
  out.clear();
  if (srcLen < 2) return false;
  size_t off = 0;
  // En-tête zlib : CM=8 (deflate) dans les 4 bits bas de CMF, checksum (CMF*256+FLG)%31==0.
  if ((src[0] & 0x0f) == 8 &&
      ((static_cast<uint32_t>(src[0]) << 8 | src[1]) % 31) == 0) {
    off = 2;
    if (src[1] & 0x20) off += 4;  // FDICT : sauter le DICTID
  }
  if (off >= srcLen) return false;
  return inflate_raw(src + off, srcLen - off, out);
}

}  // namespace tinf
