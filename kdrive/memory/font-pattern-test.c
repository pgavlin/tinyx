#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyx.h"

#define OPEN_FONT_REQUEST 45
#define QUERY_FONT_REQUEST 47

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
    static const char *patterns[] = {
        "-*-*-*-R-*-*-*-120-*-*-*-*-ISO8859-1",
        "-*-helvetica-bold-o-normal-*-14*iso8859-1",
        "-*-helvetica-medium-r-normal-*-14*iso8859-1",
        "-*-helvetica-bold-r-normal-*-14-*-*-*-*-*-iso8859-1",
        "-*-lucidatypewriter-medium-r-normal-*-14*iso8859-1",
        "-*-helvetica-medium-r-normal-*-20*iso8859-1"
    };
    tinyx_screen_config screen;
    tinyx_config config;
    tinyx_client_config client_config;
    tinyx_error error;
    tinyx_server *server = NULL;
    tinyx_client *client = NULL;
    unsigned char query_font[8] = { QUERY_FONT_REQUEST, 0 };
    unsigned char *open_font = NULL;
    unsigned char *reply = NULL;
    size_t pattern_length;
    size_t open_size;
    size_t setup_size;
    size_t count;
    size_t i;
    uint32_t font_id;
    int result = 1;

    tinyx_screen_config_init(&screen);
    screen.framebuffer.width = 64;
    screen.framebuffer.height = 64;
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
    font_id = be32(reply + 12);
    free(reply);
    reply = NULL;

    pattern_length = strlen(patterns[0]);
    open_size = 12 + ((pattern_length + 3) & ~(size_t) 3);
    open_font = calloc(open_size, 1);
    if (!open_font)
        goto done;
    open_font[0] = OPEN_FONT_REQUEST;
    put_be16(open_font + 2, (uint16_t) (open_size / 4));
    put_be32(open_font + 4, font_id);
    put_be16(open_font + 8, (uint16_t) pattern_length);
    memcpy(open_font + 12, patterns[0], pattern_length);
    if (tinyx_client_send(client, open_font, open_size, &count) != TINYX_OK ||
        count != open_size || !pump(server))
        goto done;
    free(open_font);
    open_font = NULL;
    if (tinyx_client_receive_pending(client) != 0)
        goto done;

    put_be16(query_font + 2, 2);
    put_be32(query_font + 4, font_id);
    if (tinyx_client_send(client, query_font, sizeof(query_font), &count) !=
            TINYX_OK ||
        count != sizeof(query_font) || !pump(server))
        goto done;
    count = tinyx_client_receive_pending(client);
    if (count < 60 || !(reply = malloc(count)))
        goto done;
    if (tinyx_client_receive(client, reply, count, &setup_size) != TINYX_OK ||
        setup_size != count || reply[0] != 1 || be16(reply + 2) != 2)
        goto done;
    free(reply);
    reply = NULL;

    /* Batch the authentic Xt patterns so one dispatch cycle proves every OpenFont. */
    for (i = 1; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        pattern_length = strlen(patterns[i]);
        open_size = 12 + ((pattern_length + 3) & ~(size_t) 3);
        open_font = calloc(open_size, 1);
        if (!open_font)
            goto done;
        open_font[0] = OPEN_FONT_REQUEST;
        put_be16(open_font + 2, (uint16_t) (open_size / 4));
        put_be32(open_font + 4, font_id + (uint32_t) i);
        put_be16(open_font + 8, (uint16_t) pattern_length);
        memcpy(open_font + 12, patterns[i], pattern_length);
        if (tinyx_client_send(client, open_font, open_size, &count) != TINYX_OK ||
            count != open_size)
            goto done;
        free(open_font);
        open_font = NULL;
    }
    if (!pump(server))
        goto done;

    /* OpenFont has no reply, so any output here is an X11 error. */
    if ((count = tinyx_client_receive_pending(client)) != 0) {
        unsigned char failure[32];
        size_t failure_count = 0;
        tinyx_client_receive(client, failure, sizeof(failure), &failure_count);
        fprintf(stderr, "OpenFont failed: error %u, sequence %u\n",
                failure_count > 1 ? failure[1] : 255,
                failure_count > 3 ? (failure[2] << 8) | failure[3] : 0);
        goto done;
    }

    result = 0;
done:
    free(open_font);
    free(reply);
    if (client)
        tinyx_client_destroy(client);
    if (server)
        tinyx_server_destroy(server);
    return result;
}
