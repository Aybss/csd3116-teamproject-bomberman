/* util.c - see util.h */
#define _GNU_SOURCE
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>

int ba_verbose = 0;

int64_t ba_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t ba_now_ms(void)
{
    return ba_now_ns() / 1000000LL;
}

void ba_sleep_until_ns(int64_t deadline_ns)
{
    struct timespec ts;
    ts.tv_sec = deadline_ns / 1000000000LL;
    ts.tv_nsec = deadline_ns % 1000000000LL;
    /* Sleep until an exact time, so a late wake-up does not add drift. */
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
    {
    }
}

uint64_t ba_random_u64(void)
{
    uint64_t v = 0;
    if (getrandom(&v, sizeof v, 0) != (ssize_t)sizeof v)
    {
        /* Unlikely. Use time + pid instead. */
        v = (uint64_t)ba_now_ns() ^ ((uint64_t)getpid() << 32);
    }
    return v ? v : 1; /* 0 means "no nonce"          */
}

void ba_rng_seed(ba_rng *r, uint64_t seed)
{
    r->s = seed ? seed : 0x9E3779B97F4A7C15ULL; /* state must not be 0          */
}

uint32_t ba_rng_next(ba_rng *r)
{
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

int ba_rng_range(ba_rng *r, int lo, int hi)
{
    if (hi <= lo)
        return lo;
    return lo + (int)(ba_rng_next(r) % (uint32_t)(hi - lo + 1));
}

bool ba_use_dds_config(const char *path)
{
    if (path == NULL)
        return true;

    /* Cyclone needs a URI, so turn a relative path into file:///abs/path. */
    char abs[PATH_MAX];
    if (realpath(path, abs) == NULL)
    {
        BA_LOG("Cannot open DDS config '%s': %s", path, strerror(errno));
        return false;
    }
    char uri[PATH_MAX + 16];
    snprintf(uri, sizeof uri, "file://%s", abs);
    setenv("CYCLONEDDS_URI", uri, 1);
    BA_LOG("Using DDS config %s", uri);
    return true;
}
