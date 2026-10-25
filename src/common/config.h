/* config.h - constants used by the server, client and tests. */
#ifndef BA_CONFIG_H
#define BA_CONFIG_H

/* Window and map */
#define BA_WIDTH        800         /* window width in pixels             */
#define BA_HEIGHT       600         /* window height in pixels            */
#define BA_HUD_H        40          /* top HUD bar height                 */
#define BA_MAP_W        19          /* tiles across (odd: classic layout) */
#define BA_MAP_H        13          /* tiles down                         */
#define BA_MAP_TILES    (BA_MAP_W * BA_MAP_H)   /* must match tiles[247]  */
#define BA_TILE_PX      40          /* tile size on screen                */
#define BA_MAP_X        ((BA_WIDTH - BA_MAP_W * BA_TILE_PX) / 2)    /* 20 */
#define BA_MAP_Y        (BA_HUD_H + 10)                             /* 50 */

/* Tile types (low 7 bits of a tile) */
enum
{
    BA_TILE_EMPTY = 0,
    BA_TILE_WALL = 1,               /* indestructible pillar / border     */
    BA_TILE_CRATE = 2,              /* destructible, may hide a power-up  */
    BA_TILE_PU_BOMB = 3,            /* +1 bomb                            */
    BA_TILE_PU_FIRE = 4,            /* +1 blast range                     */
    BA_TILE_PU_SPEED = 5            /* faster walking                     */
};
#define BA_TILE_FLAME   0x80        /* bit set = flame burning on tile    */

/* Players */
#define BA_MAX_PLAYERS  5           /* must match players[5] in the IDL   */
#define BA_MIN_PLAYERS  2           /* players needed to start a round    */
#define BA_NAME_LEN     16          /* string<15> + terminating NUL       */
#define BA_MAX_BOMBS    25          /* must match bombs[25] in the IDL    */

/* Timing */
#define BA_TICK_HZ            60    /* server simulation rate             */
#define BA_SNAPSHOT_EVERY     2     /* GameState every N ticks (30 Hz)    */
#define BA_INPUT_HEARTBEAT_MS 1000  /* client re-sends input this often   */
#define BA_PLAYER_TIMEOUT_MS  3000  /* server drops a silent client       */
#define BA_JOIN_RETRY_MS      1000  /* client re-sends JoinRequest        */

/* Gameplay */
#define BA_BASE_SPEED     4.0f      /* tiles per second                   */
#define BA_SPEED_STEP     0.7f      /* extra tiles/s per speed power-up   */
#define BA_MAX_SPEED_UPS  4
#define BA_START_BOMBS    1
#define BA_MAX_BOMBS_PER  5
#define BA_START_RANGE    2
#define BA_MAX_RANGE      8
#define BA_FUSE_MS        2500      /* bomb timer                         */
#define BA_FLAME_MS       500       /* how long flames burn               */
#define BA_CRATE_PERCENT  65        /* chance a free tile gets a crate    */
#define BA_POWERUP_PERCENT 35       /* chance a crate hides a power-up    */
#define BA_COUNTDOWN_MS   3000      /* "3, 2, 1" before a round           */
#define BA_ROUND_MS       150000    /* round time limit (then a draw)     */
#define BA_ROUND_OVER_MS  4000      /* winner banner time                 */

/* Input keys (Input.keys) */
#define BA_KEY_UP     0x01
#define BA_KEY_DOWN   0x02
#define BA_KEY_LEFT   0x04
#define BA_KEY_RIGHT  0x08
#define BA_KEY_MASK   0x0F          /* any other bit is rejected          */

/* Game phases (GameState.phase) */
enum
{
    BA_PHASE_LOBBY = 0,             /* waiting for BA_MIN_PLAYERS         */
    BA_PHASE_COUNTDOWN = 1,         /* new map, players frozen            */
    BA_PHASE_PLAYING = 2,           /* bombs allowed                      */
    BA_PHASE_ROUND_OVER = 3         /* show winner, then next round       */
};

/* DDS topic names (also used in the security permissions) */
#define BA_TOPIC_JOIN_REQUEST   "BA_JoinRequest"
#define BA_TOPIC_JOIN_RESPONSE  "BA_JoinResponse"
#define BA_TOPIC_INPUT          "BA_Input"
#define BA_TOPIC_GAME_STATE     "BA_GameState"
#define BA_TOPIC_MAP_STATE      "BA_MapState"

#endif /* BA_CONFIG_H */
