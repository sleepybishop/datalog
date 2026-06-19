#ifndef DATALOG_RANDOM_H
#define DATALOG_RANDOM_H

#include <stdint.h>

void xoshiro256_seed(uint64_t seed);
uint64_t xoshiro256_next(void);

#endif
