# Embedded startup fonts

The Phase 7 embedded backend uses decoded C tables generated from these X.Org
font releases:

- `font-misc-misc-1.1.3/6x13.bdf`
  - SHA-256: `c68c55ca8effcdc2714fffa9c8e07df290411867b00b547eeae952f7473efc9b`
  - the source identifies the font as public domain;
- `font-cursor-misc-1.0.4/cursor.bdf`
  - SHA-256: `308e987d461e1458860e179467924566d7a0e1c4c7046d8abe0cb86271e6ca53`
  - the source states that the glyphs are unencumbered.

Release archives are available from <https://www.x.org/releases/individual/font/>.
The BDF inputs are not needed by normal TinyX builds. To reproduce
`dix/embedded-font-data.c`, extract both releases and run:

```sh
fonts/generate-builtin-fonts.py \
  --fixed /path/to/font-misc-misc-1.1.3/6x13.bdf \
  --cursor /path/to/font-cursor-misc-1.0.4/cursor.bdf \
  --output dix/embedded-font-data.c
```

The generator checks the structure of every retained glyph. It keeps all 4,121
glyphs from the source 6x13 ISO10646-1 font in a sparse two-byte encoding and
all 154 cursor glyphs. The full 6x13 name used by xterm is available alongside
`fixed`, `6x13`, and historical ISO8859-1 aliases. Generated bitmaps use compact
MSB-first rows; the runtime materializer converts them to the format requested
by DIX.
