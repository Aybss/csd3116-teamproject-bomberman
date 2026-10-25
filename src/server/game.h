/* game.h - Blast Arena game rules.
 *
 * This module has no DDS, no threads and no clock. The server gives it events
 * (join, input, leave) and calls game_tick() at a fixed rate. Nothing here
 * uses the network, so the rules can be unit tested (tests/test_game.c).
 *
 * Coordinates are in tiles. (0,0) is the top-left tile of the 19x13 map.
 * The caller must hold the server mutex for every call.
 */
#ifndef BA_GAME_H
#define BA_GAME_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "util.h"
#include "messages.h"

typedef struct
{
    bool     active;            /* slot is used                             */
    bool     alive;             /* still in this round                      */
    float    x, y;              /* position in tiles                        */
    int      fx, fy;            /* tile moving from                         */
    int      tx, ty;            /* tile moving to                           */
    bool     moving;
    int      score;             /* rounds won                               */
    int      bombs_max;         /* power-up stats, reset each round         */
    int      range;
    int      speed_ups;
    char     name[BA_NAME_LEN];
    uint8_t  keys;              /* held keys (BA_KEY_* bits)                */
    uint8_t  bomb_seq;          /* last bomb counter from the client        */
    int64_t  last_seen_ms;      /* last message from this client            */
    uint64_t nonce;             /* nonce of the JoinRequest                 */
    uint64_t owner;             /* DDS handle of the client                 */
    uint64_t input_writer;      /* DDS handle of its Input writer           */
} ba_player;

typedef struct
{
    bool  active;
    int   x, y;
    float fuse_ms;
    int   owner;                /* player id                                */
    int   range;                /* copied from the owner                    */
} ba_bomb;

typedef struct
{
    ba_player players[BA_MAX_PLAYERS];  /* index = player id - 1 */
    int       num_active;
    int       phase;            /* BA_PHASE_*                               */
    float     phase_ms;         /* time left in this phase                  */
    int       winner;           /* winner of the last round, 0 = draw       */
    uint32_t  tick;
    ba_rng    rng;

    uint8_t   tiles[BA_MAP_TILES];      /* BA_TILE_* (no flame bit)         */
    float     flame_ms[BA_MAP_TILES];   /* above 0 while a flame burns      */
    uint8_t   hidden[BA_MAP_TILES];     /* power-up hidden under a crate    */
    uint8_t   reveal[BA_MAP_TILES];     /* power-up shown when the flame ends */
    ba_bomb   bombs[BA_MAX_BOMBS];
    uint32_t  map_version;
    bool      map_dirty;        /* a tile changed since the last MapState   */
} ba_game;

void game_init(ba_game *g, uint64_t seed);

/* Add a client. Returns its player id (1..BA_MAX_PLAYERS), or 0 if full.
 * The same nonce gives the same id, so a retry is safe. */
int  game_join(ba_game *g, uint64_t nonce, uint64_t owner,
               const char *name, int64_t now_ms);

/* Remove a player (left, disposed, or timed out). */
void game_leave(ba_game *g, int id);

/* Apply a client's input. A new bomb_seq means "drop a bomb".
 * Returns false if the message is invalid (it is ignored). */
bool game_set_input(ba_game *g, int id, uint8_t keys, uint8_t bomb_seq,
                    int64_t now_ms);

/* Move the game forward by dt_ms. Also drops silent clients. */
void game_tick(ba_game *g, float dt_ms, int64_t now_ms);

/* If the map changed since the last call, raise map_version and return true.
 * The server then sends a new MapState. */
bool game_map_changed(ba_game *g);

/* Copy the state into DDS samples. */
void game_snapshot(const ba_game *g, Blast_GameState *out);
void game_map_snapshot(const ba_game *g, Blast_MapState *out);

/* Helpers for the unit tests. */
uint8_t game_tile(const ba_game *g, int x, int y);   /* WALL if outside the map */
bool    game_place_bomb(ba_game *g, int id);
int     game_count_alive(const ba_game *g);
void    game_spawn_point(int id, int *x, int *y);

#endif /* BA_GAME_H */
