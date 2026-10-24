#ifndef KERNEL_CRYPTO_HPP
#define KERNEL_CRYPTO_HPP

#include "../types.hpp"

namespace crypto {

struct Sha256 {
    void* ctx;
};

void sha256(const uint8_t* data, size_t len, uint8_t out[32]);
void hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t len, uint8_t out[32]);

bool ecdh_generate(uint8_t public_key[64], uint8_t private_key[32]);
bool ecdh_shared(const uint8_t peer_public_key[64], const uint8_t private_key[32], uint8_t secret[32]);

bool aes128_gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* plain, size_t plain_len,
                        uint8_t* cipher, uint8_t tag[16]);
bool aes128_gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* cipher, size_t cipher_len,
                        const uint8_t tag[16], uint8_t* plain);

int random_bytes(uint8_t* out, size_t len);

} // namespace crypto

#endif // KERNEL_CRYPTO_HPP
