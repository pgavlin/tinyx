/*
 * Provisional memory-display interface.
 *
 * This interface exposes host-owned presentation data without exposing
 * KDrive or DIX structures.  It remains singleton, non-thread-safe, and may
 * change when the public embedding facade is defined.
 */
#ifndef TINYX_DISPLAY_H
#define TINYX_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The initial memory display is depth-24 in 32 bits per pixel.  Pixel words
 * use these native-endian X11 masks; on little-endian hosts bytes are B,G,R,X.
 */
#define TINYX_MEMORY_DISPLAY_DEPTH 24
#define TINYX_MEMORY_DISPLAY_BITS_PER_PIXEL 32
#define TINYX_MEMORY_DISPLAY_RED_MASK   UINT32_C(0x00ff0000)
#define TINYX_MEMORY_DISPLAY_GREEN_MASK UINT32_C(0x0000ff00)
#define TINYX_MEMORY_DISPLAY_BLUE_MASK  UINT32_C(0x000000ff)

typedef struct {
    uint32_t visualClass;
    uint32_t bitsPerRGB;
    uint32_t colormapEntries;
    uint32_t redMask;
    uint32_t greenMask;
    uint32_t blueMask;
} TinyXMemoryVisualConfig;

typedef struct {
    uint32_t depth;
    uint32_t bitsPerPixel;
    const TinyXMemoryVisualConfig *visuals;
    size_t visualCount;
} TinyXMemoryDepthConfig;

typedef struct {
    uint32_t width;
    uint32_t height;
    /* Zero selects 75 DPI initially and preserves DPI during resize. */
    uint32_t widthMM;
    uint32_t heightMM;
    /* Zero selects width * 4. Must otherwise be at least width * 4. */
    size_t strideBytes;
    /*
     * Optional borrowed buffer. When NULL, the display allocates and owns
     * strideBytes * height bytes for the active server generation.
     */
    void *pixels;
    /* Size of pixels when non-NULL; ignored for an allocated buffer. */
    size_t pixelsSize;
    /* Optional immutable depth/visual topology for generation startup. */
    const TinyXMemoryDepthConfig *depths;
    size_t depthCount;
    size_t rootDepthIndex;
    size_t rootVisualIndex;
} TinyXMemoryDisplayConfig;

typedef struct {
    void *pixels;
    uint32_t width;
    uint32_t height;
    size_t strideBytes;
    uint32_t depth;
    uint32_t bitsPerPixel;
    uint32_t redMask;
    uint32_t greenMask;
    uint32_t blueMask;
} TinyXMemoryFramebufferInfo;

typedef struct {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} TinyXDamageRect;

typedef enum {
    TINYX_MEMORY_DISPLAY_RESIZE_OK = 0,
    TINYX_MEMORY_DISPLAY_RESIZE_INVALID,
    TINYX_MEMORY_DISPLAY_RESIZE_NO_MEMORY,
    TINYX_MEMORY_DISPLAY_RESIZE_FAILED
} TinyXMemoryDisplayResizeResult;

/* Configure the singleton before generation initialization. */
int TinyXMemoryDisplayConfigure(const TinyXMemoryDisplayConfig *config);

/* Atomically replace the active framebuffer and notify the X11 screen. */
TinyXMemoryDisplayResizeResult
TinyXMemoryDisplayResize(const TinyXMemoryDisplayConfig *config);

/* Returns nonzero while a memory framebuffer is active. */
int TinyXMemoryDisplayGetFramebuffer(TinyXMemoryFramebufferInfo *info);

/*
 * Consume accumulated framebuffer damage. With capacity zero, returns the
 * number of currently accumulated rectangles without consuming them. If the
 * region has more rectangles than capacity, one bounding rectangle is
 * returned and all current damage is consumed.
 */
size_t TinyXMemoryDisplayTakeDamage(TinyXDamageRect *rects, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* TINYX_DISPLAY_H */
