#include "fmtrans/sha1.hpp"

#include <cstdint>
#include <iomanip>
#include <sstream>

namespace fmtrans {
namespace {

uint32_t rotl(uint32_t value, int bits) {
  return (value << bits) | (value >> (32 - bits));
}

}  // namespace

std::string sha1Raw(const std::string& input) {
  const uint64_t bitLength = static_cast<uint64_t>(input.size()) * 8;
  std::string msg = input;
  msg.push_back(static_cast<char>(0x80));
  while (msg.size() % 64 != 56) {
    msg.push_back(0);
  }
  for (int i = 7; i >= 0; --i) {
    msg.push_back(static_cast<char>((bitLength >> (i * 8)) & 0xff));
  }

  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  for (size_t off = 0; off < msg.size(); off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const auto* p = reinterpret_cast<const unsigned char*>(&msg[off + i * 4]);
      w[i] = (static_cast<uint32_t>(p[0]) << 24) |
             (static_cast<uint32_t>(p[1]) << 16) |
             (static_cast<uint32_t>(p[2]) << 8) | p[3];
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f = 0, k = 0;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const uint32_t tmp = rotl(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotl(b, 30);
      b = a;
      a = tmp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  std::string digest;
  digest.reserve(20);
  for (uint32_t value : h) {
    digest.push_back(static_cast<char>((value >> 24) & 0xff));
    digest.push_back(static_cast<char>((value >> 16) & 0xff));
    digest.push_back(static_cast<char>((value >> 8) & 0xff));
    digest.push_back(static_cast<char>(value & 0xff));
  }
  return digest;
}

std::string sha1Hex(const std::string& input) {
  const std::string raw = sha1Raw(input);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (unsigned char byte : raw) {
    out << std::setw(2) << static_cast<int>(byte);
  }
  return out.str();
}

std::string base64Encode(const std::string& input) {
  static const char* table =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  while (i < input.size()) {
    const unsigned char b0 = static_cast<unsigned char>(input[i]);
    const unsigned char b1 = i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0;
    const unsigned char b2 = i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0;
    out += table[b0 >> 2];
    out += table[((b0 & 0x03) << 4) | (b1 >> 4)];
    out += i + 1 < input.size() ? table[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=';
    out += i + 2 < input.size() ? table[b2 & 0x3f] : '=';
    i += 3;
  }
  return out;
}

}  // namespace fmtrans
