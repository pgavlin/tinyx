# Embeddable TinyX Refactoring Plan

## Objective

Refactor TinyX in place into a host-independent server core that can eventually
be exposed as a static library or WASM module.

The architectural refactor and build-system migration are separate projects.
The first phase will continue to use the existing Autotools build and preserve
the current native executables as reference hosts. CMake or Meson can be added
after the component boundaries and source sets are known.

The intended dependency direction is:

```text
native host ─┐
memory host ─┼──> embedding API/core <── DIX, MI, FB, selected extensions
WASM host ───┘
```

Host implementations own acquisition and presentation. The core owns X11
protocol semantics and server state.

## Implementation status

- **Phase 1 complete:** process and generation lifecycle operations are
  callable, and the native `main()` is a thin adapter.
- **Phase 2 complete:** dispatch initialization and teardown are separate from
  execution; bounded `TinyXServerStep()` is nonblocking; the native lifecycle
  retains blocking behavior; pending work and the next timer delay are
  reported to cooperative hosts.
- **Phase 3 complete:** request framing and output buffering use an explicit
  nonblocking byte-stream interface; Xtrans is an adapter; descriptor-free
  memory clients can feed and drain arbitrary chunks through the same DIX
  connection handshake and dispatch path.
- **Phase 4 complete:** monotonic time, logging, and host wakeups use explicit
  runtime operations; native process behavior remains the default; custom
  hosts can run core calls inside a fatal-error boundary that poisons and
  unwinds the singleton instead of terminating the process.
- **Phase 5 complete:** a KDrive memory backend renders a fixed depth-24,
  32-bpp screen into allocated or host-provided linear memory and exposes
  accumulated Damage regions without performing presentation.
- **Phase 6 complete:** hosts can inject absolute or relative pointer motion,
  button transitions, and X keycode transitions without descriptors; events
  retain the ordinary KDrive, MI, and DIX path and wake cooperative hosts.
- **Phase 7 complete:** a synchronous embedded bitmap-font catalog supplies
  `fixed` and `cursor` through the ordinary DIX FPE, resource, rendering, and
  glyph-cursor paths without libXfont or filesystem access. Acquisition leases
  remain separate from `FontRec` materialization for future host providers.

## Guiding principles

1. **Preserve behavior while extracting boundaries.** Each early change should
   leave the native server working as before.
2. **Separate mechanism from host policy.** Protocol framing, timers, and X11
   semantics remain in the core; sockets, blocking waits, devices, and process
   policy belong to hosts.
3. **Do not make WASM the internal architecture.** WASM is one consumer of the
   same interfaces used by native and in-process hosts.
4. **Keep the initial implementation singleton.** Removing pervasive global
   state and supporting concurrent instances is a separate project.
5. **Keep the public API small.** Establish and test internal seams before
   freezing public names, ownership rules, and error behavior.
6. **Do not migrate the build first.** Update the existing Automake manifests
   as necessary, but defer a new build system until the architecture settles.
7. **Validate boundaries with working hosts.** A boundary is not complete until
   both the existing native implementation and a host-neutral implementation
   exercise it.

## Initial scope

The first embeddable version should support:

- one server instance per process or WASM module;
- one memory-backed screen with a fixed, documented pixel format;
- logical X11 clients represented as ordered byte streams;
- bounded, nonblocking server execution;
- pointer and keyboard event injection;
- framebuffer access and dirty-region reporting;
- host-provided monotonic time and logging;
- a deliberately selected extension set;
- a minimal, explicit font strategy;
- native static-library and Emscripten consumers once the architecture is
  established.

The following are not initial goals:

- concurrent server instances;
- thread safety;
- full parity with every native extension;
- MIT-SHM, XDMCP, native authorization, DPMS, fbdev, or VESA in WASM;
- replacing DIX, MI, FB, or the X11 protocol implementation;
- replacing Autotools during the architectural phase.

## Phase 1: Extract the server lifecycle

Split the process entry point in `dix/main.c` into internal lifecycle
operations. The exact names are provisional, but the responsibilities should
be equivalent to:

```c
int TinyXServerInitialize(...);
int TinyXServerInitializeGeneration(...);
void TinyXServerResetGeneration(...);
void TinyXServerShutdown(...);
```

Keep `main()` as a thin native adapter over these operations.

The extraction must preserve:

- process-wide initialization versus generation initialization;
- initialization ordering for screens, extensions, input, fonts, and roots;
- server reset behavior;
- reverse-order generation teardown;
- native argument processing and DDX hooks.

### Completion criteria

- `main()` no longer contains the implementation of server initialization and
  teardown;
- the existing native executable follows the same startup, reset, and shutdown
  sequence;
- no embedding API has to emulate command-line startup to initialize the core.

## Phase 2: Separate dispatch from blocking waits

Refactor `Dispatch()` and `WaitForSomething()` so server work can be performed
without blocking. Separate at least these responsibilities:

- processing queued input events;
- running expired timers;
- processing deferred work;
- identifying clients with complete buffered requests;
- dispatching a bounded number of requests;
- flushing buffered output;
- calculating the next timer deadline;
- waiting for native descriptors.

The resulting internal operations may resemble:

```c
int TinyXRunPendingWork(...);
int TinyXDispatchReadyClients(unsigned request_budget);
uint32_t TinyXNextDeadline(...);
```

The native host may continue to use `select()`, but only outside the
nonblocking core operation. Its loop should wait for native activity and then
invoke the same core operations that an embedder will call directly.

Request budgeting must retain reasonable fairness between clients. Timer,
work-queue, block-handler, wakeup-handler, input, and output semantics must not
be silently lost when the blocking loop is split.

### Completion criteria

- one core step always returns without waiting for host activity;
- a caller can bound the number of dispatched requests;
- the native host can reproduce the existing blocking behavior around the
  nonblocking core;
- the core can report whether immediate work remains and when timed work is
  next due.

## Phase 3: Separate byte-stream mechanics from Xtrans

Retain the host-independent responsibilities currently mixed into `os/io.c`:

- preserving partial client input;
- parsing ordinary and BIG-REQUESTS request lengths;
- presenting one complete request to DIX;
- padding and ordering replies, errors, and events;
- buffering output and representing backpressure;
- maintaining client connection state.

Move concrete transport operations behind an internal interface, initially
implemented by an Xtrans adapter. An illustrative interface is:

```c
typedef struct TinyXTransportOps {
    ssize_t (*read)(void *connection, void *buffer, size_t size);
    ssize_t (*write)(void *connection, const void *buffer, size_t size);
    void (*close)(void *connection);
} TinyXTransportOps;
```

The interface must represent nonblocking progress, orderly closure, transport
failure, and temporary backpressure without relying on file descriptors.
Whether the final internal interface is pull-based as above or queue-based
should be decided while separating the existing buffering code.

After the Xtrans adapter preserves native behavior, add an in-memory adapter
whose clients accept and produce arbitrary byte chunks.

### Completion criteria

- Xtrans calls and descriptor readiness are confined to the native adapter;
- request framing works across every possible input fragmentation boundary;
- output can be drained incrementally by an embedder;
- opening and closing a logical client does not require a socket;
- both native and memory clients use the same DIX handshake and dispatch path.

### Implemented

`os/transport.h` defines progress, would-block, closed, and failure results
without using descriptors or `errno`. `os/transport.c` adapts native Xtrans
connections. `os/io.c` now retains only framing, padding, buffering, and
backpressure policy.

`include/tinyx-memory.h` and `os/memory.c` add a provisional in-process client
handle. Hosts can append arbitrary input fragments, drain arbitrary output
sizes, signal end-of-input, and choose a bounded output queue to exercise
backpressure. Memory readiness is merged with native readiness before DIX
priority selection, and server grabs, ignored clients, request budgets, and
round-robin dispatch apply to both kinds of client.

The interface is intentionally provisional until the phase 13 embedding facade
is frozen. In-process clients are trusted by their embedding host and bypass
native peer-address authorization; native clients retain existing Xtrans
access control.

## Phase 4: Define host runtime services

Introduce explicit internal contracts for runtime behavior currently assumed
to be process-global or Unix-specific. At minimum, evaluate interfaces for:

- monotonic time;
- logging and audit output;
- waking or rescheduling the host;
- fatal failures and error propagation;
- optional entropy or other host facilities discovered during extraction.

An illustrative starting point is:

```c
typedef struct TinyXHostOps {
    uint32_t (*monotonic_time_ms)(void *userdata);
    void (*log)(void *userdata, int level, const char *message);
    void (*wakeup)(void *userdata);
} TinyXHostOps;
```

Fatal errors require special treatment. A library must not unconditionally
terminate its host process, but a callback that returns after an unrecoverable
failure is also unsafe. The implementation needs an explicit unwind or
poisoned-server policy with errors reported at the embedding boundary.

Font and filesystem access should remain a separate interface rather than
turning the runtime operations into an unbounded collection of callbacks.

### Completion criteria

- the core does not require ownership of process logging or termination;
- all scheduling timestamps derive from an explicit monotonic clock;
- native behavior is supplied by a native `TinyXHostOps` implementation;
- an in-process host can observe failures without being terminated.

### Implemented

`include/tinyx-host.h` defines the provisional singleton runtime contract.
Every `GetTimeInMillis()` caller now obtains time through the configured
monotonic clock, `ErrorF()` and `AuditF()` route messages to the host logger,
and memory-client activity invokes the optional wakeup callback. The default
implementation preserves the native clock, stderr/audit formatting, process
cleanup, and termination behavior.

Custom hosts run lifecycle or dispatch calls with `TinyXHostRunProtected()`.
A `FatalError()` inside that boundary records the diagnostic, marks the server
poisoned, and unwinds to the caller. It never returns into the failed core
operation. A poisoned singleton cannot be entered again and must currently be
abandoned; recovery and resource reclamation will be wrapped by the phase 8
public facade. This policy is deliberately non-reentrant and non-thread-safe.
Calling a fatal core path with custom operations but without a protected
boundary aborts, because returning from `FatalError()` would be unsafe.

Font and filesystem acquisition remain separate from this runtime contract.

## Phase 5: Add a memory display and presentation boundary

Implement a memory-backed display using KDrive initially. The backend should:

- allocate or accept a linear framebuffer;
- describe width, height, depth, bits per pixel, masks, and stride;
- use existing FB and MI rendering;
- avoid hardware mapping, VT ownership, modesetting, and device power control;
- expose framebuffer metadata without exposing `KdScreenInfo` publicly.

Use the existing Damage infrastructure to collect changed regions. At a step
boundary, the core should either expose accumulated rectangles or invoke one
presentation notification. The host owns Canvas, WebGL/WebGPU, SDL, a native
window, or physical display presentation.

Start with one documented 32-bit format. Additional formats and host-provided
buffers can be added after the basic ownership model is proven.

### Completion criteria

- the complete root screen renders into ordinary memory;
- the host can obtain stable framebuffer metadata and pixels;
- drawing produces bounded dirty regions;
- presenting or uploading pixels is not performed by the core;
- native hardware backends remain available to native executables.

### Implemented

`kdrive/memory/` is a hardware-free KDrive backend. It accepts a borrowed
linear buffer or allocates one for each active generation, configures FB for a
single depth-24, 32-bpp TrueColor screen, and supplies no-op display power,
mode, VT, and device-acquisition behavior. The existing `Xfbdev` and `Xvesa`
frontends are unchanged.

The provisional `include/tinyx-display.h` interface returns framebuffer
metadata without exposing `KdScreenInfo`. Its native-endian pixel words use
red, green, and blue masks `0x00ff0000`, `0x0000ff00`, and `0x000000ff`;
on little-endian hosts this is byte order B, G, R, X.

An internal Damage object tracks writes to the screen pixmap. Hosts consume
current rectangles with `TinyXMemoryDisplayTakeDamage()`. If the caller's
capacity cannot hold the region, the backend returns one bounding rectangle,
so presentation metadata is always bounded by caller storage. The server does
not upload, display, or otherwise interpret the pixels. `Xmemory` is built as
a non-installed reference frontend. With the embedded font configuration it
completes generation startup without a font filesystem.

## Phase 6: Add explicit input injection

Separate host event acquisition from KDrive input semantics. Preserve in the
server:

- X keyboard and pointer devices;
- key and button state;
- MI event queueing;
- focus, grabs, propagation, and client delivery;
- repeat, acceleration, or emulation policies that are intentionally retained.

The host owns:

- DOM, SDL, evdev, console, or application event acquisition;
- host-key translation;
- pointer coordinate conversion;
- host-specific LED and bell effects.

Pointer injection should cover absolute motion, relative motion if needed, and
button transitions. The keyboard API requires an explicit decision between X
keycodes, scan codes, portable physical-key identifiers, and symbolic keys.
The smallest first boundary is likely X keycodes; a friendlier mapping layer
can live above it.

### Completion criteria

- memory-host input requires no descriptor, signal, or Linux device;
- injected events traverse the ordinary MI and DIX event path;
- native Linux acquisition can continue to feed the same server-side path;
- keyboard representation and repeat ownership are documented.

### Implemented

`include/tinyx-input.h` defines provisional singleton injection operations for
absolute root-screen motion, accelerated relative motion, button transitions,
and key transitions. Injection requires no input descriptor and requests a
host wakeup after adding work. KDrive continues to update pointer and keyboard
state and feeds the existing MI event queue, so focus, grabs, propagation, and
client delivery are unchanged. Linux keyboard and mouse drivers continue to
use the same KDrive enqueue path.

The memory keyboard has a stable conventional US Xorg keymap (evdev keycode
plus 8) and accepts X keycodes 8 through 247. Host integrations translate
DOM, SDL, evdev, or other host key identifiers into those keycodes. The host
owns repeat timing: a repeated press for an already-down key is interpreted by
KDrive using the active X keyboard
controls and, when repeat is enabled, becomes the traditional release/press
pair. Hosts should balance transitions or call `TinyXInputReleaseAllKeys()`
when focus is lost.

The memory pointer exposes five X buttons. Absolute coordinates are X root
coordinates and are clamped by MI; relative deltas retain X pointer
acceleration. Existing KDrive middle-button emulation and button mapping remain
in the event path. Optional LED and bell callbacks make those effects host
policy rather than requiring a native console device. The interface remains
non-thread-safe and non-reentrant and will be wrapped by the phase 8 facade.

## Phase 7: Isolate font acquisition

Treat fonts as a dedicated architectural boundary. DIX font resources,
requests, and text rendering remain server responsibilities, while font bytes,
font paths, and filesystem policy belong to a provider.

Evaluate these implementation strategies:

1. retain `libXfont` for the native host and provide a virtual filesystem in
   WASM;
2. embed a minimal bitmap text font and cursor font;
3. add a font-provider interface that supplies font data to the existing font
   machinery;
4. combine an embedded startup fallback with optional host-provided fonts.

The initial choice must support server startup, including the default cursor,
and clearly document unsupported font requests.

### Completion criteria

- startup does not implicitly require a native filesystem;
- the default text and cursor fonts are available in the memory host;
- native font behavior is preserved where practical;
- unsupported font operations fail through X11 semantics rather than crashing
  or terminating the host.

### Design

[Phase 7 Font Architecture](font-design.md) records the selected design and
implementation slices. The embedded configuration will use checked-in,
development-time-generated bitmap data behind a synchronous `built-ins` FPE.
An internal acquire/release catalog keeps font acquisition separate from the
catalog-independent `FontRec` materializer, allowing a later host provider to
reuse the backend without exposing DIX structures. The initial catalog only
serves built-in data. DIX font resources, protocol operations, FB text
rendering, and glyph cursors therefore remain intact without a runtime parser
or filesystem. Native builds retain the existing libXfont font-file behavior.
No public host font-provider API is introduced before the Phase 8 facade
defines naming, ownership, synchronization, and reentrancy rules.

### Implemented

`dix/embedded-font.c` implements a synchronous `built-ins` FPE over an
internal catalog contract. Catalog acquisition returns a decoded-font lease;
the independent materializer converts canonical bitmap rows, metrics, and
properties into generation-local `FontRec` instances and releases the lease
on close or failure. The built-in catalog is static today but the materializer
does not depend on generated symbols or process-lifetime ownership.

`dix/embedded-font-data.c` contains generated ISO-8859-1 6x13 and complete
cursor data. `fonts/generate-builtin-fonts.py` deterministically regenerates it
from the X.Org BDF releases documented in `fonts/README.md`. The catalog
supports `fixed`, `6x13`, `cursor`, the canonical 6x13 XLFD, and its historical
100-dpi alias. Listing, querying, text rendering, and glyph cursor creation use
the existing DIX and FB paths; unknown names and paths retain X11 error
semantics.

`--disable-fonts` now selects this runnable backend, defaults the font path to
`built-ins`, and links neither libXfont nor libfontenc. The default build keeps
legacy libXfont filesystem behavior for native compatibility. The Automake
check validates generated encoding maps, bitmap bounds, required aliases, and
the complete 154-glyph cursor set.

## Phase 8: Define the public embedding API

Freeze the public interface only after the internal lifecycle, transport,
display, input, and runtime seams have working implementations.

The API should be expressed in host terms rather than X server internals. Its
shape will likely include:

```c
tinyx_server *tinyx_create(const tinyx_config *config);
void tinyx_destroy(tinyx_server *server);

int tinyx_step(tinyx_server *server, uint32_t request_budget,
               tinyx_step_result *result);

tinyx_client *tinyx_client_open(tinyx_server *server,
                                const tinyx_client_config *config);
int tinyx_client_receive(tinyx_client *client,
                         const void *bytes, size_t length);
size_t tinyx_client_drain(tinyx_client *client,
                          void *bytes, size_t capacity);
void tinyx_client_close(tinyx_client *client);

int tinyx_pointer_motion(...);
int tinyx_pointer_button(...);
int tinyx_key(...);

int tinyx_framebuffer_info(...);
size_t tinyx_take_damage(...);
```

These names are placeholders. API design must specify:

- singleton behavior despite the opaque server handle;
- ownership and lifetime of server, clients, buffers, and callback data;
- whether client input is copied or borrowed;
- output backpressure and required drain behavior;
- reentrancy restrictions;
- thread-affinity requirements;
- error categories and server poisoning;
- coordinate, keycode, pixel, and byte-order conventions;
- versioning and ABI visibility.

An opaque server handle is still useful for v1 even if only one can exist. It
avoids baking global state into the API and leaves room for later instance
isolation.

### Completion criteria

- the public header contains no DIX, KDrive, Xtrans, or platform-specific
  structures;
- a native in-process example uses only the public API;
- all ownership, error, and scheduling behavior is documented;
- only deliberate public symbols are exported.

## Phase 9: Add build and host products

Once the source boundaries are established, add a sidecar CMake or Meson build
rather than immediately deleting Autotools. Expected conceptual targets are:

```text
tinyx-core
tinyx-host-native
tinyx-host-memory
tinyx-api
tinyx-wasm
```

The new build should:

- use explicit source manifests derived from the refactored components;
- provide reproducible configuration headers;
- distinguish protocol headers from linked native libraries;
- build a native static library;
- build an Emscripten module with an explicit export list;
- exclude native sockets, signals, hardware, and authorization from WASM;
- avoid relying on native `pkg-config` results during cross-compilation.

The existing Autotools build should remain as a reference until the new build
has equivalent coverage for the intended native targets.

## Validation strategy

The repository currently has little automated coverage, so tests must be added
alongside boundary extraction rather than after it.

### Lifecycle tests

- initialize and shut down successfully;
- reset a server generation where supported;
- reject a second singleton instance cleanly;
- report initialization failure without exiting the process.

### Protocol-stream tests

- split the initial handshake at every byte boundary;
- split representative requests at every byte boundary;
- queue several requests in one input chunk;
- exercise swapped clients and BIG-REQUESTS;
- drain output in very small buffers;
- close clients with partial input or pending output;
- verify request-budget fairness between clients.

### Rendering tests

- establish a client and create/map a window;
- issue representative core drawing requests;
- compare framebuffer regions against golden output;
- verify dirty regions contain changed pixels and do not persist after being
  consumed;
- exercise software cursor behavior explicitly.

### Input tests

- inject motion, buttons, and keys;
- verify event bytes received by clients;
- test modifiers, grabs, focus, and button state;
- document and test repeat ownership.

### Native regression tests

- retain successful native startup;
- verify native Xtrans clients still connect;
- run representative existing X clients where available;
- run native builds under sanitizers;
- regenerate symbol-dependency reports to ensure native-only providers do not
  leak back into the core.

## Architectural completion criteria

The host-independent architecture is considered established when:

- `main()` is a thin native adapter over callable lifecycle operations;
- one core step is bounded and never blocks;
- Xtrans, sockets, descriptors, and `select()` are confined to the native host;
- Linux input and fbdev/VESA code are confined to native backends;
- a memory backend uses the same DIX, MI, FB, and selected-extension core;
- protocol clients are represented at the embedding boundary as ordered byte
  streams;
- framebuffer presentation and input acquisition are host responsibilities;
- time, logging, and fatal failures no longer assume control of the process;
- the core can start without an implicit native font filesystem;
- the existing native executable still works.

At that point, WASM is another host implementation and packaging target rather
than the force defining the internal architecture.

## Expected implementation sequence

A practical commit sequence is:

1. document lifecycle phases and invariants in `dix/main.c`;
2. extract lifecycle functions without changing native behavior;
3. split pending work and request dispatch from blocking wait;
4. move the native blocking loop into a native adapter;
5. separate stream framing from Xtrans operations;
6. add protocol-fragmentation tests;
7. add an in-memory client transport;
8. introduce clock, logging, wakeup, and fatal-error policy;
9. add a memory-backed KDrive card;
10. expose and test damage collection;
11. separate input injection from native acquisition;
12. establish the startup-font strategy;
13. add the public embedding facade and native example;
14. audit symbol dependencies and public exports;
15. add the sidecar Emscripten-compatible build;
16. add the WASM host and presentation example.

This sequence is intentionally architecture-first. Build migration begins only
when the core and host source sets are concrete.