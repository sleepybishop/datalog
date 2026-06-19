#include "random.h"

// splitmix64 state
static uint64_t splitmix64_x = 1234567890123456789ULL;

static void splitmix64_seed(uint64_t seed)
{
    splitmix64_x = seed;
}

static uint64_t splitmix64_next(void)
{
    uint64_t z = (splitmix64_x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// xoshiro256** state
static uint64_t xoshiro256_state[4] = {0x1234567890abcdefULL, 0xfedcba0987654321ULL, 0x0f0f0f0f0f0f0f0fULL, 0xf0f0f0f0f0f0f0f0ULL};

static inline uint64_t rotl(const uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

void xoshiro256_seed(uint64_t seed)
{
    splitmix64_seed(seed);
    xoshiro256_state[0] = splitmix64_next();
    xoshiro256_state[1] = splitmix64_next();
    xoshiro256_state[2] = splitmix64_next();
    xoshiro256_state[3] = splitmix64_next();
}

uint64_t xoshiro256_next(void)
{
    const uint64_t result = rotl(xoshiro256_state[1] * 5, 7) * 9;
    const uint64_t t = xoshiro256_state[1] << 17;

    xoshiro256_state[2] ^= xoshiro256_state[0];
    xoshiro256_state[3] ^= xoshiro256_state[1];
    xoshiro256_state[1] ^= xoshiro256_state[2];
    xoshiro256_state[0] ^= xoshiro256_state[3];

    xoshiro256_state[2] ^= t;
    xoshiro256_state[3] = rotl(xoshiro256_state[3], 45);

    return result;
}
