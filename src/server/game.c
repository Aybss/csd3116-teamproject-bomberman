/* game.c - Blast Arena rules. See game.h.
 *
 * Round flow (the server decides everything):
 *   LOBBY -> COUNTDOWN (3 s) -> PLAYING -> ROUND_OVER -> back to LOBBY or COUNTDOWN
 *
 * While PLAYING, players walk from tile to tile and drop bombs. A bomb
 * explodes after 2.5 s in a cross of flames. Flames stop at walls, destroy
 * the first crate they hit, set off other bombs, and kill players.
 */
#include "game.h"

#include <math.h>
#include <string.h>

/* Start tiles, one per slot. The first two are far apart. */
static const int SPAWN[BA_MAX_PLAYERS][2] = {
    {1, 1}, {BA_MAP_W - 2, BA_MAP_H - 2}, {BA_MAP_W - 2, 1}, {1, BA_MAP_H - 2},
    {BA_MAP_W / 2, BA_MAP_H / 2},
};

static const int DIRS[4][2] = { {0, -1}, {0, 1}, {-1, 0}, {1, 0} }; /* up down left right */
static const uint8_t DIR_KEYS[4] = { BA_KEY_UP, BA_KEY_DOWN, BA_KEY_LEFT, BA_KEY_RIGHT };

/* Helpers */

static bool valid_id(int id) { return id >= 1 && id <= BA_MAX_PLAYERS; }
static ba_player *player(ba_game *g, int id) { return &g->players[id - 1]; }
static int idx(int x, int y) { return y * BA_MAP_W + x; }
static bool in_map(int x, int y) { return x >= 0 && y >= 0 && x < BA_MAP_W && y < BA_MAP_H; }

uint8_t game_tile(const ba_game *g, int x, int y)
{
    return in_map(x, y) ? g->tiles[idx(x, y)] : BA_TILE_WALL;
}

static void set_tile(ba_game *g, int x, int y, uint8_t t)
{
    if (g->tiles[idx(x, y)] != t)
    {
        g->tiles[idx(x, y)] = t;
        g->map_dirty = true;
    }
}

static ba_bomb *bomb_at(ba_game *g, int x, int y)
{
    for (int i = 0; i < BA_MAX_BOMBS; i++)
        if (g->bombs[i].active && g->bombs[i].x == x && g->bombs[i].y == y)
            return &g->bombs[i];
    return NULL;
}

/* Can a player walk onto this tile? */
static bool walkable(ba_game *g, int x, int y)
{
    uint8_t t = game_tile(g, x, y);
    return t != BA_TILE_WALL && t != BA_TILE_CRATE && bomb_at(g, x, y) == NULL;
}

/* The tile a player is on (the nearer one while moving). */
static void current_tile(const ba_player *p, int *x, int *y)
{
    *x = (int)lroundf(p->x);
    *y = (int)lroundf(p->y);
}

int game_count_alive(const ba_game *g)
{
    int n = 0;
    for (int i = 0; i < BA_MAX_PLAYERS; i++)
        n += g->players[i].active && g->players[i].alive;
    return n;
}

void game_spawn_point(int id, int *x, int *y)
{
    *x = SPAWN[id - 1][0];
    *y = SPAWN[id - 1][1];
}

/* Map and round setup */

static void generate_map(ba_game *g)
{
    memset(g->flame_ms, 0, sizeof g->flame_ms);
    memset(g->hidden, 0, sizeof g->hidden);
    memset(g->reveal, 0, sizeof g->reveal);
    memset(g->bombs, 0, sizeof g->bombs);

    for (int y = 0; y < BA_MAP_H; y++)
    {
        for (int x = 0; x < BA_MAP_W; x++)
        {
            uint8_t t = BA_TILE_EMPTY;
            bool border = x == 0 || y == 0 || x == BA_MAP_W - 1 || y == BA_MAP_H - 1;
            if (border || (x % 2 == 0 && y % 2 == 0))
                t = BA_TILE_WALL;                         /* pillar grid         */
            else if (ba_rng_range(&g->rng, 1, 100) <= BA_CRATE_PERCENT)
            {
                t = BA_TILE_CRATE;
                if (ba_rng_range(&g->rng, 1, 100) <= BA_POWERUP_PERCENT)
                    g->hidden[idx(x, y)] = (uint8_t)ba_rng_range(&g->rng,
                                                    BA_TILE_PU_BOMB, BA_TILE_PU_SPEED);
            }
            g->tiles[idx(x, y)] = t;
        }
    }

    /* Clear the start tiles and their neighbours, so nobody is boxed in. */
    for (int s = 0; s < BA_MAX_PLAYERS; s++)
    {
        int sx = SPAWN[s][0], sy = SPAWN[s][1];
        for (int d = -1; d < 4; d++)
        {
            int x = sx + (d < 0 ? 0 : DIRS[d][0]);
            int y = sy + (d < 0 ? 0 : DIRS[d][1]);
            if (game_tile(g, x, y) == BA_TILE_CRATE)
            {
                g->tiles[idx(x, y)] = BA_TILE_EMPTY;
                g->hidden[idx(x, y)] = 0;
            }
        }
    }
    g->map_dirty = true;
}

static void reset_player_for_round(ba_game *g, int id, bool alive)
{
    ba_player *p = player(g, id);
    int sx, sy;
    game_spawn_point(id, &sx, &sy);
    p->x = (float)sx;
    p->y = (float)sy;
    p->fx = p->tx = sx;
    p->fy = p->ty = sy;
    p->moving = false;
    p->alive = alive;
    p->bombs_max = BA_START_BOMBS;
    p->range = BA_START_RANGE;
    p->speed_ups = 0;
}

static void enter_lobby(ba_game *g)
{
    g->phase = BA_PHASE_LOBBY;
    generate_map(g);
    /* In the lobby everyone can walk around, but not drop bombs. */
    for (int i = 0; i < BA_MAX_PLAYERS; i++)
        if (g->players[i].active)
            reset_player_for_round(g, i + 1, true);
}

static void enter_countdown(ba_game *g)
{
    g->phase = BA_PHASE_COUNTDOWN;
    g->phase_ms = BA_COUNTDOWN_MS;
    generate_map(g);
    for (int i = 0; i < BA_MAX_PLAYERS; i++)
        if (g->players[i].active)
            reset_player_for_round(g, i + 1, true);
}

static void enter_round_over(ba_game *g)
{
    g->phase = BA_PHASE_ROUND_OVER;
    g->phase_ms = BA_ROUND_OVER_MS;
    g->winner = 0;
    if (game_count_alive(g) == 1)
    {
        for (int i = 0; i < BA_MAX_PLAYERS; i++)
        {
            if (g->players[i].active && g->players[i].alive)
            {
                g->winner = i + 1;
                g->players[i].score++;
            }
        }
    }
}

/* Join, leave, input */

void game_init(ba_game *g, uint64_t seed)
{
    memset(g, 0, sizeof *g);
    ba_rng_seed(&g->rng, seed);
    enter_lobby(g);
}

int game_join(ba_game *g, uint64_t nonce, uint64_t owner,
              const char *name, int64_t now_ms)
{
    /* Same nonce = a retry. Give the same slot. */
    for (int i = 0; i < BA_MAX_PLAYERS; i++)
    {
        if (g->players[i].active && g->players[i].nonce == nonce)
        {
            g->players[i].last_seen_ms = now_ms;
            return i + 1;
        }
    }

    for (int i = 0; i < BA_MAX_PLAYERS; i++)
    {
        ba_player *p = &g->players[i];
        if (p->active)
            continue;

        memset(p, 0, sizeof *p);
        p->active = true;
        p->nonce = nonce;
        p->owner = owner;
        p->last_seen_ms = now_ms;

        /* Never trust a name from the network. */
        size_t n = 0;
        if (name)
            for (; n < BA_NAME_LEN - 1 && name[n]; n++)
                p->name[n] = (name[n] >= 32 && name[n] < 127) ? name[n] : '?';
        p->name[n] = '\0';
        if (n == 0)
            snprintf(p->name, sizeof p->name, "P%d", i + 1);

        /* Joined mid-round: watch as a ghost until the next round. */
        bool alive = g->phase == BA_PHASE_LOBBY || g->phase == BA_PHASE_COUNTDOWN;
        reset_player_for_round(g, i + 1, alive);
        g->num_active++;
        return i + 1;
    }
    return 0; /* full */
}

void game_leave(ba_game *g, int id)
{
    if (!valid_id(id) || !player(g, id)->active)
        return;
    memset(player(g, id), 0, sizeof(ba_player));
    g->num_active--;
    /* Their bombs stay and still explode. Too few players is handled next tick. */
}

bool game_place_bomb(ba_game *g, int id)
{
    ba_player *p = player(g, id);
    if (g->phase != BA_PHASE_PLAYING || !p->active || !p->alive)
        return false;

    int x, y, mine = 0;
    current_tile(p, &x, &y);
    if (bomb_at(g, x, y))
        return false;
    for (int i = 0; i < BA_MAX_BOMBS; i++)
        mine += g->bombs[i].active && g->bombs[i].owner == id;
    if (mine >= p->bombs_max)
        return false;

    for (int i = 0; i < BA_MAX_BOMBS; i++)
    {
        if (!g->bombs[i].active)
        {
            g->bombs[i] = (ba_bomb){ true, x, y, BA_FUSE_MS, id, p->range };
            return true;
        }
    }
    return false;
}

bool game_set_input(ba_game *g, int id, uint8_t keys, uint8_t bomb_seq, int64_t now_ms)
{
    /* Check every field before using it as an array index. */
    if (!valid_id(id) || !player(g, id)->active || (keys & ~BA_KEY_MASK))
        return false;
    ba_player *p = player(g, id);
    p->keys = keys;
    p->last_seen_ms = now_ms;
    /* bomb_seq is a counter, not a flag. A quick tap is never lost,
     * and a repeated message never drops a second bomb. */
    if (bomb_seq != p->bomb_seq)
    {
        p->bomb_seq = bomb_seq;
        game_place_bomb(g, id);
    }
    return true;
}

/* Movement */

static void pick_up(ba_game *g, ba_player *p, int x, int y)
{
    switch (game_tile(g, x, y))
    {
    case BA_TILE_PU_BOMB:
        if (p->bombs_max < BA_MAX_BOMBS_PER) p->bombs_max++;
        break;
    case BA_TILE_PU_FIRE:
        if (p->range < BA_MAX_RANGE) p->range++;
        break;
    case BA_TILE_PU_SPEED:
        if (p->speed_ups < BA_MAX_SPEED_UPS) p->speed_ups++;
        break;
    default:
        return;
    }
    set_tile(g, x, y, BA_TILE_EMPTY);
}

static void move_player(ba_game *g, ba_player *p, float dt_s)
{
    float budget = (BA_BASE_SPEED + BA_SPEED_STEP * p->speed_ups) * dt_s;

    /* Loop so leftover movement carries into the next tile (no pause). */
    for (int iter = 0; iter < 3 && budget > 0.0f; iter++)
    {
        if (!p->moving)
        {
            int cx = p->tx, cy = p->ty;   /* exactly on this tile        */
            int dir = -1;
            for (int d = 0; d < 4; d++)
            {
                if ((p->keys & DIR_KEYS[d]) && walkable(g, cx + DIRS[d][0], cy + DIRS[d][1]))
                {
                    dir = d;
                    break;
                }
            }
            if (dir < 0)
                return;
            p->fx = cx;
            p->fy = cy;
            p->tx = cx + DIRS[dir][0];
            p->ty = cy + DIRS[dir][1];
            p->moving = true;
        }
        else
        {
            /* The opposite key turns around mid-step. */
            int dx = p->tx - p->fx, dy = p->ty - p->fy;
            for (int d = 0; d < 4; d++)
            {
                if ((p->keys & DIR_KEYS[d]) && DIRS[d][0] == -dx && DIRS[d][1] == -dy &&
                    walkable(g, p->fx, p->fy))
                {
                    int ox = p->fx, oy = p->fy;
                    p->fx = p->tx; p->fy = p->ty;
                    p->tx = ox;    p->ty = oy;
                    break;
                }
            }
        }

        float rx = (float)p->tx - p->x, ry = (float)p->ty - p->y;
        float dist = fabsf(rx) + fabsf(ry);           /* moving on one axis */
        if (dist <= budget)
        {
            p->x = (float)p->tx;
            p->y = (float)p->ty;
            p->moving = false;
            budget -= dist;
            pick_up(g, p, p->tx, p->ty);
        }
        else
        {
            p->x += (rx > 0 ? 1.0f : rx < 0 ? -1.0f : 0.0f) * budget;
            p->y += (ry > 0 ? 1.0f : ry < 0 ? -1.0f : 0.0f) * budget;
            budget = 0.0f;
        }
    }
}

/* Bombs and flames */

static void ignite(ba_game *g, int x, int y)
{
    g->flame_ms[idx(x, y)] = BA_FLAME_MS;
    g->map_dirty = true;
}

static void explode(ba_game *g, ba_bomb *b)
{
    b->active = false;
    ignite(g, b->x, b->y);
    for (int d = 0; d < 4; d++)
    {
        for (int r = 1; r <= b->range; r++)
        {
            int x = b->x + DIRS[d][0] * r, y = b->y + DIRS[d][1] * r;
            uint8_t t = game_tile(g, x, y);
            if (t == BA_TILE_WALL)
                break;                                /* walls stop flames */
            ignite(g, x, y);
            ba_bomb *other = bomb_at(g, x, y);
            if (other)
            {
                other->fuse_ms = 0.0f;                /* chain reaction */
                break;
            }
            if (t == BA_TILE_CRATE)
            {
                /* The crate breaks. Its power-up appears when the flame ends. */
                set_tile(g, x, y, BA_TILE_EMPTY);
                g->reveal[idx(x, y)] = g->hidden[idx(x, y)];
                g->hidden[idx(x, y)] = 0;
                break;
            }
            if (t != BA_TILE_EMPTY)
            {
                set_tile(g, x, y, BA_TILE_EMPTY);     /* power-ups burn */
                break;
            }
        }
    }
}

static void update_bombs(ba_game *g, float dt_ms)
{
    for (int i = 0; i < BA_MAX_BOMBS; i++)
    {
        ba_bomb *b = &g->bombs[i];
        if (!b->active)
            continue;
        b->fuse_ms -= dt_ms;
        if (g->flame_ms[idx(b->x, b->y)] > 0.0f)
            b->fuse_ms = 0.0f;                        /* dropped into a flame */
    }
    /* Repeat until nothing more explodes (one blast can set off earlier bombs). */
    bool again = true;
    while (again)
    {
        again = false;
        for (int i = 0; i < BA_MAX_BOMBS; i++)
        {
            if (g->bombs[i].active && g->bombs[i].fuse_ms <= 0.0f)
            {
                explode(g, &g->bombs[i]);
                again = true;
            }
        }
    }
}

static void update_flames(ba_game *g, float dt_ms)
{
    for (int i = 0; i < BA_MAP_TILES; i++)
    {
        if (g->flame_ms[i] <= 0.0f)
            continue;
        g->flame_ms[i] -= dt_ms;
        if (g->flame_ms[i] <= 0.0f)
        {
            g->flame_ms[i] = 0.0f;
            g->map_dirty = true;
            if (g->reveal[i])
            {
                g->tiles[i] = g->reveal[i];
                g->reveal[i] = 0;
            }
        }
    }
}

static void burn_players(ba_game *g)
{
    for (int i = 0; i < BA_MAX_PLAYERS; i++)
    {
        ba_player *p = &g->players[i];
        if (!p->active || !p->alive)
            continue;
        int x, y;
        current_tile(p, &x, &y);
        if (g->flame_ms[idx(x, y)] > 0.0f)
        {
            p->alive = false;
            BA_LOG("BOOM! Player %d is out", i + 1);
        }
    }
}

/* Tick */

void game_tick(ba_game *g, float dt_ms, int64_t now_ms)
{
    g->tick++;

    /* 1. Drop clients that crashed or lost the network. */
    for (int i = 0; i < BA_MAX_PLAYERS; i++)
    {
        if (g->players[i].active &&
            now_ms - g->players[i].last_seen_ms > BA_PLAYER_TIMEOUT_MS)
        {
            BA_LOG("Player %d timed out", i + 1);
            game_leave(g, i + 1);
        }
    }

    float dt_s = dt_ms / 1000.0f;

    /* 2. Phase logic. */
    switch (g->phase)
    {
    case BA_PHASE_LOBBY:
        if (g->num_active >= BA_MIN_PLAYERS)
        {
            enter_countdown(g);
            return;
        }
        break;

    case BA_PHASE_COUNTDOWN:
        g->phase_ms -= dt_ms;
        if (g->num_active < BA_MIN_PLAYERS)
            enter_lobby(g);
        else if (g->phase_ms <= 0.0f)
        {
            g->phase = BA_PHASE_PLAYING;
            g->phase_ms = BA_ROUND_MS;
        }
        return; /* nobody moves during the countdown */

    case BA_PHASE_PLAYING:
        g->phase_ms -= dt_ms;
        update_bombs(g, dt_ms);
        update_flames(g, dt_ms);
        burn_players(g);
        if (game_count_alive(g) <= 1 || g->phase_ms <= 0.0f)
        {
            enter_round_over(g);
            return;
        }
        break;

    case BA_PHASE_ROUND_OVER:
        g->phase_ms -= dt_ms;
        update_flames(g, dt_ms);    /* let the last flames die down */
        if (g->phase_ms <= 0.0f)
        {
            if (g->num_active >= BA_MIN_PLAYERS)
                enter_countdown(g);
            else
                enter_lobby(g);
        }
        return;
    }

    /* 3. Movement (lobby and playing), then flames again for anyone who walked into one. */
    for (int i = 0; i < BA_MAX_PLAYERS; i++)
        if (g->players[i].active && g->players[i].alive)
            move_player(g, &g->players[i], dt_s);
    if (g->phase == BA_PHASE_PLAYING)
        burn_players(g);
}

bool game_map_changed(ba_game *g)
{
    if (!g->map_dirty)
        return false;
    g->map_dirty = false;
    g->map_version++;
    return true;
}

/* Snapshots */

void game_snapshot(const ba_game *g, Blast_GameState *out)
{
    out->tick = g->tick;
    out->phase = (uint8_t)g->phase;
    out->phase_ms = (int32_t)(g->phase_ms > 0.0f ? g->phase_ms : 0.0f);
    out->winner = g->winner;
    out->map_version = g->map_version;
    for (int i = 0; i < BA_MAX_PLAYERS; i++)
    {
        const ba_player *p = &g->players[i];
        Blast_PlayerState *s = &out->players[i];
        s->active = p->active;
        s->alive = p->alive;
        s->x = p->x;
        s->y = p->y;
        s->score = p->score;
        s->bombs = (uint8_t)p->bombs_max;
        s->range = (uint8_t)p->range;
        s->speed = (uint8_t)p->speed_ups;
        memcpy(s->name, p->name, sizeof s->name);
    }
    for (int i = 0; i < BA_MAX_BOMBS; i++)
    {
        const ba_bomb *b = &g->bombs[i];
        Blast_BombState *s = &out->bombs[i];
        s->active = b->active;
        s->x = (uint8_t)b->x;
        s->y = (uint8_t)b->y;
        s->owner = (uint8_t)b->owner;
        s->fuse_ms = (int16_t)(b->fuse_ms > 0.0f ? b->fuse_ms : 0.0f);
    }
}

void game_map_snapshot(const ba_game *g, Blast_MapState *out)
{
    out->version = g->map_version;
    for (int i = 0; i < BA_MAP_TILES; i++)
        out->tiles[i] = (uint8_t)(g->tiles[i] | (g->flame_ms[i] > 0.0f ? BA_TILE_FLAME : 0));
}
