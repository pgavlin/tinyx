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
    tinyx_framebuffer_info framebuffer;
    tinyx_damage_rect damage;
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

    /* A successful resize replaces the framebuffer and fully damages it. */
    while (tinyx_server_take_damage(server, &damage, 1, &count) == TINYX_OK &&
           count != 0)
        ;
    screen.width = 96;
    screen.height = 48;
    if (tinyx_server_resize(server, &screen) != TINYX_OK)
        return 10;
    if (tinyx_server_get_framebuffer(server, &framebuffer) != TINYX_OK ||
        framebuffer.width != 96 || framebuffer.height != 48 ||
        framebuffer.stride_bytes < 96 * 4)
        return 11;
    if (tinyx_server_take_damage(server, &damage, 1, &count) != TINYX_OK ||
        count != 1 || damage.x != 0 || damage.y != 0 ||
        damage.width != 96 || damage.height != 48)
        return 12;

    /* Invalid resize input must leave the active framebuffer unchanged. */
    screen.width = 0;
    if (tinyx_server_resize(server, &screen) != TINYX_ERROR_INVALID_ARGUMENT)
        return 13;
    if (tinyx_server_get_framebuffer(server, &framebuffer) != TINYX_OK ||
        framebuffer.width != 96 || framebuffer.height != 48)
        return 14;

    if (tinyx_client_shutdown_send(client) != TINYX_OK)
        return 15;
    tinyx_client_destroy(client);
    if (tinyx_server_destroy(server) != TINYX_OK)
        return 16;

    screen.width = 64;
    tinyx_config_init(&config);
    config.initial_screen = &screen;
    if (tinyx_server_create(&config, &second, &error) !=
        TINYX_ERROR_ALREADY_EXISTS || second)
        return 17;
    return 0;
}
