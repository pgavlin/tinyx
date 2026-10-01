# TinyX embedders

TinyX is a library, not a standalone display server. A runnable embedder owns:

- acquiring X11 clients and moving their ordered byte streams through
  `tinyx_client_send()` and `tinyx_client_receive()`;
- deciding when to call `tinyx_server_step()`;
- presenting framebuffer damage;
- acquiring keyboard and pointer input;
- host logging, time, wakeups, bell, and LED effects;
- client admission and any external transport security.

The target platform and the embedder are separate choices. `libtinyx.a` is the
native-machine library and `tinyx-wasm` is its WebAssembly packaging; neither
is itself an interactive host.

Included embedders:

- [`headless`](headless/README.md) is the smallest native C consumer;
- [`termx`](termx/README.md) bridges a Unix-domain X11 socket, terminal input,
  and the Kitty graphics protocol.

Future SDL, Cocoa, browser, or application-specific embedders should use the
same public API rather than adding acquisition or presentation policy to the
core.
