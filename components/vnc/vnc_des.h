#pragma once
#include <cstdint>
#include <cstddef>
namespace esphome::vnc {
/// Single-block DES encryption, sufficient for RFB "VNC Authentication" (security type 2).
/// Not a general purpose crypto primitive - DES is obsolete, but the RFB handshake specifies it.
class DesCipher {
 public:
  /// Derive the round keys from an 8-byte key.
  void set_key(const uint8_t key[8]);
  /// Encrypt one 8-byte block in place-safe fashion (in and out may alias).
  void encrypt_block(const uint8_t in[8], uint8_t out[8]) const;

 protected:
  uint32_t subkeys_[16][2]{};
};
/// Compute the RFB VNC-Auth response: DES-encrypt the 16-byte challenge in two ECB blocks
/// using the password (NUL-padded/truncated to 8 bytes) with the bits of each key byte
/// reversed, which is what every VNC implementation does.
void vnc_auth_response(const char *password, size_t password_len, const uint8_t challenge[16], uint8_t response[16]);
}  // namespace esphome::vnc
