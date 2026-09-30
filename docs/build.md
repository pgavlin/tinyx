# Building TinyX

TinyX uses CMake for its native embeddable library, tests, examples, and
Emscripten module. The source manifests are explicit in the root
`CMakeLists.txt`.

## Dependencies

All builds require a C99 compiler and X.Org protocol headers (`xorgproto`).
These headers describe the wire protocol and are not linked libraries.

The native build also requires Xtrans headers because it retains the native
socket adapter. The Emscripten build does not use Xtrans, libXfont,
libfontenc, libXdmcp, native authorization, hardware backends, or native
`pkg-config` results.

If CMake cannot locate headers automatically, set:

```sh
-DTINYX_XORGPROTO_INCLUDE_DIR=/path/to/xorgproto/include
-DTINYX_XTRANS_INCLUDE_DIR=/path/to/xtrans/include # native only
```

## Native build

The default product uses the memory display, embedded fonts, in-memory clients,
and public API from `include/tinyx.h`:

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
- API, configured-visual, byte-order, keyboard-map, font, and retained-client
  tests when testing is enabled.

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

The prerelease API supports creation-time ordered pixmap depths and exact X11
visuals through `tinyx_depth_config` and `tinyx_visual_config`. A custom list is
complete, must include the depth-1 bitmap format, and is copied during server
creation. The presented root remains depth-24 TrueColor in 32-bpp storage;
non-root depth 2 in 8 bpp and depth 12 in 16 bpp are supported for indexed
colormaps, windows, pixmaps, GCs, and core drawing. The native and Emscripten
`tinyx-visual-config` test exercises a big-endian client against this topology.

## Kitty terminal host

`embedders/kitty` is a Rust reference host for the public C API. Its Cargo
build invokes CMake, listens for ordinary X11 clients on a Unix-domain socket,
presents framebuffer damage with the Kitty graphics protocol, and injects
terminal keyboard and mouse input:

```sh
cargo run --manifest-path embedders/kitty/Cargo.toml --release
# In another shell:
DISPLAY=:99 xclock
```

The default display is `:99`. The host queries Kitty for its logical DPI and
supports an explicit `--dpi` override. See `embedders/kitty/README.md` for
socket, log, DPI, and dependency configuration. This development host trusts
clients admitted through its local socket. Completed native stock-st and
stock-dwm bring-up, configuration, and validation are documented in
[Running st and dwm on TinyX](dwm-integration-plan.md).

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
modularized factory named `createTinyXModule`. The module exports only the
TinyX API, `malloc`, and `free`; the explicit list is maintained in
`cmake/wasm-exports.json`. `addFunction`, `removeFunction`, and `HEAPU8` are
available to install host callbacks and inspect API buffers.

The WASM source manifest excludes Xtrans, native access control and
authorization, XDMCP, MIT-SHM, XF86BIGFONT, Linux input, fbdev, and VESA. It
also disables the native signal-driven scheduler. Logical clients use the
in-memory transport and the memory display performs no presentation.

CTest runs the same API and protocol tests under Node through Emscripten's
cross-compiling emulator.

## Generated configuration

CMake generates `dix-config.h` and `kdrive-config.h` in its build directory
from `cmake/*.in`. It performs compile-time target checks through the selected
CMake toolchain and uses explicit embedded-product policy for extensions and
host services.

`xorgproto` is currently supplied as an external header tree. Pinning or
vendoring it can be added later for fully hermetic builds.

## Product scope

The supported product is the embeddable memory-display server. Historical
fbdev, VESA, Linux-console, filesystem-font, and standalone native-server
sources remain in the tree for reference but are not CMake products.
