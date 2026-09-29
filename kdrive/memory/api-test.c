#include <stdint.h>

#include "tinyx.h"

int
main(void)
{
    static const unsigned char setup[12] = {
        'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    tinyx_screen_config screen;
    tinyx_config config;
    tinyx_client_config clientConfig;
    tinyx_step_result step;
    tinyx_error error;
    tinyx_server *server = NULL;
    tinyx_server *second = NULL;
    tinyx_client *client = NULL;
    unsigned char output[32];
    size_t offset = 0;
    size_t count;
    unsigned int iterations;

    tinyx_screen_config_init(&screen);
    screen.width = 64;
    screen.height = 64;
    tinyx_config_init(&config);
    config.initial_screen = &screen;

    if (tinyx_server_create(&config, &server, &error) != TINYX_OK || !server)
        return 1;
    if (tinyx_server_create(&config, &second, &error) !=
        TINYX_ERROR_ALREADY_EXISTS || second)
        return 2;

    tinyx_client_config_init(&clientConfig);
    clientConfig.input_buffer_limit = 8;
    clientConfig.output_buffer_limit = 32;
    if (tinyx_client_open(server, &clientConfig, &client) != TINYX_OK)
        return 3;

    while (offset < sizeof(setup)) {
        tinyx_status status = tinyx_client_send(client, setup + offset,
                                                sizeof(setup) - offset,
                                                &count);
        if (status == TINYX_OK) {
            if (!count || count > sizeof(setup) - offset)
                return 4;
            offset += count;
        }
        else if (status != TINYX_ERROR_WOULD_BLOCK) {
            return 5;
        }
        if (tinyx_server_step(server, 1, &step) != TINYX_OK)
            return 6;
    }

    /* Drain a deliberately backpressured setup reply in small chunks. */
    for (iterations = 0; iterations < 1000; iterations++) {
        tinyx_status status;

        if (tinyx_server_step(server, 1, &step) != TINYX_OK)
            return 7;
        status = tinyx_client_receive(client, output, sizeof(output), &count);
        if (status != TINYX_OK || !count)
            continue;
        if (output[0] != 1)
            return 8;
        break;
    }
    if (iterations == 1000)
        return 9;

    if (tinyx_client_shutdown_send(client) != TINYX_OK)
        return 10;
    tinyx_client_destroy(client);
    if (tinyx_server_destroy(server) != TINYX_OK)
        return 11;

    tinyx_config_init(&config);
    config.initial_screen = &screen;
    if (tinyx_server_create(&config, &second, &error) !=
        TINYX_ERROR_ALREADY_EXISTS || second)
        return 12;
    return 0;
}
