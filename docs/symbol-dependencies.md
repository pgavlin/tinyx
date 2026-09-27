# Auditing TinyX Symbol Dependencies

TinyX's archive link order is not its source dependency graph. The
`tools/symbol-deps.py` utility derives that graph from compiled object symbol
tables. It also inventories which definitions are visible outside each object
file and which definitions remain local because they were declared `static`.

This analysis is useful when moving `main()` out of DIX, changing archive
boundaries, or deciding which calls must cross a host interface.

## 1. Three meanings of export

“Exported” can mean three different things in this codebase.

### Link-visible from another object file

A normal file-scope C definition has external linkage and usually appears as a
global symbol in its `.o` file. Another object in the final static link can
refer to it.

A definition declared `static` has internal linkage. It appears as a local
symbol, if the compiler retains a symbol for it at all, and cannot satisfy a
reference from another object file.

This is the distinction analyzed by `symbol-deps.py`.

### Visible from a shared library or executable

TinyX enables `-fvisibility=hidden` when supported. `_X_EXPORT`, defined by
X11's `Xfuncproto.h`, requests default visibility for selected symbols. This
controls dynamic symbol visibility; it does not create a clean static-library
API.

The symbol audit records `nm` linkage and type information, not ELF or Mach-O
dynamic visibility. Use `readelf -Ws`, `objdump`, or the platform equivalent
when auditing a future shared-library ABI.

### Supported public API

A link-visible symbol is not necessarily intended for embedders. The eventual
public API must be declared deliberately in a public header and kept much
smaller than TinyX's set of global internal symbols.

## 2. Build without LTO

Link-time optimization may store compiler IR in object files and can
internalize, rename, or discard symbols during the final link. Generate symbol
reports from a non-LTO build.

The configure script supports:

```sh
./configure --disable-lto [other options]
make
```

For the clearest inventory, an unoptimized build is preferable:

```sh
./configure --disable-lto CFLAGS='-O0 -g'
make
```

An out-of-tree build works as well:

```sh
mkdir -p build/symbol-audit
cd build/symbol-audit
../../configure --disable-lto CFLAGS='-O0 -g'
make
```

The normal external package and `ACLOCAL` setup described in the build
documentation is still required.

## 3. Generate the TinyX reports

For an in-tree build:

```sh
tools/tinyx-symbol-deps.sh
```

For an out-of-tree build:

```sh
tools/tinyx-symbol-deps.sh build/symbol-audit
```

An optional second argument selects the output directory:

```sh
tools/tinyx-symbol-deps.sh build/symbol-audit build/dependency-report
```

The wrapper includes all common TinyX archives and any built Linux, fbdev,
VESA, or DBE archives. It writes:

- `archive-dependencies.md`: readable archive graph, dependency edges,
  unresolved externals, and symbol inventory;
- `archive-dependencies.json`: complete machine-readable archive report;
- `object-dependencies.json`: complete dependency and symbol data at archive
  member/object-file granularity.

The Markdown report uses the Mermaid subset accepted by MDS.

## 4. Use the analyzer directly

The analyzer accepts `.o`, `.a`, and Libtool `.la` inputs. For `.la` files it
reads `old_library` and locates the generated static archive.

```sh
python3 tools/symbol-deps.py \
    --scope archive \
    --include-symbols \
    --output report.md \
    dix/libdix.la \
    kdrive/src/libkdrive.a \
    fb/libfb.la \
    mi/libmi.la \
    os/libos.la
```

Available output formats are:

```text
--format markdown
--format json
--format dot
--format mermaid
```

Dependency scope controls aggregation:

```text
--scope archive    one node per command-line input
--scope object     one node per .o or archive member
```

`--include-symbols` adds artifact exports and per-object inventories to the
Markdown report. JSON always contains the complete inventory.

If a cross compiler supplies its own `nm`, set `NM` or use `--nm`:

```sh
NM=llvm-nm python3 tools/symbol-deps.py ...
python3 tools/symbol-deps.py --nm 'wasm-nm' ...
```

## 5. Reading the graph

An edge:

```text
consumer → provider
```

means that the consumer contains an undefined symbol for which the provider
contains a link-visible definition. The edge lists or counts those symbols.

For example:

```text
libkdrive.a → libdix.la
```

may include `AddScreen`, `FatalError`, or private-index allocation functions.
This is a source-level static dependency even though `libdix.la` occurs earlier
on the native executable's link line.

The report includes every potential provider supplied on the command line. If
more than one input defines the same referenced symbol, the symbol is reported
as ambiguous. The actual linker chooses according to platform rules, archive
order, weak binding, and which members are extracted.

Undefined symbols with no provider in the supplied inputs are reported as
external. These normally include libc, libm, libXfont, libfontenc, Xdmcp, or
platform system calls.

## 6. Reading the symbol inventory

For each object, the Markdown report separates:

- link-visible functions;
- other link-visible definitions, usually data;
- local functions, usually file-scope `static` functions;
- other local definitions;
- undefined references.

The function classification is based on `nm` text-symbol types (`T` or `t`).
Unusual assembly, aliases, weak symbols, and platform-specific symbol types may
need manual interpretation. The JSON output retains the original `nm` type
letter, binding classification, and derived kind for every symbol.

On Darwin, C symbol names generally have a leading underscore. The analyzer
leaves names exactly as reported by `nm`, so `_KdInitOutput` and
`KdInitOutput` should be understood as platform spellings of the same C name.

## 7. Limits of symbol analysis

The report captures direct linker dependencies, but not every architectural
dependency.

It does not infer the runtime provider behind calls through:

- `ScreenRec` callbacks;
- `GCFuncs` or `GCOps`;
- `KdCardFuncs`;
- `KdOsFuncs`;
- extension registration tables;
- block/wakeup handlers;
- private slots containing function pointers.

It also does not report structure-layout coupling caused by including shared
internal headers. A component can depend heavily on `ScreenRec` or `ClientRec`
without introducing a linker edge for that dependency.

The proper architectural picture therefore combines:

1. the mechanically generated symbol graph;
2. the callback and registration graph in
   [Finding the Embedding Boundaries](embedding-boundaries.md);
3. an explicit inventory of shared data structures crossing the proposed
   host boundary.

## 8. Applying it to the library refactor

Regenerate the report after each boundary change and check that:

- native-only archives do not become providers for the portable core;
- the portable core references host behavior only through the intended host
  interface;
- moving `main()` does not leave previously accidental archive extraction
  dependencies unresolved;
- the public API is a deliberate subset of link-visible definitions;
- input, display, and protocol adapters depend on the core, rather than on one
  another;
- duplicate and weak providers are understood rather than linker accidents.

A final WASM build should be audited separately with its own symbol tools and
explicit export list. Static link visibility, `_X_EXPORT`, and Emscripten's
exported-function list answer different questions and should not be conflated.
