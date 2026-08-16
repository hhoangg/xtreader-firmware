#include "PartialContentHash.h"

#include <cstring>

namespace FsHelpers {

namespace {

// Portable MD5 implementation (RFC 1321), independent of any platform SDK so it can run both
// on the host (unit tests) and on the firmware target. This is the classic public-domain
// structure (as used by, e.g., Cyrus SASL / Samba): a 64-byte block buffer, 64-bit bit counter,
// and the standard four-round transform driven by the RFC 1321 Appendix A.3 constant table.

struct Md5Context {
  uint32_t buf[4];
  uint32_t bits[2];
  uint8_t in[64];
};

void md5Init(Md5Context& ctx) {
  ctx.buf[0] = 0x67452301;
  ctx.buf[1] = 0xefcdab89;
  ctx.buf[2] = 0x98badcfe;
  ctx.buf[3] = 0x10325476;
  ctx.bits[0] = 0;
  ctx.bits[1] = 0;
}

uint32_t rotateLeft(const uint32_t x, const int s) { return (x << s) | (x >> (32 - s)); }

void md5Transform(uint32_t buf[4], const uint8_t block[64]) {
  uint32_t in[16];
  for (int i = 0; i < 16; i++) {
    in[i] = static_cast<uint32_t>(block[i * 4]) | (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
            (static_cast<uint32_t>(block[i * 4 + 2]) << 16) | (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
  }

  uint32_t a = buf[0];
  uint32_t b = buf[1];
  uint32_t c = buf[2];
  uint32_t d = buf[3];

  auto f1 = [](uint32_t x, uint32_t y, uint32_t z) { return z ^ (x & (y ^ z)); };
  auto f2 = [&](uint32_t x, uint32_t y, uint32_t z) { return f1(z, x, y); };
  auto f3 = [](uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; };
  auto f4 = [](uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); };

  auto step = [](uint32_t& w, const uint32_t f, const uint32_t x, const uint32_t data, const int s) {
    w = rotateLeft(w + f + data, s) + x;
  };

  step(a, f1(b, c, d), b, in[0] + 0xd76aa478, 7);
  step(d, f1(a, b, c), a, in[1] + 0xe8c7b756, 12);
  step(c, f1(d, a, b), d, in[2] + 0x242070db, 17);
  step(b, f1(c, d, a), c, in[3] + 0xc1bdceee, 22);
  step(a, f1(b, c, d), b, in[4] + 0xf57c0faf, 7);
  step(d, f1(a, b, c), a, in[5] + 0x4787c62a, 12);
  step(c, f1(d, a, b), d, in[6] + 0xa8304613, 17);
  step(b, f1(c, d, a), c, in[7] + 0xfd469501, 22);
  step(a, f1(b, c, d), b, in[8] + 0x698098d8, 7);
  step(d, f1(a, b, c), a, in[9] + 0x8b44f7af, 12);
  step(c, f1(d, a, b), d, in[10] + 0xffff5bb1, 17);
  step(b, f1(c, d, a), c, in[11] + 0x895cd7be, 22);
  step(a, f1(b, c, d), b, in[12] + 0x6b901122, 7);
  step(d, f1(a, b, c), a, in[13] + 0xfd987193, 12);
  step(c, f1(d, a, b), d, in[14] + 0xa679438e, 17);
  step(b, f1(c, d, a), c, in[15] + 0x49b40821, 22);

  step(a, f2(b, c, d), b, in[1] + 0xf61e2562, 5);
  step(d, f2(a, b, c), a, in[6] + 0xc040b340, 9);
  step(c, f2(d, a, b), d, in[11] + 0x265e5a51, 14);
  step(b, f2(c, d, a), c, in[0] + 0xe9b6c7aa, 20);
  step(a, f2(b, c, d), b, in[5] + 0xd62f105d, 5);
  step(d, f2(a, b, c), a, in[10] + 0x02441453, 9);
  step(c, f2(d, a, b), d, in[15] + 0xd8a1e681, 14);
  step(b, f2(c, d, a), c, in[4] + 0xe7d3fbc8, 20);
  step(a, f2(b, c, d), b, in[9] + 0x21e1cde6, 5);
  step(d, f2(a, b, c), a, in[14] + 0xc33707d6, 9);
  step(c, f2(d, a, b), d, in[3] + 0xf4d50d87, 14);
  step(b, f2(c, d, a), c, in[8] + 0x455a14ed, 20);
  step(a, f2(b, c, d), b, in[13] + 0xa9e3e905, 5);
  step(d, f2(a, b, c), a, in[2] + 0xfcefa3f8, 9);
  step(c, f2(d, a, b), d, in[7] + 0x676f02d9, 14);
  step(b, f2(c, d, a), c, in[12] + 0x8d2a4c8a, 20);

  step(a, f3(b, c, d), b, in[5] + 0xfffa3942, 4);
  step(d, f3(a, b, c), a, in[8] + 0x8771f681, 11);
  step(c, f3(d, a, b), d, in[11] + 0x6d9d6122, 16);
  step(b, f3(c, d, a), c, in[14] + 0xfde5380c, 23);
  step(a, f3(b, c, d), b, in[1] + 0xa4beea44, 4);
  step(d, f3(a, b, c), a, in[4] + 0x4bdecfa9, 11);
  step(c, f3(d, a, b), d, in[7] + 0xf6bb4b60, 16);
  step(b, f3(c, d, a), c, in[10] + 0xbebfbc70, 23);
  step(a, f3(b, c, d), b, in[13] + 0x289b7ec6, 4);
  step(d, f3(a, b, c), a, in[0] + 0xeaa127fa, 11);
  step(c, f3(d, a, b), d, in[3] + 0xd4ef3085, 16);
  step(b, f3(c, d, a), c, in[6] + 0x04881d05, 23);
  step(a, f3(b, c, d), b, in[9] + 0xd9d4d039, 4);
  step(d, f3(a, b, c), a, in[12] + 0xe6db99e5, 11);
  step(c, f3(d, a, b), d, in[15] + 0x1fa27cf8, 16);
  step(b, f3(c, d, a), c, in[2] + 0xc4ac5665, 23);

  step(a, f4(b, c, d), b, in[0] + 0xf4292244, 6);
  step(d, f4(a, b, c), a, in[7] + 0x432aff97, 10);
  step(c, f4(d, a, b), d, in[14] + 0xab9423a7, 15);
  step(b, f4(c, d, a), c, in[5] + 0xfc93a039, 21);
  step(a, f4(b, c, d), b, in[12] + 0x655b59c3, 6);
  step(d, f4(a, b, c), a, in[3] + 0x8f0ccc92, 10);
  step(c, f4(d, a, b), d, in[10] + 0xffeff47d, 15);
  step(b, f4(c, d, a), c, in[1] + 0x85845dd1, 21);
  step(a, f4(b, c, d), b, in[8] + 0x6fa87e4f, 6);
  step(d, f4(a, b, c), a, in[15] + 0xfe2ce6e0, 10);
  step(c, f4(d, a, b), d, in[6] + 0xa3014314, 15);
  step(b, f4(c, d, a), c, in[13] + 0x4e0811a1, 21);
  step(a, f4(b, c, d), b, in[4] + 0xf7537e82, 6);
  step(d, f4(a, b, c), a, in[11] + 0xbd3af235, 10);
  step(c, f4(d, a, b), d, in[2] + 0x2ad7d2bb, 15);
  step(b, f4(c, d, a), c, in[9] + 0xeb86d391, 21);

  buf[0] += a;
  buf[1] += b;
  buf[2] += c;
  buf[3] += d;
}

void md5Update(Md5Context& ctx, const uint8_t* buf, size_t len) {
  const uint32_t priorBits = ctx.bits[0];
  ctx.bits[0] = priorBits + (static_cast<uint32_t>(len) << 3);
  if (ctx.bits[0] < priorBits) {
    ctx.bits[1]++;
  }
  ctx.bits[1] += static_cast<uint32_t>(len >> 29);

  size_t used = (priorBits >> 3) & 0x3f;

  if (used) {
    uint8_t* p = ctx.in + used;
    const size_t available = 64 - used;
    if (len < available) {
      memcpy(p, buf, len);
      return;
    }
    memcpy(p, buf, available);
    md5Transform(ctx.buf, ctx.in);
    buf += available;
    len -= available;
  }

  while (len >= 64) {
    memcpy(ctx.in, buf, 64);
    md5Transform(ctx.buf, ctx.in);
    buf += 64;
    len -= 64;
  }

  memcpy(ctx.in, buf, len);
}

void md5Final(uint8_t digest[16], Md5Context& ctx) {
  const size_t used = (ctx.bits[0] >> 3) & 0x3f;
  uint8_t* p = ctx.in + used;
  *p++ = 0x80;

  const size_t available = 63 - used;
  if (available < 8) {
    memset(p, 0, available);
    md5Transform(ctx.buf, ctx.in);
    memset(ctx.in, 0, 56);
  } else {
    memset(p, 0, available - 8);
  }

  ctx.in[56] = static_cast<uint8_t>(ctx.bits[0]);
  ctx.in[57] = static_cast<uint8_t>(ctx.bits[0] >> 8);
  ctx.in[58] = static_cast<uint8_t>(ctx.bits[0] >> 16);
  ctx.in[59] = static_cast<uint8_t>(ctx.bits[0] >> 24);
  ctx.in[60] = static_cast<uint8_t>(ctx.bits[1]);
  ctx.in[61] = static_cast<uint8_t>(ctx.bits[1] >> 8);
  ctx.in[62] = static_cast<uint8_t>(ctx.bits[1] >> 16);
  ctx.in[63] = static_cast<uint8_t>(ctx.bits[1] >> 24);

  md5Transform(ctx.buf, ctx.in);

  for (int i = 0; i < 16; i++) {
    digest[i] = static_cast<uint8_t>(ctx.buf[i >> 2] >> ((i & 3) << 3));
  }
}

std::string toHex(const uint8_t digest[16]) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string result;
  result.resize(32);
  for (int i = 0; i < 16; i++) {
    result[i * 2] = kHexDigits[digest[i] >> 4];
    result[i * 2 + 1] = kHexDigits[digest[i] & 0x0f];
  }
  return result;
}

constexpr size_t kChunkSize = 1024;
constexpr int kOffsetCount = 12;  // i = -1 .. 10

size_t offsetForIndex(const int i) {
  if (i < 0) {
    return 0;
  }
  return kChunkSize << (2 * i);
}

}  // namespace

std::string partialContentHashFromReader(const size_t fileSize, const ChunkReadFn readChunk, void* ctx) {
  if (fileSize == 0 || readChunk == nullptr) {
    return "";
  }

  Md5Context md5ctx;
  md5Init(md5ctx);

  uint8_t buffer[kChunkSize];
  bool anyBytesRead = false;

  for (int i = -1; i < kOffsetCount - 1; i++) {
    const size_t offset = offsetForIndex(i);
    if (offset >= fileSize) {
      continue;
    }

    const size_t bytesRead = readChunk(ctx, offset, buffer, kChunkSize);
    if (bytesRead > 0) {
      md5Update(md5ctx, buffer, bytesRead);
      anyBytesRead = true;
    }
  }

  if (!anyBytesRead) {
    return "";
  }

  uint8_t digest[16];
  md5Final(digest, md5ctx);
  return toHex(digest);
}

}  // namespace FsHelpers
