#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyx.h"

#define CREATE_WINDOW 1
#define MAP_WINDOW 8
#define CREATE_PIXMAP 53
#define CREATE_GC 55
#define POLY_FILL_RECTANGLE 70
#define CREATE_COLORMAP 78
#define CW_BORDER_PIXEL UINT32_C(0x0008)
#define CW_COLORMAP UINT32_C(0x2000)

static uint16_t
be16(const unsigned char *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t
be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void
put_be16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)value;
}

static void
put_be32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
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
             const unsigned char *request, size_t size)
{
    unsigned char output[32];
    size_t count;

    if (tinyx_client_send(client, request, size, &count) != TINYX_OK ||
        count != size || !pump(server))
        return 0;
    if (!tinyx_client_receive_pending(client))
        return 1;
    if (tinyx_client_receive(client, output, sizeof(output), &count) ==
            TINYX_OK && count >= 2)
        fprintf(stderr, "request %u failed with X error %u\n",
                request[0], output[1]);
    return 0;
}

static int
check_visual(const unsigned char *visual, uint8_t visualClass,
             uint8_t bitsPerRGB, uint16_t entries,
             uint32_t red, uint32_t green, uint32_t blue)
{
    return visual[4] == visualClass && visual[5] == bitsPerRGB &&
        be16(visual + 6) == entries && be32(visual + 8) == red &&
        be32(visual + 12) == green && be32(visual + 16) == blue;
}

static int
create_resources(tinyx_server *server, tinyx_client *client,
                 uint32_t root, uint32_t visual, uint8_t depth,
                 uint32_t *nextId)
{
    unsigned char colormap[16] = {CREATE_COLORMAP, 0};
    unsigned char window[40] = {CREATE_WINDOW, 0};
    unsigned char mapWindow[8] = {MAP_WINDOW, 0};
    unsigned char pixmap[16] = {CREATE_PIXMAP, 0};
    unsigned char gc[16] = {CREATE_GC, 0};
    unsigned char fill[20] = {POLY_FILL_RECTANGLE, 0};
    uint32_t cmap = (*nextId)++;
    uint32_t wid = (*nextId)++;
    uint32_t pid = (*nextId)++;
    uint32_t gcid = (*nextId)++;

    put_be16(colormap + 2, 4);
    put_be32(colormap + 4, cmap);
    put_be32(colormap + 8, root);
    put_be32(colormap + 12, visual);
    if (!send_request(server, client, colormap, sizeof(colormap)))
        return 0;

    window[1] = depth;
    put_be16(window + 2, 10);
    put_be32(window + 4, wid);
    put_be32(window + 8, root);
    put_be16(window + 12, 1);
    put_be16(window + 14, 1);
    put_be16(window + 16, 8);
    put_be16(window + 18, 8);
    put_be16(window + 20, 0);
    put_be16(window + 22, 1); /* InputOutput */
    put_be32(window + 24, visual);
    put_be32(window + 28, CW_BORDER_PIXEL | CW_COLORMAP);
    put_be32(window + 32, 0);
    put_be32(window + 36, cmap);
    if (!send_request(server, client, window, sizeof(window)))
        return 0;

    put_be16(mapWindow + 2, 2);
    put_be32(mapWindow + 4, wid);
    if (!send_request(server, client, mapWindow, sizeof(mapWindow)))
        return 0;

    pixmap[1] = depth;
    put_be16(pixmap + 2, 4);
    put_be32(pixmap + 4, pid);
    put_be32(pixmap + 8, root);
    put_be16(pixmap + 12, 8);
    put_be16(pixmap + 14, 8);
    if (!send_request(server, client, pixmap, sizeof(pixmap)))
        return 0;

    put_be16(gc + 2, 4);
    put_be32(gc + 4, gcid);
    put_be32(gc + 8, pid);
    put_be32(gc + 12, 0);
    if (!send_request(server, client, gc, sizeof(gc)))
        return 0;

    put_be16(fill + 2, 5);
    put_be32(fill + 4, pid);
    put_be32(fill + 8, gcid);
    put_be16(fill + 12, 0);
    put_be16(fill + 14, 0);
    put_be16(fill + 16, 8);
    put_be16(fill + 18, 8);
    if (!send_request(server, client, fill, sizeof(fill)))
        return 0;
    put_be32(fill + 4, wid);
    return send_request(server, client, fill, sizeof(fill));
}

int
main(void)
{
    static const unsigned char setup[12] = {
        'B', 0, 0, 11, 0, 0, 0, 0, 0, 0, 0, 0
    };
    static const tinyx_visual_config depth2Visuals[] = {
        {TINYX_VISUAL_PSEUDO_COLOR, 8, 4, 0, 0, 0}
    };
    static const tinyx_visual_config depth12Visuals[] = {
        {TINYX_VISUAL_PSEUDO_COLOR, 8, 4096, 0, 0, 0}
    };
    static const tinyx_visual_config depth24Visuals[] = {
        {TINYX_VISUAL_TRUE_COLOR, 8, 256,
         UINT32_C(0x00ff0000), UINT32_C(0x0000ff00), UINT32_C(0x000000ff)}
    };
    static const tinyx_depth_config depths[] = {
        {1, 1, NULL, 0},
        {2, 8, depth2Visuals, 1},
        {12, 16, depth12Visuals, 1},
        {24, 32, depth24Visuals, 1}
    };
    tinyx_screen_config screen;
    tinyx_depth_config invalidDepths[sizeof(depths) / sizeof(depths[0])];
    tinyx_config config;
    tinyx_client_config clientConfig;
    tinyx_error error;
    tinyx_framebuffer_info framebuffer;
    tinyx_server *server = NULL;
    tinyx_client *client = NULL;
    unsigned char *reply = NULL;
    const unsigned char *format;
    const unsigned char *rootInfo;
    const unsigned char *depthInfo;
    size_t replySize;
    size_t screenOffset;
    size_t count;
    uint32_t resourceBase;
    uint32_t root;
    uint32_t visual2 = 0, visual12 = 0, visual24 = 0;
    uint32_t nextId;
    int result = 1;
    int stage = 1;

    tinyx_screen_config_init(&screen);
    screen.framebuffer.width = 64;
    screen.framebuffer.height = 64;
    screen.depth_count = sizeof(depths) / sizeof(depths[0]);
    screen.root_depth_index = 3;
    screen.root_visual_index = 0;

    memcpy(invalidDepths, depths, sizeof(depths));
    invalidDepths[1].bits_per_pixel = 2;
    screen.depths = invalidDepths;
    tinyx_config_init(&config);
    config.initial_screen = &screen;
    if (tinyx_server_create(&config, &server, &error) !=
            TINYX_ERROR_INVALID_ARGUMENT || server)
        goto done;

    screen.depths = depths;
    tinyx_config_init(&config);
    config.initial_screen = &screen;
    if (tinyx_server_create(&config, &server, &error) != TINYX_OK)
        goto done;
    if (tinyx_server_get_framebuffer(server, &framebuffer) != TINYX_OK ||
        framebuffer.depth != 24 || framebuffer.bits_per_pixel != 32 ||
        framebuffer.red_mask != UINT32_C(0x00ff0000) ||
        framebuffer.green_mask != UINT32_C(0x0000ff00) ||
        framebuffer.blue_mask != UINT32_C(0x000000ff))
        goto done;
    stage = 2;

    tinyx_client_config_init(&clientConfig);
    if (tinyx_client_open(server, &clientConfig, &client) != TINYX_OK)
        goto done;
    if (tinyx_client_send(client, setup, sizeof(setup), &count) != TINYX_OK ||
        count != sizeof(setup) || !pump(server))
        goto done;

    replySize = tinyx_client_receive_pending(client);
    if (replySize < 40 || !(reply = malloc(replySize)))
        goto done;
    if (tinyx_client_receive(client, reply, replySize, &count) != TINYX_OK ||
        count != replySize || reply[0] != 1 || reply[29] != 4)
        goto done;
    stage = 3;

    resourceBase = be32(reply + 12);
    format = reply + 40 + ((be16(reply + 24) + 3) & ~(size_t)3);
    if (format + 32 > reply + replySize)
        goto done;
    if (format[0] != 1 || format[1] != 1 || format[2] != 32 ||
        format[8] != 2 || format[9] != 8 || format[10] != 32 ||
        format[16] != 12 || format[17] != 16 || format[18] != 32 ||
        format[24] != 24 || format[25] != 32 || format[26] != 32)
        goto done;
    stage = 4;

    screenOffset = (size_t)(format - reply) + 32;
    if (screenOffset + 40 > replySize)
        goto done;
    rootInfo = reply + screenOffset;
    root = be32(rootInfo);
    if (rootInfo[38] != 24 || rootInfo[39] != 4)
        goto done;
    depthInfo = rootInfo + 40;

    if (depthInfo + 8 > reply + replySize || depthInfo[0] != 1 ||
        be16(depthInfo + 2) != 0)
        goto done;
    depthInfo += 8;
    if (depthInfo + 32 > reply + replySize || depthInfo[0] != 2 ||
        be16(depthInfo + 2) != 1 ||
        !check_visual(depthInfo + 8, TINYX_VISUAL_PSEUDO_COLOR, 8, 4,
                      0, 0, 0))
        goto done;
    visual2 = be32(depthInfo + 8);
    depthInfo += 32;
    if (depthInfo + 32 > reply + replySize || depthInfo[0] != 12 ||
        be16(depthInfo + 2) != 1 ||
        !check_visual(depthInfo + 8, TINYX_VISUAL_PSEUDO_COLOR, 8, 4096,
                      0, 0, 0))
        goto done;
    visual12 = be32(depthInfo + 8);
    depthInfo += 32;
    if (depthInfo + 32 > reply + replySize || depthInfo[0] != 24 ||
        be16(depthInfo + 2) != 1 ||
        !check_visual(depthInfo + 8, TINYX_VISUAL_TRUE_COLOR, 8, 256,
                      UINT32_C(0x00ff0000), UINT32_C(0x0000ff00),
                      UINT32_C(0x000000ff)))
        goto done;
    visual24 = be32(depthInfo + 8);
    if (be32(rootInfo + 32) != visual24)
        goto done;
    stage = 5;

    nextId = resourceBase;
    if (!create_resources(server, client, root, visual2, 2, &nextId) ||
        !create_resources(server, client, root, visual12, 12, &nextId) ||
        !create_resources(server, client, root, visual24, 24, &nextId))
        goto done;

    result = 0;
done:
    if (result)
        fprintf(stderr, "visual configuration test failed at stage %d\n", stage);
    free(reply);
    if (client)
        tinyx_client_destroy(client);
    if (server)
        tinyx_server_destroy(server);
    return result;
}
