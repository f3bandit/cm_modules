/* cmremote: SHA-256 (FIPS 180-4), for the app link's device keys.
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md); Retro Remake: see ../LICENSE-RETRO-REMAKE.md. */
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(uint32_t h[8], const uint8_t *p)
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
		uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void sha256(const void *data, size_t len, uint8_t out[32])
{
	uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	const uint8_t *p = data;
	size_t n = len;
	for (; n >= 64; n -= 64, p += 64) block(h, p);
	uint8_t last[128] = { 0 };
	memcpy(last, p, n);
	last[n] = 0x80;
	size_t total = n + 9 <= 64 ? 64 : 128;
	uint64_t bits = (uint64_t)len * 8;
	for (int i = 0; i < 8; i++) last[total - 1 - i] = (uint8_t)(bits >> (i * 8));
	block(h, last);
	if (total == 128) block(h, last + 64);
	for (int i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(h[i] >> 24); out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(h[i] >> 8); out[i * 4 + 3] = (uint8_t)h[i];
	}
}

void sha256_hex(const void *data, size_t len, char out[65])
{
	uint8_t d[32];
	sha256(data, len, d);
	for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
}

/* a whole file, read a piece at a time: 0, or -1 when it can't be read */
int sha256_file(const char *path, char out[65])
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	uint8_t *buf = malloc(1 << 16);
	if (!buf) { fclose(f); return -1; }
	uint8_t carry[64];
	size_t have = 0;
	uint64_t len = 0;
	size_t r;
	while ((r = fread(buf, 1, 1 << 16, f)) > 0) {
		len += r;
		const uint8_t *p = buf;
		if (have) {
			size_t k = 64 - have < r ? 64 - have : r;
			memcpy(carry + have, p, k);
			have += k; p += k; r -= k;
			if (have < 64) continue;
			block(h, carry);
			have = 0;
		}
		for (; r >= 64; r -= 64, p += 64) block(h, p);
		memcpy(carry, p, r);
		have = r;
	}
	int bad = ferror(f);
	fclose(f);
	free(buf);
	if (bad) return -1;
	uint8_t last[128] = { 0 };
	memcpy(last, carry, have);
	last[have] = 0x80;
	size_t total = have + 9 <= 64 ? 64 : 128;
	uint64_t bits = len * 8;
	for (int i = 0; i < 8; i++) last[total - 1 - i] = (uint8_t)(bits >> (i * 8));
	block(h, last);
	if (total == 128) block(h, last + 64);
	for (int i = 0; i < 8; i++) sprintf(out + i * 8, "%08x", h[i]);
	return 0;
}
