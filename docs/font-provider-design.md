# Host-Provided Bitmap Font Design

## Status

This document proposes the public and internal architecture for loading
host-provided BDF and PCF fonts into the embedded TinyX server. It follows the
filesystem-free built-in font backend described in
[Phase 7 Font Architecture](font-design.md), but is not yet implemented.

The first implementation should use synchronous push registration. A host
acquires and, when necessary, decompresses a font file, then gives TinyX one
uncompressed BDF or PCF byte span before exposing any logical clients. TinyX
parses and copies the font during that call. Protocol dispatch never calls back
into the host to acquire a font.

## Goals

The host-font facility should:

- load ordinary BDF and PCF bitmap fonts without libXfont or a filesystem;
- work identically in native static-library and WebAssembly builds;
- preserve the existing DIX FPE, font resource, query, and rendering paths;
- keep `fixed` and `cursor` available without host participation;
- allow a host to register canonical XLFD names and aliases;
- report malformed or unsupported data without poisoning the server;
- copy input synchronously so hosts do not manage font-data leases;
- bound parser memory, arithmetic, and recursion hazards;
- keep the public API free of `FontRec`, FPE, KDrive, and libXfont types.

The initial facility does not need to:

- load scalable OpenType, TrueType, Type 1, SNF, or font-server fonts;
- implement fontconfig or pathname search inside TinyX;
- decompress `.gz`, `.Z`, `.bz2`, or `.xz` streams;
- lazily invoke a host callback from an X11 `OpenFont` request;
- remove or replace a registered font during a server generation;
- override the built-in `fixed` or `cursor` names;
- expose asynchronous acquisition or cancellation.

Modern Xft clients continue to rasterize client-side fonts and send glyphs
through RENDER. This design is for core X11 server-side bitmap fonts used by
legacy clients and toolkits.

## Why push registration

Three acquisition models were considered.

### Let the embedded server read host paths

This preserves traditional X server configuration, but makes path syntax,
filesystem access, sandboxing, decompression, and error policy core concerns.
It also does not translate naturally to browsers or other WASM hosts.

**Decision:** do not add filesystem operations to the embedded core.

### Invoke a host provider during `OpenFont`

A provider callback could lazily return bytes for an arbitrary requested name.
However, the callback would run inside request dispatch and the protected fatal
boundary. It would need reentrancy, ownership, timeout, cancellation, caching,
and asynchronous wakeup rules. `ListFonts` would additionally require stable
provider enumeration while a client request is in progress.

**Decision:** defer pull callbacks. The internal catalog lease remains capable
of supporting them later, but they are unnecessary for initial host loading.

### Register complete fonts before clients

The host performs its own I/O and decompression, calls TinyX with an
uncompressed byte span, and can discard that span when the call returns.
Registration happens at an explicit API boundary rather than from protocol
dispatch. The resulting catalog is immutable while clients can observe it.

**Decision:** use synchronous push registration.

## Public API

The addition should increment `TINYX_API_VERSION_MINOR`. It adds new types and
functions without extending any existing structure.

An illustrative API is:

```c
typedef enum tinyx_font_format {
    TINYX_FONT_FORMAT_AUTO = 0,
    TINYX_FONT_FORMAT_BDF = 1,
    TINYX_FONT_FORMAT_PCF = 2
} tinyx_font_format;

typedef struct tinyx_font_config {
    uint32_t struct_size;
    tinyx_font_format format;

    /* Optional catalog name in addition to the font's canonical XLFD. */
    const char *primary_name;

    /* Optional additional names for the same immutable font. */
    const char *const *aliases;
    size_t alias_count;
} tinyx_font_config;

void tinyx_font_config_init(tinyx_font_config *config);

tinyx_status tinyx_server_add_font(
    tinyx_server *server,
    const tinyx_font_config *config,
    const void *bytes,
    size_t length,
    tinyx_error *error);

tinyx_status tinyx_server_add_font_alias(
    tinyx_server *server,
    const char *alias,
    const char *target,
    tinyx_error *error);
```

Exact names remain provisional until implementation, but the semantics below
are required.

### Registration timing

A server is created first so startup can use the guaranteed built-in `fixed`
and `cursor` fonts. The host then registers all additional fonts and aliases
before opening its first `tinyx_client`.

The first successful `tinyx_client_open()` freezes the font catalog.
Subsequent font or alias registration returns
`TINYX_ERROR_INVALID_STATE`. This avoids changing `ListFonts` results during a
suspended or partially dispatched request and avoids defining removal or
replacement semantics for open font resources.

A typical host sequence is:

```c
tinyx_server_create(&server_config, &server, &error);

tinyx_font_config_init(&font_config);
font_config.format = TINYX_FONT_FORMAT_PCF;
font_config.aliases = aliases;
font_config.alias_count = alias_count;
tinyx_server_add_font(server, &font_config, pcf, pcf_size, &error);

/* Add aliases from fonts.alias only after their target fonts exist. */
tinyx_server_add_font_alias(server, "9x15", canonical_name, &error);

/* Catalog becomes immutable here. */
tinyx_client_open(server, &client_config, &client);
```

The API remains singleton, server-thread-only, non-thread-safe, and
non-reentrant. Font registration must not occur from a TinyX host callback.

### Input ownership

`bytes`, `primary_name`, the alias pointer array, each alias string, and
`tinyx_error` are borrowed only for the duration of the call. TinyX either
publishes a complete independently owned decoded font or publishes nothing.
The host may free or reuse every input after the call returns.

Registration does not retain compressed source bytes and does not retain
pointers into the caller's buffer. Open `FontRec` instances acquire ordinary
catalog leases over immutable decoded data owned by the server catalog.

### Names and aliases

A BDF `FONT` declaration or PCF `FONT` property supplies the canonical XLFD
and is required. `primary_name`, when present, adds another lookup name; it
does not rewrite the font's `FONT` property. Each entry in `aliases` behaves
the same way.

Names:

- are nonempty printable ASCII;
- are limited to 255 bytes, matching the embedded FPE's request handling;
- compare case-insensitively as existing core font names do;
- may contain XLFD punctuation but not embedded NUL bytes;
- must be unique across built-ins, registered fonts, and aliases.

`tinyx_server_add_font_alias()` resolves `target` immediately and stores a
direct reference to the decoded font. The target must already exist. There are
therefore no alias chains or cycles in the catalog representation.

The initial policy rejects every duplicate name with
`TINYX_ERROR_ALREADY_EXISTS`, even if it would refer to the same font. Built-in
names, including `fixed` and `cursor`, cannot be replaced. A later API may add
an explicit override namespace if a demonstrated use case requires it.

Aliases are included in `ListFonts` and `ListFontsWithInfo`, consistent with
the current built-in catalog.

### Formats and compression

`TINYX_FONT_FORMAT_AUTO` recognizes uncompressed BDF from its `STARTFONT`
header and uncompressed PCF from its file magic. Unknown data returns
`TINYX_ERROR_UNSUPPORTED`.

The core does not decompress files. A host loading `font.pcf.gz` must
uncompress it and register the resulting PCF bytes. This keeps zlib and other
compression libraries out of native and WASM core products and lets each host
use its natural streaming APIs.

A format explicitly selected by the caller must match the supplied bytes.
Trailing bytes are rejected unless the selected file format explicitly permits
them.

### Errors

The operation returns:

- `TINYX_ERROR_INVALID_ARGUMENT` for malformed data, invalid names, invalid
  structure sizes, or impossible metrics;
- `TINYX_ERROR_UNSUPPORTED` for a recognized but unsupported format or feature;
- `TINYX_ERROR_ALREADY_EXISTS` for any colliding catalog name;
- `TINYX_ERROR_OUT_OF_MEMORY` for allocation failure;
- `TINYX_ERROR_INVALID_STATE` after the catalog is frozen or generation end;
- `TINYX_ERROR_FATAL` only if an unrelated fatal server invariant unwinds the
  protected operation.

`tinyx_error.message` should identify the format, table or BDF section, and
byte offset or line number when practical. Parser errors are ordinary API
errors: malformed host input must never call `FatalError()` or poison the
server.

## Internal architecture

### Host-neutral decoded representation

`TinyXDecodedFont`, `TinyXDecodedGlyph`, properties, and encoding maps should
move into a header that has no X server includes. The representation remains:

- immutable metrics and properties;
- a sparse mapping over one- or two-byte X font encodings;
- tightly packed canonical MSB-first bitmap rows;
- one canonical name;
- explicit owned backing allocations.

The existing encoding map uses `int16_t` glyph indexes. Host fonts may contain
more than 32,767 glyphs, so the parser work should change it to `int32_t` and
permit up to 65,536 encoded glyphs.

Generated built-ins and parsed fonts must produce exactly the same decoded
view. The DIX materializer must not know whether a view came from generated C,
BDF, or PCF.

### Mutable-before-freeze catalog

The current private built-in catalog in `dix/embedded-font.c` should be
extracted into an internal catalog component with these conceptual operations:

```c
int TinyXFontCatalogAddOwned(TinyXOwnedDecodedFont *font,
                             const char *const *names,
                             size_t name_count);
int TinyXFontCatalogAddAlias(const char *alias, const char *target);
void TinyXFontCatalogFreeze(void);
void TinyXFontCatalogReset(void);
```

The actual return type should distinguish malformed input, duplicates,
unsupported features, and allocation failure without using public API types in
DIX.

Generated built-ins are installed first as borrowed process-lifetime entries.
Parsed fonts are owned generation entries. Catalog publication is atomic:
parsing, name validation, all allocations, and duplicate checks complete before
any name becomes visible.

Catalog entries never move after publication. Name lookup tables may be
reallocated because leases refer to stable font objects rather than table
slots. The first client freezes both name and font collections.

`FreeFonts()` closes all `FontRec` instances before parsed catalog storage is
released. On an ordinary shutdown, the facade resets the dynamic catalog only
after generation teardown. If the singleton is poisoned and core teardown is
unsafe, retaining catalog allocations until process or module disposal is
preferable to freeing data that a stranded `FontRec` may still reference.

### Parsers

Parsers belong below the DIX adapter, for example under `fonts/`, and return an
owned decoded font or a structured parse error. They must not create atoms,
`FontRec` objects, FPEs, resources, or screen data.

The BDF and PCF parsers share:

- checked integer conversion and multiplication helpers;
- decoded-font validation;
- canonical bitmap row construction;
- property ownership and normalization;
- a bounded diagnostic builder;
- failure cleanup for every partially allocated object.

The materializer in `dix/embedded-font.c` remains responsible for converting
canonical rows to DIX-requested bit order, byte order, scanline unit, and glyph
padding.

## BDF support

The initial parser should support BDF 2.1 and 2.2 files with:

- `FONT`, `SIZE`, and `FONTBOUNDINGBOX`;
- `STARTPROPERTIES` and string or integer properties;
- `CHARS`, `STARTCHAR`, `ENCODING`, `SWIDTH`, `DWIDTH`, `BBX`, and `BITMAP`;
- `FONT_ASCENT`, `FONT_DESCENT`, and `DEFAULT_CHAR` properties;
- `ENCODING -1 secondary` for an alternate nonnegative encoding;
- one- and two-byte encoding values from 0 through 65535;
- zero-sized glyph bitmaps where valid;
- negative left bearings and descents representable by X font metrics.

`SWIDTH` is validated syntactically but does not affect bitmap materialization.
Vertical metrics, multiple writing directions, nonzero `DWIDTH` Y values, and
encodings outside the X11 16-bit space should initially return
`TINYX_ERROR_UNSUPPORTED` rather than being silently misinterpreted.

The parser must verify declared counts, exact bitmap row widths, hexadecimal
syntax, duplicate encodings, duplicate required fields, and required end
markers. It should accept normal CRLF or LF input but reject embedded NUL bytes.

## PCF support

The initial parser should support uncompressed PCF files containing:

- `PCF_PROPERTIES`;
- default or compressed `PCF_METRICS`;
- `PCF_BITMAPS`;
- `PCF_BDF_ENCODINGS`;
- optional `PCF_ACCELERATORS`, `PCF_BDF_ACCELERATORS`, and
  `PCF_INK_METRICS` tables.

It must handle both legal byte orders, bit orders, scan units, and glyph
padding values and normalize bitmap rows into the same canonical form used by
BDF and generated fonts. Encoding entries that denote no glyph remain sparse.
All table offsets, sizes, format words, counts, string offsets, glyph offsets,
and bitmap spans are checked against the supplied byte length before use.

Accelerator tables may be validated and ignored because decoded bounds are
recomputed from glyph metrics. Ink metrics may also be ignored initially; the
ordinary metrics and bitmaps remain authoritative for core text rendering.
Unsupported PCF table formats return `TINYX_ERROR_UNSUPPORTED`.

The implementation may adapt the permissively licensed X.Org PCF algorithms,
but should not import libXfont's file, compression, cache, or `FontRec`
interfaces. A bounded memory reader and direct decoded-font output keep the
parser useful in WASM and fuzz tests.

## Resource limits and validation

Font bytes are untrusted host input even though logical clients are trusted.
The parser must impose implementation limits before allocation. Initial hard
limits should include:

- 64 MiB source byte span;
- 65,536 glyphs and encoding cells;
- 64 MiB total canonical bitmap data per font;
- 4,096 properties;
- 255-byte catalog and canonical names;
- bounded BDF line and property-string lengths;
- metrics and bitmap dimensions representable by the existing X font ABI.

These limits are implementation constants, not public configuration in the
first version. They can be relaxed compatibly after profiling real fonts.
Every count, offset, stride, area, and cumulative size uses checked arithmetic.

After parsing, one shared validator verifies:

- ordered encoding bounds and exact map dimensions;
- valid glyph indexes or the missing sentinel;
- every metric fits `xCharInfo` when materialized;
- every bitmap offset and row span lies within owned storage;
- the default character is absent or maps consistently;
- all properties and names are terminated and owned;
- catalog-visible names satisfy the public name rules.

## Host responsibilities

A native host may:

1. read `fonts.dir` and `fonts.alias` using its own filesystem policy;
2. read and decompress selected `.pcf.gz` files;
3. register each uncompressed PCF;
4. add aliases after their targets exist;
5. open its first logical client.

A browser host may fetch or bundle font assets, decompress them with browser or
JavaScript facilities, copy the uncompressed bytes into WASM memory for the
registration call, then release that temporary allocation.

TinyX does not watch directories, infer font paths, resolve filenames, or
perform locale fallback. Which fonts are made available remains host policy.

## Testing

### Parser unit tests

- parse representative public-domain X.Org BDF and PCF fixtures;
- compare decoded BDF and PCF output generated from the same source;
- exercise both PCF byte and bit orders and every legal glyph padding;
- cover compressed and uncompressed PCF metrics;
- validate sparse two-byte encodings and default characters;
- reject truncated input at every byte boundary;
- reject overflowing counts, offsets, dimensions, and bitmap spans;
- reject duplicate BDF fields and encodings;
- inject allocation failure at every owned allocation;
- fuzz both parsers with AddressSanitizer and UndefinedBehaviorSanitizer.

Normal builds must not require `bdftopcf`; checked-in fixtures should include
source provenance and deterministic regeneration instructions.

### Catalog and API tests

- register a BDF and a PCF before opening a client;
- register primary names and aliases and list each through X11;
- reject collisions with built-ins and previously registered names;
- verify a failed multi-alias registration publishes nothing;
- freeze registration on the first client open;
- verify input bytes can be freed immediately after registration;
- open and close multiple `FontRec` instances over one registered font;
- shut down with registered fonts still open;
- preserve ordinary parser errors without poisoning the server.

### Protocol and rendering tests

Through a descriptor-free client:

- `ListFonts`, `OpenFont`, `QueryFont`, and `CloseFont` a registered font;
- render 8-bit and two-byte glyphs into the framebuffer;
- verify metrics and damaged pixels against expected fixture data;
- request an unknown name and retain `BadName` behavior;
- repeat under native CMake, Emscripten/Node, and Autotools builds.

## Implementation slices

1. **Decoded ownership:** make the decoded-font representation host-neutral,
   widen encoding indexes, and add shared validation and destruction.
2. **BDF parser:** parse bounded uncompressed BDF into an owned decoded font,
   with unit and fuzz tests.
3. **PCF parser:** add a bounded memory reader and required PCF tables, then
   cross-check output against BDF fixtures.
4. **Mutable catalog:** combine borrowed built-ins and owned parsed entries,
   implement atomic aliases, freezing, and shutdown ordering.
5. **Public API:** add registration types and calls, facade state enforcement,
   detailed diagnostics, and explicit WASM exports.
6. **Host integration:** teach the Kitty host to optionally load a directory or
   explicit font files before accepting socket clients.
7. **End-to-end validation:** exercise listing, opening, rendering, aliases,
   shutdown, native builds, and WASM.

## Completion criteria

Host-provided font loading is complete when:

- native and WASM hosts can register uncompressed BDF and PCF bytes without a
  filesystem dependency in the core;
- input bytes and names are copied synchronously and have documented lifetime;
- registered names and aliases participate in normal X11 listing and opening;
- registered glyphs render through the existing DIX, GC, MI, and FB path;
- malformed files return bounded diagnostics without poisoning the server;
- the catalog is immutable once clients exist and tears down after open fonts;
- built-in `fixed` and `cursor` remain available with no host fonts;
- native libXfont behavior remains unchanged in its existing build mode.
