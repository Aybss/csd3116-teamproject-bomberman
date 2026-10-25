/* dds_setup.h - creates the DDS participant and the topics with their QoS.
 * Server and client both use it, so their QoS always match. */
#ifndef BA_DDS_SETUP_H
#define BA_DDS_SETUP_H

#include <stdbool.h>
#include "dds/dds.h"
#include "messages.h"

typedef struct
{
    dds_entity_t participant;
    dds_entity_t topic_join_request;
    dds_entity_t topic_join_response;
    dds_entity_t topic_input;
    dds_entity_t topic_game_state;
    dds_entity_t topic_map_state;

    /* QoS:
     *  reliable: JoinRequest, JoinResponse, Input. Keep only the newest value.
     *  state:    GameState. Best effort. A lost one is replaced 33 ms later.
     *  map:      MapState. Reliable and transient local, so late joiners
     *            still get the current map. */
    dds_qos_t *qos_reliable;
    dds_qos_t *qos_state;
    dds_qos_t *qos_map;
} ba_dds;

/* Returns false (and logs why) if something fails, for example a bad security config. */
bool ba_dds_open(ba_dds *d);
void ba_dds_close(ba_dds *d);

/* Reader / writer for a topic, with that topic's QoS. */
dds_entity_t ba_dds_reader(ba_dds *d, dds_entity_t topic);
dds_entity_t ba_dds_writer(ba_dds *d, dds_entity_t topic);

#endif /* BA_DDS_SETUP_H */
