#include "utils/gif_writer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

// Little-endian helpers.
inline void PutU16(std::vector<uint8_t>& b, unsigned v) {
  b.push_back(static_cast<uint8_t>(v & 0xFF));
  b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

// Bit-packer that writes LZW codes LSB-first and splits the output into GIF image
// sub-blocks (<=255 data bytes, each length-prefixed), then a 0-length terminator.
struct BitBlockWriter {
  std::vector<uint8_t>& out;
  uint8_t block[255];
  int     blockLen = 0;
  unsigned bitBuf = 0;
  int      bitCount = 0;
  explicit BitBlockWriter(std::vector<uint8_t>& o) : out(o) {}

  void pushByte(uint8_t v) {
    block[blockLen++] = v;
    if (blockLen == 255) {
      out.push_back(255);
      out.insert(out.end(), block, block + 255);
      blockLen = 0;
    }
  }
  void writeBits(unsigned code, int nbits) {
    bitBuf |= code << bitCount;
    bitCount += nbits;
    while (bitCount >= 8) {
      pushByte(static_cast<uint8_t>(bitBuf & 0xFF));
      bitBuf >>= 8;
      bitCount -= 8;
    }
  }
  void finish() {  // flush remaining bits + partial block + terminator
    if (bitCount > 0) {
      pushByte(static_cast<uint8_t>(bitBuf & 0xFF));
      bitBuf = 0;
      bitCount = 0;
    }
    if (blockLen > 0) {
      out.push_back(static_cast<uint8_t>(blockLen));
      out.insert(out.end(), block, block + blockLen);
      blockLen = 0;
    }
    out.push_back(0);  // block terminator
  }
};

// Emits the palette-index stream as standard (compressed) GIF LZW. Dictionary keyed
// by (prefix<<8 | char); the code width grows when the just-assigned code reaches
// 2^codeSize (validated to stay in sync with GIF decoders, incl. the KwKwK case),
// and the dictionary is cleared at 4095 (before the 12-bit ceiling). Codes are packed
// LSB-first into image sub-blocks by the BitBlockWriter.
void EmitIndicesLZW(BitBlockWriter& w, const uint8_t* idx, int count, int minCodeSize) {
  const unsigned clearCode = 1u << minCodeSize;
  const unsigned eoiCode   = clearCode + 1;
  int codeSize  = minCodeSize + 1;
  int nextCode  = static_cast<int>(eoiCode) + 1;
  std::unordered_map<int, int> dict;
  dict.reserve(5000);

  w.writeBits(clearCode, codeSize);
  if (count <= 0) { w.writeBits(eoiCode, codeSize); w.finish(); return; }

  int cur = idx[0];
  for (int i = 1; i < count; ++i) {
    const int c = idx[i];
    const int key = (cur << 8) | c;
    const auto it = dict.find(key);
    if (it != dict.end()) {
      cur = it->second;  // extend the current match
    } else {
      w.writeBits(static_cast<unsigned>(cur), codeSize);
      dict.emplace(key, nextCode);
      if (nextCode == (1 << codeSize) && codeSize < 12) ++codeSize;
      if (nextCode == 4095) {  // dictionary full -> clear + restart
        w.writeBits(clearCode, codeSize);
        dict.clear();
        codeSize = minCodeSize + 1;
        nextCode = static_cast<int>(eoiCode) + 1;
      } else {
        ++nextCode;
      }
      cur = c;
    }
  }
  w.writeBits(static_cast<unsigned>(cur), codeSize);
  w.writeBits(eoiCode, codeSize);
  w.finish();
}

// ── Median-cut quantiser (screen captures) ───────────────────────────────────
// Colours are bucketed into a 15-bit (RGB555) histogram first: 32768 cells is
// small enough to walk repeatedly, and fine enough that the 5-bit rounding is
// invisible once the result is squeezed into 255 colours anyway.

// One histogram cell that at least one pixel landed in.
struct Bin {
  uint16_t key;    // RGB555
  uint32_t count;  // pixels across the WHOLE clip
};

inline int BinR(uint16_t key) { return (key >> 10) & 31; }
inline int BinG(uint16_t key) { return (key >> 5) & 31; }
inline int BinB(uint16_t key) { return key & 31; }

// 5-bit channel -> 8-bit, replicating the high bits into the low ones so the
// range stays full (31 -> 255, not 248: without this every colour darkens).
inline int Bin5To8(int c) { return (c << 3) | (c >> 2); }

// A contiguous range of bins, plus the extent of the colours it covers.
struct Box {
  int      begin, end;  // half-open range into the bin array
  uint64_t count;       // pixels represented
  int      rmin, rmax, gmin, gmax, bmin, bmax;  // 5-bit units
};

void ShrinkBox(Box& box, const std::vector<Bin>& bins) {
  box.rmin = box.gmin = box.bmin = 31;
  box.rmax = box.gmax = box.bmax = 0;
  box.count = 0;
  for (int i = box.begin; i < box.end; ++i) {
    const int r = BinR(bins[i].key), g = BinG(bins[i].key), b = BinB(bins[i].key);
    if (r < box.rmin) box.rmin = r;
    if (r > box.rmax) box.rmax = r;
    if (g < box.gmin) box.gmin = g;
    if (g > box.gmax) box.gmax = g;
    if (b < box.bmin) box.bmin = b;
    if (b > box.bmax) box.bmax = b;
    box.count += bins[i].count;
  }
}

// Heckbert's median cut: split the box whose colours span the widest range, at
// the median of its PIXEL POPULATION (not of its bin count) — so a colour that
// covers half the screen earns its own entries, while a handful of stray pixels
// don't. Stops early when nothing is splittable: a clip with 40 distinct colours
// gets 40 palette entries and no empty ones.
void MedianCut(std::vector<Bin>& bins, std::vector<Box>& boxes, int want) {
  boxes.clear();
  Box first{0, static_cast<int>(bins.size()), 0, 0, 0, 0, 0, 0, 0};
  ShrinkBox(first, bins);
  boxes.push_back(first);

  while (static_cast<int>(boxes.size()) < want) {
    int best = -1, best_extent = 0;
    uint64_t best_count = 0;
    for (size_t i = 0; i < boxes.size(); ++i) {
      const Box& box = boxes[i];
      if (box.end - box.begin < 2) continue;  // one bin: nothing left to cut
      const int extent = (std::max)((std::max)(box.rmax - box.rmin, box.gmax - box.gmin),
                                    box.bmax - box.bmin);
      if (extent <= 0) continue;              // one single colour
      if (extent > best_extent || (extent == best_extent && box.count > best_count)) {
        best = static_cast<int>(i);
        best_extent = extent;
        best_count = box.count;
      }
    }
    if (best < 0) break;

    Box& box = boxes[best];
    const int rspan = box.rmax - box.rmin, gspan = box.gmax - box.gmin,
              bspan = box.bmax - box.bmin;
    const int channel = (rspan >= gspan && rspan >= bspan) ? 0 : (gspan >= bspan ? 1 : 2);
    std::sort(bins.begin() + box.begin, bins.begin() + box.end,
              [channel](const Bin& a, const Bin& b) {
                const int va = channel == 0 ? BinR(a.key) : channel == 1 ? BinG(a.key) : BinB(a.key);
                const int vb = channel == 0 ? BinR(b.key) : channel == 1 ? BinG(b.key) : BinB(b.key);
                return va < vb;
              });

    const uint64_t half = box.count / 2;
    uint64_t acc = 0;
    int split = box.begin;
    for (int i = box.begin; i < box.end - 1; ++i) {
      acc += bins[i].count;
      split = i + 1;
      if (acc >= half) break;
    }
    // Both halves must be non-empty, or the loop would spin on the same box.
    if (split <= box.begin) split = box.begin + 1;
    if (split >= box.end)   split = box.end - 1;

    Box lo{box.begin, split, 0, 0, 0, 0, 0, 0, 0};
    Box hi{split, box.end, 0, 0, 0, 0, 0, 0, 0};
    ShrinkBox(lo, bins);
    ShrinkBox(hi, bins);
    boxes[best] = lo;
    boxes.push_back(hi);
  }
}

}  // namespace

bool GifWrite(const char* path, const uint32_t* const* frames, int w, int h,
              int nframes, int delay_cs) {
  if (!path || !frames || w <= 0 || h <= 0 || nframes <= 0) return false;
  const int npix = w * h;

  std::vector<uint8_t> file;
  file.reserve(static_cast<size_t>(npix) * nframes / 2 + 1024);

  // Header + Logical Screen Descriptor (no global color table).
  const char hdr[6] = {'G', 'I', 'F', '8', '9', 'a'};
  file.insert(file.end(), hdr, hdr + 6);
  PutU16(file, static_cast<unsigned>(w));
  PutU16(file, static_cast<unsigned>(h));
  file.push_back(0x00);  // packed: no global color table
  file.push_back(0x00);  // background color index
  file.push_back(0x00);  // pixel aspect ratio

  // NETSCAPE2.0 application extension -> loop forever.
  const uint8_t loopExt[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P',
                             'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00};
  file.insert(file.end(), loopExt, loopExt + sizeof(loopExt));

  std::vector<uint8_t> indices(static_cast<size_t>(npix));
  std::vector<uint8_t> localTable;  // r,g,b * entries

  for (int f = 0; f < nframes; ++f) {
    const uint32_t* px = frames[f];
    if (!px) return false;

    // Quantize this frame's OPAQUE colours to <=255 entries by reducing colour bits
    // until they fit (RO sprites are palette-based, so 7-8 bits usually suffices).
    // Index 0 is reserved as the transparent colour.
    std::unordered_map<uint32_t, int> cmap;  // packed masked RGB -> palette index (1..)
    int bits = 8;
    for (;; --bits) {
      cmap.clear();
      const uint32_t mask = (bits >= 8) ? 0xFFu
                                        : static_cast<uint32_t>(0xFF & (0xFF << (8 - bits)));
      bool overflow = false;
      for (int i = 0; i < npix; ++i) {
        const uint32_t p = px[i];
        if ((p >> 24) < 128u) continue;  // transparent
        const uint32_t q = ((p >> 16) & mask) << 16 | ((p >> 8) & mask) << 8 | (p & mask);
        if (cmap.find(q) == cmap.end()) {
          if (static_cast<int>(cmap.size()) >= 255) { overflow = true; break; }
          cmap.emplace(q, static_cast<int>(cmap.size()) + 1);  // +1: index 0 = transparent
        }
      }
      if (!overflow || bits <= 1) break;
    }
    const uint32_t mask = (bits >= 8) ? 0xFFu
                                      : static_cast<uint32_t>(0xFF & (0xFF << (8 - bits)));

    // Build the local colour table (index 0 = transparent placeholder) and indices.
    const int numColors = static_cast<int>(cmap.size()) + 1;  // incl. transparent
    int tableBits = 2;                                        // min 4 entries / codeSize 2
    while ((1 << tableBits) < numColors) ++tableBits;         // <= 8 (numColors<=256)
    const int entries = 1 << tableBits;

    localTable.assign(static_cast<size_t>(entries) * 3, 0);
    localTable[0] = localTable[1] = localTable[2] = 0;  // transparent slot (any RGB)
    for (const auto& kv : cmap) {
      const uint32_t q = kv.first;
      const int idx = kv.second;
      // Replicate the kept high bits into the low bits so reduced colours don't darken.
      uint32_t r = (q >> 16) & 0xFF, g = (q >> 8) & 0xFF, b = q & 0xFF;
      if (bits < 8) { r |= r >> bits; g |= g >> bits; b |= b >> bits; }
      localTable[static_cast<size_t>(idx) * 3 + 0] = static_cast<uint8_t>(r);
      localTable[static_cast<size_t>(idx) * 3 + 1] = static_cast<uint8_t>(g);
      localTable[static_cast<size_t>(idx) * 3 + 2] = static_cast<uint8_t>(b);
    }
    for (int i = 0; i < npix; ++i) {
      const uint32_t p = px[i];
      if ((p >> 24) < 128u) {
        indices[static_cast<size_t>(i)] = 0;  // transparent
      } else {
        const uint32_t q = ((p >> 16) & mask) << 16 | ((p >> 8) & mask) << 8 | (p & mask);
        indices[static_cast<size_t>(i)] = static_cast<uint8_t>(cmap[q]);
      }
    }

    // Graphic Control Extension: disposal=2 (restore bg so transparency doesn't
    // accumulate), transparent-colour flag, delay, transparent index 0.
    file.push_back(0x21);
    file.push_back(0xF9);
    file.push_back(0x04);
    file.push_back(static_cast<uint8_t>((2 << 2) | 0x01));  // disposal 2 + transparent
    PutU16(file, static_cast<unsigned>(delay_cs < 0 ? 0 : delay_cs));
    file.push_back(0x00);  // transparent colour index
    file.push_back(0x00);  // block terminator

    // Image Descriptor + local colour table (flag set, size = tableBits-1).
    file.push_back(0x2C);
    PutU16(file, 0);  // left
    PutU16(file, 0);  // top
    PutU16(file, static_cast<unsigned>(w));
    PutU16(file, static_cast<unsigned>(h));
    file.push_back(static_cast<uint8_t>(0x80 | (tableBits - 1)));  // local table, size
    file.insert(file.end(), localTable.begin(), localTable.end());

    // Image data: min-code-size byte, then LZW sub-blocks.
    const int minCodeSize = tableBits;  // >= 2
    file.push_back(static_cast<uint8_t>(minCodeSize));
    BitBlockWriter bw(file);
    EmitIndicesLZW(bw, indices.data(), npix, minCodeSize);
  }

  file.push_back(0x3B);  // trailer

  FILE* fp = nullptr;
  if (fopen_s(&fp, path, "wb") != 0 || !fp) return false;
  const size_t wrote = std::fwrite(file.data(), 1, file.size(), fp);
  std::fclose(fp);
  return wrote == file.size();
}

bool GifWriteScreen(const char* path, const uint32_t* const* frames, int w, int h,
                    int nframes, int delay_cs) {
  if (!path || !frames || w <= 0 || h <= 0 || nframes <= 0) return false;
  const int npix = w * h;

  // ── 1. One histogram for the whole clip ────────────────────────────────────
  std::vector<uint32_t> hist(32768, 0);
  for (int f = 0; f < nframes; ++f) {
    const uint32_t* px = frames[f];
    if (!px) return false;
    for (int i = 0; i < npix; ++i) {
      const uint32_t p = px[i];
      const unsigned key = (((p >> 19) & 0x1Fu) << 10) | (((p >> 11) & 0x1Fu) << 5) |
                           ((p >> 3) & 0x1Fu);
      ++hist[key];
    }
  }

  std::vector<Bin> bins;
  bins.reserve(8192);
  for (int k = 0; k < 32768; ++k)
    if (hist[k]) bins.push_back({static_cast<uint16_t>(k), hist[k]});
  if (bins.empty()) return false;
  hist.clear();
  hist.shrink_to_fit();

  // ── 2. Global palette: index 0 is the transparent slot the differencing uses,
  // so 255 colours are left for the picture.
  std::vector<Box> boxes;
  MedianCut(bins, boxes, 255);

  uint8_t gct[256 * 3] = {0};
  std::vector<uint8_t> lut(32768, 0);
  for (size_t bi = 0; bi < boxes.size(); ++bi) {
    const Box& box = boxes[bi];
    const uint8_t index = static_cast<uint8_t>(bi + 1);
    uint64_t rs = 0, gs = 0, bs = 0, n = 0;
    for (int i = box.begin; i < box.end; ++i) {
      const uint64_t c = bins[i].count;
      rs += static_cast<uint64_t>(Bin5To8(BinR(bins[i].key))) * c;
      gs += static_cast<uint64_t>(Bin5To8(BinG(bins[i].key))) * c;
      bs += static_cast<uint64_t>(Bin5To8(BinB(bins[i].key))) * c;
      n  += c;
      // Every bin belongs to exactly one box, so the pixel->palette mapping is a
      // direct lookup — no nearest-colour search anywhere in the hot loop.
      lut[bins[i].key] = index;
    }
    if (n == 0) n = 1;
    gct[index * 3 + 0] = static_cast<uint8_t>(rs / n);
    gct[index * 3 + 1] = static_cast<uint8_t>(gs / n);
    gct[index * 3 + 2] = static_cast<uint8_t>(bs / n);
  }
  bins.clear();
  bins.shrink_to_fit();

  FILE* fp = nullptr;
  if (fopen_s(&fp, path, "wb") != 0 || !fp) return false;

  // ── 3. Header, global colour table, loop extension ─────────────────────────
  std::vector<uint8_t> chunk;
  chunk.reserve(static_cast<size_t>(npix) / 2 + 2048);
  const char hdr[6] = {'G', 'I', 'F', '8', '9', 'a'};
  chunk.insert(chunk.end(), hdr, hdr + 6);
  PutU16(chunk, static_cast<unsigned>(w));
  PutU16(chunk, static_cast<unsigned>(h));
  chunk.push_back(0xF7);  // global table present, 8-bit resolution, 256 entries
  chunk.push_back(0x00);  // background colour index
  chunk.push_back(0x00);  // pixel aspect ratio
  chunk.insert(chunk.end(), gct, gct + sizeof(gct));
  const uint8_t loopExt[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P',
                             'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00};
  chunk.insert(chunk.end(), loopExt, loopExt + sizeof(loopExt));

  bool ok = std::fwrite(chunk.data(), 1, chunk.size(), fp) == chunk.size();

  // ── 4. Frames ──────────────────────────────────────────────────────────────
  std::vector<uint8_t> cur(static_cast<size_t>(npix)), prev(static_cast<size_t>(npix));
  std::vector<uint8_t> sub;
  for (int f = 0; f < nframes && ok; ++f) {
    const uint32_t* px = frames[f];
    for (int i = 0; i < npix; ++i) {
      const uint32_t p = px[i];
      const unsigned key = (((p >> 19) & 0x1Fu) << 10) | (((p >> 11) & 0x1Fu) << 5) |
                           ((p >> 3) & 0x1Fu);
      cur[static_cast<size_t>(i)] = lut[key];
    }

    // Bounding box of what moved. Frame 0 is the full picture (there is nothing
    // underneath it yet); afterwards a still screen collapses to a 1x1 rectangle
    // that costs a dozen bytes and still burns its delay.
    int x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (f > 0) {
      x0 = w; y0 = h; x1 = -1; y1 = -1;
      for (int y = 0; y < h; ++y) {
        const uint8_t* c = &cur[static_cast<size_t>(y) * w];
        const uint8_t* p = &prev[static_cast<size_t>(y) * w];
        for (int x = 0; x < w; ++x) {
          if (c[x] == p[x]) continue;
          if (x < x0) x0 = x;
          if (x > x1) x1 = x;
          if (y < y0) y0 = y;
          if (y > y1) y1 = y;
        }
      }
      if (x1 < 0) { x0 = 0; y0 = 0; x1 = 0; y1 = 0; }  // nothing changed at all
      ++x1;
      ++y1;
    }
    const int rw = x1 - x0, rh = y1 - y0;

    sub.resize(static_cast<size_t>(rw) * rh);
    for (int y = 0; y < rh; ++y) {
      const size_t row = static_cast<size_t>(y0 + y) * w + x0;
      for (int x = 0; x < rw; ++x) {
        const uint8_t index = cur[row + x];
        // Unchanged -> transparent, which under disposal 1 shows the pixel that
        // is already on the canvas.
        sub[static_cast<size_t>(y) * rw + x] =
            (f > 0 && index == prev[row + x]) ? 0 : index;
      }
    }

    chunk.clear();
    chunk.push_back(0x21);  // Graphic Control Extension
    chunk.push_back(0xF9);
    chunk.push_back(0x04);
    chunk.push_back((1 << 2) | 0x01);  // disposal 1 (leave in place) + transparency
    PutU16(chunk, static_cast<unsigned>(delay_cs < 0 ? 0 : delay_cs));
    chunk.push_back(0x00);  // transparent colour index
    chunk.push_back(0x00);  // block terminator
    chunk.push_back(0x2C);  // Image Descriptor
    PutU16(chunk, static_cast<unsigned>(x0));
    PutU16(chunk, static_cast<unsigned>(y0));
    PutU16(chunk, static_cast<unsigned>(rw));
    PutU16(chunk, static_cast<unsigned>(rh));
    chunk.push_back(0x00);  // no local table: the global one covers every frame
    chunk.push_back(0x08);  // min code size (256-entry table)
    BitBlockWriter bw(chunk);
    EmitIndicesLZW(bw, sub.data(), rw * rh, 8);

    ok = std::fwrite(chunk.data(), 1, chunk.size(), fp) == chunk.size();
    prev.swap(cur);
  }

  if (ok) {
    const uint8_t trailer = 0x3B;
    ok = std::fwrite(&trailer, 1, 1, fp) == 1;
  }
  std::fclose(fp);
  return ok;
}
