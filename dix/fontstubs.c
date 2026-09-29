/*
 * Build-only replacements for symbols historically supplied by libXfont.
 *
 * This file deliberately does not register a font backend.  It exists so a
 * --disable-fonts build can compile and link while the host-independent font
 * implementation is deferred.  Such a server cannot complete startup because
 * the required default text and cursor fonts cannot be opened.
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <X11/X.h>
#include <X11/fonts/font.h>
#include <X11/fonts/fontstruct.h>

#include "dixfont.h"

struct _FontPatternCache {
    int unused;
};

int glyphCachingMode = CACHING_OFF;

void
FontFileRegisterFpeFunctions(void)
{
}

void
FontFileCheckRegisterFpeFunctions(void)
{
}

/* libXfont 1 exposed this for KDrive, but modern font headers do not. */
void
BuiltinRegisterFpeFunctions(void)
{
}

FontNamesPtr
MakeFontNamesRecord(unsigned size)
{
    FontNamesPtr names = calloc(1, sizeof(FontNamesRec));

    if (!names)
        return NULL;
    if (size == 0)
        return names;

    names->length = calloc(size, sizeof(*names->length));
    names->names = calloc(size, sizeof(*names->names));
    if (!names->length || !names->names) {
        FreeFontNames(names);
        return NULL;
    }
    names->size = size;
    return names;
}

void
FreeFontNames(FontNamesPtr names)
{
    int i;

    if (!names)
        return;
    for (i = 0; i < names->nnames; i++)
        free(names->names[i]);
    free(names->names);
    free(names->length);
    free(names);
}

int
AddFontNamesName(FontNamesPtr names, char *name, int length)
{
    char **newNames;
    int *newLengths;
    int newSize;

    if (names->nnames == names->size) {
        newSize = names->size ? names->size * 2 : 8;
        newNames = realloc(names->names, newSize * sizeof(*newNames));
        if (!newNames)
            return AllocError;
        names->names = newNames;

        newLengths = realloc(names->length, newSize * sizeof(*newLengths));
        if (!newLengths)
            return AllocError;
        names->length = newLengths;
        names->size = newSize;
    }

    names->names[names->nnames] = malloc(length + 1);
    if (!names->names[names->nnames])
        return AllocError;
    memcpy(names->names[names->nnames], name, length);
    names->names[names->nnames][length] = '\0';
    names->length[names->nnames] = length;
    names->nnames++;
    return Successful;
}

FontPatternCachePtr
MakeFontPatternCache(void)
{
    return calloc(1, sizeof(struct _FontPatternCache));
}

void
FreeFontPatternCache(FontPatternCachePtr cache)
{
    free(cache);
}

void
EmptyFontPatternCache(FontPatternCachePtr cache)
{
    (void) cache;
}

void
CacheFontPattern(FontPatternCachePtr cache, const char *pattern, int patlen,
                 FontPtr font)
{
    (void) cache;
    (void) pattern;
    (void) patlen;
    (void) font;
}

FontPtr
FindCachedFontPattern(FontPatternCachePtr cache, const char *pattern,
                      int patlen)
{
    (void) cache;
    (void) pattern;
    (void) patlen;
    return NULL;
}

void
RemoveCachedFontPattern(FontPatternCachePtr cache, FontPtr font)
{
    (void) cache;
    (void) font;
}

void
QueryGlyphExtents(FontPtr font, CharInfoPtr *charinfo, unsigned long count,
                  ExtentInfoPtr info)
{
    unsigned long i;
    int width = 0;

    memset(info, 0, sizeof(*info));
    info->drawDirection = font ? font->info.drawDirection : LeftToRight;
    info->fontAscent = font ? font->info.fontAscent : 0;
    info->fontDescent = font ? font->info.fontDescent : 0;

    for (i = 0; i < count; i++) {
        xCharInfo *metrics = &charinfo[i]->metrics;
        int left = width + metrics->leftSideBearing;
        int right = width + metrics->rightSideBearing;

        if (i == 0 || left < info->overallLeft)
            info->overallLeft = left;
        if (i == 0 || right > info->overallRight)
            info->overallRight = right;
        if (metrics->ascent > info->overallAscent)
            info->overallAscent = metrics->ascent;
        if (metrics->descent > info->overallDescent)
            info->overallDescent = metrics->descent;
        width += metrics->characterWidth;
    }
    info->overallWidth = width;
}

Bool
QueryTextExtents(FontPtr font, unsigned long count, unsigned char *chars,
                 ExtentInfoPtr info)
{
    xCharInfo **metrics;
    unsigned long metricCount = 0;
    int result;

    if (!font || !font->get_metrics)
        return FALSE;

    metrics = count ? malloc(count * sizeof(*metrics)) : NULL;
    if (count && !metrics)
        return FALSE;

    result = (*font->get_metrics) (font, count, chars, Linear8Bit,
                                   &metricCount, metrics);
    if (result == Successful)
        QueryGlyphExtents(font, (CharInfoPtr *) metrics, metricCount, info);
    free(metrics);
    return result == Successful;
}

Bool
ParseGlyphCachingMode(char *mode)
{
    (void) mode;
    glyphCachingMode = CACHING_OFF;
    return TRUE;
}

void
InitGlyphCaching(void)
{
    glyphCachingMode = CACHING_OFF;
}

void
SetGlyphCachingMode(int mode)
{
    glyphCachingMode = mode;
}

static int nextFontPrivateIndex;

int
AllocateFontPrivateIndex(void)
{
    return nextFontPrivateIndex++;
}

void
ResetFontPrivateIndex(void)
{
    nextFontPrivateIndex = 0;
}

Bool
_FontSetNewPrivate(FontPtr font, int index, void *value)
{
    void **newPrivates;
    int i;

    newPrivates = realloc(font->devPrivates,
                          (index + 1) * sizeof(*newPrivates));
    if (!newPrivates)
        return FALSE;
    for (i = font->maxPrivate + 1; i <= index; i++)
        newPrivates[i] = NULL;
    font->devPrivates = newPrivates;
    font->maxPrivate = index;
    font->devPrivates[index] = value;
    return TRUE;
}

FontPtr
CreateFontRec(void)
{
    FontPtr font = calloc(1, sizeof(FontRec));

    if (font)
        font->maxPrivate = -1;
    return font;
}

void
DestroyFontRec(FontPtr font)
{
    if (!font)
        return;
    free(font->devPrivates);
    free(font);
}
