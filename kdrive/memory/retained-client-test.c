#include <stdlib.h>
#include <string.h>

#include "tinyx.h"

static int
pump(tinyx_server *server, unsigned int minimum_requests)
{
    tinyx_step_result step;
    unsigned int requests = 0;
    unsigned int i;

    for (i = 0; i < 1000; i++) {
        if (tinyx_server_step(server, 64, &step) != TINYX_OK)
            return 0;
        requests += step.requests_processed;
        if (!step.immediate_work)
            return requests >= minimum_requests;
    }
    return 0;
}

static int
handshake(tinyx_server *server, tinyx_client *client)
{
    static const unsigned char setup[12] = {
        'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    unsigned char *reply;
    size_t count;
    size_t pending;
    int success = 0;

    if (tinyx_client_send(client, setup, sizeof(setup), &count) != TINYX_OK ||
        count != sizeof(setup) || !pump(server, 1))
        return 0;
    pending = tinyx_client_receive_pending(client);
    if (!pending || !(reply = malloc(pending)))
        return 0;
    if (tinyx_client_receive(client, reply, pending, &count) == TINYX_OK &&
        count == pending && reply[0] == 1)
        success = 1;
    free(reply);
    return success;
}

int
main(void)
{
    static const unsigned char retain_permanent[4] = {112, 1, 1, 0};
    static const unsigned char no_operation[4] = {127, 0, 1, 0};
    tinyx_screen_config screen;
    tinyx_client_config client_config;
    tinyx_config config;
    tinyx_error error;
    tinyx_server *server = NULL;
    tinyx_client *retained = NULL;
    tinyx_client *active = NULL;
    unsigned char requests[2048 * sizeof(no_operation) + 4];
    unsigned char reply[32];
    size_t count;
    size_t i;
    int result = 1;

    tinyx_screen_config_init(&screen);
    screen.framebuffer.width = 64;
    screen.framebuffer.height = 64;
    tinyx_config_init(&config);
    config.initial_screen = &screen;
    if (tinyx_server_create(&config, &server, &error) != TINYX_OK)
        goto done;

    tinyx_client_config_init(&client_config);
    if (tinyx_client_open(server, &client_config, &retained) != TINYX_OK ||
        tinyx_client_open(server, &client_config, &active) != TINYX_OK ||
        !handshake(server, retained) || !handshake(server, active))
        goto done;

    if (tinyx_client_send(retained, retain_permanent,
                          sizeof(retain_permanent), &count) != TINYX_OK ||
        count != sizeof(retain_permanent) || !pump(server, 1))
        goto done;
    tinyx_client_destroy(retained);
    retained = NULL;

    /* The retained DIX client has no transport. Scanning ready clients must
     * skip it while continuing to service another memory client. */
    if (tinyx_client_send(active, no_operation, sizeof(no_operation),
                          &count) != TINYX_OK ||
        count != sizeof(no_operation) || !pump(server, 1))
        goto done;

    /* A single host send can span several DIX input-buffer fills. Keep the
     * memory transport runnable between fills so the trailing reply request
     * is not stranded until the host happens to send more bytes. */
    for (i = 0; i < 2048; i++)
        memcpy(requests + i * sizeof(no_operation), no_operation,
               sizeof(no_operation));
    requests[sizeof(requests) - 4] = 43; /* GetInputFocus */
    requests[sizeof(requests) - 3] = 0;
    requests[sizeof(requests) - 2] = 1;
    requests[sizeof(requests) - 1] = 0;
    if (tinyx_client_send(active, requests, sizeof(requests), &count) !=
            TINYX_OK ||
        count != sizeof(requests) || !pump(server, 2049) ||
        tinyx_client_receive(active, reply, sizeof(reply), &count) !=
            TINYX_OK ||
        count != sizeof(reply) || reply[0] != 1)
        goto done;

    result = 0;
done:
    if (retained)
        tinyx_client_destroy(retained);
    if (active)
        tinyx_client_destroy(active);
    if (server)
        tinyx_server_destroy(server);
    return result;
}
