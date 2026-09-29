/* Internal decoded bitmap-font and catalog interfaces. */
#ifndef TINYX_EMBEDDED_FONT_H
#define TINYX_EMBEDDED_FONT_H

#include <stddef.h>
#include <stdint.h>

#include <X11/X.h>
#include <X11/fonts/font.h>
#include <X11/fonts/fontstruct.h>

typedef struct {
    int16_t leftSideBearing;
    int16_t rightSideBearing;
    int16_t characterWidth;
    int16_t ascent;
    int16_t descent;
    uint16_t attributes;
    uint32_t bitmapOffset;
    uint16_t bitmapStride;
    uint16_t bitmapWidth;
    uint16_t bitmapHeight;
} TinyXDecodedGlyph;

typedef struct {
    const char *name;
    const char *stringValue;
    int32_t integerValue;
} TinyXDecodedFontProperty;

typedef struct {
    const char *canonicalName;
    uint16_t firstCol;
    uint16_t lastCol;
    uint16_t firstRow;
    uint16_t lastRow;
    uint16_t defaultCh;
    int16_t fontAscent;
    int16_t fontDescent;
    const TinyXDecodedGlyph *glyphs;
    size_t glyphCount;
    const int16_t *encoding;
    size_t encodingCount;
    const uint8_t *bitmap;
    size_t bitmapSize;
    const TinyXDecodedFontProperty *properties;
    size_t propertyCount;
} TinyXDecodedFont;

typedef struct {
    const TinyXDecodedFont *font;
    void *owner;
    void (*release)(void *owner, const TinyXDecodedFont *font);
} TinyXDecodedFontLease;

typedef struct {
    const char *name;
    const TinyXDecodedFont *font;
} TinyXEmbeddedFontName;

extern const TinyXDecodedFont tinyxEmbeddedFixedFont;
extern const TinyXDecodedFont tinyxEmbeddedCursorFont;
extern const TinyXEmbeddedFontName tinyxEmbeddedFontNames[];
extern const size_t tinyxEmbeddedFontNameCount;

void TinyXRegisterEmbeddedFontFPE(void);

#endif
