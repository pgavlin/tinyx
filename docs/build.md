I’d start with a **sidecar CMake build**, leaving Autoconf intact as a reference. Don’t try to convert the entire build system before understanding which code belongs in the embedded server.

## Embedded fonts without libXfont

The native build normally requires legacy libXfont 1 and libfontenc. A
filesystem-free build instead uses TinyX's embedded bitmap backend:

```sh
./configure --disable-fonts [other options]
make
```

Despite the historical option name, this configuration has working font
support. It registers a `built-ins` font-path element containing the X.Org
6x13 `fixed` font and the complete cursor font, and does not link either
libXfont or libfontenc. The default font path is `built-ins`, so `Xmemory` can
complete generation startup without a font filesystem.

The embedded catalog supports `fixed`, `6x13`, `cursor`, the canonical 6x13
XLFD, and its historical 100-dpi alias. Other font names and filesystem font
paths fail through normal X11 font errors. The default font-enabled build
continues to use libXfont and its native filesystem behavior.

The generated data's provenance and regeneration command are documented in
[`fonts/README.md`](../fonts/README.md).

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

## Public embedding API

Phase 8 established the installed `include/tinyx.h` facade. Its opaque handles,
configuration structures, status model, ownership rules, and scheduling
contract are documented in the [Phase 8 Embedding API](embedding-api-design.md).
`kdrive/memory/embed-example.c` is the current native in-process example.

The Phase 9 build should compile that existing facade into the `tinyx` static
library rather than introduce another API or an `embed/` replacement layer.

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
    kdrive/memory/api.c
    kdrive/memory/memory.c
    kdrive/memory/meminit.c

    # Add the existing core groups incrementally:
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
        # Populate this from the deliberate TINYX_API declarations in tinyx.h.
        "SHELL:-s EXPORTED_FUNCTIONS=@TINYX_EXPORTED_FUNCTIONS@"
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
2. `tinyx_server_create()` initializes a 640×480 screen.
3. `tinyx_server_get_framebuffer()` returns it.
4. JavaScript displays it on a canvas.
5. `tinyx_server_step()` returns immediately.
6. No filesystem, socket, signal, thread, or `select()` use.

The native embedding facade already exercises the complete server
initialization path. Phase 9 must reproduce its source configuration rather
than substitute a framebuffer-only stub.

## Lifecycle refactor already completed

The historical shape was:

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

The refactored code now places the native entry point in
`dix/main-entry.c`, reusable lifecycle operations in `dix/lifecycle.c`, and the
public adapter in `kdrive/memory/api.c`. The native driver still performs
blocking dispatch, while `tinyx_server_step()` uses the bounded nonblocking
path. Keep those source boundaries intact in the sidecar build and omit
`main-entry.c` from the static embedding library.

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
