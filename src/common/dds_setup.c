/* dds_setup.c - see dds_setup.h */
#include "dds_setup.h"
#include "config.h"
#include "util.h"

#include <string.h>

static dds_entity_t make_topic(ba_dds *d, const dds_topic_descriptor_t *desc,
                               const char *name, const dds_qos_t *qos)
{
    dds_entity_t t = dds_create_topic(d->participant, desc, name, qos, NULL);
    if (t < 0)
        BA_LOG("dds_create_topic(%s): %s", name, dds_strretcode(-t));
    return t;
}

bool ba_dds_open(ba_dds *d)
{
    memset(d, 0, sizeof *d);

    d->participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (d->participant < 0)
    {
        /* With DDS Security, a bad certificate or XML file fails here. */
        BA_LOG("dds_create_participant: %s", dds_strretcode(-d->participant));
        BA_LOG("If you used --secure, check the paths in the config file "
               "and run security/gen_security.sh again.");
        return false;
    }

    d->qos_reliable = dds_create_qos();
    dds_qset_reliability(d->qos_reliable, DDS_RELIABILITY_RELIABLE, DDS_MSECS(100));
    dds_qset_history(d->qos_reliable, DDS_HISTORY_KEEP_LAST, 1);

    d->qos_state = dds_create_qos();
    dds_qset_reliability(d->qos_state, DDS_RELIABILITY_BEST_EFFORT, 0);
    dds_qset_history(d->qos_state, DDS_HISTORY_KEEP_LAST, 1);

    d->qos_map = dds_create_qos();
    dds_qset_reliability(d->qos_map, DDS_RELIABILITY_RELIABLE, DDS_MSECS(100));
    dds_qset_history(d->qos_map, DDS_HISTORY_KEEP_LAST, 1);
    dds_qset_durability(d->qos_map, DDS_DURABILITY_TRANSIENT_LOCAL);

    d->topic_join_request  = make_topic(d, &Blast_JoinRequest_desc,
                                        BA_TOPIC_JOIN_REQUEST, d->qos_reliable);
    d->topic_join_response = make_topic(d, &Blast_JoinResponse_desc,
                                        BA_TOPIC_JOIN_RESPONSE, d->qos_reliable);
    d->topic_input         = make_topic(d, &Blast_Input_desc,
                                        BA_TOPIC_INPUT, d->qos_reliable);
    d->topic_game_state    = make_topic(d, &Blast_GameState_desc,
                                        BA_TOPIC_GAME_STATE, d->qos_state);
    d->topic_map_state     = make_topic(d, &Blast_MapState_desc,
                                        BA_TOPIC_MAP_STATE, d->qos_map);

    return d->topic_join_request > 0 && d->topic_join_response > 0 &&
           d->topic_input > 0 && d->topic_game_state > 0 &&
           d->topic_map_state > 0;
}

void ba_dds_close(ba_dds *d)
{
    /* Deleting the participant deletes its readers, writers and topics.
     * Deleting the Input writer tells the server that this client left. */
    if (d->participant > 0)
        dds_delete(d->participant);
    if (d->qos_reliable)
        dds_delete_qos(d->qos_reliable);
    if (d->qos_state)
        dds_delete_qos(d->qos_state);
    if (d->qos_map)
        dds_delete_qos(d->qos_map);
    memset(d, 0, sizeof *d);
}

static const dds_qos_t *qos_for(ba_dds *d, dds_entity_t topic)
{
    if (topic == d->topic_game_state)
        return d->qos_state;
    if (topic == d->topic_map_state)
        return d->qos_map;
    return d->qos_reliable;
}

dds_entity_t ba_dds_reader(ba_dds *d, dds_entity_t topic)
{
    dds_entity_t r = dds_create_reader(d->participant, topic, qos_for(d, topic), NULL);
    if (r < 0)
        BA_LOG("dds_create_reader: %s (access control may forbid this topic)",
               dds_strretcode(-r));
    return r;
}

dds_entity_t ba_dds_writer(ba_dds *d, dds_entity_t topic)
{
    dds_entity_t w = dds_create_writer(d->participant, topic, qos_for(d, topic), NULL);
    if (w < 0)
        BA_LOG("dds_create_writer: %s (access control may forbid this topic)",
               dds_strretcode(-w));
    return w;
}
