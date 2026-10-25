/* client_net.c - DDS thread for a client. See client_net.h. */
#include "client_net.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "dds_setup.h"
#include "util.h"

#define READ_BATCH 8
#define LOST_SLOT_MS 2000   /* if our slot is missing this long, join again       */

struct ba_client
{
    ba_client_opts opts;
    char name[BA_NAME_LEN];

    ba_dds dds;
    dds_entity_t resp_reader, state_reader, map_reader;
    dds_entity_t join_writer, input_writer;
    dds_entity_t wake_guard;            /* main thread wakes the DDS thread */

    pthread_t thread;
    atomic_bool running;
    atomic_uint keys;                   /* main writes, DDS thread reads       */
    atomic_uint bomb_seq;               /* +1 per bomb press                   */

    pthread_mutex_t lock;               /* guards 'view' */
    ba_client_view view;

    /* Only touched by the DDS thread. */
    uint64_t nonce;
    uint32_t seq;
};

/* DDS thread */

static void take_join_responses(ba_client *c)
{
    Blast_JoinResponse buf[READ_BATCH];
    void *samples[READ_BATCH];
    dds_sample_info_t infos[READ_BATCH];
    for (int i = 0; i < READ_BATCH; i++)
        samples[i] = &buf[i];

    int n;
    while ((n = dds_take(c->resp_reader, samples, infos, READ_BATCH, READ_BATCH)) > 0)
    {
        for (int i = 0; i < n; i++)
        {
            /* Ignore replies for other clients: only ours has our nonce. */
            if (!infos[i].valid_data || buf[i].nonce != c->nonce)
                continue;

            pthread_mutex_lock(&c->lock);
            int old = c->view.my_id;
            if (old < 1)
                c->view.my_id = buf[i].player_id; /* 0 = full, keep trying   */
            pthread_mutex_unlock(&c->lock);

            if (old < 1 && buf[i].player_id > 0)
                BA_LOG("Joined as player %d", buf[i].player_id);
            else if (old == -1 && buf[i].player_id == 0)
                BA_LOG("Server is full, will keep trying");
        }
    }
}

static void take_game_state(ba_client *c)
{
    Blast_GameState buf[READ_BATCH];
    void *samples[READ_BATCH];
    dds_sample_info_t infos[READ_BATCH];
    for (int i = 0; i < READ_BATCH; i++)
        samples[i] = &buf[i];

    int n;
    while ((n = dds_take(c->state_reader, samples, infos, READ_BATCH, READ_BATCH)) > 0)
    {
        int64_t now = ba_now_ns();
        pthread_mutex_lock(&c->lock);
        for (int i = 0; i < n; i++)
        {
            if (!infos[i].valid_data)
                continue;
            ba_client_view *v = &c->view;
            /* Best effort can reorder, so drop an older snapshot.
             * A big jump back means the server restarted, so accept it. */
            if (v->have_state && buf[i].tick <= v->curr.tick &&
                v->curr.tick - buf[i].tick < 600)
                continue;
            v->prev = v->have_state ? v->curr : buf[i];
            v->prev_ns = v->have_state ? v->curr_ns : now;
            v->curr = buf[i];
            v->curr_ns = now;
            v->have_state = true;

            /* Feed the smoothing buffer (see interp.h). */
            ba_interp_snap snap;
            snap.tick = buf[i].tick;
            for (int p = 0; p < BA_MAX_PLAYERS; p++)
            {
                snap.present[p] = buf[i].players[p].active;
                snap.x[p] = buf[i].players[p].x;
                snap.y[p] = buf[i].players[p].y;
            }
            int slot = ba_interp_push(&v->interp, &snap, now);
            if (slot >= 0)
                v->states[slot] = buf[i];
        }
        pthread_mutex_unlock(&c->lock);
    }
}

static void take_map(ba_client *c)
{
    Blast_MapState buf[2];
    void *samples[2] = { &buf[0], &buf[1] };
    dds_sample_info_t infos[2];
    int n;
    while ((n = dds_take(c->map_reader, samples, infos, 2, 2)) > 0)
    {
        pthread_mutex_lock(&c->lock);
        for (int i = 0; i < n; i++)
        {
            if (!infos[i].valid_data)
                continue;
            c->view.map = buf[i];   /* reliable and ordered: newest wins */
            c->view.have_map = true;
            /* Keep a few recent maps, so the delayed frame can use its matching map. */
            ba_client_view *v = &c->view;
            v->map_head = (v->map_head + 1) % 4;
            v->maps[v->map_head] = buf[i];
            if (v->map_count < 4)
                v->map_count++;
        }
        pthread_mutex_unlock(&c->lock);
    }
}

static void send_join(ba_client *c)
{
    Blast_JoinRequest req;
    memset(&req, 0, sizeof req);
    req.nonce = c->nonce;
    memcpy(req.name, c->name, sizeof req.name);
    dds_write(c->join_writer, &req);
    BA_LOG_DEBUG("JoinRequest sent");
}

static void send_input(ba_client *c, int my_id, uint8_t keys, uint8_t bomb_seq)
{
    Blast_Input in;
    in.player_id = c->opts.spoof_id ? c->opts.spoof_id : my_id;
    in.seq = ++c->seq;
    in.keys = keys;
    in.bomb_seq = bomb_seq;
    dds_write(c->input_writer, &in);
    BA_LOG_DEBUG("Input sent keys=0x%x", keys);
}

static void *net_thread(void *arg)
{
    ba_client *c = arg;

    /* Wait for a reply, a snapshot or a key change, or 50 ms (then check retries and heartbeat). */
    dds_entity_t ws = dds_create_waitset(c->dds.participant);
    dds_entity_t rc_resp = dds_create_readcondition(c->resp_reader, DDS_ANY_STATE);
    dds_entity_t rc_state = dds_create_readcondition(c->state_reader, DDS_ANY_STATE);
    dds_entity_t rc_map = dds_create_readcondition(c->map_reader, DDS_ANY_STATE);
    dds_waitset_attach(ws, rc_map, 0);
    dds_waitset_attach(ws, rc_resp, 0);
    dds_waitset_attach(ws, rc_state, 0);
    dds_waitset_attach(ws, c->wake_guard, 0);

    int64_t next_join_ms = 0, next_heartbeat_ms = 0, slot_missing_since = 0;
    int sent_keys = -1, sent_bomb = -1;

    while (atomic_load(&c->running))
    {
        dds_waitset_wait(ws, NULL, 0, DDS_MSECS(50));
        bool woke;
        dds_take_guardcondition(c->wake_guard, &woke);

        take_join_responses(c);
        take_map(c);
        take_game_state(c);

        pthread_mutex_lock(&c->lock);
        int my_id = c->view.my_id;
        bool slot_gone = my_id > 0 && c->view.have_state &&
                         !c->view.curr.players[my_id - 1].active;
        pthread_mutex_unlock(&c->lock);

        int64_t now = ba_now_ms();
        if (my_id < 1)
        {
            /* Still joining (or server full): ask again every second. */
            if (now >= next_join_ms)
            {
                send_join(c);
                next_join_ms = now + BA_JOIN_RETRY_MS;
            }
            continue;
        }

        /* The server dropped us (for example we froze for 3 s): join again. */
        if (slot_gone)
        {
            if (slot_missing_since == 0)
                slot_missing_since = now;
            else if (now - slot_missing_since > LOST_SLOT_MS)
            {
                BA_LOG("Lost our slot on the server, joining again");
                c->nonce = ba_random_u64();
                pthread_mutex_lock(&c->lock);
                c->view.my_id = -1;
                pthread_mutex_unlock(&c->lock);
                slot_missing_since = 0;
                next_join_ms = 0;
                sent_keys = -1;
                sent_bomb = -1;
                continue;
            }
        }
        else
            slot_missing_since = 0;

        /* Send keys only when they change, plus a heartbeat so the server
         * knows we are still here. */
        int keys = (int)atomic_load(&c->keys);
        int bomb = (int)(atomic_load(&c->bomb_seq) & 0xFF);
        if (keys != sent_keys || bomb != sent_bomb || now >= next_heartbeat_ms)
        {
            send_input(c, my_id, (uint8_t)keys, (uint8_t)bomb);
            sent_keys = keys;
            sent_bomb = bomb;
            next_heartbeat_ms = now + BA_INPUT_HEARTBEAT_MS;
        }
    }
    dds_delete(ws);
    return NULL;
}

/* Public API */

ba_client *client_start(const ba_client_opts *opts)
{
    ba_client *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->opts = *opts;
    snprintf(c->name, sizeof c->name, "%s", opts->name ? opts->name : "");
    c->view.my_id = -1;
    c->nonce = ba_random_u64();
    pthread_mutex_init(&c->lock, NULL);
    atomic_store(&c->running, true);

    if (!ba_use_dds_config(opts->config) || !ba_dds_open(&c->dds))
    {
        free(c);
        return NULL;
    }
    c->resp_reader  = ba_dds_reader(&c->dds, c->dds.topic_join_response);
    c->state_reader = ba_dds_reader(&c->dds, c->dds.topic_game_state);
    c->map_reader   = ba_dds_reader(&c->dds, c->dds.topic_map_state);
    c->join_writer  = ba_dds_writer(&c->dds, c->dds.topic_join_request);
    c->input_writer = ba_dds_writer(&c->dds, c->dds.topic_input);
    c->wake_guard   = dds_create_guardcondition(c->dds.participant);
    if (c->resp_reader < 0 || c->state_reader < 0 || c->map_reader < 0 ||
        c->join_writer < 0 || c->input_writer < 0)
    {
        ba_dds_close(&c->dds);
        free(c);
        return NULL;
    }

    if (pthread_create(&c->thread, NULL, net_thread, c) != 0)
    {
        perror("pthread_create");
        ba_dds_close(&c->dds);
        free(c);
        return NULL;
    }
    return c;
}

void client_set_keys(ba_client *c, uint8_t keys)
{
    if (atomic_exchange(&c->keys, keys) != keys)
        dds_set_guardcondition(c->wake_guard, true); /* send it now */
}

void client_drop_bomb(ba_client *c)
{
    atomic_fetch_add(&c->bomb_seq, 1);
    dds_set_guardcondition(c->wake_guard, true);
}

void client_view_at(const ba_client_view *v, int64_t now_ns, ba_client_view *out)
{
    *out = *v;
    int slot = ba_interp_slot_at(&v->interp, now_ns);
    if (slot < 0)
        return;
    out->curr = v->states[slot];
    /* Map whose version matches that snapshot; else the newest older one. */
    int best = -1;
    for (int k = 0; k < v->map_count; k++)
    {
        const Blast_MapState *m = &v->maps[(v->map_head - k + 4) % 4];
        if (m->version <= out->curr.map_version &&
            (best < 0 || m->version > v->maps[best].version))
            best = (v->map_head - k + 4) % 4;
    }
    if (best >= 0)
        out->map = v->maps[best];
}

void client_get_view(ba_client *c, ba_client_view *out)
{
    pthread_mutex_lock(&c->lock);
    *out = c->view;    /* copy, so drawing needs no lock                */
    pthread_mutex_unlock(&c->lock);
}

void client_stop(ba_client *c)
{
    if (!c)
        return;
    atomic_store(&c->running, false);
    dds_set_guardcondition(c->wake_guard, true);
    pthread_join(c->thread, NULL);

    /* Tell the server we are leaving. Disposing our Input instance frees
     * our slot at once, without waiting for the 3 s timeout. */
    if (c->view.my_id > 0 && !c->opts.spoof_id)
    {
        Blast_Input in = { .player_id = c->view.my_id };
        dds_dispose(c->input_writer, &in);
        dds_sleepfor(DDS_MSECS(50)); /* let the dispose go out          */
    }
    ba_dds_close(&c->dds);
    pthread_mutex_destroy(&c->lock);
    free(c);
}
