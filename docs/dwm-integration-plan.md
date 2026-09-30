# Running st and dwm on TinyX

## Status

This document plans native st and dwm integration through the Unix-domain X11
socket provided by the Kitty reference host. The first milestone is an
unmodified stock st process running directly on a one-screen TinyX display.
The second is an unmodified stock dwm process managing st and other ordinary
native X11 clients on that display.

No st- or dwm-specific TinyX API or server extension is expected. The purpose
of the work is to validate the existing core protocol, Render, input,
selection, resize, and socket-host paths together and fix general X11
compatibility defects exposed by real applications.

Stock st 0.9.3 has completed initial direct bring-up on the Kitty-hosted display
using macOS XQuartz client libraries. Xft text rendering, shell output, XTEST
keyboard input, primary selection, clipboard transfer, and explicit window
resizing all worked. The bring-up exposed and fixed a CMake configuration bug
that compiled KDrive without `kdrive-config.h` on 64-bit hosts, giving KDrive
and DIX incompatible `KeySym` widths and corrupting `GetKeyboardMapping`.
Native and swapped protocol tests now cover keyboard-map reads and changes.
Stock dwm 6.8 also starts successfully, renders its Xft bar, installs its EWMH
root properties, and rejects a second window manager. The Kitty host now
reports its detected logical DPI, producing a correctly scaled dwm bar on
HiDPI displays. Running st under dwm and manual terminal-to-Kitty input remain
to be validated.

Compiling dwm, Xlib, and application processes into WebAssembly is a separate
project. It would require a client-side Xlib transport and replacements for
dwm's native `fork()` and `exec()` behavior; it is not part of this plan.

## Why st and dwm should fit

Stock st uses:

- core Xlib window, event, property, selection, cursor, and drawing operations;
- Xft and fontconfig for client-side font selection and rasterization;
- the Render extension used by Xft to upload and composite glyphs;
- X input methods when available, with a fallback when no input method is
  configured;
- a pseudoterminal and ordinary host process facilities that remain entirely
  inside the native client process.

st does not require a window manager for initial bring-up. Running it directly
therefore isolates application rendering, input, selection, and resize behavior
from window-manager redirection and policy.

Stock dwm 6.8 uses:

- core Xlib window, event, property, focus, cursor, and grab operations;
- Xft and fontconfig for client-side font selection and rasterization;
- the Render extension used by Xft to upload and composite glyphs;
- the core cursor font through `XCreateFontCursor()`;
- Xinerama when compiled with optional multi-monitor support.

It does not require Composite, GLX, MIT-SHM, XKB, or a compositor. TinyX
already has the relevant core DIX behavior, a Render/FB implementation, the
complete cursor bitmap font, multiple memory clients, a depth-24 memory screen,
input injection, runtime resizing, and the Kitty socket bridge.

The server-side BDF/PCF provider is not required. Xft opens fonts using
fontconfig and FreeType in the st or dwm process, rasterizes glyphs there, and
sends glyph images to TinyX through Render. This makes st a direct validation
that modern client-side fonts work independently of the server's core bitmap
font catalog.

## Initial scope

The initial supported configuration is:

- unmodified native st, first without and then with dwm;
- native dwm and other native X11 applications;
- the Kitty embedder as the TinyX host and presenter;
- one TinyX screen and one dwm monitor;
- a local trusted Unix-domain display socket;
- the stock dwm rendering and event loop;
- an available client-side Xft font such as `monospace:size=10`;
- clients launched externally or through host-appropriate commands.

The following are deferred:

- multiple physical or logical monitors;
- enabling the disabled RandR-backed Xinerama compatibility extension;
- a compositor, transparency, or Composite-based effects;
- a complete desktop session launcher;
- sandboxing mutually untrusted socket clients;
- a WASM build of dwm or Xlib;
- host-provided core bitmap fonts.

## Bring-up procedure

### 1. Build st and dwm for the host

Build stock st against the host's Xlib, Xft, fontconfig, and FreeType. Adjust
its `config.mk` include and library paths as needed, and ensure its configured
font is available through the host's fontconfig installation. st's shell and
pseudoterminal are native client facilities and require no TinyX integration.

Build stock dwm against the same host libraries. For the first run, disable
Xinerama by removing `-DXINERAMA` and `-lXinerama`; TinyX currently exposes one
screen and does not initialize its Xinerama compatibility extension.

Stock dwm's default launcher expects `st` and `dmenu_run`. Installing the test
st binary at the configured path makes the default terminal binding useful;
alternatively edit `config.h` or launch clients from another shell.

### 2. Start the display host

Run the Kitty host on an unused display number:

```sh
cargo run --manifest-path embedders/kitty/Cargo.toml --release -- --display 100
```

The host creates `/tmp/.X11-unix/X100`, maps each accepted socket connection to
a TinyX logical client, continuously steps the server, injects terminal input,
and presents framebuffer damage.

### 3. Run st directly

Before introducing a window manager, run:

```sh
DISPLAY=:100 ./st
```

Validate terminal output, typing, pointer selection, clipboard transfer, focus,
and terminal-window resizing. This isolates Xft/Render, keyboard lookup, XIM
fallback, selections, and ordinary configure/expose handling. Exit st before
the initial dwm run so that window-manager startup behavior can be tested from
a clean root window.

### 4. Start dwm

In another shell:

```sh
DISPLAY=:100 ./dwm
```

dwm should successfully select `SubstructureRedirectMask` on the root window,
create its supporting-WM window and bar, install passive input grabs, and enter
its `XNextEvent()` loop.

### 5. Start representative clients

For example:

```sh
DISPLAY=:100 ./st
DISPLAY=:100 /opt/X11/bin/xterm
DISPLAY=:100 /opt/X11/bin/xclock
```

Starting dwm before ordinary clients is preferable, although its startup scan
should also adopt suitable pre-existing top-level windows. Once direct st and
dwm bring-up pass independently, st under dwm is the primary compact desktop
integration test.

## Validation slices

### Slice 1: st application behavior

st is the first high-value Render and interactive-client integration test.
Validate:

- Xft/fontconfig font matching succeeds entirely in the client process;
- Render format discovery, glyph-set creation, glyph upload, and composition;
- antialiased ASCII and UTF-8 text reaches the framebuffer and produces damage;
- typing and modifier transitions produce the expected terminal bytes;
- XIM initialization succeeds or falls back cleanly when no input method is
  available;
- primary selection ownership, `SelectionRequest`, `SelectionNotify`, and
  property transfer support copy and paste;
- pointer selection, cursor changes, focus events, and expose handling work;
- configure events resize the pseudoterminal and redraw the backing pixmap.

Completion signal: an interactive shell in stock st renders continuously,
accepts keyboard and mouse input, copies and pastes text, and survives repeated
resizes without server-side font-path changes. Automated direct bring-up has
validated rendering, synthetic keyboard input, both selections, and resizing;
manual Kitty input remains outstanding.

### Slice 2: dwm Xft and Render

The dwm bar is the next high-value Render integration test. Validate:

- `XRenderQueryExtension()` and format discovery;
- matching the root visual to a Render picture format;
- Xft color allocation against the default TrueColor colormap;
- glyph-set creation, glyph upload, and glyph destruction;
- antialiased glyph composition into dwm's depth-24 backing pixmap;
- `XCopyArea()` from the backing pixmap into the bar window;
- framebuffer damage and Kitty presentation of the resulting pixels;
- UTF-8 text and Xft fallback-font behavior.

A small standalone Xft smoke client may be added if diagnosing dwm directly is
too noisy. It should draw known text into a mapped window and permit pixel and
damage assertions in a descriptor-free integration test.

Completion signal: the tag labels, layout symbol, window title, and status text
appear correctly in the dwm bar without server-side font-path changes.

### Slice 3: window-manager ownership and redirection

Validate the core behavior on which every non-reparenting window manager
relies:

- the first client may select `SubstructureRedirectMask` on the root;
- a second client attempting the same selection receives `BadAccess`;
- mapping an unmanaged top-level window produces `MapRequest` for dwm;
- client geometry requests produce `ConfigureRequest` when redirected;
- dwm can map, unmap, resize, restack, and change border widths;
- `XQueryTree()` and startup scanning report existing children correctly;
- override-redirect windows bypass management as required;
- destroying or disconnecting dwm releases root event ownership.

These should become protocol-level regression tests with two descriptor-free
clients, independently of the Kitty host.

Completion signal: newly launched xterm windows are tiled and bordered by dwm
rather than appearing unmanaged.

### Slice 4: properties, focus, and client messages

Validate the ICCCM and EWMH mechanisms used by stock dwm:

- atom creation and lookup;
- `WM_PROTOCOLS`, `WM_DELETE_WINDOW`, `WM_TAKE_FOCUS`, and `WM_STATE`;
- `_NET_SUPPORTED`, `_NET_SUPPORTING_WM_CHECK`, `_NET_ACTIVE_WINDOW`,
  `_NET_CLIENT_LIST`, `_NET_WM_NAME`, and fullscreen/window-type properties;
- property-change events and root-name status updates;
- input focus changes and focus events;
- synthetic client messages used to close or activate windows.

Completion signal: focus follows dwm policy, titles and status changes update,
fullscreen toggling works, and client close requests follow `WM_DELETE_WINDOW`
when supported.

### Slice 5: keyboard and pointer grabs

Validate both the core server and Kitty input adapter:

- core keyboard mapping exposes the expected Xorg keycodes and modifiers;
- `XGrabKey()` installs dwm's passive grabs, including lock-mask variants;
- Alt-based tag, layout, focus, and spawn bindings receive press/release pairs;
- `XGrabButton()` and active pointer grabs work for focus and move/resize;
- pointer motion, button transitions, and grab release preserve ordering;
- `EnterNotify`, button, motion, key, and focus events reach the right client;
- focus loss releases synthetic host modifiers and server key state.

Stock dwm defaults to Mod1/Alt, which is preferred for initial testing. Kitty or
the surrounding terminal may consume some host shortcuts before Crossterm can
report them; such conflicts are host keybinding issues rather than X server
failures and should be documented.

Completion signal: keyboard-driven tagging/layout operations, bar clicks,
window focus, and pointer move/resize all work without stuck modifiers or
buttons.

### Slice 6: cursors

Validate dwm's calls to `XCreateFontCursor()` for normal, move, and resize
cursors. TinyX's embedded cursor font should satisfy these without filesystem
font access. Software cursor drawing must participate correctly in framebuffer
presentation and damage.

Completion signal: cursor creation produces no X errors and the expected cursor
changes are visible over the root, client, and drag regions.

### Slice 7: runtime resize

The Kitty host resizes TinyX when terminal pixel capacity changes. dwm does not
need RandR for its basic one-monitor response; it handles root
`ConfigureNotify` and recalculates monitor and bar geometry.

Validate:

- `tinyx_server_resize()` changes the root dimensions atomically;
- dwm receives the root geometry notification;
- dwm resizes its bar backing pixmap and bar window;
- managed clients are rearranged within the new work area;
- pointer coordinate mapping follows the new framebuffer;
- repeated growth and shrink cycles do not leave stale pixels or geometry.

Completion signal: resizing the outer terminal updates the dwm bar and tiles
without restarting dwm or its clients.

## Xinerama policy

`randr/rrxinerama.c` contains a one-screen RandR-backed implementation, but
`RRXineramaExtensionInit()` currently returns without registering it. Stock dwm
handles an absent or inactive Xinerama extension by creating one monitor from
the default screen, so this is not an initial blocker.

Xinerama should only be enabled after TinyX has a defined multi-monitor model.
Advertising several monitor rectangles over one framebuffer would require
public configuration, resize semantics, input-coordinate policy, RandR event
behavior, and host presentation decisions. Enabling the extension merely to
report one rectangle has little benefit over dwm's existing fallback.

## Test strategy

The bring-up should produce reusable tests rather than only a manual success
report:

1. **Render smoke test:** upload and composite an antialiased glyph, then check
   framebuffer pixels and damage.
2. **Selection test:** exercise primary selection ownership and conversion
   between two descriptor-free clients.
3. **WM ownership test:** use two clients to verify root redirect ownership and
   `BadAccess`.
4. **Map/configure redirection test:** verify `MapRequest` and
   `ConfigureRequest` wire events.
5. **Property/focus test:** exercise the subset of ICCCM/EWMH behavior dwm and
   st use.
6. **Grab/input test:** install passive grabs, inject host input, and verify
   event routing and modifier state.
7. **Resize test:** keep application and WM clients connected across repeated
   screen changes and verify notifications and final geometry.
8. **Manual st test:** run stock st directly through the Kitty socket and record
   rendering, typing, selection, clipboard, focus, and resize results.
9. **Manual desktop test:** run stock dwm with st, xterm, and xclock through the
   Kitty socket and record the tested host versions and configuration.

Protocol tests should run in native CMake, Emscripten/Node where applicable,
and the Autotools embedded-font build. The actual dwm process and Kitty
presentation test is native-only.

## Failure diagnosis

| Symptom | Most likely boundary |
|---|---|
| st opens but text is absent or corrupt | Xft/Render formats, glyph upload, composition, or framebuffer damage |
| st cannot type non-ASCII text | XIM initialization, keyboard mapping, or keysym translation |
| st selection works but paste does not | selection ownership, conversion events, or property transfer |
| st does not track size changes | configure delivery, size hints, or pseudoterminal resize handling |
| dwm exits with "another window manager is already running" | root event-mask ownership or unexpected existing WM |
| dwm starts but no bar text appears | Xft/Render formats, glyph upload, or composition |
| bar appears but new windows are unmanaged | substructure redirect or `MapRequest` delivery |
| windows map but cannot be focused or rearranged | focus, configure, stacking, or property behavior |
| Alt bindings do nothing | passive grabs, keyboard map, or terminal modifier reporting |
| mouse focus works but dragging fails | active pointer grabs or motion delivery |
| cursors fail or remain invisible | cursor-font opening or software cursor presentation |
| layout remains at the old size | root `ConfigureNotify` or dwm resize handling |
| only Xinerama initialization fails | compile dwm without Xinerama; it is optional |

## Completion criteria

Initial st and dwm support is complete when:

- stock native st runs directly on the Kitty-hosted TinyX display;
- st renders Xft text, accepts input, supports selection and paste, and tracks
  repeated resizes without server-side font configuration;
- stock native dwm starts on the Kitty-hosted TinyX display;
- dwm owns root substructure redirection and a second WM is rejected;
- the Xft-rendered bar is legible without host-provided server fonts;
- st, xterm, and xclock windows are discovered, tiled, focused, restacked, and
  closed correctly;
- default Alt keyboard bindings and pointer interactions work;
- dwm cursors are visible and update during pointer operations;
- terminal-driven screen resizing rearranges the bar and managed clients;
- disconnecting dwm does not poison TinyX or other clients;
- general compatibility fixes have automated regression coverage;
- Xinerama and WASM dwm remain explicitly documented as deferred work.
