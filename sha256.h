#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;
    size_t fill;
} Sha256;

void sha256_init(Sha256 *c);
void sha256_update(Sha256 *c, const void *data, size_t n);
void sha256_final(Sha256 *c, uint8_t out[32]);
void sha256(const void *data, size_t n, uint8_t out[32]);

/* Low-level: process one 64-byte block into a state (used for midstate mining). */
void sha256_compress(uint32_t state[8], const uint8_t block[64]);

#endif
