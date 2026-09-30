#ifdef HAVE_CONFIG_H
#include <kdrive-config.h>
#endif

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "tinyx-display.h"
#include "damage.h"
#include "micmap.h"
#include "resource.h"

typedef struct {
    CARD8 *pixels;
    size_t size;
    Bool ownsPixels;
    Bool screenInitialized;
    DamagePtr damage;
    PixmapPtr screenPixmap;
    ScreenPtr screen;
    KdScreenInfo *screenInfo;
    KdPixmapFormat *formats;
} TinyXMemoryPriv;

static TinyXMemoryDisplayConfig memoryConfig;
static Bool memoryConfigured;
static TinyXMemoryPriv *activeMemory;

static int
TinyXMemoryValidateConfig(const TinyXMemoryDisplayConfig *config,
                          size_t *stride, size_t *size)
{
    size_t minimumStride;

    if (!config || !config->width || !config->height)
        return 0;
    if (config->width > 32767 || config->height > 32767 ||
        config->widthMM > 32767 || config->heightMM > 32767 ||
        config->width > (uint32_t)(SIZE_MAX / 4))
        return 0;

    minimumStride = (size_t)config->width * 4;
    *stride = config->strideBytes ? config->strideBytes : minimumStride;
    if (*stride < minimumStride || *stride > INT_MAX || (*stride & 3) != 0)
        return 0;
    if (config->height > SIZE_MAX / *stride)
        return 0;

    *size = *stride * config->height;
    if (config->pixels && config->pixelsSize < *size)
        return 0;
    return 1;
}

int
TinyXMemoryDisplayConfigure(const TinyXMemoryDisplayConfig *config)
{
    size_t stride, size;

    if (activeMemory || !TinyXMemoryValidateConfig(config, &stride, &size))
        return 0;

    memoryConfig = *config;
    memoryConfig.strideBytes = stride;
    memoryConfigured = TRUE;
    return 1;
}

int
TinyXMemoryDisplayIsConfigured(void)
{
    return memoryConfigured;
}

int
TinyXMemoryDisplayGetFramebuffer(TinyXMemoryFramebufferInfo *info)
{
    if (!info || !activeMemory || !activeMemory->pixels)
        return 0;

    info->pixels = activeMemory->pixels;
    info->width = memoryConfig.width;
    info->height = memoryConfig.height;
    info->strideBytes = memoryConfig.strideBytes;
    info->depth = TINYX_MEMORY_DISPLAY_DEPTH;
    info->bitsPerPixel = TINYX_MEMORY_DISPLAY_BITS_PER_PIXEL;
    info->redMask = TINYX_MEMORY_DISPLAY_RED_MASK;
    info->greenMask = TINYX_MEMORY_DISPLAY_GREEN_MASK;
    info->blueMask = TINYX_MEMORY_DISPLAY_BLUE_MASK;
    return 1;
}

size_t
TinyXMemoryDisplayTakeDamage(TinyXDamageRect *rects, size_t capacity)
{
    RegionPtr region;
    BoxPtr boxes;
    size_t count, i;

    if (!activeMemory || !activeMemory->damage)
        return 0;

    region = DamageRegion(activeMemory->damage);
    count = REGION_NUM_RECTS(region);
    if (!count || !capacity)
        return count;
    if (!rects)
        return 0;

    boxes = REGION_RECTS(region);
    if (count > capacity) {
        BoxPtr box = REGION_EXTENTS(region);

        rects[0].x = box->x1;
        rects[0].y = box->y1;
        rects[0].width = box->x2 - box->x1;
        rects[0].height = box->y2 - box->y1;
        count = 1;
    }
    else {
        for (i = 0; i < count; i++) {
            rects[i].x = boxes[i].x1;
            rects[i].y = boxes[i].y1;
            rects[i].width = boxes[i].x2 - boxes[i].x1;
            rects[i].height = boxes[i].y2 - boxes[i].y1;
        }
    }

    DamageEmpty(activeMemory->damage);
    return count;
}

static Bool
TinyXMemoryCardInit(KdCardInfo *card)
{
    TinyXMemoryPriv *priv;
    size_t stride, size;

    if (!memoryConfigured ||
        !TinyXMemoryValidateConfig(&memoryConfig, &stride, &size)) {
        ErrorF("memory display is not configured\n");
        return FALSE;
    }

    priv = calloc(1, sizeof(*priv));
    if (!priv)
        return FALSE;

    if (memoryConfig.pixels) {
        priv->pixels = memoryConfig.pixels;
    }
    else {
        priv->pixels = calloc(1, size);
        if (!priv->pixels) {
            free(priv);
            return FALSE;
        }
        priv->ownsPixels = TRUE;
    }
    priv->size = size;
    card->driver = priv;
    activeMemory = priv;
    return TRUE;
}

static int
TinyXMemoryMaskShift(Pixel mask)
{
    int shift = 0;

    if (!mask)
        return 0;
    while (!(mask & 1)) {
        shift++;
        mask >>= 1;
    }
    return shift;
}

static Bool
TinyXMemoryInitVisuals(VisualPtr *visualOut, DepthPtr *depthOut,
                       int *visualCountOut, int *depthCountOut,
                       int *rootDepthOut, VisualID *rootVisualOut,
                       unsigned long sizes, int bitsPerRGB,
                       int preferredVisual)
{
    VisualPtr visuals = NULL;
    DepthPtr depths = NULL;
    VisualPtr visual;
    size_t visualCount = 0;
    size_t d, v;

    (void)sizes;
    (void)bitsPerRGB;
    (void)preferredVisual;

    for (d = 0; d < memoryConfig.depthCount; d++)
        visualCount += memoryConfig.depths[d].visualCount;
    depths = calloc(memoryConfig.depthCount, sizeof(*depths));
    visuals = calloc(visualCount, sizeof(*visuals));
    if (!depths || !visuals)
        goto fail;

    visual = visuals;
    for (d = 0; d < memoryConfig.depthCount; d++) {
        const TinyXMemoryDepthConfig *sourceDepth = &memoryConfig.depths[d];

        depths[d].depth = sourceDepth->depth;
        depths[d].numVids = sourceDepth->visualCount;
        if (sourceDepth->visualCount) {
            depths[d].vids = calloc(sourceDepth->visualCount,
                                    sizeof(*depths[d].vids));
            if (!depths[d].vids)
                goto fail;
        }
        for (v = 0; v < sourceDepth->visualCount; v++, visual++) {
            const TinyXMemoryVisualConfig *source = &sourceDepth->visuals[v];

            visual->class = source->visualClass;
            visual->bitsPerRGBValue = source->bitsPerRGB;
            visual->ColormapEntries = source->colormapEntries;
            visual->nplanes = sourceDepth->depth;
            visual->vid = FakeClientID(0);
            visual->redMask = source->redMask;
            visual->greenMask = source->greenMask;
            visual->blueMask = source->blueMask;
            visual->offsetRed = TinyXMemoryMaskShift(source->redMask);
            visual->offsetGreen = TinyXMemoryMaskShift(source->greenMask);
            visual->offsetBlue = TinyXMemoryMaskShift(source->blueMask);
            depths[d].vids[v] = visual->vid;
            if (d == memoryConfig.rootDepthIndex &&
                v == memoryConfig.rootVisualIndex) {
                *rootDepthOut = sourceDepth->depth;
                *rootVisualOut = visual->vid;
            }
        }
    }

    *visualOut = visuals;
    *depthOut = depths;
    *visualCountOut = visualCount;
    *depthCountOut = memoryConfig.depthCount;
    miClearVisualTypes();
    miResetInitVisuals();
    return TRUE;

fail:
    if (depths) {
        for (d = 0; d < memoryConfig.depthCount; d++)
            free(depths[d].vids);
    }
    free(depths);
    free(visuals);
    miClearVisualTypes();
    miResetInitVisuals();
    return FALSE;
}

static Bool
TinyXMemoryScreenInit(KdScreenInfo *screen)
{
    TinyXMemoryPriv *priv = screen->card->driver;
    size_t i;

    if (priv->screenInitialized) {
        ErrorF("memory display supports exactly one screen\n");
        return FALSE;
    }
    priv->screenInitialized = TRUE;

    screen->width = memoryConfig.width;
    screen->height = memoryConfig.height;
    screen->width_mm = memoryConfig.widthMM;
    screen->height_mm = memoryConfig.heightMM;
    screen->fb.depth = TINYX_MEMORY_DISPLAY_DEPTH;
    screen->fb.bitsPerPixel = TINYX_MEMORY_DISPLAY_BITS_PER_PIXEL;
    screen->fb.pixelStride = memoryConfig.strideBytes / 4;
    screen->fb.byteStride = memoryConfig.strideBytes;
    screen->fb.frameBuffer = priv->pixels;
    screen->fb.visuals = 1 << TrueColor;
    screen->fb.redMask = TINYX_MEMORY_DISPLAY_RED_MASK;
    screen->fb.greenMask = TINYX_MEMORY_DISPLAY_GREEN_MASK;
    screen->fb.blueMask = TINYX_MEMORY_DISPLAY_BLUE_MASK;
    if (memoryConfig.depthCount) {
        if (memoryConfig.depthCount > INT_MAX / sizeof(*priv->formats))
            return FALSE;
        priv->formats = calloc(memoryConfig.depthCount, sizeof(*priv->formats));
        if (!priv->formats)
            return FALSE;
        for (i = 0; i < memoryConfig.depthCount; i++) {
            priv->formats[i].depth = memoryConfig.depths[i].depth;
            priv->formats[i].bitsPerPixel =
                memoryConfig.depths[i].bitsPerPixel;
        }
        screen->fb.pixmapFormats = priv->formats;
        screen->fb.numPixmapFormats = memoryConfig.depthCount;
        miInitVisualsProc = TinyXMemoryInitVisuals;
    }
    screen->memory_base = priv->pixels;
    screen->memory_size = priv->size;
    screen->off_screen_base = priv->size;
    screen->dumb = TRUE;
    screen->softCursor = TRUE;
    return TRUE;
}

static Bool
TinyXMemoryInitScreen(ScreenPtr pScreen)
{
    pScreen->CreateColormap = fbInitializeColormap;
    return TRUE;
}

static Bool
TinyXMemoryRandRGetInfo(ScreenPtr pScreen, Rotation *rotations)
{
    (void)pScreen;
    *rotations = RR_Rotate_0;
    return TRUE;
}

static CARD32
TinyXMemoryScaleMillimeters(int oldPixels, int oldMillimeters, int newPixels)
{
    uint64_t value;

    if (oldPixels <= 0 || oldMillimeters <= 0)
        return 1;
    value = ((uint64_t)newPixels * (uint64_t)oldMillimeters +
             (uint64_t)oldPixels / 2) / (uint64_t)oldPixels;
    if (!value)
        value = 1;
    if (value > INT_MAX)
        value = INT_MAX;
    return (CARD32)value;
}

static TinyXMemoryDisplayResizeResult
TinyXMemoryResize(const TinyXMemoryDisplayConfig *config,
                  CARD32 mmWidth, CARD32 mmHeight)
{
    TinyXMemoryPriv *priv = activeMemory;
    TinyXMemoryDisplayConfig replacement;
    KdScreenInfo *screen;
    ScreenPtr pScreen;
    PixmapPtr pixmap;
    CARD8 *pixels;
    CARD8 *oldPixels;
    size_t stride, size;
    Bool ownsPixels;
    Bool oldOwnsPixels;
    Bool wasEnabled;
    BoxRec box;
    RegionRec region;
    KdMouseMatrix matrix;

    if (!priv || !priv->screen || !priv->screenInfo ||
        !TinyXMemoryValidateConfig(config, &stride, &size))
        return TINYX_MEMORY_DISPLAY_RESIZE_INVALID;

    if (config->pixels) {
        pixels = config->pixels;
        ownsPixels = FALSE;
        if (pixels == priv->pixels && priv->ownsPixels)
            return TINYX_MEMORY_DISPLAY_RESIZE_INVALID;
    }
    else {
        pixels = calloc(1, size);
        if (!pixels)
            return TINYX_MEMORY_DISPLAY_RESIZE_NO_MEMORY;
        ownsPixels = TRUE;
    }

    replacement = *config;
    replacement.widthMM = mmWidth;
    replacement.heightMM = mmHeight;
    replacement.strideBytes = stride;
    screen = priv->screenInfo;
    pScreen = priv->screen;
    pixmap = priv->screenPixmap;
    oldPixels = priv->pixels;
    oldOwnsPixels = priv->ownsPixels;
    wasEnabled = KdGetScreenPriv(pScreen)->enabled;

    if (wasEnabled)
        KdDisableScreen(pScreen);

    if (!ownsPixels)
        memset(pixels, 0, size);
    DamageEmpty(priv->damage);

    screen->width = replacement.width;
    screen->height = replacement.height;
    screen->width_mm = mmWidth;
    screen->height_mm = mmHeight;
    screen->fb.pixelStride = stride / 4;
    screen->fb.byteStride = stride;
    screen->fb.frameBuffer = pixels;
    screen->memory_base = pixels;
    screen->memory_size = size;
    screen->off_screen_base = size;

    pScreen->width = replacement.width;
    pScreen->height = replacement.height;
    pScreen->mmWidth = mmWidth;
    pScreen->mmHeight = mmHeight;
    /* The memory screen always uses miModifyPixmapHeader with a valid pixmap. */
    (void)(*pScreen->ModifyPixmapHeader)(
        pixmap, replacement.width, replacement.height,
        TINYX_MEMORY_DISPLAY_DEPTH, TINYX_MEMORY_DISPLAY_BITS_PER_PIXEL,
        stride, pixels);

    priv->pixels = pixels;
    priv->size = size;
    priv->ownsPixels = ownsPixels;
    memoryConfig = replacement;

    KdComputeMouseMatrix(&matrix, RR_Rotate_0,
                         replacement.width, replacement.height);
    KdSetMouseMatrix(&matrix);
    if (wasEnabled)
        (void)KdEnableScreen(pScreen);

    RRScreenSizeNotify(pScreen);
    box.x1 = 0;
    box.y1 = 0;
    box.x2 = replacement.width;
    box.y2 = replacement.height;
    REGION_INIT(&region, &box, 1);
    DamageDamageRegion(&pixmap->drawable, &region);
    REGION_UNINIT(&region);

    if (oldOwnsPixels && oldPixels != pixels)
        free(oldPixels);
    return TINYX_MEMORY_DISPLAY_RESIZE_OK;
}

TinyXMemoryDisplayResizeResult
TinyXMemoryDisplayResize(const TinyXMemoryDisplayConfig *config)
{
    CARD32 mmWidth, mmHeight;

    if (!activeMemory || !activeMemory->screen)
        return TINYX_MEMORY_DISPLAY_RESIZE_INVALID;
    mmWidth = config && config->widthMM ? config->widthMM :
        TinyXMemoryScaleMillimeters(activeMemory->screen->width,
                                    activeMemory->screen->mmWidth,
                                    config ? config->width : 0);
    mmHeight = config && config->heightMM ? config->heightMM :
        TinyXMemoryScaleMillimeters(activeMemory->screen->height,
                                    activeMemory->screen->mmHeight,
                                    config ? config->height : 0);
    return TinyXMemoryResize(config, mmWidth, mmHeight);
}

static Bool
TinyXMemoryRandRSetSize(ScreenPtr pScreen, CARD16 width, CARD16 height,
                        CARD32 mmWidth, CARD32 mmHeight)
{
    TinyXMemoryDisplayConfig config;

    (void)pScreen;
    memset(&config, 0, sizeof(config));
    config.width = width;
    config.height = height;
    return TinyXMemoryResize(&config, mmWidth, mmHeight) ==
        TINYX_MEMORY_DISPLAY_RESIZE_OK;
}

static Bool
TinyXMemoryFinishInitScreen(ScreenPtr pScreen)
{
    rrScrPrivPtr pScrPriv;

    if (!RRScreenInit(pScreen))
        return FALSE;
    pScrPriv = rrGetScrPriv(pScreen);
    pScrPriv->rrGetInfo = TinyXMemoryRandRGetInfo;
#if RANDR_12_INTERFACE
    pScrPriv->rrScreenSetSize = TinyXMemoryRandRSetSize;
    RRScreenSetSizeRange(pScreen, 1, 1, 32767, 32767);
#endif
    return TRUE;
}

static void
TinyXMemoryDamageDestroyed(DamagePtr damage, void *closure)
{
    TinyXMemoryPriv *priv = closure;

    (void)damage;
    priv->damage = NULL;
    priv->screenPixmap = NULL;
    priv->screen = NULL;
    priv->screenInfo = NULL;
}

static Bool
TinyXMemoryCreateResources(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    TinyXMemoryPriv *priv = pScreenPriv->card->driver;

    if (!DamageSetup(pScreen))
        return FALSE;

    priv->screen = pScreen;
    priv->screenInfo = pScreenPriv->screen;
    priv->screenPixmap = fbGetScreenPixmap(pScreen);
    priv->damage = DamageCreate(NULL, TinyXMemoryDamageDestroyed,
                                DamageReportNone, TRUE, pScreen, priv);
    if (!priv->damage)
        return FALSE;
    DamageRegister(&priv->screenPixmap->drawable, priv->damage);
    return TRUE;
}

static void
TinyXMemoryCardFini(KdCardInfo *card)
{
    TinyXMemoryPriv *priv = card->driver;

    if (!priv)
        return;
    if (activeMemory == priv)
        activeMemory = NULL;
    if (priv->ownsPixels)
        free(priv->pixels);
    free(priv->formats);
    free(priv);
    card->driver = NULL;
}

const KdCardFuncs TinyXMemoryCardFuncs = {
    TinyXMemoryCardInit,
    TinyXMemoryScreenInit,
    TinyXMemoryInitScreen,
    TinyXMemoryFinishInitScreen,
    TinyXMemoryCreateResources,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    TinyXMemoryCardFini,

    NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL
};
