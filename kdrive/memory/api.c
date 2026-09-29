#ifdef HAVE_CONFIG_H
#include <kdrive-config.h>
#endif

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"
#include "tinyx.h"
#include "tinyx-display.h"
#include "tinyx-host.h"
#include "tinyx-input.h"
#include "tinyx-memory.h"

#define TINYX_DEFAULT_WIDTH 1024U
#define TINYX_DEFAULT_HEIGHT 768U
#define TINYX_DEFAULT_CLIENT_QUEUE (1024U * 1024U)

struct tinyx_client {
    struct tinyx_server *server;
    TinyXMemoryClient *transport;
    struct tinyx_client *next;
    int sendShutdown;
};

struct tinyx_server {
    tinyx_host_ops host;
    void *hostData;
    struct tinyx_client *clients;
    char lastError[1024];
    int inCall;
    int running;
    int generationFinished;
};

static tinyx_server *activeServer;
static int serverLifetimeUsed;

static void
ClearError(tinyx_error *error)
{
    if (error) {
        memset(error, 0, sizeof(*error));
        error->status = TINYX_OK;
    }
}

static void
SetError(tinyx_error *error, tinyx_status status, const char *message)
{
    if (!error)
        return;
    memset(error, 0, sizeof(*error));
    error->status = status;
    if (message)
        snprintf(error->message, sizeof(error->message), "%s", message);
}

void
tinyx_screen_config_init(tinyx_screen_config *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->width = TINYX_DEFAULT_WIDTH;
    config->height = TINYX_DEFAULT_HEIGHT;
}

void
tinyx_config_init(tinyx_config *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->api_version_major = TINYX_API_VERSION_MAJOR;
    config->api_version_minor = TINYX_API_VERSION_MINOR;
    config->host.struct_size = sizeof(config->host);
}

void
tinyx_client_config_init(tinyx_client_config *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->input_buffer_limit = TINYX_DEFAULT_CLIENT_QUEUE;
    config->output_buffer_limit = TINYX_DEFAULT_CLIENT_QUEUE;
}

static uint32_t
HostTime(void *userdata)
{
    tinyx_server *server = userdata;
    return server->host.monotonic_time_ms(server->hostData);
}

static void
HostLog(void *userdata, TinyXHostLogLevel level, const char *message)
{
    tinyx_server *server = userdata;
    tinyx_log_level publicLevel = TINYX_LOG_ERROR;

    if (!server->host.log)
        return;
    if (level == TINYX_HOST_LOG_AUDIT)
        publicLevel = TINYX_LOG_AUDIT;
    else if (level == TINYX_HOST_LOG_FATAL)
        publicLevel = TINYX_LOG_FATAL;
    server->host.log(server->hostData, publicLevel, message);
}

static void
HostWakeup(void *userdata)
{
    tinyx_server *server = userdata;
    if (server->host.wakeup)
        server->host.wakeup(server->hostData);
}

static void
HostLeds(void *userdata, uint32_t leds)
{
    tinyx_server *server = userdata;
    if (server->host.leds_changed)
        server->host.leds_changed(server->hostData, leds);
}

static void
HostBell(void *userdata, int volume, int pitch, int duration)
{
    tinyx_server *server = userdata;
    if (server->host.bell)
        server->host.bell(server->hostData, volume, pitch, duration);
}

static int
ValidateScreen(const tinyx_screen_config *screen)
{
    size_t stride;

    if (!screen || screen->struct_size < sizeof(*screen) ||
        !screen->width || !screen->height || screen->width > INT_MAX ||
        screen->height > INT_MAX ||
        (uint64_t)screen->width * 4 > (uint64_t)SIZE_MAX)
        return 0;
    stride = screen->stride_bytes ? screen->stride_bytes :
        (size_t)screen->width * 4;
    if (stride < (size_t)screen->width * 4 || stride > INT_MAX ||
        (stride & 3) || screen->height > SIZE_MAX / stride)
        return 0;
    if (screen->pixels && screen->pixels_size < stride * screen->height)
        return 0;
    return 1;
}

static int
ValidateConfig(const tinyx_config *config)
{
    return config && config->struct_size >= sizeof(*config) &&
        config->api_version_major == TINYX_API_VERSION_MAJOR &&
        config->api_version_minor <= TINYX_API_VERSION_MINOR &&
        config->host.struct_size >= sizeof(config->host) &&
        ValidateScreen(config->initial_screen);
}

typedef struct {
    int argc;
    char **argv;
    char **envp;
} InitializeClosure;

static void
InitializeServer(void *data)
{
    InitializeClosure *closure = data;
    TinyXServerInitialize(closure->argc, closure->argv, closure->envp);
    TinyXServerInitializeGeneration(closure->argc, closure->argv);
}

static void
RememberFatal(tinyx_server *server)
{
    const char *message = TinyXHostLastError();

    if (!message)
        message = "fatal server error";
    snprintf(server->lastError, sizeof(server->lastError), "%s", message);
    server->running = 0;
}

tinyx_status
tinyx_server_create(const tinyx_config *config, tinyx_server **outServer,
                    tinyx_error *error)
{
    tinyx_server *server;
    TinyXHostOps hostOps;
    TinyXInputHostOps inputOps;
    TinyXMemoryDisplayConfig display;
    InitializeClosure initialize;
    char programName[] = "tinyx";
    char *argv[] = { programName, NULL };
    char *envp[] = { NULL };

    ClearError(error);
    if (outServer)
        *outServer = NULL;
    if (!outServer || !ValidateConfig(config)) {
        SetError(error, TINYX_ERROR_INVALID_ARGUMENT,
                 "invalid TinyX configuration");
        return TINYX_ERROR_INVALID_ARGUMENT;
    }
    if (serverLifetimeUsed || activeServer) {
        SetError(error, TINYX_ERROR_ALREADY_EXISTS,
                 "TinyX permits one server lifetime per process");
        return TINYX_ERROR_ALREADY_EXISTS;
    }

    server = calloc(1, sizeof(*server));
    if (!server) {
        SetError(error, TINYX_ERROR_OUT_OF_MEMORY, "out of memory");
        return TINYX_ERROR_OUT_OF_MEMORY;
    }
    server->host = config->host;
    server->hostData = config->host_userdata;
    server->inCall = 1;

    memset(&hostOps, 0, sizeof(hostOps));
    if (server->host.monotonic_time_ms)
        hostOps.monotonicTimeMillis = HostTime;
    if (server->host.log)
        hostOps.log = HostLog;
    if (server->host.wakeup)
        hostOps.wakeup = HostWakeup;
    if (!TinyXHostSetOps(&hostOps, server)) {
        free(server);
        SetError(error, TINYX_ERROR_INVALID_STATE,
                 "host runtime cannot be configured");
        return TINYX_ERROR_INVALID_STATE;
    }

    memset(&inputOps, 0, sizeof(inputOps));
    if (server->host.leds_changed)
        inputOps.ledsChanged = HostLeds;
    if (server->host.bell)
        inputOps.bell = HostBell;
    if (!TinyXInputSetHostOps(&inputOps, server)) {
        (void)TinyXHostSetOps(NULL, NULL);
        free(server);
        SetError(error, TINYX_ERROR_INVALID_STATE,
                 "input host cannot be configured");
        return TINYX_ERROR_INVALID_STATE;
    }

    memset(&display, 0, sizeof(display));
    display.width = config->initial_screen->width;
    display.height = config->initial_screen->height;
    display.strideBytes = config->initial_screen->stride_bytes;
    display.pixels = config->initial_screen->pixels;
    display.pixelsSize = config->initial_screen->pixels_size;
    if (!TinyXMemoryDisplayConfigure(&display)) {
        (void)TinyXInputSetHostOps(NULL, NULL);
        (void)TinyXHostSetOps(NULL, NULL);
        free(server);
        SetError(error, TINYX_ERROR_INVALID_ARGUMENT,
                 "memory display cannot be configured");
        return TINYX_ERROR_INVALID_ARGUMENT;
    }

    activeServer = server;
    serverLifetimeUsed = 1;
    initialize.argc = 1;
    initialize.argv = argv;
    initialize.envp = envp;
    if (!TinyXHostRunProtected(InitializeServer, &initialize)) {
        RememberFatal(server);
        SetError(error, TINYX_ERROR_FATAL, server->lastError);
        TinyXInputClearHostOpsAfterFatal();
        TinyXHostClearCallbacksAfterFatal();
        activeServer = NULL;
        free(server);
        return TINYX_ERROR_FATAL;
    }

    server->inCall = 0;
    server->running = 1;
    *outServer = server;
    return TINYX_OK;
}

static tinyx_status
BeginCall(tinyx_server *server, int requireRunning)
{
    if (!server || server != activeServer)
        return TINYX_ERROR_INVALID_ARGUMENT;
    if (server->inCall)
        return TINYX_ERROR_INVALID_STATE;
    if (TinyXHostIsPoisoned())
        return TINYX_ERROR_POISONED;
    if (requireRunning && (!server->running || server->generationFinished))
        return TINYX_ERROR_INVALID_STATE;
    server->inCall = 1;
    return TINYX_OK;
}

static tinyx_status
RunProtected(tinyx_server *server, TinyXHostOperation operation, void *closure)
{
    if (!TinyXHostRunProtected(operation, closure)) {
        server->inCall = 0;
        RememberFatal(server);
        return TINYX_ERROR_FATAL;
    }
    server->inCall = 0;
    return TINYX_OK;
}

typedef struct {
    unsigned int budget;
    TinyXServerStepResult result;
} StepClosure;

static void
StepServer(void *data)
{
    StepClosure *closure = data;
    TinyXServerStep(closure->budget, &closure->result);
}

tinyx_status
tinyx_server_step(tinyx_server *server, uint32_t requestBudget,
                  tinyx_step_result *result)
{
    tinyx_status status;
    StepClosure closure;

    if (!result || !requestBudget)
        return TINYX_ERROR_INVALID_ARGUMENT;
    status = BeginCall(server, 1);
    if (status != TINYX_OK)
        return status;
    memset(&closure, 0, sizeof(closure));
    closure.budget = requestBudget;
    status = RunProtected(server, StepServer, &closure);
    if (status != TINYX_OK)
        return status;

    result->requests_processed = closure.result.requestsProcessed;
    result->immediate_work = closure.result.immediateWork;
    result->generation_finished = closure.result.generationFinished;
    result->next_timeout_ms = closure.result.nextTimeoutMillis < 0 ?
        TINYX_NO_TIMEOUT : (uint32_t)closure.result.nextTimeoutMillis;
    if (result->generation_finished) {
        server->generationFinished = 1;
        server->running = 0;
    }
    return TINYX_OK;
}

static void
ShutdownServer(void *data)
{
    (void)data;
    TinyXServerRequestTermination();
    (void)TinyXServerCloseGeneration();
    TinyXServerShutdown();
}

static void
FreeClientHandles(tinyx_server *server, int abandon)
{
    tinyx_client *client = server->clients;

    while (client) {
        tinyx_client *next = client->next;
        if (abandon)
            TinyXMemoryClientAbandon(client->transport);
        else
            TinyXMemoryClientDestroy(client->transport);
        free(client);
        client = next;
    }
    server->clients = NULL;
}

tinyx_status
tinyx_server_destroy(tinyx_server *server)
{
    tinyx_status status = TINYX_OK;
    int poisoned;

    if (!server || server != activeServer)
        return TINYX_ERROR_INVALID_ARGUMENT;
    if (server->inCall)
        return TINYX_ERROR_INVALID_STATE;

    poisoned = TinyXHostIsPoisoned();
    if (!poisoned) {
        server->inCall = 1;
        status = RunProtected(server, ShutdownServer, NULL);
        poisoned = status != TINYX_OK;
    }
    FreeClientHandles(server, poisoned);
    if (!poisoned) {
        (void)TinyXInputSetHostOps(NULL, NULL);
        (void)TinyXHostSetOps(NULL, NULL);
    }
    else {
        TinyXInputClearHostOpsAfterFatal();
        TinyXHostClearCallbacksAfterFatal();
    }
    activeServer = NULL;
    free(server);
    return poisoned && status == TINYX_OK ? TINYX_ERROR_POISONED : status;
}

const char *
tinyx_server_last_error(const tinyx_server *server)
{
    if (!server || server != activeServer || server->inCall ||
        !server->lastError[0])
        return NULL;
    return server->lastError;
}

typedef struct {
    size_t inputLimit;
    size_t outputLimit;
    TinyXMemoryClient *transport;
} OpenClientClosure;

static void
OpenClient(void *data)
{
    OpenClientClosure *closure = data;
    closure->transport = TinyXMemoryClientOpenWithLimits(closure->inputLimit,
                                                         closure->outputLimit);
}

tinyx_status
tinyx_client_open(tinyx_server *server, const tinyx_client_config *config,
                  tinyx_client **outClient)
{
    tinyx_client_config defaults;
    tinyx_client *client;
    OpenClientClosure closure;
    tinyx_status status;

    if (outClient)
        *outClient = NULL;
    if (!outClient)
        return TINYX_ERROR_INVALID_ARGUMENT;
    if (!config) {
        tinyx_client_config_init(&defaults);
        config = &defaults;
    }
    if (config->struct_size < sizeof(*config))
        return TINYX_ERROR_INVALID_ARGUMENT;

    closure.inputLimit = config->input_buffer_limit ?
        config->input_buffer_limit : TINYX_DEFAULT_CLIENT_QUEUE;
    closure.outputLimit = config->output_buffer_limit ?
        config->output_buffer_limit : TINYX_DEFAULT_CLIENT_QUEUE;
    closure.transport = NULL;

    client = calloc(1, sizeof(*client));
    if (!client)
        return TINYX_ERROR_OUT_OF_MEMORY;
    status = BeginCall(server, 1);
    if (status != TINYX_OK) {
        free(client);
        return status;
    }
    status = RunProtected(server, OpenClient, &closure);
    if (status != TINYX_OK) {
        free(client);
        return status;
    }
    if (!closure.transport) {
        free(client);
        return TINYX_ERROR_OUT_OF_MEMORY;
    }
    client->server = server;
    client->transport = closure.transport;
    client->next = server->clients;
    server->clients = client;
    *outClient = client;
    return TINYX_OK;
}

static tinyx_status
BeginClientCall(tinyx_client *client)
{
    if (!client || !client->server || client->server != activeServer)
        return TINYX_ERROR_INVALID_ARGUMENT;
    return BeginCall(client->server, 1);
}

tinyx_status
tinyx_client_send(tinyx_client *client, const void *bytes, size_t length,
                  size_t *outLength)
{
    tinyx_status status;

    if (outLength)
        *outLength = 0;
    if (!outLength || (length && !bytes))
        return TINYX_ERROR_INVALID_ARGUMENT;
    status = BeginClientCall(client);
    if (status != TINYX_OK)
        return status;
    if (client->sendShutdown || TinyXMemoryClientIsClosed(client->transport)) {
        client->server->inCall = 0;
        return TINYX_ERROR_CLOSED;
    }
    if (!TinyXMemoryClientFeedPartial(client->transport, bytes, length,
                                      outLength)) {
        client->server->inCall = 0;
        return TinyXMemoryClientIsClosed(client->transport) ?
            TINYX_ERROR_CLOSED : TINYX_ERROR_OUT_OF_MEMORY;
    }
    client->server->inCall = 0;
    if (length && !*outLength)
        return TINYX_ERROR_WOULD_BLOCK;
    return TINYX_OK;
}

tinyx_status
tinyx_client_receive(tinyx_client *client, void *bytes, size_t capacity,
                     size_t *outLength)
{
    tinyx_status status;

    if (outLength)
        *outLength = 0;
    if (!outLength || (capacity && !bytes))
        return TINYX_ERROR_INVALID_ARGUMENT;
    if (!client || !client->server || client->server != activeServer)
        return TINYX_ERROR_INVALID_ARGUMENT;
    status = BeginCall(client->server, 0);
    if (status != TINYX_OK)
        return status;
    *outLength = TinyXMemoryClientDrain(client->transport, bytes, capacity);
    client->server->inCall = 0;
    if (!*outLength && TinyXMemoryClientIsClosed(client->transport) &&
        !TinyXMemoryClientOutputPending(client->transport))
        return TINYX_ERROR_CLOSED;
    return TINYX_OK;
}

size_t
tinyx_client_receive_pending(const tinyx_client *client)
{
    if (!client || !client->server || client->server != activeServer ||
        client->server->inCall)
        return 0;
    return TinyXMemoryClientOutputPending(client->transport);
}

tinyx_status
tinyx_client_shutdown_send(tinyx_client *client)
{
    tinyx_status status = BeginClientCall(client);

    if (status != TINYX_OK)
        return status;
    if (!client->sendShutdown) {
        TinyXMemoryClientCloseInput(client->transport);
        client->sendShutdown = 1;
    }
    client->server->inCall = 0;
    return TINYX_OK;
}

int
tinyx_client_is_closed(const tinyx_client *client)
{
    return !client || !client->server || client->server != activeServer ||
        TinyXMemoryClientIsClosed(client->transport);
}

typedef struct {
    TinyXMemoryClient *transport;
} DestroyClientClosure;

static void
DestroyClient(void *data)
{
    DestroyClientClosure *closure = data;
    TinyXMemoryClientDestroy(closure->transport);
}

void
tinyx_client_destroy(tinyx_client *client)
{
    tinyx_server *server;
    tinyx_client **link;
    DestroyClientClosure closure;
    tinyx_status status;

    if (!client || !client->server || client->server != activeServer)
        return;
    server = client->server;
    status = BeginCall(server, 0);
    if (status != TINYX_OK)
        return;

    link = &server->clients;
    while (*link && *link != client)
        link = &(*link)->next;
    if (*link == client)
        *link = client->next;

    closure.transport = client->transport;
    status = RunProtected(server, DestroyClient, &closure);
    if (status != TINYX_OK)
        TinyXMemoryClientAbandon(client->transport);
    free(client);
}

tinyx_status
tinyx_server_get_framebuffer(tinyx_server *server,
                             tinyx_framebuffer_info *info)
{
    TinyXMemoryFramebufferInfo internal;
    tinyx_status status;
    const uint16_t endian = 1;

    if (!info)
        return TINYX_ERROR_INVALID_ARGUMENT;
    status = BeginCall(server, 0);
    if (status != TINYX_OK)
        return status;
    if (!TinyXMemoryDisplayGetFramebuffer(&internal)) {
        server->inCall = 0;
        return TINYX_ERROR_INVALID_STATE;
    }
    info->pixels = internal.pixels;
    info->width = internal.width;
    info->height = internal.height;
    info->stride_bytes = internal.strideBytes;
    info->size = internal.strideBytes * internal.height;
    info->depth = internal.depth;
    info->bits_per_pixel = internal.bitsPerPixel;
    info->red_mask = internal.redMask;
    info->green_mask = internal.greenMask;
    info->blue_mask = internal.blueMask;
    info->byte_order = *(const uint8_t *)&endian ?
        TINYX_BYTE_ORDER_LSB_FIRST : TINYX_BYTE_ORDER_MSB_FIRST;
    server->inCall = 0;
    return TINYX_OK;
}

tinyx_status
tinyx_server_take_damage(tinyx_server *server, tinyx_damage_rect *rects,
                         size_t capacity, size_t *outCount)
{
    TinyXDamageRect *internal = NULL;
    tinyx_status status;
    size_t count, i;

    if (outCount)
        *outCount = 0;
    if (!outCount || (capacity && !rects) ||
        capacity > SIZE_MAX / sizeof(*internal))
        return TINYX_ERROR_INVALID_ARGUMENT;
    status = BeginCall(server, 0);
    if (status != TINYX_OK)
        return status;
    if (capacity) {
        internal = malloc(capacity * sizeof(*internal));
        if (!internal) {
            server->inCall = 0;
            return TINYX_ERROR_OUT_OF_MEMORY;
        }
    }
    count = TinyXMemoryDisplayTakeDamage(internal, capacity);
    for (i = 0; i < count; i++) {
        rects[i].x = internal[i].x;
        rects[i].y = internal[i].y;
        rects[i].width = internal[i].width;
        rects[i].height = internal[i].height;
    }
    free(internal);
    *outCount = count;
    server->inCall = 0;
    return TINYX_OK;
}

typedef enum {
    INPUT_ABSOLUTE,
    INPUT_RELATIVE,
    INPUT_BUTTON,
    INPUT_KEY,
    INPUT_RELEASE_KEYS
} InputOperation;

typedef struct {
    InputOperation operation;
    int32_t x;
    int32_t y;
    uint32_t detail;
    int pressed;
    int accepted;
} InputClosure;

static void
InjectInput(void *data)
{
    InputClosure *closure = data;

    switch (closure->operation) {
    case INPUT_ABSOLUTE:
        closure->accepted = TinyXInputPointerMotionAbsolute(closure->x,
                                                             closure->y);
        break;
    case INPUT_RELATIVE:
        closure->accepted = TinyXInputPointerMotionRelative(closure->x,
                                                             closure->y);
        break;
    case INPUT_BUTTON:
        closure->accepted = TinyXInputPointerButton(closure->detail,
                                                     closure->pressed);
        break;
    case INPUT_KEY:
        closure->accepted = TinyXInputKey(closure->detail,
                                          closure->pressed);
        break;
    case INPUT_RELEASE_KEYS:
        closure->accepted = TinyXInputReleaseAllKeys();
        break;
    }
}

static tinyx_status
RunInput(tinyx_server *server, InputClosure *closure)
{
    tinyx_status status = BeginCall(server, 1);

    if (status != TINYX_OK)
        return status;
    status = RunProtected(server, InjectInput, closure);
    if (status != TINYX_OK)
        return status;
    return closure->accepted ? TINYX_OK : TINYX_ERROR_INVALID_STATE;
}

tinyx_status
tinyx_pointer_motion_absolute(tinyx_server *server, int32_t x, int32_t y)
{
    InputClosure closure = { INPUT_ABSOLUTE, x, y, 0, 0, 0 };
    return RunInput(server, &closure);
}

tinyx_status
tinyx_pointer_motion_relative(tinyx_server *server, int32_t dx, int32_t dy)
{
    InputClosure closure = { INPUT_RELATIVE, dx, dy, 0, 0, 0 };
    return RunInput(server, &closure);
}

tinyx_status
tinyx_pointer_button(tinyx_server *server, uint32_t button, int pressed)
{
    InputClosure closure = { INPUT_BUTTON, 0, 0, button, !!pressed, 0 };

    if (!button || button > TINYX_POINTER_BUTTON_COUNT)
        return TINYX_ERROR_INVALID_ARGUMENT;
    return RunInput(server, &closure);
}

tinyx_status
tinyx_key(tinyx_server *server, uint32_t keycode, int pressed)
{
    InputClosure closure = { INPUT_KEY, 0, 0, keycode, !!pressed, 0 };

    if (keycode < TINYX_MIN_KEYCODE || keycode > TINYX_MAX_KEYCODE)
        return TINYX_ERROR_INVALID_ARGUMENT;
    return RunInput(server, &closure);
}

tinyx_status
tinyx_release_all_keys(tinyx_server *server)
{
    InputClosure closure = { INPUT_RELEASE_KEYS, 0, 0, 0, 0, 0 };
    return RunInput(server, &closure);
}
