#ifdef HAVE_CONFIG_H
#include <kdrive-config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "tinyx-display.h"

static int
MemoryOsInit(void)
{
    return 1;
}

static void
MemoryOsNoop(void)
{
}

static Bool
MemorySpecialKey(KeySym key)
{
    (void)key;
    return FALSE;
}

/* Native KDrive hosts use this hook for VT setup. The memory host has none. */
void
OsVendorInit(void)
{
}

static const KdOsFuncs memoryOsFuncs = {
    MemoryOsInit,
    MemoryOsNoop,
    MemorySpecialKey,
    MemoryOsNoop,
    MemoryOsNoop,
    MemoryOsNoop
};

static Bool
MemoryMouseInit(void)
{
    return TRUE;
}

static void
MemoryMouseFini(void)
{
}

static const KdMouseFuncs memoryMouseFuncs = {
    MemoryMouseInit,
    MemoryMouseFini
};

static void
MemoryKeyboardLoad(void)
{
}

static int
MemoryKeyboardInit(void)
{
    return Success;
}

static void
MemoryKeyboardLeds(int leds)
{
    (void)leds;
}

static void
MemoryKeyboardBell(int volume, int frequency, int duration)
{
    (void)volume;
    (void)frequency;
    (void)duration;
}

static void
MemoryKeyboardFini(void)
{
}

static const KdKeyboardFuncs memoryKeyboardFuncs = {
    MemoryKeyboardLoad,
    MemoryKeyboardInit,
    MemoryKeyboardLeds,
    MemoryKeyboardBell,
    MemoryKeyboardFini,
    0
};

void
InitCard(char *name)
{
    (void)name;
    KdCardInfoAdd(&TinyXMemoryCardFuncs, NULL);
}

void
InitOutput(ScreenInfo *pScreenInfo, int argc, char **argv)
{
    if (!TinyXMemoryDisplayIsConfigured()) {
        TinyXMemoryDisplayConfig config;

        memset(&config, 0, sizeof(config));
        config.width = 1024;
        config.height = 768;
        if (!TinyXMemoryDisplayConfigure(&config))
            FatalError("could not configure default memory display\n");
    }

    KdOsInit(&memoryOsFuncs);
    KdInitOutput(pScreenInfo, argc, argv);
}

void
InitInput(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    KdInitInput(&memoryMouseFuncs, &memoryKeyboardFuncs);
}

void
ddxUseMsg(void)
{
    KdUseMsg();
    ErrorF("\nXmemory uses a depth-24, 32-bpp memory framebuffer.\n\n");
}

int
ddxProcessArgument(int argc, char **argv, int i)
{
    if (!strcmp(argv[i], "-version")) {
        kdVersion("Xmemory");
        exit(0);
    }
    return KdProcessArgument(argc, argv, i);
}
