/*
 * Copyright (c) 2026 TinyX contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyx.h"

#define X_QUERY_EXTENSION 98
#define XI_GET_EXTENSION_VERSION 1
#define XI_LIST_INPUT_DEVICES 2
#define XI_OPEN_DEVICE 3
#define XI_LIST_DEVICE_PROPERTIES 36
#define XI2_QUERY_VERSION 47

static void
put16(unsigned char *p, uint16_t value, int msb)
{
    p[msb ? 0 : 1] = (unsigned char) (value >> 8);
    p[msb ? 1 : 0] = (unsigned char) value;
}

static uint16_t
get16(const unsigned char *p, int msb)
{
    return msb ? (uint16_t) ((p[0] << 8) | p[1])
               : (uint16_t) ((p[1] << 8) | p[0]);
}

static uint32_t
get32(const unsigned char *p, int msb)
{
    if (msb)
        return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
               ((uint32_t) p[2] << 8) | p[3];
    return ((uint32_t) p[3] << 24) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[1] << 8) | p[0];
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
send_request(tinyx_server *server, tinyx_client *client,
             const unsigned char *request, size_t request_size,
             unsigned char **reply, size_t *reply_size)
{
    size_t count;

    *reply = NULL;
    *reply_size = 0;
    if (tinyx_client_receive_pending(client) != 0)
        return 0;
    if (tinyx_client_send(client, request, request_size, &count) != TINYX_OK ||
        count != request_size || !pump(server))
        return 0;
    *reply_size = tinyx_client_receive_pending(client);
    if (*reply_size == 0 || !(*reply = malloc(*reply_size)))
        return 0;
    if (tinyx_client_receive(client, *reply, *reply_size, &count) != TINYX_OK ||
        count != *reply_size) {
        free(*reply);
        *reply = NULL;
        return 0;
    }
    return 1;
}

static int
connect_client(tinyx_server *server, int msb, tinyx_client **client)
{
    unsigned char setup[12] = {0};
    tinyx_client_config config;
    unsigned char *reply;
    size_t reply_size;

    setup[0] = msb ? 'B' : 'l';
    put16(setup + 2, 11, msb);
    tinyx_client_config_init(&config);
    if (tinyx_client_open(server, &config, client) != TINYX_OK)
        return 0;
    if (!send_request(server, *client, setup, sizeof(setup), &reply,
                      &reply_size))
        return 0;
    if (reply_size < 40 || reply[0] != 1) {
        free(reply);
        return 0;
    }
    free(reply);
    return 1;
}

static int
expect_bad_request(tinyx_server *server, tinyx_client *client,
                   unsigned char opcode, unsigned char minor, int msb)
{
    unsigned char request[4] = {opcode, minor, 0, 0};
    unsigned char *reply;
    size_t reply_size;
    int ok;

    put16(request + 2, 1, msb);
    if (!send_request(server, client, request, sizeof(request), &reply,
                      &reply_size))
        return 0;
    ok = reply_size == 32 && reply[0] == 0 && reply[1] == 1 &&
         reply[10] == opcode && get16(reply + 8, msb) == minor;
    if (!ok)
        fprintf(stderr, "opcode %u response: size=%zu type=%u error=%u "
                "major=%u minor=%u\n", minor, reply_size, reply[0], reply[1],
                reply[10], get16(reply + 8, msb));
    free(reply);
    return ok;
}

static int
exercise_client(tinyx_server *server, tinyx_client *client, int msb)
{
    static const char extension_name[] = "XInputExtension";
    unsigned char query[24] = {X_QUERY_EXTENSION, 0};
    unsigned char version[24] = {0, XI_GET_EXTENSION_VERSION};
    unsigned char list[4] = {0, XI_LIST_INPUT_DEVICES};
    unsigned char open[8] = {0, XI_OPEN_DEVICE};
    unsigned char *reply = NULL;
    size_t reply_size;
    unsigned char opcode;
    unsigned char bad_device;
    unsigned char pointer_id = 0xff;
    unsigned char keyboard_id = 0xff;
    unsigned int i;
    int ok = 0;
    int stage = 1;

    put16(query + 2, 6, msb);
    put16(query + 4, sizeof(extension_name) - 1, msb);
    memcpy(query + 8, extension_name, sizeof(extension_name) - 1);
    if (!send_request(server, client, query, sizeof(query), &reply,
                      &reply_size) || reply_size != 32 || reply[0] != 1 ||
        !reply[8])
        goto done;
    opcode = reply[9];
    bad_device = reply[11];
    if (opcode < 128 || reply[10] == 0 || bad_device == 0)
        goto done;
    free(reply);
    reply = NULL;

    stage = 2;
    version[0] = opcode;
    put16(version + 2, 6, msb);
    put16(version + 4, sizeof(extension_name) - 1, msb);
    memcpy(version + 8, extension_name, sizeof(extension_name) - 1);
    if (!send_request(server, client, version, sizeof(version), &reply,
                      &reply_size) || reply_size != 32 || reply[0] != 1 ||
        reply[1] != XI_GET_EXTENSION_VERSION || get16(reply + 8, msb) != 1 ||
        get16(reply + 10, msb) != 3 || !reply[12])
        goto done;
    free(reply);
    reply = NULL;

    stage = 3;
    list[0] = opcode;
    put16(list + 2, 1, msb);
    if (!send_request(server, client, list, sizeof(list), &reply,
                      &reply_size) || reply_size < 48 || reply[0] != 1 ||
        reply[1] != XI_LIST_INPUT_DEVICES || reply[8] != 2 ||
        reply_size != 32 + (size_t) get32(reply + 4, msb) * 4)
        goto done;
    for (i = 0; i < reply[8]; i++) {
        const unsigned char *device = reply + 32 + i * 8;
        if (device[6] == 0)
            pointer_id = device[4];
        else if (device[6] == 1)
            keyboard_id = device[4];
    }
    if (pointer_id == 0xff || keyboard_id == 0xff ||
        pointer_id == keyboard_id)
        goto done;
    free(reply);
    reply = NULL;

    stage = 4;
    /* Core devices are enumerated by XI 1.x but cannot be opened as
       extension devices. */
    open[0] = opcode;
    open[4] = pointer_id;
    put16(open + 2, 2, msb);
    if (!send_request(server, client, open, sizeof(open), &reply,
                      &reply_size) || reply_size != 32 || reply[0] != 0 ||
        reply[1] != bad_device || reply[10] != opcode ||
        get16(reply + 8, msb) != XI_OPEN_DEVICE)
        goto done;
    free(reply);
    reply = NULL;

    stage = 5;
    /* XI 1.5 properties and the first XI2 request are intentionally absent. */
    if (!expect_bad_request(server, client, opcode,
                            XI_LIST_DEVICE_PROPERTIES, msb) ||
        !expect_bad_request(server, client, opcode, XI2_QUERY_VERSION, msb))
        goto done;

    ok = 1;
done:
    if (!ok)
        fprintf(stderr, "XInput protocol test failed at stage %d (%s-endian)\n",
                stage, msb ? "big" : "little");
    free(reply);
    return ok;
}

int
main(void)
{
    tinyx_screen_config screen;
    tinyx_config config;
    tinyx_error error;
    tinyx_server *server = NULL;
    tinyx_client *little = NULL;
    tinyx_client *big = NULL;
    int result = 1;

    tinyx_screen_config_init(&screen);
    screen.framebuffer.width = 64;
    screen.framebuffer.height = 64;
    tinyx_config_init(&config);
    config.initial_screen = &screen;
    if (tinyx_server_create(&config, &server, &error) != TINYX_OK)
        goto done;
    if (!connect_client(server, 0, &little) ||
        !connect_client(server, 1, &big) ||
        !exercise_client(server, little, 0) ||
        !exercise_client(server, big, 1))
        goto done;
    result = 0;
done:
    if (little)
        tinyx_client_destroy(little);
    if (big)
        tinyx_client_destroy(big);
    if (server)
        tinyx_server_destroy(server);
    return result;
}
