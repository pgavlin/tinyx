You’re right—I answered a design question you didn’t ask. A guided tour of what is already here is the better starting point.

And yes: refactoring in place toward a platform-independent core with host implementations is a better architectural direction than adding a WASM-specific API directly throughout the server.

## 1. The Autotools pipeline

There are three conceptual stages:

```text
configure.ac ──autoconf──> configure
Makefile.am  ──automake──> Makefile.in
                              │
                    ./configure
                              │
                              ▼
                          Makefile
```

In this repository:

```sh
./autogen.sh
```

runs:

```sh
autoreconf -v --install
```

`autoreconf` orchestrates several tools:

- `aclocal`: collects M4 macros used by `configure.ac`
- `autoconf`: turns `configure.ac` into the `configure` shell script
- `autoheader`: updates configuration-header templates
- `automake`: turns each `Makefile.am` into a `Makefile.in`
- `libtoolize`: installs Libtool support files

Then:

```sh
./configure
```

runs compiler and dependency tests and produces:

- `Makefile`
- one `Makefile` in each subdirectory
- `include/dix-config.h`
- `include/kdrive-config.h`
- `include/do-not-use-config.h`

Finally:

```sh
make
```

recurses through the subdirectories and builds the selected targets.

## 2. `configure.ac`: the global build description

The beginning establishes the package:

```m4
AC_INIT([tinyx], 1.3)
AM_INIT_AUTOMAKE([dist-bzip2 no-dist-gzip foreign -Wall])
AC_CONFIG_MACRO_DIR([m4])
```

This says:

- package name: `tinyx`
- package version: `1.3`
- use Automake
- project-local M4 macros live in `m4/`

These declare generated configuration headers:

```m4
AC_CONFIG_HEADERS(include/do-not-use-config.h)
AC_CONFIG_HEADERS(include/dix-config.h)
AC_CONFIG_HEADERS(include/kdrive-config.h)
```

Later, calls such as:

```m4
AC_DEFINE(SMART_SCHEDULE, 1, [Include time-based scheduler])
```

cause this to appear in a generated header:

```c
#define SMART_SCHEDULE 1
```

The corresponding `.in` files contain placeholders:

```c
#undef SMART_SCHEDULE
```

## 3. Compiler and system detection

These initialize the toolchain:

```m4
AC_PROG_CC
AM_PROG_CC_C_O
AM_PROG_AR
AM_PROG_AS
AC_PROG_LIBTOOL
PKG_PROG_PKG_CONFIG
```

They find:

- C compiler
- assembler
- archiver
- Libtool
- `pkg-config`

Then `configure.ac` checks headers, functions, type sizes, and libraries:

```m4
AC_CHECK_HEADERS([fcntl.h stdlib.h string.h unistd.h])
AC_CHECK_SIZEOF([unsigned long])
AC_CHECK_FUNCS([geteuid getuid link memmove ...])
AC_CHECK_LIB(m, sqrt)
```

These tests serve two purposes:

1. Abort or adjust the build based on what is available.
2. Define feature macros in `dix-config.h`.

For example:

```m4
AC_CHECK_FUNC([getdtablesize],
    AC_DEFINE(HAS_GETDTABLESIZE, 1, [...]))
```

becomes:

```c
#define HAS_GETDTABLESIZE 1
```

when the function exists.

This is the primary role Autoconf plays: it turns host capabilities into C preprocessor definitions.

## 4. Build options

Options such as:

```m4
AC_ARG_ENABLE(xres, ...)
AC_ARG_ENABLE(dbe, ...)
AC_ARG_ENABLE(kdrive, ...)
AC_ARG_ENABLE(xvesa, ...)
AC_ARG_ENABLE(xfbdev, ...)
```

create flags accepted by `configure`:

```sh
./configure --disable-xres --disable-dbe
```

The shell variable is then converted into an Automake conditional:

```m4
AM_CONDITIONAL(DBE, [test "x$DBE" = xyes])
```

That permits this syntax in `Makefile.am`:

```make
if DBE
DBE_DIR = dbe
endif
```

Automake conditionals are evaluated by `configure`, not by `make`.

There are therefore two related but distinct mechanisms:

### C preprocessor conditionals

```m4
AC_DEFINE(DBE, 1, [Build DBE support])
```

produces:

```c
#ifdef DBE
```

### Build-system conditionals

```m4
AM_CONDITIONAL(DBE, ...)
```

controls:

```make
if DBE
...
endif
```

A feature often needs both.

## 5. Substituted variables

`configure.ac` constructs variables such as:

```m4
DIX_LIB='$(top_builddir)/dix/libdix.la'
FB_LIB='$(top_builddir)/fb/libfb.la'
MI_LIB='$(top_builddir)/mi/libmi.la'
```

It makes them available to generated Makefiles with:

```m4
AC_SUBST(KDRIVE_LIBS)
```

A `Makefile.am` can consume these using either:

```make
@KDRIVE_LIBS@
```

or, after substitution:

```make
$(KDRIVE_LIBS)
```

In this tree you’ll see both styles.

For example:

```make
Xfbdev_LDADD = \
    libfbdev.a \
    @KDRIVE_LIBS@ \
    @XSERVER_LIBS@
```

When `configure` generates `kdrive/fbdev/Makefile`, it replaces `@KDRIVE_LIBS@` with the value assembled in `configure.ac`.

## 6. External dependencies

This line describes the protocol/header dependencies:

```m4
REQUIRED_MODULES="randrproto renderproto fixesproto ... xproto xtrans ..."
```

This line describes the actual libraries:

```m4
REQUIRED_LIBS="xfont fontenc"
```

Then:

```m4
PKG_CHECK_MODULES([XSERVERCFLAGS],
                  [$REQUIRED_MODULES $REQUIRED_LIBS])

PKG_CHECK_MODULES([XSERVERLIBS],
                  [$REQUIRED_LIBS])
```

creates variables such as:

```text
XSERVERCFLAGS_CFLAGS
XSERVERCFLAGS_LIBS
XSERVERLIBS_CFLAGS
XSERVERLIBS_LIBS
```

The project turns those into:

```m4
XSERVER_CFLAGS="${XSERVERCFLAGS_CFLAGS}"
XSERVER_LIBS="${XSERVERLIBS_LIBS} ${SYS_LIBS} -lm"
```

A significant detail for WASM is that most `*proto` dependencies are headers, while `xfont` and `fontenc` are compiled libraries. That distinction is currently hidden behind `pkg-config`.

## 7. The top-level `Makefile.am`

The root file is mostly a recursive build order:

```make
SUBDIRS = \
    include \
    dix \
    fb \
    mi \
    Xext \
    miext \
    os \
    randr \
    render \
    $(DBE_DIR) \
    xfixes \
    damageext \
    kdrive
```

This says to enter each directory and run its generated Makefile.

`SUBDIRS` contains the directories built in the current configuration.

`DIST_SUBDIRS` contains every directory that should be included in source distributions, even if disabled:

```make
DIST_SUBDIRS = \
    ... \
    dbe \
    ...
```

That is why `dbe` is unconditional in `DIST_SUBDIRS` but conditional in `SUBDIRS`.

## 8. The internal libraries

Most directories produce **Libtool convenience libraries**:

```make
noinst_LTLIBRARIES = libdix.la
```

Breaking that apart:

- `noinst`: do not install this library
- `LT`: managed by Libtool
- `LIBRARIES`: library target
- `libdix.la`: target name

The source-list variable is derived mechanically from the target name:

```make
libdix_la_SOURCES = \
    atom.c \
    colormap.c \
    ...
```

Dots and punctuation in target names become underscores.

A `.la` file is Libtool metadata. Because the project calls:

```m4
AC_DISABLE_SHARED
```

these are effectively internal static/convenience libraries rather than shared objects intended for installation.

The major libraries are:

| Directory | Library | Role |
|---|---|---|
| `dix` | `libdix.la` | Device-independent X server and `main()` |
| `mi` | `libmi.la` | Machine-independent graphics/window algorithms |
| `fb` | `libfb.la` | Software framebuffer rendering |
| `os` | `libos.la` | POSIX transport, event waiting, auth, logging |
| `Xext` | `libXext.la` | Traditional X extensions |
| `render` | `librender.la` | Render extension |
| `randr` | `librandr.la` | RandR extension |
| `xfixes` | `libxfixes.la` | XFixes extension |
| `damageext` | `libdamageext.la` | Damage protocol extension |
| `miext/damage` | `libdamage.la` | Internal damage tracking |
| `miext/shadow` | `libshadow.la` | Shadow framebuffer implementation |
| `dbe` | `libdbe.la` | Double-buffer extension |

These are not separately installed libraries. They are pieces linked into the final server executables.

## 9. DIX, MI, FB, DDX, and OS

The terminology matters because the tree already has some platform separation.

### DIX: Device-Independent X

`dix/` implements the core X server:

- clients
- resources
- windows
- properties
- atoms
- requests
- events
- devices
- dispatch
- server lifecycle

Unfortunately, `dix/main.c` also contains the process-level `main()` and calls platform hooks directly. So it is conceptually device-independent, but not yet an embeddable library.

### MI: Machine Independent

`mi/` provides generic implementations of graphics and screen operations.

### FB

`fb/` is the in-memory/software framebuffer renderer.

This is not the Linux framebuffer driver. It performs pixel operations against framebuffer memory.

### DDX: Device-Dependent X

The DDX supplies the display and input implementation. Here that is KDrive plus either:

- `kdrive/fbdev`
- `kdrive/vesa`

### OS

`os/` contains:

- socket setup
- X transport integration
- `select()`-based waiting
- reading and writing clients
- authorization
- process signals
- logging
- server lock files
- timing

This directory is called “OS,” but it is not currently an abstract host interface. It is largely a concrete Unix/POSIX implementation.

## 10. How the final executables are assembled

For `Xfbdev`:

```make
bin_PROGRAMS = Xfbdev

Xfbdev_SOURCES = \
    fbinit.c

Xfbdev_LDADD = \
    libfbdev.a \
    @KDRIVE_LIBS@ \
    @XSERVER_LIBS@
```

`configure.ac` assembles `KDRIVE_LIBS` as:

```m4
KDRIVE_LIBS="$DIX_LIB \
             $KDRIVE_LIB \
             $KDRIVE_OS_LIB \
             $KDRIVE_PURE_LIBS \
             $KDRIVE_STUB_LIB"
```

And `KDRIVE_PURE_LIBS` is:

```m4
$FB_LIB
$MI_LIB
$FIXES_LIB
$XEXT_LIB
$DBE_LIB
$RENDER_LIB
$RANDR_LIB
$DAMAGE_LIB
$MIEXT_DAMAGE_LIB
$MIEXT_SHADOW_LIB
$OS_LIB
```

So the approximate link is:

```text
Xfbdev
├── kdrive/fbdev/fbinit.c
├── kdrive/fbdev/libfbdev.a
├── dix/libdix.la
├── kdrive/src/libkdrive.a
├── kdrive/linux/liblinux.a
├── fb/libfb.la
├── mi/libmi.la
├── extension libraries
├── os/libos.la
├── kdrive/src/libkdrivestubs.a
├── libXfont
├── libfontenc
└── libm
```

`Xvesa` is assembled similarly, replacing `libfbdev.a` with `libvesa.a`.

An unusual but important detail is that `main()` comes from `dix/main.c`, inside `libdix.la`. The tiny `fbinit.c` and `vesainit.c` files provide symbols that the common server expects.

## 11. Existing link-time “interfaces”

The code already uses a primitive form of dependency injection through required global symbols.

`dix/main.c` calls:

```c
InitOutput(&screenInfo, argc, argv);
InitInput(argc, argv);
```

Those are not implemented in DIX. Each final DDX provides them.

For example, `kdrive/fbdev/fbinit.c` provides:

```c
void InitOutput(ScreenInfo *pScreenInfo, int argc, char **argv)
{
    KdInitOutput(pScreenInfo, argc, argv);
}

void InitInput(int argc, char **argv)
{
    KdInitInput(&LinuxMouseFuncs, &LinuxKeyboardFuncs);
}
```

It also supplies:

```c
InitCard()
ddxUseMsg()
ddxProcessArgument()
```

The generic server references these names, and whichever DDX is linked into the executable satisfies them.

This is an interface, but it is implicit and link-time rather than represented by a structure or explicit API.

## 12. KDrive’s more explicit interfaces

KDrive itself is closer to the model you described.

### Display-device interface

`KdCardFuncs` contains callbacks such as:

```c
typedef struct _KdCardFuncs {
    Bool (*cardinit)(KdCardInfo *);
    Bool (*scrinit)(KdScreenInfo *);
    Bool (*initScreen)(ScreenPtr);
    Bool (*createRes)(ScreenPtr);
    Bool (*enable)(ScreenPtr);
    void (*disable)(ScreenPtr);
    void (*restore)(KdCardInfo *);
    ...
} KdCardFuncs;
```

The fbdev backend constructs one:

```c
static const KdCardFuncs fbdevFuncs = {
    fbdevCardInit,
    fbdevScreenInit,
    fbdevInitScreen,
    ...
};
```

Then registers it:

```c
void InitCard(char *name)
{
    KdCardInfoAdd(&fbdevFuncs, 0);
}
```

That is already a real platform/backend interface.

### Host OS interface

KDrive also has:

```c
typedef struct _KdOsFuncs {
    int  (*Init)(void);
    void (*Enable)(void);
    Bool (*SpecialKey)(KeySym);
    void (*Disable)(void);
    void (*Fini)(void);
    void (*pollEvents)(void);
} KdOsFuncs;
```

The Linux backend creates:

```c
static const KdOsFuncs LinuxFuncs = {
    LinuxInit,
    LinuxEnable,
    LinuxSpecialKey,
    LinuxDisable,
    LinuxFini,
    0
};
```

It plugs this into KDrive through a hook named `OsVendorInit()`:

```c
void OsVendorInit(void)
{
    KdOsInit(&LinuxFuncs);
}
```

So part of the platform-independent/native split already exists. It is simply incomplete: client transport and the main event loop remain concrete POSIX code in `os/`.

## 13. Input interfaces

KDrive has separate interfaces for mouse and keyboard hosts:

```c
typedef struct _KdMouseFuncs {
    Bool (*Init)(void);
    void (*Fini)(void);
} KdMouseFuncs;

typedef struct _KdKeyboardFuncs {
    void (*Load)(void);
    int  (*Init)(void);
    void (*Leds)(int);
    void (*Bell)(int, int, int);
    void (*Fini)(void);
    int LockLed;
} KdKeyboardFuncs;
```

The fbdev and VESA frontends currently hard-code:

```c
KdInitInput(&LinuxMouseFuncs, &LinuxKeyboardFuncs);
```

A non-native host could supply different implementations without changing most of `kdrive/src`.

## 14. Conditional KDrive subdirectories

`kdrive/Makefile.am` chooses platform and device directories:

```make
if KDRIVEVESA
VESA_SUBDIRS = vesa
endif

if KDRIVEFBDEV
FBDEV_SUBDIRS = fbdev
endif

if KDRIVELINUX
LINUX_SUBDIRS = linux
endif

SUBDIRS = \
    src \
    $(LINUX_SUBDIRS) \
    $(FBDEV_SUBDIRS) \
    $(VESA_SUBDIRS)
```

Those conditionals come from `configure.ac`.

Linux is selected with:

```m4
case $host_os in
    *linux*)
        KDRIVE_OS_LIB='$(top_builddir)/kdrive/linux/liblinux.a'
        KDRIVELINUX=yes
        ;;
esac
```

fbdev and VESA are selected based on header tests:

```m4
AC_CHECK_HEADERS([linux/fb.h])
AC_CHECK_HEADERS([asm/vm86.h sys/io.h])
```

Then:

```m4
AM_CONDITIONAL(KDRIVEVESA, ...)
AM_CONDITIONAL(KDRIVEFBDEV, ...)
```

For a new host, the existing conceptual pattern would be another host library parallel to `kdrive/linux/liblinux.a`.

## 15. Extension source selection

`Xext/Makefile.am` illustrates conditional source composition:

```make
BUILTIN_SRCS = \
    shape.c \
    sleepuntil.c \
    xtest.c

MODULE_SRCS = \
    bigreq.c \
    shape.c \
    sync.c \
    xcmisc.c
```

Optional source groups are appended:

```make
if RES
MODULE_SRCS += $(RES_SRCS)
endif

if SCREENSAVER
MODULE_SRCS += $(SCREENSAVER_SRCS)
endif
```

Finally:

```make
libXext_la_SOURCES = $(BUILTIN_SRCS) $(MODULE_SRCS)
```

This is how configure options determine which `.c` files are compiled, rather than merely compiling everything and relying on `#ifdef`.

## 16. Compiler flags

Most libraries use:

```make
AM_CFLAGS = $(DIX_CFLAGS)
```

`configure.ac` defines:

```m4
DIX_CFLAGS="-DHAVE_DIX_CONFIG_H"
```

This activates patterns like:

```c
#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif
```

KDrive uses:

```m4
KDRIVE_CFLAGS="$XSERVER_CFLAGS -DHAVE_KDRIVE_CONFIG_H"
```

and its Makefiles apply those through:

```make
AM_CPPFLAGS = \
    @KDRIVE_INCS@ \
    @KDRIVE_CFLAGS@
```

The distinction is conventionally:

- `CPPFLAGS`: include paths and preprocessor definitions
- `CFLAGS`: optimization, warnings, language/compiler flags
- `LDFLAGS`: linker behavior
- `LDADD`: libraries and objects linked into a specific executable

This tree does not follow that distinction perfectly, but that is the intent.

## 17. What is actually platform-dependent today

The platform boundary is spread across several places:

### Clearly platform-specific

- `kdrive/linux/`
- `kdrive/fbdev/`
- `kdrive/vesa/`
- much of `os/`

### Platform-independent or close to it

- most of `dix/`
- `mi/`
- `fb/`
- `render/`
- `randr/`
- extension protocol implementations
- most of `kdrive/src/`

### Mixed files

- `dix/main.c`: core lifecycle mixed with process entry point
- `dix/dispatch.c`: protocol dispatch mixed with blocking scheduling assumptions
- `os/io.c`: protocol buffering mixed with Xtrans I/O
- `os/WaitFor.c`: timers/work queues mixed with `select()`
- `os/connection.c`: client management mixed with socket listeners
- `os/osinit.c`: generic initialization mixed with Unix process setup

Those mixed files are where an in-place host abstraction would naturally emerge.

## 18. How your intended architecture maps onto this tree

The current design already has three backend concepts, though they are inconsistent:

```text
Core X server
├── required global DDX functions
│   ├── InitOutput
│   ├── InitInput
│   ├── ddxProcessArgument
│   └── ddxUseMsg
├── KdCardFuncs
│   └── display/framebuffer device implementation
├── KdOsFuncs
│   └── console/host activation implementation
└── concrete os/*.c
    └── transport, scheduling, timing, process behavior
```

Your proposed endpoint would make the final branch explicit as well:

```text
Platform-independent core
├── display backend
├── input backend
├── client transport backend
├── clock/timer backend
├── event-loop/wakeup backend
├── logging backend
└── optional filesystem/font backend

Hosts
├── native Unix host
└── WASM host
```

The native executable would then become one consumer of the same core library rather than the definition of the server lifecycle.

That does not inherently require replacing Automake. The current build can represent that architecture with additional convenience libraries and conditionals. CMake might eventually be preferable, but build-system migration and host abstraction are separate decisions.

One cross-compilation issue worth noting: `configure.ac` branches on `$host_os` and `$host_cpu`, but does not directly call `AC_CANONICAL_HOST`. Before relying heavily on `--host=wasm32-...`, that part of the configuration should be checked or corrected so those variables are reliably canonicalized.
