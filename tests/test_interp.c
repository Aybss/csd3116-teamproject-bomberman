/* test_interp.c - unit tests for src/client/interp.c, with a jitter simulation
 * that shows why drawing by arrival time caused rubberbanding. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "interp.h"
#include "util.h"

static int g_failed, g_checks;

#define CHECK(cond)                                                      \
    do {                                                                 \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            g_failed++;                                                  \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                \
    } while (0)

#define TICK_NS   (1000000000LL / BA_TICK_HZ)
#define FRAME_NS  16666667LL                /* 60 fps */
#define SPEED     (BA_BASE_SPEED / BA_TICK_HZ)   /* tiles per tick */

static ba_interp_snap snap_at(uint32_t tick)
{
    ba_interp_snap s;
    memset(&s, 0, sizeof s);
    s.tick = tick;
    s.present[0] = true;
    s.x[0] = (float)tick * SPEED;           /* walking right forever */
    s.y[0] = 1.0f;
    return s;
}

typedef struct { int backwards, freezes; float max_step; } motion_stats;

/* A snapshot every 2 ticks, each arrives after base + random jitter.
 * Render at 60 fps with the old way (blend the last two ARRIVALS) or the
 * new way (server timeline + buffer). */
static motion_stats simulate(bool new_method, int max_jitter_ms, uint64_t seed)
{
    enum { NSNAP = 300 };
    int64_t arrive[NSNAP];
    ba_rng rng;
    ba_rng_seed(&rng, seed);
    int64_t t0 = 5000000000LL;               /* our clock when tick 0 left */
    for (int k = 0; k < NSNAP; k++)
    {
        int64_t sent = t0 + (int64_t)k * 2 * TICK_NS;
        int64_t jitter = (int64_t)ba_rng_range(&rng, 0, max_jitter_ms) * 1000000LL;
        arrive[k] = sent + 2000000LL + jitter;
        if (k > 0 && arrive[k] < arrive[k - 1])
            arrive[k] = arrive[k - 1];       /* keep delivery in order */
    }

    ba_interp b;
    ba_interp_reset(&b);
    ba_interp_snap prev = snap_at(0), curr = snap_at(0);
    int64_t prev_ns = 0, curr_ns = 0;
    int next = 0;
    motion_stats st = { 0, 0, 0.0f };
    float last = NAN;

    for (int64_t now = t0 + 300000000LL; now < arrive[NSNAP - 1]; now += FRAME_NS)
    {
        while (next < NSNAP && arrive[next] <= now)
        {
            ba_interp_snap s = snap_at((uint32_t)next * 2);
            ba_interp_push(&b, &s, arrive[next]);
            prev = curr; prev_ns = curr_ns;
            curr = s;    curr_ns = arrive[next];
            next++;
        }
        float x, y;
        if (new_method)
        {
            if (!ba_interp_sample(&b, 0, now, &x, &y))
                continue;
        }
        else
        {
            float span = (float)(curr_ns - prev_ns);
            float u = span > 0 ? (float)(now - curr_ns) / span : 1.0f;
            u = u < 0 ? 0 : (u > 1 ? 1 : u);
            x = prev.x[0] + (curr.x[0] - prev.x[0]) * u;
        }
        if (!isnan(last))
        {
            float step = x - last;
            if (step < -1e-5f) st.backwards++;
            if (fabsf(step) < 1e-5f) st.freezes++;
            if (step > st.max_step) st.max_step = step;
        }
        last = x;
    }
    return st;
}

static void test_jitter(void)
{
    puts("jitter: old method stutters, new method is smooth");
    float ideal = (float)(SPEED * FRAME_NS / TICK_NS);         /* ~0.067 tiles/frame */
    const int jitters[] = { 10, 30, 45 };
    for (int j = 0; j < 3; j++)
    {
        motion_stats o = simulate(false, jitters[j], 99);
        motion_stats n = simulate(true, jitters[j], 99);
        printf("  jitter 0-%2d ms | old: freezes %3d, max step %.3f | "
               "new: freezes %3d, max step %.3f (ideal %.3f)\n",
               jitters[j], o.freezes, o.max_step, n.freezes, n.max_step, ideal);
        CHECK(n.backwards == 0);
        CHECK(n.freezes == 0);                  /* never stops while walking */
        CHECK(n.max_step < ideal * 1.25f);       /* never jumps to catch up  */
    }
}

static void test_basics(void)
{
    puts("buffer basics: hold, ignore old ticks, reset, respawn, absent");
    ba_interp b;
    ba_interp_reset(&b);
    float x, y;
    CHECK(!ba_interp_sample(&b, 0, 0, &x, &y));               /* empty */

    int64_t base = 1000000000LL;
    for (uint32_t k = 0; k < 6; k++)
    {
        ba_interp_snap s = snap_at(k * 2);
        ba_interp_push(&b, &s, base + (int64_t)k * 2 * TICK_NS);
    }
    /* Far in the future: hold the newest position, do not guess ahead. */
    CHECK(ba_interp_sample(&b, 0, base + 5000000000LL, &x, &y));
    CHECK(fabsf(x - 10 * (float)SPEED) < 1e-4f);

    /* An old (reordered) snapshot is ignored. */
    ba_interp_snap old = snap_at(4);
    old.x[0] = 99.0f;
    ba_interp_push(&b, &old, base + 12 * TICK_NS);
    CHECK(b.snap[b.head].tick == 10);

    /* Server restart (tick goes back to 0): start over. */
    ba_interp_snap r = snap_at(0);
    ba_interp_push(&b, &r, base + 2000000000LL);
    CHECK(b.count == 1 && b.snap[b.head].tick == 0);

    /* Respawn: a jump of over 1.5 tiles snaps instead of sliding. */
    ba_interp_reset(&b);
    ba_interp_snap a = snap_at(0), c = snap_at(2);
    a.x[0] = 17.0f;
    c.x[0] = 1.0f;
    ba_interp_push(&b, &a, base);
    ba_interp_push(&b, &c, base + 2 * TICK_NS);
    int64_t mid = base + TICK_NS + BA_INTERP_DELAY_MS * 1000000LL;
    CHECK(ba_interp_sample(&b, 0, mid - 1000000, &x, &y) && (x == 17.0f || x == 1.0f));

    /* Player not present at all. */
    CHECK(!ba_interp_sample(&b, 3, mid, &x, &y));
    CHECK(!ba_interp_sample(&b, 7, mid, &x, &y));               /* bad index */
}

int main(void)
{
    test_basics();
    test_jitter();
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
