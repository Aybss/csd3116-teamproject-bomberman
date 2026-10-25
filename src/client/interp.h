/* interp.h - smooth player positions between server snapshots.
 *
 * Problem: snapshots leave the server every 33 ms, but they arrive unevenly
 * (15-80 ms apart is normal on WSL). If we move players by arrival time,
 * they freeze when a snapshot is late, then jump. That looks like rubberbanding.
 *
 * Fix: each snapshot has the server tick it was made at, so we place
 * snapshots on the SERVER's timeline:
 *   1. Keep the last few snapshots in a small ring buffer.
 *   2. Estimate the clock offset from the fastest arrivals.
 *   3. Draw the world slightly in the past (BA_INTERP_DELAY_MS), so there is
 *      nearly always a snapshot on both sides, and blend between them.
 * A late snapshot is not needed yet, so nothing freezes.
 *
 * Pure C (no OpenGL or DDS), so it is unit tested (tests/test_interp.c).
 */
#ifndef BA_INTERP_H
#define BA_INTERP_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#define BA_INTERP_SLOTS     8       /* snapshots kept (~260 ms)            */
#define BA_INTERP_DELAY_MS  80      /* how far in the past we draw         */

typedef struct
{
    uint32_t tick;
    float    x[BA_MAX_PLAYERS], y[BA_MAX_PLAYERS];
    bool     present[BA_MAX_PLAYERS];   /* active and alive */
} ba_interp_snap;

typedef struct
{
    ba_interp_snap snap[BA_INTERP_SLOTS];   /* ring buffer, ordered by tick */
    int      count;
    int      head;                          /* index of the newest snapshot */
    bool     have_offset;
    double   offset_ns;                     /* best estimate of arrival_ns - tick * tick_ns */
    double   render_offset_ns;              /* follows offset_ns slowly, so no jumps */
} ba_interp;

void ba_interp_reset(ba_interp *b);

/* Add a snapshot that arrived at arrival_ns (our monotonic clock).
 * Returns the ring slot it was stored in (0..BA_INTERP_SLOTS-1), or -1 if it
 * was older than what we have. Callers can keep extra data per snapshot in an
 * array indexed by this slot. */
int  ba_interp_push(ba_interp *b, const ba_interp_snap *s, int64_t arrival_ns);

/* Ring slot of the snapshot to show at now_ns: the newest one that is not
   newer than the render time. -1 if empty. Used to draw bombs, flames and the
   HUD from the same moment as the players. */
int  ba_interp_slot_at(const ba_interp *b, int64_t now_ns);

/* Position of player i (0-based) to draw at now_ns.
 * Returns false if the player is not in the buffered snapshots. */
bool ba_interp_sample(const ba_interp *b, int i, int64_t now_ns, float *x, float *y);

#endif /* BA_INTERP_H */
