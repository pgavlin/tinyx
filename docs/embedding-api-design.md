# Phase 8 Embedding API

## Status

The API described here is implemented by `include/tinyx.h` and
`kdrive/memory/api.c`, but remains prerelease and intentionally unstable.
TinyX and its in-tree embedders are currently its only consumers, so API and
ABI changes may be made whenever they improve the design. Such changes must
update the public header, implementation, documentation, tests, examples,
WASM exports, and in-tree consumers together.

Current version numbers identify the header and configuration contract used by
a build; they do not promise backward source or binary compatibility. The
project will declare a separate release milestone before compatibility rules
become binding.

The API consolidates the provisional lifecycle, host, memory-client,
memory-display, and input interfaces behind one host-oriented facade. DIX,
KDrive, Xtrans, and other server internals do not appear in the public header.

## Goals

The first public API should provide:

- one opaque server handle over the current process-global singleton;
- explicit creation and destruction without command-line emulation;
- bounded, nonblocking cooperative execution;
- descriptor-free X11 client byte streams;
- framebuffer and damage access;
- pointer and keyboard injection;
- host time, logging, wakeup, LED, and bell callbacks;
- explicit ownership, threading, reentrancy, and fatal-error rules;
- version fields that detect mismatched headers and implementations and can
  support a stable compatibility policy after release.

The first version does not attempt to provide concurrent server instances,
thread safety, native listeners, generation reset, or a public font-provider
interface. API 1.1 adds runtime resizing of the single memory screen, and API
1.2 adds host-supplied physical screen dimensions for DPI-aware clients.

## Public header shape

The public interface is declared in the installed `tinyx.h` header. The code
blocks below document its v1 shape; the header is authoritative for exact C
declarations and visibility annotations.

### Core types and status model

```c
#ifndef TINYX_H
#define TINYX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TINYX_API_VERSION_MAJOR 1
#define TINYX_API_VERSION_MINOR 2

typedef struct tinyx_server tinyx_server;
typedef struct tinyx_client tinyx_client;

typedef enum tinyx_status {
    TINYX_OK = 0,
    TINYX_ERROR_INVALID_ARGUMENT,
    TINYX_ERROR_INVALID_STATE,
    TINYX_ERROR_ALREADY_EXISTS,
    TINYX_ERROR_OUT_OF_MEMORY,
    TINYX_ERROR_WOULD_BLOCK,
    TINYX_ERROR_CLOSED,
    TINYX_ERROR_UNSUPPORTED,
    TINYX_ERROR_POISONED,
    TINYX_ERROR_FATAL
} tinyx_status;

typedef enum tinyx_log_level {
    TINYX_LOG_ERROR,
    TINYX_LOG_AUDIT,
    TINYX_LOG_FATAL
} tinyx_log_level;

typedef struct tinyx_error {
    tinyx_status status;
    char message[1024];
} tinyx_error;
```

All fallible operations return `tinyx_status`. Byte counts and created objects
are returned through output parameters so that a zero count or null object is
not overloaded as an error indication.

`TINYX_ERROR_FATAL` means that the current operation encountered a fatal
server failure and poisoned the singleton. Calls made after that failure
return `TINYX_ERROR_POISONED`, except for the limited inspection and cleanup
operations explicitly allowed below.

### Host callbacks

```c
typedef struct tinyx_host_ops {
    uint32_t struct_size;

    uint32_t (*monotonic_time_ms)(void *userdata);
    void (*log)(void *userdata,
                tinyx_log_level level,
                const char *message);
    void (*wakeup)(void *userdata);

    void (*leds_changed)(void *userdata, uint32_t leds);
    void (*bell)(void *userdata,
                 int volume_percent,
                 int pitch_hz,
                 int duration_ms);
} tinyx_host_ops;
```

The operation table is copied during server creation. `userdata` is borrowed
and must remain valid until `tinyx_server_destroy()` returns.

The monotonic clock uses wrapping 32-bit milliseconds. Time comparisons in the
core retain X server wraparound semantics. If the callback is absent, the
native monotonic implementation is used. Logging, wakeup, LED, and bell
callbacks are optional.

Callbacks execute synchronously on the server thread. A callback must not call
any TinyX API. Callback message strings are borrowed and remain valid only for
the duration of the callback.

The wakeup callback is advisory. It tells the host to arrange a future call to
`tinyx_server_step()`; it must not reenter the server directly.

### Screen and server configuration

```c
typedef struct tinyx_screen_config {
    uint32_t struct_size;

    uint32_t width;
    uint32_t height;

    /* Zero selects width * 4. */
    size_t stride_bytes;

    /*
     * Optional borrowed framebuffer. NULL asks TinyX to allocate it.
     * A supplied buffer remains owned by the host.
     */
    void *pixels;
    size_t pixels_size;

    /* Optional physical dimensions; zero selects/preserves default DPI. */
    uint32_t width_mm;
    uint32_t height_mm;
} tinyx_screen_config;

void tinyx_screen_config_init(tinyx_screen_config *config);

typedef struct tinyx_config {
    uint32_t struct_size;
    uint32_t api_version_major;
    uint32_t api_version_minor;

    tinyx_host_ops host;
    void *host_userdata;

    const tinyx_screen_config *initial_screen;
} tinyx_config;

void tinyx_config_init(tinyx_config *config);

tinyx_status tinyx_server_create(
    const tinyx_config *config,
    tinyx_server **out_server,
    tinyx_error *error);

tinyx_status tinyx_server_destroy(tinyx_server *server);

const char *tinyx_server_last_error(const tinyx_server *server);
```

`tinyx_server_last_error()` returns the diagnostic recorded when a fatal core
error poisoned the server, or null if no fatal error has occurred. Ordinary
status results such as `TINYX_ERROR_INVALID_ARGUMENT`,
`TINYX_ERROR_WOULD_BLOCK`, and `TINYX_ERROR_CLOSED` do not replace this
diagnostic. The returned null-terminated string is borrowed, may be truncated
to the implementation's diagnostic limit, and remains valid until server
destruction. This is one of the inspection operations permitted on a poisoned
server. It must still be called from the server thread and outside a callback.

`tinyx_screen_config_init()` zeroes the screen descriptor, records its size,
and installs the default dimensions and allocation policy. The host may then
override the dimensions, physical dimensions, stride, or storage. Zero
physical dimensions select the historical 75 DPI at creation. Supplying one
or both millimeter dimensions controls the corresponding X11 screen DPI;
values must fit the protocol's 16-bit physical-size fields.
`tinyx_config_init()` similarly records the API version and installs server
defaults. The host assigns a
pointer to its screen descriptor to `initial_screen` before creation.

The screen descriptor is copied during `tinyx_server_create()`, so the
`tinyx_screen_config` object itself only needs to remain valid for that call.
If `pixels` is non-null, however, the referenced storage remains borrowed for
the active screen's lifetime.

Keeping `initial_screen` as a pointer rather than embedding the descriptor by
value lets `tinyx_screen_config` grow independently without shifting fields in
`tinyx_config`. It also provides a reusable input contract for a future screen
resize operation.

Server creation does not accept `argc`, `argv`, environment variables, display
numbers, sockets, or authorization settings. It creates the memory-display
host directly.

The v1 lifetime rules are:

- exactly one server lifetime is permitted per process or WASM module;
- neither concurrent nor sequential replacement servers are supported in v1;
- creation binds the server to the calling thread;
- all API operations must execute on that thread;
- the API is non-thread-safe and non-reentrant;
- host, client-queue, and pixel-format configuration is immutable after
  successful creation; the single screen dimensions and storage may be
  replaced with `tinyx_server_resize()`;
- server destruction forcibly closes and invalidates all remaining client
  handles;
- destruction is the only mutating operation permitted after poisoning;
- after poisoning, destruction performs best-effort facade cleanup, but the
  process-global core cannot be reused.

The embedded `fixed` and `cursor` font catalog is used. API v1 does not expose
a font provider. The internal catalog and lease seam remains available for a
future API addition.

### Cooperative execution

```c
#define TINYX_NO_TIMEOUT UINT32_MAX

typedef struct tinyx_step_result {
    uint32_t requests_processed;

    /* Another step may make progress without new host activity. */
    int immediate_work;

    /* The server generation has stopped. */
    int generation_finished;

    /* Delay until the next timer, or TINYX_NO_TIMEOUT. */
    uint32_t next_timeout_ms;
} tinyx_step_result;

tinyx_status tinyx_server_step(
    tinyx_server *server,
    uint32_t request_budget,
    tinyx_step_result *result);
```

The public API requires `request_budget` to be at least one. It does not expose
the internal convention that zero means unlimited, because accidental
unbounded dispatch would violate cooperative scheduling guarantees.

A step never waits for host activity and dispatches no more than the requested
number of complete X11 requests. It also services pending input, expired
timers, deferred work, and output. It may invoke host callbacks.

`immediate_work` indicates that another step may make progress without waiting
for input or a timer. `next_timeout_ms` is relative to the current host time;
`TINYX_NO_TIMEOUT` means no timer is armed.

If a fatal error unwinds the operation, the call returns
`TINYX_ERROR_FATAL`. If `generation_finished` becomes nonzero, client input and
host input injection stop accepting work and the caller should destroy the
server.

In traditional X server terminology, a generation is one initialized set of
screens, devices, extensions, resources, and clients within a longer-lived
server process. Native X servers can tear that state down and initialize a new
generation after a reset without restarting the process. Historically this
allowed a long-lived display server to discard client-visible state, reclaim
resources, and restore defaults between sessions while retaining its process,
listeners, and ownership of display hardware. Reset could be requested by
server policy or a signal and commonly occurred when the last ordinary client
disconnected unless disabled by command-line policy.

The embedding facade maps one created server to one generation and does not
expose generation reset in API v1. Resizing a screen in a future API would not
by itself start a new generation.

### Logical X11 clients

```c
typedef struct tinyx_client_config {
    uint32_t struct_size;

    /* Zero selects the documented library default. */
    size_t input_buffer_limit;
    size_t output_buffer_limit;
} tinyx_client_config;

void tinyx_client_config_init(tinyx_client_config *config);

tinyx_status tinyx_client_open(
    tinyx_server *server,
    const tinyx_client_config *config,
    tinyx_client **out_client);

tinyx_status tinyx_client_send(
    tinyx_client *client,
    const void *bytes,
    size_t length,
    size_t *out_length);

tinyx_status tinyx_client_receive(
    tinyx_client *client,
    void *bytes,
    size_t capacity,
    size_t *out_length);

size_t tinyx_client_receive_pending(const tinyx_client *client);

tinyx_status tinyx_client_shutdown_send(tinyx_client *client);
int tinyx_client_is_closed(const tinyx_client *client);
void tinyx_client_destroy(tinyx_client *client);
```

Client stream operations are named from the logical client's point of view:
`send` moves X11 wire bytes from the client to the server, while `receive`
moves replies, events, and errors from the server to the client.

`tinyx_client_send()` copies as many bytes as fit in the finite
client-to-server queue and reports that count through `out_length`. It returns
`TINYX_OK` if at least one byte is accepted, including when acceptance is
partial. It returns `TINYX_ERROR_WOULD_BLOCK` with `out_length` set to zero when
no queue capacity is available. The caller retains and retries any unaccepted
suffix after stepping the server. This also permits a source chunk larger than
the entire queue limit to make bounded progress.

Accepted chunks may split the X11 setup message, an ordinary request, or a
BIG-REQUESTS request at any byte boundary. The X11 setup stream determines the
client's byte order.

Memory clients are trusted in-process clients. They use the normal DIX setup
and dispatch path but bypass native peer-address authorization.

`tinyx_client_shutdown_send()` closes the client-to-server direction and
signals orderly EOF to the server. Bytes already accepted from the client
remain available to the server. `tinyx_client_destroy()` immediately closes
both directions and may discard unread server output.

`tinyx_client_receive()` copies up to `capacity` server-to-client bytes. It
returns `TINYX_OK` with `out_length` equal to zero when the connection remains
open but no bytes are currently available. It returns `TINYX_ERROR_CLOSED`
only after the server side has closed and all buffered bytes have been
received. Receiving output releases protocol backpressure and may invoke the
wakeup callback. X11 output ordering and padding are preserved.

`tinyx_client_receive_pending()` reports the number of server-to-client bytes
currently available without consuming them.

Both queue limits have finite 1 MiB defaults in each direction.
A configured limit must also be finite; API v1 provides no unbounded-buffering
sentinel. A zero configuration value selects the default rather than disabling
the limit.

### Framebuffer access

```c
typedef enum tinyx_byte_order {
    TINYX_BYTE_ORDER_LSB_FIRST,
    TINYX_BYTE_ORDER_MSB_FIRST
} tinyx_byte_order;

typedef struct tinyx_framebuffer_info {
    const void *pixels;
    size_t size;
    uint32_t width;
    uint32_t height;
    size_t stride_bytes;

    uint32_t depth;
    uint32_t bits_per_pixel;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    tinyx_byte_order byte_order;
} tinyx_framebuffer_info;

tinyx_status tinyx_server_get_framebuffer(
    tinyx_server *server,
    tinyx_framebuffer_info *info);
```

The v1 format is depth 24 in native-endian 32-bit pixel words, with these
masks:

```text
red   = 0x00ff0000
green = 0x0000ff00
blue  = 0x000000ff
```

The unused high byte has no alpha semantics. On a little-endian host the bytes
in memory are blue, green, red, and unused. Native byte order matches the FB
renderer and existing X server representation. The byte-order field makes the
layout explicit to portable hosts; WebAssembly's little-endian linear memory
therefore receives the little-endian representation without requiring a
special API format.

The pixel pointer remains stable for the active screen configuration, until a
successful `tinyx_server_resize()` or server destruction. Host-provided
storage remains borrowed and is never freed by TinyX. Library-allocated
storage is released when replaced or during server destruction.

Framebuffer bytes are read-only from the host's perspective while TinyX owns
or borrows them. The host may inspect them only while no TinyX API call is
active and must not modify them. A host that supplies storage relinquishes
write access for the active screen's lifetime.

#### Runtime resize

API 1.1 implements the operation anticipated by the original screen descriptor
design:

```c
tinyx_status tinyx_server_resize(
    tinyx_server *server,
    const tinyx_screen_config *screen);
```

The descriptor is copied during the call and follows the same validation and
storage rules as `initial_screen`. A successful resize invalidates the previous
framebuffer view, stops using old borrowed storage before returning, clears and
fully damages the new framebuffer, resizes the root window, constrains the
pointer to the new geometry, and notifies X11 clients through RandR and root
`ConfigureNotify` events. The host must fetch framebuffer metadata again.
A failed resize leaves the existing screen and framebuffer unchanged.

Only the existing screen is resized: this does not create another screen or
server generation, and the depth-24/32-bpp pixel format remains fixed. API 1.2
allows the host to supply `width_mm` and `height_mm`. A zero axis preserves its
previous logical DPI across the resize; a nonzero axis replaces that physical
dimension and therefore its DPI. X11 RandR requests use the physical dimensions
supplied by the client.

### Damage consumption

```c
typedef struct tinyx_damage_rect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} tinyx_damage_rect;

tinyx_status tinyx_server_take_damage(
    tinyx_server *server,
    tinyx_damage_rect *rects,
    size_t capacity,
    size_t *out_count);
```

A call with zero capacity queries the current rectangle count without
consuming damage. With sufficient capacity, the operation returns and consumes
all rectangles. With insufficient nonzero capacity, it returns one bounding
rectangle and consumes all current damage. `out_count` reports the number of
rectangles actually written, not the count before collapsing.

API v1 does not report whether a returned rectangle was an original damage
rectangle or a collapsed bounding rectangle. Both require the host to present
the indicated area, so that distinction is not part of the contract.

### Input injection

```c
#define TINYX_MIN_KEYCODE 8
#define TINYX_MAX_KEYCODE 247
#define TINYX_POINTER_BUTTON_COUNT 5

tinyx_status tinyx_pointer_motion_absolute(
    tinyx_server *server,
    int32_t x,
    int32_t y);

tinyx_status tinyx_pointer_motion_relative(
    tinyx_server *server,
    int32_t dx,
    int32_t dy);

tinyx_status tinyx_pointer_button(
    tinyx_server *server,
    uint32_t button,
    int pressed);

tinyx_status tinyx_key(
    tinyx_server *server,
    uint32_t keycode,
    int pressed);

tinyx_status tinyx_release_all_keys(tinyx_server *server);
```

Absolute pointer coordinates are root-window pixels. Absolute motion is
clamped by the server and bypasses pointer acceleration. Relative motion uses
the active X pointer acceleration settings. Buttons are X button numbers 1
through 5.

Keys use the conventional US Xorg keycode layout, equivalent to an evdev code
plus 8, in the inclusive range 8 through 247. Hosts translate DOM, SDL, evdev,
or other identifiers into these keycodes. Hosts own repeat timing. Repeated
presses still pass through active X keyboard repeat controls. A host should
call `tinyx_release_all_keys()` when it loses keyboard focus.

Accepted input enters the existing KDrive, MI, and DIX event path and requests
a host wakeup.

## Ownership summary

| Object or data | Owner | Lifetime |
|---|---|---|
| `tinyx_server` | host | until `tinyx_server_destroy()` |
| `tinyx_client` | host | until client destroy or parent server destroy |
| callback table | copied by TinyX | server lifetime |
| callback userdata | host | through server destruction |
| initial screen descriptor | host, copied synchronously | only needed during creation |
| bytes passed to `send` | host, partially or fully copied synchronously | accepted prefix only needed during call |
| bytes returned by `receive` | host | host-controlled |
| host framebuffer | host, exclusively borrowed for TinyX writes | active screen configuration |
| allocated framebuffer | TinyX | active screen configuration |
| framebuffer info pointer | borrowed from TinyX | active screen configuration |
| callback message | TinyX, borrowed by callback | callback duration |
| fatal-error string | TinyX, borrowed by host | until server destruction |

## Threading and reentrancy

API v1 has strict cooperative rules:

1. The thread that creates the server owns it.
2. Every server, client, framebuffer, damage, and input operation runs on that
   thread.
3. Hosts with worker-thread I/O must marshal client bytes onto the server
   thread before calling the API.
4. No callback may reenter TinyX.
5. No operation may overlap another operation.
6. Framebuffer memory is only stable for host access while no API operation is
   active.

These restrictions apply even if a particular host platform happens to make a
call safe today.

## Fatal errors and poisoning

Every public operation that enters the core runs inside the internal protected
fatal-error boundary. A fatal server error:

1. records a bounded diagnostic;
2. invokes the fatal log callback, if configured;
3. marks the singleton poisoned;
4. unwinds to the public API boundary;
5. returns `TINYX_ERROR_FATAL` from the interrupted operation.

The core is never resumed after that unwind. Later operational calls return
`TINYX_ERROR_POISONED`. Error inspection and best-effort destruction remain
available. Poisoning is permanent for the process or module in API v1.

Creation errors that occur before a server handle exists are copied into the
optional `tinyx_error` output. If fatal initialization occurs after partial
facade creation, `tinyx_server_create()` cleans up the facade internally,
leaves `out_server` null, and reports the diagnostic through `tinyx_error`.
The process-global core remains poisoned and cannot be used to create another
server.

## ABI and symbol policy

Opaque handles isolate public consumers from DIX and KDrive structure layouts.
Configuration structures currently begin with `struct_size`, and callers
initialize them through API functions. These conventions provide validation
and leave room for a future compatible-extension policy, but they do not make
the prerelease ABI stable.

Until the project explicitly declares the embedding API released, callers must
build against matching headers and libraries. Structures may be reordered or
replaced, fields and functions may change meaning or signature, and version
numbers may advance without preserving old layouts. In-tree consumers are
updated atomically with each such change. Once a stable ABI is declared, this
section must be replaced with the compatibility rules that releases will
actually enforce.

Only deliberate `tinyx_*` public facade symbols should be exported. The
following remain private:

- provisional `TinyXHost*`, `TinyXMemory*`, `TinyXInput*`, and
  `TinyXMemoryDisplay*` symbols;
- DIX, DDX, KDrive, MI, FB, and Xtrans symbols;
- native lifecycle and dispatch adapters;
- font catalog and materialization internals.

The eventual build should enforce this with symbol visibility and an explicit
export list rather than relying only on header installation policy.

## First audit decisions

The first API audit made these decisions:

- API v1 permits exactly one server lifetime per process or WASM module;
  neither clean destruction nor poisoning permits another creation;
- client input and output queues always have finite limits, defaulting to
  1 MiB each; there is no unbounded mode;
- `tinyx_client_send()` permits partial acceptance and reports the accepted
  prefix length;
- the fixed-size `tinyx_error` is sufficient for creation diagnostics;
- generation reset is not exposed in API v1;
- damage consumption does not report whether rectangles were collapsed;
- framebuffer words retain native byte order, with byte order reported in
  framebuffer metadata;
- framebuffer bytes are read-only from the host's perspective, including when
  the host supplied the storage;
- fatal initialization is cleaned up internally and returns no server handle.

The proposed public header ends with:

```c
#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TINYX_H */
```
