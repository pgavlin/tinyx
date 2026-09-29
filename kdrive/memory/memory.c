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

typedef struct {
    CARD8 *pixels;
    size_t size;
    Bool ownsPixels;
    Bool screenInitialized;
    DamagePtr damage;
    PixmapPtr screenPixmap;
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
    if (config->width > INT_MAX || config->height > INT_MAX ||
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

static Bool
TinyXMemoryScreenInit(KdScreenInfo *screen)
{
    TinyXMemoryPriv *priv = screen->card->driver;

    if (priv->screenInitialized) {
        ErrorF("memory display supports exactly one screen\n");
        return FALSE;
    }
    priv->screenInitialized = TRUE;

    screen->width = memoryConfig.width;
    screen->height = memoryConfig.height;
    screen->fb.depth = TINYX_MEMORY_DISPLAY_DEPTH;
    screen->fb.bitsPerPixel = TINYX_MEMORY_DISPLAY_BITS_PER_PIXEL;
    screen->fb.pixelStride = memoryConfig.strideBytes / 4;
    screen->fb.byteStride = memoryConfig.strideBytes;
    screen->fb.frameBuffer = priv->pixels;
    screen->fb.visuals = 1 << TrueColor;
    screen->fb.redMask = TINYX_MEMORY_DISPLAY_RED_MASK;
    screen->fb.greenMask = TINYX_MEMORY_DISPLAY_GREEN_MASK;
    screen->fb.blueMask = TINYX_MEMORY_DISPLAY_BLUE_MASK;
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
TinyXMemoryFinishInitScreen(ScreenPtr pScreen)
{
    return RRScreenInit(pScreen);
}

static void
TinyXMemoryDamageDestroyed(DamagePtr damage, void *closure)
{
    TinyXMemoryPriv *priv = closure;

    (void)damage;
    priv->damage = NULL;
    priv->screenPixmap = NULL;
}

static Bool
TinyXMemoryCreateResources(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    TinyXMemoryPriv *priv = pScreenPriv->card->driver;

    if (!DamageSetup(pScreen))
        return FALSE;

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
