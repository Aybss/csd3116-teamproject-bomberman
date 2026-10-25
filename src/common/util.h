/* util.h - small helpers: time, random numbers, logging, security config. */
#ifndef BA_UTIL_H
#define BA_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

/* Monotonic clock in milliseconds / nanoseconds. It never goes backwards. */
int64_t ba_now_ms(void);
int64_t ba_now_ns(void);

/* Sleep until an exact monotonic time (so the tick does not drift). */
void ba_sleep_until_ns(int64_t deadline_ns);

/* 64 random bits from the OS. Used for join nonces. */
uint64_t ba_random_u64(void);

/* Small seeded random generator. Each user has its own state, so tests can repeat. */
typedef struct { uint64_t s; } ba_rng;
void     ba_rng_seed(ba_rng *r, uint64_t seed);
uint32_t ba_rng_next(ba_rng *r);
int      ba_rng_range(ba_rng *r, int lo, int hi);   /* inclusive */

/* Logging. Debug lines are only printed with -v. */
extern int ba_verbose;
#define BA_LOG(...)      do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define BA_LOG_DEBUG(...) do { if (ba_verbose) BA_LOG(__VA_ARGS__); } while (0)

/* If path is not NULL, use it as the Cyclone DDS XML config (this turns on
   security). Call it before the participant is created. */
bool ba_use_dds_config(const char *path);

#endif /* BA_UTIL_H */
