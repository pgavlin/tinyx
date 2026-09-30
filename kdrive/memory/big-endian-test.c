#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tinyx.h"

static uint16_t
be16(const unsigned char *p)
{
    return (uint16_t) ((p[0] << 8) | p[1]);
}

static uint32_t
be32(const unsigned char *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | p[3];
}

static void
put_be16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char) (value >> 8);
    p[1] = (unsigned char) value;
}

static void
put_be32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char) (value >> 24);
    p[1] = (unsigned char) (value >> 16);
    p[2] = (unsigned char) (value >> 8);
    p[3] = (unsigned char) value;
}

static int
pump(tinyx_server *server)
{
    tinyx_step_result step;
    unsigned int i;

    for (i = 0; i < 1000; i++) {
        if (tinyx_server_step(server, 64, &step) != TINYX_OK)
            return 0;
        if (!step.immediate_work)
            return 1;
    }
    return 0;
}

int
main(void)
{
    static const unsigned char setup[12] = {
        'B', 0, 0, 11, 0, 0, 0, 0, 0, 0, 0, 0
    };
    tinyx_screen_config screen;
    tinyx_config config;
    tinyx_client_config client_config;
    tinyx_error error;
    tinyx_server *server = NULL;
    tinyx_client *client = NULL;
    unsigned char create_gc[16] = {55, 0};
    unsigned char *reply = NULL;
    size_t setup_size;
    size_t screen_offset;
    size_t count;
    uint32_t resource_base;
    uint32_t root;
    int result = 1;

    tinyx_screen_config_init(&screen);
    screen.width = 64;
    screen.height = 64;
    screen.width_mm = 22;
    screen.height_mm = 17;
    tinyx_config_init(&config);
    config.initial_screen = &screen;
    if (tinyx_server_create(&config, &server, &error) != TINYX_OK)
        goto done;

    tinyx_client_config_init(&client_config);
    if (tinyx_client_open(server, &client_config, &client) != TINYX_OK)
        goto done;
    if (tinyx_client_send(client, setup, sizeof(setup), &count) != TINYX_OK ||
        count != sizeof(setup) || !pump(server))
        goto done;

    setup_size = tinyx_client_receive_pending(client);
    if (setup_size < 40 || !(reply = malloc(setup_size)))
        goto done;
    if (tinyx_client_receive(client, reply, setup_size, &count) != TINYX_OK ||
        count != setup_size || reply[0] != 1)
        goto done;

    resource_base = be32(reply + 12);
    screen_offset = 40 + ((be16(reply + 24) + 3) & ~(size_t) 3) + reply[29] * 8;
    if (screen_offset + 28 > setup_size)
        goto done;
    root = be32(reply + screen_offset);
    if (be16(reply + screen_offset + 24) != 22 ||
        be16(reply + screen_offset + 26) != 17)
        goto done;

    put_be16(create_gc + 2, 4);
    put_be32(create_gc + 4, resource_base);
    put_be32(create_gc + 8, root);
    put_be32(create_gc + 12, 0);
    if (tinyx_client_send(client, create_gc, sizeof(create_gc), &count) != TINYX_OK ||
        count != sizeof(create_gc) || !pump(server))
        goto done;
    if (tinyx_client_receive_pending(client) != 0)
        goto done;

    result = 0;
done:
    free(reply);
    if (client)
        tinyx_client_destroy(client);
    if (server)
        tinyx_server_destroy(server);
    return result;
}
