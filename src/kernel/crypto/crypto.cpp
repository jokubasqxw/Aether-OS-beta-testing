#include "crypto.hpp"
#include "../kstring.hpp"
#include "../pit.hpp"
#include "uecc/uECC.h"
#include "sha/sha256.h"

#define CBC 0
#define ECB 1
#define CTR 0
#include "aes/aes.h"

extern "C" int aether_random_bytes(uint8_t* dest, unsigned size);

namespace crypto {


static void sha256_raw(const uint8_t* data, size_t len, uint8_t out[32]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    if (data && len) sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    sha256_raw(data, len, out);
}

void hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t len, uint8_t out[32]) {
    uint8_t k[64];
    kstd::memset(k, 0, sizeof(k));
    if (key_len > 64) sha256_raw(key, key_len, k);
    else if (key && key_len) kstd::memcpy(k, key, key_len);

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
        opad[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
    }

    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    if (data && len) sha256_update(&ctx, data, len);
    uint8_t inner[32];
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
}

bool ecdh_generate(uint8_t public_key[64], uint8_t private_key[32]) {
    uECC_set_rng(&aether_random_bytes);
    return uECC_make_key(public_key, private_key, uECC_secp256r1()) != 0;
}

bool ecdh_shared(const uint8_t peer_public_key[64], const uint8_t private_key[32], uint8_t secret[32]) {
    uECC_set_rng(&aether_random_bytes);
    return uECC_shared_secret(peer_public_key, private_key, secret, uECC_secp256r1()) != 0;
}

static void aes_block(const AES_ctx* ctx, uint8_t block[16]) {
    AES_ECB_encrypt(ctx, block);
}

static void xor16(uint8_t* dst, const uint8_t* src) {
    for (int i = 0; i < 16; ++i) dst[i] ^= src[i];
}

// GHASH multiplication in GF(2^128), NIST SP 800-38D.
static void ghash_mul(uint8_t x[16], const uint8_t h[16]) {
    uint8_t z[16];
    uint8_t v[16];
    kstd::memset(z, 0, 16);
    kstd::memcpy(v, h, 16);
    for (int i = 0; i < 128; ++i) {
        if (x[i >> 3] & (0x80u >> (i & 7))) xor16(z, v);
        bool lsb = (v[15] & 1) != 0;
        for (int j = 15; j > 0; --j) v[j] = static_cast<uint8_t>((v[j] >> 1) | (v[j - 1] << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;
    }
    kstd::memcpy(x, z, 16);
}

static void ghash_block(uint8_t state[16], const uint8_t h[16], const uint8_t block[16]) {
    xor16(state, block);
    ghash_mul(state, h);
}

static void ghash_pad(uint8_t state[16], const uint8_t h[16], const uint8_t* data, size_t len) {
    uint8_t block[16];
    while (len >= 16) {
        kstd::memcpy(block, data, 16);
        ghash_block(state, h, block);
        data += 16;
        len -= 16;
    }
    if (len) {
        kstd::memset(block, 0, 16);
        kstd::memcpy(block, data, len);
        ghash_block(state, h, block);
    }
}

static void ghash_lengths(uint8_t state[16], const uint8_t h[16], uint64_t aad_bits, uint64_t data_bits) {
    uint8_t block[16];
    for (int i = 0; i < 8; ++i) {
        block[i] = static_cast<uint8_t>(aad_bits >> (56 - i * 8));
        block[8 + i] = static_cast<uint8_t>(data_bits >> (56 - i * 8));
    }
    ghash_block(state, h, block);
}

static void make_j0(const uint8_t nonce[12], uint8_t j0[16]) {
    kstd::memcpy(j0, nonce, 12);
    j0[12] = 0;
    j0[13] = 0;
    j0[14] = 0;
    j0[15] = 1;
}

static void ctr_crypt(const AES_ctx* ctx, const uint8_t nonce[12], const uint8_t* in, size_t len, uint8_t* out) {
    uint8_t counter[16];
    kstd::memcpy(counter, nonce, 12);
    uint32_t c = 2; // J0+1, J0 uses low word 1
    size_t off = 0;
    while (off < len) {
        counter[12] = static_cast<uint8_t>(c >> 24);
        counter[13] = static_cast<uint8_t>(c >> 16);
        counter[14] = static_cast<uint8_t>(c >> 8);
        counter[15] = static_cast<uint8_t>(c);
        uint8_t ks[16];
        kstd::memcpy(ks, counter, 16);
        AES_ECB_encrypt(ctx, ks);
        size_t n = len - off;
        if (n > 16) n = 16;
        for (size_t i = 0; i < n; ++i) out[off + i] = static_cast<uint8_t>(in[off + i] ^ ks[i]);
        off += n;
        ++c;
    }
}

bool aes128_gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* plain, size_t plain_len,
                        uint8_t* cipher, uint8_t tag[16]) {
    AES_ctx ctx;
    AES_init_ctx(&ctx, key);

    uint8_t h[16];
    kstd::memset(h, 0, 16);
    aes_block(&ctx, h);

    ctr_crypt(&ctx, nonce, plain, plain_len, cipher);

    uint8_t state[16];
    kstd::memset(state, 0, 16);
    if (aad_len) ghash_pad(state, h, aad, aad_len);
    if (plain_len) ghash_pad(state, h, cipher, plain_len);
    ghash_lengths(state, h, static_cast<uint64_t>(aad_len) * 8, static_cast<uint64_t>(plain_len) * 8);

    uint8_t j0[16];
    make_j0(nonce, j0);
    aes_block(&ctx, j0);
    for (int i = 0; i < 16; ++i) tag[i] = static_cast<uint8_t>(state[i] ^ j0[i]);
    return true;
}

bool aes128_gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* cipher, size_t cipher_len,
                        const uint8_t tag[16], uint8_t* plain) {
    AES_ctx ctx;
    AES_init_ctx(&ctx, key);

    uint8_t h[16];
    kstd::memset(h, 0, 16);
    aes_block(&ctx, h);

    uint8_t state[16];
    kstd::memset(state, 0, 16);
    if (aad_len) ghash_pad(state, h, aad, aad_len);
    if (cipher_len) ghash_pad(state, h, cipher, cipher_len);
    ghash_lengths(state, h, static_cast<uint64_t>(aad_len) * 8, static_cast<uint64_t>(cipher_len) * 8);

    uint8_t j0[16];
    make_j0(nonce, j0);
    aes_block(&ctx, j0);
    uint8_t expected[16];
    for (int i = 0; i < 16; ++i) expected[i] = static_cast<uint8_t>(state[i] ^ j0[i]);

    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) diff |= static_cast<uint8_t>(expected[i] ^ tag[i]);
    if (diff != 0) return false;

    ctr_crypt(&ctx, nonce, cipher, cipher_len, plain);
    return true;
}

int random_bytes(uint8_t* out, size_t len) {
    if (!out) return 0;
    static uint64_t state = 0;
    if (state == 0) {
        uint32_t lo = 0, hi = 0;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        state = (static_cast<uint64_t>(hi) << 32) | lo;
        state ^= pit::get_ticks() * 0x9e3779b97f4a7c15ull;
        state ^= reinterpret_cast<uintptr_t>(&state);
        if (state == 0) state = 0x123456789abcdef0ull;
    }
    for (size_t i = 0; i < len; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        out[i] = static_cast<uint8_t>(state >> 24);
    }
    return 1;
}

} // namespace crypto

extern "C" int aether_random_bytes(uint8_t* dest, unsigned size) {
    return crypto::random_bytes(dest, size);
}
