TinyX can become a WASM-capable library, but it needs an embedding layer rather than simply compiling the current executable with emcc.

### Recommended initial target

- Emscripten/browser
- One server instance per WASM module
- In-memory X11 transport:
    - tinyx_client_open()
    - tinyx_client_write()
    - tinyx_client_read()
    - tinyx_client_close()
- Non-blocking execution via tinyx_step() instead of Dispatch()’s infinite select() loop
- Memory-backed 32-bit framebuffer exposed to JavaScript
- Pointer and keyboard injection APIs
- Disable VESA/fbdev, XDMCP, shared memory, DPMS, and OS authorization
- Initially use bundled/built-in bitmap fonts rather than filesystem-dependent libXfont

### Main refactoring stages

1. Split dix/main.c into reusable initialize/step/reset/shutdown functions.
2. Add a public C API under include/tinyx.h.
3. Add an in-memory transport backend replacing xtrans, file descriptors, and select().
4. Add a memory framebuffer KDrive backend.
5. Replace Linux input with explicit injection functions.
6. Add an Emscripten build and a small browser example.
7. Gradually restore optional extensions and font support.

The largest constraints are the global singleton state, blocking dispatcher, POSIX socket layer, hardware-specific KDrive backends, and old external X protocol/font dependencies.

Before implementing, the important choice is the WASM environment. Should I proceed with the browser/Emscripten and in-memory transport design above, or are you targeting WASI/Node
and real sockets?

I would **not port the whole server to Rust** yet. The DIX/MI/FB code contains decades of subtle X11 behavior; rewriting it would turn a WASM-porting project into a multi-year protocol compatibility project.

A better architecture is:

- Keep the proven X server core in C.
- Replace its platform boundaries:
  - process lifecycle
  - socket transport
  - event loop
  - framebuffer DDX
  - input
  - fonts/filesystem
- Expose a small, stable C ABI.
- Optionally write the browser/runtime-facing layer in Rust and compile/link both to WASM.

### Suggested implementation order

1. **Create a separate WASM build**
   - Don’t initially force Autotools to understand WASM.
   - Make an explicit CMake/Meson source list or an `emcc` response file.
   - Disable most extensions and all Linux/VESA/fbdev code.

2. **Make dispatch cooperative**
   - Extract initialization from `main()`.
   - Refactor `Dispatch()` into something like:
     ```c
     int tinyx_step(unsigned request_budget);
     ```
   - It must never call blocking `select()`.

3. **Implement an in-memory transport**
   ```c
   int tinyx_client_open(void);
   int tinyx_client_write(int client, const void *data, size_t size);
   size_t tinyx_client_read(int client, void *data, size_t capacity);
   void tinyx_client_close(int client);
   ```
   Treat each client as input/output byte queues. This is much cleaner than pretending WASM clients are file descriptors.

4. **Implement a memory-backed DDX**
   - Width, height, stride, and pixel format supplied at initialization.
   - Use the existing FB/MI rendering implementation.
   - Expose framebuffer memory and dirty rectangles.

5. **Inject input explicitly**
   ```c
   void tinyx_pointer_motion(int x, int y);
   void tinyx_pointer_button(unsigned button, bool down);
   void tinyx_key(unsigned keycode, bool down);
   ```

6. **Handle fonts last**
   - `libXfont` and filesystem font paths may be the ugliest dependency.
   - Start with one embedded bitmap font or temporarily reject font requests.

### Where Rust would help

Rust is a good fit for:

- client queue ownership
- JavaScript bindings
- WebSocket/proxy transport
- framebuffer presentation
- font loading
- lifecycle and resource management

But keep a deliberately boring ABI between Rust and C:

```c
typedef struct tinyx_config {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    void *framebuffer;
} tinyx_config;

int tinyx_init(const tinyx_config *);
int tinyx_step(uint32_t budget);
void tinyx_shutdown(void);
```

Initially, assume **one server per WASM instance**, because the existing core is heavily global. Trying to make it reentrant at the same time would greatly expand the project.

Only consider a full Rust rewrite after this version works and you have protocol conformance tests plus recorded client/server traces. At that point, you could replace C subsystems incrementally rather than betting everything on a clean-room rewrite.
