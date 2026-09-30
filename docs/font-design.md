# Phase 7 Font Architecture

## Status

This document records the implemented Phase 7 architecture. The built-in
catalog, decoded-font materializer, and FPE are complete. The subsequent
[Host-Provided Bitmap Font Design](font-provider-design.md) defines the
proposed frozen-manifest, constrained-pull API and BDF/PCF parser architecture.

## Goals

Phase 7 must make the memory server runnable without a filesystem or
`libXfont`, while retaining the existing X11 font resource and rendering
machinery. In particular:

- generation startup must be able to open `fixed` and `cursor`;
- text must continue through the existing GC, FB, and DIX paths;
- glyph cursors must continue through `AllocGlyphCursor()`;
- unsupported font requests must produce normal X11 errors;
- native builds may retain their existing filesystem font behavior;
- no font parser, filesystem abstraction, or host callback is required merely
  to start the memory server.

The first embedded implementation is deliberately a small bitmap-font
backend. Scalable fonts, arbitrary host fonts, font servers, and full native
font-path parity are not Phase 7 requirements.

## Existing architecture

DIX already contains the abstraction needed by the X protocol: a font path
is a list of `FontPathElementRec` objects, and each element dispatches through
an `FPEFunctions` table registered by `RegisterFPEFunctions()`.
`dix/dixfonts.c` owns protocol behavior, resource lifetime, aliases, font-path
changes, listing, querying, and text request suspension. A backend is
responsible for opening a `FontRec`, returning glyphs and metrics, listing its
fonts, and destroying backend-owned font data.

The current native path registers two libXfont implementations:

1. libXfont's `built-ins` FPE, containing 6x13 and cursor PCF data;
2. libXfont's font-file FPE, which reads configured filesystem paths.

The temporary `--disable-fonts` build replaces libXfont utility symbols with
`dix/fontstubs.c`, but registers no FPE. `SetDefaultFont("fixed")` therefore
fails during every generation.

This means Phase 7 does not need to replace DIX font semantics. It needs a
small, host-independent FPE and the few libXfont utility functions that DIX
still calls directly.

## Options considered

### Keep libXfont and provide a virtual filesystem

This preserves broad format support, but retains a large legacy dependency,
compression and font-encoding dependencies, pathname policy, and code not
needed by the initial memory server. A WASM virtual filesystem would hide the
host dependency rather than establish a clean boundary.

**Decision:** retain this path for native compatibility, but do not use it as
the embedded implementation.

### Port the server to libXfont2

libXfont2 remains filesystem-oriented and has a different server callback
registration API. Porting to it could improve the native build independently,
but it does not provide a filesystem-free startup font strategy.

**Decision:** out of scope for Phase 7. It can replace legacy libXfont in a
separate native-host change.

### Embed PCF files and a runtime PCF reader

This closely resembles libXfont's built-in backend, but requires importing a
PCF reader, buffered I/O, format conversion, and optionally decompression.
Those components are substantially larger than the two fonts being exposed.

**Decision:** do not parse a file format at runtime for the initial backend.

### Embed decoded bitmap fonts behind an FPE

A development-time generator can convert authoritative BDF sources into
checked-in C tables. The runtime backend then constructs ordinary `FontRec`
objects from immutable metrics and bitmap data. It needs no filesystem and no
parser, and DIX continues to implement all X11-visible behavior.

**Decision:** use this approach.

## Selected design

### 1. Preserve the DIX/FPE boundary

Add an internal embedded FPE, provisionally named
`TinyXRegisterEmbeddedFontFPE()`. It is registered in embedded-font builds in
place of libXfont's built-in and font-file FPEs. It recognizes exactly one
font-path element:

```text
built-ins
```

The backend exposes these names:

| Name | Meaning |
|---|---|
| `fixed` | Alias for the embedded 6x13 text font |
| `6x13` | Alias for the embedded 6x13 text font |
| `-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso8859-1` | Canonical text-font name |
| `cursor` | Canonical cursor-font name |

The existing 100-dpi 6x13 alias used by libXfont's built-ins should also be
retained for compatibility.

Open, close, list, and list-with-info callbacks are synchronous. Wakeup,
client-death, and glyph-loading callbacks are harmless no-ops because all
glyphs are resident. The callback table must nevertheless contain valid
functions wherever DIX invokes a callback unconditionally.

The implementation belongs under `dix/` initially because it adapts embedded
font data directly to DIX's `FontRec` and FPE ABI. The generated data can live
in a small `fonts/` subdirectory without becoming a public interface.

### 2. Generate source data; do not generate during normal builds

Check in generated C data for:

- the complete BMP repertoire of X.Org's 6x13 ISO10646-1 fixed font;
- the complete X.Org cursor font, including source/mask glyph pairs.

Also check in the generator and source provenance so regeneration is
repeatable, but do not require Python, BDF tools, or font packages to compile
TinyX. Generated files must include a source release/version, source file
hash, generation command, and applicable copyright notice.

The generator should reject malformed or unsupported BDF input and emit:

- per-character `xCharInfo`-equivalent metrics;
- canonical, tightly packed, MSB-first bitmap rows;
- font bounds, ascent, descent, default character, and properties;
- explicit encoding-to-glyph lookup tables.

The initial Phase 7 implementation retained encodings 0 through 255. The
post-Phase-9 font expansion now retains all 4,121 source glyphs in the 16-bit
BMP encoding space so the canonical ISO10646-1 font requested by xterm works
without host fonts. Historical ISO8859-1 names remain aliases to the same
font.

Generated arrays are one implementation of an internal decoded-font view;
they must not become inputs that the materializer references directly by
symbol. The same view must be constructible over borrowed or dynamically
owned metrics, properties, lookup tables, and bitmap spans.

### 3. Separate acquisition from `FontRec` materialization

The embedded FPE obtains fonts from an internal synchronous catalog boundary,
then passes a decoded-font view to a separate materializer. Conceptually, the
boundary has these responsibilities:

```c
typedef struct TinyXFontCatalogOps {
    int (*acquire)(void *userdata, const char *name,
                   TinyXDecodedFontLease *lease);
    void (*release)(void *userdata, TinyXDecodedFontLease *lease);
    int (*list)(void *userdata, const char *pattern, ...);
} TinyXFontCatalogOps;
```

These names and signatures are illustrative and remain internal. The
important contract is that a successful acquisition returns an immutable view
plus a lease that remains valid until its matching release. The view contains
only host-neutral values and byte spans; it contains no `FontRec`, atom IDs,
FPE objects, or screen pointers. Release is called on every close and on every
partially failed open. A catalog may return borrowed process-lifetime data,
reference-counted data, or per-open allocated data without changing the FPE or
materializer.

Phase 7 supplies exactly one catalog implementation: the built-in catalog over
checked-in generated data. Its acquire operation returns static views and its
release operation is a no-op. Defining and exposing a host-configurable catalog
is explicitly deferred, but the FPE must depend on the internal catalog
contract rather than directly searching generated arrays.

Catalog acquisition is synchronous in Phase 7. This does not preclude later
asynchronous acquisition: DIX already supports suspended FPE operations, but
cancellation, wakeup, and reentrancy policy must be designed with the public
embedding API rather than guessed now.

### 4. Materialize an ordinary `FontRec` on open

The built-in catalog's generated definitions are immutable process-lifetime
data, but the materializer does not rely on that lifetime. Each successful
open allocates a `FontRec` plus an internal instance containing:

- `CharInfoRec` entries;
- glyph storage converted to the bitmap format requested by DIX;
- generation-specific font properties and atoms;
- the acquired decoded-font lease and its release context.

The backend implements `get_glyphs` and `get_metrics` for all four
`FontEncoding` values. Missing characters use the declared default character
when appropriate, following bitmap-font behavior. It validates every index,
row size, multiplication, and allocation before exposing the font.

The runtime converter handles the requested bit order, byte order, scanline
unit, and glyph padding instead of assuming the build host's representation.
This keeps generated data portable and allows FB and glyph-cursor code to
consume the resulting `FontRec` exactly as they consume a libXfont font.

Properties cannot be emitted as final `FontPropRec` values because atom IDs
are generation-local. They are represented as strings/integers in generated
data and materialized with `MakeAtom()` for each opened instance.

Closing the final reference frees the materialized instance and releases its
catalog lease. Generated data remains immutable and is never owned by an X
client. The materializer must not retain pointers outside the leased decoded
view, and catalog code must not inspect the resulting `FontRec`.

### 5. Keep native and embedded policies distinct

The native default build continues to register libXfont's built-in and
font-file FPEs and keeps its current compiled font path. This avoids reducing
`Xfbdev` or `Xvesa` font compatibility during the embedding refactor.

The current `--disable-fonts` configuration becomes the embedded-font
configuration:

- it does not link `xfont` or `fontenc`;
- it compiles the local font utility support and embedded FPE;
- its default font path is `built-ins`;
- it is runnable rather than build-only.

The configure help text now describes the embedded behavior and the old
`TINYX_NO_FONTS` macro has been replaced by `TINYX_EMBEDDED_FONTS`. A follow-up
may replace the historical boolean option with an explicit
`--with-font-backend=xfont|embedded` selection.

An explicit `-fp` or `SetFontPath` request in an embedded build accepts only
`built-ins`. Unknown elements fail with the existing `BadValue` font-path
semantics. The backend must not silently reinterpret arbitrary paths as the
embedded catalog.

### 6. Keep acquisition internal for now

Phase 7 does not add a public host font-provider callback. A callback accepting
raw PCF/BDF bytes would still require a runtime parser; one returning
`FontRec` would expose server internals; and an asynchronous provider would
introduce ownership, suspension, cancellation, and reentrancy rules just
before the Phase 8 public API is defined.

The internal catalog contract is therefore a required architectural seam, not
a provisional public API. A later embedding facade can adapt host-registered
fonts or a host provider to that contract without changing DIX, the FPE, or
bitmap conversion. A decoded-bitmap provider should use the same host-neutral
metrics and bitmap spans and should express ownership through acquisition
leases rather than exposing server objects.

To preserve that path, the Phase 7 implementation must satisfy these
invariants:

- the FPE performs lookup and listing only through the catalog boundary;
- the materializer accepts a decoded view and has no dependency on generated
  font symbols or catalog implementation details;
- every acquired lease is released exactly once, including allocation and
  realization failures;
- built-in aliases belong to catalog policy rather than bitmap conversion;
- generation teardown releases all open leases before catalog teardown;
- no process-lifetime assumption appears in the decoded-font view contract.

The future public provider can then add naming, registration, ownership, and
possibly asynchronous policy without replacing the Phase 7 backend.

## X11-visible behavior

The embedded backend supports:

- `OpenFont` and `CloseFont` for the names above;
- `QueryFont` and `QueryTextExtents`;
- `ListFonts` and `ListFontsWithInfo` over the small catalog;
- `PolyText8/16` and `ImageText8/16` using the normal GC/FB path;
- `CreateGlyphCursor`, including the root cursor;
- `GetFontPath` and a font path containing `built-ins`.

Font-name matching for listing must support the X font wildcard syntax used
by this server (`*` and `?`) and ASCII case-insensitive matching. Aliases are
resolved without exposing duplicate font resources.

Unknown names return `BadName` through the existing DIX request path. Invalid
font paths return `BadValue`. Allocation failures return `BadAlloc`. No
unsupported operation may call `FatalError()` or dereference a missing FPE
callback. Failure to open the configured startup fonts remains fatal because
that indicates an invalid server configuration, not a client request.

## Lifetime and reset behavior

FPE registration and font-path elements are generation state and continue to
be released by `FreeFonts()`. Open font instances are owned through normal DIX
resources. Generated definitions have process lifetime and require no reset.

No backend pointer may survive in an active resource after generation
teardown. Tests must cover at least two initialize/reset cycles to catch stale
atom IDs, FPE indices, cached font pointers, and double frees.

The implementation remains singleton, non-thread-safe, and non-reentrant,
consistent with the rest of the provisional embedding core.

## Validation plan

### Generator and backend tests

- verify source hashes and deterministic generated output;
- validate the fixed font's complete 16-bit encoding map, representative
  non-Latin glyphs, and default character;
- validate all cursor source/mask pairs and representative hotspots;
- exercise each `FontEncoding` lookup mode and missing-glyph behavior;
- exercise supported bitmap bit orders and glyph padding;
- test exact names, aliases, wildcard listing, and unknown names;
- test the materializer with both static and dynamically owned decoded views;
- inject failures at each open stage and verify every catalog lease is released
  exactly once;
- run open/close repeatedly under AddressSanitizer where available.

### Protocol and rendering tests

Using a memory client through the normal handshake and dispatch path:

1. open `fixed`, query it, and close it;
2. open `cursor` and create representative glyph cursors;
3. list `*`, `fixed`, and a nonmatching pattern;
4. verify an unknown font reports the expected X11 error;
5. render text into the memory framebuffer and observe nonempty damage;
6. initialize, tear down, and initialize a second generation.

Tests should fragment client requests and incrementally drain replies so the
font coverage also exercises the Phase 3 transport path.

### Build matrix

- embedded-font recursive build with no `xfont` or `fontenc` linkage;
- `Xmemory` startup through successful default font and root cursor creation;
- existing native libXfont build when legacy dependencies are available;
- existing host-runtime tests and `git diff --check`.

## Implementation slices

1. **Data pipeline:** add provenance, BDF generator, and checked-in fixed and
   cursor definitions with deterministic tests.
2. **Decoded view and catalog seam:** define the internal immutable view and
   acquire/release contract, then implement the static built-in catalog.
3. **Embedded FontRec support:** split reusable libXfont compatibility helpers
   out of the temporary stubs and implement catalog-independent glyph lookup,
   format conversion, properties, lease release, and destruction.
4. **Embedded FPE:** implement registration, path handling, catalog-based
   aliases and listing, opening, and error behavior.
5. **Build and lifecycle integration:** make the no-libXfont configuration use
   `built-ins`, remove the startup-failure warning, and preserve native policy.
6. **End-to-end validation:** add protocol/render/reset tests and update the
   architecture and build documentation.

Phase 7 is complete only when the embedded configuration starts successfully,
renders text, creates its root cursor, and handles unsupported requests using
X11 errors without requiring a filesystem or libXfont.
