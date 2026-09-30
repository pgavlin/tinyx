# TinyX Kitty embedder

This is a native reference host for the public `tinyx.h` API. It exposes
in-memory TinyX clients through a Unix-domain X11 socket, renders the memory
framebuffer as a PNG using the Kitty graphics protocol, and translates terminal
keyboard and mouse events into TinyX input injection calls.

The Cargo build invokes the repository's sidecar CMake build and statically
links `libtinyx.a`. The X.Org protocol and Xtrans headers must be installed. If
they are outside the compiler's default search path, set:

```sh
export TINYX_XORGPROTO_INCLUDE_DIR=/path/to/xorgproto/include
export TINYX_XTRANS_INCLUDE_DIR=/path/to/xtrans/include
```

Build and run inside Kitty or another terminal implementing the Kitty graphics
protocol:

```sh
cargo run --manifest-path embedders/kitty/Cargo.toml --release
```

The default socket is `/tmp/.X11-unix/X99`. In another shell, run an X client
with `DISPLAY=:99`. The host deliberately trusts clients admitted through its
local socket and is intended for development, not as a security boundary.

Options and corresponding environment variables are:

```text
--display NUMBER   TINYX_DISPLAY
--socket PATH      TINYX_X11_SOCKET
--log PATH         TINYX_KITTY_LOG
```

Press Control-C to exit. The X screen is sized to the terminal pixel area
available above the log when the host starts. API v1 does not support live
screen resizing, so later terminal resizes preserve and fit that initial screen
rather than changing its X11 geometry.

Enhanced terminal keyboard events are translated to the fixed US Xorg keymap
documented by `tinyx.h`. Explicit modifier-key events are preserved; when a
terminal reports modifiers only as flags, the host emits ordered synthetic
modifier presses and releases around the affected X keys.
