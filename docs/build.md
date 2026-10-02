# Building TinyX

TinyX uses CMake for its library, tests, reference embedders, and Emscripten
module. The source manifests are explicit in the root `CMakeLists.txt`.

## Dependencies

All builds require a C99 compiler and X.Org protocol headers (`xorgproto`).
These headers describe the wire protocol and are not linked libraries. TinyX
does not require Xtrans, libXfont, libfontenc, libXdmcp, or native
`pkg-config` metadata.

If CMake cannot locate headers automatically, set:

```sh
-DTINYX_XORGPROTO_INCLUDE_DIR=/path/to/xorgproto/include
```

## Native library and embedders

The default native-toolchain build produces the platform's static TinyX
library. Runnable programs are embedders over the public API in
`include/tinyx.h`; the core itself does not open listening sockets or acquire
clients:

```sh
cmake -S . -B build/native -G Ninja \
    -DTINYX_XORGPROTO_INCLUDE_DIR=/path/to/xorgproto/include
cmake --build build/native
ctest --test-dir build/native --output-on-failure
```

This produces:

- `libtinyx.a`, the native static library;
- `tinyx-embedder-headless`, the minimal headless C embedder;
- API, configured-visual, byte-order, keyboard-map, font, and retained-client
  tests when testing is enabled.

Useful options are:

```text
TINYX_BUILD_TESTS               build CTest tests (defaults to BUILD_TESTING)
TINYX_BUILD_HEADLESS_EMBEDDER   build the headless embedder (default ON)
TINYX_BUILD_WASM                build the Emscripten module (default ON under Emscripten)
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
colormaps, windows, pixmaps, GCs, and core drawing. Optional
`tinyx_overlay_visual_config` records add pixel-transparency allocation
semantics to individual indexed visuals without enabling overlay composition.
The native and Emscripten `tinyx-visual-config` test exercises a big-endian
client against ordinary and transparent-overlay visuals in this topology.

## Reference embedders

`embedders/headless` is the smallest native consumer. It creates a server,
inspects the framebuffer, and exits without acquiring clients or presenting
pixels.

### TermX terminal embedder

`embedders/termx` is a complete native embedder for the public C API. Its Cargo
build invokes CMake, listens for ordinary X11 clients on a Unix-domain socket,
presents framebuffer damage with the Kitty graphics protocol, and injects
terminal keyboard and mouse input:

```sh
cargo run --manifest-path embedders/termx/Cargo.toml --release
# In another shell:
DISPLAY=:99 xclock
```

The default display is `:99`. The host queries Kitty for its logical DPI and
supports an explicit `--dpi` override. See `embedders/termx/README.md` for
socket, log, DPI, and dependency configuration. This development host trusts
clients admitted through its local socket. Completed native stock-st and
stock-dwm bring-up, configuration, and validation are documented in
[Running st and dwm on TinyX](dwm-integration-plan.md).

## Emscripten build

Use Emscripten's CMake toolchain rather than setting `CC=emcc` manually:

```sh
emcmake cmake -S . -B build/wasm -G Ninja \
    -DTINYX_XORGPROTO_INCLUDE_DIR=/path/to/xorgproto/include \
    -DTINYX_BUILD_HEADLESS_EMBEDDER=OFF
cmake --build build/wasm
ctest --test-dir build/wasm --output-on-failure
```

The result is `tinyx-wasm.js` plus `tinyx-wasm.wasm`. The JavaScript file is a
modularized factory named `createTinyXModule`. The module exports only the
TinyX API, `malloc`, and `free`; the explicit list is maintained in
`cmake/wasm-exports.json`. `addFunction`, `removeFunction`, and `HEAPU8` are
available to install host callbacks and inspect API buffers.

Like the native library, the WASM module uses only descriptor-free logical
clients and embedder-owned acquisition. WASM additionally excludes MIT-SHM and
XF86BIGFONT. Neither target includes Xtrans, native access control,
authorization, XDMCP, Linux input, fbdev, or VESA. The memory display performs
no presentation.

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
