# Host-Provided Bitmap Font Design

## Status

This document proposes the public and internal architecture for loading
host-provided BDF and PCF fonts into the embedded TinyX server. It follows the
filesystem-free built-in font backend described in
[Phase 7 Font Architecture](font-design.md), but is not yet implemented.

The first implementation should use a synchronous, constrained pull provider.
A host supplies one provider and a complete immutable manifest of canonical
font names, aliases, formats, and opaque source identifiers in the initial
server configuration. TinyX uses that manifest for name lookup and listing,
but acquires an uncompressed BDF or PCF byte span from the provider only when
protocol work first requires that font's contents. TinyX parses and copies the font during
that acquisition, releases the provider's byte lease before resuming protocol
dispatch, and caches the decoded result for the rest of the server lifetime.

The provider callback is deliberately not an I/O API. It must synchronously
return already-available bytes, must not block, and must not reenter TinyX.
Filesystem access, fetching, decompression, and readiness remain host
responsibilities.

## Goals

The host-font facility should:

- load ordinary BDF and PCF bitmap fonts without libXfont or a core filesystem;
- work identically in native static-library and WebAssembly builds;
- preserve the existing DIX FPE, font resource, query, and rendering paths;
- keep `fixed` and `cursor` available without host participation;
- expose a stable creation-time catalog of canonical XLFD names and aliases;
- acquire and parse a font only when its metrics or glyphs are first needed;
- avoid copying or retaining provider source bytes after parsing;
- report malformed or unsupported data without poisoning the server;
- express provider byte ownership with an explicit acquire/release lease;
- bound parser memory, arithmetic, and recursion hazards;
- keep the public API free of `FontRec`, FPE, KDrive, and libXfont types.

The initial facility does not need to:

- load scalable OpenType, TrueType, Type 1, SNF, or font-server fonts;
- implement fontconfig or pathname search inside TinyX;
- decompress `.gz`, `.Z`, `.bz2`, or `.xz` streams;
- perform filesystem or network I/O from a provider callback;
- suspend an `OpenFont` request for asynchronous acquisition;
- add, remove, or replace a manifest entry after server creation;
- override the built-in `fixed` or `cursor` names;
- evict successfully decoded fonts during the server lifetime;
- retry a provider or parser failure during the same server lifetime.

Modern Xft clients continue to rasterize client-side fonts and send glyphs
through RENDER. This design is for core X11 server-side bitmap fonts used by
legacy clients and toolkits.

## Why constrained pull

Four acquisition models were considered.

### Let the embedded server read host paths

This preserves traditional X server configuration, but makes path syntax,
filesystem access, sandboxing, decompression, and error policy core concerns.
It also does not translate naturally to browsers or other WASM hosts.

**Decision:** do not add filesystem operations to the embedded core.

### Push complete fonts before clients

A host could perform its own I/O and decompression, then give TinyX every
uncompressed byte span before clients exist. This creates a simple explicit
failure boundary and lets TinyX validate the complete catalog eagerly.
However, it also requires every advertised font to be transferred, parsed, and
retained even if no client ever requests it. A host that already owns or caches
font assets must duplicate work and decoded storage up front.

**Decision:** do not require eager payload registration.

### Resolve arbitrary names during `OpenFont`

A provider callback could receive an arbitrary client-supplied font name and
return bytes. That leaves `ListFonts` and `ListFontsWithInfo` without a stable
namespace, permits the visible catalog to change during dispatch, and makes
aliases and wildcard matching host policy. If acquisition can block or return
pending, it additionally requires request suspension, cancellation, wakeups,
timeouts, and client-death handling.

**Decision:** do not make the provider an open-ended name resolver and do not
support asynchronous acquisition in the initial API.

### Register a manifest and pull payloads by source identifier

The host declares the complete visible namespace in `tinyx_config`. TinyX
validates and copies that namespace while creating the server, performs
wildcard matching and alias resolution itself, and calls the provider only with
an opaque identifier from a validated manifest entry. The provider returns
already-available, uncompressed bytes under a short-lived lease. TinyX parses once and caches the
host-neutral decoded font.

This preserves deterministic protocol enumeration while avoiding eager parsing
and source-byte duplication. It also matches the acquire/release seam already
used by the embedded FPE.

**Decision:** use an immutable creation-time manifest with synchronous pulled
payloads.

## Public API

The addition should increment `TINYX_API_VERSION_MINOR`. It adds new types and
functions and appends one optional field to the size-versioned `tinyx_config`.

An illustrative API is:

```c
typedef enum tinyx_font_format {
    TINYX_FONT_FORMAT_AUTO = 0,
    TINYX_FONT_FORMAT_BDF = 1,
    TINYX_FONT_FORMAT_PCF = 2
} tinyx_font_format;

typedef struct tinyx_font_provider_ops {
    uint32_t struct_size;

    tinyx_status (*acquire)(void *userdata,
                            uint64_t source_id,
                            const void **bytes,
                            size_t *length,
                            void **lease,
                            tinyx_error *error);
    void (*release)(void *userdata, void *lease);
} tinyx_font_provider_ops;

typedef struct tinyx_font_source {
    uint32_t struct_size;
    uint64_t source_id;
    tinyx_font_format format;

    /* Required catalog name and expected BDF/PCF canonical FONT value. */
    const char *canonical_name;
} tinyx_font_source;

typedef struct tinyx_font_alias {
    uint32_t struct_size;
    const char *name;

    /* A canonical host-source name or a built-in name, never an alias. */
    const char *target;
} tinyx_font_alias;

typedef struct tinyx_font_provider_config {
    uint32_t struct_size;
    tinyx_font_provider_ops ops;
    void *userdata;

    const tinyx_font_source *sources;
    size_t source_count;
    const tinyx_font_alias *aliases;
    size_t alias_count;
} tinyx_font_provider_config;

/* Appended to the existing size-versioned structure. */
typedef struct tinyx_config {
    /* Existing fields... */
    const tinyx_font_provider_config *font_provider;
} tinyx_config;

void tinyx_font_provider_ops_init(tinyx_font_provider_ops *ops);
void tinyx_font_provider_config_init(tinyx_font_provider_config *config);
void tinyx_font_source_init(tinyx_font_source *source);
void tinyx_font_alias_init(tinyx_font_alias *alias);
```

Exact names remain provisional until implementation, but the semantics below
are required.

### Creation-time configuration

The provider and complete manifest are immutable server configuration, like the
existing host callbacks and initial screen. The host constructs provider state,
source descriptors, and aliases first and supplies them through `tinyx_config`.
There is no post-creation catalog mutation API and no intermediate server state
in which configuration is incomplete.

A typical host sequence is:

```c
tinyx_font_source_init(&sources[0]);
sources[0].source_id = FONT_9X15;
sources[0].format = TINYX_FONT_FORMAT_PCF;
sources[0].canonical_name = canonical_9x15_name;

tinyx_font_alias_init(&aliases[0]);
aliases[0].name = "9x15";
aliases[0].target = canonical_9x15_name;

tinyx_font_provider_config_init(&font_provider);
font_provider.ops.acquire = acquire_font;
font_provider.ops.release = release_font;
font_provider.userdata = host_fonts;
font_provider.sources = sources;
font_provider.source_count = 1;
font_provider.aliases = aliases;
font_provider.alias_count = 1;
server_config.font_provider = &font_provider;

tinyx_server_create(&server_config, &server, &error);
```

TinyX validates and copies the whole manifest atomically before entering the
core server lifecycle. Duplicate source identifiers, duplicate names, unknown
alias targets, aliases that target aliases, malformed structures, and
collisions with built-in names reject creation without publishing a partial
catalog. An ordinary configuration error leaves `out_server` null and does not
consume the process's single permitted TinyX lifetime.

Startup continues to use the guaranteed built-in `fixed` and `cursor` fonts,
so provider callbacks are not invoked by `tinyx_server_create()`. The manifest
is nevertheless already frozen when creation succeeds. Lazy source state may
subsequently move from unloaded to decoded or terminal failure, but its visible
names and mappings never change.

The provider configuration may be omitted when only built-in fonts are needed.
A non-null provider requires valid acquire and release operations. Source and
alias arrays may be empty, although aliases without host sources may target
built-in canonical names.

`tinyx_config.font_provider`, its nested configuration, operation table, source
and alias arrays, and every manifest string are borrowed only for
`tinyx_server_create()`. TinyX validates and copies them during creation. Only
`userdata` remains borrowed for the server lifetime and must remain valid until
`tinyx_server_destroy()` returns. Older callers whose
`tinyx_config.struct_size` ends before the appended field behave as though
`font_provider` were null.

The API remains singleton, server-thread-only, non-thread-safe, and
non-reentrant. Provider state must be initialized before server creation and
must not call TinyX from its callbacks.

### Provider callback contract

`acquire` is invoked from protocol dispatch after TinyX resolves a frozen
manifest entry. Its `source_id` is exactly the value copied from that entry;
client-controlled names are never passed to the provider.

The callback must:

- return synchronously and promptly;
- perform no filesystem, network, or other potentially blocking operation;
- return an uncompressed BDF or PCF span already available to the host;
- return a non-null lease on success, even when the bytes have static lifetime;
- keep `bytes[0..length]` valid and immutable until `release`;
- avoid calling any TinyX API, directly or indirectly;
- write only bounded diagnostic text to the supplied `tinyx_error`.

`release` is called exactly once after every successful acquisition, including
format mismatch, parse failure, validation failure, and allocation failure.
TinyX does not retain pointers into provider storage after release. The release
callback must also be synchronous and non-reentrant.

`TINYX_ERROR_WOULD_BLOCK` is not a supported acquisition result in the initial
API because dispatch has no public pending-font operation. Hosts must acquire,
fetch, map, or decompress assets before accepting clients that may request
them. Asynchronous host work can invoke the ordinary host wakeup mechanism,
but cannot resume an already failed font request.

The provider operations are copied. `userdata` is borrowed and must remain
valid until `tinyx_server_destroy()` returns. Provider callbacks are never
invoked after destruction returns. If fatal poisoning prevents safe core
teardown, TinyX clears callbacks before returning control so stranded internal
objects cannot call dead host state.

A successful payload is parsed and cached on first use. The provider is not
called again for that source during the server lifetime. Provider and parser
failures are also cached as terminal failures, preventing repeated callbacks
or repeated parsing from client requests. A client observes ordinary font-open
failure; the detailed diagnostic is sent to the configured host logger and
retained where practical as server diagnostic state.

### Manifest ownership

The source and alias arrays, every `canonical_name`, alias `name` and `target`,
and `tinyx_error` are borrowed only for the duration of
`tinyx_server_create()`. TinyX validates and copies the complete manifest
atomically. The host may free or reuse every manifest input after creation
returns.

`source_id` is an opaque value interpreted only by the host. It must be unique
among host font sources and remains associated with its copied manifest entry.
No pointer is encoded in the identifier, which keeps the API portable across
native and WebAssembly hosts.

### Names and aliases

Every source declares its expected canonical name at creation. The BDF `FONT`
declaration or PCF `FONT` property must compare equal to that name when the
payload is first parsed. A mismatch permanently fails the source and is
reported as malformed provider data. Requiring the name in the manifest
prevents lazy parsing from adding a new protocol-visible name after creation.

Names:

- are nonempty printable ASCII;
- are limited to 255 bytes, matching the embedded FPE's request handling;
- compare case-insensitively as existing core font names do;
- may contain XLFD punctuation but not embedded NUL bytes;
- must be unique across built-ins, source names, and aliases.

Each alias target is resolved while validating the complete creation-time
manifest and stored as a direct reference to a built-in or host source. Targets
must name canonical sources rather than other aliases. Declaration order is
irrelevant, and there can be no alias chains or cycles in the catalog
representation.

The initial policy rejects every duplicate name with
`TINYX_ERROR_ALREADY_EXISTS`, even if it would refer to the same font. Built-in
names, including `fixed` and `cursor`, cannot be replaced. A later API may add
an explicit override namespace if a demonstrated use case requires it.

Manifest names and aliases are included in `ListFonts` without acquiring their
payloads. `ListFontsWithInfo`, `OpenFont`, and any other operation requiring
metrics or properties may acquire and parse matching sources. A single
`ListFontsWithInfo` request can therefore invoke the provider for multiple
previously unused fonts; the prompt, nonblocking callback contract applies to
each invocation.

### Formats and compression

`TINYX_FONT_FORMAT_AUTO` recognizes uncompressed BDF from its `STARTFONT`
header and uncompressed PCF from its file magic. Unknown data produces a
terminal unsupported-source failure.

The core does not decompress files. A host advertising `font.pcf.gz` must make
the uncompressed PCF bytes synchronously available to `acquire`. This keeps
zlib and other compression libraries out of native and WASM core products and
lets each host use its natural acquisition and decompression APIs outside
protocol dispatch.

A format explicitly selected in the manifest must match the acquired bytes.
Trailing bytes are rejected unless the selected file format explicitly permits
them.

### Errors

Server creation returns:

- `TINYX_ERROR_INVALID_ARGUMENT` for an invalid provider configuration,
  callbacks, structures, names, identifiers, or alias targets;
- `TINYX_ERROR_ALREADY_EXISTS` for a duplicate source identifier or colliding
  catalog name;
- `TINYX_ERROR_OUT_OF_MEMORY` for manifest allocation failure;
- `TINYX_ERROR_FATAL` only if an unrelated fatal server invariant unwinds the
  protected operation.

Manifest validation occurs before core initialization. These ordinary errors
leave `out_server` null, release all temporary copies, and do not poison or
consume the singleton.

Acquisition and parse failures happen during protocol dispatch rather than the
configuration call. They must never call `FatalError()` or poison the server.
They map to ordinary X11 errors appropriate to the FPE operation, principally
`BadName` for an unavailable or malformed font and `BadAlloc` for allocation
failure. The host diagnostic should identify the source identifier, format,
table or BDF section, and byte offset or line number when practical.

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

### Frozen manifest and lazy source state

The current private built-in catalog in `dix/embedded-font.c` should be
extracted into an internal catalog component. Conceptually it stores:

- borrowed process-lifetime built-in decoded fonts;
- copied host source manifests and direct alias mappings;
- copied provider operations and borrowed provider userdata;
- per-source states: unloaded, loading, decoded, or terminal failure;
- owned decoded fonts produced by successful lazy parsing.

Its operations resemble:

```c
int TinyXFontManifestCreate(const TinyXFontProvider *provider,
                            const TinyXFontSourceManifest *sources,
                            size_t source_count,
                            const TinyXFontAliasManifest *aliases,
                            size_t alias_count,
                            TinyXFontManifest **out_manifest);
int TinyXFontCatalogInstall(TinyXFontManifest *manifest);
int TinyXFontCatalogAcquire(const char *name, TinyXDecodedFontLease *lease);
void TinyXFontCatalogReset(void);
```

The actual result type should distinguish provider, malformed-data,
unsupported-feature, duplicate, and allocation failures without using public
API types in DIX.

Manifest creation combines built-in definitions, host sources, and aliases in
temporary storage, validates the whole namespace, and publishes nothing on
failure. The facade performs this step before core initialization. Generation
startup adopts the already immutable manifest when registering the embedded
FPE. Catalog entries and name mappings never move after successful creation;
leases refer to stable source/font objects rather than table slots.

On first acquire, a source transitions from unloaded to loading. Reentrant
lookup of the same or another host source while this state is active is an API
contract violation and fails safely. Successful parsing publishes one owned
immutable decoded font and transitions to decoded before the materializer sees
it. Failure stores a bounded terminal diagnostic and transitions to terminal
failure. No partially decoded object becomes visible.

`FreeFonts()` closes all `FontRec` instances before parsed catalog storage is
released. On ordinary shutdown, the facade resets the dynamic catalog only
after generation teardown. If the singleton is poisoned and core teardown is
unsafe, retaining decoded catalog allocations until process or module disposal
is preferable to freeing data that a stranded `FontRec` may still reference.
Host callbacks must nevertheless be detached before control returns after a
fatal unwind.

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
encodings outside the X11 16-bit space should initially return an unsupported
parse result rather than being silently misinterpreted.

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
Unsupported PCF table formats return an unsupported parse result.

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

The manifest should additionally bound the number of host sources, total
aliases, and total copied name bytes so an embedder cannot create an unbounded
`ListFonts` workload at creation. Exact limits should be implementation
constants rather than public configuration in the first version.

Every count, offset, stride, area, and cumulative size uses checked arithmetic.
After parsing, one shared validator verifies:

- the parsed canonical name matches the frozen manifest;
- ordered encoding bounds and exact map dimensions;
- valid glyph indexes or the missing sentinel;
- every metric fits `xCharInfo` when materialized;
- every bitmap offset and row span lies within owned storage;
- the default character is absent or maps consistently;
- all properties and names are terminated and owned;
- catalog-visible names satisfy the public name rules.

## Host responsibilities

A native embedder may:

1. read `fonts.dir` and `fonts.alias` using its own filesystem policy;
2. assign stable numeric source identifiers and construct the complete
   creation-time manifest;
3. create TinyX with that manifest;
4. read, map, and decompress selected `.pcf.gz` files before clients can request
   them;
5. serve the resulting uncompressed spans synchronously from `acquire`;
6. retain or discard its source cache after TinyX calls `release`, according to
   host policy.

A browser host may fetch or bundle font assets and decompress them with browser
or JavaScript facilities before accepting clients. Its provider then exposes
already-resident bytes to WASM for the duration of acquisition. The host may
release its original asset after successful parsing if it knows no other
consumer needs it; TinyX retains its own decoded copy.

TinyX does not watch directories, infer font paths, resolve filenames, fetch
URLs, perform locale fallback, or invoke asynchronous host work. Which fonts
are advertised and how their payloads become ready remain host policy.

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

### Provider, catalog, and API tests

- configure a provider and complete BDF/PCF manifest during server creation;
- create a server without a provider and retain built-in-only behavior;
- reject missing or malformed provider operations when a manifest is present;
- verify provider configuration, source, alias, and string storage is borrowed
  only during creation;
- list creation-time canonical names and aliases without acquisition;
- reject duplicate source identifiers, duplicate names, unknown alias targets,
  alias chains, and collisions with built-ins;
- verify any invalid source or alias rejects creation and publishes nothing;
- verify an ordinary manifest error does not consume the singleton lifetime;
- verify `OpenFont` receives the expected opaque source identifier;
- verify acquired bytes are released after success and every failure path;
- verify source bytes may be freed immediately from `release`;
- verify successful decoding and terminal failures are each cached;
- reject callback reentrancy without corrupting catalog state;
- open and close multiple `FontRec` instances over one decoded font;
- shut down with registered sources and fonts still open;
- detach callbacks safely after fatal poisoning.

### Protocol and rendering tests

Through a descriptor-free client:

- `ListFonts` a registered source without invoking its provider;
- `ListFontsWithInfo`, `OpenFont`, `QueryFont`, and `CloseFont` a host source;
- render 8-bit and two-byte glyphs into the framebuffer;
- verify metrics and damaged pixels against expected fixture data;
- request an unknown name without invoking the provider and retain `BadName`;
- supply malformed provider bytes and retain ordinary X11 failure semantics;
- repeat under native CMake and Emscripten/Node builds.

## Implementation slices

1. **Decoded ownership:** make the decoded-font representation host-neutral,
   widen encoding indexes, and add shared validation and destruction.
2. **BDF parser:** parse bounded uncompressed BDF into an owned decoded font,
   with unit and fuzz tests.
3. **PCF parser:** add a bounded memory reader and required PCF tables, then
   cross-check output against BDF fixtures.
4. **Manifest and lazy catalog:** atomically combine borrowed built-ins with
   copied creation-time source descriptors and aliases; add lazy source state,
   decoded caching, and shutdown ordering.
5. **Public provider API:** extend initial server configuration with immutable
   provider operations and the complete source/alias manifest; add callback
   leases, diagnostics, reentrancy guards, and explicit WASM exports.
6. **Embedder integration:** teach the Kitty embedder to discover and preload a font
   manifest and serve ready uncompressed bytes by source identifier.
7. **End-to-end validation:** exercise listing without acquisition, lazy open,
   rendering, caching, aliases, failure, shutdown, native embedders, and WASM.

## Completion criteria

Host-provided font loading is complete when:

- native and WASM hosts can advertise BDF and PCF sources without giving the
  core filesystem access;
- manifest names and aliases are validated and copied atomically during server
  creation and remain immutable;
- `ListFonts` uses the frozen manifest without acquiring source bytes;
- metrics or glyph requests synchronously acquire each source at most once;
- every successful acquisition has one matching release and TinyX retains no
  provider byte pointer afterward;
- registered glyphs render through the existing DIX, GC, MI, and FB path;
- malformed files produce bounded host diagnostics and ordinary X11 errors
  without poisoning the server;
- provider callbacks are prompt, non-reentrant, and detached safely at teardown;
- decoded catalog storage outlives all open `FontRec` leases;
- built-in `fixed` and `cursor` remain available with no provider;
- native libXfont behavior remains unchanged in its existing build mode.
