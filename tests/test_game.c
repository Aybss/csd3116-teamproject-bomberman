/* test_game.c - unit tests for the game rules (src/server/game.c).
 * Run with: ctest --test-dir build  (or ./build/test_game) */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "game.h"

static int g_failed, g_checks;

#define CHECK(cond)                                                      \
    do {                                                                 \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            g_failed++;                                                  \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                \
    } while (0)

#define TICK_MS (1000.0f / BA_TICK_HZ)

static int64_t g_now;

/* Run n ticks. Inputs are refreshed each tick, so nobody times out. */
static void run(ba_game *g, int n, bool keep_alive)
{
    for (int i = 0; i < n; i++)
    {
        g_now += (int64_t)TICK_MS;
        if (keep_alive)
            for (int p = 0; p < BA_MAX_PLAYERS; p++)
                if (g->players[p].active)
                    g->players[p].last_seen_ms = g_now;
        game_tick(g, TICK_MS, g_now);
    }
}

static void run_ms(ba_game *g, int ms) { run(g, (int)(ms / TICK_MS) + 1, true); }

/* Two players, straight into PLAYING, on a map with no crates. */
static void setup_playing(ba_game *g)
{
    g_now = 0;
    game_init(g, 12345);
    game_join(g, 1, 0, "a", 0);
    game_join(g, 2, 0, "b", 0);
    run_ms(g, BA_COUNTDOWN_MS + 100);
    for (int i = 0; i < BA_MAP_TILES; i++)
        if (g->tiles[i] != BA_TILE_WALL)
            g->tiles[i] = BA_TILE_EMPTY;
    memset(g->hidden, 0, sizeof g->hidden);
}

static void put(ba_game *g, int id, int x, int y)
{
    ba_player *p = &g->players[id - 1];
    p->x = (float)x; p->y = (float)y;
    p->fx = p->tx = x; p->fy = p->ty = y;
    p->moving = false;
}

/* Tests */

static void test_join_and_full(void)
{
    puts("join assigns ids, refuses when full, is idempotent");
    ba_game g;
    game_init(&g, 1);
    for (int i = 0; i < BA_MAX_PLAYERS; i++)
        CHECK(game_join(&g, 100 + i, 0, "p", 0) == i + 1);
    CHECK(game_join(&g, 999, 0, "late", 0) == 0);
    CHECK(game_join(&g, 102, 0, "p", 0) == 3);
    game_leave(&g, 2);
    CHECK(g.num_active == BA_MAX_PLAYERS - 1);
    CHECK(game_join(&g, 999, 0, "late", 0) == 2);
}

static void test_names_are_sanitised(void)
{
    puts("names are truncated and made printable");
    ba_game g;
    game_init(&g, 1);
    int id = game_join(&g, 1, 0, "abcdefghijklmnopqrstuvwxyz", 0);
    CHECK(strlen(g.players[id - 1].name) == BA_NAME_LEN - 1);
    id = game_join(&g, 2, 0, "a\x01" "b", 0);
    CHECK(strcmp(g.players[id - 1].name, "a?b") == 0);
    id = game_join(&g, 3, 0, "", 0);
    CHECK(strcmp(g.players[id - 1].name, "P3") == 0);
}

static void test_input_validation(void)
{
    puts("bad inputs are rejected");
    ba_game g;
    game_init(&g, 1);
    int id = game_join(&g, 1, 0, "a", 0);
    CHECK(game_set_input(&g, id, BA_KEY_LEFT, 0, 0));
    CHECK(!game_set_input(&g, 0, BA_KEY_LEFT, 0, 0));
    CHECK(!game_set_input(&g, 6, BA_KEY_LEFT, 0, 0));
    CHECK(!game_set_input(&g, -100000, BA_KEY_LEFT, 0, 0));
    CHECK(!game_set_input(&g, 2, BA_KEY_LEFT, 0, 0));       /* empty slot */
    CHECK(!game_set_input(&g, id, 0x80, 0, 0));             /* unknown bit */
}

static void test_map_generation(void)
{
    puts("map: border, pillar grid, clear spawns, seeded");
    ba_game g, h;
    game_init(&g, 77);
    game_init(&h, 77);
    CHECK(memcmp(g.tiles, h.tiles, sizeof g.tiles) == 0);   /* same seed = same map */
    for (int x = 0; x < BA_MAP_W; x++)
        CHECK(game_tile(&g, x, 0) == BA_TILE_WALL && game_tile(&g, x, BA_MAP_H - 1) == BA_TILE_WALL);
    CHECK(game_tile(&g, 2, 2) == BA_TILE_WALL && game_tile(&g, 4, 6) == BA_TILE_WALL);
    CHECK(game_tile(&g, -1, 3) == BA_TILE_WALL);            /* outside = wall */
    int crates = 0;
    for (int i = 0; i < BA_MAP_TILES; i++)
        crates += g.tiles[i] == BA_TILE_CRATE;
    CHECK(crates > 50);
    for (int id = 1; id <= BA_MAX_PLAYERS; id++)
    {
        int sx, sy;
        game_spawn_point(id, &sx, &sy);
        CHECK(game_tile(&g, sx, sy) == BA_TILE_EMPTY);
        int free_n = 0;
        free_n += game_tile(&g, sx + 1, sy) == BA_TILE_EMPTY;
        free_n += game_tile(&g, sx - 1, sy) == BA_TILE_EMPTY;
        free_n += game_tile(&g, sx, sy + 1) == BA_TILE_EMPTY;
        free_n += game_tile(&g, sx, sy - 1) == BA_TILE_EMPTY;
        CHECK(free_n >= 2);                                 /* never boxed in */
    }
}

static void test_movement(void)
{
    puts("movement: speed, walls, crates, turning back");
    ba_game g;
    setup_playing(&g);
    put(&g, 1, 1, 1);
    ba_player *p = &g.players[0];
    game_set_input(&g, 1, BA_KEY_RIGHT, 0, g_now);
    run_ms(&g, 1000);
    CHECK(fabsf(p->x - (1 + BA_BASE_SPEED)) < 0.15f && p->y == 1.0f);

    put(&g, 1, 1, 1);
    game_set_input(&g, 1, BA_KEY_UP, 0, g_now);             /* border above */
    run_ms(&g, 500);
    CHECK(p->x == 1.0f && p->y == 1.0f);

    g.tiles[1 * BA_MAP_W + 2] = BA_TILE_CRATE;              /* crate to the right */
    game_set_input(&g, 1, BA_KEY_RIGHT, 0, g_now);
    run_ms(&g, 500);
    CHECK(p->x == 1.0f);

    g.tiles[1 * BA_MAP_W + 2] = BA_TILE_EMPTY;
    run(&g, 5, true);                                        /* now moving right */
    CHECK(p->x > 1.0f && p->x < 2.0f);
    game_set_input(&g, 1, BA_KEY_LEFT, 0, g_now);           /* turn back mid-step */
    run_ms(&g, 500);
    CHECK(p->x == 1.0f);
}

static void test_bomb_placement(void)
{
    puts("bombs: counter semantics, limit, only while playing");
    ba_game g;
    g_now = 0;
    game_init(&g, 3);
    game_join(&g, 1, 0, "a", 0);
    CHECK(!game_place_bomb(&g, 1));                          /* lobby: no bombs */

    setup_playing(&g);
    put(&g, 1, 1, 1);
    game_set_input(&g, 1, 0, 1, g_now);                      /* press #1 */
    int n = 0;
    for (int i = 0; i < BA_MAX_BOMBS; i++) n += g.bombs[i].active;
    CHECK(n == 1);
    game_set_input(&g, 1, 0, 1, g_now);                      /* same counter: no new bomb */
    put(&g, 1, 3, 1);
    game_set_input(&g, 1, 0, 2, g_now);                      /* press #2: limit is 1 */
    n = 0;
    for (int i = 0; i < BA_MAX_BOMBS; i++) n += g.bombs[i].active;
    CHECK(n == 1);
    g.players[0].bombs_max = 2;
    game_set_input(&g, 1, 0, 3, g_now);
    n = 0;
    for (int i = 0; i < BA_MAX_BOMBS; i++) n += g.bombs[i].active;
    CHECK(n == 2);
}

static void test_explosion(void)
{
    puts("explosion: cross shape, walls, crates, power-up reveal, death");
    ba_game g;
    setup_playing(&g);
    put(&g, 1, 3, 1);
    put(&g, 2, 9, 5);                                        /* far away */
    g.tiles[1 * BA_MAP_W + 4] = BA_TILE_CRATE;               /* right of the bomb */
    g.hidden[1 * BA_MAP_W + 4] = BA_TILE_PU_FIRE;
    g.tiles[1 * BA_MAP_W + 5] = BA_TILE_CRATE;               /* behind it: must survive */
    CHECK(game_place_bomb(&g, 1));
    put(&g, 1, 1, 3);                                        /* step out of the way */
    run_ms(&g, BA_FUSE_MS - 100);
    CHECK(g.flame_ms[1 * BA_MAP_W + 3] == 0.0f);             /* not yet */
    run_ms(&g, 150);
    CHECK(g.flame_ms[1 * BA_MAP_W + 3] > 0.0f);              /* centre */
    CHECK(g.flame_ms[1 * BA_MAP_W + 1] > 0.0f);              /* 2 left */
    CHECK(g.flame_ms[3 * BA_MAP_W + 3] > 0.0f);              /* 2 down */
    CHECK(g.flame_ms[0 * BA_MAP_W + 3] == 0.0f);             /* wall above */
    CHECK(g.flame_ms[1 * BA_MAP_W + 4] > 0.0f);              /* crate burnt */
    CHECK(g.flame_ms[1 * BA_MAP_W + 5] == 0.0f);             /* stopped by crate */
    CHECK(game_tile(&g, 5, 1) == BA_TILE_CRATE);
    CHECK(game_tile(&g, 4, 1) == BA_TILE_EMPTY);             /* power-up not shown yet */
    CHECK(g.players[0].alive);                               /* (1,3) is safe */
    run_ms(&g, BA_FLAME_MS + 50);
    CHECK(game_tile(&g, 4, 1) == BA_TILE_PU_FIRE);           /* revealed after the flame */

    /* walk onto the power-up */
    put(&g, 1, 3, 1);
    game_set_input(&g, 1, BA_KEY_RIGHT, 0, g_now);
    run_ms(&g, 400);
    CHECK(g.players[0].range == BA_START_RANGE + 1);
    CHECK(game_tile(&g, 4, 1) == BA_TILE_EMPTY);
}

static void test_chain_and_winner(void)
{
    puts("chain reaction, death and round winner");
    ba_game g;
    setup_playing(&g);
    put(&g, 1, 1, 1);
    put(&g, 2, 7, 1);
    g.players[0].bombs_max = 2;
    CHECK(game_place_bomb(&g, 1));                           /* bomb A at (1,1) */
    put(&g, 1, 3, 1);
    run_ms(&g, 1000);
    CHECK(game_place_bomb(&g, 1));                           /* bomb B at (3,1), later fuse */
    put(&g, 1, 1, 5);                                        /* safe spot */
    g.players[1].range = 2;
    put(&g, 2, 5, 1);                                        /* in B's range only */
    run_ms(&g, BA_FUSE_MS - 1000 + 100);                     /* A explodes, sets off B */
    bool any = false;
    for (int i = 0; i < BA_MAX_BOMBS; i++) any |= g.bombs[i].active;
    CHECK(!any);
    CHECK(!g.players[1].alive);                              /* killed by the chained bomb */
    CHECK(g.players[0].alive);
    CHECK(g.phase == BA_PHASE_ROUND_OVER);
    CHECK(g.winner == 1 && g.players[0].score == 1);

    run_ms(&g, BA_ROUND_OVER_MS + 100);
    CHECK(g.phase == BA_PHASE_COUNTDOWN);                    /* next round */
    CHECK(g.players[0].alive && g.players[1].alive);
    CHECK(g.players[0].range == BA_START_RANGE);             /* power-ups reset */
}

static void test_draw(void)
{
    puts("both players caught = draw");
    ba_game g;
    setup_playing(&g);
    put(&g, 1, 3, 1);
    put(&g, 2, 3, 1);
    CHECK(game_place_bomb(&g, 1));
    run_ms(&g, BA_FUSE_MS + 50);
    CHECK(g.phase == BA_PHASE_ROUND_OVER && g.winner == 0);
    CHECK(g.players[0].score == 0 && g.players[1].score == 0);
}

static void test_time_limit(void)
{
    puts("round time limit = draw");
    ba_game g;
    setup_playing(&g);
    g.phase_ms = 50;
    run(&g, 5, true);
    CHECK(g.phase == BA_PHASE_ROUND_OVER && g.winner == 0);
}

static void test_timeout_and_leave(void)
{
    puts("silent clients time out; leaving ends the round");
    ba_game g;
    setup_playing(&g);
    game_leave(&g, 2);
    run(&g, 2, true);
    CHECK(g.phase == BA_PHASE_ROUND_OVER && g.winner == 1);
    run(&g, BA_TICK_HZ * 4, false);                          /* nobody sends anything */
    CHECK(g.num_active == 0);
}

static void test_late_joiner_is_ghost(void)
{
    puts("joining mid-round = ghost until the next round");
    ba_game g;
    setup_playing(&g);
    int late = game_join(&g, 3, 0, "c", g_now);
    CHECK(late == 3 && !g.players[late - 1].alive);
    CHECK(!game_place_bomb(&g, late));
}

static void test_map_versions_and_snapshots(void)
{
    puts("map version bumps only on change; snapshots match");
    ba_game g;
    setup_playing(&g);
    game_map_changed(&g);
    uint32_t v = g.map_version;
    run(&g, 10, true);
    CHECK(!game_map_changed(&g) && g.map_version == v);      /* nothing changed */
    put(&g, 1, 3, 1);
    game_place_bomb(&g, 1);
    put(&g, 1, 1, 5);
    run_ms(&g, BA_FUSE_MS + 50);
    CHECK(game_map_changed(&g) && g.map_version == v + 1);

    Blast_MapState m;
    game_map_snapshot(&g, &m);
    CHECK(m.version == g.map_version);
    CHECK(m.tiles[1 * BA_MAP_W + 3] & BA_TILE_FLAME);
    CHECK((m.tiles[0] & 0x7F) == BA_TILE_WALL);

    Blast_GameState s;
    game_snapshot(&g, &s);
    CHECK(s.players[0].active && s.players[0].x == g.players[0].x);
    CHECK(s.map_version == g.map_version);
}

int main(void)
{
    test_join_and_full();
    test_names_are_sanitised();
    test_input_validation();
    test_map_generation();
    test_movement();
    test_bomb_placement();
    test_explosion();
    test_chain_and_winner();
    test_draw();
    test_time_limit();
    test_timeout_and_leave();
    test_late_joiner_is_ghost();
    test_map_versions_and_snapshots();
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
