#include "vnc_des.h"
#include <cstring>

namespace esphome::vnc {

// Standard DES tables (FIPS 46-3).
static const uint8_t PC1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18, 10, 2,  59, 51, 43,
                                35, 27, 19, 11, 3,  60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7,  62, 54,
                                46, 38, 30, 22, 14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};
static const uint8_t PC2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
                                26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
                                51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
static const uint8_t SHIFTS[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
static const uint8_t IP[64] = {58, 50, 42, 34, 26, 18, 10, 2,  60, 52, 44, 36, 28, 20, 12, 4,  62, 54, 46, 38, 30, 22,
                               14, 6,  64, 56, 48, 40, 32, 24, 16, 8,  57, 49, 41, 33, 25, 17, 9,  1,  59, 51, 43, 35,
                               27, 19, 11, 3,  61, 53, 45, 37, 29, 21, 13, 5,  63, 55, 47, 39, 31, 23, 15, 7};
static const uint8_t FP[64] = {40, 8,  48, 16, 56, 24, 64, 32, 39, 7,  47, 15, 55, 23, 63, 31, 38, 6,  46, 14, 54, 22,
                               62, 30, 37, 5,  45, 13, 53, 21, 61, 29, 36, 4,  44, 12, 52, 20, 60, 28, 35, 3,  43, 11,
                               51, 19, 59, 27, 34, 2,  42, 10, 50, 18, 58, 26, 33, 1,  41, 9,  49, 17, 57, 25};
static const uint8_t E[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11,
                              12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
                              22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
static const uint8_t P[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
                              2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25};
static const uint8_t SBOX[8][64] = {
    {14, 4,  13, 1, 2,  15, 11, 8, 3, 10, 6, 12, 5,  9,  0,  7,  0,  15, 7,  4,  14, 2,
     13, 1,  10, 6, 12, 11, 9,  5, 3, 8,  4, 1,  14, 8,  13, 6,  2,  11, 15, 12, 9,  7,
     3,  10, 5,  0, 15, 12, 8,  2, 4, 9,  1, 7,  5,  11, 3,  14, 10, 0,  6,  13},
    {15, 1,  8, 14, 6,  11, 3,  4, 9, 7, 2,  13, 12, 0, 5, 10, 3,  13, 4,  7, 15, 2,  8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
     0,  14, 7, 11, 10, 4,  13, 1, 5, 8, 12, 6,  9,  3, 2, 15, 13, 8,  10, 1, 3,  15, 4, 2,  11, 6, 7, 12, 0, 5, 14, 9},
    {10, 0,  9,  14, 6, 3,  15, 5,  1,  13, 12, 7, 11, 4,  2,  8,  13, 7, 0,  9, 3, 4,
     6,  10, 2,  8,  5, 14, 12, 11, 15, 1,  13, 6, 4,  9,  8,  15, 3,  0, 11, 1, 2, 12,
     5,  10, 14, 7,  1, 10, 13, 0,  6,  9,  8,  7, 4,  15, 14, 3,  11, 5, 2,  12},
    {7, 13, 14, 3, 0, 6,  9, 10, 1,  2, 8,  5, 11, 12, 4,  15, 13, 8,  11, 5, 6, 15,
     0, 3,  4,  7, 2, 12, 1, 10, 14, 9, 10, 6, 9,  0,  12, 11, 7,  13, 15, 1, 3, 14,
     5, 2,  8,  4, 3, 15, 0, 6,  10, 1, 13, 8, 9,  4,  5,  11, 12, 7,  2,  14},
    {2,  12, 4, 1,  7,  10, 11, 6, 8, 5,  3, 15, 13, 0,  14, 9,  14, 11, 2,  12, 4,  7,
     13, 1,  5, 0,  15, 10, 3,  9, 8, 6,  4, 2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,
     6,  3,  0, 14, 11, 8,  12, 7, 1, 14, 2, 13, 6,  15, 0,  9,  10, 4,  5,  3},
    {12, 1,  10, 15, 9,  2,  6, 8,  0, 13, 3,  4,  14, 7,  5, 11, 10, 15, 4, 2, 7, 12,
     9,  5,  6,  1,  13, 14, 0, 11, 3, 8,  9,  14, 15, 5,  2, 8,  12, 3,  7, 0, 4, 10,
     1,  13, 11, 6,  4,  3,  2, 12, 9, 5,  15, 10, 11, 14, 1, 7,  6,  0,  8, 13},
    {4, 11, 2,  14, 15, 0, 8, 13, 3,  12, 9, 7, 5, 10, 6, 1, 13, 0,  11, 7, 4, 9, 1,  10, 14, 3, 5, 12, 2,  15, 8, 6,
     1, 4,  11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5,  9, 2, 6,  11, 13, 8, 1, 4, 10, 7,  9,  5, 0, 15, 14, 2,  3, 12},
    {13, 2, 8, 4, 6,  15, 11, 1, 10, 9,  3,  14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7,  4,  12, 5, 6, 11, 0, 14, 9, 2, 7,
     11, 4, 1, 9, 12, 14, 2,  0, 6,  10, 13, 15, 3, 5, 8,  2, 1, 14, 7,  4, 10, 8, 13, 15, 12, 9, 0, 3,  5, 6,  11}};

/// Read bit n (1-based, MSB first) out of a byte buffer.
static inline uint8_t get_bit(const uint8_t *src, uint8_t n) { return (src[(n - 1) >> 3] >> (7 - ((n - 1) & 7))) & 1; }

void DesCipher::set_key(const uint8_t key[8]) {
  // PC1: 64 key bits -> two 28 bit halves
  uint32_t c = 0, d = 0;
  for (int i = 0; i != 28; i++)
    c = (c << 1) | get_bit(key, PC1[i]);
  for (int i = 28; i != 56; i++)
    d = (d << 1) | get_bit(key, PC1[i]);

  for (int round = 0; round != 16; round++) {
    for (uint8_t s = 0; s != SHIFTS[round]; s++) {
      c = ((c << 1) | (c >> 27)) & 0x0FFFFFFF;
      d = ((d << 1) | (d >> 27)) & 0x0FFFFFFF;
    }
    // PC2: the 56 bits of CD -> a 48 bit subkey, held as two 24 bit words
    uint8_t cd[7];
    uint64_t joined = ((uint64_t) c << 28) | d;
    for (int i = 0; i != 7; i++)
      cd[i] = (uint8_t) (joined >> (48 - 8 * i));
    uint32_t k0 = 0, k1 = 0;
    for (int i = 0; i != 24; i++)
      k0 = (k0 << 1) | get_bit(cd, PC2[i]);
    for (int i = 24; i != 48; i++)
      k1 = (k1 << 1) | get_bit(cd, PC2[i]);
    this->subkeys_[round][0] = k0;
    this->subkeys_[round][1] = k1;
  }
}

void DesCipher::encrypt_block(const uint8_t in[8], uint8_t out[8]) const {
  // initial permutation
  uint32_t l = 0, r = 0;
  for (int i = 0; i != 32; i++)
    l = (l << 1) | get_bit(in, IP[i]);
  for (int i = 32; i != 64; i++)
    r = (r << 1) | get_bit(in, IP[i]);

  for (int round = 0; round != 16; round++) {
    // expansion of R to 48 bits, as two 24 bit words to match the subkey layout
    uint8_t rb[4] = {(uint8_t) (r >> 24), (uint8_t) (r >> 16), (uint8_t) (r >> 8), (uint8_t) r};
    uint32_t e0 = 0, e1 = 0;
    for (int i = 0; i != 24; i++)
      e0 = (e0 << 1) | get_bit(rb, E[i]);
    for (int i = 24; i != 48; i++)
      e1 = (e1 << 1) | get_bit(rb, E[i]);
    e0 ^= this->subkeys_[round][0];
    e1 ^= this->subkeys_[round][1];

    // eight S-boxes, 6 bits in / 4 bits out
    uint32_t sout = 0;
    for (int box = 0; box != 8; box++) {
      uint32_t chunk = box < 4 ? (e0 >> (18 - 6 * box)) : (e1 >> (18 - 6 * (box - 4)));
      chunk &= 0x3F;
      // row is the outer two bits, column the middle four
      uint8_t row = (uint8_t) (((chunk >> 5) & 1) * 2 + (chunk & 1));
      uint8_t col = (uint8_t) ((chunk >> 1) & 0x0F);
      sout = (sout << 4) | SBOX[box][row * 16 + col];
    }

    // P permutation, then the Feistel swap
    uint8_t sb[4] = {(uint8_t) (sout >> 24), (uint8_t) (sout >> 16), (uint8_t) (sout >> 8), (uint8_t) sout};
    uint32_t f = 0;
    for (int i = 0; i != 32; i++)
      f = (f << 1) | get_bit(sb, P[i]);
    uint32_t prev_r = r;
    r = l ^ f;
    l = prev_r;
  }

  // final permutation, applied to R then L (the halves are swapped after the last round)
  uint8_t pre[8] = {(uint8_t) (r >> 24), (uint8_t) (r >> 16), (uint8_t) (r >> 8), (uint8_t) r,
                    (uint8_t) (l >> 24), (uint8_t) (l >> 16), (uint8_t) (l >> 8), (uint8_t) l};
  uint8_t result[8] = {};
  for (int i = 0; i != 64; i++)
    result[i >> 3] = (uint8_t) ((result[i >> 3] << 1) | get_bit(pre, FP[i]));
  memcpy(out, result, 8);
}

void vnc_auth_response(const char *password, size_t password_len, const uint8_t challenge[16], uint8_t response[16]) {
  // The password is truncated or NUL-padded to eight bytes, and the bits within each byte are
  // reversed before use as the DES key - an artefact of the original VNC implementation that
  // every client and server has to reproduce to interoperate.
  uint8_t key[8] = {};
  for (size_t i = 0; i != 8 && i != password_len; i++) {
    uint8_t v = (uint8_t) password[i];
    uint8_t rev = 0;
    for (int b = 0; b != 8; b++)
      rev = (uint8_t) (rev | (((v >> b) & 1) << (7 - b)));
    key[i] = rev;
  }
  DesCipher des;
  des.set_key(key);
  des.encrypt_block(challenge, response);
  des.encrypt_block(challenge + 8, response + 8);
}

}  // namespace esphome::vnc
