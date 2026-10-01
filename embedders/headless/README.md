# Headless embedder

This is the smallest native consumer of the public `tinyx.h` API. It creates a
memory-backed server, inspects its framebuffer, and shuts it down without
opening sockets or presenting pixels.

Build and run it with:

```sh
cmake -S . -B build/native -G Ninja
cmake --build build/native --target tinyx-embedder-headless
./build/native/tinyx-embedder-headless
```

Real embedders add their own client acquisition, presentation, and input event
sources. `embedders/termx` is the complete native reference embedder.
