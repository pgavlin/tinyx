#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdio.h>
#include <string.h>

#include "embedded-font.h"

static int
validateFont(const TinyXDecodedFont *font)
{
    size_t i;

    if (!font || !font->canonicalName || !font->glyphs || !font->encoding ||
        !font->bitmap || font->firstRow > font->lastRow ||
        font->firstCol > font->lastCol)
        return 0;
    if (font->encodingCount !=
        (size_t)(font->lastRow - font->firstRow + 1) *
        (font->lastCol - font->firstCol + 1))
        return 0;
    for (i = 0; i < font->encodingCount; i++) {
        if (font->encoding[i] < -1 ||
            (font->encoding[i] >= 0 &&
             (size_t) font->encoding[i] >= font->glyphCount))
            return 0;
    }
    for (i = 0; i < font->glyphCount; i++) {
        const TinyXDecodedGlyph *glyph = &font->glyphs[i];
        size_t bytes;

        if (glyph->bitmapStride < (glyph->bitmapWidth + 7) / 8)
            return 0;
        bytes = (size_t) glyph->bitmapStride * glyph->bitmapHeight;
        if (glyph->bitmapOffset > font->bitmapSize ||
            bytes > font->bitmapSize - glyph->bitmapOffset)
            return 0;
    }
    return 1;
}

int
main(void)
{
    static const char *required[] = {
        "fixed",
        "cursor",
        "-adobe-helvetica-medium-r-normal--14-100-100-100-p-76-iso8859-1",
        "-adobe-helvetica-bold-r-normal--14-100-100-100-p-82-iso8859-1",
        "-adobe-helvetica-bold-o-normal--14-100-100-100-p-82-iso8859-1",
        "-adobe-helvetica-medium-r-normal--20-140-100-100-p-100-iso8859-1",
        "-b&h-lucidatypewriter-medium-r-normal-sans-14-100-100-100-m-80-iso8859-1"
    };
    size_t i, j;
    unsigned int found = 0;

    if (!validateFont(&tinyxEmbeddedFixedFont) ||
        !validateFont(&tinyxEmbeddedCursorFont)) {
        fprintf(stderr, "invalid embedded font data\n");
        return 1;
    }
    if (tinyxEmbeddedFixedFont.encodingCount != 65536 ||
        tinyxEmbeddedFixedFont.glyphCount <= 256 ||
        tinyxEmbeddedFixedFont.encoding[0x2500] < 0 ||
        tinyxEmbeddedCursorFont.encodingCount != 154 ||
        tinyxEmbeddedCursorFont.glyphCount != 154) {
        fprintf(stderr, "unexpected embedded font encoding range\n");
        return 1;
    }
    for (i = 0; i < tinyxEmbeddedFontNameCount; i++) {
        const TinyXEmbeddedFontName *entry = &tinyxEmbeddedFontNames[i];

        if (!validateFont(entry->font)) {
            fprintf(stderr, "invalid catalog font %s\n", entry->name);
            return 1;
        }
        for (j = 0; j < sizeof(required) / sizeof(required[0]); j++) {
            if (!strcmp(entry->name, required[j]))
                found |= 1U << j;
        }
    }
    if (found != (1U << (sizeof(required) / sizeof(required[0]))) - 1) {
        fprintf(stderr, "missing required embedded font names\n");
        return 1;
    }
    return 0;
}
