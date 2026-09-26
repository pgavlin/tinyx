# TinyX, Layer by Layer

This document is a companion to [the architecture tour](architecture.md). It
follows the order in which the native TinyX executables are linked and explains
what each archive contributes, which interfaces join it to its neighbors, and
which external packages it brings into the final program.

The order is worth following because TinyX is assembled from static archives,
not from independently versioned shared libraries. The boundaries are useful,
but they are not strict one-way dependencies.

## 1. How to read the layers

The final targets are approximately:

```text
Xfbdev = fbinit.o + libfbdev.a + KDRIVE_LIBS + XSERVER_LIBS
Xvesa  = vesainit.o + libvesa.a  + KDRIVE_LIBS + XSERVER_LIBS
```

`KDRIVE_LIBS` expands to the internal archives described below. Every `.c`
file is compiled separately. Automake then collects the object files into one
of two kinds of archive:

- `.la` files are non-installed Libtool convenience libraries;
- `.a` files are ordinary static libraries.

The final executable is the first point at which references between these
archives are resolved. An archive is therefore a packaging boundary, not a
runtime component. There is no dynamic loader or plugin boundary between DIX,
MI, FB, KDrive, or the extensions.

Several forms of coupling cross the archive boundaries:

- ordinary C calls to symbols in another archive;
- global functions that the final frontend is required to define;
- callback tables such as `ScreenRec`, `GCOps`, and `KdCardFuncs`;
- global process state;
- private slots attached to shared server objects;
- wrappers that replace a callback and retain the previous implementation.

For a traditional static linker, the listed order is significant. Broadly,
code that introduces unresolved references appears before the archive that
satisfies them. Link-time optimization may optimize across selected objects,
but does not change the source or archive boundaries.

## 2. Frontends: `fbinit.o` and `vesainit.o`

**Sources**

- `kdrive/fbdev/fbinit.c`
- `kdrive/vesa/vesainit.c`

**Final targets**

- `Xfbdev`
- `Xvesa`

The frontend is a small adapter rather than the owner of the server's `main()`.
`main()` lives in `dix/main.c`; the frontend satisfies the DDX symbols that
DIX expects to find at final link time.

Both frontends define:

- `InitCard()` to register their `KdCardFuncs` table;
- `InitOutput()` to enter `KdInitOutput()`;
- `InitInput()` to select the Linux KDrive mouse and keyboard drivers;
- `ddxUseMsg()` for device-specific help;
- `ddxProcessArgument()` for device-specific command-line arguments.

The two frontends differ mainly in the display backend they register and the
arguments they recognize. `fbinit.c` registers `fbdevFuncs` and handles
`-fb`. `vesainit.c` registers `vesaFuncs` and delegates VESA-specific argument
parsing to `vesaProcessArgument()`.

The frontend is thus the **composition root** for a native executable: it
chooses a display backend and input implementation while reusing the common
server entry point and all common archives.

## 3. Display backends: `libfbdev.a` and `libvesa.a`

Only one display-backend archive is linked into each executable, immediately
before the shared archive list.

### 3.1 Linux framebuffer: `libfbdev.a`

**Directory:** `kdrive/fbdev/`

**Principal source:** `fbdev.c`

The framebuffer backend adapts a Linux framebuffer device to `KdCardInfo` and
`KdScreenInfo`. It:

- opens `/dev/fb0` or the path supplied with `-fb`;
- queries fixed and variable framebuffer information with `ioctl()`;
- maps device memory with `mmap()`;
- discovers or sets dimensions, depth, stride, visuals, and color masks;
- exposes palette operations where appropriate;
- implements enable, disable, preserve, restore, DPMS, and teardown callbacks;
- optionally participates in RandR and shadow-buffer operation.

Its primary interface to the rest of the server is `KdCardFuncs`. Once the
backend has populated `KdScreenInfo.fb`, the generic KDrive and FB layers do
most of the screen construction and rendering.

This archive brings in Linux framebuffer APIs directly: `<linux/fb.h>`,
`ioctl()`, device files, and memory mapping. It does not itself implement X11
protocol semantics.

### 3.2 VESA: `libvesa.a`

**Directory:** `kdrive/vesa/`

**Sources:** `vesa.c`, `vbe.c`, `vga.c`, and `vm86.c`

The VESA backend performs the corresponding job for VESA/VBE hardware. It
contains:

- VBE mode discovery and selection;
- VGA and BIOS-facing support;
- VM86 interaction;
- framebuffer mapping and screen description;
- palette, DPMS, preserve, restore, and mode operations.

Like fbdev, it presents itself through `KdCardFuncs`. Unlike fbdev, its native
boundary includes low-level x86 video and BIOS facilities rather than a Linux
framebuffer device API.

The build only enables this backend when the expected VM86 and I/O headers are
available. The frontend also rejects the unsupported 64-bit execution case.

## 4. Core server: `libdix.la`

**Directory:** `dix/`

**Archive:** `libdix.la`

DIX is the device-independent X server. It is both the largest semantic layer
and, in this tree, the owner of the process entry point.

### What it contains

The archive includes:

- `main.c`: process entry point, generation lifecycle, screen bootstrap, and
  connection setup data;
- `dispatch.c` and `tables.c`: client lifecycle, core request handlers, and
  opcode vectors;
- `resource.c`: XID-to-object tables, ownership, and destruction;
- `window.c`: window tree, mapping, geometry, clipping, and root windows;
- `events.c` and `grabs.c`: event selection, propagation, focus, and grabs;
- `gc.c`, `pixmap.c`, `colormap.c`, and `cursor.c`: core graphics objects;
- `property.c`, `atom.c`, and selection support;
- `dixfonts.c` and `glyphcurs.c`: font protocol integration and glyph cursors;
- request and reply byte-swapping code;
- private-slot, callback, device, and extension infrastructure.

### What it exports downward

Lower layers call DIX services for resources, regions, events, devices,
private storage, timers, callbacks, and error reporting. DIX data structures
such as `ScreenRec`, `WindowRec`, `DrawableRec`, `GC`, and `ClientRec` are the
common object model used throughout the final program.

### What it expects from below

DIX delegates display behavior through `ScreenRec` and GC callbacks, but also
expects several global DDX entry points to exist. The frontend and KDrive
provide `InitOutput()`, `InitInput()`, `ddxProcessArgument()`,
`ddxUseMsg()`, `ddxGiveUp()`, and related hooks.

It also calls the OS layer for connections, request input, output buffering,
authorization, timers, waiting, logging, and process setup.

### External-library pressure

DIX is the direct consumer of the font abstraction. `dix/dixfonts.c` calls
`BuiltinRegisterFpeFunctions()` and `FontFileRegisterFpeFunctions()`, opens the
default font, manages font paths, and translates font errors into X errors.
This is the principal reason the final server links `libXfont`.

## 5. KDrive framework: `libkdrive.a`

**Directory:** `kdrive/src/`

**Archive:** `libkdrive.a`

KDrive is the reusable compact DDX between DIX and a particular display or
host implementation.

### Main responsibilities

- maintain `KdCardInfo` and `KdScreenInfo` lists;
- parse KDrive screen, rotation, depth, and mouse options;
- convert backend framebuffer descriptions into `ScreenRec` instances;
- call FB and MI screen initialization;
- install KDrive screen wrappers;
- coordinate enable, disable, suspend, resume, DPMS, and VT switching;
- initialize core keyboard and pointer devices;
- translate backend input into X events;
- manage software cursor, shadow framebuffer, colormap, and mode support;
- map and unmap device memory through host facilities.

### Main interfaces

`KdCardFuncs` is implemented by fbdev or VESA and describes display hardware.
`KdOsFuncs` is implemented by the Linux host and describes console lifecycle.
`KdMouseFuncs` and `KdKeyboardFuncs` connect input backends to the common input
machinery.

KDrive fills `ScreenRec` partly by calling FB and MI, then wraps selected
callbacks to add hardware enablement, colormap, cursor, and lifecycle behavior.
It is therefore an assembly layer as much as a graphics layer.

### A notable source-list detail

`kdrive/src/Makefile.am` compiles `mi/miinitext.c` into `libkdrive.a`. Despite
its path, that translation unit is the built-in extension initialization list
for this server configuration. It determines which linked extension
initializers are invoked and in what order.

## 6. Native KDrive host: `liblinux.a`

**Directory:** `kdrive/linux/`

**Archive:** `liblinux.a`

This archive adapts KDrive to the Linux host environment. It is separate from
the display backend: both `Xfbdev` and `Xvesa` use it.

### `linux.c`

The Linux host implementation:

- verifies required privileges;
- opens and owns a virtual terminal;
- switches the terminal between text and graphics modes;
- handles VT release and reacquisition signals;
- integrates APM events;
- enables, disables, and tears down console state;
- publishes `LinuxFuncs`, a `KdOsFuncs` table;
- defines `OsVendorInit()`, which installs that table with `KdOsInit()`.

### `keyboard.c` and `mouse.c`

These files open and decode native input devices, register their file
descriptors with the server event loop, and feed events into
`KdEnqueueKeyboardEvent()` and `KdEnqueueMouseEvent()`.

The output backend and host backend are orthogonal in principle, even though
the current frontend chooses the Linux input functions directly.

## 7. Framebuffer renderer: `libfb.la`

**Directory:** `fb/`

**Archive:** `libfb.la`

FB is the software rasterizer. It knows how to draw into memory laid out as a
framebuffer or pixmap, but it does not know how that memory was allocated or
presented by the host.

### Screen and object integration

`fbSetupScreen()` installs FB implementations into `ScreenRec`, including:

- image and span access;
- window creation, painting, mapping, and copying;
- pixmap creation and destruction;
- GC creation;
- font realization;
- colormap defaults;
- bitmap-to-region conversion.

`fbFinishScreenInit()` selects visuals and depths, calls `miScreenInit()`, and
sets up the screen pixmap over the supplied memory.

### Rasterization

The remaining sources implement:

- bit block transfers and raster operations;
- solid, tiled, and stippled fills;
- spans, points, lines, segments, arcs, and rectangles;
- image upload and download;
- glyph and text drawing;
- 24/32-bit conversion;
- Render picture, edge, trapezoid, and composition support;
- overlay and screen-pixmap handling.

FB depends heavily on DIX object definitions and calls MI for generic visual,
colormap, region, and fallback behavior. It is not a leaf library despite
being conceptually below both.

### Why `fbcmap.c` is absent

`fb/Makefile.am` lists `fbcmap.c` as `EXTRA_DIST`, not as part of `libfb.la`.
The file is compiled separately into `libkdrivestubs.a` at the end of the link
list. That is why several FB colormap entry points arrive from the fallback
archive rather than this archive.

## 8. Machine-independent algorithms: `libmi.la`

**Directory:** `mi/`

**Archive:** `libmi.la`

MI provides generic algorithms that can be reused by multiple DDX and
rendering implementations.

### Screen and window machinery

MI initializes much of `ScreenRec` and implements:

- regions and clipping;
- window validation and visibility;
- movement, resize, stacking, and exposure handling;
- backing-store-related screen behavior;
- generic colormap and visual setup.

### Drawing algorithms

MI contains machine-independent implementations for:

- arcs and filled arcs;
- wide and dashed lines;
- polygons and polygon filling;
- points, rectangles, segments, and spans;
- text and glyph positioning;
- bit-block-transfer helpers and zero-width primitives.

These implementations may emit spans or invoke lower-level GC operations,
allowing FB or another renderer to provide the pixel-writing primitives.

### Pointer, cursor, and input queue

MI also supplies pointer tracking, software cursor support, sprite handling,
and `mieq`, the machine-independent event queue used by KDrive input.

### External-library pressure

Arc and wide-line geometry uses functions such as `sqrt`, `sin`, `cos`,
`asin`, `atan2`, `floor`, and `ceil`. Together with math used by Render and
KDrive input acceleration, this is why the final link explicitly includes
`-lm`.

## 9. Extensions

The extension archives follow MI in the final link. Their dispatch functions
are registered from `mi/miinitext.c`, which is included in `libkdrive.a`.

### 9.1 XFixes: `libxfixes.la`

**Directory:** `xfixes/`

XFixes provides protocol additions for cursor notification and images,
server-side regions, save sets, and event selection. Its region support builds
on DIX and MI region objects. Cursor support wraps screen cursor operations,
which is why extension initialization order matters.

### 9.2 Traditional X extensions: `libXext.la`

**Directory:** `Xext/`

This archive combines several extensions:

- **SHAPE** adds bounding, clip, and input shapes to windows;
- **MIT-SHM** exposes shared-memory images and pixmaps;
- **XTEST** synthesizes input and supports client testing;
- **BIG-REQUESTS** permits requests larger than the core protocol limit;
- **SYNC** provides counters, alarms, and synchronization requests;
- **XC-MISC** supplies resource-ID management helpers;
- **XRes**, when enabled, reports client resource usage;
- **MIT-SCREEN-SAVER**, when enabled, exposes screen-saver state;
- **XF86-BIGFONT**, when enabled, optimizes large font-property transfer;
- **DPMS**, when enabled, exposes display power management.

The exact source list is selected by Automake conditionals generated from
`configure.ac`.

### 9.3 Double buffering: `libdbe.la`

**Directory:** `dbe/`

DBE implements the DOUBLE-BUFFER extension. It creates extension resources and
coordinates front and back drawables through DIX and screen callbacks. The
archive and its place in `KDRIVE_LIBS` are present only when DBE is enabled.

### 9.4 Render: `librender.la`

**Directory:** `render/`

Render introduces pictures, formats, glyph sets, transforms, filters,
compositing, triangles, trapezoids, and gradients beyond core X drawing. The
archive includes both protocol dispatch and machine-independent picture
management. Pixel composition is delegated to screen picture callbacks, with
FB supplying the software implementation used here.

### 9.5 RandR: `librandr.la`

**Directory:** `randr/`

RandR models screens, modes, CRTCs, outputs, rotation, properties, and pointer
position across reconfiguration. KDrive connects its own mode and rotation
state to RandR's screen callbacks.

### 9.6 DAMAGE protocol: `libdamageext.la`

**Directory:** `damageext/`

This is the client-visible DAMAGE extension. It handles requests, creates
per-client damage resources, and reports damage events. The actual drawable
tracking mechanism is in `miext/damage/libdamage.la` later in the link.

### 9.7 Internal damage tracking: `libdamage.la`

**Directory:** `miext/damage/`

The MI damage layer wraps drawable and screen operations to accumulate changed
regions and notify registered consumers. It is infrastructure used by the
DAMAGE extension and other server components, not itself a wire-protocol
extension.

### 9.8 Shadow framebuffer: `libshadow.la`

**Directory:** `miext/shadow/`

Shadow is also extension infrastructure rather than an X11 protocol
extension. It tracks changed areas in a software framebuffer and copies them
to another layout. Its specialized update functions handle packed, planar,
and rotated framebuffers. KDrive uses it for rotation and hardware layouts
that are unsuitable for direct rendering.

## 10. Native OS and transport: `libos.la`

**Directory:** `os/`

**Archive:** `libos.la`

The OS archive is the concrete Unix server runtime. It combines several roles
that would be separate interfaces in a more explicitly hosted library.

### Transport and connection management

- `xstrans.c` compiles the selected Xtrans implementation directly into the
  archive;
- `connection.c` creates listeners, accepts clients, maps descriptors to DIX
  clients, and closes connections;
- `io.c` buffers requests, replies, events, and errors around Xtrans reads and
  writes;
- `WaitFor.c` combines timers, work queues, block/wakeup handlers, and
  `select()` readiness.

### Process and host policy

- `osinit.c` manages streams, limits, lock files, timers, and vendor startup;
- `utils.c` handles common command-line options, signals, scheduling, time,
  allocation, and fatal errors;
- `access.c` implements host access control;
- `auth.c`, `mitauth.c`, and optional `xdmauth.c` implement authorization;
- `log.c` implements server logging;
- `oscolor.c` supplies color-name lookup support.

### Embedded support code

`xauembed.c` contains the small subset of Xau needed to read and dispose of
Xauthority records. TinyX includes the Xauth structure declarations but does
not link a separate `libXau` for these operations.

Similarly, Xtrans is primarily a source/header framework here: `xstrans.c`
includes Xtrans implementation `.c` files according to `LOCALCONN`, `TCPCONN`,
and `UNIXCONN`. The `xtrans` pkg-config module supplies build metadata and
headers; there is no separate `libXtrans` in the final link.

## 11. KDrive fallback symbols: `libkdrivestubs.a`

**Directory:** `kdrive/src/`

**Archive:** `libkdrivestubs.a`

Despite the plural name, this archive contains one source file in the current
build: `fb/fbcmap.c`.

That source supplies generic FB colormap and visual helpers by forwarding to
MI, including:

- installed-colormap operations;
- color resolution and initialization;
- direct-color expansion;
- default colormap creation;
- visual-type setup.

Placing this archive at the end lets earlier archives introduce references to
these generic implementations before the linker searches it. The name reflects
its role as a final generic provider; the functions are real implementations,
not empty no-op stubs.

## 12. External dependencies

The external dependency list in `configure.ac` mixes header-only protocol
packages, source-style infrastructure, and actual linked libraries. They are
best considered separately.

### 12.1 Protocol and server headers

`REQUIRED_MODULES` asks pkg-config for:

- `xproto`;
- `randrproto`;
- `renderproto`;
- `fixesproto`;
- `damageproto`;
- `xcmiscproto`;
- `xextproto`;
- `xf86bigfontproto`;
- `scrnsaverproto`;
- `bigreqsproto`;
- `resourceproto`;
- `fontsproto`;
- `inputproto`;
- `kbproto`.

These packages principally provide wire-protocol constants and structure
definitions under `X11/` and `X11/extensions/`. The server compiles those
definitions into its own request and reply handling; it does not link a client
library for each extension.

### 12.2 Xtrans

The `xtrans` package supplies transport headers and implementation sources used
by `os/xstrans.c`. It is brought in by the native OS/transport layer. There is
normally no separate Xtrans library on the final link line because its source
is included into `libos.la`.

### 12.3 `libXfont`

`libXfont` supplies font-path-element implementations, font-file loading,
built-in font registration, bitmap font handling, and the font structures
used by DIX. It is brought in directly by `dix/dixfonts.c` and is required at
startup to locate the default text and cursor fonts.

This is an actual linked library through `XSERVER_LIBS`.

### 12.4 `libfontenc`

`libfontenc` provides legacy X font encoding support used by the font-file
stack. TinyX source does not call it directly; it is a dependency of the font
loading path and is also named explicitly in `REQUIRED_LIBS`.

This is an actual linked library through `XSERVER_LIBS`.

### 12.5 `libXdmcp`

When XDMCP is enabled and detected, `os/xdmcp.c` is added to `libos.la` and
`xdmcp` is added to the linked packages. The library supplies XDMCP packet,
array, key, wrap, and unwrap operations. `os/xdmauth.c` also uses these
functions when XDM-AUTH-1 is enabled.

XDMCP and XDM-AUTH-1 are configurable and may be absent from a build.

### 12.6 The math library

The final link always adds `-lm`. MI arc and line code, Render geometry, and
KDrive pointer acceleration use standard mathematical functions.

### 12.7 Platform system libraries

`SYS_LIBS` may add architecture-specific native libraries selected in
`configure.ac`, such as BSD I/O privilege libraries. Older platforms may also
require `-lrt` for `clock_gettime()`. The C library and normal compiler runtime
are linked implicitly.

An `AC_CHECK_LIB([ife], [meaning])` probe also exists near the end of
`configure.ac`; on a system that actually provides that symbol, Autoconf may
add `-life` through its normal `LIBS` handling. It is not part of the core
TinyX architecture.

### 12.8 Header use does not imply a linked library

Two cases are easy to misread:

- `<X11/Xauth.h>` is used, but the required Xau routines are embedded in
  `os/xauembed.c`;
- Xtrans headers and source files are used, but the implementation is compiled
  into `libos.la`.

Conversely, `libfontenc` may appear on the link line even though TinyX does not
call its API directly, because the font stack needs it.

## 13. What the link order does not tell us

The link order is useful for locating code and understanding final assembly,
but it does not establish a strict architectural hierarchy.

For example:

- DIX invokes screen callbacks implemented by MI, FB, and KDrive;
- those implementations call DIX resource, event, and object helpers;
- KDrive invokes extension initialization, while extensions wrap screen
  callbacks installed by lower layers;
- FB calls MI colormap and visual helpers;
- the OS layer creates clients through DIX and DIX performs I/O through OS;
- the frontend satisfies symbols referenced from `libdix.la`, even though its
  object appears before that archive.

The executable is best understood as one statically assembled program with
several useful internal interfaces, not as a stack of independently usable
libraries. Those existing interfaces are nevertheless the natural places to
look when separating a platform-independent core from native host code.
