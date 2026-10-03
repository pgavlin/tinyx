# Embedded startup fonts

The Phase 7 embedded backend uses decoded C tables generated from these X.Org
font releases:

- `font-misc-misc-1.1.3/6x13.bdf`
  - SHA-256: `c68c55ca8effcdc2714fffa9c8e07df290411867b00b547eeae952f7473efc9b`
  - the source identifies the font as public domain;
- `font-cursor-misc-1.0.4/cursor.bdf`
  - SHA-256: `308e987d461e1458860e179467924566d7a0e1c4c7046d8abe0cb86271e6ca53`
  - the source states that the glyphs are unencumbered;
- `font-adobe-100dpi-1.0.4/{helvR10,helvB10,helvBO10,helvR14}.bdf`
  - SHA-256: `8974855b72a59c8ef21b1773c86e350f6b7ec976ac6d09c37bc910f1ecb0e347`,
    `bc63f2c266a25b1451a74940615b4c784057c5224afdf4d73f9f9ab799a4f530`,
    `645ae507f1636740ac5cd065d7633ac1bfb58caa5513642cc7c944b216c4b177`, and
    `84dad849581fd2bf823dd87bc3a53d8868c73c33e9a4e9dac1584af2f04805b5`;
- `font-bh-lucidatypewriter-100dpi-1.0.4/lutRS10.bdf`
  - SHA-256: `0bbc345c3a5e681bc2d8ffaca1528b2b5994c1899ec2993eca760c3b977fe6d3`.

The proportional and typewriter faces cover the core-font XLFDs used by legacy
IRIX Motif applications. Release archives are available from
<https://www.x.org/releases/individual/font/>. The BDF inputs are not needed by
normal TinyX builds. To reproduce `dix/embedded-font-data.c`, extract the
releases and run:

```sh
fonts/generate-builtin-fonts.py \
  --fixed /path/to/font-misc-misc-1.1.3/6x13.bdf \
  --cursor /path/to/font-cursor-misc-1.0.4/cursor.bdf \
  --helvetica-regular-14 /path/to/font-adobe-100dpi-1.0.4/helvR10.bdf \
  --helvetica-bold-14 /path/to/font-adobe-100dpi-1.0.4/helvB10.bdf \
  --helvetica-bold-oblique-14 /path/to/font-adobe-100dpi-1.0.4/helvBO10.bdf \
  --helvetica-regular-20 /path/to/font-adobe-100dpi-1.0.4/helvR14.bdf \
  --lucida-typewriter-14 \
    /path/to/font-bh-lucidatypewriter-100dpi-1.0.4/lutRS10.bdf \
  --output dix/embedded-font-data.c
```

The generator checks the structure of every retained glyph. It keeps all 4,121
glyphs from the source 6x13 ISO10646-1 font in a sparse two-byte encoding, all
154 cursor glyphs, and the ISO-8859-1 ranges of the compatibility faces. The
full 6x13 name used by xterm is available alongside `fixed`, `6x13`, and
historical ISO8859-1 aliases. Generated bitmaps use compact MSB-first rows; the
runtime materializer converts them to the format requested by DIX.
