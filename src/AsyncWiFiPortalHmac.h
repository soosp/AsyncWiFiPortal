/**
 * @file AsyncWiFiPortalHmac.h
 * @brief Portable SHA-256 + HMAC-SHA256 + Crockford base32 for the MacDerived AP
 *        password.
 *
 * Self-contained (no mbedTLS/BearSSL), so the output is byte-identical on ESP32
 * and ESP8266 and matches tools/genpass.py. SHA-256 is a trimmed public-domain
 * implementation (Brad Conte style).
 */
#pragma once

#include <stdint.h>
#include <string.h>

namespace wifiportal_detail {

struct Sha256 {
    uint32_t s[8];
    uint64_t len;
    uint8_t  buf[64];
    uint32_t n;
};

inline uint32_t _ror(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

inline void _sha256_block(Sha256& c, const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i*4]<<24 | (uint32_t)p[i*4+1]<<16 | (uint32_t)p[i*4+2]<<8 | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = _ror(w[i-15],7) ^ _ror(w[i-15],18) ^ (w[i-15]>>3);
        uint32_t s1 = _ror(w[i-2],17) ^ _ror(w[i-2],19) ^ (w[i-2]>>10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c.s[0],b=c.s[1],cc=c.s[2],d=c.s[3],e=c.s[4],f=c.s[5],g=c.s[6],h=c.s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = _ror(e,6) ^ _ror(e,11) ^ _ror(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = _ror(a,2) ^ _ror(a,13) ^ _ror(a,22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c.s[0]+=a; c.s[1]+=b; c.s[2]+=cc; c.s[3]+=d; c.s[4]+=e; c.s[5]+=f; c.s[6]+=g; c.s[7]+=h;
}

inline void sha256_init(Sha256& c) {
    c.s[0]=0x6a09e667; c.s[1]=0xbb67ae85; c.s[2]=0x3c6ef372; c.s[3]=0xa54ff53a;
    c.s[4]=0x510e527f; c.s[5]=0x9b05688c; c.s[6]=0x1f83d9ab; c.s[7]=0x5be0cd19;
    c.len=0; c.n=0;
}
inline void sha256_update(Sha256& c, const uint8_t* p, uint32_t n) {
    c.len += n;
    while (n) {
        uint32_t take = 64 - c.n; if (take > n) take = n;
        memcpy(c.buf + c.n, p, take); c.n += take; p += take; n -= take;
        if (c.n == 64) { _sha256_block(c, c.buf); c.n = 0; }
    }
}
inline void sha256_final(Sha256& c, uint8_t out[32]) {
    uint64_t bits = c.len * 8;
    uint8_t pad = 0x80; sha256_update(c, &pad, 1);
    uint8_t z = 0; while (c.n != 56) sha256_update(c, &z, 1);
    uint8_t lb[8]; for (int i=0;i<8;i++) lb[i] = (uint8_t)(bits >> (56-8*i));
    sha256_update(c, lb, 8);
    for (int i=0;i<8;i++){ out[i*4]=c.s[i]>>24; out[i*4+1]=c.s[i]>>16; out[i*4+2]=c.s[i]>>8; out[i*4+3]=c.s[i]; }
}

/**
 * @brief HMAC-SHA256.
 * @param key  Key bytes.
 * @param klen Key length.
 * @param msg  Message bytes.
 * @param mlen Message length.
 * @param out  32-byte output buffer.
 */
inline void hmac_sha256(const uint8_t* key, uint32_t klen,
                        const uint8_t* msg, uint32_t mlen, uint8_t out[32]) {
    uint8_t k[64]; memset(k, 0, 64);
    if (klen > 64) { Sha256 c; sha256_init(c); sha256_update(c,key,klen); sha256_final(c,k); }
    else memcpy(k, key, klen);
    uint8_t ip[64], op[64];
    for (int i=0;i<64;i++){ ip[i]=k[i]^0x36; op[i]=k[i]^0x5c; }
    uint8_t ih[32];
    Sha256 c; sha256_init(c); sha256_update(c,ip,64); sha256_update(c,msg,mlen); sha256_final(c,ih);
    sha256_init(c); sha256_update(c,op,64); sha256_update(c,ih,32); sha256_final(c,out);
}

/**
 * @brief Crockford base32 (uppercase, no I/L/O/U), big-endian, left-aligned last
 *        group. Matches tools/genpass.py.
 * @param data  Input bytes.
 * @param n     Number of input bytes.
 * @param out   Output buffer (null-terminated on return).
 * @param outsz Size of @p out.
 */
inline void crockford32(const uint8_t* data, uint32_t n, char* out, uint32_t outsz) {
    static const char A[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    uint32_t bits = 0, nbits = 0, o = 0;
    for (uint32_t i = 0; i < n; i++) {
        bits = (bits << 8) | data[i]; nbits += 8;
        while (nbits >= 5) { nbits -= 5; if (o+1 < outsz) out[o++] = A[(bits >> nbits) & 0x1F]; }
    }
    if (nbits && o+1 < outsz) out[o++] = A[(bits << (5 - nbits)) & 0x1F];
    if (o < outsz) out[o] = '\0';
}

/**
 * @brief Derive the AP password: crockford32( HMAC-SHA256(salt, mac6)[:10] ),
 *        16 characters.
 * @param salt  Null-terminated salt (may be empty).
 * @param mac   6-byte base MAC.
 * @param out   Output buffer (null-terminated on return).
 * @param outsz Size of @p out (>= 17 for the full password).
 */
inline void deriveApPassword(const char* salt, const uint8_t mac[6],
                             char* out, uint32_t outsz) {
    uint8_t h[32];
    hmac_sha256((const uint8_t*)salt, (uint32_t)strlen(salt), mac, 6, h);
    crockford32(h, 10, out, outsz);
}

} // namespace wifiportal_detail
