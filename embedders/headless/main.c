/* SPDX-License-Identifier: MIT OR GPL-3.0-only */
/* Minimal headless embedder using only the public TinyX API. */
#include <stdio.h>

#include "tinyx.h"

int
main(void)
{
    tinyx_screen_config screen;
    tinyx_config config;
    tinyx_framebuffer_info framebuffer;
    tinyx_error error;
    tinyx_server *server = NULL;
    tinyx_status status;

    tinyx_screen_config_init(&screen);
    screen.framebuffer.width = 640;
    screen.framebuffer.height = 480;

    tinyx_config_init(&config);
    config.initial_screen = &screen;

    status = tinyx_server_create(&config, &server, &error);
    if (status != TINYX_OK) {
        fprintf(stderr, "TinyX startup failed: %s\n", error.message);
        return 1;
    }
    status = tinyx_server_get_framebuffer(server, &framebuffer);
    if (status != TINYX_OK) {
        (void)tinyx_server_destroy(server);
        return 2;
    }

    printf("TinyX framebuffer: %ux%u, %zu-byte stride\n",
           framebuffer.width, framebuffer.height, framebuffer.stride_bytes);
    return tinyx_server_destroy(server) == TINYX_OK ? 0 : 3;
}
