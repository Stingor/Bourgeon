#include "utils/gif_writer.h"

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
