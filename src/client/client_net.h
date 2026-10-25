/* client_net.h - the networking half of a client.
 *
 * Runs a DDS thread that joins the game, sends the keys and receives
 * GameState and MapState. The graphical client uses it.
 *
 * All functions are thread safe.
 */
#ifndef BA_CLIENT_NET_H
#define BA_CLIENT_NET_H

#include <stdbool.h>
#include <stdint.h>
#include "messages.h"
#include "interp.h"

typedef struct ba_client ba_client;

typedef struct
{
    const char *name;       /* shown on the scoreboard (max 15 chars)       */
    const char *config;     /* Cyclone DDS XML (security), or NULL          */
    int spoof_id;           /* TEST ONLY: pretend to be this player (0 = off) */
} ba_client_opts;

typedef struct
{
    int my_id;              /* -1 = connecting, 0 = server full, 1..5 = me  */
    bool have_state;        /* got at least one snapshot                    */
    Blast_GameState prev, curr; /* last two snapshots                   */
    int64_t prev_ns, curr_ns;       /* when each one arrived                */
    ba_interp interp;               /* recent positions, for smooth drawing */
    Blast_GameState states[BA_INTERP_SLOTS]; /* full snapshot for each interp slot */
    Blast_MapState maps[4];         /* last few maps, by version            */
    int map_head, map_count;
    bool have_map;                  /* got at least one MapState            */
    Blast_MapState map;             /* latest map (tiles + flame bits)      */
} ba_client_view;

/* Returns NULL (and logs why) if DDS could not start. */
ba_client *client_start(const ba_client_opts *opts);

/* Call when the held keys change. Wakes the DDS thread at once. */
void client_set_keys(ba_client *c, uint8_t keys);

/* Call once per bomb key press (not while held). */
void client_drop_bomb(ba_client *c);

/* Copy of the latest state, for drawing. */
void client_get_view(ba_client *c, ba_client_view *out);

/* The view to DRAW at now_ns: curr and map come from the same slightly
   delayed moment as the smoothed player positions, so bombs, flames and
   players line up. */
void client_view_at(const ba_client_view *v, int64_t now_ns, ba_client_view *out);

/* Leave the game cleanly (the server frees the slot at once). */
void client_stop(ba_client *c);

#endif /* BA_CLIENT_NET_H */
