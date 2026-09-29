I’d start with a **sidecar CMake build**, leaving Autoconf intact as a reference. Don’t try to convert the entire build system before understanding which code belongs in the embedded server.

## Temporary build without libXfont

The native build normally requires legacy libXfont 1 and libfontenc. During the
embedding refactor it can instead build against local, build-only font stubs:

```sh
./configure --disable-fonts [other options]
make
```

This configuration compiles and links without either font library, but it is
not runnable: no font backend is registered, so generation initialization
fails when the server opens its required `fixed` text font. The embedded
`fixed` and `cursor` implementation planned for the font phase will replace
these stubs. Do not treat `--disable-fonts` as a supported runtime mode.

## What Autoconf currently does

Very roughly:

- `configure.ac` describes feature tests, dependencies, and generated configuration headers.
- `Makefile.am` files describe targets and source lists.
- `./autogen.sh` generates `configure` and `Makefile.in`.
- `./configure` detects the host and generates:
  - `Makefile`
  - `include/dix-config.h`
  - `include/kdrive-config.h`
- `make` compiles everything.

In theory you could do:

```sh
./autogen.sh

mkdir build-wasm
cd build-wasm

emconfigure ../configure \
    --host=wasm32-unknown-emscripten \
    --disable-xdmcp \
    --disable-xdm-auth-1 \
    --disable-dpms \
    --disable-xf86bigfont \
    --disable-xvesa \
    --disable-xfbdev

emmake make
```

In practice, this will get tangled in native `pkg-config` packages, `libXfont`, `xtrans`, POSIX sockets, and hardware DDX code. It’s useful as an experiment, but I wouldn’t make it the main strategy.

## Recommended directory structure

Something like:

```text
CMakeLists.txt
cmake/
    wasm/
        dix-config.h
        kdrive-config.h
include/
    tinyx.h
embed/
    tinyx.c
    transport.c
    transport.h
    framebuffer.c
    input.c
wasm/
    main.c
tests/
    smoke.c
```

The existing source remains where it is.

## Start with a small public API

`include/tinyx.h`:

```c
#ifndef TINYX_H
#define TINYX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tinyx_config {
    uint32_t width;
    uint32_t height;

    void (*damage)(
        void *userdata,
        uint32_t x,
        uint32_t y,
        uint32_t width,
        uint32_t height);

    void (*log)(void *userdata, const char *message);
    void *userdata;
} tinyx_config;

/*
 * Initially TinyX should be treated as a singleton. An opaque server
 * object can be introduced later if the X server globals are removed.
 */
int tinyx_init(const tinyx_config *config);
void tinyx_shutdown(void);

/* Process at most `budget` requests without blocking. */
int tinyx_step(uint32_t budget);

int tinyx_client_open(void);
void tinyx_client_close(int client);

int tinyx_client_write(int client, const void *data, size_t size);
size_t tinyx_client_read(int client, void *data, size_t capacity);

uint8_t *tinyx_framebuffer(void);
uint32_t tinyx_framebuffer_stride(void);

void tinyx_pointer_motion(int32_t x, int32_t y);
void tinyx_pointer_button(uint32_t button, int pressed);
void tinyx_key(uint32_t keycode, int pressed);

#ifdef __cplusplus
}
#endif

#endif
```

The first implementation can return errors for most operations. The point is to establish the embedding boundary early.

## Initial CMake file

Don’t put every source file in immediately. Add subsystems as you make them compile.

```cmake
cmake_minimum_required(VERSION 3.20)

project(tinyx LANGUAGES C)

option(TINYX_BUILD_WASM "Build the Emscripten module" OFF)
option(TINYX_BUILD_TESTS "Build native tests" ON)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

add_library(tinyx STATIC)

target_sources(tinyx PRIVATE
    embed/tinyx.c

    # Add these groups incrementally:
    # dix/atom.c
    # dix/resource.c
    # ...
)

target_include_directories(tinyx
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/config/wasm
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/dix
        ${CMAKE_CURRENT_SOURCE_DIR}/mi
        ${CMAKE_CURRENT_SOURCE_DIR}/fb
        ${CMAKE_CURRENT_SOURCE_DIR}/render
        ${CMAKE_CURRENT_SOURCE_DIR}/randr
        ${CMAKE_CURRENT_SOURCE_DIR}/Xext
)

target_compile_definitions(tinyx PRIVATE
    HAVE_DIX_CONFIG_H=1
    VENDOR_STRING="TinyX"
    VENDOR_RELEASE=1
)

if(TINYX_BUILD_TESTS AND NOT EMSCRIPTEN)
    add_executable(tinyx-smoke tests/smoke.c)
    target_link_libraries(tinyx-smoke PRIVATE tinyx)
endif()

if(EMSCRIPTEN)
    add_executable(tinyx-wasm wasm/main.c)
    target_link_libraries(tinyx-wasm PRIVATE tinyx)

    set_target_properties(tinyx-wasm PROPERTIES
        OUTPUT_NAME tinyx
        SUFFIX ".js"
    )

    target_link_options(tinyx-wasm PRIVATE
        "SHELL:-s WASM=1"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s NO_EXIT_RUNTIME=1"
        "SHELL:-s EXPORTED_FUNCTIONS=['_tinyx_init','_tinyx_shutdown','_tinyx_step','_tinyx_client_open','_tinyx_client_close','_tinyx_client_write','_tinyx_client_read','_tinyx_framebuffer','_tinyx_framebuffer_stride','_malloc','_free']"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8']"
    )
endif()
```

Build it natively:

```sh
cmake -S . -B build/native \
    -DTINYX_BUILD_TESTS=ON

cmake --build build/native
ctest --test-dir build/native
```

Build through Emscripten:

```sh
emcmake cmake -S . -B build/wasm \
    -DTINYX_BUILD_WASM=ON \
    -DTINYX_BUILD_TESTS=OFF

cmake --build build/wasm
```

`emcmake` supplies the compiler and CMake toolchain; avoid manually setting `CC=emcc` if you can.

## Configuration headers

The current build generates `dix-config.h` and `kdrive-config.h`. For the sidecar build, make checked-in WASM-specific versions:

```text
cmake/wasm/dix-config.h
cmake/wasm/kdrive-config.h
```

Start with almost everything disabled:

```c
#ifndef TINYX_WASM_DIX_CONFIG_H
#define TINYX_WASM_DIX_CONFIG_H

#define X_BYTE_ORDER X_LITTLE_ENDIAN
#define SIZEOF_UNSIGNED_LONG 4

#define VENDOR_STRING "TinyX"
#define VENDOR_RELEASE 1

/* Intentionally absent:
 * XDMCP
 * HASXDMAUTH
 * DPMSExtension
 * HAS_MTRR_SUPPORT
 * HAS_MMAP
 * TCPCONN
 * UNIXCONN
 */

#endif
```

Expect to adjust this repeatedly while bringing sources in. The generated native header is useful as a reference:

```sh
./autogen.sh
./configure ...
less include/dix-config.h
```

Do not blindly copy it because it describes macOS, not WASM.

## Bring code into CMake by subsystem

Use the existing `Makefile.am` files as source manifests:

1. `mi`
2. `fb`
3. core `dix`
4. minimal extensions
5. memory framebuffer DDX
6. in-memory transport
7. fonts

Initially put everything in one `tinyx` static target. The old source tree has circular dependencies between components, and splitting it into many static libraries can introduce link-order problems before that organization provides much value.

Compile after every source group:

```sh
cmake --build build/wasm -j
```

Treat the compiler errors as the porting work queue.

## The first meaningful milestone

Don’t aim for “an X server in the browser” first. Aim for:

1. WASM module loads.
2. `tinyx_init()` allocates a 640×480 framebuffer.
3. `tinyx_framebuffer()` returns it.
4. JavaScript displays it on a canvas.
5. `tinyx_step()` returns immediately.
6. No filesystem, socket, signal, thread, or `select()` use.

Then wire in the X server initialization.

## The critical refactor

The current shape is:

```c
main()
{
    initialize_everything();

    while (...) {
        Dispatch(); /* eventually blocks in select() */
    }

    clean_up();
}
```

You want:

```c
int
tinyx_init(const tinyx_config *config)
{
    /* One-time part of dix/main.c. */
    return 0;
}

int
tinyx_step(uint32_t budget)
{
    /*
     * Poll injected input.
     * Process queued client requests.
     * Run expired timers.
     * Flush client output.
     * Return without waiting.
     */
    return requests_processed;
}

void
tinyx_shutdown(void)
{
    /* Cleanup part of dix/main.c. */
}
```

I would first extract `main()` into internal lifecycle functions while preserving the native executable:

```c
int
main(int argc, char **argv, char **envp)
{
    if (TinyXServerInit(argc, argv, envp) != 0)
        return 1;

    while (!TinyXServerShouldExit())
        TinyXServerDispatch();

    TinyXServerShutdown();
    return 0;
}
```

After that works, introduce the cooperative `tinyx_step()` implementation.

## Dependency policy

There are two kinds of X dependencies:

### Protocol headers

Headers such as:

```text
X11/Xproto.h
X11/extensions/renderproto.h
```

These are architecture-independent. Using installed host headers during early compilation is generally okay, though eventually pinning or vendoring `xorgproto` will make builds reproducible.

### Native libraries

Do **not** accidentally link Homebrew libraries into the WASM build. In particular:

- `libXfont`
- `libfontenc`
- `libXdmcp`
- `xtrans`

A native `pkg-config` success does not mean the library can be linked into WASM. The initial WASM target should avoid these altogether.

## Practical first commits

I’d organize the work into small commits:

1. `build: add empty CMake build`
2. `api: add public embedding interface`
3. `dix: separate server entry point from main`
4. `dispatch: add nonblocking request budget`
5. `os: add in-memory client transport`
6. `ddx: add memory framebuffer backend`
7. `input: expose pointer and keyboard injection`
8. `wasm: add Emscripten module and canvas demo`
9. `fonts: add embedded fallback font`

The main rule is: **don’t emulate Unix inside WASM unless necessary**. Replace sockets and blocking dispatch with the API the embedder actually wants.
