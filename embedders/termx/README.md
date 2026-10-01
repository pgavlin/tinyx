# TermX

TermX is a native reference embedder for the public `tinyx.h` API. It exposes
in-memory TinyX clients through a Unix-domain X11 socket, presents the memory
framebuffer using the Kitty graphics protocol, and translates terminal keyboard
and mouse events into TinyX input injection calls. It applies TinyX damage
rectangles directly to Kitty's root animation frame using compressed raw RGB
data.
When running in a local Kitty window, larger payloads use POSIX shared memory
to avoid base64 transfer through the terminal stream. A temporary file is used
if shared-memory creation fails, and direct transfer remains the remote and
small-payload fallback.

The Cargo build invokes the repository's CMake build and statically links
`libtinyx.a`. X.Org protocol headers must be installed. If they are outside the
compiler's default search path, set:

```sh
export TINYX_XORGPROTO_INCLUDE_DIR=/path/to/xorgproto/include
```

Build and run inside Kitty or another terminal implementing the Kitty graphics
protocol:

```sh
cargo run --manifest-path embedders/termx/Cargo.toml --release
```

An optional trailing command starts a client or session with `DISPLAY` set to
TermX's selected display. TermX exits when that command exits. For example:

```sh
termx --display 0 -- dwm
```

A launched command requires the standard display socket; `--socket` remains
available when clients are started separately.

The default socket is `/tmp/.X11-unix/X99`. In another shell, run an X client
with `DISPLAY=:99`. The host deliberately trusts clients admitted through its
local socket and is intended for development, not as a security boundary.

Options and corresponding environment variables are:

```text
--config PATH      TERMX_CONFIG
--display NUMBER   TINYX_DISPLAY
--socket PATH      TINYX_X11_SOCKET
--log PATH         TERMX_LOG
--trace-directory PATH  TERMX_TRACE_DIRECTORY
--dpi NUMBER       TINYX_DPI
-h, --help          Print command-line help
```

TermX reads `$XDG_CONFIG_HOME/termx/config.toml`, or
`$HOME/.config/termx/config.toml` when `XDG_CONFIG_HOME` is unset. A missing
default file is ignored. An explicit `--config` or `TERMX_CONFIG` path must
exist. For example:

```toml
display = 0
log = "/tmp/termx.log"
command = ["dwm"]
```

Supported keys are `display`, `socket`, `log`, `trace_directory`, `dpi`, and
`command`. Command-line values override environment values, which override file
values. A command on the command line replaces the configured command.

For protocol diagnosis, set `trace_directory` to a directory under which TermX
may create one run directory. Each client produces exact `client-N-c2s.bin` and
`client-N-s2c.bin` byte streams. Tracing is disabled by default.

Run `termx --benchmark-kitty [PATH]` inside Kitty to measure the graphics
transport without starting TinyX clients. Results default to
`/tmp/termx-kitty-benchmark-<pid>.csv`; an explicit path overrides that file. It reports acknowledgement latency for base
frames and cursor-sized, text-line, window-sized, and full-screen damage
updates across 320×200, 640×400, 1280×720, 1920×1080, and the current terminal
pixel dimensions. Each size is tested with disabled, adaptive, and forced zlib;
direct, temporary-file, and shared-memory transport; and 1024- and 4096-byte
direct chunks.

Press Control-C to exit. The X screen uses the full terminal pixel area and is
resized when that area changes. TinyX replaces its framebuffer and notifies
connected X11 clients through RandR. Runtime logs are written only to the
configured log file.

Before taking over terminal input, the host queries Kitty's `dpi_x` and `dpi_y`
capabilities and reports corresponding physical screen dimensions to X11. This
allows point-sized Xft fonts to follow the active Kitty window's logical DPI.
If the query is unavailable, the host falls back to 75 DPI. `--dpi` overrides
automatic detection on both axes; it is useful with other compatible terminals
or when a different UI scale is desired. Applications that request an explicit
pixel-sized font, including stock st's default `pixelsize=12`, do not scale in
response to DPI. Use a point-sized Xft pattern when launching st to make it
follow the detected DPI:

```sh
DISPLAY=:99 st -f 'Liberation Mono:size=12:antialias=true:autohint=true'
```

Enhanced terminal keyboard events are translated to the fixed US Xorg keymap
documented by `tinyx.h`. Explicit modifier-key events are preserved; when a
terminal reports modifiers only as flags, the host emits ordered synthetic
modifier presses and releases around the affected X keys.
