/* access_check.c - check that DDS Security access control blocks a client.
 *
 * Run with a client's config:
 *   ./build/access_check security/out/client1.xml
 *
 * A client may write BA_JoinRequest and BA_Input, and read the server's
 * topics. It must NOT be able to write BA_GameState (fake the game) or
 * read BA_Input (spy on other players). Each line says what was tried.
 */
#include <stdio.h>

#include "dds_setup.h"
#include "util.h"

static int g_failed;

static void expect(const char *what, dds_entity_t e, int should_work)
{
    int worked = e > 0;
    printf("%-34s %s (expected %s)\n", what, worked ? "ALLOWED" : "DENIED",
           should_work ? "ALLOWED" : "DENIED");
    if (worked != should_work)
        g_failed++;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("Usage: %s security/out/clientN.xml\n", argv[0]);
        return 1;
    }
    ba_dds d;
    if (!ba_use_dds_config(argv[1]) || !ba_dds_open(&d))
        return 1;

    expect("write BA_Input", ba_dds_writer(&d, d.topic_input), 1);
    expect("write BA_JoinRequest", ba_dds_writer(&d, d.topic_join_request), 1);
    expect("read  BA_GameState", ba_dds_reader(&d, d.topic_game_state), 1);
    expect("write BA_GameState (fake state)", ba_dds_writer(&d, d.topic_game_state), 0);
    expect("write BA_MapState (fake map)", ba_dds_writer(&d, d.topic_map_state), 0);
    expect("read  BA_Input (spy on others)", ba_dds_reader(&d, d.topic_input), 0);

    ba_dds_close(&d);
    printf("%s\n", g_failed ? "FAIL" : "PASS: access control works");
    return g_failed ? 1 : 0;
}
