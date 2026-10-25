/* server_main.c - Blast Arena server.
 *
 * Two threads:
 *   main thread: fixed 60 Hz game loop. Sleeps until the next tick, runs
 *                game_tick(), and every second tick builds a GameState snapshot
 *                and wakes the DDS thread.
 *   DDS thread:  waits on a DDS WaitSet until something happens (JoinRequest,
 *                Input, or "snapshot ready"), handles it, then waits again.
 *
 * One mutex protects the shared data (the game and the snapshot). It is only
 * held for short in-memory work, never during a DDS call or a sleep.
 *
 * Usage: ./ba_server [--secure security/out/server.xml] [--seed N] [-v]
 */
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "dds_setup.h"
#include "game.h"
#include "util.h"

#define READ_BATCH 16   /* samples taken per dds_take call */

/* Shared by both threads (guarded by g_lock) */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static ba_game g_game;
static Blast_GameState g_snapshot;
static Blast_MapState g_map;          /* copied only when the map changes     */
static bool g_snapshot_pending, g_map_pending;

/* Flags and statistics */
static atomic_bool g_running = true;
static atomic_uint_fast64_t g_msgs_in, g_msgs_out, g_rejected, g_map_msgs;

/* DDS entities used by the DDS thread. The main thread only touches the
   guard condition (it is thread safe). */
static ba_dds g_dds;
static dds_entity_t g_join_reader, g_input_reader;
static dds_entity_t g_join_writer, g_state_writer, g_map_writer;
static dds_entity_t g_snapshot_guard;

static void on_signal(int sig)
{
    (void)sig;
    atomic_store(&g_running, false);
}

/* Which client (DDS participant) owns this writer? With DDS Security, the
   participant is tied to a checked certificate, so this tells us who really
   sent a message, whatever the message says. */
static uint64_t owner_of(dds_entity_t reader, dds_instance_handle_t writer)
{
    dds_builtintopic_endpoint_t *ep = dds_get_matched_publication_data(reader, writer);
    if (ep == NULL)
        return 0;
    uint64_t owner = ep->participant_instance_handle;
    dds_builtintopic_free_endpoint(ep);
    return owner;
}

/* DDS thread: message handlers */

static void handle_join_requests(void)
{
    Blast_JoinRequest buf[READ_BATCH];
    void *samples[READ_BATCH];
    dds_sample_info_t infos[READ_BATCH];
    for (int i = 0; i < READ_BATCH; i++)
        samples[i] = &buf[i];

    /* Take everything that is waiting, not just one sample. */
    int n;
    while ((n = dds_take(g_join_reader, samples, infos, READ_BATCH, READ_BATCH)) > 0)
    {
        for (int i = 0; i < n; i++)
        {
            if (!infos[i].valid_data)
                continue;
            atomic_fetch_add(&g_msgs_in, 1);

            uint64_t owner = owner_of(g_join_reader, infos[i].publication_handle);

            pthread_mutex_lock(&g_lock);
            int before = g_game.num_active;
            int id = game_join(&g_game, buf[i].nonce, owner, buf[i].name, ba_now_ms());
            bool is_new = g_game.num_active > before;
            pthread_mutex_unlock(&g_lock);

            if (is_new)
                BA_LOG("Player %d joined (%s)", id, buf[i].name);
            else if (id == 0)
                BA_LOG("Join refused: server full");

            Blast_JoinResponse resp = { .nonce = buf[i].nonce, .player_id = id };
            if (dds_write(g_join_writer, &resp) == DDS_RETCODE_OK)
                atomic_fetch_add(&g_msgs_out, 1);
        }
    }
}

static void handle_inputs(void)
{
    Blast_Input buf[READ_BATCH];
    void *samples[READ_BATCH];
    dds_sample_info_t infos[READ_BATCH];
    for (int i = 0; i < READ_BATCH; i++)
        samples[i] = &buf[i];

    int n;
    while ((n = dds_take(g_input_reader, samples, infos, READ_BATCH, READ_BATCH)) > 0)
    {
        for (int i = 0; i < n; i++)
        {
            atomic_fetch_add(&g_msgs_in, 1);
            int id = buf[i].player_id; /* key field, filled in even for invalid samples */
            if (id < 1 || id > BA_MAX_PLAYERS)
            {
                atomic_fetch_add(&g_rejected, 1);
                continue;
            }
            dds_instance_handle_t writer = infos[i].publication_handle;

            /* Is this writer allowed to control player 'id'?
             * Fast path: we checked this writer before (cached). */
            pthread_mutex_lock(&g_lock);
            ba_player *p = &g_game.players[id - 1];
            bool active = p->active;
            bool known = active && p->input_writer == writer;
            uint64_t expected_owner = p->owner;
            pthread_mutex_unlock(&g_lock);

            if (!active)
                continue; /* for example a late message from a player who timed out */

            if (!known)
            {
                /* Slow path, once per writer: find its participant
                 * outside the lock, then compare with the slot owner. */
                if (owner_of(g_input_reader, writer) != expected_owner)
                {
                    atomic_fetch_add(&g_rejected, 1);
                    BA_LOG("SECURITY: rejected Input for player %d from a "
                           "client that does not own that player", id);
                    continue;
                }
                pthread_mutex_lock(&g_lock);
                if (p->active && p->owner == expected_owner)
                    p->input_writer = writer;
                pthread_mutex_unlock(&g_lock);
            }

            /* The owner disposed its instance or closed its writer:
             * it left on purpose. */
            if (infos[i].instance_state != DDS_IST_ALIVE)
            {
                pthread_mutex_lock(&g_lock);
                game_leave(&g_game, id);
                pthread_mutex_unlock(&g_lock);
                BA_LOG("Player %d left", id);
                continue;
            }
            if (!infos[i].valid_data)
                continue;

            pthread_mutex_lock(&g_lock);
            bool ok = game_set_input(&g_game, id, buf[i].keys, buf[i].bomb_seq, ba_now_ms());
            pthread_mutex_unlock(&g_lock);
            if (!ok)
                atomic_fetch_add(&g_rejected, 1);
        }
    }
}

static void publish_snapshot(void)
{
    /* Copy under the lock (a few hundred bytes), write without it. */
    Blast_GameState snap;
    Blast_MapState map;
    pthread_mutex_lock(&g_lock);
    bool send_map = g_map_pending, send_state = g_snapshot_pending;
    if (send_map)
        map = g_map;
    if (send_state)
        snap = g_snapshot;
    g_map_pending = g_snapshot_pending = false;
    pthread_mutex_unlock(&g_lock);

    /* Map first, so a client never sees bombs on a map it does not have. */
    if (send_map && dds_write(g_map_writer, &map) == DDS_RETCODE_OK)
    {
        atomic_fetch_add(&g_msgs_out, 1);
        atomic_fetch_add(&g_map_msgs, 1);
    }
    if (send_state && dds_write(g_state_writer, &snap) == DDS_RETCODE_OK)
        atomic_fetch_add(&g_msgs_out, 1);
}

/* DDS thread */

static void *dds_thread(void *arg)
{
    (void)arg;

    /* A WaitSet blocks the thread until a condition fires, so it does not
     * use a CPU core while nobody is playing. */
    dds_entity_t ws = dds_create_waitset(g_dds.participant);
    dds_entity_t join_cond = dds_create_readcondition(g_join_reader, DDS_ANY_STATE);
    dds_entity_t input_cond = dds_create_readcondition(g_input_reader, DDS_ANY_STATE);
    dds_waitset_attach(ws, join_cond, join_cond);
    dds_waitset_attach(ws, input_cond, input_cond);
    dds_waitset_attach(ws, g_snapshot_guard, g_snapshot_guard);

    dds_attach_t fired[4];
    while (atomic_load(&g_running))
    {
        int n = dds_waitset_wait(ws, fired, 4, DDS_MSECS(500));
        for (int i = 0; i < n; i++)
        {
            if (fired[i] == (dds_attach_t)join_cond)
                handle_join_requests();
            else if (fired[i] == (dds_attach_t)input_cond)
                handle_inputs();
            else if (fired[i] == (dds_attach_t)g_snapshot_guard)
            {
                bool triggered;
                dds_take_guardcondition(g_snapshot_guard, &triggered); /* reset */
                publish_snapshot();
            }
        }
    }
    dds_delete(ws);
    return NULL;
}

/* Main thread: fixed-rate game loop */

static void usage(const char *prog)
{
    printf("Usage: %s [--secure CONFIG.xml] [--seed N] [-v]\n"
           "  --secure  enable DDS Security with the given Cyclone config\n"
           "  --seed    fixed random seed (repeatable games for testing)\n"
           "  -v        verbose debug logging\n", prog);
}

int main(int argc, char **argv)
{
    const char *config = NULL;
    uint64_t seed = ba_random_u64();
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--secure") && i + 1 < argc)
            config = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-v"))
            ba_verbose = 1;
        else
        {
            usage(argv[0]);
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (!ba_use_dds_config(config) || !ba_dds_open(&g_dds))
        return 1;
    if (!config)
        BA_LOG("WARNING: running WITHOUT DDS Security (no --secure). "
               "Traffic is unencrypted and any client can join.");

    g_join_reader  = ba_dds_reader(&g_dds, g_dds.topic_join_request);
    g_input_reader = ba_dds_reader(&g_dds, g_dds.topic_input);
    g_join_writer  = ba_dds_writer(&g_dds, g_dds.topic_join_response);
    g_state_writer = ba_dds_writer(&g_dds, g_dds.topic_game_state);
    g_map_writer   = ba_dds_writer(&g_dds, g_dds.topic_map_state);
    g_snapshot_guard = dds_create_guardcondition(g_dds.participant);
    if (g_join_reader < 0 || g_input_reader < 0 || g_join_writer < 0 || g_state_writer < 0 ||
        g_map_writer < 0)
    {
        ba_dds_close(&g_dds);
        return 1;
    }

    game_init(&g_game, seed);

    pthread_t thread;
    if (pthread_create(&thread, NULL, dds_thread, NULL) != 0)
    {
        perror("pthread_create");
        ba_dds_close(&g_dds);
        return 1;
    }

    BA_LOG("Blast Arena server running at %d Hz. Ctrl+C to stop.", BA_TICK_HZ);

    const int64_t tick_ns = 1000000000LL / BA_TICK_HZ;
    const float tick_ms = 1000.0f / BA_TICK_HZ;
    int64_t next = ba_now_ns();

    /* Numbers printed every 5 s. */
    int64_t stat_start = next, work_ns_sum = 0, work_ns_max = 0;
    int stat_ticks = 0;

    while (atomic_load(&g_running))
    {
        int64_t t0 = ba_now_ns();

        pthread_mutex_lock(&g_lock);
        game_tick(&g_game, tick_ms, t0 / 1000000LL);
        bool send = (g_game.tick % BA_SNAPSHOT_EVERY) == 0;
        /* The map (247 bytes) is only sent when a tile changed. */
        if (game_map_changed(&g_game))
        {
            game_map_snapshot(&g_game, &g_map);
            g_map_pending = true;
            send = true;
        }
        if (send)
        {
            game_snapshot(&g_game, &g_snapshot);
            g_snapshot_pending = true;
        }
        pthread_mutex_unlock(&g_lock);

        if (send)
            dds_set_guardcondition(g_snapshot_guard, true); /* wake the DDS thread */

        int64_t work = ba_now_ns() - t0;
        work_ns_sum += work;
        if (work > work_ns_max)
            work_ns_max = work;
        stat_ticks++;

        if (t0 - stat_start >= 5000000000LL)
        {
            double secs = (double)(t0 - stat_start) / 1e9;
            BA_LOG("[stats] players %d | tick avg %.1f us max %.1f us | "
                   "msgs in %.0f/s out %.0f/s (maps %.1f/s) | rejected %lu",
                   g_game.num_active, work_ns_sum / 1000.0 / stat_ticks,
                   work_ns_max / 1000.0,
                   atomic_exchange(&g_msgs_in, 0) / secs,
                   atomic_exchange(&g_msgs_out, 0) / secs,
                   atomic_exchange(&g_map_msgs, 0) / secs,
                   (unsigned long)atomic_load(&g_rejected));
            stat_start = t0;
            work_ns_sum = work_ns_max = 0;
            stat_ticks = 0;
        }

        /* Sleep until an exact time, so a slow tick does not delay later ones. */
        next += tick_ns;
        int64_t now = ba_now_ns();
        if (now > next + 10 * tick_ns)
            next = now; /* far behind (for example suspended): resync  */
        ba_sleep_until_ns(next);
    }

    BA_LOG("Shutting down");
    dds_set_guardcondition(g_snapshot_guard, true);
    pthread_join(thread, NULL);
    ba_dds_close(&g_dds);
    return 0;
}
