/* render.c - drawing for the Blast Arena client. See render.h. */
#include "render.h"

#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "stb_easy_font.h"

#define T ((float)BA_TILE_PX)

/* Drawing helpers */

#define CIRCLE_SEGS 24
/* sin/cos for circles are computed once, not every frame. */
static float g_cos[CIRCLE_SEGS + 1], g_sin[CIRCLE_SEGS + 1];

static const float PLAYER_COLORS[BA_MAX_PLAYERS][3] = {
    {0.30f, 0.65f, 1.00f},  /* 1 blue   */
    {1.00f, 0.45f, 0.45f},  /* 2 red    */
    {0.35f, 0.85f, 0.45f},  /* 3 green  */
    {1.00f, 0.85f, 0.30f},  /* 4 yellow */
    {0.80f, 0.50f, 1.00f},  /* 5 purple */
};

/* Tile (x,y) -> top-left pixel on screen. */
static float px(float x) { return BA_MAP_X + x * T; }
static float py(float y) { return BA_MAP_Y + y * T; }

/* Always set alpha. Some OpenGL drivers kept the old alpha, which made sprites see-through. */
static void rgb(float r, float g, float b) { glColor4f(r, g, b, 1.0f); }

static void rect_fill(float x, float y, float w, float h)
{
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

static void rect_line(float x, float y, float w, float h, float width)
{
    glLineWidth(width);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
    glLineWidth(1.0f);
}

static void circle_fill(float cx, float cy, float r)
{
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int i = 0; i <= CIRCLE_SEGS; i++)
        glVertex2f(cx + r * g_cos[i], cy + r * g_sin[i]);
    glEnd();
}

static float text_width(const char *s, float scale)
{
    return (float)stb_easy_font_width((char *)s) * scale;
}

static void draw_text(float x, float y, float scale, const char *s)
{
    /* static: a 32 KB buffer is too big for the stack. */
    static char buf[32768];
    int quads = stb_easy_font_print(0, 0, (char *)s, NULL, buf, sizeof buf);
    glPushMatrix();
    glTranslatef(x, y, 0.0f);
    glScalef(scale, scale, 1.0f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 16, buf);
    glDrawArrays(GL_QUADS, 0, quads * 4);
    glDisableClientState(GL_VERTEX_ARRAY);
    glPopMatrix();
}

/* Centered text on a dark band, so it is easy to read over the map. */
static void draw_banner(float cy, float scale, const char *s, float r, float g, float b)
{
    float w = text_width(s, scale), h = 8.0f * scale;
    float x = (BA_WIDTH - w) / 2.0f;
    glColor4f(0.0f, 0.0f, 0.0f, 0.55f);
    rect_fill(x - 16, cy - 10, w + 32, h + 20);
    rgb(r, g, b);
    draw_text(x, cy, scale, s);
}

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

/* Effects (client only, nothing extra goes over the network) */

#define MAX_PARTICLES 512
typedef struct { float x, y, vx, vy, life, size, r, g, b; } particle;
static particle g_parts[MAX_PARTICLES];
static float g_shake;

static uint8_t g_prev_tiles[BA_MAP_TILES];
static bool    g_have_prev_map;
static bool    g_prev_alive[BA_MAX_PLAYERS];
static float   g_last_pos[BA_MAX_PLAYERS][2];

static void burst(float x, float y, int count, float speed, float r, float g, float b)
{
    for (int i = 0; i < MAX_PARTICLES && count > 0; i++)
    {
        if (g_parts[i].life > 0.0f)
            continue;
        float a = frand() * 6.2831853f, s = speed * (0.3f + frand());
        g_parts[i] = (particle){ x, y, cosf(a) * s, sinf(a) * s, 0.4f + frand() * 0.6f,
                                 2.0f + frand() * 3.0f, r, g, b };
        count--;
    }
}

static void update_and_draw_particles(float dt)
{
    glBegin(GL_QUADS);
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
        particle *p = &g_parts[i];
        if (p->life <= 0.0f)
            continue;
        p->life -= dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->vx *= 0.94f;
        p->vy = p->vy * 0.94f + 200.0f * dt;
        float a = p->life > 1.0f ? 1.0f : p->life;
        glColor4f(p->r, p->g, p->b, a);
        float s = p->size;
        glVertex2f(p->x - s, p->y - s);
        glVertex2f(p->x + s, p->y - s);
        glVertex2f(p->x + s, p->y + s);
        glVertex2f(p->x - s, p->y + s);
    }
    glEnd();
}

/* Compare with the previous map and players to find explosions and deaths. */
static void detect_events(const ba_client_view *v)
{
    if (v->have_map)
    {
        bool boom = false;
        for (int i = 0; i < BA_MAP_TILES && g_have_prev_map; i++)
        {
            uint8_t now = v->map.tiles[i], before = g_prev_tiles[i];
            float cx = px((float)(i % BA_MAP_W)) + T / 2, cy = py((float)(i / BA_MAP_W)) + T / 2;
            if ((now & BA_TILE_FLAME) && !(before & BA_TILE_FLAME))
            {
                burst(cx, cy, 6, 140.0f, 1.0f, 0.6f, 0.1f);
                boom = true;
            }
            if ((before & 0x7F) == BA_TILE_CRATE && (now & 0x7F) != BA_TILE_CRATE)
                burst(cx, cy, 10, 160.0f, 0.55f, 0.35f, 0.15f);       /* wood chips */
        }
        if (boom)
            g_shake = g_shake > 6.0f ? g_shake : 6.0f;
        memcpy(g_prev_tiles, v->map.tiles, sizeof g_prev_tiles);
        g_have_prev_map = true;
    }
    if (v->have_state)
    {
        for (int i = 0; i < BA_MAX_PLAYERS; i++)
        {
            const Blast_PlayerState *p = &v->curr.players[i];
            if (g_prev_alive[i] && p->active && !p->alive && v->curr.phase != BA_PHASE_COUNTDOWN)
            {
                const float *c = PLAYER_COLORS[i];
                burst(px(g_last_pos[i][0]) + T / 2, py(g_last_pos[i][1]) + T / 2, 60, 260.0f,
                      c[0], c[1], c[2]);
                g_shake = 12.0f;
            }
            g_prev_alive[i] = p->active && p->alive;
        }
    }
}

/* Interpolation */

/* interp.c places snapshots on the server's timeline and draws slightly in the
   past, so players move at a steady speed (no freezing and jumping). */
static void player_pos(const ba_client_view *v, int i, int64_t now_ns, float *x, float *y)
{
    if (!ba_interp_sample(&v->interp, i, now_ns, x, y))
    {
        *x = v->curr.players[i].x;          /* not buffered yet */
        *y = v->curr.players[i].y;
    }
}

/* Scene */

static void draw_map(const ba_client_view *v, float t)
{
    for (int y = 0; y < BA_MAP_H; y++)
    {
        for (int x = 0; x < BA_MAP_W; x++)
        {
            uint8_t raw = v->have_map ? v->map.tiles[y * BA_MAP_W + x] : BA_TILE_EMPTY;
            uint8_t tile = raw & 0x7F;
            float X = px((float)x), Y = py((float)y);

            /* floor (checkerboard) */
            if ((x + y) & 1) rgb(0.17f, 0.20f, 0.24f);
            else             rgb(0.19f, 0.22f, 0.27f);
            rect_fill(X, Y, T, T);

            switch (tile)
            {
            case BA_TILE_WALL:
                rgb(0.36f, 0.40f, 0.48f);
                rect_fill(X, Y, T, T);
                rgb(0.48f, 0.53f, 0.62f);           /* highlight       */
                rect_fill(X, Y, T, 5);
                rgb(0.26f, 0.29f, 0.35f);
                rect_fill(X, Y + T - 5, T, 5);
                break;
            case BA_TILE_CRATE:
                rgb(0.62f, 0.40f, 0.20f);
                rect_fill(X + 2, Y + 2, T - 4, T - 4);
                rgb(0.45f, 0.28f, 0.13f);           /* planks */
                rect_fill(X + 2, Y + 13, T - 4, 3);
                rect_fill(X + 2, Y + 25, T - 4, 3);
                rect_line(X + 2, Y + 2, T - 4, T - 4, 2.0f);
                break;
            case BA_TILE_PU_BOMB:
            case BA_TILE_PU_FIRE:
            case BA_TILE_PU_SPEED:
            {
                float bob = 2.0f * sinf(t * 4.0f + (float)(x + y));
                const char *label = tile == BA_TILE_PU_BOMB ? "B" : tile == BA_TILE_PU_FIRE ? "F" : "S";
                if (tile == BA_TILE_PU_BOMB)      rgb(0.85f, 0.25f, 0.30f);
                else if (tile == BA_TILE_PU_FIRE) rgb(1.00f, 0.55f, 0.10f);
                else                              rgb(0.20f, 0.75f, 0.90f);
                rect_fill(X + 7, Y + 7 + bob, T - 14, T - 14);
                rgb(1.0f, 1.0f, 1.0f);
                rect_line(X + 7, Y + 7 + bob, T - 14, T - 14, 2.0f);
                draw_text(X + 14, Y + 11 + bob, 2.5f, label);
                break;
            }
            default:
                break;
            }

            if (raw & BA_TILE_FLAME)
            {
                float k = 0.75f + 0.25f * sinf(t * 40.0f + (float)(x * 3 + y));
                glColor4f(1.0f, 0.45f, 0.05f, 0.9f * k);
                rect_fill(X + 1, Y + 1, T - 2, T - 2);
                glColor4f(1.0f, 0.85f, 0.25f, k);
                rect_fill(X + 9, Y + 9, T - 18, T - 18);
            }
        }
    }
}

static void draw_bombs(const Blast_GameState *s, float t)
{
    for (int i = 0; i < BA_MAX_BOMBS; i++)
    {
        const Blast_BombState *b = &s->bombs[i];
        if (!b->active)
            continue;
        float cx = px(b->x) + T / 2, cy = py(b->y) + T / 2;
        float urgency = 1.0f - (float)b->fuse_ms / BA_FUSE_MS;      /* 0 -> 1 */
        float pulse = 1.0f + 0.12f * sinf(t * (8.0f + 30.0f * urgency));
        if (b->owner >= 1 && b->owner <= BA_MAX_PLAYERS)
        {
            const float *c = PLAYER_COLORS[b->owner - 1];
            glColor4f(c[0], c[1], c[2], 0.6f);
            circle_fill(cx, cy + 2, 16.0f * pulse);
        }
        rgb(0.08f + 0.5f * urgency * (0.5f + 0.5f * sinf(t * 30.0f)), 0.08f, 0.10f);
        circle_fill(cx, cy + 2, 13.0f * pulse);
        glColor4f(1.0f, 1.0f, 1.0f, 0.35f);
        circle_fill(cx - 4, cy - 3, 3.5f);                          /* shine */
        rgb(1.0f, 0.5f + 0.5f * frand(), 0.1f);               /* spark */
        circle_fill(cx + 9, cy - 11, 2.0f + 2.5f * frand());
    }
}

static void draw_players(const ba_client_view *v, int64_t now_ns)
{
    const Blast_GameState *s = &v->curr;
    for (int i = 0; i < BA_MAX_PLAYERS; i++)
    {
        const Blast_PlayerState *p = &s->players[i];
        if (!p->active)
            continue;
        float x, y;
        player_pos(v, i, now_ns, &x, &y);
        g_last_pos[i][0] = x;
        g_last_pos[i][1] = y;
        float X = px(x), Y = py(y);
        const float *c = PLAYER_COLORS[i];

        if (!p->alive)
        {
            /* Ghost: out, or waiting for the next round. */
            glColor4f(c[0], c[1], c[2], 0.3f);
            rect_line(X + 7, Y + 7, T - 14, T - 14, 2.0f);
            continue;
        }
        glColor4f(0.0f, 0.0f, 0.0f, 0.3f);                          /* shadow */
        rect_fill(X + 8, Y + 30, T - 16, 6);
        glColor4f(c[0], c[1], c[2], 1.0f);
        rect_fill(X + 6, Y + 5, T - 12, T - 12);
        rgb(1.0f, 1.0f, 1.0f);                                /* eyes */
        rect_fill(X + 12, Y + 12, 6, 7);
        rect_fill(X + 22, Y + 12, 6, 7);
        rgb(0.05f, 0.05f, 0.1f);
        rect_fill(X + 14, Y + 14, 3, 4);
        rect_fill(X + 24, Y + 14, 3, 4);
        if (i + 1 == v->my_id)
        {
            rgb(1.0f, 1.0f, 1.0f);
            rect_line(X + 4, Y + 3, T - 8, T - 8, 2.0f);
        }
    }

    /* Names on top */
    for (int i = 0; i < BA_MAX_PLAYERS; i++)
    {
        const Blast_PlayerState *p = &s->players[i];
        if (!p->active || !p->alive)
            continue;
        float X = px(g_last_pos[i][0]), Y = py(g_last_pos[i][1]);
        float w = text_width(p->name, 1.3f);
        glColor4f(0.0f, 0.0f, 0.0f, 0.5f);
        rect_fill(X + (T - w) / 2 - 2, Y - 11, w + 4, 11);
        rgb(1.0f, 1.0f, 1.0f);
        draw_text(X + (T - w) / 2, Y - 10, 1.3f, p->name);
    }
}

static void draw_hud(const ba_client_view *v)
{
    const Blast_GameState *s = &v->curr;
    char line[64];

    rgb(0.08f, 0.09f, 0.12f);
    rect_fill(0, 0, BA_WIDTH, BA_HUD_H);

    float x = 12.0f;
    if (v->my_id > 0 && v->have_state)
    {
        const Blast_PlayerState *me = &s->players[v->my_id - 1];
        const float *c = PLAYER_COLORS[v->my_id - 1];
        rgb(c[0], c[1], c[2]);
        snprintf(line, sizeof line, "P%d %s", v->my_id, me->name);
        draw_text(x, 12, 2.0f, line);
        x += text_width(line, 2.0f) + 18.0f;
        /* power-up stats */
        const struct { const char *fmt; int val; float r, g, b; } st[3] = {
            {"BOMB %d", me->bombs, 0.95f, 0.40f, 0.45f},
            {"FIRE %d", me->range, 1.00f, 0.60f, 0.20f},
            {"SPD %d",  me->speed, 0.30f, 0.80f, 0.95f},
        };
        for (int i = 0; i < 3; i++)
        {
            snprintf(line, sizeof line, st[i].fmt, st[i].val);
            rgb(st[i].r, st[i].g, st[i].b);
            draw_text(x, 15, 1.6f, line);
            x += text_width(line, 1.6f) + 12.0f;
        }
    }
    else
    {
        rgb(0.9f, 0.9f, 0.9f);
        draw_text(x, 12, 2.0f, v->my_id == 0 ? "SERVER FULL - retrying" : "Connecting...");
    }

    /* Round timer */
    if (v->have_state && s->phase == BA_PHASE_PLAYING)
    {
        int secs = (int)(s->phase_ms / 1000);
        snprintf(line, sizeof line, "%d:%02d", secs / 60, secs % 60);
        rgb(secs < 30 ? 1.0f : 0.9f, secs < 30 ? 0.4f : 0.9f, secs < 30 ? 0.3f : 0.9f);
        draw_text(BA_WIDTH / 2.0f + 40, 12, 2.0f, line);
    }

    /* Scoreboard, right */
    float sx = BA_WIDTH - 12.0f;
    for (int i = BA_MAX_PLAYERS - 1; i >= 0 && v->have_state; i--)
    {
        if (!s->players[i].active)
            continue;
        snprintf(line, sizeof line, "%s %d", s->players[i].name, (int)s->players[i].score);
        float w = text_width(line, 1.5f);
        sx -= w;
        const float *c = PLAYER_COLORS[i];
        glColor4f(c[0], c[1], c[2], s->players[i].alive ? 1.0f : 0.4f);
        draw_text(sx, 16, 1.5f, line);
        sx -= 14.0f;
    }

    if (!v->have_state)
        return;

    int active = 0;
    for (int i = 0; i < BA_MAX_PLAYERS; i++)
        active += s->players[i].active;

    switch (s->phase)
    {
    case BA_PHASE_LOBBY:
        snprintf(line, sizeof line, "Waiting for players %d/%d", active, BA_MIN_PLAYERS);
        draw_banner(250, 3.0f, line, 0.95f, 0.95f, 0.95f);
        draw_banner(300, 2.0f, "Move: arrows / WASD    Bomb: SPACE", 0.75f, 0.8f, 0.9f);
        break;
    case BA_PHASE_COUNTDOWN:
        snprintf(line, sizeof line, "%d", (int)(s->phase_ms / 1000) + 1);
        draw_banner(230, 9.0f, line, 1.0f, 0.8f, 0.3f);
        draw_banner(330, 2.0f, "Blow up crates, grab power-ups, be the last one standing", 0.95f, 0.95f, 0.95f);
        break;
    case BA_PHASE_PLAYING:
        if (v->my_id > 0 && !s->players[v->my_id - 1].alive)
            draw_banner(300, 2.0f, "You're out - spectating", 0.8f, 0.8f, 0.8f);
        break;
    case BA_PHASE_ROUND_OVER:
        if (s->winner >= 1 && s->winner <= BA_MAX_PLAYERS)
        {
            const float *c = PLAYER_COLORS[s->winner - 1];
            snprintf(line, sizeof line, "%s WINS!", s->players[s->winner - 1].name);
            draw_banner(250, 5.0f, line, c[0], c[1], c[2]);
        }
        else
            draw_banner(250, 5.0f, "DRAW!", 0.95f, 0.95f, 0.95f);
        draw_banner(320, 2.0f, "Next round soon...", 0.8f, 0.8f, 0.9f);
        break;
    }
}

/* Public API */

void render_init(void)
{
    for (int i = 0; i <= CIRCLE_SEGS; i++)
    {
        float a = (float)i / CIRCLE_SEGS * 6.2831853f;
        g_cos[i] = cosf(a);
        g_sin[i] = sinf(a);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void render_frame(const ba_client_view *live, int64_t now_ns, float dt_s)
{
    /* Draw everything from one slightly delayed moment. */
    static ba_client_view shown;
    client_view_at(live, now_ns, &shown);
    const ba_client_view *v = &shown;

    float t = (float)(now_ns % 1000000000000LL) / 1e9f;

    detect_events(v);

    glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glPushMatrix();
    if (g_shake > 0.1f)
    {
        glTranslatef((frand() - 0.5f) * g_shake, (frand() - 0.5f) * g_shake, 0.0f);
        g_shake *= expf(-8.0f * dt_s);
    }
    draw_map(v, t);
    if (v->have_state)
    {
        draw_bombs(&v->curr, t);
        draw_players(v, now_ns);
    }
    update_and_draw_particles(dt_s);
    glPopMatrix();

    draw_hud(v);
}
