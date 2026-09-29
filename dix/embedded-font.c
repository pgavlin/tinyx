/* Filesystem-free bitmap font catalog and DIX FPE adapter. */
#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <X11/X.h>
#include <X11/fonts/fsmasks.h>
#include <X11/fonts/font.h>
#include <X11/fonts/fontstruct.h>

#include "dix.h"
#include "dixfont.h"
#include "embedded-font.h"
#include "misc.h"
#include "os.h"
#include "servermd.h"

#define TINYX_EMBEDDED_FONT_PATH "built-ins"

typedef struct {
    TinyXDecodedFontLease lease;
    CharInfoRec *glyphs;
    unsigned char *bitmap;
    size_t glyphCount;
} TinyXFontInstance;

typedef struct _TinyXLfwiIterator {
    struct _TinyXLfwiIterator *next;
    void *client;
    char pattern[256];
    int patternLength;
    size_t nextName;
    int remaining;
} TinyXLfwiIterator;

typedef struct {
    int (*acquire)(void *userdata, const char *name, size_t length,
                   TinyXDecodedFontLease *lease);
    size_t (*nameCount)(void *userdata);
    int (*nameAt)(void *userdata, size_t index, const char **name,
                  const TinyXDecodedFont **font);
} TinyXFontCatalogOps;

typedef struct {
    const TinyXFontCatalogOps *ops;
    void *userdata;
} TinyXFontCatalog;

static TinyXLfwiIterator *lfwiIterators;
static xCharInfo missingMetric;

static int
asciiEqual(const char *left, size_t leftLength, const char *right)
{
    size_t i;

    if (strlen(right) != leftLength)
        return 0;
    for (i = 0; i < leftLength; i++) {
        if (tolower((unsigned char) left[i]) !=
            tolower((unsigned char) right[i]))
            return 0;
    }
    return 1;
}

static int
patternMatches(const char *pattern, size_t patternLength, const char *name)
{
    size_t p = 0, n = 0;
    size_t star = SIZE_MAX, retry = 0;

    while (name[n]) {
        if (p < patternLength && pattern[p] == '*') {
            star = p++;
            retry = n;
        }
        else if (p < patternLength &&
                 (pattern[p] == '?' ||
                  tolower((unsigned char) pattern[p]) ==
                  tolower((unsigned char) name[n]))) {
            p++;
            n++;
        }
        else if (star != SIZE_MAX) {
            p = star + 1;
            n = ++retry;
        }
        else {
            return 0;
        }
    }
    while (p < patternLength && pattern[p] == '*')
        p++;
    return p == patternLength;
}

static int
builtInCatalogAcquire(void *userdata, const char *name, size_t length,
                      TinyXDecodedFontLease *lease)
{
    size_t i;

    (void) userdata;
    memset(lease, 0, sizeof(*lease));
    for (i = 0; i < tinyxEmbeddedFontNameCount; i++) {
        if (asciiEqual(name, length, tinyxEmbeddedFontNames[i].name)) {
            lease->font = tinyxEmbeddedFontNames[i].font;
            return Successful;
        }
    }
    return BadFontName;
}

static size_t
builtInCatalogNameCount(void *userdata)
{
    (void) userdata;
    return tinyxEmbeddedFontNameCount;
}

static int
builtInCatalogNameAt(void *userdata, size_t index, const char **name,
                     const TinyXDecodedFont **font)
{
    (void) userdata;
    if (index >= tinyxEmbeddedFontNameCount)
        return BadFontName;
    *name = tinyxEmbeddedFontNames[index].name;
    *font = tinyxEmbeddedFontNames[index].font;
    return Successful;
}

static const TinyXFontCatalogOps builtInCatalogOps = {
    builtInCatalogAcquire,
    builtInCatalogNameCount,
    builtInCatalogNameAt
};

static TinyXFontCatalog activeCatalog = {
    &builtInCatalogOps,
    NULL
};

static int
catalogAcquire(const char *name, size_t length, TinyXDecodedFontLease *lease)
{
    return (*activeCatalog.ops->acquire)(activeCatalog.userdata, name, length,
                                         lease);
}

static size_t
catalogNameCount(void)
{
    return (*activeCatalog.ops->nameCount)(activeCatalog.userdata);
}

static int
catalogNameAt(size_t index, const char **name, const TinyXDecodedFont **font)
{
    return (*activeCatalog.ops->nameAt)(activeCatalog.userdata, index, name,
                                        font);
}

static void
catalogRelease(TinyXDecodedFontLease *lease)
{
    if (lease->release)
        (*lease->release)(lease->owner, lease->font);
    memset(lease, 0, sizeof(*lease));
}

static unsigned char
reverseByte(unsigned char value)
{
    value = (unsigned char) ((value >> 4) | (value << 4));
    value = (unsigned char) (((value & 0xcc) >> 2) |
                             ((value & 0x33) << 2));
    value = (unsigned char) (((value & 0xaa) >> 1) |
                             ((value & 0x55) << 1));
    return value;
}

static int
formatValue(fsBitmapFormat format, fsBitmapFormatMask mask,
            int *bitOrder, int *byteOrder, int *glyphPad, int *scanUnit)
{
    long value;

    *bitOrder = BITMAP_BIT_ORDER;
    *byteOrder = IMAGE_BYTE_ORDER;
    *glyphPad = GLYPHPADBYTES;
    *scanUnit = 1;

    if (mask & BitmapFormatMaskBit)
        *bitOrder = (format & BitmapFormatBitOrderMask) ? MSBFirst : LSBFirst;
    if (mask & BitmapFormatMaskByte)
        *byteOrder = (format & BitmapFormatByteOrderMask) ? MSBFirst : LSBFirst;
    if (mask & BitmapFormatMaskScanLinePad) {
        value = (format & BitmapFormatScanlinePadMask) >> 8;
        *glyphPad = 1 << value;
    }
    if (mask & BitmapFormatMaskScanLineUnit) {
        value = (format & BitmapFormatScanlineUnitMask) >> 12;
        *scanUnit = 1 << value;
    }
    if ((mask & BitmapFormatMaskImageRectangle) &&
        (format & BitmapFormatImageRectMask) != BitmapFormatImageRectMin)
        return BadFontFormat;
    if ((*glyphPad != 1 && *glyphPad != 2 && *glyphPad != 4 && *glyphPad != 8) ||
        (*scanUnit != 1 && *scanUnit != 2 && *scanUnit != 4 && *scanUnit != 8))
        return BadFontFormat;
    return Successful;
}

static int
makeProperties(const TinyXDecodedFont *source, FontInfoPtr info)
{
    size_t i;

    info->nprops = (int) source->propertyCount;
    if (!source->propertyCount)
        return Successful;
    if (source->propertyCount > INT_MAX / sizeof(*info->props))
        return AllocError;
    info->props = calloc(source->propertyCount, sizeof(*info->props));
    info->isStringProp = calloc(source->propertyCount, sizeof(*info->isStringProp));
    if (!info->props || !info->isStringProp)
        goto fail;
    for (i = 0; i < source->propertyCount; i++) {
        const TinyXDecodedFontProperty *property = &source->properties[i];

        info->props[i].name = MakeAtom((char *) property->name,
                                       strlen(property->name), TRUE);
        if (property->stringValue) {
            info->props[i].value = MakeAtom((char *) property->stringValue,
                                             strlen(property->stringValue), TRUE);
            info->isStringProp[i] = TRUE;
        }
        else {
            info->props[i].value = property->integerValue;
        }
        if (!info->props[i].name ||
            (property->stringValue && !info->props[i].value))
            goto fail;
    }
    return Successful;

fail:
    free(info->props);
    free(info->isStringProp);
    info->props = NULL;
    info->isStringProp = NULL;
    info->nprops = 0;
    return AllocError;
}

static void
computeBounds(const TinyXDecodedFont *source, FontInfoPtr info)
{
    size_t i;
    int first = 1;
    int constantMetrics = 1;
    int constantWidth = 1;
    int allExist = 1;
    int maxOverlap = SHRT_MIN;
    xCharInfo firstMetrics;

    memset(&firstMetrics, 0, sizeof(firstMetrics));
    memset(&info->minbounds, 0, sizeof(info->minbounds));
    memset(&info->maxbounds, 0, sizeof(info->maxbounds));
    for (i = 0; i < source->glyphCount; i++) {
        const TinyXDecodedGlyph *glyph = &source->glyphs[i];
        xCharInfo metrics;
        int overlap;

        metrics.leftSideBearing = glyph->leftSideBearing;
        metrics.rightSideBearing = glyph->rightSideBearing;
        metrics.characterWidth = glyph->characterWidth;
        metrics.ascent = glyph->ascent;
        metrics.descent = glyph->descent;
        metrics.attributes = glyph->attributes;
        if (first) {
            info->minbounds = info->maxbounds = firstMetrics = metrics;
            first = 0;
        }
        else {
#define UPDATE_BOUND(field) do { \
            if (metrics.field < info->minbounds.field) \
                info->minbounds.field = metrics.field; \
            if (metrics.field > info->maxbounds.field) \
                info->maxbounds.field = metrics.field; \
        } while (0)
            UPDATE_BOUND(leftSideBearing);
            UPDATE_BOUND(rightSideBearing);
            UPDATE_BOUND(characterWidth);
            UPDATE_BOUND(ascent);
            UPDATE_BOUND(descent);
#undef UPDATE_BOUND
            info->minbounds.attributes &= metrics.attributes;
            info->maxbounds.attributes |= metrics.attributes;
            if (memcmp(&metrics, &firstMetrics, sizeof(metrics)) != 0)
                constantMetrics = 0;
            if (metrics.characterWidth != firstMetrics.characterWidth)
                constantWidth = 0;
        }
        overlap = metrics.rightSideBearing - metrics.characterWidth;
        if (overlap > maxOverlap)
            maxOverlap = overlap;
    }
    for (i = 0; i < source->encodingCount; i++) {
        if (source->encoding[i] < 0) {
            allExist = 0;
            break;
        }
    }
    info->ink_minbounds = info->minbounds;
    info->ink_maxbounds = info->maxbounds;
    info->drawDirection = LeftToRight;
    info->fontAscent = source->fontAscent;
    info->fontDescent = source->fontDescent;
    info->firstCol = source->firstCol;
    info->lastCol = source->lastCol;
    info->firstRow = source->firstRow;
    info->lastRow = source->lastRow;
    info->defaultCh = source->defaultCh;
    info->allExist = allExist;
    info->constantMetrics = constantMetrics;
    info->constantWidth = constantWidth;
    info->terminalFont = constantMetrics;
    info->inkInside = TRUE;
    info->cachable = TRUE;
    info->maxOverlap = maxOverlap == SHRT_MIN ? 0 : maxOverlap;
    info->noOverlap = info->maxOverlap <= info->minbounds.leftSideBearing;
}

static int
copyFontInfo(const TinyXDecodedFont *source, FontInfoPtr info)
{
    memset(info, 0, sizeof(*info));
    computeBounds(source, info);
    return makeProperties(source, info);
}

static int
lookupGlyphIndex(const TinyXDecodedFont *source, unsigned char **chars,
                 FontEncoding encoding)
{
    unsigned int row, col;
    size_t index;

    switch (encoding) {
    case Linear8Bit:
    case TwoD8Bit:
        row = 0;
        col = *(*chars)++;
        if (source->firstRow > 0)
            return -1;
        break;
    case Linear16Bit:
        row = 0;
        col = (unsigned int) (*(*chars)++) << 8;
        col |= *(*chars)++;
        break;
    case TwoD16Bit:
        row = *(*chars)++;
        col = *(*chars)++;
        break;
    default:
        return -1;
    }
    if (row < source->firstRow || row > source->lastRow ||
        col < source->firstCol || col > source->lastCol)
        return -1;
    index = (row - source->firstRow) *
            (source->lastCol - source->firstCol + 1) +
            (col - source->firstCol);
    if (index >= source->encodingCount)
        return -1;
    return source->encoding[index];
}

static int
embeddedGetGlyphs(FontPtr font, unsigned long count, unsigned char *chars,
                  FontEncoding encoding, unsigned long *glyphCount,
                  CharInfoPtr *glyphs)
{
    TinyXFontInstance *instance = font->fontPrivate;
    const TinyXDecodedFont *source = instance->lease.font;
    int defaultIndex = -1;

    if (source->defaultCh >= source->firstCol &&
        source->defaultCh <= source->lastCol)
        defaultIndex = source->encoding[source->defaultCh - source->firstCol];
    *glyphCount = 0;
    while (count--) {
        int index = lookupGlyphIndex(source, &chars, encoding);

        if (index < 0)
            index = defaultIndex;
        if (index >= 0 && (size_t) index < instance->glyphCount)
            glyphs[(*glyphCount)++] = &instance->glyphs[index];
    }
    return Successful;
}

static int
embeddedGetMetrics(FontPtr font, unsigned long count, unsigned char *chars,
                   FontEncoding encoding, unsigned long *glyphCount,
                   xCharInfo **metrics)
{
    TinyXFontInstance *instance = font->fontPrivate;
    const TinyXDecodedFont *source = instance->lease.font;

    *glyphCount = 0;
    while (count--) {
        int index = lookupGlyphIndex(source, &chars, encoding);

        if (index >= 0 && (size_t) index < instance->glyphCount)
            metrics[(*glyphCount)++] = &instance->glyphs[index].metrics;
        else
            metrics[(*glyphCount)++] = &missingMetric;
    }
    return Successful;
}

static void
freeMaterializedFont(FontPtr font)
{
    TinyXFontInstance *instance;

    if (!font)
        return;
    instance = font->fontPrivate;
    free(font->info.props);
    free(font->info.isStringProp);
    if (instance) {
        catalogRelease(&instance->lease);
        free(instance->glyphs);
        free(instance->bitmap);
        free(instance);
    }
    DestroyFontRec(font);
}

static int
materializeFont(TinyXDecodedFontLease *lease, fsBitmapFormat format,
                fsBitmapFormatMask formatMask, FontPtr *result)
{
    const TinyXDecodedFont *source = lease->font;
    TinyXFontInstance *instance = NULL;
    FontPtr font = NULL;
    size_t total = 0;
    size_t i;
    int bitOrder, byteOrder, glyphPad, scanUnit;
    int error;

    *result = NULL;
    error = formatValue(format, formatMask, &bitOrder, &byteOrder,
                        &glyphPad, &scanUnit);
    if (error != Successful)
        return error;
    if (!source || source->glyphCount > SIZE_MAX / sizeof(CharInfoRec))
        return BadFontFormat;

    instance = calloc(1, sizeof(*instance));
    font = CreateFontRec();
    if (!instance || !font)
        goto alloc_fail;
    instance->lease = *lease;
    memset(lease, 0, sizeof(*lease));
    instance->glyphCount = source->glyphCount;
    instance->glyphs = calloc(source->glyphCount, sizeof(*instance->glyphs));
    if (source->glyphCount && !instance->glyphs)
        goto alloc_fail;

    for (i = 0; i < source->glyphCount; i++) {
        const TinyXDecodedGlyph *glyph = &source->glyphs[i];
        size_t rowBytes = (glyph->bitmapWidth + 7) / 8;
        size_t stride = (rowBytes + glyphPad - 1) & ~(size_t)(glyphPad - 1);
        size_t bytes;

        if (stride % scanUnit != 0 || glyph->bitmapStride < rowBytes ||
            glyph->bitmapOffset > source->bitmapSize ||
            glyph->bitmapHeight > SIZE_MAX / glyph->bitmapStride ||
            glyph->bitmapOffset +
                (size_t) glyph->bitmapHeight * glyph->bitmapStride >
                source->bitmapSize ||
            glyph->bitmapHeight > SIZE_MAX / stride)
            goto format_fail;
        bytes = (size_t) glyph->bitmapHeight * stride;
        if (bytes > SIZE_MAX - total)
            goto alloc_fail;
        total += bytes;
    }
    instance->bitmap = calloc(total ? total : 1, 1);
    if (!instance->bitmap)
        goto alloc_fail;

    total = 0;
    for (i = 0; i < source->glyphCount; i++) {
        const TinyXDecodedGlyph *glyph = &source->glyphs[i];
        CharInfoPtr target = &instance->glyphs[i];
        size_t rowBytes = (glyph->bitmapWidth + 7) / 8;
        size_t stride = (rowBytes + glyphPad - 1) & ~(size_t)(glyphPad - 1);
        size_t row;

        target->metrics.leftSideBearing = glyph->leftSideBearing;
        target->metrics.rightSideBearing = glyph->rightSideBearing;
        target->metrics.characterWidth = glyph->characterWidth;
        target->metrics.ascent = glyph->ascent;
        target->metrics.descent = glyph->descent;
        target->metrics.attributes = glyph->attributes;
        target->bits = (char *) instance->bitmap + total;
        for (row = 0; row < glyph->bitmapHeight; row++) {
            const unsigned char *input = source->bitmap + glyph->bitmapOffset +
                                         row * glyph->bitmapStride;
            unsigned char *output = instance->bitmap + total + row * stride;
            size_t column;

            for (column = 0; column < rowBytes; column++)
                output[column] = bitOrder == MSBFirst ?
                                 input[column] : reverseByte(input[column]);
            if (scanUnit > 1 && byteOrder != bitOrder) {
                for (column = 0; column < stride; column += scanUnit) {
                    size_t left = column, right = column + scanUnit - 1;
                    while (left < right) {
                        unsigned char temporary = output[left];
                        output[left++] = output[right];
                        output[right--] = temporary;
                    }
                }
            }
        }
        total += (size_t) glyph->bitmapHeight * stride;
    }

    if (copyFontInfo(source, &font->info) != Successful)
        goto alloc_fail;
    font->fontPrivate = instance;
    font->get_glyphs = embeddedGetGlyphs;
    font->get_metrics = embeddedGetMetrics;
    font->unload_font = freeMaterializedFont;
    font->unload_glyphs = NULL;
    font->bit = bitOrder;
    font->byte = byteOrder;
    font->glyph = glyphPad;
    font->scan = scanUnit;
    font->format = format;
    *result = font;
    return Successful;

format_fail:
    error = BadFontFormat;
    goto fail;
alloc_fail:
    error = AllocError;
fail:
    if (font) {
        font->fontPrivate = instance;
        freeMaterializedFont(font);
    }
    else if (instance) {
        catalogRelease(&instance->lease);
        free(instance->glyphs);
        free(instance->bitmap);
        free(instance);
    }
    return error;
}

static Bool
embeddedNameCheck(const char *name)
{
    return strcmp(name, TINYX_EMBEDDED_FONT_PATH) == 0;
}

static int
embeddedInitFpe(FontPathElementPtr fpe)
{
    fpe->private = NULL;
    return Successful;
}

static int
embeddedResetFpe(FontPathElementPtr fpe)
{
    (void) fpe;
    return Successful;
}

static int
embeddedFreeFpe(FontPathElementPtr fpe)
{
    (void) fpe;
    while (lfwiIterators) {
        TinyXLfwiIterator *iterator = lfwiIterators;
        lfwiIterators = iterator->next;
        free(iterator);
    }
    return Successful;
}

static int
embeddedOpenFont(void *client, FontPathElementPtr fpe, Mask flags,
                 const char *name, int nameLength, fsBitmapFormat format,
                 fsBitmapFormatMask formatMask, XID id, FontPtr *font,
                 char **aliasName, FontPtr nonCachableFont)
{
    TinyXDecodedFontLease lease;
    int error;

    (void) client;
    (void) fpe;
    (void) flags;
    (void) id;
    (void) nonCachableFont;
    *font = NULL;
    *aliasName = NULL;
    error = catalogAcquire(name, nameLength, &lease);
    if (error != Successful)
        return error;
    error = materializeFont(&lease, format, formatMask, font);
    catalogRelease(&lease);
    return error;
}

static void
embeddedCloseFont(FontPathElementPtr fpe, FontPtr font)
{
    (void) fpe;
    freeMaterializedFont(font);
}

static int
embeddedListFonts(void *client, FontPathElementPtr fpe, const char *pattern,
                  int patternLength, int maxNames, FontNamesPtr names)
{
    size_t i;

    (void) client;
    (void) fpe;
    for (i = 0; i < catalogNameCount() && maxNames > 0; i++) {
        const TinyXDecodedFont *font;
        const char *name;
        int error = catalogNameAt(i, &name, &font);

        (void) font;
        if (error != Successful)
            return error;
        if (!patternMatches(pattern, patternLength, name))
            continue;
        error = AddFontNamesName(names, (char *) name, strlen(name));
        if (error != Successful)
            return error;
        maxNames--;
    }
    return Successful;
}

static void
unlinkIterator(TinyXLfwiIterator *iterator)
{
    TinyXLfwiIterator **link = &lfwiIterators;

    while (*link && *link != iterator)
        link = &(*link)->next;
    if (*link)
        *link = iterator->next;
}

static int
embeddedStartLfwi(void *client, FontPathElementPtr fpe, const char *pattern,
                  int patternLength, int maxNames, void **private)
{
    TinyXLfwiIterator *iterator;

    (void) fpe;
    if (patternLength < 0 || patternLength > 255)
        return BadFontName;
    iterator = calloc(1, sizeof(*iterator));
    if (!iterator)
        return AllocError;
    iterator->client = client;
    memcpy(iterator->pattern, pattern, patternLength);
    iterator->patternLength = patternLength;
    iterator->remaining = maxNames;
    iterator->next = lfwiIterators;
    lfwiIterators = iterator;
    *private = iterator;
    return Successful;
}

static int
embeddedNextLfwi(void *client, FontPathElementPtr fpe, char **name,
                 int *nameLength, FontInfoPtr *info, int *numFonts,
                 void *private)
{
    TinyXLfwiIterator *iterator = private;

    (void) client;
    (void) fpe;
    while (iterator->remaining > 0 &&
           iterator->nextName < catalogNameCount()) {
        const TinyXDecodedFont *font;
        const char *entryName;
        int error = catalogNameAt(iterator->nextName++, &entryName, &font);

        if (error != Successful) {
            unlinkIterator(iterator);
            free(iterator);
            return error;
        }
        if (!patternMatches(iterator->pattern, iterator->patternLength,
                            entryName))
            continue;
        error = copyFontInfo(font, *info);
        if (error != Successful) {
            unlinkIterator(iterator);
            free(iterator);
            return error;
        }
        *name = (char *) entryName;
        *nameLength = strlen(entryName);
        iterator->remaining--;
        *numFonts = iterator->remaining;
        return Successful;
    }
    unlinkIterator(iterator);
    free(iterator);
    return BadFontName;
}

static int
embeddedWakeup(FontPathElementPtr fpe, unsigned long *mask)
{
    (void) fpe;
    (void) mask;
    return Successful;
}

static void
embeddedClientDied(void *client, FontPathElementPtr fpe)
{
    TinyXLfwiIterator **link = &lfwiIterators;

    (void) fpe;
    while (*link) {
        TinyXLfwiIterator *iterator = *link;
        if (iterator->client == client) {
            *link = iterator->next;
            free(iterator);
        }
        else {
            link = &iterator->next;
        }
    }
}

static int
embeddedLoadGlyphs(void *client, FontPtr font, Bool range, unsigned int count,
                   int itemSize, unsigned char *data)
{
    (void) client;
    (void) font;
    (void) range;
    (void) count;
    (void) itemSize;
    (void) data;
    return Successful;
}

void
TinyXRegisterEmbeddedFontFPE(void)
{
    int type = RegisterFPEFunctions(embeddedNameCheck,
                                    embeddedInitFpe,
                                    embeddedFreeFpe,
                                    embeddedResetFpe,
                                    embeddedOpenFont,
                                    embeddedCloseFont,
                                    embeddedListFonts,
                                    embeddedStartLfwi,
                                    embeddedNextLfwi,
                                    embeddedWakeup,
                                    embeddedClientDied,
                                    embeddedLoadGlyphs,
                                    NULL, NULL, NULL);
    if (type < 0)
        FatalError("could not register embedded font backend");
}
