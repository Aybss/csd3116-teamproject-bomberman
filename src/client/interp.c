/* interp.c - snapshot buffer and interpolation. See interp.h. */
#include "interp.h"

#include <math.h>
#include <string.h>

#define TICK_NS (1e9 / BA_TICK_HZ)

void ba_interp_reset(ba_interp *b)
{
    memset(b, 0, sizeof *b);
}

static const ba_interp_snap *at(const ba_interp *b, int k) /* k = 0 is the oldest */
{
    int idx = (b->head - (b->count - 1) + k + 2 * BA_INTERP_SLOTS) % BA_INTERP_SLOTS;
    return &b->snap[idx];
}

int ba_interp_push(ba_interp *b, const ba_interp_snap *s, int64_t arrival_ns)
{
    double sample = (double)arrival_ns - (double)s->tick * TICK_NS;

    if (b->count > 0)
    {
        uint32_t newest = b->snap[b->head].tick;
        /* A tick that is off by over a second, or a huge jump forward,
         * means the server restarted: start over. */
        if (fabs(sample - b->offset_ns) > 1e9 || (s->tick > newest && s->tick - newest > 600))
            ba_interp_reset(b);
        else if (s->tick <= newest)
            return -1;  /* older or repeated (best effort can reorder) */
    }

    /* Clock offset = lowest value of (arrival - send time). The fastest
     * deliveries show the true offset. It creeps up slowly to follow real changes. */
    if (!b->have_offset)
        b->offset_ns = b->render_offset_ns = sample;
    else if (sample < b->offset_ns)
        b->offset_ns = sample;
    else
        b->offset_ns += (sample - b->offset_ns) * 0.002;
    b->have_offset = true;

    /* The drawing offset moves towards the estimate by at most 1 ms per
     * snapshot, so a correction is never seen as a jump or pause. */
    double diff = b->offset_ns - b->render_offset_ns;
    b->render_offset_ns += diff > 1e6 ? 1e6 : (diff < -1e6 ? -1e6 : diff);

    b->head = (b->count == 0) ? 0 : (b->head + 1) % BA_INTERP_SLOTS;
    b->snap[b->head] = *s;
    if (b->count < BA_INTERP_SLOTS)
        b->count++;
    return b->head;
}

static double render_tick(const ba_interp *b, int64_t now_ns)
{
    return ((double)now_ns - b->render_offset_ns - BA_INTERP_DELAY_MS * 1e6) / TICK_NS;
}

int ba_interp_slot_at(const ba_interp *b, int64_t now_ns)
{
    if (b->count == 0)
        return -1;
    double rt = render_tick(b, now_ns);
    int best = (b->head - (b->count - 1) + 2 * BA_INTERP_SLOTS) % BA_INTERP_SLOTS; /* oldest */
    for (int k = 0; k < b->count; k++)
    {
        int idx = (b->head - (b->count - 1) + k + 2 * BA_INTERP_SLOTS) % BA_INTERP_SLOTS;
        if ((double)b->snap[idx].tick <= rt)
            best = idx;
    }
    return best;
}

bool ba_interp_sample(const ba_interp *b, int i, int64_t now_ns, float *x, float *y)
{
    if (b->count == 0 || i < 0 || i >= BA_MAX_PLAYERS)
        return false;

    /* The server tick to show, slightly in the past. */
    double rt = render_tick(b, now_ns);

    const ba_interp_snap *a = at(b, 0), *c = a;
    if (rt <= (double)a->tick)
        c = a;                                  /* older than everything */
    else
    {
        for (int k = 1; k < b->count; k++)
        {
            c = at(b, k);
            if ((double)c->tick >= rt)
                break;
            a = c;
        }
    }
    /* Past the newest snapshot (a very late packet): hold the last position,
     * do not guess ahead. */
    if (rt >= (double)c->tick)
        a = c;

    if (!a->present[i] && !c->present[i])
        return false;
    if (!a->present[i] || !c->present[i])
    {
        const ba_interp_snap *s = c->present[i] ? c : a;
        *x = s->x[i];
        *y = s->y[i];
        return true;
    }

    float u = 1.0f;
    if (c->tick > a->tick)
    {
        u = (float)((rt - (double)a->tick) / (double)(c->tick - a->tick));
        u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    }
    /* Respawned at a corner: jump, do not slide across the map. */
    if (fabsf(c->x[i] - a->x[i]) > 1.5f || fabsf(c->y[i] - a->y[i]) > 1.5f)
        u = u < 0.5f ? 0.0f : 1.0f;

    *x = a->x[i] + (c->x[i] - a->x[i]) * u;
    *y = a->y[i] + (c->y[i] - a->y[i]) * u;
    return true;
}
