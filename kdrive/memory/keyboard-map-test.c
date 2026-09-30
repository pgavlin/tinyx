#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyx.h"

#define XK_CONTROL_L 0x0000ffe3U
#define XK_SHIFT_L 0x0000ffe1U
#define XK_C 0x00000063U
#define XK_CAPITAL_C 0x00000043U

static uint16_t
read16(const unsigned char *p, int big_endian)
{
    if (big_endian)
        return (uint16_t) ((p[0] << 8) | p[1]);
    return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t
read32(const unsigned char *p, int big_endian)
{
    if (big_endian)
        return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
               ((uint32_t) p[2] << 8) | p[3];
    return p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}

static void
write16(unsigned char *p, uint16_t value, int big_endian)
{
    if (big_endian) {
        p[0] = (unsigned char) (value >> 8);
        p[1] = (unsigned char) value;
    }
    else {
        p[0] = (unsigned char) value;
        p[1] = (unsigned char) (value >> 8);
    }
}

static void
write32(unsigned char *p, uint32_t value, int big_endian)
{
    if (big_endian) {
        p[0] = (unsigned char) (value >> 24);
        p[1] = (unsigned char) (value >> 16);
        p[2] = (unsigned char) (value >> 8);
        p[3] = (unsigned char) value;
    }
    else {
        p[0] = (unsigned char) value;
        p[1] = (unsigned char) (value >> 8);
        p[2] = (unsigned char) (value >> 16);
        p[3] = (unsigned char) (value >> 24);
    }
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

static int
drain(tinyx_client *client, unsigned char **output, size_t *output_size)
{
    size_t pending = tinyx_client_receive_pending(client);
    size_t count;

    *output = NULL;
    *output_size = 0;
    if (!pending)
        return 0;
    *output = malloc(pending);
    if (!*output)
        return 0;
    if (tinyx_client_receive(client, *output, pending, &count) != TINYX_OK ||
        count != pending) {
        free(*output);
        *output = NULL;
        return 0;
    }
    *output_size = pending;
    return 1;
}

static int
test_client(tinyx_server *server, int big_endian)
{
    unsigned char setup[12] = {0};
    unsigned char get_map[8] = {101, 0, 0, 0, 37, 18, 0, 0};
    unsigned char change_map[16] = {100, 1, 0, 0, 200, 2, 0, 0};
    unsigned char get_changed[8] = {101, 0, 0, 0, 200, 1, 0, 0};
    tinyx_client_config config;
    tinyx_client *client = NULL;
    unsigned char *output = NULL;
    size_t output_size;
    size_t count;
    size_t row;
    int result = 0;

    setup[0] = big_endian ? 'B' : 'l';
    write16(setup + 2, 11, big_endian);
    tinyx_client_config_init(&config);
    if (tinyx_client_open(server, &config, &client) != TINYX_OK)
        goto done;
    if (tinyx_client_send(client, setup, sizeof(setup), &count) != TINYX_OK ||
        count != sizeof(setup) || !pump(server) ||
        !drain(client, &output, &output_size) || output_size < 8 ||
        output[0] != 1)
        goto done;
    free(output);
    output = NULL;

    write16(get_map + 2, 2, big_endian);
    if (tinyx_client_send(client, get_map, sizeof(get_map), &count) != TINYX_OK ||
        count != sizeof(get_map) || !pump(server) ||
        !drain(client, &output, &output_size))
        goto done;
    if (output_size != 32 + 18 * 4 * 4 || output[0] != 1 || output[1] != 4 ||
        read16(output + 2, big_endian) != 1 ||
        read32(output + 4, big_endian) != 18 * 4) {
        fprintf(stderr, "bad header endian=%d size=%lu type=%u width=%u sequence=%u length=%u\n",
                big_endian, (unsigned long) output_size, output[0], output[1],
                read16(output + 2, big_endian), read32(output + 4, big_endian));
        goto done;
    }
    row = 32;
    if (read32(output + row, big_endian) != XK_CONTROL_L) {
        fprintf(stderr, "bad Control_L endian=%d value=%#x\n", big_endian,
                read32(output + row, big_endian));
        goto done;
    }
    row = 32 + (50 - 37) * 4 * 4;
    if (read32(output + row, big_endian) != XK_SHIFT_L) {
        fprintf(stderr, "bad Shift_L endian=%d value=%#x\n", big_endian,
                read32(output + row, big_endian));
        goto done;
    }
    row = 32 + (54 - 37) * 4 * 4;
    if (read32(output + row, big_endian) != XK_C ||
        read32(output + row + 4, big_endian) != XK_CAPITAL_C) {
        fprintf(stderr, "bad c endian=%d values=%#x,%#x\n", big_endian,
                read32(output + row, big_endian),
                read32(output + row + 4, big_endian));
        goto done;
    }
    free(output);
    output = NULL;

    write16(change_map + 2, 4, big_endian);
    write32(change_map + 8, 0x01001234U, big_endian);
    write32(change_map + 12, 0x01005678U, big_endian);
    write16(get_changed + 2, 2, big_endian);
    if (tinyx_client_send(client, change_map, sizeof(change_map), &count) !=
            TINYX_OK || count != sizeof(change_map) ||
        tinyx_client_send(client, get_changed, sizeof(get_changed), &count) !=
            TINYX_OK || count != sizeof(get_changed) || !pump(server) ||
        !drain(client, &output, &output_size))
        goto done;
    if (output_size != 32 + 32 + 4 * 4 || output[0] != 34 ||
        output[32] != 1 || output[33] != 4 ||
        read16(output + 34, big_endian) != 3 ||
        read32(output + 64, big_endian) != 0x01001234U ||
        read32(output + 68, big_endian) != 0x01005678U ||
        read32(output + 72, big_endian) != 0 ||
        read32(output + 76, big_endian) != 0) {
        fprintf(stderr, "bad changed map endian=%d size=%lu event=%u type=%u width=%u sequence=%u values=%#x,%#x,%#x,%#x\n",
                big_endian, (unsigned long) output_size, output[0], output[32],
                output[33], read16(output + 34, big_endian),
                read32(output + 64, big_endian),
                read32(output + 68, big_endian),
                read32(output + 72, big_endian),
                read32(output + 76, big_endian));
        goto done;
    }

    result = 1;
done:
    free(output);
    if (client)
        tinyx_client_destroy(client);
    return result;
}

int
main(void)
{
    tinyx_screen_config screen;
    tinyx_config config;
    tinyx_error error;
    tinyx_server *server = NULL;
    int result = 1;

    tinyx_screen_config_init(&screen);
    screen.width = 64;
    screen.height = 64;
    tinyx_config_init(&config);
    config.initial_screen = &screen;
    if (tinyx_server_create(&config, &server, &error) != TINYX_OK)
        return 1;
    if (!test_client(server, 0) || !test_client(server, 1))
        goto done;
    result = 0;
done:
    if (server)
        tinyx_server_destroy(server);
    return result;
}
