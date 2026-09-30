# Building TinyX

TinyX retains Autotools for the historical native servers and provides a
sidecar CMake build for the embeddable memory host. The two builds intentionally
coexist while the CMake source/configuration manifests are validated against
the native reference build.

## Dependencies

Both builds require a C99 compiler and X.Org protocol headers (`xorgproto`).
These headers describe the wire protocol and are not linked libraries.

The CMake native build also requires Xtrans headers because it retains the
native socket adapter. The Emscripten build does not use Xtrans, libXfont,
libfontenc, libXdmcp, native authorization, hardware backends, or native
`pkg-config` results.

If CMake cannot locate headers automatically, set:

```sh
-DTINYX_XORGPROTO_INCLUDE_DIR=/path/to/xorgproto/include
-DTINYX_XTRANS_INCLUDE_DIR=/path/to/xtrans/include # native only
```

## Native CMake build

The default CMake product uses the memory display, embedded fonts, in-memory
clients, and public API from `include/tinyx.h`:

```sh
cmake -S . -B build/native -G Ninja \
    -DTINYX_XORGPROTO_INCLUDE_DIR=/path/to/xorgproto/include \
    -DTINYX_XTRANS_INCLUDE_DIR=/path/to/xtrans/include
cmake --build build/native
ctest --test-dir build/native --output-on-failure
```

This produces:

- `libtinyx.a`, the embeddable static library;
- `tinyx-embed-example`, the native API example;
- `tinyx-api-test` and `tinyx-embedded-font-data-test` when tests are enabled.

Useful options are:

```text
TINYX_BUILD_TESTS    build CTest tests (defaults to BUILD_TESTING)
TINYX_BUILD_EXAMPLE  build the native example (default ON)
TINYX_BUILD_WASM     build the Emscripten module (default ON under Emscripten)
```

Install the static library, public header, and CMake target with:

```sh
cmake --install build/native --prefix /desired/prefix
```

Installed CMake consumers can use the exported `TinyX::tinyx` target.

## Kitty terminal host

`embedders/kitty` is a Rust reference host for the public C API. Its Cargo
build invokes the sidecar CMake build, listens for ordinary X11 clients on a
Unix-domain socket, presents framebuffer damage with the Kitty graphics
protocol, and injects terminal keyboard and mouse input:

```sh
cargo run --manifest-path embedders/kitty/Cargo.toml --release
# In another shell:
DISPLAY=:99 xclock
```

The default display is `:99`. See `embedders/kitty/README.md` for socket, log,
and dependency configuration. This development host trusts clients admitted
through its local socket. The native window-manager bring-up and compatibility
test plan is documented in [Running dwm on TinyX](dwm-integration-plan.md).

## Emscripten build

Use Emscripten's CMake toolchain rather than setting `CC=emcc` manually:

```sh
emcmake cmake -S . -B build/wasm -G Ninja \
    -DTINYX_XORGPROTO_INCLUDE_DIR=/path/to/xorgproto/include \
    -DTINYX_BUILD_EXAMPLE=OFF
cmake --build build/wasm
ctest --test-dir build/wasm --output-on-failure
```

The result is `tinyx-wasm.js` plus `tinyx-wasm.wasm`. The JavaScript file is a
modularized factory named `createTinyXModule`. The module exports only the v1
TinyX API, `malloc`, and `free`; the explicit list is maintained in
`cmake/wasm-exports.json`. `addFunction`, `removeFunction`, and `HEAPU8` are
available to install host callbacks and inspect API buffers.

The WASM source manifest excludes Xtrans, native access control and
authorization, XDMCP, MIT-SHM, XF86BIGFONT, Linux input, fbdev, and VESA. It
also disables the native signal-driven scheduler. Logical clients use the
in-memory transport and the memory display performs no presentation.

CTest runs the same API integration and embedded-font tests under Node through
Emscripten's cross-compiling emulator. This checks complete server startup,
the X11 setup handshake, bounded client streams, and shutdown inside WASM.

## Generated configuration

CMake generates `dix-config.h` and `kdrive-config.h` in its build directory
from `cmake/*.in`. It performs compile-time target checks through the selected
CMake toolchain and uses explicit embedded-product policy for extensions and
host services. It never imports an Autotools-generated host configuration.

`xorgproto` is currently supplied as an external header tree. Pinning or
vendoring it can be added later for fully hermetic builds.

## Autotools reference build

The native build normally uses legacy libXfont 1 and libfontenc:

```sh
./autogen.sh
./configure
make
make check
```

A filesystem-free build uses TinyX's embedded bitmap backend:

```sh
./configure --disable-fonts [other options]
make
make check
```

Despite the historical option name, this configuration has working font
support. It registers a `built-ins` font-path element containing the X.Org
complete 4,121-glyph ISO10646-1 6x13 `fixed` font and complete cursor font,
and links neither libXfont nor libfontenc. Supported aliases and generated-data provenance are documented in
[`fonts/README.md`](../fonts/README.md).

The Autotools build remains the reference for `Xfbdev`, `Xvesa`, native Xtrans
listeners, filesystem fonts, and other historical host features. Those native
products are deliberately not duplicated by the Phase 9 CMake build.
